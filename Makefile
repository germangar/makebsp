CC = gcc
CXX = g++
CFLAGS = -O2 -Wall -I. -Icommon -Ilibs -Ilibs/pak -Iq3map -Ishared -Ilight_gpu -Ilibs/assimp/include -Ilibs/coacd/public -Ilibs/MeshLib-Lite/eigen -Ilibs/hacd -Ilibs/MeshLib-Lite/MRMeshC -Ilibs/MeshLib-Lite -Ilibs/embree/prebuilt/windows/include -DMRMESH_STATIC_LIB -DMRMESH_NO_GTEST -D_WIN32 -DNDEBUG -D_CONSOLE -DWITH_3RD_PARTY_LIBS=0 -DSTB_IMAGE_IMPLEMENTATION
CXXFLAGS = $(CFLAGS) -Ilibs/MeshLib-Lite -Ilibs/MeshLib-Lite/MRMesh -Ilibs/MeshLib-Lite/MRPch -Ilibs/MeshLib-Lite/tbb -Ilibs/MeshLib-Lite/parallel_hashmap -Wno-class-memaccess
BASE_LDFLAGS = -mconsole -lwsock32 -lws2_32 -lopengl32 -lglu32 -lm -lstdc++ -fopenmp
Q3MAP_LDFLAGS = $(BASE_LDFLAGS) -Llibs/assimp/lib -lassimp -Llibs/coacd/build -lcoacd -lz
Q3LIGHT_LDFLAGS = $(BASE_LDFLAGS)
LIGHT_LDFLAGS = $(BASE_LDFLAGS) -Llibs/embree/prebuilt/windows/lib -lembree4 -ltbb12

# Directories
COMMON_DIR = common
Q3MAP_DIR = q3map
SHARED_DIR = shared
Q3LIGHT_DIR = light
LIGHT_DIR = light_embree
PAK_DIR = libs/pak
HACD_DIR = libs/hacd
OBJ_DIR = obj
OBJ_LITE_DIR = obj_lite

