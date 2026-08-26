#ifndef QBSP_H
#define QBSP_H

/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "../common/bspfile.h"
#include "../common/cmdlib.h"
#include "../common/mathlib.h"
#include "../common/polylib.h"
#include "../common/scriplib.h"
#include "../common/threads.h"
#include "../shared/globals.h"
#include "../shared/mesh.h"
#include "../shared/shaders.h"

extern qboolean g_fast;

// Currently active engine limits
#define MAX_SURFACE_VERTS (game->maxSurfaceVerts)
#define MAX_SURFACE_INDEXES (game->maxSurfaceIndexes)

#define MAX_MAP_BRUSHES_LIMIT MAX_MAP_BRUSHES

#define MAX_PATCH_SIZE 32

#define CLIP_EPSILON 0.1
#define PLANENUM_LEAF -1

#define HINT_PRIORITY 1000

typedef struct parseMesh_s
{
    struct parseMesh_s *next;
    mesh_t mesh;
    shaderInfo_t *shaderInfo;

    qboolean grouped; // used during shared edge grouping
    struct parseMesh_s *groupChain;
    
    int entitynum; // original entity index for error diagnostics and collision grouping
    epair_t *epairs; // Decoupled local properties
} parseMesh_t;

typedef struct bspface_s
{
    struct bspface_s *next;
    int planenum;
    int priority; // added to value calculation
    qboolean checked;
    qboolean hint;
    winding_t *w;
} bspface_t;

typedef struct plane_s
{
    vec3_t normal;
    vec_t dist;
    int type;
    struct plane_s *hash_chain;
} plane_t;

typedef struct side_s
{
    int planenum;

    float texMat[2][3]; // brush primitive texture matrix
    // for old brush coordinates mode
    float vecs[2][4]; // texture coordinate mapping

    winding_t *winding;
    winding_t *visibleHull; // convex hull of all visible fragments

    struct shaderInfo_s *shaderInfo;

    int contents;     // from shaderInfo
    int surfaceFlags; // from shaderInfo
    int value;        // from shaderInfo

    qboolean visible;  // choose visble planes first
    qboolean bevel;    // don't ever use for bsp splitting, and don't bother
                       // making windings for it
    qboolean backSide; // generated side for a q3map_backShader

    int surfaceNum; // the index of the dsurface_t this side generated
    int outputNum;  // the index of the dbrushside_t this side generated
} side_t;

#define MAX_BRUSH_SIDES 1024

typedef struct bspbrush_s
{
    struct bspbrush_s *next;

    int entitynum; // original entity index for error diagnostics and collision grouping
    int brushnum;  // editor numbering
    epair_t *epairs; // Decoupled local properties

    struct shaderInfo_s *contentShader;

    int contents;
    qboolean detail;
    qboolean opaque;
    int outputNumber; // set when the brush is written to the file list

    int portalareas[2];

    struct bspbrush_s
        *original; // chopped up brushes will reference the originals

    vec3_t mins, maxs;
    int numsides;
    side_t sides[6]; // variably sized
} bspbrush_t;

