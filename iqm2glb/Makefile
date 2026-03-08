# iqm2glb Makefile (cgltf implementation)

CXX = g++
CXXFLAGS = -O2 -Wall -std=c++17
LDFLAGS = 

TARGET = iqm2glb.exe
SRCS = main.cpp cgltf_impl.cpp cgltf_write_impl.cpp
OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
