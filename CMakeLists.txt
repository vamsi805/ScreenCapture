# CMake build configuration for Screen Capture Encoder
# CMake is a cross-platform build system generator

# Minimum CMake version required
cmake_minimum_required(VERSION 3.10)

# Project name and language
project(ScreenCaptureEncoder CXX)

# Set C++ standard to C++17
# C++17 provides modern features like std::optional, structured bindings, etc.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add executable
# This tells CMake to build an executable named ScreenCaptureEncoder
# from the source files listed
add_executable(ScreenCaptureEncoder
    main.cpp           # Entry point
    screen_capture.cpp # Implementation
    screen_capture.h   # Header
)

# Link libraries
# These are the Windows libraries required for our program
target_link_libraries(ScreenCaptureEncoder
    d3d11          # Direct3D 11
    dxgi           # DirectX Graphics Infrastructure
    mfplat         # Media Foundation Platform
    mf             # Core Media Foundation (MFCreateSampleGrabberSinkActivate)
    mfuuid         # Media Foundation UUIDs
    mfreadwrite    # Media Foundation Read/Write
    wmcodecdspuuid # H.264 encoder CLSIDs
    ole32          # COM
    shlwapi      
    strmiids       # Additional Media Foundation IDs
)

# FFmpeg (NVENC) - set FFMPEG_ROOT to your FFmpeg install path
set(FFMPEG_ROOT "" CACHE PATH "Path to FFmpeg install (with include/lib)")
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h HINTS ${FFMPEG_ROOT}/include)
find_library(AVCODEC_LIB NAMES avcodec avcodec-60 avcodec-59 HINTS ${FFMPEG_ROOT}/lib)
find_library(AVUTIL_LIB NAMES avutil avutil-58 avutil-57 HINTS ${FFMPEG_ROOT}/lib)

if (FFMPEG_INCLUDE_DIR AND AVCODEC_LIB AND AVUTIL_LIB)
    target_include_directories(ScreenCaptureEncoder PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(ScreenCaptureEncoder ${AVCODEC_LIB} ${AVUTIL_LIB})
else()
    message(FATAL_ERROR "FFmpeg not found. Set FFMPEG_ROOT to your FFmpeg install path.")
endif()

# Compiler flags for Windows
if(MSVC)
    # /W4 = Warning level 4 (high)
    # /EHsc = Enable C++ exception handling
    target_compile_options(ScreenCaptureEncoder PRIVATE /W4 /EHsc)
endif()
