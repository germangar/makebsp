#include "glb_loader.h"
#include "cgltf.h"
#include <iostream>
#include <vector>
#include <map>
#include <cstring>
#include <cmath>
#include <algorithm>


static void sample_channel(cgltf_animation_channel* chan, float t, float* out, int components) {
    cgltf_accessor* times = chan->sampler->input;
    cgltf_accessor* values = chan->sampler->output;
    
    if (t <= times->min[0]) {
        cgltf_accessor_read_float(values, 0, out, components);
        return;
    }
    if (t >= times->max[0]) {
        cgltf_accessor_read_float(values, times->count - 1, out, components);
        return;
    }
    
    // Binary search for interval
    uint32_t left = 0, right = times->count - 1;
    while (right - left > 1) {
        uint32_t mid = (left + right) / 2;
        float mt = 0; cgltf_accessor_read_float(times, mid, &mt, 1);
        if (t < mt) right = mid; else left = mid;
    }
    
    float t0 = 0, t1 = 0;
    cgltf_accessor_read_float(times, left, &t0, 1);
    cgltf_accessor_read_float(times, right, &t1, 1);
    float factor = (t - t0) / (t1 - t0);
    
    float v0[4], v1[4];
    cgltf_accessor_read_float(values, left, v0, components);
    cgltf_accessor_read_float(values, right, v1, components);
    
    if (chan->sampler->interpolation == cgltf_interpolation_type_step) {
        memcpy(out, v0, components * 4);
    } else if (components == 4 && chan->target_path == cgltf_animation_path_type_rotation) {
        // SLERP for quaternions
        float dot = v0[0]*v1[0] + v0[1]*v1[1] + v0[2]*v1[2] + v0[3]*v1[3];
        if (dot < 0) { dot = -dot; for(int i=0; i<4; ++i) v1[i] = -v1[i]; }
        if (dot > 0.9995f) {
            for(int i=0; i<4; ++i) out[i] = v0[i] + factor*(v1[i]-v0[i]);
        } else {
            float theta0 = acosf(dot);
            float theta = theta0 * factor;
            float s0 = cosf(theta) - dot * sin(theta) / sin(theta0);
            float s1 = sin(theta) / sin(theta0);
            for(int i=0; i<4; ++i) out[i] = s0*v0[i] + s1*v1[i];
        }
        quat_normalize(out);
    } else {
        // LERP for T/S
        for (int i = 0; i < components; ++i) out[i] = v0[i] + factor * (v1[i] - v0[i]);
    }
}

