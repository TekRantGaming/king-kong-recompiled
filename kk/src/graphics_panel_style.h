#pragma once
#include "launcher_ui.h"
namespace kk::ui {
inline void PushGraphicsPanelStyle(float alpha) {
    ImGui::SetNextWindowBgAlpha(alpha);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::color::kBase);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ui::color::kText);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ui::color::kTextFaint);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ui::color::kAccent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ui::color::kAccent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ui::color::kFocus);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ui::color::kBase);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ui::color::kBase);
    ImGui::PushStyleColor(ImGuiCol_Border, ui::color::kHairline);
    ImGui::PushStyleColor(ImGuiCol_Separator, ui::color::kHairline);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(32, 30, 26, 220));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(52, 45, 34, 230));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(70, 57, 37, 230));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(48, 42, 32, 230));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(72, 59, 38, 240));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(96, 75, 45, 240));
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(40, 36, 30, 220));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(64, 54, 38, 230));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(90, 72, 44, 230));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(14, 16, 20, 245));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 60));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(90, 78, 58, 220));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(130, 106, 70, 230));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ui::color::kAccent);
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, IM_COL32(90, 78, 58, 120));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, IM_COL32(130, 106, 70, 200));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, ui::color::kAccent);
  }
inline void PopGraphicsPanelStyle() { ImGui::PopStyleColor(28); }

}
