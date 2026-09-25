// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "slf/spec.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "slf/builder.hpp"
#include "slf/checked.hpp"

namespace slf {
namespace {

struct Token {
  std::string key;
  std::string value;
  bool quoted{false};
};

[[nodiscard]] bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r'; }

[[nodiscard]] std::string quote_value(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default: out.push_back(c); break;
    }
  }
  out.push_back('"');
  return out;
}

[[nodiscard]] Status tokenize_line(std::string_view line, std::vector<Token>& out) {
  std::size_t i = 0;
  const std::size_t n = line.size();
  const auto skip_spaces = [&]() {
    while (i < n && is_space(line[i])) {
      ++i;
    }
  };
  skip_spaces();
  if (i >= n) {
    return Status::success();
  }
  const std::size_t directive_begin = i;
  while (i < n && !is_space(line[i])) {
    ++i;
  }
  Token directive;
  directive.key = std::string(line.substr(directive_begin, i - directive_begin));
  out.push_back(std::move(directive));
  while (true) {
    skip_spaces();
    if (i >= n) {
      break;
    }
    const std::size_t key_begin = i;
    while (i < n && line[i] != '=' && !is_space(line[i])) {
      ++i;
    }
    if (i >= n || line[i] != '=') {
      return Status(StatusCode::Invalid,
                    "expected key=value near \"" + std::string(line.substr(key_begin)) + "\"");
    }
    Token token;
    token.key = std::string(line.substr(key_begin, i - key_begin));
    if (token.key.empty()) {
      return Status(StatusCode::Invalid, "empty key in a key=value pair");
    }
    ++i;
    if (i < n && line[i] == '"') {
      ++i;
      token.quoted = true;
      std::string value;
      bool closed = false;
      while (i < n) {
        const char c = line[i];
        if (c == '\\') {
          if (i + 1 >= n) {
            return Status(StatusCode::Invalid, "trailing backslash in a quoted value");
          }
          switch (line[i + 1]) {
            case 'n': value.push_back('\n'); break;
            case 't': value.push_back('\t'); break;
            case 'r': value.push_back('\r'); break;
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            default: return Status(StatusCode::Invalid, "unsupported escape sequence in a quoted value");
          }
          i += 2;
          continue;
        }
        if (c == '"') {
          closed = true;
          ++i;
          break;
        }
        value.push_back(c);
        ++i;
      }
      if (!closed) {
        return Status(StatusCode::Invalid, "unterminated quoted value");
      }
      token.value = std::move(value);
    } else {
      const std::size_t value_begin = i;
      while (i < n && !is_space(line[i])) {
        ++i;
      }
      token.value = std::string(line.substr(value_begin, i - value_begin));
      if (token.value.empty()) {
        return Status(StatusCode::Invalid, "empty unquoted value");
      }
    }
    out.push_back(std::move(token));
  }
  return Status::success();
}

