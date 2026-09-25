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

#pragma once

// Minimal dependency-free test harness.
//
// * Tests self-register with SLF_TEST.
// * Failures never abort the process: they are recorded and reported with the
//   file, the line, and (for property tests) the seed that produced them.
// * c slf_test_main runs every registered test unless a filter is given.

#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "slf/eligibility.hpp"
#include "slf/identity.hpp"
#include "slf/model.hpp"
#include "slf/status.hpp"
#include "slf/store.hpp"

namespace slf::test {

/// Deterministic 64-bit PRNG (SplitMix64). Property tests print the seed they
/// used so a failing run can be reproduced exactly.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }

  [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept {
    return bound == 0 ? 0 : next() % bound;
  }

  [[nodiscard]] bool chance(std::uint32_t percent) noexcept { return below(100) < percent; }

  [[nodiscard]] std::uint64_t state() const noexcept { return state_; }

 private:
  std::uint64_t state_;
};

struct Context {
  std::string test_name;
  std::uint64_t seed{0};
  int failures{0};
  int checks{0};
  std::vector<std::string> messages;
  std::function<void(const std::string&)> sink;

  void report(const char* file, int line, const std::string& what) {
    ++failures;
    std::ostringstream out;
    out << file << ":" << line << ": " << what;
    // Failures are written to the unbuffered stream immediately so that a
    // defect which aborts the process still leaves the reason behind.
    std::cerr << "    " << out.str() << "\n";
    std::cerr.flush();
    if (seed != 0) {
      out << " [seed=" << seed << " (" << std::hex << seed << std::dec << ")]";
    }
    messages.push_back(out.str());
    if (sink) {
      sink(out.str());
    }
  }

  void note(const std::string& what) {
    std::ostringstream out;
    out << "note: " << what;
    messages.push_back(out.str());
  }
};

using TestFunction = void (*)(Context&);

struct TestCase {
  std::string_view name;
  TestFunction function;
  std::string_view file;
  int line;
};

/// Registry of all tests in the binary.
[[nodiscard]] std::vector<TestCase>& registry();

/// Command-line access for tests that need auxiliary paths (for example the
/// multiprocess test needs the path to slfctl).
[[nodiscard]] const std::vector<std::string>& extra_arguments();

struct Registrar {
  Registrar(std::string_view name, TestFunction function, std::string_view file, int line) {
    registry().push_back(TestCase{name, function, file, line});
  }
};

/// Evaluates a condition through a function so that a deliberately constant
/// condition (a compile-time invariant under test) does not trip the compiler's
/// "conditional expression is constant" warning, which is an error here.
[[nodiscard]] inline bool evaluate(bool value) noexcept { return value; }

/// Uniform access to the Status of either a Status or an Outcome<T>.
[[nodiscard]] inline const ::slf::Status& as_status(const ::slf::Status& status) { return status; }

template <class T>
[[nodiscard]] const ::slf::Status& as_status(const ::slf::Outcome<T>& outcome) {
  return outcome.status();
}

/// Formats a value for diagnostics.
template <class T, class = void>
struct is_streamable : std::false_type {};

template <class T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <class T>
[[nodiscard]] std::string show(const T& value) {
  if constexpr (is_streamable<T>::value) {
    std::ostringstream out;
    out << value;
    return out.str();
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else {
    return "<value>";
  }
}

[[nodiscard]] inline std::string show(bool value) { return value ? "true" : "false"; }
[[nodiscard]] inline std::string show(const std::string& value) { return "\"" + value + "\""; }
[[nodiscard]] inline std::string show(std::string_view value) { return "\"" + std::string(value) + "\""; }

template <class T>
[[nodiscard]] std::string show(const std::optional<T>& value) {
  return value.has_value() ? ("some(" + show(*value) + ")") : std::string("none");
}

template <class T>
[[nodiscard]] std::string show(const std::vector<T>& value) {
  std::string out = "[";
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i != 0) {
      out += ", ";
    }
    out += show(value[i]);
    if (i >= 7 && value.size() > 9) {
      out += ", ...";
      break;
    }
  }
  out += "]";
  return out;
}

// Enum diagnostics for the modelled vocabulary.
[[nodiscard]] inline std::string show(::slf::Tier value) { return std::string(::slf::to_string(value)); }
[[nodiscard]] inline std::string show(::slf::LeafRole value) { return std::string(::slf::to_string(value)); }
[[nodiscard]] inline std::string show(::slf::SpineRole value) { return std::string(::slf::to_string(value)); }
[[nodiscard]] inline std::string show(::slf::NodeState value) { return std::string(::slf::to_string(value)); }
[[nodiscard]] inline std::string show(::slf::PortAdmin value) { return std::string(::slf::to_string(value)); }
[[nodiscard]] inline std::string show(::slf::LinkAdmin value) { return std::string(::slf::to_string(value)); }
[[nodiscard]] inline std::string show(::slf::LinkClass value) { return std::string(::slf::to_string(value)); }
[[nodiscard]] inline std::string show(::slf::FabricState value) { return std::string(::slf::to_string(value)); }
[[nodiscard]] inline std::string show(::slf::EvidencePolicy value) { return std::string(::slf::to_string(value)); }
[[nodiscard]] inline std::string show(::slf::EvidenceFreshness value) {
  return std::string(::slf::to_string(value));
}
[[nodiscard]] inline std::string show(::slf::LinkObservation value) {
  return std::string(::slf::to_string(value));
}
[[nodiscard]] inline std::string show(::slf::FabricHealth value) { return std::string(::slf::to_string(value)); }
[[nodiscard]] inline std::string show(::slf::ReachabilityClass value) {
  return std::string(::slf::to_string(value));
}
[[nodiscard]] inline std::string show(::slf::EligibilityVerdict value) {
  return std::string(::slf::to_string(value));
}
[[nodiscard]] inline std::string show(::slf::OversubscriptionClass value) {
  return std::string(::slf::to_string(value));
}
[[nodiscard]] inline std::string show(::slf::StatusCode value) { return std::string(::slf::to_string(value)); }
[[nodiscard]] inline std::string show(::slf::RecoveryStatus value) {
  return std::string(::slf::to_string(value));
}
[[nodiscard]] inline std::string show(::slf::Digest value) { return value.short_hex(8); }
[[nodiscard]] inline std::string show(::slf::ControllerIncarnation value) {
  return ::slf::to_string(value);
}
[[nodiscard]] inline std::string show(::slf::NodeKey value) { return ::slf::to_string(value); }
[[nodiscard]] inline std::string show(const ::slf::Status& value) { return value.to_string(); }

