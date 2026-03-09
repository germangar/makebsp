#include <iostream>
#include <string>
#include <algorithm>
#include "iqm_loader.h"
#include "anim_cfg.h"
#include "glb_loader.h"
#include "glb_loader_assimp.h"
#include "iqm_writer.h"
#include "glb_writer.h"
#include "glb_writer_assimp.h"

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

        // Option 1: Assimp-based GLB Writer (Default: Supports skeletal animations, PBR)
        std::cout << "Writing GLB (Assimp): " << argv[2] << "..." << std::endl;
        if (!write_glb_assimp(model, argv[2])) {
            std::cerr << "Failed to write GLB (Assimp): " << argv[2] << "\n";
            return 3;
        }

        /*
        // Option 2: Original cgltf-based GLB Writer (Lighter, no animation support)
        std::cout << "Writing GLB (cgltf): " << argv[2] << "..." << std::endl;
        if (!write_glb(model, argv[2])) {
            std::cerr << "Failed to write GLB (cgltf): " << argv[2] << "\n";
            return 3;
        }
        */
    } else if (ends_with(in_path, ".glb") || ends_with(in_path, ".gltf")) {
        Model model;
        
        /*
        // Option 1: cgltf-based GLB Loader (Default: Lean, fast)
        std::cout << "Loading GLB (cgltf): " << argv[1] << "..." << std::endl;
        if (!load_glb(argv[1], model)) {
            std::cerr << "Failed to load GLB (cgltf): " << argv[1] << "\n";
            return 4;
        }
        */

        // Option 2: Assimp-based GLB Loader (Heavier, but robust & supports other formats)
        std::cout << "Loading GLB (Assimp): " << argv[1] << "..." << std::endl;
        if (!load_glb_assimp(argv[1], model)) {
            std::cerr << "Failed to load GLB (Assimp): " << argv[1] << "\n";
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
