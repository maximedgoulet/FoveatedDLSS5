/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The status card, drawn (PLAN_UI_V7.md 1 and 3).
//
// state_card.hpp decides the card; this draws it: a rounded card with a
// stripe in the card's tone down its left edge, the glyph and title at the
// title size, the body, the counts line, the path note, the numbered fix
// steps and the card's button.  Nothing here decides anything, so a card the
// unit tests have pinned is the card the player sees.  Its text is drawn in
// the panel's language (i18n.hpp); the card itself stays English.
//
// Include after <imgui.h> and reshade.hpp, like theme.hpp.

#pragma once

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "state_card.hpp"
#include "theme.hpp"

namespace renodx::addons::dlss5::ui {

// i18n: begin - display strings (tools/i18n/ui_strings.py catalog)

inline bool force_next_tooltip = false;  // UI capture only; never set by normal sessions
inline void ItemTooltip(const char* fmt, ...) {
  if (force_next_tooltip) {
    force_next_tooltip = false;
    ImGui::BeginTooltip();
  } else if (!ImGui::BeginItemTooltip()) {
    return;
  }
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * tokens::kTooltipEm);
  va_list args;
  va_start(args, fmt);
  ImGui::TextV(fmt, args);
  va_end(args);
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

inline ImVec4 ToneColor(CardTone tone) {
  switch (tone) {
    case CardTone::kGood: return tokens::kGood;
    case CardTone::kWarn: return tokens::kWarn;
    case CardTone::kBad: return tokens::kBad;
    case CardTone::kIdle: return tokens::kIdle;
  }
  return tokens::kIdle;
}

// The tone as 8-bit RGB, for the capture lane's pixel count.
inline std::uint32_t ToneRgb(CardTone tone) {
  const ImVec4 color = ToneColor(tone);
  const auto channel = [](float value) {
    return static_cast<std::uint32_t>(value * 255.f + 0.5f);
  };
  return (channel(color.x) << 16) | (channel(color.y) << 8) | channel(color.z);
}

inline const char* GlyphIcon(CardGlyph glyph) {
  switch (glyph) {
    case CardGlyph::kCheck: return icons::kCheck;
    case CardGlyph::kWarning: return icons::kWarning;
    case CardGlyph::kCross: return icons::kCross;
    case CardGlyph::kIdle: return icons::kCircle;
    case CardGlyph::kWaiting: return icons::kHourglass;
    case CardGlyph::kInfo: return icons::kInfo;
  }
  return icons::kCircle;
}

inline const char* ActionLabel(CardAction action) {
  switch (action) {
    case CardAction::kTurnOn: return Tr("Turn on");
    case CardAction::kOpenDiagnostics: return Tr("Open Diagnostics");
    case CardAction::kDismiss: return Tr("Got it");
    case CardAction::kNone: break;
  }
  return nullptr;
}

inline constexpr float kStripeWidth = 4.f;

// The glyph in the card's tone and the title, at the title size: the card's
// first line, and all of the HUD pill but its hint.
inline void CardHeading(const StatusCard& card) {
  {
    StyleScope title;
    title.FontSize(TitleFontSize()).Color(ImGuiCol_Text, ToneColor(card.tone));
    ImGui::TextUnformatted(GlyphIcon(card.glyph));
  }
  ImGui::SameLine();
  StyleScope title;
  title.FontSize(TitleFontSize());
  ImGui::TextUnformatted(Tr(card.title));
}

// Draws the card; returns true on the frame its button was pressed.
inline bool DrawStatusCard(const StatusCard& card) {
  using namespace tokens;
  bool pressed = false;

  StyleScope frame;
  frame.Color(ImGuiCol_ChildBg, kBgCard)
      .Var(ImGuiStyleVar_WindowPadding,
           ImVec2(kCardPadding + kStripeWidth, kCardPadding));
  const bool visible = ImGui::BeginChild(
      "##nr_status_card", ImVec2(0.f, 0.f),
      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  if (visible) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    draw->AddRectFilled(origin,
                        ImVec2(origin.x + kStripeWidth, origin.y + size.y),
                        ToU32(ToneColor(card.tone)), kRadiusCard,
                        ImDrawFlags_RoundCornersLeft);

    // Before the title: a long title in a narrow docked panel wraps inside
    // the card instead of running past its edge.
    ImGui::PushTextWrapPos(0.f);
    CardHeading(card);
    ImGui::TextUnformatted(Tr(card.body));
    char detail[160];
    const bool has_detail = FormatCardDetail(detail, sizeof(detail), card) > 0;
    if (has_detail || card.path_note != nullptr) {
      StyleScope secondary;
      secondary.FontSize(SmallFontSize()).Color(ImGuiCol_Text, kTextSecondary);
      if (has_detail) ImGui::TextUnformatted(detail);
      if (card.path_note != nullptr) ImGui::TextUnformatted(Tr(card.path_note));
    }
    for (std::size_t step = 0; step < card.fix_count; ++step) {
      if (card.fix[step] == nullptr) continue;
      ImGui::Text("%u. %s", static_cast<unsigned>(step + 1), Tr(card.fix[step]));
    }
    ImGui::PopTextWrapPos();

    if (const char* label = ActionLabel(card.action); label != nullptr) {
      StyleScope button;
      button.Color(ImGuiCol_Button, kAccent)
          .Color(ImGuiCol_ButtonHovered, kAccentHover)
          .Color(ImGuiCol_ButtonActive, kAccentActive)
          .Color(ImGuiCol_Text, kBgWindow);
      pressed = ImGui::Button(label);
    }
  }
  ImGui::EndChild();
  return pressed;
}

// ---- the modern layout's controls (U2; rows reworked in alpha46) -----------
//
// Positions here are screen positions: the panel is a one-column table (the
// width cap), and SameLine's window-relative offsets are shifted by a table
// cell's own offset.

// The reset slot of the row RowLabel started, as a screen x.
inline float row_slot_x = 0.f;

// Test-only (RENODX_NR_TEST_UI_EXPAND): every section header opens, so a
// capture lane draws the collapsed sections' controls too.
inline bool expand_all_sections = false;

// `text` cut to fit `width` with "..." at the end, or whole when it fits.
// The cut falls between UTF-8 characters: a translated label is Cyrillic,
// Thai or CJK, and half a character draws as a stray glyph.
inline std::string Ellipsize(const char* text, float width) {
  std::string shown(text);
  if (ImGui::CalcTextSize(text).x <= width) return shown;
  while (!shown.empty() && ImGui::CalcTextSize((shown + "...").c_str()).x > width) {
    while (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0x80) {
      shown.pop_back();
    }
    if (!shown.empty()) shown.pop_back();
  }
  return shown + "...";
}

// This frame's count of drawn reset buttons: the capture line states it
// (resets=), so a lane can tell a changed setting's row from a stock one.
inline int reset_buttons_drawn = 0;

// A settings row: the label in the secondary colour in a fixed-width left
// column, then the control, then a reset slot one frame high at the end.
// The slot is reserved whether or not the row shows its reset button, so
// every control on the panel starts and ends at the same x.  A label longer
// than its column ends in "..." and shows in full on hover, and a row's
// `tooltip` shows over its label as well as over its control.  On a panel
// narrower than kStackEm the label takes its own line and the control the
// panel's width under it, a little apart from the row above.  Call right
// before a control whose own label is hidden ("##id"); the next item's width
// is the control column.  Both texts are English; the row draws them in the
// panel's language.
inline void RowLabel(const char* label, const char* tooltip = nullptr) {
  label = Tr(label);
  tooltip = Tr(tooltip);
  const ImGuiStyle& style = ImGui::GetStyle();
  const float avail = ImGui::GetContentRegionAvail().x;
  const float font = ImGui::GetFontSize();
  const bool stacked = avail < font * tokens::kStackEm;
  const float column =
      stacked ? 0.f : (std::min)(font * tokens::kLabelEm, avail * tokens::kLabelShare);
  if (stacked) ImGui::Dummy(ImVec2(0.f, 0.f));
  const ImVec2 at = ImGui::GetCursorScreenPos();
  row_slot_x = at.x + avail - ImGui::GetFrameHeight();
  const float fit = stacked ? avail : column - style.ItemSpacing.x;
  const bool clipped = ImGui::CalcTextSize(label).x > fit;
  if (!stacked) ImGui::AlignTextToFramePadding();
  // A stacked row without a label ("Custom" under Resolution) draws no
  // empty line; a side-by-side one keeps its label cell for SameLine.
  if (!stacked || *label != '\0') {
    {
      StyleScope secondary;
      secondary.Color(ImGuiCol_Text, tokens::kTextSecondary);
      ImGui::TextUnformatted(Ellipsize(label, fit).c_str());
    }
    if (clipped && tooltip != nullptr) {
      ItemTooltip("%s\n%s", label, tooltip);
    } else if (clipped || tooltip != nullptr) {
      ItemTooltip("%s", clipped ? label : tooltip);
    }
  }
  if (!stacked) {
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(at.x + column, ImGui::GetCursorScreenPos().y));
  }
  ImGui::SetNextItemWidth(row_slot_x - style.ItemSpacing.x - (at.x + column));
}

