# func_snip Implementation Plan — 2-Phase Rollout (Validated)

## Decisions Finalized
- **Cap Texturing**: Caps on cut faces will have `(0,0)` texture UVs. Accepted, not a concern for initial implementation.
- **Brush Union**: The `MRBooleanOperationUnion` step in `ProcessMapEntities` is confirmed. It is scoped strictly to the `func_snip` entity being processed — one independent union per entity.

---

## Critical Findings from Final Audit

| # | Issue | Fix |
|---|-------|-----|
| 1 | `miscModelMesh_t`, `mesh->numMeshes`, `mesh->wasCut` **do not exist** in `modelInstance_t` (see `qbsp.h:530`). The current struct only holds `drawSurfs[]` and `collision_meshes[]`. | Must add `miscModelMesh_t` struct definition and fields `meshes[]`, `numMeshes`, `wasCut` to `qbsp.h` and `modelInstance_t`. |
| 2 | `mrMeshTopologyGetTriVerts` **does not exist** in the MeshLib C API (`MRMeshTopology.h`). The triangulation is accessed via `mrMeshTopologyGetTriangulation(top)` which returns a `MRTriangulation*` (a vector of `MRThreeVertIds`). | Replace the per-face call in the reconstruction loop with a single `mrMeshTopologyGetTriangulation` call, then index into `triangulation->data[f]` for valid faces. |
| 3 | **FreeBrush ordering bug**: The loop calls `FreeBrush(b)` at the end of each iteration, but `b->next` has already been captured as `next` at the top. This is actually **correct** — no bug. The loop advances to `next` before `FreeBrush(b)` is called. ✅ Verified. |
| 4 | The `decals.c` cleanup pattern does **not** free `epairs`. In `ProcessMapEntities`, other entities like `func_group` and `func_terrain` call `FreeEpairs` and set `epairs = NULL` to suppress entity from BSP output. `_decal` does NOT do this because it is processed separately (not in `ProcessMapEntities`). For `func_snip`, which IS in `ProcessMapEntities`, we must follow the `func_group`/`func_terrain` pattern: call `FreeEpairs` and set `epairs = NULL`. |
| 5 | `mrBitSetTest` takes `(const MRBitSet*, size_t)`. `mrMeshTopologyGetValidFaces` returns `const MRFaceBitSet*`. `MRFaceBitSet` is an opaque struct. The cast `(const MRBitSet*)validF` is **correct** by convention (all typed BitSets are layout-compatible). ✅ Verified. |
| 6 | `MRFaceId` and `MRVertId` are `struct {int id;}` (see `MRId.h:12-14`). Designator init `{.id = f}` is correct C99. ✅ Verified. |
| 7 | `mrBooleanWithAttributes` signature: `(meshA, attrsA, meshB, attrsB, operation, params, outAttrs)`. The operator mesh (meshB) has no attributes, so `NULL` is correct for `attrsB`. ✅ Verified. |
| 8 | `mrMeshPoints` returns `const MRVector3f*` indexed by `VertId`. `mrMeshPointsNum` returns `size_t`. Both confirmed in `MRMesh.h:28,34`. ✅ Verified. |
| 9 | `mrMeshNormalFromVert` exists at `MRMesh.h:114`. Takes `(const MRMesh*, MRVertId)`. ✅ Verified. |
| 10| **CRITICAL LINKER FAILURE**: `MeshLib-Lite` lacks the `MRMeshLogic` module which provides the actual C++ implementation of `MR::boolean`. The `MRMeshC` wrapper declares `mrBoolean` but linking fails with undefined references. | **Pivot approach**: Since `func_snip` brushes are purely convex (composed of planes), intersecting a mesh with a brush is mathematically identical to sequentially trimming the mesh with each plane of the brush. We will use `MR::trimWithPlane` instead of `mrBoolean`, exposing it via a tiny C wrapper in `csg_mesh.cpp` (renamed from `.c` to allow C++ calls). |

---

## New Struct Definitions Required (in `qbsp.h`)

