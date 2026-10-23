// Sequences (GMS 2.3+): playback of the sequence elements on layers. SequenceLib.cpp.
#pragma once

#include <cstdint>
#include <memory>

namespace gm {

class Runner;
struct Layer;
struct LayerElement;

struct SequenceStore {
    explicit SequenceStore(Runner& rt);
    ~SequenceStore();
    // A sequence element on a layer (layer_sequence_create and room asset layers); -1 if the
    // sequence doesn't exist.
    int32_t createElement(Layer& L, int32_t sequence, double x, double y);
    void setElementStart(int32_t element, double headPosition, double speedScale);  // room assets
    // Per step, right before the Step event (YoYo's UpdateInstancesForRoom): advances every
    // sequence in the room, moves the instances it controls, plays its audio and sends its
    // broadcast messages and moments; also runs the sequences' own step events.
    void update();
    void stepEvent(uint32_t subtype);  // sequence event_step_begin / event_step / event_step_end
    void draw(const Layer& L, const LayerElement& e);
    // The element is being removed (layer_sequence_destroy, its layer destroyed): runs the
    // destroy event and destroys the instances the sequence created.
    void elementRemoved(int32_t id);
    // Leaving a room: a non-persistent room's sequences go with it.
    void roomEnd(bool persistent);

    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace gm
