#pragma once
#include "model.h"

// Load a GLB/GLTF file using Assimp into the intermediate Model representation.
// Returns true on success.
bool load_glb_assimp(const char* path, Model& out);
