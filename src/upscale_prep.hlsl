// copies the game's colour and depth into the upscaler's shared inputs and derives motion
// vectors (current -> previous, in uv units) from depth and the camera reprojection
Texture2D<float4> Color : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<uint2> Stencil : register(t2);
Texture2D<float4> Particles : register(t3);
RWTexture2D<float4> OutColor : register(u0);
RWTexture2D<float> OutDepth : register(u1);
RWTexture2D<float2> OutMotion : register(u2);
RWTexture2D<float> OutReactive : register(u3);

cbuffer Params : register(b0)
{
    row_major float4x4 Reproject; // unjittered clip space, current -> previous
    float2 RenderSize;
    float2 JitterNdc;             // jitter applied to this frame's projection, in ndc
    uint HasStencil;
    uint HasParticles;
    uint2 Pad;
    row_major float4x4 Objects[128]; // per stencil value - 128: Reproject, or a moving object's own
};

float2 PrevUv(float4x4 m, float2 ndc, float z)
{
    const float4 prev = mul(m, float4(ndc, z, 1.0));
    const float2 prevNdc = prev.xy / prev.w;
    return float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
}

[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= (uint)RenderSize.x || p.y >= (uint)RenderSize.y)
        return;
    const uint s = HasStencil ? Stencil[p].g : 0;
    float4 c = Color[p];
    // smoke and sparks: the game draws them into their own buffer, alpha = coverage
    const float cover = HasParticles ? saturate(Particles[p].a) : 0.0;
    OutReactive[p] = min(saturate((cover - 0.05) * 1.25), 0.9); // faint haze stays out
    // they hang in the world, so where they cover a car they do not move with it
    const uint id = s >= 128 && cover < 0.25 ? s - 128 : 0;
    OutColor[p] = c;
    const float z = Depth[p];
    OutDepth[p] = z;

    const float2 uv = (float2(p) + 0.5) / RenderSize;
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) - JitterNdc;
    const float2 curUv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    OutMotion[p] = PrevUv(Objects[id], ndc, z) - curUv;
}
