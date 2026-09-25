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

// slfctl: inspection and operation tool for the Spine-Leaf Fabric runtime.
//
// Exit codes (stable, documented in README):
//   0  success
//   2  usage error
//   3  invalid input (structural, malformed, unsupported value)
//   4  refused or fenced (authority, policy, stale coordinate)
//   5  indeterminate (required evidence is missing, stale, or conflicting)
//   6  input/output or persistence failure
//   7  cancelled
//   8  integrity failure (corrupt, truncated, wrong version)
//   9  not started / not found / conflict

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "slf/eligibility.hpp"
#include "slf/platform.hpp"
#include "slf/runtime.hpp"
#include "slf/service.hpp"
#include "slf/spec.hpp"
#include "slf/store.hpp"

namespace {

using namespace slf;

int exit_code_for(StatusCode code) {
  switch (code) {
    case StatusCode::Ok: return 0;
    case StatusCode::Invalid: return 3;
    case StatusCode::Unsupported: return 3;
    case StatusCode::ContradictoryRole: return 3;
    case StatusCode::IllegalAdjacency: return 3;
    case StatusCode::Overflow: return 3;
    case StatusCode::Exhausted: return 3;
    case StatusCode::Refused: return 4;
    case StatusCode::Fenced: return 4;
    case StatusCode::Stale: return 4;
    case StatusCode::Cancelled: return 7;
    case StatusCode::Unknown:
    case StatusCode::Indeterminate:
    case StatusCode::Incomplete:
    case StatusCode::Conflicting: return 5;
    case StatusCode::IoError: return 6;
    case StatusCode::Closed: return 6;
    case StatusCode::Corrupt:
    case StatusCode::Truncated:
    case StatusCode::IntegrityError:
    case StatusCode::IncompatibleVersion: return 8;
    case StatusCode::NotFound:
    case StatusCode::NotStarted:
    case StatusCode::AlreadyExists:
    case StatusCode::DuplicateIdentity:
    case StatusCode::DanglingEdge:
    case StatusCode::CrossGeneration:
    case StatusCode::Busy: return 9;
    case StatusCode::DeadlineExpired: return 6;
    case StatusCode::Internal: return 6;
  }
  return 6;
}

void print_usage() {
  std::cout <<
      "slfctl - Spine-Leaf Fabric inspection and operation tool\n"
      "\n"
      "Usage:\n"
      "  slfctl validate   --spec FILE [--quiet]\n"
      "  slfctl inspect    --spec FILE\n"
      "  slfctl digest     --spec FILE\n"
      "  slfctl paths      --spec FILE --from NODE --to NODE [constraints]\n"
      "  slfctl spines     --spec FILE --leaf NODE [constraints]\n"
      "  slfctl health     --spec FILE [constraints]\n"
      "  slfctl authorize  --spec FILE --from NODE --to NODE [--ttl MS]\n"
      "  slfctl serve      --spec FILE --state DIR [--port N] [--bind HOST] [--fault POINT] [--workers N]\n"
      "  slfctl client     status|paths|spines|health|lease|command|evidence|compact [options]\n"
      "  slfctl version\n"
      "\n"
      "NODE is leaf:N or spine:N.\n"
      "Constraints: [--diversity] [--min-spines N] [--min-capacity MBPS] [--max-oversub NUM:DEN]\n"
      "             [--evidence] [--no-peer] [--sets N] [--explanations N]\n"
      "Fault points: none | before_commit | after_commit_before_ack | after_ack_before_snapshot\n"
      "\n"
      "Exit codes: 0 ok, 2 usage, 3 invalid, 4 refused/fenced, 5 indeterminate, 6 io,\n"
      "            7 cancelled, 8 integrity, 9 not found/conflict\n";
}

struct Options {
  std::string spec;
  std::string state;
  std::string bind_host{"127.0.0.1"};
  std::string from;
  std::string to;
  std::string leaf;
  std::string fault{"none"};
  std::string kind;
  std::string value;
  std::string token;
  std::string lease_request{"slfctl"};
  std::string observed;
  std::uint16_t port{0};
  std::uint32_t workers{2};
  std::uint64_t ttl_ms{0};
  std::uint64_t link{0};
  std::uint32_t sets{4};
  std::uint32_t explanations{8};
  std::uint64_t min_capacity{0};
  std::uint32_t min_spines{0};
  std::string max_oversub;
  bool diversity{false};
  bool evidence{false};
  bool no_peer{false};
  bool quiet{false};
  bool replace{false};
  std::vector<std::string> positional;
};

Outcome<Options> parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const auto next = [&](std::string& target) -> bool {
      if (i + 1 >= argc) {
        return false;
      }
      target = argv[++i];
      return true;
    };
    if (argument == "--spec") {
      if (!next(options.spec)) return Status(StatusCode::Invalid, "--spec requires a value");
    } else if (argument == "--state") {
      if (!next(options.state)) return Status(StatusCode::Invalid, "--state requires a value");
    } else if (argument == "--bind") {
      if (!next(options.bind_host)) return Status(StatusCode::Invalid, "--bind requires a value");
    } else if (argument == "--from") {
      if (!next(options.from)) return Status(StatusCode::Invalid, "--from requires a value");
    } else if (argument == "--to") {
      if (!next(options.to)) return Status(StatusCode::Invalid, "--to requires a value");
    } else if (argument == "--leaf") {
      if (!next(options.leaf)) return Status(StatusCode::Invalid, "--leaf requires a value");
    } else if (argument == "--fault") {
      if (!next(options.fault)) return Status(StatusCode::Invalid, "--fault requires a value");
    } else if (argument == "--kind") {
      if (!next(options.kind)) return Status(StatusCode::Invalid, "--kind requires a value");
    } else if (argument == "--value") {
      if (!next(options.value)) return Status(StatusCode::Invalid, "--value requires a value");
    } else if (argument == "--token") {
      if (!next(options.token)) return Status(StatusCode::Invalid, "--token requires a value");
    } else if (argument == "--requester") {
      if (!next(options.lease_request)) return Status(StatusCode::Invalid, "--requester requires a value");
    } else if (argument == "--observed") {
      if (!next(options.observed)) return Status(StatusCode::Invalid, "--observed requires a value");
    } else if (argument == "--max-oversub") {
      if (!next(options.max_oversub)) return Status(StatusCode::Invalid, "--max-oversub requires a value");
    } else if (argument == "--port") {
      std::string text;
      if (!next(text)) return Status(StatusCode::Invalid, "--port requires a value");
      const auto value = parse_u32(text);
      if (!value.has_value() || *value == 0 || *value > 65535U) {
        return Status(StatusCode::Invalid, "--port must be in 1..65535");
      }
      options.port = static_cast<std::uint16_t>(*value);
    } else if (argument == "--workers") {
      std::string text;
      if (!next(text)) return Status(StatusCode::Invalid, "--workers requires a value");
      const auto value = parse_u32(text);
      if (!value.has_value() || *value > 8U) {
        return Status(StatusCode::Invalid, "--workers must be in 0..8");
      }
      options.workers = *value;
    } else if (argument == "--link") {
      std::string text;
      if (!next(text)) return Status(StatusCode::Invalid, "--link requires a value");
      const auto value = parse_u32(text);
      if (!value.has_value()) return Status(StatusCode::Invalid, "--link must be an unsigned integer");
      options.link = *value;
    } else if (argument == "--ttl") {
      std::string text;
      if (!next(text)) return Status(StatusCode::Invalid, "--ttl requires a value");
      const auto value = parse_u64(text);
      if (!value.has_value()) return Status(StatusCode::Invalid, "--ttl must be an unsigned integer");
      options.ttl_ms = *value;
    } else if (argument == "--min-capacity") {
      std::string text;
      if (!next(text)) return Status(StatusCode::Invalid, "--min-capacity requires a value");
      const auto value = parse_u64(text);
      if (!value.has_value()) return Status(StatusCode::Invalid, "--min-capacity must be an unsigned integer");
      options.min_capacity = *value;
    } else if (argument == "--min-spines") {
      std::string text;
      if (!next(text)) return Status(StatusCode::Invalid, "--min-spines requires a value");
      const auto value = parse_u32(text);
      if (!value.has_value()) return Status(StatusCode::Invalid, "--min-spines must be an unsigned integer");
      options.min_spines = *value;
    } else if (argument == "--sets") {
      std::string text;
      if (!next(text)) return Status(StatusCode::Invalid, "--sets requires a value");
      const auto value = parse_u32(text);
      if (!value.has_value() || *value > 64U) return Status(StatusCode::Invalid, "--sets must be in 0..64");
      options.sets = *value;
    } else if (argument == "--explanations") {
      std::string text;
      if (!next(text)) return Status(StatusCode::Invalid, "--explanations requires a value");
      const auto value = parse_u32(text);
      if (!value.has_value() || *value > 64U) {
        return Status(StatusCode::Invalid, "--explanations must be in 0..64");
      }
      options.explanations = *value;
    } else if (argument == "--diversity") {
      options.diversity = true;
    } else if (argument == "--evidence") {
      options.evidence = true;
    } else if (argument == "--no-peer") {
      options.no_peer = true;
    } else if (argument == "--quiet") {
      options.quiet = true;
    } else if (argument == "--replace") {
      options.replace = true;
    } else if (argument == "--help" || argument == "-h") {
      print_usage();
      std::exit(0);
    } else if (!argument.empty() && argument[0] == '-') {
      return Status(StatusCode::Invalid, "unknown option " + argument);
    } else {
      options.positional.push_back(argument);
    }
  }
  return options;
}

