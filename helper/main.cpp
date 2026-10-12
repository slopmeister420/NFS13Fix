// NFS13Upscaler.exe - runs DLSS or FSR for NFS13Fix. Both SDKs only ship 64-bit binaries, so the
// 32-bit game shares its textures and a GPU fence with this process instead of loading them.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <memory>
#include <string>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_d3d.h"

#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"

#include "upscale_ipc.h"

using Microsoft::WRL::ComPtr;
namespace ipc = upscale_ipc;

namespace {

ipc::Shared* g_shm = nullptr;
std::wstring g_dir;
FILE* g_log = nullptr;

void Log(const char* fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g_log) {
        fprintf(g_log, "%s\n", line);
        fflush(g_log);
    }
    if (g_shm) {
        // keep the newest lines visible to the plugin
        size_t used = strnlen(g_shm->log, sizeof(g_shm->log));
        const size_t add = strlen(line) + 1;
        if (used + add >= sizeof(g_shm->log))
            used = 0;
        memcpy(g_shm->log + used, line, add - 1);
        g_shm->log[used + add - 1] = '\n';
        if (used + add < sizeof(g_shm->log))
            g_shm->log[used + add] = 0;
    }
}

void Fail(const char* fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Log("error: %s", line);
    if (g_shm) {
        strncpy_s(g_shm->error, line, _TRUNCATE);
        g_shm->state = ipc::kFailed;
    }
}

struct Inputs {
    ComPtr<ID3D12Resource> color, depth, motion, output, reactive;
};

struct Upscaler {
    virtual ~Upscaler() = default;
    virtual bool Create(ID3D12GraphicsCommandList* cl, const ipc::Resources& r) = 0;
    virtual bool Evaluate(ID3D12GraphicsCommandList* cl, const Inputs& in, const ipc::Resources& r,
                          const ipc::Frame& f) = 0;
};

// ---- DLSS -------------------------------------------------------------------------------

constexpr char kProjectId[] = "a0f57b54-1daf-4934-90ae-c4035c19df04";

NVSDK_NGX_PerfQuality_Value DlssQuality(uint32_t q)
{
    switch (q) {
    case 0: return NVSDK_NGX_PerfQuality_Value_DLAA;
    case 1: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case 2: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case 3: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    default: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    }
}

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    Log("ngx: %s", message);
}

struct Dlss : Upscaler {
    ID3D12Device* dev;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    bool initialised = false;

    explicit Dlss(ID3D12Device* d) : dev(d) {}
    ~Dlss() override
    {
        if (feature)
            NVSDK_NGX_D3D12_ReleaseFeature(feature);
        if (params)
            NVSDK_NGX_D3D12_DestroyParameters(params);
        if (initialised)
            NVSDK_NGX_D3D12_Shutdown1(dev);
    }

    bool Init()
    {
        const wchar_t* paths[] = {g_dir.c_str()};
        NVSDK_NGX_FeatureCommonInfo info{};
        info.PathListInfo.Path = paths;
        info.PathListInfo.Length = 1;
        info.LoggingInfo.LoggingCallback = NgxLog;
        info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
        info.LoggingInfo.DisableOtherLoggingSinks = true;
        NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(
            kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", g_dir.c_str(), dev, &info);
        if (NVSDK_NGX_FAILED(r)) {
            Log("dlss: init failed %08x", r);
            return false;
        }
        initialised = true;
        r = NVSDK_NGX_D3D12_GetCapabilityParameters(&params);
        int available = 0;
        if (NVSDK_NGX_FAILED(r) ||
            NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_Available, &available)) ||
            !available) {
            Log("dlss: not available on this gpu/driver");
            return false;
        }
        return true;
    }

    bool Create(ID3D12GraphicsCommandList* cl, const ipc::Resources& r) override
    {
        if (feature) {
            NVSDK_NGX_D3D12_ReleaseFeature(feature);
            feature = nullptr;
        }
        NVSDK_NGX_DLSS_Create_Params c{};
        c.Feature.InWidth = r.renderWidth;
        c.Feature.InHeight = r.renderHeight;
        c.Feature.InTargetWidth = r.displayWidth;
        c.Feature.InTargetHeight = r.displayHeight;
        c.Feature.InPerfQualityValue = DlssQuality(r.quality);
        c.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                 (r.hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0) |
                                 (r.depthInverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
        const NVSDK_NGX_Result res = NGX_D3D12_CREATE_DLSS_EXT(cl, 1, 1, &feature, params, &c);
        if (NVSDK_NGX_FAILED(res)) {
            Fail("dlss: create failed %08x", res);
            return false;
        }
        Log("dlss: %ux%u -> %ux%u quality %u", r.renderWidth, r.renderHeight, r.displayWidth, r.displayHeight, r.quality);
        return true;
    }

    bool Evaluate(ID3D12GraphicsCommandList* cl, const Inputs& in, const ipc::Resources&, const ipc::Frame& f) override
    {
        NVSDK_NGX_D3D12_DLSS_Eval_Params e{};
        e.Feature.pInColor = in.color.Get();
        e.Feature.pInOutput = in.output.Get();
        e.pInDepth = in.depth.Get();
        e.pInMotionVectors = in.motion.Get();
        e.InJitterOffsetX = f.jitterX;
        e.InJitterOffsetY = -f.jitterY; // dlss counts y up
        e.InRenderSubrectDimensions = {f.renderWidth, f.renderHeight};
        e.InReset = f.reset ? 1 : 0;
        e.InMVScaleX = f.mvScaleX;
        e.InMVScaleY = f.mvScaleY;
        e.InPreExposure = f.preExposure;
        e.InExposureScale = 1.0f;
        e.InFrameTimeDeltaInMsec = f.frameTimeMs;
        const NVSDK_NGX_Result res = NGX_D3D12_EVALUATE_DLSS_EXT(cl, feature, params, &e);
        if (NVSDK_NGX_FAILED(res)) {
            Log("dlss: evaluate failed %08x", res);
            return false;
        }
        return true;
    }
};

