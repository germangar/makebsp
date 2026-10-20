#include "qbsp.h"
#include "cmdlib.h"
#include "imagelib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

/* VFS-aware image load. Non-fatal: returns 0 (with *pixels NULL) instead of
 * aborting on undecodable/unreadable files, so batch runs never die on a
 * single bad asset. */
static int LoadImageVFS(const char *path, byte **pixels, int *w, int *h)
{
    void *buf = NULL;
    *pixels = NULL;
    *w = 0;
    *h = 0;
    int len = vfsLoadFile(path, &buf);
    if (len >= 0 && buf)
    {
        int ok = TryLoadImageFromBuffer((byte *)buf, len, pixels, w, h);
        free(buf);
        return ok;
    }
    return 0;
}

/* Resolve the on-disk path to write a processed image to.
 * Absolute inputs are written in place. VFS-relative inputs are written as a
 * loose file at the same relative path under the writedir, so that files that
 * came from inside a .pk3/.pak end up as loose overrides. */
static void VfsWritePath(const char *path, char *out, int cap)
{
    if (path[0] == '/' || path[0] == '\\' || (path[0] && path[1] == ':'))
    {
        snprintf(out, cap, "%s", path);
        return;
    }
    if (numVFSPaths > 0)
        snprintf(out, cap, "%s%s", vfsPaths[0], path);
    else
        snprintf(out, cap, "%s", path);
}

/* Create the parent directory chain of a file path (best effort). */
static void EnsureParentDir(const char *filePath)
{
    char tmp[2048];
    snprintf(tmp, sizeof(tmp), "%s", filePath);
    char *slash = strrchr(tmp, '/');
    char *bslash = strrchr(tmp, '\\');
    if (bslash > slash) slash = bslash;
    if (!slash) return;
    *slash = 0;
#ifdef _WIN32
    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/' || *p == '\\')
        {
            char c = *p; *p = 0;
            _mkdir(tmp);
            *p = c;
        }
    }
    _mkdir(tmp);
#else
    mkdir(tmp, 0755);
#endif
}

/* Can we overwrite this file? (existing-but-locked/read-only => no) */
static int IsWritable(const char *path)
{
    if (!FileExists(path)) return 1;
    FILE *f = fopen(path, "r+b");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* jpg can be read but has no writer (deliberately: heightmaps and normalmaps
 * should not live in a degrading format). Outputs of this extension are
 * skipped everywhere. */
static int IsJpegPath(const char *path)
{
    const char *base = strrchr(path, '/');
    const char *bbase = strrchr(path, '\\');
    if (bbase > base) base = bbase;
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot || !dot[1]) return 0;
    return !Q_stricmp(dot + 1, "jpg") || !Q_stricmp(dot + 1, "jpeg");
}

/* ------------------------------------------------------------------ */
/* Suffix classification (PrimeXT / Unvanquished / id Tech 4 / OpenMW) */
/* ------------------------------------------------------------------ */

static qboolean IsNormalmapSuffix(const char *suffix)
{
    /* suffix already lower-cased, WITHOUT the leading underscore */
    if (!strcmp(suffix, "norm")) return qtrue;
    if (!strcmp(suffix, "nrm"))  return qtrue;
    if (!strcmp(suffix, "local")) return qtrue;
    if (!strcmp(suffix, "ddn"))  return qtrue;
    if (!strcmp(suffix, "n"))    return qtrue;
    if (!strcmp(suffix, "nh"))   return qtrue;
    return qfalse;
}

static qboolean IsHeightmapSuffix(const char *suffix)
{
    if (!strcmp(suffix, "h"))      return qtrue;
    if (!strcmp(suffix, "height"))  return qtrue;
    if (!strcmp(suffix, "hmap"))   return qtrue;
    if (!strcmp(suffix, "offset"))  return qtrue;
    if (!strcmp(suffix, "disp"))   return qtrue;
    return qfalse;
}

/* Extract the last "_" component of a basename (without extension). Returns "" if none. */
static void LastSuffix(const char *baseNoExt, char *out, int cap)
{
    const char *us = strrchr(baseNoExt, '_');
    out[0] = 0;
    if (!us || !us[1]) return;
    snprintf(out, cap, "%s", us + 1);
    /* lower-case */
    for (int i = 0; out[i]; i++)
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] += 32;
}

/* ------------------------------------------------------------------ */
/* Options                                                            */
/* ------------------------------------------------------------------ */

