// Sequences (GMS 2.3+), after YoYo's runner (yySequence.js, yyRoom.js, Function_Layers.js).
//
// A sequence asset is a GML struct tree, as in GameMaker (name, tracks[], keyframes[],
// channels[] ...), so games can read and edit it: games swap a track's sprite with
// `seq.tracks[i].keyframes[0].channels[0].spriteIndex = ...`. Playback works from a compiled
// copy of that tree, rebuilt from the structs once per step for sequences the game has been
// handed (layer_sequence_get_sequence, sequence_get, ...), so such edits take effect.
//
// Each frame a sequence element evaluates its track tree at the play head: parameter tracks
// (position, rotation, scale, origin, colour multiply/add, image index/speed, ...) give each
// track a local transform that nests into its parent's; graphic tracks draw sprite frames,
// instance tracks create and steer instances, sequence tracks nest other sequences, audio
// tracks play sounds. Message and moment keyframes fire as the head passes them.
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <unordered_map>

#include "runtime/Audio.h"
#include "runtime/BuiltinVars.h"
#include "runtime/Builtins.h"
#include "runtime/Graphics.h"
#include "runtime/Sequences.h"
#include "runtime/Stores.h"

namespace gm {

namespace {

// Track types (YoYo's eSTT_*) and built-in parameters (eT_*).
enum TrackType : int {
    TT_None = 0, TT_Graphic = 1, TT_Audio = 2, TT_Real = 3, TT_Colour = 4, TT_Bool = 5, TT_String = 6,
    TT_Sequence = 7, TT_ClipMask = 8, TT_ClipMaskMask = 9, TT_ClipMaskSubject = 10, TT_Group = 11,
    TT_Empty = 12, TT_SpriteFrames = 13, TT_Instance = 14, TT_Message = 15, TT_Moment = 16, TT_Text = 17,
    TT_Particle = 18, TT_AudioEffect = 19
};
enum Param : int {
    P_None = 0, P_Gain = 5, P_Pitch = 6, P_Falloff = 7, P_Rotation = 8, P_BlendAdd = 9, P_BlendMultiply = 10,
    P_Position = 14, P_Scale = 15, P_Origin = 16, P_ImageSpeed = 17, P_ImageIndex = 18
};
constexpr int kTraitChildrenIgnoreOrigin = 1;

int trackTypeOfModel(std::string_view m) {
    static const std::pair<const char*, int> kModels[] = {
        {"GMGraphicTrack", TT_Graphic}, {"GMAudioTrack", TT_Audio}, {"GMRealTrack", TT_Real},
        {"GMColourTrack", TT_Colour}, {"GMBoolTrack", TT_Bool}, {"GMStringTrack", TT_String},
        {"GMSequenceTrack", TT_Sequence}, {"GMClipMaskTrack", TT_ClipMask}, {"GMClipMask_Mask", TT_ClipMaskMask},
        {"GMClipMask_Subject", TT_ClipMaskSubject}, {"GMGroupTrack", TT_Group}, {"GMSpriteFramesTrack", TT_SpriteFrames},
        {"GMInstanceTrack", TT_Instance}, {"GMTextTrack", TT_Text}, {"GMParticleTrack", TT_Particle},
        {"GMAudioEffectTrack", TT_AudioEffect}};
    for (auto& [name, t] : kModels)
        if (m == name) return t;
    return TT_None;
}
// AssignBuiltinTrackName: parameter tracks are recognised by name.
int paramOfName(const std::string& n) {
    static const std::pair<const char*, int> kNames[] = {
        {"position", P_Position}, {"scale", P_Scale}, {"gain", P_Gain}, {"pitch", P_Pitch}, {"falloff", P_Falloff},
        {"rotation", P_Rotation}, {"image_angle", P_Rotation}, {"blend_add", P_BlendAdd},
        {"blend_multiply", P_BlendMultiply}, {"image_blend", P_BlendMultiply}, {"origin", P_Origin},
        {"image_speed", P_ImageSpeed}, {"image_index", P_ImageIndex}};
    for (auto& [name, p] : kNames)
        if (n == name) return p;
    return P_None;
}
bool isParameter(int t) { return t == TT_Real || t == TT_Colour || t == TT_Bool || t == TT_String || t == TT_AudioEffect; }

// ---------------------------------------------------------------------------------------------
// GML struct helpers

struct Ids {
    int32_t name, loopmode, playbackSpeed, playbackSpeedType, length, volume, xorigin, yorigin, tracks,
        messageEventKeyframes, momentKeyframes, type, traits, enabled, visible, keyframes, interpolation, frame,
        stretch, disabled, channels, channel, spriteIndex, value, curve, hasEmbeddedCurve, colour, color,
        objectIndex, sequence, soundIndex, mode, imageIndex, events, event, headPosition, headDirection,
        speedScale, paused, finished, elementID, activeTracks, event_create, event_destroy, event_clean_up,
        event_step, event_step_begin, event_step_end, event_broadcast_message, particleSystemIndex;
};
Ids ids;
void initIds(Runner& rt) {
    auto v = [&](const char* n) { return rt.vm.varIdForName(n); };
    ids = {v("name"), v("loopmode"), v("playbackSpeed"), v("playbackSpeedType"), v("length"), v("volume"),
           v("xorigin"), v("yorigin"), v("tracks"), v("messageEventKeyframes"), v("momentKeyframes"), v("type"),
           v("traits"), v("enabled"), v("visible"), v("keyframes"), v("interpolation"), v("frame"), v("stretch"),
           v("disabled"), v("channels"), v("channel"), v("spriteIndex"), v("value"), v("curve"),
           v("hasEmbeddedCurve"), v("colour"), v("color"), v("objectIndex"), v("sequence"), v("soundIndex"),
           v("mode"), v("imageIndex"), v("events"), v("event"), v("headPosition"), v("headDirection"),
           v("speedScale"), v("paused"), v("finished"), v("elementID"), v("activeTracks"), v("event_create"),
           v("event_destroy"), v("event_clean_up"), v("event_step"), v("event_step_begin"), v("event_step_end"),
           v("event_broadcast_message"), v("particleSystemIndex")};
}

Runner* gRt = nullptr;
void set(const Value& s, int32_t id, Value v) {
    if (GMObject* o = s.obj()) gRt->vm.setInstanceVar(o->inst, id, std::move(v));
}
Value get(const Value& s, int32_t id) {
    GMObject* o = s.obj();
    return o ? gRt->vm.getInstanceVar(o->inst, id) : Value();
}
double num(const Value& s, int32_t id, double def = 0) {
    Value v = get(s, id);
    return v.isNumeric() ? v.num() : def;
}
Value newStruct() { return gRt->vm.newStruct(nullptr); }
Value newArray(std::vector<Value> items) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    a->rows[0] = std::move(items);
    return arr;
}
const std::vector<Value>& items(const Value& arr) {
    static const std::vector<Value> none;
    GMArray* a = arr.arr();
    return a && !a->rows.empty() ? a->rows[0] : none;
}

// Sequence asset -> struct tree (the shape YoYo's runner exposes to GML).
Value channelStruct(const SeqChannel& c, int type, const DataWin& dw) {
    Value s = newStruct();
    set(s, ids.channel, Value(c.channel));
    switch (type) {
        case TT_Graphic: set(s, ids.spriteIndex, Value(c.index)); break;
        case TT_Instance: set(s, ids.objectIndex, Value(c.index)); break;
        case TT_Sequence: set(s, ids.sequence, Value(c.index)); break;
        case TT_Particle: set(s, ids.particleSystemIndex, Value(c.index)); break;
        case TT_SpriteFrames: set(s, ids.imageIndex, Value(c.index)); break;
        case TT_Bool: set(s, ids.value, Value::boolean(c.index != 0)); break;
        case TT_String: set(s, ids.value, Value(c.text)); break;
        case TT_Audio:
            set(s, ids.soundIndex, Value(c.index));
            set(s, ids.mode, Value(c.mode));
            break;
        case TT_Real:
        case TT_Colour: {
            if (type == TT_Real) {
                set(s, ids.value, Value(static_cast<double>(c.real)));
            } else {
                set(s, ids.colour, Value(static_cast<double>(c.colour)));
                set(s, ids.color, Value(static_cast<double>(c.colour)));
            }
            bool embedded = c.embeddedCurve >= 0 && static_cast<size_t>(c.embeddedCurve) < dw.seqCurves.size();
            set(s, ids.hasEmbeddedCurve, Value::boolean(embedded));
            if (embedded) set(s, ids.curve, animCurveFromData(*gRt, dw.seqCurves[c.embeddedCurve]));
            else set(s, ids.curve, Value(c.curve));
            break;
        }
        default: break;
    }
    return s;
}
Value keyframeStruct(const SeqKeyframe& k, int type, const DataWin& dw) {
    Value s = newStruct();
    set(s, ids.frame, Value(static_cast<double>(k.key)));
    set(s, ids.length, Value(static_cast<double>(k.length)));
    set(s, ids.stretch, Value::boolean(k.stretch));
    set(s, ids.disabled, Value::boolean(k.disabled));
    // channels[] is indexed by channel number (sparse in YoYo's runner; holes are undefined)
    std::vector<Value> chans;
    for (const SeqChannel& c : k.channels) {
        if (c.channel < 0 || c.channel > 63) continue;
        if (static_cast<size_t>(c.channel) >= chans.size()) chans.resize(c.channel + 1);
        chans[c.channel] = channelStruct(c, type, dw);
    }
    set(s, ids.channels, newArray(std::move(chans)));
    return s;
}
Value trackStruct(const SeqTrack& t, const DataWin& dw) {
    int type = trackTypeOfModel(t.model);
    Value s = newStruct();
    set(s, ids.name, Value(t.name));
    set(s, ids.type, Value(type));
    set(s, ids.traits, Value(t.traits));
    auto tagged = [&](int32_t tag) { return std::find(t.tags.begin(), t.tags.end(), tag) != t.tags.end(); };
    set(s, ids.enabled, Value::boolean(!tagged(2)));  // the IDE's Disable and Invisible tags
    set(s, ids.visible, Value::boolean(!tagged(1)));
    if (type == TT_Real || type == TT_Colour) set(s, ids.interpolation, Value(t.interpolation));
    std::vector<Value> sub, keys;
    // track lists are stored top track first; GameMaker keeps (and draws) them bottom first
    for (auto it = t.tracks.rbegin(); it != t.tracks.rend(); ++it) sub.push_back(trackStruct(*it, dw));
    for (const SeqKeyframe& k : t.keys) keys.push_back(keyframeStruct(k, type, dw));
    set(s, ids.tracks, newArray(std::move(sub)));
    set(s, ids.keyframes, newArray(std::move(keys)));
    return s;
}
Value sequenceStruct(const Sequence& q, const DataWin& dw) {
    Value s = newStruct();
    set(s, ids.name, Value(q.name));
    set(s, ids.loopmode, Value(static_cast<double>(q.playback)));
    set(s, ids.playbackSpeed, Value(static_cast<double>(q.speed)));
    set(s, ids.playbackSpeedType, Value(static_cast<double>(q.speedType)));
    set(s, ids.length, Value(static_cast<double>(q.length)));
    set(s, ids.volume, Value(static_cast<double>(q.volume)));
    set(s, ids.xorigin, Value(q.originX));
    set(s, ids.yorigin, Value(q.originY));
    std::vector<Value> tracks, messages, moments;
    for (auto it = q.tracks.rbegin(); it != q.tracks.rend(); ++it) tracks.push_back(trackStruct(*it, dw));
    for (const SeqKeyframe& k : q.messages) {
        Value ks = keyframeStruct(k, TT_None, dw);
        std::vector<Value> chans;
        for (const SeqChannel& c : k.channels) {
            Value cs = newStruct();
            set(cs, ids.channel, Value(c.channel));
            std::vector<Value> ev;
            for (auto m : c.messages) ev.push_back(Value(m));
            set(cs, ids.events, newArray(std::move(ev)));
            chans.push_back(cs);
        }
        set(ks, ids.channels, newArray(std::move(chans)));
        messages.push_back(ks);
    }
    for (const SeqKeyframe& k : q.moments) {
        Value ks = keyframeStruct(k, TT_None, dw);
        std::vector<Value> chans;
        for (const SeqChannel& c : k.channels) {
            Value cs = newStruct();
            set(cs, ids.channel, Value(c.channel));
            // the moment's function, by name (a script or a function of the game)
            Value fn = Value(-1.0);
            for (size_t i = 0; i < dw.scripts.size(); i++)
                if (dw.scripts[i].name == c.text) fn = gRt->vm.scriptValue(static_cast<int32_t>(i));
            set(cs, ids.event, fn);
            chans.push_back(cs);
        }
        set(ks, ids.channels, newArray(std::move(chans)));
        moments.push_back(ks);
    }
    set(s, ids.tracks, newArray(std::move(tracks)));
    set(s, ids.messageEventKeyframes, newArray(std::move(messages)));
    set(s, ids.momentKeyframes, newArray(std::move(moments)));
    return s;
}

// ---------------------------------------------------------------------------------------------
// Compiled form used for playback

struct Pt {
    double x, v;
};
struct RtChannel {
    bool present = false;
    double value = 0;
    uint32_t colour = 0xFFFFFFFF;
    Value curve;  // curve struct, or undefined
    int index = -1, mode = 0;
    std::vector<std::string> messages;
    Value fn;
};
struct RtKey {
    double key = 0, length = 1;
    bool stretch = false, disabled = false;
    std::vector<RtChannel> channels;  // by channel number
    const RtChannel* ch(size_t c) const { return c < channels.size() && channels[c].present ? &channels[c] : nullptr; }
};
struct RtTrack {
    int type = TT_None, param = P_None, traits = 0, interpolation = 0;
    bool enabled = true, visible = true;
    std::vector<RtKey> keys;
    std::vector<RtTrack> tracks;
    std::vector<std::vector<Pt>> points;  // real/colour tracks: sampled points per channel
};
struct RtSeq {
    bool valid = false;
    int playback = 0, speedType = 0;
    double speed = 60, length = 0, ox = 0, oy = 0, volume = 1;
    std::vector<RtTrack> tracks;
    std::vector<RtKey> messages, moments;
};

// The span a key covers (for drawing and nesting): to the next key or the end when stretched.
double keySpan(const std::vector<RtKey>& keys, size_t i, double seqLength) {
    if (!keys[i].stretch) return keys[i].length;
    return i + 1 < keys.size() ? keys[i + 1].key - keys[i].key : seqLength - keys[i].key;
}
// GetKeyframeIndexAtFrame
int keyAt(const std::vector<RtKey>& keys, double head, double seqLength) {
    int n = static_cast<int>(keys.size());
    if (n == 0) return -1;
    auto len = [&](int i) {
        return keys[i].stretch ? (i + 1 < n ? keys[i + 1].key : 2147483647.0) : keys[i].length;
    };
    int start = 0, end = n, idx = (start + end) >> 1;
    while (idx != start) {
        if (keys[idx].key <= head && keys[idx].key + len(idx) > head) return idx;
        if (keys[idx].key > head) end = idx;
        else start = idx;
        idx = (start + end) >> 1;
    }
    if (keys[idx].key <= head && keys[idx].key + len(idx) > head) return idx;
    // a final key ending exactly at the end of the sequence still shows at the end
    if (idx == n - 1 && head == seqLength && keys[idx].key + len(idx) == seqLength) return idx;
    return -1;
}

// UpdateCachedPoints: a real/colour channel as points (plain keys hold their value over their
// length; curve keys add the curve's samples scaled over the key).
void buildPoints(RtTrack& t, double seqLength) {
    seqLength += 1;  // as YoYo's runner ("mirror new IDE length paradigm")
    bool colour = t.type == TT_Colour;
    size_t nch = colour ? 4 : 1;
    for (const RtKey& k : t.keys) nch = std::max(nch, k.channels.size());
    t.points.assign(nch, {});
    auto channelValue = [&](const RtChannel& c, size_t ch) {
        if (!colour) return c.value;
        uint32_t v = c.colour;
        uint32_t byte = ch == 0 ? v >> 24 : ch == 1 ? v : ch == 2 ? v >> 8 : v >> 16;
        return (byte & 0xFF) / 255.0;
    };
    for (size_t ch = 0; ch < nch; ch++) {
        std::vector<Pt>& pts = t.points[ch];
        for (size_t i = 0; i < t.keys.size(); i++) {
            const RtKey& k = t.keys[i];
            const RtChannel* c = k.ch(ch);
            if (!c) {
                c = k.ch(0);  // keys without per-channel data: use the first (a curve, or a colour)
                if (!c || (!colour && c->curve.isUndefined())) continue;
            }
            if (c->curve.isUndefined()) {
                double v = channelValue(*c, ch);
                pts.push_back({k.key, v});
                double endOff = colour ? 1 : 0;
                if (!k.stretch && k.length > 1) {
                    pts.push_back({k.key + k.length - endOff, v});
                } else if (k.stretch) {
                    if (i + 1 == t.keys.size()) {
                        if (seqLength - k.key > 1) pts.push_back({seqLength, v});
                    } else if (t.keys[i + 1].key > k.key + 1) {
                        pts.push_back({t.keys[i + 1].key - endOff, v});
                    }
                }
            } else {
                double keyLength = k.length;
                if (k.stretch) {
                    if (i + 1 == t.keys.size()) {
                        if (seqLength <= 0) continue;
                        keyLength = seqLength - k.key;
                    } else if (t.keys[i + 1].key > k.key) {
                        keyLength = t.keys[i + 1].key - k.key;
                    }
                }
                Value chans = get(c->curve, ids.channels);
                const std::vector<Value>& cl = items(chans);
                if (cl.empty()) continue;
                const Value& cc = cl[std::min(ch, cl.size() - 1)];
                for (auto& [sx, sv] : animCurveSamples(*gRt, cc)) pts.push_back({k.key + sx * keyLength, sv});
            }
        }
    }
}
// getCachedChannelVal: false when the channel has no points.
bool channelValue(const RtTrack& t, size_t ch, double head, double& out) {
    if (ch >= t.points.size() || t.points[ch].empty()) return false;
    const std::vector<Pt>& p = t.points[ch];
    size_t n = p.size();
    if (p[0].x > head) return out = p[0].v, true;
    if (p[n - 1].x < head) return out = p[n - 1].v, true;
    size_t start = 0, end = n, mid = (start + end) >> 1;
    while (mid != start) {
        if (p[mid].x > head) end = mid;
        else start = mid;
        mid = (start + end) >> 1;
    }
    if (t.interpolation == 0 || mid == n - 1) return out = p[mid].v, true;
    double span = p[mid + 1].x - p[mid].x, f = span > 0 ? (head - p[mid].x) / span : 0;
    out = p[mid + 1].v * f + p[mid].v * (1 - f);
    return true;
}

RtKey compileKey(const Value& ks, int type) {
    RtKey k;
    k.key = num(ks, ids.frame);
    k.length = num(ks, ids.length, 1);
    k.stretch = get(ks, ids.stretch).truthy();
    k.disabled = get(ks, ids.disabled).truthy();
    const std::vector<Value>& chans = items(get(ks, ids.channels));
    k.channels.resize(chans.size());
    for (size_t i = 0; i < chans.size(); i++) {
        const Value& cs = chans[i];
        if (!cs.obj()) continue;
        RtChannel& c = k.channels[i];
        c.present = true;
        switch (type) {
            case TT_Graphic: c.index = static_cast<int>(num(cs, ids.spriteIndex, -1)); break;
            case TT_Instance: c.index = static_cast<int>(num(cs, ids.objectIndex, -1)); break;
            case TT_Sequence: c.index = static_cast<int>(num(cs, ids.sequence, -1)); break;
            case TT_Particle: c.index = static_cast<int>(num(cs, ids.particleSystemIndex, -1)); break;
            case TT_SpriteFrames: c.index = static_cast<int>(num(cs, ids.imageIndex, 0)); break;
            case TT_Audio:
                c.index = static_cast<int>(num(cs, ids.soundIndex, -1));
                c.mode = static_cast<int>(num(cs, ids.mode));
                break;
            case TT_Real:
            case TT_Colour: {
                if (type == TT_Real) c.value = num(cs, ids.value);
                else c.colour = static_cast<uint32_t>(static_cast<int64_t>(num(cs, ids.colour, 4294967295.0)));
                Value cv = get(cs, ids.curve);
                if (cv.obj()) c.curve = cv;
                else if (cv.isNumeric() && cv.toInt() >= 0) c.curve = animCurveArg(*gRt, cv);
                break;
            }
            case TT_Message:
                for (const Value& m : items(get(cs, ids.events))) c.messages.push_back(m.toString());
                break;
            case TT_Moment: c.fn = get(cs, ids.event); break;
            default: break;
        }
    }
    return k;
}
RtTrack compileTrack(const Value& ts, double seqLength) {
    RtTrack t;
    t.type = static_cast<int>(num(ts, ids.type));
    t.param = paramOfName(get(ts, ids.name).toString());
    t.traits = static_cast<int>(num(ts, ids.traits));
    Value en = get(ts, ids.enabled), vis = get(ts, ids.visible);
    t.enabled = en.isUndefined() || en.truthy();
    t.visible = vis.isUndefined() || vis.truthy();
    t.interpolation = static_cast<int>(num(ts, ids.interpolation));
    for (const Value& k : items(get(ts, ids.keyframes))) t.keys.push_back(compileKey(k, t.type));
    for (const Value& c : items(get(ts, ids.tracks))) t.tracks.push_back(compileTrack(c, seqLength));
    if (t.type == TT_Real || t.type == TT_Colour) buildPoints(t, seqLength);
    return t;
}
RtSeq compileSequence(const Value& s) {
    RtSeq q;
    if (!s.obj()) return q;
    q.valid = true;
    q.playback = static_cast<int>(num(s, ids.loopmode));
    q.speed = num(s, ids.playbackSpeed, 60);
    q.speedType = static_cast<int>(num(s, ids.playbackSpeedType));
    q.length = num(s, ids.length);
    q.volume = num(s, ids.volume, 1);
    q.ox = num(s, ids.xorigin);
    q.oy = num(s, ids.yorigin);
    for (const Value& t : items(get(s, ids.tracks))) q.tracks.push_back(compileTrack(t, q.length));
    for (const Value& k : items(get(s, ids.messageEventKeyframes))) q.messages.push_back(compileKey(k, TT_Message));
    for (const Value& k : items(get(s, ids.momentKeyframes))) q.moments.push_back(compileKey(k, TT_Moment));
    return q;
}

// HandleSequenceWrapping: true when the head left the sequence (looped, bounced or finished).
bool wrapHead(const RtSeq& s, double& head, double& dir, bool& finished) {
    double end = s.length;
    auto fwrap = [](double v, double d) { return d > 0 ? (v / d - std::floor(v / d)) * d : v; };
    if (s.playback == 0) {
        if (head <= 0) {
            head = 0;
            if (dir < 0) return finished = true, true;
        } else if (head >= end) {
            head = end;
            if (dir > 0) return finished = true, true;
        }
        return false;
    }
    if (head >= 0 && head < end) return false;
    if (s.playback == 1) {
        if (end > 0) head = fwrap(head, end);
        return true;
    }
    if (head < 0) head = -head;
    double loops = end > 0 ? head / end : 0;
    head = fwrap(head, end);
    if (static_cast<int64_t>(loops) & 1) {
        head = end - head;
        dir = -1;
    } else {
        dir = 1;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Evaluation

struct Affine {  // x' = a x + c y + tx, y' = b x + d y + ty (GameMaker's row-vector matrices)
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    static Affine translate(double x, double y) { return {1, 0, 0, 1, x, y}; }
    static Affine scale(double x, double y) { return {x, 0, 0, y, 0, 0}; }
    static Affine rotate(double deg) {
        double r = deg * M_PI / 180.0, cs = std::cos(r), sn = std::sin(r);
        return {cs, -sn, sn, cs, 0, 0};
    }
    Affine then(const Affine& o) const {  // this transform, then o
        return {a * o.a + b * o.c, a * o.b + b * o.d, c * o.a + d * o.c,
                c * o.b + d * o.d, tx * o.a + ty * o.c + o.tx, tx * o.b + ty * o.d + o.ty};
    }
};

// A track's evaluated values (YoYo's TrackEval).
struct Eval {
    double x = 0, y = 0, rot = 0, sx = 1, sy = 1, ox = 0, oy = 0;
    double mul[4] = {1, 1, 1, 1}, add[4] = {0, 0, 0, 0};
    double imageIndex = 0, imageSpeed = 1, gain = 1, pitch = 1;
    uint64_t set = 0;
    bool has(int p) const { return (set >> p) & 1; }
};

void evaluateParams(const RtTrack& track, double head, Eval& ev) {
    auto value = [&](const RtTrack& t, size_t ch, double def) {
        double v;
        return channelValue(t, ch, head, v) ? v : def;
    };
    for (const RtTrack& t : track.tracks) {
        if (!isParameter(t.type) || !t.enabled || t.param == P_None) continue;
        switch (t.param) {
            case P_Rotation: ev.rot = value(t, 0, 0); break;
            case P_Position:
                ev.x = value(t, 0, 0);
                ev.y = value(t, 1, 0);
                break;
            case P_Scale:
                ev.sx = value(t, 0, 1);
                ev.sy = value(t, 1, 1);
                break;
            case P_Origin:
                ev.ox = value(t, 0, 0);
                ev.oy = value(t, 1, 0);
                break;
            case P_BlendMultiply:
            case P_BlendAdd: {
                double* dst = t.param == P_BlendMultiply ? ev.mul : ev.add;
                double a, r, g, b;
                if (channelValue(t, 0, head, a) && channelValue(t, 1, head, r) && channelValue(t, 2, head, g) &&
                    channelValue(t, 3, head, b)) {
                    // packed to 8 bits per channel and back, as GameMaker does
                    auto q = [](double v) { return std::floor(std::min(v * 255.0, 255.0)) / 255.0; };
                    dst[0] = q(r), dst[1] = q(g), dst[2] = q(b), dst[3] = q(a);
                } else {
                    dst[0] = dst[1] = dst[2] = dst[3] = 1;
                }
                break;
            }
            case P_Gain: ev.gain = value(t, 0, 1); break;
            case P_Pitch: ev.pitch = value(t, 0, 1); break;
            case P_ImageSpeed: ev.imageSpeed = value(t, 0, ev.imageSpeed); break;
            case P_ImageIndex: ev.imageIndex = value(t, 0, ev.imageIndex); break;
            default: continue;
        }
        ev.set |= uint64_t(1) << t.param;
    }
}

uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xBF58476D1CE4E5B9ull;
}

struct TrackInstance {
    int32_t instance = -1;
    bool owned = true;
};
struct TrackAudio {
    int voice = -1;
    double dir = 1;
};

// A sequence element's playback (YoYo's CSequenceInstance).
struct SeqInstance {
    int32_t element = -1, sequence = -1;
    double head = 0, lastHead = 0, dir = 1, speedScale = 1, volume = 1;
    bool paused = false, finished = false, hasPlayed = false, wrapped = false, evaluated = false;
    // dirty: element properties changed since the last evaluation (layer_sequence_x & co.)
    bool dirtyPos = false, dirtyRot = false, dirtyScale = false, dirtyHead = false;
    std::unordered_map<uint64_t, TrackInstance> instances;
    std::unordered_map<uint64_t, TrackAudio> audio;
    struct TrackParticles {
        int system = -1, key = -1;
    };
    std::unordered_map<uint64_t, TrackParticles> particles;  // particle tracks' systems
    Value gml;  // the sequence instance struct, created when the game asks for it
    struct Synced {
        double head, dir, speedScale, volume;
        bool paused;
        Value sequence;
    } synced{};
};

}  // namespace

struct SequenceStore::Impl {
    Runner& rt;
    explicit Impl(Runner& r) : rt(r) {}
    std::vector<Value> structs;      // sequence structs by id (assets first, then sequence_create)
    std::vector<RtSeq> compiled;
    std::vector<bool> exposed;       // handed to GML: recompile from the structs every step
    std::vector<uint64_t> compiledStep;
    std::map<int32_t, SeqInstance> instances;  // by element id
    struct Message {
        std::string text;
        int32_t element;
    };
    std::vector<Message> messages;
    std::vector<std::pair<Value, int32_t>> moments;  // function, element

