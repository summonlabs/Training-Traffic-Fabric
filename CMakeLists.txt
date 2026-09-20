cmake_minimum_required(VERSION 3.25)

project(TrainingTrafficFabric
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral C++20 runtime governing synchronization-heavy distributed-training network traffic"
  HOMEPAGE_URL "https://github.com/summonlabs/Training-Traffic-Fabric"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(TTF_BUILD_APPS "Build ttf-coordinator, ttfctl and ttf-publisher" ON)
option(TTF_BUILD_TESTS "Build the test suite" ON)
option(TTF_BUILD_EXAMPLES "Build the examples" ON)
option(TTF_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)
option(TTF_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(TTF_ENABLE_GPU_PROOF "Build the optional CUDA local-transfer evidence proof" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(BUILD_SHARED_LIBS AND WIN32 AND MSVC)
  # A shared build must export the public surface; the runtime declares no
  # dllexport annotations because static is the default shape.
  set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# First-party warning policy. Strict from the first commit: /W4 /WX on MSVC,
# -Wall -Wextra -Wpedantic -Werror elsewhere.
# ---------------------------------------------------------------------------

function(ttf_apply_warnings target)
  if(MSVC)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc)
    if(TTF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(TTF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# AddressSanitizer on MSVC links against a runtime DLL that lives next to the
# compiler. Deploying it beside each executable keeps `ctest` runnable from a
# plain shell without editing PATH.
function(ttf_asan_deploy target)
  if(TTF_ENABLE_ASAN AND MSVC)
    get_filename_component(_ttf_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_ttf_asan_dll "${_ttf_cxx_dir}/clang_rt.asan_dynamic-x86_64.dll")
    if(EXISTS "${_ttf_asan_dll}")
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_ttf_asan_dll}" "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Deploying the AddressSanitizer runtime beside ${target}")
    else()
      message(WARNING "TTF_ENABLE_ASAN: ${_ttf_asan_dll} was not found; tests need it on PATH")
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# The runtime library
# ---------------------------------------------------------------------------

add_library(ttf
  src/client.cpp
  src/codec.cpp
  src/coordinator.cpp
  src/fabric_api.cpp
  src/fabric_core.cpp
  src/fabric_recovery.cpp
  src/fabric_state.cpp
  src/fabric_traffic.cpp
  src/model.cpp
  src/net.cpp
  src/persistence.cpp
  src/util.cpp
  src/wire.cpp)

add_library(ttf::ttf ALIAS ttf)

target_include_directories(ttf
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_sources(ttf
  PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
      include/ttf/accounting.hpp
      include/ttf/client.hpp
      include/ttf/codec.hpp
      include/ttf/contract.hpp
      include/ttf/coordinator.hpp
      include/ttf/decision.hpp
      include/ttf/error.hpp
      include/ttf/explain.hpp
      include/ttf/fabric.hpp
      include/ttf/identity.hpp
      include/ttf/net.hpp
      include/ttf/parallelism.hpp
      include/ttf/persistence.hpp
      include/ttf/phase.hpp
      include/ttf/policy.hpp
      include/ttf/result.hpp
      include/ttf/topology.hpp
      include/ttf/util.hpp
      include/ttf/version.hpp
      include/ttf/wire.hpp)

target_link_libraries(ttf PUBLIC Threads::Threads)

if(WIN32)
  # Winsock. PUBLIC so an installed consumer links it without knowing this
  # runtime uses sockets.
  target_link_libraries(ttf PUBLIC ws2_32)
endif()

set_target_properties(ttf PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

if(MSVC)
  target_compile_definitions(ttf PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

ttf_apply_warnings(ttf)

if(TTF_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(ttf PUBLIC /fsanitize=address)
    target_link_options(ttf PUBLIC /INCREMENTAL:NO)
  else()
    target_compile_options(ttf PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(ttf PUBLIC -fsanitize=address)
  endif()
endif()

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------

install(TARGETS ttf
  EXPORT ttfTargets
  FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(EXPORT ttfTargets
  FILE ttfTargets.cmake
  NAMESPACE ttf::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ttf)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ttfConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ttfConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ttf)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/ttfConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/ttfConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/ttfConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ttf)

# ---------------------------------------------------------------------------
# Applications, examples and tests
# ---------------------------------------------------------------------------

if(TTF_BUILD_APPS)
  add_subdirectory(apps)
endif()

if(TTF_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(TTF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
