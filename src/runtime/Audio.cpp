#include "Audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "Runner.h"
#include "util/Fs.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
#undef L
#undef C
#undef R

namespace gm {

// ---------------------------------------------------------------------------------------------
// Sources: sequential readers producing interleaved float frames.

struct Audio::Source {
    virtual ~Source() = default;
    int channels = 1, rate = Audio::kRate;
    int64_t length = 0;  // frames, 0 if unknown
    virtual int read(float* out, int frames) = 0;
    virtual void seek(int64_t frame) = 0;
};

namespace {

struct PcmSource : Audio::Source {
    std::shared_ptr<std::vector<int16_t>> owned;  // keeps converted samples alive
    const uint8_t* bytes = nullptr;               // little-endian int16 samples, possibly unaligned
    size_t samples = 0;
    int64_t pos = 0;
    int read(float* out, int frames) override {
        int64_t total = static_cast<int64_t>(samples) / channels;
        int n = static_cast<int>(std::min<int64_t>(frames, total - pos));
        if (n <= 0) return 0;
        const uint8_t* src = bytes + pos * channels * 2;
        for (int i = 0; i < n * channels; i++) {
            int16_t v;
            std::memcpy(&v, src + i * 2, 2);
            out[i] = v / 32768.f;
        }
        pos += n;
        return n;
    }
    void seek(int64_t f) override { pos = std::max<int64_t>(0, std::min<int64_t>(f, length)); }
};

struct VorbisSource : Audio::Source {
    std::shared_ptr<std::vector<uint8_t>> data;
    stb_vorbis* v = nullptr;
    ~VorbisSource() override {
        if (v) stb_vorbis_close(v);
    }
    bool open() {
        int err = 0;
        v = stb_vorbis_open_memory(data->data(), static_cast<int>(data->size()), &err, nullptr);
        if (!v) return false;
        stb_vorbis_info info = stb_vorbis_get_info(v);
        channels = info.channels;
        rate = static_cast<int>(info.sample_rate);
        length = stb_vorbis_stream_length_in_samples(v);
        return true;
    }
    int read(float* out, int frames) override {
        return stb_vorbis_get_samples_float_interleaved(v, channels, out, frames * channels);
    }
    void seek(int64_t f) override { stb_vorbis_seek(v, static_cast<unsigned>(std::max<int64_t>(0, f))); }
};

// Parses a WAV file. 16-bit PCM is returned in place (pcmBytes/pcmSamples); other formats are
// converted into `pcm`.
bool parseWav(const uint8_t* d, size_t n, std::vector<int16_t>& pcm, const uint8_t*& pcmBytes, size_t& pcmSamples,
              int& channels, int& rate) {
    if (n < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) return false;
    int format = 0, bits = 0;
    channels = 0;
    for (size_t p = 12; p + 8 <= n;) {
        uint32_t len;
        std::memcpy(&len, d + p + 4, 4);
        const uint8_t* body = d + p + 8;
        if (p + 8 + len > n) len = static_cast<uint32_t>(n - p - 8);
        if (std::memcmp(d + p, "fmt ", 4) == 0 && len >= 16) {
            uint16_t f, ch, bps;
            uint32_t sr;
            std::memcpy(&f, body, 2);
            std::memcpy(&ch, body + 2, 2);
            std::memcpy(&sr, body + 4, 4);
            std::memcpy(&bps, body + 14, 2);
            format = f;
            channels = ch;
            rate = static_cast<int>(sr);
            bits = bps;
        } else if (std::memcmp(d + p, "data", 4) == 0 && channels > 0) {
            if (format == 1 && bits == 16) {
                pcmBytes = body;
                pcmSamples = len / 2;
            } else if (format == 1 && bits == 8) {
                pcm.resize(len);
                for (uint32_t i = 0; i < len; i++) pcm[i] = static_cast<int16_t>((body[i] - 128) << 8);
            } else if (format == 3 && bits == 32) {
                pcm.resize(len / 4);
                for (size_t i = 0; i < pcm.size(); i++) {
                    float s;
                    std::memcpy(&s, body + i * 4, 4);
                    pcm[i] = static_cast<int16_t>(std::max(-1.f, std::min(1.f, s)) * 32767);
                }
            } else {
                return false;
            }
            return true;
        }
        p += 8 + len + (len & 1);
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Voices

struct Audio::Voice {
    int id = 0;
    int sound = -1;
    bool loop = false, paused = false, finished = false;
    double priority = 0;
    double gain = 1, gainTarget = 1, gainStep = 0;
    double pitch = 1;
    // Positional sound: an emitter, or a position with its own falloff (audio_play_sound_at).
    int emitter = -1;
    bool positional = false;
    double px = 0, py = 0, pz = 0, ref = 0, max = 1, factor = 1;
    // Per-step results: stereo gains (falloff, panning, group, bus, emitter) and pitch.
    double mixL = 1, mixR = 1, mixPitch = 1;
    int64_t loopStartFrames = 0, loopEndFrames = 0;  // loop points in source frames (0 = whole sound)
    std::unique_ptr<Source> src;
    // Sliding window of decoded source frames (always stereo).
    std::vector<float> win;
    double pos = 0;          // position within win, in frames
    int64_t consumed = 0;    // source frames dropped from the front of win
    bool srcEnded = false;
    std::vector<float> tmp;

    bool fill(size_t needFrames) {
        while (win.size() / 2 < needFrames) {
            if (srcEnded) return false;
            const int chunk = 2048;
            tmp.resize(static_cast<size_t>(chunk) * src->channels);
            int n = src->read(tmp.data(), chunk);
            if (n <= 0) {
                if (loop && src->length != 0) {
                    src->seek(loopStartFrames);
                    consumed = loopStartFrames - static_cast<int64_t>(win.size() / 2);
                    n = src->read(tmp.data(), chunk);
                }
                if (n <= 0) {
                    srcEnded = true;
                    return false;
                }
            }
            size_t old = win.size();
            win.resize(old + static_cast<size_t>(n) * 2);
            for (int i = 0; i < n; i++) {
                float l = tmp[static_cast<size_t>(i) * src->channels];
                float r = src->channels > 1 ? tmp[static_cast<size_t>(i) * src->channels + 1] : l;
                win[old + i * 2] = l;
                win[old + i * 2 + 1] = r;
            }
        }
        return true;
    }

    void render(float* out, int frames, double gainL, double gainR) {
        double step = static_cast<double>(src->rate) / Audio::kRate * pitch;
        for (int i = 0; i < frames; i++) {
            // A loop end inside the sound (audio_sound_loop_end) jumps back to the loop start.
            if (loop && loopEndFrames > loopStartFrames && consumed + static_cast<int64_t>(pos) >= loopEndFrames)
                seekFrames(loopStartFrames);
            size_t i0 = static_cast<size_t>(pos);
            if (!fill(i0 + 2)) {
                if (win.size() / 2 <= i0) {
                    finished = true;
                    return;
                }
            }
            float t = static_cast<float>(pos - i0);
            size_t i1 = std::min(i0 + 1, win.size() / 2 - 1);
            float l = win[i0 * 2] + (win[i1 * 2] - win[i0 * 2]) * t;
            float r = win[i0 * 2 + 1] + (win[i1 * 2 + 1] - win[i0 * 2 + 1]) * t;
            out[i * 2] += l * static_cast<float>(gainL);
            out[i * 2 + 1] += r * static_cast<float>(gainR);
            pos += step;
        }
        // Drop consumed frames from the front of the window.
        size_t drop = static_cast<size_t>(pos);
        if (drop > 4096 && drop <= win.size() / 2) {
            win.erase(win.begin(), win.begin() + drop * 2);
            pos -= drop;
            consumed += drop;
        }
    }

    double positionFrames() const {
        double p = consumed + pos;
        if (src->length > 0) {
            p = std::fmod(p, static_cast<double>(src->length));
            if (p < 0) p += static_cast<double>(src->length);  // the tail before a loop wrapped
        }
        return std::max(0.0, p);
    }
    void seekFrames(int64_t f) {
        src->seek(f);
        win.clear();
        pos = 0;
        consumed = f;
        srcEnded = false;
    }
};

Audio::Audio(Runner& rt) : rt_(rt) {
    assets_.reserve(rt.dw.sounds.size() + 4096);  // streams are appended without moving the others
    assets_.resize(rt.dw.sounds.size());
}
Audio::~Audio() {
    std::lock_guard<std::mutex> lock(mu_);
    voices_.clear();
}

Audio::Asset* Audio::asset(int sound) {
    if (sound < 0 || static_cast<size_t>(sound) >= assets_.size()) return nullptr;
    Asset& a = assets_[sound];
    if (static_cast<size_t>(sound) >= rt_.dw.sounds.size()) return &a;  // audio_create_stream
    const Sound& s = rt_.dw.sounds[sound];
    if (!a.configured) {
        a.configured = true;
        a.gain = a.gainTarget = s.volume > 0 ? s.volume : 1;
        a.pitch = s.pitch > 0 ? s.pitch : 1;
    }
    if (a.tried) return &a;
    a.tried = true;
    // Streamed sounds (music) keep a stale AUDO index in data.win; only trust it when the sound
    // is flagged as embedded (0x1) or compressed (0x2). Otherwise play the external file.
    bool embedded = (s.flags & 0x3) != 0;
    // GMS2 audio groups other than the default live in audiogroupN.dat.
    std::shared_ptr<std::vector<uint8_t>> grouped = embedded && s.group > 0 ? groupBlob(s.group, s.audioFile) : nullptr;
    if (grouped || (embedded && s.group <= 0 && s.audioFile >= 0 &&
                    static_cast<size_t>(s.audioFile) < rt_.dw.audio.size())) {
        AudioBlobRef b = grouped ? AudioBlobRef{grouped->data(), grouped->size()}
                                 : AudioBlobRef{rt_.dw.audio[s.audioFile].data, rt_.dw.audio[s.audioFile].size};
        auto pcm = std::make_shared<std::vector<int16_t>>();
        if (parseWav(b.data, b.size, *pcm, a.pcmBytes, a.pcmSamples, a.channels, a.rate)) {
            if (!a.pcmBytes) {
                a.pcm = pcm;
                a.pcmBytes = reinterpret_cast<const uint8_t*>(pcm->data());
                a.pcmSamples = pcm->size();
            } else if (grouped) {
                a.blob = grouped;  // the samples are used in place
            }
        } else if (b.size > 4 && std::memcmp(b.data, "OggS", 4) == 0) {
            a.file = grouped ? grouped : std::make_shared<std::vector<uint8_t>>(b.data, b.data + b.size);
            a.streamed = true;
        } else {
            rt_.log("[audio] unsupported embedded format for %.*s", int(s.name.size()), s.name.data());
        }
    } else {
        std::string path = rt_.gameDir + "/" + std::string(s.file);
        auto data = std::make_shared<std::vector<uint8_t>>();
        if (readFile(path, *data)) {
            a.file = std::move(data);
            a.streamed = true;
        } else {
            rt_.log("[audio] missing sound file %s", path.c_str());
        }
    }
    return &a;
}

// GMS2 keeps non-default audio groups in audiogroupN.dat next to data.win (a FORM file with
// an AUDO chunk). The first use reads the chunk list and the AUDO entry table; each sound's
// bytes are then read on their own (games ship groups of well over 100 MB).
std::shared_ptr<std::vector<uint8_t>> Audio::groupBlob(int group, int index) {
    std::string path = rt_.gameDir + "/audiogroup" + std::to_string(group) + ".dat";
    auto it = groupIndex_.find(group);
    if (it == groupIndex_.end()) {
        auto& entries = groupIndex_[group];
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) {
            rt_.log("[audio] missing %s", path.c_str());
        } else {
            auto u32At = [&](uint64_t o, uint32_t& v) {
                return fseeko(f, static_cast<off_t>(o), SEEK_SET) == 0 && fread(&v, 4, 1, f) == 1;
            };
            uint32_t formLen = 0, len = 0, tag = 0, n = 0;
            uint64_t end = u32At(4, formLen) ? 8 + static_cast<uint64_t>(formLen) : 0;
            for (uint64_t p = 8; p + 8 <= end && u32At(p, tag) && u32At(p + 4, len); p += 8 + static_cast<uint64_t>(len)) {
                if (std::memcmp(&tag, "AUDO", 4) != 0 || !u32At(p + 8, n) || n >= 1000000) continue;
                std::vector<uint32_t> table(n);
                if (n && fread(table.data(), 4, n, f) != n) break;
                for (uint32_t e : table) {
                    uint32_t size = 0;
                    if (u32At(e, size)) entries.push_back({e + 4ull, size});
                }
            }
            fclose(f);
        }
        it = groupIndex_.find(group);
    }
    if (index < 0 || static_cast<size_t>(index) >= it->second.size()) return nullptr;
    auto data = std::make_shared<std::vector<uint8_t>>();
    auto [offset, bytes] = it->second[index];
    if (!readFile(path, *data, offset, bytes)) return nullptr;
    return data;
}

std::unique_ptr<Audio::Source> Audio::openSource(int sound) {
    Asset* a = asset(sound);
    if (!a) return nullptr;
    if (a->pcmBytes) {
        auto s = std::make_unique<PcmSource>();
        s->owned = a->pcm;
        s->bytes = a->pcmBytes;
        s->samples = a->pcmSamples;
        s->channels = std::max(1, a->channels);
        s->rate = a->rate;
        s->length = static_cast<int64_t>(a->pcmSamples) / s->channels;
        return s;
    }
    if (a->file) {
        auto s = std::make_unique<VorbisSource>();
        s->data = a->file;
        if (s->open()) return s;
        rt_.log("[audio] cannot decode sound %d", sound);
    }
    return nullptr;
}

Audio::Voice* Audio::voice(int id) {
    for (auto& v : voices_)
        if (v->id == id) return v.get();
    return nullptr;
}

void Audio::mix(float* out, int frames) {
    std::memset(out, 0, sizeof(float) * frames * 2);
    std::lock_guard<std::mutex> lock(mu_);
    if (allPaused_ || muted) return;
    for (auto& v : voices_) {
        if (v->paused || v->finished || !v->src) continue;
        Asset& a = assets_[v->sound];
        double g = master_ * v->gain * a.gain;
        double savedPitch = v->pitch;
        v->pitch = savedPitch * a.pitch * v->mixPitch;
        v->render(out, frames, g * v->mixL, g * v->mixR);
        v->pitch = savedPitch;
    }
    // Soft-knee limiter: transparent below 0.9, smoothly saturating above instead of hard clipping.
    for (int i = 0; i < frames * 2; i++) {
        float x = out[i], ax = std::fabs(x);
        if (ax > 0.9f) out[i] = std::copysign(0.9f + 0.1f * std::tanh((ax - 0.9f) / 0.1f), x);
    }
}

void Audio::update() {
    if (beforeUpdate) beforeUpdate();
    std::lock_guard<std::mutex> lock(mu_);
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](auto& v) { return v->finished; }), voices_.end());
    auto approach = [](double& g, double target, double step) {
        if (step == 0 || g == target) return;
        if (g < target) g = std::min(target, g + step);
        else g = std::max(target, g - step);
    };
    for (auto& v : voices_) approach(v->gain, v->gainTarget, v->gainStep);
    for (auto& [id, g] : groups_) approach(g.gain, g.target, g.step);
    for (auto& v : voices_) updateSpatial(*v);
    for (auto& a : assets_) {
        approach(a.gain, a.gainTarget, a.gainStep);
        // Free large streamed files (music) once no voice uses them; small ones stay cached.
        if (a.file && a.file.use_count() == 1 && a.file->size() > (512u << 10)) {
            a.file.reset();
            a.tried = false;
        }
    }
}

