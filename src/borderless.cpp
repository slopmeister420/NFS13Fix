// Borderless fullscreen for NFS Most Wanted 2012.
//
// Stock fullscreen is DXGI exclusive mode: graphics init calls SetFullscreenState(TRUE)
// (NFS13 0x89BAFA) and the WM_ACTIVATEAPP handler (0x412521) drops out of it with
// ResizeTarget + SetFullscreenState(FALSE) on every focus loss, then goes back in - the
// mode switch is what makes alt-tab so violent. The game's own window is already
// caption-less (WS_SYSMENU|WS_MINIMIZEBOX) and it never moves or sizes it, so borderless is
// just: never go exclusive, and place the window ourselves - the whole monitor, or the
// game's resolution centred on it.
//
// Only DXGI vtables and the render window are touched - no game-code addresses. The
// vtables come from a throwaway factory and swapchain made on our own thread, because the
// EA wrapper resolves the game's imports itself and there is no import table to patch.

#include "borderless.h"
#include "log.h"
#include "renderscale.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <libmem/libmem.h>

#include <algorithm>


namespace borderless {
namespace {

// ------------------------------------------------------- vtable plumbing

// another overlay (Steam's) re-hooks a slot when it sees it changed, and then each hook
// holds the other as its "original" - forwarding blindly recurses until the stack runs
// out. on re-entry go straight to dxgi's own function instead.
thread_local int g_depth = 0;
struct Reentry {
    Reentry() { ++g_depth; }
    ~Reentry() { --g_depth; }
};

// the slot's value before anyone patched it: the dll's on-disk pointer, rebased
uintptr_t PristineSlot(const void* slot)
{
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(slot), &mod))
        return 0;
    const auto base = reinterpret_cast<uintptr_t>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const uintptr_t rva = reinterpret_cast<uintptr_t>(slot) - base;

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    DWORD fileOff = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (rva >= sec->VirtualAddress && rva < sec->VirtualAddress + sec->SizeOfRawData)
            fileOff = static_cast<DWORD>(rva - sec->VirtualAddress + sec->PointerToRawData);
    if (!fileOff)
        return 0;

    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(mod, path, MAX_PATH);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    // the loader rewrites ImageBase in the mapped header, so the preferred base comes
    // from the file too
    uintptr_t value = 0, preferred = 0;
    DWORD got = 0, got2 = 0;
    SetFilePointer(f, static_cast<LONG>(fileOff), nullptr, FILE_BEGIN);
    ReadFile(f, &value, sizeof(value), &got, nullptr);
    SetFilePointer(f,
                   dos->e_lfanew +
                       static_cast<LONG>(offsetof(IMAGE_NT_HEADERS, OptionalHeader.ImageBase)),
                   nullptr, FILE_BEGIN);
    ReadFile(f, &preferred, sizeof(preferred), &got2, nullptr);
    CloseHandle(f);
    if (got != sizeof(value) || got2 != sizeof(preferred))
        return 0;

    value = value - preferred + base;
    return value >= base && value < base + nt->OptionalHeader.SizeOfImage ? value : 0;
}

struct Slot {
    lm_vmt_t* vmt;
    size_t index;
    uintptr_t real;
};

template <typename Fn> Fn Original(const Slot& s)
{
    // re-entered: the saved original is another overlay that is calling us back
    return reinterpret_cast<Fn>(g_depth > 1 ? s.real
                                            : static_cast<uintptr_t>(
                                                  LM_VmtGetOriginal(s.vmt, s.index)));
}

bool HookSlot(lm_vmt_t* vmt, lm_address_t* vtable, size_t index, void* fn, Slot& out)
{
    out = {vmt, index, PristineSlot(vtable + index)};
    if (!out.real) {
        LOGF("[dxgi] slot %zu: could not read dxgi's own function, not hooking", index);
        return false;
    }
    if (!LM_VmtHook(vmt, index, reinterpret_cast<lm_address_t>(fn))) {
        LOGF("[dxgi] slot %zu: LM_VmtHook failed", index);
        return false;
    }
    return true;
}

// ------------------------------------------------------------- window

