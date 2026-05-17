#include "lightdata.h"
#include "light.h"
#include "../common/bspfile.h"
#include "../common/cmdlib.h"
#include "../shared/globals.h"
#include "../shared/mesh.h"
#include "../common/imagelib.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

drawVert32_t *internalDrawVerts = NULL;
float *lightFloats = NULL;
float *deluxeFloats = NULL;
float *energyFloats = NULL;
float *normalFloats = NULL;
int *lightSurfaceIndex = NULL;
float *radiosityFloats = NULL;
float *accumRadiosityFloats = NULL;
float *radiosityDeluxeFloats = NULL;
float *radiosityEnergyFloats = NULL;
float *accumRadiosityDeluxeSum = NULL;
float *accumRadiosityEnergyFloats = NULL;
byte *lightAlphaMask = NULL;
bspGridPoint32_t *gridData32 = NULL;

float maxLightIntensity = 0.0f;
static tonemap_t tonemapMode;

/*
===============================================================================
DOWNSCALE LIGHTMAP IMAGES (Experimental).
Generate the whole lightmaps at a higher resolution and scale them
back down only for writting. It currently has a lot of edge bleeding.
Don't use.
===============================================================================
*/

static void DilateLightmapAtlas(int width, int passes)
{
    int i, j, p, s;
    int numLMs = (numLightBytes / 3) / (width * width);
    float *temp = malloc(numLightBytes * sizeof(float));
    byte *tempMask = malloc(numLMs * width * width);
    float *tempDeluxe = NULL;
    float *tempNormal = NULL;
    if (deluxeFloats)
        tempDeluxe = malloc(numLightBytes * sizeof(float));
    if (normalFloats)
        tempNormal = malloc(numLightBytes * sizeof(float));

    _printf("Dilating lightmaps (%d passes)...\n", passes);

    for (p = 0; p < passes; p++)
    {
        memcpy(temp, lightFloats, numLightBytes * sizeof(float));
        memcpy(tempMask, lightAlphaMask, numLMs * width * width);
        if (deluxeFloats)
            memcpy(tempDeluxe, deluxeFloats, numLightBytes * sizeof(float));
        if (normalFloats)
            memcpy(tempNormal, normalFloats, numLightBytes * sizeof(float));

        for (s = 0; s < numDrawSurfaces; s++)
        {
            dsurface_t *ds = &drawSurfaces[s];
            int sLM = ds->lightmapNum[0];
            if (sLM < 0)
                continue;

            int sX = ds->lightmapOffset[0][0];
            int sY = ds->lightmapOffset[0][1];
            int sW = ds->lightmapWidth;
            int sH = ds->lightmapHeight;

            // The allocator reserves a 1-texel gutter around the surface.
            // sX and sY already include the +1 offset, so the true allocated block is:
            int minX = sX - 1;
            int minY = sY - 1;
            int maxX = sX + sW;
            int maxY = sY + sH;

            // Clamp to atlas bounds just in case
            if (minX < 0)
                minX = 0;
            if (minY < 0)
                minY = 0;
            if (maxX >= width)
                maxX = width - 1;
            if (maxY >= width)
                maxY = width - 1;

            for (int y = minY; y <= maxY; y++)
            {
                for (int x = minX; x <= maxX; x++)
                {
                    int idx = (sLM * width * width) + y * width + x;
                    float energy = temp[idx * 3] + temp[idx * 3 + 1] + temp[idx * 3 + 2];
                    if (tempMask[idx] && energy > 0.0002f)
                        continue;

                    float sum[3] = {0, 0, 0}, weight = 0;
                    float litSum[3] = {0, 0, 0}, litWeight = 0;
                    float deluxeSum[3] = {0, 0, 0};
                    float deluxeLitSum[3] = {0, 0, 0};
                    float normalSum[3] = {0, 0, 0};
                    float normalLitSum[3] = {0, 0, 0};

                    for (j = -1; j <= 1; j++)
                    {
                        for (i = -1; i <= 1; i++)
                        {
                            if (i == 0 && j == 0)
                                continue;
                            int nx = x + i;
                            int ny = y + j;
                            // Only sample neighbors that are WITHIN this surface's bounding box
                            if (nx >= minX && nx <= maxX && ny >= minY && ny <= maxY)
                            {
                                int nidx = (sLM * width * width) + ny * width + nx;
                                if (tempMask[nidx])
                                {
                                    float nEnergy = temp[nidx * 3] + temp[nidx * 3 + 1] + temp[nidx * 3 + 2];
                                    if (nEnergy > 0.0001f)
                                    {
                                        VectorAdd(litSum, &temp[nidx * 3], litSum);
                                        if (tempDeluxe) VectorAdd(deluxeLitSum, &tempDeluxe[nidx * 3], deluxeLitSum);
                                        if (tempNormal) VectorAdd(normalLitSum, &tempNormal[nidx * 3], normalLitSum);
                                        litWeight += 1.0f;
                                    }
                                    VectorAdd(sum, &temp[nidx * 3], sum);
                                    if (tempDeluxe) VectorAdd(deluxeSum, &tempDeluxe[nidx * 3], deluxeSum);
                                    if (tempNormal) VectorAdd(normalSum, &tempNormal[nidx * 3], normalSum);
                                    weight += 1.0f;
                                }
                            }
                        }
                    }
                    if (litWeight > 0)
                    {
                        VectorScale(litSum, 1.0f / litWeight, &lightFloats[idx * 3]);
                        lightAlphaMask[idx] = 1;
                        if (deluxeFloats)
                        {
                            vec3_t avgDir;
                            VectorScale(deluxeLitSum, 1.0f / litWeight, avgDir);
                            if (VectorNormalize(avgDir, avgDir) > 0)
                                VectorCopy(avgDir, &deluxeFloats[idx * 3]);
                        }
                        if (normalFloats)
                        {
                            vec3_t avgNrm;
                            VectorScale(normalLitSum, 1.0f / litWeight, avgNrm);
                            if (VectorNormalize(avgNrm, avgNrm) > 0)
                                VectorCopy(avgNrm, &normalFloats[idx * 3]);
                        }
                    }
                    else if (weight > 0)
                    {
                        VectorScale(sum, 1.0f / weight, &lightFloats[idx * 3]);
                        lightAlphaMask[idx] = 1;
                        if (deluxeFloats)
                        {
                            vec3_t avgDir;
                            VectorScale(deluxeSum, 1.0f / weight, avgDir);
                            if (VectorNormalize(avgDir, avgDir) > 0)
                                VectorCopy(avgDir, &deluxeFloats[idx * 3]);
                        }
                        if (normalFloats)
                        {
                            vec3_t avgNrm;
                            VectorScale(normalSum, 1.0f / weight, avgNrm);
                            if (VectorNormalize(avgNrm, avgNrm) > 0)
                                VectorCopy(avgNrm, &normalFloats[idx * 3]);
                        }
                    }
                }
            }
        }
    }
    free(temp);
    free(tempMask);
    if (tempDeluxe)
        free(tempDeluxe);
    if (tempNormal)
        free(tempNormal);
}

