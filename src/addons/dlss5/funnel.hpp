/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The engagement funnel and the one verdict derived from it
// (PLAN_REHAB_V7.md R1).
//
// Six consecutive releases shipped with a green harness and four of them
// engaged on 0 % of evaluates in the field.  Nothing in the addon could say
// so: the overlay reported "ACTIVE - NR INJECTED" while every evaluate was
// declining, because its ladder ended at `active_features != 0` - a feature
// bound to an output, not a frame NR enhanced.  The session totals
// printed only inside Shutdown(), which a crashing or Alt-F4'd game never
// reaches.
//
// So engagement gets ONE definition, in one place, computed from counters
// that are a partition rather than a pile:
//
//   addon_loaded -> nr_enabled -> runtime_available -> ngx_hooked ->
//   dlss_feature_seen -> evaluate_seen -> evaluate_eligible ->
//   list_hooks_live -> nr_feature_ready -> gate_open -> injected ->
//   submitted_ok
//
// The verdict names the FIRST stage that did not happen, which is the only
// thing a reporter needs to send and the only thing a triager needs to read.
//
// This header stays free of addon dependencies (no reshade, no NGX, no
// Detours, no logging globals): the decision is a pure function of a plain
// struct, so test/dlss5 can run every rung of the ladder - including the ones
// that need a 100 %-decline session - without a device or a game.

#pragma once

#include <cstdint>
#include <cstdio>

namespace renodx::addons::dlss5 {

// ---------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------

enum class FunnelStage : std::uint8_t {
  // The addon's DllMain ran and ReShade loaded it.  False is only ever
  // observed from outside (no addon lines in the log at all).
  kAddonLoaded = 0,
  // The NeuralUplift setting is on AND the hook policy allows NR.
  kNrEnabled,
  // A signed nvngx_dlssnr.dll is present and the GPU is supported.
  kRuntimeAvailable,
  // The NGX export detours are installed on at least one loaded module.
  kNgxHooked,
  // The game created a DLSS (feature 1) or DLSS-D feature.
  kDlssFeatureSeen,
  // At least one NGX evaluate reached the hook.
  kEvaluateSeen,
  // At least one evaluate was a real game DLSS evaluate: not a non-DLSS
  // feature, and not this addon's own feature-18 evaluate re-entering the
  // hook, and not an enclosing layer mirroring an inner hook's work.
  kEvaluateEligible,
  // The command-list state hooks resolved and installed.
  kListHooksLive,
  // An NR (feature 18) slot exists and is bound to an output.
  kNrFeatureReady,
  // The compute-state restore target completed at least once, so the
  // injection gate opened rather than declining every frame.
  kGateOpen,
  // NR actually ran on at least one evaluate.
  kInjected,
  // The list carrying an injection was submitted without the queue detour
  // rejecting it.
  kSubmittedOk,
  kCount,
};

inline constexpr std::size_t kFunnelStageCount =
    static_cast<std::size_t>(FunnelStage::kCount);

// Stage names are a FIELD CONTRACT: they appear in the verdict line that
// reporters paste and tools parse.  A rename is a breaking change and belongs
// with a schema bump, which is why test/dlss5 pins them.
inline const char* const kFunnelStageNames[kFunnelStageCount] = {
    "addon_loaded",      "nr_enabled",        "runtime_available",
    "ngx_hooked",        "dlss_feature_seen", "evaluate_seen",
    "evaluate_eligible", "list_hooks_live",   "nr_feature_ready",
    "gate_open",         "injected",          "submitted_ok",
};

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------

// The API the process presents through (verdict v3, PLAN_DX11_V68.md 4.6).
// Sticky the way the addon's flags are: one D3D12 swapchain or present makes
// the session D3D12 for good.  kUnknown before any swapchain or present.
enum class SessionApi : std::uint8_t {
  kUnknown = 0,
  kD3D12,
  kD3D11,
  kOther,
};

inline const char* SessionApiName(SessionApi api) {
  switch (api) {
    case SessionApi::kD3D12: return "d3d12";
    case SessionApi::kD3D11: return "d3d11";
    case SessionApi::kOther: return "other";
    case SessionApi::kUnknown: break;
  }
  return "-";
}

// Everything the verdict is allowed to depend on, as plain data.  Filling
// this from the addon's globals is a dozen atomic loads; keeping the decision
// out of the addon is what makes it testable.
struct FunnelInputs {
  // Session facts.  Every one of them is CUMULATIVE - "did this stage happen
  // in this session" - because that is the question the ladder answers, and
  // a stage that happened must never read as one that did not.  The three
  // that used to be live state cost exactly that: a teardown (the game
  // released its DLSS feature, or the swapchain NR was initialized on went
  // away) legitimately un-hooks everything and drops the feature count for a
  // few frames, and a session that had enhanced 238 of 240 evaluates then
  // reported NOT_ENGAGED at stage=nr_feature_ready, or NOT_ENGAGED at
  // stage=ngx_hooked two frames before recovering on its own.  Both measured
  // on the T2 lanes the moment a teardown trigger existed to make them
  // deterministic; both are the field's normal behaviour (a resolution
  // change, an alt-tab), so both were false alarms in a log a reporter
  // reads.
  //
  // Whether NR is running RIGHT NOW is a different question with its own
  // answers: the telemetry line's per-interval nr[evals=], the decline tag,
  // and the ratio.  Asking a rung to carry it is how a warning stops being
  // read.
  bool hooks_enabled = true;
  bool nr_enabled = false;
  bool runtime_available = false;
  bool ngx_hooked = false;
  bool list_hooks_live = false;
  bool dlss_feature_seen = false;
  bool nr_feature_ready = false;
  bool gate_ever_opened = false;

