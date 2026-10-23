// Video decoding for video_open / video_draw: FFmpeg when the libraries are found (GMR_FFMPEG:
// desktop, Switch, PortMaster), the system's decoders on phones (GMR_PLATFORM_VIDEO: Android
// MediaCodec, iOS AVFoundation, in src/platform/video/). Without either createVideoDecoder
// returns nullptr and videos fail to open.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gm {

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    virtual bool open(const std::string& path, std::string& error) = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual double duration() const = 0;  // seconds
    // Decodes up to time t (seconds); when the frame showing at t changed, writes it to rgba
    // (width * height * 4 bytes) and returns true.
    virtual bool frameAt(double t, std::vector<uint8_t>& rgba) = 0;
    virtual void seek(double t) = 0;
    // The whole audio track as interleaved 16-bit samples (false if the video has none).
    virtual bool audio(int rate, std::vector<int16_t>& pcm, int& channels) = 0;
};

std::unique_ptr<VideoDecoder> createVideoDecoder();

}  // namespace gm
