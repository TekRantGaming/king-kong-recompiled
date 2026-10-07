// Where guest memory starts in the host address space (guest address 0), set
// once the runtime is up. For code that runs outside a hook (no ctx/base).

#pragma once

#include <cstdint>

namespace kk {
inline uint8_t* g_guest_base = nullptr;
}  // namespace kk
