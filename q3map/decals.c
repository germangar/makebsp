/*
===========================================================================
decals.c
_decal entity processing
===========================================================================
*/

#include "qbsp.h"

int numDecalProjectors = 0;
decalProjector_t decalProjectors[MAX_DECAL_PROJECTORS];

/*
================
FindTargetEntityByName
================
*/
static entity_t *FindTargetEntityByName(const char *target)
{
    int i;
    for (i = 0; i < num_entities; i++)
    {
        const char *n = ValueForKey(&entities[i], "targetname");
        if (!strcmp(n, target))
            return &entities[i];
    }
    return NULL;
}

/*
================
MakeTextureMatrix
Builds dp->texMat[2][4] from the 3 triangle verts projected onto the projection plane
Uses barycentric coordinate math
================
*/
static qboolean MakeTextureMatrix(decalProjector_t *dp, vec3_t projNormal, float projDist,
                                   drawVert_t *a, drawVert_t *b, drawVert_t *c)
{
    float bb;
    float sWeight[3], tWeight[3];
    int i, j;
    vec3_t pa, pb, pc;
    vec3_t xyz_s, xyz_t;
    vec3_t vec_s, vec_t;

    // 1. Project points onto projection plane
    VectorCopy(a->xyz, pa);
    VectorMA(pa, -(DotProduct(pa, projNormal) - projDist), projNormal, pa);
    
    VectorCopy(b->xyz, pb);
    VectorMA(pb, -(DotProduct(pb, projNormal) - projDist), projNormal, pb);
    
    VectorCopy(c->xyz, pc);
    VectorMA(pc, -(DotProduct(pc, projNormal) - projDist), projNormal, pc);

    // 2. Calculate barycentric basis denominator (bb)
    bb = (b->st[0] - a->st[0]) * (c->st[1] - a->st[1]) - (c->st[0] - a->st[0]) * (b->st[1] - a->st[1]);
    
    if (fabs(bb) < 1e-8f) 
    {
        return qfalse;  // Degenerate UVs
    }
    
    // 3. Find S-vector: bary weights for (s=a.st[0]+1, t=a.st[1])
    sWeight[1] = (c->st[1] - a->st[1]) / bb;
    sWeight[2] = -(b->st[1] - a->st[1]) / bb;
    sWeight[0] = 1.0f - sWeight[1] - sWeight[2];
    
    // 4. Find T-vector: bary weights for (s=a.st[0], t=a.st[1]+1)
    tWeight[1] = -(c->st[0] - a->st[0]) / bb;
    tWeight[2] = (b->st[0] - a->st[0]) / bb;
    tWeight[0] = 1.0f - tWeight[1] - tWeight[2];

    // 5. Calculate XYZ space tangent/bitangent vectors
    for (i = 0; i < 3; i++)
    {
        xyz_s[i] = sWeight[0] * pa[i] + sWeight[1] * pb[i] + sWeight[2] * pc[i];
        xyz_t[i] = tWeight[0] * pa[i] + tWeight[1] * pb[i] + tWeight[2] * pc[i];
    }
    
    VectorSubtract(xyz_s, pa, vec_s);
    VectorSubtract(xyz_t, pa, vec_t);

    // 6. Invert length squared to convert from dP/dS to dS/dP
    {
        float lenS = VectorNormalize(vec_s, vec_s);
        float lenT = VectorNormalize(vec_t, vec_t);
        
        for (j = 0; j < 3; j++)
        {
            dp->texMat[0][j] = (lenS != 0.0f) ? (vec_s[j] / lenS) : 0.0f;
            dp->texMat[1][j] = (lenT != 0.0f) ? (vec_t[j] / lenT) : 0.0f;
        }
    }

    // 7. Calculate translation component
    dp->texMat[0][3] = a->st[0] - DotProduct(pa, dp->texMat[0]);
    dp->texMat[1][3] = a->st[1] - DotProduct(pa, dp->texMat[1]);
    
    return qtrue;
}