Outcome<Fabric> load_spec(const Options& options, ValidationReport* report) {
  if (options.spec.empty()) {
    return Status(StatusCode::Invalid, "--spec FILE is required");
  }
  return load_spec_file(options.spec, SpecLimits{}, report);
}

Outcome<PathConstraints> make_constraints(const Fabric& fabric, const Options& options) {
  PathConstraints constraints = PathConstraints::defaults_for(fabric.options);
  if (options.diversity) {
    constraints.require_domain_diversity = true;
  }
  if (options.min_spines != 0) {
    constraints.min_eligible_spines = options.min_spines;
  }
  if (options.min_capacity != 0) {
    constraints.min_total_capacity_mbps = options.min_capacity;
  }
  if (options.no_peer) {
    constraints.allow_peer_paths = false;
  }
  if (options.evidence) {
    constraints.require_evidence = true;
  }
  constraints.max_sets = options.sets;
  constraints.max_explanations = options.explanations;
  if (!options.max_oversub.empty()) {
    const std::size_t colon = options.max_oversub.find(':');
    if (colon == std::string::npos) {
      return Status(StatusCode::Invalid, "--max-oversub must be NUM:DEN");
    }
    const auto num = parse_u64(options.max_oversub.substr(0, colon));
    const auto den = parse_u64(options.max_oversub.substr(colon + 1));
    if (!num.has_value() || !den.has_value()) {
      return Status(StatusCode::Invalid, "--max-oversub must be NUM:DEN with decimal integers");
    }
    const auto ratio = Rational::make(*num, *den);
    if (!ratio.ok()) {
      return ratio.status();
    }
    constraints.max_oversubscription = ratio.value();
  }
  return constraints;
}