bool load_glb(const char* path, Model& out) {
    cgltf_options options = {};
    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse_file(&options, path, &data);
    if (result != cgltf_result_success) return false;

    result = cgltf_load_buffers(&options, data, path);
    if (result != cgltf_result_success) { 
        cgltf_free(data); 
        return false; 
    }

    std::cout << "GLB Nodes: " << data->nodes_count << ", Skins: " << data->skins_count << std::endl;

    // 1. Build Full Hierarchy (All Nodes)
    std::map<cgltf_node*, int> node_to_joint;
    out.joints.clear();
    
    // First pass: create all joints from all nodes to preserve hierarchy
    for (size_t i = 0; i < data->nodes_count; ++i) {
        cgltf_node* node = &data->nodes[i];
        node_to_joint[node] = (int)out.joints.size();
        
        Joint j;
        j.name = node->name ? node->name : "node_" + std::to_string(i);
        j.parent = -1; // Will set in second pass
        
        float t[3] = {0,0,0}, r[4] = {0,0,0,1}, s[3] = {1,1,1};
        if (node->has_translation) memcpy(t, node->translation, 12);
        if (node->has_rotation) memcpy(r, node->rotation, 16);
        if (node->has_scale) memcpy(s, node->scale, 12);
        memcpy(j.translate, t, 12); memcpy(j.rotate, r, 16); memcpy(j.scale, s, 12);
        
        out.joints.push_back(j);
    }
    
    // Second pass: set parent relationships
    for (size_t i = 0; i < data->nodes_count; ++i) {
        cgltf_node* node = &data->nodes[i];
        if (node->parent) {
            out.joints[i].parent = node_to_joint[node->parent];
        }
    }

    // 2. Load IBMs if a skin exists
    if (data->skins_count > 0) {
        cgltf_skin* skin = &data->skins[0];
        out.ibms.assign(out.joints.size(), mat4_identity()); // Default to identity
        
        if (skin->inverse_bind_matrices) {
            for (size_t i = 0; i < skin->joints_count; ++i) {
                int jidx = node_to_joint[skin->joints[i]];
                cgltf_accessor_read_float(skin->inverse_bind_matrices, i, (float*)&out.ibms[jidx], 16);
            }
        }
    }

    // Meshes
    for (size_t i = 0; i < data->meshes_count; ++i) {
        cgltf_mesh* m = &data->meshes[i];
        for (size_t j = 0; j < m->primitives_count; ++j) {
            cgltf_primitive* prim = &m->primitives[j];
            Mesh mesh;
            mesh.name = m->name ? m->name : "mesh_" + std::to_string(i);
            mesh.material_name = (prim->material && prim->material->name) ? prim->material->name : "default";
            mesh.first_vertex = out.positions.size() / 3;
            mesh.first_triangle = out.indices.size() / 3;
            size_t vertex_count = 0;
            for (size_t k = 0; k < prim->attributes_count; ++k) {
                cgltf_attribute* attr = &prim->attributes[k];
                vertex_count = attr->data->count;
                if (attr->type == cgltf_attribute_type_position) {
                    out.positions.resize(mesh.first_vertex * 3 + vertex_count * 3);
                    for (size_t v = 0; v < vertex_count; ++v) {
                        float* p = &out.positions[(mesh.first_vertex + v)*3];
                        cgltf_accessor_read_float(attr->data, v, p, 3);
                    }
                } else if (attr->type == cgltf_attribute_type_normal) {
                    out.normals.resize(mesh.first_vertex * 3 + vertex_count * 3);
                    for (size_t v = 0; v < vertex_count; ++v) {
                        float* p = &out.normals[(mesh.first_vertex + v)*3];
                        cgltf_accessor_read_float(attr->data, v, p, 3);
                    }
                } else if (attr->type == cgltf_attribute_type_texcoord) {
                    out.texcoords.resize(mesh.first_vertex * 2 + vertex_count * 2);
                    for (size_t v = 0; v < vertex_count; ++v)
                        cgltf_accessor_read_float(attr->data, v, &out.texcoords[(mesh.first_vertex + v)*2], 2);
                } else if (attr->type == cgltf_attribute_type_joints) {
                    out.joints_0.resize(mesh.first_vertex * 4 + vertex_count * 4);
                    cgltf_skin* skin = (data->skins_count > 0) ? &data->skins[0] : nullptr;
                    for (size_t v = 0; v < vertex_count; ++v) {
                        uint32_t id[4]; cgltf_accessor_read_uint(attr->data, v, id, 4);
                        for(int b=0; b<4; ++b) {
                            if (skin && id[b] < skin->joints_count) {
                                out.joints_0[(mesh.first_vertex + v)*4 + b] = (uint8_t)node_to_joint[skin->joints[id[b]]];
                            } else {
                                out.joints_0[(mesh.first_vertex + v)*4 + b] = (uint8_t)id[b];
                            }
                        }
                    }
                } else if (attr->type == cgltf_attribute_type_weights) {
                    out.weights_0.resize(mesh.first_vertex * 4 + vertex_count * 4);
                    for (size_t v = 0; v < vertex_count; ++v)
                        cgltf_accessor_read_float(attr->data, v, &out.weights_0[(mesh.first_vertex + v)*4], 4);
                }
            }
            mesh.num_vertexes = vertex_count;
            if (prim->indices) {
                size_t index_count = prim->indices->count;
                out.indices.resize(mesh.first_triangle * 3 + index_count);
                for (size_t v = 0; v < index_count; v += 3) {
                    out.indices[mesh.first_triangle*3 + v + 0] = (uint32_t)cgltf_accessor_read_index(prim->indices, v + 0) + mesh.first_vertex;
                    out.indices[mesh.first_triangle*3 + v + 1] = (uint32_t)cgltf_accessor_read_index(prim->indices, v + 1) + mesh.first_vertex;
                    out.indices[mesh.first_triangle*3 + v + 2] = (uint32_t)cgltf_accessor_read_index(prim->indices, v + 2) + mesh.first_vertex;
                }
                mesh.num_triangles = index_count / 3;
            }
            out.meshes.push_back(mesh);
        }
    }

    // Animations
    if (data->animations_count > 0 && !out.joints.empty()) {
        out.num_framechannels = out.joints.size() * 10;
        out.poses.resize(out.joints.size());
        for (size_t i = 0; i < out.joints.size(); ++i) {
            out.poses[i].parent = out.joints[i].parent;
            out.poses[i].mask = 0x3FF;
            for (int c = 0; c < 10; ++c) {
                out.poses[i].channeloffset[c] = (c < 3) ? 0.0f : (c < 7 ? 0.0f : 1.0f);
                out.poses[i].channelscale[c] = 1.0f;
            }
        }

        uint32_t total_frames = 0;
        float fps = BASE_FPS;
        for (size_t i = 0; i < data->animations_count; ++i) {
            cgltf_animation* anim = &data->animations[i];
            float max_t = 0;
            for (size_t j = 0; j < anim->channels_count; ++j) max_t = std::max(max_t, anim->channels[j].sampler->input->max[0]);
            uint32_t nf = (uint32_t)std::round(max_t * fps) + 1;
            
            AnimationDef ad;
            ad.name = anim->name ? anim->name : "anim_" + std::to_string(i);
            std::replace(ad.name.begin(), ad.name.end(), ' ', '_');
            ad.first_frame = total_frames;
            ad.last_frame = total_frames + nf - 1;
            ad.fps = fps;
            ad.loop_frames = 0;
            out.animations.push_back(ad);
            
            size_t frame_start = out.frames.size();
            out.frames.resize(frame_start + (size_t)nf * out.num_framechannels);
            
            for (uint32_t f = 0; f < nf; ++f) {
                float t = f / fps;
                float* fptr = &out.frames[frame_start + (size_t)f * out.num_framechannels];
                for (size_t ji = 0; ji < out.joints.size(); ++ji) {
                    float tr[3], ro[4], sc[3];
                    memcpy(tr, out.joints[ji].translate, 12);
                    memcpy(ro, out.joints[ji].rotate, 16);
                    memcpy(sc, out.joints[ji].scale, 12);
                    
                    cgltf_node* node = &data->nodes[ji];
                    for (size_t ci = 0; ci < anim->channels_count; ++ci) {
                        cgltf_animation_channel* chan = &anim->channels[ci];
                        if (chan->target_node != node) continue;
                        if (chan->target_path == cgltf_animation_path_type_translation) sample_channel(chan, t, tr, 3);
                        else if (chan->target_path == cgltf_animation_path_type_rotation) sample_channel(chan, t, ro, 4);
                        else if (chan->target_path == cgltf_animation_path_type_scale) sample_channel(chan, t, sc, 3);
                    }
                    
                    for(int c=0; c<3; ++c) fptr[c] = tr[c];
                    for(int c=0; c<4; ++c) fptr[3+c] = ro[c];
                    for(int c=0; c<3; ++c) fptr[7+c] = sc[c];
                    fptr += 10;
                }
            }
            total_frames += nf;
        }
        out.num_frames = total_frames;
    }

    if (out.ibms.empty()) {
        out.compute_bind_pose();
    }
    cgltf_free(data);
    return true;
}
