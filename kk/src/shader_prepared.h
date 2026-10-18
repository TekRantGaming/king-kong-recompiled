// The ONE definition of "the shaders are prepared" (owner, 18 Oct 2026, DECISIONS.md): the native renderer's
// shaders are prepared exactly once per port version, at the first setup or after an update, and never again
// unless the player asks (Rebuild shaders). The setup's end, Home's SHADERS status, What's new and Play all ask
// ShadersPrepared(); nothing else decides it. Kept free of the launcher so tests check every case
// (tests/launcher_logic_tests.cpp).
//
// Beta 8 and the Beta 9 test build stored only the version in kk_shaders_prepared, wrote it for a job that was
// cancelled or ended early, and Home read the launcher's own job instead: a launcher that found the shaders
// prepared started no job, so Home said "Not prepared" on every start after the setup.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace kk {

// The packs (NR_SHADER_PACK_SPIRV / NR_SHADER_PACK_DXIL in native-renderer/plugin/native_shader_pack_api.h).
constexpr int kShaderPackSpirv = 1;
constexpr int kShaderPackDxil = 2;
constexpr int kShaderPackAll = kShaderPackSpirv | kShaderPackDxil;

inline const char* ShaderPackFileName(int pack) {
  return pack == kShaderPackSpirv ? "kkshaders-spirv.pack" : "kkshaders-dxil.pack";
}

// kk_shaders_prepared: "<version>;<packs>", e.g. "2.0.0-beta.9;spirv,dxil": the port version whose one
// preparation finished, and the packs it covered on this PC (both graphics APIs on Windows, Vulkan on Linux,
// less an API that failed to start here). A value without the packs (Beta 8, Beta 9 test build) is not trusted.
struct PreparedMarker {
  std::string version;
  int packs = 0;
};

inline std::string FormatPreparedMarker(const std::string& version, int packs) {
  std::string out = version + ";";
  if (packs & kShaderPackSpirv) out += "spirv";
  if (packs & kShaderPackDxil) out += (packs & kShaderPackSpirv) ? ",dxil" : "dxil";
  return out;
}

inline PreparedMarker ParsePreparedMarker(std::string_view text) {
  PreparedMarker m;
  const size_t semi = text.find(';');
  if (semi == std::string_view::npos) return m;  // the old form: not trusted
  m.version = std::string(text.substr(0, semi));
  std::string_view rest = text.substr(semi + 1);
  while (!rest.empty()) {
    const size_t comma = rest.find(',');
    const std::string_view name = rest.substr(0, comma);
    if (name == "spirv") m.packs |= kShaderPackSpirv;
    else if (name == "dxil") m.packs |= kShaderPackDxil;
    if (comma == std::string_view::npos) break;
    rest = rest.substr(comma + 1);
  }
  return m;
}

// The definition. Prepared for this version when the marker names this version and at least one pack, and every
// pack it names is still in the cache (`present`: the packs found in <cache>/native-shaders). Settings changed
// after the setup do not matter: the packs carry every variant the settings choose between (render scale,
// motion vectors, water), and a pipeline a new setting needs is made by the game in the background.
inline bool ShadersPrepared(std::string_view marker, std::string_view version, int present) {
  const PreparedMarker m = ParsePreparedMarker(marker);
  return !m.version.empty() && m.version == version && m.packs != 0 && (present & m.packs) == m.packs;
}

// How a shader job ended (NrShaderPackProgress once finished).
struct ShaderJobEnd {
  bool finished = false;
  bool cancelled = false;
  int packs_needed = 0;  // the packs the job covers on this PC
  int packs_ready = 0;   // those it left built and current
};

// The job counts as the version's one preparation: it ran to its end (not cancelled) and left every pack it
// covers. Pipelines it could not make (a list record naming a missing shader, a second API's device that
// would not start) do not count against it: the game makes those as it meets them, and the log says which.
inline bool JobIsThePreparation(const ShaderJobEnd& e) {
  return e.finished && !e.cancelled && e.packs_needed != 0 && (e.packs_ready & e.packs_needed) == e.packs_needed;
}

// The launcher opening: after an update everything is built again in full (once); otherwise a job starts only
// when this version's preparation has not finished yet (a first start, or a setup the player closed half way).
enum class ShaderJobAtStart { kNone, kPrepare, kRebuild };
inline ShaderJobAtStart JobAtLauncherStart(bool updated, bool prepared) {
  if (updated) return ShaderJobAtStart::kRebuild;
  return prepared ? ShaderJobAtStart::kNone : ShaderJobAtStart::kPrepare;
}

// Play: a job still running finishes first; prepared shaders go straight to the game; a version not prepared yet
// whose job has not run in this launcher prepares them first (once); a job that already ended without preparing
// them in this launcher is not repeated (the game compiles what it needs as it starts).
enum class ShaderPlayStep { kPlay, kWaitForJob, kPrepareFirst };
inline ShaderPlayStep AtPlay(bool busy, bool prepared, bool finished_here) {
  if (busy) return ShaderPlayStep::kWaitForJob;
  if (prepared || finished_here) return ShaderPlayStep::kPlay;
  return ShaderPlayStep::kPrepareFirst;
}

// Home's SHADERS status (and the Graphics > Advanced row's).
enum class ShaderHomeStatus { kChecking, kPreparing, kReady, kFailed, kNotPrepared };
inline ShaderHomeStatus HomeStatus(bool busy, bool working, bool prepared, bool finished_here, bool cancelled) {
  if (busy) return working ? ShaderHomeStatus::kPreparing : ShaderHomeStatus::kChecking;
  if (prepared) return ShaderHomeStatus::kReady;
  if (finished_here && !cancelled) return ShaderHomeStatus::kFailed;
  return ShaderHomeStatus::kNotPrepared;
}

}  // namespace kk
