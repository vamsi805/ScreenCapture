#ifndef SCREEN_CAPTURE_H
#define SCREEN_CAPTURE_H

// Windows headers - must be included in this order to avoid conflicts
#include <windows.h>          // Core Windows API types and functions
#include <d3d11.h>            // Direct3D 11 interface for GPU access
#include <dxgi1_2.h>          // DirectX Graphics Infrastructure for desktop duplication
#include <mfapi.h>            // Media Foundation API for video encoding
#include <mfidl.h>            // Media Foundation interfaces
#include <mfreadwrite.h>      // Media Foundation read/write interfaces
#include <mferror.h>          // Media Foundation error codese
#include <mmdeviceapi.h>      // Multimedia device enumeration

// Standard library
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <chrono>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>

// Link required libraries - tells linker to include these .lib files
#pragma comment(lib, "d3d11.lib")        // Direct3D 11 library
#pragma comment(lib, "dxgi.lib")         // DXGI library
#pragma comment(lib, "mfplat.lib")       // Media Foundation platform library
#pragma comment(lib, "mfuuid.lib")       // Media Foundation UUIDs
#pragma comment(lib, "mfreadwrite.lib")  // Media Foundation sink writer
#pragma comment(lib, "ole32.lib")        // COM library for object creation

// Structure to hold a single encoded frame with timestamp
struct EncodedFrame {
    std::vector<uint8_t> data;      // Actual encoded video/audio bytes
    uint64_t timestamp;              // Timestamp in microseconds
    bool is_keyframe;                // True if this is an I-frame (keyframe)
    bool is_audio;                   // True for audio, false for video
    
    EncodedFrame() : timestamp(0), is_keyframe(false), is_audio(false) {}
};

// Main capture and encoding class
class ScreenCaptureEncoder {
public:
    ScreenCaptureEncoder();
    ~ScreenCaptureEncoder();
    
    // Initialize all components (D3D11, DXGI, encoders)
    bool Initialize(int width, int height, int fps, const std::wstring& pipe_name);
    
    // Start capture and encoding threads
    bool Start();
    
    // Stop all threads and cleanup
    void Stop();
    
    // Main capture loop (runs in separate thread)
    void CaptureLoop();
    
    
    // Pipe writing loop (runs in separate thread)
    void PipeWriteLoop();
    
private:
    // Direct3D 11 initialization
    bool InitializeD3D11();
    
    // Desktop Duplication API initialization
    bool InitializeDuplication();
    
    // Video encoder (H.264) initialization
    bool InitializeVideoEncoder();
    
    
    // Named pipe initialization for IPC with Go process
    bool InitializeNamedPipe();
    
    // Capture one frame from desktop
    bool CaptureFrame(ID3D11Texture2D** out_texture, DXGI_OUTDUPL_FRAME_INFO* frame_info);
    
    // Encode captured texture to H.264
    bool EncodeVideoFrame(ID3D11Texture2D* texture, uint64_t timestamp);
    
    // Send encoded frame through named pipe
    bool SendFrameToPipe(const EncodedFrame& frame);
    
    // D3D11 objects
    ID3D11Device* d3d_device_;                          // Direct3D 11 device object
    ID3D11DeviceContext* d3d_context_;                  // Device context for commands
    IDXGIOutputDuplication* desktop_duplication_;       // Desktop duplication interface
    ID3D11Texture2D* staging_texture_;                  // CPU-accessible staging texture
    
    // Media Foundation objects for video encoding
    IMFSinkWriter* video_sink_writer_;                  // Writes encoded samples
    DWORD video_stream_index_;                          // Stream index in sink writer
    
       
    // Named pipe for IPC
    HANDLE pipe_handle_;                                // Windows pipe handle
    std::wstring pipe_name_;                            // Pipe name (e.g., \\.\pipe\MyPipe)
    
    // Frame queue (thread-safe)
    std::queue<EncodedFrame> frame_queue_;              // Queue of frames to send
    std::mutex queue_mutex_;                            // Protects frame queue
    
    // Configuration
    int width_;                                          // Capture width in pixels
    int height_;                                         // Capture height in pixels
    int fps_;                                            // Target frames per second
    uint64_t frame_duration_;                            // Duration per frame in 100ns units
    
    // Threading
    std::atomic<bool> running_;                          // Atomic flag for thread safety
    std::thread capture_thread_;                         // Video capture thread
    std::thread pipe_thread_;                            // Pipe writing thread
    
    // Timing
    std::chrono::high_resolution_clock::time_point start_time_;  // Capture start time
};

#endif // SCREEN_CAPTURE_H
