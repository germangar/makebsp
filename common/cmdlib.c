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
// cmdlib.c

#include "cmdlib.h"
#include "connect.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <stdint.h>
#include <windows.h>
#endif

#ifndef _WIN32
#include <dirent.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/time.h>
#endif

#include "../libs/pakstuff.h"

#ifdef NeXT
#include <libc.h>
#endif

#define BASEDIRNAME "quake" // assumed to have a 2 or 3 following
#define PATHSEPERATOR '/'

// set these before calling CheckParm
int myargc;
char **myargv;

char com_token[1024];
qboolean com_eof;

qboolean archive;
char archivedir[1024];

/*
===================
ExpandWildcards

Mimic unix command line expansion
===================
*/
#define MAX_EX_ARGC 1024
int ex_argc;
char *ex_argv[MAX_EX_ARGC];
#ifdef _WIN32
#include "io.h"
void ExpandWildcards(int *argc, char ***argv)
{
    struct _finddata_t fileinfo;
    intptr_t handle;
    int i;
    char filename[1024];
    char filebase[1024];
    char *path;

    ex_argc = 0;
    for (i = 0; i < *argc; i++)
    {
        path = (*argv)[i];
        if (path[0] == '-' || (!strstr(path, "*") && !strstr(path, "?")))
        {
            ex_argv[ex_argc++] = path;
            continue;
        }

        handle = _findfirst(path, &fileinfo);
        if (handle == -1)
            return;

        ExtractFilePath(path, filebase);

        do
        {
            sprintf(filename, "%s%s", filebase, fileinfo.name);
            ex_argv[ex_argc++] = copystring(filename);
        } while (_findnext(handle, &fileinfo) != -1);

        _findclose(handle);
    }

    *argc = ex_argc;
    *argv = ex_argv;
}
#else
void ExpandWildcards(int *argc, char ***argv) {}
#endif

#ifdef WIN_ERROR
#include <windows.h>
/*
=================
Error

For abnormal program terminations in windowed apps
=================
*/
void Error(const char *error, ...)
{
    va_list argptr;
    char text[1024];
    char text2[1024];
    int err;

    err = GetLastError();

    va_start(argptr, error);
    vsprintf(text, error, argptr);
    va_end(argptr);

    sprintf(text2, "%s\nGetLastError() = %i", text, err);
    MessageBox(NULL, text2, "Error", 0 /* MB_OK */);

    exit(1);
}

#else
/*
=================
Error

For abnormal program terminations in console apps
=================
*/
jmp_buf *fatal_error_jmp = NULL;

void Error(const char *error, ...)
{
    va_list argptr;

    char errorBuf[4096];
    va_start(argptr, error);
    vsnprintf(errorBuf, sizeof(errorBuf), error, argptr);
    va_end(argptr);

    char msg[8192];
    snprintf(msg, sizeof(msg), "\n************ ERROR ************\n%s\n", errorBuf);
    Broadcast_Print(3, msg);

    printf("%s", msg);
    fflush(stdout);
    fprintf(stderr, "%s", msg);
    fflush(stderr);

    if (fatal_error_jmp) {
        longjmp(*fatal_error_jmp, 1);
    }

    Broadcast_Shutdown();

    exit(1);
}
#endif

// only printf if in verbose mode
qboolean verbose = qfalse;
void qprintf(const char *format, ...)
{
    va_list argptr;

    if (!verbose)
        return;

    va_start(argptr, format);
    vprintf(format, argptr);
    va_end(argptr);
}

#ifdef _WIN32
HWND hwndOut = NULL;
qboolean lookedForServer = qfalse;
UINT wm_BroadcastCommand = -1;
#endif

void _printf(const char *format, ...)
{
    va_list argptr;
    char text[4096];

    va_start(argptr, format);
    vsnprintf(text, sizeof(text), format, argptr);
    va_end(argptr);

    printf("%s", text);
    fflush(stdout);
    
    Broadcast_Print(1, text);

#ifdef _WIN32
    if (!lookedForServer)
    {
        lookedForServer = qtrue;
        hwndOut = FindWindow(NULL, "Q3Map Process Server");
        if (hwndOut)
        {
            wm_BroadcastCommand = RegisterWindowMessage("Q3MPS_BroadcastCommand");
        }
    }
    if (hwndOut)
    {
        if (strlen(text) < 255)
        {
            ATOM a = GlobalAddAtom(text);
            PostMessage(hwndOut, wm_BroadcastCommand, (WPARAM)a, 0);
        }
    }
#endif
}

/*
================
va

Returns a static buffer cycling between 4 possible buffers
================
*/
char *va(const char *format, ...)
{
    va_list argptr;
    static char string[4][1024];
    static int curstring;

    curstring = (curstring + 1) & 3;
    va_start(argptr, format);
    vsprintf(string[curstring], format, argptr);
    va_end(argptr);

    return string[curstring];
}

