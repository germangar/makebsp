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
along with Foobar; if not, write to the Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "qbsp.h"

#define DUPLICATE_EPSILON  0.1f   // world-space vertex match tolerance
#define AUTOCAULK_EPSILON  0.1f   // world-space vertex match tolerance

qboolean noautocaulk = qfalse;

/*
==================
BrushesAreEqual
==================
*/
static qboolean BrushesAreEqual(bspbrush_t *b1, bspbrush_t *b2)
{
    int s1, s2;
    int matchedSides = 0;

    if (b1->numsides != b2->numsides)
        return qfalse;

    // Compare bounding boxes
    for (int i = 0; i < 3; i++)
    {
        if (fabs(b1->mins[i] - b2->mins[i]) > DUPLICATE_EPSILON ||
            fabs(b1->maxs[i] - b2->maxs[i]) > DUPLICATE_EPSILON)
        {
            return qfalse;
        }
    }

    // Compare all sides
    for (s1 = 0; s1 < b1->numsides; s1++)
    {
        side_t *side1 = &b1->sides[s1];
        qboolean sideMatched = qfalse;

        for (s2 = 0; s2 < b2->numsides; s2++)
        {
            side_t *side2 = &b2->sides[s2];

            if (side1->planenum != side2->planenum) continue;
            if (side1->bevel != side2->bevel) continue;
            
            // Only enforce shader and content flags for non-bevel sides
            if (!side1->bevel)
            {
                if (side1->shaderInfo != side2->shaderInfo) continue;
                if (side1->surfaceFlags != side2->surfaceFlags) continue;
                
                // Mask out CONTENTS_DETAIL and CONTENTS_STRUCTURAL flags to allow comparing structural vs detail
                int c1 = side1->contents & ~(CONTENTS_DETAIL | CONTENTS_STRUCTURAL);
                int c2 = side2->contents & ~(CONTENTS_DETAIL | CONTENTS_STRUCTURAL);
                if (c1 != c2) continue;
            }

            sideMatched = qtrue;
            break;
        }

        if (sideMatched)
        {
            matchedSides++;
        }
        else
        {
            return qfalse;
        }
    }

    return (matchedSides == b1->numsides);
}

/*
==================
FilterDuplicateBrushes
==================
*/
void FilterDuplicateBrushes(void)
{
    int numDuplicates = 0;

    _printf("--- FilterDuplicateBrushes ---\n");

    for (int i = 0; i < num_entities; i++)
    {
        entity_t *ent = &entities[i];
        
        // Skip entities with 0 or 1 brush
        if (ent->brushes == NULL || ent->brushes->next == NULL)
            continue;

        bspbrush_t *prev = NULL;
        bspbrush_t *b1 = ent->brushes;

        while (b1 != NULL)
        {
            // Only filter solid, opaque brushes
            if (!(b1->contents & CONTENTS_SOLID) || !b1->opaque || (b1->contents & CONTENTS_TRANSLUCENT))
            {
                prev = b1;
                b1 = b1->next;
                continue;
            }

            bspbrush_t *b2_prev = b1;
            bspbrush_t *b2 = b1->next;
            qboolean b1_deleted = qfalse;

            while (b2 != NULL)
            {
                // Only compare solid, opaque brushes
                if ((b2->contents & CONTENTS_SOLID) && b2->opaque && !(b2->contents & CONTENTS_TRANSLUCENT))
                {
                    if (BrushesAreEqual(b1, b2))
                    {
                        // Duplicate found. Decide which one to keep.
                        // If one is detail and the other is structural, keep structural, delete detail.
                        if (b1->detail && !b2->detail)
                        {
                            // Keep b2 (structural), delete b1 (detail)
                            bspbrush_t *dup = b1;
                            
                            if (prev == NULL)
                                ent->brushes = b1->next;
                            else
                                prev->next = b1->next;
                            
                            b1 = b1->next;
                            FreeBrush(dup);
                            numDuplicates++;
                            b1_deleted = qtrue;
                            break; // b1 is gone, break inner loop to continue outer loop
                        }
                        else
                        {
                            // Keep b1, delete b2 (if both are detail, both structural, or b1 structural and b2 detail)
                            bspbrush_t *dup = b2;
                            b2_prev->next = b2->next;
                            b2 = b2->next;
                            
                            FreeBrush(dup);
                            numDuplicates++;
                            continue;
                        }
                    }
                }
                
                b2_prev = b2;
                if (b2 != NULL)
                {
                    b2 = b2->next;
                }
            }

            if (!b1_deleted)
            {
                prev = b1;
                b1 = b1->next;
            }
        }
    }

    if (numDuplicates > 0)
    {
        _printf("%i duplicate brushes removed\n", numDuplicates);
    }
}

