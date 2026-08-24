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
#include <stddef.h>

// if a brush just barely pokes onto the other side,
// let it slide by without chopping
#define PLANESIDE_EPSILON 0.001
// 0.1

/*
================
CountBrushList
================
*/
int CountBrushList(bspbrush_t *brushes)
{
    int c;

    c = 0;
    for (; brushes; brushes = brushes->next)
        c++;
    return c;
}

/*
================
AllocBrush
================
*/
bspbrush_t *AllocBrush(int numsides)
{
    bspbrush_t *bb;
    int c;

    c = (size_t)&(((bspbrush_t *)0)->sides[numsides]);
    bb = malloc(c);
    memset(bb, 0, c);
    bb->original = bb;
    return bb;
}

/*
================
FreeBrush
================
*/
void FreeBrush(bspbrush_t *brushes)
{
    int i;

    for (i = 0; i < brushes->numsides; i++)
        if (brushes->sides[i].winding)
            FreeWinding(brushes->sides[i].winding);
    FreeEpairs(brushes->epairs);
    free(brushes);
}

/*
================
FreeBrushList
================
*/
void FreeBrushList(bspbrush_t *brushes)
{
    bspbrush_t *next;

    for (; brushes; brushes = next)
    {
        next = brushes->next;

        FreeBrush(brushes);
    }
}

/*
==================
CopyBrush

Duplicates the brush, the sides, and the windings
==================
*/
bspbrush_t *CopyBrush(bspbrush_t *brush)
{
    bspbrush_t *newbrush;
    int size;
    int i;

    size = (size_t)&(((bspbrush_t *)0)->sides[brush->numsides]);

    newbrush = AllocBrush(brush->numsides);
    memcpy(newbrush, brush, size);
    newbrush->epairs = CopyEpairs(brush->epairs);

    for (i = 0; i < brush->numsides; i++)
    {
        if (brush->sides[i].winding)
            newbrush->sides[i].winding = CopyWinding(brush->sides[i].winding);
    }

    return newbrush;
}



/*
=============
PrintBrush
=============
*/
void PrintBrush(bspbrush_t *brush)
{
    int i;

    _printf("brush: %p\n", brush);
    for (i = 0; i < brush->numsides; i++)
    {
        pw(brush->sides[i].winding);
        _printf("\n");
    }
}

/*
==================
BoundBrush

Sets the mins/maxs based on the windings
returns false if the brush doesn't enclose a valid volume
==================
*/
qboolean BoundBrush(bspbrush_t *brush)
{
    int i, j;
    winding_t *w;

    ClearBounds(brush->mins, brush->maxs);
    for (i = 0; i < brush->numsides; i++)
    {
        w = brush->sides[i].winding;
        if (!w)
            continue;
        for (j = 0; j < w->numpoints; j++)
            AddPointToBounds(w->points[j], brush->mins, brush->maxs);
    }

    for (i = 0; i < 3; i++)
    {
        if (brush->mins[i] < MIN_WORLD_COORD || brush->maxs[i] > MAX_WORLD_COORD ||
            brush->mins[i] >= brush->maxs[i])
        {
            return qfalse;
        }
    }

    return qtrue;
}

/*
==================
CreateBrushWindings

makes basewindigs for sides and mins / maxs for the brush
returns false if the brush doesn't enclose a valid volume
==================
*/
qboolean CreateBrushWindings(bspbrush_t *brush)
{
    int i, j;
    winding_t *w;
    side_t *side;
    plane_t *plane;

    for (i = 0; i < brush->numsides; i++)
    {
        side = &brush->sides[i];
        // don't create a winding for a bevel
        if (side->bevel)
        {
            continue;
        }
        plane = &mapplanes[side->planenum];
        w = BaseWindingForPlane(plane->normal, plane->dist);
        for (j = 0; j < brush->numsides && w; j++)
        {
            if (i == j)
                continue;
            if (brush->sides[j].planenum == (brush->sides[i].planenum ^ 1))
                continue; // back side clipaway
            if (brush->sides[j].bevel)
                continue;
            if (brush->sides[j].backSide)
                continue;
            plane = &mapplanes[brush->sides[j].planenum ^ 1];
            ChopWindingInPlace(&w, plane->normal, plane->dist, 0); // CLIP_EPSILON);
        }
        // free any existing winding
        if (side->winding)
        {
            FreeWinding(side->winding);
        }
        side->winding = w;
    }

    return BoundBrush(brush);
}