typedef struct drawsurf_s
{
    shaderInfo_t *shaderInfo;

    bspbrush_t *mapBrush; // not valid for patches
    side_t *side;         // not valid for patches

    struct drawsurf_s *nextOnShader; // when sorting by shader for lightmaps

    int fogNum; // set by FogDrawSurfs

    int lightmapNum; // -1 = no lightmap
    int lightmapX, lightmapY;
    int lightmapWidth, lightmapHeight;

    int numVerts;
    drawVert_t *verts;

    int numIndexes;
    int *indexes;
    int numParentIndexes;

    // for faces only
    int planeNum;

    vec3_t lightmapOrigin;  // also used for flares
    vec3_t lightmapVecs[3]; // also used for flares

    // for patches only
    qboolean patch;
    int patchWidth;
    int patchHeight;

    // for misc_models only
    qboolean miscModel;
    qboolean planarDerived; // true for atomic trisoups merged from planar surfaces (e.g. chamfers)
    qboolean isPlanar;      // true if this trisoup was derived from flat coplanar surface(s)
    qboolean patchDerived;  // true if tessellated from a Bezier patch (has indexed triangles, no side/mapBrush)

    int entityNum; // -1 = worldspawn; set for patches derived from named entities

    qboolean flareSurface;
    float samplesize;
    float lightmapScale;
    float smoothingRadius;
    float lightValue;
    vec3_t lightColor;
    float backsplashFraction;
    float lightSubdivide;
    int overrideVertexColor;
    vec3_t vertexColor;
    int overrideVertexAlpha;
    float vertexAlpha;
    float superSampleRadius;
    qboolean isHalo;
    int upscale;
    int enforceSampleSize;
    float cutoff;
    float fadeout;
    qboolean hasAttenuationOverride;
    int attenuationModel;
    int noDeluxeInfluence;
    int noDeluxeInfluenceBacksplash;
    float gridAmbientScale;
    float gridDirectScale;
    int castShadows;
    qboolean isDecal;      // NEW: surface generated by _decal projection
    char decalgroup[128];   // group string for _decal projection filtering
    int parentSurfaceNum;  // -1 = none, otherwise index of parent surface (e.g. chamfer strips)
    float chamferConvexWidth;
    float chamferConcaveWidth;
    char smoothgroup[32];
} mapDrawSurface_t;

#define MAX_CHAMFER_VERTS 256

typedef struct surfaceNeighbor_s {
    int neighborSurfaceNum;
    int sharedChainIndicesA[MAX_CHAMFER_VERTS];
    int sharedChainIndicesB[MAX_CHAMFER_VERTS];
    int sharedChainLen;
    qboolean isConcave;
    struct surfaceNeighbor_s *next;
} surfaceNeighbor_t;

typedef struct surfaceChamferEdge_s {
    int   chainIndices[MAX_CHAMFER_VERTS];
    int   chainLen;
    float width;
} surfaceChamferEdge_t;

extern surfaceNeighbor_t **surfaceNeighbors;
extern qboolean chamfernosubdivide;

void BuildSurfaceAdjacencyGraph(entity_t *e);
void ChamferSurfaceEdges(entity_t *e);
void MergeChamferStripsIntoParents(entity_t *e);
void MergeParentedTrisoups(entity_t *e);
void MergeAdjacentTrisoups(entity_t *e);
void CleanupAllTrisoups(entity_t *e);
void DecimateAllTrisoups(entity_t *e, qboolean onlyPlanar);
void GenerateTrisoupUVs(entity_t *e);

#ifdef DECIMATE_PLANAR_WITH_MESHLIB
qboolean DecimateSurfaceWithMeshLib(mapDrawSurface_t *ds);
#endif
typedef struct drawSurfRef_s
{
    struct drawSurfRef_s *nextRef;
    int outputNumber;
} drawSurfRef_t;

typedef struct node_s
{
    // both leafs and nodes
    int planenum; // -1 = leaf node
    struct node_s *parent;
    vec3_t mins, maxs;  // valid after portalization
    bspbrush_t *volume; // one for each leaf/node

    // nodes only
    side_t *side; // the side that created the node
    struct node_s *children[2];
    qboolean hint;
    int tinyportals;
    vec3_t referencepoint;

    // leafs only
    qboolean opaque; // view can never be inside
    qboolean areaportal;
    int cluster;                       // for portalfile writing
    int area;                          // for areaportals
    bspbrush_t *brushlist;             // fragments of all brushes in this leaf
    drawSurfRef_t *drawSurfReferences; // references to patches pushed down

    int occupied;       // 1 or greater can reach entity
    entity_t *occupant; // for leak file testing

    struct portal_s *portals; // also on nodes during construction
} node_t;

typedef struct portal_s
{
    plane_t plane;
    node_t *onnode;   // NULL = outside box
    node_t *nodes[2]; // [0] = front side of plane
    struct portal_s *next[2];
    winding_t *winding;

    qboolean sidefound; // false if ->side hasn't been checked
    qboolean hint;
    side_t *side; // NULL = non-visible
} portal_t;

