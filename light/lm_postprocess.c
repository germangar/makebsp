#include "light.h"
#include <omp.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef min
#define min(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef clamp
#define clamp(v, a, b) min(max(v, a), b)
#endif

#define AA_ANGLE_MATCH_COS 0.85f

static int lightmapAA;
static float lightmapSmoothRadius;
static int lightmapSmoothPasses;

#define FILTER_UPSCALE 1
#define TRISOUP_SMOOTH_CHEAT(A) ((A) + (useOpenCL ? 1.0f : 1.5f))

typedef struct { vec3_t pos; vec3_t normal; qboolean valid; } pixelCache_t;

typedef struct {
	int surfaceNum; vec3_t origin; vec3_t vecs[2]; float invMagSq[2]; int width, height; int lmNum; int lmOffset[2];
	vec3_t normal; float dist; int surfaceFlags; int contentFlags; int numPartners; int *partners;
	float smoothingRadius;
} planarInfo_t;

static planarInfo_t *planarSurfaces = NULL;
static int numPlanarSurfaces = 0;
static int *planarSortIndex = NULL;

typedef struct aaTexel_s { vec3_t pos; vec3_t normal; vec3_t color; vec3_t dir; vec3_t nrm; float density; struct aaTexel_s *next; } aaTexel_t;

#define POS_TO_INT(p) ((int)roundf((p) * 128.0f))
typedef struct { int v[2][3]; } edge_t;
typedef struct { edge_t edge; int surfaceIdx; } edgeRef_t;

static int ComparePlanarInfo(const void *a, const void *b) {
	const planarInfo_t *pa = &planarSurfaces[*(const int *)a], *pb = &planarSurfaces[*(const int *)b];
	for (int i=0; i<3; i++) { if (pa->normal[i] < pb->normal[i]-0.0001f) return -1; if (pa->normal[i] > pb->normal[i]+0.0001f) return 1; }
	if (pa->dist < pb->dist-0.01f) return -1; if (pa->dist > pb->dist+0.01f) return 1; return 0;
}

static int CompareEdges(const void *a, const void *b) {
	const edgeRef_t *ea = (const edgeRef_t *)a, *eb = (const edgeRef_t *)b;
	for (int i=0; i<2; i++) for (int j=0; j<3; j++) { if (ea->edge.v[i][j] < eb->edge.v[i][j]) return -1; if (ea->edge.v[i][j] > eb->edge.v[i][j]) return 1; }
	return 0;
}

void BuildPlanarSurfaceIndex(void) {
	int i, j, k;
	if (planarSurfaces) { for (i=0; i<numPlanarSurfaces; i++) if (planarSurfaces[i].partners) free(planarSurfaces[i].partners); free(planarSurfaces); }
	if (planarSortIndex) free(planarSortIndex); numPlanarSurfaces = 0;
	planarSurfaces = malloc(numDrawSurfaces * sizeof(planarInfo_t));
	planarSortIndex = malloc(numDrawSurfaces * sizeof(int));
	for (i=0; i<numDrawSurfaces; i++) {
		dsurface_t *ds = &drawSurfaces[i]; if (ds->lightmapNum[0] < 0) continue;
		if (ds->surfaceType != MST_PLANAR && ds->surfaceType != MST_PATCH) continue;
		planarInfo_t *p = &planarSurfaces[numPlanarSurfaces]; planarSortIndex[numPlanarSurfaces] = numPlanarSurfaces; numPlanarSurfaces++;
		p->surfaceNum = i; VectorCopy(ds->lightmapOrigin, p->origin);
		if (ds->surfaceType == MST_PLANAR) {
			VectorMA(p->origin, -0.5f, ds->lightmapVecs[0], p->origin); VectorMA(p->origin, -0.5f, ds->lightmapVecs[1], p->origin);
			VectorCopy(ds->lightmapVecs[0], p->vecs[0]); VectorCopy(ds->lightmapVecs[1], p->vecs[1]);
		} else {
			mesh_t *m = localSurfaces[i].patchMesh;
			if (m && m->width>1 && m->height>1) {
				vec3_t vU, vV; VectorSubtract(m->verts[1].xyz, m->verts[0].xyz, vU); VectorSubtract(m->verts[m->width].xyz, m->verts[0].xyz, vV);
				VectorMA(p->origin, -0.5f, vU, p->origin); VectorMA(p->origin, -0.5f, vV, p->origin);
				VectorCopy(vU, p->vecs[0]); VectorCopy(vV, p->vecs[1]);
			} else { VectorClear(p->vecs[0]); VectorClear(p->vecs[1]); }
		}
		VectorAdd(p->origin, localSurfaces[i].entityOrigin, p->origin);
		p->invMagSq[0] = (DotProduct(p->vecs[0], p->vecs[0]) > 0.0001f) ? 1.0f / DotProduct(p->vecs[0], p->vecs[0]) : 0;
		p->invMagSq[1] = (DotProduct(p->vecs[1], p->vecs[1]) > 0.0001f) ? 1.0f / DotProduct(p->vecs[1], p->vecs[1]) : 0;
		p->width = ds->lightmapWidth; p->height = ds->lightmapHeight; p->lmNum = ds->lightmapNum[0];
		p->lmOffset[0] = ds->lightmapOffset[0][0]; p->lmOffset[1] = ds->lightmapOffset[0][1];
		CrossProduct(p->vecs[0], p->vecs[1], p->normal); VectorNormalize(p->normal, p->normal); p->dist = DotProduct(p->origin, p->normal);
		p->surfaceFlags = dshaders[ds->shaderNum].surfaceFlags; p->contentFlags = dshaders[ds->shaderNum].contentFlags;
		p->smoothingRadius = localSurfaces[i].smoothingRadius;
		p->numPartners = 0; p->partners = NULL;
	}
	if (numPlanarSurfaces == 0) return;
	edgeRef_t *allEdges = malloc(numDrawIndexes * sizeof(edgeRef_t)); int numEdges = 0;
	for (i=0; i<numPlanarSurfaces; i++) {
		dsurface_t *ds = &drawSurfaces[planarSurfaces[i].surfaceNum];
		for (j=0; j<ds->numIndexes; j+=3) for (k=0; k<3; k++) {
			int idx1 = ds->firstVert+drawIndexes[ds->firstIndex+j+k], idx2 = ds->firstVert+drawIndexes[ds->firstIndex+j+((k+1)%3)];
			vec3_t p1, p2; VectorCopy(drawVerts[idx1].xyz, p1); VectorCopy(drawVerts[idx2].xyz, p2);
			int ip1[3]={POS_TO_INT(p1[0]),POS_TO_INT(p1[1]),POS_TO_INT(p1[2])}, ip2[3]={POS_TO_INT(p2[0]),POS_TO_INT(p2[1]),POS_TO_INT(p2[2])};
			edgeRef_t *e = &allEdges[numEdges++]; e->surfaceIdx = i;
			if (ip1[0]>ip2[0] || (ip1[0]==ip2[0]&&ip1[1]>ip2[1]) || (ip1[0]==ip2[0]&&ip1[1]==ip2[1]&&ip1[2]>ip2[2])) {
				for(int m=0;m<3;m++){e->edge.v[0][m]=ip2[m]; e->edge.v[1][m]=ip1[m];}
			} else { for(int m=0;m<3;m++){e->edge.v[0][m]=ip1[m]; e->edge.v[1][m]=ip2[m];} }
		}
	}
	qsort(allEdges, numEdges, sizeof(edgeRef_t), CompareEdges);
	for (i=0; i<numEdges; ) {
		int next = i+1; while (next < numEdges && CompareEdges(&allEdges[i], &allEdges[next]) == 0) next++;
		if (next > i+1) for (j=i; j<next; j++) for (k=j+1; k<next; k++) {
			int s1=allEdges[j].surfaceIdx, s2=allEdges[k].surfaceIdx; if (s1==s2) continue;
			planarInfo_t *p1=&planarSurfaces[s1], *p2=&planarSurfaces[s2];
			if (DotProduct(p1->normal, p2->normal)<0.99f || fabs(p1->dist-p2->dist)>0.1f) continue;
			qboolean f=qfalse; for(int m=0;m<p1->numPartners;m++) if(p1->partners[m]==s2){f=qtrue; break;}
			if(!f){ p1->partners=realloc(p1->partners, (p1->numPartners+1)*sizeof(int)); p1->partners[p1->numPartners++]=s2; }
			f=qfalse; for(int m=0;m<p2->numPartners;m++) if(p2->partners[m]==s1){f=qtrue; break;}
			if(!f){ p2->partners=realloc(p2->partners, (p2->numPartners+1)*sizeof(int)); p2->partners[p2->numPartners++]=s1; }
		}
		i=next;
	}
	free(allEdges); qsort(planarSortIndex, numPlanarSurfaces, sizeof(int), ComparePlanarInfo);
}

