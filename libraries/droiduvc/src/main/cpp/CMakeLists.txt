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

add_library(droiduvc SHARED uvc_camera.cpp vulkan_probe.cpp)
target_compile_options(droiduvc PRIVATE -O2)
find_library(ANDROID_VULKAN_LIBRARY vulkan REQUIRED)
target_link_libraries(droiduvc PRIVATE droiduvc_core LibUVC::UVCStatic usb-1.0 android log ${ANDROID_VULKAN_LIBRARY})