static void DownscaleSurfaceLightmap(dsurface_t *ds, int ratio, float *oldFloats, byte *oldMask, int oldW, float *newFloats, byte *newMask, int newW)
{
    int x, y, dx, dy;
    int sLM = ds->lightmapNum[0];
    int sOldX = ds->lightmapOffset[0][0];
    int sOldY = ds->lightmapOffset[0][1];
    int sOldW = ds->lightmapWidth;
    int sOldH = ds->lightmapHeight;

    int sNewX = sOldX / ratio;
    int sNewY = sOldY / ratio;
    int sNewW = sOldW / ratio;
    int sNewH = sOldH / ratio;

    if (sNewW <= 0)
        sNewW = 1;
    if (sNewH <= 0)
        sNewH = 1;

    // This follows the logic in lm_postprocess.c:1700-1719
    // Loop through new luxels and average the corresponding 'ratio x ratio' block from the old atlas
    for (y = 0; y < sNewH; y++)
    {
        for (x = 0; x < sNewW; x++)
        {
            int nX = sNewX + x;
            int nY = sNewY + y;
            int newP = (sLM * newW * newW) + nY * newW + nX;

            float sumColor[3] = {0, 0, 0}, sumWeight = 0.0f;

            // Map back to absolute coordinates in the old atlas to maintain alignment
            int startX = sOldX + (x * ratio);
            int startY = sOldY + (y * ratio);

            for (dy = 0; dy < ratio; dy++)
            {
                for (dx = 0; dx < ratio; dx++)
                {
                    int X = startX + dx;
                    int Y = startY + dy;

                    // Ensure we stay within the boundaries of the original surface and the atlas
                    if (X >= oldW || Y >= oldW)
                        continue;
                    if (X >= sOldX + sOldW || Y >= sOldY + sOldH)
                        continue;

                    int oldP = (sLM * oldW * oldW) + Y * oldW + X;
                    if (oldMask[oldP] != 0)
                    {
                        VectorAdd(sumColor, &oldFloats[oldP * 3], sumColor);
                        sumWeight += 1.0f;
                    }
                }
            }

            if (sumWeight > 0.01f)
            {
                VectorScale(sumColor, 1.0f / sumWeight, &newFloats[newP * 3]);
                newMask[newP] = 1;
            }
        }
    }

    // Update metadata
    for (int j = 0; j < 4; j++)
    {
        ds->lightmapOffset[j][0] = sNewX;
        ds->lightmapOffset[j][1] = sNewY;
    }
    ds->lightmapWidth = sNewW;
    ds->lightmapHeight = sNewH;
}

