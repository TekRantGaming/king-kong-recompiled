// King Kong native renderer: the prelude every translated shader starts with.
//
// This file is the shader side of the binding and constant model (the C++ side is
// include/kkshaders/abi.h; keep the two in step and bump kAbiVersion there when either changes).
// Compiled with DXC, HLSL 2021, to DXIL (vs_6_0 / ps_6_0) and to SPIR-V (Vulkan 1.2, DX layout).
//
// Bindings (DXIL register / space = Vulkan binding / descriptor set):
//   b0 space0  KKVertexConstants  the vertex shader float constants c0-c255 (device +1920, 4096 bytes)
//   b1 space0  KKPixelConstants   the pixel shader float constants c0-c255 (device +6016, 4096 bytes)
//   b2 space0  KKDrawConstants    bool and loop constants, descriptor indices, vertex fetch table, misc
//   t0 space1  Texture2D[]        bindless (1D textures are bound as Nx1 2D textures)
//   t0 space2  Texture3D[]
//   t0 space3  TextureCube[]
//   t0 space4  Texture2DArray[]   stacked 3D textures (bit 31 of the texture index selects this array)
//   s0 space5  SamplerState[]
//   t0 space6  ByteAddressBuffer[] vertex data in guest (big-endian) layout
#ifndef KK_COMMON_HLSLI
#define KK_COMMON_HLSLI

#define KK_FLT_MAX asfloat(0x7F7FFFFFu)

cbuffer KKVertexConstants : register(b0, space0)
{
    float4 kk_VC[256];
};

cbuffer KKPixelConstants : register(b1, space0)
{
    float4 kk_PC[256];
};

cbuffer KKDrawConstants : register(b2, space0)
{
    uint4 kk_Bool[2];            // 256 bool constants as on the GPU: bit (n & 31) of dword n >> 5
    uint4 kk_Loop[8];            // 32 loop constants: bits 0-7 count, 8-15 start, 16-23 step (signed)
    uint4 kk_TextureIndex[8];    // per texture fetch constant 0-31: descriptor index (bit 31: stacked 3D)
    uint4 kk_VSSampler[8];       // vertex shader sampler bindings (ShaderBindings::samplers order)
    uint4 kk_PSSampler[8];       // pixel shader sampler bindings
    uint4 kk_VertexFetch[16];    // per vertex binding: see abi.h (VertexBinding)
    float4 kk_ClipPlane[6];      // user clip planes in clip space
    float4 kk_PosOffset;         // xy: added to the vertex position xy times w (half-pixel offset)
    uint4 kk_Flags;              // x: clip plane enable mask, y: alpha test function (0 off, 1-8 D3DCMP),
                                 // z: asuint(alpha reference 0-1), w: reserved
};

Texture2D<float4> kk_Tex2D[] : register(t0, space1);
Texture3D<float4> kk_Tex3D[] : register(t0, space2);
TextureCube<float4> kk_TexCube[] : register(t0, space3);
Texture2DArray<float4> kk_Tex2DArray[] : register(t0, space4);
SamplerState kk_Sampler[] : register(s0, space5);
ByteAddressBuffer kk_Buffer[] : register(t0, space6);

// ---------------------------------------------------------------------------------------------
// Constants.

float4 kk_VSConst(int index)
{
    return select(uint(index) < 256u, kk_VC[min(uint(index), 255u)], float4(0.0, 0.0, 0.0, 0.0));
}

float4 kk_PSConst(int index)
{
    return select(uint(index) < 256u, kk_PC[min(uint(index), 255u)], float4(0.0, 0.0, 0.0, 0.0));
}

bool kk_BoolConst(uint index)
{
    return (kk_Bool[index >> 7][(index >> 5) & 3] & (1u << (index & 31))) != 0;
}

uint kk_LoopConst(uint index)
{
    return kk_Loop[index >> 2][index & 3];
}

uint kk_TexIndex(uint slot)
{
    return kk_TextureIndex[slot >> 2][slot & 3];
}

SamplerState kk_VSSamplerState(uint binding)
{
    return kk_Sampler[kk_VSSampler[binding >> 2][binding & 3]];
}

SamplerState kk_PSSamplerState(uint binding)
{
    return kk_Sampler[kk_PSSampler[binding >> 2][binding & 3]];
}

// ---------------------------------------------------------------------------------------------
// ALU helpers with the Xenos (Direct3D 9) rules: 0 * anything = +0, max / min as comparisons.

