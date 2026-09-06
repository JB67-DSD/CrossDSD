# Copyright 2026-present, Jean-Luc EVRARD.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# Author Jean-Luc EVRARD

cmake_minimum_required(VERSION 3.20)
project(CrossDSDPackageConsumer LANGUAGES C CXX)

find_package(CrossDSD 1.0 REQUIRED)

add_executable(crossdsd-package-consumer main.c)
target_link_libraries(crossdsd-package-consumer PRIVATE CrossDSD::formats-static)

set_property(TARGET crossdsd-package-consumer PROPERTY LINKER_LANGUAGE CXX)
