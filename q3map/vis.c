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
// vis.c

#include "vis.h"
#include "../shared/globals.h"
#include "../common/threads.h"
#include "stdlib.h"
#ifdef _WIN32
#include "../libs/pakstuff.h"
#endif

#define VIS_HEADER_SIZE 8

extern char outbase[32];

int v_numportals;
int v_portalclusters;
int v_numfaces;

char inbase[32];

vportal_t *v_portals;
leaf_t *v_leafs;

vportal_t *faces;
leaf_t *faceleafs;

int c_portaltest, c_portalpass, c_portalcheck;

int leafbytes; // (v_portalclusters+63)>>3
int leaflongs;

int portalbytes, portallongs;

qboolean noPassageVis;
qboolean passageVisOnly;
qboolean mergevis = qfalse;
qboolean nosort;
qboolean saveprt;

float farPlaneDist = 0.0f;
char farPlaneDistMode = '\0';
int testlevel = 2;

int totalvis;

vportal_t *sorted_portals[MAX_MAP_PORTALS * 2];

void PassageMemory(void);

//=============================================================================

void PlaneFromWinding(winding_t *w, plane_t *plane)
{
    vec3_t v1, v2;

    // calc plane
    VectorSubtract(w->points[2], w->points[1], v1);
    VectorSubtract(w->points[0], w->points[1], v2);
    CrossProduct(v2, v1, plane->normal);
    VectorNormalize(plane->normal, plane->normal);
    plane->dist = DotProduct(w->points[0], plane->normal);
}

/*
==================
CheckPortalWinding

Sanity validation for a portal winding loaded from the .prt. Catches
malformed portal geometry (chop drift off the portal plane, duplicate
points, degenerate plane basis, near-zero area) that would silently
narrow the PVS instead of erroring out.
==================
*/
#define PORTAL_PLANARITY_EPSILON 0.5
#define PORTAL_DUPLICATE_EPSILON 0.05
#define PORTAL_MIN_AREA 0.1

static void CheckPortalWinding(int portalnum, winding_t *w, plane_t *plane)
{
    static int c_warned = 0;
    vec_t maxdist = 0.0;
    vec_t area = 0.0;
    vec3_t v1, v2, cross;
    int i, j;

    if (VectorLength(plane->normal) < 0.5)
    {
        if (++c_warned <= 32)
            _printf("WARNING: portal %i has a degenerate plane basis (colinear basis points)\n", portalnum);
        return;
    }

    // planarity: every point must lie on the portal plane
    for (i = 0; i < w->numpoints; i++)
    {
        vec_t d = fabs(DotProduct(w->points[i], plane->normal) - plane->dist);
        if (d > maxdist)
            maxdist = d;
    }
    if (maxdist > PORTAL_PLANARITY_EPSILON)
    {
        if (++c_warned <= 32)
            _printf("WARNING: portal %i deviates from its own plane by %.3f units\n", portalnum, maxdist);
    }

    // duplicate/near-duplicate consecutive points (incl. wraparound)
    for (i = 0; i < w->numpoints; i++)
    {
        j = (i + 1) % w->numpoints;
        VectorSubtract(w->points[i], w->points[j], v1);
        if (VectorLength(v1) < PORTAL_DUPLICATE_EPSILON)
        {
            if (++c_warned <= 32)
                _printf("WARNING: portal %i has duplicate consecutive points %i/%i (%.4f apart)\n",
                        portalnum, i, j, VectorLength(v1));
            break;
        }
    }

    // fan area around point 0
    for (i = 1; i < w->numpoints - 1; i++)
    {
        VectorSubtract(w->points[i], w->points[0], v1);
        VectorSubtract(w->points[i + 1], w->points[0], v2);
        CrossProduct(v1, v2, cross);
        area += 0.5 * VectorLength(cross);
    }
    if (area < PORTAL_MIN_AREA)
    {
        if (++c_warned <= 32)
            _printf("WARNING: portal %i has a degenerate area (%.4f)\n", portalnum, area);
    }

    if (c_warned == 33)
        _printf("WARNING: further portal winding warnings suppressed\n");
}

/*
==================
NewWinding
==================
*/
winding_t *NewWinding(int points)
{
    winding_t *w;
    size_t size;

    if (points > MAX_POINTS_ON_WINDING)
        Error("NewWinding: %i points", points);

    size = (size_t)((winding_t *)0)->points[points];
    w = malloc(size);
    memset(w, 0, size);

    return w;
}

void prl(leaf_t *l)
{
    int i;
    vportal_t *p;
    plane_t pl;

    for (i = 0; i < l->numportals_in_leaf; i++)
    {
        p = l->portals_in_leaf[i];
        pl = p->plane;
        _printf("portal %4i to leaf %4i : %7.1f : (%4.1f, %4.1f, %4.1f)\n",
                (int)(p - v_portals), p->leaf, pl.dist, pl.normal[0], pl.normal[1],
                pl.normal[2]);
    }
}

