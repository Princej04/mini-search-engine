cmake_minimum_required(VERSION 3.14)
project(MiniSearch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)   # benchmark in Release, not Debug
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Core library: everything except main(), so tests can link against it.
add_library(searchlib
  src/tokenizer.cpp
  src/index.cpp
  src/index_io.cpp
  src/query.cpp
  src/corpus.cpp
)
target_include_directories(searchlib PUBLIC include)

# Command-line app
add_executable(search src/main.cpp)
target_link_libraries(search PRIVATE searchlib)

# Tests. Turn off with: cmake -S . -B build -DBUILD_TESTS=OFF
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
  # Use an installed GoogleTest if there is one, otherwise download it.
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)   # needed on Windows/MSVC
    FetchContent_MakeAvailable(googletest)
  endif()

  enable_testing()
  add_executable(tests
    tests/test_tokenizer.cpp
    tests/test_index.cpp
    tests/test_bm25.cpp
    tests/test_query.cpp
    tests/test_incremental.cpp
  )
  target_link_libraries(tests PRIVATE searchlib GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(tests)
endif()
