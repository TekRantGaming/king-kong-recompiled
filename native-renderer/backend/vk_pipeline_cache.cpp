#include "backend/vk_pipeline_cache.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

#include "backend/file_lock.h"

#ifndef VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#endif
#include <vulkan/vulkan.hpp>

namespace nr {

namespace {

constexpr uint32_t kFileMagic = 0x4350564Bu;  // 'KVPC'
constexpr uint32_t kFileVersion = 1;

struct FileHeader {
  uint32_t magic = kFileMagic;
  uint32_t version = kFileVersion;
  uint32_t header_size = 0;
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  uint32_t driver_version = 0;
  uint8_t pipeline_cache_uuid[VK_UUID_SIZE] = {};
  uint8_t driver_uuid[VK_UUID_SIZE] = {};
  uint64_t data_size = 0;
  uint64_t data_hash = 0;
};
static_assert(sizeof(FileHeader) == 72, "FileHeader layout changed: bump kFileVersion");

// Vulkan's own header at the start of the data (VkPipelineCacheHeaderVersionOne).
constexpr size_t kVulkanHeaderSize = 16 + VK_UUID_SIZE;

uint64_t Fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

std::string Hex(const uint8_t* p, size_t n) {
  std::string s;
  char b[3];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(b, sizeof(b), "%02x", p[i]);
    s += b;
  }
  return s;
}

void Set(std::string* note, std::string text) {
  if (note) *note = std::move(text);
}

// How long a load or save waits for another device's save of the same file (a 150 MB save takes well
// under a second; the game's exit flush allows 3 s in all).
constexpr std::chrono::milliseconds kLockWait(2000);

std::string LockPath(const std::string& path) { return path + ".lock"; }

bool ReadFile(const std::string& path, std::vector<uint8_t>* file) {
  std::FILE* in = std::fopen(path.c_str(), "rb");
  if (!in) return false;
  uint8_t buffer[65536];
  size_t n = 0;
  while ((n = std::fread(buffer, 1, sizeof(buffer), in)) > 0) file->insert(file->end(), buffer, buffer + n);
  std::fclose(in);
  return true;
}

// The header of the file at `path`, when it has ours.
bool ReadHeader(const std::string& path, FileHeader* h) {
  std::FILE* in = std::fopen(path.c_str(), "rb");
  if (!in) return false;
  const bool ok = std::fread(h, 1, sizeof(*h), in) == sizeof(*h);
  std::fclose(in);
  return ok && h->magic == kFileMagic && h->version == kFileVersion && h->header_size == sizeof(*h);
}

}  // namespace

VulkanPipelineCache::Identity VulkanPipelineCache::QueryIdentity(VkPhysicalDevice physical_device) {
  VkPhysicalDeviceIDProperties ids = {};
  ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2 props = {};
  props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props.pNext = &ids;
  VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties2(physical_device, &props);
  Identity id;
  id.vendor_id = props.properties.vendorID;
  id.device_id = props.properties.deviceID;
  id.driver_version = props.properties.driverVersion;
  std::memcpy(id.pipeline_cache_uuid, props.properties.pipelineCacheUUID, VK_UUID_SIZE);
  std::memcpy(id.driver_uuid, ids.driverUUID, VK_UUID_SIZE);
  return id;
}

std::unique_ptr<VulkanPipelineCache> VulkanPipelineCache::Create(VkPhysicalDevice physical_device, VkDevice device) {
  std::unique_ptr<VulkanPipelineCache> cache(new VulkanPipelineCache());
  cache->device_ = device;
  cache->identity_ = QueryIdentity(physical_device);
  VkPipelineCacheCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreatePipelineCache(device, &info, nullptr, &cache->cache_) != VK_SUCCESS) {
    return nullptr;
  }
  return cache;
}

VulkanPipelineCache::~VulkanPipelineCache() {
  if (cache_) VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyPipelineCache(device_, cache_, nullptr);
}

std::vector<uint8_t> VulkanPipelineCache::MakeFile(const Identity& identity, const std::vector<uint8_t>& data) {
  FileHeader h;
  h.header_size = sizeof(FileHeader);
  h.vendor_id = identity.vendor_id;
  h.device_id = identity.device_id;
  h.driver_version = identity.driver_version;
  std::memcpy(h.pipeline_cache_uuid, identity.pipeline_cache_uuid, VK_UUID_SIZE);
  std::memcpy(h.driver_uuid, identity.driver_uuid, VK_UUID_SIZE);
  h.data_size = data.size();
  h.data_hash = Fnv1a(data.data(), data.size());
  std::vector<uint8_t> file(sizeof(h) + data.size());
  std::memcpy(file.data(), &h, sizeof(h));
  if (!data.empty()) std::memcpy(file.data() + sizeof(h), data.data(), data.size());
  return file;
}

