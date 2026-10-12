// DLSS / FSR in the game's renderer. The 3d scene renders below display size (renderscale), its
// projection gets a sub-pixel jitter each frame, and the final FXAA blit to the backbuffer is
// replaced by the upscaler's output. Everything here is D3D11-side: the jitter goes into the
// camera matrix as the engine uploads it, and motion vectors come from consecutive matrices.
// The choice also appears as graphics setting 14 in the game's own settings menu.

#include "upscaling.h"
#include "config.h"
#include "iat.h"
#include "log.h"
#include "renderscale.h"
#include "upscale.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <libmem/libmem.h>

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace upscaling {
namespace {

upscale::Config g_cfg;
std::wstring g_helperDir;
bool g_started = false, g_failed = false, g_noHelper = false;

// the menu value: 0 off, 1-5 dlss (dlaa, quality, balanced, performance, ultra performance),
// 6-10 fsr (native aa, quality, balanced, performance, ultra performance)
constexpr int kChoices = 11;
volatile LONG g_sel = 0;
volatile LONG g_pending = 0; // the menu changed g_sel: reconfigure on the render thread
volatile LONG g_rebuilt = 0; // the game rebuilt its render targets: drop cached views

int SelBackend(int sel) { return sel <= 0 ? 0 : sel <= 5 ? 1 : 2; }
int SelQuality(int sel) { return sel <= 0 ? 1 : sel <= 5 ? sel - 1 : sel - 6; }
int ToSel(int backend, int quality) { return backend == 1 ? 1 + quality : backend == 2 ? 6 + quality : 0; }
bool Active() { return g_sel > 0 && !g_failed; }

// device context vtable
constexpr size_t kPSSetSamplers = 10, kOMSetRenderTargets = 33, kUpdateSubresource = 48;
// contexts of devices created with different flags have different vtables
struct ContextVmt {
    lm_address_t* vtable;
    lm_vmt_t vmt;
};
ContextVmt g_ctxVmts[4]{};
int g_ctxCount = 0;

using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                      ID3D11DepthStencilView*);
using PSSetSamplersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);
using UpdateSubresourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                                     const void*, UINT, UINT);

template <typename Fn> Fn Original(ID3D11DeviceContext* ctx, size_t i)
{
    auto* vt = *reinterpret_cast<lm_address_t**>(ctx);
    for (int k = 0; k < g_ctxCount; ++k)
        if (g_ctxVmts[k].vtable == vt)
            return reinterpret_cast<Fn>(LM_VmtGetOriginal(&g_ctxVmts[k].vmt, i));
    return nullptr;
}

// what we know about a view or resource, cached by pointer (the game keeps its targets alive)
struct Target {
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ID3D11Texture2D* tex = nullptr;
};
std::unordered_map<ID3D11View*, Target> g_views;
std::unordered_map<ID3D11Resource*, bool> g_cbuffers;

Target Describe(ID3D11View* v)
{
    auto it = g_views.find(v);
    if (it != g_views.end())
        return it->second;
    Target t;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    ID3D11Texture2D* tex = nullptr;
    if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&tex)))) {
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        t.w = d.Width;
        t.h = d.Height;
        t.format = d.Format;
        t.tex = tex;
        tex->Release(); // the view keeps it alive
    }
    if (r)
        r->Release();
    v->AddRef(); // keeps the address from being reused while cached
    g_views.emplace(v, t);
    return t;
}

IDXGISwapChain* g_swapchain = nullptr;

bool IsBackbuffer(ID3D11Texture2D* tex)
{
    if (!tex || !g_swapchain)
        return false;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_swapchain->GetBuffer(0, IID_PPV_ARGS(&bb))))
        return false;
    bb->Release();
    return bb == tex;
}

// render state of the current frame
UINT g_rtCount = 0;
Target g_rt0, g_ds;
ID3D11Texture2D* g_depthTex = nullptr;
ID3D11ShaderResourceView* g_depthSrv = nullptr;
ID3D11ShaderResourceView* g_stencilSrv = nullptr;
ID3D11Texture2D* g_sceneColor = nullptr;
bool g_sceneActive = false;
Target g_last;
uint32_t g_renderW = 0, g_renderH = 0;

uint64_t g_frame = 0;
float g_jx = 0, g_jy = 0;        // this frame's jitter, render pixels
float g_cur[16], g_prev[16];     // unjittered clip matrices (column-vector convention)
bool g_haveCur = false, g_havePrev = false;
bool g_anyCamera = false;   // fallback when the per-frame buffer never shows up
uint32_t g_framesWithout = 0;
float g_camA = 0, g_camB = 0, g_camW[3] = {}; // z = A*d + B, and the view direction row, of the main camera
LARGE_INTEGER g_lastTime{};
uint32_t g_jittered = 0;

// moving objects: the g-buffer pass writes a stencil class (192 player car, 160 traffic, 144 scenery,
// 129 sky) that later passes only test against 128. so each moving object gets its own value 128 + id
// instead, and its pixels reproject through its own world matrix this frame and last rather than
// moving with the scenery. ids equal to values the game itself writes are skipped
struct Object {
    float world[16];
    const void* vb; // identifies the model across frames, never dereferenced
    UINT id;
};
uint32_t g_reserved[4] = {0x00010003u, 1, 1, 0}; // 0, 1 sky, 16 scenery, 32 and 64 the vehicle classes, and any seen
UINT g_nextId = 1;
ID3D11DepthStencilState* g_taggedDss = nullptr; // the state and ref we last set, and the class it replaced
UINT g_taggedRef = 0, g_taggedClass = 0;
std::vector<Object> g_objects;            // moving objects this frame, by id
std::vector<Object> g_seen, g_prevSeen;   // every distinct world matrix this frame and last
std::unordered_set<uint64_t> g_seenHash, g_prevSeenHash;
std::unordered_set<ID3D11Resource*> g_objectBuffers; // seen bound as the vertex shader's per-object buffer (b1)
ID3D11Texture2D* g_stencilCopy = nullptr; // the g-buffer stencil, before later passes overwrite it
// particles (smoke, sparks) go into their own premultiplied buffer that is then composited over the
// scene; its alpha is their coverage. kept from the end of the particle pass
ID3D11Texture2D* g_particleTex = nullptr;  // the game's buffer while it is bound
ID3D11Texture2D* g_particleCopy = nullptr;
ID3D11ShaderResourceView* g_particleSrv = nullptr;
bool g_particlesDone = false, g_particlesBound = false;
bool g_gbufferBound = false;

bool InScene()
{
    return g_renderW && ((g_rt0.w == g_renderW && g_rt0.h == g_renderH) ||
                         (g_ds.w == g_renderW && g_ds.h == g_renderH));
}

