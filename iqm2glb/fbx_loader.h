#pragma once
#include "model.h"

// Load a Binary or ASCII FBX file using the ufbx library.
// Converts data into our intermediate Model representation (Y-up CCW).
// Returns true on success.
bool load_fbx(const char* path, Model& out);
