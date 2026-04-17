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

#define CURVE_FACET_ERROR 8
#define TRACE_EPSILON 0.001

int c_totalTrace;
int c_cullTrace, c_testTrace;
int c_testFacets;

surfaceTest_t *surfaceTest[MAX_MAP_DRAW_SURFS];

/*
=====================
CM_GenerateBoundaryForPoints
=====================
*/
void CM_GenerateBoundaryForPoints(float boundary[4], float plane[4], vec3_t a,
                                  vec3_t b) {
  vec3_t d1;

  // amke a perpendicular vector to the edge and the surface
  VectorSubtract(b, a, d1);
  CrossProduct(plane, d1, boundary);
  VectorNormalize(boundary, boundary);
  boundary[3] = DotProduct(a, boundary);
}

/*
=====================
TextureMatrixFromPoints
=====================
*/
void TextureMatrixFromPoints(cFacet_t *f, drawVert_t *a, drawVert_t *b,
                             drawVert_t *c) {
  int i;

  for (i = 0; i < 2; i++) {
    TexturePlaneFromPoints(f->textureMatrix[i], a->xyz, a->st[i], b->xyz, b->st[i], c->xyz, c->st[i]);
  }
}
    /*
                    s = fabs( DotProduct( a->xyz, f->textureMatrix[i] ) -
       a->st[i] ); if ( s > 0.01 ) { Error( "Bad textureMatrix" );
                    }
                    s = fabs( DotProduct( b->xyz, f->textureMatrix[i] ) -
       b->st[i] ); if ( s > 0.01 ) { Error( "Bad textureMatrix" );
                    }
                    s = fabs( DotProduct( c->xyz, f->textureMatrix[i] ) -
       c->st[i] ); if ( s > 0.01 ) { Error( "Bad textureMatrix" );
                    }
    */

/*
=====================
CM_GenerateFacetFor3Points
=====================
*/
qboolean CM_GenerateFacetFor3Points(cFacet_t *f, drawVert_t *a, drawVert_t *b,
                                    drawVert_t *c) {
  // if we can't generate a valid plane for the points, ignore the facet
  if (!PlaneFromPoints(f->surface, a->xyz, b->xyz, c->xyz)) {
    f->numBoundaries = 0;
    return qfalse;
  }

  // make boundaries
  f->numBoundaries = 3;

  CM_GenerateBoundaryForPoints(f->boundaries[0], f->surface, a->xyz, b->xyz);
  CM_GenerateBoundaryForPoints(f->boundaries[1], f->surface, b->xyz, c->xyz);
  CM_GenerateBoundaryForPoints(f->boundaries[2], f->surface, c->xyz, a->xyz);

  VectorCopy(a->xyz, f->points[0]);
  VectorCopy(b->xyz, f->points[1]);
  VectorCopy(c->xyz, f->points[2]);

  TextureMatrixFromPoints(f, a, b, c);

  return qtrue;
}

/*
=====================
CM_GenerateFacetFor4Points

Attempts to use four points as a planar quad
=====================
*/
#define PLANAR_EPSILON 0.1
qboolean CM_GenerateFacetFor4Points(cFacet_t *f, drawVert_t *a, drawVert_t *b,
                                    drawVert_t *c, drawVert_t *d) {
  float dist;
  int i;
  vec4_t plane;

  // if we can't generate a valid plane for the points, ignore the facet
  if (!PlaneFromPoints(f->surface, a->xyz, b->xyz, c->xyz)) {
    f->numBoundaries = 0;
    return qfalse;
  }

  // if the fourth point is also on the plane, we can make a quad facet
  dist = DotProduct(d->xyz, f->surface) - f->surface[3];
  if (fabs(dist) > PLANAR_EPSILON) {
    f->numBoundaries = 0;
    return qfalse;
  }

  // make boundaries
  f->numBoundaries = 4;

  CM_GenerateBoundaryForPoints(f->boundaries[0], f->surface, a->xyz, b->xyz);
  CM_GenerateBoundaryForPoints(f->boundaries[1], f->surface, b->xyz, c->xyz);
  CM_GenerateBoundaryForPoints(f->boundaries[2], f->surface, c->xyz, d->xyz);
  CM_GenerateBoundaryForPoints(f->boundaries[3], f->surface, d->xyz, a->xyz);

  VectorCopy(a->xyz, f->points[0]);
  VectorCopy(b->xyz, f->points[1]);
  VectorCopy(c->xyz, f->points[2]);
  VectorCopy(d->xyz, f->points[3]);

  for (i = 1; i < 4; i++) {
    if (!PlaneFromPoints(plane, f->points[i], f->points[(i + 1) % 4],
                         f->points[(i + 2) % 4])) {
      f->numBoundaries = 0;
      return qfalse;
    }

    if (DotProduct(f->surface, plane) < 0.9) {
      f->numBoundaries = 0;
      return qfalse;
    }
  }

  TextureMatrixFromPoints(f, a, b, c);

  return qtrue;
}