```c
// Intermediate per-mesh data held between Load and Integrate passes.
// Added alongside modelInstance_t definition.
typedef struct miscModelMesh_s
{
    float      *positions;   // flat xyz array: [x0,y0,z0, x1,y1,z1, ...] — world space
    float      *normals;     // flat xyz array, world space
    float      *st;          // flat uv  array: [u0,v0, u1,v1, ...] — texture UVs channel 0
    byte       *colors;      // flat rgba array: [r0,g0,b0,a0, ...] — 255 default
    int        *indices;     // triangle index array
    int         numVerts;
    int         numIndices;
    vec3_t      mins, maxs;  // world-space AABB for AABB overlap test
    qboolean    wasCut;      // set to qtrue by PerformMeshCSG if this mesh was modified
    shaderInfo_t *si;        // shader for this sub-mesh
    char        shaderName[MAX_QPATH];

    // Entity keys needed by IntegrateTriangleModels
    qboolean    flipWinding;
    int         uvChannel;   // 0 or 1 — which Assimp channel had valid UVs (0 for cut meshes)
    qboolean    hasOriginalUVs; // false if Assimp had no UVs, or if wasCut
} miscModelMesh_t;

#define MAX_MISC_MODEL_MESHES 256  // per modelInstance_t
```

Add to `modelInstance_t`:
```c
typedef struct modelInstance_s
{
    // ... existing fields ...
    miscModelMesh_t *meshes[MAX_MISC_MODEL_MESHES]; // intermediate load data
    int              numMeshes;
} modelInstance_t;
```

---

## Rollout Phase 1: Architecture Refactor

### Step 1. `LoadTriangleModels` Refactor (`misc_model.c`)

`LoadTriangleModels` now outputs instance range indices. It fills `miscModelMesh_t` but generates **no** collision, **no** xatlas, **no** `drawSurf_t`.

