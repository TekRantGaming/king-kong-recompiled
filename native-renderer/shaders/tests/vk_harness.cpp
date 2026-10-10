#include "vk_harness.h"

#include <cstdio>
#include <cstring>

namespace vkh {

namespace {

int gValidationErrors = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++gValidationErrors;
        std::fprintf(stderr, "  [vulkan validation] %s\n", data->pMessage);
    }
    return VK_FALSE;
}

#define VKH_CHECK(call)                                                        \
    do {                                                                       \
        VkResult vkh_r = (call);                                               \
        if (vkh_r != VK_SUCCESS) {                                             \
            if (error) *error = std::string(#call) + " = " + std::to_string(vkh_r); \
            return false;                                                      \
        }                                                                      \
    } while (0)

}  // namespace

int Gpu::validationErrors() const { return gValidationErrors; }

bool Gpu::init(std::string* error) {
    // Instance (with the Khronos validation layer when installed).
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    std::vector<const char*> enabledLayers;
    for (const auto& l : layers)
        if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
            enabledLayers.push_back("VK_LAYER_KHRONOS_validation");
            validation_ = true;
        }
    std::vector<const char*> extensions;
    uint32_t extCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, exts.data());
    bool debugUtils = false;
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) debugUtils = true;
    if (debugUtils) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "kkshaders_gpu_tests";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = uint32_t(enabledLayers.size());
    ici.ppEnabledLayerNames = enabledLayers.data();
    ici.enabledExtensionCount = uint32_t(extensions.size());
    ici.ppEnabledExtensionNames = extensions.data();
    VKH_CHECK(vkCreateInstance(&ici, nullptr, &instance_));
    if (debugUtils) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        mci.pfnUserCallback = debugCallback;
        if (create) create(instance_, &mci, nullptr, &messenger_);
    }

    // Device: lavapipe first, else the first Vulkan 1.3 device.
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    for (int pass = 0; pass < 2 && !physical_; pass++) {
        for (auto d : devices) {
            VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
            VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            p.pNext = &p12;
            vkGetPhysicalDeviceProperties2(d, &p);
            if (p.properties.apiVersion < VK_API_VERSION_1_3) continue;
            if (pass == 0 && p12.driverID != VK_DRIVER_ID_MESA_LLVMPIPE) continue;
            physical_ = d;
            deviceName_ = p.properties.deviceName;
            break;
        }
    }
    if (!physical_) {
        if (error) *error = "no Vulkan 1.3 device";
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(physical_, &memoryProperties_);
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qCount, families.data());
    for (uint32_t i = 0; i < qCount; i++)
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            family_ = i;
            break;
        }

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(physical_, &f2);
    if (!f12.runtimeDescriptorArray || !f13.dynamicRendering || !f2.features.robustBufferAccess) {
        if (error) *error = deviceName_ + " lacks runtimeDescriptorArray, dynamicRendering or robustBufferAccess";
        return false;
    }
    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.runtimeDescriptorArray = VK_TRUE;
    e12.pNext = &e13;
    VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    e2.features.robustBufferAccess = VK_TRUE;
    e2.features.shaderSampledImageArrayDynamicIndexing = f2.features.shaderSampledImageArrayDynamicIndexing;
    e2.features.shaderStorageBufferArrayDynamicIndexing = f2.features.shaderStorageBufferArrayDynamicIndexing;
    e2.features.shaderUniformBufferArrayDynamicIndexing = f2.features.shaderUniformBufferArrayDynamicIndexing;
    e2.features.largePoints = f2.features.largePoints;
    e2.features.independentBlend = f2.features.independentBlend;
    e2.pNext = &e12;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &e2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VKH_CHECK(vkCreateDevice(physical_, &dci, nullptr, &device_));
    vkGetDeviceQueue(device_, family_, 0, &queue_);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family_;
    VKH_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &pool_));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKH_CHECK(vkCreateFence(device_, &fci, nullptr, &fence_));

    // Descriptor set layouts.
    auto setLayout = [&](std::vector<VkDescriptorSetLayoutBinding> b, VkDescriptorSetLayout* out) {
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = uint32_t(b.size());
        lci.pBindings = b.data();
        return vkCreateDescriptorSetLayout(device_, &lci, nullptr, out);
    };
    auto binding = [](uint32_t index, VkDescriptorType type, uint32_t count) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = index;
        b.descriptorType = type;
        b.descriptorCount = count;
        b.stageFlags = VK_SHADER_STAGE_ALL;
        return b;
    };
    VKH_CHECK(setLayout({binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1), binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1),
                         binding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1)},
                        &setLayouts_[0]));
    VKH_CHECK(setLayout({binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kTex2D)}, &setLayouts_[1]));
    VKH_CHECK(setLayout({binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kTex3D)}, &setLayouts_[2]));
    VKH_CHECK(setLayout({binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kTexCube)}, &setLayouts_[3]));
    VKH_CHECK(setLayout({binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kTex2DArray)}, &setLayouts_[4]));
    VKH_CHECK(setLayout({binding(0, VK_DESCRIPTOR_TYPE_SAMPLER, kSamplers)}, &setLayouts_[5]));
    VKH_CHECK(setLayout({binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBuffers)}, &setLayouts_[6]));
    VKH_CHECK(setLayout({binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1)}, &setLayouts_[7]));
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 8;
    plci.pSetLayouts = setLayouts_;
    VKH_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &layout_));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kTex2D + kTex3D + kTexCube + kTex2DArray},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, kSamplers},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBuffers + 1}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets = 8;
    dpci.poolSizeCount = 4;
    dpci.pPoolSizes = sizes;
    VKH_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &descriptorPool_));

    // Dummies.
    const VkImageUsageFlags sampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    dummy2D_ = image(VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 1, 1, sampled);
    dummy3D_ = image(VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 1, 1, sampled);
    dummyCube_ = image(VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 1, 6, sampled);
    dummy2DArray_ = image(VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 1, 1, sampled);
    float zero[4 * 6] = {};
    if (!upload(dummy2D_, zero, 16) || !upload(dummy3D_, zero, 16) || !upload(dummyCube_, zero, 16 * 6) ||
        !upload(dummy2DArray_, zero, 16)) {
        if (error) *error = "dummy textures";
        return false;
    }
    dummyBuffer_ = buffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    return true;
}

