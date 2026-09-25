# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Test suite. No CTest TIMEOUT is configured anywhere: every test must terminate
# on its own, and a hang is treated as a defect rather than as a pass.

set(SLF_TEST_SOURCES
    slf_test_main.cpp
    fixtures.cpp
    oracle.cpp
    test_identity.cpp
    test_model.cpp
    test_builder.cpp
    test_topology.cpp
    test_capacity.cpp
    test_eligibility.cpp
    test_governance.cpp
    test_spec.cpp
    test_store.cpp
    test_transport.cpp
    test_runtime.cpp
    test_property.cpp
    test_differential.cpp
    test_adversarial.cpp
    test_concurrency.cpp
    test_multiproc.cpp)

add_executable(slf_tests ${SLF_TEST_SOURCES})
target_link_libraries(slf_tests PRIVATE slf slf_warnings)
target_include_directories(slf_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)

# The multiprocess test spawns slfctl as a real child process and kills it at
# material lifecycle boundaries; the path is passed explicitly so the test never
# depends on the working directory or on PATH.
set(SLF_CTL_PATH "")
if(TARGET slfctl)
  set(SLF_CTL_PATH $<TARGET_FILE:slfctl>)
endif()

add_test(NAME slf_unit COMMAND slf_tests --filter=unit)
add_test(NAME slf_model COMMAND slf_tests --filter=model)
add_test(NAME slf_builder COMMAND slf_tests --filter=builder)
add_test(NAME slf_topology COMMAND slf_tests --filter=topology)
add_test(NAME slf_capacity COMMAND slf_tests --filter=capacity)
add_test(NAME slf_eligibility COMMAND slf_tests --filter=eligibility)
add_test(NAME slf_governance COMMAND slf_tests --filter=governance)
add_test(NAME slf_spec COMMAND slf_tests --filter=spec)
add_test(NAME slf_store COMMAND slf_tests --filter=store)
add_test(NAME slf_transport COMMAND slf_tests --filter=transport)
add_test(NAME slf_runtime COMMAND slf_tests --filter=runtime)
add_test(NAME slf_property COMMAND slf_tests --filter=property)
add_test(NAME slf_differential COMMAND slf_tests --filter=differential)
add_test(NAME slf_adversarial COMMAND slf_tests --filter=adversarial)
add_test(NAME slf_concurrency COMMAND slf_tests --filter=concurrency)
if(SLF_CTL_PATH)
  add_test(NAME slf_multiproc COMMAND slf_tests --filter=multiproc ${SLF_CTL_PATH})
endif()

# Every test also runs from one binary, which is what the release checklist uses.
add_test(NAME slf_all COMMAND slf_tests ${SLF_CTL_PATH})