void FreePlanarSurfaceIndex(void) {
	if (planarSurfaces) { for (int i=0; i<numPlanarSurfaces; i++) if (planarSurfaces[i].partners) free(planarSurfaces[i].partners); free(planarSurfaces); }
	if (planarSortIndex) free(planarSortIndex); planarSurfaces=NULL; planarSortIndex=NULL; numPlanarSurfaces=0;
}

qboolean SampleLightmapWorldBilinear(int srcIdx, const vec3_t pos, const vec3_t normal, float *out, const float *buf) {
	if (srcIdx<0 || srcIdx>=numPlanarSurfaces) return qfalse;
	planarInfo_t *srcP = &planarSurfaces[srcIdx];
	for (int i=0; i<srcP->numPartners; i++) {
		planarInfo_t *p = &planarSurfaces[srcP->partners[i]]; vec3_t d; VectorSubtract(pos, p->origin, d);
		float u = DotProduct(d, p->vecs[0])*p->invMagSq[0], v = DotProduct(d, p->vecs[1])*p->invMagSq[1];
		if (u<-0.51f || u>(float)p->width-0.49f || v<-0.51f || v>(float)p->height-0.49f) continue;
		float ux=u-0.5f, vy=v-0.5f; int x0=(int)floorf(ux), y0=(int)floorf(vy); float fx=ux-x0, fy=vy-y0;
		int x1=x0+1, y1=y0+1; x0=max(0,min(p->width-1,x0)); x1=max(0,min(p->width-1,x1)); y0=max(0,min(p->height-1,y0)); y1=max(0,min(p->height-1,y1));
		int p00=(p->lmNum*LIGHTMAP_HEIGHT+p->lmOffset[1]+y0)*LIGHTMAP_WIDTH+p->lmOffset[0]+x0;
		int p10=(p->lmNum*LIGHTMAP_HEIGHT+p->lmOffset[1]+y0)*LIGHTMAP_WIDTH+p->lmOffset[0]+x1;
		int p01=(p->lmNum*LIGHTMAP_HEIGHT+p->lmOffset[1]+y1)*LIGHTMAP_WIDTH+p->lmOffset[0]+x0;
		int p11=(p->lmNum*LIGHTMAP_HEIGHT+p->lmOffset[1]+y1)*LIGHTMAP_WIDTH+p->lmOffset[0]+x1;
		float w00=(1-fx)*(1-fy), w10=fx*(1-fy), w01=(1-fx)*fy, w11=fx*fy;
		if (lightAlphaMask[p00]==0) w00=0; if (lightAlphaMask[p10]==0) w10=0; if (lightAlphaMask[p01]==0) w01=0; if (lightAlphaMask[p11]==0) w11=0;
		float sW = w00+w10+w01+w11; if (sW>0.01f) { for(int c=0;c<3;c++) out[c]=(w00*buf[p00*3+c]+w10*buf[p10*3+c]+w01*buf[p01*3+c]+w11*buf[p11*3+c])/sW; return qtrue; }
	}
	return qfalse;
}

static qboolean GetFilteredTexel(int sIdx, float px, float py, float *out, const float *buf) {
	planarInfo_t *pI = &planarSurfaces[sIdx]; dsurface_t *ds = &drawSurfaces[pI->surfaceNum];
	float ux=px-0.5f, vy=py-0.5f; int x0=(int)floorf(ux), y0=(int)floorf(vy); float fx=ux-x0, fy=vy-y0;
	if (x0>=0 && x0+1<ds->lightmapWidth && y0>=0 && y0+1<ds->lightmapHeight) {
		int x1=x0+1, y1=y0+1, base=(ds->lightmapNum[0]*LIGHTMAP_HEIGHT+ds->lightmapOffset[0][1])*LIGHTMAP_WIDTH+ds->lightmapOffset[0][0];
		int p00=base+y0*LIGHTMAP_WIDTH+x0, p10=base+y0*LIGHTMAP_WIDTH+x1, p01=base+y1*LIGHTMAP_WIDTH+x0, p11=base+y1*LIGHTMAP_WIDTH+x1;
		float w00=(1-fx)*(1-fy), w10=fx*(1-fy), w01=(1-fx)*fy, w11=fx*fy;
		if (lightAlphaMask[p00]==0) w00=0; if (lightAlphaMask[p10]==0) w10=0; if (lightAlphaMask[p01]==0) w01=0; if (lightAlphaMask[p11]==0) w11=0;
		float sW = w00+w10+w01+w11; if (sW>0.01f) { for(int c=0;c<3;c++) out[c]=(w00*buf[p00*3+c]+w10*buf[p10*3+c]+w01*buf[p01*3+c]+w11*buf[p11*3+c])/sW; return qtrue; }
	}
	vec3_t wP; VectorMA(pI->origin, px, pI->vecs[0], wP); VectorMA(wP, py, pI->vecs[1], wP);
	if (SampleLightmapWorldBilinear(sIdx, wP, ds->lightmapVecs[2], out, buf)) return qtrue;
	int cx=max(0,min(ds->lightmapWidth-1,x0)), cy=max(0,min(ds->lightmapHeight-1,y0));
	int p = (ds->lightmapNum[0]*LIGHTMAP_HEIGHT+ds->lightmapOffset[0][1]+cy)*LIGHTMAP_WIDTH+ds->lightmapOffset[0][0]+cx;
	if (lightAlphaMask[p]==0) return qfalse; VectorCopy(&buf[p*3], out); return qtrue;
}

static const float ssPattern8[][2] = { {0,0}, {-0.354f,-0.854f}, {0.354f,-0.354f}, {0.854f,0.146f}, {0.354f,0.646f}, {-0.146f,0.354f}, {-0.646f,-0.146f}, {-0.854f,0.354f} };