// ---- FSR --------------------------------------------------------------------------------

void FfxMessage(uint32_t type, const wchar_t* message)
{
    char narrow[512];
    WideCharToMultiByte(CP_UTF8, 0, message, -1, narrow, sizeof(narrow), nullptr, nullptr);
    Log("fsr %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", narrow);
}

struct Fsr : Upscaler {
    ID3D12Device* dev;
    HMODULE module = nullptr;
    ffxFunctions fn{};
    ffxContext ctx = nullptr;

    explicit Fsr(ID3D12Device* d) : dev(d) {}
    ~Fsr() override
    {
        if (ctx)
            fn.DestroyContext(&ctx, nullptr);
    }

    bool Init()
    {
        module = LoadLibraryW((g_dir + L"\\amd_fidelityfx_loader_dx12.dll").c_str());
        if (!module) {
            Log("fsr: amd_fidelityfx_loader_dx12.dll missing");
            return false;
        }
        ffxLoadFunctions(&fn, module);
        if (!fn.CreateContext || !fn.Dispatch || !fn.Query) {
            Log("fsr: loader exports missing");
            return false;
        }
        uint64_t count = 0;
        ffxQueryDescGetVersions q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        q.device = dev;
        q.outputCount = &count;
        fn.Query(nullptr, &q.header);
        if (!count) {
            Log("fsr: no upscaler provider");
            return false;
        }
        const char* names[8] = {};
        uint64_t ids[8] = {};
        count = count > 8 ? 8 : count;
        q.versionIds = ids;
        q.versionNames = names;
        fn.Query(nullptr, &q.header);
        for (uint64_t i = 0; i < count; ++i)
            Log("fsr: provider %s", names[i] ? names[i] : "?");
        return true;
    }

    bool Create(ID3D12GraphicsCommandList*, const ipc::Resources& r) override
    {
        if (ctx) {
            fn.DestroyContext(&ctx, nullptr);
            ctx = nullptr;
        }
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = dev;
        ffxCreateContextDescUpscaleVersion version{};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        version.version = FFX_UPSCALER_VERSION;
        version.header.pNext = &backend.header;
        ffxCreateContextDescUpscale c{};
        c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        c.header.pNext = &version.header;
        c.flags = (r.hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE
                         : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE) |
                  (r.depthInverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0);
        c.maxRenderSize = {r.renderWidth, r.renderHeight};
        c.maxUpscaleSize = {r.displayWidth, r.displayHeight};
        c.fpMessage = FfxMessage;
        const ffxReturnCode_t res = fn.CreateContext(&ctx, &c.header, nullptr);
        if (res != FFX_API_RETURN_OK) {
            Fail("fsr: create failed %u", res);
            ctx = nullptr;
            return false;
        }
        ffxQueryGetProviderVersion pv{};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        fn.Query(&ctx, &pv.header);
        Log("fsr: %s, %ux%u -> %ux%u", pv.versionName ? pv.versionName : "?", r.renderWidth, r.renderHeight,
            r.displayWidth, r.displayHeight);
        return true;
    }

