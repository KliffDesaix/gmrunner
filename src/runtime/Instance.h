#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "vm/Value.h"

namespace gm {

// Open-addressing map from variable id to value. Instances only touch a few dozen of the
// game's thousands of variable ids, so this keeps per-instance memory small. Each slot remembers
// when its variable was added, for the struct functions that list fields in that order.
class VarMap {
public:
    Value* find(int32_t key) {
        if (slots_.empty()) return nullptr;
        size_t mask = slots_.size() - 1;
        for (size_t i = hash(key) & mask;; i = (i + 1) & mask) {
            if (slots_[i].key == key) return &slots_[i].v;
            if (slots_[i].key == kEmpty) return nullptr;
        }
    }
    Value& getOrCreate(int32_t key) {
        if ((count_ + 1) * 4 > slots_.size() * 3) grow();
        size_t mask = slots_.size() - 1;
        for (size_t i = hash(key) & mask;; i = (i + 1) & mask) {
            if (slots_[i].key == key) return slots_[i].v;
            if (slots_[i].key == kEmpty) {
                slots_[i].key = key;
                slots_[i].order = next_++;
                count_++;
                return slots_[i].v;
            }
        }
    }
    // Removes the variable, moving later slots of its probe run back into the gap.
    bool erase(int32_t key) {
        if (slots_.empty()) return false;
        size_t mask = slots_.size() - 1, i = hash(key) & mask;
        while (slots_[i].key != key) {
            if (slots_[i].key == kEmpty) return false;
            i = (i + 1) & mask;
        }
        for (size_t j = (i + 1) & mask; slots_[j].key != kEmpty; j = (j + 1) & mask) {
            size_t home = hash(slots_[j].key) & mask;
            // Slot j can fill the gap at i unless its home lies cyclically in (i, j].
            if (((j - home) & mask) >= ((j - i) & mask)) {
                slots_[i] = std::move(slots_[j]);
                i = j;
            }
        }
        slots_[i] = Slot{};
        count_--;
        return true;
    }
    void clear() {
        slots_.clear();
        count_ = 0;
    }
    template <typename F> void forEach(F&& f) const {
        for (auto& s : slots_)
            if (s.key != kEmpty) f(s.key, s.v);
    }
    // In the order the variables were added.
    template <typename F> void forEachInOrder(F&& f) const {
        std::vector<const Slot*> used;
        used.reserve(count_);
        for (auto& s : slots_)
            if (s.key != kEmpty) used.push_back(&s);
        std::sort(used.begin(), used.end(), [](const Slot* a, const Slot* b) { return a->order < b->order; });
        for (const Slot* s : used) f(s->key, s->v);
    }
    size_t size() const { return count_; }

private:
    static constexpr int32_t kEmpty = INT32_MIN;
    struct Slot {
        int32_t key = kEmpty;
        uint32_t order = 0;
        Value v;
    };
    static size_t hash(int32_t k) { return static_cast<uint32_t>(k) * 2654435761u >> 4; }
    void grow() {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.resize(old.empty() ? 16 : old.size() * 2);
        count_ = 0;
        size_t mask = slots_.size() - 1;
        for (auto& s : old) {
            if (s.key == kEmpty) continue;
            size_t i = hash(s.key) & mask;
            while (slots_[i].key != kEmpty) i = (i + 1) & mask;
            slots_[i] = std::move(s);
            count_++;
        }
    }
    std::vector<Slot> slots_;
    size_t count_ = 0;
    uint32_t next_ = 0;
};

struct Instance {
    int32_t id = 0;
    int32_t object = -1;
    bool destroyed = false;   // instance_destroy called; removed at end of frame
    bool active = true;       // false while deactivated
    bool visible = true, solid = false, persistent = false;
    bool mouseOver = false;  // the mouse was over it last step (Mouse Enter / Leave events)

    double x = 0, y = 0, xprevious = 0, yprevious = 0, xstart = 0, ystart = 0;
    double hspeed = 0, vspeed = 0, speed = 0, direction = 0;
    double friction = 0, gravity = 0, gravityDirection = 270;

    int32_t sprite = -1, mask = -1;
    double imageIndex = 0, imageSpeed = 1, imageXscale = 1, imageYscale = 1, imageAngle = 0, imageAlpha = 1;
    uint32_t imageBlend = 0xFFFFFF;
    double depth = 0;

    double alarm[12] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

    int32_t pathIndex = -1;
    double pathPosition = 0, pathPositionPrevious = 0, pathSpeed = 0, pathScale = 1, pathOrientation = 0;
    int32_t pathEndAction = 0;
    double pathXStart = 0, pathYStart = 0;  // where the path's first point is placed (path_xstart)

    int32_t timelineIndex = -1;
    double timelinePosition = 0, timelineSpeed = 1;
    bool timelineRunning = false, timelineLoop = false;

    uint64_t createOrder = 0;  // for stable ordering
    uint64_t createdStep = 0;  // step it was created in (alarms start counting the step after)
    GMObject* structObj = nullptr;  // set when this Instance is a GMS 2.3 struct's storage
    Instance* staticParent = nullptr;  // GMS 2.3: the constructor's static scope (variable fallback)
    int32_t layer = -1;             // GMS2 layer id
    int32_t layerElement = -1;      // its instance element on that layer
    std::string persistLayer;       // persistent instances: layer name to rejoin in the next room
    // Sequences: the sequence element whose instance track controls this instance (-1 when
    // none, or once it finished: sequence_instance), in_sequence, and whether it is drawn in
    // the sequence's track order instead of with its layer (controlled and drawn_by_sequence).
    int32_t seqElement = -1;
    bool inSequence = false, seqControlled = false, drawnBySequence = true;
    bool wasOutside = false;   // edge-triggers the Outside Room event
    VarMap vars;

    // GameMaker keeps speed/direction and hspeed/vspeed in sync.
    void setHVSpeed(double h, double v);
    void setSpeedDir(double s, double d);
    void addSpeed(double dir, double amount);  // motion_add / gravity
};

}  // namespace gm