static float GetSurfaceTexelSize(dsurface_t *ds) {
    if (ds->numIndexes == 0) return (float)samplesize;
    float tW=0, tUV=0; for (int j=0; j<ds->numIndexes; j+=3) for (int k=0; k<3; k++) {
        drawVert_t *v0=&drawVerts[ds->firstVert+drawIndexes[ds->firstIndex+j+k]], *v1=&drawVerts[ds->firstVert+drawIndexes[ds->firstIndex+j+((k+1)%3)]];
        vec3_t dW; VectorSubtract(v0->xyz, v1->xyz, dW); float wD=VectorLength(dW);
        float dU=(v0->lightmap[0][0]-v1->lightmap[0][0])*LIGHTMAP_WIDTH, dV=(v0->lightmap[0][1]-v1->lightmap[0][1])*LIGHTMAP_HEIGHT, uvD=sqrtf(dU*dU+dV*dV);
        if (uvD>0.001f) { tW+=wD; tUV+=uvD; }
    }
    return (tUV>0.001f) ? clamp(tW/tUV, 0.1f, 256.0f) : (float)samplesize;
}
void GpuLightmapState_Upload(void) {
    int s, x, y; GpuLightmapState *st = &g_gpuLM; cl_int err;
    int scale = FILTER_UPSCALE ? 2 : 1; st->upscale = scale;
    int totalP1x = numLightBytes/3, totalP = totalP1x*scale*scale; st->totalAtlasPixels=totalP; st->numPlanarSurfaces=numPlanarSurfaces; st->pingIsA=1;
    size_t aB = (size_t)totalP*3*sizeof(float), mB = (size_t)totalP*sizeof(byte);
    float *tA = lightFloats; byte *tM = lightAlphaMask;
    if (scale > 1) {
        if (verbose) _printf("  Upscaling atlas to 2x...\n");
        tA = malloc(aB); tM = malloc(mB); int nL=totalP1x/(LIGHTMAP_WIDTH*LIGHTMAP_HEIGHT), W=LIGHTMAP_WIDTH, H=LIGHTMAP_HEIGHT, Ws=W*scale, Hs=H*scale;
        #pragma omp parallel for schedule(static)
        for (int m=0; m<nL; m++) for (int ys=0; ys<Hs; ys++) for (int xs=0; xs<Ws; xs++) {
            int ps=(m*Hs+ys)*Ws+xs; float fx=((float)xs+0.5f)/scale-0.5f, fy=((float)ys+0.5f)/scale-0.5f; int ix0=(int)floorf(fx), iy0=(int)floorf(fy); float tx=fx-ix0, ty=fy-iy0;
            int ix1=max(0,min(W-1,ix0+1)), iy1=max(0,min(H-1,iy0+1)); ix0=max(0,min(W-1,ix0)); iy0=max(0,min(H-1,iy0));
            int i00=(m*H+iy0)*W+ix0, i10=(m*H+iy0)*W+ix1, i01=(m*H+iy1)*W+ix0, i11=(m*H+iy1)*W+ix1;
            float w00=(1-tx)*(1-ty), w10=tx*(1-ty), w01=(1-tx)*ty, w11=tx*ty; if (lightAlphaMask[i00]==0) w00=0; if (lightAlphaMask[i10]==0) w10=0; if (lightAlphaMask[i01]==0) w01=0; if (lightAlphaMask[i11]==0) w11=0;
            float *dst=&tA[ps*3], sW=w00+w10+w01+w11; if (sW>0.01f) { float iW=1.0f/sW; for(int c=0;c<3;c++) dst[c]=(w00*lightFloats[i00*3+c]+w10*lightFloats[i10*3+c]+w01*lightFloats[i01*3+c]+w11*lightFloats[i11*3+c])*iW; }
            else { int in=(lightAlphaMask[i00]!=0)?i00:(lightAlphaMask[i10]!=0?i10:(lightAlphaMask[i01]!=0?i01:i11)); VectorCopy(&lightFloats[in*3], dst); }
            tM[ps] = lightAlphaMask[(m*H+(ys/scale))*W+(xs/scale)];
        }
        #pragma omp parallel for schedule(dynamic, 1)
        for (int sidx=0; sidx<numPlanarSurfaces; sidx++) {
            planarInfo_t *p=&planarSurfaces[sidx]; dsurface_t *ds=&drawSurfaces[p->surfaceNum];
            int sWs=ds->lightmapWidth*scale, sHs=ds->lightmapHeight*scale, Hs=LIGHTMAP_HEIGHT*scale, Ws=LIGHTMAP_WIDTH*scale, oXs=ds->lightmapOffset[0][0]*scale, oYs=ds->lightmapOffset[0][1]*scale;
            for (int ys=0; ys<sHs; ys++) for (int xs=0; xs<sWs; xs++) {
                float px=((float)xs+0.5f)/scale, py=((float)ys+0.5f)/scale, col[3]; int pa=(ds->lightmapNum[0]*Hs+oYs+ys)*Ws+oXs+xs;
                if (GetFilteredTexel(sidx,px,py,col,lightFloats)) { VectorCopy(col,&tA[pa*3]); tM[pa]=ds->surfaceType; } else tM[pa]=0;
            }
        }
    }
    st->atlasA=clCreateBuffer(g_clContext, CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR, aB, tA, &err);
    st->atlasB=clCreateBuffer(g_clContext, CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR, aB, tA, &err);
    st->maskBuf=clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, mB, tM, &err);
    if (scale>1) { free(tA); free(tM); }

    // Upload deluxe directions and surface normals (same upscale logic, guarded by deluxeFloats)
    if (deluxeFloats) {
        float *tdA = deluxeFloats;
        float *tnA = normalFloats;
        if (scale > 1) {
            tdA = malloc(aB);
            if (tnA) tnA = malloc(aB);
            int nL=totalP1x/(LIGHTMAP_WIDTH*LIGHTMAP_HEIGHT), W=LIGHTMAP_WIDTH, H=LIGHTMAP_HEIGHT, Ws=W*scale, Hs=H*scale;
            #pragma omp parallel for schedule(static)
            for (int m=0; m<nL; m++) for (int ys=0; ys<Hs; ys++) for (int xs=0; xs<Ws; xs++) {
                int ps=(m*Hs+ys)*Ws+xs;
                float fx=((float)xs+0.5f)/scale-0.5f, fy=((float)ys+0.5f)/scale-0.5f;
                int ix0=(int)floorf(fx), iy0=(int)floorf(fy); float tx=fx-ix0, ty=fy-iy0;
                int ix1=max(0,min(W-1,ix0+1)), iy1=max(0,min(H-1,iy0+1)); ix0=max(0,min(W-1,ix0)); iy0=max(0,min(H-1,iy0));
                int i00=(m*H+iy0)*W+ix0, i10=(m*H+iy0)*W+ix1, i01=(m*H+iy1)*W+ix0, i11=(m*H+iy1)*W+ix1;
                float w00=(1-tx)*(1-ty), w10=tx*(1-ty), w01=(1-tx)*ty, w11=tx*ty;
                if (lightAlphaMask[i00]==0) w00=0; if (lightAlphaMask[i10]==0) w10=0;
                if (lightAlphaMask[i01]==0) w01=0; if (lightAlphaMask[i11]==0) w11=0;
                float sW=w00+w10+w01+w11;
                if (sW>0.01f) {
                    float iW=1.0f/sW;
                    float ddx=(w00*deluxeFloats[i00*3]+w10*deluxeFloats[i10*3]+w01*deluxeFloats[i01*3]+w11*deluxeFloats[i11*3])*iW;
                    float ddy=(w00*deluxeFloats[i00*3+1]+w10*deluxeFloats[i10*3+1]+w01*deluxeFloats[i01*3+1]+w11*deluxeFloats[i11*3+1])*iW;
                    float ddz=(w00*deluxeFloats[i00*3+2]+w10*deluxeFloats[i10*3+2]+w01*deluxeFloats[i01*3+2]+w11*deluxeFloats[i11*3+2])*iW;
                    float dlen=sqrtf(ddx*ddx+ddy*ddy+ddz*ddz);
                    if (dlen>0.001f) { tdA[ps*3]=ddx/dlen; tdA[ps*3+1]=ddy/dlen; tdA[ps*3+2]=ddz/dlen; }
                    else { tdA[ps*3]=deluxeFloats[i00*3]; tdA[ps*3+1]=deluxeFloats[i00*3+1]; tdA[ps*3+2]=deluxeFloats[i00*3+2]; }
                    if (normalFloats && tnA) {
                        float nnx=(w00*normalFloats[i00*3]+w10*normalFloats[i10*3]+w01*normalFloats[i01*3]+w11*normalFloats[i11*3])*iW;
                        float nny=(w00*normalFloats[i00*3+1]+w10*normalFloats[i10*3+1]+w01*normalFloats[i01*3+1]+w11*normalFloats[i11*3+1])*iW;
                        float nnz=(w00*normalFloats[i00*3+2]+w10*normalFloats[i10*3+2]+w01*normalFloats[i01*3+2]+w11*normalFloats[i11*3+2])*iW;
                        float nlen=sqrtf(nnx*nnx+nny*nny+nnz*nnz);
                        if (nlen>0.001f) { tnA[ps*3]=nnx/nlen; tnA[ps*3+1]=nny/nlen; tnA[ps*3+2]=nnz/nlen; }
                        else { tnA[ps*3]=normalFloats[i00*3]; tnA[ps*3+1]=normalFloats[i00*3+1]; tnA[ps*3+2]=normalFloats[i00*3+2]; }
                    }
                } else {
                    int in=(lightAlphaMask[i00]!=0)?i00:(lightAlphaMask[i10]!=0?i10:(lightAlphaMask[i01]!=0?i01:i11));
                    tdA[ps*3]=deluxeFloats[in*3]; tdA[ps*3+1]=deluxeFloats[in*3+1]; tdA[ps*3+2]=deluxeFloats[in*3+2];
                    if (normalFloats && tnA) { tnA[ps*3]=normalFloats[in*3]; tnA[ps*3+1]=normalFloats[in*3+1]; tnA[ps*3+2]=normalFloats[in*3+2]; }
                }
            }
        }
        st->deluxeA = clCreateBuffer(g_clContext, CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR, aB, tdA, &err);
        st->deluxeB = clCreateBuffer(g_clContext, CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR, aB, tdA, &err);
        if (normalFloats && tnA) {
            st->normalA = clCreateBuffer(g_clContext, CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR, aB, tnA, &err);
            st->normalB = clCreateBuffer(g_clContext, CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR, aB, tnA, &err);
        }
        if (scale>1) { free(tdA); if (tnA && tnA != normalFloats) free(tnA); }
    }

    GpuPlanarSurface_t *cPs = malloc(numPlanarSurfaces*sizeof(GpuPlanarSurface_t)); int tL=0; float fS=scale, sh=(0.5f/fS)-0.5f;
    for (s=0; s<numPlanarSurfaces; s++) {
        planarInfo_t *p=&planarSurfaces[s]; GpuPlanarSurface_t *g=&cPs[s];
        g->originX=p->origin[0]+sh*p->vecs[0][0]+sh*p->vecs[1][0]; g->originY=p->origin[1]+sh*p->vecs[0][1]+sh*p->vecs[1][1]; g->originZ=p->origin[2]+sh*p->vecs[0][2]+sh*p->vecs[1][2];
        g->vecs0X=p->vecs[0][0]/fS; g->vecs0Y=p->vecs[0][1]/fS; g->vecs0Z=p->vecs[0][2]/fS; g->vecs1X=p->vecs[1][0]/fS; g->vecs1Y=p->vecs[1][1]/fS; g->vecs1Z=p->vecs[1][2]/fS;
        g->invMagSq0=p->invMagSq[0]*fS*fS; g->invMagSq1=p->invMagSq[1]*fS*fS; g->width=p->width*scale; g->height=p->height*scale; g->lmNum=p->lmNum; g->lmOffX=p->lmOffset[0]*scale; g->lmOffY=p->lmOffset[1]*scale;
        tL+=p->numPartners;
    }
    st->surfacesBuf=clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, numPlanarSurfaces*sizeof(GpuPlanarSurface_t), cPs, &err); free(cPs);
    int *off=malloc((numPlanarSurfaces+1)*sizeof(int)), *dat=malloc((tL>0?tL:1)*sizeof(int)); off[0]=0;
    for(s=0;s<numPlanarSurfaces;s++) { int b=off[s]; for(int i=0;i<planarSurfaces[s].numPartners;i++) dat[b+i]=planarSurfaces[s].partners[i]; off[s+1]=b+planarSurfaces[s].numPartners; }
    st->partnerOffsets=clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, (numPlanarSurfaces+1)*sizeof(int), off, &err);
    st->partnerData=clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, (tL>0?tL:1)*sizeof(int), dat, &err); free(off); free(dat);
    int *pS=malloc(totalP*sizeof(int)), *pX=malloc(totalP*sizeof(int)), *pY=malloc(totalP*sizeof(int)), *vL=malloc(totalP*sizeof(int));
    memset(pS, -1, totalP*sizeof(int)); int nV=0; int W=LIGHTMAP_WIDTH*scale, H=LIGHTMAP_HEIGHT*scale;
    for (s=0; s<numPlanarSurfaces; s++) {
        dsurface_t *ds=&drawSurfaces[planarSurfaces[s].surfaceNum]; int lm=ds->lightmapNum[0], sW=ds->lightmapWidth*scale, sH=ds->lightmapHeight*scale, oX=ds->lightmapOffset[0][0]*scale, oY=ds->lightmapOffset[0][1]*scale;
        for(y=0;y<sH;y++) for(x=0;x<sW;x++) { int p=(lm*H+oY+y)*W+oX+x, p1=(lm*LIGHTMAP_HEIGHT+(oY+y)/scale)*LIGHTMAP_WIDTH+(oX+x)/scale; if(lightAlphaMask[p1]==0)continue; pS[p]=s; pX[p]=x; pY[p]=y; vL[nV++]=p; }
    }
    st->numValid=nV; st->validList=clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, nV*sizeof(int), vL, &err);
    float *radii = malloc(numPlanarSurfaces * sizeof(float));
    for (s = 0; s < numPlanarSurfaces; s++) radii[s] = planarSurfaces[s].smoothingRadius;
    st->radiiBuf = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, numPlanarSurfaces * sizeof(float), radii, &err);
    free(radii);
    st->pixelToSurface=clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, totalP*sizeof(int), pS, &err);
    st->pixelToX=clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, totalP*sizeof(int), pX, &err);
    st->pixelToY=clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, totalP*sizeof(int), pY, &err);
    free(pS); free(pX); free(pY); free(vL);
}

