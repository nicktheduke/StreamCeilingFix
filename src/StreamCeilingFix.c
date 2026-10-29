// StreamCeilingFix - SKSE plugin
// Copyright (c) 2026 NSM. MIT License, see LICENSE.
//
// Skyrim keeps every plugin (.esp/.esm/.esl) open through the C runtime (two FILE* per plugin:
// TESFile::lockedFile + TESFile::file). The UCRT caps open streams at 8192 for the whole process,
// shared with every SKSE DLL built against the shared CRT. With ~3600 plugins the game sits near
// that ceiling; once cell loads push it over, fopen fails everywhere and every save "is corrupted".
//
// This plugin redirects the game's own read-only binary opens of plugin files to plain Win32 handles
// (no CRT slot used). Everything else - saves, inis, writes, text mode, other DLLs - is passed
// through untouched to whatever the import table pointed to before.

#include <windows.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define EXPORT __declspec(dllexport)

/* ---------------- SKSE ---------------- */
typedef struct {
    uint32_t dataVersion;
    uint32_t pluginVersion;
    char name[256];
    char author[256];
    char supportEmail[252];
    uint32_t versionIndependenceEx;
    uint32_t versionIndependence;
    uint32_t compatibleVersions[16];
    uint32_t seVersionRequired;
} SKSEPluginVersionData;

EXPORT SKSEPluginVersionData SKSEPlugin_Version = {
    1,
    (1 << 24) | (0 << 16) | (1 << 4),
    "StreamCeilingFix",
    "NSM",
    "",
    1,          /* kVersionIndependentEx_NoStructUse */
    1u << 1,    /* kVersionIndependent_Signatures: no game addresses used, import table only */
    { 0 },
    0
};

/* ---------------- logging ---------------- */
static HANDLE g_log = INVALID_HANDLE_VALUE;
static SRWLOCK g_logLock = SRWLOCK_INIT;
static int g_logIndex = 0;

static void slog(const char* fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    char buf[1024];
    SYSTEMTIME st; GetLocalTime(&st);
    int n = _snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    int m = _vsnprintf(buf + n, sizeof(buf) - n - 3, fmt, ap);
    va_end(ap);
    if (m < 0) m = (int)(sizeof(buf) - n - 3);
    n += m;
    buf[n++] = '\r'; buf[n++] = '\n';
    DWORD w;
    AcquireSRWLockExclusive(&g_logLock);
    WriteFile(g_log, buf, (DWORD)n, &w, NULL);
    ReleaseSRWLockExclusive(&g_logLock);
}

static void openLog(void)
{
    /* Every handle is opened read/write/delete-shareable, so a session that is still shutting down (or a
       leftover process) never blocks the next one. If the main log is still held by something that did not
       share it, fall back to a numbered file instead of running silently. */
    wchar_t dir[MAX_PATH * 2], path[MAX_PATH * 2];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, dir))) return;
    wcscat(dir, L"\\My Games\\Skyrim Special Edition\\SKSE\\");
    for (int i = 0; i < 4 && g_log == INVALID_HANDLE_VALUE; ++i) {
        if (i == 0) _snwprintf(path, MAX_PATH * 2, L"%lsStreamCeilingFix.log", dir);
        else        _snwprintf(path, MAX_PATH * 2, L"%lsStreamCeilingFix.%d.log", dir, i);
        path[MAX_PATH * 2 - 1] = 0;
        g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        g_logIndex = i;
    }
}

/* ---------------- our stream objects ----------------
   Layout of the first 0x58 bytes mirrors UCRT's __crt_stdio_stream_data with flags = 0 and a live
   CRITICAL_SECTION at 0x30, so if anything ever hands one of ours to the real CRT it fails a read
   cleanly instead of crashing. */
#define SLOT_SIZE   256
#define MAX_SLOTS   (1u << 16)          /* 65536 simultaneous plugin streams */
#define BUF_SIZE    8192

