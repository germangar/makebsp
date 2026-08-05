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

static qboolean JSON_LoadGame_Internal(const char *filename, game_t *game, int depth)
{
    if (depth > 4)
    {
        _printf("ERROR: Maximum game profile template depth exceeded (%d) when loading '%s'\n", depth, filename);
        return qfalse;
    }

    struct json_value_s *root = JSON_ReadFile(filename);
    if (!root)
        return qfalse;

    struct json_object_s *obj = json_value_as_object(root);
    if (!obj)
    {
        JSON_Free(root);
        return qfalse;
    }

    // Pre-pass: check for "template" key
    struct json_object_element_s *el = obj->start;
    while (el)
    {
        if (!Q_stricmp(el->name->string, "template") && el->value->type == json_type_string)
        {
            const char *templateName = json_value_as_string(el->value)->string;
            if (templateName[0] && Q_stricmp(templateName, "qfusion")) // Skip if empty or base qfusion template
            {
                char dir[1024];
                char templatePath[1024];
                ExtractFilePath(filename, dir);
                sprintf(templatePath, "%s%s.json", dir, templateName);
                if (FileExists(templatePath))
                {
                    _printf("Loading template game profile: %s\n", templatePath);
                    JSON_LoadGame_Internal(templatePath, game, depth + 1);
                }
                else
                {
                    _printf("WARNING: Template profile '%s' not found for '%s'\n", templateName, filename);
                }
            }
            break;
        }
        el = el->next;
    }

    // Main pass: load keys
    el = obj->start;
    while (el)
    {
        const char *key = el->name->string;
        struct json_value_s *val = el->value;

        if (!Q_stricmp(key, "template"))
        {
            // Already handled
        }
        else if (!Q_stricmp(key, "game") && val->type == json_type_string)
        {
            game->arg = copystring(json_value_as_string(val)->string);
        }
        else if (!Q_stricmp(key, "rootDir") && val->type == json_type_string)
        {
            game->rootDir = copystring(json_value_as_string(val)->string);
        }
        else if (!Q_stricmp(key, "userDir") && val->type == json_type_string)
        {
            game->userDir = copystring(json_value_as_string(val)->string);
        }
        else if (!Q_stricmp(key, "gameDir") && val->type == json_type_string)
        {
            game->gameDir = copystring(json_value_as_string(val)->string);
        }
        else if (!Q_stricmp(key, "bspIdent") && val->type == json_type_string)
        {
            game->bspIdent = copystring(json_value_as_string(val)->string);
        }
        else if (!Q_stricmp(key, "bspVersion") && val->type == json_type_number)
        {
            game->bspVersion = atoi(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "lumpCount") && val->type == json_type_number)
        {
            game->lumpCount = atoi(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "maxLMSurfaceVerts") && val->type == json_type_number)
        {
            game->maxLMSurfaceVerts = atoi(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "maxSurfaceVerts") && val->type == json_type_number)
        {
            game->maxSurfaceVerts = atoi(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "maxSurfaceIndexes") && val->type == json_type_number)
        {
            game->maxSurfaceIndexes = atoi(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "customSurfaceParms") && val->type == json_type_array)
        {
            struct json_array_s *arr = json_value_as_array(val);
            if (arr)
            {
                struct json_array_element_s *arr_el = arr->start;
                game->numCustomSurfaceParms = 0;
                while (arr_el && game->numCustomSurfaceParms < MAX_CUSTOM_SURFACEPARMS)
                {
                    if (arr_el->value->type == json_type_object)
                    {
                        struct json_object_s *parmObj = json_value_as_object(arr_el->value);
                        if (parmObj)
                        {
                            customSurfaceParm_t *cp = &game->customSurfaceParms[game->numCustomSurfaceParms];
                            memset(cp, 0, sizeof(customSurfaceParm_t));

                            struct json_object_element_s *pel = parmObj->start;
                            while (pel)
                            {
                                const char *pkey = pel->name->string;
                                struct json_value_s *pval = pel->value;

                                if (!Q_stricmp(pkey, "name") && pval->type == json_type_string)
                                {
                                    strncpy(cp->name, json_value_as_string(pval)->string, sizeof(cp->name)-1);
                                }
                                else if (!Q_stricmp(pkey, "clearSolid"))
                                {
                                    if (pval->type == json_type_true) cp->clearSolid = 1;
                                    else if (pval->type == json_type_number) cp->clearSolid = atoi(json_value_as_number(pval)->number);
                                }
                                else if (!Q_stricmp(pkey, "surfaceFlags"))
                                {
                                    if (pval->type == json_type_number)
                                        cp->surfaceFlags = atoi(json_value_as_number(pval)->number);
                                    else if (pval->type == json_type_string)
                                        cp->surfaceFlags = strtol(json_value_as_string(pval)->string, NULL, 0);
                                }
                                else if (!Q_stricmp(pkey, "contentFlags") || !Q_stricmp(pkey, "contents"))
                                {
                                    if (pval->type == json_type_number)
                                        cp->contents = atoi(json_value_as_number(pval)->number);
                                    else if (pval->type == json_type_string)
                                        cp->contents = strtol(json_value_as_string(pval)->string, NULL, 0);
                                }
                                pel = pel->next;
                            }
                            game->numCustomSurfaceParms++;
                        }
                    }
                    arr_el = arr_el->next;
                }
            }
        }
        else if (!Q_stricmp(key, "lightmapSize") && val->type == json_type_number)
        {
            game->lightmapSize = atoi(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "externalLightmaps") || !Q_stricmp(key, "exportLightmaps"))
        {
            if (val->type == json_type_true)
                game->externalLightmaps = qtrue;
            else if (val->type == json_type_false)
                game->externalLightmaps = qfalse;
        }
        else if (!Q_stricmp(key, "keepLights"))
        {
            if (val->type == json_type_true)
                game->keepLights = qtrue;
            else if (val->type == json_type_false)
                game->keepLights = qfalse;
        }
        else if (!Q_stricmp(key, "cutoff") && val->type == json_type_number)
        {
            game->minLightAdd = (float)atof(json_value_as_number(val)->number);
            if (game->minLightAdd < 0.001f)
                game->minLightAdd = 0.001f;
        }
        else if (!Q_stricmp(key, "fadeout") && val->type == json_type_number)
        {
            game->fadeout = (float)atof(json_value_as_number(val)->number);
            if (game->fadeout < 0.0f)
                game->fadeout = 0.0f;
            else if (game->fadeout > 1.0f)
                game->fadeout = 1.0f;
        }
        else if (!Q_stricmp(key, "backSplashSpot") && val->type == json_type_number)
        {
            game->backSplashSpot = (float)atof(json_value_as_number(val)->number);
            if (game->backSplashSpot < 0.0f) game->backSplashSpot = 0.0f;
            else if (game->backSplashSpot > 1.0f) game->backSplashSpot = 1.0f;
        }
        else if (!Q_stricmp(key, "backSplashSurface") && val->type == json_type_number)
        {
            game->backSplashSurface = (float)atof(json_value_as_number(val)->number);
            if (game->backSplashSurface < 0.0f) game->backSplashSurface = 0.0f;
            else if (game->backSplashSurface > 1.0f) game->backSplashSurface = 1.0f;
        }
        else if (!Q_stricmp(key, "sampleSize") && val->type == json_type_number)
        {
            game->defaultSampleSize = atoi(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "hdr") && val->type == json_type_string)
        {
            const char *h = json_value_as_string(val)->string;
            if (!Q_stricmp(h, "rgb8"))
                game->hdr = HDR_8BIT;
            else if (!Q_stricmp(h, "rgba16f"))
                game->hdr = HDR_16BIT;
            else if (!Q_stricmp(h, "rgba32f"))
                game->hdr = HDR_32BIT;
            else
                game->hdr = HDR_OFF;
        }
        else if (!Q_stricmp(key, "hdr8BitScale") && val->type == json_type_number)
        {
            game->hdr8BitScale = (float)atof(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "lightmapsRGB"))
        {
            if (val->type == json_type_true)
                game->lightmapsRGB = qtrue;
            else if (val->type == json_type_false)
                game->lightmapsRGB = qfalse;
        }
        else if (!Q_stricmp(key, "lightgridRGB"))
        {
            if (val->type == json_type_true)
                game->lightgridRGB = qtrue;
            else if (val->type == json_type_false)
                game->lightgridRGB = qfalse;
        }
        else if (!Q_stricmp(key, "texturesRGB"))
        {
            if (val->type == json_type_true)
                game->texturesRGB = qtrue;
            else if (val->type == json_type_false)
                game->texturesRGB = qfalse;
        }
        else if (!Q_stricmp(key, "colorsRGB"))
        {
            if (val->type == json_type_true)
                game->colorsRGB = qtrue;
            else if (val->type == json_type_false)
                game->colorsRGB = qfalse;
        }
        else if (!Q_stricmp(key, "radiosityPasses") && val->type == json_type_number)
        {
            game->radiosityPasses = atoi(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "radiosityIntensity") && val->type == json_type_number)
        {
            game->radiosityIntensity = (float)atof(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "radiosityColorRatio") && val->type == json_type_number)
        {
            game->radiosityColorRatio = (float)atof(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "radiosityInterval") && val->type == json_type_number)
        {
            game->radiosityInterval = atoi(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "rad_ao_intensity") && val->type == json_type_number)
        {
            game->rad_ao_intensity = (float)atof(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "rad_ao_min") && val->type == json_type_number)
        {
            game->rad_ao_min = (float)atof(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "rad_ao_max") && val->type == json_type_number)
        {
            game->rad_ao_max = (float)atof(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "ambientTestRadius") && val->type == json_type_number)
        {
            game->ambientTestRadius = (float)atof(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "ambientGatherRadius") && val->type == json_type_number)
        {
            game->ambientGatherRadius = (float)atof(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "deluxeMap"))
        {
            if (val->type == json_type_true)
                game->deluxeMap = qtrue;
            else if (val->type == json_type_false)
                game->deluxeMap = qfalse;
        }
        else if (!Q_stricmp(key, "deluxeMinAngle") && val->type == json_type_number)
        {
            game->deluxeMinAngle = (float)atof(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "smoothPasses") && val->type == json_type_number)
        {
            game->defaultSmoothPasses = atoi(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "smoothRadius") && val->type == json_type_number)
        {
            game->defaultSmoothRadius = (float)atof(json_value_as_number(val)->number);
            if (game->defaultSmoothRadius < 0.1f)
                game->defaultSmoothRadius = 0.1f;
        }
        else if (!Q_stricmp(key, "antialiasingPasses") && val->type == json_type_number)
        {
            game->antialiasingPasses = atoi(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "superSampleRadius") && val->type == json_type_number)
        {
            game->superSampleRadius = (float)atof(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "shading") && val->type == json_type_string)
        {
            const char *f = json_value_as_string(val)->string;
            if (!Q_stricmp(f, "halflambert"))
                game->shadingModel = SHADING_MODEL_HALFLAMBERT;
            else if (!Q_stricmp(f, "quadratic"))
                game->shadingModel = SHADING_MODEL_QUADRATIC;
            else if (!Q_stricmp(f, "doublequadratic"))
                game->shadingModel = SHADING_MODEL_DOUBLEQUADRATIC;
            else if (!Q_stricmp(f, "unreal"))
                game->shadingModel = SHADING_MODEL_UNREAL;
            else
                game->shadingModel = SHADING_MODEL_LAMBERT;
        }
        else if (!Q_stricmp(key, "sunShading") && val->type == json_type_string)
        {
            const char *f = json_value_as_string(val)->string;
            if (!Q_stricmp(f, "halflambert"))
                game->sunShadingModel = SHADING_MODEL_HALFLAMBERT;
            else if (!Q_stricmp(f, "quadratic"))
                game->sunShadingModel = SHADING_MODEL_QUADRATIC;
            else if (!Q_stricmp(f, "doublequadratic"))
                game->sunShadingModel = SHADING_MODEL_DOUBLEQUADRATIC;
            else if (!Q_stricmp(f, "unreal"))
                game->sunShadingModel = SHADING_MODEL_UNREAL;
            else
                game->sunShadingModel = SHADING_MODEL_LAMBERT;
        }
        else if (!Q_stricmp(key, "attenuation") && val->type == json_type_string)
        {
            const char *a = json_value_as_string(val)->string;
            if (!Q_stricmp(a, "soft"))
                game->attenuationModel = ATTENUATION_INVERSE;
            else if (!Q_stricmp(a, "linear"))
                game->attenuationModel = ATTENUATION_LINEAR;
            else
                game->attenuationModel = ATTENUATION_INVERSE_SQUARE;
        }
        else if (!Q_stricmp(key, "exposurefilter") && val->type == json_type_string)
        {
            const char *ef = json_value_as_string(val)->string;
            if (!Q_stricmp(ef, "softknee"))
                game->exposureFilter = TONEMAP_SOFTKNEE;
            else if (!Q_stricmp(ef, "reinhard"))
                game->exposureFilter = TONEMAP_REINHARD;
            else if (!Q_stricmp(ef, "filmic"))
                game->exposureFilter = TONEMAP_FILMIC;
            else
                game->exposureFilter = TONEMAP_LINEAR;
        }
        else if (!Q_stricmp(key, "saturation") && val->type == json_type_number)
        {
            game->saturation = (float)atof(json_value_as_number(val)->number);
            if (game->saturation < 0.0f)
                game->saturation = 0.0f;
        }
        else if (!Q_stricmp(key, "saturationRamp") && val->type == json_type_string)
        {
            const char *sr = json_value_as_string(val)->string;
            if (!Q_stricmp(sr, "filmic"))
                game->saturationRamp = SATRAMP_FILMIC;
            else if (!Q_stricmp(sr, "power"))
                game->saturationRamp = SATRAMP_POWER;
            else if (!Q_stricmp(sr, "halfpower"))
                game->saturationRamp = SATRAMP_HALF_POWER;
            else if (!Q_stricmp(sr, "midtone"))
                game->saturationRamp = SATRAMP_MIDTONE;
            else
                game->saturationRamp = SATRAMP_OFF;
        }
        else if (!Q_stricmp(key, "enforceSampleSize"))
        {
            if (val->type == json_type_true)
                game->enforceSampleSize = qtrue;
            else if (val->type == json_type_false)
                game->enforceSampleSize = qfalse;
        }
        else if (!Q_stricmp(key, "forceUVGen"))
        {
            if (val->type == json_type_true)
                game->forceUVGen = qtrue;
            else if (val->type == json_type_false)
                game->forceUVGen = qfalse;
        }
        else if (!Q_stricmp(key, "flareShader") && val->type == json_type_string)
        {
            game->flareShader = copystring(json_value_as_string(val)->string);
        }
        else if (!Q_stricmp(key, "haloShader") && val->type == json_type_string)
        {
            game->haloShader = copystring(json_value_as_string(val)->string);
        }
        else if (!Q_stricmp(key, "chamferEdges"))
        {
            if (val->type == json_type_true)
                game->chamferEdges = qtrue;
            else if (val->type == json_type_false)
                game->chamferEdges = qfalse;
        }
        else if (!Q_stricmp(key, "chamferConvexWidth") && val->type == json_type_number) {
            game->chamferConvexWidth = (float)atof(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "chamferConcaveWidth") && val->type == json_type_number)
        {
            game->chamferConcaveWidth = (float)atof(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "decalExtrusion") && val->type == json_type_number)
        {
            game->decalExtrusion = (float)atof(json_value_as_number(val)->number);
        }


        el = el->next;
    }

    // Lightmap size validation: must be power of 2, max 4096, min 128
    int targetSize = game->lightmapSize;
    if (targetSize < 128) targetSize = 128;
    if (targetSize > 4096) targetSize = 4096;
    
    int p = 128;
    while (p * 2 <= targetSize) p *= 2;
    if (targetSize - p > (p * 2) - targetSize) p *= 2;

    if (game->lightmapSize != p)
    {
        _printf("WARNING: lightmapSize (%d) snapped to valid power of 2 (%d).\n", game->lightmapSize, p);
        game->lightmapSize = p;
    }

    JSON_Free(root);
    return qtrue;
}

