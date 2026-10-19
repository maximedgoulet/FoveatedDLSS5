/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Design tokens and the scoped theme (PLAN_UI_V7.md 1).
//
// Everything here is pushed inside this addon's overlay callback and popped
// before it returns, so ReShade's own tabs and other add-ons never see it.
// Two constraints decided the shape (U0):
//   - ReShade calls Begin() for our window before our callback runs
//     (runtime_gui.cpp), so the window background is already drawn; the dark
//     background is a full-size child region, not a style push;
//   - an add-on cannot load a font (the add-on ImGui table has no ImFontAtlas
//     entry points and ReShade rebuilds its own atlas), so text sizes are
//     ReShade's font through PushFont(nullptr, size), and icons are the
//     ForkAwesome glyphs ReShade already merges into it.
// The capture lane (dlss5_e2e_ui_capture) renders the panel through a real
// ReShade overlay, reads the pixels back, and logs style_depth, which must be
// zero at the end of every callback.
//
// Include after <imgui.h> and reshade.hpp, like the rest of the overlay.

#pragma once

#include <cstdint>

namespace renodx::addons::dlss5::ui {

namespace tokens {

constexpr ImVec4 Hex(std::uint32_t rgb, float alpha = 1.f) {
  return ImVec4(static_cast<float>((rgb >> 16) & 0xff) / 255.f,
                static_cast<float>((rgb >> 8) & 0xff) / 255.f,
                static_cast<float>(rgb & 0xff) / 255.f, alpha);
}

inline constexpr ImVec4 kBgWindow = Hex(0x14161A, 0.94f);
inline constexpr ImVec4 kBgCard = Hex(0x1D2026);
inline constexpr ImVec4 kBgControl = Hex(0x262A31);
inline constexpr ImVec4 kBgControlHover = Hex(0x2F343C);
inline constexpr ImVec4 kStroke = Hex(0x2A2E36);
inline constexpr ImVec4 kTextPrimary = Hex(0xE8EAED);
inline constexpr ImVec4 kTextSecondary = Hex(0x9AA0A8);
inline constexpr ImVec4 kGood = Hex(0x76B900);
inline constexpr ImVec4 kWarn = Hex(0xF5A623);
inline constexpr ImVec4 kBad = Hex(0xEB5757);
inline constexpr ImVec4 kIdle = Hex(0x6E7681);
inline constexpr ImVec4 kAccent = Hex(0x4CA6A0);
inline constexpr ImVec4 kAccentHover = Hex(0x5BB8B2);
inline constexpr ImVec4 kAccentActive = Hex(0x3E8C87);

inline constexpr float kRadiusCard = 8.f;
inline constexpr float kRadiusControl = 6.f;
inline constexpr float kRadiusSmall = 4.f;
inline constexpr float kRowGap = 8.f;
inline constexpr float kSectionGap = 16.f;
inline constexpr float kCardPadding = 12.f;
// Row geometry, in units of the font size so it follows the player's
// ReShade font.  alpha45 gave labels 42 % of the panel, which at 4K put
// every control in the middle of the screen; the label column is now a fixed
// width, and never more than kLabelShare of a narrow panel.  The panel is
// one column at most kColumnEm wide, so a wide window does not stretch a
// slider across the screen, and a slider's value has its own column
// (kValueEm) instead of sitting under the knob.  A panel narrower than
// kStackEm has no room for a label column beside a usable control - ReShade
// docks it at ~450 px and doubles its font at 2160p - so each label goes on
// its own line over a control as wide as the panel.
inline constexpr float kLabelEm = 12.f;
inline constexpr float kLabelShare = 0.42f;
inline constexpr float kColumnEm = 40.f;
inline constexpr float kValueEm = 3.5f;
inline constexpr float kStackEm = 24.f;
inline constexpr float kTooltipEm = 35.f;

}  // namespace tokens

// ForkAwesome glyphs, as ReShade merges them (external/reshade/res/fonts/
// forkawesome.h: the subset ReShade ships, which is all the atlas holds).
namespace icons {
inline constexpr const char* kCheck = "\xef\x80\x8c";          // U+F00C ok
inline constexpr const char* kWarning = "\xef\x81\xb1";        // U+F071 warning
inline constexpr const char* kCross = "\xef\x80\x8d";          // U+F00D cancel
inline constexpr const char* kCircle = "\xef\x84\x8c";         // U+F10C circle_empty
inline constexpr const char* kHourglass = "\xef\x89\x92";      // U+F252 hourglass_half
inline constexpr const char* kClipboard = "\xef\x83\xaa";      // U+F0EA clipboard
inline constexpr const char* kFolderOpen = "\xef\x84\x95";     // U+F115 folder_open
inline constexpr const char* kInfo = "\xef\x81\x9a";           // U+F05A info_circle
inline constexpr const char* kWrench = "\xef\x82\xad";         // U+F0AD wrench
inline constexpr const char* kPower = "\xef\x80\x91";          // U+F011 off
inline constexpr const char* kUndo = "\xef\x83\xa2";           // U+F0E2 undo
}  // namespace icons

// Text sizes, relative to the size the player chose for ReShade (its
// FontSize setting, 13 by default): 15 / 13 / 12 at the default, scaled with
// it otherwise, so a player who enlarged ReShade's font keeps that here.
inline float BaseFontSize() {
  const float base = ImGui::GetStyle().FontSizeBase;
  return base > 0.f ? base : 13.f;
}
inline float TitleFontSize() { return BaseFontSize() * (15.f / 13.f); }
inline float BodyFontSize() { return BaseFontSize(); }
inline float SmallFontSize() { return BaseFontSize() * (12.f / 13.f); }

inline ImU32 ToU32(const ImVec4& color) { return ImGui::GetColorU32(color); }

// Pushes and pops in one object, so a return from the middle of a panel
// cannot leave a colour or a variable on ReShade's stacks.  The depth
// counter is the U0 witness: the capture lane logs it at the end of every
// callback, and anything but zero is a leak.
inline int style_depth = 0;

class StyleScope {
 public:
  StyleScope() = default;
  StyleScope(const StyleScope&) = delete;
  StyleScope& operator=(const StyleScope&) = delete;
  ~StyleScope() {
    if (vars_ != 0) ImGui::PopStyleVar(vars_);
    if (colors_ != 0) ImGui::PopStyleColor(colors_);
    if (fonts_ != 0) {
      for (int i = 0; i < fonts_; ++i) ImGui::PopFont();
    }
    style_depth -= vars_ + colors_ + fonts_;
  }
  StyleScope& Color(ImGuiCol index, const ImVec4& color) {
    ImGui::PushStyleColor(index, color);
    ++colors_;
    ++style_depth;
    return *this;
  }
  StyleScope& Var(ImGuiStyleVar index, float value) {
    ImGui::PushStyleVar(index, value);
    ++vars_;
    ++style_depth;
    return *this;
  }
  StyleScope& Var(ImGuiStyleVar index, const ImVec2& value) {
    ImGui::PushStyleVar(index, value);
    ++vars_;
    ++style_depth;
    return *this;
  }
  StyleScope& FontSize(float size) {
    ImGui::PushFont(nullptr, size);
    ++fonts_;
    ++style_depth;
    return *this;
  }

