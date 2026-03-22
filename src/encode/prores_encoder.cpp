/**
 * prores_encoder.cpp - ProRes encoding via macOS VideoToolbox
 *
 * On macOS, we use the native VideoToolbox framework which provides
 * hardware-accelerated ProRes encoding on Apple Silicon and
 * software ProRes encoding on Intel Macs.
 *
 * This avoids having to implement the ProRes bitstream format manually.
 */

#include "prores_encoder.h"

#ifdef __APPLE__
#include <VideoToolbox/VideoToolbox.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#endif

#include <iostream>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <condition_variable>

namespace dcpconv {

struct ProResEncoder::Impl {
#ifdef __APPLE__
    VTCompressionSessionRef session = nullptr;
    CMFormatDescriptionRef format = nullptr;

    int width = 0;
    int height = 0;
    int fps_num = 24;
    int fps_den = 1;
    int64_t pts = 0;
    std::string profile;

    // Output buffer (filled by callback)
    std::vector<uint8_t> output_buffer;
    std::mutex mtx;
    std::condition_variable cv;
    bool frame_ready = false;

    // Map profile string to VideoToolbox codec type
    CMVideoCodecType get_codec_type() const {
        if (profile == "proxy")   return kCMVideoCodecType_AppleProRes422Proxy;
        if (profile == "lt")      return kCMVideoCodecType_AppleProRes422LT;
        if (profile == "standard") return kCMVideoCodecType_AppleProRes422;
        if (profile == "hq")      return kCMVideoCodecType_AppleProRes422HQ;
        if (profile == "4444")    return kCMVideoCodecType_AppleProRes4444;
        return kCMVideoCodecType_AppleProRes422HQ; // default
    }
#endif
};

ProResEncoder::ProResEncoder() : impl_(std::make_unique<Impl>()) {}

ProResEncoder::~ProResEncoder() {
#ifdef __APPLE__
    if (impl_->session) {
        VTCompressionSessionInvalidate(impl_->session);
        CFRelease(impl_->session);
    }
#endif
}

#ifdef __APPLE__
// VideoToolbox output callback
static void vt_output_callback(void* refcon,
                                void* source_ref,
                                OSStatus status,
                                VTEncodeInfoFlags flags,
                                CMSampleBufferRef sample_buffer) {
    auto* impl = static_cast<ProResEncoder::Impl*>(refcon);

    if (status != noErr || !sample_buffer) {
        std::cerr << "VideoToolbox encode error: " << status << "\n";
        std::lock_guard<std::mutex> lock(impl->mtx);
        impl->frame_ready = true;
        impl->cv.notify_one();
        return;
    }

    // Get the data
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample_buffer);
    size_t length = 0;
    char* data = nullptr;
    CMBlockBufferGetDataPointer(block, 0, nullptr, &length, &data);

    {
        std::lock_guard<std::mutex> lock(impl->mtx);
        impl->output_buffer.assign(data, data + length);
        impl->frame_ready = true;
    }
    impl->cv.notify_one();
}
#endif

void ProResEncoder::init(const Config& cfg) {
#ifdef __APPLE__
    impl_->width = cfg.width;
    impl_->height = cfg.height;
    impl_->fps_num = cfg.fps_num;
    impl_->fps_den = cfg.fps_den;
    impl_->profile = cfg.profile;

    // Create compression session
    CFMutableDictionaryRef encoder_spec = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    // Prefer software encoding for compatibility
    CFDictionarySetValue(encoder_spec,
        kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
        kCFBooleanTrue);

    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        cfg.width, cfg.height,
        impl_->get_codec_type(),
        encoder_spec,
        nullptr,  // source pixel buffer attributes
        kCFAllocatorDefault,
        vt_output_callback,
        impl_.get(),
        &impl_->session);

    CFRelease(encoder_spec);

    if (status != noErr) {
        throw std::runtime_error("Failed to create VideoToolbox ProRes session: "
                                 + std::to_string(status));
    }

    // Set properties
    VTSessionSetProperty(impl_->session,
        kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    VTSessionSetProperty(impl_->session,
        kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

    // Frame rate
    int64_t fps_value = cfg.fps_num / cfg.fps_den;
    CFNumberRef fps_ref = CFNumberCreate(nullptr, kCFNumberSInt64Type, &fps_value);
    VTSessionSetProperty(impl_->session,
        kVTCompressionPropertyKey_ExpectedFrameRate, fps_ref);
    CFRelease(fps_ref);

    VTCompressionSessionPrepareToEncodeFrames(impl_->session);

    std::cout << "ProRes encoder initialized (" << cfg.profile
              << ", VideoToolbox)\n";
#else
    throw std::runtime_error("ProRes encoding requires macOS (VideoToolbox)");
#endif
}

bool ProResEncoder::encode_frame(const uint8_t* rgb_data,
                                  std::vector<uint8_t>& out) {
    out.clear();

#ifdef __APPLE__
    // Create CVPixelBuffer from RGB data
    CVPixelBufferRef pixel_buffer = nullptr;

    // ProRes works with 422 YCbCr, but VideoToolbox can accept RGB
    // and convert internally. Use kCVPixelFormatType_24RGB.
    CVReturn cv_ret = CVPixelBufferCreate(
        kCFAllocatorDefault,
        impl_->width, impl_->height,
        kCVPixelFormatType_24RGB,
        nullptr,
        &pixel_buffer);

    if (cv_ret != kCVReturnSuccess) {
        std::cerr << "Failed to create pixel buffer\n";
        return false;
    }

    CVPixelBufferLockBaseAddress(pixel_buffer, 0);

    uint8_t* dst = (uint8_t*)CVPixelBufferGetBaseAddress(pixel_buffer);
    size_t dst_stride = CVPixelBufferGetBytesPerRow(pixel_buffer);
    size_t src_stride = impl_->width * 3;

    for (int y = 0; y < impl_->height; ++y) {
        memcpy(dst + y * dst_stride,
               rgb_data + y * src_stride,
               src_stride);
    }

    CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);

    // Create timestamp
    CMTime pts = CMTimeMake(impl_->pts, impl_->fps_num);
    CMTime duration = CMTimeMake(impl_->fps_den, impl_->fps_num);

    // Reset output state
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->frame_ready = false;
        impl_->output_buffer.clear();
    }

    // Encode
    OSStatus status = VTCompressionSessionEncodeFrame(
        impl_->session,
        pixel_buffer,
        pts, duration,
        nullptr,  // frame properties
        nullptr,  // source frame refcon
        nullptr); // info flags out

    CVPixelBufferRelease(pixel_buffer);

    if (status != noErr) {
        std::cerr << "VTCompressionSessionEncodeFrame error: " << status << "\n";
        return false;
    }

    // Wait for callback
    {
        std::unique_lock<std::mutex> lock(impl_->mtx);
        impl_->cv.wait(lock, [this]{ return impl_->frame_ready; });
        out = impl_->output_buffer;
    }

    impl_->pts += impl_->fps_den;
    return true;
#else
    return false;
#endif
}

bool ProResEncoder::flush(std::vector<uint8_t>& out) {
    out.clear();

#ifdef __APPLE__
    if (impl_->session) {
        VTCompressionSessionCompleteFrames(impl_->session, kCMTimeInvalid);
    }
#endif

    return true;
}

} // namespace dcpconv