//=============================================================================

/*
=============
SortPortals

Sorts the v_portals from the least complex, so the later ones can reuse
the earlier information.
=============
*/
int PComp(const void *a, const void *b)
{
    if ((*(vportal_t **)a)->nummightsee == (*(vportal_t **)b)->nummightsee)
        return 0;
    if ((*(vportal_t **)a)->nummightsee < (*(vportal_t **)b)->nummightsee)
        return -1;
    return 1;
}
void SortPortals(void)
{
    int i;

    for (i = 0; i < v_numportals * 2; i++)
        sorted_portals[i] = &v_portals[i];

    if (nosort)
        return;
    qsort(sorted_portals, v_numportals * 2, sizeof(sorted_portals[0]), PComp);
}

/*
==============
LeafVectorFromPortalVector
==============
*/
int LeafVectorFromPortalVector(byte *portalbits, byte *leafbits)
{
    int i, j, leafnum;
    vportal_t *p;
    int c_leafs;

    for (i = 0; i < v_numportals * 2; i++)
    {
        if (portalbits[i >> 3] & (1 << (i & 7)))
        {
            p = v_portals + i;
            leafbits[p->leaf >> 3] |= (1 << (p->leaf & 7));
        }
    }

    for (j = 0; j < v_portalclusters; j++)
    {
        leafnum = j;
        while (v_leafs[leafnum].merged >= 0)
            leafnum = v_leafs[leafnum].merged;
        // if the merged leaf is visible then the original leaf is visible
        if (leafbits[leafnum >> 3] & (1 << (leafnum & 7)))
        {
            leafbits[j >> 3] |= (1 << (j & 7));
        }
    }

    c_leafs = CountBits(leafbits, v_portalclusters);

    return c_leafs;
}

/*
===============
ClusterMerge

Merges the portal visibility for a leaf
===============
*/
void ClusterMerge(int leafnum)
{
    leaf_t *leaf;
    byte portalvector[MAX_PORTALS / 8];
    byte *uncompressed = malloc(game->maxMapLeafs / 8);
    int i, j;
    int numvis, mergedleafnum;
    vportal_t *p;
    int pnum;

    // OR together all the portalvis bits

    mergedleafnum = leafnum;
    while (v_leafs[mergedleafnum].merged >= 0)
        mergedleafnum = v_leafs[mergedleafnum].merged;

    memset(portalvector, 0, portalbytes);
    leaf = &v_leafs[mergedleafnum];
    for (i = 0; i < leaf->numportals_in_leaf; i++)
    {
        p = leaf->portals_in_leaf[i];
        if (p->removed)
            continue;

        if (p->status != stat_done)
            Error("portal not done");
        for (j = 0; j < portallongs; j++)
            ((long *)portalvector)[j] |= ((long *)p->portalvis)[j];
        pnum = p - v_portals;
        portalvector[pnum >> 3] |= 1 << (pnum & 7);
    }

    memset(uncompressed, 0, leafbytes);

    uncompressed[mergedleafnum >> 3] |= (1 << (mergedleafnum & 7));
    // convert portal bits to leaf bits
    numvis = LeafVectorFromPortalVector(portalvector, uncompressed);

    //	if (uncompressed[leafnum>>3] & (1<<(leafnum&7)))
    //		_printf ("WARNING: Leaf v_portals saw into leaf\n");

    //	uncompressed[leafnum>>3] |= (1<<(leafnum&7));

    numvis++; // count the leaf itself

    totalvis += numvis;

    qprintf("cluster %4i : %4i visible\n", leafnum, numvis);

    memcpy(visBytes + VIS_HEADER_SIZE + leafnum * leafbytes, uncompressed,
           leafbytes);
    free(uncompressed);
}

/*
==================
CalcPortalVis
==================
*/
void CalcPortalVis(void)
{
    RunThreadsOnIndividual(v_numportals * 2, qtrue, PortalFlow);
    _printf("\n");
}

/*
==================
CalcPassageVis
==================
*/
void CalcPassageVis(void)
{
    PassageMemory();

    RunThreadsOnIndividual(v_numportals * 2, qtrue, CreatePassages);
    _printf("\n");
    RunThreadsOnIndividual(v_numportals * 2, qtrue, PassageFlow);
    _printf("\n");
}

/*
==================
CalcPassagePortalVis
==================
*/
void CalcPassagePortalVis(void)
{
    PassageMemory();

    RunThreadsOnIndividual(v_numportals * 2, qtrue, CreatePassages);
    _printf("\n");
    RunThreadsOnIndividual(v_numportals * 2, qtrue, PassagePortalFlow);
    _printf("\n");
}