DriverPipelineCache::Load VulkanPipelineCache::CheckFile(const std::vector<uint8_t>& file, const Identity& identity,
                                                         std::string* note, size_t* data_offset) {
  FileHeader h;
  if (file.size() < sizeof(h)) {
    Set(note, "shorter than its header");
    return Load::kDamaged;
  }
  std::memcpy(&h, file.data(), sizeof(h));
  if (h.magic != kFileMagic || h.version != kFileVersion || h.header_size != sizeof(h)) {
    Set(note, "not a driver pipeline cache file of this version");
    return Load::kDamaged;
  }
  // Made by another device or driver: say what changed.
  std::string changed;
  auto add = [&](const std::string& what) { changed += (changed.empty() ? "" : ", ") + what; };
  char text[96];
  if (h.vendor_id != identity.vendor_id || h.device_id != identity.device_id) {
    std::snprintf(text, sizeof(text), "GPU %04X:%04X, now %04X:%04X", h.vendor_id, h.device_id, identity.vendor_id,
                  identity.device_id);
    add(text);
  }
  if (h.driver_version != identity.driver_version) {
    std::snprintf(text, sizeof(text), "driver version %08X, now %08X", h.driver_version, identity.driver_version);
    add(text);
  }
  if (std::memcmp(h.driver_uuid, identity.driver_uuid, VK_UUID_SIZE) != 0) {
    add("driver UUID " + Hex(h.driver_uuid, VK_UUID_SIZE) + ", now " + Hex(identity.driver_uuid, VK_UUID_SIZE));
  }
  if (std::memcmp(h.pipeline_cache_uuid, identity.pipeline_cache_uuid, VK_UUID_SIZE) != 0) {
    add("pipeline cache UUID " + Hex(h.pipeline_cache_uuid, VK_UUID_SIZE) + ", now " +
        Hex(identity.pipeline_cache_uuid, VK_UUID_SIZE));
  }
  if (!changed.empty()) {
    Set(note, "made with " + changed);
    return Load::kDriverChanged;
  }
  if (h.data_size != file.size() - sizeof(h)) {
    Set(note, "cut short or too long for the size its header gives");
    return Load::kDamaged;
  }
  const uint8_t* data = file.data() + sizeof(h);
  if (Fnv1a(data, size_t(h.data_size)) != h.data_hash) {
    Set(note, "its data does not match its hash");
    return Load::kDamaged;
  }
  // Vulkan's header in the data: the driver checks it too, this says why.
  if (h.data_size < kVulkanHeaderSize) {
    Set(note, "the driver's data is shorter than Vulkan's header");
    return Load::kDamaged;
  }
  uint32_t vk[4];
  std::memcpy(vk, data, sizeof(vk));
  if (vk[0] < kVulkanHeaderSize || vk[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) {
    Set(note, "the driver's data has another header version");
    return Load::kDriverChanged;
  }
  if (vk[2] != identity.vendor_id || vk[3] != identity.device_id ||
      std::memcmp(data + 16, identity.pipeline_cache_uuid, VK_UUID_SIZE) != 0) {
    Set(note, "the driver's data names another GPU or pipeline cache UUID");
    return Load::kDriverChanged;
  }
  if (data_offset) *data_offset = sizeof(h);
  Set(note, std::to_string(h.data_size / 1024) + " KB");
  return Load::kLoaded;
}

DriverPipelineCache::Load VulkanPipelineCache::LoadFile(const std::string& path, std::string* note) {
  std::vector<uint8_t> file;
  {
    // Not while another device writes it (the rename is atomic, but a reader holding the file open
    // makes the writer's rename fail on Windows). Without the lock the file is read anyway.
    FileLock lock;
    if (std::filesystem::exists(path)) lock.Lock(LockPath(path), kLockWait);
    if (!ReadFile(path, &file)) {
      Set(note, "no file yet");
      return Load::kAbsent;
    }
  }
  size_t offset = 0;
  const Load check = CheckFile(file, identity_, note, &offset);
  if (check != Load::kLoaded) return check;
  VkPipelineCacheCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  info.initialDataSize = file.size() - offset;
  info.pInitialData = file.data() + offset;
  VkPipelineCache loaded = VK_NULL_HANDLE;
  if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreatePipelineCache(device_, &info, nullptr, &loaded) != VK_SUCCESS) {
    Set(note, "vkCreatePipelineCache refused the data");
    return Load::kRefused;
  }
  const VkResult merged = VULKAN_HPP_DEFAULT_DISPATCHER.vkMergePipelineCaches(device_, cache_, 1, &loaded);
  VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyPipelineCache(device_, loaded, nullptr);
  if (merged != VK_SUCCESS) {
    Set(note, "vkMergePipelineCaches failed");
    return Load::kRefused;
  }
  {
    std::lock_guard save_lock(save_mutex_);
    Remember(file);
  }
  return Load::kLoaded;
}

