# iqm2glb Makefile

CXX      = g++
CXXFLAGS = -O2 -Wall -std=c++17 -Ilibs
CFLAGS   = -O2 -Wall -Ilibs
LDFLAGS  =

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
         skp_loader.cpp \
         fbx_writer.cpp \
         fbx_loader.cpp

OBJS   = $(addprefix $(OBJDIR)/, $(SRCS:.cpp=.o)) \
         $(addprefix $(OBJDIR)/, fbx.o) \
         $(addprefix $(OBJDIR)/, ufbx.o) \
         $(addprefix $(OBJDIR)/, miniz.o)

all: $(OBJDIR) $(TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lstdc++

$(OBJDIR)/%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/fbx.o: libs/fbx.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/ufbx.o: libs/ufbx.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/miniz.o: libs/miniz.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/cgltf_impl.o: libs/cgltf_impl.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/cgltf_write_impl.o: libs/cgltf_write_impl.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(TARGET)

.PHONY: all clean