// SameLine when an item `width` wide still fits after the last one, so a
// pair of buttons wraps onto two lines on a narrow panel instead of running
// off its edge.
inline void SameLineIfFits(float width) {
  const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
  if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right) {
    ImGui::SameLine();
  }
}

// The reset button in the row's slot: an undo arrow, drawn only for a
// setting that differs from its default (the caller decides).  `id` keeps
// the buttons apart; `default_text` names what a click restores.  Returns
// true when clicked.
inline bool ResetButton(const char* id, const char* default_text) {
  ++reset_buttons_drawn;
  ImGui::SameLine();
  ImGui::SetCursorScreenPos(ImVec2(row_slot_x, ImGui::GetCursorScreenPos().y));
  bool clicked = false;
  {
    StyleScope flat;
    flat.Color(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f))
        .Color(ImGuiCol_Text, tokens::kTextSecondary)
        .Var(ImGuiStyleVar_FramePadding, ImVec2(0.f, ImGui::GetStyle().FramePadding.y));
    ImGui::PushID(id);
    clicked = ImGui::Button(icons::kUndo, ImVec2(ImGui::GetFrameHeight(), 0.f));
    ImGui::PopID();
  }
  ItemTooltip(Tr("Reset to default (%s)"), Tr(default_text));
  return clicked;
}

