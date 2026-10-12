// SDL3 in place of the game's XInput. MW2012 imports XInputGetState / SetState /
// GetCapabilities from xinput1_3 by ordinal; those three slots now read from pads that SDL
// opens on its own thread, so every controller SDL supports looks like an Xbox pad to the
// game. The same pads are hidden from the game's DirectInput enumeration so they cannot
// register twice, and keyboard and pad input are muted while the game is not focused.

#include "input.h"
#include "iat.h"
#include "log.h"

#include <windows.h>
#include <Xinput.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <SDL3/SDL.h>
#include <libmem/libmem.h>

#include <cstring>

namespace input {
namespace {

constexpr DWORD kPads = XUSER_MAX_COUNT;

struct Pad {
    SDL_Gamepad* gamepad = nullptr; // sdl thread only
    SDL_JoystickID id = 0;          // sdl thread only
    bool connected = false;
    XINPUT_GAMEPAD state{};
    DWORD packet = 0;
    Uint16 vendor = 0, product = 0;
    WORD rumbleLow = 0, rumbleHigh = 0; // what the game asked for
};

Pad g_pads[kPads];
SRWLOCK g_lock = SRWLOCK_INIT;
HANDLE g_enumerated = nullptr; // set once SDL has reported the pads present at startup
volatile bool g_sdlReady = false;

bool GameHasFocus()
{
    DWORD pid = 0;
    if (HWND fg = GetForegroundWindow())
        GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// ------------------------------------------------------------ sdl thread

SHORT FlipY(Sint16 v) { return v == SDL_JOYSTICK_AXIS_MIN ? 32767 : static_cast<SHORT>(-v); }
BYTE Trigger(Sint16 v) { return v <= 0 ? 0 : static_cast<BYTE>(v * 255 / 32767); }

XINPUT_GAMEPAD Read(SDL_Gamepad* g)
{
    // sdl names buttons by position, which is the xbox layout on every pad
    static const struct {
        SDL_GamepadButton sdl;
        WORD xinput;
    } kButtons[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
        {SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START},
        {SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A},
        {SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B},
        {SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X},
        {SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y},
    };
    XINPUT_GAMEPAD x{};
    for (const auto& b : kButtons)
        if (SDL_GetGamepadButton(g, b.sdl))
            x.wButtons |= b.xinput;
    x.bLeftTrigger = Trigger(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    x.bRightTrigger = Trigger(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    // sdl's y axes point down, xinput's up
    x.sThumbLX = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
    x.sThumbLY = FlipY(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
    x.sThumbRX = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX);
    x.sThumbRY = FlipY(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
    return x;
}

SDL_JoystickID g_failed = 0; // last id that would not attach, so a retry does not log again

void Attach(SDL_JoystickID id)
{
    DWORD slot = kPads;
    for (DWORD i = 0; i < kPads; ++i) {
        if (g_pads[i].gamepad && g_pads[i].id == id)
            return;
        if (!g_pads[i].gamepad && slot == kPads)
            slot = i;
    }
    if (slot == kPads) {
        if (g_failed != id)
            LOGF("[pad] %s ignored - all %lu slots taken", SDL_GetGamepadNameForID(id), kPads);
        g_failed = id;
        return;
    }
    SDL_Gamepad* g = SDL_OpenGamepad(id);
    if (!g) {
        if (g_failed != id)
            LOGF("[pad] could not open %s: %s", SDL_GetGamepadNameForID(id), SDL_GetError());
        g_failed = id;
        return;
    }
    Pad& p = g_pads[slot];
    p.gamepad = g;
    p.id = id;
    AcquireSRWLockExclusive(&g_lock);
    p.connected = true;
    p.state = {};
    p.vendor = SDL_GetGamepadVendor(g);
    p.product = SDL_GetGamepadProduct(g);
    p.rumbleLow = p.rumbleHigh = 0;
    ReleaseSRWLockExclusive(&g_lock);
    LOGF("[pad] %s (%04X:%04X) -> player %lu", SDL_GetGamepadName(g), p.vendor, p.product,
         slot + 1);
}

void Detach(SDL_JoystickID id)
{
    for (DWORD i = 0; i < kPads; ++i) {
        Pad& p = g_pads[i];
        if (!p.gamepad || p.id != id)
            continue;
        LOGF("[pad] player %lu disconnected", i + 1);
        SDL_CloseGamepad(p.gamepad);
        p.gamepad = nullptr;
        AcquireSRWLockExclusive(&g_lock);
        p.connected = false;
        ReleaseSRWLockExclusive(&g_lock);
    }
}

DWORD WINAPI SdlThread(LPVOID)
{
    // there is no sdl window to have focus; focus is checked against the game's instead
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // steam puts every controller it supports in these variables for games it launches,
    // steam input on or off, which would hide the physical pads from us. we own the pads
    // instead, and skip steam's virtual one so a pad cannot show up twice.
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, "", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "0",
                            SDL_HINT_OVERRIDE);
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGF("[sdl] init failed (%s) - the game keeps its own XInput", SDL_GetError());
        SetEvent(g_enumerated);
        return 0;
    }
    LOGF("[sdl] SDL %d.%d.%d up", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
    g_sdlReady = true;

    WORD sentLow[kPads]{}, sentHigh[kPads]{};
    ULONGLONG sentAt[kPads]{};
    ULONGLONG syncedAt = 0;
    bool first = true;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_GAMEPAD_ADDED)
                Attach(e.gdevice.which);
            else if (e.type == SDL_EVENT_GAMEPAD_REMOVED)
                Detach(e.gdevice.which);
        }
        // pads present at startup arrive as ADDED events on the first poll
        if (first) {
            first = false;
            SetEvent(g_enumerated);
        }

        const bool focus = GameHasFocus();
        const ULONGLONG now = GetTickCount64();
        // a pad whose open failed, or whose event was missed, gets picked up here
        if (now - syncedAt > 1000) {
            syncedAt = now;
            int n = 0;
            if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
                for (int i = 0; i < n; ++i)
                    Attach(ids[i]);
                SDL_free(ids);
            }
        }
        for (DWORD i = 0; i < kPads; ++i) {
            Pad& p = g_pads[i];
            if (!p.gamepad)
                continue;
            const XINPUT_GAMEPAD s = focus ? Read(p.gamepad) : XINPUT_GAMEPAD{};
            AcquireSRWLockExclusive(&g_lock);
            if (memcmp(&s, &p.state, sizeof(s)) != 0) {
                p.state = s;
                ++p.packet;
            }
            const WORD low = focus ? p.rumbleLow : 0;
            const WORD high = focus ? p.rumbleHigh : 0;
            ReleaseSRWLockExclusive(&g_lock);

            // xinput rumble lasts until changed, sdl's for a duration: renew it in time
            if (low != sentLow[i] || high != sentHigh[i] ||
                ((low || high) && now - sentAt[i] > 30000)) {
                SDL_RumbleGamepad(p.gamepad, low, high, (low || high) ? 60000 : 0);
                sentLow[i] = low;
                sentHigh[i] = high;
                sentAt[i] = now;
            }
        }
        SDL_Delay(2);
    }
}

// ---------------------------------------------------------------- xinput

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using GetCapabilitiesFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);

