cmake_minimum_required(VERSION 3.20)

project(sbtsim LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build-time pattern source (Spike encoding.h) for generating a hermetic subset header.
set(SBT_SPIKE_ENCODING_H "${CMAKE_CURRENT_SOURCE_DIR}/../spike/riscv/encoding.h" CACHE FILEPATH "Path to Spike encoding.h")
if (NOT EXISTS "${SBT_SPIKE_ENCODING_H}")
  message(FATAL_ERROR
    "SBT_SPIKE_ENCODING_H not found: ${SBT_SPIKE_ENCODING_H}\n"
    "Pass -DSBT_SPIKE_ENCODING_H=/abs/path/to/spike/riscv/encoding.h"
  )
endif ()

add_library(sbt_lib
  sbt/elf_reader.cpp
  sbt/spike_encoding_parser.cpp
  sbt/instruction_metadata.cpp
  sbt/riscv_decode.cpp
  sbt/dump_parser.cpp
  sbt/control_semantics.cpp
  sbt/cfg.cpp
  sbt/cfg_verify.cpp
  sbt/ptx_mma.cpp
  sbt/ptx_emit.cpp
  sbt/ptx_emit_core.cpp
  sbt/ptx_emit_runtime.cpp
  sbt/ptx_emit_memory.cpp
  sbt/ptx_emit_call.cpp
  sbt/ptx_emit_builtin.cpp
  sbt/ptx_emit_control.cpp
  sbt/ptx_emit_scalar.cpp
  sbt/ptx_emit_scalar_fp.cpp
  sbt/ptx_emit_vector.cpp
  sbt/ptx_emit_custom.cpp
  sbt/ptx_emit_mma_lowering.cpp
  sbt/want_file.cpp
)

target_include_directories(sbt_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(sbt_lib PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sbt_lib PRIVATE elf)

add_executable(sbt_decode tools/sbt_decode.cpp)
target_link_libraries(sbt_decode PRIVATE sbt_lib)

add_executable(gen_spike_encoding_subset tools/gen_spike_encoding_subset.cpp)
target_link_libraries(gen_spike_encoding_subset PRIVATE sbt_lib)

set(SBT_SPIKE_WANT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/data/spike_want.txt")
if (NOT EXISTS "${SBT_SPIKE_WANT_FILE}")
  message(FATAL_ERROR "Spike want file not found: ${SBT_SPIKE_WANT_FILE}")
endif ()
set(SBT_SPIKE_SUBSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SBT_SPIKE_SUBSET_HEADER "${SBT_SPIKE_SUBSET_DIR}/spike_encoding_subset.hpp")

add_custom_command(
  OUTPUT "${SBT_SPIKE_SUBSET_HEADER}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${SBT_SPIKE_SUBSET_DIR}"
  COMMAND "$<TARGET_FILE:gen_spike_encoding_subset>"
          --encoding-h "${SBT_SPIKE_ENCODING_H}"
          --want-file "${SBT_SPIKE_WANT_FILE}"
          --out "${SBT_SPIKE_SUBSET_HEADER}"
  DEPENDS
    gen_spike_encoding_subset
    "${SBT_SPIKE_WANT_FILE}"
    "${SBT_SPIKE_ENCODING_H}"
  VERBATIM
  COMMENT "Generating Spike encoding subset header: ${SBT_SPIKE_SUBSET_HEADER}"
)

add_custom_target(spike_encoding_subset_header DEPENDS "${SBT_SPIKE_SUBSET_HEADER}")

add_executable(sbt_ptx tools/sbt_ptx.cpp)
target_link_libraries(sbt_ptx PRIVATE sbt_lib)

add_dependencies(sbt_decode spike_encoding_subset_header)
add_dependencies(sbt_ptx spike_encoding_subset_header)
target_include_directories(sbt_decode PRIVATE "${SBT_SPIKE_SUBSET_DIR}")
target_include_directories(sbt_ptx PRIVATE "${SBT_SPIKE_SUBSET_DIR}")

add_executable(regext_bundle_test tools/regext_bundle_test.cpp)
target_link_libraries(regext_bundle_test PRIVATE sbt_lib)

add_executable(custom_decode_test tools/custom_decode_test.cpp)
target_link_libraries(custom_decode_test PRIVATE sbt_lib)

add_executable(mma_decode_test tools/mma_decode_test.cpp)
target_link_libraries(mma_decode_test PRIVATE sbt_lib)

add_executable(instruction_metadata_contract_test tools/instruction_metadata_contract_test.cpp)
target_link_libraries(instruction_metadata_contract_test PRIVATE sbt_lib)

add_executable(external_mnemonic_contract_test tools/external_mnemonic_contract_test.cpp)
target_link_libraries(external_mnemonic_contract_test PRIVATE sbt_lib)

add_executable(ptx_emit_call_prototype_test tools/ptx_emit_call_prototype_test.cpp)
target_link_libraries(ptx_emit_call_prototype_test PRIVATE sbt_lib)

add_executable(ptx_emit_leader_lane_abi_test tools/ptx_emit_leader_lane_abi_test.cpp)
target_link_libraries(ptx_emit_leader_lane_abi_test PRIVATE sbt_lib)

add_executable(custom_ptx_emit_test tools/custom_ptx_emit_test.cpp)
target_link_libraries(custom_ptx_emit_test PRIVATE sbt_lib)

add_executable(mma_ptx_emit_test tools/mma_ptx_emit_test.cpp)
target_link_libraries(mma_ptx_emit_test PRIVATE sbt_lib)

# Optional: OpenCL micro-test runner (requires Ventus OpenCL headers under Ventus install prefix).
set(VENTUS_INSTALL_PREFIX_HINT "$ENV{VENTUS_INSTALL_PREFIX}")
if (NOT VENTUS_INSTALL_PREFIX_HINT)
  # Integrated layout: this repo lives at "<ventus-env>/sbtsim".
  set(VENTUS_INSTALL_PREFIX_HINT "${CMAKE_CURRENT_SOURCE_DIR}/../install")
endif ()

if (EXISTS "${VENTUS_INSTALL_PREFIX_HINT}/include/CL/cl.h")
  find_library(OPENCL_LIB OpenCL
    HINTS
      "${VENTUS_INSTALL_PREFIX_HINT}/lib"
      /usr/local/cuda/targets/x86_64-linux/lib
      /usr/lib/x86_64-linux-gnu
  )
  if (OPENCL_LIB)
    add_executable(ventus_ocl_run tools/ventus_ocl_run.cpp)
    target_include_directories(ventus_ocl_run PRIVATE "${VENTUS_INSTALL_PREFIX_HINT}/include")
    target_compile_options(ventus_ocl_run PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(ventus_ocl_run PRIVATE "${OPENCL_LIB}")
  endif ()
endif ()

# Optional: LD_PRELOAD helper to bypass slow shell-outs in PoCL Ventus backend (assemble.sh, nm|grep pipeline).
add_library(ventus_pocl_fastpath SHARED tools/ventus_pocl_fastpath.cpp)
target_compile_options(ventus_pocl_fastpath PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ventus_pocl_fastpath PRIVATE dl)

# Optional: LD_PRELOAD helper for tracing CUDA Driver API calls (no direct dependency on our code).
find_path(CUDA_INCLUDE_DIR cuda.h HINTS /usr/local/cuda/include)
if (CUDA_INCLUDE_DIR)
  add_library(cuda_trace SHARED tools/cuda_trace.cpp)
  target_include_directories(cuda_trace PRIVATE ${CUDA_INCLUDE_DIR})
  target_compile_options(cuda_trace PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(cuda_trace PRIVATE dl)
endif ()

install(TARGETS sbt_ptx sbt_decode
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
