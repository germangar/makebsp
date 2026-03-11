#include <iostream>
#include <string>
#include <cstring>
#include <algorithm>
#include <vector>
#include "iqm_loader.h"
#include "anim_cfg.h"
#include "glb_loader.h"
#include "glb_loader_assimp.h"
#include "skp_loader.h"
#include "iqm_writer.h"
#include "glb_writer.h"
#include "glb_writer_assimp.h"
#include "fbx_writer.h"
#include "fbx_loader.h"

void sanitize_animations(Model& model) {
    auto it = model.animations.begin();
    while (it != model.animations.end()) {
        bool valid = true;
        if (model.num_frames > 0) {
            if (it->first_frame < 0) it->first_frame = 0;
            if (it->last_frame >= (int)model.num_frames) {
                std::cerr << "Animation Error: \"" << it->name << "\" last frame " << it->last_frame << " exceeds model max " << (model.num_frames - 1) << ". Clamping." << std::endl;
                it->last_frame = (int)model.num_frames - 1;
            }
            if (it->first_frame > it->last_frame) {
                std::cerr << "Animation Warning: \"" << it->name << "\" has invalid range [" << it->first_frame << ", " << it->last_frame << "]. Removing." << std::endl;
                valid = false;
            }
        } else if (it->first_frame != 0 || it->last_frame != 0) {
             // If model has no frames, only allow 0-0 range if it exists for some reason
             valid = false;
        }

        if (valid) {
            ++it;
        } else {
            it = model.animations.erase(it);
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input.iqm/glb/skm> <output.glb/iqm/fbx> [--base] [--anim] [--qfusion]\n";
        return 1;
    }

    std::string in_path = argv[1];
    std::string out_path = argv[2];

    auto ends_with = [](const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() && 
               std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(), 
               [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    };

    Model model;
    bool loaded = false;

    bool qfusion = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--qfusion") {
            qfusion = true;
            model.qfusion = true;
        }
    }

    // Load Phase
    if (ends_with(in_path, ".iqm")) {
        std::cout << "Loading IQM: " << in_path << "..." << std::endl;
        if (load_iqm(in_path.c_str(), model)) {
            loaded = true;
            std::string cfg_path = find_animation_cfg(in_path);
            
            if (!cfg_path.empty()) {
                std::cout << "Found animation config: " << cfg_path << std::endl;
                model.animations.clear();
                if (qfusion) {
                    model.animations.push_back({"base", 0, 0, 0, BASE_FPS});
                    model.animations.push_back({"STAND_IDLE", 1, 39, 0, BASE_FPS});
                }
                std::vector<AnimationDef> cfg_anims = parse_animation_cfg(cfg_path);
                for (const auto& a : cfg_anims) model.animations.push_back(a);
            } else {
                if (model.animations.empty() && model.num_frames > 0) {
                    model.animations.push_back({"frames", 0, (int)model.num_frames - 1, 0, BASE_FPS});
                }
            }
        }
    } else if (ends_with(in_path, ".skm") || ends_with(in_path, ".skp")) {
        std::cout << "Loading SKM/SKP: " << in_path << "..." << std::endl;
        if (load_skm(in_path.c_str(), model)) loaded = true;
    } else if (ends_with(in_path, ".glb") || ends_with(in_path, ".gltf")) {
        std::cout << "Loading GLB (cgltf): " << in_path << "..." << std::endl;
        if (load_glb(in_path.c_str(), model)) loaded = true;
    } else if (ends_with(in_path, ".fbx")) {
        std::cout << "Loading FBX (ufbx): " << in_path << "..." << std::endl;
        if (load_fbx(in_path.c_str(), model)) loaded = true;
    }

    if (!loaded) {
        std::cerr << "Failed to load input file or unsupported format: " << in_path << "\n";
        return 2;
    }

    // Universal Sanitization
    sanitize_animations(model);

    std::cout << "Model loaded: " << model.meshes.size() << " meshes, " << model.joints.size() << " joints, " << model.num_frames << " frames" << std::endl;
    std::cout << "Final Animations (" << model.animations.size() << "):" << std::endl;
    for (const auto& a : model.animations) {
        std::cout << "  - \"" << a.name << "\" (Frames " << a.first_frame << " to " << a.last_frame << ", " << a.fps << " FPS)" << std::endl;
    }


    // Write Phase
    if (ends_with(out_path, ".iqm")) {
        std::cout << "Writing IQM: " << out_path << "..." << std::endl;
        if (!write_iqm(model, out_path.c_str())) return 3;
    } else if (ends_with(out_path, ".fbx")) {
        bool write_base = true;
        bool write_anim = true;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--base") write_anim = false;
            if (arg == "--anim") write_base = false;
        }

        if (write_base && !write_anim) std::cout << "Writing FBX (Base Pose only): " << out_path << "..." << std::endl;
        else if (!write_base && write_anim) std::cout << "Writing FBX (Animations only): " << out_path << "..." << std::endl;
        else std::cout << "Writing FBX (Complete): " << out_path << "..." << std::endl;

        if (!write_fbx(out_path.c_str(), model, write_base, write_anim)) return 3;
    } else if (ends_with(out_path, ".glb")) {
        std::cout << "Writing GLB (cgltf): " << out_path << "..." << std::endl;
        if (!write_glb(model, out_path.c_str())) return 3;
    } else {
        std::cerr << "Unsupported output format: " << out_path << "\n";
        return 4;
    }

    return 0;
}
