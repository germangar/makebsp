#include "anim_cfg.h"
#include "model.h"
#include <fstream>
#include <iostream>
#include <cstdio>   // sscanf
#include <cctype>   // isdigit

std::string find_animation_cfg(const std::string& iqm_path) {
    size_t last_dot   = iqm_path.find_last_of('.');
    size_t last_slash = iqm_path.find_last_of("/\\");

    std::string dir = (last_slash == std::string::npos)
                    ? ""
                    : iqm_path.substr(0, last_slash + 1);

    // Priority 1: <modelname>.cfg (e.g. tris.cfg)
    std::string model_cfg = (last_dot != std::string::npos)
                          ? iqm_path.substr(0, last_dot) + ".cfg"
                          : iqm_path + ".cfg";

    // Priority 2: animation.cfg in the same directory
    std::string generic_cfg = dir + "animation.cfg";

    if (std::ifstream(model_cfg).good())   return model_cfg;
    if (std::ifstream(generic_cfg).good()) return generic_cfg;
    return "";
}

std::vector<AnimationDef> parse_animation_cfg(const std::string& path) {
    std::vector<AnimationDef> anims;

    std::ifstream f(path);
    if (!f.is_open()) {
        // Fallback: Hardcoded leading animations only if no file
        anims.push_back({"base",       0,  0,  0, BASE_FPS});
        anims.push_back({"STAND_IDLE", 1, 39, 0, BASE_FPS});
        return anims;
    }

    std::string line;
    while (std::getline(f, line)) {
        // Trim leading whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        if (line.empty()) continue;

        // Skip Warsow-specific non-animation commands
        if (line.find("sex")          == 0 ||
            line.find("rootanim")     == 0 ||
            line.find("tagmask")      == 0 ||
            line.find("rotationbone") == 0) continue;

        // Animation lines start with a digit
        if (!std::isdigit((unsigned char)line[0])) continue;

        int   first = 0, last = 0, loop = 0;
        float fps = BASE_FPS;

        // Require at least two integers (first, last)
        if (sscanf(line.c_str(), "%d %d", &first, &last) < 2) continue;

        AnimationDef ad;
        ad.first_frame = first;
        ad.last_frame  = last;
        ad.loop_frames = 0;
        ad.fps         = BASE_FPS;

        // Optionally parse loop + fps if all four fields are present
        if (sscanf(line.c_str(), "%d %d %d %f", &first, &last, &loop, &fps) == 4) {
            ad.loop_frames = loop;
            ad.fps         = fps;
        }

        // Extract name from trailing // comment
        size_t comment_pos = line.find("//");
        if (comment_pos != std::string::npos) {
            std::string comment = line.substr(comment_pos + 2);
            comment.erase(0, comment.find_first_not_of(" \t"));
            // Take the first word only
            size_t space_pos = comment.find_first_of(" \t\r\n");
            ad.name = (space_pos != std::string::npos)
                    ? comment.substr(0, space_pos)
                    : comment;
        } else {
            ad.name = "unnamed_" + std::to_string(anims.size());
        }

        anims.push_back(ad);
    }

    // Ensure we have at least one "base" animation if file was empty
    if (anims.empty()) {
        anims.push_back({"base", 0, 0, 0, BASE_FPS});
    }

    return anims;
}
