#include <iostream>
#include <string>
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

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input.iqm/glb/skm> <output.glb/iqm/fbx> [--base] [--anim]\n";
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

    // Load Phase
    if (ends_with(in_path, ".iqm")) {
        std::cout << "Loading IQM: " << in_path << "..." << std::endl;
        if (load_iqm(in_path.c_str(), model)) {
            loaded = true;
            std::string cfg_path = find_animation_cfg(in_path);
            if (!cfg_path.empty()) {
                std::cout << "Found animation config: " << cfg_path << std::endl;
                model.animations = parse_animation_cfg(cfg_path);
            }
        }
    } else if (ends_with(in_path, ".skm") || ends_with(in_path, ".skp")) {
        std::cout << "Loading SKM/SKP: " << in_path << "..." << std::endl;
        if (load_skm(in_path.c_str(), model)) loaded = true;
    } else if (ends_with(in_path, ".glb") || ends_with(in_path, ".gltf")) {
        std::cout << "Loading GLB (cgltf): " << in_path << "..." << std::endl;
        if (load_glb(in_path.c_str(), model)) loaded = true;
    }

    if (!loaded) {
        std::cerr << "Failed to load input file or unsupported format: " << in_path << "\n";
        return 2;
    }

    std::cout << "Model loaded: " << model.meshes.size() << " meshes, " << model.joints.size() << " joints, " << model.num_frames << " frames" << std::endl;

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
