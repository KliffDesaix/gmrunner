// video_* (one video at a time, as GameMaker): frames come from the VideoDecoder into a surface
// that video_draw returns (video_format_rgba); the audio track plays as a sound and sets the
// clock, so picture and sound stay together (a video without audio follows the game's steps).
// Starting and finishing send Async Social events with async_load.type "video_start" /
// "video_end", as YoYo's runner.
#include <cmath>

#include "runtime/Audio.h"
#include "runtime/Builtins.h"
#include "runtime/Graphics.h"
#include "runtime/Stores.h"
#include "runtime/Video.h"

namespace gm {
namespace {

enum Status { Closed = 0, Preparing = 1, Playing = 2, Paused = 3 };

struct Player {
    std::unique_ptr<VideoDecoder> dec;
    Status status = Closed;
    bool loop = false;
    double volume = 1, clock = 0;
    int sound = -1, voice = -1, surface = -1;
    std::vector<uint8_t> rgba;
    bool frameReady = false;
};
Player gVideo;

void stopAudio(Runner& rt) {
    if (gVideo.voice >= 0) rt.audio->stop(gVideo.voice);
    gVideo.voice = -1;
}
void startAudio(Runner& rt, double at) {
    stopAudio(rt);
    if (gVideo.sound < 0) return;
    Audio::PlayParams p;
    p.gain = gVideo.volume;
    p.offset = at;
    gVideo.voice = rt.audio->play(gVideo.sound, 100, false, p);
}
void closeVideo(Runner& rt) {
    stopAudio(rt);
    if (gVideo.sound >= 0) rt.audio->destroyStream(gVideo.sound);
    if (gVideo.surface >= 0 && rt.gfx) rt.gfx->surfaceFree(gVideo.surface);
    gVideo = Player();
}

// Once per step: advance the clock and finish (or loop) at the end.
void update(Runner& rt) {
    if (gVideo.status != Playing || !gVideo.dec) return;
    if (gVideo.voice >= 0 && rt.audio->isPlaying(gVideo.voice)) gVideo.clock = rt.audio->getPosition(gVideo.voice);
    else gVideo.clock += 1.0 / (rt.roomSpeed > 0 ? rt.roomSpeed : 60);
    double dur = gVideo.dec->duration();
    if (dur > 0 && gVideo.clock >= dur) {
        if (gVideo.loop) {
            gVideo.clock = 0;
            gVideo.dec->seek(0);
            startAudio(rt, 0);
        } else {
            gVideo.status = Closed;
            stopAudio(rt);
            rt.queueAsync(70, {{"type", Value("video_end")}});
        }
    }
}

}  // namespace

GMFN(video_open) {
    closeVideo(rt);
    gVideo.dec = createVideoDecoder();
    std::string err = "no video decoder in this build";
    if (!gVideo.dec || !gVideo.dec->open(rt.files->readPath(S(0)), err)) {
        rt.vm.warnOnce("video:" + S(0), "video_open " + S(0) + ": " + err);
        gVideo = Player();
        return Value();
    }
    std::vector<int16_t> pcm;
    int channels = 0;
    bool sound = gVideo.dec->audio(Audio::kRate, pcm, channels);
    rt.log("[video] %s: %dx%d, %.1f s, %s", S(0).c_str(), gVideo.dec->width(), gVideo.dec->height(),
           gVideo.dec->duration(), sound ? "with sound" : "no sound");
    if (sound) gVideo.sound = rt.audio->createPcmSound(std::move(pcm), channels, Audio::kRate);
    gVideo.status = Playing;
    startAudio(rt, 0);
    rt.queueAsync(70, {{"type", Value("video_start")}});
    return Value();
}
GMFN(video_close) { closeVideo(rt); return Value(); }
GMFN(video_draw) {  // [status (0 ok, -1 error), surface, -1]
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    auto& row = a->rows[0];
    if (gVideo.status == Closed || !gVideo.dec) {
        row = {Value(-1.0), Value(-1.0), Value(-1.0)};
        return arr;
    }
    int w = gVideo.dec->width(), h = gVideo.dec->height();
    if (gVideo.dec->frameAt(gVideo.clock, gVideo.rgba)) gVideo.frameReady = true;
    if (!rt.gfx->surfaceExists(gVideo.surface) || rt.gfx->surfaceWidth(gVideo.surface) != w ||
        rt.gfx->surfaceHeight(gVideo.surface) != h) {
        if (gVideo.surface >= 0) rt.gfx->surfaceFree(gVideo.surface);
        gVideo.surface = rt.gfx->surfaceCreate(w, h);
        gVideo.frameReady = !gVideo.rgba.empty();
    }
    if (gVideo.frameReady && gVideo.rgba.size() == static_cast<size_t>(w) * h * 4) {
        rt.gfx->writeSurface(gVideo.surface, gVideo.rgba.data(), w, h);
        gVideo.frameReady = false;
    }
    row = {Value(0.0), Value(static_cast<double>(gVideo.surface)), Value(-1.0)};
    return arr;
}
GMFN(video_get_format) { return Value(0.0); }  // video_format_rgba
GMFN(video_get_status) { return Value(static_cast<double>(gVideo.status)); }
GMFN(video_get_duration) { return Value(gVideo.dec ? gVideo.dec->duration() * 1000.0 : 0.0); }
GMFN(video_get_position) { return Value(gVideo.clock * 1000.0); }
GMFN(video_seek_to) {
    if (!gVideo.dec) return Value();
    gVideo.clock = std::max(0.0, D(0) / 1000.0);
    gVideo.dec->seek(gVideo.clock);
    gVideo.frameReady = false;
    if (gVideo.status == Playing) startAudio(rt, gVideo.clock);
    return Value();
}
GMFN(video_pause) {
    if (gVideo.status == Playing) {
        gVideo.status = Paused;
        if (gVideo.voice >= 0) rt.audio->pause(gVideo.voice, true);
    }
    return Value();
}
GMFN(video_resume) {
    if (gVideo.status == Paused) {
        gVideo.status = Playing;
        if (gVideo.voice >= 0) rt.audio->pause(gVideo.voice, false);
    }
    return Value();
}
GMFN(video_enable_loop) { gVideo.loop = B(0); return Value(); }
GMFN(video_is_looping) { return boolV(gVideo.loop); }
GMFN(video_set_volume) {
    gVideo.volume = std::max(0.0, D(0));
    if (gVideo.voice >= 0) rt.audio->setGain(gVideo.voice, gVideo.volume, 0);
    return Value();
}
GMFN(video_get_volume) { return Value(gVideo.volume); }

void registerVideoBuiltins(Runner& rt) {
    gVideo = Player();  // a new game (game_change) starts without a video
    rt.videoUpdate = [&rt] { update(rt); };
    GMREG(video_open); GMREG(video_close); GMREG(video_draw); GMREG(video_get_format); GMREG(video_get_status);
    GMREG(video_get_duration); GMREG(video_get_position); GMREG(video_seek_to); GMREG(video_pause);
    GMREG(video_resume); GMREG(video_enable_loop); GMREG(video_is_looping); GMREG(video_set_volume);
    GMREG(video_get_volume);
}

}  // namespace gm