/*
================
MakeDecalProjector
================
*/
static void MakeDecalProjector(shaderInfo_t *si, vec3_t projNormal, float distance,
                                drawVert_t *a, drawVert_t *b, drawVert_t *c, int decalEntityNum)
{
    decalProjector_t *dp;
    vec3_t backPoint;
    vec3_t verts[3];
    int i;
    vec3_t extent;

    if (numDecalProjectors >= MAX_DECAL_PROJECTORS) 
    {
        _printf("WARNING: MAX_DECAL_PROJECTORS reached\n");
        return;
    }
    
    dp = &decalProjectors[numDecalProjectors];
    memset(dp, 0, sizeof(*dp));
    dp->si = si;
    dp->decalEntityNum = decalEntityNum;
    dp->numPlanes = 5;
    
    VectorCopy(a->xyz, verts[0]);
    VectorCopy(b->xyz, verts[1]);
    VectorCopy(c->xyz, verts[2]);
    
    // 1. Front plane (coplanar with triangle)
    vec4_t p;
    PlaneFromPoints(p, verts[0], verts[1], verts[2]);
    VectorCopy(p, dp->planes[0].normal);
    dp->planes[0].dist = p[3];
    
    // 2. Back plane (parallel to front, pushed by 'distance' along projection vector)
    VectorMA(a->xyz, distance, projNormal, backPoint);
    VectorCopy(dp->planes[0].normal, dp->planes[1].normal);
    VectorNegate(dp->planes[1].normal);
    dp->planes[1].dist = DotProduct(backPoint, dp->planes[1].normal);
    
    // 3. Three side planes (triangle edges extruded along projection vector)
    VectorCopy(a->xyz, verts[0]);
    VectorCopy(b->xyz, verts[1]);
    VectorCopy(c->xyz, verts[2]);

    for (i = 0; i < 3; i++) 
    {
        vec3_t extruded;
        VectorMA(verts[i], distance, projNormal, extruded);
        vec4_t p;
        PlaneFromPoints(p, verts[(i+1)%3], verts[i], extruded);
        VectorCopy(p, dp->planes[i + 2].normal);
        dp->planes[i + 2].dist = p[3];
    }
    
    // 4. Bounding box & sphere from 6 points (3 verts + 3 projected verts)
    ClearBounds(dp->mins, dp->maxs);
    for (i = 0; i < 3; i++) 
    {
        vec3_t proj;
        AddPointToBounds(verts[i], dp->mins, dp->maxs);
        VectorMA(verts[i], distance, projNormal, proj);
        AddPointToBounds(proj, dp->mins, dp->maxs);
    }
    
    // center = midpoint of AABB, radius = distance from center to corner
    VectorAdd(dp->mins, dp->maxs, dp->center);
    VectorScale(dp->center, 0.5f, dp->center);
    VectorSubtract(dp->maxs, dp->center, extent);
    dp->radius = VectorLength(extent);
    
    // 5. Texture matrix
    if (!MakeTextureMatrix(dp, projNormal, DotProduct(a->xyz, projNormal), a, b, c)) 
    {
        _printf("WARNING: Degenerate decal triangle UVs, skipping projector\n");
        return;
    }
    
    numDecalProjectors++;
}