// OpenAL-style distance models, as GameMaker's native runners use (gain clamped to 0..1).
double Audio::attenuation(double d, double ref, double max, double factor) const {
    if (ref <= 0) ref = 0.0001;
    auto clampD = [&](double x) { return std::max(ref, std::min(x, std::max(ref, max))); };
    double g = 1;
    switch (falloffModel) {
        case 1: g = ref / (ref + factor * (d - ref)); break;                              // inverse
        case 2: d = clampD(d); g = ref / (ref + factor * (d - ref)); break;               // inverse clamped
        case 3: g = max > ref ? 1 - factor * (d - ref) / (max - ref) : 1; break;          // linear
        case 4: d = clampD(d); g = max > ref ? 1 - factor * (d - ref) / (max - ref) : 1; break;
        case 5: g = d > 0 ? std::pow(d / ref, -factor) : 1; break;                         // exponent
        case 6: d = clampD(d); g = std::pow(d / ref, -factor); break;
        case 7: case 8: {  // scaled: the clamped curve, rescaled to reach 0 at the max distance
            auto curve = [&](double x) {
                x = clampD(x);
                return falloffModel == 7 ? ref / (ref + factor * (x - ref)) : std::pow(x / ref, -factor);
            };
            double atMax = curve(max);
            g = atMax < 1 ? (curve(d) - atMax) / (1 - atMax) : 1;
            break;
        }
        default: break;
    }
    return std::max(0.0, std::min(1.0, g));
}

