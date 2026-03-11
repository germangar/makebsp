#include "fbx_writer_assimp.h"
#include <assimp/scene.h>
#include <assimp/exporter.hpp>
#include <assimp/postprocess.h>
#include <iostream>
#include <vector>
#include <map>

// Helper to convert internal mat4 to aiMatrix4x4
static aiMatrix4x4 to_aiMatrix4x4(const mat4& m) {
    aiMatrix4x4 res;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            res[row][col] = m.m[col * 4 + row];
        }
    }
    return res;
}

bool write_fbx_assimp(const char* path, const Model& model) {
    aiScene* scene = new aiScene();
    
    // 1. Materials
    std::map<std::string, unsigned int> material_map;
    std::vector<aiMaterial*> materials;
    for (const auto& mesh : model.meshes) {
        if (material_map.find(mesh.material_name) == material_map.end()) {
            aiMaterial* mat = new aiMaterial();
            aiString name(mesh.material_name.c_str());
            mat->AddProperty(&name, AI_MATKEY_NAME);
            
            // Standard FBX-friendly material properties
            aiColor3D diffuse(0.8f, 0.8f, 0.8f);
            mat->AddProperty(&diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);
            
            material_map[mesh.material_name] = (unsigned int)materials.size();
            materials.push_back(mat);
        }
    }
    scene->mNumMaterials = (unsigned int)materials.size();
    scene->mMaterials = new aiMaterial*[scene->mNumMaterials];
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i) scene->mMaterials[i] = materials[i];

    // 2. Nodes (Joint Hierarchy)
    std::map<int, aiNode*> joint_nodes;
    std::vector<aiNode*> roots;
    
    for (size_t i = 0; i < model.joints.size(); ++i) {
        const auto& j = model.joints[i];
        aiNode* node = new aiNode(j.name.c_str());
        joint_nodes[(int)i] = node;
        
        float q[4] = { j.rotate[0], j.rotate[1], j.rotate[2], j.rotate[3] };
        mat4 local = mat4_from_trs(j.translate, q, j.scale);
        node->mTransformation = to_aiMatrix4x4(local);
    }
    
    for (size_t i = 0; i < model.joints.size(); ++i) {
        int p = model.joints[i].parent;
        if (p >= 0 && joint_nodes.count(p)) {
            aiNode* parent = joint_nodes[p];
            parent->mChildren = (aiNode**)realloc(parent->mChildren, sizeof(aiNode*) * (parent->mNumChildren + 1));
            parent->mChildren[parent->mNumChildren++] = joint_nodes[(int)i];
            joint_nodes[(int)i]->mParent = parent;
        } else {
            roots.push_back(joint_nodes[(int)i]);
        }
    }

    scene->mRootNode = new aiNode("Armature");
    scene->mRootNode->mNumChildren = (unsigned int)roots.size() + 1; // +1 for Meshes node
    scene->mRootNode->mChildren = new aiNode*[scene->mRootNode->mNumChildren];
    for (size_t i = 0; i < roots.size(); ++i) {
        scene->mRootNode->mChildren[i] = roots[i];
        roots[i]->mParent = scene->mRootNode;
    }

    // 3. Meshes
    scene->mNumMeshes = (unsigned int)model.meshes.size();
    scene->mMeshes = new aiMesh*[scene->mNumMeshes];
    
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        const auto& m = model.meshes[i];
        aiMesh* am = new aiMesh();
        scene->mMeshes[i] = am;
        
        am->mName = aiString(m.name.c_str());
        am->mMaterialIndex = material_map[m.material_name];
        am->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
        
        am->mNumVertices = m.num_vertexes;
        am->mVertices = new aiVector3D[am->mNumVertices];
        am->mNormals = new aiVector3D[am->mNumVertices];
        am->mTextureCoords[0] = new aiVector3D[am->mNumVertices];
        am->mNumUVComponents[0] = 2;
        
        for (unsigned int j = 0; j < m.num_vertexes; ++j) {
            uint32_t vidx = m.first_vertex + j;
            am->mVertices[j] = aiVector3D(model.positions[vidx * 3 + 0], model.positions[vidx * 3 + 1], model.positions[vidx * 3 + 2]);
            am->mNormals[j] = aiVector3D(model.normals[vidx * 3 + 0], model.normals[vidx * 3 + 1], model.normals[vidx * 3 + 2]);
            am->mTextureCoords[0][j] = aiVector3D(model.texcoords[vidx * 2 + 0], model.texcoords[vidx * 2 + 1], 0.0f);
        }
        
        am->mNumFaces = m.num_triangles;
        am->mFaces = new aiFace[am->mNumFaces];
        for (unsigned int j = 0; j < m.num_triangles; ++j) {
            uint32_t tidx = (m.first_triangle + j) * 3;
            aiFace& face = am->mFaces[j];
            face.mNumIndices = 3;
            face.mIndices = new unsigned int[3];
            face.mIndices[0] = model.indices[tidx + 0] - m.first_vertex;
            face.mIndices[1] = model.indices[tidx + 1] - m.first_vertex;
            face.mIndices[2] = model.indices[tidx + 2] - m.first_vertex;
        }

        // Skinning
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
                
                // CRITICAL: Always add an aiBone for every joint in the hierarchy.
                // This ensures Assimp marks the FBX node as a "Skeleton" (LimbNode) 
                // instead of a regular "Null" or "Model" node.
                aiBone* bone = new aiBone();
                bone->mName = aiString(model.joints[j].name.c_str());
                bone->mOffsetMatrix = to_aiMatrix4x4(ibms[j]);
                bone->mNumWeights = (unsigned int)vWeights.size();
                if (bone->mNumWeights > 0) {
                    bone->mWeights = new aiVertexWeight[bone->mNumWeights];
                    for (unsigned int w = 0; w < bone->mNumWeights; w++) bone->mWeights[w] = vWeights[w];
                }
                meshBones.push_back(bone);
            }
            if (!meshBones.empty()) {
                am->mNumBones = (unsigned int)meshBones.size();
                am->mBones = new aiBone*[am->mNumBones];
                for (unsigned int j = 0; j < am->mNumBones; j++) am->mBones[j] = meshBones[j];
            }
        }
    }

    aiNode* meshNode = new aiNode("Meshes");
    meshNode->mParent = scene->mRootNode;
    meshNode->mNumMeshes = scene->mNumMeshes;
    meshNode->mMeshes = new unsigned int[scene->mNumMeshes];
    for (unsigned int i = 0; i < scene->mNumMeshes; i++) meshNode->mMeshes[i] = i;
    scene->mRootNode->mChildren[scene->mRootNode->mNumChildren - 1] = meshNode;

    // 4. Animations
    if (!model.animations.empty() && model.num_frames > 0) {
        scene->mNumAnimations = (unsigned int)model.animations.size();
        scene->mAnimations = new aiAnimation*[scene->mNumAnimations];
        
        for (unsigned int a = 0; a < scene->mNumAnimations; a++) {
            const auto& ad = model.animations[a];
            aiAnimation* anim = new aiAnimation();
            scene->mAnimations[a] = anim;
            anim->mName = aiString(ad.name.c_str());

            uint32_t num_keys = (uint32_t)(ad.last_frame - ad.first_frame + 1);
            // In Assimp, mDuration is in ticks.
            anim->mDuration = (double)(num_keys > 0 ? num_keys - 1 : 0);
            anim->mTicksPerSecond = (double)ad.fps;
            
            anim->mNumChannels = (unsigned int)model.joints.size();
            anim->mChannels = new aiNodeAnim*[anim->mNumChannels];
            
            for (unsigned int j = 0; j < anim->mNumChannels; j++) {
                aiNodeAnim* chan = new aiNodeAnim();
                anim->mChannels[j] = chan;
                chan->mNodeName = aiString(model.joints[j].name.c_str());
                
                chan->mNumPositionKeys = num_keys;
                chan->mPositionKeys = new aiVectorKey[num_keys];
                chan->mNumRotationKeys = num_keys;
                chan->mRotationKeys = new aiQuatKey[num_keys];
                chan->mNumScalingKeys = num_keys;
                chan->mScalingKeys = new aiVectorKey[num_keys];
                
                for (uint32_t k = 0; k < num_keys; k++) {
                    uint32_t frame_idx = std::min((uint32_t)ad.first_frame + k, model.num_frames - 1);
                    float* frame = (float*)&model.frames[frame_idx * model.num_framechannels + j * 10];
                    double time = (double)k;
                    
                    chan->mPositionKeys[k] = aiVectorKey(time, aiVector3D(frame[0], frame[1], frame[2]));
                    // iqm2glb stores quats as xyzw. aiQuaternion(w, x, y, z)
                    chan->mRotationKeys[k] = aiQuatKey(time, aiQuaternion(frame[6], frame[3], frame[4], frame[5]));
                    chan->mScalingKeys[k] = aiVectorKey(time, aiVector3D(frame[7], frame[8], frame[9]));
                }
            }
        }
    }
    
    // 5. Export Metadata (Crucial for FBX Units/Axes)
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->mNumProperties = 8;
    scene->mMetaData->mKeys = new aiString[scene->mMetaData->mNumProperties];
    scene->mMetaData->mValues = new aiMetadataEntry[scene->mMetaData->mNumProperties];

    // FBX defaults to centimeters. If our internal space is meters, we need to tell exporters.
    // Blender's FBX importer uses UnitScaleFactor.
    scene->mMetaData->Set(0, "UnitScaleFactor", (double)100.0); 
    scene->mMetaData->Set(1, "OriginalUnitScaleFactor", (double)100.0);
    
    // Y-up (UpAxis = 1)
    scene->mMetaData->Set(2, "UpAxis", (int)1);
    scene->mMetaData->Set(3, "UpAxisSign", (int)1);
    scene->mMetaData->Set(4, "FrontAxis", (int)2);
    scene->mMetaData->Set(5, "FrontAxisSign", (int)1);
    scene->mMetaData->Set(6, "CoordAxis", (int)0);
    scene->mMetaData->Set(7, "CoordAxisSign", (int)1);

    Assimp::Exporter exporter;
    const char* format_id = "fbx"; 
    
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
