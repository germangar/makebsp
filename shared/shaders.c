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

#include "../common/cmdlib.h"
#include "../common/imagelib.h"
#include "../common/mathlib.h"
#include "../common/scriplib.h"
#include <math.h>
#include <string.h>

#include "../common/qfiles.h"
#include "../common/surfaceflags.h"

#include "shaders.h"
#ifdef _WIN32
#include "../libs/pakstuff.h"
#endif

// Default backsplash is disabled unless explicitly requested
#define DEFAULT_BACKSPLASH_FRACTION 0.0
#define DEFAULT_BACKSPLASH_DISTANCE 24

#define MAX_SURFACE_INFO 4096

shaderInfo_t defaultInfo;
shaderInfo_t shaderInfo[MAX_SURFACE_INFO];
int numShaderInfo;

typedef struct
{
    char *name;
    int clearSolid, surfaceFlags, contents;
} infoParm_t;

infoParm_t infoParms[] = {
    // server relevant contents
    {"water", 1, 0, CONTENTS_WATER},
    {"slime", 1, 0, CONTENTS_SLIME}, // mildly damaging
    {"lava", 1, 0, CONTENTS_LAVA},   // very damaging
    {"playerclip", 1, 0, CONTENTS_PLAYERCLIP},
    {"monsterclip", 1, 0, CONTENTS_MONSTERCLIP},
    {"nodrop", 1, 0, CONTENTS_NODROP}, // don't drop items or leave bodies
                                       // (death fog, lava, etc)
    {"nonsolid", 1, SURF_NONSOLID, 0}, // clears the solid flag
    {"nosolid", 1, SURF_NONSOLID, 0},  // alias for nonsolid


    // utility relevant attributes
    {"origin", 1, 0, CONTENTS_ORIGIN},     // center of rotating brushes
    {"trans", 0, 0, CONTENTS_TRANSLUCENT}, // don't eat contained surfaces
    {"detail", 0, 0, CONTENTS_DETAIL},     // don't include in structural bsp
    {"structural", 0, 0,
     CONTENTS_STRUCTURAL},                           // force into structural bsp even if trnas
    {"areaportal", 1, 0, CONTENTS_AREAPORTAL},       // divides areas
    {"clusterportal", 1, 0, CONTENTS_CLUSTERPORTAL}, // for bots
    {"donotenter", 1, 0, CONTENTS_DONOTENTER},       // for bots
    {"botclip", 1, 0, CONTENTS_BOTCLIP},             // for bots
    {"nobotclip", 0, 0, CONTENTS_NOBOTCLIP},         // don't use for bot clipping

    {"fog", 1, 0, CONTENTS_FOG},             // carves surfaces entering
    {"sky", 0, SURF_SKY, 0},                 // emit light from an environment map
    {"lightfilter", 0, SURF_LIGHTFILTER, 0}, // filter light going through it
    {"alphashadow", 0, SURF_ALPHASHADOW, 0}, // test light on a per-pixel basis
    {"hint", 0, SURF_HINT, 0},               // use as a primary splitter

    // server attributes
    {"slick", 0, SURF_SLICK, 0},
    {"noimpact", 0, SURF_NOIMPACT, 0}, // don't make impact explosions or marks
    {"nomarks", 0, SURF_NOMARKS,
     0}, // don't make impact marks, but still explode
    {"ladder", 0, SURF_LADDER, 0},
    {"nodamage", 0, SURF_NODAMAGE, 0},
    {"metalsteps", 0, SURF_METALSTEPS, 0},
    {"flesh", 0, SURF_FLESH, 0},
    {"nosteps", 0, SURF_NOSTEPS, 0},

    // drawsurf attributes
    {"nodraw", 0, SURF_NODRAW,
     0},                                   // don't generate a drawsurface (or a lightmap)
    {"pointlight", 0, SURF_POINTLIGHT, 0}, // sample lighting at vertexes
    {"nolightmap", 0, SURF_NOLIGHTMAP, 0}, // don't generate a lightmap
    {"nodlight", 0, SURF_NODLIGHT, 0},     // don't ever add dynamic lights
    {"dust", 0, SURF_DUST, 0},             // leave dust trail when walking on this surface
    {"skip", 0, SURF_SKIP, 0},             // completely ignore, allowing non-closed brushes
    {"nowalljump", 0, SURF_NOWALLJUMP, 0}  // QFusion: disable wall jumping
};

