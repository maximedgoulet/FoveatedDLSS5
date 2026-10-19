/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Where a capture window's state lives.
//
// ComputeStateEnvelope held a CapturedComputeState BY VALUE, so it sat on the
// stack of whatever thread the game chose to call the evaluate on.  That is
// 18,016 bytes (measured; test/dlss5 pins it), and 17,920 of them are the
// root-argument array: kMaxRootParameters (64) entries each carrying
// kMaxRootConstantDwords (64) DWORDs.  D3D12 caps a WHOLE root signature at
// 64 DWORDs - "The maximum size of a root signature is 64 DWORDs", descriptor
// tables and root constants 1 DWORD each, root descriptors 2 (MS, Root
// Signature Limits) - so that product is 64x anything a legal signature can
// hold.  Making the storage proportionate is worth doing and is its own
// change; this one only stops the game's thread paying for it.
//
// What it cost there, measured on the T2 `small_stack` profile:
//
//     no addon at all        64 KB  OK
//     addon loaded, NR OFF   64 KB  STACK_OVERFLOW (0xC00000FD)
//     addon loaded, NR ON    64 KB  STACK_OVERFLOW
//     addon loaded, either   72 KB  OK
//
// 64 KB is the smallest reserve Windows will give a thread, and job- and
// fiber-based engines use sizes in that region.  DLSS on its own fits; DLSS
// with this addon loaded did not - and the cost was paid with NR SWITCHED
// OFF, so turning the feature off did not get such an engine running.  The
// archived v6.5.3 gives the same two numbers, so this is a property of the
// line and not something the rehab introduced (PLAN_REHAB_V7.md R8).
//
// The state has no reason to be on that stack: it is scoped to the envelope
// and never outlives it, so a per-thread pool serves it exactly as well.  The
// pool is a stack of its own because capture windows nest - a plugin NGX
// module forwarding into the detoured core export re-enters the hooks on the
// same thread - and it is allocated once per thread that actually evaluates,
// never resized, so the pointers it hands out stay valid.
//
// Deeper than the pool, or a pool that could not be allocated, takes a
// one-off from the heap rather than declining: losing a frame of NR to save
// 18 KB would be the wrong trade, and the counter says when it happened.
//
// Dependency-free so test/dlss5 can exercise the nesting and the overflow.

#pragma once

#include <atomic>
#include <cstdint>
#include <new>

#include "command_state.hpp"

namespace renodx::addons::dlss5 {

// Two is the shape seen so far (a plugin module forwarding into the core
// export); the pre-SR envelope is sequential with the after-upscale one, not
// nested inside it.  Eight leaves room for a chain nobody has met yet without
// making the per-thread block large: 8 x 18 KB is ~141 KB, taken only by
// threads that actually run an evaluate.  `capture_arena_high_water` says
// what the field really reaches, so this number is evidence-led next time.
inline constexpr unsigned kCaptureArenaSlots = 8;

// Session diagnostics, reported in the telemetry line.
inline std::atomic_uint32_t capture_arena_high_water{0};
inline std::atomic_uint64_t capture_arena_overflows{0};
// A release that was not the slot most recently acquired.  Envelopes are
// scoped objects, so this is zero by construction; a non-zero value means
// something now owns one that is not, and the pool would quietly hand the
// same slot to two windows if it were trusted.
inline std::atomic_uint64_t capture_arena_out_of_order{0};

namespace capture_arena {

struct Storage {
  CapturedComputeState* slots = nullptr;
  unsigned depth = 0;
  ~Storage() { delete[] slots; }
  Storage() = default;
  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;
};

inline thread_local Storage storage;

}  // namespace capture_arena

// Never returns a slot that is in use.  Returns nullptr only if the heap is
// exhausted, and the caller treats that as "no capture" - which declines the
// evaluate rather than injecting without a restore target.
inline CapturedComputeState* AcquireCaptureSlot() noexcept {
  capture_arena::Storage& arena = capture_arena::storage;
  if (arena.slots == nullptr) {
    arena.slots =
        new (std::nothrow) CapturedComputeState[kCaptureArenaSlots];
  }
  if (arena.slots != nullptr && arena.depth < kCaptureArenaSlots) {
    CapturedComputeState* slot = &arena.slots[arena.depth++];
    // A pooled slot is reused, so it must start where a fresh stack object
    // would: every knowledge bit false.  Carrying a previous window's bits
    // would be the worst possible defect here - it would open the injection
    // gate on a restore target this window never observed.
    // Constructed in place: `*slot = CapturedComputeState{}` built the 18 KB
    // blank on this frame first, and a frame over one page is probed page by
    // page on every call, so each acquire committed 18 KB of the caller's
    // stack inside NGX's own evaluate (dlss5_e2e_small_stack: 65,536-69,632
    // bytes against a 64 KB job/fiber reserve).  Trivially destructible.
    ::new (static_cast<void*>(slot)) CapturedComputeState{};
    unsigned seen = capture_arena_high_water.load(std::memory_order_relaxed);
    while (arena.depth > seen
           && !capture_arena_high_water.compare_exchange_weak(
               seen, arena.depth, std::memory_order_relaxed)) {
    }
    return slot;
  }
  capture_arena_overflows.fetch_add(1, std::memory_order_relaxed);
  return new (std::nothrow) CapturedComputeState{};
}

inline void ReleaseCaptureSlot(CapturedComputeState* slot) noexcept {
  if (slot == nullptr) return;
  capture_arena::Storage& arena = capture_arena::storage;
  const bool pooled = arena.slots != nullptr && slot >= arena.slots
      && slot < arena.slots + kCaptureArenaSlots;
  if (!pooled) {
    delete slot;
    return;
  }
  if (arena.depth != 0 && slot == &arena.slots[arena.depth - 1]) {
    --arena.depth;
    return;
  }
  // Not the top of the stack: leave the depth alone rather than free a slot
  // an outer window is still recording into.  The thread loses a slot until
  // it unwinds, which is recoverable; handing it out twice would not be.
  capture_arena_out_of_order.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace renodx::addons::dlss5
