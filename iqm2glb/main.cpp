#include <iostream>
#include <vector>
#include <fstream>
#include <cstring>
#include <cmath>
#include <map>
#include <string>

#include "cgltf.h"
#include "cgltf_write.h"
#include "iqm.h"

// Helper to sanitize names for JSON compatibility
#include <algorithm>

cgltf_component_type ctype_for_format(uint32_t format) {
    switch (format) {
        case IQM_BYTE: return cgltf_component_type_r_8;
        case IQM_UBYTE: return cgltf_component_type_r_8u;
        case IQM_SHORT: return cgltf_component_type_r_16;
        case IQM_USHORT: return cgltf_component_type_r_16u;
        case IQM_INT: return cgltf_component_type_invalid; // Not supported for attributes
        case IQM_UINT: return cgltf_component_type_r_32u;
        case IQM_FLOAT: return cgltf_component_type_r_32f;
        default: return cgltf_component_type_invalid;
    }
}

cgltf_type type_for_size(uint32_t size, uint32_t iqm_type) {
    if (iqm_type == IQM_TEXCOORD) return cgltf_type_vec2;
    switch (size) {
        case 1: return cgltf_type_scalar;
        case 2: return cgltf_type_vec2;
        case 3: return cgltf_type_vec3;
        case 4: return cgltf_type_vec4;
        default: return cgltf_type_invalid;
    }
}

char* sanitize_name(const char* name) {
    if (!name) return (char*)"unnamed";
    static char buf[256];
    size_t i = 0;
    for (; i < 255 && name[i]; ++i) {
        unsigned char c = name[i];
        if (c < 32 || c > 126) buf[i] = '_';
        else buf[i] = c;
    }
    buf[i] = '\0';
    return strdup(buf);
}

struct mat4 {
    float m[16];
};

mat4 mat4_identity() {
    mat4 r = {0};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

void quat_mul(const float* a, const float* b, float* r) {
    float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    float y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    float w = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
    r[0] = x; r[1] = y; r[2] = z; r[3] = w;
}

void quat_normalize(float* q) {
    float len = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (len < 1e-12f) {
        q[0] = q[1] = q[2] = 0; q[3] = 1.0f;
    } else {
        float inv_len = 1.0f / len;
        q[0] *= inv_len; q[1] *= inv_len; q[2] *= inv_len; q[3] *= inv_len;
    }
}

mat4 mat4_mul(const mat4& a, const mat4& b) {
    mat4 r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++) sum += a.m[k*4 + row] * b.m[col*4 + k];
            r.m[col*4 + row] = sum;
        }
    }
    return r;
}

mat4 mat4_from_trs(const float* t, const float* r, const float* s) {
    mat4 m = mat4_identity();
    float x = r[0], y = r[1], z = r[2], w = r[3];
    m.m[0] = (1 - 2*y*y - 2*z*z) * s[0];
    m.m[1] = (2*x*y + 2*z*w) * s[0];
    m.m[2] = (2*x*z - 2*y*w) * s[0];
    m.m[4] = (2*x*y - 2*z*w) * s[1];
    m.m[5] = (1 - 2*x*x - 2*z*z) * s[1];
    m.m[6] = (2*y*z + 2*x*w) * s[1];
    m.m[8] = (2*x*z + 2*y*w) * s[2];
    m.m[9] = (2*y*z - 2*x*w) * s[2];
    m.m[10] = (1 - 2*x*x - 2*y*y) * s[2];
    m.m[12] = t[0]; m.m[13] = t[1]; m.m[14] = t[2];
    return m;
}

