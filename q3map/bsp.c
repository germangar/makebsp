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
#include "qbsp.h"
#include "../shared/json_parser.h"

#ifdef _WIN32
#include "../libs/pakstuff.h"
extern HWND hwndOut;
#endif

char tempsource[1024];

vec_t microvolume = 1.0;
qboolean glview;
qboolean nodetail;
qboolean fulldetail;
qboolean onlyents;
qboolean onlytextures;
qboolean nowater;
qboolean nofill;
qboolean noopt;
qboolean leaktest;
qboolean verboseentities;
qboolean noCurveBrushes;
qboolean fakemap;
qboolean notjunc;
qboolean nomerge;
qboolean nofog;
qboolean nosubdivide;
qboolean testExpand;
qboolean showseams;

qboolean guessUVs = qfalse;

char outbase[32];

int entity_num;

/*
============
ProcessWorldModel

============
*/
void ProcessWorldModel(void)
{
    entity_t *e;
    tree_t *tree;
    bspface_t *faces;
    qboolean leaked;

    BeginModel();

    e = &entities[0];
    e->firstDrawSurf = 0; // numMapDrawSurfs;

    // check for patches with adjacent edges that need to LOD together
    PatchMapDrawSurfs(e);

    // loading pass for misc_models (bakes transformations)
    LoadTriangleModels();

    // build an initial bsp tree using all of the sides
    // of all of the structural brushes
    faces = MakeStructuralBspFaceList(entities[0].brushes);
    tree = FaceBSP(faces);
    MakeTreePortals(tree);
    FilterStructuralBrushesIntoTree(e, tree);

    // see if the bsp is completely enclosed
    if (FloodEntities(tree))
    {
        // rebuild a better bsp tree using only the
        // sides that are visible from the inside
        FillOutside(tree->headnode);

        // chop the sides to the convex hull of
        // their visible fragments, giving us the smallest
        // polygons
        ClipSidesIntoTree(e, tree);

        faces = MakeVisibleBspFaceList(entities[0].brushes);
        FreeTree(tree);
        tree = FaceBSP(faces);
        MakeTreePortals(tree);
        FilterStructuralBrushesIntoTree(e, tree);
        leaked = qfalse;
    }
    else
    {
        _printf("**********************\n");
        _printf("******* leaked *******\n");
        _printf("**********************\n");
        LeakFile(tree);
        if (leaktest)
        {
            _printf("--- MAP LEAKED, ABORTING LEAKTEST ---\n");
            exit(0);
        }
        leaked = qtrue;

        // chop the sides to the convex hull of
        // their visible fragments, giving us the smallest
        // polygons
        ClipSidesIntoTree(e, tree);
    }

    // save out information for visibility processing
    NumberClusters(tree);
    if (!leaked)
    {
        WritePortalFile(tree);
    }
    if (glview)
    {
        // dump the portals for debugging
        WriteGLView(tree, source);
    }
    FloodAreas(tree);

    // Create collision brushes for triangle models
    CreateTriangleModelCollision();

    // add references to the detail brushes
    FilterDetailBrushesIntoTree(e, tree);

    // create drawsurfs for triangle models
    AddTriangleModels(tree);

    // Set default block size
    VectorSet(blockSize, 1024, 1024, 1024);
    const char *value = ValueForKey(&entities[0], "_blocksize");
    if (value && value[0])
    {
        int s = sscanf(value, "%f %f %f", &blockSize[0], &blockSize[1], &blockSize[2]);
        if (s == 1)
        {
            blockSize[1] = blockSize[2] = blockSize[0];
        }
        _printf("block size = { %1.0f %1.0f %1.0f }\n", blockSize[0], blockSize[1], blockSize[2]);
    }

    // drawsurfs that cross fog boundaries will need to
    // be split along the bound
    if (!nofog)
    {
        FogDrawSurfs(); // may fragment drawsurfs
    }

    // subdivide each drawsurf as required by shader tesselation
    if (!nosubdivide)
    {
        SubdivideDrawSurfs(e, tree);
    }

    // merge together all common shaders on the same plane and remove
    // all colinear points, so extra tjunctions won't be generated
    if (!nomerge)
    {
        MergeSides(e, tree); // !@# testing
    }

    // add in any vertexes required to fix tjunctions
    if (!notjunc)
    {
        FixTJunctions(e);
    }

    // allocate lightmaps for faces and patches
    AllocateLightmaps(e);

    // add references to the final drawsurfs in the apropriate clusters
    FilterDrawsurfsIntoTree(e, tree);

    EndModel(tree->headnode);

    FreeTree(tree);
}