typedef enum { ANCHOR_TOP, ANCHOR_BOTTOM, ANCHOR_CENTER } anchor_t;

typedef struct
{
    int      hasRange;
    anchor_t rangeAnchor;
    int      rangeLo, rangeHi;
    int      hasChannel;
    char     channel;   /* 'a','r','g','b','l' (luma), 'x' (rgb passthrough) */
    const char *outPath; /* NULL = in-place */
    qboolean embed;
} hm_opts_t;

/* ------------------------------------------------------------------ */
/* Channel helpers                                                    */
/* ------------------------------------------------------------------ */

static int ChannelIndex(const hm_opts_t *o)
{
    /* returns 0..3, or -1 for luma (computed) */
    if (o->channel == 'r') return 0;
    if (o->channel == 'g') return 1;
    if (o->channel == 'b') return 2;
    if (o->channel == 'a') return 3;
    return -1; /* luma */
}

static byte ReadHeight(const byte *px, int idx, const hm_opts_t *o, qboolean isNormalmap)
{
    int ci = ChannelIndex(o);
    if (ci >= 0) return px[idx * 4 + ci];
    /* luma */
    return (byte)((px[idx*4+0]*77 + px[idx*4+1]*150 + px[idx*4+2]*29) >> 8);
}

/* ------------------------------------------------------------------ */
/* Core processing of a single image buffer                           */
/* ------------------------------------------------------------------ */

/* Returns 1 if the buffer was actually modified, 0 if it was left untouched. */
static int ProcessBuffer(byte *pixels, int w, int h, const hm_opts_t *o,
                         anchor_t anchor, qboolean isNormalmap)
{
    int n = w * h;
    /* first pass: min/max of the selected height channel */
    byte mn = 255, mx = 0;
    for (int i = 0; i < n; i++)
    {
        byte v = ReadHeight(pixels, i, o, isNormalmap);
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }

    if (mn == mx)
    {
        _printf("  warning: flat heightmap (min==max==%d), skipping transform\n", mn);
        return 0;
    }

    /* build output height into a temp buffer (0..255) */
    byte *outH = (byte *)malloc(n);
    if (!outH) Error("Out of memory in heightmap processing");

    if (o->hasRange)
    {
        int lo = o->rangeLo, hi = o->rangeHi;
        if (lo < 0) lo = 0;
        if (hi > 255) hi = 255;
        if (lo > hi) { int t = lo; lo = hi; hi = t; }
        for (int i = 0; i < n; i++)
        {
            byte v = ReadHeight(pixels, i, o, isNormalmap);
            int val;
            if (o->rangeAnchor == ANCHOR_TOP)
            {
                /* pin mx -> hi */
                val = hi - (int)((mx - v) * (hi - lo) / (float)(mx - mn));
            }
            else if (o->rangeAnchor == ANCHOR_BOTTOM)
            {
                /* pin mn -> lo */
                val = lo + (int)((v - mn) * (hi - lo) / (float)(mx - mn));
            }
            else /* CENTER */
            {
                float mid = (mn + mx) * 0.5f;
                float halfRange = (hi - lo) * 0.5f;
                float centre = (lo + hi) * 0.5f;
                val = (int)(centre + (v - mid) * (halfRange / ((mx - mn) * 0.5f)));
            }
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            outH[i] = (byte)val;
        }
    }
    else
    {
        for (int i = 0; i < n; i++)
        {
            byte v = ReadHeight(pixels, i, o, isNormalmap);
            int val;
            if (anchor == ANCHOR_TOP)
                val = v + (255 - mx);
            else if (anchor == ANCHOR_BOTTOM)
                val = v - mn;
            else
                val = (int)(v - (mn + mx) * 0.5f + 127.5f);
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            outH[i] = (byte)val;
        }
    }

    /* write back */
    int ci = ChannelIndex(o);
    int changed = 0;
    if (isNormalmap)
    {
        /* preserve RGB, overwrite selected channel (default alpha) */
        int ch = (ci < 0) ? 3 : ci;
        for (int i = 0; i < n; i++)
        {
            if (pixels[i * 4 + ch] != outH[i]) changed = 1;
            pixels[i * 4 + ch] = outH[i];
        }
    }
    else
    {
        if (ci < 0)
        {
            /* luma default: write greyscale to all RGB, keep alpha */
            for (int i = 0; i < n; i++)
            {
                if (pixels[i*4+0] != outH[i]) changed = 1;
                pixels[i*4+0] = outH[i];
                pixels[i*4+1] = outH[i];
                pixels[i*4+2] = outH[i];
            }
        }
        else
        {
            for (int i = 0; i < n; i++)
            {
                if (pixels[i * 4 + ci] != outH[i]) changed = 1;
                pixels[i * 4 + ci] = outH[i];
            }
        }
    }

    free(outH);
    return changed;
}

