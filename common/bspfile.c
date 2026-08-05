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

#include "cmdlib.h"
#include "mathlib.h"
#include "bspfile.h"
#include "../shared/globals.h"
#include "scriplib.h"
#include "../shared/globals.h"

void GetLeafNums(void);

typedef struct
{
    int planeNum;
    int shaderNum;
} ibspBrushSide_t;

typedef struct
{
    vec3_t xyz;
    float st[2];
    float lightmap[2];
    vec3_t normal;
    byte color[4];
} ibspDrawVert_t;

typedef struct
{
    int shaderNum;
    int fogNum;
    int surfaceType;

    int firstVert;
    int numVerts;

    int firstIndex;
    int numIndexes;

    int lightmapNum;
    int lightmapX, lightmapY;
    int lightmapWidth, lightmapHeight;

    vec3_t lightmapOrigin;
    vec3_t lightmapVecs[3];

    int patchWidth;
    int patchHeight;
} ibspSurface_t;

//=============================================================================

int nummodels;
dmodel_t dmodels[MAX_MAP_MODELS];

int numShaders;
dshader_t dshaders[MAX_MAP_SHADERS];

int entdatasize;
char dentdata[MAX_MAP_ENTSTRING];

int numleafs;
dleaf_t *dleafs = NULL;

int numplanes;
dplane_t *dplanes = NULL;

int numnodes;
dnode_t *dnodes = NULL;

int numleafsurfaces;
int *dleafsurfaces = NULL;

int numleafbrushes;
int *dleafbrushes = NULL;

int numbrushes;
dbrush_t dbrushes[MAX_MAP_BRUSHES];

int numbrushsides;
dbrushside_t *dbrushsides = NULL;

int numLightBytes;
byte *lightBytes = NULL;

int numGridPoints;
bspGridPoint_t *gridData = NULL;

int numLightArray;
unsigned short *lightArray = NULL;

int numVisBytes;
byte visBytes[MAX_MAP_VISIBILITY];

int numDrawVerts;
drawVert_t *drawVerts = NULL;

int numDrawIndexes;
int *drawIndexes = NULL;

int numDrawSurfaces;
dsurface_t *drawSurfaces = NULL;

int numFogs;
dfog_t dfogs[MAX_MAP_FOGS];

//=============================================================================

/*
=============
SwapBlock

If all values are 32 bits, this can be used to swap everything
=============
*/
void SwapBlock(int *block, int sizeOfBlock)
{
    int i;

    sizeOfBlock >>= 2;
    for (i = 0; i < sizeOfBlock; i++)
    {
        block[i] = LittleLong(block[i]);
    }
}

int CopyLump(dheader_t *header, int lump, void *dest, int size)
{
    int length, ofs;

    length = header->lumps[lump].filelen;
    ofs = header->lumps[lump].fileofs;

    if (length % size)
    {
        Error("LoadBSPFile: odd lump size");
    }

    if (dest)
    {
        memcpy(dest, (byte *)header + ofs, length);
    }

    return length / size;
}