void DownscaleLightmaps(int oldW, int newW)
{
    int i, j;
    int ratio = oldW / newW;
    int numLMs = (numLightBytes / 3) / (oldW * oldW);
    int newTotalPixels = numLMs * newW * newW;

    _printf("--- DownscaleLightmaps (PER-SURFACE) (%dx%d -> %dx%d) ---\n", oldW, oldW, newW, newW);

    float *newFloats = calloc(newTotalPixels * 3, sizeof(float));
    float *newDeluxe = deluxeFloats ? calloc(newTotalPixels * 3, sizeof(float)) : NULL;
    byte *newMask = calloc(newTotalPixels, sizeof(byte));

    for (i = 0; i < numDrawSurfaces; i++)
    {
        if (drawSurfaces[i].lightmapNum[0] >= 0)
        {
            DownscaleSurfaceLightmap(&drawSurfaces[i], ratio, lightFloats, lightAlphaMask, oldW, newFloats, newMask, newW);
            if (newDeluxe)
            {
                DownscaleSurfaceLightmap(&drawSurfaces[i], ratio, deluxeFloats, lightAlphaMask, oldW, newDeluxe, NULL, newW);
            }
        }
    }

    // Update vertex UVs
    for (i = 0; i < numDrawVerts; i++)
    {
        drawVert_t *dv = &drawVerts[i];
        for (j = 0; j < 4; j++)
        {
            dv->lightmap[j][0] = ((dv->lightmap[j][0] * (float)oldW - 0.5f) / (float)ratio + 0.5f) / (float)newW;
            dv->lightmap[j][1] = ((dv->lightmap[j][1] * (float)oldW - 0.5f) / (float)ratio + 0.5f) / (float)newW;
        }
    }

    free(lightFloats);
    free(lightAlphaMask);
    lightFloats = newFloats;
    lightAlphaMask = newMask;

    if (deluxeFloats)
    {
        free(deluxeFloats);
        deluxeFloats = newDeluxe;
    }

    // Update global state
    game->lightmapSize = newW;
    numLightBytes = newTotalPixels * 3;

    if (num_entities > 0)
    {
        SetKeyValue(&entities[0], "__lightmapsize", va("%d", newW));
    }
}

/*
===============================================================================
COLOR CONVERSION HELPERS
===============================================================================
*/

void InternalColorToBytes(const float *color, byte *colorBytes, qboolean sRGB)
{
    float max;
    vec3_t sample;
    int i;

    VectorCopy(color, sample);

    if (sRGB)
    {
        for (i = 0; i < 3; i++)
        {
            float l = sample[i] / 255.0f;
            if (l <= 0.0031308f)
                l *= 12.92f;
            else
                l = 1.055f * (float)pow(l, 1.0f / 2.4f) - 0.055f;
            sample[i] = l * 255.0f;
        }
    }

    // clamp with color normalization
    max = sample[0];
    if (sample[1] > max)
    {
        max = sample[1];
    }
    if (sample[2] > max)
    {
        max = sample[2];
    }
    if (max > 255)
    {
        VectorScale(sample, 255.0f / max, sample);
    }

    for (i = 0; i < 3; i++)
    {
        int c = (int)floor(sample[i] + 0.5f);
        if (c < 0)
        {
            c = 0;
        }
        else if (c > 255)
        {
            c = 255;
        }
        colorBytes[i] = (byte)c;
    }
}

void InternalColorToBytesScaled(const float *color, byte *colorBytes, float scale, qboolean sRGB)
{
    vec3_t sample;
    int i;
    VectorCopy(color, sample);

    if (tonemapMode != TONEMAP_LINEAR)
    {
        float maxC = sample[0];
        if (sample[1] > maxC)
            maxC = sample[1];
        if (sample[2] > maxC)
            maxC = sample[2];

        if (maxC > 0.001f)
        {
            float limit = (game->hdr == HDR_8BIT) ? maxLightIntensity : 255.0f;
            float threshold = limit * 0.75f;

            if (tonemapMode == TONEMAP_SOFTKNEE)
            {
                if (maxC > threshold)
                {
                    // Math: y = threshold + (x - threshold) / (1 + ((x - threshold) / (limit - threshold)))
                    float softMax = threshold + (maxC - threshold) / (1.0f + ((maxC - threshold) / (limit - threshold)));
                    VectorScale(sample, softMax / maxC, sample);
                }
            }
            else if (tonemapMode == TONEMAP_REINHARD)
            {
                // Reinhard relative to 'limit'
                float normalized = maxC / limit;
                float reinhard = normalized / (1.0f + normalized);
                VectorScale(sample, (reinhard * limit) / maxC, sample);
            }
            else if (tonemapMode == TONEMAP_FILMIC)
            {
                // Filmic exponential relative to 'limit'
                float normalized = maxC / limit;
                float filmic = 1.0f - (float)exp(-normalized);
                VectorScale(sample, (filmic * limit) / maxC, sample);
            }
        }
    }

    VectorScale(sample, scale, sample);

    if (sRGB)
    {
        for (i = 0; i < 3; i++)
        {
            float l = sample[i] / 255.0f;
            if (l <= 0.0031308f)
                l *= 12.92f;
            else
                l = 1.055f * (float)pow(l, 1.0f / 2.4f) - 0.055f;
            sample[i] = l * 255.0f;
        }
    }

    for (i = 0; i < 3; i++)
    {
        int c = (int)floor(sample[i] + 0.5f);
        if (c < 0)
        {
            c = 0;
        }
        else if (c > 255)
        {
            c = 255;
        }
        colorBytes[i] = (byte)c;
    }
}

void ScanLightmapIntensity(void)
{
    int i, j;
    maxLightIntensity = 0.0f;

    if (!lightFloats)
        return;

    _printf("--- ScanLightmapIntensity ---\n");
    for (i = 0; i < numLightBytes / 3; i++)
    {
        for (j = 0; j < 3; j++)
        {
            if (lightFloats[i * 3 + j] > maxLightIntensity)
            {
                maxLightIntensity = lightFloats[i * 3 + j];
            }
        }
    }
    _printf("Peak lightmap intensity found: %.3f\n", maxLightIntensity);
}