/// Strict key=value accessor. Duplicate keys and unknown keys are refused.
class Args {
 public:
  Args(const std::vector<Token>& tokens, std::string where) : where_(std::move(where)) {
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      if (values_.count(tokens[i].key) != 0U) {
        duplicate_ = tokens[i].key;
      }
      values_[tokens[i].key] = tokens[i].value;
    }
  }

  [[nodiscard]] const std::string& where() const noexcept { return where_; }

  [[nodiscard]] Status check(bool required_key = true) const {
    (void)required_key;
    if (!duplicate_.empty()) {
      return Status(StatusCode::Invalid, where_ + ": duplicate key \"" + duplicate_ + "\"");
    }
    return Status::success();
  }

  [[nodiscard]] Outcome<std::string> str(std::string_view key, bool required, std::uint32_t max_bytes,
                                        std::string fallback = std::string()) const {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) {
      if (required) {
        return Status(StatusCode::Invalid, where_ + ": missing required key \"" + std::string(key) + "\"");
      }
      return fallback;
    }
    if (it->second.empty()) {
      return Status(StatusCode::Invalid, where_ + ": key \"" + std::string(key) + "\" is empty");
    }
    if (it->second.size() > max_bytes) {
      return Status(StatusCode::Exhausted, where_ + ": key \"" + std::string(key) +
                                               "\" exceeds the configured length bound");
    }
    if (!is_valid_utf8(it->second)) {
      return Status(StatusCode::Invalid, where_ + ": key \"" + std::string(key) + "\" is not valid UTF-8");
    }
    return it->second;
  }

  [[nodiscard]] Outcome<std::uint64_t> u64(std::string_view key, bool required,
                                           std::uint64_t fallback = 0) const {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) {
      if (required) {
        return Status(StatusCode::Invalid, where_ + ": missing required key \"" + std::string(key) + "\"");
      }
      return fallback;
    }
    const auto value = parse_u64(it->second);
    if (!value.has_value()) {
      return Status(StatusCode::Invalid, where_ + ": key \"" + std::string(key) +
                                             "\" is not an unsigned decimal integer");
    }
    return *value;
  }

  [[nodiscard]] Outcome<std::uint32_t> u32(std::string_view key, bool required,
                                           std::uint32_t fallback = 0) const {
    const auto value = u64(key, required, fallback);
    if (!value.ok()) {
      return value.status();
    }
    std::uint32_t narrowed = 0;
    if (!checked_narrow_u32(value.value(), narrowed)) {
      return Status(StatusCode::Overflow,
                    where_ + ": key \"" + std::string(key) + "\" does not fit in 32 bits");
    }
    return narrowed;
  }

  [[nodiscard]] Outcome<bool> boolean(std::string_view key, bool required, bool fallback) const {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) {
      if (required) {
        return Status(StatusCode::Invalid, where_ + ": missing required key \"" + std::string(key) + "\"");
      }
      return fallback;
    }
    if (it->second == "true") {
      return true;
    }
    if (it->second == "false") {
      return false;
    }
    return Status(StatusCode::Invalid, where_ + ": key \"" + std::string(key) + "\" must be true or false");
  }

  [[nodiscard]] Status reject_unknown(const std::set<std::string>& allowed) const {
    for (const auto& [key, value] : values_) {
      (void)value;
      if (allowed.count(key) == 0U) {
        return Status(StatusCode::Invalid, where_ + ": unknown key \"" + key + "\"");
      }
    }
    return Status::success();
  }

 private:
  std::map<std::string, std::string> values_;
  std::string where_;
  std::string duplicate_;
};

[[nodiscard]] Outcome<TopologyGeneration> require_generation(const Args& args, TopologyGeneration fallback) {
  const auto generation = args.u64("generation", false, fallback.value);
  if (!generation.ok()) {
    return generation.status();
  }
  return TopologyGeneration{generation.value()};
}

}  // namespace

