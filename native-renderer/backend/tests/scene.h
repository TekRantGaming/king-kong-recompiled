// A synthetic frame in the form the game issues it: guest objects in fake
// guest memory and the call sequence as KK_DEV_D3D_TRACE log lines, following
// the per-draw sequence in docs/d3d-api-map.md (engine-side flow):
//
//   Clear
//   SetRenderTarget(0, backbuffer), SetViewport
//   BeginConditionalRendering(id)
//   SetIndices(IB)
//   SetVertexShaderConstantF(0, world, 4)
//   SetStreamSource(0, VB, 0, stride)
//   SetVertexDeclaration, SetTexture(0), SetPixelShader, SetVertexShader, blend control
//   [render / sampler states]
//   DrawIndexedVertices(TRIANGLELIST, baseVertex, startIndex, n)
//   EndConditionalRendering()
//   Present
#pragma once

#include <cstdio>
#include <string>

#include "backend/tests/fake_guest.h"

namespace nr::test {

constexpr uint32_t kDevice = 0x4006A580;
constexpr uint32_t kClearColor = 0xFF102030;  // ARGB

inline std::string TraceLine(uint32_t address, std::initializer_list<uint32_t> regs_from_r4,
                             double f1 = 0.0) {
  uint32_t r[8] = {kDevice};
  int i = 1;
  for (uint32_t v : regs_from_r4) r[i++] = v;
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "i> 0000 KK d3d: sub_%08X lr=82862200 r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X "
                "r8=%08X r9=%08X r10=%08X f1=%g\n",
                address, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], f1);
  return buf;
}

struct TriangleScene {
  uint32_t backbuffer = 0x4F000100;   // surface objects are opaque here
  uint32_t shadow_map = 0x4F000200;
  uint32_t vertex_shader = 0x4E001000;
  uint32_t pixel_shader = 0x4E002000;
  uint32_t texture = 0x4D000040;
  uint32_t width = 0, height = 0;
  uint32_t ib = 0, vb = 0, vb_data = 0, decl = 0, wvp = 0, viewport = 0;
  static constexpr uint32_t kStride = 24;  // colour @0, position @4, uv @16
  static constexpr uint32_t kBaseVertex = 2;
  static constexpr uint32_t kStartIndex = 3;
  static constexpr uint32_t kConditionalId = 17;
  static constexpr uint32_t kBlend = 0x07060706;
  // Object-space positions; the matrix scales x and y by 1.6 and moves x by
  // +0.1, so the clip-space triangle is (-0.7,-0.8) (0.1,0.8) (0.9,-0.8).
  static constexpr float kPositions[3][3] = {{-0.5f, -0.5f, 0.5f}, {0.0f, 0.5f, 0.5f},
                                             {0.5f, -0.5f, 0.5f}};

  TriangleScene(FakeGuestMemory& m, uint32_t w, uint32_t h) : width(w), height(h) {
    // Two junk vertices first: the draw reaches its vertices through the
    // base vertex; three junk indices first: through the start index.
    ib = m.NewIndexBuffer({7, 7, 7, 0, 1, 2}, false);
    vb = m.NewVertexBuffer(kStride * 5, vb_data);
    for (uint32_t v = 0; v < 5; ++v) {
      uint32_t at = vb_data + v * kStride;
      m.Write32(at, 0xFF00FF00);
      const float* p = v >= kBaseVertex ? kPositions[v - kBaseVertex] : kPositions[0];
      float junk = v < kBaseVertex ? 100.0f : 1.0f;  // junk vertices far off screen
      m.WriteFloats(at + 4, {p[0] * junk, p[1] * junk, p[2]});
      m.WriteFloats(at + 16, {0.25f, 0.75f});
    }
    decl = m.NewDeclaration({{0, 0, kDeclColor, kUsageColor, 0},
                             {0, 4, kDeclFloat3, kUsagePosition, 0},
                             {0, 16, kDeclFloat2, kUsageTexcoord, 0}});
    // A perspective-shaped matrix (z row parallel to the w row, which the
    // renderer looks for): w = 2z = 1 for these positions, so x and y come out
    // as above, at depth 0.5.
    wvp = m.NewFloats({1.6f, 0, 0, 0.1f,  //
                       0, 1.6f, 0, 0,     //
                       0, 0, 2, -0.5f,    //
                       0, 0, 2, 0});
    viewport = m.NewViewport(0, 0, w, h, 0.0f, 1.0f);
  }

  // The frame as trace lines.
  std::string Log() const {
    std::string s;
    s += "i> 0000 KK d3d: window start at frame 1 (0.0 s), 1 frames\n";
    s += TraceLine(0x8210C378, {0, backbuffer});         // SetRenderTarget(0, backbuffer)
    s += TraceLine(0x8210BAC8, {viewport});              // SetViewport
    s += TraceLine(0x82115418, {0, 0, 1, kClearColor, 0, 0x55}, 1.0);  // Clear(0, null, TARGET, colour, z=1, stencil r9)
    s += TraceLine(0x8210CB50, {kConditionalId});        // BeginConditionalRendering
    s += TraceLine(0x8210BE38, {ib});                    // SetIndices
    s += "i> 0000 KK d3d: sub_821241A8 lr=82862210 r3=40001000 r4=40001040 r5=40001080 r6=0 r7=0 r8=0 r9=0 r10=0 f1=0\n";  // XMMatrixMultiply (not hooked)
    s += TraceLine(0x82110300, {0, wvp, 4});             // SetVertexShaderConstantF(0, wvp, 4)
    s += TraceLine(0x8210BD38, {0, vb, 0, kStride});     // SetStreamSource(0, vb, 0, stride)
    s += TraceLine(0x82111E68, {decl});                  // SetVertexDeclaration
    s += TraceLine(0x82118F78, {0, texture});            // SetTexture(0, texture)
    s += TraceLine(0x821108B8, {pixel_shader});          // SetPixelShader (see hook_table.cpp)
    s += TraceLine(0x82110C28, {vertex_shader});         // SetVertexShader
    s += TraceLine(0x8210C130, {0, kBlend});             // blend control RT0
    s += TraceLine(0x82109788, {6});                     // CULLMODE = CCW
    s += TraceLine(0x82109E78, {1});                     // ZENABLE
    s += TraceLine(0x82109F00, {7});                     // ZFUNC = ALWAYS
    s += TraceLine(0x8210B270, {0, 1});                  // sampler 0 MAGFILTER = LINEAR
    s += TraceLine(0x82115708, {4, kBaseVertex, kStartIndex, 3});  // DrawIndexedVertices
    s += TraceLine(0x8210CB70, {});                      // EndConditionalRendering
    // A render-to-texture draw: must not land in the frame once the main
    // surface is known.
    s += TraceLine(0x8210C378, {0, shadow_map});
    s += TraceLine(0x82115708, {4, kBaseVertex, kStartIndex, 3});
    s += TraceLine(0x8210C378, {0, backbuffer});
    s += TraceLine(0x821147B8, {});                      // Present
    s += "i> 0000 KK d3d: frame 1 end: sub_82115708=2\n";
    return s;
  }
};

}  // namespace nr::test