typedef struct {
    char*   ptr;            /* 00 */
    char*   base;           /* 08 */
    int     cnt;            /* 10 */
    long    flags;          /* 14 */
    long    file;           /* 18 */
    int     charbuf;        /* 1C */
    int     bufsiz;         /* 20 */
    int     pad24;          /* 24 */
    char*   tmpfname;       /* 28 */
    CRITICAL_SECTION cs;    /* 30 .. 58 */
    /* ours */
    HANDLE   h;
    uint64_t pos;           /* logical position */
    uint64_t hpos;          /* OS file pointer, UINT64_MAX if unknown */
    uint64_t bufStart;
    uint32_t bufLen;
    int      eof;
    int      err;
    int      pushback;      /* -1 = none */
    unsigned char* buf;
    uint32_t nextFree;
} SStream;

static unsigned char* g_pool = NULL;
static SRWLOCK g_poolLock = SRWLOCK_INIT;
static uint32_t g_freeHead = 0xFFFFFFFF;
static uint32_t g_highWater = 0;
static volatile LONG g_open = 0, g_peak = 0, g_totalOpened = 0, g_passPlugin = 0;

static __inline int isOurs(const void* f)
{
    return g_pool && (const unsigned char*)f >= g_pool && (const unsigned char*)f < g_pool + (size_t)MAX_SLOTS * SLOT_SIZE;
}

static SStream* allocStream(void)
{
    SStream* s = NULL;
    AcquireSRWLockExclusive(&g_poolLock);
    if (g_freeHead != 0xFFFFFFFF) {
        s = (SStream*)(g_pool + (size_t)g_freeHead * SLOT_SIZE);
        g_freeHead = s->nextFree;
    } else if (g_highWater < MAX_SLOTS) {
        unsigned char* p = g_pool + (size_t)g_highWater * SLOT_SIZE;
        if (VirtualAlloc(p, SLOT_SIZE, MEM_COMMIT, PAGE_READWRITE)) { s = (SStream*)p; g_highWater++; }
    }
    ReleaseSRWLockExclusive(&g_poolLock);
    if (s) {
        memset(s, 0, sizeof(*s));
        s->file = -1;
        s->pushback = -1;
        s->hpos = UINT64_MAX;
        InitializeCriticalSectionAndSpinCount(&s->cs, 1000);
    }
    return s;
}

static void freeStream(SStream* s)
{
    DeleteCriticalSection(&s->cs);
    uint32_t idx = (uint32_t)(((unsigned char*)s - g_pool) / SLOT_SIZE);
    memset(s, 0, sizeof(*s));
    AcquireSRWLockExclusive(&g_poolLock);
    s->nextFree = g_freeHead;
    g_freeHead = idx;
    ReleaseSRWLockExclusive(&g_poolLock);
}

/* ---------------- UCRT errno ---------------- */
typedef int* (__cdecl* errno_fn)(void);
static errno_fn p_errno = NULL;
static void setErrno(int e) { if (p_errno) *p_errno() = e; }

static int mapErr(DWORD e)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH: case ERROR_BAD_PATHNAME: case ERROR_FILENAME_EXCED_RANGE: return 2;   /* ENOENT */
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return 13; /* EACCES */
    case ERROR_TOO_MANY_OPEN_FILES: return 24;  /* EMFILE */
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return 12;
    default: return 22;
    }
}

