#include <iostream>
#include <string>
#include <algorithm>
#include "iqm_loader.h"
#include "anim_cfg.h"
#include "glb_writer.h"
#include "glb_loader.h"
#include "iqm_writer.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input.iqm/glb> <output.glb/iqm>\n";
        return 1;
    }

    std::string in_path = argv[1];
    std::string out_path = argv[2];

    auto ends_with = [](const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() && 
               std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(), 
               [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    };

    if (ends_with(in_path, ".iqm")) {
        Model model;
        std::cout << "Loading IQM: " << argv[1] << "..." << std::endl;
        if (!load_iqm(argv[1], model)) {
            std::cerr << "Failed to load IQM: " << argv[1] << "\n";
            return 2;
        }
        std::cout << "Loaded IQM (" << model.meshes.size() << " meshes, " << model.joints.size() << " joints)" << std::endl;

        std::string cfg_path = find_animation_cfg(argv[1]);
        if (!cfg_path.empty()) {
            std::cout << "Found animation config: " << cfg_path << std::endl;
            model.animations = parse_animation_cfg(cfg_path);
            std::cout << "Parsed " << model.animations.size() << " animations" << std::endl;
        }

        std::cout << "Writing GLB: " << argv[2] << "..." << std::endl;
        if (!write_glb(model, argv[2])) {
            std::cerr << "Failed to write GLB: " << argv[2] << "\n";
            return 3;
        }
    } else if (ends_with(in_path, ".glb") || ends_with(in_path, ".gltf")) {
        Model model;
        if (!load_glb(argv[1], model)) {
            std::cerr << "Failed to load GLB: " << argv[1] << "\n";
            return 4;
        }

        if (!write_iqm(model, argv[2])) {
            std::cerr << "Failed to write IQM: " << argv[2] << "\n";
            return 5;
        }
    } else {
        std::cerr << "Unsupported file format. Use .iqm or .glb/.gltf\n";
        return 6;
    }

    return 0;
}