int command_validate(const Options& options) {
  ValidationReport report;
  const auto fabric = load_spec(options, &report);
  if (!fabric.ok()) {
    std::cout << "invalid: " << fabric.status().to_string() << "\n";
    std::cout << report.to_string();
    return exit_code_for(fabric.code());
  }
  if (!options.quiet) {
    std::cout << report.to_string();
  }
  std::cout << "valid: " << report.summary() << "\n";
  std::cout << "topology_digest=" << fabric.value().topology_digest.hex() << "\n";
  if (!fabric.value().evidence.empty()) {
    std::cout << "evidence_digest=" << fabric.value().evidence_digest.hex() << "\n";
  }
  return 0;
}

int command_inspect(const Options& options) {
  const auto fabric = load_spec(options, nullptr);
  if (!fabric.ok()) {
    std::cout << "invalid: " << fabric.status().to_string() << "\n";
    return exit_code_for(fabric.code());
  }
  std::cout << describe_fabric(fabric.value());
  const auto index = TopologyIndex::build(fabric.value());
  if (!index.ok()) {
    std::cout << "index failed: " << index.status().to_string() << "\n";
    return exit_code_for(index.code());
  }
  std::cout << "  adjacency: " << index.value().node_count() << " nodes, " << index.value().edge_count()
            << " directed arcs\n";
  for (const auto key : index.value().nodes()) {
    std::cout << "    " << to_string(key) << " degree=" << index.value().degree(key) << " ->";
    for (const auto& adjacency : index.value().adjacencies(key)) {
      std::cout << " " << to_string(adjacency.peer) << "(" << to_string(adjacency.link) << ","
                << adjacency.speed_mbps << ")";
    }
    std::cout << "\n";
  }
  return 0;
}