/* ---------------- originals ---------------- */
typedef FILE* (__cdecl* fopen_t)(const char*, const char*);
typedef int (__cdecl* fopen_s_t)(FILE**, const char*, const char*);
typedef int (__cdecl* wfopen_s_t)(FILE**, const wchar_t*, const wchar_t*);
typedef int (__cdecl* fclose_t)(FILE*);
typedef size_t (__cdecl* fread_t)(void*, size_t, size_t, FILE*);
typedef size_t (__cdecl* fwrite_t)(const void*, size_t, size_t, FILE*);
typedef int (__cdecl* fseek_t)(FILE*, long, int);
typedef int (__cdecl* fseeki64_t)(FILE*, long long, int);
typedef long (__cdecl* ftell_t)(FILE*);
typedef int (__cdecl* f_int_t)(FILE*);
typedef char* (__cdecl* fgets_t)(char*, int, FILE*);
typedef int (__cdecl* fgetpos_t)(FILE*, fpos_t*);
typedef int (__cdecl* fsetpos_t)(FILE*, const fpos_t*);
typedef void (__cdecl* rewind_t)(FILE*);
typedef int (__cdecl* setvbuf_t)(FILE*, char*, int, size_t);
typedef int (__cdecl* ungetc_t)(int, FILE*);
typedef int (__cdecl* fputc_t)(int, FILE*);
typedef void (__cdecl* lock_t)(FILE*);
typedef int (__cdecl* gsbp_t)(FILE*, char***, char***, int**);
typedef int (__cdecl* common_t)(unsigned long long, FILE*, const void*, void*, va_list);

static fopen_t o_fopen; static fopen_s_t o_fopen_s; static wfopen_s_t o_wfopen_s;
static fclose_t o_fclose; static fread_t o_fread; static fwrite_t o_fwrite;
static fseek_t o_fseek; static fseeki64_t o_fseeki64; static ftell_t o_ftell;
static f_int_t o_feof, o_fgetc, o_fflush; static fgets_t o_fgets;
static fgetpos_t o_fgetpos; static fsetpos_t o_fsetpos; static rewind_t o_rewind;
static setvbuf_t o_setvbuf; static ungetc_t o_ungetc; static fputc_t o_fputc;
static lock_t o_lock, o_unlock; static gsbp_t o_gsbp;
static common_t o_vfprintf, o_vfprintf_s, o_vfprintf_p, o_vfwprintf, o_vfwprintf_s, o_vfwprintf_p, o_vfscanf, o_vfwscanf;

/* ---------------- filters ---------------- */
static int isPluginName(const char* n)
{
    size_t l = strlen(n);
    if (l < 4) return 0;
    const char* e = n + l - 4;
    return e[0] == '.' && (e[1] | 32) == 'e' && (e[2] | 32) == 's' &&
           ((e[3] | 32) == 'p' || (e[3] | 32) == 'm' || (e[3] | 32) == 'l');
}
static int isPluginNameW(const wchar_t* n)
{
    size_t l = wcslen(n);
    if (l < 4) return 0;
    const wchar_t* e = n + l - 4;
    return e[0] == L'.' && (e[1] | 32) == L'e' && (e[2] | 32) == L's' &&
           ((e[3] | 32) == L'p' || (e[3] | 32) == L'm' || (e[3] | 32) == L'l');
}
static int isReadBinary(const char* m)
{
    if (!m || m[0] != 'r') return 0;
    int b = 0;
    for (const char* p = m; *p && *p != ','; ++p) {
        if (*p == '+' || *p == 'w' || *p == 'a') return 0;
        if (*p == 'b') b = 1;
        if (*p == 't') return 0;
    }
    return b;
}
static int isReadBinaryW(const wchar_t* m)
{
    if (!m || m[0] != L'r') return 0;
    int b = 0;
    for (const wchar_t* p = m; *p && *p != L','; ++p) {
        if (*p == L'+' || *p == L'w' || *p == L'a') return 0;
        if (*p == L'b') b = 1;
        if (*p == L't') return 0;
    }
    return b;
}

static void notePass(const char* name, const char* mode)
{
    LONG n = InterlockedIncrement(&g_passPlugin);
    if (n <= 5) slog("note: plugin opened in mode \"%s\", left to the CRT: %s", mode ? mode : "(null)", name ? name : "(null)");
}