iat::Hook g_getState, g_setState, g_getCaps;

DWORD WINAPI HookedGetState(DWORD user, XINPUT_STATE* out)
{
    if (!g_sdlReady)
        return reinterpret_cast<GetStateFn>(g_getState.original)(user, out);
    if (user >= kPads || !out)
        return ERROR_BAD_ARGUMENTS;
    AcquireSRWLockShared(&g_lock);
    const Pad& p = g_pads[user];
    const bool connected = p.connected;
    if (connected) {
        out->dwPacketNumber = p.packet;
        out->Gamepad = p.state;
    }
    ReleaseSRWLockShared(&g_lock);
    return connected ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI HookedSetState(DWORD user, XINPUT_VIBRATION* vib)
{
    if (!g_sdlReady)
        return reinterpret_cast<SetStateFn>(g_setState.original)(user, vib);
    if (user >= kPads || !vib)
        return ERROR_BAD_ARGUMENTS;
    AcquireSRWLockExclusive(&g_lock);
    Pad& p = g_pads[user];
    const bool connected = p.connected;
    if (connected) {
        p.rumbleLow = vib->wLeftMotorSpeed;
        p.rumbleHigh = vib->wRightMotorSpeed;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return connected ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI HookedGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* out)
{
    if (!g_sdlReady)
        return reinterpret_cast<GetCapabilitiesFn>(g_getCaps.original)(user, flags, out);
    if (user >= kPads || !out)
        return ERROR_BAD_ARGUMENTS;
    AcquireSRWLockShared(&g_lock);
    const bool connected = g_pads[user].connected;
    ReleaseSRWLockShared(&g_lock);
    if (!connected)
        return ERROR_DEVICE_NOT_CONNECTED;
    *out = {};
    out->Type = XINPUT_DEVTYPE_GAMEPAD;
    out->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    out->Gamepad.wButtons = 0xF3FF;
    out->Gamepad.bLeftTrigger = out->Gamepad.bRightTrigger = 0xFF;
    out->Gamepad.sThumbLX = out->Gamepad.sThumbLY = static_cast<SHORT>(0xFFC0);
    out->Gamepad.sThumbRX = out->Gamepad.sThumbRY = static_cast<SHORT>(0xFFC0);
    out->Vibration.wLeftMotorSpeed = out->Vibration.wRightMotorSpeed = 0xFFFF;
    return ERROR_SUCCESS;
}

// -------------------------------------------------------------- keyboard

using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
iat::Hook g_keys;
bool g_keysFocus = true;

// the game keeps polling while alt-tabbed and would drive with whatever is typed elsewhere
SHORT WINAPI HookedGetAsyncKeyState(int vk)
{
    // the per-frame poll starts at VK_BACK: check focus once per sweep
    if (vk == VK_BACK)
        g_keysFocus = GameHasFocus();
    // always call through, so a mod chained behind us still sees every poll
    const SHORT r = reinterpret_cast<GetAsyncKeyStateFn>(g_keys.original)(vk);
    return g_keysFocus ? r : 0;
}

// ----------------------------------------------------------- directinput

using EnumDevicesWFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8W*, DWORD,
                                                   LPDIENUMDEVICESCALLBACKW, LPVOID, DWORD);
using EnumDevicesAFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8A*, DWORD,
                                                   LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD);
