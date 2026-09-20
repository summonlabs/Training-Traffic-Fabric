# Training Traffic Fabric 1.0.0
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# An independent downstream project. It is deliberately NOT part of the runtime's
# own build: it consumes the installed package through find_package(ttf) and
# links ttf::ttf, exactly as an outside user would.

cmake_minimum_required(VERSION 3.25)

project(TrainingTrafficFabricConsumer
  VERSION 0.1.0
  DESCRIPTION "Independent consumer of the installed Training Traffic Fabric package"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ttf 1.0 REQUIRED CONFIG)

if(NOT TARGET ttf::ttf)
  message(FATAL_ERROR "the installed package did not provide the ttf::ttf target")
endif()

add_executable(ttf-consumer consumer_main.cpp)
target_link_libraries(ttf-consumer PRIVATE ttf::ttf)
if(MSVC)
  target_compile_options(ttf-consumer PRIVATE /W4 /WX /permissive- /utf-8)
else()
  target_compile_options(ttf-consumer PRIVATE -Wall -Wextra -Werror)
endif()

message(STATUS "consumer: Training Traffic Fabric v${ttf_VERSION} found at ${ttf_DIR}")
