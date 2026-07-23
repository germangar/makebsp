/*
===========================================================================
csg_brush.c

Contains CSG Merge operations for collision brushes.
===========================================================================
*/
#include "qbsp.h"

/*
==================
CSGMergeBrushes

Tests whether two convex brushes sharing a face can be merged into a single
strictly convex brush. If so, outputs the new brush and returns qtrue.

This uses a mathematical volume check: if building a candidate brush from
all non-shared planes yields a shape whose total volume exactly equals
Volume(A) + Volume(B), the union is proven mathematically safe and convex.
==================
*/
static qboolean CSGMergeBrushes(bspbrush_t *a, bspbrush_t *b, bspbrush_t **out)
{
    int i, j;
    int shared_a = -1;
    int shared_b = -1;
    bspbrush_t *c;
    int maxSides;
    vec_t volA, volB, volC;
    qboolean duplicate;

    if (!a || !b || !out)
        return qfalse;

    // 1. Find a shared opposite plane.
    for (i = 0; i < a->numsides; i++)
    {
        for (j = 0; j < b->numsides; j++)
        {
            if (a->sides[i].planenum == (b->sides[j].planenum ^ 1))
            {
                shared_a = i;
                shared_b = j;
                break;
            }
        }
        if (shared_a != -1)
            break;
    }

    // Not sharing an opposite face
    if (shared_a == -1)
    {
        return qfalse;
    }

    // 2. Create the candidate brush
    maxSides = a->numsides + b->numsides - 2;
    c = AllocBrush(maxSides);
    c->numsides = 0;

    // Copy sides from A (except the shared face)
    for (i = 0; i < a->numsides; i++)
    {
        if (i == shared_a)
            continue;
        c->sides[c->numsides] = a->sides[i];
        c->sides[c->numsides].winding = NULL; // New brush needs fresh windings
        c->numsides++;
    }

    // Copy sides from B (except shared face AND duplicates of A's geometry)
    for (j = 0; j < b->numsides; j++)
    {
        if (j == shared_b)
            continue;

        duplicate = qfalse;
        for (i = 0; i < a->numsides; i++)
        {
            if (i == shared_a)
                continue;
            if (a->sides[i].planenum == b->sides[j].planenum)
            {
                duplicate = qtrue;
                break;
            }
        }

        if (!duplicate)
        {
            c->sides[c->numsides] = b->sides[j];
            c->sides[c->numsides].winding = NULL;
            c->numsides++;
        }
    }

    // Inherit structural properties
    c->original = a->original;
    c->detail = a->detail;
    c->opaque = a->opaque;
    c->contents = a->contents;
    c->contentShader = a->contentShader;
    c->epairs = CopyEpairs(a->epairs);

    // 3. Create windings for the candidate brush
    if (!CreateBrushWindings(c))
    {
        FreeBrush(c);
        return qfalse;
    }

    // 4. Verify Convexity via Volume Check
    volA = BrushVolume(a);
    volB = BrushVolume(b);
    volC = BrushVolume(c);

    if (volC != volC)
    {
        FreeBrush(c);
        return qfalse; // NaN
    }

    vec_t expected = volA + volB;
    // Use a tight epsilon: 0.1% of volume or tiny absolute floor for float noise
    vec_t epsilon = expected * 0.001f + 0.0025f;

    if (fabs(volC - expected) > epsilon)
    {
        FreeBrush(c);
        return qfalse;
    }

    *out = c;
    return qtrue;
}

/*
==================
CSGMergeBrushList

Takes a pointer to a linked list of bspbrush_t, tests every possible pair O(N^2),
and merges touching convex brushes iteratively until no further merges occur.
==================
*/
int CSGMergeBrushList(bspbrush_t **pList)
{
    qboolean merged_any;
    int total_merges = 0;
    bspbrush_t *b1, *b2, *prev1, *prev2;
    bspbrush_t *merged_brush;

    if (!pList || !*pList)
        return 0;

    do
    {
        merged_any = qfalse;
        b1 = *pList;
        prev1 = NULL;

        while (b1)
        {
            b2 = b1->next;
            prev2 = b1;

            while (b2)
            {
                merged_brush = NULL;

                if (CSGMergeBrushes(b1, b2, &merged_brush))
                {
                    // Replace b2
                    prev2->next = b2->next;
                    FreeBrush(b2);

                    // Replace b1
                    merged_brush->next = b1->next;
                    if (prev1)
                    {
                        prev1->next = merged_brush;
                    }
                    else
                    {
                        *pList = merged_brush;
                    }
                    FreeBrush(b1);

                    // b1 pointer now tracks the newly constructed unified brush
                    b1 = merged_brush;
                    merged_any = qtrue;
                    total_merges++;

                    // Restart inner check against the newly enlarged b1 brush
                    b2 = b1->next;
                    prev2 = b1;
                }
                else
                {
                    // Advance b2
                    prev2 = b2;
                    b2 = b2->next;
                }
            }

            // Advance b1
            prev1 = b1;
            b1 = b1->next;
        }
    } while (merged_any);

    if (total_merges > 0)
    {
        _printf("CSG Merge: %d brushes dynamically fused\n", total_merges);
    }

    return total_merges;
}
