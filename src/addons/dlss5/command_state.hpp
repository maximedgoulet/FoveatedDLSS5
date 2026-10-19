/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Command-list compute-state observation and exact restoration
// (audit issues 02/03/04, 2026-09-16).
//
// Three cooperating pieces, all keyed by ID3D12GraphicsCommandList*:
//
//   ComputeStateFields - the observed last-write-wins compute state with
//     per-aspect KNOWLEDGE bits.  D3D12 has no state getters, so "never
//     observed" must stay distinguishable from "observed as none": the
//     ClearState aftermath (no heaps, no root signature) is exact, known
//     state, while a never-bound aspect is unknown and is never replayed
//     (replaying an unobserved value would bind garbage, not host state).
//
//   The capture window (thread-local CapturedComputeState) - what the host
//     bound during a real NGX evaluate, seeded from the persistent shadow so
//     bindings made BEFORE the window still restore (issue 03: capture
//     windows cannot recover inherited bindings on their own).
//
//   The persistent per-list shadow - host state as of the last observation,
//     striped by pointer hash with try-only locking.  Entries pin their COM
//     references so a replay can never dereference a destroyed object, and a
//     stripe that loses an observation (lock contention) drops its entries:
//     they can no longer be trusted, because the missed call may have been
//     the Reset/ClearState/destroy that invalidated them (issue 04: a parked
//     stripe must not revive stale entries when its cooldown expires).
//
// This header must stay free of addon dependencies (no reshade, no Detours,
// no logging globals): test/dlss5_gpu includes it directly to run sentinel
// restoration cases against a real device with the debug layer.  The addon
// wires its park logger through SetShadowStripeParkedLogger at init.
//
// Restore order follows the root-signature contract: heaps, then the compute
// root signature, then root arguments (only when the signature is known -
// argument indices belong to a signature layout), then the PSO.
//
// Injection is gated on ComputeStateFields::RestoreTargetComplete().  NGX
// evaluates bind nothing but descriptor heaps through the command-list
// vtable (DLSS, DLSS-RR and DLSSNR run their compute as NVAPI cubin
// launches; measured 2026-09-18), so a capture window alone never learns
// the host's signature, arguments or PSO: they come only from the persistent
// shadow, which is complete once the list's Reset or ClearState has been
// observed.  The first frame after the hooks install mid-recording
// therefore declines.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include <d3d12.h>
#include <windows.h>

#include "../../utils/directx.hpp"
#include "indirect_signature.hpp"
#include "module_guid.hpp"
#include "root_arguments.hpp"

namespace renodx::addons::dlss5 {

// ---------------------------------------------------------------------------
// Observed compute state
// ---------------------------------------------------------------------------

// At most one CBV/SRV/UAV heap and one sampler heap can be set at a time
// (MS, ID3D12GraphicsCommandList::SetDescriptorHeaps).
constexpr UINT kMaxDescriptorHeaps = 2;

// Order-insensitive: an engine that re-sets the same two heaps with the
// sampler heap first has not changed the set, and "redundantly setting the
// same descriptor heap does not cause descriptor table settings to be
// undefined" (MS, D3D12 Resource Binding).
inline bool SameDescriptorHeapSet(
    ID3D12DescriptorHeap* const* a, UINT a_count,
    ID3D12DescriptorHeap* const* b, UINT b_count) {
  if (a_count != b_count) return false;
  for (UINT i = 0; i < a_count; ++i) {
    bool found = false;
    for (UINT j = 0; j < b_count && !found; ++j) found = a[i] == b[j];
    if (!found) return false;
  }
  return true;
}

struct ComputeStateFields {
  // Knowledge bits: false = never observed in this capture/shadow lifetime,
  // and therefore not restored.  true = observed (ClearState and Reset
  // observe the "none" baseline for every aspect).
  bool heaps_known = false;
  bool root_signature_known = false;
  // The PSO slot is one binding shared by graphics and compute.  A known
  // null (Reset/ClearState with a null argument) is exact state: D3D12
  // requires the host to set a PSO before its next draw or dispatch.
  bool pipeline_state_known = false;
  // The root-argument SET is known: every argument bound under the current
  // signature was observed, so kNone means unbound, not unobserved.  Only a
  // Reset/ClearState baseline or an observed change away from a KNOWN
  // signature proves it.  First sight of a signature cannot: an identical
  // rebind keeps arguments bound before observation began, and bindless
  // engines bind their global tables once per list, then re-set the
  // signature every pass.
  bool root_arguments_known = false;
  UINT heap_count = 0;
  ID3D12DescriptorHeap* heaps[4] = {};
  ID3D12RootSignature* compute_root_signature = nullptr;
  ID3D12PipelineState* pipeline_state = nullptr;
  // Per-parameter "observed" knowledge lives in RootArgumentState::kind
  // (kNone = never bound within this state's scope).
  RootArgumentState root_arguments[kMaxRootParameters];

  // What THIS recording set, as opposed to what it started from.
  //
  // "Bundles inherit all state from the parent command list on which
  // ExecuteBundle is called, except the pipeline state object and primitive
  // topology.  All of the state that is set in a bundle will affect the
  // state of the parent command list" (MS,
  // ID3D12GraphicsCommandList::ExecuteBundle).  So a bundle's shadow is an
  // ABSOLUTE state and is NOT its effect on the parent: the Reset baseline a
  // bundle starts from reads "nothing bound" for every aspect it merely
  // inherited, and merging that wholesale into the parent would unbind the
  // host's state in the model - a second corruption with the opposite sign
  // of the one OnExecuteBundle exists to stop.  These bits are the delta:
  // exactly the aspects ExecuteBundle may carry into the parent.
  //
  // The PSO needs no bit.  It is one of the two things a bundle does NOT
  // inherit, so the bundle's absolute PSO (its Reset/creation argument,
  // updated by any SetPipelineState it records) is precisely what the parent
  // is left with.
  bool touched_heaps = false;
  bool touched_root_signature = false;
  uint64_t touched_root_arguments = 0;  // bit i = parameter i set here

  void ForgetTouched() {
    touched_heaps = false;
    touched_root_signature = false;
    touched_root_arguments = 0;
  }

  // Injection overwrites the heaps, the compute signature, its arguments and
  // the PSO; a restore can only put back what was observed.  Every injection
  // path requires this (v6.3.1): the v6.0.0-v6.3.0 heaps-only gate injected
  // into a list whose hooks had just installed mid-recording, under an NGX
  // evaluate that binds nothing but heaps, and RE Requiem ran its next
  // dispatch against the codec's signature, PSO and arguments.
  bool RestoreTargetComplete() const noexcept {
    return heaps_known && root_signature_known && root_arguments_known
        && pipeline_state_known;
  }

  // ClearState unbinds resources, descriptor heaps, root arguments, and the
  // root signature deterministically, leaving the pipeline state argument.
  // The aftermath is written as KNOWN baseline, not as "unknown".
  void OnClearState(ID3D12PipelineState* reset_pipeline_state) {
    heaps_known = true;
    heap_count = 0;
    std::memset(heaps, 0, sizeof(heaps));
    root_signature_known = true;
    compute_root_signature = nullptr;
    pipeline_state_known = true;
    pipeline_state = reset_pipeline_state;
    root_arguments_known = true;
    // A Reset (which writes this same baseline) starts a new recording, so
    // nothing is "set by this recording" any more.  A bundle that is reset
    // and closed without binding anything must leave its parent alone.
    ForgetTouched();
    for (RootArgumentState& argument : root_arguments) {
      argument.Reset();
    }
  }

  void OnSetPipelineState(ID3D12PipelineState* new_pipeline_state) {
    pipeline_state_known = true;
    pipeline_state = new_pipeline_state;
  }

  // ExecuteBundle with the bundle's recording UNOBSERVED (it was recorded
  // before the hooks installed, or its entry was evicted).  A bundle
  // inherits the parent's compute state, may change any of it, and every
  // change it makes persists into the parent afterwards (MS, ExecuteBundle),
  // so an unobserved bundle leaves the four aspects an injection must
  // restore no longer known.  Dropping the knowledge - not the values -
  // keeps the next host re-bind able to complete the target again without a
  // Reset.  When the recording WAS observed, MergeExecutedBundle applies its
  // delta instead and the target stays complete.
  void OnExecuteBundle() {
    heaps_known = false;
    root_signature_known = false;
    pipeline_state_known = false;
    root_arguments_known = false;
  }

  // ExecuteBundle with the bundle's recording observed: apply exactly what
  // the bundle set.  Defined below BundleDelta.
  void MergeExecutedBundle(const struct BundleDelta& delta);

  // The documented aftermath of ExecuteIndirect, applied exactly.  "No
  // command signature state leaks back to the command list" (MS, Indirect
  // Drawing), so nothing outside the arguments the signature NAMES changes,
  // and a graphics signature cannot touch compute state at all.  For the
  // arguments it does name: a root constant reads 0 afterwards, a root view
  // is a NULL view.
  //
  // Both halves are load-bearing and each has a build that got them wrong.
  // Resetting more than this is what made Alan Wake 2 decline 912/912
  // evaluates on v6.7.0 (a GPU-driven engine's indirects sit between its
  // last bind and the evaluate, so blanket invalidation never lets the
  // restore target complete).  Resetting less than this - v6.5.3, which does
  // not watch the call - leaves the shadow holding the pre-call constant,
  // and the restore then writes that stale value over the zero D3D12 left,
  // corrupting the host's next dispatch on 239 of 240 frames.  Both measured
  // on the T2 `gpu_driven_indirect` profile, 2026-09-20.
  //
  // The caller must have checked `effect.complete`; an incomplete
  // description is counted and ignored, never half-applied.
  void OnExecuteIndirect(const struct IndirectSignatureEffect& effect);

