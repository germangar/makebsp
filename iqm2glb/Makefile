# iqm2glb Makefile

CXX      = g++
CXXFLAGS = -O2 -Wall -std=c++17 -I../libs/assimp/include
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
         glb_loader_assimp.cpp

# Map source files to object files in the obj directory
OBJS   = $(addprefix $(OBJDIR)/, $(SRCS:.cpp=.o))

all: $(OBJDIR) $(TARGET)

# Create the object directory if it doesn't exist
$(OBJDIR):
	mkdir -p $(OBJDIR)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lstdc++

# Rule for compiling .cpp to .o inside OBJDIR
$(OBJDIR)/%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(TARGET)

.PHONY: all clean
