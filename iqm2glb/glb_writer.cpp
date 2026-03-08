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

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static char* sanitize_name(const char* name) {
    if (!name || !name[0]) return strdup("unnamed");
    std::string s(name);
    for (char& c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc > 126) c = '_';
    }
    return strdup(s.c_str());
}

static cgltf_component_type ctype_for_iqm_format(uint32_t fmt) {
    switch (fmt) {
        case IQM_BYTE:   return cgltf_component_type_r_8;
        case IQM_UBYTE:  return cgltf_component_type_r_8u;
        case IQM_SHORT:  return cgltf_component_type_r_16;
        case IQM_USHORT: return cgltf_component_type_r_16u;
        case IQM_INT:    return cgltf_component_type_invalid;
        case IQM_UINT:   return cgltf_component_type_r_32u;
        case IQM_FLOAT:  return cgltf_component_type_r_32f;
        default:         return cgltf_component_type_invalid;
    }
}

static cgltf_type type_for_iqm_va(uint32_t size, uint32_t iqm_type) {
    if (iqm_type == IQM_TEXCOORD) return cgltf_type_vec2;
    switch (size) {
        case 1: return cgltf_type_scalar;
        case 2: return cgltf_type_vec2;
        case 3: return cgltf_type_vec3;
        case 4: return cgltf_type_vec4;
        default: return cgltf_type_invalid;
    }
}

// Append data to the working buffer and update the cgltf_buffer size.
static void append_to_buffer(std::vector<char>& buf, cgltf_buffer* gltf_buf,
                             const void* data, size_t bytes) {
    buf.insert(buf.end(),
               static_cast<const char*>(data),
               static_cast<const char*>(data) + bytes);
    gltf_buf->size = buf.size();
}

