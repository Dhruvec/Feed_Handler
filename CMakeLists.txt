cmake_minimum_required(VERSION 3.16)
project(feed_handler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-O3 -Wall -Wextra)
elseif(MSVC)
  add_compile_options(/O2 /W4)
endif()

# Header-only core library. Consumers compile against include/feed/*.hpp; this
# interface target only carries the include path and C++20 requirement.
add_library(feed_core INTERFACE)
target_include_directories(feed_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(feed_core INTERFACE cxx_std_20)

# Synthetic feed generator.
add_executable(feed_gen tools/feed_gen.cpp)
target_link_libraries(feed_gen PRIVATE feed_core)

# The feed handler itself.
add_executable(feed_handler src/main.cpp)
target_link_libraries(feed_handler PRIVATE feed_core)

# Correctness tests.
enable_testing()
add_executable(feed_tests tests/test_main.cpp)
target_link_libraries(feed_tests PRIVATE feed_core)
add_test(NAME feed_tests COMMAND feed_tests)