  // Evaluate accounting.  `seen` counts every evaluate that reached the hook,
  // including ones that were never ours to inject; `eligible` is the
  // denominator engagement is measured against, and `injected` the numerator.
  std::uint64_t seen = 0;
  std::uint64_t eligible = 0;
  std::uint64_t injected = 0;
  // Submissions seen to carry the addon's recorded work.  Reported, NOT
  // judged - but no longer blind.
  //
  // Through v6.7 this read ZERO on every engaged session of every profile,
  // and the reason was not the observer: the submission tracker keyed its
  // lists by pointer, while under ReShade the record side is handed the
  // wrapper and the submit side the native, so no submission could ever be
  // matched to the work it carried (submission_tracker.hpp, ListIdentity).
  // v6.8.0 keys by a stable identity and the count is real.
  //
  // It stays reported rather than judged for one more release because the
  // question "was our work in this submission" is now answered by code
  // whose first field evidence is this build's: the criterion for gating
  // the verdict on it is `submitted > 0` on every ENGAGED profile of the
  // T-NOREG matrix plus one owner field session, and until that is
  // measured, gating would repeat the exact mistake the verdict exists to
  // stop - deciding engagement from a signal not yet known to mean what it
  // is being read to mean.  T-PROOF (test/dlss5_e2e/tests.cpp) asserts the
  // count is non-zero, so a silent return to zero fails the gate even while
  // the verdict ignores it.
  std::uint64_t submitted_ok = 0;
  // How many times the addon's present handler ran.  Not a funnel rung - it
  // is the question that comes BEFORE the ladder, and the ladder cannot ask
  // it: every rung reports on the game, so a session in which the addon was
  // never ticked at all reads as `stage=dlss_feature_seen`, "the game
  // created no DLSS feature".  Measured on a v6.7.3-recovery1 GTA V
  // Enhanced session (2026-09-20): ZERO telemetry lines in 5 minutes from a
  // build that emits one every 5 seconds, across a launcher window, two
  // unload/reload cycles and a non-primary device - and the archive scored
  // it as the game's fault.  `presents=0` on a shutdown-triggered verdict is
  // that session saying so itself.
  std::uint64_t presents = 0;
  // v7.0.0 (A-3, PLAN_REHAB_V7.md 8 row 24): the process presented through
  // D3D11 and never through D3D12.  Not a rung either - it is the question of
  // which API the rungs are about.  This build hooks only the NGX core's D3D12
  // exports, so in such a process "no DLSS feature was created" means "none
  // was created through D3D12", and a D3D11 game running DLSS reads as a game
  // that forgot to turn it on.
  bool d3d11_only = false;
  // v7.0.0 (A-6): DX11Source=off in a process that presents through D3D11
  // and has shown no D3D12 swapchain or present.  The addon is inert by the
  // user's choice there, which is OFF, not a stage's fault.
  bool dx11_source_off = false;
  // v7.0.0: another Neural Rendering producer created a feature-18 instance
  // in this process and ForeignNr=yield stands this addon down.  A choice,
  // like DX11Source=off: OFF, not a stage's fault.
  bool foreign_nr = false;
  // v3: the presenting API (above).  Reported, and the input the path is
  // derived from; the rungs keep reading d3d11_only.
  SessionApi api = SessionApi::kUnknown;
  // v7.0.0-alpha45 (Path B): the game's own D3D11 DLSS exports were
  // detoured, so its evaluates run NR through the Direct3D 11 bridge.  The
  // path is "bridge", and the d3d11_native rung no longer applies: this
  // build does serve the game's API.
  bool d3d11_bridge = false;
  // Terminals that did not name themselves.  Any value above zero means the
  // partition leaks and the ratio below cannot be trusted (T-SILENT).
  std::uint64_t unaccounted = 0;

