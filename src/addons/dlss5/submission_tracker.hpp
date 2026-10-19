/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Exact submission-use tracking for GPU object lifetime (v6 audit issue 01).
//
// A GpuLease (gpu_lease.hpp) snapshots `last_assigned + 1` for EVERY known
// queue: any queue's next submission proves the lease, whether or not that
// submission touched the guarded resources, and an idle queue starves every
// lease forever. This module replaces that guess for resource retention with
// the actual dependency graph:
//
//   - Recording a command that can reference an addon-owned GPU object
//     registers a USE on the list's current RECORDING GENERATION (everything
//     recorded between Reset calls; D3D12 requires recording to finish before
//     the first submit, so uses always precede the generation's submissions).
//   - ExecuteCommandLists attaches an exact (fence, value) completion token to
//     the submitted generations via a two-phase protocol (see
//     BeginSubmission/CompleteSubmission) that cannot be orphaned by a
//     concurrent Reset.
//   - A generation releases its uses only when it is closed (Reset/destroy:
//     replay impossible) AND every submission completed - or the device was
//     removed (UINT64_MAX fence), which is the existing retirement semantic.
//     A submission without a proof (faulted queue Signal) retains until
//     teardown, exactly like an unproven lease.
//
// D3D12 lifetime facts this depends on (Microsoft, "Recording command lists
// and bundles"): command lists do not retain referenced objects; a recorded
// list may be executed repeatedly until it is Reset; Reset discards the
// recording but not in-flight executions of earlier submissions.
//
// Pointer identity is safe for USES because every consumer holds its own COM
// reference for the object's lifetime (retirement releases last), so a
// tracked address cannot be recycled by a new allocation while tracked.

#pragma once

#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../../utils/directx.hpp"
#include "gpu_lease.hpp"
#include "module_guid.hpp"

namespace renodx::addons::dlss5::submission {

// One completion token attached to a generation by one ExecuteCommandLists
// call. `pending` marks the two-phase window: reserved before the real
// forward, filled with the fence proof after the post-submit Signal. A
// completed token with a null fence is an UNPROVABLE submission - nothing may
// ever prove it, so its generation retains until teardown.
struct GenerationSubmission {
  bool pending = true;
  FenceRef fence;
  uint64_t value = 0;
};

// One recording generation of one command list.
struct Generation {
  // False once the recording was discarded (Reset) or the list destroyed:
  // the app can no longer replay it.
  bool open = true;
  // Addon-owned GPU objects this recording can reference (deduplicated).
  std::vector<const void*> uses;
  std::vector<GenerationSubmission> submissions;
};

struct ListState {
  // Generation id currently being recorded; entries are created lazily by the
  // first use or submission, so untouched game lists cost nothing.
  uint64_t current = 1;
  bool destroyed = false;
  std::unordered_map<uint64_t, Generation> generations;
  // Every pointer seen to carry this identity - under ReShade two, the
  // wrapper the record side is handed and the native the submit side is.
  // They exist for the destroy path (see identity_by_pointer) and are
  // dropped with the entry that names them.
  std::vector<const void*> aliases;
};

// {B0E3C1A4-7D25-4E86-9F0B-2C6A8D41E573}, per loaded copy (module_guid.hpp):
// another copy's identities are keys into ITS tracker, not this one's.
inline const GUID kListIdentityGuid = ModuleScopedGuid(
    {0xb0e3c1a4, 0x7d25, 0x4e86, {0x9f, 0x0b, 0x2c, 0x6a, 0x8d, 0x41, 0xe5, 0x73}});
inline std::atomic_uint64_t next_list_identity{1};
// Lists whose private data refused the identity (telemetry; expected 0).
// They fall back to a pointer key, which is exactly the broken state below,
// so a non-zero value here is the one thing that makes it visible.
inline std::atomic_uint64_t list_identity_fallbacks{0};

// The tracker key of `list`.
//
// It cannot be the pointer.  The record side (TrackUse, OnCommandListReset)
// runs from the command-list detours, which are installed on the object the
// host holds and hands to NGX - under ReShade, its WRAPPER.  The submit side
// (BeginSubmission) runs from the queue detour, which is installed on
// `queue->get_native()` and is therefore handed NATIVE list pointers.  Keyed
// by pointer, the two halves name two different lists and no submission can
// ever be matched to the work it carried: measured on every T2 profile,
// `submitted=0` on runs that injected 238 of 240 evaluates.
//
// That is not a lost counter.  A generation that closes with an empty
// submissions vector reads as COMPLETE in PruneCompletedGenerations, so its
// uses are dropped - and when a retirement carries `tracker_proof` the
// tracker is the ONLY authority deciding that a workset or an NR feature may
// be freed, the GPU lease having been bypassed.
//
// Private data is the join, and it is the same one the state shadow already
// relies on: D3D12 proxies forward private data to the native object, so an
// identity written through the wrapper IS the identity the native read
// returns.  Verbatim forwarding, verified in the pinned source
// (external/reshade/source/d3d12/d3d12_command_list.cpp:129-136:
// `return _orig->GetPrivateData(guid, pDataSize, pData);` and the same for
// SetPrivateData), and measured end to end on the T2 host: `submitted=239
// fallbacks=0` on the profile that read `submitted=0` the build before.  With `create`, a list without one gets one (TrackUse); without,
// an untracked list returns its pointer key, which matches only lists whose
// identity could not be stored.  Pointer keys carry the top bit; identities
// count up from 1 and never reach it.
inline uint64_t ListIdentity(ID3D12CommandList* list, bool create) {
  uint64_t identity = 0;
  UINT size = sizeof(identity);
  if (SUCCEEDED(list->GetPrivateData(kListIdentityGuid, &size, &identity))
      && size == sizeof(identity) && identity != 0) {
    return identity;
  }
  if (create) {
    identity = next_list_identity.fetch_add(1, std::memory_order_relaxed);
    if (SUCCEEDED(list->SetPrivateData(
            kListIdentityGuid, sizeof(identity), &identity))) {
      return identity;
    }
    list_identity_fallbacks.fetch_add(1, std::memory_order_relaxed);
  }
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(list))
      | (uint64_t{1} << 63);
}

