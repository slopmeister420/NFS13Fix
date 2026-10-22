// The game runs its simulation in fixed ticks of 1/rate seconds and renders once per pass,
// running extra ticks when a frame took longer. The rate comes from a function that returns a
// hardcoded 60, and the frame limiter and both sim timers are derived from it at startup. We
// make it return a higher rate before that happens, and scale the cap on catch-up ticks per
// frame (3 at 60) with it, so slow frames still keep real time. Traffic only updated at the
// stock step sizes, so its 10 Hz timer is rewritten to work at any rate, and the engine sound
// synth froze its pitch when updated this often, so its glide is fixed too. Volume emitters
// (rain) spawned per frame, so they now spawn per second.

#include "framerate.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace framerate {
namespace {

int g_rate = 60;

struct Pattern {
    const char* text; // hex bytes, ?? = any
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;
};

Pattern Parse(const char* text)
{
    Pattern p{text};
    for (const char* s = text; *s;) {
        if (*s == ' ') {
            ++s;
            continue;
        }
        const bool any = s[0] == '?';
        p.bytes.push_back(any ? 0 : static_cast<uint8_t>(strtoul(std::string(s, 2).c_str(), nullptr, 16)));
        p.mask.push_back(!any);
        s += 2;
    }
    return p;
}

// committed executable pages of the game module
std::vector<std::pair<uint8_t*, uint8_t*>> CodeRanges()
{
    std::vector<std::pair<uint8_t*, uint8_t*>> out;
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    uint8_t* const end = base + nt->OptionalHeader.SizeOfImage;
    MEMORY_BASIC_INFORMATION mbi{};
    for (uint8_t* p = base; p < end && VirtualQuery(p, &mbi, sizeof(mbi));
         p = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
        const DWORD exec = PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & exec) && !(mbi.Protect & PAGE_GUARD))
            out.emplace_back(static_cast<uint8_t*>(mbi.BaseAddress),
                             std::min(end, static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize));
    }
    return out;
}

// every match; the longest run of fixed bytes is searched for, the rest compared after
std::vector<uint8_t*> FindAll(const std::vector<std::pair<uint8_t*, uint8_t*>>& ranges, const Pattern& p)
{
    size_t a = 0, b = 0;
    for (size_t i = 0; i < p.mask.size();) {
        size_t j = i;
        while (j < p.mask.size() && p.mask[j])
            ++j;
        if (j - i > b - a)
            a = i, b = j;
        i = j + 1;
    }
    const std::boyer_moore_horspool_searcher anchor(p.bytes.begin() + a, p.bytes.begin() + b);
    std::vector<uint8_t*> hits;
    for (const auto& [lo, hi] : ranges)
        for (uint8_t* it = lo; (it = std::search(it, hi, anchor)) != hi; ++it) {
            uint8_t* start = it - a;
            if (start < lo || start + p.bytes.size() > hi)
                continue;
            bool ok = true;
            for (size_t i = 0; i < p.bytes.size() && ok; ++i)
                ok = !p.mask[i] || start[i] == p.bytes[i];
            if (ok)
                hits.push_back(start);
        }
    return hits;
}

void Write(void* at, const void* data, size_t size)
{
    DWORD old;
    VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, data, size);
    VirtualProtect(at, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, size);
}

// sim ticks since boot, counted by a hook in the tick loop
volatile uint32_t g_ticks = 0;
alignas(16) const float kNegBig[4] = {-1e30f, -1e30f, -1e30f, -1e30f};

// x86 code built straight into an executable cave
struct Asm {
    uint8_t* base;
    size_t n = 0;
    void Bytes(std::initializer_list<uint8_t> b)
    {
        for (uint8_t x : b)
            base[n++] = x;
    }
    void Dword(uint32_t v)
    {
        memcpy(base + n, &v, 4);
        n += 4;
    }
    void Rel(const void* to) { Dword(static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(to) - (base + n + 4))); }
    void Jmp(const void* to) { Bytes({0xE9}), Rel(to); }
    uint8_t* Here() { return base + n; }
    // CF set when this tick lines up with a 60 Hz tick: (ticks * 60) % rate < 60. uses eax, edx
    void Cadence()
    {
        Bytes({0xA1}), Dword(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_ticks)));
        Bytes({0xBA, 60, 0, 0, 0, 0xF7, 0xE2});
        Bytes({0xF7, 0x35}), Dword(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_rate)));
        Bytes({0x83, 0xFA, 60});
    }
};

