#include "iqm_loader.h"
#include <iostream>
#include <fstream>
#include <cstring>

bool load_iqm(const char* path, IQMModel& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "Could not open: " << path << "\n";
        return false;
    }

    // Read header first to validate magic
    iqmheader hdr;
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (std::memcmp(hdr.magic, IQM_MAGIC, 16) != 0) {
        std::cerr << "Not a valid IQM file: " << path << "\n";
        return false;
    }

    // Read entire file
    f.seekg(0, std::ios::end);
    size_t file_size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);

    out.raw_data.resize(file_size);
    f.read(out.raw_data.data(), file_size);

    // Copy header from raw_data so out.header is stable
    std::memcpy(&out.header, out.raw_data.data(), sizeof(iqmheader));
    const iqmheader& h = out.header;

    // Set up raw pointers into the (now stable) buffer
    out.text         = out.raw_data.data() + h.ofs_text;
    out.joints       = reinterpret_cast<iqmjoint*>      (out.raw_data.data() + h.ofs_joints);
    out.poses        = reinterpret_cast<iqmpose*>       (out.raw_data.data() + h.ofs_poses);
    out.meshes       = reinterpret_cast<iqmmesh*>       (out.raw_data.data() + h.ofs_meshes);
    out.vertex_arrays= reinterpret_cast<iqmvertexarray*>(out.raw_data.data() + h.ofs_vertexarrays);

    // Count animated frame channels (sum of set bits in every pose mask)
    out.frame_channels = 0;
    for (uint32_t p = 0; p < h.num_poses; ++p) {
        uint32_t mask = out.poses[p].mask;
        for (int c = 0; c < 10; ++c) {
            if (mask & (1u << c)) out.frame_channels++;
        }
    }

    // Pre-compute bind-pose world matrices and inverse bind matrices
    out.world_matrices.resize(h.num_joints);
    out.ibms.resize(h.num_joints);

    for (uint32_t i = 0; i < h.num_joints; ++i) {
        const iqmjoint& j = out.joints[i];
        float q[4] = { j.rotate[0], j.rotate[1], j.rotate[2], j.rotate[3] };
        quat_normalize(q);
        mat4 local = mat4_from_trs(j.translate, q, j.scale);

        if (j.parent >= 0) {
            out.world_matrices[i] = mat4_mul(out.world_matrices[j.parent], local);
        } else {
            out.world_matrices[i] = local;
        }
        out.ibms[i] = mat4_invert(out.world_matrices[i]);
    }

    return true;
}