inline std::mutex tracker_mutex;
inline std::unordered_map<uint64_t, ListState> tracked_lists;
// Pointer -> identity, for the one caller that must NOT touch the object.
// The destroy notification arrives while the list is being destroyed -
// ReShade invokes it from ~D3D12GraphicsCommandList, and the GPU harness
// delivers it deliberately for pointers that are already dead - so that key
// has to come from a table.  Filled by the live paths, which see the object
// alive, and emptied with the entry it names.
inline std::unordered_map<const void*, uint64_t> identity_by_pointer;
// Live-use refcount per tracked pointer: how many generations can still
// execute or replay it. Absent/zero means nothing retains the object.
inline std::unordered_map<const void*, uint64_t> live_uses;

// Sticky arming for the queue-detour gate (v6.5.3, 007 First Light field
// log).  The ExecuteCommandLists detour must stay GPU-inert - no per-queue
// fence, no appended post-submit signal - until the addon recorded its first
// tracked use, because in a passive session (NR declined by the restore-
// target gate, the game initializing frame generation) those appended
// signals were the mod's only GPU-visible mutation.  Every consumer of the
// proof machinery - generations with uses, and every lease (retirements,
// probes, captures) - appears strictly after some TrackUse, so arming here
// can never strand a proof: a use is always recorded before the submission
// that must prove it, and arming is sticky, so leases held across idle
// stretches still see their queues advance exactly as before.  Cleared only
// by ResetAll (teardown / device-generation boundary).
inline std::atomic_bool ever_tracked_uses{false};

inline bool HasEverTrackedUses() noexcept {
  return ever_tracked_uses.load(std::memory_order_acquire);
}

// Record the pointer that resolved to `key`, so the destroy notification
// can find the entry without dereferencing a dying object. Lock held.
inline void RememberAliasLocked(
    const void* list, uint64_t key, ListState& state) {
  const auto existing = identity_by_pointer.find(list);
  if (existing != identity_by_pointer.end()) {
    if (existing->second == key) return;
    // A recycled address: the pointer now names a different list. The live
    // path is authoritative - it just read the identity off the object.
    existing->second = key;
  } else {
    identity_by_pointer.emplace(list, key);
  }
  for (const void* alias : state.aliases) {
    if (alias == list) return;
  }
  state.aliases.push_back(list);
}

// The pointer the destroy event will name for `list`: ReShade raises it with
// `get_native()`, the list behind its proxy.  The record side holds the proxy,
// and until v7.0.0-alpha47 the native alias was learned only at
// BeginSubmission - so a recording destroyed without ever being submitted was
// destroyed under a key nothing had stored, and its open generation retained
// every use for the rest of the session (dlss5_gpu case
// wrapped_list_destroyed_unsubmitted).  Resolved before the tracker lock.
inline ID3D12CommandList* DestroyAlias(ID3D12CommandList* list) {
  renodx::utils::directx::NativeFromReShadeProxy(&list);
  return list;
}

// Record-time (evaluate thread): the list's current recording can reference
// `resource`. Idempotent within a generation.
inline void TrackUse(ID3D12CommandList* list, const void* resource) {
  if (list == nullptr || resource == nullptr) return;
  ever_tracked_uses.store(true, std::memory_order_release);
  ID3D12CommandList* const native = DestroyAlias(list);
  std::lock_guard<std::mutex> lock(tracker_mutex);
  const uint64_t key = ListIdentity(list, true);
  ListState& state = tracked_lists[key];
  RememberAliasLocked(list, key, state);
  if (native != list) RememberAliasLocked(native, key, state);
  Generation& generation = state.generations[state.current];
  for (const void* use : generation.uses) {
    if (use == resource) return;
  }
  generation.uses.push_back(resource);
  ++live_uses[resource];
}