Outcome<Fabric> parse_spec(std::string_view text, const SpecLimits& limits, ValidationReport* report_out) {
  if (text.size() > limits.max_file_bytes) {
    return Status(StatusCode::Exhausted, "specification exceeds the configured size bound");
  }
  if (text.find('\0') != std::string_view::npos) {
    return Status(StatusCode::Invalid, "specification contains a NUL byte");
  }
  if (!is_valid_utf8(text)) {
    return Status(StatusCode::Invalid, "specification is not valid UTF-8");
  }

  std::unique_ptr<FabricBuilder> builder;
  FabricOptions options;
  TopologyGeneration fabric_generation{};
  std::uint32_t line_number = 0;
  bool nodes_started = false;
  // Header fields are collected first so that option directives apply no matter
  // where they appear in the header section; the builder is created lazily on
  // the first record directive.
  bool have_fabric = false;
  FabricId fabric_id{};
  std::string fabric_name;
  std::string fabric_site;
  Epoch fabric_epoch{};
  FabricState fabric_state{FabricState::Operational};
  const auto ensure_builder = [&]() -> Status {
    if (builder != nullptr) {
      return Status::success();
    }
    if (!have_fabric) {
      return Status(StatusCode::Invalid, "the fabric directive must appear before any other directive");
    }
    builder = std::make_unique<FabricBuilder>(fabric_id, fabric_name, fabric_generation, fabric_epoch,
                                              options, limits.builder);
    if (!fabric_site.empty()) {
      const Status status = builder->set_site(fabric_site);
      if (!status.ok()) {
        return status;
      }
    }
    return builder->set_state(fabric_state);
  };
  std::size_t offset = 0;
  std::vector<Token> tokens;

  while (offset <= text.size()) {
    const std::size_t newline = text.find('\n', offset);
    const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
    std::string_view line = text.substr(offset, end - offset);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    offset = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
    ++line_number;
    if (line_number > limits.max_lines) {
      return Status(StatusCode::Exhausted, "specification exceeds the configured line bound");
    }
    if (line.size() > limits.max_line_bytes) {
      return Status(StatusCode::Exhausted, "line " + std::to_string(line_number) +
                                              " exceeds the configured line length bound");
    }
    const std::size_t comment = line.find('#');
    if (comment != std::string_view::npos) {
      line = line.substr(0, comment);
    }
    tokens.clear();
    const Status tokenized = tokenize_line(line, tokens);
    if (!tokenized.ok()) {
      return Status(tokenized.code(), "line " + std::to_string(line_number) + ": " +
                                          std::string(tokenized.detail()));
    }
    if (tokens.empty()) {
      continue;
    }
    const std::string directive = tokens.front().key;
    const std::string where = "line " + std::to_string(line_number) + " (" + directive + ")";
    Args args(tokens, where);
    const Status duplicates = args.check();
    if (!duplicates.ok()) {
      return duplicates;
    }

    if (directive == "fabric") {
      if (have_fabric) {
        return Status(StatusCode::Invalid, where + ": the fabric directive may appear only once");
      }
      const Status known = args.reject_unknown({"id", "name", "site", "generation", "epoch", "state"});
      if (!known.ok()) {
        return known;
      }
      const auto id = args.u64("id", true);
      const auto name = args.str("name", true, limits.max_name_bytes);
      const auto site = args.str("site", false, limits.max_name_bytes);
      const auto generation = args.u64("generation", false, 1);
      const auto epoch = args.u64("epoch", false, 1);
      if (!id.ok() || !name.ok() || !site.ok() || !generation.ok() || !epoch.ok()) {
        return !id.ok() ? id.status() : !name.ok() ? name.status() : !site.ok() ? site.status()
                          : !generation.ok() ? generation.status() : epoch.status();
      }
      if (id.value() == 0) {
        return Status(StatusCode::Invalid, where + ": fabric id zero is reserved");
      }
      if (generation.value() == 0) {
        return Status(StatusCode::Invalid, where + ": fabric generation must be non-zero");
      }
      fabric_generation = TopologyGeneration{generation.value()};
      fabric_id = FabricId{id.value()};
      fabric_name = name.value();
      fabric_site = site.value();
      fabric_epoch = Epoch{epoch.value()};
      have_fabric = true;
      const auto state = args.str("state", false, 64);
      if (!state.ok()) {
        return state.status();
      }
      if (!state.value().empty()) {
        const auto parsed = parse_fabric_state(state.value());
        if (!parsed.has_value()) {
          return Status(StatusCode::Invalid, where + ": unknown fabric state \"" + state.value() + "\"");
        }
        fabric_state = *parsed;
      }
      continue;
    }

    if (!have_fabric) {
      return Status(StatusCode::Invalid,
                    where + ": the fabric directive must appear before any other directive");
    }

    if (directive == "option") {
      if (nodes_started) {
        return Status(StatusCode::Invalid,
                      where + ": option directives must appear before any node, port, or link directive");
      }
      const Status known =
          args.reject_unknown({"allow_same_tier_adjacency", "require_domain_diversity",
                               "min_eligible_spines", "evidence_policy"});
      if (!known.ok()) {
        return known;
      }
      const auto peer = args.boolean("allow_same_tier_adjacency", false, options.allow_same_tier_adjacency);
      const auto diversity = args.boolean("require_domain_diversity", false, options.require_domain_diversity);
      const auto min_spines = args.u32("min_eligible_spines", false, options.min_eligible_spines);
      const auto policy = args.str("evidence_policy", false, 64);
      if (!peer.ok() || !diversity.ok() || !min_spines.ok() || !policy.ok()) {
        return !peer.ok() ? peer.status() : !diversity.ok() ? diversity.status()
                                  : !min_spines.ok() ? min_spines.status() : policy.status();
      }
      options.allow_same_tier_adjacency = peer.value();
      options.require_domain_diversity = diversity.value();
      options.min_eligible_spines = min_spines.value();
      if (!policy.value().empty()) {
        const auto parsed = parse_evidence_policy(policy.value());
        if (!parsed.has_value()) {
          return Status(StatusCode::Invalid, where + ": unknown evidence policy \"" + policy.value() + "\"");
        }
        options.evidence_policy = *parsed;
      }
      continue;
    }

    if (directive == "domain") {
      const Status ready = ensure_builder();
      if (!ready.ok()) {
        return ready;
      }
      nodes_started = true;
      const Status known = args.reject_unknown({"id", "name", "generation"});
      if (!known.ok()) {
        return known;
      }
      const auto id = args.u32("id", true);
      const auto name = args.str("name", true, limits.max_name_bytes);
      const auto generation = require_generation(args, fabric_generation);
      if (!id.ok() || !name.ok() || !generation.ok()) {
        return !id.ok() ? id.status() : !name.ok() ? name.status() : generation.status();
      }
      FailureDomainRecord record;
      record.id = FailureDomainId{id.value()};
      record.name = name.value();
      record.generation = generation.value();
      const Status added = builder->add_failure_domain(std::move(record));
      if (!added.ok()) {
        return added;
      }
      continue;
    }

    if (directive == "leaf") {
      const Status ready = ensure_builder();
      if (!ready.ok()) {
        return ready;
      }
      nodes_started = true;
      const Status known = args.reject_unknown({"id", "name", "role", "domain", "state", "access_mbps",
                                                "access_links", "role_incarnation", "generation"});
      if (!known.ok()) {
        return known;
      }
      const auto id = args.u32("id", true);
      const auto name = args.str("name", true, limits.max_name_bytes);
      const auto role_text = args.str("role", true, 32);
      const auto domain = args.u32("domain", false, 0);
      const auto state_text = args.str("state", false, 32, "active");
      const auto capacity = args.u64("access_mbps", false, 0);
      const auto access_links = args.u32("access_links", false, 0);
      const auto incarnation = args.u64("role_incarnation", false, 1);
      const auto generation = require_generation(args, fabric_generation);
      if (!id.ok() || !name.ok() || !role_text.ok() || !domain.ok() || !state_text.ok() || !capacity.ok() ||
          !access_links.ok() || !incarnation.ok() || !generation.ok()) {
        return Status(StatusCode::Invalid, where + ": a leaf directive has a malformed argument");
      }
      const auto role = parse_leaf_role(role_text.value());
      if (!role.has_value()) {
        const auto spine_role = parse_spine_role(role_text.value());
        if (spine_role.has_value()) {
          return Status(StatusCode::ContradictoryRole,
                        where + ": leaf " + std::to_string(id.value()) + " carries a spine-tier role");
        }
        return Status(StatusCode::Invalid, where + ": unknown leaf role \"" + role_text.value() + "\"");
      }
      const auto state = parse_node_state(state_text.value());
      if (!state.has_value()) {
        return Status(StatusCode::Invalid, where + ": unknown node state \"" + state_text.value() + "\"");
      }
      LeafRecord record;
      record.id = LeafId{id.value()};
      record.name = name.value();
      record.role = *role;
      record.role_incarnation = RoleIncarnation{incarnation.value()};
      record.domain = FailureDomainId{domain.value()};
      record.state = *state;
      record.access_capacity_mbps = capacity.value();
      record.access_link_count = access_links.value();
      record.generation = generation.value();
      const Status added = builder->add_leaf(std::move(record));
      if (!added.ok()) {
        return added;
      }
      continue;
    }

    if (directive == "spine") {
      const Status ready = ensure_builder();
      if (!ready.ok()) {
        return ready;
      }
      nodes_started = true;
      const Status known = args.reject_unknown({"id", "name", "role", "domain", "state", "fabric_mbps",
                                                "role_incarnation", "generation"});
      if (!known.ok()) {
        return known;
      }
      const auto id = args.u32("id", true);
      const auto name = args.str("name", true, limits.max_name_bytes);
      const auto role_text = args.str("role", true, 32);
      const auto domain = args.u32("domain", false, 0);
      const auto state_text = args.str("state", false, 32, "active");
      const auto capacity = args.u64("fabric_mbps", false, 0);
      const auto incarnation = args.u64("role_incarnation", false, 1);
      const auto generation = require_generation(args, fabric_generation);
      if (!id.ok() || !name.ok() || !role_text.ok() || !domain.ok() || !state_text.ok() || !capacity.ok() ||
          !incarnation.ok() || !generation.ok()) {
        return Status(StatusCode::Invalid, where + ": a spine directive has a malformed argument");
      }
      const auto role = parse_spine_role(role_text.value());
      if (!role.has_value()) {
        const auto leaf_role = parse_leaf_role(role_text.value());
        if (leaf_role.has_value()) {
          return Status(StatusCode::ContradictoryRole,
                        where + ": spine " + std::to_string(id.value()) + " carries a leaf-tier role");
        }
        return Status(StatusCode::Invalid, where + ": unknown spine role \"" + role_text.value() + "\"");
      }
      const auto state = parse_node_state(state_text.value());
      if (!state.has_value()) {
        return Status(StatusCode::Invalid, where + ": unknown node state \"" + state_text.value() + "\"");
      }
      SpineRecord record;
      record.id = SpineId{id.value()};
      record.name = name.value();
      record.role = *role;
      record.role_incarnation = RoleIncarnation{incarnation.value()};
      record.domain = FailureDomainId{domain.value()};
      record.state = *state;
      record.fabric_capacity_mbps = capacity.value();
      record.generation = generation.value();
      const Status added = builder->add_spine(std::move(record));
      if (!added.ok()) {
        return added;
      }
      continue;
    }

    if (directive == "port") {
      const Status ready = ensure_builder();
      if (!ready.ok()) {
        return ready;
      }
      nodes_started = true;
      const Status known = args.reject_unknown({"id", "owner", "speed_mbps", "admin", "generation"});
      if (!known.ok()) {
        return known;
      }
      const auto id = args.u32("id", true);
      const auto owner_text = args.str("owner", true, 64);
      const auto speed = args.u64("speed_mbps", true);
      const auto admin_text = args.str("admin", false, 32, "enabled");
      const auto generation = require_generation(args, fabric_generation);
      if (!id.ok() || !owner_text.ok() || !speed.ok() || !admin_text.ok() || !generation.ok()) {
        return Status(StatusCode::Invalid, where + ": a port directive has a malformed argument");
      }
      const auto owner = parse_node_key(owner_text.value());
      if (!owner.has_value()) {
        return Status(StatusCode::Invalid, where + ": unknown port owner \"" + owner_text.value() + "\"");
      }
      const auto admin = parse_port_admin(admin_text.value());
      if (!admin.has_value()) {
        return Status(StatusCode::Invalid, where + ": unknown port admin state \"" + admin_text.value() + "\"");
      }
      PortRecord record;
      record.id = PortId{id.value()};
      record.owner = *owner;
      record.speed_mbps = speed.value();
      record.admin = *admin;
      record.generation = generation.value();
      const Status added = builder->add_port(std::move(record));
      if (!added.ok()) {
        return added;
      }
      continue;
    }

    if (directive == "link" || directive == "history-link") {
      const Status ready = ensure_builder();
      if (!ready.ok()) {
        return ready;
      }
      nodes_started = true;
      const Status known = args.reject_unknown({"id", "a", "pa", "b", "pb", "class", "admin", "speed_mbps",
                                                "generation"});
      if (!known.ok()) {
        return known;
      }
      const auto id = args.u32("id", true);
      const auto a_text = args.str("a", true, 64);
      const auto b_text = args.str("b", true, 64);
      const auto pa = args.u32("pa", true);
      const auto pb = args.u32("pb", true);
      const auto class_text = args.str("class", false, 32);
      const auto admin_text = args.str("admin", false, 32, "enabled");
      const auto speed = args.u64("speed_mbps", false, 0);
      const auto generation = require_generation(args, fabric_generation);
      if (!id.ok() || !a_text.ok() || !b_text.ok() || !pa.ok() || !pb.ok() || !class_text.ok() ||
          !admin_text.ok() || !speed.ok() || !generation.ok()) {
        return Status(StatusCode::Invalid, where + ": a link directive has a malformed argument");
      }
      const auto a = parse_node_key(a_text.value());
      const auto b = parse_node_key(b_text.value());
      if (!a.has_value()) {
        return Status(StatusCode::Invalid, where + ": unknown link endpoint \"" + a_text.value() + "\"");
      }
      if (!b.has_value()) {
        return Status(StatusCode::Invalid, where + ": unknown link endpoint \"" + b_text.value() + "\"");
      }
      LinkRecord record;
      record.id = LinkId{id.value()};
      record.a = *a;
      record.port_a = PortId{pa.value()};
      record.b = *b;
      record.port_b = PortId{pb.value()};
      record.generation = generation.value();
      record.declared_speed_mbps = speed.value();
      if (class_text.value().empty()) {
        record.cls = a->tier == b->tier ? LinkClass::SameTierPeer : LinkClass::LeafSpine;
      } else if (class_text.value() == "leaf_spine") {
        record.cls = LinkClass::LeafSpine;
      } else if (class_text.value() == "same_tier_peer") {
        record.cls = LinkClass::SameTierPeer;
      } else {
        return Status(StatusCode::Invalid, where + ": unknown link class \"" + class_text.value() + "\"");
      }
      const auto admin = parse_link_admin(admin_text.value());
      if (!admin.has_value()) {
        return Status(StatusCode::Invalid, where + ": unknown link admin state \"" + admin_text.value() + "\"");
      }
      record.admin = *admin;
      const Status added = directive == "link" ? builder->add_link(std::move(record))
                                               : builder->add_history_link(std::move(record));
      if (!added.ok()) {
        return added;
      }
      continue;
    }

    if (directive == "evidence") {
      const Status ready = ensure_builder();
      if (!ready.ok()) {
        return ready;
      }
      const Status known =
          args.reject_unknown({"link", "observed", "generation", "observer", "epoch", "seq", "at_ms"});
      if (!known.ok()) {
        return known;
      }
      const auto link = args.u32("link", true);
      const auto observed_text = args.str("observed", true, 32);
      const auto generation = require_generation(args, fabric_generation);
      const auto observer_text = args.str("observer", true, 40);
      const auto epoch = args.u64("epoch", false, 1);
      const auto seq = args.u64("seq", true);
      const auto at_ms = args.u64("at_ms", false, 0);
      if (!link.ok() || !observed_text.ok() || !generation.ok() || !observer_text.ok() || !epoch.ok() ||
          !seq.ok() || !at_ms.ok()) {
        return Status(StatusCode::Invalid, where + ": an evidence directive has a malformed argument");
      }
      const auto observed = parse_link_observation(observed_text.value());
      if (!observed.has_value()) {
        return Status(StatusCode::Invalid, where + ": unknown observation \"" + observed_text.value() + "\"");
      }
      const auto observer = parse_incarnation(observer_text.value());
      if (!observer.has_value()) {
        return Status(StatusCode::Invalid,
                      where + ": observer must be 32 hexadecimal digits");
      }
      LinkEvidence record;
      record.link = LinkId{link.value()};
      record.observed = *observed;
      record.generation = generation.value();
      record.observer = *observer;
      record.epoch = Epoch{epoch.value()};
      record.observed_seq = SequenceNumber{seq.value()};
      record.observed_at_unix_ms = at_ms.value();
      const Status added = builder->add_evidence(std::move(record));
      if (!added.ok()) {
        return added;
      }
      continue;
    }

    return Status(StatusCode::Invalid, where + ": unknown directive \"" + directive + "\"");
  }

  const Status ready = ensure_builder();
  if (!ready.ok()) {
    return ready;
  }
  Outcome<Fabric> fabric = builder->freeze();
  if (report_out != nullptr) {
    *report_out = builder->report();
  }
  return fabric;
}

