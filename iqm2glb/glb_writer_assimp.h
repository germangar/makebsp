#pragma once
#include "math_utils.h"
#include "model.h"

#if ENABLE_ASSIMP
/**
 * Writes a Model to a GLB file using the Assimp library.
 * This is a secondary implementation for comparison and debugging.
 */
bool write_glb_assimp(const Model& model, const char* filename);
#endif