typedef struct
{
    node_t *headnode;
    node_t outside_node;
    vec3_t mins, maxs;
} tree_t;

extern int entity_num;

extern qboolean noprune;
extern qboolean nodetail;
extern qboolean fulldetail;
extern qboolean nowater;
extern qboolean noCurveBrushes;
extern qboolean fakemap;
extern qboolean coplanar;
extern qboolean nofog;
extern qboolean testExpand;
extern qboolean showseams;

extern qboolean guessUVs;

extern vec_t microvolume;

extern char outbase[32];
extern char source[1024];

extern int novertexlighting;
extern int nogridlighting;

//=============================================================================

// brush.c

int CountBrushList(bspbrush_t *brushes);
bspbrush_t *AllocBrush(int numsides);
void FreeBrush(bspbrush_t *brushes);
void FreeBrushList(bspbrush_t *brushes);
bspbrush_t *CopyBrush(bspbrush_t *brush);

void FreeParseMesh(parseMesh_t *pm);

epair_t *CopyEpairs(epair_t *e);
void FreeEpairs(epair_t *e);


void PrintBrush(bspbrush_t *brush);
qboolean BoundBrush(bspbrush_t *brush);
qboolean CreateBrushWindings(bspbrush_t *brush);
bspbrush_t *BrushFromBounds(vec3_t mins, vec3_t maxs);
vec_t BrushVolume(bspbrush_t *brush);
void WriteBspBrushMap(char *name, bspbrush_t *list);
bspbrush_t *AddBevelsToBrush(bspbrush_t *b);
void SnapVector(vec3_t normal);
qboolean PlaneEqual(plane_t *p, vec3_t normal, vec_t dist);

void ExportModels(int count, char **fileNames);

void FilterDetailBrushesIntoTree(entity_t *e, tree_t *tree);
void FilterStructuralBrushesIntoTree(entity_t *e, tree_t *tree);

//=============================================================================

// map.c

extern int entitySourceBrushes;

// mapplanes[ num^1 ] will always be the mirror or mapplanes[ num ]
// nummapplanes will always be even
extern plane_t *mapplanes;
extern int nummapplanes;

extern vec3_t map_mins, map_maxs;

extern char (*mapIndexedShaders)[MAX_QPATH];
extern int numMapIndexedShaders;

extern entity_t *mapent;

#define MAX_BUILD_SIDES 300
extern bspbrush_t *buildBrush;

void LoadMapFile(char *filename);
int MapPlaneFromPoints(vec3_t p0, vec3_t p1, vec3_t p2);
int FindFloatPlane(vec3_t normal, vec_t dist);
int PlaneTypeForNormal(vec3_t normal);
bspbrush_t *FinishBrush(void);
mapDrawSurface_t *AllocDrawSurf(void);
float SnapToNearestPowerOfTwo(float value);
mapDrawSurface_t *DrawSurfaceForSide(bspbrush_t *b, side_t *s, winding_t *w);
void MoveBrushesToWorld(entity_t *mapent);
void SpawnLightEntity(vec3_t origin, vec3_t normal, qboolean isPoint, entity_t *sourceEnt, const char *shaderName);

//=============================================================================



// csg

bspbrush_t *MakeBspBrushList(bspbrush_t *brushes, vec3_t clipmins,
                             vec3_t clipmaxs);

//=============================================================================

// brushbsp

#define PSIDE_FRONT 1
#define PSIDE_BACK 2
#define PSIDE_BOTH (PSIDE_FRONT | PSIDE_BACK)
#define PSIDE_FACING 4

int BoxOnPlaneSide(vec3_t mins, vec3_t maxs, plane_t *plane);
qboolean WindingIsTiny(winding_t *w);

void SplitBrush(bspbrush_t *brush, int planenum, bspbrush_t **front,
                bspbrush_t **back);

tree_t *AllocTree(void);
node_t *AllocNode(void);

tree_t *BrushBSP(bspbrush_t *brushlist, vec3_t mins, vec3_t maxs);

//=============================================================================

// portals.c

