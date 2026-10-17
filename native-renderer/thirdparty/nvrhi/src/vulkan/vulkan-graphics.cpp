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

#include "vulkan-backend.h"
#include "../../../../backend/dev_gpu_stats.h"
#include <nvrhi/common/misc.h>
#include <chrono>
#include <functional>

namespace nvrhi::vulkan
{
    static TextureDimension getDimensionForFramebuffer(TextureDimension dimension, bool isArray)
    {
        // Can't render into cubes and 3D textures directly, convert them to 2D arrays
        if (dimension == TextureDimension::TextureCube || dimension == TextureDimension::TextureCubeArray || dimension == TextureDimension::Texture3D)
            dimension = TextureDimension::Texture2DArray;

        if (!isArray)
        {
            // Demote arrays to single textures if we just need one layer
            switch(dimension)  // NOLINT(clang-diagnostic-switch-enum)
            {
            case TextureDimension::Texture1DArray:
                dimension = TextureDimension::Texture1D;
                break;
            case TextureDimension::Texture2DArray:
                dimension = TextureDimension::Texture2D;
                break;
            case TextureDimension::Texture2DMSArray:
                dimension = TextureDimension::Texture2DMS;
                break;
            default:
                break;
            }
        }

        return dimension;
    }

    FramebufferHandle Device::createFramebuffer(const FramebufferDesc& desc)
    {
        Framebuffer *fb = new Framebuffer();
        fb->desc = desc;
        fb->framebufferInfo = FramebufferInfoEx(desc);

        for(uint32_t i = 0; i < desc.colorAttachments.size(); i++)
        {
            const auto& rt = desc.colorAttachments[i];
            Texture* t = checked_cast<Texture*>(rt.texture);

            assert(fb->framebufferInfo.width == std::max(t->desc.width >> rt.subresources.baseMipLevel, 1u));
            assert(fb->framebufferInfo.height == std::max(t->desc.height >> rt.subresources.baseMipLevel, 1u));

            TextureSubresourceSet subresources = rt.subresources.resolve(t->desc, true);

            TextureDimension dimension = getDimensionForFramebuffer(t->desc.dimension, subresources.numArraySlices > 1);

            const auto& view = t->getSubresourceView(subresources, dimension, rt.format, vk::ImageUsageFlagBits::eColorAttachment);
            
            vk::RenderingAttachmentInfo& attachmentInfo = fb->colorAttachments.emplace_back();
            attachmentInfo = vk::RenderingAttachmentInfo()
                .setImageView(view.view)
                .setImageLayout(vk::ImageLayout::eColorAttachmentOptimal)
                .setLoadOp(vk::AttachmentLoadOp::eLoad)
                .setStoreOp(vk::AttachmentStoreOp::eStore);

            fb->resources.push_back(rt.texture);
        }

        // add depth/stencil attachment if present
        if (desc.depthAttachment.valid())
        {
            const auto& att = desc.depthAttachment;

            Texture* texture = checked_cast<Texture*>(att.texture);

            assert(fb->framebufferInfo.width == std::max(texture->desc.width >> att.subresources.baseMipLevel, 1u));
            assert(fb->framebufferInfo.height == std::max(texture->desc.height >> att.subresources.baseMipLevel, 1u));

            vk::ImageLayout depthLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
            if (desc.depthAttachment.isReadOnly)
            {
                depthLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
            }

            TextureSubresourceSet subresources = att.subresources.resolve(texture->desc, true);

            TextureDimension dimension = getDimensionForFramebuffer(texture->desc.dimension, subresources.numArraySlices > 1);

            const auto& view = texture->getSubresourceView(subresources, dimension, att.format, vk::ImageUsageFlagBits::eDepthStencilAttachment);

            fb->depthAttachment = vk::RenderingAttachmentInfo()
                .setImageView(view.view)
                .setImageLayout(depthLayout)
                .setLoadOp(vk::AttachmentLoadOp::eLoad)
                .setStoreOp(vk::AttachmentStoreOp::eStore);

            if (getFormatInfo(texture->desc.format).hasStencil)
                fb->stencilAttachment = fb->depthAttachment;
                
            fb->resources.push_back(att.texture);
        }

        // add VRS attachment
        if (desc.shadingRateAttachment.valid())
        {
            const auto& vrsAttachment = desc.shadingRateAttachment;
            Texture* vrsTexture = checked_cast<Texture*>(vrsAttachment.texture);
            assert(vrsTexture->imageInfo.format == vk::Format::eR8Uint);
            assert(vrsTexture->imageInfo.samples == vk::SampleCountFlagBits::e1);

            TextureSubresourceSet subresources = vrsAttachment.subresources.resolve(vrsTexture->desc, true);
            TextureDimension dimension = getDimensionForFramebuffer(vrsTexture->desc.dimension, subresources.numArraySlices > 1);

            const auto& view = vrsTexture->getSubresourceView(subresources, dimension, vrsAttachment.format, vk::ImageUsageFlagBits::eFragmentShadingRateAttachmentKHR);

            auto rateProps = vk::PhysicalDeviceFragmentShadingRatePropertiesKHR();
            auto props = vk::PhysicalDeviceProperties2();
            props.pNext = &rateProps;
            m_Context.physicalDevice.getProperties2(&props);

            fb->shadingRateAttachment = vk::RenderingFragmentShadingRateAttachmentInfoKHR()
                .setImageLayout(vk::ImageLayout::eFragmentShadingRateAttachmentOptimalKHR)
                .setImageView(view.view)
                .setShadingRateAttachmentTexelSize(rateProps.minFragmentShadingRateAttachmentTexelSize);

            fb->resources.push_back(vrsAttachment.texture);
        }
        
        return FramebufferHandle::Create(fb);
    }

    void countSpecializationConstants(
        Shader* shader,
        size_t& numShaders,
        size_t& numShadersWithSpecializations,
        size_t& numSpecializationConstants)
    {
        if (!shader)
            return;

        numShaders += 1;

        if (shader->specializationConstants.empty())
            return;

        numShadersWithSpecializations += 1;
        numSpecializationConstants += shader->specializationConstants.size();
    }

