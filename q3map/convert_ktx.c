#include "qbsp.h"
#include "cmdlib.h"
#include "imagelib.h"
#include "dds-ktx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ConvertKTX(const char *path)
{
    byte *buffer = NULL;
    int length = LoadFile(path, (void **)&buffer);
    if (!buffer || length <= 0)
    {
        Error("ConvertKTX: Failed to load file '%s'", path);
    }

    byte *pixels = NULL;
    int width = 0;
    int height = 0;

    _printf("Loading KTX image: %s\n", path);
    LoadKTXFromMemory(buffer, length, &pixels, &width, &height);

    if (!pixels || width <= 0 || height <= 0)
    {
        free(buffer);
        Error("ConvertKTX: Failed to parse KTX or invalid dimensions");
    }

    // Determine if the image has an alpha channel
    qboolean hasAlpha = qfalse;
    int numPixels = width * height;
    for (int i = 0; i < numPixels; i++)
    {
        if (pixels[i * 4 + 3] < 255)
        {
            hasAlpha = qtrue;
            break;
        }
    }

    char outPath[1024];
    strcpy(outPath, path);
    StripExtension(outPath);

    if (hasAlpha)
    {
        strcat(outPath, ".tga");
        _printf("Alpha channel detected. Saving as TGA: %s\n", outPath);
        SaveImage(outPath, pixels, width, height, 4);
    }
    else
    {
        strcat(outPath, ".png");
        _printf("No alpha channel detected. Stripping alpha and saving as PNG: %s\n", outPath);
        
        // Pack into 3 bytes per pixel for PNG
        byte *rgb = (byte *)malloc(width * height * 3);
        if (!rgb)
        {
            Error("ConvertKTX: Failed to allocate RGB buffer");
        }
        for (int i = 0; i < numPixels; i++)
        {
            rgb[i * 3 + 0] = pixels[i * 4 + 0];
            rgb[i * 3 + 1] = pixels[i * 4 + 1];
            rgb[i * 3 + 2] = pixels[i * 4 + 2];
        }
        
        SaveImage(outPath, rgb, width, height, 3);
        free(rgb);
    }

    free(pixels);
    free(buffer);
    
    _printf("Conversion successful!\n");
}
