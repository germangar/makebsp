#include "hacdHACD.h"
#include "hacd_c_wrapper.h"
#include <vector>

struct HACD_Wrapper {
    HACD::HACD hacd;
    std::vector<HACD::Vec3<HACD::Real>> points;
    std::vector<HACD::Vec3<long>> triangles;
};

extern "C" {

HACD_Wrapper* hacd_new(void) {
    return new HACD_Wrapper();
}

void hacd_delete(HACD_Wrapper* hacd) {
    delete hacd;
}

void hacd_set_points(HACD_Wrapper* hacd, const HACD_Vec3* points, size_t nPoints) {
    hacd->points.clear();
    hacd->points.reserve(nPoints);
    for (size_t i = 0; i < nPoints; ++i) {
        hacd->points.push_back(HACD::Vec3<HACD::Real>((HACD::Real)points[i].x, (HACD::Real)points[i].y, (HACD::Real)points[i].z));
    }
    hacd->hacd.SetPoints(hacd->points.data());
    hacd->hacd.SetNPoints(nPoints);
}

void hacd_set_triangles(HACD_Wrapper* hacd, const HACD_Triangle* triangles, size_t nTriangles) {
    hacd->triangles.clear();
    hacd->triangles.reserve(nTriangles);
    for (size_t i = 0; i < nTriangles; ++i) {
        hacd->triangles.push_back(HACD::Vec3<long>(triangles[i].v1, triangles[i].v2, triangles[i].v3));
    }
    hacd->hacd.SetTriangles(hacd->triangles.data());
    hacd->hacd.SetNTriangles(nTriangles);
}

void hacd_set_nclusters(HACD_Wrapper* hacd, size_t nClusters) {
    hacd->hacd.SetNClusters(nClusters);
}

void hacd_set_concavity(HACD_Wrapper* hacd, double concavity) {
    hacd->hacd.SetConcavity(concavity);
}

void hacd_set_compacity_weight(HACD_Wrapper* hacd, double alpha) {
    hacd->hacd.SetCompacityWeight(alpha);
}

void hacd_set_volume_weight(HACD_Wrapper* hacd, double beta) {
    hacd->hacd.SetVolumeWeight(beta);
}

void hacd_set_add_extra_dist_points(HACD_Wrapper* hacd, bool addExtraDistPoints) {
    hacd->hacd.SetAddExtraDistPoints(addExtraDistPoints);
}

void hacd_set_add_faces_points(HACD_Wrapper* hacd, bool addFacesPoints) {
    hacd->hacd.SetAddFacesPoints(addFacesPoints);
}

void hacd_set_cc_connect_dist(HACD_Wrapper* hacd, double dist) {
    hacd->hacd.SetConnectDist(dist);
}

void hacd_set_disable_normalize(HACD_Wrapper* hacd, bool disable) {
    hacd->hacd.SetDisableNormalize(disable);
}

bool hacd_compute(HACD_Wrapper* hacd, bool fullCH) {
    return hacd->hacd.Compute(fullCH);
}

size_t hacd_get_nclusters(HACD_Wrapper* hacd) {
    return hacd->hacd.GetNClusters();
}

size_t hacd_get_npoints_ch(HACD_Wrapper* hacd, size_t numCH) {
    return hacd->hacd.GetNPointsCH(numCH);
}

size_t hacd_get_ntriangles_ch(HACD_Wrapper* hacd, size_t numCH) {
    return hacd->hacd.GetNTrianglesCH(numCH);
}

bool hacd_get_ch(HACD_Wrapper* hacd, size_t numCH, HACD_Vec3* points, HACD_Triangle* triangles) {
    return hacd->hacd.GetCH(numCH, (HACD::Vec3<HACD::Real>*)points, (HACD::Vec3<long>*)triangles);
}

void hacd_normalize_data(HACD_Wrapper* hacd) {
    hacd->hacd.NormalizeData();
}

void hacd_denormalize_data(HACD_Wrapper* hacd) {
    hacd->hacd.DenormalizeData();
}

}
