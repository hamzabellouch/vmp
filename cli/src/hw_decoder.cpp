#include "hw_decoder.h"
#include <algorithm>

static enum AVPixelFormat g_hw_pix_fmt = AV_PIX_FMT_NONE;

enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    (void)ctx;
    const enum AVPixelFormat* p;
    for (p = pix_fmts; *p != -1; p++) {
        if (*p == g_hw_pix_fmt) {
            return *p;
        }
    }
    std::cerr << "[VMP HW] Note: Hardware surface format unavailable. Falling back to software decoding." << std::endl;
    for (p = pix_fmts; *p != -1; p++) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            return *p;
        }
    }
    return pix_fmts[0];
}

HardwareDecoder::HardwareDecoder() {}

HardwareDecoder::~HardwareDecoder() {
    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
    }
}

std::vector<std::string> HardwareDecoder::get_supported_hw_devices() {
    std::vector<std::string> supported_devices;
    
    // Priority order for optimal Linux video hardware decoding performance
    const std::vector<std::string> priority_order = {
        "vaapi", "cuda", "vulkan", "drm", "qsv", "vdpau"
    };

    std::cout << "[VMP Engine] Probing active HW Acceleration Engines on Linux..." << std::endl;
    for (const auto& dev_name : priority_order) {
        enum AVHWDeviceType type = av_hwdevice_find_type_by_name(dev_name.c_str());
        if (type != AV_HWDEVICE_TYPE_NONE) {
            AVBufferRef* test_ctx = nullptr;
            int err = av_hwdevice_ctx_create(&test_ctx, type, NULL, NULL, 0);
            if (err < 0 && type == AV_HWDEVICE_TYPE_VAAPI) {
                err = av_hwdevice_ctx_create(&test_ctx, type, "/dev/dri/renderD128", NULL, 0);
            }
            if (err >= 0 && test_ctx != nullptr) {
                supported_devices.push_back(dev_name);
                std::cout << " -> Found Active HW Engine: " << dev_name << std::endl;
                av_buffer_unref(&test_ctx);
            }
        }
    }
    return supported_devices;
}

bool HardwareDecoder::init_hardware_context(AVCodecContext* codec_ctx, enum AVHWDeviceType type) {
    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
        hw_device_ctx = nullptr;
    }

    int err = av_hwdevice_ctx_create(&hw_device_ctx, type, NULL, NULL, 0);
    if (err < 0 && type == AV_HWDEVICE_TYPE_VAAPI) {
        err = av_hwdevice_ctx_create(&hw_device_ctx, type, "/dev/dri/renderD128", NULL, 0);
        if (err < 0) {
            err = av_hwdevice_ctx_create(&hw_device_ctx, type, "/dev/dri/card0", NULL, 0);
        }
    } else if (err < 0 && type == AV_HWDEVICE_TYPE_DRM) {
        err = av_hwdevice_ctx_create(&hw_device_ctx, type, "/dev/dri/renderD128", NULL, 0);
        if (err < 0) {
            err = av_hwdevice_ctx_create(&hw_device_ctx, type, "/dev/dri/card0", NULL, 0);
        }
    }

    if (err < 0) {
        char errbuf[256];
        av_strerror(err, errbuf, sizeof(errbuf));
        std::cerr << "[VMP HW] Note: HW device " << av_hwdevice_get_type_name(type) 
                  << " not active on current GPU: " << errbuf << std::endl;
        return false;
    }

    active_device_type = type;
    
    // Find hardware pixel format for this config
    for (int i = 0;; i++) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec_ctx->codec, i);
        if (!config) {
            std::cerr << "[VMP HW] Decoder " << codec_ctx->codec->name 
                      << " does not support HW device type " << av_hwdevice_get_type_name(type) << std::endl;
            av_buffer_unref(&hw_device_ctx);
            hw_device_ctx = nullptr;
            return false;
        }
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            config->device_type == type) {
            g_hw_pix_fmt = config->pix_fmt;
            codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
            codec_ctx->get_format = get_hw_format;
            std::cout << "[VMP HW] Successfully attached HW Device: " << av_hwdevice_get_type_name(type) 
                      << " (PixFmt: " << av_get_pix_fmt_name(g_hw_pix_fmt) << ")" << std::endl;
            return true;
        }
    }

    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
        hw_device_ctx = nullptr;
    }
    return false;
}

AVPixelFormat HardwareDecoder::find_best_pixel_format(AVCodecContext* ctx, enum AVHWDeviceType type) {
    (void)ctx;
    (void)type;
    return g_hw_pix_fmt;
}