```c
void LoadTriangleModels(entity_t *eparent, int *outStartInst, int *outEndInst)
{
    *outStartInst = numModelInstances;

    // ... existing entity loop (modelgroup filtering, origin/angles/scale parsing) ...

    for (each matching misc_model entity) {
        modelInstance_t *inst = &modelInstances[numModelInstances++];

        // ... existing: lightmapScale, forceUVGen, collision_type_override ...

        inst->numMeshes = 0;
        inst->numDrawSurfs = 0;
        inst->drawSurfs = malloc(sizeof(mapDrawSurface_t*) * 1024);

        AnglesToMatrix(angles, rotationMatrix);

        for (int i = 0; i < scene->mNumMeshes; i++) {
            struct aiMesh *mesh = scene->mMeshes[i];

            // Skip tag_light, etc (existing logic)
            if (!Q_stricmp(shaderName, "tag_light")) { /* existing processing */ continue; }
            if (si && (si->surfaceFlags & SURF_SKIP)) continue;

            miscModelMesh_t *mm = malloc(sizeof(miscModelMesh_t));
            memset(mm, 0, sizeof(miscModelMesh_t));
            mm->si = si;
            Q_strncpyz(mm->shaderName, shaderName, MAX_QPATH);
            mm->wasCut = qfalse;

            // Determine UV channel and whether original UVs exist
            mm->uvChannel = (mesh->mTextureCoords[1]) ? 1 : 0;
            mm->hasOriginalUVs = (mesh->mTextureCoords[mm->uvChannel] != NULL);

            // Negative scale: flip winding if mirrored
            float scaleDet = scale_vec[0] * scale_vec[1] * scale_vec[2];
            mm->flipWinding = (scaleDet < 0.0f);

            // Allocate arrays
            mm->numVerts = mesh->mNumVertices;
            mm->positions = malloc(sizeof(float) * 3 * mm->numVerts);
            mm->normals   = malloc(sizeof(float) * 3 * mm->numVerts);
            mm->st        = malloc(sizeof(float) * 2 * mm->numVerts);
            mm->colors    = malloc(sizeof(byte)  * 4 * mm->numVerts);

            // Compute AABB and transform vertices
            ClearBounds(mm->mins, mm->maxs);
            for (int j = 0; j < mm->numVerts; j++) {
                // Axis Swap (Assimp Y-Up -> Quake Z-Up), then scale_vec, then rotation, then translation
                vec3_t tx, wp;
                tx[0] = mesh->mVertices[j].x * scale_vec[0];
                tx[1] = -mesh->mVertices[j].z * scale_vec[1];
                tx[2] =  mesh->mVertices[j].y * scale_vec[2];
                wp[0] = origin[0] + tx[0]*R[0][0] + tx[1]*R[1][0] + tx[2]*R[2][0];
                wp[1] = origin[1] + tx[0]*R[0][1] + tx[1]*R[1][1] + tx[2]*R[2][1];
                wp[2] = origin[2] + tx[0]*R[0][2] + tx[1]*R[1][2] + tx[2]*R[2][2];
                mm->positions[j*3+0] = wp[0];
                mm->positions[j*3+1] = wp[1];
                mm->positions[j*3+2] = wp[2];
                AddPointToBounds(wp, mm->mins, mm->maxs);

                // Normal (same transform, no translation)
                vec3_t tn, wn;
                tn[0] = mesh->mNormals[j].x * (scale_vec[0] < 0 ? -1 : 1); // invert axis if mirrored
                tn[1] = -mesh->mNormals[j].z * (scale_vec[1] < 0 ? -1 : 1);
                tn[2] =  mesh->mNormals[j].y * (scale_vec[2] < 0 ? -1 : 1);
                wn[0] = tn[0]*R[0][0] + tn[1]*R[1][0] + tn[2]*R[2][0];
                wn[1] = tn[0]*R[0][1] + tn[1]*R[1][1] + tn[2]*R[2][1];
                wn[2] = tn[0]*R[0][2] + tn[1]*R[1][2] + tn[2]*R[2][2];
                VectorNormalize(wn, wn);
                mm->normals[j*3+0] = wn[0];
                mm->normals[j*3+1] = wn[1];
                mm->normals[j*3+2] = wn[2];

                // Texture ST (channel 0 only — channel 1 is custom lightmap, useless post-cut)
                if (mm->hasOriginalUVs) {
                    mm->st[j*2+0] = mesh->mTextureCoords[mm->uvChannel][j].x;
                    mm->st[j*2+1] = mesh->mTextureCoords[mm->uvChannel][j].y;
                } else {
                    mm->st[j*2+0] = mm->st[j*2+1] = 0.0f;
                }

                // Vertex color (default white)
                mm->colors[j*4+0] = mm->colors[j*4+1] = mm->colors[j*4+2] = mm->colors[j*4+3] = 255;
            }

            // Build index array (respecting flipWinding)
            int validTris = 0;
            for (int j = 0; j < (int)mesh->mNumFaces; j++)
                if (mesh->mFaces[j].mNumIndices == 3) validTris++;
            mm->numIndices = validTris * 3;
            mm->indices = malloc(sizeof(int) * mm->numIndices);
            int idx = 0;
            for (int j = 0; j < (int)mesh->mNumFaces; j++) {
                if (mesh->mFaces[j].mNumIndices != 3) continue;
                mm->indices[idx++] = mesh->mFaces[j].mIndices[0];
                mm->indices[idx++] = mm->flipWinding ? mesh->mFaces[j].mIndices[2] : mesh->mFaces[j].mIndices[1];
                mm->indices[idx++] = mm->flipWinding ? mesh->mFaces[j].mIndices[1] : mesh->mFaces[j].mIndices[2];
            }

            inst->meshes[inst->numMeshes++] = mm;
        }
    }

    *outEndInst = numModelInstances;
}
```

### Step 2. New `xatlas` Wrappers

Since `xatlas` is deferred and vertices are already in world space, new wrappers accepting raw float arrays must replace the `aiMesh*`-taking functions:

```c
// Attempts to pack existing UVs via xatlas
uv_t *TryXAtlasUVsFromArrays(
    const float *uvs2f,     // [u0,v0, u1,v1, ...] — source UVs
    int numVerts,
    const float *positions3f, // world-space positions (for island area calc)
    const int *indices,
    int numIndices,
    int ssize,
    float lightmapScale);

// Generates a full UV map from scratch via xatlas
uv_t *GenerateXAtlasUVsFromArrays(
    const float *positions3f,
    int numVerts,
    const int *indices,
    int numIndices,
    int ssize,
    float lightmapScale);
// Note: scale_vec is {1,1,1} (already world space), flipWinding is qfalse
```

### Step 3. `IntegrateTriangleModels` (`misc_model.c`)

```c
void IntegrateTriangleModels(int startInst, int endInst, entity_t *eparent)
{
    for (int i = startInst; i < endInst; i++) {
        modelInstance_t *inst = &modelInstances[i];

        for (int j = 0; j < inst->numMeshes; j++) {
            miscModelMesh_t *mm = inst->meshes[j];
            shaderInfo_t *si = mm->si;

            // 1. STEP 1: Extract collision topology (existing logic, sourced from mm arrays)
            if (si && (si->contents & CONTENTS_SOLID) && inst->num_collision_meshes < MAX_MODEL_COLLISION_MESHES) {
                colMesh_t *cm = malloc(sizeof(colMesh_t));
                memset(cm, 0, sizeof(colMesh_t));
                cm->shaderInfo = si;
                cm->numVerts = mm->numVerts;
                cm->verts = malloc(sizeof(vec3_t) * cm->numVerts);
                for (int k = 0; k < mm->numVerts; k++) {
                    cm->verts[k][0] = mm->positions[k*3+0];
                    cm->verts[k][1] = mm->positions[k*3+1];
                    cm->verts[k][2] = mm->positions[k*3+2];
                }
                // colMesh_t tris — indices already winding-corrected
                int numTris = mm->numIndices / 3;
                cm->numTris = numTris;
                cm->tris = malloc(sizeof(colTri_t) * numTris);
                for (int k = 0; k < numTris; k++) {
                    cm->tris[k][0] = mm->indices[k*3+0];
                    cm->tris[k][1] = mm->indices[k*3+1];
                    cm->tris[k][2] = mm->indices[k*3+2];
                }
                inst->collision_meshes[inst->num_collision_meshes++] = cm;
            }

            // 2. STEP 2: Generate lightmap UVs
            if (si && (si->surfaceFlags & SURF_SKIP)) {
                FreeMiscModelMesh(mm); continue;
            }
            uv_t *xatlasUVs = NULL;
            int ssize = /* resolve from si / entity / game defaults, existing logic */;
            if (!mm->wasCut && mm->hasOriginalUVs && !forceUVGen) {
                xatlasUVs = TryXAtlasUVsFromArrays(mm->st, mm->numVerts, mm->positions, mm->indices, mm->numIndices, ssize, inst->lightmapScale);
            }
            if (!xatlasUVs) {
                xatlasUVs = GenerateXAtlasUVsFromArrays(mm->positions, mm->numVerts, mm->indices, mm->numIndices, ssize, inst->lightmapScale);
            }
            if (!xatlasUVs)
                _printf("WARNING: Total xatlas generation failure for %s.\n", mm->shaderName);

            // 3. STEP 3: Chunk into mapDrawSurface_t (existing MAX_SURFACE_VERTS logic)
            // ... existing chunking loop, sourcing verts from mm->positions/normals/st/colors ...
            // Note: xatlasUVs is indexed per-face-corner (face*3+corner), same as current code
            // ResolveMiscModelSurfaceProperties still reads from inst->creator entity

            free(xatlasUVs);
            FreeMiscModelMesh(mm);
        }
    }
}
```

### Step 4. BSP Hook for Phase 1 (`bsp.c`)
```c
// Inside ProcessWorldModel and ProcessSubModel — replaces current LoadTriangleModels(e) call
int startInst, endInst;
LoadTriangleModels(e, &startInst, &endInst);
IntegrateTriangleModels(startInst, endInst, e);
// PerformMeshCSG not yet present — added in Phase 2
```