/*
==================
BrushFromBounds

Creates a new axial brush
==================
*/
bspbrush_t *BrushFromBounds(vec3_t mins, vec3_t maxs)
{
    bspbrush_t *b;
    int i;
    vec3_t normal;
    vec_t dist;

    b = AllocBrush(6);
    b->numsides = 6;
    for (i = 0; i < 3; i++)
    {
        VectorClear(normal);
        normal[i] = 1;
        dist = maxs[i];
        b->sides[i].planenum = FindFloatPlane(normal, dist);

        normal[i] = -1;
        dist = -mins[i];
        b->sides[3 + i].planenum = FindFloatPlane(normal, dist);
    }

    CreateBrushWindings(b);

    return b;
}

/*
=================
AddBevelsToBrush

Adds any additional axial planes necessary to allow the brush being
built to be expanded against axial bounding boxes (player box traces).
=================
*/
bspbrush_t *AddBevelsToBrush(bspbrush_t *b)
{
    int i, j, k, l, m, a;
    int axis, dir, found, valid, axial;
    int refContents, refSurfFlags, originalSideCount, finalSides;
    vec3_t normal, edge, axisVec;
    float dist, d;
    side_t *s, *s2;
    plane_t *p;
    winding_t *w, *w2;
    bspbrush_t *tmp, *out;

    // 1. Allocate a staging buffer large enough to hold all bevels.
    //    MAX_BRUSH_SIDES (1024) is already defined and is generous enough.
    tmp = AllocBrush(MAX_BRUSH_SIDES);
    
    // Copy the fixed-size header fields
    memcpy(tmp, b, (size_t)&(((bspbrush_t *)0)->sides));
    tmp->numsides = b->numsides;
    
    // Copy original sides and their windings into tmp
    for (i = 0; i < b->numsides; i++) {
        tmp->sides[i] = b->sides[i];
        if (b->sides[i].winding)
            tmp->sides[i].winding = CopyWinding(b->sides[i].winding);
    }

    // Get reference contents/flags from the first non-bevel side
    refContents = b->sides[0].contents;
    refSurfFlags = b->sides[0].surfaceFlags;

    //--- PHASE 1: Axial Bevels ---
    // Add +/-X, +/-Y, +/-Z planes anchored at b->mins / b->maxs
    for (axis = 0; axis < 3; axis++) {
        for (dir = -1; dir <= 1; dir += 2) {
            // Check if this axial plane already exists
            found = 0;
            for (i = 0; i < tmp->numsides; i++) {
                p = &mapplanes[tmp->sides[i].planenum];
                if (p->normal[axis] == dir &&
                    p->normal[(axis+1)%3] == 0 &&
                    p->normal[(axis+2)%3] == 0) {
                    found = 1;
                    break;
                }
            }
            
            if (found) continue;
            
            if (tmp->numsides == MAX_BRUSH_SIDES) {
                _printf("WARNING: AddBevelsToBrush hit MAX_BRUSH_SIDES (axial)\n");
                break;
            }
            
            VectorClear(normal);
            normal[axis] = dir;
            dist = (dir == 1) ? b->maxs[axis] : -b->mins[axis];
            
            s = &tmp->sides[tmp->numsides++];
            memset(s, 0, sizeof(*s));
            s->planenum = FindFloatPlane(normal, dist);
            s->contents = refContents;
            s->surfaceFlags = refSurfFlags;
            s->bevel = qtrue;
            // No shaderInfo (convention: bevel sides never have shaderInfo)
        }
    }

    //--- PHASE 2: Edge Bevels ---
    // Only needed for brushes with more than 6 sides (non-pure-axial)
    if (tmp->numsides > 6) {
        // Iterate over all non-bevel sides that have windings
        originalSideCount = tmp->numsides; // freeze count before we add more
        for (i = 0; i < originalSideCount; i++) {
            s = &tmp->sides[i];
            if (s->bevel) continue;
            w = s->winding;
            if (!w) continue;

            for (j = 0; j < w->numpoints; j++) {
                k = (j + 1) % w->numpoints;
                VectorSubtract(w->points[j], w->points[k], edge);
                
                if (VectorNormalize(edge, edge) < 0.5f) continue;

                // Skip near-axial edges (they already have axial bevel coverage)
                SnapVector(edge);
                axial = 0;
                for (a = 0; a < 3; a++) {
                    if (edge[a] == -1 || edge[a] == 1) { 
                        axial = 1; 
                        break; 
                    }
                }
                
                if (axial) continue; // only test non-axial edges

                // Try the 6 possible slanted bevel planes from this edge
                for (axis = 0; axis < 3; axis++) {
                    for (dir = -1; dir <= 1; dir += 2) {
                        VectorClear(axisVec);
                        axisVec[axis] = dir;
                        
                        CrossProduct(edge, axisVec, normal);
                        
                        if (VectorNormalize(normal, normal) < 0.5f) continue;
                        
                        dist = DotProduct(w->points[j], normal);

                        // Verify all points on all non-bevel windings are behind this plane
                        valid = 1;
                        for (m = 0; m < tmp->numsides && valid; m++) {
                            // Dedup: skip if this plane already exists
                            if (PlaneEqual(&mapplanes[tmp->sides[m].planenum], normal, dist)) {
                                valid = 0; 
                                break;
                            }
                            
                            w2 = tmp->sides[m].winding;
                            if (!w2) continue;
                            
                            for (l = 0; l < w2->numpoints; l++) {
                                d = DotProduct(w2->points[l], normal) - dist;
                                if (d > 0.1f) { 
                                    valid = 0; 
                                    break; 
                                }
                            }
                        }
                        
                        if (!valid) continue;

                        if (tmp->numsides == MAX_BRUSH_SIDES) {
                            _printf("WARNING: AddBevelsToBrush hit MAX_BRUSH_SIDES (edge)\n");
                            goto done_edge_bevels;
                        }
                        
                        s2 = &tmp->sides[tmp->numsides++];
                        memset(s2, 0, sizeof(*s2));
                        s2->planenum = FindFloatPlane(normal, dist);
                        s2->contents = refContents;
                        s2->surfaceFlags = refSurfFlags;
                        s2->bevel = qtrue;
                    }
                }
            }
        }
        done_edge_bevels:;
    }

    // 3. Allocate a final, exactly-sized brush and transfer everything into it.
    //    Free the old brush (b) and the temp buffer (tmp windings are moved, not copied).
    finalSides = tmp->numsides;
    out = AllocBrush(finalSides);
    
    memcpy(out, tmp, (size_t)&(((bspbrush_t *)0)->sides));
    out->numsides = finalSides;
    out->original = out; // reset self-reference
    
    for (i = 0; i < finalSides; i++) {
        out->sides[i] = tmp->sides[i]; // winding pointers transferred
        tmp->sides[i].winding = NULL;  // prevent double-free in FreeBrush(tmp)
    }
    
    FreeBrush(tmp);
    FreeBrush(b); // free original input brush

    return out;
}

