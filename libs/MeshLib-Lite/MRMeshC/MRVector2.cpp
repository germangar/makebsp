#include "MRVector2.h"

#include "detail/TypeCast.h"

#include "MRMesh/MRVector2.h"

using namespace MR;

REGISTER_AUTO_CAST( Vector2f )

static_assert( sizeof( MRVector2f ) == sizeof( Vector2f ) );

MRVector2f mrVector2fDiagonal( float a )
{
    RETURN( Vector2f::diagonal( a ) );
}
