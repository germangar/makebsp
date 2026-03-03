#include "MRPositionVertsSmoothly.h"
#include "MRMesh.h"

namespace MR
{

void positionVertsSmoothly( Mesh& mesh, const VertBitSet& verts,
    EdgeWeights edgeWeights, VertexMass vmass, const VertBitSet * fixedSharpVertices )
{
    // STUBBED: Removed Eigen / Laplacian dependencies. 
    // MeshLib-Lite does not require smoothing for collision hulls.
}

void positionVertsSmoothly( const MeshTopology& topology, VertCoords& points, const VertBitSet& verts,
    EdgeWeights edgeWeights, VertexMass vmass, const VertBitSet * fixedSharpVertices )
{
}

void positionVertsSmoothlySharpBd( Mesh& mesh, const PositionVertsSmoothlyParams& params )
{
}

void positionVertsSmoothlySharpBd( const MeshTopology& topology, VertCoords& points, const PositionVertsSmoothlyParams& params )
{
}

void positionVertsWithSpacing( Mesh& mesh, const SpacingSettings & settings )
{
}

void positionVertsWithSpacing( const MeshTopology& topology, VertCoords& points, const SpacingSettings & settings )
{
}

void positionVertsSmoothlySharpBd( Mesh& mesh, const VertBitSet& verts )
{
}

void inflate( Mesh& mesh, const VertBitSet& verts, const InflateSettings & settings )
{
}

void inflate( const MeshTopology& topology, VertCoords& points, const VertBitSet& verts, const InflateSettings & settings )
{
}

void inflate1( const MeshTopology& topology, VertCoords& points, const VertBitSet& verts, float pressure )
{
}

} //namespace MR
