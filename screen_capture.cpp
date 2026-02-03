#include "screen_capture.h"

// Constructor - Initialize all pointers to nullptr for safety
class SampleGrabberCallback : public IMFSampleGrabberSinkCallback {
private:
    LONG ref_count_;
    std::queue<EncodedFrame>* frame_queue_;
    std::mutex* queue_mutex_;

public:
    SampleGrabberCallback(std::queue<EncodedFrame>* queue, std::mutex* mutex)
        : ref_count_(1), frame_queue_(queue), queue_mutex_(mutex) {
    }

    // IUnknown implementation
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        static const QITAB qit[] = {
            QITABENT(SampleGrabberCallback, IMFSampleGrabberSinkCallback),
            QITABENT(SampleGrabberCallback, IMFClockStateSink),
            { 0 }
        };
        return QISearch(this, qit, riid, ppv);
    }

    STDMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement(&ref_count_);
    }

    STDMETHODIMP_(ULONG) Release() {
        ULONG count = InterlockedDecrement(&ref_count_);
        if (count == 0) delete this;
        return count;
    }

    // IMFClockStateSink implementation (required but not used)
    STDMETHODIMP OnClockStart(MFTIME, LONGLONG) { return S_OK; }
    STDMETHODIMP OnClockStop(MFTIME) { return S_OK; }
    STDMETHODIMP OnClockPause(MFTIME) { return S_OK; }
    STDMETHODIMP OnClockRestart(MFTIME) { return S_OK; }
    STDMETHODIMP OnClockSetRate(MFTIME, float) { return S_OK; }

    // IMFSampleGrabberSinkCallback - THIS IS WHERE WE GET ENCODED DATA!
    STDMETHODIMP OnProcessSample(
        REFGUID guidMajorMediaType,
        DWORD dwSampleFlags,
        LONGLONG llSampleTime,
        LONGLONG llSampleDuration,
        const BYTE* pSampleBuffer,
        DWORD dwSampleSize)
    {
        // This is called by Media Foundation when an encoded frame is ready!

        if (dwSampleSize == 0 || pSampleBuffer == nullptr) {
            return S_OK;  // Empty sample, skip
        }

        // Create encoded frame structure
        EncodedFrame frame;
        frame.data.resize(dwSampleSize);
        memcpy(frame.data.data(), pSampleBuffer, dwSampleSize);
        frame.timestamp = llSampleTime / 10;  // Convert 100ns to microseconds

        // Check if this is a keyframe (I-frame)
        // In H.264, keyframes start with NAL unit type 5 (IDR)
        // NAL header is in first byte: (pSampleBuffer[0] & 0x1F)
        if (dwSampleSize > 4) {
            // Check for NAL start code (00 00 00 01) followed by IDR (0x65)
            if (pSampleBuffer[0] == 0x00 && pSampleBuffer[1] == 0x00 &&
                pSampleBuffer[2] == 0x00 && pSampleBuffer[3] == 0x01) {
                uint8_t nal_type = pSampleBuffer[4] & 0x1F;
                frame.is_keyframe = (nal_type == 5);  // NAL type 5 = IDR (keyframe)
            }
        }

        frame.is_audio = false;

        // Add to queue (thread-safe)
        {
            std::lock_guard<std::mutex> lock(*queue_mutex_);
            frame_queue_->push(frame);
        }

        return S_OK;
    }

    STDMETHODIMP OnSetPresentationClock(IMFPresentationClock*) { return S_OK; }
    STDMETHODIMP OnShutdown() { return S_OK; }
};

ScreenCaptureEncoder::ScreenCaptureEncoder()
    : d3d_device_(nullptr)              // NULL until InitializeD3D11() succeeds
    , d3d_context_(nullptr)
    , desktop_duplication_(nullptr)
    , staging_texture_(nullptr)
    , video_sink_writer_(nullptr)
    , video_stream_index_(0)
    , pipe_handle_(INVALID_HANDLE_VALUE)  // Invalid handle value from Windows
    , width_(1920)                         // Default 1080p width
    , height_(1080)                        // Default 1080p height
    , fps_(60)                             // Default 60 FPS
    , frame_duration_(0)
    , running_(false)                      // Not running initially
{
}