    bool Evaluate(ID3D12GraphicsCommandList* cl, const Inputs& in, const ipc::Resources& r, const ipc::Frame& f) override
    {
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = cl;
        d.color = ffxApiGetResourceDX12(in.color.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = ffxApiGetResourceDX12(in.depth.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors = ffxApiGetResourceDX12(in.motion.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        // smoke and sparks have no depth or motion of their own: lean on the current frame there
        d.reactive = ffxApiGetResourceDX12(in.reactive.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output = ffxApiGetResourceDX12(in.output.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {f.jitterX, f.jitterY};
        d.motionVectorScale = {f.mvScaleX, f.mvScaleY};
        d.renderSize = {f.renderWidth, f.renderHeight};
        d.upscaleSize = {r.displayWidth, r.displayHeight};
        d.enableSharpening = f.sharpness > 0.0f;
        d.sharpness = f.sharpness;
        d.frameTimeDelta = f.frameTimeMs;
        d.preExposure = f.preExposure;
        d.reset = f.reset != 0;
        d.cameraNear = f.cameraNear;
        d.cameraFar = f.cameraFar;
        d.cameraFovAngleVertical = f.fovY;
        d.viewSpaceToMetersFactor = 1.0f;
        d.flags = r.hdr ? 0 : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
        const ffxReturnCode_t res = fn.Dispatch(&ctx, &d.header);
        if (res != FFX_API_RETURN_OK) {
            Log("fsr: dispatch failed %u", res);
            return false;
        }
        return true;
    }
};

// ---- device and shared resources ----------------------------------------------------------

struct Device {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc[ipc::kSlots];
    uint64_t allocDone[ipc::kSlots] = {};
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> local;
    uint64_t localValue = 0;
    HANDLE localEvent = nullptr;

    bool Create(uint64_t luid)
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
            return false;
        LUID l;
        memcpy(&l, &luid, sizeof(l));
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapterByLuid(l, IID_PPV_ARGS(&adapter)))) {
            Fail("adapter %016llx not found", luid);
            return false;
        }
        DXGI_ADAPTER_DESC1 ad;
        adapter->GetDesc1(&ad);
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
        Log("adapter: %s", name);
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) {
            Fail("D3D12CreateDevice failed");
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
            return false;
        for (auto& a : alloc)
            if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))))
                return false;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].Get(), nullptr, IID_PPV_ARGS(&list))))
            return false;
        list->Close();
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&local))))
            return false;
        localEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return true;
    }

    // waits for the gpu to finish with this slot's allocator, then starts recording on it
    ID3D12GraphicsCommandList* Begin(uint32_t slot)
    {
        if (local->GetCompletedValue() < allocDone[slot]) {
            local->SetEventOnCompletion(allocDone[slot], localEvent);
            WaitForSingleObject(localEvent, 2000);
        }
        alloc[slot]->Reset();
        list->Reset(alloc[slot].Get(), nullptr);
        return list.Get();
    }

    void End(uint32_t slot)
    {
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        allocDone[slot] = ++localValue;
        queue->Signal(local.Get(), localValue);
    }

    void Idle()
    {
        queue->Signal(local.Get(), ++localValue);
        local->SetEventOnCompletion(localValue, localEvent);
        WaitForSingleObject(localEvent, 5000);
    }
};

void Barrier(ID3D12GraphicsCommandList* cl, const Inputs& in, bool toUse)
{
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    struct {
        ID3D12Resource* r;
        D3D12_RESOURCE_STATES use;
    } items[] = {{in.color.Get(), read}, {in.depth.Get(), read}, {in.motion.Get(), read}, {in.reactive.Get(), read},
                 {in.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS}};
    D3D12_RESOURCE_BARRIER b[5] = {};
    for (int i = 0; i < 5; ++i) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[i].Transition.pResource = items[i].r;
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[i].Transition.StateBefore = toUse ? D3D12_RESOURCE_STATE_COMMON : items[i].use;
        b[i].Transition.StateAfter = toUse ? items[i].use : D3D12_RESOURCE_STATE_COMMON;
    }
    cl->ResourceBarrier(5, b);
}

