#ifndef FONT_PARSER_H
#define FONT_PARSER_H

#include "qtypes.h"

typedef struct glyph_s {
    int code;
    int x, y, w, h;
    float u0, v0, u1, v1;
    float xoff, yoff;
    float xadvance;
} glyph_t;

typedef struct fontDescriptor_s {
    char name[128];
    char texture[256];
    int atlasWidth;
    int atlasHeight;
    float fontSize;
    float ascent;
    float descent;
    float lineGap;
    int firstChar;
    int numChars;
    glyph_t *glyphs;
    int glyphCount;
    struct fontDescriptor_s *next;
} fontDescriptor_t;

fontDescriptor_t *LoadFontDescriptor(const char *fontPath);
const glyph_t *FindGlyph(const fontDescriptor_t *font, int charCode);

#endif /* FONT_PARSER_H */
