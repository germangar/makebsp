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
#include "qbsp.h"

/*

  Lightmap allocation has to be done after all flood filling and
  visible surface determination.

*/

int numSortShaders;
mapDrawSurface_t *surfsOnShader[MAX_MAP_SHADERS];

#define MAX_LIGHTMAPS 2048
#define MAX_LIGHTMAP_WIDTH 1024
int *lightmapHeights = NULL;

int numLightmaps = 0;
int c_exactLightmap;

void PrepareNewLightmap(void) {
  if (numLightmaps >= MAX_LIGHTMAPS) {
    Error("MAX_LIGHTMAPS exceeded");
  }
  // Explicitly clear the memory for the new lightmap's heightmap
  memset(&lightmapHeights[numLightmaps * MAX_LIGHTMAP_WIDTH], 0, sizeof(int) * MAX_LIGHTMAP_WIDTH);
  numLightmaps++;
}

/*
===============
AllocLMBlock

returns a texture number and the position inside it
===============
*/
qboolean AllocLMBlock(int lmIndex, int w, int h, int *x, int *y) {
  int i, j;
  int *allocated = &lightmapHeights[lmIndex * MAX_LIGHTMAP_WIDTH];
  int bestY;

  // Search for the first horizontal run where it fits vertically
  for (i = 0; i <= LIGHTMAP_WIDTH - w; i++) {
    bestY = 0;
    for (j = 0; j < w; j++) {
      if (allocated[i + j] > bestY) {
        bestY = allocated[i + j];
      }
      if (bestY + h > LIGHTMAP_HEIGHT) {
        break; // Doesn't fit in this run starting at 'i'
      }
    }
    
    if (j == w) { // Fits!
      *x = i;
      *y = bestY;
      
      // Update the heightmap
      for (j = 0; j < w; j++) {
        allocated[i + j] = bestY + h;
      }
      return qtrue;
    }
  }

  return qfalse;
}

