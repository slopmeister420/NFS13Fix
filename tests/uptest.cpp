// offline check of the upscaler path: renders a panning high-frequency pattern at render
// resolution with jitter, upscales it through the helper and scores it against a supersampled
// reference. uptest <helperDir> <dlss|fsr> <quality> [frames]
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "upscale.h"

using Microsoft::WRL::ComPtr;

namespace {

const char kShader[] = R"(
cbuffer P : register(b0) { float2 Cam; float2 WorldPerPixel; float2 Jitter; float Samples; float pad; };
float3 Pattern(float2 w)
{
    float3 c = float3(0.2, 0.25, 0.3);
    if ((int(floor(w.x * 0.25)) + int(floor(w.y * 0.25))) & 1) c = float3(0.85, 0.8, 0.7);
    float r = length(w - float2(300, 200));
    if (sin(r * 0.9) > 0.6) c = float3(0.9, 0.2, 0.15);
    if (abs(frac(w.x * 0.093) - 0.5) < 0.03) c = float3(0.05, 0.05, 0.05);
    if (abs(frac((w.x + w.y) * 0.061) - 0.5) < 0.02) c = float3(1, 1, 1);
    return c;
}
float4 VS(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 pos : SV_Position) : SV_Target
{
    int n = int(Samples);
    float3 acc = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            float2 sub = (float2(x, y) + 0.5) / n - 0.5;
            float2 p = floor(pos.xy) + 0.5 + sub - Jitter; // jitter shifts the image, so sample at -jitter
            acc += Pattern(Cam + p * WorldPerPixel);
        }
    return float4(acc / (n * n), 1);
}
Texture2D T : register(t0); SamplerState S : register(s0);
float4 PSBlit(float4 pos : SV_Position) : SV_Target
{
    float2 dim; T.GetDimensions(dim.x, dim.y);
    return T.SampleLevel(S, pos.xy / float2(1920, 1080), 0);
}
)";

struct alignas(16) Params {
    float cam[2], worldPerPixel[2], jitter[2], samples, pad;
};

ComPtr<ID3D11Device> dev;
ComPtr<ID3D11DeviceContext> ctx;

ComPtr<ID3DBlob> Compile(const char* entry, const char* target)
{
    ComPtr<ID3DBlob> b, e;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr, entry, target, 0, 0, &b, &e))) {
        printf("shader: %s\n", e ? static_cast<const char*>(e->GetBufferPointer()) : "?");
        exit(1);
    }
    return b;
}

struct Target {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
};

Target MakeTarget(uint32_t w, uint32_t h, DXGI_FORMAT f)
{
    Target t;
    D3D11_TEXTURE2D_DESC d{w, h, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT,
                           D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
    dev->CreateTexture2D(&d, nullptr, &t.tex);
    dev->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv);
    dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
    return t;
}

