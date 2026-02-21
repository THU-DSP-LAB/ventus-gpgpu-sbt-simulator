cmake_minimum_required(VERSION 3.20)

project(gpusim_sbt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(sbt_lib
  sbt/elf_reader.cpp
  sbt/spike_encoding_parser.cpp
  sbt/riscv_decode.cpp
  sbt/dump_parser.cpp
  sbt/cfg.cpp
  sbt/cfg_verify.cpp
  sbt/ptx_emit.cpp
)

target_include_directories(sbt_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(sbt_lib PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sbt_lib PRIVATE elf)

add_executable(sbt_decode tools/sbt_decode.cpp)
target_link_libraries(sbt_decode PRIVATE sbt_lib)

add_executable(gen_spike_encoding_subset tools/gen_spike_encoding_subset.cpp)
target_link_libraries(gen_spike_encoding_subset PRIVATE sbt_lib)

add_executable(sbt_ptx tools/sbt_ptx.cpp)
target_link_libraries(sbt_ptx PRIVATE sbt_lib)

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
