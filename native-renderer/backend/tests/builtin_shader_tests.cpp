#include "plugin/builtin_shaders.h"
#include "backend/tests/check.h"
#include "kkshaders/container.h"
#include "kkshaders/translator.h"

using namespace nr;
namespace {
void Put(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[at + i] = uint8_t(value >> (24 - 8 * i));
}
std::vector<uint8_t> SyntheticXex() {
  std::vector<uint8_t> bytes(0x40300);
  Put(bytes, 0, 0x58455832); Put(bytes, 8, 0x300);
  Put(bytes, 16, 0x100); Put(bytes, 20, 1);
  Put(bytes, 24, 0x3FF); Put(bytes, 28, 0x50);
  Put(bytes, 0x50, 16); Put(bytes, 0x54, 1); // unencrypted, basic compression
  Put(bytes, 0x58, 0x40000); Put(bytes, 0x5C, 0);
  Put(bytes, 0x210, 0x82000000);
  bytes[0x300] = 'M'; bytes[0x301] = 'Z';
  for (unsigned i = 0; i < 3; ++i) {
    const size_t offset = 0x300 + (i == 0 ? 0x3E800 : i == 1 ? 0x3E908 : 0x3EAC8);
    Put(bytes, offset, i == 0 ? 0x102A0E01 : 0x102A0E00);
    Put(bytes, offset + 4, 24); Put(bytes, offset + 8, 4);
    bytes[offset + 24] = uint8_t(0xA0 + i);
  }
  return bytes;
}
}

TEST(ExtractsExecutableShadersWithoutGuestRuntime) {
  const auto bytes = SyntheticXex();
  const auto shaders = BuiltinShaderContainers(bytes);
  CHECK_EQ(shaders.size(), size_t(3));
  if (shaders.size() != 3) return;
  CHECK_EQ(shaders[0].size(), size_t(28)); CHECK_EQ(shaders[1].size(), size_t(28));
  CHECK_EQ(shaders[0][24], 0xA0); CHECK_EQ(shaders[1][24], 0xA1);
  CHECK_EQ(shaders[2][24], 0xA2);
  auto uncompressed = bytes;
  Put(uncompressed, 0x54, 0);
  CHECK(BuiltinShaderContainers(uncompressed) == shaders);
}

TEST(RejectsTruncatedAndUnsupportedExecutableData) {
  CHECK(BuiltinShaderContainers({}).empty());
  const auto valid = SyntheticXex();
  for (size_t cut : {size_t(16), size_t(0x200), size_t(0x3E800)})
    CHECK(BuiltinShaderContainers(std::span(valid).first(cut)).empty());
  for (const auto [at, value] : {std::pair<size_t, uint32_t>{8, 0xFFFFFFFF},
       {16, 0xFFFFFFFF}, {20, 0xFFFFFFFF}, {28, 0xFFFFFFFF}, {0x50, 0xFFFFFFFF},
       {0x54, 2}, {0x58, 0xFFFFFFFF}, {0x210, 0x83000000}, {0x3EC0C, 0xFFFFFFFF}}) {
    auto damaged = valid;
    Put(damaged, at, value);
    CHECK(BuiltinShaderContainers(damaged).empty());
  }
}

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--inspect") == 0) {
    const auto containers = ReadBuiltinShaderContainers(argv[2]);
    std::printf("executable shader containers: %zu\n", containers.size());
    for (const auto& bytes : containers) {
      auto parsed = kkshaders::parseContainer(bytes);
      std::printf("parsed=%d kind=%d hash=%016llX input=%016llX\n", parsed.ok, int(parsed.info.kind),
          parsed.info.ucodeHash, kkshaders::translationInputHash(parsed.info));
      if (!parsed.ok) return 1;
      if (parsed.info.kind == kkshaders::ShaderKind::Pixel) {
        parsed.info.renderScaleAware = true;
        std::printf("scaled input=%016llX\n", kkshaders::translationInputHash(parsed.info));
      }
    }
    return containers.size() == 3 ? 0 : 1;
  }
  return nr::test::RunAll(argc, argv);
}