/*
===============
LoadShaderImage
===============
*/

// Returns the length of the file, and sets *bTGA to qtrue if the matched extension is .tga
int LoadImageFile(char *filename, byte **bufferptr, qboolean *bTGA)
{
    byte *buffer = NULL;
    int nLen = 0;

    // Extensions to check exactly in this order
    const char *exts[] = {".tga", ".TGA", ".jpg", ".JPG", ".png", ".PNG", ".bmp", ".BMP", NULL};

    char base[1024];
    strcpy(base, filename);
    StripExtension(base);

    for (int i = 0; exts[i] != NULL; i++)
    {
        char testPath[1024];
        sprintf(testPath, "%s%s", base, exts[i]);

        if (FileExists(testPath))
        {
            nLen = LoadFileBlock(testPath, (void **)&buffer);
        }
#ifdef _WIN32
        else
        {
            nLen = PakLoadAnyFile(testPath, (void **)&buffer);
        }
#endif

        if (buffer != NULL)
        {
            *bTGA = (Q_stricmp(exts[i], ".tga") == 0) ? qtrue : qfalse;
            *bufferptr = buffer;
            return nLen;
        }
    }

    *bufferptr = NULL;
    return 0;
}

static void LoadShaderImage(shaderInfo_t *si)
{
    char filename[1024];
    int i, count, nLen;
    float color[4];
    byte *buffer;
    qboolean bTGA = qtrue;

    // look for the lightimage if it is specified
    if (si->lightimage[0])
    {
        sprintf(filename, "%s%s", gamedir, si->lightimage);
        nLen = LoadImageFile(filename, &buffer, &bTGA);
        if (buffer != NULL)
        {
            goto loadTga;
        }
    }

    // look for the editorimage if it is specified
    if (si->editorimage[0])
    {
        sprintf(filename, "%s%s", gamedir, si->editorimage);
        nLen = LoadImageFile(filename, &buffer, &bTGA);
        if (buffer != NULL)
        {
            goto loadTga;
        }
    }

    // look for the materialImage if it is specified
    if (si->materialImage[0])
    {
        sprintf(filename, "%s%s", gamedir, si->materialImage);
        nLen = LoadImageFile(filename, &buffer, &bTGA);
        if (buffer != NULL)
        {
            goto loadTga;
        }
    }

    // try the shader name
    sprintf(filename, "%s%s", gamedir, si->shader);
    nLen = LoadImageFile(filename, &buffer, &bTGA);
    if (buffer != NULL)
    {
        goto loadTga;
    }

    // couldn't load anything
    _printf("WARNING: Missing image for material: %s\n", si->shader);

    si->color[0] = 1;
    si->color[1] = 1;
    si->color[2] = 1;
    si->width = 64;
    si->height = 64;
    si->pixels = malloc(si->width * si->height * 4);
    memset(si->pixels, 255, si->width * si->height * 4);
    return;

// load the image to get dimensions and color
loadTga:
    if (bTGA)
    {
        LoadImageFromBuffer(buffer, nLen, &si->pixels, &si->width, &si->height);
    }
    else
    {
        LoadImageFromBuffer(buffer, nLen, &si->pixels, &si->width, &si->height);
    }

    free(buffer);

    count = si->width * si->height;

    VectorClear(color);
    color[3] = 0;
    for (i = 0; i < count; i++)
    {
        color[0] += si->pixels[i * 4 + 0];
        color[1] += si->pixels[i * 4 + 1];
        color[2] += si->pixels[i * 4 + 2];
        color[3] += si->pixels[i * 4 + 3];
    }
    VectorScale(color, 1.0 / count, si->averageColor);
    if (!si->colorOverride)
    {
        for (i = 0; i < 3; i++)
        {
            si->color[i] = si->averageColor[i] / 255.0f;
        }
    }
}

