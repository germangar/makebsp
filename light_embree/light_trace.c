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

#define CURVE_FACET_ERROR 8
#define TRACE_EPSILON 0.001

RTCDevice g_device = NULL;
RTCScene g_scene = NULL;

struct MyRayQueryContext {
  struct RTCRayQueryContext context;
  traceWork_t *tw;
};

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
  int i, j;
  float t;
  float m[3][4];
  float s;

  // This is an incredibly stupid way of solving a three variable equation
  for (i = 0; i < 2; i++) {

    m[0][0] = a->xyz[0];
    m[0][1] = a->xyz[1];
    m[0][2] = a->xyz[2];
    m[0][3] = a->st[i];

    m[1][0] = b->xyz[0];
    m[1][1] = b->xyz[1];
    m[1][2] = b->xyz[2];
    m[1][3] = b->st[i];

    m[2][0] = c->xyz[0];
    m[2][1] = c->xyz[1];
    m[2][2] = c->xyz[2];
    m[2][3] = c->st[i];

    if (fabs(m[1][0]) > fabs(m[0][0]) && fabs(m[1][0]) > fabs(m[2][0])) {
      for (j = 0; j < 4; j++) {
        t = m[0][j];
        m[0][j] = m[1][j];
        m[1][j] = t;
      }
    } else if (fabs(m[2][0]) > fabs(m[0][0]) && fabs(m[2][0]) > fabs(m[1][0])) {
      for (j = 0; j < 4; j++) {
        t = m[0][j];
        m[0][j] = m[2][j];
        m[2][j] = t;
      }
    }

    s = 1.0 / m[0][0];
    m[0][0] *= s;
    m[0][1] *= s;
    m[0][2] *= s;
    m[0][3] *= s;

    s = m[1][0];
    m[1][0] -= m[0][0] * s;
    m[1][1] -= m[0][1] * s;
    m[1][2] -= m[0][2] * s;
    m[1][3] -= m[0][3] * s;

    s = m[2][0];
    m[2][0] -= m[0][0] * s;
    m[2][1] -= m[0][1] * s;
    m[2][2] -= m[0][2] * s;
    m[2][3] -= m[0][3] * s;

    if (fabs(m[2][1]) > fabs(m[1][1])) {
      for (j = 0; j < 4; j++) {
        t = m[1][j];
        m[1][j] = m[2][j];
        m[2][j] = t;
      }
    }

    s = 1.0 / m[1][1];
    m[1][0] *= s;
    m[1][1] *= s;
    m[1][2] *= s;
    m[1][3] *= s;

    s = m[2][1];
    m[2][0] -= m[1][0] * s;
    m[2][1] -= m[1][1] * s;
    m[2][2] -= m[1][2] * s;
    m[2][3] -= m[1][3] * s;

    s = 1.0 / m[2][2];
    m[2][0] *= s;
    m[2][1] *= s;
    m[2][2] *= s;
    m[2][3] *= s;

    f->textureMatrix[i][2] = m[2][3];
    f->textureMatrix[i][1] = m[1][3] - f->textureMatrix[i][2] * m[1][2];
    f->textureMatrix[i][0] = m[0][3] - f->textureMatrix[i][2] * m[0][2] -
                             f->textureMatrix[i][1] * m[0][1];

    f->textureMatrix[i][3] = 0;
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
  }
}

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
===============
SphereFromBounds
===============
*/
void SphereFromBounds(vec3_t mins, vec3_t maxs, vec3_t origin, float *radius) {
  vec3_t temp;

  VectorAdd(mins, maxs, origin);
  VectorScale(origin, 0.5, origin);
  VectorSubtract(maxs, origin, temp);
  *radius = VectorLength(temp);
}

/*
====================
FacetsForTriangleSurface
====================
*/
void FacetsForTriangleSurface(dsurface_t *dsurf, shaderInfo_t *si,
                              surfaceTest_t *test) {
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

  // we may have turned some pairs into quads
  test->numFacets = count;
}

/*
====================
FacetsForPatch
====================
*/
void FacetsForPatch(dsurface_t *dsurf, shaderInfo_t *si, surfaceTest_t *test) {
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
  test->numFacets = count;
  FreeMesh(subdivided);
}