    vk::PipelineShaderStageCreateInfo makeShaderStageCreateInfo(
        Shader *shader,
        std::vector<vk::SpecializationInfo>& specInfos,
        std::vector<vk::SpecializationMapEntry>& specMapEntries,
        std::vector<uint32_t>& specData)
    {
        auto shaderStageCreateInfo = vk::PipelineShaderStageCreateInfo()
            .setStage(shader->stageFlagBits)
            .setModule(shader->shaderModule)
            .setPName(shader->desc.entryName.c_str());

        if (!shader->specializationConstants.empty())
        {
            // For specializations, this functions allocates:
            //  - One entry in specInfos per shader
            //  - One entry in specMapEntries and specData each per constant
            // The vectors are pre-allocated, so it's safe to use .data() before writing the data

            assert(specInfos.data());
            assert(specMapEntries.data());
            assert(specData.data());

            shaderStageCreateInfo.setPSpecializationInfo(specInfos.data() + specInfos.size());
            
            auto specInfo = vk::SpecializationInfo()
                .setPMapEntries(specMapEntries.data() + specMapEntries.size())
                .setMapEntryCount(static_cast<uint32_t>(shader->specializationConstants.size()))
                .setPData(specData.data() + specData.size())
                .setDataSize(shader->specializationConstants.size() * sizeof(uint32_t));

            size_t dataOffset = 0;
            for (const auto& constant : shader->specializationConstants)
            {
                auto specMapEntry = vk::SpecializationMapEntry()
                    .setConstantID(constant.constantID)
                    .setOffset(static_cast<uint32_t>(dataOffset))
                    .setSize(sizeof(uint32_t));

                specMapEntries.push_back(specMapEntry);
                specData.push_back(constant.value.u);
                dataOffset += specMapEntry.size;
            }

            specInfos.push_back(specInfo);
        }

        return shaderStageCreateInfo;
    }

    // ---- KK patch: graphics pipeline libraries (VK_EXT_graphics_pipeline_library) ------------------------------
    //
    // A pipeline is linked from four parts that the device makes once and keeps: vertex input (topology), the
    // pre-rasterization shaders (VS/HS/DS/GS with the polygon mode and depth clip), the fragment shader (PS with the
    // multisample state) and the fragment output (formats, blending). The link has no link-time optimisation, so it
    // takes microseconds once the parts exist; a new pipeline of known shaders needs no shader compile at all. To
    // share parts between more pipelines the pre-rasterization part leaves cull mode, front face and depth bias
    // dynamic and the fragment shader part leaves the depth and stencil state dynamic (all core in Vulkan 1.3): the
    // command list sets them from the pipeline description when it binds the pipeline (setGraphicsState).

    namespace
    {
        enum PipelineLibraryPartKind : int
        {
            PartVertexInput = 0,
            PartPreRasterization = 1,
            PartFragmentShader = 2,
            PartFragmentOutput = 3,
        };

        struct PartKey
        {
            std::string bytes;
            template <typename T> void put(const T& value)
            {
                bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
            }
            void putObject(const void* object) { put(reinterpret_cast<uintptr_t>(object)); }
        };

        uint64_t microsecondsSince(std::chrono::steady_clock::time_point start)
        {
            return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        }

        const vk::DynamicState c_PreRasterizationDynamicStates[] = {
            vk::DynamicState::eViewport, vk::DynamicState::eScissor, vk::DynamicState::eCullMode,
            vk::DynamicState::eFrontFace, vk::DynamicState::eDepthBiasEnable, vk::DynamicState::eDepthBias,
        };
        const vk::DynamicState c_FragmentShaderDynamicStates[] = {
            vk::DynamicState::eDepthTestEnable, vk::DynamicState::eDepthWriteEnable, vk::DynamicState::eDepthCompareOp,
            vk::DynamicState::eStencilTestEnable, vk::DynamicState::eStencilOp, vk::DynamicState::eStencilCompareMask,
            vk::DynamicState::eStencilWriteMask, vk::DynamicState::eStencilReference,
        };
    }

    // The part of `kind` with this key: made now by this thread if nobody has made it (null when the driver failed).
    static std::shared_ptr<PipelineLibraryPart> getPipelineLibraryPart(PipelineLibraryCache& cache, const VulkanContext& context,
        int kind, PartKey&& key, std::vector<RefCountPtr<IResource>>&& keep, const std::function<vk::Pipeline(vk::PipelineLayout&)>& make)
    {
        std::shared_ptr<PipelineLibraryPart> part;
        {
            std::lock_guard<std::mutex> lock(cache.mutex);
            std::shared_ptr<PipelineLibraryPart>& slot = cache.parts[kind][key.bytes];
            if (!slot)
            {
                slot = std::make_shared<PipelineLibraryPart>();
                slot->device = context.device;
                slot->allocationCallbacks = context.allocationCallbacks;
                slot->keep = std::move(keep);
            }
            part = slot;
        }
        std::lock_guard<std::mutex> lock(part->mutex);
        if (part->done)
        {
            cache.partsReused[kind]++;
            return part->pipeline ? part : nullptr;
        }
        const auto start = std::chrono::steady_clock::now();
        part->pipeline = make(part->layout);
        part->done = true;
        cache.partMicroseconds += microsecondsSince(start);
        if (!part->pipeline)
        {
            cache.partFailures++;
            return nullptr;
        }
        cache.partsMade[kind]++;
        return part;
    }

    bool Device::prepareGraphicsPipelineParts(const GraphicsPipelineDesc& desc, const FramebufferInfo& fbinfo)
    {
        if (!m_PipelineLibrary)
            return false;
        bool made = false;
        createLinkedGraphicsPipeline(desc, fbinfo, true, &made);
        return made;
    }