// Destructor - Cleanup is handled in Stop()
ScreenCaptureEncoder::~ScreenCaptureEncoder() {
    Stop();  // Ensure everything is cleaned up
}

// Main initialization function - sets up all subsystems
bool ScreenCaptureEncoder::Initialize(int width, int height, int fps, const std::wstring& pipe_name) {
    width_ = width;
    height_ = height;
    fps_ = fps;
    pipe_name_ = pipe_name;
    
    // Calculate frame duration in 100-nanosecond units (Media Foundation uses this)
    // Example: 60 FPS = 16.67ms = 166,667 * 100ns
    frame_duration_ = 10000000ULL / fps_;  // 10,000,000 = 1 second in 100ns units
    
    // Initialize COM (Component Object Model) - required for DirectX and Media Foundation
    // COINIT_MULTITHREADED allows COM objects to be called from any thread
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize COM: 0x" << std::hex << hr << std::endl;
        return false;
    }
    
    // Initialize Media Foundation - required for video encoding
    // MFStartup must be called before using any MF APIs
    hr = MFStartup(MF_VERSION);  // MF_VERSION ensures version compatibility
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize Media Foundation: 0x" << std::hex << hr << std::endl;
        CoUninitialize();  // Cleanup COM if MF fails
        return false;
    }
    
    // Initialize each subsystem in order
    if (!InitializeD3D11()) {
        std::cerr << "Failed to initialize D3D11" << std::endl;
        return false;
    }
    
    if (!InitializeDuplication()) {
        std::cerr << "Failed to initialize desktop duplication" << std::endl;
        return false;
    }
    
    if (!InitializeVideoEncoder()) {
        std::cerr << "Failed to initialize video encoder" << std::endl;
        return false;
    }
    
    if (!InitializeNamedPipe()) {
        std::cerr << "Failed to initialize named pipe" << std::endl;
        return false;
    }
    
    std::cout << "Initialization complete!" << std::endl;
    return true;
}

// Initialize Direct3D 11 device and context
bool ScreenCaptureEncoder::InitializeD3D11() {
    // Feature levels to try (DirectX version support)
    // D3D_FEATURE_LEVEL_11_0 = DirectX 11.0 (most common)
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0  // We need at least D3D11 for desktop duplication
    };
    
    D3D_FEATURE_LEVEL feature_level_out;  // Will receive actual feature level
    
    // Create D3D11 device and immediate context
    // nullptr = use default adapter (primary GPU)
    // D3D_DRIVER_TYPE_HARDWARE = use GPU hardware acceleration
    // D3D11_CREATE_DEVICE_BGRA_SUPPORT = support BGRA format (needed for desktop capture)
    HRESULT hr = D3D11CreateDevice(
        nullptr,                           // Use default adapter (primary GPU)
        D3D_DRIVER_TYPE_HARDWARE,         // Use hardware GPU (not software emulation)
        nullptr,                           // No software rasterizer DLL
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, // Support BGRA pixel format
        feature_levels,                    // Array of feature levels to try
        1,                                 // Number of feature levels in array
        D3D11_SDK_VERSION,                // SDK version
        &d3d_device_,                     // OUT: receives device pointer
        &feature_level_out,               // OUT: receives actual feature level
        &d3d_context_                     // OUT: receives context pointer
    );
    
    if (FAILED(hr)) {
        std::cerr << "D3D11CreateDevice failed: 0x" << std::hex << hr << std::endl;
        return false;
    }
    
    std::cout << "D3D11 device created successfully" << std::endl;
    return true;
}

