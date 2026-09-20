# Training Traffic Fabric 1.0.0
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

add_executable(ttf-fabric-demo fabric_demo.cpp)
target_link_libraries(ttf-fabric-demo PRIVATE ttf::ttf)
ttf_apply_warnings(ttf-fabric-demo)
ttf_asan_deploy(ttf-fabric-demo)