    GraphicsPipelineHandle Device::createLinkedGraphicsPipeline(const GraphicsPipelineDesc& desc, FramebufferInfo const& fbinfo,
                                                                bool partsOnly, bool* partsMade)
    {
        PipelineLibraryCache& cache = *m_PipelineLibrary;
        InputLayout* inputLayout = checked_cast<InputLayout*>(desc.inputLayout.Get());
        Shader* stages[4] = {
            checked_cast<Shader*>(desc.VS.Get()), checked_cast<Shader*>(desc.HS.Get()),
            checked_cast<Shader*>(desc.DS.Get()), checked_cast<Shader*>(desc.GS.Get()) };
        Shader* PS = checked_cast<Shader*>(desc.PS.Get());
        const auto& rasterState = desc.renderState.rasterState;
        const auto& blendState = desc.renderState.blendState;
        const vk::PipelineCreateFlags libraryFlags = vk::PipelineCreateFlagBits::eLibraryKHR;
        const vk::SampleCountFlagBits samples = vk::SampleCountFlagBits(fbinfo.sampleCount);
        const bool usesBlendConstants = blendState.usesConstantColor(uint32_t(fbinfo.colorFormats.size()));

        auto makeLayout = [&](vk::PipelineLayout& layout) {
            BindingVector<RefCountPtr<BindingLayout>> bindingLayouts;
            vk::ShaderStageFlags pushConstantVisibility;
            BindingVector<uint32_t> setToBinding;
            return createPipelineLayout(layout, bindingLayouts, pushConstantVisibility, setToBinding, m_Context, desc.bindingLayouts);
        };
        auto createLibrary = [&](vk::GraphicsPipelineCreateInfo& info) {
            vk::Pipeline pipeline;
            const vk::Result res = m_Context.device.createGraphicsPipelines(m_Context.pipelineCache, 1, &info, m_Context.allocationCallbacks, &pipeline);
            return res == vk::Result::eSuccess ? pipeline : vk::Pipeline();
        };

        // Vertex input: the input layout and topology.
        std::shared_ptr<PipelineLibraryPart> vertexInputPart;
        if (!partsOnly)
        {
            PartKey key;
            key.putObject(inputLayout);
            key.put(desc.primType);
            std::vector<RefCountPtr<IResource>> keep;
            if (inputLayout) keep.emplace_back(inputLayout);
            vertexInputPart = getPipelineLibraryPart(cache, m_Context, PartVertexInput, std::move(key), std::move(keep), [&](vk::PipelineLayout&) {
                auto vertexInput = vk::PipelineVertexInputStateCreateInfo();
                if (inputLayout)
                {
                    vertexInput.setVertexBindingDescriptionCount(uint32_t(inputLayout->bindingDesc.size()))
                               .setPVertexBindingDescriptions(inputLayout->bindingDesc.data())
                               .setVertexAttributeDescriptionCount(uint32_t(inputLayout->attributeDesc.size()))
                               .setPVertexAttributeDescriptions(inputLayout->attributeDesc.data());
                }
                auto inputAssembly = vk::PipelineInputAssemblyStateCreateInfo().setTopology(convertPrimitiveTopology(desc.primType));
                auto libraryInfo = vk::GraphicsPipelineLibraryCreateInfoEXT().setFlags(vk::GraphicsPipelineLibraryFlagBitsEXT::eVertexInputInterface);
                auto info = vk::GraphicsPipelineCreateInfo().setPNext(&libraryInfo).setFlags(libraryFlags)
                    .setPVertexInputState(&vertexInput).setPInputAssemblyState(&inputAssembly);
                return createLibrary(info);
            });
        }

        // Pre-rasterization shaders.
        std::shared_ptr<PipelineLibraryPart> preRasterizationPart;
        if (partsOnly ? desc.VS != nullptr : vertexInputPart != nullptr)
        {
            PartKey key;
            std::vector<RefCountPtr<IResource>> keep;
            for (Shader* s : stages)
            {
                key.putObject(s);
                if (s) keep.emplace_back(s);
            }
            for (const BindingLayoutHandle& l : desc.bindingLayouts)
            {
                key.putObject(l.Get());
                keep.emplace_back(l.Get());
            }
            key.put(rasterState.fillMode);
            // Depth clip changes the part only through VK_EXT_depth_clip_enable (as in the whole pipeline).
            key.put(m_Context.extensions.EXT_depth_clip_enable ? rasterState.depthClipEnable : true);
            key.put(rasterState.conservativeRasterEnable);
            key.put(desc.primType == PrimitiveType::PatchList ? desc.patchControlPoints : 0u);
            preRasterizationPart = getPipelineLibraryPart(cache, m_Context, PartPreRasterization, std::move(key), std::move(keep), [&](vk::PipelineLayout& layout) {
                if (makeLayout(layout) != vk::Result::eSuccess)
                    return vk::Pipeline();
                std::vector<vk::PipelineShaderStageCreateInfo> shaderStages;
                std::vector<vk::SpecializationInfo> specInfos;
                std::vector<vk::SpecializationMapEntry> specMapEntries;
                std::vector<uint32_t> specData;
                size_t numShaders = 0, numShadersWithSpecializations = 0, numSpecializationConstants = 0;
                for (Shader* s : stages)
                    countSpecializationConstants(s, numShaders, numShadersWithSpecializations, numSpecializationConstants);
                shaderStages.reserve(numShaders);
                specInfos.reserve(numShadersWithSpecializations);
                specMapEntries.reserve(numSpecializationConstants);
                specData.reserve(numSpecializationConstants);
                for (Shader* s : stages)
                    if (s) shaderStages.push_back(makeShaderStageCreateInfo(s, specInfos, specMapEntries, specData));
                auto viewportState = vk::PipelineViewportStateCreateInfo().setViewportCount(1).setScissorCount(1);
                auto rasterizer = vk::PipelineRasterizationStateCreateInfo()
                    .setPolygonMode(convertFillMode(rasterState.fillMode))
                    .setCullMode(vk::CullModeFlagBits::eNone)
                    .setFrontFace(vk::FrontFace::eCounterClockwise)
                    .setLineWidth(1.0f);
                vk::PipelineRasterizationDepthClipStateCreateInfoEXT depthClipEnableExt;
                auto conservativeRasterState = vk::PipelineRasterizationConservativeStateCreateInfoEXT()
                    .setConservativeRasterizationMode(vk::ConservativeRasterizationModeEXT::eOverestimate);
                if (m_Context.extensions.EXT_depth_clip_enable && !rasterState.depthClipEnable)
                {
                    depthClipEnableExt.setDepthClipEnable(false);
                    rasterizer.setPNext(&depthClipEnableExt);
                }
                if (rasterState.conservativeRasterEnable)
                    rasterizer.setPNext(&conservativeRasterState);
                auto dynamicStateInfo = vk::PipelineDynamicStateCreateInfo()
                    .setDynamicStateCount(uint32_t(std::size(c_PreRasterizationDynamicStates)))
                    .setPDynamicStates(c_PreRasterizationDynamicStates);
                auto tessellationState = vk::PipelineTessellationStateCreateInfo().setPatchControlPoints(desc.patchControlPoints);
                auto libraryInfo = vk::GraphicsPipelineLibraryCreateInfoEXT().setFlags(vk::GraphicsPipelineLibraryFlagBitsEXT::ePreRasterizationShaders);
                auto info = vk::GraphicsPipelineCreateInfo().setPNext(&libraryInfo).setFlags(libraryFlags)
                    .setStageCount(uint32_t(shaderStages.size())).setPStages(shaderStages.data())
                    .setPViewportState(&viewportState).setPRasterizationState(&rasterizer)
                    .setPDynamicState(&dynamicStateInfo).setLayout(layout);
                if (desc.primType == PrimitiveType::PatchList)
                    info.setPTessellationState(&tessellationState);
                return createLibrary(info);
            });
        }

        // Fragment shader, with the multisample state (the fragment output part has the same).
        auto multisample = vk::PipelineMultisampleStateCreateInfo()
            .setRasterizationSamples(samples)
            .setAlphaToCoverageEnable(blendState.alphaToCoverageEnable);
        std::shared_ptr<PipelineLibraryPart> fragmentShaderPart;
        if (partsOnly ? desc.PS != nullptr : preRasterizationPart != nullptr)
        {
            PartKey key;
            std::vector<RefCountPtr<IResource>> keep;
            key.putObject(PS);
            if (PS) keep.emplace_back(PS);
            for (const BindingLayoutHandle& l : desc.bindingLayouts)
            {
                key.putObject(l.Get());
                keep.emplace_back(l.Get());
            }
            key.put(fbinfo.sampleCount);
            key.put(blendState.alphaToCoverageEnable);
            fragmentShaderPart = getPipelineLibraryPart(cache, m_Context, PartFragmentShader, std::move(key), std::move(keep), [&](vk::PipelineLayout& layout) {
                if (makeLayout(layout) != vk::Result::eSuccess)
                    return vk::Pipeline();
                std::vector<vk::SpecializationInfo> specInfos;
                std::vector<vk::SpecializationMapEntry> specMapEntries;
                std::vector<uint32_t> specData;
                size_t numShaders = 0, numShadersWithSpecializations = 0, numSpecializationConstants = 0;
                countSpecializationConstants(PS, numShaders, numShadersWithSpecializations, numSpecializationConstants);
                specInfos.reserve(numShadersWithSpecializations);
                specMapEntries.reserve(numSpecializationConstants);
                specData.reserve(numSpecializationConstants);
                vk::PipelineShaderStageCreateInfo stage;
                if (PS) stage = makeShaderStageCreateInfo(PS, specInfos, specMapEntries, specData);
                auto depthStencil = vk::PipelineDepthStencilStateCreateInfo();  // all dynamic but depth bounds (off)
                auto dynamicStateInfo = vk::PipelineDynamicStateCreateInfo()
                    .setDynamicStateCount(uint32_t(std::size(c_FragmentShaderDynamicStates)))
                    .setPDynamicStates(c_FragmentShaderDynamicStates);
                auto libraryInfo = vk::GraphicsPipelineLibraryCreateInfoEXT().setFlags(vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentShader);
                auto info = vk::GraphicsPipelineCreateInfo().setPNext(&libraryInfo).setFlags(libraryFlags)
                    .setStageCount(PS ? 1u : 0u).setPStages(PS ? &stage : nullptr)
                    .setPDepthStencilState(&depthStencil).setPMultisampleState(&multisample)
                    .setPDynamicState(&dynamicStateInfo).setLayout(layout);
                return createLibrary(info);
            });
        }

        if (partsOnly)
        {
            if (partsMade)
                *partsMade = (!desc.VS || preRasterizationPart) && (!desc.PS || fragmentShaderPart);
            return nullptr;
        }

        // Fragment output: attachment formats and blending.
        std::shared_ptr<PipelineLibraryPart> fragmentOutputPart;
        if (fragmentShaderPart)
        {
            PartKey key;
            key.put(uint32_t(fbinfo.colorFormats.size()));
            for (size_t i = 0; i < fbinfo.colorFormats.size(); i++)
            {
                const BlendState::RenderTarget& t = blendState.targets[i];
                key.put(fbinfo.colorFormats[i]);
                key.put(t.blendEnable); key.put(t.srcBlend); key.put(t.destBlend); key.put(t.blendOp);
                key.put(t.srcBlendAlpha); key.put(t.destBlendAlpha); key.put(t.blendOpAlpha); key.put(t.colorWriteMask);
            }
            key.put(fbinfo.depthFormat);
            key.put(fbinfo.sampleCount);
            key.put(blendState.alphaToCoverageEnable);
            key.put(usesBlendConstants);
            fragmentOutputPart = getPipelineLibraryPart(cache, m_Context, PartFragmentOutput, std::move(key), {}, [&](vk::PipelineLayout&) {
                static_vector<vk::PipelineColorBlendAttachmentState, c_MaxRenderTargets> colorBlendAttachments(fbinfo.colorFormats.size());
                for (uint32_t i = 0; i < uint32_t(fbinfo.colorFormats.size()); i++)
                    colorBlendAttachments[i] = convertBlendState(blendState.targets[i]);
                auto colorBlend = vk::PipelineColorBlendStateCreateInfo()
                    .setAttachmentCount(uint32_t(colorBlendAttachments.size()))
                    .setPAttachments(colorBlendAttachments.data());
                const vk::DynamicState blendConstants = vk::DynamicState::eBlendConstants;
                auto dynamicStateInfo = vk::PipelineDynamicStateCreateInfo()
                    .setDynamicStateCount(usesBlendConstants ? 1u : 0u).setPDynamicStates(&blendConstants);
                std::array<vk::Format, c_MaxRenderTargets> colorFormats;
                for (size_t i = 0; i < fbinfo.colorFormats.size(); i++)
                    colorFormats[i] = vk::Format(convertFormat(fbinfo.colorFormats[i]));
                FormatInfo const& depthStencilFormatInfo = getFormatInfo(fbinfo.depthFormat);
                const vk::Format depthStencilFormat = vk::Format(convertFormat(fbinfo.depthFormat));
                auto renderingInfo = vk::PipelineRenderingCreateInfo()
                    .setColorAttachmentCount(uint32_t(fbinfo.colorFormats.size()))
                    .setPColorAttachmentFormats(colorFormats.data())
                    .setDepthAttachmentFormat(depthStencilFormatInfo.hasDepth ? depthStencilFormat : vk::Format::eUndefined)
                    .setStencilAttachmentFormat(depthStencilFormatInfo.hasStencil ? depthStencilFormat : vk::Format::eUndefined);
                auto libraryInfo = vk::GraphicsPipelineLibraryCreateInfoEXT().setFlags(vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentOutputInterface);
                libraryInfo.setPNext(&renderingInfo);
                auto info = vk::GraphicsPipelineCreateInfo().setPNext(&libraryInfo).setFlags(libraryFlags)
                    .setPColorBlendState(&colorBlend).setPMultisampleState(&multisample)
                    .setPDynamicState(&dynamicStateInfo);
                return createLibrary(info);
            });
        }
        if (!fragmentOutputPart)
            return nullptr;

        // The link.
        RefCountPtr<GraphicsPipeline> pso = RefCountPtr<GraphicsPipeline>::Create(new GraphicsPipeline(m_Context));
        pso->desc = desc;
        pso->framebufferInfo = fbinfo;
        pso->usesBlendConstants = usesBlendConstants;
        pso->dynamicFixedFunction = true;
        pso->libraryParts = { vertexInputPart, preRasterizationPart, fragmentShaderPart, fragmentOutputPart };
        if (desc.VS) pso->shaderMask = pso->shaderMask | ShaderType::Vertex;
        if (desc.HS) pso->shaderMask = pso->shaderMask | ShaderType::Hull;
        if (desc.DS) pso->shaderMask = pso->shaderMask | ShaderType::Domain;
        if (desc.GS) pso->shaderMask = pso->shaderMask | ShaderType::Geometry;
        if (desc.PS) pso->shaderMask = pso->shaderMask | ShaderType::Pixel;
        if (createPipelineLayout(pso->pipelineLayout, pso->pipelineBindingLayouts, pso->pushConstantVisibility,
                pso->descriptorSetIdxToBindingIdx, m_Context, desc.bindingLayouts) != vk::Result::eSuccess)
            return nullptr;
        const vk::Pipeline libraries[4] = { vertexInputPart->pipeline, preRasterizationPart->pipeline,
                                            fragmentShaderPart->pipeline, fragmentOutputPart->pipeline };
        auto libraryInfo = vk::PipelineLibraryCreateInfoKHR().setLibraryCount(4).setPLibraries(libraries);
        auto info = vk::GraphicsPipelineCreateInfo().setPNext(&libraryInfo).setLayout(pso->pipelineLayout)
            .setBasePipelineIndex(-1);
        const auto start = std::chrono::steady_clock::now();
        const vk::Result res = m_Context.device.createGraphicsPipelines(m_Context.pipelineCache, 1, &info,
            m_Context.allocationCallbacks, &pso->pipeline);
        cache.linkMicroseconds += microsecondsSince(start);
        if (res != vk::Result::eSuccess)
        {
            cache.linkFailures++;
            return nullptr;
        }
        cache.links++;
        return GraphicsPipelineHandle(pso.Get());
    }

