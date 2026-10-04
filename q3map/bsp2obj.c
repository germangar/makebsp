#include "qbsp.h"
#include <ctype.h>

/*
====================
Bsp2Obj

Converts a compiled BSP into a single Wavefront OBJ/MTL
model. See bsp2obj_study.txt for the implementation plan.
====================
*/

#define BSP2OBJ_NAME_CAP       96
#define BSP2OBJ_STREAM_BUF     (4 * 1024 * 1024)
#define BSP2OBJ_COLINEAR_AREA  10

typedef struct
{
    char raw[MAX_QPATH];
    char name[BSP2OBJ_NAME_CAP];
    int  shaderNum;
} objMaterial_t;

#define C_OTHER     0
#define C_INVALID   1
#define C_STORED    2
#define C_PATCHGEN  3

static int           *s_class;
static int           *s_mat;
static objMaterial_t *s_materials;
static int            s_numMaterials;

static int c_models, c_modelskip;
static int c_planar, c_patch, c_soup;
static int c_other, c_invalid;
static int c_verts, c_facesStored, c_facesPatch, c_degen;

static void SanitizeName(const char *in, char *out, int cap)
{
    int i, n = 0;

    for (i = 0; in[i] && n < cap - 1; i++)
    {
        unsigned char ch = (unsigned char)in[i];
        out[n++] = (isalnum(ch) || ch == '_') ? ch : '_';
    }
    out[n] = 0;
}

static void StripImageExtension(const char *in, char *out, int cap)
{
    static const char *exts[] = { "tga", "jpg", "jpeg", "png", "dds",
        "pcx", "wal", "bmp", "tif", "tiff", NULL };
    const char *dot;
    int e;

    snprintf(out, cap, "%s", in);

    dot = strrchr(out, '.');
    if (!dot || !dot[1])
        return;

    for (e = 0; exts[e]; e++)
    {
        if (!Q_stricmp(dot + 1, exts[e]))
        {
            out[dot - out] = 0;
            break;
        }
    }
}

static qboolean TriDegenerate(drawVert_t *points, int a, int b, int c)
{
    vec3_t v1, v2, v3;
    float d;

    VectorSubtract(points[b].xyz, points[a].xyz, v1);
    VectorSubtract(points[c].xyz, points[a].xyz, v2);
    CrossProduct(v1, v2, v3);
    d = VectorLength(v3);

    if (d < BSP2OBJ_COLINEAR_AREA)
        return qtrue;

    return qfalse;
}

static int ClassifySurface(dsurface_t *ds)
{
    if (ds->surfaceType != MST_PLANAR &&
        ds->surfaceType != MST_PATCH &&
        ds->surfaceType != MST_TRIANGLE_SOUP)
        return C_OTHER;

    if (ds->shaderNum < 0 || ds->shaderNum >= numShaders)
        return C_INVALID;
    if (ds->numVerts <= 0 || ds->firstVert < 0 ||
        ds->firstVert + ds->numVerts > numDrawVerts)
        return C_INVALID;
    if (ds->numIndexes < 0 || ds->firstIndex < 0 ||
        ds->firstIndex + ds->numIndexes > numDrawIndexes ||
        (ds->numIndexes % 3) != 0)
        return C_INVALID;

    if (ds->surfaceType == MST_PATCH && ds->numIndexes == 0)
    {
        if (ds->patchWidth < 0 || ds->patchHeight < 0 ||
            ds->patchWidth > 4096 || ds->patchHeight > 4096)
            return C_INVALID;
        if (ds->numVerts < ds->patchWidth * ds->patchHeight)
            return C_INVALID;
        return C_PATCHGEN;
    }

    if (ds->numIndexes < 3)
        return C_INVALID;

    return C_STORED;
}

static int RegisterMaterial(int shaderNum)
{
    int i;
    objMaterial_t *mat;

    for (i = 0; i < s_numMaterials; i++)
    {
        if (s_materials[i].shaderNum == shaderNum)
            return i;
    }

    mat = &s_materials[s_numMaterials];
    mat->shaderNum = shaderNum;
    strncpy(mat->raw, dshaders[shaderNum].shader, MAX_QPATH - 1);
    mat->raw[MAX_QPATH - 1] = 0;

    if (strpbrk(mat->raw, " \t\r\n"))
    {
        char suffix[16];
        SanitizeName(mat->raw, mat->name, BSP2OBJ_NAME_CAP - 16);
        snprintf(suffix, sizeof(suffix), "_s%d", shaderNum);
        strcat(mat->name, suffix);
    }
    else
    {
        snprintf(mat->name, sizeof(mat->name), "%s", mat->raw);
    }

    return s_numMaterials++;
}

