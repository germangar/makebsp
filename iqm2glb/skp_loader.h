#pragma once
#include "model.h"

// Load an SKM/SKP model pair into the intermediate Model representation.
// The path should be the base name or point to the .skm file.
// Returns true on success.
bool load_skm(const char* path, Model& out);
