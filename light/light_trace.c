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
#include "light.h"
#include <embree4/rtcore.h>

RTCDevice g_device = NULL;
RTCScene g_scene = NULL;



static void AddBrushesToEmbree(RTCScene scene);
void AlphaFilter(const struct RTCFilterFunctionNArguments *args);

#define TRACE_EPSILON 0.001

int c_totalTrace;

/*
====================
InitTracingGeometry

Builds the Embree scene for ray tracing against surfaces and brushes
====================
*/
void InitTracingGeometry(void) {
  int i, j;
  dsurface_t *dsurf;
  vec3_t mins, maxs;

  // Embree 4 initialization
  g_device = rtcNewDevice(NULL);
  if (!g_device) {
    Error("Embree: Failed to create device\n");
  }
  g_scene = rtcNewScene(g_device);
  rtcSetSceneFlags(g_scene, RTC_SCENE_FLAG_ROBUST);

  _printf("--- InitTracingGeometry: Embree 4.x ---\n");

  // Add brushes to Embree for solid occlusion
  AddBrushesToEmbree(g_scene);

  int count = 0;
  for (i = 0; i < numDrawSurfaces; i++) {
    dsurf = &drawSurfaces[i];
    
    // don't make surfaces for transparent objects
    // because we want light to pass through them
    shaderInfo_t *si = ShaderInfoForShader(dshaders[dsurf->shaderNum].shader);
    if ((si->contents & CONTENTS_TRANSLUCENT) &&
        !(si->surfaceFlags & SURF_ALPHASHADOW)) {
      continue;
    }



    if (dsurf->numIndexes > 0 &&
        (dsurf->surfaceType == MST_TRIANGLE_SOUP ||
         dsurf->surfaceType == MST_PLANAR)) {
      
      RTCGeometry geom = rtcNewGeometry(g_device, RTC_GEOMETRY_TYPE_TRIANGLE);
      rtcSetGeometryBuildQuality(geom, RTC_BUILD_QUALITY_HIGH);

      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0,
                                 RTC_FORMAT_FLOAT3, drawVerts, 0,
                                 sizeof(drawVert_t), numDrawVerts);

      unsigned int *indices = rtcSetNewGeometryBuffer(
          geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
          3 * sizeof(unsigned int), dsurf->numIndexes / 3);
      for (j = 0; j < dsurf->numIndexes; j++) {
        indices[j] = (unsigned int)(drawIndexes[dsurf->firstIndex + j] +
                                    dsurf->firstVert);
      }

      rtcSetGeometryIntersectFilterFunction(geom, AlphaFilter);
      rtcCommitGeometry(geom);
      rtcAttachGeometryByID(g_scene, geom, i);
      rtcReleaseGeometry(geom);
      count++;
    } else if (dsurf->surfaceType == MST_PATCH) {
      mesh_t srcMesh, *subdivided, *mesh;
      srcMesh.width = dsurf->patchWidth;
      srcMesh.height = dsurf->patchHeight;
      srcMesh.verts = &drawVerts[dsurf->firstVert];

      mesh = SubdivideMesh(srcMesh, 8, 999);
      PutMeshOnCurve(*mesh);
      MakeMeshNormals(*mesh);
      subdivided = RemoveLinearMeshColumnsRows(mesh);
      FreeMesh(mesh);

      RTCGeometry geom = rtcNewGeometry(g_device, RTC_GEOMETRY_TYPE_TRIANGLE);
      rtcSetGeometryBuildQuality(geom, RTC_BUILD_QUALITY_HIGH);

      int numVerts = subdivided->width * subdivided->height;
      void *verts = rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0,
                                            RTC_FORMAT_FLOAT3,
                                            sizeof(drawVert_t), numVerts);
      memcpy(verts, subdivided->verts, sizeof(drawVert_t) * numVerts);

      int numTris = (subdivided->width - 1) * (subdivided->height - 1) * 2;
      unsigned int *indices = rtcSetNewGeometryBuffer(
          geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
          3 * sizeof(unsigned int), numTris);

      int triCount = 0;
      for (int x = 0; x < subdivided->width - 1; x++) {
        for (int y = 0; y < subdivided->height - 1; y++) {
          int v1 = y * subdivided->width + x;
          int v2 = v1 + 1;
          int v3 = v1 + subdivided->width + 1;
          int v4 = v1 + subdivided->width;

          indices[triCount * 3 + 0] = v1;
          indices[triCount * 3 + 1] = v4;
          indices[triCount * 3 + 2] = v3;
          triCount++;

          indices[triCount * 3 + 0] = v1;
          indices[triCount * 3 + 1] = v3;
          indices[triCount * 3 + 2] = v2;
          triCount++;
        }
      }

      FreeMesh(subdivided);

      rtcSetGeometryIntersectFilterFunction(geom, AlphaFilter);
      rtcCommitGeometry(geom);
      rtcAttachGeometryByID(g_scene, geom, i);
      rtcReleaseGeometry(geom);
      count++;
    }
  }

  rtcCommitScene(g_scene);
  _printf("%d surfaces added to Embree scene\n", count);
}