/*
===============
AllocShaderInfo
===============
*/
static shaderInfo_t *AllocShaderInfo(void)
{
    shaderInfo_t *si;

    if (numShaderInfo == MAX_SURFACE_INFO)
    {
        Error("MAX_SURFACE_INFO");
    }

    si = &shaderInfo[numShaderInfo];
    numShaderInfo++;

    // set defaults
    si->contents = CONTENTS_SOLID;

    si->backsplashFraction = DEFAULT_BACKSPLASH_FRACTION;
    si->backsplashDistance = DEFAULT_BACKSPLASH_DISTANCE;
    si->surfaceLightGlow = -1.0f;

    si->lightmapSampleSize = 0;
    si->forceTraceLight = qfalse;
    si->forceVLight = qfalse;
    si->vertexShadows = qfalse;
    si->noVertexShadows = qfalse;
    si->forceSunLight = qfalse;
    si->vertexScale = 1.0;
    si->notjunc = qfalse;
    si->materialImage[0] = 0;
    si->colorOverride = qfalse;
    si->hasVertexColor = qfalse;
    VectorClear(si->vertexColor);

    return si;
}

/*
===============
ShaderInfoForShader
===============
*/
shaderInfo_t *ShaderInfoForShader(const char *shaderName)
{
    int i;
    shaderInfo_t *si;
    char shader[MAX_QPATH];

    // strip off extension
    strcpy(shader, shaderName);
    StripExtension(shader);

    // search for it
    for (i = 0; i < numShaderInfo; i++)
    {
        si = &shaderInfo[i];
        if (!Q_stricmp(shader, si->shader))
        {
            if (!si->width)
            {
                LoadShaderImage(si);
            }
            return si;
        }
    }

    si = AllocShaderInfo();
    strcpy(si->shader, shader);

    LoadShaderImage(si);

    return si;
}