HWND g_hwnd = nullptr;
RECT g_target{};
bool g_haveTarget = false;
bool g_applying = false;
WNDPROC g_origWndProc = nullptr;
// the game's resolution, and the backbuffer size, which the game never changes after creating
// the swapchain: it draws a new resolution into the top-left corner of the same buffers
volatile LONG g_modeW = 0, g_modeH = 0;
LONG g_bufW = 0, g_bufH = 0;
LONG g_rgnW = -1, g_rgnH = -1;

bool Centred(const RECT& mon)
{
    const LONG w = g_modeW, h = g_modeH;
    return w > 0 && h > 0 && w <= g_bufW && h <= g_bufH && w <= mon.right - mon.left &&
           h <= mon.bottom - mon.top && (w < g_bufW || h < g_bufH);
}

// a smaller resolution: the buffer-sized window placed so the drawn corner sits in the middle
// of the monitor, clipped to that corner so the desktop shows around it
RECT Target(const RECT& mon)
{
    if (!Centred(mon))
        return mon;
    const LONG x = mon.left + (mon.right - mon.left - g_modeW) / 2;
    const LONG y = mon.top + (mon.bottom - mon.top - g_modeH) / 2;
    return {x, y, x + g_bufW, y + g_bufH};
}

// on the window's own thread: setting a region from elsewhere can wait on it
void UpdateRegion(HWND h)
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
    const bool centred = Centred(mi.rcMonitor);
    const LONG w = centred ? g_modeW : 0, hh = centred ? g_modeH : 0;
    if (w == g_rgnW && hh == g_rgnH)
        return;
    g_rgnW = w;
    g_rgnH = hh;
    SetWindowRgn(h, centred ? CreateRectRgn(0, 0, w, hh) : nullptr, TRUE);
}

// WS_MINIMIZEBOX stays: without it the taskbar button cannot minimise the game
constexpr LONG kBorderStyle =
    WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME;
constexpr LONG kBorderExStyle =
    WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE | WS_EX_TOPMOST;

// anything else that moves or sizes the window passes through here, whatever API did it
LRESULT CALLBACK SubclassProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (h == g_hwnd && msg == WM_WINDOWPOSCHANGING && g_haveTarget && !g_applying) {
        auto* wp = reinterpret_cast<WINDOWPOS*>(l);
        const int tw = g_target.right - g_target.left;
        const int th = g_target.bottom - g_target.top;
        // leave minimising alone (it parks the window at -32000,-32000), so the taskbar
        // button and Win+D keep working
        const bool minimising = IsIconic(h) || (!(wp->flags & SWP_NOMOVE) && wp->x <= -32000);
        if (!minimising) {
            if (!(wp->flags & SWP_NOMOVE) && (wp->x != g_target.left || wp->y != g_target.top)) {
                wp->x = g_target.left;
                wp->y = g_target.top;
            }
            if (!(wp->flags & SWP_NOSIZE) && (wp->cx != tw || wp->cy != th)) {
                wp->cx = tw;
                wp->cy = th;
            }
        }
    }
    if (h == g_hwnd && msg == WM_WINDOWPOSCHANGED)
        UpdateRegion(h);
    return CallWindowProcW(g_origWndProc, h, msg, w, l);
}

void ApplyBorderless(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return;
    g_hwnd = hwnd;

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi))
        return;
    g_target = Target(mi.rcMonitor);
    g_haveTarget = true;

    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    const LONG wantStyle = (style & ~kBorderStyle) | WS_POPUP;
    const LONG wantEx = (ex & ~kBorderExStyle) | WS_EX_APPWINDOW;
    RECT now{};
    GetWindowRect(hwnd, &now);

    if (GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId())
        UpdateRegion(hwnd);
    if (style != wantStyle || ex != wantEx || !EqualRect(&now, &g_target)) {
        g_applying = true;
        SetWindowLongW(hwnd, GWL_STYLE, wantStyle);
        SetWindowLongW(hwnd, GWL_EXSTYLE, wantEx);
        // from another thread, post it: the window's thread may be waiting on ours
        UINT flags = SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
        if (GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId())
            flags |= SWP_ASYNCWINDOWPOS;
        SetWindowPos(hwnd, HWND_NOTOPMOST, g_target.left, g_target.top,
                     g_target.right - g_target.left, g_target.bottom - g_target.top, flags);
        g_applying = false;
        LOGF("[win] %p borderless at %ld,%ld %ldx%ld (style %08lX -> %08lX)",
             static_cast<void*>(hwnd), g_target.left, g_target.top,
             g_target.right - g_target.left, g_target.bottom - g_target.top, style,
             wantStyle);
    }

    if (!g_origWndProc) {
        g_origWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SubclassProc)));
        LOGF("[win] subclassed (original wndproc %p)", reinterpret_cast<void*>(g_origWndProc));
    }
}

} // namespace

