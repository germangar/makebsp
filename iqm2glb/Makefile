# iqm2glb Makefile

CXX      = g++
CXXFLAGS = -O2 -Wall -std=c++17 -I../libs/assimp/include -I../libs/fbx-file
CFLAGS   = -O2 -Wall -I../libs/fbx-file
LDFLAGS  = -L../libs/assimp/lib -lassimp -lzlibstatic

TARGET = iqm2glb.exe
OBJDIR = obj

SRCS   = main.cpp \
         iqm_loader.cpp \
         anim_cfg.cpp \
         glb_writer.cpp \
         glb_loader.cpp \
         iqm_writer.cpp \
         cgltf_impl.cpp \
         cgltf_write_impl.cpp \
         glb_writer_assimp.cpp \
         glb_loader_assimp.cpp \
         skp_loader.cpp \
         fbx_writer.cpp \
         fbx_loader.cpp

LIB_SRCS_CPP = ../libs/fbx-file/fbx.cpp

OBJS   = $(addprefix $(OBJDIR)/, $(SRCS:.cpp=.o)) \
         $(addprefix $(OBJDIR)/, fbx.o) \
         $(addprefix $(OBJDIR)/, ufbx.o)

all: $(OBJDIR) $(TARGET)

# Create the object directory if it doesn't exist
$(OBJDIR):
	mkdir -p $(OBJDIR)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lstdc++

# Rule for compiling .cpp to .o inside OBJDIR
$(OBJDIR)/%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/fbx.o: ../libs/fbx-file/fbx.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/ufbx.o: ../ufbx.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(TARGET)

.PHONY: all clean