// ---------------------------------------------------------------------------
// IsFuncStatic(entity_t *e)
// Returns qtrue if the entity's classname is "func_static".
// ---------------------------------------------------------------------------
static qboolean IsFuncStatic(entity_t *e)
{
    return !Q_stricmp("func_static", ValueForKey(e, "classname"));
}

// ---------------------------------------------------------------------------
// IsDynamic(entity_t *e)
// Returns qtrue for any entity that can move/disappear at runtime
// (func_door, func_plat, func_rotating, etc.) and is NOT func_static.
// ---------------------------------------------------------------------------
static qboolean IsDynamic(entity_t *e)
{
    const char *cn = ValueForKey(e, "classname");
    // worldspawn already merged func_group/func_light — skip
    if (!Q_stricmp(cn, "worldspawn")) return qfalse;
    if (!Q_stricmp(cn, "func_static")) return qfalse;
    return qtrue; // everything else is potentially dynamic
}

// ---------------------------------------------------------------------------
// CanCaulkSideA(entA, entB)
// Returns qtrue if the face belonging to entA may be caulked
// when it touches a face from entB.
// ---------------------------------------------------------------------------
static qboolean CanCaulkSideA(entity_t *entA, entity_t *entB)
{
    // Case 1: Both are worldspawn — mutual caulk, sideA is safe
    if (entA == &entities[0] && entB == &entities[0])
        return qtrue;

    // Case 2: sideA is func_static touching worldspawn — caulk sideA only
    if (IsFuncStatic(entA) && entB == &entities[0])
        return qtrue;

    // Case 3: Same dynamic entity — caulk within self
    if (IsDynamic(entA) && entA == entB)
        return qtrue;

    return qfalse;
}

// ---------------------------------------------------------------------------
// WindingsMatch(wA, wB)
// Set-based point test. Both windings are CCW w.r.t. their outward normals,
// so they traverse the shared polygon in reverse order relative to each other.
// We do NOT rely on index ordering — only on point set equality.
// ---------------------------------------------------------------------------
static qboolean WindingsMatch(winding_t *wA, winding_t *wB)
{
    int i, j;
    qboolean found;

    if (wA->numpoints != wB->numpoints)
        return qfalse;

    for (i = 0; i < wA->numpoints; i++)
    {
        found = qfalse;
        for (j = 0; j < wB->numpoints; j++)
        {
            if (fabs(wA->points[i][0] - wB->points[j][0]) < AUTOCAULK_EPSILON &&
                fabs(wA->points[i][1] - wB->points[j][1]) < AUTOCAULK_EPSILON &&
                fabs(wA->points[i][2] - wB->points[j][2]) < AUTOCAULK_EPSILON)
            {
                found = qtrue;
                break;
            }
        }
        if (!found)
            return qfalse;
    }
    return qtrue;
}

// ---------------------------------------------------------------------------
// WindingContainedInWinding(wA, wB, normalB)
// Returns qtrue if all points of wA lie inside (or on the boundary of) wB.
// normalB is the outward normal of plane B (mapplanes[sideB->planenum].normal).
// ---------------------------------------------------------------------------
static qboolean WindingContainedInWinding(winding_t *wA, winding_t *wB, vec3_t normalB)
{
    int i, j;
    vec3_t edge, outwardNormal, diff;
    vec_t dist;

    for (j = 0; j < wB->numpoints; j++)
    {
        vec3_t *v0 = &wB->points[j];
        vec3_t *v1 = &wB->points[(j + 1) % wB->numpoints];

        VectorSubtract(*v1, *v0, edge);
        CrossProduct(normalB, edge, outwardNormal);
        if (VectorNormalize(outwardNormal, outwardNormal) == 0)
            continue;

        for (i = 0; i < wA->numpoints; i++)
        {
            VectorSubtract(wA->points[i], *v0, diff);
            dist = DotProduct(diff, outwardNormal);
            // If dist > AUTOCAULK_EPSILON, point i of wA is outside edge j of wB
            if (dist > AUTOCAULK_EPSILON)
            {
                return qfalse;
            }
        }
    }

    return qtrue;
}

