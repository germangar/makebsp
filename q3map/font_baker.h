#ifndef FONT_BAKER_H
#define FONT_BAKER_H

#include "qtypes.h"

// Bakes a TTF font into an atlas texture and JSON descriptor
// fontPath: path to .ttf or .otf file
// atlasSize: power of two size for the texture (e.g. 1024, 2048)
// customFontSize: explicit pixel height, or 0 to auto-fit to maximum crisp size
qboolean BakeFontAtlas(const char *fontPath, int atlasSize, int customFontSize);

#endif /* FONT_BAKER_H */
