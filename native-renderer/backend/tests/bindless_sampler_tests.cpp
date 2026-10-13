#include <cstdio>
#include <wrl/client.h>
#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>

struct Messages final : nvrhi::IMessageCallback {
  int errors = 0;
  void message(nvrhi::MessageSeverity severity, const char* text) override {
    if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal) {
      ++errors;
      std::fprintf(stderr, "%s\n", text);
    }
  }
};

int main() {
  Microsoft::WRL::ComPtr<ID3D12Device> native;
  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&native)))) return 1;
  Messages messages;
  nvrhi::d3d12::DeviceDesc deviceDesc;
  deviceDesc.pDevice = native.Get();
  deviceDesc.errorCB = &messages;
  auto backend = nvrhi::d3d12::createDevice(deviceDesc);
  if (!backend) return 2;
  auto device = nvrhi::validation::createValidationLayer(backend);
  nvrhi::BindlessLayoutDesc desc;
  desc.visibility = nvrhi::ShaderType::All;
  desc.maxCapacity = 1024;
  desc.registerSpaces.push_back(nvrhi::BindingLayoutItem::Sampler(5));
  auto layout = device->createBindlessLayout(desc);
  if (!layout || messages.errors) return 3;
  auto table = device->createDescriptorTable(layout);
  if (!table) return 4;
  device->resizeDescriptorTable(table, 1024, false);
  if (table->getCapacity() != 1024) return 4;
  auto sampler = device->createSampler(nvrhi::SamplerDesc());
  if (!sampler || !device->writeDescriptorTable(table, nvrhi::BindingSetItem::Sampler(0, sampler))) return 5;
  if (messages.errors) return 6;
  desc.registerSpaces.push_back(nvrhi::BindingLayoutItem::Texture_SRV(1));
  if (device->createBindlessLayout(desc) || messages.errors != 1) return 7;
  std::puts("PASS: D3D12 sampler-only table accepts 1024 slots and sampler writes; mixed table rejected");
  return 0;
}
