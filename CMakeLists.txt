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
    wmcodecdspuuid # Color converter DMO UUIDs
    ole32          # COM
    shlwapi      
    strmiids       # Additional Media Foundation IDs
)

# Compiler flags for Windows
if(MSVC)
    # /W4 = Warning level 4 (high)
    # /EHsc = Enable C++ exception handling
    target_compile_options(ScreenCaptureEncoder PRIVATE /W4 /EHsc)
endif()
