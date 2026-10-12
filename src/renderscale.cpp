// the scene targets are sized from the display size times the game's own (whole-number) ssaa
// factor, so a smaller render size goes in by replacing that display size before they are built.

#include "renderscale.h"
#include "log.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace renderscale {
namespace {

SizeFn g_size = nullptr;
uint32_t g_displayW = 0;

// called from the cave with the display size the scene targets are about to be built from
void __stdcall Apply(uint32_t* wh)
{
    uint32_t w = wh[0], h = wh[1];
    g_displayW = w;
    g_size(wh[0], wh[1], w, h);
    LOGF("[scale] scene targets %ux%u for display %ux%u", w, h, wh[0], wh[1]);
    wh[0] = w;
    wh[1] = h;
}

// mov [esi+0x280],edx; movzx eax,word [eax+6]; mov [esp+?],ecx; mov [esi+0x284],eax
const uint8_t kSite[] = {0x89, 0x96, 0x80, 0x02, 0x00, 0x00, 0x0F, 0xB7, 0x40, 0x06,
                         0x89, 0x4C, 0x24, 0x00, 0x89, 0x86, 0x84, 0x02, 0x00, 0x00};

uint8_t* Find()
{
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    uint8_t* const end = base + nt->OptionalHeader.SizeOfImage;
    uint8_t* hit = nullptr;
    int n = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (uint8_t* p = base; p < end && VirtualQuery(p, &mbi, sizeof(mbi));
         p = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
        const DWORD exec = PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (mbi.State != MEM_COMMIT || !(mbi.Protect & exec) || (mbi.Protect & PAGE_GUARD))
            continue;
        uint8_t* lo = static_cast<uint8_t*>(mbi.BaseAddress);
        uint8_t* hi = lo + mbi.RegionSize - sizeof(kSite);
        for (uint8_t* q = lo; q < hi; ++q) {
            if (q[0] != 0x89 || q[1] != 0x96)
                continue;
            bool ok = true;
            for (size_t i = 0; i < sizeof(kSite) && ok; ++i)
                ok = i == 13 || q[i] == kSite[i];
            if (ok)
                hit = q, ++n;
        }
    }
    return n == 1 ? hit : nullptr;
}

bool TryPatch()
{
    uint8_t* site = Find();
    if (!site)
        return false;
    site += 14; // mov [esi+0x284],eax
    auto* cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    uint8_t body[] = {
        0x89, 0x86, 0x84, 0x02, 0x00, 0x00,       // mov [esi+0x284],eax
        0x60,                                     // pushad
        0x8D, 0x86, 0x80, 0x02, 0x00, 0x00,       // lea eax,[esi+0x280]
        0x50,                                     // push eax
        0xE8, 0, 0, 0, 0,                         // call Apply
        0x61,                                     // popad
        0xE9, 0, 0, 0, 0,                         // jmp back
    };
    const int32_t call = static_cast<int32_t>(reinterpret_cast<uint8_t*>(&Apply) - (cave + 19));
    memcpy(body + 15, &call, 4);
    const int32_t back = static_cast<int32_t>(site + 6 - (cave + sizeof(body)));
    memcpy(body + sizeof(body) - 4, &back, 4);
    memcpy(cave, body, sizeof(body));
    FlushInstructionCache(GetCurrentProcess(), cave, sizeof(body));
    uint8_t jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
    const int32_t to = static_cast<int32_t>(cave - (site + 5));
    memcpy(jmp + 1, &to, 4);
    DWORD old;
    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old);
    memcpy(site, jmp, 6);
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    return true;
}

DWORD WINAPI Waiter(LPVOID)
{
    for (int i = 0; i < 600; ++i) {
        if (TryPatch())
            return 0;
        Sleep(100);
    }
    LOGF("[scale] render target setup not found - scene left at display size");
    return 0;
}

} // namespace

void Init(SizeFn size)
{
    g_size = size;
    if (HANDLE t = CreateThread(nullptr, 0, Waiter, nullptr, 0, nullptr))
        CloseHandle(t);
}

uint32_t DisplayWidth()
{
    return g_displayW;
}

} // namespace renderscale
