cmake_minimum_required(VERSION 3.16)
project(ps2recomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ps2recomp
    src/main.cpp
    src/iso/udf_parser.cpp
    src/elf/elf_loader.cpp
    src/mips/disasm.cpp
    src/recomp/recompiler.cpp
)

target_include_directories(ps2recomp PRIVATE src)

target_compile_options(ps2recomp PRIVATE
    -Wall -Wextra -O2
)
