// A small Vulkan harness for running translated shaders on the CPU driver (lavapipe) or any
// Vulkan 1.3 device: buffers, images, samplers, and one pipeline layout with the descriptor
// sets the shader ABI uses (abi.h / kk_common.hlsli), plus set 7 for the harness itself:
//
//   set 0  b0 b1 b2 uniform buffers (vertex constants, pixel constants, draw constants)
//   set 1  Texture2D[kTex2D]       set 2  Texture3D[kTex3D]     set 3  TextureCube[kTexCube]
//   set 4  Texture2DArray[kTex2DArray]   set 5  SamplerState[kSamplers]
//   set 6  ByteAddressBuffer[kBuffers]   set 7  binding 0: the harness's storage buffer
//
// Every array entry is written (unused ones point at small dummies), so no
// partially-bound descriptors are needed; robustBufferAccess keeps out-of-range vertex
// fetches of random programs inside the buffers.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace vkh {

constexpr uint32_t kTex2D = 16, kTex3D = 8, kTexCube = 8, kTex2DArray = 4, kSamplers = 32, kBuffers = 16;

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
    uint32_t width = 1, height = 1, depth = 1, layers = 1;
};

// The descriptors of one run.
struct Bindings {
    VkBuffer constants[3] = {};                 // b0 b1 b2
    VkDeviceSize constantSizes[3] = {};
    std::vector<VkImageView> tex2D, tex3D, texCube, tex2DArray;  // up to the array sizes (rest: dummies)
    std::vector<VkSampler> samplers;
    std::vector<VkBuffer> buffers;
    VkBuffer harness = VK_NULL_HANDLE;
    VkDeviceSize harnessSize = 0;
};

class Gpu {
public:
    Gpu() = default;
    ~Gpu();
    Gpu(const Gpu&) = delete;
    Gpu& operator=(const Gpu&) = delete;

    bool init(std::string* error);
    const std::string& deviceName() const { return deviceName_; }
    bool validation() const { return validation_; }
    int validationErrors() const;

    Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage);
    void destroy(Buffer& b);
    Image image(VkImageViewType type, VkFormat format, uint32_t width, uint32_t height, uint32_t depth, uint32_t layers,
                VkImageUsageFlags usage);
    void destroy(Image& i);
    // Tightly packed texels of every layer (and depth slice), RGBA32F; leaves the image in
    // SHADER_READ_ONLY_OPTIMAL.
    bool upload(Image& image, const void* data, size_t bytes);
    // Copies a 2D colour image (in COLOR_ATTACHMENT_OPTIMAL) to host memory.
    bool download(Image& image, void* out, size_t bytes);
    VkSampler sampler(bool linear, bool clamp);  // cached
    VkShaderModule shader(const std::vector<uint8_t>& spirv);

    VkPipeline computePipeline(VkShaderModule module, const char* entry);
    // A point-list pipeline drawing into `colorCount` RGBA32F targets with the given write
    // masks (dynamic rendering, viewport and scissor dynamic).
    VkPipeline pointPipeline(VkShaderModule vs, const char* vsEntry, VkShaderModule ps, const char* psEntry,
                             const std::vector<VkColorComponentFlags>& writeMasks);
    void destroy(VkPipeline p);
    void destroy(VkShaderModule m);

    // Records `record` with the run's descriptor sets bound, submits and waits (false on a
    // device loss or a timeout).
    bool run(const Bindings& bindings, VkPipelineBindPoint bindPoint, const std::function<void(VkCommandBuffer)>& record,
             uint64_t timeoutNs = 60'000'000'000ull);

    VkDevice device() const { return device_; }
    VkPipelineLayout layout() const { return layout_; }

private:
    bool oneShot(const std::function<void(VkCommandBuffer)>& record, uint64_t timeoutNs = 60'000'000'000ull);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayouts_[8] = {};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::string deviceName_;
    bool validation_ = false;
    VkPhysicalDeviceMemoryProperties memoryProperties_{};
    std::vector<std::pair<uint32_t, VkSampler>> samplers_;
    // Dummies for unused array entries.
    Image dummy2D_, dummy3D_, dummyCube_, dummy2DArray_;
    Buffer dummyBuffer_;
    bool lost_ = false;
};

}  // namespace vkh
