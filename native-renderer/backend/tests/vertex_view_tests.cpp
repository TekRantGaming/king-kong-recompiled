#include "backend/game_renderer.h"
#include "backend/tests/fake_guest.h"
#include "backend/tests/check.h"
#include <random>

namespace nr {
struct VertexViewTestAccess {
  static nvrhi::BindingSetItem View(GameRenderer& renderer, nvrhi::IBuffer* buffer,
                                  uint32_t slot, bool pooled, uint64_t offset, uint32_t bytes) {
    GameRenderer::HostBuffer host;
    host.buffer = buffer; host.slot = slot; host.view_bytes = bytes;
    // Even a failed pool allocation may leave allocation metadata; only valid() selects it.
    host.range.allocation.offset = offset;
    if (pooled) host.range.buffer = buffer;
    return renderer.VertexView(&host);
  }
};
}
class FakeBuffer final : public nvrhi::RefCounter<nvrhi::IBuffer> {
  nvrhi::BufferDesc desc_;
 public:
  const nvrhi::BufferDesc& getDesc() const override { return desc_; }
  nvrhi::GpuVirtualAddress getGpuVirtualAddress() const override { return 0; }
};

TEST(VertexViewUnpooledUsesWholeBufferAndPooledUsesRange) {
  nr::test::FakeGuestMemory memory(64, 64);
  nr::GameRenderer renderer(nullptr, memory, {}, nullptr);  // no GPU, no Init or game
  nvrhi::BufferHandle buffer;
  buffer.Attach(new FakeBuffer);
  std::mt19937 rng(340);
  for (unsigned i = 0; i < 10000; ++i) {
    const uint32_t slot = rng(), bytes = i < 2 ? i : rng();
    const uint64_t offset = uint64_t(rng()) * 256;
    for (bool pooled : {false, true}) {
      const auto got = nr::VertexViewTestAccess::View(renderer, buffer, slot, pooled, offset, bytes);
      const auto want = nvrhi::BindingSetItem::RawBuffer_SRV(slot, buffer,
          pooled ? nvrhi::BufferRange(offset, bytes) : nvrhi::EntireBuffer);
      CHECK(std::memcmp(&got, &want, sizeof(got)) == 0);
    }
  }
}
NR_TEST_MAIN()
