#include "screen_capture.h"
#include <wmcodecdsp.h> // CLSID_CMSH264EncoderMFT (fallback if needed)
#include <errno.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/rational.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

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

}  // namespace

class FfmpegNvencEncoder {
public:
    FfmpegNvencEncoder()
        : codec_ctx_(nullptr)
        , hw_device_ctx_(nullptr)
        , hw_frames_ctx_(nullptr)
        , device_(nullptr)
        , context_(nullptr)
        , width_(0)
        , height_(0)
        , fps_(0) {
    }

    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                    int width, int height, int fps, int bitrate) {
        if (!device) return false;

        width_ = width;
        height_ = height;
        fps_ = fps;
        device_ = device;
        context_ = context;
        device_->AddRef();
        if (context_) context_->AddRef();

        const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
        if (!codec) {
            std::cerr << "FFmpeg: h264_nvenc encoder not found" << std::endl;
            return false;
        }

        codec_ctx_ = avcodec_alloc_context3(codec);
        if (!codec_ctx_) {
            std::cerr << "FFmpeg: failed to allocate codec context" << std::endl;
            return false;
        }

        codec_ctx_->width = width_;
        codec_ctx_->height = height_;
        codec_ctx_->time_base = AVRational{1, fps_};
        codec_ctx_->framerate = AVRational{fps_, 1};
        codec_ctx_->gop_size = fps_ * 2;
        codec_ctx_->max_b_frames = 0;
        codec_ctx_->bit_rate = bitrate;
        codec_ctx_->pix_fmt = AV_PIX_FMT_D3D11;

        // Low-latency, WebRTC-friendly defaults.
        av_opt_set(codec_ctx_->priv_data, "preset", "p1", 0);
        av_opt_set(codec_ctx_->priv_data, "tune", "ll", 0);
        av_opt_set(codec_ctx_->priv_data, "rc", "cbr", 0);
        av_opt_set(codec_ctx_->priv_data, "profile", "baseline", 0);
        av_opt_set(codec_ctx_->priv_data, "repeat_headers", "1", 0);

        if (!InitHwDevice()) {
            return false;
        }

        if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
            std::cerr << "FFmpeg: avcodec_open2 failed" << std::endl;
            return false;
        }

        return true;
    }

    AVFrame* AcquireFrame() {
        if (!codec_ctx_ || !hw_frames_ctx_) return nullptr;

        AVFrame* frame = av_frame_alloc();
        if (!frame) return nullptr;

        frame->format = AV_PIX_FMT_D3D11;
        frame->width = width_;
        frame->height = height_;
        frame->hw_frames_ctx = av_buffer_ref(hw_frames_ctx_);

        if (av_hwframe_get_buffer(hw_frames_ctx_, frame, 0) < 0) {
            av_frame_free(&frame);
            return nullptr;
        }

        return frame;
    }

    bool GetFrameTexture(AVFrame* frame, ID3D11Texture2D** texture, UINT* subresource) {
        if (!frame || !texture || !subresource) return false;
        auto* desc = reinterpret_cast<AVD3D11FrameDescriptor*>(frame->data[0]);
        if (!desc || !desc->texture) return false;
        *texture = desc->texture;
        *subresource = desc->index;
        return true;
    }

    bool EncodeFrame(AVFrame* frame, uint64_t timestamp_us, std::vector<EncodedFrame>& out_frames) {
        if (!codec_ctx_ || !frame) return false;

        int64_t pts = av_rescale_q(static_cast<int64_t>(timestamp_us), AVRational{1, 1000000}, codec_ctx_->time_base);
        frame->pts = pts;

        int ret = avcodec_send_frame(codec_ctx_, frame);
        av_frame_free(&frame);
        if (ret < 0) {
            return false;
        }

        AVPacket pkt;
        av_init_packet(&pkt);

        while (ret >= 0) {
            ret = avcodec_receive_packet(codec_ctx_, &pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                av_packet_unref(&pkt);
                return false;
            }

            EncodedFrame out;
            if (pkt.size >= 4 &&
                pkt.data[0] == 0x00 && pkt.data[1] == 0x00 &&
                (pkt.data[2] == 0x01 || (pkt.data[2] == 0x00 && pkt.data[3] == 0x01))) {
                out.data.assign(pkt.data, pkt.data + pkt.size);
            } else {
                ConvertLengthPrefixedToAnnexB(pkt.data, pkt.size, out.data);
                if (out.data.empty()) {
                    out.data.assign(pkt.data, pkt.data + pkt.size);
                }
            }

            out.timestamp = timestamp_us;
            out.is_keyframe = (pkt.flags & AV_PKT_FLAG_KEY) != 0;
            out.is_audio = false;
            out_frames.push_back(std::move(out));

            av_packet_unref(&pkt);
        }

        return true;
    }

    void Shutdown() {
        if (codec_ctx_) {
            avcodec_free_context(&codec_ctx_);
            codec_ctx_ = nullptr;
        }

        if (hw_frames_ctx_) {
            av_buffer_unref(&hw_frames_ctx_);
            hw_frames_ctx_ = nullptr;
        }

        if (hw_device_ctx_) {
            av_buffer_unref(&hw_device_ctx_);
            hw_device_ctx_ = nullptr;
        }

        if (context_) {
            context_->Release();
            context_ = nullptr;
        }
        if (device_) {
            device_->Release();
            device_ = nullptr;
        }
    }

