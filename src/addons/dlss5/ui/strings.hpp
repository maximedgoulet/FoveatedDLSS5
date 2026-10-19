/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The panel's display strings (PLAN_UI_V7.md 5, U2).
//
// Overlay text is free to change; LOG text is a contract and never lives
// here.  The two layouts share every section function, and a section reads
// the labels that differ between them from the table of the layout that
// called it: kClassicText is the alpha44 wording verbatim (Developer view
// promises today's layout), kModernText applies the rename table.  A label
// only one section draws is spelled at its control as
// text.Pick(classic, modern), so the two wordings sit side by side.  Strings
// only the modern layout draws follow the tables.  U5 moves the rest of the
// panel's text in here.
//
// Every string here is English and is translated where it is drawn
// (i18n.hpp): the modern layout's text goes through Tr, the Developer view's
// through PanelText::Tr, which leaves it English.  Both layouts draw the
// status card, the Streamline warning and the Developer view toggle
// translated: they are how a player finds their way in either layout.

#pragma once

#include "i18n.hpp"

namespace renodx::addons::dlss5::ui {

struct PanelText {
  // Which layout the table serves.  The modern one also draws label-left
  // rows with a reset button and a switch where the classic one draws
  // ImGui's label-right controls and a checkbox.
  bool modern;
  const char* nr_enable;
  const char* intensity;
  const char* pre_sr;
  const char* stack_passes;
  const char* codec;
  const char* diffuse_white;
  const char* governor_attack;
  const char* governor_release;
  const char* guide_overrides_note;  // nullptr: the section header says it

  constexpr const char* Pick(const char* classic, const char* modern_label) const {
    return modern ? modern_label : classic;
  }

