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

#ifndef __BSPFILE_H__
#define __BSPFILE_H__

#include "mathlib.h"
#include "qtypes.h"

#include "qfiles.h"
#include "surfaceflags.h"

extern int nummodels;
extern dmodel_t dmodels[MAX_MAP_MODELS];

extern int numShaders;
extern dshader_t dshaders[MAX_MAP_MODELS];

extern int entdatasize;
extern char dentdata[MAX_MAP_ENTSTRING];

extern int numleafs;
extern dleaf_t *dleafs;

extern int numplanes;
extern dplane_t *dplanes;

extern int numnodes;
extern dnode_t *dnodes;

extern int numleafsurfaces;
extern int *dleafsurfaces;

extern int numleafbrushes;
extern int *dleafbrushes;

extern int numbrushes;
extern dbrush_t dbrushes[MAX_MAP_BRUSHES];

extern int numbrushsides;
extern dbrushside_t *dbrushsides;

extern int numLightBytes;
extern byte *lightBytes;

extern int numGridPoints;
extern bspGridPoint_t *gridData;

extern int numLightArray;
extern unsigned short *lightArray;

extern int numVisBytes;
extern byte visBytes[MAX_MAP_VISIBILITY];

extern int numDrawVerts;
extern drawVert_t *drawVerts;

extern int numDrawIndexes;
extern int *drawIndexes;

extern int numDrawSurfaces;
extern dsurface_t *drawSurfaces;

extern int numFogs;
extern dfog_t dfogs[MAX_MAP_FOGS];

void LoadBSPFile(const char *filename);
void WriteBSPFile(const char *filename);
void BSP_AllocateForWrite(void);
void PrintBSPFileSizes(void);

//===============

typedef struct epair_s
{
    struct epair_s *next;
    char *key;
    char *value;
} epair_t;

typedef struct
{
    vec3_t origin;
    struct bspbrush_s *brushes;
    struct parseMesh_s *patches;
    int firstDrawSurf;
    epair_t *epairs;
} entity_t;

extern int num_entities;
extern entity_t entities[MAX_MAP_ENTITIES];

void ParseEntities(void);
void UnparseEntities(void);

void SetKeyValue(entity_t *ent, const char *key, const char *value);
void RemoveKeyValue(entity_t *ent, const char *key);
qboolean KeyMatches(const char *keyInMap, const char *keyRequested);

const char *ValueForEpair(epair_t *epairs, const char *key);
vec_t FloatForEpair(epair_t *epairs, const char *key);
void GetVectorForEpair(epair_t *epairs, const char *key, vec3_t vec);
void SetEpairValue(epair_t **epairs, const char *key, const char *value);

const char *ValueForKey(const entity_t *ent, const char *key);
// will return "" if not present

vec_t FloatForKey(const entity_t *ent, const char *key);
void GetVectorForKey(const entity_t *ent, const char *key, vec3_t vec);

epair_t *ParseEpair(void);

void PrintEntity(const entity_t *ent);

#endif