static void ClassifyAll(void)
{
    int m, s;

    memset(s_class, 0, numDrawSurfaces * sizeof(int));
    for (s = 0; s < numDrawSurfaces; s++)
        s_mat[s] = -1;

    for (m = 0; m < nummodels; m++)
    {
        dmodel_t *mod = &dmodels[m];
        int first = mod->firstSurface;
        int count = mod->numSurfaces;
        int sIdx;

        if (count <= 0)
            continue;
        if (first < 0 || first + count > numDrawSurfaces)
            continue;

        for (sIdx = first; sIdx < first + count; sIdx++)
        {
            int cls = ClassifySurface(&drawSurfaces[sIdx]);
            s_class[sIdx] = cls;

            if (cls == C_OTHER)
                c_other++;
            else if (cls == C_INVALID)
                c_invalid++;
            else
            {
                if (drawSurfaces[sIdx].surfaceType == MST_PLANAR)
                    c_planar++;
                else if (drawSurfaces[sIdx].surfaceType == MST_PATCH)
                    c_patch++;
                else
                    c_soup++;

                s_mat[sIdx] = RegisterMaterial(drawSurfaces[sIdx].shaderNum);
            }
        }
    }
}

static void WriteMTL(const char *mtlName)
{
    FILE *f;
    int i;

    f = fopen(mtlName, "w");
    if (!f)
    {
        _printf("ERROR: Could not open %s for writing\n", mtlName);
        return;
    }
    setvbuf(f, NULL, _IOFBF, BSP2OBJ_STREAM_BUF);

    fprintf(f, "# Generated by makebsp -bsp2obj\n");

    for (i = 0; i < s_numMaterials; i++)
    {
        char noext[MAX_QPATH];
        StripImageExtension(s_materials[i].raw, noext, sizeof(noext));
        fprintf(f, "newmtl %s\n", s_materials[i].name);
        fprintf(f, "Kd 1.0 1.0 1.0\n");
        fprintf(f, "map_Kd %s\n\n", noext);
    }

    fclose(f);
}

static void EmitFace(FILE *f, int a, int b, int c)
{
    fprintf(f, "f %d/%d/%d %d/%d/%d %d/%d/%d\n", a, a, a, b, b, b, c, c, c);
}

static int EmitStoredFaces(FILE *fObj, dsurface_t *ds, int base)
{
    int k, faces = 0;

    for (k = 0; k + 2 < ds->numIndexes; k += 3)
    {
        int a = drawIndexes[ds->firstIndex + k + 0];
        int b = drawIndexes[ds->firstIndex + k + 1];
        int c = drawIndexes[ds->firstIndex + k + 2];

        if (a < 0 || a >= ds->numVerts || b < 0 || b >= ds->numVerts ||
            c < 0 || c >= ds->numVerts)
        {
            c_degen++;
            continue;
        }
        if (a == b || b == c || a == c)
        {
            c_degen++;
            continue;
        }

        EmitFace(fObj, base + a + 1, base + c + 1, base + b + 1);
        faces++;
    }

    return faces;
}

static int EmitPatchGridFaces(FILE *fObj, dsurface_t *ds, int base)
{
    int W = ds->patchWidth;
    int H = ds->patchHeight;
    drawVert_t *verts = drawVerts + ds->firstVert;
    int i, j, faces = 0;

    for (j = 0; j + 1 < H; j++)
    {
        for (i = 0; i + 1 < W; i++)
        {
            int v0 = j * W + i;
            int v1 = v0 + 1;
            int v2 = v0 + W;
            int v3 = v2 + 1;

            if (!TriDegenerate(verts, v0, v2, v1))
            {
                EmitFace(fObj, base + v0 + 1, base + v1 + 1, base + v2 + 1);
                faces++;
            }
            else
                c_degen++;

            if (!TriDegenerate(verts, v1, v2, v3))
            {
                EmitFace(fObj, base + v1 + 1, base + v3 + 1, base + v2 + 1);
                faces++;
            }
            else
                c_degen++;
        }
    }

    return faces;
}

