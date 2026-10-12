cmake_minimum_required(VERSION 3.21)
project(nfs13upscaler CXX)

# the upscaler sdks only ship 64-bit binaries
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NFS13Upscaler is x64 - configure with -A x64")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

set(EXT ${CMAKE_CURRENT_SOURCE_DIR}/../external)

add_executable(NFS13Upscaler main.cpp)
target_include_directories(NFS13Upscaler PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${EXT}/dlss/include
    ${EXT}/fsr/include
)
target_compile_definitions(NFS13Upscaler PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WINDOWS _CRT_SECURE_NO_WARNINGS)
target_link_libraries(NFS13Upscaler PRIVATE
    ${EXT}/dlss/lib/x64/nvsdk_ngx_d.lib
    d3d12 dxgi
)
# no console window
set_target_properties(NFS13Upscaler PROPERTIES WIN32_EXECUTABLE ON LINK_FLAGS "/ENTRY:wmainCRTStartup")

# runtime dlls next to the exe
add_custom_command(TARGET NFS13Upscaler POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${EXT}/dlss/bin/nvngx_dlss.dll
        ${EXT}/fsr/bin/amd_fidelityfx_loader_dx12.dll
        ${EXT}/fsr/bin/amd_fidelityfx_upscaler_dx12.dll
        $<TARGET_FILE_DIR:NFS13Upscaler>
)
