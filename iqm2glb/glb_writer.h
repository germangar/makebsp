#pragma once
#include "iqm_loader.h"
#include "anim_cfg.h"
#include <vector>

// Write the loaded IQM model and its animations to a GLB file.
// Returns true on success.
bool write_glb(const IQMModel& model,
               const std::vector<AnimationDef>& animations,
               const char* output_path);