/*
================
ProcessDecals
Phase A
================
*/
void ProcessDecals(void)
{
    int i, y, x;
    parseMesh_t *pm;
    bspbrush_t *b;
    
    _printf("--- ProcessDecals ---\n");
    numDecalProjectors = 0;
    
    for (i = 0; i < num_entities; i++)
    {
        entity_t *e = &entities[i];
        const char *classname = ValueForKey(e, "classname");
        vec3_t entityOrigin;
        const char *targetName;
        entity_t *targetEnt = NULL;

        if (strcmp(classname, "_decal") != 0) 
            continue;
        
        GetVectorForKey(e, "origin", entityOrigin);
        targetName = ValueForKey(e, "target");
        if (targetName[0])
            targetEnt = FindTargetEntityByName(targetName);
            
        if (!e->patches)
        {
            _printf("WARNING: Decal entity without any patch meshes, ignoring.\n");
            continue;
        }
            
        // 2. Extract triangles from entity's patches
        for (pm = e->patches; pm; pm = pm->next)
        {
            vec3_t patchCenter, mins, maxs;
            vec3_t origin;
            vec3_t projNormal;
            float distance = 64.0f;
            int j;
            
            ClearBounds(mins, maxs);
            for (j = 0; j < pm->mesh.width * pm->mesh.height; j++)
                AddPointToBounds(pm->mesh.verts[j].xyz, mins, maxs);
            VectorAdd(mins, maxs, patchCenter);
            VectorScale(patchCenter, 0.5f, patchCenter);

            if (VectorCompare(entityOrigin, vec3_origin)) {
                VectorCopy(patchCenter, origin);
            } else {
                VectorCopy(entityOrigin, origin);
            }

            if (targetEnt)
            {
                vec3_t targetOrigin;
                GetVectorForKey(targetEnt, "origin", targetOrigin);
                VectorSubtract(targetOrigin, origin, projNormal);
                distance = VectorLength(projNormal);
                if (distance > 0.0f)
                    VectorNormalize(projNormal, projNormal);
                else
                {
                    VectorSet(projNormal, 0, 0, -1);
                    distance = 64.0f;
                }
            }
            else
            {
                VectorSet(projNormal, 0, 0, -1);
                distance = 64.0f;
            }

            mesh_t *tess = SubdivideMesh(pm->mesh, 8.0f, 999.0f);
            for (y = 0; y < tess->height - 1; y++) 
            {
                for (x = 0; x < tess->width - 1; x++) 
                {
                    int r = (x + y) & 1;
                    drawVert_t *pw[5], *idx[4];
                    
                    pw[0] = &tess->verts[y * tess->width + x];
                    pw[1] = &tess->verts[(y+1) * tess->width + x];
                    pw[2] = &tess->verts[(y+1) * tess->width + x + 1];
                    pw[3] = &tess->verts[y * tess->width + x + 1];
                    pw[4] = pw[0];
                    
                    idx[0] = pw[r + 0];
                    idx[1] = pw[r + 1];
                    idx[2] = pw[r + 2];
                    idx[3] = pw[r + 3];

                    MakeDecalProjector(pm->shaderInfo, projNormal, distance, idx[0], idx[1], idx[2], i);
                    MakeDecalProjector(pm->shaderInfo, projNormal, distance, idx[0], idx[2], idx[3], i);
                }
            }
            FreeMesh(tess);
        }

        
        // 5. Delete entity geometry so it's not compiled into BSP
        // Free brushes
        {
            bspbrush_t *next;
            for (b = e->brushes; b; b = next)
            {
                next = b->next;
                FreeBrush(b);
            }
        }
        e->brushes = NULL;

        // Free patches
        {
            parseMesh_t *next_pm;
            for (pm = e->patches; pm; pm = next_pm)
            {
                next_pm = pm->next;
                free(pm->mesh.verts); // Free Mesh verts (mesh itself is embedded in pm)
                free(pm);
            }
        }
        e->patches = NULL;
    }
    
    // 6. Delete target_position entities that are ONLY targeted by _decal entities
    for (i = 0; i < num_entities; i++)
    {
        entity_t *tEnt = &entities[i];
        const char *tClass = ValueForKey(tEnt, "classname");
        const char *tName;
        int j;
        qboolean targetedByNonDecal = qfalse;
        qboolean targetedByDecal = qfalse;

        if (strcmp(tClass, "target_position") != 0)
            continue;

        tName = ValueForKey(tEnt, "targetname");
        if (!tName[0])
            continue;

        // Check who targets this entity
        for (j = 0; j < num_entities; j++)
        {
            entity_t *src = &entities[j];
            const char *srcTarget = ValueForKey(src, "target");
            const char *srcClass;

            if (srcTarget[0] && !strcmp(srcTarget, tName))
            {
                srcClass = ValueForKey(src, "classname");
                if (strcmp(srcClass, "_decal") == 0)
                    targetedByDecal = qtrue;
                else
                    targetedByNonDecal = qtrue;
            }
        }

        if (targetedByDecal && !targetedByNonDecal)
        {
            // Only targeted by decals, delete it
            epair_t *ep, *next;
            for (ep = tEnt->epairs; ep; ep = next)
            {
                next = ep->next;
                free(ep->key);
                free(ep->value);
                free(ep);
            }
            tEnt->epairs = NULL;
        }
    }
    
    _printf("%d decal projectors created\n", numDecalProjectors);
}

