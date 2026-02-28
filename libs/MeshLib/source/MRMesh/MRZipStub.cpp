#include "MRZip.h"
#include "MRExpected.h"
#include <iostream>

namespace MR {
Expected<void> decompressZip( const std::filesystem::path&, const std::filesystem::path&, const char * ) {
    return unexpected("Zip support disabled in this build");
}
Expected<void> decompressZip( std::istream&, const std::filesystem::path&, const char * ) {
    return unexpected("Zip support disabled in this build");
}
Expected<void> compressZip( const std::filesystem::path&, const std::filesystem::path&, const CompressZipSettings& ) {
    return unexpected("Zip support disabled in this build");
}
Expected<void> compressZip( const std::filesystem::path&, const std::filesystem::path&, const std::vector<std::filesystem::path>&, const char *, ProgressCallback ) {
    return unexpected("Zip support disabled in this build");
}
}