float4 kk_Mul(float4 a, float4 b)
{
    return select(or(a == 0.0, b == 0.0), float4(0.0, 0.0, 0.0, 0.0), a * b);
}

float kk_Muls(float a, float b)
{
    return select(or(a == 0.0, b == 0.0), 0.0, a * b);
}

// Scalar operations with the clamping variants of rcp / rsq / log.
float kk_ClampInf(float v, float replacement)
{
    return select(isinf(v), select(v < 0.0, -replacement, replacement), v);
}

float kk_RcpC(float a) { return kk_ClampInf(1.0 / a, KK_FLT_MAX); }
float kk_RcpF(float a) { float r = 1.0 / a; return select(isinf(r), select(r < 0.0, -0.0, 0.0), r); }
float kk_RsqC(float a) { return kk_ClampInf(rsqrt(a), KK_FLT_MAX); }
float kk_RsqF(float a) { float r = rsqrt(a); return select(isinf(r), select(r < 0.0, -0.0, 0.0), r); }
float kk_LogC(float a) { float r = log2(a); return select(and(isinf(r), r < 0.0), -KK_FLT_MAX, r); }

float kk_MulsPrev2(float a, float b, float previous)
{
    bool invalid = or(or(previous == -KK_FLT_MAX, !isfinite(previous)), or(!isfinite(b), b <= 0.0));
    return select(invalid, -KK_FLT_MAX, kk_Muls(a, previous));
}

float4 kk_Max(float4 a, float4 b)
{
    return select(a >= b, a, b);
}

float4 kk_Min(float4 a, float4 b)
{
    return select(a < b, a, b);
}

float kk_Dot4(float4 a, float4 b)
{
    float4 p = kk_Mul(a, b);
    return p.x + p.y + p.z + p.w;
}

float kk_Dot3(float4 a, float4 b)
{
    float4 p = kk_Mul(a, b);
    return p.x + p.y + p.z;
}

float kk_Dot2Add(float4 a, float4 b, float4 c)
{
    float4 p = kk_Mul(a, b);
    return p.x + p.y + c.x;
}

float kk_Max4(float4 a)
{
    float zw = select(a.z >= a.w, a.z, a.w);
    float yzw = select(and(a.y >= a.z, a.y >= a.w), a.y, zw);
    return select(and(and(a.x >= a.y, a.x >= a.z), a.x >= a.w), a.x, yzw);
}

float4 kk_Dst(float4 a, float4 b)
{
    return float4(1.0, kk_Muls(a.y, b.y), a.z, b.w);
}

// cube: operand 0 is .z_xy of the direction. Result: T coordinate, S coordinate, 2 * major axis,
// face index (+X, -X, +Y, -Y, +Z, -Z).
float4 kk_Cube(float4 src0)
{
    float x = src0.z, y = src0.w, z = src0.x;
    float4 r;
    if (abs(z) >= abs(x) && abs(z) >= abs(y))
        r = float4(-y, z < 0.0 ? -x : x, z, z < 0.0 ? 5.0 : 4.0);
    else if (abs(y) >= abs(x))
        r = float4(y < 0.0 ? -z : z, x, y, y < 0.0 ? 3.0 : 2.0);
    else
        r = float4(-y, x < 0.0 ? z : -z, x, x < 0.0 ? 1.0 : 0.0);
    r.z *= 2.0;
    return r;
}

// tfetchCube takes (S, T, face) with S and T in [1, 2] (what the compiler's cube sequence makes:
// coordinate / |2 * major axis| + 1.5). Turns them back into a direction for TextureCube.
float3 kk_CubeDirection(float3 c)
{
    float u = (c.x - 1.5) * 2.0;
    float v = (c.y - 1.5) * 2.0;
    uint face = uint(clamp(floor(c.z + 0.5), 0.0, 5.0));
    switch (face)
    {
    case 0: return float3(1.0, -v, -u);
    case 1: return float3(-1.0, -v, u);
    case 2: return float3(u, 1.0, v);
    case 3: return float3(u, -1.0, -v);
    case 4: return float3(u, -v, 1.0);
    default: return float3(-u, -v, -1.0);
    }
}

