// Parallax occlusion mapping on the game's own pixel shaders: see kkshaders/parallax.h.

#include "kkshaders/parallax.h"

#include "kkshaders/lighting.h"

namespace kkshaders {

namespace {

constexpr const char* kText = R"HLSL(
// Parallax occlusion mapping (kkshaders/parallax.h, docs/lighting.md). The surface's tangent frame comes from screen
// derivatives of its view position and texture coordinates, so no tangents are needed. The height is the base texture's
// brightness: dark texels lie deep. A march of at most 16 steps along the view finds where the view enters the surface.
static const float kp_Depth = 0.05;  // the deepest texel, in the texture's own units
static const float kp_Far = 24.0;     // metres: it fades out from half this, and is off beyond

float kp_Height(Texture2D<float4> tex, SamplerState s, float2 uv, float2 dx, float2 dy)
{
    return 1.0 - dot(tex.SampleGrad(s, uv, dx, dy).rgb, float3(0.3, 0.59, 0.11));
}

void kp_Parallax(float4 pos, inout float4 t0, inout float4 t1, bool normalUv)
{
    const float2 uv0 = t0.xy;
    const float2 dx0 = ddx(uv0), dy0 = ddy(uv0);
    const float2 dx1 = ddx(t1.xy), dy1 = ddy(t1.xy);
    const float2 puv = (pos.xy - kl_SsrRect.xy) / max(kl_SsrRect.zw, float2(1.0, 1.0));
    const float z = kl_SsrDepth.x / (kl_SsrDepth.y - pos.z);
    const float3 p = float3((puv * kl_SsrScreen.xy + kl_SsrScreen.zw) * z, z);
    const float3 dp1 = ddx(p), dp2 = ddy(p);
    if (kl_SsrRect.z <= 0.0 || !(z > 0.0) || z > kp_Far)
        return;
    float3 n = normalize(cross(dp1, dp2));
    const float3 v = normalize(-p);
    if (dot(n, v) < 0.0)
        n = -n;
    const float3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    float3 t = dp2perp * dx0.x + dp1perp * dy0.x;
    float3 b = dp2perp * dx0.y + dp1perp * dy0.y;
    const float lt = dot(t, t), lb = dot(b, b);
    if (!(lt > 0.0) || !(lb > 0.0))
        return;
    t *= rsqrt(lt);
    b *= rsqrt(lb);
    const float3 vt = float3(dot(v, t), dot(v, b), dot(v, n));
    if (vt.z < 0.05)
        return;
    const float fade = 1.0 - smoothstep(kp_Far * 0.5, kp_Far, z);
    const float layers = lerp(16.0, 6.0, saturate(vt.z));
    // Limited at grazing angles (vt.z + 0.4 in place of vt.z), where a full march smears the texture into streaks.
    const float2 shift = vt.xy / (vt.z + 0.4) * (kp_Depth * fade / layers);
    Texture2D<float4> tex = kk_Tex2D[kk_TexIndex(0)];
    SamplerState s = kk_PSSamplerState(0);
    float2 uv = uv0;
    float layer = 0.0;
    float depth = kp_Height(tex, s, uv, dx0, dy0);
    float before = depth;
    [loop] for (int i = 0; i < 16 && layer < depth; ++i)
    {
        before = depth - layer;
        uv -= shift;
        layer += 1.0 / layers;
        depth = kp_Height(tex, s, uv, dx0, dy0);
    }
    // Between the last two steps, where the view crossed the height.
    const float after = depth - layer;
    const float w = before - after > 1e-5 ? saturate(after / (after - before)) : 0.0;
    uv += shift * w;
    const float2 moved = uv - uv0;
    t0.xy = uv;
    if (normalUv)
    {
        // The same move on screen, in the normal map's coordinates.
        const float det = dx0.x * dy0.y - dx0.y * dy0.x;
        if (abs(det) > 1e-14)
        {
            const float2 onScreen = float2(moved.x * dy0.y - moved.y * dy0.x, dx0.x * moved.y - dx0.y * moved.x) / det;
            t1.xy += dx1 * onScreen.x + dy1 * onScreen.y;
        }
    }
}
)HLSL";

// The fetch block of fetch constant `slot`: where its coordinate line is, and the register it reads.
size_t FetchCoordinates(const std::string& h, size_t from, int slot, std::string* source) {
    const std::string mark = "uint kti = kk_TexIndex(" + std::to_string(slot) + ");";
    const size_t at = h.find(mark, from);
    if (at == std::string::npos) return std::string::npos;
    const std::string kc = "float2 kc = ";
    const size_t c = h.find(kc, at);
    const size_t end = h.find("}", at);
    if (c == std::string::npos || c > end) return std::string::npos;
    const size_t semi = h.find(';', c);
    *source = h.substr(c + kc.size(), semi - c - kc.size());
    return c;
}

// Whether register `reg` is written between `from` and `to` (after its copy from the input).
bool Written(const std::string& h, size_t from, size_t to, const std::string& reg) {
    for (const std::string& w : {"\t" + reg + " = ", "\t" + reg + ".", "\t\t" + reg + " = ", "\t\t" + reg + "."}) {
        size_t at = h.find(w, from);
        if (at != std::string::npos && at < to) return true;
    }
    return false;
}

}  // namespace

const std::string& parallaxText() {
    static const std::string text(kText);
    return text;
}

bool applyParallax(std::string& h, std::string& error) {
    const size_t main = h.find("void main(");
    if (main == std::string::npos) return error = "parallax: no main", false;
    const size_t copy0 = h.find("\tr0 = iT0;\n", main);
    // The body: the first instruction (annotated or not), after the copies of the inputs.
    size_t body = h.find("\t// @", main);
    const size_t block = copy0 == std::string::npos ? std::string::npos : h.find("\n\t{\n", copy0);
    if (block != std::string::npos && (body == std::string::npos || block + 1 < body)) body = block + 1;
    if (copy0 == std::string::npos || body == std::string::npos || copy0 > body)
        return error = "parallax: TEXCOORD0 is not copied into r0", false;
    std::string source;
    const size_t fetch0 = FetchCoordinates(h, body, 0, &source);
    if (fetch0 == std::string::npos || source != "r0.xy")
        return error = "parallax: the base texture is not fetched at r0.xy", false;
    if (Written(h, body, fetch0, "r0")) return error = "parallax: r0 changes before the base texture", false;
    bool normal = false;
    const size_t copy1 = h.find("\tr1 = iT1;\n", main);
    const size_t fetch1 = FetchCoordinates(h, body, 1, &source);
    if (copy1 != std::string::npos && copy1 < body && fetch1 != std::string::npos && source == "r1.xy" &&
        !Written(h, body, fetch1, "r1"))
        normal = true;
    h.insert(body, std::string("\tkp_Parallax(iPos, r0, r1, ") + (normal ? "true" : "false") + ");\n");
    h.insert(main, lightingText() + "\n" + parallaxText() + "\n");
    return true;
}

}  // namespace kkshaders