int command_digest(const Options& options) {
  const auto fabric = load_spec(options, nullptr);
  if (!fabric.ok()) {
    std::cout << "invalid: " << fabric.status().to_string() << "\n";
    return exit_code_for(fabric.code());
  }
  std::cout << "fabric=" << to_string(fabric.value().id) << "\n";
  std::cout << "generation=" << fabric.value().generation.value << "\n";
  std::cout << "epoch=" << fabric.value().epoch.value << "\n";
  std::cout << "topology_digest=" << fabric.value().topology_digest.hex() << "\n";
  std::cout << "evidence_digest="
            << (fabric.value().evidence.empty() ? std::string("none")
                                                : fabric.value().evidence_digest.hex())
            << "\n";
  std::cout << "canonical_bytes=" << canonical_topology_bytes(fabric.value()).size() << "\n";
  return 0;
}

int command_paths(const Options& options) {
  const auto fabric = load_spec(options, nullptr);
  if (!fabric.ok()) {
    std::cout << "invalid: " << fabric.status().to_string() << "\n";
    return exit_code_for(fabric.code());
  }
  const auto from = parse_node_key(options.from);
  const auto to = parse_node_key(options.to);
  if (!from.has_value() || !to.has_value()) {
    std::cout << "usage: --from NODE --to NODE with NODE = leaf:N or spine:N\n";
    return 2;
  }
  const auto constraints = make_constraints(fabric.value(), options);
  if (!constraints.ok()) {
    std::cout << constraints.status().to_string() << "\n";
    return 2;
  }
  const auto index = TopologyIndex::build(fabric.value());
  if (!index.ok()) {
    std::cout << index.status().to_string() << "\n";
    return exit_code_for(index.code());
  }
  EligibilityEngine engine(index.value());
  Outcome<PathAssessment> assessment =
      from->tier == Tier::Leaf && to->tier == Tier::Leaf
          ? engine.assess_leaf_pair(LeafId{from->index}, LeafId{to->index}, constraints.value())
          : (from->tier == Tier::Leaf && to->tier == Tier::Spine
                 ? engine.assess_leaf_to_spine(LeafId{from->index}, SpineId{to->index}, constraints.value())
                 : Outcome<PathAssessment>(Status(StatusCode::Unsupported,
                                                  "only leaf-to-leaf and leaf-to-spine queries are supported")));
  if (!assessment.ok()) {
    std::cout << assessment.status().to_string() << "\n";
    return exit_code_for(assessment.code());
  }
  std::cout << assessment.value().to_string();
  return exit_code_for(assessment.value().verdict == EligibilityVerdict::Eligible
                           ? StatusCode::Ok
                           : assessment.value().code);
}