Gpu::~Gpu() {
    if (!device_) {
        if (instance_) vkDestroyInstance(instance_, nullptr);
        return;
    }
    vkDeviceWaitIdle(device_);
    destroy(dummy2D_);
    destroy(dummy3D_);
    destroy(dummyCube_);
    destroy(dummy2DArray_);
    destroy(dummyBuffer_);
    for (auto& [key, s] : samplers_) vkDestroySampler(device_, s, nullptr);
    vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    for (auto l : setLayouts_) vkDestroyDescriptorSetLayout(device_, l, nullptr);
    vkDestroyFence(device_, fence_, nullptr);
    vkDestroyCommandPool(device_, pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
    if (messenger_) {
        auto destroyMessenger =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger) destroyMessenger(instance_, messenger_, nullptr);
    }
    vkDestroyInstance(instance_, nullptr);
}

uint32_t Gpu::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memoryProperties_.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memoryProperties_.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

Buffer Gpu::buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    if (vkCreateBuffer(device_, &bci, nullptr, &b.buffer) != VK_SUCCESS) return {};
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device_, &mai, nullptr, &b.memory);
    vkBindBufferMemory(device_, b.buffer, b.memory, 0);
    vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
    std::memset(b.mapped, 0, size_t(size));
    return b;
}

void Gpu::destroy(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
    b = {};
}

