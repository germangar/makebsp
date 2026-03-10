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
    } else if (ends_with(in_path, ".fbx")) {
        std::cout << "Loading FBX (ufbx): " << in_path << "..." << std::endl;
        if (load_fbx(in_path.c_str(), model)) loaded = true;
    }

    if (!loaded) {
        std::cerr << "Failed to load input file or unsupported format: " << in_path << "\n";
        return 2;
    }

    std::cout << "Model loaded: " << model.meshes.size() << " meshes, " << model.joints.size() << " joints, " << model.num_frames << " frames" << std::endl;

    if (model.joints.size() > 0) {
        FILE* f = fopen((std::string(out_path) + ".dump").c_str(), "wb");
        if (f) {
            uint32_t nj = model.joints.size();
            fwrite(&nj, 4, 1, f);
            for(size_t i=0; i<nj; ++i) {
                char nbuf[64] = {0};
                strncpy(nbuf, model.joints[i].name.c_str(), 63);
                fwrite(nbuf, 1, 64, f);
                fwrite(model.joints[i].translate, 4, 3, f);
                fwrite(model.joints[i].rotate, 4, 4, f);
            }
            uint32_t nf = model.num_frames;
            uint32_t nfc = model.num_framechannels;
            fwrite(&nf, 4, 1, f);
            uint32_t stride = model.joints.size() * 10;
            for(uint32_t fi=0; fi<nf; ++fi) {
                for(size_t ji=0; ji<nj; ++ji) {
                    const iqmpose& ip = model.poses[ji];
                    float p[10];
                    for(int c=0; c<10; ++c) p[c] = ip.channeloffset[c];
                    int ch = 0;
                    for(int i=0; i<ji; ++i) for(int c=0; c<10; ++c) if(model.poses[i].mask & (1<<c)) ch++;
                    const float* fptr = model.frames.data() + fi * model.num_framechannels;
                    for(int c=0; c<10; ++c) if(ip.mask & (1<<c)) p[c] += fptr[ch++] * ip.channelscale[c];
                    fwrite(p, 4, 10, f);
                }
            }
            fclose(f);
            std::cout << "DEBUG: Dumped model pure mathematical matrices to " << out_path << ".dump\n";
        }
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
