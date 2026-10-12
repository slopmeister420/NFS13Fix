// Three checks stop a one-player SpeedList: PlaylistManager's CanStartPlaylist wants more than one
// active player, once running the SoloPlayerCancelPlaylist state-machine hook cancels it ("not
// enough players"), and at each event's meetup the host waits for a second player before starting.
// All three are waived while the game's visibility is friends-only or invite-only.

#include "speedlist.h"
#include "log.h"

#include <libmem/libmem.h>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace speedlist {
namespace {

using Ranges = std::vector<std::pair<const uint8_t*, const uint8_t*>>;
using CanStartFn = char(__cdecl*)(uint8_t*, uint8_t*);
using CancelFn = uint64_t(__cdecl*)();
using RematchFn = bool(__thiscall*)(void*);

constexpr int kFriendsOnly = 3, kInviteOnly = 2; // world visibility; 4 is public

uintptr_t g_world = 0;      // global holding the game world
uint32_t g_visibility = 0;  // its visibility field
uintptr_t g_hookResult = 0; // the value state-machine hooks return (two dwords)
uint32_t g_rematchObj = 0;  // world + this = the matchmaking state
RematchFn g_rematch = nullptr;
CanStartFn g_canStart = nullptr;
CancelFn g_cancel = nullptr;
bool(__cdecl* g_meetupAllowed)(int) = nullptr; // for the meetup stubs

// a `cmp reg,1; jle wait` player check: more players go on, alone in a private game goes to `alone`
struct Gate {
    uintptr_t go, alone, wait;
};
Gate g_arrival, g_countdown, g_ready;

uint32_t U32(const uint8_t* p) { return *reinterpret_cast<const uint32_t*>(p); }

// committed readable pages of the game module
Ranges ReadableRanges()
{
    Ranges out;
    auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uint8_t* const end = base + nt->OptionalHeader.SizeOfImage;
    MEMORY_BASIC_INFORMATION mbi{};
    for (const uint8_t* p = base; p < end && VirtualQuery(p, &mbi, sizeof(mbi));
         p = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            continue;
        const auto* lo = static_cast<const uint8_t*>(mbi.BaseAddress);
        out.emplace_back(lo, (std::min)(end, lo + mbi.RegionSize));
    }
    return out;
}

// the function registered under `name`: a state-machine hook (push fn; push name) or a binding
// callback (push fn; push 0; push name)
const uint8_t* Registered(const Ranges& ranges, const char* name, bool binding)
{
    const size_t len = strlen(name) + 1;
    uintptr_t str = 0;
    for (const auto& [lo, hi] : ranges)
        for (const uint8_t* p = lo + 1; p + len <= hi && !str; ++p)
            if (p[-1] == 0 && memcmp(p, name, len) == 0)
                str = reinterpret_cast<uintptr_t>(p);
    if (!str)
        return nullptr;
    for (const auto& [lo, hi] : ranges)
        for (const uint8_t* p = lo + 7; p + 5 <= hi; ++p) {
            if (p[0] != 0x68 || U32(p + 1) != str)
                continue;
            if (!binding && p[-5] == 0x68)
                return reinterpret_cast<const uint8_t*>(U32(p - 4));
            if (binding && p[-2] == 0x6A && p[-1] == 0x00 && p[-7] == 0x68)
                return reinterpret_cast<const uint8_t*>(U32(p - 6));
        }
    return nullptr;
}

// hex bytes separated by spaces, ?? = any
bool Matches(const uint8_t* p, const char* pattern)
{
    for (const char* s = pattern; s[0] && s[1]; ++p) {
        if (s[0] != '?' && *p != static_cast<uint8_t>(strtoul(s, nullptr, 16)))
            return false;
        s += s[2] == ' ' ? 3 : 2;
    }
    return true;
}

// first match of `pattern` (must start with a fixed byte) in the module
uint8_t* Find(const Ranges& ranges, const char* pattern)
{
    const auto first = static_cast<uint8_t>(strtoul(pattern, nullptr, 16));
    const size_t len = (strlen(pattern) + 1) / 3;
    for (const auto& [lo, hi] : ranges)
        for (const uint8_t* p = lo; p + len <= hi; ++p)
            if (*p == first && Matches(p, pattern))
                return const_cast<uint8_t*>(p);
    return nullptr;
}

bool PrivateGame()
{
    const auto* world = *reinterpret_cast<uint8_t* const*>(g_world);
    if (!world)
        return false;
    const int vis = *reinterpret_cast<const int*>(world + g_visibility);
    static int logged = -1;
    if (vis != logged) {
        logged = vis;
        LOGF("[speedlist] game visibility %d (%s)", vis,
             vis == kFriendsOnly ? "friends only" : vis == kInviteOnly ? "invite only" : vis == 4 ? "public" : "offline");
    }
    return vis == kFriendsOnly || vis == kInviteOnly;
}

// stock: more than one player and no rematchmaking. in a private game the player count is waived
char __cdecl CanStart(uint8_t* manager, uint8_t* out)
{
    const char r = g_canStart(manager, out);
    if (out && !*out && PrivateGame()) {
        auto* world = *reinterpret_cast<uint8_t**>(manager + 0x13B0);
        if (world && !g_rematch(world + g_rematchObj))
            *out = 1;
    }
    return r;
}

// the hook cancels with "not enough players"; in a private game it only reports success
uint64_t __cdecl SoloCancel()
{
    if (PrivateGame())
        return *reinterpret_cast<const uint64_t*>(g_hookResult);
    return g_cancel();
}

bool __cdecl MeetupAllowed(int humans)
{
    const bool ok = PrivateGame();
    static int logged = -2;
    if (logged != (ok ? humans : -humans - 1)) {
        logged = ok ? humans : -humans - 1;
        LOGF("[speedlist] meetup check: %d human player(s), %s", humans, ok ? "starting alone" : "waiting (not private)");
    }
    return ok;
}

// someone arrived at the meetup: start the countdown (esi = human players)
__declspec(naked) void ArrivalGate()
{
    __asm {
        cmp esi, 1
        jg more
        pushad
        push esi
        call dword ptr [g_meetupAllowed]
        add esp, 4
        test al, al
        popad // keeps the flags
        jz keep_waiting
        jmp dword ptr [g_arrival.alone]
    more:
        jmp dword ptr [g_arrival.go]
    keep_waiting:
        jmp dword ptr [g_arrival.wait]
    }
}

// countdown over (eax = players): tell everyone the event starts
__declspec(naked) void CountdownGate()
{
    __asm {
        cmp eax, 1
        jg more
        pushad
        push eax
        call dword ptr [g_meetupAllowed]
        add esp, 4
        test al, al
        popad
        jz keep_waiting
        jmp dword ptr [g_countdown.alone]
    more:
        jmp dword ptr [g_countdown.go]
    keep_waiting:
        jmp dword ptr [g_countdown.wait]
    }
}

// start announced (eax = players): wait until everyone is ready, then start
__declspec(naked) void ReadyGate()
{
    __asm {
        cmp eax, 1
        jg more
        pushad
        push eax
        call dword ptr [g_meetupAllowed]
        add esp, 4
        test al, al
        popad
        jz keep_waiting
        jmp dword ptr [g_ready.alone]
    more:
        jmp dword ptr [g_ready.go]
    keep_waiting:
        jmp dword ptr [g_ready.wait]
    }
}

// replaces `cmp reg,1; jle rel32` (9 bytes) with a jump to `stub`
bool PatchGate(uint8_t* at, void (*stub)(), Gate& gate, const uint8_t* alone)
{
    gate.go = reinterpret_cast<uintptr_t>(at + 9);
    gate.wait = gate.go + static_cast<int32_t>(U32(at + 5));
    gate.alone = reinterpret_cast<uintptr_t>(alone);
    uint8_t jump[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub) - reinterpret_cast<uintptr_t>(at + 5));
    memcpy(jump + 1, &rel, 4);
    lm_prot_t old;
    if (!LM_ProtMemory(reinterpret_cast<lm_address_t>(at), sizeof(jump), LM_PROT_XRW, &old))
        return false;
    memcpy(at, jump, sizeof(jump));
    LM_ProtMemory(reinterpret_cast<lm_address_t>(at), sizeof(jump), old, nullptr);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(jump));
    return true;
}