/*
==================
BrushVolume

==================
*/
vec_t BrushVolume(bspbrush_t *brush)
{
    int i;
    winding_t *w;
    vec3_t corner;
    vec_t d, area, volume;
    plane_t *plane;

    if (!brush)
        return 0;

    // grab the first valid point as the corner

    w = NULL;
    for (i = 0; i < brush->numsides; i++)
    {
        w = brush->sides[i].winding;
        if (w)
            break;
    }
    if (!w)
        return 0;
    VectorCopy(w->points[0], corner);

    // make tetrahedrons to all other faces

    volume = 0;
    for (; i < brush->numsides; i++)
    {
        w = brush->sides[i].winding;
        if (!w)
            continue;
        plane = &mapplanes[brush->sides[i].planenum];
        d = -(DotProduct(corner, plane->normal) - plane->dist);
        area = WindingArea(w);
        volume += d * area;
    }

    volume /= 3;
    return volume;
}

/*
==================
WriteBspBrushMap
==================
*/
void WriteBspBrushMap(char *name, bspbrush_t *list)
{
    FILE *f;
    side_t *s;
    int i;
    winding_t *w;

    _printf("writing %s\n", name);
    f = fopen(name, "wb");
    if (!f)
        Error("Can't write %s\b", name);

    fprintf(f, "{\n\"classname\" \"worldspawn\"\n");

    for (; list; list = list->next)
    {
        fprintf(f, "{\n");
        for (i = 0, s = list->sides; i < list->numsides; i++, s++)
        {
            if (s->winding && s->winding->numpoints >= 3)
            {
                w = s->winding;
            }
            else
            {
                w = BaseWindingForPlane(mapplanes[s->planenum].normal,
                                        mapplanes[s->planenum].dist);
            }

            fprintf(f, "( %.3f %.3f %.3f ) ", w->points[0][0], w->points[0][1], w->points[0][2]);
            fprintf(f, "( %.3f %.3f %.3f ) ", w->points[2][0], w->points[2][1], w->points[2][2]);
            fprintf(f, "( %.3f %.3f %.3f ) ", w->points[1][0], w->points[1][1], w->points[1][2]);

            const char *shader = "textures/common/caulk";
            if (s->shaderInfo)
            {
                shader = s->shaderInfo->shader;
            }
            if (!Q_strncasecmp(shader, "textures/", 9))
            {
                shader += 9;
            }
            fprintf(f, "%s 0 0 0 1 1\n", shader);

            if (w != s->winding)
            {
                FreeWinding(w);
            }
        }
        fprintf(f, "}\n");
    }
    fprintf(f, "}\n");

    fclose(f);
}

