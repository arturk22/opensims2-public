cmake_minimum_required(VERSION 3.24)
project(opensims2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_library(os2_formats
  src/formats/dbpf.cpp
  src/formats/qfs.cpp
)
target_include_directories(os2_formats PUBLIC src)
target_compile_options(os2_formats PRIVATE -Wall -Wextra)

add_executable(s2pkg cli/s2pkg/main.cpp)
target_link_libraries(s2pkg PRIVATE os2_formats)

enable_testing()
add_executable(test_qfs tests/test_qfs.cpp)
target_link_libraries(test_qfs PRIVATE os2_formats)
add_test(NAME qfs COMMAND test_qfs)