/*
================
Trace_SampleFilter

Returns qtrue if the ray is fully blocked (absolute opacity).
Returns qfalse if the ray passes through (transparent or tinted).
Multiplies 'filter' by the sampled texture color.
================
*/
qboolean Trace_SampleFilter(shaderInfo_t *si, float s, float t, vec3_t filter) {
  int x, y;
  byte *pixel;
  byte alpha;

  if (!si || !si->pixels) {
    VectorClear(filter);
    return qtrue; // Solid
  }

  // 1. Wrap UVs
  s = s - floor(s);
  t = 1.0f - (t - floor(t)); // Flip T for standard texture orientation

  x = s * si->width;
  y = t * si->height;
  if (x < 0)
    x = 0;
  else if (x >= si->width)
    x = si->width - 1;
  if (y < 0)
    y = 0;
  else if (y >= si->height)
    y = si->height - 1;

  pixel = si->pixels + 4 * (y * si->width + x);
  alpha = pixel[3];

  // 2. Apply Alpha Threshold (Proposed 80% transparency rule)
  // If it's more than 80% transparent (alpha < 51), it's a hole.
  if (alpha < 51) {
    return qfalse; // Ray passes through unfiltered
  }

  // 3. Apply Tinting
  // Multiply cumulative filter by normalized texture RGB
  filter[0] *= (float)pixel[0] / 255.0f;
  filter[1] *= (float)pixel[1] / 255.0f;
  filter[2] *= (float)pixel[2] / 255.0f;

  // 4. Determine if it blocks entirely
  // If alpha is high (> 250), we consider it fully opaque for occlusion.
  if (alpha > 250) {
    return qtrue; // Blocks ray
  }

  return qfalse; // Continues ray (tinted)
}

/*
=============
AlphaFilter

Embree intersection filter for handling ignoreSurface and alpha shadows
=============
*/
void AlphaFilter(const struct RTCFilterFunctionNArguments *args) {
  if (args->valid[0] != -1)
    return;
  if (!args->context)
    return; // No context provided, skip advanced filtering
  struct MyRayQueryContext *mcontext = (struct MyRayQueryContext *)args->context;
  traceWork_t *tw = mcontext->tw;
  struct RTCHit *hit = (struct RTCHit *)args->hit;
  unsigned int geomID = hit->geomID;
  unsigned int primID = hit->primID;

  // Only perform additional checks for draw surfaces
  if (geomID < (unsigned int)numDrawSurfaces) {
    dsurface_t *ds = &drawSurfaces[geomID];

    // Respect patchshadows setting
    if (!mcontext->patchshadows && ds->surfaceType == MST_PATCH) {
      args->valid[0] = 0;
      return;
    }

    // Ignore hits on the starting surface within a small epsilon to prevent self-shadowing artifacts
    if (tw && tw->ignoreSurface != -1 && geomID == (unsigned int)tw->ignoreSurface) {
      struct RTCRay *ray = (struct RTCRay *)args->ray;
      if (ray->tfar < SELF_SHADOW_EPSILON) {
        args->valid[0] = 0;
        return;
      }
    }

    shaderInfo_t *si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);

    if (si->surfaceFlags & SURF_ALPHASHADOW) {
      float u = hit->u;
      float v = hit->v;
      float s, t;

      if (ds->surfaceType == MST_TRIANGLE_SOUP || ds->surfaceType == MST_PLANAR) {
        // Standard draw surface vertices
        drawVert_t *v0 = &drawVerts[drawIndexes[ds->firstIndex + primID * 3 + 0] + ds->firstVert];
        drawVert_t *v1 = &drawVerts[drawIndexes[ds->firstIndex + primID * 3 + 1] + ds->firstVert];
        drawVert_t *v2 = &drawVerts[drawIndexes[ds->firstIndex + primID * 3 + 2] + ds->firstVert];

        s = (1.0f - u - v) * v0->st[0] + u * v1->st[0] + v * v2->st[0];
        t = (1.0f - u - v) * v0->st[1] + u * v1->st[1] + v * v2->st[1];
      } else if (ds->surfaceType == MST_PATCH) {
        // Patches have their vertices copied into the Embree buffer during InitTracingGeometry.
        RTCGeometry geom = rtcGetGeometry(g_scene, geomID);
        drawVert_t *verts = (drawVert_t *)rtcGetGeometryBufferData(geom, RTC_BUFFER_TYPE_VERTEX, 0);
        unsigned int *indices = (unsigned int *)rtcGetGeometryBufferData(geom, RTC_BUFFER_TYPE_INDEX, 0);

        drawVert_t *v0 = &verts[indices[primID * 3 + 0]];
        drawVert_t *v1 = &verts[indices[primID * 3 + 1]];
        drawVert_t *v2 = &verts[indices[primID * 3 + 2]];

        s = (1.0f - u - v) * v0->st[0] + u * v1->st[0] + v * v2->st[0];
        t = (1.0f - u - v) * v0->st[1] + u * v1->st[1] + v * v2->st[1];
      } else {
        return; // Unknown surface type
      }

      // Sample the filter
      if (Trace_SampleFilter(si, s, t, tw->trace->filter)) {
        // Opaque hit - keep the valid flag (blocks)
      } else {
        // Transparent/Tinted hit - tell Embree to ignore and continue
        args->valid[0] = 0;
      }
    }
  }
}

