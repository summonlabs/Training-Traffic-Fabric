# Training Traffic Fabric 1.0.0
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

add_executable(ttf-coordinator coordinator_main.cpp)
target_link_libraries(ttf-coordinator PRIVATE ttf::ttf)
target_include_directories(ttf-coordinator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ttf_apply_warnings(ttf-coordinator)
ttf_asan_deploy(ttf-coordinator)

add_executable(ttfctl ctl_main.cpp)
target_link_libraries(ttfctl PRIVATE ttf::ttf)
target_include_directories(ttfctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ttf_apply_warnings(ttfctl)
ttf_asan_deploy(ttfctl)

add_executable(ttf-publisher publisher_main.cpp)
target_link_libraries(ttf-publisher PRIVATE ttf::ttf)
target_include_directories(ttf-publisher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ttf_apply_warnings(ttf-publisher)
ttf_asan_deploy(ttf-publisher)

install(TARGETS ttf-coordinator ttfctl ttf-publisher
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
