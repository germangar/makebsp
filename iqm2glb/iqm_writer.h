#pragma once
#include "model.h"

// Write the intermediate Model representation to an IQM file.
// Also generates a .cfg file if animations are present.
// Returns true on success.
bool write_iqm(const Model& model, const char* output_path);