template <class T>
bool OpenShared(ID3D12Device* dev, uint64_t handle, ComPtr<T>& out)
{
    out.Reset();
    return handle && SUCCEEDED(dev->OpenSharedHandle(reinterpret_cast<HANDLE>(handle), IID_PPV_ARGS(&out)));
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::wstring shmName, eventName;
    bool log = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--shm") && i + 1 < argc)
            shmName = argv[++i];
        else if (!wcscmp(argv[i], L"--event") && i + 1 < argc)
            eventName = argv[++i];
        else if (!wcscmp(argv[i], L"--log"))
            log = true;
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_dir = exe;
    g_dir.erase(g_dir.find_last_of(L'\\'));
    SetDllDirectoryW(g_dir.c_str());
    const std::wstring logPath = g_dir + L"\\NFS13Upscaler.log";
    if (log)
        g_log = _wfsopen(logPath.c_str(), L"w", _SH_DENYNO); // readable while the game runs
    else
        DeleteFileW(logPath.c_str());

    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmName.c_str());
    HANDLE frameEvent = OpenEventW(SYNCHRONIZE, FALSE, eventName.c_str());
    if (!mapping || !frameEvent) {
        Log("no shared memory or event (%ls / %ls)", shmName.c_str(), eventName.c_str());
        return 1;
    }
    g_shm = static_cast<ipc::Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ipc::Shared)));
    if (!g_shm || g_shm->magic != ipc::kMagic || g_shm->version != ipc::kVersion) {
        Log("shared memory version mismatch");
        return 1;
    }
    g_shm->helperPid = GetCurrentProcessId();
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, g_shm->parentPid);

    Device d;
    if (!d.Create(g_shm->adapterLuid))
        return 1;

    std::unique_ptr<Dlss> dlss(new Dlss(d.dev.Get()));
    std::unique_ptr<Fsr> fsr(new Fsr(d.dev.Get()));
    uint32_t available = 0;
    if (dlss->Init())
        available |= 1u << ipc::kDLSS;
    else
        dlss.reset();
    if (fsr->Init())
        available |= 1u << ipc::kFSR;
    else
        fsr.reset();
    g_shm->available = available;
    Log("ready: dlss %s, fsr %s", dlss ? "yes" : "no", fsr ? "yes" : "no");

    Inputs in;
    ComPtr<ID3D12Fence> fence;
    ipc::Resources res{};
    Upscaler* active = nullptr;
    uint32_t generation = 0;
    bool created = false;

    HANDLE waits[2] = {frameEvent, parent};
    for (;;) {
        const DWORD w = WaitForMultipleObjects(parent ? 2 : 1, waits, FALSE, 500);
        if (w == WAIT_OBJECT_0 + 1)
            break; // the game exited

        // the plugin (re)created its resources
        if (g_shm->resources.generation != generation) {
            d.Idle();
            res = g_shm->resources;
            generation = res.generation;
            created = false;
            const bool opened = OpenShared(d.dev.Get(), res.color, in.color) && OpenShared(d.dev.Get(), res.depth, in.depth) &&
                                OpenShared(d.dev.Get(), res.motion, in.motion) &&
                                OpenShared(d.dev.Get(), res.reactive, in.reactive) &&
                                OpenShared(d.dev.Get(), res.output, in.output) && OpenShared(d.dev.Get(), res.fence, fence);
            active = res.backend == ipc::kDLSS ? static_cast<Upscaler*>(dlss.get())
                     : res.backend == ipc::kFSR ? static_cast<Upscaler*>(fsr.get())
                                                : nullptr;
            if (!opened)
                Fail("could not open the shared resources");
            else if (!active)
                Fail("backend %u not available", res.backend);
            else {
                ID3D12GraphicsCommandList* cl = d.Begin(0);
                created = active->Create(cl, res);
                d.End(0);
                d.Idle();
            }
            if (created) {
                g_shm->error[0] = 0;
                g_shm->state = ipc::kReady;
            }
            g_shm->readyGeneration = generation;
        }
        if (!fence)
            continue;

        // every submitted frame gets its fence signalled, upscaled or not, so the game never waits forever
        while (g_shm->completed + 2 <= g_shm->submitted) {
            const uint64_t v = g_shm->completed + 2;
            const uint32_t slot = static_cast<uint32_t>((v / 2) % ipc::kSlots);
            const ipc::Frame f = g_shm->frames[slot];
            d.queue->Wait(fence.Get(), v);
            ID3D12GraphicsCommandList* cl = d.Begin(slot);
            if (created && f.generation == generation && f.fenceValue == v) {
                Barrier(cl, in, true);
                active->Evaluate(cl, in, res, f);
                Barrier(cl, in, false);
            }
            d.End(slot);
            d.queue->Signal(fence.Get(), v + 1);
            g_shm->completed = v;
        }
    }
    d.Idle();
    Log("exit");
    return 0;
}