/*
==================
CalcVis
==================
*/
void CalcVis(void)
{
    int i;

    RunThreadsOnIndividual(v_numportals * 2, qtrue, BasePortalVis);
    _printf("\n");

    SortPortals();

    if (noPassageVis)
    {
        CalcPortalVis();
    }
    else if (passageVisOnly)
    {
        CalcPassageVis();
    }
    else
    {
        CalcPassagePortalVis();
    }
    //
    // assemble the leaf vis lists by oring and compressing the portal lists
    //
    _printf("creating leaf vis...\n");
    for (i = 0; i < v_portalclusters; i++)
        ClusterMerge(i);

    _printf("Total visible clusters: %i\n", totalvis);
    _printf("Average clusters visible: %i\n", totalvis / v_portalclusters);
}

/*
==================
SetPortalSphere
==================
*/
void SetPortalSphere(vportal_t *p)
{
    int i;
    // accumulate in double precision (NetRadiant fix): on huge portals the
    // float accumulation under-estimates the radius, making every radius
    // fast-reject in the vis flow more aggressive at grazing angles
    double total[3], dist[3];
    winding_t *w;
    double r, bestr;

    w = p->winding;
    total[0] = total[1] = total[2] = 0;
    for (i = 0; i < w->numpoints; i++)
    {
        total[0] += w->points[i][0];
        total[1] += w->points[i][1];
        total[2] += w->points[i][2];
    }

    for (i = 0; i < 3; i++)
        total[i] /= w->numpoints;

    bestr = 0;
    for (i = 0; i < w->numpoints; i++)
    {
        dist[0] = w->points[i][0] - total[0];
        dist[1] = w->points[i][1] - total[1];
        dist[2] = w->points[i][2] - total[2];
        r = sqrt(dist[0] * dist[0] + dist[1] * dist[1] + dist[2] * dist[2]);
        if (r > bestr)
            bestr = r;
    }
    p->origin[0] = (vec_t)total[0];
    p->origin[1] = (vec_t)total[1];
    p->origin[2] = (vec_t)total[2];
    p->radius = (vec_t)bestr;
}

/*
=============
Winding_PlanesConcave
=============
*/
#define WCONVEX_EPSILON 0.2

int Winding_PlanesConcave(winding_t *w1, winding_t *w2, vec3_t normal1,
                          vec3_t normal2, float dist1, float dist2)
{
    int i;

    if (!w1 || !w2)
        return qfalse;

    // check if one of the points of winding 1 is at the front of the plane of
    // winding 2
    for (i = 0; i < w1->numpoints; i++)
    {
        if (DotProduct(normal2, w1->points[i]) - dist2 > WCONVEX_EPSILON)
            return qtrue;
    }
    // check if one of the points of winding 2 is at the front of the plane of
    // winding 1
    for (i = 0; i < w2->numpoints; i++)
    {
        if (DotProduct(normal1, w2->points[i]) - dist1 > WCONVEX_EPSILON)
            return qtrue;
    }

    return qfalse;
}

/*
============
TryMergeLeaves
============
*/
int TryMergeLeaves(int l1num, int l2num)
{
    int i, j, k, n, v_numportals;
    plane_t plane1, plane2;
    leaf_t *l1, *l2;
    vportal_t *p1, *p2;
    vportal_t *v_portals[MAX_PORTALS_ON_LEAF];

    for (k = 0; k < 2; k++)
    {
        if (k)
            l1 = &v_leafs[l1num];
        else
            l1 = &faceleafs[l1num];
        for (i = 0; i < l1->numportals_in_leaf; i++)
        {
            p1 = l1->portals_in_leaf[i];
            if (p1->leaf == l2num)
                continue;
            for (n = 0; n < 2; n++)
            {
                if (n)
                    l2 = &v_leafs[l2num];
                else
                    l2 = &faceleafs[l2num];
                for (j = 0; j < l2->numportals_in_leaf; j++)
                {
                    p2 = l2->portals_in_leaf[j];
                    if (p2->leaf == l1num)
                        continue;
                    //
                    plane1 = p1->plane;
                    plane2 = p2->plane;
                    if (Winding_PlanesConcave(p1->winding, p2->winding, plane1.normal,
                                              plane2.normal, plane1.dist, plane2.dist))
                        return qfalse;
                }
            }
        }
    }
    for (k = 0; k < 2; k++)
    {
        if (k)
        {
            l1 = &v_leafs[l1num];
            l2 = &v_leafs[l2num];
        }
        else
        {
            l1 = &faceleafs[l1num];
            l2 = &faceleafs[l2num];
        }
        v_numportals = 0;
        // the leaves can be merged now
        for (i = 0; i < l1->numportals_in_leaf; i++)
        {
            p1 = l1->portals_in_leaf[i];
            if (p1->leaf == l2num)
            {
                p1->removed = qtrue;
                continue;
            }
            v_portals[v_numportals++] = p1;
        }
        for (j = 0; j < l2->numportals_in_leaf; j++)
        {
            p2 = l2->portals_in_leaf[j];
            if (p2->leaf == l1num)
            {
                p2->removed = qtrue;
                continue;
            }
            v_portals[v_numportals++] = p2;
        }
        for (i = 0; i < v_numportals; i++)
        {
            l2->portals_in_leaf[i] = v_portals[i];
        }
        l2->numportals_in_leaf = v_numportals;
        l1->merged = l2num;
    }
    return qtrue;
}

