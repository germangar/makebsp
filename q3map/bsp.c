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
qboolean nodetail;
qboolean fulldetail;
qboolean nowater;
qboolean onlyents;
qboolean leaktest;
qboolean verboseentities;
qboolean noCurveBrushes;
qboolean fakemap;
qboolean notjunc;
qboolean nomerge;
qboolean nofog;
qboolean chamfersubdivide;
qboolean mergetrisoups = qtrue;
qboolean nosubdivide;
qboolean testExpand;
qboolean showseams;
qboolean novis;
extern qboolean saveprt;
qboolean g_fast = qfalse;

qboolean guessUVs = qfalse;

char outbase[32];

// Visibility bridge prototypes
void LoadPortals(char *name);
void CalculateVisibility(qboolean mergeportals);
void Broadcast_Setup(const char *dest);
void Broadcast_Shutdown(void);

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
    LoadTriangleModels(&entities[0]);

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

        if (!novis)
        {
            char portalfile[1024];
            sprintf(portalfile, "%s.prt", source);
            _printf("--- Inline VIS ---\n");

            // 0. Explicitly set healthy defaults for VIS engine switches
            extern qboolean noPassageVis, passageVisOnly, mergevis, nosort;
            extern int testlevel;
            
            noPassageVis = qfalse;
            passageVisOnly = qfalse;
            mergevis = qfalse; // Default is no merging
            nosort = qfalse;
            testlevel = 2;     // Default test level

            void FreeVisibility(void);

            // 1. Load the flat graph data from the just-written .prt file
            LoadPortals(portalfile);

            // 2. Execute the core visibility math with the default merge setting
            CalculateVisibility(mergevis);

            // 3. Cleanup VIS memory
            FreeVisibility();

            // 4. Clean up the bridge file (matching standard VisMain behavior)
            if (!saveprt)
            {
                remove(portalfile);
            }
        }
    }
    FloodAreas(tree);

    // Create collision brushes for triangle models
    CreateTriangleModelCollision(&entities[0]);

    // add references to the detail brushes
    FilterDetailBrushesIntoTree(e, tree);

    // create drawsurfs for triangle models
    AddTriangleModels(tree);

    // Set default block size
    VectorSet(blockSize, 1024, 1024, 1024);
    const char *value = ValueForKey(&entities[0], "blocksize");
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

    if (chamfersubdivide)
    {
        ChopTjunctions(e);
    }

    if (game->chamferEdges)
    {
        ChamferSurfaceEdges(e);
        MergeChamferStripsIntoParents(e);
        MergeParentedTrisoups(e);
    }

    if (mergetrisoups)
    {
        MergeAdjacentTrisoups(e);
    }

    if (!notjunc && !chamfersubdivide)
    {
        FixTJunctions(e);
    }

    MakeEntityDecals(e);

    GenerateHalos(e);

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
    e = &entities[entity_num];
    e->firstDrawSurf = numMapDrawSurfs;

    LoadTriangleModels(e);

    PatchMapDrawSurfs(e);

    // Create collision brushes for the submodel's triangle models
    CreateTriangleModelCollision(e);

    BeginModel();

    // Expand the bmodel bounds to include the loaded misc_model geometry.
    // Otherwise, the engine might cull the model because the misc_model is outside
    // the bounds of the original func_plat brushes.
    {
        int i, j;
        dmodel_t *mod = &dmodels[nummodels];
        for (i = e->firstDrawSurf; i < numMapDrawSurfs; i++) {
            mapDrawSurface_t *ds = &mapDrawSurfs[i];
            if (!ds->miscModel) continue;
            for (j = 0; j < ds->numVerts; j++) {
                AddPointToBounds(ds->verts[j].xyz, mod->mins, mod->maxs);
            }
        }
    }

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

    if (chamfersubdivide)
    {
        ChopTjunctions(e);
    }

    if (game->chamferEdges)
    {
        ChamferSurfaceEdges(e);
        MergeChamferStripsIntoParents(e);
        MergeParentedTrisoups(e);
    }

    if (mergetrisoups)
    {
        MergeAdjacentTrisoups(e);
    }

    // add in any vertexes required to fix tjunctions
    if (!notjunc && !chamfersubdivide)
    {
        FixTJunctions(e);
    }

    MakeEntityDecals(e);

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

    extern int totalLightmappedShaders;
    _printf("%5i total lightmapped shaders\n", totalLightmappedShaders);
    _printf("%5i lightmaps allocated (%dx%d resolution)\n", numLightmaps, LIGHTMAP_WIDTH, LIGHTMAP_HEIGHT);
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
    ProcessDecals();
    SetModelNumbers();
    SetLightStyles();

    UnparseEntities();

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
        if (ValueForKey(&entities[i], "sun")[0])
        {
            _printf("WARNING: Sun entity already exists, skipping shader injection.\n");
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
            SetKeyValue(e, "sun", "1");

            // Store the normalized direction vector to avoid precision loss
            sprintf(buf, "%f %f %f", si->sunDirection[0], si->sunDirection[1],
                    si->sunDirection[2]);
            SetKeyValue(e, "sun_dir", buf);

            // Store the color and intensity
            sprintf(buf, "%f %f %f", si->sunLight[0], si->sunLight[1],
                    si->sunLight[2]);
            SetKeyValue(e, "color", buf);

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
into a binary .srf sidecar file for the makelight.exe tool.
================
*/
static void WriteSurfaceExtraFile(const char *path)
{
    char srfPath[1024];
    char baseDir[1024];
    char cacheDir[1024];
    char baseName[256];
    FILE *f;

    ExtractFileBase(path, baseName);
    GetMapOutputDir(path, baseDir);
    
    sprintf(cacheDir, "%scache/", baseDir);
    CreatePath(cacheDir);

    sprintf(srfPath, "%s%s.srf", cacheDir, baseName);

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
    if (val[0] && !HasArg("-samplesize", argc, argv)) {
        game->defaultSampleSize = atoi(val);
        if (game->defaultSampleSize < 1) game->defaultSampleSize = 1;
        _printf("Worldspawn override: default lightmap sample size = %dx%d units\n", game->defaultSampleSize, game->defaultSampleSize);
    }

    val = ValueForKey(ent, "enforcesamplesize");
    if (val[0] && !HasArg("-enforcesamplesize", argc, argv)) {
        game->enforceSampleSize = atoi(val) != 0;
        _printf("Worldspawn override: enforceSampleSize = %d\n", game->enforceSampleSize);
    }

    val = ValueForKey(ent, "haloshader");
    if (val[0]) {
        game->haloShader = copystring(val);
        _printf("Worldspawn override: haloShader = %s\n", game->haloShader);
    }
}

int main(int argc, char **argv)
{
    int i;
    double start, end;
    char path[1024];

    GetExecutablePath(argv[0]);

    _printf("Makebsp %s (%s) (c) 2026 Germán \"jal\" García and Id Software Inc.\n", MAKEBSP_VERSION, BUILD_INFO);
    _printf("Based on the original q3map by Id Software.\n");

    if (argc < 2)
    {
        Error("usage: q3map [options] mapfile");
    }

    // check for general program options
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-info")) {
            Bspinfo(argc - (i + 1), argv + (i + 1));
            return 0;
        }
        if (!strcmp(argv[i], "-exportmodels")) {
            ExportModels(argc - (i + 1), argv + (i + 1));
            return 0;
        }
    }

    // Initialize game profile from JSON and CLI
    game = InitGame(argc, argv);

    // Pre-scan CLI for VFS path construction
    const char *cliPakPaths[MAX_VFS_PATHS];
    int numCliPakPaths = 0;
    const char *cliUserDirs[MAX_VFS_PATHS];
    int numCliUserDirs = 0;
    const char *cliBasePaths[MAX_VFS_PATHS];
    int numCliBasePaths = 0;
    const char *modGameDirs[MAX_VFS_PATHS];
    int numModGameDirs = 0;
    const char *baseGameDir = game->gameDir;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-fs_pakpath") && i + 1 < argc)
        {
            if (numCliPakPaths < MAX_VFS_PATHS) cliPakPaths[numCliPakPaths++] = argv[i + 1];
            i++;
        }
        else if ((!strcmp(argv[i], "-userdir") || !strcmp(argv[i], "-fs_homepath")) && i + 1 < argc)
        {
            if (numCliUserDirs < MAX_VFS_PATHS) cliUserDirs[numCliUserDirs++] = argv[i + 1];
            i++;
        }
        else if ((!strcmp(argv[i], "-basepath") || !strcmp(argv[i], "-rootdir") || !strcmp(argv[i], "-fs_basepath")) && i + 1 < argc)
        {
            if (numCliBasePaths < MAX_VFS_PATHS) cliBasePaths[numCliBasePaths++] = argv[i + 1];
            i++;
        }
        else if ((!strcmp(argv[i], "-gamedir") || !strcmp(argv[i], "-fs_game")) && i + 1 < argc)
        {
            if (numModGameDirs < MAX_VFS_PATHS) {
                modGameDirs[numModGameDirs++] = argv[i + 1];
                AddActiveGamedir(argv[i + 1]);
            }
            i++;
        }
        else if (!strcmp(argv[i], "-connect") && i + 1 < argc)
        {
            Broadcast_Setup(argv[i + 1]);
            i++;
        }
    }

    // Default fallbacks if no CLI arguments provided
    if (numCliUserDirs == 0 && game->userDir && game->userDir[0])
        cliUserDirs[numCliUserDirs++] = game->userDir;
        
    if (numCliBasePaths == 0)
        cliBasePaths[numCliBasePaths++] = (game->rootDir && game->rootDir[0]) ? game->rootDir : ".";

    // 1. Pak Paths (Highest priority for searching and preferred write destination)
    for (i = 0; i < numCliPakPaths; i++)
    {
        AddVFSPath(cliPakPaths[i], "");
    }

    // 2. Mod GameDirs Layer
    // Mod directories take precedence over the base game directory, across both user and base paths
    for (int j = 0; j < numModGameDirs; j++)
    {
        for (i = 0; i < numCliUserDirs; i++)
            AddVFSPath(cliUserDirs[i], modGameDirs[j]);
        for (i = 0; i < numCliBasePaths; i++)
            AddVFSPath(cliBasePaths[i], modGameDirs[j]);
    }

    // 3. Base GameDir Layer (Deepest fallback)
    // Only add userdir/baseGameDir if we are not working on a mod.
    if (numModGameDirs == 0)
    {
        for (i = 0; i < numCliUserDirs; i++)
            AddVFSPath(cliUserDirs[i], baseGameDir);
    }
    
    // Always add the rootdir/baseGameDir as the final fallback for base game assets
    for (i = 0; i < numCliBasePaths; i++)
    {
        AddVFSPath(cliBasePaths[i], baseGameDir);
    }

    InitVFSWriteDir();

    // Check for tool modes after VFS is ready
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-visonly")) {
            VisMain(argc, argv);
            return 0;
        }
    }

    // do a bsp if nothing else was specified
    ClearCacheDirectory();



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
        else if (!strcmp(argv[i], "-v"))
        {
            _printf("verbose = true\n");
            verbose = qtrue;
        }
        else if (!strcmp(argv[i], "-nowater"))
        {
            _printf("nowater = true\n");
            nowater = qtrue;
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
        else if (!strcmp(argv[i], "-noautocaulk"))
        {
            noautocaulk = qtrue;
            _printf("early face auto-caulking disabled\n");
        }
        else if (!strcmp(argv[i], "-chamferedges"))
        {
            game->chamferEdges = qtrue;
            _printf("edge chamfering enabled\n");
        }
        else if (!strcmp(argv[i], "-chamfersubdivide"))
        {
            game->chamferEdges = qtrue;
            chamfersubdivide = qtrue;
            _printf("edge chamfering & T-junction subdivision enabled\n");
        }
        else if (!strcmp(argv[i], "-chamferconvexwidth"))
        {
            game->chamferConvexWidth = atof(argv[i + 1]);
            _printf("chamfer convex width = %f\n", game->chamferConvexWidth);
            i++;
        }
        else if (!strcmp(argv[i], "-chamferconcavewidth"))
        {
            game->chamferConcaveWidth = atof(argv[i + 1]);
            _printf("chamfer concave width = %f\n", game->chamferConcaveWidth);
            i++;
        }
        else if (!strcmp(argv[i], "-mergetrisoups"))
        {
            mergetrisoups = atoi(argv[i + 1]) != 0;
            _printf("adjacent trisoup merging = %d\n", mergetrisoups);
            i++;
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
        else if (!strcmp(argv[i], "-basepath") || !strcmp(argv[i], "-rootdir") ||
                 !strcmp(argv[i], "-fs_basepath"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("%s requires a directory path", argv[i]);
            i++; // Handled in pre-scan
        }
        else if (!strcmp(argv[i], "-userdir") || !strcmp(argv[i], "-fs_homepath") || !strcmp(argv[i], "-fs_pakpath"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("%s requires a directory path", argv[i]);
            i++; // Handled in pre-scan
        }
        else if (!strcmp(argv[i], "-gamedir") || !strcmp(argv[i], "-fs_game"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("%s requires a directory path", argv[i]);
            i++; // Handled in pre-scan
        }
        else if (!strcmp(argv[i], "-connect"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("%s requires an IP address", argv[i]);
            Broadcast_Setup(argv[++i]);
        }
        else if (!strcmp(argv[i], "-game"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("-game requires a profile name");
            i++; // Handled in pre-scan
        }
        else if (!strcmp(argv[i], "-visonly") || !strcmp(argv[i], "-bsp"))
        {
            // Handled by mode switcher
        }
        else if (!strcmp(argv[i], "-novis"))
        {
            novis = qtrue;
            _printf("Inline visibility calculation disabled.\n");
        }
        else if (!strcmp(argv[i], "-saveprt"))
        {
            saveprt = qtrue;
            _printf("saveprt = true\n");
        }
        else if (!strcmp(argv[i], "-fakemap"))
        {
            fakemap = qtrue;
            _printf("will generate fakemap.map\n");
        }
        else if (!strcmp(argv[i], "-fast"))
        {
            g_fast = qtrue;
            _printf("fast compilation mode enabled\n");
        }
        else if (!strcmp(argv[i], "-samplesize"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("-samplesize requires a numeric argument");
            game->defaultSampleSize = atoi(argv[i + 1]);
            if (game->defaultSampleSize < 1)
                game->defaultSampleSize = 1;
            i++;
            _printf("lightmap sample size is %dx%d units\n", game->defaultSampleSize, game->defaultSampleSize);
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
        else if (!strcmp(argv[i], "-lightmapimagesize"))
        {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
                Error("-lightmapimagesize requires a numeric argument");
            int newSize = atoi(argv[i + 1]);
            if (newSize < 128) newSize = 128;
            if (newSize > 4096) newSize = 4096;
            int p = 128;
            while (p * 2 <= newSize) p *= 2;
            if (newSize - p > (p * 2) - newSize) p *= 2;
            newSize = p;

            if (newSize != game->lightmapSize) {
                game->lightmapSize = newSize;
                game->externalLightmaps = qtrue;
            }
            i++;
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
                "   nodetail       = ignore detail brushes\n"
                "   fulldetail     = treat all brushes as structural\n"
                "   onlyents       = only update entities in an existing BSP\n"
                "   micro <V>      = set the micro volume threshold to V\n"
                "   nofog          = don't process fog volumes\n"
                "   nosubdivide    = don't subdivide large surfaces\n"
                "   leaktest       = abort on first leak found\n"
                "   verboseentities = verbose entity processing output\n"
                "   nomerge        = don't merge brush faces\n"
                "   notjunc        = skip T-junction narrowing and fixing\n"
                "   noautocaulk    = disable early face auto-caulking\n"
                "   chamferedges   = enable edge chamfering for smooth corner lighting\n"
                "   chamfersubdivide = enable T-junction surface splitting before chamfering\n"
                "   -chamferconvexwidth  = size of the convex chamfer strip (default 1.25)\n"
                "   -chamferconcavewidth = size of concave chamfer strips (< 0 uses -chamferconvexwidth, 0 skips concave chamfers)\n"
                "   -mergetrisoups <0/1> = enable/disable global merging of adjacent triangle soups (default 1)\n"
                "   nosubdivide    = skip space subdivision\n"
                "   expand         = write out an expanded map (debugging)\n"
                "   showseams      = show seams on terrain surfaces\n"
                "   guessuvs       = figure out optimal texture resolution for trisoup before xatlas repacking\n"
                "   visonly        = run visibility calculation only (requires .prt file)\n"
                "   novis          = skip inline visibility calculation\n"
                "   tmpout         = write output files to /tmp\n"
                "   basepath <P>   = set the base filesystem path to P\n"
                "   game <G>       = set the active game profile to G\n"
                "   fakemap        = generate a fakemap.map after processing\n"
                "   fast           = fast compile (ignores q3map_maxsamplesize)\n"
                "   saveprt        = do not delete the .prt file after processing\n"
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

    ProcessDecals();

    SetModelNumbers();
    SetLightStyles();

    AutoCaulkBrushes();

    ProcessModels();

    // Store the lightmap texel resolution in worldspawn for makelight.exe
    if (num_entities > 0)
    {
        char buf[64];

        sprintf(buf, "%d", game->defaultSampleSize);
        SetKeyValue(&entities[0], "__texelsize", buf);
        sprintf(buf, "%d", game->lightmapSize);
        SetKeyValue(&entities[0], "_lightmapImageSize", buf);
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

    Broadcast_Shutdown();

    return 0;
}