// Allocate one accessor from the pre-allocated pool.
static cgltf_accessor* alloc_accessor(cgltf_data* out,
                                      cgltf_buffer_view* view,
                                      cgltf_type type,
                                      cgltf_component_type comp,
                                      size_t count,
                                      size_t byte_offset,
                                      bool normalized = false) {
    cgltf_accessor* acc  = &out->accessors[out->accessors_count++];
    acc->buffer_view     = view;
    acc->type            = type;
    acc->component_type  = comp;
    acc->count           = count;
    acc->offset          = byte_offset;
    acc->normalized      = normalized;
    return acc;
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

bool write_glb(const IQMModel& model,
               const std::vector<AnimationDef>& animations,
               const char* output_path)
{
    const iqmheader& h = model.header;

    // We will grow this buffer with all extra data we need to append.
    // Start with a copy of the raw IQM bytes (mesh/vertex/anim data is already there).
    std::vector<char> buf(model.raw_data);

    // -----------------------------------------------------------------------
    // cgltf_data + asset
    // -----------------------------------------------------------------------
    cgltf_data* out = static_cast<cgltf_data*>(calloc(1, sizeof(cgltf_data)));
    out->asset.version   = const_cast<char*>("2.0");
    out->asset.generator = const_cast<char*>("iqm2glb");

    // -----------------------------------------------------------------------
    // Accessor pool
    // We need one accessor per vertex array per mesh   (num_meshes * num_va)
    //                + one index accessor per mesh      (num_meshes)
    //                + one IBM accessor                 (1)
    // Per animation: one time accessor + per joint: T/R/S accessors
    //   = animations.size() * (1 + num_joints*3)
    // Multiply by a safety factor of 2 to be conservative.
    // -----------------------------------------------------------------------
    size_t anim_acc = animations.empty() ? 0 : animations.size() * (1 + h.num_joints * 3);
    size_t total_acc = (h.num_meshes * (h.num_vertexarrays + 1) + 1 + anim_acc) * 2;
    out->accessors       = static_cast<cgltf_accessor*>(calloc(total_acc, sizeof(cgltf_accessor)));
    out->accessors_count = 0;

    // -----------------------------------------------------------------------
    // Single buffer (backed by buf)
    // -----------------------------------------------------------------------
    out->buffers_count = 1;
    out->buffers       = static_cast<cgltf_buffer*>(calloc(1, sizeof(cgltf_buffer)));
    out->buffers[0].size = buf.size();

    // -----------------------------------------------------------------------
    // Buffer views:
    //   [0 .. num_va-1]    : vertex arrays (one per IQM vertex array)
    //   [num_va]           : indices
    //   [num_va+1]         : IBMs (inverse bind matrices)
    //   [num_va+2]         : animation data
    // -----------------------------------------------------------------------
    uint32_t BV_ANIM = h.num_vertexarrays + 2;
    out->buffer_views_count = h.num_vertexarrays + 3;
    out->buffer_views = static_cast<cgltf_buffer_view*>(
        calloc(out->buffer_views_count, sizeof(cgltf_buffer_view)));

    // Vertex array views (point directly into the IQM raw data)
    for (uint32_t i = 0; i < h.num_vertexarrays; ++i) {
        // Re-derive pointer from raw_data every time (buf may have grown, but the
        // IQM data at fixed offsets is unchanged)
        const iqmvertexarray& va = model.vertex_arrays[i];
        uint32_t elem_bytes = (va.format == IQM_FLOAT)   ? 4 :
                              (va.format == IQM_HALF ||
                               va.format == IQM_USHORT ||
                               va.format == IQM_SHORT)   ? 2 : 1;
        cgltf_buffer_view* bv = &out->buffer_views[i];
        bv->buffer = &out->buffers[0];
        bv->type   = cgltf_buffer_view_type_vertices;
        bv->offset = va.offset;
        bv->size   = h.num_vertexes * va.size * elem_bytes;
        bv->stride = va.size * elem_bytes;
    }

    cgltf_buffer_view* bv_indices = &out->buffer_views[h.num_vertexarrays];
    bv_indices->buffer = &out->buffers[0];
    bv_indices->type   = cgltf_buffer_view_type_indices;

    cgltf_buffer_view* bv_ibms = &out->buffer_views[h.num_vertexarrays + 1];
    bv_ibms->buffer = &out->buffers[0];
    bv_ibms->type   = cgltf_buffer_view_type_invalid;

    cgltf_buffer_view* bv_anim = &out->buffer_views[BV_ANIM];
    bv_anim->buffer = &out->buffers[0];
    bv_anim->type   = cgltf_buffer_view_type_invalid;

    // -----------------------------------------------------------------------
    // Inverse bind matrices
    // -----------------------------------------------------------------------
    size_t ibm_offset = buf.size();
    append_to_buffer(buf, &out->buffers[0],
                     model.ibms.data(),
                     model.ibms.size() * sizeof(mat4));
    bv_ibms->offset = ibm_offset;
    bv_ibms->size   = model.ibms.size() * sizeof(mat4);

    // -----------------------------------------------------------------------
    // Nodes: root + joints + mesh nodes
    // -----------------------------------------------------------------------
    out->nodes_count = 1 + h.num_joints + h.num_meshes;
    out->nodes = static_cast<cgltf_node*>(calloc(out->nodes_count, sizeof(cgltf_node)));

    cgltf_node* root = &out->nodes[0];
    root->name = sanitize_name("IQMRoot");
    // Axis swap: IQM is Z-up; glTF expects Y-up.  -90° around X.
    root->rotation[0] = -0.7071068f;
    root->rotation[1] =  0.0f;
    root->rotation[2] =  0.0f;
    root->rotation[3] =  0.7071068f;
    root->has_rotation = true;

    cgltf_node* joints_start = &out->nodes[1];
    for (uint32_t i = 0; i < h.num_joints; ++i) {
        const iqmjoint& j = model.joints[i];
        cgltf_node* node = &joints_start[i];
        node->name = sanitize_name(model.text + j.name);

        node->translation[0] = j.translate[0];
        node->translation[1] = j.translate[1];
        node->translation[2] = j.translate[2];

        float q[4] = { j.rotate[0], j.rotate[1], j.rotate[2], j.rotate[3] };
        quat_normalize(q);
        node->rotation[0] = q[0]; node->rotation[1] = q[1];
        node->rotation[2] = q[2]; node->rotation[3] = q[3];

        node->scale[0] = j.scale[0];
        node->scale[1] = j.scale[1];
        node->scale[2] = j.scale[2];

        node->has_translation = node->has_rotation = node->has_scale = true;
    }

    // Build joint hierarchy
    for (uint32_t i = 0; i < h.num_joints; ++i) {
        const iqmjoint& j = model.joints[i];
        if (j.parent >= 0) {
            cgltf_node* parent = &joints_start[j.parent];
            parent->children = static_cast<cgltf_node**>(
                realloc(parent->children, sizeof(cgltf_node*) * (parent->children_count + 1)));
            parent->children[parent->children_count++] = &joints_start[i];
            joints_start[i].parent = parent;
        } else {
            root->children = static_cast<cgltf_node**>(
                realloc(root->children, sizeof(cgltf_node*) * (root->children_count + 1)));
            root->children[root->children_count++] = &joints_start[i];
            joints_start[i].parent = root;
        }
    }

    // -----------------------------------------------------------------------
    // Skin
    // -----------------------------------------------------------------------
    out->skins_count = 1;
    out->skins = static_cast<cgltf_skin*>(calloc(1, sizeof(cgltf_skin)));
    cgltf_skin* skin = &out->skins[0];
    skin->name = sanitize_name("IQMSkin");
    skin->joints_count = h.num_joints;
    skin->joints = static_cast<cgltf_node**>(calloc(h.num_joints, sizeof(cgltf_node*)));
    for (uint32_t i = 0; i < h.num_joints; ++i) skin->joints[i] = &joints_start[i];
    skin->skeleton = root;
    skin->inverse_bind_matrices = alloc_accessor(out, bv_ibms, cgltf_type_mat4,
                                                  cgltf_component_type_r_32f,
                                                  h.num_joints, 0);

    // -----------------------------------------------------------------------
    // Materials  (unique by IQM text offset)
    // -----------------------------------------------------------------------
    std::map<uint32_t, cgltf_material*> mat_map;
    for (uint32_t i = 0; i < h.num_meshes; ++i)
        mat_map.emplace(model.meshes[i].material, nullptr);

    out->materials_count = mat_map.size();
    out->materials = static_cast<cgltf_material*>(
        calloc(out->materials_count, sizeof(cgltf_material)));

    uint32_t mat_idx = 0;
    for (auto& kv : mat_map) {
        cgltf_material* mat = &out->materials[mat_idx++];
        mat->name = sanitize_name(model.text + kv.first);
        mat->has_pbr_metallic_roughness = true;
        mat->pbr_metallic_roughness.base_color_factor[0] = 1.0f;
        mat->pbr_metallic_roughness.base_color_factor[1] = 1.0f;
        mat->pbr_metallic_roughness.base_color_factor[2] = 1.0f;
        mat->pbr_metallic_roughness.base_color_factor[3] = 1.0f;
        mat->pbr_metallic_roughness.metallic_factor  = 0.0f;
        mat->pbr_metallic_roughness.roughness_factor = 1.0f;
        kv.second = mat;
    }

    // -----------------------------------------------------------------------
    // Meshes
    // -----------------------------------------------------------------------
    cgltf_node* mesh_nodes_start = &out->nodes[1 + h.num_joints];
    out->meshes_count = h.num_meshes;
    out->meshes = static_cast<cgltf_mesh*>(calloc(h.num_meshes, sizeof(cgltf_mesh)));

    // Index data starts at the current end of buf
    bv_indices->offset = buf.size();

    for (uint32_t mi = 0; mi < h.num_meshes; ++mi) {
        // Re-derive raw pointer each iteration (buf may grow in this loop)
        const iqmmesh& iqm_mesh = model.meshes[mi];

        cgltf_mesh*      mesh = &out->meshes[mi];
        cgltf_primitive* prim;

        mesh->name = sanitize_name(model.text + iqm_mesh.name);
        mesh->primitives_count = 1;
        mesh->primitives = static_cast<cgltf_primitive*>(calloc(1, sizeof(cgltf_primitive)));
        prim = &mesh->primitives[0];
        prim->type     = cgltf_primitive_type_triangles;
        prim->material = mat_map[iqm_mesh.material];

        // -- Vertex attributes --
        for (uint32_t vi = 0; vi < h.num_vertexarrays; ++vi) {
            const iqmvertexarray& va = model.vertex_arrays[vi];
            uint32_t iqm_type = va.type;
            uint32_t fmt      = va.format;

            cgltf_attribute attr = {};
            switch (iqm_type) {
                case IQM_POSITION:     attr.name = const_cast<char*>("POSITION");   attr.type = cgltf_attribute_type_position;  break;
                case IQM_NORMAL:       attr.name = const_cast<char*>("NORMAL");     attr.type = cgltf_attribute_type_normal;    break;
                case IQM_TEXCOORD:     attr.name = const_cast<char*>("TEXCOORD_0"); attr.type = cgltf_attribute_type_texcoord;  break;
                case IQM_BLENDINDICES: attr.name = const_cast<char*>("JOINTS_0");   attr.type = cgltf_attribute_type_joints;    break;
                case IQM_BLENDWEIGHTS: attr.name = const_cast<char*>("WEIGHTS_0");  attr.type = cgltf_attribute_type_weights;   break;
                default: continue;
            }

            uint32_t elem_bytes = (ctype_for_iqm_format(fmt) == cgltf_component_type_r_32f) ? 4 : 1;
            size_t stride = va.size * elem_bytes;

            bool normalized = (iqm_type == IQM_BLENDWEIGHTS && fmt != IQM_FLOAT);
            attr.data = alloc_accessor(out, &out->buffer_views[vi],
                                       type_for_iqm_va(va.size, iqm_type),
                                       ctype_for_iqm_format(fmt),
                                       iqm_mesh.num_vertexes,
                                       iqm_mesh.first_vertex * stride,
                                       normalized);

            // POSITION: compute AABB min/max
            if (iqm_type == IQM_POSITION && fmt == IQM_FLOAT) {
                attr.data->has_min = attr.data->has_max = true;
                const float* p = reinterpret_cast<const float*>(
                    model.raw_data.data() + va.offset + iqm_mesh.first_vertex * stride);
                for (int k = 0; k < 3; k++) attr.data->min[k] = attr.data->max[k] = p[k];
                for (uint32_t v = 0; v < iqm_mesh.num_vertexes; ++v) {
                    for (int k = 0; k < 3; ++k) {
                        float val = p[v * 3 + k];
                        if (val < attr.data->min[k]) attr.data->min[k] = val;
                        if (val > attr.data->max[k]) attr.data->max[k] = val;
                    }
                }
            }

            prim->attributes = static_cast<cgltf_attribute*>(
                realloc(prim->attributes, sizeof(cgltf_attribute) * (prim->attributes_count + 1)));
            prim->attributes[prim->attributes_count++] = attr;
        }

        // -- Indices: flip winding order (IQM is CW; glTF expects CCW) --
        const uint32_t idx_count = iqm_mesh.num_triangles * 3;
        std::vector<uint32_t> flipped(idx_count);
        const uint32_t* src = reinterpret_cast<const uint32_t*>(
            model.raw_data.data() + h.ofs_triangles +
            iqm_mesh.first_triangle * sizeof(iqmtriangle));
        for (uint32_t t = 0; t < iqm_mesh.num_triangles; ++t) {
            flipped[t*3+0] = src[t*3+0] - iqm_mesh.first_vertex;
            flipped[t*3+1] = src[t*3+2] - iqm_mesh.first_vertex; // swapped
            flipped[t*3+2] = src[t*3+1] - iqm_mesh.first_vertex; // swapped
        }

        size_t idx_offset = buf.size();
        append_to_buffer(buf, &out->buffers[0], flipped.data(), flipped.size() * sizeof(uint32_t));

        prim->indices = alloc_accessor(out, bv_indices, cgltf_type_scalar,
                                       cgltf_component_type_r_32u, idx_count,
                                       idx_offset - bv_indices->offset);

        // Attach mesh to its scene node
        mesh_nodes_start[mi].name = mesh->name;
        mesh_nodes_start[mi].mesh = mesh;
        mesh_nodes_start[mi].skin = skin;
        root->children = static_cast<cgltf_node**>(
            realloc(root->children, sizeof(cgltf_node*) * (root->children_count + 1)));
        root->children[root->children_count++] = &mesh_nodes_start[mi];
        mesh_nodes_start[mi].parent = root;
    }
    bv_indices->size = buf.size() - bv_indices->offset;

    // -----------------------------------------------------------------------
    // Animations
    // -----------------------------------------------------------------------
    if (!animations.empty() && h.num_frames > 0 && h.num_poses > 0) {
        out->animations_count = animations.size();
        out->animations = static_cast<cgltf_animation*>(
            calloc(out->animations_count, sizeof(cgltf_animation)));

        // Read pose + frame data from the (still unmodified) beginning of buf
        const iqmpose*          poses_raw = reinterpret_cast<const iqmpose*>(
            model.raw_data.data() + h.ofs_poses);
        const unsigned short*   frames_raw = reinterpret_cast<const unsigned short*>(
            model.raw_data.data() + h.ofs_frames);

        // Make local copies so we have safe pointers independent of buf
        std::vector<iqmpose>         poses(poses_raw, poses_raw + h.num_poses);
        std::vector<unsigned short>  frames(frames_raw,
            frames_raw + static_cast<size_t>(h.num_frames) * model.frame_channels);

        // Animation view starts at the current end of buf (set on first use)
        bool anim_view_started = false;

        for (uint32_t ai = 0; ai < animations.size(); ++ai) {
            const AnimationDef& def = animations[ai];
            cgltf_animation*    anim = &out->animations[ai];
            anim->name = sanitize_name(def.name.c_str());

            int   first_f   = def.first_frame;
            int   last_f    = def.last_frame;
            float framerate = (def.fps > 0.0f) ? def.fps : 24.0f;
            uint32_t nf = (last_f >= first_f) ? static_cast<uint32_t>(last_f - first_f + 1) : 0;

            if (nf == 0) {
                std::cout << "Warning: animation '" << def.name << "' has 0 frames, skipping.\n";
                continue;
            }

            // -- Timestamps --
            std::vector<float> timestamps(nf);
            for (uint32_t f = 0; f < nf; ++f) timestamps[f] = f / framerate;

            size_t ts_off = buf.size();
            if (!anim_view_started) {
                bv_anim->offset  = ts_off;
                anim_view_started = true;
            }
            append_to_buffer(buf, &out->buffers[0], timestamps.data(), nf * sizeof(float));
            bv_anim->size = buf.size() - bv_anim->offset;

            cgltf_accessor* time_acc = alloc_accessor(out, bv_anim, cgltf_type_scalar,
                                                       cgltf_component_type_r_32f, nf,
                                                       ts_off - bv_anim->offset);
            time_acc->has_min = time_acc->has_max = true;
            time_acc->min[0]  = timestamps[0];
            time_acc->max[0]  = timestamps[nf - 1];

            // 3 samplers/channels per joint (T, R, S)
            anim->samplers_count = h.num_joints * 3;
            anim->samplers = static_cast<cgltf_animation_sampler*>(
                calloc(anim->samplers_count, sizeof(cgltf_animation_sampler)));
            anim->channels_count = h.num_joints * 3;
            anim->channels = static_cast<cgltf_animation_channel*>(
                calloc(anim->channels_count, sizeof(cgltf_animation_channel)));

            // Pre-compute per-pose channel offsets (how many channels precede pose p)
            std::vector<uint32_t> pose_chan_start(h.num_poses + 1, 0);
            for (uint32_t p = 0; p < h.num_poses; ++p) {
                uint32_t cnt = 0;
                for (int c = 0; c < 10; ++c)
                    if (poses[p].mask & (1u << c)) cnt++;
                pose_chan_start[p + 1] = pose_chan_start[p] + cnt;
            }

            for (uint32_t ji = 0; ji < h.num_joints; ++ji) {
                std::vector<float> t_data(nf * 3);
                std::vector<float> r_data(nf * 4);
                std::vector<float> s_data(nf * 3);

                for (uint32_t f = 0; f < nf; ++f) {
                    int frame_idx = first_f + static_cast<int>(f);
                    if (frame_idx >= static_cast<int>(h.num_frames))
                        frame_idx = static_cast<int>(h.num_frames) - 1;

                    const unsigned short* fptr = frames.data() +
                        static_cast<size_t>(frame_idx) * model.frame_channels +
                        pose_chan_start[ji];

                    float pose[10];
                    const iqmpose& ip = poses[ji];
                    uint32_t ch = 0;
                    for (int c = 0; c < 10; ++c) {
                        pose[c] = ip.channeloffset[c];
                        if (ip.mask & (1u << c))
                            pose[c] += fptr[ch++] * ip.channelscale[c];
                    }

                    float q[4] = { pose[3], pose[4], pose[5], pose[6] };
                    quat_normalize(q);

                    t_data[f*3+0] = pose[0]; t_data[f*3+1] = pose[1]; t_data[f*3+2] = pose[2];
                    r_data[f*4+0] = q[0];    r_data[f*4+1] = q[1];    r_data[f*4+2] = q[2];    r_data[f*4+3] = q[3];
                    s_data[f*3+0] = pose[7]; s_data[f*3+1] = pose[8]; s_data[f*3+2] = pose[9];
                }

                // Lambda to append one TRS channel
                auto add_channel = [&](int type_idx,
                                       cgltf_animation_path_type ptype,
                                       cgltf_type gtype,
                                       const std::vector<float>& data) {
                    size_t off = buf.size();
                    append_to_buffer(buf, &out->buffers[0], data.data(), data.size() * sizeof(float));
                    bv_anim->size = buf.size() - bv_anim->offset;

                    int idx = static_cast<int>(ji) * 3 + type_idx;
                    anim->samplers[idx].input  = time_acc;
                    anim->samplers[idx].output = alloc_accessor(out, bv_anim, gtype,
                                                                 cgltf_component_type_r_32f,
                                                                 nf, off - bv_anim->offset);
                    anim->channels[idx].sampler     = &anim->samplers[idx];
                    anim->channels[idx].target_node = &joints_start[ji];
                    anim->channels[idx].target_path = ptype;
                };

                add_channel(0, cgltf_animation_path_type_translation, cgltf_type_vec3, t_data);
                add_channel(1, cgltf_animation_path_type_rotation,    cgltf_type_vec4, r_data);
                add_channel(2, cgltf_animation_path_type_scale,       cgltf_type_vec3, s_data);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Scene
    // -----------------------------------------------------------------------
    out->scenes_count = 1;
    out->scenes = static_cast<cgltf_scene*>(calloc(1, sizeof(cgltf_scene)));
    out->scenes[0].nodes_count = 1;
    out->scenes[0].nodes = static_cast<cgltf_node**>(calloc(1, sizeof(cgltf_node*)));
    out->scenes[0].nodes[0] = root;

    // -----------------------------------------------------------------------
    // Write GLB
    // -----------------------------------------------------------------------
    out->bin      = buf.data();
    out->bin_size = buf.size();

    cgltf_options opts = {};
    opts.type = cgltf_file_type_glb;

    cgltf_result res = cgltf_write_file(&opts, output_path, out);
    if (res != cgltf_result_success) {
        std::cerr << "cgltf_write_file failed (code " << res << ")\n";
        return false;
    }

    std::cout << "GLB written successfully: " << output_path << "\n";
    return true;
}
