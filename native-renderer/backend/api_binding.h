// The NrApi function table (plugin/native_api.h) bound to a DrawTracker. The
// plugin binds it to its backend's tracker (inactive until the guest GPU is
// set up); the tests bind it to a tracker of their own, so the same code path
// runs in both.
#pragma once

#include "plugin/native_api.h"

namespace nr {

class DrawTracker;

struct ApiBinding {
  void* owner = nullptr;
  // Returns the tracker to forward to, or null while inactive (calls are then
  // dropped and is_active returns 0).
  DrawTracker* (*resolve)(void* owner) = nullptr;
  NrApi api = {};
};

// Fills binding.api; api.self points at the binding, which must outlive it.
void InitApiBinding(ApiBinding& binding, void* owner, DrawTracker* (*resolve)(void* owner));

}  // namespace nr
