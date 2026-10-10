// King Kong native renderer: what the backend uploads and binds for a translated shader.
//
// The shader side is hlsl/kk_common.hlsli; keep the two in step. Every translated shader uses
// the same layout, so one root signature / pipeline layout serves them all:
//
//   b0 space0  VertexConstants   4096 bytes: the device's vertex float constants (+1920), as is
//   b1 space0  PixelConstants    4096 bytes: the device's pixel float constants (+6016), as is
//   b2 space0  DrawConstants     below
//   t0 space1  Texture2D[]       bindless; 1D textures are bound as Nx1 2D textures
//   t0 space2  Texture3D[]
//   t0 space3  TextureCube[]
//   t0 space4  Texture2DArray[]  stacked 3D textures
//   s0 space5  SamplerState[]
//   t0 space6  ByteAddressBuffer[] vertex data as it is in guest memory (big-endian)
//
// In SPIR-V the register is the binding and the space the descriptor set (DXC's default
// mapping); constant buffers use the D3D layout (-fvk-use-dx-layout) so these structs serve both.
// Vertex shaders are compiled with -fvk-invert-y for Vulkan.
//
// The float constants are uploaded as the device holds them (little-endian floats, the hooks
// byte-swap). A shader's literal constants (ShaderInfo::registerWrites) must be applied to the
// shadow when the shader is set, as the XDK's SetVertexShader / SetPixelShader do. The
// translated code does not depend on it for float literals (read directly or relatively, they
// are answered in the shader) nor for loop literals (inlined); bool literals go through the
// buffers (no shader of the game's database reads a bool constant). Applying them keeps the
// shadow identical to the console's for anything else that reads it.
#pragma once

#include <cstdint>

namespace kkshaders {

// Bumped whenever the binding model, the constant layouts or the prelude change incompatibly.
constexpr uint32_t kAbiVersion = 1;

struct alignas(16) DrawConstants {
    uint32_t boolConstants[8];      // 256 bool constants, bit (n & 31) of word n >> 5 (vertex 0-127, pixel 128-255)
    uint32_t loopConstants[32];     // bits 0-7 count, 8-15 start, 16-23 step (signed); as at device +10144
    uint32_t textureIndex[32];      // per texture fetch constant: descriptor index; bit 31 = stacked 3D (2D array)
    uint32_t vertexSamplers[32];    // descriptor index per sampler binding of the vertex shader
    uint32_t pixelSamplers[32];     // the same for the pixel shader
    uint32_t vertexFetch[16][4];    // per vertex binding, see VertexBinding
    float clipPlanes[6][4];         // user clip planes in clip space
    float positionOffset[4];        // xy: added to the position's xy times w (half-pixel offset)
    uint32_t clipPlaneMask;         // planes enabled (bit per plane)
    uint32_t alphaFunc;             // 0 = no alpha test, else D3DCMPFUNC (1 never ... 8 always)
    float alphaRef;                 // 0-1
    uint32_t reserved;
};
static_assert(sizeof(DrawConstants) == 58 * 16, "DrawConstants must match KKDrawConstants");

// A vertex fetch format word (the w of a binding mode entry; built by the translator for
// instruction mode).
namespace vertex_format {
constexpr uint32_t kFormatMask = 0x3F;          // xenos::VertexFormat (0 = element missing: reads 0,0,0,1)
constexpr uint32_t kSigned = 1u << 6;
constexpr uint32_t kInteger = 1u << 7;          // not normalised
constexpr uint32_t kSignedNoZero = 1u << 8;     // OpenGL-style signed normalisation
constexpr uint32_t kEndianShift = 9;            // 0 none, 1 8in16, 2 8in32, 3 16in32
constexpr uint32_t kExpAdjustShift = 11;        // 6-bit signed power-of-two scale
constexpr uint32_t kSwizzleShift = 17;          // 3 bits per component: 0-3 XYZW, 4 = 0, 5 = 1
constexpr uint32_t kIdentitySwizzle = (0u | (1u << 3) | (2u << 6) | (3u << 9)) << kSwizzleShift;
}  // namespace vertex_format

// DrawConstants::vertexFetch[binding]:
//  - binding mode (the shader came with a container: VertexBinding::raw = false): the
//    declaration element with VertexBinding::usage / usageIndex, found in the vertex declaration
//    set at draw time: {descriptor index of the buffer, byte offset of the element for vertex 0
//    (stream offset + element offset), byte stride of the stream, format word with the element's
//    format, flags, endian (2 = 8in32 for normal guest buffers) and a swizzle that fills missing
//    components with 0 / 1 (D3DDECLTYPE FLOAT2 = x y 0 1, D3DCOLOR = z y x w)}. A usage the
//    declaration lacks: format 0.
//  - instruction mode (raw microcode with patched vfetch instructions: raw = true): the vertex
//    fetch constant VertexBinding::fetchConstant: {descriptor index, byte offset of the fetch
//    constant's address in that buffer, byte size of the buffer range, endian}.
// The vertex index is SV_VertexID (it includes the base vertex on both APIs).

}  // namespace kkshaders
