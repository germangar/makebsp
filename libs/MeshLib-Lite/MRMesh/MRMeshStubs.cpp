#include "MRMeshIntersect.h"
#include "MRExpandShrink.h"
#include "MRMeshDecimateCallbacks.h"
#include "MRMeshRelax.h"
#include "MRIdentifyVertices.h"
#include "MROrder.h"
#include "MRDipole.h"
#include "MRPointsInBall.h"
#include "MRPointCloud.h"
#include "MRMeshComponents.h"
#include "MRAABBTreePoints.h"
#include "MRPolylineTopology.h"
#include "MRAABBTreePolyline.h"
#include "MRPolyline.h"
#include "MRMeshOrPoints.h"
#include "MRPrecisePredicates3.h"

namespace MR {

Vector3f findTriangleSegmentIntersectionPrecise( const Vector3f& fv0, const Vector3f& fv1, const Vector3f& fv2, const Vector3f& ev0, const Vector3f& ev1, CoordinateConverters )
{
    Vector3f n = cross(fv1 - fv0, fv2 - fv0).normalized();
    float d = dot(ev1 - ev0, n);
    if ( std::abs( d ) < 1e-6f ) return ev0;
    float t = dot( fv0 - ev0, n ) / d;
    return ev0 + t * ( ev1 - ev0 );
}

TriangleSegmentIntersectResult doTriangleSegmentIntersect( const std::array<PreciseVertCoords, 5> & ) { return { .doIntersect = true, .dIsLeftFromABC = false }; }

ConvertToIntVector getToIntConverter( const Box3d& ) { return [](const Vector3f& v) { return Vector3i( (int)v.x, (int)v.y, (int)v.z ); }; }
ConvertToFloatVector getToFloatConverter( const Box3d& ) { return [](const Vector3i& v) { return Vector3f( (float)v.x, (float)v.y, (float)v.z ); }; }

// STUB: MeshOrPoints
Box3f MeshOrPoints::getObjBoundingBox() const { return {}; }

// STUB: Polyline
bool PolylineTopology::isLoneEdge( EdgeId a ) const { return false; }

namespace MeshComponents {
void excludeFullySelectedComponents( const Mesh& mesh, VertBitSet& selection )
{
}
}

// STUB: PointCloud AABB
const AABBTreePoints& PointCloud::getAABBTree() const {
    static AABBTreePoints dummy( VertCoords{} );
    return dummy;
}

// STUB: equalizeTriAreas (removes Laplacian dependency)
bool equalizeTriAreas( Mesh& mesh, const MeshEqualizeTriAreasParams& params, const ProgressCallback& cb )
{
    return true;
}

bool equalizeTriAreas( const MeshTopology& topology, VertCoords& points, const MeshEqualizeTriAreasParams& params, const ProgressCallback& cb )
{
    return true;
}

// STUB: meshPreCollapseVertAttribute (preserves performance, we don't care about UV/color conservation in making brushes)
PreCollapseCallback meshPreCollapseVertAttribute( const Mesh& mesh, const MeshAttributesToUpdate& params )
{
    return {};
}



namespace MeshBuilder {
}

// STUB: calcDipoles
void calcDipoles( Vector<Dipole, NodeId>& dipoles, const AABBTree& tree, const Mesh& mesh ) {}
Vector<Dipole, NodeId> calcDipoles( const AABBTree& tree, const Mesh& mesh ) { return {}; }

// STUB: calcFastWindingNumber
float calcFastWindingNumber( const Vector<Dipole, NodeId>& dipoles, const AABBTree& tree, const Mesh& mesh, const Vector3f& p, float beta, FaceId skipFace )
{
    return 0.0f;
}

} // namespace MR
