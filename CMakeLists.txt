cmake_minimum_required(VERSION 3.20)

project(ipc_shootout
  VERSION 0.1.0
  DESCRIPTION "Cross-platform benchmark of IPC mechanisms (latency, throughput, complexity)"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(ipc_bench src/ipc_bench.cpp)
target_include_directories(ipc_bench PRIVATE include)
target_compile_features(ipc_bench PRIVATE cxx_std_23)

if(MSVC)
  target_compile_options(ipc_bench PRIVATE /O2 /W3)
else()
  target_compile_options(ipc_bench PRIVATE -O2 -Wall -Wextra)
endif()

# Platform libraries: Winsock on Windows; librt for shm_open on Linux.
if(WIN32)
  target_link_libraries(ipc_bench PRIVATE ws2_32)
elseif(UNIX AND NOT APPLE)
  target_link_libraries(ipc_bench PRIVATE rt)
  # cpu_set_t / sched_setaffinity need _GNU_SOURCE under strict -std=c++23.
  target_compile_definitions(ipc_bench PRIVATE _GNU_SOURCE)
endif()

enable_testing()
# A smoke test per channel: the benchmark must complete (exit 0) with tiny counts.
foreach(ch tcp uds pipe shm)
  add_test(NAME smoke_${ch}
           COMMAND ipc_bench --channel ${ch} --warmup 10 --lat 200 --tput 50)
endforeach()