/* ------------------------------------------------------------------ */
/* Suffix-based normalmap detection                                   */
/* ------------------------------------------------------------------ */

static qboolean DetectNormalmap(const char *path)
{
    char base[1024];
    strcpy(base, path);
    /* strip directory */
    char *slash = strrchr(base, '/');
    if (!slash) slash = strrchr(base, '\\');
    char *name = slash ? slash + 1 : base;
    char noext[1024];
    strcpy(noext, name);
    char *dot = strrchr(noext, '.');
    if (dot) *dot = 0;

    char suf[64];
    LastSuffix(noext, suf, sizeof(suf));
    if (IsNormalmapSuffix(suf)) return qtrue;

    /* fallback: inspect pixels */
    byte *pixels = NULL;
    int w = 0, h = 0;
    LoadImageVFS(path, &pixels, &w, &h);
    if (!pixels || w <= 0 || h <= 0) return qfalse;

    int n = w * h;
    /* alpha non-trivial? */
    int alphaVar = 0;
    for (int i = 0; i < n; i++)
        if (pixels[i*4+3] != 255) { alphaVar = 1; break; }
    if (!alphaVar) { free(pixels); return qfalse; }

    /* RGB clustered around 128, B near 255 */
    long sr=0, sg=0, sb=0;
    for (int i = 0; i < n; i++) { sr += pixels[i*4+0]; sg += pixels[i*4+1]; sb += pixels[i*4+2]; }
    int mr = sr/n, mg = sg/n, mb = sb/n;
    free(pixels);
    if (mb >= 200 && mr > 80 && mr < 180 && mg > 80 && mg < 180) return qtrue;
    return qfalse;
}

/* ------------------------------------------------------------------ */
/* Embed into sibling normalmap (same directory)                      */
/* ------------------------------------------------------------------ */

static const char *NORMALMAP_EXTS[] = { "png", "tga", "bmp", "dds", NULL };

static void FindSiblingNormalmap(const char *heightPath, char *outPath, int cap)
{
    outPath[0] = 0;
    char dir[1024];
    strcpy(dir, heightPath);
    char *slash = strrchr(dir, '/');
    if (!slash) slash = strrchr(dir, '\\');
    char *nameStart = slash ? slash + 1 : dir;
    /* split dir / name */
    char nameBuf[1024];
    strcpy(nameBuf, nameStart);
    if (slash) *slash = 0; else dir[0] = 0;

    char noext[1024];
    strcpy(noext, nameBuf);
    char *dot = strrchr(noext, '.');
    if (dot) *dot = 0;

    /* strip the height suffix to get the material base name */
    char suf[64];
    LastSuffix(noext, suf, sizeof(suf));
    char matBase[1024];
    strcpy(matBase, noext);
    if (IsHeightmapSuffix(suf))
    {
        char *us = strrchr(matBase, '_');
        if (us) *us = 0;
    }

    static const char *NSUF[] = { "_norm", "_nrm", "_local", "_ddn", "_n", "_nh", NULL };
    for (int si = 0; NSUF[si]; si++)
    {
        for (int ei = 0; NORMALMAP_EXTS[ei]; ei++)
        {
            char cand[2048];
            if (dir[0])
                snprintf(cand, sizeof(cand), "%s/%s%s.%s", dir, matBase, NSUF[si], NORMALMAP_EXTS[ei]);
            else
                snprintf(cand, sizeof(cand), "%s%s.%s", matBase, NSUF[si], NORMALMAP_EXTS[ei]);

            if (cand[0] == '/' || cand[0] == '\\' || (cand[0] && cand[1] == ':'))
            {
                /* absolute path (e.g. drag-and-drop): check the filesystem directly */
                if (FileExists(cand))
                {
                    snprintf(outPath, cap, "%s", cand);
                    return;
                }
            }
            else
            {
                char fullCand[2048];
                if (vfsFindFile(cand, fullCand, sizeof(fullCand)))
                {
                    snprintf(outPath, cap, "%s", cand);
                    return;
                }
            }
        }
    }
}