// A slider drawn the modern way: a thin rail filled up to the value, a round
// knob, and the value in its own column on the right, so the number never
// sits under the knob (alpha45 drew Strength's 1.00 under the grab).  ImGui's
// slider still runs underneath, invisible, so dragging, keyboard and gamepad
// steps and Ctrl+click typing behave as before; while it is being typed
// into, it is drawn as ImGui draws it.  Call after RowLabel.  Returns true
// on the frames *value changed.
inline bool Slider(const char* id, float* value, float min, float max,
                   const char* format, const char* tooltip) {
  static ImGuiID typing = 0;
  format = Tr(format);
  tooltip = Tr(tooltip);
  const ImGuiID item = ImGui::GetID(id);
  const bool was_typing = typing == item;
  const float height = ImGui::GetFrameHeight();
  const float knob = height * 0.32f;
  char text[32];
  std::snprintf(text, sizeof(text), format, *value);
  const float text_width = ImGui::CalcTextSize(text).x;
  const float value_width = (std::max)(ImGui::GetFontSize() * tokens::kValueEm,
                                        text_width + ImGui::GetStyle().ItemSpacing.x);
  const float width = ImGui::CalcItemWidth();
  bool changed = false;
  {
    StyleScope hidden;
    // ImGui centres a grab of GrabMinSize on the value: the knob's diameter,
    // so the knob drawn below sits where the mouse drags it.
    hidden.Var(ImGuiStyleVar_GrabMinSize, knob * 2.f);
    if (!was_typing) {
      const ImVec4 clear(0.f, 0.f, 0.f, 0.f);
      hidden.Color(ImGuiCol_FrameBg, clear)
          .Color(ImGuiCol_FrameBgHovered, clear)
          .Color(ImGuiCol_FrameBgActive, clear)
          .Color(ImGuiCol_SliderGrab, clear)
          .Color(ImGuiCol_SliderGrabActive, clear)
          .Color(ImGuiCol_Text, clear);
    }
    ImGui::SetNextItemWidth(was_typing ? width : (std::max)(width - value_width, height * 2.f));
    changed = ImGui::SliderFloat(id, value, min, max, format);
  }
  if (tooltip != nullptr) ItemTooltip("%s", tooltip);
  const bool active = ImGui::IsItemActive();
  // A Ctrl+click turns ImGui's slider into a text field; the text field
  // raises WantTextInput from the next frame on.
  if (active && ImGui::GetIO().WantTextInput) {
    typing = item;
  } else if (was_typing && !active) {
    typing = 0;
  }
  if (was_typing) return changed;

  const ImVec2 lo = ImGui::GetItemRectMin();
  const ImVec2 hi = ImGui::GetItemRectMax();
  // ImGui's own grab geometry: 2 px of padding, then half the grab.
  const float x0 = lo.x + 2.f + knob;
  const float x1 = hi.x - 2.f - knob;
  const float t = max > min ? std::clamp((*value - min) / (max - min), 0.f, 1.f) : 0.f;
  const float x = x0 + t * (x1 - x0);
  const float y = (lo.y + hi.y) * 0.5f;
  const float rail = (std::max)(2.f, height * 0.14f);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(ImVec2(x0, y - rail * 0.5f), ImVec2(x1, y + rail * 0.5f),
                      ToU32(tokens::kBgControlHover), rail);
  draw->AddRectFilled(ImVec2(x0, y - rail * 0.5f), ImVec2(x, y + rail * 0.5f),
                      ToU32(tokens::kAccent), rail);
  if (active || ImGui::IsItemHovered()) {
    draw->AddCircleFilled(ImVec2(x, y), knob + 3.f, ToU32(ImVec4(tokens::kAccent.x,
                          tokens::kAccent.y, tokens::kAccent.z, 0.35f)));
  }
  draw->AddCircleFilled(ImVec2(x, y), knob, ToU32(tokens::kTextPrimary));

  ImGui::SameLine(0.f, 0.f);
  ImGui::SetCursorScreenPos(
      ImVec2(hi.x + value_width - text_width, ImGui::GetCursorScreenPos().y));
  ImGui::TextUnformatted(text);
  return changed;
}

