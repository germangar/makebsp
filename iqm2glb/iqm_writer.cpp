#include "iqm_writer.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>

static uint32_t write_text(std::vector<char>& pool, const std::string& s) {
    if (s.empty()) return 0;
    uint32_t pos = (uint32_t)pool.size();
    for (char c : s) pool.push_back(c);
    pool.push_back('\0');
    return pos;
}

bool write_iqm(const Model& model, const char* output_path) {
    std::ofstream f(output_path, std::ios::binary);
    if (!f) return false;

    iqmheader hdr = {};
    memcpy(hdr.magic, IQM_MAGIC, 16);
    hdr.version = IQM_VERSION;

    std::vector<char> text;
    text.push_back('\0'); 

    // 1. Prepare Joints (Convert Y-up to Z-up)
    std::vector<iqmjoint> iqm_joints(model.joints.size());
    float q_rot_inv[4] = {0.7071068f, 0.0f, 0.0f, 0.7071068f}; // +90X
    
    for (size_t i = 0; i < model.joints.size(); ++i) {
        iqm_joints[i].name = write_text(text, model.joints[i].name);
        iqm_joints[i].parent = model.joints[i].parent;
        
        float t[3]; memcpy(t, model.joints[i].translate, 12);
        float r[4]; memcpy(r, model.joints[i].rotate, 16);
        quat_normalize(r);
        
        if (model.joints[i].parent == -1) {
            // Root Translation: (x, y, z) -> (x, -z, y)
            float t_zup[3] = {t[0], -t[2], t[1]};
            memcpy(t, t_zup, 12);
            
            // Root Rotation: pre-multiply by +90X
            float r_new[4];
            quat_mul(q_rot_inv, r, r_new);
            quat_normalize(r_new);
            memcpy(r, r_new, 16);
        }
        
        memcpy(iqm_joints[i].translate, t, 12);
        memcpy(iqm_joints[i].rotate, r, 16);
        memcpy(iqm_joints[i].scale, model.joints[i].scale, 12);
    }

    // 2. Prepare Meshes
    std::vector<iqmmesh> iqm_meshes(model.meshes.size());
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        iqm_meshes[i].name = write_text(text, model.meshes[i].name);
        iqm_meshes[i].material = write_text(text, model.meshes[i].material_name);
        iqm_meshes[i].first_vertex = model.meshes[i].first_vertex;
        iqm_meshes[i].num_vertexes = model.meshes[i].num_vertexes;
        iqm_meshes[i].first_triangle = model.meshes[i].first_triangle;
        iqm_meshes[i].num_triangles = model.meshes[i].num_triangles;
    }

    // 3. Prepare Animations & Poses
    std::vector<iqmanim> iqm_anims;
    if (model.qfusion && model.num_frames > 0) {
        iqmanim ad = {};
        ad.name = write_text(text, "frames");
        ad.first_frame = 0;
        ad.num_frames = model.num_frames;
        ad.framerate = BASE_FPS;
        ad.flags = IQM_LOOP;
        iqm_anims.push_back(ad);
    } else {
        iqm_anims.resize(model.animations.size());
        for (size_t i = 0; i < model.animations.size(); ++i) {
            iqm_anims[i].name = write_text(text, model.animations[i].name);
            iqm_anims[i].first_frame = model.animations[i].first_frame;
            iqm_anims[i].num_frames = model.animations[i].last_frame - model.animations[i].first_frame + 1;
            iqm_anims[i].framerate = model.animations[i].fps;
            iqm_anims[i].flags = (model.animations[i].loop_frames > 0) ? IQM_LOOP : 0;
        }
    }

    // We will build poses from full 10-channel precision
    std::vector<iqmpose> iqm_poses(model.joints.size());
    for (size_t i = 0; i < model.joints.size(); ++i) {
        iqm_poses[i].parent = model.joints[i].parent;
        iqm_poses[i].mask = 0x3FF; // all channels active for now, we'll compress if needed
    }

    std::vector<unsigned short> out_frames;
    if (!model.frames.empty() && model.num_framechannels > 0) {
        // Collect Z-up space converted frame data
        std::vector<float> zup_frames(model.frames.size());
        
        for (uint32_t f = 0; f < model.num_frames; ++f) {
            for (size_t ji = 0; ji < model.joints.size(); ++ji) {
                const float* fin = &model.frames[f * model.num_framechannels + ji * 10];
                float* fout = &zup_frames[f * model.num_framechannels + ji * 10];
                
                float vals[10]; memcpy(vals, fin, 40);
                
                // Always normalize rotations to prevent drift/mashup
                float r[4] = {vals[3], vals[4], vals[5], vals[6]};
                quat_normalize(r);
                vals[3] = r[0]; vals[4] = r[1]; vals[5] = r[2]; vals[6] = r[3];

                if (model.joints[ji].parent == -1) {
                    // Root Translation: (x, y, z) -> (x, -z, y)
                    float tx = vals[0], ty = vals[1], tz = vals[2];
                    vals[0] = tx; vals[1] = -tz; vals[2] = ty;
                    
                    // Root Rotation: pre-multiply by +90X
                    float r_val[4] = {vals[3], vals[4], vals[5], vals[6]};
                    float r_new[4];
                    quat_mul(q_rot_inv, r_val, r_new);
                    quat_normalize(r_new);
                    vals[3] = r_new[0]; vals[4] = r_new[1]; vals[5] = r_new[2]; vals[6] = r_new[3];
                }
                
                memcpy(fout, vals, 40);
            }
        }

        struct ChanStat { float min = 1e30f, max = -1e30f; };
        std::vector<std::vector<ChanStat>> stats(model.joints.size(), std::vector<ChanStat>(10));
        
        for (uint32_t f_idx = 0; f_idx < model.num_frames; ++f_idx) {
            const float* fptr = &zup_frames[f_idx * model.num_framechannels];
            for (size_t p = 0; p < model.joints.size(); ++p) {
                for (int c = 0; c < 10; ++c) {
                    float val = fptr[p * 10 + c];
                    if (val < stats[p][c].min) stats[p][c].min = val;
                    if (val > stats[p][c].max) stats[p][c].max = val;
                }
            }
        }
        
        out_frames.resize(model.frames.size());
        for (size_t p = 0; p < model.joints.size(); ++p) {
            for (int c = 0; c < 10; ++c) {
                float mn = stats[p][c].min, mx = stats[p][c].max;
                if (mn >= mx) { iqm_poses[p].channeloffset[c] = mn; iqm_poses[p].channelscale[c] = 0; }
                else { iqm_poses[p].channeloffset[c] = mn; iqm_poses[p].channelscale[c] = (mx - mn) / 65535.0f; }
            }
        }

        for (uint32_t f_idx = 0; f_idx < model.num_frames; ++f_idx) {
            const float* fptr = &zup_frames[f_idx * model.num_framechannels];
            unsigned short* out_fptr = &out_frames[f_idx * model.num_framechannels];
            for (size_t p = 0; p < model.joints.size(); ++p) {
                for (int c = 0; c < 10; ++c) {
                    float val = fptr[p * 10 + c];
                    float off = iqm_poses[p].channeloffset[c];
                    float scl = iqm_poses[p].channelscale[c];
                    if (scl > 0) out_fptr[p * 10 + c] = (unsigned short)std::max(0.0f, std::min(65535.0f, (val - off) / scl + 0.5f));
                    else out_fptr[p * 10 + c] = 0;
                }
            }
        }
    }

    // 4. Write Blocks (with strict alignment)
    uint32_t current_offset = sizeof(iqmheader);
    
    auto align = [&](uint32_t boundary) {
        uint32_t remainder = current_offset % boundary;
        if (remainder > 0) {
            uint32_t padding = boundary - remainder;
            for (uint32_t i = 0; i < padding; ++i) f.put(0);
            current_offset += padding;
        }
    };

    f.seekp(sizeof(iqmheader));

    // 1. Text Pool
    align(4);
    hdr.num_text = (uint32_t)text.size();
    hdr.ofs_text = current_offset;
    f.write(text.data(), text.size());
    current_offset += (uint32_t)text.size();

    // 2. Meshes
    align(4);
    hdr.num_meshes = (uint32_t)iqm_meshes.size();
    hdr.ofs_meshes = current_offset;
    f.write((const char*)iqm_meshes.data(), iqm_meshes.size() * sizeof(iqmmesh));
    current_offset += (uint32_t)(iqm_meshes.size() * sizeof(iqmmesh));

    std::vector<iqmvertexarray> va;
    auto write_va_data = [&](uint32_t type, uint32_t format, uint32_t size, const void* data, size_t bytes) {
        if (bytes == 0) return;
        align(4);
        va.push_back({type, 0, format, size, current_offset});
        f.write((const char*)data, bytes);
        current_offset += (uint32_t)bytes;
    };

    // Convert positions and normals from Y-up back to Z-up: (x, y, z) -> (x, -z, y)
    std::vector<float> zup_positions(model.positions.size());
    for(size_t i=0; i<model.positions.size()/3; ++i) {
        zup_positions[i*3+0] = model.positions[i*3+0];
        zup_positions[i*3+1] = -model.positions[i*3+2];
        zup_positions[i*3+2] = model.positions[i*3+1];
    }
    
    std::vector<float> zup_normals(model.normals.size());
    for(size_t i=0; i<model.normals.size()/3; ++i) {
        zup_normals[i*3+0] = model.normals[i*3+0];
        zup_normals[i*3+1] = -model.normals[i*3+2];
        zup_normals[i*3+2] = model.normals[i*3+1];
    }

    write_va_data(IQM_POSITION, IQM_FLOAT, 3, zup_positions.data(), zup_positions.size() * 4);
    write_va_data(IQM_TEXCOORD, IQM_FLOAT, 2, model.texcoords.data(), model.texcoords.size() * 4);
    write_va_data(IQM_NORMAL,   IQM_FLOAT, 3, zup_normals.data(), zup_normals.size() * 4);
    write_va_data(IQM_BLENDINDICES, IQM_UBYTE, 4, model.joints_0.data(), model.joints_0.size());
    
    std::vector<uint8_t> weights_ub;
    if (!model.weights_0.empty()) {
        weights_ub.resize(model.weights_0.size());
        for (size_t i = 0; i < model.weights_0.size(); ++i)
            weights_ub[i] = (uint8_t)std::max(0.0f, std::min(255.0f, model.weights_0[i] * 255.0f + 0.5f));
        write_va_data(IQM_BLENDWEIGHTS, IQM_UBYTE, 4, weights_ub.data(), weights_ub.size());
    }

    align(4);
    hdr.num_vertexarrays = (uint32_t)va.size();
    hdr.ofs_vertexarrays = current_offset;
    f.write((const char*)va.data(), va.size() * sizeof(iqmvertexarray));
    current_offset += (uint32_t)(va.size() * sizeof(iqmvertexarray));

    hdr.num_vertexes = (uint32_t)model.positions.size() / 3;

    // 4. Triangles
    align(4);
    hdr.num_triangles = (uint32_t)model.indices.size() / 3;
    hdr.ofs_triangles = current_offset;
    
    // Flip standard CCW winding back to IQM's backward winding
    std::vector<uint32_t> iqm_indices(model.indices.size());
    for (uint32_t i = 0; i < hdr.num_triangles; ++i) {
        iqm_indices[i * 3 + 0] = model.indices[i * 3 + 0];
        iqm_indices[i * 3 + 1] = model.indices[i * 3 + 2]; // Backward flip
        iqm_indices[i * 3 + 2] = model.indices[i * 3 + 1]; // Backward flip
    }
    
    f.write((const char*)iqm_indices.data(), iqm_indices.size() * 4);
    current_offset += (uint32_t)(iqm_indices.size() * 4);

    // 5. Joints
    align(4);
    hdr.num_joints = (uint32_t)iqm_joints.size();
    hdr.ofs_joints = current_offset;
    f.write((const char*)iqm_joints.data(), iqm_joints.size() * sizeof(iqmjoint));
    current_offset += (uint32_t)(iqm_joints.size() * sizeof(iqmjoint));

    // 6. Poses
    align(4);
    hdr.num_poses = (uint32_t)iqm_poses.size();
    hdr.ofs_poses = current_offset;
    f.write((const char*)iqm_poses.data(), iqm_poses.size() * sizeof(iqmpose));
    current_offset += (uint32_t)(iqm_poses.size() * sizeof(iqmpose));

    // 7. Animations
    align(4);
    hdr.num_anims = (uint32_t)iqm_anims.size();
    hdr.ofs_anims = current_offset;
    f.write((const char*)iqm_anims.data(), iqm_anims.size() * sizeof(iqmanim));
    current_offset += (uint32_t)(iqm_anims.size() * sizeof(iqmanim));

    // 8. Frames
    align(4);
    hdr.num_frames = model.num_frames;
    hdr.num_framechannels = model.num_framechannels;
    hdr.ofs_frames = current_offset;
    f.write((const char*)out_frames.data(), out_frames.size() * 2);
    current_offset += (uint32_t)(out_frames.size() * 2);

    // 9. Bounds
    if (model.has_bounds) {
        align(4);
        hdr.ofs_bounds = current_offset;
        uint32_t num_bounds = std::max(1u, model.num_frames);
        for (uint32_t i = 0; i < num_bounds; ++i) {
            iqmbounds b = {};
            // Y-up to Z-up (x,y,z) -> (x,-z,y)
            b.bbmin[0] = model.mins[0]; b.bbmin[1] = -model.maxs[2]; b.bbmin[2] = model.mins[1];
            b.bbmax[0] = model.maxs[0]; b.bbmax[1] = -model.mins[2]; b.bbmax[2] = model.maxs[1];
            b.xyradius = model.xyradius;
            b.radius = model.radius;
            f.write((const char*)&b, sizeof(iqmbounds));
            current_offset += sizeof(iqmbounds);
        }
    }

    // Final Header Write
    hdr.filesize = current_offset;
    f.seekp(0);
    f.write((const char*)&hdr, sizeof(hdr));

    std::cout << "IQM Stats:" << std::endl;
    std::cout << "  Joints: " << hdr.num_joints << " (Offset: " << hdr.ofs_joints << ")" << std::endl;
    std::cout << "  Meshes: " << hdr.num_meshes << " (Offset: " << hdr.ofs_meshes << ")" << std::endl;
    std::cout << "  Vertices: " << hdr.num_vertexes << " (VA Offset: " << hdr.ofs_vertexarrays << ")" << std::endl;
    std::cout << "  Triangles: " << hdr.num_triangles << " (Offset: " << hdr.ofs_triangles << ")" << std::endl;
    std::cout << "  Animations: " << hdr.num_anims << " (Offset: " << hdr.ofs_anims << ")" << std::endl;
    std::cout << "  Frames: " << hdr.num_frames << " (Offset: " << hdr.ofs_frames << ")" << std::endl;
    std::cout << "  Total Filesize: " << hdr.filesize << " bytes" << std::endl;

    // 6. Write animation.cfg
    if (!model.animations.empty()) {
        std::string cfg_path = output_path;
        size_t dot = cfg_path.find_last_of('.');
        if (dot != std::string::npos) cfg_path = cfg_path.substr(0, dot) + ".cfg";
        else cfg_path += ".cfg";
        
        std::ofstream cfg(cfg_path);
        if (cfg) {
            for (const auto& ad : model.animations) {
                cfg << ad.first_frame << " " << ad.last_frame << " " 
                    << ad.loop_frames << " " << ad.fps << " // " << ad.name << "\n";
            }
        }
    }

    std::cout << "IQM written successfully: " << output_path << "\n";
    return true;
}