/*
  vfsPaths[] holds all search paths in priority order.
  Index 0 = highest priority (first user path or mod path).
  Last index = lowest priority (base game path from game profile).
  
  Each entry is a fully resolved, normalized path ending in '/'.
  writedir = vfsPaths[0], the single write destination.
*/

char vfsPaths[MAX_VFS_PATHS][1024];
int   numVFSPaths;
char  writedir[1024];
char  executablePath[1024];

/*
==============
GetExecutablePath

Determines the directory where the executable is located.
Uses GetModuleFileName on Windows for absolute robustness.
==============
*/
void GetExecutablePath(const char *argv0)
{
#ifdef _WIN32
    char path[1024];
    if (GetModuleFileName(NULL, path, sizeof(path)))
    {
        ExtractFilePath(path, executablePath);
        NormalizePath(executablePath);
        return;
    }
#elif defined(__linux__)
    char path[1024];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len != -1)
    {
        path[len] = '\0';
        ExtractFilePath(path, executablePath);
        NormalizePath(executablePath);
        return;
    }
#endif

    // Fallback to argv[0] parsing
    ExtractFilePath(argv0, executablePath);
    if (executablePath[0] == '\0')
    {
        strcpy(executablePath, "./");
    }
    NormalizePath(executablePath);
}

void NormalizePath(char *path)
{
    int i;
    for (i = 0; i < strlen(path); i++)
    {
        if (path[i] == '\\')
            path[i] = '/';
    }
    if (path[0] && path[strlen(path) - 1] != '/')
    {
        strcat(path, "/");
    }
}