 private:
  int colors_ = 0;
  int vars_ = 0;
  int fonts_ = 0;
};

// The panel theme: every colour and shape token ImGui draws our widgets
// with.  Pushed once at the top of the callback.
inline void PushPanelTheme(StyleScope& scope) {
  using namespace tokens;
  scope.Color(ImGuiCol_Text, kTextPrimary)
      .Color(ImGuiCol_TextDisabled, kTextSecondary)
      .Color(ImGuiCol_ChildBg, kBgWindow)
      .Color(ImGuiCol_Border, kStroke)
      .Color(ImGuiCol_Separator, kStroke)
      .Color(ImGuiCol_FrameBg, kBgControl)
      .Color(ImGuiCol_FrameBgHovered, kBgControlHover)
      .Color(ImGuiCol_FrameBgActive, kBgControlHover)
      .Color(ImGuiCol_Button, kBgControl)
      .Color(ImGuiCol_ButtonHovered, kBgControlHover)
      .Color(ImGuiCol_ButtonActive, kAccentActive)
      .Color(ImGuiCol_Header, kBgCard)
      .Color(ImGuiCol_HeaderHovered, kBgControlHover)
      .Color(ImGuiCol_HeaderActive, kBgControlHover)
      .Color(ImGuiCol_SliderGrab, kAccent)
      .Color(ImGuiCol_SliderGrabActive, kAccentHover)
      .Color(ImGuiCol_CheckMark, kAccent)
      .Color(ImGuiCol_PopupBg, kBgCard)
      .Color(ImGuiCol_TextLink, kAccent)
      .Var(ImGuiStyleVar_FrameRounding, kRadiusControl)
      .Var(ImGuiStyleVar_GrabRounding, kRadiusSmall)
      .Var(ImGuiStyleVar_ChildRounding, kRadiusCard)
      .Var(ImGuiStyleVar_PopupRounding, kRadiusControl)
      .Var(ImGuiStyleVar_ItemSpacing, ImVec2(kRowGap, kRowGap))
      .Var(ImGuiStyleVar_FramePadding, ImVec2(8.f, 5.f));
}

}  // namespace renodx::addons::dlss5::ui
