// kknr_sdkref --gpu: the SDK's D3D12 texture load shaders run as the game's renderer runs them. See sdkref.h.
// Covers the load shaders the census needs (8, 16, 32, 64 and 128 bits per block copies, depth_unorm).
#if defined(_WIN32)

#include "sdkref.h"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace shaders {
#include "texture_load_128bpb_cs.h"
#include "texture_load_16bpb_cs.h"
#include "texture_load_32bpb_cs.h"
#include "texture_load_64bpb_cs.h"
#include "texture_load_8bpb_cs.h"
#include "texture_load_depth_unorm_cs.h"
}  // namespace shaders

#pragma comment(lib, "d3d12.lib")

using Microsoft::WRL::ComPtr;
namespace sdk = rex::graphics;
namespace tu = rex::graphics::texture_util;

namespace kknr_sdkref {
namespace {

// TextureCache::LoadConstants and the LoadShaderInfo entries the SDK uses for these shaders
// (src/graphics/pipeline/texture/cache.cpp: {source_bpe_log2, dest_bpe_log2, bytes_per_host_block,
// guest_x_blocks_per_thread_log2}).
struct LoadConstants {
  uint32_t is_tiled_3d_endian_scale;
  uint32_t guest_offset;
  uint32_t guest_pitch_aligned;
  uint32_t guest_z_stride_block_rows_aligned;
  uint32_t size_blocks[3];
  uint32_t host_offset;
  uint32_t host_pitch;
  uint32_t height_texels;
};
struct LoadShader {
  const BYTE* code;
  size_t size;
  uint32_t source_bpe_log2, dest_bpe_log2, bytes_per_host_block, guest_x_blocks_per_thread_log2;
};
enum ShaderIndex { k8bpb, k16bpb, k32bpb, k64bpb, k128bpb, kDepthUnorm, kShaderCount };
const LoadShader kShaders[kShaderCount] = {
    {shaders::texture_load_8bpb_cs, sizeof(shaders::texture_load_8bpb_cs), 3, 4, 1, 4},
    {shaders::texture_load_16bpb_cs, sizeof(shaders::texture_load_16bpb_cs), 4, 4, 2, 4},
    {shaders::texture_load_32bpb_cs, sizeof(shaders::texture_load_32bpb_cs), 4, 4, 4, 3},
    {shaders::texture_load_64bpb_cs, sizeof(shaders::texture_load_64bpb_cs), 4, 4, 8, 2},
    {shaders::texture_load_128bpb_cs, sizeof(shaders::texture_load_128bpb_cs), 4, 4, 16, 1},
    {shaders::texture_load_depth_unorm_cs, sizeof(shaders::texture_load_depth_unorm_cs), 4, 4, 4, 3},
};
constexpr uint32_t kLoadGuestXThreadsPerGroupLog2 = 2, kLoadGuestYBlocksPerGroupLog2 = 5;

DXGI_FORMAT UintPow2Format(uint32_t log2) {
  switch (log2) {
    case 0: return DXGI_FORMAT_R8_UINT;
    case 1: return DXGI_FORMAT_R16_UINT;
    case 2: return DXGI_FORMAT_R32_UINT;
    case 3: return DXGI_FORMAT_R32G32_UINT;
    default: return DXGI_FORMAT_R32G32B32A32_UINT;
  }
}

uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

struct Gpu {
  bool ok = false;
  std::string why;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  UINT64 fence_value = 0;
  HANDLE event = nullptr;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> pso[kShaderCount];
  ComPtr<ID3D12DescriptorHeap> heap;
  UINT descriptor_size = 0;

  Gpu() {
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
      why = "no D3D12 device";
      return;
    }
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
    list->Close();
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // The SDK's load root signature: constants b0, an SRV table t0 (source), a UAV table u0 (destination).
    D3D12_DESCRIPTOR_RANGE srv_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_DESCRIPTOR_RANGE uav_range = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = sizeof(LoadConstants) / 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &srv_range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = {1, &uav_range};
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)))) {
      why = "root signature";
      return;
    }
    for (int i = 0; i < kShaderCount; ++i) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
      pd.pRootSignature = root.Get();
      pd.CS = {kShaders[i].code, kShaders[i].size};
      if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso[i])))) {
        why = "pipeline " + std::to_string(i);
        return;
      }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ok = true;
  }

  ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags,
                                D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {type};
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
    return r;
  }

  void Run() {
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), ++fence_value);
    fence->SetEventOnCompletion(fence_value, event);
    WaitForSingleObject(event, INFINITE);
  }
};