    // The fixed-function state a linked pipeline's parts leave dynamic, from its description.
    static void setLinkedPipelineDynamicState(vk::CommandBuffer cmdBuf, const GraphicsPipeline& pso, uint32_t stencilRef)
    {
        const auto& rs = pso.desc.renderState.rasterState;
        const auto& ds = pso.desc.renderState.depthStencilState;
        cmdBuf.setCullMode(convertCullMode(rs.cullMode));
        cmdBuf.setFrontFace(rs.frontCounterClockwise ? vk::FrontFace::eCounterClockwise : vk::FrontFace::eClockwise);
        cmdBuf.setDepthBiasEnable(rs.depthBias ? true : false);
        cmdBuf.setDepthBias(float(rs.depthBias), rs.depthBiasClamp, rs.slopeScaledDepthBias);
        cmdBuf.setDepthTestEnable(ds.depthTestEnable);
        cmdBuf.setDepthWriteEnable(ds.depthWriteEnable);
        cmdBuf.setDepthCompareOp(convertCompareOp(ds.depthFunc));
        cmdBuf.setStencilTestEnable(ds.stencilEnable);
        const auto setOps = [&](vk::StencilFaceFlags face, const DepthStencilState::StencilOpDesc& op) {
            cmdBuf.setStencilOp(face, convertStencilOp(op.failOp), convertStencilOp(op.passOp),
                                convertStencilOp(op.depthFailOp), convertCompareOp(op.stencilFunc));
        };
        setOps(vk::StencilFaceFlagBits::eFront, ds.frontFaceStencil);
        setOps(vk::StencilFaceFlagBits::eBack, ds.backFaceStencil);
        cmdBuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack, ds.stencilReadMask);
        cmdBuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack, ds.stencilWriteMask);
        cmdBuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack, ds.dynamicStencilRef ? stencilRef : uint32_t(ds.stencilRefValue));
    }

    GraphicsPipelineHandle Device::createGraphicsPipeline(const GraphicsPipelineDesc& desc, FramebufferInfo const& fbinfo)
    {
        if (desc.renderState.singlePassStereo.enabled)
        {
            m_Context.error("Single-pass stereo is not supported by the Vulkan backend");
            return nullptr;
        }

        if (desc.vulkanLibraryLink && m_PipelineLibrary && !desc.shadingRateState.enabled)
        {
            if (GraphicsPipelineHandle linked = createLinkedGraphicsPipeline(desc, fbinfo))
                return linked;
            // A part or the link failed: compiled whole below (getPipelineLibraryStats counts it).
        }

        vk::Result res;

        InputLayout* inputLayout = checked_cast<InputLayout*>(desc.inputLayout.Get());

        GraphicsPipeline *pso = new GraphicsPipeline(m_Context);
        pso->desc = desc;
        pso->framebufferInfo = fbinfo;

        Shader* VS = checked_cast<Shader*>(desc.VS.Get());
        Shader* HS = checked_cast<Shader*>(desc.HS.Get());
        Shader* DS = checked_cast<Shader*>(desc.DS.Get());
        Shader* GS = checked_cast<Shader*>(desc.GS.Get());
        Shader* PS = checked_cast<Shader*>(desc.PS.Get());

        size_t numShaders = 0;
        size_t numShadersWithSpecializations = 0;
        size_t numSpecializationConstants = 0;

        // Count the spec constants for all stages
        countSpecializationConstants(VS, numShaders, numShadersWithSpecializations, numSpecializationConstants);
        countSpecializationConstants(HS, numShaders, numShadersWithSpecializations, numSpecializationConstants);
        countSpecializationConstants(DS, numShaders, numShadersWithSpecializations, numSpecializationConstants);
        countSpecializationConstants(GS, numShaders, numShadersWithSpecializations, numSpecializationConstants);
        countSpecializationConstants(PS, numShaders, numShadersWithSpecializations, numSpecializationConstants);

        std::vector<vk::PipelineShaderStageCreateInfo> shaderStages;
        std::vector<vk::SpecializationInfo> specInfos;
        std::vector<vk::SpecializationMapEntry> specMapEntries;
        std::vector<uint32_t> specData;

        // Allocate buffers for specialization constants and related structures
        // so that shaderStageCreateInfo(...) can directly use pointers inside the vectors
        // because the vectors won't reallocate their buffers
        shaderStages.reserve(numShaders);
        specInfos.reserve(numShadersWithSpecializations);
        specMapEntries.reserve(numSpecializationConstants);
        specData.reserve(numSpecializationConstants);

        // Set up shader stages
        if (desc.VS)
        {
            shaderStages.push_back(makeShaderStageCreateInfo(VS, 
                specInfos, specMapEntries, specData));
            pso->shaderMask = pso->shaderMask | ShaderType::Vertex;
        }

        if (desc.HS)
        {
            shaderStages.push_back(makeShaderStageCreateInfo(HS, 
                specInfos, specMapEntries, specData));
            pso->shaderMask = pso->shaderMask | ShaderType::Hull;
        }

        if (desc.DS)
        {
            shaderStages.push_back(makeShaderStageCreateInfo(DS, 
                specInfos, specMapEntries, specData));
            pso->shaderMask = pso->shaderMask | ShaderType::Domain;
        }

        if (desc.GS)
        {
            shaderStages.push_back(makeShaderStageCreateInfo(GS, 
                specInfos, specMapEntries, specData));
            pso->shaderMask = pso->shaderMask | ShaderType::Geometry;
        }

        if (desc.PS)
        {
            shaderStages.push_back(makeShaderStageCreateInfo(PS, 
                specInfos, specMapEntries, specData));
            pso->shaderMask = pso->shaderMask | ShaderType::Pixel;
        }

        // set up vertex input state
        auto vertexInput = vk::PipelineVertexInputStateCreateInfo();
        if (inputLayout)
        {
            vertexInput.setVertexBindingDescriptionCount(uint32_t(inputLayout->bindingDesc.size()))
                       .setPVertexBindingDescriptions(inputLayout->bindingDesc.data())
                       .setVertexAttributeDescriptionCount(uint32_t(inputLayout->attributeDesc.size()))
                       .setPVertexAttributeDescriptions(inputLayout->attributeDesc.data());
        }

        auto inputAssembly = vk::PipelineInputAssemblyStateCreateInfo()
                                .setTopology(convertPrimitiveTopology(desc.primType));

        // fixed function state
        const auto& rasterState = desc.renderState.rasterState;
        const auto& depthStencilState = desc.renderState.depthStencilState;
        const auto& blendState = desc.renderState.blendState;

        auto viewportState = vk::PipelineViewportStateCreateInfo()
            .setViewportCount(1)
            .setScissorCount(1);

        auto rasterizer = vk::PipelineRasterizationStateCreateInfo()
                            // .setDepthClampEnable(??)
                            // .setRasterizerDiscardEnable(??)
                            .setPolygonMode(convertFillMode(rasterState.fillMode))
                            .setCullMode(convertCullMode(rasterState.cullMode))
                            .setFrontFace(rasterState.frontCounterClockwise ?
                                            vk::FrontFace::eCounterClockwise : vk::FrontFace::eClockwise)
                            .setDepthBiasEnable(rasterState.depthBias ? true : false)
                            .setDepthBiasConstantFactor(float(rasterState.depthBias))
                            .setDepthBiasClamp(rasterState.depthBiasClamp)
                            .setDepthBiasSlopeFactor(rasterState.slopeScaledDepthBias)
                            .setLineWidth(1.0f);

        vk::PipelineRasterizationDepthClipStateCreateInfoEXT depthClipEnableExt;
        if (m_Context.extensions.EXT_depth_clip_enable && !rasterState.depthClipEnable) {
            depthClipEnableExt.setDepthClipEnable(false);
            rasterizer.setPNext(&depthClipEnableExt);
        }
        
        // Conservative raster state
        auto conservativeRasterState = vk::PipelineRasterizationConservativeStateCreateInfoEXT()
            .setConservativeRasterizationMode(vk::ConservativeRasterizationModeEXT::eOverestimate);
		if (rasterState.conservativeRasterEnable)
		{
			rasterizer.setPNext(&conservativeRasterState);
		}

        auto multisample = vk::PipelineMultisampleStateCreateInfo()
                            .setRasterizationSamples(vk::SampleCountFlagBits(fbinfo.sampleCount))
                            .setAlphaToCoverageEnable(blendState.alphaToCoverageEnable);

        auto depthStencil = vk::PipelineDepthStencilStateCreateInfo()
                                .setDepthTestEnable(depthStencilState.depthTestEnable)
                                .setDepthWriteEnable(depthStencilState.depthWriteEnable)
                                .setDepthCompareOp(convertCompareOp(depthStencilState.depthFunc))
                                .setStencilTestEnable(depthStencilState.stencilEnable)
                                .setFront(convertStencilState(depthStencilState, depthStencilState.frontFaceStencil))
                                .setBack(convertStencilState(depthStencilState, depthStencilState.backFaceStencil));

        // VRS state
        std::array<vk::FragmentShadingRateCombinerOpKHR, 2> combiners = {
            convertShadingRateCombiner(desc.shadingRateState.pipelinePrimitiveCombiner),
            convertShadingRateCombiner(desc.shadingRateState.imageCombiner)
        };
        auto shadingRateState = vk::PipelineFragmentShadingRateStateCreateInfoKHR()
            .setCombinerOps(combiners)
            .setFragmentSize(convertFragmentShadingRate(desc.shadingRateState.shadingRate));

        res = createPipelineLayout(
            pso->pipelineLayout,
            pso->pipelineBindingLayouts,
            pso->pushConstantVisibility,
            pso->descriptorSetIdxToBindingIdx,
            m_Context,
            desc.bindingLayouts);
        CHECK_VK_FAIL(res)

        static_vector<vk::PipelineColorBlendAttachmentState, c_MaxRenderTargets> colorBlendAttachments(fbinfo.colorFormats.size());

        for(uint32_t i = 0; i < uint32_t(fbinfo.colorFormats.size()); i++)
        {
            colorBlendAttachments[i] = convertBlendState(blendState.targets[i]);
        }

        auto colorBlend = vk::PipelineColorBlendStateCreateInfo()
                            .setAttachmentCount(uint32_t(colorBlendAttachments.size()))
                            .setPAttachments(colorBlendAttachments.data());

        pso->usesBlendConstants = blendState.usesConstantColor(uint32_t(fbinfo.colorFormats.size()));

        static_vector<vk::DynamicState, 5> dynamicStates = {
            vk::DynamicState::eViewport,
            vk::DynamicState::eScissor
        };
        if (pso->usesBlendConstants)
            dynamicStates.push_back(vk::DynamicState::eBlendConstants);
        if (pso->desc.renderState.depthStencilState.dynamicStencilRef)
            dynamicStates.push_back(vk::DynamicState::eStencilReference);
        if (pso->desc.shadingRateState.enabled)
            dynamicStates.push_back(vk::DynamicState::eFragmentShadingRateKHR);

        auto dynamicStateInfo = vk::PipelineDynamicStateCreateInfo()
            .setDynamicStateCount(uint32_t(dynamicStates.size()))
            .setPDynamicStates(dynamicStates.data());

        std::array<vk::Format, c_MaxRenderTargets> colorFormats;
        for (size_t i = 0; i < fbinfo.colorFormats.size(); i++)
            colorFormats[i] = vk::Format(convertFormat(fbinfo.colorFormats[i]));

        FormatInfo const& depthStencilFormatInfo = getFormatInfo(fbinfo.depthFormat);
        vk::Format depthStencilFormat = vk::Format(convertFormat(fbinfo.depthFormat));

        auto renderingInfo = vk::PipelineRenderingCreateInfo()
            .setColorAttachmentCount(uint32_t(fbinfo.colorFormats.size()))
            .setPColorAttachmentFormats(colorFormats.data())
            .setDepthAttachmentFormat(depthStencilFormatInfo.hasDepth ? depthStencilFormat : vk::Format::eUndefined)
            .setStencilAttachmentFormat(depthStencilFormatInfo.hasStencil ? depthStencilFormat : vk::Format::eUndefined);

        auto pipelineInfo = vk::GraphicsPipelineCreateInfo()
            .setPNext(&renderingInfo)
            .setStageCount(uint32_t(shaderStages.size()))
            .setPStages(shaderStages.data())
            .setPVertexInputState(&vertexInput)
            .setPInputAssemblyState(&inputAssembly)
            .setPViewportState(&viewportState)
            .setPRasterizationState(&rasterizer)
            .setPMultisampleState(&multisample)
            .setPDepthStencilState(&depthStencil)
            .setPColorBlendState(&colorBlend)
            .setPDynamicState(&dynamicStateInfo)
            .setLayout(pso->pipelineLayout)
            .setBasePipelineHandle(nullptr)
            .setBasePipelineIndex(-1)
            .setPTessellationState(nullptr);

        if (pso->desc.shadingRateState.enabled)
            renderingInfo.setPNext(&shadingRateState);

        auto tessellationState = vk::PipelineTessellationStateCreateInfo();

        if (desc.primType == PrimitiveType::PatchList)
        {
            tessellationState.setPatchControlPoints(desc.patchControlPoints);
            pipelineInfo.setPTessellationState(&tessellationState);
        }

        res = m_Context.device.createGraphicsPipelines(m_Context.pipelineCache,
                                                     1, &pipelineInfo,
                                                     m_Context.allocationCallbacks,
                                                     &pso->pipeline);
        ASSERT_VK_OK(res); // for debugging
        CHECK_VK_FAIL(res);
        
        return GraphicsPipelineHandle::Create(pso);
    }

    GraphicsPipelineHandle Device::createGraphicsPipeline(const GraphicsPipelineDesc& desc, IFramebuffer* fb)
    {
        if (!fb)
            return nullptr;
            
        return createGraphicsPipeline(desc, fb->getFramebufferInfo());
    }

    GraphicsPipeline::~GraphicsPipeline()
    {
        if (pipeline)
        {
            m_Context.device.destroyPipeline(pipeline, m_Context.allocationCallbacks);
            pipeline = nullptr;
        }

        if (pipelineLayout)
        {
            m_Context.device.destroyPipelineLayout(pipelineLayout, m_Context.allocationCallbacks);
            pipelineLayout = nullptr;
        }
    }

    Object GraphicsPipeline::getNativeObject(ObjectType objectType)
    {
        switch (objectType)
        {
        case ObjectTypes::VK_PipelineLayout:
            return Object(pipelineLayout);
        case ObjectTypes::VK_Pipeline:
            return Object(pipeline);
        default:
            return nullptr;
        }
    }

    void CommandList::beginRenderPass(nvrhi::IFramebuffer* _framebuffer)
    {
        endRenderPass();

        Framebuffer* framebuffer = checked_cast<Framebuffer*>(_framebuffer);

        if (!framebuffer)
            return;

        m_CurrentGraphicsState.framebuffer = framebuffer;
        m_CurrentMeshletState.framebuffer = framebuffer;

        vk::RenderingInfo renderingInfo = vk::RenderingInfo()
            .setRenderArea(vk::Rect2D()
                .setOffset(vk::Offset2D(0, 0))
                .setExtent(vk::Extent2D(framebuffer->framebufferInfo.width, framebuffer->framebufferInfo.height)))
            .setLayerCount(framebuffer->framebufferInfo.arraySize)
            .setColorAttachmentCount(uint32_t(framebuffer->colorAttachments.size()))
            .setPColorAttachments(framebuffer->colorAttachments.data())
            .setPDepthAttachment(framebuffer->depthAttachment.imageView ? &framebuffer->depthAttachment : nullptr)
            .setPStencilAttachment(framebuffer->stencilAttachment.imageView ? &framebuffer->stencilAttachment : nullptr);
        
        m_CurrentCmdBuf->cmdBuf.beginRendering(renderingInfo);
        m_CurrentCmdBuf->referencedResources.push_back(framebuffer);
    }

    void CommandList::endRenderPass()
    {
        if (m_CurrentGraphicsState.framebuffer || m_CurrentMeshletState.framebuffer)
        {
            m_CurrentCmdBuf->cmdBuf.endRendering();
            m_CurrentGraphicsState.framebuffer = nullptr;
            m_CurrentMeshletState.framebuffer = nullptr;
        }
    }

    static vk::Viewport VKViewportWithDXCoords(const Viewport& v)
    {
        return vk::Viewport(v.minX, v.maxY, v.maxX - v.minX, -(v.maxY - v.minY), v.minZ, v.maxZ);
    }

    void CommandList::setGraphicsState(const GraphicsState& state)
    {
        assert(m_CurrentCmdBuf);

        GraphicsPipeline* pso = checked_cast<GraphicsPipeline*>(state.pipeline);
        Framebuffer* fb = checked_cast<Framebuffer*>(state.framebuffer);

        if (m_EnableAutomaticBarriers)
        {
            insertGraphicsResourceBarriers(state);
        }

        bool anyBarriers = this->anyBarriers();
        bool updatePipeline = false;

        if (m_CurrentGraphicsState.pipeline != state.pipeline)
        {
            m_CurrentCmdBuf->cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pso->pipeline);
            nr::CountGpuCommand(nr::DevGpuCounter::Pipeline);

            m_CurrentCmdBuf->referencedResources.push_back(state.pipeline);
            updatePipeline = true;
        }

        if (m_CurrentGraphicsState.framebuffer != state.framebuffer || anyBarriers /* because barriers cannot be set inside a renderpass */)
        {
            endRenderPass();
        }

        commitBarriers();

        if(!m_CurrentGraphicsState.framebuffer)
        {
            beginRenderPass(fb);
        }

        m_CurrentPipelineLayout = pso->pipelineLayout;
        m_CurrentPushConstantsVisibility = pso->pushConstantVisibility;

        if (arraysAreDifferent(m_CurrentGraphicsState.bindings, state.bindings) || m_AnyVolatileBufferWrites)
        {
            bindBindingSets(vk::PipelineBindPoint::eGraphics, pso->pipelineLayout, state.bindings, pso->descriptorSetIdxToBindingIdx);
        }

        if (!state.viewport.viewports.empty() && arraysAreDifferent(state.viewport.viewports, m_CurrentGraphicsState.viewport.viewports))
        {
            nvrhi::static_vector<vk::Viewport, c_MaxViewports> viewports;
            for (const auto& vp : state.viewport.viewports)
            {
                viewports.push_back(VKViewportWithDXCoords(vp));
            }

            m_CurrentCmdBuf->cmdBuf.setViewport(0, uint32_t(viewports.size()), viewports.data());
            nr::CountGpuCommand(nr::DevGpuCounter::Viewport);
        }

        if (!state.viewport.scissorRects.empty() && arraysAreDifferent(state.viewport.scissorRects, m_CurrentGraphicsState.viewport.scissorRects))
        {
            nvrhi::static_vector<vk::Rect2D, c_MaxViewports> scissors;
            for (const auto& sc : state.viewport.scissorRects)
            {
                scissors.push_back(vk::Rect2D(vk::Offset2D(sc.minX, sc.minY),
                    vk::Extent2D(std::abs(sc.maxX - sc.minX), std::abs(sc.maxY - sc.minY))));
            }

            m_CurrentCmdBuf->cmdBuf.setScissor(0, uint32_t(scissors.size()), scissors.data());
            nr::CountGpuCommand(nr::DevGpuCounter::Scissor);
        }

        if (pso->dynamicFixedFunction && updatePipeline)
        {
            // KK patch: a pipeline linked from library parts.
            setLinkedPipelineDynamicState(m_CurrentCmdBuf->cmdBuf, *pso, state.dynamicStencilRefValue);
        }
        else if (pso->desc.renderState.depthStencilState.dynamicStencilRef && (updatePipeline || m_CurrentGraphicsState.dynamicStencilRefValue != state.dynamicStencilRefValue))
        {
            m_CurrentCmdBuf->cmdBuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack, state.dynamicStencilRefValue);
            nr::CountGpuCommand(nr::DevGpuCounter::Stencil);
        }

        if (pso->usesBlendConstants && (updatePipeline || m_CurrentGraphicsState.blendConstantColor != state.blendConstantColor))
        {
            m_CurrentCmdBuf->cmdBuf.setBlendConstants(&state.blendConstantColor.r);
            nr::CountGpuCommand(nr::DevGpuCounter::Blend);
        }

        if (state.indexBuffer.buffer && m_CurrentGraphicsState.indexBuffer != state.indexBuffer)
        {
            m_CurrentCmdBuf->cmdBuf.bindIndexBuffer(checked_cast<Buffer*>(state.indexBuffer.buffer)->buffer,
                state.indexBuffer.offset,
                state.indexBuffer.format == Format::R16_UINT ?
                vk::IndexType::eUint16 : vk::IndexType::eUint32);

            m_CurrentCmdBuf->referencedResources.push_back(state.indexBuffer.buffer);
        }

        if (!state.vertexBuffers.empty() && arraysAreDifferent(state.vertexBuffers, m_CurrentGraphicsState.vertexBuffers))
        {
            vk::Buffer vertexBuffers[c_MaxVertexAttributes];
            vk::DeviceSize vertexBufferOffsets[c_MaxVertexAttributes];
            uint32_t maxVbIndex = 0;

            for (const auto& binding : state.vertexBuffers)
            {
                // This is tested by the validation layer, skip invalid slots here if VL is not used.
                if (binding.slot >= c_MaxVertexAttributes)
                    continue;

                vertexBuffers[binding.slot] = checked_cast<Buffer*>(binding.buffer)->buffer;
                vertexBufferOffsets[binding.slot] = vk::DeviceSize(binding.offset);
                maxVbIndex = std::max(maxVbIndex, binding.slot);

                m_CurrentCmdBuf->referencedResources.push_back(binding.buffer);
            }

            m_CurrentCmdBuf->cmdBuf.bindVertexBuffers(0, maxVbIndex + 1, vertexBuffers, vertexBufferOffsets);
        }

        if (state.indirectParams)
        {
            m_CurrentCmdBuf->referencedResources.push_back(state.indirectParams);
        }
        
        if (state.indirectCountBuffer && state.indirectCountBuffer != state.indirectParams)
        {
            m_CurrentCmdBuf->referencedResources.push_back(state.indirectCountBuffer);
        }

        if (state.shadingRateState.enabled)
        {
            vk::FragmentShadingRateCombinerOpKHR combiners[2] = { convertShadingRateCombiner(state.shadingRateState.pipelinePrimitiveCombiner), convertShadingRateCombiner(state.shadingRateState.imageCombiner) };
            vk::Extent2D shadingRate = convertFragmentShadingRate(state.shadingRateState.shadingRate);
            m_CurrentCmdBuf->cmdBuf.setFragmentShadingRateKHR(&shadingRate, combiners);
        }

        m_CurrentGraphicsState = state;
        m_CurrentComputeState = ComputeState();
        m_CurrentMeshletState = MeshletState();
        m_CurrentRayTracingState = rt::State();
        m_AnyVolatileBufferWrites = false;
    }

    void CommandList::updateGraphicsVolatileBuffers()
    {
        if (m_AnyVolatileBufferWrites && m_CurrentGraphicsState.pipeline)
        {
            GraphicsPipeline* pso = checked_cast<GraphicsPipeline*>(m_CurrentGraphicsState.pipeline);

            bindBindingSets(vk::PipelineBindPoint::eGraphics, pso->pipelineLayout, m_CurrentGraphicsState.bindings, pso->descriptorSetIdxToBindingIdx);

            m_AnyVolatileBufferWrites = false;
        }
    }

    void CommandList::draw(const DrawArguments& args)
    {
        assert(m_CurrentCmdBuf);

        updateGraphicsVolatileBuffers();

        m_CurrentCmdBuf->cmdBuf.draw(args.vertexCount,
            args.instanceCount,
            args.startVertexLocation,
            args.startInstanceLocation);
    }

    void CommandList::drawIndexed(const DrawArguments& args)
    {
        assert(m_CurrentCmdBuf);

        updateGraphicsVolatileBuffers();

        m_CurrentCmdBuf->cmdBuf.drawIndexed(args.vertexCount,
            args.instanceCount,
            args.startIndexLocation,
            args.startVertexLocation,
            args.startInstanceLocation);
    }

    void CommandList::drawIndirect(uint32_t offsetBytes, uint32_t drawCount)
    {
        assert(m_CurrentCmdBuf);

        updateGraphicsVolatileBuffers();

        Buffer* indirectParams = checked_cast<Buffer*>(m_CurrentGraphicsState.indirectParams);
        assert(indirectParams);

        m_CurrentCmdBuf->cmdBuf.drawIndirect(indirectParams->buffer, offsetBytes, drawCount, sizeof(DrawIndirectArguments));
    }

    void CommandList::drawIndexedIndirect(uint32_t offsetBytes, uint32_t drawCount)
    {
        assert(m_CurrentCmdBuf);

        updateGraphicsVolatileBuffers();

        Buffer* indirectParams = checked_cast<Buffer*>(m_CurrentGraphicsState.indirectParams);
        assert(indirectParams);

        m_CurrentCmdBuf->cmdBuf.drawIndexedIndirect(indirectParams->buffer, offsetBytes, drawCount, sizeof(DrawIndexedIndirectArguments));
    }

    void CommandList::drawIndexedIndirectCount(uint32_t paramOffsetBytes, uint32_t countOffsetBytes, uint32_t maxDrawCount)
    {
        assert(m_CurrentCmdBuf);

        updateGraphicsVolatileBuffers();

        Buffer* paramBuffer = checked_cast<Buffer*>(m_CurrentGraphicsState.indirectParams);
        Buffer* countBuffer = checked_cast<Buffer*>(m_CurrentGraphicsState.indirectCountBuffer);
        assert(paramBuffer);
        assert(countBuffer);

        m_CurrentCmdBuf->cmdBuf.drawIndexedIndirectCount(
            paramBuffer->buffer,
            paramOffsetBytes,
            countBuffer->buffer,
            countOffsetBytes,
            maxDrawCount,
            sizeof(DrawIndexedIndirectArguments)
        );
    }

} // namespace nvrhi::vulkan