mat4 mat4_invert(const mat4& m) {
    mat4 inv;
    float s0 = m.m[0] * m.m[5] - m.m[4] * m.m[1];
    float s1 = m.m[0] * m.m[6] - m.m[4] * m.m[2];
    float s2 = m.m[0] * m.m[7] - m.m[4] * m.m[3];
    float s3 = m.m[1] * m.m[6] - m.m[5] * m.m[2];
    float s4 = m.m[1] * m.m[7] - m.m[5] * m.m[3];
    float s5 = m.m[2] * m.m[7] - m.m[6] * m.m[3];
    float c5 = m.m[10] * m.m[15] - m.m[14] * m.m[11];
    float c4 = m.m[9] * m.m[15] - m.m[13] * m.m[11];
    float c3 = m.m[9] * m.m[14] - m.m[13] * m.m[10];
    float c2 = m.m[8] * m.m[15] - m.m[12] * m.m[11];
    float c1 = m.m[8] * m.m[14] - m.m[12] * m.m[10];
    float c0 = m.m[8] * m.m[13] - m.m[12] * m.m[9];
    float det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (std::abs(det) < 1e-6) return mat4_identity();
    float idet = 1.0f / det;
    inv.m[0] = (m.m[5] * c5 - m.m[6] * c4 + m.m[7] * c3) * idet;
    inv.m[1] = (-m.m[1] * c5 + m.m[2] * c4 - m.m[3] * c3) * idet;
    inv.m[2] = (m.m[13] * s5 - m.m[14] * s4 + m.m[15] * s3) * idet;
    inv.m[3] = (-m.m[9] * s5 + m.m[10] * s4 - m.m[11] * s3) * idet;
    inv.m[4] = (-m.m[4] * c5 + m.m[6] * c2 - m.m[7] * c1) * idet;
    inv.m[5] = (m.m[0] * c5 - m.m[2] * c2 + m.m[3] * c1) * idet;
    inv.m[6] = (-m.m[12] * s5 + m.m[14] * s2 - m.m[15] * s1) * idet;
    inv.m[7] = (m.m[8] * s5 - m.m[10] * s2 + m.m[11] * s1) * idet;
    inv.m[8] = (m.m[4] * c4 - m.m[5] * c2 + m.m[7] * c0) * idet;
    inv.m[9] = (-m.m[0] * c4 + m.m[1] * c2 - m.m[3] * c0) * idet;
    inv.m[10] = (m.m[12] * s4 - m.m[13] * s2 + m.m[15] * s0) * idet;
    inv.m[11] = (-m.m[8] * s4 + m.m[9] * s2 - m.m[11] * s0) * idet;
    inv.m[12] = (-m.m[4] * c3 + m.m[5] * c1 - m.m[6] * c0) * idet;
    inv.m[13] = (m.m[0] * c3 - m.m[1] * c1 + m.m[2] * c0) * idet;
    inv.m[14] = (-m.m[12] * s3 + m.m[13] * s1 - m.m[14] * s0) * idet;
    inv.m[15] = (m.m[8] * s3 - m.m[9] * s1 + m.m[10] * s0) * idet;
    
    // Force affine integrity for glTF
    inv.m[3] = inv.m[7] = inv.m[11] = 0.0f;
    inv.m[15] = 1.0f;
    
    return inv;
}

struct AnimationDef {
    std::string name;
    int first_frame;
    int last_frame;
    int loop_frames;
    float fps;
};

std::vector<AnimationDef> parse_animation_cfg(const std::string& path) {
    std::vector<AnimationDef> anims;
    
    // Always add hardcoded 'base' (Frame 0) and 'STAND_IDLE' (Frames 1-39)
    anims.push_back({"base", 0, 0, 0, 24.0f});
    std::cout << " Parsed anim: base (0-0)" << std::endl;
    anims.push_back({"STAND_IDLE", 1, 39, 0, 24.0f});
    std::cout << " Parsed anim: STAND_IDLE (1-39)" << std::endl;
    
    std::ifstream f(path);
    if (!f.is_open()) return anims;
    
    std::string line;
    while (std::getline(f, line)) {
        // Basic trim
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        if (line.empty()) continue;
        
        // Skip specific commands
        if (line.find("sex") == 0 || line.find("rootanim") == 0 || 
            line.find("tagmask") == 0 || line.find("rotationbone") == 0) continue;
            
        // Check if it starts with a number (animation definition)
        if (std::isdigit(line[0])) {
            AnimationDef ad;
            int first, last, loop;
            float fps;
            
            // Format: first last loop fps // NAME
            if (sscanf(line.c_str(), "%d %d", &first, &last) >= 2) {
                ad.first_frame = first;
                ad.last_frame = last;
                ad.loop_frames = 0;
                ad.fps = 24.0f;
                // Try to get loop and fps if available
                sscanf(line.c_str(), "%d %d %d %f", &first, &last, &loop, &fps);
                if (line.find_first_of("0123456789", line.find_first_not_of("0123456789 \t")) != std::string::npos) {
                   ad.loop_frames = loop;
                   ad.fps = fps;
                }
                
                size_t comment_pos = line.find("//");
                if (comment_pos != std::string::npos) {
                    std::string comment = line.substr(comment_pos + 2);
                    comment.erase(0, comment.find_first_not_of(" \t"));
                    // Take the first word of the comment as name
                    size_t space_pos = comment.find_first_of(" \t\r\n");
                    if (space_pos != std::string::npos) {
                        ad.name = comment.substr(0, space_pos);
                    } else {
                        ad.name = comment;
                    }
                } else {
                    ad.name = "unnamed_" + std::to_string(anims.size());
                }
                std::cout << " Parsed anim: " << ad.name << " (" << ad.first_frame << "-" << ad.last_frame << ")" << std::endl;
                anims.push_back(ad);
            }
        }
    }
    return anims;
}

