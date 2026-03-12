#include "iqm_loader.h"
#include <iostream>
#include <fstream>
#include <cstring>

bool load_iqm(const char* path, Model& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    iqmheader hdr;
    f.read((char*)&hdr, sizeof(hdr));
    if (std::memcmp(hdr.magic, IQM_MAGIC, 16) != 0) return false;
    

    f.seekg(0, std::ios::end);
    size_t file_size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<char> buffer(file_size);
    f.read(buffer.data(), file_size);

    const char* text_pool = buffer.data() + hdr.ofs_text;

    // Joints
    out.joints.resize(hdr.num_joints);
    if (hdr.num_joints > 0) {
        const iqmjoint* iqm_joints = (const iqmjoint*)(buffer.data() + hdr.ofs_joints);
        for (uint32_t i = 0; i < hdr.num_joints; ++i) {
            out.joints[i].name = text_pool + iqm_joints[i].name;
            out.joints[i].parent = iqm_joints[i].parent;
            
            float t[3]; memcpy(t, iqm_joints[i].translate, 12);
            float r[4]; memcpy(r, iqm_joints[i].rotate, 16);
            
            if (out.joints[i].parent == -1) {
                // Root Translation: (x, y, z) -> (x, z, -y)
                float t_yup[3] = {t[0], t[2], -t[1]};
                memcpy(t, t_yup, 12);
                
                // Root Rotation: pre-multiply by -90X
                float q_rot[4] = {-0.7071068f, 0.0f, 0.0f, 0.7071068f};
                float r_new[4];
                quat_mul(q_rot, r, r_new);
                quat_normalize(r_new);
                memcpy(r, r_new, 16);
            }
            
            memcpy(out.joints[i].translate, t, 12);
            memcpy(out.joints[i].rotate, r, 16);
            memcpy(out.joints[i].scale, iqm_joints[i].scale, 12);
        }
    }

    // Meshes
    out.meshes.resize(hdr.num_meshes);
    if (hdr.num_meshes > 0) {
        const iqmmesh* iqm_meshes = (const iqmmesh*)(buffer.data() + hdr.ofs_meshes);
        for (uint32_t i = 0; i < hdr.num_meshes; ++i) {
            out.meshes[i].name = text_pool + iqm_meshes[i].name;
            out.meshes[i].material_name = text_pool + iqm_meshes[i].material;
            out.meshes[i].first_vertex = iqm_meshes[i].first_vertex;
            out.meshes[i].num_vertexes = iqm_meshes[i].num_vertexes;
            out.meshes[i].first_triangle = iqm_meshes[i].first_triangle;
            out.meshes[i].num_triangles = iqm_meshes[i].num_triangles;
        }
    }

    // Vertex data
    iqmvertexarray* va = (iqmvertexarray*)(buffer.data() + hdr.ofs_vertexarrays);
    for (uint32_t i = 0; i < hdr.num_vertexarrays; ++i) {
        const char* src = buffer.data() + va[i].offset;
        if (va[i].type == IQM_POSITION && va[i].format == IQM_FLOAT && va[i].size == 3) {
            out.positions.resize(hdr.num_vertexes * 3);
            const float* p_src = (const float*)src;
            for (uint32_t v = 0; v < hdr.num_vertexes; ++v) {
                out.positions[v*3+0] = p_src[v*3+0];
                out.positions[v*3+1] = p_src[v*3+2];
                out.positions[v*3+2] = -p_src[v*3+1];
            }
        } else if (va[i].type == IQM_NORMAL && va[i].format == IQM_FLOAT && va[i].size == 3) {
            out.normals.resize(hdr.num_vertexes * 3);
            const float* n_src = (const float*)src;
            for (uint32_t v = 0; v < hdr.num_vertexes; ++v) {
                out.normals[v*3+0] = n_src[v*3+0];
                out.normals[v*3+1] = n_src[v*3+2];
                out.normals[v*3+2] = -n_src[v*3+1];
            }
        } else if (va[i].type == IQM_TEXCOORD && va[i].format == IQM_FLOAT && va[i].size == 2) {
            out.texcoords.resize(hdr.num_vertexes * 2);
            memcpy(out.texcoords.data(), src, hdr.num_vertexes * 2 * sizeof(float));
        } else if (va[i].type == IQM_BLENDINDICES && va[i].format == IQM_UBYTE && va[i].size == 4) {
            out.joints_0.resize(hdr.num_vertexes * 4);
            memcpy(out.joints_0.data(), src, hdr.num_vertexes * 4);
        } else if (va[i].type == IQM_BLENDWEIGHTS && va[i].size == 4) {
            out.weights_0.resize(hdr.num_vertexes * 4);
            if (va[i].format == IQM_UBYTE) {
                const uint8_t* u8src = (const uint8_t*)src;
                for (uint32_t v = 0; v < hdr.num_vertexes * 4; ++v) out.weights_0[v] = u8src[v] / 255.0f;
            } else if (va[i].format == IQM_FLOAT) {
                memcpy(out.weights_0.data(), src, hdr.num_vertexes * 4 * sizeof(float));
            }
        }
    }

    // Indices
    out.indices.resize(hdr.num_triangles * 3);
    if (hdr.num_triangles > 0) {
        const uint32_t* iqm_indices = (const uint32_t*)(buffer.data() + hdr.ofs_triangles);
        for (uint32_t i = 0; i < hdr.num_triangles; ++i) {
            out.indices[i * 3 + 0] = iqm_indices[i * 3 + 0];
            out.indices[i * 3 + 1] = iqm_indices[i * 3 + 2]; // Standard-CCW flip
            out.indices[i * 3 + 2] = iqm_indices[i * 3 + 1]; // Standard-CCW flip
        }
    }

    // Animations (Unpack into sparse tracks)
    if (hdr.num_frames > 0 && hdr.num_anims > 0) {
        std::vector<iqmpose> orig_poses(hdr.num_poses);
        if (hdr.num_poses > 0) memcpy(orig_poses.data(), buffer.data() + hdr.ofs_poses, hdr.num_poses * sizeof(iqmpose));
        
        uint32_t orig_framechannels = 0;
        for (uint32_t p = 0; p < hdr.num_poses; ++p) {
            for (int c = 0; c < 10; ++c) if (orig_poses[p].mask & (1 << c)) orig_framechannels++;
        }
        
        const unsigned short* orig_fdata = (const unsigned short*)(buffer.data() + hdr.ofs_frames);
        const iqmanim* iqm_anims = (const iqmanim*)(buffer.data() + hdr.ofs_anims);

        // Pre-unpack all dense frames into a temp buffer for easy track "explosion"
        std::vector<float> dense_frames(hdr.num_frames * hdr.num_poses * 10);
        for (uint32_t f = 0; f < hdr.num_frames; ++f) {
            const unsigned short* fin = orig_fdata + f * orig_framechannels;
            float* fout = dense_frames.data() + f * hdr.num_poses * 10;
            uint32_t ch_idx = 0;
            for (size_t p = 0; p < hdr.num_poses; ++p) {
                const iqmpose& op = orig_poses[p];
                float vals[10];
                for (int c = 0; c < 10; ++c) {
                    vals[c] = op.channeloffset[c];
                    if (op.mask & (1 << c)) vals[c] += fin[ch_idx++] * op.channelscale[c];
                }

                if (out.joints[p].parent == -1) {
                    // Root Translation: (x, y, z) -> (x, z, -y)
                    float tx = vals[0], ty = vals[1], tz = vals[2];
                    vals[0] = tx; vals[1] = tz; vals[2] = -ty;
                    // Root Rotation: pre-multiply by -90X
                    float r[4] = {vals[3], vals[4], vals[5], vals[6]};
                    float q_rot[4] = {-0.7071068f, 0.0f, 0.0f, 0.7071068f};
                    float r_new[4];
                    quat_mul(q_rot, r, r_new);
                    quat_normalize(r_new);
                    memcpy(&vals[3], r_new, 16);
                }
                memcpy(fout + p * 10, vals, 40);
            }
        }

        // Animations (Unpack into sparse tracks)
        std::string cfg_path = find_animation_cfg(path);
        if (!cfg_path.empty()) {
            std::cout << "Found animation config: " << cfg_path << std::endl;
            std::vector<AnimationDef> cfg_anims = parse_animation_cfg(cfg_path);
            
            if (out.qfusion) {
                out.animations.push_back({"base", 0, 0, 0, BASE_FPS});
                out.animations.push_back({"STAND_IDLE", 1, 39, 0, BASE_FPS});
            }

            for (const auto& a : cfg_anims) {
                bool found = false;
                for (auto& existing : out.animations) {
                    if (existing.name == a.name) {
                        existing = a;
                        found = true;
                        break;
                    }
                }
                if (!found) out.animations.push_back(a);
            }
        } else {
            for (uint32_t i = 0; i < hdr.num_anims; ++i) {
                AnimationDef ad;
                ad.name = text_pool + iqm_anims[i].name;
                ad.first_frame = iqm_anims[i].first_frame;
                ad.last_frame = iqm_anims[i].first_frame + iqm_anims[i].num_frames - 1;
                ad.fps = (iqm_anims[i].framerate > 0.0f) ? iqm_anims[i].framerate : BASE_FPS;
                ad.loop_frames = (iqm_anims[i].flags & IQM_LOOP) ? 1 : 0;
                out.animations.push_back(ad);
            }
        }

        if (out.animations.empty()) {
            AnimationDef ad;
            ad.name = "base";
            ad.first_frame = 0;
            ad.last_frame = hdr.num_frames - 1;
            ad.fps = BASE_FPS;
            ad.loop_frames = 0;
            out.animations.push_back(ad);
        }

        for (auto& ad : out.animations) {
            ad.track.bones.resize(out.joints.size());
            for (int f = ad.first_frame; f <= ad.last_frame; ++f) {
                if (f < 0 || f >= (int)hdr.num_frames) continue;
                double time = (double)(f - ad.first_frame) / ad.fps;
                float* fptr = dense_frames.data() + (size_t)f * hdr.num_poses * 10;
                
                for (size_t ji = 0; ji < out.joints.size(); ++ji) {
                    BoneAnim& ba = ad.track.bones[ji];
                    float* bptr = fptr + ji * 10;
                    
                    ba.translation.times.push_back(time);
                    ba.translation.values.push_back(bptr[0]);
                    ba.translation.values.push_back(bptr[1]);
                    ba.translation.values.push_back(bptr[2]);
                    
                    ba.rotation.times.push_back(time);
                    ba.rotation.values.push_back(bptr[3]);
                    ba.rotation.values.push_back(bptr[4]);
                    ba.rotation.values.push_back(bptr[5]);
                    ba.rotation.values.push_back(bptr[6]);
                    
                    ba.scale.times.push_back(time);
                    ba.scale.values.push_back(bptr[7]);
                    ba.scale.values.push_back(bptr[8]);
                    ba.scale.values.push_back(bptr[9]);
                }
            }
        }
    }
    
    return true;
}
