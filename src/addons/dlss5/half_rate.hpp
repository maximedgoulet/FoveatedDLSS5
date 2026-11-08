/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Half-rate NR: the motion gate ([RenoDX.DLSS5] NRHalfRate=1).
//
// With half-rate on, the after path runs the NR evaluation on alternate
// frames only and carries the previous frame's edit through the frame in
// between with the game's motion vectors (look_reproject.cs_5_1.hlsl,
// mode 0).  That is only sound while motion is short, so every frame of a
// half-rate workset also measures its motion inside the midpoint zone
// (mode 1) into a 16-byte statistics buffer, and this module copies it into
// a readback ring and decides, a few frames later, whether the workset may
// skip:
//   - a measurement is LONG when more than kFastFractionLimit of the zone's
//     samples move faster than the player's limit (or the field is broken);
//   - a long measurement disarms the workset at once and restarts the hold;
//   - the workset re-arms only after `debounce` consecutive short
//     measurements, so it never re-arms while the latest one is still long;
//   - a workset whose latest measurement is older than kMaxResultAgeTicks is
//     not armed (the readback stalled: never skip on stale evidence).
// The readback latency (1-3 frames) is covered by the reprojection itself:
// a pixel whose own motion exceeds the hard limit gets no edit that frame.
//
// Same completion-proof discipline as edit_trace.hpp: a slot publishes once
// the recording that carried its copy was submitted and completed
// (submission_tracker.hpp) or the every-queue GpuLease landed; the CPU stamp
// in the sample-count word tells a copy that never ran from a real result.
// Every function requires runtime_mutex.

#pragma once

#include <d3d12.h>

#include <algorithm>
#include <cstdint>
#include <unordered_map>

#include "../../utils/directx.hpp"
#include "gpu_lease.hpp"
#include "submission_tracker.hpp"

namespace renodx::addons::dlss5::half_rate {

// The statistics buffer: sum |mv| (1/16 px), fast samples, samples, max |mv|
// (1/16 px).  Allocated at kStatsBytes; the shader touches the first 16.
constexpr uint64_t kStatsBytes = 256;
constexpr uint32_t kStatsWords = 4;
constexpr uint32_t kWordSum = 0;
constexpr uint32_t kWordFast = 1;
constexpr uint32_t kWordSamples = 2;
constexpr uint32_t kWordMax = 3;
// The measurement grid (mode 1): 64 x 36 samples, ~0.2 us of GPU time.
constexpr uint32_t kMeasureGridX = 64;
constexpr uint32_t kMeasureGridY = 36;
constexpr float kFastFractionLimit = 0.05f;
// The hard per-pixel limit of the reprojection, in multiples of the limit.
constexpr float kHardLimitFactor = 4.f;

constexpr uint32_t kSlots = 32;
constexpr uint64_t kSlotStride = 256;
constexpr uint32_t kUnwritten = 0xFFFFFFFFu;
constexpr uint64_t kStaleTicks = 120;
constexpr uint64_t kMaxResultAgeTicks = 8;

struct Gate {
  bool armed = false;
  uint32_t hold = 0;
  bool seeded = false;
  uint64_t last_tick = 0;
  uint64_t measured = 0;
  float mean_px = 0.f;
  float max_px = 0.f;
  float fast_fraction = 0.f;
};

struct Slot {
  bool recorded = false;
  GpuLease lease;
  uint64_t tick = 0;
  uint32_t workset = 0;
};

namespace internal {

inline IUnknown* context_device = nullptr;
inline ID3D12Resource* readback = nullptr;
inline uint8_t* mapped = nullptr;
inline Slot slots[kSlots];
inline uint32_t head = 0;
inline uint32_t tail = 0;
inline uint64_t ticks = 0;
inline uint64_t dropped = 0;
inline uint64_t published = 0;
inline uint64_t skipped_frames = 0;
inline uint64_t nr_frames = 0;
inline std::unordered_map<uint32_t, Gate> gates;

inline void Release() {
  if (mapped != nullptr) readback->Unmap(0, nullptr);
  mapped = nullptr;
  if (readback != nullptr) readback->Release();
  readback = nullptr;
  context_device = nullptr;
  for (Slot& slot : slots) slot = Slot{};
  head = 0;
  tail = 0;
}

inline bool Ensure(ID3D12Device* device) {
  IUnknown* const identity = renodx::utils::directx::NativeIdentity(device);
  if (context_device == identity) return readback != nullptr;
  Release();
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = kSlotStride * kSlots;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
          nullptr, IID_PPV_ARGS(&readback)))
      || readback == nullptr) {
    readback = nullptr;
    return false;
  }
  if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
    mapped = nullptr;
    readback->Release();
    readback = nullptr;
    return false;
  }
  context_device = identity;
  return true;
}