void CheckGridData32(void)
{
    if (gridData32)
        free(gridData32);
    gridData32 = malloc(numGridPoints * sizeof(bspGridPoint32_t));
    if (!gridData32)
        Error("CheckGridData32: malloc failed");
    memset(gridData32, 0, numGridPoints * sizeof(bspGridPoint32_t));
}

/*
===============================================================================
UP-CONVERSION (8-bit -> 32-bit Float)
===============================================================================
*/

static void UpConvertDrawVerts(void)
{
    int i, j, k;
    if (internalDrawVerts)
        free(internalDrawVerts);
    internalDrawVerts = malloc(MAX_MAP_DRAW_VERTS * sizeof(drawVert32_t));
    if (!internalDrawVerts)
        Error("UpConvertDrawVerts: malloc failed");
    memset(internalDrawVerts, 0, MAX_MAP_DRAW_VERTS * sizeof(drawVert32_t));

    for (i = 0; i < numDrawVerts; i++)
    {
        VectorCopy(drawVerts[i].xyz, internalDrawVerts[i].xyz);
        internalDrawVerts[i].st[0] = drawVerts[i].st[0];
        internalDrawVerts[i].st[1] = drawVerts[i].st[1];
        for (j = 0; j < 4; j++)
        {
            internalDrawVerts[i].lightmap[j][0] = drawVerts[i].lightmap[j][0];
            internalDrawVerts[i].lightmap[j][1] = drawVerts[i].lightmap[j][1];
            // Clean additive start
            for (k = 0; k < 3; k++)
                internalDrawVerts[i].color[j][k] = 0.0f;
        }
        VectorCopy(drawVerts[i].normal, internalDrawVerts[i].normal);
    }
}

static void UpConvertLightmaps(void)
{
    if (lightFloats)
        free(lightFloats);
    _printf("UpConvert: Allocating %d pixel buffers for lightmaps...\n", numLightBytes / 3);
    lightFloats = malloc((numLightBytes / 3) * sizeof(vec3_t));
    if (!lightFloats)
        Error("UpConvertLightmaps: malloc failed");
    memset(lightFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));

    if (!lightAlphaMask)
    {
        lightAlphaMask = malloc((numLightBytes / 3) * sizeof(byte));
        if (!lightAlphaMask)
            Error("UpConvert: malloc lightAlphaMask failed");
        memset(lightAlphaMask, 0, (numLightBytes / 3) * sizeof(byte));
    }

    if (game->deluxeMap)
    {
        if (deluxeFloats)
            free(deluxeFloats);
        if (lightSurfaceIndex)
            free(lightSurfaceIndex);
        _printf("UpConvert: Allocating deluxeMap buffers...\n");
        deluxeFloats = malloc(numLightBytes * sizeof(float));
        if (!deluxeFloats)
            Error("UpConvert: malloc deluxeFloats failed");
        memset(deluxeFloats, 0, numLightBytes * sizeof(float));

        lightSurfaceIndex = malloc((numLightBytes / 3) * sizeof(int));
        if (!lightSurfaceIndex)
            Error("UpConvert: malloc lightSurfaceIndex failed");
        for (int i = 0; i < numLightBytes / 3; i++)
            lightSurfaceIndex[i] = -1;

        if (energyFloats)
            free(energyFloats);
        energyFloats = malloc(numLightBytes * sizeof(float));
        if (!energyFloats)
            Error("UpConvert: malloc energyFloats failed");
        memset(energyFloats, 0, numLightBytes * sizeof(float));

        if (normalFloats)
            free(normalFloats);
        normalFloats = malloc(numLightBytes * sizeof(float));
        if (!normalFloats)
            Error("UpConvert: malloc normalFloats failed");
        memset(normalFloats, 0, numLightBytes * sizeof(float));
    }
}

void UpConvertLightingData(void)
{
    _printf("--- UpConvertLightingData ---\n");

    // The 8-bit lightmap data loaded from the BSP is completely unused
    // by the light compiler (which computes everything from scratch).
    // Free it now to save RAM during the heavy lighting and radiosity passes.
    if (lightBytes)
    {
        free(lightBytes);
        lightBytes = NULL;
    }

    UpConvertDrawVerts();
    UpConvertLightmaps();
    CheckGridData32();
}

/*
===============================================================================
DOWN-CONVERSION (32-bit Float -> 8-bit)
===============================================================================
*/

static void DownConvertDrawVerts(float scale, qboolean lightmapRange)
{
    int i, j;
    if (!internalDrawVerts)
        return;
    _printf("DownConvert: %d DrawVerts\n", numDrawVerts);
    for (i = 0; i < numDrawVerts; i++)
    {
        for (j = 0; j < 4; j++)
        {
            if (lightmapRange)
            {
                InternalColorToBytesScaled(internalDrawVerts[i].color[j], (byte *)drawVerts[i].color[j], scale, game->colorsRGB);
            }
            else
            {
                InternalColorToBytes(internalDrawVerts[i].color[j], (byte *)drawVerts[i].color[j], game->colorsRGB);
            }
        }
    }
}