// Gains for one voice this step: falloff and left/right balance from the listener, plus the
// audio group's, the emitter's and its bus's gain.
void Audio::updateSpatial(Voice& v) {
    double gain = 1, pitch = 1, pan = 0;
    double x = v.px, y = v.py, z = v.pz, ref = v.ref, max = v.max, factor = v.factor;
    bool spatial = v.positional;
    if (Emitter* e = v.emitter >= 0 ? emitter(v.emitter) : nullptr) {
        spatial = true;
        x = e->x, y = e->y, z = e->z, ref = e->ref, max = e->max, factor = e->factor;
        gain *= e->gain;
        pitch *= e->pitch;
        size_t bus = e->bus < 0 ? 0 : static_cast<size_t>(e->bus);
        if (bus < busGains.size()) gain *= busGains[bus];
    } else if (!busGains.empty()) {
        gain *= busGains[0];  // the main bus
    }
    if (spatial) {
        double dx = x - listener.x, dy = y - listener.y, dz = z - listener.z;
        double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        gain *= attenuation(dist, ref, max, factor);
        // Right = look-at x up; with the default orientation sounds to the right play on the left,
        // as in GameMaker (games set the orientation to (0, 0, 1000, 0, -1, 0) to fix this).
        double rx = listener.atY * listener.upZ - listener.atZ * listener.upY;
        double ry = listener.atZ * listener.upX - listener.atX * listener.upZ;
        double rz = listener.atX * listener.upY - listener.atY * listener.upX;
        double rl = std::sqrt(rx * rx + ry * ry + rz * rz);
        if (dist > 0 && rl > 0) pan = (dx * rx + dy * ry + dz * rz) / (dist * rl);
    }
    auto g = groups_.find(soundGroup(v.sound));
    if (g != groups_.end()) gain *= g->second.gain;
    v.mixL = gain * std::min(1.0, 1 - pan);
    v.mixR = gain * std::min(1.0, 1 + pan);
    v.mixPitch = pitch;
}

