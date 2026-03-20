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

#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/*
============================================================================

LOAD IMAGE

============================================================================
*/

void LoadImage(const char *name, byte **pixels, int *width, int *height) {
  int x, y, n;
  char ext[128];

  ExtractFileExtension(name, ext);
  if (!Q_stricmp(ext, "lbm") || !Q_stricmp(ext, "pcx")) {
    Error("LoadImage: %s format is no longer supported. Please convert to TGA, PNG, or BMP.", ext);
  }

  stbi_uc *data = stbi_load(name, &x, &y, &n, 4);
  if (!data) {
    Error("Failed to load image: %s (%s)", name, stbi_failure_reason());
  }
  if (width) *width = x;
  if (height) *height = y;
  *pixels = (byte *)data;
}

void LoadTGA(const char *filename, byte **pixels, int *width, int *height) {
  LoadImage(filename, pixels, width, height);
}

void LoadBMP(const char *filename, byte **pixels, int *width, int *height) {
  LoadImage(filename, pixels, width, height);
}

void LoadPNG(const char *filename, byte **pixels, int *width, int *height) {
  LoadImage(filename, pixels, width, height);
}

void Load32BitImage(const char *name, unsigned **pixels, int *width, int *height) {
  LoadImage(name, (byte **)pixels, width, height);
}

void LoadImageFromBuffer(byte *buffer, int buflen, byte **pixels, int *width, int *height) {
  int x, y, n;
  stbi_uc *data = stbi_load_from_memory(buffer, buflen, &x, &y, &n, 4);
  if (!data) {
    Error("Failed to load image from buffer: %s", stbi_failure_reason());
  }
  if (width) *width = x;
  if (height) *height = y;
  *pixels = (byte *)data;
}

/*
============================================================================

SAVE IMAGE

============================================================================
*/

void SaveImage(const char *name, byte *pixels, int width, int height, int components) {
  char ext[128];
  ExtractFileExtension(name, ext);

  if (!Q_stricmp(ext, "tga")) {
    SaveTGA(name, pixels, width, height, components);
  } else if (!Q_stricmp(ext, "bmp")) {
    SaveBMP(name, pixels, width, height, components);
  } else if (!Q_stricmp(ext, "png")) {
    SavePNG(name, pixels, width, height, components);
  } else {
    Error("SaveImage: Unknown extension '%s' for file '%s'", ext, name);
  }
}

void SaveTGA(const char *filename, byte *pixels, int width, int height, int components) {
  if (!stbi_write_tga(filename, width, height, components, pixels)) {
    Error("Failed to write TGA: %s", filename);
  }
}

void SaveBMP(const char *filename, byte *pixels, int width, int height, int components) {
  if (!stbi_write_bmp(filename, width, height, components, pixels)) {
    Error("Failed to write BMP: %s", filename);
  }
}

void SavePNG(const char *filename, byte *pixels, int width, int height, int components) {
  if (!stbi_write_png(filename, width, height, components, pixels, width * components)) {
    Error("Failed to write PNG: %s", filename);
  }
}
