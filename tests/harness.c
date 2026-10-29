// Self-checking test harness for StreamCeilingFix.
// Must be linked against the Universal CRT (like SkyrimSE.exe) so the plugin finds the
// api-ms-win-crt-stdio imports it patches. See the Makefile ("make test", runs under Wine).
//
//   harness.exe          run all tests
//   harness.exe lock     also hold StreamCeilingFix.log open the way a stuck game process would,
//                        and check the plugin falls back to StreamCeilingFix.1.log
//
// Exit code = number of failed checks.

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef struct { unsigned infoVersion; const char* name; unsigned version; } PluginInfo;
typedef BOOL (*query_t)(const void*, PluginInfo*);
typedef BOOL (*load_t)(const void*);

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) { printf("  ok   "); } else { printf("  FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

#define FILE_SIZE 300000
#define HOLD      9000

static unsigned char ref[FILE_SIZE];

static int readFile(const wchar_t* path, char* out, int cap)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD n = 0; ReadFile(h, out, cap - 1, &n, NULL); out[n] = 0; CloseHandle(h);
    return (int)n;
}

int main(int argc, char** argv)
{
    int lockMode = argc > 1 && strcmp(argv[1], "lock") == 0;
    wchar_t dir[600], logMain[700], logAlt[700];
    SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, dir);
    wcscat(dir, L"\\My Games\\Skyrim Special Edition\\SKSE");
    SHCreateDirectoryExW(NULL, dir, NULL);
    swprintf(logMain, 700, L"%ls\\StreamCeilingFix.log", dir);
    swprintf(logAlt, 700, L"%ls\\StreamCeilingFix.1.log", dir);
    DeleteFileW(logAlt);

    HANDLE lock = INVALID_HANDLE_VALUE;
    if (lockMode) {
        lock = CreateFileW(logMain, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
        printf("holding StreamCeilingFix.log like a stuck game process: %s\n", lock != INVALID_HANDLE_VALUE ? "yes" : "no");
    }

    printf("plugin entry points\n");
    HMODULE m = LoadLibraryA("StreamCeilingFix.dll");
    CHECK(m != NULL, "LoadLibrary");
    if (!m) return 1;
    CHECK(GetProcAddress(m, "SKSEPlugin_Version") != NULL, "SKSEPlugin_Version export (AE)");
    query_t q = (query_t)GetProcAddress(m, "SKSEPlugin_Query");
    PluginInfo pi; memset(&pi, 0, sizeof(pi));
    int qr = q ? q(NULL, &pi) : 0;
    CHECK(qr && pi.name && strcmp(pi.name, "StreamCeilingFix") == 0, "SKSEPlugin_Query (SE 1.5.97) name=%s version=%08x", pi.name ? pi.name : "?", pi.version);
    load_t ld = (load_t)GetProcAddress(m, "SKSEPlugin_Load");
    CHECK(ld && ld(NULL), "SKSEPlugin_Load");

    static char logbuf[65536];
    if (lockMode) {
        CHECK(readFile(logAlt, logbuf, sizeof(logbuf)) > 0 && strstr(logbuf, "was locked by another process"), "fell back to StreamCeilingFix.1.log with a note");
    } else {
        CHECK(readFile(logMain, logbuf, sizeof(logbuf)) > 0, "StreamCeilingFix.log written");
    }
    CHECK(strstr(logbuf, "active:") != NULL, "log says active");

    printf("write mode passes through to the CRT\n");
    srand(12345);
    for (int i = 0; i < FILE_SIZE; ++i) ref[i] = (unsigned char)(rand() >> 3);
    FILE* w = fopen("test.esp", "wb");
    CHECK(w != NULL, "fopen(\"test.esp\", \"wb\")");
    if (!w) return g_fail;
    CHECK(fwrite(ref, 1, FILE_SIZE, w) == FILE_SIZE, "fwrite %d bytes", FILE_SIZE);
    fclose(w);

    printf("hold %d plugin streams at once (UCRT default limit is 512, max 8192)\n", HOLD);
    static FILE* f[HOLD];
    int opened = 0;
    for (int i = 0; i < HOLD; ++i) { f[i] = fopen("test.esp", "rb"); opened += f[i] != NULL; }
    CHECK(opened == HOLD, "%d/%d opened", opened, HOLD);

    FILE* plain = fopen("plain.txt", "w");
    CHECK(plain != NULL, "a non-plugin file still opens while %d plugin streams are held", HOLD);
    if (plain) { fputs("hello\n", plain); fclose(plain); }

    printf("reads, seeks and tells match the file (random ops on 50 streams)\n");
    int bad = 0, ops = 0;
    unsigned char buf[4096];
    for (int k = 0; k < 50 && opened == HOLD; ++k) {
        FILE* s = f[k * (HOLD / 50)];
        long pos = 0; rewind(s);
        for (int i = 0; i < 4000; ++i, ++ops) {
            int op = rand() % 7;
            if (op == 0) { long to = rand() % (FILE_SIZE + 10); if (fseek(s, to, SEEK_SET) == 0) pos = to; else bad++; }
            else if (op == 1) { long long to = rand() % FILE_SIZE; if (_fseeki64(s, to, SEEK_SET) == 0) pos = (long)to; else bad++; }
            else if (op == 2) { long d = (rand() % 2000) - 1000; if (pos + d >= 0 && fseek(s, d, SEEK_CUR) == 0) pos += d; }
            else if (op == 3) { size_t want = rand() % sizeof(buf); size_t got = fread(buf, 1, want, s);
                                size_t exp = pos >= FILE_SIZE ? 0 : (size_t)(FILE_SIZE - pos) < want ? (size_t)(FILE_SIZE - pos) : want;
                                if (got != exp || (got && memcmp(buf, ref + pos, got))) bad++; pos += (long)got; }
            else if (op == 4) { int c = fgetc(s); int exp = pos < FILE_SIZE ? ref[pos] : EOF; if (c != exp) bad++; if (c != EOF) pos++;
                                if (c != EOF && rand() % 4 == 0) { if (ungetc(c, s) != c) bad++; else pos--; } }
            else if (op == 5) { if (ftell(s) != pos) bad++; }
            else { fpos_t fp; if (fgetpos(s, &fp) != 0) bad++; else { fseek(s, 7, SEEK_SET); if (fsetpos(s, &fp) != 0 || ftell(s) != pos) bad++; } }
        }
        if (fseek(s, 0, SEEK_END) != 0 || ftell(s) != FILE_SIZE) bad++;
        if (fgetc(s) != EOF || !feof(s)) bad++;
    }
    CHECK(bad == 0, "%d random operations, %d mismatches", ops, bad);

    int closed = 0;
    for (int i = 0; i < HOLD; ++i) if (f[i]) closed += fclose(f[i]) == 0;
    CHECK(closed == opened, "fclose all %d", closed);

    printf("other open variants and errors\n");
    FILE* s2 = NULL;
    CHECK(fopen_s(&s2, "test.esp", "rb") == 0 && s2 && fgetc(s2) == ref[0], "fopen_s");
    if (s2) fclose(s2);
    FILE* s3 = NULL;
    CHECK(_wfopen_s(&s3, L"test.esp", L"rb") == 0 && s3 && fgetc(s3) == ref[0], "_wfopen_s");
    if (s3) fclose(s3);
    errno = 0;
    FILE* missing = fopen("does-not-exist.esm", "rb");
    CHECK(missing == NULL && errno == ENOENT, "missing plugin returns NULL with ENOENT (errno=%d)", errno);
    FILE* t = fopen("plain.txt", "r");
    char line[32] = {0};
    CHECK(t && fgets(line, sizeof(line), t) && strcmp(line, "hello\n") == 0, "non-plugin text file reads normally");
    if (t) fclose(t);

    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    DeleteFileA("test.esp"); DeleteFileA("plain.txt");
    printf("%s: %d failed\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
