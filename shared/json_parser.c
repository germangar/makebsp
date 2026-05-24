#include "json_parser.h"
#include "../common/cmdlib.h"
#include "../common/mathlib.h"
#include "../common/qfiles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_IMPLEMENTATION
#include "../libs/json.h"

#define VALIDATE_LIMIT(var, max_val, name)                                                        \
    if (var > max_val)                                                                            \
    {                                                                                             \
        _printf("WARNING: %s (%d) exceeds internal limit (%d). Clamping.\n", name, var, max_val); \
        var = max_val;                                                                            \
    }

static void JSON_StripComments(char *buffer, int len)
{
    int i;
    qboolean inString = qfalse;
    qboolean inComment = qfalse;
    qboolean inLineComment = qfalse;

    for (i = 0; i < len; i++)
    {
        if (!inComment && !inLineComment)
        {
            if (buffer[i] == '\"' && (i == 0 || buffer[i - 1] != '\\'))
            {
                inString = !inString;
            }
            if (!inString)
            {
                if (buffer[i] == '/' && i + 1 < len)
                {
                    if (buffer[i + 1] == '*')
                    {
                        inComment = qtrue;
                        buffer[i] = ' ';
                        buffer[i + 1] = ' ';
                        i++;
                    }
                    else if (buffer[i + 1] == '/')
                    {
                        inLineComment = qtrue;
                        buffer[i] = ' ';
                        buffer[i + 1] = ' ';
                        i++;
                    }
                }
            }
        }
        else if (inComment)
        {
            if (buffer[i] == '*' && i + 1 < len && buffer[i + 1] == '/')
            {
                inComment = qfalse;
                buffer[i] = ' ';
                buffer[i + 1] = ' ';
                i++;
            }
            else
            {
                if (buffer[i] != '\n' && buffer[i] != '\r')
                    buffer[i] = ' ';
            }
        }
        else if (inLineComment)
        {
            if (buffer[i] == '\n' || buffer[i] == '\r')
            {
                inLineComment = qfalse;
            }
            else
            {
                buffer[i] = ' ';
            }
        }
    }
}

struct json_value_s *JSON_ReadFile(const char *filename)
{
    void *buffer = NULL;
    int len;
    struct json_value_s *root;

    len = LoadFile(filename, &buffer);
    if (len <= 0)
    {
        return NULL;
    }

    JSON_StripComments((char *)buffer, len);

    root = json_parse(buffer, (size_t)len);
    free(buffer);

    return root;
}

void JSON_Free(struct json_value_s *value)
{
    if (value)
    {
        free(value);
    }
}