// Reset hook: the recorded generation can never be replayed or submitted
// again; earlier submissions of it may still be executing, so it releases
// only once they completed. A new generation begins implicitly.
inline void OnCommandListReset(ID3D12CommandList* list) {
  if (list == nullptr) return;
  const uint64_t key = ListIdentity(list, false);
  ID3D12CommandList* const native = DestroyAlias(list);
  std::lock_guard<std::mutex> lock(tracker_mutex);
  const auto it = tracked_lists.find(key);
  if (it == tracked_lists.end()) return;
  ListState& state = it->second;
  RememberAliasLocked(list, key, state);
  if (native != list) RememberAliasLocked(native, key, state);
  state.generations[state.current].open = false;
  ++state.current;
}

// Destruction (final Release observed): replay is impossible, so open
// generations close; already-submitted generations still wait for their
// completion proofs. The list entry is forgotten once everything pruned.
inline void OnCommandListDestroyed(ID3D12CommandList* list) {
  if (list == nullptr) return;
  std::lock_guard<std::mutex> lock(tracker_mutex);
  // NOT ListIdentity: `list` may already be dead here, and a GetPrivateData
  // on it is an access violation (measured - the GPU harness untracks its
  // case lists after they were dropped).
  const auto alias = identity_by_pointer.find(list);
  if (alias == identity_by_pointer.end()) return;
  const auto it = tracked_lists.find(alias->second);
  if (it == tracked_lists.end()) return;
  it->second.destroyed = true;
  for (auto& [id, generation] : it->second.generations) {
    generation.open = false;
  }
}

// Phase 1 of the submit protocol, called BEFORE forwarding the real
// ExecuteCommandLists: reserve a pending submission slot on every submitted
// list's current generation. The reservation exists before the call returns,
// so a Reset issued right after the submit can never close the generation
// ahead of its proof. Generations with no tracked uses are skipped - the
// addon did not record into them and retains nothing.
struct PendingSubmission {
  // The list's stable identity, not its pointer: the reservation is made on
  // the submit side and redeemed there too, but the generation it names was
  // opened by the record side through a different object (see ListIdentity).
  uint64_t list = 0;
  uint64_t generation = 0;
};

inline std::vector<PendingSubmission> BeginSubmission(
    UINT count, ID3D12CommandList* const* lists) {
  std::vector<PendingSubmission> pending;
  if (lists == nullptr) return pending;
  std::lock_guard<std::mutex> lock(tracker_mutex);
  for (UINT i = 0; i < count; ++i) {
    ID3D12CommandList* list = lists[i];
    if (list == nullptr) continue;
    const uint64_t key = ListIdentity(list, false);
    const auto it = tracked_lists.find(key);
    if (it == tracked_lists.end()) continue;
    ListState& state = it->second;
    RememberAliasLocked(list, key, state);
    Generation& generation = state.generations[state.current];
    if (generation.uses.empty()) {
      // Nothing tracked in this recording; drop the empty generation again.
      if (generation.submissions.empty()) {
        state.generations.erase(state.current);
      }
      continue;
    }
    generation.submissions.push_back({});
    pending.push_back({key, state.current});
  }
  return pending;
}

// Phase 2, called after the post-submit Signal: fill the reserved slots with
// the exact completion proof. A null fence leaves them unprovable (faulted
// queue) - retained until teardown, never guessed.
inline void CompleteSubmission(
    const std::vector<PendingSubmission>& pending,
    ID3D12Fence* fence,
    uint64_t value) {
  if (pending.empty()) return;
  std::lock_guard<std::mutex> lock(tracker_mutex);
  for (const PendingSubmission& reservation : pending) {
    const auto list_it = tracked_lists.find(reservation.list);
    if (list_it == tracked_lists.end()) continue;
    const auto gen_it = list_it->second.generations.find(reservation.generation);
    if (gen_it == list_it->second.generations.end()) continue;
    for (GenerationSubmission& submission : gen_it->second.submissions) {
      if (!submission.pending) continue;
      submission.pending = false;
      submission.fence = FenceRef(fence);
      submission.value = value;
      break;
    }
  }
}