int command_spines(const Options& options) {
  const auto fabric = load_spec(options, nullptr);
  if (!fabric.ok()) {
    std::cout << "invalid: " << fabric.status().to_string() << "\n";
    return exit_code_for(fabric.code());
  }
  const auto leaf = parse_leaf_id(options.leaf.empty() ? options.from : options.leaf);
  if (!leaf.has_value()) {
    std::cout << "usage: --leaf leaf:N\n";
    return 2;
  }
  const auto constraints = make_constraints(fabric.value(), options);
  if (!constraints.ok()) {
    std::cout << constraints.status().to_string() << "\n";
    return 2;
  }
  const auto index = TopologyIndex::build(fabric.value());
  if (!index.ok()) {
    std::cout << index.status().to_string() << "\n";
    return exit_code_for(index.code());
  }
  EligibilityEngine engine(index.value());
  const auto spines = engine.eligible_spines_for_leaf(leaf.value(), constraints.value());
  if (!spines.ok()) {
    std::cout << spines.status().to_string() << "\n";
    return exit_code_for(spines.code());
  }
  std::uint32_t eligible = 0;
  for (const auto& entry : spines.value()) {
    std::cout << to_string(leaf.value()) << " -> " << to_string(entry.spine) << " domain=" << entry.domain.value
              << " capacity_mbps=" << entry.capacity_mbps << " verdict=" << to_string(entry.verdict)
              << " code=" << to_string(entry.code) << " evidence=" << to_string(entry.freshness);
    if (!entry.detail.empty()) {
      std::cout << " detail=\"" << entry.detail << "\"";
    }
    std::cout << "\n";
    if (entry.verdict == EligibilityVerdict::Eligible) {
      ++eligible;
    }
  }
  std::cout << "eligible=" << eligible << " considered=" << spines.value().size() << "\n";
  return eligible == 0 ? 5 : 0;
}

int command_health(const Options& options) {
  const auto fabric = load_spec(options, nullptr);
  if (!fabric.ok()) {
    std::cout << "invalid: " << fabric.status().to_string() << "\n";
    return exit_code_for(fabric.code());
  }
  const auto constraints = make_constraints(fabric.value(), options);
  if (!constraints.ok()) {
    std::cout << constraints.status().to_string() << "\n";
    return 2;
  }
  const auto index = TopologyIndex::build(fabric.value());
  if (!index.ok()) {
    std::cout << index.status().to_string() << "\n";
    return exit_code_for(index.code());
  }
  EligibilityEngine engine(index.value());
  const auto report = engine.health(constraints.value(), ControllerIncarnation{}, fabric.value().epoch);
  if (!report.ok()) {
    std::cout << report.status().to_string() << "\n";
    return exit_code_for(report.code());
  }
  std::cout << report.value().to_string();
  return report.value().health == FabricHealth::Operational ? 0
         : report.value().health == FabricHealth::Degraded ? 4
                                                           : 5;
}

int command_authorize(const Options& options) {
  RuntimeConfig config;
  config.enable_persistence = false;
  config.worker_threads = 0;
  ControllerRuntime runtime(config);
  const Status started = runtime.start();
  if (!started.ok()) {
    std::cout << started.to_string() << "\n";
    return exit_code_for(started.code());
  }
  ValidationReport report;
  auto fabric = load_spec(options, &report);
  if (!fabric.ok()) {
    std::cout << "invalid: " << fabric.status().to_string() << "\n";
    return exit_code_for(fabric.code());
  }
  const auto constraints = make_constraints(fabric.value(), options);
  if (!constraints.ok()) {
    std::cout << constraints.status().to_string() << "\n";
    return 2;
  }
  const Status installed = runtime.install_fabric(std::move(fabric.value()), true);
  if (!installed.ok()) {
    std::cout << installed.to_string() << "\n";
    return exit_code_for(installed.code());
  }
  const auto lease = runtime.acquire_authority("slfctl");
  if (!lease.ok()) {
    std::cout << lease.status().to_string() << "\n";
    return exit_code_for(lease.code());
  }
  const auto from = parse_node_key(options.from);
  const auto to = parse_node_key(options.to);
  if (!from.has_value() || !to.has_value()) {
    std::cout << "usage: --from NODE --to NODE\n";
    return 2;
  }
  const auto authority = runtime.authorize_path(*from, *to, constraints.value(), options.ttl_ms);
  if (!authority.ok()) {
    std::cout << authority.status().to_string() << "\n";
    return exit_code_for(authority.code());
  }
  std::cout << "controller=" << to_string(authority.value().controller) << "\n";
  std::cout << "epoch=" << authority.value().epoch.value << "\n";
  std::cout << "generation=" << authority.value().generation.value << "\n";
  std::cout << "digest=" << authority.value().topology_digest.hex() << "\n";
  std::cout << "lease=" << authority.value().lease.value << "\n";
  std::cout << "expires_at_ms=" << authority.value().expires_at_unix_ms << "\n";
  std::cout << "spines=";
  for (const auto spine : authority.value().spines) {
    std::cout << " " << to_string(spine);
  }
  std::cout << "\n";
  const Status stopped = runtime.stop();
  return stopped.ok() ? 0 : exit_code_for(stopped.code());
}