/*
===================
AllocateLightmapForMiscModel
===================
*/
void AllocateLightmapForMiscModel(mapDrawSurface_t *ds) {
  int i, x, y, ssize;
  float min_s, max_s, min_t, max_t;
  double area3D = 0, areaUV = 0;
  float s, t, scale;
  int w, h;
  drawVert_t *v0, *v1, *v2;

  if (ds->numIndexes < 3)
    return;

  ssize = ds->samplesize;

  // 1. Initial UV bounds
  min_s = min_t = 1000000;
  max_s = max_t = -1000000;
  for (i = 0; i < ds->numVerts; i++) {
    s = ds->verts[i].lightmap[0][0];
    t = ds->verts[i].lightmap[0][1];
    if (s < min_s)
      min_s = s;
    if (s > max_s)
      max_s = s;
    if (t < min_t)
      min_t = t;
    if (t > max_t)
      max_t = t;
  }

  // 2. Area Calculation
  for (i = 0; i < ds->numIndexes; i += 3) {
    v0 = &ds->verts[ds->indexes[i]];
    v1 = &ds->verts[ds->indexes[i + 1]];
    v2 = &ds->verts[ds->indexes[i + 2]];

    // 3D Area (cross product)
    vec3_t side1, side2, cross;
    VectorSubtract(v1->xyz, v0->xyz, side1);
    VectorSubtract(v2->xyz, v0->xyz, side2);
    CrossProduct(side1, side2, cross);
    area3D += 0.5 * VectorLength(cross);

    // UV Area (2D cross product)
    areaUV +=
        0.5 *
        fabs((v1->lightmap[0][0] - v0->lightmap[0][0]) *
                 (v2->lightmap[0][1] - v0->lightmap[0][1]) -
             (v2->lightmap[0][0] - v0->lightmap[0][0]) *
                 (v1->lightmap[0][1] - v0->lightmap[0][1]));
  }

  if (areaUV < 0.0001) {
    _printf("WARNING: misc_model surface with degenerate UVs (areaUV: %f)\n",
            areaUV);
    return;
  }

  // 3. Scale Determination
  // Target density: 1/ssize^2 luxels per square unit.
  scale = sqrt((area3D / (ssize * ssize)) / areaUV);

  // Safeguard against extreme scaling
  if (scale < 0.01)
    scale = 0.01;

  // Enforce dynamic minimum lightmap area
  float uvWidth = max_s - min_s;
  float uvHeight = max_t - min_t;
  float uvArea = uvWidth * uvHeight;
  if (uvArea > 0.0001f) {
    float minDimension = 192.0f;
    float targetArea = minDimension * minDimension;
    float minScale = sqrt(targetArea / uvArea);
    if (scale < minScale) {
        scale = minScale;
    }
  }

  // Final quality knob adjustment
  scale *= ds->lightmapScale;

  // ==========================================================================
  // UV SNAPPING TO TEXEL GRID
  // ==========================================================================
  if (snapUVs) {
    int W = (int)floor((max_s - min_s) * scale + 0.5f);
    int H = (int)floor((max_t - min_t) * scale + 0.5f);
    
    // Safeguard against collapse
    if (W < 1) W = 1;
    if (H < 1) H = 1;

    // Use a mandatory 1-pixel gutter on all sides (total +2)
    // This allows exact integer snapping while respecting bilinear safety.
    int w_snapped = W + 2;
    int h_snapped = H + 2;

    // Check if the snapped block fits in the atlas
    if (w_snapped <= LIGHTMAP_WIDTH && h_snapped <= LIGHTMAP_HEIGHT) {

      // Allocate
      qboolean allocated_success = qfalse;
      for (i = 0; i < numLightmaps; i++) {
        if (AllocLMBlock(i, W + 2, H + 2, &x, &y)) {
          ds->lightmapNum = i;
          allocated_success = qtrue;
          break;
        }
      }

      if (!allocated_success) {
        PrepareNewLightmap();
        if (AllocLMBlock(numLightmaps - 1, W + 2, H + 2, &x, &y)) {
            ds->lightmapNum = numLightmaps - 1;
            allocated_success = qtrue;
        }
      }

      if (allocated_success) {
        ds->lightmapWidth = W;
        ds->lightmapHeight = H;
        ds->lightmapX = x + 1;
        ds->lightmapY = y + 1;

        for (i = 0; i < ds->numVerts; i++) {
          float s_scaled = (ds->verts[i].lightmap[0][0] - min_s) * scale;
          float t_scaled = (ds->verts[i].lightmap[0][1] - min_t) * scale;
          
          // Snap to integer (texel edge)
          s_scaled = floor(s_scaled + 0.5f);
          t_scaled = floor(t_scaled + 0.5f);

          // Apply +1 offset for the mandatory gutter
          ds->verts[i].lightmap[0][0] = (x + 1.0f + s_scaled) / LIGHTMAP_WIDTH;
          ds->verts[i].lightmap[0][1] = (y + 1.0f + t_scaled) / LIGHTMAP_HEIGHT;
        }
        return; // Done!
      }
    }
    // Fallback if allocation failed or too big
    _printf("WARNING: Snapped UV allocation failed or too large, falling back to standard logic.\n");
  }

  // ==========================================================================
  // STANDARD (FALLBACK) LOGIC
  // ==========================================================================
  // Limit lightmap size and adjust scale proportionally
  // so no UV coordinates fall outside the allocated block.
  w = ceil((max_s - min_s) * scale) + 1;
  h = ceil((max_t - min_t) * scale) + 1;

  if (w > LIGHTMAP_WIDTH - 2 || h > LIGHTMAP_HEIGHT - 2) {
    float scaleX = scale;
    float scaleY = scale;
    
    if (w > LIGHTMAP_WIDTH - 2) {
      scaleX = (float)(LIGHTMAP_WIDTH - 3) / (max_s - min_s);
    }
    if (h > LIGHTMAP_HEIGHT - 2) {
      scaleY = (float)(LIGHTMAP_HEIGHT - 3) / (max_t - min_t);
    }
    
    // Use the more restrictive scale to keep aspect ratio
    scale = (scaleX < scaleY) ? scaleX : scaleY;
    
    w = ceil((max_s - min_s) * scale) + 1;
    h = ceil((max_t - min_t) * scale) + 1;
  }

  if (w < 1)
    w = 1;
  if (h < 1)
    h = 1;

  // 4. Allocation (including 1-texel padding on all sides)
  qboolean allocated_success = qfalse;
  for (i = 0; i < numLightmaps; i++) {
    if (AllocLMBlock(i, w + 2, h + 2, &x, &y)) {
      ds->lightmapNum = i;
      allocated_success = qtrue;
      break;
    }
  }

  if (!allocated_success) {
    PrepareNewLightmap();
    if (!AllocLMBlock(numLightmaps - 1, w + 2, h + 2, &x, &y)) {
      Error("misc_model: Lightmap allocation failed");
    }
    ds->lightmapNum = numLightmaps - 1;
  }
  ds->lightmapWidth = w;
  ds->lightmapHeight = h;
  ds->lightmapX = x + 1;
  ds->lightmapY = y + 1;

  for (i = 0; i < ds->numVerts; i++) {
    ds->verts[i].lightmap[0][0] =
        (x + 1.0f + 0.5f + (ds->verts[i].lightmap[0][0] - min_s) * scale) /
        LIGHTMAP_WIDTH;
    ds->verts[i].lightmap[0][1] =
        (y + 1.0f + 0.5f + (ds->verts[i].lightmap[0][1] - min_t) * scale) /
        LIGHTMAP_HEIGHT;
  }
}