/*
=============
LoadBSPFile
=============
*/
void LoadBSPFile(const char *filename)
{
    dheader_t *header;
    int i, j, k;
    int ident, version;

    // load the file header
    LoadFile(filename, (void **)&header);

    ident = LittleLong(header->ident);
    version = LittleLong(header->version);

    // Check if the current game profile already matches this BSP format.
    // This allows the -game switch to take precedence over automatic detection.
    if (game)
    {
        int activeIdent = *(int *)game->bspIdent;
        if (ident != activeIdent || version != (int)game->bspVersion)
        {
            game = NULL;
        }
    }

    if (!game)
    {
        for (i = numGames - 1; i >= 0; i--)
        {
            int gameIdent = *(int *)gameTemplates[i].bspIdent;
            if (ident == gameIdent && version == (int)gameTemplates[i].bspVersion)
            {
                game = &gameTemplates[i];
                break;
            }
        }
    }

    if (!game)
    {
        Error("%s is an unknown BSP format (ident: %c%c%c%c, version: %d)", filename,
              ident & 0xFF, (ident >> 8) & 0xFF, (ident >> 16) & 0xFF,
              (ident >> 24) & 0xFF, version);
    }

    // swap the header
    for (i = 0; i < game->lumpCount; i++)
    {
        header->lumps[i].fileofs = LittleLong(header->lumps[i].fileofs);
        header->lumps[i].filelen = LittleLong(header->lumps[i].filelen);
    }

#define ALLOC_AND_COPY_LUMP(header, lump, dest, type, num) \
    do { \
        num = CopyLump(header, lump, NULL, sizeof(type)); \
        if (num > 0) { \
            dest = malloc(num * sizeof(type)); \
            CopyLump(header, lump, dest, sizeof(type)); \
        } else { \
            dest = NULL; \
        } \
    } while(0)

    numShaders = CopyLump(header, LUMP_SHADERS, dshaders, sizeof(dshader_t));
    nummodels = CopyLump(header, LUMP_MODELS, dmodels, sizeof(dmodel_t));
    ALLOC_AND_COPY_LUMP(header, LUMP_PLANES, dplanes, dplane_t, numplanes);
    ALLOC_AND_COPY_LUMP(header, LUMP_LEAFS, dleafs, dleaf_t, numleafs);
    ALLOC_AND_COPY_LUMP(header, LUMP_NODES, dnodes, dnode_t, numnodes);
    ALLOC_AND_COPY_LUMP(header, LUMP_LEAFSURFACES, dleafsurfaces, int, numleafsurfaces);
    ALLOC_AND_COPY_LUMP(header, LUMP_LEAFBRUSHES, dleafbrushes, int, numleafbrushes);
    numbrushes = CopyLump(header, LUMP_BRUSHES, dbrushes, sizeof(dbrush_t));

    if (ident == FBSP_IDENT)
    {
        ALLOC_AND_COPY_LUMP(header, LUMP_BRUSHSIDES, dbrushsides, dbrushside_t, numbrushsides);
        ALLOC_AND_COPY_LUMP(header, LUMP_DRAWVERTS, drawVerts, drawVert_t, numDrawVerts);
        ALLOC_AND_COPY_LUMP(header, LUMP_SURFACES, drawSurfaces, dsurface_t, numDrawSurfaces);
        ALLOC_AND_COPY_LUMP(header, LUMP_LIGHTGRID, gridData, bspGridPoint_t, numGridPoints);
        
        numLightArray = CopyLump(header, LUMP_LIGHTARRAY, NULL, 2);
        if (numLightArray > 0) {
            lightArray = malloc(numLightArray * 2);
            CopyLump(header, LUMP_LIGHTARRAY, lightArray, 2);
        } else {
            lightArray = NULL;
        }
    }
    else
    {
        ibspBrushSide_t *isides;
        numbrushsides = CopyLump(header, LUMP_BRUSHSIDES, NULL, sizeof(ibspBrushSide_t));
        isides = malloc(numbrushsides * sizeof(ibspBrushSide_t));
        CopyLump(header, LUMP_BRUSHSIDES, isides, sizeof(ibspBrushSide_t));
        
        dbrushsides = malloc(numbrushsides * sizeof(dbrushside_t));
        for (i = 0; i < numbrushsides; i++)
        {
            dbrushsides[i].planeNum = isides[i].planeNum;
            dbrushsides[i].shaderNum = isides[i].shaderNum;
            dbrushsides[i].surfaceNum = -1;
        }
        free(isides);

        // up-convert IBSP
        ibspDrawVert_t *iv;
        int numiv = CopyLump(header, LUMP_DRAWVERTS, NULL, sizeof(ibspDrawVert_t));
        iv = malloc(numiv * sizeof(ibspDrawVert_t));
        CopyLump(header, LUMP_DRAWVERTS, iv, sizeof(ibspDrawVert_t));
        numDrawVerts = numiv;
        
        drawVerts = malloc(numDrawVerts * sizeof(drawVert_t));
        for (i = 0; i < numiv; i++)
        {
            memset(&drawVerts[i], 0, sizeof(drawVerts[i]));
            VectorCopy(iv[i].xyz, drawVerts[i].xyz);
            drawVerts[i].st[0] = iv[i].st[0];
            drawVerts[i].st[1] = iv[i].st[1];
            drawVerts[i].lightmap[0][0] = iv[i].lightmap[0];
            drawVerts[i].lightmap[0][1] = iv[i].lightmap[1];
            VectorCopy(iv[i].normal, drawVerts[i].normal);
            for (j = 0; j < 4; j++)
            {
                drawVerts[i].color[0][j] = iv[i].color[j];
            }
            // initialize auxiliary layers
            for (j = 1; j < 4; j++)
            {
                drawVerts[i].lightmap[j][0] = 0;
                drawVerts[i].lightmap[j][1] = 0;
                for (k = 0; k < 4; k++)
                {
                    drawVerts[i].color[j][k] = 255;
                }
            }
        }
        free(iv);

        ibspSurface_t *is;
        int numis = CopyLump(header, LUMP_SURFACES, NULL, sizeof(ibspSurface_t));
        is = malloc(numis * sizeof(ibspSurface_t));
        CopyLump(header, LUMP_SURFACES, is, sizeof(ibspSurface_t));
        numDrawSurfaces = numis;
        
        drawSurfaces = malloc(numDrawSurfaces * sizeof(dsurface_t));
        for (i = 0; i < numis; i++)
        {
            memset(&drawSurfaces[i], 0, sizeof(drawSurfaces[i]));
            drawSurfaces[i].shaderNum = is[i].shaderNum;
            drawSurfaces[i].fogNum = is[i].fogNum;
            drawSurfaces[i].surfaceType = is[i].surfaceType;
            drawSurfaces[i].firstVert = is[i].firstVert;
            drawSurfaces[i].numVerts = is[i].numVerts;
            drawSurfaces[i].firstIndex = is[i].firstIndex;
            drawSurfaces[i].numIndexes = is[i].numIndexes;

            drawSurfaces[i].lightmapNum[0] = is[i].lightmapNum;
            drawSurfaces[i].lightmapStyles[0] = 0; // LS_NORMAL
            drawSurfaces[i].vertexStyles[0] = 0;
            for (j = 1; j < 4; j++)
            {
                drawSurfaces[i].lightmapNum[j] = -1;
                drawSurfaces[i].lightmapStyles[j] = 0xFF; // LS_NONE
                drawSurfaces[i].vertexStyles[j] = 0xFF;
            }
            drawSurfaces[i].lightmapOffset[0][0] = is[i].lightmapX;
            drawSurfaces[i].lightmapOffset[0][1] = is[i].lightmapY;
            drawSurfaces[i].lightmapWidth = is[i].lightmapWidth;
            drawSurfaces[i].lightmapHeight = is[i].lightmapHeight;
            VectorCopy(is[i].lightmapOrigin, drawSurfaces[i].lightmapOrigin);
            for (j = 0; j < 3; j++)
                VectorCopy(is[i].lightmapVecs[j], drawSurfaces[i].lightmapVecs[j]);
            drawSurfaces[i].patchWidth = is[i].patchWidth;
            drawSurfaces[i].patchHeight = is[i].patchHeight;
        }
        free(is);

        v46GridPoint_t *ig;
        int numig = CopyLump(header, LUMP_LIGHTGRID, NULL, 8);
        ig = malloc(numig * 8);
        CopyLump(header, LUMP_LIGHTGRID, ig, 8);
        numGridPoints = numig;
        
        gridData = malloc(numGridPoints * sizeof(bspGridPoint_t));
        for (i = 0; i < numig; i++)
        {
            memset(&gridData[i], 0, sizeof(gridData[i]));
            VectorCopy(ig[i].ambient, gridData[i].ambient[0]);
            VectorCopy(ig[i].directed, gridData[i].directed[0]);
            gridData[i].latLong[0] = ig[i].latLong[0];
            gridData[i].latLong[1] = ig[i].latLong[1];
            gridData[i].styles[0] = 0;    // LS_NORMAL
            gridData[i].styles[1] = 0xFF; // LS_NONE
            gridData[i].styles[2] = 0xFF;
            gridData[i].styles[3] = 0xFF;
        }
        free(ig);
        numLightArray = 0;
    }

    numFogs = CopyLump(header, LUMP_FOGS, dfogs, sizeof(dfog_t));
    ALLOC_AND_COPY_LUMP(header, LUMP_DRAWINDEXES, drawIndexes, int, numDrawIndexes);

    numVisBytes = CopyLump(header, LUMP_VISIBILITY, visBytes, 1);
    
    numLightBytes = CopyLump(header, LUMP_LIGHTMAPS, NULL, 1);
    if (numLightBytes)
    {
        if (lightBytes) Q_Free(lightBytes);
        lightBytes = Q_Alloc(numLightBytes);
        if (!lightBytes) Error("Failed to allocate %d bytes for lightBytes", numLightBytes);
        CopyLump(header, LUMP_LIGHTMAPS, lightBytes, 1);
    }
    else
    {
        if (lightBytes) { Q_Free(lightBytes); lightBytes = NULL; }
    }

    entdatasize = CopyLump(header, LUMP_ENTITIES, dentdata, 1);

    free(header); // everything has been copied out

    // swap everything
}