// ---------------------------------------------------------------------------------------------
// Vertex fetch from guest memory. A format word (abi.h VertexFormatWord):
//   bits 0-5 format (xenos VertexFormat), 6 signed, 7 integer (not normalized), 8 signed values
//   without zero (OpenGL style), 9-10 endian (0 none, 1 8in16, 2 8in32, 3 16in32), 11-16 exponent
//   adjust (signed), 17-28 element swizzle (3 bits per component: 0-3 XYZW, 4 zero, 5 one).

uint kk_Swap(uint v, uint endian)
{
    if (endian == 1u)
        return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    if (endian == 2u)
        return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
    if (endian == 3u)
        return (v << 16) | (v >> 16);
    return v;
}

uint kk_VertexFormatDwords(uint format)
{
    switch (format)
    {
    case 26u: case 32u: case 34u: case 37u: return 2u;
    case 57u: return 3u;
    case 35u: case 38u: return 4u;
    default: return 1u;
    }
}

float kk_UnpackPacked(uint v, uint offset, uint width, uint word)
{
    bool isSigned = (word & 0x40u) != 0;
    bool isInteger = (word & 0x80u) != 0;
    float s = float(int(v << (32u - width - offset)) >> int(32u - width));
    float sn = select((word & 0x100u) != 0, (s + 0.5) * 2.0 / float((1u << width) - 1u),
                      max(-1.0, s / float((1u << (width - 1u)) - 1u)));
    float u = float((v >> offset) & ((1u << width) - 1u));
    float un = u / float((1u << width) - 1u);
    return select(isSigned, select(isInteger, s, sn), select(isInteger, u, un));
}

float kk_Unpack32(uint v, uint word)
{
    bool isSigned = (word & 0x40u) != 0;
    bool isInteger = (word & 0x80u) != 0;
    if (isSigned)
    {
        float r = float(int(v));
        if (!isInteger)
            r = select((word & 0x100u) != 0, (r + 0.5) / 2147483647.5, r / 2147483647.0);
        return r;
    }
    float r = float(v);
    return select(isInteger, r, r / 4294967295.0);
}

// A vertex format as data: bits 0-23 the width of each component (6 bits each, packed from bit 0
// of the first dword on), 24-26 the component count, 27-28 the kind (0 packed integers, 1 16-bit
// floats, 2 32-bit floats). Table-driven so every vfetch site expands to one small loop instead
// of a switch over every format (this kept the SPIR-V of a five-input shader from 190 KB down).
uint kk_VertexFormatDesc(uint format)
{
    switch (format)
    {
    case 6u: return 8u | (8u << 6) | (8u << 12) | (8u << 18) | (4u << 24);            // 8_8_8_8
    case 7u: return 10u | (10u << 6) | (10u << 12) | (2u << 18) | (4u << 24);         // 2_10_10_10
    case 16u: return 11u | (11u << 6) | (10u << 12) | (3u << 24);                     // 10_11_11
    case 17u: return 10u | (11u << 6) | (11u << 12) | (3u << 24);                     // 11_11_10
    case 25u: return 16u | (16u << 6) | (2u << 24);                                   // 16_16
    case 26u: return 16u | (16u << 6) | (16u << 12) | (16u << 18) | (4u << 24);       // 16_16_16_16
    case 31u: return 16u | (16u << 6) | (2u << 24) | (1u << 27);                      // 16_16_FLOAT
    case 32u: return 16u | (16u << 6) | (16u << 12) | (16u << 18) | (4u << 24) | (1u << 27);
    case 33u: return 32u | (1u << 24);                                                // 32
    case 34u: return 32u | (32u << 6) | (2u << 24);                                   // 32_32
    case 35u: return 32u | (32u << 6) | (32u << 12) | (32u << 18) | (4u << 24);       // 32_32_32_32
    case 36u: return 32u | (1u << 24) | (2u << 27);                                   // 32_FLOAT
    case 37u: return 32u | (32u << 6) | (2u << 24) | (2u << 27);                      // 32_32_FLOAT
    case 38u: return 32u | (32u << 6) | (32u << 12) | (32u << 18) | (4u << 24) | (2u << 27);
    case 57u: return 32u | (32u << 6) | (32u << 12) | (3u << 24) | (2u << 27);        // 32_32_32_FLOAT
    default: return 0u;  // undefined: the element is missing from the declaration
    }
}