  // "Only one descriptor heap of each type can be set at one time, which
  // means a maximum of 2 heaps (one sampler, one CBV/SRV/UAV) can be set at
  // one time" (MS, ID3D12GraphicsCommandList::SetDescriptorHeaps).  A larger
  // set is off-contract: it is counted and its aftermath applied, but it is
  // not recorded as state and it does not decline the frame (see below).
  //
  // "Descriptor table state is undefined at the beginning of a command list
  // and after descriptor heaps are changed on a command list", and
  // "Redundantly setting the same descriptor heap does not cause descriptor
  // table settings to be undefined" (MS, D3D12 Resource Binding).  So a
  // CHANGED set takes the table slots with it, and replaying one afterwards
  // is illegal - rejected at SET time, not at dispatch.  Measured twice on
  // this hardware with the debug layer: the v6.7.2 audit (2026-09-19) and
  // the T2 rebind_all validation lane (2026-09-20), both
  // "CGraphicsCommandList::SetComputeRootDescriptorTable: The descriptor
  // heap ... containing handle ... is different from currently set
  // descriptor heap".  v6.5.2 - the last field-proven build - replayed them,
  // so it was emitting that error in the field on every engaged frame.
  //
  // `root_arguments_known` STAYS true.  "Undefined" is the HOST's
  // obligation - it must re-set its tables before its next dispatch whatever
  // we do - not a gap in our knowledge: the aftermath is fully known, since
  // the spec attaches nothing to root constants or root CBV/SRV/UAV, which
  // survive.  Written as a known aftermath, exactly as the Reset/ClearState
  // baseline is.  v6.7.1 (671-02) dropped the knowledge bit here as well and
  // made the injection gate unsatisfiable in the field - AW2 and KCD2 on
  // v6.7.2 declined 100% of evaluates (0 NR evals) against KCD2 on v6.5.2 at
  // 517 - because the NGX evaluate binds its OWN heap and that landed in
  // every capture window microseconds before the gate read it.  Since
  // v6.8.0-alpha9 the runtime's own binds are not observed at all
  // (NgxRuntimeCommandScope), so what reaches this function is the host's.
  //
  // Returns whether a table argument was dropped (telemetry only).
  bool OnSetDescriptorHeaps(
      UINT num_descriptor_heaps, ID3D12DescriptorHeap* const* descriptor_heaps) {
    if (num_descriptor_heaps > kMaxDescriptorHeaps) {
      // Off-contract.  The runtime rejects the call, so the set that is
      // actually bound is NOT the one we were handed and recording it would
      // be a guess; the recorded set is left exactly as it was.  The tables
      // go, because a heap change may still have happened and an undefined
      // table must never be replayed.  Counted, not gated: this is
      // unreachable in a conforming engine, and PLAN_REHAB_V7.md 8 keeps
      // "heaps > 2 -> heaps unknown" observe-only.  (The v6.5.3 trunk
      // recorded heap_count = 0 here - it would have restored the EMPTY set
      // over the host's heaps.)
      return DropTablesForHeapChange();
    }
    const bool same_set = heaps_known
        && SameDescriptorHeapSet(
               heaps, heap_count, descriptor_heaps, num_descriptor_heaps);
    heaps_known = true;
    touched_heaps = true;
    heap_count = num_descriptor_heaps;
    for (UINT i = 0; i < heap_count; ++i) heaps[i] = descriptor_heaps[i];
    for (UINT i = heap_count; i < 4; ++i) heaps[i] = nullptr;
    if (same_set) return false;
    return DropTablesForHeapChange();
  }

  // The table half of that aftermath, shared with the persistent shadow's
  // own recorder (which writes heaps itself, because it pins them).  The
  // touched bit goes with the drop: a bundle that changes heaps leaves its
  // parent's tables undefined too, so the delta must carry the drop.
  bool DropTablesForHeapChange() {
    bool dropped_table = false;
    for (unsigned index = 0; index < kMaxRootParameters; ++index) {
      RootArgumentState& argument = root_arguments[index];
      if (argument.kind != RootArgumentKind::kTable) continue;
      argument.Reset();
      touched_root_arguments |= 1ull << index;
      dropped_table = true;
    }
    return dropped_table;
  }

  // An IDENTICAL root-signature rebind preserves root arguments in D3D12
  // ("Using a Root Signature"); only a layout change invalidates them.
  void OnSetComputeRootSignature(ID3D12RootSignature* new_root_signature) {
    touched_root_signature = true;
    if (root_signature_known && compute_root_signature == new_root_signature) {
      return;
    }
    // A change away from a known signature leaves the set known-empty;
    // first sight cannot tell a change from an identical rebind.
    root_arguments_known = root_signature_known;
    root_signature_known = true;
    compute_root_signature = new_root_signature;
    for (RootArgumentState& argument : root_arguments) {
      argument.Reset();
    }
  }

  void OnSetComputeRootDescriptorTable(UINT root_parameter_index, uint64_t base_descriptor) {
    if (root_parameter_index >= kMaxRootParameters) return;
    root_arguments[root_parameter_index].SetView(
        RootArgumentKind::kTable, base_descriptor);
    touched_root_arguments |= 1ull << root_parameter_index;
  }

  void OnSetComputeRoot32BitConstants(
      UINT root_parameter_index,
      UINT num_32bit_values_to_set,
      const void* src_data,
      UINT dest_offset) {
    if (root_parameter_index >= kMaxRootParameters || src_data == nullptr) return;
    root_arguments[root_parameter_index].MergeConstants(
        dest_offset,
        num_32bit_values_to_set,
        static_cast<const uint32_t*>(src_data));
    touched_root_arguments |= 1ull << root_parameter_index;
  }

  void OnSetComputeRootConstantBufferView(
      UINT root_parameter_index, uint64_t buffer_location) {
    OnRootView(root_parameter_index, RootArgumentKind::kConstantBufferView, buffer_location);
  }

  void OnSetComputeRootShaderResourceView(
      UINT root_parameter_index, uint64_t buffer_location) {
    OnRootView(root_parameter_index, RootArgumentKind::kShaderResourceView, buffer_location);
  }

  void OnSetComputeRootUnorderedAccessView(
      UINT root_parameter_index, uint64_t buffer_location) {
    OnRootView(root_parameter_index, RootArgumentKind::kUnorderedAccessView, buffer_location);
  }

 private:
  void OnRootView(
      UINT root_parameter_index, RootArgumentKind kind, uint64_t address) {
    if (root_parameter_index >= kMaxRootParameters) return;
    root_arguments[root_parameter_index].SetView(kind, address);
    touched_root_arguments |= 1ull << root_parameter_index;
  }
};

// ---------------------------------------------------------------------------
// Executed-bundle delta
// ---------------------------------------------------------------------------

// A snapshot of one bundle recording, reduced to what ExecuteBundle carries
// into the parent command list.  Taken from the bundle's own shadow - a
// bundle records through the same detoured function bodies as a direct list,
// so its bindings are observed the same way.
//
// `observed` false means the bundle's recording was never seen (it was
// recorded before the hooks installed, or its entry was evicted); the caller
// must then fall back to ComputeStateFields::OnExecuteBundle, which is the
// safe reading.
//
// The COM pins are AddRef'd by the reader and OWNED by this snapshot: a
// successful merge hands them to the parent's shadow entry, and a merge that
// does not happen releases them.  Only the members the flags mark valid
// carry a reference.
struct BundleDelta {
  bool observed = false;
  bool touched_heaps = false;
  bool touched_root_signature = false;
  bool pipeline_state_known = false;
  uint64_t touched_root_arguments = 0;
  // The pins below are this snapshot's to give away or give back.  Cleared
  // once they are handed to the parent's entry or released; the VALUES stay,
  // so a capture window can still copy them (a capture pins nothing).
  bool pins_held = false;
  UINT heap_count = 0;
  ID3D12DescriptorHeap* heaps[4] = {};
  ID3D12RootSignature* compute_root_signature = nullptr;
  ID3D12PipelineState* pipeline_state = nullptr;
  // Only the parameters set in `touched_root_arguments` hold a valid value;
  // the rest are untouched storage, never read.
  RootArgumentState root_arguments[kMaxRootParameters];

