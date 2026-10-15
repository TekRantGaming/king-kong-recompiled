#include "plugin/builtin_shaders.h"

#include <algorithm>
#include <cstring>
#include <fstream>

// Reuse the SDK loader's AES implementation (exported by rexruntime). No guest
// runtime, guest execution, global kernel state or files are needed to decode.
void aes_decrypt_buffer(const uint8_t*, const uint8_t*, size_t, uint8_t*, size_t);

namespace nr {
namespace {
uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
constexpr uint32_t kBase = 0x82000000;
constexpr uint32_t kAddresses[] = {0x8203E800, 0x8203E908, 0x8203EAC8};
constexpr size_t kPrefix = 0x40000;
}

std::vector<std::vector<uint8_t>> BuiltinShaderContainers(std::span<const uint8_t> xex) {
  if (xex.size() < 24 || Be32(xex.data()) != 0x58455832) return {};
  const uint32_t header = Be32(xex.data() + 8), security = Be32(xex.data() + 16);
  const uint32_t count = Be32(xex.data() + 20);
  if (header > xex.size() || count > (xex.size() - 24) / 8 ||
      security > xex.size() || xex.size() - security < 0x164 ||
      Be32(xex.data() + security + 0x110) != kBase) return {};
  size_t format = 0;
  for (uint32_t i = 0; i < count; ++i)
    if (Be32(xex.data() + 24 + i * 8) == 0x3FF) format = Be32(xex.data() + 28 + i * 8);
  if (!format || format > header || header - format < 8) return {};
  const uint32_t info_size = Be32(xex.data() + format);
  if (info_size < 8 || info_size > header - format) return {};
  const uint16_t encryption = uint16_t(xex[format + 4] << 8 | xex[format + 5]);
  const uint16_t compression = uint16_t(xex[format + 6] << 8 | xex[format + 7]);
  // This port's executable uses basic compression. Fail closed for other
  // formats rather than treating compressed bytes as shader containers.
  if (encryption > 1 || compression > 1 || (compression == 1 && (info_size - 8) % 8)) return {};
  const size_t source_size = xex.size() - header;
  const size_t prefix_size = std::min(source_size, kPrefix);
  std::vector<uint8_t> source(prefix_size);
  if (encryption == 1) {
    if (prefix_size % 16) return {};
    // The same retail key used by the SDK XEX loader, not game-derived data.
    constexpr uint8_t retail[16] = {0x20,0xB1,0x85,0xA5,0x9D,0x28,0xFD,0xC3,
                                    0x40,0x58,0x3F,0xBB,0x08,0x96,0xBF,0x91};
    uint8_t session[16];
    aes_decrypt_buffer(retail, xex.data() + security + 0x150, 16, session, 16);
    aes_decrypt_buffer(session, xex.data() + header, prefix_size, source.data(), source.size());
  } else {
    std::copy_n(xex.data() + header, prefix_size, source.data());
  }
  std::vector<uint8_t> image(kPrefix);
  if (compression == 0) {
    std::copy(source.begin(), source.end(), image.begin());
  } else {
    size_t input = 0, output = 0;
    for (size_t at = format + 8; at + 8 <= format + info_size && output < kPrefix; at += 8) {
      const uint32_t data = Be32(xex.data() + at), zero = Be32(xex.data() + at + 4);
      if (input > source_size || data > source_size - input) return {};
      const size_t take = std::min<size_t>(data, kPrefix - output);
      if (input > source.size() || take > source.size() - input) return {};
      std::copy_n(source.data() + input, take, image.data() + output);
      input += data;
      output += take;
      output += std::min<size_t>(zero, kPrefix - output);
    }
  }
  if (image[0] != 'M' || image[1] != 'Z') return {};
  std::vector<std::vector<uint8_t>> containers;
  for (const uint32_t address : kAddresses) {
    const size_t offset = address - kBase;
    const uint32_t magic = Be32(image.data() + offset);
    const uint32_t virtual_size = Be32(image.data() + offset + 4);
    const uint32_t physical_size = Be32(image.data() + offset + 8);
    if (magic != (containers.empty() ? 0x102A0E01u : 0x102A0E00u) || virtual_size < 24 ||
        virtual_size > image.size() - offset || physical_size > image.size() - offset - virtual_size) return {};
    containers.emplace_back(image.begin() + offset, image.begin() + offset + virtual_size + physical_size);
  }
  return containers;
}

std::vector<std::vector<uint8_t>> ReadBuiltinShaderContainers(const std::filesystem::path& xex) {
  std::ifstream f(xex, std::ios::binary | std::ios::ate);
  if (!f) return {};
  const auto size = f.tellg();
  if (size < 0 || size > 128 * 1024 * 1024) return {};
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return {};
  return BuiltinShaderContainers(bytes);
}
}  // namespace nr