/* ---------------- open ---------------- */
static SStream* openOurs(HANDLE h, const char* nameForLog)
{
    SStream* s = allocStream();
    if (!s) { CloseHandle(h); return NULL; }
    s->h = h;
    s->hpos = 0;
    LONG o = InterlockedIncrement(&g_open);
    LONG t = InterlockedIncrement(&g_totalOpened);
    LONG pk;
    while ((pk = g_peak) < o && InterlockedCompareExchange(&g_peak, o, pk) != pk) {}
    if (t <= 3) slog("taking over: %s", nameForLog);
    if (o % 1000 == 0) slog("plugin streams held outside the CRT: %ld open (peak %ld, %ld opened in total)", o, g_peak, t);
    return s;
}

static SStream* tryOpenA(const char* name, int* err)
{
    HANDLE h = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { *err = mapErr(GetLastError()); return NULL; }
    SStream* s = openOurs(h, name);
    if (!s) *err = 24;
    return s;
}

static FILE* __cdecl h_fopen(const char* name, const char* mode)
{
    if (name && isPluginName(name)) {
        if (isReadBinary(mode)) {
            int e = 0;
            SStream* s = tryOpenA(name, &e);
            if (s) return (FILE*)s;
            if (e != 24) { setErrno(e); return NULL; }
        } else notePass(name, mode);
    }
    return o_fopen(name, mode);
}

static int __cdecl h_fopen_s(FILE** pf, const char* name, const char* mode)
{
    if (pf && name && isPluginName(name)) {
        if (isReadBinary(mode)) {
            int e = 0;
            SStream* s = tryOpenA(name, &e);
            if (s) { *pf = (FILE*)s; return 0; }
            if (e != 24) { *pf = NULL; setErrno(e); return e; }
        } else notePass(name, mode);
    }
    return o_fopen_s(pf, name, mode);
}

static int __cdecl h_wfopen_s(FILE** pf, const wchar_t* name, const wchar_t* mode)
{
    if (pf && name && isPluginNameW(name)) {
        if (isReadBinaryW(mode)) {
            HANDLE h = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE) { int e = mapErr(GetLastError()); *pf = NULL; setErrno(e); return e; }
            SStream* s = openOurs(h, "(wide name)");
            if (s) { *pf = (FILE*)s; return 0; }
        } else notePass("(wide name)", "(wide mode)");
    }
    return o_wfopen_s(pf, name, mode);
}

/* ---------------- I/O on ours (caller holds cs) ---------------- */
static int osSeek(SStream* s, uint64_t at)
{
    if (s->hpos == at) return 1;
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)at;
    if (!SetFilePointerEx(s->h, li, NULL, FILE_BEGIN)) { s->hpos = UINT64_MAX; return 0; }
    s->hpos = at;
    return 1;
}

static uint32_t osRead(SStream* s, uint64_t at, void* dst, uint32_t n)
{
    if (!osSeek(s, at)) { s->err = 1; return 0; }
    DWORD got = 0;
    if (!ReadFile(s->h, dst, n, &got, NULL)) { s->err = 1; s->hpos = UINT64_MAX; return 0; }
    s->hpos = at + got;
    return got;
}

static size_t readBytes(SStream* s, unsigned char* out, size_t want)
{
    size_t done = 0;
    if (want && s->pushback >= 0) {
        out[done++] = (unsigned char)s->pushback;
        s->pushback = -1;
        s->pos++;
    }
    while (done < want) {
        if (s->buf && s->pos >= s->bufStart && s->pos < s->bufStart + s->bufLen) {
            size_t off = (size_t)(s->pos - s->bufStart);
            size_t n = s->bufLen - off;
            if (n > want - done) n = want - done;
            memcpy(out + done, s->buf + off, n);
            done += n; s->pos += n;
            continue;
        }
        size_t left = want - done;
        if (left >= BUF_SIZE) {
            uint32_t chunk = left > 0x40000000 ? 0x40000000 : (uint32_t)left;
            uint32_t got = osRead(s, s->pos, out + done, chunk);
            done += got; s->pos += got;
            if (got < chunk) { if (!s->err) s->eof = 1; break; }
            continue;
        }
        if (!s->buf) {
            s->buf = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, BUF_SIZE);
            if (!s->buf) { s->err = 1; break; }
        }
        uint32_t got = osRead(s, s->pos, s->buf, BUF_SIZE);
        s->bufStart = s->pos; s->bufLen = got;
        if (got == 0) { if (!s->err) s->eof = 1; break; }
    }
    return done;
}

