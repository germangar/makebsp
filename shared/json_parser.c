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

  // Smart template selection: Peek at 'bspIdent' to decide between Quake 3 or Qfusion baseline.
  // Defaults to games[0] (quake3) unless "FBSP" is explicitly found.
  int templateIdx = 0; // quake3
  struct json_object_element_s *peek_el = obj->start;
  while (peek_el) {
    if (!strcmp(peek_el->name->string, "bspIdent") && peek_el->value->type == json_type_string) {
      const char *ident = json_value_as_string(peek_el->value)->string;
      if (!strcmp(ident, "FBSP")) {
        templateIdx = 1; // qfusion
      }
      break;
    }
    peek_el = peek_el->next;
  }
  
  // Initialize with the selected template baseline
  memcpy(game, &games[templateIdx], sizeof(game_t));

  struct json_object_element_s *el = obj->start;
  while (el) {
    const char *key = el->name->string;
    struct json_value_s *val = el->value;

    if (!strcmp(key, "game") && val->type == json_type_string) {
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
    } else if (!strcmp(key, "sampleSize") && val->type == json_type_number) {
      game->defaultSampleSize = atoi(json_value_as_number(val)->number);
    } else if (!strcmp(key, "hdr") && val->type == json_type_string) {
      const char *h = json_value_as_string(val)->string;
      if (!strcmp(h, "rgb8"))
        game->hdr = HDR_8BIT;
      else if (!strcmp(h, "rgba16f"))
        game->hdr = HDR_16BIT;
      else if (!strcmp(h, "rgba32f"))
        game->hdr = HDR_32BIT;
      else
        game->hdr = HDR_OFF;
    } else if (!strcmp(key, "lightmapsRGB")) {
      if (val->type == json_type_true) game->lightmapsRGB = qtrue;
      else if (val->type == json_type_false) game->lightmapsRGB = qfalse;
    } else if (!strcmp(key, "lightgridRGB")) {
      if (val->type == json_type_true) game->lightgridRGB = qtrue;
      else if (val->type == json_type_false) game->lightgridRGB = qfalse;
    } else if (!strcmp(key, "texturesRGB")) {
      if (val->type == json_type_true) game->texturesRGB = qtrue;
      else if (val->type == json_type_false) game->texturesRGB = qfalse;
    } else if (!strcmp(key, "colorsRGB")) {
      if (val->type == json_type_true) game->colorsRGB = qtrue;
      else if (val->type == json_type_false) game->colorsRGB = qfalse;
    } else if (!strcmp(key, "radiosityPasses") && val->type == json_type_number) {
      game->radiosityPasses = atoi(json_value_as_number(val)->number);
    } else if (!strcmp(key, "radiosityIntensity") && val->type == json_type_number) {
      game->radiosityIntensity = (float)atof(json_value_as_number(val)->number);
    } else if (!strcmp(key, "radiosityColorRatio") && val->type == json_type_number) {
      game->radiosityColorRatio = (float)atof(json_value_as_number(val)->number);
    } else if (!strcmp(key, "deluxeMap")) {
      if (val->type == json_type_true) game->deluxeMap = qtrue;
      else if (val->type == json_type_false) game->deluxeMap = qfalse;
    } else if (!strcmp(key, "smoothPasses") && val->type == json_type_number) {
      game->defaultSmoothPasses = atoi(json_value_as_number(val)->number);
    } else if (!strcmp(key, "smoothRadius") && val->type == json_type_number) {
      game->defaultSmoothRadius = (float)atof(json_value_as_number(val)->number);
      if (game->defaultSmoothRadius < 0.1f) game->defaultSmoothRadius = 0.1f;
    } else if (!strcmp(key, "antialiasingPasses") && val->type == json_type_number) {
      game->antialiasingPasses = atoi(json_value_as_number(val)->number);
    } else if (!strcmp(key, "forceUVGen")) {
      if (val->type == json_type_true) game->forceUVGen = qtrue;
      else if (val->type == json_type_false) game->forceUVGen = qfalse;
    } else if (!strcmp(key, "snapUVs")) {
      if (val->type == json_type_true) game->snapUVs = qtrue;
      else if (val->type == json_type_false) game->snapUVs = qfalse;
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
    } else if (!strcmp(key, "exposurefilter") && val->type == json_type_string) {
      const char *ef = json_value_as_string(val)->string;
      if (!strcmp(ef, "softknee"))
        game->exposureFilter = TONEMAP_SOFTKNEE;
      else if (!strcmp(ef, "reinhard"))
        game->exposureFilter = TONEMAP_REINHARD;
      else if (!strcmp(ef, "filmic"))
        game->exposureFilter = TONEMAP_FILMIC;
      else
        game->exposureFilter = TONEMAP_LINEAR;
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

  // Template selection is now handled internally by JSON_LoadGame
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

void JSON_ExportGame(const char *filename, game_t *game) {
  char buffer[4096];
  const char *falloffStr;
  switch (game->falloff) {
  case FALLOFF_LAMBERT:
    falloffStr = "lambert";
    break;
  case FALLOFF_HALFLAMBERT:
    falloffStr = "halflambert";
    break;
  case FALLOFF_QUADRATIC:
    falloffStr = "quadratic";
    break;
  case FALLOFF_DOUBLEQUADRATIC:
    falloffStr = "doublequadratic";
    break;
  case FALLOFF_UNREAL:
    falloffStr = "unreal";
    break;
  case FALLOFF_WRAPPED:
    falloffStr = "wrapped";
    break;
  default:
    falloffStr = "unknown";
    break;
  }

  const char *hdrStr;
  switch (game->hdr) {
  case HDR_8BIT:
    hdrStr = "rgb8";
    break;
  case HDR_16BIT:
    hdrStr = "rgba16f";
    break;
  case HDR_32BIT:
    hdrStr = "rgba32f";
    break;
  default:
    hdrStr = "off";
    break;
  }

  const char *filterStr;
  switch (game->exposureFilter) {
  case TONEMAP_SOFTKNEE:
    filterStr = "softknee";
    break;
  case TONEMAP_REINHARD:
    filterStr = "reinhard";
    break;
  case TONEMAP_FILMIC:
    filterStr = "filmic";
    break;
  default:
    filterStr = "off";
    break;
  }

  sprintf(buffer,
          "{\n"
          "  \"game\": \"%s\",\n"
          "  \"gamePath\": \"%s\",\n"
          "  \"bspIdent\": \"%s\",\n"
          "  \"bspVersion\": %d,\n"
          "  \"lumpCount\": %d,\n"
          "  \"maxLMSurfaceVerts\": %d,\n"
          "  \"maxSurfaceVerts\": %d,\n"
          "  \"maxSurfaceIndexes\": %d,\n"
          "  \"lightmapSize\": %d,\n"
          "  \"sampleSize\": %d,\n"
          "  \"hdr\": \"%s\", /* [ off, rgb8, rgb16, rgb32 ] More than 8 bit requires a bsp version change */\n"
          "  \"lightmapsRGB\": %s,\n"
          "  \"lightgridRGB\": %s,\n"
          "  \"texturesRGB\": %s,\n"
          "  \"colorsRGB\": %s,\n"
          "  \"radiosityPasses\": %d,\n"
          "  \"radiosityIntensity\": %.2f,\n"
          "  \"radiosityColorRatio\": %.2f,\n"
          "  \"falloff\": \"%s\",  /* [ lambert, halflambert, quadratic, doublequadratic, unreal, wrapped ] */\n"
          "  \"deluxeMap\": %s,\n"
          "  \"forceUVGen\": %s,\n"
          "  \"snapUVs\": %s,\n"
          "  \"antialiasingPasses\": %d, /* post-process AA passes */\n"
          "  \"smoothPasses\": %d, /* passes of blurring lightmaps */\n"
          "  \"smoothRadius\": %.2f, /* fractional values accepted. Minimum 0.1 */\n"
          "  \"exposurefilter\": \"%s\" /* [ off, softknee, reinhard, filmic ] */\n"
          "}\n",
          game->arg, game->gamePath, game->bspIdent, game->bspVersion,
          game->lumpCount, game->maxLMSurfaceVerts, game->maxSurfaceVerts,
          game->maxSurfaceIndexes, game->lightmapSize,
          game->defaultSampleSize, hdrStr, game->lightmapsRGB ? "true" : "false",
          game->lightgridRGB ? "true" : "false",
          game->texturesRGB ? "true" : "false",
          game->colorsRGB ? "true" : "false",
          game->radiosityPasses,
          game->radiosityIntensity,
          game->radiosityColorRatio,
          falloffStr,
          game->deluxeMap ? "true" : "false",
          game->forceUVGen ? "true" : "false",
          game->snapUVs ? "true" : "false",
          game->antialiasingPasses,
          game->defaultSmoothPasses, game->defaultSmoothRadius, filterStr);
  SaveFile(filename, buffer, strlen(buffer));
}

void JSON_ExportStandardPackages(const char *directory) {
  char path[1024];
  Q_mkdir(directory);

  sprintf(path, "%s/quake3.json", directory);
  if (!FileExists(path)) {
    JSON_ExportGame(path, &games[0]);
    _printf("Exporting default 'quake3.json'...\n");
  }

  sprintf(path, "%s/qfusion.json", directory);
  if (!FileExists(path)) {
    JSON_ExportGame(path, &games[1]);
    _printf("Exporting default 'qfusion.json'...\n");
  }
}
