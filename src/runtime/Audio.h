// Software audio mixer implementing GameMaker's audio_* model.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gm {

class Runner;
struct AudioBlobRef {
    const uint8_t* data;
    size_t size;
};

class Audio {
public:
    explicit Audio(Runner& rt);
    ~Audio();

    static constexpr int kRate = 44100;
    static constexpr int kFirstVoiceId = 100000;

    // Audio thread: mixes `frames` stereo frames into `out` (interleaved float).
    void mix(float* out, int frames);
    // Game thread, once per step.
    void update();

    // audio_play_sound(_at/_on/_ext) options: gain, offset (seconds), pitch, and an emitter or a
    // position with its own falloff.
    struct PlayParams {
        double gain = 1, offset = 0, pitch = 1;
        int emitter = -1;
        bool positional = false;
        double x = 0, y = 0, z = 0, ref = 0, max = 1, factor = 1;
    };
    int play(int sound, double priority, bool loop, const PlayParams& params);
    int play(int sound, double priority, bool loop) { return play(sound, priority, loop, PlayParams()); }

    // Emitters, the listener and the falloff model (audio_falloff_none .. _exponent_distance_scaled).
    struct Emitter {
        bool live = true;
        double x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0;
        double ref = 100, max = 100000, factor = 1, gain = 1, pitch = 1;
        int bus = -1;  // -1: the main bus
        uint32_t listenerMask = 1;
    };
    struct Listener {
        double x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0;
        double atX = 0, atY = 0, atZ = 1, upX = 0, upY = 1, upZ = 0;  // GameMaker's default orientation
    };
    int emitterCreate();
    Emitter* emitter(int id);
    void emitterFree(int id);
    Listener listener;
    int falloffModel = 0;
    // Buses (GameMaker 2023): `gain` and `bypass` fields of GML structs, read every step.
    std::vector<double> busGains;  // by bus index; set from the structs by the audio library
    std::function<void()> beforeUpdate;  // game thread, start of update()

    // Audio groups, loop points, sync groups.
    void setGroupGain(int group, double gain, double ms);
    double groupGain(int group);
    void stopGroup(int group);
    bool isPaused(int idOrSound);
    void setLoop(int idOrSound, bool loop);
    bool getLoop(int idOrSound);
    void setLoopPoint(int idOrSound, bool end, double seconds);
    double getLoopPoint(int idOrSound, bool end);
    int createStream(const std::string& path);  // audio_create_stream
    int createPcmSound(std::vector<int16_t> pcm, int channels, int rate);  // decoded audio (a video's track)
    void destroyStream(int sound);
    std::string streamName(int sound) const;
    int soundGroup(int sound) const;
    int syncCreate(bool loop);
    void syncAdd(int group, int sound);
    void syncStart(int group);
    void syncStop(int group);
    void syncPause(int group, bool paused);
    bool syncPlaying(int group);
    double syncPosition(int group);
    void syncDestroy(int group);
    void stop(int idOrSound);
    void stopAll();
    bool isPlaying(int idOrSound);
    bool exists(int sound) const;
    void pause(int idOrSound, bool paused);
    void pauseAll(bool paused);
    void setGain(int idOrSound, double gain, double ms);
    double getGain(int idOrSound);
    void setPitch(int idOrSound, double pitch);
    double getPitch(int idOrSound);
    double getPosition(int id);
    void setPosition(int id, double seconds);
    void setMasterGain(double g) { master_ = g; }
    double masterGain() const { return master_; }
    double soundLength(int sound);
    bool muted = false;

    struct Source;
    struct Voice;

private:
    struct Asset {
        bool tried = false;      // data loaded (or failed); reset when idle music is released
        bool configured = false; // gain/pitch initialised from the sound's defaults
        bool streamed = false;
        std::shared_ptr<std::vector<int16_t>> pcm;  // converted samples (8-bit / float WAVs)
        const uint8_t* pcmBytes = nullptr;           // 16-bit WAV data used in place from data.win
        size_t pcmSamples = 0;
        std::shared_ptr<std::vector<uint8_t>> file; // compressed data, for streamed sounds
        std::shared_ptr<std::vector<uint8_t>> blob; // audio group WAV data that pcmBytes points into
        int channels = 0, rate = 0;
        double gain = 1, gainTarget = 1, gainStep = 0;
        double pitch = 1;
        double loopStart = 0, loopEnd = 0;  // seconds; an end of 0 means the end of the sound
    };
    struct Group {
        double gain = 1, target = 1, step = 0;
    };
    struct SyncGroup {
        bool live = true, loop = false;
        std::vector<int> sounds, voices;
    };
    Asset* asset(int sound);
    void updateSpatial(Voice& v);
    double attenuation(double dist, double ref, double max, double factor) const;
    std::unique_ptr<Source> openSource(int sound);
    Voice* voice(int id);

    // A sound's data from audiogroupN.dat (read from the file when needed), or null.
    std::shared_ptr<std::vector<uint8_t>> groupBlob(int group, int index);

    Runner& rt_;
    mutable std::mutex mu_;
    // audiogroupN.dat files (GMS2): where each AUDO entry is in the file (offset, size). Only the
    // index is read up front; each sound's data is read when it is first played.
    std::unordered_map<int, std::vector<std::pair<uint64_t, uint32_t>>> groupIndex_;
    std::vector<Asset> assets_;
    std::vector<std::unique_ptr<Voice>> voices_;
    int nextId_ = kFirstVoiceId;
    std::vector<Emitter> emitters_;
    std::unordered_map<int, Group> groups_;
    std::vector<SyncGroup> syncs_;
    std::vector<std::string> streamNames_;
    double master_ = 1;
    bool allPaused_ = false;
};

}  // namespace gm