/*
============
UpdatePortals
============
*/
void UpdatePortals(void)
{
    int i;
    vportal_t *p;

    for (i = 0; i < v_numportals * 2; i++)
    {
        p = &v_portals[i];
        if (p->removed)
            continue;
        while (v_leafs[p->leaf].merged >= 0)
            p->leaf = v_leafs[p->leaf].merged;
    }
}

/*
============
MergeLeaves

try to merge leaves but don't merge through hint splitters
============
*/
void MergeLeaves(void)
{
    int i, j, nummerges, totalnummerges;
    leaf_t *leaf;
    vportal_t *p;

    totalnummerges = 0;
    do
    {
        nummerges = 0;
        for (i = 0; i < v_portalclusters; i++)
        {
            leaf = &v_leafs[i];
            // if this leaf is merged already
            if (leaf->merged >= 0)
                continue;
            //
            for (j = 0; j < leaf->numportals_in_leaf; j++)
            {
                p = leaf->portals_in_leaf[j];
                //
                if (p->removed)
                    continue;
                // never merge through hint v_portals
                if (p->hint)
                    continue;
                if (TryMergeLeaves(i, p->leaf))
                {
                    UpdatePortals();
                    nummerges++;
                    break;
                }
            }
        }
        totalnummerges += nummerges;
    } while (nummerges);
    _printf("%6d leaves merged\n", totalnummerges);
}

/*
============
TryMergeWinding
============
*/
#define CONTINUOUS_EPSILON 0.005

winding_t *TryMergeWinding(winding_t *f1, winding_t *f2, vec3_t planenormal)
{
    vec_t *p1, *p2, *p3, *p4, *back;
    winding_t *newf;
    int i, j, k, l;
    vec3_t normal, delta;
    vec_t dot;
    qboolean keep1, keep2;

    //
    // find a common edge
    //
    p1 = p2 = NULL; // stop compiler warning
    j = 0;          //

    for (i = 0; i < f1->numpoints; i++)
    {
        p1 = f1->points[i];
        p2 = f1->points[(i + 1) % f1->numpoints];
        for (j = 0; j < f2->numpoints; j++)
        {
            p3 = f2->points[j];
            p4 = f2->points[(j + 1) % f2->numpoints];
            for (k = 0; k < 3; k++)
            {
                if (fabs(p1[k] - p4[k]) > 0.1) // EQUAL_EPSILON) //ME
                    break;
                if (fabs(p2[k] - p3[k]) > 0.1) // EQUAL_EPSILON) //ME
                    break;
            } // end for
            if (k == 3)
                break;
        } // end for
        if (j < f2->numpoints)
            break;
    } // end for

    if (i == f1->numpoints)
        return NULL; // no matching edges

    //
    // check slope of connected lines
    // if the slopes are colinear, the point can be removed
    //
    back = f1->points[(i + f1->numpoints - 1) % f1->numpoints];
    VectorSubtract(p1, back, delta);
    CrossProduct(planenormal, delta, normal);
    VectorNormalize(normal, normal);

    back = f2->points[(j + 2) % f2->numpoints];
    VectorSubtract(back, p1, delta);
    dot = DotProduct(delta, normal);
    if (dot > CONTINUOUS_EPSILON)
        return NULL; // not a convex polygon
    keep1 = (qboolean)(dot < -CONTINUOUS_EPSILON);

    back = f1->points[(i + 2) % f1->numpoints];
    VectorSubtract(back, p2, delta);
    CrossProduct(planenormal, delta, normal);
    VectorNormalize(normal, normal);

    back = f2->points[(j + f2->numpoints - 1) % f2->numpoints];
    VectorSubtract(back, p2, delta);
    dot = DotProduct(delta, normal);
    if (dot > CONTINUOUS_EPSILON)
        return NULL; // not a convex polygon
    keep2 = (qboolean)(dot < -CONTINUOUS_EPSILON);

    //
    // build the new polygon
    //
    newf = NewWinding(f1->numpoints + f2->numpoints);

    // copy first polygon
    for (k = (i + 1) % f1->numpoints; k != i; k = (k + 1) % f1->numpoints)
    {
        if (k == (i + 1) % f1->numpoints && !keep2)
            continue;

        VectorCopy(f1->points[k], newf->points[newf->numpoints]);
        newf->numpoints++;
    }

    // copy second polygon
    for (l = (j + 1) % f2->numpoints; l != j; l = (l + 1) % f2->numpoints)
    {
        if (l == (j + 1) % f2->numpoints && !keep1)
            continue;
        VectorCopy(f2->points[l], newf->points[newf->numpoints]);
        newf->numpoints++;
    }

    return newf;
}