static int seekOurs(SStream* s, long long off, int origin)
{
    long long base;
    if (origin == SEEK_SET) base = 0;
    else if (origin == SEEK_CUR) base = (long long)s->pos;
    else if (origin == SEEK_END) {
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(s->h, &sz)) { setErrno(22); return -1; }
        base = sz.QuadPart;
    } else { setErrno(22); return -1; }
    long long np = base + off;
    if (np < 0) { setErrno(22); return -1; }
    s->pos = (uint64_t)np;
    s->pushback = -1;
    s->eof = 0;
    return 0;
}

/* ---------------- hooks ---------------- */
static int __cdecl h_fclose(FILE* f)
{
    if (!isOurs(f)) return o_fclose(f);
    SStream* s = (SStream*)f;
    EnterCriticalSection(&s->cs);
    CloseHandle(s->h);
    if (s->buf) HeapFree(GetProcessHeap(), 0, s->buf);
    s->h = NULL; s->buf = NULL;
    LeaveCriticalSection(&s->cs);
    freeStream(s);
    InterlockedDecrement(&g_open);
    return 0;
}

static size_t __cdecl h_fread(void* dst, size_t size, size_t count, FILE* f)
{
    if (!isOurs(f)) return o_fread(dst, size, count, f);
    if (size == 0 || count == 0) return 0;
    if (!dst) { setErrno(22); return 0; }
    SStream* s = (SStream*)f;
    EnterCriticalSection(&s->cs);
    size_t total = size * count;
    size_t got = readBytes(s, (unsigned char*)dst, total);
    LeaveCriticalSection(&s->cs);
    return got / size;
}

static size_t __cdecl h_fwrite(const void* src, size_t size, size_t count, FILE* f)
{
    if (!isOurs(f)) return o_fwrite(src, size, count, f);
    SStream* s = (SStream*)f;
    EnterCriticalSection(&s->cs); s->err = 1; LeaveCriticalSection(&s->cs);
    setErrno(9);
    return 0;
}

static int __cdecl h_fseek(FILE* f, long off, int origin)
{
    if (!isOurs(f)) return o_fseek(f, off, origin);
    SStream* s = (SStream*)f;
    EnterCriticalSection(&s->cs);
    int r = seekOurs(s, off, origin);
    LeaveCriticalSection(&s->cs);
    return r;
}

static int __cdecl h_fseeki64(FILE* f, long long off, int origin)
{
    if (!isOurs(f)) return o_fseeki64(f, off, origin);
    SStream* s = (SStream*)f;
    EnterCriticalSection(&s->cs);
    int r = seekOurs(s, off, origin);
    LeaveCriticalSection(&s->cs);
    return r;
}

static long __cdecl h_ftell(FILE* f)
{
    if (!isOurs(f)) return o_ftell(f);
    SStream* s = (SStream*)f;
    EnterCriticalSection(&s->cs);
    uint64_t p = s->pos;
    LeaveCriticalSection(&s->cs);
    if (p > 0x7FFFFFFF) { setErrno(22); return -1L; }
    return (long)p;
}

static int __cdecl h_feof(FILE* f)
{
    if (!isOurs(f)) return o_feof(f);
    return ((SStream*)f)->eof ? 0x10 : 0;
}