void SetMode(UINT w, UINT h)
{
    if (!w || !h || (static_cast<LONG>(w) == g_modeW && static_cast<LONG>(h) == g_modeH))
        return;
    InterlockedExchange(&g_modeW, static_cast<LONG>(w));
    InterlockedExchange(&g_modeH, static_cast<LONG>(h));
    LOGF("[win] game resolution %ux%u", w, h);
    if (g_hwnd)
        ApplyBorderless(g_hwnd);
}

namespace {

// ---------------------------------------------------------- dxgi hooks

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*,
                                                      DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using SetFullscreenStateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
using GetFullscreenStateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL*,
                                                         IDXGIOutput**);
using ResizeTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, const DXGI_MODE_DESC*);

constexpr size_t kCreateSwapChain = 10;    // IDXGIFactory
constexpr size_t kSetFullscreenState = 10; // IDXGISwapChain
constexpr size_t kGetFullscreenState = 11;
constexpr size_t kResizeTarget = 14;

lm_vmt_t g_facVmt{}, g_scVmt{};
Slot g_createSwapChain{}, g_setFullscreen{}, g_getFullscreen{}, g_resizeTarget{};

HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(IDXGIFactory* self, IUnknown* device,
                                                DXGI_SWAP_CHAIN_DESC* desc,
                                                IDXGISwapChain** out)
{
    Reentry guard;
    // other mods grab the same vtables with a tiny throwaway swapchain - leave those be,
    // or their dummy window becomes "the game window"
    if (g_depth > 1 || !desc || desc->BufferDesc.Width * desc->BufferDesc.Height < 320 * 240)
        return Original<CreateSwapChainFn>(g_createSwapChain)(self, device, desc, out);

    LOGF("[dxgi] CreateSwapChain %ux%u windowed=%d hwnd=%p", desc->BufferDesc.Width,
         desc->BufferDesc.Height, desc->Windowed, static_cast<void*>(desc->OutputWindow));
    DXGI_SWAP_CHAIN_DESC d = *desc;
    d.Windowed = TRUE;
    d.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    // at least monitor-sized, so a resolution picked later up to the monitor's still fits
    MONITORINFO mi{sizeof(mi)};
    if (d.OutputWindow &&
        GetMonitorInfoW(MonitorFromWindow(d.OutputWindow, MONITOR_DEFAULTTOPRIMARY), &mi)) {
        d.BufferDesc.Width = std::max(d.BufferDesc.Width, static_cast<UINT>(mi.rcMonitor.right - mi.rcMonitor.left));
        d.BufferDesc.Height = std::max(d.BufferDesc.Height, static_cast<UINT>(mi.rcMonitor.bottom - mi.rcMonitor.top));
    }

    HRESULT hr = Original<CreateSwapChainFn>(g_createSwapChain)(self, device, &d, out);
    LOGF("[dxgi] CreateSwapChain -> %08lX", hr);
    if (SUCCEEDED(hr) && d.OutputWindow) {
        // stops DXGI turning Alt+Enter into an exclusive-mode switch of its own
        self->MakeWindowAssociation(d.OutputWindow, DXGI_MWA_NO_ALT_ENTER);
        g_bufW = d.BufferDesc.Width;
        g_bufH = d.BufferDesc.Height;
        LOGF("[dxgi] backbuffer %ldx%ld", g_bufW, g_bufH);
        SetMode(desc->BufferDesc.Width, desc->BufferDesc.Height);
        ApplyBorderless(d.OutputWindow);
    }
    return hr;
}

