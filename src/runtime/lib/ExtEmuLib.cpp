// Stand-ins for native extensions games commonly ship, built on the runner's own subsystems so
// the game works without the DLL. Registered before the generic extension stubs, which only fill
// in names nothing else provides.
//
// FMODGMS (FMOD for GameMaker): sounds load as audio streams and play on channels; lengths and
// positions are in samples at the mix rate, which is what FMODGMS_Sys_Get_SampleRate reports
// (games convert with it).
#include <map>

#include "runtime/Audio.h"
#include "runtime/Builtins.h"
#include "runtime/Stores.h"

namespace gm {
namespace {

struct FmodChannel {
    int voice = -1;
    double volume = 1, pitch = 1;
};
std::map<int, FmodChannel> gChannels;
std::map<int, bool> gLoop;  // per sound, from FMODGMS_Snd_Set_LoopMode
int gNextChannel = 0;

FmodChannel* channel(int c) {
    auto it = gChannels.find(c);
    return it == gChannels.end() ? nullptr : &it->second;
}
void stopChannel(Runner& rt, FmodChannel& ch) {
    if (ch.voice >= 0) rt.audio->stop(ch.voice);
    ch.voice = -1;
}

}  // namespace

GMFN(FMODGMS_ok) { GM_UNUSED_FN_ARGS; return Value(1.0); }
GMFN(FMODGMS_zero) { GM_UNUSED_FN_ARGS; return Value(0.0); }
GMFN(FMODGMS_Util_GetErrorMessage) { GM_UNUSED_FN_ARGS; return Value(""); }
GMFN(FMODGMS_Sys_Close) {
    for (auto& [id, ch] : gChannels) stopChannel(rt, ch);
    gChannels.clear();
    return Value(1.0);
}
GMFN(FMODGMS_Sys_Get_SampleRate) { GM_UNUSED_FN_ARGS; return Value(static_cast<double>(Audio::kRate)); }
GMFN(FMODGMS_Snd_LoadSound) {  // (path) -> sound index, or -1
    int s = rt.audio->createStream(rt.files->readPath(S(0)));
    return Value(static_cast<double>(s));
}
GMFN(FMODGMS_Snd_Unload) {
    rt.audio->destroyStream(I(0));
    return Value(1.0);
}
GMFN(FMODGMS_Snd_Set_LoopMode) {  // (sound, mode, count): mode 1 loops
    gLoop[I(0)] = I(1) != 0;
    return Value(1.0);
}
GMFN(FMODGMS_Snd_Get_Length) {  // samples
    return Value(rt.audio->soundLength(I(0)) * Audio::kRate);
}
GMFN(FMODGMS_Snd_PlaySound) {  // (sound, channel)
    FmodChannel* ch = channel(I(1));
    if (!ch) return Value(-1.0);
    stopChannel(rt, *ch);
    ch->voice = rt.audio->play(I(0), 100, gLoop[I(0)]);
    if (ch->voice >= 0) {
        rt.audio->setGain(ch->voice, ch->volume, 0);
        rt.audio->setPitch(ch->voice, ch->pitch);
    }
    return Value(1.0);
}
GMFN(FMODGMS_Chan_CreateChannel) {
    GM_UNUSED_FN_ARGS;
    gChannels[gNextChannel] = FmodChannel();
    return Value(static_cast<double>(gNextChannel++));
}
GMFN(FMODGMS_Chan_RemoveChannel) {
    if (FmodChannel* ch = channel(I(0))) stopChannel(rt, *ch);
    gChannels.erase(I(0));
    return Value(1.0);
}
GMFN(FMODGMS_Chan_StopChannel) {
    if (FmodChannel* ch = channel(I(0))) stopChannel(rt, *ch);
    return Value(1.0);
}
GMFN(FMODGMS_Chan_PauseChannel) {
    if (FmodChannel* ch = channel(I(0)); ch && ch->voice >= 0) rt.audio->pause(ch->voice, true);
    return Value(1.0);
}
GMFN(FMODGMS_Chan_ResumeChannel) {
    if (FmodChannel* ch = channel(I(0)); ch && ch->voice >= 0) rt.audio->pause(ch->voice, false);
    return Value(1.0);
}
GMFN(FMODGMS_Chan_Get_Position) {  // samples
    FmodChannel* ch = channel(I(0));
    if (!ch || ch->voice < 0 || !rt.audio->isPlaying(ch->voice)) return Value(0.0);
    return Value(rt.audio->getPosition(ch->voice) * Audio::kRate);
}
GMFN(FMODGMS_Chan_Set_Position) {
    if (FmodChannel* ch = channel(I(0)); ch && ch->voice >= 0) rt.audio->setPosition(ch->voice, D(1) / Audio::kRate);
    return Value(1.0);
}
GMFN(FMODGMS_Chan_Set_Volume) {
    if (FmodChannel* ch = channel(I(0))) {
        ch->volume = D(1);
        if (ch->voice >= 0) rt.audio->setGain(ch->voice, ch->volume, 0);
    }
    return Value(1.0);
}
GMFN(FMODGMS_Chan_Get_Volume) {
    FmodChannel* ch = channel(I(0));
    return Value(ch ? ch->volume : 0.0);
}
GMFN(FMODGMS_Chan_Set_Pitch) {
    if (FmodChannel* ch = channel(I(0))) {
        ch->pitch = D(1);
        if (ch->voice >= 0) rt.audio->setPitch(ch->voice, ch->pitch);
    }
    return Value(1.0);
}
GMFN(FMODGMS_Chan_Get_Pitch) {
    FmodChannel* ch = channel(I(0));
    return Value(ch ? ch->pitch : 1.0);
}

void registerExtensionEmulation(Runner& rt) {
    gChannels.clear();
    gLoop.clear();
    gNextChannel = 0;
    for (const char* n : {"FMODGMS_Sys_Create", "FMODGMS_Sys_Initialize", "FMODGMS_Sys_Update",
                          "FMODGMS_Util_Handshake", "FMODGMS_Sys_Set_SoftwareFormat", "FMODGMS_Sys_Set_DSPBufferSize",
                          "FMODGMS_Sys_Set_OutputMode", "FMODGMS_FFT_Init", "FMODGMS_Snd_Set_LoopPoints"})
        rt.vm.registerBuiltin(n, fn_FMODGMS_ok);
    for (const char* n : {"FMODGMS_Chan_Get_Level", "FMODGMS_Sys_Get_CPUUsage", "FMODGMS_FFT_Get_BinValue",
                          "FMODGMS_FFT_Get_DominantFrequency", "FMODGMS_Chan_Get_Mute", "FMODGMS_Snd_Get_NumTags"})
        rt.vm.registerBuiltin(n, fn_FMODGMS_zero);
    GMREG(FMODGMS_Util_GetErrorMessage); GMREG(FMODGMS_Sys_Close); GMREG(FMODGMS_Sys_Get_SampleRate);
    GMREG(FMODGMS_Snd_LoadSound); GMREG(FMODGMS_Snd_Unload); GMREG(FMODGMS_Snd_Set_LoopMode);
    GMREG(FMODGMS_Snd_Get_Length); GMREG(FMODGMS_Snd_PlaySound); GMREG(FMODGMS_Chan_CreateChannel);
    GMREG(FMODGMS_Chan_RemoveChannel); GMREG(FMODGMS_Chan_StopChannel); GMREG(FMODGMS_Chan_PauseChannel);
    GMREG(FMODGMS_Chan_ResumeChannel); GMREG(FMODGMS_Chan_Get_Position); GMREG(FMODGMS_Chan_Set_Position);
    GMREG(FMODGMS_Chan_Set_Volume); GMREG(FMODGMS_Chan_Get_Volume); GMREG(FMODGMS_Chan_Set_Pitch);
    GMREG(FMODGMS_Chan_Get_Pitch);
    rt.vm.registerBuiltin("FMODGMS_Snd_LoadStream", fn_FMODGMS_Snd_LoadSound);
}

}  // namespace gm