/*
===============
ParseShaderFile
===============
*/
static void ParseShaderFile(const char *filename)
{
    int i;
    int numInfoParms = sizeof(infoParms) / sizeof(infoParms[0]);
    shaderInfo_t *si;

    //	qprintf( "shaderFile: %s\n", filename );
    LoadScriptFile(filename);
    while (1)
    {
        if (!GetToken(qtrue))
        {
            break;
        }

        si = AllocShaderInfo();
        strcpy(si->shader, token);
        MatchToken("{");
        while (1)
        {
            if (!GetToken(qtrue))
            {
                break;
            }
            if (!strcmp(token, "}"))
            {
                break;
            }

            // skip internal braced sections
            if (!strcmp(token, "{"))
            {
                si->hasPasses = qtrue;
                while (1)
                {
                    if (!GetToken(qtrue))
                    {
                        break;
                    }
                    if (!strcmp(token, "}"))
                    {
                        break;
                    }

                    // QFusion: scan for material keyword inside passes
                    if (!Q_stricmp(token, "material") && !si->materialImage[0])
                    {
                        GetToken(qfalse);
                        strcpy(si->materialImage, token);
                        DefaultExtension(si->materialImage, ".tga");
                    }
                }
                continue;
            }

            if (!Q_stricmp(token, "surfaceparm"))
            {
                GetToken(qfalse);
                for (i = 0; i < numInfoParms; i++)
                {
                    if (!Q_stricmp(token, infoParms[i].name))
                    {
                        si->surfaceFlags |= infoParms[i].surfaceFlags;
                        si->contents |= infoParms[i].contents;
                        if (infoParms[i].clearSolid)
                        {
                            si->contents &= ~CONTENTS_SOLID;
                        }
                        break;
                    }
                }
                if (i == numInfoParms)
                {
                    // we will silently ignore all tokens beginning with qer,
                    // which are QuakeEdRadient parameters
                    if (Q_strncasecmp(token, "qer", 3))
                    {
                        _printf("Unknown surfaceparm: \"%s\"\n", token);
                    }
                }
                continue;
            }

            // qer_editorimage <image>
            if (!Q_stricmp(token, "qer_editorimage"))
            {
                GetToken(qfalse);
                strcpy(si->editorimage, token);
                DefaultExtension(si->editorimage, ".tga");
                continue;
            }

            // q3map_lightimage <image>
            if (!Q_stricmp(token, "q3map_lightimage"))
            {
                GetToken(qfalse);
                strcpy(si->lightimage, token);
                DefaultExtension(si->lightimage, ".tga");
                continue;
            }

            // q3map_surfacelight <value>
            if (!Q_stricmp(token, "q3map_surfacelight"))
            {
                GetToken(qfalse);
                si->value = atoi(token);
                continue;
            }

            // q3map_lightRGB <red> <green> <blue>
            if (!Q_stricmp(token, "q3map_lightRGB") || !Q_stricmp(token, "q3map_lightColor"))
            {
                GetToken(qfalse);
                if (token[0] == '#')
                {
                    ParseColor(token, si->color);
                }
                else
                {
                    float c[3];
                    c[0] = atof(token);
                    GetToken(qfalse);
                    c[1] = atof(token);
                    GetToken(qfalse);
                    c[2] = atof(token);

                    if (c[0] > 1.0001f || c[1] > 1.0001f || c[2] > 1.0001f)
                    {
                        VectorScale(c, 1.0f / 255.0f, si->color);
                    }
                    else
                    {
                        VectorCopy(c, si->color);
                    }
                }
                si->colorOverride = qtrue;
                continue;
            }

            // q3map_vertexcolor <red> <green> <blue>
            if (!Q_stricmp(token, "q3map_vertexcolor"))
            {
                GetToken(qfalse);
                if (token[0] == '#')
                {
                    ParseColor(token, si->vertexColor);
                }
                else
                {
                    float c[3];
                    c[0] = atof(token);
                    GetToken(qfalse);
                    c[1] = atof(token);
                    GetToken(qfalse);
                    c[2] = atof(token);

                    if (c[0] > 1.0001f || c[1] > 1.0001f || c[2] > 1.0001f)
                    {
                        VectorScale(c, 1.0f / 255.0f, si->vertexColor);
                    }
                    else
                    {
                        VectorCopy(c, si->vertexColor);
                    }
                }
                si->hasVertexColor = qtrue;
                continue;
            }

            // q3map_surfacelight_glow <value>
            if (!Q_stricmp(token, "q3map_surfacelight_glow"))
            {
                GetToken(qfalse);
                si->surfaceLightGlow = atof(token);
                continue;
            }

            // q3map_lightsubdivide <value>
            if (!Q_stricmp(token, "q3map_lightsubdivide"))
            {
                GetToken(qfalse);
                si->lightSubdivide = atoi(token);
                continue;
            }

            // q3map_lightmapsamplesize <value>
            if (!Q_stricmp(token, "q3map_lightmapsamplesize"))
            {
                GetToken(qfalse);
                si->lightmapSampleSize = atoi(token);
                continue;
            }

            // q3map_tracelight
            if (!Q_stricmp(token, "q3map_tracelight"))
            {
                si->forceTraceLight = qtrue;
                continue;
            }

            // q3map_vlight
            if (!Q_stricmp(token, "q3map_vlight"))
            {
                si->forceVLight = qtrue;
                continue;
            }

            // q3map_vertexshadows
            if (!Q_stricmp(token, "q3map_vertexshadows"))
            {
                si->vertexShadows = qtrue;
                continue;
            }

            // q3map_novertexshadows
            if (!Q_stricmp(token, "q3map_novertexshadows"))
            {
                si->noVertexShadows = qtrue;
                continue;
            }

            // q3map_forcesunlight
            if (!Q_stricmp(token, "q3map_forcesunlight"))
            {
                si->forceSunLight = qtrue;
                continue;
            }

            // q3map_vertexscale
            if (!Q_stricmp(token, "q3map_vertexscale"))
            {
                GetToken(qfalse);
                si->vertexScale = atof(token);
                continue;
            }

            // q3map_notjunc
            if (!Q_stricmp(token, "q3map_notjunc"))
            {
                si->notjunc = qtrue;
                continue;
            }

            // q3map_globaltexture
            if (!Q_stricmp(token, "q3map_globaltexture"))
            {
                si->globalTexture = qtrue;
                continue;
            }

            // q3map_backsplash <percent> <distance>
            if (!Q_stricmp(token, "q3map_backsplash"))
            {
                GetToken(qfalse);
                si->backsplashFraction = atof(token) * 0.01;
                GetToken(qfalse);
                si->backsplashDistance = atof(token);
                continue;
            }

            // q3map_backshader <shader>
            if (!Q_stricmp(token, "q3map_backshader"))
            {
                GetToken(qfalse);
                strcpy(si->backShader, token);
                continue;
            }

            // q3map_flare <shader>
            if (!Q_stricmp(token, "q3map_flare"))
            {
                GetToken(qfalse);
                strcpy(si->flareShader, token);
                continue;
            }

            // light <value>
            // old style flare specification
            if (!Q_stricmp(token, "light"))
            {
                GetToken(qfalse);
                strcpy(si->flareShader, "flareshader");
                continue;
            }

            // q3map_sun <red> <green> <blue> <intensity> <degrees> <elivation>
            // color will be normalized, so it doesn't matter what range you use
            // intensity falls off with angle but not distance 100 is a fairly bright
            // sun degree of 0 = from the east, 90 = north, etc.  altitude of 0 =
            // sunrise/set, 90 = noon
            if (!Q_stricmp(token, "q3map_sun") || !Q_stricmp(token, "q3map_sunExt"))
            {
                float a, b;
                qboolean isExt = !Q_stricmp(token, "q3map_sunExt");

                GetToken(qfalse);
                si->sunLight[0] = atof(token);
                GetToken(qfalse);
                si->sunLight[1] = atof(token);
                GetToken(qfalse);
                si->sunLight[2] = atof(token);

                // Detect if the color was provided in 0-255 scale
                if (si->sunLight[0] > 1.0001f || si->sunLight[1] > 1.0001f || si->sunLight[2] > 1.0001f)
                {
                    VectorScale(si->sunLight, 1.0f / 255.0f, si->sunLight);
                }

                VectorNormalize(si->sunLight, si->sunLight);

                GetToken(qfalse);
                a = atof(token);
                VectorScale(si->sunLight, a, si->sunLight);

                GetToken(qfalse);
                a = atof(token);
                a = a / 180 * Q_PI;

                GetToken(qfalse);
                b = atof(token);
                b = b / 180 * Q_PI;

                si->sunDirection[0] = cos(a) * cos(b);
                si->sunDirection[1] = sin(a) * cos(b);
                si->sunDirection[2] = sin(b);

                // Consume extra parameters for sunExt without storing them
                if (isExt)
                {
                    GetToken(qfalse); // deviance
                    GetToken(qfalse); // samples
                }

                si->surfaceFlags |= SURF_SKY;
                continue;
            }

            // tesssize is used to force liquid surfaces to subdivide
            if (!Q_stricmp(token, "tesssize") || !Q_stricmp(token, "q3map_tesssize"))
            {
                GetToken(qfalse);
                si->subdivisions = atof(token);
                continue;
            }

            // cull none will set twoSided
            if (!Q_stricmp(token, "cull"))
            {
                GetToken(qfalse);
                if (!Q_stricmp(token, "none"))
                {
                    si->twoSided = qtrue;
                }
                continue;
            }

            // deformVertexes autosprite[2]
            // we catch this so autosprited surfaces become point
            // lights instead of area lights
            if (!Q_stricmp(token, "deformVertexes"))
            {
                GetToken(qfalse);
                if (!Q_strncasecmp(token, "autosprite", 10))
                {
                    si->autosprite = qtrue;
                    si->contents |= CONTENTS_DETAIL;
                }
                continue;
            }

            // ignore all other tokens on the line

            while (TokenAvailable())
            {
                GetToken(qfalse);
            }
        }
    }
}