// ---------------------------------------------------------------------------
// WindingInsideBrush(side, brush)
// Returns qtrue if all points of side->winding lie inside solid opaque brush.
// We must ensure side does not lie on a boundary plane of brush pointing
// OUTWARD in the same direction (i.e. side->planenum == brush->sides[k].planenum),
// because if it lies on an outward boundary plane of brush, it faces outward into
// open air rather than being occluded by brush.
// ---------------------------------------------------------------------------
static qboolean WindingInsideBrush(side_t *side, bspbrush_t *brush)
{
    int        i, k;
    winding_t *w = side->winding;
    vec_t      dist;
    qboolean   onOutwardBoundary = qfalse;

    if (w == NULL || brush == NULL)
        return qfalse;

    for (k = 0; k < brush->numsides; k++)
    {
        side_t  *bSide = &brush->sides[k];
        plane_t *plane;

        if (bSide->bevel)
            continue;

        plane = &mapplanes[bSide->planenum];

        // Check if all points of winding are on or inside side k of brush
        for (i = 0; i < w->numpoints; i++)
        {
            dist = DotProduct(w->points[i], plane->normal) - plane->dist;
            if (dist > AUTOCAULK_EPSILON)
            {
                return qfalse;
            }
        }

        // Check if this side is coplanar and facing the same way as side k of brush
        if (side->planenum == bSide->planenum)
        {
            qboolean allOnPlane = qtrue;
            for (i = 0; i < w->numpoints; i++)
            {
                dist = DotProduct(w->points[i], plane->normal) - plane->dist;
                if (fabs(dist) > AUTOCAULK_EPSILON)
                {
                    allOnPlane = qfalse;
                    break;
                }
            }
            if (allOnPlane)
            {
                onOutwardBoundary = qtrue;
            }
        }
    }

    if (onOutwardBoundary)
        return qfalse;

    return qtrue;
}