qboolean JSON_LoadGame(const char *filename, game_t *game)
{
    struct json_value_s *root = JSON_ReadFile(filename);
    if (!root)
        return qfalse;

    struct json_object_s *obj = json_value_as_object(root);
    if (!obj)
    {
        JSON_Free(root);
        return qfalse;
    }

    struct json_object_element_s *el = obj->start;
    while (el)
    {
        const char *key = el->name->string;
        struct json_value_s *val = el->value;

        if (!strcmp(key, "game") && val->type == json_type_string)
        {
            game->arg = copystring(json_value_as_string(val)->string);
        }
        else if (!strcmp(key, "gamePath") && val->type == json_type_string)
        {
            game->gamePath = copystring(json_value_as_string(val)->string);
        }
        else if (!strcmp(key, "bspIdent") && val->type == json_type_string)
        {
            game->bspIdent = copystring(json_value_as_string(val)->string);
        }
        else if (!strcmp(key, "bspVersion") && val->type == json_type_number)
        {
            game->bspVersion = atoi(json_value_as_number(val)->number);
        }
        else if (!strcmp(key, "lumpCount") && val->type == json_type_number)
        {
            game->lumpCount = atoi(json_value_as_number(val)->number);
        }
        else if (!strcmp(key, "maxLMSurfaceVerts") && val->type == json_type_number)
        {
            game->maxLMSurfaceVerts = atoi(json_value_as_number(val)->number);
        }
        else if (!strcmp(key, "maxSurfaceVerts") && val->type == json_type_number)
        {
            game->maxSurfaceVerts = atoi(json_value_as_number(val)->number);
        }
        else if (!strcmp(key, "maxSurfaceIndexes") && val->type == json_type_number)
        {
            game->maxSurfaceIndexes = atoi(json_value_as_number(val)->number);
        }
        else if (!strcmp(key, "lightmapSize") && val->type == json_type_number)
        {
            int oldSize = game->lightmapSize;
            game->lightmapSize = atoi(json_value_as_number(val)->number);
            if (game->writeLightmapSize == oldSize)
                game->writeLightmapSize = game->lightmapSize;
        }
        else if (!strcmp(key, "writeLightmapSize") && val->type == json_type_number)
        {
            game->writeLightmapSize = atoi(json_value_as_number(val)->number);
        }
        else if (!strcmp(key, "sampleSize") && val->type == json_type_number)
        {
            game->defaultSampleSize = atoi(json_value_as_number(val)->number);
        }
        else if (!strcmp(key, "hdr") && val->type == json_type_string)
        {
            const char *h = json_value_as_string(val)->string;
            if (!strcmp(h, "rgb8"))
                game->hdr = HDR_8BIT;
            else if (!strcmp(h, "rgba16f"))
                game->hdr = HDR_16BIT;
            else if (!strcmp(h, "rgba32f"))
                game->hdr = HDR_32BIT;
            else
                game->hdr = HDR_OFF;
        }
        else if (!strcmp(key, "hdr8BitScale") && val->type == json_type_number)
        {
            game->hdr8BitScale = (float)atof(json_value_as_number(val)->number);
        }
        else if (!strcmp(key, "lightmapsRGB"))
        {
            if (val->type == json_type_true)
                game->lightmapsRGB = qtrue;
            else if (val->type == json_type_false)
                game->lightmapsRGB = qfalse;
        }
        else if (!strcmp(key, "lightgridRGB"))
        {
            if (val->type == json_type_true)
                game->lightgridRGB = qtrue;
            else if (val->type == json_type_false)
                game->lightgridRGB = qfalse;
        }
        else if (!strcmp(key, "texturesRGB"))
        {
            if (val->type == json_type_true)
                game->texturesRGB = qtrue;
            else if (val->type == json_type_false)
                game->texturesRGB = qfalse;
        }
        else if (!strcmp(key, "colorsRGB"))
        {
            if (val->type == json_type_true)
                game->colorsRGB = qtrue;
            else if (val->type == json_type_false)
                game->colorsRGB = qfalse;
        }
        else if (!strcmp(key, "radiosityPasses") && val->type == json_type_number)
        {
            game->radiosityPasses = atoi(json_value_as_number(val)->number);
        }
        else if (!strcmp(key, "radiosityIntensity") && val->type == json_type_number)
        {
            game->radiosityIntensity = (float)atof(json_value_as_number(val)->number);
        }
        else if (!strcmp(key, "radiosityColorRatio") && val->type == json_type_number)
        {
            game->radiosityColorRatio = (float)atof(json_value_as_number(val)->number);
        }
        else if (!strcmp(key, "deluxeMap"))
        {
            if (val->type == json_type_true)
                game->deluxeMap = qtrue;
            else if (val->type == json_type_false)
                game->deluxeMap = qfalse;
        }
        else if (!strcmp(key, "deluxeMinAngle") && val->type == json_type_number)
        {
            game->deluxeMinAngle = (float)atof(json_value_as_number(val)->number);
        }
        else if (!strcmp(key, "deluxeAmbientExaggerate") && val->type == json_type_number)
        {
            game->deluxeAmbientExaggerate = (float)atof(json_value_as_number(val)->number);
        }
        else if (!strcmp(key, "deluxeRadiosityExaggerate") && val->type == json_type_number)
        {
            game->deluxeRadiosityExaggerate = (float)atof(json_value_as_number(val)->number);
        }
        else if (!strcmp(key, "smoothPasses") && val->type == json_type_number)
        {
            game->defaultSmoothPasses = atoi(json_value_as_number(val)->number);
        }
        else if (!strcmp(key, "smoothRadius") && val->type == json_type_number)
        {
            game->defaultSmoothRadius = (float)atof(json_value_as_number(val)->number);
            if (game->defaultSmoothRadius < 0.1f)
                game->defaultSmoothRadius = 0.1f;
        }
        else if (!strcmp(key, "antialiasingPasses") && val->type == json_type_number)
        {
            game->antialiasingPasses = atoi(json_value_as_number(val)->number);
        }
        else if (!strcmp(key, "snapUVs"))
        {
            if (val->type == json_type_true)
                game->snapUVs = qtrue;
            else if (val->type == json_type_false)
                game->snapUVs = qfalse;
        }
        else if (!strcmp(key, "falloff") && val->type == json_type_string)
        {
            const char *f = json_value_as_string(val)->string;
            if (!strcmp(f, "halflambert"))
                game->falloff = FALLOFF_HALFLAMBERT;
            else if (!strcmp(f, "quadratic"))
                game->falloff = FALLOFF_QUADRATIC;
            else if (!strcmp(f, "doublequadratic"))
                game->falloff = FALLOFF_DOUBLEQUADRATIC;
            else if (!strcmp(f, "unreal"))
                game->falloff = FALLOFF_UNREAL;
            else
                game->falloff = FALLOFF_LAMBERT;
        }
        else if (!strcmp(key, "sunFalloff") && val->type == json_type_string)
        {
            const char *f = json_value_as_string(val)->string;
            if (!strcmp(f, "halflambert"))
                game->sunFalloff = FALLOFF_HALFLAMBERT;
            else if (!strcmp(f, "quadratic"))
                game->sunFalloff = FALLOFF_QUADRATIC;
            else if (!strcmp(f, "doublequadratic"))
                game->sunFalloff = FALLOFF_DOUBLEQUADRATIC;
            else if (!strcmp(f, "unreal"))
                game->sunFalloff = FALLOFF_UNREAL;
            else
                game->sunFalloff = FALLOFF_LAMBERT;
        }
        else if (!strcmp(key, "exposurefilter") && val->type == json_type_string)
        {
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
        else if (!strcmp(key, "enforceSampleSize"))
        {
            if (val->type == json_type_true)
                game->enforceSampleSize = qtrue;
            else if (val->type == json_type_false)
                game->enforceSampleSize = qfalse;
        }

        el = el->next;
    }

    // Lightmap size validation: must be power of 2, max 4096 (reasonable limit for modern GPUs)
    if (game->lightmapSize > 4096)
    {
        _printf("WARNING: lightmapSize (%d) exceeds limit (4096). Clamping.\n", game->lightmapSize);
        game->lightmapSize = 4096;
    }
    if (game->lightmapSize < 1)
    {
        game->lightmapSize = 128;
    }
    if (game->writeLightmapSize < 1)
    {
        game->writeLightmapSize = game->lightmapSize;
    }

    JSON_Free(root);
    return qtrue;
}

void JSON_ExportGame(const char *filename, game_t *game)
{
    char buffer[4096];
    const char *falloffStr;
    switch (game->falloff)
    {
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
    default:
        falloffStr = "unknown";
        break;
    }

    const char *sunFalloffStr;
    switch (game->sunFalloff)
    {
    case FALLOFF_LAMBERT:
        sunFalloffStr = "lambert";
        break;
    case FALLOFF_HALFLAMBERT:
        sunFalloffStr = "halflambert";
        break;
    case FALLOFF_QUADRATIC:
        sunFalloffStr = "quadratic";
        break;
    case FALLOFF_DOUBLEQUADRATIC:
        sunFalloffStr = "doublequadratic";
        break;
    case FALLOFF_UNREAL:
        sunFalloffStr = "unreal";
        break;
    default:
        sunFalloffStr = "unknown";
        break;
    }

    const char *hdrStr;
    switch (game->hdr)
    {
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
    switch (game->exposureFilter)
    {
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
            "  \"writeLightmapSize\": %d,\n"
            "  \"sampleSize\": %d,\n"
            "  \"hdr\": \"%s\", /* [ off, rgb8, rgb16, rgb32 ] More than 8 bit requires a bsp version change */\n"
            "  \"hdr8BitScale\": %.2f,\n"
            "  \"lightmapsRGB\": %s,\n"
            "  \"lightgridRGB\": %s,\n"
            "  \"texturesRGB\": %s,\n"
            "  \"colorsRGB\": %s,\n"
            "  \"radiosityPasses\": %d,\n"
            "  \"radiosityIntensity\": %.2f,\n"
            "  \"radiosityColorRatio\": %.2f,\n"
            "  \"falloff\": \"%s\",  /* [ lambert, halflambert, quadratic, doublequadratic, unreal ] */\n"
            "  \"sunFalloff\": \"%s\",  /* [ lambert, halflambert, quadratic, doublequadratic, unreal ] */\n"
            "  \"deluxeMap\": %s,\n"
            "  \"deluxeMinAngle\": %.2f,\n"
            "  \"deluxeAmbientExaggerate\": %.2f,\n"
            "  \"deluxeRadiosityExaggerate\": %.2f,\n"
            "  \"snapUVs\": %s,\n"
            "  \"antialiasingPasses\": %d, /* post-process AA passes */\n"
            "  \"smoothPasses\": %d, /* passes of blurring lightmaps */\n"
            "  \"smoothRadius\": %.2f, /* fractional values accepted. Minimum 0.1 */\n"
            "  \"exposurefilter\": \"%s\", /* [ off, softknee, reinhard, filmic ] */\n"
            "  \"enforceSampleSize\": %s\n"
            "}\n",
            game->arg, game->gamePath, game->bspIdent, game->bspVersion,
            game->lumpCount, game->maxLMSurfaceVerts, game->maxSurfaceVerts,
            game->maxSurfaceIndexes, game->lightmapSize, game->writeLightmapSize,
            game->defaultSampleSize, hdrStr, game->hdr8BitScale,
            game->lightmapsRGB ? "true" : "false",
            game->lightgridRGB ? "true" : "false",
            game->texturesRGB ? "true" : "false",
            game->colorsRGB ? "true" : "false",
            game->radiosityPasses,
            game->radiosityIntensity,
            game->radiosityColorRatio,
            falloffStr,
            sunFalloffStr,
            game->deluxeMap ? "true" : "false",
            game->deluxeMinAngle,
            game->deluxeAmbientExaggerate,
            game->deluxeRadiosityExaggerate,
            game->snapUVs ? "true" : "false",
            game->antialiasingPasses,
            game->defaultSmoothPasses, game->defaultSmoothRadius, filterStr,
            game->enforceSampleSize ? "true" : "false");
    SaveFile(filename, buffer, strlen(buffer));
}

void JSON_ExportStandardPackages(const char *directory)
{
    char path[1024];
    Q_mkdir(directory);

    sprintf(path, "%s/quake3.json", directory);
    if (!FileExists(path))
    {
        JSON_ExportGame(path, &gameTemplates[1]);
        _printf("Exporting default 'quake3.json'...\n");
    }

    sprintf(path, "%s/qfusion.json", directory);
    if (!FileExists(path))
    {
        JSON_ExportGame(path, &gameTemplates[0]);
        _printf("Exporting default 'qfusion.json'...\n");
    }
}
