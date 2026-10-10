#include "vk_runner.h"

#include <cstring>

#include <vulkan/vulkan.h>

namespace {

constexpr uint32_t kTargets = 4;
constexpr uint32_t kArray = 8;  // descriptors per bindless array in this harness
constexpr uint32_t kSets = 7;

}  // namespace

struct VkRunner::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkPhysicalDeviceMemoryProperties memory{};
    std::string name;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkImage images[kTargets]{};
    VkDeviceMemory imageMemory[kTargets]{};
    VkImageView views[kTargets]{};
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayouts[kSets]{};
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkSampler samplers[2]{};

    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
        for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        return UINT32_MAX;
    }

    Buffer makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
        Buffer b;
        b.size = size < 16 ? 16 : size;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = b.size;
        ci.usage = usage;
        vkCreateBuffer(device, &ci, nullptr, &b.buffer);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.buffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &ai, nullptr, &b.memory);
        vkBindBufferMemory(device, b.buffer, b.memory, 0);
        vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
        std::memset(b.mapped, 0, size_t(b.size));
        return b;
    }

    void destroy(Buffer& b) {
        if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(device, b.memory, nullptr);
        b = {};
    }

    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    Image makeImage(uint32_t w, uint32_t h, VkImageUsageFlags usage) {
        Image img;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(device, &ci, nullptr, &img.image);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, img.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = memoryType(req.memoryTypeBits, 0);
        vkAllocateMemory(device, &ai, nullptr, &img.memory);
        vkBindImageMemory(device, img.image, img.memory, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device, &vi, nullptr, &img.view);
        return img;
    }

    void destroy(Image& i) {
        if (i.view) vkDestroyImageView(device, i.view, nullptr);
        if (i.image) vkDestroyImage(device, i.image, nullptr);
        if (i.memory) vkFreeMemory(device, i.memory, nullptr);
        i = {};
    }

    void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                 VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
};

VkRunner::VkRunner() : impl_(std::make_unique<Impl>()) {}

VkRunner::~VkRunner() {
    Impl& m = *impl_;
    if (m.device) {
        vkDeviceWaitIdle(m.device);
        for (auto s : m.samplers)
            if (s) vkDestroySampler(m.device, s, nullptr);
        if (m.commandPool) vkDestroyCommandPool(m.device, m.commandPool, nullptr);
        if (m.pipelineLayout) vkDestroyPipelineLayout(m.device, m.pipelineLayout, nullptr);
        for (auto l : m.setLayouts)
            if (l) vkDestroyDescriptorSetLayout(m.device, l, nullptr);
        if (m.framebuffer) vkDestroyFramebuffer(m.device, m.framebuffer, nullptr);
        for (uint32_t i = 0; i < kTargets; i++) {
            if (m.views[i]) vkDestroyImageView(m.device, m.views[i], nullptr);
            if (m.images[i]) vkDestroyImage(m.device, m.images[i], nullptr);
            if (m.imageMemory[i]) vkFreeMemory(m.device, m.imageMemory[i], nullptr);
        }
        if (m.renderPass) vkDestroyRenderPass(m.device, m.renderPass, nullptr);
        vkDestroyDevice(m.device, nullptr);
    }
    if (m.instance) vkDestroyInstance(m.instance, nullptr);
}

std::string VkRunner::deviceName() const { return impl_->name; }

