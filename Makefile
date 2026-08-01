# =============================================================================
# Modernized BSP Toolchain Makefile
# =============================================================================
#
# QUICK START GUIDE:
#
# WINDOWS (Optimization Focused):
#   - 'make': Fast build. Recompiles only changed project code.
#   - 'make clean': Deletes project objects but PRESERVES heavy libraries 
#      (Assimp and MeshLib) to keep development cycles under 5 seconds.
#   - 'make clean-all': Wipes EVERYTHING. Use this if you need to force a 
#      full re-compile of the internal libraries from source.
#
# LINUX (Safety Focused):
#   - 'make': Standard build. All objects go into a temporary directory.
#   - 'make clean': Wipes everything (including libraries) for a fresh start.
#     The Linux build choice defaults to safety over speed to avoid version
#     conflicts between different Linux distributions.
#
# LIBRARIES (NOT self-compiled):
#   - Intel Embree 4: Uses pre-built binaries (located in libs/embree/prebuilt).
#   - OpenCL: Linked from system drivers on Linux, local loader on Windows.
#
# =============================================================================

CC = gcc
CXX = g++

ifeq ($(OS),Windows_NT)
    EXECUTABLE_EXT = .exe
    CFLAGS = -O2 -Wall -I. -Icommon -Ilibs -Ilibs/pak -Iq3map -Ishared -Ilight_gpu -Ilibs/assimp/include -Ilibs/MeshLib-Lite/eigen -Ilibs/hacd -Ilibs/xatlas -Ilibs/MeshLib-Lite/MRMeshC -Ilibs/MeshLib-Lite -Ilibs/embree/prebuilt/windows/include -Ilibs/opencl/include -DCL_TARGET_OPENCL_VERSION=120 -DMRMESH_STATIC_LIB -DMRMESH_NO_GTEST -D_WIN32 -DNDEBUG -D_CONSOLE -DWITH_3RD_PARTY_LIBS=0 -DSTB_IMAGE_IMPLEMENTATION -fopenmp -Wno-unknown-pragmas -Wno-attributes -Wno-sign-compare -Wno-unused-parameter
    BASE_LDFLAGS = -mconsole -static -lwsock32 -lws2_32 -lm -lstdc++ -fopenmp -Wl,--stack,16777216
    LIGHT_LDFLAGS = $(BASE_LDFLAGS) -Llibs/embree/prebuilt/windows/lib -lembree4 -ltbb12 -Llibs/opencl/lib -lOpenCL -lcfgmgr32 -lruntimeobject -lole32 -lsetupapi
    GENERATE_KERNELS = powershell.exe -NoProfile -ExecutionPolicy Bypass -File stringify_kernels.ps1
else
    EXECUTABLE_EXT = .elf
    CFLAGS = -O2 -Wall -I. -Icommon -Ilibs -Ilibs/pak -Iq3map -Ishared -Ilight_gpu -Ilibs/assimp/include -Ilibs/MeshLib-Lite/eigen -Ilibs/hacd -Ilibs/xatlas -Ilibs/MeshLib-Lite/MRMeshC -Ilibs/MeshLib-Lite -Ilibs/embree/prebuilt/linux/include -Ilibs/opencl/include -DCL_TARGET_OPENCL_VERSION=120 -DMRMESH_STATIC_LIB -DMRMESH_NO_GTEST -DNDEBUG -DWITH_3RD_PARTY_LIBS=0 -DSTB_IMAGE_IMPLEMENTATION -fopenmp -Wno-unknown-pragmas -Wno-attributes -Wno-sign-compare -Wno-unused-parameter
    BASE_LDFLAGS = -lpthread -ldl -lm -lstdc++ -fopenmp
    LIGHT_LDFLAGS = $(BASE_LDFLAGS) -Llibs/embree/prebuilt/linux/lib -lembree4 -ltbb12 -lOpenCL
    GENERATE_KERNELS = chmod +x stringify_kernels.sh && ./stringify_kernels.sh
endif

ifeq ($(RELEASE), 1)
    CFLAGS += -DRELEASE_BUILD
endif
CXXFLAGS = $(CFLAGS) -Ilibs/MeshLib-Lite -Ilibs/MeshLib-Lite/MRMesh -Ilibs/MeshLib-Lite/MRPch -Ilibs/MeshLib-Lite/tbb -Ilibs/MeshLib-Lite/parallel_hashmap -Wno-class-memaccess
Q3MAP_LDFLAGS = $(BASE_LDFLAGS) -lz

ifeq ($(COACD_ENABLED), 1)
    CFLAGS += -DCOACD_ENABLED -Ilibs/coacd/public
    Q3MAP_LDFLAGS += -Llibs/coacd/build -lcoacd
endif
Q3LIGHT_LDFLAGS = $(BASE_LDFLAGS)