  void Reset() {
    observed = false;
    touched_heaps = false;
    touched_root_signature = false;
    pipeline_state_known = false;
    touched_root_arguments = 0;
    pins_held = false;
    heap_count = 0;
    std::memset(heaps, 0, sizeof(heaps));
    compute_root_signature = nullptr;
    pipeline_state = nullptr;
  }
};

// Applies a bundle's delta.  Ordering follows the root-signature contract:
// heaps, then the signature (whose change semantics decide what happens to
// the arguments), then the arguments the bundle bound under it, then the
// pipeline state.
//
// The caller owns the COM pins: on return this state holds the pointers the
// flags marked valid, and the caller has moved the values they replaced into
// its own release list.
inline void ComputeStateFields::MergeExecutedBundle(const BundleDelta& delta) {
  if (delta.touched_heaps) {
    heaps_known = true;
    heap_count = delta.heap_count;
    std::memcpy(heaps, delta.heaps, sizeof(heaps));
  }
  if (delta.touched_root_signature) {
    // Reuse the audited semantics: an identical rebind preserves the
    // arguments, a change away from a KNOWN signature leaves the set
    // known-empty, and a change from an unknown one leaves it unknown.
    OnSetComputeRootSignature(delta.compute_root_signature);
  }
  if (delta.touched_root_arguments != 0) {
    for (unsigned index = 0; index < kMaxRootParameters; ++index) {
      if ((delta.touched_root_arguments & (1ull << index)) == 0) continue;
      root_arguments[index] = delta.root_arguments[index];
    }
  }
  // The PSO is not inherited by the bundle, so whatever the bundle ended on
  // is what the parent is left with - including the PSO its Reset/creation
  // named.  An observed bundle therefore always resolves the parent's PSO;
  // only a bundle whose birth we missed leaves it unknown.
  pipeline_state_known = delta.pipeline_state_known;
  if (delta.pipeline_state_known) pipeline_state = delta.pipeline_state;
  // What the bundle set, this recording has now set too.  The runtime drops
  // an ExecuteBundle recorded into a bundle, so nothing reads these on a
  // parent today; keeping them true costs one OR and stops the model from
  // lying if that ever changes.
  touched_heaps = touched_heaps || delta.touched_heaps;
  touched_root_signature = touched_root_signature || delta.touched_root_signature;
  touched_root_arguments |= delta.touched_root_arguments;
}

inline void ComputeStateFields::OnExecuteIndirect(
    const IndirectSignatureEffect& effect) {
  if (!effect.complete
      || effect.signature_class != IndirectSignatureEffect::Class::kCompute) {
    return;
  }
  for (unsigned i = 0; i < effect.named_count; ++i) {
    const NamedIndirectArgument& named = effect.named[i];
    if (named.root_parameter_index >= kMaxRootParameters) continue;
    RootArgumentState& argument = root_arguments[named.root_parameter_index];
    if (named.constant_count != 0) {
      // KNOWN(0) - the restore can and must write this back, because a host
      // that relies on the documented zero would otherwise see our stale
      // observation.
      const uint32_t zeros[kMaxRootConstantDwords] = {};
      argument.MergeConstants(named.constant_offset, named.constant_count, zeros);
    } else {
      // A NULL root view.  Forgetting the slot is how this model says "do
      // not replay it": the restore skips an unobserved slot, which leaves
      // the NULL D3D12 put there, and that IS the host's state.  Binding
      // address 0 back explicitly would be the same value through a call the
      // debug layer has opinions about, for no gain.
      argument.Reset();
    }
    // What this recording has changed.  A bundle carries its net delta into
    // the parent, and an indirect recorded inside one changes the same
    // slots any direct call would.
    touched_root_arguments |= 1ull << named.root_parameter_index;
  }
}

// ---------------------------------------------------------------------------
// Capture windows
// ---------------------------------------------------------------------------

struct CapturedComputeState : ComputeStateFields {
  // Identity of the list this window restores.  nullptr (the Streamline
  // fallback, which only receives its list after the real evaluate) means
  // "record every call on this thread" - the pre-v5.3 behavior, kept only
  // for that path.
  ID3D12GraphicsCommandList* command_list = nullptr;
  // Diagnostics only (log wording): a host ClearState was observed during
  // this window.  Restoration itself is driven by the knowledge bits.
  bool observed_clear_state = false;
  // Capture windows nest: a plugin NGX module forwarding into the detoured
  // core export re-enters the hooks on the same thread, and the inner window
  // must resume the outer one on close (the outer window must stay armed so
  // the inner restore is recorded into it - see EndComputeStateCapture).
  CapturedComputeState* outer_capture = nullptr;
};

// Points at the owning envelope's storage while its capture window is open.
inline thread_local CapturedComputeState* compute_state_capture = nullptr;

inline void BeginComputeStateCapture(
    CapturedComputeState* storage,
    ID3D12GraphicsCommandList* command_list = nullptr);

inline void EndComputeStateCapture(CapturedComputeState* storage) {
  compute_state_capture = storage->outer_capture;
}

// ---------------------------------------------------------------------------
// Persistent per-list shadow
// ---------------------------------------------------------------------------

struct ListComputeShadow : ComputeStateFields {};

// The addon (or a test) wires its logger here; null means silent parking.
inline void (*OnShadowStripeParked)(const char* site, unsigned cooldown_ms) = nullptr;

inline constexpr uint32_t kListShadowStripeCount = 16;
// Cooldown in steady-clock milliseconds (0 = enabled).  Parking is FAIL-OPEN
// with data loss: while parked, the stripe observes nothing, and on re-arm
// its entries are dropped rather than trusted (a missed Reset/ClearState/
// destroy during the park window would otherwise revive stale state).
inline constexpr uint64_t kListShadowCooldownMs = 5000;

// The COM pins of one shadow entry, without the ~18 KB root-argument table
// (64 parameters x 64 constant DWORDs).  Every hooked host call carries one
// of these for the references it replaces; value-initializing a whole
// ListComputeShadow per call zeroed that table on every SetPipelineState and
// compute root call the game makes.
struct ShadowPins {
  ID3D12DescriptorHeap* heaps[4] = {};
  ID3D12RootSignature* compute_root_signature = nullptr;
  ID3D12PipelineState* pipeline_state = nullptr;
};

// An entry lives behind a pointer so the per-call lookup scans a dense
// (key, pointer) array instead of striding over the argument tables.
struct ListShadowEntry {
  ID3D12GraphicsCommandList* list = nullptr;
  // list_shadow_tick value of the last host observation (idle eviction).
  uint32_t last_tick = 0;
  std::unique_ptr<ListComputeShadow> shadow;
};

// Coarse wall clock for idle eviction, advanced by the owner (the addon
// stores elapsed seconds from its lifecycle tick).  A clock that never
// advances disables eviction.
inline std::atomic_uint32_t list_shadow_tick{0};

struct ListShadowStripe {
  SRWLOCK lock{};
  std::vector<ListShadowEntry> shadows;
  // Mirrors shadows.size(); written under the lock, read lock-free by
  // telemetry.
  std::atomic_uint32_t entry_count{0};
  std::atomic_uint64_t parks{0};
  std::atomic_uint64_t idle_evictions{0};
  std::atomic_uint64_t disabled_until_ms{0};
  std::atomic_bool logged_disabled{false};
  std::atomic_bool entries_suspect{false};
  // Whole-entry drains performed while the lock was held; pin releases run
  // from ReleaseParkedDrains after the guard drops (never COM under lock).
  std::vector<std::vector<ListComputeShadow>> parked_drains;

  ListShadowStripe() noexcept { InitializeSRWLock(&lock); }
};

struct ListShadowState {
  ListShadowStripe stripes[kListShadowStripeCount];
};

inline ListShadowState& GetListShadowState() {
  static ListShadowState* const state = new ListShadowState();
  return *state;
}

// Pointer-hash stripe select: command-list pointers are allocator-aligned,
// so spread the shifted bits multiplicatively; a collision only shares a
// lock, never a correctness property.
inline ListShadowStripe& ListShadowStripeFor(
    ListShadowState& state, ID3D12GraphicsCommandList* command_list) {
  const uintptr_t bits = reinterpret_cast<uintptr_t>(command_list);
  const uint32_t hash =
      static_cast<uint32_t>((bits >> 4) * 2654435761ull >> 13);
  return state.stripes[hash & (kListShadowStripeCount - 1)];
}

inline bool ListShadowStripeOff(const ListShadowStripe& stripe) {
  const uint64_t until =
      stripe.disabled_until_ms.load(std::memory_order_acquire);
  if (until == 0) return false;
  const uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
  return now < until;
}

inline void ParkListShadowStripe(ListShadowStripe& stripe, const char* site) {
  const uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
  stripe.disabled_until_ms.store(
      now + kListShadowCooldownMs, std::memory_order_release);
  // Entries observed before this park can no longer be trusted once the
  // stripe re-arms: the very next failed observation proves calls were
  // missed, and any of them may have invalidated these entries.
  stripe.entries_suspect.store(true, std::memory_order_release);
  stripe.parks.fetch_add(1, std::memory_order_relaxed);
  if (OnShadowStripeParked != nullptr
      && !stripe.logged_disabled.exchange(true, std::memory_order_relaxed)) {
    OnShadowStripeParked(site, static_cast<unsigned>(kListShadowCooldownMs));
  }
}

// Bounded RAII around a stripe's SRW lock: empty only when the lock stays
// unavailable past kListShadowWaitMs, or on same-thread recursion (fails fast
// - an exclusive SRW lock is not recursive).  Critical sections here are
// vector mutations (microseconds) and no COM AddRef/Release runs under the
// lock, so a lock that outlasts the spin means its holder was PREEMPTED, not
// that it is slow.  The spin-only guard parked the stripe in that case, and
// a park costs every list in the stripe its restore target for the 5 s
// cooldown: the injection gate then declines those lists while admitting
// lists in other stripes, which is NR flickering on and off per frame (RDR2
// v6.4.1 field log: ~1370 lists over oversubscribed recording threads,
// parks=374 in 7 min, evals ~74% of presents).  After the spin the guard
// yields the core so the holder can finish; parking remains the fail-open
// outcome for a holder that stays away past the bound.
inline constexpr unsigned kListShadowSpinTries = 4000;
inline constexpr uint64_t kListShadowWaitMs = 50;
// Acquisitions that outlasted the spin and succeeded in the yield phase
// (telemetry: the positive control that the wait path ran).
inline std::atomic_uint64_t list_shadow_waits{0};
// Descriptor-table root arguments dropped because the host changed the
// descriptor-heap set under them (the documented aftermath - see
// ComputeStateFields::OnSetDescriptorHeaps).  A title where this climbs is
// one whose tables our restore deliberately does not put back, because
// D3D12 says they are gone; a title where it stays at 0 keeps every table
// it had.  It is the counter to read first if engagement is fine but a
// game still looks wrong after an evaluate.
inline std::atomic_uint64_t list_shadow_heap_table_drops{0};
// SetDescriptorHeaps calls with more heaps than D3D12 accepts.  Unreachable
// in a conforming engine, so a non-zero value is a fact about the title
// worth having before anything else about it is believed.
inline std::atomic_uint64_t list_shadow_heap_sets_illegal{0};
inline thread_local bool list_shadow_lock_held = false;
struct ListShadowTryGuard {
  SRWLOCK* lock = nullptr;