// going exclusive is exactly what borderless has to avoid
HRESULT STDMETHODCALLTYPE HookedSetFullscreenState(IDXGISwapChain* sc, BOOL fs, IDXGIOutput* o)
{
    Reentry guard;
    if (g_depth > 1)
        return Original<SetFullscreenStateFn>(g_setFullscreen)(sc, fs, o);
    LOGF("[dxgi] SetFullscreenState(%d) swallowed", fs);
    if (g_hwnd)
        ApplyBorderless(g_hwnd);
    return S_OK;
}

// the truth - windowed. the focus-loss handler then has no exclusive mode to tear down.
HRESULT STDMETHODCALLTYPE HookedGetFullscreenState(IDXGISwapChain* sc, BOOL* fs,
                                                   IDXGIOutput** o)
{
    Reentry guard;
    if (g_depth > 1)
        return Original<GetFullscreenStateFn>(g_getFullscreen)(sc, fs, o);
    if (fs)
        *fs = FALSE;
    if (o)
        *o = nullptr;
    return S_OK;
}

// would size the window to the render mode; we place it ourselves. not a mode source either:
// focus loss passes the resolution the game started with
HRESULT STDMETHODCALLTYPE HookedResizeTarget(IDXGISwapChain* sc, const DXGI_MODE_DESC* mode)
{
    Reentry guard;
    if (g_depth > 1)
        return Original<ResizeTargetFn>(g_resizeTarget)(sc, mode);
    LOGF("[dxgi] ResizeTarget(%ux%u) swallowed", mode ? mode->Width : 0,
         mode ? mode->Height : 0);
    return S_OK;
}

// every factory and swapchain dxgi hands out shares one vtable per class, so hooking a
// throwaway instance catches the game's too, whenever it makes them
bool InstallHooks()
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return false;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NFS13FixDummy";
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                               nullptr, nullptr, wc.hInstance, nullptr);

    ID3D11Device* dev = nullptr;
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, &got, nullptr);
    if (FAILED(hr)) // no usable GPU at this point - the reference rasteriser has the same vtables
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                               D3D11_SDK_VERSION, &dev, &got, nullptr);

    IDXGISwapChain* sc = nullptr;
    if (SUCCEEDED(hr) && wnd) {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 1;
        sd.BufferDesc.Width = 64;
        sd.BufferDesc.Height = 64;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = wnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = factory->CreateSwapChain(dev, &sd, &sc);
    }

    bool ok = false;
    if (sc) {
        auto* facVt = *reinterpret_cast<lm_address_t**>(factory);
        auto* scVt = *reinterpret_cast<lm_address_t**>(sc);
        ok = LM_VmtNew(facVt, &g_facVmt) && LM_VmtNew(scVt, &g_scVmt) &&
             HookSlot(&g_facVmt, facVt, kCreateSwapChain,
                      reinterpret_cast<void*>(&HookedCreateSwapChain), g_createSwapChain) &&
             HookSlot(&g_scVmt, scVt, kSetFullscreenState,
                      reinterpret_cast<void*>(&HookedSetFullscreenState), g_setFullscreen) &&
             HookSlot(&g_scVmt, scVt, kGetFullscreenState,
                      reinterpret_cast<void*>(&HookedGetFullscreenState), g_getFullscreen) &&
             HookSlot(&g_scVmt, scVt, kResizeTarget,
                      reinterpret_cast<void*>(&HookedResizeTarget), g_resizeTarget);
        LOGF("[dxgi] factory vtable %p, swapchain vtable %p: %s", static_cast<void*>(facVt),
             static_cast<void*>(scVt), ok ? "hooked" : "FAILED");
    } else {
        LOGF("[dxgi] throwaway swapchain failed (%08lX)", hr);
    }

    if (sc)
        sc->Release();
    if (dev)
        dev->Release();
    factory->Release();
    if (wnd)
        DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

DWORD WINAPI Startup(LPVOID)
{
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (InstallHooks())
            return 0;
        Sleep(1000);
    }
    LOGF("[init] gave up - the game will run in its stock window mode");
    return 0;
}

} // namespace

void Init()
{
    renderscale::Watch([](uint32_t w, uint32_t h) { SetMode(w, h); });
    // never do D3D work under the loader lock
    HANDLE t = CreateThread(nullptr, 0, Startup, nullptr, 0, nullptr);
    if (t)
        CloseHandle(t);
}

} // namespace borderless
