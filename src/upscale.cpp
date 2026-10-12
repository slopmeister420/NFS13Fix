#include "upscale.h"
#include "upscale_ipc.h"
#include "upscale_prep_cs.h"

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;
namespace ipc = upscale_ipc;

namespace upscale {
namespace {

constexpr float kRatio[] = {1.0f, 1.5f, 1.7241379f, 2.0f, 3.0f};

struct SharedTex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    HANDLE handle = nullptr;
};

Config g_cfg;
ComPtr<ID3D11Device> g_dev;
ComPtr<ID3D11Device5> g_dev5;
ComPtr<ID3D11ComputeShader> g_prep;
ComPtr<ID3D11Buffer> g_params;
ComPtr<ID3D11Fence> g_fence;
HANDLE g_fenceHandle = nullptr;
uint64_t g_luid = 0;

HANDLE g_mapping = nullptr, g_event = nullptr, g_process = nullptr, g_watchdog = nullptr;
ipc::Shared* g_shm = nullptr;
volatile LONG g_dead = 0;

SharedTex g_color, g_depth, g_motion, g_reactive, g_output;
uint32_t g_generation = 0;
uint32_t g_displayW = 0, g_displayH = 0, g_renderW = 0, g_renderH = 0;
DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN, g_outFormat = DXGI_FORMAT_UNKNOWN;
char g_status[256] = "not started";

struct alignas(16) PrepParams {
    float reproject[16];
    float renderSize[2];
    float jitterNdc[2];
    uint32_t hasStencil;
    uint32_t hasParticles;
    uint32_t pad[2];
    float objects[128][16];
};

DXGI_FORMAT UavFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return f;
    }
}

uint64_t Duplicate(HANDLE h)
{
    HANDLE dup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), h, g_process, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return 0;
    return reinterpret_cast<uint64_t>(dup);
}

bool MakeShared(SharedTex& t, uint32_t w, uint32_t h, DXGI_FORMAT format)
{
    if (t.handle)
        CloseHandle(t.handle);
    t = {};
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &t.tex)) ||
        FAILED(g_dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv)) ||
        FAILED(g_dev->CreateUnorderedAccessView(t.tex.Get(), nullptr, &t.uav)))
        return false;
    ComPtr<IDXGIResource1> r;
    return SUCCEEDED(t.tex.As(&r)) &&
           SUCCEEDED(r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &t.handle));
}

// the helper died: release anything still waiting on its fence, from a d3d12 device of our own
DWORD WINAPI Watchdog(LPVOID)
{
    WaitForSingleObject(g_process, INFINITE);
    InterlockedExchange(&g_dead, 1);
    snprintf(g_status, sizeof(g_status), "helper exited");
    if (!g_fenceHandle)
        return 0;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D12Fence> f12;
    LUID l;
    memcpy(&l, &g_luid, sizeof(l));
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->EnumAdapterByLuid(l, IID_PPV_ARGS(&adapter))) &&
        SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12))) &&
        SUCCEEDED(d12->OpenSharedHandle(g_fenceHandle, IID_PPV_ARGS(&f12))))
        f12->Signal(UINT64_MAX);
    return 0;
}

bool Recreate(uint32_t displayW, uint32_t displayH, DXGI_FORMAT format, DXGI_FORMAT outFormat)
{
    uint32_t rw, rh;
    RenderSize(displayW, displayH, rw, rh);
    const DXGI_FORMAT f = UavFormat(format);
    if (!MakeShared(g_color, rw, rh, f) || !MakeShared(g_depth, rw, rh, DXGI_FORMAT_R32_FLOAT) ||
        !MakeShared(g_motion, rw, rh, DXGI_FORMAT_R16G16_FLOAT) || !MakeShared(g_reactive, rw, rh, DXGI_FORMAT_R8_UNORM) ||
        !MakeShared(g_output, displayW, displayH, UavFormat(outFormat))) {
        snprintf(g_status, sizeof(g_status), "could not create shared textures (format %d)", f);
        return false;
    }
    ipc::Resources r{};
    r.generation = ++g_generation;
    r.backend = static_cast<uint32_t>(g_cfg.backend);
    r.quality = g_cfg.quality;
    r.hdr = g_cfg.hdr;
    r.renderWidth = rw;
    r.renderHeight = rh;
    r.displayWidth = displayW;
    r.displayHeight = displayH;
    r.colorFormat = f;
    r.depthInverted = g_cfg.depthInverted;
    r.color = Duplicate(g_color.handle);
    r.depth = Duplicate(g_depth.handle);
    r.motion = Duplicate(g_motion.handle);
    r.reactive = Duplicate(g_reactive.handle);
    r.output = Duplicate(g_output.handle);
    r.fence = Duplicate(g_fenceHandle);
    g_shm->resources = r;
    MemoryBarrier();
    SetEvent(g_event);
    g_displayW = displayW;
    g_displayH = displayH;
    g_renderW = rw;
    g_renderH = rh;
    g_format = format;
    g_outFormat = outFormat;
    return true;
}

