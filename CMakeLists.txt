cmake_minimum_required(VERSION 3.16)
project(MEOW LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(OpenMP REQUIRED)

set(ABC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/abc" CACHE PATH "Pinned ABC source")
if(NOT EXISTS "${ABC_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "Run python3 run.py build to fetch the pinned ABC dependency.")
endif()
add_subdirectory("${ABC_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/abc")

add_library(meow_core STATIC
  src/core/aig.cpp
  src/analysis/analysis.cpp
  src/analysis/meow.cpp
  src/pruning/netlist_transform.cpp
  src/pruning/pruning.cpp
  src/verification/wce_sat.cpp
  src/reporting/result_writer.cpp)
target_include_directories(meow_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(meow_core PRIVATE ABC_USE_STDINT_H=1)
target_link_libraries(meow_core PUBLIC OpenMP::OpenMP_CXX PRIVATE libabc)

add_executable(meow src/app/main.cpp)
target_link_libraries(meow PRIVATE meow_core)
add_executable(meow-wce src/app/exhaustive_wce.cpp)
target_link_libraries(meow-wce PRIVATE meow_core)