/*
============
ProcessSubModel

============
*/
void ProcessSubModel(void)
{
    entity_t *e;
    tree_t *tree;
    bspbrush_t *b, *bc;
    node_t *node;

    BeginModel();

    e = &entities[entity_num];
    e->firstDrawSurf = numMapDrawSurfs;

    PatchMapDrawSurfs(e);

    // just put all the brushes in an empty leaf
    // FIXME: patches?
    node = AllocNode();
    node->planenum = PLANENUM_LEAF;
    for (b = e->brushes; b; b = b->next)
    {
        bc = CopyBrush(b);
        bc->next = node->brushlist;
        node->brushlist = bc;
    }

    tree = AllocTree();
    tree->headnode = node;

    ClipSidesIntoTree(e, tree);

    // subdivide each drawsurf as required by shader tesselation or fog
    if (!nosubdivide)
    {
        SubdivideDrawSurfs(e, tree);
    }

    // merge together all common shaders on the same plane and remove
    // all colinear points, so extra tjunctions won't be generated
    if (!nomerge)
    {
        MergeSides(e, tree); // !@# testing
    }

    // add in any vertexes required to fix tjunctions
    if (!notjunc)
    {
        FixTJunctions(e);
    }

    // allocate lightmaps for faces and patches
    AllocateLightmaps(e);

    // add references to the final drawsurfs in the apropriate clusters
    FilterDrawsurfsIntoTree(e, tree);

    EndModel(node);

    FreeTree(tree);
}

/*
============
ProcessModels
============
*/
void ProcessModels(void)
{
    qboolean oldVerbose;
    entity_t *entity;

    oldVerbose = verbose;

    BeginBSPFile();

    for (entity_num = 0; entity_num < num_entities; entity_num++)
    {
        entity = &entities[entity_num];

        if (!entity->brushes && !entity->patches)
        {
            continue;
        }

        qprintf("############### model %i ###############\n", nummodels);
        if (entity_num == 0)
            ProcessWorldModel();
        else
            ProcessSubModel();

        if (!verboseentities)
            verbose = qfalse; // don't bother printing submodels
    }

    verbose = oldVerbose;
}

/*
============
Bspinfo
============
*/
void Bspinfo(int count, char **fileNames)
{
    int i;
    char source[1024];
    int size;
    FILE *f;

    if (count < 1)
    {
        _printf("No files to dump info for.\n");
        return;
    }

    for (i = 0; i < count; i++)
    {
        _printf("---------------------\n");
        strcpy(source, fileNames[i]);
        DefaultExtension(source, ".bsp");
        f = fopen(source, "rb");
        if (f)
        {
            size = Q_filelength(f);
            fclose(f);
        }
        else
            size = 0;
        _printf("%s: %i\n", source, size);

        LoadBSPFile(source);
        PrintBSPFileSizes();
        _printf("---------------------\n");
    }
}

/*
============
OnlyEnts
============
*/
void OnlyEnts(void)
{
    char out[1024];

    sprintf(out, "%s.bsp", source);
    LoadBSPFile(out);
    num_entities = 0;

    LoadMapFile(name);
    SetModelNumbers();
    SetLightStyles();

    UnparseEntities();

    WriteBSPFile(out);
}

/*
============
OnlyTextures
============
*/
void OnlyTextures(void)
{ // FIXME!!!
    char out[1024];
    int i;

    Error("-onlytextures isn't working now...");

    sprintf(out, "%s.bsp", source);

    LoadMapFile(name);

    LoadBSPFile(out);

    // replace all the drawsurface shader names
    for (i = 0; i < numDrawSurfaces; i++)
    {
    }

    WriteBSPFile(out);
}