// A group's name inside a section ("Pass 2", "HDR"): small text in the
// secondary colour.
inline void SubCaption(const char* caption) {
  StyleScope caption_text;
  caption_text.FontSize(SmallFontSize()).Color(ImGuiCol_Text, tokens::kTextSecondary);
  ImGui::TextUnformatted(Tr(caption));
}

// A link at the right end of the current line (screen x `right`).  Returns
// true when clicked.
inline bool RightLink(const char* label, float right) {
  ImGui::SameLine();
  ImGui::SetCursorScreenPos(
      ImVec2(right - ImGui::CalcTextSize(label).x, ImGui::GetCursorScreenPos().y));
  return ImGui::TextLink(label);
}

// An always-open section's caption: small capitals in the secondary colour
// over a hairline, and at the right end a link that resets the section,
// shown while `modified`.  Returns true on the frame the link was clicked.
inline bool SectionCaption(const char* caption, const char* reset_label, bool modified) {
  reset_label = Tr(reset_label);
  ImGui::Dummy(ImVec2(0.f, tokens::kRowGap * 0.5f));
  const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
  bool clicked = false;
  {
    StyleScope caption_text;
    caption_text.FontSize(SmallFontSize());
    {
      StyleScope secondary;
      secondary.Color(ImGuiCol_Text, tokens::kTextSecondary);
      ImGui::TextUnformatted(Tr(caption));
    }
    if (modified) clicked = RightLink(reset_label, right);
  }
  ImGui::Separator();
  return clicked;
}

// The gap between two sections: kSectionGap in all, counting the item
// spacing ImGui adds before and after the spacer.
inline void SectionGap() {
  ImGui::Dummy(ImVec2(
      0.f, (std::max)(0.f, tokens::kSectionGap - 2.f * ImGui::GetStyle().ItemSpacing.y)));
}

// A collapsible section: ImGui's header row, a little taller, a hint in the
// secondary colour after the title, and the same reset link at the right end
// while the section holds a changed setting.  A header too narrow for the
// link's words after the title shows the undo arrow instead, with the words
// on hover; the hint shows only while enough of it fits to say something.
// Returns whether the section is open; *reset is true on the frame the link
// was clicked.  The header's id is its English title, so a section keeps
// its open state across a language change.
inline bool SectionHeader(const char* english, const char* hint, const char* reset_label,
                          bool modified, bool* reset) {
  const char* title = Tr(english);
  hint = Tr(hint);
  reset_label = Tr(reset_label);
  // The header's vertical padding, which is also where the hint's baseline
  // sits: the two must match or the hint drifts off the title line.
  constexpr float kPadY = 7.f;
  const float pad = tokens::kCardPadding * 0.75f;
  const float font = ImGui::GetFontSize();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float right = at.x + ImGui::GetContentRegionAvail().x - pad;
  // ImGui draws a framed header's title after the arrow: one font size plus
  // three paddings in.
  const float title_end = at.x + font + pad * 3.f + ImGui::CalcTextSize(title).x;
  bool open = false;
  if (expand_all_sections) ImGui::SetNextItemOpen(true);
  {
    StyleScope tall;
    tall.Var(ImGuiStyleVar_FramePadding, ImVec2(pad, kPadY));
    open = ImGui::CollapsingHeader((std::string(title) + "###" + english).c_str(),
                                   ImGuiTreeNodeFlags_AllowOverlap);
  }
  const ImVec2 lo = ImGui::GetItemRectMin();
  float hint_end = right;
  *reset = false;
  if (modified && reset_label != nullptr) {
    const bool words = title_end + font + ImGui::CalcTextSize(reset_label).x <= right;
    const char* shown = words ? reset_label : icons::kUndo;
    hint_end -= ImGui::CalcTextSize(shown).x;
    ImGui::PushID(reset_label);
    *reset = RightLink(shown, right);
    ImGui::PopID();
    if (!words) ItemTooltip("%s", reset_label);
  }
  if (hint != nullptr) {
    const ImVec2 hint_at(title_end + font, lo.y + kPadY);
    const float room = hint_end - font - hint_at.x;
    if (room >= (std::min)(ImGui::CalcTextSize(hint).x, font * 6.f)) {
      ImGui::GetWindowDrawList()->AddText(hint_at, ToU32(tokens::kTextSecondary),
                                          Ellipsize(hint, room).c_str());
    }
  }
  return open;
}

