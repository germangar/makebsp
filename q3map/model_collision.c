/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2024-2026 Antigravity Implementation

This file is part of Quake III Arena source code.
===========================================================================
*/

#include "../common/cmdlib.h"
#include "../libs/coacd_api.h"
#include "qbsp.h"

#define WRITE_COLLISION_MAP qtrue

#define DIRECT_AXIAL_BRUSH_SIZE 32 // An enclosed trisoup with axial planes becomes a brush directly

#define MAX_FUNC_CLIPS 16       // Max number of func_static groups
#define MAX_CLIP_ENTITY_GROUPS                                                 \
  MAX_MODEL_INSTANCES // Max number of clip entity groups
typedef struct {
  entity_t *entity;
  float brush_density;
  vec3_t mins, maxs;
  int numBrushes;
} clip_entity_group_t;

clip_entity_group_t clip_entity_groups[MAX_CLIP_ENTITY_GROUPS];
int num_clip_entity_groups = 0;

int c_degenerate_triangles = 0;
int c_degenerate_hulls = 0;


/*
====================
BrushFromMesh

Converts a convex hull (triangle soup) into a bspbrush_t.
Deduplicates coplanar faces by finding unique plane indices.
====================
*/
bspbrush_t *BrushFromMesh(CoACD_Mesh *mesh, shaderInfo_t *si) {
  int i, j;
  int numUniquePlanes = 0;
  int uniquePlanes[MAX_BRUSH_SIDES];
  float maxPlaneArea[MAX_BRUSH_SIDES];
  vec3_t trianglePoints[MAX_BRUSH_SIDES][3];
  bspbrush_t *b;

  for (i = 0; i < MAX_BRUSH_SIDES; i++) {
    maxPlaneArea[i] = -1.0f;
  }

  // For each triangle, find its unique plane
  for (i = 0; i < mesh->triangles_count; i++) {
    int idx0 = mesh->triangles_ptr[i * 3 + 0];
    int idx1 = mesh->triangles_ptr[i * 3 + 1];
    int idx2 = mesh->triangles_ptr[i * 3 + 2];

    vec3_t p0, p1, p2;
    p0[0] = mesh->vertices_ptr[idx0 * 3 + 0];
    p0[1] = mesh->vertices_ptr[idx0 * 3 + 1];
    p0[2] = mesh->vertices_ptr[idx0 * 3 + 2];
    p1[0] = mesh->vertices_ptr[idx1 * 3 + 0];
    p1[1] = mesh->vertices_ptr[idx1 * 3 + 1];
    p1[2] = mesh->vertices_ptr[idx1 * 3 + 2];
    p2[0] = mesh->vertices_ptr[idx2 * 3 + 0];
    p2[1] = mesh->vertices_ptr[idx2 * 3 + 1];
    p2[2] = mesh->vertices_ptr[idx2 * 3 + 2];

    // Check for degenerate triangle (near zero area)
    vec3_t t1, t2, cross;
    VectorSubtract(p1, p0, t1);
    VectorSubtract(p2, p0, t2);
    CrossProduct(t1, t2, cross);
    float area = VectorLength(cross);
    if (area < 0.1) {
      c_degenerate_triangles++;
      continue;
    }

    // MapPlaneFromPoints computes normal pointing OUT for CW triangles.
    // If our mesh is CCW (standard), feeding MapPlaneFromPoints(p0, p2, p1)
    // gives the correct outgoing normal.
    int planenum = MapPlaneFromPoints(p0, p2, p1);
    if (planenum == -1) {
      c_degenerate_triangles++;
      continue;
    }

    // Deduplicate against existing planes in this brush
    for (j = 0; j < numUniquePlanes; j++) {
      if (uniquePlanes[j] == planenum) {
        // If this triangle is significantly larger, use it for the .map points
        if (area > maxPlaneArea[j]) {
          maxPlaneArea[j] = area;
          VectorCopy(p0, trianglePoints[j][0]);
          VectorCopy(p1, trianglePoints[j][1]);
          VectorCopy(p2, trianglePoints[j][2]);
        }
        break;
      }
    }

    if (j == numUniquePlanes) {
      if (numUniquePlanes >= MAX_BRUSH_SIDES) {
        _printf("WARNING: BrushFromMesh reached MAX_BRUSH_SIDES\n");
        break;
      }
      uniquePlanes[numUniquePlanes] = planenum;
      maxPlaneArea[numUniquePlanes] = area;
      // Capture CCW triangle points (p0, p1, p2)
      VectorCopy(p0, trianglePoints[numUniquePlanes][0]);
      VectorCopy(p1, trianglePoints[numUniquePlanes][1]);
      VectorCopy(p2, trianglePoints[numUniquePlanes][2]);
      numUniquePlanes++;
    }
  }

  if (numUniquePlanes < 4) {
    return NULL;
  }

  b = AllocBrush(numUniquePlanes);
  b->numsides = numUniquePlanes;
  b->detail = qtrue;
  b->contents = si->contents;
  b->contentShader = si;

  for (i = 0; i < numUniquePlanes; i++) {
    b->sides[i].planenum = uniquePlanes[i];
    b->sides[i].shaderInfo = si;

    // Create a 3-point winding for WriteBspBrushMap.
    // Storing them as original CCW (p0, p1, p2).
    // WriteBspBrushMap will flip them to CW (0, 2, 1).
    b->sides[i].winding = AllocWinding(3);
    b->sides[i].winding->numpoints = 3;
    VectorCopy(trianglePoints[i][0], b->sides[i].winding->p[0]);
    VectorCopy(trianglePoints[i][1], b->sides[i].winding->p[1]);
    VectorCopy(trianglePoints[i][2], b->sides[i].winding->p[2]);
  }

  // Calculate bounds from the 3-point windings
  if (!BoundBrush(b)) {
    c_degenerate_hulls++;
    FreeBrush(b);
    return NULL;
  }

  return b;
}