Image Gpu::image(VkImageViewType type, VkFormat format, uint32_t width, uint32_t height, uint32_t depth, uint32_t layers,
                 VkImageUsageFlags usage) {
    Image im;
    im.format = format;
    im.viewType = type;
    im.width = width;
    im.height = height;
    im.depth = depth;
    im.layers = layers;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, depth};
    ici.mipLevels = 1;
    ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    if (type == VK_IMAGE_VIEW_TYPE_CUBE) ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (vkCreateImage(device_, &ici, nullptr, &im.image) != VK_SUCCESS) return {};
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, im.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType(req.memoryTypeBits, 0);
    vkAllocateMemory(device_, &mai, nullptr, &im.memory);
    vkBindImageMemory(device_, im.image, im.memory, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im.image;
    vci.viewType = type;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCreateImageView(device_, &vci, nullptr, &im.view);
    return im;
}

void Gpu::destroy(Image& i) {
    if (i.view) vkDestroyImageView(device_, i.view, nullptr);
    if (i.image) vkDestroyImage(device_, i.image, nullptr);
    if (i.memory) vkFreeMemory(device_, i.memory, nullptr);
    i = {};
}

bool Gpu::oneShot(const std::function<void(VkCommandBuffer)>& record, uint64_t timeoutNs) {
    if (lost_) return false;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(device_, &cai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    record(cb);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkResetFences(device_, 1, &fence_);
    bool ok = vkQueueSubmit(queue_, 1, &si, fence_) == VK_SUCCESS && vkWaitForFences(device_, 1, &fence_, VK_TRUE, timeoutNs) == VK_SUCCESS;
    if (!ok) {
        lost_ = true;
        return false;
    }
    vkFreeCommandBuffers(device_, pool_, 1, &cb);
    return true;
}

static void barrier(VkCommandBuffer cb, VkImage image, uint32_t layers, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                    VkAccessFlags dst, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool Gpu::upload(Image& image, const void* data, size_t bytes) {
    Buffer staging = buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped, data, bytes);
    bool ok = oneShot([&](VkCommandBuffer cb) {
        barrier(cb, image.image, image.layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, image.layers};
        c.imageExtent = {image.width, image.height, image.depth};
        vkCmdCopyBufferToImage(cb, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(cb, image.image, image.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    });
    destroy(staging);
    return ok;
}

bool Gpu::download(Image& image, void* out, size_t bytes) {
    Buffer staging = buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    bool ok = oneShot([&](VkCommandBuffer cb) {
        barrier(cb, image.image, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {image.width, image.height, 1};
        vkCmdCopyImageToBuffer(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &c);
        barrier(cb, image.image, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    });
    if (ok) std::memcpy(out, staging.mapped, bytes);
    destroy(staging);
    return ok;
}

VkSampler Gpu::sampler(bool linear, bool clamp) {
    uint32_t key = (linear ? 1u : 0u) | (clamp ? 2u : 0u);
    for (auto& [k, s] : samplers_)
        if (k == key) return s;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW =
        clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 0.0f;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(device_, &sci, nullptr, &s);
    samplers_.push_back({key, s});
    return s;
}

VkShaderModule Gpu::shader(const std::vector<uint8_t>& spirv) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device_, &ci, nullptr, &m);
    return m;
}

VkPipeline Gpu::computePipeline(VkShaderModule module, const char* entry) {
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = entry;
    ci.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
    return p;
}

VkPipeline Gpu::pointPipeline(VkShaderModule vs, const char* vsEntry, VkShaderModule ps, const char* psEntry,
                              const std::vector<VkColorComponentFlags>& writeMasks) {
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = vsEntry;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = psEntry;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    std::vector<VkPipelineColorBlendAttachmentState> blends(writeMasks.size());
    for (size_t i = 0; i < writeMasks.size(); i++) blends[i].colorWriteMask = writeMasks[i];
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = uint32_t(blends.size());
    cb.pAttachments = blends.data();
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    std::vector<VkFormat> formats(writeMasks.size(), VK_FORMAT_R32G32B32A32_SFLOAT);
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = uint32_t(formats.size());
    ri.pColorAttachmentFormats = formats.data();
    VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gci.pNext = &ri;
    gci.stageCount = 2;
    gci.pStages = stages;
    gci.pVertexInputState = &vi;
    gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vp;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms;
    gci.pColorBlendState = &cb;
    gci.pDynamicState = &ds;
    gci.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gci, nullptr, &p);
    return p;
}

void Gpu::destroy(VkPipeline p) {
    if (p) vkDestroyPipeline(device_, p, nullptr);
}

void Gpu::destroy(VkShaderModule m) {
    if (m) vkDestroyShaderModule(device_, m, nullptr);
}

bool Gpu::run(const Bindings& b, VkPipelineBindPoint bindPoint, const std::function<void(VkCommandBuffer)>& record, uint64_t timeoutNs) {
    if (lost_) return false;
    VkDescriptorSet sets[8];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descriptorPool_;
    ai.descriptorSetCount = 8;
    ai.pSetLayouts = setLayouts_;
    if (vkAllocateDescriptorSets(device_, &ai, sets) != VK_SUCCESS) return false;

    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> bufferInfos;
    std::vector<VkDescriptorImageInfo> imageInfos;
    bufferInfos.reserve(256);
    imageInfos.reserve(256);
    auto write = [&](VkDescriptorSet set, uint32_t binding, uint32_t element, VkDescriptorType type) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding;
        w.dstArrayElement = element;
        w.descriptorCount = 1;
        w.descriptorType = type;
        return w;
    };
    for (uint32_t i = 0; i < 3; i++) {
        bufferInfos.push_back({b.constants[i], 0, b.constantSizes[i]});
        VkWriteDescriptorSet w = write(sets[0], i, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        w.pBufferInfo = &bufferInfos.back();
        writes.push_back(w);
    }
    auto images = [&](uint32_t set, const std::vector<VkImageView>& views, uint32_t count, VkImageView dummy) {
        for (uint32_t i = 0; i < count; i++) {
            imageInfos.push_back({VK_NULL_HANDLE, i < views.size() && views[i] ? views[i] : dummy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            VkWriteDescriptorSet w = write(sets[set], 0, i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            w.pImageInfo = &imageInfos.back();
            writes.push_back(w);
        }
    };
    images(1, b.tex2D, kTex2D, dummy2D_.view);
    images(2, b.tex3D, kTex3D, dummy3D_.view);
    images(3, b.texCube, kTexCube, dummyCube_.view);
    images(4, b.tex2DArray, kTex2DArray, dummy2DArray_.view);
    for (uint32_t i = 0; i < kSamplers; i++) {
        imageInfos.push_back({i < b.samplers.size() && b.samplers[i] ? b.samplers[i] : sampler(false, false), VK_NULL_HANDLE,
                              VK_IMAGE_LAYOUT_UNDEFINED});
        VkWriteDescriptorSet w = write(sets[5], 0, i, VK_DESCRIPTOR_TYPE_SAMPLER);
        w.pImageInfo = &imageInfos.back();
        writes.push_back(w);
    }
    for (uint32_t i = 0; i < kBuffers; i++) {
        bool real = i < b.buffers.size() && b.buffers[i];
        bufferInfos.push_back({real ? b.buffers[i] : dummyBuffer_.buffer, 0, VK_WHOLE_SIZE});
        VkWriteDescriptorSet w = write(sets[6], 0, i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        w.pBufferInfo = &bufferInfos.back();
        writes.push_back(w);
    }
    bufferInfos.push_back({b.harness ? b.harness : dummyBuffer_.buffer, 0, VK_WHOLE_SIZE});
    {
        VkWriteDescriptorSet w = write(sets[7], 0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        w.pBufferInfo = &bufferInfos.back();
        writes.push_back(w);
    }
    vkUpdateDescriptorSets(device_, uint32_t(writes.size()), writes.data(), 0, nullptr);

    bool ok = oneShot(
        [&](VkCommandBuffer cb) {
            vkCmdBindDescriptorSets(cb, bindPoint, layout_, 0, 8, sets, 0, nullptr);
            record(cb);
        },
        timeoutNs);
    if (ok) vkFreeDescriptorSets(device_, descriptorPool_, 8, sets);
    return ok;
}

}  // namespace vkh