    size_t count() const { return std::max(structs.size(), rt.dw.sequences.size()); }
    bool exists(int id) {
        if (id < 0 || static_cast<size_t>(id) >= count()) return false;
        return !structOf(id).isUndefined();
    }
    Value structOf(int id) {
        if (id < 0) return Value();
        size_t n = count();
        if (static_cast<size_t>(id) >= n) return Value();
        if (structs.size() < n) structs.resize(n), compiled.resize(n), exposed.resize(n), compiledStep.resize(n);
        if (structs[id].isUndefined() && static_cast<size_t>(id) < rt.dw.sequences.size() &&
            !rt.dw.sequences[id].name.empty())
            structs[id] = sequenceStruct(rt.dw.sequences[id], rt.dw);
        return structs[id];
    }
    // A sequence given as id or struct; the struct marks it as visible to GML.
    int idOf(const Value& v) {
        if (GMObject* o = v.obj()) {
            for (size_t i = 0; i < structs.size(); i++)
                if (structs[i].obj() == o) return static_cast<int>(i);
            return -1;
        }
        return v.isNumeric() ? v.toInt() : -1;
    }
    Value expose(int id) {
        Value s = structOf(id);
        if (!s.isUndefined()) exposed[id] = true;
        return s;
    }
    const RtSeq* seq(int id) {
        Value s = structOf(id);
        if (s.isUndefined()) return nullptr;
        if (!compiled[id].valid || (exposed[id] && compiledStep[id] != rt.stepCount)) {
            compiled[id] = compileSequence(s);
            compiledStep[id] = rt.stepCount;
        }
        return &compiled[id];
    }
    int add(Value s) {
        size_t n = count();
        structOf(0);  // sizes the tables
        structs.resize(n + 1), compiled.resize(n + 1), exposed.resize(n + 1, true), compiledStep.resize(n + 1);
        structs[n] = std::move(s);
        exposed[n] = true;
        return static_cast<int>(n);
    }

