// Without the skeleton sprite module (see runtime/SkeletonSprites.h) such sprites aren't drawn.
#ifndef GMR_SKELETON_MODULE

#include "runtime/SkeletonSprites.h"

namespace gm {
struct SkeletonStore {};
void destroySkeletonStore(SkeletonStore*) {}
bool isSkeletonSprite(Runner&, int) { return false; }
int skeletonFrameCount(Runner&, const Instance*) { return 0; }
void skeletonDrawInstance(Runner&, Instance*) {}
void skeletonDrawSprite(Runner&, Instance*, int, double, double, double, double, double, double, uint32_t, double) {}
void registerSkeletonBuiltins(Runner&) {}
}  // namespace gm

#endif
