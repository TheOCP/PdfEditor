#pragma once
#include "imgui.h"

// Compact dark theme: near-black panels, soft rounded corners, a blue accent.
inline void ApplyDarkTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 6.0f;
    s.FrameRounding     = 5.0f;
    s.GrabRounding      = 5.0f;
    s.TabRounding       = 5.0f;
    s.ChildRounding     = 6.0f;
    s.PopupRounding     = 5.0f;
    s.ScrollbarRounding = 8.0f;
    s.FramePadding      = ImVec2(9, 5);
    s.ItemSpacing       = ImVec2(8, 7);
    s.WindowPadding     = ImVec2(12, 10);
    s.ScrollbarSize     = 13.0f;
    s.WindowBorderSize  = 0.0f;
    s.FrameBorderSize   = 0.0f;

    ImVec4* c = s.Colors;
    const ImVec4 accent    = ImVec4(0.24f, 0.56f, 0.95f, 1.00f);
    const ImVec4 accentDim = ImVec4(0.20f, 0.44f, 0.74f, 1.00f);
    const ImVec4 accentHi  = ImVec4(0.34f, 0.66f, 1.00f, 1.00f);

    c[ImGuiCol_Text]                 = ImVec4(0.90f, 0.91f, 0.93f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.48f, 0.50f, 0.54f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_Border]               = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.23f, 0.23f, 0.28f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.32f, 0.32f, 0.37f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = accentDim;
    c[ImGuiCol_CheckMark]            = accentHi;
    c[ImGuiCol_SliderGrab]           = accentDim;
    c[ImGuiCol_SliderGrabActive]     = accentHi;
    c[ImGuiCol_Button]               = ImVec4(0.17f, 0.17f, 0.21f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = accentDim;
    c[ImGuiCol_ButtonActive]         = accent;
    c[ImGuiCol_Header]               = ImVec4(0.17f, 0.17f, 0.21f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = accentDim;
    c[ImGuiCol_HeaderActive]         = accent;
    c[ImGuiCol_Separator]            = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
    c[ImGuiCol_SeparatorHovered]     = accentDim;
    c[ImGuiCol_SeparatorActive]      = accent;
    c[ImGuiCol_Tab]                  = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    c[ImGuiCol_TabHovered]           = accentDim;
    c[ImGuiCol_TabSelected]          = ImVec4(0.17f, 0.17f, 0.21f, 1.00f);
}