constexpr size_t kEnumDevices = 4;

lm_vmt_t g_diW{}, g_diA{};

// a pad sdl has opened would otherwise also reach the game as a raw directinput device
bool OwnedBySdl(DWORD devType, const GUID& product)
{
    if (GET_DIDEVICE_TYPE(devType) < DI8DEVTYPE_JOYSTICK)
        return false;
    const Uint16 vendor = LOWORD(product.Data1), prod = HIWORD(product.Data1);
    bool owned = false;
    AcquireSRWLockShared(&g_lock);
    for (const Pad& p : g_pads)
        owned |= p.connected && p.vendor == vendor && p.product == prod;
    ReleaseSRWLockShared(&g_lock);
    return owned;
}

template <typename Callback> struct EnumContext {
    Callback callback;
    LPVOID ref;
};

template <typename Instance, typename Callback>
BOOL CALLBACK FilterDevice(const Instance* d, LPVOID ctx)
{
    auto* c = static_cast<EnumContext<Callback>*>(ctx);
    if (OwnedBySdl(d->dwDevType, d->guidProduct))
        return DIENUM_CONTINUE;
    return c->callback(d, c->ref);
}

HRESULT STDMETHODCALLTYPE HookedEnumDevicesW(IDirectInput8W* self, DWORD type,
                                             LPDIENUMDEVICESCALLBACKW cb, LPVOID ref, DWORD flags)
{
    auto orig = reinterpret_cast<EnumDevicesWFn>(LM_VmtGetOriginal(&g_diW, kEnumDevices));
    if (!cb || !g_sdlReady)
        return orig(self, type, cb, ref, flags);
    WaitForSingleObject(g_enumerated, 3000);
    EnumContext<LPDIENUMDEVICESCALLBACKW> c{cb, ref};
    return orig(self, type, FilterDevice<DIDEVICEINSTANCEW, LPDIENUMDEVICESCALLBACKW>, &c,
                flags);
}

