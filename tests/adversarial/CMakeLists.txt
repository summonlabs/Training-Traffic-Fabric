# Training Traffic Fabric 1.0.0
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Adversarial surfaces: codec, protocol, persistence and state-transition
# hardening. One executable per surface, one CTest entry per executable.

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/adversarial_tests.cpp)
  add_executable(ttf_adversarial_tests adversarial_tests.cpp)
  target_link_libraries(ttf_adversarial_tests PRIVATE ttf_test_support)
  ttf_apply_warnings(ttf_adversarial_tests)
ttf_asan_deploy(ttf_adversarial_tests)
  add_test(NAME adversarial COMMAND ttf_adversarial_tests)
else()
  message(STATUS "ttf_adversarial_tests: adversarial_tests.cpp is not present yet, target skipped")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/persistence_tests.cpp)
  add_executable(ttf_persistence_tests persistence_tests.cpp)
  target_link_libraries(ttf_persistence_tests PRIVATE ttf_test_support)
  ttf_apply_warnings(ttf_persistence_tests)
ttf_asan_deploy(ttf_persistence_tests)
  add_test(NAME persistence COMMAND ttf_persistence_tests)
else()
  message(STATUS "ttf_persistence_tests: persistence_tests.cpp is not present yet, target skipped")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/protocol_tests.cpp)
  add_executable(ttf_protocol_tests protocol_tests.cpp)
  target_link_libraries(ttf_protocol_tests PRIVATE ttf_test_support)
  ttf_apply_warnings(ttf_protocol_tests)
ttf_asan_deploy(ttf_protocol_tests)
  add_test(NAME protocol COMMAND ttf_protocol_tests)
else()
  message(STATUS "ttf_protocol_tests: protocol_tests.cpp is not present yet, target skipped")
endif()
