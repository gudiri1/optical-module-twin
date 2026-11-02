# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(optical_module_twin VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(OMT_SANITIZE "Build with ASan/UBSan" OFF)
option(OMT_COVERAGE "Build with gcov coverage" OFF)

set(OMT_WARN -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
             -Wno-float-conversion -Werror)
if(OMT_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
if(OMT_COVERAGE)
  add_compile_options(--coverage -O0)
  add_link_options(--coverage)
endif()

# ---- module controller firmware (target independent C11) ----
file(GLOB OMT_FW_SRC ${CMAKE_SOURCE_DIR}/firmware/src/*.c)
add_library(omt_fw STATIC ${OMT_FW_SRC})
target_include_directories(omt_fw PUBLIC firmware/include)
target_compile_options(omt_fw PRIVATE ${OMT_WARN})
target_link_libraries(omt_fw PUBLIC m)

# ---- hardware digital twin + C API (shared lib for Python/ctypes) ----
add_library(omt_sim SHARED sim/src/twin.cpp)
target_include_directories(omt_sim PUBLIC sim/include)
target_link_libraries(omt_sim PRIVATE omt_fw)
target_compile_options(omt_sim PRIVATE -Wall -Wextra -Werror)
set_target_properties(omt_sim PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ---- C unit tests ----
enable_testing()
file(GLOB OMT_UNIT_TESTS ${CMAKE_SOURCE_DIR}/tests/unit/test_*.c)
foreach(src ${OMT_UNIT_TESTS})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src} tests/unit/fake_hal.c)
  target_include_directories(${name} PRIVATE tests/unit)
  target_link_libraries(${name} PRIVATE omt_fw)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
  add_test(NAME ${name} COMMAND ${name})
endforeach()
