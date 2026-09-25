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

# Independent downstream consumer. It is deliberately a separate CMake project
# that only ever calls find_package(spine_leaf_fabric) against an installed
# prefix: nothing from the source tree is on its include or link path.
cmake_minimum_required(VERSION 3.20)
project(spine_leaf_fabric_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(spine_leaf_fabric CONFIG REQUIRED)

add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE SummonLabs::slf)