bool TryInstall()
{
    const Ranges ranges = ReadableRanges();
    const uint8_t* vis = Registered(ranges, "SetGameVisibilityPublic", false);
    const uint8_t* cancel = Registered(ranges, "SoloPlayerCancelPlaylist", false);
    const uint8_t* start = Registered(ranges, "CanStartPlaylist", true);
    // host, someone has arrived, `cmp esi,1; jle wait`, then the countdown starts
    uint8_t* arrival = Find(ranges, "84 C0 0F 84 ?? ?? ?? ?? 83 7C 24 ?? 00 0F 8E ?? ?? ?? ?? 83 FE 01 0F 8E ?? ?? ?? ?? "
                                    "8B 43 18 8B 48 28 8B 51 04");
    // countdown over: player count, `cmp eax,1; jle`, then broadcast the start
    uint8_t* countdown = Find(ranges, "84 C0 0F 84 ?? ?? ?? ?? 8B 56 18 8B 42 28 8B 48 04 8B 89 ?? ?? ?? ?? "
                                      "E8 ?? ?? ?? ?? 83 F8 01 0F 8E ?? ?? ?? ?? 33 FF");
    // start broadcast: player count, `cmp eax,1; jle`, then count the players who are ready
    uint8_t* ready = Find(ranges, "8B 89 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 F8 01 0F 8E ?? ?? ?? ?? 33 FF 8D 86 BC 00 00 00");
    if (!vis || !cancel || !start || !arrival || !countdown || !ready)
        return false;
    arrival += 19;
    countdown += 28;
    ready += 11;
    // mov eax,[world]; mov edx,[result+4]; mov dword [eax+vis],4; mov eax,[result]; ret
    // mov eax,[world]; mov ecx,[eax+X]; mov ecx,[ecx+0x14]; push 1; call cancel; mov eax,[result]
    // push esi; mov esi,[esp+8]; mov eax,[esi+0x13B0]; ...; cmp eax,1; jle; ...; call rematchmaking
    if (!Matches(vis, "A1 ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? C7 80 ?? ?? ?? ?? 04 00 00 00 A1") ||
        !Matches(cancel, "A1 ?? ?? ?? ?? 8B 88 ?? ?? ?? ?? 8B 49 14 6A 01 E8 ?? ?? ?? ?? A1") ||
        !Matches(start, "56 8B 74 24 08 8B 86 B0 13 00 00 8B 88 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 F8 01 7E ?? "
                        "8B 8E B0 13 00 00 81 C1 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0")) {
        LOGF("[speedlist] unexpected code at %p / %p / %p - solo SpeedLists off", vis, cancel, start);
        return true; // found but not ours to patch: stop looking
    }
    // alone, the countdown skips its two-slot check and goes straight to the broadcast
    // (mov ecx,[clock]; call), and the ready check straight to the start (push exit point; call)
    const uint8_t* broadcast = countdown + 0x36;
    const uint8_t* go = ready + 9 + static_cast<int32_t>(U32(ready + 5)) - 0x1D;
    if (!Matches(broadcast, "8B 0D ?? ?? ?? ?? E8") || !Matches(go, "68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 52 50")) {
        LOGF("[speedlist] unexpected meetup code at %p / %p - solo SpeedLists off", broadcast, go);
        return true;
    }
    g_world = U32(vis + 1);
    g_visibility = U32(vis + 13);
    g_hookResult = U32(vis + 22);
    if (U32(vis + 7) != g_hookResult + 4 || U32(cancel + 1) != g_world) {
        LOGF("[speedlist] hook result or world global disagree - solo SpeedLists off");
        return true;
    }
    g_rematchObj = U32(start + 0x23);
    g_rematch = reinterpret_cast<RematchFn>(start + 0x2C + static_cast<int32_t>(U32(start + 0x28)));

    lm_address_t trampoline = 0;
    if (!LM_HookCode(reinterpret_cast<lm_address_t>(start), reinterpret_cast<lm_address_t>(&CanStart), &trampoline)) {
        LOGF("[speedlist] could not hook CanStartPlaylist");
        return true;
    }
    g_canStart = reinterpret_cast<CanStartFn>(trampoline);
    if (!LM_HookCode(reinterpret_cast<lm_address_t>(cancel), reinterpret_cast<lm_address_t>(&SoloCancel), &trampoline)) {
        LOGF("[speedlist] could not hook SoloPlayerCancelPlaylist");
        return true;
    }
    g_cancel = reinterpret_cast<CancelFn>(trampoline);

    g_meetupAllowed = &MeetupAllowed;
    if (!PatchGate(arrival, &ArrivalGate, g_arrival, arrival + 9) ||
        !PatchGate(countdown, &CountdownGate, g_countdown, broadcast) ||
        !PatchGate(ready, &ReadyGate, g_ready, go)) {
        LOGF("[speedlist] could not patch the meetup checks");
        return true;
    }
    LOGF("[speedlist] solo SpeedLists in friends-only and invite-only games "
         "(start check %p, solo cancel %p, meetup %p %p %p)",
         start, cancel, arrival, countdown, ready);
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
    LOGF("[speedlist] SpeedList checks not found - solo SpeedLists off");
    return 0;
}

} // namespace

void Init()
{
    if (HANDLE t = CreateThread(nullptr, 0, Waiter, nullptr, 0, nullptr))
        CloseHandle(t);
}

} // namespace speedlist