//=====================================================================================

/*
====================
FilterBrushIntoTree_r

====================
*/
int FilterBrushIntoTree_r(bspbrush_t *b, node_t *node)
{
    bspbrush_t *front, *back;
    int c;

    if (!b)
    {
        return 0;
    }

    // add it to the leaf list
    if (node->planenum == PLANENUM_LEAF)
    {
        // skip if detail inside solid
        if (b->detail && node->opaque)
        {
            FreeBrush(b);
            return 0;
        }

        b->next = node->brushlist;
        node->brushlist = b;

        // classify the leaf by the structural brush
        if (!b->detail)
        {
            if (b->opaque)
            {
                node->opaque = qtrue;
                node->areaportal = qfalse;
            }
            else if (b->contents & CONTENTS_AREAPORTAL)
            {
                if (!node->opaque)
                {
                    node->areaportal = qtrue;
                }
            }
        }

        return 1;
    }

    // split it by the node plane
    SplitBrush(b, node->planenum, &front, &back);
    FreeBrush(b);

    c = 0;
    c += FilterBrushIntoTree_r(front, node->children[0]);
    c += FilterBrushIntoTree_r(back, node->children[1]);

    return c;
}

/*
=====================
FilterDetailBrushesIntoTree

Fragment all the detail brushes into the structural leafs
=====================
*/
void FilterDetailBrushesIntoTree(entity_t *e, tree_t *tree)
{
    bspbrush_t *b, *newb;
    int r;
    int c_unique, c_clusters;
    int i;

    qprintf("----- FilterDetailBrushesIntoTree -----\n");

    c_unique = 0;
    c_clusters = 0;
    for (b = e->brushes; b; b = b->next)
    {
        if (!b->detail)
        {
            continue;
        }
        c_unique++;
        newb = CopyBrush(b);
        r = FilterBrushIntoTree_r(newb, tree->headnode);
        c_clusters += r;

        // mark all sides as visible so drawsurfs are created
        if (r)
        {
            for (i = 0; i < b->numsides; i++)
            {
                if (b->sides[i].winding)
                {
                    b->sides[i].visible = qtrue;
                }
            }
        }
    }

    qprintf("%5i detail brushes\n", c_unique);
    qprintf("%5i cluster references\n", c_clusters);
}

