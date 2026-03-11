#include "skp_loader.h"
#include "skmodel2/skmformat.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>

static void transform_vec(const mat4& m, const float* in, float* out, float weight) {
    out[0] += (m.m[0] * in[0] + m.m[4] * in[1] + m.m[8] * in[2] + m.m[12] * weight);
    out[1] += (m.m[1] * in[0] + m.m[5] * in[1] + m.m[9] * in[2] + m.m[13] * weight);
    out[2] += (m.m[2] * in[0] + m.m[6] * in[1] + m.m[10] * in[2] + m.m[14] * weight);
}

static void rotate_vec(const mat4& m, const float* in, float* out) {
    out[0] += (m.m[0] * in[0] + m.m[4] * in[1] + m.m[8] * in[2]);
    out[1] += (m.m[1] * in[0] + m.m[5] * in[1] + m.m[9] * in[2]);
    out[2] += (m.m[2] * in[0] + m.m[6] * in[1] + m.m[10] * in[2]);
}

bool load_skm(const char* path, Model& out) {
    std::string base_path = path;
    size_t dot = base_path.find_last_of('.');
    if (dot != std::string::npos && (base_path.substr(dot) == ".skm" || base_path.substr(dot) == ".skp")) {
        base_path = base_path.substr(0, dot);
    }

    std::string skm_path = base_path + ".skm";
    std::string skp_path = base_path + ".skp";

    // 1. Load SKP (Skeleton and Poses)
    std::ifstream f_skp(skp_path, std::ios::binary);
    if (!f_skp) {
        std::cerr << "Failed to open SKP file: " << skp_path << std::endl;
        return false;
    }
    f_skp.seekg(0, std::ios::end);
    size_t skp_size = f_skp.tellg();
    f_skp.seekg(0, std::ios::beg);
    std::vector<char> skp_buf(skp_size);
    f_skp.read(skp_buf.data(), skp_size);
    f_skp.close();

    dskpheader_t* skp_hdr = (dskpheader_t*)skp_buf.data();
    if (std::memcmp(skp_hdr->id, "SKM1", 4) != 0) {
        std::cerr << "Invalid SKP header magic" << std::endl;
        return false;
    }

    uint32_t num_bones = skp_hdr->num_bones;
    uint32_t num_frames = skp_hdr->num_frames;

    out.joints.resize(num_bones);
    dskpbone_t* skp_bones = (dskpbone_t*)(skp_buf.data() + skp_hdr->ofs_bones);
    for (uint32_t i = 0; i < num_bones; ++i) {
        out.joints[i].name = skp_bones[i].name;
        out.joints[i].parent = skp_bones[i].parent;
    }

    // Load Poses (Frame 0 is base pose)
    std::vector<std::vector<dskpbonepose_t>> frame_poses(num_frames, std::vector<dskpbonepose_t>(num_bones));
    dskpframe_t* skp_frames = (dskpframe_t*)(skp_buf.data() + skp_hdr->ofs_frames);
    for (uint32_t f = 0; f < num_frames; ++f) {
        dskpbonepose_t* poses = (dskpbonepose_t*)(skp_buf.data() + skp_frames[f].ofs_bonepositions);
        std::memcpy(frame_poses[f].data(), poses, num_bones * sizeof(dskpbonepose_t));
    }

    // Compute Z-up World Transforms for Bind Pose (Frame 0)
    std::vector<mat4> zup_world_matrices(num_bones);
    for (uint32_t i = 0; i < num_bones; ++i) {
        float scale[3] = {1, 1, 1};
        mat4 local = mat4_from_trs(frame_poses[0][i].origin, frame_poses[0][i].quat, scale);
        if (out.joints[i].parent >= 0) {
            zup_world_matrices[i] = mat4_mul(zup_world_matrices[out.joints[i].parent], local);
        } else {
            zup_world_matrices[i] = local;
        }
    }

    // Populate Model Joints (Y-up transformed Bind Pose)
    for (uint32_t i = 0; i < num_bones; ++i) {
        float t[3]; std::memcpy(t, frame_poses[0][i].origin, 12);
        float r[4]; std::memcpy(r, frame_poses[0][i].quat, 16);
        float s[3] = {1, 1, 1};

        if (out.joints[i].parent == -1) {
            // Root Translation: (x, y, z) -> (x, z, -y)
            float t_yup[3] = {t[0], t[2], -t[1]};
            std::memcpy(t, t_yup, 12);
            
            // Root Rotation: pre-multiply by -90X
            float q_rot[4] = {-0.7071068f, 0.0f, 0.0f, 0.7071068f};
            float r_new[4];
            quat_mul(q_rot, r, r_new);
            quat_normalize(r_new);
            std::memcpy(r, r_new, 16);
        }
        std::memcpy(out.joints[i].translate, t, 12);
        std::memcpy(out.joints[i].rotate, r, 16);
        std::memcpy(out.joints[i].scale, s, 12);
    }

    // 2. Load SKM (Mesh Data)
    std::ifstream f_skm(skm_path, std::ios::binary);
    if (!f_skm) {
        std::cerr << "Failed to open SKM file: " << skm_path << std::endl;
        return false;
    }
    f_skm.seekg(0, std::ios::end);
    size_t skm_size = f_skm.tellg();
    f_skm.seekg(0, std::ios::beg);
    std::vector<char> skm_buf(skm_size);
    f_skm.read(skm_buf.data(), skm_size);
    f_skm.close();

    dskmheader_t* skm_hdr = (dskmheader_t*)skm_buf.data();
    if (std::memcmp(skm_hdr->id, "SKM1", 4) != 0) {
        std::cerr << "Invalid SKM header magic" << std::endl;
        return false;
    }

    dskmmesh_t* skm_meshes = (dskmmesh_t*)(skm_buf.data() + skm_hdr->ofs_meshes);
    out.meshes.resize(skm_hdr->num_meshes);
    
    for (uint32_t m = 0; m < skm_hdr->num_meshes; ++m) {
        Mesh& mesh = out.meshes[m];
        mesh.name = skm_meshes[m].meshname;
        mesh.material_name = skm_meshes[m].shadername;
        mesh.first_vertex = out.positions.size() / 3;
        mesh.num_vertexes = skm_meshes[m].num_verts;
        mesh.first_triangle = out.indices.size() / 3;
        mesh.num_triangles = skm_meshes[m].num_tris;

        // Vertices
        out.positions.resize((mesh.first_vertex + mesh.num_vertexes) * 3);
        out.normals.resize((mesh.first_vertex + mesh.num_vertexes) * 3);
        out.texcoords.resize((mesh.first_vertex + mesh.num_vertexes) * 2);
        out.joints_0.resize((mesh.first_vertex + mesh.num_vertexes) * 4, 0);
        out.weights_0.resize((mesh.first_vertex + mesh.num_vertexes) * 4, 0.0f);

        unsigned char* v_ptr = (unsigned char*)skm_buf.data() + skm_meshes[m].ofs_verts;
        for (uint32_t v = 0; v < mesh.num_vertexes; ++v) {
            unsigned int num_influences = *(unsigned int*)v_ptr;
            v_ptr += sizeof(unsigned int);
            dskmbonevert_t* influences = (dskmbonevert_t*)v_ptr;
            v_ptr += num_influences * sizeof(dskmbonevert_t);

            float zup_pos[3] = {0, 0, 0};
            float zup_norm[3] = {0, 0, 0};

            struct Influence {
                int bone;
                float weight;
            };
            std::vector<Influence> sorted_influences;
            for (uint32_t i = 0; i < num_influences; ++i) {
                if (influences[i].bonenum < num_bones) {
                    transform_vec(zup_world_matrices[influences[i].bonenum], influences[i].origin, zup_pos, influences[i].influence);
                    rotate_vec(zup_world_matrices[influences[i].bonenum], influences[i].normal, zup_norm);
                    sorted_influences.push_back({(int)influences[i].bonenum, influences[i].influence});
                }
            }

            std::sort(sorted_influences.begin(), sorted_influences.end(), [](const Influence& a, const Influence& b) {
                return a.weight > b.weight;
            });

            float total_weight = 0;
            int n_infl = std::min((int)sorted_influences.size(), 4);
            for (int i = 0; i < n_infl; ++i) total_weight += sorted_influences[i].weight;
            
            for (int i = 0; i < n_infl; ++i) {
                out.joints_0[(mesh.first_vertex + v) * 4 + i] = (uint8_t)sorted_influences[i].bone;
                out.weights_0[(mesh.first_vertex + v) * 4 + i] = (total_weight > 0) ? (sorted_influences[i].weight / total_weight) : 0;
            }

            // Convert reconstructed Z-up Model Position to Y-up
            out.positions[(mesh.first_vertex + v) * 3 + 0] = zup_pos[0];
            out.positions[(mesh.first_vertex + v) * 3 + 1] = zup_pos[2];
            out.positions[(mesh.first_vertex + v) * 3 + 2] = -zup_pos[1];

            // Convert reconstructed Z-up Model Normal to Y-up
            float norm_len = std::sqrt(zup_norm[0]*zup_norm[0] + zup_norm[1]*zup_norm[1] + zup_norm[2]*zup_norm[2]);
            if (norm_len > 1e-6f) {
                float inv_len = 1.0f / norm_len;
                zup_norm[0] *= inv_len; zup_norm[1] *= inv_len; zup_norm[2] *= inv_len;
            }
            out.normals[(mesh.first_vertex + v) * 3 + 0] = zup_norm[0];
            out.normals[(mesh.first_vertex + v) * 3 + 1] = zup_norm[2];
            out.normals[(mesh.first_vertex + v) * 3 + 2] = -zup_norm[1];
        }

        // Texcoords
        dskmcoord_t* st = (dskmcoord_t*)(skm_buf.data() + skm_meshes[m].ofs_texcoords);
        for (uint32_t v = 0; v < mesh.num_vertexes; ++v) {
            out.texcoords[(mesh.first_vertex + v) * 2 + 0] = st[v].st[0];
            out.texcoords[(mesh.first_vertex + v) * 2 + 1] = st[v].st[1]; 
        }

        // Indices
        unsigned int* idx = (unsigned int*)(skm_buf.data() + skm_meshes[m].ofs_indices);
        for (uint32_t t = 0; t < mesh.num_triangles; ++t) {
            out.indices.push_back(mesh.first_vertex + idx[t * 3 + 0]);
            out.indices.push_back(mesh.first_vertex + idx[t * 3 + 2]); // CW to CCW flip
            out.indices.push_back(mesh.first_vertex + idx[t * 3 + 1]); // CW to CCW flip
        }
    }

    // 3. Animations (Frames)
    out.num_frames = num_frames;
    out.num_framechannels = num_bones * 10;
    out.frames.resize(num_frames * out.num_framechannels);
    
    out.poses.resize(num_bones);
    for (uint32_t i = 0; i < num_bones; ++i) {
        out.poses[i].parent = out.joints[i].parent;
        out.poses[i].mask = 0x3FF; // All channels
        for(int c=0; c<10; ++c) {
            out.poses[i].channeloffset[c] = 0;
            out.poses[i].channelscale[c] = 1;
        }
    }

    for (uint32_t f = 0; f < num_frames; ++f) {
        float* fout = &out.frames[f * out.num_framechannels];
        for (uint32_t p = 0; p < num_bones; ++p) {
            float t[3]; std::memcpy(t, frame_poses[f][p].origin, 12);
            float r[4]; std::memcpy(r, frame_poses[f][p].quat, 16);
            
            if (out.joints[p].parent == -1) {
                float tx = t[0], ty = t[1], tz = t[2];
                t[0] = tx; t[1] = tz; t[2] = -ty;
                
                float q_rot[4] = {-0.7071068f, 0.0f, 0.0f, 0.7071068f};
                float r_new[4];
                quat_mul(q_rot, r, r_new);
                quat_normalize(r_new);
                std::memcpy(r, r_new, 16);
            }
            
            fout[p * 10 + 0] = t[0];
            fout[p * 10 + 1] = t[1];
            fout[p * 10 + 2] = t[2];
            fout[p * 10 + 3] = r[0];
            fout[p * 10 + 4] = r[1];
            fout[p * 10 + 5] = r[2];
            fout[p * 10 + 6] = r[3];
            fout[p * 10 + 7] = 1.0f; // scale x
            fout[p * 10 + 8] = 1.0f; // scale y
            fout[p * 10 + 9] = 1.0f; // scale z
        }
    }

    // Try to find animation names (SKP frames have names!)
    for (uint32_t f = 0; f < num_frames; ++f) {
        // SKP doesn't have "clips" but individual frame names.
        // We can group them or just create a def for the whole thing.
        // For now, let's create a single animation clip spanning all frames.
    }
    
    // Check if there's a Warsow style animation.cfg
    std::string cfg_path = find_animation_cfg(skm_path);
    if (!cfg_path.empty()) {
        out.animations = parse_animation_cfg(cfg_path);
    } else {
        // Fallback: one clip for all frames
        AnimationDef ad;
        ad.name = "all";
        ad.first_frame = 0;
        ad.last_frame = num_frames - 1;
        ad.fps = BASE_FPS;
        ad.loop_frames = 0;
        out.animations.push_back(ad);
    }
    
    return true;
}
