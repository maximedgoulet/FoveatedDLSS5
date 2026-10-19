/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Opt-in per-stage GPU timestamps around the injected NR chain
// ([RenoDX.DLSS5] NRGpuTimers).  Diagnostic only - when disabled the frame
// path pays nothing, and when enabled it pays a handful of EndQuery calls plus
// one ResolveQueryData per evaluate.
//
// Mechanism: every injected evaluate writes up to kSlotCount TIMESTAMP
// EndQuery markers into one segment of a ringed query heap and resolves its
// segment into a persistently-mapped READBACK buffer on the game's own command
// list (ResolveQueryData executes on the GPU with the list - no CPU wait).
// Completion is polled from the per-present lifecycle tick: the exact proof -
// the recording that carried the resolve was submitted and that submission
// completed (submission_tracker.hpp) - wherever the queue hooks are live, else
// the every-queue fence lease (gpu_lease.hpp), which starves while any tracked
// queue idles (norm_trace.hpp, rc4).  End stamps the segment's frame-start
// texel with kUnwritten, so a segment the resolve never reached is counted
// as unexecuted instead of publishing a lap-old timing.  A segment whose
// proof never lands is dropped unread after a bounded tick window (torn
// timing is worthless, but a blocked GPU must not wedge diagnostics).
// Everything below is guarded by runtime_mutex: Begin/Mark/End run inside the
// evaluate hook's lock, Poll runs in RunLifecycleTick's lock.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include <d3d12.h>

#include <include/reshade.hpp>

#include "../../utils/directx.hpp"
#include "gpu_lease.hpp"
#include "submission_tracker.hpp"

