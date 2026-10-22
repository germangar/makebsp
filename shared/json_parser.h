#ifndef JSON_PARSER_H
#define JSON_PARSER_H

#include "../libs/json.h"
#include "globals.h"
#include "../common/bspfile.h"

// Loads and parses a JSON file. Returns NULL on failure.
// The returned value must be freed with JSON_Free.
struct json_value_s *JSON_ReadFile(const char *filename);

// Frees the memory allocated for a JSON value.
void JSON_Free(struct json_value_s *value);

// Game definition loading
qboolean JSON_LoadGame(const char *filename, game_t *game);


// Game definition exporting
void JSON_ExportGame(const char *filename, game_t *game);
void JSON_ExportStandardPackages(const char *directory);

// Canonical enum -> name mappings shared by the JSON exporter and the
// _makebsp_profile worldspawn key
const char *ShadingModelName(shadingModel_t model);
const char *HdrFormatName(hdrFormat_t hdr);
const char *AttenuationModelName(attenuationModel_t model);

// _makebsp_profile worldspawn key: single-line compilation configuration
// ("-field value" tokens, booleans as 1|0, floats with 2 decimals)
void InjectMakebspProfileKey(entity_t *worldspawn, game_t *game);

// Applies the compile-time hardlocked fields (lightmapSize, sampleSize,
// hdr, hdr8BitScale); all other fields are informational. Returns qfalse
// when the value is empty or the mandatory -lightmapSize token is missing
// (BSP compiled by an older makebsp).
qboolean ApplyMakebspProfileHardlocks(const char *profileValue, game_t *game);

#endif // JSON_PARSER_H