/*
====================
FacetsForTriangleSurface
====================
*/
void FacetsForTriangleSurface(dsurface_t *dsurf, shaderInfo_t *si,
                              surfaceTest_t *test, int surfaceNum) {
  int i;
  drawVert_t *v1, *v2, *v3, *v4;
  int count;
  int i1, i2, i3, i4, i5, i6;

  test->patch = qfalse;
  test->numFacets = dsurf->numIndexes / 3;
  test->facets = malloc(sizeof(test->facets[0]) * test->numFacets);
  test->shader = si;

  count = 0;
  for (i = 0; i < test->numFacets; i++) {
    i1 = drawIndexes[dsurf->firstIndex + i * 3];
    i2 = drawIndexes[dsurf->firstIndex + i * 3 + 1];
    i3 = drawIndexes[dsurf->firstIndex + i * 3 + 2];

    v1 = &drawVerts[dsurf->firstVert + i1];
    v2 = &drawVerts[dsurf->firstVert + i2];
    v3 = &drawVerts[dsurf->firstVert + i3];

    // try and make a quad out of two triangles
    if (i != test->numFacets - 1) {
      i4 = drawIndexes[dsurf->firstIndex + i * 3 + 3];
      i5 = drawIndexes[dsurf->firstIndex + i * 3 + 4];
      i6 = drawIndexes[dsurf->firstIndex + i * 3 + 5];
      if (i4 == i3 && i5 == i2) {
        v4 = &drawVerts[dsurf->firstVert + i6];
        if (CM_GenerateFacetFor4Points(&test->facets[count], v1, v2, v4, v3)) {
          count++;
          i++; // skip next tri
          continue;
        }
      }
    }

    if (CM_GenerateFacetFor3Points(&test->facets[count], v1, v2, v3))
      count++;
  }

  for (i = 0; i < count; i++) {
    test->facets[i].surfaceNum = surfaceNum;
  }

  // we may have turned some pairs into quads
  test->numFacets = count;
}

/*
====================
FacetsForPatch
====================
*/
void FacetsForPatch(dsurface_t *dsurf, shaderInfo_t *si, surfaceTest_t *test,
                    int surfaceNum) {
  int i, j;
  drawVert_t *v1, *v2, *v3, *v4;
  int count;
  mesh_t srcMesh, *subdivided, *mesh;

  srcMesh.width = dsurf->patchWidth;
  srcMesh.height = dsurf->patchHeight;
  srcMesh.verts = &drawVerts[dsurf->firstVert];

  // subdivided = SubdivideMesh( mesh, CURVE_FACET_ERROR, 9999 );
  mesh = SubdivideMesh(srcMesh, 8, 999);
  PutMeshOnCurve(*mesh);
  MakeMeshNormals(*mesh);

  subdivided = RemoveLinearMeshColumnsRows(mesh);
  FreeMesh(mesh);

  test->patch = qtrue;
  test->numFacets = (subdivided->width - 1) * (subdivided->height - 1) * 2;
  test->facets = malloc(sizeof(test->facets[0]) * test->numFacets);
  test->shader = si;

  count = 0;
  for (i = 0; i < subdivided->width - 1; i++) {
    for (j = 0; j < subdivided->height - 1; j++) {

      v1 = subdivided->verts + j * subdivided->width + i;
      v2 = v1 + 1;
      v3 = v1 + subdivided->width + 1;
      v4 = v1 + subdivided->width;

      if (CM_GenerateFacetFor4Points(&test->facets[count], v1, v4, v3, v2)) {
        count++;
      } else {
        if (CM_GenerateFacetFor3Points(&test->facets[count], v1, v4, v3))
          count++;
        if (CM_GenerateFacetFor3Points(&test->facets[count], v1, v3, v2))
          count++;
      }
    }
  }
  for (i = 0; i < count; i++) {
    test->facets[i].surfaceNum = surfaceNum;
  }
  test->numFacets = count;
  FreeMesh(subdivided);
}

