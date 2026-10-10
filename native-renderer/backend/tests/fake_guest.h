// Synthetic guest memory and builders for the guest objects the draw path
// reads (layouts in backend/guest_layout.h). Everything big-endian, as on the
// 360. No game data: tests build what they need.
#pragma once

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "backend/guest_layout.h"
#include "backend/guest_memory.h"

namespace nr::test {

class FakeGuestMemory final : public GuestMemory {
 public:
  // Engine heap (objects, constants, viewports) at 0x40000000; physical
  // memory seen through the 0xA0000000 CPU view (physical = va - 0xA0000000).
  static constexpr uint32_t kHeapBase = 0x40000000;
  static constexpr uint32_t kPhysicalView = 0xA0000000;

  explicit FakeGuestMemory(uint32_t heap_bytes = 1 << 20, uint32_t physical_bytes = 4 << 20)
      : heap_(heap_bytes, 0), physical_(physical_bytes, 0) {}

  const uint8_t* Virtual(uint32_t a) const override {
    if (a >= kHeapBase && a - kHeapBase < heap_.size()) return heap_.data() + (a - kHeapBase);
    if (a >= kPhysicalView && a - kPhysicalView < physical_.size()) {
      return physical_.data() + (a - kPhysicalView);
    }
    return nullptr;
  }
  const uint8_t* Physical(uint32_t p) const override {
    return p < physical_.size() ? physical_.data() + p : nullptr;
  }
  uint8_t* Writable(uint32_t va) { return const_cast<uint8_t*>(Virtual(va)); }
  // Physical memory from address 0 (the game renderer's kknr::GuestMemory).
  const uint8_t* PhysicalBase() const { return physical_.data(); }
  uint32_t PhysicalSize() const { return uint32_t(physical_.size()); }

  uint32_t AllocHeap(uint32_t bytes, uint32_t align = 16) {
    heap_used_ = (heap_used_ + align - 1) & ~(align - 1);
    uint32_t a = kHeapBase + heap_used_;
    heap_used_ += bytes;
    return a;
  }
  // Returns the CPU (0xA0000000 view) address.
  uint32_t AllocPhysical(uint32_t bytes, uint32_t align = 4096) {
    phys_used_ = (phys_used_ + align - 1) & ~(align - 1);
    uint32_t a = kPhysicalView + phys_used_;
    phys_used_ += bytes;
    return a;
  }

  void Write32(uint32_t va, uint32_t v) { StoreBE32(Writable(va), v); }
  void Write16(uint32_t va, uint16_t v) { StoreBE16(Writable(va), v); }
  void WriteFloat(uint32_t va, float f) { StoreBEFloat(Writable(va), f); }
  void WriteFloats(uint32_t va, std::initializer_list<float> fs) {
    for (float f : fs) {
      WriteFloat(va, f);
      va += 4;
    }
  }
  uint32_t NewFloats(std::initializer_list<float> fs) {
    uint32_t a = AllocHeap(uint32_t(fs.size() * 4));
    WriteFloats(a, fs);
    return a;
  }

  // An index buffer object over new physical memory holding `indices`.
  uint32_t NewIndexBuffer(const std::vector<uint32_t>& indices, bool index32) {
    uint32_t size = uint32_t(indices.size()) * (index32 ? 4 : 2);
    uint32_t data = AllocPhysical(size);
    for (size_t i = 0; i < indices.size(); ++i) {
      if (index32) {
        Write32(data + uint32_t(i) * 4, indices[i]);
      } else {
        Write16(data + uint32_t(i) * 2, uint16_t(indices[i]));
      }
    }
    uint32_t obj = AllocHeap(32);
    Write32(obj, (index32 ? 0x80000000u : 0) | (2u << 16) | 1);  // type 2, refcount 1
    Write32(obj + 12, data);
    Write32(obj + 16, size);
    return obj;
  }

  // A vertex buffer object over `bytes` of new physical memory; returns the
  // object and stores the data's CPU address in data_out.
  uint32_t NewVertexBuffer(uint32_t bytes, uint32_t& data_out) {
    data_out = AllocPhysical(bytes);
    uint32_t obj = AllocHeap(32);
    Write32(obj, (1u << 16) | 1);  // type 1, refcount 1
    Write32(obj + 12, ((data_out - kPhysicalView) & ~3u) | 3);
    Write32(obj + 16, ((bytes / 4) << 2) | 2);  // 8in32
    return obj;
  }

  struct Element {
    uint16_t stream, offset;
    uint32_t type;
    uint8_t usage, usage_index;
  };
  uint32_t NewDeclaration(std::initializer_list<Element> elements) {
    uint32_t obj = AllocHeap(kDeclElementsOffset + uint32_t(elements.size() + 1) * kDeclElementSize);
    Write32(obj + kDeclCountOffset, uint32_t(elements.size()));
    uint32_t e = obj + kDeclElementsOffset;
    for (const Element& el : elements) {
      Write16(e, el.stream);
      Write16(e + 2, el.offset);
      Write32(e + 4, el.type);
      Writable(e + 8)[0] = 0;
      Writable(e + 9)[0] = el.usage;
      Writable(e + 10)[0] = el.usage_index;
      e += kDeclElementSize;
    }
    Write16(e, 0xFF);  // D3DDECL_END
    return obj;
  }

  uint32_t NewViewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float min_z, float max_z) {
    uint32_t a = AllocHeap(24);
    Write32(a, x);
    Write32(a + 4, y);
    Write32(a + 8, w);
    Write32(a + 12, h);
    WriteFloat(a + 16, min_z);
    WriteFloat(a + 20, max_z);
    return a;
  }

 private:
  std::vector<uint8_t> heap_;
  std::vector<uint8_t> physical_;
  uint32_t heap_used_ = 0x100;
  uint32_t phys_used_ = 0;
};

// The 360 D3DDECLTYPEs the tests use.
constexpr uint32_t kDeclFloat3 = 0x002A23B9;
constexpr uint32_t kDeclFloat2 = 0x002C23A5;
constexpr uint32_t kDeclColor = 0x00182886;
constexpr uint8_t kUsagePosition = 0;
constexpr uint8_t kUsageTexcoord = 5;
constexpr uint8_t kUsageColor = 10;

}  // namespace nr::test
