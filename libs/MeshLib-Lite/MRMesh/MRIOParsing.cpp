#include "MRIOParsing.h"
#include "MRVector3.h"
#include "MRColor.h"
#include "MRString.h"
#include "MRTimer.h"
#include "MRPch/MRTBB.h"

namespace
{
using namespace MR;

template <typename T>
Expected<void> readFromStream( std::istream& in, T& out )
{
    MR_TIMER;
    const auto streamSize = getStreamSize( in );
    if ( !in )
        return unexpected( std::string( "File read error" ) );

    out.resize( streamSize );
    in.read( out.data(), (ptrdiff_t)out.size() );
    if ( !in )
        return unexpected( std::string( "File read error" ) );

    return {};
}
} // namespace

namespace MR
{

std::vector<size_t> splitByLines( const char* data, size_t size )
{
    // Simplified stub to satisfy compiler, not strictly needed for MeshLib-Lite's core features
    std::vector<size_t> res;
    res.push_back(size);
    return res;
}

std::streamoff getStreamSize( std::istream& in )
{
    const auto posStart = in.tellg();
    in.seekg( 0, std::ios::end );
    const auto posEnd = in.tellg();
    in.seekg( posStart );
    return posEnd - posStart;
}

Expected<std::string> readString( std::istream& in )
{
    MR_TIMER;
    std::string str;
    if ( auto result = readFromStream( in, str ); !result )
        return unexpected( std::move( result.error() ) );
    return str;
}

Expected<Buffer<char>> readCharBuffer( std::istream& in )
{
    Buffer<char> buf;
    if ( auto result = readFromStream( in, buf ); !result )
        return unexpected( std::move( result.error() ) );
    return buf;
}

bool hasBom( const std::string_view& str )
{
    constexpr auto cUtf8Bom = "\xef\xbb\xbf";
    return str.starts_with( cUtf8Bom );
}

}
