// Test-only forwarding device for allocation fault injection.
#pragma once
#include <nvrhi/nvrhi.h>
namespace nvrhi {
class TestDeviceProxy : public IDevice {
 public:
  explicit TestDeviceProxy(IDevice* device) : device_(device) {}
  unsigned long AddRef() override { return device_->AddRef(); }
  unsigned long Release() override { return device_->Release(); }
  unsigned long GetRefCount() override { return device_->GetRefCount(); }
  Object getNativeObject(ObjectType type) override { return device_->getNativeObject(type); }
  HeapHandle createHeap(const HeapDesc& d) override { return device_->createHeap(d); }
  bool queryTopLevelAccelStructPrebuildInfo(const rt::AccelStructDesc& desc, uint32_t instanceCount, rt::AccelStructPrebuildInfo& outInfo) override { return device_->queryTopLevelAccelStructPrebuildInfo(desc, instanceCount, outInfo); }
  TextureHandle createTexture(const TextureDesc& d) override { return device_->createTexture(d); }
  MemoryRequirements getTextureMemoryRequirements(ITexture* texture) override { return device_->getTextureMemoryRequirements(texture); }
  bool bindTextureMemory(ITexture* texture, IHeap* heap, uint64_t offset) override { return device_->bindTextureMemory(texture, heap, offset); }
  TextureHandle createHandleForNativeTexture(ObjectType objectType, Object texture, const TextureDesc& desc) override { return device_->createHandleForNativeTexture(objectType, texture, desc); }
  StagingTextureHandle createStagingTexture(const TextureDesc& d, CpuAccessMode cpuAccess) override { return device_->createStagingTexture(d, cpuAccess); }
  void* mapStagingTexture(IStagingTexture* tex, const TextureSlice& slice, CpuAccessMode cpuAccess, size_t* outRowPitch) override { return device_->mapStagingTexture(tex, slice, cpuAccess, outRowPitch); }
  void unmapStagingTexture(IStagingTexture* tex) override { return device_->unmapStagingTexture(tex); }
  void getTextureTiling(ITexture* texture, uint32_t* numTiles, PackedMipDesc* desc, TileShape* tileShape, uint32_t* subresourceTilingsNum, SubresourceTiling* subresourceTilings) override { return device_->getTextureTiling(texture, numTiles, desc, tileShape, subresourceTilingsNum, subresourceTilings); }
  void updateTextureTileMappings(ITexture* texture, const TextureTilesMapping* tileMappings, uint32_t numTileMappings, CommandQueue executionQueue) override { return device_->updateTextureTileMappings(texture, tileMappings, numTileMappings, executionQueue); }
  SamplerFeedbackTextureHandle createSamplerFeedbackTexture(ITexture* pairedTexture, const SamplerFeedbackTextureDesc& desc) override { return device_->createSamplerFeedbackTexture(pairedTexture, desc); }
  SamplerFeedbackTextureHandle createSamplerFeedbackForNativeTexture(ObjectType objectType, Object texture, ITexture* pairedTexture) override { return device_->createSamplerFeedbackForNativeTexture(objectType, texture, pairedTexture); }
  BufferHandle createBuffer(const BufferDesc& d) override { return device_->createBuffer(d); }
  void* mapBuffer(IBuffer* buffer, CpuAccessMode cpuAccess) override { return device_->mapBuffer(buffer, cpuAccess); }
  void unmapBuffer(IBuffer* buffer) override { return device_->unmapBuffer(buffer); }
  MemoryRequirements getBufferMemoryRequirements(IBuffer* buffer) override { return device_->getBufferMemoryRequirements(buffer); }
  bool bindBufferMemory(IBuffer* buffer, IHeap* heap, uint64_t offset) override { return device_->bindBufferMemory(buffer, heap, offset); }
  BufferHandle createHandleForNativeBuffer(ObjectType objectType, Object buffer, const BufferDesc& desc) override { return device_->createHandleForNativeBuffer(objectType, buffer, desc); }
  ShaderHandle createShader(const ShaderDesc& d, const void* binary, size_t binarySize) override { return device_->createShader(d, binary, binarySize); }
  ShaderHandle createShaderSpecialization(IShader* baseShader, const ShaderSpecialization* constants, uint32_t numConstants) override { return device_->createShaderSpecialization(baseShader, constants, numConstants); }
  ShaderLibraryHandle createShaderLibrary(const void* binary, size_t binarySize) override { return device_->createShaderLibrary(binary, binarySize); }
  SamplerHandle createSampler(const SamplerDesc& d) override { return device_->createSampler(d); }
  InputLayoutHandle createInputLayout(const VertexAttributeDesc* d, uint32_t attributeCount, IShader* vertexShader) override { return device_->createInputLayout(d, attributeCount, vertexShader); }
  EventQueryHandle createEventQuery() override { return device_->createEventQuery(); }
  void setEventQuery(IEventQuery* query, CommandQueue queue) override { return device_->setEventQuery(query, queue); }
  bool pollEventQuery(IEventQuery* query) override { return device_->pollEventQuery(query); }
  void waitEventQuery(IEventQuery* query) override { return device_->waitEventQuery(query); }
  void resetEventQuery(IEventQuery* query) override { return device_->resetEventQuery(query); }
  TimerQueryHandle createTimerQuery() override { return device_->createTimerQuery(); }
  bool pollTimerQuery(ITimerQuery* query) override { return device_->pollTimerQuery(query); }
  float getTimerQueryTime(ITimerQuery* query) override { return device_->getTimerQueryTime(query); }
  void resetTimerQuery(ITimerQuery* query) override { return device_->resetTimerQuery(query); }
  GraphicsAPI getGraphicsAPI() override { return device_->getGraphicsAPI(); }
  FramebufferHandle createFramebuffer(const FramebufferDesc& desc) override { return device_->createFramebuffer(desc); }
  GraphicsPipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc, FramebufferInfo const& fbinfo) override { return device_->createGraphicsPipeline(desc, fbinfo); }
  GraphicsPipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc, IFramebuffer* fb) override { return device_->createGraphicsPipeline(desc, fb->getFramebufferInfo()); }
  ComputePipelineHandle createComputePipeline(const ComputePipelineDesc& desc) override { return device_->createComputePipeline(desc); }
  MeshletPipelineHandle createMeshletPipeline(const MeshletPipelineDesc& desc, FramebufferInfo const& fbinfo) override { return device_->createMeshletPipeline(desc, fbinfo); }
  MeshletPipelineHandle createMeshletPipeline(const MeshletPipelineDesc& desc, IFramebuffer* fb) override { return device_->createMeshletPipeline(desc, fb->getFramebufferInfo()); }
  rt::PipelineHandle createRayTracingPipeline(const rt::PipelineDesc& desc) override { return device_->createRayTracingPipeline(desc); }
  BindingLayoutHandle createBindingLayout(const BindingLayoutDesc& desc) override { return device_->createBindingLayout(desc); }
  BindingLayoutHandle createBindlessLayout(const BindlessLayoutDesc& desc) override { return device_->createBindlessLayout(desc); }
  BindingSetHandle createBindingSet(const BindingSetDesc& desc, IBindingLayout* layout) override { return device_->createBindingSet(desc, layout); }
  DescriptorTableHandle createDescriptorTable(IBindingLayout* layout) override { return device_->createDescriptorTable(layout); }
  void resizeDescriptorTable(IDescriptorTable* descriptorTable, uint32_t newSize, bool keepContents) override { return device_->resizeDescriptorTable(descriptorTable, newSize, keepContents); }
  bool writeDescriptorTable(IDescriptorTable* descriptorTable, const BindingSetItem& item) override { return device_->writeDescriptorTable(descriptorTable, item); }
  rt::OpacityMicromapHandle createOpacityMicromap(const rt::OpacityMicromapDesc& desc) override { return device_->createOpacityMicromap(desc); }
  rt::AccelStructHandle createAccelStruct(const rt::AccelStructDesc& desc) override { return device_->createAccelStruct(desc); }
  MemoryRequirements getAccelStructMemoryRequirements(rt::IAccelStruct* as) override { return device_->getAccelStructMemoryRequirements(as); }
  rt::cluster::OperationSizeInfo getClusterOperationSizeInfo(const rt::cluster::OperationParams& params) override { return device_->getClusterOperationSizeInfo(params); }
  bool bindAccelStructMemory(rt::IAccelStruct* as, IHeap* heap, uint64_t offset) override { return device_->bindAccelStructMemory(as, heap, offset); }
  CommandListHandle createCommandList(const CommandListParameters& params) override { return device_->createCommandList(params); }
  uint64_t executeCommandLists(ICommandList* const* pCommandLists, size_t numCommandLists, CommandQueue executionQueue) override { return device_->executeCommandLists(pCommandLists, numCommandLists, executionQueue); }
  void queueWaitForCommandList(CommandQueue waitQueue, CommandQueue executionQueue, uint64_t instance) override { return device_->queueWaitForCommandList(waitQueue, executionQueue, instance); }
  bool waitForIdle() override { return device_->waitForIdle(); }
  CommandListLifetimeTrackerHandle createCommandListLifetimeTracker(CommandQueue executionQueue) override { return device_->createCommandListLifetimeTracker(executionQueue); }
  void runGarbageCollection() override { return device_->runGarbageCollection(); }
  bool queryFeatureSupport(Feature feature, void* pInfo, size_t infoSize) override { return device_->queryFeatureSupport(feature, pInfo, infoSize); }
  FormatSupport queryFormatSupport(Format format) override { return device_->queryFormatSupport(format); }
  coopvec::DeviceFeatures queryCoopVecFeatures() override { return device_->queryCoopVecFeatures(); }
  coopvec::MatMulFormatSupport queryCoopVecMatMulFormatSupport(const coopvec::MatMulFormatCombo& combination) override { return device_->queryCoopVecMatMulFormatSupport(combination); }
  coopvec::TrainingFormatSupport queryCoopVecTrainingFormatSupport(coopvec::DataType componentType) override { return device_->queryCoopVecTrainingFormatSupport(componentType); }
  size_t getCoopVecMatrixSize(coopvec::DataType type, coopvec::MatrixLayout layout, int rows, int columns) override { return device_->getCoopVecMatrixSize(type, layout, rows, columns); }
  Object getNativeQueue(ObjectType objectType, CommandQueue queue) override { return device_->getNativeQueue(objectType, queue); }
  IMessageCallback* getMessageCallback() override { return device_->getMessageCallback(); }
  bool isAftermathEnabled() override { return device_->isAftermathEnabled(); }
  AftermathCrashDumpHelper& getAftermathCrashDumpHelper() override { return device_->getAftermathCrashDumpHelper(); }
 protected:
  IDevice* device_;
};
}  // namespace nvrhi
