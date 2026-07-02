/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "imagelib.h"
#include "cmdlib.h"
#include <assert.h>

#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define DDSKTX_IMPLEMENT
#include "../libs/dds-ktx.h"

#define BCDEC_IMPLEMENTATION
#include "../libs/bcdec.h"

#define ETCDEC_IMPLEMENTATION
#include "../libs/etcdec.h"

/*
============================================================================

LOAD IMAGE

============================================================================
*/

void Q_LoadImage(const char *name, byte **pixels, int *width, int *height)
{
    int x, y, n;
    char ext[128];

    ExtractFileExtension(name, ext);
    if (!Q_stricmp(ext, "lbm") || !Q_stricmp(ext, "pcx"))
    {
        Error("LoadImage: %s format is no longer supported. Please convert to TGA, PNG, or BMP.", ext);
    }

    stbi_uc *data = stbi_load(name, &x, &y, &n, 4);
    if (!data)
    {
        Error("Failed to load image: %s (%s)", name, stbi_failure_reason());
    }
    if (width)
        *width = x;
    if (height)
        *height = y;
    *pixels = (byte *)data;
}

void LoadTGA(const char *filename, byte **pixels, int *width, int *height)
{
    Q_LoadImage(filename, pixels, width, height);
}

void LoadBMP(const char *filename, byte **pixels, int *width, int *height)
{
    Q_LoadImage(filename, pixels, width, height);
}

void LoadPNG(const char *filename, byte **pixels, int *width, int *height)
{
    Q_LoadImage(filename, pixels, width, height);
}

void Load32BitImage(const char *name, unsigned **pixels, int *width, int *height)
{
    Q_LoadImage(name, (byte **)pixels, width, height);
}

void LoadImageFromBuffer(byte *buffer, int buflen, byte **pixels, int *width, int *height)
{
    int x, y, n;
    stbi_uc *data = stbi_load_from_memory(buffer, buflen, &x, &y, &n, 4);
    if (!data)
    {
        Error("Failed to load image from buffer: %s", stbi_failure_reason());
    }
    if (width)
        *width = x;
    if (height)
        *height = y;
    *pixels = (byte *)data;
}