void AddVFSPath(const char *basePath, const char *gameDir)
{
    if (numVFSPaths >= MAX_VFS_PATHS)
    {
        _printf("WARNING: MAX_VFS_PATHS (%d) reached, ignoring path: %s\n",
                MAX_VFS_PATHS, basePath);
        return;
    }

    char buf[1024];
    strncpy(buf, basePath, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    NormalizePath(buf);

    if (gameDir && gameDir[0] && strcmp(gameDir, ".") != 0)
    {
        // For absolute paths or specific mounts, we check if the gameDir is ALREADY the last component.
        // We do this by checking if the path ends with /gameDir or is exactly gameDir.
        int gameLen = strlen(gameDir);
        int pathLen = strlen(buf);
        
        // Remove trailing slash for comparison
        if (pathLen > 0 && buf[pathLen - 1] == '/') {
            buf[pathLen - 1] = '\0';
            pathLen--;
        }

        qboolean needsAppend = qtrue;
        if (pathLen >= gameLen) {
            const char *lastComponent = buf + pathLen - gameLen;
            if (!Q_stricmp(lastComponent, gameDir)) {
                if (pathLen == gameLen || *(lastComponent - 1) == '/') {
                    needsAppend = qfalse;
                }
            }
        }

        if (needsAppend) {
            // Restore slash if we removed it and didn't find the gameDir
            strncat(buf, "/", sizeof(buf) - strlen(buf) - 1);
            strncat(buf, gameDir, sizeof(buf) - strlen(buf) - 1);
        }
        
        // Ensure it ends with a slash for the VFS standard
        int finalLen = strlen(buf);
        if (finalLen > 0 && buf[finalLen-1] != '/') {
             strncat(buf, "/", sizeof(buf) - finalLen - 1);
        }
    }

    // Skip duplicates
    for (int i = 0; i < numVFSPaths; i++)
    {
        if (!Q_stricmp(vfsPaths[i], buf))
            return;
    }

    strcpy(vfsPaths[numVFSPaths], buf);
    numVFSPaths++;
}

void InitVFSWriteDir(void)
{
    if (numVFSPaths > 0)
        strcpy(writedir, vfsPaths[0]);
    else
    {
        Q_getwd(writedir);
        NormalizePath(writedir);
    }
}

/*
==============
vfsFindFile

Searches all VFS paths in priority order for a loose file.
Returns 1 if found (writes full path to outFullPath). Returns 0 if not found.
==============
*/
int vfsFindFile(const char *relativePath, char *outFullPath, int outSize)
{
    int i;
    for (i = 0; i < numVFSPaths; i++)
    {
        snprintf(outFullPath, outSize, "%s%s", vfsPaths[i], relativePath);
        if (FileExists(outFullPath))
            return 1;
    }
    return 0;
}

/*
==============
TryLoadFile

Allows failure
==============
*/
int TryLoadFile(const char *filename, void **bufferptr)
{
    FILE *f;
    int length;
    void *buffer;

    *bufferptr = NULL;

    f = fopen(filename, "rb");
    if (!f)
    {
        return -1;
    }
    length = Q_filelength(f);
    buffer = malloc(length + 1);
    ((char *)buffer)[length] = 0;
    SafeRead(f, buffer, length);
    fclose(f);

    *bufferptr = buffer;
    return length;
}

/*
==============
vfsLoadFile

Load a file using VFS priority: loose files across all paths, then PAK/PK3 across all paths.
Returns file length on success, -1 on failure.
==============
*/
int vfsLoadFile(const char *relativePath, void **bufferptr)
{
    char fullPath[1024];
    int length;
    int i;

    *bufferptr = NULL;

    // 0. Try absolute path directly if it looks like one
    if (relativePath[0] == '/' || relativePath[0] == '\\' || (relativePath[0] && relativePath[1] == ':'))
    {
        length = TryLoadFile(relativePath, bufferptr);
        if (length >= 0)
            return length;
    }

    // 1. Scan VFS paths in priority order
    for (i = 0; i < numVFSPaths; i++)
    {
        // a. Try loose files in this path
        snprintf(fullPath, sizeof(fullPath), "%s%s", vfsPaths[i], relativePath);
        length = TryLoadFile(fullPath, bufferptr);
        if (length >= 0)
            return length;

        // b. Try PAK/PK3 archives in this path
        length = PakLoadAnyFile(fullPath, bufferptr);
        if (length >= 0)
            return length;
    }

    return -1;
}

char *ExpandArg(const char *path)
{
    static char full[1024];

    if (path[0] != '/' && path[0] != '\\' && path[1] != ':')
    {
        Q_getwd(full);
        strcat(full, path);
    }
    else
        strcpy(full, path);
    return full;
}

char *ExpandPath(const char *path)
{
    static char full[1024];
    if (numVFSPaths == 0)
        Error("ExpandPath called before VFS is initialized");
    if (path[0] == '/' || path[0] == '\\' || path[1] == ':')
    {
        strcpy(full, path);
        return full;
    }
    // Use lowest-priority (base game) path — equivalent to old rootDir+gameDir
    sprintf(full, "%s%s", vfsPaths[numVFSPaths - 1], path);
    return full;
}

char *ExpandGamePath(const char *path)
{
    static char full[1024];
    if (numVFSPaths == 0)
        Error("ExpandGamePath called before VFS is initialized");
    if (path[0] == '/' || path[0] == '\\' || path[1] == ':')
    {
        strcpy(full, path);
        return full;
    }

    // Search VFS in priority order
    if (vfsFindFile(path, full, sizeof(full)))
    {
        return full;
    }

    // Fallback: lowest-priority path (base game)
    sprintf(full, "%s%s", vfsPaths[numVFSPaths - 1], path);
    return full;
}

char *ExpandPathAndArchive(const char *path)
{
    char *expanded;
    char archivename[1024];

    expanded = ExpandPath(path);

    if (archive)
    {
        sprintf(archivename, "%s/%s", archivedir, path);
        QCopyFile(expanded, archivename);
    }
    return expanded;
}

void *copystring(const char *s)
{
    char *b;
    b = malloc(strlen(s) + 1);
    strcpy(b, s);
    return b;
}

/*
================
MEMORY MAPPING WRAPPERS (Low-Memory Mode)
================
*/

extern qboolean g_lowmem;

#ifdef _WIN32
typedef struct {
    void *ptr;
    HANDLE hFile;
    HANDLE hMap;
} memmap_t;

#define MAX_MEMMAPS 1024
static memmap_t memmaps[MAX_MEMMAPS];
static int numMemmaps = 0;
#else
typedef struct {
    void *ptr;
    size_t size;
    int fd;
} memmap_t;

#define MAX_MEMMAPS 1024
static memmap_t memmaps[MAX_MEMMAPS];
static int numMemmaps = 0;
#endif

void *Q_Alloc(size_t size) {
    if (size == 0) return NULL;

#ifdef _WIN32
    if (g_lowmem) {
        if (numMemmaps >= MAX_MEMMAPS) {
            Error("Q_Alloc: MAX_MEMMAPS exceeded");
        }

        // Create a temporary file for the mapping
        char tempPath[MAX_PATH];
        char tempFile[MAX_PATH];
        if (!GetTempPath(MAX_PATH, tempPath)) {
            Error("Q_Alloc: GetTempPath failed");
        }
        if (!GetTempFileName(tempPath, "makebsp", 0, tempFile)) {
            Error("Q_Alloc: GetTempFileName failed");
        }

        // Open with FILE_FLAG_DELETE_ON_CLOSE so it's nuked when handles are closed
        HANDLE hFile = CreateFile(tempFile, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            Error("Q_Alloc: CreateFile failed for %s", tempFile);
        }

        ULARGE_INTEGER liSize;
        liSize.QuadPart = size;

        HANDLE hMap = CreateFileMapping(hFile, NULL, PAGE_READWRITE, liSize.HighPart, liSize.LowPart, NULL);
        if (hMap == NULL) {
            CloseHandle(hFile);
            Error("Q_Alloc: CreateFileMapping failed for size %zu (Error: %d)", size, (int)GetLastError());
        }

        void *ptr = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, size);
        if (ptr == NULL) {
            CloseHandle(hMap);
            CloseHandle(hFile);
            Error("Q_Alloc: MapViewOfFile failed for size %zu (Error: %d)", size, (int)GetLastError());
        }

        if (verbose) {
            _printf("Q_Alloc: Memory-mapped %zu bytes to %s\n", size, tempFile);
        }

        memmaps[numMemmaps].ptr = ptr;
        memmaps[numMemmaps].hFile = hFile;
        memmaps[numMemmaps].hMap = hMap;
        numMemmaps++;

        return ptr;
    }
#else
    if (g_lowmem) {
        if (numMemmaps >= MAX_MEMMAPS) {
            Error("Q_Alloc: MAX_MEMMAPS exceeded");
        }

        char tempFile[] = "/tmp/makebspXXXXXX";
        int fd = mkstemp(tempFile);
        if (fd == -1) {
            Error("Q_Alloc: mkstemp failed");
        }
        unlink(tempFile); // Nuked on close

        if (ftruncate(fd, size) == -1) {
            close(fd);
            Error("Q_Alloc: ftruncate failed");
        }

        void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) {
            close(fd);
            Error("Q_Alloc: mmap failed");
        }

        memmaps[numMemmaps].ptr = ptr;
        memmaps[numMemmaps].size = size;
        memmaps[numMemmaps].fd = fd;
        numMemmaps++;

        return ptr;
    }
