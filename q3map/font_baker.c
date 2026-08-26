#include "qbsp.h"
#include "font_baker.h"
#include "imagelib.h"

#define STB_RECT_PACK_IMPLEMENTATION
#include "../libs/stb_rect_pack.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "../libs/stb_truetype.h"

#define ASCII_START 32
#define NUM_CHARS 95 // 32 to 126

/*
================
BakeFontAtlas
================
*/
qboolean BakeFontAtlas(const char *fontPath, int atlasSize, int customFontSize)
{
    byte *fontBuffer = NULL;
    int fontLen;
    stbtt_pack_context spc;
    stbtt_fontinfo fontInfo;
    int i;
    int trySize = 0;
    stbtt_packedchar packedChars[NUM_CHARS];
    char baseName[1024];
    char outTgaPath[1024];
    char outJsonPath[1024];
    byte *atlasAlpha = NULL;
    byte *atlasRGBA = NULL;
    FILE *fJson = NULL;

    _printf("--- Font Atlas Baker ---\n");
    _printf("Loading font: %s\n", fontPath);

    fontLen = LoadFile(fontPath, (void **)&fontBuffer);
    if (fontLen <= 0 || !fontBuffer)
    {
        _printf("ERROR: Could not load font file '%s'\n", fontPath);
        return qfalse;
    }

    if (!stbtt_InitFont(&fontInfo, fontBuffer, stbtt_GetFontOffsetForIndex(fontBuffer, 0)))
    {
        _printf("ERROR: Failed to initialize TrueType font data from '%s'\n", fontPath);
        free(fontBuffer);
        return qfalse;
    }

    if (atlasSize <= 0 || (atlasSize & (atlasSize - 1)) != 0)
    {
        _printf("WARNING: Atlas size %d is not a power of two. Things might look weird.\n", atlasSize);
    }

    atlasAlpha = (byte *)malloc(atlasSize * atlasSize);
    if (!atlasAlpha)
    {
        Error("Out of memory allocating %d bytes for atlas alpha buffer", atlasSize * atlasSize);
    }

    // Auto-fit algorithm
    if (customFontSize <= 0)
    {
        float minSize = 16.0f;
        float maxSize = atlasSize * 0.5f;
        float bestSize = 16.0f;
        int iter;
        stbtt_packedchar tempPackedChars[NUM_CHARS];
        byte *tempAtlasAlpha = (byte *)malloc(atlasSize * atlasSize);

        if (!tempAtlasAlpha) Error("Out of memory");

        _printf("Auto-fitting font size for %dx%d atlas...\n", atlasSize, atlasSize);

        for (iter = 0; iter < 12; iter++)
        {
            float testSize = (minSize + maxSize) * 0.5f;
            
            memset(tempAtlasAlpha, 0, atlasSize * atlasSize);
            stbtt_PackBegin(&spc, tempAtlasAlpha, atlasSize, atlasSize, 0, 2, NULL);
            if (stbtt_PackFontRange(&spc, fontBuffer, 0, testSize, ASCII_START, NUM_CHARS, tempPackedChars))
            {
                bestSize = testSize;
                minSize = testSize; // Fits, try larger
                
                // Backup the successful state
                memcpy(packedChars, tempPackedChars, sizeof(packedChars));
                memcpy(atlasAlpha, tempAtlasAlpha, atlasSize * atlasSize);
            }
            else
            {
                maxSize = testSize; // Doesn't fit, try smaller
            }
            stbtt_PackEnd(&spc);
        }

        trySize = (int)bestSize; // We keep the int just for metrics reporting
        free(tempAtlasAlpha);
        _printf("Calculated optimal font size: %d pixels\n", trySize);
    }
    else
    {
        trySize = customFontSize;
        _printf("Using custom font size: %d pixels\n", trySize);
        
        // Final pack (only if custom size was used, otherwise we already backed up the best fit)
        memset(atlasAlpha, 0, atlasSize * atlasSize);
        stbtt_PackBegin(&spc, atlasAlpha, atlasSize, atlasSize, 0, 2, NULL);
        if (!stbtt_PackFontRange(&spc, fontBuffer, 0, (float)trySize, ASCII_START, NUM_CHARS, packedChars))
        {
            _printf("ERROR: Failed to pack font at size %d into %dx%d atlas.\n", trySize, atlasSize, atlasSize);
            free(fontBuffer);
            free(atlasAlpha);
            return qfalse;
        }
        stbtt_PackEnd(&spc);
    }

    // Get metrics
    float ascent, descent, lineGap;
    stbtt_GetScaledFontVMetrics(fontBuffer, 0, (float)trySize, &ascent, &descent, &lineGap);

    ExtractFileBase(fontPath, baseName);
    
    char outBasePath[1024];
    strcpy(outBasePath, fontPath);
    StripExtension(outBasePath);
    
    sprintf(outTgaPath, "%s.tga", outBasePath);
    sprintf(outJsonPath, "%s.font", outBasePath);

    // Convert to RGBA
    _printf("Converting to 32-bit RGBA and writing TGA...\n");
    atlasRGBA = (byte *)malloc(atlasSize * atlasSize * 4);
    if (!atlasRGBA)
    {
        Error("Out of memory allocating RGBA buffer");
    }

    for (i = 0; i < atlasSize * atlasSize; i++)
    {
        atlasRGBA[i * 4 + 0] = 255; // R
        atlasRGBA[i * 4 + 1] = 255; // G
        atlasRGBA[i * 4 + 2] = 255; // B
        atlasRGBA[i * 4 + 3] = atlasAlpha[i]; // A
    }

    SaveTGA(outTgaPath, atlasRGBA, atlasSize, atlasSize, 4);
    
    free(atlasAlpha);
    free(atlasRGBA);
    free(fontBuffer);

    // Write JSON descriptor
    _printf("Writing descriptor to %s...\n", outJsonPath);
    fJson = fopen(outJsonPath, "w");
    if (!fJson)
    {
        Error("Failed to open %s for writing", outJsonPath);
    }

    fprintf(fJson, "{\n");
    fprintf(fJson, "  \"font\": \"%s\",\n", baseName);
    fprintf(fJson, "  \"texture\": \"%s\",\n", baseName);
    fprintf(fJson, "  \"atlasWidth\": %d,\n", atlasSize);
    fprintf(fJson, "  \"atlasHeight\": %d,\n", atlasSize);
    fprintf(fJson, "  \"fontSize\": %d.0,\n", trySize);
    fprintf(fJson, "  \"ascent\": %f,\n", ascent);
    fprintf(fJson, "  \"descent\": %f,\n", descent);
    fprintf(fJson, "  \"lineGap\": %f,\n", lineGap);
    fprintf(fJson, "  \"firstChar\": %d,\n", ASCII_START);
    fprintf(fJson, "  \"numChars\": %d,\n", NUM_CHARS);
    fprintf(fJson, "  \"glyphs\": [\n");

    for (i = 0; i < NUM_CHARS; i++)
    {
        stbtt_packedchar *pc = &packedChars[i];
        int code = ASCII_START + i;
        char charStr[2] = { (char)code, '\0' };
        
        // Escape json characters if necessary (only \ and " need escaping for standard ASCII)
        char escapedChar[4] = {0};
        if (code == '\\') strcpy(escapedChar, "\\\\");
        else if (code == '"') strcpy(escapedChar, "\\\"");
        else strcpy(escapedChar, charStr);

        float u0 = (float)pc->x0 / (float)atlasSize;
        float v0 = (float)pc->y0 / (float)atlasSize;
        float u1 = (float)pc->x1 / (float)atlasSize;
        float v1 = (float)pc->y1 / (float)atlasSize;
        
        int w = pc->x1 - pc->x0;
        int h = pc->y1 - pc->y0;

        fprintf(fJson, "    {\n");
        fprintf(fJson, "      \"char\": \"%s\",\n", escapedChar);
        fprintf(fJson, "      \"code\": %d,\n", code);
        fprintf(fJson, "      \"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d,\n", pc->x0, pc->y0, w, h);
        fprintf(fJson, "      \"u0\": %.8f, \"v0\": %.8f, \"u1\": %.8f, \"v1\": %.8f,\n", u0, v0, u1, v1);
        fprintf(fJson, "      \"xoff\": %.4f, \"yoff\": %.4f,\n", pc->xoff, pc->yoff);
        fprintf(fJson, "      \"xadvance\": %.4f\n", pc->xadvance);
        
        if (i < NUM_CHARS - 1)
            fprintf(fJson, "    },\n");
        else
            fprintf(fJson, "    }\n");
    }

    fprintf(fJson, "  ]\n");
    fprintf(fJson, "}\n");
    
    fclose(fJson);

    _printf("SUCCESS: Baked %d glyphs to %dx%d atlas.\n", NUM_CHARS, atlasSize, atlasSize);
    return qtrue;
}