int Audio::play(int sound, double priority, bool loop, const PlayParams& params) {
    std::unique_ptr<Source> src = openSource(sound);
    if (!src) return -1;
    if (rt_.debugTrace && static_cast<size_t>(sound) < rt_.dw.sounds.size()) {
        const Sound& s = rt_.dw.sounds[sound];
        Asset& a = assets_[sound];
        rt_.log("[audio] play %.*s%s from %s (%.1fs)", int(s.name.size()), s.name.data(), loop ? " (loop)" : "",
                a.pcmBytes ? "data.win" : std::string(s.file).c_str(),
                src->rate ? static_cast<double>(src->length) / src->rate : 0.0);
    }
    auto v = std::make_unique<Voice>();
    v->sound = sound;
    v->loop = loop;
    v->priority = priority;
    v->gain = v->gainTarget = std::max(0.0, params.gain);
    v->pitch = std::max(0.01, params.pitch);
    v->emitter = params.emitter;
    v->positional = params.positional;
    v->px = params.x, v->py = params.y, v->pz = params.z;
    v->ref = params.ref, v->max = params.max, v->factor = params.factor;
    if (Asset* a = asset(sound)) {
        v->loopStartFrames = static_cast<int64_t>(a->loopStart * src->rate);
        v->loopEndFrames = static_cast<int64_t>(a->loopEnd * src->rate);
    }
    v->src = std::move(src);
    if (params.offset > 0) v->seekFrames(static_cast<int64_t>(params.offset * v->src->rate));
    updateSpatial(*v);
    std::lock_guard<std::mutex> lock(mu_);
    if (voices_.size() >= 128) {
        auto lowest = std::min_element(voices_.begin(), voices_.end(),
                                       [](auto& a, auto& b) { return a->priority < b->priority; });
        if ((*lowest)->priority > priority) return -1;
        voices_.erase(lowest);
    }
    v->id = nextId_++;
    int id = v->id;
    voices_.push_back(std::move(v));
    return id;
}