/*
====================
CategorizeModel

Reserved for future entity-specific classification for CoACD.
====================
*/
static void CategorizeModel(modelInstance_t *inst) {
  // Shell for future entity-based classification (e.g., checking epairs)
}

/*
====================
DecomposeModelCollision

Aggregates solid geometry for one instance, runs CoACD, and converts results to brushes.
====================
*/
static void DecomposeModelCollision(modelInstance_t *inst) {
  int j, k;
  mapDrawSurface_t *ds;
  int totalVerts = 0;
  int totalIndexes = 0;
  double *allVerts;
  int *allIndexes;
  int currentVert = 0;
  int currentIndex = 0;
  bspbrush_t *hulls_list = NULL;
  int numHulls = 0;

  if (num_clip_entity_groups >= MAX_CLIP_ENTITY_GROUPS) {
    _printf("WARNING: MAX_CLIP_ENTITY_GROUPS reached\n");
    return;
  }

  // Reset per-model degenerate counters
  c_degenerate_triangles = 0;
  c_degenerate_hulls = 0;

  // Step 1: Count solid vertices and indices for this instance
  for (j = 0; j < inst->numDrawSurfs; j++) {
    ds = inst->drawSurfs[j];
    if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID)) {
      continue;
    }
    totalVerts += ds->numVerts;
    totalIndexes += ds->numIndexes;
  }

  if (totalVerts == 0) {
    return;
  }

  _printf("Instance %s: Aggregating %i vertices and %i indices...\n",
          inst->modelName, totalVerts, totalIndexes);

  // Step 2: Allocate and fill buffers
  allVerts = malloc(totalVerts * 3 * sizeof(double));
  allIndexes = malloc(totalIndexes * sizeof(int));

  for (j = 0; j < inst->numDrawSurfs; j++) {
    ds = inst->drawSurfs[j];
    if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID)) {
      continue;
    }

    int startVert = currentVert;
    for (k = 0; k < ds->numVerts; k++) {
      allVerts[currentVert * 3 + 0] = ds->verts[k].xyz[0];
      allVerts[currentVert * 3 + 1] = ds->verts[k].xyz[1];
      allVerts[currentVert * 3 + 2] = ds->verts[k].xyz[2];
      currentVert++;
    }

    for (k = 0; k < ds->numIndexes; k++) {
      allIndexes[currentIndex++] = startVert + ds->indexes[k];
    }
  }

  // Step 3: Run CoACD for this instance
  CoACD_Mesh input;
  input.vertices_ptr = allVerts;
  input.vertices_count = totalVerts;
  input.triangles_ptr = allIndexes;
  input.triangles_count = totalIndexes / 3;

  _printf("Running CoACD decomposition for instance %s...\n", inst->modelName);
  fflush(stdout);

  CoACD_MeshArray hulls =
      CoACD_run(&input,
                0.2, // threshold
                -1,  // max_convex_hull
                COACD_PREPROCESS_AUTO,
                50,                      // prep_resolution
                2000,                    // sample_resolution
                20,                      // mcts_nodes
                100,                     // mcts_iteration
                3,                       // mcts_max_depth
                false,                   // pca
                true,                    // merge
                true,                    // decimate
                (int)(totalVerts * 0.9), // max_ch_vertex
                false,                   // extrude
                0.01,                    // extrude_margin
                COACD_APX_CH,            // apx_mode
                1234                     // seed
      );

  _printf("CoACD generated %i convex hulls.\n", (int)hulls.meshes_count);

  // Step 4: Convert hulls to bspbrushes
  shaderInfo_t *caulk = ShaderInfoForShader("textures/common/caulk");
  for (j = 0; j < (int)hulls.meshes_count; j++) {
    bspbrush_t *b = BrushFromMesh(&hulls.meshes_ptr[j], caulk);
    if (b) {
      b->next = hulls_list;
      hulls_list = b;
      numHulls++;
    }
  }

  // Step 5: Populate clip entity group
  if (hulls_list) {
    clip_entity_group_t *group = &clip_entity_groups[num_clip_entity_groups++];

    // Create a local entity (not part of the map entities yet)
    entity_t *ent = malloc(sizeof(entity_t));
    memset(ent, 0, sizeof(entity_t));
    ent->epairs = malloc(sizeof(epair_t));
    memset(ent->epairs, 0, sizeof(epair_t));
    ent->epairs->key = strdup("classname");
    ent->epairs->value = strdup("func_static");
    ent->brushes = hulls_list;

    group->entity = ent;
    group->numBrushes = numHulls;

    // Calculate bounds
    ClearBounds(group->mins, group->maxs);
    for (bspbrush_t *b = hulls_list; b; b = b->next) {
      AddPointToBounds(b->mins, group->mins, group->maxs);
      AddPointToBounds(b->maxs, group->mins, group->maxs);
    }

    // Calculate brush density
    vec3_t size;
    VectorSubtract(group->maxs, group->mins, size);
    float volume = size[0] * size[1] * size[2];
    if (volume > 1.0f) {
      group->brush_density = (float)numHulls / volume;
    } else {
      group->brush_density = 0;
    }

    _printf("Instance %s: Created clip group with %i brushes, density %e\n",
            inst->modelName, numHulls, group->brush_density);
  }

  // Step 6: Per-model reporting
  if (c_degenerate_triangles > 0 || c_degenerate_hulls > 0) {
    _printf("Instance %s: Degenerate geometry skipped: %i triangles, %i hulls\n",
            inst->modelName, c_degenerate_triangles, c_degenerate_hulls);
  }

  // Cleanup
  CoACD_freeMeshArray(hulls);
  free(allVerts);
  free(allIndexes);
}

