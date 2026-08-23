#pragma once

#include "qbsp.h"

#define MAX_CSG_OPERATORS 256
#define MAX_TRIM_PLANES 128

typedef struct {
    plane_t     planes[MAX_TRIM_PLANES];
    int         numPlanes;
    vec3_t      mins, maxs; // union AABB of all brushes in this operator
    char        target[64]; // if non-empty, only cuts misc_model entities with matching targetname
} funcTrimOperator_t;

void StoreFuncTrimOperator(funcTrimOperator_t *op);
void FreeFuncTrimOperators(void);
void PerformMeshCSG(int startInst, int endInst);
void PerformMeshDecimation(int startInst, int endInst);