  // The decline that terminated the most eligible evaluates, as a short tag
  // ("state_target", "backoff", ...), or null when nothing declined.
  const char* top_decline_tag = nullptr;
  std::uint64_t top_decline_count = 0;
  // When the top decline is the restore-target gate, WHICH aspect was most
  // often unobserved ("root_arguments", "heaps", ...).  "state_target" on its
  // own does not say what to fix; this does, and it is the single field that
  // turns a field report into a triage - v6.7.2 declined 100% of AW2 and
  // KCD2 evaluates on exactly one of these, and no log said which.
  const char* top_missing_aspect = nullptr;
};

// ---------------------------------------------------------------------------
// Verdict
// ---------------------------------------------------------------------------

enum class NrState : std::uint8_t {
  // The addon is loaded, NR is on, and evaluates are being enhanced.
  kEngaged = 0,
  // NR is on and has work to do, and it is not happening.  This is the state
  // four shipped releases were in while the overlay said ACTIVE.
  kNotEngaged,
  // NR is switched off by the user or by hook policy.  Not a fault.
  kOff,
  // NR is on but cannot run here: no signed runtime, an unsupported GPU, or
  // (reason d3d11_native) a D3D11 game on the foreign route whose own DLSS
  // exports are not served, and no third-party D3D12 DLSS has arrived.  Not
  // a fault of the game or the user, and it must never be reported as one.
  kUnavailable,
};

inline const char* NrStateName(NrState state) {
  switch (state) {
    case NrState::kEngaged: return "ENGAGED";
    case NrState::kNotEngaged: return "NOT_ENGAGED";
    case NrState::kOff: return "OFF";
    case NrState::kUnavailable: return "UNAVAILABLE";
  }
  return "NOT_ENGAGED";
}

struct NrVerdict {
  NrState state = NrState::kNotEngaged;
  // The first stage of the funnel that did not happen.  For an ENGAGED
  // verdict this is kCount ("-"): nothing is missing.
  FunnelStage stage = FunnelStage::kCount;
  const char* reason = nullptr;
  std::uint64_t seen = 0;
  std::uint64_t eligible = 0;
  std::uint64_t injected = 0;
  std::uint64_t unaccounted = 0;
  const char* missing = nullptr;
  // injected / eligible, or 0 when nothing was eligible.  T-ENG wants
  // >= 0.99 once the session is warm.
  double ratio = 0.0;
  // v3: which API the game presents through and which NR path serves it -
  // "inline" (D3D12: NR on the game's own list), "foreign" (D3D11 game, a
  // third-party tool's D3D12 NGX work: Path A), "bridge" (D3D11 game, its
  // own D3D11 DLSS followed by NR through the Direct3D 11 bridge: Path B),
  // "-" when no path applies.  Never null.
  const char* api = "-";
  const char* path = "-";