template <class Tag, class Rep>
[[nodiscard]] std::string show(::slf::StrongId<Tag, Rep> value) {
  return ::slf::to_string(value);
}

}  // namespace slf::test

// Some assertions check invariants that the compiler can fold to a constant
// (for example "this checked-arithmetic helper refuses everything"). MSVC warns
// about a constant conditional expression (C4127) and we build with /WX, so the
// warning is suppressed - and only there - around the assertion conditions.
#if defined(_MSC_VER)
#  define SLF_ASSERT_GUARD __pragma(warning(push)) __pragma(warning(disable : 4127))
#  define SLF_ASSERT_RESTORE __pragma(warning(pop))
#else
#  define SLF_ASSERT_GUARD
#  define SLF_ASSERT_RESTORE
#endif

#define SLF_TEST(name)                                                                       \
  static void slf_test_##name(::slf::test::Context& slf_ctx);                                \
  static const ::slf::test::Registrar slf_registrar_##name(#name, &slf_test_##name, __FILE__, \
                                                          __LINE__);                         \
  static void slf_test_##name(::slf::test::Context& slf_ctx)

#define SLF_EXPECT(condition)                                                                   \
  do {                                                                                          \
    ++slf_ctx.checks;                                                                           \
    SLF_ASSERT_GUARD                                                                            \
    if (!::slf::test::evaluate(static_cast<bool>(condition))) {                                  \
      slf_ctx.report(__FILE__, __LINE__, std::string("expected: ") + #condition);                \
    }                                                                                           \
    SLF_ASSERT_RESTORE                                                                          \
  } while (false)

#define SLF_EXPECT_EQ(actual, expected)                                                          \
  do {                                                                                           \
    ++slf_ctx.checks;                                                                            \
    const auto& slf_actual_value = (actual);                                                      \
    const auto& slf_expected_value = (expected);                                                  \
    SLF_ASSERT_GUARD                                                                             \
    if (!(slf_actual_value == slf_expected_value)) {                                             \
      slf_ctx.report(__FILE__, __LINE__,                                                         \
                     std::string(#actual) + " == " + #expected + " (actual=" +                  \
                         ::slf::test::show(slf_actual_value) + ", expected=" +                   \
                         ::slf::test::show(slf_expected_value) + ")");                           \
    }                                                                                            \
    SLF_ASSERT_RESTORE                                                                           \
  } while (false)

#define SLF_EXPECT_NE(actual, expected)                                                          \
  do {                                                                                           \
    ++slf_ctx.checks;                                                                            \
    const auto& slf_actual_value = (actual);                                                      \
    const auto& slf_expected_value = (expected);                                                  \
    SLF_ASSERT_GUARD                                                                             \
    if (slf_actual_value == slf_expected_value) {                                                \
      slf_ctx.report(__FILE__, __LINE__, std::string(#actual) + " != " + #expected);             \
    }                                                                                            \
    SLF_ASSERT_RESTORE                                                                           \
  } while (false)

/// Asserts a StatusCode exactly. Because every status code is distinct, this is
/// how the tests pin down that UNKNOWN, STALE, CONFLICTING, INDETERMINATE,
/// REFUSED, CANCELLED and INVALID are never conflated.
#define SLF_EXPECT_CODE(expression, expected_code)                                               \
  do {                                                                                           \
    ++slf_ctx.checks;                                                                            \
    const auto& slf_outcome = (expression);                                                       \
    const ::slf::Status& slf_status_value = ::slf::test::as_status(slf_outcome);                  \
    const ::slf::StatusCode slf_code = slf_status_value.code();                                   \
    SLF_ASSERT_GUARD                                                                             \
    if (slf_code != (expected_code)) {                                                            \
      slf_ctx.report(__FILE__, __LINE__,                                                          \
                     std::string(#expression) + " code " + ::std::string(::slf::to_string(slf_code)) + \
                         " != " + ::std::string(::slf::to_string(expected_code)) +                \
                         " detail=\"" + std::string(slf_status_value.detail()) + "\"");          \
    }                                                                                            \
    SLF_ASSERT_RESTORE                                                                           \
  } while (false)

#define SLF_EXPECT_OK(expression) SLF_EXPECT_CODE(expression, ::slf::StatusCode::Ok)

#define SLF_FAIL(message)                                                       \
  do {                                                                          \
    ++slf_ctx.checks;                                                            \
    slf_ctx.report(__FILE__, __LINE__, std::string("failure: ") + (message));     \
  } while (false)
