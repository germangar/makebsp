#include "qbsp.h"

/*
====================
ExportModels
====================
*/
void ExportModels(int count, char **fileNames)
{
    int i, j, k;
    char source[1024];
    char base[1024];
    char mtlName[1024];
    char objName[1024];
    char outDir[1024];
    FILE *fObj, *fMtl;

    if (count < 1)
    {
        _printf("No files to export models from.\n");
        return;
    }

    for (i = 0; i < count; i++)
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

        LoadBSPFile(source);

        int exportedCount = 0;
        for (j = 0; j < numDrawSurfaces; j++)
        {
            dsurface_t *ds = &drawSurfaces[j];
            if (ds->surfaceType != MST_TRIANGLE_SOUP)
                continue;

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

        _printf("Exported %d trisoup models as OBJ/MTL pairs.\n", exportedCount);
    }
}