static void RunGpuAAKernel(float *pattern, int numSamples) {
    GpuLightmapState *st = &g_gpuLM; cl_int err; static cl_program prog = NULL;
    if (!prog) { prog = BuildOpenCLProgramWithCommon("aa_filter.cl", ""); if (!prog) return; }
    cl_kernel kernel = clCreateKernel(prog, "aa_filter", &err);
    cl_mem src = st->pingIsA?st->atlasA:st->atlasB, dst = st->pingIsA?st->atlasB:st->atlasA, patB=clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, (size_t)numSamples*2*sizeof(float), pattern, &err);
    int a=0; clSetKernelArg(kernel,a++,sizeof(cl_mem),&src); clSetKernelArg(kernel,a++,sizeof(cl_mem),&dst); clSetKernelArg(kernel,a++,sizeof(cl_mem),&st->maskBuf); clSetKernelArg(kernel,a++,sizeof(cl_mem),&st->surfacesBuf);
    clSetKernelArg(kernel,a++,sizeof(cl_mem),&st->partnerData); clSetKernelArg(kernel,a++,sizeof(cl_mem),&st->partnerOffsets); clSetKernelArg(kernel,a++,sizeof(cl_mem),&st->validList); clSetKernelArg(kernel,a++,sizeof(cl_mem),&st->pixelToSurface);
    clSetKernelArg(kernel,a++,sizeof(cl_mem),&st->pixelToX); clSetKernelArg(kernel,a++,sizeof(cl_mem),&st->pixelToY); clSetKernelArg(kernel,a++,sizeof(cl_mem),&patB); clSetKernelArg(kernel,a++,sizeof(cl_mem),&st->radiiBuf);
    float uS=(float)st->upscale; clSetKernelArg(kernel,a++,sizeof(int),&numSamples); clSetKernelArg(kernel,a++,sizeof(float),&uS);
    size_t gS=(size_t)st->numValid; clEnqueueNDRangeKernel(g_clQueue, kernel, 1, NULL, &gS, NULL, 0, NULL, NULL); clFinish(g_clQueue); st->pingIsA^=1; clReleaseMemObject(patB); clReleaseKernel(kernel);
}

static void RunGpuSmoothKernel(void) {
    GpuLightmapState *st = &g_gpuLM; cl_int err;
    static cl_program prog=NULL; if(!prog) { prog=BuildOpenCLProgramWithCommon("smooth_filter.cl", ""); if(!prog) return; }
    cl_kernel k=clCreateKernel(prog, "smooth_filter", &err);
    cl_mem src=st->pingIsA?st->atlasA:st->atlasB, dst=st->pingIsA?st->atlasB:st->atlasA;
    cl_mem dSrc=st->pingIsA?st->deluxeA:st->deluxeB, dDst=st->pingIsA?st->deluxeB:st->deluxeA;
    cl_mem nSrc=st->pingIsA?st->normalA:st->normalB, nDst=st->pingIsA?st->normalB:st->normalA;
    int a=0; clSetKernelArg(k,a++,sizeof(cl_mem),&src); clSetKernelArg(k,a++,sizeof(cl_mem),&dst); clSetKernelArg(k,a++,sizeof(cl_mem),&st->maskBuf); clSetKernelArg(k,a++,sizeof(cl_mem),&st->surfacesBuf);
    clSetKernelArg(k,a++,sizeof(cl_mem),&st->partnerData); clSetKernelArg(k,a++,sizeof(cl_mem),&st->partnerOffsets); clSetKernelArg(k,a++,sizeof(cl_mem),&st->validList); clSetKernelArg(k,a++,sizeof(cl_mem),&st->pixelToSurface);
    clSetKernelArg(k,a++,sizeof(cl_mem),&st->pixelToX); clSetKernelArg(k,a++,sizeof(cl_mem),&st->pixelToY); clSetKernelArg(k,a++,sizeof(cl_mem),&st->radiiBuf);
    float uS=(float)st->upscale; clSetKernelArg(k,a++,sizeof(float),&uS);
    // Deluxe/normal direction smoothing buffers (NULL-safe: kernel checks internally)
    clSetKernelArg(k,a++,sizeof(cl_mem),&dSrc); clSetKernelArg(k,a++,sizeof(cl_mem),&dDst);
    clSetKernelArg(k,a++,sizeof(cl_mem),&nSrc); clSetKernelArg(k,a++,sizeof(cl_mem),&nDst);
    size_t gS=(size_t)st->numValid; clEnqueueNDRangeKernel(g_clQueue, k, 1, NULL, &gS, NULL, 0, NULL, NULL); clFinish(g_clQueue); st->pingIsA^=1; clReleaseKernel(k);
}


void AntiAliasLightmapsGPU(int p) { float pat[16]; for(int i=0;i<8;i++){pat[i*2]=ssPattern8[i][0];pat[i*2+1]=ssPattern8[i][1];} for(int i=0;i<p;i++) RunGpuAAKernel(pat, 8); }
void SmoothLightmapsGPU(void) { RunGpuSmoothKernel(); }

