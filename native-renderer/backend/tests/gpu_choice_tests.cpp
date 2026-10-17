// The GPU every device we make uses (task 203): selection order with fake adapter lists.
#include "backend/gpu_choice.h"

#include <cstdio>

using nr::gpu_choice::Candidate;
using nr::gpu_choice::Choose;
using nr::gpu_choice::Kind;

namespace {
int failures = 0;
void Expect(const char* what, int got, int want) {
  if (got != want) {
    std::printf("FAIL %s: chose %d, want %d\n", what, got, want);
    ++failures;
  }
}
constexpr uint64_t kGiB = 1ull << 30;
Candidate Vk(int index, const char* name, Kind kind, uint64_t memory) {
  Candidate c;
  c.index = index;
  c.name = name;
  c.kind = kind;
  c.local_memory = memory;
  c.software = kind == Kind::kCpu;
  return c;
}
Candidate Dxgi(int index, const char* name, uint64_t memory, int preference, bool software = false) {
  Candidate c;
  c.index = index;
  c.name = name;
  c.local_memory = memory;
  c.preference = preference;
  c.software = software;
  return c;
}
}  // namespace

int main() {
  // The crash reports' laptop, Vulkan: 0 Intel UHD, 1 Dozen on the Intel UHD, 2 RTX 4050.
  Expect("vulkan hybrid laptop",
         Choose({Vk(0, "Intel(R) UHD Graphics", Kind::kIntegrated, 2 * kGiB),
                 Vk(1, "Microsoft Direct3D12 (Intel(R) UHD Graphics)", Kind::kIntegrated, 2 * kGiB),
                 Vk(2, "NVIDIA GeForce RTX 4050 Laptop GPU", Kind::kDiscrete, 6 * kGiB)}),
         2);
  // Integrated first, discrete second (even with less memory reported by the dGPU).
  Expect("vulkan integrated first", Choose({Vk(0, "AMD Radeon(TM) Graphics", Kind::kIntegrated, 8 * kGiB),
                                            Vk(1, "AMD Radeon RX 7600S", Kind::kDiscrete, 4 * kGiB)}),
         1);
  // Two discrete GPUs: the most device-local memory, then the lower index.
  Expect("vulkan two discrete", Choose({Vk(0, "NVIDIA GeForce GTX 1650", Kind::kDiscrete, 4 * kGiB),
                                        Vk(1, "NVIDIA GeForce RTX 4070 Ti", Kind::kDiscrete, 12 * kGiB)}),
         1);
  Expect("vulkan two equal discrete", Choose({Vk(0, "GPU A", Kind::kDiscrete, 8 * kGiB),
                                              Vk(1, "GPU B", Kind::kDiscrete, 8 * kGiB)}),
         0);
  // A CPU device (lavapipe) is never chosen, even alone.
  Expect("vulkan cpu device", Choose({Vk(0, "llvmpipe (LLVM 17.0.6, 256 bits)", Kind::kCpu, 16 * kGiB),
                                      Vk(1, "AMD Custom GPU 0405", Kind::kIntegrated, 1 * kGiB)}),
         1);
  Expect("vulkan cpu only", Choose({Vk(0, "llvmpipe (LLVM 17.0.6, 256 bits)", Kind::kCpu, 16 * kGiB)}), -1);
  // Dozen alone or with a CPU device: nothing (the SDK chooses), never the layered device.
  Expect("vulkan dozen only", Choose({Vk(0, "Microsoft Direct3D12 (NVIDIA GeForce RTX 4050 Laptop GPU)",
                                          Kind::kDiscrete, 6 * kGiB)}),
         -1);
  // A discrete Dozen device does not beat a real integrated one.
  Expect("vulkan dozen discrete vs real integrated",
         Choose({Vk(0, "Microsoft Direct3D12 (NVIDIA GeForce RTX 4050 Laptop GPU)", Kind::kDiscrete, 6 * kGiB),
                 Vk(1, "Intel(R) UHD Graphics", Kind::kIntegrated, 2 * kGiB)}),
         1);
  // Single GPU: itself.
  Expect("vulkan single", Choose({Vk(0, "NVIDIA GeForce RTX 4070 Ti", Kind::kDiscrete, 12 * kGiB)}), 0);

  // DXGI: EnumAdapters1 order has the iGPU first; the high-performance rank decides.
  Expect("dxgi integrated first",
         Choose({Dxgi(0, "Intel(R) UHD Graphics", 128ull << 20, 1), Dxgi(1, "NVIDIA GeForce RTX 4050 Laptop GPU", 6 * kGiB, 0),
                 Dxgi(2, "Microsoft Basic Render Driver", 0, 2, true)}),
         1);
  // Older Windows without IDXGIFactory6 (no rank): the most dedicated memory.
  Expect("dxgi no preference", Choose({Dxgi(0, "Intel(R) UHD Graphics", 128ull << 20, -1),
                                       Dxgi(1, "NVIDIA GeForce RTX 4050 Laptop GPU", 6 * kGiB, -1)}),
         1);
  // Two discrete: DXGI's own order.
  Expect("dxgi two discrete", Choose({Dxgi(0, "NVIDIA GeForce RTX 3060", 12 * kGiB, 1),
                                      Dxgi(1, "NVIDIA GeForce RTX 4070 Ti", 12 * kGiB, 0)}),
         1);
  // A software adapter (WARP / Basic Render) is never chosen, even ranked first.
  Expect("dxgi software first", Choose({Dxgi(0, "Microsoft Basic Render Driver", 0, 0, true),
                                        Dxgi(1, "Intel(R) Iris(R) Xe Graphics", 128ull << 20, 1)}),
         1);
  Expect("dxgi software only", Choose({Dxgi(0, "Microsoft Basic Render Driver", 0, 0, true)}), -1);
  // An adapter that cannot make a D3D12 device is skipped.
  {
    auto list = std::vector<Candidate>{Dxgi(0, "Intel(R) UHD Graphics", 128ull << 20, 1),
                                       Dxgi(1, "NVIDIA GeForce GTX 660", 2 * kGiB, 0)};
    list[1].usable = false;
    Expect("dxgi unusable skipped", Choose(list), 0);
  }
  // The Basic Render Driver is recognised by vendor/device even without the software flag.
  Expect("basic render by id", nr::gpu_choice::IsBasicRender(0x1414, 0x8C, "x") ? 1 : 0, 1);
  Expect("dozen name", nr::gpu_choice::IsLayeredName("Microsoft Direct3D12 (Intel(R) UHD Graphics)") ? 1 : 0, 1);
  Expect("not dozen", nr::gpu_choice::IsLayeredName("Intel(R) UHD Graphics") ? 1 : 0, 0);

  if (failures) return 1;
  std::printf("gpu_choice: all selection cases passed\n");
  return 0;
}