/*
================
TransformDecalProjector
================
*/
static void TransformDecalProjector(const decalProjector_t *in, vec3_t entityOrigin,
                                     decalProjector_t *out)
{
    int i;
    *out = *in; 
    
    VectorSubtract(in->mins, entityOrigin, out->mins);
    VectorSubtract(in->maxs, entityOrigin, out->maxs);
    VectorSubtract(in->center, entityOrigin, out->center);
    
    for (i = 0; i < in->numPlanes; i++) 
    {
        VectorCopy(in->planes[i].normal, out->planes[i].normal);
        out->planes[i].dist = in->planes[i].dist - DotProduct(in->planes[i].normal, entityOrigin);
    }
    
    for (i = 0; i < 2; i++) 
    {
        out->texMat[i][3] = in->texMat[i][3]
            + in->texMat[i][0] * entityOrigin[0]
            + in->texMat[i][1] * entityOrigin[1]
            + in->texMat[i][2] * entityOrigin[2];
    }
}

/*
================
ClipWindingEpsilonStrict
================
*/
static winding_t *ClipWindingEpsilonStrict(winding_t *in, vec3_t normal, vec_t dist)
{
    winding_t *front, *back;
    ClipWindingEpsilon(in, normal, dist, 0.001f, &front, &back);
    if (front) FreeWinding(front);
    return back;
}