static void FilterPlanarSurfaceHighFidelityCPU(int sIdx, float radius, const float *tF, int aaP, int smP) {
    dsurface_t *ds=&drawSurfaces[planarSurfaces[sIdx].surfaceNum]; int W=ds->lightmapWidth, H=ds->lightmapHeight; if(W<=0||H<=0)return; int W2=W*2, H2=H*2;
    float *g2=malloc(W2*H2*3*sizeof(float)), *b2=malloc(W2*H2*3*sizeof(float)); byte *m2=malloc(W2*H2), *bm2=malloc(W2*H2);
    float *g2dir = deluxeFloats ? malloc(W2*H2*3*sizeof(float)) : NULL; float *b2dir = deluxeFloats ? malloc(W2*H2*3*sizeof(float)) : NULL;
    float *g2nrm = normalFloats ? malloc(W2*H2*3*sizeof(float)) : NULL; float *b2nrm = normalFloats ? malloc(W2*H2*3*sizeof(float)) : NULL;
    if(!g2||!b2||!m2||!bm2){if(g2)free(g2);if(b2)free(b2);if(m2)free(m2);if(bm2)free(bm2);if(g2dir)free(g2dir);if(b2dir)free(b2dir);if(g2nrm)free(g2nrm);if(b2nrm)free(b2nrm);return;}
    for(int Y=0;Y<H2;Y++) for(int X=0;X<W2;X++) { 
        float px=(X+0.5f)*0.5f, py=(Y+0.5f)*0.5f, col[3], dir[3], nrm[3]; 
        if(GetFilteredTexel(sIdx,px,py,col,tF)){
            m2[Y*W2+X]=ds->surfaceType;VectorCopy(col,&g2[(Y*W2+X)*3]);
            if (deluxeFloats) {
                if(GetFilteredTexel(sIdx,px,py,dir,deluxeFloats)) { VectorCopy(dir,&g2dir[(Y*W2+X)*3]); } else { VectorClear(&g2dir[(Y*W2+X)*3]); }
            }
            if (normalFloats) {
                if(GetFilteredTexel(sIdx,px,py,nrm,normalFloats)) { VectorCopy(nrm,&g2nrm[(Y*W2+X)*3]); } else { VectorClear(&g2nrm[(Y*W2+X)*3]); }
            }
        } else {
            m2[Y*W2+X]=0;VectorClear(&g2[(Y*W2+X)*3]);
            if (deluxeFloats) VectorClear(&g2dir[(Y*W2+X)*3]);
            if (normalFloats) VectorClear(&g2nrm[(Y*W2+X)*3]);
        } 
    }
    int totP=aaP+smP; float sig=radius*2/3.0f; if(sig<1)sig=1; int kR=min(32,(int)ceilf(radius*2)); float gK[65][65], gKS=0;
    for(int j=-kR;j<=kR;j++) for(int i=-kR;i<=kR;i++) { gK[j+kR][i+kR]=expf(-(float)(i*i+j*j)/(2*sig*sig)); gKS+=gK[j+kR][i+kR]; }
    for(int j=0;j<=kR*2;j++) for(int i=0;i<=kR*2;i++) gK[j][i]/=gKS;
    for(int pass=0;pass<totP;pass++) {
        qboolean isAA=(pass<aaP); for(int Y=0;Y<H2;Y++) for(int X=0;X<W2;X++) {
            if(m2[Y*W2+X]==0){bm2[Y*W2+X]=0;continue;} float sC[3]={0,0,0}, sD[3]={0,0,0}, sN[3]={0,0,0}, sW=0, sDW=0;
            if(isAA) for(int k=0;k<8;k++) { 
                float px=X+ssPattern8[k][0]*radius*2, py=Y+ssPattern8[k][1]*radius*2; int ix=(int)roundf(px), iy=(int)roundf(py);
                if(ix>=0&&ix<W2&&iy>=0&&iy<H2 && m2[iy*W2+ix]!=0) { 
                    VectorAdd(sC,&g2[(iy*W2+ix)*3],sC); sW+=1; 
                    float lum = 0.2126f * g2[(iy*W2+ix)*3] + 0.7152f * g2[(iy*W2+ix)*3+1] + 0.0722f * g2[(iy*W2+ix)*3+2];
                    if (deluxeFloats) VectorMA(sD, lum, &g2dir[(iy*W2+ix)*3], sD);
                    if (normalFloats) VectorMA(sN, lum, &g2nrm[(iy*W2+ix)*3], sN);
                    sDW += lum;
                } else { 
                    float sX=(px+0.5f)*0.5f, sY=(py+0.5f)*0.5f, c[3], d[3], n[3]; 
                    if(GetFilteredTexel(sIdx,sX,sY,c,tF)){
                        VectorAdd(sC,c,sC); sW+=1;
                        float lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
                        if (deluxeFloats && GetFilteredTexel(sIdx,sX,sY,d,deluxeFloats)) VectorMA(sD, lum, d, sD);
                        if (normalFloats && GetFilteredTexel(sIdx,sX,sY,n,normalFloats)) VectorMA(sN, lum, n, sN);
                        sDW += lum;
                    } 
                }
            } else for(int j=-kR;j<=kR;j++) for(int i=-kR;i<=kR;i++) { 
                float w=gK[j+kR][i+kR]; int ix=X+i, iy=Y+j;
                if(ix>=0&&ix<W2&&iy>=0&&iy<H2 && m2[iy*W2+ix]!=0) { 
                    VectorMA(sC,w,&g2[(iy*W2+ix)*3],sC); sW+=w; 
                    float lum = 0.2126f * g2[(iy*W2+ix)*3] + 0.7152f * g2[(iy*W2+ix)*3+1] + 0.0722f * g2[(iy*W2+ix)*3+2];
                    if (deluxeFloats) VectorMA(sD, w*lum, &g2dir[(iy*W2+ix)*3], sD);
                    if (normalFloats) VectorMA(sN, w*lum, &g2nrm[(iy*W2+ix)*3], sN);
                    sDW += w*lum;
                } else { 
                    float sX=(ix+0.5f)*0.5f, sY=(iy+0.5f)*0.5f, c[3], d[3], n[3]; 
                    if(GetFilteredTexel(sIdx,sX,sY,c,tF)){
                        VectorMA(sC,w,c,sC); sW+=w;
                        float lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
                        if (deluxeFloats && GetFilteredTexel(sIdx,sX,sY,d,deluxeFloats)) VectorMA(sD, w*lum, d, sD);
                        if (normalFloats && GetFilteredTexel(sIdx,sX,sY,n,normalFloats)) VectorMA(sN, w*lum, n, sN);
                        sDW += w*lum;
                    } 
                }
            }
            if(sW>0.0001f){
                bm2[Y*W2+X]=ds->surfaceType;VectorScale(sC,1/sW,&b2[(Y*W2+X)*3]);
                if (deluxeFloats) {
                    if (sDW>0.0001f && VectorLength(sD)>0.001f) { VectorNormalize(sD, &b2dir[(Y*W2+X)*3]); }
                    else { VectorCopy(&g2dir[(Y*W2+X)*3], &b2dir[(Y*W2+X)*3]); }
                }
                if (normalFloats) {
                    if (sDW>0.0001f && VectorLength(sN)>0.001f) { VectorNormalize(sN, &b2nrm[(Y*W2+X)*3]); }
                    else { VectorCopy(&g2nrm[(Y*W2+X)*3], &b2nrm[(Y*W2+X)*3]); }
                }
            } else bm2[Y*W2+X]=0;
        }
        memcpy(g2,b2,W2*H2*3*sizeof(float)); memcpy(m2,bm2,W2*H2);
        if (deluxeFloats) memcpy(g2dir,b2dir,W2*H2*3*sizeof(float));
        if (normalFloats) memcpy(g2nrm,b2nrm,W2*H2*3*sizeof(float));
    }
    for(int y=0;y<H;y++) for(int x=0;x<W;x++) { 
        int p=(ds->lightmapNum[0]*LIGHTMAP_HEIGHT+ds->lightmapOffset[0][1]+y)*LIGHTMAP_WIDTH+ds->lightmapOffset[0][0]+x; if(lightAlphaMask[p]!=ds->surfaceType)continue;
        float sC[3]={0,0,0}, sD[3]={0,0,0}, sN[3]={0,0,0}, sW=0, sDW=0; 
        for(int dy=0;dy<2;dy++) for(int dx=0;dx<2;dx++) { 
            int X=x*2+dx, Y=y*2+dy; 
            if(m2[Y*W2+X]!=0){
                VectorAdd(sC,&g2[(Y*W2+X)*3],sC); sW+=1;
                float lum = 0.2126f * g2[(Y*W2+X)*3] + 0.7152f * g2[(Y*W2+X)*3+1] + 0.0722f * g2[(Y*W2+X)*3+2];
                if (deluxeFloats) VectorMA(sD, lum, &g2dir[(Y*W2+X)*3], sD);
                if (normalFloats) VectorMA(sN, lum, &g2nrm[(Y*W2+X)*3], sN);
                sDW += lum;
            } 
        }
        if(sW>0.01f) {
            VectorScale(sC,1/sW,&lightFloats[p*3]);
            if (deluxeFloats) {
                if (sDW>0.0001f && VectorLength(sD)>0.001f) { VectorNormalize(sD, &deluxeFloats[p*3]); }
            }
            if (normalFloats) {
                if (sDW>0.0001f && VectorLength(sN)>0.001f) { VectorNormalize(sN, &normalFloats[p*3]); }
            }
        }
    }
    free(g2); free(m2); free(b2); free(bm2);
    if (g2dir) free(g2dir); if (b2dir) free(b2dir);
    if (g2nrm) free(g2nrm); if (b2nrm) free(b2nrm);
}

