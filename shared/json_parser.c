#include "json_parser.h"
#include "../common/cmdlib.h"
#include "../common/mathlib.h"
#include "../common/qfiles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_IMPLEMENTATION
#include "../libs/json.h"

#define VALIDATE_LIMIT(var, max_val, name) \
  if (var > max_val) {                     \
    _printf("WARNING: %s (%d) exceeds internal limit (%d). Clamping.\n", name, var, max_val); \
    var = max_val;                         \
  }

struct json_value_s *JSON_ReadFile(const char *filename) {
  void *buffer = NULL;
  int len;
  struct json_value_s *root;

  len = LoadFile(filename, &buffer);
  if (len <= 0) {
    return NULL;
  }

  root = json_parse(buffer, (size_t)len);
  free(buffer);

  return root;
}

void JSON_Free(struct json_value_s *value) {
  if (value) {
    free(value);
  }
}

qboolean JSON_LoadGame(const char *filename, game_t *game) {
  struct json_value_s *root = JSON_ReadFile(filename);
  if (!root)
    return qfalse;

  struct json_object_s *obj = json_value_as_object(root);
  if (!obj) {
    JSON_Free(root);
    return qfalse;
  }

  struct json_object_element_s *el = obj->start;
  while (el) {
    const char *key = el->name->string;
    struct json_value_s *val = el->value;

    if (!strcmp(key, "arg") && val->type == json_type_string) {
      game->arg = copystring(json_value_as_string(val)->string);
    } else if (!strcmp(key, "gamePath") && val->type == json_type_string) {
      game->gamePath = copystring(json_value_as_string(val)->string);
    } else if (!strcmp(key, "bspIdent") && val->type == json_type_string) {
      game->bspIdent = copystring(json_value_as_string(val)->string);
    } else if (!strcmp(key, "bspVersion") && val->type == json_type_number) {
      game->bspVersion = atoi(json_value_as_number(val)->number);
    } else if (!strcmp(key, "lumpCount") && val->type == json_type_number) {
      game->lumpCount = atoi(json_value_as_number(val)->number);
    } else if (!strcmp(key, "maxLMSurfaceVerts") && val->type == json_type_number) {
      game->maxLMSurfaceVerts = atoi(json_value_as_number(val)->number);
    } else if (!strcmp(key, "maxSurfaceVerts") && val->type == json_type_number) {
      game->maxSurfaceVerts = atoi(json_value_as_number(val)->number);
    } else if (!strcmp(key, "maxSurfaceIndexes") && val->type == json_type_number) {
      game->maxSurfaceIndexes = atoi(json_value_as_number(val)->number);
    } else if (!strcmp(key, "lightmapSize") && val->type == json_type_number) {
      game->lightmapSize = atoi(json_value_as_number(val)->number);
    } else if (!strcmp(key, "defaultSampleSize") && val->type == json_type_number) {
      game->defaultSampleSize = atoi(json_value_as_number(val)->number);
    } else if (!strcmp(key, "lightmapsRGB")) {
      if (val->type == json_type_true) game->lightmapsRGB = qtrue;
      else if (val->type == json_type_false) game->lightmapsRGB = qfalse;
    } else if (!strcmp(key, "texturesRGB")) {
      if (val->type == json_type_true) game->texturesRGB = qtrue;
      else if (val->type == json_type_false) game->texturesRGB = qfalse;
    } else if (!strcmp(key, "colorsRGB")) {
      if (val->type == json_type_true) game->colorsRGB = qtrue;
      else if (val->type == json_type_false) game->colorsRGB = qfalse;
    } else if (!strcmp(key, "deluxeMap")) {
      if (val->type == json_type_true) game->deluxeMap = qtrue;
      else if (val->type == json_type_false) game->deluxeMap = qfalse;
    } else if (!strcmp(key, "falloff") && val->type == json_type_string) {
      const char *f = json_value_as_string(val)->string;
      if (!strcmp(f, "halflambert"))
        game->falloff = FALLOFF_HALFLAMBERT;
      else if (!strcmp(f, "quadratic"))
        game->falloff = FALLOFF_QUADRATIC;
      else if (!strcmp(f, "doublequadratic"))
        game->falloff = FALLOFF_DOUBLEQUADRATIC;
      else if (!strcmp(f, "unreal"))
        game->falloff = FALLOFF_UNREAL;
      else if (!strcmp(f, "wrapped"))
        game->falloff = FALLOFF_WRAPPED;
      else
        game->falloff = FALLOFF_LAMBERT;
    }

    el = el->next;
  }


  // Lightmap size validation: must be power of 2, max 4096 (reasonable limit for modern GPUs)
  if (game->lightmapSize > 4096) {
    _printf("WARNING: lightmapSize (%d) exceeds limit (4096). Clamping.\n", game->lightmapSize);
    game->lightmapSize = 4096;
  }
  if (game->lightmapSize < 1) {
    game->lightmapSize = 128;
  }

  JSON_Free(root);
  return qtrue;
}

static const char *g_loadDir;
static int g_loadCount;

static void JSON_LoadGameCallback(const char *filename) {
  if (numGames >= MAX_GAMES)
    return;

  char fullpath[1024];
  sprintf(fullpath, "%s/%s", g_loadDir, filename);

  // Use 'qfusion' as the template for all loaded JSON definitions
  int j, templateIdx = 0;
  for (j = 0; j < numGames; j++) {
    if (!strcmp(games[j].arg, "qfusion")) {
      templateIdx = j;
      break;
    }
  }

  // Initialize with the qfusion template before loading JSON overrides
  memcpy(&games[numGames], &games[templateIdx], sizeof(game_t));

  if (JSON_LoadGame(fullpath, &games[numGames])) {
    numGames++;
    g_loadCount++;
  }
}

int JSON_LoadPackages(const char *directory) {
  g_loadDir = directory;
  g_loadCount = 0;
  Sys_ListFiles(directory, "*.json", JSON_LoadGameCallback);
  return g_loadCount;
}