/*
================
ProjectDecalOntoWinding
================
*/
static void ProjectDecalOntoWinding(decalProjector_t *dp, mapDrawSurface_t *ds,
                                     winding_t *w)
{
    int p, i;
    mapDrawSurface_t *newDs;
    int planenum;
    side_t *dummySide;
    vec4_t plane;
    vec3_t surfNormal;

    if (w->numpoints < 3 || !PlaneFromPoints(plane, w->points[0], w->points[1], w->points[2]))
    {
        FreeWinding(w);
        return;
    }
    VectorCopy(plane, surfNormal);

    // 1. Backface cull
    if (DotProduct(dp->planes[0].normal, surfNormal) < -0.0001f)
    {
        FreeWinding(w);
        return;
    }
    
    // 2. Clip against all 5 frustum planes
    for (p = 0; p < dp->numPlanes; p++)
    {
        winding_t *clipped = ClipWindingEpsilonStrict(w, dp->planes[p].normal, dp->planes[p].dist);
        FreeWinding(w);
        if (!clipped) return; // Completely outside
        w = clipped;
    }
    
    // 3. Emit the surviving winding as a new mapDrawSurface_t
    newDs = AllocDrawSurf();
    newDs->isDecal = qtrue;
    if (dp->si && dp->si->hasVertexColor)
    {
        newDs->hasVertexColor = 1;
        VectorCopy(dp->si->vertexColor, newDs->vertexColor);
    }
    else
    {
        newDs->hasVertexColor = -1;
    }
    newDs->shaderInfo = dp->si;
    newDs->numVerts = w->numpoints;
    newDs->verts = malloc(w->numpoints * sizeof(drawVert_t));
    memset(newDs->verts, 0, w->numpoints * sizeof(drawVert_t));
    newDs->mapBrush = NULL;
    
    newDs->samplesize = ds->samplesize;
    if (dp->si->lightmapSampleSize > 0)
        newDs->samplesize = dp->si->lightmapSampleSize;
    newDs->enforceSampleSize = ds->enforceSampleSize;

    if (ds->side) 
    {
        newDs->side = ds->side;
    } 
    else 
    {
        planenum = FindFloatPlane(surfNormal, DotProduct(w->points[0], surfNormal));
        dummySide = malloc(sizeof(side_t));
        memset(dummySide, 0, sizeof(side_t));
        dummySide->planenum = planenum;
        newDs->side = dummySide;
    }
    
    for (i = 0; i < w->numpoints; i++)
    {
        drawVert_t *dv = &newDs->verts[i];
        float d, d2, alpha;

        VectorCopy(w->points[i], dv->xyz);
        
        dv->st[0] = DotProduct(dv->xyz, dp->texMat[0]) + dp->texMat[0][3];
        dv->st[1] = DotProduct(dv->xyz, dp->texMat[1]) + dp->texMat[1][3];
        
        VectorCopy(surfNormal, dv->normal);
        
        d  = DotProduct(dp->planes[0].normal, w->points[i]) - dp->planes[0].dist;
        d2 = DotProduct(dp->planes[1].normal, w->points[i]) - dp->planes[1].dist;
        float absD = (float)fabs(d);
        float absD2 = (float)fabs(d2);
        alpha = 255.0f; // Force to 255 to prove if alpha was 0
        
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 255.0f) alpha = 255.0f;
        
        dv->color[0][0] = 255;
        dv->color[0][1] = 255;
        dv->color[0][2] = 255;
        // dv->color[0][3] = (byte)alpha;
    }
    
    FreeWinding(w);
}

/*
================
InitDecalMesh
================
*/
void InitDecalMesh(decalMesh_t *m)
{
    m->maxVerts = DECAL_MESH_INITIAL_VERTS;
    m->numVerts = 0;
    m->verts = malloc(m->maxVerts * sizeof(drawVert_t));
    
    m->maxIndexes = DECAL_MESH_INITIAL_INDEXES;
    m->numIndexes = 0;
    m->indexes = malloc(m->maxIndexes * sizeof(int));
}

/*
================
FreeDecalMesh
================
*/
void FreeDecalMesh(decalMesh_t *m)
{
    if (m->verts) free(m->verts);
    if (m->indexes) free(m->indexes);
    memset(m, 0, sizeof(*m));
}

/*
================
AddWindingToDecalMesh
================
*/
static void AddWindingToDecalMesh(decalProjector_t *dp, winding_t *w, decalMesh_t *m)
{
    vec4_t plane;
    vec3_t surfNormal;
    int p, i;
    int newTriangles;
    int baseVert;

    if (w->numpoints < 3 || !PlaneFromPoints(plane, w->points[0], w->points[1], w->points[2]))
    {
        FreeWinding(w);
        return;
    }
    VectorCopy(plane, surfNormal);
    
    if (DotProduct(dp->planes[0].normal, surfNormal) < -0.0001f)
    {
        FreeWinding(w);
        return;
    }
    
    for (p = 0; p < dp->numPlanes; p++)
    {
        winding_t *clipped = ClipWindingEpsilonStrict(w, dp->planes[p].normal, dp->planes[p].dist);
        FreeWinding(w);
        if (!clipped) return;
        w = clipped;
    }
    
    while (m->numVerts + w->numpoints > m->maxVerts)
    {
        m->maxVerts *= 2;
        m->verts = realloc(m->verts, m->maxVerts * sizeof(drawVert_t));
    }
    newTriangles = w->numpoints - 2;
    while (m->numIndexes + newTriangles * 3 > m->maxIndexes)
    {
        m->maxIndexes *= 2;
        m->indexes = realloc(m->indexes, m->maxIndexes * sizeof(int));
    }
    
    baseVert = m->numVerts;
    for (i = 0; i < w->numpoints; i++)
    {
        drawVert_t *dv = &m->verts[m->numVerts++];
        memset(dv, 0, sizeof(*dv));
        VectorCopy(w->points[i], dv->xyz);
        VectorCopy(surfNormal, dv->normal);
        
        dv->st[0] = DotProduct(dv->xyz, dp->texMat[0]) + dp->texMat[0][3];
        dv->st[1] = DotProduct(dv->xyz, dp->texMat[1]) + dp->texMat[1][3];
        
        dv->lightmap[0][0] = dv->st[0];
        dv->lightmap[0][1] = dv->st[1];
        
        dv->color[0][0] = 255;
        dv->color[0][1] = 255;
        dv->color[0][2] = 255;
        dv->color[0][3] = 255;
    }
    
    for (i = 0; i < newTriangles; i++)
    {
        m->indexes[m->numIndexes++] = baseVert;
        m->indexes[m->numIndexes++] = baseVert + i + 1;
        m->indexes[m->numIndexes++] = baseVert + i + 2;
    }
    
    FreeWinding(w);
}

