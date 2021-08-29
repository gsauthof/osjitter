cmake_minimum_required(VERSION 3.1)
project(bench_syscalls CXX)


add_subdirectory(benchmark)

add_executable(bench_syscalls
    bench_syscalls.cc
    )
target_link_libraries(bench_syscalls benchmark::benchmark)


