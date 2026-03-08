#pragma once
#include <string>
#include <vector>
#include "model.h"

// Load an IQM file into the intermediate Model representation.
// Returns true on success.
bool load_iqm(const char* path, Model& out);