std::vector<uint8_t> Read(ID3D11Texture2D* tex, uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> s;
    dev->CreateTexture2D(&d, nullptr, &s);
    ctx->CopyResource(s.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m;
    std::vector<uint8_t> out(w * h * 4);
    if (SUCCEEDED(ctx->Map(s.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        for (uint32_t y = 0; y < h; ++y)
            memcpy(&out[y * w * 4], static_cast<uint8_t*>(m.pData) + y * m.RowPitch, w * 4);
        ctx->Unmap(s.Get(), 0);
    }
    return out;
}

void SaveBmp(const char* path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    BITMAPFILEHEADER fh{0x4D42, static_cast<DWORD>(54 + w * h * 4), 0, 0, 54};
    BITMAPINFOHEADER ih{sizeof(ih), static_cast<LONG>(w), -static_cast<LONG>(h), 1, 32};
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        uint8_t bgra[4] = {rgba[i + 2], rgba[i + 1], rgba[i], 255};
        fwrite(bgra, 4, 1, f);
    }
    fclose(f);
}

double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    double se = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        for (int c = 0; c < 3; ++c, ++n) {
            const double d = double(a[i + c]) - b[i + c];
            se += d * d;
        }
    return 10.0 * log10(255.0 * 255.0 / (se / n + 1e-9));
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        printf("uptest <helperDir> <dlss|fsr> <quality 0-4> [frames]\n");
        return 2;
    }
    std::wstring helperDir(argv[1], argv[1] + strlen(argv[1]));
    const bool fsr = !strcmp(argv[2], "fsr");
    const uint32_t quality = atoi(argv[3]);
    const int frames = argc > 4 ? atoi(argv[4]) : 120;
    // sign the sdk sees for the jitter, to check conventions
    const float signX = argc > 5 ? float(atof(argv[5])) : 1.0f, signY = argc > 6 ? float(atof(argv[6])) : 1.0f;
    const uint32_t W = 1920, H = 1080;

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx))) {
        printf("no d3d11 device\n");
        return 1;
    }
    upscale::Config cfg;
    cfg.backend = fsr ? upscale::Backend::FSR : upscale::Backend::DLSS;
    cfg.quality = quality;
    cfg.helperDir = helperDir.c_str();
    cfg.jitterSignX = signX;
    cfg.jitterSignY = signY;
    if (!upscale::Init(dev.Get(), cfg)) {
        printf("init: %s\n", upscale::Status());
        return 1;
    }
    uint32_t rw, rh;
    upscale::RenderSize(W, H, rw, rh);
    printf("display %ux%u render %ux%u\n", W, H, rw, rh);

    auto vsb = Compile("VS", "vs_5_0"), psb = Compile("PS", "ps_5_0"), blb = Compile("PSBlit", "ps_5_0");
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps, blit;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    dev->CreatePixelShader(blb->GetBufferPointer(), blb->GetBufferSize(), nullptr, &blit);
    D3D11_BUFFER_DESC bd{sizeof(Params), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    ComPtr<ID3D11Buffer> cb;
    dev->CreateBuffer(&bd, nullptr, &cb);
    D3D11_SAMPLER_DESC sd{D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                          D3D11_TEXTURE_ADDRESS_CLAMP};
    ComPtr<ID3D11SamplerState> smp;
    dev->CreateSamplerState(&sd, &smp);

    Target scene = MakeTarget(rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM);
    Target ref = MakeTarget(W, H, DXGI_FORMAT_R8G8B8A8_UNORM);
    Target bilinear = MakeTarget(W, H, DXGI_FORMAT_R8G8B8A8_UNORM);
    // flat depth: an orthographic pan moves every pixel the same way
    std::vector<float> depthData(rw * rh, 0.5f);
    D3D11_TEXTURE2D_DESC dd{rw, rh, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    D3D11_SUBRESOURCE_DATA init{depthData.data(), rw * 4};
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11ShaderResourceView> depthSrv;
    dev->CreateTexture2D(&dd, &init, &depth);
    dev->CreateShaderResourceView(depth.Get(), nullptr, &depthSrv);

    const float viewW = 960.0f, viewH = viewW * H / W; // world units across the screen
    const float speedX = 1.37f, speedY = -0.61f;       // world units per frame
    auto draw = [&](Target& t, uint32_t w, uint32_t h, float camX, float camY, float jx, float jy, float samples) {
        Params p{{camX, camY}, {viewW / w, viewH / h}, {jx, jy}, samples};
        ctx->UpdateSubresource(cb.Get(), 0, nullptr, &p, 0, 0);
        D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, t.rtv.GetAddressOf(), nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->Draw(3, 0);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
    };

    // wait for the helper
    for (int i = 0; i < 300 && !upscale::Ready(W, H, DXGI_FORMAT_R8G8B8A8_UNORM); ++i)
        Sleep(50);
    if (!upscale::Ready(W, H, DXGI_FORMAT_R8G8B8A8_UNORM)) {
        printf("helper not ready: %s\n", upscale::Status());
        upscale::Shutdown();
        return 1;
    }

    float camX = 0, camY = 0;
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < frames; ++i) {
        float jx, jy;
        upscale::Jitter(i, rw, W, jx, jy);
        draw(scene, rw, rh, camX, camY, jx, jy, 1);
        upscale::Frame f{};
        f.color = scene.srv.Get();
        f.depth = depthSrv.Get();
        // current -> previous clip: the camera moved by (speedX, speedY) since last frame
        const float tx = i ? 2.0f * speedX / viewW : 0, ty = i ? -2.0f * speedY / viewH : 0;
        const float m[16] = {1, 0, 0, tx, 0, 1, 0, ty, 0, 0, 1, 0, 0, 0, 0, 1};
        memcpy(f.reproject, m, sizeof(m));
        f.jitterX = jx;
        f.jitterY = jy;
        f.reset = i == 0;
        f.frameTimeMs = 16.6f;
        f.cameraNear = 0.1f;
        f.cameraFar = 1000.0f;
        f.fovY = 1.0f;
        if (!upscale::Run(ctx.Get(), f, W, H, DXGI_FORMAT_R8G8B8A8_UNORM)) {
            printf("run failed at frame %d: %s\n", i, upscale::Status());
            break;
        }
        if (i + 1 < frames) {
            camX += speedX;
            camY += speedY;
        }
    }
    auto out = Read(upscale::Output(), W, H);
    QueryPerformanceCounter(&t1);
    printf("%d frames in %.1f ms (%.2f ms/frame incl. scene)\n", frames,
           1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart,
           1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart / frames);

    draw(ref, W, H, camX, camY, 0, 0, 4);
    draw(scene, rw, rh, camX, camY, 0, 0, 1);
    D3D11_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, bilinear.rtv.GetAddressOf(), nullptr);
    ctx->PSSetShader(blit.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, scene.srv.GetAddressOf());
    ctx->PSSetSamplers(0, 1, smp.GetAddressOf());
    ctx->Draw(3, 0);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);

    auto refPx = Read(ref.tex.Get(), W, H), bilPx = Read(bilinear.tex.Get(), W, H);
    printf("%s: upscaled psnr %.2f dB, bilinear %.2f dB\n", argv[2], Psnr(out, refPx), Psnr(bilPx, refPx));
    printf("status: %s\n", upscale::Status());
    std::string tag = std::string(argv[2]) + "_q" + argv[3];
    SaveBmp((tag + "_out.bmp").c_str(), out, W, H);
    SaveBmp((tag + "_ref.bmp").c_str(), refPx, W, H);
    SaveBmp((tag + "_bilinear.bmp").c_str(), bilPx, W, H);
    upscale::Shutdown();
    return 0;
}
