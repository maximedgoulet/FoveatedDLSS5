/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace renodx::addons::dlss5 {

// Compute root-argument shadow model for the v5.3 host-state restoration.
//
// D3D12 bounds a root signature to 64 parameters / 64 DWORDs of cost, so
// fixed-size per-parameter tracking covers every legal root signature.
constexpr unsigned int kMaxRootParameters = 64;
constexpr unsigned int kMaxRootConstantDwords = 64;

// One compute root parameter slot.  kNone models "not observed" separately
// from a null/zero binding: a parameter the host never bound within the
// capture window is not replayed (restoring an unobserved value would bind
// garbage, not the host's state).
enum class RootArgumentKind : uint8_t {
  kNone,
  kTable,
  kConstants,
  kConstantBufferView,
  kShaderResourceView,
  kUnorderedAccessView,
};

struct RootArgumentState {
  RootArgumentKind kind = RootArgumentKind::kNone;
  uint64_t address = 0;  // descriptor-table GPU handle or CBV/SRV/UAV address
  uint32_t constants[kMaxRootConstantDwords] = {};
  uint64_t constant_mask = 0;  // bit i = constants[i] holds an observed value

  void Reset() {
    kind = RootArgumentKind::kNone;
    address = 0;
    constant_mask = 0;
  }

  void SetView(RootArgumentKind view_kind, uint64_t view_address) {
    kind = view_kind;
    address = view_address;
    constant_mask = 0;
  }

  // Merges one SetComputeRoot32BitConstants call into the observed-DWORD
  // mask; partial updates coexist, and a later full replay restores exactly
  // the DWORDs the host had bound.  Clipped to the fixed capacity.
  void MergeConstants(
      uint32_t dword_offset, uint32_t dword_count, const uint32_t* values) {
    if (dword_count == 0 || dword_offset >= kMaxRootConstantDwords) return;
    kind = RootArgumentKind::kConstants;
    const uint32_t clipped =
        std::min(dword_count, kMaxRootConstantDwords - dword_offset);
    std::memcpy(
        constants + dword_offset, values, clipped * sizeof(uint32_t));
    constant_mask |=
        (clipped == kMaxRootConstantDwords
             ? ~0ull
             : ((1ull << clipped) - 1ull) << dword_offset);
  }

  // Finds the next contiguous run of observed DWORDs at or after `offset`;
  // replay walks these so partial updates merge back into exactly the
  // observed set.  Returns false when no observed DWORD remains.
  bool NextConstantRun(
      uint32_t offset, uint32_t* run_offset, uint32_t* run_length) const {
    uint32_t cursor = offset;
    while (cursor < kMaxRootConstantDwords
        && (constant_mask & (1ull << cursor)) == 0) {
      ++cursor;
    }
    if (cursor >= kMaxRootConstantDwords) return false;
    uint32_t length = 1;
    while (cursor + length < kMaxRootConstantDwords
        && (constant_mask & (1ull << (cursor + length))) != 0) {
      ++length;
    }
    *run_offset = cursor;
    *run_length = length;
    return true;
  }
};

}  // namespace renodx::addons::dlss5