//============================================================================

#define LG_EPSILON 4

qboolean GridPointEqual(bspGridPoint_t *p1, bspGridPoint_t *p2)
{
    int i, j;

    for (i = 0; i < 4; i++)
    {
        if (p1->styles[i] != p2->styles[i])
            return qfalse;
        for (j = 0; j < 3; j++)
        {
            if (abs((int)p1->ambient[i][j] - (int)p2->ambient[i][j]) > LG_EPSILON)
                return qfalse;
            if (abs((int)p1->directed[i][j] - (int)p2->directed[i][j]) > LG_EPSILON)
                return qfalse;
        }
    }

    for (i = 0; i < 2; i++)
    {
        int d = abs((int)p1->latLong[i] - (int)p2->latLong[i]);
        if (d > LG_EPSILON && d < (255 - LG_EPSILON))
            return qfalse;
    }

    return qtrue;
}

void CompressGrid(void)
{
    int i, j;
    bspGridPoint_t *palette;
    int numPalette = 0;

    if (game->bspVersion != 1)
        return;
    if (numGridPoints == 0)
        return;

    _printf("--- CompressGrid ---\n");

    numLightArray = numGridPoints; // Store original grid count
    palette = malloc(numGridPoints * sizeof(bspGridPoint_t));

    if (lightArray) free(lightArray);
    lightArray = malloc(numGridPoints * sizeof(unsigned short));
    if (!lightArray) Error("CompressGrid: malloc failed for lightArray");

    for (i = 0; i < numGridPoints; i++)
    {
        for (j = 0; j < numPalette; j++)
        {
            if (GridPointEqual(&gridData[i], &palette[j]))
            {
                break;
            }
        }
        if (j == numPalette)
        {
            j = numPalette++;
            palette[j] = gridData[i];
        }
        lightArray[i] = (unsigned short)j;
    }

    _printf("%i points compressed to %i unique points\n", numGridPoints, numPalette);

    memcpy(gridData, palette, numPalette * sizeof(bspGridPoint_t));
    numGridPoints = numPalette;

    free(palette);
}

