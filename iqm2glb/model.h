#pragma once
#define BASE_FPS 30.0f
#include <string>
#include <vector>
#include <cstdint>
#include "iqm.h"
#include "math_utils.h"
#include "anim_cfg.h"

struct Joint {
    std::string name;
    int parent;
    float translate[3];
    float rotate[4]; // xyzw
    float scale[3];
};

struct Mesh {
    std::string name;
    std::string material_name;
    uint32_t first_vertex, num_vertexes;
    uint32_t first_triangle, num_triangles;
};

struct Model {
    std::vector<Joint> joints;
    std::vector<Mesh> meshes;
    std::vector<AnimationDef> animations;
    
    // Vertex data
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> texcoords;
    std::vector<uint8_t> joints_0;
    std::vector<float> weights_0;
    
    // Indices (shared buffer)
    std::vector<uint32_t> indices;

    // Animation frame data (floats, will be quantized by writer)
    std::vector<float> frames;
    std::vector<iqmpose> poses; // We'll keep iqmpose for now as it's a good compact rep
    
    uint32_t num_frames = 0;
    uint32_t num_framechannels = 0;
    bool qfusion = false;

    // Bind-pose transform cache
    std::vector<mat4> world_matrices;
    std::vector<mat4> ibms;

    // Helper to calculate bind pose
    void compute_bind_pose() {
        world_matrices.resize(joints.size());
        ibms.resize(joints.size());
        for (size_t i = 0; i < joints.size(); ++i) {
            const Joint& j = joints[i];
            float q[4] = { j.rotate[0], j.rotate[1], j.rotate[2], j.rotate[3] };
            quat_normalize(q);
            mat4 local = mat4_from_trs(j.translate, q, j.scale);
            if (j.parent >= 0) {
                world_matrices[i] = mat4_mul(world_matrices[j.parent], local);
            } else {
                world_matrices[i] = local;
            }
            ibms[i] = mat4_invert(world_matrices[i]);
        }
    }
};