// Initialize Desktop Duplication API for screen capture
bool ScreenCaptureEncoder::InitializeDuplication() {
    // Get DXGI device from D3D11 device
    // QueryInterface converts d3d_device_ to IDXGIDevice interface
    IDXGIDevice* dxgi_device = nullptr;
    HRESULT hr = d3d_device_->QueryInterface(__uuidof(IDXGIDevice), 
                                             reinterpret_cast<void**>(&dxgi_device));
    if (FAILED(hr)) {
        std::cerr << "Failed to get DXGI device: 0x" << std::hex << hr << std::endl;
        return false;
    }
    
    // Get DXGI adapter (represents physical GPU)
    IDXGIAdapter* dxgi_adapter = nullptr;
    hr = dxgi_device->GetAdapter(&dxgi_adapter);  // Get parent adapter
    dxgi_device->Release();  // Release DXGI device (no longer needed)
    
    if (FAILED(hr)) {
        std::cerr << "Failed to get DXGI adapter: 0x" << std::hex << hr << std::endl;
        return false;
    }
    
    // Get output (monitor) - 0 is primary monitor
    IDXGIOutput* dxgi_output = nullptr;
    hr = dxgi_adapter->EnumOutputs(0, &dxgi_output);  // Enumerate output 0 (primary)
    dxgi_adapter->Release();  // Release adapter (no longer needed)
    
    if (FAILED(hr)) {
        std::cerr << "Failed to get DXGI output: 0x" << std::hex << hr << std::endl;
        return false;
    }
    
    // Query for IDXGIOutput1 interface (needed for duplication)
    IDXGIOutput1* dxgi_output1 = nullptr;
    hr = dxgi_output->QueryInterface(__uuidof(IDXGIOutput1), 
                                     reinterpret_cast<void**>(&dxgi_output1));
    dxgi_output->Release();  // Release base output
    
    if (FAILED(hr)) {
        std::cerr << "Failed to get IDXGIOutput1: 0x" << std::hex << hr << std::endl;
        return false;
    }
    
    // Create desktop duplication output
    // This gives us a handle to capture frames from the desktop
    hr = dxgi_output1->DuplicateOutput(d3d_device_, &desktop_duplication_);
    dxgi_output1->Release();  // Release output1
    
    if (FAILED(hr)) {
        std::cerr << "DuplicateOutput failed: 0x" << std::hex << hr << std::endl;
        std::cerr << "This can fail if another process is already capturing or in a game" << std::endl;
        return false;
    }
    
    // Create a staging texture for CPU access
    // Desktop duplication gives us GPU textures, we need CPU-accessible staging
    D3D11_TEXTURE2D_DESC staging_desc = {};
    staging_desc.Width = width_;                    // Texture width
    staging_desc.Height = height_;                  // Texture height
    staging_desc.MipLevels = 1;                     // No mipmaps
    staging_desc.ArraySize = 1;                     // Single texture (not array)
    staging_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // 32-bit BGRA format
    staging_desc.SampleDesc.Count = 1;              // No multisampling
    staging_desc.SampleDesc.Quality = 0;            // No multisampling
    staging_desc.Usage = D3D11_USAGE_STAGING;       // Staging = CPU can read
    staging_desc.BindFlags = 0;                     // No binding (staging only)
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;  // CPU can read
    staging_desc.MiscFlags = 0;                     // No misc flags
    
    hr = d3d_device_->CreateTexture2D(&staging_desc, nullptr, &staging_texture_);
    if (FAILED(hr)) {
        std::cerr << "Failed to create staging texture: 0x" << std::hex << hr << std::endl;
        return false;
    }
    
    std::cout << "Desktop duplication initialized successfully" << std::endl;
    return true;
}