/*
=============
AddLump
=============
*/
void AddLump(FILE *bspfile, dheader_t *header, int lumpnum, const void *data, int len)
{
    lump_t *lump;

    lump = &header->lumps[lumpnum];

    lump->fileofs = LittleLong(ftell(bspfile));
    lump->filelen = LittleLong(len);
    SafeWrite(bspfile, data, (len + 3) & ~3);
}

/*
=============
BSP_AllocateForWrite

Allocates the massive global BSP arrays to their maximum bucket size
as defined by the current game profile. This is required for makebsp
which builds the map from scratch.
=============
*/
void BSP_AllocateForWrite(void)
{
    if (!game)
        Error("BSP_AllocateForWrite: game profile not set");

    dleafs = malloc(game->maxMapLeafs * sizeof(dleaf_t));
    dplanes = malloc(game->maxMapPlanes * sizeof(dplane_t));
    dnodes = malloc(game->maxMapNodes * sizeof(dnode_t));
    dleafsurfaces = malloc(game->maxMapLeafSurfaces * sizeof(int));
    dleafbrushes = malloc(game->maxMapLeafBrushes * sizeof(int));
    dbrushsides = malloc(game->maxMapBrushSides * sizeof(dbrushside_t));
    
    // Lightgrid (uses MAX_MAP_LIGHTGRID natively, but we allocate based on standard 33MB / 2MB scale)
    // Note: gridData limit is not in game_t right now, but we'll allocate it based on MAX_MAP_LIGHTGRID.
    gridData = malloc(MAX_MAP_LIGHTGRID);
    lightArray = malloc(MAX_MAP_LIGHTGRID);

    drawVerts = malloc(game->maxMapDrawVerts * sizeof(drawVert_t));
    drawIndexes = malloc(game->maxMapDrawIndexes * sizeof(int));
    drawSurfaces = malloc(game->maxMapDrawSurfs * sizeof(dsurface_t));

    if (!dleafs || !dplanes || !dnodes || !dleafsurfaces || !dleafbrushes || !dbrushsides || 
        !gridData || !lightArray || !drawVerts || !drawIndexes || !drawSurfaces)
    {
        Error("BSP_AllocateForWrite: Failed to allocate massive global BSP arrays. Out of memory.");
    }
}