void Audio::stop(int x) {
    std::lock_guard<std::mutex> lock(mu_);
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [&](auto& v) { return x >= kFirstVoiceId ? v->id == x : v->sound == x; }),
                  voices_.end());
}

void Audio::stopAll() {
    std::lock_guard<std::mutex> lock(mu_);
    voices_.clear();
}

bool Audio::isPlaying(int x) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& v : voices_)
        if (!v->finished && (x >= kFirstVoiceId ? v->id == x : v->sound == x)) return true;
    return false;
}

// A sound asset, or a voice that is still playing (or paused): games stop the previous
// track only when audio_exists(voice) says so.
bool Audio::exists(int sound) const {
    if (sound >= kFirstVoiceId) {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& v : voices_)
            if (v->id == sound) return true;
        return false;
    }
    return sound >= 0 && static_cast<size_t>(sound) < assets_.size();
}

void Audio::pause(int x, bool paused) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& v : voices_)
        if (x >= kFirstVoiceId ? v->id == x : v->sound == x) v->paused = paused;
}

void Audio::pauseAll(bool paused) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& v : voices_) v->paused = paused;
}

void Audio::setGain(int x, double gain, double ms) {
    double steps = ms / 1000.0 * (rt_.roomSpeed > 0 ? rt_.roomSpeed : 60);
    std::lock_guard<std::mutex> lock(mu_);
    auto apply = [&](double& g, double& target, double& step) {
        target = std::max(0.0, gain);
        if (steps <= 1) {
            g = target;
            step = 0;
        } else {
            step = std::fabs(target - g) / steps;
        }
    };
    if (x >= kFirstVoiceId) {
        if (Voice* v = voice(x)) apply(v->gain, v->gainTarget, v->gainStep);
    } else if (Asset* a = asset(x)) {
        apply(a->gain, a->gainTarget, a->gainStep);
    }
}