/*
============
MergeLeafPortals
============
*/
void MergeLeafPortals(void)
{
    int i, j, k, nummerges, hintsmerged;
    leaf_t *leaf;
    vportal_t *p1, *p2;
    winding_t *w;

    nummerges = 0;
    hintsmerged = 0;
    for (i = 0; i < v_portalclusters; i++)
    {
        leaf = &v_leafs[i];
        if (leaf->merged >= 0)
            continue;
        for (j = 0; j < leaf->numportals_in_leaf; j++)
        {
            p1 = leaf->portals_in_leaf[j];
            if (p1->removed)
                continue;
            for (k = j + 1; k < leaf->numportals_in_leaf; k++)
            {
                p2 = leaf->portals_in_leaf[k];
                if (p2->removed)
                    continue;
                if (p1->leaf == p2->leaf)
                {
                    w = TryMergeWinding(p1->winding, p2->winding, p1->plane.normal);
                    if (w)
                    {
                        FreeWinding(p1->winding);
                        p1->winding = w;
                        if (p1->hint && p2->hint)
                            hintsmerged++;
                        p1->hint |= p2->hint;
                        SetPortalSphere(p1);
                        p2->removed = qtrue;
                        nummerges++;
                        i--;
                        break;
                    }
                }
            }
            if (k < leaf->numportals_in_leaf)
                break;
        }
    }
    _printf("%6d v_portals merged\n", nummerges);
    _printf("%6d hint v_portals merged\n", hintsmerged);
}

/*
============
WritePortals
============
*/
int CountActivePortals(void)
{
    int num, hints, j;
    vportal_t *p;

    num = 0;
    hints = 0;
    for (j = 0; j < v_numportals * 2; j++)
    {
        p = v_portals + j;
        if (p->removed)
            continue;
        if (p->hint)
            hints++;
        num++;
    }
    _printf("%6d active v_portals\n", num);
    _printf("%6d hint v_portals\n", hints);
    return num;
}

/*
============
WritePortals
============
*/
void WriteFloat(FILE *f, vec_t v);

void WritePortals(char *filename)
{
    int i, j, num;
    FILE *pf;
    vportal_t *p;
    winding_t *w;

    // write the file
    pf = fopen(filename, "w");
    if (!pf)
        Error("Error opening %s", filename);

    num = 0;
    for (j = 0; j < v_numportals * 2; j++)
    {
        p = v_portals + j;
        if (p->removed)
            continue;
        //		if (!p->hint)
        //			continue;
        num++;
    }

    fprintf(pf, "%s\n", PORTALFILE);
    fprintf(pf, "%i\n", 0);
    fprintf(pf, "%i\n", num); // + v_numfaces);
    fprintf(pf, "%i\n", 0);

    for (j = 0; j < v_numportals * 2; j++)
    {
        p = v_portals + j;
        if (p->removed)
            continue;
        //		if (!p->hint)
        //			continue;
        w = p->winding;
        fprintf(pf, "%i %i %i ", w->numpoints, 0, 0);
        fprintf(pf, "%d ", p->hint);
        for (i = 0; i < w->numpoints; i++)
        {
            fprintf(pf, "(");
            WriteFloat(pf, w->points[i][0]);
            WriteFloat(pf, w->points[i][1]);
            WriteFloat(pf, w->points[i][2]);
            fprintf(pf, ") ");
        }
        fprintf(pf, "\n");
    }

    /*
    for (j = 0; j < v_numfaces; j++)
    {
            p = faces + j;
            w = p->winding;
            fprintf (pf,"%i %i %i ",w->numpoints, 0, 0);
            fprintf (pf, "0 ");
            for (i=0 ; i<w->numpoints ; i++)
            {
                    fprintf (pf,"(");
                    WriteFloat (pf, w->points[i][0]);
                    WriteFloat (pf, w->points[i][1]);
                    WriteFloat (pf, w->points[i][2]);
                    fprintf (pf,") ");
            }
            fprintf (pf,"\n");
    }*/

    fclose(pf);
}

