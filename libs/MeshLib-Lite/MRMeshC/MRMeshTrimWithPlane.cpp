#include "MRMeshTrimWithPlane.h"
#include "MRMesh/MRMeshTrimWithPlane.h"
#include "MRMesh/MRMesh.h"
#include "detail/TypeCast.h"

using namespace MR;
REGISTER_AUTO_CAST( Mesh )

MR_EXTERN_C_BEGIN

void mrTrimMeshWithPlane( MRMesh* mesh, const MRPlane3f* plane )
{
    if ( !mesh || !plane ) return;
    MR::Plane3f p( { plane->n.x, plane->n.y, plane->n.z }, plane->d );
    MR::trimWithPlane( *auto_cast( mesh ), p );
}

MR_EXTERN_C_END