static void WriteOBJModel(const char *objName, const char *base, const char *source)
{
    FILE *fObj;
    int m, k, runningVerts = 0;

    fObj = fopen(objName, "w");
    if (!fObj)
    {
        _printf("ERROR: Could not open %s for writing\n", objName);
        return;
    }
    setvbuf(fObj, NULL, _IOFBF, BSP2OBJ_STREAM_BUF);

    fprintf(fObj, "# Generated by makebsp -bsp2obj\n");
    fprintf(fObj, "# Source: %s\n", source);
    fprintf(fObj, "# Axis swap: Q3 Z-up -> OBJ Y-up (X=X, Y=Z, Z=-Y)\n");
    fprintf(fObj, "o %s\n", base);
    fprintf(fObj, "mtllib %s.mtl\n", base);

    for (m = 0; m < nummodels; m++)
    {
        dmodel_t *mod = &dmodels[m];
        int first = mod->firstSurface;
        int count = mod->numSurfaces;
        int sIdx;

        if (count <= 0 || first < 0 || first + count > numDrawSurfaces)
        {
            c_modelskip++;
            continue;
        }
        c_models++;

        for (sIdx = first; sIdx < first + count; sIdx++)
        {
            dsurface_t *ds = &drawSurfaces[sIdx];
            drawVert_t *dv;
            int cls = s_class[sIdx];
            int vbase = runningVerts;

            if (cls != C_STORED && cls != C_PATCHGEN)
                continue;

            fprintf(fObj, "g mat%dm%ds%d\n", s_mat[sIdx], m, sIdx);
            fprintf(fObj, "usemtl %s\n", s_materials[s_mat[sIdx]].name);

            dv = drawVerts + ds->firstVert;
            for (k = 0; k < ds->numVerts; k++, dv++)
            {
                fprintf(fObj, "v %.6f %.6f %.6f\n",
                        dv->xyz[0], dv->xyz[2], -dv->xyz[1]);
                fprintf(fObj, "vt %.6f %.6f\n",
                        dv->st[0], 1.0f - dv->st[1]);
                fprintf(fObj, "vn %.6f %.6f %.6f\n",
                        dv->normal[0], dv->normal[2], -dv->normal[1]);
            }
            c_verts += ds->numVerts;

            if (cls == C_STORED)
                c_facesStored += EmitStoredFaces(fObj, ds, vbase);
            else
                c_facesPatch += EmitPatchGridFaces(fObj, ds, vbase);

            runningVerts += ds->numVerts;
        }
    }

    fclose(fObj);
}

void Bsp2Obj(int count, char **args)
{
    int argStart, fileCount, i;
    char source[1024];
    char base[MAX_QPATH];
    char filePath[1024];
    char objName[1024];
    char mtlName[1024];

    for (argStart = 0; argStart < count; argStart++)
    {
        if (args[argStart][0] == '-')
            _printf("WARNING: Unknown option %s\n", args[argStart]);
        else
            break;
    }

    fileCount = count - argStart;
    if (fileCount < 1)
    {
        _printf("No files to convert.\n");
        return;
    }

    for (i = 0; i < fileCount; i++)
    {
        strcpy(source, args[argStart + i]);
        StripExtension(source);
        DefaultExtension(source, ".bsp");
        ExtractFileBase(source, base);
        ExtractFilePath(source, filePath);

        snprintf(objName, sizeof(objName), "%s%s.obj", filePath, base);
        snprintf(mtlName, sizeof(mtlName), "%s%s.mtl", filePath, base);

        _printf("--- bsp2obj: %s -> %s ---\n", source, objName);

        LoadBSPFile(source);

        if (numDrawSurfaces <= 0)
        {
            _printf("No draw surfaces found.\n");
            continue;
        }
        ParseEntities();

        c_models = c_modelskip = 0;
        c_planar = c_patch = c_soup = 0;
        c_other = c_invalid = 0;
        c_verts = c_facesStored = c_facesPatch = c_degen = 0;

        s_class = malloc(numDrawSurfaces * sizeof(int));
        s_mat = malloc(numDrawSurfaces * sizeof(int));
        s_materials = malloc((numShaders > 0 ? numShaders : 1) * sizeof(objMaterial_t));
        s_numMaterials = 0;

        if (!s_class || !s_mat || !s_materials)
        {
            _printf("ERROR: Out of memory\n");
            free(s_class);
            free(s_mat);
            free(s_materials);
            s_class = s_mat = NULL;
            s_materials = NULL;
            return;
        }

        ClassifyAll();
        WriteMTL(mtlName);
        WriteOBJModel(objName, base, source);

        _printf("%d models exported (%d empty skipped)\n", c_models, c_modelskip);
        _printf("%d surfaces: %d planar, %d patch, %d trisoup\n",
                c_planar + c_patch + c_soup, c_planar, c_patch, c_soup);
        _printf("skipped: %d flares/other, %d invalid\n", c_other, c_invalid);
        _printf("%d verts, %d faces (%d stored + %d patch-generated, "
                "%d degenerate skipped)\n",
                c_verts, c_facesStored + c_facesPatch, c_facesStored,
                c_facesPatch, c_degen);

        free(s_class);
        free(s_mat);
        free(s_materials);
        s_class = s_mat = NULL;
        s_materials = NULL;
    }
}