// ---------------------------------------------------------------------------
// AutoCaulkBrushes — Main entry point
// Called from main() in bsp.c AFTER LoadMapFile(), BEFORE ProcessModels().
// ---------------------------------------------------------------------------
void AutoCaulkBrushes(void)
{
    shaderInfo_t *caulkShader;
    int           i, j, sA, sB;
    int           caulkedFaces = 0;
    qboolean      aInB, bInA;

    if (noautocaulk)
    {
        _printf("early face auto-caulking disabled\n");
        return;
    }

    caulkShader = ShaderInfoForShader("textures/common/caulk");
    if (caulkShader == NULL)
    {
        _printf("WARNING: AutoCaulkBrushes: textures/common/caulk not found\n");
        return;
    }

    _printf("--- AutoCaulkBrushes ---\n");

    for (i = 0; i < num_entities; i++)
    {
        entity_t *entA = &entities[i];
        bspbrush_t  *brushA;

        for (brushA = entA->brushes; brushA; brushA = brushA->next)
        {
            // Skip transparent brushes (don't caulk them, nor any face looking at them)
            if (!brushA->opaque || (brushA->contents & CONTENTS_TRANSLUCENT)) continue;

            for (sA = 0; sA < brushA->numsides; sA++)
            {
                side_t *sideA = &brushA->sides[sA];

                if (sideA->bevel) continue;
                if (!sideA->winding) continue;

                // Skip non-solid, fog, and liquid faces
                if (!(sideA->contents & CONTENTS_SOLID)) continue;
                if (sideA->contents & (CONTENTS_FOG | CONTENTS_WATER | CONTENTS_LAVA | CONTENTS_SLIME)) continue;

                // Skip transparent faces
                if (sideA->contents & CONTENTS_TRANSLUCENT) continue;
                if (sideA->shaderInfo && (sideA->shaderInfo->contents & CONTENTS_TRANSLUCENT)) continue;

                // Inner loops: entity B, brush B, side B
                // Start j from i to avoid processing the same pair twice
                for (j = i; j < num_entities; j++)
                {
                    entity_t *entB = &entities[j];
                    bspbrush_t  *brushB;

                    for (brushB = (j == i ? brushA->next : entB->brushes);
                         brushB; brushB = brushB->next)
                    {
                        // Skip transparent brushes (don't caulk them, nor any face looking at them)
                        if (!brushB->opaque || (brushB->contents & CONTENTS_TRANSLUCENT)) continue;

                        // --- AABB early-out ---
                        if (brushA->maxs[0] < brushB->mins[0] - AUTOCAULK_EPSILON ||
                            brushA->mins[0] > brushB->maxs[0] + AUTOCAULK_EPSILON ||
                            brushA->maxs[1] < brushB->mins[1] - AUTOCAULK_EPSILON ||
                            brushA->mins[1] > brushB->maxs[1] + AUTOCAULK_EPSILON ||
                            brushA->maxs[2] < brushB->mins[2] - AUTOCAULK_EPSILON ||
                            brushA->mins[2] > brushB->maxs[2] + AUTOCAULK_EPSILON)
                            continue;

                        // --- Case 3: Face A entirely inside Brush B ---
                        if (!(sideA->surfaceFlags & (SURF_NODRAW | SURF_SKIP)) && CanCaulkSideA(entA, entB))
                        {
                            if (WindingInsideBrush(sideA, brushB))
                            {
                                sideA->shaderInfo   = caulkShader;
                                sideA->surfaceFlags = caulkShader->surfaceFlags;
                                sideA->contents     = caulkShader->contents;
                                caulkedFaces++;
                            }
                        }

                        for (sB = 0; sB < brushB->numsides; sB++)
                        {
                            side_t *sideB = &brushB->sides[sB];

                            if (sideB->bevel) continue;
                            if (!sideB->winding) continue;
                            if (((sideA->surfaceFlags & (SURF_NODRAW | SURF_SKIP)) && (sideB->surfaceFlags & (SURF_NODRAW | SURF_SKIP)))) continue;

                            // Skip non-solid, fog, and liquid faces
                            if (!(sideB->contents & CONTENTS_SOLID)) continue;
                            if (sideB->contents & (CONTENTS_FOG | CONTENTS_WATER | CONTENTS_LAVA | CONTENTS_SLIME)) continue;

                            // Skip transparent faces
                            if (sideB->contents & CONTENTS_TRANSLUCENT) continue;
                            if (sideB->shaderInfo && (sideB->shaderInfo->contents & CONTENTS_TRANSLUCENT)) continue;

                            // --- Case 3: Face B entirely inside Brush A ---
                            if (!(sideB->surfaceFlags & (SURF_NODRAW | SURF_SKIP)) && CanCaulkSideA(entB, entA))
                            {
                                if (WindingInsideBrush(sideB, brushA))
                                {
                                    sideB->shaderInfo   = caulkShader;
                                    sideB->surfaceFlags = caulkShader->surfaceFlags;
                                    sideB->contents     = caulkShader->contents;
                                    caulkedFaces++;
                                    continue;
                                }
                            }

                            // --- Exact plane opposite test (integer, no float error) ---
                            if (sideA->planenum != (sideB->planenum ^ 1))
                                continue;

                            // --- Polygon containment / match test ---
                            if (WindingsMatch(sideA->winding, sideB->winding))
                            {
                                aInB = qtrue;
                                bInA = qtrue;
                            }
                            else
                            {
                                aInB = WindingContainedInWinding(sideA->winding, sideB->winding, mapplanes[sideB->planenum].normal);
                                bInA = WindingContainedInWinding(sideB->winding, sideA->winding, mapplanes[sideA->planenum].normal);
                            }

                            if (!aInB && !bInA)
                                continue;

                            // --- Entity compatibility & apply caulk ---
                            if (aInB && CanCaulkSideA(entA, entB) && !(sideA->surfaceFlags & (SURF_NODRAW | SURF_SKIP)))
                            {
                                sideA->shaderInfo   = caulkShader;
                                sideA->surfaceFlags = caulkShader->surfaceFlags;
                                sideA->contents     = caulkShader->contents;
                                caulkedFaces++;
                            }
                            if (bInA && CanCaulkSideA(entB, entA) && !(sideB->surfaceFlags & (SURF_NODRAW | SURF_SKIP)))
                            {
                                sideB->shaderInfo   = caulkShader;
                                sideB->surfaceFlags = caulkShader->surfaceFlags;
                                sideB->contents     = caulkShader->contents;
                                caulkedFaces++;
                            }
                        }
                    }
                }
            }
        }
    }

    _printf("%5i faces auto-caulked\n", caulkedFaces);
}