static void DownConvertLightmaps(float scale, qboolean lightmapRange)
{
    int i;
    if (!lightFloats)
        return;
    _printf("DownConvert: %d Lightmap pixels\n", numLightBytes / 3);
    int processedCount = 0;
    for (i = 0; i < numLightBytes / 3; i++)
    {
        if (lightmapRange)
        {
            InternalColorToBytesScaled(&lightFloats[i * 3], &lightBytes[i * 3], scale, game->lightmapsRGB);
        }
        else
        {
            InternalColorToBytes(&lightFloats[i * 3], &lightBytes[i * 3], game->lightmapsRGB);
        }
        if (lightAlphaMask && lightAlphaMask[i])
            processedCount++;
    }
    _printf("DownConvert: %d pixels marked in alpha mask\n", processedCount);
}

static void DownConvertDeluxeMaps(void)
{
    int i, lm;
    if (!deluxeFloats)
        return;

    int totalPixels = numLightBytes / 3;
    int numLMs = totalPixels / (game->lightmapSize * game->lightmapSize);
    int lmSize = game->lightmapSize * game->lightmapSize * 3;

    _printf("DownConvert: %d DeluxeMap pixels (interleaving as stride 2 for QFusion)\n", totalPixels);

    // Dynamically reallocate lightBytes to double its size for the deluxe layer
    byte *newLightBytes = realloc(lightBytes, numLightBytes * 2);
    if (!newLightBytes)
    {
        _printf("WARNING: DownConvertDeluxeMaps: Failed to reallocate %d bytes! Skipping deluxeMap layer.\n", numLightBytes * 2);
        return;
    }
    lightBytes = newLightBytes;

    // Step 1: Interleave. Move standard lightmaps to EVEN slots
    for (lm = numLMs - 1; lm >= 0; lm--)
    {
        memmove(&lightBytes[(lm * 2) * lmSize], &lightBytes[lm * lmSize], lmSize);
    }

    // Step 2: Encode and write deluxe maps into ODD slots
    for (lm = 0; lm < numLMs; lm++)
    {
        byte *dst = &lightBytes[(lm * 2 + 1) * lmSize];
        int basePixel = lm * (game->lightmapSize * game->lightmapSize);

        for (i = 0; i < (game->lightmapSize * game->lightmapSize); i++)
        {
            vec3_t dir;
            VectorCopy(&deluxeFloats[(basePixel + i) * 3], dir);

            byte *pixelDst = &dst[i * 3];

            if (VectorNormalize(dir, dir) > 0)
            {
                pixelDst[0] = (byte)(dir[0] * 127.5f + 127.5f);
                pixelDst[1] = (byte)(dir[1] * 127.5f + 127.5f);
                pixelDst[2] = (byte)(dir[2] * 127.5f + 127.5f);
            }
            else
            {
                // Default to Up (0,0,1)
                pixelDst[0] = 127;
                pixelDst[1] = 127;
                pixelDst[2] = 255;
            }
        }
    }

    // Step 3: Update surface lightmap indices.
    // The engine requires the base lightmap to be on an EVEN index to detect deluxemaps.
    for (i = 0; i < numDrawSurfaces; i++)
    {
        if (drawSurfaces[i].lightmapNum[0] >= 0)
        {
            drawSurfaces[i].lightmapNum[0] *= 2;
            // The engine implicitly loads the deluxemap from base + 1.
            // Do NOT set lightmapNum[1], as MAX_LIGHTMAPS is for styles, not layers.
        }
    }

    // Step 4: Ensure UVs are duplicated for the deluxe layer in all vertices
    for (i = 0; i < numDrawVerts; i++)
    {
        drawVerts[i].lightmap[1][0] = drawVerts[i].lightmap[0][0];
        drawVerts[i].lightmap[1][1] = drawVerts[i].lightmap[0][1];
    }

    numLightBytes *= 2; // Double it!
}

static void DownConvertGrid(float scale, qboolean lightmapRange)
{
    int i, j;
    if (!gridData32)
        return;
    for (i = 0; i < numGridPoints; i++)
    {
        for (j = 0; j < 4; j++)
        {
            if (lightmapRange)
            {
                InternalColorToBytesScaled(gridData32[i].ambient[j], (byte *)gridData[i].ambient[j], scale, game->lightgridRGB);
                InternalColorToBytesScaled(gridData32[i].directed[j], (byte *)gridData[i].directed[j], scale, game->lightgridRGB);
            }
            else
            {
                InternalColorToBytes(gridData32[i].ambient[j], (byte *)gridData[i].ambient[j], game->lightgridRGB);
                InternalColorToBytes(gridData32[i].directed[j], (byte *)gridData[i].directed[j], game->lightgridRGB);
            }
        }
        gridData[i].latLong[0] = gridData32[i].latLong[0];
        gridData[i].latLong[1] = gridData32[i].latLong[1];
        for (j = 0; j < 4; j++)
        {
            gridData[i].styles[j] = gridData32[i].styles[j];
        }
    }
}

