#pragma once
#include <string>
#include <vector>

struct AnimationDef;

// Locate an animation config alongside the given IQM path.
// Checks <model>.cfg first, then animation.cfg in the same directory.
// Returns empty string if not found.
std::string find_animation_cfg(const std::string& iqm_path);

// Parse a Warsow/Warfork-style animation.cfg.
// Always prepends the hardcoded 'base' (frame 0) and 'STAND_IDLE' (frames 1-39).
// Falls back to an empty additional list if the file cannot be opened.
std::vector<AnimationDef> parse_animation_cfg(const std::string& path);