double Audio::getGain(int x) {
    std::lock_guard<std::mutex> lock(mu_);
    if (x >= kFirstVoiceId) {
        Voice* v = voice(x);
        return v ? v->gain : 0;
    }
    Asset* a = asset(x);
    return a ? a->gain : 0;
}

void Audio::setPitch(int x, double pitch) {
    std::lock_guard<std::mutex> lock(mu_);
    pitch = std::max(0.01, pitch);
    if (x >= kFirstVoiceId) {
        if (Voice* v = voice(x)) v->pitch = pitch;
    } else if (Asset* a = asset(x)) {
        a->pitch = pitch;
    }
}

double Audio::getPitch(int x) {
    std::lock_guard<std::mutex> lock(mu_);
    if (x >= kFirstVoiceId) {
        Voice* v = voice(x);
        return v ? v->pitch : 1;
    }
    Asset* a = asset(x);
    return a ? a->pitch : 1;
}

double Audio::getPosition(int id) {
    std::lock_guard<std::mutex> lock(mu_);
    Voice* v = id >= kFirstVoiceId ? voice(id) : nullptr;
    if (!v) {
        for (auto& w : voices_)
            if (w->sound == id) v = w.get();
    }
    if (!v || !v->src) return 0;
    return v->positionFrames() / v->src->rate;
}

void Audio::setPosition(int id, double seconds) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& v : voices_)
        if ((id >= kFirstVoiceId ? v->id == id : v->sound == id) && v->src)
            v->seekFrames(static_cast<int64_t>(seconds * v->src->rate));
}

