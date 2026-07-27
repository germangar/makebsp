/*
===========================================================================
Map Exporter: Export in-memory entities to Brush Primitives map format
===========================================================================
*/

#include "qbsp.h"
#include "map_export.h"

static void GetSideTexMat(side_t *s, float outTexMat[2][3])
{
    if (g_bBrushPrimit == BPRIMIT_NEWBRUSHES)
    {
        memcpy(outTexMat, s->texMat, sizeof(s->texMat));
        return;
    }

    vec3_t texX, texY;
    ComputeAxisBase(mapplanes[s->planenum].normal, texX, texY);

    float width = (s->shaderInfo && s->shaderInfo->width > 0) ? (float)s->shaderInfo->width : 512.0f;
    float height = (s->shaderInfo && s->shaderInfo->height > 0) ? (float)s->shaderInfo->height : 512.0f;

    outTexMat[0][0] = DotProduct(s->vecs[0], texX) / width;
    outTexMat[0][1] = DotProduct(s->vecs[0], texY) / width;
    outTexMat[0][2] = (s->vecs[0][3] + mapplanes[s->planenum].dist * DotProduct(s->vecs[0], mapplanes[s->planenum].normal)) / width;

    outTexMat[1][0] = DotProduct(s->vecs[1], texX) / height;
    outTexMat[1][1] = DotProduct(s->vecs[1], texY) / height;
    outTexMat[1][2] = (s->vecs[1][3] + mapplanes[s->planenum].dist * DotProduct(s->vecs[1], mapplanes[s->planenum].normal)) / height;
}

static void WriteBrush(FILE *f, bspbrush_t *b)
{
    int i;
    fprintf(f, "{\nbrushDef\n{\n");
    for (i = 0; i < b->numsides; i++)
    {
        side_t *s = &b->sides[i];
        winding_t *w;

        // Skip internal/generated planes (e.g., axial bevels for collision)
        if (s->bevel || s->backSide || !s->shaderInfo)
        {
            continue;
        }

        w = BaseWindingForPlane(mapplanes[s->planenum].normal, mapplanes[s->planenum].dist);

        fprintf(f, "( %.3f %.3f %.3f ) ", w->points[0][0], w->points[0][1], w->points[0][2]);
        fprintf(f, "( %.3f %.3f %.3f ) ", w->points[1][0], w->points[1][1], w->points[1][2]);
        fprintf(f, "( %.3f %.3f %.3f ) ", w->points[2][0], w->points[2][1], w->points[2][2]);

        FreeWinding(w);

        float texMat[2][3];
        GetSideTexMat(s, texMat);

        fprintf(f, "( ( %.6f %.6f %.6f ) ( %.6f %.6f %.6f ) ) ",
                texMat[0][0], texMat[0][1], texMat[0][2],
                texMat[1][0], texMat[1][1], texMat[1][2]);

        const char *shader = "common/caulk";
        if (s->shaderInfo && s->shaderInfo->shader && s->shaderInfo->shader[0])
        {
            shader = s->shaderInfo->shader;
        }
        if (!Q_strncasecmp(shader, "textures/", 9))
        {
            shader += 9;
        }

        int exportContents = (s->contents & CONTENTS_DETAIL) ? CONTENTS_DETAIL : 0;
        fprintf(f, "%s %d 0 0\n", shader, exportContents);
    }
    fprintf(f, "}\n}\n");
}

static void WritePatch(FILE *f, parseMesh_t *p)
{
    int i, j;
    const char *shader = "common/caulk";
    if (p->shaderInfo && p->shaderInfo->shader && p->shaderInfo->shader[0])
    {
        shader = p->shaderInfo->shader;
    }
    if (!Q_strncasecmp(shader, "textures/", 9))
    {
        shader += 9;
    }

    fprintf(f, "{\npatchDef2\n{\n");
    fprintf(f, "%s\n", shader);
    fprintf(f, "( %i %i 0 0 0 )\n", p->mesh.width, p->mesh.height);
    fprintf(f, "(\n");

    for (j = 0; j < p->mesh.width; j++)
    {
        fprintf(f, "( ");
        for (i = 0; i < p->mesh.height; i++)
        {
            drawVert_t *v = &p->mesh.verts[i * p->mesh.width + j];
            fprintf(f, "( %.3f %.3f %.3f %.5f %.5f ) ",
                    v->xyz[0], v->xyz[1], v->xyz[2], v->st[0], v->st[1]);
        }
        fprintf(f, ")\n");
    }

    fprintf(f, ")\n}\n}\n");
}

void ExportMapAsBrushPrimitives(const char *filename)
{
    FILE *f;
    int i;
    bspbrush_t *b;
    parseMesh_t *p;
    epair_t *ep;

    _printf("--- ExportMapAsBrushPrimitives: writing %s ---\n", filename);

    f = fopen(filename, "wb");
    if (!f)
    {
        Error("ExportMapAsBrushPrimitives: Can't write %s\n", filename);
    }

    for (i = 0; i < num_entities; i++)
    {
        entity_t *ent = &entities[i];

        if (!ent->epairs)
        {
            continue;
        }

        fprintf(f, "// Entity %i\n{\n", i);

        for (ep = ent->epairs; ep; ep = ep->next)
        {
            fprintf(f, "\"%s\" \"%s\"\n", ep->key, ep->value);
        }

        for (b = ent->brushes; b; b = b->next)
        {
            WriteBrush(f, b);
        }

        for (p = ent->patches; p; p = p->next)
        {
            WritePatch(f, p);
        }

        fprintf(f, "}\n");
    }

    fclose(f);
}
