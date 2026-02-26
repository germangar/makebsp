#include "qbsp.h"
#include "../libs/meshoptimizer/src/meshoptimizer.h"

void TestMeshOptimizer(void) {
    _printf("MeshOptimizer library included successfully.\n");
    // Just a placeholder call to ensure the library is linked correctly
    meshopt_encodeIndexVersion(1);
    meshopt_encodeVertexVersion(1);
}
