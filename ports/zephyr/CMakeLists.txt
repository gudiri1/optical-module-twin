# SPDX-License-Identifier: Apache-2.0
# Zephyr port of the module controller firmware.
#   west build -b native_sim ports/zephyr        (bring-up smoke test)
#   west build -b <your_mcu_board> ports/zephyr  (real hardware, see boards/README)
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(omt_module_fw C)

set(OMT_FW ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware)
file(GLOB OMT_FW_SRC ${OMT_FW}/src/*.c)
target_sources(app PRIVATE ${OMT_FW_SRC} src/main.c src/hal_zephyr.c)
target_include_directories(app PRIVATE ${OMT_FW}/include)
