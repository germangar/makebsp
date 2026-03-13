#pragma once
#include "math_utils.h"
#include "model.h"

#if ENABLE_ASSIMP
// Load a GLB/GLTF file using Assimp into the intermediate Model representation.
// Returns true on success.
bool load_glb_assimp(const char* path, Model& out);
#endif