# Directories
COMMON_DIR = common
Q3MAP_DIR = q3map
SHARED_DIR = shared
Q3LIGHT_DIR = light
LIGHT_DIR = light_embree
PAK_DIR = libs/pak
HACD_DIR = libs/hacd
XATLAS_DIR = libs/xatlas
OBJ_DIR = obj
OBJ_LITE_DIR = obj_lite

ifeq ($(OS),Windows_NT)
    OBJ_ASSIMP_DIR = $(OBJ_LITE_DIR)/assimp
    OBJ_ML_DIR = $(OBJ_LITE_DIR)
else
    OBJ_ASSIMP_DIR = $(OBJ_DIR)/assimp
    OBJ_ML_DIR = $(OBJ_DIR)
endif

# Source files
COMMON_SRC = $(wildcard $(COMMON_DIR)/*.c)
SHARED_SRC = $(wildcard $(SHARED_DIR)/*.c)
Q3MAP_SRC = $(wildcard $(Q3MAP_DIR)/*.c)
Q3LIGHT_SRC = $(wildcard $(Q3LIGHT_DIR)/*.c)
LIGHT_SRC = $(wildcard $(LIGHT_DIR)/*.c)
PAK_SRC = $(wildcard $(PAK_DIR)/*.cpp)
HACD_SRC = $(wildcard $(HACD_DIR)/*.cpp)
XATLAS_SRC = $(wildcard $(XATLAS_DIR)/*.cpp)

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

ASSIMP_COMMON_SRC = $(filter-out libs/assimp/src/code/Common/ZipArchiveIOSystem.cpp, $(wildcard libs/assimp/src/code/Common/*.cpp))
ASSIMP_SRC = $(ASSIMP_COMMON_SRC) \
             $(wildcard libs/assimp/src/code/PostProcessing/*.cpp) \
             $(wildcard libs/assimp/src/code/Material/*.cpp) \
             $(wildcard libs/assimp/src/code/CApi/*.cpp) \
             $(wildcard libs/assimp/src/code/Geometry/*.cpp) \
             $(wildcard libs/assimp/src/code/AssetLib/Obj/*.cpp) \
             $(wildcard libs/assimp/src/code/AssetLib/FBX/*.cpp) \
             $(wildcard libs/assimp/src/code/AssetLib/glTF2/*.cpp) \
             $(wildcard libs/assimp/src/code/AssetLib/glTF/*.cpp) \
             $(wildcard libs/assimp/src/code/AssetLib/ASE/*.cpp) \
             $(wildcard libs/assimp/src/code/AssetLib/MD3/*.cpp) \
             $(wildcard libs/assimp/src/code/AssetLib/LWO/*.cpp) \
             $(wildcard libs/assimp/src/code/AssetLib/IQM/*.cpp) \
             $(wildcard libs/assimp/src/code/AssetLib/MD5/*.cpp)

ASSIMP_CONTRIB_SRC = libs/assimp/src/contrib/pugixml/src/pugixml.cpp

ASSIMP_OBJ = $(ASSIMP_SRC:libs/assimp/src/code/%.cpp=$(OBJ_ASSIMP_DIR)/%.o) \
             $(ASSIMP_CONTRIB_SRC:libs/assimp/src/contrib/%.cpp=$(OBJ_ASSIMP_DIR)/contrib/%.o)

ASSIMP_LIB = libs/assimp/libassimp_lite.a

# Object files for Q3MAP (BSP/VIS)
Q3MAP_OBJ = $(COMMON_SRC:$(COMMON_DIR)/%.c=$(OBJ_DIR)/common/%.o) $(SHARED_SRC:$(SHARED_DIR)/%.c=$(OBJ_DIR)/shared/q3map_%.o) $(Q3MAP_SRC:$(Q3MAP_DIR)/%.c=$(OBJ_DIR)/q3map/%.o) $(PAK_SRC:libs/pak/%.cpp=$(OBJ_DIR)/pak/%.o) $(HACD_SRC:libs/hacd/%.cpp=$(OBJ_DIR)/hacd/%.o) $(XATLAS_SRC:$(XATLAS_DIR)/%.cpp=$(OBJ_DIR)/xatlas/%.o)

# Object files for Q3LIGHT
Q3LIGHT_OBJ = $(COMMON_SRC:$(COMMON_DIR)/%.c=$(OBJ_DIR)/common/%.o) $(SHARED_SRC:$(SHARED_DIR)/%.c=$(OBJ_DIR)/shared/light_%.o) $(Q3LIGHT_SRC:$(Q3LIGHT_DIR)/%.c=$(OBJ_DIR)/q3light/%.o) $(PAK_SRC:libs/pak/%.cpp=$(OBJ_DIR)/pak/%.o)

# Object files for LIGHT
LIGHT_OBJ = $(COMMON_SRC:$(COMMON_DIR)/%.c=$(OBJ_DIR)/common/%.o) $(SHARED_SRC:$(SHARED_DIR)/%.c=$(OBJ_DIR)/shared/light_%.o) $(Q3LIGHT_SRC:light/%.c=$(OBJ_DIR)/light/%.o) $(PAK_SRC:libs/pak/%.cpp=$(OBJ_DIR)/pak/%.o)

# Object files for MeshLib-Lite (persistent)
ML_LITE_OBJ = $(ML_LITE_CORE_SRC:libs/MeshLib-Lite/MRMesh/%.cpp=$(OBJ_ML_DIR)/MRMesh/%.o) $(ML_C_SRC:libs/MeshLib-Lite/MRMeshC/%.cpp=$(OBJ_ML_DIR)/MRMeshC/%.o)

Q3MAP_TARGET = makebsp$(EXECUTABLE_EXT)
Q3LIGHT_TARGET = q3light$(EXECUTABLE_EXT)
LIGHT_TARGET = makelight$(EXECUTABLE_EXT)
KERNELS_HEADER = light/kernels_embedded.h

all: $(KERNELS_HEADER) $(Q3MAP_TARGET) $(LIGHT_TARGET)

$(KERNELS_HEADER): makebsp/kernels/*.cl stringify_kernels.ps1
	$(GENERATE_KERNELS)

$(ML_LITE_LIB): $(ML_LITE_OBJ)
	echo Building persistent MeshLib-Lite library...
	ar rcs $@ $^

$(ASSIMP_LIB): $(ASSIMP_OBJ)
	echo Building Assimp-Lite library...
	ar rcs $@ $^

$(Q3MAP_TARGET): $(Q3MAP_OBJ) $(ML_LITE_LIB) $(ASSIMP_LIB)
	$(CXX) -o $@ $(Q3MAP_OBJ) $(ML_LITE_LIB) $(ASSIMP_LIB) $(Q3MAP_LDFLAGS)

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
	$(CC) $(CFLAGS) -DQ3MAP_TOOL -c $< -o $@

$(OBJ_DIR)/light/%.o: $(Q3LIGHT_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DLIGHT_TOOL -c $< -o $@

$(OBJ_DIR)/shared/q3map_%.o: $(SHARED_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DQ3MAP_TOOL -c $< -o $@

$(OBJ_DIR)/shared/light_%.o: $(SHARED_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DLIGHT_TOOL -c $< -o $@

$(OBJ_DIR)/pak/%.o: libs/pak/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/xatlas/%.o: $(XATLAS_DIR)/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/hacd/%.o: $(HACD_DIR)/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile rules for Assimp-Lite
ASSIMP_CXXFLAGS = $(filter-out -DSTB_IMAGE_IMPLEMENTATION, $(CXXFLAGS)) -Ilibs/assimp/include -Ilibs/assimp/src/code -Ilibs/assimp/src/contrib -Ilibs/assimp/src/contrib/pugixml/src -Ilibs/assimp/src/contrib/rapidjson/include -Ilibs/assimp/src/contrib/utf8cpp/source -Ilibs/assimp/src/contrib/stb -DASSIMP_BUILD_NO_OWN_ZLIB=1 -DASSIMP_BUILD_NO_EXPORT=1 -DASSIMP_BUILD_NO_X3D_IMPORTER=1 -DASSIMP_BUILD_NO_M3D_IMPORTER=1 -DASSIMP_BUILD_NO_3DS_IMPORTER=1 -DASSIMP_BUILD_NO_DRACO=1

$(OBJ_ASSIMP_DIR)/%.o: libs/assimp/src/code/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(ASSIMP_CXXFLAGS) -c $< -o $@

$(OBJ_ASSIMP_DIR)/contrib/%.o: libs/assimp/src/contrib/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(ASSIMP_CXXFLAGS) -c $< -o $@

# Compile rules for persistent MeshLib-Lite
$(OBJ_LITE_DIR)/MRMesh/%.o: libs/MeshLib-Lite/MRMesh/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -std=c++20 -O1 -Wno-sign-compare -c $< -o $@

$(OBJ_LITE_DIR)/MRMeshC/%.o: libs/MeshLib-Lite/MRMeshC/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -std=c++20 -O1 -Wno-sign-compare -c $< -o $@

clean:
	rm -rf $(OBJ_DIR)
	rm -f $(Q3MAP_TARGET) $(Q3LIGHT_TARGET) $(LIGHT_TARGET) $(KERNELS_HEADER)

clean-all: clean
	rm -rf $(OBJ_LITE_DIR)
	rm -f $(ML_LITE_LIB) $(ASSIMP_LIB)

.PHONY: all clean clean-all