int command_serve(const Options& options) {
  ValidationReport report;
  auto fabric = load_spec(options, &report);
  if (!fabric.ok()) {
    std::cout << "invalid: " << fabric.status().to_string() << "\n";
    return exit_code_for(fabric.code());
  }
  const auto fault = parse_fault_point(options.fault);
  if (!fault.has_value()) {
    std::cout << "unknown fault point: " << options.fault << "\n";
    return 2;
  }
  RuntimeConfig config;
  config.fabric_id = fabric.value().id;
  config.name = fabric.value().name;
  config.site = fabric.value().site;
  config.options = fabric.value().options;
  config.state_dir = options.state;
  config.enable_persistence = !options.state.empty();
  config.worker_threads = options.workers;
  ControllerRuntime runtime(config);
  const Status started = runtime.start();
  if (!started.ok()) {
    std::cout << "start failed: " << started.to_string() << "\n";
    return exit_code_for(started.code());
  }
  const Status installed = runtime.install_fabric(std::move(fabric.value()), !options.replace ? false : true);
  if (!installed.ok() && installed.code() != StatusCode::AlreadyExists) {
    std::cout << "install failed: " << installed.to_string() << "\n";
    return exit_code_for(installed.code());
  }
  runtime.set_fault_point(*fault);

  net::RuntimeService service(runtime);
  net::ServerConfig server_config;
  server_config.endpoint.host = options.bind_host;
  server_config.endpoint.port = options.port;
  server_config.trace = false;
  auto server = net::FabricServer::create(server_config, &service);
  if (!server.ok()) {
    std::cout << "listen failed: " << server.status().to_string() << "\n";
    return exit_code_for(server.code());
  }
  const Status listening = server.value()->start();
  if (!listening.ok()) {
    std::cout << "listen failed: " << listening.to_string() << "\n";
    return exit_code_for(listening.code());
  }
  const auto status = runtime.status();
  std::cout << "slfctl serving on " << options.bind_host << ":" << server.value()->port() << "\n";
  std::cout << "controller=" << to_string(status.controller) << " epoch=" << status.epoch.value
            << " generation=" << status.generation.value << "\n";
  std::cout << "recovery=" << to_string(status.recovery.status) << " detail=\"" << status.recovery.detail
            << "\"\n";
  std::cout.flush();

  // The accept loop runs on its own thread; the main thread waits for either a
  // stop file or a process-level signal. Ctrl+C terminates the process, and the
  // crash-recovery tests kill the process outright, so no signal handling is
  // required for correctness.
  while (server.value()->running()) {
    platform::sleep_ms(20);
  }
  const Status stopped_server = server.value()->stop();
  const Status stopped_runtime = runtime.stop();
  return stopped_server.ok() && stopped_runtime.ok() ? 0 : exit_code_for(StatusCode::IoError);
}

