cmake_minimum_required(VERSION 3.22.1)
project(droiduvc LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Fetch the exact upstream revisions used by droiduvc v0.2.1.
# Pinning commits makes the native build reproducible without Git submodules.
include(FetchContent)

FetchContent_Declare(droiduvc_libjpeg
        GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo.git
        GIT_TAG 81feffa632bcd928d4cd1c35e5bb6c1eb02ac199
        GIT_PROGRESS TRUE)
FetchContent_Declare(droiduvc_libusb
        GIT_REPOSITORY https://github.com/libusb/libusb.git
        GIT_TAG 43107c84e4a5f6b15c296eff8cc3578100f35dce
        GIT_PROGRESS TRUE)
FetchContent_Declare(droiduvc_libuvc
        GIT_REPOSITORY https://github.com/libuvc/libuvc.git
        GIT_TAG 047920bcdfb1dac42424c90de5cc77dfc9fba04
        GIT_PROGRESS TRUE)

FetchContent_GetProperties(droiduvc_libjpeg)
if(NOT droiduvc_libjpeg_POPULATED)
    FetchContent_Populate(droiduvc_libjpeg)
endif()
FetchContent_GetProperties(droiduvc_libusb)
if(NOT droiduvc_libusb_POPULATED)
    FetchContent_Populate(droiduvc_libusb)
endif()
FetchContent_GetProperties(droiduvc_libuvc)
if(NOT droiduvc_libuvc_POPULATED)
    FetchContent_Populate(droiduvc_libuvc)
endif()

set(DROIDUVC_LIBJPEG_SOURCE_DIR ${droiduvc_libjpeg_SOURCE_DIR})
set(DROIDUVC_LIBUSB_SOURCE_DIR ${droiduvc_libusb_SOURCE_DIR})
set(DROIDUVC_LIBUVC_SOURCE_DIR ${droiduvc_libuvc_SOURCE_DIR})

# libjpeg-turbo is built as an ExternalProject; libusb is compiled into a
# static target; libuvc is added as a subdirectory after both targets exist.
include(cmake/libjpeg-turbo.cmake)
include(cmake/libusb.cmake)
target_compile_options(usb-1.0 PRIVATE -O2)

set(JPEG_FOUND TRUE)
set(CMAKE_BUILD_TARGET Static CACHE STRING "" FORCE)
set(BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(CMAKE_SKIP_INSTALL_RULES ON)
add_subdirectory(${DROIDUVC_LIBUVC_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/libuvc EXCLUDE_FROM_ALL)
unset(CMAKE_SKIP_INSTALL_RULES)

# Debug APKs must still use optimized native video code. MJPEG decode and RGB
# conversion run on CPU until the GPU texture path is implemented.
if(TARGET uvc_static)
    target_compile_options(uvc_static PRIVATE -O2)
endif()

add_subdirectory(core)
target_compile_options(droiduvc_core PRIVATE -O2)

# RIFE 4.6 + ncnn Vulkan: build the inference library, not the upstream CLI.
# Pin both source revisions so the native build remains reproducible.
set(NCNN_VULKAN ON CACHE BOOL "" FORCE)
set(NCNN_VULKAN_ONLINE_SPIRV ON CACHE BOOL "" FORCE)
set(NCNN_OPENMP OFF CACHE BOOL "" FORCE)
set(NCNN_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NCNN_INSTALL_SDK OFF CACHE BOOL "" FORCE)
set(NCNN_SYSTEM_GLSLANG OFF CACHE BOOL "" FORCE)
set(NCNN_INT8 ON CACHE BOOL "" FORCE)
set(NCNN_PIXEL ON CACHE BOOL "" FORCE)

find_library(ANDROID_VULKAN_LIBRARY vulkan REQUIRED)
set(Vulkan_LIBRARY ${ANDROID_VULKAN_LIBRARY} CACHE FILEPATH "" FORCE)
set(Vulkan_INCLUDE_DIR ${CMAKE_SYSROOT}/usr/include CACHE PATH "" FORCE)

FetchContent_Declare(itx_ncnn
        GIT_REPOSITORY https://github.com/Tencent/ncnn.git
        GIT_TAG b4ba207c18d3103d6df890c0e3a97b469b196b26
        GIT_SUBMODULES glslang
        GIT_SUBMODULES_RECURSE TRUE
        GIT_PROGRESS TRUE)
FetchContent_GetProperties(itx_ncnn)
if(NOT itx_ncnn_POPULATED)
    FetchContent_Populate(itx_ncnn)
endif()
add_subdirectory(${itx_ncnn_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/itx_ncnn EXCLUDE_FROM_ALL)

FetchContent_Declare(itx_rife_source
        GIT_REPOSITORY https://github.com/nihui/rife-ncnn-vulkan.git
        GIT_TAG a7532fc3f9f8f008cd6eecd6f2ffe2a9698e0cf7
        GIT_SUBMODULES ""
        GIT_PROGRESS TRUE)
FetchContent_GetProperties(itx_rife_source)
if(NOT itx_rife_source_POPULATED)
    FetchContent_Populate(itx_rife_source)
endif()

set(ITX_RIFE_SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/rife_shader_headers)
file(MAKE_DIRECTORY ${ITX_RIFE_SHADER_OUTPUT_DIR})
set(ITX_RIFE_SHADER_HEADERS)
function(itx_add_rife_shader shader_name)
    set(shader_src ${itx_rife_source_SOURCE_DIR}/src/${shader_name}.comp)
    set(shader_header ${ITX_RIFE_SHADER_OUTPUT_DIR}/${shader_name}.comp.hex.h)
    add_custom_command(
            OUTPUT ${shader_header}
            COMMAND ${CMAKE_COMMAND} -DSHADER_SRC=${shader_src}
                    -DSHADER_COMP_HEADER=${shader_header}
                    -P ${itx_rife_source_SOURCE_DIR}/src/generate_shader_comp_header.cmake
            DEPENDS ${shader_src}
                    ${itx_rife_source_SOURCE_DIR}/src/generate_shader_comp_header.cmake
            COMMENT "Embedding RIFE shader ${shader_name}"
            VERBATIM)
    set(ITX_RIFE_SHADER_HEADERS ${ITX_RIFE_SHADER_HEADERS} ${shader_header} PARENT_SCOPE)
endfunction()

foreach(shader_name
        rife_preproc
        rife_postproc
        rife_preproc_tta
        rife_postproc_tta
        rife_flow_tta_avg
        rife_v2_flow_tta_avg
        rife_v4_flow_tta_avg
        rife_flow_tta_temporal_avg
        rife_v2_flow_tta_temporal_avg
        rife_v4_flow_tta_temporal_avg
        rife_out_tta_temporal_avg
        rife_v4_timestep
        rife_v4_timestep_tta
        warp
        warp_pack4
        warp_pack8)
    itx_add_rife_shader(${shader_name})
endforeach()

add_custom_target(itx_rife_shader_headers DEPENDS ${ITX_RIFE_SHADER_HEADERS})
add_library(itx_rife STATIC
        ${itx_rife_source_SOURCE_DIR}/src/rife.cpp
        ${itx_rife_source_SOURCE_DIR}/src/warp.cpp
        ${ITX_RIFE_SHADER_HEADERS})
add_dependencies(itx_rife itx_rife_shader_headers)
target_include_directories(itx_rife PRIVATE
        ${itx_rife_source_SOURCE_DIR}/src
        ${itx_ncnn_SOURCE_DIR}/src
        ${ITX_RIFE_SHADER_OUTPUT_DIR})
target_link_libraries(itx_rife PUBLIC ncnn)
target_compile_options(itx_rife PRIVATE -O2)

add_library(droiduvc SHARED uvc_camera.cpp vulkan_probe.cpp rife_engine.cpp)
target_compile_options(droiduvc PRIVATE -O2)
target_link_libraries(droiduvc PRIVATE droiduvc_core LibUVC::UVCStatic usb-1.0 android log ${ANDROID_VULKAN_LIBRARY} itx_rife)
