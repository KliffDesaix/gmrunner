// Video on iOS: AVFoundation's decoders, through the Swift reader in ios/VideoReader.swift.
#if defined(__APPLE__) && defined(GMR_MOBILE)

#include <algorithm>

#include "runtime/Video.h"

extern "C" {
void* gmr_av_open(const char* path, int32_t* width, int32_t* height, double* duration);
int32_t gmr_av_frame(void* handle, double seconds, uint8_t* rgba, int32_t width, int32_t height);
void gmr_av_seek(void* handle, double seconds);
int16_t* gmr_av_audio(void* handle, int32_t rate, size_t* count);
void gmr_av_free_audio(int16_t* samples);
void gmr_av_close(void* handle);
}

namespace gm {

namespace {

class AppleVideoDecoder : public VideoDecoder {
public:
    ~AppleVideoDecoder() override {
        if (h_) gmr_av_close(h_);
    }
    bool open(const std::string& path, std::string& error) override {
        int32_t w = 0, h = 0;
        h_ = gmr_av_open(path.c_str(), &w, &h, &duration_);
        if (!h_) {
            error = "not a video the system can read";
            return false;
        }
        w_ = w, h_px_ = h;
        return w_ > 0 && h_px_ > 0;
    }
    int width() const override { return w_; }
    int height() const override { return h_px_; }
    double duration() const override { return duration_; }
    bool frameAt(double t, std::vector<uint8_t>& rgba) override {
        rgba.resize(static_cast<size_t>(w_) * h_px_ * 4);
        return gmr_av_frame(h_, t, rgba.data(), w_, h_px_) != 0;
    }
    void seek(double t) override { gmr_av_seek(h_, t); }
    bool audio(int rate, std::vector<int16_t>& pcm, int& channels) override {
        size_t count = 0;
        int16_t* samples = gmr_av_audio(h_, rate, &count);
        if (!samples) return false;
        pcm.assign(samples, samples + count);
        gmr_av_free_audio(samples);
        channels = 2;
        return true;
    }

private:
    void* h_ = nullptr;
    int w_ = 0, h_px_ = 0;
    double duration_ = 0;
};

}  // namespace

std::unique_ptr<VideoDecoder> createVideoDecoder() { return std::make_unique<AppleVideoDecoder>(); }

}  // namespace gm

#endif