  explicit ListShadowTryGuard(SRWLOCK& candidate) noexcept {
    if (list_shadow_lock_held) return;
    for (unsigned tries = 0; tries < kListShadowSpinTries; ++tries) {
      if (TryAcquireSRWLockExclusive(&candidate) != FALSE) {
        Hold(candidate);
        return;
      }
      YieldProcessor();
    }
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(kListShadowWaitMs);
    for (unsigned yields = 0;; ++yields) {
      // SwitchToThread only yields to a thread ready on this core; the
      // periodic Sleep(1) lets a holder queued elsewhere run as well.
      if ((yields & 7u) == 7u || SwitchToThread() == FALSE) Sleep(1);
      if (TryAcquireSRWLockExclusive(&candidate) != FALSE) {
        Hold(candidate);
        list_shadow_waits.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      if (std::chrono::steady_clock::now() >= deadline) return;
    }
  }
  ListShadowTryGuard(const ListShadowTryGuard&) = delete;
  ListShadowTryGuard& operator=(const ListShadowTryGuard&) = delete;
  ~ListShadowTryGuard() noexcept {
    if (lock != nullptr) {
      list_shadow_lock_held = false;
      ReleaseSRWLockExclusive(lock);
    }
  }
  explicit operator bool() const noexcept { return lock != nullptr; }

 private:
  void Hold(SRWLOCK& candidate) noexcept {
    lock = &candidate;
    list_shadow_lock_held = true;
  }
};

// Releases every COM pin a LOCAL shadow copy owns (drains, replaced fields,
// teardown batches).  Never call while a stripe lock is held.
inline void ReleaseShadowComRefs(ShadowPins& pins) noexcept {
  for (UINT i = 0; i < 4; ++i) {
    if (pins.heaps[i] != nullptr) pins.heaps[i]->Release();
    pins.heaps[i] = nullptr;
  }
  if (pins.compute_root_signature != nullptr) {
    pins.compute_root_signature->Release();
    pins.compute_root_signature = nullptr;
  }
  if (pins.pipeline_state != nullptr) {
    pins.pipeline_state->Release();
    pins.pipeline_state = nullptr;
  }
}

inline void ReleaseShadowComRefs(ListComputeShadow& shadow) noexcept {
  for (UINT i = 0; i < 4; ++i) {
    if (shadow.heaps[i] != nullptr) shadow.heaps[i]->Release();
    shadow.heaps[i] = nullptr;
  }
  if (shadow.compute_root_signature != nullptr) {
    shadow.compute_root_signature->Release();
    shadow.compute_root_signature = nullptr;
  }
  if (shadow.pipeline_state != nullptr) {
    shadow.pipeline_state->Release();
    shadow.pipeline_state = nullptr;
  }
  shadow.heap_count = 0;
  shadow.heaps_known = false;
  shadow.root_signature_known = false;
  shadow.pipeline_state_known = false;
  shadow.root_arguments_known = false;
}

// ---------------------------------------------------------------------------
// Object-attached list state (ListStateMode 1, v6.5.0)
// ---------------------------------------------------------------------------
//
// The striped table above is a shared container, and everything fragile about
// it follows from that: a lock every recording thread in the process crosses,
// a fail-open park that costs a whole stripe its restore targets (the RDR2
// on/off flicker), idle eviction because destruction is unobserved, and a
// recycled list address inheriting a dead list's entry (S12).  None of it is
// inherent to the problem.  A D3D12 command list is not free-threaded - one
// thread records into it at a time - so state that lives ON the list needs
// no lock, and state D3D12 owns with the list needs no eviction and cannot
// be inherited.  ID3D12Object::SetPrivateDataInterface gives exactly that:
// D3D12 holds the only long-lived reference and releases it when the list
// dies.  Proxies (ReShade, Streamline) forward private data to the native
// object, so proxy and native pointers reach the same state.
//
// Measured 2026-09-18 (8 recording threads): lookup + Release ~41 ns per
// hooked call, debug layer on or off.
//
// The mode is session-constant: set it before the first observation.  The
// table stays selectable as mode 0 (generic-mod policy), the per-game
// fallback should a title's list implementation refuse private data
// (`attach_failed` in telemetry).

inline constexpr int kListStateStripedTable = 0;
inline constexpr int kListStateObjectAttached = 1;
inline std::atomic_int list_state_mode{kListStateObjectAttached};

// {B7C5E0A2-3F41-4D6B-9E57-64D1A90C52F3}, per loaded copy (module_guid.hpp:
// two copies sharing it freed each other's objects, ENV-03).
inline const GUID kListStateGuid = ModuleScopedGuid(
    {0xb7c5e0a2, 0x3f41, 0x4d6b, {0x9e, 0x57, 0x64, 0xd1, 0xa9, 0x0c, 0x52, 0xf3}});

struct ListStateObject;

// Live objects, for teardown and telemetry only: touched when an object is
// created or destroyed (list lifetime events), never by a hooked call.
struct ListStateRegistry {
  SRWLOCK lock{};
  ListStateObject* head = nullptr;
  std::atomic_uint32_t live{0};
  // SetPrivateDataInterface refused the object or allocation failed: the
  // observation is lost (telemetry; expected 0).
  std::atomic_uint64_t attach_failures{0};

  ListStateRegistry() noexcept { InitializeSRWLock(&lock); }
};

inline ListStateRegistry& GetListStateRegistry() {
  static ListStateRegistry* const registry = new ListStateRegistry();
  return *registry;
}

struct ListStateObject final : IUnknown {
  std::atomic<ULONG> refs{1};
  // Not owned: the NATIVE list, never a ReShade proxy.  D3D12 keeps the
  // private data on the native object (a proxy forwards it there), so the
  // native is what holds this object's reference and what is alive whenever
  // that reference is; the proxy that attached the state can be deleted
  // first (dlss5_gpu sentinel object_attached, block c2).
  ID3D12GraphicsCommandList* list = nullptr;
  ListStateObject* previous = nullptr;
  ListStateObject* next = nullptr;
  ListComputeShadow shadow;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) return E_POINTER;
    if (riid == __uuidof(IUnknown)) {
      *object = static_cast<IUnknown*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return refs.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  // The final Release runs inside the list's destruction (or the teardown
  // detach): unlink, drop the pins, free.  The registry lock covers only the
  // unlink; pin releases are COM calls and run after it.
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left != 0) return left;
    ListStateRegistry& registry = GetListStateRegistry();
    AcquireSRWLockExclusive(&registry.lock);
    if (previous != nullptr) previous->next = next;
    if (next != nullptr) next->previous = previous;
    if (registry.head == this) registry.head = next;
    ReleaseSRWLockExclusive(&registry.lock);
    registry.live.fetch_sub(1, std::memory_order_relaxed);
    ReleaseShadowComRefs(shadow);
    delete this;
    return 0;
  }

 private:
  ~ListStateObject() = default;
};

