/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

namespace renodx::addons::dlss5 {

// Stack-slot reset decision for the v5.3 reset-epoch model.
//
// v5.2.2 used one global Boolean latch consumed by the final stack pass:
//
//   do_reset = (!chained && slot_index != 0)      // stateless passes 2+
//           || slot.pending_reset                 // once per new slot
//           || frame.frame_reset != 0             // game flagged Reset
//           || (last_pass ? latch.exchange(false) // consume on final pass
//                         : latch.load());
//
// In the default stacked configuration (chained history off) the first term
// is always true for passes 2+, so the short-circuit never reached
// exchange(false); the latch stuck true forever after the first creation or
// config change, and pass 1 - which the comment above it promised keeps its
// temporal history - reset on every frame for the rest of the session.
//
// The epoch model removes the consumed flag entirely: a single monotonic
// global epoch advances whenever a reset-worthy event affects every stream
// (scene cut signaled globally, codec/style/configuration change, startup).
// Each stack slot records the epoch it has acknowledged; a slot needs a
// reset when it has not yet acknowledged the epoch resolved for the current
// chain.  Nothing is consumed, so the same epoch correctly resets every
// stream exactly once, a stream's own contract changes (handled through the
// slot's pending_reset) cannot consume or steal another stream's reset, and
// no expression ordering can wedge the mechanism.
//
// The chain epoch must be resolved once before recording a chain and passed
// to every pass of that chain, so a mid-chain advance cannot split one
// chain's resets across two epochs.
inline bool StackSlotNeedsReset(
    bool chained_history,
    uint32_t slot_index,
    bool pending_reset,
    int32_t frame_reset,
    uint64_t acknowledged_epoch,
    uint64_t chain_epoch) {
  return pending_reset
      || frame_reset != 0
      || acknowledged_epoch != chain_epoch
      || (!chained_history && slot_index != 0);
}

}  // namespace renodx::addons::dlss5