void JumpTo(uint8_t* site, size_t len, const void* to)
{
    uint8_t j[16];
    memset(j, 0x90, len);
    j[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(static_cast<const uint8_t*>(to) - (site + 5));
    memcpy(j + 1, &rel, 4);
    Write(site, j, len);
}

// backfires and exhaust flames are tuned per 60 Hz tick: the pop roll needs the rpm drop since
// the previous tick, and the flame envelope counts ticks. run both on a 60 Hz cadence instead.
void PatchExhaust(const std::vector<std::pair<uint8_t*, uint8_t*>>& ranges)
{
    // tick loop: lea ecx,[esi+0x80]; call SystemModule tick; cmp [esi+0xa39fc],0xa
    static const Pattern kTick = Parse("8D 8E 80 00 00 00 E8 ?? ?? ?? ?? 83 BE FC 39 0A 00 0A 75 10");
    // pop gate: drop = prev rpm [esi+0x1eb0] - rpm [esi+0x1f00]
    static const Pattern kGate = Parse("0F 28 8E 00 1F 00 00 0F 28 86 B0 1E 00 00 8B 86 D8 1B 00 00");
    // end of the pop update: prev rpm = rpm
    static const Pattern kStore = Parse(
        "0F 28 86 B0 1E 00 00 0F 28 C8 0F C6 8C 24 C0 00 00 00 05 5F 0F C6 C8 E2 0F 29 8E B0 1E 00 00 5E");
    // flame envelope: ticks since the last pop
    static const Pattern kFlame = Parse("FF 86 7C 1E 00 00 8B 96 D8 1E 00 00");

    const auto tick = FindAll(ranges, kTick), gate = FindAll(ranges, kGate),
               store = FindAll(ranges, kStore), flame = FindAll(ranges, kFlame);
    if (tick.size() != 1 || gate.size() != 1 || store.size() != 1 || flame.size() != 1) {
        LOGF("[fps] exhaust pops not found (%zu/%zu/%zu/%zu) - backfires stay tick-based",
             tick.size(), gate.size(), store.size(), flame.size());
        return;
    }
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!mem)
        return;
    Asm a{mem};

    // count ticks, then on to the original callee
    uint8_t* const call = tick[0] + 6;
    uint8_t* const callee = call + 5 + *reinterpret_cast<int32_t*>(call + 1);
    uint8_t* const counter = a.Here();
    a.Bytes({0xFF, 0x05}), a.Dword(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_ticks)));
    a.Jmp(callee);

    // off-cadence: prev rpm reads as -inf, so the drop is 0 and nothing rolls
    uint8_t* const gateSite = gate[0] + 7;
    uint8_t* const gateCave = a.Here();
    a.Cadence();
    a.Bytes({0x72, 12});                               // jb roll
    a.Bytes({0x0F, 0x28, 0x05}), a.Dword(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(kNegBig)));
    a.Jmp(gateSite + 7);
    a.Bytes({0x0F, 0x28, 0x86, 0xB0, 0x1E, 0x00, 0x00}); // roll: movaps xmm0,[esi+0x1eb0]
    a.Jmp(gateSite + 7);

    // off-cadence: keep the prev rpm from the last 60 Hz tick
    uint8_t* const storeSite = store[0];
    uint8_t* const storeCave = a.Here();
    a.Cadence();
    a.Bytes({0x72, 6});                                // jb keep going
    a.Bytes({0x5F});                                   // pop edi
    a.Jmp(storeSite + 0x1F);                           // pop esi; pop ebx; leave; ret
    a.Bytes({0x0F, 0x28, 0x86, 0xB0, 0x1E, 0x00, 0x00});
    a.Jmp(storeSite + 7);

    // flame envelope advances only on 60 Hz ticks
    uint8_t* const flameSite = flame[0];
    uint8_t* const flameCave = a.Here();
    a.Cadence();
    a.Bytes({0x73, 6});                                // jae skip
    a.Bytes({0xFF, 0x86, 0x7C, 0x1E, 0x00, 0x00});     // inc [esi+0x1e7c]
    a.Jmp(flameSite + 6);
    FlushInstructionCache(GetCurrentProcess(), mem, a.n);

    uint8_t rel[4];
    const int32_t r = static_cast<int32_t>(counter - (call + 5));
    memcpy(rel, &r, 4);
    Write(call + 1, rel, 4);
    JumpTo(gateSite, 7, gateCave);
    JumpTo(storeSite, 7, storeCave);
    JumpTo(flameSite, 6, flameCave);
    LOGF("[fps] exhaust pops and flames on a 60 Hz cadence");
}

