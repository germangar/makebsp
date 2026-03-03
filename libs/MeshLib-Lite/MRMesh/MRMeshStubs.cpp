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

namespace MR {

// STUB: MeshOrPoints
Box3f MeshOrPoints::getObjBoundingBox() const { return {}; }

// STUB: Polyline
bool PolylineTopology::isLoneEdge( EdgeId a ) const { return false; }

// STUB: rayMeshIntersectAll
void rayMeshIntersectAll( const MeshPart& meshPart, const Line3d& line, MeshIntersectionCallback callback,
                          double rayStart, double rayEnd, const IntersectionPrecomputes<double>* prec) 
{
}

void rayMeshIntersectAll( const MeshPart& meshPart, const Line3f& line, MeshIntersectionCallback callback,
                          float rayStart, float rayEnd, const IntersectionPrecomputes<float>* prec) 
{
}

// STUB: expand
void expand( const MeshTopology & topology, FaceBitSet & region, int hops )
{
}

void expand( const MeshTopology & topology, VertBitSet & region, int hops )
{
}

void shrink( const MeshTopology & topology, VertBitSet & region, int hops )
{
}

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

// STUB: findPointsInBall
void findPointsInBall( const AABBTreePoints& tree, Ball3f ball, const OnPointInBallFound& foundCallback, const AffineXf3f* xf )
{
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
// STUB: VertexIdentifier 
void VertexIdentifier::reserve( size_t numTris )
{
}
void VertexIdentifier::addTriangles( const std::vector<Triangle3f> & buffer )
{
}
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
