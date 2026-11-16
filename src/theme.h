#pragma once
#include "imgui.h"

// Modern dark theme: neutral greys separated by tone (no hard borders), one
// blue accent, rounded controls and generous spacing.
namespace theme {
inline const ImVec4 Accent      = ImVec4(0.231f, 0.510f, 0.965f, 1.00f);   // #3B82F6
inline const ImVec4 AccentHover = ImVec4(0.357f, 0.608f, 1.000f, 1.00f);
inline const ImVec4 AccentDim   = ImVec4(0.173f, 0.392f, 0.769f, 1.00f);
inline const ImVec4 Bg          = ImVec4(0.086f, 0.086f, 0.102f, 1.00f);   // app background
inline const ImVec4 Panel       = ImVec4(0.118f, 0.118f, 0.137f, 1.00f);   // side panels, bars
inline const ImVec4 Raised      = ImVec4(0.149f, 0.149f, 0.173f, 1.00f);   // popups, cards
inline const ImVec4 Control     = ImVec4(0.169f, 0.169f, 0.196f, 1.00f);
inline const ImVec4 ControlHot  = ImVec4(0.212f, 0.212f, 0.247f, 1.00f);
inline const ImVec4 ControlAct  = ImVec4(0.251f, 0.251f, 0.294f, 1.00f);
inline const ImVec4 Line        = ImVec4(0.200f, 0.200f, 0.235f, 1.00f);
inline const ImVec4 Text        = ImVec4(0.910f, 0.918f, 0.937f, 1.00f);
inline const ImVec4 TextDim     = ImVec4(0.545f, 0.561f, 0.612f, 1.00f);
inline const ImVec4 Good        = ImVec4(0.420f, 0.820f, 0.600f, 1.00f);
inline const ImVec4 Bad         = ImVec4(1.000f, 0.478f, 0.478f, 1.00f);
inline const ImU32 CanvasBg     = IM_COL32(38, 38, 44, 255);
inline const ImU32 AccentU32    = IM_COL32(59, 130, 246, 255);
}

inline void ApplyDarkTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 10.0f;
    s.ChildRounding     = 8.0f;
    s.FrameRounding     = 6.0f;
    s.PopupRounding     = 8.0f;
    s.GrabRounding      = 6.0f;
    s.TabRounding       = 6.0f;
    s.ScrollbarRounding = 9.0f;
    s.WindowPadding     = ImVec2(14, 12);
    s.FramePadding      = ImVec2(10, 6);
    s.ItemSpacing       = ImVec2(8, 8);
    s.ItemInnerSpacing  = ImVec2(6, 6);
    s.IndentSpacing     = 18.0f;
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 10.0f;
    s.WindowBorderSize  = 0.0f;
    s.ChildBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.SeparatorTextBorderSize = 1.0f;
    s.WindowMenuButtonPosition = ImGuiDir_None;

    using namespace theme;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = Text;
    c[ImGuiCol_TextDisabled]          = TextDim;
    c[ImGuiCol_WindowBg]              = Bg;
    c[ImGuiCol_ChildBg]               = Panel;
    c[ImGuiCol_PopupBg]               = Raised;
    c[ImGuiCol_Border]                = Line;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]               = Control;
    c[ImGuiCol_FrameBgHovered]        = ControlHot;
    c[ImGuiCol_FrameBgActive]         = ControlAct;
    c[ImGuiCol_TitleBg]               = Raised;
    c[ImGuiCol_TitleBgActive]         = Raised;
    c[ImGuiCol_TitleBgCollapsed]      = Raised;
    c[ImGuiCol_MenuBarBg]             = Panel;
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]         = ControlAct;
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.32f, 0.32f, 0.37f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = AccentDim;
    c[ImGuiCol_CheckMark]             = AccentHover;
    c[ImGuiCol_SliderGrab]            = Accent;
    c[ImGuiCol_SliderGrabActive]      = AccentHover;
    c[ImGuiCol_Button]                = Control;
    c[ImGuiCol_ButtonHovered]         = ControlHot;
    c[ImGuiCol_ButtonActive]          = ControlAct;
    c[ImGuiCol_Header]                = Control;
    c[ImGuiCol_HeaderHovered]         = ControlHot;
    c[ImGuiCol_HeaderActive]          = ControlAct;
    c[ImGuiCol_Separator]             = Line;
    c[ImGuiCol_SeparatorHovered]      = AccentDim;
    c[ImGuiCol_SeparatorActive]       = Accent;
    c[ImGuiCol_ResizeGrip]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]     = AccentDim;
    c[ImGuiCol_ResizeGripActive]      = Accent;
    c[ImGuiCol_Tab]                   = Panel;
    c[ImGuiCol_TabHovered]            = ControlHot;
    c[ImGuiCol_TabSelected]           = Control;
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.231f, 0.510f, 0.965f, 0.35f);
    c[ImGuiCol_NavHighlight]          = Accent;
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.50f);
    c[ImGuiCol_DragDropTarget]        = Accent;
}

// A filled accent button (the main action of a dialog / the start screen).
inline bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme::Accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::AccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::AccentDim);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

// A borderless button that only shows a background on hover (toolbars).
inline bool FlatButton(const char* label, bool active = false, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, active ? theme::Accent : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? theme::AccentHover : theme::ControlHot);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active ? theme::AccentDim : theme::ControlAct);
    if (active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(active ? 4 : 3);
    return r;
}
