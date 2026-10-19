/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// GPU-completion leases shared by the retirement paths (dlssnr.hpp) and the
// readback paths (dlssnr.hpp adaptation, screenshot.hpp).
//
// v5.2.2 proved GPU completion with present counts ("four presents means the
// queue flushed"): a blocked or slow GPU can be arbitrarily many CPU presents
// ahead of actual execution, so retired resources could be freed and readback
// buffers mapped while command lists referencing them were still in flight.
// Instead, every tracked command queue carries a signal fence advanced after
// each ExecuteCommandLists (the detour lives in dlssnr.hpp, which owns Detours
// and logging), and a lease records the fence values it must outlive.
//
// A lease never blocks and never allocates a CPU wait: readiness is a poll of
// ID3D12Fence::GetCompletedValue.  An empty lease means "no tracked queue
// could provide a fence". Screenshot diagnostics may settle on a bounded tick
// window (a torn diagnostic plane beats no plane), but meter readbacks are
// skip-until-proven, and resource retirement must treat an empty lease as
// unknown and keep the resource alive until teardown rather than infer GPU
// completion from CPU presents.

#pragma once

#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace renodx::addons::dlss5 {

// Per-queue signal state.  `last_assigned` is the newest fence value this
// process handed to ID3D12CommandQueue::Signal after forwarding the real
// ExecuteCommandLists; the queue's submissions complete in order, so a fence
// that reached value V proves every submission recorded before V finished.
// `faulted` marks a queue whose Signal call failed: its fence can no longer
// prove completions, so new leases skip it (the fence object stays alive for
// leases already holding it). New retirement leases then become conservative:
// without a proof of completion, the guarded resource is retained until teardown.
struct QueueCompletion {
  ID3D12Fence* fence = nullptr;
  std::atomic_uint64_t last_assigned{0};
  bool faulted = false;
  // steady_clock time of the latest submission (telemetry): an idle tracked
  // queue can never satisfy the `last_assigned + 1` a new lease waits for.
  std::atomic<int64_t> last_submit_ns{0};
};

inline std::mutex queue_completion_mutex;
inline std::unordered_map<ID3D12CommandQueue*, QueueCompletion> queue_completions;

// Co-owning fence reference for lease waits (v6 audit issue 14): a lease may
// long outlive the tracker entry (a pending screenshot settles across a
// device rebuild, retired resources drain late), and ReleaseQueueCompletions
// releases the tracker's references - a raw borrowed pointer left a poll of
// ID3D12Fence::GetCompletedValue hitting freed memory on the next lifecycle.
// Each reference is just an AddRef/Release pair; fences stay alive exactly as
// long as some holder still needs to poll them.
struct FenceRef {
  ID3D12Fence* fence = nullptr;

  FenceRef() = default;
  explicit FenceRef(ID3D12Fence* value) : fence(value) {
    if (fence != nullptr) fence->AddRef();
  }
  FenceRef(const FenceRef& other) : fence(other.fence) {
    if (fence != nullptr) fence->AddRef();
  }
  FenceRef& operator=(const FenceRef& other) {
    if (this == &other) return *this;
    if (fence != nullptr) fence->Release();
    fence = other.fence;
    if (fence != nullptr) fence->AddRef();
    return *this;
  }
  FenceRef(FenceRef&& other) noexcept : fence(other.fence) {
    other.fence = nullptr;
  }
  FenceRef& operator=(FenceRef&& other) noexcept {
    if (this == &other) return *this;
    if (fence != nullptr) fence->Release();
    fence = other.fence;
    other.fence = nullptr;
    return *this;
  }
  ~FenceRef() {
    if (fence != nullptr) fence->Release();
  }
  ID3D12Fence* operator->() const { return fence; }
  bool empty() const { return fence == nullptr; }
};

struct GpuLease {
  // (fence, completed value required) per tracked queue; the fence reference
  // is co-owned, see FenceRef.
  std::vector<std::pair<FenceRef, uint64_t>> waits;
  // True only when every queue already known to the tracker had a healthy
  // completion fence at lease acquisition. A faulted/failed known queue makes
  // the lease unproven even if other queue fences are present.
  bool coverage_complete = false;
  bool empty() const { return !coverage_complete || waits.empty(); }
};

// A lease over every tracked queue is conservative (a multi-queue game waits
// for queues that never touched the resource). The required value is the next
// post-ExecuteCommandLists signal (`last_assigned + 1`), so a lease taken while
// recording conservatively waits for the next submission. If no later submit
// occurs, the lease remains pending and retirement keeps the object until
// teardown rather than guessing that the GPU is done.  `ahead = 0` waits only
// for work already submitted (the teardown's bounded drain).
inline GpuLease AcquireGpuLease(uint64_t ahead = 1) {
  GpuLease lease;
  std::lock_guard<std::mutex> lock(queue_completion_mutex);
  if (queue_completions.empty()) return lease;
  lease.coverage_complete = true;
  for (auto& [queue, tracker] : queue_completions) {
    if (tracker.fence != nullptr && !tracker.faulted) {
      lease.waits.emplace_back(
          FenceRef(tracker.fence),
          tracker.last_assigned.load(std::memory_order_relaxed) + ahead);
    } else {
      lease.coverage_complete = false;
    }
  }
  if (lease.waits.empty()) lease.coverage_complete = false;
  return lease;
}