/*
=====================
InitSurfacesForTesting

Builds structures to speed the ray tracing against surfaces
=====================
*/
void InitTracingGeometry(void) {
  int i, j;
  dsurface_t *dsurf;
  surfaceTest_t *test;
  drawVert_t *dvert;
  shaderInfo_t *si;

  if (!embree) {
    _printf("--- InitTracingGeometry: standard ---\n");
    for (i = 0; i < numDrawSurfaces; i++) {
      dsurf = &drawSurfaces[i];
      if (!dsurf->numIndexes && !dsurf->patchWidth) {
        continue;
      }

      // don't make surfaces for transparent objects
      // because we want light to pass through them
      si = ShaderInfoForShader(dshaders[dsurf->shaderNum].shader);
      if ((si->contents & CONTENTS_TRANSLUCENT) &&
          !(si->surfaceFlags & SURF_ALPHASHADOW)) {
        continue;
      }

      test = malloc(sizeof(*test));
      surfaceTest[i] = test;
      ClearBounds(test->mins, test->maxs);

      dvert = &drawVerts[dsurf->firstVert];
      for (j = 0; j < dsurf->numVerts; j++, dvert++) {
        AddPointToBounds(dvert->xyz, test->mins, test->maxs);
      }

      SphereFromBounds(test->mins, test->maxs, test->origin, &test->radius);

      if (dsurf->surfaceType == MST_TRIANGLE_SOUP ||
          dsurf->surfaceType == MST_PLANAR) {
        FacetsForTriangleSurface(dsurf, si, test, i);
      } else if (dsurf->surfaceType == MST_PATCH) {
        FacetsForPatch(dsurf, si, test, i);
      }
      test->surfaceNum = i;
    }
    return;
  }

  // Embree 4 initialization
  g_device = rtcNewDevice(NULL);
  if (!g_device) {
    Error("Embree: Failed to create device\n");
  }
  g_scene = rtcNewScene(g_device);
  rtcSetSceneFlags(g_scene, RTC_SCENE_FLAG_ROBUST);

  _printf("--- InitTracingGeometry: embree ---\n");

  // Embree path is strictly brute-force (everything to Embree)
  AddBrushesToEmbree(g_scene);

  int count = 0;
  for (i = 0; i < numDrawSurfaces; i++) {
    dsurface_t *dsurf = &drawSurfaces[i];
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
=====================
GenerateBoundaryForPoints
=====================
*/
void GenerateBoundaryForPoints(float boundary[4], float plane[4], vec3_t a,
                               vec3_t b) {
  vec3_t d1;

  // amke a perpendicular vector to the edge and the surface
  VectorSubtract(b, a, d1);
  CrossProduct(plane, d1, boundary);
  VectorNormalize(boundary, boundary);
  boundary[3] = DotProduct(a, boundary);
}

/*
=================
SetFacetFilter

Given a point on a facet, determine the color filter
for light passing through
=================
*/
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

void SetFacetFilter(traceWork_t *tr, shaderInfo_t *shader, cFacet_t *facet,
                    vec3_t point) {
  float s, t;

  // most surfaces are completely opaque
  if (!(shader->surfaceFlags & SURF_ALPHASHADOW)) {
    VectorClear(tr->trace->filter);
    return;
  }

  s = DotProduct(point, facet->textureMatrix[0]) + facet->textureMatrix[0][3];
  t = DotProduct(point, facet->textureMatrix[1]) + facet->textureMatrix[1][3];

  if (Trace_SampleFilter(shader, s, t, tr->trace->filter)) {
    VectorClear(tr->trace->filter); // Opaque hit
  }
}

/*
====================
TraceAgainstFacet

Shader is needed for translucent surfaces
====================
*/
void TraceAgainstFacet(traceWork_t *tr, shaderInfo_t *shader, cFacet_t *facet) {
  int j;
  double d1, d2, dist;
  float d, f;
  vec3_t point;

  // ignore degenerate facets
  if (facet->numBoundaries < 3) {
    return;
  }

  dist = facet->surface[3];

  // compare the trace endpoints against the facet plane
  d1 = (double)tr->start[0] * facet->surface[0] +
       (double)tr->start[1] * facet->surface[1] +
       (double)tr->start[2] * facet->surface[2] - dist;
  if (d1 > -SELF_SHADOW_EPSILON && d1 < SELF_SHADOW_EPSILON) {
    return; // don't self intersect
  }
  d2 = (double)tr->end[0] * facet->surface[0] +
       (double)tr->end[1] * facet->surface[1] +
       (double)tr->end[2] * facet->surface[2] - dist;

  // calculate the intersection fraction
  f = d1 / (d1 - d2);
  if (f < TRACE_EPSILON) {
    return;
  }

  // --- Mesh Trace Distance Jump (q3map2 style) ---
  {
    vec3_t dir;
    float len;
    float dist_hit;

    VectorSubtract(tr->end, tr->start, dir);
    len = VectorLength(dir);
    dist_hit = f * len;

    if (dist_hit < SELF_SHADOW_EPSILON) {
      return; // handle self-shadowing and junctions
    }

    // --- ignore own planar surface ---
    if (tr->ignoreSurface != -1) {
      if (tr->ignoreSurface == (int)facet->surfaceNum) {
        if (drawSurfaces[tr->ignoreSurface].surfaceType == MST_PLANAR) {
          return;
        }
      }
    }
  }

  if (f >= tr->trace->hitFraction) {
    return; // we have hit something earlier
  }

  // calculate the intersection point
  for (j = 0; j < 3; j++) {
    point[j] = tr->start[j] + f * (tr->end[j] - tr->start[j]);
  }

  // check the point against the facet boundaries
  for (j = 0; j < facet->numBoundaries; j++) {
    // adjust the plane distance apropriately for mins/maxs
    dist = facet->boundaries[j][3];

    d = DotProduct(point, facet->boundaries[j]);
    if (d > dist + ON_EPSILON) {
      break; // outside the bounds
    }
  }

  if (j != facet->numBoundaries) {
    return; // we are outside the bounds of the facet
  }

  // we hit this facet

  // if this is a transparent surface, calculate filter value
  if (shader->surfaceFlags & SURF_ALPHASHADOW) {
    SetFacetFilter(tr, shader, facet, point);
  } else {
    // completely opaque
    VectorClear(tr->trace->filter);
    tr->trace->hitFraction = f;
  }

  //	VectorCopy( facet->surface, tr->trace->plane.normal );
  //	tr->trace->plane.dist = facet->surface[3];
}

/*
===============================================================

  LINE TRACING

===============================================================
*/



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

  // Only skip ignoreSurface for planar surfaces (which can't shadow themselves).
  // Non-planar surfaces like trisoups and patches MUST be allowed to self-shadow.
  if (tw && tw->ignoreSurface != -1 && geomID == (unsigned int)tw->ignoreSurface) {
    if (drawSurfaces[geomID].surfaceType == MST_PLANAR) {
      args->valid[0] = 0;
      return;
    }
  }

  // Only perform additional checks for draw surfaces
  if (geomID < (unsigned int)numDrawSurfaces) {
    dsurface_t *ds = &drawSurfaces[geomID];
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
        // We'll need to retrieve them from the geometry.
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

Loads the node structure out of a .bsp file to be used for light occlusion
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

//==========================================================================================

/*
================
SphereCull
================
*/
qboolean SphereCull(vec3_t start, vec3_t stop, vec3_t origin, float radius) {
  vec3_t v;
  float d;
  vec3_t dir;
  float len;
  vec3_t on;

  VectorSubtract(stop, start, dir);
  len = VectorNormalize(dir, dir);

  VectorSubtract(origin, start, v);
  d = DotProduct(v, dir);
  if (d > len + radius) {
    return qtrue; // too far ahead
  }
  if (d < -radius) {
    return qtrue; // too far behind
  }
  VectorMA(start, d, dir, on);

  VectorSubtract(on, origin, v);

  len = VectorLength(v);

  if (len > radius) {
    return qtrue; // too far to the side
  }

  return qfalse; // must be traced against
}

/*
================
TraceAgainstSurface
================
*/
void TraceAgainstSurface(traceWork_t *tw, surfaceTest_t *surf) {
  int i;

  // if surfaces are trans
  if (!bruteTrace) {
    if (SphereCull(tw->start, tw->end, surf->origin, surf->radius)) {
      if (numthreads == 1) {
        c_cullTrace++;
      }
      return;
    }
  }

  if (numthreads == 1) {
    c_testTrace++;
    c_testFacets += surf->numFacets;
  }

  /*
  // MrE: backface culling
  if (!surf->patch && surf->numFacets) {
          // if the surface does not cast an alpha shadow
          if ( !(surf->shader->surfaceFlags & SURF_ALPHASHADOW) ) {
                  vec3_t vec;
                  VectorSubtract(tw->end, tw->start, vec);
                  if (DotProduct(vec, surf->facets->surface) > 0)
                          return;
          }
  }
  */

  // test against each facet
  for (i = 0; i < surf->numFacets; i++) {
    TraceAgainstFacet(tw, surf->shader, surf->facets + i);
  }
}

/*
=============
TraceLine_Surface_r

Recursive traversal that collects ALL leaves along the ray (ignoring Solid/Empty)
=============
*/
void TraceLine_Surface_r(int node, const vec3_t start, const vec3_t stop,
                         traceWork_t *tw) {
  dnode_t *dnode;
  dplane_t *dplane;
  double d1, d2, frac;
  int side;
  vec3_t mid;

  if (node < 0) {
    // save the leaf number for surface testing
    if (tw->numOpenLeafs == MAX_MAP_LEAFS) {
      return;
    }
    tw->openLeafNumbers[tw->numOpenLeafs] = -node - 1;
    tw->numOpenLeafs++;
    return;
  }

  dnode = &dnodes[node];
  dplane = &dplanes[dnode->planeNum];

  int type = PlaneTypeForNormal(dplane->normal);
  if (type <= PLANE_Z) {
    d1 = (double)start[type] - dplane->dist;
    d2 = (double)stop[type] - dplane->dist;
  } else {
    d1 = ((double)start[0] * dplane->normal[0] +
          (double)start[1] * dplane->normal[1] +
          (double)start[2] * dplane->normal[2]) -
         dplane->dist;
    d2 = ((double)stop[0] * dplane->normal[0] +
          (double)stop[1] * dplane->normal[1] +
          (double)stop[2] * dplane->normal[2]) -
         dplane->dist;
  }

  if (d1 >= TRACE_EPSILON && d2 >= TRACE_EPSILON) {
    TraceLine_Surface_r(dnode->children[0], start, stop, tw);
    return;
  }

  if (d1 <= -TRACE_EPSILON && d2 <= -TRACE_EPSILON) {
    TraceLine_Surface_r(dnode->children[1], start, stop, tw);
    return;
  }

  side = d1 < 0;

  frac = d1 / (d1 - d2);

  mid[0] = start[0] + (stop[0] - start[0]) * frac;
  mid[1] = start[1] + (stop[1] - start[1]) * frac;
  mid[2] = start[2] + (stop[2] - start[2]) * frac;

  TraceLine_Surface_r(dnode->children[side], start, mid, tw);
  TraceLine_Surface_r(dnode->children[!side], mid, stop, tw);
}

/*
=============
TraceLine_Surface

New mesh-only tracing mode (q3map2 style)
=============
*/
void TraceLine_Surface(const vec3_t start, const vec3_t stop, trace_t *trace,
                       qboolean testAll, traceWork_t *tw) {
  int i, j;
  dleaf_t *leaf;
  float oldHitFrac;
  surfaceTest_t *test;
  int surfaceNum;
  byte surfaceTested[MAX_MAP_DRAW_SURFS / 8];

  if (numthreads == 1) {
    c_totalTrace++;
  }

  // assume all light gets through
  trace->filter[0] = 1.0;
  trace->filter[1] = 1.0;
  trace->filter[2] = 1.0;

  VectorCopy(start, tw->start);
  VectorCopy(stop, tw->end);
  tw->trace = trace;

  tw->numOpenLeafs = 0;

  trace->passSolid = qfalse;
  trace->hitFraction = 1.0;

  // collect ALL leaves along the ray (including those BSP thinks are solid)
  TraceLine_Surface_r(0, start, stop, tw);

  if (!tw->numOpenLeafs) {
    return;
  }

  memset(surfaceTested, 0, (numDrawSurfaces + 7) / 8);
  oldHitFrac = trace->hitFraction;

  if (bruteTrace) {
    for (surfaceNum = 0; surfaceNum < numDrawSurfaces; surfaceNum++) {
      test = surfaceTest[surfaceNum];
      if (!test)
        continue;
      if (!tw->patchshadows && test->patch)
        continue;
      TraceAgainstSurface(tw, test);
    }
  } else {
    for (i = 0; i < tw->numOpenLeafs; i++) {
      leaf = &dleafs[tw->openLeafNumbers[i]];
      for (j = 0; j < leaf->numLeafSurfaces; j++) {
        surfaceNum = dleafsurfaces[leaf->firstLeafSurface + j];

        // make sure we don't test the same ray against a surface more than once
        if (surfaceTested[surfaceNum >> 3] & (1 << (surfaceNum & 7))) {
          continue;
        }
        surfaceTested[surfaceNum >> 3] |= (1 << (surfaceNum & 7));

        test = surfaceTest[surfaceNum];
        if (!test) {
          continue;
        }

        if (!tw->patchshadows && test->patch) {
          continue;
        }

        TraceAgainstSurface(tw, test);
      }

      if (!testAll && trace->hitFraction < oldHitFrac) {
        trace->passSolid = qtrue;
        break;
      }
    }
  }

  for (i = 0; i < 3; i++) {
    trace->hit[i] = start[i] + (stop[i] - start[i]) * trace->hitFraction;
  }
}

/*
=============
TraceLine_Embree

High-performance Embree tracing path
=============
*/
static void TraceLine_Embree(const vec3_t start, const vec3_t stop,
                             trace_t *trace, traceWork_t *tw) {
  int i;
  struct RTCRayHit rayhit;
  struct MyRayQueryContext context;
  rtcInitRayQueryContext(&context.context);
  context.tw = tw;

  rayhit.ray.org_x = start[0];
  rayhit.ray.org_y = start[1];
  rayhit.ray.org_z = start[2];
  rayhit.ray.dir_x = stop[0] - start[0];
  rayhit.ray.dir_y = stop[1] - start[1];
  rayhit.ray.dir_z = stop[2] - start[2];
  rayhit.ray.tnear = 0.0001f;
  rayhit.ray.tfar = 1.0f;
  rayhit.ray.mask = 0xFFFFFFFF;
  rayhit.ray.flags = 0;
  rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
  rayhit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

  struct RTCIntersectArguments iargs;
  rtcInitIntersectArguments(&iargs);
  iargs.context = &context.context;

  rtcIntersect1(g_scene, &rayhit, &iargs);

  trace->filter[0] = 1.0;
  trace->filter[1] = 1.0;
  trace->filter[2] = 1.0;
  trace->passSolid = (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID);
  trace->hitFraction = (rayhit.hit.geomID == RTC_INVALID_GEOMETRY_ID) ? 1.0f : rayhit.ray.tfar;

  for (i = 0; i < 3; i++) {
    trace->hit[i] = start[i] + (stop[i] - start[i]) * trace->hitFraction;
  }
}

/*

/*
=============
*/
void TraceLine(const vec3_t start, const vec3_t stop, trace_t *trace,
               qboolean testAll, traceWork_t *tw) {
  if (embree) {
    TraceLine_Embree(start, stop, trace, tw);
    return;
  }

  TraceLine_Surface(start, stop, trace, testAll, tw);
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