> **STOP HERE: Test that compiled maps are visually and physically identical to the current pipeline before proceeding.**

---

## Rollout Phase 2: MeshLib & `func_snip` Integration

### Step 1. Compile `csg_mesh.c` as C++

To directly use `MR::trimWithPlane`, we must rename `csg_mesh.c` to `csg_mesh.cpp` and update the `Makefile` or just add `extern "C"` to its functions.
Wait, `makebsp` Makefile compiles `.c` files in `q3map` with `gcc`. We can just write `csg_mesh.cpp` in `q3map/` and add a rule in `Makefile`, OR we can just write a wrapper in `shared/` or `libs/`. 
To minimize `Makefile` changes, we will keep `csg_mesh.c` as C, and add a C-wrapper `MRMeshTrimWithPlane.cpp` in `libs/MeshLib-Lite/MRMeshC/` which automatically gets compiled by the existing Makefile wildcard rule.

### Step 2. `BrushToPlanes` instead of `BrushToMRMesh`

Instead of creating a MeshLib operator mesh, we just need the planes of the `func_snip` brushes.
Wait, `func_snip` can have multiple brushes. We will store all brushes in `funcSnipOperator_t`.

```c
typedef struct {
    int numPlanes;
    plane_t planes[MAX_BUILD_SIDES];
} snipBrush_t;

typedef struct {
    snipBrush_t brushes[MAX_BUILD_SIDES];
    int numBrushes;
    vec3_t mins, maxs;
} funcSnipOperator_t;
```

### Step 3. Intercept & Store in `map.c` (`ProcessMapEntities`)

```c
if (!strcmp("func_snip", classname))
{
    funcSnipOperator_t op = {0};
    ClearBounds(op.mins, op.maxs);

    bspbrush_t *b, *next;
    for (b = mapent->brushes; b; b = next) {
        next = b->next;

        if (op.numBrushes < MAX_BUILD_SIDES) {
            snipBrush_t *sb = &op.brushes[op.numBrushes++];
            sb->numPlanes = b->numsides;
            for (int i = 0; i < b->numsides; i++) {
                sb->planes[i] = mapplanes[b->sides[i].planenum];
            }
            AddPointToBounds(b->mins, op.mins, op.maxs);
            AddPointToBounds(b->maxs, op.mins, op.maxs);
        }
        FreeBrush(b);
    }
    mapent->brushes = NULL;

    if (op.numBrushes > 0)
        StoreFuncSnipOperator(&op);

    // Free patches
    // Suppress entity
    continue;
}
```

### Step 3. `PerformMeshCSG(startInst, endInst)` in `csg_mesh.c`