// the engine's camera matrix: rows x, y, view depth d, then (A, B, C, D) with z = A*d + B, w = C*d + D
bool IsCamera(const float* m)
{
    return m[14] == 1.0f && m[15] == 0.0f && m[12] > 1.0f && m[12] < 1.01f && m[13] < -0.001f && m[13] > -10.0f;
}

bool Near(float a, float b, float tol)
{
    return std::fabs(a - b) <= tol * (1.0f + std::fabs(b));
}

// 1: view-projection for row vectors (z column = A * w column + B), 2: the inverse the deferred
// passes use (rows: right, up, (0,0,0,1/B), (view direction, -A/B)), 0: neither
int OtherCameraForm(const float* m)
{
    if (g_camA == 0.0f)
        return 0;
    bool vp = m[3] != 0.0f || m[7] != 0.0f || m[11] != 0.0f;
    for (int r = 0; r < 4 && vp; ++r)
        vp = Near(m[r * 4 + 2], g_camA * m[r * 4 + 3] + (r == 3 ? g_camB : 0.0f), 1e-4f);
    if (vp)
        return 1;
    if (m[8] == 0.0f && m[9] == 0.0f && m[10] == 0.0f && Near(m[11] * g_camB, 1.0f, 1e-3f) &&
        Near(m[15], -m[11] * g_camA, 1e-3f) && Near(m[12], g_camW[0], 1e-3f) && Near(m[13], g_camW[1], 1e-3f) &&
        Near(m[14], g_camW[2], 1e-3f))
        return 2;
    return 0;
}

void ToClip(const float* m, float* out)
{
    for (int i = 0; i < 8; ++i)
        out[i] = m[i];
    for (int i = 0; i < 4; ++i) {
        out[8 + i] = m[12] * m[8 + i];
        out[12 + i] = m[14] * m[8 + i];
    }
    out[11] += m[13];
    out[15] += m[15];
}