// Returns the list's state with a reference the caller Releases, or nullptr
// (absent and !create, or the attach failed - counted).
inline ListStateObject* AcquireListState(
    ID3D12GraphicsCommandList* command_list, bool create) {
  ListStateObject* object = nullptr;
  UINT size = sizeof(object);
  if (SUCCEEDED(command_list->GetPrivateData(kListStateGuid, &size, &object))
      && object != nullptr) {
    return object;
  }
  if (!create) return nullptr;
  ListStateRegistry& registry = GetListStateRegistry();
  object = new (std::nothrow) ListStateObject();
  if (object == nullptr) {
    registry.attach_failures.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  object->list = command_list;
  renodx::utils::directx::NativeFromReShadeProxy(&object->list);
  AcquireSRWLockExclusive(&registry.lock);
  object->next = registry.head;
  if (registry.head != nullptr) registry.head->previous = object;
  registry.head = object;
  ReleaseSRWLockExclusive(&registry.lock);
  registry.live.fetch_add(1, std::memory_order_relaxed);
  if (FAILED(command_list->SetPrivateDataInterface(kListStateGuid, object))) {
    registry.attach_failures.fetch_add(1, std::memory_order_relaxed);
    object->Release();
    return nullptr;
  }
  return object;
}

// Teardown: D3D12 holds a pointer into this module's vtable for every live
// object, so each one is detached from its list before the module can
// unload.  A reference is taken only on objects whose count is still
// non-zero (one at zero is inside its final Release, waiting for the lock);
// a count above ours afterwards means D3D12 - and so the list - is alive.
inline void DetachListStateObjects() {
  ListStateRegistry& registry = GetListStateRegistry();
  std::vector<ListStateObject*> live;
  AcquireSRWLockExclusive(&registry.lock);
  try {
    for (ListStateObject* object = registry.head; object != nullptr;
         object = object->next) {
      ULONG count = object->refs.load(std::memory_order_acquire);
      while (count != 0
             && !object->refs.compare_exchange_weak(
                 count, count + 1, std::memory_order_acq_rel)) {
      }
      if (count != 0) live.push_back(object);
    }
  } catch (...) {
  }
  ReleaseSRWLockExclusive(&registry.lock);
  for (ListStateObject* object : live) {
    if (object->refs.load(std::memory_order_acquire) > 1) {
      object->list->SetPrivateDataInterface(kListStateGuid, nullptr);
    }
    object->Release();
  }
}

// Finds a shadow entry without creating one.  Returns nullptr when absent,
// the stripe is parked, or the lock is unavailable.
inline ListComputeShadow* FindListShadowLocked(
    ListShadowStripe& stripe, ID3D12GraphicsCommandList* command_list) {
  for (ListShadowEntry& entry : stripe.shadows) {
    if (entry.list == command_list) {
      entry.last_tick = list_shadow_tick.load(std::memory_order_relaxed);
      return entry.shadow.get();
    }
  }
  return nullptr;
}

// Releases the COM pins of entries drained under a stripe's lock.  Call only
// after dropping that stripe's guard; takes the parked drains with swap.
inline void ReleaseParkedDrains(ListShadowStripe& stripe) {
  std::vector<std::vector<ListComputeShadow>> drains;
  drains.swap(stripe.parked_drains);
  for (auto& drained : drains) {
    for (auto& shadow : drained) ReleaseShadowComRefs(shadow);
  }
}

namespace internal_shadow {

// Core mutation protocol shared by every writer: park on contention, drop
// suspect entries first, then hand the entry (created on demand when
// `create_entry`) to `body`.  `body` returns false to erase the entry.
// COM refs being replaced must be moved by `body` into `replaced` so their
// Release() runs after the lock; whole dropped entries land in `drained`.
template <typename Body>
inline bool WithListShadow(
    ID3D12GraphicsCommandList* command_list,
    const char* site,
    bool create_entry,
    ShadowPins& replaced,
    std::vector<ListComputeShadow>& drained,
    Body&& body) {
  if (list_state_mode.load(std::memory_order_relaxed)
      == kListStateObjectAttached) {
    ListStateObject* object = AcquireListState(command_list, create_entry);
    if (object == nullptr) return false;
    bool observed = true;
    try {
      if (!body(object->shadow)) {
        // Erase = back to unknown; the pins leave through `drained`.
        drained.push_back(object->shadow);
        // In place, not `= ListComputeShadow{}`: that 18 KB blank sat on
        // this frame, which every list hook enters from inside NGX's
        // evaluate, and the page probe committed it on every call.
        ::new (static_cast<void*>(&object->shadow)) ListComputeShadow{};
      }
    } catch (...) {
      observed = false;
    }
    object->Release();
    return observed;
  }
  ListShadowStripe& stripe =
      ListShadowStripeFor(GetListShadowState(), command_list);
  if (ListShadowStripeOff(stripe)) return false;
  try {
    ListShadowTryGuard guard(stripe.lock);
    if (!guard) {
      ParkListShadowStripe(stripe, site);
      return false;
    }
    if (stripe.entries_suspect.exchange(false, std::memory_order_acq_rel)) {
      for (ListShadowEntry& suspect : stripe.shadows) {
        drained.push_back(std::move(*suspect.shadow));
      }
      stripe.shadows.clear();
      stripe.entry_count.store(0, std::memory_order_relaxed);
    }
    ListComputeShadow* entry = FindListShadowLocked(stripe, command_list);
    if (entry == nullptr && create_entry) {
      ListShadowEntry created;
      created.list = command_list;
      created.last_tick = list_shadow_tick.load(std::memory_order_relaxed);
      created.shadow = std::make_unique<ListComputeShadow>();
      stripe.shadows.push_back(std::move(created));
      stripe.entry_count.store(
          static_cast<uint32_t>(stripe.shadows.size()),
          std::memory_order_relaxed);
      entry = stripe.shadows.back().shadow.get();
    }
    if (entry == nullptr) return false;
    if (!body(*entry)) {
      drained.push_back(std::move(*entry));
      for (size_t i = 0; i < stripe.shadows.size(); ++i) {
        if (stripe.shadows[i].list == command_list) {
          // Order carries no meaning: swap-and-pop instead of shifting.
          if (i + 1 != stripe.shadows.size()) {
            stripe.shadows[i] = std::move(stripe.shadows.back());
          }
          stripe.shadows.pop_back();
          break;
        }
      }
      stripe.entry_count.store(
          static_cast<uint32_t>(stripe.shadows.size()),
          std::memory_order_relaxed);
    }
    return true;
  } catch (...) {
    ParkListShadowStripe(stripe, site);
    return false;
  }
}

}  // namespace internal_shadow

// --- Host-call recorders (called by the hooks after the real call's
// arguments are known; every one is fail-open: a lost observation leaves
// the aspect unknown, never wrong) ---

inline void RecordShadowHeaps(
    ID3D12GraphicsCommandList* command_list,
    UINT num_descriptor_heaps,
    ID3D12DescriptorHeap* const* descriptor_heaps) {
  const bool legal = num_descriptor_heaps <= kMaxDescriptorHeaps;
  const UINT new_count = legal ? num_descriptor_heaps : 0;
  // Same-set rebind fast path (engines re-set the same heaps every pass):
  // pointer comparison only, no pins churned.  Bindless engines share one
  // heap across every list, so its reference count is a contended line.
  // A same set is also the case that must NOT drop the table arguments.
  const auto same_set = [&](const ListComputeShadow* entry) {
    return entry != nullptr && entry->heaps_known && legal
        && SameDescriptorHeapSet(
               entry->heaps, entry->heap_count, descriptor_heaps, new_count);
  };
  if (list_state_mode.load(std::memory_order_relaxed)
      == kListStateObjectAttached) {
    if (ListStateObject* object = AcquireListState(command_list, false)) {
      const bool same = same_set(&object->shadow);
      object->Release();
      if (same) return;
    }
  } else {
    ListShadowStripe& stripe =
        ListShadowStripeFor(GetListShadowState(), command_list);
    if (ListShadowStripeOff(stripe)) return;
    ListShadowTryGuard guard(stripe.lock);
    if (guard && same_set(FindListShadowLocked(stripe, command_list))) return;
  }
  // Pin the incoming heaps BEFORE the lock: virtual COM calls never run
  // under the shadow lock.
  ShadowPins pinned{};
  for (UINT i = 0; i < new_count; ++i) {
    pinned.heaps[i] = descriptor_heaps[i];
    if (pinned.heaps[i] != nullptr) pinned.heaps[i]->AddRef();
  }
  ShadowPins replaced{};
  std::vector<ListComputeShadow> drained;
  unsigned dropped_tables = 0;
  unsigned illegal_sets = 0;
  const bool observed = internal_shadow::WithListShadow(
      command_list, "RecordShadowHeaps", true, replaced, drained,
      [&](ListComputeShadow& entry) {
        if (!legal) {
          // Same aftermath as the capture window - see
          // ComputeStateFields::OnSetDescriptorHeaps.  Nothing is written,
          // so nothing is pinned or replaced here either.
          ++illegal_sets;
          if (entry.DropTablesForHeapChange()) ++dropped_tables;
          return true;
        }
        for (UINT i = 0; i < 4; ++i) {
          replaced.heaps[i] = entry.heaps[i];
        }
        // The racing fast path above can miss (stripe parked, or the entry
        // was created between), so the set is compared again under the lock:
        // dropping a table on a redundant rebind would cost engagement.
        const bool same = entry.heaps_known
            && SameDescriptorHeapSet(
                   entry.heaps, entry.heap_count, pinned.heaps, new_count);
        entry.heaps_known = true;
        entry.heap_count = new_count;
        entry.touched_heaps = true;
        for (UINT i = 0; i < 4; ++i) {
          entry.heaps[i] = pinned.heaps[i];
        }
        if (!same && entry.DropTablesForHeapChange()) ++dropped_tables;
        return true;
      });
  if (!observed) {
    // The pins this call created are the only owners now.
    ReleaseShadowComRefs(pinned);
  }
  if (dropped_tables != 0) {
    list_shadow_heap_table_drops.fetch_add(
        dropped_tables, std::memory_order_relaxed);
  }
  if (illegal_sets != 0) {
    list_shadow_heap_sets_illegal.fetch_add(
        illegal_sets, std::memory_order_relaxed);
  }
  ReleaseShadowComRefs(replaced);
  for (ListComputeShadow& dropped : drained) ReleaseShadowComRefs(dropped);
}

inline void RecordShadowPipelineState(
    ID3D12GraphicsCommandList* command_list,
    ID3D12PipelineState* pipeline_state) {
  if (pipeline_state != nullptr) pipeline_state->AddRef();
  ShadowPins replaced{};
  std::vector<ListComputeShadow> drained;
  const bool observed = internal_shadow::WithListShadow(
      command_list, "RecordShadowPipelineState", true, replaced, drained,
      [&](ListComputeShadow& entry) {
        replaced.pipeline_state = entry.pipeline_state;
        entry.OnSetPipelineState(pipeline_state);
        return true;
      });
  if (!observed && pipeline_state != nullptr) pipeline_state->Release();
  ReleaseShadowComRefs(replaced);
  for (ListComputeShadow& dropped : drained) ReleaseShadowComRefs(dropped);
}

inline void RecordShadowComputeRootSignature(
    ID3D12GraphicsCommandList* command_list,
    ID3D12RootSignature* root_signature) {
  // Identical-signature rebinds are common and preserve arguments; skip the
  // pin churn entirely.
  const auto same_signature = [&](const ListComputeShadow* entry) {
    return entry != nullptr && entry->root_signature_known
        && entry->compute_root_signature == root_signature;
  };
  if (list_state_mode.load(std::memory_order_relaxed)
      == kListStateObjectAttached) {
    if (ListStateObject* object = AcquireListState(command_list, false)) {
      const bool same = same_signature(&object->shadow);
      object->Release();
      if (same) return;
    }
  } else {
    ListShadowStripe& stripe =
        ListShadowStripeFor(GetListShadowState(), command_list);
    if (ListShadowStripeOff(stripe)) return;
    ListShadowTryGuard guard(stripe.lock);
    if (guard && same_signature(FindListShadowLocked(stripe, command_list))) {
      return;
    }
  }
  if (root_signature != nullptr) root_signature->AddRef();
  ShadowPins replaced{};
  std::vector<ListComputeShadow> drained;
  const bool observed = internal_shadow::WithListShadow(
      command_list, "RecordShadowComputeRootSignature", true, replaced,
      drained, [&](ListComputeShadow& entry) {
        replaced.compute_root_signature = entry.compute_root_signature;
        entry.OnSetComputeRootSignature(root_signature);
        return true;
      });
  if (!observed && root_signature != nullptr) root_signature->Release();
  ReleaseShadowComRefs(replaced);
  for (ListComputeShadow& dropped : drained) ReleaseShadowComRefs(dropped);
}

inline void RecordShadowRootArguments(
    ID3D12GraphicsCommandList* command_list,
    UINT root_parameter_index,
    void (ComputeStateFields::*apply)(UINT, uint64_t),
    uint64_t address) {
  if (root_parameter_index >= kMaxRootParameters) return;
  ShadowPins replaced{};
  std::vector<ListComputeShadow> drained;
  internal_shadow::WithListShadow(
      command_list, "RecordShadowRootArguments", true, replaced, drained,
      [&](ListComputeShadow& entry) {
        (entry.*apply)(root_parameter_index, address);
        return true;
      });
  ReleaseShadowComRefs(replaced);
  for (ListComputeShadow& dropped : drained) ReleaseShadowComRefs(dropped);
}

inline void RecordShadowRootConstants(
    ID3D12GraphicsCommandList* command_list,
    UINT root_parameter_index,
    UINT num_32bit_values_to_set,
    const void* src_data,
    UINT dest_offset) {
  if (root_parameter_index >= kMaxRootParameters || src_data == nullptr) return;
  ShadowPins replaced{};
  std::vector<ListComputeShadow> drained;
  internal_shadow::WithListShadow(
      command_list, "RecordShadowRootConstants", true, replaced, drained,
      [&](ListComputeShadow& entry) {
        entry.OnSetComputeRoot32BitConstants(
            root_parameter_index, num_32bit_values_to_set, src_data, dest_offset);
        return true;
      });
  ReleaseShadowComRefs(replaced);
  for (ListComputeShadow& dropped : drained) ReleaseShadowComRefs(dropped);
}

// ClearState and a successful Reset both leave a fully KNOWN baseline
// (everything unbound; PSO = the call's argument, possibly null).  Writing
// the baseline keeps later binds incremental instead of forcing re-learning.
inline void WriteShadowBaseline(
    ID3D12GraphicsCommandList* command_list,
    ID3D12PipelineState* reset_pipeline_state,
    const char* site) {
  // The baseline ADOPTS the reset PSO into the entry, so it must take a
  // reference for it exactly like RecordShadowPipelineState does - the entry
  // is released through ReleaseShadowComRefs, which cannot tell a pin it was
  // given from one it took.  Without this, every `Reset(allocator, pso)` and
  // every `ClearState(pso)` with a non-null PSO armed one over-release of
  // the GAME's pipeline state, redeemed when the entry was next replaced,
  // evicted or destroyed.  Nothing caught it because every model test passes
  // a bogus non-COM pointer and every host in the harness passes
  // `pInitialState = nullptr` - the one argument shape the field always has
  // and the tests never did (test/dlss5_gpu S16 now uses a real PSO).
  // AddRef before the lock, Release if no entry took it: the stripe's
  // critical section is documented to run no COM call.
  if (reset_pipeline_state != nullptr) reset_pipeline_state->AddRef();
  ShadowPins replaced{};
  std::vector<ListComputeShadow> drained;
  const bool observed = internal_shadow::WithListShadow(
      command_list, site, true, replaced, drained, [&](ListComputeShadow& entry) {
        // Move the entry's COM pins out, then rebuild the deterministic
        // baseline in place.
        for (UINT i = 0; i < 4; ++i) replaced.heaps[i] = entry.heaps[i];
        replaced.compute_root_signature = entry.compute_root_signature;
        replaced.pipeline_state = entry.pipeline_state;
        entry.OnClearState(reset_pipeline_state);
        return true;
      });
  if (!observed && reset_pipeline_state != nullptr) {
    reset_pipeline_state->Release();
  }
  ReleaseShadowComRefs(replaced);
  for (ListComputeShadow& dropped : drained) ReleaseShadowComRefs(dropped);
}

// Executed bundles: how many, so the field can tell "NR is off because this
// engine records bundles" from every other decline.
inline std::atomic_uint64_t list_shadow_bundles{0};
// Of those, how many carried an observed delta - the parent keeps a complete
// restore target and NR keeps running - and how many were opaque, which is
// the safe reading but costs engagement until the host re-binds.  An opaque
// count that tracks the total means the bundles were recorded before the
// hooks installed (PLAN_REHAB_V7.md R2 closes that by observing birth).
inline std::atomic_uint64_t list_shadow_bundles_merged{0};
inline std::atomic_uint64_t list_shadow_bundles_opaque{0};

// ExecuteIndirect: how many, and how they were read.  `inert` is the large
// majority in a GPU-driven engine - a graphics signature or a dispatch-only
// compute one, neither of which can change a compute root argument - and it
// is reported so that a field log can show the volume is being seen and
// costing nothing.  `applied` is a compute signature whose named arguments
// were zeroed exactly.  `unknown_sig` is a signature created before the
// device hook was live (or one whose descriptor did not fit): the call is
// left alone, which keeps engagement but is the one case where a stale root
// constant can still be restored.  `unknown_sig` staying at 0 is what says
// the device hook is covering the title.
inline std::atomic_uint64_t list_shadow_indirects{0};
inline std::atomic_uint64_t list_shadow_indirects_applied{0};
inline std::atomic_uint64_t list_shadow_indirects_inert{0};
inline std::atomic_uint64_t list_shadow_indirects_unknown_sig{0};

// Snapshots what a bundle's recording will carry into its parent.
//
// The pins are AddRef'd after every lock is dropped.  That window is safe by
// the API's own contract: command lists hold no references to the objects
// bound into them, so "applications are responsible for ensuring that a
// command list is never submitted for execution that references a destroyed
// resource" (MS, Creating and recording command lists and bundles).  A heap,
// signature or PSO recorded in a bundle the host is executing right now is
// alive, or the host has already broken that contract.
inline void ReadBundleDelta(
    ID3D12GraphicsCommandList* bundle, BundleDelta& out) {
  out.Reset();
  if (bundle == nullptr) return;
  const auto take = [&out](const ListComputeShadow& shadow) {
    out.touched_heaps = shadow.touched_heaps;
    out.touched_root_signature = shadow.touched_root_signature;
    out.touched_root_arguments = shadow.touched_root_arguments;
    out.pipeline_state_known = shadow.pipeline_state_known;
    if (shadow.touched_heaps) {
      out.heap_count = shadow.heap_count;
      std::memcpy(out.heaps, shadow.heaps, sizeof(out.heaps));
    }
    if (shadow.touched_root_signature) {
      out.compute_root_signature = shadow.compute_root_signature;
    }
    if (shadow.pipeline_state_known) {
      out.pipeline_state = shadow.pipeline_state;
    }
    for (unsigned index = 0; index < kMaxRootParameters; ++index) {
      if ((out.touched_root_arguments & (1ull << index)) == 0) continue;
      out.root_arguments[index] = shadow.root_arguments[index];
    }
    out.observed = true;
  };
  if (list_state_mode.load(std::memory_order_relaxed)
      == kListStateObjectAttached) {
    if (ListStateObject* object = AcquireListState(bundle, false)) {
      take(object->shadow);
      object->Release();
    }
  } else {
    ListShadowStripe& stripe =
        ListShadowStripeFor(GetListShadowState(), bundle);
    if (ListShadowStripeOff(stripe)) return;
    ListShadowTryGuard guard(stripe.lock);
    if (!guard) {
      ParkListShadowStripe(stripe, "ReadBundleDelta");
      return;
    }
    if (const ListComputeShadow* entry =
            FindListShadowLocked(stripe, bundle)) {
      take(*entry);
    }
  }
  if (!out.observed) return;
  for (UINT i = 0; i < 4; ++i) {
    if (out.heaps[i] != nullptr) out.heaps[i]->AddRef();
  }
  if (out.compute_root_signature != nullptr) out.compute_root_signature->AddRef();
  if (out.pipeline_state != nullptr) out.pipeline_state->AddRef();
  out.pins_held = true;
}

// Gives the pins back.  The values are left in place: a capture window still
// copies them, and it pins nothing of its own.
inline void ReleaseBundleDeltaPins(BundleDelta& delta) noexcept {
  if (!delta.pins_held) return;
  delta.pins_held = false;
  for (UINT i = 0; i < 4; ++i) {
    if (delta.heaps[i] != nullptr) delta.heaps[i]->Release();
  }
  if (delta.compute_root_signature != nullptr) {
    delta.compute_root_signature->Release();
  }
  if (delta.pipeline_state != nullptr) delta.pipeline_state->Release();
}

// An executed bundle changes the parent's compute state, and every change it
// makes persists into the parent (MS, ExecuteBundle).  A bundle records
// through the same detoured function bodies as a direct list, so when its
// recording was observed the parent's shadow takes exactly the bundle's
// delta and the restore target stays complete.  When it was not observed the
// knowledge - not the values - is dropped, so the gate declines until the
// host re-binds, with no Reset required.
//
// Returns the delta for the caller's capture windows (null = unobserved).
// The pointers in it are borrowed: a merged delta's references now belong to
// the parent's shadow entry.
inline const BundleDelta* RecordShadowExecutedBundle(
    ID3D12GraphicsCommandList* command_list,
    ID3D12GraphicsCommandList* bundle) {
  list_shadow_bundles.fetch_add(1, std::memory_order_relaxed);
  // ~18 KB of per-thread scratch, built once: ExecuteBundle is a per-pass
  // call on engines that use it, and a stack copy of this per call would
  // dwarf the work.
  static thread_local BundleDelta delta;
  ReadBundleDelta(bundle, delta);
  ShadowPins replaced{};
  std::vector<ListComputeShadow> drained;
  const bool observed = internal_shadow::WithListShadow(
      command_list, "RecordShadowExecutedBundle", false, replaced, drained,
      [&](ListComputeShadow& entry) {
        if (!delta.observed) {
          entry.OnExecuteBundle();
          return true;
        }
        // Move out only the pins the merge actually overwrites: releasing a
        // pin the entry keeps would drop a reference it still holds.
        if (delta.touched_heaps) {
          for (UINT i = 0; i < 4; ++i) replaced.heaps[i] = entry.heaps[i];
        }
        if (delta.touched_root_signature) {
          replaced.compute_root_signature = entry.compute_root_signature;
        }
        if (delta.pipeline_state_known) {
          replaced.pipeline_state = entry.pipeline_state;
        }
        entry.MergeExecutedBundle(delta);
        return true;
      });
  const bool merged = observed && delta.observed;
  if (merged) {
    list_shadow_bundles_merged.fetch_add(1, std::memory_order_relaxed);
    // The entry owns them now.
    delta.pins_held = false;
  } else {
    // A bundle whose recording was never seen is the one that costs
    // engagement; a parent with no entry at all changed nothing.
    if (!delta.observed) {
      list_shadow_bundles_opaque.fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseBundleDeltaPins(delta);
  }
  ReleaseShadowComRefs(replaced);
  for (ListComputeShadow& dropped : drained) ReleaseShadowComRefs(dropped);
  return delta.observed ? &delta : nullptr;
}

// ExecuteIndirect with a compute signature that names root arguments: apply
// the documented aftermath to the list's shadow.  The caller has already
// established that there is something to do - a graphics signature or a
// dispatch-only one short-circuits before here, which is the large majority
// of a GPU-driven engine's indirects.
//
// No pins change: every slot this touches becomes either a constant value or
// nothing, and neither holds a COM reference.
inline void RecordShadowExecuteIndirect(
    ID3D12GraphicsCommandList* command_list,
    const IndirectSignatureEffect& effect) {
  ShadowPins replaced{};
  std::vector<ListComputeShadow> drained;
  internal_shadow::WithListShadow(
      command_list, "RecordShadowExecuteIndirect", false, replaced, drained,
      [&](ListComputeShadow& entry) {
        entry.OnExecuteIndirect(effect);
        return true;
      });
  for (ListComputeShadow& dropped : drained) ReleaseShadowComRefs(dropped);
}

// A FAILED Reset leaves the list's state undefined from the observer's
// perspective; the only safe shadow state is "unknown" (entry erased).
inline void EraseListShadow(ID3D12GraphicsCommandList* command_list) {
  ShadowPins replaced{};
  std::vector<ListComputeShadow> drained;
  internal_shadow::WithListShadow(
      command_list, "EraseListShadow", false, replaced, drained,
      [](ListComputeShadow&) { return false; });
  for (ListComputeShadow& dropped : drained) ReleaseShadowComRefs(dropped);
}

// Shared read protocol: reads return the surviving entries even on a
// previously-parked stripe.  The v6audit3 drain-on-read (entries dropped
// after every park) was briefly the freeze suspect and disabled here; the
// final 2026-09-16 boot-canary bisect pinned the freeze on the typed-SRV
// exposure probe (see kExposureProbe in dlssnr.hpp) and this simplified read
// is what the passing canaries (C7-C9) shipped with, so it stays: audit
// issue 04's stale-revival concern remains theoretical while the simplified
// read is field-proven on KCD2.  Do not re-add the drain without the Stage-3
// harness qualifying it.
inline ListComputeShadow* ReadListShadowLocked(
    ListShadowStripe& stripe, ID3D12GraphicsCommandList* command_list) {
  return FindListShadowLocked(stripe, command_list);
}

inline void SeedCaptureFromShadow(
    ID3D12GraphicsCommandList* command_list,
    ComputeStateFields& out) {
  if (command_list == nullptr) return;
  if (list_state_mode.load(std::memory_order_relaxed)
      == kListStateObjectAttached) {
    if (ListStateObject* object = AcquireListState(command_list, false)) {
      out = object->shadow;
      object->Release();
    }
    return;
  }
  ListShadowStripe& stripe =
      ListShadowStripeFor(GetListShadowState(), command_list);
  if (ListShadowStripeOff(stripe)) return;
  ListShadowTryGuard guard(stripe.lock);
  if (!guard) {
    ParkListShadowStripe(stripe, "SeedCaptureFromShadow");
    return;
  }
  const ListComputeShadow* entry = ReadListShadowLocked(stripe, command_list);
  ReleaseParkedDrains(stripe);
  if (entry == nullptr) return;
  out.heaps_known = entry->heaps_known;
  out.heap_count = entry->heap_count;
  for (UINT i = 0; i < 4; ++i) out.heaps[i] = entry->heaps[i];
  out.root_signature_known = entry->root_signature_known;
  out.compute_root_signature = entry->compute_root_signature;
  out.pipeline_state_known = entry->pipeline_state_known;
  out.pipeline_state = entry->pipeline_state;
  out.root_arguments_known = entry->root_arguments_known;
  for (unsigned i = 0; i < kMaxRootParameters; ++i) {
    out.root_arguments[i] = entry->root_arguments[i];
  }
}

// Teardown: drain every stripe, ignoring park state (pins must not leak at
// process teardown).  A stripe whose lock still cannot be taken keeps its
// pins - the state is heap-resident for the process lifetime by design.
inline void ClearListShadowAtTeardown() {
  DetachListStateObjects();
  ListShadowState& state = GetListShadowState();
  for (auto& stripe : state.stripes) {
    std::vector<ListShadowEntry> drained;
    try {
      ListShadowTryGuard guard(stripe.lock);
      if (!guard) continue;
      drained.swap(stripe.shadows);
      stripe.entry_count.store(0, std::memory_order_relaxed);
      stripe.disabled_until_ms.store(0, std::memory_order_release);
      stripe.entries_suspect.store(false, std::memory_order_release);
    } catch (...) {
      continue;
    }
    for (ListShadowEntry& entry : drained) {
      ReleaseShadowComRefs(*entry.shadow);
    }
  }
}

// Idle eviction.  List destruction is not observed in the field configuration
// (see OnDestroyCommandListEvent in dlssnr.hpp), so without this an entry and
// its COM pins outlive its list for the whole session and every hooked call
// scans them.  An entry no host call touched for more than `max_idle_ticks`
// of list_shadow_tick is dropped, which is the fail-open "unknown" state: a
// list that comes back is Reset before it records, and that rewrites the
// baseline.  One lock attempt per stripe, no spin and no park - a busy stripe
// is simply swept on a later call.  Returns the entries dropped.
inline size_t SweepIdleListShadows(uint32_t max_idle_ticks) {
  const uint32_t now = list_shadow_tick.load(std::memory_order_relaxed);
  size_t dropped = 0;
  ListShadowState& state = GetListShadowState();
  for (auto& stripe : state.stripes) {
    if (stripe.entry_count.load(std::memory_order_relaxed) == 0) continue;
    std::vector<ListShadowEntry> drained;
    if (TryAcquireSRWLockExclusive(&stripe.lock) == FALSE) continue;
    try {
      for (size_t i = 0; i < stripe.shadows.size();) {
        if (now - stripe.shadows[i].last_tick > max_idle_ticks) {
          drained.push_back(std::move(stripe.shadows[i]));
          if (i + 1 != stripe.shadows.size()) {
            stripe.shadows[i] = std::move(stripe.shadows.back());
          }
          stripe.shadows.pop_back();
        } else {
          ++i;
        }
      }
      stripe.entry_count.store(
          static_cast<uint32_t>(stripe.shadows.size()),
          std::memory_order_relaxed);
    } catch (...) {
    }
    ReleaseSRWLockExclusive(&stripe.lock);
    for (ListShadowEntry& entry : drained) {
      ReleaseShadowComRefs(*entry.shadow);
    }
    stripe.idle_evictions.fetch_add(drained.size(), std::memory_order_relaxed);
    dropped += drained.size();
  }
  return dropped;
}

// Lock-free bookkeeping snapshot (telemetry only).
struct ListShadowStats {
  uint32_t entries = 0;
  uint32_t longest_stripe = 0;
  uint64_t parks = 0;
  uint64_t waits = 0;
  uint64_t idle_evictions = 0;
  // Object-attached mode: live attached objects and lost attaches.
  uint32_t attached = 0;
  uint64_t attach_failures = 0;
  // Bundles executed on a list we shadow (v6.8.0-alpha1), and how they were
  // read: `merged` carried an observed delta and cost no engagement,
  // `opaque` was recorded unobserved and dropped the parent's knowledge.
  uint64_t bundles = 0;
  uint64_t bundles_merged = 0;
  uint64_t bundles_opaque = 0;
  // ExecuteIndirect (v6.8.0-alpha8): total, of which `applied` changed a
  // named compute root argument, `inert` could not change any, and
  // `unknown_sig` had no description to act on.
  uint64_t indirects = 0;
  uint64_t indirects_applied = 0;
  uint64_t indirects_inert = 0;
  uint64_t indirects_unknown_sig = 0;
  // Table arguments dropped by a host heap change, and off-contract heap
  // sets seen (v6.8.0-alpha9).
  uint64_t heap_table_drops = 0;
  uint64_t heap_sets_illegal = 0;
};

inline ListShadowStats SnapshotListShadowStats() {
  ListShadowStats stats;
  stats.attached = GetListStateRegistry().live.load(std::memory_order_relaxed);
  stats.attach_failures =
      GetListStateRegistry().attach_failures.load(std::memory_order_relaxed);
  stats.waits = list_shadow_waits.load(std::memory_order_relaxed);
  stats.bundles = list_shadow_bundles.load(std::memory_order_relaxed);
  stats.bundles_merged =
      list_shadow_bundles_merged.load(std::memory_order_relaxed);
  stats.bundles_opaque =
      list_shadow_bundles_opaque.load(std::memory_order_relaxed);
  stats.heap_table_drops =
      list_shadow_heap_table_drops.load(std::memory_order_relaxed);
  stats.heap_sets_illegal =
      list_shadow_heap_sets_illegal.load(std::memory_order_relaxed);
  stats.indirects = list_shadow_indirects.load(std::memory_order_relaxed);
  stats.indirects_applied =
      list_shadow_indirects_applied.load(std::memory_order_relaxed);
  stats.indirects_inert =
      list_shadow_indirects_inert.load(std::memory_order_relaxed);
  stats.indirects_unknown_sig =
      list_shadow_indirects_unknown_sig.load(std::memory_order_relaxed);
  ListShadowState& state = GetListShadowState();
  for (auto& stripe : state.stripes) {
    const uint32_t count = stripe.entry_count.load(std::memory_order_relaxed);
    stats.entries += count;
    if (count > stats.longest_stripe) stats.longest_stripe = count;
    stats.parks += stripe.parks.load(std::memory_order_relaxed);
    stats.idle_evictions +=
        stripe.idle_evictions.load(std::memory_order_relaxed);
  }
  return stats;
}

inline void BeginComputeStateCapture(
    CapturedComputeState* storage,
    ID3D12GraphicsCommandList* command_list) {
  storage->command_list = command_list;
  storage->observed_clear_state = false;
  storage->heaps_known = false;
  storage->heap_count = 0;
  std::memset(storage->heaps, 0, sizeof(storage->heaps));
  storage->root_signature_known = false;
  storage->compute_root_signature = nullptr;
  storage->pipeline_state_known = false;
  storage->pipeline_state = nullptr;
  storage->root_arguments_known = false;
  for (RootArgumentState& argument : storage->root_arguments) {
    argument.Reset();
  }
  storage->outer_capture = compute_state_capture;
  compute_state_capture = storage;
  // Seed the host's pre-window state from the persistent shadow: bindings
  // made before this window (or never re-bound inside it) still restore.
  // The shadow pins its COM references, so seeded pointers cannot dangle
  // before the restore.
  SeedCaptureFromShadow(command_list, *storage);
}

// ---------------------------------------------------------------------------
// Restoration
// ---------------------------------------------------------------------------

// Re-applies the observed state AFTER injected work, through the command
// list interface (not saved raw pointers) so a nested outer capture window
// records the restore itself and stays last-write-wins-correct.
inline void ApplyCapturedComputeState(
    ID3D12GraphicsCommandList* command_list,
    const ComputeStateFields& captured,
    bool hooks_installed) {
  if (command_list == nullptr || !hooks_installed) {
    return;
  }
  // Heaps: the known-empty baseline (ClearState/Reset aftermath) restores
  // the empty set explicitly - the injection always swaps the shader-visible
  // heap.  Callers gate on RestoreTargetComplete(): the per-aspect checks
  // below only ever replay observed values, they cannot make a partial
  // target safe (an unrestored aspect keeps the injected binding).
  if (captured.heaps_known) {
    // The heaps go to D3D12 in the LIST's world.  The shadow holds what the
    // hooked implementation was handed: ReShade heap proxies when the list
    // detours sit on its proxy (the full add-on build wraps CBV_SRV_UAV and
    // SAMPLER heaps), native heaps when they sit on d3d12's own methods.  A
    // native list is handed native heaps - ReShade's proxy unwraps them the
    // same way before it forwards (d3d12_command_list.cpp:507) - so heap
    // proxies are unwrapped for a native list: the Streamline fallback
    // injects on the native list of a session whose hooks were installed
    // from the game's proxy (dlss5_gpu S-world; until v7.0.0-alpha47 this
    // replayed ReShade's proxy objects to D3D12 as heaps).  A proxy list is
    // handed what the shadow holds; the native-to-proxy direction has no
    // conversion and the envelope logs it.
    // The array is never NULL: the D3D12 debug layer rejects one even with
    // count 0 ("pDescriptorHeaps cannot be NULL"), so the empty set restores
    // through entries the runtime does not read (v6.7.0, sentinel audit).
    const UINT heap_count = captured.heap_count < kMaxDescriptorHeaps
                                ? captured.heap_count
                                : kMaxDescriptorHeaps;
    ID3D12DescriptorHeap* heaps[kMaxDescriptorHeaps] = {};
    std::memcpy(heaps, captured.heaps, heap_count * sizeof(heaps[0]));
    ID3D12GraphicsCommandList* native_list = command_list;
    if (!renodx::utils::directx::NativeFromReShadeProxy(&native_list)) {
      for (ID3D12DescriptorHeap*& heap : heaps) {
        if (heap != nullptr) renodx::utils::directx::NativeFromReShadeProxy(&heap);
      }
    }
    command_list->SetDescriptorHeaps(heap_count, heaps);
  }
  // Root signature first: argument indices belong to its layout.  A
  // known-null signature (ClearState without a rebind) is restored as null;
  // unknown leaves the injected signature bound and the arguments below are
  // skipped with it (replaying layout-mismatched arguments would bind
  // garbage, not host state).
  if (captured.root_signature_known) {
    command_list->SetComputeRootSignature(captured.compute_root_signature);
    for (UINT index = 0; index < kMaxRootParameters; ++index) {
      const RootArgumentState& argument = captured.root_arguments[index];
      switch (argument.kind) {
        case RootArgumentKind::kNone:
          break;
        case RootArgumentKind::kTable:
          command_list->SetComputeRootDescriptorTable(
              index, D3D12_GPU_DESCRIPTOR_HANDLE{argument.address});
          break;
        case RootArgumentKind::kConstants: {
          // Replay per contiguous observed run so partial updates merge
          // back into exactly the DWORDs the host had bound.
          uint32_t offset = 0;
          uint32_t length = 0;
          while (argument.NextConstantRun(offset, &offset, &length)) {
            command_list->SetComputeRoot32BitConstants(
                index, length, argument.constants + offset, offset);
            offset += length;
          }
          break;
        }
        case RootArgumentKind::kConstantBufferView:
          command_list->SetComputeRootConstantBufferView(index, argument.address);
          break;
        case RootArgumentKind::kShaderResourceView:
          command_list->SetComputeRootShaderResourceView(index, argument.address);
          break;
        case RootArgumentKind::kUnorderedAccessView:
          command_list->SetComputeRootUnorderedAccessView(index, argument.address);
          break;
      }
    }
  }
  // A known-null PSO stays unrestored: the host must set one before its
  // next draw or dispatch, so the injected PSO is never executed by it.
  if (captured.pipeline_state_known && captured.pipeline_state != nullptr) {
    command_list->SetPipelineState(captured.pipeline_state);
  }
}

}  // namespace renodx::addons::dlss5