/*
============
LoadPortals
============
*/
void LoadPortals(char *name)
{
    int i, j, flags;
    vportal_t *p;
    leaf_t *l;
    char magic[80];
    FILE *f;
    int numpoints;
    winding_t *w;
    int leafnums[2];
    plane_t plane;

    if (!strcmp(name, "-"))
        f = stdin;
    else
    {
        f = fopen(name, "r");
        if (!f)
            Error("LoadPortals: couldn't read %s\n", name);
    }

    if (fscanf(f, "%79s\n%i\n%i\n%i\n", magic, &v_portalclusters, &v_numportals,
               &v_numfaces) != 4)
        Error("LoadPortals: failed to read header");
    if (strcmp(magic, PORTALFILE))
        Error("LoadPortals: not a portal file");

    _printf("%6i v_portalclusters\n", v_portalclusters);
    _printf("%6i v_numportals\n", v_numportals);
    _printf("%6i v_numfaces\n", v_numfaces);

    // Bounds guards (ported from NetRadiant q3map2 vis.cpp LoadPortals):
    // exceeding these silently smashes the fixed mightsee/portalvector
    // stack buffers and the visBytes array, corrupting the PVS.
    if (v_numportals > MAX_PORTALS)
        Error("LoadPortals: %i portals exceeds MAX_PORTALS (%i)", v_numportals, MAX_PORTALS);
    if (v_portalclusters >= MAX_PORTALS)
        Error("LoadPortals: %i portal clusters exceeds MAX_PORTALS (%i)", v_portalclusters, MAX_PORTALS);

    // these counts should take advantage of 64 bit systems automatically
    leafbytes = ((v_portalclusters + 63) & ~63) >> 3;
    leaflongs = leafbytes / sizeof(long);

    portalbytes = ((v_numportals * 2 + 63) & ~63) >> 3;
    portallongs = portalbytes / sizeof(long);

    // each file portal is split into two memory v_portals
    v_portals = malloc(2 * v_numportals * sizeof(vportal_t));
    memset(v_portals, 0, 2 * v_numportals * sizeof(vportal_t));

    v_leafs = malloc(v_portalclusters * sizeof(leaf_t));
    memset(v_leafs, 0, v_portalclusters * sizeof(leaf_t));

    for (i = 0; i < v_portalclusters; i++)
        v_leafs[i].merged = -1;

    numVisBytes = VIS_HEADER_SIZE + v_portalclusters * leafbytes;

    if (numVisBytes > MAX_MAP_VISIBILITY)
        Error("LoadPortals: %i vis bytes exceeds MAX_MAP_VISIBILITY (%i)", numVisBytes, MAX_MAP_VISIBILITY);

    ((int *)visBytes)[0] = v_portalclusters;
    ((int *)visBytes)[1] = leafbytes;

    for (i = 0, p = v_portals; i < v_numportals; i++)
    {
        if (fscanf(f, "%i %i %i ", &numpoints, &leafnums[0], &leafnums[1]) != 3)
            Error("LoadPortals: reading portal %i", i);
        if (numpoints > MAX_POINTS_ON_WINDING)
            Error("LoadPortals: portal %i has too many points", i);
        if ((unsigned)leafnums[0] > v_portalclusters ||
            (unsigned)leafnums[1] > v_portalclusters)
            Error("LoadPortals: reading portal %i", i);
        if (fscanf(f, "%i ", &flags) != 1)
            Error("LoadPortals: reading portal flags");

        w = p->winding = NewWinding(numpoints);
        w->numpoints = numpoints;

        for (j = 0; j < numpoints; j++)
        {
            double v[3];
            int k;

            // scanf into double, then assign to vec_t
            // so we don't care what size vec_t is
            if (fscanf(f, "(%lf %lf %lf ) ", &v[0], &v[1], &v[2]) != 3)
                Error("LoadPortals: reading portal %i", i);
            for (k = 0; k < 3; k++)
                w->points[j][k] = v[k];
        }
        fscanf(f, "\n");

        // calc plane
        PlaneFromWinding(w, &plane);
        CheckPortalWinding(i, w, &plane);

        // create forward portal
        l = &v_leafs[leafnums[0]];
        if (l->numportals_in_leaf == MAX_PORTALS_ON_LEAF)
            Error("Leaf with too many v_portals");
        l->portals_in_leaf[l->numportals_in_leaf] = p;
        l->numportals_in_leaf++;

        p->num = i + 1;
        p->hint = (flags & 1) != 0;
        p->sky  = (flags & 2) != 0;
        p->winding = w;
        VectorSubtract(vec3_origin, plane.normal, p->plane.normal);
        p->plane.dist = -plane.dist;
        p->leaf = leafnums[1];
        SetPortalSphere(p);
        p++;

        // create backwards portal
        l = &v_leafs[leafnums[1]];
        if (l->numportals_in_leaf == MAX_PORTALS_ON_LEAF)
            Error("Leaf with too many v_portals");
        l->portals_in_leaf[l->numportals_in_leaf] = p;
        l->numportals_in_leaf++;

        p->num = i + 1;
        p->hint = (flags & 1) != 0;
        p->sky  = (flags & 2) != 0;
        p->winding = NewWinding(w->numpoints);
        p->winding->numpoints = w->numpoints;
        for (j = 0; j < w->numpoints; j++)
        {
            VectorCopy(w->points[w->numpoints - 1 - j], p->winding->points[j]);
        }

        p->plane = plane;
        p->leaf = leafnums[0];
        SetPortalSphere(p);
        p++;
    }

    faces = malloc(2 * v_numfaces * sizeof(vportal_t));
    memset(faces, 0, 2 * v_numfaces * sizeof(vportal_t));

    faceleafs = malloc(v_portalclusters * sizeof(leaf_t));
    memset(faceleafs, 0, v_portalclusters * sizeof(leaf_t));

    for (i = 0, p = faces; i < v_numfaces; i++)
    {
        if (fscanf(f, "%i %i ", &numpoints, &leafnums[0]) != 2)
            Error("LoadPortals: reading portal %i", i);

        w = p->winding = NewWinding(numpoints);
        w->numpoints = numpoints;

        for (j = 0; j < numpoints; j++)
        {
            double v[3];
            int k;

            // scanf into double, then assign to vec_t
            // so we don't care what size vec_t is
            if (fscanf(f, "(%lf %lf %lf ) ", &v[0], &v[1], &v[2]) != 3)
                Error("LoadPortals: reading portal %i", i);
            for (k = 0; k < 3; k++)
                w->points[j][k] = v[k];
        }
        fscanf(f, "\n");

        // calc plane
        PlaneFromWinding(w, &plane);

        l = &faceleafs[leafnums[0]];
        l->merged = -1;
        if (l->numportals_in_leaf == MAX_PORTALS_ON_LEAF)
            Error("Leaf with too many faces");
        l->portals_in_leaf[l->numportals_in_leaf] = p;
        l->numportals_in_leaf++;

        p->num = i + 1;
        p->winding = w;
        // normal pointing out of the leaf
        VectorSubtract(vec3_origin, plane.normal, p->plane.normal);
        p->plane.dist = -plane.dist;
        p->leaf = -1;
        SetPortalSphere(p);
        p++;
    }

    fclose(f);
}

