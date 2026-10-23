// Video on Android: the system's own decoders (MediaCodec, through the NDK), hardware decoders on
// nearly every phone. Frames come out as YUV 4:2:0, planar or semi-planar, and are turned into
// RGBA here; the audio track is decoded whole and resampled for the mixer, as the FFmpeg
// backend does on desktop and Switch.
#ifdef __ANDROID__

#include <fcntl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "runtime/Video.h"

namespace gm {

namespace {

constexpr int32_t kColorI420 = 19;  // COLOR_FormatYUV420Planar; anything else is treated as NV12

// One track of a file (the first video or audio one) and a started decoder for it.
struct Track {
    int fd = -1;
    AMediaExtractor* ex = nullptr;
    AMediaCodec* codec = nullptr;
    AMediaFormat* format = nullptr;
    bool inputDone = false;

    bool open(const std::string& path, const char* kind, std::string& error) {
        fd = ::open(path.c_str(), O_RDONLY);
        struct stat st {};
        if (fd < 0 || fstat(fd, &st) != 0) {
            error = "cannot open " + path;
            return false;
        }
        ex = AMediaExtractor_new();
        if (AMediaExtractor_setDataSourceFd(ex, fd, 0, st.st_size) != AMEDIA_OK) {
            error = "not a file the system's decoders can read";
            return false;
        }
        for (size_t i = 0; i < AMediaExtractor_getTrackCount(ex); i++) {
            AMediaFormat* f = AMediaExtractor_getTrackFormat(ex, i);
            const char* m = nullptr;
            AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m);
            std::string mime = m ? m : "";
            if (mime.rfind(kind, 0) != 0) {
                AMediaFormat_delete(f);
                continue;
            }
            AMediaExtractor_selectTrack(ex, i);
            codec = AMediaCodec_createDecoderByType(mime.c_str());
            if (!codec || AMediaCodec_configure(codec, f, nullptr, nullptr, 0) != AMEDIA_OK ||
                AMediaCodec_start(codec) != AMEDIA_OK) {
                error = "no decoder for " + mime + " on this device";
                AMediaFormat_delete(f);
                return false;
            }
            format = f;
            return true;
        }
        error = std::string("no ") + kind + " track";
        return false;
    }

    // Gives the decoder the next sample when it has room for one.
    void feed() {
        if (inputDone) return;
        ssize_t in = AMediaCodec_dequeueInputBuffer(codec, 0);
        if (in < 0) return;
        size_t capacity = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(codec, static_cast<size_t>(in), &capacity);
        ssize_t size = buf ? AMediaExtractor_readSampleData(ex, buf, capacity) : -1;
        if (size < 0) {
            AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(in), 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            inputDone = true;
            return;
        }
        AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(in), 0, static_cast<size_t>(size),
                                     static_cast<uint64_t>(AMediaExtractor_getSampleTime(ex)), 0);
        AMediaExtractor_advance(ex);
    }

    ~Track() {
        if (codec) {
            AMediaCodec_stop(codec);
            AMediaCodec_delete(codec);
        }
        if (format) AMediaFormat_delete(format);
        if (ex) AMediaExtractor_delete(ex);
        if (fd >= 0) ::close(fd);
    }
};

class AndroidVideoDecoder : public VideoDecoder {
public:
    bool open(const std::string& path, std::string& error) override {
        path_ = path;
        if (!v_.open(path, "video/", error)) return false;
        int32_t w = 0, h = 0;
        int64_t us = 0;
        AMediaFormat_getInt32(v_.format, AMEDIAFORMAT_KEY_WIDTH, &w);
        AMediaFormat_getInt32(v_.format, AMEDIAFORMAT_KEY_HEIGHT, &h);
        AMediaFormat_getInt64(v_.format, AMEDIAFORMAT_KEY_DURATION, &us);
        w_ = w, h_ = h, stride_ = w, sliceH_ = h;
        duration_ = us / 1e6;
        if (w_ <= 0 || h_ <= 0) error = "the video has no picture size";
        return w_ > 0 && h_ > 0;
    }
    int width() const override { return w_; }
    int height() const override { return h_; }
    double duration() const override { return duration_; }

