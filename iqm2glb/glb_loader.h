#pragma once
#include "model.h"

// Load a GLB or glTF file into the intermediate Model representation.
// Returns true on success.
bool load_glb(const char* path, Model& out);
