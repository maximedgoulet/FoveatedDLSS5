/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Per-frame NR edit trace ([RenoDX.DLSS5] NREditTrace=1, PLAN_NR_LOOK_V71.md
// section 3 I).
//
// The look stage reshapes the network's edit e = log2(n / p), and no control
// can be tuned before real games say what that edit looks like.  DLSSNR is a
// Neural Rendering enhancer; nothing here denoises - what is measured is the
// enhancement the model applied.  Per traced pass look_trace.cs_5_1.hlsl
// reduces the RAW edit (the pass's untouched NR output, whether or not the
// look then reshapes it) at NR resolution into two surfaces, and this module
// copies both into a readback ring:
//   - the histogram, look::kTraceWords uint32: 128 bins of a over [-4, 4)
//     stops at 1/16, then the pixel count, the |a| > 1 count and the halo
//     count (darkened by over 0.1 stop, more than 2 stops below the block's
//     brightest input);
//   - the block surface, one RGBA32F texel per 32x32 NR block: mean a,
//     mean I, mean |c_r|_w, mean |c_t|_w.
// Once a completion proof lands it writes one CSV row per frame
// (RenoDX-DLSS5-edittrace.csv) and, per (workset, pass), one summary line per
// second: a's percentiles over the window's summed histogram, the |a| > 1 and
// halo fractions, flicker, and the pixel-weighted mean chroma edits.  The
// summary line is a frozen contract, pinned character for character in
// tools/field/log_verdict.py (EDIT_TRACE_FORMAT).
//
// Flicker is the block-level approximation of the plan's "mean
// |a_B,t - a_B,t-1| on static pixels": the mean |delta block-mean a| over the
// blocks whose block-mean I moved less than kStaticStops between consecutive
// published frames of one (workset, pass) at unchanged NR dims.  Per block it
// is a lower bound of the per-pixel measure (|mean d| <= mean |d|), and a
// block counts as static by its mean input, not pixel by pixel - read it as a
// trend, not as the plan's number.
//
// Observe-only and off by default: the copies ride the game's command list
// right after look::RecordTrace, and a slot publishes on a completion proof
// polled from the lifecycle tick (never a CPU wait, never a present count
// taken as proof): the exact one - the recording that carried the copies was
// submitted and that submission completed (submission_tracker.hpp) - wherever
// the queue hooks are live, else the every-queue GpuLease, which starves
// while any tracked queue idles (the norm trace's rc3 Alan Wake 2 run, 27 of
// 18713 frames).  Neither proof says the copy ran - a recording reset
// unsubmitted is "released" too - so Record stamps the slot's pixel-count
// word with kUnwritten and a slot still carrying it is counted as
// unexecuted, not published.  A slot whose proof never lands is dropped
// unread.  Nothing read back here feeds the image path.  Every function
// requires runtime_mutex.

#pragma once

#include <d3d12.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <include/reshade.hpp>

#include "../../utils/directx.hpp"
#include "gpu_lease.hpp"
#include "look_stage.hpp"
#include "submission_tracker.hpp"

namespace renodx::addons::dlss5::edit_trace {

constexpr uint32_t kSlots = 16;
// Ring capacity sized for an 8K NR grid: ceil(7680/32) x ceil(4320/32) blocks
// of one R32G32B32A32 texel each.  Larger grids are skipped (logged once).
constexpr uint32_t kMaxBlocksX = 240;
constexpr uint32_t kMaxBlocksY = 135;
constexpr uint64_t kPitchAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
constexpr uint64_t kPlacementAlign = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
constexpr uint64_t kHistogramBytes = look::kTraceWords * sizeof(uint32_t);
constexpr uint64_t kBlockOffset =
    ((kHistogramBytes + kPlacementAlign - 1) / kPlacementAlign) * kPlacementAlign;
constexpr uint64_t kSlotStride =
    ((kBlockOffset
      + ((kMaxBlocksX * 16ull + kPitchAlign - 1) / kPitchAlign) * kPitchAlign
            * kMaxBlocksY
      + kPlacementAlign - 1)
     / kPlacementAlign)
    * kPlacementAlign;
// The histogram's words after the bins (look_trace.cs_5_1.hlsl).
constexpr uint32_t kWordPixels = look::kTraceBins;
constexpr uint32_t kWordOver1 = look::kTraceBins + 1;
constexpr uint32_t kWordHalo = look::kTraceBins + 2;
// Written by the CPU over a slot's pixel-count word at Record; the copy
// replaces it with the grid's pixel count, which an 8K grid keeps below 2^25.
constexpr uint32_t kUnwritten = 0xFFFFFFFFu;
// Lifecycle ticks a slot may wait for its completion proof before it is
// dropped unread (~2 s of presents).
constexpr uint64_t kStaleTicks = 120;
// A block whose mean input moved less than this between two frames is static
// for the flicker measure.
constexpr float kStaticStops = 0.05f;
constexpr double kQuantiles[3] = {0.05, 0.50, 0.95};
constexpr int64_t kSummaryNs = 1'000'000'000;

struct Meta {
  uint64_t generation = 0;
  int64_t time_ns = 0;
  uint32_t workset = 0;
  uint32_t pass = 0;       // 1-based stack pass
  uint32_t nr_width = 0;   // the NR grid the trace reduced
  uint32_t nr_height = 0;
};

struct Slot {
  bool recorded = false;
  GpuLease lease;
  uint64_t tick = 0;
  Meta meta;
  uint32_t blocks_x = 0;
  uint32_t blocks_y = 0;
  uint32_t row_pitch = 0;
};

// What a histogram says, for one frame or for a window's sum.
struct HistogramStats {
  float percentiles[3] = {0.f, 0.f, 0.f};  // kQuantiles, stops
  double over1_pct = 0.0;
  double halo_pct = 0.0;
};

// Accumulators for one (workset, pass) summary line.
struct Window {
  int64_t start_ns = 0;
  uint64_t frames = 0;
  uint64_t words[look::kTraceWords] = {};  // the summed histogram
  double c_weight = 0.0;                   // pixels behind the two sums below
  double c_r_sum = 0.0;
  double c_t_sum = 0.0;
  double flicker_sum = 0.0;                // sum of per-pair flicker means
  uint64_t flicker_pairs = 0;
};

// One (workset, pass): its window, and the previous published frame's block
// means (a, I interleaved) at its NR dims, which outlive the window.
struct Track {
  Window window;
  uint32_t previous_width = 0;
  uint32_t previous_height = 0;
  std::vector<float> previous;
};

namespace internal {

// Native identity, compared only - norm_trace's and gpu_timers' reasoning:
// the evaluate's device face can change without the device changing, and
// the readback buffer (never proxied) may still be the target of a copy.
inline IUnknown* context_device = nullptr;
inline ID3D12Resource* readback = nullptr;
inline uint8_t* mapped = nullptr;
inline Slot slots[kSlots];
inline uint32_t head = 0;
inline uint32_t tail = 0;
inline uint64_t ticks = 0;
inline uint64_t dropped = 0;
inline uint64_t published = 0;
inline uint64_t unexecuted = 0;
inline uint64_t lease_starved = 0;
inline bool warned_stale = false;
inline bool warned_capacity = false;
inline std::filesystem::path csv_path;
inline std::ofstream csv;
inline int64_t csv_origin_ns = 0;
inline int64_t last_flush_ns = 0;
inline std::map<std::pair<uint32_t, uint32_t>, Track> tracks;

inline void Log(reshade::log::level level, const std::string& message) {
  reshade::log::message(level, ("DLSS5 Generic: " + message).c_str());
}

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

// Percentiles are the centre of the first bin whose cumulative count reaches
// q * total, exactly percentile() in tools/look/look_reference.py.
inline HistogramStats Summarize(const uint64_t (&words)[look::kTraceWords]) {
  HistogramStats stats;
  const uint64_t total = words[kWordPixels];
  if (total == 0) return stats;
  uint64_t running = 0;
  uint32_t found = 0;
  for (uint32_t bin = 0; bin < look::kTraceBins && found < 3; ++bin) {
    running += words[bin];
    while (found < 3
           && static_cast<double>(running)
                  >= kQuantiles[found] * static_cast<double>(total)) {
      stats.percentiles[found++] =
          look::kTraceLow + (static_cast<float>(bin) + 0.5f) * look::kTraceStep;
    }
  }
  for (; found < 3; ++found) {
    stats.percentiles[found] =
        look::kTraceLow
        + (static_cast<float>(look::kTraceBins) - 0.5f) * look::kTraceStep;
  }
  stats.over1_pct = 100.0 * words[kWordOver1] / static_cast<double>(total);
  stats.halo_pct = 100.0 * words[kWordHalo] / static_cast<double>(total);
  return stats;
}

// False for a slot the copy never reached (counted in `unexecuted`).
inline bool Publish(const Slot& slot) {
  const uint64_t base = static_cast<uint64_t>(&slot - slots) * kSlotStride;
  const auto* histogram = reinterpret_cast<const uint32_t*>(mapped + base);
  if (histogram[kWordPixels] == kUnwritten) {
    ++unexecuted;
    return false;
  }
  uint64_t words[look::kTraceWords];
  std::copy(histogram, histogram + look::kTraceWords, words);
  const HistogramStats stats = Summarize(words);

  Track& track = tracks[{slot.meta.workset, slot.meta.pass}];
  const bool paired = track.previous_width == slot.meta.nr_width
                      && track.previous_height == slot.meta.nr_height;
  track.previous.resize(2ull * slot.blocks_x * slot.blocks_y);
  double c_weight = 0.0;
  double c_r_sum = 0.0;
  double c_t_sum = 0.0;
  double flicker_sum = 0.0;
  uint64_t static_blocks = 0;
  for (uint32_t y = 0; y < slot.blocks_y; ++y) {
    const auto* row = reinterpret_cast<const float*>(
        mapped + base + kBlockOffset
        + static_cast<uint64_t>(y) * slot.row_pitch);
    const uint32_t rows =
        std::min(look::kTraceBlock, slot.meta.nr_height - y * look::kTraceBlock);
    for (uint32_t x = 0; x < slot.blocks_x; ++x) {
      const float* block = row + x * 4;
      float* previous = track.previous.data()
                        + 2ull * (static_cast<uint64_t>(y) * slot.blocks_x + x);
      // Edge blocks are partial: weight each by the pixels it averaged.
      const double pixels = static_cast<double>(
          rows
          * std::min(look::kTraceBlock, slot.meta.nr_width - x * look::kTraceBlock));
      if (std::isfinite(block[2]) && std::isfinite(block[3])) {
        c_weight += pixels;
        c_r_sum += pixels * block[2];
        c_t_sum += pixels * block[3];
      }
      // A non-finite mean on either side makes the step non-finite, and a
      // NaN input step fails the static test.
      const float step = std::fabs(block[0] - previous[0]);
      if (paired && std::fabs(block[1] - previous[1]) < kStaticStops
          && std::isfinite(step)) {
        flicker_sum += step;
        ++static_blocks;
      }
      previous[0] = block[0];
      previous[1] = block[1];
    }
  }
  track.previous_width = slot.meta.nr_width;
  track.previous_height = slot.meta.nr_height;
  const double flicker =
      static_blocks != 0 ? flicker_sum / static_cast<double>(static_blocks) : 0.0;
  ++published;

  if (!csv.is_open() && !csv_path.empty()) {
    csv.open(csv_path, std::ios::trunc);
    if (csv.is_open()) {
      csv_origin_ns = slot.meta.time_ns;
      csv << "t_s,generation,workset,pass,nr_w,nr_h,pixels,p05,p50,p95,"
             "over1_pct,halo_pct,c_r,c_t,flicker\n";
    }
  }
  if (csv.is_open()) {
    // Blank flicker: no previous frame at these dims, or no static block.
    char flicker_text[32] = "";
    if (static_blocks != 0) {
      std::snprintf(flicker_text, sizeof(flicker_text), "%.6f", flicker);
    }
    char row[320];
    std::snprintf(
        row, sizeof(row),
        "%.3f,%llu,%u,%u,%u,%u,%llu,%.5f,%.5f,%.5f,%.4f,%.4f,%.6f,%.6f,%s\n",
        (slot.meta.time_ns - csv_origin_ns) / 1e9,
        static_cast<unsigned long long>(slot.meta.generation),
        slot.meta.workset, slot.meta.pass, slot.meta.nr_width,
        slot.meta.nr_height, static_cast<unsigned long long>(words[kWordPixels]),
        stats.percentiles[0], stats.percentiles[1], stats.percentiles[2],
        stats.over1_pct, stats.halo_pct,
        c_weight > 0.0 ? c_r_sum / c_weight : 0.0,
        c_weight > 0.0 ? c_t_sum / c_weight : 0.0, flicker_text);
    csv << row;
    if (slot.meta.time_ns - last_flush_ns > 1'000'000'000) {
      csv.flush();
      last_flush_ns = slot.meta.time_ns;
    }
  }

  Window& w = track.window;
  if (w.frames == 0) w.start_ns = slot.meta.time_ns;
  ++w.frames;
  for (uint32_t i = 0; i < look::kTraceWords; ++i) w.words[i] += words[i];
  w.c_weight += c_weight;
  w.c_r_sum += c_r_sum;
  w.c_t_sum += c_t_sum;
  if (static_blocks != 0) {
    w.flicker_sum += flicker;
    ++w.flicker_pairs;
  }
  if (slot.meta.time_ns - w.start_ns >= kSummaryNs) {
    const HistogramStats summary = Summarize(w.words);
    char line[512];
    std::snprintf(
        line, sizeof(line),
        "edit trace ws%u pass%u: frames=%llu a[p05=%.3f p50=%.3f p95=%.3f"
        " over1=%.1f%%] halo=%.2f%% flicker=%.4fst c_r=%.4fst c_t=%.4fst"
        " dropped=%llu",
        slot.meta.workset, slot.meta.pass,
        static_cast<unsigned long long>(w.frames), summary.percentiles[0],
        summary.percentiles[1], summary.percentiles[2], summary.over1_pct,
        summary.halo_pct,
        w.flicker_pairs != 0
            ? w.flicker_sum / static_cast<double>(w.flicker_pairs)
            : 0.0,
        w.c_weight > 0.0 ? w.c_r_sum / w.c_weight : 0.0,
        w.c_weight > 0.0 ? w.c_t_sum / w.c_weight : 0.0,
        static_cast<unsigned long long>(dropped));
    Log(reshade::log::level::info, line);
    // The previous frame's blocks stay with the track across windows.
    w = Window{};
  }
  return true;
}

}  // namespace internal

inline void Configure(const std::filesystem::path& csv_path) {
  internal::csv_path = csv_path;
}

// Records the readback copies onto `list` right after look::RecordTrace:
// `histogram` is the raw buffer of look::kTraceWords uint32, `blocks` the
// RGBA32F texture of ceil(nr/32) texels; both rest in UNORDERED_ACCESS and
// are returned to it.
inline void Record(
    ID3D12GraphicsCommandList* list,
    ID3D12Device* device,
    ID3D12Resource* histogram,
    ID3D12Resource* blocks,
    const Meta& meta) {
  using internal::slots;
  const uint32_t blocks_x =
      (meta.nr_width + look::kTraceBlock - 1) / look::kTraceBlock;
  const uint32_t blocks_y =
      (meta.nr_height + look::kTraceBlock - 1) / look::kTraceBlock;
  if (blocks_x > kMaxBlocksX || blocks_y > kMaxBlocksY) {
    if (!internal::warned_capacity) {
      internal::warned_capacity = true;
      internal::Log(reshade::log::level::warning,
                    "edit trace: NR grid larger than 8K; frames not traced");
    }
    return;
  }
  if (blocks_x == 0 || blocks_y == 0 || histogram == nullptr
      || blocks == nullptr || !internal::Ensure(device)) {
    return;
  }
  Slot& slot = slots[internal::head];
  if (slot.recorded) {
    // Lapping an undrained slot: drop the oldest rather than overwrite a
    // region the GPU may still be copying into.
    slot.recorded = false;
    ++internal::dropped;
    if (internal::tail == internal::head) {
      internal::tail = (internal::tail + 1) % kSlots;
    }
  }
  const uint64_t base = static_cast<uint64_t>(internal::head) * kSlotStride;
  const uint32_t row_pitch = static_cast<uint32_t>(
      ((blocks_x * 16ull + kPitchAlign - 1) / kPitchAlign) * kPitchAlign);

  D3D12_RESOURCE_BARRIER barriers[2]{};
  barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[0].Transition.pResource = histogram;
  barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barriers[1] = barriers[0];
  barriers[1].Transition.pResource = blocks;
  list->ResourceBarrier(2, barriers);

  list->CopyBufferRegion(internal::readback, base, histogram, 0, kHistogramBytes);
  D3D12_TEXTURE_COPY_LOCATION destination{};
  destination.pResource = internal::readback;
  destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destination.PlacedFootprint.Offset = base + kBlockOffset;
  destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  destination.PlacedFootprint.Footprint.Width = blocks_x;
  destination.PlacedFootprint.Footprint.Height = blocks_y;
  destination.PlacedFootprint.Footprint.Depth = 1;
  destination.PlacedFootprint.Footprint.RowPitch = row_pitch;
  D3D12_TEXTURE_COPY_LOCATION source{};
  source.pResource = blocks;
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  // Only this frame's grid: the surface may be allocated for a larger one.
  const D3D12_BOX grid{0, 0, 0, blocks_x, blocks_y, 1};
  list->CopyTextureRegion(&destination, 0, 0, 0, &source, &grid);

  std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
  std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
  list->ResourceBarrier(2, barriers);

  // The slot is its own tracked object, as in norm_trace::Record; the CPU
  // stamp lands before this list can be submitted.
  reinterpret_cast<uint32_t*>(internal::mapped + base)[kWordPixels] = kUnwritten;
  submission::TrackUse(list, &slot);
  slot.lease = AcquireGpuLease();
  slot.tick = internal::ticks;
  slot.meta = meta;
  slot.blocks_x = blocks_x;
  slot.blocks_y = blocks_y;
  slot.row_pitch = row_pitch;
  slot.recorded = true;
  internal::head = (internal::head + 1) % kSlots;
}

// Lifecycle-tick side, after DrainRetiredResources pruned the tracker:
// publishes slots whose completion proof landed, in order, and drops slots
// whose proof can never land.  `exact_proofs`: the queue hooks are live, so
// the tracker's release of a slot is the proof (queue_tracking_active).
inline void Poll(bool exact_proofs) {
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
    const GpuLeaseState lease = QueryGpuLease(slot.lease);
    const GpuLeaseState state = exact ? GpuLeaseState::kCompleted : lease;
    if (state == GpuLeaseState::kPending) {
      if (internal::ticks - slot.tick < kStaleTicks) return;
      ++internal::dropped;
      if (!internal::warned_stale) {
        internal::warned_stale = true;
        internal::Log(
            reshade::log::level::warning,
            "edit trace: a slot's completion proof never landed (its list was"
            " neither submitted nor reset, or the queue hooks are not live and"
            " a tracked queue idles); dropping it unread");
      }
    } else if (state == GpuLeaseState::kCompleted) {
      // Counted: the frames only the exact proof could publish.
      if (internal::Publish(slot) && lease == GpuLeaseState::kPending) {
        ++internal::lease_starved;
      }
    }
    slot.recorded = false;
    internal::tail = (internal::tail + 1) % kSlots;
  }
}

inline uint64_t Published() { return internal::published; }
inline uint64_t Dropped() { return internal::dropped; }
inline uint64_t Unexecuted() { return internal::unexecuted; }
inline uint64_t LeaseStarved() { return internal::lease_starved; }

// Device teardown / shutdown: pending slots are abandoned (their device is
// going away) and the CSV is flushed.
inline void ReleaseAll() {
  internal::Release();
  internal::tracks.clear();
  if (internal::csv.is_open()) internal::csv.flush();
}

}  // namespace renodx::addons::dlss5::edit_trace