#endif

    void *ptr = malloc(size);
    if (!ptr) {
        Error("Q_Alloc: malloc failed for size %zu", size);
    }
    return ptr;
}

void Q_Free(void *ptr) {
    if (ptr == NULL) return;

#ifdef _WIN32
    for (int i = 0; i < numMemmaps; i++) {
        if (memmaps[i].ptr == ptr) {
            UnmapViewOfFile(ptr);
            CloseHandle(memmaps[i].hMap);
            CloseHandle(memmaps[i].hFile);
            
            // Swap with last element to keep array dense
            memmaps[i] = memmaps[numMemmaps - 1];
            numMemmaps--;
            return;
        }
    }
#else
    for (int i = 0; i < numMemmaps; i++) {
        if (memmaps[i].ptr == ptr) {
            munmap(ptr, memmaps[i].size);
            close(memmaps[i].fd);
            
            // Swap with last element to keep array dense
            memmaps[i] = memmaps[numMemmaps - 1];
            numMemmaps--;
            return;
        }
    }
#endif

    free(ptr);
}

void *Q_Realloc(void *ptr, size_t oldSize, size_t newSize) {
    if (ptr == NULL) return Q_Alloc(newSize);
    if (newSize == 0) {
        Q_Free(ptr);
        return NULL;
    }

#ifdef _WIN32
    // Check if this was a mapped pointer
    for (int i = 0; i < numMemmaps; i++) {
        if (memmaps[i].ptr == ptr) {
            void *newPtr = Q_Alloc(newSize);
            size_t copySize = (oldSize < newSize) ? oldSize : newSize;
            memcpy(newPtr, ptr, copySize);
            Q_Free(ptr);
            return newPtr;
        }
    }
#endif

    void *newPtr = realloc(ptr, newSize);
    if (!newPtr) {
        Error("Q_Realloc: realloc failed for size %zu", newSize);
    }
    return newPtr;
}

/*
================
I_FloatTime
================
*/
double I_FloatTime(void)
{
#ifdef _WIN32
    time_t t;
    time(&t);
    return (double)t;
#else
    struct timeval tp;
    static int secbase;

    gettimeofday(&tp, NULL);
    if (!secbase)
    {
        secbase = tp.tv_sec;
        return tp.tv_usec / 1000000.0;
    }
    return (double)(tp.tv_sec - secbase) + tp.tv_usec / 1000000.0;
#endif
}

void Q_getwd(char *out)
{
    int i = 0;

#ifdef _WIN32
    _getcwd(out, 256);
    strcat(out, "\\");
#else
    getcwd(out, 256);
    strcat(out, "/");
#endif

    while (out[i] != 0)
    {
        if (out[i] == '\\')
            out[i] = '/';
        i++;
    }
}