/*
=====================
FilterStructuralBrushesIntoTree

Mark the leafs as opaque and areaportals
=====================
*/
void FilterStructuralBrushesIntoTree(entity_t *e, tree_t *tree)
{
    bspbrush_t *b, *newb;
    int r;
    int c_unique, c_clusters;
    int i;

    qprintf("----- FilterStructuralBrushesIntoTree -----\n");

    c_unique = 0;
    c_clusters = 0;
    for (b = e->brushes; b; b = b->next)
    {
        if (b->detail)
        {
            continue;
        }
        c_unique++;
        newb = CopyBrush(b);
        r = FilterBrushIntoTree_r(newb, tree->headnode);
        c_clusters += r;

        // mark all sides as visible so drawsurfs are created
        if (r)
        {
            for (i = 0; i < b->numsides; i++)
            {
                if (b->sides[i].winding)
                {
                    b->sides[i].visible = qtrue;
                }
            }
        }
    }

    qprintf("%5i structural brushes\n", c_unique);
    qprintf("%5i cluster references\n", c_clusters);
}

/*
================
AllocTree
================
*/
tree_t *AllocTree(void)
{
    tree_t *tree;

    tree = malloc(sizeof(*tree));
    memset(tree, 0, sizeof(*tree));
    ClearBounds(tree->mins, tree->maxs);

    return tree;
}

/*
================
AllocNode
================
*/
node_t *AllocNode(void)
{
    node_t *node;

    node = malloc(sizeof(*node));
    memset(node, 0, sizeof(*node));

    return node;
}

/*
================
WindingIsTiny

Returns true if the winding would be crunched out of
existance by the vertex snapping.
================
*/
#define EDGE_LENGTH 0.2
qboolean WindingIsTiny(winding_t *w)
{
    /*
            if (WindingArea (w) < 1)
                    return qtrue;
            return qfalse;
    */
    int i, j;
    vec_t len;
    vec3_t delta;
    int edges;

    edges = 0;
    for (i = 0; i < w->numpoints; i++)
    {
        j = i == w->numpoints - 1 ? 0 : i + 1;
        VectorSubtract(w->points[j], w->points[i], delta);
        len = VectorLength(delta);
        if (len > EDGE_LENGTH)
        {
            if (++edges == 3)
                return qfalse;
        }
    }
    return qtrue;
}

/*
================
WindingIsHuge

Returns true if the winding still has one of the points
from basewinding for plane
================
*/
qboolean WindingIsHuge(winding_t *w)
{
    int i, j;

    for (i = 0; i < w->numpoints; i++)
    {
        for (j = 0; j < 3; j++)
            if (w->points[i][j] <= MIN_WORLD_COORD || w->points[i][j] >= MAX_WORLD_COORD)
                return qtrue;
    }
    return qfalse;
}

//============================================================

/*
==================
BrushMostlyOnSide

==================
*/
int BrushMostlyOnSide(bspbrush_t *brush, plane_t *plane)
{
    int i, j;
    winding_t *w;
    vec_t d, max;
    int side;

    max = 0;
    side = PSIDE_FRONT;
    for (i = 0; i < brush->numsides; i++)
    {
        w = brush->sides[i].winding;
        if (!w)
            continue;
        for (j = 0; j < w->numpoints; j++)
        {
            d = DotProduct(w->points[j], plane->normal) - plane->dist;
            if (d > max)
            {
                max = d;
                side = PSIDE_FRONT;
            }
            if (-d > max)
            {
                max = -d;
                side = PSIDE_BACK;
            }
        }
    }
    return side;
}