/*
=============
WriteBSPFile

Swaps the bsp file in place, so it should not be referenced again
=============
*/
void WriteBSPFile(const char *filename)
{
    dheader_t outheader, *header;
    FILE *bspfile;
    int i, j;

    _printf("--- WriteBSPFile ---\n");

    header = &outheader;
    memset(header, 0, sizeof(dheader_t));

    // identifier and version from game
    header->ident = LittleLong(*(int *)game->bspIdent);
    header->version = LittleLong(game->bspVersion);

    // swap everything in place (internal format)

    bspfile = SafeOpenWrite(filename);
    SafeWrite(bspfile, header, sizeof(dheader_t)); // overwritten later

    // UnparseEntities...
    UnparseEntities();
    AddLump(bspfile, header, LUMP_ENTITIES, dentdata, entdatasize);

    AddLump(bspfile, header, LUMP_SHADERS, dshaders, numShaders * sizeof(dshader_t));
    AddLump(bspfile, header, LUMP_PLANES, dplanes, numplanes * sizeof(dplane_t));
    AddLump(bspfile, header, LUMP_LEAFS, dleafs, numleafs * sizeof(dleaf_t));
    AddLump(bspfile, header, LUMP_NODES, dnodes, numnodes * sizeof(dnode_t));
    AddLump(bspfile, header, LUMP_BRUSHES, dbrushes, numbrushes * sizeof(dbrush_t));

    if (game->bspVersion == 1)
    {
        // FBSP: engine expects dbrushside_t (12 bytes: planeNum, shaderNum, surfaceNum)
        AddLump(bspfile, header, LUMP_BRUSHSIDES, dbrushsides, numbrushsides * sizeof(dbrushside_t));
    }
    else
    {
        // IBSP: standard 8-byte brushsides
        ibspBrushSide_t *isides = malloc(numbrushsides * sizeof(ibspBrushSide_t));
        for (int k = 0; k < numbrushsides; k++)
        {
            isides[k].planeNum = dbrushsides[k].planeNum;
            isides[k].shaderNum = dbrushsides[k].shaderNum;
        }
        AddLump(bspfile, header, LUMP_BRUSHSIDES, isides, numbrushsides * sizeof(ibspBrushSide_t));
        free(isides);
    }
    AddLump(bspfile, header, LUMP_LEAFSURFACES, dleafsurfaces, numleafsurfaces * sizeof(dleafsurfaces[0]));
    AddLump(bspfile, header, LUMP_LEAFBRUSHES, dleafbrushes, numleafbrushes * sizeof(dleafbrushes[0]));
    AddLump(bspfile, header, LUMP_MODELS, dmodels, nummodels * sizeof(dmodel_t));
    AddLump(bspfile, header, LUMP_DRAWINDEXES, drawIndexes, numDrawIndexes * sizeof(drawIndexes[0]));
    AddLump(bspfile, header, LUMP_VISIBILITY, visBytes, numVisBytes);
    AddLump(bspfile, header, LUMP_LIGHTMAPS, lightBytes, numLightBytes);
    AddLump(bspfile, header, LUMP_FOGS, dfogs, numFogs * sizeof(dfog_t));

    if (game->bspVersion == 1)
    {
        // FBSP v1
        CompressGrid();
        AddLump(bspfile, header, LUMP_DRAWVERTS, drawVerts, numDrawVerts * sizeof(drawVert_t));
        AddLump(bspfile, header, LUMP_SURFACES, drawSurfaces, numDrawSurfaces * sizeof(dsurface_t));
        AddLump(bspfile, header, LUMP_LIGHTGRID, gridData, numGridPoints * sizeof(bspGridPoint_t));
        AddLump(bspfile, header, LUMP_LIGHTARRAY, lightArray, numLightArray * 2);
    }
    else
    {
        // IBSP v46
        // We need to down-convert.

        ibspDrawVert_t *iv = malloc(numDrawVerts * sizeof(ibspDrawVert_t));
        for (i = 0; i < numDrawVerts; i++)
        {
            VectorCopy(drawVerts[i].xyz, iv[i].xyz);
            iv[i].st[0] = LittleFloat(drawVerts[i].st[0]);
            iv[i].st[1] = LittleFloat(drawVerts[i].st[1]);
            iv[i].lightmap[0] = LittleFloat(drawVerts[i].lightmap[0][0]);
            iv[i].lightmap[1] = LittleFloat(drawVerts[i].lightmap[0][1]);
            VectorCopy(drawVerts[i].normal, iv[i].normal);
            for (j = 0; j < 3; j++)
            {
                iv[i].xyz[j] = LittleFloat(iv[i].xyz[j]);
                iv[i].normal[j] = LittleFloat(iv[i].normal[j]);
                iv[i].color[j] = drawVerts[i].color[0][j];
            }
            iv[i].color[3] = drawVerts[i].color[0][3];
        }
        AddLump(bspfile, header, LUMP_DRAWVERTS, iv, numDrawVerts * sizeof(ibspDrawVert_t));
        free(iv);

        ibspSurface_t *is = malloc(numDrawSurfaces * sizeof(ibspSurface_t));
        for (i = 0; i < numDrawSurfaces; i++)
        {
            is[i].shaderNum = LittleLong(drawSurfaces[i].shaderNum);
            is[i].fogNum = LittleLong(drawSurfaces[i].fogNum);
            is[i].surfaceType = LittleLong(drawSurfaces[i].surfaceType);
            is[i].firstVert = LittleLong(drawSurfaces[i].firstVert);
            is[i].numVerts = LittleLong(drawSurfaces[i].numVerts);
            is[i].firstIndex = LittleLong(drawSurfaces[i].firstIndex);
            is[i].numIndexes = LittleLong(drawSurfaces[i].numIndexes);
            is[i].lightmapNum = LittleLong(drawSurfaces[i].lightmapNum[0]);
            is[i].lightmapX = LittleLong(drawSurfaces[i].lightmapOffset[0][0]);
            is[i].lightmapY = LittleLong(drawSurfaces[i].lightmapOffset[0][1]);
            is[i].lightmapWidth = LittleLong(drawSurfaces[i].lightmapWidth);
            is[i].lightmapHeight = LittleLong(drawSurfaces[i].lightmapHeight);
            for (j = 0; j < 3; j++)
            {
                is[i].lightmapOrigin[j] = LittleFloat(drawSurfaces[i].lightmapOrigin[j]);
                is[i].lightmapVecs[0][j] = LittleFloat(drawSurfaces[i].lightmapVecs[0][j]);
                is[i].lightmapVecs[1][j] = LittleFloat(drawSurfaces[i].lightmapVecs[1][j]);
                is[i].lightmapVecs[2][j] = LittleFloat(drawSurfaces[i].lightmapVecs[2][j]);
            }
            is[i].patchWidth = LittleLong(drawSurfaces[i].patchWidth);
            is[i].patchHeight = LittleLong(drawSurfaces[i].patchHeight);
        }
        AddLump(bspfile, header, LUMP_SURFACES, is, numDrawSurfaces * sizeof(ibspSurface_t));
        free(is);

        v46GridPoint_t *ig = malloc(numGridPoints * 8);
        for (i = 0; i < numGridPoints; i++)
        {
            VectorCopy(gridData[i].ambient[0], ig[i].ambient);
            VectorCopy(gridData[i].directed[0], ig[i].directed);
            ig[i].latLong[0] = gridData[i].latLong[0];
            ig[i].latLong[1] = gridData[i].latLong[1];
        }
        AddLump(bspfile, header, LUMP_LIGHTGRID, ig, numGridPoints * 8);
        free(ig);
    }

    fseek(bspfile, 0, SEEK_SET);
    SafeWrite(bspfile, header, sizeof(dheader_t));
    fclose(bspfile);

    _printf("BSP written to %s\n", filename);
}

