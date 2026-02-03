#include "screen_capture.h"
#include <wmcodecdsp.h>

namespace {
void AppendStartCode(std::vector<uint8_t>& out) {
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x01);
}

bool ConvertAvccToAnnexB(const uint8_t* data, size_t len, std::vector<uint8_t>& out) {
    if (len < 7) {
        return false;
    }

    size_t offset = 5;  // Skip configuration version + profile/compat/level + lengthSizeMinusOne
    uint8_t num_sps = data[offset++] & 0x1F;
    for (uint8_t i = 0; i < num_sps; ++i) {
        if (offset + 2 > len) return false;
        uint16_t sps_len = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
        offset += 2;
        if (offset + sps_len > len) return false;
        AppendStartCode(out);
        out.insert(out.end(), data + offset, data + offset + sps_len);
        offset += sps_len;
    }

    if (offset + 1 > len) return false;
    uint8_t num_pps = data[offset++];
    for (uint8_t i = 0; i < num_pps; ++i) {
        if (offset + 2 > len) return false;
        uint16_t pps_len = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
        offset += 2;
        if (offset + pps_len > len) return false;
        AppendStartCode(out);
        out.insert(out.end(), data + offset, data + offset + pps_len);
        offset += pps_len;
    }

    return !out.empty();
}

bool ConvertLengthPrefixedToAnnexB(const uint8_t* data, size_t len, std::vector<uint8_t>& out) {
    size_t offset = 0;
    while (offset + 4 <= len) {
        uint32_t nal_len = (static_cast<uint32_t>(data[offset]) << 24) |
                           (static_cast<uint32_t>(data[offset + 1]) << 16) |
                           (static_cast<uint32_t>(data[offset + 2]) << 8) |
                           static_cast<uint32_t>(data[offset + 3]);
        offset += 4;
        if (offset + nal_len > len) break;
        AppendStartCode(out);
        out.insert(out.end(), data + offset, data + offset + nal_len);
        offset += nal_len;
    }
    return !out.empty();
}

bool ContainsKeyframe(const std::vector<uint8_t>& data) {
    for (size_t i = 0; i + 4 < data.size(); ++i) {
        if (data[i] == 0x00 && data[i + 1] == 0x00 &&
            ((data[i + 2] == 0x00 && data[i + 3] == 0x01) ||
             (data[i + 2] == 0x01))) {
            size_t nal_index = (data[i + 2] == 0x01) ? (i + 3) : (i + 4);
            if (nal_index < data.size()) {
                uint8_t nal_type = data[nal_index] & 0x1F;
                if (nal_type == 5) {
                    return true;
                }
            }
        }
    }
    return false;
}

HRESULT CreateH264Encoder(IMFTransform** out_encoder) {
    if (!out_encoder) return E_POINTER;
    *out_encoder = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(out_encoder));
    if (SUCCEEDED(hr)) return hr;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    MFT_REGISTER_TYPE_INFO out_type = { MFMediaType_Video, MFVideoFormat_H264 };
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                   nullptr, &out_type, &activates, &count);
    if (SUCCEEDED(hr) && count > 0) {
        hr = activates[0]->ActivateObject(IID_PPV_ARGS(out_encoder));
    }

    for (UINT32 i = 0; i < count; ++i) {
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    return hr;
}
}  // namespace

