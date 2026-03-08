#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>

struct GLBHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t length;
};

struct ChunkHeader {
    uint32_t length;
    uint32_t type;
};

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) return 1;

    GLBHeader header;
    f.read((char*)&header, sizeof(header));

    ChunkHeader jsonChunk;
    f.read((char*)&jsonChunk, sizeof(jsonChunk));

    std::vector<char> jsonData(jsonChunk.length);
    f.read(jsonData.data(), jsonChunk.length);

    std::cout.write(jsonData.data(), jsonData.size());
    std::cout << std::endl;

    return 0;
}