    SeqInstance* inst(int32_t element) {
        auto it = instances.find(element);
        return it == instances.end() ? nullptr : &it->second;
    }

    // --- instance tracks: SetupInstances (on first play) ---
    void setupInstances(SeqInstance& si, const RtSeq& s, const std::vector<RtTrack>& tracks, uint64_t path,
                        int32_t layer, int depth) {
        if (depth > 16) return;
        for (size_t i = 0; i < tracks.size(); i++) {
            const RtTrack& t = tracks[i];
            uint64_t tp = mix(path, i);
            if (t.type == TT_Instance) {
                for (size_t k = 0; k < t.keys.size(); k++)
                    for (size_t c = 0; c < t.keys[k].channels.size(); c++) {
                        const RtChannel* ch = t.keys[k].ch(c);
                        if (!ch || ch->index < 0) continue;
                        uint64_t key = mix(mix(tp, 0x10000 + k), 0x20000 + c);
                        if (si.instances.count(key)) continue;
                        if (Instance* in = createTrackInstance(si, ch->index, layer)) si.instances[key] = {in->id, true};
                    }
            } else if (t.type == TT_Sequence) {
                for (size_t k = 0; k < t.keys.size(); k++)
                    if (const RtChannel* ch = t.keys[k].ch(0))
                        if (const RtSeq* sub = seq(ch->index))
                            setupInstances(si, *sub, sub->tracks, mix(tp, 0x10000 + k), layer, depth + 1);
            }
            setupInstances(si, s, t.tracks, tp, layer, depth + 1);
        }
    }
    Instance* createTrackInstance(SeqInstance& si, int object, int32_t layer) {
        Instance* in = rt.createInstance(0, 0, object, -1, false);
        if (!in) return nullptr;
        if (Layer* L = rt.layerById(layer)) rt.addInstanceToLayer(in, *L);
        in->seqElement = si.element;
        in->inSequence = true;
        in->seqControlled = true;
        rt.performEvent(in, EV_PRECREATE, 0);
        if (!in->destroyed) rt.performEvent(in, EV_CREATE, 0);
        return in;
    }

