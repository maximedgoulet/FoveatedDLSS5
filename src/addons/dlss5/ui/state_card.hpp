/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The status card: one card per state, decided as a pure function
// (PLAN_UI_V7.md 3).
//
// The overlay's first job is the one question a player opens it with - "is it
// working?" - and every answer it can give lives in this table.  Through
// alpha43 the answer was a ladder of funnel rungs in capitals ("EVALUATES SEEN
// - NONE WERE OURS"), which names what the ADDON saw, not what the PLAYER
// should do, and a D3D11 game running DLSS was told its DLSS was off.  The
// card names the outcome in plain language, colours it by who has to act, and
// carries the fix inline.
//
// Two rules the table must never break, both unit-asserted:
//   - a card is red only when the player must install or replace something.
//     A game without DLSS, a D3D11 game and a setting the player chose are
//     gray: nothing is wrong (the God of War lesson);
//   - every amber or red card carries its fix, not a pointer to a log.
//
// Like funnel.hpp this header has no addon dependencies (no ImGui, no
// reshade, no globals): test/dlss5 runs every row without a device.  The
// overlay renders the card; the NR-CARD log line names it, so the lanes can
// assert the card a real session reached.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../funnel.hpp"
#include "i18n.hpp"

namespace renodx::addons::dlss5::ui {

// ---------------------------------------------------------------------------
// The atoms the verdict does not carry
// ---------------------------------------------------------------------------

// Why the signed runtime is unavailable.  The funnel has one rung for it,
// runtime_available, and the loader's DirectLoadState one value, Failed; the
// player needs to know which of two different fixes applies.
enum class RuntimeFault : std::uint8_t {
  kNone = 0,
  // No nvngx_dlssnr.dll beside the addon or the game executable.
  kMissing,
  // A file was found and could not be used: it did not load, lacked a
  // required export, or the caller-identity patch failed.
  kUnusable,
};

// The Streamline shape the EnableHooks=1 hint fired for (alpha40 logic): the
// hint used to exist only as a log line, so the overlay could not show it.
enum class StreamlineShape : std::uint8_t {
  kNone = 0,
  // NGX evaluates reach the hooks and none engaged while sl.common.dll is
  // loaded: the guides likely arrive as Streamline tags.
  kGuidesViaTags,
  // sl.common.dll is loaded and not one DLSS create or evaluate reached the
  // NGX hooks: the title drives DLSS entirely through Streamline.
  kNothingIntercepted,
};

// The starting grace: NR was switched on (or the session began with it on)
// less than `grace_ns` ago.  `nr_on_since_ns` is 0 until the lifecycle tick
// anchors it, which reads as starting.  A grace of zero - the lanes - never
// starts, so a lane sees the card its session settles on at once.
inline bool InStartingWindow(std::int64_t now_ns, std::int64_t nr_on_since_ns,
                             std::int64_t grace_ns) {
  if (grace_ns <= 0) return false;
  if (nr_on_since_ns == 0) return true;
  return now_ns - nr_on_since_ns < grace_ns;
}

// ---------------------------------------------------------------------------
// The card
// ---------------------------------------------------------------------------

enum class CardTone : std::uint8_t {
  kGood = 0,  // NR is enhancing frames
  kWarn,      // not engaged, and the player can fix it
  kBad,       // the player must install or replace something
  kIdle,      // off by choice or nothing to do: nothing is wrong
};

enum class CardGlyph : std::uint8_t {
  kCheck = 0,
  kWarning,
  kCross,
  kIdle,     // an empty circle: at rest
  kWaiting,  // an hourglass: about to change by itself
  kInfo,     // an i: the welcome card (U5), not a state
};

// One per PLAN_UI_V7.md 3 row.  The slugs are a FIELD CONTRACT: the NR-CARD
// log line carries them and the lanes assert them.  Append only.
enum class CardId : std::uint8_t {
  kActive = 0,
  kPartlyActive,
  kWaitingForDlss,
  kStarting,
  kDlssOff,
  kFrameGenOnly,
  kStreamlineSetting,
  kNotRunning,
  kRuntimeMissing,
  kRuntimeFailed,
  kNoDlss,
  kDx11Game,
  kDisabledBySetting,
  kOffByUser,
  kForeignNr,
  kCount,
};

inline constexpr std::size_t kCardIdCount = static_cast<std::size_t>(CardId::kCount);

inline const char* const kCardSlugs[kCardIdCount] = {
    "active",          "partly_active",       "waiting_for_dlss",
    "starting",        "dlss_off",            "frame_gen_only",
    "streamline_setting", "not_running",      "runtime_missing",
    "runtime_failed",  "no_dlss",             "dx11_game",
    "disabled_by_setting", "off_by_user", "foreign_nr",
};

inline const char* CardSlug(CardId id) {
  const auto index = static_cast<std::size_t>(id);
  return index < kCardIdCount ? kCardSlugs[index] : "-";
}

inline const char* CardToneName(CardTone tone) {
  switch (tone) {
    case CardTone::kGood: return "good";
    case CardTone::kWarn: return "warn";
    case CardTone::kBad: return "bad";
    case CardTone::kIdle: return "idle";
  }
  return "idle";
}

// What a card's button does, when it has one.
enum class CardAction : std::uint8_t {
  kNone = 0,
  kTurnOn,           // switch NR on
  kOpenDiagnostics,  // expand Diagnostics
  kDismiss,          // close the welcome card for good (U5)
};

// Everything the card may depend on, as plain data.
struct CardInputs {
  bool partial_latched = false;
  NrVerdict verdict;
  // The alpha40 hint machine: 0 none, 1 DLSS off, 2 frame generation only,
  // 3 DLSS outside Direct3D 12 (A-4).
  std::uint32_t dlss_hint = 0;
  // Live, not cumulative: the one input that answers "right now".
  std::uint32_t active_features = 0;
  bool pre_sr = false;
  bool streamline_hooks_on = false;
  StreamlineShape streamline_shape = StreamlineShape::kNone;
  RuntimeFault runtime_fault = RuntimeFault::kNone;
  bool starting = false;
  // Latest evaluate's sizes, 0 when unknown.
  std::uint32_t input_width = 0;
  std::uint32_t input_height = 0;
  std::uint32_t output_width = 0;
  std::uint32_t output_height = 0;
};

inline constexpr std::size_t kMaxFixSteps = 3;

// Below this many eligible evaluates an engaged session is not judged
// partial: the first evaluates of a session decline by design (warm-up), so
// the cumulative ratio starts under 0.99 and crosses it a second later.
// Measured on the rebind_all lane: starting -> partly_active -> active, an
// amber flash on a healthy session.  300 is five seconds at 60 fps.
inline constexpr std::uint64_t kPartialMinEligible = 300;
// Enter amber below 98%; recover at 99%, avoiding a flickering status card.
inline constexpr double kPartialEnterRatio = 0.98;
inline constexpr double kPartialLeaveRatio = 0.99;

struct StatusCard {
  CardId id = CardId::kNotRunning;
  CardTone tone = CardTone::kWarn;
  CardGlyph glyph = CardGlyph::kWarning;
  const char* title = "";
  const char* body = "";
  // Which path a live card is served through, or null ("inline" D3D12 needs
  // no note).
  const char* path_note = nullptr;
  const char* fix[kMaxFixSteps] = {nullptr, nullptr, nullptr};
  std::size_t fix_count = 0;
  CardAction action = CardAction::kNone;
  // Carried for FormatCardDetail.
  std::uint64_t frames_enhanced = 0;
  std::uint64_t frames_eligible = 0;
  std::uint32_t input_width = 0;
  std::uint32_t input_height = 0;
  std::uint32_t output_width = 0;
  std::uint32_t output_height = 0;
};

namespace card_internal {

inline void SetFix(StatusCard& card, const char* a, const char* b = nullptr,
                   const char* c = nullptr) {
  card.fix[0] = a;
  card.fix[1] = b;
  card.fix[2] = c;
  card.fix_count = (a != nullptr) + (b != nullptr) + (c != nullptr);
}

inline void Set(StatusCard& card, CardId id, CardTone tone, CardGlyph glyph,
                const char* title, const char* body) {
  card.id = id;
  card.tone = tone;
  card.glyph = glyph;
  card.title = title;
  card.body = body;
}

inline bool Equal(const char* a, const char* b) {
  return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

// i18n: begin - display strings (tools/i18n/ui_strings.py catalog)
// A Direct3D 11 game on the foreign route (Path A) that no third-party
// Direct3D 12 DLSS has reached: reached from the verdict's d3d11_native rung
// and from the hint machine's state 3, which say the same thing.  Gray -
// nothing is wrong - and it still names the setting that serves the game's
// own DLSS, because the route is a choice the player can change.
inline constexpr const char* kDx11ForeignTitle =
    "DX11 game - NR waits for a D3D12 tool";
inline constexpr const char* kDx11ForeignBody =
    "This session serves only a third-party tool's Direct3D 12 DLSS "
    "(DX11Source=foreign, or another project's Direct3D 11 bridge add-on is "
    "loaded), and none has run. Nothing is wrong.";
inline constexpr const char* kDx11ForeignFix =
    "To serve the game's own DLSS, set DX11Source=native in [RenoDX.DLSS5] in "
    "ReShade.ini and restart the game.";

// The NOT_ENGAGED rungs past the hint machine, in plain language.  Every one
// is amber with a fix: DLSS is loaded, so something the player or a support
// report can act on stands between it and NR.
//
// On the bridge path (Path B) the rungs from list_hooks_live on mean
// something else: the D3D12 list hooks are the private device the bridge
// brings up, and the NR feature is created only after the frame's surfaces
// have crossed.  A bridge that never came up, or a frame whose surfaces
// cannot cross, would otherwise read as "could not attach safely" or "the
// runtime did not start" - the wrong fix - so the bridge's own declines name
// the card first.
inline void NotRunningRung(StatusCard& card, const NrVerdict& verdict) {
  if (Equal(verdict.path, "bridge") && verdict.stage >= FunnelStage::kListHooksLive) {
    if (Equal(verdict.reason, "bridge_format")) {
      Set(card, CardId::kNotRunning, CardTone::kWarn, CardGlyph::kWarning,
          "NR can't read this game's DLSS surfaces",
          "The game's DLSS output uses a format or multisampling that the "
          "Direct3D 11 bridge cannot share with Direct3D 12, so every frame "
          "keeps the game's own DLSS image.");
      SetFix(card,
             "Copy the support report under Diagnostics and share it with "
             "ReShade.log; it names the surface format.");
      card.action = CardAction::kOpenDiagnostics;
      return;
    }
    if (Equal(verdict.reason, "bridge_lost") || Equal(verdict.reason, "bridge_timeout")) {
      Set(card, CardId::kNotRunning, CardTone::kWarn, CardGlyph::kWarning,
          "NR's Direct3D 11 bridge stopped",
          "The private Direct3D 12 device NR runs on beside this game stalled "
          "or was removed, so NR stepped aside and the game keeps its own "
          "DLSS image.");
      SetFix(card, "Restart the game.",
             "If it happens again, update the NVIDIA driver and share "
             "ReShade.log.");
      return;
    }
    if (verdict.stage == FunnelStage::kListHooksLive) {
      Set(card, CardId::kNotRunning, CardTone::kWarn, CardGlyph::kWarning,
          "NR could not start the Direct3D 11 bridge",
          "Neural Rendering runs on a private Direct3D 12 device beside this "
          "Direct3D 11 game, and that device did not start.");
      SetFix(card, "Update the NVIDIA driver and restart the game.",
             "ReShade.log names the failing step ('D3D11 bridge could not "
             "start').");
      card.action = CardAction::kOpenDiagnostics;
      return;
    }
  }
  switch (verdict.stage) {
    case FunnelStage::kDlssFeatureSeen:
      Set(card, CardId::kNotRunning, CardTone::kWarn, CardGlyph::kWarning,
          "NR is idle - DLSS has not started",
          "NVIDIA's DLSS is loaded, but the game has not started DLSS "
          "upscaling.");
      SetFix(card,
             "Turn on DLSS or DLAA in the game's graphics settings.",
             "If DLSS is already on, set EnableHooks=1 in [RenoDX.DLSS5] in "
             "ReShade.ini and restart the game.");
      return;
    case FunnelStage::kEvaluateSeen:
      Set(card, CardId::kNotRunning, CardTone::kWarn, CardGlyph::kWarning,
          "NR is idle - DLSS is set up but not running",
          "The game created its DLSS upscaler but has not run it yet.");
      SetFix(card,
             "Load into gameplay with DLSS on; this card turns green when NR "
             "runs.");
      return;
    case FunnelStage::kEvaluateEligible:
      Set(card, CardId::kNotRunning, CardTone::kWarn, CardGlyph::kWarning,
          "NR can't reach this game's DLSS",
          "DLSS is running, but every call so far belonged to another DLSS "
          "feature or came through a path the addon cannot see.");
      SetFix(card,
             "If the game uses NVIDIA Streamline, set EnableHooks=1 in "
             "[RenoDX.DLSS5] in ReShade.ini and restart the game.");
      return;
    case FunnelStage::kListHooksLive:
      Set(card, CardId::kNotRunning, CardTone::kWarn, CardGlyph::kWarning,
          "NR could not attach safely",
          "The addon could not find the Direct3D 12 functions it needs to "
          "keep the game's state intact, so NR stays off rather than risk "
          "the image.");
      SetFix(card,
             "Copy the support report under Diagnostics and share it with "
             "ReShade.log.");
      card.action = CardAction::kOpenDiagnostics;
      return;
    case FunnelStage::kNrFeatureReady:
      Set(card, CardId::kNotRunning, CardTone::kWarn, CardGlyph::kWarning,
          "NR runtime did not start",
          "DLSS is running, but NVIDIA's Neural Rendering runtime did not "
          "start on this system.");
      SetFix(card, "Update the NVIDIA driver and restart the game.",
             "Diagnostics shows the runtime's last result; ReShade.log names "
             "the failing step.");
      card.action = CardAction::kOpenDiagnostics;
      return;
    case FunnelStage::kGateOpen:
      Set(card, CardId::kNotRunning, CardTone::kWarn, CardGlyph::kWarning,
          "NR is on but holding back",
          "The game's GPU state could not be restored safely around NR, so "
          "every frame was skipped rather than risk the image. This usually "
          "clears by itself.");
      SetFix(card,
             "If it never clears, copy the support report under Diagnostics "
             "and share it with ReShade.log.");
      card.action = CardAction::kOpenDiagnostics;
      return;
    case FunnelStage::kInjected:
    case FunnelStage::kSubmittedOk:
    default:
      Set(card, CardId::kNotRunning, CardTone::kWarn, CardGlyph::kWarning,
          "NR is on but has not run yet",
          "DLSS is running and NR is on, but NR has not run on a frame yet.");
      SetFix(card,
             "Copy the support report under Diagnostics; ReShade.log's "
             "NR-VERDICT line names the stage and the reason.");
      card.action = CardAction::kOpenDiagnostics;
      return;
  }
}

}  // namespace card_internal

// The whole table, in priority order.  Settled states outrank transient
// ones: OFF and UNAVAILABLE are decided before anything the session is still
// discovering, ENGAGED before the hint machine, and the hint machine before
// the rungs it explains better.
inline StatusCard ComputeStatusCard(const CardInputs& in) {
  using card_internal::Equal;
  using card_internal::Set;
  using card_internal::SetFix;
  StatusCard card;
  card.frames_enhanced = in.verdict.injected;
  card.frames_eligible = in.verdict.eligible;
  card.input_width = in.input_width;
  card.input_height = in.input_height;
  card.output_width = in.output_width;
  card.output_height = in.output_height;
  if (Equal(in.verdict.path, "foreign")) {
    card.path_note = "Running through a third-party tool's Direct3D 12 DLSS.";
  } else if (Equal(in.verdict.path, "bridge")) {
    card.path_note = "Running through the Direct3D 11 bridge.";
  }
  const bool d3d11 = Equal(in.verdict.api, "d3d11");

  switch (in.verdict.state) {
    case NrState::kOff:
      if (Equal(in.verdict.reason, "hooks_disabled")) {
        Set(card, CardId::kDisabledBySetting, CardTone::kIdle, CardGlyph::kIdle,
            "Disabled by setting",
            "EnableHooks=0 in [RenoDX.DLSS5] (ReShade.ini) turns every hook "
            "off. Set EnableHooks=2 to use Neural Rendering.");
      } else if (Equal(in.verdict.reason, "dx11_source_off")) {
        Set(card, CardId::kDisabledBySetting, CardTone::kIdle, CardGlyph::kIdle,
            "Disabled by setting",
            "DX11Source=off in [RenoDX.DLSS5] (ReShade.ini) keeps the addon "
            "inert in Direct3D 11 games. Remove the key to let Neural "
            "Rendering follow the game's DLSS again.");
      } else if (Equal(in.verdict.reason, "foreign_nr")) {
        Set(card, CardId::kForeignNr, CardTone::kIdle, CardGlyph::kIdle,
            "Another Neural Rendering mod is running",
            "Something else in this game already runs Neural Rendering, so "
            "this addon stands down instead of enhancing every frame twice.");
        SetFix(card,
               "To use this addon instead, remove the other mod and restart "
               "the game.",
               "To run both anyway, set ForeignNr=observe in [RenoDX.DLSS5] "
               "(ReShade.ini) and restart.");
      } else {
        Set(card, CardId::kOffByUser, CardTone::kIdle, CardGlyph::kIdle,
            "Neural Rendering is off",
            "Turned off in settings or by the toggle hotkey.");
        card.action = CardAction::kTurnOn;
      }
      return card;

    case NrState::kUnavailable:
      if (Equal(in.verdict.reason, "d3d11_native")) {
        Set(card, CardId::kDx11Game, CardTone::kIdle, CardGlyph::kIdle,
            card_internal::kDx11ForeignTitle, card_internal::kDx11ForeignBody);
        SetFix(card, card_internal::kDx11ForeignFix);
        return card;
      }
      if (in.runtime_fault == RuntimeFault::kMissing) {
        Set(card, CardId::kRuntimeMissing, CardTone::kBad, CardGlyph::kCross,
            "NR runtime not installed",
            "NVIDIA's signed nvngx_dlssnr.dll is not next to the addon or "
            "the game's executable.");
        SetFix(card,
               "Place nvngx_dlssnr.dll in the game's folder, next to the "
               "addon.",
               "Restart the game.");
      } else {
        Set(card, CardId::kRuntimeFailed, CardTone::kBad, CardGlyph::kCross,
            "NR runtime could not be used",
            "nvngx_dlssnr.dll was found but could not be loaded.");
        SetFix(card,
               "Replace it with an official NVIDIA build of "
               "nvngx_dlssnr.dll.",
               "Update the NVIDIA driver, then restart the game.",
               "ReShade.log names the failing step.");
      }
      return card;

    case NrState::kEngaged:
      if (in.active_features == 0) {
        Set(card, CardId::kWaitingForDlss, CardTone::kIdle, CardGlyph::kWaiting,
            "NR is waiting for DLSS",
            "It resumes by itself when the game runs DLSS again - normal "
            "after a resolution or quality change.");
        return card;
      }
      if (in.verdict.ratio >= (in.partial_latched ? kPartialLeaveRatio : kPartialEnterRatio)
          || in.verdict.eligible < kPartialMinEligible) {
        Set(card, CardId::kActive, CardTone::kGood, CardGlyph::kCheck,
            "Neural Rendering is active",
            in.pre_sr ? "Enhancing the game's DLSS input before upscaling"
                      : "Replacing the game's DLSS output");
        return card;
      }
      Set(card, CardId::kPartlyActive, CardTone::kWarn, CardGlyph::kWarning,
          "Neural Rendering is partly active",
          in.pre_sr ? "Enhancing the game's DLSS input before upscaling"
                    : "Replacing the game's DLSS output");
      SetFix(card,
             "Open Diagnostics: 'Skipped because' names why the other frames "
             "were skipped.");
      card.action = CardAction::kOpenDiagnostics;
      return card;

    case NrState::kNotEngaged:
    default:
      break;
  }

  // NOT_ENGAGED.  The hint machine outranks the rungs it explains; it only
  // claims a state after its own grace, so it never pre-empts Starting.
  switch (in.dlss_hint) {
    case 3u:
      if (d3d11) {
        Set(card, CardId::kDx11Game, CardTone::kIdle, CardGlyph::kIdle,
            card_internal::kDx11ForeignTitle, card_internal::kDx11ForeignBody);
        SetFix(card, card_internal::kDx11ForeignFix);
        return card;
      }
      Set(card, CardId::kDx11Game, CardTone::kIdle, CardGlyph::kIdle,
          "DLSS on an API NR does not serve",
          "This game runs DLSS without presenting through Direct3D 12 or "
          "Direct3D 11, the two APIs Neural Rendering serves. The addon is "
          "loaded and safe; nothing is wrong.");
      return card;
    case 2u:
      Set(card, CardId::kFrameGenOnly, CardTone::kWarn, CardGlyph::kWarning,
          "NR is idle - only Frame Generation is on",
          "Frame Generation is a different DLSS feature. Neural Rendering "
          "improves DLSS upscaling.");
      SetFix(card,
             "Turn on DLSS or DLAA upscaling in the game's graphics settings, "
             "not only Frame Generation.",
             "This card turns green when it works.");
      return card;
    case 1u:
      Set(card, CardId::kDlssOff, CardTone::kWarn, CardGlyph::kWarning,
          "NR is idle - DLSS upscaling is off",
          "Neural Rendering improves DLSS, so it needs DLSS on.");
      SetFix(card, "Open the game's graphics settings.",
             "Set DLSS or DLAA to any quality mode.",
             "This card turns green when it works.");
      return card;
    default:
      break;
  }
  // The EnableHooks=1 hint (alpha40): the title drives DLSS through
  // Streamline.  Moot once the Streamline layer is on.
  if (in.streamline_shape != StreamlineShape::kNone && !in.streamline_hooks_on) {
    Set(card, CardId::kStreamlineSetting, CardTone::kWarn, CardGlyph::kWarning,
        "NR needs the Streamline setting",
        in.streamline_shape == StreamlineShape::kGuidesViaTags
            ? "This game hands DLSS its inputs through NVIDIA Streamline, "
              "which the default hook mode cannot read."
            : "This game runs DLSS entirely through NVIDIA Streamline, which "
              "the default hook mode cannot see.");
    SetFix(card,
           "Set EnableHooks=1 in the [RenoDX.DLSS5] section of ReShade.ini.",
           "Restart the game.");
    return card;
  }
  if (in.starting) {
    Set(card, CardId::kStarting, CardTone::kIdle, CardGlyph::kWaiting,
        "Starting...",
        in.verdict.stage < FunnelStage::kEvaluateEligible
            ? "Waiting for the game to run DLSS."
            : "DLSS is running; Neural Rendering is getting ready.");
    return card;
  }
  if (in.verdict.stage == FunnelStage::kNgxHooked) {
    Set(card, CardId::kNoDlss, CardTone::kIdle, CardGlyph::kIdle,
        "This game isn't using DLSS",
        "No NVIDIA DLSS module has loaded, so Neural Rendering stays idle. "
        "Nothing is broken.");
    return card;
  }
  card_internal::NotRunningRung(card, in.verdict);
  return card;
}

// U4: the HUD pill shows a card only while NR is not working.  Gray (at rest,
// or off by choice) and green stay silent, and so does "partly active", which
// is working: a pill that is always there trains a player to ignore it.
inline bool ShowsOnHud(const StatusCard& card) {
  return card.tone == CardTone::kBad
         || (card.tone == CardTone::kWarn && card.id != CardId::kPartlyActive);
}

// The card's second line: counts and sizes, for the cards that have them,
// in the panel's language (English unless a language was selected).
// Returns the characters written, as std::snprintf does; an empty string for
// cards with no detail.
inline int FormatCardDetail(char* buffer, std::size_t size,
                            const StatusCard& card) {
  if (size == 0) return 0;
  buffer[0] = '\0';
  const bool sized = card.input_width != 0 && card.input_height != 0
                     && card.output_width != 0 && card.output_height != 0;
  char sizes[64] = {};
  if (sized) {
    std::snprintf(sizes, sizeof(sizes), "%ux%u -> %ux%u", card.input_width,
                  card.input_height, card.output_width, card.output_height);
  }
  switch (card.id) {
    case CardId::kActive:
      return std::snprintf(
          buffer, size, Tr("%s%s%llu frames enhanced this session"), sizes,
          sized ? " | " : "",
          static_cast<unsigned long long>(card.frames_enhanced));
    case CardId::kPartlyActive:
      return std::snprintf(
          buffer, size, Tr("%s%sEnhancing %llu of %llu frames"), sizes,
          sized ? " | " : "",
          static_cast<unsigned long long>(card.frames_enhanced),
          static_cast<unsigned long long>(card.frames_eligible));
    default:
      return 0;
  }
}

// i18n: end
// The NR-CARD line: one per card change and one at shutdown, so a lane - and
// a reporter's log - names the card a session reached.  A CONTRACT like the
// verdict line; test/dlss5 pins it.
inline int FormatCardLine(char* buffer, std::size_t size,
                          const StatusCard& card, const char* trigger) {
  return std::snprintf(buffer, size, "NR-CARD v1 card=%s tone=%s trigger=%s",
                       CardSlug(card.id), CardToneName(card.tone), trigger);
}

}  // namespace renodx::addons::dlss5::ui
