#include "qbsp.h"

/*
====================
ExportModels
====================
*/
static qboolean IsPlanar(dsurface_t *ds)
{
    vec3_t normal;
    vec3_t v0;
    qboolean foundPlane = qfalse;
    int k;

    for (k = 0; k < ds->numIndexes; k += 3)
    {
        int i1 = drawIndexes[ds->firstIndex + k + 0];
        int i2 = drawIndexes[ds->firstIndex + k + 1];
        int i3 = drawIndexes[ds->firstIndex + k + 2];
        vec3_t d1, d2;
        VectorSubtract(drawVerts[ds->firstVert + i2].xyz, drawVerts[ds->firstVert + i1].xyz, d1);
        VectorSubtract(drawVerts[ds->firstVert + i3].xyz, drawVerts[ds->firstVert + i1].xyz, d2);
        CrossProduct(d1, d2, normal);
        float len = VectorLength(normal);
        if (len > 0.1f)
        {
            VectorScale(normal, 1.0f / len, normal);
            VectorCopy(drawVerts[ds->firstVert + i1].xyz, v0);
            foundPlane = qtrue;
            break;
        }
    }

    if (!foundPlane) return qtrue; // Degenerate is technically planar

    float dist = DotProduct(normal, v0);

    for (k = 0; k < ds->numVerts; k++)
    {
        float d = DotProduct(normal, drawVerts[ds->firstVert + k].xyz) - dist;
        if (fabs(d) > 0.5f) return qfalse; // Allow some epsilon
    }
    return qtrue;
}

void ExportModels(int count, char **args)
{
    int i, j, k;
    char source[1024];
    char base[1024];
    char mtlName[1024];
    char objName[1024];
    char outDir[1024];
    FILE *fObj, *fMtl;
    qboolean ignorePlanar = qtrue;

    int argStart = 0;
    for (argStart = 0; argStart < count; argStart++)
    {
        if (!strcmp(args[argStart], "-ignoreplanar"))
        {
            if (argStart + 1 < count && (args[argStart + 1][0] == '0' || args[argStart + 1][0] == '1'))
            {
                ignorePlanar = atoi(args[argStart + 1]) != 0 ? qtrue : qfalse;
                argStart++;
            }
        }
        else if (args[argStart][0] == '-')
        {
            _printf("WARNING: Unknown option %s\n", args[argStart]);
        }
        else
        {
            break;
        }
    }

    int fileCount = count - argStart;
    char **fileNames = args + argStart;

    if (fileCount < 1)
    {
        _printf("No files to export models from.\n");
        return;
    }

    for (i = 0; i < fileCount; i++)
    {
        strcpy(source, fileNames[i]);
        StripExtension(source);
        DefaultExtension(source, ".bsp");
        ExtractFileBase(source, base);

        // Determine output directory: same dir as BSP + /<mapname>_models
        ExtractFilePath(source, outDir);
        strcat(outDir, base);
        strcat(outDir, "_models");
        Q_mkdir(outDir);

        _printf("--- Exporting models from %s into %s/ ---\n", source, outDir);
        if (ignorePlanar) {
            _printf("Ignoring planar surfaces.\n");
        }

        LoadBSPFile(source);

        int exportedCount = 0;
        int skippedCount = 0;
        for (j = 0; j < numDrawSurfaces; j++)
        {
            dsurface_t *ds = &drawSurfaces[j];
            if (ds->surfaceType != MST_TRIANGLE_SOUP)
                continue;

            if (ignorePlanar && IsPlanar(ds))
            {
                skippedCount++;
                continue;
            }

            sprintf(objName, "%s/%s_surf%04d.obj", outDir, base, j);
            sprintf(mtlName, "%s/%s_surf%04d.mtl", outDir, base, j);
            
            // Create MTL for THIS surface
            fMtl = fopen(mtlName, "w");
            if (!fMtl)
            {
                _printf("ERROR: Could not open %s for writing\n", mtlName);
                continue;
            }

            dshader_t *sh = &dshaders[ds->shaderNum];
            fprintf(fMtl, "newmtl %s\n", sh->shader);
            fprintf(fMtl, "Kd 1.0 1.0 1.0\n");
            fprintf(fMtl, "map_Kd %s.tga\n\n", sh->shader);
            fclose(fMtl);

            // Create OBJ
            fObj = fopen(objName, "w");
            if (!fObj)
            {
                _printf("ERROR: Could not open %s for writing\n", objName);
                continue;
            }

            // Reference MTL filename only (no path)
            char *mtlRef = strrchr(mtlName, '/');
            if (!mtlRef)
                mtlRef = strrchr(mtlName, '\\');
            if (mtlRef)
                mtlRef++;
            else
                mtlRef = mtlName;

            fprintf(fObj, "mtllib %s\n", mtlRef);
            fprintf(fObj, "o surface_%d\n", j);

            // Vertices
            for (k = 0; k < ds->numVerts; k++)
            {
                drawVert_t *dv = &drawVerts[ds->firstVert + k];
                // Axis swap: Q3 Z-up -> OBJ Y-up (X=X, Y=Z, Z=-Y)
                fprintf(fObj, "v %f %f %f\n", dv->xyz[0], dv->xyz[2], -dv->xyz[1]);
            }

            // Texture Coordinates
            for (k = 0; k < ds->numVerts; k++)
            {
                drawVert_t *dv = &drawVerts[ds->firstVert + k];
                fprintf(fObj, "vt %f %f\n", dv->st[0], 1.0f - dv->st[1]);
            }

            // Normals
            for (k = 0; k < ds->numVerts; k++)
            {
                drawVert_t *dv = &drawVerts[ds->firstVert + k];
                fprintf(fObj, "vn %f %f %f\n", dv->normal[0], dv->normal[2], -dv->normal[1]);
            }

            fprintf(fObj, "usemtl %s\n", sh->shader);

            // Faces (1-indexed, flipped winding to fix backface culling)
            for (k = 0; k < ds->numIndexes; k += 3)
            {
                int i1 = drawIndexes[ds->firstIndex + k + 0] + 1;
                int i2 = drawIndexes[ds->firstIndex + k + 2] + 1;
                int i3 = drawIndexes[ds->firstIndex + k + 1] + 1;
                fprintf(fObj, "f %d/%d/%d %d/%d/%d %d/%d/%d\n", i1, i1, i1, i2, i2, i2, i3, i3, i3);
            }

            fclose(fObj);
            exportedCount++;
        }

        if (ignorePlanar)
            _printf("Exported %d trisoup models as OBJ/MTL pairs (skipped %d planar surfaces).\n", exportedCount, skippedCount);
        else
            _printf("Exported %d trisoup models as OBJ/MTL pairs.\n", exportedCount);
    }
}