static void EmbedIntoNormalmap(const char *normPath, const byte *heightPixels, int w, int h)
{
    byte *npix = NULL;
    int nw = 0, nh = 0;
    LoadImageVFS(normPath, &npix, &nw, &nh);
    if (!npix || nw != w || nh != h)
    {
        _printf("  embed: size mismatch or load failure for %s, skipping embed\n", normPath);
        if (npix) free(npix);
        return;
    }
    for (int i = 0; i < w * h; i++)
        npix[i * 4 + 3] = heightPixels[i];
    char outFull[2048];
    VfsWritePath(normPath, outFull, sizeof(outFull));
    if (!IsWritable(outFull))
    {
        _printf("  embed: target not writable: %s, skipping embed\n", outFull);
        free(npix);
        return;
    }
    EnsureParentDir(outFull);
    SaveImage(outFull, npix, nw, nh, 4);
    free(npix);
    _printf("  embedded into %s\n", normPath);
}

/* ------------------------------------------------------------------ */
/* Single-file processing                                             */
/* ------------------------------------------------------------------ */

static int ProcessOneFile(const char *path, hm_opts_t *o, anchor_t anchor)
{
    char outFull[1024];
    VfsWritePath(o->outPath ? o->outPath : path, outFull, sizeof(outFull));

    if (IsJpegPath(outFull))
    {
        _printf("  SKIPPED (jpg: degrading format, no writer): %s\n", path);
        return 0;
    }

    byte *pixels = NULL;
    int w = 0, h = 0;
    LoadImageVFS(path, &pixels, &w, &h);
    if (!pixels || w <= 0 || h <= 0)
    {
        _printf("  FAILED to load %s\n", path);
        if (pixels) free(pixels);
        return 0;
    }

    qboolean isNm = DetectNormalmap(path);
    /* default channel */
    if (!o->hasChannel)
    {
        if (isNm) o->channel = 'a';
        else      o->channel = 'l'; /* luma */
    }

    if (!IsWritable(outFull))
    {
        _printf("  SKIPPED (target not writable): %s\n", outFull);
        free(pixels);
        return 0;
    }
    EnsureParentDir(outFull);

    int changed = ProcessBuffer(pixels, w, h, o, anchor, isNm);
    if (!changed)
    {
        _printf("  unchanged, skipping write: %s\n", path);
        free(pixels);
        return 1;
    }
    SaveImage(outFull, pixels, w, h, 4);

    if (o->embed && !isNm)
    {
        char normPath[2048];
        FindSiblingNormalmap(path, normPath, sizeof(normPath));
        if (normPath[0])
        {
            /* rebuild height buffer from processed image (channel already written) */
            /* re-read the processed height values from the saved pixels */
            byte *hbuf = (byte *)malloc(w * h);
            if (hbuf)
            {
                for (int i = 0; i < w * h; i++)
                {
                    int ci = ChannelIndex(o);
                    if (ci < 0) hbuf[i] = (byte)((pixels[i*4+0]*77 + pixels[i*4+1]*150 + pixels[i*4+2]*29) >> 8);
                    else        hbuf[i] = pixels[i*4 + ci];
                }
                EmbedIntoNormalmap(normPath, hbuf, w, h);
                free(hbuf);
            }
        }
    }

    free(pixels);
    _printf("  OK: %s\n", path);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Batch mode                                                         */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
#include <windows.h>
#endif

static void BatchProcessDir(const char *dir, hm_opts_t *o, anchor_t anchor,
                           int *processed, int *failed)
{
#ifdef _WIN32
    char pattern[2048];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do
    {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;

        char full[4096];
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            BatchProcessDir(full, o, anchor, processed, failed);
        }
        else
        {
            /* extension gate: only formats we can also write back */
            char noext[1024];
            strcpy(noext, fd.cFileName);
            char *dot = strrchr(noext, '.');
            char extbuf[32];
            extbuf[0] = 0;
            if (dot)
            {
                snprintf(extbuf, sizeof(extbuf), "%s", dot + 1);
                *dot = 0;
            }
            for (int k = 0; extbuf[k]; k++)
                if (extbuf[k] >= 'A' && extbuf[k] <= 'Z') extbuf[k] += 32;
            static const char *IMGEXT[] = { "tga", "png", "bmp", NULL };
            int isImg = 0;
            for (int k = 0; IMGEXT[k]; k++)
                if (!strcmp(extbuf, IMGEXT[k])) { isImg = 1; break; }
            if (!isImg) continue;

            /* check suffix */
            char suf[64];
            LastSuffix(noext, suf, sizeof(suf));

            if (IsNormalmapSuffix(suf) || IsHeightmapSuffix(suf))
            {
                _printf("Processing: %s\n", full);
                if (ProcessOneFile(full, o, anchor)) (*processed)++;
                else (*failed)++;
            }
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
#else
    /* Minimal POSIX fallback: not implemented; use find(1) externally. */
    _printf("Batch mode requires Windows on this build.\n");
#endif
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                 */
/* ------------------------------------------------------------------ */

void ProcessHeightmap(const char *input, int argc, char **argv)
{
    /* parse options from argv tail */
    hm_opts_t o;
    memset(&o, 0, sizeof(o));
    o.outPath = NULL;
    o.embed = qfalse;
    o.hasRange = 0;
    o.hasChannel = 0;
    o.channel = 0;

    anchor_t anchor = ANCHOR_TOP;

    int i = 0;
    /* first positional anchor keyword */
    if (i < argc && argv[i][0] && argv[i][0] != '-')
    {
        if (!Q_stricmp(argv[i], "top"))    { anchor = ANCHOR_TOP; i++; }
        else if (!Q_stricmp(argv[i], "bottom")) { anchor = ANCHOR_BOTTOM; i++; }
        else if (!Q_stricmp(argv[i], "center")) { anchor = ANCHOR_CENTER; i++; }
    }

    for (; i < argc; i++)
    {
        if (!Q_stricmp(argv[i], "-range") || !Q_stricmp(argv[i], "range"))
        {
            if (i + 3 >= argc) { Error("-range requires <anchor> <lo> <hi>"); }
            const char *a = argv[i+1];
            if (!Q_stricmp(a, "top")) o.rangeAnchor = ANCHOR_TOP;
            else if (!Q_stricmp(a, "bottom")) o.rangeAnchor = ANCHOR_BOTTOM;
            else if (!Q_stricmp(a, "center")) o.rangeAnchor = ANCHOR_CENTER;
            else Error("-range anchor must be top/bottom/center");
            o.rangeLo = atoi(argv[i+2]);
            o.rangeHi = atoi(argv[i+3]);
            o.hasRange = 1;
            /* range overrides the standalone anchor */
            anchor = o.rangeAnchor;
            i += 3;
        }
        else if (!Q_stricmp(argv[i], "-channel"))
        {
            if (i + 1 >= argc) Error("-channel requires an argument");
            const char *c = argv[i+1];
            if (!Q_stricmp(c, "rgb"))   o.channel = 'x';
            else if (!Q_stricmp(c, "a") || !Q_stricmp(c, "alpha")) o.channel = 'a';
            else if (!Q_stricmp(c, "r") || !Q_stricmp(c, "red"))   o.channel = 'r';
            else if (!Q_stricmp(c, "g") || !Q_stricmp(c, "green")) o.channel = 'g';
            else if (!Q_stricmp(c, "b") || !Q_stricmp(c, "blue"))  o.channel = 'b';
            else if (!Q_stricmp(c, "luma") || !Q_stricmp(c, "l"))  o.channel = 'l';
            else Error("Unknown -channel '%s'", c);
            o.hasChannel = 1;
            i++;
        }
        else if (!Q_stricmp(argv[i], "-o"))
        {
            if (i + 1 >= argc) Error("-o requires a path");
            o.outPath = argv[i+1];
            i++;
        }
        else if (!Q_stricmp(argv[i], "-embed"))
        {
            o.embed = qtrue;
        }
        else
        {
            Error("Unknown heightmap option: %s", argv[i]);
        }
    }

    /* directory? */
    struct stat st;
    char batchBuf[1024];
    const char *batchDir = input;
    if (stat(input, &st) == 0 && (st.st_mode & S_IFDIR))
    {
        /* already an absolute/local directory */
    }
    else if (numVFSPaths > 0)
    {
        snprintf(batchBuf, sizeof(batchBuf), "%s%s", vfsPaths[0], input);
        batchDir = batchBuf;
    }
    if (stat(batchDir, &st) == 0 && (st.st_mode & S_IFDIR))
    {
        int processed = 0, failed = 0;
        _printf("--- heightmap batch: %s ---\n", input);
        BatchProcessDir(batchDir, &o, anchor, &processed, &failed);
        _printf("--- done: %d processed, %d failed ---\n", processed, failed);
        return;
    }

    _printf("--- heightmap: %s ---\n", input);
    ProcessOneFile(input, &o, anchor);
}
