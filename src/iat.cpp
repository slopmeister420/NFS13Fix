#include "iat.h"

#include <windows.h>
#include <libmem/libmem.h>

#include <cstring>
#include <utility>
#include <vector>

namespace iat {
namespace {

using Ranges = std::vector<std::pair<uintptr_t, uintptr_t>>;

struct Module {
    uintptr_t base = 0, end = 0;
    Ranges readable;
};

lm_bool_t LM_CALL CollectSegment(lm_segment_t* seg, lm_void_t* arg)
{
    auto* m = static_cast<Module*>(arg);
    const uintptr_t b = static_cast<uintptr_t>(seg->base);
    const uintptr_t e = static_cast<uintptr_t>(seg->end);
    if (e > m->base && b < m->end && (seg->prot & LM_PROT_R))
        m->readable.emplace_back(b < m->base ? m->base : b, e > m->end ? m->end : e);
    return LM_TRUE;
}

// enumerate segments rather than walking the module range: HP Remastered's image
// is ~460 MB and not fully committed, so a blind sweep would fault the game.
bool GameModule(Module& m)
{
    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* name = strrchr(exe, '\\');

    lm_module_t mod{};
    if (!LM_FindModule(name ? name + 1 : exe, &mod))
        return false;
    m.base = static_cast<uintptr_t>(mod.base);
    m.end = static_cast<uintptr_t>(mod.end);
    LM_EnumSegments(CollectSegment, &m);
    return !m.readable.empty();
}

bool WritePointer(uintptr_t at, uintptr_t value)
{
    lm_prot_t old{};
    if (!LM_ProtMemory(at, sizeof(uintptr_t), LM_PROT_RW, &old))
        return false;
    *reinterpret_cast<uintptr_t*>(at) = value;
    LM_ProtMemory(at, sizeof(uintptr_t), old, nullptr);
    return true;
}

// import slots are pointer-aligned: 8 on x64, 4 on x86
template <typename F> void ForEachSlot(const Ranges& rs, F&& f)
{
    constexpr uintptr_t a = sizeof(uintptr_t);
    for (const auto& r : rs)
        for (uintptr_t p = (r.first + a - 1) & ~(a - 1); p + a <= r.second; p += a)
            f(p);
}

} // namespace

size_t Replace(uintptr_t oldValue, uintptr_t newValue)
{
    if (!oldValue || oldValue == newValue)
        return 0;

    Module m;
    if (!GameModule(m))
        return 0;

    size_t patched = 0;
    ForEachSlot(m.readable, [&](uintptr_t p) {
        if (*reinterpret_cast<uintptr_t*>(p) == oldValue && WritePointer(p, newValue))
            ++patched;
    });
    return patched;
}

void ReplaceWhenResolved(Pending* imports, size_t count, unsigned timeoutMs)
{
    Module m;
    if (!GameModule(m))
        return;

    // an unresolved thunk holds the RVA of an IMAGE_IMPORT_BY_NAME, i.e. of the
    // 2-byte hint just before the name
    std::vector<std::vector<uintptr_t>> watch(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t len = strlen(imports[i].name) + 1;
        std::vector<uintptr_t> rvas;
        for (const auto& r : m.readable)
            for (uintptr_t p = r.first + 2; p + len <= r.second; ++p)
                if (memcmp(reinterpret_cast<const void*>(p), imports[i].name, len) == 0 &&
                    ((p - 2 - m.base) & 1) == 0)
                    rvas.push_back(p - 2 - m.base);

        // the stub keeps resolving while this scans, so a slot may already hold
        // the real address by the time we reach it
        ForEachSlot(m.readable, [&](uintptr_t p) {
            const uintptr_t v = *reinterpret_cast<uintptr_t*>(p);
            if (v == imports[i].real) {
                if (WritePointer(p, imports[i].hook))
                    ++imports[i].patched;
                return;
            }
            for (uintptr_t rva : rvas)
                if (v == rva)
                    watch[i].push_back(p);
        });
    }

    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        bool done = true;
        for (size_t i = 0; i < count; ++i) {
            for (uintptr_t& slot : watch[i]) {
                if (!slot)
                    continue;
                // resolved = now points into another loaded module. the stub may
                // pick the kernelbase export over kernel32's, so don't insist on
                // `real`; a heap pointer means this was never a slot.
                const uintptr_t v = *reinterpret_cast<uintptr_t*>(slot);
                if (v && (v < m.base || v >= m.end)) {
                    HMODULE owner = nullptr;
                    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                            reinterpret_cast<LPCWSTR>(v), &owner)) {
                        slot = 0;
                        continue;
                    }
                    if (WritePointer(slot, imports[i].hook))
                        ++imports[i].patched;
                    slot = 0;
                }
            }
            if (!imports[i].patched && !watch[i].empty())
                done = false;
        }
        if (done || GetTickCount64() > deadline)
            return;
        // the window between the stub writing a slot and the game's first call
        // through it is short, so spin rather than sleep a whole tick
        SwitchToThread();
    }
}

