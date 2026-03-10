#include "fbx_writer.h"
#include "fbx.hpp"
#include <iostream>
#include <map>
#include <ctime>
#include <algorithm>

// FBX constants
const int64_t KTIME_ONE_SECOND = 46186158000LL;

// Helper to generate a unique 64-bit ID for FBX nodes
static int64_t generate_id() {
    static int64_t last_id = 2000000;
    return ++last_id;
}

bool write_fbx(const char* path, const Model& in, bool write_mesh, bool write_anim, int anim_index) {
    Fbx::Record file;
    
    // 1. Header Extension
    Fbx::Record* hdr_ext = new Fbx::Record("FBXHeaderExtension", &file);
    (*hdr_ext->insert(new Fbx::Record("FBXHeaderVersion")))->properties().insert(new Fbx::Property((int32_t)1003));
    (*hdr_ext->insert(new Fbx::Record("FBXVersion")))->properties().insert(new Fbx::Property((int32_t)7400));
    
    time_t now = time(0);
    tm* ltm = localtime(&now);
    Fbx::Record* creation_time = new Fbx::Record("CreationTimeStamp", hdr_ext);
    (*creation_time->insert(new Fbx::Record("Version")))->properties().insert(new Fbx::Property((int32_t)1000));
    (*creation_time->insert(new Fbx::Record("Year")))->properties().insert(new Fbx::Property((int32_t)(1900 + ltm->tm_year)));
    (*creation_time->insert(new Fbx::Record("Month")))->properties().insert(new Fbx::Property((int32_t)(1 + ltm->tm_mon)));
    (*creation_time->insert(new Fbx::Record("Day")))->properties().insert(new Fbx::Property((int32_t)ltm->tm_mday));
    (*creation_time->insert(new Fbx::Record("Hour")))->properties().insert(new Fbx::Property((int32_t)ltm->tm_hour));
    (*creation_time->insert(new Fbx::Record("Minute")))->properties().insert(new Fbx::Property((int32_t)ltm->tm_min));
    (*creation_time->insert(new Fbx::Record("Second")))->properties().insert(new Fbx::Property((int32_t)ltm->tm_sec));
    (*creation_time->insert(new Fbx::Record("Millisecond")))->properties().insert(new Fbx::Property((int32_t)0));
    
    (*hdr_ext->insert(new Fbx::Record("Creator")))->properties().insert(new Fbx::Property("iqm2glb - Antigravity Agent"));
    
    Fbx::Record* metadata = new Fbx::Record("Metadata", hdr_ext);
    (*metadata->insert(new Fbx::Record("Version")))->properties().insert(new Fbx::Property((int32_t)100));
    (*metadata->insert(new Fbx::Record("Title")))->properties().insert(new Fbx::Property(""));
    (*metadata->insert(new Fbx::Record("Subject")))->properties().insert(new Fbx::Property(""));
    (*metadata->insert(new Fbx::Record("Author")))->properties().insert(new Fbx::Property(""));
    (*metadata->insert(new Fbx::Record("Keywords")))->properties().insert(new Fbx::Property(""));
    (*metadata->insert(new Fbx::Record("Revision")))->properties().insert(new Fbx::Property(""));
    (*metadata->insert(new Fbx::Record("Comment")))->properties().insert(new Fbx::Property(""));

    // 2. GlobalSettings
    Fbx::Record* gs = new Fbx::Record("GlobalSettings", &file);
    (*gs->insert(new Fbx::Record("Version")))->properties().insert(new Fbx::Property((int32_t)1000));
    Fbx::Record* props70 = new Fbx::Record("Properties70", gs);
    
    auto add_prop70 = [&](Fbx::Record* parent, const char* name, const char* type, const char* label, const char* flag, auto value) {
        Fbx::Record* p = new Fbx::Record("P", parent);
        p->properties().insert(new Fbx::Property(name));
        p->properties().insert(new Fbx::Property(type));
        p->properties().insert(new Fbx::Property(label));
        p->properties().insert(new Fbx::Property(flag));
        p->properties().insert(new Fbx::Property(value));
    };

    add_prop70(props70, "UpAxis", "int", "Integer", "", (int32_t)1); // Y-up
    add_prop70(props70, "UpAxisSign", "int", "Integer", "", (int32_t)1);
    add_prop70(props70, "FrontAxis", "int", "Integer", "", (int32_t)2); // Z
    add_prop70(props70, "FrontAxisSign", "int", "Integer", "", (int32_t)1);
    add_prop70(props70, "CoordAxis", "int", "Integer", "", (int32_t)0); // Right
    add_prop70(props70, "CoordAxisSign", "int", "Integer", "", (int32_t)1);
    add_prop70(props70, "OriginalUpAxis", "int", "Integer", "", (int32_t)1);
    add_prop70(props70, "OriginalUpAxisSign", "int", "Integer", "", (int32_t)1);
    add_prop70(props70, "UnitScaleFactor", "double", "Number", "", (double)100.0);
    add_prop70(props70, "OriginalUnitScaleFactor", "double", "Number", "", (double)100.0);

    // 3. Documents
    Fbx::Record* docs = new Fbx::Record("Documents", &file);
    (*docs->insert(new Fbx::Record("Count")))->properties().insert(new Fbx::Property((int32_t)1));
    Fbx::Record* doc = new Fbx::Record("Document", docs);
    int64_t doc_id = generate_id();
    doc->properties().insert(new Fbx::Property(doc_id));
    doc->properties().insert(new Fbx::Property(""));
    doc->properties().insert(new Fbx::Property("Scene"));

    // 4. References (Empty)
    new Fbx::Record("References", &file);

    // 5. Definitions
    Fbx::Record* defs = new Fbx::Record("Definitions", &file);
    (*defs->insert(new Fbx::Record("Version")))->properties().insert(new Fbx::Property((int32_t)100));
    
    int32_t num_models = in.joints.size() + (write_mesh ? 1 : 0);
    int32_t num_node_attributes = (int32_t)in.joints.size();
    int32_t num_geoms = write_mesh ? 1 : 0;
    int32_t num_mats = write_mesh ? 1 : 0;
    
    // Count valid clusters (joints that actually have vertex weights)
    int32_t num_valid_clusters = 0;
    if (write_mesh) {
        for (size_t i = 0; i < in.joints.size(); ++i) {
            bool has_weight = false;
            for (uint32_t v = 0; v < (uint32_t)in.positions.size() / 3; ++v) {
                for (int k = 0; k < 4; ++k) {
                    if (in.joints_0[v*4+k] == (uint8_t)i && in.weights_0[v*4+k] > 0.001f) {
                        has_weight = true;
                        break;
                    }
                }
                if (has_weight) break;
            }
            if (has_weight) num_valid_clusters++;
        }
    }
    int32_t num_deformers = write_mesh ? (num_valid_clusters + 1) : 0;

    int32_t actual_anims = (write_anim) ? (anim_index >= 0 ? 1 : (int32_t)in.animations.size()) : 0;
    int32_t num_poses = 1;
    int32_t num_joints = (int32_t)in.joints.size();
    int32_t num_anim_stacks = actual_anims;
    int32_t num_anim_layers = actual_anims;
    int32_t num_anim_curve_nodes = actual_anims * num_joints * 3;
    int32_t num_anim_curves = actual_anims * num_joints * 9;
    
    int32_t total_objs = num_models + num_node_attributes + num_geoms + num_mats + num_deformers + num_poses + 
                         num_anim_stacks + num_anim_layers + num_anim_curve_nodes + num_anim_curves;
                         
    (*defs->insert(new Fbx::Record("Count")))->properties().insert(new Fbx::Property(total_objs));
    
    auto add_def = [&](const char* type, int32_t count) {
        if (count > 0) {
            Fbx::Record* ot = new Fbx::Record("ObjectType", defs);
            ot->properties().insert(new Fbx::Property(type));
            (*ot->insert(new Fbx::Record("Count")))->properties().insert(new Fbx::Property(count));
        }
    };
    
    add_def("Model", num_models);
    add_def("NodeAttribute", num_node_attributes);
    add_def("Geometry", num_geoms);
    add_def("Material", num_mats);
    add_def("Deformer", num_deformers);
    add_def("Pose", num_poses);
    add_def("AnimationStack", num_anim_stacks);
    add_def("AnimationLayer", num_anim_layers);
    add_def("AnimationCurveNode", num_anim_curve_nodes);
    add_def("AnimationCurve", num_anim_curves);

    // 6. Objects
    Fbx::Record* objs = new Fbx::Record("Objects", &file);
    
    std::map<int, int64_t> joint_to_id;
    std::map<int, int64_t> joint_to_attr_id;
    int64_t mesh_model_id = generate_id();
    int64_t mesh_geom_id = generate_id();
    int64_t material_id = generate_id();
    int64_t skin_id = generate_id();
    std::vector<int64_t> cluster_ids;
    std::vector<int64_t> valid_cluster_ids;
    std::vector<int64_t> valid_joint_ids;

    auto add_prop70_3d = [&](Fbx::Record* parent, const char* name, const char* type, const char* label, const char* flag, double x, double y, double z) {
        Fbx::Record* p = new Fbx::Record("P", parent);
        p->properties().insert(new Fbx::Property(name));
        p->properties().insert(new Fbx::Property(type));
        p->properties().insert(new Fbx::Property(label));
        p->properties().insert(new Fbx::Property(flag));
        p->properties().insert(new Fbx::Property(x));
        p->properties().insert(new Fbx::Property(y));
        p->properties().insert(new Fbx::Property(z));
    };

    // A. Joints (LimbNodes)
    for (size_t i = 0; i < in.joints.size(); ++i) {
        int64_t j_id = generate_id();
        joint_to_id[(int)i] = j_id;
        
        Fbx::Record* mod = new Fbx::Record("Model", objs);
        mod->properties().insert(new Fbx::Property(j_id));
        mod->properties().insert(new Fbx::Property(in.joints[i].name + std::string("\x00\x01", 2) + "Model"));
        mod->properties().insert(new Fbx::Property("LimbNode"));
        
        (*mod->insert(new Fbx::Record("Version")))->properties().insert(new Fbx::Property((int32_t)232));
        Fbx::Record* p70 = new Fbx::Record("Properties70", mod);
        
        add_prop70_3d(p70, "Lcl Translation", "Lcl Translation", "", "A", (double)in.joints[i].translate[0], (double)in.joints[i].translate[1], (double)in.joints[i].translate[2]);
        
        float euler[3]; quat_to_euler(in.joints[i].rotate, euler);
        add_prop70_3d(p70, "Lcl Rotation", "Lcl Rotation", "", "A", (double)euler[0], (double)euler[1], (double)euler[2]);
        
        add_prop70_3d(p70, "Lcl Scaling", "Lcl Scaling", "", "A", (double)in.joints[i].scale[0], (double)in.joints[i].scale[1], (double)in.joints[i].scale[2]);
        add_prop70(p70, "RotationOrder", "enum", "", "", (int32_t)0);
        
        (*mod->insert(new Fbx::Record("Shading")))->properties().insert(new Fbx::Property(true));
        (*mod->insert(new Fbx::Record("Culling")))->properties().insert(new Fbx::Property("CullingOff"));

        // Create NodeAttribute for this joint
        int64_t attr_id = generate_id();
        joint_to_attr_id[(int)i] = attr_id;
        Fbx::Record* attr = new Fbx::Record("NodeAttribute", objs);
        attr->properties().insert(new Fbx::Property(attr_id));
        attr->properties().insert(new Fbx::Property("NodeAttribute::" + in.joints[i].name + std::string("\x00\x01", 2) + "NodeAttribute"));
        attr->properties().insert(new Fbx::Property("LimbNode"));
        (*attr->insert(new Fbx::Record("TypeFlags")))->properties().insert(new Fbx::Property("Skeleton"));
    }

    if (write_mesh) {
        // B. Geometry
        Fbx::Record* geom = new Fbx::Record("Geometry", objs);
        geom->properties().insert(new Fbx::Property(mesh_geom_id));
        geom->properties().insert(new Fbx::Property(std::string("\x00\x01", 2) + "Geometry"));
        geom->properties().insert(new Fbx::Property("Mesh"));
        
        std::vector<double> verts;
        for (float v : in.positions) verts.push_back((double)v);
        (*geom->insert(new Fbx::Record("Vertices")))->properties().insert(new Fbx::Property(verts.data(), (uint32_t)verts.size()));
        
        std::vector<int32_t> poly_indices;
        for (size_t i = 0; i < in.indices.size(); i += 3) {
            poly_indices.push_back((int32_t)in.indices[i]);
            poly_indices.push_back((int32_t)in.indices[i+1]);
            poly_indices.push_back((int32_t)in.indices[i+2] ^ -1);
        }
        (*geom->insert(new Fbx::Record("PolygonVertexIndex")))->properties().insert(new Fbx::Property(poly_indices.data(), (uint32_t)poly_indices.size()));

        // Normals
        Fbx::Record* layer_norm = new Fbx::Record("LayerElementNormal", geom);
        layer_norm->properties().insert(new Fbx::Property((int32_t)0));
        (*layer_norm->insert(new Fbx::Record("Version")))->properties().insert(new Fbx::Property((int32_t)101));
        (*layer_norm->insert(new Fbx::Record("Name")))->properties().insert(new Fbx::Property(""));
        (*layer_norm->insert(new Fbx::Record("MappingInformationType")))->properties().insert(new Fbx::Property("ByPolygonVertex"));
        (*layer_norm->insert(new Fbx::Record("ReferenceInformationType")))->properties().insert(new Fbx::Property("Direct"));
        std::vector<double> norms;
        for (uint32_t idx : in.indices) {
            norms.push_back((double)in.normals[idx*3]);
            norms.push_back((double)in.normals[idx*3+1]);
            norms.push_back((double)in.normals[idx*3+2]);
        }
        (*layer_norm->insert(new Fbx::Record("Normals")))->properties().insert(new Fbx::Property(norms.data(), (uint32_t)norms.size()));

        // UVs
        Fbx::Record* layer_uv = new Fbx::Record("LayerElementUV", geom);
        layer_uv->properties().insert(new Fbx::Property((int32_t)0));
        (*layer_uv->insert(new Fbx::Record("Version")))->properties().insert(new Fbx::Property((int32_t)101));
        (*layer_uv->insert(new Fbx::Record("Name")))->properties().insert(new Fbx::Property(""));
        (*layer_uv->insert(new Fbx::Record("MappingInformationType")))->properties().insert(new Fbx::Property("ByPolygonVertex"));
        (*layer_uv->insert(new Fbx::Record("ReferenceInformationType")))->properties().insert(new Fbx::Property("Direct"));
        std::vector<double> uv_data;
        for (uint32_t idx : in.indices) {
            uv_data.push_back((double)in.texcoords[idx*2]);
            uv_data.push_back((double)(1.0f - in.texcoords[idx*2+1]));
        }
        (*layer_uv->insert(new Fbx::Record("UV")))->properties().insert(new Fbx::Property(uv_data.data(), (uint32_t)uv_data.size()));

        // Materials
        Fbx::Record* layer_mat = new Fbx::Record("LayerElementMaterial", geom);
        layer_mat->properties().insert(new Fbx::Property((int32_t)0));
        (*layer_mat->insert(new Fbx::Record("Version")))->properties().insert(new Fbx::Property((int32_t)101));
        (*layer_mat->insert(new Fbx::Record("Name")))->properties().insert(new Fbx::Property(""));
        (*layer_mat->insert(new Fbx::Record("MappingInformationType")))->properties().insert(new Fbx::Property("AllSame"));
        (*layer_mat->insert(new Fbx::Record("ReferenceInformationType")))->properties().insert(new Fbx::Property("IndexToDirect"));
        std::vector<int32_t> mat_indices = {0};
        (*layer_mat->insert(new Fbx::Record("Materials")))->properties().insert(new Fbx::Property(mat_indices.data(), 1));

        // Layer 0
        Fbx::Record* layer = new Fbx::Record("Layer", geom);
        layer->properties().insert(new Fbx::Property((int32_t)0));
        (*layer->insert(new Fbx::Record("Version")))->properties().insert(new Fbx::Property((int32_t)100));
        
        Fbx::Record* le_norm = new Fbx::Record("LayerElement", layer);
        (*le_norm->insert(new Fbx::Record("Type")))->properties().insert(new Fbx::Property("LayerElementNormal"));
        (*le_norm->insert(new Fbx::Record("TypedIndex")))->properties().insert(new Fbx::Property((int32_t)0));
        
        Fbx::Record* le_uv = new Fbx::Record("LayerElement", layer);
        (*le_uv->insert(new Fbx::Record("Type")))->properties().insert(new Fbx::Property("LayerElementUV"));
        (*le_uv->insert(new Fbx::Record("TypedIndex")))->properties().insert(new Fbx::Property((int32_t)0));

        Fbx::Record* le_mat = new Fbx::Record("LayerElement", layer);
        (*le_mat->insert(new Fbx::Record("Type")))->properties().insert(new Fbx::Property("LayerElementMaterial"));
        (*le_mat->insert(new Fbx::Record("TypedIndex")))->properties().insert(new Fbx::Property((int32_t)0));

        // Mesh Model
        Fbx::Record* mod_mesh = new Fbx::Record("Model", objs);
        mod_mesh->properties().insert(new Fbx::Property(mesh_model_id));
        mod_mesh->properties().insert(new Fbx::Property("Mesh" + std::string("\x00\x01", 2) + "Model"));
        mod_mesh->properties().insert(new Fbx::Property("Mesh"));
        (*mod_mesh->insert(new Fbx::Record("Version")))->properties().insert(new Fbx::Property((int32_t)232));
        Fbx::Record* p70_mesh = new Fbx::Record("Properties70", mod_mesh);
        add_prop70(p70_mesh, "RotationOrder", "enum", "", "", (int32_t)0);

        // Material
        Fbx::Record* mat = new Fbx::Record("Material", objs);
        mat->properties().insert(new Fbx::Property(material_id));
        mat->properties().insert(new Fbx::Property("DefaultMaterial" + std::string("\x00\x01", 2) + "Material"));
        mat->properties().insert(new Fbx::Property(""));
        (*mat->insert(new Fbx::Record("Version")))->properties().insert(new Fbx::Property((int32_t)102));

        // Skin & Clusters
        Fbx::Record* skin_rec = new Fbx::Record("Deformer", objs);
        skin_rec->properties().insert(new Fbx::Property(skin_id));
        skin_rec->properties().insert(new Fbx::Property("Skin" + std::string("\x00\x01", 2) + "Deformer"));
        skin_rec->properties().insert(new Fbx::Property("Skin"));
        (*skin_rec->insert(new Fbx::Record("Version")))->properties().insert(new Fbx::Property((int32_t)101));
        (*skin_rec->insert(new Fbx::Record("Link_DeformAcuracy")))->properties().insert(new Fbx::Property((double)50.0));
        
        for (size_t i = 0; i < in.joints.size(); ++i) {
            std::vector<int32_t> idxs;
            std::vector<double> wts;
            for (uint32_t v = 0; v < (uint32_t)in.positions.size() / 3; ++v) {
                for (int k = 0; k < 4; ++k) {
                    if (in.joints_0[v*4+k] == (uint8_t)i && in.weights_0[v*4+k] > 0.001f) {
                        idxs.push_back((int32_t)v);
                        wts.push_back((double)in.weights_0[v*4+k]);
                        break;
                    }
                }
            }

            // Skip zero-weight joints entirely
            if (idxs.empty()) {
                cluster_ids.push_back(0); // Maintain index alignment but mark as invalid
                continue;
            }

            int64_t c_id = generate_id();
            cluster_ids.push_back(c_id);
            valid_cluster_ids.push_back(c_id);
            valid_joint_ids.push_back(joint_to_id[i]);

            Fbx::Record* cluster = new Fbx::Record("Deformer", objs);
            cluster->properties().insert(new Fbx::Property(c_id));
            cluster->properties().insert(new Fbx::Property("Cluster_" + in.joints[i].name + std::string("\x00\x01", 2) + "SubDeformer"));
            cluster->properties().insert(new Fbx::Property("Cluster"));
            (*cluster->insert(new Fbx::Record("Version")))->properties().insert(new Fbx::Property((int32_t)100));
            
            Fbx::Record* userdata = new Fbx::Record("UserData", cluster);
            userdata->properties().insert(new Fbx::Property(""));
            userdata->properties().insert(new Fbx::Property(""));

            (*cluster->insert(new Fbx::Record("Indexes")))->properties().insert(new Fbx::Property(idxs.data(), (uint32_t)idxs.size()));
            (*cluster->insert(new Fbx::Record("Weights")))->properties().insert(new Fbx::Property(wts.data(), (uint32_t)wts.size()));
            // In standard FBX, 'Transform' is the Mesh matrix at bind time (Identity for us).
            // 'TransformLink' is the Bone matrix at bind time.
            Fbx::Record* transf = new Fbx::Record("Transform", cluster);
            double id_m[16];
            mat4 ident = mat4_identity();
            for(int r=0; r<16; ++r) id_m[r] = (double)ident.m[r];
            transf->properties().insert(new Fbx::Property(id_m, 16));

            Fbx::Record* tlink = new Fbx::Record("TransformLink", cluster);
            double m[16];
            for(int r=0; r<16; ++r) m[r] = (double)in.world_matrices[i].m[r];
            tlink->properties().insert(new Fbx::Property(m, 16));
            
            Fbx::Record* tassoc = new Fbx::Record("TransformAssociateModel", cluster);
            double assoc_m[16];
            for(int r=0; r<16; ++r) assoc_m[r] = (double)ident.m[r];
            tassoc->properties().insert(new Fbx::Property(assoc_m, 16));

    }
}

// 6.5. BindPose
int64_t bindpose_id = generate_id();
Fbx::Record* pose = new Fbx::Record("Pose", objs);
pose->properties().insert(new Fbx::Property(bindpose_id));
pose->properties().insert(new Fbx::Property("BindPose" + std::string("\x00\x01", 2) + "Pose"));
pose->properties().insert(new Fbx::Property("BindPose"));

(*pose->insert(new Fbx::Record("Type")))->properties().insert(new Fbx::Property("BindPose"));
(*pose->insert(new Fbx::Record("Version")))->properties().insert(new Fbx::Property((int32_t)100));

int32_t num_pose_nodes = (write_mesh ? 1 : 0) + (int32_t)in.joints.size();
(*pose->insert(new Fbx::Record("NbPoseNodes")))->properties().insert(new Fbx::Property(num_pose_nodes));

auto add_pose_node = [&](int64_t node_id, const mat4& m) {
    Fbx::Record* pn = new Fbx::Record("PoseNode", pose);
    (*pn->insert(new Fbx::Record("Node")))->properties().insert(new Fbx::Property(node_id));
    Fbx::Record* pm = new Fbx::Record("Matrix", pn);
    double dm[16];
    for(int r=0; r<16; ++r) dm[r] = (double)m.m[r];
    pm->properties().insert(new Fbx::Property(dm, 16));
};

if (write_mesh) add_pose_node(mesh_model_id, mat4_identity());
for (size_t i = 0; i < in.joints.size(); ++i) {
    add_pose_node(joint_to_id[i], in.world_matrices[i]);
}

// G. Animation
    struct AnimLink { int64_t stack_id; int64_t layer_id; 
                     struct Channel { int64_t t, r, s; };
                     std::vector<Channel> nodes;
                     struct CurvePair { int64_t tx, ty, tz, rx, ry, rz, sx, sy, sz; };
                     std::vector<CurvePair> curves; };
    std::vector<AnimLink> anim_links;

    if (write_anim) {
        for (size_t ai = 0; ai < in.animations.size(); ++ai) {
            if (anim_index >= 0 && (int)ai != anim_index) continue;
            
            AnimLink link;
            link.stack_id = generate_id();
            link.layer_id = generate_id();
            
            Fbx::Record* stack = new Fbx::Record("AnimationStack", objs);
            stack->properties().insert(new Fbx::Property(link.stack_id));
            stack->properties().insert(new Fbx::Property(in.animations[ai].name + std::string("\x00\x01", 2) + "AnimStack"));
            stack->properties().insert(new Fbx::Property(""));
            
            Fbx::Record* s_props70 = new Fbx::Record("Properties70", stack);
            int64_t start_time = 0;
            int64_t end_time = (int64_t)(in.animations[ai].last_frame - in.animations[ai].first_frame) * KTIME_ONE_SECOND / (int64_t)(in.animations[ai].fps > 0 ? in.animations[ai].fps : BASE_FPS);
            add_prop70(s_props70, "LocalStart", "KTime", "Time", "", start_time);
            add_prop70(s_props70, "LocalStop", "KTime", "Time", "", end_time);
            add_prop70(s_props70, "ReferenceStart", "KTime", "Time", "", start_time);
            add_prop70(s_props70, "ReferenceStop", "KTime", "Time", "", end_time);
            
            Fbx::Record* layer = new Fbx::Record("AnimationLayer", objs);
            layer->properties().insert(new Fbx::Property(link.layer_id));
            layer->properties().insert(new Fbx::Property("BaseLayer" + std::string("\x00\x01", 2) + "AnimLayer"));
            layer->properties().insert(new Fbx::Property(""));

            link.nodes.resize(in.joints.size());
            link.curves.resize(in.joints.size());

            for (size_t ji = 0; ji < in.joints.size(); ++ji) {
                auto add_curve_node = [&](const char* name, const char* type) {
                    int64_t node_id = generate_id();
                    Fbx::Record* cn = new Fbx::Record("AnimationCurveNode", objs);
                    cn->properties().insert(new Fbx::Property(node_id));
                    cn->properties().insert(new Fbx::Property(std::string(name) + std::string("\x00\x01", 2) + "AnimCurveNode"));
                    cn->properties().insert(new Fbx::Property(""));
                    Fbx::Record* cp = new Fbx::Record("Properties70", cn);
                    add_prop70(cp, "d", type, "", "A", (double)0);
                    return node_id;
                };

                link.nodes[ji].t = add_curve_node("Lcl Translation", "Lcl Translation");
                link.nodes[ji].r = add_curve_node("Lcl Rotation", "Lcl Rotation");
                link.nodes[ji].s = add_curve_node("Lcl Scaling", "Lcl Scaling");

                auto add_curve = [&](const std::vector<double>& keys, float anim_fps) {
                    int64_t c_id = generate_id();
                    Fbx::Record* c = new Fbx::Record("AnimationCurve", objs);
                    c->properties().insert(new Fbx::Property(c_id));
                    c->properties().insert(new Fbx::Property(std::string("") + std::string("\x00\x01", 2) + "AnimCurve"));
                    c->properties().insert(new Fbx::Property(""));
                    
                    std::vector<int64_t> times;
                    uint32_t nf = (uint32_t)keys.size();
                    for (uint32_t k = 0; k < nf; ++k) times.push_back(k * KTIME_ONE_SECOND / (int64_t)anim_fps);
                    
                    (*c->insert(new Fbx::Record("KeyTime")))->properties().insert(new Fbx::Property(times.data(), (uint32_t)times.size()));
                    std::vector<float> kvals;
                    for(double k : keys) kvals.push_back((float)k);
                    (*c->insert(new Fbx::Record("KeyValueFloat")))->properties().insert(new Fbx::Property(kvals.data(), (uint32_t)kvals.size()));

                    if (nf > 0) {
                        std::vector<int32_t> attr_flags = { 4 }; // 4 = Linear
                        (*c->insert(new Fbx::Record("KeyAttrFlags")))->properties().insert(new Fbx::Property(attr_flags.data(), 1));
                        std::vector<float> attr_data = { 0.0f, 0.0f, 0.0f, 0.0f };
                        (*c->insert(new Fbx::Record("KeyAttrDataFloat")))->properties().insert(new Fbx::Property(attr_data.data(), 4));
                        std::vector<int32_t> attr_refs = { (int32_t)nf };
                        (*c->insert(new Fbx::Record("KeyAttrRefCount")))->properties().insert(new Fbx::Property(attr_refs.data(), 1));
                    }
                    return c_id;
                };

                std::vector<double> tx, ty, tz, rx, ry, rz, sx, sy, sz;
                uint32_t nf = in.animations[ai].last_frame - in.animations[ai].first_frame + 1;
                
                float prev_e[3] = {0,0,0};
                for (uint32_t k = 0; k < nf; ++k) {
                    uint32_t f_idx = in.animations[ai].first_frame + k;
                    const float* f = &in.frames[f_idx * in.num_framechannels + ji * 10];
                    tx.push_back(f[0]); ty.push_back(f[1]); tz.push_back(f[2]);
                    
                    // Normalize quaternion to ensure stable Euler conversion
                    float q[4] = { f[3], f[4], f[5], f[6] };
                    quat_normalize(q);
                    float e[3]; quat_to_euler(q, e);
                    
                    if (k == 0) {
                        for (int c = 0; c < 3; ++c) prev_e[c] = e[c];
                    } else {
                        // Un-wrap euler angles (prevent > 180 deg jumps)
                        for (int c = 0; c < 3; ++c) {
                            float diff = e[c] - prev_e[c];
                            if (diff < -180.0f) e[c] += std::ceil(-diff / 360.0f) * 360.0f;
                            else if (diff > 180.0f) e[c] -= std::ceil(diff / 360.0f) * 360.0f;
                            prev_e[c] = e[c];
                        }
                    }
                    
                    rx.push_back(e[0]); ry.push_back(e[1]); rz.push_back(e[2]);
                    sx.push_back(f[7]); sy.push_back(f[8]); sz.push_back(f[9]);
                }

                float anim_fps = in.animations[ai].fps > 0 ? in.animations[ai].fps : BASE_FPS;
                link.curves[ji].tx = add_curve(tx, anim_fps); link.curves[ji].ty = add_curve(ty, anim_fps); link.curves[ji].tz = add_curve(tz, anim_fps);
                link.curves[ji].rx = add_curve(rx, anim_fps); link.curves[ji].ry = add_curve(ry, anim_fps); link.curves[ji].rz = add_curve(rz, anim_fps);
                link.curves[ji].sx = add_curve(sx, anim_fps); link.curves[ji].sy = add_curve(sy, anim_fps); link.curves[ji].sz = add_curve(sz, anim_fps);
            }
            anim_links.push_back(link);
        }
    }

    // 7. Connections
    Fbx::Record* conns = new Fbx::Record("Connections", &file);
    auto add_c = [&](const char* type, int64_t child, int64_t parent, const char* prop = nullptr) {
        Fbx::Record* r = new Fbx::Record("C", conns);
        r->properties().insert(new Fbx::Property(type));
        r->properties().insert(new Fbx::Property(child));
        r->properties().insert(new Fbx::Property(parent));
        if (prop) r->properties().insert(new Fbx::Property(prop));
    };

    for (size_t i = 0; i < in.joints.size(); ++i) {
        if (in.joints[i].parent >= 0) add_c("OO", joint_to_id[i], joint_to_id[in.joints[i].parent]);
        else add_c("OO", joint_to_id[i], 0);

        // Connect NodeAttribute to Joint Model
        add_c("OO", joint_to_attr_id[i], joint_to_id[i]);
    }

    if (write_mesh) {
        add_c("OO", mesh_model_id, 0);
        add_c("OO", mesh_geom_id, mesh_model_id);
        add_c("OO", material_id, mesh_model_id);
        add_c("OO", skin_id, mesh_geom_id);
        for (size_t i = 0; i < valid_cluster_ids.size(); ++i) {
            add_c("OO", valid_cluster_ids[i], skin_id);
            add_c("OO", valid_joint_ids[i], valid_cluster_ids[i]);
        }
    }

    for (const auto& link : anim_links) {
        add_c("OO", link.layer_id, link.stack_id);
        for (size_t ji = 0; ji < in.joints.size(); ++ji) {
            add_c("OO", link.nodes[ji].t, link.layer_id);
            add_c("OP", link.curves[ji].tx, link.nodes[ji].t, "d|X");
            add_c("OP", link.curves[ji].ty, link.nodes[ji].t, "d|Y");
            add_c("OP", link.curves[ji].tz, link.nodes[ji].t, "d|Z");
            
            add_c("OO", link.nodes[ji].r, link.layer_id);
            add_c("OP", link.curves[ji].rx, link.nodes[ji].r, "d|X");
            add_c("OP", link.curves[ji].ry, link.nodes[ji].r, "d|Y");
            add_c("OP", link.curves[ji].rz, link.nodes[ji].r, "d|Z");
            
            add_c("OO", link.nodes[ji].s, link.layer_id);
            add_c("OP", link.curves[ji].sx, link.nodes[ji].s, "d|X");
            add_c("OP", link.curves[ji].sy, link.nodes[ji].s, "d|Y");
            add_c("OP", link.curves[ji].sz, link.nodes[ji].s, "d|Z");
            
            // Link curve nodes to the joints
            add_c("OP", link.nodes[ji].t, joint_to_id[ji], "Lcl Translation");
            add_c("OP", link.nodes[ji].r, joint_to_id[ji], "Lcl Rotation");
            add_c("OP", link.nodes[ji].s, joint_to_id[ji], "Lcl Scaling");
        }
    }

    file.write(path, 7400);
    return true;
}
