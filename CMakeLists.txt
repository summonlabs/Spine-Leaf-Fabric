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

cmake_minimum_required(VERSION 3.20)

project(
  spine_leaf_fabric
  VERSION 1.0.0
  DESCRIPTION "Explicit tiered leaf/spine fabric representation and governance runtime"
  HOMEPAGE_URL "https://github.com/summonlabs/Spine-Leaf-Fabric"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SLF_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(SLF_BUILD_TESTS "Build the test suite" ON)
option(SLF_BUILD_TOOLS "Build slfctl" ON)
option(SLF_BUILD_BENCHMARKS "Build the benchmark binary" ON)
option(SLF_BUILD_EXAMPLES "Build the examples" ON)
option(SLF_ENABLE_ASAN "Build with AddressSanitizer" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Warnings and sanitizer configuration
# ---------------------------------------------------------------------------
add_library(slf_warnings INTERFACE)
if(MSVC)
  target_compile_options(slf_warnings INTERFACE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
  if(SLF_WARNINGS_AS_ERRORS)
    target_compile_options(slf_warnings INTERFACE /WX)
  endif()
  target_compile_definitions(slf_warnings INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(slf_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                                             -Wsign-conversion -Wold-style-cast)
  if(SLF_WARNINGS_AS_ERRORS)
    target_compile_options(slf_warnings INTERFACE -Werror)
  endif()
endif()

if(SLF_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(slf_warnings INTERFACE /fsanitize=address /Zi)
    target_link_options(slf_warnings INTERFACE /fsanitize=address /INCREMENTAL:NO)
  else()
    target_compile_options(slf_warnings INTERFACE -fsanitize=address -fno-omit-frame-pointer -g)
    target_link_options(slf_warnings INTERFACE -fsanitize=address)
  endif()
endif()

find_package(Threads REQUIRED)

# The framed transport uses the platform socket API directly.
if(WIN32)
  target_link_libraries(slf_warnings INTERFACE ws2_32)
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(SLF_SOURCES
    src/builder.cpp
    src/bytes.cpp
    src/capacity.cpp
    src/digest.cpp
    src/eligibility.cpp
    src/governance.cpp
    src/identity.cpp
    src/model.cpp
    src/platform.cpp
    src/runtime.cpp
    src/service.cpp
    src/spec.cpp
    src/status.cpp
    src/store.cpp
    src/topology.cpp
    src/transport.cpp
    src/version.cpp)

add_library(slf ${SLF_SOURCES})
add_library(SummonLabs::slf ALIAS slf)
add_library(slf::slf ALIAS slf)

target_include_directories(
  slf
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(slf PRIVATE slf_warnings)
target_link_libraries(slf PUBLIC Threads::Threads)
target_compile_features(slf PUBLIC cxx_std_20)

set_target_properties(slf PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})

# ---------------------------------------------------------------------------
# Tools, examples, benchmarks, tests
# ---------------------------------------------------------------------------
if(SLF_BUILD_TOOLS)
  add_executable(slfctl tools/slfctl/main.cpp)
  target_link_libraries(slfctl PRIVATE slf slf_warnings)
endif()

if(SLF_BUILD_EXAMPLES)
  add_executable(slf_example_basic examples/basic_fabric.cpp)
  target_link_libraries(slf_example_basic PRIVATE slf slf_warnings)
  add_executable(slf_example_governance examples/governance_fence.cpp)
  target_link_libraries(slf_example_governance PRIVATE slf slf_warnings)
endif()

if(SLF_BUILD_BENCHMARKS)
  add_executable(slfbench benchmarks/slfbench.cpp)
  target_link_libraries(slfbench PRIVATE slf slf_warnings)
endif()

if(SLF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# The warnings interface library is exported because the static library links it
# privately; ${LINK_ONLY:...} keeps its compile options from propagating into
# downstream consumers.
install(TARGETS slf_warnings EXPORT spine_leaf_fabricTargets)

install(TARGETS slf EXPORT spine_leaf_fabricTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(SLF_BUILD_TOOLS)
  install(TARGETS slfctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT spine_leaf_fabricTargets
        FILE spine_leaf_fabricTargets.cmake
        NAMESPACE SummonLabs::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/spine_leaf_fabric)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/spine_leaf_fabric-config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/spine_leaf_fabric-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/spine_leaf_fabric)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/spine_leaf_fabric-config-version.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/spine_leaf_fabric-config.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/spine_leaf_fabric-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/spine_leaf_fabric)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/spine_leaf_fabric)