Outcome<Fabric> load_spec_file(const std::filesystem::path& path, const SpecLimits& limits,
                               ValidationReport* report_out) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return Status(StatusCode::NotFound, "cannot stat specification file: " + path.string());
  }
  if (size > limits.max_file_bytes) {
    return Status(StatusCode::Exhausted, "specification file exceeds the configured size bound");
  }
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return Status(StatusCode::IoError, "cannot open specification file: " + path.string());
  }
  std::string text(static_cast<std::size_t>(size), '\0');
  const std::size_t read = size == 0 ? 0 : std::fread(text.data(), 1, text.size(), file);
  std::fclose(file);
  if (read != text.size()) {
    return Status(StatusCode::IoError, "short read on specification file: " + path.string());
  }
  return parse_spec(text, limits, report_out);
}

std::string format_spec(const Fabric& fabric) {
  std::ostringstream out;
  out << "fabric id=" << fabric.id.value << " name=" << quote_value(fabric.name);
  if (!fabric.site.empty()) {
    out << " site=" << quote_value(fabric.site);
  }
  out << " generation=" << fabric.generation.value << " epoch=" << fabric.epoch.value
      << " state=" << slf::to_string(fabric.state) << "\n";
  out << "option allow_same_tier_adjacency=" << (fabric.options.allow_same_tier_adjacency ? "true" : "false")
      << " require_domain_diversity=" << (fabric.options.require_domain_diversity ? "true" : "false")
      << " min_eligible_spines=" << fabric.options.min_eligible_spines
      << " evidence_policy=" << slf::to_string(fabric.options.evidence_policy) << "\n";
  for (const auto& domain : fabric.domains) {
    out << "domain id=" << domain.id.value << " name=" << quote_value(domain.name)
        << " generation=" << domain.generation.value << "\n";
  }
  for (const auto& leaf : fabric.leaves) {
    out << "leaf id=" << leaf.id.value << " name=" << quote_value(leaf.name)
        << " role=" << slf::to_string(leaf.role) << " domain=" << leaf.domain.value
        << " state=" << slf::to_string(leaf.state) << " access_mbps=" << leaf.access_capacity_mbps
        << " access_links=" << leaf.access_link_count
        << " role_incarnation=" << leaf.role_incarnation.value
        << " generation=" << leaf.generation.value << "\n";
  }
  for (const auto& spine : fabric.spines) {
    out << "spine id=" << spine.id.value << " name=" << quote_value(spine.name)
        << " role=" << slf::to_string(spine.role) << " domain=" << spine.domain.value
        << " state=" << slf::to_string(spine.state) << " fabric_mbps=" << spine.fabric_capacity_mbps
        << " role_incarnation=" << spine.role_incarnation.value
        << " generation=" << spine.generation.value << "\n";
  }
  for (const auto& port : fabric.ports) {
    out << "port id=" << port.id.value << " owner=" << to_string(port.owner)
        << " speed_mbps=" << port.speed_mbps << " admin=" << slf::to_string(port.admin)
        << " generation=" << port.generation.value << "\n";
  }
  const auto emit_link = [&out](const char* prefix, const LinkRecord& link) {
    out << prefix << " id=" << link.id.value << " a=" << to_string(link.a) << " pa=" << link.port_a.value
        << " b=" << to_string(link.b) << " pb=" << link.port_b.value
        << " class=" << slf::to_string(link.cls) << " admin=" << slf::to_string(link.admin);
    if (link.declared_speed_mbps != 0) {
      out << " speed_mbps=" << link.declared_speed_mbps;
    }
    out << " generation=" << link.generation.value << "\n";
  };
  for (const auto& link : fabric.links) {
    emit_link("link", link);
  }
  for (const auto& link : fabric.history_links) {
    emit_link("history-link", link);
  }
  for (const auto& record : fabric.evidence) {
    out << "evidence link=" << record.link.value << " observed=" << slf::to_string(record.observed)
        << " generation=" << record.generation.value << " observer=" << slf::to_string(record.observer)
        << " epoch=" << record.epoch.value << " seq=" << record.observed_seq.value
        << " at_ms=" << record.observed_at_unix_ms << "\n";
  }
  return out.str();
}

Status write_spec_file(const std::filesystem::path& path, const Fabric& fabric) {
  const std::string text = format_spec(fabric);
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    return Status(StatusCode::IoError, "cannot create specification file: " + path.string());
  }
  const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
  const int closed = std::fclose(file);
  if (written != text.size()) {
    return Status(StatusCode::IoError, "short write on specification file");
  }
  if (closed != 0) {
    return Status(StatusCode::IoError, "cannot flush specification file");
  }
  return Status::success();
}

}  // namespace slf
