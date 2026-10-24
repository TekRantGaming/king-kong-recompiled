#include "coop_resume.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#include "coop_handover.h"
#include "coop_runtime.h"
#include "handoff.h"
#include "ui_paint.h"

REXCVAR_DECLARE(std::string, kk_coop_resume);

namespace kk::coop {
namespace {
using Clock = std::chrono::steady_clock;
double Now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
long long UnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string requested_name;
bool requested_read = false;
const char* ParseName(handover::ParseResult r) {
  switch (r) {
    case handover::ParseResult::Ok: return "ok";
    case handover::ParseResult::Malformed: return "malformed";
    case handover::ParseResult::Expired: return "stale (expired)";
    case handover::ParseResult::Protocol: return "from another version";
  }
  return "?";
}
#if defined(KK_DEV_TOOLS)
bool DevFlag(const char* name) { const char* v = std::getenv(name); return v && *v && std::string_view(v) != "0"; }
std::string DevText(const char* name) { const char* v = std::getenv(name); return v ? v : ""; }
#endif

class ResumeDialog final : public rex::ui::ImGuiDialog {
 public:
  ResumeDialog(rex::ui::ImGuiDrawer* drawer, handover::File file, ResumeCallbacks callbacks)
      : ImGuiDialog(drawer), file_(std::move(file)), cb_(std::move(callbacks)) {
    request_.kind = file_.plan.continue_game ? play::StartKind::kContinue : play::StartKind::kChapter;
    request_.slot = file_.host ? file_.slot : -1;
    request_.chapter = file_.plan.continue_game ? -1 : int(file_.plan.chapter);
    if (request_.kind == play::StartKind::kChapter && request_.slot < 0) request_.slot = 0;
    StartLink();
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (done_) return;
    const double now = Now();
    if (!drawn_) { drawn_ = true; handoff::ContentDrawn(); }  // the old process may close now
    Step(now);
    if (done_) return;
    Paint(io, now);
  }

