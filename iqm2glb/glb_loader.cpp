#include "glb_loader.h"
#include "cgltf.h"
#include <iostream>
#include <vector>
#include <map>
#include <cstring>
#include <cmath>
#include <algorithm>

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

    std::map<cgltf_node*, int> node_to_joint;
    
    // 1. Load Joints
    for (size_t i = 0; i < data->nodes_count; ++i) {
        cgltf_node* node = &data->nodes[i];
        if (node->camera || node->light) continue; 
        
        node_to_joint[node] = (int)out.joints.size();
        Joint j;
        j.name = node->name ? node->name : "node_" + std::to_string(i);
        j.parent = -1; // Will set later
        
        if (node->has_translation) memcpy(j.translate, node->translation, 12);
        else { j.translate[0] = j.translate[1] = j.translate[2] = 0.0f; }
        
        if (node->has_rotation) memcpy(j.rotate, node->rotation, 16);
        else { j.rotate[0] = j.rotate[1] = j.rotate[2] = 0.0f; j.rotate[3] = 1.0f; }
        
        if (node->has_scale) memcpy(j.scale, node->scale, 12);
        else { j.scale[0] = j.scale[1] = j.scale[2] = 1.0f; }
        
        out.joints.push_back(j);
    }
    
    // Set parents
    for (size_t i = 0; i < data->nodes_count; ++i) {
        cgltf_node* node = &data->nodes[i];
        if (node_to_joint.count(node) && node->parent && node_to_joint.count(node->parent)) {
            out.joints[node_to_joint[node]].parent = node_to_joint[node->parent];
        }
    }

    // 2. Load Meshes (simplified - take first mesh)
    for (size_t i = 0; i < data->meshes_count; ++i) {
        cgltf_mesh* mesh = &data->meshes[i];
        for (size_t j = 0; j < mesh->primitives_count; ++j) {
            cgltf_primitive* prim = &mesh->primitives[j];
            if (prim->type != cgltf_primitive_type_triangles) continue;

            Mesh m;
            m.name = mesh->name ? mesh->name : "mesh";
            m.first_vertex = (uint32_t)out.positions.size() / 3;
            m.first_triangle = (uint32_t)out.indices.size() / 3;

            // Attributes
            for (size_t k = 0; k < prim->attributes_count; ++k) {
                cgltf_attribute* attr = &prim->attributes[k];
                if (attr->type == cgltf_attribute_type_position) {
                    size_t start = out.positions.size();
                    out.positions.resize(start + attr->data->count * 3);
                    for (size_t v = 0; v < attr->data->count; ++v)
                        cgltf_accessor_read_float(attr->data, v, &out.positions[start + v * 3], 3);
                    m.num_vertexes = (uint32_t)attr->data->count;
                } else if (attr->type == cgltf_attribute_type_normal) {
                    size_t start = out.normals.size();
                    out.normals.resize(start + attr->data->count * 3);
                    for (size_t v = 0; v < attr->data->count; ++v)
                        cgltf_accessor_read_float(attr->data, v, &out.normals[start + v * 3], 3);
                } else if (attr->type == cgltf_attribute_type_texcoord) {
                    size_t start = out.texcoords.size();
                    out.texcoords.resize(start + attr->data->count * 2);
                    for (size_t v = 0; v < attr->data->count; ++v)
                        cgltf_accessor_read_float(attr->data, v, &out.texcoords[start + v * 2], 2);
                } else if (attr->type == cgltf_attribute_type_joints) {
                    size_t start = out.joints_0.size();
                    out.joints_0.resize(start + attr->data->count * 4);
                    // cgltf handles type conversion from u16 to u8 if needed
                    for (size_t v = 0; v < attr->data->count; ++v) {
                        uint32_t joints[4];
                        cgltf_accessor_read_uint(attr->data, v, joints, 4);
                        for(int c=0; c<4; ++c) out.joints_0[start + v*4+c] = (uint8_t)joints[c];
                    }
                } else if (attr->type == cgltf_attribute_type_weights) {
                    size_t start = out.weights_0.size();
                    out.weights_0.resize(start + attr->data->count * 4);
                    for (size_t v = 0; v < attr->data->count; ++v)
                        cgltf_accessor_read_float(attr->data, v, &out.weights_0[start + v * 4], 4);
                }
            }

            // Indices
            if (prim->indices) {
                size_t start = out.indices.size();
                out.indices.resize(start + prim->indices->count);
                for (size_t v = 0; v < prim->indices->count; ++v)
                    out.indices[start + v] = (uint32_t)cgltf_accessor_read_index(prim->indices, v);
                m.num_triangles = (uint32_t)prim->indices->count / 3;
            }

            out.meshes.push_back(m);
        }
    }

    // 3. Animations (Sparse Ingest)
    if (data->animations_count > 0 && !out.joints.empty()) {
        for (size_t i = 0; i < data->animations_count; ++i) {
            cgltf_animation* anim = &data->animations[i];
            
            AnimationDef ad;
            ad.name = anim->name ? anim->name : "anim_" + std::to_string(i);
            std::replace(ad.name.begin(), ad.name.end(), ' ', '_');
            ad.fps = BASE_FPS;
            ad.loop_frames = 0;
            ad.track.bones.resize(out.joints.size());

            for (size_t j = 0; j < anim->channels_count; ++j) {
                cgltf_animation_channel* chan = &anim->channels[j];
                if (!chan->target_node) continue;
                
                int ji = node_to_joint[chan->target_node];
                BoneAnim& ba = ad.track.bones[ji];
                AnimChannel* target = nullptr;

                if (chan->target_path == cgltf_animation_path_type_translation) target = &ba.translation;
                else if (chan->target_path == cgltf_animation_path_type_rotation) target = &ba.rotation;
                else if (chan->target_path == cgltf_animation_path_type_scale) target = &ba.scale;

                if (target && chan->sampler->input && chan->sampler->output) {
                    cgltf_accessor* times = chan->sampler->input;
                    cgltf_accessor* values = chan->sampler->output;
                    
                    target->times.resize(times->count);
                    for (size_t k = 0; k < times->count; ++k) {
                        float t;
                        cgltf_accessor_read_float(times, k, &t, 1);
                        target->times[k] = (double)t;
                    }

                    int components = (chan->target_path == cgltf_animation_path_type_rotation) ? 4 : 3;
                    target->values.resize(values->count * components);
                    for (size_t k = 0; k < values->count; ++k) {
                        cgltf_accessor_read_float(values, k, &target->values[k * components], components);
                    }
                }
            }
            out.animations.push_back(ad);
        }
    }
    
    cgltf_free(data);
    return true;
}
