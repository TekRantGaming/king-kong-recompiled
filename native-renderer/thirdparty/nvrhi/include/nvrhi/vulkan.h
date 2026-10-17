/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <vulkan/vulkan.h>
#include <nvrhi/nvrhi.h>

namespace nvrhi 
{
    namespace ObjectTypes
    {
        constexpr ObjectType Nvrhi_VK_Device = 0x00030101;
    };
}

namespace nvrhi::vulkan
{
    class IDevice : public nvrhi::IDevice
    {
    public:
        // Additional Vulkan-specific public methods
        virtual VkSemaphore getQueueSemaphore(CommandQueue queue) = 0;
        virtual void queueWaitForSemaphore(CommandQueue waitQueue, VkSemaphore semaphore, uint64_t value) = 0;
        virtual void queueSignalSemaphore(CommandQueue executionQueue, VkSemaphore semaphore, uint64_t value) = 0;
        virtual uint64_t queueGetCompletedInstance(CommandQueue queue) = 0;

        // Graphics pipeline library parts and links so far (KK patch; all zero without the library).
        struct PipelineLibraryStats
        {
            uint64_t partsMade[4] = {};   // vertex input, pre-rasterization, fragment shader, fragment output
            uint64_t partsReused[4] = {}; // found made (or being made by another thread)
            uint64_t partFailures = 0;
            double partMs = 0;            // making parts, all threads
            uint64_t links = 0;
            uint64_t linkFailures = 0;    // compiled whole instead
            double linkMs = 0;            // linking alone
        };
        virtual PipelineLibraryStats getPipelineLibraryStats() { return {}; }
        // Makes the pre-rasterization part of desc.VS and the fragment shader part of desc.PS (whichever are set)
        // now, without a pipeline: a later linked pipeline of these shaders then compiles nothing. False without
        // the library or when the driver failed.
        virtual bool prepareGraphicsPipelineParts(const GraphicsPipelineDesc& desc, const FramebufferInfo& fbinfo)
        {
            (void)desc; (void)fbinfo;
            return false;
        }
    };

    typedef RefCountPtr<IDevice> DeviceHandle;

    struct DeviceDesc
    {
        IMessageCallback* errorCB = nullptr;

        VkInstance instance;
        VkPhysicalDevice physicalDevice;
        VkDevice device;

        // any of the queues can be null if this context doesn't intend to use them
        VkQueue graphicsQueue;
        int graphicsQueueIndex = -1;
        VkQueue transferQueue;
        int transferQueueIndex = -1;
        VkQueue computeQueue;
        int computeQueueIndex = -1;

        VkAllocationCallbacks *allocationCallbacks = nullptr;

        const char **instanceExtensions = nullptr;
        size_t numInstanceExtensions = 0;
        
        const char **deviceExtensions = nullptr;
        size_t numDeviceExtensions = 0;

        uint32_t maxTimerQueries = 256;

        // Indicates if VkPhysicalDeviceVulkan12Features::bufferDeviceAddress was set to 'true' at device creation time
        bool bufferDeviceAddressSupported = false;
        // Indicates if VkPhysicalDeviceVulkan12Features::descriptorBindingUniformBufferUpdateAfterBind was set to
        // 'true' at device creation time. Without it, ConstantBuffer entries of a bindless layout do not get
        // UPDATE_AFTER_BIND. The other descriptorBinding*UpdateAfterBind features are required by bindless layouts.
        bool descriptorBindingUniformBufferUpdateAfterBind = false;
        bool aftermathEnabled = false;
        bool logBufferLifetime = false;

        std::string vulkanLibraryName; // if empty, use default

        // A pipeline cache to create every pipeline with, made and kept by the application (which can
        // fill it from a file and save it). The device does not destroy it. VK_NULL_HANDLE: the device
        // makes an empty cache of its own.
        VkPipelineCache pipelineCache = VK_NULL_HANDLE;

        // KK patch: the application enabled VK_KHR_pipeline_library and VK_EXT_graphics_pipeline_library with
        // VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT::graphicsPipelineLibrary on a Vulkan 1.3 device (the
        // linked pipelines leave cull mode, front face, depth bias, depth and stencil state dynamic, core in 1.3).
        // Then GraphicsPipelineDesc::vulkanLibraryLink pipelines are linked from cached parts
        // (Feature::GraphicsPipelineLibrary). False: every pipeline is compiled whole.
        bool graphicsPipelineLibrary = false;
    };

    NVRHI_API DeviceHandle createDevice(const DeviceDesc& desc);
   
    NVRHI_API VkFormat convertFormat(nvrhi::Format format);

    NVRHI_API const char* resultToString(VkResult result);
}