void MakeHeadnodePortals(tree_t *tree);
void MakeNodePortal(node_t *node);
void SplitNodePortals(node_t *node);

qboolean Portal_Passable(portal_t *p);

qboolean FloodEntities(tree_t *tree);
void FillOutside(node_t *headnode);
void FloodAreas(tree_t *tree);
bspface_t *VisibleFaces(entity_t *e, tree_t *tree);
void FreePortal(portal_t *p);

void MakeTreePortals(tree_t *tree);

//=============================================================================


//=============================================================================

// leakfile.c

void LeakFile(tree_t *tree);

//=============================================================================

// prtfile.c

void NumberClusters(tree_t *tree);
void WritePortalFile(tree_t *tree);

//=============================================================================

// writebsp.c

void SetModelNumbers(void);
void SetLightStyles(void);

int EmitShader(const char *shader);

void BeginBSPFile(void);
void EndBSPFile(void);

void EmitBrushes(bspbrush_t *brushes);

void BeginModel(void);
void EndModel(node_t *headnode);

//=============================================================================

// tree.c

void FreeTree(tree_t *tree);
void FreeTree_r(node_t *node);
void PrintTree_r(node_t *node, int depth);
void FreeTreePortals_r(node_t *node);

//=============================================================================

// patch.c

extern int numMapPatches;

mapDrawSurface_t *DrawSurfaceForMesh(mesh_t *m);
void ParsePatch(void);
mesh_t *SubdivideMesh(mesh_t in, float maxError, float minLength);
void PatchMapDrawSurfs(entity_t *e);

//=============================================================================

// lightmap.c

extern int numLightBytes;
extern byte *lightBytes;
extern int numLightmaps;

void AllocateLightmaps(entity_t *e);
void AllocateLightmapForPatch(mapDrawSurface_t *ds);
void FreeLightmaps(void);

//=============================================================================

// tjunction.c

void FixTJunctions(entity_t *e);
void ChopTjunctions(entity_t *e);

//=============================================================================

// fog.c

void FogDrawSurfs(void);
winding_t *WindingFromDrawSurf(mapDrawSurface_t *ds);

//=============================================================================

// facebsp.c

bspface_t *BspFaceForPortal(portal_t *p);
bspface_t *MakeStructuralBspFaceList(bspbrush_t *list);
bspface_t *MakeVisibleBspFaceList(bspbrush_t *list);
tree_t *FaceBSP(bspface_t *list);

//=============================================================================

// misc_model.c

typedef enum
{
    MC_NONE,
    MC_OBJECT,
    MC_WRAP,
    MC_TERRAIN,
    MC_EXTRUDE,
    MC_OBJECTDETAIL,   // user-forced only; dual-mode extrude (solid) + object (playerclip)
    MC_WRAPDETAIL      // user-forced only; dual-mode extrude (solid) + wrap (playerclip)
} modelCategory_t;

#define MAX_MODEL_COLLISION_MESHES 256
#define MAX_MISC_MODEL_MESHES 256

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

typedef struct modelInstance_s
{
    char modelName[MAX_QPATH];
    int numDrawSurfs;
    mapDrawSurface_t **drawSurfs; // References to surfaces created for this instance
    entity_t *creator;            // Reference to the entity that created this instance
    float lightmapScale;
    float triangle_density;
    modelCategory_t category;
    
    qboolean has_collision_type_override;
    modelCategory_t collision_type_override;

    int maxtriangles; // budget for entire instance across all meshes

    int num_collision_meshes;
    struct colMesh_s *collision_meshes[MAX_MODEL_COLLISION_MESHES]; // Extracted, healed and decimated collision meshes

    int numMeshes;
    miscModelMesh_t *meshes[MAX_MISC_MODEL_MESHES]; // intermediate load data
} modelInstance_t;

extern int c_triangleModels;
extern int c_triangleSurfaces;
extern int c_triangleVertexes;
extern int c_triangleIndexes;

void LoadTriangleModels(entity_t *eparent, int *outStartInst, int *outEndInst);
void PerformMeshCSG(int startInst, int endInst);
void PerformMeshDecimation(int startInst, int endInst);
void FreeFuncTrimOperators(void);
void IntegrateTriangleModels(int startInst, int endInst, entity_t *eparent);
void AddTriangleModels(tree_t *tree);

