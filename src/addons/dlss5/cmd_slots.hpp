/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// D3D12 vtable slots used by the compute-state shadow: the command-list
// slots it detours, and the ID3D12Device slots it detours to see lists born.
//
// Every constant in this header is checked against the SDK by the compiler:
// test/dlss5/vtable_slots.c is compiled as C, where d3d12.h exposes the COM
// vtables as plain structs, and static-asserts each index with offsetof.  A
// wrong number fails the build with the name of the slot, which is the check
// v6.7.4 did not have when it detoured ID3D12Device slot 42 believing it was
// CreateCommandSignature (it is 41; 42 is GetResourceTiling).  Do not edit a
// number here without the assertion agreeing - and do not add one without
// asserting it.
// Slot 2 (IUnknown::Release) is deliberately NOT detoured: it is the hottest
// COM method on these objects, runs on every thread, and installing its
// detour mid-frame patches a body other threads execute.  Destruction is
// learned from ReShade's destroy_command_list event instead, which costs no
// detour at all (OnDestroyCommandListEvent in dlssnr.hpp).  Through v6.7
// this comment said "no list-destruction tracking exists at all", and it was
// right for the wrong reason: the handler was there, and the registration
// was not.  The hook installer validates and detaches exactly
// kCmdHookSlots - scanning a slot-index-sized array instead once made every
// unfilled entry look like an unresolved slot and declined the whole shadow
// (v5.3.0-dev field regression on KCD2: "NR skipped: command-list state
// shadow unavailable").

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace renodx::addons::dlss5 {

constexpr std::uint32_t kCmdSlotRelease = 2;
constexpr std::uint32_t kCmdSlotReset = 10;
constexpr std::uint32_t kCmdSlotClearState = 11;
constexpr std::uint32_t kCmdSlotSetPipelineState = 25;
constexpr std::uint32_t kCmdSlotExecuteBundle = 27;
constexpr std::uint32_t kCmdSlotSetDescriptorHeaps = 28;
constexpr std::uint32_t kCmdSlotSetComputeRootSignature = 29;
constexpr std::uint32_t kCmdSlotSetComputeRootDescriptorTable = 31;
constexpr std::uint32_t kCmdSlotSetComputeRoot32BitConstant = 33;
constexpr std::uint32_t kCmdSlotSetComputeRoot32BitConstants = 35;
constexpr std::uint32_t kCmdSlotSetComputeRootConstantBufferView = 37;
constexpr std::uint32_t kCmdSlotSetComputeRootShaderResourceView = 39;
constexpr std::uint32_t kCmdSlotSetComputeRootUnorderedAccessView = 41;

// Hooked since v6.8.0-alpha8, for the EXACT documented aftermath and nothing
// more (see indirect_signature.hpp for the quoted rule).  Both of the
// numbers below were written down from memory first and both were wrong
// (ExecuteIndirect as 55, SetPipelineState1 as 63); the assertions in
// test/dlss5/vtable_slots.c caught them on their first build, which is the
// check v6.7.4 did not have when it detoured device slot 42 believing it was
// CreateCommandSignature.
constexpr std::uint32_t kCmdSlotExecuteIndirect = 59;
// ID3D12GraphicsCommandList4 - a DIFFERENT vtable, so this index is not
// comparable with the ones above.  Not hooked.
constexpr std::uint32_t kCmdSlot4SetPipelineState1 = 75;

// ID3D12Device - the birth-time observation hooks (R2).  A command list's
// compute state is only fully KNOWN if the shadow saw the list born: a list
// first seen mid-recording may have had a root signature, heaps and root
// arguments bound before the shadow existed, and "we observed nothing" is
// not "nothing was bound".  That difference is the whole no-seed shape (007
// First Light, AW2 rs=0): the restore target can never complete, so every
// evaluate declines with state_target forever.
// CreateCommandList hands the list back ALREADY RECORDING, in the Reset
// aftermath with the pipeline state set to the pInitialState argument -
// which is why this hook has to be on the device call that carries that
// argument.  ReShade's init_command_list event fires at the same moment but
// does not pass pInitialState, and a baseline that guesses the pipeline
// state would restore a pipeline the host never bound.
constexpr std::uint32_t kDeviceSlotCreateCommandList = 12;
// ID3D12Device4 - a DIFFERENT vtable, so this index is not comparable with
// the one above.  CreateCommandList1 returns a CLOSED list and takes no
// initial pipeline state, so it is owed no baseline: the Reset the host must
// call before recording writes one through kCmdSlotReset.  It is hooked
// anyway, because in an engine that only ever calls CreateCommandList1 it is
// the earliest sight of a command-list vtable in the process.
constexpr std::uint32_t kDeviceSlot4CreateCommandList1 = 51;
// ID3D12Device - hooked since v6.8.0-alpha8, so that ExecuteIndirect can ask
// the signature what it names instead of guessing.  This is the slot v6.7.4
// got wrong (it detoured 42, GetResourceTiling, and its signature lookups
// therefore missed every time); the assertion keeps the correction honest.
// Games create their command signatures once, at load - after ReShade's
// init_device and before the first evaluate - which is why the hook belongs
// at device level and not, as v6.7.4 had it, at the first evaluate.
constexpr std::uint32_t kDeviceSlotCreateCommandSignature = 41;
// ID3D12CommandQueue - the submission-fence tracker's hook.  It lived next to
// its detour with the derivation written out in a comment ("IUnknown(3) +
// ID3D12Object(4) + GetDevice + UpdateTileMappings + CopyTileMappings -> 10"),
// which is the method that produced v6.7.4's wrong number.  This one happened
// to be right; a slot number that is only as good as its luck belongs here,
// where the compiler checks it.
constexpr std::uint32_t kQueueSlotExecuteCommandLists = 10;

// Every slot the shadow detours.  Install/validate/unhook iterate THIS list,
// never a slot-index-sized array (sparse by construction).  kCmdSlotRelease
// is deliberately absent: it is the hottest COM method on these objects.
// kCmdSlotSetComputeRoot32BitConstant was absent from v6audit3 (2026-09-16,
// the KCD2 first-frame freeze the bisect later pinned on the typed-SRV
// exposure probe) to 7.0.0: a host constant set through the singular setter
// was invisible to the shadow, so the restore replayed the stale plural value
// (PLAN_REHAB_V7 section 8 item 5).  The feared foreign-DWORD replay - the
// NGX runtime's own constants recorded into the host's shadow - has been
// closed since v6.8.0-alpha9 by NgxRuntimeCommandScope; the e2e
// `singular_constant` profile holds the restore.
// ExecuteBundle joined this list in v6.8.0-alpha1.  A bundle's bindings
// persist into the parent list (MS, "Creating and Recording Command Lists and
// Bundles"), so a shadow that cannot see them restores stale state over the
// host's live state after an injection.  Measured on the trunk with the T2
// `bundles` profile: NR engaged and 119 of 120 frames ran the host's next
// dispatch under the previous pass's pipeline (PLAN_REHAB_V7.md 6.2).
// ExecuteIndirect joined it in v6.8.0-alpha8, for the same reason with the
// same kind of evidence: the T2 `gpu_driven_indirect` profile showed the
// trunk restoring a stale root constant over the zero ExecuteIndirect had
// left, on 239 of 240 frames.  It applies the exact documented aftermath and
// nothing more - a graphics signature and a dispatch-only compute signature
// change nothing at all - because the BLANKET version of this rule is what
// made Alan Wake 2 decline 912/912 evaluates on v6.7.0.  See
// indirect_signature.hpp for the quoted rule and both measurements.
inline constexpr std::uint32_t kCmdHookSlots[] = {
    kCmdSlotReset,
    kCmdSlotClearState,
    kCmdSlotSetPipelineState,
    kCmdSlotExecuteBundle,
    kCmdSlotSetDescriptorHeaps,
    kCmdSlotSetComputeRootSignature,
    kCmdSlotSetComputeRootDescriptorTable,
    kCmdSlotSetComputeRoot32BitConstant,
    kCmdSlotSetComputeRoot32BitConstants,
    kCmdSlotSetComputeRootConstantBufferView,
    kCmdSlotSetComputeRootShaderResourceView,
    kCmdSlotSetComputeRootUnorderedAccessView,
    kCmdSlotExecuteIndirect,
};

// Highest slot the installer has to address, derived from the list above so
// the two can never disagree.  Adding a slot to kCmdHookSlots and forgetting
// to resolve it left the installer validating a null entry and declining the
// whole shadow - NR off in every title, with the honest-but-useless message
// "command list vtable slots unresolved" (hit and caught by T2 while adding
// ExecuteBundle, 2026-09-20).  The installer now fills `targets` by iterating
// this same list.
inline constexpr std::uint32_t kCmdSlotMax = [] {
  std::uint32_t highest = 0;
  for (const std::uint32_t slot : kCmdHookSlots) {
    if (slot > highest) highest = slot;
  }
  return highest;
}();

// ---------------------------------------------------------------------------
// Did each detour ever run?
// ---------------------------------------------------------------------------
//
// `test/dlss5/vtable_slots.c` proves every number above against the SDK's own
// vtable layout, and the installer proves the detour was accepted.  Neither
// proves the function was ENTERED, and those are different claims: a title
// that drives D3D12 through its own command-list implementation, or that
// records the pass this addon cares about somewhere the detour does not sit,
// produces a log identical to a title that simply never got there.  That is
// the unsettled half of the Forza Horizon 6 field session (PLAN_REHAB_V7.md
// 10.1, G3): "my detours were never called" and "the calls went somewhere I
// did not detour" were byte-identical in a log.
//
// One flag per slot, reported once on the telemetry line.  Not a per-call
// counter: these are the hottest functions in a frame, and the question is
// "ever", not "how often".  The load-before-store keeps the line from being
// invalidated on every call after the first, so the steady-state cost is a
// relaxed load from a shared, clean cache line.
inline std::atomic_bool cmd_hook_entered[kCmdSlotMax + 1];

inline void NoteCmdHookEntered(std::uint32_t slot) {
  if (slot > kCmdSlotMax) return;
  if (!cmd_hook_entered[slot].load(std::memory_order_relaxed)) {
    cmd_hook_entered[slot].store(true, std::memory_order_relaxed);
  }
}

// Short field tags, one per entry of kCmdHookSlots and in the same order.
// They appear in telemetry users paste, so they are bare identifiers.
inline constexpr const char* kCmdHookTags[] = {
    "reset", "clearstate", "pso", "bundle", "heaps", "rootsig",
    "table", "const1", "consts", "cbv", "srv", "uav", "indirect",
};
static_assert(
    sizeof(kCmdHookTags) / sizeof(kCmdHookTags[0])
        == sizeof(kCmdHookSlots) / sizeof(kCmdHookSlots[0]),
    "every hooked slot needs a tag, in the same order: a tag table out of "
    "step with the slot list mislabels every entry after the gap");

// "9/12", plus the tags that never ran when any are missing.  The key is
// cmd_hooks= at the emit site: a restore-target line already spells
// hooks=0/1 for "are this list's hooks live", and one log must not
// use one key for two questions.
inline std::string CmdHookEntryReport() {
  constexpr std::size_t kCount =
      sizeof(kCmdHookSlots) / sizeof(kCmdHookSlots[0]);
  std::size_t entered = 0;
  std::string missing;
  for (std::size_t i = 0; i < kCount; ++i) {
    if (cmd_hook_entered[kCmdHookSlots[i]].load(std::memory_order_relaxed)) {
      ++entered;
    } else {
      if (!missing.empty()) missing += ',';
      missing += kCmdHookTags[i];
    }
  }
  return std::to_string(entered) + "/" + std::to_string(kCount)
      + (missing.empty() ? std::string()
                         : (" cmd_hooks_missing=" + missing));
}

}  // namespace renodx::addons::dlss5