// An on/off switch: a rounded track in the accent colour when on, the knob
// at the end it points to.  Returns true on the frame it flipped *value.
inline bool Switch(const char* id, bool* value) {
  const float height = ImGui::GetFrameHeight();
  const float width = height * 1.9f;
  const float radius = height * 0.5f;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
  if (pressed) *value = !*value;
  const bool hovered = ImGui::IsItemHovered();
  const ImVec4& track = *value ? (hovered ? tokens::kAccentHover : tokens::kAccent)
                               : (hovered ? tokens::kBgControlHover : tokens::kBgControl);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), ToU32(track), radius);
  draw->AddCircleFilled(
      ImVec2(*value ? at.x + width - radius : at.x + radius, at.y + radius),
      radius - 3.f, ToU32(tokens::kTextPrimary));
  return pressed;
}

// Equal-width buttons in one row, the selected one in the accent colour.
// `current` is -1 when no segment matches the setting; `tooltip`, when not
// null, shows over every segment.  The row is the next item's width, which
// RowLabel sets.  Returns the index clicked this frame, or -1.  A row too
// narrow for its longest label (ReShade docks the panel at ~450 px) draws a
// dropdown of the same choices instead of clipped words.
inline int Segmented(const char* id, const char* const* english, int count,
                     int current, const char* tooltip) {
  constexpr float kGap = 2.f;
  constexpr int kMaxSegments = 8;
  const char* labels[kMaxSegments] = {};
  count = (std::min)(count, kMaxSegments);
  for (int i = 0; i < count; ++i) labels[i] = Tr(english[i]);
  tooltip = Tr(tooltip);
  const float width =
      (ImGui::CalcItemWidth() - kGap * static_cast<float>(count - 1))
      / static_cast<float>(count);
  float widest = 0.f;
  for (int i = 0; i < count; ++i) {
    widest = (std::max)(widest, ImGui::CalcTextSize(labels[i]).x);
  }
  int clicked = -1;
  if (widest + ImGui::GetStyle().FramePadding.x * 2.f > width) {
    if (ImGui::BeginCombo(id, current >= 0 && current < count ? labels[current] : Tr("Custom"))) {
      for (int i = 0; i < count; ++i) {
        if (ImGui::Selectable(labels[i], i == current)) clicked = i;
      }
      ImGui::EndCombo();
    }
    if (tooltip != nullptr) ItemTooltip("%s", tooltip);
    return clicked;
  }
  ImGui::PushID(id);
  StyleScope row;
  row.Var(ImGuiStyleVar_ItemSpacing, ImVec2(kGap, ImGui::GetStyle().ItemSpacing.y));
  for (int i = 0; i < count; ++i) {
    if (i != 0) ImGui::SameLine();
    {
      StyleScope segment;
      if (i == current) {
        segment.Color(ImGuiCol_Button, tokens::kAccent)
            .Color(ImGuiCol_ButtonHovered, tokens::kAccentHover)
            .Color(ImGuiCol_Text, tokens::kBgWindow);
      }
      if (ImGui::Button(labels[i], ImVec2(width, 0.f))) clicked = i;
    }
    // After the segment's colours pop: a tooltip draws its text in the
    // current ImGuiCol_Text, and the selected segment's is the window
    // background, which on the popup's dark fill left the text unreadable.
    if (tooltip != nullptr) ItemTooltip("%s", tooltip);
  }
  ImGui::PopID();
  return clicked;
}

// i18n: end

}  // namespace renodx::addons::dlss5::ui
