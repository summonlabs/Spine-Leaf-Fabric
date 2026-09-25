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

#include "slf_test.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace slf::test {
namespace {

std::vector<std::string> g_extra_arguments;

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

const std::vector<std::string>& extra_arguments() { return g_extra_arguments; }

}  // namespace slf::test

int main(int argc, char** argv) {
  std::string filter;
  std::uint64_t seed = 0x5EED1234ULL;
  bool list_only = false;
  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    } else if (argument.rfind("--seed=", 0) == 0) {
      seed = std::strtoull(argument.c_str() + 7, nullptr, 0);
    } else if (argument == "--list") {
      list_only = true;
    } else if (argument == "--verbose") {
      verbose = true;
    } else {
      slf::test::g_extra_arguments.push_back(argument);
    }
  }

  auto& tests = slf::test::registry();
  std::sort(tests.begin(), tests.end(), [](const slf::test::TestCase& a, const slf::test::TestCase& b) {
    return a.name < b.name;
  });
  if (list_only) {
    for (const auto& test : tests) {
      std::cout << test.name << "\n";
    }
    return 0;
  }

  int failed_tests = 0;
  int checks = 0;
  int executed = 0;
  for (const auto& test : tests) {
    if (!filter.empty() && std::string(test.name).find(filter) == std::string::npos) {
      continue;
    }
    ++executed;
    slf::test::Context context;
    context.test_name = std::string(test.name);
    context.seed = seed;
    context.sink = [verbose](const std::string& message) { std::cout << "    " << message << "\n"; };
    test.function(context);
    checks += context.checks;
    if (context.failures == 0) {
      std::cout << "[pass] " << test.name << " (" << context.checks << " checks)\n";
      continue;
    }
    ++failed_tests;
    std::cout << "[FAIL] " << test.name << " (" << context.failures << " of " << context.checks
              << " checks failed)\n";
    for (const auto& message : context.messages) {
      std::cout << "    " << message << "\n";
    }
  }
  std::cout << "seed=" << seed << " tests=" << executed << " failed=" << failed_tests
            << " checks=" << checks << "\n";
  return failed_tests == 0 ? 0 : 1;
}
