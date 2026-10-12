cmake_minimum_required(VERSION 3.21)
project(nfs13fix C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# libmem is distributed as -static-md, so the whole thing must be /MD.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# MW2012 is a 32-bit game
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "NFS13.exe is x86 - configure with -A Win32")
endif()

set(LIBMEM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/libmem-i686)

# SDL3, statically linked, gamepads only
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_UNINSTALL OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
add_subdirectory(external/SDL EXCLUDE_FROM_ALL)

add_library(nfs13fix SHARED
    src/borderless.cpp
    src/config.cpp
    src/dllmain.cpp
    src/framerate.cpp
    src/renderscale.cpp
    src/upscale.cpp
    src/upscaling.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/upscale_prep_cs.h
    src/iat.cpp
    src/input.cpp
    src/log.cpp
    src/modloader.cpp
    src/volume.cpp
)

target_include_directories(nfs13fix PRIVATE
    src
    ${CMAKE_CURRENT_BINARY_DIR}/gen
    ${LIBMEM_DIR}/include
)

target_compile_definitions(nfs13fix PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(nfs13fix PRIVATE
    SDL3::SDL3-static
    $<$<CONFIG:Debug>:${LIBMEM_DIR}/lib/debug/libmem.lib>
    $<$<NOT:$<CONFIG:Debug>>:${LIBMEM_DIR}/lib/release/libmem.lib>
    d3d11
    d3d12
    dxgi
    dxguid
    user32
    psapi
    shell32
    ntdll
    ws2_32
    userenv
    bcrypt
    advapi32
)

# Ultimate ASI Loader picks up *.asi next to the game exe.
set_target_properties(nfs13fix PROPERTIES
    OUTPUT_NAME "NFS13Fix"
    SUFFIX ".asi"
)

# diagnostic: what the bundled sdl sees, outside the game
add_executable(padtest EXCLUDE_FROM_ALL tools/padtest.cpp)
target_link_libraries(padtest PRIVATE SDL3::SDL3-static)

# upscaler client: compute shader compiled at build time
find_program(FXC fxc PATHS "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" REQUIRED)
set(UPSCALE_GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_custom_command(
    OUTPUT ${UPSCALE_GEN}/upscale_prep_cs.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${UPSCALE_GEN}
    COMMAND ${FXC} /nologo /T cs_5_0 /E main /O3 /Vn kUpscalePrepCS /Fh ${UPSCALE_GEN}/upscale_prep_cs.h
            ${CMAKE_CURRENT_SOURCE_DIR}/src/upscale_prep.hlsl
    DEPENDS src/upscale_prep.hlsl
)

# offline test of the upscaler path against the x64 helper
add_executable(uptest EXCLUDE_FROM_ALL tests/uptest.cpp src/upscale.cpp ${UPSCALE_GEN}/upscale_prep_cs.h)
target_include_directories(uptest PRIVATE src ${UPSCALE_GEN})
target_compile_definitions(uptest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(uptest PRIVATE d3d11 d3d12 dxgi d3dcompiler)
