#pragma once

namespace kk {
// Developer-only sampler of the renderer's existing, locked benchmark snapshot.
// KK_DEV_PREPARE_TRACE=<csv>: 50 Hz, no work added to the game's frame hooks.
void DevStartPreparationTrace();
void DevStopPreparationTrace();
}
