#ifndef HACD_C_WRAPPER_H
#define HACD_C_WRAPPER_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HACD_Wrapper HACD_Wrapper;

typedef struct {
    double x, y, z;
} HACD_Vec3;

typedef struct {
    long v1, v2, v3;
} HACD_Triangle;

HACD_Wrapper* hacd_new(void);
void hacd_delete(HACD_Wrapper* hacd);

void hacd_set_points(HACD_Wrapper* hacd, const HACD_Vec3* points, size_t nPoints);
void hacd_set_triangles(HACD_Wrapper* hacd, const HACD_Triangle* triangles, size_t nTriangles);

void hacd_set_nclusters(HACD_Wrapper* hacd, size_t nClusters);
void hacd_set_concavity(HACD_Wrapper* hacd, double concavity);
void hacd_set_compacity_weight(HACD_Wrapper* hacd, double alpha);
void hacd_set_volume_weight(HACD_Wrapper* hacd, double beta);
void hacd_set_add_extra_dist_points(HACD_Wrapper* hacd, bool addExtraDistPoints);
void hacd_set_add_faces_points(HACD_Wrapper* hacd, bool addFacesPoints);
void hacd_set_cc_connect_dist(HACD_Wrapper* hacd, double dist);
void hacd_set_disable_normalize(HACD_Wrapper* hacd, bool disable);
void hacd_set_scale_factor(HACD_Wrapper* hacd, double scale);

bool hacd_compute(HACD_Wrapper* hacd, bool fullCH);

size_t hacd_get_nclusters(HACD_Wrapper* hacd);
size_t hacd_get_npoints_ch(HACD_Wrapper* hacd, size_t numCH);
size_t hacd_get_ntriangles_ch(HACD_Wrapper* hacd, size_t numCH);
bool hacd_get_ch(HACD_Wrapper* hacd, size_t numCH, HACD_Vec3* points, HACD_Triangle* triangles);

void hacd_normalize_data(HACD_Wrapper* hacd);
void hacd_denormalize_data(HACD_Wrapper* hacd);

#ifdef __cplusplus
}
#endif

#endif