namespace renodx::addons::dlss5::gpu_timers {

// Marker slots within one evaluated chain.  Stages that do not run on a given
// workset (copy-in is after-path only, linearize/commit are HDR-only) simply
// stay absent: `written` has a bit per slot the chain wrote, and every delta
// below re-checks presence against it.  (Presence by "below the highest slot
// written" read unwritten slots - another evaluate's timestamps - and the
// pre-SR chain published linearize+scale 2052279 ms.)
constexpr uint32_t kSlotFrameStart = 0;
constexpr uint32_t kSlotCopyInEnd = 1;
constexpr uint32_t kSlotLinearizeEnd = 2;
constexpr uint32_t kSlotPassBase = 3;
// Per stack pass: encode start/end, NGX evaluate start/end, resolve end, and
// the end of the NR look stage, which runs between the evaluate and the
// resolve and marks only when it recorded work.
constexpr uint32_t kSlotPerPass = 6;
constexpr uint32_t kTimerMaxPasses = 4;
constexpr uint32_t kSlotCommitStart =
    kSlotPassBase + kSlotPerPass * kTimerMaxPasses;
constexpr uint32_t kSlotCommitEnd = kSlotCommitStart + 1;
constexpr uint32_t kSlotCopyBackEnd = kSlotCommitEnd + 1;
constexpr uint32_t kSlotCount = kSlotCopyBackEnd + 1;

// 64 slots per segment keeps the readback stride at 512 bytes, the alignment
// ResolveQueryData requires for its buffer offset.
constexpr uint32_t kSlotsPerEval = 64;
// Evaluations that may be awaiting their completion proof at once.  At one
// (or two, DLSS+RR) evaluates per frame this is several frames of slack even
// with the present thread stalled.
constexpr uint32_t kSegmentDepth = 32;
constexpr uint32_t kHeapSlots = kSlotsPerEval * kSegmentDepth;
// Lifecycle ticks a segment may stay pending before it is dropped unread
// (~1 s of presents; the starvation fallback keeps ticks flowing when
// presents themselves stop).
constexpr uint64_t kStaleTicks = 64;
// Written by the CPU over a segment's frame-start texel at End; the resolve
// replaces it with a timestamp.
constexpr uint64_t kUnwritten = UINT64_MAX;
constexpr char kLogPrefix[] = "DLSS5 Generic";

constexpr uint32_t PassEncodeStart(uint32_t pass) {
  return kSlotPassBase + pass * kSlotPerPass;
}
constexpr uint32_t PassEncodeEnd(uint32_t pass) { return PassEncodeStart(pass) + 1; }
constexpr uint32_t PassEvalStart(uint32_t pass) { return PassEncodeStart(pass) + 2; }
constexpr uint32_t PassEvalEnd(uint32_t pass) { return PassEncodeStart(pass) + 3; }
constexpr uint32_t PassResolveEnd(uint32_t pass) { return PassEncodeStart(pass) + 4; }
constexpr uint32_t PassLookEnd(uint32_t pass) { return PassEncodeStart(pass) + 5; }
static_assert(kSlotCount <= kSlotsPerEval);

inline void SafeLog(reshade::log::level level, const std::string& message) {
  reshade::log::message(
      level, (std::string(kLogPrefix) + ": " + message).c_str());
}

struct Segment {
  uint32_t base = 0;   // first heap slot (== index * kSlotsPerEval)
  uint32_t count = 0;  // highest slot written + 1 (the resolve range)
  uint64_t written = 0;  // bit per slot written (kSlotsPerEval == 64)
  bool recorded = false;
  bool bypass = false;  // zero-strength passthrough frame: no stats
  bool lease_taken = false;
  GpuLease lease;
  uint64_t tick = 0;
  char path[8] = {};
  uint32_t width = 0;
  uint32_t height = 0;
};

// Latest published per-evaluate timing, in microseconds.  Guarded by
// runtime_mutex; the overlay copies it under that lock.
struct Stats {
  uint32_t total_us = 0;
  uint32_t own_us = 0;  // total minus the model evaluates
  uint32_t eval_us = 0;
  uint32_t encode_us = 0;
  uint32_t resolve_us = 0;
  uint32_t look_us = 0;  // part of resolve_us: the look stage's own span
  uint32_t linearize_us = 0;
  uint32_t commit_us = 0;
  uint32_t copies_us = 0;
  char path[8] = {};
  uint32_t width = 0;
  uint32_t height = 0;
  bool valid = false;
};
inline Stats stats;
inline uint64_t dropped_segments = 0;
inline uint64_t published_segments = 0;
inline uint64_t unexecuted_segments = 0;
inline uint64_t lease_starved_segments = 0;

namespace internal {

inline Segment segments[kSegmentDepth];
inline uint32_t head = 0;  // next segment Begin will claim
inline uint32_t tail = 0;  // oldest segment awaiting its proof
// The device's native identity (utils::directx::NativeIdentity), compared
// only.  The raw pointer is whichever face the evaluate's list reports -
// ReShade's device proxy or the native device - and a change of face is not a
// change of device: releasing the query heap there would free it under a
// resolve the GPU may still be executing.  Query heaps and readback buffers
// are never proxied, so the ones created through either face serve both.
inline IUnknown* context_device = nullptr;
inline ID3D12QueryHeap* heap = nullptr;
inline ID3D12Resource* readback = nullptr;
inline uint64_t* mapped = nullptr;
inline double frequency = 0.0;
inline std::atomic_uint64_t poll_ticks{0};
inline std::atomic<int64_t> last_log_ns{0};
inline std::atomic_bool stale_warned{false};

struct EvalState {
  bool active = false;
  uint32_t idx = 0;
};
// No nesting: ProcessInline is serialized by runtime_mutex and the internal
// stack-pass evaluates never re-enter the injection path.
inline EvalState current;

inline void Release() {
  if (mapped != nullptr) {
    readback->Unmap(0, nullptr);
    mapped = nullptr;
  }
  if (readback != nullptr) {
    readback->Release();
    readback = nullptr;
  }
  if (heap != nullptr) {
    heap->Release();
    heap = nullptr;
  }
  context_device = nullptr;
  frequency = 0.0;
  head = 0;
  tail = 0;
  current.active = false;
  for (Segment& s : segments) s = Segment{};
  stats.valid = false;
}

inline bool Ensure(ID3D12Device* device) {
  IUnknown* const identity = renodx::utils::directx::NativeIdentity(device);
  if (context_device == identity) return heap != nullptr;
  Release();
  D3D12_QUERY_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  heap_desc.Count = kHeapSlots;
  if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&heap)))) {
    heap = nullptr;
    return false;
  }
  D3D12_HEAP_PROPERTIES rb_heap{};
  rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC rb_desc{};
  rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rb_desc.Width = static_cast<UINT64>(kHeapSlots) * sizeof(uint64_t);
  rb_desc.Height = 1;
  rb_desc.DepthOrArraySize = 1;
  rb_desc.MipLevels = 1;
  rb_desc.SampleDesc.Count = 1;
  rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(
          &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) {
    readback = nullptr;
    heap->Release();
    heap = nullptr;
    return false;
  }
  if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
    mapped = nullptr;
    readback->Release();
    readback = nullptr;
    heap->Release();
    heap = nullptr;
    return false;
  }
  // GPU timestamp frequency is reported per queue; direct queues on one
  // adapter report one value in practice, so a throwaway queue on the same
  // device calibrates the conversion.
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ID3D12CommandQueue* queue = nullptr;
  if (SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) {
    UINT64 rate = 0;
    if (SUCCEEDED(queue->GetTimestampFrequency(&rate)) && rate > 0) {
      frequency = static_cast<double>(rate);
    }
    queue->Release();
  }
  context_device = identity;
  return true;
}

