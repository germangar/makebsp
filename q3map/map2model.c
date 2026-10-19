#include "qbsp.h"
#include <ctype.h>

/*
====================
map2model.c

Exports the in-memory compiled map state to a model file (Wavefront
OBJ/MTL and ASCII Scene ASE). The traversal, surface classification,
material registry and vertex scaling are shared here; each output
format is a thin modelWriter_t implementation fed by the RunModelExport
walk. Driven by the -map2obj and -map2ase compile modes.
====================
*/

#define MAP2MODEL_NAME_CAP     96
#define MAP2MODEL_STREAM_BUF   (4 * 1024 * 1024)

typedef struct
{
    char raw[MAX_QPATH];
    char name[MAP2MODEL_NAME_CAP];
    int  shaderNum;
} modelMaterial_t;

#define C_OTHER     0
#define C_INVALID   1
#define C_PATCHSKIP 2
#define C_NODRAW    3
#define C_STORED    4

static int             *s_class;
static int             *s_mat;
static modelMaterial_t *s_materials;
static int              s_numMaterials;

static int c_models, c_modelskip;
static int c_planar, c_soup, c_patch;
static int c_nodraw, c_other, c_invalid;
static int c_verts, c_facesStored, c_degen;

/*
====================
modelWriter_t

Format-specific output sink. The shared walk (RunModelExport) owns every
geometry decision (surface filtering, vertex scaling, degenerate
rejection, material/surface grouping); a writer only turns the callbacks
into syntax. 'face' receives surface-local 0-based indices (a, b, c).
'beginSurface' also passes the surface's vertex count and face capacity
(upper bound, before degenerate rejection) so buffering writers can
allocate. Writers receive raw Q3-axis data (positions scaled about the
world origin, UVs/normals unmodified); any axis or UV convention belongs
in the writer.
====================
*/
typedef struct
{
    void (*begin)(void *ctx, const char *base, const char *source);
    void (*beginModel)(void *ctx, int modelIndex);
    void (*beginSurface)(void *ctx, int modelIndex, int surfIndex, int matIndex, int numVerts, int numFaces);
    void (*vertex)(void *ctx, const vec3_t pos, const vec2_t uv, const vec3_t normal);
    void (*face)(void *ctx, int a, int b, int c);
    void (*endSurface)(void *ctx);
    void (*endModel)(void *ctx);
    void (*end)(void *ctx);
} modelWriter_t;

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

static void ModelVertex(const drawVert_t *dv, float scale,
                        vec3_t pos, vec2_t uv, vec3_t normal)
{
    pos[0] = dv->xyz[0] * scale;
    pos[1] = dv->xyz[1] * scale;
    pos[2] = dv->xyz[2] * scale;
    uv[0] = dv->st[0];
    uv[1] = dv->st[1];
    normal[0] = dv->normal[0];
    normal[1] = dv->normal[1];
    normal[2] = dv->normal[2];
}

static int ClassifySurface(dsurface_t *ds)
{
    if (ds->surfaceType == MST_PATCH)
        return C_PATCHSKIP;

    if (ds->surfaceType != MST_PLANAR &&
        ds->surfaceType != MST_TRIANGLE_SOUP)
        return C_OTHER;

    if (ds->shaderNum < 0 || ds->shaderNum >= numShaders)
        return C_INVALID;
    if (dshaders[ds->shaderNum].surfaceFlags & SURF_NODRAW)
        return C_NODRAW;
    if (ds->numVerts <= 0 || ds->firstVert < 0 ||
        ds->firstVert + ds->numVerts > numDrawVerts)
        return C_INVALID;
    if (ds->numIndexes < 0 || ds->firstIndex < 0 ||
        ds->firstIndex + ds->numIndexes > numDrawIndexes ||
        (ds->numIndexes % 3) != 0)
        return C_INVALID;

    if (ds->numIndexes < 3)
        return C_INVALID;

    return C_STORED;
}