HRESULT STDMETHODCALLTYPE HookedEnumDevicesA(IDirectInput8A* self, DWORD type,
                                             LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD flags)
{
    auto orig = reinterpret_cast<EnumDevicesAFn>(LM_VmtGetOriginal(&g_diA, kEnumDevices));
    if (!cb || !g_sdlReady)
        return orig(self, type, cb, ref, flags);
    WaitForSingleObject(g_enumerated, 3000);
    EnumContext<LPDIENUMDEVICESCALLBACKA> c{cb, ref};
    return orig(self, type, FilterDevice<DIDEVICEINSTANCEA, LPDIENUMDEVICESCALLBACKA>, &c,
                flags);
}

void HookDirectInput()
{
    // by full path: "dinput8.dll" in the game folder is the asi loader
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\dinput8.dll");
    HMODULE di = LoadLibraryW(path);
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto create = di ? reinterpret_cast<CreateFn>(GetProcAddress(di, "DirectInput8Create")) : nullptr;
    if (!create) {
        LOGF("[dinput] system dinput8 not found - pads may register twice");
        return;
    }
    // objects stay alive: releasing the last one may unload dinput8 and its vtables
    IDirectInput8W* w = nullptr;
    IDirectInput8A* a = nullptr;
    const HINSTANCE self = GetModuleHandleW(nullptr);
    bool ok = SUCCEEDED(create(self, DIRECTINPUT_VERSION, IID_IDirectInput8W,
                               reinterpret_cast<void**>(&w), nullptr)) &&
              SUCCEEDED(create(self, DIRECTINPUT_VERSION, IID_IDirectInput8A,
                               reinterpret_cast<void**>(&a), nullptr));
    ok = ok && LM_VmtNew(*reinterpret_cast<lm_address_t**>(w), &g_diW) &&
         LM_VmtNew(*reinterpret_cast<lm_address_t**>(a), &g_diA) &&
         LM_VmtHook(&g_diW, kEnumDevices, reinterpret_cast<lm_address_t>(&HookedEnumDevicesW)) &&
         LM_VmtHook(&g_diA, kEnumDevices, reinterpret_cast<lm_address_t>(&HookedEnumDevicesA));
    LOGF("[dinput] device enumeration %s", ok ? "filtered" : "NOT filtered - pads may register twice");
}

// ------------------------------------------------------------------ setup

DWORD WINAPI Startup(LPVOID)
{
    HookDirectInput();

    // the ea wrapper fills these slots in after we load, before the game first uses them
    const struct {
        const char* dll;
        const char* name;
        uint16_t ordinal;
        void* hook;
        iat::Hook* out;
    } kImports[] = {
        {"xinput1_3.dll", nullptr, 2, reinterpret_cast<void*>(&HookedGetState), &g_getState},
        {"xinput1_3.dll", nullptr, 3, reinterpret_cast<void*>(&HookedSetState), &g_setState},
        {"xinput1_3.dll", nullptr, 4, reinterpret_cast<void*>(&HookedGetCapabilities), &g_getCaps},
        {"USER32.dll", "GetAsyncKeyState", 0, reinterpret_cast<void*>(&HookedGetAsyncKeyState),
         &g_keys},
    };
    for (const auto& imp : kImports) {
        const bool ok = iat::HookImport(imp.dll, imp.name, imp.ordinal,
                                        reinterpret_cast<uintptr_t>(imp.hook), 60000, *imp.out);
        if (imp.name)
            LOGF("[iat] %s!%s %s", imp.dll, imp.name, ok ? "hooked" : "NOT FOUND");
        else
            LOGF("[iat] %s!#%u %s", imp.dll, imp.ordinal, ok ? "hooked" : "NOT FOUND");
    }
    return 0;
}

} // namespace

void Init()
{
    g_enumerated = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // sdl gets a thread of its own: it must not borrow the game's, and the pads are read
    // and handed over through g_pads
    const LPTHREAD_START_ROUTINE threads[] = {SdlThread, Startup};
    for (LPTHREAD_START_ROUTINE fn : threads)
        if (HANDLE t = CreateThread(nullptr, 0, fn, nullptr, 0, nullptr))
            CloseHandle(t);
}

} // namespace input