// False for a segment the resolve never reached (counted as unexecuted).
inline bool Publish(const Segment& s) {
  if (frequency <= 0.0 || mapped == nullptr) return false;
  const uint64_t* q = mapped + s.base;
  if (q[kSlotFrameStart] == kUnwritten) {
    ++unexecuted_segments;
    return false;
  }
  ++published_segments;
  const double inv = 1000000.0 / frequency;  // ticks -> microseconds
  const auto delta_us = [&](uint32_t a, uint32_t b) -> uint32_t {
    if ((s.written >> a & 1) == 0 || (s.written >> b & 1) == 0 || q[b] <= q[a]) {
      return 0;
    }
    return static_cast<uint32_t>((q[b] - q[a]) * inv + 0.5);
  };

  Stats next;
  next.total_us = delta_us(kSlotFrameStart, kSlotCopyBackEnd);
  if (next.total_us == 0) return true;
  next.copies_us = delta_us(kSlotFrameStart, kSlotCopyInEnd)
                   + delta_us(kSlotCommitEnd, kSlotCopyBackEnd);
  next.linearize_us = delta_us(kSlotCopyInEnd, kSlotLinearizeEnd);
  if ((s.written >> kSlotCopyInEnd & 1) == 0) {
    // Pre-SR has no copy-in: the linearize span starts at the frame mark.
    next.linearize_us = delta_us(kSlotFrameStart, kSlotLinearizeEnd);
  }
  for (uint32_t pass = 0; pass < kTimerMaxPasses; ++pass) {
    next.encode_us += delta_us(PassEncodeStart(pass), PassEncodeEnd(pass));
    next.eval_us += delta_us(PassEvalStart(pass), PassEvalEnd(pass));
    next.resolve_us += delta_us(PassEvalEnd(pass), PassResolveEnd(pass));
    next.look_us += delta_us(PassEvalEnd(pass), PassLookEnd(pass));
  }
  next.commit_us = delta_us(kSlotCommitStart, kSlotCommitEnd);
  next.own_us = next.total_us - next.eval_us;
  next.width = s.width;
  next.height = s.height;
  std::memcpy(next.path, s.path, sizeof(next.path));
  next.valid = true;
  stats = next;

  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  int64_t last = last_log_ns.load(std::memory_order_relaxed);
  if (now_ns - last < 5000000000LL
      || !last_log_ns.compare_exchange_strong(last, now_ns,
                                              std::memory_order_relaxed)) {
    return true;
  }
  SafeLog(
      reshade::log::level::info,
      "NR gpu timers [" + std::string(s.path) + " " + std::to_string(s.width)
          + "x" + std::to_string(s.height)
          + "]: total " + std::to_string(next.total_us / 1000.0)
          + "ms, model " + std::to_string(next.eval_us / 1000.0)
          + "ms, own overhead " + std::to_string(next.own_us / 1000.0)
          + "ms (encode " + std::to_string(next.encode_us / 1000.0)
          + ", resolve " + std::to_string(next.resolve_us / 1000.0)
          + ", linearize+scale " + std::to_string(next.linearize_us / 1000.0)
          + ", commit " + std::to_string(next.commit_us / 1000.0)
          + ", copies+park " + std::to_string(next.copies_us / 1000.0) + ")");
  // The look stage on its own line, so the line above keeps its shape for
  // every reader (analyze_longrun.py, the e2e lanes); its span is inside
  // "resolve" there.
  if (next.look_us != 0) {
    SafeLog(
        reshade::log::level::info,
        "NR gpu timers look [" + std::string(s.path) + " " + std::to_string(s.width) + "x"
            + std::to_string(s.height) + "]: look stage "
            + std::to_string(next.look_us / 1000.0) + "ms, inside resolve "
            + std::to_string(next.resolve_us / 1000.0) + "ms");
  }
  return true;
}

}  // namespace internal

