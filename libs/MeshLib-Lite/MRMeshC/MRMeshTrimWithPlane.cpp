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

    try {

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

    // Phase 0 instrumentation (print/check only, no behavior change):
    // The planned attribute fix relies on MeshLib's splitEdge topology —
    //   org(eOrig) == dest(eNew) == the newly created vertex,
    //   org(eNew)  and dest(eOrig) == the two pre-plane endpoints.
    // NDEBUG is always defined in this project's build, so these are runtime
    // integer checks counted per split, not assert()s.
    int cCallbacks = 0;
    int cSplits = 0;
    int cMappingWarnings = 0;
    int vertSizeBeforePlane = 0;

    for (int p = 0; p < numPlanes; ++p) {
        vertSizeBeforePlane = (int)mesh.topology.vertSize();
        MR::Plane3f plane( { planes[p].n.x, planes[p].n.y, planes[p].n.z }, planes[p].d );
        
        auto onEdgeSplit = [&]( MR::EdgeId eOrig, MR::EdgeId eNew, float w ) {
            ++cCallbacks;
            if ( mesh.topology.org( eOrig ) != mesh.topology.dest( eNew ) )
                ++cMappingWarnings;   // halves disagree on the split vertex
            if ( (int)mesh.topology.org( eOrig ).get() <  vertSizeBeforePlane )
                ++cMappingWarnings;   // "new" vertex is not fresh this plane
            if ( (int)mesh.topology.org( eNew ).get() >= vertSizeBeforePlane )
                ++cMappingWarnings;   // origin endpoint is not a pre-plane vertex
            if ( (int)mesh.topology.dest( eOrig ).get() >= vertSizeBeforePlane )
                ++cMappingWarnings;   // dest endpoint is not a pre-plane vertex

            // Phase 1 fix: after Mesh::splitEdge( eOrig ) the new vertex is at
            // org(eOrig) == dest(eNew); the original positive-side endpoint is
            // org(eNew) and the original negative-side endpoint is dest(eOrig).
            // w is the interpolation fraction from the origin endpoint toward
            // the destination endpoint (attr[N] = attr[origin]*(1-w) + attr[dest]*w).
            // The previous code read org(eOrig)/dest(eNew) — both the new
            // vertex — and wrote org(eNew) — the original endpoint — leaving
            // every cut vertex zero-initialized and corrupting retained verts.
            MR::VertId vNew = mesh.topology.org( eOrig );   // the split vertex
            MR::VertId vA   = mesh.topology.org( eNew );    // positive-side endpoint
            MR::VertId vB   = mesh.topology.dest( eOrig );  // negative-side endpoint

            size_t maxReq = std::max({ (size_t)vNew.get(), (size_t)vA.get(), (size_t)vB.get() }) + 1024;
            if (maxReq > normals.size()) normals.resize(maxReq);
            if (maxReq > uvs.size()) uvs.resize(maxReq);
            if (maxReq > colors.size()) colors.resize(maxReq);

            // With correct maxReq sizing these fallbacks are unreachable;
            // firing one means a MeshLib split-semantics regression.
            static bool warnedAttrFallback = false;
            auto noteAttrFallback = [&]() {
                if (!warnedAttrFallback) {
                    printf("WARNING: mrTrim attribute fallback used (vertex id out of range)\n");
                    warnedAttrFallback = true;
                }
            };

            if (inNormals) {
                MR::Vector3f n1, n2;
                if (vA.get() < (int)normals.size()) n1 = normals[vA.get()]; else { n1 = MR::Vector3f(0,0,1); noteAttrFallback(); }
                if (vB.get() < (int)normals.size()) n2 = normals[vB.get()]; else { n2 = MR::Vector3f(0,0,1); noteAttrFallback(); }
                MR::Vector3f n = n1 * (1.0f - w) + n2 * w;
                float len = n.length();
                if (len > 1e-6f) n /= len;
                normals[vNew.get()] = n;
            }
            if (inUVs) {
                MR::Vector2f uv1, uv2;
                if (vA.get() < (int)uvs.size()) uv1 = uvs[vA.get()]; else { uv1 = MR::Vector2f(0,0); noteAttrFallback(); }
                if (vB.get() < (int)uvs.size()) uv2 = uvs[vB.get()]; else { uv2 = MR::Vector2f(0,0); noteAttrFallback(); }
                uvs[vNew.get()] = uv1 * (1.0f - w) + uv2 * w;
            }
            if (inColors) {
                Rgba c1, c2;
                if (vA.get() < (int)colors.size()) c1 = colors[vA.get()]; else { c1 = Rgba{255,255,255,255}; noteAttrFallback(); }
                if (vB.get() < (int)colors.size()) c2 = colors[vB.get()]; else { c2 = Rgba{255,255,255,255}; noteAttrFallback(); }
                // Round-to-nearest per component; there is no old attribute
                // behavior worth preserving (cut vertices were simply zeroed).
                colors[vNew.get()].r = (unsigned char)(c1.r * (1.0f - w) + c2.r * w + 0.5f);
                colors[vNew.get()].g = (unsigned char)(c1.g * (1.0f - w) + c2.g * w + 0.5f);
                colors[vNew.get()].b = (unsigned char)(c1.b * (1.0f - w) + c2.b * w + 0.5f);
                colors[vNew.get()].a = (unsigned char)(c1.a * (1.0f - w) + c2.a * w + 0.5f);
            }
        };

        MR::TrimWithPlaneParams trimParams;
        trimParams.plane = plane;
        trimParams.onEdgeSplitCallback = onEdgeSplit;
        MR::trimWithPlane(mesh, trimParams);
        // Independent split count: vertex slots created by this plane
        // (pack() at the end removes dead verts, so measure inside the loop).
        cSplits += (int)mesh.topology.vertSize() - vertSizeBeforePlane;
    }

    if ( cMappingWarnings > 0 )
        printf("WARNING: mrTrim: %d split-callbacks, %d splits, %d MAPPING CHECK FAILURES\n",
            cCallbacks, cSplits, cMappingWarnings);
    else
        printf("  mrTrim: mesh callbacks=%d splits=%d\n", cCallbacks, cSplits);

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
        if (newId.valid() && newId.get() < newNumVerts) {
            (*outPositions)[newId.get() * 3 + 0] = mesh.points[newId].x;
            (*outPositions)[newId.get() * 3 + 1] = mesh.points[newId].y;
            (*outPositions)[newId.get() * 3 + 2] = mesh.points[newId].z;

            if (inNormals) {
                MR::Vector3f n = (i < (int)normals.size()) ? normals[i] : MR::Vector3f(0,0,1);
                (*outNormals)[newId.get() * 3 + 0] = n.x;
                (*outNormals)[newId.get() * 3 + 1] = n.y;
                (*outNormals)[newId.get() * 3 + 2] = n.z;
            }
            if (inUVs) {
                MR::Vector2f uv = (i < (int)uvs.size()) ? uvs[i] : MR::Vector2f(0,0);
                (*outUVs)[newId.get() * 2 + 0] = uv.x;
                (*outUVs)[newId.get() * 2 + 1] = uv.y;
            }
            if (inColors) {
                Rgba c = (i < (int)colors.size()) ? colors[i] : Rgba{255,255,255,255};
                (*outColors)[newId.get() * 4 + 0] = c.r;
                (*outColors)[newId.get() * 4 + 1] = c.g;
                (*outColors)[newId.get() * 4 + 2] = c.b;
                (*outColors)[newId.get() * 4 + 3] = c.a;
            }
        }
    }

    int triIdx = 0;
    auto triArr = mesh.topology.getTriangulation();
    for (size_t fi = 0; fi < triArr.size() && triIdx < newNumTris; ++fi) {
        MR::FaceId fId(fi);
        if (triArr[fId][0].valid()) {
            (*outIndices)[triIdx * 3 + 0] = triArr[fId][0].get();
            (*outIndices)[triIdx * 3 + 1] = triArr[fId][1].get();
            (*outIndices)[triIdx * 3 + 2] = triArr[fId][2].get();
            triIdx++;
        }
    }
    }
    catch (const std::exception& e) {
        // C++ exception from MeshLib. Log it and return completely trimmed mesh
        printf("WARNING: MeshLib exception in mrTrimMiscModelMesh: %s\n", e.what());
        *outNumVerts = 0;
        *outNumIndices = 0;
    }
    catch (...) {
        printf("WARNING: Unknown C++ exception in mrTrimMiscModelMesh\n");
        *outNumVerts = 0;
        *outNumIndices = 0;
    }
}

MR_EXTERN_C_END
#include <exception>