int command_client(const Options& options) {
  if (options.positional.size() < 2) {
    std::cout << "usage: slfctl client <status|paths|spines|health|lease|command|evidence|compact> "
                 "[--port N]\n";
    return 2;
  }
  const std::string subcommand = options.positional[1];
  net::ClientConfig config;
  config.endpoint.host = options.bind_host;
  config.endpoint.port = options.port;
  auto client = net::FabricClient::connect(config);
  if (!client.ok()) {
    std::cout << "connect failed: " << client.status().to_string() << "\n";
    return exit_code_for(client.code());
  }
  const auto hello = net::handshake(*client.value(), "slfctl");
  if (!hello.ok()) {
    std::cout << "handshake failed: " << hello.status().to_string() << "\n";
    return exit_code_for(hello.code());
  }
  RequestId request_id{1};
  net::Request request;
  request.request_id = request_id;
  if (subcommand == "status") {
    request.kind = net::RequestKind::Status;
  } else if (subcommand == "paths") {
    const auto from = parse_node_key(options.from);
    const auto to = parse_node_key(options.to);
    if (!from.has_value() || !to.has_value()) {
      std::cout << "--from and --to are required\n";
      return 2;
    }
    request.kind = net::RequestKind::Paths;
    request.from = *from;
    request.to = *to;
    request.constraints = PathConstraints::defaults_for(FabricOptions{});
    request.constraints.require_domain_diversity = options.diversity;
    if (options.min_spines != 0) {
      request.constraints.min_eligible_spines = options.min_spines;
    }
    if (options.evidence) {
      request.constraints.require_evidence = true;
    }
    request.constraints.max_sets = options.sets;
  } else if (subcommand == "spines") {
    const auto leaf = parse_leaf_id(options.leaf.empty() ? options.from : options.leaf);
    if (!leaf.has_value()) {
      std::cout << "--leaf is required\n";
      return 2;
    }
    request.kind = net::RequestKind::EligibleSpines;
    request.from = NodeKey{*leaf};
    if (options.evidence) {
      request.constraints.require_evidence = true;
    }
  } else if (subcommand == "health") {
    request.kind = net::RequestKind::Health;
    request.constraints.require_evidence = options.evidence;
  } else if (subcommand == "lease") {
    request.kind = net::RequestKind::Status;
  } else if (subcommand == "compact") {
    request.kind = net::RequestKind::Compact;
  } else {
    std::cout << "unknown client subcommand: " << subcommand << "\n";
    return 2;
  }
  const auto response = client.value()->call(request);
  if (!response.ok()) {
    std::cout << "request failed: " << response.status().to_string() << "\n";
    return exit_code_for(response.code());
  }
  if (subcommand == "status" || subcommand == "lease") {
    std::cout << response.value().status.to_string();
  } else if (subcommand == "paths") {
    std::cout << response.value().assessment.to_string();
  } else if (subcommand == "spines") {
    for (const auto& entry : response.value().spines) {
      std::cout << to_string(entry.spine) << " domain=" << entry.domain.value
                << " capacity_mbps=" << entry.capacity_mbps << " verdict=" << to_string(entry.verdict)
                << " code=" << to_string(entry.code) << "\n";
    }
  } else if (subcommand == "health") {
    std::cout << response.value().health.to_string();
  } else {
    std::cout << response.value().detail << "\n";
  }
  (void)client.value()->close();
  return exit_code_for(response.value().code);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 2;
  }
  const std::string command = argv[1];
  if (command == "help" || command == "--help" || command == "-h") {
    print_usage();
    return 0;
  }
  if (command == "version" || command == "--version") {
    std::cout << build_info() << " interface=" << kInterfaceVersion
              << " persistence=" << kPersistenceFormatVersion << " protocol=" << kProtocolVersion << "\n";
    return 0;
  }
  const auto options = parse_options(argc, argv);
  if (!options.ok()) {
    std::cout << options.status().to_string() << "\n";
    print_usage();
    return 2;
  }
  const auto& value = options.value();
  if (command == "validate") return command_validate(value);
  if (command == "inspect") return command_inspect(value);
  if (command == "digest") return command_digest(value);
  if (command == "paths") return command_paths(value);
  if (command == "spines") return command_spines(value);
  if (command == "health") return command_health(value);
  if (command == "authorize") return command_authorize(value);
  if (command == "serve") return command_serve(value);
  if (command == "client") return command_client(value);
  std::cout << "unknown command: " << command << "\n";
  print_usage();
  return 2;
}
