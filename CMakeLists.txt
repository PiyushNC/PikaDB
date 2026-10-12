cmake_minimum_required(VERSION 3.20)
project(MiniDB LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 QUIET COMPONENTS Widgets)
if(Qt6_FOUND)
  set(CMAKE_AUTOMOC ON)
endif()

add_library(minidb src/minidb.cpp)
target_include_directories(minidb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
  target_compile_options(minidb PRIVATE /W4)
else()
  target_compile_options(minidb PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_executable(minidb_tests tests/test_minidb.cpp)
target_link_libraries(minidb_tests PRIVATE minidb)
add_test(NAME minidb_tests COMMAND minidb_tests)

add_executable(minidb_demo demo/minidb_demo.cpp)
target_link_libraries(minidb_demo PRIVATE minidb)

add_executable(minidb_benchmark benchmarks/minidb_benchmark.cpp)
target_link_libraries(minidb_benchmark PRIVATE minidb)

add_executable(minidb_shell shell/minidb_shell.cpp)
target_link_libraries(minidb_shell PRIVATE minidb)

if(Qt6_FOUND)
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/Gemini_Generated_Image_rulom0rulom0rulo.png
    ${CMAKE_CURRENT_BINARY_DIR}/Gemini_Generated_Image_rulom0rulom0rulo.png
    COPYONLY
  )
  add_executable(minidb_gui gui/minidb_gui.cpp)
  target_link_libraries(minidb_gui PRIVATE minidb Qt6::Widgets)
else()
  message(STATUS "Qt6 not found; skipping optional minidb_gui target")
endif()