void LoadKTXFromMemory(byte *buffer, int buflen, byte **pixels, int *width, int *height)
{
    ddsktx_texture_info tc = {0};
    ddsktx_error err;

    if (!ddsktx_parse(&tc, buffer, buflen, &err))
    {
        Error("Failed to parse KTX image buffer: %s", err.msg);
    }

    if (width) *width = tc.width;
    if (height) *height = tc.height;

    int w = tc.width;
    int h = tc.height;
    if (w <= 0 || h <= 0)
    {
        Error("KTX image has invalid dimensions: %d x %d", w, h);
    }

    ddsktx_sub_data sub_data;
    ddsktx_get_sub(&tc, &sub_data, buffer, buflen, 0, 0, 0);
    if (!sub_data.buff)
    {
        Error("Failed to get sub-data for KTX image");
    }

    byte *out_pixels = malloc(w * h * 4);
    if (!out_pixels)
    {
        Error("Out of memory allocating %d bytes for KTX image", w * h * 4);
    }

    if (tc.format == DDSKTX_FORMAT_RGBA8)
    {
        memcpy(out_pixels, sub_data.buff, w * h * 4);
    }
    else if (tc.format == DDSKTX_FORMAT_BGRA8)
    {
        const byte *src = (const byte *)sub_data.buff;
        for (int i = 0; i < w * h; i++)
        {
            out_pixels[i * 4 + 0] = src[i * 4 + 2];
            out_pixels[i * 4 + 1] = src[i * 4 + 1];
            out_pixels[i * 4 + 2] = src[i * 4 + 0];
            out_pixels[i * 4 + 3] = src[i * 4 + 3];
        }
    }
    else if (tc.format == DDSKTX_FORMAT_RGB8)
    {
        const byte *src = (const byte *)sub_data.buff;
        for (int i = 0; i < w * h; i++)
        {
            out_pixels[i * 4 + 0] = src[i * 3 + 0];
            out_pixels[i * 4 + 1] = src[i * 3 + 1];
            out_pixels[i * 4 + 2] = src[i * 3 + 2];
            out_pixels[i * 4 + 3] = 255;
        }
    }
    else if (tc.format == DDSKTX_FORMAT_RG8)
    {
        const byte *src = (const byte *)sub_data.buff;
        for (int i = 0; i < w * h; i++)
        {
            out_pixels[i * 4 + 0] = src[i * 2 + 0]; // L -> R
            out_pixels[i * 4 + 1] = src[i * 2 + 0]; // L -> G
            out_pixels[i * 4 + 2] = src[i * 2 + 0]; // L -> B
            out_pixels[i * 4 + 3] = src[i * 2 + 1]; // A -> A
        }
    }
    else if (tc.format == DDSKTX_FORMAT_R8)
    {
        const byte *src = (const byte *)sub_data.buff;
        for (int i = 0; i < w * h; i++)
        {
            out_pixels[i * 4 + 0] = src[i];
            out_pixels[i * 4 + 1] = src[i];
            out_pixels[i * 4 + 2] = src[i];
            out_pixels[i * 4 + 3] = 255;
        }
    }
    else if (tc.format == DDSKTX_FORMAT_BC1 || tc.format == DDSKTX_FORMAT_BC2 ||
             tc.format == DDSKTX_FORMAT_BC3 || tc.format == DDSKTX_FORMAT_BC4 ||
             tc.format == DDSKTX_FORMAT_BC5 || tc.format == DDSKTX_FORMAT_BC7 ||
             tc.format == DDSKTX_FORMAT_ETC1 || tc.format == DDSKTX_FORMAT_ETC2 ||
             tc.format == DDSKTX_FORMAT_ETC2A1 || tc.format == DDSKTX_FORMAT_ETC2A)
    {
        int blocks_x = (w + 3) / 4;
        int blocks_y = (h + 3) / 4;
        int block_size;
        
        if (tc.format == DDSKTX_FORMAT_BC1 || tc.format == DDSKTX_FORMAT_BC4 || 
            tc.format == DDSKTX_FORMAT_ETC1 || tc.format == DDSKTX_FORMAT_ETC2 || tc.format == DDSKTX_FORMAT_ETC2A1) {
            block_size = 8;
        } else {
            block_size = 16;
        }
        
        const byte *src_blocks = (const byte *)sub_data.buff;

        for (int by = 0; by < blocks_y; by++)
        {
            for (int bx = 0; bx < blocks_x; bx++)
            {
                const byte *compressed_block = src_blocks + (by * blocks_x + bx) * block_size;
                byte temp_block[4 * 4 * 4];

                if (tc.format == DDSKTX_FORMAT_BC1)
                {
                    bcdec_bc1(compressed_block, temp_block, 4 * 4);
                }
                else if (tc.format == DDSKTX_FORMAT_BC2)
                {
                    bcdec_bc2(compressed_block, temp_block, 4 * 4);
                }
                else if (tc.format == DDSKTX_FORMAT_BC3)
                {
                    bcdec_bc3(compressed_block, temp_block, 4 * 4);
                }
                else if (tc.format == DDSKTX_FORMAT_BC4)
                {
                    byte bc4_temp[16];
                    bcdec_bc4(compressed_block, bc4_temp, 4);
                    for (int i = 0; i < 16; i++)
                    {
                        temp_block[i * 4 + 0] = bc4_temp[i];
                        temp_block[i * 4 + 1] = bc4_temp[i];
                        temp_block[i * 4 + 2] = bc4_temp[i];
                        temp_block[i * 4 + 3] = 255;
                    }
                }
                else if (tc.format == DDSKTX_FORMAT_BC5)
                {
                    byte bc5_temp[32];
                    bcdec_bc5(compressed_block, bc5_temp, 4 * 2);
                    for (int i = 0; i < 16; i++)
                    {
                        temp_block[i * 4 + 0] = bc5_temp[i * 2 + 0];
                        temp_block[i * 4 + 1] = bc5_temp[i * 2 + 1];
                        temp_block[i * 4 + 2] = 0;
                        temp_block[i * 4 + 3] = 255;
                    }
                }
                else if (tc.format == DDSKTX_FORMAT_BC7)
                {
                    bcdec_bc7(compressed_block, temp_block, 4 * 4);
                }
                else if (tc.format == DDSKTX_FORMAT_ETC1 || tc.format == DDSKTX_FORMAT_ETC2)
                {
                    etcdec_etc_rgb(compressed_block, temp_block, 4 * 4);
                }
                else if (tc.format == DDSKTX_FORMAT_ETC2A1)
                {
                    etcdec_etc_rgb_a1(compressed_block, temp_block, 4 * 4);
                }
                else if (tc.format == DDSKTX_FORMAT_ETC2A)
                {
                    etcdec_eac_rgba(compressed_block, temp_block, 4 * 4);
                }

                for (int py = 0; py < 4; py++)
                {
                    int y_coord = by * 4 + py;
                    if (y_coord >= h) continue;

                    for (int px = 0; px < 4; px++)
                    {
                        int x_coord = bx * 4 + px;
                        if (x_coord >= w) continue;

                        int dst_idx = (y_coord * w + x_coord) * 4;
                        int src_idx = (py * 4 + px) * 4;
                        out_pixels[dst_idx + 0] = temp_block[src_idx + 0];
                        out_pixels[dst_idx + 1] = temp_block[src_idx + 1];
                        out_pixels[dst_idx + 2] = temp_block[src_idx + 2];
                        out_pixels[dst_idx + 3] = temp_block[src_idx + 3];
                    }
                }
            }
        }
    }
    else
    {
        free(out_pixels);
        Error("Unsupported KTX image format (%s)", ddsktx_format_str(tc.format));
    }

    *pixels = out_pixels;
}

/*
============================================================================

SAVE IMAGE

============================================================================
*/

void SaveImage(const char *name, byte *pixels, int width, int height, int components)
{
    char ext[128];
    ExtractFileExtension(name, ext);

    if (!Q_stricmp(ext, "tga"))
    {
        SaveTGA(name, pixels, width, height, components);
    }
    else if (!Q_stricmp(ext, "bmp"))
    {
        SaveBMP(name, pixels, width, height, components);
    }
    else if (!Q_stricmp(ext, "png"))
    {
        SavePNG(name, pixels, width, height, components);
    }
    else
    {
        Error("SaveImage: Unknown extension '%s' for file '%s'", ext, name);
    }
}

void SaveTGA(const char *filename, byte *pixels, int width, int height, int components)
{
    if (!stbi_write_tga(filename, width, height, components, pixels))
    {
        Error("Failed to write TGA: %s", filename);
    }
}

void SaveBMP(const char *filename, byte *pixels, int width, int height, int components)
{
    if (!stbi_write_bmp(filename, width, height, components, pixels))
    {
        Error("Failed to write BMP: %s", filename);
    }
}

void SavePNG(const char *filename, byte *pixels, int width, int height, int components)
{
    if (!stbi_write_png(filename, width, height, components, pixels, width * components))
    {
        Error("Failed to write PNG: %s", filename);
    }
}