//============================================================================

/*
=============
PrintBSPFileSizes

Dumps info about current file
=============
*/
void PrintBSPFileSizes(void)
{
    if (!num_entities)
    {
        ParseEntities();
    }

    printf("%6i models       %7i\n", nummodels, (int)(nummodels * sizeof(dmodel_t)));
    printf("%6i shaders      %7i\n", numShaders, (int)(numShaders * sizeof(dshader_t)));
    printf("%6i brushes      %7i\n", numbrushes, (int)(numbrushes * sizeof(dbrush_t)));
    printf("%6i brushsides   %7i\n", numbrushsides, (int)(numbrushsides * sizeof(dbrushside_t)));
    printf("%6i fogs         %7i\n", numFogs, (int)(numFogs * sizeof(dfog_t)));
    printf("%6i planes       %7i\n", numplanes, (int)(numplanes * sizeof(dplane_t)));
    printf("%6i entdata      %7i\n", num_entities, entdatasize);

    printf("\n");

    printf("%6i nodes        %7i\n", numnodes, (int)(numnodes * sizeof(dnode_t)));
    printf("%6i leafs        %7i\n", numleafs, (int)(numleafs * sizeof(dleaf_t)));
    printf("%6i leafsurfaces %7i\n", numleafsurfaces, (int)(numleafsurfaces * sizeof(dleafsurfaces[0])));
    printf("%6i leafbrushes  %7i\n", numleafbrushes, (int)(numleafbrushes * sizeof(dleafbrushes[0])));
    printf("%6i drawverts    %7i\n", numDrawVerts, (int)(numDrawVerts * sizeof(drawVerts[0])));
    printf("%6i drawindexes  %7i\n", numDrawIndexes, (int)(numDrawIndexes * sizeof(drawIndexes[0])));
    printf("%6i drawsurfaces %7i\n", numDrawSurfaces, (int)(numDrawSurfaces * sizeof(drawSurfaces[0])));

    printf("%6i lightmaps    %7i\n", numLightBytes / (LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3), numLightBytes);
    printf("       visibility   %7i\n", numVisBytes);
}