  // `english` as this layout draws it: translated in the modern layout,
  // English in the Developer view.
  const char* Tr(const char* english) const {
    return modern ? ::renodx::addons::dlss5::ui::Tr(english) : english;
  }
};

inline constexpr PanelText kClassicText = {
    false,
    "Enable DLSS Neural Rendering",
    "Overall Intensity",
    "Apply NR before upscaling (pre-SR)",
    "NR Passes (stack)",
    "NR Codec",
    "Diffuse White (nits)",
    "Governor attack (stops/s)",
    "Governor release (stops/s)",
    "Guide overrides (leave at defaults unless diagnostics require them)",
};

// i18n: begin - display strings (tools/i18n/ui_strings.py catalog)
inline constexpr PanelText kModernText = {
    true,
    "Neural Rendering",
    "Strength",
    "Before upscaling",
    "Stack passes",
    "HDR mode",
    "Paper white (nits)",
    "Brighten speed",
    "Darken speed",
    nullptr,
};

namespace text {

inline constexpr const char* kNrEnableTip =
    "Turns Neural Rendering on and off.  The key shown next to the switch"
    " does the same in game.";
inline constexpr const char* kIntensityTip =
    "How strongly Neural Rendering reworks the image: 0 leaves the game's"
    " image untouched, 1 is the default, 2 the strongest.  A change restarts"
    " NR's history for a moment.";
// Shared by pass 1's HDR rows and every stacked pass's.
inline constexpr const char* kTransferTip =
    "Float-HDR games: how much of the model's dark lift is removed when Dark"
    " pedestal removal is on. Other HDR games: how much of Neural Rendering's"
    " change reaches the image, from 0 (none) to 1 (all).";
inline constexpr const char* kTransferInertTip =
    "Nothing to scale in this game: its HDR brightness is relative, so Dark"
    " pedestal removal (Auto) leaves the model's dark lift alone.  Set Dark"
    " pedestal removal to Always to use this slider.";
inline constexpr const char* kColorTip =
    "On float-HDR games, how much of Neural Rendering's change reaches the"
    " image: 0 keeps the game's image, 1.00 shows all of it.";
inline constexpr const char* kPaperWhiteTip =
    "How bright white surfaces glow, in nits (203 is the HDR reference).  PQ"
    " HDR games only; other HDR games use Scene white scale.";
inline constexpr const char* kSceneWhiteTip =
    "The same for float-HDR and classic sources (2.5375 is 203-nit paper"
    " white).  PQ HDR games use Paper white and PQ calibration instead.";
inline constexpr const char* kRebindTip =
    "Click, then press the new key; Esc cancels.";
inline constexpr const char* kNoneYet = "none yet";
inline constexpr const char* kResetDiagnostics = "Reset diagnostics";
inline constexpr const char* kSkippedBecause = "Skipped because";
inline constexpr const char* kSkippedBecauseTip =
    "Why Neural Rendering left frames alone this session, most frequent"
    " first, with the number of frames.  Each of those frames shows the"
    " game's own image.";
// i18n: end
// Both layouts' footer: version, runtime build, the game's file name.
inline constexpr const char* kFooter = "RenoDX DLSS5 Generic %s | DLSSNR %s | %s";
// i18n: begin - display strings (tools/i18n/ui_strings.py catalog)

// The Defaults view (alpha46): segment 0 runs the built-in look, segment 1
// the player's own.
inline constexpr const char* kSettingsView = "Settings";
inline constexpr const char* kSettingsViewLabels[] = {"Defaults", "Mine"};
inline constexpr const char* kSettingsViewTip =
    "Compare in game: Defaults runs the built-in look (Strength, the Look"
    " section and stacking) while your own values wait; Mine brings them"
    " back.  Nothing is saved while Defaults is on.";
inline constexpr const char* kDefaultsViewNote =
    "Showing the built-in look.  Your settings are kept; pick Mine to return.";

inline constexpr const char* kResolution = "Resolution";
inline constexpr const char* kLook = "LOOK";
inline constexpr const char* kResetLook = "Reset look";
// The LOOK's two groups (v7.1, PLAN_NR_LOOK_V71.md): the model's own
// controls, which restart its history, and the look stage's, which reshape
// what it returned, live.  "\xc2\xb7" is U+00B7, a middle dot.
inline constexpr const char* kLookModel = "Model \xc2\xb7 restarts NR history";
inline constexpr const char* kLookResult = "Result \xc2\xb7 live, reshapes NR's output";
inline constexpr const char* kLookLimits = "Limits and stability";
inline constexpr const char* kLookLimitsChanged = "Limits and stability \xc2\xb7 changed";
inline constexpr const char* kPerformance = "Performance";
inline constexpr const char* kPerformanceHint = "pre-SR, stacking, resolution, upsampling";
inline constexpr const char* kResetPerformance = "Reset performance";
inline constexpr const char* kHotkeys = "Hotkeys & screenshots";
// The Hotkeys header's hint: the toggle key, then the screenshot key.
inline constexpr const char* kHotkeysHint = "%s toggle | %s screenshot";
inline constexpr const char* kResetHotkeys = "Reset hotkeys";
inline constexpr const char* kDiagnostics = "Diagnostics";
inline constexpr const char* kDiagnosticsHint = "counters, support report";
inline constexpr const char* kFixes = "Fixes";
inline constexpr const char* kFixesHint = "only change if you know what you are doing";
inline constexpr const char* kResetFixes = "Reset fixes";
inline constexpr const char* kFixesWarning =
    "These correct games that report their HDR or motion data wrongly.  The"
    " defaults are right for nearly every game, and a wrong value here breaks"
    " the image.  Reset fixes puts every one of them back.";
inline constexpr const char* kFixesHdr = "HDR";
inline constexpr const char* kFixesGuides = "Depth and motion";
inline constexpr const char* kAdaptationSpeed = "HDR adaptation speed, in stops per second";
inline constexpr const char* kRestoreAll = "Restore all defaults";
inline constexpr const char* kRestoreConfirm =
    "Every setting for this game goes back to its default, hotkeys included.";
inline constexpr const char* kDeveloperView = "Developer view";
inline constexpr const char* kDeveloperViewTip =
    "The classic flat layout: every control in one list, as before v7.";

// The panel's language (UiLanguage).
inline constexpr const char* kLanguage = "Language";
inline constexpr const char* kLanguageAuto = "Auto (ReShade's language)";  // i18n: skip (never translated, see DrawSectionLanguage)
inline constexpr const char* kLanguageTip =
    "Auto follows ReShade's own language (ReShade's Settings tab), which"
    " follows Windows unless you picked one there.  ReShade loads the font"
    " for its own language only, so a language written in another script"
    " can show as boxes: pick it in ReShade's settings too.";

// The resolution presets, in segment order.  Quality, Balanced and
// Performance are the working-resolution fractions of DLSS's own modes;
// "Match game" follows the game's render resolution (NRFollowInputRes).
inline constexpr const char* kResolutionLabels[] = {
    "Full", "Quality", "Balanced", "Performance", "Match game"};
inline constexpr const char* kResolutionTip =
    "The resolution Neural Rendering works at.  Full is the sharpest; the"
    " lower presets cost less GPU time.  Match game follows the game's own"
    " render resolution.";
inline constexpr const char* kCustomResolution = "Custom (%.0f%%): set in Performance";

// Diagnostics, translated (U3).  Raw details keeps the alpha44 counters.
inline constexpr const char* kLastFrame = "Last frame";
inline constexpr const char* kFramesThisSession = "Frames this session";
inline constexpr const char* kWorkingResolution = "Working resolution";
inline constexpr const char* kBackend = "Backend";
inline constexpr const char* kRawDetails = "Raw details";
inline constexpr const char* kCopySupportReport = "Copy support report";
inline constexpr const char* kOpenLogFolder = "Open log folder";
inline constexpr const char* kReportCopied =
    "Copied. Paste it into your report together with ReShade.log.";
inline constexpr const char* kReportTip =
    "Copies a short text with the version, the verdict, the card and the"
    " counters.  It holds the game's file name and no folder paths.";

// The HUD pill's second line (U4): where the details are, and the one key
// that silences it.  %s: the overlay tab's name, then the toggle key.
inline constexpr const char* kHudHint =
    "Details: the %s tab in ReShade's overlay.  %s turns Neural Rendering off.";

// The first-run welcome (U5), shown under the status card until dismissed.
inline constexpr const char* kWelcomeTitle = "Welcome to DLSS 5 Neural Rendering";
inline constexpr const char* kWelcomeBody =
    "Neural Rendering is NVIDIA's DLSS enhancer.  It works on the image the"
    " game's own DLSS makes, so DLSS or DLAA has to be on in the game's"
    " settings.";
inline constexpr const char* kWelcomeSteps[] = {
    "The card above says whether it is working.  When it is not, a note in"
    " the game's top-left corner says so, even with this overlay closed.",
    "The switch below turns it on and off; in game, the key shown beside it"
    " does the same.",
    "Strength and the Look section shape the image; a row you changed gets a"
    " reset arrow.",
};

// i18n: end
}  // namespace text

}  // namespace renodx::addons::dlss5::ui
