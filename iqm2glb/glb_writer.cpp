#include "glb_writer.h"
#include "cgltf.h"
#include "cgltf_write.h"
#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <cstring>
#include <cstdlib>
#include <algorithm>

static char* sanitize_name(const char* name) {
    if (!name || !name[0]) return strdup("unnamed");
    std::string s(name);
    for (char& c : s) {
        unsigned char uc = (unsigned char)c;
        if (uc < 32 || uc > 126) c = '_';
    }
    return strdup(s.c_str());
}

static void append_to_buffer(std::vector<char>& buf, cgltf_buffer* gltf_buf, const void* data, size_t bytes) {
    if (bytes == 0) return;
    buf.insert(buf.end(), (const char*)data, (const char*)data + bytes);
    gltf_buf->size = buf.size();
}

static cgltf_accessor* alloc_accessor(cgltf_data* out, cgltf_buffer_view* view, cgltf_type type, cgltf_component_type comp, size_t count, size_t byte_offset, bool normalized = false) {
    cgltf_accessor* acc = &out->accessors[out->accessors_count++];
    acc->buffer_view = view;
    acc->type = type;
    acc->component_type = comp;
    acc->count = count;
    acc->offset = byte_offset;
    acc->normalized = normalized;
    return acc;
}

bool write_glb(const Model& model, const char* output_path) {
    std::vector<char> buf;
    
    cgltf_data* out = (cgltf_data*)calloc(1, sizeof(cgltf_data));
    out->asset.version = (char*)"2.0";
    out->asset.generator = (char*)"iqm2glb";

    size_t total_acc = (model.meshes.size() * 6 + 1 + model.animations.size() * (1 + model.joints.size() * 3)) * 2;
    out->accessors = (cgltf_accessor*)calloc(total_acc, sizeof(cgltf_accessor));
    out->accessors_count = 0;

    out->buffers_count = 1;
    out->buffers = (cgltf_buffer*)calloc(1, sizeof(cgltf_buffer));

    // Buffer views: [0] Mesh Data, [1] Indices, [2] IBMs, [3] Animation Data
    out->buffer_views_count = 4;
    out->buffer_views = (cgltf_buffer_view*)calloc(out->buffer_views_count, sizeof(cgltf_buffer_view));
    for (int i = 0; i < 4; ++i) {
        out->buffer_views[i].buffer = &out->buffers[0];
    }
    out->buffer_views[0].type = cgltf_buffer_view_type_vertices;
    out->buffer_views[1].type = cgltf_buffer_view_type_indices;

    // Pack mesh data
    out->buffer_views[0].offset = 0;
    size_t pos_off = buf.size(); append_to_buffer(buf, &out->buffers[0], model.positions.data(), model.positions.size() * sizeof(float));
    size_t norm_off = buf.size(); append_to_buffer(buf, &out->buffers[0], model.normals.data(), model.normals.size() * sizeof(float));
    size_t uv_off = buf.size(); append_to_buffer(buf, &out->buffers[0], model.texcoords.data(), model.texcoords.size() * sizeof(float));
    size_t joint_off = buf.size(); append_to_buffer(buf, &out->buffers[0], model.joints_0.data(), model.joints_0.size());
    size_t weight_off = buf.size(); append_to_buffer(buf, &out->buffers[0], model.weights_0.data(), model.weights_0.size() * sizeof(float));
    out->buffer_views[0].size = buf.size();

    // Indices in view 1
    out->buffer_views[1].offset = buf.size();

    // IBMs will be packed after meshes/indices
    // Skip IBM pack for now, we'll do it later.

    // Materials
    std::map<std::string, cgltf_material*> mat_map;
    for (auto& m : model.meshes) mat_map[m.material_name] = nullptr;
    out->materials_count = mat_map.size();
    out->materials = (cgltf_material*)calloc(out->materials_count, sizeof(cgltf_material));
    uint32_t mat_idx = 0;
    for (auto& kv : mat_map) {
        kv.second = &out->materials[mat_idx++];
        kv.second->name = sanitize_name(kv.first.c_str());
        kv.second->has_pbr_metallic_roughness = true;
        kv.second->pbr_metallic_roughness.base_color_factor[0] = 1.0f;
        kv.second->pbr_metallic_roughness.base_color_factor[1] = 1.0f;
        kv.second->pbr_metallic_roughness.base_color_factor[2] = 1.0f;
        kv.second->pbr_metallic_roughness.base_color_factor[3] = 1.0f;
        kv.second->pbr_metallic_roughness.metallic_factor = 0.0f;
        kv.second->pbr_metallic_roughness.roughness_factor = 1.0f;
    }

    // Meshes logic
    out->meshes_count = model.meshes.size();
    out->meshes = (cgltf_mesh*)calloc(out->meshes_count, sizeof(cgltf_mesh));
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        cgltf_mesh* mesh = &out->meshes[i];
        mesh->name = sanitize_name(model.meshes[i].name.c_str());
        mesh->primitives_count = 1;
        mesh->primitives = (cgltf_primitive*)calloc(1, sizeof(cgltf_primitive));
        cgltf_primitive* prim = &mesh->primitives[0];
        prim->type = cgltf_primitive_type_triangles;
        prim->material = mat_map[model.meshes[i].material_name];

        prim->attributes_count = 5;
        prim->attributes = (cgltf_attribute*)calloc(5, sizeof(cgltf_attribute));
        
        prim->attributes[0].name = (char*)"POSITION"; prim->attributes[0].type = cgltf_attribute_type_position;
        prim->attributes[0].data = alloc_accessor(out, &out->buffer_views[0], cgltf_type_vec3, cgltf_component_type_r_32f, model.meshes[i].num_vertexes, pos_off + model.meshes[i].first_vertex*3*4);
        prim->attributes[0].data->has_min = prim->attributes[0].data->has_max = true;
        for(int k=0; k<3; ++k) prim->attributes[0].data->min[k] = prim->attributes[0].data->max[k] = model.positions[model.meshes[i].first_vertex*3 + k];
        for(uint32_t v=0; v<model.meshes[i].num_vertexes; ++v) {
            for(int k=0; k<3; ++k) {
                float val = model.positions[(model.meshes[i].first_vertex + v)*3 + k];
                if(val < prim->attributes[0].data->min[k]) prim->attributes[0].data->min[k] = val;
                if(val > prim->attributes[0].data->max[k]) prim->attributes[0].data->max[k] = val;
            }
        }

        prim->attributes[1].name = (char*)"NORMAL"; prim->attributes[1].type = cgltf_attribute_type_normal;
        prim->attributes[1].data = alloc_accessor(out, &out->buffer_views[0], cgltf_type_vec3, cgltf_component_type_r_32f, model.meshes[i].num_vertexes, norm_off + model.meshes[i].first_vertex*3*4);

        prim->attributes[2].name = (char*)"TEXCOORD_0"; prim->attributes[2].type = cgltf_attribute_type_texcoord;
        prim->attributes[2].data = alloc_accessor(out, &out->buffer_views[0], cgltf_type_vec2, cgltf_component_type_r_32f, model.meshes[i].num_vertexes, uv_off + model.meshes[i].first_vertex*2*4);

        prim->attributes[3].name = (char*)"JOINTS_0"; prim->attributes[3].type = cgltf_attribute_type_joints;
        prim->attributes[3].data = alloc_accessor(out, &out->buffer_views[0], cgltf_type_vec4, cgltf_component_type_r_8u, model.meshes[i].num_vertexes, joint_off + model.meshes[i].first_vertex*4);

        prim->attributes[4].name = (char*)"WEIGHTS_0"; prim->attributes[4].type = cgltf_attribute_type_weights;
        prim->attributes[4].data = alloc_accessor(out, &out->buffer_views[0], cgltf_type_vec4, cgltf_component_type_r_32f, model.meshes[i].num_vertexes, weight_off + model.meshes[i].first_vertex*4*4);

        // Indices
        std::vector<uint32_t> standard_indices(model.meshes[i].num_triangles * 3);
        for(uint32_t t=0; t<model.meshes[i].num_triangles; ++t) {
            standard_indices[t*3+0] = model.indices[model.meshes[i].first_triangle*3 + t*3+0] - model.meshes[i].first_vertex;
            standard_indices[t*3+1] = model.indices[model.meshes[i].first_triangle*3 + t*3+1] - model.meshes[i].first_vertex;
            standard_indices[t*3+2] = model.indices[model.meshes[i].first_triangle*3 + t*3+2] - model.meshes[i].first_vertex;
        }
        size_t off = buf.size();
        append_to_buffer(buf, &out->buffers[0], standard_indices.data(), standard_indices.size()*4);
        prim->indices = alloc_accessor(out, &out->buffer_views[1], cgltf_type_scalar, cgltf_component_type_r_32u, standard_indices.size(), off - out->buffer_views[1].offset);
    }
    out->buffer_views[1].size = buf.size() - out->buffer_views[1].offset;

    // IBMs
    out->buffer_views[2].offset = buf.size();
    const std::vector<mat4>& source_ibms = model.ibms.empty() ? model.computed_ibms : model.ibms;
    append_to_buffer(buf, &out->buffers[0], source_ibms.data(), source_ibms.size() * sizeof(mat4));
    out->buffer_views[2].size = buf.size() - out->buffer_views[2].offset;
    cgltf_accessor* ibm_acc = alloc_accessor(out, &out->buffer_views[2], cgltf_type_mat4, cgltf_component_type_r_32f, source_ibms.size(), 0);

    // Nodes
    out->nodes_count = model.joints.size() + model.meshes.size();
    out->nodes = (cgltf_node*)calloc(out->nodes_count, sizeof(cgltf_node));

    cgltf_node* joints_start = &out->nodes[0];
    for (size_t i = 0; i < model.joints.size(); ++i) {
        cgltf_node* n = &joints_start[i];
        n->name = sanitize_name(model.joints[i].name.c_str());
        memcpy(n->translation, model.joints[i].translate, 3*sizeof(float));
        memcpy(n->rotation, model.joints[i].rotate, 4*sizeof(float));
        memcpy(n->scale, model.joints[i].scale, 3*sizeof(float));
        n->has_translation = n->has_rotation = n->has_scale = true;
    }
    for (size_t i = 0; i < model.joints.size(); ++i) {
        int p = model.joints[i].parent;
        if (p >= 0) {
            cgltf_node* parent = &joints_start[p];
            parent->children = (cgltf_node**)realloc(parent->children, sizeof(cgltf_node*) * (parent->children_count + 1));
            parent->children[parent->children_count++] = &joints_start[i];
            joints_start[i].parent = parent;
        }
    }

    // Skin
    out->skins_count = 1;
    out->skins = (cgltf_skin*)calloc(1, sizeof(cgltf_skin));
    out->skins[0].name = sanitize_name("IQMSkin");
    out->skins[0].joints_count = model.joints.size();
    out->skins[0].joints = (cgltf_node**)calloc(model.joints.size(), sizeof(cgltf_node*));
    for(size_t i=0; i<model.joints.size(); ++i) out->skins[0].joints[i] = &joints_start[i];
    
    // Find a proper skeleton root (first joint with no parent)
    cgltf_node* skeleton_root = nullptr;
    for (size_t i = 0; i < model.joints.size(); ++i) {
        if (model.joints[i].parent == -1) {
            skeleton_root = &joints_start[i];
            break;
        }
    }
    out->skins[0].skeleton = skeleton_root;
    out->skins[0].inverse_bind_matrices = ibm_acc;

    // Default Scene
    out->scenes_count = 1;
    out->scenes = (cgltf_scene*)calloc(1, sizeof(cgltf_scene));
    out->scenes[0].name = sanitize_name("DefaultScene");

    // Gather all root nodes for the scene
    std::vector<cgltf_node*> scene_roots;
    for (size_t i = 0; i < model.joints.size(); ++i) {
        if (model.joints[i].parent == -1) {
            scene_roots.push_back(&joints_start[i]);
        }
    }

    // Attach meshes
    cgltf_node* mesh_nodes = &out->nodes[model.joints.size()];
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        mesh_nodes[i].name = sanitize_name(model.meshes[i].name.c_str());
        mesh_nodes[i].mesh = &out->meshes[i];
        mesh_nodes[i].skin = &out->skins[0];
        scene_roots.push_back(&mesh_nodes[i]);
    }

    out->scenes[0].nodes_count = scene_roots.size();
    out->scenes[0].nodes = (cgltf_node**)calloc(scene_roots.size(), sizeof(cgltf_node*));
    for (size_t i = 0; i < scene_roots.size(); ++i) {
        out->scenes[0].nodes[i] = scene_roots[i];
    }
    out->scene = &out->scenes[0];

    // Animations
    if (!model.animations.empty() && model.num_frames > 0) {
        out->animations_count = model.animations.size();
        out->animations = (cgltf_animation*)calloc(out->animations_count, sizeof(cgltf_animation));
        out->buffer_views[3].offset = buf.size();

        std::vector<uint32_t> chan_start(model.poses.size() + 1, 0);
        for(size_t i=0; i<model.poses.size(); ++i) {
            uint32_t cnt = 0;
            for(int c=0; c<10; ++c) if(model.poses[i].mask & (1<<c)) cnt++;
            chan_start[i+1] = chan_start[i] + cnt;
        }

        for (size_t ai = 0; ai < model.animations.size(); ++ai) {
            const AnimationDef& def = model.animations[ai];
            cgltf_animation* anim = &out->animations[ai];
            anim->name = sanitize_name(def.name.c_str());
            uint32_t nf = (def.last_frame >= def.first_frame) ? (def.last_frame - def.first_frame + 1) : 0;
            if (nf > 1000000) {
                std::cerr << "GLB Writer: ERROR: Animation \"" << def.name << "\" has too many frames (" << nf << "). Skipping." << std::endl;
                continue;
            }
            if (nf == 0) continue;

            std::vector<float> times(nf);
            for(uint32_t f=0; f<nf; ++f) {
                int frame_idx = (int)def.first_frame + (int)f;
                if (frame_idx < (int)model.timestamps.size()) {
                    times[f] = (float)(model.timestamps[frame_idx] - model.timestamps[def.first_frame]);
                } else {
                    times[f] = (float)f / def.fps;
                }
            }
            size_t ts_off = buf.size();
            append_to_buffer(buf, &out->buffers[0], times.data(), nf*4);
            cgltf_accessor* time_acc = alloc_accessor(out, &out->buffer_views[3], cgltf_type_scalar, cgltf_component_type_r_32f, nf, ts_off - out->buffer_views[3].offset);
            time_acc->has_min = time_acc->has_max = true;
            time_acc->min[0] = times[0]; time_acc->max[0] = times[nf-1];

            anim->samplers_count = model.joints.size() * 3;
            anim->samplers = (cgltf_animation_sampler*)calloc(anim->samplers_count, sizeof(cgltf_animation_sampler));
            anim->channels_count = model.joints.size() * 3;
            anim->channels = (cgltf_animation_channel*)calloc(anim->channels_count, sizeof(cgltf_animation_channel));

            for (size_t ji = 0; ji < model.joints.size(); ++ji) {
                std::vector<float> t_data(nf*3), r_data(nf*4), s_data(nf*3);
                for (uint32_t f = 0; f < nf; ++f) {
                    int frame_idx = std::min((int)def.first_frame + (int)f, (int)model.num_frames - 1);
                    const float* fptr = model.frames.data() + (size_t)frame_idx * model.num_framechannels + chan_start[ji];
                    float p[10];
                    const iqmpose& ip = model.poses[ji];
                    uint32_t ch = 0;
                    for(int c=0; c<10; ++c) {
                        p[c] = ip.channeloffset[c];
                        if(ip.mask & (1<<c)) p[c] += fptr[ch++] * ip.channelscale[c];
                    }
                    float q[4] = {p[3], p[4], p[5], p[6]}; quat_normalize(q);
                    
                    // Neighborhood against previous frame
                    if (f > 0) {
                        float dot = q[0]*r_data[(f-1)*4+0] + q[1]*r_data[(f-1)*4+1] + q[2]*r_data[(f-1)*4+2] + q[3]*r_data[(f-1)*4+3];
                        if (dot < 0) {
                            for(int k=0; k<4; ++k) q[k] = -q[k];
                        }
                    } else {
                        // For the first frame, neighborhood against the joint's base rotation
                        float dot = q[0]*model.joints[ji].rotate[0] + q[1]*model.joints[ji].rotate[1] + q[2]*model.joints[ji].rotate[2] + q[3]*model.joints[ji].rotate[3];
                        if (dot < 0) {
                            for(int k=0; k<4; ++k) q[k] = -q[k];
                        }
                    }
                    
                    memcpy(&t_data[f*3], p, 3*4); memcpy(&r_data[f*4], q, 4*4); memcpy(&s_data[f*3], p+7, 3*4);
                }
                auto add_ch = [&](int sid, cgltf_animation_path_type path, cgltf_type type, const std::vector<float>& data) {
                    size_t off = buf.size();
                    append_to_buffer(buf, &out->buffers[0], data.data(), data.size()*4);
                    anim->samplers[sid].input = time_acc;
                    anim->samplers[sid].output = alloc_accessor(out, &out->buffer_views[3], type, cgltf_component_type_r_32f, nf, off - out->buffer_views[3].offset);
                    anim->channels[sid].sampler = &anim->samplers[sid];
                    anim->channels[sid].target_node = &joints_start[ji];
                    anim->channels[sid].target_path = path;
                };
                add_ch(ji*3+0, cgltf_animation_path_type_translation, cgltf_type_vec3, t_data);
                add_ch(ji*3+1, cgltf_animation_path_type_rotation, cgltf_type_vec4, r_data);
                add_ch(ji*3+2, cgltf_animation_path_type_scale, cgltf_type_vec3, s_data);
            }
        }
        out->buffer_views[3].size = buf.size() - out->buffer_views[3].offset;
    }

    out->bin = buf.data();
    out->bin_size = buf.size();
    cgltf_options opts = {}; opts.type = cgltf_file_type_glb;
    cgltf_result res = cgltf_write_file(&opts, output_path, out);
    if (res == cgltf_result_success) {
        std::cout << "GLB written successfully: " << output_path << "\n";
    }
    return res == cgltf_result_success;
}
