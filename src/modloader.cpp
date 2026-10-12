#include "modloader.h"
#include "iat.h"
#include "log.h"
#include "pathkey.h"

#include <windows.h>

#include <string>
#include <unordered_map>

namespace modloader {
namespace {

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                      HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                      HANDLE);

CreateFileWFn g_realW = nullptr;
CreateFileAFn g_realA = nullptr;

std::unordered_map<std::wstring, std::wstring> g_overrides; // key -> full path
std::wstring g_gameDir;

const std::wstring* Lookup(const wchar_t* path, DWORD access, DWORD disposition)
{
    // reads of existing files only, or the game would write saves and settings into mods
    if (g_overrides.empty() || disposition != OPEN_EXISTING || (access & GENERIC_WRITE))
        return nullptr;
    auto it = g_overrides.find(pathkey::Make(path, g_gameDir));
    return it == g_overrides.end() ? nullptr : &it->second;
}

HANDLE WINAPI HookedCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                DWORD disposition, DWORD flags, HANDLE tmpl)
{
    const std::wstring* to = Lookup(name, access, disposition);
    return g_realW(to ? to->c_str() : name, access, share, sa, disposition, flags, tmpl);
}

HANDLE WINAPI HookedCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                DWORD disposition, DWORD flags, HANDLE tmpl)
{
    wchar_t wide[MAX_PATH * 2];
    if (name && MultiByteToWideChar(CP_ACP, 0, name, -1, wide, ARRAYSIZE(wide)) > 0) {
        // an override path may not survive a round trip through the ansi code page
        if (const std::wstring* to = Lookup(wide, access, disposition))
            return g_realW(to->c_str(), access, share, sa, disposition, flags, tmpl);
    }
    return g_realA(name, access, share, sa, disposition, flags, tmpl);
}

void IndexDir(const std::wstring& dir, const std::wstring& prefix)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        const std::wstring full = dir + L"\\" + name;
        const std::wstring rel = prefix.empty() ? name : prefix + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            IndexDir(full, rel);
        else
            g_overrides[pathkey::Lower(rel)] = full;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

DWORD WINAPI Startup(LPVOID)
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir.erase(dir.find_last_of(L'\\'));
    const std::wstring mods = dir + L"\\mods";
    g_gameDir = pathkey::Lower(dir);
    if (GetFileAttributesW(mods.c_str()) == INVALID_FILE_ATTRIBUTES)
        CreateDirectoryW(mods.c_str(), nullptr);

    // index before hooking, so the map is complete and read-only once a redirect can fire
    IndexDir(mods, L"");
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    g_realW = reinterpret_cast<CreateFileWFn>(GetProcAddress(k32, "CreateFileW"));
    g_realA = reinterpret_cast<CreateFileAFn>(GetProcAddress(k32, "CreateFileA"));
    const auto hookW = reinterpret_cast<uintptr_t>(&HookedCreateFileW);
    const auto hookA = reinterpret_cast<uintptr_t>(&HookedCreateFileA);

    // the ea wrapper may resolve a slot to kernelbase's export rather than kernel32's
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    const auto baseW = kb ? reinterpret_cast<uintptr_t>(GetProcAddress(kb, "CreateFileW")) : 0;
    const auto baseA = kb ? reinterpret_cast<uintptr_t>(GetProcAddress(kb, "CreateFileA")) : 0;
    const auto sweep = [&](size_t& w, size_t& a) {
        if (!w)
            w = iat::Replace(reinterpret_cast<uintptr_t>(g_realW), hookW) + (baseW ? iat::Replace(baseW, hookW) : 0);
        if (!a)
            a = iat::Replace(reinterpret_cast<uintptr_t>(g_realA), hookA) + (baseA ? iat::Replace(baseA, hookA) : 0);
    };

    // every slot holding the export, including ones the ea wrapper fills in after we load
    size_t w = 0, a = 0;
    sweep(w, a);
    if (!w || !a) {
        iat::Pending p[2] = {{"CreateFileW", reinterpret_cast<uintptr_t>(g_realW), hookW},
                             {"CreateFileA", reinterpret_cast<uintptr_t>(g_realA), hookA}};
        iat::ReplaceWhenResolved(w ? p + 1 : p, (w || a) ? 1 : 2, 60000);
        w += p[0].patched;
        a += p[1].patched;
        // a slot resolved outside the watch list (to the other module's export) is caught here
        for (const ULONGLONG end = GetTickCount64() + 30000; (!w || !a) && GetTickCount64() < end; SwitchToThread())
            sweep(w, a);
    }
    LOGF("[mods] %zu override(s) under mods, CreateFileW x%zu, CreateFileA x%zu",
         g_overrides.size(), w, a);
    return 0;
}

} // namespace

void Init()
{
    if (HANDLE t = CreateThread(nullptr, 0, Startup, nullptr, 0, nullptr))
        CloseHandle(t);
}

} // namespace modloader
