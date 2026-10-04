#ifndef VMP_HW_DECODER_H
#define VMP_HW_DECODER_H

#include <iostream>
#include <vector>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

class HardwareDecoder {
public:
    HardwareDecoder();
    ~HardwareDecoder();

    // Auto-detect and initialize hardware acceleration device for given codec
    AVPixelFormat find_best_pixel_format(AVCodecContext* ctx, enum AVHWDeviceType type);
    bool init_hardware_context(AVCodecContext* codec_ctx, enum AVHWDeviceType type);
    
    // Probes supported hardware device types on current system
    std::vector<std::string> get_supported_hw_devices();
    
    enum AVHWDeviceType active_device_type = AV_HWDEVICE_TYPE_NONE;
    AVBufferRef* hw_device_ctx = nullptr;
};

// Callback for FFmpeg pixel format selection
enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts);

#endif // VMP_HW_DECODER_H