#define MAX_MODEL_INSTANCES 1024
extern modelInstance_t modelInstances[MAX_MODEL_INSTANCES];
extern int numModelInstances;

//=============================================================================

// model_collision.c

void CreateTriangleModelCollision(entity_t *parent);
void CreateCollisionTris(modelInstance_t *inst);

bspbrush_t *GenerateExtrusionCollision(modelInstance_t *inst, shaderInfo_t *shader);
bspbrush_t *GenerateHACDCollision(modelInstance_t *inst, shaderInfo_t *shader);

int CSGMergeBrushList(bspbrush_t **pList);
const char *CategoryString(modelCategory_t cat);

//=============================================================================

// surface.c

extern mapDrawSurface_t *mapDrawSurfs;
extern int numMapDrawSurfs;
extern extraSurface_t *drawExtraSurfaces;

mapDrawSurface_t *AllocDrawSurf(void);
void ResolveSurfaceExtraProperties(mapDrawSurface_t *ds, epair_t *epairs);
winding_t *WindingFromDrawSurf(mapDrawSurface_t *ds);
void MergeSides(entity_t *e, tree_t *tree);
void SubdivideDrawSurfs(entity_t *e, tree_t *tree);
void MakeDrawSurfaces(bspbrush_t *b);
void ClipSidesIntoTree(entity_t *e, tree_t *tree);
void FilterDrawsurfsIntoTree(entity_t *e, tree_t *tree);
void GenerateHalos(entity_t *e);

extern qboolean nodecimateplanar;

// tjunction.c
void FixTJunctions(entity_t *e);
qboolean IsEdgeSharingCandidate(mapDrawSurface_t *ds);
void InsertCollinearVertices(entity_t *e, float minDot, float maxDot, int targetEntityNum);
qboolean VectorsNearEqual(const vec3_t a, const vec3_t b, float epsilon);

// func_trisoup.c
void ProcessFuncTrisoup(entity_t *e);

//==============================================================================
// decals.c

#define MAX_DECAL_PROJECTORS 4096
typedef struct {
    vec3_t center;
    float radius;
    vec3_t mins, maxs;
    plane_t planes[MAX_POINTS_ON_WINDING + 2]; // 0=front, 1=back, 2..N=side planes
    int numPlanes;
    float texMat[2][4];  // S and T rows of the 3D->2D texture matrix
    shaderInfo_t *si;
    int decalEntityNum;
    char decalgroup[128];
    int overrideVertexColor;
    vec4_t vertexColor;
    int overrideVertexAlpha;
    float vertexAlpha;
} decalProjector_t;

extern int numDecalProjectors;
extern decalProjector_t decalProjectors[MAX_DECAL_PROJECTORS];

#define DECAL_MESH_INITIAL_VERTS   1024
#define DECAL_MESH_INITIAL_INDEXES 2048

typedef struct {
    int maxVerts;
    int numVerts;
    drawVert_t *verts;
    
    int maxIndexes;
    int numIndexes;
    int *indexes;
} decalMesh_t;

void InitDecalMesh(decalMesh_t *m);
void FreeDecalMesh(decalMesh_t *m);
void WeldDecalMesh(decalMesh_t *m, float epsilon);
void ExtrudeDecalMesh(decalMesh_t *m);

void ParseDecalProjectors(void);
void MakeEntityDecals(entity_t *e);

//==============================================================================

// brush_primit.c

#define BPRIMIT_UNDEFINED 0
#define BPRIMIT_OLDBRUSHES 1
#define BPRIMIT_NEWBRUSHES 2
extern int g_bBrushPrimit;

void ComputeAxisBase(vec3_t normal, vec3_t texX, vec3_t texY);

// autocaulk.c
extern qboolean noautocaulk;
void FilterDuplicateBrushes(void);
void AutoCaulkBrushes(void);

// convert_ktx.c
void ConvertKTX(const char *path);

#endif