// Present-thread poll: drop closed generations whose submissions all
// completed. Device removal (fence reports UINT64_MAX) counts as completed -
// a dead device cannot touch the resources again. Returns generations pruned
// (diagnostic).
inline size_t PruneCompletedGenerations() {
  std::lock_guard<std::mutex> lock(tracker_mutex);
  size_t pruned = 0;
  for (auto list_it = tracked_lists.begin(); list_it != tracked_lists.end();) {
    auto& generations = list_it->second.generations;
    for (auto gen_it = generations.begin(); gen_it != generations.end();) {
      const Generation& generation = gen_it->second;
      bool done = !generation.open;
      if (done) {
        for (const GenerationSubmission& submission :
             generation.submissions) {
          if (submission.pending || submission.fence.empty()) {
            done = false;
            break;
          }
          const uint64_t completed = submission.fence->GetCompletedValue();
          if (completed != UINT64_MAX && completed < submission.value) {
            done = false;
            break;
          }
        }
      }
      if (!done) {
        ++gen_it;
        continue;
      }
      for (const void* use : generation.uses) {
        const auto use_it = live_uses.find(use);
        if (use_it != live_uses.end() && --use_it->second == 0) {
          live_uses.erase(use_it);
        }
      }
      gen_it = generations.erase(gen_it);
      ++pruned;
    }
    if (list_it->second.destroyed && generations.empty()) {
      for (const void* alias : list_it->second.aliases) {
        const auto a = identity_by_pointer.find(alias);
        // Guarded: a recycled address may already name a newer list.
        if (a != identity_by_pointer.end() && a->second == list_it->first) {
          identity_by_pointer.erase(a);
        }
      }
      list_it = tracked_lists.erase(list_it);
    } else {
      ++list_it;
    }
  }
  return pruned;
}

// True when no tracked generation can still execute or replay `resource`.
// Untracked pointers are releasable by definition (nothing was recorded
// against them).
inline bool ResourceReleasable(const void* resource) {
  if (resource == nullptr) return true;
  std::lock_guard<std::mutex> lock(tracker_mutex);
  return live_uses.find(resource) == live_uses.end();
}

// True when every recording that carries addon work has been submitted AND
// has completed - the proof a whole-session release (Shutdown) needs.  A
// recording with uses and no submission is still the app's to submit, so it
// blocks even while open: a game's submission thread can hold a recorded
// list across the moment its swapchain is destroyed (see
// OnDestroySwapchain).  An OPEN recording whose submissions all completed
// does not block: the one thing that can still happen to it is a replay
// without a Reset, which D3D12 allows and no engine seen so far does.  That
// is the accepted residue of the proof; ResourceReleasable carries the same
// one for a single object.
inline bool RecordedWorkComplete() {
  std::lock_guard<std::mutex> lock(tracker_mutex);
  for (const auto& [key, state] : tracked_lists) {
    for (const auto& [id, generation] : state.generations) {
      if (generation.uses.empty()) continue;
      if (generation.submissions.empty()) {
        if (generation.open) return false;
        continue;
      }
      for (const GenerationSubmission& submission : generation.submissions) {
        if (submission.pending || submission.fence.empty()) return false;
        const uint64_t completed = submission.fence->GetCompletedValue();
        if (completed != UINT64_MAX && completed < submission.value) {
          return false;
        }
      }
    }
  }
  return true;
}

// Telemetry snapshot of the bookkeeping itself (never a lifetime decision):
// a generation count that only grows means some list's recordings are never
// closed (no Reset observed) and everything they used is retained.
struct TrackerStats {
  size_t lists = 0;
  size_t generations = 0;
  size_t open_generations = 0;
  size_t unprovable_submissions = 0;
  size_t live_uses = 0;
  // Lists that could not store an identity (see ListIdentity).  Above zero
  // means some list is keyed by pointer again and its submissions may go
  // unmatched, so the number is reported next to the counts it explains.
  uint64_t identity_fallbacks = 0;
};

inline TrackerStats Snapshot() {
  std::lock_guard<std::mutex> lock(tracker_mutex);
  TrackerStats stats;
  stats.lists = tracked_lists.size();
  stats.live_uses = live_uses.size();
  stats.identity_fallbacks =
      list_identity_fallbacks.load(std::memory_order_relaxed);
  for (const auto& [list, state] : tracked_lists) {
    stats.generations += state.generations.size();
    for (const auto& [id, generation] : state.generations) {
      if (generation.open) ++stats.open_generations;
      for (const GenerationSubmission& submission : generation.submissions) {
        if (!submission.pending && submission.fence.empty()) {
          ++stats.unprovable_submissions;
        }
      }
    }
  }
  return stats;
}

// Teardown / device-generation boundary: forget all bookkeeping. Call only
// when the corresponding resources are being released anyway. Clears the
// sticky arming too: the rebuilt device generation starts GPU-inert again.
inline void ResetAll() {
  std::lock_guard<std::mutex> lock(tracker_mutex);
  tracked_lists.clear();
  identity_by_pointer.clear();
  live_uses.clear();
  ever_tracked_uses.store(false, std::memory_order_release);
}

}  // namespace renodx::addons::dlss5::submission