```c
void PerformMeshCSG(int startInst, int endInst)
{
    if (numFuncSnipOperators == 0) return; // fast-path

    for (int i = startInst; i < endInst; i++) {
        modelInstance_t *inst = &modelInstances[i];

        for (int j = 0; j < inst->numMeshes; j++) {
            miscModelMesh_t *mm = inst->meshes[j];

            for (int k = 0; k < numFuncSnipOperators; k++) {
                funcSnipOperator_t *op = &funcSnipOperators[k];

                // 1. AABB overlap test
                if (!BoundsIntersect(mm->mins, mm->maxs, op->mins, op->maxs)) continue;

                // 2. Convert mm → MRMesh + MRMeshAttributes
                MRVector3f *mrVerts = malloc(sizeof(MRVector3f) * mm->numVerts);
                for (int v = 0; v < mm->numVerts; v++) {
                    mrVerts[v].x = mm->positions[v*3+0];
                    mrVerts[v].y = mm->positions[v*3+1];
                    mrVerts[v].z = mm->positions[v*3+2];
                }
                MRThreeVertIds *mrTris = malloc(sizeof(MRThreeVertIds) * (mm->numIndices / 3));
                for (int t = 0; t < mm->numIndices / 3; t++) {
                    mrTris[t][0].id = mm->indices[t*3+0];
                    mrTris[t][1].id = mm->indices[t*3+1];
                    mrTris[t][2].id = mm->indices[t*3+2];
                }
                MRMesh *mrMeshA = mrMeshFromTriangles(mrVerts, mm->numVerts, mrTris, mm->numIndices / 3);
                free(mrVerts); free(mrTris);

                MRMeshAttributes attrsA = {0};
                attrsA.uvCoords = malloc(sizeof(MRVector2f) * mm->numVerts);
                attrsA.numUvs   = mm->numVerts;
                for (int v = 0; v < mm->numVerts; v++) {
                    attrsA.uvCoords[v].x = mm->st[v*2+0];
                    attrsA.uvCoords[v].y = mm->st[v*2+1];
                }
                attrsA.vertColors = malloc(sizeof(MRColor) * mm->numVerts);
                attrsA.numColors  = mm->numVerts;
                for (int v = 0; v < mm->numVerts; v++) {
                    attrsA.vertColors[v].r = mm->colors[v*4+0];
                    attrsA.vertColors[v].g = mm->colors[v*4+1];
                    attrsA.vertColors[v].b = mm->colors[v*4+2];
                    attrsA.vertColors[v].a = mm->colors[v*4+3];
                }

                // 3. Perform Boolean via Plane Trimming
                // For each brush in the snip operator, we take a copy of the original mesh,
                // trim it sequentially against all planes of the brush, and then collect the results.
                // (Detailed implementation of this C++ wrapper will be presented in execution)

                // 4. Reconstruct mm from result
                size_t numPts = mrMeshPointsNum(res.mesh);
                const MRVector3f *outPts = mrMeshPoints(res.mesh);
                const MRMeshTopology *top = mrMeshTopology(res.mesh);

                // Triangulation gives all face records — we must skip invalid ones using ValidFaces
                const MRFaceBitSet *validF = mrMeshTopologyGetValidFaces(top);
                size_t faceMax = mrMeshTopologyFaceSize(top);
                // NOTE: mrMeshTopologyGetTriangulation returns a vector-like struct
                MRTriangulation *tri = mrMeshTopologyGetTriangulation(top);

                float *newPositions = malloc(sizeof(float) * 3 * numPts);
                float *newNormals   = malloc(sizeof(float) * 3 * numPts);
                float *newST        = malloc(sizeof(float) * 2 * numPts);
                byte  *newColors    = malloc(sizeof(byte)  * 4 * numPts);

                for (size_t v = 0; v < numPts; v++) {
                    newPositions[v*3+0] = outPts[v].x;
                    newPositions[v*3+1] = outPts[v].y;
                    newPositions[v*3+2] = outPts[v].z;

                    // Recalculate normals from MeshLib (clean at cut seams)
                    MRVertId vid = {.id = (int)v};
                    MRVector3f n = mrMeshNormalFromVert(res.mesh, vid);
                    newNormals[v*3+0] = n.x;
                    newNormals[v*3+1] = n.y;
                    newNormals[v*3+2] = n.z;

                    // Interpolated texture ST
                    newST[v*2+0] = (outAttrs.uvCoords && v < outAttrs.numUvs) ? outAttrs.uvCoords[v].x : 0.0f;
                    newST[v*2+1] = (outAttrs.uvCoords && v < outAttrs.numUvs) ? outAttrs.uvCoords[v].y : 0.0f;

### 3. `q3map/csg_mesh.c`

Implement `PerformMeshCSG`. We use a custom attributed winding structure `awinding_t` to perform pure C intersection clipping of `misc_model` triangles against the planes of `func_snip` brushes.

- Convert each mesh triangle into an `awinding_t` (which carries positions, normals, STs, and colors).
- Sequentially clip against every plane of a brush using a custom `ClipAWindingEpsilon` (similar to Quake 3's built-in `ClipWindingEpsilon`). We keep the `SIDE_BACK` (inside) polygons.
- Do this for every brush in a `func_snip`.
- Triangulate the surviving convex polygons (windings) via a simple fan triangulation.
- Rebuild the `miscModelMesh_t` from the triangulated result.
- Flag the mesh with `wasCut = qtrue` to trigger `xatlas` UV regeneration in `IntegrateTriangleModels`.

Because we use pure C winding intersection, we eliminate the need for `MeshLib` C++ dependencies and robustly retain per-vertex normals, UVs, and colors.
                    // Interpolated vertex color
                    if (outAttrs.vertColors && v < outAttrs.numColors) {
                        newColors[v*4+0] = outAttrs.vertColors[v].r;
                        newColors[v*4+1] = outAttrs.vertColors[v].g;
                        newColors[v*4+2] = outAttrs.vertColors[v].b;
                        newColors[v*4+3] = outAttrs.vertColors[v].a;
                    } else {
                        newColors[v*4+0] = newColors[v*4+1] = newColors[v*4+2] = newColors[v*4+3] = 255;
                    }
                }

                int numNewIndices = 0;
                for (int f = 0; f < (int)faceMax; f++) {
                    if (!mrBitSetTest((const MRBitSet*)validF, (size_t)f)) continue;
                    // tri->data[f] is MRThreeVertIds = MRVertId[3]
                    newIndices[numNewIndices++] = tri->data[f][0].id;
                    newIndices[numNewIndices++] = tri->data[f][1].id;
                    newIndices[numNewIndices++] = tri->data[f][2].id;
                }

                // Recompute AABB on new positions
                ClearBounds(mm->mins, mm->maxs);
                for (size_t v = 0; v < numPts; v++) {
                    vec3_t wp = {newPositions[v*3+0], newPositions[v*3+1], newPositions[v*3+2]};
                    AddPointToBounds(wp, mm->mins, mm->maxs);
                }

                // Replace mm data
                free(mm->positions); mm->positions = newPositions;
                free(mm->normals);   mm->normals   = newNormals;
                free(mm->st);        mm->st        = newST;
                free(mm->colors);    mm->colors    = newColors;
                free(mm->indices);   mm->indices   = newIndices;
                mm->numVerts   = (int)numPts;
                mm->numIndices = numNewIndices;
                mm->wasCut     = qtrue; // forces IntegrateTriangleModels to regenerate xatlas from scratch
                mm->hasOriginalUVs = qfalse;

                // Free MeshLib outputs
                mrMeshFree(res.mesh);
                if (outAttrs.uvCoords)   free(outAttrs.uvCoords);
                if (outAttrs.vertColors) free(outAttrs.vertColors);
            }
        }
    }
}
```