/*
================
CalcPHS

Calculate the PHS (Potentially Hearable Set)
by ORing together all the PVS visible from a leaf
================
*/
void CalcPHS(void)
{
    int i, j, k, l, index;
    int bitbyte;
    long *src;
    byte *scan;
    int count;
    byte *uncompressed = malloc(game->maxMapLeafs / 8);

    _printf("Building PHS...\n");

    count = 0;
    for (i = 0; i < v_portalclusters; i++)
    {
        scan = visBytes + i * leafbytes;
        memcpy(uncompressed, scan, leafbytes);
        for (j = 0; j < leafbytes; j++)
        {
            bitbyte = scan[j];
            if (!bitbyte)
                continue;
            for (k = 0; k < 8; k++)
            {
                if (!(bitbyte & (1 << k)))
                    continue;
                // OR this pvs row into the phs
                index = ((j << 3) + k);
                if (index >= v_portalclusters)
                    Error("Bad bit in PVS"); // pad bits should be 0
                src = (long *)(visBytes + index * leafbytes);
                for (l = 0; l < leaflongs; l++)
                    ((long *)uncompressed)[l] |= src[l];
            }
        }
        for (j = 0; j < v_portalclusters; j++)
            if (uncompressed[j >> 3] & (1 << (j & 7)))
                count++;

        // FIXME: copy it off
    }

    _printf("Average clusters hearable: %i\n", count / v_portalclusters);
    free(uncompressed);
}

/*
==================
CalculateVisibility
==================
*/
void CalculateVisibility(qboolean mergeportals)
{
    if (mergeportals)
    {
        MergeLeaves();
        MergeLeafPortals();
    }

    CountActivePortals();
    //	WritePortals("maps/hints.prs");

    _printf("visdatasize:%i\n", numVisBytes);

    CalcVis();
}

/*
==================
FreeVisibility
==================
*/
void FreeVisibility(void)
{
    int i;

    // Free main portals
    if (v_portals)
    {
        for (i = 0; i < v_numportals * 2; i++)
        {
            vportal_t *p = &v_portals[i];
            if (p->winding)
                FreeWinding(p->winding);
            if (p->portalfront)
                free(p->portalfront);
            if (p->portalflood)
                free(p->portalflood);
            if (p->portalvis)
                free(p->portalvis);
            
            // Free passages
            passage_t *ps = p->passages;
            while (ps)
            {
                passage_t *next = ps->next;
                free(ps);
                ps = next;
            }
        }
        free(v_portals);
        v_portals = NULL;
    }

    // Free leafs
    if (v_leafs)
    {
        free(v_leafs);
        v_leafs = NULL;
    }

    // Free solid faces
    if (faces)
    {
        for (i = 0; i < v_numfaces * 2; i++)
        {
            vportal_t *p = &faces[i];
            if (p->winding)
                FreeWinding(p->winding);
            if (p->portalfront)
                free(p->portalfront);
            if (p->portalflood)
                free(p->portalflood);
            if (p->portalvis)
                free(p->portalvis);
            
            passage_t *ps = p->passages;
            while (ps)
            {
                passage_t *next = ps->next;
                free(ps);
                ps = next;
            }
        }
        free(faces);
        faces = NULL;
    }

    // Free faceleafs
    if (faceleafs)
    {
        free(faceleafs);
        faceleafs = NULL;
    }

    // Reset counters
    v_numportals = 0;
    v_portalclusters = 0;
    v_numfaces = 0;
    totalvis = 0;
    c_portaltest = 0;
    c_portalpass = 0;
    c_portalcheck = 0;
}