void Q_mkdir(const char *path)
{
#ifdef _WIN32
    if (_mkdir(path) != -1)
        return;
#else
    if (mkdir(path, 0777) != -1)
        return;
#endif
    if (errno != EEXIST)
        Error("mkdir %s: %s", path, strerror(errno));
}

/*
============
FileTime

returns -1 if not present
============
*/
int FileTime(const char *path)
{
    struct stat buf;

    if (stat(path, &buf) == -1)
        return -1;

    return buf.st_mtime;
}

/*
==============
COM_Parse

Parse a token out of a string
==============
*/
char *COM_Parse(char *data)
{
    int c;
    int len;

    len = 0;
    com_token[0] = 0;

    if (!data)
        return NULL;

// skip whitespace
skipwhite:
    while ((c = *data) <= ' ')
    {
        if (c == 0)
        {
            com_eof = qtrue;
            return NULL; // end of file;
        }
        data++;
    }

    // skip // comments
    if (c == '/' && data[1] == '/')
    {
        while (*data && *data != '\n')
            data++;
        goto skipwhite;
    }

    // handle quoted strings specially
    if (c == '\"')
    {
        data++;
        do
        {
            c = *data++;
            if (c == '\"')
            {
                com_token[len] = 0;
                return data;
            }
            com_token[len] = c;
            len++;
        } while (1);
    }

    // parse single characters
    if (c == '{' || c == '}' || c == ')' || c == '(' || c == '\'' || c == ':')
    {
        com_token[len] = c;
        len++;
        com_token[len] = 0;
        return data + 1;
    }

    // parse a regular word
    do
    {
        com_token[len] = c;
        data++;
        len++;
        c = *data;
        if (c == '{' || c == '}' || c == ')' || c == '(' || c == '\'' || c == ':')
            break;
    } while (c > 32);

    com_token[len] = 0;
    return data;
}

int Q_strncasecmp(const char *s1, const char *s2, int n)
{
    int c1, c2;

    do
    {
        c1 = *s1++;
        c2 = *s2++;

        if (!n--)
            return 0; // strings are equal until end point

        if (c1 != c2)
        {
            if (c1 >= 'a' && c1 <= 'z')
                c1 -= ('a' - 'A');
            if (c2 >= 'a' && c2 <= 'z')
                c2 -= ('a' - 'A');
            if (c1 != c2)
                return -1; // strings not equal
        }
    } while (c1);

    return 0; // strings are equal
}

int Q_stricmp(const char *s1, const char *s2)
{
    return Q_strncasecmp(s1, s2, 99999);
}

char *Q_stristr(const char *s, const char *find)
{
    size_t len1, len2;
    size_t i;

    if (!s || !find) return NULL;

    len1 = strlen(s);
    len2 = strlen(find);

    if (len2 == 0) return (char *)s;
    if (len1 < len2) return NULL;

    for (i = 0; i <= len1 - len2; ++i) {
        if (Q_strncasecmp(s + i, find, len2) == 0) {
            return (char *)(s + i);
        }
    }
    return NULL;
}

char *strupr(char *start)
{
    char *in;
    in = start;
    while (*in)
    {
        *in = toupper(*in);
        in++;
    }
    return start;
}

char *strlower(char *start)
{
    char *in;
    in = start;
    while (*in)
    {
        *in = tolower(*in);
        in++;
    }
    return start;
}

/*
=============================================================================

                                                MISC FUNCTIONS

=============================================================================
*/

/*
=================
CheckParm

Checks for the given parameter in the program's command line arguments
Returns the argument number (1 to argc-1) or 0 if not present
=================
*/
int CheckParm(const char *check)
{
    int i;

    for (i = 1; i < myargc; i++)
    {
        if (!Q_stricmp(check, myargv[i]))
            return i;
    }

    return 0;
}

/*
================
Q_filelength
================
*/
int Q_filelength(FILE *f)
{
    int pos;
    int end;

    pos = ftell(f);
    fseek(f, 0, SEEK_END);
    end = ftell(f);
    fseek(f, pos, SEEK_SET);

    return end;
}

FILE *SafeOpenWrite(const char *filename)
{
    FILE *f;

    f = fopen(filename, "wb");

    if (!f)
        Error("Error opening %s: %s", filename, strerror(errno));

    return f;
}

FILE *SafeOpenRead(const char *filename)
{
    FILE *f;

    f = fopen(filename, "rb");

    if (!f)
        Error("Error opening %s: %s", filename, strerror(errno));

    return f;
}

void SafeRead(FILE *f, void *buffer, int count)
{
    if (fread(buffer, 1, count, f) != (size_t)count)
        Error("File read failure");
}

void SafeWrite(FILE *f, const void *buffer, int count)
{
    if (fwrite(buffer, 1, count, f) != (size_t)count)
        Error("File write failure");
}

