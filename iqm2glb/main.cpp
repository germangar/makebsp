#include <iostream>
#include <string>
#include <cstring>
#include <algorithm>
#include <vector>
#include <fstream>
#include "iqm_loader.h"
#include "anim_cfg.h"
#include "math_utils.h"
#include "glb_loader.h"
#include "skp_loader.h"
#include "iqm_writer.h"
#include "glb_writer.h"
#include "fbx_writer.h"
#include "fbx_loader.h"

// Animations are validated during loading in the sparse model

static std::string get_unique_path(const std::string& path) {
    std::ifstream f(path.c_str());
    if (!f.good()) return path;
    f.close();

    size_t dot = path.find_last_of('.');
    std::string base = (dot == std::string::npos) ? path : path.substr(0, dot);
    std::string ext = (dot == std::string::npos) ? "" : path.substr(dot);

    int counter = 1;
    while (true) {
        std::string new_path = base + "_" + std::to_string(counter) + ext;
        std::ifstream nf(new_path.c_str());
        if (!nf.good()) return new_path;
        nf.close();
        counter++;
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

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--qfusion") {
            model.qfusion = true;
        }
    }

    // Load Phase
    if (ends_with(in_path, ".iqm")) {
        std::cout << "Loading IQM: " << in_path << "..." << std::endl;
        if (load_iqm(in_path.c_str(), model)) {
            loaded = true;
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

    // Universal Sanitization & Initialization
    if (!model.validate_skeleton()) {
        std::cout << "Reordering skeleton for topological consistency..." << std::endl;
        model.reorder_skeleton();
    }
    model.compute_bind_pose();
    model.compute_bounds();

    size_t total_keys = 0;
    for(const auto& a : model.animations) {
        for(const auto& b : a.track.bones) {
            total_keys += b.translation.times.size() + b.rotation.times.size() + b.scale.times.size();
        }
    }

    std::cout << "Model loaded: " << model.meshes.size() << " meshes, " << model.joints.size() << " joints, " 
              << model.animations.size() << " animations (" << total_keys << " keys)" << std::endl;
    if (model.has_bounds) {
        printf("Bounds: (%.3f %.3f %.3f) to (%.3f %.3f %.3f)\n", 
               model.mins[0], model.mins[1], model.mins[2],
               model.maxs[0], model.maxs[1], model.maxs[2]);
    }


    // Write Phase
    std::string unique_out = get_unique_path(out_path);

    if (ends_with(unique_out, ".iqm")) {
        std::cout << "Writing IQM: " << unique_out << "..." << std::endl;
        if (!write_iqm(model, unique_out.c_str())) return 3;
    } else if (ends_with(unique_out, ".fbx")) {
        bool write_base = true;
        bool write_anim = true;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--base") write_anim = false;
            if (arg == "--anim") write_base = false;
        }

        if (write_base && !write_anim) std::cout << "Writing FBX (Base Pose only): " << unique_out << "..." << std::endl;
        else if (!write_base && write_anim) std::cout << "Writing FBX (Animations only): " << unique_out << "..." << std::endl;
        else std::cout << "Writing FBX (Complete): " << unique_out << "..." << std::endl;

        // Using Native writer as the primary FBX exporter
        if (!write_fbx(unique_out.c_str(), model)) return 3;
    } else if (ends_with(unique_out, ".glb")) {
        std::cout << "Writing GLB (cgltf): " << unique_out << "..." << std::endl;
        if (!write_glb(model, unique_out.c_str())) return 3;
    } else {
        std::cerr << "Unsupported output format: " << unique_out << "\n";
        return 4;
    }

    return 0;
}
