#ifndef COACD_API_H
#define COACD_API_H

#include <stdbool.h>
#include <stdint.h>


/* Static linking, so we don't need dllimport/dllexport */
#define COACD_API

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CoACD_Mesh {
  double *vertices_ptr;
  uint64_t vertices_count;
  int *triangles_ptr;
  uint64_t triangles_count;
} CoACD_Mesh;

typedef struct CoACD_MeshArray {
  CoACD_Mesh *meshes_ptr;
  uint64_t meshes_count;
} CoACD_MeshArray;

COACD_API void CoACD_freeMeshArray(CoACD_MeshArray arr);

#define COACD_PREPROCESS_AUTO 0
#define COACD_PREPROCESS_ON 1
#define COACD_PREPROCESS_OFF 2

#define COACD_APX_CH 0
#define COACD_APX_BOX 1

COACD_API CoACD_MeshArray CoACD_run(const CoACD_Mesh *input, double threshold,
                                    int max_convex_hull, int preprocess_mode,
                                    int prep_resolution, int sample_resolution,
                                    int mcts_nodes, int mcts_iteration,
                                    int mcts_max_depth, bool pca, bool merge,
                                    bool decimate, int max_ch_vertex,
                                    bool extrude, double extrude_margin,
                                    int apx_mode, unsigned int seed);

COACD_API void CoACD_setLogLevel(char const *level);

#ifdef __cplusplus
}
#endif

#endif
