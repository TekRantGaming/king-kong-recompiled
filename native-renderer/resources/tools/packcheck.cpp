// Checks a pack against a catalog extracted from the player's own BF file.
#include "kknr/texture_pack.h"
#include <cstdio>
#include <map>
#include <set>
int main(int argc, char** argv) {
  if (argc != 3) {
    std::printf("usage: kknr_packcheck PACK_FOLDER EXTRACTED_TEMPLATE_FOLDER\n");
    return 2;
  }
  kknr::TexturePackManifest pack, catalog;
  std::string why;
  if (!pack.Read(std::filesystem::u8path(argv[1]), why) || !catalog.Read(std::filesystem::u8path(argv[2]), why)) {
    std::printf("manifest: %s\n", why.c_str()); return 1;
  }
  std::map<std::string,kknr::TexturePackEntry> originals;
  for (const auto& e : catalog.entries) originals.emplace(kknr::TexturePackKey(e.hash,e.name),e);
  unsigned errors=0;
  std::set<std::string> files;
  for (const auto& e : pack.entries) {
    files.insert(e.file);
    const auto key=kknr::TexturePackKey(e.hash,e.name);
    auto it=originals.find(key);
    if (it==originals.end()) { std::printf("UNKNOWN KEY %s\n",key.c_str()); ++errors; continue; }
    const auto& o=it->second;
    if (e.format!=o.format || e.width!=o.width || e.height!=o.height) {
      std::printf("ORIGINAL MISMATCH %s\n",key.c_str()); ++errors; continue;
    }
    kknr::HostTextureData data;
    if (!kknr::LoadTextureReplacement(std::filesystem::u8path(argv[1]),e,data,why)) {
      std::printf("INVALID %s: %s\n",e.file.c_str(),why.c_str()); ++errors;
    } else std::printf("OK %s: %ux%u %s, %u mips, %zu bytes\n",e.file.c_str(),data.plan.width,data.plan.height,
        kknr::GetHostFormatInfo(data.plan.format).name,data.plan.levels,data.bytes.size());
  }
  std::error_code ec;
  for (const auto& f : std::filesystem::directory_iterator(std::filesystem::u8path(argv[1]),ec)) {
    const auto ext=f.path().extension().string();
    if ((ext==".dds" || ext==".png") && !files.contains(f.path().filename().string())) {
      std::printf("UNLISTED/UNKNOWN FILE %s\n",f.path().filename().string().c_str()); ++errors;
    }
  }
  if (ec) ++errors;
  std::printf("%zu entries, %u errors\n",pack.entries.size(),errors);
  return errors ? 1 : 0;
}
