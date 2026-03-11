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
    std::vector<mat4> ibms; // Loaded/Source IBMs
    std::vector<mat4> computed_ibms; // IBMs computed from bind pose

    // Metrics
    float mins[3];
    float maxs[3];
    float radius;
    float xyradius;
    bool has_bounds = false;

    // Helper to calculate bind pose (non-destructive)
    void compute_bind_pose() {
        world_matrices.resize(joints.size());
        computed_ibms.resize(joints.size());
        for (size_t i = 0; i < joints.size(); ++i) {
            const Joint& j = joints[i];
            float q[4] = { j.rotate[0], j.rotate[1], j.rotate[2], j.rotate[3] };
            quat_normalize(q);
            mat4 local = mat4_from_trs(j.translate, q, j.scale);
            if (j.parent >= 0) {
                if (j.parent < (int)i) {
                    world_matrices[i] = mat4_mul(world_matrices[j.parent], local);
                } else {
                    // Out of order or cyclic - fallback to local until validation pass fixes/flags it
                    world_matrices[i] = local;
                }
            } else {
                world_matrices[i] = local;
            }
            computed_ibms[i] = mat4_invert(world_matrices[i]);
        }
    }

    void compute_bounds() {
        if (positions.empty()) return;
        mins[0] = mins[1] = mins[2] = 1e30f;
        maxs[0] = maxs[1] = maxs[2] = -1e30f;
        radius = xyradius = 0;
        for (size_t i = 0; i < positions.size() / 3; ++i) {
            float x = positions[i*3+0];
            float y = positions[i*3+1];
            float z = positions[i*3+2];
            mins[0] = std::min(mins[0], x);
            mins[1] = std::min(mins[1], y);
            mins[2] = std::min(mins[2], z);
            maxs[0] = std::max(maxs[0], x);
            maxs[1] = std::max(maxs[1], y);
            maxs[2] = std::max(maxs[2], z);
            
            float dist2 = x*x + y*y + z*z;
            radius = std::max(radius, sqrtf(dist2));
            float ground_dist2 = x*x + z*z; // Horizontal plane for internal Y-up
            xyradius = std::max(xyradius, sqrtf(ground_dist2));
        }
        has_bounds = true;
    }

    bool validate_skeleton() {
        for (size_t i = 0; i < joints.size(); ++i) {
            if (joints[i].parent >= (int)joints.size()) return false;
            // Detect cycles (simple check: parent must be < child for our tree-building writers)
            // Note: IQM and FBX generally expect parents to be defined before children
        }
        return true;
    }
};