private:
    bool InitHwDevice() {
        hw_device_ctx_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hw_device_ctx_) {
            std::cerr << "FFmpeg: av_hwdevice_ctx_alloc failed" << std::endl;
            return false;
        }

        auto* device_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_device_ctx_->data);
        auto* d3d11_ctx = reinterpret_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx);
        d3d11_ctx->device = device_;
        d3d11_ctx->device_context = context_;

        if (av_hwdevice_ctx_init(hw_device_ctx_) < 0) {
            std::cerr << "FFmpeg: av_hwdevice_ctx_init failed" << std::endl;
            return false;
        }

        hw_frames_ctx_ = av_hwframe_ctx_alloc(hw_device_ctx_);
        if (!hw_frames_ctx_) {
            std::cerr << "FFmpeg: av_hwframe_ctx_alloc failed" << std::endl;
            return false;
        }

        auto* frames_ctx = reinterpret_cast<AVHWFramesContext*>(hw_frames_ctx_->data);
        frames_ctx->format = AV_PIX_FMT_D3D11;
        frames_ctx->sw_format = AV_PIX_FMT_NV12;
        frames_ctx->width = width_;
        frames_ctx->height = height_;
        frames_ctx->initial_pool_size = 4;

        auto* d3d11_frames = reinterpret_cast<AVD3D11VAFramesContext*>(frames_ctx->hwctx);
        if (d3d11_frames) {
            // NVENC needs render-target capable surfaces.
            d3d11_frames->BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        }

        if (av_hwframe_ctx_init(hw_frames_ctx_) < 0) {
            std::cerr << "FFmpeg: av_hwframe_ctx_init failed" << std::endl;
            return false;
        }

        codec_ctx_->hw_frames_ctx = av_buffer_ref(hw_frames_ctx_);
        return true;
    }

    AVCodecContext* codec_ctx_;
    AVBufferRef* hw_device_ctx_;
    AVBufferRef* hw_frames_ctx_;
    ID3D11Device* device_;
    ID3D11DeviceContext* context_;
    int width_;
    int height_;
    int fps_;
};

HRESULT CreateH264Encoder(IMFTransform** out_encoder) {
    if (!out_encoder) return E_POINTER;
    *out_encoder = nullptr;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    MFT_REGISTER_TYPE_INFO out_type = { MFMediaType_Video, MFVideoFormat_H264 };
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                   nullptr, &out_type, &activates, &count);
    if (SUCCEEDED(hr) && count > 0) {
        hr = activates[0]->ActivateObject(IID_PPV_ARGS(out_encoder));
    }

    for (UINT32 i = 0; i < count; ++i) {
        activates[i]->Release();
    }
    CoTaskMemFree(activates);

    if (SUCCEEDED(hr) && *out_encoder) {
        return hr;
    }

    // Fallback to software encoder if no hardware MFT is available.
    hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(out_encoder));
    return hr;
}