double Halton(uint32_t i, uint32_t base)
{
    double f = 1.0, r = 0.0;
    for (; i; i /= base) {
        f /= base;
        r += f * (i % base);
    }
    return r;
}

} // namespace

bool Init(ID3D11Device* device, const Config& config)
{
    g_cfg = config;
    g_dev = device;
    if (FAILED(g_dev.As(&g_dev5))) {
        snprintf(g_status, sizeof(g_status), "d3d11.4 fences unavailable");
        return false;
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC ad{};
    if (FAILED(g_dev.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&ad)))
        return false;
    memcpy(&g_luid, &ad.AdapterLuid, sizeof(g_luid));

    if (FAILED(g_dev->CreateComputeShader(kUpscalePrepCS, sizeof(kUpscalePrepCS), nullptr, &g_prep)))
        return false;
    D3D11_BUFFER_DESC bd{sizeof(PrepParams), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
    if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_params)))
        return false;
    if (FAILED(g_dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_fence))) ||
        FAILED(g_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &g_fenceHandle))) {
        snprintf(g_status, sizeof(g_status), "shared fence unavailable");
        return false;
    }

    const DWORD pid = GetCurrentProcessId();
    wchar_t shmName[64], eventName[64];
    swprintf_s(shmName, L"Local\\NFS13Upscale_%lu", pid);
    swprintf_s(eventName, L"Local\\NFS13UpscaleFrame_%lu", pid);
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ipc::Shared), shmName);
    g_event = CreateEventW(nullptr, FALSE, FALSE, eventName);
    if (!g_mapping || !g_event)
        return false;
    g_shm = static_cast<ipc::Shared*>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ipc::Shared)));
    if (!g_shm)
        return false;
    memset(g_shm, 0, sizeof(*g_shm));
    g_shm->magic = ipc::kMagic;
    g_shm->version = ipc::kVersion;
    g_shm->adapterLuid = g_luid;
    g_shm->parentPid = pid;

    std::wstring exe = std::wstring(config.helperDir ? config.helperDir : L".") + L"\\NFS13Upscaler.exe";
    std::wstring cmd = L"\"" + exe + L"\" --shm " + shmName + L" --event " + eventName + (config.log ? L" --log" : L"");
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        config.helperDir, &si, &pi)) {
        snprintf(g_status, sizeof(g_status), "could not start %ls (%lu)", exe.c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    g_process = pi.hProcess;
    g_watchdog = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
    snprintf(g_status, sizeof(g_status), "helper starting");
    return true;
}

void Reconfigure(Backend backend, uint32_t quality)
{
    g_cfg.backend = backend;
    g_cfg.quality = quality;
    g_cfg.jitterSignY = 1.0f; // the helper turns y around for dlss
    g_displayW = 0; // forces Recreate
    snprintf(g_status, sizeof(g_status), "reconfiguring");
}

void Shutdown()
{
    if (g_process) {
        TerminateProcess(g_process, 0);
        WaitForSingleObject(g_watchdog, 2000);
    }
}

void RenderSize(uint32_t displayWidth, uint32_t displayHeight, uint32_t& width, uint32_t& height)
{
    RenderSize(g_cfg.quality, displayWidth, displayHeight, width, height);
}

void RenderSize(uint32_t quality, uint32_t displayWidth, uint32_t displayHeight, uint32_t& width, uint32_t& height)
{
    const float r = kRatio[quality < 5 ? quality : 1];
    width = static_cast<uint32_t>(std::ceil(displayWidth / r));
    height = static_cast<uint32_t>(std::ceil(displayHeight / r));
}

void Jitter(uint64_t frame, uint32_t renderWidth, uint32_t displayWidth, float& x, float& y)
{
    const float ratio = static_cast<float>(displayWidth) / renderWidth;
    const uint32_t phases = static_cast<uint32_t>(std::ceil(8.0f * ratio * ratio));
    const uint32_t i = static_cast<uint32_t>(frame % phases) + 1;
    x = static_cast<float>(Halton(i, 2) - 0.5);
    y = static_cast<float>(Halton(i, 3) - 0.5);
}

namespace {
bool ReadyFor(uint32_t displayWidth, uint32_t displayHeight, DXGI_FORMAT colorFormat, DXGI_FORMAT outFormat)
{
    if (!g_shm || g_dead)
        return false;
    if (!(g_shm->available & (1u << static_cast<uint32_t>(g_cfg.backend)))) {
        if (g_shm->available || g_shm->state == ipc::kFailed)
            snprintf(g_status, sizeof(g_status), "backend unavailable (available mask %u)", g_shm->available);
        return false;
    }
    if (displayWidth != g_displayW || displayHeight != g_displayH || colorFormat != g_format || outFormat != g_outFormat) {
        // swap resources only once the helper has drained every frame using the old ones
        if (g_shm->completed != g_shm->submitted)
            return false;
        if (!Recreate(displayWidth, displayHeight, colorFormat, outFormat))
            return false;
    }
    if (g_shm->readyGeneration != g_generation)
        return false;
    if (g_shm->state != ipc::kReady) {
        snprintf(g_status, sizeof(g_status), "helper: %s", g_shm->error);
        return false;
    }
    snprintf(g_status, sizeof(g_status), "running %ux%u -> %ux%u", g_renderW, g_renderH, g_displayW, g_displayH);
    return true;
}
} // namespace

bool Ready(uint32_t displayWidth, uint32_t displayHeight, DXGI_FORMAT colorFormat)
{
    return ReadyFor(displayWidth, displayHeight, colorFormat, colorFormat);
}

ID3D11ShaderResourceView* Run(ID3D11DeviceContext* ctx, const Frame& frame, uint32_t displayWidth,
                              uint32_t displayHeight, DXGI_FORMAT colorFormat, DXGI_FORMAT outputFormat)
{
    if (outputFormat == DXGI_FORMAT_UNKNOWN)
        outputFormat = colorFormat;
    if (!ReadyFor(displayWidth, displayHeight, colorFormat, outputFormat))
        return nullptr;
    ComPtr<ID3D11DeviceContext4> ctx4;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx4))))
        return nullptr;

    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return nullptr;
    auto* p = static_cast<PrepParams*>(m.pData);
    memcpy(p->reproject, frame.reproject, sizeof(p->reproject));
    p->renderSize[0] = static_cast<float>(g_renderW);
    p->renderSize[1] = static_cast<float>(g_renderH);
    p->jitterNdc[0] = 2.0f * frame.jitterX / g_renderW;
    p->jitterNdc[1] = -2.0f * frame.jitterY / g_renderH;
    p->hasStencil = frame.stencil != nullptr;
    p->hasParticles = frame.particles != nullptr;
    memcpy(p->objects, frame.objects, sizeof(p->objects));
    ctx->Unmap(g_params.Get(), 0);

    ID3D11ShaderResourceView* srvs[] = {frame.color, frame.depth, frame.stencil, frame.particles};
    ID3D11UnorderedAccessView* uavs[] = {g_color.uav.Get(), g_depth.uav.Get(), g_motion.uav.Get(), g_reactive.uav.Get()};
    ctx->CSSetShader(g_prep.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, g_params.GetAddressOf());
    ctx->CSSetShaderResources(0, 4, srvs);
    ctx->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
    ctx->Dispatch((g_renderW + 7) / 8, (g_renderH + 7) / 8, 1);
    ID3D11ShaderResourceView* noSrv[4] = {};
    ID3D11UnorderedAccessView* noUav[4] = {};
    ctx->CSSetShaderResources(0, 4, noSrv);
    ctx->CSSetUnorderedAccessViews(0, 4, noUav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    const uint64_t v = g_shm->submitted + 2;
    ipc::Frame& f = g_shm->frames[(v / 2) % ipc::kSlots];
    f.fenceValue = v;
    f.generation = g_generation;
    f.renderWidth = g_renderW;
    f.renderHeight = g_renderH;
    f.reset = frame.reset;
    f.jitterX = frame.jitterX * g_cfg.jitterSignX;
    f.jitterY = frame.jitterY * g_cfg.jitterSignY;
    // motion is in uv units; both sdks want render pixels
    f.mvScaleX = static_cast<float>(g_renderW);
    f.mvScaleY = static_cast<float>(g_renderH);
    f.sharpness = g_cfg.sharpness;
    f.frameTimeMs = frame.frameTimeMs;
    f.cameraNear = frame.cameraNear;
    f.cameraFar = frame.cameraFar;
    f.fovY = frame.fovY;
    f.preExposure = 1.0f;

    ctx4->Signal(g_fence.Get(), v);
    ctx->Flush();
    MemoryBarrier();
    g_shm->submitted = v;
    SetEvent(g_event);
    // the game's queue waits for the helper; the cpu does not
    ctx4->Wait(g_fence.Get(), v + 1);
    return g_output.srv.Get();
}

ID3D11Texture2D* Output()
{
    return g_output.tex.Get();
}

const char* Status()
{
    return g_status;
}

} // namespace upscale