void VulkanPipelineCache::Remember(const std::vector<uint8_t>& file) {
  FileHeader h;
  std::memcpy(&h, file.data(), sizeof(h));
  known_ = true;
  known_size_ = h.data_size;
  known_hash_ = h.data_hash;
}

std::vector<uint8_t> VulkanPipelineCache::Data() { return DataOf(cache_); }

std::vector<uint8_t> VulkanPipelineCache::DataOf(VkPipelineCache cache) {
  std::vector<uint8_t> data;
  // The cache may grow between the two calls (workers keep creating pipelines): try again.
  for (int attempt = 0; attempt < 4; ++attempt) {
    size_t size = 0;
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPipelineCacheData(device_, cache, &size, nullptr) != VK_SUCCESS) return {};
    data.resize(size);
    const VkResult r = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPipelineCacheData(device_, cache, &size, data.data());
    if (r == VK_SUCCESS) {
      data.resize(size);
      return data;
    }
    if (r != VK_INCOMPLETE) return {};
  }
  return {};
}

bool VulkanPipelineCache::MergeFile(const std::string& path, std::vector<uint8_t>* data, std::string* note) {
  std::vector<uint8_t> file;
  size_t offset = 0;
  std::string why;
  if (!ReadFile(path, &file) || CheckFile(file, identity_, &why, &offset) != Load::kLoaded) {
    Set(note, "not merged (" + (why.empty() ? std::string("unreadable") : why) + ")");
    return false;  // another driver's or a damaged file: replaced
  }
  // A cache of its own made from the file, this cache merged into it (only the destination of a merge
  // must not be used meanwhile: the live cache keeps serving pipeline creation).
  VkPipelineCacheCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  info.initialDataSize = file.size() - offset;
  info.pInitialData = file.data() + offset;
  VkPipelineCache both = VK_NULL_HANDLE;
  if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreatePipelineCache(device_, &info, nullptr, &both) != VK_SUCCESS) {
    Set(note, "not merged (vkCreatePipelineCache refused the file)");
    return false;
  }
  std::vector<uint8_t> merged;
  if (VULKAN_HPP_DEFAULT_DISPATCHER.vkMergePipelineCaches(device_, both, 1, &cache_) == VK_SUCCESS) {
    merged = DataOf(both);
  }
  VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyPipelineCache(device_, both, nullptr);
  if (merged.size() < kVulkanHeaderSize) {
    Set(note, "not merged (vkMergePipelineCaches failed)");
    return false;
  }
  Set(note, "merged with another device's save of " + std::to_string((file.size() - offset) / 1024) + " KB");
  *data = std::move(merged);
  return true;
}

bool VulkanPipelineCache::SaveFile(const std::string& path, size_t* bytes, std::string* note) {
  std::lock_guard lock(save_mutex_);
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  // One save of this file at a time across devices and processes (task 185).
  FileLock file_lock;
  if (!file_lock.Lock(LockPath(path), kLockWait)) {
    Set(note, "another device or process kept the file locked (" + LockPath(path) + "): tried again later");
    return false;
  }
  std::vector<uint8_t> data = Data();
  if (data.size() < kVulkanHeaderSize) {
    if (bytes) *bytes = data.size();
    Set(note, "the driver gave no data");
    return false;
  }
  // Another device saved since this cache last loaded or saved the file: keep its pipelines too.
  std::string merge_note;
  FileHeader on_disk;
  if (ReadHeader(path, &on_disk) &&
      (merged_ || !known_ || on_disk.data_size != known_size_ || on_disk.data_hash != known_hash_)) {
    if (MergeFile(path, &data, &merge_note)) merged_ = true;
  }
  if (bytes) *bytes = data.size();
  const std::vector<uint8_t> file = MakeFile(identity_, data);
  // Only the lock holder writes the temporary file, so its name can be fixed (a save cut off by the
  // end of the process leaves one temporary file, replaced by the next save).
  const std::string temp = path + ".tmp";
  std::FILE* out = std::fopen(temp.c_str(), "wb");
  if (!out) {
    Set(note, "cannot create " + temp);
    return false;
  }
  bool ok = std::fwrite(file.data(), 1, file.size(), out) == file.size();
  ok = std::fflush(out) == 0 && ok;
  ok = std::fclose(out) == 0 && ok;
  if (ok) {
    // An atomic replace. A reader of an older build (or a virus scanner) holding the file open can make
    // it fail for a moment on Windows: a few tries.
    for (int attempt = 0; attempt < 5; ++attempt) {
      std::filesystem::rename(temp, p, ec);
      if (!ec) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ok = !ec;
  }
  if (!ok) {
    std::filesystem::remove(temp, ec);
    Set(note, "cannot write " + path);
    return false;
  }
  Remember(file);
  Set(note, std::to_string(data.size() / 1024) + " KB" + (merge_note.empty() ? "" : ", " + merge_note));
  return true;
}

}  // namespace nr
