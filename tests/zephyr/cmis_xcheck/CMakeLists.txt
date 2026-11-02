# SPDX-License-Identifier: Apache-2.0
# Cross-check of the Zephyr CMIS library against Optical Module Twin vectors.
#   python3 tools/gen_vectors.py build/vectors
#   west twister -T tests/zephyr -p native_sim/native/64
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cmis_xcheck)

set(OMT_VECTORS ${CMAKE_CURRENT_SOURCE_DIR}/../../../build/vectors CACHE PATH "gen_vectors.py output")
if(NOT EXISTS ${OMT_VECTORS}/cmis_vectors.h)
  message(FATAL_ERROR "run tools/gen_vectors.py first (looked in ${OMT_VECTORS})")
endif()
target_include_directories(app PRIVATE ${OMT_VECTORS})
target_sources(app PRIVATE src/main.c)