static int __cdecl h_fgetc(FILE* f)
{
    if (!isOurs(f)) return o_fgetc(f);
    SStream* s = (SStream*)f;
    unsigned char c;
    EnterCriticalSection(&s->cs);
    size_t n = readBytes(s, &c, 1);
    LeaveCriticalSection(&s->cs);
    return n ? (int)c : EOF;
}

static char* __cdecl h_fgets(char* dst, int n, FILE* f)
{
    if (!isOurs(f)) return o_fgets(dst, n, f);
    if (!dst || n <= 0) { setErrno(22); return NULL; }
    SStream* s = (SStream*)f;
    EnterCriticalSection(&s->cs);
    int i = 0;
    while (i < n - 1) {
        unsigned char c;
        if (!readBytes(s, &c, 1)) break;
        dst[i++] = (char)c;
        if (c == '\n') break;
    }
    LeaveCriticalSection(&s->cs);
    if (i == 0 && n > 1) return NULL;
    dst[i] = 0;
    return dst;
}

static int __cdecl h_fgetpos(FILE* f, fpos_t* p)
{
    if (!isOurs(f)) return o_fgetpos(f, p);
    if (!p) { setErrno(22); return -1; }
    SStream* s = (SStream*)f;
    EnterCriticalSection(&s->cs);
    *p = (fpos_t)s->pos;
    LeaveCriticalSection(&s->cs);
    return 0;
}

static int __cdecl h_fsetpos(FILE* f, const fpos_t* p)
{
    if (!isOurs(f)) return o_fsetpos(f, p);
    if (!p) { setErrno(22); return -1; }
    SStream* s = (SStream*)f;
    EnterCriticalSection(&s->cs);
    int r = seekOurs(s, (long long)*p, SEEK_SET);
    LeaveCriticalSection(&s->cs);
    return r;
}

static void __cdecl h_rewind(FILE* f)
{
    if (!isOurs(f)) { o_rewind(f); return; }
    SStream* s = (SStream*)f;
    EnterCriticalSection(&s->cs);
    seekOurs(s, 0, SEEK_SET);
    s->err = 0;
    LeaveCriticalSection(&s->cs);
}

static int __cdecl h_fflush(FILE* f)
{
    if (f == NULL || !isOurs(f)) return o_fflush(f);
    return 0;
}

static int __cdecl h_setvbuf(FILE* f, char* b, int mode, size_t sz)
{
    if (!isOurs(f)) return o_setvbuf(f, b, mode, sz);
    return 0;
}

static int __cdecl h_ungetc(int c, FILE* f)
{
    if (!isOurs(f)) return o_ungetc(c, f);
    if (c == EOF) return EOF;
    SStream* s = (SStream*)f;
    EnterCriticalSection(&s->cs);
    int r = EOF;
    if (s->pushback < 0) {
        s->pushback = (unsigned char)c;
        if (s->pos > 0) s->pos--;
        s->eof = 0;
        r = (unsigned char)c;
    }
    LeaveCriticalSection(&s->cs);
    return r;
}

static int __cdecl h_fputc(int c, FILE* f)
{
    if (!isOurs(f)) return o_fputc(c, f);
    setErrno(9);
    return EOF;
}

static void __cdecl h_lock(FILE* f) { if (!isOurs(f)) o_lock(f); else EnterCriticalSection(&((SStream*)f)->cs); }
static void __cdecl h_unlock(FILE* f) { if (!isOurs(f)) o_unlock(f); else LeaveCriticalSection(&((SStream*)f)->cs); }

static int __cdecl h_gsbp(FILE* f, char*** b, char*** p, int** c)
{
    if (!isOurs(f)) return o_gsbp(f, b, p, c);
    return 22;
}

