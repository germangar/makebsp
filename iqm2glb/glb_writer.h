#pragma once
#include "model.h"

// Write the intermediate Model representation to a GLB file.
// Returns true on success.
bool write_glb(const Model& model, const char* output_path);
