#pragma once

#include "MRMeshFwd.h"

MR_EXTERN_C_BEGIN

/// two-dimensional vector of floats
typedef struct MRVector2f
{
    float x;
    float y;
} MRVector2f;

/// (a, a)
MRMESHC_API MRVector2f mrVector2fDiagonal( float a );

MR_EXTERN_C_END