#define COMMON_HOOK(name) \
    static int __cdecl h_##name(unsigned long long o, FILE* f, const void* fmt, void* loc, va_list a) \
    { if (!isOurs(f)) return o_##name(o, f, fmt, loc, a); setErrno(9); return -1; }
COMMON_HOOK(vfprintf) COMMON_HOOK(vfprintf_s) COMMON_HOOK(vfprintf_p)
COMMON_HOOK(vfwprintf) COMMON_HOOK(vfwprintf_s) COMMON_HOOK(vfwprintf_p)
COMMON_HOOK(vfscanf) COMMON_HOOK(vfwscanf)

/* ---------------- IAT patching ---------------- */
typedef struct { const char* name; void* hook; void** orig; int required; } HookDef;

static HookDef g_hooks[] = {
    { "fopen", (void*)h_fopen, (void**)&o_fopen, 0 },
    { "fopen_s", (void*)h_fopen_s, (void**)&o_fopen_s, 0 },
    { "_wfopen_s", (void*)h_wfopen_s, (void**)&o_wfopen_s, 0 },
    { "fclose", (void*)h_fclose, (void**)&o_fclose, 1 },
    { "fread", (void*)h_fread, (void**)&o_fread, 1 },
    { "fwrite", (void*)h_fwrite, (void**)&o_fwrite, 1 },
    { "fseek", (void*)h_fseek, (void**)&o_fseek, 1 },
    { "_fseeki64", (void*)h_fseeki64, (void**)&o_fseeki64, 1 },
    { "ftell", (void*)h_ftell, (void**)&o_ftell, 1 },
    { "feof", (void*)h_feof, (void**)&o_feof, 1 },
    { "fgetc", (void*)h_fgetc, (void**)&o_fgetc, 1 },
    { "fgets", (void*)h_fgets, (void**)&o_fgets, 1 },
    { "fgetpos", (void*)h_fgetpos, (void**)&o_fgetpos, 1 },
    { "fsetpos", (void*)h_fsetpos, (void**)&o_fsetpos, 1 },
    { "rewind", (void*)h_rewind, (void**)&o_rewind, 1 },
    { "fflush", (void*)h_fflush, (void**)&o_fflush, 1 },
    { "setvbuf", (void*)h_setvbuf, (void**)&o_setvbuf, 1 },
    { "ungetc", (void*)h_ungetc, (void**)&o_ungetc, 1 },
    { "fputc", (void*)h_fputc, (void**)&o_fputc, 1 },
    { "_lock_file", (void*)h_lock, (void**)&o_lock, 1 },
    { "_unlock_file", (void*)h_unlock, (void**)&o_unlock, 1 },
    { "_get_stream_buffer_pointers", (void*)h_gsbp, (void**)&o_gsbp, 1 },
    { "__stdio_common_vfprintf", (void*)h_vfprintf, (void**)&o_vfprintf, 1 },
    { "__stdio_common_vfprintf_s", (void*)h_vfprintf_s, (void**)&o_vfprintf_s, 1 },
    { "__stdio_common_vfprintf_p", (void*)h_vfprintf_p, (void**)&o_vfprintf_p, 1 },
    { "__stdio_common_vfwprintf", (void*)h_vfwprintf, (void**)&o_vfwprintf, 1 },
    { "__stdio_common_vfwprintf_s", (void*)h_vfwprintf_s, (void**)&o_vfwprintf_s, 1 },
    { "__stdio_common_vfwprintf_p", (void*)h_vfwprintf_p, (void**)&o_vfwprintf_p, 1 },
    { "__stdio_common_vfscanf", (void*)h_vfscanf, (void**)&o_vfscanf, 1 },
    { "__stdio_common_vfwscanf", (void*)h_vfwscanf, (void**)&o_vfwscanf, 1 },
};
#define NHOOKS (sizeof(g_hooks) / sizeof(g_hooks[0]))

typedef struct { void** slot; HookDef* def; } Found;

static int isCrtDll(const char* n)
{
    char l[64]; size_t i = 0;
    for (; n[i] && i < sizeof(l) - 1; ++i) l[i] = (char)(n[i] | ((n[i] >= 'A' && n[i] <= 'Z') ? 32 : 0));
    l[i] = 0;
    return strncmp(l, "api-ms-win-crt-", 15) == 0 || strcmp(l, "ucrtbase.dll") == 0;
}

static int patchIAT(void)
{
    unsigned char* base = (unsigned char*)GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) { slog("no import table"); return 0; }

    Found found[NHOOKS]; memset(found, 0, sizeof(found));
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        if (!isCrtDll((const char*)(base + d->Name)) || !d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA64* names = (IMAGE_THUNK_DATA64*)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA64* slots = (IMAGE_THUNK_DATA64*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const char* fn = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            for (size_t i = 0; i < NHOOKS; ++i)
                if (!found[i].slot && strcmp(fn, g_hooks[i].name) == 0) { found[i].slot = (void**)&slots->u1.Function; found[i].def = &g_hooks[i]; }
        }
    }

    /* every FILE*-taking function the game imports must be covered, or ours could reach the real CRT */
    int haveOpen = 0;
    for (size_t i = 0; i < NHOOKS; ++i) {
        if (found[i].slot && !g_hooks[i].required) haveOpen = 1;
        if (!found[i].slot && g_hooks[i].required) slog("note: game does not import %s (fine)", g_hooks[i].name);
    }
    if (!haveOpen) { slog("game imports no fopen variant; nothing to do"); return 0; }

    /* fill originals first, then flip open-functions last so no stream of ours exists before all I/O hooks are live */
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < NHOOKS; ++i) {
            if (!found[i].slot) continue;
            int isOpen = !g_hooks[i].required;
            if ((pass == 0) == isOpen) continue;
            DWORD old;
            if (!VirtualProtect(found[i].slot, sizeof(void*), PAGE_READWRITE, &old)) { slog("VirtualProtect failed for %s", g_hooks[i].name); return 0; }
            *g_hooks[i].orig = *found[i].slot;
            InterlockedExchangePointer(found[i].slot, g_hooks[i].hook);
            VirtualProtect(found[i].slot, sizeof(void*), old, &old);
        }
    }
    /* any required hook the game doesn't import still needs a valid orig for safety */
    for (size_t i = 0; i < NHOOKS; ++i)
        if (!*g_hooks[i].orig) *g_hooks[i].orig = (void*)GetProcAddress(GetModuleHandleW(L"ucrtbase.dll"), g_hooks[i].name);
    return 1;
}

