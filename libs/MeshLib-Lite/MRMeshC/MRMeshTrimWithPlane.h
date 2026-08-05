#pragma once
#include "MRMeshFwd.h"
#include "MRVector3.h"

MR_EXTERN_C_BEGIN

typedef struct MRPlane3f {
    MRVector3f n;
    float d;
} MRPlane3f;

// Trims a mesh with a plane. Leaves only the part in the positive normal direction.
MRMESHC_API void mrTrimMeshWithPlane( MRMesh* mesh, const MRPlane3f* plane );

MR_EXTERN_C_END