// False for a slot the copy never reached.
inline bool Publish(const Slot& slot, uint32_t debounce) {
  const uint64_t base = static_cast<uint64_t>(&slot - slots) * kSlotStride;
  const auto* words = reinterpret_cast<const uint32_t*>(mapped + base);
  if (words[kWordSamples] == kUnwritten) return false;
  Gate& gate = gates[slot.workset];
  if (!gate.seeded) {
    gate.seeded = true;
    gate.hold = debounce;
  }
  const uint32_t samples = words[kWordSamples];
  gate.mean_px = samples != 0 ? words[kWordSum] / 16.f / samples : 0.f;
  gate.max_px = words[kWordMax] / 16.f;
  gate.fast_fraction = samples != 0 ? static_cast<float>(words[kWordFast]) / samples : 0.f;
  gate.last_tick = ticks;
  ++gate.measured;
  // No sample inside the zone: nothing NR edits can smear - short.
  const bool long_motion = gate.fast_fraction > kFastFractionLimit;
  if (long_motion) {
    gate.armed = false;
    gate.hold = debounce;
  } else if (!gate.armed) {
    if (gate.hold > 0) --gate.hold;
    if (gate.hold == 0) gate.armed = true;
  }
  ++published;
  return true;
}

}  // namespace internal

// Records the readback copy of `stats` (resting in UNORDERED_ACCESS after
// the measure dispatch; returned to it) for `workset`.
inline void Record(
    ID3D12GraphicsCommandList* list, ID3D12Device* device, ID3D12Resource* stats,
    uint32_t workset) {
  using internal::slots;
  if (stats == nullptr || !internal::Ensure(device)) return;
  Slot& slot = slots[internal::head];
  if (slot.recorded) {
    slot.recorded = false;
    ++internal::dropped;
    if (internal::tail == internal::head) internal::tail = (internal::tail + 1) % kSlots;
  }
  const uint64_t base = static_cast<uint64_t>(internal::head) * kSlotStride;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = stats;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  list->ResourceBarrier(1, &barrier);
  list->CopyBufferRegion(internal::readback, base, stats, 0, kStatsWords * sizeof(uint32_t));
  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  list->ResourceBarrier(1, &barrier);
  reinterpret_cast<uint32_t*>(internal::mapped + base)[kWordSamples] = kUnwritten;
  submission::TrackUse(list, &slot);
  slot.lease = AcquireGpuLease();
  slot.tick = internal::ticks;
  slot.workset = workset;
  slot.recorded = true;
  internal::head = (internal::head + 1) % kSlots;
}

// Lifecycle tick, after DrainRetiredResources (edit_trace::Poll's place).
inline void Poll(bool exact_proofs, uint32_t debounce) {
  using internal::slots;
  ++internal::ticks;
  uint32_t scanned = 0;
  while (internal::tail != internal::head && scanned++ < kSlots) {
    Slot& slot = slots[internal::tail];
    if (!slot.recorded) {
      internal::tail = (internal::tail + 1) % kSlots;
      continue;
    }
    const bool exact = exact_proofs && submission::ResourceReleasable(&slot);
    const GpuLeaseState state = exact ? GpuLeaseState::kCompleted : QueryGpuLease(slot.lease);
    if (state == GpuLeaseState::kPending) {
      if (internal::ticks - slot.tick < kStaleTicks) return;
      ++internal::dropped;
    } else if (state == GpuLeaseState::kCompleted) {
      internal::Publish(slot, debounce);
    }
    slot.recorded = false;
    internal::tail = (internal::tail + 1) % kSlots;
  }
}

// May this workset skip the NR evaluation this frame?
inline bool Armed(uint32_t workset) {
  const auto found = internal::gates.find(workset);
  return found != internal::gates.end() && found->second.armed
         && internal::ticks - found->second.last_tick <= kMaxResultAgeTicks;
}

inline Gate Inspect(uint32_t workset) {
  const auto found = internal::gates.find(workset);
  return found != internal::gates.end() ? found->second : Gate{};
}

inline void CountFrame(bool skipped) {
  if (skipped) {
    ++internal::skipped_frames;
  } else {
    ++internal::nr_frames;
  }
}
inline uint64_t SkippedFrames() { return internal::skipped_frames; }
inline uint64_t NrFrames() { return internal::nr_frames; }

// A retired workset's id is never reused for another workset's gate.
inline void Forget(uint32_t workset) { internal::gates.erase(workset); }

inline void ReleaseAll() {
  internal::Release();
  internal::gates.clear();
}

}  // namespace renodx::addons::dlss5::half_rate
