// Replays recorded Direct3D call sequences through the hook table. The input
// is the call log kk/src/dev_d3d_trace.cpp writes (KK_DEV_D3D_TRACE), one call
// per line:
//   ... KK d3d: sub_82115708 lr=82793C10 r3=4006A580 r4=00000004 ... r10=... f1=0
// Other lines are ignored. Tests write sequences in the same format, so a
// real trace from the Windows side replays the same way (without guest memory
// it exercises the call routing and state tracking; draws that need buffer
// contents are counted as dropped).
#pragma once

#include <cstdint>
#include <istream>
#include <optional>
#include <string_view>

#include "hooks/hook_table.h"

namespace nr::hooks {

struct TraceCall {
  uint32_t address = 0;
  GuestArgs args;
};

// Parses one log line; nullopt when it is not a call line.
std::optional<TraceCall> ParseTraceLine(std::string_view line);

struct ReplayStats {
  uint64_t lines = 0;
  uint64_t calls = 0;     // call lines parsed
  uint64_t hooked = 0;    // of those, entry points in the hook table
  uint64_t unhooked = 0;  // traced functions the native renderer does not hook
};

// Runs every call line of `log` through Run() with an empty original (the
// log lists nested calls on their own lines, so each is replayed as a call
// of its own).
ReplayStats ReplayTrace(std::istream& log, const NrApi* api);

}  // namespace nr::hooks