/*
===========
VisMain
===========
*/
int VisMain(int argc, char **argv)
{
    char portalfile[1024];
    char name[1024];
    int i;
    double start, end;

    _printf("---- vis ----\n");

    verbose = qfalse;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-threads"))
        {
            numthreads = atoi(argv[i + 1]);
            i++;
        }
        else if (!strcmp(argv[i], "-nopassage"))
        {
            _printf("nopassage = true\n");
            noPassageVis = qtrue;
        }
        else if (!strcmp(argv[i], "-passageOnly"))
        {
            _printf("passageOnly = true\n");
            passageVisOnly = qtrue;
        }
        else if (!strcmp(argv[i], "-level"))
        {
            testlevel = atoi(argv[i + 1]);
            _printf("testlevel = %i\n", testlevel);
            i++;
        }
        else if (!strcmp(argv[i], "-v"))
        {
            _printf("verbose = true\n");
            verbose = qtrue;
        }
        else if (!strcmp(argv[i], "-nosort"))
        {
            _printf("nosort = true\n");
            nosort = qtrue;
        }
        else if (!strcmp(argv[i], "-vismerge") || !strcmp(argv[i], "-mergevis"))
        {
            _printf("vismerge = true\n");
            mergevis = qtrue;
        }
        else if (!strcmp(argv[i], "-saveprt"))
        {
            _printf("saveprt = true\n");
            saveprt = qtrue;
        }
        else if (!strcmp(argv[i], "-farplane"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("-farplane requires a numeric argument");
            farPlaneDist = atof(argv[i + 1]);
            _printf("farPlaneDist = %f\n", farPlaneDist);
            i++;
        }
        else if (!strcmp(argv[i], "-farplanemode"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("-farplanemode requires a mode argument (o, e, r)");
            farPlaneDistMode = argv[i + 1][0];
            _printf("farPlaneDistMode = %c\n", farPlaneDistMode);
            i++;
        }
        else if (!strcmp(argv[i], "-tmpin"))
        {
            strcpy(inbase, "/tmp");
        }
        else if (!strcmp(argv[i], "-tmpout"))
        {
            strcpy(outbase, "/tmp");
        }
        else if (!strcmp(argv[i], "-basepath") || !strcmp(argv[i], "-rootdir") ||
                 !strcmp(argv[i], "-fs_basepath"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("%s requires a directory path", argv[i]);
            i++;
        }
        else if (!strcmp(argv[i], "-tempname"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("-tempname requires a string argument");
            i++;
        }
        else if (!strcmp(argv[i], "-userdir") || !strcmp(argv[i], "-fs_homepath") || !strcmp(argv[i], "-fs_pakpath"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("%s requires a directory path", argv[i]);
            i++;
        }
        else if (!strcmp(argv[i], "-gamedir") || !strcmp(argv[i], "-fs_game"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("%s requires a directory path", argv[i]);
            i++;
        }
        else if (!strcmp(argv[i], "-game"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("-game requires a profile name");
            i++;
        }
        else if (!strcmp(argv[i], "-connect"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("%s requires an IP address", argv[i]);
            // Handled in main pre-scan
            i++;
        }
        else if (!strcmp(argv[i], "-visonly") || !strcmp(argv[i], "-bsp"))
        {
            // Handled by mode switcher
        }
        else if (argv[i][0] == '-')
        {
            Error("Unknown option \"%s\"", argv[i]);
        }
        else
        {
            break;
        }
    }

    if (i != argc - 1)
        Error("usage: vis [-threads #] [-level 0-4] [-v] bspfile");

#ifdef MREDEBUG
    start = clock();
#else
    start = I_FloatTime();
#endif

    ThreadSetDefault();

    // Print active VFS paths
    {
        int p;
        for (p = 0; p < numVFSPaths; p++)
            _printf("vfsPath[%d]: %s\n", p, vfsPaths[p]);
        _printf("writedir: %s\n", writedir);
    }

#ifdef _WIN32
    {
        int p;
        for (p = 0; p < numVFSPaths; p++)
            InitPakFile(vfsPaths[p], NULL);
    }
#endif

    // load the bsp
    sprintf(name, "%s%s", inbase, ExpandArg(argv[i]));
    StripExtension(name);
    strcat(name, ".bsp");
    _printf("reading %s\n", name);
    LoadBSPFile(name);
    ParseEntities();

    // load the portal file
    sprintf(portalfile, "%s%s", inbase, ExpandArg(argv[i]));
    StripExtension(portalfile);
    strcat(portalfile, ".prt");
    _printf("reading %s\n", portalfile);
    LoadPortals(portalfile);

    CalculateVisibility(mergevis);

    //	CalcPHS ();

    // delete the prt file
    if (!saveprt)
    {
        remove(portalfile);
    }

    // write the bsp file
    _printf("writing %s\n", name);
    WriteBSPFile(name);

    FreeVisibility();

#ifdef MREDEBUG
    end = clock();
    _printf("%5.2f seconds elapsed\n", (end - start) / CLK_TCK);
#else
    end = I_FloatTime();
    _printf("%5.2f seconds elapsed\n", end - start);
#endif
    return 0;
}