void AllocateLightmapForPatch(mapDrawSurface_t *ds) {
  int i, j, k;
  drawVert_t *verts;
  int w, h;
  int x, y;
  float s, t;
  mesh_t mesh, *subdividedMesh, *tempMesh, *newmesh;
  int widthtable[1024], heighttable[1024], ssize;

  verts = ds->verts;

  mesh.width = ds->patchWidth;
  mesh.height = ds->patchHeight;
  mesh.verts = verts;
  newmesh = SubdivideMesh(mesh, 8, 999);

  PutMeshOnCurve(*newmesh);
  tempMesh = RemoveLinearMeshColumnsRows(newmesh);
  FreeMesh(newmesh);

  ssize = ds->samplesize;

  subdividedMesh = SubdivideMeshQuads(tempMesh, ssize, LIGHTMAP_WIDTH - 2,
                                      widthtable, heighttable);

  w = subdividedMesh->width;
  h = subdividedMesh->height;

  FreeMesh(subdividedMesh);

  // allocate the lightmap (including 1-texel padding on all sides)
  c_exactLightmap += (w + 2) * (h + 2);

  qboolean allocated_patch_success = qfalse;
  for (i = 0; i < numLightmaps; i++) {
    if (AllocLMBlock(i, w + 2, h + 2, &x, &y)) {
      ds->lightmapNum = i;
      allocated_patch_success = qtrue;
      break;
    }
  }

  if (!allocated_patch_success) {
    PrepareNewLightmap();
    if (!AllocLMBlock(numLightmaps - 1, w + 2, h + 2, &x, &y)) {
      Error("Entity %i, brush %i: Patch lightmap allocation failed",
            ds->mapBrush->entitynum, ds->mapBrush->brushnum);
    }
    ds->lightmapNum = numLightmaps - 1;
  }

  // set the lightmap texture coordinates in the drawVerts
  // we add 1 to x and y to account for the padding gutter
  ds->lightmapWidth = w;
  ds->lightmapHeight = h;
  ds->lightmapX = x + 1;
  ds->lightmapY = y + 1;

  x = ds->lightmapX;
  y = ds->lightmapY;

  for (i = 0; i < ds->patchWidth; i++) {
    int k_w;
    for (k_w = 0; k_w < w; k_w++) {
      if (originalWidths[k_w] >= i) {
        break;
      }
    }
    if (k_w >= w)
      k_w = w - 1;
    s = x + k_w + 0.5f;
    for (j = 0; j < ds->patchHeight; j++) {
      int k_h;
      for (k_h = 0; k_h < h; k_h++) {
        if (originalHeights[k_h] >= j) {
          break;
        }
      }
      if (k_h >= h)
        k_h = h - 1;
      t = y + k_h + 0.5f;
      verts[i + j * ds->patchWidth].lightmap[0][0] = s / (float)LIGHTMAP_WIDTH;
      verts[i + j * ds->patchWidth].lightmap[0][1] = t / (float)LIGHTMAP_HEIGHT;
    }
  }

  // precision nudge pass: shift UVs slightly outward to prevent float point inaccuracies
  for (i = 0; i < ds->patchWidth * ds->patchHeight; i++) {
    float *uv = verts[i].lightmap[0];
    if (uv[0] <= (float)x / LIGHTMAP_WIDTH + 0.50001f / LIGHTMAP_WIDTH)
      uv[0] -= 0.0001f / LIGHTMAP_WIDTH;
    if (uv[0] >= (float)(x + w) / LIGHTMAP_WIDTH - 0.50001f / LIGHTMAP_WIDTH)
      uv[0] += 0.0001f / LIGHTMAP_WIDTH;
    if (uv[1] <= (float)y / LIGHTMAP_HEIGHT + 0.50001f / LIGHTMAP_HEIGHT)
      uv[1] -= 0.0001f / LIGHTMAP_HEIGHT;
    if (uv[1] >= (float)(y + h) / LIGHTMAP_HEIGHT - 0.50001f / LIGHTMAP_HEIGHT)
      uv[1] += 0.0001f / LIGHTMAP_HEIGHT;
  }
}