const float kSixty = 60.0f, kMaxScale = 3.0f, kOne = 1.0f;
uint32_t g_seed = 0x2545F491;
uint32_t g_scratch;

// volume emitters (rain, ambient particles) spawn a fixed count per rendered frame, so they
// spawn more per second above 60 fps. scale the count by dt * 60, dithered so the fraction
// isn't lost to the truncation.
void PatchVolumeEmitter(const std::vector<std::pair<uint8_t*, uint8_t*>>& ranges)
{
    // count = binding * mfNumParticles: fld [eax+edx*4]; mulps; fmul [esi+0x88]
    static const Pattern kSpawn = Parse(
        "0F B6 96 D9 00 00 00 0F 28 57 30 0F 58 C4 D9 04 90 0F 59 47 20 D8 8E 88 00 00 00 0F 58 C2");
    // earlier in the same function: movss xmm1,[ecx+0x64] (ecx = sim interface, +0x64 = frame dt)
    static const uint8_t kDt[] = {0xF3, 0x0F, 0x10, 0x49, 0x64};
    const auto spawn = FindAll(ranges, kSpawn);
    if (spawn.size() != 1 || memcmp(spawn[0] + 0x15 - 0x257, kDt, sizeof(kDt)) != 0) {
        LOGF("[fps] volume emitter not found (%zu) - its particles stay per frame", spawn.size());
        return;
    }
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!mem)
        return;
    auto addr = [](const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); };
    uint8_t* const site = spawn[0] + 0x15;
    Asm a{mem};
    a.Bytes({0xD8, 0x8E, 0x88, 0x00, 0x00, 0x00});          // fmul [esi+0x88]
    a.Bytes({0xD9, 0x41, 0x64});                            // fld [ecx+0x64]
    a.Bytes({0xD8, 0x0D}), a.Dword(addr(&kSixty));          // fmul [60]
    a.Bytes({0xD9, 0x05}), a.Dword(addr(&kMaxScale));       // fld [3]
    a.Bytes({0xDB, 0xF1});                                  // fcomi st,st(1)
    a.Bytes({0xDB, 0xC1});                                  // fcmovnb st,st(1): min(3, dt*60)
    a.Bytes({0xDD, 0xD9});                                  // fstp st(1)
    a.Bytes({0xDE, 0xC9});                                  // fmulp
    a.Bytes({0x50});                                        // push eax
    a.Bytes({0xA1}), a.Dword(addr(&g_seed));                // lcg step
    a.Bytes({0x69, 0xC0}), a.Dword(1664525);
    a.Bytes({0x05}), a.Dword(1013904223);
    a.Bytes({0xA3}), a.Dword(addr(&g_seed));
    a.Bytes({0xC1, 0xE8, 0x09, 0x0D}), a.Dword(0x3F800000); // 1 + r in [1, 2)
    a.Bytes({0xA3}), a.Dword(addr(&g_scratch));
    a.Bytes({0x58});                                        // pop eax
    a.Bytes({0xD8, 0x05}), a.Dword(addr(&g_scratch));       // fadd
    a.Bytes({0xD8, 0x25}), a.Dword(addr(&kOne));            // fsub [1]
    a.Jmp(site + 6);
    FlushInstructionCache(GetCurrentProcess(), mem, a.n);
    JumpTo(site, 6, mem);
    LOGF("[fps] volume emitters spawn per second, not per frame");
}

