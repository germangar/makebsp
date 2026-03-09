#include "glb_writer_assimp.h"
#include <assimp/scene.h>
#include <assimp/exporter.hpp>
#include <assimp/postprocess.h>
#include <iostream>
#include <vector>
#include <map>

// Helper to convert internal mat4 to aiMatrix4x4
aiMatrix4x4 to_aiMatrix4x4(const mat4& m) {
    aiMatrix4x4 res;
    // mat4 is column-major: m[col*4 + row]
    // aiMatrix4x4 is row-major: res[row][col]
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            res[row][col] = m.m[col * 4 + row];
        }
    }
    return res;
}

#include <assimp/anim.h>

bool write_glb_assimp(const Model& model, const char* filename) {
    aiScene* scene = new aiScene();
    scene->mRootNode = new aiNode("Root");

    // 1. Materials (Targeting GLTF PBR)
    std::map<std::string, unsigned int> material_map;
    std::vector<aiMaterial*> materials;
    
    for (const auto& mesh : model.meshes) {
        if (material_map.find(mesh.material_name) == material_map.end()) {
            aiMaterial* mat = new aiMaterial();
            aiString name(mesh.material_name.c_str());
            mat->AddProperty(&name, AI_MATKEY_NAME);
            
            // Standard GLTF PBR properties
            aiColor3D baseColor(0.8f, 0.8f, 0.8f);
            mat->AddProperty(&baseColor, 1, AI_MATKEY_COLOR_DIFFUSE);
            float roughness = 0.5f;
            mat->AddProperty(&roughness, 1, AI_MATKEY_ROUGHNESS_FACTOR);
            float metallic = 0.0f;
            mat->AddProperty(&metallic, 1, AI_MATKEY_METALLIC_FACTOR);
            
            // Fix backface culling/occlusion issues
            int two_sided = 1;
            mat->AddProperty(&two_sided, 1, AI_MATKEY_TWOSIDED);
            
            material_map[mesh.material_name] = (unsigned int)materials.size();
            materials.push_back(mat);
        }
    }
    
    scene->mNumMaterials = (unsigned int)materials.size();
    scene->mMaterials = new aiMaterial*[scene->mNumMaterials];
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i) scene->mMaterials[i] = materials[i];

    // 2. Nodes (Skeleton)
    std::map<int, aiNode*> joint_nodes;
    if (!model.joints.empty()) {
        for (size_t i = 0; i < model.joints.size(); ++i) {
            const auto& j = model.joints[i];
            aiNode* node = new aiNode(j.name.c_str());
            joint_nodes[i] = node;
            
            float q[4] = { j.rotate[0], j.rotate[1], j.rotate[2], j.rotate[3] };
            mat4 local = mat4_from_trs(j.translate, q, j.scale);
            node->mTransformation = to_aiMatrix4x4(local);
        }
        
        std::vector<aiNode*> roots;
        for (size_t i = 0; i < model.joints.size(); ++i) {
            if (model.joints[i].parent >= 0) {
                aiNode* parent = joint_nodes[model.joints[i].parent];
                if (!parent->mChildren) parent->mChildren = new aiNode*[model.joints.size()];
                parent->mChildren[parent->mNumChildren++] = joint_nodes[i];
                joint_nodes[i]->mParent = parent;
            } else {
                roots.push_back(joint_nodes[i]);
            }
        }
        
        scene->mRootNode->mNumChildren = (unsigned int)roots.size() + 1;
        scene->mRootNode->mChildren = new aiNode*[scene->mRootNode->mNumChildren];
        for (size_t i = 0; i < roots.size(); ++i) {
            scene->mRootNode->mChildren[i] = roots[i];
            roots[i]->mParent = scene->mRootNode;
        }
    } else {
        scene->mRootNode->mNumChildren = 1;
        scene->mRootNode->mChildren = new aiNode*[1];
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

        // Skinning (Bones)
        if (!model.joints.empty()) {
            std::vector<aiBone*> meshBones;
            for (unsigned int j = 0; j < (unsigned int)model.joints.size(); ++j) {
                std::vector<aiVertexWeight> vWeights;
                for (unsigned int v = 0; v < m.num_vertexes; v++) {
                    uint32_t vidx = m.first_vertex + v;
                    for (int k = 0; k < 4; k++) {
                        if (model.joints_0[vidx * 4 + k] == (uint8_t)j && model.weights_0[vidx * 4 + k] > 0.01f) {
                            vWeights.push_back(aiVertexWeight(v, model.weights_0[vidx * 4 + k]));
                        }
                    }
                }
                if (!vWeights.empty()) {
                    aiBone* bone = new aiBone();
                    bone->mName = aiString(model.joints[j].name.c_str());
                    bone->mOffsetMatrix = to_aiMatrix4x4(model.ibms[j]);
                    bone->mNumWeights = (unsigned int)vWeights.size();
                    bone->mWeights = new aiVertexWeight[bone->mNumWeights];
                    for (unsigned int w = 0; w < bone->mNumWeights; w++) bone->mWeights[w] = vWeights[w];
                    meshBones.push_back(bone);
                }
            }
            if (!meshBones.empty()) {
                am->mNumBones = (unsigned int)meshBones.size();
                am->mBones = new aiBone*[am->mNumBones];
                for (unsigned int j = 0; j < am->mNumBones; j++) am->mBones[j] = meshBones[j];
            }
        }
    }

    // Connect meshes to a dedicated child node
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

            uint32_t num_keys = (ad.last_frame - ad.first_frame + 1);
            anim->mDuration = (double)(num_keys > 1 ? num_keys - 1 : 0.001);
            anim->mTicksPerSecond = (double)ad.fps;
            
            // Debug: std::cout << "Writing Anim: " << ad.name << " Keys: " << num_keys << " Dur: " << anim->mDuration << " TPS: " << anim->mTicksPerSecond << std::endl;

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
                    uint32_t frame_idx = std::min(ad.first_frame + k, model.num_frames - 1);
                    float* frame = (float*)&model.frames[frame_idx * model.num_framechannels + j * 10];
                    double time = (double)k;
                    
                    chan->mPositionKeys[k] = aiVectorKey(time, aiVector3D(frame[0], frame[1], frame[2]));
                    chan->mRotationKeys[k] = aiQuatKey(time, aiQuaternion(frame[6], frame[3], frame[4], frame[5])); // w, x, y, z
                    chan->mScalingKeys[k] = aiVectorKey(time, aiVector3D(frame[7], frame[8], frame[9]));
                }
            }
        }
    }

    // 5. Export
    Assimp::Exporter exporter;
    
    // Choose exporter based on extension
    std::string fn(filename);
    const char* format_id = "gltf2"; // Default to glTF 2.0 (JSON)
    if (fn.size() > 4 && fn.substr(fn.size() - 4) == ".glb") {
        format_id = "glb2"; // Binary glTF 2.0 (Single file)
    }

    std::cout << "Exporting to " << filename << " using '" << format_id << "' exporter..." << std::endl;
    aiReturn ret = exporter.Export(scene, format_id, filename);
    
    if (ret != aiReturn_SUCCESS) {
        std::cerr << "Assimp failed to export: " << exporter.GetErrorString() << std::endl;
        delete scene;
        return false;
    }
    
    delete scene; 
    return true;
}