double Audio::soundLength(int sound) {
    if (sound >= kFirstVoiceId) {  // a playing instance: the length of the sound it plays
        std::lock_guard<std::mutex> lock(mu_);
        Voice* v = voice(sound);
        if (!v) return 0;
        if (v->src && v->src->rate > 0) return static_cast<double>(v->src->length) / v->src->rate;
        sound = v->sound;
    }
    std::unique_ptr<Source> s = openSource(sound);
    if (!s || s->rate <= 0) return 0;
    return static_cast<double>(s->length) / s->rate;
}

int Audio::soundGroup(int sound) const {
    return sound >= 0 && static_cast<size_t>(sound) < rt_.dw.sounds.size() ? rt_.dw.sounds[sound].group : 0;
}

// audio_create_stream: an OGG file played like a sound asset, numbered after the game's sounds.
int Audio::createStream(const std::string& path) {
    auto data = std::make_shared<std::vector<uint8_t>>();
    if (!readFile(path, *data)) {
        rt_.log("[audio] audio_create_stream: cannot open %s", path.c_str());
        return -1;
    }
    Asset a;
    a.tried = a.configured = a.streamed = true;
    a.file = std::move(data);
    std::lock_guard<std::mutex> lock(mu_);
    if (assets_.size() >= assets_.capacity()) return -1;
    assets_.push_back(std::move(a));
    streamNames_.push_back(path);
    return static_cast<int>(assets_.size() - 1);
}

int Audio::createPcmSound(std::vector<int16_t> pcm, int channels, int rate) {
    Asset a;
    a.tried = a.configured = true;
    a.pcm = std::make_shared<std::vector<int16_t>>(std::move(pcm));
    a.pcmBytes = reinterpret_cast<const uint8_t*>(a.pcm->data());
    a.pcmSamples = a.pcm->size();
    a.channels = channels;
    a.rate = rate;
    std::lock_guard<std::mutex> lock(mu_);
    if (assets_.size() >= assets_.capacity()) return -1;
    assets_.push_back(std::move(a));
    streamNames_.push_back("");
    return static_cast<int>(assets_.size() - 1);
}

void Audio::destroyStream(int sound) {
    if (static_cast<size_t>(sound) < rt_.dw.sounds.size() || static_cast<size_t>(sound) >= assets_.size()) return;
    stop(sound);
    std::lock_guard<std::mutex> lock(mu_);
    assets_[sound].file.reset();
}

std::string Audio::streamName(int sound) const {
    size_t i = static_cast<size_t>(sound) - rt_.dw.sounds.size();
    if (sound < 0 || static_cast<size_t>(sound) < rt_.dw.sounds.size() || i >= streamNames_.size()) return "";
    std::string n = streamNames_[i];
    size_t slash = n.find_last_of("/\\");
    return slash == std::string::npos ? n : n.substr(slash + 1);
}

// ---------------------------------------------------------------------------------------------
// Emitters, groups, loop points, sync groups

int Audio::emitterCreate() {
    std::lock_guard<std::mutex> lock(mu_);
    for (size_t i = 0; i < emitters_.size(); i++)
        if (!emitters_[i].live) {
            emitters_[i] = Emitter();
            return static_cast<int>(i);
        }
    emitters_.emplace_back();
    return static_cast<int>(emitters_.size() - 1);
}

Audio::Emitter* Audio::emitter(int id) {
    return id >= 0 && static_cast<size_t>(id) < emitters_.size() && emitters_[id].live ? &emitters_[id] : nullptr;
}

void Audio::emitterFree(int id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!emitter(id)) return;
    emitters_[id].live = false;
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [&](auto& v) { return v->emitter == id; }),
                  voices_.end());
}

void Audio::setGroupGain(int group, double gain, double ms) {
    double steps = ms / 1000.0 * (rt_.roomSpeed > 0 ? rt_.roomSpeed : 60);
    std::lock_guard<std::mutex> lock(mu_);
    Group& g = groups_[group];
    g.target = std::max(0.0, gain);
    if (steps <= 1) {
        g.gain = g.target;
        g.step = 0;
    } else {
        g.step = std::fabs(g.target - g.gain) / steps;
    }
}

double Audio::groupGain(int group) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groups_.find(group);
    return it == groups_.end() ? 1.0 : it->second.gain;
}

