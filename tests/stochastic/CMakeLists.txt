# Training Traffic Fabric 1.0.0
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Stochastic and load surfaces: seeded property tests with invariant checks,
# concurrency and lifecycle races, and scale probes that expose accidental
# quadratic behaviour.

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/property_tests.cpp)
  add_executable(ttf_property_tests property_tests.cpp)
  target_link_libraries(ttf_property_tests PRIVATE ttf_test_support)
  ttf_apply_warnings(ttf_property_tests)
ttf_asan_deploy(ttf_property_tests)
  add_test(NAME property COMMAND ttf_property_tests)
else()
  message(STATUS "ttf_property_tests: property_tests.cpp is not present yet, target skipped")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/concurrency_tests.cpp)
  add_executable(ttf_concurrency_tests concurrency_tests.cpp)
  target_link_libraries(ttf_concurrency_tests PRIVATE ttf_test_support)
  ttf_apply_warnings(ttf_concurrency_tests)
ttf_asan_deploy(ttf_concurrency_tests)
  add_test(NAME concurrency COMMAND ttf_concurrency_tests)
else()
  message(STATUS "ttf_concurrency_tests: concurrency_tests.cpp is not present yet, target skipped")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/scale_tests.cpp)
  add_executable(ttf_scale_tests scale_tests.cpp)
  target_link_libraries(ttf_scale_tests PRIVATE ttf_test_support)
  ttf_apply_warnings(ttf_scale_tests)
ttf_asan_deploy(ttf_scale_tests)
  add_test(NAME scale COMMAND ttf_scale_tests)
else()
  message(STATUS "ttf_scale_tests: scale_tests.cpp is not present yet, target skipped")
endif()
