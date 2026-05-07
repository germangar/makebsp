#ifndef JSON_PARSER_H
#define JSON_PARSER_H

#include "../libs/json.h"
#include "globals.h"

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

#endif // JSON_PARSER_H