float4 kk_DecodeVertex(uint4 d, uint word)
{
    uint desc = kk_VertexFormatDesc(word & 0x3Fu);
    uint count = (desc >> 24) & 7u;
    if (count == 0u)
        return float4(0.0, 0.0, 0.0, 1.0);
    uint kind = (desc >> 27) & 3u;
    float4 r = float4(0.0, 0.0, 0.0, 0.0);
    uint position = 0u;
    [loop] for (uint c = 0u; c < count; c++)
    {
        uint width = (desc >> (c * 6u)) & 63u;
        uint dword = position >> 5;
        uint shift = position & 31u;
        uint v = select(dword == 0u, d.x, select(dword == 1u, d.y, select(dword == 2u, d.z, d.w)));
        float f = select(kind == 2u, asfloat(v),
                         select(kind == 1u, f16tof32(v >> shift),
                                select(width == 32u, kk_Unpack32(v, word), kk_UnpackPacked(v, shift, min(width, 31u), word))));
        r[c] = f;
        position += width;
    }
    int expAdjust = int(word << 15) >> 26;
    if (expAdjust != 0)
        r *= exp2(float(expAdjust));
    return r;
}

float kk_SwizzleComponent(float4 v, uint s)
{
    return select(s < 4u, v[min(s, 3u)], select(s == 5u, 1.0, 0.0));
}

float4 kk_ApplyElementSwizzle(float4 v, uint word)
{
    uint s = word >> 17;
    return float4(kk_SwizzleComponent(v, s & 7u), kk_SwizzleComponent(v, (s >> 3) & 7u),
                  kk_SwizzleComponent(v, (s >> 6) & 7u), kk_SwizzleComponent(v, (s >> 9) & 7u));
}

float4 kk_LoadVertex(uint buffer, uint byteAddress, uint word)
{
    uint endian = (word >> 9) & 3u;
    uint dwords = kk_VertexFormatDwords(word & 0x3Fu);
    uint4 d = uint4(0u, 0u, 0u, 0u);
    d.x = kk_Swap(kk_Buffer[buffer].Load(byteAddress), endian);
    if (dwords > 1u)
        d.y = kk_Swap(kk_Buffer[buffer].Load(byteAddress + 4u), endian);
    if (dwords > 2u)
        d.z = kk_Swap(kk_Buffer[buffer].Load(byteAddress + 8u), endian);
    if (dwords > 3u)
        d.w = kk_Swap(kk_Buffer[buffer].Load(byteAddress + 12u), endian);
    return kk_ApplyElementSwizzle(kk_DecodeVertex(d, word), word);
}

// A vertex declaration element (binding mode): x buffer, y byte offset, z byte stride, w format word.
float4 kk_FetchElement(uint binding, uint index)
{
    uint4 e = kk_VertexFetch[binding];
    return kk_LoadVertex(e.x, e.y + index * e.z, e.w);
}

// A patched vfetch instruction (instruction mode): the table gives x buffer, y base byte offset,
// z byte size (fetches past it read 0), w endian; the format and layout come from the instruction.
float4 kk_FetchRaw(uint binding, uint index, uint strideBytes, int offsetBytes, uint word)
{
    uint4 e = kk_VertexFetch[binding];
    uint address = uint(int(index * strideBytes) + offsetBytes);
    if (address >= e.z)
        return kk_ApplyElementSwizzle(kk_DecodeVertex(uint4(0u, 0u, 0u, 0u), word), word);
    return kk_LoadVertex(e.x, e.y + address, word | ((e.w & 3u) << 9));
}

// ---------------------------------------------------------------------------------------------
// Fixed-function pieces the 360 did outside the shader.

#ifdef KK_PIXEL_SHADER
void kk_AlphaTest(float alpha)
{
    uint func = kk_Flags.y;
    if (func == 0u)
        return;
    float reference = asfloat(kk_Flags.z);
    bool pass;
    switch (func)
    {
    case 1u: pass = false; break;
    case 2u: pass = alpha < reference; break;
    case 3u: pass = alpha == reference; break;
    case 4u: pass = alpha <= reference; break;
    case 5u: pass = alpha > reference; break;
    case 6u: pass = alpha != reference; break;
    case 7u: pass = alpha >= reference; break;
    default: pass = true; break;
    }
    if (!pass)
        discard;
}
#endif

float kk_ClipDistance(float4 position, uint plane)
{
    return select((kk_Flags.x & (1u << plane)) != 0, dot(position, kk_ClipPlane[plane]), 1.0);
}

#endif
