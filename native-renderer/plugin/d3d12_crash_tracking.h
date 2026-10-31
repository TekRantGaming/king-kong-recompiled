// D3D12 crash tracking: Device Removed Extended Data, Windows' record of a lost device. Auto breadcrumbs say which
// GPU operation each command list had reached, the page fault output which allocation a bad GPU address hit. It has
// to be on before the device is made, and is only read once the device is gone (OnHostGpuLoss). Older Windows
// without the interface goes on without it.
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

#include <fmt/format.h>

namespace nr {

inline void EnableD3d12CrashTracking() {
  ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
  if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)))) return;
  settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  settings->Release();
}

inline std::string D3d12BreadcrumbName(D3D12_AUTO_BREADCRUMB_OP op) {
  switch (op) {
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "draw";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "indexed draw";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "indirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "copy buffer";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "copy texture";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "copy resource";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "resolve";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "clear colour";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "clear UAV";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "clear depth";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "barrier";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "present";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "resolve query";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "begin submission";
    case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "end submission";
    default: return fmt::format("operation {}", int(op));
  }
}

// `line` gets each finding. Only the lists the GPU had not finished; a few of them, the log has to stay readable.
template <typename Line>
void LogD3d12CrashTracking(ID3D12Device* device, Line&& line) {
  ID3D12DeviceRemovedExtendedData* dred = nullptr;
  if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
    line("D3D12 crash tracking: not available on this Windows");
    return;
  }
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
    int shown = 0, unfinished = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
      const uint32_t done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
      if (done >= node->BreadcrumbCount) continue;
      ++unfinished;
      if (shown++ >= 6) continue;
      line(fmt::format("D3D12 crash tracking: list '{}' on queue '{}': {} of {} operations finished, next: {}",
                       node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "unnamed",
                       node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "unnamed", done,
                       node->BreadcrumbCount,
                       node->pCommandHistory ? D3d12BreadcrumbName(node->pCommandHistory[done]) : "unknown"));
    }
    line(fmt::format("D3D12 crash tracking: {} unfinished command lists", unfinished));
  } else {
    line("D3D12 crash tracking: no breadcrumbs");
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault))) {
    if (fault.PageFaultVA) line(fmt::format("D3D12 crash tracking: page fault at GPU address 0x{:X}", fault.PageFaultVA));
    int shown = 0;
    const auto list = [&](const D3D12_DRED_ALLOCATION_NODE* node, const char* kind) {
      for (; node && shown < 8; node = node->pNext, ++shown)
        line(fmt::format("D3D12 crash tracking: {} allocation there: '{}' (type {})", kind,
                         node->ObjectNameA ? node->ObjectNameA : "unnamed", int(node->AllocationType)));
    };
    list(fault.pHeadExistingAllocationNode, "live");
    list(fault.pHeadRecentFreedAllocationNode, "freed");
  }
  dred->Release();
}

}  // namespace nr
