cmake_minimum_required(VERSION 3.16)
project(cpu_counter VERSION 0.1.0 LANGUAGES CXX)

# Detect whether we're the top-level project (CMake < 3.21 lacks PROJECT_IS_TOP_LEVEL).
if(NOT DEFINED PROJECT_IS_TOP_LEVEL)
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(PROJECT_IS_TOP_LEVEL TRUE)
  else()
    set(PROJECT_IS_TOP_LEVEL FALSE)
  endif()
endif()

include(GNUInstallDirs)  # defines CMAKE_INSTALL_INCLUDEDIR (used just below)

# ----------------------------------------------------------------------------
# The library: a single header. Consumers link `perf::perf`, which carries the
# include path and the C++20 requirement.
# ----------------------------------------------------------------------------
add_library(perf INTERFACE)
add_library(perf::perf ALIAS perf)
target_include_directories(perf INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(perf INTERFACE cxx_std_20)

# Build the demo lab, tests, and examples only when this is the top-level build
# (so add_subdirectory()/FetchContent consumers get just the library).
option(CPU_COUNTER_BUILD_DEMOS "Build the cpu_counter demo, tests, and examples" ${PROJECT_IS_TOP_LEVEL})

if(CPU_COUNTER_BUILD_DEMOS)
  add_compile_options(-Wall -Wextra -pedantic)

  add_executable(cpu_counter main.cpp demos/catalog.cpp demos/workloads.cpp)
  target_link_libraries(cpu_counter PRIVATE perf::perf)

  add_executable(perf_quickstart examples/quickstart.cpp)
  target_link_libraries(perf_quickstart PRIVATE perf::perf)

  enable_testing()
  add_executable(test_api_compile tests/api_compile.cpp)
  target_link_libraries(test_api_compile PRIVATE perf::perf)
  add_test(NAME api_compile COMMAND test_api_compile)

  add_executable(test_registry_check tests/registry_check.cpp demos/catalog.cpp demos/workloads.cpp)
  target_link_libraries(test_registry_check PRIVATE perf::perf)
  add_test(NAME registry_check COMMAND test_registry_check)
endif()

# ----------------------------------------------------------------------------
# Install + package config so downstreams can `find_package(perf CONFIG)`.
# ----------------------------------------------------------------------------
install(FILES perf.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS perf EXPORT perfTargets)
install(EXPORT perfTargets
  NAMESPACE perf::
  FILE perfConfig.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/perf)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/perfConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/perfConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/perf)