/*
=============
AddBrushesToEmbree

Experimental: Converts all brushes to triangles and adds them to the Embree scene.
=============
*/
static void AddBrushesToEmbree(RTCScene scene) {
  int i, j, k;
  dbrush_t *b;
  dbrushside_t *s;
  dplane_t *p, *p2;
  winding_t *w;
  int brushTriangles = 0;

  _printf("--- AddBrushesToEmbree: Converting brushes to triangles ---\n");

  for (i = 0; i < numbrushes; i++) {
    b = &dbrushes[i];

    if (dshaders[b->shaderNum].contentFlags &
        (CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_WATER |
         CONTENTS_TRANSLUCENT)) {
      continue;
    }

    for (j = 0; j < b->numSides; j++) {
      s = &dbrushsides[b->firstSide + j];
      p = &dplanes[s->planeNum];

      w = BaseWindingForPlane(p->normal, p->dist);
      for (k = 0; k < b->numSides && w; k++) {
        p2 = &dplanes[dbrushsides[b->firstSide + k].planeNum];
        ChopWindingInPlace(&w, p2->normal, p2->dist, 0.1f);
      }

      if (!w) {
        continue;
      }

      if (w->numpoints < 3) {
        FreeWinding(w);
        continue;
      }

      RTCGeometry geom = rtcNewGeometry(g_device, RTC_GEOMETRY_TYPE_TRIANGLE);
      rtcSetGeometryBuildQuality(geom, RTC_BUILD_QUALITY_HIGH);

      vec3_t *verts = (vec3_t *)rtcSetNewGeometryBuffer(
          geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sizeof(vec3_t),
          w->numpoints);
      int *indices = (int *)rtcSetNewGeometryBuffer(
          geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, sizeof(int) * 3,
          w->numpoints - 2);

      for (k = 0; k < w->numpoints; k++) {
        VectorCopy(w->points[k], verts[k]);
      }

      for (k = 0; k < w->numpoints - 2; k++) {
        indices[k * 3 + 0] = 0;
        indices[k * 3 + 1] = k + 1;
        indices[k * 3 + 2] = k + 2;
      }

      rtcCommitGeometry(geom);
      rtcAttachGeometryByID(scene, geom, numDrawSurfaces + brushTriangles);
      rtcReleaseGeometry(geom);

      brushTriangles++;
      FreeWinding(w);
    }
  }
  _printf("Added %i brush sides as geometry to Embree\n", brushTriangles);
}

/*
==============
InitTrace

Loads the tracing geometry structure
=============
*/
void InitTrace(void) {
  InitTracingGeometry();
}

/*
===================
PointInSolid
===================
*/
qboolean PointInSolid_r(vec3_t start, int node) {
  dnode_t *dnode;
  dplane_t *dplane;
  double front;

  while (node >= 0) {
    dnode = &dnodes[node];
    dplane = &dplanes[dnode->planeNum];

    int type = PlaneTypeForNormal(dplane->normal);
    if (type <= PLANE_Z) {
      front = (double)start[type] - dplane->dist;
    } else {
      front = ((double)start[0] * dplane->normal[0] +
               (double)start[1] * dplane->normal[1] +
               (double)start[2] * dplane->normal[2]) -
              dplane->dist;
    }

    if (front > -TRACE_EPSILON && front < TRACE_EPSILON) {
      // exactly on node, must check both sides
      return (qboolean)(PointInSolid_r(start, dnode->children[0]) |
                        PointInSolid_r(start, dnode->children[1]));
    }

    if (front >= TRACE_EPSILON) {
      node = dnode->children[0];
    } else {
      node = dnode->children[1];
    }
  }

  // Handle leaf
  int leafNum = -node - 1;
  if (dleafs[leafNum].cluster == -1) {
    return qtrue; // Opaque cluster is solid
  }
  return qfalse;
}

/*
=============
PointInSolid
=============
*/
qboolean PointInSolid(vec3_t start) { return PointInSolid_r(start, 0); }

