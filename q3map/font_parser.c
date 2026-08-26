#include "qbsp.h"
#include "font_parser.h"
#include "../libs/json.h"

static fontDescriptor_t *loadedFonts = NULL;

static void StripJSONComments(char *buffer, int len)
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

/*
================
TryLoadFontBuffer
================
*/
static int TryLoadFontBuffer(const char *fontPath, void **buffer)
{
    char path[1024];
    char base[1024];
    int len;

    // 1. Direct path
    strcpy(path, fontPath);
    if (!strstr(path, ".font"))
        strcat(path, ".font");

    len = vfsLoadFile(path, buffer);
    if (len > 0) return len;

    len = TryLoadFile(path, buffer);
    if (len > 0) return len;

    ExtractFileBase(fontPath, base);

    // 2. textures/decals/fonts/<basename>.font
    sprintf(path, "textures/decals/fonts/%s.font", base);
    len = vfsLoadFile(path, buffer);
    if (len > 0) return len;

    len = TryLoadFile(path, buffer);
    if (len > 0) return len;

    // 3. fonts/<basename>.font
    sprintf(path, "fonts/%s.font", base);
    len = vfsLoadFile(path, buffer);
    if (len > 0) return len;

    len = TryLoadFile(path, buffer);
    if (len > 0) return len;

    return -1;
}

