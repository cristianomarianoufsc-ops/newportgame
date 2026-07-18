cmake_minimum_required(VERSION 3.16)
project(ps2_game CXX C)

# -----------------------------------------------------------------------
# Standards
# -----------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# -----------------------------------------------------------------------
# Dependencies — all installable on MX Linux via install_runtime_deps.sh
# -----------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

# SDL2
pkg_check_modules(SDL2 REQUIRED sdl2)

# OpenGL + GLEW
find_package(OpenGL REQUIRED)
find_package(GLEW   REQUIRED)

# OpenAL (optional — used by SPU2 stub when implemented)
find_package(OpenAL QUIET)
if(OPENAL_FOUND)
    message(STATUS "OpenAL: ${OPENAL_LIBRARY}")
else()
    message(STATUS "OpenAL: not found — audio will be silent")
endif()

# -----------------------------------------------------------------------
# Sources
# -----------------------------------------------------------------------
set(RUNTIME_CPP
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gs_stub.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bios_stub.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/spu2_stub.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/host_main.cpp
)

set(RUNTIME_C
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ps2_runtime_data.c
    # Generated + patched game translation (created by patch_output.py)
    ${CMAKE_CURRENT_SOURCE_DIR}/../build/output_runtime.c
)

# Mark C sources explicitly so CMake doesn't try to compile them as C++
set_source_files_properties(${RUNTIME_C} PROPERTIES LANGUAGE C)

# -----------------------------------------------------------------------
# Target
# -----------------------------------------------------------------------
add_executable(ps2_game ${RUNTIME_CPP} ${RUNTIME_C})

target_include_directories(ps2_game PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${SDL2_INCLUDE_DIRS}
)
if(OPENAL_FOUND)
    target_include_directories(ps2_game PRIVATE ${OPENAL_INCLUDE_DIR})
endif()

# -----------------------------------------------------------------------
# Compile flags
# -----------------------------------------------------------------------
target_compile_options(ps2_game PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wno-unused-function -Wno-unused-variable>
    ${SDL2_CFLAGS_OTHER}
    # Suppress label-only-in-block warning from generated goto code
    $<$<COMPILE_LANGUAGE:C>:-Wno-unused-label>
    # Optimise — adjust for debug with -O0 -g
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Debug>:-O0 -g -DPS2_DEBUG>
)

# output.c is large — give the compiler more time
# (useful on low-RAM machines like MX Linux on older hardware)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../build/output_runtime.c
    PROPERTIES COMPILE_FLAGS "-O1")

# -----------------------------------------------------------------------
# Link
# -----------------------------------------------------------------------
target_link_libraries(ps2_game PRIVATE
    ${SDL2_LIBRARIES}
    OpenGL::GL
    GLEW::GLEW
    m          # libm  (math functions used in generated C)
    pthread    # POSIX threads
)
if(OPENAL_FOUND)
    target_link_libraries(ps2_game PRIVATE ${OPENAL_LIBRARY})
endif()

# -----------------------------------------------------------------------
# Post-build: print binary size
# -----------------------------------------------------------------------
add_custom_command(TARGET ps2_game POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo
        "=== Build complete: $<TARGET_FILE:ps2_game> ==="
    COMMAND ls -lh $<TARGET_FILE:ps2_game>
    VERBATIM)

message(STATUS "SDL2:   ${SDL2_LIBRARIES}")
message(STATUS "OpenGL: ${OPENGL_LIBRARIES}")
message(STATUS "GLEW:   ${GLEW_LIBRARIES}")