void Audio::stopGroup(int group) {
    std::lock_guard<std::mutex> lock(mu_);
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [&](auto& v) { return soundGroup(v->sound) == group; }),
                  voices_.end());
}

bool Audio::isPaused(int x) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& v : voices_)
        if ((x >= kFirstVoiceId ? v->id == x : v->sound == x) && v->paused) return true;
    return false;
}

void Audio::setLoop(int x, bool loop) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& v : voices_)
        if (x >= kFirstVoiceId ? v->id == x : v->sound == x) v->loop = loop;
}

bool Audio::getLoop(int x) {
    std::lock_guard<std::mutex> lock(mu_);
    if (Voice* v = x >= kFirstVoiceId ? voice(x) : nullptr) return v->loop;
    return false;
}

void Audio::setLoopPoint(int x, bool end, double seconds) {
    std::lock_guard<std::mutex> lock(mu_);
    seconds = std::max(0.0, seconds);
    if (x >= kFirstVoiceId) {
        if (Voice* v = voice(x); v && v->src) (end ? v->loopEndFrames : v->loopStartFrames) = static_cast<int64_t>(seconds * v->src->rate);
    } else if (Asset* a = asset(x)) {
        (end ? a->loopEnd : a->loopStart) = seconds;
    }
}

double Audio::getLoopPoint(int x, bool end) {
    std::lock_guard<std::mutex> lock(mu_);
    if (x >= kFirstVoiceId) {
        Voice* v = voice(x);
        if (!v || !v->src || v->src->rate <= 0) return 0;
        return static_cast<double>(end ? v->loopEndFrames : v->loopStartFrames) / v->src->rate;
    }
    Asset* a = asset(x);
    return a ? (end ? a->loopEnd : a->loopStart) : 0;
}

int Audio::syncCreate(bool loop) {
    std::lock_guard<std::mutex> lock(mu_);
    syncs_.emplace_back();
    syncs_.back().loop = loop;
    return static_cast<int>(syncs_.size() - 1);
}

void Audio::syncAdd(int group, int sound) {
    std::lock_guard<std::mutex> lock(mu_);
    if (group >= 0 && static_cast<size_t>(group) < syncs_.size() && syncs_[group].live) syncs_[group].sounds.push_back(sound);
}

// All of a sync group's sounds start in the same mix call, so they stay sample-aligned.
void Audio::syncStart(int group) {
    if (group < 0 || static_cast<size_t>(group) >= syncs_.size() || !syncs_[group].live) return;
    syncStop(group);
    std::vector<int> ids;
    std::vector<std::unique_ptr<Voice>> made;
    for (int sound : syncs_[group].sounds) {
        std::unique_ptr<Source> src = openSource(sound);
        if (!src) continue;
        auto v = std::make_unique<Voice>();
        v->sound = sound;
        v->loop = syncs_[group].loop;
        v->priority = 0;
        v->src = std::move(src);
        made.push_back(std::move(v));
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& v : made) {
        v->id = nextId_++;
        ids.push_back(v->id);
        updateSpatial(*v);
        voices_.push_back(std::move(v));
    }
    syncs_[group].voices = ids;
}

void Audio::syncStop(int group) {
    if (group < 0 || static_cast<size_t>(group) >= syncs_.size()) return;
    for (int id : syncs_[group].voices) stop(id);
    syncs_[group].voices.clear();
}

void Audio::syncPause(int group, bool paused) {
    if (group < 0 || static_cast<size_t>(group) >= syncs_.size()) return;
    for (int id : syncs_[group].voices) pause(id, paused);
}

bool Audio::syncPlaying(int group) {
    if (group < 0 || static_cast<size_t>(group) >= syncs_.size()) return false;
    for (int id : syncs_[group].voices)
        if (isPlaying(id)) return true;
    return false;
}

double Audio::syncPosition(int group) {
    if (group < 0 || static_cast<size_t>(group) >= syncs_.size() || syncs_[group].voices.empty()) return 0;
    return getPosition(syncs_[group].voices.front());
}

void Audio::syncDestroy(int group) {
    syncStop(group);
    if (group >= 0 && static_cast<size_t>(group) < syncs_.size()) syncs_[group].live = false;
}

}  // namespace gm