/*
==============
FileExists
==============
*/
qboolean FileExists(const char *filename)
{
    FILE *f;

    f = fopen(filename, "rb");
    if (!f)
    {
        return qfalse;
    }
    fclose(f);
    return qtrue;
}

/*
==============
LoadFile
==============
*/
int LoadFile(const char *filename, void **bufferptr)
{
    FILE *f;
    int length;
    void *buffer;

    f = SafeOpenRead(filename);
    length = Q_filelength(f);
    buffer = malloc(length + 1);
    ((char *)buffer)[length] = 0;
    SafeRead(f, buffer, length);
    fclose(f);

    *bufferptr = buffer;
    return length;
}

/*
==============
LoadFileBlock
-
rounds up memory allocation to 4K boundry
-
==============
*/
int LoadFileBlock(const char *filename, void **bufferptr)
{
    FILE *f;
    int length, nBlock, nAllocSize;
    void *buffer;

    f = SafeOpenRead(filename);
    length = Q_filelength(f);
    nAllocSize = length;
    nBlock = nAllocSize % MEM_BLOCKSIZE;
    if (nBlock > 0)
    {
        nAllocSize += MEM_BLOCKSIZE - nBlock;
    }

    buffer = malloc(nAllocSize + 1);
    if (!buffer)
    {
        Error("LoadFileBlock: Failed to allocate %i bytes", nAllocSize + 1);
    }
    memset(buffer, 0, nAllocSize + 1);

    SafeRead(f, buffer, length);
    fclose(f);

    *bufferptr = buffer;

    return length;
}

/*
==============
SaveFile
==============
*/
void SaveFile(const char *filename, const void *buffer, int count)
{
    FILE *f;

    f = SafeOpenWrite(filename);
    SafeWrite(f, buffer, count);
    fclose(f);
}

void DefaultExtension(char *path, const char *extension)
{
    char *src;
    //
    // if path doesnt have a .EXT, append extension
    // (extension should include the .)
    //
    src = path + strlen(path) - 1;

    while (*src != '/' && *src != '\\' && src != path)
    {
        if (*src == '.')
            return; // it has an extension
        src--;
    }

    strcat(path, extension);
}

void DefaultPath(char *path, const char *basepath)
{
    char temp[128];

    if (path[0] == PATHSEPERATOR)
        return; // absolute path location
    strcpy(temp, path);
    strcpy(path, basepath);
    strcat(path, temp);
}

void StripFilename(char *path)
{
    int length;

    length = strlen(path) - 1;
    while (length > 0 && path[length] != PATHSEPERATOR)
        length--;
    path[length] = 0;
}

void StripExtension(char *path)
{
    int length;

    length = strlen(path) - 1;
    while (length > 0 && path[length] != '.')
    {
        length--;
        if (path[length] == '/')
            return; // no extension
    }
    if (length)
        path[length] = 0;
}

/*
====================
Extract file parts
====================
*/
// FIXME: should include the slash, otherwise
// backing to an empty path will be wrong when appending a slash
void ExtractFilePath(const char *path, char *dest)
{
    const char *src;

    src = path + strlen(path) - 1;

    //
    // back up until a \ or the start
    //
    while (src != path && *(src - 1) != '\\' && *(src - 1) != '/')
        src--;

    memcpy(dest, path, src - path);
    dest[src - path] = 0;
}

void ExtractFileBase(const char *path, char *dest)
{
    const char *src;

    src = path + strlen(path) - 1;

    //
    // back up until a slash or the start
    //
    while (src != path && *(src - 1) != '/' && *(src - 1) != '\\')
        src--;

    while (*src && *src != '.')
    {
        *dest++ = *src++;
    }
    *dest = 0;
}

void ExtractFileExtension(const char *path, char *dest)
{
    const char *src;

    src = path + strlen(path) - 1;

    //
    // back up until a . or the start
    //
    while (src != path && *(src - 1) != '.')
        src--;

    // If we hit a slash before a dot, there is no extension
    if (src == path) {
        *dest = 0; // no extension
        return;
    }

    strcpy(dest, src);
}

/*
==============
ParseNum / ParseHex
==============
*/
int ParseHex(const char *hex)
{
    const char *str;
    int num;

    num = 0;
    str = hex;

    while (*str)
    {
        num <<= 4;
        if (*str >= '0' && *str <= '9')
            num += *str - '0';
        else if (*str >= 'a' && *str <= 'f')
            num += 10 + *str - 'a';
        else if (*str >= 'A' && *str <= 'F')
            num += 10 + *str - 'A';
        else
            Error("Bad hex number: %s", hex);
        str++;
    }

    return num;
}

int ParseNum(const char *str)
{
    if (str[0] == '$')
        return ParseHex(str + 1);
    if (str[0] == '0' && str[1] == 'x')
        return ParseHex(str + 2);
    return atol(str);
}

