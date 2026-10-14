// The launcher's upscaler question in the plugin (native_upscaler_query.h is its C interface).
#pragma once

#include <cstdint>

#include "plugin/native_upscaler_query.h"

namespace nr {

// Starts the question once a process (on a thread of its own) and returns its state; fills `out` when
// it is done.
uint32_t UpscalerQuery(NrUpscalerSupport* out);
// The answer if the question was asked (the launcher asks it), without asking it: the graphics panel
// (F8) runs while the game's device exists, when a device of the query's own must not be made.
uint32_t UpscalerQueryPeek(NrUpscalerSupport* out);
uint32_t UpscalerQueryAnswer(NrUpscalerSupport* out, bool start);
// Returns once the question's thread is done (at once when it never started).
void WaitUpscalerQuery();

}  // namespace nr
