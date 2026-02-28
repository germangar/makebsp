CC = gcc
CXX = g++
CFLAGS = -O2 -Wall -I. -Icommon -Ilibs -Ilibs/jpeg6 -Ilibs/pak -Iq3map \
         -Ilibs/assimp/include -Ilibs/coacd/public -Ilibs/meshoptimizer/src -Ilibs/pmp-library/src -Ilibs/pmp-library/external/eigen-3.4.0 \
         -Ilibs/hacd -Ilibs/MeshLib/source/MRMeshC \
         -D_WIN32 -DNDEBUG -D_CONSOLE -DWITH_3RD_PARTY_LIBS=0
CXXFLAGS = $(CFLAGS)
LDFLAGS = -mconsole -lwsock32 -lws2_32 -lopengl32 -lglu32 -lm \
          -Llibs/assimp/lib -lassimp -Llibs/coacd/build -lcoacd -lzlibstatic -lstdc++ -fopenmp \
          -Llibs/MeshLib/build/bin -lMRMeshC -lMRMesh

# Directories
COMMON_DIR = common
Q3MAP_DIR = q3map
JPEG_DIR = libs/jpeg6
PAK_DIR = libs/pak
MESHOPT_DIR = libs/meshoptimizer/src
HACD_DIR = libs/hacd
OBJ_DIR = obj

# Source files
COMMON_SRC = $(wildcard $(COMMON_DIR)/*.c)
Q3MAP_SRC = $(filter-out $(Q3MAP_DIR)/nodraw.c $(Q3MAP_DIR)/misc_model_old.c, $(wildcard $(Q3MAP_DIR)/*.c))
JPEG_SRC = $(wildcard $(JPEG_DIR)/*.cpp)
PAK_SRC = $(wildcard $(PAK_DIR)/*.cpp)
MESHOPT_SRC = $(wildcard $(MESHOPT_DIR)/*.cpp)
HACD_SRC = $(wildcard $(HACD_DIR)/*.cpp)

# Object files divided into libraries and application
LIB_OBJ = $(JPEG_SRC:$(JPEG_DIR)/%.cpp=$(OBJ_DIR)/jpeg6/%.o) \
          $(PAK_SRC:$(PAK_DIR)/%.cpp=$(OBJ_DIR)/pak/%.o) \
          $(MESHOPT_SRC:$(MESHOPT_DIR)/%.cpp=$(OBJ_DIR)/meshoptimizer/%.o) \
          $(HACD_SRC:$(HACD_DIR)/%.cpp=$(OBJ_DIR)/hacd/%.o)

APP_OBJ = $(COMMON_SRC:$(COMMON_DIR)/%.c=$(OBJ_DIR)/common/%.o) \
          $(Q3MAP_SRC:$(Q3MAP_DIR)/%.c=$(OBJ_DIR)/q3map/%.o)

TARGET = q3map.exe

# By default, we only depend on application objects
# Use REBUILD_LIBS=1 to force checking libraries
ALL_OBJ = $(LIB_OBJ) $(APP_OBJ)

all: $(TARGET)

# Define a specific target to build libraries
libs: $(LIB_OBJ)

$(TARGET): $(ALL_OBJ)
	$(CXX) -o $@ $(LIB_OBJ) $(APP_OBJ) $(LDFLAGS)

# Compile rules
$(OBJ_DIR)/common/%.o: $(COMMON_DIR)/%.c q3map/qbsp.h
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/q3map/%.o: $(Q3MAP_DIR)/%.c q3map/qbsp.h
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@


$(OBJ_DIR)/jpeg6/%.o: $(JPEG_DIR)/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/pak/%.o: $(PAK_DIR)/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/meshoptimizer/%.o: $(MESHOPT_DIR)/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/hacd/%.o: $(HACD_DIR)/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/pmp/%.o: $(PMP_DIR)/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -std=c++20 -c $< -o $@


clean:
	rm -rf $(OBJ_DIR)
	rm -f $(TARGET)

.PHONY: all clean libs

