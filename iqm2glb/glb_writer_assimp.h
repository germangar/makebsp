#pragma once
#include "model.h"

/**
 * Writes a Model to a GLB file using the Assimp library.
 * This is a secondary implementation for comparison and debugging.
 */
bool write_glb_assimp(const Model& model, const char* filename);
