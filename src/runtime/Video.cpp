#include "Video.h"

#ifdef GMR_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace gm {

#ifdef GMR_FFMPEG
namespace {

class FfmpegDecoder : public VideoDecoder {
public:
    ~FfmpegDecoder() override { close(); }

    bool open(const std::string& path, std::string& error) override {
        path_ = path;
        if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) return fail(error, "cannot open file");
        if (avformat_find_stream_info(fmt_, nullptr) < 0) return fail(error, "no stream info");
        stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream_ < 0) return fail(error, "no video stream");
        AVStream* st = fmt_->streams[stream_];
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) return fail(error, "no decoder for the video codec");
        ctx_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ctx_, st->codecpar);
        ctx_->thread_count = 0;  // as many as useful
        if (avcodec_open2(ctx_, codec, nullptr) < 0) return fail(error, "cannot open the video decoder");
        timeBase_ = av_q2d(st->time_base);
        if (fmt_->duration > 0) duration_ = fmt_->duration / static_cast<double>(AV_TIME_BASE);
        else if (st->duration > 0) duration_ = st->duration * timeBase_;
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        return true;
    }
    int width() const override { return ctx_ ? ctx_->width : 0; }
    int height() const override { return ctx_ ? ctx_->height : 0; }
    double duration() const override { return duration_; }

    bool frameAt(double t, std::vector<uint8_t>& rgba) override {
        bool changed = false;
        while (true) {
            if (!havePending_ && !decodeNext()) break;
            if (pendingTime_ > t && shown_) break;
            convert(rgba);
            havePending_ = false;
            shown_ = changed = true;
        }
        return changed;
    }

    void seek(double t) override {
        if (!fmt_) return;
        int64_t ts = static_cast<int64_t>(t / timeBase_);
        av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(ctx_);
        havePending_ = shown_ = eof_ = false;
        // Skip ahead to the frame showing at t.
        while (decodeNext()) {
            if (pendingTime_ + 1e-6 >= t) break;
            havePending_ = false;
        }
    }

    bool audio(int rate, std::vector<int16_t>& pcm, int& channels) override {
        AVFormatContext* fmt = nullptr;
        if (avformat_open_input(&fmt, path_.c_str(), nullptr, nullptr) < 0) return false;
        bool ok = false;
        AVCodecContext* ctx = nullptr;
        SwrContext* swr = nullptr;
        AVFrame* frame = av_frame_alloc();
        AVPacket* pkt = av_packet_alloc();
        int idx = -1;
        const AVCodec* codec = nullptr;
        if (avformat_find_stream_info(fmt, nullptr) >= 0 &&
            (idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0)) >= 0 && codec) {
            ctx = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(ctx, fmt->streams[idx]->codecpar);
            if (avcodec_open2(ctx, codec, nullptr) >= 0) {
                AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, rate, &ctx->ch_layout, ctx->sample_fmt,
                                    ctx->sample_rate, 0, nullptr);
                if (swr && swr_init(swr) >= 0) {
                    channels = 2;
                    auto drain = [&] {
                        while (avcodec_receive_frame(ctx, frame) == 0) {
                            int out = swr_get_out_samples(swr, frame->nb_samples);
                            size_t at = pcm.size();
                            pcm.resize(at + static_cast<size_t>(out) * 2);
                            uint8_t* dst = reinterpret_cast<uint8_t*>(pcm.data() + at);
                            int got = swr_convert(swr, &dst, out, const_cast<const uint8_t**>(frame->extended_data),
                                                  frame->nb_samples);
                            pcm.resize(at + static_cast<size_t>(std::max(0, got)) * 2);
                        }
                    };
                    while (av_read_frame(fmt, pkt) >= 0) {
                        if (pkt->stream_index == idx && avcodec_send_packet(ctx, pkt) >= 0) drain();
                        av_packet_unref(pkt);
                    }
                    avcodec_send_packet(ctx, nullptr);
                    drain();
                    ok = !pcm.empty();
                }
            }
        }
        swr_free(&swr);
        avcodec_free_context(&ctx);
        av_packet_free(&pkt);
        av_frame_free(&frame);
        avformat_close_input(&fmt);
        return ok;
    }

private:
    bool fail(std::string& error, const char* what) {
        error = what;
        close();
        return false;
    }
    void close() {
        sws_freeContext(sws_);
        sws_ = nullptr;
        av_frame_free(&frame_);
        av_packet_free(&packet_);
        avcodec_free_context(&ctx_);
        avformat_close_input(&fmt_);
    }
    // Reads packets until the decoder gives the next frame (kept in frame_ as the pending one).
    bool decodeNext() {
        while (true) {
            int r = avcodec_receive_frame(ctx_, frame_);
            if (r == 0) {
                int64_t pts = frame_->best_effort_timestamp;
                pendingTime_ = pts == AV_NOPTS_VALUE ? pendingTime_ : pts * timeBase_;
                havePending_ = true;
                return true;
            }
            if (r == AVERROR_EOF || eof_) return false;
            if (av_read_frame(fmt_, packet_) < 0) {
                eof_ = true;
                avcodec_send_packet(ctx_, nullptr);  // drain the frames still buffered
                continue;
            }
            if (packet_->stream_index == stream_) avcodec_send_packet(ctx_, packet_);
            av_packet_unref(packet_);
        }
    }
    void convert(std::vector<uint8_t>& rgba) {
        int w = ctx_->width, h = ctx_->height;
        sws_ = sws_getCachedContext(sws_, w, h, static_cast<AVPixelFormat>(frame_->format), w, h, AV_PIX_FMT_RGBA,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
        rgba.resize(static_cast<size_t>(w) * h * 4);
        uint8_t* dst[1] = {rgba.data()};
        int stride[1] = {w * 4};
        sws_scale(sws_, frame_->data, frame_->linesize, 0, h, dst, stride);
    }

    std::string path_;
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ = nullptr;
    int stream_ = -1;
    double timeBase_ = 0, duration_ = 0, pendingTime_ = 0;
    bool havePending_ = false, shown_ = false, eof_ = false;
};

}  // namespace

std::unique_ptr<VideoDecoder> createVideoDecoder() { return std::make_unique<FfmpegDecoder>(); }
#elif !defined(GMR_PLATFORM_VIDEO)
std::unique_ptr<VideoDecoder> createVideoDecoder() { return nullptr; }
#endif  // GMR_PLATFORM_VIDEO: the platform's decoders (src/platform/video/) define it

}  // namespace gm