    // --- the track walk, shared by update and draw ---
    enum Mode { Update, Draw };
    struct Walk {
        Mode mode;
        SeqInstance& si;
        const LayerElement& el;
        const Layer& layer;
    };

    void walk(Walk& w, const RtSeq& s, const std::vector<RtTrack>& tracks, double head, double lastHead, double dir,
              const Affine& world, const Eval* parent, int parentTraits, uint64_t path, int depth) {
        if (depth > 16) return;
        for (size_t i = 0; i < tracks.size(); i++) {
            const RtTrack& t = tracks[i];
            if (isParameter(t.type) || !t.enabled) continue;
            if (w.mode == Draw && (!t.visible || t.type == TT_ClipMaskMask || t.type == TT_ClipMaskSubject)) continue;
            uint64_t tp = mix(path, i);
            Eval ev;
            evaluateParams(t, head, ev);
            // source origin: graphic tracks add their sprite's origin, sequence tracks the
            // nested sequence's
            int sprite = -1, gkey = -1;
            if (t.type == TT_Graphic) {
                gkey = keyAt(t.keys, head, s.length);
                if (gkey >= 0)
                    if (const RtChannel* c = t.keys[gkey].ch(0)) sprite = c->index;
                if (sprite >= 0 && static_cast<size_t>(sprite) < rt.dw.sprites.size()) {
                    ev.ox += rt.dw.sprites[sprite].originX;
                    ev.oy += rt.dw.sprites[sprite].originY;
                } else {
                    sprite = -1;
                }
            } else if (t.type == TT_Sequence) {
                forSubSequences(t, s, head, [&](size_t, const RtSeq& sub, double, double) {
                    ev.ox += sub.ox;
                    ev.oy += sub.oy;
                });
            }
            Affine local = Affine::translate(-ev.ox, -ev.oy)
                               .then(Affine::scale(ev.sx, ev.sy))
                               .then(Affine::rotate(ev.rot))
                               .then(Affine::translate(ev.x, ev.y));
            if (parent) {
                if (!(parentTraits & kTraitChildrenIgnoreOrigin)) local = local.then(Affine::translate(parent->ox, parent->oy));
                uint64_t parentOnly = parent->set & ~ev.set;
                ev.set |= parent->set;
                auto inherit = [&](int p, auto&& fromParent, auto&& combine) {
                    if (!ev.has(p)) return;
                    if ((parentOnly >> p) & 1) fromParent();
                    else combine();
                };
                inherit(P_BlendMultiply, [&] { std::copy(parent->mul, parent->mul + 4, ev.mul); },
                        [&] { for (int k = 0; k < 4; k++) ev.mul[k] *= parent->mul[k]; });
                inherit(P_BlendAdd, [&] { std::copy(parent->add, parent->add + 4, ev.add); },
                        [&] { for (int k = 0; k < 4; k++) ev.add[k] += parent->add[k]; });
                inherit(P_Gain, [&] { ev.gain = parent->gain; }, [&] { ev.gain *= parent->gain; });
                inherit(P_Pitch, [&] { ev.pitch = parent->pitch; }, [&] { ev.pitch *= parent->pitch; });
                inherit(P_ImageSpeed, [&] { ev.imageSpeed = parent->imageSpeed; }, [&] { ev.imageSpeed *= parent->imageSpeed; });
                inherit(P_ImageIndex, [&] { ev.imageIndex = parent->imageIndex; }, [&] { ev.imageIndex += parent->imageIndex; });
            }
            Affine m = local.then(world);
            switch (t.type) {
                case TT_Graphic:
                    if (w.mode == Draw && sprite >= 0) drawGraphic(w, s, t, gkey, sprite, ev, m, head);
                    break;
                case TT_Sequence:
                    forSubSequences(t, s, head, [&](size_t k, const RtSeq& sub, double key, double len) {
                        // head positions relative to the key, scaled by the relative playback speeds
                        double sh = std::min(std::max(head, key), key + len) - key;
                        double slh = std::min(std::max(lastHead, key), key + len) - key;
                        double ts = 1;
                        if (s.speed != 0) ts = s.speedType == 0 ? sub.speed / s.speed : sub.speed;
                        sh *= ts;
                        slh *= ts;
                        double sdir = lastHead > head ? -1 : 1;
                        bool fin = false;
                        wrapHead(sub, sh, sdir, fin);
                        walk(w, sub, sub.tracks, sh, slh, sdir, m, &ev, t.traits, mix(tp, 0x10000 + k), depth + 1);
                    });
                    break;
                case TT_Instance: instanceTrack(w, s, t, ev, m, head, tp); break;
                case TT_Particle: particleTrack(w, s, t, ev, m, head, tp); break;
                case TT_Audio:
                    if (w.mode == Update) audioTrack(w, s, t, ev, m, head, lastHead, dir, tp);
                    break;
                default: break;
            }
            if (!t.tracks.empty()) walk(w, s, t.tracks, head, lastHead, dir, m, &ev, t.traits, tp, depth + 1);
        }
    }