bool VkRunner::init(std::string* error) {
    Impl& m = *impl_;
    auto fail = [&](const char* what) {
        if (error) *error = what;
        return false;
    };
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "kkshaders_exec_tests";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, nullptr, &m.instance) != VK_SUCCESS) return fail("vkCreateInstance failed");
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m.instance, &count, nullptr);
    if (!count) return fail("no Vulkan device");
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m.instance, &count, devices.data());
    m.physical = devices[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m.physical, &props);
    m.name = props.deviceName;
    vkGetPhysicalDeviceMemoryProperties(m.physical, &m.memory);

    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m.physical, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> families(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(m.physical, &qcount, families.data());
    m.queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < qcount; i++)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            m.queueFamily = i;
            break;
        }
    if (m.queueFamily == UINT32_MAX) return fail("no graphics queue");

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = m.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    f2.features.shaderClipDistance = VK_TRUE;
    f2.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    f2.features.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
    f2.features.independentBlend = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(m.physical, &dci, nullptr, &m.device) != VK_SUCCESS) return fail("vkCreateDevice failed");
    vkGetDeviceQueue(m.device, m.queueFamily, 0, &m.queue);

    // Render pass and targets.
    VkAttachmentDescription att[kTargets];
    VkAttachmentReference refs[kTargets];
    for (uint32_t i = 0; i < kTargets; i++) {
        att[i] = {};
        att[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        att[i].samples = VK_SAMPLE_COUNT_1_BIT;
        att[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[i].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        refs[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = kTargets;
    sub.pColorAttachments = refs;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = kTargets;
    rpi.pAttachments = att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    if (vkCreateRenderPass(m.device, &rpi, nullptr, &m.renderPass) != VK_SUCCESS) return fail("render pass");
    for (uint32_t i = 0; i < kTargets; i++) {
        Impl::Image img = m.makeImage(1, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        m.images[i] = img.image;
        m.imageMemory[i] = img.memory;
        m.views[i] = img.view;
    }
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = m.renderPass;
    fci.attachmentCount = kTargets;
    fci.pAttachments = m.views;
    fci.width = fci.height = 1;
    fci.layers = 1;
    if (vkCreateFramebuffer(m.device, &fci, nullptr, &m.framebuffer) != VK_SUCCESS) return fail("framebuffer");

    // Descriptor set layouts: the kkshaders binding model (abi.h).
    {
        VkDescriptorSetLayoutBinding b[3];
        for (uint32_t i = 0; i < 3; i++) b[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 3;
        ci.pBindings = b;
        vkCreateDescriptorSetLayout(m.device, &ci, nullptr, &m.setLayouts[0]);
    }
    VkDescriptorType arrayTypes[kSets] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER,
                                          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    for (uint32_t s = 1; s < kSets; s++) {
        VkDescriptorSetLayoutBinding b{0, arrayTypes[s], kArray, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr};
        VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo fi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        fi.bindingCount = 1;
        fi.pBindingFlags = &flags;
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.pNext = &fi;
        ci.bindingCount = 1;
        ci.pBindings = &b;
        vkCreateDescriptorSetLayout(m.device, &ci, nullptr, &m.setLayouts[s]);
    }
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = kSets;
    pli.pSetLayouts = m.setLayouts;
    if (vkCreatePipelineLayout(m.device, &pli, nullptr, &m.pipelineLayout) != VK_SUCCESS) return fail("pipeline layout");

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = m.queueFamily;
    vkCreateCommandPool(m.device, &cpi, nullptr, &m.commandPool);

    for (int i = 0; i < 2; i++) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.0f;
        vkCreateSampler(m.device, &si, nullptr, &m.samplers[i]);
    }
    return true;
}

VkRunner::Result VkRunner::run(const Draw& draw) {
    Impl& m = *impl_;
    Result result;
    std::vector<Impl::Buffer> buffers;
    std::vector<Impl::Image> textures;
    VkShaderModule modules[2]{};
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    auto cleanup = [&] {
        if (fence) vkDestroyFence(m.device, fence, nullptr);
        if (cb) vkFreeCommandBuffers(m.device, m.commandPool, 1, &cb);
        if (pool) vkDestroyDescriptorPool(m.device, pool, nullptr);
        if (pipeline) vkDestroyPipeline(m.device, pipeline, nullptr);
        for (auto mod : modules)
            if (mod) vkDestroyShaderModule(m.device, mod, nullptr);
        for (auto& b : buffers) m.destroy(b);
        for (auto& t : textures) m.destroy(t);
    };

    const std::vector<uint8_t>* code[2] = {&draw.vs, &draw.ps};
    for (int i = 0; i < 2; i++) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code[i]->size();
        ci.pCode = reinterpret_cast<const uint32_t*>(code[i]->data());
        if (vkCreateShaderModule(m.device, &ci, nullptr, &modules[i]) != VK_SUCCESS) {
            result.error = "shader module";
            cleanup();
            return result;
        }
    }

    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = modules[0];
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = modules[1];
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, 1, 1, 0, 1};
    VkRect2D scissor{{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    vp.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend[kTargets]{};
    for (auto& b : blend) b.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = kTargets;
    cbs.pAttachments = blend;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vin;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &ds;
    gpi.pColorBlendState = &cbs;
    gpi.layout = m.pipelineLayout;
    gpi.renderPass = m.renderPass;
    if (vkCreateGraphicsPipelines(m.device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline) != VK_SUCCESS) {
        result.error = "pipeline creation failed";
        cleanup();
        return result;
    }

    // Constant buffers and storage buffers.
    const std::vector<uint8_t>* cbData[3] = {&draw.vertexConstants, &draw.pixelConstants, &draw.drawConstants};
    for (int i = 0; i < 3; i++) {
        Impl::Buffer b = m.makeBuffer(4096, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        std::memcpy(b.mapped, cbData[i]->data(), std::min<size_t>(cbData[i]->size(), 4096));
        buffers.push_back(b);
    }
    size_t firstStorage = buffers.size();
    for (size_t i = 0; i < kArray; i++) {
        size_t size = i < draw.buffers.size() ? draw.buffers[i].size() : 16;
        Impl::Buffer b = m.makeBuffer((size + 15) & ~size_t(15), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (i < draw.buffers.size()) std::memcpy(b.mapped, draw.buffers[i].data(), draw.buffers[i].size());
        buffers.push_back(b);
    }

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = m.commandPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(m.device, &cai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    // Textures: uploaded through a staging buffer in the same command buffer.
    size_t stagingStart = buffers.size();
    for (size_t t = 0; t < draw.textures.size() && t < kArray; t++) {
        const auto& tex = draw.textures[t];
        Impl::Image img = m.makeImage(tex.width, tex.height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        textures.push_back(img);
        Impl::Buffer staging = m.makeBuffer(tex.texels.size() * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memcpy(staging.mapped, tex.texels.data(), tex.texels.size() * 4);
        buffers.push_back(staging);
        m.barrier(cb, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {tex.width, tex.height, 1};
        vkCmdCopyBufferToImage(cb, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        m.barrier(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    (void)stagingStart;

    // Descriptor sets.
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 * kArray},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, kArray},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kArray}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = kSets;
    dpi.poolSizeCount = 4;
    dpi.pPoolSizes = sizes;
    vkCreateDescriptorPool(m.device, &dpi, nullptr, &pool);
    VkDescriptorSet sets[kSets];
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool;
    dai.descriptorSetCount = kSets;
    dai.pSetLayouts = m.setLayouts;
    vkAllocateDescriptorSets(m.device, &dai, sets);

    std::vector<VkWriteDescriptorSet> writes;
    VkDescriptorBufferInfo cbInfo[3];
    for (uint32_t i = 0; i < 3; i++) {
        cbInfo[i] = {buffers[i].buffer, 0, 4096};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets[0];
        w.dstBinding = i;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &cbInfo[i];
        writes.push_back(w);
    }
    VkDescriptorBufferInfo sbInfo[kArray];
    for (uint32_t i = 0; i < kArray; i++) sbInfo[i] = {buffers[firstStorage + i].buffer, 0, VK_WHOLE_SIZE};
    {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets[6];
        w.dstBinding = 0;
        w.descriptorCount = kArray;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = sbInfo;
        writes.push_back(w);
    }
    VkDescriptorImageInfo samplerInfo[2] = {{m.samplers[0], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
                                            {m.samplers[1], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}};
    {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets[5];
        w.dstBinding = 0;
        w.descriptorCount = 2;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.pImageInfo = samplerInfo;
        writes.push_back(w);
    }
    std::vector<VkDescriptorImageInfo> texInfo;
    texInfo.reserve(textures.size());
    for (auto& t : textures) texInfo.push_back({VK_NULL_HANDLE, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    if (!texInfo.empty()) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets[1];
        w.dstBinding = 0;
        w.descriptorCount = uint32_t(texInfo.size());
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = texInfo.data();
        writes.push_back(w);
    }
    vkUpdateDescriptorSets(m.device, uint32_t(writes.size()), writes.data(), 0, nullptr);

    VkClearValue clears[kTargets];
    for (auto& c : clears) c.color = {{kClear, kClear, kClear, kClear}};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = m.renderPass;
    rbi.framebuffer = m.framebuffer;
    rbi.renderArea = {{0, 0}, {1, 1}};
    rbi.clearValueCount = kTargets;
    rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m.pipelineLayout, 0, kSets, sets, 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);

    Impl::Buffer readback = m.makeBuffer(kTargets * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    buffers.push_back(readback);
    for (uint32_t i = 0; i < kTargets; i++) {
        VkBufferImageCopy copy{};
        copy.bufferOffset = i * 16;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {1, 1, 1};
        vkCmdCopyImageToBuffer(cb, m.images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
    }
    vkEndCommandBuffer(cb);

    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(m.device, &fi, nullptr, &fence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(m.queue, 1, &si, fence);
    if (vkWaitForFences(m.device, 1, &fence, VK_TRUE, 20ull * 1000 * 1000 * 1000) != VK_SUCCESS) {
        result.error = "timeout";
        vkDeviceWaitIdle(m.device);
        cleanup();
        return result;
    }
    std::memcpy(result.targets.data(), readback.mapped, kTargets * 16);
    result.ok = true;
    cleanup();
    return result;
}