/*
===================
AllocateLightmapForSurface
===================
*/
// #define	LIGHTMAP_BLOCK	16
void AllocateLightmapForSurface(mapDrawSurface_t *ds) {
  vec3_t mins, maxs, size, delta;
  int i;
  drawVert_t *verts;
  int w, h;
  int x, y, ssize;
  int axis;
  vec3_t vecs[2];
  float s, t;
  vec3_t origin;
  plane_t *plane;
  float d;
  vec3_t planeNormal;

  if (ds->patch) {
    AllocateLightmapForPatch(ds);
    return;
  }

  ssize = ds->samplesize;

  plane = &mapplanes[ds->side->planenum];

  // bound the surface
  ClearBounds(mins, maxs);
  verts = ds->verts;
  for (i = 0; i < ds->numVerts; i++) {
    AddPointToBounds(verts[i].xyz, mins, maxs);
  }

  // round to the lightmap resolution
  for (i = 0; i < 3; i++) {
    mins[i] = ssize * floor(mins[i] / ssize);
    maxs[i] = ssize * ceil(maxs[i] / ssize);
    size[i] = (maxs[i] - mins[i]) / ssize + 1;
  }

  // the two largest axis will be the lightmap size
  memset(vecs, 0, sizeof(vecs));

  planeNormal[0] = fabs(plane->normal[0]);
  planeNormal[1] = fabs(plane->normal[1]);
  planeNormal[2] = fabs(plane->normal[2]);

  if (planeNormal[0] >= planeNormal[1] && planeNormal[0] >= planeNormal[2]) {
    w = size[1];
    h = size[2];
    axis = 0;
    vecs[0][1] = 1.0 / ssize;
    vecs[1][2] = 1.0 / ssize;
  } else if (planeNormal[1] >= planeNormal[0] &&
             planeNormal[1] >= planeNormal[2]) {
    w = size[0];
    h = size[2];
    axis = 1;
    vecs[0][0] = 1.0 / ssize;
    vecs[1][2] = 1.0 / ssize;
  } else {
    w = size[0];
    h = size[1];
    axis = 2;
    vecs[0][0] = 1.0 / ssize;
    vecs[1][1] = 1.0 / ssize;
  }

  if (!plane->normal[axis]) {
    Error("Chose a 0 valued axis");
  }

  if (w > LIGHTMAP_WIDTH - 2) {
    VectorScale(vecs[0], (float)(LIGHTMAP_WIDTH - 2) / w, vecs[0]);
    w = LIGHTMAP_WIDTH - 2;
  }

  if (h > LIGHTMAP_HEIGHT - 2) {
    VectorScale(vecs[1], (float)(LIGHTMAP_HEIGHT - 2) / h, vecs[1]);
    h = LIGHTMAP_HEIGHT - 2;
  }

  // allocate the lightmap (including 1-texel padding on all sides)
  c_exactLightmap += (w + 2) * (h + 2);

  qboolean allocated_surf_success = qfalse;
  for (i = 0; i < numLightmaps; i++) {
    if (AllocLMBlock(i, w + 2, h + 2, &x, &y)) {
      ds->lightmapNum = i;
      allocated_surf_success = qtrue;
      break;
    }
  }

  if (!allocated_surf_success) {
    PrepareNewLightmap();
    if (!AllocLMBlock(numLightmaps - 1, w + 2, h + 2, &x, &y)) {
      Error("Entity %i, brush %i: Surface lightmap allocation failed",
            ds->mapBrush->entitynum, ds->mapBrush->brushnum);
    }
    ds->lightmapNum = numLightmaps - 1;
  }

  // set the lightmap texture coordinates in the drawVerts
  // we add 1 to x and y to account for the padding gutter
  ds->lightmapWidth = w;
  ds->lightmapHeight = h;
  ds->lightmapX = x + 1;
  ds->lightmapY = y + 1;

  x = ds->lightmapX;
  y = ds->lightmapY;

  for (i = 0; i < ds->numVerts; i++) {
    VectorSubtract(verts[i].xyz, mins, delta);
    s = DotProduct(delta, vecs[0]) + x + 0.5f;
    t = DotProduct(delta, vecs[1]) + y + 0.5f;

    // micro-nudge UVs slightly outward to prevent float point inaccuracies
    if (s <= (float)x + 0.5001f) s -= 0.0001f;
    if (s >= (float)(x + w) - 0.5001f) s += 0.0001f;
    if (t <= (float)y + 0.5001f) t -= 0.0001f;
    if (t >= (float)(y + h) - 0.5001f) t += 0.0001f;

    verts[i].lightmap[0][0] = s / LIGHTMAP_WIDTH;
    verts[i].lightmap[0][1] = t / LIGHTMAP_HEIGHT;
  }

  // calculate the world coordinates of the lightmap samples

  // project mins onto plane to get origin
  d = DotProduct(mins, plane->normal) - plane->dist;
  d /= plane->normal[axis];
  VectorCopy(mins, origin);
  origin[axis] -= d;

  // project stepped lightmap blocks and subtract to get planevecs
  for (i = 0; i < 2; i++) {
    vec3_t normalized;
    float len;

    len = VectorNormalize(vecs[i], normalized);
    VectorScale(normalized, (1.0 / len), vecs[i]);
    d = DotProduct(vecs[i], plane->normal);
    d /= plane->normal[axis];
    vecs[i][axis] -= d;
  }

  VectorCopy(origin, ds->lightmapOrigin);
  VectorCopy(vecs[0], ds->lightmapVecs[0]);
  VectorCopy(vecs[1], ds->lightmapVecs[1]);
  VectorCopy(plane->normal, ds->lightmapVecs[2]);
}