    // The keys of a sequence track whose span holds the head (by whole frames, as YoYo's runner).
    template <typename F> void forSubSequences(const RtTrack& t, const RtSeq& s, double head, F&& f) {
        double fh = std::floor(head);
        for (size_t k = 0; k < t.keys.size(); k++) {
            const RtKey& key = t.keys[k];
            double len = keySpan(t.keys, k, s.length);
            if (fh < key.key || fh >= key.key + len) continue;
            const RtChannel* c = key.ch(0);
            if (!c) continue;
            if (const RtSeq* sub = seq(c->index)) f(k, *sub, key.key, len);
        }
    }

    // HandleParticleTrackUpdate / HandleSequenceParticle: the key under the head has a system of
    // its own, made from the asset when the key starts and destroyed when another key (or none)
    // takes over. The track steps it, bursts its emitters again when the sequence loops, and draws
    // it in the track's space.
    void particleTrack(Walk& w, const RtSeq& s, const RtTrack& t, const Eval& ev, const Affine& m, double head,
                       uint64_t tp) {
        ParticleStore* P = rt.particles.get();
        if (!P) return;
        int k = keyAt(t.keys, head, s.length);
        int asset = -1;
        if (k >= 0)
            if (const RtChannel* c = t.keys[k].ch(0)) asset = c->index;
        auto it = w.si.particles.find(tp);
        if (w.mode == Update) {
            if (it != w.si.particles.end() && it->second.system >= 0 && it->second.key != k) {
                P->destroy(it->second.system);
                it->second.system = -1;
            }
            if (asset < 0) return;
            SeqInstance::TrackParticles& tpart = w.si.particles[tp];
            if (tpart.system < 0) {
                tpart.system = P->instantiate(asset);
                if (ParticleSystem* sys = P->system(tpart.system)) sys->autoDraw = sys->autoUpdate = false;
            } else if (w.si.wrapped) {
                if (ParticleSystem* sys = P->system(tpart.system))
                    for (size_t i = 0; i < sys->emitters.size(); i++) {
                        ParticleEmitter& e = sys->emitters[i];
                        if (e.created && e.enabled && e.burstMode && e.delayMin == 0 && e.delayMax == 0)
                            P->burst(tpart.system, static_cast<int>(i), e.partType, e.number);
                    }
            }
            tpart.key = k;
            P->updateSystem(tpart.system);
            return;
        }
        if (it == w.si.particles.end() || it->second.system < 0) return;
        uint32_t blend = w.el.blend;
        auto channel = [&](int c, int shift) {
            double v = (ev.mul[c] + ev.add[c]) * ((blend >> shift) & 0xFF);
            return static_cast<uint32_t>(std::max(0.0, std::min(255.0, v)));
        };
        uint32_t col = channel(0, 0) | channel(1, 8) << 8 | channel(2, 16) << 16;
        double a = std::max(0.0, std::min(1.0, (ev.mul[3] + ev.add[3]) * w.el.alpha));
        // the track's space, then whatever world matrix is in effect
        float local[16] = {static_cast<float>(m.a), static_cast<float>(m.b), 0, 0, static_cast<float>(m.c),
                           static_cast<float>(m.d), 0, 0, 0, 0, 1, 0, static_cast<float>(m.tx), static_cast<float>(m.ty), 0, 1};
        const float* outer = rt.gfx->renderer().worldMatrix();
        float world[16];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                world[i * 4 + j] = local[i * 4] * outer[j] + local[i * 4 + 1] * outer[4 + j] +
                                   local[i * 4 + 2] * outer[8 + j] + local[i * 4 + 3] * outer[12 + j];
        P->draw(it->second.system, world, col, a);
    }
    void destroyParticles(SeqInstance& si) {
        if (rt.particles)
            for (auto& [key, tpart] : si.particles) rt.particles->destroy(tpart.system);
        si.particles.clear();
    }

    // HandleSequenceGraphic
    void drawGraphic(Walk& w, const RtSeq& s, const RtTrack& t, int gkey, int sprite, const Eval& ev, const Affine& m,
                     double head) {
        const Sprite& spr = rt.dw.sprites[sprite];
        double index = 0;
        int n = static_cast<int>(spr.frames.size());
        if (n > 1) {
            const RtKey& k = t.keys[gkey];
            double span = k.stretch ? keySpan(t.keys, gkey, s.length) - 1 : k.length;
            double rel = std::min(std::max(head, k.key), k.key + span) - k.key;
            // the sprite's own speed against the sequence's
            double ts = 1;
            if (s.speed != 0) {
                double fps = rt.roomSpeed > 0 ? rt.roomSpeed : 60;
                ts = spr.playbackSpeedType == 0 ? spr.playbackSpeed / s.speed : spr.playbackSpeed * (fps / s.speed);
            }
            rel *= ts;
            if (ev.has(P_ImageSpeed)) rel *= ev.imageSpeed;
            if (ev.has(P_ImageIndex)) {
                index = std::min(std::max(ev.imageIndex, 0.0), static_cast<double>(n - 1));
            } else {
                index = std::fmod(rel, n);
                if (index < 0) index += n;
            }
        }
        uint32_t blend = w.el.blend;
        auto chan = [&](int k, int shift) {
            return std::max(0.0, std::min(255.0, (ev.mul[k] + ev.add[k]) * ((blend >> shift) & 0xFF)));
        };
        uint32_t col = static_cast<uint32_t>(chan(0, 0)) | static_cast<uint32_t>(chan(1, 8)) << 8 |
                       static_cast<uint32_t>(chan(2, 16)) << 16;
        double a = std::max(0.0, std::min(1.0, (ev.mul[3] + ev.add[3]) * w.el.alpha));
        const double mat[6] = {m.a, m.b, m.c, m.d, m.tx, m.ty};
        rt.gfx->drawSpriteAffine(sprite, index, mat, col, a);
    }

    // HandleInstanceTrackUpdate (update) and HandleSequenceInstance (draw)
    void instanceTrack(Walk& w, const RtSeq& s, const RtTrack& t, const Eval& ev, const Affine& m, double head,
                       uint64_t tp) {
        int active = keyAt(t.keys, head, s.length);
        // rotation, scale and position from the world matrix
        double rot = std::atan2(m.b, m.a) * -(180.0 / M_PI);
        double sx = std::sqrt(m.a * m.a + m.b * m.b), sy = std::sqrt(m.c * m.c + m.d * m.d);
        if (m.a * m.d - m.b * m.c < 0) {
            if (std::fabs(rot - ev.rot) > 0.0001) sx = -sx, rot -= 180.0;
            else sy = -sy;
        }
        for (size_t k = 0; k < t.keys.size(); k++)
            for (size_t c = 0; c < t.keys[k].channels.size(); c++) {
                auto it = w.si.instances.find(mix(mix(tp, 0x10000 + k), 0x20000 + c));
                if (it == w.si.instances.end()) continue;
                Instance* in = rt.findById(it->second.instance);
                if (!in || in->destroyed) continue;
                if (w.mode == Draw) {
                    if (static_cast<int>(k) != active || !in->drawnBySequence || !in->visible || !in->active) continue;
                    if (!rt.performEvent(in, EV_DRAW, DRAW_NORMAL)) rt.gfx->drawSelf(in);
                    continue;
                }
                in->inSequence = true;
                in->seqControlled = true;
                in->seqElement = w.si.element;
                if (static_cast<int>(k) != active) {
                    in->visible = false;
                    continue;
                }
                in->visible = true;
                const LayerElement& el = w.el;
                if (ev.has(P_Position) || w.si.dirtyPos || w.layer.x != 0 || w.layer.y != 0) in->x = m.tx, in->y = m.ty;
                if (ev.has(P_Rotation) || w.si.dirtyRot) in->imageAngle = rot;
                if (ev.has(P_Scale) || w.si.dirtyScale) in->imageXscale = sx, in->imageYscale = sy;
                if (ev.has(P_BlendMultiply) || (el.blend & 0xFFFFFF) != 0xFFFFFF || el.alpha != 1) {
                    double r = (el.blend & 0xFF) / 255.0, g = ((el.blend >> 8) & 0xFF) / 255.0,
                           b = ((el.blend >> 16) & 0xFF) / 255.0;
                    in->imageBlend = (static_cast<uint32_t>(ev.mul[0] * r * 255) & 0xFF) |
                                     (static_cast<uint32_t>(ev.mul[1] * g * 255) & 0xFF) << 8 |
                                     (static_cast<uint32_t>(ev.mul[2] * b * 255) & 0xFF) << 16;
                    in->imageAlpha = ev.mul[3] * el.alpha;
                }
                if (ev.has(P_ImageSpeed)) in->imageSpeed = ev.imageSpeed;
                if (ev.has(P_ImageIndex)) in->imageIndex = ev.imageIndex;
            }
    }