ScreenCaptureEncoder::ScreenCaptureEncoder()
    : d3d_device_(nullptr)              // NULL until InitializeD3D11() succeeds
    , d3d_context_(nullptr)
    , desktop_duplication_(nullptr)
    , staging_texture_(nullptr)
    , color_converter_(nullptr)
    , h264_encoder_(nullptr)
    , sent_sequence_header_(false)
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

    // Create color converter (RGB32 -> NV12)
    hr = CoCreateInstance(CLSID_CColorConvertDMO, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&color_converter_));
    if (FAILED(hr)) {
        std::cerr << "Failed to create color converter: 0x" << std::hex << hr << std::endl;
        return false;
    }

    IMFMediaType* rgb_type = nullptr;
    hr = MFCreateMediaType(&rgb_type);
    if (FAILED(hr)) return false;
    rgb_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgb_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    rgb_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(rgb_type, MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(rgb_type, MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(rgb_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = color_converter_->SetInputType(0, rgb_type, 0);
    rgb_type->Release();
    if (FAILED(hr)) {
        std::cerr << "Failed to set color converter input type: 0x" << std::hex << hr << std::endl;
        return false;
    }

    IMFMediaType* nv12_type = nullptr;
    hr = MFCreateMediaType(&nv12_type);
    if (FAILED(hr)) return false;
    nv12_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    nv12_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    nv12_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(nv12_type, MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(nv12_type, MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(nv12_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = color_converter_->SetOutputType(0, nv12_type, 0);
    nv12_type->Release();
    if (FAILED(hr)) {
        std::cerr << "Failed to set color converter output type: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // Create H.264 encoder MFT
    hr = CreateH264Encoder(&h264_encoder_);
    if (FAILED(hr) || !h264_encoder_) {
        std::cerr << "Failed to create H.264 encoder: 0x" << std::hex << hr << std::endl;
        return false;
    }

    IMFMediaType* enc_in = nullptr;
    hr = MFCreateMediaType(&enc_in);
    if (FAILED(hr)) return false;
    enc_in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    enc_in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    enc_in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(enc_in, MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(enc_in, MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(enc_in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = h264_encoder_->SetInputType(0, enc_in, 0);
    enc_in->Release();
    if (FAILED(hr)) {
        std::cerr << "Failed to set H.264 encoder input type: 0x" << std::hex << hr << std::endl;
        return false;
    }

    IMFMediaType* enc_out = nullptr;
    hr = MFCreateMediaType(&enc_out);
    if (FAILED(hr)) return false;
    enc_out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    enc_out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    enc_out->SetUINT32(MF_MT_AVG_BITRATE, 5000000);
    enc_out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(enc_out, MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(enc_out, MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(enc_out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = h264_encoder_->SetOutputType(0, enc_out, 0);
    enc_out->Release();
    if (FAILED(hr)) {
        std::cerr << "Failed to set H.264 encoder output type: 0x" << std::hex << hr << std::endl;
        return false;
    }

    IMFMediaType* current_out = nullptr;
    hr = h264_encoder_->GetOutputCurrentType(0, &current_out);
    if (SUCCEEDED(hr) && current_out) {
        UINT32 blob_size = 0;
        if (SUCCEEDED(current_out->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blob_size)) && blob_size > 0) {
            std::vector<uint8_t> avcc(blob_size);
            if (SUCCEEDED(current_out->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, avcc.data(), blob_size, nullptr))) {
                h264_sequence_header_.clear();
                ConvertAvccToAnnexB(avcc.data(), avcc.size(), h264_sequence_header_);
            }
        }
        current_out->Release();
    }

    color_converter_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    color_converter_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    h264_encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    h264_encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    sent_sequence_header_ = false;

    std::cout << "Video encoder initialized successfully (raw H.264 Annex-B)" << std::endl;
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
    if (h264_encoder_) {
        h264_encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        h264_encoder_->Release();
        h264_encoder_ = nullptr;
    }

    if (color_converter_) {
        color_converter_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        color_converter_->Release();
        color_converter_ = nullptr;
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
    
    // Create Media Foundation sample (RGB32)
    IMFSample* rgb_sample = nullptr;
    hr = MFCreateSample(&rgb_sample);
    if (FAILED(hr)) {
        d3d_context_->Unmap(staging_texture_, 0);
        return false;
    }
    
    // Create media buffer from mapped data
    IMFMediaBuffer* buffer = nullptr;
    DWORD buffer_size = mapped_resource.RowPitch * height_;  // Total bytes
    
    hr = MFCreateMemoryBuffer(buffer_size, &buffer);
    if (FAILED(hr)) {
        rgb_sample->Release();
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
    rgb_sample->AddBuffer(buffer);
    buffer->Release();

    // Set sample timestamp (in 100ns units)
    rgb_sample->SetSampleTime(timestamp * 10);  // Convert microseconds to 100ns

    // Set sample duration
    rgb_sample->SetSampleDuration(frame_duration_);

    // Convert RGB32 -> NV12
    hr = color_converter_->ProcessInput(0, rgb_sample, 0);
    rgb_sample->Release();
    if (FAILED(hr)) {
        std::cerr << "Color converter ProcessInput failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    MFT_OUTPUT_STREAM_INFO cc_info = {};
    hr = color_converter_->GetOutputStreamInfo(0, &cc_info);
    if (FAILED(hr)) {
        std::cerr << "Color converter GetOutputStreamInfo failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    DWORD nv12_buffer_size = 0;
    if (FAILED(MFCalculateImageSize(MFVideoFormat_NV12, width_, height_, &nv12_buffer_size))) {
        nv12_buffer_size = width_ * height_ * 3 / 2;
    }
    DWORD cc_cb = cc_info.cbSize > 0 ? cc_info.cbSize : nv12_buffer_size;

    IMFSample* nv12_sample = nullptr;
    hr = MFCreateSample(&nv12_sample);
    if (FAILED(hr)) return false;

    IMFMediaBuffer* nv12_buffer = nullptr;
    hr = MFCreateMemoryBuffer(cc_cb, &nv12_buffer);
    if (FAILED(hr)) {
        nv12_sample->Release();
        return false;
    }
    nv12_sample->AddBuffer(nv12_buffer);
    nv12_buffer->Release();
    nv12_sample->SetSampleTime(timestamp * 10);
    nv12_sample->SetSampleDuration(frame_duration_);

    MFT_OUTPUT_DATA_BUFFER cc_out = {};
    cc_out.dwStreamID = 0;
    cc_out.pSample = nv12_sample;
    DWORD cc_status = 0;
    hr = color_converter_->ProcessOutput(0, 1, &cc_out, &cc_status);
    if (cc_out.pEvents) {
        cc_out.pEvents->Release();
    }
    if (FAILED(hr)) {
        nv12_sample->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return true;
        }
        std::cerr << "Color converter ProcessOutput failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // Feed NV12 to H.264 encoder
    hr = h264_encoder_->ProcessInput(0, nv12_sample, 0);
    nv12_sample->Release();
    if (FAILED(hr)) {
        std::cerr << "H.264 encoder ProcessInput failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    MFT_OUTPUT_STREAM_INFO enc_info = {};
    hr = h264_encoder_->GetOutputStreamInfo(0, &enc_info);
    if (FAILED(hr)) {
        std::cerr << "H.264 encoder GetOutputStreamInfo failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    while (true) {
        IMFSample* enc_sample = nullptr;
        if ((enc_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
            DWORD enc_cb = enc_info.cbSize > 0 ? enc_info.cbSize : (width_ * height_ * 4);
            IMFMediaBuffer* enc_buffer = nullptr;
            if (FAILED(MFCreateSample(&enc_sample))) return false;
            if (FAILED(MFCreateMemoryBuffer(enc_cb, &enc_buffer))) {
                enc_sample->Release();
                return false;
            }
            enc_sample->AddBuffer(enc_buffer);
            enc_buffer->Release();
        }

        MFT_OUTPUT_DATA_BUFFER enc_out = {};
        enc_out.dwStreamID = 0;
        enc_out.pSample = enc_sample;
        DWORD enc_status = 0;
        hr = h264_encoder_->ProcessOutput(0, 1, &enc_out, &enc_status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (enc_out.pEvents) enc_out.pEvents->Release();
            if (enc_sample) enc_sample->Release();
            break;
        }
        if (FAILED(hr)) {
            if (enc_out.pEvents) enc_out.pEvents->Release();
            if (enc_sample) enc_sample->Release();
            std::cerr << "H.264 encoder ProcessOutput failed: 0x" << std::hex << hr << std::endl;
            return false;
        }

        IMFSample* out_sample = enc_out.pSample;
        IMFMediaBuffer* out_buffer = nullptr;
        if (SUCCEEDED(out_sample->ConvertToContiguousBuffer(&out_buffer))) {
            BYTE* out_data = nullptr;
            DWORD out_max = 0;
            DWORD out_len = 0;
            if (SUCCEEDED(out_buffer->Lock(&out_data, &out_max, &out_len))) {
                if (out_len > 0) {
                    std::vector<uint8_t> annexb;
                    if (out_len >= 4 &&
                        out_data[0] == 0x00 && out_data[1] == 0x00 &&
                        (out_data[2] == 0x01 || (out_data[2] == 0x00 && out_data[3] == 0x01))) {
                        annexb.assign(out_data, out_data + out_len);
                    } else if (!ConvertLengthPrefixedToAnnexB(out_data, out_len, annexb)) {
                        annexb.assign(out_data, out_data + out_len);
                    }

                    if (!sent_sequence_header_ && !h264_sequence_header_.empty()) {
                        EncodedFrame header_frame;
                        header_frame.data = h264_sequence_header_;
                        header_frame.timestamp = timestamp;
                        header_frame.is_keyframe = false;
                        header_frame.is_audio = false;
                        {
                            std::lock_guard<std::mutex> lock(queue_mutex_);
                            frame_queue_.push(std::move(header_frame));
                        }
                        sent_sequence_header_ = true;
                    }

                    EncodedFrame frame;
                    frame.data = std::move(annexb);
                    frame.timestamp = timestamp;
                    frame.is_keyframe = ContainsKeyframe(frame.data);
                    frame.is_audio = false;
                    {
                        std::lock_guard<std::mutex> lock(queue_mutex_);
                        frame_queue_.push(std::move(frame));
                    }
                }
                out_buffer->Unlock();
            }
            out_buffer->Release();
        }

        if (enc_out.pEvents) enc_out.pEvents->Release();
        if (out_sample) out_sample->Release();
    }

    return true;
}

bool ScreenCaptureEncoder::SendFrameToPipe(const EncodedFrame& frame) {
    // Protocol (little-endian):
    // [4 bytes: size] [8 bytes: timestamp_us] [1 byte: flags] [size bytes: data]
    // flags bit0 = keyframe, bit1 = audio
    
    uint32_t size = static_cast<uint32_t>(frame.data.size());
    uint64_t timestamp_us = frame.timestamp;
    uint8_t flags = 0;
    if (frame.is_keyframe) flags |= 0x01;
    if (frame.is_audio) flags |= 0x02;
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

    // Write timestamp (microseconds)
    success = WriteFile(
        pipe_handle_,
        &timestamp_us,
        sizeof(timestamp_us),
        &bytes_written,
        nullptr
    );

    if (!success || bytes_written != sizeof(timestamp_us)) {
        std::cerr << "Failed to write frame timestamp to pipe" << std::endl;
        return false;
    }

    // Write flags
    success = WriteFile(
        pipe_handle_,
        &flags,
        sizeof(flags),
        &bytes_written,
        nullptr
    );

    if (!success || bytes_written != sizeof(flags)) {
        std::cerr << "Failed to write frame flags to pipe" << std::endl;
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
