#include "MRMeshTrimWithPlane.h"
#include "MRMesh/MRMeshTrimWithPlane.h"
#include "MRMesh/MRMesh.h"
#include "detail/TypeCast.h"

using namespace MR;
REGISTER_AUTO_CAST( Mesh )

MR_EXTERN_C_BEGIN

void mrTrimMeshWithPlane( MRMesh* mesh, const MRPlane3f* plane )
{
    if ( !mesh || !plane ) return;
    MR::Plane3f p( { plane->n.x, plane->n.y, plane->n.z }, plane->d );
    MR::trimWithPlane( *auto_cast( mesh ), p );
}

MR_EXTERN_C_END

#include <vector>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "MRMesh/MRVector2.h"
#include "MRMesh/MRVector3.h"

MR_EXTERN_C_BEGIN

void mrTrimMiscModelMesh(
    const float* inPositions, int numVerts,
    const float* inNormals,
    const float* inUVs,      
    const unsigned char* inColors, 
    const int* inIndices, int numIndices,
    const MRPlane3f* planes, int numPlanes,
    float** outPositions, float** outNormals, float** outUVs, unsigned char** outColors, int** outIndices,
    int* outNumVerts, int* outNumIndices
) {
    *outPositions = nullptr; *outNormals = nullptr; *outUVs = nullptr; *outColors = nullptr; *outIndices = nullptr;
    *outNumVerts = 0; *outNumIndices = 0;

    if (numVerts == 0 || numIndices == 0) return;

    std::vector<MR::Vector3f> pts(numVerts);
    for (int i = 0; i < numVerts; ++i) {
        pts[i] = MR::Vector3f(inPositions[i*3], inPositions[i*3+1], inPositions[i*3+2]);
    }

    std::vector<MR::Vector3f> normals;
    if (inNormals) {
        normals.resize(numVerts);
        for (int i = 0; i < numVerts; ++i) {
            normals[i] = MR::Vector3f(inNormals[i*3], inNormals[i*3+1], inNormals[i*3+2]);
        }
    }

    std::vector<MR::Vector2f> uvs;
    if (inUVs) {
        uvs.resize(numVerts);
        for (int i = 0; i < numVerts; ++i) {
            uvs[i] = MR::Vector2f(inUVs[i*2], inUVs[i*2+1]);
        }
    }

    struct Rgba { unsigned char r, g, b, a; };
    std::vector<Rgba> colors;
    if (inColors) {
        colors.resize(numVerts);
        for (int i = 0; i < numVerts; ++i) {
            colors[i] = {inColors[i*4], inColors[i*4+1], inColors[i*4+2], inColors[i*4+3]};
        }
    }

    std::vector<MR::ThreeVertIds> tris(numIndices / 3);
    for (int i = 0; i < numIndices / 3; ++i) {
        tris[i] = { MR::VertId(inIndices[i*3]), MR::VertId(inIndices[i*3+1]), MR::VertId(inIndices[i*3+2]) };
    }

    // Let's use standard fromTriangles to maintain 1:1 mapping!
    MR::Triangulation triArray(tris.begin(), tris.end());
    MR::Mesh mesh = MR::Mesh::fromTriangles(std::move(pts), triArray);

    normals.resize(mesh.points.capacity());
    uvs.resize(mesh.points.capacity());
    colors.resize(mesh.points.capacity());

    for (int p = 0; p < numPlanes; ++p) {
        MR::Plane3f plane( { planes[p].n.x, planes[p].n.y, planes[p].n.z }, planes[p].d );
        
        auto onEdgeSplit = [&]( MR::EdgeId eOrig, MR::EdgeId eNew, float w ) {
            MR::VertId v1 = mesh.topology.org( eOrig );
            MR::VertId v2 = mesh.topology.dest( eNew );
            MR::VertId vNew = mesh.topology.org( eNew );

            if (vNew.get() >= normals.size()) {
                normals.resize( vNew.get() + 1000 );
            }
            if (vNew.get() >= uvs.size()) {
                uvs.resize( vNew.get() + 1000 );
            }
            if (vNew.get() >= colors.size()) {
                colors.resize( vNew.get() + 1000 );
            }

            if (inNormals) {
                MR::Vector3f n = normals[v1.get()] * w + normals[v2.get()] * (1.0f - w);
                float len = n.length();
                if (len > 1e-6f) n /= len;
                normals[vNew.get()] = n;
            }
            if (inUVs) {
                uvs[vNew.get()] = uvs[v1.get()] * w + uvs[v2.get()] * (1.0f - w);
            }
            if (inColors) {
                const auto& c1 = colors[v1.get()];
                const auto& c2 = colors[v2.get()];
                colors[vNew.get()].r = (unsigned char)(c1.r * w + c2.r * (1.0f - w));
                colors[vNew.get()].g = (unsigned char)(c1.g * w + c2.g * (1.0f - w));
                colors[vNew.get()].b = (unsigned char)(c1.b * w + c2.b * (1.0f - w));
                colors[vNew.get()].a = (unsigned char)(c1.a * w + c2.a * (1.0f - w));
            }
        };

        MR::TrimWithPlaneParams trimParams;
        trimParams.plane = plane;
        trimParams.onEdgeSplitCallback = onEdgeSplit;
        MR::trimWithPlane(mesh, trimParams);
    }

    MR::VertMap outVmap;
    mesh.pack( (MR::FaceMap*)nullptr, &outVmap );

    int newNumVerts = mesh.points.size();
    int newNumTris = mesh.topology.faceSize();

    if (newNumVerts == 0 || newNumTris == 0) return;

    *outPositions = (float*)malloc(newNumVerts * 3 * sizeof(float));
    if (inNormals) *outNormals = (float*)malloc(newNumVerts * 3 * sizeof(float));
    if (inUVs) *outUVs = (float*)malloc(newNumVerts * 2 * sizeof(float));
    if (inColors) *outColors = (unsigned char*)malloc(newNumVerts * 4);
    *outIndices = (int*)malloc(newNumTris * 3 * sizeof(int));

    *outNumVerts = newNumVerts;
    *outNumIndices = newNumTris * 3;

    for (int i = 0; i < outVmap.size(); ++i) {
        MR::VertId newId = outVmap[MR::VertId(i)];
        if (newId.valid()) {
            (*outPositions)[newId.get() * 3 + 0] = mesh.points[newId].x;
            (*outPositions)[newId.get() * 3 + 1] = mesh.points[newId].y;
            (*outPositions)[newId.get() * 3 + 2] = mesh.points[newId].z;

            if (inNormals) {
                (*outNormals)[newId.get() * 3 + 0] = normals[i].x;
                (*outNormals)[newId.get() * 3 + 1] = normals[i].y;
                (*outNormals)[newId.get() * 3 + 2] = normals[i].z;
            }
            if (inUVs) {
                (*outUVs)[newId.get() * 2 + 0] = uvs[i].x;
                (*outUVs)[newId.get() * 2 + 1] = uvs[i].y;
            }
            if (inColors) {
                (*outColors)[newId.get() * 4 + 0] = colors[i].r;
                (*outColors)[newId.get() * 4 + 1] = colors[i].g;
                (*outColors)[newId.get() * 4 + 2] = colors[i].b;
                (*outColors)[newId.get() * 4 + 3] = colors[i].a;
            }
        }
    }

    int triIdx = 0;
    auto triArr = mesh.topology.getTriangulation();
    for (size_t fi = 0; fi < triArr.size(); ++fi) {
        MR::FaceId fId(fi);
        if (triArr[fId][0].valid()) {
            (*outIndices)[triIdx * 3 + 0] = triArr[fId][0].get();
            (*outIndices)[triIdx * 3 + 1] = triArr[fId][1].get();
            (*outIndices)[triIdx * 3 + 2] = triArr[fId][2].get();
            triIdx++;
        }
    }
}

MR_EXTERN_C_END