enum class GpuLeaseState {
  kPending,
  kCompleted,
  // GetCompletedValue returned UINT64_MAX: the device was removed.  The GPU
  // will execute nothing more, but that is not proof the guarded work ran -
  // readback consumers (meter stats, screenshots) must discard, while
  // retirement may release safely because a dead device cannot touch the
  // resources again.
  kDeviceRemoved,
};

inline GpuLeaseState QueryGpuLease(const GpuLease& lease) {
  if (!lease.coverage_complete) return GpuLeaseState::kPending;
  for (const auto& [fence, value] : lease.waits) {
    // fence is a FenceRef; a fence whose device was removed reports
    // UINT64_MAX, and co-ownership keeps the object alive to poll (issue 14).
    const uint64_t completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) return GpuLeaseState::kDeviceRemoved;
    if (completed < value) return GpuLeaseState::kPending;
  }
  return GpuLeaseState::kCompleted;
}

// Bounded fallback windows for leases whose tracked fences never advance.
// A ReShade present-starved session (game swapchain without the proxy
// device) stops submissions on the present-time queue that carries the
// tracked fence while the game renders through other queues, so the fence
// proof never lands and an unbounded wait would hang the readback consumer.
// Screenshots settle on the tick window (a torn diagnostic plane beats no
// plane).  The meter NEVER reads on age alone: kMeterLeaseFallbackFrames is
// only the latency after which a stalled fence is reported - reads stay
// skipped until a real completion proof lands, because a torn sample would
// poison the slewed divisor and the normalization state machine, not just
// one image.
inline constexpr uint64_t kCaptureLeaseFallbackTicks = 10;
inline constexpr uint64_t kMeterLeaseFallbackFrames = 30;
// The Stage B norm-stats histogram may settle on this frame-age window when
// its fence proof never lands (v6.0.1, AW2-class field evidence: a starved
// stats feed froze the normalization lock on a garbage estimated factor -
// normtgt 1.7e-4, divisor ~160, model input crushed toward black - because
// the candidate scorer never received a real sample).  The histogram is a
// ~2 Hz whole-frame statistic consumed ONLY by the candidate scorer, never
// by the image divisor path, and a 30-present-old copy cannot be torn; the
// scene-meter reads that DO feed the image path keep the skip-until-proven
// rule above unchanged.
inline constexpr uint64_t kNormStatsFallbackFrames = 30;

// Strict readiness: device removal is NOT a completion.  Callers that may
// release resources on a dead device should branch on QueryGpuLease instead.
inline bool GpuLeaseCompleted(const GpuLease& lease) {
  return QueryGpuLease(lease) == GpuLeaseState::kCompleted;
}

// A destroyed queue can take no submission again, yet its entry stayed in the
// tracker until teardown: every lease taken after the destroy waited for its
// `last_assigned + 1`, which never comes, so retirement and every lease-proven
// readback went unproven for the rest of the session (and a new queue created
// at the recycled address inherited the dead one's fence).  The entry now
// leaves the tracker at destroy_command_queue, and its fence waits here until
// the GPU finished the queue's last signalled submission.  Then the CPU
// signals `last_assigned + 1`, the most any lease asks of this queue: nothing
// can run on it after that point, so leases already holding the fence
// complete instead of starving.  A faulted queue's fence never proved
// anything and is released at once.
struct RetiredQueueFence {
  ID3D12Fence* fence = nullptr;  // the tracker's reference, moved here
  uint64_t last_assigned = 0;
};
inline std::vector<RetiredQueueFence> retired_queue_fences;

// destroy_command_queue, with the queue's native pointer.  False when the
// queue was never tracked.
inline bool ForgetQueueCompletion(ID3D12CommandQueue* queue) {
  std::lock_guard<std::mutex> lock(queue_completion_mutex);
  const auto entry = queue_completions.find(queue);
  if (entry == queue_completions.end()) return false;
  QueueCompletion& tracker = entry->second;
  if (tracker.fence != nullptr) {
    if (tracker.faulted) {
      tracker.fence->Release();
    } else {
      retired_queue_fences.push_back(
          {tracker.fence, tracker.last_assigned.load(std::memory_order_relaxed)});
    }
  }
  queue_completions.erase(entry);
  return true;
}

// Lifecycle tick: settles the fences of destroyed queues whose last signalled
// submission completed (or whose device was removed).
inline void SettleRetiredQueueFences() {
  std::lock_guard<std::mutex> lock(queue_completion_mutex);
  std::erase_if(retired_queue_fences, [](const RetiredQueueFence& retired) {
    const uint64_t completed = retired.fence->GetCompletedValue();
    if (completed != UINT64_MAX && completed < retired.last_assigned) return false;
    if (completed != UINT64_MAX) retired.fence->Signal(retired.last_assigned + 1);
    retired.fence->Release();
    return true;
  });
}

// Teardown (Shutdown): releases the TRACKER's fence references and forgets
// the queues. Leases already handed out co-own their fences (FenceRef), so a
// holder that still needs to poll - a pending screenshot, an undrained
// retirement - keeps a live fence instead of a dangling pointer.
inline void ReleaseQueueCompletions() {
  std::lock_guard<std::mutex> lock(queue_completion_mutex);
  for (auto& [queue, tracker] : queue_completions) {
    if (tracker.fence != nullptr) tracker.fence->Release();
  }
  queue_completions.clear();
  for (const RetiredQueueFence& retired : retired_queue_fences) retired.fence->Release();
  retired_queue_fences.clear();
}

}  // namespace renodx::addons::dlss5