/*
==================
WriteCollisionMap
==================
*/
static void WriteCollisionMap(const char *name) {
  FILE *f;
  int i, j;
  side_t *s;
  winding_t *w;
  clip_entity_group_t *group;

  _printf("Writing diagnostic map: %s\n", name);
  f = fopen(name, "wb");
  if (!f) {
    Error("Can't write %s", name);
  }

  // Worldspawn (empty or placeholder)
  fprintf(f, "{\n\"classname\" \"worldspawn\"\n}\n");

  for (i = 0; i < num_clip_entity_groups; i++) {
    group = &clip_entity_groups[i];
    if (!group->entity || !group->entity->brushes) {
      continue;
    }

    fprintf(f, "{\n");
    // Write entity epairs
    for (epair_t *ep = group->entity->epairs; ep; ep = ep->next) {
      if (ep->key && ep->value) {
        fprintf(f, "\"%s\" \"%s\"\n", ep->key, ep->value);
      }
    }

    // Write entity brushes
    for (bspbrush_t *b = group->entity->brushes; b; b = b->next) {
      fprintf(f, "{\n");
      for (j = 0; j < b->numsides; j++) {
        s = &b->sides[j];
        w = s->winding;
        if (!w || w->numpoints < 3) {
          continue;
        }

        // Write points in CW order (0, 2, 1) to define plane pointing OUT
        fprintf(f, "( %.3f %.3f %.3f ) ", w->p[0][0], w->p[0][1], w->p[0][2]);
        fprintf(f, "( %.3f %.3f %.3f ) ", w->p[2][0], w->p[2][1], w->p[2][2]);
        fprintf(f, "( %.3f %.3f %.3f ) ", w->p[1][0], w->p[1][1], w->p[1][2]);

        const char *shader = "textures/common/caulk";
        if (s->shaderInfo) {
          shader = s->shaderInfo->shader;
        }
        if (!Q_strncasecmp(shader, "textures/", 9)) {
          shader += 9;
        }
        fprintf(f, "%s 0 0 0 1 1\n", shader);
      }
      fprintf(f, "}\n");
    }
    fprintf(f, "}\n");
  }

  fclose(f);
}

/*
====================
CreateTriangleModelCollision

Generates collision brushes from model geometry using CoACD (per-instance pass).
====================
*/
void CreateTriangleModelCollision(void) {
  int i;
  modelInstance_t *inst;
  qboolean hasSolid = qfalse;

  _printf("----- CreateTriangleModelCollision -----\n");

  // Step 1: Quick check for any solid geometry (optimization)
  for (i = 0; i < numModelInstances; i++) {
    inst = &modelInstances[i];
    for (int j = 0; j < inst->numDrawSurfs; j++) {
      if (inst->drawSurfs[j]->shaderInfo &&
          (inst->drawSurfs[j]->shaderInfo->contents & CONTENTS_SOLID)) {
        hasSolid = qtrue;
        break;
      }
    }
    if (hasSolid)
      break;
  }

  if (!hasSolid) {
    _printf("No solid model geometry found for collision.\n");
    return;
  }

  // Reset groups
  num_clip_entity_groups = 0;

  // Step 2: Decomposition and Categorization Pass (per instance)
  for (i = 0; i < numModelInstances; i++) {
    inst = &modelInstances[i];
    CategorizeModel(inst);
    DecomposeModelCollision(inst);
  }

  if (WRITE_COLLISION_MAP) {
    char debugName[1024];
    sprintf(debugName, "%s_collision.map", source);
    WriteCollisionMap(debugName);
  }
}
