#pragma once
#include "model.h"

// Write an FBX file from internal Model data.
// path: Output file path.
// in: Input model data.
// write_mesh: Include geometry and skinning.
// write_anim: Include animation stacks/curves.
// anim_index: If >= 0, only export that specific animation. If -1, export all.
bool write_fbx(const char* path, const Model& in, bool write_mesh = true, bool write_anim = true, int anim_index = -1);