//============================================

int num_entities;
entity_t entities[MAX_MAP_ENTITIES];

void StripTrailing(char *e)
{
    char *s;

    s = e + strlen(e) - 1;
    while (s >= e && *s <= 32)
    {
        *s = 0;
        s--;
    }
}

/*
=================
ParseEpair
=================
*/
epair_t *ParseEpair(void)
{
    epair_t *e;

    e = malloc(sizeof(epair_t));
    memset(e, 0, sizeof(epair_t));

    if (strlen(token) >= MAX_KEY - 1)
    {
        Error("ParseEpar: token too long");
    }
    e->key = copystring(token);
    GetToken(qfalse);
    if (strlen(token) >= MAX_VALUE - 1)
    {
        Error("ParseEpar: token too long");
    }
    e->value = copystring(token);

    // strip trailing spaces that sometimes get accidentally
    // added in the editor
    StripTrailing(e->key);
    StripTrailing(e->value);

    return e;
}

/*
================
ParseEntity
================
*/
qboolean ParseEntity(void)
{
    epair_t *e;
    entity_t *mapent;

    if (!GetToken(qtrue))
    {
        return qfalse;
    }

    if (strcmp(token, "{"))
    {
        Error("ParseEntity: { not found");
    }
    if (num_entities == MAX_MAP_ENTITIES)
    {
        Error("num_entities == MAX_MAP_ENTITIES");
    }
    mapent = &entities[num_entities];
    num_entities++;

    do
    {
        if (!GetToken(qtrue))
        {
            Error("ParseEntity: EOF without closing brace");
        }
        if (!strcmp(token, "}"))
        {
            break;
        }
        e = ParseEpair();
        e->next = mapent->epairs;
        mapent->epairs = e;
    } while (1);

    return qtrue;
}

/*
================
ParseEntities

Parses the dentdata string into entities
================
*/
void ParseEntities(void)
{
    num_entities = 0;
    ParseFromMemory(dentdata, entdatasize, "BSP Entities", qfalse);

    while (ParseEntity())
    {
    }
}

static qboolean IsValidColorString(const char *value)
{
    int numTokens = 0;
    const char *p = value;
    qboolean inToken = qfalse;
    char firstChar = 0;

    while (*p)
    {
        if (*p > ' ')
        {
            if (!firstChar)
                firstChar = *p;
            if (!inToken)
            {
                numTokens++;
                inToken = qtrue;
            }
        }
        else
        {
            inToken = qfalse;
        }
        p++;
    }

    if (numTokens == 1 && firstChar == '#')
    {
        return qtrue;
    }
    if (numTokens == 3)
    {
        return qtrue;
    }

    return qfalse;
}

/*
================
UnparseEntities

Generates the dentdata string from all the entities
This allows the utilities to add or remove key/value pairs
to the data created by the map editor.
================
*/
void UnparseEntities(void)
{
    char *buf, *end;
    epair_t *ep;
    char line[2048];
    int i;
    char key[1024], value[1024];

    buf = dentdata;
    end = buf;
    *end = 0;

    for (i = 0; i < num_entities; i++)
    {
        ep = entities[i].epairs;
        if (!ep)
        {
            continue; // ent got removed
        }

        // Strip compiler-only entities that have no brushes/model
        const char *classname = ValueForKey(&entities[i], "classname");
        if (!strcmp(classname, "func_group") || !strcmp(classname, "func_light") ||
            !strcmp(classname, "_decal") || !strcmp(classname, "misc_decal") ||
            (!game->keepLights && !strcmp(classname, "light")))
        {
            if (!ValueForKey(&entities[i], "model")[0])
            {
                continue;
            }
        }

        strcat(end, "{\n");
        end += 2;

        for (ep = entities[i].epairs; ep; ep = ep->next)
        {
            strcpy(key, ep->key);
            StripTrailing(key);
            strcpy(value, ep->value);
            StripTrailing(value);

            if (Q_stristr(key, "color") != NULL)
            {
                if (IsValidColorString(value))
                {
                    vec3_t color;
                    ParseColor(value, color);
                    sprintf(value, "%f %f %f", color[0], color[1], color[2]);
                }
            }

            sprintf(line, "\"%s\" \"%s\"\n", key, value);
            strcat(end, line);
            end += strlen(line);
        }
        strcat(end, "}\n");
        end += 2;

        if (end > buf + MAX_MAP_ENTSTRING)
        {
            Error("Entity text too long");
        }
    }
    entdatasize = end - buf + 1;
}

