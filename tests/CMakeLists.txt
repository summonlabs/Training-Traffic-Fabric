# Training Traffic Fabric 1.0.0
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# One executable per proof surface, one CTest entry per executable. No test
# carries a TIMEOUT property: a hang is a defect to diagnose, not something to
# cut short.

add_library(ttf_test_support STATIC support/child_process.cpp)
target_link_libraries(ttf_test_support PUBLIC ttf::ttf)
target_include_directories(ttf_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
ttf_apply_warnings(ttf_test_support)

add_executable(ttf_unit_tests unit/unit_core.cpp)
target_link_libraries(ttf_unit_tests PRIVATE ttf_test_support)
ttf_apply_warnings(ttf_unit_tests)
ttf_asan_deploy(ttf_unit_tests)
add_test(NAME unit COMMAND ttf_unit_tests)

add_subdirectory(adversarial)
add_subdirectory(stochastic)

if(TTF_BUILD_APPS)
  add_executable(ttf_multiprocess_proof multiprocess/multiprocess_proof.cpp)
  target_link_libraries(ttf_multiprocess_proof PRIVATE ttf_test_support)
  target_compile_definitions(ttf_multiprocess_proof PRIVATE
    TTF_COORDINATOR_EXE="$<TARGET_FILE:ttf-coordinator>"
    TTF_PUBLISHER_EXE="$<TARGET_FILE:ttf-publisher>")
  ttf_apply_warnings(ttf_multiprocess_proof)
ttf_asan_deploy(ttf_multiprocess_proof)
  add_test(NAME multiprocess COMMAND ttf_multiprocess_proof)
else()
  message(STATUS "TTF_BUILD_APPS=OFF: the multiprocess proof is not built")
endif()
