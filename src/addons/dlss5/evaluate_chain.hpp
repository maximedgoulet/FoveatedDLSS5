/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// One NR injection per real DLSS evaluate, decided from the CALL, not from
// the present clock (hardening plan step 3, v6.5.2).
//
// A single game evaluate reaches the addon several times: NGX module copies
// forward into each other (plugin -> nvngx -> core, every copy hooked), the
// C export delegates to the C++ one, and Streamline's slEvaluateFeature runs
// the NGX evaluate inside itself.  All of those are NESTED calls on one
// thread, so the innermost hook that injects marks the output here and every
// enclosing hook sees the mark when the real call returns: a mirror.
//
// The per-present map this replaces asked "was this output processed since
// the last present event".  That is a different question: evaluates are
// recorded on worker threads while Present runs elsewhere, so the next
// frame's evaluate can arrive before the previous frame's present tick and
// be declined as a duplicate (a frame without NR), and a tick that lands
// between an inner and an outer hook clears the map and lets the outer hook
// inject a second time.  The map stays as DedupeMode 0.
//
// Dependency-free so test/dlss5 can include it.

#pragma once

#include <cstdint>

namespace renodx::addons::dlss5 {

inline constexpr uint32_t kDedupePerPresent = 0;
inline constexpr uint32_t kDedupePerEvaluate = 1;

struct EvaluateChain {
  static constexpr uint32_t kMaxOutputs = 8;
  uint32_t depth = 0;
  // A hooked NGX evaluate inside this chain got past the structural gates
  // (a DLSS block with guides and usable geometry): the NGX path owns the
  // stream, whatever this frame's outcome.  The Streamline fallback only
  // engages when no nested NGX evaluate could.
  bool ngx_owned = false;
  uint32_t output_count = 0;
  const void* outputs[kMaxOutputs] = {};

  bool Injected(const void* output) const noexcept {
    for (uint32_t i = 0; i < output_count; ++i) {
      if (outputs[i] == output) return true;
    }
    return false;
  }
  // Past the capacity the mark is dropped: the enclosing hook then injects
  // again rather than silently skipping a pass it cannot prove ran.
  void MarkInjected(const void* output) noexcept {
    if (depth == 0 || Injected(output) || output_count == kMaxOutputs) return;
    outputs[output_count++] = output;
  }
};

inline thread_local EvaluateChain evaluate_chain;

// Opened by every evaluate hook around its real call AND its own injection.
// The chain's marks live exactly as long as the outermost hook.
struct EvaluateChainScope {
  EvaluateChainScope() noexcept { ++evaluate_chain.depth; }
  ~EvaluateChainScope() noexcept {
    if (--evaluate_chain.depth == 0) evaluate_chain = EvaluateChain{};
  }
  EvaluateChainScope(const EvaluateChainScope&) = delete;
  EvaluateChainScope& operator=(const EvaluateChainScope&) = delete;
};

}  // namespace renodx::addons::dlss5