// Begins one timed chain on the game's command list.  Returns false (and
// records nothing) when the timer context cannot be created on this device -
// diagnostics never block the image path.
inline bool Begin(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Device* device,
    const char* path,
    uint32_t width,
    uint32_t height) {
  if (!internal::Ensure(device)) return false;
  Segment& s = internal::segments[internal::head];
  if (s.recorded) {
    // Lapping an undrained segment (a pathologically deep evaluate backlog):
    // drop the oldest rather than overwrite a segment the GPU may still be
    // resolving.
    s.recorded = false;
    ++dropped_segments;
    if (internal::tail == internal::head) {
      internal::tail = (internal::tail + 1) % kSegmentDepth;
    }
  }
  s.base = internal::head * kSlotsPerEval;
  s.count = 0;
  s.bypass = false;
  s.lease_taken = false;
  s.lease = GpuLease{};
  s.tick = internal::poll_ticks.load(std::memory_order_relaxed);
  std::strncpy(s.path, path, sizeof(s.path) - 1);
  s.path[sizeof(s.path) - 1] = '\0';
  s.width = width;
  s.height = height;
  internal::current.active = true;
  internal::current.idx = internal::head;
  command_list->EndQuery(
      internal::heap, D3D12_QUERY_TYPE_TIMESTAMP, s.base + kSlotFrameStart);
  s.count = kSlotFrameStart + 1;
  s.written = uint64_t{1} << kSlotFrameStart;
  return true;
}

inline void Mark(ID3D12GraphicsCommandList* command_list, uint32_t slot) {
  if (!internal::current.active || slot >= kSlotsPerEval) return;
  Segment& s = internal::segments[internal::current.idx];
  command_list->EndQuery(
      internal::heap, D3D12_QUERY_TYPE_TIMESTAMP, s.base + slot);
  if (slot >= s.count) s.count = slot + 1;
  s.written |= uint64_t{1} << slot;
}

// Flags the current segment as a zero-strength passthrough frame: it resolves
// and frees the ring slot, but never publishes stats (the chain recorded no
// work worth timing).
inline void MarkBypassed() {
  if (!internal::current.active) return;
  internal::segments[internal::current.idx].bypass = true;
}