// false while the code is still encrypted; true once patched or found unpatchable
bool TryPatch()
{
    // SystemModule setup: call GetVSyncRate; 1.0 / rate -> both sim timers; rate -> [esi+0x10]
    static const Pattern kSetup = Parse(
        "E8 ?? ?? ?? ?? F3 0F 10 05 ?? ?? ?? ?? 0F 57 C9 F3 0F 2A C8 F3 0F 5E C1 51 0F 28 C8 "
        "F3 0F 59 0D ?? ?? ?? ?? 8D 4E 18 89 46 10");
    // frame rate manager setup: max ticks per frame = 3
    static const Pattern kMaxTicks = Parse("C7 86 10 3B 62 00 03 00 00 00");
    // traffic's 10 Hz update: counter += 12/10/6/5 only when the step is exactly 1/25..1/60
    static const Pattern kTraffic = Parse(
        "C6 86 E0 2B 0A 00 00 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? F3 0F 10 05 ?? ?? ?? ?? 0F 2E 40 10");
    static const Pattern kTrafficWrap = Parse("8A 86 DA 2B 0A 00 3C 1E 72 0F 2C 1E");
    // audio command buffer: push 0x20000; push eax; mov edi,ecx; call System init
    static const Pattern kAudioCmds = Parse("8B 44 24 08 53 56 57 68 00 00 02 00 50 8B F9 E8");
    // ginsu engine synth: a new rpm target restarts its pitch glide from a stale position
    static const Pattern kGinsu = Parse(
        "F3 0F 10 4F 30 0F 2E 8F D4 01 00 00 F3 0F 59 05 ?? ?? ?? ?? 9F F3 0F 59 C2 F6 C4 44 7B 1C "
        "D9 87 D8 01 00 00 F3 0F 11 87 CC 01 00 00 D9 9F D0 01 00 00 F3 0F 11 8F D4 01 00 00 "
        "F3 0F 10 97 CC 01 00 00 F3 0F 10 3D");

    const auto ranges = CodeRanges();
    const auto setup = FindAll(ranges, kSetup);
    if (setup.empty())
        return false;
    if (setup.size() != 1) {
        LOGF("[fps] %zu matches for the sim rate setup - frame rate left at 60", setup.size());
        return true;
    }
    uint8_t* rateFn = setup[0] + 5 + *reinterpret_cast<int32_t*>(setup[0] + 1);
    static const uint8_t kStock[] = {0xB8, 60, 0, 0, 0, 0xC3}; // mov eax, 60; ret
    const auto maxTicks = FindAll(ranges, kMaxTicks);
    const auto traffic = FindAll(ranges, kTraffic);
    uint8_t* const wrap = traffic.size() == 1 ? traffic[0] + 0x9D : nullptr;
    const bool wrapOk = wrap && [&] {
        for (size_t i = 0; i < kTrafficWrap.bytes.size(); ++i)
            if (wrap[i] != kTrafficWrap.bytes[i])
                return false;
        return true;
    }();
    const auto audioCmds = FindAll(ranges, kAudioCmds);
    const auto ginsu = FindAll(ranges, kGinsu);
    uint8_t* const cave = ginsu.size() == 1
        ? static_cast<uint8_t*>(VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
        : nullptr;
    if (memcmp(rateFn, kStock, sizeof(kStock)) != 0 || maxTicks.size() != 2 || !wrapOk ||
        audioCmds.size() != 1 || !cave) {
        LOGF("[fps] unexpected code (rate fn %08X, %zu tick caps, %zu traffic timers, %zu audio "
             "buffers, %zu engine synths) - frame rate left at 60",
             static_cast<unsigned>(reinterpret_cast<uintptr_t>(rateFn)), maxTicks.size(),
             traffic.size(), audioCmds.size(), ginsu.size());
        return true;
    }

    uint8_t rate[6] = {0xB8, 0, 0, 0, 0, 0xC3};
    memcpy(rate + 1, &g_rate, 4);
    Write(rateFn, rate, sizeof(rate));
    const int32_t cap = (3 * g_rate + 59) / 60;
    for (uint8_t* p : maxTicks)
        Write(p + 6, &cap, 4);

    // traffic: add `step` per tick and wrap at `wrapAt`, keeping step/wrapAt = 10 / rate
    int step = 1, wrapAt = (g_rate + 5) / 10;
    for (int g = 10; g > 0; --g)
        if (g_rate % g == 0 && 10 % g == 0 && 10 / g + g_rate / g < 256) {
            step = 10 / g;
            wrapAt = g_rate / g;
            break;
        }
    uint8_t add[12] = {0x80, 0x86, 0xDA, 0x2B, 0x0A, 0x00, static_cast<uint8_t>(step), 0xE9};
    const int32_t rel = static_cast<int32_t>(wrap - (traffic[0] + 7 + sizeof(add)));
    memcpy(add + 8, &rel, 4);
    Write(traffic[0] + 7, add, sizeof(add)); // add byte [esi+0xa2bda], step; jmp wrap check
    const uint8_t w = static_cast<uint8_t>(wrapAt);
    Write(wrap + 7, &w, 1);
    Write(wrap + 11, &w, 1);

    // every tick of a frame queues ~10 KB of sound commands into one unchecked buffer; stock
    // sizes it for 3 ticks, so scale it with the cap or long frames overrun it
    const int32_t cmdBytes = 0x20000 * ((cap + 2) / 3);
    Write(audioCmds[0] + 8, &cmdBytes, 4);

    // the synth glides toward each new rpm target, but a new target restarts the glide from the
    // position stored at the previous 10 ms step. targets arriving about that often (above ~60
    // per second) freeze the pitch, so grains play too long. bring the position up to date first.
    uint8_t* const site = ginsu[0] + 0x1E; // fld [edi+0x1d8]
    const uint32_t one = *reinterpret_cast<uint32_t*>(ginsu[0] + 0x46); // address of 1.0f
    static const uint8_t kBody[] = {
        0xF3, 0x0F, 0x10, 0x9F, 0xCC, 0x01, 0x00, 0x00, // movss xmm3,[edi+0x1cc]  samples left
        0x0F, 0x2F, 0x1D, 0, 0, 0, 0,                   // comiss xmm3,[1.0]
        0x72, 0x30,                                     // jb keep
        0xF3, 0x0F, 0x10, 0xE0, 0xF3, 0x0F, 0x5C, 0xE3, // xmm4 = (len - left) / len
        0xF3, 0x0F, 0x5E, 0xE0,
        0xF3, 0x0F, 0x10, 0x9F, 0xD4, 0x01, 0x00, 0x00, // xmm3 = target - start
        0xF3, 0x0F, 0x5C, 0x9F, 0xD0, 0x01, 0x00, 0x00,
        0xF3, 0x0F, 0x59, 0xE3,                         // xmm4 = start + xmm4 * xmm3
        0xF3, 0x0F, 0x58, 0xA7, 0xD0, 0x01, 0x00, 0x00,
        0xF3, 0x0F, 0x11, 0xA7, 0xD8, 0x01, 0x00, 0x00, // position = xmm4
        0xD9, 0x87, 0xD8, 0x01, 0x00, 0x00,             // keep: fld [edi+0x1d8]
        0xE9, 0, 0, 0, 0,                               // jmp back
    };
    memcpy(cave, kBody, sizeof(kBody));
    memcpy(cave + 11, &one, 4);
    const int32_t back = static_cast<int32_t>(site + 6 - (cave + sizeof(kBody)));
    memcpy(cave + sizeof(kBody) - 4, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), cave, sizeof(kBody));
    uint8_t jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
    const int32_t to = static_cast<int32_t>(cave - (site + 5));
    memcpy(jmp + 1, &to, 4);
    Write(site, jmp, sizeof(jmp));

    PatchExhaust(ranges);
    PatchVolumeEmitter(ranges);
    LOGF("[fps] simulation and frame cap at %d Hz (was 60), up to %d ticks per frame, %d KB "
         "sound command buffer",
         g_rate, cap, cmdBytes / 1024);
    return true;
}

DWORD WINAPI Waiter(LPVOID)
{
    // the ea wrapper decrypts the code after we load; the setup runs well after that
    for (int i = 0; i < 60000; ++i) {
        if (TryPatch())
            return 0;
        Sleep(1);
    }
    LOGF("[fps] game code never appeared - frame rate left at 60");
    return 0;
}

} // namespace

void Init(int rate)
{
    if (rate <= 0) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        rate = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1
                   ? static_cast<int>(dm.dmDisplayFrequency)
                   : 60;
    }
    g_rate = std::clamp(rate, 30, 500);
    if (g_rate == 60) {
        LOGF("[fps] frame rate 60 - leaving the game's timing alone");
        return;
    }
    if (TryPatch())
        return;
    if (HANDLE t = CreateThread(nullptr, 0, Waiter, nullptr, 0, nullptr))
        CloseHandle(t);
}

} // namespace framerate