### Step 4. BSP Hook for Phase 2 (`bsp.c`)
```c
int startInst, endInst;
LoadTriangleModels(e, &startInst, &endInst);
PerformMeshCSG(startInst, endInst);      // no-op if no func_snip operators
IntegrateTriangleModels(startInst, endInst, e);
```

### Step 5. `AddTriangleModels(tree)` — Unchanged
`AddTriangleModels` only stats and inserts pre-built `drawSurfs` into the BSP tree. It loops over **all** `numModelInstances` (the global counter) which is correct because at the point it is called, `LoadTriangleModels` and `IntegrateTriangleModels` have already fully populated `drawSurfs[]`. No change needed.

---

## Files Modified / Created

| File | Action | Notes |
|------|--------|-------|
| `q3map/qbsp.h` | MODIFY | Add `miscModelMesh_t` struct, `numMeshes`/`meshes[]` fields to `modelInstance_t`, update `LoadTriangleModels` signature |
| `q3map/misc_model.c` | MODIFY | Refactor `LoadTriangleModels`, add `IntegrateTriangleModels`, new `xatlas` wrappers |
| `q3map/bsp.c` | MODIFY | Update call sites in `ProcessWorldModel` and `ProcessSubModel` |
| `q3map/map.c` | MODIFY (Phase 2) | Add `func_snip` case in `ProcessMapEntities` |
| `q3map/csg_mesh.c` | NEW (Phase 2) | `BrushToMRMesh`, `StoreFuncSnipOperator`, `PerformMeshCSG` |
| `q3map/csg_mesh.h` | NEW (Phase 2) | Public declarations |