void PrintEntity(const entity_t *ent)
{
    epair_t *ep;

    printf("------- entity %p -------\n", ent);
    for (ep = ent->epairs; ep; ep = ep->next)
    {
        printf("%s = %s\n", ep->key, ep->value);
    }
}

void SetEpairValue(epair_t **epairs, const char *key, const char *value)
{
    epair_t *ep;

    for (ep = *epairs; ep; ep = ep->next)
    {
        if (!strcmp(ep->key, key))
        {
            free(ep->value);
            ep->value = copystring(value);
            return;
        }
    }
    ep = malloc(sizeof(*ep));
    ep->next = *epairs;
    *epairs = ep;
    ep->key = copystring(key);
    ep->value = copystring(value);
}

void SetKeyValue(entity_t *ent, const char *key, const char *value)
{
    SetEpairValue(&ent->epairs, key, value);
}

void RemoveKeyValue(entity_t *ent, const char *key)
{
    epair_t *ep, *prev;

    prev = NULL;
    for (ep = ent->epairs; ep; prev = ep, ep = ep->next)
    {
        if (!strcmp(ep->key, key))
        {
            if (prev)
            {
                prev->next = ep->next;
            }
            else
            {
                ent->epairs = ep->next;
            }
            free(ep->key);
            free(ep->value);
            free(ep);
            return;
        }
    }
}

/*
================
KeyMatches

Unified case-insensitive key comparison with optional single '_' prefix handling.
Used to identify if keyInMap (from the map file) matches keyRequested (from the code).
================
*/
qboolean KeyMatches(const char *keyInMap, const char *keyRequested)
{
    // 1. Case-insensitive exact match
    if (!Q_stricmp(keyInMap, keyRequested))
    {
        return qtrue;
    }

    // 2. Double underscore keys are internal/special, do not perform fallback
    if (keyRequested[0] == '_' && keyRequested[1] == '_')
    {
        return qfalse;
    }

    // 3. Prefix fallback logic
    if (keyRequested[0] == '_')
    {
        // Requested "_color", match if map has "color"
        if (!Q_stricmp(keyInMap, keyRequested + 1))
        {
            return qtrue;
        }
    }
    else
    {
        // Requested "color", match if map has "_color"
        if (keyInMap[0] == '_' && keyInMap[1] != '_')
        {
            if (!Q_stricmp(keyInMap + 1, keyRequested))
            {
                return qtrue;
            }
        }
    }

    return qfalse;
}

const char *ValueForEpair(epair_t *epairs, const char *key)
{
    epair_t *ep;

    for (ep = epairs; ep; ep = ep->next)
    {
        if (KeyMatches(ep->key, key))
        {
            return ep->value;
        }
    }

    return "";
}

const char *ValueForKey(const entity_t *ent, const char *key)
{
    return ValueForEpair(ent->epairs, key);
}

vec_t FloatForEpair(epair_t *epairs, const char *key)
{
    const char *k;

    k = ValueForEpair(epairs, key);
    return atof(k);
}

vec_t FloatForKey(const entity_t *ent, const char *key)
{
    return FloatForEpair(ent->epairs, key);
}

void GetVectorForEpair(epair_t *epairs, const char *key, vec3_t vec)
{
    const char *k;
    double v1, v2, v3;

    k = ValueForEpair(epairs, key);

    // scanf into doubles, then assign, so it is vec_t size independent
    v1 = v2 = v3 = 0;
    sscanf(k, "%lf %lf %lf", &v1, &v2, &v3);
    vec[0] = v1;
    vec[1] = v2;
    vec[2] = v3;
}

void GetVectorForKey(const entity_t *ent, const char *key, vec3_t vec)
{
    GetVectorForEpair(ent->epairs, key, vec);
}