// Initialize H.264 video encoder using Media Foundation
bool ScreenCaptureEncoder::InitializeVideoEncoder() {
    HRESULT hr;

    // Create sample grabber callback (this will receive encoded frames)
    SampleGrabberCallback* callback = new SampleGrabberCallback(&frame_queue_, &queue_mutex_);
    IMFActivate* sample_grabber_activate = nullptr;

    // Create sample grabber sink
    IMFMediaSink* sample_grabber_sink = nullptr;
    IMFMediaType* output_type = nullptr;

    // Create output media type (H.264)
    hr = MFCreateMediaType(&output_type);
    if (FAILED(hr)) return false;

    output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);

    hr = MFCreateSampleGrabberSinkActivate(output_type, callback, &sample_grabber_activate);
    output_type->Release();
    callback->Release();  // Activate takes ownership

    if (FAILED(hr)) {
        std::cerr << "Failed to create sample grabber: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // Activate to get the actual media sink
    IMFMediaSink* media_sink = nullptr;
    hr = sample_grabber_activate->ActivateObject(IID_PPV_ARGS(&media_sink));
    sample_grabber_activate->Release();

    // Create sink writer with sample grabber
    IMFAttributes* attrs = nullptr;
    hr = MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    // Use the activated sample grabber sink as the writer's output sink.
    sample_grabber_sink = media_sink;
    hr = MFCreateSinkWriterFromMediaSink(sample_grabber_sink, attrs, &video_sink_writer_);
    attrs->Release();
    sample_grabber_sink->Release();

    if (FAILED(hr)) {
        std::cerr << "Failed to create sink writer: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // Configure output format (H.264)
    hr = MFCreateMediaType(&output_type);
    if (FAILED(hr)) return false;

    output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    output_type->SetUINT32(MF_MT_AVG_BITRATE, 5000000);
    output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(output_type, MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(output_type, MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(output_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    // Configure input format (RGB32)
    IMFMediaType* input_type = nullptr;
    hr = MFCreateMediaType(&input_type);
    if (FAILED(hr)) {
        output_type->Release();
        return false;
    }

    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(input_type, MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(input_type, MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(input_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = video_sink_writer_->AddStream(output_type, &video_stream_index_);
    output_type->Release();

    if (FAILED(hr)) {
        input_type->Release();
        std::cerr << "Failed to add stream: 0x" << std::hex << hr << std::endl;
        return false;
    }

    hr = video_sink_writer_->SetInputMediaType(video_stream_index_, input_type, nullptr);
    input_type->Release();

    if (FAILED(hr)) {
        std::cerr << "Failed to set input media type: 0x" << std::hex << hr << std::endl;
        return false;
    }

    hr = video_sink_writer_->BeginWriting();
    if (FAILED(hr)) {
        std::cerr << "Failed to begin writing: 0x" << std::hex << hr << std::endl;
        return false;
    }

    std::cout << "Video encoder initialized successfully (with callback)" << std::endl;
    return true;
}


// Initialize named pipe for IPC with Go process
bool ScreenCaptureEncoder::InitializeNamedPipe() {
    // Create named pipe
    // Format: \\.\pipe\PipeName
    pipe_handle_ = CreateNamedPipeW(
        pipe_name_.c_str(),                  // Pipe name
        PIPE_ACCESS_OUTBOUND,                // Write-only access
        PIPE_TYPE_BYTE | PIPE_WAIT,          // Byte-type, blocking mode
        1,                                    // Max instances
        65536,                                // Output buffer size (64KB)
        0,                                    // Input buffer size (0 for outbound)
        0,                                    // Default timeout
        nullptr                               // Default security attributes
    );
    
    if (pipe_handle_ == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to create named pipe. Error: " << GetLastError() << std::endl;
        return false;
    }
    
    std::wcout << L"Named pipe created: " << pipe_name_ << std::endl;
    std::cout << "Waiting for Go process to connect..." << std::endl;
    
    // Wait for client (Go process) to connect
    // This blocks until a client calls CreateFile on the pipe
    BOOL connected = ConnectNamedPipe(pipe_handle_, nullptr);
    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
        std::cerr << "Failed to connect pipe. Error: " << GetLastError() << std::endl;
        CloseHandle(pipe_handle_);
        pipe_handle_ = INVALID_HANDLE_VALUE;
        return false;
    }
    
    std::cout << "Go process connected to pipe!" << std::endl;
    return true;
}

// Start capture threads
bool ScreenCaptureEncoder::Start() {
    if (running_) {
        std::cerr << "Already running!" << std::endl;
        return false;
    }
    
    running_ = true;  // Set atomic flag
    start_time_ = std::chrono::high_resolution_clock::now();  // Record start time
    
    // Launch capture thread
    // std::thread constructor calls CaptureLoop() in a new thread
    capture_thread_ = std::thread(&ScreenCaptureEncoder::CaptureLoop, this);
    
    // Launch pipe writing thread
    pipe_thread_ = std::thread(&ScreenCaptureEncoder::PipeWriteLoop, this);
    
    std::cout << "Capture started!" << std::endl;
    return true;
}

// Stop capture threads
void ScreenCaptureEncoder::Stop() {
    if (!running_) {
        return;  // Already stopped
    }
    
    running_ = false;  // Clear atomic flag (threads will exit)
    
    // Wait for threads to finish
    if (capture_thread_.joinable()) {
        capture_thread_.join();  // Block until thread exits
    }

    if (pipe_thread_.joinable()) {
        pipe_thread_.join();
    }
    
    // Cleanup COM objects
    if (video_sink_writer_) {
        video_sink_writer_->Finalize();  // Flush remaining samples
        video_sink_writer_->Release();
        video_sink_writer_ = nullptr;
    }
    
    
    if (staging_texture_) {
        staging_texture_->Release();
        staging_texture_ = nullptr;
    }
    
    if (desktop_duplication_) {
        desktop_duplication_->ReleaseFrame();  // Release any held frame
        desktop_duplication_->Release();
        desktop_duplication_ = nullptr;
    }
    
    if (d3d_context_) {
        d3d_context_->Release();
        d3d_context_ = nullptr;
    }
    
    if (d3d_device_) {
        d3d_device_->Release();
        d3d_device_ = nullptr;
    }
    
    if (pipe_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_handle_);  // Close pipe
        pipe_handle_ = INVALID_HANDLE_VALUE;
    }
    
    MFShutdown();      // Shutdown Media Foundation
    CoUninitialize();  // Shutdown COM
    
    std::cout << "Capture stopped and cleaned up" << std::endl;
}

// Main capture loop (runs in separate thread)
void ScreenCaptureEncoder::CaptureLoop() {
    std::cout << "Capture loop started" << std::endl;
    
    uint64_t frame_count = 0;
    
    while (running_) {
        // Calculate current timestamp
        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start_time_);
        uint64_t timestamp = elapsed.count();
        
        // Capture frame
        ID3D11Texture2D* acquired_texture = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frame_info = {};
        
        if (CaptureFrame(&acquired_texture, &frame_info)) {
            // Encode the frame
            EncodeVideoFrame(acquired_texture, timestamp);
            
            // Release the frame back to desktop duplication
            desktop_duplication_->ReleaseFrame();
            
            frame_count++;
        }
        
        // Sleep to maintain target FPS
        // frame_duration_ is in 100ns units, we need milliseconds
        uint64_t frame_duration_ms = frame_duration_ / 10000;
        std::this_thread::sleep_for(std::chrono::milliseconds(frame_duration_ms));
    }
    
    std::cout << "Capture loop ended. Frames captured: " << frame_count << std::endl;
}

// Pipe writing loop (sends frames to Go process)
void ScreenCaptureEncoder::PipeWriteLoop() {
    std::cout << "Pipe write loop started" << std::endl;
    
    while (running_) {
        EncodedFrame frame;
        bool has_frame = false;
        
        // Check queue for frames
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);  // Lock mutex
            if (!frame_queue_.empty()) {
                frame = frame_queue_.front();  // Copy front frame
                frame_queue_.pop();             // Remove from queue
                has_frame = true;
            }
        }  // Mutex automatically unlocked when lock_guard goes out of scope
        
        if (has_frame) {
            SendFrameToPipe(frame);
        } else {
            // No frames, sleep briefly
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    std::cout << "Pipe write loop ended" << std::endl;
}

// Capture one frame from desktop
bool ScreenCaptureEncoder::CaptureFrame(ID3D11Texture2D** out_texture, DXGI_OUTDUPL_FRAME_INFO* frame_info) {
    IDXGIResource* desktop_resource = nullptr;
    
    // Acquire next frame
    // This blocks until a new frame is available or timeout occurs
    HRESULT hr = desktop_duplication_->AcquireNextFrame(
        100,                // Timeout in milliseconds
        frame_info,         // OUT: frame info (metadata)
        &desktop_resource   // OUT: resource containing frame
    );
    
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new frame available (desktop hasn't changed)
        return false;
    }
    
    if (FAILED(hr)) {
        std::cerr << "AcquireNextFrame failed: 0x" << std::hex << hr << std::endl;
        return false;
    }
    
    // Query for ID3D11Texture2D interface
    hr = desktop_resource->QueryInterface(__uuidof(ID3D11Texture2D), 
                                          reinterpret_cast<void**>(out_texture));
    desktop_resource->Release();  // Release resource
    
    if (FAILED(hr)) {
        desktop_duplication_->ReleaseFrame();
        return false;
    }
    
    return true;
}

// Encode captured texture to H.264
bool ScreenCaptureEncoder::EncodeVideoFrame(ID3D11Texture2D* texture, uint64_t timestamp) {
    // Copy texture to staging texture (GPU to CPU)
    d3d_context_->CopyResource(staging_texture_, texture);
    
    // Map staging texture for CPU access
    D3D11_MAPPED_SUBRESOURCE mapped_resource = {};
    HRESULT hr = d3d_context_->Map(
        staging_texture_,           // Resource to map
        0,                          // Subresource index
        D3D11_MAP_READ,            // Map for reading
        0,                          // Flags
        &mapped_resource            // OUT: mapped data
    );
    
    if (FAILED(hr)) {
        std::cerr << "Failed to map texture: 0x" << std::hex << hr << std::endl;
        return false;
    }
    
    // Create Media Foundation sample
    IMFSample* sample = nullptr;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        d3d_context_->Unmap(staging_texture_, 0);
        return false;
    }
    
    // Create media buffer from mapped data
    IMFMediaBuffer* buffer = nullptr;
    DWORD buffer_size = mapped_resource.RowPitch * height_;  // Total bytes
    
    hr = MFCreateMemoryBuffer(buffer_size, &buffer);
    if (FAILED(hr)) {
        sample->Release();
        d3d_context_->Unmap(staging_texture_, 0);
        return false;
    }
    
    // Lock buffer and copy data
    BYTE* buffer_data = nullptr;
    hr = buffer->Lock(&buffer_data, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        // Copy each row (may have padding)
        for (int y = 0; y < height_; y++) {
            memcpy(
                buffer_data + y * width_ * 4,  // Destination (4 bytes per pixel)
                static_cast<BYTE*>(mapped_resource.pData) + y * mapped_resource.RowPitch,  // Source
                width_ * 4  // Bytes per row
            );
        }
        buffer->Unlock();
        buffer->SetCurrentLength(width_ * height_ * 4);  // Set actual data length
    }
    
    // Unmap staging texture
    d3d_context_->Unmap(staging_texture_, 0);
    
    // Add buffer to sample
    sample->AddBuffer(buffer);
    buffer->Release();
    
    // Set sample timestamp (in 100ns units)
    sample->SetSampleTime(timestamp * 10);  // Convert microseconds to 100ns
    
    // Set sample duration
    sample->SetSampleDuration(frame_duration_);
    
    // Write sample to encoder
    hr = video_sink_writer_->WriteSample(video_stream_index_, sample);
    sample->Release();
    
    if (FAILED(hr)) {
        std::cerr << "Failed to write video sample: 0x" << std::hex << hr << std::endl;
        return false;
    }
    
    // Note: Encoded data is not immediately available
    // Media Foundation buffers internally and outputs compressed frames
    // To get encoded data, you need to use IMFSinkWriterCallback or read from the byte stream
    
    return true;
}

bool ScreenCaptureEncoder::SendFrameToPipe(const EncodedFrame& frame) {
    // Protocol: [4 bytes: size] [size bytes: data]
    // This allows the Go process to know how much data to read
    
    uint32_t size = static_cast<uint32_t>(frame.data.size());
    DWORD bytes_written = 0;
    
    // Write size
    BOOL success = WriteFile(
        pipe_handle_,                 // Pipe handle
        &size,                         // Data to write
        sizeof(size),                  // Bytes to write
        &bytes_written,                // OUT: bytes written
        nullptr                        // Not overlapped
    );
    
    if (!success || bytes_written != sizeof(size)) {
        std::cerr << "Failed to write frame size to pipe" << std::endl;
        return false;
    }
    
    // Write data
    success = WriteFile(
        pipe_handle_,
        frame.data.data(),             // Frame data
        size,                          // Bytes to write
        &bytes_written,
        nullptr
    );
    
    if (!success || bytes_written != size) {
        std::cerr << "Failed to write frame data to pipe" << std::endl;
        return false;
    }
    
    // Flush pipe to ensure data is sent immediately
    FlushFileBuffers(pipe_handle_);
    
    return true;
}