// in double: the z and w rows are almost parallel (far/near ~ 1), so float cofactors cancel badly and
// a still camera reprojected to a slight zoom and shift
bool Invert(const float* mf, double* inv)
{
    double m[16], t[16];
    for (int i = 0; i < 16; ++i)
        m[i] = mf[i];
    t[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    t[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    t[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    t[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    t[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    t[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    t[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    t[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    t[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    t[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    t[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    t[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    t[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    t[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    t[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    t[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * t[0] + m[1] * t[4] + m[2] * t[8] + m[3] * t[12];
    if (std::fabs(det) < 1e-30)
        return false;
    for (int i = 0; i < 16; ++i)
        inv[i] = t[i] / det;
    return true;
}

// camera position: the point the x, y and w rows of the clip matrix all map to zero
bool CameraPos(const float* m, float* c)
{
    const float* r[3] = {m, m + 4, m + 12};
    // solve r[i].xyz . c = -r[i].w
    const double a00 = r[0][0], a01 = r[0][1], a02 = r[0][2], a10 = r[1][0], a11 = r[1][1], a12 = r[1][2], a20 = r[2][0],
                 a21 = r[2][1], a22 = r[2][2];
    const double det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
    if (std::fabs(det) < 1e-12)
        return false;
    const double b0 = -r[0][3], b1 = -r[1][3], b2 = -r[2][3];
    c[0] = static_cast<float>((b0 * (a11 * a22 - a12 * a21) - a01 * (b1 * a22 - a12 * b2) + a02 * (b1 * a21 - a11 * b2)) / det);
    c[1] = static_cast<float>((a00 * (b1 * a22 - a12 * b2) - b0 * (a10 * a22 - a12 * a20) + a02 * (a10 * b2 - b1 * a20)) / det);
    c[2] = static_cast<float>((a00 * (a11 * b2 - b1 * a21) - a01 * (a10 * b2 - b1 * a20) + b0 * (a10 * a21 - a11 * a20)) / det);
    return true;
}

void Mul(const float* a, const double* b, float* out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r * 4 + c] = static_cast<float>(a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] +
                                                a[r * 4 + 3] * b[12 + c]);
}

void HookVariants(ID3D11DeviceContext* ctx);
void InstallMenu();
void DropCaches();
void TrackParticles(ID3D11DeviceContext* ctx);
void EndFrame();
bool UpscaleFrame(ID3D11DeviceContext* ctx);
ID3D11RenderTargetView* Dummy(ID3D11DeviceContext* ctx, UINT w, UINT h);

// the menu changed the choice: switch the helper over on the render thread
void ApplyPending()
{
    if (!InterlockedExchange(&g_pending, 0))
        return;
    const int sel = g_sel;
    g_cfg.backend = SelBackend(sel) == 2 ? upscale::Backend::FSR : upscale::Backend::DLSS;
    g_cfg.quality = static_cast<uint32_t>(SelQuality(sel));
    g_cfg.jitterSignY = 1.0f; // the helper turns y around for dlss
    if (g_started && sel)
        upscale::Reconfigure(g_cfg.backend, g_cfg.quality);
    g_havePrev = g_haveCur = false;
    LOGF("[upscale] menu: %s, quality %d", sel ? (SelBackend(sel) == 1 ? "dlss" : "fsr") : "off", SelQuality(sel));
}

void STDMETHODCALLTYPE HookedOMSetRenderTargets(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs,
                                                ID3D11DepthStencilView* dsv)
{
    const auto orig = Original<OMSetRenderTargetsFn>(ctx, kOMSetRenderTargets);
    InstallMenu();
    // drop cached views when the rebuild starts and again once the old targets are gone
    static uint32_t calls = 0, dropAgain = 0;
    ++calls;
    if (InterlockedExchange(&g_rebuilt, 0)) {
        DropCaches();
        dropAgain = calls + 4000;
    } else if (dropAgain && calls >= dropAgain) {
        dropAgain = 0;
        DropCaches();
    }
    ApplyPending();
    if (!g_renderW || !Active())
        return orig(ctx, n, rtvs, dsv);
    HookVariants(ctx);
    g_rtCount = 0;
    g_rt0 = {};
    for (UINT i = 0; i < n; ++i)
        if (rtvs && rtvs[i]) {
            if (!g_rtCount)
                g_rt0 = Describe(rtvs[i]);
            ++g_rtCount;
        }
    g_ds = dsv ? Describe(dsv) : Target{};
    // the g-buffer pass: its depth is the scene depth the upscaler needs
    const bool gbuffer = g_rtCount >= 3 && g_ds.tex && g_ds.w == g_renderW && g_ds.h == g_renderH;
    if (g_gbufferBound && !gbuffer && g_stencilCopy && g_depthTex)
        ctx->CopyResource(g_stencilCopy, g_depthTex);
    g_gbufferBound = gbuffer;
    TrackParticles(ctx);
    if (gbuffer) {
        g_sceneActive = true;
        if (g_ds.tex != g_depthTex) {
            if (g_depthSrv)
                g_depthSrv->Release();
            if (g_stencilSrv)
                g_stencilSrv->Release();
            g_depthSrv = g_stencilSrv = nullptr;
            g_depthTex = g_ds.tex;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            const HRESULT hr = dev->CreateShaderResourceView(g_depthTex, &sd, &g_depthSrv);
            if (g_stencilCopy)
                g_stencilCopy->Release();
            g_stencilCopy = nullptr;
            D3D11_TEXTURE2D_DESC td;
            g_depthTex->GetDesc(&td);
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.CPUAccessFlags = td.MiscFlags = 0;
            sd.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
            if (td.Format != DXGI_FORMAT_R24G8_TYPELESS || FAILED(dev->CreateTexture2D(&td, nullptr, &g_stencilCopy)) ||
                FAILED(dev->CreateShaderResourceView(g_stencilCopy, &sd, &g_stencilSrv))) {
                g_stencilSrv = nullptr;
                LOGF("[upscale] no stencil copy (depth format %d): vehicles move with the scenery", td.Format);
            }
            dev->Release();
            LOGF("[upscale] scene depth %ux%u format %d: srv %08X", g_ds.w, g_ds.h, g_ds.format, static_cast<unsigned>(hr));
        }
    }
    // the last scene-sized target written before the backbuffer is the finished scene
    const bool backbuffer = g_sceneActive && g_rtCount == 1 && IsBackbuffer(g_rt0.tex);
    if (g_sceneActive && !backbuffer && g_rtCount == 1 && g_rt0.w == g_renderW && g_rt0.h == g_renderH)
        g_sceneColor = g_rt0.tex;
    if (backbuffer && !g_sceneColor) {
        static int logged = 0;
        if (logged++ < 3)
            LOGF("[upscale] no scene colour before the backbuffer (last target %ux%u fmt %d)", g_last.w, g_last.h, g_last.format);
    }
    if (!backbuffer && g_rtCount)
        g_last = g_rt0;
    if (backbuffer) {
        g_sceneActive = false;
        // the game's fxaa blit (and letterbox quads) go to a scratch target; binding it first also
        // unbinds the scene colour, which the upscaler reads
        if (ID3D11RenderTargetView* dummy = Dummy(ctx, g_rt0.w, g_rt0.h)) {
            orig(ctx, 1, &dummy, nullptr);
            const bool done = UpscaleFrame(ctx);
            EndFrame();
            if (done)
                return;
        } else {
            EndFrame();
        }
    }
    orig(ctx, n, rtvs, dsv);
}

bool Reserved(UINT id)
{
    return (g_reserved[id >> 5] >> (id & 31)) & 1;
}

// world: the per-object buffer's world matrix, or null for an upload that is not a vehicle's
// the particle buffer: the first single half-float scene-sized target drawn with the scene depth after
// the g-buffer
void TrackParticles(ID3D11DeviceContext* ctx)
{
    if (g_particlesBound) {
        g_particlesBound = false;
        g_particlesDone = true;
        if (g_particleCopy)
            ctx->CopyResource(g_particleCopy, g_particleTex);
    }
    const bool half = g_rt0.format == DXGI_FORMAT_R16G16B16A16_TYPELESS || g_rt0.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (g_particlesDone || !g_sceneActive || g_gbufferBound || g_rtCount != 1 || !half || g_rt0.w != g_renderW ||
        g_rt0.h != g_renderH || !g_depthTex || g_ds.tex != g_depthTex)
        return;
    if (g_particleTex != g_rt0.tex) {
        if (g_particleSrv)
            g_particleSrv->Release();
        if (g_particleCopy)
            g_particleCopy->Release();
        g_particleSrv = nullptr;
        g_particleCopy = nullptr;
        g_particleTex = g_rt0.tex;
        D3D11_TEXTURE2D_DESC td;
        g_particleTex->GetDesc(&td);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = td.MiscFlags = 0;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        const bool ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_particleCopy)) &&
                        SUCCEEDED(dev->CreateShaderResourceView(g_particleCopy, &sd, &g_particleSrv));
        dev->Release();
        if (!ok) {
            if (g_particleCopy)
                g_particleCopy->Release();
            g_particleCopy = nullptr;
            g_particleSrv = nullptr;
        }
        LOGF("[upscale] particle buffer %ux%u format %d: %s", g_rt0.w, g_rt0.h, g_rt0.format, ok ? "tracked" : "copy failed");
    }
    g_particlesBound = true;
}

// a world matrix as the object buffers upload it: rows x, y, z axis, translation. the fourth column
// carries other per-object values, so only the camera matrices sharing the layout are ruled out
bool IsWorld(const float* m)
{
    if (IsCamera(m) || OtherCameraForm(m))
        return false;
    for (int r = 0; r < 3; ++r) {
        const float l = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
        if (!(l > 1e-6f && l < 1e6f))
            return false;
    }
    return std::isfinite(m[12]) && std::isfinite(m[13]) && std::isfinite(m[14]);
}

// the placement only: a change in the fourth column (a fade, say) does not move anything
uint64_t WorldHash(const float* m)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (int r = 0; r < 4; ++r) {
        const auto* b = reinterpret_cast<const uint8_t*>(m + r * 4);
        for (int i = 0; i < 12; ++i)
            h = (h ^ b[i]) * 0x100000001b3ull;
    }
    return h;
}

// every object upload of the g-buffer pass: a world matrix that last frame did not have is a moving
// object (a car, a wheel, a cone knocked flying), so its draws get their own stencil value and its
// pixels reproject through its own motion. everything else keeps the game's value
void TagObject(ID3D11DeviceContext* ctx, const float* world)
{
    ID3D11DepthStencilState* dss = nullptr;
    UINT ref = 0;
    ctx->OMGetDepthStencilState(&dss, &ref);
    if (!dss)
        return;
    const UINT cls = dss == g_taggedDss && ref == g_taggedRef ? g_taggedClass : ref;
    if ((cls & 0x80) && !Reserved(cls & 0x7F)) {
        g_reserved[(cls & 0x7F) >> 5] |= 1u << (cls & 31);
        LOGF("[upscale] stencil value %u is in use: not used as an object id", cls);
    }
    UINT tag = cls;
    if (world) {
        const uint64_t h = WorldHash(world);
        ID3D11Buffer* vb = nullptr;
        UINT stride = 0, offset = 0;
        ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
        if (vb)
            vb->Release();
        if (g_seenHash.insert(h).second) {
            Object o;
            memcpy(o.world, world, sizeof(o.world));
            o.vb = vb;
            o.id = 0;
            g_seen.push_back(o);
        }
        // moving: assign (or reuse, for the object's other parts) an id
        if (!g_prevSeenHash.empty() && !g_prevSeenHash.count(h)) {
            size_t i = 0;
            while (i < g_objects.size() && memcmp(g_objects[i].world, world, sizeof(Object::world)))
                ++i;
            if (i == g_objects.size()) {
                while (g_nextId < 128 && Reserved(g_nextId))
                    ++g_nextId;
                if (g_nextId < 128) {
                    Object o;
                    memcpy(o.world, world, sizeof(o.world));
                    o.vb = vb;
                    o.id = g_nextId++;
                    g_objects.push_back(o);
                }
            }
            // out of ids: the object moves with the scenery
            if (i < g_objects.size())
                tag = 0x80 | g_objects[i].id;
        }
    }
    // a static object must not keep the value set for a moving one drawn before it; other uploads
    // (materials, the camera) leave the draw's value alone
    if (world && tag != ref) {
        ctx->OMSetDepthStencilState(dss, tag);
        g_taggedDss = dss;
        g_taggedRef = tag;
        g_taggedClass = cls;
    }
    dss->Release();
}

UpdateSubresourceFn g_usTramp[4]{};
void* g_usTarget[4]{};
int g_usCount = 0;


void UpdateSubresourceImpl(UpdateSubresourceFn orig, ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT sub,
                           const D3D11_BOX* box, const void* data, UINT pitch, UINT depthPitch)
{
    if (box || !data || !Active() || !InScene())
        return orig(ctx, dst, sub, box, data, pitch, depthPitch);
    auto it = g_cbuffers.find(dst);
    if (it == g_cbuffers.end()) {
        D3D11_RESOURCE_DIMENSION dim;
        dst->GetType(&dim);
        bool cb = false;
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC d;
            static_cast<ID3D11Buffer*>(dst)->GetDesc(&d);
            cb = (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && d.ByteWidth >= 64 && d.ByteWidth <= 4096;
        }
        dst->AddRef(); // keeps the address from being reused while cached
        it = g_cbuffers.emplace(dst, cb).first;
    }
    if (!it->second)
        return orig(ctx, dst, sub, box, data, pitch, depthPitch);

    D3D11_BUFFER_DESC d;
    static_cast<ID3D11Buffer*>(dst)->GetDesc(&d);
    const auto* f = static_cast<const float*>(data);
    const UINT floats = d.ByteWidth / 4;
    // every per-object buffer (vertex shader b1) ends in the object's world matrix, whatever comes
    // before it. the game rebinds the same buffer for each draw, so the binding left from the last
    // draw identifies it; other buffers can end in matrices that are not placements
    if (g_stencilSrv && g_gbufferBound) {
        if (!g_objectBuffers.count(dst)) {
            ID3D11Buffer* bound = nullptr;
            ctx->VSGetConstantBuffers(1, 1, &bound);
            if (bound) {
                if (bound == dst)
                    g_objectBuffers.insert(dst);
                bound->Release();
            }
        }
        TagObject(ctx, g_objectBuffers.count(dst) && IsWorld(f + floats - 16) ? f + floats - 16 : nullptr);
    }
    alignas(16) float copy[1024];
    bool changed = false;
    const float jxn = 2.0f * g_jx / g_renderW, jyn = -2.0f * g_jy / g_renderH; // jitter in ndc
    for (UINT i = 0; i + 16 <= floats; i += 4) {
        if (!IsCamera(f + i)) {
            // the same camera in the other forms some passes use: a full (world)view-projection for
            // row vectors (sky, particles, scattering), and the inverse the lighting rebuilds
            // positions with. jitter them alike, or those passes stay unjittered and alias in motion
            const int kind = OtherCameraForm(f + i);
            if (!kind)
                continue;
            if (!changed) {
                memcpy(copy, f, d.ByteWidth);
                changed = true;
            }
            if (kind == 1) {
                for (int r = 0; r < 4; ++r) {
                    copy[i + r * 4 + 0] += jxn * f[i + r * 4 + 3];
                    copy[i + r * 4 + 1] += jyn * f[i + r * 4 + 3];
                }
            } else {
                for (int c = 0; c < 4; ++c)
                    copy[i + 12 + c] -= jxn * f[i + c] + jyn * f[i + 4 + c];
            }
            ++g_jittered;
            i += 12;
            continue;
        }
        if (!changed) {
            memcpy(copy, f, d.ByteWidth);
            changed = true;
        }
        // the g-buffer pass uploads two slightly different camera matrices; motion vectors need the
        // same one every frame, or they swing between the two. the main one sits at float 4 of the
        // 80-byte per-frame buffer (time, matrix)
        if (!g_haveCur && g_rtCount >= 3 && (g_anyCamera || (d.ByteWidth == 80 && i == 4))) {
            ToClip(f + i, g_cur);
            g_haveCur = true;
            g_camA = f[i + 12];
            g_camB = f[i + 13];
            memcpy(g_camW, f + i + 8, sizeof(g_camW));
        }
        // x += jx_ndc * w, y += jy_ndc * w, with w = C * d + D and D = 0
        const float jx = 2.0f * g_jx / g_renderW * f[i + 14], jy = -2.0f * g_jy / g_renderH * f[i + 14];
        for (int k = 0; k < 4; ++k) {
            copy[i + k] += jx * f[i + 8 + k];
            copy[i + 4 + k] += jy * f[i + 8 + k];
        }
        ++g_jittered;
        i += 12;
    }
    orig(ctx, dst, sub, box, changed ? copy : data, pitch, depthPitch);
}

template <int N>
void STDMETHODCALLTYPE HookedUpdateSubresource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT sub,
                                               const D3D11_BOX* box, const void* data, UINT pitch, UINT depthPitch)
{
    UpdateSubresourceImpl(g_usTramp[N], ctx, dst, sub, box, data, pitch, depthPitch);
}

constexpr UpdateSubresourceFn kUsHooks[4] = {&HookedUpdateSubresource<0>, &HookedUpdateSubresource<1>,
                                            &HookedUpdateSubresource<2>, &HookedUpdateSubresource<3>};

// detour whichever UpdateSubresource implementation the context currently points at
void HookVariants(ID3D11DeviceContext* ctx)
{
    void* fn = (*reinterpret_cast<void***>(ctx))[kUpdateSubresource];
    for (int k = 0; k < g_usCount; ++k)
        if (g_usTarget[k] == fn || reinterpret_cast<void*>(kUsHooks[k]) == fn)
            return;
    if (g_usCount == 4)
        return;
    const auto* b = static_cast<const uint8_t*>(fn);
    if (!(b[0] == 0x8B && b[1] == 0xFF && b[2] == 0x55)) { // mov edi,edi; push ebp
        LOGF("[upscale] UpdateSubresource at %p has an unexpected prologue, not hooking", fn);
        g_usTarget[g_usCount++] = fn; // remember so we do not retry
        return;
    }
    lm_address_t tramp = 0;
    if (!LM_HookCode(reinterpret_cast<lm_address_t>(fn), reinterpret_cast<lm_address_t>(kUsHooks[g_usCount]), &tramp)) {
        LOGF("[upscale] could not detour UpdateSubresource at %p", fn);
        g_usTarget[g_usCount++] = fn;
        return;
    }
    g_usTramp[g_usCount] = reinterpret_cast<UpdateSubresourceFn>(tramp);
    g_usTarget[g_usCount++] = fn;
    LOGF("[upscale] UpdateSubresource variant %p detoured", fn);
}

void EndFrame()
{
    ++g_frame;
    if (!g_haveCur && !g_anyCamera && ++g_framesWithout == 120) {
        g_anyCamera = true;
        LOGF("[upscale] no per-frame camera buffer seen - taking the first camera matrix instead");
    }
    upscale::Jitter(g_frame, g_renderW, renderscale::DisplayWidth(), g_jx, g_jy);
    if (g_haveCur) {
        memcpy(g_prev, g_cur, sizeof(g_prev));
        g_havePrev = true;
    } else {
        g_havePrev = false;
    }
    g_haveCur = false;
    g_jittered = 0;
    g_objects.clear();
    g_prevSeen.swap(g_seen);
    g_seen.clear();
    g_prevSeenHash.swap(g_seenHash);
    g_seenHash.clear();
    g_nextId = 1;
    g_particlesDone = g_particlesBound = false;
}

std::unordered_map<ID3D11Texture2D*, std::pair<ID3D11ShaderResourceView*, DXGI_FORMAT>> g_colorSrvs;

ID3D11ShaderResourceView* ColorSrv(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, DXGI_FORMAT& format)
{
    auto it = g_colorSrvs.find(tex);
    if (it == g_colorSrvs.end()) {
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        DXGI_FORMAT f = d.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS ? DXGI_FORMAT_R16G16B16A16_FLOAT
                        : d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS   ? DXGI_FORMAT_R8G8B8A8_UNORM
                                                                      : d.Format;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = f;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        ID3D11ShaderResourceView* v = nullptr;
        const HRESULT hr = dev->CreateShaderResourceView(tex, &sd, &v);
        dev->Release();
        LOGF("[upscale] scene colour %ux%u format %d -> view %d: %08X", d.Width, d.Height, d.Format, f, static_cast<unsigned>(hr));
        it = g_colorSrvs.emplace(tex, std::make_pair(v, f)).first;
    }
    format = it->second.second;
    return it->second.first;
}

// a target for the game's own fxaa blit to land in once we have drawn the frame
ID3D11RenderTargetView* Dummy(ID3D11DeviceContext* ctx, UINT w, UINT h)
{
    static ID3D11RenderTargetView* rtv = nullptr;
    static UINT dw = 0, dh = 0;
    if (rtv && dw == w && dh == h)
        return rtv;
    if (rtv)
        rtv->Release();
    rtv = nullptr;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC d{w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    ID3D11Texture2D* t = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t))) {
        dev->CreateRenderTargetView(t, nullptr, &rtv);
        t->Release();
    }
    dev->Release();
    dw = w;
    dh = h;
    return rtv;
}

// the backbuffer is about to receive the finished scene: upscale it there ourselves
// the world matrix as uploaded (rows: x, y, z axis, translation) as a column-vector matrix
void WorldColumns(const float* w, float* out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r * 4 + c] = r < 3 ? w[c * 4 + r] : (c == 3 ? 1.0f : 0.0f);
}

void MulD(const double* a, const double* b, double* out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

// current clip -> world -> the object's own space -> where it was last frame -> previous clip
void ObjectReprojections(const double* invCur, upscale::Frame& fr)
{
    for (const Object& o : g_objects) {
        const Object* best = nullptr;
        float bestD = 20.0f * 20.0f; // further than a frame's travel at any speed: not the same object
        for (const Object& p : g_prevSeen) {
            if (p.vb != o.vb)
                continue;
            const float dx = p.world[12] - o.world[12], dy = p.world[13] - o.world[13], dz = p.world[14] - o.world[14];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < bestD) {
                bestD = d2;
                best = &p;
            }
        }
        float wc[16], wp[16];
        double invW[16];
        WorldColumns(o.world, wc);
        if (!best || !Invert(wc, invW))
            continue;
        WorldColumns(best->world, wp);
        double prev[16], prevW[16], t[16], u[16];
        for (int i = 0; i < 16; ++i) {
            prev[i] = g_prev[i];
            prevW[i] = wp[i];
        }
        MulD(prev, prevW, t);
        MulD(t, invW, u);
        MulD(u, invCur, t);
        for (int i = 0; i < 16; ++i)
            fr.objects[o.id][i] = static_cast<float>(t[i]);
    }
}

bool UpscaleFrame(ID3D11DeviceContext* ctx)
{
    if (!g_sceneColor || !g_depthSrv)
        return false;
    if (!g_started) {
        // the helper starts on the device the game renders with (it makes throwaway ones first)
        g_started = true;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        g_cfg.helperDir = g_helperDir.c_str();
        g_failed = !upscale::Init(dev, g_cfg);
        LOGF("[upscale] feature level %04X, %s helper: %s", dev->GetFeatureLevel(),
             g_cfg.backend == upscale::Backend::DLSS ? "dlss" : "fsr", upscale::Status());
        dev->Release();
    }
    if (g_failed)
        return false;
    DXGI_FORMAT colorFormat;
    static upscale::Frame fr; // 8 kb of vehicle matrices: keep it off the render thread's stack
    memset(&fr, 0, sizeof(fr));
    fr.color = ColorSrv(ctx, g_sceneColor, colorFormat);
    fr.depth = g_depthSrv;
    fr.stencil = g_stencilSrv;
    fr.particles = g_particlesDone ? g_particleSrv : nullptr;
    if (!fr.color)
        return false;
    fr.jitterX = g_jx;
    fr.jitterY = g_jy;
    double inv[16];
    if (g_haveCur && g_havePrev && Invert(g_cur, inv)) {
        Mul(g_prev, inv, fr.reproject);
        // a camera cut jumps the camera: drop history
        float a[3], b[3];
        fr.reset = !CameraPos(g_prev, a) || !CameraPos(g_cur, b) ||
                   (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]) > 15.0f * 15.0f;
        if (fr.reset)
            LOGF("[upscale] history reset: camera moved from %.1f %.1f %.1f to %.1f %.1f %.1f", a[0], a[1], a[2], b[0], b[1], b[2]);
        for (auto& o : fr.objects)
            memcpy(o, fr.reproject, sizeof(o));
        if (!fr.reset)
            ObjectReprojections(inv, fr);
    } else {
        for (int i = 0; i < 16; ++i)
            fr.reproject[i] = (i % 5) ? 0.0f : 1.0f;
        for (auto& o : fr.objects)
            memcpy(o, fr.reproject, sizeof(o));
        fr.reset = true;
    }
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    fr.frameTimeMs = g_lastTime.QuadPart ? static_cast<float>((now.QuadPart - g_lastTime.QuadPart) * 1000.0 / freq.QuadPart) : 16.7f;
    g_lastTime = now;
    fr.cameraNear = 0.3f;
    fr.cameraFar = 15000.0f;
    fr.fovY = 1.0f;

    ID3D11ShaderResourceView* out = upscale::Run(ctx, fr, g_rt0.w, g_rt0.h, colorFormat, g_rt0.format);
    // log status changes only
    static std::string last;
    if (last != upscale::Status()) {
        last = upscale::Status();
        LOGF("[upscale] %s (%u matrix uploads jittered)", last.c_str(), g_jittered);
    }
    if (out)
        ctx->CopyResource(g_rt0.tex, upscale::Output());
    return out != nullptr;
}

// ------------------------------------------------------------- settings menu

// render targets were rebuilt: views and textures we cached by address may be gone or reused
void DropCaches()
{
    for (auto& entry : g_views)
        entry.first->Release();
    g_views.clear();
    for (auto& entry : g_cbuffers)
        entry.first->Release();
    g_cbuffers.clear();
    for (auto& entry : g_colorSrvs)
        if (entry.second.first)
            entry.second.first->Release();
    g_colorSrvs.clear();
    if (g_depthSrv)
        g_depthSrv->Release();
    if (g_stencilSrv)
        g_stencilSrv->Release();
    if (g_stencilCopy)
        g_stencilCopy->Release();
    g_depthSrv = g_stencilSrv = nullptr;
    g_stencilCopy = nullptr;
    if (g_particleSrv)
        g_particleSrv->Release();
    if (g_particleCopy)
        g_particleCopy->Release();
    g_particleSrv = nullptr;
    g_particleCopy = nullptr;
    g_particleTex = nullptr;
    g_particlesDone = g_particlesBound = false;
    g_depthTex = nullptr;
    g_gbufferBound = false;
    g_objects.clear();
    g_seen.clear();
    g_prevSeen.clear();
    g_seenHash.clear();
    g_prevSeenHash.clear();
    g_objectBuffers.clear();
    g_sceneColor = nullptr;
    g_sceneActive = false;
    g_haveCur = g_havePrev = false;
}

// the game's graphics settings are SettingBase objects in a 14-entry map; retail uses 13, so
// setting 14 (method: off, dlss, fsr) goes into the map, where the menu's save and revert passes
// reach it. setting 15 (quality) does not fit and is served by hooking the view model's lookups,
// which every per-setting script hook goes through. layout and vtable come from the super
// sampling setting (13): its generic methods (get/set value, min/max, revert, binding path) work
// on these fields as they are; construct, prepare (load), save and action (apply) are ours.
struct MenuSetting {
    void** vtable;
    void* owner;
    int value;
    int max;
    int min;
    uint8_t dirty;
    uint8_t pad[3];
};
constexpr int kMethodIndex = 14, kQualityIndex = 15;
constexpr int kSlotConstruct = 0, kSlotPrepare = 1, kSlotSave = 8, kSlotAction = 10, kSlots = 14;
void* g_menuVt[kSlots];
MenuSetting g_menuMethod{}, g_menuQuality{};
volatile LONG g_quality = 1; // kept while the method is off
bool g_menuDone = false;

using GetSettingsFn = uint8_t*(__fastcall*)(void* self, void* edx); // thiscall, no arguments
void** g_settingsGlobal = nullptr;
GetSettingsFn g_getSettings = nullptr;

void __fastcall MenuPrepare(MenuSetting* s, void*)
{
    s->value = s == &g_menuQuality ? static_cast<int>(g_quality) : SelBackend(g_sel);
    s->dirty = 0;
}

void __fastcall MenuConstruct(MenuSetting* s, void*, void* owner)
{
    s->owner = owner;
    s->max = s == &g_menuQuality ? 4 : 2;
    s->min = 0;
    MenuPrepare(s, nullptr);
}

// apply either row: the pair decides. the next frames reconfigure, and the game rebuilds its
// render targets at the new size
void __fastcall MenuAction(MenuSetting*, void*)
{
    const int method = g_noHelper ? 0 : g_menuMethod.value < 0 ? 0 : g_menuMethod.value > 2 ? 2 : g_menuMethod.value;
    const int quality = g_menuQuality.value < 0 ? 0 : g_menuQuality.value > 4 ? 4 : g_menuQuality.value;
    g_menuMethod.value = method;
    g_menuQuality.value = quality;
    InterlockedExchange(&g_quality, quality);
    const int sel = ToSel(method, quality);
    if (sel == g_sel)
        return;
    InterlockedExchange(&g_sel, sel);
    InterlockedExchange(&g_pending, 1);
    if (g_settingsGlobal && g_getSettings)
        if (uint8_t* gs = g_getSettings(*g_settingsGlobal, nullptr))
            *reinterpret_cast<uint16_t*>(gs + 0x388) = 0x101; // the flag super sampling raises
}

void __fastcall MenuSave(MenuSetting* s, void*)
{
    MenuAction(s, nullptr);
    config::Save("Upscaler", SelBackend(g_sel));
    config::Save("UpscaleQuality", g_quality);
}

// graphics view model lookups, so setting 15 exists outside the full map
constexpr size_t kVmIsValid = 2, kVmGetConst = 3, kVmGet = 4;
lm_vmt_t g_vmVmt{};
using VmIsValidFn = bool(__fastcall*)(void* vm, void* edx, int index);
using VmGetFn = MenuSetting*(__fastcall*)(void* vm, void* edx, int index);

bool __fastcall VmIsValid(void* vm, void*, int index)
{
    return index == kQualityIndex ||
           reinterpret_cast<VmIsValidFn>(LM_VmtGetOriginal(&g_vmVmt, kVmIsValid))(vm, nullptr, index);
}

MenuSetting* __fastcall VmGetConst(void* vm, void*, int index)
{
    return index == kQualityIndex ? &g_menuQuality
                                  : reinterpret_cast<VmGetFn>(LM_VmtGetOriginal(&g_vmVmt, kVmGetConst))(vm, nullptr, index);
}

MenuSetting* __fastcall VmGet(void* vm, void*, int index)
{
    return index == kQualityIndex ? &g_menuQuality
                                  : reinterpret_cast<VmGetFn>(LM_VmtGetOriginal(&g_vmVmt, kVmGet))(vm, nullptr, index);
}

uint32_t Read32(uintptr_t a)
{
    return *reinterpret_cast<const uint32_t*>(a);
}

// walks to the graphics view model and adds our two settings once the game made it
void InstallMenu()
{
    if (g_menuDone)
        return;
    static uint32_t calls = 0;
    if (++calls % 64)
        return;
    __try {
        // the settings manager, as the GetSettingsValue script hook reaches it
        const uintptr_t root = Read32(0x117e9f4);
        if (!root || !Read32(root + 0x2020) || !Read32(Read32(root + 0x2020) + 0x2f3e0))
            return;
        const uintptr_t mgr = Read32(Read32(root + 0x2020) + 0x2f3e0) + 0x4210;
        const uint32_t cats = Read32(mgr + 0x8d4);
        if (cats == 0 || cats > 6)
            return;
        uintptr_t vm = 0;
        for (uint32_t i = 0; i < cats; ++i)
            if (Read32(mgr + 0x8d8 + i * 4) == 1)
                vm = Read32(mgr + 0x8f0 + i * 4);
        if (!vm)
            return;
        // map at vm+4: count, 14 keys, 14 values
        const uint32_t count = Read32(vm + 4);
        if (count != 13 || Read32(vm + 8 + 12 * 4) != 13) {
            LOGF("[upscale] graphics settings map has an unexpected shape (%u entries) - no menu option", count);
            g_menuDone = true;
            return;
        }
        auto* ssaa = reinterpret_cast<MenuSetting*>(Read32(vm + 0x40 + 12 * 4));
        const auto* action = static_cast<const uint8_t*>(ssaa->vtable[kSlotAction]);
        // mov ecx,[settings global]; call GetSettings; mov [eax+0x394],esi
        static const uint8_t kStore[] = {0x89, 0xB0, 0x94, 0x03, 0x00, 0x00};
        // the view model's lookups open with mov eax,[ecx+4] (the map count)
        auto* vmVt = *reinterpret_cast<lm_address_t**>(vm);
        const auto* isValid = reinterpret_cast<const uint8_t*>(vmVt[kVmIsValid]);
        if (!(action[0x25] == 0x8B && action[0x26] == 0x0D && action[0x2B] == 0xE8 &&
              memcmp(action + 0x30, kStore, sizeof(kStore)) == 0) ||
            !(isValid[0] == 0x8B && isValid[1] == 0x41 && isValid[2] == 0x04)) {
            LOGF("[upscale] graphics settings code does not look as expected - no menu option");
            g_menuDone = true;
            return;
        }
        g_settingsGlobal = *reinterpret_cast<void** const*>(action + 0x27);
        g_getSettings = reinterpret_cast<GetSettingsFn>(action + 0x30 + *reinterpret_cast<const int32_t*>(action + 0x2C));

        memcpy(g_menuVt, ssaa->vtable, sizeof(g_menuVt));
        g_menuVt[kSlotConstruct] = reinterpret_cast<void*>(&MenuConstruct);
        g_menuVt[kSlotPrepare] = reinterpret_cast<void*>(&MenuPrepare);
        g_menuVt[kSlotSave] = reinterpret_cast<void*>(&MenuSave);
        g_menuVt[kSlotAction] = reinterpret_cast<void*>(&MenuAction);
        g_menuMethod.vtable = g_menuQuality.vtable = g_menuVt;
        MenuConstruct(&g_menuMethod, nullptr, ssaa->owner);
        MenuConstruct(&g_menuQuality, nullptr, ssaa->owner);

        if (!LM_VmtNew(vmVt, &g_vmVmt) ||
            !LM_VmtHook(&g_vmVmt, kVmIsValid, reinterpret_cast<lm_address_t>(&VmIsValid)) ||
            !LM_VmtHook(&g_vmVmt, kVmGetConst, reinterpret_cast<lm_address_t>(&VmGetConst)) ||
            !LM_VmtHook(&g_vmVmt, kVmGet, reinterpret_cast<lm_address_t>(&VmGet))) {
            LOGF("[upscale] could not hook the graphics settings - no menu option");
            g_menuDone = true;
            return;
        }
        // value first, then key, then the count that makes them visible
        *reinterpret_cast<uint32_t*>(vm + 0x40 + 13 * 4) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_menuMethod));
        *reinterpret_cast<uint32_t*>(vm + 8 + 13 * 4) = kMethodIndex;
        MemoryBarrier();
        *reinterpret_cast<uint32_t*>(vm + 4) = 14;
        g_menuDone = true;
        LOGF("[upscale] graphics settings %d (method %d) and %d (quality %d) added", kMethodIndex, g_menuMethod.value,
             kQualityIndex, g_menuQuality.value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_menuDone = true;
        LOGF("[upscale] graphics settings not where expected - no menu option");
    }
}

// below display resolution, textures need a negative mip bias or they are sampled from mips sized
// for the render resolution and look blurry: log2(render / display) - 1, as the dlss guide says
std::unordered_map<ID3D11SamplerState*, ID3D11SamplerState*> g_biased;
float g_biasFor = 0.0f;

ID3D11SamplerState* Biased(ID3D11DeviceContext* ctx, ID3D11SamplerState* s, float bias)
{
    auto it = g_biased.find(s);
    if (it != g_biased.end())
        return it->second;
    D3D11_SAMPLER_DESC d;
    s->GetDesc(&d);
    ID3D11SamplerState* b = nullptr;
    if (d.MaxLOD > 0.0f) {
        d.MipLODBias = std::fmax(-16.0f, std::fmin(15.99f, d.MipLODBias + bias));
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        dev->CreateSamplerState(&d, &b);
        dev->Release();
    }
    if (!b) {
        b = s;
        b->AddRef();
    }
    s->AddRef(); // keeps the address from being reused while cached
    g_biased.emplace(s, b);
    return b;
}

void STDMETHODCALLTYPE HookedPSSetSamplers(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11SamplerState* const* samplers)
{
    const auto orig = Original<PSSetSamplersFn>(ctx, kPSSetSamplers);
    const uint32_t display = renderscale::DisplayWidth();
    if (!Active() || !samplers || n == 0 || n > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || !display || !InScene())
        return orig(ctx, start, n, samplers);
    const float bias = std::log2(static_cast<float>(g_renderW) / display) - 1.0f;
    if (bias != g_biasFor) {
        for (auto& e : g_biased) {
            e.first->Release();
            e.second->Release();
        }
        g_biased.clear();
        g_biasFor = bias;
        LOGF("[upscale] texture mip bias %.2f", bias);
    }
    ID3D11SamplerState* out[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    for (UINT i = 0; i < n; ++i)
        out[i] = samplers[i] ? Biased(ctx, samplers[i], bias) : nullptr;
    orig(ctx, start, n, out);
}

void HookContext(ID3D11DeviceContext* ctx)
{
    // vtables live on d3d11's heap: a new context can reuse a dead one's address
    auto* vt = *reinterpret_cast<lm_address_t**>(ctx);
    if (vt[kOMSetRenderTargets] == reinterpret_cast<lm_address_t>(&HookedOMSetRenderTargets))
        return;
    int slot = g_ctxCount;
    for (int k = 0; k < g_ctxCount; ++k)
        if (g_ctxVmts[k].vtable == vt)
            slot = k;
    if (slot == 4)
        return;
    ContextVmt& c = g_ctxVmts[slot];
    c.vtable = vt;
    // d3d11 swaps its own Draw / UpdateSubresource entries as state changes, so only this one
    // stays put; UpdateSubresource is detoured inline (HookVariants)
    if (!LM_VmtNew(vt, &c.vmt) ||
        !LM_VmtHook(&c.vmt, kOMSetRenderTargets, reinterpret_cast<lm_address_t>(&HookedOMSetRenderTargets)) ||
        !LM_VmtHook(&c.vmt, kPSSetSamplers, reinterpret_cast<lm_address_t>(&HookedPSSetSamplers))) {
        LOGF("[upscale] could not hook the device context");
        return;
    }
    if (slot == g_ctxCount)
        ++g_ctxCount;
}

// ------------------------------------------------------------- device creation

using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
                                        UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
iat::Hook g_create;

// throwaway devices the game makes first; their contexts are hooked too, as they share code
HRESULT WINAPI HookedCreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                  const D3D_FEATURE_LEVEL* levels, UINT count, UINT sdk, ID3D11Device** dev,
                                  D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx)
{
    const auto orig = reinterpret_cast<CreateDeviceFn>(g_create.original);
    if (!dev || (type != D3D_DRIVER_TYPE_HARDWARE && type != D3D_DRIVER_TYPE_UNKNOWN))
        return orig(adapter, type, sw, flags, levels, count, sdk, dev, got, ctx);
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL asked = count && levels ? levels[0] : D3D_FEATURE_LEVEL_11_0;
    ID3D11DeviceContext* c = nullptr;
    HRESULT hr = orig(adapter, type, sw, flags, &want, 1, sdk, dev, got, &c);
    if (FAILED(hr))
        return orig(adapter, type, sw, flags, levels, count, sdk, dev, got, ctx);
    if (got && asked < want)
        *got = asked; // report what the game asked for
    HookContext(c);
    if (ctx)
        *ctx = c;
    else
        c->Release();
    return hr;
}

using CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                                                    UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
                                                    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
iat::Hook g_createSc;

// the game's real device: asks for 10_0 only, the upscaler's compute pass needs 11_0
HRESULT WINAPI HookedCreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                              const D3D_FEATURE_LEVEL* levels, UINT count, UINT sdk,
                                              const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** sc,
                                              ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx)
{
    const auto orig = reinterpret_cast<CreateDeviceAndSwapChainFn>(g_createSc.original);
    ID3D11Device* d = nullptr;
    ID3D11DeviceContext* c = nullptr;
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL asked = count && levels ? levels[0] : D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = orig(adapter, type, sw, flags, &want, 1, sdk, desc, sc, &d, got, &c);
    if (FAILED(hr)) {
        LOGF("[upscale] feature level 11_0 refused (%08X), keeping the game's", static_cast<unsigned>(hr));
        hr = orig(adapter, type, sw, flags, levels, count, sdk, desc, sc, &d, got, &c);
        if (FAILED(hr))
            return hr;
    } else if (got && asked < want) {
        *got = asked;
    }
    if (c)
        HookContext(c);
    if (sc && *sc)
        g_swapchain = *sc;
    if (dev)
        *dev = d;
    else if (d)
        d->Release();
    if (ctx)
        *ctx = c;
    else if (c)
        c->Release();
    return hr;
}

DWORD WINAPI Installer(LPVOID)
{
    // the ea wrapper decrypts the import tables after we load: retry until both are found
    bool sc = false, dev = false;
    for (int i = 0; i < 600 && !(sc && dev); ++i) {
        if (!sc)
            sc = iat::HookImport("d3d11.dll", "D3D11CreateDeviceAndSwapChain", 0,
                                 reinterpret_cast<uintptr_t>(&HookedCreateDeviceAndSwapChain), 30000, g_createSc);
        if (!dev)
            dev = iat::HookImport("d3d11.dll", "D3D11CreateDevice", 0, reinterpret_cast<uintptr_t>(&HookedCreateDevice),
                                  30000, g_create);
        if (!(sc && dev))
            Sleep(50);
    }
    if (!sc)
        LOGF("[upscale] D3D11CreateDeviceAndSwapChain import not found - upscaling off");
    return 0;
}

} // namespace

void Init(HMODULE self, int backend, int quality, float sharpness, bool log)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\'))
        *slash = 0;
    g_helperDir = std::wstring(path) + L"\\NFS13Upscaler";
    // without the helper the menu entry still exists (a menu mod may show it), but stays off
    if (GetFileAttributesW((g_helperDir + L"\\NFS13Upscaler.exe").c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOGF("[upscale] NFS13Upscaler\\NFS13Upscaler.exe not found - upscaling off");
        g_noHelper = true;
        if (HANDLE t = CreateThread(nullptr, 0, Installer, nullptr, 0, nullptr))
            CloseHandle(t);
        return;
    }
    quality = quality < 0 ? 0 : quality > 4 ? 4 : quality;
    g_quality = quality;
    g_sel = ToSel(backend == 1 || backend == 2 ? backend : 0, quality);
    g_cfg.backend = backend == 2 ? upscale::Backend::FSR : upscale::Backend::DLSS;
    g_cfg.quality = static_cast<uint32_t>(quality);
    g_cfg.sharpness = sharpness;
    g_cfg.jitterSignX = 1.0f;
    g_cfg.jitterSignY = 1.0f; // the helper turns y around for dlss
    g_cfg.log = log;

    // render size follows the upscaler's ratio for the current choice
    renderscale::Init([](uint32_t dw, uint32_t dh, uint32_t& rw, uint32_t& rh) {
        if (Active())
            upscale::RenderSize(static_cast<uint32_t>(SelQuality(g_sel)), dw, dh, rw, rh);
        else
            rw = dw, rh = dh;
        g_renderW = rw;
        g_renderH = rh;
        InterlockedExchange(&g_rebuilt, 1);
    });
    if (HANDLE t = CreateThread(nullptr, 0, Installer, nullptr, 0, nullptr))
        CloseHandle(t);
}

} // namespace upscaling