/*
=====================
InitSurfacesForTesting

Builds structures to speed the ray tracing against surfaces
=====================
*/
void InitSurfacesForTesting(void) {

  int i, j;
  dsurface_t *dsurf;
  surfaceTest_t *test;
  drawVert_t *dvert;
  shaderInfo_t *si;

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
      FacetsForTriangleSurface(dsurf, si, test);
    } else if (dsurf->surfaceType == MST_PATCH) {
      FacetsForPatch(dsurf, si, test);
    }
  }
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
void SetFacetFilter(traceWork_t *tr, shaderInfo_t *shader, cFacet_t *facet,
                    vec3_t point) {
  float s, t;
  int is, it;
  byte *image;
  int b;

  // most surfaces are completely opaque
  if (!(shader->surfaceFlags & SURF_ALPHASHADOW)) {
    VectorClear(tr->trace->filter);
    return;
  }

  s = DotProduct(point, facet->textureMatrix[0]) + facet->textureMatrix[0][3];
  t = DotProduct(point, facet->textureMatrix[1]) + facet->textureMatrix[1][3];

  if (!shader->pixels) {
    // assume completely solid
    VectorClear(point);
    return;
  }

  s = s - floor(s);
  t = t - floor(t);

  is = s * shader->width;
  it = t * shader->height;

  image = shader->pixels + 4 * (it * shader->width + is);

  // alpha filter
  b = image[3];

  // alpha test makes this a binary option
  b = b < 128 ? 0 : 255;

  tr->trace->filter[0] = tr->trace->filter[0] * (255 - b) / 255;
  tr->trace->filter[1] = tr->trace->filter[1] * (255 - b) / 255;
  tr->trace->filter[2] = tr->trace->filter[2] * (255 - b) / 255;
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
  if (d2 > -SELF_SHADOW_EPSILON && d2 < SELF_SHADOW_EPSILON) {
    return; // don't self intersect
  }

  // calculate the intersection fraction
  f = d1 / (d1 - d2);
  if (f < TRACE_EPSILON) {
    return;
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

typedef struct tnode_s {
  int type;
  vec3_t normal;
  float dist;
  int children[2];
  int planeNum;
} tnode_t;

#define MAX_TNODES (MAX_MAP_NODES * 4)
tnode_t *tnodes, *tnode_p;

/*
==============
MakeTnode

Converts the disk node structure into the efficient tracing structure
==============
*/
void MakeTnode(int nodenum) {
  tnode_t *t;
  dplane_t *plane;
  int i;
  dnode_t *node;
  int leafNum;

  t = tnode_p++;

  node = dnodes + nodenum;
  plane = dplanes + node->planeNum;

  t->planeNum = node->planeNum;
  t->type = PlaneTypeForNormal(plane->normal);
  VectorCopy(plane->normal, t->normal);
  t->dist = plane->dist;

  for (i = 0; i < 2; i++) {
    if (node->children[i] < 0) {
      leafNum = -node->children[i] - 1;
      if (dleafs[leafNum].cluster == -1) {
        // solid
        t->children[i] = leafNum | (1 << 31) | (1 << 30);
      } else {
        t->children[i] = leafNum | (1 << 31);
      }
    } else {
      t->children[i] = tnode_p - tnodes;
      MakeTnode(node->children[i]);
    }
  }
}

/*
=============
AlphaFilter

Embree intersection filter for handling ignoreSurface and alpha shadows
=============
*/
void AlphaFilter(const struct RTCFilterFunctionNArguments *args) {
  if (args->valid[0] != -1) return;
  struct MyRayQueryContext *mcontext = (struct MyRayQueryContext *)args->context;
  traceWork_t *tw = mcontext->tw;
  struct RTCHit *hit = (struct RTCHit *)args->hit;

  if (tw && hit->geomID == (unsigned int)tw->ignoreSurface) {
    // Only ignore self-intersection for flat planar surfaces.
    // Models (Triangle Soup) and Patches need self-shadowing.
    if (drawSurfaces[hit->geomID].surfaceType == MST_PLANAR) {
      args->valid[0] = 0;
    }
  }
}

/*
=============
InitTrace

Loads the node structure out of a .bsp file to be used for light occlusion
=============
*/
void InitTrace(void) {
  int i, j;
  dsurface_t *dsurf;

  // 32 byte align the structs
  tnodes = malloc((MAX_TNODES + 1) * sizeof(tnode_t));
  tnodes = (tnode_t *)(((size_t)tnodes + 31) & ~31);
  tnode_p = tnodes;

  MakeTnode(0);

  // Embree 4 initialization
  g_device = rtcNewDevice(NULL);
  if (!g_device) {
    Error("Embree: Failed to create device\n");
  }
  g_scene = rtcNewScene(g_device);

  _printf("--- Embree Initializing ---\n");

  int count = 0;
  for (i = 0; i < numDrawSurfaces; i++) {
    dsurf = &drawSurfaces[i];
    if (dsurf->numIndexes > 0 &&
        (dsurf->surfaceType == MST_TRIANGLE_SOUP ||
         dsurf->surfaceType == MST_PLANAR)) {
      RTCGeometry geom = rtcNewGeometry(g_device, RTC_GEOMETRY_TYPE_TRIANGLE);

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

/*
===================
PointInSolid
===================
*/
qboolean PointInSolid_r(vec3_t start, int node) {
  tnode_t *tnode;
  double front;

  while (!(node & (1 << 31))) {
    tnode = &tnodes[node];
    switch (tnode->type) {
    case PLANE_X:
      front = (double)start[0] - tnode->dist;
      break;
    case PLANE_Y:
      front = (double)start[1] - tnode->dist;
      break;
    case PLANE_Z:
      front = (double)start[2] - tnode->dist;
      break;
    default:
      front = ((double)start[0] * tnode->normal[0] +
               (double)start[1] * tnode->normal[1] +
               (double)start[2] * tnode->normal[2]) -
              tnode->dist;
      break;
    }

    if (front > -TRACE_EPSILON && front < TRACE_EPSILON) {
      // exactly on node, must check both sides
      return (qboolean)(PointInSolid_r(start, tnode->children[0]) |
                        PointInSolid_r(start, tnode->children[1]));
    }

    if (front >= TRACE_EPSILON) {
      node = tnode->children[0];
    } else {
      node = tnode->children[1];
    }
  }

  if (node & (1 << 30)) {
    return qtrue;
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
=============
TraceLine_r

Returns qtrue if something is hit and tracing can stop
=============
*/
int TraceLine_r(int node, const vec3_t start, const vec3_t stop,
                traceWork_t *tw) {
  tnode_t *tnode;
  double d1, d2, frac;
  int side;
  vec3_t mid;
  int r;

  if (node & (1 << 31)) {
    if (node & (1 << 30)) {
      VectorCopy(start, tw->trace->hit);
      tw->trace->passSolid = qtrue;
      return qtrue;
    } else {
      // save the node off for more exact testing
      if (tw->numOpenLeafs == MAX_MAP_LEAFS) {
        return qfalse;
      }
      tw->openLeafNumbers[tw->numOpenLeafs] = node & ~(3 << 30);
      tw->numOpenLeafs++;
      return qfalse;
    }
  }

  tnode = &tnodes[node];

  switch (tnode->type) {
  case PLANE_X:
    d1 = (double)start[0] - tnode->dist;
    d2 = (double)stop[0] - tnode->dist;
    break;
  case PLANE_Y:
    d1 = (double)start[1] - tnode->dist;
    d2 = (double)stop[1] - tnode->dist;
    break;
  case PLANE_Z:
    d1 = (double)start[2] - tnode->dist;
    d2 = (double)stop[2] - tnode->dist;
    break;
  default:
    d1 = ((double)start[0] * tnode->normal[0] +
          (double)start[1] * tnode->normal[1] +
          (double)start[2] * tnode->normal[2]) -
         tnode->dist;
    d2 = ((double)stop[0] * tnode->normal[0] +
          (double)stop[1] * tnode->normal[1] +
          (double)stop[2] * tnode->normal[2]) -
         tnode->dist;
    break;
  }

  if (d1 >= TRACE_EPSILON && d2 >= TRACE_EPSILON) {
    return TraceLine_r(tnode->children[0], start, stop, tw);
  }

  if (d1 <= -TRACE_EPSILON && d2 <= -TRACE_EPSILON) {
    return TraceLine_r(tnode->children[1], start, stop, tw);
  }

  side = d1 < 0;

  frac = d1 / (d1 - d2);

  mid[0] = start[0] + (stop[0] - start[0]) * frac;
  mid[1] = start[1] + (stop[1] - start[1]) * frac;
  mid[2] = start[2] + (stop[2] - start[2]) * frac;

  r = TraceLine_r(tnode->children[side], start, mid, tw);

  if (r) {
    return r;
  }

  return TraceLine_r(tnode->children[!side], mid, stop, tw);
}

/*
=============
TraceLine

Uses legacy BSP traversal for solid brushes/leaves and Embree for surfaces.
=============
*/
void TraceLine(const vec3_t start, const vec3_t stop, trace_t *trace,
               qboolean testAll, traceWork_t *tw) {
  int r;
  struct RTCRayHit rayhit;
  struct RTCIntersectArguments iargs;
  struct MyRayQueryContext mcontext;
  vec3_t dir;
  float len;

  if (numthreads == 1) {
    c_totalTrace++;
  }

  // assume all light gets through, unless the ray crosses
  // a translucent surface
  trace->filter[0] = 1.0;
  trace->filter[1] = 1.0;
  trace->filter[2] = 1.0;

  VectorCopy(start, tw->start);
  VectorCopy(stop, tw->end);
  tw->trace = trace;

  tw->numOpenLeafs = 0;

  trace->passSolid = qfalse;
  trace->hitFraction = 1.0;
  r = qfalse;

  if (oldTrace) {
    // 1. Legacy BSP traversal for solid brushes and opaque clusters
    // --- Legacy Junction Fix (q3map2 style) ---
    // We shift the start point by 1.25 units to avoid being trapped
    // inside intersecting geometry at junctions.
    {
      vec3_t dir;
      float len;
      vec3_t shiftedStart;

      VectorSubtract(stop, start, dir);
      len = VectorNormalize(dir, dir);

      if (len > SELF_SHADOW_EPSILON) {
        VectorMA(start, SELF_SHADOW_EPSILON, dir, shiftedStart);
        r = TraceLine_r(0, shiftedStart, stop, tw);
        if (r) {
          // Adjust hitFraction to be relative to the ORIGINAL start point
          float hitDist = (trace->hitFraction * (len - SELF_SHADOW_EPSILON)) +
                          SELF_SHADOW_EPSILON;
          trace->hitFraction = hitDist / len;
        }
      } else {
        r = TraceLine_r(0, start, stop, tw);
      }
    }
  }

  // If we hit a solid leaf and we don't need all hits, we can stop here
  if (r && !testAll) {
    return;
  }

  if (noSurfaces || !g_scene) {
    return;
  }

  // 2. Embree trace for all draw surfaces (faces, patches, meshes)
  VectorSubtract(stop, start, dir);
  len = VectorLength(dir);
  if (len < 0.001f) {
    return;
  }
  VectorScale(dir, 1.0 / len, dir);

  // Initialize ray
  rayhit.ray.org_x = start[0];
  rayhit.ray.org_y = start[1];
  rayhit.ray.org_z = start[2];
  rayhit.ray.dir_x = dir[0];
  rayhit.ray.dir_y = dir[1];
  rayhit.ray.dir_z = dir[2];
  rayhit.ray.tnear = SELF_SHADOW_EPSILON;
  rayhit.ray.tfar = len * trace->hitFraction; // Use hitFraction from BSP test
  rayhit.ray.mask = -1;
  rayhit.ray.flags = 0;
  rayhit.ray.time = 0;

  // Initialize hit
  rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
  rayhit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

  // Set up intersection arguments with context for ignoreSurface
  rtcInitRayQueryContext(&mcontext.context);
  mcontext.tw = tw;

  rtcInitIntersectArguments(&iargs);
  iargs.context = &mcontext.context;

  // Perform the trace
  rtcIntersect1(g_scene, &rayhit, &iargs);

  if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
    trace->hitFraction = rayhit.ray.tfar / len;
    trace->passSolid = qtrue;
    VectorClear(trace->filter); // For now, treat any surface hit as opaque
  }

  for (int i = 0; i < 3; i++) {
    trace->hit[i] = start[i] + (stop[i] - start[i]) * trace->hitFraction;
  }
}