/*
================
WeldDecalMesh
================
*/
void WeldDecalMesh(decalMesh_t *m, float epsilon)
{
    int *remap = malloc(m->numVerts * sizeof(int));
    int uniqueVerts = 0;
    int i, j;
    int newNumIndexes;
    
    for (i = 0; i < m->numVerts; i++)
    {
        int match = -1;
        for (j = 0; j < uniqueVerts; j++)
        {
            if (fabs(m->verts[i].xyz[0] - m->verts[j].xyz[0]) < epsilon &&
                fabs(m->verts[i].xyz[1] - m->verts[j].xyz[1]) < epsilon &&
                fabs(m->verts[i].xyz[2] - m->verts[j].xyz[2]) < epsilon)
            {
                match = j;
                break;
            }
        }
        
        if (match == -1)
        {
            remap[i] = uniqueVerts;
            if (uniqueVerts != i)
                m->verts[uniqueVerts] = m->verts[i];
            uniqueVerts++;
        }
        else
        {
            remap[i] = match;
        }
    }
    
    for (i = 0; i < m->numIndexes; i++)
    {
        m->indexes[i] = remap[m->indexes[i]];
    }
    
    m->numVerts = uniqueVerts;
    free(remap);
    
    newNumIndexes = 0;
    for (i = 0; i < m->numIndexes; i += 3)
    {
        int i0 = m->indexes[i];
        int i1 = m->indexes[i + 1];
        int i2 = m->indexes[i + 2];
        if (i0 != i1 && i1 != i2 && i0 != i2)
        {
            m->indexes[newNumIndexes++] = i0;
            m->indexes[newNumIndexes++] = i1;
            m->indexes[newNumIndexes++] = i2;
        }
    }
    m->numIndexes = newNumIndexes;
}

/*
================
ExtrudeDecalMesh
================
*/
void ExtrudeDecalMesh(decalMesh_t *m, float distance)
{
    vec3_t *smoothNormals = malloc(m->numVerts * sizeof(vec3_t));
    memset(smoothNormals, 0, m->numVerts * sizeof(vec3_t));
    int i;
    
    for (i = 0; i < m->numIndexes; i += 3)
    {
        int i0 = m->indexes[i];
        int i1 = m->indexes[i + 1];
        int i2 = m->indexes[i + 2];
        
        vec3_t e1, e2, faceNormal;
        VectorSubtract(m->verts[i1].xyz, m->verts[i0].xyz, e1);
        VectorSubtract(m->verts[i2].xyz, m->verts[i0].xyz, e2);
        
        CrossProduct(e1, e2, faceNormal);
        
        VectorAdd(smoothNormals[i0], faceNormal, smoothNormals[i0]);
        VectorAdd(smoothNormals[i1], faceNormal, smoothNormals[i1]);
        VectorAdd(smoothNormals[i2], faceNormal, smoothNormals[i2]);
    }
    
    for (i = 0; i < m->numVerts; i++)
    {
        vec3_t n;
        float len;
        VectorCopy(smoothNormals[i], n);
        
        len = VectorNormalize(n, n);
        if (len > 0.0001f)
        {
            if (DotProduct(n, m->verts[i].normal) < 0.0f) {
                VectorNegate(n);
            }
            VectorMA(m->verts[i].xyz, distance, n, m->verts[i].xyz);
        }
        else
        {
            VectorMA(m->verts[i].xyz, distance, m->verts[i].normal, m->verts[i].xyz);
        }
    }
    
    free(smoothNormals);
}