qboolean JSON_LoadGame(const char *filename, game_t *game)
{
    return JSON_LoadGame_Internal(filename, game, 0);
}

void JSON_ExportGame(const char *filename, game_t *game)
{
    char buffer[16384];
    const char *shadingModelStr;
    switch (game->shadingModel)
    {
    case SHADING_MODEL_LAMBERT:
        shadingModelStr = "lambert";
        break;
    case SHADING_MODEL_HALFLAMBERT:
        shadingModelStr = "halflambert";
        break;
    case SHADING_MODEL_QUADRATIC:
        shadingModelStr = "quadratic";
        break;
    case SHADING_MODEL_DOUBLEQUADRATIC:
        shadingModelStr = "doublequadratic";
        break;
    case SHADING_MODEL_UNREAL:
        shadingModelStr = "unreal";
        break;
    default:
        shadingModelStr = "unknown";
        break;
    }

    const char *sunShadingModelStr;
    switch (game->sunShadingModel)
    {
    case SHADING_MODEL_LAMBERT:
        sunShadingModelStr = "lambert";
        break;
    case SHADING_MODEL_HALFLAMBERT:
        sunShadingModelStr = "halflambert";
        break;
    case SHADING_MODEL_QUADRATIC:
        sunShadingModelStr = "quadratic";
        break;
    case SHADING_MODEL_DOUBLEQUADRATIC:
        sunShadingModelStr = "doublequadratic";
        break;
    case SHADING_MODEL_UNREAL:
        sunShadingModelStr = "unreal";
        break;
    default:
        sunShadingModelStr = "unknown";
        break;
    }

    const char *attenuationModelStr;
    switch (game->attenuationModel)
    {
    case ATTENUATION_INVERSE:
        attenuationModelStr = "soft";
        break;
    case ATTENUATION_LINEAR:
        attenuationModelStr = "linear";
        break;
    default:
        attenuationModelStr = "standard";
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

    const char *satRampStr;
    switch (game->saturationRamp)
    {
    case SATRAMP_FILMIC:
        satRampStr = "filmic";
        break;
    case SATRAMP_POWER:
        satRampStr = "power";
        break;
    case SATRAMP_HALF_POWER:
        satRampStr = "halfpower";
        break;
    case SATRAMP_MIDTONE:
        satRampStr = "midtone";
        break;
    default:
        satRampStr = "off";
        break;
    }

    sprintf(buffer,
            "{\n"
            "  \"game\": \"%s\",\n"
            "  \"template\": \"\",\n"
            "  \"rootDir\": \"%s\",\n"
            "  \"userDir\": \"%s\",\n"
            "  \"gameDir\": \"%s\",\n"
            "  \"bspIdent\": \"%s\",\n"
            "  \"bspVersion\": %d,\n"
            "  \"lumpCount\": %d,\n"
            "  \"maxLMSurfaceVerts\": %d,\n"
            "  \"maxSurfaceVerts\": %d,\n"
            "  \"maxSurfaceIndexes\": %d,\n"
            "  \"lightmapSize\": %d,\n"
            "  \"externalLightmaps\": %s, /* if true, lightmaps are stored as external .tga files instead of inside the BSP */\n"
            "  \"keepLights\": %s, /* if true, light entities are not stripped from the BSP */\n"
            "  \"sampleSize\": %i,\n"
            "  \"hdr\": \"%s\", /* [ off, rgb8, rgb16, rgb32 ] More than 8 bit requires a bsp version change */\n"
            "  \"hdr8BitScale\": %.2f,\n"
            "  \"lightmapsRGB\": %s,\n"
            "  \"lightgridRGB\": %s,\n"
            "  \"texturesRGB\": %s,\n"
            "  \"colorsRGB\": %s,\n"
            "  \"radiosityPasses\": %d, /* Number of times the light bounces */\n"
            "  \"radiosityIntensity\": %.2f,\n"
            "  \"radiosityColorRatio\": %.2f, /* Percentage (0.0 to 1.0) of surface color transferred to the bounce light */\n"
            "  \"radiosityInterval\": %d, /* Radiosity grid sample size */\n"
            "  \"rad_ao_intensity\": %.2f, /* Intensity of ambient occlusion shadowing */\n"
            "  \"rad_ao_min\": %.2f, /* Minimum distance for ambient occlusion shadowing */\n"
            "  \"rad_ao_max\": %.2f, /* Maximum distance for ambient occlusion shadowing */\n"
            "  \"ambientTestRadius\": %.2f, /* Radius at which a light probe tests for being occluded */\n"
            "  \"ambientGatherRadius\": %.2f, /* Distance at which a lightmap pixel gathers light from probes */\n"
            "  \"shading\": \"%s\",  /* [ lambert, halflambert, quadratic, doublequadratic, unreal ] */\n"
            "  \"sunShading\": \"%s\",  /* [ lambert, halflambert, quadratic, doublequadratic, unreal ] */\n"
            "  \"attenuation\": \"%s\",  /* [ standard, soft, linear ] */\n"
            "  \"cutoff\": %f, /* Minimum remaining light energy to apply the contribution to a surface */\n"
            "  \"fadeout\": %f, /* Percentage of the light's outer radius to fade linearly until reaching cutoff */\n"
            "  \"backSplashSpot\": %f, /* Default entity spotlight backsplash fraction (0.0 to 1.0) */\n"
            "  \"backSplashSurface\": %f, /* Default surface light backsplash fraction (0.0 to 1.0) */\n"
            "  \"deluxeMap\": %s,\n"
            "  \"deluxeMinAngle\": %.2f,\n"
            "  \"antialiasingPasses\": %d, /*Number of post-process AA passes */\n"
            "  \"superSampleRadius\": %.2f,\n"
            "  \"smoothPasses\": %d, /* passes of blurring lightmaps */\n"
            "  \"smoothRadius\": %.2f, /* fractional values accepted. Minimum 0.1 */\n"
            "  \"exposurefilter\": \"%s\", /* [ off, softknee, reinhard, filmic ] */\n"
            "  \"saturation\": %.2f, /* Multiplier (1.0 = normal, 0.0 = grayscale) */\n"
            "  \"saturationRamp\": \"%s\", /* [ off, filmic, power, midtone ] */\n"
            "  \"chamferEdges\": %s,\n"
            "  \"chamferConvexWidth\": %.2f,\n"
            "  \"chamferConcaveWidth\": %.2f,\n"
            "  \"decalExtrusion\": %.2f,\n"
            "  \"enforceSampleSize\": %s,\n"
            "  \"forceUVGen\": %s,\n"
            "  \"flareShader\": \"%s\",\n"
            "  \"haloShader\": \"%s\"",
            game->arg, game->rootDir, game->userDir ? game->userDir : "", game->gameDir, game->bspIdent, game->bspVersion,
            game->lumpCount, game->maxLMSurfaceVerts, game->maxSurfaceVerts,
            game->maxSurfaceIndexes, game->lightmapSize,
            game->externalLightmaps ? "true" : "false",
            game->keepLights ? "true" : "false",
            game->defaultSampleSize, hdrStr, game->hdr8BitScale,
            game->lightmapsRGB ? "true" : "false",
            game->lightgridRGB ? "true" : "false",
            game->texturesRGB ? "true" : "false",
            game->colorsRGB ? "true" : "false",
            game->radiosityPasses,
            game->radiosityIntensity,
            game->radiosityColorRatio,
            game->radiosityInterval,
            game->rad_ao_intensity,
            game->rad_ao_min,
            game->rad_ao_max,
            game->ambientTestRadius,
            game->ambientGatherRadius,
            shadingModelStr,
            sunShadingModelStr,
            attenuationModelStr,
            game->minLightAdd,
            game->fadeout,
            game->backSplashSpot,
            game->backSplashSurface,
            game->deluxeMap ? "true" : "false",
            game->deluxeMinAngle,

            game->antialiasingPasses,
            game->superSampleRadius,
            game->defaultSmoothPasses,
            game->defaultSmoothRadius,
            filterStr,
            game->saturation,
            satRampStr,
            game->chamferEdges ? "true" : "false",
            game->chamferConvexWidth,
            game->chamferConcaveWidth,
            game->decalExtrusion,
            game->enforceSampleSize ? "true" : "false",
            game->forceUVGen ? "true" : "false",
            game->flareShader ? game->flareShader : "",
            game->haloShader ? game->haloShader : "");

    if (game->numCustomSurfaceParms > 0)
    {
        strcat(buffer, ",\n  \"customSurfaceParms\": [\n");
        for (int i = 0; i < game->numCustomSurfaceParms; i++)
        {
            char temp[256];
            sprintf(temp, "    { \"name\": \"%s\", \"clearSolid\": %s, \"surfaceFlags\": %d, \"contentFlags\": %d }%s\n",
                    game->customSurfaceParms[i].name,
                    game->customSurfaceParms[i].clearSolid ? "true" : "false",
                    game->customSurfaceParms[i].surfaceFlags,
                    game->customSurfaceParms[i].contents,
                    (i < game->numCustomSurfaceParms - 1) ? "," : "");
            strcat(buffer, temp);
        }
        strcat(buffer, "  ]\n}\n");
    }
    else
    {
        strcat(buffer, "\n}\n");
    }

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
