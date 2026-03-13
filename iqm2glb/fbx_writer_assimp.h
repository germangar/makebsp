#pragma once
#include "math_utils.h"
#include "model.h"

#if ENABLE_ASSIMP
bool write_fbx_assimp(const char* path, const Model& model);
#endif