bool HookImport(const char* dll, const char* name, uint16_t ordinal, uintptr_t hook,
                unsigned timeoutMs, Hook& out)
{
    Module m;
    if (!GameModule(m))
        return false;
    const auto readable = [&](uintptr_t p, size_t n) {
        for (const auto& r : m.readable)
            if (p >= r.first && p + n <= r.second)
                return true;
        return false;
    };

    // descriptors name their dll by rva; find every copy of the name first
    const size_t dllLen = strlen(dll) + 1;
    std::vector<uint32_t> nameRvas;
    for (const auto& r : m.readable)
        for (uintptr_t p = r.first; p + dllLen <= r.second; ++p)
            if (_strnicmp(reinterpret_cast<const char*>(p), dll, dllLen) == 0)
                nameRvas.push_back(static_cast<uint32_t>(p - m.base));

    const uintptr_t size = m.end - m.base;
    uintptr_t slot = 0;
    for (const auto& r : m.readable) {
        for (uintptr_t p = (r.first + 3) & ~uintptr_t(3); p + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= r.second && !slot; p += 4) {
            const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(p);
            bool named = false;
            for (uint32_t rva : nameRvas)
                named |= d->Name == rva;
            if (!named || !d->OriginalFirstThunk || !d->FirstThunk ||
                d->OriginalFirstThunk >= size || d->FirstThunk >= size)
                continue;
            // the lookup table keeps the names after the slots are overwritten
            const auto* ilt = reinterpret_cast<const uintptr_t*>(m.base + d->OriginalFirstThunk);
            for (size_t i = 0; i < 4096 && readable(reinterpret_cast<uintptr_t>(ilt + i), sizeof(uintptr_t)) && ilt[i]; ++i) {
                const uintptr_t e = ilt[i];
                bool match;
                if (e & IMAGE_ORDINAL_FLAG)
                    match = !name && IMAGE_ORDINAL(e) == ordinal;
                else
                    match = name && e + 2 < size && readable(m.base + e + 2, strlen(name) + 1) &&
                            strcmp(reinterpret_cast<const char*>(m.base + e + 2), name) == 0;
                if (match) {
                    slot = m.base + d->FirstThunk + i * sizeof(uintptr_t);
                    break;
                }
            }
        }
        if (slot)
            break;
    }
    if (!slot)
        return false;

    // resolved = points into a loaded module other than the game (an unresolved slot
    // still holds an rva or an ordinal)
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        const uintptr_t v = *reinterpret_cast<volatile uintptr_t*>(slot);
        HMODULE owner = nullptr;
        if (v && (v < m.base || v >= m.end) &&
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(v), &owner))
            break;
        if (GetTickCount64() > deadline)
            return false;
        SwitchToThread();
    }

    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(uintptr_t), PAGE_READWRITE, &old))
        return false;
    out.slot = slot;
    out.original = reinterpret_cast<uintptr_t>(InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(hook)));
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(uintptr_t), old, &old);
    return true;
}

void Unhook(Hook& h, uintptr_t hook)
{
    if (h.slot && *reinterpret_cast<uintptr_t*>(h.slot) == hook)
        WritePointer(h.slot, h.original);
    h = {};
}

} // namespace iat