static void ProcessTrisoupVolumetricGPU(int surfIdx, float radius, float *tempFloats, int aaPasses, int smoothPasses) {
    dsurface_t *ds = &drawSurfaces[surfIdx]; if (ds->lightmapNum[0] < 0 || ds->surfaceType != MST_TRIANGLE_SOUP) return; if (aaPasses <= 0 && smoothPasses <= 0) return;
    float tS = GetSurfaceTexelSize(ds), eR = (smoothPasses > 0) ? TRISOUP_SMOOTH_CHEAT(radius) : radius, sR = eR * tS; if (sR < 0.1f) return;
    float vS = sR, mDSq = sR*sR, sig = sR/3.0f, tS2 = 2*sig*sig; if (sig < 0.1f) sig = 0.1f;
    int nP = 0; voxelPoint_t *cP = VoxelCache_Load(surfIdx, &nP); if (!cP || nP == 0) { if (cP) free(cP); return; }
    int N = nP, nS = (aaPasses > 0) ? 8 : 1; ThreadLock(); _printf("."); fflush(stdout); ThreadUnlock();
    float *tP=malloc(N*3*4), *tN=malloc(N*3*4), *tC=malloc(N*3*4); int *vL=malloc(N*4), *tX=malloc(N*4), *tY=malloc(N*4);
    float *jP=malloc((size_t)N*nS*3*4), *jN=malloc((size_t)N*nS*3*4); byte *jV=malloc((size_t)N*nS);
    float *tD=deluxeFloats?malloc(N*3*4):NULL, *tNr=normalFloats?malloc(N*3*4):NULL;
    if (!tP||!tN||!tC||!vL||!tX||!tY||!jP||!jN||!jV) { 
        free(tP);free(tN);free(tC);free(vL);free(tX);free(tY);free(jP);free(jN);free(jV);free(cP);
        if(tD)free(tD);if(tNr)free(tNr);
        return; 
    }
    memset(jV, 0, (size_t)N*nS); vec3_t gMin={99999,99999,99999}, gMax={-99999,-99999,-99999};
    for (int i=0; i<N; i++) {
        int p = cP[i].pixelIndex; vL[i]=p; int lL=p%(LIGHTMAP_WIDTH*LIGHTMAP_HEIGHT); tX[i]=lL%LIGHTMAP_WIDTH-ds->lightmapOffset[0][0]; tY[i]=lL/LIGHTMAP_WIDTH-ds->lightmapOffset[0][1];
        VectorCopy(cP[i].pos, &tP[i*3]); VectorCopy(cP[i].normal, &tN[i*3]); for (int k=0; k<3; k++) { gMin[k]=min(gMin[k],tP[i*3+k]); gMax[k]=max(gMax[k],tP[i*3+k]); }
        VectorCopy(cP[i].pos, &jP[i*nS*3]); VectorCopy(cP[i].normal, &jN[i*nS*3]); jV[i*nS]=1;
    }
    if (aaPasses > 0) {
        #pragma omp parallel for schedule(dynamic, 64)
        for (int i=0; i<N; i++) {
            for (int k=1; k<nS; k++) {
                float st[2] = { (float)ds->lightmapOffset[0][0]+tX[i]+0.5f+ssPattern8[k][0]*radius, (float)ds->lightmapOffset[0][1]+tY[i]+0.5f+ssPattern8[k][1]*radius };
                if (TriSoupSamplePoint(ds, st, &jP[(i*nS+k)*3], &jN[(i*nS+k)*3])) jV[i*nS+k]=1;
            }
        }
    }
    for (int k=0; k<3; k++) { gMin[k]-=vS; gMax[k]+=vS; } float mR=0.1f; for(int k=0;k<3;k++) mR=max(mR, gMax[k]-gMin[k]);
    if (vS<1.0f) vS=1.0f; if (mR/vS>128.0f) vS=mR/128.0f;
    int gD[3]; for(int k=0;k<3;k++){ gD[k]=(int)ceilf((gMax[k]-gMin[k])/vS); if(gD[k]<1)gD[k]=1; }
    size_t nB=(size_t)gD[0]*gD[1]*gD[2], gS1=(size_t)gD[1]*gD[2], gS2=(size_t)gD[2];
    int *bC=calloc(nB, 4), *bS=malloc(nB*4), *sT=malloc(N*4), *wP=malloc(nB*4);
    if (!bC||!bS||!sT||!wP) { 
        free(bC);free(bS);free(sT);free(wP);free(tP);free(tN);free(tC);free(vL);free(tX);free(tY);free(jP);free(jN);free(jV);free(cP); 
        if(tD)free(tD);if(tNr)free(tNr);
        return; 
    }
    for (int i=0; i<N; i++) { int v[3]; for(int k=0;k<3;k++) v[k]=(int)((tP[i*3+k]-gMin[k])/vS); if(v[0]>=0&&v[0]<gD[0]&&v[1]>=0&&v[1]<gD[1]&&v[2]>=0&&v[2]<gD[2]) bC[(size_t)v[0]*gS1+v[1]*gS2+v[2]]++; }
    bS[0]=0; for(size_t i=1;i<nB;i++) bS[i]=bS[i-1]+bC[i-1]; memcpy(wP, bS, nB*4);
    for (int i=0; i<N; i++) { int v[3]; for(int k=0;k<3;k++) v[k]=(int)((tP[i*3+k]-gMin[k])/vS); if(v[0]>=0&&v[0]<gD[0]&&v[1]>=0&&v[1]<gD[1]&&v[2]>=0&&v[2]<gD[2]) sT[wP[(size_t)v[0]*gS1+v[1]*gS2+v[2]]++]=i; }
    cl_int err; cl_program prog=BuildOpenCLProgram("trisoup_filter.cl", "");
    if (prog) {
        cl_kernel dK=clCreateKernel(prog,"trisoup_density",&err), fK=clCreateKernel(prog,"trisoup_filter",&err);
        if (dK && fK) {
            size_t aBy=(size_t)(numLightBytes/3)*3*4, nf3=(size_t)N*3*4, bBy=nB*4;
            cl_mem btP=clCreateBuffer(g_clContext,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,nf3,tP,&err);
            cl_mem btN=clCreateBuffer(g_clContext,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,nf3,tN,&err);
            cl_mem bjP=clCreateBuffer(g_clContext,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,(size_t)N*nS*3*4,jP,&err);
            cl_mem bjN=clCreateBuffer(g_clContext,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,(size_t)N*nS*3*4,jN,&err);
            cl_mem bjV=clCreateBuffer(g_clContext,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,(size_t)N*nS,jV,&err);
            cl_mem bbS=clCreateBuffer(g_clContext,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,bBy,bS,&err);
            cl_mem bbC=clCreateBuffer(g_clContext,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,bBy,bC,&err);
            cl_mem bsT=clCreateBuffer(g_clContext,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,(size_t)N*4,sT,&err);
            cl_mem bOut=clCreateBuffer(g_clContext,CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR,aBy,lightFloats,&err);
            cl_mem bOutD=deluxeFloats?clCreateBuffer(g_clContext,CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR,aBy,deluxeFloats,&err):NULL;
            cl_mem bOutN=normalFloats?clCreateBuffer(g_clContext,CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR,aBy,normalFloats,&err):NULL;
            cl_mem bvL=clCreateBuffer(g_clContext,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,(size_t)N*4,vL,&err);
            cl_mem btC=clCreateBuffer(g_clContext,CL_MEM_READ_ONLY,nf3,NULL,&err);
            cl_mem btD=deluxeFloats?clCreateBuffer(g_clContext,CL_MEM_READ_ONLY,nf3,NULL,&err):NULL;
            cl_mem btNr=normalFloats?clCreateBuffer(g_clContext,CL_MEM_READ_ONLY,nf3,NULL,&err):NULL;
            cl_mem bDen=clCreateBuffer(g_clContext,CL_MEM_READ_WRITE,(size_t)N*4,NULL,&err);
            
            for (int p=0; p<aaPasses+smoothPasses; p++) {
                int smp=(p<aaPasses)?8:1; 
                for(int i=0;i<N;i++) { 
                    VectorCopy(&lightFloats[vL[i]*3],&tC[i*3]); 
                    if(deluxeFloats) VectorCopy(&deluxeFloats[vL[i]*3],&tD[i*3]);
                    if(normalFloats) VectorCopy(&normalFloats[vL[i]*3],&tNr[i*3]);
                }
                clEnqueueWriteBuffer(g_clQueue,btC,CL_TRUE,0,nf3,tC,0,NULL,NULL);
                if(deluxeFloats) clEnqueueWriteBuffer(g_clQueue,btD,CL_TRUE,0,nf3,tD,0,NULL,NULL);
                if(normalFloats) clEnqueueWriteBuffer(g_clQueue,btNr,CL_TRUE,0,nf3,tNr,0,NULL,NULL);
                
                int da=0; clSetKernelArg(dK,da++,sizeof(cl_mem),&btP); clSetKernelArg(dK,da++,sizeof(cl_mem),&btN); clSetKernelArg(dK,da++,sizeof(cl_mem),&bbS); clSetKernelArg(dK,da++,sizeof(cl_mem),&bbC);
                clSetKernelArg(dK,da++,sizeof(cl_mem),&bsT); clSetKernelArg(dK,da++,sizeof(cl_mem),&bDen); for(int k=0;k<3;k++) clSetKernelArg(dK,da++,4,&gMin[k]);
                clSetKernelArg(dK,da++,4,&vS); for(int k=0;k<3;k++) clSetKernelArg(dK,da++,4,&gD[k]); clSetKernelArg(dK,da++,4,&mDSq); clSetKernelArg(dK,da++,4,&tS2);
                float aM=AA_ANGLE_MATCH_COS; clSetKernelArg(dK,da++,4,&aM); clSetKernelArg(dK,da++,4,&N);
                size_t gS=(size_t)N; clEnqueueNDRangeKernel(g_clQueue,dK,1,NULL,&gS,NULL,0,NULL,NULL); clFinish(g_clQueue);
                
                int fa=0; clSetKernelArg(fK,fa++,sizeof(cl_mem),&btP); clSetKernelArg(fK,fa++,sizeof(cl_mem),&btN); clSetKernelArg(fK,fa++,sizeof(cl_mem),&bjP); clSetKernelArg(fK,fa++,sizeof(cl_mem),&bjN);
                clSetKernelArg(fK,fa++,sizeof(cl_mem),&bjV); clSetKernelArg(fK,fa++,sizeof(cl_mem),&btC); clSetKernelArg(fK,fa++,sizeof(cl_mem),&bDen); clSetKernelArg(fK,fa++,sizeof(cl_mem),&bbS);
                clSetKernelArg(fK,fa++,sizeof(cl_mem),&bbC); clSetKernelArg(fK,fa++,sizeof(cl_mem),&bsT); clSetKernelArg(fK,fa++,sizeof(cl_mem),&bOut); clSetKernelArg(fK,fa++,sizeof(cl_mem),&bvL);
                for(int k=0;k<3;k++) clSetKernelArg(fK,fa++,4,&gMin[k]); clSetKernelArg(fK,fa++,4,&vS); for(int k=0;k<3;k++) clSetKernelArg(fK,fa++,4,&gD[k]);
                clSetKernelArg(fK,fa++,4,&mDSq); clSetKernelArg(fK,fa++,4,&tS2); clSetKernelArg(fK,fa++,4,&aM); clSetKernelArg(fK,fa++,4,&smp); clSetKernelArg(fK,fa++,4,&N);
                // Extra args for directions
                cl_mem dNULL=NULL;
                if(deluxeFloats){clSetKernelArg(fK,fa++,sizeof(cl_mem),&btD); clSetKernelArg(fK,fa++,sizeof(cl_mem),&bOutD);} else {clSetKernelArg(fK,fa++,sizeof(cl_mem),&dNULL);clSetKernelArg(fK,fa++,sizeof(cl_mem),&dNULL);}
                if(normalFloats){clSetKernelArg(fK,fa++,sizeof(cl_mem),&btNr);clSetKernelArg(fK,fa++,sizeof(cl_mem),&bOutN);} else {clSetKernelArg(fK,fa++,sizeof(cl_mem),&dNULL);clSetKernelArg(fK,fa++,sizeof(cl_mem),&dNULL);}
                
                clEnqueueNDRangeKernel(g_clQueue,fK,1,NULL,&gS,NULL,0,NULL,NULL); clFinish(g_clQueue);
                
                if (p<aaPasses+smoothPasses-1) {
                    clEnqueueReadBuffer(g_clQueue,bOut,CL_TRUE,0,aBy,lightFloats,0,NULL,NULL);
                    if(deluxeFloats) clEnqueueReadBuffer(g_clQueue,bOutD,CL_TRUE,0,aBy,deluxeFloats,0,NULL,NULL);
                    if(normalFloats) clEnqueueReadBuffer(g_clQueue,bOutN,CL_TRUE,0,aBy,normalFloats,0,NULL,NULL);
                }
            }
            clEnqueueReadBuffer(g_clQueue,bOut,CL_TRUE,0,aBy,lightFloats,0,NULL,NULL);
            if(deluxeFloats) clEnqueueReadBuffer(g_clQueue,bOutD,CL_TRUE,0,aBy,deluxeFloats,0,NULL,NULL);
            if(normalFloats) clEnqueueReadBuffer(g_clQueue,bOutN,CL_TRUE,0,aBy,normalFloats,0,NULL,NULL);
            
            clReleaseMemObject(btP); clReleaseMemObject(btN); clReleaseMemObject(bjP); clReleaseMemObject(bjN); clReleaseMemObject(bjV);
            clReleaseMemObject(bbS); clReleaseMemObject(bbC); clReleaseMemObject(bsT); clReleaseMemObject(bOut); clReleaseMemObject(bvL); clReleaseMemObject(btC); clReleaseMemObject(bDen);
            if(bOutD)clReleaseMemObject(bOutD); if(bOutN)clReleaseMemObject(bOutN);
            if(btD)clReleaseMemObject(btD); if(btNr)clReleaseMemObject(btNr);
            clReleaseKernel(fK); clReleaseKernel(dK);
        }
        clReleaseProgram(prog);
    }
    free(tP);free(tN);free(tC);free(vL);free(tX);free(tY);free(jP);free(jN);free(jV);free(bC);free(bS);free(sT);free(wP);free(cP);
    if(tD)free(tD);if(tNr)free(tNr);
}
static void ProcessTrisoupVolumetricCPU(int surfIdx, float radius, float *tF, int aaP, int smP) {
    dsurface_t *ds=&drawSurfaces[surfIdx]; if(ds->lightmapNum[0]<0||ds->surfaceType!=MST_TRIANGLE_SOUP)return;
    float tS=GetSurfaceTexelSize(ds), eR=(smP>0)?TRISOUP_SMOOTH_CHEAT(radius):radius, sR=eR*tS; if(sR<0.1f)return;
    float vS=sR, mDSq=sR*sR, sig=sR/3.0f, tS2=2*sig*sig; if(sig<0.1f)sig=0.1f;
    int nP=0; voxelPoint_t *cP=VoxelCache_Load(surfIdx,&nP); if(!cP||nP==0){if(cP)free(cP);return;}
    vec3_t gMin={99999,99999,99999}, gMax={-99999,-99999,-99999}; for(int i=0;i<nP;i++) for(int k=0;k<3;k++) { gMin[k]=min(gMin[k],cP[i].pos[k]); gMax[k]=max(gMax[k],cP[i].pos[k]); }
    for(int k=0;k<3;k++){gMin[k]-=vS;gMax[k]+=vS;} int gD[3]; for(int k=0;k<3;k++){gD[k]=(int)ceilf((gMax[k]-gMin[k])/vS);if(gD[k]<1)gD[k]=1;}
    const size_t gS1=(size_t)gD[1]*gD[2], gS2=(size_t)gD[2], nB=(size_t)gD[0]*gD[1]*gD[2]; aaTexel_t **fG=calloc(nB,sizeof(aaTexel_t*)), *pool=malloc(nP*sizeof(aaTexel_t));
    for(int i=0;i<nP;i++) { int v[3]; qboolean b=qtrue; for(int k=0;k<3;k++){ v[k]=(int)((cP[i].pos[k]-gMin[k])/vS); if(v[k]<0||v[k]>=gD[k])b=qfalse; }
        if(b){ 
            aaTexel_t *nT=&pool[i]; VectorCopy(cP[i].pos,nT->pos); VectorCopy(cP[i].normal,nT->normal); 
            int p=cP[i].pixelIndex; VectorCopy(&tF[p*3],nT->color); 
            if (deluxeFloats) { VectorCopy(&deluxeFloats[p*3], nT->dir); } else { VectorClear(nT->dir); }
            if (normalFloats) { VectorCopy(&normalFloats[p*3], nT->nrm); } else { VectorClear(nT->nrm); }
            size_t c=(size_t)v[0]*gS1+v[1]*gS2+v[2]; nT->next=fG[c]; fG[c]=nT; 
        }
    }
    for(int pass=0;pass<aaP+smP;pass++) {
        qboolean isAA=(pass<aaP); for(int i=0;i<nP;i++) {
            vec3_t o, n; VectorCopy(cP[i].pos,o); VectorCopy(cP[i].normal,n); int v[3]; for(int k=0;k<3;k++) v[k]=(int)((o[k]-gMin[k])/vS); float lD=0;
            for(int dx=-1;dx<=1;dx++) for(int dy=-1;dy<=1;dy++) for(int dz=-1;dz<=1;dz++) {
                int nx=v[0]+dx, ny=v[1]+dy, nz=v[2]+dz; if(nx>=0&&nx<gD[0]&&ny>=0&&ny<gD[1]&&nz>=0&&nz<gD[2]) {
                    aaTexel_t *c=fG[(size_t)nx*gS1+ny*gS2+nz]; while(c) {
                        vec3_t d; VectorSubtract(o,c->pos,d); float dSq=DotProduct(d,d); if(dSq<mDSq) {
                            float dot=DotProduct(n,c->normal), aW=clamp((dot-AA_ANGLE_MATCH_COS)/(1-AA_ANGLE_MATCH_COS),0.0f,1.0f);
                            if(aW>0) lD+=expf(-dSq/tS2)*aW;
                        } c=c->next;
                    }
                }
            } pool[i].density=(lD>0.0001f)?lD:1.0f;
        }
        for(int i=0;i<nP;i++) {
            int p=cP[i].pixelIndex; const int nS=isAA?8:1; vec3_t fC={0,0,0}, fD={0,0,0}, fN={0,0,0}; float fW=0, fDW=0;
            for(int k=0;k<nS;k++) {
                vec3_t o, n; if(isAA) { float st[2]={(float)ds->lightmapOffset[0][0]+(p%(LIGHTMAP_WIDTH*LIGHTMAP_HEIGHT))%LIGHTMAP_WIDTH-ds->lightmapOffset[0][0]+0.5f+ssPattern8[k][0]*radius, (float)ds->lightmapOffset[0][1]+(p%(LIGHTMAP_WIDTH*LIGHTMAP_HEIGHT))/LIGHTMAP_WIDTH-ds->lightmapOffset[0][1]+0.5f+ssPattern8[k][1]*radius}; if(!TriSoupSamplePoint(ds,st,o,n))continue; }
                else { VectorCopy(cP[i].pos,o); VectorCopy(cP[i].normal,n); }
                int v[3]; for(int m=0;m<3;m++) v[m]=(int)((o[m]-gMin[m])/vS); vec3_t tC={0,0,0}, tD={0,0,0}, tN={0,0,0}; float tW=0, tDW=0;
                for(int dx=-1;dx<=1;dx++) for(int dy=-1;dy<=1;dy++) for(int dz=-1;dz<=1;dz++) {
                    int nx=v[0]+dx, ny=v[1]+dy, nz=v[2]+dz; if(nx>=0&&nx<gD[0]&&ny>=0&&ny<gD[1]&&nz>=0&&nz<gD[2]) {
                        aaTexel_t *c=fG[(size_t)nx*gS1+ny*gS2+nz]; while(c) {
                            vec3_t d; VectorSubtract(o,c->pos,d); float dSq=DotProduct(d,d); if(dSq<mDSq) {
                                float dot=DotProduct(n,c->normal), aW=clamp((dot-AA_ANGLE_MATCH_COS)/(1-AA_ANGLE_MATCH_COS),0.0f,1.0f);
                                if(aW>0) { 
                                    float wN=(expf(-dSq/tS2)*aW)/c->density; 
                                    VectorMA(tC,wN,c->color,tC); tW+=wN; 
                                    float lum=0.2126f*c->color[0] + 0.7152f*c->color[1] + 0.0722f*c->color[2];
                                    if (deluxeFloats) VectorMA(tD,wN*lum,c->dir,tD);
                                    if (normalFloats) VectorMA(tN,wN*lum,c->nrm,tN);
                                    tDW+=wN*lum;
                                }
                            } c=c->next;
                        }
                    }
                } 
                if(tW>0.0001f){ 
                    VectorMA(fC,1/tW,tC,fC); fW+=1; 
                    if (deluxeFloats) { if (tDW>0.0001f && VectorLength(tD)>0.001f) { vec3_t nD; VectorNormalize(tD,nD); VectorAdd(fD,nD,fD); } else { VectorAdd(fD,pool[i].dir,fD); } }
                    if (normalFloats) { if (tDW>0.0001f && VectorLength(tN)>0.001f) { vec3_t nN; VectorNormalize(tN,nN); VectorAdd(fN,nN,fN); } else { VectorAdd(fN,pool[i].nrm,fN); } }
                    fDW+=1;
                }
            } 
            if(fW>0.0001f) {
                VectorScale(fC,1/fW,&lightFloats[p*3]);
                if (deluxeFloats) { if (fDW>0.0001f && VectorLength(fD)>0.001f) { VectorNormalize(fD,&deluxeFloats[p*3]); } }
                if (normalFloats) { if (fDW>0.0001f && VectorLength(fN)>0.001f) { VectorNormalize(fN,&normalFloats[p*3]); } }
            }
        }
        if(pass<aaP+smP-1) for(int i=0;i<nP;i++){ 
            int p=cP[i].pixelIndex; 
            VectorCopy(&lightFloats[p*3],pool[i].color); 
            if (deluxeFloats) VectorCopy(&deluxeFloats[p*3],pool[i].dir);
            if (normalFloats) VectorCopy(&normalFloats[p*3],pool[i].nrm);
        }
    }
    free(pool); free(fG); free(cP);
}

