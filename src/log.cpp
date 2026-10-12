#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace fixlog {
namespace {

char g_path[MAX_PATH];
bool g_enabled = false;
SRWLOCK g_lock = SRWLOCK_INIT;

} // namespace

void Init(HMODULE self, bool enabled)
{
    g_enabled = enabled;
    GetModuleFileNameA(self, g_path, MAX_PATH);
    if (char* dot = strrchr(g_path, '.'))
        strcpy_s(dot, g_path + MAX_PATH - dot, ".log");
    DeleteFileA(g_path);
}

void Write(const char* fmt, ...)
{
    if (!g_enabled)
        return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > static_cast<int>(sizeof(line)) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    // reopened per line so a crash keeps the tail; shared so it can be read live
    AcquireSRWLockExclusive(&g_lock);
    HANDLE h = CreateFileA(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(h, line, static_cast<DWORD>(n), &written, nullptr);
        CloseHandle(h);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace fixlog