/*
===================
PointInTrisoup
===================
*/
qboolean PointInTrisoup(vec3_t origin, vec3_t normal) {
  struct RTCRayHit rayhit;
  struct RTCIntersectArguments iargs;
  struct MyRayQueryContext context;
  rtcInitRayQueryContext(&context.context);
  context.tw = NULL;
  context.patchshadows = patchshadows;
  
  rayhit.ray.org_x = origin[0];
  rayhit.ray.org_y = origin[1];
  rayhit.ray.org_z = origin[2];
  rayhit.ray.dir_x = normal[0];
  rayhit.ray.dir_y = normal[1];
  rayhit.ray.dir_z = normal[2];
  rayhit.ray.tnear = 0.0001f;
  rayhit.ray.tfar = 10000.0f; // Cast far enough to hit the enclosing hull
  rayhit.ray.mask = 0xFFFFFFFF;
  rayhit.ray.flags = 0;
  rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
  rayhit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

  rtcInitIntersectArguments(&iargs);
  iargs.context = &context.context;
  rtcIntersect1(g_scene, &rayhit, &iargs);

  if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID && rayhit.hit.geomID < (unsigned int)numDrawSurfaces) {
      dsurface_t *ds = &drawSurfaces[rayhit.hit.geomID];
      if (ds->surfaceType == MST_TRIANGLE_SOUP) {
          // Check if we hit the backface (inside of the Trisoup looking out)
          float dot = rayhit.ray.dir_x * rayhit.hit.Ng_x + rayhit.ray.dir_y * rayhit.hit.Ng_y + rayhit.ray.dir_z * rayhit.hit.Ng_z;
          if (dot > 0.0f) {
              return qtrue; // We are inside a closed Trisoup
          }
      }
  }

  return qfalse;
}

/*
=============
TraceLine_Embree

High-performance Embree tracing path
=============
*/
static void TraceLine_Embree(const vec3_t start, const vec3_t stop,
                             trace_t *trace, qboolean testAll, traceWork_t *tw) {
  int i;
  struct RTCRayHit rayhit;
  struct MyRayQueryContext context;
  rtcInitRayQueryContext(&context.context);
  context.tw = tw;
  context.patchshadows = tw ? tw->patchshadows : patchshadows;

  vec3_t dir;
  float length;

  VectorSubtract(stop, start, dir);
  length = VectorLength(dir);
  if (length < 0.0001f) {
      trace->hitFraction = 1.0f;
      VectorCopy(start, trace->hit);
      VectorSet(trace->filter, 1, 1, 1);
      return;
  }
  VectorScale(dir, 1.0f / length, dir);

  rayhit.ray.org_x = start[0];
  rayhit.ray.org_y = start[1];
  rayhit.ray.org_z = start[2];
  rayhit.ray.dir_x = dir[0];
  rayhit.ray.dir_y = dir[1];
  rayhit.ray.dir_z = dir[2];
  rayhit.ray.tnear = 0.0001f;
  rayhit.ray.tfar = length;
  rayhit.ray.mask = 0xFFFFFFFF;
  rayhit.ray.flags = 0;
  rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
  rayhit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

  if (testAll) {
    struct RTCIntersectArguments iargs;
    rtcInitIntersectArguments(&iargs);
    iargs.context = &context.context;
    rtcIntersect1(g_scene, &rayhit, &iargs);
  } else {
    // Optimization for simple occlusion checks
    struct RTCOccludedArguments oargs;
    rtcInitOccludedArguments(&oargs);
    oargs.context = &context.context;
    rtcOccluded1(g_scene, &rayhit.ray, &oargs);

    // If occluded, tfar becomes -infinity in Embree 4
    if (rayhit.ray.tfar < 0) {
      rayhit.hit.geomID = 0; // Mark as hit
      rayhit.ray.tfar = 0.0f;
    }
  }

  trace->filter[0] = 1.0;
  trace->filter[1] = 1.0;
  trace->filter[2] = 1.0;
  trace->passSolid = (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID);
  trace->hitFraction = rayhit.ray.tfar / length;

  for (i = 0; i < 3; i++) {
    trace->hit[i] = start[i] + (stop[i] - start[i]) * trace->hitFraction;
  }
}

/*
=============
TraceLine
=============
*/
void TraceLine(const vec3_t start, const vec3_t stop, trace_t *trace,
               qboolean testAll, traceWork_t *tw) {
  TraceLine_Embree(start, stop, trace, testAll, tw);
}

/*
=============
CleanupTrace

Releases Embree resources
=============
*/
void CleanupTrace(void) {
  if (g_scene) {
    rtcReleaseScene(g_scene);
    g_scene = NULL;
  }
  if (g_device) {
    rtcReleaseDevice(g_device);
    g_device = NULL;
  }
}
