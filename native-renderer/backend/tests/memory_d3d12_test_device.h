// Windowless host for the budget migration regression, using only existing
// system D3D12 components. The native debug layer is optional, never installed.
#pragma once
#if NR_HAS_D3D12
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>
#include "backend/tests/readback.h"

namespace nr::test {
class MemoryD3D12TestDevice final : private nvrhi::IMessageCallback {
 public:
  static std::unique_ptr<MemoryD3D12TestDevice> Create() {
    auto host = std::unique_ptr<MemoryD3D12TestDevice>(new MemoryD3D12TestDevice);
    Microsoft::WRL::ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer(); host->native_validation_ = true;
    }
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&host->native_)))) return {};
    D3D12_COMMAND_QUEUE_DESC queue = {};
    if (FAILED(host->native_->CreateCommandQueue(&queue, IID_PPV_ARGS(&host->queue_)))) return {};
    host->native_.As(&host->info_);
    nvrhi::d3d12::DeviceDesc desc;
    desc.pDevice = host->native_.Get(); desc.pGraphicsCommandQueue = host->queue_.Get();
    desc.errorCB = host.get(); desc.shaderResourceViewHeapSize = 65536; desc.samplerHeapSize = 2048;
    auto backend = nvrhi::d3d12::createDevice(desc);
    if (!backend) return {};
    host->device_ = nvrhi::validation::createValidationLayer(backend);
    return host;
  }
  ~MemoryD3D12TestDevice() { if (device_) { WaitIdle(); device_ = nullptr; } }
  nvrhi::IDevice* device() const { return device_; }
  std::string device_name() const { return "D3D12 default hardware adapter"; }
  bool native_validation() const { return native_validation_; }
  void Execute(nvrhi::ICommandList* cl) { device_->executeCommandList(cl); }
  void WaitIdle() { device_->waitForIdle(); device_->runGarbageCollection(); }
  Pixels ReadBack(nvrhi::ITexture* texture) { return ReadBackTexture(device_, texture); }
  int errors() const {
    int total = errors_.load();
    if (info_) for (UINT64 i = 0; i < info_->GetNumStoredMessages(); ++i) {
      SIZE_T size = 0;
      if (FAILED(info_->GetMessage(i, nullptr, &size))) { ++total; continue; }
      std::vector<uint8_t> bytes(size);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
      if (FAILED(info_->GetMessage(i, message, &size))) { ++total; continue; }
      if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
        std::fprintf(stderr, "[D3D12 validation] %s\n", message->pDescription); ++total;
      }
    }
    return total;
  }
 private:
  void message(nvrhi::MessageSeverity severity, const char* text) override {
    if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal) {
      ++errors_; std::fprintf(stderr, "[NVRHI] %s\n", text);
    }
  }
  Microsoft::WRL::ComPtr<ID3D12Device> native_;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
  Microsoft::WRL::ComPtr<ID3D12InfoQueue> info_;
  nvrhi::DeviceHandle device_;
  std::atomic<int> errors_{0};
  bool native_validation_ = false;
};
}  // namespace nr::test
#endif