void DownConvertLightingData(void)
{
    float scale = 1.0f;

    _printf("--- DownConvertLightingData ---\n");
    tonemapMode = game->exposureFilter;

    // experimental (and broken) feature
    if (game->writeLightmapSize > 0 && game->writeLightmapSize < game->lightmapSize)
    {
        DownscaleLightmaps(game->lightmapSize, game->writeLightmapSize);
    }

    DilateLightmapAtlas(game->lightmapSize, 2);

    // Deferred Deluxe Division: convert lightFloats from Radiance to Radiance/w
    if (deluxeFloats && normalFloats)
    {
        int totalPixels = numLightBytes / 3;
        for (int idx = 0; idx < totalPixels; idx++)
        {
            if (lightAlphaMask && !lightAlphaMask[idx])
                continue;
            vec3_t n;
            VectorCopy(&normalFloats[idx * 3], n);
            if (VectorLength(n) < 0.001f)
                continue;
            float w = DotProduct(n, &deluxeFloats[idx * 3]);
            if (w < 0.01f) w = 0.01f;
            lightFloats[idx * 3 + 0] /= w;
            lightFloats[idx * 3 + 1] /= w;
            lightFloats[idx * 3 + 2] /= w;
        }
    }

    if (game->hdr == HDR_8BIT)
    {
        const char *existingIntensity = ValueForKey(&entities[0], "_lightingIntensity");
        float customIntensity = existingIntensity[0] ? atof(existingIntensity) : 0.0f;

        if (customIntensity > 1.0f)
        {
            // Respect custom intensity: Scale pixels by 1/Intensity to match engine boost
            _printf("Custom _lightingIntensity detected (%f), using as fixed scale.\n", customIntensity);
            scale = customIntensity;
        }
        else
        {
            // No custom intensity: Apply fixed normalization from game profile
            maxLightIntensity = 255.0f * game->hdr8BitScale;
            scale = 255.0f / maxLightIntensity;
            float engineIntensity = maxLightIntensity / 255.0f;

            _printf("LightingIntensity Fixed Normalization: Scale %f (_lightingIntensity %f)\n", scale, engineIntensity);
            SetKeyValue(&entities[0], "_lightingIntensity", va("%f", engineIntensity));
        }
    }

    if (!lightBytes)
    {
        lightBytes = malloc(numLightBytes);
        if (!lightBytes && numLightBytes > 0)
        {
            Error("Failed to allocate %d bytes for lightBytes during DownConvert", numLightBytes);
        }
        if (lightBytes)
        {
            memset(lightBytes, 0, numLightBytes);
        }
    }

    DownConvertDrawVerts(scale, (game->hdr == HDR_8BIT));
    DownConvertLightmaps(scale, (game->hdr == HDR_8BIT));
    DownConvertDeluxeMaps();
    DownConvertGrid(scale, (game->hdr == HDR_8BIT));

    _printf("DownConvert: Done\n");

    // Meticulous cleanup: free all high-precision processing buffers now that 
    // the data has been successfully down-converted to 8-bit for export.
    if (internalDrawVerts) { free(internalDrawVerts); internalDrawVerts = NULL; }
    if (lightFloats)       { free(lightFloats);       lightFloats = NULL; }
    if (deluxeFloats)      { free(deluxeFloats);      deluxeFloats = NULL; }
    if (energyFloats)      { free(energyFloats);      energyFloats = NULL; }
    if (normalFloats)      { free(normalFloats);      normalFloats = NULL; }
    if (lightAlphaMask)    { free(lightAlphaMask);    lightAlphaMask = NULL; }
    if (lightSurfaceIndex) { free(lightSurfaceIndex); lightSurfaceIndex = NULL; }
    if (gridData32)        { free(gridData32);        gridData32 = NULL; }
}

void AllocateRadiosityFloats(void)
{
    if (numLightBytes <= 0)
    {
        _printf("AllocateRadiosityFloats: ERROR! numLightBytes is %d\n", numLightBytes);
        return;
    }

    if (radiosityFloats) free(radiosityFloats);
    if (accumRadiosityFloats) free(accumRadiosityFloats);
    if (radiosityDeluxeFloats) free(radiosityDeluxeFloats);
    if (radiosityEnergyFloats) free(radiosityEnergyFloats);
    if (accumRadiosityDeluxeSum) free(accumRadiosityDeluxeSum);
    if (accumRadiosityEnergyFloats) free(accumRadiosityEnergyFloats);

    radiosityFloats = malloc((numLightBytes / 3) * sizeof(vec3_t));
    if (!radiosityFloats)
        Error("AllocateRadiosityFloats: malloc failed (radiosity). numLightBytes: %d", numLightBytes);
    memset(radiosityFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));

    accumRadiosityFloats = malloc((numLightBytes / 3) * sizeof(vec3_t));
    if (!accumRadiosityFloats)
        Error("AllocateRadiosityFloats: malloc failed (accum). numLightBytes: %d", numLightBytes);
    memset(accumRadiosityFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));

    if (game->deluxeMap)
    {
        radiosityDeluxeFloats = malloc((numLightBytes / 3) * sizeof(vec3_t));
        if (!radiosityDeluxeFloats) Error("AllocateRadiosityFloats: malloc failed (radiosityDeluxe).");
        memset(radiosityDeluxeFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));

        radiosityEnergyFloats = malloc((numLightBytes / 3) * sizeof(vec3_t));
        if (!radiosityEnergyFloats) Error("AllocateRadiosityFloats: malloc failed (radiosityEnergy).");
        memset(radiosityEnergyFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));

        accumRadiosityDeluxeSum = malloc((numLightBytes / 3) * sizeof(vec3_t));
        if (!accumRadiosityDeluxeSum) Error("AllocateRadiosityFloats: malloc failed (accumRadiosityDeluxe).");
        memset(accumRadiosityDeluxeSum, 0, (numLightBytes / 3) * sizeof(vec3_t));

        accumRadiosityEnergyFloats = malloc((numLightBytes / 3) * sizeof(vec3_t));
        if (!accumRadiosityEnergyFloats) Error("AllocateRadiosityFloats: malloc failed (accumRadiosityEnergy).");
        memset(accumRadiosityEnergyFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));
    }

    {
        int pixels = numLightBytes / 3;
        float megabytes = (float)(pixels * sizeof(vec3_t) * (game->deluxeMap ? 6 : 2)) / (1024.0f * 1024.0f);
        _printf("  AllocateRadiosityFloats: %d pixels allocated (%.1f MB)\n", pixels, megabytes);
    }
}