/*
================
InjectSunEntity

Finds the first sky shader with sun info and creates a light entity if needed
================
*/
void InjectSunEntity(void)
{
    int i;
    shaderInfo_t *si;
    entity_t *e;
    char buf[1024];

    // Check if a sun entity already exists
    for (i = 0; i < num_entities; i++)
    {
        if (ValueForKey(&entities[i], "_sun")[0])
        {
            _printf("Sun entity already exists, skipping shader injection.\n");
            return;
        }
    }

    // Scan unique shaders emitted into the BSP for any sky sun definitions
    for (i = 0; i < numShaders; i++)
    {
        // Only look at shaders that have the sky surface flag
        if (!(dshaders[i].surfaceFlags & SURF_SKY))
        {
            continue;
        }

        si = ShaderInfoForShader(dshaders[i].shader);
        if (!si)
        {
            continue;
        }

        // Check if it has any sun intensity defined
        if (si->sunLight[0] || si->sunLight[1] || si->sunLight[2])
        {
            if (num_entities >= MAX_MAP_ENTITIES)
            {
                _printf("WARNING: Could not inject sun entity, MAX_MAP_ENTITIES "
                        "reached.\n");
                return;
            }

            _printf("--- InjectSunEntity ---\n");
            _printf("Injecting sun entity from shader: %s\n", si->shader);

            e = &entities[num_entities];
            num_entities++;
            memset(e, 0, sizeof(*e));

            SetKeyValue(e, "classname", "light");
            SetKeyValue(e, "_sun", "1");

            // Store the normalized direction vector to avoid precision loss
            sprintf(buf, "%f %f %f", si->sunDirection[0], si->sunDirection[1],
                    si->sunDirection[2]);
            SetKeyValue(e, "_sun_dir", buf);

            // Store the color and intensity
            sprintf(buf, "%f %f %f", si->sunLight[0], si->sunLight[1],
                    si->sunLight[2]);
            SetKeyValue(e, "_color", buf);

            // We only ever support ONE global sun direction in our engine
            break;
        }
    }
}

/*
============
main
============
*/
/*
============
ExportGameToJson
============
*/
int VisMain(int argc, char **argv);

/*
================
WriteSurfaceExtraFile

Writes per-surface metadata (radiosity fill mode, smoothing radius)
into a binary .srf sidecar file for the light.exe tool.
================
*/
static void WriteSurfaceExtraFile(const char *path)
{
    char srfPath[1024];
    char baseName[256];
    FILE *f;

    ExtractFileBase(path, baseName);
    sprintf(srfPath, "cache/%s.srf", baseName);
    Q_mkdir("cache");

    f = fopen(srfPath, "wb");
    if (!f)
    {
        _printf("WARNING: Could not write surface extra file %s\n", srfPath);
        return;
    }

    fwrite(&numDrawSurfaces, sizeof(int), 1, f);
    fwrite(drawExtraSurfaces, sizeof(extraSurface_t), numDrawSurfaces, f);
    fclose(f);
}

static qboolean HasArg(const char *arg, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], arg)) return qtrue;
    }
    return qfalse;
}

static void ParseWorldspawnKeys(int argc, char **argv)
{
    if (num_entities <= 0) return;
    entity_t *ent = &entities[0];
    const char *val;

    val = ValueForKey(ent, "samplesize");
    if (!val[0]) val = ValueForKey(ent, "_samplesize");
    if (!val[0]) val = ValueForKey(ent, "_lightmapsamplesize");
    if (val[0] && !HasArg("-samplesize", argc, argv)) {
        samplesize = atoi(val);
        if (samplesize < 1) samplesize = 1;
        _printf("Worldspawn override: default lightmap sample size = %dx%d units\n", samplesize, samplesize);
    }
}