    // HandleAudioTrackUpdate (without emitters: sequences' sounds play unpositioned)
    void audioTrack(Walk& w, const RtSeq& s, const RtTrack& t, const Eval& ev, const Affine&, double head,
                    double lastHead, double dir, uint64_t tp) {
        if (!rt.audio) return;
        int active = (w.si.finished || w.si.paused) ? -1 : keyAt(t.keys, head, s.length);
        for (size_t k = 0; k < t.keys.size(); k++)
            for (size_t c = 0; c < t.keys[k].channels.size(); c++) {
                const RtChannel* ch = t.keys[k].ch(c);
                if (!ch) continue;
                TrackAudio& a = w.si.audio[mix(mix(tp, 0x10000 + k), 0x20000 + c)];
                if (static_cast<int>(k) != active) {
                    if (a.voice >= 0) rt.audio->stop(a.voice), a.voice = -1;
                    continue;
                }
                if (a.voice >= 0 && (a.dir * dir < 0 || (head - lastHead) * a.dir < 0 || !rt.audio->isPlaying(a.voice)))
                    if (rt.audio->isPlaying(a.voice) || ch->mode == 1) rt.audio->stop(a.voice), a.voice = -1;
                double gain = ev.gain * w.si.volume * s.volume;
                if (a.voice < 0 && ch->index >= 0) {
                    a.dir = dir;
                    const RtKey& key = t.keys[k];
                    double fromStart = dir > 0 ? head - key.key : std::max(0.0, key.key + key.length - 1 - head);
                    double speed = s.speed * w.si.speedScale;
                    if (speed > 0) fromStart /= speed;
                    Audio::PlayParams p;
                    p.offset = fromStart;
                    p.gain = gain;
                    p.pitch = ev.pitch;
                    a.voice = rt.audio->play(ch->index, 1, ch->mode == 1, p);
                } else if (a.voice >= 0) {
                    rt.audio->setGain(a.voice, gain, 0);
                    rt.audio->setPitch(a.voice, ev.pitch);
                }
            }
    }

    // The element's own transform (EvaluateLayerSequenceElement): origin, scale, angle, position.
    Affine elementMatrix(const RtSeq& s, const LayerElement& el, const Layer& L) {
        return Affine::translate(-s.ox, -s.oy)
            .then(Affine::scale(el.xscale, el.yscale))
            .then(Affine::rotate(el.angle))
            .then(Affine::translate(el.x + L.x, el.y + L.y));
    }

    // Keys a head move from `last` to `head` passed (message and moment keyframes), in order.
    std::vector<size_t> passedKeys(const std::vector<RtKey>& keys, const RtSeq& s, double last, double head, double dir,
                                   bool wrapped) {
        std::vector<size_t> out;
        auto range = [&](double from, double to) {  // [from, to) forwards, (to, from] backwards
            for (size_t i = 0; i < keys.size(); i++) {
                double k = keys[i].key;
                if (from <= to ? (k >= from && k < to) : (k <= from && k > to)) out.push_back(i);
            }
            if (from > to) std::reverse(out.end() - static_cast<long>(std::min(out.size(), keys.size())), out.end());
        };
        if (!wrapped) {
            range(last, head + (dir > 0 && head >= s.length ? 1 : 0));
        } else if (dir > 0) {
            range(last, s.length + 1);
            range(0, head);
        } else {
            range(last, -1);
            range(s.length, head);
        }
        return out;
    }

    void syncFromGml(SeqInstance& si) {
        if (si.gml.isUndefined()) return;
        auto changed = [&](int32_t id, double old, double& dst) {
            Value v = get(si.gml, id);
            if (v.isNumeric() && v.num() != old) dst = v.num();
        };
        double head = si.head;
        changed(ids.headPosition, si.synced.head, head);
        if (head != si.head) si.head = si.lastHead = head;
        changed(ids.headDirection, si.synced.dir, si.dir);
        changed(ids.speedScale, si.synced.speedScale, si.speedScale);
        changed(ids.volume, si.synced.volume, si.volume);
        bool paused = get(si.gml, ids.paused).truthy();
        if (paused != si.synced.paused) si.paused = paused;
        Value sq = get(si.gml, ids.sequence);
        if (sq.obj() != si.synced.sequence.obj() || (!sq.obj() && sq.isNumeric() && sq.toInt() != si.sequence)) {
            int id = idOf(sq);
            if (exists(id)) si.sequence = id;
        }
    }
    void syncToGml(SeqInstance& si) {
        if (si.gml.isUndefined()) return;
        Value sq = expose(si.sequence);
        set(si.gml, ids.sequence, sq);
        set(si.gml, ids.headPosition, Value(si.head));
        set(si.gml, ids.headDirection, Value(si.dir));
        set(si.gml, ids.speedScale, Value(si.speedScale));
        set(si.gml, ids.volume, Value(si.volume));
        set(si.gml, ids.paused, Value::boolean(si.paused));
        set(si.gml, ids.finished, Value::boolean(si.finished));
        set(si.gml, ids.elementID, Value(si.element));
        si.synced = {si.head, si.dir, si.speedScale, si.volume, si.paused, sq};
    }
    Value gmlOf(SeqInstance& si) {
        if (si.gml.isUndefined()) {
            si.gml = newStruct();
            set(si.gml, ids.activeTracks, newArray({}));
            syncToGml(si);
        }
        return si.gml;
    }

    // Runs one of the sequence's own events (sequence.event_step = function () {...}), with the
    // sequence instance as self.
    void sequenceEvent(SeqInstance& si, int32_t eventId) {
        Value s = structOf(si.sequence);
        Value fn = get(s, eventId);
        if (!fn.method()) return;
        Value self = gmlOf(si);
        syncToGml(si);
        Value arg = self;
        rt.vm.callValue(fn, self.obj()->inst, self.obj()->inst, 1, &arg);
        syncFromGml(si);
    }

    void evaluate(SeqInstance& si, LayerElement& el, const Layer& L, bool advance) {
        const RtSeq* s = seq(si.sequence);
        if (!s) return;
        if (!si.hasPlayed) {
            setupInstances(si, *s, s->tracks, mix(0, static_cast<uint64_t>(si.sequence)), L.id, 0);
            si.hasPlayed = true;
            if (!(s = seq(si.sequence))) return;
        }
        si.wrapped = false;
        si.lastHead = si.head;
        if (advance && !si.paused && !si.finished) {
            double speed = s->speed;
            if (s->speedType == 0) speed /= (rt.roomSpeed > 0 ? rt.roomSpeed : 60);
            si.head += si.dir * si.speedScale * speed;
        }
        bool finished = si.finished;
        si.wrapped = wrapHead(*s, si.head, si.dir, finished);
        Walk w{Update, si, el, L};
        walk(w, *s, s->tracks, si.head, si.lastHead, si.dir, elementMatrix(*s, el, L), nullptr, 0,
             mix(0, static_cast<uint64_t>(si.sequence)), 0);
        if (advance && !si.paused && !si.finished) {
            double speed = s->speed * (si.speedScale != 0 ? si.speedScale : 1);
            double dir = speed < 0 ? -si.dir : si.dir;
            for (size_t k : passedKeys(s->messages, *s, si.lastHead, si.head, dir, si.wrapped))
                if (const RtChannel* c = s->messages[k].ch(0))
                    for (const std::string& m : c->messages) messages.push_back({m, si.element});
            for (size_t k : passedKeys(s->moments, *s, si.lastHead, si.head, dir, si.wrapped))
                if (const RtChannel* c = s->moments[k].ch(0))
                    if (c->fn.method()) moments.push_back({c->fn, si.element});
        }
        if (finished && !si.finished) {
            for (auto& [key, ti] : si.instances)
                if (Instance* in = rt.findById(ti.instance)) in->inSequence = false, in->seqElement = -1;
            if (rt.audio)
                for (auto& [key, a] : si.audio)
                    if (a.voice >= 0) rt.audio->stop(a.voice), a.voice = -1;
        }
        si.finished = finished;
        si.dirtyPos = si.dirtyRot = si.dirtyScale = si.dirtyHead = false;
        si.evaluated = true;
    }

    void broadcast(const Message& m) {
        int32_t map = rt.ds->maps.create();
        DsMap* dm = rt.ds->maps.get(map);
        auto put = [&](const char* k, Value v) {
            ValueKey key = ValueKey::of(Value(k));
            dm->order.push_back(key);
            dm->values[key] = std::move(v);
        };
        put("event_type", Value("sequence event"));
        put("element_id", Value(m.element));
        put("message", Value(m.text));
        int32_t saved = rt.eventData;
        rt.eventData = map;
        for (auto& [el, si] : instances) sequenceEvent(si, ids.event_broadcast_message);
        std::vector<Instance*> all = rt.instances;
        for (Instance* i : all)
            if (Runner::live(i) && rt.findHandler(i->object, EV_OTHER, OTHER_BROADCAST))
                rt.performEvent(i, EV_OTHER, OTHER_BROADCAST);
        rt.eventData = saved;
        rt.ds->maps.destroy(map);
    }

