#include <iostream>
#include <string>
#include "iqm_loader.h"
#include "anim_cfg.h"
#include "glb_writer.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input.iqm> <output.glb>\n";
        return 1;
    }

    // 1. Load the IQM model
    IQMModel model;
    if (!load_iqm(argv[1], model)) return 2;

    // 2. Resolve animations: prefer animation.cfg, fall back to IQM native anims
    std::vector<AnimationDef> animations;
    std::string cfg_path = find_animation_cfg(argv[1]);

    if (!cfg_path.empty()) {
        animations = parse_animation_cfg(cfg_path);
    } else {
        const iqmheader& h = model.header;
        const iqmanim* iqm_anims =
            reinterpret_cast<const iqmanim*>(model.raw_data.data() + h.ofs_anims);
        for (uint32_t i = 0; i < h.num_anims; ++i) {
            AnimationDef ad;
            ad.name        = model.text + iqm_anims[i].name;
            ad.first_frame = iqm_anims[i].first_frame;
            ad.last_frame  = iqm_anims[i].first_frame + iqm_anims[i].num_frames - 1;
            ad.fps         = (iqm_anims[i].framerate > 0.0f) ? iqm_anims[i].framerate : 24.0f;
            ad.loop_frames = 0;
            animations.push_back(ad);
        }
    }

    // 3. Write GLB
    if (!write_glb(model, animations, argv[2])) return 3;

    return 0;
}