    bool frameAt(double t, std::vector<uint8_t>& rgba) override {
        int64_t target = static_cast<int64_t>(t * 1e6);
        bool changed = false;
        for (int spins = 0; spins < 400; spins++) {
            if (pending_ >= 0) {  // a decoded frame waiting for its time
                if (pendingPts_ > target) break;
                size_t size = 0;
                uint8_t* data = AMediaCodec_getOutputBuffer(v_.codec, static_cast<size_t>(pending_), &size);
                if (data && pendingOffset_ + static_cast<size_t>(stride_) * sliceH_ <= size) {
                    convert(data + pendingOffset_, rgba);
                    changed = true;
                }
                AMediaCodec_releaseOutputBuffer(v_.codec, static_cast<size_t>(pending_), false);
                pending_ = -1;
                continue;
            }
            if (outputDone_) break;
            v_.feed();
            AMediaCodecBufferInfo info{};
            ssize_t out = AMediaCodec_dequeueOutputBuffer(v_.codec, &info, v_.inputDone ? 5000 : 1000);
            if (out >= 0) {
                if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) outputDone_ = true;
                if (info.size > 0) {
                    pending_ = out;
                    pendingPts_ = info.presentationTimeUs;
                    pendingOffset_ = static_cast<size_t>(info.offset);
                } else {
                    AMediaCodec_releaseOutputBuffer(v_.codec, static_cast<size_t>(out), false);
                }
            } else if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                readOutputFormat();
            }
        }
        return changed;
    }

    void seek(double t) override {
        if (pending_ >= 0) AMediaCodec_releaseOutputBuffer(v_.codec, static_cast<size_t>(pending_), false);
        pending_ = -1;
        AMediaExtractor_seekTo(v_.ex, static_cast<int64_t>(t * 1e6), AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
        AMediaCodec_flush(v_.codec);
        v_.inputDone = outputDone_ = false;
    }

    bool audio(int rate, std::vector<int16_t>& pcm, int& channels) override {
        Track a;
        std::string error;
        if (!a.open(path_, "audio/", error)) return false;
        int32_t srcRate = 44100, srcChannels = 2, encoding = 2;  // 2: 16-bit PCM, 4: float
        AMediaFormat_getInt32(a.format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &srcRate);
        AMediaFormat_getInt32(a.format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &srcChannels);
        std::vector<float> raw;
        bool done = false;
        for (int idle = 0; !done && idle < 500;) {
            a.feed();
            AMediaCodecBufferInfo info{};
            ssize_t out = AMediaCodec_dequeueOutputBuffer(a.codec, &info, 2000);
            if (out >= 0) {
                idle = 0;
                size_t size = 0;
                uint8_t* data = AMediaCodec_getOutputBuffer(a.codec, static_cast<size_t>(out), &size);
                if (data && info.size > 0) {
                    const uint8_t* p = data + info.offset;
                    if (encoding == 4) {
                        const auto* f = reinterpret_cast<const float*>(p);
                        raw.insert(raw.end(), f, f + info.size / 4);
                    } else {
                        const auto* s = reinterpret_cast<const int16_t*>(p);
                        for (int32_t i = 0; i < info.size / 2; i++) raw.push_back(s[i] / 32768.f);
                    }
                }
                if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) done = true;
                AMediaCodec_releaseOutputBuffer(a.codec, static_cast<size_t>(out), false);
            } else if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat* f = AMediaCodec_getOutputFormat(a.codec);
                AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &srcRate);
                AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &srcChannels);
                AMediaFormat_getInt32(f, "pcm-encoding", &encoding);
                AMediaFormat_delete(f);
            } else {
                idle++;
            }
        }
        if (raw.empty() || srcChannels <= 0 || srcRate <= 0) return false;
        // Stereo at the mixer's rate (linear interpolation).
        size_t frames = raw.size() / static_cast<size_t>(srcChannels);
        size_t outFrames = static_cast<size_t>(static_cast<double>(frames) * rate / srcRate);
        pcm.resize(outFrames * 2);
        for (size_t i = 0; i < outFrames; i++) {
            double pos = static_cast<double>(i) * srcRate / rate;
            size_t i0 = std::min(frames - 1, static_cast<size_t>(pos));
            size_t i1 = std::min(frames - 1, i0 + 1);
            double f = pos - static_cast<double>(i0);
            for (int c = 0; c < 2; c++) {
                int sc = std::min(c, srcChannels - 1);
                double v = raw[i0 * srcChannels + sc] * (1 - f) + raw[i1 * srcChannels + sc] * f;
                pcm[i * 2 + c] = static_cast<int16_t>(std::clamp(v, -1.0, 1.0) * 32767);
            }
        }
        channels = 2;
        return true;
    }

    ~AndroidVideoDecoder() override {
        if (pending_ >= 0 && v_.codec) AMediaCodec_releaseOutputBuffer(v_.codec, static_cast<size_t>(pending_), false);
    }

