#include <cstring>
#include "backend/fallback_resources.h"
#include "backend/tests/check.h"

namespace {
struct Texture { unsigned layers = 1; unsigned written = 0; bool readable = false; };
struct Buffer { bool written = false, readable = false; };
struct Commands {
  Texture* gamma;
  unsigned uploads = 0, transitions = 0;
  bool committed = false;
  void writeTexture(Texture* t, uint32_t slice, uint32_t mip, const void* data, size_t pitch) {
    CHECK_EQ(mip, 0u);
    CHECK(slice < t->layers);
    CHECK(!(t->written & (1u << slice)));
    t->written |= 1u << slice;
    if (t == gamma) {
      CHECK_EQ(pitch, 2048u);
      const auto* ramp = static_cast<const uint16_t*>(data);
      for (unsigned i = 0; i < 256; ++i) {
        for (unsigned channel = 0; channel < 3; ++channel) CHECK_EQ(ramp[i * 4 + channel], i * 257);
        CHECK_EQ(ramp[i * 4 + 3], 65535u);
      }
    } else {
      CHECK_EQ(pitch, 4u);
      CHECK_EQ(*static_cast<const uint32_t*>(data), 0u);
    }
    ++uploads;
  }
  void writeBuffer(Buffer* b, const void* data, size_t bytes) {
    CHECK_EQ(bytes, 256u);
    for (size_t i = 0; i < bytes; ++i) CHECK_EQ(static_cast<const uint8_t*>(data)[i], 0u);
    b->written = true;
    ++uploads;
  }
  void setTextureState(Texture* t, const nvrhi::TextureSubresourceSet& sub, nvrhi::ResourceStates state) {
    CHECK_EQ(t->written, (1u << t->layers) - 1);
    CHECK_EQ(sub.numArraySlices, nvrhi::AllSubresources.numArraySlices);
    CHECK(state == nvrhi::ResourceStates::ShaderResource);
    t->readable = true;
    ++transitions;
  }
  void setBufferState(Buffer* b, nvrhi::ResourceStates state) {
    CHECK(b->written);
    CHECK(state == nvrhi::ResourceStates::ShaderResource);
    b->readable = true;
    ++transitions;
  }
  void commitBarriers() { CHECK_EQ(transitions, 6u); committed = true; }
};
}

TEST(FallbackResources_BlackAllFacesZeroBufferIdentityGammaBeforeRead) {
  Texture two, three, cube{6}, array, gamma;
  Buffer buffer;
  Commands cl{&gamma};
  nr::UploadFallbackResources(&cl, &two, &three, &cube, &array, &buffer, &gamma);
  CHECK_EQ(cl.uploads, 11u);
  for (auto* t : {&two, &three, &cube, &array, &gamma}) CHECK(t->readable);
  CHECK(buffer.readable);
  CHECK(cl.committed);
}

NR_TEST_MAIN()