Gpu& GetGpu() {
  static Gpu gpu;
  return gpu;
}

struct HostSlice {
  UINT64 offset = 0;
  uint32_t width = 0, height = 0, depth = 0, row_pitch = 0;
  UINT64 size = 0;
};

}  // namespace

bool SdkGpuReference(const uint32_t words[6], const std::vector<uint8_t>& memory, std::vector<RefSubresource>& out,
                     std::string& why) {
  Gpu& gpu = GetGpu();
  if (!gpu.ok) return why = gpu.why, false;
  sdk::xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, words, sizeof(fetch));
  const sdk::FormatInfo* info = sdk::FormatInfo::Get(fetch.format);
  const uint32_t bw = info->block_width, bh = info->block_height, bpb = info->bytes_per_block();
  ShaderIndex shader;
  using F = sdk::xenos::TextureFormat;
  if (fetch.format == F::k_24_8) {
    shader = kDepthUnorm;
  } else {
    switch (fetch.format) {
      case F::k_8: case F::k_8_A: case F::k_8_8: case F::k_8_8_8_8: case F::k_2_10_10_10: case F::k_32_FLOAT:
      case F::k_16: case F::k_16_16: case F::k_16_FLOAT: case F::k_16_16_FLOAT: case F::k_16_16_16_16_FLOAT:
      case F::k_32_32_FLOAT: case F::k_32_32_32_32_FLOAT: case F::k_DXT1: case F::k_DXT2_3: case F::k_DXT4_5:
      case F::k_DXN: case F::k_DXT5A: case F::k_8_8_8_8_AS_16_16_16_16: case F::k_DXT1_AS_16_16_16_16:
      case F::k_DXT2_3_AS_16_16_16_16: case F::k_DXT4_5_AS_16_16_16_16:
        break;
      default:
        return why = "format not run on the GPU", false;
    }
    switch (bpb) {
      case 1: shader = k8bpb; break;
      case 2: shader = k16bpb; break;
      case 4: shader = k32bpb; break;
      case 8: shader = k64bpb; break;
      case 16: shader = k128bpb; break;
      default: return why = "block size", false;
    }
  }
  const LoadShader& ls = kShaders[shader];

  uint32_t width_m1, height_m1, depth_m1, base_page, mip_page, mip_min, mip_max;
  tu::GetSubresourcesFromFetchConstant(fetch, &width_m1, &height_m1, &depth_m1, &base_page, &mip_page, &mip_min,
                                       &mip_max);
  if (!base_page && !mip_page) return why = "no data", false;
  const sdk::xenos::DataDimension dimension = fetch.dimension;
  const uint32_t width = width_m1 + 1, height = height_m1 + 1, depth_or_array = depth_m1 + 1;
  const bool is_3d = dimension == sdk::xenos::DataDimension::k3D;
  const uint32_t depth = is_3d ? depth_or_array : 1, array_size = is_3d ? 1 : depth_or_array;
  const tu::TextureGuestLayout layout =
      tu::GetGuestTextureLayout(dimension, fetch.pitch, width, height, depth_or_array, fetch.tiled, fetch.format,
                                fetch.packed_mips, base_page != 0, mip_max);
  const uint32_t level_first = base_page ? 0 : 1, level_last = mip_page ? mip_max : 0;
  const uint32_t level_packed = layout.packed_level;
  uint32_t loop_first, loop_last;
  if (level_packed == 0) {
    loop_first = uint32_t(level_first != 0);
    loop_last = uint32_t(level_last != 0);
  } else {
    loop_first = std::min(level_first, level_packed);
    loop_last = std::min(level_last, level_packed);
  }
  const bool host_bc = bw > 1 || bh > 1;
  const uint32_t host_bw = host_bc ? bw : 1, host_bh = host_bc ? bh : 1;
  const uint32_t x_blocks_per_thread = 1u << ls.guest_x_blocks_per_thread_log2;

  // Host layout of the copy buffer (as the SDK: per loop level, slices one after another).
  HostSlice base_slice, mip_slices[sdk::xenos::kTextureMaxMips];
  UINT64 copy_size = 0;
  for (uint32_t loop = loop_first; loop <= loop_last; ++loop) {
    const bool is_base = loop == 0;
    const uint32_t level = level_packed == 0 ? 0 : loop;
    HostSlice& hs = is_base ? base_slice : mip_slices[level];
    hs.offset = copy_size;
    if (level == level_packed) {
      const tu::TextureGuestLayout::Level& lg = is_base ? layout.base : layout.mips[level];
      hs.width = lg.x_extent_blocks * bw;
      hs.height = lg.y_extent_blocks * bh;
      hs.depth = lg.z_extent;
    } else {
      hs.width = std::max(width >> level, 1u);
      hs.height = std::max(height >> level, 1u);
      hs.depth = std::max(depth >> level, 1u);
    }
    hs.width = Align(hs.width, host_bw);
    hs.height = Align(hs.height, host_bh);
    hs.row_pitch = Align(Align(hs.width / host_bw, x_blocks_per_thread) * ls.bytes_per_host_block,
                         D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    hs.size = (UINT64(hs.row_pitch) * (hs.height / host_bh) * hs.depth + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) /
              D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    copy_size += hs.size * array_size;
  }
  copy_size = (copy_size + 15) & ~UINT64(15);

  // Source: the guest memory in an upload buffer, read through the SDK's uint pow2 SRV.
  const UINT64 source_size = (UINT64(memory.size()) + 15) & ~UINT64(15);
  ComPtr<ID3D12Resource> source =
      gpu.Buffer(source_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  ComPtr<ID3D12Resource> dest = gpu.Buffer(copy_size, D3D12_HEAP_TYPE_DEFAULT,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  ComPtr<ID3D12Resource> readback =
      gpu.Buffer(copy_size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!source || !dest || !readback) return why = "buffers", false;
  void* mapped = nullptr;
  source->Map(0, nullptr, &mapped);
  std::memset(mapped, 0, size_t(source_size));
  std::memcpy(mapped, memory.data(), memory.size());
  source->Unmap(0, nullptr);

  D3D12_CPU_DESCRIPTOR_HANDLE cpu = gpu.heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = gpu.heap->GetGPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = UintPow2Format(ls.source_bpe_log2);
  sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Buffer.NumElements = UINT(source_size >> ls.source_bpe_log2);
  gpu.device->CreateShaderResourceView(source.Get(), &sd, cpu);
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = UintPow2Format(ls.dest_bpe_log2);
  ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  ud.Buffer.NumElements = UINT(copy_size >> ls.dest_bpe_log2);
  D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav = {cpu.ptr + gpu.descriptor_size};
  gpu.device->CreateUnorderedAccessView(dest.Get(), nullptr, &ud, cpu_uav);

  gpu.allocator->Reset();
  gpu.list->Reset(gpu.allocator.Get(), gpu.pso[shader].Get());
  ID3D12DescriptorHeap* heaps[] = {gpu.heap.Get()};
  gpu.list->SetDescriptorHeaps(1, heaps);
  gpu.list->SetComputeRootSignature(gpu.root.Get());
  gpu.list->SetComputeRootDescriptorTable(1, gpu_handle);
  gpu.list->SetComputeRootDescriptorTable(2, {gpu_handle.ptr + gpu.descriptor_size});
  LoadConstants lc = {};
  lc.is_tiled_3d_endian_scale = uint32_t(fetch.tiled) | uint32_t(is_3d) << 1 | uint32_t(fetch.endianness) << 2 |
                                1u << 4 | 1u << 7;
  const uint32_t gx_log2 = kLoadGuestXThreadsPerGroupLog2 + ls.guest_x_blocks_per_thread_log2;
  for (uint32_t loop = loop_first; loop <= loop_last; ++loop) {
    const bool is_base = loop == 0;
    const uint32_t level = level_packed == 0 ? 0 : loop;
    lc.guest_offset = (is_base ? base_page : mip_page) << 12;
    if (!is_base) lc.guest_offset += layout.mip_offsets_bytes[level];
    const tu::TextureGuestLayout::Level& lg = is_base ? layout.base : layout.mips[level];
    lc.guest_pitch_aligned = fetch.tiled ? lg.row_pitch_bytes / bpb : lg.row_pitch_bytes;
    lc.guest_z_stride_block_rows_aligned = lg.z_slice_stride_block_rows;
    uint32_t lw, lh, ld;
    if (level == level_packed) {
      lw = lg.x_extent_blocks * bw;
      lh = lg.y_extent_blocks * bh;
      ld = lg.z_extent;
    } else {
      lw = std::max(width >> level, 1u);
      lh = std::max(height >> level, 1u);
      ld = std::max(depth >> level, 1u);
    }
    lc.size_blocks[0] = (lw + bw - 1) / bw;
    lc.size_blocks[1] = (lh + bh - 1) / bh;
    lc.size_blocks[2] = ld;
    lc.height_texels = lh;
    const HostSlice& hs = is_base ? base_slice : mip_slices[level];
    lc.host_offset = uint32_t(hs.offset);
    lc.host_pitch = hs.row_pitch;
    const UINT gx = (lc.size_blocks[0] + (1u << gx_log2) - 1) >> gx_log2;
    const UINT gy = (lc.size_blocks[1] + (1u << kLoadGuestYBlocksPerGroupLog2) - 1) >> kLoadGuestYBlocksPerGroupLog2;
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      gpu.list->SetComputeRoot32BitConstants(0, sizeof(lc) / 4, &lc, 0);
      gpu.list->Dispatch(gx, gy, lc.size_blocks[2]);
      lc.guest_offset += lg.array_slice_stride_bytes;
      lc.host_offset += uint32_t(hs.size);
    }
  }
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = dest.Get();
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  gpu.list->ResourceBarrier(1, &b);
  gpu.list->CopyResource(readback.Get(), dest.Get());
  gpu.Run();
  if (FAILED(gpu.device->GetDeviceRemovedReason())) return why = "device removed", false;

  const uint8_t* rb = nullptr;
  D3D12_RANGE range = {0, SIZE_T(copy_size)};
  readback->Map(0, &range, reinterpret_cast<void**>(const_cast<uint8_t**>(&rb)));
  // CopyTextureRegion into each host subresource.
  for (uint32_t level = level_first; level <= level_last; ++level) {
    const uint32_t guest_level = std::min(level, level_packed);
    const HostSlice& hs = level ? mip_slices[guest_level] : base_slice;
    uint32_t left = 0, top = 0, front = 0;
    if (level >= level_packed) {
      uint32_t ox, oy, oz;
      tu::GetPackedMipOffset(width, height, depth, fetch.format, level, ox, oy, oz);
      left = ox * bw / host_bw;
      top = oy * bh / host_bh;
      front = oz;
    }
    const uint32_t wb = Align(std::max(width >> level, 1u), host_bw) / host_bw;
    const uint32_t hb = Align(std::max(height >> level, 1u), host_bh) / host_bh;
    const uint32_t ld = std::max(depth >> level, 1u);
    const uint32_t hbpb = ls.bytes_per_host_block;
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      RefSubresource r;
      r.level = level;
      r.layer = slice;
      r.width_blocks = wb;
      r.height_blocks = hb;
      r.depth = ld;
      r.bytes_per_block = hbpb;
      r.blocks.assign(size_t(wb) * hb * ld * hbpb, 0);
      const UINT64 slice_base = hs.offset + slice * hs.size;
      const UINT64 rows_per_z = hs.height / host_bh;
      for (uint32_t z = 0; z < ld; ++z)
        for (uint32_t y = 0; y < hb; ++y) {
          const UINT64 at = slice_base + (UINT64(front + z) * rows_per_z + top + y) * hs.row_pitch + UINT64(left) * hbpb;
          if (at + UINT64(wb) * hbpb <= copy_size)
            std::memcpy(&r.blocks[(size_t(z) * hb + y) * wb * hbpb], rb + at, size_t(wb) * hbpb);
        }
      out.push_back(std::move(r));
    }
  }
  readback->Unmap(0, nullptr);
  return true;
}

}  // namespace kknr_sdkref

#else

#include "sdkref.h"

namespace kknr_sdkref {
bool SdkGpuReference(const uint32_t*, const std::vector<uint8_t>&, std::vector<RefSubresource>&, std::string& why) {
  why = "the GPU reference needs D3D12 (Windows)";
  return false;
}
}  // namespace kknr_sdkref

#endif