/*
===============
LoadShaderInfo
===============
*/
#define MAX_SHADER_FILES 64
static char loadedShaderFiles[MAX_SHADER_FILES][MAX_QPATH];
static int numLoadedShaderFiles;

static void AddShaderFile(const char *filename)
{
    char base[MAX_QPATH];
    int i;

    ExtractFileBase(filename, base);

    for (i = 0; i < numLoadedShaderFiles; i++)
    {
        if (!Q_stricmp(loadedShaderFiles[i], base))
        {
            return;
        }
    }

    if (numLoadedShaderFiles == MAX_SHADER_FILES)
    {
        Error("MAX_SHADER_FILES");
    }

    strcpy(loadedShaderFiles[numLoadedShaderFiles], base);
    numLoadedShaderFiles++;

    ParseShaderFile(filename);
}

static void ShaderLooseCallback(const char *filename)
{
    char full[MAX_QPATH];
    sprintf(full, "scripts/%s", filename);
    AddShaderFile(full);
}

static void ShaderPakCallback(const char *filename)
{
    if (strstr(filename, "scripts/") && strstr(filename, ".shader"))
    {
        AddShaderFile(filename);
    }
}

void LoadShaderInfo(void)
{
    _printf("Scanning for shaders...\n");

    numLoadedShaderFiles = 0;

    // Scan loose files on disk (portable)
    Sys_ListFiles(ExpandGamePath("scripts/"), "*.shader", ShaderLooseCallback);

    // Scan files in PAKs
    ScanPakFiles(ShaderPakCallback);

    _printf("%5i shader files parsed\n", numLoadedShaderFiles);
    _printf("%5i shaders found\n", numShaderInfo);
}