/*
===================
AllocateLightmaps
===================
*/
void AllocateLightmaps(entity_t *e) {
  int i, j;
  mapDrawSurface_t *ds;
  shaderInfo_t *si;

  qprintf("--- AllocateLightmaps ---\n");

  if (!lightmapHeights) {
    lightmapHeights = calloc(MAX_LIGHTMAPS * MAX_LIGHTMAP_WIDTH, sizeof(int));
    numLightmaps = 0;
    PrepareNewLightmap(); // Start with the first lightmap
  }

  // sort all surfaces by shader so common shaders will usually
  // be in the same lightmap
  numSortShaders = 0;

  for (i = e->firstDrawSurf; i < numMapDrawSurfs; i++) {
    ds = &mapDrawSurfs[i];
    if (!ds->numVerts) {
      continue; // leftover from a surface subdivision
    }
    if (!ds->patch && !ds->miscModel) {
      VectorCopy(mapplanes[ds->side->planenum].normal, ds->lightmapVecs[2]);
    }

    // search for this shader
    for (j = 0; j < numSortShaders; j++) {
      if (ds->shaderInfo == surfsOnShader[j]->shaderInfo) {
        ds->nextOnShader = surfsOnShader[j];
        surfsOnShader[j] = ds;
        break;
      }
    }
    if (j == numSortShaders) {
      if (numSortShaders >= MAX_MAP_SHADERS) {
        Error("MAX_MAP_SHADERS");
      }
      surfsOnShader[j] = ds;
      numSortShaders++;
    }
  }
  qprintf("%5i unique shaders\n", numSortShaders);

  // for each shader, allocate lightmaps for each surface

  //	numLightmaps = 0;
  //	PrepareNewLightmap();

  for (i = 0; i < numSortShaders; i++) {
    si = surfsOnShader[i]->shaderInfo;

    for (ds = surfsOnShader[i]; ds; ds = ds->nextOnShader) {
      // some surfaces don't need lightmaps allocated for them
      if (si->surfaceFlags & SURF_NOLIGHTMAP) {
        ds->lightmapNum = -1;
        if (ds->miscModel)
          _printf("TriSoup surface skipped (SURF_NOLIGHTMAP): shader %s\n",
                  si->shader);
      } else if (si->surfaceFlags & SURF_POINTLIGHT) {
        ds->lightmapNum = -3;
        if (ds->miscModel)
          _printf("TriSoup surface skipped (SURF_POINTLIGHT): shader %s\n",
                  si->shader);
      } else {
        if (ds->miscModel) {
          AllocateLightmapForMiscModel(ds);
        } else {
          AllocateLightmapForSurface(ds);
        }
      }
    }
  }

  // Set numLightBytes so WriteBSPFile will export the lump
  numLightBytes = numLightmaps * LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3;
  if (numLightBytes > MAX_MAP_LIGHTING) {
    Error("MAX_MAP_LIGHTING exceeded");
  }

  _printf("%5i unique shaders\n", numSortShaders);
  _printf("%5i lightmaps allocated (%dx%d resolution)\n", numLightmaps, LIGHTMAP_WIDTH, LIGHTMAP_HEIGHT);

  // Clear the lightmap buffer
  memset(lightBytes, 0, numLightBytes);

  qprintf("%7i exact lightmap texels\n", c_exactLightmap);
  qprintf("%7i block lightmap texels\n", numLightBytes);
}

void FreeLightmaps(void) {
  if (lightmapHeights) {
    free(lightmapHeights);
    lightmapHeights = NULL;
  }
}
