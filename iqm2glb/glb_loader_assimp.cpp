#include "math_utils.h"
#if ENABLE_ASSIMP
#include "glb_loader_assimp.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <iostream>
#include <vector>
#include <map>
#include <cstring>

// Helper to convert aiMatrix4x4 to our mat4 (column-major)
static void to_mat4(const aiMatrix4x4& m, float* out) {
    out[0] = m.a1; out[1] = m.b1; out[2] = m.c1; out[3] = m.d1;
    out[4] = m.a2; out[5] = m.b2; out[6] = m.c2; out[7] = m.d2;
    out[8] = m.a3; out[9] = m.b3; out[10] = m.c3; out[11] = m.d3;
    out[12] = m.a4; out[13] = m.b4; out[14] = m.c4; out[15] = m.d4;
}

static void decompose_aiMatrix(const aiMatrix4x4& m, float* t, float* r, float* s) {
    aiVector3D pos, scale;
    aiQuaternion rot;
    m.Decompose(scale, rot, pos);
    t[0] = pos.x; t[1] = pos.y; t[2] = pos.z;
    r[0] = rot.x; r[1] = rot.y; r[2] = rot.z; r[3] = rot.w;
    s[0] = scale.x; s[1] = scale.y; s[2] = scale.z;
}

bool load_glb_assimp(const char* path, Model& out) {
    out = Model(); // Clear model
    Assimp::Importer importer;
    // We want the original data structure as much as possible, but triangulated.
    const aiScene* scene = importer.ReadFile(path, 
        aiProcess_Triangulate | 
        aiProcess_FlipUVs | 
        aiProcess_LimitBoneWeights |
        aiProcess_JoinIdenticalVertices);

    if (!scene || !scene->mRootNode) {
        std::cerr << "Assimp load failed: " << importer.GetErrorString() << std::endl;
        return false;
    }

    // 1. Flatten Nodes / Joint Hierarchy
    std::vector<aiNode*> nodes;
    std::map<aiNode*, int> node_to_idx;
    
    auto collect_nodes = [&](auto self, aiNode* node) -> void {
        node_to_idx[node] = (int)nodes.size();
        nodes.push_back(node);
        for (unsigned int i = 0; i < node->mNumChildren; ++i) self(self, node->mChildren[i]);
    };
    collect_nodes(collect_nodes, scene->mRootNode);

    out.joints.resize(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        aiNode* node = nodes[i];
        out.joints[i].name = node->mName.C_Str();
        out.joints[i].parent = node->mParent ? node_to_idx[node->mParent] : -1;
        
        decompose_aiMatrix(node->mTransformation, out.joints[i].translate, out.joints[i].rotate, out.joints[i].scale);
    }

    // 2. Process Meshes
    // For simplicity with IQM, we combine all meshes into one internal structure
    uint32_t total_verts = 0;
    uint32_t total_tris = 0;
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        total_verts += scene->mMeshes[i]->mNumVertices;
        total_tris += scene->mMeshes[i]->mNumFaces;
    }

    out.positions.reserve(total_verts * 3);
    out.normals.reserve(total_verts * 3);
    out.texcoords.reserve(total_verts * 2);
    out.joints_0.assign(total_verts * 4, 0);
    out.weights_0.assign(total_verts * 4, 0.0f);
    out.indices.reserve(total_tris * 3);

    uint32_t v_offset = 0;
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        aiMesh* m = scene->mMeshes[i];
        Mesh md;
        md.name = m->mName.C_Str();
        md.material_name = scene->mMaterials[m->mMaterialIndex]->GetName().C_Str();
        md.first_vertex = v_offset;
        md.num_vertexes = m->mNumVertices;
        md.first_triangle = (uint32_t)out.indices.size() / 3;
        md.num_triangles = m->mNumFaces;
        out.meshes.push_back(md);

        for (unsigned int v = 0; v < m->mNumVertices; ++v) {
            out.positions.push_back(m->mVertices[v].x);
            out.positions.push_back(m->mVertices[v].y);
            out.positions.push_back(m->mVertices[v].z);
            if (m->mNormals) {
                out.normals.push_back(m->mNormals[v].x);
                out.normals.push_back(m->mNormals[v].y);
                out.normals.push_back(m->mNormals[v].z);
            } else {
                out.normals.push_back(0); out.normals.push_back(1); out.normals.push_back(0);
            }
            if (m->mTextureCoords[0]) {
                out.texcoords.push_back(m->mTextureCoords[0][v].x);
                out.texcoords.push_back(m->mTextureCoords[0][v].y);
            } else {
                out.texcoords.push_back(0); out.texcoords.push_back(0);
            }
        }

        // Process Bones for this mesh
        std::map<std::string, int> name_to_joint;
        for(size_t j=0; j<out.joints.size(); ++j) name_to_joint[out.joints[j].name] = (int)j;

        for (unsigned int b = 0; b < m->mNumBones; ++b) {
            aiBone* bone = m->mBones[b];
            int j_idx = -1;
            // Assimp might sometimes name bones differently than nodes? 
            // aiProcess_PopulateArmatureData should have fixed this.
            if (bone->mNode && node_to_idx.count(bone->mNode)) {
                j_idx = node_to_idx[bone->mNode];
            } else {
                auto it = name_to_joint.find(bone->mName.C_Str());
                if (it != name_to_joint.end()) j_idx = it->second;
            }

            if (j_idx != -1) {
                // Store IBM
                if (out.ibms.size() < out.joints.size()) out.ibms.resize(out.joints.size());
                to_mat4(bone->mOffsetMatrix, out.ibms[j_idx].m);

                for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
                    uint32_t vid = v_offset + bone->mWeights[w].mVertexId;
                    float weight = bone->mWeights[w].mWeight;
                    // Find empty slot
                    for (int k = 0; k < 4; ++k) {
                        if (out.weights_0[vid * 4 + k] == 0.0f) {
                            out.joints_0[vid * 4 + k] = (uint8_t)j_idx;
                            out.weights_0[vid * 4 + k] = weight;
                            break;
                        }
                    }
                }
            }
        }

        for (unsigned int f = 0; f < m->mNumFaces; ++f) {
            out.indices.push_back(v_offset + m->mFaces[f].mIndices[0]);
            out.indices.push_back(v_offset + m->mFaces[f].mIndices[1]);
            out.indices.push_back(v_offset + m->mFaces[f].mIndices[2]);
        }
        v_offset += m->mNumVertices;
    }

    // 3. Animations
    out.num_framechannels = (uint32_t)out.joints.size() * 10;
    float fps = BASE_FPS;
    for (unsigned int i = 0; i < scene->mNumAnimations; ++i) {
        aiAnimation* anim = scene->mAnimations[i];
        AnimationDef ad;
        ad.name = anim->mName.C_Str();
        float dur_ticks = (float)anim->mDuration;
        float tps = (anim->mTicksPerSecond ? (float)anim->mTicksPerSecond : 1.0f);
        uint32_t nf = (uint32_t)std::round(dur_ticks * fps / tps) + 1;
        
        std::cout << "Anim: " << ad.name << " Dur: " << dur_ticks << " TPS: " << tps << " NF: " << nf << std::endl;

        ad.first_frame = (uint32_t)out.frames.size() / out.num_framechannels;
        ad.last_frame = ad.first_frame + nf - 1;
        ad.fps = fps;
        ad.loop_frames = 0;
        out.animations.push_back(ad);

        size_t frame_start = out.frames.size();
        out.frames.resize(frame_start + (size_t)nf * out.num_framechannels);

        for (uint32_t f = 0; f < nf; ++f) {
            float time = (float)f / fps;
            float ticks = time * (anim->mTicksPerSecond ? (float)anim->mTicksPerSecond : 1.0f);
            float* fptr = &out.frames[frame_start + (size_t)f * out.num_framechannels];
            
            for (size_t ji = 0; ji < out.joints.size(); ++ji) {
                aiNode* node = nodes[ji];
                // Try to find channel for this node
                aiNodeAnim* channel = nullptr;
                for (unsigned int c = 0; c < anim->mNumChannels; ++c) {
                    if (anim->mChannels[c]->mNodeName == node->mName) {
                        channel = anim->mChannels[c];
                        break;
                    }
                }

                float tr[3], ro[4], sc[3];
                decompose_aiMatrix(node->mTransformation, tr, ro, sc);

                if (channel) {
                    // Sample Position
                    if (channel->mNumPositionKeys > 0) {
                        unsigned int k = 0;
                        while (k < channel->mNumPositionKeys - 1 && ticks > (float)channel->mPositionKeys[k+1].mTime) k++;
                        aiVector3D v = channel->mPositionKeys[k].mValue;
                        tr[0] = v.x; tr[1] = v.y; tr[2] = v.z;
                    }
                    // Sample Rotation
                    if (channel->mNumRotationKeys > 0) {
                        unsigned int k = 0;
                        while (k < channel->mNumRotationKeys - 1 && ticks > (float)channel->mRotationKeys[k+1].mTime) k++;
                        aiQuaternion q = channel->mRotationKeys[k].mValue;
                        ro[0] = q.x; ro[1] = q.y; ro[2] = q.z; ro[3] = q.w;
                    }
                    // Sample Scale
                    if (channel->mNumScalingKeys > 0) {
                        unsigned int k = 0;
                        while (k < channel->mNumScalingKeys - 1 && ticks > (float)channel->mScalingKeys[k+1].mTime) k++;
                        aiVector3D s = channel->mScalingKeys[k].mValue;
                        sc[0] = s.x; sc[1] = s.y; sc[2] = s.z;
                    }
                }

                memcpy(fptr, tr, 12);
                memcpy(fptr+3, ro, 16);
                memcpy(fptr+7, sc, 12);
                fptr += 10;
            }
        }
    }
    out.num_frames = (uint32_t)out.frames.size() / out.num_framechannels;

    return true;
}
#endif
