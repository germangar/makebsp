#include "MRMeshBoolean.h"

#include "detail/TypeCast.h"

#include "MRMesh/MRMeshBoolean.h"
#include "MRMeshC.h"
#include "MRMesh/MRObjectMeshData.h"
#include "MRMesh/MRProjectionMeshAttribute.h"
#include "MRMesh/MRRegionBoundary.h"

using namespace MR;

REGISTER_AUTO_CAST( AffineXf3f )
REGISTER_AUTO_CAST( Mesh )
REGISTER_AUTO_CAST( BooleanOperation )
REGISTER_AUTO_CAST( BooleanResultMapper )
REGISTER_AUTO_CAST2( std::string, MRString )

MRBooleanParameters mrBooleanParametersNew( void )
{
    static const BooleanParameters def;
    return {
        .rigidB2A = auto_cast( def.rigidB2A ),
        .mapper = auto_cast( def.mapper ),
        .mergeAllNonIntersectingComponents = def.mergeAllNonIntersectingComponents,
        .cb = nullptr,
    };
}

MRBooleanResult mrBoolean( const MRMesh* meshA_, const MRMesh* meshB_, MRBooleanOperation operation_, const MRBooleanParameters* params_ )
{
    ARG( meshA ); ARG( meshB ); ARG_VAL( operation );

    BooleanParameters params;
    if ( params_ )
    {
        params = {
            .rigidB2A = auto_cast( params_->rigidB2A ),
            .mapper = auto_cast( params_->mapper ),
            .mergeAllNonIntersectingComponents = params_->mergeAllNonIntersectingComponents,
            .cb = params_->cb,
        };
    }
    auto res = MR::boolean( meshA, meshB, operation, params );
    return {
        .mesh = auto_cast( new_from( std::move( res.mesh ) ) ),
        .errorString = auto_cast( new_from( std::move( res.errorString ) ) ),
    };
}

MRBooleanResult mrBooleanWithAttributes(
    const MRMesh* meshA_, const MRMeshAttributes* attrsA,
    const MRMesh* meshB_, const MRMeshAttributes* attrsB,
    MRBooleanOperation op_, const MRBooleanParameters* params_,
    MRMeshAttributes* outAttrs )
{
    ARG( meshA ); ARG( meshB ); ARG_VAL( op );

    MR::ObjectMeshData dataA, dataB;
    dataA.mesh = std::make_shared<MR::Mesh>( meshA );
    dataB.mesh = std::make_shared<MR::Mesh>( meshB );

    if ( attrsA && attrsA->uvCoords && attrsA->numUvs > 0 )
    {
        auto* src = reinterpret_cast<MR::UVCoord*>( attrsA->uvCoords );
        dataA.uvCoordinates.vec_.assign( src, src + attrsA->numUvs );
    }
    if ( attrsA && attrsA->vertColors && attrsA->numColors > 0 )
    {
        auto* src = reinterpret_cast<MR::Color*>( attrsA->vertColors );
        dataA.vertColors.vec_.assign( src, src + attrsA->numColors );
    }

    if ( attrsB && attrsB->uvCoords && attrsB->numUvs > 0 )
    {
        auto* src = reinterpret_cast<MR::UVCoord*>( attrsB->uvCoords );
        dataB.uvCoordinates.vec_.assign( src, src + attrsB->numUvs );
    }
    if ( attrsB && attrsB->vertColors && attrsB->numColors > 0 )
    {
        auto* src = reinterpret_cast<MR::Color*>( attrsB->vertColors );
        dataB.vertColors.vec_.assign( src, src + attrsB->numColors );
    }

    MR::BooleanResultMapper mapper;
    MR::BooleanParameters params;
    if ( params_ )
    {
        params = {
            .rigidB2A = auto_cast( params_->rigidB2A ),
            .mapper = &mapper,
            .mergeAllNonIntersectingComponents = params_->mergeAllNonIntersectingComponents,
            .cb = params_->cb,
        };
    }
    else
    {
        params.mapper = &mapper;
    }

    auto res = MR::boolean( meshA, meshB, op, params );

    if ( !res.valid() )
    {
        if ( outAttrs )
        {
            outAttrs->uvCoords   = nullptr; outAttrs->numUvs    = 0;
            outAttrs->vertColors = nullptr; outAttrs->numColors = 0;
        }
        return {
            .mesh = nullptr,
            .errorString = auto_cast( new_from( std::move( res.errorString ) ) ),
        };
    }

    MR::ObjectMeshData outData;
    outData.mesh = std::make_shared<MR::Mesh>( std::move( res.mesh ) );

    MR::FaceBitSet allFacesA( meshA.topology.faceSize(), true );
    MR::FaceBitSet allFacesB( meshB.topology.faceSize(), true );
    MR::FaceBitSet regionA = mapper.map( allFacesA, MR::BooleanResultMapper::MapObject::A );
    MR::FaceBitSet regionB = mapper.map( allFacesB, MR::BooleanResultMapper::MapObject::B );

    MR::ObjectMeshData outDataA = outData;
    MR::ObjectMeshData outDataB = outData;

    auto pResA = MR::projectObjectMeshData( dataA, outDataA, &regionA );
    auto pResB = MR::projectObjectMeshData( dataB, outDataB, &regionB );

    outData.uvCoordinates = std::move( outDataA.uvCoordinates );
    outData.vertColors = std::move( outDataA.vertColors );

    MR::VertBitSet vertsB = MR::getIncidentVerts( outData.mesh->topology, regionB );
    for ( MR::VertId v : vertsB )
    {
        if ( size_t( v ) < outDataB.uvCoordinates.size() )
        {
            if ( outData.uvCoordinates.size() <= size_t( v ) )
                outData.uvCoordinates.resize( outDataB.uvCoordinates.size() );
            outData.uvCoordinates[v] = outDataB.uvCoordinates[v];
        }
        if ( size_t( v ) < outDataB.vertColors.size() )
        {
            if ( outData.vertColors.size() <= size_t( v ) )
                outData.vertColors.resize( outDataB.vertColors.size() );
            outData.vertColors[v] = outDataB.vertColors[v];
        }
    }

    if ( outAttrs )
    {
        if ( !outData.uvCoordinates.empty() )
        {
            size_t cap = outData.uvCoordinates.size();
            MRVector2f* newUvs = (MRVector2f*)malloc( cap * sizeof( MRVector2f ) );
            if ( newUvs )
            {
                memcpy( newUvs, outData.uvCoordinates.data(), cap * sizeof( MRVector2f ) );
                outAttrs->uvCoords = newUvs;
                outAttrs->numUvs = cap;
            }
            else
            {
                outAttrs->uvCoords = nullptr;
                outAttrs->numUvs = 0;
            }
        }
        else
        {
            outAttrs->uvCoords = nullptr;
            outAttrs->numUvs = 0;
        }
        
        if ( !outData.vertColors.empty() )
        {
            size_t cap = outData.vertColors.size();
            MRColor* newColors = (MRColor*)malloc( cap * sizeof( MRColor ) );
            if ( newColors )
            {
                memcpy( newColors, outData.vertColors.data(), cap * sizeof( MRColor ) );
                outAttrs->vertColors = newColors;
                outAttrs->numColors = cap;
            }
            else
            {
                outAttrs->vertColors = nullptr;
                outAttrs->numColors = 0;
            }
        }
        else
        {
            outAttrs->vertColors = nullptr;
            outAttrs->numColors = 0;
        }
    }

    return {
        .mesh = auto_cast( new_from( std::move( *outData.mesh ) ) ),
        .errorString = auto_cast( new_from( std::move( res.errorString ) ) ),
    };
}