  const char* StageName() const {
    return stage == FunnelStage::kCount
        ? "-"
        : kFunnelStageNames[static_cast<std::size_t>(stage)];
  }
  const char* ReasonName() const { return reason != nullptr ? reason : "-"; }
  const char* MissingName() const { return missing != nullptr ? missing : "-"; }
};

// The whole decision, in the order the stages happen.  Each rung answers
// "did this happen?" and the first no is the verdict; there is deliberately
// no path that reports success from a later signal while an earlier one is
// missing, because that is precisely the bug this replaces (a bound feature
// was read as an enhanced frame).
inline NrVerdict ComputeNrVerdict(const FunnelInputs& in) {
  NrVerdict out;
  out.seen = in.seen;
  out.eligible = in.eligible;
  out.injected = in.injected;
  out.unaccounted = in.unaccounted;
  out.ratio = in.eligible != 0
      ? static_cast<double>(in.injected) / static_cast<double>(in.eligible)
      : 0.0;
  out.reason = in.top_decline_tag;
  out.missing = in.top_missing_aspect;
  // v3's api/path hold on every rung, OFF and UNAVAILABLE included: they
  // say what the session IS, not how far it got.  A D3D11 game has the
  // bridge path once its own D3D11 exports are detoured (Path B), the
  // foreign path when a tool's D3D12 NGX work reached the hooks (Path A);
  // with the addon inert by DX11Source=off it has none.
  out.api = SessionApiName(in.api);
  if (in.api == SessionApi::kD3D12) {
    out.path = "inline";
  } else if (in.api == SessionApi::kD3D11 && !in.dx11_source_off
             && in.d3d11_bridge) {
    out.path = "bridge";
  } else if (in.api == SessionApi::kD3D11 && !in.dx11_source_off
             && (in.dlss_feature_seen || in.seen != 0)) {
    out.path = "foreign";
  }

  // Off and unavailable are not failures and must not be reported as any
  // stage's fault: a user who turned NR off, and a machine with no signed
  // runtime, are both working as intended.
  if (!in.hooks_enabled || !in.nr_enabled || in.dx11_source_off
      || in.foreign_nr) {
    out.state = NrState::kOff;
    out.stage = FunnelStage::kNrEnabled;
    out.reason = !in.hooks_enabled ? "hooks_disabled"
        : !in.nr_enabled           ? "nr_disabled"
        : in.dx11_source_off       ? "dx11_source_off"
                                   : "foreign_nr";
    return out;
  }
  if (!in.runtime_available) {
    out.state = NrState::kUnavailable;
    out.stage = FunnelStage::kRuntimeAvailable;
    out.reason = "no_signed_runtime";
    return out;
  }
  // A-3: NGX is hooked in a D3D11-only process and nothing D3D12 ever
  // arrived - no DLSS create, no evaluate.  The ladder below would stop at
  // dlss_feature_seen, "the game created no DLSS feature", which blames the
  // game for the addon's API.  A D3D11 process where a third-party tool does
  // drive D3D12 NGX (Path A) has a create or an evaluate and takes the
  // ladder; one with no NGX at all stops at ngx_hooked as before.  On the
  // bridge path the D3D11 exports ARE hooked, so "no DLSS create" is the
  // truth about the game and the ladder says it.
  if (in.d3d11_only && in.ngx_hooked && !in.d3d11_bridge
      && !in.dlss_feature_seen && in.seen == 0) {
    out.state = NrState::kUnavailable;
    out.stage = FunnelStage::kDlssFeatureSeen;
    out.reason = "d3d11_native";
    return out;
  }

  out.state = NrState::kNotEngaged;
  if (!in.ngx_hooked) {
    out.stage = FunnelStage::kNgxHooked;
  } else if (!in.dlss_feature_seen) {
    out.stage = FunnelStage::kDlssFeatureSeen;
  } else if (in.seen == 0) {
    out.stage = FunnelStage::kEvaluateSeen;
  } else if (in.eligible == 0) {
    out.stage = FunnelStage::kEvaluateEligible;
  } else if (!in.list_hooks_live) {
    out.stage = FunnelStage::kListHooksLive;
  } else if (!in.nr_feature_ready) {
    out.stage = FunnelStage::kNrFeatureReady;
  } else if (!in.gate_ever_opened) {
    out.stage = FunnelStage::kGateOpen;
  } else if (in.injected == 0) {
    out.stage = FunnelStage::kInjected;
  } else {
    out.state = NrState::kEngaged;
    out.stage = FunnelStage::kCount;
    // An engaged session still names what declined most, because engaged is
    // not the same as engaging on every frame: a 0.4 ratio is ENGAGED and
    // still wrong, and the tag is the first thing to look at.
  }
  return out;
}

// ---------------------------------------------------------------------------
// The line
// ---------------------------------------------------------------------------

// Formats the verdict line.  It lives here, next to the decision, because it
// is a CONTRACT: `tools/field/log_verdict.py` matches it, and the addon and
// the tool drifting apart is a failure this project has already had (the
// canary and long-run patterns stopped matching the log text and nobody
// noticed until a field run needed them).  test/dlss5 pins the exact output
// and log_verdict.py --selftest pins the same string against its regex, so
// the two can only move together.
//
// v3 (v7.0.0) appends api= and path= after trigger=; every v2 field keeps
// its place, so a v2-era reader of the prefix still reads the same values.
//
// Returns the number of characters written, as std::snprintf does.
inline int FormatNrVerdictLine(
    char* buffer,
    std::size_t size,
    const NrVerdict& verdict,
    std::uint64_t submitted,
    std::uint64_t presents,
    const char* build,
    const char* trigger) {
  return std::snprintf(
      buffer, size,
      "NR-VERDICT v3 state=%s stage=%s reason=%s missing=%s seen=%llu"
      " eligible=%llu evals=%llu ratio=%.4f unaccounted=%llu submitted=%llu"
      " presents=%llu build=%s trigger=%s api=%s path=%s",
      NrStateName(verdict.state), verdict.StageName(), verdict.ReasonName(),
      verdict.MissingName(),
      static_cast<unsigned long long>(verdict.seen),
      static_cast<unsigned long long>(verdict.eligible),
      static_cast<unsigned long long>(verdict.injected), verdict.ratio,
      static_cast<unsigned long long>(verdict.unaccounted),
      static_cast<unsigned long long>(submitted),
      static_cast<unsigned long long>(presents), build, trigger, verdict.api,
      verdict.path);
}

}  // namespace renodx::addons::dlss5