bool SetH264OutputType(IMFTransform* encoder, UINT32 width, UINT32 height, UINT32 fps) {
    if (!encoder) return false;

    IMFMediaType* type = nullptr;
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) return false;

    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    type->SetUINT32(MF_MT_AVG_BITRATE, 5000000);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(type, MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_MPEG2_PROFILE, 66); // Baseline

    hr = encoder->SetOutputType(0, type, MFT_SET_TYPE_TEST_ONLY);
    if (SUCCEEDED(hr)) {
        hr = encoder->SetOutputType(0, type, 0);
    }
    type->Release();

    if (FAILED(hr)) {
        std::cerr << "SetOutputType failed: 0x" << std::hex << hr << std::endl;
        return false;
    }
    return true;
}

bool SetH264InputType(IMFTransform* encoder, UINT32 width, UINT32 height, UINT32 fps) {
    if (!encoder) return false;

    auto try_type = [&](bool add_sample_size) -> HRESULT {
        IMFMediaType* type = nullptr;
        HRESULT hr = MFCreateMediaType(&type);
        if (FAILED(hr)) return hr;

        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(type, MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        if (add_sample_size) {
            type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
            type->SetUINT32(MF_MT_SAMPLE_SIZE, width * height * 3 / 2);
        }

        hr = encoder->SetInputType(0, type, MFT_SET_TYPE_TEST_ONLY);
        if (SUCCEEDED(hr)) {
            hr = encoder->SetInputType(0, type, 0);
        }
        type->Release();
        return hr;
    };

    HRESULT hr = try_type(false);
    if (SUCCEEDED(hr)) return true;

    hr = try_type(true);
    if (SUCCEEDED(hr)) return true;

    std::cerr << "SetInputType failed: 0x" << std::hex << hr << std::endl;
    return false;
}
ScreenCaptureEncoder::ScreenCaptureEncoder()
    : d3d_device_(nullptr)              // NULL until InitializeD3D11() succeeds
    , d3d_context_(nullptr)
    , desktop_duplication_(nullptr)
    , dxgi_manager_(nullptr)
    , reset_token_(0)
    , color_converter_(nullptr)
    , ffmpeg_encoder_(nullptr)
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
    
    std::cout << "Initializing video encoder..." << std::endl;
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
    UINT device_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(
        nullptr,                           // Use default adapter (primary GPU)
        D3D_DRIVER_TYPE_HARDWARE,         // Use hardware GPU (not software emulation)
        nullptr,                           // No software rasterizer DLL
        device_flags,                     // Support BGRA + video processing
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

    // Create DXGI device manager for Media Foundation MFTs (hardware path).
    hr = MFCreateDXGIDeviceManager(&reset_token_, &dxgi_manager_);
    if (FAILED(hr)) {
        std::cerr << "MFCreateDXGIDeviceManager failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    hr = dxgi_manager_->ResetDevice(d3d_device_, reset_token_);
    if (FAILED(hr)) {
        std::cerr << "DXGIDeviceManager ResetDevice failed: 0x" << std::hex << hr << std::endl;
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
    
    std::cout << "Desktop duplication initialized successfully" << std::endl;
    return true;
}

// Initialize H.264 video encoder using Media Foundation
bool ScreenCaptureEncoder::InitializeVideoEncoder() {
    HRESULT hr;

    std::cout << "[Encoder] Initializing GPU pipeline..." << std::endl;

    // Create GPU video processor (RGB32 -> NV12)
    hr = CoCreateInstance(CLSID_VideoProcessorMFT, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&color_converter_));
    if (FAILED(hr)) {
        std::cerr << "Failed to create video processor MFT: 0x" << std::hex << hr << std::endl;
        return false;
    }
    std::cout << "[Encoder] Video processor MFT created" << std::endl;

    IMFAttributes* cc_attrs = nullptr;
    if (SUCCEEDED(color_converter_->GetAttributes(&cc_attrs)) && cc_attrs) {
        cc_attrs->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
        cc_attrs->Release();
    }
    color_converter_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                     reinterpret_cast<ULONG_PTR>(dxgi_manager_));
    std::cout << "[Encoder] Video processor D3D manager set" << std::endl;

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
    std::cout << "[Encoder] Video processor input type set" << std::endl;

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
    std::cout << "[Encoder] Video processor output type set" << std::endl;

    color_converter_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    color_converter_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    ffmpeg_encoder_ = std::make_unique<FfmpegNvencEncoder>();
    if (!ffmpeg_encoder_->Initialize(d3d_device_, d3d_context_, width_, height_, fps_, 5000000)) {
        std::cerr << "Failed to initialize FFmpeg NVENC encoder" << std::endl;
        return false;
    }

    std::cout << "Video encoder initialized successfully (NVENC via FFmpeg)" << std::endl;
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
    
    // Cleanup encoder
    if (ffmpeg_encoder_) {
        ffmpeg_encoder_->Shutdown();
        ffmpeg_encoder_.reset();
    }

    if (color_converter_) {
        color_converter_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        color_converter_->Release();
        color_converter_ = nullptr;
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

    if (dxgi_manager_) {
        dxgi_manager_->Release();
        dxgi_manager_ = nullptr;
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
    if (!texture) {
        return false;
    }

    // Wrap the GPU texture directly in an MF sample (no CPU readback).
    IMFSample* rgb_sample = nullptr;
    HRESULT hr = MFCreateSample(&rgb_sample);
    if (FAILED(hr)) {
        return false;
    }

    IMFMediaBuffer* rgb_buffer = nullptr;
    hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0, FALSE, &rgb_buffer);
    if (FAILED(hr)) {
        rgb_sample->Release();
        std::cerr << "MFCreateDXGISurfaceBuffer failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    rgb_sample->AddBuffer(rgb_buffer);
    rgb_buffer->Release();
    rgb_sample->SetSampleTime(timestamp * 10);     // 100ns units
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

    // Option A: use FFmpeg's NV12 surfaces as the video processor output target.
    AVFrame* nv12_frame = ffmpeg_encoder_ ? ffmpeg_encoder_->AcquireFrame() : nullptr;
    if (!nv12_frame) {
        return false;
    }

    ID3D11Texture2D* nv12_texture = nullptr;
    UINT nv12_subresource = 0;
    if (!ffmpeg_encoder_->GetFrameTexture(nv12_frame, &nv12_texture, &nv12_subresource)) {
        av_frame_free(&nv12_frame);
        return false;
    }

    IMFSample* nv12_sample = nullptr;
    hr = MFCreateSample(&nv12_sample);
    if (FAILED(hr)) {
        av_frame_free(&nv12_frame);
        return false;
    }

    IMFMediaBuffer* nv12_buffer = nullptr;
    hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12_texture, nv12_subresource, FALSE, &nv12_buffer);
    if (FAILED(hr)) {
        nv12_sample->Release();
        av_frame_free(&nv12_frame);
        std::cerr << "MFCreateDXGISurfaceBuffer (NV12) failed: 0x" << std::hex << hr << std::endl;
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
        av_frame_free(&nv12_frame);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return true;
        }
        std::cerr << "Color converter ProcessOutput failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    nv12_sample->Release();

    std::vector<EncodedFrame> out_frames;
    if (!ffmpeg_encoder_ ||
        !ffmpeg_encoder_->EncodeFrame(nv12_frame, timestamp, out_frames)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (auto& frame : out_frames) {
            frame_queue_.push(std::move(frame));
        }
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