/*
============================================================================

                                        BYTE ORDER FUNCTIONS

============================================================================
*/

#ifdef _SGI_SOURCE
#define __BIG_ENDIAN__
#endif

#ifdef __BIG_ENDIAN__

short LittleShort(short l)
{
    byte b1, b2;

    b1 = l & 255;
    b2 = (l >> 8) & 255;

    return (b1 << 8) + b2;
}

short BigShort(short l) { return l; }

int LittleLong(int l)
{
    byte b1, b2, b3, b4;

    b1 = l & 255;
    b2 = (l >> 8) & 255;
    b3 = (l >> 16) & 255;
    b4 = (l >> 24) & 255;

    return ((int)b1 << 24) + ((int)b2 << 16) + ((int)b3 << 8) + b4;
}

int BigLong(int l) { return l; }

float LittleFloat(float l)
{
    union
    {
        byte b[4];
        float f;
    } in, out;

    in.f = l;
    out.b[0] = in.b[3];
    out.b[1] = in.b[2];
    out.b[2] = in.b[1];
    out.b[3] = in.b[0];

    return out.f;
}

float BigFloat(float l) { return l; }

#else

short BigShort(short l)
{
    byte b1, b2;

    b1 = l & 255;
    b2 = (l >> 8) & 255;

    return (b1 << 8) + b2;
}

short LittleShort(short l) { return l; }

int BigLong(int l)
{
    byte b1, b2, b3, b4;

    b1 = l & 255;
    b2 = (l >> 8) & 255;
    b3 = (l >> 16) & 255;
    b4 = (l >> 24) & 255;

    return ((int)b1 << 24) + ((int)b2 << 16) + ((int)b3 << 8) + b4;
}

int LittleLong(int l) { return l; }

float BigFloat(float l)
{
    union
    {
        byte b[4];
        float f;
    } in, out;

    in.f = l;
    out.b[0] = in.b[3];
    out.b[1] = in.b[2];
    out.b[2] = in.b[1];
    out.b[3] = in.b[0];

    return out.f;
}

float LittleFloat(float l) { return l; }

#endif

//=======================================================

// FIXME: byte swap?

// this is a 16 bit, non-reflected CRC using the polynomial 0x1021
// and the initial and final xor values shown below...  in other words, the
// CCITT standard CRC used by XMODEM

#define CRC_INIT_VALUE 0xffff
#define CRC_XOR_VALUE 0x0000

static unsigned short crctable[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7, 0x8108,
    0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef, 0x1231, 0x0210,
    0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6, 0x9339, 0x8318, 0xb37b,
    0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de, 0x2462, 0x3443, 0x0420, 0x1401,
    0x64e6, 0x74c7, 0x44a4, 0x5485, 0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee,
    0xf5cf, 0xc5ac, 0xd58d, 0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6,
    0x5695, 0x46b4, 0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d,
    0xc7bc, 0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b, 0x5af5,
    0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12, 0xdbfd, 0xcbdc,
    0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a, 0x6ca6, 0x7c87, 0x4ce4,
    0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41, 0xedae, 0xfd8f, 0xcdec, 0xddcd,
    0xad2a, 0xbd0b, 0x8d68, 0x9d49, 0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13,
    0x2e32, 0x1e51, 0x0ed1, 0x1ef0, 0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a,
    0x9f59, 0x8f78, 0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e,
    0xe16f, 0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e, 0x02b1,
    0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256, 0xb5ea, 0xa5cb,
    0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d, 0x34e2, 0x24c3, 0x14a0,
    0x0481, 0x7466, 0x6447, 0x5424, 0x4405, 0xa7db, 0xb7fa, 0x8799, 0x97b8,
    0xe75f, 0xf77e, 0xc71d, 0xd73c, 0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657,
    0x7676, 0x4615, 0x5634, 0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9,
    0xb98a, 0xa9ab, 0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882,
    0x28a3, 0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
    0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92, 0xfd2e,
    0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9, 0x7c26, 0x6c07,
    0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1, 0xef1f, 0xff3e, 0xcf5d,
    0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8, 0x6e17, 0x7e36, 0x4e55, 0x5e74,
    0x2e93, 0x3eb2, 0x0ed1, 0x1ef0};

void CRC_Init(unsigned short *crcvalue) { *crcvalue = CRC_INIT_VALUE; }

void CRC_ProcessByte(unsigned short *crcvalue, byte data)
{
    *crcvalue = (*crcvalue << 8) ^ crctable[(*crcvalue >> 8) ^ data];
}

unsigned short CRC_Value(unsigned short crcvalue)
{
    return crcvalue ^ CRC_XOR_VALUE;
}
//=============================================================================

