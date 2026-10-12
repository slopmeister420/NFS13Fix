#pragma once
#include <cstddef>
#include <cstdint>

// import-slot hooks for the game module. a pointer-sized data write cannot tear
// an instruction stream, which is why hooks go in this way: an inline detour on
// a live win32 export crashed HP Remastered on boot.
namespace iat {

// rewrites every slot in the game module holding oldValue, returning the number
// changed.
size_t Replace(uintptr_t oldValue, uintptr_t newValue);

struct Pending {
    const char* name; // import name, e.g. "CreateFileW"
    uintptr_t real;   // what the loader will write into the slot
    uintptr_t hook;
    size_t patched = 0;
};

// for games whose real imports are resolved by a DRM stub after we load (MW2012's
// EA wrapper): finds the slots still holding the RVA of each import's hint/name
// entry, waits for the stub to fill them in and swaps in the hook straight away.
// returns once every import is patched or timeoutMs runs out.
void ReplaceWhenResolved(Pending* imports, size_t count, unsigned timeoutMs);

struct Hook {
    uintptr_t slot = 0;
    uintptr_t original = 0; // whatever the slot held: the real function or another mod's hook
};

// finds the game's import slot for dll!name (or dll!#ordinal when name is null) through its
// import descriptor, waits up to timeoutMs for it to be resolved, then swaps in hook. chains
// with any other mod that hooked the slot first, so load order does not matter.
bool HookImport(const char* dll, const char* name, uint16_t ordinal, uintptr_t hook,
                unsigned timeoutMs, Hook& out);

// puts the original back, unless someone chained on top of us since
void Unhook(Hook& h, uintptr_t hook);

} // namespace iat