void PostProcessLightmaps(void) {
    lightmapAA=game->antialiasingPasses; lightmapSmoothRadius=game->defaultSmoothRadius; lightmapSmoothPasses=game->defaultSmoothPasses;
    _printf("--- Post Processing ---\n"); BuildPlanarSurfaceIndex(); double start=I_FloatTime();
    if (useOpenCL) {
        GpuLightmapState_Upload();
        if(!useOpenCL) goto fallback;
        AntiAliasLightmapsGPU(lightmapAA);
        if(lightmapSmoothPasses>0){
            _printf("  Smoothing surfaces (%d passes): ", lightmapSmoothPasses);
            for(int i=1;i<=lightmapSmoothPasses;i++){ _printf("%d ",i); SmoothLightmapsGPU(); }
            _printf("Done\n");
        }
        GpuLightmapState_Download(); GpuLightmapState_Free();
    } else {
    fallback:
        if(FILTER_UPSCALE) {
            _printf("  High-Fidelity Filtering: "); int prg=0;
            #pragma omp parallel for schedule(dynamic,1)
            for(int s=0;s<numPlanarSurfaces;s++) {
                float r = planarSurfaces[s].smoothingRadius;
                if (r > 0.0f) {
                    FilterPlanarSurfaceHighFidelityCPU(s,r,lightFloats,lightmapAA,lightmapSmoothPasses);
                }
                int c;
                #pragma omp atomic capture
                c=++prg;
                if(numPlanarSurfaces>=10 && (c*10/numPlanarSurfaces > (c-1)*10/numPlanarSurfaces)) { ThreadLock(); _printf("."); ThreadUnlock(); }
            }
            _printf("Done\n");
        }
    }
    if (lightmapAA>0 || lightmapSmoothPasses>0) {
        float r=lightmapSmoothRadius; _printf("  Volumetric Filtering: "); float *tF=malloc((size_t)numLightBytes*4);
        memcpy(tF,lightFloats,(size_t)numLightBytes*4); int prg=0;
        if(useOpenCL) {
            for(int s=0;s<numDrawSurfaces;s++){
                float r = localSurfaces[s].smoothingRadius;
                if (r > 0.0f) {
                    ProcessTrisoupVolumetricGPU(s,r,tF,lightmapAA,lightmapSmoothPasses);
                }
                int c=++prg; if(numDrawSurfaces>=10 && (c*10/numDrawSurfaces > (c-1)*10/numDrawSurfaces)) { ThreadLock(); _printf("."); ThreadUnlock(); }
            }
        } else {
            #pragma omp parallel for schedule(dynamic,1)
            for(int s=0;s<numDrawSurfaces;s++){
                float r = localSurfaces[s].smoothingRadius;
                if (r > 0.0f) {
                    ProcessTrisoupVolumetricCPU(s,r,tF,lightmapAA,lightmapSmoothPasses);
                }
                int c;
                #pragma omp atomic capture
                c=++prg;
                if(numDrawSurfaces>=10 && (c*10/numDrawSurfaces > (c-1)*10/numDrawSurfaces)) { ThreadLock(); _printf("."); ThreadUnlock(); }
            }
        }
        free(tF); _printf("Done\n");
    }
    _printf("  Total filtering time: %.2f seconds\n", I_FloatTime()-start); FreePlanarSurfaceIndex();
}
