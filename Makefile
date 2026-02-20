CC = gcc
CXX = g++
CFLAGS = -O2 -Wall -I. -Icommon -Ilibs -Ilibs/jpeg6 -Ilibs/pak -Iq3map \
         -Ilibs/assimp/include \
         -D_WIN32 -DNDEBUG -D_CONSOLE
CXXFLAGS = $(CFLAGS)
LDFLAGS = -mconsole -lwsock32 -lws2_32 -lopengl32 -lglu32 -lm \
          -Llibs/assimp/lib -lassimp -lzlibstatic -lstdc++

# Directories
COMMON_DIR = common
Q3MAP_DIR = q3map
JPEG_DIR = libs/jpeg6
PAK_DIR = libs/pak
OBJ_DIR = obj

# Source files
COMMON_SRC = $(wildcard $(COMMON_DIR)/*.c)
Q3MAP_SRC = $(filter-out $(Q3MAP_DIR)/nodraw.c $(Q3MAP_DIR)/misc_model_old.c, $(wildcard $(Q3MAP_DIR)/*.c))
JPEG_SRC = $(wildcard $(JPEG_DIR)/*.cpp)
PAK_SRC = $(wildcard $(PAK_DIR)/*.cpp)

# Object files divided into libraries and application
LIB_OBJ = $(JPEG_SRC:$(JPEG_DIR)/%.cpp=$(OBJ_DIR)/jpeg6/%.o) \
          $(PAK_SRC:$(PAK_DIR)/%.cpp=$(OBJ_DIR)/pak/%.o)

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
$(OBJ_DIR)/common/%.o: $(COMMON_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/q3map/%.o: $(Q3MAP_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/jpeg6/%.o: $(JPEG_DIR)/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/pak/%.o: $(PAK_DIR)/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR)
	rm -f $(TARGET)

.PHONY: all clean libs