/*
================
SplitBrush

Generates two new brushes, leaving the original
unchanged
================
*/
void SplitBrush(bspbrush_t *brush, int planenum, bspbrush_t **front,
                bspbrush_t **back)
{
    bspbrush_t *b[2];
    int i, j;
    winding_t *w, *cw[2], *midwinding;
    plane_t *plane, *plane2;
    side_t *s, *cs;
    float d, d_front, d_back;

    *front = *back = NULL;
    plane = &mapplanes[planenum];

    // check all points
    d_front = d_back = 0;
    for (i = 0; i < brush->numsides; i++)
    {
        w = brush->sides[i].winding;
        if (!w)
            continue;
        for (j = 0; j < w->numpoints; j++)
        {
            d = DotProduct(w->points[j], plane->normal) - plane->dist;
            if (d > 0 && d > d_front)
                d_front = d;
            if (d < 0 && d < d_back)
                d_back = d;
        }
    }
    if (d_front < 0.1) // PLANESIDE_EPSILON)
    {                  // only on back
        *back = CopyBrush(brush);
        return;
    }
    if (d_back > -0.1) // PLANESIDE_EPSILON)
    {                  // only on front
        *front = CopyBrush(brush);
        return;
    }

    // create a new winding from the split plane

    w = BaseWindingForPlane(plane->normal, plane->dist);
    for (i = 0; i < brush->numsides && w; i++)
    {
        if (brush->sides[i].backSide)
        {
            continue; // fake back-sided polygons never split
        }
        plane2 = &mapplanes[brush->sides[i].planenum ^ 1];
        ChopWindingInPlace(&w, plane2->normal, plane2->dist,
                           0); // PLANESIDE_EPSILON);
    }

    if (!w || WindingIsTiny(w))
    { // the brush isn't really split
        int side;

        side = BrushMostlyOnSide(brush, plane);
        if (side == PSIDE_FRONT)
            *front = CopyBrush(brush);
        if (side == PSIDE_BACK)
            *back = CopyBrush(brush);
        return;
    }

    if (WindingIsHuge(w))
    {
        qprintf("WARNING: huge winding\n");
    }

    midwinding = w;

    // split it for real

    for (i = 0; i < 2; i++)
    {
        b[i] = AllocBrush(brush->numsides + 1);
        memcpy(b[i], brush, sizeof(bspbrush_t) - sizeof(brush->sides));
        b[i]->numsides = 0;
        b[i]->next = NULL;
        b[i]->original = brush->original;
        b[i]->epairs = CopyEpairs(brush->epairs);
    }

    // split all the current windings

    for (i = 0; i < brush->numsides; i++)
    {
        s = &brush->sides[i];
        w = s->winding;
        if (!w)
            continue;
        ClipWindingEpsilon(w, plane->normal, plane->dist, 0 /*PLANESIDE_EPSILON*/,
                           &cw[0], &cw[1]);
        for (j = 0; j < 2; j++)
        {
            if (!cw[j])
                continue;
            /*
                                    if (WindingIsTiny (cw[j]))
                                    {
                                            FreeWinding (cw[j]);
                                            continue;
                                    }
            */
            cs = &b[j]->sides[b[j]->numsides];
            b[j]->numsides++;
            *cs = *s;
            cs->winding = cw[j];
        }
    }

    // see if we have valid polygons on both sides

    for (i = 0; i < 2; i++)
    {
        BoundBrush(b[i]);
        for (j = 0; j < 3; j++)
        {
            if (b[i]->mins[j] < MIN_WORLD_COORD || b[i]->maxs[j] > MAX_WORLD_COORD)
            {
                qprintf("bogus brush after clip\n");
                break;
            }
        }

        if (b[i]->numsides < 3 || j < 3)
        {
            FreeBrush(b[i]);
            b[i] = NULL;
        }
    }

    if (!(b[0] && b[1]))
    {
        if (!b[0] && !b[1])
            qprintf("split removed brush\n");
        else
            qprintf("split not on both sides\n");
        if (b[0])
        {
            FreeBrush(b[0]);
            *front = CopyBrush(brush);
        }
        if (b[1])
        {
            FreeBrush(b[1]);
            *back = CopyBrush(brush);
        }
        return;
    }

    // add the midwinding to both sides
    for (i = 0; i < 2; i++)
    {
        cs = &b[i]->sides[b[i]->numsides];
        b[i]->numsides++;

        cs->planenum = planenum ^ i ^ 1;
        cs->shaderInfo = NULL;
        if (i == 0)
            cs->winding = CopyWinding(midwinding);
        else
            cs->winding = midwinding;
    }

    {
        vec_t v1;
        int i;

        for (i = 0; i < 2; i++)
        {
            v1 = BrushVolume(b[i]);
            if (v1 < 1.0)
            {
                FreeBrush(b[i]);
                b[i] = NULL;
                //			qprintf ("tiny volume after clip\n");
            }
        }
    }

    *front = b[0];
    *back = b[1];
}
