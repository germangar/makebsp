#pragma once
#define BASE_FPS 30.0f
#include <string>
#include <vector>
#include <cstdint>
#include <iostream>
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

struct Pose {
    float translate[3];
    float rotate[4];
    float scale[3];
};

struct AnimChannel {
    std::vector<double> times;
    std::vector<float> values; // 3 floats per key for T/S, 4 for R
};

struct BoneAnim {
    AnimChannel translation;
    AnimChannel rotation;
    AnimChannel scale;
};

struct AnimationTrack {
    std::vector<BoneAnim> bones; // One per joint in the model
};

struct AnimationDef {
    std::string name;
    int   first_frame; // Original range (for IQM/SKP reference)
    int   last_frame;
    int   loop_frames;
    float fps;
    
    AnimationTrack track; // The actual sparse data
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
    // Animation data (Sparse Tracks)
    std::vector<AnimationDef> animations;
    
    // Vertex data
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> texcoords;
    std::vector<uint8_t> joints_0;
    std::vector<float> weights_0;
    
    // Indices (shared buffer)
    std::vector<uint32_t> indices;

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
            if (joints[i].parent >= (int)i) return false; // Not topologically sorted
        }
        return true;
    }

    void reorder_skeleton() {
        if (validate_skeleton()) return;

        std::vector<Joint> old_joints = joints;
        std::vector<int> old_to_new(old_joints.size(), -1);
        std::vector<Joint> new_joints;
        new_joints.reserve(old_joints.size());

        // Simple breadth-first or depth-first would work, let's just do iterative passes
        while (new_joints.size() < old_joints.size()) {
            size_t added_this_pass = 0;
            for (size_t i = 0; i < old_joints.size(); ++i) {
                if (old_to_new[i] != -1) continue;
                
                int p = old_joints[i].parent;
                if (p == -1 || old_to_new[p] != -1) {
                    old_to_new[i] = (int)new_joints.size();
                    Joint j = old_joints[i];
                    if (p != -1) j.parent = old_to_new[p];
                    new_joints.push_back(j);
                    added_this_pass++;
                }
            }
            if (added_this_pass == 0) {
                // Cycle detected or disconnected component with invalid parent
                break;
            }
        }

        if (new_joints.size() < old_joints.size()) {
            std::cerr << "Warning: Cyclic or invalid hierarchy detected during reorder. Some joints lost." << std::endl;
        }

        joints = new_joints;

        // Must also re-map joint indices in vertex data
        for (size_t i = 0; i < joints_0.size(); ++i) {
            joints_0[i] = (uint8_t)old_to_new[joints_0[i]];
        }

        // And in animations
        for (auto& ad : animations) {
            std::vector<BoneAnim> new_bones(joints.size());
            for (size_t i = 0; i < old_joints.size(); ++i) {
                if (old_to_new[i] != -1) {
                    new_bones[old_to_new[i]] = ad.track.bones[i];
                }
            }
            ad.track.bones = new_bones;
        }
    }

    // Evaluate animation at a specific time
    void evaluate_animation(int anim_idx, double time, std::vector<Pose>& out_poses) const {
        out_poses.resize(joints.size());
        if (anim_idx < 0 || anim_idx >= (int)animations.size()) {
            for (size_t i = 0; i < joints.size(); ++i) {
                memcpy(out_poses[i].translate, joints[i].translate, 12);
                memcpy(out_poses[i].rotate, joints[i].rotate, 16);
                memcpy(out_poses[i].scale, joints[i].scale, 12);
            }
            return;
        }

        const AnimationDef& anim = animations[anim_idx];
        for (size_t i = 0; i < joints.size(); ++i) {
            const BoneAnim& ba = anim.track.bones[i];
            Pose& p = out_poses[i];

            // Default to bind pose
            memcpy(p.translate, joints[i].translate, 12);
            memcpy(p.rotate, joints[i].rotate, 16);
            memcpy(p.scale, joints[i].scale, 12);

            // Interpolate Translation
            if (!ba.translation.times.empty()) {
                sample_vec3(ba.translation, time, p.translate);
            }
            // Interpolate Rotation
            if (!ba.rotation.times.empty()) {
                sample_quat(ba.rotation, time, p.rotate);
            }
            // Interpolate Scale
            if (!ba.scale.times.empty()) {
                sample_vec3(ba.scale, time, p.scale);
            }
        }
    }

private:
    void sample_vec3(const AnimChannel& chan, double time, float* out) const {
        if (time <= chan.times.front()) {
            memcpy(out, chan.values.data(), 12);
            return;
        }
        if (time >= chan.times.back()) {
            memcpy(out, &chan.values[(chan.times.size() - 1) * 3], 12);
            return;
        }

        auto it = std::lower_bound(chan.times.begin(), chan.times.end(), time);
        size_t idx1 = std::distance(chan.times.begin(), it);
        size_t idx0 = idx1 - 1;

        double t0 = chan.times[idx0];
        double t1 = chan.times[idx1];
        float factor = (float)((time - t0) / (t1 - t0));

        const float* v0 = &chan.values[idx0 * 3];
        const float* v1 = &chan.values[idx1 * 3];
        for (int i = 0; i < 3; ++i) out[i] = v0[i] + factor * (v1[i] - v0[i]);
    }

    void sample_quat(const AnimChannel& chan, double time, float* out) const {
        if (time <= chan.times.front()) {
            memcpy(out, chan.values.data(), 16);
            return;
        }
        if (time >= chan.times.back()) {
            memcpy(out, &chan.values[(chan.times.size() - 1) * 4], 16);
            return;
        }

        auto it = std::lower_bound(chan.times.begin(), chan.times.end(), time);
        size_t idx1 = std::distance(chan.times.begin(), it);
        size_t idx0 = idx1 - 1;

        double t0 = chan.times[idx0];
        double t1 = chan.times[idx1];
        float factor = (float)((time - t0) / (t1 - t0));

        float v0[4], v1[4];
        memcpy(v0, &chan.values[idx0 * 4], 16);
        memcpy(v1, &chan.values[idx1 * 4], 16);

        float dot = v0[0]*v1[0] + v0[1]*v1[1] + v0[2]*v1[2] + v0[3]*v1[3];
        if (dot < 0) {
            dot = -dot;
            for(int i=0; i<4; ++i) v1[i] = -v1[i];
        }

        if (dot > 0.9995f) {
            for(int i=0; i<4; ++i) out[i] = v0[i] + factor*(v1[i]-v0[i]);
        } else {
            float theta0 = acosf(dot);
            float theta = theta0 * factor;
            float s0 = cosf(theta) - dot * sinf(theta) / sinf(theta0);
            float s1 = sinf(theta) / sinf(theta0);
            for(int i=0; i<4; ++i) out[i] = s0*v0[i] + s1*v1[i];
        }
        quat_normalize(out);
    }
};