 private:
  void StartLink() {
    machine_.Begin(Now());
    begun_ = false; relink_at_ = -1; auto_choice_at_ = -1; failure_logged_ = false;
#if defined(KK_DEV_TOOLS)
    if (DevFlag("KK_DEV_RESUME_STALL")) {
      REXLOG_INFO("KK dev co-op hand over: KK_DEV_RESUME_STALL: this process never reconnects (timeout test)");
      return;
    }
#endif
    if (!ResumeLink(file_)) REXLOG_WARN("KK co-op hand over: the {} link could not start",
                                        file_.plan.transport == StartTransport::Steam ? "Steam" : "Direct IP");
    else REXLOG_INFO("KK co-op hand over: resumed {} reconnecting over {}", file_.host ? "host" : "guest",
                     file_.plan.transport == StartTransport::Steam ? "Steam" : "Direct IP");
  }
  void Step(double now) {
    const auto link = CurrentLink();
    const auto flow = SessionFlowSnapshot();
    handover::ResumeInput in;
    in.state = link.state; in.rejection = link.rejection;
    in.start_ready = flow.start_ready && flow.phase != FlowPhase::Failed;
    in.flow_failed = flow.active && flow.phase == FlowPhase::Failed;
    const auto before = machine_.Phase();
    if (relink_at_ >= 0) {
      if (now < relink_at_) { in.state = SessionState::Connecting; }
      else { relink_at_ = -1; StopResumeLink(); StartLinkKeepClock(); in.state = SessionState::Connecting; }
    }
    const bool begin = machine_.Tick(now, in);
    if (machine_.TakeRelink() && relink_at_ < 0) {
      relink_at_ = now + 1;
      REXLOG_INFO("KK co-op hand over: the link ended before the partner arrived; trying again ({:.0f} s left)",
                  machine_.Remaining(now));
    }
    if (machine_.Phase() != before && machine_.Phase() == handover::ResumePhase::Starting)
      REXLOG_INFO("KK co-op hand over: reconnected to the partner after {:.1f} s", handover::kReconnectSeconds - machine_.Remaining(now));
    if (begin && file_.host && !begun_) {
      begun_ = true;
      if (!BeginResumedStart(request_)) machine_.LaunchFailed();
    }
    if (machine_.Phase() == handover::ResumePhase::Launched) {
      play::StartRequest r;
      if (TakeSessionLaunch(r)) {
        REXLOG_INFO("KK co-op hand over: both reconnected; the game starts chapter {} together", r.chapter);
        done_ = true;
        cb_.play(r);
        return;
      }
      machine_.LaunchFailed();
    }
    if (machine_.Phase() == handover::ResumePhase::Failed && !failure_logged_) {
      failure_logged_ = true;
      REXLOG_WARN("KK co-op hand over: couldn't reach the partner: {}", handover::FailureText(machine_.Failure()));
      StopResumeLink();
#if defined(KK_DEV_TOOLS)
      if (!DevText("KK_DEV_RESUME_CHOICE").empty()) auto_choice_at_ = now + 3;
#endif
    }
#if defined(KK_DEV_TOOLS)
    if (auto_choice_at_ >= 0 && now >= auto_choice_at_) {
      auto_choice_at_ = -1;
      const auto choice = DevText("KK_DEV_RESUME_CHOICE");
      REXLOG_INFO("KK dev co-op hand over: KK_DEV_RESUME_CHOICE={}", choice);
      if (choice == "retry") TryAgain(); else BackToMenu();
    }
#endif
  }
  void StartLinkKeepClock() {
    if (!ResumeLink(file_)) REXLOG_WARN("KK co-op hand over: the link could not start again");
  }
  void TryAgain() {
    REXLOG_INFO("KK co-op hand over: Try again");
    StopResumeLink();
    StartLink();
  }
  void BackToMenu() {
    if (leaving_) return;
    leaving_ = true;
    REXLOG_INFO("KK co-op hand over: Back to menu (the launcher with the scene)");
    StopResumeLink();
    cb_.back_to_menu();
  }
  void Paint(ImGuiIO& io, double now) {
    RequestUiPaint();
    const auto* vp = ImGui::GetMainViewport();
    auto* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(0, 0, 0, 255));
    const float scale = std::clamp(io.DisplaySize.y / 720.f, .65f, 2.f);
    ImFont* font = ImGui::GetFont();
    const float size = 22.f * scale;
    auto centered = [&](const std::string& text, float y, ImU32 color, float s) {
      const ImVec2 m = font->CalcTextSizeA(s, 1e6f, 0, text.c_str());
      bg->AddText(font, s, ImVec2(vp->Pos.x + (vp->Size.x - m.x) * .5f, vp->Pos.y + y), color, text.c_str());
    };
    const std::string& name = file_.partner_name;
    if (machine_.Phase() != handover::ResumePhase::Failed) {
      const int dots = int(now * 2) % 4;
      std::string line = (machine_.Connected() ? "Starting the chapter with " : "Connecting to ") + name + std::string(size_t(dots), '.');
      centered(line, vp->Size.y * .46f, IM_COL32(235, 235, 230, 255), size);
      if (!machine_.Connected()) {
        char left[64]; std::snprintf(left, sizeof(left), "%d s", std::max(0, int(machine_.Remaining(now) + .99)));
        centered(left, vp->Size.y * .46f + size * 1.6f, IM_COL32(160, 160, 155, 255), size * .7f);
      }
      return;
    }
    centered("Couldn't reach " + name + ".", vp->Size.y * .40f, IM_COL32(240, 240, 235, 255), size);
    centered(handover::FailureText(machine_.Failure()), vp->Size.y * .40f + size * 1.6f, IM_COL32(170, 170, 165, 255), size * .75f);
    // Buttons: mouse, keyboard (Enter / Escape) and pad (A / B).
    const ImVec2 button(220 * scale, 44 * scale);
    const float y = vp->Size.y * .40f + size * 4.f;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * .5f - button.x - 12 * scale, vp->Pos.y + y), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0);
    // The launcher's quiet look: dark buttons, a light edge on hover, no frame around them.
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(38, 38, 36, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(72, 70, 64, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(200, 160, 90, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 235, 230, 255));
    if (ImGui::Begin("##coop-resume", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
      ImGui::SetWindowFontScale(scale);
      if (ImGui::Button("Try again (A)", button)) TryAgain();
      ImGui::SameLine(0, 24 * scale);
      if (ImGui::Button("Back to menu (B)", button)) BackToMenu();
    }
    ImGui::End();
    ImGui::PopStyleColor(5);
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) TryAgain();
    else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) BackToMenu();
  }

  handover::File file_;
  ResumeCallbacks cb_;
  play::StartRequest request_;
  handover::ResumeMachine machine_;
  bool done_ = false, drawn_ = false, begun_ = false, leaving_ = false, failure_logged_ = false;
  double relink_at_ = -1, auto_choice_at_ = -1;
};
}  // namespace

bool ResumeRequested() {
  if (!requested_read) {
    requested_read = true;
    requested_name = REXCVAR_GET(kk_coop_resume);
    rex::cvar::ResetToDefault("kk_coop_resume");  // never saved
  }
  return !requested_name.empty();
}

void BeginResume(rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& user_dir, ResumeCallbacks callbacks) {
  ResumeRequested();
  handover::File file;
  auto result = handover::ParseResult::Malformed;
  if (!handover::SafeFileName(requested_name)) {
    REXLOG_WARN("KK co-op hand over: ignored an unsafe hand over file name");
  } else {
    const auto path = user_dir / requested_name;
    std::ifstream in(path, std::ios::binary);
    std::string text;
    if (in) {
      text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      in.close();
      if (text.size() > 4096) text.clear();
    }
    // The file is used once: a later start never resumes this session.
    std::error_code ec;
    std::filesystem::remove(path, ec);
    result = text.empty() ? handover::ParseResult::Malformed : handover::Parse(text, UnixMs(), KK_VERSION, file);
    REXLOG_INFO("KK co-op hand over: hand over file {} ({})", text.empty() ? "missing" : "read", ParseName(result));
  }
  if (result != handover::ParseResult::Ok) {
    REXLOG_WARN("KK co-op hand over: no valid hand over; back to the menu");
    callbacks.back_to_menu();
    return;
  }
  REXLOG_INFO("KK co-op hand over: resumed {} for chapter {} ({}), {}", file.host ? "host" : "guest", file.plan.chapter,
              file.plan.continue_game ? "Continue" : "chapter select",
              file.plan.transport == StartTransport::Steam ? "Steam" : "Direct IP");
  new ResumeDialog(drawer, std::move(file), std::move(callbacks));  // deletes itself with the UI
}
}  // namespace kk::coop