void FreeRadiosityFloats(void)
{
    if (radiosityFloats)
    {
        free(radiosityFloats);
        radiosityFloats = NULL;
    }
    if (accumRadiosityFloats)
    {
        free(accumRadiosityFloats);
        accumRadiosityFloats = NULL;
    }
    if (radiosityDeluxeFloats)
    {
        free(radiosityDeluxeFloats);
        radiosityDeluxeFloats = NULL;
    }
    if (radiosityEnergyFloats)
    {
        free(radiosityEnergyFloats);
        radiosityEnergyFloats = NULL;
    }
    if (accumRadiosityDeluxeSum)
    {
        free(accumRadiosityDeluxeSum);
        accumRadiosityDeluxeSum = NULL;
    }
    if (accumRadiosityEnergyFloats)
    {
        free(accumRadiosityEnergyFloats);
        accumRadiosityEnergyFloats = NULL;
    }
}

/*
===============================================================================

VOXEL CACHE SERVICE

===============================================================================
*/

void VoxelCache_BakeAll(void)
{
    Q_mkdir("cache");

    int numBaked = 0;
    double start = I_FloatTime();

    if (g_fast)
    {
        _printf("--- VoxelCache_BakeAll (FAST/Rasterized) ---\n");
    }
    else
    {
        _printf("--- VoxelCache_BakeAll (FULL/Sampled) ---\n");
    }

    _printf("    [baking] ");
    fflush(stdout);

#pragma omp parallel for reduction(+ : numBaked) schedule(dynamic)
    for (int i = 0; i < numDrawSurfaces; i++)
    {
        dsurface_t *ds = &drawSurfaces[i];
        if (ds->surfaceType != MST_TRIANGLE_SOUP || ds->lightmapNum[0] < 0)
            continue;

        char path[256];
        sprintf(path, "cache/surf_%d.vxl", i);
        FILE *f_test = fopen(path, "rb");
        if (f_test)
        {
            fclose(f_test);
#pragma omp critical
            {
                _printf(".");
                fflush(stdout);
            }
            continue;
        }

        int W = ds->lightmapWidth;
        int H = ds->lightmapHeight;

        voxelPoint_t *grid = calloc(W * H, sizeof(voxelPoint_t));
        byte *gridValid = calloc(W * H, sizeof(byte));

        if (g_fast)
        {
            // --- Optimized Path: Integrated Rasterization ---
            for (int j = 0; j < ds->numIndexes; j += 3)
            {
                int i0 = drawIndexes[ds->firstIndex + j];
                int i1 = drawIndexes[ds->firstIndex + j + 1];
                int i2 = drawIndexes[ds->firstIndex + j + 2];

                drawVert_t *v[3] = {
                    &drawVerts[ds->firstVert + i0],
                    &drawVerts[ds->firstVert + i1],
                    &drawVerts[ds->firstVert + i2]};

                float st[3][2];
                for (int k = 0; k < 3; k++)
                {
                    st[k][0] = v[k]->lightmap[0][0] * LIGHTMAP_WIDTH - ds->lightmapOffset[0][0];
                    st[k][1] = v[k]->lightmap[0][1] * LIGHTMAP_HEIGHT - ds->lightmapOffset[0][1];
                }

                // Find triangle bounds in local lightmap space and expand by 2 texels for dilation
                float mins[2], maxs[2];
                mins[0] = st[0][0];
                mins[1] = st[0][1];
                maxs[0] = st[0][0];
                maxs[1] = st[0][1];
                for (int k = 1; k < 3; k++)
                {
                    if (st[k][0] < mins[0])
                        mins[0] = st[k][0];
                    if (st[k][1] < mins[1])
                        mins[1] = st[k][1];
                    if (st[k][0] > maxs[0])
                        maxs[0] = st[k][0];
                    if (st[k][1] > maxs[1])
                        maxs[1] = st[k][1];
                }

                int minX = (int)floorf(mins[0] - 0.5f) - 2;
                int minY = (int)floorf(mins[1] - 0.5f) - 2;
                int maxX = (int)ceilf(maxs[0] + 0.5f) + 2;
                int maxY = (int)ceilf(maxs[1] + 0.5f) + 2;

                if (minX < 0)
                    minX = 0;
                if (minY < 0)
                    minY = 0;
                if (maxX >= W)
                    maxX = W - 1;
                if (maxY >= H)
                    maxY = H - 1;

                // Pre-calculate distance thresholds for barycentric dilation (2 texels)
                float area = (st[1][1] - st[2][1]) * (st[0][0] - st[2][0]) + (st[2][0] - st[1][0]) * (st[0][1] - st[2][1]);
                if (fabsf(area) < 0.0001f)
                    continue;

                float thresholds[3];
                for (int k = 0; k < 3; k++)
                {
                    int k1 = (k + 1) % 3;
                    int k2 = (k + 2) % 3;
                    float dx = st[k1][0] - st[k2][0];
                    float dy = st[k1][1] - st[k2][1];
                    float edgeLen = sqrtf(dx * dx + dy * dy);
                    float height = (edgeLen > 0.001f) ? (fabsf(area) / edgeLen) : 1.0f;
                    thresholds[k] = -2.0f / height; // 2-texel outward threshold
                }

                for (int ty = minY; ty <= maxY; ty++)
                {
                    for (int tx = minX; tx <= maxX; tx++)
                    {
                        int pIdx = ty * W + tx;
                        if (gridValid[pIdx])
                            continue;

                        float pST[2] = {(float)tx + 0.5f, (float)ty + 0.5f};
                        float w0 = ((st[1][1] - st[2][1]) * (pST[0] - st[2][0]) + (st[2][0] - st[1][0]) * (pST[1] - st[2][1])) / area;
                        float w1 = ((st[2][1] - st[0][1]) * (pST[0] - st[2][0]) + (st[0][0] - st[2][0]) * (pST[1] - st[2][1])) / area;
                        float w2 = 1.0f - w0 - w1;

                        if (w0 >= thresholds[0] && w1 >= thresholds[1] && w2 >= thresholds[2])
                        {
                            gridValid[pIdx] = 1;
                            for (int k = 0; k < 3; k++)
                            {
                                grid[pIdx].pos[k] = w0 * v[0]->xyz[k] + w1 * v[1]->xyz[k] + w2 * v[2]->xyz[k];
                                grid[pIdx].normal[k] = w0 * v[0]->normal[k] + w1 * v[1]->normal[k] + w2 * v[2]->normal[k];
                            }
                            VectorNormalize(grid[pIdx].normal, grid[pIdx].normal);
                            grid[pIdx].pixelIndex = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ty) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + tx;
                        }
                    }
                }
            }
        }
        else
        {
            // --- High-Fidelity Path: Sampled Iteration ---
            for (int ty = 0; ty < H; ty++)
            {
                for (int tx = 0; tx < W; tx++)
                {
                    int pIdx = ty * W + tx;
                    float st[2];
                    st[0] = (float)ds->lightmapOffset[0][0] + (float)tx + 0.5f;
                    st[1] = (float)ds->lightmapOffset[0][1] + (float)ty + 0.5f;

                    if (TriSoupSamplePoint(ds, st, grid[pIdx].pos, grid[pIdx].normal))
                    {
                        gridValid[pIdx] = 1;
                        grid[pIdx].pixelIndex = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ty) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + tx;
                    }
                }
            }
        }

        // Collect and save
        int validCount = 0;
        for (int j = 0; j < W * H; j++)
            if (gridValid[j])
                validCount++;

        if (validCount > 0)
        {
            voxelPoint_t *points = malloc(validCount * sizeof(voxelPoint_t));
            int outIdx = 0;
            for (int j = 0; j < W * H; j++)
            {
                if (gridValid[j])
                {
                    points[outIdx++] = grid[j];
                }
            }

            FILE *f = fopen(path, "wb");
            if (f)
            {
                int magic = 0x4C584F56;
                int version = 1;
                fwrite(&magic, 4, 1, f);
                fwrite(&version, 4, 1, f);
                fwrite(&validCount, 4, 1, f);
                fwrite(points, sizeof(voxelPoint_t), validCount, f);
                fclose(f);
                numBaked++;
            }
            free(points);
        }
        free(grid);
        free(gridValid);

#pragma omp critical
        {
            _printf(".");
            fflush(stdout);
        }
    }

    _printf("\n");

    double end = I_FloatTime();
    _printf("    %d Trisoup surfaces baked to cache in %.2f seconds\n", numBaked, end - start);
}

voxelPoint_t *VoxelCache_Load(int surfIdx, int *outNumPoints)
{
    char path[256];
    sprintf(path, "cache/surf_%d.vxl", surfIdx);

    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    int magic, version, numPoints;
    if (fread(&magic, 4, 1, f) != 1 || magic != 0x4C584F56)
    {
        fclose(f);
        return NULL;
    }
    if (fread(&version, 4, 1, f) != 1 || version != 1)
    {
        fclose(f);
        return NULL;
    }
    if (fread(&numPoints, 4, 1, f) != 1)
    {
        fclose(f);
        return NULL;
    }

    voxelPoint_t *points = malloc(numPoints * sizeof(voxelPoint_t));
    if (!points)
    {
        fclose(f);
        return NULL;
    }

    if (fread(points, sizeof(voxelPoint_t), numPoints, f) != numPoints)
    {
        free(points);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *outNumPoints = numPoints;
    return points;
}