/*
================
LoadFontDescriptor
================
*/
fontDescriptor_t *LoadFontDescriptor(const char *fontPath)
{
    fontDescriptor_t *f;
    char baseName[128];
    void *buffer = NULL;
    int len;
    struct json_value_s *root;
    struct json_object_s *rootObj;
    struct json_object_element_s *el;

    if (!fontPath || !fontPath[0])
        return NULL;

    ExtractFileBase(fontPath, baseName);

    // Check cache
    for (f = loadedFonts; f; f = f->next)
    {
        if (!Q_stricmp(f->name, baseName) || !Q_stricmp(f->name, fontPath))
        {
            return f;
        }
    }

    len = TryLoadFontBuffer(fontPath, &buffer);
    if (len <= 0 || !buffer)
    {
        _printf("WARNING: Could not load font descriptor for '%s'\n", fontPath);
        return NULL;
    }

    StripJSONComments((char *)buffer, len);
    root = json_parse(buffer, (size_t)len);
    free(buffer);

    if (!root)
    {
        _printf("ERROR: Failed to parse JSON in font descriptor for '%s'\n", fontPath);
        return NULL;
    }

    rootObj = json_value_as_object(root);
    if (!rootObj)
    {
        _printf("ERROR: Root of font descriptor '%s' is not an object\n", fontPath);
        free(root);
        return NULL;
    }

    f = (fontDescriptor_t *)malloc(sizeof(fontDescriptor_t));
    memset(f, 0, sizeof(fontDescriptor_t));
    snprintf(f->name, sizeof(f->name), "%s", baseName);
    f->fontSize = 32.0f; // default fallback

    for (el = rootObj->start; el; el = el->next)
    {
        const char *key = el->name->string;
        struct json_value_s *val = el->value;

        if (!Q_stricmp(key, "font") && val->type == json_type_string)
        {
            strncpy(f->name, json_value_as_string(val)->string, sizeof(f->name) - 1);
        }
        else if (!Q_stricmp(key, "texture") && val->type == json_type_string)
        {
            strncpy(f->texture, json_value_as_string(val)->string, sizeof(f->texture) - 1);
        }
        else if (!Q_stricmp(key, "atlasWidth") && val->type == json_type_number)
        {
            f->atlasWidth = atoi(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "atlasHeight") && val->type == json_type_number)
        {
            f->atlasHeight = atoi(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "fontSize") && val->type == json_type_number)
        {
            f->fontSize = (float)atof(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "ascent") && val->type == json_type_number)
        {
            f->ascent = (float)atof(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "descent") && val->type == json_type_number)
        {
            f->descent = (float)atof(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "lineGap") && val->type == json_type_number)
        {
            f->lineGap = (float)atof(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "firstChar") && val->type == json_type_number)
        {
            f->firstChar = atoi(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "numChars") && val->type == json_type_number)
        {
            f->numChars = atoi(json_value_as_number(val)->number);
        }
        else if (!Q_stricmp(key, "glyphs") && val->type == json_type_array)
        {
            struct json_array_s *arr = json_value_as_array(val);
            if (arr && arr->length > 0)
            {
                struct json_array_element_s *arr_el;
                int gIndex = 0;
                
                f->glyphCount = (int)arr->length;
                f->glyphs = (glyph_t *)malloc(f->glyphCount * sizeof(glyph_t));
                memset(f->glyphs, 0, f->glyphCount * sizeof(glyph_t));

                for (arr_el = arr->start; arr_el && gIndex < f->glyphCount; arr_el = arr_el->next, gIndex++)
                {
                    if (arr_el->value->type == json_type_object)
                    {
                        struct json_object_s *gObj = json_value_as_object(arr_el->value);
                        struct json_object_element_s *gEl;
                        glyph_t *g = &f->glyphs[gIndex];

                        for (gEl = gObj->start; gEl; gEl = gEl->next)
                        {
                            const char *gKey = gEl->name->string;
                            struct json_value_s *gVal = gEl->value;

                            if (!Q_stricmp(gKey, "code") && gVal->type == json_type_number)
                                g->code = atoi(json_value_as_number(gVal)->number);
                            else if (!Q_stricmp(gKey, "x") && gVal->type == json_type_number)
                                g->x = atoi(json_value_as_number(gVal)->number);
                            else if (!Q_stricmp(gKey, "y") && gVal->type == json_type_number)
                                g->y = atoi(json_value_as_number(gVal)->number);
                            else if (!Q_stricmp(gKey, "w") && gVal->type == json_type_number)
                                g->w = atoi(json_value_as_number(gVal)->number);
                            else if (!Q_stricmp(gKey, "h") && gVal->type == json_type_number)
                                g->h = atoi(json_value_as_number(gVal)->number);
                            else if (!Q_stricmp(gKey, "u0") && gVal->type == json_type_number)
                                g->u0 = (float)atof(json_value_as_number(gVal)->number);
                            else if (!Q_stricmp(gKey, "v0") && gVal->type == json_type_number)
                                g->v0 = (float)atof(json_value_as_number(gVal)->number);
                            else if (!Q_stricmp(gKey, "u1") && gVal->type == json_type_number)
                                g->u1 = (float)atof(json_value_as_number(gVal)->number);
                            else if (!Q_stricmp(gKey, "v1") && gVal->type == json_type_number)
                                g->v1 = (float)atof(json_value_as_number(gVal)->number);
                            else if (!Q_stricmp(gKey, "xoff") && gVal->type == json_type_number)
                                g->xoff = (float)atof(json_value_as_number(gVal)->number);
                            else if (!Q_stricmp(gKey, "yoff") && gVal->type == json_type_number)
                                g->yoff = (float)atof(json_value_as_number(gVal)->number);
                            else if (!Q_stricmp(gKey, "xadvance") && gVal->type == json_type_number)
                                g->xadvance = (float)atof(json_value_as_number(gVal)->number);
                        }
                    }
                }
            }
        }
    }

    free(root);

    _printf("Loaded font descriptor '%s' (fontSize: %.1f, %d glyphs)\n", f->name, f->fontSize, f->glyphCount);

    // Insert into cache
    f->next = loadedFonts;
    loadedFonts = f;

    return f;
}

/*
================
FindGlyph
Looks up a glyph by charCode.
Falls back to '^' (ASCII 94), then '?' (ASCII 63).
================
*/
const glyph_t *FindGlyph(const fontDescriptor_t *font, int charCode)
{
    int i;
    const glyph_t *fallbackCaret = NULL;
    const glyph_t *fallbackQuestion = NULL;

    if (!font || !font->glyphs || font->glyphCount <= 0)
        return NULL;

    for (i = 0; i < font->glyphCount; i++)
    {
        if (font->glyphs[i].code == charCode)
        {
            return &font->glyphs[i];
        }
        if (font->glyphs[i].code == '^')
        {
            fallbackCaret = &font->glyphs[i];
        }
        if (font->glyphs[i].code == '?')
        {
            fallbackQuestion = &font->glyphs[i];
        }
    }

    if (fallbackCaret)
        return fallbackCaret;

    if (fallbackQuestion)
        return fallbackQuestion;

    return NULL;
}
