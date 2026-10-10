// Every kknr host format against NVRHI's own table: same name, bytes per block and block size, so the data
// ConvertTexture produces is what nvrhi::ICommandList::writeTexture expects for that format.
#include <cstring>
#include <string>

#include "kknr/nvrhi_format.h"
#include "test.h"

using namespace kknr;

TEST(nvrhi_texture_formats_match) {
  for (uint32_t i = 1; i < uint32_t(HostFormat::COUNT); ++i) {
    const HostFormat f = HostFormat(i);
    const HostFormatInfo& ours = GetHostFormatInfo(f);
    const nvrhi::FormatInfo& theirs = nvrhi::getFormatInfo(ToNvrhi(f));
    if (std::strcmp(ours.name, theirs.name) != 0 || ours.bytes_per_block != theirs.bytesPerBlock ||
        ours.block_size != theirs.blockSize)
      kknr_test::Fail(__FILE__, __LINE__, std::string(ours.name) + " vs nvrhi " + theirs.name);
  }
}

TEST(nvrhi_every_guest_format_has_a_host_format) {
  for (uint32_t i = 0; i < 64; ++i) {
    const FormatChoice c = GetFormatChoice(TextureFormat(i));
    CHECK(ToNvrhi(c.format) != nvrhi::Format::UNKNOWN);
    if (c.signed_format != HostFormat::UNKNOWN) CHECK(ToNvrhi(c.signed_format) != nvrhi::Format::UNKNOWN);
    if (c.decompressed_format != HostFormat::UNKNOWN) CHECK(ToNvrhi(c.decompressed_format) != nvrhi::Format::UNKNOWN);
  }
}

TEST(nvrhi_rt_and_vertex_formats) {
  for (uint32_t i = 1; i <= uint32_t(HostRtFormat::D32S8); ++i) {
    const nvrhi::FormatInfo& info = nvrhi::getFormatInfo(ToNvrhi(HostRtFormat(i)));
    if (std::strcmp(info.name, HostRtFormatName(HostRtFormat(i))) != 0)
      kknr_test::Fail(__FILE__, __LINE__, std::string(HostRtFormatName(HostRtFormat(i))) + " vs " + info.name);
  }
  for (uint32_t i = 1; i <= uint32_t(HostVertexFormat::RGBA32_FLOAT); ++i) {
    const nvrhi::FormatInfo& info = nvrhi::getFormatInfo(ToNvrhi(HostVertexFormat(i)));
    if (std::strcmp(info.name, HostVertexFormatName(HostVertexFormat(i))) != 0)
      kknr_test::Fail(__FILE__, __LINE__, std::string(HostVertexFormatName(HostVertexFormat(i))) + " vs " + info.name);
  }
  const nvrhi::ComponentMapping m = ToNvrhiMapping(MakeSwizzle(kSwzZ, kSwzY, kSwzX, kSwz1));
  CHECK(m.r == nvrhi::ComponentSwizzle::B && m.g == nvrhi::ComponentSwizzle::G && m.b == nvrhi::ComponentSwizzle::R &&
        m.a == nvrhi::ComponentSwizzle::One);
  CHECK_EQ(m.pack(), MakeSwizzle(kSwzZ, kSwzY, kSwzX, kSwz1));
}