    std::vector<int32_t> roomElements() {
        std::vector<int32_t> out;
        for (const Layer& L : rt.layers)
            for (int32_t id : L.elements)
                if (const LayerElement* e = rt.elementById(id); e && e->type == ElementType::Sequence) out.push_back(id);
        return out;
    }
};

SequenceStore::SequenceStore(Runner& rt) : impl(std::make_unique<Impl>(rt)) {}
SequenceStore::~SequenceStore() = default;

int32_t SequenceStore::createElement(Layer& L, int32_t sequence, double x, double y) {
    if (!impl->exists(sequence)) return -1;
    LayerElement e;
    e.type = ElementType::Sequence;
    e.sprite = sequence;
    e.x = x;
    e.y = y;
    int32_t id = impl->rt.addLayerElement(L, std::move(e));
    SeqInstance& si = impl->instances[id];
    si.element = id;
    si.sequence = sequence;
    impl->sequenceEvent(si, ids.event_create);
    return id;
}

void SequenceStore::setElementStart(int32_t element, double headPosition, double speedScale) {
    if (SeqInstance* si = impl->inst(element)) {
        si->head = si->lastHead = headPosition;
        si->speedScale = speedScale;
    }
}

void SequenceStore::update() {
    Impl& m = *impl;
    m.messages.clear();
    m.moments.clear();
    for (int32_t id : m.roomElements()) {
        LayerElement* el = m.rt.elementById(id);
        SeqInstance* si = m.inst(id);
        const Layer* L = el ? m.rt.layerById(el->layer) : nullptr;
        if (!el || !si || !L) continue;
        m.syncFromGml(*si);
        bool dirty = si->dirtyPos || si->dirtyRot || si->dirtyScale || si->dirtyHead;
        if (!dirty && ((si->paused && si->hasPlayed) || si->finished)) continue;
        m.evaluate(*si, *el, *L, true);
        m.syncToGml(*si);
    }
    for (auto& msg : std::vector<Impl::Message>(m.messages)) m.broadcast(msg);
    for (auto& [fn, element] : std::vector<std::pair<Value, int32_t>>(m.moments))
        if (SeqInstance* si = m.inst(element)) {
            Value self = m.gmlOf(*si);
            Value arg = self;
            m.rt.vm.callValue(fn, self.obj()->inst, self.obj()->inst, 1, &arg);
            m.syncFromGml(*si);
        }
    stepEvent(STEP_NORMAL);
}

void SequenceStore::stepEvent(uint32_t subtype) {
    int32_t id = subtype == STEP_BEGIN ? ids.event_step_begin : subtype == STEP_END ? ids.event_step_end : ids.event_step;
    for (int32_t el : impl->roomElements())
        if (SeqInstance* si = impl->inst(el); si && !si->paused && !si->finished) impl->sequenceEvent(*si, id);
}

void SequenceStore::draw(const Layer& L, const LayerElement& e) {
    Impl& m = *impl;
    SeqInstance* si = m.inst(e.id);
    if (!si) return;
    // A sequence shows once it has been updated (its first step), or when moved since.
    if (si->dirtyPos || si->dirtyRot || si->dirtyScale || si->dirtyHead)
        m.evaluate(*si, const_cast<LayerElement&>(e), L, false);
    if (!si->evaluated) return;
    const RtSeq* s = m.seq(si->sequence);
    if (!s) return;
    Impl::Walk w{Impl::Draw, *si, e, L};
    m.walk(w, *s, s->tracks, si->head, si->lastHead, si->dir, m.elementMatrix(*s, e, L), nullptr, 0,
           mix(0, static_cast<uint64_t>(si->sequence)), 0);
}

void SequenceStore::elementRemoved(int32_t id) {
    Impl& m = *impl;
    auto it = m.instances.find(id);
    if (it == m.instances.end()) return;
    SeqInstance& si = it->second;
    m.sequenceEvent(si, ids.event_destroy);
    m.sequenceEvent(si, ids.event_clean_up);
    for (auto& [key, ti] : si.instances)
        if (Instance* in = m.rt.findById(ti.instance)) {
            in->inSequence = in->seqControlled = false;
            in->seqElement = -1;
            if (ti.owned && !in->destroyed) m.rt.destroyInstance(in, true);
        }
    if (m.rt.audio)
        for (auto& [key, a] : si.audio)
            if (a.voice >= 0) m.rt.audio->stop(a.voice);
    m.destroyParticles(si);
    m.instances.erase(id);
}

void SequenceStore::roomEnd(bool persistent) {
    if (persistent) return;
    for (int32_t id : impl->roomElements()) {
        auto it = impl->instances.find(id);
        if (it == impl->instances.end()) continue;
        if (impl->rt.audio)
            for (auto& [key, a] : it->second.audio)
                if (a.voice >= 0) impl->rt.audio->stop(a.voice);
        impl->destroyParticles(it->second);
        impl->instances.erase(it);
    }
}

Value sequenceInstanceStruct(Runner& rt, int32_t element) {
    if (element < 0 || !rt.sequences) return Value();
    SeqInstance* si = rt.sequences->impl->inst(element);
    return si ? rt.sequences->impl->gmlOf(*si) : Value();
}

// ---------------------------------------------------------------------------------------------
// GML

#define SQ (*rt.sequences->impl)

static SeqInstance* elementInst(Runner& rt, const Value& v, LayerElement** elOut = nullptr) {
    LayerElement* el = rt.elementById(v.toInt());
    if (!el || el->type != ElementType::Sequence) return nullptr;
    if (elOut) *elOut = el;
    return SQ.inst(el->id);
}

GMFN(layer_sequence_create) {  // (layer, x, y, sequence)
    Layer* L = rt.layerFromValue(ARG(0));
    if (!L) return Value(-1.0);
    int id = SQ.idOf(ARG(3));
    return Value(static_cast<double>(rt.sequences->createElement(*L, id, D(1), D(2))));
}
GMFN(layer_sequence_destroy) {
    if (elementInst(rt, ARG(0))) rt.removeLayerElement(I(0), false);
    return Value();
}
GMFN(layer_sequence_exists) {  // (layer, element)
    LayerElement* el = rt.elementById(I(1));
    Layer* L = rt.layerFromValue(ARG(0));
    return boolV(el && L && el->layer == L->id && el->type == ElementType::Sequence);
}
GMFN(layer_sequence_get_instance) {
    SeqInstance* si = elementInst(rt, ARG(0));
    return si ? SQ.gmlOf(*si) : Value(-1.0);
}
GMFN(layer_sequence_get_sequence) {
    SeqInstance* si = elementInst(rt, ARG(0));
    return si ? SQ.expose(si->sequence) : Value(-1.0);
}
GMFN(layer_sequence_get_length) {
    SeqInstance* si = elementInst(rt, ARG(0));
    const RtSeq* s = si ? SQ.seq(si->sequence) : nullptr;
    return Value(s ? s->length : -1.0);
}
GMFN(layer_sequence_headpos) {
    SeqInstance* si = elementInst(rt, ARG(0));
    const RtSeq* s = si ? SQ.seq(si->sequence) : nullptr;
    if (!s) return Value(-1.0);
    double pos = std::min(std::max(D(1), 0.0), s->length);
    if (si->head != pos || si->lastHead != pos) {
        si->head = si->lastHead = pos;
        si->dirtyHead = true;
    }
    return Value(-1.0);
}
GMFN(layer_sequence_get_headpos) {
    SeqInstance* si = elementInst(rt, ARG(0));
    return Value(si ? si->head : -1.0);
}
GMFN(layer_sequence_headdir) {
    if (SeqInstance* si = elementInst(rt, ARG(0)); si && D(1) != 0) si->dir = D(1) < 0 ? -1 : 1;
    return Value(-1.0);
}
GMFN(layer_sequence_get_headdir) {
    SeqInstance* si = elementInst(rt, ARG(0));
    return Value(si ? si->dir : -1.0);
}
GMFN(layer_sequence_pause) {
    if (SeqInstance* si = elementInst(rt, ARG(0))) si->paused = true;
    return Value(-1.0);
}
GMFN(layer_sequence_play) {
    if (SeqInstance* si = elementInst(rt, ARG(0))) {
        si->paused = false;
        if (si->finished) {
            const RtSeq* s = SQ.seq(si->sequence);
            si->head = si->lastHead = si->dir < 0 && s ? s->length - 1 : 0;
            si->finished = false;
        }
    }
    return Value(-1.0);
}
GMFN(layer_sequence_is_paused) {
    SeqInstance* si = elementInst(rt, ARG(0));
    return boolV(si && si->paused);
}
GMFN(layer_sequence_is_finished) {
    SeqInstance* si = elementInst(rt, ARG(0));
    return boolV(si && si->finished);
}
GMFN(layer_sequence_speedscale) {
    if (SeqInstance* si = elementInst(rt, ARG(0))) si->speedScale = D(1);
    return Value(-1.0);
}
GMFN(layer_sequence_get_speedscale) {
    SeqInstance* si = elementInst(rt, ARG(0));
    return Value(si ? si->speedScale : -1.0);
}

#define SEQ_ELEMENT_PROP(getName, setName, field, dirty)        \
    GMFN(setName) {                                              \
        LayerElement* el = nullptr;                              \
        if (SeqInstance* si = elementInst(rt, ARG(0), &el)) {    \
            el->field = D(1);                                    \
            si->dirty = true;                                    \
        }                                                        \
        return Value(-1.0);                                      \
    }                                                            \
    GMFN(getName) {                                              \
        LayerElement* el = nullptr;                              \
        return Value(elementInst(rt, ARG(0), &el) ? el->field : -1.0); \
    }
SEQ_ELEMENT_PROP(layer_sequence_get_x, layer_sequence_x, x, dirtyPos)
SEQ_ELEMENT_PROP(layer_sequence_get_y, layer_sequence_y, y, dirtyPos)
SEQ_ELEMENT_PROP(layer_sequence_get_angle, layer_sequence_angle, angle, dirtyRot)
SEQ_ELEMENT_PROP(layer_sequence_get_xscale, layer_sequence_xscale, xscale, dirtyScale)
SEQ_ELEMENT_PROP(layer_sequence_get_yscale, layer_sequence_yscale, yscale, dirtyScale)
#undef SEQ_ELEMENT_PROP
GMFN(layer_sequence_blend) {
    LayerElement* el = nullptr;
    if (SeqInstance* si = elementInst(rt, ARG(0), &el)) el->blend = C(1), si->dirtyPos = true;
    return Value(-1.0);
}
GMFN(layer_sequence_get_blend) {
    LayerElement* el = nullptr;
    return Value(elementInst(rt, ARG(0), &el) ? static_cast<double>(el->blend) : -1.0);
}
GMFN(layer_sequence_alpha) {
    LayerElement* el = nullptr;
    if (SeqInstance* si = elementInst(rt, ARG(0), &el)) el->alpha = D(1), si->dirtyPos = true;
    return Value(-1.0);
}
GMFN(layer_sequence_get_alpha) {
    LayerElement* el = nullptr;
    return Value(elementInst(rt, ARG(0), &el) ? el->alpha : -1.0);
}

GMFN(sequence_exists) { return boolV(SQ.exists(SQ.idOf(ARG(0)))); }
GMFN(sequence_get) { return SQ.expose(SQ.idOf(ARG(0))); }
GMFN(sequence_get_name) {
    Value s = SQ.structOf(SQ.idOf(ARG(0)));
    return s.isUndefined() ? Value("") : get(s, ids.name);
}
GMFN(sequence_create) {
    Value s = newStruct();
    set(s, ids.name, Value(""));
    set(s, ids.loopmode, Value(0.0));
    set(s, ids.playbackSpeed, Value(60.0));
    set(s, ids.playbackSpeedType, Value(0.0));
    set(s, ids.length, Value(60.0));
    set(s, ids.volume, Value(1.0));
    set(s, ids.xorigin, Value(0.0));
    set(s, ids.yorigin, Value(0.0));
    set(s, ids.tracks, newArray({}));
    set(s, ids.messageEventKeyframes, newArray({}));
    set(s, ids.momentKeyframes, newArray({}));
    SQ.add(s);
    return s;
}
GMFN(sequence_destroy) {
    int id = SQ.idOf(ARG(0));
    if (id >= static_cast<int>(rt.dw.sequences.size()) && static_cast<size_t>(id) < SQ.structs.size())
        SQ.structs[id] = Value(), SQ.compiled[id] = RtSeq();
    return Value();
}
GMFN(sequence_track_new) {  // (type)
    Value s = newStruct();
    set(s, ids.name, Value(""));
    set(s, ids.type, Value(I(0)));
    set(s, ids.traits, Value(0.0));
    set(s, ids.enabled, Value::boolean(true));
    set(s, ids.visible, Value::boolean(true));
    set(s, ids.tracks, newArray({}));
    set(s, ids.keyframes, newArray({}));
    if (I(0) == TT_Real || I(0) == TT_Colour) set(s, ids.interpolation, Value(1.0));
    return s;
}
GMFN(sequence_keyframe_new) {  // (type)
    GM_UNUSED_FN_ARGS;
    Value s = newStruct();
    set(s, ids.frame, Value(0.0));
    set(s, ids.length, Value(1.0));
    set(s, ids.stretch, Value::boolean(false));
    set(s, ids.disabled, Value::boolean(false));
    set(s, ids.channels, newArray({}));
    return s;
}
GMFN(sequence_keyframedata_new) {  // (type)
    Value s = newStruct();
    set(s, ids.channel, Value(0.0));
    switch (I(0)) {
        case TT_Graphic: set(s, ids.spriteIndex, Value(-1.0)); break;
        case TT_Instance: set(s, ids.objectIndex, Value(-1.0)); break;
        case TT_Sequence: set(s, ids.sequence, Value(-1.0)); break;
        case TT_Particle: set(s, ids.particleSystemIndex, Value(-1.0)); break;
        case TT_Audio: set(s, ids.soundIndex, Value(-1.0)), set(s, ids.mode, Value(0.0)); break;
        case TT_SpriteFrames: set(s, ids.imageIndex, Value(0.0)); break;
        case TT_Real: set(s, ids.value, Value(0.0)), set(s, ids.curve, Value(-1.0)); break;
        case TT_Colour:
            set(s, ids.colour, Value(4294967295.0));
            set(s, ids.color, Value(4294967295.0));
            set(s, ids.curve, Value(-1.0));
            break;
        default: break;
    }
    return s;
}
GMFN(sequence_keyframestore_new) {  // (type): a keyframe store is an array of keyframes
    GM_UNUSED_FN_ARGS;
    return newArray({});
}
GMFN(sequence_instance_exists) {
    LayerElement* el = nullptr;
    if (GMObject* o = ARG(0).obj()) {
        for (auto& [id, si] : SQ.instances)
            if (si.gml.obj() == o) return Value::boolean(true);
        return Value::boolean(false);
    }
    return boolV(elementInst(rt, ARG(0), &el) != nullptr);
}
GMFN(sequence_get_objects) {  // the objects the sequence's instance tracks use
    const RtSeq* s = SQ.seq(SQ.idOf(ARG(0)));
    std::vector<Value> out;
    std::function<void(const std::vector<RtTrack>&)> scan = [&](const std::vector<RtTrack>& tracks) {
        for (const RtTrack& t : tracks) {
            if (t.type == TT_Instance)
                for (const RtKey& k : t.keys)
                    for (const RtChannel& c : k.channels)
                        if (c.present && c.index >= 0 &&
                            std::none_of(out.begin(), out.end(), [&](const Value& v) { return v.toInt() == c.index; }))
                            out.push_back(Value(c.index));
            scan(t.tracks);
        }
    };
    if (s) scan(s->tracks);
    return newArray(std::move(out));
}
GMFN(sequence_instance_override_object) {  // (sequence instance, object, instance or object)
    GM_UNUSED_FN_ARGS;
    rt.vm.warnOnce("sequence_instance_override_object", "sequence_instance_override_object is not supported");
    return Value();
}

void registerSequenceBuiltins(Runner& rt) {
    gRt = &rt;
    initIds(rt);
    GMREG(layer_sequence_create); GMREG(layer_sequence_destroy); GMREG(layer_sequence_exists);
    GMREG(layer_sequence_get_instance); GMREG(layer_sequence_get_sequence); GMREG(layer_sequence_get_length);
    GMREG(layer_sequence_headpos); GMREG(layer_sequence_get_headpos); GMREG(layer_sequence_headdir);
    GMREG(layer_sequence_get_headdir); GMREG(layer_sequence_pause); GMREG(layer_sequence_play);
    GMREG(layer_sequence_is_paused); GMREG(layer_sequence_is_finished); GMREG(layer_sequence_speedscale);
    GMREG(layer_sequence_get_speedscale); GMREG(layer_sequence_x); GMREG(layer_sequence_get_x);
    GMREG(layer_sequence_y); GMREG(layer_sequence_get_y); GMREG(layer_sequence_angle);
    GMREG(layer_sequence_get_angle); GMREG(layer_sequence_xscale); GMREG(layer_sequence_get_xscale);
    GMREG(layer_sequence_yscale); GMREG(layer_sequence_get_yscale); GMREG(layer_sequence_blend);
    GMREG(layer_sequence_get_blend); GMREG(layer_sequence_alpha); GMREG(layer_sequence_get_alpha);
    GMREG(sequence_exists); GMREG(sequence_get); GMREG(sequence_get_name); GMREG(sequence_create);
    GMREG(sequence_destroy); GMREG(sequence_track_new); GMREG(sequence_keyframe_new);
    GMREG(sequence_keyframedata_new); GMREG(sequence_instance_exists); GMREG(sequence_get_objects);
    GMREG(sequence_instance_override_object);
    GMREG(sequence_keyframestore_new);
}

}  // namespace gm