private:
    void readOutputFormat() {
        AMediaFormat* f = AMediaCodec_getOutputFormat(v_.codec);
        int32_t v = 0;
        if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v)) color_ = v;
        int32_t w = w_, h = h_;
        AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &w);
        AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &h);
        stride_ = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &v) && v > 0 ? v : w;
        sliceH_ = AMediaFormat_getInt32(f, "slice-height", &v) && v > 0 ? v : h;
        int32_t left = 0, top = 0, right = 0, bottom = 0;
        if (AMediaFormat_getInt32(f, "crop-left", &left) && AMediaFormat_getInt32(f, "crop-top", &top) &&
            AMediaFormat_getInt32(f, "crop-right", &right) && AMediaFormat_getInt32(f, "crop-bottom", &bottom)) {
            cropX_ = left, cropY_ = top;
        }
        AMediaFormat_delete(f);
    }

    // YUV 4:2:0 to RGBA (BT.601, video range: what FFmpeg's scaler does by default).
    void convert(const uint8_t* data, std::vector<uint8_t>& rgba) {
        rgba.resize(static_cast<size_t>(w_) * h_ * 4);
        const uint8_t* yPlane = data;
        const uint8_t* uvPlane = data + static_cast<size_t>(stride_) * sliceH_;
        bool planar = color_ == kColorI420;
        size_t quarter = static_cast<size_t>(stride_ / 2) * (sliceH_ / 2);
        for (int y = 0; y < h_; y++) {
            int sy = y + cropY_;
            uint8_t* dst = &rgba[static_cast<size_t>(y) * w_ * 4];
            for (int x = 0; x < w_; x++) {
                int sx = x + cropX_;
                int Y = yPlane[static_cast<size_t>(sy) * stride_ + sx], U, V;
                if (planar) {
                    size_t i = static_cast<size_t>(sy / 2) * (stride_ / 2) + sx / 2;
                    U = uvPlane[i], V = uvPlane[quarter + i];
                } else {
                    size_t i = static_cast<size_t>(sy / 2) * stride_ + (sx & ~1);
                    U = uvPlane[i], V = uvPlane[i + 1];
                }
                int c = 298 * (Y - 16), d = U - 128, e = V - 128;
                dst[x * 4 + 0] = static_cast<uint8_t>(std::clamp((c + 409 * e + 128) >> 8, 0, 255));
                dst[x * 4 + 1] = static_cast<uint8_t>(std::clamp((c - 100 * d - 208 * e + 128) >> 8, 0, 255));
                dst[x * 4 + 2] = static_cast<uint8_t>(std::clamp((c + 516 * d + 128) >> 8, 0, 255));
                dst[x * 4 + 3] = 255;
            }
        }
    }

    std::string path_;
    Track v_;
    int w_ = 0, h_ = 0, stride_ = 0, sliceH_ = 0, cropX_ = 0, cropY_ = 0;
    int32_t color_ = kColorI420;
    double duration_ = 0;
    ssize_t pending_ = -1;
    int64_t pendingPts_ = 0;
    size_t pendingOffset_ = 0;
    bool outputDone_ = false;
};

}  // namespace

std::unique_ptr<VideoDecoder> createVideoDecoder() { return std::make_unique<AndroidVideoDecoder>(); }

}  // namespace gm

#endif
