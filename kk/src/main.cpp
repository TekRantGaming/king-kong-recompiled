// king_kong - ReXGlue Recompiled Project

#include "generated/default/king_kong_init.h"

#include "king_kong_app.h"

REX_DEFINE_APP(king_kong, KingKongApp::Create)

#ifdef _WIN32
// Hybrid laptops (Intel/AMD iGPU + NVIDIA/AMD dGPU): the drivers start an exe that exports these
// on the discrete GPU (task 203; Beta 7/8 crash reports from an Intel UHD + RTX 4050 laptop).
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
