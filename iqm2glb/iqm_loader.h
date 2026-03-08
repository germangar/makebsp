#pragma once
#include <string>
#include <vector>
#include "math_utils.h"
#include "iqm.h"

struct IQMModel {
    // Owns the raw file data. All pointer members below point into this buffer.
    // Do NOT resize or reallocate raw_data after the pointers are set.
    std::vector<char> raw_data;

    iqmheader        header;

    // Raw pointers into raw_data — valid as long as raw_data isn't reallocated
    iqmjoint*        joints        = nullptr;
    iqmpose*         poses         = nullptr;
    iqmmesh*         meshes        = nullptr;
    iqmvertexarray*  vertex_arrays = nullptr;
    const char*      text          = nullptr;   // text pool base

    // Pre-computed skinning data
    std::vector<mat4> world_matrices;  // bind-pose world matrices per joint
    std::vector<mat4> ibms;            // inverse bind matrices per joint

    // Total animated frame channels (sum of set bits in all pose masks)
    uint32_t frame_channels = 0;
};

// Load an IQM file. Returns true on success.
bool load_iqm(const char* path, IQMModel& out);
