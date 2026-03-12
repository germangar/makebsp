#include "fbx_writer_assimp.h"
#include <assimp/scene.h>
#include <assimp/exporter.hpp>
#include <assimp/postprocess.h>
#include <iostream>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdio>

static aiMatrix4x4 to_aiMatrix4x4(const mat4& m) {
    aiMatrix4x4 res;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            res[row][col] = m.m[col * 4 + row];
        }
    }
    return res;
}

bool write_fbx_assimp(const char* path, const Model& model) 
{
    aiScene* scene = new aiScene();
    
    // 0. Materials (Assimp needs at least one if meshes refer to them)
    scene->mNumMaterials = 1;
    scene->mMaterials = new aiMaterial*[1];
    scene->mMaterials[0] = new aiMaterial();
    
    // 1. Root & Armature Nodes
    aiNode* armatureNode = new aiNode("Armature");
    
    // 2. Joints
    std::vector<aiNode*> joints(model.joints.size());
    for (size_t i = 0; i < model.joints.size(); ++i) {
        std::string name = model.joints[i].name;
        std::replace(name.begin(), name.end(), ' ', '_');
        joints[i] = new aiNode(name);
        mat4 local = mat4_from_trs(model.joints[i].translate, model.joints[i].rotate, model.joints[i].scale);
        joints[i]->mTransformation = to_aiMatrix4x4(local);
    }
    
    // Build bone hierarchy
    for (size_t i = 0; i < model.joints.size(); ++i) {
        int parent = model.joints[i].parent;
        if (parent == -1) {
            armatureNode->mNumChildren++;
        } else {
            joints[parent]->mNumChildren++;
        }
    }
    
    armatureNode->mChildren = new aiNode*[armatureNode->mNumChildren];
    int root_count = 0;
    for (size_t i = 0; i < model.joints.size(); ++i) {
        int parent = model.joints[i].parent;
        if (parent == -1) {
            armatureNode->mChildren[root_count++] = joints[i];
            joints[i]->mParent = armatureNode;
        } else {
            if (!joints[parent]->mChildren) {
                joints[parent]->mChildren = new aiNode*[joints[parent]->mNumChildren];
                for (unsigned int c = 0; c < joints[parent]->mNumChildren; c++) joints[parent]->mChildren[c] = nullptr;
            }
            unsigned int cidx = 0;
            while (joints[parent]->mChildren[cidx] != nullptr) cidx++;
            joints[parent]->mChildren[cidx] = joints[i];
            joints[i]->mParent = joints[parent];
        }
    }

    // 3. Meshes
    std::vector<aiNode*> mesh_nodes;
    scene->mNumMeshes = (unsigned int)model.meshes.size();
    scene->mMeshes = new aiMesh*[scene->mNumMeshes];
    
    for (int i = 0; i < (int)model.meshes.size(); ++i) {
        const Mesh& m = model.meshes[i];
        aiMesh* am = new aiMesh();
        scene->mMeshes[i] = am;
        am->mName = aiString(m.name);
        am->mMaterialIndex = 0;
        am->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
        
        am->mNumVertices = m.num_vertexes;
        am->mVertices = new aiVector3D[am->mNumVertices];
        am->mNormals = new aiVector3D[am->mNumVertices];
        am->mTextureCoords[0] = new aiVector3D[am->mNumVertices];
        am->mNumUVComponents[0] = 2;
        
        for (uint32_t v = 0; v < m.num_vertexes; v++) {
            uint32_t vidx = m.first_vertex + v;
            am->mVertices[v] = aiVector3D(model.positions[vidx * 3 + 0], model.positions[vidx * 3 + 1], model.positions[vidx * 3 + 2]);
            am->mNormals[v] = aiVector3D(model.normals[vidx * 3 + 0], model.normals[vidx * 3 + 1], model.normals[vidx * 3 + 2]);
            am->mTextureCoords[0][v] = aiVector3D(model.texcoords[vidx * 2 + 0], model.texcoords[vidx * 2 + 1], 0.0f);
        }
        
        am->mNumFaces = m.num_triangles;
        am->mFaces = new aiFace[am->mNumFaces];
        for (unsigned int f = 0; f < m.num_triangles; ++f) {
            uint32_t tidx = (m.first_triangle + f) * 3;
            aiFace& face = am->mFaces[f];
            face.mNumIndices = 3;
            face.mIndices = new unsigned int[3];
            face.mIndices[0] = model.indices[tidx + 0] - m.first_vertex;
            face.mIndices[1] = model.indices[tidx + 1] - m.first_vertex;
            face.mIndices[2] = model.indices[tidx + 2] - m.first_vertex;
        }

        if (!model.joints.empty()) {
            std::vector<aiBone*> meshBones;
            const std::vector<mat4>& ibms = model.ibms.empty() ? model.computed_ibms : model.ibms;
            
            for (unsigned int j = 0; j < (unsigned int)model.joints.size(); ++j) {
                std::vector<aiVertexWeight> vWeights;
                for (unsigned int v = 0; v < m.num_vertexes; v++) {
                    uint32_t vidx = m.first_vertex + v;
                    for (int k = 0; k < 4; k++) {
                        if (model.joints_0[vidx * 4 + k] == (uint8_t)j && model.weights_0[vidx * 4 + k] > 0.0001f) {
                            vWeights.push_back(aiVertexWeight(v, model.weights_0[vidx * 4 + k]));
                        }
                    }
                }
                
                aiBone* bone = new aiBone();
                std::string bname = model.joints[j].name;
                std::replace(bname.begin(), bname.end(), ' ', '_');
                bone->mName = aiString(bname);
                bone->mOffsetMatrix = to_aiMatrix4x4(ibms[j]);
                bone->mNumWeights = (unsigned int)vWeights.size();
                if (bone->mNumWeights > 0) {
                    bone->mWeights = new aiVertexWeight[bone->mNumWeights];
                    for (unsigned int w = 0; w < bone->mNumWeights; w++) bone->mWeights[w] = vWeights[w];
                    meshBones.push_back(bone);
                } else {
                    delete bone;
                }
            }
            if (!meshBones.empty()) {
                am->mNumBones = (unsigned int)meshBones.size();
                am->mBones = new aiBone*[am->mNumBones];
                for (unsigned int j = 0; j < am->mNumBones; j++) am->mBones[j] = meshBones[j];
            }
        }

        aiNode* meshNode = new aiNode(m.name);
        std::string mname = meshNode->mName.C_Str();
        std::replace(mname.begin(), mname.end(), ' ', '_');
        meshNode->mName = aiString(mname);
        
        meshNode->mNumMeshes = 1;
        meshNode->mMeshes = new unsigned int[1];
        meshNode->mMeshes[0] = i;
        mesh_nodes.push_back(meshNode);
    }

    scene->mRootNode = new aiNode("RootNode");
    scene->mRootNode->mNumChildren = 1;
    scene->mRootNode->mChildren = new aiNode*[1];
    scene->mRootNode->mChildren[0] = armatureNode;
    armatureNode->mParent = scene->mRootNode;

    // Nest meshes under Armature
    unsigned int old_num = armatureNode->mNumChildren;
    aiNode** old_children = armatureNode->mChildren;
    
    armatureNode->mNumChildren = old_num + (unsigned int)mesh_nodes.size();
    armatureNode->mChildren = new aiNode*[armatureNode->mNumChildren];
    
    for (unsigned int i = 0; i < old_num; ++i) {
        armatureNode->mChildren[i] = old_children[i];
    }
    for (size_t i = 0; i < mesh_nodes.size(); ++i) {
        armatureNode->mChildren[old_num + i] = mesh_nodes[i];
        mesh_nodes[i]->mParent = armatureNode;
    }
    // Note: old_children was Allocated with new aiNode*[N], but we don't delete it here 
    // to avoid potential issues if Assimp expects it. Actually aiNode doesn't have a destructor that cleans mChildren.

    // 4. Animations
    if (!model.animations.empty() && model.num_frames > 0) {
        scene->mNumAnimations = (unsigned int)model.animations.size();
        scene->mAnimations = new aiAnimation*[scene->mNumAnimations];
        
        for (unsigned int a = 0; a < scene->mNumAnimations; a++) {
            const auto& ad = model.animations[a];
            aiAnimation* anim = new aiAnimation();
            scene->mAnimations[a] = anim;
            
            anim->mName = aiString(ad.name);

            uint32_t num_keys = (uint32_t)(ad.last_frame - ad.first_frame + 1);
            anim->mDuration = (double)(num_keys > 1 ? num_keys - 1 : 1);
            anim->mTicksPerSecond = (double)ad.fps;
            
            anim->mNumChannels = (unsigned int)model.joints.size();
            anim->mChannels = new aiNodeAnim*[anim->mNumChannels];
            
            for (unsigned int j = 0; j < anim->mNumChannels; j++) {
                aiNodeAnim* chan = new aiNodeAnim();
                anim->mChannels[j] = chan;
                std::string jname = model.joints[j].name;
                std::replace(jname.begin(), jname.end(), ' ', '_');
                chan->mNodeName = aiString(jname);
                
                chan->mNumPositionKeys = num_keys;
                chan->mPositionKeys = new aiVectorKey[num_keys];
                chan->mNumRotationKeys = num_keys;
                chan->mRotationKeys = new aiQuatKey[num_keys];
                chan->mNumScalingKeys = num_keys;
                chan->mScalingKeys = new aiVectorKey[num_keys];
                
                for (uint32_t k = 0; k < num_keys; k++) {
                    uint32_t fidx = std::min((uint32_t)ad.first_frame + k, model.num_frames - 1);
                    float* frame = (float*)&model.frames[fidx * model.num_framechannels + j * 10];
                    double time = (double)k;
                    
                    chan->mPositionKeys[k] = aiVectorKey(time, aiVector3D(frame[0], frame[1], frame[2]));
                    // xyzw -> wxyz
                    aiQuaternion rot(frame[6], frame[3], frame[4], frame[5]);
                    chan->mRotationKeys[k] = aiQuatKey(time, rot);
                    chan->mScalingKeys[k] = aiVectorKey(time, aiVector3D(frame[7], frame[8], frame[9]));
                }
            }
        }
    }
    

    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add("UnitScaleFactor", (double)100.0);
    scene->mMetaData->Add("UpAxis", (int)1); // Y-up
    scene->mMetaData->Add("FrontAxis", (int)2);
    scene->mMetaData->Add("CoordAxis", (int)0);

    Assimp::Exporter exporter;
    const char* format_id = (std::string(path).find(".fbx") != std::string::npos && std::string(path).find("ascii") != std::string::npos) ? "fbxa" : "fbx";
    
    std::cout << "Exporting to " << path << " using Assimp '" << format_id << "'..." << std::endl;
    aiReturn ret = exporter.Export(scene, format_id, path);
    
    if (ret != aiReturn_SUCCESS) {
        std::cerr << "Assimp FBX Export FAILED: " << exporter.GetErrorString() << std::endl;
        delete scene;
        return false;
    }

    delete scene;
    return true;
}