/*
============
CreatePath
============
*/
void CreatePath(const char *path)
{
    const char *ofs;
    char c;
    char dir[1024];

#ifdef _WIN32
    int olddrive = -1;

    if (path[1] == ':')
    {
        olddrive = _getdrive();
        _chdrive(toupper(path[0]) - 'A' + 1);
    }
#endif

    if (path[1] == ':')
        path += 2;

    for (ofs = path + 1; *ofs; ofs++)
    {
        c = *ofs;
        if (c == '/' || c == '\\')
        { // create the directory
            memcpy(dir, path, ofs - path);
            dir[ofs - path] = 0;
            Q_mkdir(dir);
        }
    }

#ifdef _WIN32
    if (olddrive != -1)
    {
        _chdrive(olddrive);
    }
#endif
}

/*
============
QCopyFile

  Used to archive source files
============
*/
void QCopyFile(const char *from, const char *to)
{
    void *buffer;
    int length;

    length = LoadFile(from, &buffer);
    CreatePath(to);
    SaveFile(to, buffer, length);
    free(buffer);
}
void Sys_ListFiles(const char *directory, const char *extension,
                   void (*callback)(const char *filename))
{
    char search[MAX_OS_PATH];

#ifdef _WIN32
    struct _finddata_t fileinfo;
    intptr_t handle;

    if (directory[0] && directory[strlen(directory) - 1] != '/' && directory[strlen(directory) - 1] != '\\')
        sprintf(search, "%s/%s", directory, extension);
    else
        sprintf(search, "%s%s", directory, extension);
    
    // Convert forward slashes to backslashes for Windows API
    for (int j = 0; search[j]; j++) {
        if (search[j] == '/') search[j] = '\\';
    }

    handle = _findfirst(search, &fileinfo);
    if (handle == -1)
        return;

    do
    {
        callback(fileinfo.name);
    } while (_findnext(handle, &fileinfo) != -1);

    _findclose(handle);
#else
    DIR *dir;
    struct dirent *entry;
    int nameLen;
    int extLen;

    extLen = strlen(extension);

    dir = opendir(directory);
    if (!dir)
        return;

    while ((entry = readdir(dir)) != NULL)
    {
        nameLen = strlen(entry->d_name);
        if (nameLen >= extLen &&
            !Q_stricmp(entry->d_name + nameLen - extLen, extension + 1))
        {
            callback(entry->d_name);
        }
    }

    closedir(dir);
#endif
}

/*
================
ParseColor

Parses a color string into a vec3_t.
Supports hex colors starting with '#' (e.g., "#FFFFFF")
and RGB colors in "R G B" format (0.0-1.0 or 0-255 scale).
Defaults to white (1, 1, 1) on invalid input.
================
*/
void ParseColor(const char *str, vec3_t color)
{
    int r, g, b;
    float fr, fg, fb;
    qboolean forceNormalize = qfalse;

    if (!str || !str[0])
    {
        VectorSet(color, 1.0f, 1.0f, 1.0f);
        return;
    }

    // Hex color: #RRGGBB
    if (str[0] == '#')
    {
        if (sscanf(str + 1, "%02x%02x%02x", &r, &g, &b) == 3)
        {
            fr = (float)r;
            fg = (float)g;
            fb = (float)b;
            forceNormalize = qtrue;
        }
        else
        {
            VectorSet(color, 1.0f, 1.0f, 1.0f);
            return;
        }
    }
    else
    {
        // RGB color: "R G B"
        if (sscanf(str, "%f %f %f", &fr, &fg, &fb) != 3)
        {
            VectorSet(color, 1.0f, 1.0f, 1.0f);
            return;
        }
    }

    // If any component is > 1.0001, or if it was a hex color, assume 0-255 scale and normalize
    if (forceNormalize || fr > 1.0001f || fg > 1.0001f || fb > 1.0001f)
    {
        if (fr < 0.0f) fr = 0.0f; else if (fr > 255.0f) fr = 255.0f;
        if (fg < 0.0f) fg = 0.0f; else if (fg > 255.0f) fg = 255.0f;
        if (fb < 0.0f) fb = 0.0f; else if (fb > 255.0f) fb = 255.0f;

        color[0] = fr / 255.0f;
        color[1] = fg / 255.0f;
        color[2] = fb / 255.0f;
    }
    else
    {
        // Otherwise assume 0-1 scale and just copy
        color[0] = fr;
        color[1] = fg;
        color[2] = fb;
    }

    // Clamp channels to [0.0, 1.0] to handle edge cases or overflow
    for (int i = 0; i < 3; i++)
    {
        if (color[i] < 0.0f)
            color[i] = 0.0f;
        else if (color[i] > 1.0f)
            color[i] = 1.0f;
    }
}
