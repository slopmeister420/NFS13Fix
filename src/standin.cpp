// A vehicle id with no bundle never finishes loading: the local player is left frozen without a
// car, and in an online game everyone else keeps waiting on it. An id the vehicle list does not
// know (another player's add-on car) is read past the list and crashes. Such an id is swapped for a
// stand-in car where a racer's car changes, where its vehicle settings are stored, and where a race
// car is loaded, which is also where another player's car change arrives from the network. The
// player's own car loads its _HI bundle and every other racer's its _LO one, so that is the file
// required.

#include "standin.h"
#include "log.h"

#include <libmem/libmem.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace standin {
namespace {

// SetRacerVehicle(racer index, vehicle settings, mode), thiscall: settings are 0xac bytes, id at +0x30
const char kSetSig[] = "8B 44 24 04 69 C0 D8 00 00 00 53 55 8B E9 56 8B 75 04 8D 9C 28 7C 82 00 00 57 8B BD A0 8F 00 00";
constexpr size_t kSettingsSize = 0xac, kIdOffset = 0x30;
// further in: mov ecx,[this+4] (world); ...; mov ecx,[ecx+manager]; call GetLocalRacer;
// mov edx,[esp+14]; cmp edx,[eax+index] - which racer is the player
const char kLocalSig[] = "8B 4D 04 89 85 ?? ?? ?? ?? 8B 89 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 54 24 14 3B 90 ?? ?? ?? ??";
constexpr size_t kLocalAt = 0x9b;

// ChangeVehicle(vehicle id, livery, settings), thiscall on a racer: also hands the id to the
// vehicle manager before the setter, so a bad id has to be replaced here as well
const char kChangeSig[] = "55 8B EC 83 E4 F8 81 EC B4 00 00 00 53 8B D9 8B 83 A0 00 00 00 56 57 83 F8 FF";
// mov ecx,[racer+world]
const char kChangeWorldSig[] = "8B 8B ?? ?? ?? ?? 50 81 C1";
constexpr size_t kChangeWorldAt = 0x2a;

// RaceCarEntityModule::LoadRaceCar(slot, vehicle id, livery id, flag), thiscall
const char kLoadSig[] = "83 EC 10 53 55 56 8B 74 24 20 8B EE 69 ED 40 1C 00 00 8B D9 03 EB 80 BD 6A 30 00 00 00";
constexpr size_t kCarStride = 0x1c40;
// lea esi,[ebx+vehicle list]; push edx; mov ecx,esi; call FindVehicleIndexById
const char kLoadFindSig[] = "8D B3 ?? ?? ?? ?? 52 8B CE E8";
constexpr size_t kLoadFindAt = 0x48;
// mov [ebp+field],eax / mov [ebp+field],edx: the vehicle and livery ids a car was loaded with
const char kLoadIdSig[] = "89 85 ?? ?? 00 00";
const char kLoadLiverySig[] = "89 95 ?? ?? 00 00";
constexpr size_t kLoadIdAt = 0x93, kLoadLiveryAt = 0x152;
// push esi; mov esi,ecx; mov ecx,[game logic]; call GetLocalVehicleIndex; cmp eax,-1; je; mov ecx,eax; imul ecx,stride
const char kLocalSlotSig[] = "56 8B F1 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 F8 FF 0F 84 ?? ?? ?? ?? 8B C8 69 C9 40 1C 00 00";
// which reads mov eax,[ecx+world]; mov ecx,[eax+manager] with no null checks
const char kLocalSlotFnSig[] = "8B 41 ?? 8B 88 ?? ?? ?? ??";

using SetFn = void(__fastcall*)(void* self, void* edx, int racer, const uint8_t* settings, int mode);
using ChangeFn = void(__fastcall*)(void* racer, void* edx, uint32_t id, uint32_t livery, const uint8_t* settings);
using LocalFn = uint8_t*(__fastcall*)(void* manager, void* edx);
using LoadFn = void(__fastcall*)(uint8_t* self, void* edx, int slot, uint32_t id, uint32_t livery, int flag);
using FindFn = int(__fastcall*)(void* list, void* edx, uint32_t id);
using SlotFn = int(__fastcall*)(void* gameLogic, void* edx);
SetFn g_set = nullptr;
ChangeFn g_change = nullptr;
LocalFn g_localRacer = nullptr;
LoadFn g_load = nullptr;
FindFn g_findIndex = nullptr;
SlotFn g_localSlot = nullptr;
uintptr_t g_gameLogic = 0;
uint32_t g_offList = 0, g_offCarId = 0, g_offCarLivery = 0, g_offLogicWorld = 0, g_offLogicManager = 0;
uint32_t g_offManager = 0, g_offIndex = 0, g_offRacerWorld = 0;
uint32_t g_standIn = 0;
std::wstring g_dir; // game folder, with a trailing backslash
std::mutex g_mtx;
std::unordered_map<uint64_t, bool> g_known;

uint32_t U32(uintptr_t p) { return *reinterpret_cast<const uint32_t*>(p); }

bool Matches(uintptr_t at, const char* sig)
{
    return LM_SigScan(sig, at, 64) == at;
}

// a file in the game's folder or the mods folder the mod loader serves
bool Exists(const wchar_t* name)
{
    return GetFileAttributesW((g_dir + name).c_str()) != INVALID_FILE_ATTRIBUTES ||
           GetFileAttributesW((g_dir + L"mods\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool HasBundle(uint32_t id, bool local)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    const uint64_t key = static_cast<uint64_t>(id) << 1 | (local ? 1u : 0u);
    auto it = g_known.find(key);
    if (it != g_known.end())
        return it->second;
    wchar_t name[64];
    swprintf_s(name, L"VEHICLES\\VEH_%u_%s.BNDL", id, local ? L"HI" : L"LO");
    const bool found = Exists(name);
    g_known.emplace(key, found);
    return found;
}

// the player's racer; null before the world has one
uint8_t* LocalRacer(uintptr_t world)
{
    const uintptr_t manager = world ? U32(world + g_offManager) : 0;
    return manager ? g_localRacer(reinterpret_cast<void*>(manager), nullptr) : nullptr;
}

bool Missing(uint32_t id, bool local)
{
    return id && id != g_standIn && !HasBundle(id, local);
}

void __fastcall SetRacerVehicle(void* self, void*, int racer, const uint8_t* settings, int mode)
{
    uint32_t id = 0;
    if (settings)
        memcpy(&id, settings + kIdOffset, sizeof(id));
    // unknown counts as the player: their own cars only need _HI
    const uint8_t* player = LocalRacer(U32(reinterpret_cast<uintptr_t>(self) + 4));
    const bool local = !player || static_cast<int>(U32(reinterpret_cast<uintptr_t>(player) + g_offIndex)) == racer;
    if (Missing(id, local)) {
        uint8_t copy[kSettingsSize];
        memcpy(copy, settings, sizeof(copy));
        memcpy(copy + kIdOffset, &g_standIn, sizeof(g_standIn));
        LOGF("[standin] racer %d: vehicle %u has no %s bundle, using %u", racer, id, local ? "_HI" : "_LO", g_standIn);
        return g_set(self, nullptr, racer, copy, mode);
    }
    g_set(self, nullptr, racer, settings, mode);
}

void __fastcall ChangeVehicle(void* racer, void*, uint32_t id, uint32_t livery, const uint8_t* settings)
{
    const uint8_t* player = LocalRacer(U32(reinterpret_cast<uintptr_t>(racer) + g_offRacerWorld));
    const bool local = !player || player == racer;
    if (Missing(id, local)) {
        LOGF("[standin] change to vehicle %u, which has no %s bundle: using %u", id, local ? "_HI" : "_LO", g_standIn);
        id = g_standIn;
    }
    g_change(racer, nullptr, id, livery, settings);
}

void __fastcall LoadRaceCar(uint8_t* self, void*, int slot, uint32_t id, uint32_t livery, int flag)
{
    const bool listed = g_findIndex(self + g_offList, nullptr, id) >= 0;
    // unknown counts as the player, as above
    const uint32_t gameLogic = U32(g_gameLogic);
    const uint32_t world = gameLogic ? U32(gameLogic + g_offLogicWorld) : 0;
    const bool local = !world || !U32(world + g_offLogicManager) ||
                       g_localSlot(reinterpret_cast<void*>(gameLogic), nullptr) == slot;
    if (id && id != g_standIn && (!listed || !HasBundle(id, local))) {
        LOGF("[standin] race car %d: vehicle %u is %s, using %u", slot, id,
             listed ? (local ? "missing its _HI bundle" : "missing its _LO bundle") : "not in the vehicle list", g_standIn);
        g_load(self, nullptr, slot, g_standIn, 0, flag);
        // keep the ids that were asked for, or every network update for this car reloads it
        uint8_t* car = self + slot * kCarStride;
        memcpy(car + g_offCarId, &id, sizeof(id));
        memcpy(car + g_offCarLivery, &livery, sizeof(livery));
        return;
    }
    g_load(self, nullptr, slot, id, livery, flag);
}

// the race car loader hook; false leaves it off
bool InstallLoad(const lm_module_t& mod)
{
    const lm_address_t load = LM_SigScan(kLoadSig, mod.base, mod.size);
    const lm_address_t local = LM_SigScan(kLocalSlotSig, mod.base, mod.size);
    if (load == LM_ADDRESS_BAD || local == LM_ADDRESS_BAD || !Matches(load + kLoadFindAt, kLoadFindSig) ||
        !Matches(load + kLoadIdAt, kLoadIdSig) || !Matches(load + kLoadLiveryAt, kLoadLiverySig))
        return false;
    g_offList = U32(load + kLoadFindAt + 2);
    g_findIndex = reinterpret_cast<FindFn>(load + kLoadFindAt + 14 + static_cast<int32_t>(U32(load + kLoadFindAt + 10)));
    g_offCarId = U32(load + kLoadIdAt + 2);
    g_offCarLivery = U32(load + kLoadLiveryAt + 2);
    g_gameLogic = U32(local + 5);
    const lm_address_t slotFn = local + 14 + static_cast<int32_t>(U32(local + 10));
    if (!Matches(slotFn, kLocalSlotFnSig))
        return false;
    g_localSlot = reinterpret_cast<SlotFn>(slotFn);
    g_offLogicWorld = *reinterpret_cast<const uint8_t*>(slotFn + 2);
    g_offLogicManager = U32(slotFn + 5);
    lm_address_t tramp = 0;
    if (!LM_HookCode(load, reinterpret_cast<lm_address_t>(&LoadRaceCar), &tramp))
        return false;
    g_load = reinterpret_cast<LoadFn>(tramp);
    return true;
}

bool TryInstall()
{
    lm_module_t mod;
    if (!LM_FindModule("NFS13.exe", &mod))
        return false;
    const lm_address_t set = LM_SigScan(kSetSig, mod.base, mod.size);
    const lm_address_t change = LM_SigScan(kChangeSig, mod.base, mod.size);
    if (set == LM_ADDRESS_BAD || change == LM_ADDRESS_BAD)
        return false;
    if (!Matches(set + kLocalAt, kLocalSig) || !Matches(change + kChangeWorldAt, kChangeWorldSig)) {
        LOGF("[standin] unexpected code in the racer vehicle functions - stand-in cars off");
        return true;
    }
    g_offManager = U32(set + kLocalAt + 11);
    g_localRacer = reinterpret_cast<LocalFn>(set + kLocalAt + 20 + static_cast<int32_t>(U32(set + kLocalAt + 16)));
    g_offIndex = U32(set + kLocalAt + 26);
    g_offRacerWorld = U32(change + kChangeWorldAt + 2);

    lm_address_t a = 0, b = 0;
    if (!LM_HookCode(set, reinterpret_cast<lm_address_t>(&SetRacerVehicle), &a) ||
        !LM_HookCode(change, reinterpret_cast<lm_address_t>(&ChangeVehicle), &b)) {
        LOGF("[standin] could not hook the racer vehicle functions");
        return true;
    }
    g_set = reinterpret_cast<SetFn>(a);
    g_change = reinterpret_cast<ChangeFn>(b);
    if (!InstallLoad(mod))
        LOGF("[standin] race car loader not found - other players' unknown cars are not replaced");
    LOGF("[standin] cars without a bundle are replaced by %u (setter %p, change %p)", g_standIn,
         reinterpret_cast<void*>(set), reinterpret_cast<void*>(change));
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
    LOGF("[standin] racer vehicle setter not found - stand-in cars off");
    return 0;
}

} // namespace

void Init(uint32_t standInId)
{
    g_standIn = standInId;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_dir = exe;
    g_dir.resize(g_dir.find_last_of(L'\\') + 1);
    if (HANDLE t = CreateThread(nullptr, 0, Waiter, nullptr, 0, nullptr))
        CloseHandle(t);
}

} // namespace standin