# Source files
COMMON_SRC = $(wildcard $(COMMON_DIR)/*.c)
SHARED_SRC = $(wildcard $(SHARED_DIR)/*.c)
Q3MAP_SRC = $(filter-out $(Q3MAP_DIR)/nodraw.c $(Q3MAP_DIR)/misc_model_old.c, $(wildcard $(Q3MAP_DIR)/*.c))
Q3LIGHT_SRC = $(wildcard $(Q3LIGHT_DIR)/*.c)
LIGHT_SRC = $(wildcard $(LIGHT_DIR)/*.c)
PAK_SRC = $(wildcard $(PAK_DIR)/*.cpp)
HACD_SRC = $(wildcard $(HACD_DIR)/*.cpp)

ML_LITE_CORE_SRC = \
	libs/MeshLib-Lite/MRMesh/MRAABBTree.cpp \
	libs/MeshLib-Lite/MRMesh/MRAABBTreeObjects.cpp \
	libs/MeshLib-Lite/MRMesh/MRAABBTreePoints.cpp \
	libs/MeshLib-Lite/MRMesh/MRAABBTreePolyline.cpp \
	libs/MeshLib-Lite/MRMesh/MRAABBTreePolyline2.cpp \
	libs/MeshLib-Lite/MRMesh/MRAABBTreePolyline3.cpp \
	libs/MeshLib-Lite/MRMesh/MRAffineXf3.cpp \
	libs/MeshLib-Lite/MRMesh/MRBestFit.cpp \
	libs/MeshLib-Lite/MRMesh/MRBitSet.cpp \
	libs/MeshLib-Lite/MRMesh/MRBitSetParallelFor.cpp \
	libs/MeshLib-Lite/MRMesh/MRCloseVertices.cpp \
	libs/MeshLib-Lite/MRMesh/MRComputeBoundingBox.cpp \
	libs/MeshLib-Lite/MRMesh/MREdgeLengthMesh.cpp \
	libs/MeshLib-Lite/MRMesh/MREdgeMetric.cpp \
	libs/MeshLib-Lite/MRMesh/MREdgePaths.cpp \
	libs/MeshLib-Lite/MRMesh/MREdgePoint.cpp \
	libs/MeshLib-Lite/MRMesh/MRFillHoleNicely.cpp \
	libs/MeshLib-Lite/MRMesh/MRIOFilters.cpp \
	libs/MeshLib-Lite/MRMesh/MRIOParsing.cpp \
	libs/MeshLib-Lite/MRMesh/MRId.cpp \
	libs/MeshLib-Lite/MRMesh/MRMapEdge.cpp \
	libs/MeshLib-Lite/MRMesh/MRMarkedContour.cpp \
	libs/MeshLib-Lite/MRMesh/MRMesh.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshBuilder.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshDecimate.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshDelete.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshDelone.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshFillHole.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshFixer.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshMath.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshMetrics.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshNormals.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshPatch.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshProject.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshStubs.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshSubdivide.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshSubdivideCallbacks.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshTopology.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshTriPoint.cpp \
	libs/MeshLib-Lite/MRMesh/MROrder.cpp \
	libs/MeshLib-Lite/MRMesh/MRParallelFor.cpp \
	libs/MeshLib-Lite/MRMesh/MRPartMappingAdapters.cpp \
	libs/MeshLib-Lite/MRMesh/MRPositionVertsSmoothly.cpp \
	libs/MeshLib-Lite/MRMesh/MRProgressReadWrite.cpp \
	libs/MeshLib-Lite/MRMesh/MRQuadraticForm.cpp \
	libs/MeshLib-Lite/MRMesh/MRReducePath.cpp \
	libs/MeshLib-Lite/MRMesh/MRRegionBoundary.cpp \
	libs/MeshLib-Lite/MRMesh/MRSaveSettings.cpp \
	libs/MeshLib-Lite/MRMesh/MRSharedThreadSafeOwner.cpp \
	libs/MeshLib-Lite/MRMesh/MRSpdlog.cpp \
	libs/MeshLib-Lite/MRMesh/MRString.cpp \
	libs/MeshLib-Lite/MRMesh/MRTbbThreadMutex.cpp \
	libs/MeshLib-Lite/MRMesh/MRTimer.cpp \
	libs/MeshLib-Lite/MRMesh/MRTwoLineSegmDist.cpp \
	libs/MeshLib-Lite/MRMesh/MRIdentifyVertices.cpp \
	libs/MeshLib-Lite/MRMesh/MRExpandShrink.cpp \
	libs/MeshLib-Lite/MRMesh/MRPointsInBall.cpp \
	libs/MeshLib-Lite/MRMesh/MRMeshIntersect.cpp

ML_C_SRC = $(wildcard libs/MeshLib-Lite/MRMeshC/*.cpp)

ML_LITE_LIB = libs/MeshLib-Lite/libmrmesh_lite.a

# Object files for Q3MAP (BSP/VIS)
Q3MAP_OBJ = $(COMMON_SRC:$(COMMON_DIR)/%.c=$(OBJ_DIR)/common/%.o) $(SHARED_SRC:$(SHARED_DIR)/%.c=$(OBJ_DIR)/shared/%.o) $(Q3MAP_SRC:$(Q3MAP_DIR)/%.c=$(OBJ_DIR)/q3map/%.o) $(PAK_SRC:libs/pak/%.cpp=$(OBJ_DIR)/pak/%.o) $(HACD_SRC:libs/hacd/%.cpp=$(OBJ_DIR)/hacd/%.o)

# Object files for Q3LIGHT
Q3LIGHT_OBJ = $(COMMON_SRC:$(COMMON_DIR)/%.c=$(OBJ_DIR)/common/%.o) $(SHARED_SRC:$(SHARED_DIR)/%.c=$(OBJ_DIR)/shared/%.o) $(Q3LIGHT_SRC:$(Q3LIGHT_DIR)/%.c=$(OBJ_DIR)/q3light/%.o) $(PAK_SRC:libs/pak/%.cpp=$(OBJ_DIR)/pak/%.o)

# Object files for LIGHT
LIGHT_OBJ = $(COMMON_SRC:$(COMMON_DIR)/%.c=$(OBJ_DIR)/common/%.o) $(SHARED_SRC:$(SHARED_DIR)/%.c=$(OBJ_DIR)/shared/%.o) $(LIGHT_SRC:$(LIGHT_DIR)/%.c=$(OBJ_DIR)/light_embree/%.o) $(PAK_SRC:libs/pak/%.cpp=$(OBJ_DIR)/pak/%.o)

# Object files for MeshLib-Lite (persistent)
ML_LITE_OBJ = $(ML_LITE_CORE_SRC:libs/MeshLib-Lite/MRMesh/%.cpp=$(OBJ_LITE_DIR)/MRMesh/%.o) $(ML_C_SRC:libs/MeshLib-Lite/MRMeshC/%.cpp=$(OBJ_LITE_DIR)/MRMeshC/%.o)

Q3MAP_TARGET = q3map.exe
Q3LIGHT_TARGET = q3light.exe
LIGHT_TARGET = light.exe

all: $(Q3MAP_TARGET) $(Q3LIGHT_TARGET) $(LIGHT_TARGET)

$(ML_LITE_LIB): $(ML_LITE_OBJ)
	echo Building persistent MeshLib-Lite library...
	ar rcs $@ $^

$(Q3MAP_TARGET): $(Q3MAP_OBJ) $(ML_LITE_LIB)
	$(CXX) -o $@ $(Q3MAP_OBJ) $(ML_LITE_LIB) $(Q3MAP_LDFLAGS)

$(Q3LIGHT_TARGET): $(Q3LIGHT_OBJ)
	$(CXX) -o $@ $(Q3LIGHT_OBJ) $(Q3LIGHT_LDFLAGS)

$(LIGHT_TARGET): $(LIGHT_OBJ)
	$(CXX) -o $@ $(LIGHT_OBJ) $(LIGHT_LDFLAGS)

# Compile rules
$(OBJ_DIR)/common/%.o: $(COMMON_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/shared/%.o: $(SHARED_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/q3map/%.o: $(Q3MAP_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/q3light/%.o: $(Q3LIGHT_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/light_embree/%.o: $(LIGHT_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/pak/%.o: libs/pak/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/hacd/%.o: libs/hacd/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile rules for persistent MeshLib-Lite
$(OBJ_LITE_DIR)/MRMesh/%.o: libs/MeshLib-Lite/MRMesh/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -std=c++20 -O1 -Wno-sign-compare -c $< -o $@

$(OBJ_LITE_DIR)/MRMeshC/%.o: libs/MeshLib-Lite/MRMeshC/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -std=c++20 -O1 -Wno-sign-compare -c $< -o $@

clean:
	rm -rf $(OBJ_DIR)
	rm -f $(Q3MAP_TARGET) $(Q3LIGHT_TARGET) $(LIGHT_TARGET)

clean-all: clean
	rm -rf $(OBJ_LITE_DIR)
	rm -f $(ML_LITE_LIB)

.PHONY: all clean clean-all
