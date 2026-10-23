// Skeleton sprites (GameMaker's sprite type 2): drawn by a module kept outside this repository,
// built in when it is present in private/. Without it they are left undrawn and the skeleton_*
// functions are unimplemented (src/runtime/lib/SkeletonStub.cpp).
#pragma once

#include <cstdint>

namespace gm {

class Runner;
struct Instance;

struct SkeletonStore;
void destroySkeletonStore(SkeletonStore* s);

bool isSkeletonSprite(Runner& rt, int sprite);
// image_number of an instance showing a skeleton sprite.
int skeletonFrameCount(Runner& rt, const Instance* inst);
// The instance's own draw (draw_self, or no Draw event).
void skeletonDrawInstance(Runner& rt, Instance* inst);
// draw_sprite / draw_sprite_ext of a skeleton sprite, `drawing` being the instance drawing.
void skeletonDrawSprite(Runner& rt, Instance* drawing, int sprite, double subimg, double x, double y, double xs,
                        double ys, double rot, uint32_t col, double alpha);
void registerSkeletonBuiltins(Runner& rt);

}  // namespace gm
