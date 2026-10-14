// An Autolog request reusing a kept-alive connection the server has just ended fails before
// anything is sent, and the game reads that one failure as "not connected to the Autolog servers".
// DirtySDK already retries such a request on a fresh connection when it fails while waiting for
// the reply; the same retry is now made when it fails while sending.

#include "httpretry.h"
#include "log.h"

#include <libmem/libmem.h>
#include <windows.h>

#include <cstring>

namespace httpretry {
namespace {

// ProtoHttp's send step (esi = http ref): its failure exit sets the state to failed
const char kSendSig[] = "8B 46 54 8B 4E 60 03 46 5C 53 57 51 50 8B 06 50 33 DB E8 ?? ?? ?? ?? 8B F8 83 C4 0C 85 FF 7E 13";
constexpr size_t kFailAt = 0x4d;
// pop edi; mov [esi+0x60],ebx; mov dword [esi+0x2c],7 (failed); or eax,-1; pop ebx; ret
const uint8_t kFail[] = {0x5F, 0x89, 0x5E, 0x60, 0xC7, 0x46, 0x2C, 0x07, 0x00, 0x00, 0x00, 0x83, 0xC8, 0xFF, 0x5B, 0xC3};
// the keep-alive retry (esi = http ref, returns 1 if it reconnected): GET/HEAD only
const char kRetrySig[] = "53 33 DB 38 9E A3 0A 00 00 75 ?? 8B 46 18 83 F8 01 74 ?? 3B C3 75";

bool TryInstall()
{
    lm_module_t mod;
    if (!LM_FindModule("NFS13.exe", &mod))
        return false;
    const lm_address_t send = LM_SigScan(kSendSig, mod.base, mod.size);
    const lm_address_t retry = LM_SigScan(kRetrySig, mod.base, mod.size);
    if (send == LM_ADDRESS_BAD || retry == LM_ADDRESS_BAD)
        return false;
    auto* site = reinterpret_cast<uint8_t*>(send + kFailAt);
    if (memcmp(site, kFail, sizeof(kFail)) != 0) {
        LOGF("[httpretry] unexpected code in the http send step - retry off");
        return true;
    }
    auto* cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!cave)
        return true;
    // only a request on a reused connection (keep-alive count > 1) is retried; the retry resets
    // the count to 1, so a request is retried at most once
    uint8_t code[] = {
        0x5F,                                     // pop edi
        0x83, 0xBE, 0x88, 0x00, 0x00, 0x00, 0x01, // cmp dword [esi+0x88],1
        0x7E, 0x0D,                               // jle fail
        0xE8, 0, 0, 0, 0,                         // call retry
        0x85, 0xC0,                               // test eax,eax
        0x74, 0x04,                               // jz fail
        0x33, 0xC0,                               // xor eax,eax: nothing sent yet, not an error
        0x5B, 0xC3,                               // pop ebx; ret
        // fail: the original exit
        0x89, 0x5E, 0x60, 0xC7, 0x46, 0x2C, 0x07, 0x00, 0x00, 0x00, 0x83, 0xC8, 0xFF, 0x5B, 0xC3,
    };
    const int32_t call = static_cast<int32_t>(retry - (reinterpret_cast<uintptr_t>(cave) + 15));
    memcpy(code + 11, &call, 4);
    memcpy(cave, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), cave, sizeof(code));

    uint8_t jmp[11];
    memset(jmp, 0x90, sizeof(jmp));
    jmp[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(cave - (site + 5));
    memcpy(jmp + 1, &rel, 4);
    DWORD old;
    VirtualProtect(site, sizeof(jmp), PAGE_EXECUTE_READWRITE, &old);
    memcpy(site, jmp, sizeof(jmp));
    VirtualProtect(site, sizeof(jmp), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(jmp));
    LOGF("[httpretry] failed sends on reused connections are retried (send step %p)", reinterpret_cast<void*>(send));
    return true;
}

DWORD WINAPI Waiter(LPVOID)
{
    // the game's code is decrypted after we load
    for (int i = 0; i < 120; ++i) {
        if (TryInstall())
            return 0;
        Sleep(500);
    }
    LOGF("[httpretry] http send step not found - retry off");
    return 0;
}

} // namespace

void Init()
{
    if (HANDLE t = CreateThread(nullptr, 0, Waiter, nullptr, 0, nullptr))
        CloseHandle(t);
}

} // namespace httpretry