EXPORT BOOL SKSEPlugin_Load(const void* skse)
{
    (void)skse;
    openLog();
    slog("StreamCeilingFix 1.0.1 (pid %lu)", GetCurrentProcessId());
    if (g_logIndex > 0) slog("note: StreamCeilingFix.log was locked by another process (a game that did not fully exit?), logging here instead");
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    if (ucrt) p_errno = (errno_fn)(void*)GetProcAddress(ucrt, "_errno");
    g_pool = (unsigned char*)VirtualAlloc(NULL, (size_t)MAX_SLOTS * SLOT_SIZE, MEM_RESERVE, PAGE_READWRITE);
    if (!g_pool) { slog("could not reserve stream pool; disabled"); return TRUE; }
    if (patchIAT()) slog("active: the game's read-only plugin file opens no longer use C runtime stream slots");
    else slog("NOT active");
    return TRUE;
}

/* SKSE for 1.5.97 asks this first; AE uses SKSEPlugin_Version above. */
typedef struct { uint32_t infoVersion; const char* name; uint32_t version; } SKSEPluginInfo;
EXPORT BOOL SKSEPlugin_Query(const void* skse, SKSEPluginInfo* info)
{
    (void)skse;
    info->infoVersion = 1;
    info->name = "StreamCeilingFix";
    info->version = 0x01000100;
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { (void)h; (void)r; (void)p; return TRUE; }