// Helper to write data to buffer and return accessor
cgltf_accessor* create_accessor(cgltf_data* data, cgltf_buffer_view* view, cgltf_type type, cgltf_component_type comp_type, size_t count, size_t offset, bool normalized = false) {
    cgltf_accessor* acc = &data->accessors[data->accessors_count++];
    acc->buffer_view = view;
    acc->type = type;
    acc->component_type = comp_type;
    acc->count = count;
    acc->offset = offset;
    acc->normalized = normalized;
    return acc;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cout << "Usage: " << argv[0] << " <input.iqm> <output.glb>" << std::endl;
        return 1;
    }

    std::ifstream f(argv[1], std::ios::binary);
    if (!f) {
        std::cerr << "Could not open " << argv[1] << std::endl;
        return 2;
    }

    iqmheader header;
    f.read((char*)&header, sizeof(header));
    if (std::memcmp(header.magic, IQM_MAGIC, 16) != 0) {
        std::cerr << "Not a valid IQM file" << std::endl;
        return 3;
    }

    f.seekg(0, std::ios::end);
    size_t size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<char> buffer(size);
    f.read(buffer.data(), size);

    // Try to load animation.cfg early to know the final animation count for allocation
    std::string cfg_path = "";
    std::string input_path = std::string(argv[1]);
    size_t last_dot_cfg = input_path.find_last_of(".");
    size_t last_slash_cfg = input_path.find_last_of("\\/");
    std::string cfg_dir = (last_slash_cfg == std::string::npos) ? "" : input_path.substr(0, last_slash_cfg + 1);
    
    std::string model_cfg = (last_dot_cfg != std::string::npos) ? input_path.substr(0, last_dot_cfg) + ".cfg" : input_path + ".cfg";
    std::string generic_cfg = cfg_dir + "animation.cfg";

    std::vector<AnimationDef> final_anims;
    if (std::ifstream(model_cfg).good()) {
        cfg_path = model_cfg;
    } else if (std::ifstream(generic_cfg).good()) {
        cfg_path = generic_cfg;
    }

    if (!cfg_path.empty()) {
        final_anims = parse_animation_cfg(cfg_path);
        std::cout << "Successfully loaded " << final_anims.size() << " animations from " << cfg_path << std::endl;
    } else {
        std::cout << "No animation config found. Using native IQM animations." << std::endl;
        iqmanim* iqm_anims_ptr = (iqmanim*)(buffer.data() + header.ofs_anims);
        for (uint32_t i = 0; i < header.num_anims; ++i) {
            AnimationDef ad;
            ad.name = (char*)(buffer.data() + header.ofs_text + iqm_anims_ptr[i].name);
            ad.first_frame = iqm_anims_ptr[i].first_frame;
            ad.last_frame = iqm_anims_ptr[i].first_frame + iqm_anims_ptr[i].num_frames - 1;
            ad.fps = iqm_anims_ptr[i].framerate;
            if (ad.fps <= 0) ad.fps = 24.0f;
            final_anims.push_back(ad);
        }
    }


    cgltf_data* out_data = (cgltf_data*)calloc(1, sizeof(cgltf_data));
    out_data->asset.version = (char*)"2.0";
    out_data->asset.generator = (char*)"iqm2glb-custom";

    // Pre-calculate and allocate accessors to keep pointers stable
    size_t total_accessors = header.num_meshes * (header.num_vertexarrays + 1) + final_anims.size() * (1 + header.num_joints * 3) + 1; // +1 for IBMs
    out_data->accessors = (cgltf_accessor*)calloc(total_accessors, sizeof(cgltf_accessor));
    out_data->accessors_count = 0;

    iqmvertexarray* iqm_va = (iqmvertexarray*)(buffer.data() + header.ofs_vertexarrays);

    // Setup Buffer
    out_data->buffers_count = 1;
    out_data->buffers = (cgltf_buffer*)calloc(1, sizeof(cgltf_buffer));
    out_data->buffers[0].size = size;

    // Buffer Views
    // [0...num_vertexarrays-1]: Vertex Arrays
    // [num_vertexarrays]: Indices
    // [num_vertexarrays+1]: Inverse Bind Matrices
    // [num_vertexarrays+2]: Animation Data
    out_data->buffer_views_count = header.num_vertexarrays + 3;
    out_data->buffer_views = (cgltf_buffer_view*)calloc(out_data->buffer_views_count, sizeof(cgltf_buffer_view));
    
    for (uint32_t i = 0; i < header.num_vertexarrays; ++i) {
        cgltf_buffer_view* vview = &out_data->buffer_views[i];
        vview->buffer = &out_data->buffers[0];
        vview->type = cgltf_buffer_view_type_vertices;
        vview->offset = iqm_va[i].offset;
        uint32_t fs = (iqm_va[i].format == IQM_FLOAT) ? 4 : (iqm_va[i].format == IQM_HALF || iqm_va[i].format == IQM_USHORT || iqm_va[i].format == IQM_SHORT) ? 2 : 1;
        vview->size = header.num_vertexes * iqm_va[i].size * fs;
        vview->stride = iqm_va[i].size * fs;
    }
    
    cgltf_buffer_view* view_indices = &out_data->buffer_views[header.num_vertexarrays];
    view_indices->buffer = &out_data->buffers[0];
    view_indices->type = cgltf_buffer_view_type_indices;

    cgltf_buffer_view* view_ibms = &out_data->buffer_views[header.num_vertexarrays + 1];
    view_ibms->buffer = &out_data->buffers[0];
    view_ibms->type = cgltf_buffer_view_type_invalid;

    cgltf_buffer_view* view_anims = &out_data->buffer_views[header.num_vertexarrays + 2];
    view_anims->buffer = &out_data->buffers[0];
    view_anims->type = cgltf_buffer_view_type_invalid;

    // Pre-calculate world matrices for IBMs
    iqmjoint* iqm_joints = (iqmjoint*)(buffer.data() + header.ofs_joints);


    std::vector<mat4> world_matrices(header.num_joints);
    std::vector<mat4> ibms(header.num_joints);
    for (uint32_t i = 0; i < header.num_joints; ++i) {
        float q[4] = { iqm_joints[i].rotate[0], iqm_joints[i].rotate[1], iqm_joints[i].rotate[2], iqm_joints[i].rotate[3] };
        quat_normalize(q);
        mat4 local = mat4_from_trs(iqm_joints[i].translate, q, iqm_joints[i].scale);
        if (iqm_joints[i].parent >= 0) {
            world_matrices[i] = mat4_mul(world_matrices[iqm_joints[i].parent], local);
        } else {
            world_matrices[i] = local;
        }
        // IBM in raw IQM (Z-up) space
        ibms[i] = mat4_invert(world_matrices[i]);
    }

    // Nodes
    out_data->nodes_count = 1 + header.num_joints + header.num_meshes;
    out_data->nodes = (cgltf_node*)calloc(out_data->nodes_count, sizeof(cgltf_node));

    cgltf_node* root = &out_data->nodes[0];
    root->name = sanitize_name("IQMRoot");
    // Axis swap: Z-up to Y-up = -90 deg rotation on X
    root->rotation[0] = -0.7071068f;
    root->rotation[1] = 0.0f;
    root->rotation[2] = 0.0f;
    root->rotation[3] = 0.7071068f;
    root->has_rotation = true;

    // Joints Node - output raw IQM transforms, no axis swap
    cgltf_node* joints_start = &out_data->nodes[1];
    for (uint32_t i = 0; i < header.num_joints; ++i) {
        cgltf_node* node = &joints_start[i];
        node->name = sanitize_name((char*)(buffer.data() + header.ofs_text + iqm_joints[i].name));
        node->translation[0] = iqm_joints[i].translate[0];
        node->translation[1] = iqm_joints[i].translate[1];
        node->translation[2] = iqm_joints[i].translate[2];
        float q[4] = { iqm_joints[i].rotate[0], iqm_joints[i].rotate[1], iqm_joints[i].rotate[2], iqm_joints[i].rotate[3] };
        quat_normalize(q);
        node->rotation[0] = q[0]; node->rotation[1] = q[1]; node->rotation[2] = q[2]; node->rotation[3] = q[3];
        
        node->scale[0] = iqm_joints[i].scale[0];
        node->scale[1] = iqm_joints[i].scale[1];
        node->scale[2] = iqm_joints[i].scale[2];
        node->has_translation = node->has_rotation = node->has_scale = true;
    }

    // Joint Hierarchy
    for (uint32_t i = 0; i < header.num_joints; ++i) {
        if (iqm_joints[i].parent >= 0) {
            cgltf_node* parent = &joints_start[iqm_joints[i].parent];
            parent->children = (cgltf_node**)realloc(parent->children, sizeof(cgltf_node*) * (parent->children_count + 1));
            parent->children[parent->children_count++] = &joints_start[i];
            joints_start[i].parent = parent;
        } else {
            root->children = (cgltf_node**)realloc(root->children, sizeof(cgltf_node*) * (root->children_count + 1));
            root->children[root->children_count++] = &joints_start[i];
            joints_start[i].parent = root;
        }
    }

    // IBM Data in Buffer
    size_t ibm_offset = buffer.size();
    buffer.insert(buffer.end(), (char*)ibms.data(), (char*)(ibms.data() + ibms.size()));
    view_ibms->offset = ibm_offset;
    view_ibms->size = ibms.size() * sizeof(mat4);
    out_data->buffers[0].size = buffer.size();

    // Skin
    out_data->skins_count = 1;
    out_data->skins = (cgltf_skin*)calloc(1, sizeof(cgltf_skin));
    cgltf_skin* skin = &out_data->skins[0];
    skin->name = sanitize_name("IQMSkin");
    skin->joints_count = header.num_joints;
    skin->joints = (cgltf_node**)calloc(header.num_joints, sizeof(cgltf_node*));
    for (uint32_t i = 0; i < header.num_joints; ++i) {
        skin->joints[i] = &joints_start[i];
    }
    skin->skeleton = root;
    skin->inverse_bind_matrices = create_accessor(out_data, view_ibms, cgltf_type_mat4, cgltf_component_type_r_32f, header.num_joints, 0);
    
    // Materials
    std::map<uint32_t, cgltf_material*> material_map;
    iqmmesh* iqm_meshes_all = (iqmmesh*)(buffer.data() + header.ofs_meshes);
    for (uint32_t i = 0; i < header.num_meshes; ++i) {
        uint32_t mat_offset = iqm_meshes_all[i].material;
        if (material_map.find(mat_offset) == material_map.end()) {
            material_map[mat_offset] = nullptr;
        }
    }

    out_data->materials_count = material_map.size();
    out_data->materials = (cgltf_material*)calloc(out_data->materials_count, sizeof(cgltf_material));
    
    uint32_t cur_mat_idx = 0;
    for (auto& pair : material_map) {
        cgltf_material* mat = &out_data->materials[cur_mat_idx++];
        const char* mat_name = (char*)(buffer.data() + header.ofs_text + pair.first);
        mat->name = sanitize_name(mat_name);
        // Default PBR to make it visible
        mat->has_pbr_metallic_roughness = true;
        mat->pbr_metallic_roughness.base_color_factor[0] = 1.0f;
        mat->pbr_metallic_roughness.base_color_factor[1] = 1.0f;
        mat->pbr_metallic_roughness.base_color_factor[2] = 1.0f;
        mat->pbr_metallic_roughness.base_color_factor[3] = 1.0f;
        mat->pbr_metallic_roughness.metallic_factor = 0.0f;
        mat->pbr_metallic_roughness.roughness_factor = 1.0f;
        pair.second = mat;
    }

    // Meshes
    out_data->meshes_count = header.num_meshes;
    out_data->meshes = (cgltf_mesh*)calloc(header.num_meshes, sizeof(cgltf_mesh));
    cgltf_node* mesh_nodes_start = &out_data->nodes[1 + header.num_joints];
    
    view_indices->offset = buffer.size();

    for (uint32_t i = 0; i < header.num_meshes; ++i) {
        // Refresh pointers in case vector reallocated
        iqmvertexarray* iqm_va_loop = (iqmvertexarray*)(buffer.data() + header.ofs_vertexarrays);
        iqmmesh* iqm_meshes_loop = (iqmmesh*)(buffer.data() + header.ofs_meshes);
        
        cgltf_mesh* mesh = &out_data->meshes[i];
        const char* mesh_name = (char*)(buffer.data() + header.ofs_text + iqm_meshes_loop[i].name);
        mesh->name = sanitize_name(mesh_name);
        mesh->primitives_count = 1;
        mesh->primitives = (cgltf_primitive*)calloc(1, sizeof(cgltf_primitive));
        cgltf_primitive* prim = &mesh->primitives[0];
        prim->type = cgltf_primitive_type_triangles;
        prim->material = material_map[iqm_meshes_loop[i].material];

        for (uint32_t j = 0; j < header.num_vertexarrays; ++j) {
            uint32_t type = iqm_va_loop[j].type;
            uint32_t iqm_offset_val = iqm_va_loop[j].offset;
            uint32_t format = iqm_va_loop[j].format;

            cgltf_attribute attr = {};
            if (type == IQM_POSITION) { attr.name = (char*)"POSITION"; attr.type = cgltf_attribute_type_position; }
            else if (type == IQM_NORMAL) { attr.name = (char*)"NORMAL"; attr.type = cgltf_attribute_type_normal; }
            else if (type == IQM_TEXCOORD) { attr.name = (char*)"TEXCOORD_0"; attr.type = cgltf_attribute_type_texcoord; }
            else if (type == IQM_BLENDINDICES) { attr.name = (char*)"JOINTS_0"; attr.type = cgltf_attribute_type_joints; }
            else if (type == IQM_BLENDWEIGHTS) { attr.name = (char*)"WEIGHTS_0"; attr.type = cgltf_attribute_type_weights; }
            else continue;

            size_t stride_val = (ctype_for_format(format) == cgltf_component_type_r_32f) ? 4 : 1;
            stride_val *= iqm_va_loop[j].size;

            attr.data = create_accessor(out_data, &out_data->buffer_views[j], type_for_size(iqm_va_loop[j].size, type), ctype_for_format(format), iqm_meshes_loop[i].num_vertexes, (iqm_meshes_loop[i].first_vertex * stride_val), (type == IQM_BLENDWEIGHTS && format != IQM_FLOAT));
            
            if (type == IQM_POSITION && format == IQM_FLOAT) {
                attr.data->has_min = attr.data->has_max = true;
                float* p_ptr = (float*)(buffer.data() + iqm_offset_val + iqm_meshes_loop[i].first_vertex * stride_val);
                
                for (int m = 0; m < 3; m++) attr.data->min[m] = attr.data->max[m] = p_ptr[m];
                for (uint32_t v = 0; v < iqm_meshes_loop[i].num_vertexes; v++) {
                    for (int m = 0; m < 3; m++) {
                        float val = p_ptr[v * 3 + m];
                        if (val < attr.data->min[m]) attr.data->min[m] = val;
                        if (val > attr.data->max[m]) attr.data->max[m] = val;
                    }
                }
            } else if (type == IQM_NORMAL && format == IQM_FLOAT) {
                // Keep raw normals
            }
            
            prim->attributes = (cgltf_attribute*)realloc(prim->attributes, sizeof(cgltf_attribute) * (prim->attributes_count + 1));
            prim->attributes[prim->attributes_count++] = attr;
        }

        // Handle indices (flip winding for glTF CCW)
        std::vector<uint32_t> flipped_indices(iqm_meshes_loop[i].num_triangles * 3);
        const uint32_t* original_indices = (const uint32_t*)(buffer.data() + header.ofs_triangles + iqm_meshes_loop[i].first_triangle * sizeof(iqmtriangle));
        for (uint32_t t = 0; t < iqm_meshes_loop[i].num_triangles; ++t) {
            flipped_indices[t * 3 + 0] = original_indices[t * 3 + 0] - iqm_meshes_loop[i].first_vertex;
            flipped_indices[t * 3 + 1] = original_indices[t * 3 + 2] - iqm_meshes_loop[i].first_vertex; // Swap
            flipped_indices[t * 3 + 2] = original_indices[t * 3 + 1] - iqm_meshes_loop[i].first_vertex; // Swap
        }
        
        size_t indices_offset = buffer.size();
        buffer.insert(buffer.end(), (char*)flipped_indices.data(), (char*)(flipped_indices.data() + flipped_indices.size()));
        out_data->buffers[0].size = buffer.size();
        
        prim->indices = create_accessor(out_data, view_indices, cgltf_type_scalar, cgltf_component_type_r_32u, iqm_meshes_loop[i].num_triangles * 3, indices_offset - view_indices->offset);

        mesh_nodes_start[i].name = mesh->name;
        mesh_nodes_start[i].mesh = mesh;
        mesh_nodes_start[i].skin = skin;
        
        root->children = (cgltf_node**)realloc(root->children, sizeof(cgltf_node*) * (root->children_count + 1));
        root->children[root->children_count++] = &mesh_nodes_start[i];
        mesh_nodes_start[i].parent = root;
    }
    view_indices->size = buffer.size() - view_indices->offset;

    // Animations - decode actual per-frame data from iqmpose + ofs_frames
    if (header.num_anims > 0 && header.num_frames > 0 && header.num_poses > 0) {
        out_data->animations_count = final_anims.size();
        out_data->animations = (cgltf_animation*)calloc(out_data->animations_count, sizeof(cgltf_animation));
        
        std::vector<iqmpose> poses_copy(header.num_poses);
        memcpy(poses_copy.data(), buffer.data() + header.ofs_poses, header.num_poses * sizeof(iqmpose));
        
        // Count total frame channels to know frame stride
        uint32_t frame_channels = 0;
        for (uint32_t p = 0; p < header.num_poses; ++p) {
            for (int c = 0; c < 10; ++c) {
                if (poses_copy[p].mask & (1 << c)) frame_channels++;
            }
        }
        
        // Copy frame data
        size_t frame_data_size = header.num_frames * frame_channels;
        std::vector<unsigned short> frames_copy(frame_data_size);
        memcpy(frames_copy.data(), buffer.data() + header.ofs_frames, frame_data_size * sizeof(unsigned short));
        
        // Copy joints for parent checks
        std::vector<iqmjoint> joints_copy(header.num_joints);
        memcpy(joints_copy.data(), buffer.data() + header.ofs_joints, header.num_joints * sizeof(iqmjoint));

        for (uint32_t i = 0; i < final_anims.size(); ++i) {
            cgltf_animation* anim = &out_data->animations[i];
            anim->name = sanitize_name(final_anims[i].name.c_str());
            
            float framerate = final_anims[i].fps;
            int first_f = final_anims[i].first_frame;
            int last_f = final_anims[i].last_frame;
            uint32_t num_frames = (last_f >= first_f) ? (last_f - first_f + 1) : 0;

            if (num_frames == 0) {
                std::cout << "Warning: Animation " << anim->name << " has 0 frames. Skipping." << std::endl;
                continue;
            }

            std::vector<float> anim_timestamps(num_frames);
            for(uint32_t f=0; f<num_frames; ++f) anim_timestamps[f] = f / framerate;
            
            size_t ts_offset = buffer.size();
            buffer.insert(buffer.end(), (char*)anim_timestamps.data(), (char*)(anim_timestamps.data() + anim_timestamps.size()));
            if (i == 0) view_anims->offset = ts_offset;
            out_data->buffers[0].size = buffer.size();

            cgltf_accessor* time_acc = create_accessor(out_data, view_anims, cgltf_type_scalar, cgltf_component_type_r_32f, num_frames, ts_offset - view_anims->offset);
            time_acc->has_min = time_acc->has_max = true;
            time_acc->min[0] = anim_timestamps[0];
            time_acc->max[0] = anim_timestamps[num_frames - 1];

            anim->channels_count = header.num_joints * 3;
            anim->channels = (cgltf_animation_channel*)calloc(anim->channels_count, sizeof(cgltf_animation_channel));
            anim->samplers_count = header.num_joints * 3;
            anim->samplers = (cgltf_animation_sampler*)calloc(anim->samplers_count, sizeof(cgltf_animation_sampler));



            for (uint32_t j = 0; j < header.num_joints; ++j) {
                auto add_channel = [&](int joint_idx, int type_idx, cgltf_animation_path_type ptype, cgltf_type gtype, const std::vector<float>& anim_data) {
                    size_t data_offset = buffer.size();
                    buffer.insert(buffer.end(), (char*)anim_data.data(), (char*)(anim_data.data() + anim_data.size()));
                    out_data->buffers[0].size = buffer.size();
                    view_anims->size = buffer.size() - view_anims->offset;

                    int idx = joint_idx * 3 + type_idx;
                    anim->samplers[idx].input = time_acc;
                    anim->samplers[idx].output = create_accessor(out_data, view_anims, gtype, cgltf_component_type_r_32f, num_frames, data_offset - view_anims->offset);
                    anim->channels[idx].sampler = &anim->samplers[idx];
                    anim->channels[idx].target_node = &joints_start[joint_idx];
                    anim->channels[idx].target_path = ptype;
                };

                std::vector<float> t_data(num_frames * 3);
                std::vector<float> r_data(num_frames * 4);
                std::vector<float> s_data(num_frames * 3);
                
                for (uint32_t f = 0; f < num_frames; ++f) {
                    // Walk through frame data to find this joint's channels
                    int frame_idx = first_f + f;
                    if (frame_idx >= (int)header.num_frames) frame_idx = header.num_frames - 1; // Clamp

                    const unsigned short* frame_ptr = frames_copy.data() + frame_idx * frame_channels;
                    
                    // Skip past preceding joints' data for this frame
                    uint32_t channel_idx = 0;
                    for (uint32_t p = 0; p < (uint32_t)j; ++p) {
                        for (int c = 0; c < 10; ++c) {
                            if (poses_copy[p].mask & (1 << c)) channel_idx++;
                        }
                    }
                    
                    // Decode pose for joint j
                    float pose[10];
                    const iqmpose& ip = poses_copy[j];
                    for (int c = 0; c < 10; ++c) {
                        pose[c] = ip.channeloffset[c];
                        if (ip.mask & (1 << c)) {
                            pose[c] += frame_ptr[channel_idx] * ip.channelscale[c];
                            channel_idx++;
                        }
                    }
                    
                    
                    // pose[0..2] = translate, pose[3..6] = rotate (xyzw), pose[7..9] = scale
                    float q[4] = { pose[3], pose[4], pose[5], pose[6] };
                    quat_normalize(q);
                    
                    // Output raw IQM transforms - no axis swap (root node handles that)
                    t_data[f*3+0] = pose[0]; 
                    t_data[f*3+1] = pose[1]; 
                    t_data[f*3+2] = pose[2];
                    r_data[f*4+0] = q[0]; r_data[f*4+1] = q[1]; r_data[f*4+2] = q[2]; r_data[f*4+3] = q[3];
                    s_data[f*3+0] = pose[7]; s_data[f*3+1] = pose[8]; s_data[f*3+2] = pose[9];
                }

                add_channel(j, 0, cgltf_animation_path_type_translation, cgltf_type_vec3, t_data);
                add_channel(j, 1, cgltf_animation_path_type_rotation, cgltf_type_vec4, r_data);
                add_channel(j, 2, cgltf_animation_path_type_scale, cgltf_type_vec3, s_data);
            }
        }

        /*
        // -- OBJ DUMPER UTILITY --
        auto export_frame_obj = [&](int target_frame_idx, const char* filename) {
            if (target_frame_idx >= (int)anims_copy[0].num_frames && target_frame_idx != -1) return;
            std::ofstream obj(filename);
            if(!obj) return;
            obj << "# IQM Frame " << target_frame_idx << "\n";
            
            // Compute joint world matrices for this frame
            std::vector<mat4> frame_world(header.num_joints);
            for (uint32_t j = 0; j < header.num_joints; ++j) {
                mat4 local;
                if (target_frame_idx == -1) {
                    float q[4] = { joints_copy[j].rotate[0], joints_copy[j].rotate[1], joints_copy[j].rotate[2], joints_copy[j].rotate[3] };
                    quat_normalize(q);
                    local = mat4_from_trs(joints_copy[j].translate, q, joints_copy[j].scale);
                } else {
                    uint32_t first_frame = anims_copy[0].first_frame;
                    const unsigned short* frame_ptr = frames_copy.data() + (first_frame + target_frame_idx) * frame_channels;
                    
                    uint32_t channel_idx = 0;
                    for (uint32_t p = 0; p < j; ++p) {
                        for (int c = 0; c < 10; ++c) if (poses_copy[p].mask & (1 << c)) channel_idx++;
                    }
                    
                    float pose[10];
                    const iqmpose& ip = poses_copy[j];
                    for (int c = 0; c < 10; ++c) {
                        pose[c] = ip.channeloffset[c];
                        if (ip.mask & (1 << c)) pose[c] += frame_ptr[channel_idx++] * ip.channelscale[c];
                    }
                    
                    float q[4] = { pose[3], pose[4], pose[5], pose[6] };
                    quat_normalize(q);
                    local = mat4_from_trs(pose, q, pose+7);
                }
                
                if (joints_copy[j].parent >= 0) {
                    frame_world[j] = mat4_mul(frame_world[joints_copy[j].parent], local);
                } else {
                    frame_world[j] = mat4_mul(rot_x_90, local); // Re-enabled root rotation
                }
            }
            
            // Re-find the buffer pointers
            iqmmesh* iqm_meshes_loop = (iqmmesh*)(buffer.data() + header.ofs_meshes);
            iqmvertexarray* va_loop = (iqmvertexarray*)(buffer.data() + header.ofs_vertexarrays);
            
            // Output vertices
            for (uint32_t i = 0; i < header.num_meshes; ++i) {
                float* pos_ptr = nullptr;
                unsigned char* blendidx_ptr = nullptr;
                float* blendweight_ptr = nullptr;
                unsigned char* blendweight_ub_ptr = nullptr;
                
                for (uint32_t j = 0; j < header.num_vertexarrays; ++j) {
                    if (va_loop[j].type == IQM_POSITION && va_loop[j].format == IQM_FLOAT) {
                        pos_ptr = (float*)(buffer.data() + va_loop[j].offset + iqm_meshes_loop[i].first_vertex * va_loop[j].size * 4);
                    } else if (va_loop[j].type == IQM_BLENDINDICES && va_loop[j].format == IQM_UBYTE) {
                        blendidx_ptr = (unsigned char*)(buffer.data() + va_loop[j].offset + iqm_meshes_loop[i].first_vertex * va_loop[j].size);
                    } else if (va_loop[j].type == IQM_BLENDWEIGHTS && va_loop[j].format == IQM_FLOAT) {
                        blendweight_ptr = (float*)(buffer.data() + va_loop[j].offset + iqm_meshes_loop[i].first_vertex * va_loop[j].size * 4);
                    } else if (va_loop[j].type == IQM_BLENDWEIGHTS && va_loop[j].format == IQM_UBYTE) {
                        blendweight_ub_ptr = (unsigned char*)(buffer.data() + va_loop[j].offset + iqm_meshes_loop[i].first_vertex * va_loop[j].size);
                    }
                }
                
                for (uint32_t v = 0; v < iqm_meshes_loop[i].num_vertexes; ++v) {
                    // Note: pos_ptr was modified in-place earlier to be Y-up.
                    float in_v[3] = { pos_ptr[v*3], pos_ptr[v*3+1], pos_ptr[v*3+2] };
                    float out_v[3] = {0};
                    
                    for (int b = 0; b < 4; ++b) {
                        int bone = blendidx_ptr[v*4+b];
                        float weight = blendweight_ptr ? blendweight_ptr[v*4+b] : (blendweight_ub_ptr[v*4+b] / 255.0f);
                        if (weight > 0.0f) {
                            mat4 skin_mat = mat4_mul(frame_world[bone], ibms[bone]);
                            float vx = in_v[0]*skin_mat.m[0] + in_v[1]*skin_mat.m[4] + in_v[2]*skin_mat.m[8] + skin_mat.m[12];
                            float vy = in_v[0]*skin_mat.m[1] + in_v[1]*skin_mat.m[5] + in_v[2]*skin_mat.m[9] + skin_mat.m[13];
                            float vz = in_v[0]*skin_mat.m[2] + in_v[1]*skin_mat.m[6] + in_v[2]*skin_mat.m[10] + skin_mat.m[14];
                            out_v[0] += vx * weight; out_v[1] += vy * weight; out_v[2] += vz * weight;
                        }
                    }
                    obj << "v " << out_v[0] << " " << out_v[1] << " " << out_v[2] << "\n";
                }
            }
            
            // Output faces
            uint32_t vertex_offset = 1;
            for (uint32_t i = 0; i < header.num_meshes; ++i) {
                const uint32_t* oidx = (const uint32_t*)(buffer.data() + header.ofs_triangles + iqm_meshes_loop[i].first_triangle * sizeof(iqmtriangle));
                for (uint32_t t = 0; t < iqm_meshes_loop[i].num_triangles; ++t) {
                    uint32_t i0 = (oidx[t*3+0] - iqm_meshes_loop[i].first_vertex) + vertex_offset;
                    uint32_t i1 = (oidx[t*3+2] - iqm_meshes_loop[i].first_vertex) + vertex_offset;
                    uint32_t i2 = (oidx[t*3+1] - iqm_meshes_loop[i].first_vertex) + vertex_offset;
                    obj << "f " << i0 << " " << i1 << " " << i2 << "\n";
                }
                vertex_offset += iqm_meshes_loop[i].num_vertexes;
            }
        };

        export_frame_obj(-1, "bind_pose.obj");
        export_frame_obj(381, "frame_381.obj");
        export_frame_obj(578, "frame_578.obj");
        // -- END OBJ DUMPER UTILITY --
        */
    }


    out_data->scenes_count = 1;
    out_data->scenes = (cgltf_scene*)calloc(1, sizeof(cgltf_scene));
    out_data->scenes[0].nodes_count = 1;
    out_data->scenes[0].nodes = (cgltf_node**)calloc(1, sizeof(cgltf_node*));
    out_data->scenes[0].nodes[0] = root;

    // IMPORTANT: Point cgltf to our data before writing
    out_data->bin = buffer.data();
    out_data->bin_size = buffer.size();



    cgltf_options options = {};
    options.type = cgltf_file_type_glb;

    cgltf_result res = cgltf_write_file(&options, argv[2], out_data);
    if (res != cgltf_result_success) {
        std::cerr << "cgltf_write_file failed" << std::endl;
        return 4;
    }

    std::cout << "GLB written successfully: " << argv[2] << std::endl;
    return 0;
}