/*
================
EmitDecalMeshAsMiscModel
================
*/
static void EmitDecalMeshAsMiscModel(decalMesh_t *m, decalProjector_t *dp, mapDrawSurface_t *templateDs)
{
    mapDrawSurface_t *newDs;

    if (m->numIndexes < 3) return;
    
    newDs = AllocDrawSurf();
    newDs->isDecal = qtrue;
    newDs->miscModel = qtrue;
    newDs->shaderInfo = dp->si;
    newDs->side = NULL;
    newDs->mapBrush = NULL;
    newDs->lightmapScale = 1.0f;
    
    newDs->enforceSampleSize = templateDs->enforceSampleSize;
    
    newDs->samplesize = dp->si->lightmapSampleSize > 0 
                        ? dp->si->lightmapSampleSize 
                        : templateDs->samplesize;
    
    newDs->numVerts = m->numVerts;
    newDs->verts = malloc(m->numVerts * sizeof(drawVert_t));
    memcpy(newDs->verts, m->verts, m->numVerts * sizeof(drawVert_t));
    
    newDs->numIndexes = m->numIndexes;
    newDs->indexes = malloc(m->numIndexes * sizeof(int));
    memcpy(newDs->indexes, m->indexes, m->numIndexes * sizeof(int));
}

/*
================
MakeEntityDecals
================
*/
void MakeEntityDecals(entity_t *e)
{
    int i, s, v, d;
    vec3_t entityOrigin;
    int initialSurfs;

    if (numDecalProjectors == 0) return;
    
    GetVectorForKey(e, "origin", entityOrigin);
    
    initialSurfs = numMapDrawSurfs; 
    
    // Group projectors by their parent decal entity
    for (d = 0; d < num_entities; d++)
    {
        entity_t *decalEnt = &entities[d];
        const char *classname = ValueForKey(decalEnt, "classname");
        decalMesh_t decalTrisoup;
        int firstProjectorIndex = -1;
        
        if (strcmp(classname, "_decal") != 0) continue;
        
        InitDecalMesh(&decalTrisoup);
        
        for (i = 0; i < numDecalProjectors; i++)
        {
            decalProjector_t localDp;
            
            if (decalProjectors[i].decalEntityNum != d) continue;
            if (firstProjectorIndex == -1) firstProjectorIndex = i;
            
            TransformDecalProjector(&decalProjectors[i], entityOrigin, &localDp);
            
            for (s = e->firstDrawSurf; s < initialSurfs; s++)
            {
                mapDrawSurface_t *ds = &mapDrawSurfs[s];
                vec3_t dsMins, dsMaxs;

                if (!ds->numVerts) continue;
                if (ds->isDecal) continue;
                if (ds->shaderInfo->autosprite) continue;
                if (ds->shaderInfo->surfaceFlags & SURF_NOMARKS) continue;
                
                ClearBounds(dsMins, dsMaxs);
                for (v = 0; v < ds->numVerts; v++)
                    AddPointToBounds(ds->verts[v].xyz, dsMins, dsMaxs);
                
                if (localDp.mins[0] > dsMaxs[0] || localDp.maxs[0] < dsMins[0] ||
                    localDp.mins[1] > dsMaxs[1] || localDp.maxs[1] < dsMins[1] ||
                    localDp.mins[2] > dsMaxs[2] || localDp.maxs[2] < dsMins[2])
                    continue;
                
                if (ds->patch)
                {
                    mesh_t srcMesh;
                    mesh_t *tess;
                    int x, y;
                    
                    srcMesh.width = ds->patchWidth;
                    srcMesh.height = ds->patchHeight;
                    srcMesh.verts = ds->verts;
                    
                    tess = SubdivideMesh(srcMesh, 0.1f, 999.0f);
                    for (y = 0; y < tess->height - 1; y++) 
                    {
                        for (x = 0; x < tess->width - 1; x++) 
                        {
                            int r = (x + y) & 1;
                            drawVert_t *pw[5], *idx[4];
                            winding_t *w1, *w2;
                            
                            pw[0] = &tess->verts[y * tess->width + x];
                            pw[1] = &tess->verts[(y+1) * tess->width + x];
                            pw[2] = &tess->verts[(y+1) * tess->width + x + 1];
                            pw[3] = &tess->verts[y * tess->width + x + 1];
                            pw[4] = pw[0];
                            
                            idx[0] = pw[r + 0];
                            idx[1] = pw[r + 1];
                            idx[2] = pw[r + 2];
                            idx[3] = pw[r + 3];
                            
                            w1 = AllocWinding(3);
                            w1->numpoints = 3;
                            VectorCopy(idx[0]->xyz, w1->points[0]);
                            VectorCopy(idx[1]->xyz, w1->points[1]);
                            VectorCopy(idx[2]->xyz, w1->points[2]);
                            AddWindingToDecalMesh(&localDp, w1, &decalTrisoup);
                            
                            w2 = AllocWinding(3);
                            w2->numpoints = 3;
                            VectorCopy(idx[0]->xyz, w2->points[0]);
                            VectorCopy(idx[2]->xyz, w2->points[1]);
                            VectorCopy(idx[3]->xyz, w2->points[2]);
                            AddWindingToDecalMesh(&localDp, w2, &decalTrisoup);
                        }
                    }
                    FreeMesh(tess);
                }
                else
                {
                    int j;
                    if (ds->numIndexes > 0)
                    {
                        for (j = 0; j < ds->numIndexes; j += 3)
                        {
                            winding_t *w = AllocWinding(3);
                            w->numpoints = 3;
                            VectorCopy(ds->verts[ds->indexes[j]].xyz, w->points[0]);
                            VectorCopy(ds->verts[ds->indexes[j+1]].xyz, w->points[1]);
                            VectorCopy(ds->verts[ds->indexes[j+2]].xyz, w->points[2]);
                            AddWindingToDecalMesh(&localDp, w, &decalTrisoup);
                        }
                    }
                    else
                    {
                        winding_t *w = AllocWinding(ds->numVerts);
                        w->numpoints = ds->numVerts;
                        for (j = 0; j < ds->numVerts; j++)
                            VectorCopy(ds->verts[j].xyz, w->points[j]);
                        AddWindingToDecalMesh(&localDp, w, &decalTrisoup);
                    }
                }
            }
        }
        
        if (decalTrisoup.numIndexes >= 3 && firstProjectorIndex != -1)
        {
            mapDrawSurface_t *templateDs = &mapDrawSurfs[0];
            if (initialSurfs > e->firstDrawSurf)
                templateDs = &mapDrawSurfs[e->firstDrawSurf];
            
            WeldDecalMesh(&decalTrisoup, 0.25f);
            ExtrudeDecalMesh(&decalTrisoup, 1.0f);
            EmitDecalMeshAsMiscModel(&decalTrisoup, &decalProjectors[firstProjectorIndex], templateDs);
        }
        
        FreeDecalMesh(&decalTrisoup);
    }
}