static int RegisterMaterial(int shaderNum)
{
    int i;
    modelMaterial_t *mat;

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
        SanitizeName(mat->raw, mat->name, MAP2MODEL_NAME_CAP - 16);
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

            if (cls == C_PATCHSKIP)
                c_patch++;
            else if (cls == C_NODRAW)
                c_nodraw++;
            else if (cls == C_OTHER)
                c_other++;
            else if (cls == C_INVALID)
                c_invalid++;
            else
            {
                if (drawSurfaces[sIdx].surfaceType == MST_PLANAR)
                    c_planar++;
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
    setvbuf(f, NULL, _IOFBF, MAP2MODEL_STREAM_BUF);

    fprintf(f, "# Generated by makebsp -map2obj\n");

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

/*
====================
RunModelExport

The single, format-agnostic geometry walk. It owns model/surface
iteration, the C_STORED filter, the vertex scaling, degenerate-face
rejection and all export counters; it forwards syntax-free callbacks to
the writer.
====================
*/
static void RunModelExport(const modelWriter_t *w, void *ctx,
                           const char *base, const char *source, float scale)
{
    int m, sIdx, k;

    w->begin(ctx, base, source);

    for (m = 0; m < nummodels; m++)
    {
        dmodel_t *mod = &dmodels[m];
        int first = mod->firstSurface;
        int count = mod->numSurfaces;

        if (count <= 0 || first < 0 || first + count > numDrawSurfaces)
        {
            c_modelskip++;
            continue;
        }
        c_models++;

        w->beginModel(ctx, m);

        for (sIdx = first; sIdx < first + count; sIdx++)
        {
            dsurface_t *ds = &drawSurfaces[sIdx];
            drawVert_t *dv;

            if (s_class[sIdx] != C_STORED)
                continue;

            w->beginSurface(ctx, m, sIdx, s_mat[sIdx],
                            ds->numVerts, ds->numIndexes / 3);

            dv = drawVerts + ds->firstVert;
            for (k = 0; k < ds->numVerts; k++, dv++)
            {
                vec3_t pos, normal;
                vec2_t uv;

                ModelVertex(dv, scale, pos, uv, normal);
                w->vertex(ctx, pos, uv, normal);
            }
            c_verts += ds->numVerts;

            for (k = 0; k + 2 < ds->numIndexes; k += 3)
            {
                int a = drawIndexes[ds->firstIndex + k + 0];
                int b = drawIndexes[ds->firstIndex + k + 1];
                int c = drawIndexes[ds->firstIndex + k + 2];

                if (a < 0 || a >= ds->numVerts || b < 0 || b >= ds->numVerts ||
                    c < 0 || c >= ds->numVerts ||
                    a == b || b == c || a == c)
                {
                    c_degen++;
                    continue;
                }

                w->face(ctx, a, b, c);
                c_facesStored++;
            }

            w->endSurface(ctx);
        }

        w->endModel(ctx);
    }

    w->end(ctx);
}

/*
====================
Wavefront OBJ writer
====================
*/
typedef struct
{
    FILE *f;
    int   runningVerts;
    int   vbase;
    int   surfaceVerts;
} objWriterCtx_t;

static void Obj_Begin(void *vctx, const char *base, const char *source)
{
    objWriterCtx_t *ctx = (objWriterCtx_t *)vctx;

    fprintf(ctx->f, "# Generated by makebsp -map2obj\n");
    fprintf(ctx->f, "# Source: %s\n", source);
    fprintf(ctx->f, "# Axis swap: Q3 Z-up -> OBJ Y-up (X=X, Y=Z, Z=-Y)\n");
    fprintf(ctx->f, "o %s\n", base);
    fprintf(ctx->f, "mtllib %s.mtl\n", base);
}

static void Obj_BeginModel(void *vctx, int modelIndex)
{
    (void)vctx;
    (void)modelIndex;
}

static void Obj_BeginSurface(void *vctx, int modelIndex, int surfIndex, int matIndex, int numVerts, int numFaces)
{
    objWriterCtx_t *ctx = (objWriterCtx_t *)vctx;

    (void)numVerts;
    (void)numFaces;

    fprintf(ctx->f, "g mat%dm%ds%d\n", matIndex, modelIndex, surfIndex);
    fprintf(ctx->f, "usemtl %s\n", s_materials[matIndex].name);

    ctx->vbase = ctx->runningVerts;
    ctx->surfaceVerts = 0;
}

static void Obj_Vertex(void *vctx, const vec3_t pos, const vec2_t uv, const vec3_t normal)
{
    objWriterCtx_t *ctx = (objWriterCtx_t *)vctx;

    /* Q3 Z-up (X, Y, Z) -> OBJ Y-up (X=X, Y=Z, Z=-Y); V-down st -> V-up vt */
    fprintf(ctx->f, "v %.6f %.6f %.6f\n", pos[0], pos[2], -pos[1]);
    fprintf(ctx->f, "vt %.6f %.6f\n", uv[0], 1.0f - uv[1]);
    fprintf(ctx->f, "vn %.6f %.6f %.6f\n", normal[0], normal[2], -normal[1]);

    ctx->surfaceVerts++;
}

static void Obj_Face(void *vctx, int a, int b, int c)
{
    objWriterCtx_t *ctx = (objWriterCtx_t *)vctx;
    int base = ctx->vbase;

    fprintf(ctx->f, "f %d/%d/%d %d/%d/%d %d/%d/%d\n",
            base + a + 1, base + a + 1, base + a + 1,
            base + c + 1, base + c + 1, base + c + 1,
            base + b + 1, base + b + 1, base + b + 1);
}

static void Obj_EndSurface(void *vctx)
{
    objWriterCtx_t *ctx = (objWriterCtx_t *)vctx;

    ctx->runningVerts += ctx->surfaceVerts;
    ctx->surfaceVerts = 0;
}

static void Obj_EndModel(void *vctx)
{
    (void)vctx;
}

static void Obj_End(void *vctx)
{
    (void)vctx;
}

static const modelWriter_t objWriter = {
    Obj_Begin,
    Obj_BeginModel,
    Obj_BeginSurface,
    Obj_Vertex,
    Obj_Face,
    Obj_EndSurface,
    Obj_EndModel,
    Obj_End
};

static void ExportOBJ(const char *objName, const char *mtlName,
                      const char *base, const char *source, float scale)
{
    FILE *fObj;
    objWriterCtx_t ctx;

    WriteMTL(mtlName);

    fObj = fopen(objName, "w");
    if (!fObj)
    {
        _printf("ERROR: Could not open %s for writing\n", objName);
        return;
    }
    setvbuf(fObj, NULL, _IOFBF, MAP2MODEL_STREAM_BUF);

    ctx.f = fObj;
    ctx.runningVerts = 0;
    ctx.vbase = 0;
    ctx.surfaceVerts = 0;

    RunModelExport(&objWriter, &ctx, base, source, scale);

    fclose(fObj);
}

/*
====================
ASCII Scene (ASE) writer

One *GEOMOBJECT per stored surface, materials deduped to the used set.
The layout is built for the assimp ASE reader (the importer makebsp
itself uses for misc_model), which imposes hard ordering rules:

- Positions are raw Q3 Z-up: assimp's ASE loader applies a Z-up -> Y-up
  root transform itself, exactly mirroring makebsp's model importer.
- *MESH_NORMALS must come AFTER *MESH_FACE_LIST (the reader sizes its
  normal storage from the parsed face list) and must be interleaved
  per face: a *MESH_FACENORMAL line selects the active face and the
  *MESH_VERTEXNORMAL lines that follow fill that face's corners by
  index match. Any other layout is silently discarded or misassigned.
- Every *MESH_NUM* count must precede its list block.
- TVERTs carry the same (st.x, 1 - st.y) values as the OBJ writer: the
  importer applies one aiProcess_FlipUVs pass to every format alike,
  so both exports round-trip to identical st coordinates.
- Faces are emitted (a, c, b), the same winding order as the OBJ writer.

Surfaces that lost every face to degenerate rejection are skipped:
assimp drops empty meshes and other tools choke on them.
====================
*/

typedef struct
{
    FILE  *f;
    int    modelIndex;
    int    surfIndex;
    int    matIndex;
    int    surfaceType;
    float *verts;      /* 3 floats per vertex */
    float *uvs;        /* 2 floats per vertex */
    float *normals;    /* 3 floats per vertex */
    int   *faces;      /* 3 indices per face */
    int    numVerts, numFaces;
    int    capVerts, capFaces;
} aseWriterCtx_t;

static void AseReserve(aseWriterCtx_t *ctx, int numVerts, int numFaces)
{
    float *v, *u, *n;
    int   *fc;

    if (numVerts > ctx->capVerts)
    {
        v = realloc(ctx->verts, numVerts * 3 * sizeof(float));
        u = realloc(ctx->uvs, numVerts * 2 * sizeof(float));
        n = realloc(ctx->normals, numVerts * 3 * sizeof(float));
        if (!v || !u || !n)
            Error("Out of memory allocating ASE surface buffers");
        ctx->verts = v;
        ctx->uvs = u;
        ctx->normals = n;
        ctx->capVerts = numVerts;
    }
    if (numFaces > ctx->capFaces)
    {
        fc = realloc(ctx->faces, numFaces * 3 * sizeof(int));
        if (!fc)
            Error("Out of memory allocating ASE surface buffers");
        ctx->faces = fc;
        ctx->capFaces = numFaces;
    }
}

static void Ase_Begin(void *vctx, const char *base, const char *source)
{
    aseWriterCtx_t *ctx = (aseWriterCtx_t *)vctx;
    FILE *f = ctx->f;
    int i;

    fprintf(f, "*3DSMAX_ASCIIEXPORT\t200\n");
    fprintf(f, "*COMMENT\t\"Generated by makebsp -map2ase\"\n");
    fprintf(f, "*COMMENT\t\"Source: %s\"\n", source);
    fprintf(f, "*SCENE\t{\n");
    fprintf(f, "\t*SCENE_FILENAME\t\"%s.map\"\n", base);
    fprintf(f, "\t*SCENE_FIRSTFRAME\t0\n");
    fprintf(f, "\t*SCENE_LASTFRAME\t100\n");
    fprintf(f, "\t*SCENE_FRAMESPEED\t30\n");
    fprintf(f, "\t*SCENE_TICKSPERFRAME\t160\n");
    fprintf(f, "\t*SCENE_BACKGROUND_STATIC\t0.000000\t0.000000\t0.000000\n");
    fprintf(f, "\t*SCENE_AMBIENT_STATIC\t0.000000\t0.000000\t0.000000\n");
    fprintf(f, "}\n");

    fprintf(f, "*MATERIAL_LIST\t{\n");
    fprintf(f, "\t*MATERIAL_COUNT\t%d\n", s_numMaterials);
    for (i = 0; i < s_numMaterials; i++)
    {
        char noext[MAX_QPATH];

        StripImageExtension(s_materials[i].raw, noext, sizeof(noext));
        fprintf(f, "\t*MATERIAL\t%d\t{\n", i);
        fprintf(f, "\t\t*MATERIAL_NAME\t\"%s\"\n", s_materials[i].raw);
        fprintf(f, "\t\t*MATERIAL_CLASS\t\"Standard\"\n");
        fprintf(f, "\t\t*MATERIAL_DIFFUSE\t1.000000\t1.000000\t1.000000\n");
        fprintf(f, "\t\t*MATERIAL_SHADING\tPhong\n");
        fprintf(f, "\t\t*MAP_DIFFUSE\t{\n");
        fprintf(f, "\t\t\t*MAP_NAME\t\"%s\"\n", s_materials[i].raw);
        fprintf(f, "\t\t\t*MAP_CLASS\t\"Bitmap\"\n");
        fprintf(f, "\t\t\t*MAP_SUBNO\t1\n");
        fprintf(f, "\t\t\t*MAP_AMOUNT\t1.0\n");
        fprintf(f, "\t\t\t*MAP_TYPE\tScreen\n");
        fprintf(f, "\t\t\t*BITMAP\t\"%s\"\n", noext);
        fprintf(f, "\t\t\t*BITMAP_FILTER\tPyramidal\n");
        fprintf(f, "\t\t}\n");
        fprintf(f, "\t}\n");
    }
    fprintf(f, "}\n");
}

static void Ase_BeginModel(void *vctx, int modelIndex)
{
    (void)vctx;
    (void)modelIndex;
}

static void Ase_BeginSurface(void *vctx, int modelIndex, int surfIndex, int matIndex, int numVerts, int numFaces)
{
    aseWriterCtx_t *ctx = (aseWriterCtx_t *)vctx;

    ctx->modelIndex = modelIndex;
    ctx->surfIndex = surfIndex;
    ctx->matIndex = matIndex;
    ctx->surfaceType = drawSurfaces[surfIndex].surfaceType;
    AseReserve(ctx, numVerts, numFaces);
    ctx->numVerts = 0;
    ctx->numFaces = 0;
}

static void Ase_Vertex(void *vctx, const vec3_t pos, const vec2_t uv, const vec3_t normal)
{
    aseWriterCtx_t *ctx = (aseWriterCtx_t *)vctx;
    float *vp = ctx->verts + ctx->numVerts * 3;
    float *up = ctx->uvs + ctx->numVerts * 2;
    float *np = ctx->normals + ctx->numVerts * 3;

    vp[0] = pos[0];
    vp[1] = pos[1];
    vp[2] = pos[2];
    up[0] = uv[0];
    up[1] = uv[1];
    np[0] = normal[0];
    np[1] = normal[1];
    np[2] = normal[2];
    ctx->numVerts++;
}

static void Ase_Face(void *vctx, int a, int b, int c)
{
    aseWriterCtx_t *ctx = (aseWriterCtx_t *)vctx;
    int *fp = ctx->faces + ctx->numFaces * 3;

    fp[0] = a;
    fp[1] = b;
    fp[2] = c;
    ctx->numFaces++;
}

static void Ase_EndSurface(void *vctx)
{
    aseWriterCtx_t *ctx = (aseWriterCtx_t *)vctx;
    FILE *f = ctx->f;
    char name[128];
    int i;

    /* skip surfaces whose every face was rejected as degenerate */
    if (ctx->numVerts <= 0 || ctx->numFaces <= 0)
        return;

    snprintf(name, sizeof(name), "mat%dmodel%dsurf%d",
             ctx->matIndex, ctx->modelIndex, ctx->surfIndex);

    fprintf(f, "*GEOMOBJECT\t{\n");
    fprintf(f, "\t*NODE_NAME\t\"%s\"\n", name);
    fprintf(f, "\t*NODE_TM\t{\n");
    fprintf(f, "\t\t*NODE_NAME\t\"%s\"\n", name);
    fprintf(f, "\t\t*INHERIT_POS\t0\t0\t0\n");
    fprintf(f, "\t\t*INHERIT_ROT\t0\t0\t0\n");
    fprintf(f, "\t\t*INHERIT_SCL\t0\t0\t0\n");
    fprintf(f, "\t\t*TM_ROW0\t1.0\t0\t0\n");
    fprintf(f, "\t\t*TM_ROW1\t0\t1.0\t0\n");
    fprintf(f, "\t\t*TM_ROW2\t0\t0\t1.0\n");
    fprintf(f, "\t\t*TM_ROW3\t0\t0\t0\n");
    fprintf(f, "\t\t*TM_POS\t0.000000\t0.000000\t0.000000\n");
    fprintf(f, "\t}\n");

    fprintf(f, "\t*MESH\t{\n");
    fprintf(f, "\t\t*TIMEVALUE\t0\n");
    fprintf(f, "\t\t*MESH_NUMVERTEX\t%d\n", ctx->numVerts);
    fprintf(f, "\t\t*MESH_NUMFACES\t%d\n", ctx->numFaces);
    if (ctx->surfaceType == MST_PLANAR)
        fprintf(f, "\t\t*COMMENT\t\"SURFACETYPE\tMST_PLANAR\"\n");
    else if (ctx->surfaceType == MST_TRIANGLE_SOUP)
        fprintf(f, "\t\t*COMMENT\t\"SURFACETYPE\tMST_TRIANGLE_SOUP\"\n");

    fprintf(f, "\t\t*MESH_VERTEX_LIST\t{\n");
    for (i = 0; i < ctx->numVerts; i++)
        fprintf(f, "\t\t\t*MESH_VERTEX\t%d\t%.6f\t%.6f\t%.6f\n", i,
                ctx->verts[i * 3 + 0], ctx->verts[i * 3 + 1], ctx->verts[i * 3 + 2]);
    fprintf(f, "\t\t}\n");

    fprintf(f, "\t\t*MESH_FACE_LIST\t{\n");
    for (i = 0; i < ctx->numFaces; i++)
    {
        int a = ctx->faces[i * 3 + 0];
        int b = ctx->faces[i * 3 + 1];
        int c = ctx->faces[i * 3 + 2];

        fprintf(f, "\t\t\t*MESH_FACE\t%d\tA:\t%d\tB:\t%d\tC:\t%d\tAB:\t1\tBC:\t1\tCA:\t1\t*MESH_SMOOTHING\t0\t*MESH_MTLID\t0\n",
                i, a, c, b);
    }
    fprintf(f, "\t\t}\n");

    fprintf(f, "\t\t*MESH_NUMTVERTEX\t%d\n", ctx->numVerts);
    fprintf(f, "\t\t*MESH_TVERTLIST\t{\n");
    for (i = 0; i < ctx->numVerts; i++)
        fprintf(f, "\t\t\t*MESH_TVERT\t%d\t%.6f\t%.6f\t1.000000\n", i,
                ctx->uvs[i * 2 + 0], 1.0f - ctx->uvs[i * 2 + 1]);
    fprintf(f, "\t\t}\n");

    fprintf(f, "\t\t*MESH_NUMTVFACES\t%d\n", ctx->numFaces);
    fprintf(f, "\t\t*MESH_TFACELIST\t{\n");
    for (i = 0; i < ctx->numFaces; i++)
    {
        int a = ctx->faces[i * 3 + 0];
        int b = ctx->faces[i * 3 + 1];
        int c = ctx->faces[i * 3 + 2];

        fprintf(f, "\t\t\t*MESH_TFACE\t%d\t%d\t%d\t%d\n", i, a, c, b);
    }
    fprintf(f, "\t\t}\n");

    /* must follow MESH_FACE_LIST and stay interleaved per face */
    fprintf(f, "\t\t*MESH_NORMALS\t{\n");
    for (i = 0; i < ctx->numFaces; i++)
    {
        int a = ctx->faces[i * 3 + 0];
        int b = ctx->faces[i * 3 + 1];
        int c = ctx->faces[i * 3 + 2];
        vec3_t fn;

        fn[0] = ctx->normals[a * 3 + 0] + ctx->normals[b * 3 + 0] + ctx->normals[c * 3 + 0];
        fn[1] = ctx->normals[a * 3 + 1] + ctx->normals[b * 3 + 1] + ctx->normals[c * 3 + 1];
        fn[2] = ctx->normals[a * 3 + 2] + ctx->normals[b * 3 + 2] + ctx->normals[c * 3 + 2];
        if (VectorNormalize(fn, fn) == 0.0f)
            VectorClear(fn);

        fprintf(f, "\t\t\t*MESH_FACENORMAL\t%d\t%.6f\t%.6f\t%.6f\n", i, fn[0], fn[1], fn[2]);
        fprintf(f, "\t\t\t*MESH_VERTEXNORMAL\t%d\t%.6f\t%.6f\t%.6f\n", a,
                ctx->normals[a * 3 + 0], ctx->normals[a * 3 + 1], ctx->normals[a * 3 + 2]);
        fprintf(f, "\t\t\t*MESH_VERTEXNORMAL\t%d\t%.6f\t%.6f\t%.6f\n", c,
                ctx->normals[c * 3 + 0], ctx->normals[c * 3 + 1], ctx->normals[c * 3 + 2]);
        fprintf(f, "\t\t\t*MESH_VERTEXNORMAL\t%d\t%.6f\t%.6f\t%.6f\n", b,
                ctx->normals[b * 3 + 0], ctx->normals[b * 3 + 1], ctx->normals[b * 3 + 2]);
    }
    fprintf(f, "\t\t}\n");

    fprintf(f, "\t}\n");
    fprintf(f, "\t*PROP_MOTIONBLUR\t0\n");
    fprintf(f, "\t*PROP_CASTSHADOW\t1\n");
    fprintf(f, "\t*PROP_RECVSHADOW\t1\n");
    fprintf(f, "\t*MATERIAL_REF\t%d\n", ctx->matIndex);
    fprintf(f, "}\n");
}

static void Ase_EndModel(void *vctx)
{
    (void)vctx;
}

static void Ase_End(void *vctx)
{
    (void)vctx;
}

static const modelWriter_t aseWriter = {
    Ase_Begin,
    Ase_BeginModel,
    Ase_BeginSurface,
    Ase_Vertex,
    Ase_Face,
    Ase_EndSurface,
    Ase_EndModel,
    Ase_End
};

static void ExportASE(const char *aseName, const char *base,
                      const char *source, float scale)
{
    FILE *fAse;
    aseWriterCtx_t ctx;

    fAse = fopen(aseName, "w");
    if (!fAse)
    {
        _printf("ERROR: Could not open %s for writing\n", aseName);
        return;
    }
    setvbuf(fAse, NULL, _IOFBF, MAP2MODEL_STREAM_BUF);

    ctx.f = fAse;
    ctx.verts = NULL;
    ctx.uvs = NULL;
    ctx.normals = NULL;
    ctx.faces = NULL;
    ctx.numVerts = ctx.numFaces = 0;
    ctx.capVerts = ctx.capFaces = 0;

    RunModelExport(&aseWriter, &ctx, base, source, scale);

    free(ctx.verts);
    free(ctx.uvs);
    free(ctx.normals);
    free(ctx.faces);
    fclose(fAse);
}

/*
====================
Export session: allocate, classify, summarize, free. Format-agnostic.
====================
*/
static qboolean BeginModelExport(void)
{
    c_models = c_modelskip = 0;
    c_planar = c_soup = c_patch = 0;
    c_nodraw = c_other = c_invalid = 0;
    c_verts = c_facesStored = c_degen = 0;

    s_class = malloc(numDrawSurfaces * sizeof(int));
    s_mat = malloc(numDrawSurfaces * sizeof(int));
    s_materials = malloc((numShaders > 0 ? numShaders : 1) * sizeof(modelMaterial_t));
    s_numMaterials = 0;

    if (!s_class || !s_mat || !s_materials)
    {
        _printf("ERROR: Out of memory\n");
        free(s_class);
        free(s_mat);
        free(s_materials);
        s_class = s_mat = NULL;
        s_materials = NULL;
        return qfalse;
    }

    ClassifyAll();
    return qtrue;
}

static void EndModelExport(void)
{
    free(s_class);
    free(s_mat);
    free(s_materials);
    s_class = s_mat = NULL;
    s_materials = NULL;
}

static void PrintModelExportSummary(void)
{
    _printf("%d models exported (%d empty skipped)\n", c_models, c_modelskip);
    _printf("%d surfaces: %d planar, %d trisoup\n",
            c_planar + c_soup, c_planar, c_soup);
    _printf("skipped: %d patches, %d nodraw, %d flares/other, %d invalid\n",
            c_patch, c_nodraw, c_other, c_invalid);
    _printf("%d verts, %d faces (%d degenerate skipped)\n",
            c_verts, c_facesStored, c_degen);
}

void Bsp2ObjFromCompiledState(const char *source)
{
    char base[MAX_QPATH];
    char objName[1024];
    char mtlName[1024];

    ExtractFileBase(source, base);
    snprintf(objName, sizeof(objName), "%s.obj", source);
    snprintf(mtlName, sizeof(mtlName), "%s.mtl", source);

    _printf("--- map2obj: %s -> %s ---\n", source, objName);

    if (numDrawSurfaces <= 0)
    {
        _printf("No draw surfaces found.\n");
        return;
    }

    if (!BeginModelExport())
        return;

    ExportOBJ(objName, mtlName, base, source, map2ModelScale);
    PrintModelExportSummary();
    EndModelExport();

    if (!saveprt)
    {
        char prtName[1024];
        snprintf(prtName, sizeof(prtName), "%s.prt", source);
        remove(prtName);
    }
}

void Bsp2AseFromCompiledState(const char *source)
{
    char base[MAX_QPATH];
    char aseName[1024];

    ExtractFileBase(source, base);
    snprintf(aseName, sizeof(aseName), "%s.ase", source);

    _printf("--- map2ase: %s -> %s ---\n", source, aseName);

    if (numDrawSurfaces <= 0)
    {
        _printf("No draw surfaces found.\n");
        return;
    }

    if (!BeginModelExport())
        return;

    ExportASE(aseName, base, source, map2ModelScale);
    PrintModelExportSummary();
    EndModelExport();

    if (!saveprt)
    {
        char prtName[1024];
        snprintf(prtName, sizeof(prtName), "%s.prt", source);
        remove(prtName);
    }
}