// Resolves the current segment on the list it was recorded with, tracks the
// segment as a use of that recording (the exact proof) and takes the
// completion lease (the fallback, conservatively requiring the next
// submission on every tracked queue, like resource retirement).
//
// An empty lease is not kept.  The queue tracker creates a queue's entry at
// that queue's first submission carrying injected work, so the FIRST timed
// evaluate of a session - and of every D3D11 bridge session - ends before any
// queue is known, and its lease covers nothing and can never complete.  Until
// alpha45 End kept it anyway: that segment pinned the ring, the ring lapped,
// and the timers published nothing in any session from v6.1.1 on.  Poll takes
// the lease once a queue is known.
inline void End(ID3D12GraphicsCommandList* command_list) {
  if (!internal::current.active) return;
  internal::current.active = false;
  Segment& s = internal::segments[internal::current.idx];
  if (s.count < 2) return;  // no measurable span; the slot is reusable
  // Only the slots this chain wrote.  A chain writes a sparse set - one stack
  // pass leaves the other passes' slots unwritten, a pass without look work
  // its look slot - and resolving a query that was never performed is invalid
  // D3D12 ("Cannot Resolve query that has never been performed"): through
  // alpha47 every timed evaluate with fewer than four passes raised it once
  // per unwritten slot under the debug layer.  Each run of written slots
  // resolves on its own; the readers skip unwritten slots by `written`.
  for (uint32_t first = 0; first < s.count;) {
    if ((s.written >> first & 1) == 0) {
      ++first;
      continue;
    }
    uint32_t end = first + 1;
    while (end < s.count && (s.written >> end & 1) != 0) ++end;
    command_list->ResolveQueryData(
        internal::heap, D3D12_QUERY_TYPE_TIMESTAMP, s.base + first, end - first,
        internal::readback, static_cast<UINT64>(s.base + first) * sizeof(uint64_t));
    first = end;
  }
  // The segment is its own tracked object, as in norm_trace::Record; the CPU
  // stamp lands before this list can be submitted.
  internal::mapped[s.base + kSlotFrameStart] = kUnwritten;
  submission::TrackUse(command_list, &s);
  s.lease = AcquireGpuLease();
  s.lease_taken = !s.lease.empty();
  s.tick = internal::poll_ticks.load(std::memory_order_relaxed);
  s.recorded = true;
  internal::head = (internal::head + 1) % kSegmentDepth;
}

// Lifecycle-tick side (runtime_mutex held), after DrainRetiredResources
// pruned the tracker: publishes every segment whose completion proof has
// landed and drops segments whose proof can never land.  `exact_proofs`: the
// queue hooks are live, so the tracker's release of a segment is the proof
// (queue_tracking_active).
inline void Poll(bool exact_proofs) {
  internal::poll_ticks.fetch_add(1, std::memory_order_relaxed);
  uint32_t scanned = 0;
  while (scanned++ < kSegmentDepth) {
    Segment& s = internal::segments[internal::tail];
    if (!s.recorded) {
      // tail == head means empty only when the tail segment is idle: a lap
      // (Begin dropping the oldest) leaves tail == head on a FULL ring, and
      // reading that as empty is what wedged the timers before alpha45.
      if (internal::tail == internal::head) return;
      internal::tail = (internal::tail + 1) % kSegmentDepth;
      continue;
    }
    if (!s.lease_taken) {
      s.lease = AcquireGpuLease();
      s.lease_taken = !s.lease.empty();
    }
    const bool exact = exact_proofs && submission::ResourceReleasable(&s);
    const GpuLeaseState lease = QueryGpuLease(s.lease);
    switch (exact ? GpuLeaseState::kCompleted : lease) {
      case GpuLeaseState::kCompleted:
        // Counted: the segments only the exact proof could publish.
        if (!s.bypass && internal::Publish(s) && lease == GpuLeaseState::kPending) {
          ++lease_starved_segments;
        }
        s.recorded = false;
        internal::tail = (internal::tail + 1) % kSegmentDepth;
        break;
      case GpuLeaseState::kDeviceRemoved:
        s.recorded = false;
        internal::tail = (internal::tail + 1) % kSegmentDepth;
        break;
      case GpuLeaseState::kPending:
        if (internal::poll_ticks.load(std::memory_order_relaxed) - s.tick
            < kStaleTicks) {
          return;  // oldest outstanding segment still pending; keep order
        }
        s.recorded = false;
        ++dropped_segments;
        if (!internal::stale_warned.exchange(true)) {
          SafeLog(
              reshade::log::level::warning,
              "NR gpu timers: a segment's completion proof never landed (its"
              " list was neither submitted nor reset, or the queue hooks are"
              " not live and a tracked queue idles); dropping its timing"
              " sample");
        }
        internal::tail = (internal::tail + 1) % kSegmentDepth;
        break;
    }
  }
}

// Device teardown / shutdown.
inline void ReleaseAll() { internal::Release(); }

}  // namespace renodx::addons::dlss5::gpu_timers