int main(int argc, char **argv)
{
    int i;
    double start, end;
    char path[1024];

    _printf("Q3Map v1.0s (c) 1999 Id Software Inc.\n");

    if (argc < 2)
    {
        Error("usage: q3map [options] mapfile");
    }

    // check for general program options
    if (!strcmp(argv[1], "-info"))
    {
        Bspinfo(argc - 2, argv + 2);
        return 0;
    }
    if (!strcmp(argv[1], "-exportmodels"))
    {
        ExportModels(argc - 2, argv + 2);
        return 0;
    }
    if (!strcmp(argv[1], "-vis"))
    {
        VisMain(argc - 1, argv + 1);
        return 0;
    }

    // do a bsp if nothing else was specified

    _printf("---- q3map ----\n");

    ClearCacheDirectory();

    // Initialize game profile from JSON and CLI
    game = InitGame(argc, argv);

    // Apply game defaults before parsing CLI
    samplesize = game->defaultSampleSize;



    tempsource[0] = '\0';

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-tempname"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("-tempname requires a string argument");
            strcpy(tempsource, argv[++i]);
        }
        else if (!strcmp(argv[i], "-threads"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("-threads requires a numeric argument");
            numthreads = atoi(argv[i + 1]);
            i++;
        }
        else if (!strcmp(argv[i], "-glview"))
        {
            glview = qtrue;
        }
        else if (!strcmp(argv[i], "-v"))
        {
            _printf("verbose = true\n");
            verbose = qtrue;
        }
        else if (!strcmp(argv[i], "-draw"))
        {
            _printf("drawflag = true\n");
            drawflag = qtrue;
        }
        else if (!strcmp(argv[i], "-nowater"))
        {
            _printf("nowater = true\n");
            nowater = qtrue;
        }
        else if (!strcmp(argv[i], "-noopt"))
        {
            _printf("noopt = true\n");
            noopt = qtrue;
        }
        else if (!strcmp(argv[i], "-nofill"))
        {
            _printf("nofill = true\n");
            nofill = qtrue;
        }
        else if (!strcmp(argv[i], "-nodetail"))
        {
            _printf("nodetail = true\n");
            nodetail = qtrue;
        }
        else if (!strcmp(argv[i], "-fulldetail"))
        {
            _printf("fulldetail = true\n");
            fulldetail = qtrue;
        }
        else if (!strcmp(argv[i], "-onlyents"))
        {
            _printf("onlyents = true\n");
            onlyents = qtrue;
        }
        else if (!strcmp(argv[i], "-onlytextures"))
        {
            _printf("onlytextures = true\n"); // FIXME: make work again!
            onlytextures = qtrue;
        }
        else if (!strcmp(argv[i], "-micro"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("-micro requires a numeric argument");
            microvolume = atof(argv[i + 1]);
            _printf("microvolume = %f\n", microvolume);
            i++;
        }
        else if (!strcmp(argv[i], "-nofog"))
        {
            _printf("nofog = true\n");
            nofog = qtrue;
        }
        else if (!strcmp(argv[i], "-nosubdivide"))
        {
            _printf("nosubdivide = true\n");
            nosubdivide = qtrue;
        }
        else if (!strcmp(argv[i], "-leaktest"))
        {
            _printf("leaktest = true\n");
            leaktest = qtrue;
        }
        else if (!strcmp(argv[i], "-verboseentities"))
        {
            _printf("verboseentities = true\n");
            verboseentities = qtrue;
        }
        else if (!strcmp(argv[i], "-nocurves"))
        {
            noCurveBrushes = qtrue;
            _printf("no curve brushes\n");
        }
        else if (!strcmp(argv[i], "-notjunc"))
        {
            notjunc = qtrue;
            _printf("no tjunction fixing\n");
        }
        else if (!strcmp(argv[i], "-expand"))
        {
            testExpand = qtrue;
            _printf("Writing expanded.map.\n");
        }
        else if (!strcmp(argv[i], "-showseams"))
        {
            showseams = qtrue;
            _printf("Showing seams on terrain.\n");
        }
        else if (!strcmp(argv[i], "-tmpout"))
        {
            strcpy(outbase, "/tmp");
        }
        else if (!strcmp(argv[i], "-basepath"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("-basepath requires a directory path");
            strcpy(qdir, argv[++i]);
        }
        else if (!strcmp(argv[i], "-game"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("-game requires a profile name");
            i++; // Handled in pre-scan
        }
        else if (!strcmp(argv[i], "-fakemap"))
        {
            fakemap = qtrue;
            _printf("will generate fakemap.map\n");
        }
        else if (!strcmp(argv[i], "-samplesize"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("-samplesize requires a numeric argument");
            samplesize = atoi(argv[i + 1]);
            if (samplesize < 1)
                samplesize = 1;
            i++;
            _printf("lightmap sample size is %dx%d units\n", samplesize, samplesize);
        }
        else if (!strcmp(argv[i], "-guessuvs"))
        {
            guessUVs = qtrue;
            _printf("Guessing optimal UV packing resolution\n");
        }
        else if (!strcmp(argv[i], "-enforceSampleSize"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("-enforceSampleSize requires a 0|1 argument");
            game->enforceSampleSize = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "-bsp"))
        {
            // Redundant, just to satisfy usage
        }
        else
        {
            break;
        }
    }

    if (i != argc - 1)
    {
        if (i < argc)
        {
            _printf("Error: Unrecognized switch or extra argument '%s'\n", argv[i]);
        }
        _printf("usage: q3map [-<switch> [-<switch> ...]] <mapname>\n"
                "\n"
                "Switches:\n"
                "   v              = verbose output\n"
                "   draw           = enable draw flag\n"
                "   nowater        = don't process water surfaces\n"
                "   noopt          = don't optimize the BSP tree\n"
                "   nofill         = don't fill outside volumes\n"
                "   nodetail       = ignore detail brushes\n"
                "   fulldetail     = treat all brushes as structural\n"
                "   onlyents       = only update entities in an existing BSP\n"
                "   onlytextures   = only update textures in an existing BSP\n"
                "   micro <V>      = set the micro volume threshold to V\n"
                "   nofog          = don't process fog volumes\n"
                "   nosubdivide    = don't subdivide large surfaces\n"
                "   leaktest       = abort on first leak found\n"
                "   verboseentities = verbose entity processing output\n"
                "   nocurves       = ignore curved surfaces (patches)\n"
                "   notjunc        = skip T-junction narrowing and fixing\n"
                "   expand         = write out an expanded map (debugging)\n"
                "   showseams      = show seams on terrain surfaces\n"
                "   guessuvs       = figure out optimal texture resolution for trisoup before xatlas repacking\n"
                "   tmpout         = write output files to /tmp\n"
                "   basepath <P>   = set the base filesystem path to P\n"
                "   game <G>       = set the active game profile to G\n"
                "   fakemap        = generate a fakemap.map after processing\n"
                "   samplesize <N> = set the default lightmap sample size to NxN\n"

                "   enforceSampleSize <0|1> = strictly follow shader/global sample size\n");
        exit(0);
    }

    _printf("Active game: %s (BSP format: %s)\n", game->arg, game->bspIdent);

    start = I_FloatTime();

    _printf("guessUVs: %s\n", guessUVs ? "true" : "false");

    ThreadSetDefault();
    // numthreads = 1;		// multiple threads aren't helping because of
    // heavy malloc use
    SetQdirFromPath(argv[i]);
    if (game->gamePath[0] && strcmp(game->gamePath, "."))
    {
        strcat(gamedir, game->gamePath);
        strcat(gamedir, "/");
    }

#ifdef _WIN32
    InitPakFile(gamedir, NULL);
#endif

    strcpy(source, ExpandArg(argv[i]));
    StripExtension(source);

    // delete portal and line files
    sprintf(path, "%s.prt", source);
    remove(path);
    sprintf(path, "%s.lin", source);
    remove(path);

    strcpy(name, ExpandArg(argv[i]));
    if (strcmp(name + strlen(name) - 4, ".reg"))
    {
        // if we are doing a full map, delete the last saved region map
        sprintf(path, "%s.reg", source);
        remove(path);

        DefaultExtension(name, ".map"); // might be .reg
    }

    //
    // if onlyents, just grab the entites and resave
    //
    if (onlyents)
    {
        OnlyEnts();
        return 0;
    }

    //
    // if onlytextures, just grab the textures and resave
    //
    if (onlytextures)
    {
        OnlyTextures();
        return 0;
    }

    //
    // start from scratch
    //
    LoadShaderInfo();

    // load original file from temp spot in case it was renamed by the editor on
    // the way in
    if (strlen(tempsource) > 0)
    {
        LoadMapFile(tempsource);
    }
    else
    {
        LoadMapFile(name);
    }

    ParseWorldspawnKeys(argc, argv);

    SetModelNumbers();
    SetLightStyles();

    ProcessModels();

    // Store the lightmap texel resolution in worldspawn for light.exe
    if (num_entities > 0)
    {
        char buf[16];
        sprintf(buf, "%d", samplesize);
        SetKeyValue(&entities[0], "__texelsize", buf);
        sprintf(buf, "%d", game->lightmapSize);
        SetKeyValue(&entities[0], "__lightmapImageSize", buf);
    }

    InjectSunEntity();

    EndBSPFile();

    WriteSurfaceExtraFile(source);

    end = I_FloatTime();
    _printf("%5.0f seconds elapsed\n", end - start);

    // remove temp name if appropriate
    if (strlen(tempsource) > 0)
    {
        remove(tempsource);
    }

    FreeLightmaps();

    return 0;
}
