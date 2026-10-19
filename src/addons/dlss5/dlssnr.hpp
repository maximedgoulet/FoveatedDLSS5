/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "debug.hpp"

#include <d3d12.h>
#include <detours.h>
#include <nvsdk_ngx.h>
#include <sl_core_api.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cwctype>
#include <sl.h>
#include <sl_dlss.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include "lastgasp.hpp"
#include <embed/shaders.h>

#include "../../utils/directx.hpp"
#include "../../utils/float16.hpp"
#include "../../utils/path.hpp"
#include "../../utils/vtable.hpp"
// Helpers the snapshot expects but public RenoDX main lacks (see the file).
#include "renodx_compat.hpp"
#include "arg_plausibility.hpp"
#include "bridge_params.hpp"
#include "cmd_slots.hpp"
#include "codec_gain.hpp"
#include "foveation.hpp"
#include "declines.hpp"
#include "direct_call.hpp"
#include "gpu_lease.hpp"
#include "reset_epoch.hpp"
#include "root_arguments.hpp"
#include "runtime_lock.hpp"
#include "capture_arena.hpp"
#include "command_state.hpp"
#include "evaluate_chain.hpp"
#include "funnel.hpp"
#include "ui/state_card.hpp"
#include "ui/strings.hpp"
#include "ui/widgets.hpp"
#include "submission_tracker.hpp"
#include "screenshot.hpp"
#include "report_bundle.hpp"
#include "report_upload.hpp"
#include "edit_trace.hpp"
#include "gpu_timers.hpp"
#include "look_stage.hpp"
#include "norm_trace.hpp"
#include "telemetry.hpp"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "version.lib")

// The NGX C++ parameter interface is sufficient for the signed feature DLL and
// keeps this addon independent of the public nvngx import library.
#define DLSS5_NGX_SET_F(parameters, name, value) \
  (parameters)->Set((name), static_cast<float>(value))
#define DLSS5_NGX_SET_UI(parameters, name, value) \
  (parameters)->Set((name), static_cast<unsigned int>(value))
#define DLSS5_NGX_SET_I(parameters, name, value) \
  (parameters)->Set((name), static_cast<int>(value))
#define DLSS5_NGX_SET_VOIDP(parameters, name, value) \
  (parameters)->Set((name), (value))
#define DLSS5_NGX_GET_F(parameters, name, value) (parameters)->Get((name), (value))
#define DLSS5_NGX_GET_UI(parameters, name, value) (parameters)->Get((name), (value))
#define DLSS5_NGX_GET_I(parameters, name, value) (parameters)->Get((name), (value))

#define NVSDK_NGX_Parameter_SetF DLSS5_NGX_SET_F
#define NVSDK_NGX_Parameter_SetUI DLSS5_NGX_SET_UI
#define NVSDK_NGX_Parameter_SetI DLSS5_NGX_SET_I
#define NVSDK_NGX_Parameter_SetVoidPointer DLSS5_NGX_SET_VOIDP
#define NVSDK_NGX_Parameter_GetF DLSS5_NGX_GET_F
#define NVSDK_NGX_Parameter_GetUI DLSS5_NGX_GET_UI
#define NVSDK_NGX_Parameter_GetI DLSS5_NGX_GET_I

namespace renodx::addons::dlss5 {
namespace internal {

// The v6 gain math lives in renodx::dlss5::codec (codec_gain.hpp) so the
// unit tests can include it without the addon's D3D12/NGX dependencies.
namespace codec = ::renodx::dlss5::codec;

constexpr NVSDK_NGX_Feature kFeatureDlss = NVSDK_NGX_Feature_SuperSampling;
constexpr NVSDK_NGX_Feature kFeatureDlssd = NVSDK_NGX_Feature_RayReconstruction;
constexpr NVSDK_NGX_Feature kFeatureDlssNr = NVSDK_NGX_Feature_Reserved18;
static_assert(static_cast<int>(kFeatureDlssNr) == 18);
constexpr unsigned long long kDirectApplicationId = 0x876232cULL;
constexpr char kConfigSection[] = "RenoDX.DLSS5";
// Dedicated overlay window title (Add-ons tab).  Registering a named overlay
// instead of the nullptr generic callback keeps the NR UI in its own window,
// separate from every other add-on's section.
constexpr char kOverlayTitle[] = "DLSS 5 Neural Rendering";
// Config schema marker stamped into [RenoDX.DLSS5].  Bump it whenever a
// default changes or a key changes meaning: LoadConfiguration treats a stored
// marker below this as a stale config, applies the defaults namespace instead
// of the stored values, and re-persists.  1 = pre-v5.0 schema (classic codec
// default, no marker).  2 = v5.0 schema (anchored codec default).  3 = v5.2.2
// schema (classic codec default again - bumps 2 because the stored
// NRCodecMode=1 that v5.0 persisted must not outlive the default change).
// v4 (v5.3): wholesale stale-config reset replaced by targeted key-wise
// migration of the inherited NRCodecMode/NRChainedHistory defaults, with a
// timestamped ReShade.ini backup taken before the first migration write.
// v5 (v6): Auto codec (NRCodecMode=2) becomes the default; NRPQCalibration
// gains legacy-semantics migration (2.5375 -> old ~515-nit effective divisor,
// 1 -> 203-nit reference); new source-interpretation override keys
// (NRSourceEncoding/NRSourcePrimaries/NRLinearUnitNits).  Manual Classic (0)
// and Anchored (1) selections are preserved; only defaultless/fresh configs
// land on Auto.
// v6 (v6.3): the Stable normalization governor (NRNormGovernor=2) becomes the
// default; a stored 1 (the inherited v6.1.2 default) migrates, a stored 0
// (explicit Off) is preserved.
// v8: read v8-only settings through rc10-compatible migration rules.
constexpr uint32_t kConfigVersion = 8;
constexpr char kAddonVersion[] = "v7.5.0-rc5";
// SHA-256 of the reference signed NR runtime build.  Identification only: a
// mismatch is reported in the log/overlay but never blocks loading, because
// swapping in a custom runtime build is a supported diagnostics workflow.
constexpr char kExpectedNrSha256[] =
    "E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E";

using GetModuleFileNameWFn = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);
using DirectInitFn = NVSDK_NGX_Result(NVSDK_CONV*)(
    unsigned long long,
    const wchar_t*,
    ID3D12Device*,
    NVSDK_NGX_Version,
    const NVSDK_NGX_Parameter*);
using DirectCreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(
    ID3D12GraphicsCommandList*,
    NVSDK_NGX_Feature,
    const NVSDK_NGX_Parameter*,
    NVSDK_NGX_Handle**);
using DirectEvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(
    ID3D12GraphicsCommandList*,
    const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*,
    PFN_NVSDK_NGX_ProgressCallback);
using DirectReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using DirectShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
using AllocateParametersFn =
    NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using DestroyParametersFn =
    NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);

struct DirectApi {
  HMODULE module = nullptr;
  AllocateParametersFn allocate = nullptr;
  DestroyParametersFn destroy = nullptr;
  DirectInitFn init = nullptr;
  DirectCreateFn create = nullptr;
  DirectEvaluateFn evaluate = nullptr;
  DirectReleaseFn release = nullptr;
  DirectShutdownFn shutdown = nullptr;
};

enum class DirectLoadState : uint8_t {
  Unknown,
  Ready,
  Failed,
};

// NR stacking: up to kMaxNrPasses chained feature-18 evaluations per frame.
// Every pass owns its own NR feature handle - a single handle evaluated N
// times per frame would advance its internal temporal recursion N times per
// frame (double motion integration -> ghosting), while separate handles keep
// the 1:1 evaluate-per-frame history each pass was trained for.
constexpr uint32_t kMaxNrPasses = 4;
// Contract-debounce window: a new NR working-resolution contract must hold
// for this many consecutive frames before NR features and the workset are
// recreated (see FeatureState::debounce_* and ProcessInline).
constexpr uint32_t kContractDebounceFrames = 30;

// The NR look stage's temporal history (look_stage.hpp, group E): a ping-pong
// pair of raw buffers of look::kHistoryBytesPerPixel per NR pixel.  It
// belongs to the NGX handle, as the model's own history does, so worksets
// that share a handle (a game rotating its outputs) filter one stream.  The
// buffers rest in COMMON and are promoted to UNORDERED_ACCESS by each use
// (buffers promote implicitly); a UAV barrier orders a frame's read after
// the previous frame's write.  Allocated while Stabilize is on.
struct LookHistory {
  ID3D12Resource* buffers[2] = {};
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t written = 0;  // the buffer the last frame wrote
  bool valid = false;
  uint64_t epoch = 0;
  int64_t last_ns = 0;
};

// One NR feature instance (handle + parameter block + latches).  Slot 0 is
// the original single-pass NR feature; slots 1..kMaxNrPasses-1 exist only
// while NR stacking is enabled.
struct NrFeatureSlot {
  NVSDK_NGX_Handle* handle = nullptr;
  NVSDK_NGX_Parameter* parameters = nullptr;  // Owned through the initialized NGX core.
  bool via_core = false;
  uint8_t nr_hdr_mode = 0;  // hdr_mode the NR feature was created with
  bool failed = false;
  // Steady-clock time at which `failed` was latched; the slot retries
  // automatically once the backoff interval expires (see ProcessInline).
  int64_t failed_ns = 0;
  // Consecutive failure-retry cycles: 0 = first failure, retried on the
  // next evaluate; then 1, 2, 4 ... s capped at 30 s, so a transient
  // failure costs one frame while a permanently rejected contract backs
  // off to the slow interval.
  uint32_t fail_count = 0;
  uint64_t configuration_generation = 0;
  // Set at slot creation and consumed by the slot's first evaluate so a newly
  // created stacked pass starts from clean history without touching the
  // global reset epoch (a new slot must never reset older slots or another
  // stream's history).
  bool pending_reset = false;
  // The queue tracker was live when this handle was created, so every
  // recording that used it is a tracked submission use (see
  // RetiredNrFeature::tracker_proof).
  bool tracked_since_create = false;
  // Reset-epoch acknowledgement (v5.3): the global history_reset_epoch value
  // this slot has already reset at.  Replaces the v5.2.2 global Boolean
  // latch, whose stacked short-circuit never reached the consuming
  // exchange() and stuck the latch true (see reset_epoch.hpp).
  uint64_t acknowledged_reset_epoch = 0;
  // DLSSNR.Reset as this slot's latest evaluate sent it: the look history
  // restarts exactly when the model's own history does.
  bool model_reset = false;
  // The model steering this slot last LOGGED: the six panel values followed
  // by independent-skin mode and the two values sent to the runtime. NaN
  // until the first evaluate, so that one always logs.
  float logged_model[9] = {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};
  int64_t model_logged_ns = 0;
  LookHistory look_history;
};

// Where a relative-HDR frame's NR input scale comes from (NRFeedMode; the
// values are v6_exposure_scale's Source and NRNormTrace's feed state).
enum class FeedSource : uint32_t {
  kV1 = 0,       // the content meter (v6_autoscale + governor)
  kTexture = 1,  // v2: PreExposure / (ExposureTexture * ExposureScale)
  kFixed = 2,    // v2: 1, a pre-exposed buffer is the tonemapper input
  kHold = 3,     // v2, shader only: keep the committed scale this frame
};

struct FeatureState {
  NVSDK_NGX_Feature source_feature = kFeatureDlss;
  uint32_t input_width = 0;
  uint32_t input_height = 0;
  uint32_t output_width = 0;
  uint32_t output_height = 0;
  // The guide dimensions as captured at CreateFeature/lazy registration.
  // input_width/height above are REWRITTEN every frame with the applied NR
  // working resolution, so they can no longer serve as the create-contract
  // fallback for titles that only send guide dims at create time.
  uint32_t create_input_width = 0;
  uint32_t create_input_height = 0;
  // Foveated grid (foveation.hpp) of the last evaluate: a change of warp
  // re-registers the working grid, so the after path resets the temporal
  // histories when it moves (the size, if it changes, recreates as usual).
  foveation::Grid foveation{};
  uint32_t motion_x = 0;
  uint32_t motion_y = 0;
  uint32_t depth_x = 0;
  uint32_t depth_y = 0;
  uint32_t create_flags = 0;
  // Motion scales are stored presence-flagged: an absent scale must not read
  // as the zero the raw Get would leave, and its dimension-derived default is
  // only resolved once the final guide dims are known (see ProcessInline).
  float motion_scale_x = 0.f;
  float motion_scale_y = 0.f;
  bool has_motion_scale_x = false;
  bool has_motion_scale_y = false;
  // Per-frame jitter in render pixels; absent stays absent (no create-time
  // fallback - create jitter is meaningless for later frames).
  float jitter_x = 0.f;
  float jitter_y = 0.f;
  bool has_jitter = false;
  int32_t perf_quality = 0;
  int32_t frame_reset = 0;
  // Engine-managed exposure contract: the game supplies an ExposureTexture,
  // declares DLSS.Pre.Exposure, or set the AutoExposure create flag.  Such a
  // source already normalizes brightness frame-to-frame; the codec must not
  // re-normalize (no metered slew - scene darkness is intentional).
  bool has_exposure_texture = false;
  bool has_pre_exposure = false;
  float pre_exposure = 0.f;
  // The game resource this stream's previous evaluate owned a workset for
  // (EnsureWorkset; compared only, never dereferenced).
  const ID3D12Resource* last_workset_key = nullptr;
  // Raw evidence preserves the exposure contract for diagnostics.
  codec::ExposureEvidence exposure;
  bool ExposureManaged() const {
    return has_exposure_texture || has_pre_exposure
        || (create_flags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) != 0;
  }
  // Feed Auto's evidence latch (stored stream state only): set once this
  // stream sends a pre-exposure other than 1.  The NGX helpers send 1 when
  // the engine has none, so presence alone proves nothing; a single real
  // value does, and latching keeps a value that passes through 1 from
  // switching the feed back and forth.
  bool pre_exposure_seen = false;
  // The feed last logged for this stream (FeedSource), UINT32_MAX = none.
  uint32_t logged_feed = UINT32_MAX;
  // Contract-debounce state: a NEW working-resolution contract must persist
  // for kContractDebounceFrames consecutive frames before the NR features and
  // workset are recreated.  Slider commits, follow-input toggles, and
  // game-side resolution changes then cost exactly one rebuild, and titles
  // that alternate contracts every frame (MSFS 2024-class second viewports)
  // never trigger one at all - the documented recreate-storm/VRAM-churn
  // failure mode.  Only touched under runtime_mutex.
  uint32_t debounce_width = 0;
  uint32_t debounce_height = 0;
  uint32_t debounce_frames = 0;
  // False until this stream has applied a working resolution once.  The first
  // evaluation commits immediately instead of running the debounce window on
  // stale stored render dims (the startup double-workset recreation).
  bool working_resolution_committed = false;
  NrFeatureSlot slots[kMaxNrPasses];
};

struct StreamlineResourceRef {
  ID3D12Resource* resource = nullptr;
  uint32_t state = UINT_MAX;
  sl::Extent extent{};
  bool owns_reference = false;

  StreamlineResourceRef() = default;
  StreamlineResourceRef(const StreamlineResourceRef& other)
      : resource(other.resource), state(other.state), extent(other.extent) {}
  StreamlineResourceRef& operator=(const StreamlineResourceRef& other) {
    if (this == &other) return *this;
    Reset();
    resource = other.resource;
    state = other.state;
    extent = other.extent;
    return *this;
  }
  StreamlineResourceRef(StreamlineResourceRef&& other) noexcept
      : resource(other.resource),
        state(other.state),
        extent(other.extent),
        owns_reference(other.owns_reference) {
    other.resource = nullptr;
    other.owns_reference = false;
  }
  StreamlineResourceRef& operator=(StreamlineResourceRef&& other) noexcept {
    if (this == &other) return *this;
    Reset();
    resource = other.resource;
    state = other.state;
    extent = other.extent;
    owns_reference = other.owns_reference;
    other.resource = nullptr;
    other.owns_reference = false;
    return *this;
  }
  ~StreamlineResourceRef() { Reset(); }

  void Reset() {
    if (owns_reference && resource != nullptr) resource->Release();
    resource = nullptr;
    owns_reference = false;
  }
};

struct StreamlineViewportState {
  std::unordered_map<sl::BufferType, StreamlineResourceRef> resources;
  uint32_t frame = 0;
};

struct StreamlineCapture {
  StreamlineResourceRef color;
  StreamlineResourceRef output;
  StreamlineResourceRef motion;
  StreamlineResourceRef depth;
  uint32_t viewport = 0;
  uint32_t frame = 0;
  bool has_viewport = false;
  bool has_constants = false;
  bool hdr = false;
  bool has_options = false;
  bool depth_inverted = false;
  bool reset = false;
  float motion_scale_x = 1.f;
  float motion_scale_y = 1.f;
  float jitter_x = 0.f;
  float jitter_y = 0.f;
};

// GPU-completion leases (gpu_lease.hpp) drive retirement and readback
// readiness; the queue tracker detour that advances them lives with the
// other command-queue hooks below.

struct RetiredNrFeature {
  NVSDK_NGX_Handle* handle = nullptr;
  NVSDK_NGX_Parameter* parameters = nullptr;
  bool via_core = false;
  // Retirement age is diagnostic only. GPU completion is proven by `lease`;
  // an empty lease is intentionally retained until teardown.
  uint64_t generation = 0;
  GpuLease lease;
  // Exact submission-use proof, as for worksets (issue 01): every create and
  // evaluate recorded against `handle` is a tracked use, so release waits for
  // exactly those submissions.  The every-queue `lease` above never completes
  // while any tracked queue sits idle (a loading-only or destroyed queue), so
  // on its own it retained every retired feature - one full NR model
  // allocation per settings change or game DLSS rebuild - until teardown.
  bool tracker_proof = false;
  // The slot's look history (LookHistory), retired with its handle, or on
  // its own (handle null) when Stabilize goes off.  Every use is tracked.
  ID3D12Resource* look_history[2] = {};
};

struct CodecPipeline {
  ID3D12RootSignature* root_signature = nullptr;
  ID3D12PipelineState* encode = nullptr;
  ID3D12PipelineState* decode = nullptr;
  ID3D12PipelineState* block_mean_reduce = nullptr;
  ID3D12PipelineState* black_restore_apply = nullptr;
  // The two restore reduces built with MEASURE_UNSHAPED: while the look
  // shapes a pass they measure NR's own lift from the unshaped model output,
  // so the restore no longer takes back the look's change.  Optional - a
  // failed creation keeps the classic reduce.
  ID3D12PipelineState* block_mean_reduce_unshaped = nullptr;
  ID3D12PipelineState* pedestal_reduce_unshaped = nullptr;
  // v6 linear-working-space passes (HDR worksets).  They share the legacy
  // root signature: each shader family declares its own cbuffer view over the
  // same 28 root-constant dwords (112 bytes; dword 24 is Foveation in every
  // view - the v6 dword map is
  // V6CodecConstants, mirrored by v6_common.hlsli), so one signature serves
  // both.
  ID3D12PipelineState* linearize = nullptr;
  ID3D12PipelineState* encode_v6 = nullptr;
  ID3D12PipelineState* resolve_v6 = nullptr;
  ID3D12PipelineState* pedestal_reduce = nullptr;
  ID3D12PipelineState* commit = nullptr;
  ID3D12PipelineState* autoscale = nullptr;
  // Feed v2: the exposure-anchored scale and the commit whose dark gate is
  // in NR-input units.  Optional: without both, every frame runs v1.
  ID3D12PipelineState* exposure_scale = nullptr;
  ID3D12PipelineState* commit_exposure = nullptr;
  // Foveated NR (foveation.hpp): resamples depth and motion onto the warped
  // working grid.  Created on first foveated frame; optional - without it the
  // foveated frame runs with the game's guides and logs once.
  ID3D12PipelineState* fov_guides = nullptr;
  // The NR look stage: its own root signature (look::SerializeRootSignature)
  // and programs, created on first use (EnsureLookPipeline).
  look::Pipeline look;
};

// Workset descriptor-heap layout (see EnsureWorkset):
//   SRV region, slots 0..15 - one four-entry set per stacking pass. Pass k
//   binds root table 0 at k * kCodecSrvSetStride: [t0]=ref_k, [t1]=proxy,
//   [t2]=nr_output, [t3]=ref_k. ref_0 is pass 0's reference image (the
//   copied game output; the game color on the pre-SR path - the only slots
//   ever rebound after creation, and only when their bound resource
//   actually changes, see SetSourceView).
//   ref_k>=1 is the previous pass's decoded ping-pong surface. A dedicated
//   set per pass means a later pass can never overwrite descriptors that an
//   earlier dispatch of the same command list reads: the GPU executes the
//   finished list, so a mid-recording slot rewrite would feed pass 0 the
//   last pass's scratch surface (the original infinite-accumulation bug).
//   The workset itself is keyed by (source handle, output resource), so two
//   distinct source features sharing one output never interleave their
//   chains on these surfaces either.
//   UAV region starts after the fixed SRV sets. The root table exposes u0/u1;
//   each stacking write surface remains adjacent to its block-mean view. v6
//   adds work0/work_a/work_b plus one 1x1 same-frame scale UAV.
//   v6 fixed SRV sets, slots 16..31 (HDR worksets only; filled at creation
//   except the pre-SR linearize source, which rebinds write-on-change):
//     16..19 commitA  [t0]=work_a [t1]=work0   (final surface = work_a)
//     20..23 commitB  [t0]=work_b [t1]=work0   (final surface = work_b)
//     24..27 linearize [t0..3]=source (inline: `original`; pre-SR: game color)
//     28..31 autoscale [t0..3]=work0
constexpr uint32_t kCodecSrvSetStride = 4;
constexpr uint32_t kDescriptorCommitASet = kMaxNrPasses * kCodecSrvSetStride;
constexpr uint32_t kDescriptorCommitBSet = kDescriptorCommitASet + 4;
constexpr uint32_t kDescriptorLinearizeSet = kDescriptorCommitBSet + 4;
constexpr uint32_t kDescriptorMeterSet = kDescriptorLinearizeSet + 4;
constexpr uint32_t kDescriptorProxyUav = kDescriptorMeterSet + 4;
constexpr uint32_t kDescriptorDecodedUav = kDescriptorProxyUav + 1;
constexpr uint32_t kDescriptorBlockMeanUav = kDescriptorDecodedUav + 1;
constexpr uint32_t kDescriptorDecodedAltUav = kDescriptorBlockMeanUav + 1;
constexpr uint32_t kDescriptorBlockMeanAltUav = kDescriptorDecodedAltUav + 1;
constexpr uint32_t kDescriptorWork0Uav = kDescriptorBlockMeanAltUav + 1;
constexpr uint32_t kDescriptorWorkAUav = kDescriptorWork0Uav + 1;
constexpr uint32_t kDescriptorWorkBUav = kDescriptorWorkAUav + 1;
constexpr uint32_t kDescriptorNormScaleUav = kDescriptorWorkBUav + 1;
// Governor state, deliberately adjacent: the autoscale dispatch binds
// norm_scale at u0, so this lands at u1 of the same two-descriptor table
// with no second bind and no root-signature change.
constexpr uint32_t kDescriptorNormCommitUav = kDescriptorNormScaleUav + 1;
// The root signature exposes u0/u1 as one contiguous two-descriptor table.
// Every v6 dispatch binds one output at u0, so keep one inert descriptor after
// the last real UAV to make the table valid without inventing another resource.
constexpr uint32_t kDescriptorUavPadding = kDescriptorNormCommitUav + 1;
// The NR look stage (look_stage.hpp), appended so that no codec slot moves.
// Written at workset creation like the codec sets (the pre-SR SDR pass 0
// rebinds slots 0 and 3 of both per-pass sets with the game color, as it
// does the codec set), and only while the workset has the surfaces:
//   look set, one per pass: [ref_k, proxy, look_output, ref_k|norm_scale] -
//     the codec set with N' in place of N; a shaped pass's resolve reads it.
//   transport set, one per pass: [ref_k, look_up_proxy, look_up_neural,
//     ref_k|norm_scale] - edge-aware transport hands the resolve P and N' at
//     the output resolution, which it reads 1:1.
//   motion ring: write-once views of the game's motion vectors (the temporal
//     filter's Motion), one per distinct resource, see LookMotionView.
//   look UAVs: the look::kUav* table.
constexpr uint32_t kDescriptorLookSet = kDescriptorUavPadding + 1;
constexpr uint32_t kDescriptorTransportSet =
    kDescriptorLookSet + kMaxNrPasses * kCodecSrvSetStride;
constexpr uint32_t kMotionRingSize = 4;
constexpr uint32_t kDescriptorMotionRing =
    kDescriptorTransportSet + kMaxNrPasses * kCodecSrvSetStride;
constexpr uint32_t kDescriptorLookUav = kDescriptorMotionRing + kMotionRingSize;
// Feed v2's exposure ring, appended so that no earlier slot moves: one
// four-entry set per distinct exposure texture (the codec root table spans
// t0..t3; v6_exposure_scale reads t0, all four hold the same view), written
// once, like the motion ring.
constexpr uint32_t kExposureRingSize = 4;
constexpr uint32_t kDescriptorExposureRing = kDescriptorLookUav + look::kUavCount;
// Foveated NR guides (foveation.hpp), appended so that no earlier slot moves:
// a write-once ring of codec SRV sets [depth, motion, depth, motion], one
// per distinct (depth, motion) pair the game hands NR, and the guide UAV
// pair (u0 = warped motion, u1 = warped depth) the codec root table spans.
constexpr uint32_t kFovGuideRingSize = 8;
constexpr uint32_t kDescriptorFovGuideRing =
    kDescriptorExposureRing + kExposureRingSize * kCodecSrvSetStride;
constexpr uint32_t kDescriptorFovGuideUav =
    kDescriptorFovGuideRing + kFovGuideRingSize * kCodecSrvSetStride;
constexpr uint32_t kDescriptorHeapSize = kDescriptorFovGuideUav + 2;

// FinalResources::look_surfaces bits: what a workset allocated for the look.
constexpr uint32_t kLookSurfaceOutput = 1u;     // look_output, the look set
constexpr uint32_t kLookSurfaceBands = 2u;      // the four band surfaces
constexpr uint32_t kLookSurfaceTransport = 4u;  // the up pair, the transport set
constexpr uint32_t kLookSurfaceTrace = 8u;      // the trace block statistics

// Handle for one slot of a workset's descriptor heap. Every descriptor
// creation and every runtime bind states its slot explicitly, so the fill
// in EnsureWorkset and the binds in BindCodec can only agree through the
// kDescriptor* constants - a shared, incrementally advanced cursor lets
// the two drift apart silently (writes land at N, binds read 16).
template <typename DescriptorHandle>
inline DescriptorHandle DescriptorHeapSlot(
    DescriptorHandle heap_start,
    uint32_t slot,
    uint32_t descriptor_size) {
  DescriptorHandle handle = heap_start;
  handle.ptr += static_cast<uint64_t>(descriptor_size) * slot;
  return handle;
}

struct FinalResources {
  ID3D12Resource* original = nullptr;
  ID3D12Resource* proxy = nullptr;
  ID3D12Resource* nr_output = nullptr;
  ID3D12Resource* decoded = nullptr;
  // Stacking ping-pong partner of `decoded`: pass k reads the previous
  // result (as SRV t0/t3) while writing the new one (as UAV u0), which one
  // surface cannot do.  Allocated only while NR stacking is enabled
  // (stack_passes > 1); passes alternate decoded -> decoded_alt -> decoded.
  ID3D12Resource* decoded_alt = nullptr;
  ID3D12DescriptorHeap* descriptors = nullptr;
  uint32_t descriptor_size = 0;
  // Resource bound into each SRV heap slot (the slots 0..31 SRV region -
  // per-pass sets plus the fixed v6 sets, whose linearize source the pre-SR
  // path rebinds - and the look and transport sets).  Descriptors must stay
  // immutable while any recorded command list referencing the heap may still
  // be in flight - writing "the same bytes again" is not a synchronization
  // guarantee - so rebinding a slot writes only when the bound resource
  // actually changes.
  ID3D12Resource* bound_sources[kDescriptorHeapSize] = {};
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t input_width = 0;
  uint32_t input_height = 0;
  // Stack depth the surfaces were dimensioned for (drives decoded_alt).
  uint32_t stack_passes = 1;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  DXGI_FORMAT working_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  uint8_t hdr_mode = 0;  // 0 = SDR, 1 = linear HDR, 2 = PQ HDR
  D3D12_RESOURCE_STATES original_state = D3D12_RESOURCE_STATE_COPY_DEST;
  D3D12_RESOURCE_STATES proxy_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  D3D12_RESOURCE_STATES nr_output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  D3D12_RESOURCE_STATES decoded_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  D3D12_RESOURCE_STATES decoded_alt_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  // Pre-SR mode: persistent NR-enhanced render-res color surface handed to the
  // game's DLSS evaluate in place of the game's own input color.  Same format
  // and dimensions as the game's color; written once per frame (copy from
  // `decoded`), read by NGX as an SRV, and state-tracked here because NGX
  // restores the state it was given.
  ID3D12Resource* pre_sr_color = nullptr;
  D3D12_RESOURCE_STATES pre_sr_color_state = D3D12_RESOURCE_STATE_COPY_DEST;
  bool with_pre_sr_color = false;
  // Per-32x32-block mean luma of the decoded (post-NR) image and the pre-NR
  // reference, consumed by the black-level restore passes.  Lives only in
  // UAV state its whole lifetime.
  ID3D12Resource* block_mean = nullptr;
  // ---- v6 linear working-space pipeline (HDR worksets only) ----
  // work0: the untouched linearized source, kept whole-frame as the metering
  // input, the pedestal reference, and pass-0's encode input.
  // work_a/work_b: resolve ping-pong pair; pass k reads work_in and writes
  // the other surface.  HDR never allocates decoded_alt - stacking lives in
  // linear space and `decoded` receives exactly one encode-back per frame.
  ID3D12Resource* work0 = nullptr;
  ID3D12Resource* work_a = nullptr;
  ID3D12Resource* work_b = nullptr;
  D3D12_RESOURCE_STATES work0_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  D3D12_RESOURCE_STATES work_a_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  D3D12_RESOURCE_STATES work_b_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  // Same-frame GPU normalization. A single small compute pass samples the
  // untouched linear source and writes one divisor texel; encode consumes it
  // immediately on the same command list. No readback or normalization state.
  ID3D12Resource* norm_scale = nullptr;
  D3D12_RESOURCE_STATES norm_scale_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  // Governor state: the committed divisor carried between frames.  Unlike
  // norm_scale this is never bound as an SRV and never changes state, so it
  // has no *_state companion - it lives its whole life in UNORDERED_ACCESS.
  ID3D12Resource* norm_commit = nullptr;
  // Reset epoch this workset's committed divisor has acknowledged.  A
  // mismatch snaps instead of slewing: a scene cut, a codec/config change,
  // or a feature rebuild makes the carried value meaningless, and slewing
  // out of it would be the lag the v6.0 lock was rightly criticized for.
  uint64_t norm_commit_epoch = 0;
  bool norm_commit_primed = false;
  // Stable governor clock: when this workset last ran the autoscale, so the
  // per-second attack/release rates become this frame's limits.
  int64_t norm_last_ns = 0;
  // ---- NR look stage (look_stage.hpp) ----
  // kLookSurface* bits of what is allocated.  With any bit set the trace
  // histogram exists too: it is the inert root binding for an unused history.
  uint32_t look_surfaces = 0;
  // N' at the NR resolution (RGBA16F; alpha = G_low under transport).
  ID3D12Resource* look_output = nullptr;
  D3D12_RESOURCE_STATES look_output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  // The guided-filter bands at half the NR resolution; UAV their whole life.
  ID3D12Resource* look_band_a = nullptr;
  ID3D12Resource* look_band_b = nullptr;
  ID3D12Resource* look_band_max_a = nullptr;
  ID3D12Resource* look_band_max_b = nullptr;
  // P and N' at the output resolution (transport); they change state together.
  ID3D12Resource* look_up_proxy = nullptr;
  ID3D12Resource* look_up_neural = nullptr;
  D3D12_RESOURCE_STATES look_up_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  // NREditTrace: look::kTraceWords uints (a buffer, resting in COMMON like
  // the history) and one RGBA32F texel per 32x32 NR block (UAV).
  ID3D12Resource* look_trace_histogram = nullptr;
  ID3D12Resource* look_trace_blocks = nullptr;
  // The motion ring's resources, each holding a reference so the write-once
  // view can never outlive (or be aliased by a successor of) what it views.
  ID3D12Resource* motion_views[kMotionRingSize] = {};
  // Foveated NR (foveation.hpp).  `foveation` is this frame's warp, written
  // by the after path before its first codec bind (uniform elsewhere);
  // every codec and look bind of the workset forwards it.  The guide pair is
  // allocated on the first foveated frame at the workset's working size and
  // lives until the workset retires; fov_guides_valid says this frame's
  // evaluate may hand them to NR in place of the game's guides.
  foveation::Grid foveation{};
  ID3D12Resource* fov_motion = nullptr;
  ID3D12Resource* fov_depth = nullptr;
  D3D12_RESOURCE_STATES fov_guides_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  bool fov_guides_failed = false;
  bool fov_guides_valid = false;
  // Write-once ring keys (AddRef'd), like motion_views.
  ID3D12Resource* fov_depth_views[kFovGuideRingSize] = {};
  ID3D12Resource* fov_motion_views[kFovGuideRingSize] = {};
  // The exposure ring's resources, held for the same reason.
  ID3D12Resource* exposure_views[kExposureRingSize] = {};
  // The pre-SR path's game color, held for the same reason (v7.0.0-rc10):
  // the workset is keyed by its pointer and its views are written once, so
  // a same-size color the game recreated at a freed one's address matched
  // both and left the views on freed memory.
  ID3D12Resource* pre_sr_source = nullptr;
  // The feed this workset's norm_commit last ran under: a change snaps,
  // because the carried value belongs to the other feed.
  FeedSource norm_feed = FeedSource::kV1;
  // Diagnostics: session-unique id (log/trace correlation), governor snaps
  // this workset took, and the device-reported footprint of its surfaces.
  uint32_t id = 0;
  uint64_t snaps = 0;
  uint64_t allocated_bytes = 0;
  // Interpreted source units (encoding + absolutes) for the v6 pedestal math;
  // set once at workset creation by InterpretSourceUnits.
  codec::SourceUnits units;
  // Evaluate-time exposure evidence remains diagnostic only.  It does not
  // participate in scaling or decide whether NR is allowed to run.
  // The typed format the scratch surfaces were actually created with: the
  // game's output can be *_TYPELESS, whose family member this records.
  DXGI_FORMAT resource_format = DXGI_FORMAT_UNKNOWN;
  // Last present this workset served, and when; the generation drives LRU
  // eviction of the pool, the time idle retirement (RetireSupersededWorksets).
  uint64_t last_used_generation = 0;
  int64_t last_used_ns = 0;
};

struct RetiredFinalResources {
  FinalResources resources;
  // Fallback age metric for queues without a completion fence.
  uint64_t generation = 0;
  GpuLease lease;
  // Exact submission-use proof (v6 audit issue 01): release when the
  // submission tracker reports every recorded use of these resources
  // complete - the lease above is then only a pre-tracker-era fallback.
  bool tracker_proof = false;
};

struct EvaluationContract {
  const NVSDK_NGX_Handle* source_handle = nullptr;
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* depth = nullptr;
  uint32_t input_width = 0;
  uint32_t input_height = 0;
  uint32_t output_width = 0;
  uint32_t output_height = 0;
  uint32_t motion_x = 0;
  uint32_t motion_y = 0;
  uint32_t depth_x = 0;
  uint32_t depth_y = 0;
  uint32_t create_flags = 0;
  float motion_scale_x = 0.f;
  float motion_scale_y = 0.f;
  // Jitter rides the contract presence-flagged (Streamline games supply it
  // every frame; native NGX titles may never set it).
  float jitter_x = 0.f;
  float jitter_y = 0.f;
  bool has_jitter = false;
  int32_t frame_reset = 0;
  // Feed v2 inputs: the game's exposure texture (in NON_PIXEL_SHADER_RESOURCE
  // for the evaluate, guide 3.4), its CPU factors with the NGX helpers' "0
  // means unset" rule applied, and this frame's FeedSource.
  ID3D12Resource* exposure = nullptr;
  float pre_exposure = 1.f;
  float exposure_scale = 1.f;
  FeedSource feed = FeedSource::kV1;
};

inline HMODULE addon_module = nullptr;
inline DirectApi direct_api;
inline DirectLoadState direct_load_state = DirectLoadState::Unknown;
// Why the runtime is unavailable, for the status card (ui::RuntimeFault):
// DirectLoadState has one Failed, and "the file is not there" and "the file
// is there and unusable" need different fixes.  Written where the load
// fails, latched like the failure itself, read by the overlay.
inline std::atomic_uint8_t nr_runtime_fault{0};
inline bool direct_runtime_failed = false;
inline std::string direct_runtime_sha256;
inline bool direct_runtime_reference_match = false;
inline CodecPipeline codec_pipeline;
// Multi-pass support: a bounded pool of per-output NR worksets instead of one
// shared scratch set.  A multi-pass title evaluates more than one DLSS-family
// pass per frame (upscaler + RR, chained denoisers, per-eye passes), and the
// passes can differ in resolution or format; with a single shared set every
// such frame retired and recreated the scratch resources (and, historically,
// every NR feature).  A workset is keyed by the game resource the pass owns -
// the NGX Output for the after path, the NGX Color for pre-SR - so mirrored
// duplicates of one pass share a set while genuinely different passes, even
// into the same output, get independent scratch surfaces.
// Scratch ownership: one entry per (source feature handle, output resource)
// pair.  Keying by the resource alone let two distinct source features that
// share one output interleave their chains on the same proxy/decoded
// ping-pong even though their temporal histories belong to different NGX
// handles; keying by the handle alone would let rotating outputs alias one
// chain's frame scratch.  The NGX-side history stays owned by the feature
// handle (FeatureState); the workset owns only per-frame scratch for one
// stream/output pair, reused across frames on the game's ordered command
// list.
struct WorksetKey {
  const NVSDK_NGX_Handle* handle = nullptr;
  const ID3D12Resource* resource = nullptr;
  bool operator==(const WorksetKey& other) const {
    return handle == other.handle && resource == other.resource;
  }
};

struct WorksetKeyHash {
  size_t operator()(const WorksetKey& key) const {
    return std::hash<const void*>()(key.handle)
        ^ (std::hash<const void*>()(key.resource) << 1);
  }
};

inline std::unordered_map<WorksetKey, FinalResources, WorksetKeyHash> worksets;
// Live-workset budget.  Single-pass titles allocate exactly one workset; the
// cap bounds worst-case memory for games that evaluate several NGX passes per
// frame.  Automatic - not a menu control (it is unrelated to NR stacking);
// power users can override it with the NRMaxWorksets ini key (1-8).
constexpr uint32_t kDefaultMaxWorksets = 4;
inline std::atomic_uint32_t max_worksets = kDefaultMaxWorksets;
inline std::vector<RetiredFinalResources> retired_final_resources;
inline std::vector<RetiredNrFeature> retired_nr_features;
inline ID3D12Device* direct_device = nullptr;
// The same device with ReShade's wrapper peeled off, for IDENTITY only.
// NGX keeps the wrapper (it is evaluated with the game's wrapped command
// list and the pair has to match - unwrapping just the device is an
// access violation on the first evaluate, measured), but
// reshade::api::device::get_native() reports the unwrapped pointer, so
// this is what the teardown callbacks can compare against.  Equal to
// direct_device when there is no wrapper.
inline ID3D12Device* direct_device_native = nullptr;
// The same value, kept as a NUMBER across a teardown: never dereferenced, and
// deliberately not cleared by Shutdown().  Once OnDestroySwapchain has
// released NR state, `direct_device` is null, and this is the only way the
// destroy_device that arrives afterwards can still be recognised as ours -
// without it the one-shot line reports tracked=0x0 and the match counter
// stays at zero for a session that tore down exactly as intended.
inline ID3D12Device* last_nr_device_native = nullptr;
// EnsureDirectRuntime's re-initializations for a device other than the one it
// holds, wrapper-world flips included; logged up to the cap.
inline std::atomic_uint64_t nr_runtime_device_changes{0};
constexpr uint64_t kNrRuntimeDeviceChangeLogCap = 16;
// ...and the ones that released NR state while a recording carrying NR
// work was not proven complete (observe-only, v7.0.0-rc10).
inline std::atomic_uint64_t nr_runtime_reinit_unproven{0};
// Did destroy_device fire, for how many D3D12 devices, and did any of
// them turn out to be ours?  The handler returns early on a device it
// does not recognise, so without these three a log cannot tell "the
// event never came" from "it came and the pointers did not compare" -
// and the difference is the whole of R8's device-teardown row.
inline std::atomic_uint64_t destroy_device_events{0};
inline std::atomic_uint64_t destroy_device_d3d12_events{0};
inline std::atomic_uint64_t destroy_device_matches{0};
// Swapchains this addon saw go away for good, and the ones that were on the
// device NR was initialized on.
inline std::atomic_uint64_t destroy_swapchain_events{0};
inline std::atomic_uint64_t destroy_swapchain_matches{0};
inline HMODULE ngx_core_module = nullptr;
inline bool parameter_runtime_ready = false;
inline std::atomic_bool parameter_runtime_via_core = false;
inline AllocateParametersFn core_allocate_parameters = nullptr;
inline DestroyParametersFn core_destroy_parameters = nullptr;
inline DirectCreateFn core_create_feature = nullptr;
inline DirectEvaluateFn core_evaluate_feature = nullptr;
inline DirectReleaseFn core_release_feature = nullptr;
inline void** get_module_filename_iat = nullptr;
inline void* original_get_module_filename = nullptr;
inline GetModuleFileNameWFn real_get_module_filename = nullptr;
inline std::unordered_map<const NVSDK_NGX_Handle*, FeatureState> features;
// Create-time record of EVERY NGX feature handle whose CreateFeature we
// intercepted (the features map holds DLSS/DLSSD contracts only).  Evaluates
// gate on this: a handle known to belong to another feature (DLSSG multi-frame
// generation, DeepDVC, ...) must never reach ProcessInline, even when its
// evaluate parameter block looks DLSS-shaped.  Only handles whose create
// slipped past the hooks (early-boot titles) fall back to the lazy path.
inline std::unordered_map<const NVSDK_NGX_Handle*, NVSDK_NGX_Feature>
    created_feature_ids;
// LIFE-02: the game's create contract survives an NR-only teardown. Games
// with separate create/evaluate blocks do not repeat create flags or guide
// dimensions on evaluate. These snapshots own no NR objects and are removed
// by the game's release hook or tracked device destruction (runtime_mutex).
inline std::unordered_map<const NVSDK_NGX_Handle*, FeatureState> create_contracts;
inline std::unordered_map<uint32_t, StreamlineViewportState> streamline_viewports;
// std::map keeps node addresses stable; those addresses are used as opaque
// keys in the existing NGX feature-state table for persistent Streamline
// fallback handles.
inline std::map<uint64_t, uint8_t> streamline_handle_keys;
inline std::mutex runtime_mutex;
inline bool hooks_installed = false;
// Hook policy, [RenoDX.DLSS5] EnableHooks in ReShade.ini:
//   0 = fully inert (safe mode: no NGX/Streamline hooks, no NR pass)
//   1 = NGX + Streamline hooks (opt-in, only for private Streamline bridges)
//   2 = NGX hooks only (default) - sl.interposer.dll/sl.common.dll unpatched.
// NGX-only still provides full NR: every FH6/Cyberpunk evaluation passes
// through _nvngx.dll no matter which interposer submitted it, so the Streamline
// layer is only a tag/fallback aid.  sl.interposer is also patched by other
// injected mods, so skipping it removes the most contested patch site.
inline std::atomic_bool hooks_enabled = true;
inline std::atomic_bool streamline_hooks_enabled = true;
// Row 23 (item 21 reopened): set by OnInitDevice when the session's first
// D3D12 device arrives. This - not hooks_enabled, which defaults on - is what
// distinguishes a process that can ever use the D3D12-only NR runtime from a
// DX11-only one that would only ever hold it for nothing.
inline std::atomic_bool d3d12_device_seen{false};
// P4 Path A (PLAN_DX11_V68.md 4.1/4.5): what makes a session FOREIGN - the
// process presents through D3D11 while a third-party tool's D3D12 device
// evaluates through the NGX hooks.  Set in OnPresent from the presenting
// queue's device API; both are sticky for the session, so a D3D12 present
// makes it a D3D12 session forever and the foreign paths below go quiet.
// The alpha33 present-path install gate stays exactly as it is: no D3D11
// object is ever detoured, and the present path keeps installing for real
// D3D12 devices.
inline std::atomic_bool d3d11_present_seen{false};
inline std::atomic_bool d3d12_present_seen{false};
// The DX11Source key (PLAN_DX11_V68.md 4.1, decision D1 in PLAN_REHAB_V7.md
// 12): what this addon does in a D3D11-presenting process.
//   'foreign' - the P4 behavior plus the Path A fixes: serve D3D12 evaluates
//               that arrive from a third-party tool; never hook a D3D11
//               export;
//   'native'  - v7.0.0-alpha45, Path B: detour the game's own D3D11 DLSS and
//               run NR after it through the Direct3D 11 bridge
//               (d3d11_bridge.hpp); a third-party tool's D3D12 evaluates
//               pass through untouched (source_other);
//   'auto'    - the default: native, unless a DX11 bridge add-on of another
//               project is loaded, which keeps that session foreign;
//   'off'     - A-6, v7.0.0: inert (Dx11SourceOffDefers below; lanes
//               e2e11_source_off and source_off_d3d12).
// LoadConfiguration reads the key and states the effective value one-shot;
// an unknown value falls back to auto, whose own fallback (a detected tool)
// is the existing foreign behavior.
inline constexpr uint32_t kDx11SourceForeign = 0;
inline constexpr uint32_t kDx11SourceNative = 1;
inline constexpr uint32_t kDx11SourceAuto = 2;
inline constexpr uint32_t kDx11SourceOff = 3;
inline std::atomic_uint32_t dx11_source{kDx11SourceAuto};
// The route the key resolved to, latched once the process is known to
// present through D3D11 (DecideDx11Route, d3d11_bridge.hpp).  Undecided in
// every D3D12 session, so nothing below changes there.
inline constexpr uint8_t kDx11RouteUndecided = 0;
inline constexpr uint8_t kDx11RouteNative = 1;
inline constexpr uint8_t kDx11RouteForeign = 2;
inline std::atomic_uint8_t dx11_route{kDx11RouteUndecided};
inline bool Dx11NativeRoute() {
  return dx11_route.load(std::memory_order_acquire) == kDx11RouteNative;
}
// Set while the bridge is up: EnsureDirectRuntime then creates NR through
// the signed runtime on the bridge's private device, because the NGX core in
// a D3D11 process was initialized for the game's D3D11 device.
inline std::atomic_bool bridge_force_snippet{false};
// v7.0.0 (A-6): the swapchain's API, recorded when ReShade creates it.  The
// present flags above arrive a frame late for a policy that must hold from
// the first frame: a foreign tool evaluates from its own present callback,
// which ReShade runs before this addon's, and the addon may attach after NGX
// is already loaded.  DX11Source=off reads these; the Path A gates keep
// keying on presents.
inline std::atomic_bool d3d11_swapchain_seen{false};
inline std::atomic_bool d3d12_swapchain_seen{false};
// Verdict v3's api=other: a swapchain or present through an API that is
// neither (D3D9/10, OpenGL, Vulkan).
inline std::atomic_bool other_api_seen{false};
inline bool D3D12SessionKnown() {
  return d3d12_present_seen.load(std::memory_order_relaxed)
         || d3d12_swapchain_seen.load(std::memory_order_relaxed);
}
inline bool D3D11OnlySession() {
  return (d3d11_present_seen.load(std::memory_order_relaxed)
          || d3d11_swapchain_seen.load(std::memory_order_relaxed))
         && !D3D12SessionKnown();
}
// DX11Source=off: every D3D12-side step - the NGX detours, the device hooks,
// the NR runtime pre-load - waits until the process has shown a D3D12
// swapchain or present.  Detouring is the one step that cannot be taken
// back, so under off it waits for the API to be KNOWN rather than for a
// D3D11 swapchain to rule it out.  The cost, in a D3D12 game that sets off
// (the key is for D3D11 games): the device hooks go on at swapchain creation
// instead of device creation, so lists created in between are first
// observed at their next Reset.
inline bool Dx11SourceOffDefers() {
  return dx11_source.load(std::memory_order_relaxed) == kDx11SourceOff
         && !D3D12SessionKnown();
}
inline bool Dx11SourceOffHere() {
  return dx11_source.load(std::memory_order_relaxed) == kDx11SourceOff
         && D3D11OnlySession();
}
// The native route serves the game's own D3D11 DLSS; a D3D12 evaluate in
// that session is a third-party tool's, and NR on it would be a second pass
// over the same frame (PLAN_DX11_V68.md 4.3).  It passes through untouched
// and is counted ineligible (source_other).
inline bool ForeignD3D12SourceIgnored() {
  return Dx11NativeRoute() && D3D11OnlySession();
}
inline std::atomic_bool logged_source_other{false};
inline std::atomic_bool logged_dx11_source_off_inert{false};
// v7.0.0: another Neural Rendering producer in the same process (a second
// NR mod, or a tool that drives DLSSNR itself).  Two producers would enhance
// the same frame twice.  The strong signal - a feature-18 create that is not
// this addon's, seen by the NGX create hooks - latches foreign_nr_seen, and
// with ForeignNr=yield (the default) every evaluate from then on passes
// through untouched and counts foreign_nr.  The weak signal - the NR runtime
// already mapped before this addon first loads it - is observe-only: a
// producer that loads the runtime and never creates through NGX is
// indistinguishable from a harmless preload, so it only logs.
// ForeignNr=observe keeps NR running after the strong signal too.
inline std::atomic_bool foreign_nr_yield{true};
inline std::atomic_bool foreign_nr_seen{false};
inline std::atomic_bool logged_foreign_nr_weak{false};
inline std::atomic_bool logged_foreign_nr_strong{false};
inline bool NrYieldsToForeign() {
  return foreign_nr_seen.load(std::memory_order_acquire)
         && foreign_nr_yield.load(std::memory_order_relaxed);
}
// v7.0.0-rc9: a swapchain teardown waiting for its proof (OnDestroySwapchain,
// ServicePendingTeardown).  Raised when the swapchain on NR's device goes
// away while recorded NR work has not completed; lowered by the release, or
// by an evaluate that shows the game kept rendering on that device
// (NrWaitsForTeardown).  `since` is for the release line's latency only.
inline std::atomic_bool teardown_pending{false};
inline std::atomic<int64_t> teardown_pending_since_ns{0};
// Auto-escalation (EnableHooks left at its 2/NGX-only default only): counts
// consecutive NGX evaluates that reached the hook but NR could not run on
// (not a DLSS evaluation, or DLSS without guide pointers).  60 of them while
// sl.common.dll is loaded proves the title drives DLSS through a private
// Streamline bridge the NGX-only mode can never read; the session then turns
// the Streamline layer on instead of staying inert.  Any successful NR
// evaluate resets the streak, so games that work never escalate.
inline std::atomic_uint32_t streamline_escalation_deficits = 0;
inline std::atomic_bool streamline_escalation_allowed = false;
inline std::atomic_bool streamline_auto_escalated = false;
// The shape the EnableHooks=1 hint last fired for (ui::StreamlineShape), so
// the status card can show what used to exist only as a log line.  Latched:
// the card ignores it once the Streamline layer is on, and ENGAGED outranks
// it.
inline std::atomic_uint8_t streamline_hint_shape{0};
inline HMODULE hooked_ngx_module = nullptr;
// Atomic: read as InstallStreamlineHooks' fast path on the present thread,
// written under runtime_mutex (install) and in UnhookInstalledDetours.
inline std::atomic_bool streamline_hooks_installed = false;
inline HMODULE hooked_streamline_module = nullptr;
// `inside_direct_call` moved to direct_call.hpp in v6.8.0 and is now
// DirectCallScope / InsideDirectCall().  It is private there: the seven
// hand-managed set/clear pairs it used to have leaked the fence on any
// C++ exception at three of them, and a bare bool cannot stop an eighth
// site being written the same way.  See the header.
inline thread_local bool inside_game_evaluate_wrapper = false;

// Command-list state hooks observe HOST state so injected work can restore it.
// They must not learn from this addon's own codec/NGX traffic: doing so turns
// the observer into a participant and contaminates the persistent host shadow.
// A depth counter (rather than a bool) keeps nested NGX/module-chain calls safe.
inline thread_local uint32_t injected_command_scope_depth = 0;

struct InjectedCommandScope {
  InjectedCommandScope() noexcept { ++injected_command_scope_depth; }
  ~InjectedCommandScope() noexcept {
    if (injected_command_scope_depth != 0) --injected_command_scope_depth;
  }
  InjectedCommandScope(const InjectedCommandScope&) = delete;
  InjectedCommandScope& operator=(const InjectedCommandScope&) = delete;
};

inline bool SuppressCommandStateObservation() noexcept {
  // InsideDirectCall() is a defensive second fence for any internal NGX call
  // that is ever moved outside ProcessInline/ProcessInlinePreSR.
  return injected_command_scope_depth != 0 || InsideDirectCall();
}

// The NGX runtime's own traffic during an evaluate is not host state either,
// so it raises the same fence.  A host cannot see a bind the runtime makes
// inside EvaluateFeature and cannot depend on it: D3D12 invalidates every
// root descriptor table when the descriptor heaps change, so after a vanilla
// evaluate the host MUST re-bind before its next dispatch
// (MS, ID3D12GraphicsCommandList::SetDescriptorHeaps).  Recording the
// runtime's SetDescriptorHeaps made the restore replay the RUNTIME's heap
// together with the HOST's table handle - a pair that never existed on the
// list, and an invalid call: "The descriptor heap containing handle ... is
// different from currently set descriptor heap".  Measured on the T2
// `rebind_all` profile with the D3D12 debug layer, 2026-09-20: 0 validation
// errors with NR off, 239 of 240 frames with NR on, every one of them this
// call.  With the fence the restore replays the host's heap first and the
// host's table against it, which is defined, and which leaves an engine
// that does not re-bind BETTER off than vanilla rather than equal to it.
using NgxRuntimeCommandScope = InjectedCommandScope;

// Teardown sequencing.  Shutdown() raises shutting_down, waits for in-flight
// hook callbacks to leave addon state, and only then unhooks and releases
// resources - so a hooked NGX evaluate running on a game thread can never
// touch a handle or resource that Shutdown already freed.
inline std::atomic_bool shutting_down = false;
inline std::atomic_uint32_t callbacks_in_flight = 0;
// Held by the one Shutdown() that runs; only its holder raises and lowers
// `shutting_down`, so a swapchain teardown that refuses cannot lower it
// under another thread's release (v7.0.0-rc10).
inline std::atomic_bool teardown_owned = false;

// RAII gate for every hook entry point.  While shutdown is draining, new
// callback entries forward to the original function without touching addon
// state (counters included).  The double check closes the window between the
// first test and the increment.  The increment, the second test, and
// Shutdown's raise and count are sequentially consistent: this is a
// store-then-load handshake on two variables, which acquire/release does
// not order (an x86 store buffer lets Shutdown read a zero count while this
// side reads the flag still down).
struct CallbackScope {
  bool active = false;

  CallbackScope() {
    if (shutting_down.load(std::memory_order_acquire)) return;
    callbacks_in_flight.fetch_add(1, std::memory_order_seq_cst);
    if (shutting_down.load(std::memory_order_seq_cst)) {
      callbacks_in_flight.fetch_sub(1, std::memory_order_acq_rel);
      return;
    }
    active = true;
  }
  ~CallbackScope() {
    if (active) callbacks_in_flight.fetch_sub(1, std::memory_order_acq_rel);
  }
  CallbackScope(const CallbackScope&) = delete;
  CallbackScope& operator=(const CallbackScope&) = delete;

  explicit operator bool() const { return active; }
};

// A game can load more than one NGX module (its own nvngx_dlss.dll plus the
// driver/NGX core _nvngx.dll), and the core can load AFTER the addon attaches.
// Each loaded module that exports the NGX D3D12 symbols gets its own inline
// detour so the hook can never be bypassed by calling a different copy.
//
// CRITICAL: each detoured module must call ITS OWN original function pointer.
// Every NGX plugin forwards its Create/Evaluate calls into the core, so routing
// one module's calls into a *different* copy's original re-enters the hook and
// smears the output (seen in KCD2: both nvngx_dlss.dll and the _nvngx.dll core
// were detoured and the shared real_* globals pointed the core's calls back into
// the plugin forwarder).  A per-module slot table keeps every original local to
// the module that owns it, so a chain like plugin -> core -> real terminates at
// the true core instead of looping back into the hook.
constexpr int kMaxNgxSlots = 8;
struct NgxModuleReal {
  void* create = nullptr;
  void* evaluate = nullptr;
  void* evaluate_c = nullptr;
  void* release = nullptr;
};
inline NgxModuleReal ngx_slot_real[kMaxNgxSlots];
inline bool ngx_slot_used[kMaxNgxSlots] = {};
// "A slot was hooked at some point in this session", latched and never
// cleared, for the funnel's ngx_hooked rung - see CollectFunnelInputs.
inline std::atomic_bool ngx_ever_hooked = false;
inline HMODULE ngx_slot_module[kMaxNgxSlots] = {};
inline std::unordered_set<HMODULE> ngx_hooked_modules;
// Modules whose detour attempt failed (no resolvable exports, or a Detours
// failure).  Until v6.8.0-alpha29 a failure wrote the module off for the
// WHOLE SESSION, and that is a permanent-off mode: one failed install at
// start-up cost the user Neural Rendering until they restarted the game,
// with a log that looked healthy afterwards because the failure was named
// once and never again.  The reason for the write-off was real - the
// module scan runs every 90 presents, and a module that can NEVER hook must
// not produce a warning every 90 frames for an hour - so the replacement
// keeps that property by backing off instead of latching: each failure
// schedules the next attempt 1, 3, 7, 15 ... presents later, capped at
// kNgxRetryCeilingPresents, and only the first failure and the first
// arrival at the ceiling are logged.  A module that can never hook
// therefore still costs two log lines and a map lookup, and a module whose
// install failed once gets another go.  The one cause measured so far was
// this addon's own: two of its threads contending for its statically
// linked copy of Detours (v6.8.0-alpha31, see vtable::TransactionMutex).
// runtime_mutex-guarded like ngx_hooked_modules; cleared in Shutdown
// alongside it.
struct NgxHookRetry {
  uint64_t next_attempt_present = 0;
  uint32_t attempts = 0;
};
// About an hour of game frames at 60 fps, which is the cadence the v6.7.3
// "recovery1" build settled on for the same failure.
inline constexpr uint64_t kNgxRetryCeilingPresents = 216000;

inline std::unordered_map<HMODULE, NgxHookRetry> ngx_failed_modules;
// Atomic: InstallHooks' scan decision reads it on the present thread while
// the hook phase and UnhookInstalledDetours write it under runtime_mutex.
inline std::atomic_bool ngx_any_hooked = false;

// Entry tallies for the four detoured NGX exports, incremented as the FIRST
// statement of each wrapper - BEFORE the CallbackScope filter that
// `intercepted_creates` and `intercepted_evaluations` sit behind.
//
// Those two answer "did a call reach this addon's logic".  Neither can
// answer "was the detour entered at all", because a filtered call leaves
// them at zero and so does a call that never arrived.  The Forza Horizon 6
// G3 field session turned on exactly that distinction: "my detours were
// never called" and "the calls went to a copy I did not detour" produce
// byte-identical logs today.  debug::TouchNgxCreate() counts entries, but
// it compiles to nothing unless the trace build is on - and a field log
// only ever comes from a shipped build.
//
// Per slot, because which module copy gets called is the other half of the
// same question: KCD2 had both nvngx_dlss.dll and the _nvngx.dll core
// detoured, and a copy that is hooked but never entered is a finding.
// Never reset on unhook - like intercepted_creates, these are the
// session's cumulative record.  A log reading copies=0 entered=2 is a
// teardown that has already run, not a contradiction.
inline std::atomic_uint64_t ngx_entry_create[kMaxNgxSlots];
inline std::atomic_uint64_t ngx_entry_evaluate[kMaxNgxSlots];
inline std::atomic_uint64_t ngx_entry_evaluate_c[kMaxNgxSlots];
inline std::atomic_uint64_t ngx_entry_release[kMaxNgxSlots];

// The same claim for the two detours that are neither a command-list vtable
// slot nor an NGX export: the device's CreateCommandList and the queue's
// ExecuteCommandLists.  These are where "installed is not entered" has
// already cost something - v6.7.4 detoured device slot 42
// (GetResourceTiling) believing it was CreateCommandSignature, and nothing
// at runtime could say the wrong function had been patched.
// CreateCommandSignature's own entries are already visible as
// `shadow[sigs=]`, so these two complete the set.
inline std::atomic_uint64_t device_create_list_entries{0};
inline std::atomic_uint64_t queue_submit_entries{0};

// Built-in defaults - the single source of truth for every user setting.  The
// setting atomics below initialize from these, LoadConfiguration falls back
// to them, and PersistConfig/ResetToDefaults write them.  kConfigVersion (at
// the top of this file) makes changed defaults reach existing installs: a
// stored config with an older marker is reset instead of silently shadowing
// the new values.  Deliberately game-agnostic: stock/neutral NR model
// parameters plus the anchored codec, which is the mode that keeps the decode
// free of the NR model's raised-black pedestal at any anchor.
namespace defaults {
inline constexpr bool kEnabled = true;
inline constexpr bool kPreSr = false;
inline constexpr uint32_t kStackPasses = 1;
// Production default (v5.3): stacked passes 2+ keep their temporal history
// across frames like pass 1 - independent per-slot histories driven by the
// v5.3 reset-epoch model, the stacking arrangement the temporal gates
// qualify.  The v5.2 reset-every-frame stateless refinement stays available
// as an explicitly selected legacy diagnostic (NRChainedHistory=0); a stored
// explicit false is honored, only the absent-key default flips.
inline constexpr bool kChainedHistory = true;
inline constexpr uint32_t kPreset = 0;
inline constexpr uint32_t kStyle = 0;
inline constexpr float kIntensity = 1.f;
inline constexpr float kLocalTone = 1.f;
inline constexpr float kLocalStructure = 1.f;
// A negative value asks runtime 310.8 to make Skin follow Structure. Keep
// -1 as the stored stock sentinel; independent steering sends it as 1 when
// the Character mask is on. At Structure 1 those are the same stock output.
inline constexpr float kSkinStructure = -1.f;
inline constexpr bool kSkinIndependent = true;
inline constexpr bool kAutoMask = false;
inline constexpr bool kUiCorrection = false;
// NR codec mode default.  2 = Auto (the v6 default and the one mode that
// "just works"): interpret the source encoding, run the linear
// working-space pipeline, and pick the normalization per stream - anchored
// when the source carries absolute units, the fixed Display curve when it
// does not (engine-relative float HDR; the FP8 input floor makes any
// content-derived gain unstable - see codec_gain.hpp).  0 = classic (raw
// paper-white gain), 1 = anchored + black-level restore, and 3 = Display
// (forced fixed curve) remain manual selections; config migration preserves
// an explicit 0/1 and only defaultless configs land on Auto.
inline constexpr uint32_t kCodecMode = 2;
inline constexpr float kProxyAnchorNits = 4.f;
// PQ calibration (NRPQCalibration): the divisor scale the legacy PQ bridge
// was tuned against.  1 = the 203-nit reference semantics.  Pre-v6 configs
// stored 2.5375 (paper-white), whose effective anchor was ~515 nits; the
// v5 config migration keeps the stored number's meaning, not its value.
inline constexpr float kPqCalibration = 1.f;
// Source-interpretation overrides (advanced).  0 = infer from the resource
// format (float -> linear HDR, R10G10B10A2 -> PQ, otherwise SDR).
// NRSourceEncoding 1/2/3 force linear/PQ/SDR; NRLinearUnitNits > 0 declares
// an absolute scRGB scaling (nits per 1.0); NRSourcePrimaries is accepted
// and logged (the NR proxy is gamut-agnostic today - explicit gamut mapping
// is future work).
inline constexpr uint32_t kSourceEncoding = 0;
inline constexpr uint32_t kSourcePrimaries = 0;
inline constexpr float kLinearUnitNits = 0.f;
inline constexpr uint32_t kDepthMode = 0;
inline constexpr float kMvecScale = 1.f;
// 203/80 nits - the sRGB reference white, not a tuning value.
inline constexpr float kPaperWhiteScale = 2.5375f;
// Diffuse white in nits for the PQ HDR colour bridge (hdr_mode == 2 only;
// upstream-v4.7 parity).  It replaces the hardcoded BT.2408 203-nits anchor
// behind the PQ normalization and never multiplies on top of the codec gain
// above - paper white is the display-referred scene gain, diffuse white is
// the PQ bridge anchor.  SDR ignores it; the linear HDR path has no separate
// diffuse-white constant (its anchor IS the paper-white gain).
inline constexpr float kDiffuseWhiteNits = 203.f;
inline constexpr float kTransferStrength = 1.f;
inline constexpr float kColorStrength = 1.f;
inline constexpr float kChromaClampStops = 1.f;
// Display-codec resolve transfer (docs/history/pre-rehab/PLAN_CODEC_TRANSFER_V64.md).  0 = bounded
// ratio: the N/P gain alone, which under-transfers the model's brightness
// edits wherever the proxy curve bends (harness: 0.16-0.25 stops of a 1-stop
// edit lost in the midtones).  1 = consistent: adds the damped curve-gain
// correction g(m_P)/g(m_N) (harness: 0.004).  The default stays 0 until an
// in-game A/B with readback; both modes feed the model an identical proxy.
inline constexpr uint32_t kTransferMode = 0;
// The commit's dark-pedestal removal on Display-codec frames
// (FramePedestalRemoval).  0 = Auto: skipped where the units are relative;
// 1 = Always: the removal on every HDR frame, the behavior through rc7.
inline constexpr uint32_t kDisplayPedestal = 0;
// Neural-floor chroma guard on the divisor-family resolve (v6_common
// UpgradeToneMap, Curve 2; ported from v6.6.0's C6, PLAN_REHAB_V7 section 8
// item 8).  Where the model's value sits at the floor and the pixel is
// amplified past a stop, the colour is the original's at the transferred
// luminance: a near-black neural result under a bright original turned
// orange highlights green (harness: 40 stops of chroma error).  Off is the
// previous math exactly; the Display codec never takes this branch.
inline constexpr bool kNeuralFloorGuard = true;
// Per-list compute-state store (command_state.hpp): 0 = striped table,
// 1 = state attached to the command list object.  Read once at load.
// Default 1 since v6.5.1: the KCD2 session of 2026-09-18 (boot, engage,
// 5 feature re-creates) ran every frame with attach_failed=0 at baseline
// frame time; `ListStateMode=0` is the per-game fallback.
inline constexpr uint32_t kListStateMode = 1;
// Duplicate-evaluate detection: 0 = per-present map, 1 = per-evaluate call
// nesting (evaluate_chain.hpp).  Read once at load.
inline constexpr uint32_t kDedupeMode = 1;
// Normalization governor.  The per-frame autoscale candidate is an
// estimator, not a decision.  2 = Stable (v6.3 default): continuous
// estimator, attack/release limits in stops per SECOND, hysteresis hold,
// and only a workset's first frame snaps (see v6_autoscale).  1 = Slew
// (v6.1.2): step-gated estimator under a symmetric per-frame limit, snapping
// on every reset-epoch advance and game Reset.  0 = Off: the raw candidate
// (exact v6.1.0).  1 and 0 stay selectable per the generic-mod policy - new
// normalization behavior is a mode beside the old one, never a replacement.
inline constexpr uint32_t kNormGovernor = 2;
// Stable-mode limits, log2 stops per second of wall-clock time.  Attack (the
// divisor rising: the source went brighter and the proxy is heading into the
// shoulder/clip, the destructive direction) is fast; release (relaxing) is
// slow, so a candidate that flickers produces a held, near-peak divisor
// instead of a ramp in each direction.  Engine eye adaptation runs ~1-2
// stops/s, so a 0.5 st/s release never outruns the game's own exposure.
inline constexpr float kNormAttackStops = 4.f;
inline constexpr float kNormReleaseStops = 0.5f;
// Rate limit in log2 stops per frame.  present_generation is this addon's
// per-frame clock (there is no frame-time feed), so the bound is per frame:
// 0.02 is ~1.2 stops/s at 60 Hz, comfortably faster than engine eye
// adaptation (~1-2 stops/s) yet ~38x slower than the single-frame step the
// autoscale's 1/32 shoulder gate produces when a title's own exposure walks
// the buffer statistics across it (the Silent Hill 2 class).
inline constexpr float kNormSlewStops = 0.02f;
// NR input scale on relative HDR sources (Feed, v7.0.0-rc3): 1 = v1, the
// content meter above (v6_autoscale + governor); 2 = v2, the game's own
// exposure (v6_exposure_scale: PreExposure / (ExposureTexture *
// ExposureScale), or a fixed 1 for a pre-exposed buffer); 0 = Auto, which
// picks v2 only on evidence - a readable exposure texture without the
// AutoExposure flag - and v1 everywhere else, pre-exposed streams included
// (FrameFeed says why).
inline constexpr uint32_t kFeedMode = 0;
inline constexpr float kPassStrength = 1.f;
inline constexpr bool kFollowInputRes = false;
inline constexpr float kResolutionScale = 1.f;
// Foveated NR (after-upscale path): off by default.  A 3000 x 1800 output-
// pixel centre at full density, a 400 px smoothstep fall-off on all four
// sides, half density (per axis) beyond.  Clamped per output resolution.
inline constexpr bool kFoveation = false;
inline constexpr uint32_t kFoveationWidth = 3000;
inline constexpr uint32_t kFoveationHeight = 1800;
inline constexpr uint32_t kFoveationFalloff = 400;
inline constexpr float kFoveationEdge = 0.5f;
// Config/overlay bounds for the pixel settings (the output clamps further).
inline constexpr uint32_t kFoveationMaxPixels = 16384;
inline constexpr uint32_t kToggleHotkey = VK_F6;
inline constexpr uint32_t kScreenshotHotkey = VK_F5;
// Per-stage GPU timestamp diagnostics around the injected chain
// (gpu_timers.hpp).  Off by default: the frame path pays nothing until the
// user (or a profiling session) asks for numbers.
inline constexpr bool kGpuTimers = false;
// The NR look stage (PLAN_NR_LOOK_V71.md): Shape result off, and every gain
// the identity (look::Settings{}).  Off means the stage is never dispatched,
// so the resolve reads the model's output bit for bit.
inline constexpr bool kLookMode = false;
inline constexpr look::Settings kLook{};
// NREditTrace: observe-only statistics of the model's edit, off.
inline constexpr bool kEditTrace = false;
// Per-pass model steering (group G): passes 2+ follow pass 1.
inline constexpr bool kPassFollow = true;
// The workset budget keeps its own kDefaultMaxWorksets (workset pool block).
}  // namespace defaults

inline std::atomic_bool enabled = defaults::kEnabled;
// NR insertion point.  false (default) = "after": NR runs on the game's DLSS
// OUTPUT at display resolution.  true = "pre-SR": NR runs on the game's DLSS
// INPUT color at render resolution BEFORE the game's evaluate, so the
// NR network sees render-res color with resolution-matched depth/motion
// guides and the game's own DLSS upscales the already-enhanced image.  The two paths
// are mutually exclusive per frame; the pre-SR path additionally hard-requires
// guide resources that exactly match the color dimensions (else it declines
// for that frame and the game evaluates its own color).
inline std::atomic_bool nr_before_upscale = defaults::kPreSr;
// Global hotkeys (Windows virtual-key codes, persisted in [RenoDX.DLSS5] as
// NRToggleKey / NRScreenshotKey).  F6 is the default NR toggle because F5
// belongs to the screenshot pair.  hotkey_capture_target arms the overlay's
// rebind scan: 1 = toggle key, 2 = screenshot key; the scan runs in the
// overlay callback (OnOverlay) because ReShade blocks game input while the
// overlay is open (InputProcessing=2, its default), and its input hook then
// answers GetAsyncKeyState with "up" for every key.  For the same reason the
// hotkeys themselves are read by OnOverlayHotkeys while the overlay is open.
inline std::atomic_uint32_t toggle_hotkey = defaults::kToggleHotkey;
inline std::atomic_uint32_t screenshot_hotkey = defaults::kScreenshotHotkey;
inline std::atomic_bool toggle_hotkey_was_down = false;
inline std::atomic_uint32_t hotkey_capture_target = 0;
// ReShade's overlay is open (OnReshadeOpenOverlay): the HUD hides and the
// hotkeys switch from GetAsyncKeyState to ReShade's own input.
inline std::atomic_bool hud_overlay_open{false};
// The ImGui frame in which OnOverlay found this addon's window floating at a
// size no one would pick, or -1 (OnDockOverlayWindow docks it; see there).
inline std::atomic_int overlay_window_stranded{-1};
// OnDockOverlayWindow's progress: 0 nothing done, 1 docked and the tab still
// to select, 2 finished for this session.
inline std::atomic_uint32_t overlay_dock_step{0};
// NRGpuTimers: per-stage GPU timestamps for the injected chain (see
// gpu_timers.hpp).  Diagnostic instrumentation, not a behavior knob.
inline std::atomic_bool gpu_timers_enabled = defaults::kGpuTimers;
// Global history-reset epoch (v5.3).  Advances once for every event that
// must reset every stream's temporal history (startup, globally signaled
// scene cut, codec/style/config change).  Each slot carries the epoch it has
// acknowledged in NrFeatureSlot::acknowledged_reset_epoch; per-stream events
// (slot creation, contract recreation) use the slot's pending_reset instead
// so they never reset another stream.  Nothing consumes the epoch - see
// reset_epoch.hpp for the v5.2.2 latch defect this replaces.
inline std::atomic_uint64_t history_reset_epoch = 1;
// Who advanced the epoch (telemetry): the game's NGX Reset flag, an overlay
// setting edit, or a feature recreate (toggle, pass count, reset button).
// Every source also snaps the normalization governor, so a support log must
// be able to say which one fired.
enum class HistoryResetSource : uint8_t { kGameReset, kSetting, kRecreate, kCount };
inline std::atomic_uint64_t history_reset_counts[
    static_cast<size_t>(HistoryResetSource::kCount)] = {};
inline void RequestHistoryReset(
    HistoryResetSource source = HistoryResetSource::kSetting) {
  history_reset_epoch.fetch_add(1, std::memory_order_relaxed);
  history_reset_counts[static_cast<size_t>(source)].fetch_add(
      1, std::memory_order_relaxed);
}
inline std::atomic_uint64_t configuration_generation = 1;
inline std::atomic_uint32_t preset = defaults::kPreset;
inline std::atomic<float> intensity = defaults::kIntensity;
inline std::atomic_uint32_t style = defaults::kStyle;
inline std::atomic<float> local_tone_strength = defaults::kLocalTone;
inline std::atomic<float> local_structure_strength = defaults::kLocalStructure;
inline std::atomic<float> skin_structure_strength = defaults::kSkinStructure;
inline std::atomic_bool skin_independent = defaults::kSkinIndependent;
inline std::atomic_bool use_auto_mask = defaults::kAutoMask;
inline std::atomic_bool ui_correction = defaults::kUiCorrection;
// NR codec operating mode.  0 = classic: the codec input gain is the raw
// paper-white scale (a scene-linear multiplier).  1 = anchored: the gain is
// expressed as the proxy anchor (the scene luminance in nits that lands on
// the proxy shoulder start) and a post-decode black-level restore measures
// and removes the NR model's dark pedestal explicitly.  2 = Auto: the v6
// per-stream interpretation picks anchored/metered/classic per workset.
inline std::atomic_uint32_t codec_mode = defaults::kCodecMode;
inline std::atomic<float> proxy_anchor_nits = defaults::kProxyAnchorNits;
inline std::atomic<float> pq_calibration = defaults::kPqCalibration;
// Source-interpretation overrides (0 = infer from format); see defaults.
inline std::atomic_uint32_t source_encoding = defaults::kSourceEncoding;
inline std::atomic_uint32_t source_primaries = defaults::kSourcePrimaries;
inline std::atomic<float> linear_unit_nits = defaults::kLinearUnitNits;
// NR stacking (v5): chained feature-18 passes.  1 (default) is the original
// single-pass behavior.  Pass 1 uses the existing global strength controls
// (NRIntensity / NRTransferStrength / NRColorStrength); passes 2..kMaxNrPasses
// have their own intensity and decode-blend strengths so the stack can be
// tuned pass by pass.
inline std::atomic_uint32_t stack_passes = defaults::kStackPasses;
inline std::atomic<float> pass_intensity[kMaxNrPasses - 1] = {
    defaults::kPassStrength, defaults::kPassStrength, defaults::kPassStrength};
inline std::atomic<float> pass_transfer_strength[kMaxNrPasses - 1] = {
    defaults::kPassStrength, defaults::kPassStrength, defaults::kPassStrength};
inline std::atomic<float> pass_color_strength[kMaxNrPasses - 1] = {
    defaults::kPassStrength, defaults::kPassStrength, defaults::kPassStrength};
// Per-pass model steering (group G, NGX parameters only): a pass whose
// "Same as pass 1" is on sends pass 1's values, the Look section's.
inline std::atomic_bool pass_follow[kMaxNrPasses - 1] = {
    defaults::kPassFollow, defaults::kPassFollow, defaults::kPassFollow};
inline std::atomic_uint32_t pass_style[kMaxNrPasses - 1] = {
    defaults::kStyle, defaults::kStyle, defaults::kStyle};
inline std::atomic<float> pass_local_tone[kMaxNrPasses - 1] = {
    defaults::kLocalTone, defaults::kLocalTone, defaults::kLocalTone};
inline std::atomic<float> pass_local_structure[kMaxNrPasses - 1] = {
    defaults::kLocalStructure, defaults::kLocalStructure, defaults::kLocalStructure};
inline std::atomic<float> pass_skin_structure[kMaxNrPasses - 1] = {
    defaults::kSkinStructure, defaults::kSkinStructure, defaults::kSkinStructure};
inline std::atomic_bool pass_auto_mask[kMaxNrPasses - 1] = {
    defaults::kAutoMask, defaults::kAutoMask, defaults::kAutoMask};
inline std::atomic_bool pass_ui_correction[kMaxNrPasses - 1] = {
    defaults::kUiCorrection, defaults::kUiCorrection, defaults::kUiCorrection};
// The NR look stage's settings (look::Settings, field for field).  Live: the
// chain snapshots them once per frame (LookSettingsSnapshot); NRLookMode,
// Stabilize and Upsampling also decide which surfaces a workset allocates.
inline std::atomic_bool look_mode = defaults::kLookMode;
inline std::atomic<float> look_strength = defaults::kLook.strength;
inline std::atomic<float> look_brighten = defaults::kLook.brighten;
inline std::atomic<float> look_darken = defaults::kLook.darken;
inline std::atomic<float> look_max_brighten = defaults::kLook.max_brighten;
inline std::atomic<float> look_max_darken = defaults::kLook.max_darken;
inline std::atomic<float> look_colour = defaults::kLook.colour;
inline std::atomic<float> look_hue = defaults::kLook.hue;
inline std::atomic<float> look_max_colour = defaults::kLook.max_colour;
inline std::atomic<float> look_shadows = defaults::kLook.shadows;
inline std::atomic<float> look_midtones = defaults::kLook.midtones;
inline std::atomic<float> look_highlights = defaults::kLook.highlights;
inline std::atomic<float> look_tone = defaults::kLook.tone;
inline std::atomic<float> look_detail = defaults::kLook.detail;
inline std::atomic<float> look_halo = defaults::kLook.halo;
inline std::atomic<float> look_detail_radius = defaults::kLook.detail_radius;
inline std::atomic_uint32_t look_stabilize = defaults::kLook.stabilize;
inline std::atomic<float> look_stabilize_ms = defaults::kLook.stabilize_ms;
inline std::atomic_bool look_stabilize_detail = defaults::kLook.stabilize_detail;
inline std::atomic_uint32_t look_upsample = defaults::kLook.upsample;
inline std::atomic_bool edit_trace_enabled = defaults::kEditTrace;
// Latched when the look's programs or surfaces could not be created: the
// look (and the trace) stay off for the session, NR runs on without them.
inline std::atomic_bool look_unavailable = false;
// Look stage counters (telemetry and the support report): shaped passes,
// transported passes, traced passes, history restarts, and frames that ran
// the temporal filter without motion because the ring was full.
inline std::atomic_uint64_t look_shaped_passes{0};
inline std::atomic_uint64_t look_transport_passes{0};
inline std::atomic_uint64_t look_traced_passes{0};
inline std::atomic_uint64_t look_history_restarts{0};
inline std::atomic_uint64_t look_motion_ring_full{0};
// Opt-in chained temporal history (v5.2): with the toggle ON, stacked passes
// 2+ keep their temporal history like pass 1 (the documented NVIDIA warning
// is that Reset on every frame causes temporal flicker); OFF (default) is the
// field-tested stateless stacking.  Consumed in SetEvaluationParameters.
inline std::atomic_bool chained_temporal_history = defaults::kChainedHistory;
// NR working resolution (both insertion points, v5.1).  follow_input: NR
// runs at the game's guide (render) resolution.  Otherwise nr_resolution_scale
// (0.33..1) is applied to the reference resolution - the DLSS output for the
// after path, the NGX color for pre-SR; 1.0 = native.  The NR feature is
// created 1:1 at the working resolution and the decode stage upscales, which
// avoids the low-res Color contract the signed runtime rejects outright.
inline std::atomic_bool nr_follow_input_res = defaults::kFollowInputRes;
inline std::atomic<float> nr_resolution_scale = defaults::kResolutionScale;
// Foveated NR working grid (foveation.hpp), after-upscale path only: the
// working image is warped per axis - density 1 (relative to the resolution
// above) inside a centre zone of width x height OUTPUT pixels, a smoothstep
// fall-off of `falloff` pixels on every side, `edge` beyond - and NR runs
// once on it with warped guides.  Pre-SR keeps the uniform grid.
inline std::atomic_bool nr_foveation = defaults::kFoveation;
inline std::atomic_uint32_t nr_foveation_width = defaults::kFoveationWidth;
inline std::atomic_uint32_t nr_foveation_height = defaults::kFoveationHeight;
inline std::atomic_uint32_t nr_foveation_falloff = defaults::kFoveationFalloff;
inline std::atomic<float> nr_foveation_edge = defaults::kFoveationEdge;

// The grid for an output of width x height (uniform when off, when the
// centre covers the screen, or when it would save under 5% of the NR
// pixels).  Snapshot once per evaluate: the encode, guides, NR, look and
// resolve of one frame must agree on it.
inline foveation::Grid CurrentFoveation(
    uint32_t width, uint32_t height, foveation::Inactive* why = nullptr) {
  return foveation::MakeGrid(
      nr_foveation.load(),
      {.center_width = nr_foveation_width.load(),
       .center_height = nr_foveation_height.load(),
       .falloff = nr_foveation_falloff.load(),
       .edge = nr_foveation_edge.load()},
      width, height, why);
}
// An overlay resolution commit arms this so the next evaluation applies the
// new working dimensions immediately, without the kContractDebounceFrames
// settle window (that window protects against game-driven contract
// alternation, not deliberate user changes).
inline std::atomic<bool> nr_resolution_commit_now = false;
inline std::atomic_uint32_t depth_mode = defaults::kDepthMode;
inline std::atomic<float> motion_scale_x_multiplier = defaults::kMvecScale;
inline std::atomic<float> motion_scale_y_multiplier = defaults::kMvecScale;
inline std::atomic<float> paper_white_scale = defaults::kPaperWhiteScale;
// PQ bridge anchor (nits, 80..1000): replaces the hardcoded 203 nits behind
// the PQ normalization (0.0203 = 203/10000).  Control split vs
// paper_white_scale: paper white is the display-referred scene gain the codec
// applies on every mode; diffuse white only relocates the PQ bridge anchor
// and does not multiply the scene on top of it.  Ignored on the SDR path;
// the linear HDR path has no separate diffuse-white constant and is
// untouched.
inline std::atomic<float> diffuse_white_nits = defaults::kDiffuseWhiteNits;
inline std::atomic<float> transfer_strength = defaults::kTransferStrength;
inline std::atomic<float> color_strength = defaults::kColorStrength;
// v9 Phase 6 bounded log-space transfer (see defaults).
inline std::atomic<float> chroma_clamp_stops = defaults::kChromaClampStops;
inline std::atomic<uint32_t> transfer_mode = defaults::kTransferMode;
inline std::atomic<uint32_t> display_pedestal = defaults::kDisplayPedestal;
// The last evaluated frame had nothing for HDR Transfer Strength to scale
// (a Display frame without the pedestal removal): the panel greys it out.
inline std::atomic_bool transfer_strength_inert = false;
inline std::atomic_bool neural_floor_guard = defaults::kNeuralFloorGuard;
// Normalization governor mode and its rate limits (see defaults).
inline std::atomic<uint32_t> norm_governor = defaults::kNormGovernor;
inline std::atomic<float> norm_slew_stops = defaults::kNormSlewStops;
inline std::atomic<float> norm_attack_stops = defaults::kNormAttackStops;
inline std::atomic<float> norm_release_stops = defaults::kNormReleaseStops;
// Counts the frames the governor adopted a candidate outright instead of
// slewing (workset creation, reset-epoch advance, game-signalled cut).  CPU
// side because every input to the decision is CPU side; the committed value
// itself never leaves the GPU.
inline std::atomic_uint64_t norm_snap_count{0};
// The same snaps by reason (a frame can carry several): first governed frame
// of a workset, epoch advance, and the game's own NGX Reset flag.
inline std::atomic_uint64_t norm_snap_prime{0};
inline std::atomic_uint64_t norm_snap_epoch{0};
inline std::atomic_uint64_t norm_snap_reset{0};
// Feed mode (see defaults::kFeedMode), frames scaled by each v2 source,
// exposure view ring entries recycled for a new texture, and frames a full
// ring with no provably idle entry made hold the previous scale.
inline std::atomic<uint32_t> feed_mode = defaults::kFeedMode;
inline std::atomic_uint64_t feed_texture_frames{0};
inline std::atomic_uint64_t feed_fixed_frames{0};
inline std::atomic_uint64_t feed_exposure_ring_recycled{0};
inline std::atomic_uint64_t feed_exposure_ring_full{0};
// Runtime telemetry (telemetry.hpp): log interval in seconds, 0 = off.
inline std::atomic_uint32_t telemetry_seconds = 30;
// Per-frame normalization trace (norm_trace.hpp), diagnostic, default off.
inline std::atomic_bool norm_trace_enabled = false;
// Pool/retirement lifetime counters (telemetry).  created - released - live
// that keeps growing is a retention leak; runtime_mutex guards every writer.
inline uint64_t worksets_created = 0;
inline uint64_t worksets_retired = 0;
// Worksets retired because the pool was full, as opposed to because the
// game changed geometry or the session ended.  The difference matters:
// this is the only path that retires a workset that is still current.
inline uint64_t worksets_evicted = 0;
// One-shot latch for the thrash warning beside it.  Not reset per session
// because there is only ever one session per process.
inline bool logged_workset_thrash = false;
// One-shot latches for the recycle and second-workset lines (EnsureWorkset).
inline bool logged_workset_recycle = false;
inline bool logged_second_workset = false;
inline uint64_t worksets_released = 0;
inline uint64_t nr_features_created = 0;
inline uint64_t nr_features_retired = 0;
inline uint64_t nr_features_released = 0;
inline uint32_t next_workset_id = 1;

// Per-pass strength lookups (stack pass index, 0-based).  Pass 1 IS the
// original single-pass controls - the existing NRIntensity /
// NRTransferStrength / NRColorStrength config keys and overlay sliders - so
// single-pass configurations behave and persist exactly as before.  Passes
// 2..kMaxNrPasses have their own controls, shown in the overlay as passes are
// enabled.
inline float PassIntensity(uint32_t pass_index) {
  return pass_index == 0
      ? intensity.load()
      : pass_intensity[pass_index - 1].load();
}
inline float PassTransferStrength(uint32_t pass_index) {
  return pass_index == 0
      ? transfer_strength.load()
      : pass_transfer_strength[pass_index - 1].load();
}
inline float PassColorStrength(uint32_t pass_index) {
  return pass_index == 0
      ? color_strength.load()
      : pass_color_strength[pass_index - 1].load();
}
// A model-steering value for a stack pass: the Look section's own for pass 1
// and for every pass whose "Same as pass 1" is on, the pass's otherwise.
template <typename T>
inline T PassModel(uint32_t pass_index, const std::atomic<T>& pass_one,
                   const std::atomic<T> (&passes)[kMaxNrPasses - 1]) {
  return pass_index == 0 || pass_follow[pass_index - 1].load()
      ? pass_one.load()
      : passes[pass_index - 1].load();
}
// The look values one chain shapes with, read once so every pass of a frame
// sees the same ones while the overlay may be changing them.  With Shape
// result off every gain is the identity, and Upsampling still applies: it
// is how NR's edit reaches the output resolution, not a look (rc5; through
// rc4 it needed the look on).
inline look::Settings LookSettingsSnapshot() {
  if (!look_mode.load()) return {.upsample = look_upsample.load()};
  return {
      .strength = look_strength.load(),
      .brighten = look_brighten.load(),
      .darken = look_darken.load(),
      .max_brighten = look_max_brighten.load(),
      .max_darken = look_max_darken.load(),
      .colour = look_colour.load(),
      .hue = look_hue.load(),
      .max_colour = look_max_colour.load(),
      .shadows = look_shadows.load(),
      .midtones = look_midtones.load(),
      .highlights = look_highlights.load(),
      .tone = look_tone.load(),
      .detail = look_detail.load(),
      .halo = look_halo.load(),
      .detail_radius = look_detail_radius.load(),
      .stabilize = look_stabilize.load(),
      .stabilize_ms = look_stabilize_ms.load(),
      .stabilize_detail = look_stabilize_detail.load(),
      .upsample = look_upsample.load(),
  };
}
inline std::atomic_uint32_t last_result = NVSDK_NGX_Result_FAIL_NotInitialized;
inline std::atomic_uint64_t successful_evaluations = 0;
// v6 audit issue 23: a bypass frame (hard-invalid evidence, or an
// all-zero-strength chain that copies through) is NOT a successful NR frame;
// count it separately so "Successful NR frames" means model evaluations.
inline std::atomic_uint64_t bypassed_evaluations = 0;
inline std::atomic_uint64_t captured_guides = 0;
inline std::atomic_uint32_t active_features = 0;
// Latched the first time that count is non-zero, and never cleared.  The
// funnel's nr_feature_ready rung asks "did this stage happen", and every
// other rung in the ladder is cumulative; see funnel.hpp for what reading
// the live count instead cost.
inline std::atomic_bool nr_feature_ever_ready = false;
inline std::atomic_uint32_t last_input_width = 0;
inline std::atomic_uint32_t last_input_height = 0;
inline std::atomic_uint32_t last_output_width = 0;
inline std::atomic_uint32_t last_output_height = 0;
inline std::atomic_uint64_t intercepted_creates = 0;
// One-shot diagnostics: a game can route NGX through a different module copy
// than the one hooked, so the first interception is logged to prove calls
// actually pass through the hooked exports.  Create and skip logging is
// one-shot PER FEATURE ID (not per process): the first create is usually the
// game's DLSS/RR, and a later DLSSG (frame generation) or DeepDVC create is
// exactly the event a field log must not hide.
inline std::atomic_uint32_t logged_create_ids = 0;
// One-shot RESULT line per feature ID (v6.5.3, 007 First Light field log):
// that session's last line was this wrapper's "intercepted" line, so whether
// the runtime's own create ever returned was unknowable from the log.  The
// result line lands immediately after the forwarded call and pairs with the
// intercepted line above.
inline std::atomic_uint32_t logged_create_result_ids = 0;
inline std::atomic_bool logged_first_evaluate = false;
inline std::atomic_uint32_t streamline_hook_attempts = 0;
inline std::atomic_bool logged_no_streamline = false;
inline std::atomic_uint64_t intercepted_evaluations = 0;
// R1 funnel accounting.  `intercepted_evaluations` is the RAW count - every
// evaluate that reached a hook, including ones that were never ours - and it
// keeps that meaning so the archived log corpus stays comparable.  The clean
// denominator is derived from it by subtracting the evaluates that were never
// candidates for injection:
//
//   eligible = seen - not_dlss - mirror - own
//
// `own` is this addon's own feature-18 evaluate re-entering the detoured
// export while InsideDirectCall() is true.  Those inflated `seen=` in every
// release so far, which is why a field ratio computed from it read low even
// on a healthy session.
inline std::atomic_uint64_t own_evaluations = 0;
// Evaluates counted in `seen` whose terminal has not been recorded yet.
//
// T-SILENT asserts that every eligible evaluate either injected or named a
// terminal, and reads any remainder as a leak in the funnel.  An evaluate
// still running is neither, so a verdict emitted from INSIDE one reports a
// leak that does not exist.  That is not hypothetical: when presents stall,
// NgxLifecycleTick drives the whole per-frame lifecycle - telemetry and the
// verdict line included - from the evaluate itself, between the `seen`
// increment and the terminal.  Measured on the T2 `present_starved` lane,
// 2026-09-20: `unaccounted=1`, every time, on an otherwise perfect session.
// The same skew is possible in the field on any multi-threaded host, since
// these counters are read without a snapshot.
inline std::atomic_uint64_t evaluations_in_flight = 0;

struct EvaluateInFlightScope {
  bool counted = false;

  explicit EvaluateInFlightScope(bool count) : counted(count) {
    if (counted) evaluations_in_flight.fetch_add(1, std::memory_order_relaxed);
  }
  ~EvaluateInFlightScope() {
    if (counted) evaluations_in_flight.fetch_sub(1, std::memory_order_relaxed);
  }
  EvaluateInFlightScope(const EvaluateInFlightScope&) = delete;
  EvaluateInFlightScope& operator=(const EvaluateInFlightScope&) = delete;
};

// Submissions that actually carried the addon's recorded GPU work.
inline std::atomic_uint64_t submitted_injections = 0;
// Presents the addon's own handler ran.  The one thing every other counter
// assumes and none of them states: a session in which this stays 0 has not
// failed to engage, it has not been asked to (see funnel.hpp, presents).
inline std::atomic_uint64_t presents_observed = 0;
// The injection gate admitted at least once this session: the restore target
// completed.  Separate from the one-shot LOG flag beside it - a verdict must
// not depend on whether logging was on.
inline std::atomic_bool gate_ever_opened = false;
inline std::atomic_uint64_t intercepted_streamline_evaluations = 0;
inline std::atomic_uint64_t intercepted_streamline_dlss_evaluations = 0;
inline std::atomic_uint64_t intercepted_streamline_tag_calls = 0;
inline std::atomic_uint64_t streamline_direct_fallback_attempts = 0;
inline std::atomic_uint64_t streamline_direct_fallback_successes = 0;
// Per-present aggregate of Streamline tag activity.  Games batch tags across
// several SetTag/SetTagForFrame calls per frame (Monster Hunter Wilds: one
// 5-resource call plus smaller ones), so "last call" values read as
// "no guides" whenever the final call carries a single buffer type outside
// the tracked set.  The mask ORs every call in the present; the count is the
// largest single batch.
inline std::atomic_uint32_t streamline_tag_mask_this_present = 0;
inline std::atomic_uint32_t streamline_tag_max_batch = 0;
// One-shot contract diagnostic: logs a subrect/dimension mismatch only once so
// the evaluate hot path does not flood the log (see ProcessInline).
inline std::atomic_bool logged_contract_note = false;
// One-shot multi-mip diagnostic: a mip-chained DLSS output is legal for NGX
// but rare (e.g. Resonance: A Plague Tale Legacy); log once that NR replaces
// mip 0 only instead of failing silently.
inline std::atomic_bool logged_multimip_output = false;
// One-shot diagnostic for evaluates that decline on the native NGX path
// because the game's parameter block omits classic guide pointers (SL-integrated
// engines supply them via tags instead).  Makes the fallback dependency
// visible instead of silent.
inline std::atomic_bool logged_native_decline = false;
// One-shot warning for evaluates skipped because the create/evaluate contract
// carries no guide (input) dimensions at all.  Without the latch this repeats
// on every DLSS evaluate (60x/s).
inline std::atomic_bool logged_no_guide_dims = false;
// One-shot diagnostic for NR being withheld from a registered non-DLSS NGX
// feature (e.g. DLSSG multi-frame generation or DeepDVC).  Their evaluate
// parameter blocks can look DLSS-shaped, so this proves the feature-ID gate
// engaged instead of leaving the skip silent.  One-shot per feature ID.
inline std::atomic_uint32_t logged_skip_ids = 0;
inline std::atomic_bool logged_ngx_missing_guides = false;
inline std::atomic_bool logged_ngx_output_geometry = false;
inline std::atomic_bool logged_pre_sr_geometry = false;
inline std::atomic_bool logged_streamline_capture = false;
inline std::atomic_bool logged_streamline_states = false;
// Per-present record of which NR passes already ran on each game output.
// Multi-pass v5: the value is the list of source handles that processed the
// output this present.  The same handle evaluating the same output twice is a
// mirror (Streamline public API + NGX plugin submit one viewport twice) and is
// skipped; a different handle from the SAME source family is a legitimate
// second pass (two NGX features chaining into one target) and runs.  A
// real-NGX handle paired with a synthetic Streamline handle on one output is
// the mirroring case again and stays skipped - otherwise NR would feed on its
// own output and corrupt the temporal state.
inline std::unordered_map<const ID3D12Resource*, std::vector<const void*>>
    processed_outputs;
constexpr size_t kMaxPassesPerOutput = 4;
// Addresses of the synthetic Streamline fallback handles (map-node storage in
// streamline_handle_keys, see ProcessStreamlineInline).  Used only to
// distinguish the mirroring case in processed_outputs; never dereferenced.
inline std::unordered_set<const void*> synthetic_handles;
inline uint64_t present_generation = 1;

// Every hook family that can fail to install shares this schedule, because
// three hand-written copies of a backoff is three chances to write the one
// that latches.  Returns the delay it scheduled, and sets `announce` on the
// failures worth a log line: the first, and the first to reach the ceiling.
// A module that can never hook therefore costs two lines per session.
inline uint64_t NoteHookInstallFailure(uint32_t& attempts,
                                       uint64_t& next_attempt_present,
                                       bool& announce) {
  ++attempts;
  const uint64_t delay =
      attempts >= 18
          ? kNgxRetryCeilingPresents
          : (std::min)(static_cast<uint64_t>((1ull << attempts) - 1ull),
                       kNgxRetryCeilingPresents);
  next_attempt_present = present_generation + delay;
  announce = attempts == 1 || delay == kNgxRetryCeilingPresents;
  return delay;
}

inline int64_t SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Runtime telemetry window (NRTelemetrySeconds, telemetry.hpp; emitted by
// EmitTelemetry from the lifecycle tick).  One line per interval answers what
// a "slow after a while / after changing a setting" report raises: frame
// pacing, the injected chain's CPU cost, the process's video memory, and
// whether every workset and NR feature the addon retired was actually
// released.  runtime_mutex guards all of it.
inline telemetry::Pacing telemetry_pacing;
inline int64_t telemetry_previous_present_ns = 0;
inline int64_t telemetry_session_start_ns = 0;
inline int64_t telemetry_window_start_ns = 0;
inline uint64_t telemetry_window_ticks = 0;
inline uint64_t telemetry_window_start_evals = 0;
inline uint64_t telemetry_window_nr_calls = 0;
inline int64_t telemetry_window_nr_ns = 0;
inline int64_t telemetry_window_nr_max_ns = 0;
// A lease taken at each emission and checked at the next: still pending a
// whole interval later proves an idle tracked queue starves every-queue
// leases, the retention class retired NR features depend on.
inline GpuLease telemetry_probe_lease;
inline bool telemetry_probe_taken = false;

// CPU cost of one injected chain (ProcessInline / ProcessInlinePreSR), timed
// by the evaluate hooks while they hold runtime_mutex.
inline void RecordInjectionCpu(int64_t started_ns) {
  const int64_t elapsed = SteadyNowNs() - started_ns;
  ++telemetry_window_nr_calls;
  telemetry_window_nr_ns += elapsed;
  telemetry_window_nr_max_ns = std::max(telemetry_window_nr_max_ns, elapsed);
}

// Present-starvation fallback state.  ReShade stops delivering present
// events for the rest of the session when the game switches to a swapchain
// created without the proxy device, while NGX keeps evaluating once per
// rendered frame (observed in Alan Wake 2: presents stalled for minutes
// while ngx evaluations advanced 1:1).  Nearly everything above is keyed on
// present_generation as the per-frame clock, so while the present event is
// silent the NGX evaluate path drives synthetic lifecycle ticks to keep
// captures, hotkeys, meters, retirement, and feature retries alive.
inline std::atomic<int64_t> last_present_steady_ns = 0;
inline std::atomic<int64_t> last_synthetic_tick_ns = 0;
inline std::atomic_bool synthetic_tick_active = false;
// No present event for this long while NGX evaluates continue counts as
// starvation.  Synthetic ticks then pace at most once per ~2 ms so
// multi-feature or dual-export frames collapse into a single tick.
inline constexpr int64_t kPresentStarvationNs = 250'000'000;
inline constexpr int64_t kSyntheticTickMinIntervalNs = 2'000'000;
// Defined with the OnPresent lifecycle below; the NGX evaluate hooks call it.
inline void NgxLifecycleTick();

// Multi-pass admission: returns false when a pass from `handle` on `resource`
// must stay skipped for this present (map is cleared per present).  The pass
// is recorded only on success (processed_outputs[resource].push_back).
inline bool OutputPassAllowed(
    const ID3D12Resource* resource,
    const void* handle) {
  const auto passes = processed_outputs.find(resource);
  if (passes == processed_outputs.end()) return true;
  const bool new_synthetic = synthetic_handles.count(handle) != 0;
  for (const void* ran : passes->second) {
    if (ran == handle) return false;
    // A real-NGX handle meeting a synthetic Streamline handle on one output is
    // the Streamline mirroring case, not a second pass.
    if ((synthetic_handles.count(ran) != 0) != new_synthetic) return false;
  }
  return passes->second.size() < kMaxPassesPerOutput;
}

// 0 = per-present map above, 1 = per-evaluate call nesting
// (evaluate_chain.hpp).  Read once at load.
inline std::atomic<uint32_t> dedupe_mode = kDedupePerEvaluate;

// The admission every injection path shares.  A mirror is recognized from
// the call chain in both modes; the per-present rule applies in mode 0.
inline bool AdmitOutputPass(const ID3D12Resource* resource, const void* handle) {
  if (synthetic_handles.count(handle) == 0) evaluate_chain.ngx_owned = true;
  if (evaluate_chain.Injected(resource)) {
    CountNrDecline(NrDeclineReason::kNestedMirror);
    return false;
  }
  if (dedupe_mode.load(std::memory_order_relaxed) == kDedupePerPresent
      && !OutputPassAllowed(resource, handle)) {
    CountNrDecline(NrDeclineReason::kDuplicateOutputThisPresent);
    return false;
  }
  return true;
}

inline void RecordOutputPass(const ID3D12Resource* resource, const void* handle) {
  processed_outputs[resource].push_back(handle);
  evaluate_chain.MarkInjected(resource);
}

inline decltype(&slEvaluateFeature) real_streamline_evaluate = nullptr;
inline decltype(&slSetTag) real_streamline_set_tag = nullptr;
inline decltype(&slSetTagForFrame) real_streamline_set_tag_for_frame = nullptr;

inline void Log(reshade::log::level level, const std::string& message) {
  reshade::log::message(level, ("DLSS5 Generic: " + message).c_str());
}

// Logs the first foreign-producer signal of each kind; only a strong one
// latches the stand-down (see foreign_nr_seen), whose evaluates then count
// foreign_nr.
inline void NoteForeignNr(bool strong, const char* what) {
  if (!strong) {
    if (!logged_foreign_nr_weak.exchange(true)) {
      Log(reshade::log::level::warning,
          std::string("another Neural Rendering producer may be active in this"
                      " process: ")
              + what + "; observed only - this addon keeps running NR");
    }
    return;
  }
  foreign_nr_seen.store(true, std::memory_order_release);
  if (!logged_foreign_nr_strong.exchange(true)) {
    Log(reshade::log::level::warning,
        std::string("another Neural Rendering producer is active in this"
                    " process: ")
            + what
            + (foreign_nr_yield.load(std::memory_order_relaxed)
                   ? "; this addon stands down so no frame is enhanced twice"
                     " (ForeignNr=yield; set ForeignNr=observe to keep NR"
                     " running)"
                   : "; ForeignNr=observe - this addon keeps running NR"));
  }
}

// The evaluate side of a pending teardown (teardown_pending).  Caller holds
// runtime_mutex.  An evaluate on the device NR was initialized on means the
// swapchain destroy was not the end of that device - a fullscreen or
// frame-generation change recreating its swapchain - so NR resumes on the
// state it has: nothing was released, and v6 never released it at all.  An
// evaluate on any other device passes through (teardown_pending): taking it
// would re-initialize NR for that device, which releases the old state at
// once (EnsureDirectRuntime), and that is the release the proof is for.
inline bool NrWaitsForTeardown(ID3D12GraphicsCommandList* command_list) {
  if (!teardown_pending.load(std::memory_order_acquire)) return false;
  bool same_device = false;
  ID3D12Device* device = nullptr;
  if (direct_device != nullptr
      && SUCCEEDED(command_list->GetDevice(IID_PPV_ARGS(&device)))
      && device != nullptr) {
    same_device =
        renodx::utils::directx::SameNativeObject(direct_device, device);
    device->Release();
  }
  if (!same_device) return true;
  if (teardown_pending.exchange(false, std::memory_order_acq_rel)) {
    Log(reshade::log::level::info,
        "the game kept rendering on the device NR was initialized on after"
        " its swapchain was destroyed; NR continues on its current state"
        " (nothing was released)");
  }
  return false;
}

inline std::string NarrowPath(const wchar_t* path) {
  std::string result;
  for (const wchar_t* p = path; *p != L'\0'; ++p) {
    result.push_back(*p < 128 ? static_cast<char>(*p) : '_');
  }
  return result;
}

inline const char* SourceFeatureName(NVSDK_NGX_Feature feature) {
  switch (feature) {
    case kFeatureDlssd: return "DLSSD/RR";
    case kFeatureDlss: return "DLSS/DLAA";
    case NVSDK_NGX_Feature_FrameGeneration: return "DLSSG/FrameGeneration";
    case NVSDK_NGX_Feature_DeepDVC: return "DeepDVC";
    case kFeatureDlssNr: return "DLSSNR/reserved-18";
    default: return "other NGX feature";
  }
}

inline uint32_t StreamlineTagMask(
    const sl::ResourceTag* tags,
    uint32_t count) {
  uint32_t mask = 0;
  if (tags == nullptr) return mask;
  for (uint32_t i = 0; i < count; ++i) {
    if (tags[i].resource == nullptr) continue;
    switch (tags[i].type) {
      case sl::kBufferTypeDepth: mask |= 1u << 0; break;
      case sl::kBufferTypeMotionVectors: mask |= 1u << 1; break;
      case sl::kBufferTypeScalingInputColor: mask |= 1u << 2; break;
      case sl::kBufferTypeScalingOutputColor: mask |= 1u << 3; break;
      case sl::kBufferTypeHUDLessColor: mask |= 1u << 4; break;
      default: break;
    }
  }
  return mask;
}

// Called from both tag hooks; the CAS loop is an atomic max.
inline void RecordStreamlineTagBatch(uint32_t num_tags, uint32_t tag_mask) {
  streamline_tag_mask_this_present.fetch_or(tag_mask, std::memory_order_relaxed);
  uint32_t largest = streamline_tag_max_batch.load(std::memory_order_relaxed);
  while (num_tags > largest
         && !streamline_tag_max_batch.compare_exchange_weak(
             largest, num_tags, std::memory_order_relaxed)) {
  }
}

inline StreamlineResourceRef ToStreamlineResourceRef(const sl::ResourceTag& tag) {
  StreamlineResourceRef result;
  if (tag.resource == nullptr || tag.resource->native == nullptr) return result;

  // A Streamline resource may be a ReShade proxy.  QueryInterface is used as
  // an ABI-safe D3D12 test; no blind ID3D12Resource cast is allowed here.
  auto* unknown = reinterpret_cast<IUnknown*>(tag.resource->native);
  ID3D12Resource* native_resource = nullptr;
  if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&native_resource)))
      || native_resource == nullptr) {
    return result;
  }
  const D3D12_RESOURCE_DESC desc = native_resource->GetDesc();
  result.resource = native_resource;
  result.owns_reference = true;
  result.state = tag.resource->state;
  result.extent = tag.extent;
  if (!result.extent) {
    result.extent.width = static_cast<uint32_t>(desc.Width);
    result.extent.height = desc.Height;
  }
  return result;
}

inline void CaptureStreamlineTagSet(
    uint32_t viewport,
    const sl::ResourceTag* tags,
    uint32_t count,
    uint32_t frame) {
  if (tags == nullptr && count != 0) return;
  auto& state = streamline_viewports[viewport];
  state.frame = frame;
  for (uint32_t i = 0; i < count; ++i) {
    if (tags[i].resource == nullptr) {
      state.resources.erase(tags[i].type);
      continue;
    }
    StreamlineResourceRef resource = ToStreamlineResourceRef(tags[i]);
    if (resource.resource != nullptr) {
      state.resources[tags[i].type] = std::move(resource);
    }
  }
}

inline void CaptureStreamlineLocalTags(
    const sl::BaseStructure** inputs,
    uint32_t count,
    uint32_t& viewport,
    bool& has_viewport,
    StreamlineViewportState& local) {
  if (inputs == nullptr) return;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t chain_length = 0;
    for (const sl::BaseStructure* current = inputs[i];
         current != nullptr && chain_length++ < 64;
         current = current->next) {
      if (current->structType == sl::ViewportHandle::s_structType) {
        viewport = static_cast<uint32_t>(
            *static_cast<const sl::ViewportHandle*>(current));
        has_viewport = true;
      } else if (current->structType == sl::ResourceTag::s_structType) {
        const auto* tag = static_cast<const sl::ResourceTag*>(current);
        if (tag->resource == nullptr) continue;
        StreamlineResourceRef resource = ToStreamlineResourceRef(*tag);
        if (resource.resource != nullptr) {
          local.resources[tag->type] = std::move(resource);
        }
      }
    }
  }
}

inline StreamlineCapture BuildStreamlineCapture(
    const sl::FrameToken& frame,
    const sl::BaseStructure** inputs,
    uint32_t count) {
  StreamlineCapture capture;
  capture.frame = static_cast<uint32_t>(frame);
  StreamlineViewportState local;
  CaptureStreamlineLocalTags(
      inputs, count, capture.viewport, capture.has_viewport, local);
  if (inputs != nullptr) {
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t chain_length = 0;
      for (const sl::BaseStructure* current = inputs[i];
           current != nullptr && chain_length++ < 64;
           current = current->next) {
        if (current->structType == sl::Constants::s_structType) {
          const auto* constants = static_cast<const sl::Constants*>(current);
          capture.has_constants = true;
          capture.depth_inverted = constants->depthInverted == sl::eTrue;
          capture.reset = constants->reset == sl::eTrue;
          capture.motion_scale_x = constants->mvecScale.x;
          capture.motion_scale_y = constants->mvecScale.y;
          capture.jitter_x = constants->jitterOffset.x;
          capture.jitter_y = constants->jitterOffset.y;
        } else if (current->structType == sl::DLSSOptions::s_structType) {
          const auto* options = static_cast<const sl::DLSSOptions*>(current);
          capture.has_options = true;
          capture.hdr = options->colorBuffersHDR == sl::eTrue;
        }
      }
    }
  }

  if (capture.has_viewport) {
    const auto it = streamline_viewports.find(capture.viewport);
    if (it != streamline_viewports.end()) {
      local.resources.insert(it->second.resources.begin(), it->second.resources.end());
    }
  }
  const auto take = [&local](sl::BufferType type) -> StreamlineResourceRef {
    const auto it = local.resources.find(type);
    if (it == local.resources.end()) return {};
    return std::move(it->second);
  };
  capture.color = take(sl::kBufferTypeScalingInputColor);
  if (capture.color.resource == nullptr) {
    capture.color = take(sl::kBufferTypeHUDLessColor);
  }
  capture.output = take(sl::kBufferTypeScalingOutputColor);
  capture.motion = take(sl::kBufferTypeMotionVectors);
  capture.depth = take(sl::kBufferTypeDepth);
  if (capture.depth.resource == nullptr) {
    capture.depth = take(sl::kBufferTypeLinearDepth);
  }
  return capture;
}

inline HMODULE FindNgxCoreModule() {
  NoteLoaderCall("FindNgxCoreModule/GetModuleHandleW");
  constexpr const wchar_t* kModuleNames[] = {
      L"_nvngx.dll",
      L"nvngx.dll",
      L"nvngx_dlss.dll",
      L"nvngx_dlssd.dll",
  };
  for (const wchar_t* name : kModuleNames) {
    if (HMODULE module = GetModuleHandleW(name); module != nullptr) {
      return module;
    }
  }
  return nullptr;
}

inline uint32_t GetUInt(
    const NVSDK_NGX_Parameter* parameters,
    const char* name,
    uint32_t fallback = 0) {
  uint32_t value = fallback;
  return parameters != nullptr
             && NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetUI(
                 const_cast<NVSDK_NGX_Parameter*>(parameters), name, &value))
             ? value
             : fallback;
}

inline int32_t GetInt(
    const NVSDK_NGX_Parameter* parameters,
    const char* name,
    int32_t fallback = 0) {
  int32_t value = fallback;
  return parameters != nullptr
             && NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(
                 const_cast<NVSDK_NGX_Parameter*>(parameters), name, &value))
             ? value
             : fallback;
}

inline float GetFloat(
    const NVSDK_NGX_Parameter* parameters,
    const char* name,
    float fallback = 0.f) {
  float value = fallback;
  return parameters != nullptr
             && NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetF(
                 const_cast<NVSDK_NGX_Parameter*>(parameters), name, &value))
             ? value
             : fallback;
}

// Presence-aware read: a game that never set a parameter must stay
// distinguishable from one that set it to zero (guide-contract semantics).
inline bool GetFloatIfPresent(
    const NVSDK_NGX_Parameter* parameters,
    const char* name,
    float& value) {
  return parameters != nullptr
      && NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetF(
             const_cast<NVSDK_NGX_Parameter*>(parameters), name, &value));
}

// DLSS-NR's private contract asks the host to provide this callback during
// feature creation.  The NR feature always runs 1:1 (the low-resolution
// scaling contract was probed and rejected by shipped runtimes with
// 0xbad00005), so the deterministic answer is a ratio of 1.
inline NVSDK_NGX_Result NVSDK_CONV DlssNrScalingRatioCallback(
    NVSDK_NGX_Parameter* parameters) {
  if (parameters == nullptr) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  NVSDK_NGX_Parameter_SetF(parameters, "DLSSNR.ScalingRatio", 1.f);
  return NVSDK_NGX_Result_Success;
}

inline ID3D12Resource* GetD3D12Resource(
    const NVSDK_NGX_Parameter* parameters,
    const char* name) {
  ID3D12Resource* value = nullptr;
  return parameters != nullptr
             && NVSDK_NGX_SUCCEED(parameters->Get(name, &value))
             ? value
             : nullptr;
}

inline void ReleaseCom(IUnknown*& object) {
  if (object == nullptr) return;
  object->Release();
  object = nullptr;
}

template <typename T>
inline void ReleaseCom(T*& object) {
  if (object == nullptr) return;
  object->Release();
  object = nullptr;
}

inline void** FindImportAddress(HMODULE module, const char* import_name) {
  if (module == nullptr || import_name == nullptr) return nullptr;
  auto* base = reinterpret_cast<unsigned char*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
  const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (directory.VirtualAddress == 0 || directory.Size == 0) return nullptr;

  auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
  for (; descriptor->Name != 0; ++descriptor) {
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(
        base + (descriptor->OriginalFirstThunk != 0
                    ? descriptor->OriginalFirstThunk
                    : descriptor->FirstThunk));
    auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
    for (; names->u1.AddressOfData != 0; ++names, ++addresses) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<const char*>(import->Name), import_name) == 0) {
        return reinterpret_cast<void**>(&addresses->u1.Function);
      }
    }
  }
  return nullptr;
}

inline DWORD WINAPI CallerGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) {
  if (module == addon_module && filename != nullptr && size != 0) {
    constexpr wchar_t kIdentity[] = L"nvngx.dll";
    constexpr size_t kLength = std::size(kIdentity) - 1;
    if (kLength >= size) {
      filename[0] = L'\0';
      SetLastError(ERROR_INSUFFICIENT_BUFFER);
      return size;
    }
    std::memcpy(filename, kIdentity, sizeof(kIdentity));
    return static_cast<DWORD>(kLength);
  }
  return real_get_module_filename != nullptr
             ? real_get_module_filename(module, filename, size)
             : 0;
}

inline bool InstallCallerIdentity() {
  if (get_module_filename_iat != nullptr) return true;
  get_module_filename_iat = FindImportAddress(direct_api.module, "GetModuleFileNameW");
  if (get_module_filename_iat == nullptr) {
    Log(
        reshade::log::level::error,
        "signed feature has no GetModuleFileNameW import; the nvngx_dlssnr.dll in"
        " the addon folder is not a valid signed NGX runtime - replace it with an"
        " official NVIDIA build and restart");
    return false;
  }

  DWORD old_protection = 0;
  if (!VirtualProtect(
          get_module_filename_iat,
          sizeof(*get_module_filename_iat),
          PAGE_READWRITE,
          &old_protection)) {
    get_module_filename_iat = nullptr;
    Log(
        reshade::log::level::error,
        "failed to make signed-feature IAT writable; security software may be"
        " protecting nvngx_dlssnr.dll - add an exclusion for the game folder and"
        " restart");
    return false;
  }

  original_get_module_filename = *get_module_filename_iat;
  real_get_module_filename = reinterpret_cast<GetModuleFileNameWFn>(original_get_module_filename);
  *get_module_filename_iat = reinterpret_cast<void*>(&CallerGetModuleFileNameW);
  FlushInstructionCache(
      GetCurrentProcess(), get_module_filename_iat, sizeof(*get_module_filename_iat));
  DWORD ignored = 0;
  VirtualProtect(
      get_module_filename_iat,
      sizeof(*get_module_filename_iat),
      old_protection,
      &ignored);
  return true;
}

inline void RestoreCallerIdentity() {
  if (get_module_filename_iat == nullptr) return;
  DWORD old_protection = 0;
  if (VirtualProtect(
          get_module_filename_iat,
          sizeof(*get_module_filename_iat),
          PAGE_READWRITE,
          &old_protection)) {
    *get_module_filename_iat = original_get_module_filename;
    FlushInstructionCache(
        GetCurrentProcess(), get_module_filename_iat, sizeof(*get_module_filename_iat));
    DWORD ignored = 0;
    VirtualProtect(
        get_module_filename_iat,
        sizeof(*get_module_filename_iat),
        old_protection,
        &ignored);
  }
  get_module_filename_iat = nullptr;
  original_get_module_filename = nullptr;
  real_get_module_filename = nullptr;
}

// The addon directory never changes for the life of the process, and the
// resolving GetModuleFileNameW takes the loader lock - row 17's audit found
// the direct-runtime init (under runtime_mutex) calling this on every init,
// an inversion the instrument could not see.  Resolve once, then answer from
// the cache so locked callers make no loader call at all.  The one resolving
// call happens at addon load, before any runtime_mutex can exist, and is
// deliberately NOT noted: a note here would break the EnableHooks=0 inert
// contract (loader_total=0), and GetModuleFileNameW re-enters the loader
// lock only recursively, so DllMain is not a hazard for it - the runtime_mutex
// order is, and the cache is what closes that.
inline const std::filesystem::path& AddonDirectory() {
  static const std::filesystem::path addon_dir = [] {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(addon_module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return std::filesystem::path();
    path.resize(length);
    return std::filesystem::path(path).parent_path();
  }();
  return addon_dir;
}

// ReShade.ini belongs to this game even when AddonPath is shared.
inline const std::filesystem::path& ReShadeBaseDirectory() {
  static const std::filesystem::path base = renodx::utils::path::GetReShadeBasePath();
  return base;
}

// Search order for the signed NR runtime: the addon's own folder first (an
// explicit user placement always wins), then the game executable's folder -
// ReShade setups with a central AddonPath load the addon far from the game
// while the runtime sits beside the exe.  Field logs showed exactly that
// class failing with "dll was not found beside the addon".
inline std::filesystem::path FindNrRuntimePath() {
  const std::filesystem::path primary = AddonDirectory() / L"nvngx_dlssnr.dll";
  std::error_code ignored;
  if (std::filesystem::exists(primary, ignored)) return primary;
  std::wstring exe(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
  if (length != 0 && length < exe.size()) {
    exe.resize(length);
    const std::filesystem::path fallback =
        std::filesystem::path(exe).parent_path() / L"nvngx_dlssnr.dll";
    if (std::filesystem::exists(fallback, ignored)) return fallback;
  }
  return primary;
}

// Cached by path, size and write time (v7.0.0-rc10): a device-rebuild
// teardown unmaps the runtime and the re-arm loads it again, and hashing its
// ~20 MB again cost 156-207 ms on the re-arming thread at every rebuild -
// an alt-tab hitch in games that rebuild on focus changes.  Caller holds
// ngx_loader_prime_mutex, which guards the cache.
inline std::string ComputeRuntimeSha256(const std::filesystem::path& path) {
  struct CachedHash {
    std::filesystem::path path;
    uintmax_t size = 0;
    std::filesystem::file_time_type written;
    std::string sha256;
  };
  static CachedHash cached;
  std::error_code stat_error;
  const uintmax_t size = std::filesystem::file_size(path, stat_error);
  const std::filesystem::file_time_type written =
      stat_error ? std::filesystem::file_time_type{}
                 : std::filesystem::last_write_time(path, stat_error);
  if (!stat_error && !cached.sha256.empty() && cached.path == path
      && cached.size == size && cached.written == written) {
    return cached.sha256;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (FAILED(BCryptOpenAlgorithmProvider(
          &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
    return {};
  }
  std::string result;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (FAILED(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return {};
  }
  char buffer[65536];
  while (file) {
    file.read(buffer, sizeof(buffer));
    const std::streamsize read = file.gcount();
    if (read <= 0) break;
    BCryptHashData(
        hash, reinterpret_cast<PUCHAR>(buffer), static_cast<ULONG>(read), 0);
  }
  unsigned char digest[32];
  if (FAILED(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return {};
  }
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (const unsigned char byte : digest) {
    result.push_back(kHex[byte >> 4]);
    result.push_back(kHex[byte & 0xF]);
  }
  if (!stat_error) cached = {path, size, written, result};
  return result;
}

// ---- everything the Windows loader has to be asked for --------------------
//
// The addon needs two modules resolved before NR can run at all: the
// signed NR runtime (LoadLibraryW + seven exports) and, when the game ships a
// public NGX core, that module's parameter and feature exports.  Both are
// process-global, both are resolved at most once, and both take the LOADER
// LOCK - so neither may be reached while runtime_mutex is held
// (runtime_lock.hpp has the deadlock).
//
// It used to be reached that way on two paths.  The lazy one is the first
// engaged evaluate: HookedEvaluateFeature takes runtime_mutex, then
// ProcessInline -> EnsureDirectRuntime -> LoadDirectApi -> LoadLibraryW.  The
// eager one is OnInitDevice, whose whole point is to do the load OUTSIDE the
// detoured evaluate chain - and which took runtime_mutex anyway, for a reason
// its own comment stated: "LoadDirectApi writes non-atomic state (direct_api,
// direct_load_state, the sha256 string) that EnsureDirectRuntime reads under
// runtime_mutex, so this eager load must hold the mutex too."
//
// That reason was correct, and this is the way out of it.  The loader results
// land HERE, in a block filled by one thread at a time (its own small mutex,
// which no runtime_mutex holder ever takes) and PUBLISHED as an IMMUTABLE
// SNAPSHOT behind one release store.  Readers - all of them under
// runtime_mutex, unchanged - acquire the pointer and read a block nothing
// will ever write again, so they need no lock of their own, they see a
// consistent set of symbols rather than a half-updated one, and they make
// no loader call.
//
// A snapshot rather than a mutable block behind a ready flag, because the
// flag version has a teardown race that is easy to miss: Shutdown clears
// the flag and then wipes the block, while a reader that loaded the flag one
// instruction earlier is still copying a std::string out of it.  Taking the
// prime's mutex in the reader would fix that and reintroduce the deadlock
// this whole change is about - a thread holding runtime_mutex would then be
// waiting on a thread holding the loader lock.  Publishing by pointer has
// neither problem.
//
// The cost, stated rather than hidden: a retired snapshot is never freed,
// because a reader may still be inside it and this addon has no reclamation
// scheme worth adding at this size.  A session publishes one for the
// runtime, one for the NGX core, and one per device rebuild.
//
// The sha256 of the ~20 MB runtime comes along for the ride: it is file IO
// with nothing to do with the lock, and it was being done under it.
struct NgxLoaderSymbols {
  // Readiness travels WITH the symbols rather than beside them, so one
  // acquire-load of the snapshot pointer gives a reader a consistent answer
  // to "is it resolved" and "what is it" together.
  bool nr_ready = false;
  // The runtime was looked for and is not usable.  Latched, so the error is
  // logged once rather than on every evaluate.
  bool nr_failed = false;
  bool core_ready = false;
  // The signed NR runtime, nvngx_dlssnr.dll.
  HMODULE nr_module = nullptr;
  DirectInitFn init = nullptr;
  AllocateParametersFn allocate = nullptr;
  DestroyParametersFn destroy = nullptr;
  DirectCreateFn create = nullptr;
  DirectEvaluateFn evaluate = nullptr;
  DirectReleaseFn release = nullptr;
  DirectShutdownFn shutdown = nullptr;
  std::filesystem::path nr_path;
  std::string nr_sha256;
  std::string nr_file_version;
  // The game's public NGX core, when it has one.
  HMODULE core = nullptr;
  AllocateParametersFn core_allocate = nullptr;
  DestroyParametersFn core_destroy = nullptr;
  DirectCreateFn core_create = nullptr;
  DirectEvaluateFn core_evaluate = nullptr;
  DirectReleaseFn core_release = nullptr;
};

// The published snapshot.  Null until the first prime; never written after
// publication.  Readers acquire-load it and nothing else.
inline std::atomic<const NgxLoaderSymbols*> ngx_loader_published{nullptr};
// The prime's own working copy, touched only under the mutex below, and the
// mutex, which no holder of runtime_mutex ever takes.
inline NgxLoaderSymbols ngx_loader_staging;
inline std::mutex ngx_loader_prime_mutex;

// What a reader sees.  An empty snapshot when nothing has been published, so
// callers need no null check and cannot forget one.
inline const NgxLoaderSymbols& NgxLoaderView() noexcept {
  static const NgxLoaderSymbols kNone;
  const NgxLoaderSymbols* published =
      ngx_loader_published.load(std::memory_order_acquire);
  return published != nullptr ? *published : kNone;
}

// Copy the staging block into a fresh snapshot and publish it.  Caller holds
// ngx_loader_prime_mutex.  A failed allocation simply leaves the previous
// snapshot in place: NR stays as it was rather than losing its symbols.
inline void PublishNgxLoaderSymbols() {
  const auto* snapshot = new (std::nothrow) NgxLoaderSymbols(ngx_loader_staging);
  if (snapshot != nullptr) {
    ngx_loader_published.store(snapshot, std::memory_order_release);
  }
}

// The signed NR runtime names the oldest driver it runs on in its version
// resource: `NGXMinimumDriverVersion` (615.00 on 310.8.0).  The NGX core reads
// that for every snippet it loads and refuses one that asks for a newer
// driver ("snippet requires newer driver"); this addon drives the runtime
// directly, so nothing checked it, and a user on an older driver saw NR
// silently not work (Borderlands 4, driver 595.97, 2026-09-23).  The installed
// driver is System32\nvapi64.dll's file version, which NVIDIA stamps
// 32.0.1X.YYYY for driver XYY.YY (616.56 = 32.0.16.1656).  Observe-only: one
// line, no decline, and none when either version cannot be read.  Makes
// loader calls, so it runs from the prime, with no runtime_mutex held.
inline void LogNrDriverRequirement(const std::filesystem::path& runtime) {
  const auto read_version_info = [](const std::wstring& path) {
    std::vector<char> block;
    DWORD ignored = 0;
    NoteLoaderCall("LogNrDriverRequirement/GetFileVersionInfoW",
                   LoaderCallSafety::kUnsafeUnderLoaderLock);
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return block;
    block.resize(size);
    if (GetFileVersionInfoW(path.c_str(), 0, size, block.data()) == FALSE) block.clear();
    return block;
  };

  std::vector<char> runtime_info = read_version_info(runtime.wstring());
  if (runtime_info.empty()) return;
  VS_FIXEDFILEINFO* runtime_version = nullptr;
  UINT version_bytes = 0;
  if (VerQueryValueW(runtime_info.data(), L"\\", reinterpret_cast<void**>(&runtime_version),
                     &version_bytes)
      && runtime_version != nullptr && version_bytes >= sizeof(VS_FIXEDFILEINFO)
      && runtime_version->dwSignature == 0xFEEF04BD) {
    ngx_loader_staging.nr_file_version =
        std::to_string(HIWORD(runtime_version->dwFileVersionMS)) + "."
        + std::to_string(LOWORD(runtime_version->dwFileVersionMS)) + "."
        + std::to_string(HIWORD(runtime_version->dwFileVersionLS)) + "."
        + std::to_string(LOWORD(runtime_version->dwFileVersionLS));
  }
  // The string table the runtime's own translation entry names, then the two
  // US-English tables NVIDIA builds use (310.8.0 ships 040904E4).
  std::vector<std::wstring> tables;
  WORD* translation = nullptr;
  UINT translation_bytes = 0;
  if (VerQueryValueW(runtime_info.data(), L"\\VarFileInfo\\Translation",
                     reinterpret_cast<void**>(&translation), &translation_bytes)
      && translation_bytes >= 2 * sizeof(WORD)) {
    wchar_t table[9] = {};
    swprintf_s(table, L"%04x%04x", translation[0], translation[1]);
    tables.emplace_back(table);
  }
  tables.emplace_back(L"040904E4");
  tables.emplace_back(L"040904B0");
  double minimum = 0.0;
  for (const std::wstring& table : tables) {
    wchar_t* value = nullptr;
    UINT length = 0;
    if (VerQueryValueW(runtime_info.data(),
                       (L"\\StringFileInfo\\" + table + L"\\NGXMinimumDriverVersion").c_str(),
                       reinterpret_cast<void**>(&value), &length)
        && value != nullptr && length > 1) {
      minimum = wcstod(value, nullptr);
      break;
    }
  }
  if (minimum <= 0.0) return;

  wchar_t system_dir[MAX_PATH] = {};
  const UINT system_length = GetSystemDirectoryW(system_dir, MAX_PATH);
  if (system_length == 0 || system_length >= MAX_PATH) return;
  std::vector<char> driver_info =
      read_version_info(std::wstring(system_dir) + L"\\nvapi64.dll");
  VS_FIXEDFILEINFO* fixed = nullptr;
  UINT fixed_bytes = 0;
  if (driver_info.empty()
      || !VerQueryValueW(driver_info.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixed_bytes)
      || fixed == nullptr || fixed->dwSignature != 0xFEEF04BD) {
    return;
  }
  const uint32_t driver_x100 = (HIWORD(fixed->dwFileVersionLS) % 10) * 10000
                               + LOWORD(fixed->dwFileVersionLS);
  const auto minimum_x100 = static_cast<uint32_t>(std::lround(minimum * 100.0));
  char text[256] = {};
  const bool too_old = driver_x100 < minimum_x100;
  if (too_old) {
    snprintf(text, sizeof(text),
             "NVIDIA driver %u.%02u is older than %u.%02u, the minimum the signed"
             " NR runtime declares; NGX refuses this runtime on this driver, so"
             " Neural Rendering is not expected to work - update the NVIDIA"
             " driver to %u.%02u or newer",
             driver_x100 / 100, driver_x100 % 100, minimum_x100 / 100,
             minimum_x100 % 100, minimum_x100 / 100, minimum_x100 % 100);
  } else {
    snprintf(text, sizeof(text),
             "NVIDIA driver %u.%02u meets the signed NR runtime's declared"
             " minimum %u.%02u",
             driver_x100 / 100, driver_x100 % 100, minimum_x100 / 100,
             minimum_x100 % 100);
  }
  Log(too_old ? reshade::log::level::warning : reshade::log::level::info, text);
}

// Set in the process environment once this addon has loaded the NR runtime
// (see PrimeNgxLoaderSymbols).
inline constexpr wchar_t kNrRuntimeLoadedMarker[] = L"RENODX_DLSS5_NR_RUNTIME_LOADED";

// Ask the loader for everything, with no addon lock held.  Cheap and
// lock-free once both halves are resolved; safe to call on every evaluate and
// every present, which is what keeps a late-loading NGX core from being
// missed.
inline void PrimeNgxLoaderSymbols() {
  {
    const NgxLoaderSymbols& published = NgxLoaderView();
    if ((published.nr_ready || published.nr_failed) && published.core_ready) {
      return;
    }
  }
  if (HoldingRuntimeLock()) {
    ngx_loader_prime_inversions.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  std::scoped_lock prime(ngx_loader_prime_mutex);
  if (lastgasp::dumps_on.load(std::memory_order_relaxed)
      && lastgasp::write_minidump.load(std::memory_order_acquire) == nullptr) {
    NoteLoaderCall("crash-diagnostics/PrepareDumps", LoaderCallSafety::kUnsafeUnderLoaderLock);
    lastgasp::PrepareDumps();
  }
  NgxLoaderSymbols& symbols = ngx_loader_staging;

  if (!symbols.nr_ready && !symbols.nr_failed) {
    // Note (disassembly, 310.8.0): the runtime carries the NGX framework's
    // env/registry logging config (NGXCore LogLevel/LogPath, __NGX_LOG_LEVEL
    // env) but its InitCommon/NGXInitLog are dead code — the legacy Init
    // export that reaches them is stubbed to FAIL_FeatureNotSupported and no
    // stored pointers reference them.  The runtime's internal log cannot be
    // enabled; behavioral probing and our own captures are the only ground
    // truth available.
    symbols.nr_path = FindNrRuntimePath();
    // The process-wide marker names this addon's own earlier load: ReShade
    // unloads and reloads add-ons with the devices that hold them (a game's
    // start-up probe device is enough), and DetachLite leaves the runtime
    // mapped on purpose, so every reload after the first found it mapped and
    // warned of "another producer" - in the rc8 UE5 reports among others.
    // The environment block outlives the addon's image; nothing else sets it.
    if (GetModuleHandleW(L"nvngx_dlssnr.dll") != nullptr
        && GetEnvironmentVariableW(kNrRuntimeLoadedMarker, nullptr, 0) == 0) {
      NoteForeignNr(false, "nvngx_dlssnr.dll was already loaded before this"
                           " addon first loaded it");
    }
    NoteLoaderCall("PrimeNgxLoaderSymbols/LoadLibraryW",
                   LoaderCallSafety::kUnsafeUnderLoaderLock);
    HMODULE module = LoadLibraryW(symbols.nr_path.c_str());
    const DWORD load_error = module == nullptr ? GetLastError() : 0;
    if (module != nullptr) {
      SetEnvironmentVariableW(kNrRuntimeLoadedMarker, L"1");
    }
    if (module == nullptr) {
      // FindNrRuntimePath falls back to the addon folder when neither
      // candidate exists, so a file at the path means it is there and did
      // not load: a different fix from placing it, and a different line
      // (until rc10 both said "was not found").
      std::error_code probe;
      const bool present = std::filesystem::exists(symbols.nr_path, probe);
      nr_runtime_fault.store(
          static_cast<uint8_t>(present ? ui::RuntimeFault::kUnusable
                                       : ui::RuntimeFault::kMissing),
          std::memory_order_relaxed);
      Log(
          reshade::log::level::error,
          present
              ? "nvngx_dlssnr.dll is at " + NarrowPath(symbols.nr_path.c_str())
                    + " but Windows could not load it (error "
                    + std::to_string(load_error)
                    + (load_error == ERROR_BAD_EXE_FORMAT
                           ? ": not a 64-bit DLL, or a damaged file"
                           : "")
                    + "); replace it with NVIDIA's signed 64-bit"
                      " nvngx_dlssnr.dll and restart the game. NR stays off"
                      " until then."
              : "nvngx_dlssnr.dll was not found beside the addon ("
                    + NarrowPath(AddonDirectory().c_str())
                    + ") or the game executable; place NVIDIA's signed"
                      " nvngx_dlssnr.dll in either folder and restart the"
                      " game. NR stays off until then.");
      symbols.nr_failed = true;
      PublishNgxLoaderSymbols();
    } else {
      NoteLoaderCall("PrimeNgxLoaderSymbols/GetProcAddress");
      symbols.init = reinterpret_cast<DirectInitFn>(
          GetProcAddress(module, "NVSDK_NGX_D3D12_Init_Ext"));
      symbols.allocate = reinterpret_cast<AllocateParametersFn>(
          GetProcAddress(module, "NVSDK_NGX_D3D12_AllocateParameters"));
      symbols.destroy = reinterpret_cast<DestroyParametersFn>(
          GetProcAddress(module, "NVSDK_NGX_D3D12_DestroyParameters"));
      symbols.create = reinterpret_cast<DirectCreateFn>(
          GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature"));
      symbols.evaluate = reinterpret_cast<DirectEvaluateFn>(
          GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature"));
      symbols.release = reinterpret_cast<DirectReleaseFn>(
          GetProcAddress(module, "NVSDK_NGX_D3D12_ReleaseFeature"));
      symbols.shutdown = reinterpret_cast<DirectShutdownFn>(
          GetProcAddress(module, "NVSDK_NGX_D3D12_Shutdown1"));
      if (symbols.init == nullptr || symbols.create == nullptr
          || symbols.evaluate == nullptr || symbols.release == nullptr
          || symbols.shutdown == nullptr) {
        Log(
            reshade::log::level::error,
            "signed feature is missing a required D3D12 export; "
                + NarrowPath(symbols.nr_path.c_str())
                + " is not a usable signed NGX runtime - replace it with an official"
                  " NVIDIA build and restart");
        NoteLoaderCall("PrimeNgxLoaderSymbols/FreeLibrary",
                       LoaderCallSafety::kUnsafeUnderLoaderLock);
        FreeLibrary(module);
        symbols = {};
        symbols.nr_failed = true;
        nr_runtime_fault.store(static_cast<uint8_t>(ui::RuntimeFault::kUnusable),
                               std::memory_order_relaxed);
        PublishNgxLoaderSymbols();
      } else {
        // Identify the deployed runtime build (log + overlay only, see
        // kExpectedNrSha256).  Never fatal: diagnostics rely on swapping
        // builds.  ~20 MB of file IO, which is exactly why it does not
        // belong on the far side of a lock every present takes.
        symbols.nr_sha256 = ComputeRuntimeSha256(symbols.nr_path);
        LogNrDriverRequirement(symbols.nr_path);
        symbols.nr_module = module;
        symbols.nr_ready = true;
        PublishNgxLoaderSymbols();
      }
    }
  }

  if (!symbols.core_ready) {
    HMODULE core = FindNgxCoreModule();
    if (core != nullptr) {
      NoteLoaderCall("PrimeNgxLoaderSymbols/core-GetProcAddress");
      const auto allocate = reinterpret_cast<AllocateParametersFn>(
          GetProcAddress(core, "NVSDK_NGX_D3D12_AllocateParameters"));
      const auto destroy = reinterpret_cast<DestroyParametersFn>(
          GetProcAddress(core, "NVSDK_NGX_D3D12_DestroyParameters"));
      const auto create = reinterpret_cast<DirectCreateFn>(
          GetProcAddress(core, "NVSDK_NGX_D3D12_CreateFeature"));
      const auto evaluate = reinterpret_cast<DirectEvaluateFn>(
          GetProcAddress(core, "NVSDK_NGX_D3D12_EvaluateFeature"));
      const auto release = reinterpret_cast<DirectReleaseFn>(
          GetProcAddress(core, "NVSDK_NGX_D3D12_ReleaseFeature"));
      if (allocate != nullptr && destroy != nullptr && create != nullptr
          && evaluate != nullptr && release != nullptr) {
        symbols.core = core;
        symbols.core_allocate = allocate;
        symbols.core_destroy = destroy;
        symbols.core_create = create;
        symbols.core_evaluate = evaluate;
        symbols.core_release = release;
        symbols.core_ready = true;
        PublishNgxLoaderSymbols();
      }
    }
  }
}

// Give the loader back what it gave us.  Called with NO addon lock held, for
// the same reason as the prime: FreeLibrary takes the loader lock.  The flags
// are cleared BEFORE the unmap, so a thread that arrives in between re-primes
// from scratch instead of adopting a module that is being unmapped.
inline void ReleaseNgxLoaderSymbols(HMODULE unload) {
  {
    std::scoped_lock prime(ngx_loader_prime_mutex);
    ngx_loader_staging = {};
    // A fresh empty snapshot, so a reader mid-copy keeps reading the old
    // one and every reader after this sees "nothing resolved".
    PublishNgxLoaderSymbols();
  }
  if (unload != nullptr) {
    NoteLoaderCall("ReleaseNgxLoaderSymbols/FreeLibrary",
                   LoaderCallSafety::kUnsafeUnderLoaderLock);
    FreeLibrary(unload);
  }
}

// Adopt what the prime resolved.  Runs under runtime_mutex like every other
// reader of direct_api, and - this is the point - asks the loader for
// nothing.  Returns false while the prime has not produced a runtime yet; the
// callers all prime immediately before taking the lock, so in practice that
// is only the session where there is no runtime to find.
inline bool LoadDirectApi() {
  if (direct_load_state == DirectLoadState::Ready && direct_api.module != nullptr) {
    return true;
  }
  if (direct_load_state == DirectLoadState::Failed) return false;
  // One acquire-load, and the block it hands back is immutable: the
  // readiness and the symbols cannot disagree, and nothing rewrites them
  // while this copies them out.
  const NgxLoaderSymbols& symbols = NgxLoaderView();
  if (symbols.nr_failed) {
    last_result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    direct_load_state = DirectLoadState::Failed;
    return false;
  }
  if (!symbols.nr_ready) return false;
  direct_api.module = symbols.nr_module;
  direct_api.init = symbols.init;
  direct_api.allocate = symbols.allocate;
  direct_api.destroy = symbols.destroy;
  direct_api.create = symbols.create;
  direct_api.evaluate = symbols.evaluate;
  direct_api.release = symbols.release;
  direct_api.shutdown = symbols.shutdown;
  direct_runtime_sha256 = symbols.nr_sha256;
  direct_runtime_reference_match = direct_runtime_sha256 == kExpectedNrSha256;
  Log(
      direct_runtime_reference_match ? reshade::log::level::info
                                     : reshade::log::level::warning,
      "signed runtime sha256 "
          + (direct_runtime_sha256.empty()
                 ? std::string("(unavailable)")
                 : direct_runtime_sha256)
          + (direct_runtime_reference_match
                 ? " (reference match)"
                 : " (custom runtime accepted; untested build, NR failures may "
                   "be specific to it)"));
  // An IAT patch on an already-loaded module: no loader lock, so it stays
  // here with the rest of the state it writes.  Its failure path no longer
  // unmaps the runtime, because the prime owns that reference now and a
  // FreeLibrary here would leave the published block holding a dangling
  // module.  Shutdown unmaps it; a mapped-but-unused NVIDIA runtime is the
  // cheaper of the two mistakes, and the existing "loaded but never
  // initialized; leaving it mapped" path already makes that trade.
  if (!InstallCallerIdentity()) {
    direct_api = {};
    last_result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    direct_load_state = DirectLoadState::Failed;
    nr_runtime_fault.store(static_cast<uint8_t>(ui::RuntimeFault::kUnusable),
                           std::memory_order_relaxed);
    return false;
  }
  direct_load_state = DirectLoadState::Ready;
  return true;
}

inline void ReleaseFinalResources(FinalResources& resources) {
  ReleaseCom(resources.descriptors);
  ReleaseCom(resources.block_mean);
  ReleaseCom(resources.decoded_alt);
  ReleaseCom(resources.decoded);
  ReleaseCom(resources.nr_output);
  ReleaseCom(resources.proxy);
  ReleaseCom(resources.original);
  ReleaseCom(resources.pre_sr_color);
  ReleaseCom(resources.norm_commit);
  ReleaseCom(resources.norm_scale);
  ReleaseCom(resources.work_b);
  ReleaseCom(resources.work_a);
  ReleaseCom(resources.work0);
  ReleaseCom(resources.look_output);
  ReleaseCom(resources.look_band_a);
  ReleaseCom(resources.look_band_b);
  ReleaseCom(resources.look_band_max_a);
  ReleaseCom(resources.look_band_max_b);
  ReleaseCom(resources.look_up_proxy);
  ReleaseCom(resources.look_up_neural);
  ReleaseCom(resources.look_trace_histogram);
  ReleaseCom(resources.look_trace_blocks);
  for (ID3D12Resource*& motion : resources.motion_views) ReleaseCom(motion);
  for (ID3D12Resource*& exposure : resources.exposure_views) ReleaseCom(exposure);
  ReleaseCom(resources.fov_motion);
  ReleaseCom(resources.fov_depth);
  for (ID3D12Resource*& view : resources.fov_depth_views) ReleaseCom(view);
  for (ID3D12Resource*& view : resources.fov_motion_views) ReleaseCom(view);
  ReleaseCom(resources.pre_sr_source);
  resources = {};
}

// Every GPU object a workset recording can reference.  Keep this list equal to
// the actual product path: frame surfaces, the same-frame autoscale resource,
// the governor commit texel, the look surfaces and the game motion vectors the
// motion ring views, and the shader-visible descriptor heap.  There are
// deliberately no readback resources in v6; norm_commit is the only
// cross-frame normalization state and it is GPU-resident.
// Feed v2's exposure ring entries are deliberately absent: PrepareFrameScale
// tracks the one texture a frame binds, which is what lets a full ring prove
// an entry idle and recycle it (listing them here tracked every entry on
// every evaluate, so none ever was).  Retirement still covers them: every
// list that binds an exposure set binds this workset's heap, listed below.
inline void CollectWorksetGpuObjects(
    const FinalResources& res, std::vector<const void*>& objects) {
  for (const ID3D12Resource* look_object :
       {res.look_output, res.look_band_a, res.look_band_b, res.look_band_max_a,
        res.look_band_max_b, res.look_up_proxy, res.look_up_neural,
        res.look_trace_histogram, res.look_trace_blocks, res.motion_views[0],
        res.motion_views[1], res.motion_views[2], res.motion_views[3]}) {
    if (look_object != nullptr) objects.push_back(look_object);
  }
  static_assert(kMotionRingSize == 4, "list every motion ring slot above");
  if (res.original != nullptr) objects.push_back(res.original);
  if (res.proxy != nullptr) objects.push_back(res.proxy);
  if (res.nr_output != nullptr) objects.push_back(res.nr_output);
  if (res.decoded != nullptr) objects.push_back(res.decoded);
  if (res.decoded_alt != nullptr) objects.push_back(res.decoded_alt);
  if (res.pre_sr_color != nullptr) objects.push_back(res.pre_sr_color);
  if (res.pre_sr_source != nullptr) objects.push_back(res.pre_sr_source);
  if (res.block_mean != nullptr) objects.push_back(res.block_mean);
  if (res.work0 != nullptr) objects.push_back(res.work0);
  if (res.work_a != nullptr) objects.push_back(res.work_a);
  if (res.work_b != nullptr) objects.push_back(res.work_b);
  if (res.norm_scale != nullptr) objects.push_back(res.norm_scale);
  if (res.norm_commit != nullptr) objects.push_back(res.norm_commit);
  if (res.fov_motion != nullptr) objects.push_back(res.fov_motion);
  if (res.fov_depth != nullptr) objects.push_back(res.fov_depth);
  for (const ID3D12Resource* view : res.fov_depth_views) {
    if (view != nullptr) objects.push_back(view);
  }
  for (const ID3D12Resource* view : res.fov_motion_views) {
    if (view != nullptr) objects.push_back(view);
  }
  if (res.descriptors != nullptr) objects.push_back(res.descriptors);
}

// True when no recorded or submitted command list can still reach any of the
// workset's GPU objects (the submission tracker's exact proof).  Retirement
// frees on it; the pool re-keys an idle workset on it.
inline bool WorksetRecordingsComplete(const FinalResources& res) {
  std::vector<const void*> objects;
  CollectWorksetGpuObjects(res, objects);
  for (const void* object : objects) {
    if (!submission::ResourceReleasable(object)) return false;
  }
  return true;
}

// Registers the workset's GPU objects against the command list's current
// recording generation: the exact submission-use proof retirement consumes
// instead of the every-queue lease guess (issue 01).  Called once per
// evaluate after the workset is ensured; the autoscale and exposure
// work recorded later in the same flow belong to the same generation
// (D3D12 requires recording to finish before the first submit).
inline void TrackWorksetRecording(
    ID3D12GraphicsCommandList* command_list, const FinalResources& res) {
  std::vector<const void*> objects;
  CollectWorksetGpuObjects(res, objects);
  for (const void* object : objects) {
    submission::TrackUse(command_list, object);
  }
}

// Set once the ExecuteCommandLists detour (and its post-submit signal
// fences) is live - see InstallQueueCompletionHooks.  Retirement and the
// exposure-probe generation swap read it to decide whether exact
// submission-use proofs exist; before install (and from a device-rebuild
// teardown to the next present) they fall back to the every-queue lease
// proof.  Since v6.5.3 the detour also stays
// GPU-inert until the first tracked use arms the tracker (see
// HookedQueueExecuteCommandLists); every entry that reads exact proofs here
// was necessarily recorded after that arming, so the meaning is unchanged.
inline std::atomic_bool queue_tracking_active = false;

// Cumulative ExecuteCommandLists forwards the detour left completely alone:
// hooks installed, but no tracked use was ever recorded (v6.5.3 inert gate,
// see HookedQueueExecuteCommandLists).  A passive session - NR declined by
// the restore-target gate while the game initializes frame generation, the
// 007 First Light crash report's posture - shows inert= growing and
// queues=0 in telemetry: the mod appended nothing to any foreign submit.
inline std::atomic_uint64_t queue_detour_inert_submits = 0;

inline void RetireWorksetResources(FinalResources&& resources) {
  if (resources.original == nullptr) return;
  ++worksets_retired;
  retired_final_resources.push_back(
      {std::move(resources),
       present_generation,
       AcquireGpuLease(),
       // The queue hook was live for every recording that used this workset,
       // so the tracker's exact proofs - not the every-queue lease guess -
       // decide its release (issue 01: an idle queue can no longer star the
       // retirement, and an unrelated queue's submission can no longer
       // "prove" it).
       queue_tracking_active.load(std::memory_order_relaxed)});
}

// Retires one workset's resources on the settle queue (the evaluates that may
// still reference them are in flight on a command list).  Caller holds
// runtime_mutex.
inline void RetireWorkset(const WorksetKey& key) {
  const auto it = worksets.find(key);
  if (it == worksets.end()) return;
  RetireWorksetResources(std::move(it->second));
  worksets.erase(it);
}

inline void RetireAllWorksets() {
  for (auto& [_, resources] : worksets) {
    RetireWorksetResources(std::move(resources));
  }
  worksets.clear();
}

inline void MarkWorksetUsed(FinalResources* resources) {
  resources->last_used_generation = present_generation;
  resources->last_used_ns = SteadyNowNs();
}

// Idle retirement: a workset its own stream has moved away from.  Until rc5
// only the NRMaxWorksets cap (LRU) or the game's feature release freed one,
// so outputs a game wrote once kept their scratch for the session: Alan
// Wake 2 re-creating its DLSS evaluated into four outputs within a second,
// and three worksets then held ~675 MiB idle to the end (rc4 field log).  A
// workset retires once it has been idle kWorksetIdleRetireNs while a sibling
// of the SAME game feature served within the last second.  A stream that
// pauses as a whole (a menu without DLSS) keeps its worksets - no sibling is
// recent - and rotating outputs are each used every few frames.  Retirement
// is fence-backed like every other.  Caller holds runtime_mutex.
inline constexpr int64_t kWorksetIdleRetireNs = 5'000'000'000;
inline uint32_t worksets_idle_retired_logged = 0;
inline void RetireSupersededWorksets(int64_t now_ns) {
  for (auto it = worksets.begin(); it != worksets.end();) {
    const int64_t idle_ns = now_ns - it->second.last_used_ns;
    const bool superseded = idle_ns >= kWorksetIdleRetireNs
        && std::any_of(worksets.begin(), worksets.end(), [&](const auto& other) {
             return other.first.handle == it->first.handle
                 && now_ns - other.second.last_used_ns < 1'000'000'000;
           });
    if (!superseded) {
      ++it;
      continue;
    }
    if (worksets_idle_retired_logged < 16) {
      ++worksets_idle_retired_logged;
      std::ostringstream message;
      message << "NR workset ws" << it->second.id << " retired: idle "
              << idle_ns / 1'000'000'000 << " s while its stream serves another"
              << " output (frees " << (it->second.allocated_bytes >> 20) << " MiB)";
      Log(reshade::log::level::info, message.str());
    }
    RetireWorksetResources(std::move(it->second));
    it = worksets.erase(it);
  }
}

// Immediate release (device-rebuild shutdown path): nothing will evaluate
// against these again.
inline void ReleaseAllWorksets() {
  for (auto& [_, resources] : worksets) {
    ReleaseFinalResources(resources);
  }
  worksets.clear();
}

inline void RefreshActiveFeatureCount() {
  uint32_t count = 0;
  for (const auto& [_, state] : features) {
    for (const auto& slot : state.slots) {
      if (slot.handle != nullptr) ++count;
    }
  }
  active_features = count;
  // The one place the count goes up, so the one place the latch belongs.
  if (count != 0) nr_feature_ever_ready.store(true, std::memory_order_relaxed);
}

// Releases one NR feature slot (stack pass) of a game feature state.
inline void ReleaseNrSlot(
    FeatureState& state,
    uint32_t slot_index,
    bool retire = false) {
  NrFeatureSlot& slot = state.slots[slot_index];
  if (slot.handle == nullptr && slot.parameters == nullptr) return;
  if (slot.handle != nullptr) {
    ++(retire ? nr_features_retired : nr_features_released);
  }
  if (retire) {
    retired_nr_features.push_back({
        .handle = slot.handle,
        .parameters = slot.parameters,
        .via_core = slot.via_core,
        .generation = present_generation,
        .lease = AcquireGpuLease(),
        .tracker_proof = slot.tracked_since_create
            && queue_tracking_active.load(std::memory_order_relaxed),
        .look_history = {slot.look_history.buffers[0], slot.look_history.buffers[1]},
    });
  } else {
    for (ID3D12Resource*& buffer : slot.look_history.buffers) ReleaseCom(buffer);
    if (slot.handle != nullptr) {
      DirectCallScope ngx_direct_call;
      if (slot.via_core && core_release_feature != nullptr) {
        core_release_feature(slot.handle);
      } else if (direct_api.release != nullptr) {
        direct_api.release(slot.handle);
      }
    }
    if (slot.parameters != nullptr && core_destroy_parameters != nullptr) {
      core_destroy_parameters(slot.parameters);
    }
  }
  slot.handle = nullptr;
  slot.parameters = nullptr;
  slot.via_core = false;
  slot.tracked_since_create = false;
  slot.look_history = {};
  RefreshActiveFeatureCount();
}

// Releases every NR feature slot of every registered feature (contract-level
// change: dimensions, path, or configuration changed for all stack passes).
inline void ReleaseAllNrSlots(FeatureState& state, bool retire = false) {
  for (uint32_t slot_index = 0; slot_index < kMaxNrPasses; ++slot_index) {
    ReleaseNrSlot(state, slot_index, retire);
  }
}

inline void ReleaseFeatureState(FeatureState& state) {
  ReleaseAllNrSlots(state);
}

inline void ReleaseFeatureStates() {
  for (auto& [_, state] : features) ReleaseFeatureState(state);
  features.clear();
  // The game's features and their create contracts still exist after re-arm.
  active_features = 0;
}

// Retired features and resources are released on the present thread only once
// their GPU lease completed (every tracked queue's fence passed the values
// recorded at retirement). If no queue fence could be created, completion is
// unknown: retain the object until teardown rather than treating CPU presents
// as evidence that GPU work finished. This intentionally trades bounded memory
// growth in a broken/untracked queue configuration for use-after-free safety.

// Bounded automatic retry after an NR feature failure.  A transient failure
// (driver reset, level load with a dimension change, a one-off rejected
// contract) used to latch `failed` until the user opened the overlay, and
// until v6.5.2 cost 60 presents of silent NR-off even when the very next
// attempt would have succeeded.  The first failure retries on the next
// evaluate; each further consecutive failure waits 1, 2, 4 ... s capped at
// 30 s, so a permanently rejected contract re-fails at most that often.
// Wall clock, not presents: frame generation and the present-starved
// lifecycle ticks both run the present clock at a different rate.
constexpr int64_t kFeatureRetryFirstNs = 1'000'000'000;
constexpr int64_t kFeatureRetryMaxNs = 30'000'000'000;

inline bool FeatureFailureExpired(const NrFeatureSlot& slot) {
  if (slot.fail_count == 0) return true;
  return SteadyNowNs() - slot.failed_ns
      >= std::min(kFeatureRetryFirstNs << std::min(slot.fail_count - 1, 5u),
                  kFeatureRetryMaxNs);
}

inline void ReleaseRetiredNrFeature(const RetiredNrFeature& retired) {
  if (retired.handle != nullptr) {
    DirectCallScope ngx_direct_call;
    if (retired.via_core && core_release_feature != nullptr) {
      core_release_feature(retired.handle);
    } else if (direct_api.release != nullptr) {
      direct_api.release(retired.handle);
    }
  }
  if (retired.parameters != nullptr && core_destroy_parameters != nullptr) {
    core_destroy_parameters(retired.parameters);
  }
  for (ID3D12Resource* buffer : retired.look_history) {
    if (buffer != nullptr) buffer->Release();
  }
}

// Requires runtime_mutex. Releases only entries with a positive completion
// proof: exact submission-use proofs where the queue hook was live (issue 01),
// the every-queue lease otherwise.  Empty leases remain retained until
// FlushRetiredResources at teardown.
inline void DrainRetiredResources() {
  // Advance the exact proofs first: drop closed generations whose
  // submissions all completed.
  submission::PruneCompletedGenerations();
  // A removed device will never execute the retired work, so releasing is
  // safe - but that must be diagnosed, not mistaken for a proven completion.
  static std::atomic<bool> logged_device_removal{false};
  static std::atomic<bool> logged_unproven_retirement{false};
  const auto lease_released = [&](const GpuLease& lease) {
    if (lease.empty()) {
      if (!logged_unproven_retirement.exchange(true)) {
        Log(reshade::log::level::warning,
            "D3D12 retirement deferred until teardown: no GPU completion "
            "fence was available; CPU present count is not a completion proof");
      }
      return false;
    }
    const GpuLeaseState state = QueryGpuLease(lease);
    if (state == GpuLeaseState::kDeviceRemoved
        && !logged_device_removal.exchange(true)) {
      Log(reshade::log::level::error,
          "D3D12 device removed: retired NR resources released without "
          "proven GPU completion");
    }
    return state != GpuLeaseState::kPending;
  };
  const auto workset_released = [&](const RetiredFinalResources& entry) {
    return entry.tracker_proof ? WorksetRecordingsComplete(entry.resources)
                               : lease_released(entry.lease);
  };
  for (auto it = retired_final_resources.begin();
       it != retired_final_resources.end();) {
    if (workset_released(*it)) {
      ReleaseFinalResources(it->resources);
      ++worksets_released;
      it = retired_final_resources.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = retired_nr_features.begin(); it != retired_nr_features.end();) {
    if (it->tracker_proof ? submission::ResourceReleasable(it->handle)
                                && submission::ResourceReleasable(it->look_history[0])
                                && submission::ResourceReleasable(it->look_history[1])
                          : lease_released(it->lease)) {
      ReleaseRetiredNrFeature(*it);
      if (it->handle != nullptr) ++nr_features_released;
      it = retired_nr_features.erase(it);
    } else {
      ++it;
    }
  }
}

// Requires runtime_mutex. Releases every retired entry immediately - used
// before the direct runtime shuts down, after which its stale handles could
// no longer be released safely.
inline void FlushRetiredResources() {
  for (auto& retired : retired_final_resources) {
    ReleaseFinalResources(retired.resources);
    ++worksets_released;
  }
  retired_final_resources.clear();
  for (const RetiredNrFeature& retired : retired_nr_features) {
    ReleaseRetiredNrFeature(retired);
    if (retired.handle != nullptr) ++nr_features_released;
  }
  retired_nr_features.clear();
}

inline void ReleaseCodecPipeline() {
  ReleaseCom(codec_pipeline.black_restore_apply);
  ReleaseCom(codec_pipeline.block_mean_reduce);
  ReleaseCom(codec_pipeline.block_mean_reduce_unshaped);
  ReleaseCom(codec_pipeline.pedestal_reduce_unshaped);
  ReleaseCom(codec_pipeline.decode);
  ReleaseCom(codec_pipeline.encode);
  ReleaseCom(codec_pipeline.autoscale);
  ReleaseCom(codec_pipeline.commit);
  ReleaseCom(codec_pipeline.exposure_scale);
  ReleaseCom(codec_pipeline.commit_exposure);
  ReleaseCom(codec_pipeline.pedestal_reduce);
  ReleaseCom(codec_pipeline.resolve_v6);
  ReleaseCom(codec_pipeline.encode_v6);
  ReleaseCom(codec_pipeline.linearize);
  ReleaseCom(codec_pipeline.fov_guides);
  ReleaseCom(codec_pipeline.root_signature);
  ReleaseCom(codec_pipeline.look.compose);
  ReleaseCom(codec_pipeline.look.band);
  ReleaseCom(codec_pipeline.look.upsample);
  ReleaseCom(codec_pipeline.look.trace);
  ReleaseCom(codec_pipeline.look.root_signature);
  codec_pipeline = {};
}

// The proof a swapchain teardown waits for: every recording that carries NR
// work was submitted and has completed (submission::RecordedWorkComplete).
// It needs the queue detour, which is what attaches submissions to
// recordings; without it only a session that never recorded NR work has
// nothing to wait for, and anything else keeps its state (v6's behavior)
// rather than guess.
inline bool TeardownProofHolds() {
  if (!submission::HasEverTrackedUses()) return true;
  return queue_tracking_active.load(std::memory_order_acquire)
         && submission::RecordedWorkComplete();
}

// Waits for the work already submitted on every tracked queue, bounded to
// 1 s per queue - what a release without per-resource retirement proofs
// leans on.  A removed device (UINT64_MAX) has nothing left to wait for.
// Returns whether every queue finished in time.
inline bool WaitForTrackedQueues() {
  bool finished = true;
  if (HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
    for (const auto& [fence, value] : AcquireGpuLease(0).waits) {
      if (fence->GetCompletedValue() < value
          && SUCCEEDED(fence->SetEventOnCompletion(value, event))
          && WaitForSingleObject(event, 1000) == WAIT_TIMEOUT) {
        finished = false;
      }
    }
    CloseHandle(event);
  }
  return finished;
}

inline bool EnsureDirectRuntime(ID3D12GraphicsCommandList* command_list) {
  if (command_list == nullptr || !LoadDirectApi()) return false;
  if (direct_runtime_failed && direct_device == nullptr) return false;
  if (!parameter_runtime_ready) {
    // Resolved by PrimeNgxLoaderSymbols outside this lock; the acquire pairs
    // with its release store.  `core_ready` false means no public NGX core is
    // loaded (yet) - the same condition the old FindNgxCoreModule() ==
    // nullptr produced, and the prime keeps retrying it, so a core that
    // arrives later is still picked up.
    const NgxLoaderSymbols& symbols = NgxLoaderView();
    const bool core_ready = symbols.core_ready
                            && !bridge_force_snippet.load(std::memory_order_acquire);
    HMODULE core = core_ready ? symbols.core : nullptr;
    const auto allocate = core_ready ? symbols.core_allocate : nullptr;
    const auto destroy = core_ready ? symbols.core_destroy : nullptr;
    const auto create = core_ready ? symbols.core_create : nullptr;
    const auto evaluate = core_ready ? symbols.core_evaluate : nullptr;
    const auto release = core_ready ? symbols.core_release : nullptr;
    // The signed runtime's own allocator is optional: 310.8.0 exports none
    // (the allocator lives in the NGX core), and a block the addon owns is
    // the shape the D0 probe's K1/K2 arms created and evaluated feature 18
    // through (bridge_params.hpp, OwnedParameters).  Until alpha45 its
    // absence failed every session without a public core - the bridge's
    // private device always, and a D3D12 process whose NGX is bundled.
    const bool runtime_allocator =
        direct_api.allocate != nullptr && direct_api.destroy != nullptr;
    const bool direct_ready = direct_api.create != nullptr
        && direct_api.evaluate != nullptr && direct_api.release != nullptr;
    if (!core_ready && !direct_ready) {
      Log(
          reshade::log::level::error,
          "NGX/Streamline runtime has no complete parameter or feature export set");
      last_result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
      direct_runtime_failed = true;
      return false;
    }
    if (core_ready) {
      ngx_core_module = core;
      parameter_runtime_via_core = true;
      core_allocate_parameters = allocate;
      core_destroy_parameters = destroy;
      core_create_feature = create;
      core_evaluate_feature = evaluate;
      core_release_feature = release;
    } else {
      // Streamline can ship its own NGX bridge.  Keep the same parameter
      // contract, but source every operation from the signed runtime when no
      // complete public core is present.
      ngx_core_module = nullptr;
      parameter_runtime_via_core = false;
      core_allocate_parameters = runtime_allocator
          ? direct_api.allocate : bridge::AllocateOwnedParameters;
      core_destroy_parameters = runtime_allocator
          ? direct_api.destroy : bridge::DestroyOwnedParameters;
      Log(reshade::log::level::info,
          runtime_allocator
              ? "NR parameter blocks come from the signed runtime's allocator"
              : "NR parameter blocks are the addon's own: the signed runtime"
                " exports no parameter allocator and no public NGX core serves"
                " this device");
      core_create_feature = nullptr;
      core_evaluate_feature = nullptr;
      core_release_feature = nullptr;
    }
    parameter_runtime_ready = true;
  }
  ID3D12Device* device = nullptr;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    last_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
    return false;
  }

  if (direct_device == device) {
    device->Release();
    return true;
  }
  if (direct_device != nullptr) {
    // Named, not silent (observe-only).  The compare above is by raw
    // pointer on purpose: NGX has to run in the world of the list it records
    // into, so a change of FACE - ReShade's device proxy to the native device
    // or back, the evaluate's list having changed world - re-initializes like
    // a change of device.  That rebuilds every feature and workset and, via
    // FlushRetiredResources, releases retirements without waiting for their
    // proofs; one flip is the Streamline layer arriving mid-session, a steady
    // stream is two paths alternating worlds.  No lane produces either shape,
    // so the line is capped rather than tuned.
    //
    // The frames NR recorded on the old face can still be executing when
    // this runs (the same GPU, one or two frames in flight), so the tracked
    // queues' submitted work is waited for first, as Shutdown does (v7.0.0-
    // rc10; until then the release did not wait at all).  A recording that
    // carries NR work and is not yet submitted cannot be waited for here -
    // the thread that submits it may be this one - so it is counted and
    // named, observe-only: no field log has shown this path (235 logs), and
    // a new decline needs one first (AGENTS.md).
    const bool queues_finished = WaitForTrackedQueues();
    const bool proven = TeardownProofHolds();
    if (!proven) {
      nr_runtime_reinit_unproven.fetch_add(1, std::memory_order_relaxed);
    }
    const uint64_t changes =
        nr_runtime_device_changes.fetch_add(1, std::memory_order_relaxed) + 1;
    if (changes <= kNrRuntimeDeviceChangeLogCap) {
      ID3D12Device* native_face = device;
      const bool proxy_face =
          renodx::utils::directx::NativeFromReShadeProxy(&native_face);
      std::ostringstream message;
      message << "NR's direct runtime re-initializes for "
              << (!renodx::utils::directx::SameNativeObject(direct_device, device)
                      ? "a different D3D12 device"
                  : proxy_face
                      ? "ReShade's proxy of the same D3D12 device (the"
                        " evaluate's command list changed world, and NGX"
                        " records in the world of its list)"
                      : "the native face of the same D3D12 device (the"
                        " evaluate's command list changed world, and NGX"
                        " records in the world of its list)")
              << "; change " << changes
              << (proven ? "; NR's recorded work had completed"
                  : queues_finished
                      ? "; a recording carrying NR work was NOT proven"
                        " complete (unsubmitted, or its queue untracked)"
                      : "; a tracked queue had not finished its submitted"
                        " work after 1 s")
              << (changes == kNrRuntimeDeviceChangeLogCap
                      ? ", further changes are not logged"
                      : "");
      Log(reshade::log::level::warning, message.str());
    }
    ReleaseFeatureStates();
    RetireAllWorksets();
    ReleaseCodecPipeline();
    FlushRetiredResources();
    // Guard re-entrancy like create/evaluate/release do: this runs under
    // runtime_mutex, and if the signed runtime called back into a detoured
    // NGX export the wrapper would try to re-lock the (non-recursive) mutex
    // on this thread and deadlock.
    {
      DirectCallScope ngx_direct_call;
      direct_api.shutdown(direct_device);
    }
    direct_device->Release();
    direct_device = nullptr;
    direct_device_native = nullptr;
    direct_runtime_failed = false;
    parameter_runtime_ready = false;
    parameter_runtime_via_core = false;
    ngx_core_module = nullptr;
  }

  const auto data_path = AddonDirectory();
  NVSDK_NGX_Result result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
  {
    DirectCallScope ngx_direct_call;
    try {
      result = direct_api.init(
          kDirectApplicationId,
          data_path.c_str(),
          device,
          NVSDK_NGX_Version_API,
          nullptr);
    } catch (...) {
      // A C++ exception escaping the signed runtime must not propagate
      // through the detoured evaluate call into the game.  It no longer
      // has to lower the fence as well - leaving the scope does that.
      Log(reshade::log::level::error, "direct Init raised an exception");
    }
  }
  last_result = static_cast<uint32_t>(result);
  if (NVSDK_NGX_FAILED(result)) {
    std::ostringstream message;
    message << "direct Init failed with 0x" << std::hex
            << static_cast<uint32_t>(result)
            << "; NR is unavailable in this session. Update your NVIDIA driver, or"
               " replace nvngx_dlssnr.dll with the reference build (sha256 in the"
               " addon README) and restart";
    Log(reshade::log::level::error, message.str());
    direct_runtime_failed = true;
    device->Release();
    return false;
  }
  direct_device = device;
  direct_device_native = device;
  renodx::utils::directx::NativeFromReShadeProxy(&direct_device_native);
  last_nr_device_native = direct_device_native;
  direct_runtime_failed = false;
  Log(reshade::log::level::info, "signed DLSSNR 310.8.0 D3D12 runtime initialized");
  return true;
}

inline FeatureState DeriveFeatureState(const NVSDK_NGX_Parameter* parameters) {
  FeatureState state;
  state.input_width = GetUInt(
      parameters,
      NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
      GetUInt(parameters, NVSDK_NGX_Parameter_Width));
  state.input_height = GetUInt(
      parameters,
      NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
      GetUInt(parameters, NVSDK_NGX_Parameter_Height));
  // The create-time snapshot of the guide dims: FeatureState::input_width is
  // later overwritten per frame with the applied NR working resolution, but
  // titles that send guide dims only at CreateFeature need them back.
  state.create_input_width = state.input_width;
  state.create_input_height = state.input_height;
  state.output_width = GetUInt(parameters, NVSDK_NGX_Parameter_OutWidth);
  state.output_height = GetUInt(parameters, NVSDK_NGX_Parameter_OutHeight);
  state.motion_x = GetUInt(parameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X);
  state.motion_y = GetUInt(parameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y);
  state.depth_x = GetUInt(parameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X);
  state.depth_y = GetUInt(parameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y);
  // Motion scales are read presence-aware with NO derive-time default: on a
  // per-evaluate parameter block without Width/Height the dims are still zero
  // here (they are filled from the create contract by the caller afterwards),
  // so a dim-derived default baked at this point would silently be zero.  The
  // default is resolved by the caller once the final guide dims are known.
  state.has_motion_scale_x = GetFloatIfPresent(
      parameters, NVSDK_NGX_Parameter_MV_Scale_X, state.motion_scale_x);
  state.has_motion_scale_y = GetFloatIfPresent(
      parameters, NVSDK_NGX_Parameter_MV_Scale_Y, state.motion_scale_y);
  state.has_jitter = GetFloatIfPresent(
                         parameters,
                         NVSDK_NGX_Parameter_Jitter_Offset_X,
                         state.jitter_x)
      && GetFloatIfPresent(
             parameters,
             NVSDK_NGX_Parameter_Jitter_Offset_Y,
             state.jitter_y);
  state.perf_quality = GetInt(parameters, NVSDK_NGX_Parameter_PerfQualityValue, 0);
  state.frame_reset = GetInt(parameters, NVSDK_NGX_Parameter_Reset, 0);
  // Exposure evidence (Phase 0.2): keep the legacy collapsed fields in sync
  // and record the texture desc plus every CPU scalar presence-flagged.
  if (ID3D12Resource* exposure_texture =
          GetD3D12Resource(parameters, NVSDK_NGX_Parameter_ExposureTexture);
      exposure_texture != nullptr) {
    const D3D12_RESOURCE_DESC desc = exposure_texture->GetDesc();
    state.has_exposure_texture = true;
    state.exposure.has_texture = true;
    state.exposure.texture_format = static_cast<uint32_t>(desc.Format);
    state.exposure.texture_width = static_cast<uint32_t>(desc.Width);
    state.exposure.texture_height = desc.Height;
  }
  state.has_pre_exposure = GetFloatIfPresent(
      parameters, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, state.pre_exposure);
  state.exposure.has_pre_exposure = state.has_pre_exposure;
  state.exposure.pre_exposure = state.pre_exposure;
  state.exposure.has_scale = GetFloatIfPresent(
      parameters, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, state.exposure.scale);

  int32_t flags = 0;
  if (NVSDK_NGX_SUCCEED(parameters->Get(
          NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
          &flags))) {
    state.create_flags = static_cast<uint32_t>(flags);
  }
  state.exposure.auto_exposure =
      (state.create_flags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) != 0;
  if (ID3D12Resource* output = GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Output);
      output != nullptr) {
    const auto desc = output->GetDesc();
    if (state.output_width == 0) state.output_width = static_cast<uint32_t>(desc.Width);
    if (state.output_height == 0) state.output_height = desc.Height;
  }
  return state;
}

inline bool IsDlssEvaluation(const NVSDK_NGX_Parameter* parameters) {
  return GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Color) != nullptr
      && GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Output) != nullptr
      && GetD3D12Resource(parameters, NVSDK_NGX_Parameter_MotionVectors) != nullptr
      && GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Depth) != nullptr;
}

// Creates a root signature from a serialization's result, releasing both
// blobs.  The failure lines are whole literals at the call site, because
// log_verdict.py pins the codec's by its literal text.
inline bool CreateRootSignature(
    ID3D12Device* device, HRESULT serialize_result, ID3DBlob* serialized,
    ID3DBlob* errors, ID3D12RootSignature** root_signature, const wchar_t* debug_name,
    const char* serialization_failed, const char* creation_failed) {
  if (FAILED(serialize_result) || serialized == nullptr) {
    std::string detail;
    if (errors != nullptr && errors->GetBufferPointer() != nullptr) {
      detail.assign(
          static_cast<const char*>(errors->GetBufferPointer()),
          errors->GetBufferSize());
    }
    Log(
        reshade::log::level::error,
        std::string(serialization_failed) + detail);
    ReleaseCom(errors);
    ReleaseCom(serialized);
    return false;
  }
  ReleaseCom(errors);
  const HRESULT result = device->CreateRootSignature(
      0,
      serialized->GetBufferPointer(),
      serialized->GetBufferSize(),
      IID_PPV_ARGS(root_signature));
  ReleaseCom(serialized);
  if (FAILED(result) || *root_signature == nullptr) {
    std::ostringstream message;
    message << creation_failed << std::hex << static_cast<uint32_t>(result);
    Log(reshade::log::level::error, message.str());
    return false;
  }
  (*root_signature)->SetName(debug_name);
  return true;
}

// The codec root signature's constant block, in dwords (see the root
// parameter below and V6CodecConstants).
inline constexpr uint32_t kCodecRootConstants = 28;
// Dword index of Foveation in every codec cbuffer view.
// First dword of the foveated grid in every codec view: packed X, packed Y,
// G(1) X, G(1) Y (foveation.hlsli).
inline constexpr uint32_t kCodecFoveationDword = 24;

inline bool EnsureCodecRootSignature(ID3D12Device* device) {
  if (codec_pipeline.root_signature != nullptr) return true;
  NoteFirstDirectxInitialize();
  const char* missing = device == nullptr ? "no D3D12 device"
      : !renodx::utils::directx::Initialize()
          ? "the DirectX helper library did not initialize (d3d12.dll /"
            " d3dcompiler not loadable)"
      : renodx::utils::directx::pD3D12SerializeRootSignature == nullptr
          ? "d3d12.dll exports no D3D12SerializeRootSignature"
          : nullptr;
  if (missing != nullptr) {
    Log(reshade::log::level::error,
        std::string("codec root-signature prerequisites unavailable: ") + missing);
    return false;
  }

  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 4;
  ranges[0].BaseShaderRegister = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 2;
  ranges[1].BaseShaderRegister = 0;

  D3D12_ROOT_PARAMETER root_parameters[3]{};
  root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
  root_parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
  root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  root_parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
  root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  // 28 dwords: the v6/legacy maps (24) plus Foveation at dword 24, rounded to
  // the 16-byte register the HLSL views declare (an undersized root constant
  // block trips the debug layer against a declared cbuffer).
  root_parameters[2].Constants.Num32BitValues = kCodecRootConstants;
  root_parameters[2].Constants.ShaderRegister = 0;

  D3D12_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.NumParameters = 3;
  root_desc.pParameters = root_parameters;
  ID3DBlob* serialized = nullptr;
  ID3DBlob* errors = nullptr;
  const HRESULT result = renodx::utils::directx::pD3D12SerializeRootSignature(
      &root_desc,
      D3D_ROOT_SIGNATURE_VERSION_1,
      &serialized,
      &errors);
  return CreateRootSignature(
      device, result, serialized, errors, &codec_pipeline.root_signature,
      L"DLSS5 Generic Control codec root signature",
      "codec root signature serialization failed: ",
      "codec root signature creation failed with HRESULT 0x");
}

inline bool CreateCodecPipelineState(
    ID3D12Device* device,
    std::span<const uint8_t> bytecode,
    ID3D12PipelineState** pipeline,
    const wchar_t* debug_name,
    const char* log_name,
    ID3D12RootSignature* root_signature = codec_pipeline.root_signature) {
  if (*pipeline != nullptr) return true;
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root_signature;
  desc.CS = {bytecode.data(), bytecode.size()};
  const HRESULT result =
      device->CreateComputePipelineState(&desc, IID_PPV_ARGS(pipeline));
  if (FAILED(result) || *pipeline == nullptr) {
    std::ostringstream message;
    message << log_name << " PSO creation failed with HRESULT 0x" << std::hex
            << static_cast<uint32_t>(result);
    Log(reshade::log::level::error, message.str());
    ReleaseCom(*pipeline);
    return false;
  }
  (*pipeline)->SetName(debug_name);
  return true;
}

inline bool EnsureLegacyCodecPipeline(ID3D12Device* device) {
  if (codec_pipeline.encode != nullptr && codec_pipeline.decode != nullptr
      && codec_pipeline.block_mean_reduce != nullptr
      && codec_pipeline.black_restore_apply != nullptr) {
    return true;
  }
  if (!EnsureCodecRootSignature(device)) return false;
  if (!CreateCodecPipelineState(
          device,
          __legacy_encode,
          &codec_pipeline.encode,
          L"DLSS5 Generic Control proxy encode",
          "legacy proxy encode")
      || !CreateCodecPipelineState(
          device,
          __legacy_decode,
          &codec_pipeline.decode,
          L"DLSS5 Generic Control HDR transfer",
          "legacy proxy decode")
      || !CreateCodecPipelineState(
          device,
          __legacy_reduce,
          &codec_pipeline.block_mean_reduce,
          L"DLSS5 Generic block mean reduce",
          "legacy block mean reduce")
      || !CreateCodecPipelineState(
          device,
          __legacy_apply,
          &codec_pipeline.black_restore_apply,
          L"DLSS5 Generic black level restore",
          "legacy black level restore")) {
    ReleaseCom(codec_pipeline.encode);
    ReleaseCom(codec_pipeline.decode);
    ReleaseCom(codec_pipeline.block_mean_reduce);
    ReleaseCom(codec_pipeline.black_restore_apply);
    return false;
  }
  CreateCodecPipelineState(
      device,
      __legacy_reduce_unshaped,
      &codec_pipeline.block_mean_reduce_unshaped,
      L"DLSS5 Generic block mean reduce (unshaped)",
      "legacy block mean reduce, unshaped");
  Log(
      reshade::log::level::info,
      "created legacy NR codec from build-validated shader bytecode");
  return true;
}

inline bool EnsureV6CodecPipeline(ID3D12Device* device) {
  if (codec_pipeline.linearize != nullptr && codec_pipeline.encode_v6 != nullptr
      && codec_pipeline.resolve_v6 != nullptr
      && codec_pipeline.pedestal_reduce != nullptr
      && codec_pipeline.commit != nullptr && codec_pipeline.autoscale != nullptr) {
    return true;
  }
  if (!EnsureCodecRootSignature(device)) return false;
  if (!CreateCodecPipelineState(
          device,
          __v6_linearize,
          &codec_pipeline.linearize,
          L"DLSS5 Generic v6 linearize",
          "v6 linearize")
      || !CreateCodecPipelineState(
          device,
          __v6_encode,
          &codec_pipeline.encode_v6,
          L"DLSS5 Generic v6 proxy encode",
          "v6 proxy encode")
      || !CreateCodecPipelineState(
          device,
          __v6_resolve,
          &codec_pipeline.resolve_v6,
          L"DLSS5 Generic v6 resolve",
          "v6 resolve")
      || !CreateCodecPipelineState(
          device,
          __v6_pedestal_reduce,
          &codec_pipeline.pedestal_reduce,
          L"DLSS5 Generic v6 pedestal reduce",
          "v6 pedestal reduce")
      || !CreateCodecPipelineState(
          device,
          __v6_commit,
          &codec_pipeline.commit,
          L"DLSS5 Generic v6 commit",
          "v6 commit")
      || !CreateCodecPipelineState(
          device,
          __v6_autoscale,
          &codec_pipeline.autoscale,
          L"DLSS5 Generic v6 same-frame autoscale",
          "v6 same-frame autoscale")) {
    ReleaseCom(codec_pipeline.linearize);
    ReleaseCom(codec_pipeline.encode_v6);
    ReleaseCom(codec_pipeline.resolve_v6);
    ReleaseCom(codec_pipeline.pedestal_reduce);
    ReleaseCom(codec_pipeline.commit);
    ReleaseCom(codec_pipeline.autoscale);
    return false;
  }
  CreateCodecPipelineState(
      device,
      __v6_pedestal_reduce_unshaped,
      &codec_pipeline.pedestal_reduce_unshaped,
      L"DLSS5 Generic v6 pedestal reduce (unshaped)",
      "v6 pedestal reduce, unshaped");
  // Feed v2 is a mode beside v1: if either program fails, FrameFeed runs v1.
  if (!CreateCodecPipelineState(
          device,
          __v6_exposure_scale,
          &codec_pipeline.exposure_scale,
          L"DLSS5 Generic v6 exposure scale (feed v2)",
          "v6 exposure scale")
      || !CreateCodecPipelineState(
          device,
          __v6_commit_exposure,
          &codec_pipeline.commit_exposure,
          L"DLSS5 Generic v6 commit (feed v2)",
          "v6 commit, feed v2")) {
    ReleaseCom(codec_pipeline.exposure_scale);
    ReleaseCom(codec_pipeline.commit_exposure);
  }
  Log(
      reshade::log::level::info,
      "created v6 NR codec from build-validated shader bytecode");
  return true;
}

// The look stage's root signature, serialized by look_stage.hpp so the GPU
// harness binds exactly this layout, and its four programs.  Created on first
// use: a session that never shapes the result or traces the edit never
// builds them.
inline bool EnsureLookPipeline(ID3D12Device* device) {
  look::Pipeline& pipeline = codec_pipeline.look;
  if (pipeline.compose != nullptr && pipeline.band != nullptr
      && pipeline.upsample != nullptr && pipeline.trace != nullptr) {
    return true;
  }
  if (pipeline.root_signature == nullptr) {
    if (renodx::utils::directx::pD3D12SerializeRootSignature == nullptr) {
      Log(reshade::log::level::error,
          "look root-signature prerequisites unavailable: d3d12.dll exports no"
          " D3D12SerializeRootSignature");
      return false;
    }
    ID3DBlob* serialized = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT result = look::SerializeRootSignature(
        renodx::utils::directx::pD3D12SerializeRootSignature, &serialized, &errors);
    if (!CreateRootSignature(
            device, result, serialized, errors, &pipeline.root_signature,
            L"DLSS5 Generic look root signature",
            "look root signature serialization failed: ",
            "look root signature creation failed with HRESULT 0x")) {
      return false;
    }
  }
  if (!CreateCodecPipelineState(
          device, __look_compose, &pipeline.compose, L"DLSS5 Generic look compose",
          "look compose", pipeline.root_signature)
      || !CreateCodecPipelineState(
          device, __look_band, &pipeline.band, L"DLSS5 Generic look bands",
          "look bands", pipeline.root_signature)
      || !CreateCodecPipelineState(
          device, __look_upsample, &pipeline.upsample,
          L"DLSS5 Generic look edge-aware upsample", "look upsample",
          pipeline.root_signature)
      || !CreateCodecPipelineState(
          device, __look_trace, &pipeline.trace, L"DLSS5 Generic look edit trace",
          "look edit trace", pipeline.root_signature)) {
    ReleaseCom(pipeline.compose);
    ReleaseCom(pipeline.band);
    ReleaseCom(pipeline.upsample);
    ReleaseCom(pipeline.trace);
    return false;
  }
  Log(
      reshade::log::level::info,
      "created the NR look stage from build-validated shader bytecode");
  return true;
}

inline bool CreateScratchTexture(
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initial_state,
    ID3D12Resource** resource,
    const wchar_t* name) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = flags;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;
  const HRESULT result = device->CreateCommittedResource(
      &heap,
      D3D12_HEAP_FLAG_NONE,
      &desc,
      initial_state,
      nullptr,
      IID_PPV_ARGS(resource));
  if (FAILED(result) || *resource == nullptr) {
    std::ostringstream message;
    message << "scratch texture creation failed with HRESULT 0x" << std::hex
            << static_cast<uint32_t>(result) << " (" << static_cast<uint32_t>(format)
            << ' ' << width << 'x' << height << ')';
    Log(reshade::log::level::error, message.str());
    return false;
  }
  (*resource)->SetName(name);
  return true;
}

// A UAV-capable raw buffer, created in COMMON: buffers are promoted to
// UNORDERED_ACCESS by their first use in each command list and decay back
// after it, so the look history and the trace histogram need no transitions.
inline bool CreateScratchBuffer(
    ID3D12Device* device,
    uint64_t bytes,
    ID3D12Resource** resource,
    const wchar_t* name) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;
  const HRESULT result = device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
      IID_PPV_ARGS(resource));
  if (FAILED(result) || *resource == nullptr) {
    std::ostringstream message;
    message << "scratch buffer creation failed with HRESULT 0x" << std::hex
            << static_cast<uint32_t>(result) << std::dec << " (" << bytes << " bytes)";
    Log(reshade::log::level::error, message.str());
    return false;
  }
  (*resource)->SetName(name);
  return true;
}

inline bool SupportsCodecFormat(
    ID3D12Device* device,
    DXGI_FORMAT format) {
  if (device == nullptr) return false;

  D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support{};
  format_support.Format = format;
  if (FAILED(device->CheckFeatureSupport(
          D3D12_FEATURE_FORMAT_SUPPORT,
          &format_support,
          sizeof(format_support)))) {
    return false;
  }

  constexpr D3D12_FORMAT_SUPPORT1 kRequiredSupport =
      D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE
      | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
  return (format_support.Support1 & kRequiredSupport) == kRequiredSupport;
}

inline DXGI_FORMAT CodecViewFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_UNORM_SRGB: return DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_UNORM;
    default: return format;
  }
}

// Games commonly declare their DLSS output/colour surfaces *_TYPELESS and bind
// typed views on top.  Scratch surfaces are created as real resources, so a
// typeless contract is mapped to its typed family member (an identical-layout
// copy source/dest and a valid view format); CopyTextureRegion accepts
// same-family format pairs, so the game's resource stays untouched.  Typed
// sRGB contracts are mapped to the family's UNORM member as well: sRGB
// formats cannot be created with the UAV flag the scratch surfaces need, and
// every view of them already goes through CodecViewFormat (sRGB strip) - an
// sRGB output previously failed scratch creation outright and lost NR.
inline DXGI_FORMAT ConcreteResourceFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    default: return format;
  }
}

// The float SRV format feed v2 reads a game's exposure texture through, or
// UNKNOWN when the texture is not one a Texture2D<float4> view can read
// (integer, depth-only, or block formats).  The guide asks only that the
// first channel hold the value and suggests R16F (DLSS Programming Guide
// 3.9); typeless families map to their float member.
inline DXGI_FORMAT ExposureViewFormat(const D3D12_RESOURCE_DESC& desc) {
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
      || desc.SampleDesc.Count != 1) {
    return DXGI_FORMAT_UNKNOWN;
  }
  switch (desc.Format) {
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

// HDR transfer-mode inference for the DLSS output buffer.
//   0 = SDR        (display-referred already; identity proxy fed straight to DLSSNR)
//   1 = linear HDR (scRGB / linear FP16 / R11G11B10): soft-clip -> sRGB proxy
//   2 = PQ HDR    (R10G10B10A2): linearize PQ -> soft-clip -> sRGB proxy, reverse on
//                 decode.  NVIDIA's DLSSNR is trained on an sRGB display-referred
//                 proxy, so a PQ buffer must be linearized (not passed through) or
//                 colors come out wrong.  Alan Wake 2's DLSSD/RR output is exactly
//                 this R10G10B10A2 PQ case.
inline uint8_t InferHdrMode(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
      return 1;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return 2;
    default:
      return 0;
  }
}

// Resolves the v6 codec's source-content interpretation for an HDR workset.
// The output format is EVIDENCE, not proof: games write PQ into FP16, scene
// linear into R10G10B10A2, and display-referred SDR into float buffers, and
// the user overrides (NRSourceEncoding / NRLinearUnitNits) exist for exactly
// those cases.  Auto (0) trusts the format inference.  PQ content is always
// absolute (10000-nit PQ code range); linear content is relative UNLESS the
// user declares an scRGB-style absolute unit via NRLinearUnitNits (>0).
// NRSourcePrimaries is accepted and logged only - wide-gamut primaries ride
// through the linear path untouched.
inline codec::SourceUnits InterpretSourceUnits(uint8_t hdr_mode) {
  codec::SourceUnits units;
  const uint32_t override_encoding = source_encoding.load();
  uint8_t encoding_choice = 0;
  if (override_encoding != 0u) {
    encoding_choice = static_cast<uint8_t>(override_encoding);
  } else if (hdr_mode == 2u) {
    encoding_choice = 3u;
  } else if (hdr_mode == 1u) {
    encoding_choice = 2u;
  }
  switch (encoding_choice) {
    case 1u:  // SDR: display-referred values in a float buffer, relative
      units.encoding = codec::Encoding::Sdr;
      break;
    case 2u:  // scene-linear, relative until a unit is declared
      units.encoding = codec::Encoding::Linear;
      break;
    case 3u:  // PQ: absolute 10000-nit code range
      units.encoding = codec::Encoding::Pq;
      units.absolute = true;
      units.unit_nits = 10000.f;
      break;
    default:
      units.encoding = codec::Encoding::Linear;
      break;
  }
  if (units.encoding == codec::Encoding::Linear
      && linear_unit_nits.load() > 0.f) {
    units.absolute = true;
    units.unit_nits = linear_unit_nits.load();
  }
  return units;
}

// What the game sent over one telemetry window (PLAN_FEED_V2.md section 7),
// for the interval line's game[...] group: PreExposure and ExposureScale
// (last, min, max), Reset frames and the bursts they form, and output
// alternations - one NR stream evaluating into a different game resource
// than its previous evaluate did.  Each resource owns a workset with its own
// scale texel and near-black state, so a stream that alternates output
// buffers alternates adaptation state (the MSFS class, F2).  Observe-only;
// guarded by runtime_mutex.
struct GameWindow {
  uint64_t frames = 0;
  float pe_last = 1.f;
  float pe_min = 1.f;
  float pe_max = 1.f;
  float es_last = 1.f;
  float es_min = 1.f;
  float es_max = 1.f;
  uint64_t resets = 0;
  uint64_t bursts = 0;
  uint64_t alternations = 0;
};
inline GameWindow game_window;
inline int64_t last_game_reset_ns = 0;
inline uint32_t game_reset_bursts_logged = 0;
// A Reset more than this long after the previous one opens a new burst.
inline constexpr int64_t kGameResetBurstNs = 1'000'000'000;
inline constexpr uint32_t kGameResetBurstLines = 32;

// Every evaluate path, right after DeriveFeatureState: the window above, and
// the game's Reset latched into the epoch - a reset=1 frame declined by any
// later gate would otherwise lose the reset entirely, while the NR slots
// still get DLSSNR.Reset=1 on their next evaluated frame via the un-acked
// epoch.
inline void ObserveGameFrame(const FeatureState& current) {
  const float pe = current.pre_exposure > 0.f ? current.pre_exposure : 1.f;
  const float es = current.exposure.scale > 0.f ? current.exposure.scale : 1.f;
  GameWindow& w = game_window;
  w.pe_min = w.frames == 0 ? pe : (std::min)(w.pe_min, pe);
  w.pe_max = w.frames == 0 ? pe : (std::max)(w.pe_max, pe);
  w.es_min = w.frames == 0 ? es : (std::min)(w.es_min, es);
  w.es_max = w.frames == 0 ? es : (std::max)(w.es_max, es);
  w.pe_last = pe;
  w.es_last = es;
  ++w.frames;
  if (current.frame_reset == 0) return;
  RequestHistoryReset(HistoryResetSource::kGameReset);
  ++w.resets;
  const int64_t now = SteadyNowNs();
  if (now - last_game_reset_ns > kGameResetBurstNs) {
    ++w.bursts;
    if (game_reset_bursts_logged < kGameResetBurstLines) {
      ++game_reset_bursts_logged;
      Log(reshade::log::level::info,
          game_reset_bursts_logged < kGameResetBurstLines
              ? "game Reset: the game flagged a history reset (a burst starts;"
                " resets within 1 s join it)"
              : "game Reset: a burst starts; further bursts are counted in the"
                " interval line's game[...] group only");
    }
  }
  last_game_reset_ns = now;
}

// Returns the workset for `key` (the game resource the pass owns), creating
// its scratch surfaces on first use or on any contract change.  `hdr_mode`
// participates in the reuse check: HDR worksets allocate the v6 linear
// working surfaces and the same-frame scale texture that SDR worksets lack, so
// an SDR<->HDR flip of the same output contract must rebuild.  Caller holds
// runtime_mutex.
inline FinalResources* EnsureWorkset(
    ID3D12Device* device,
    const NVSDK_NGX_Handle* owner,
    const ID3D12Resource* key,
    uint32_t input_width,
    uint32_t input_height,
    uint32_t output_width,
    uint32_t output_height,
    DXGI_FORMAT format,
    uint8_t hdr_mode,
    // Snapshot by the caller (same value its stack loop uses) so the UI
    // cannot change the stack depth between this check and the chain.
    uint32_t requested_stack,
    // kLookSurface* bits this chain needs (LookSurfacesWanted).  Sticky
    // while the rest of the contract holds: a workset keeps what it has and
    // only grows, so a slider crossing its identity value costs one rebuild,
    // not one per crossing.  None wanted keeps them too (rc5): through rc4
    // that freed them, so every Shape result toggle rebuilt the workset -
    // 13 rebuilds in one 20-second A/B in Alan Wake 2.  They go with the
    // workset.
    uint32_t look_surfaces,
    bool create_pre_sr_color = false) {
  if (device == nullptr || key == nullptr || input_width == 0 || input_height == 0
      || output_width == 0 || output_height == 0) {
    // Bounded by the caller's periodic failure retry, not per-frame.
    std::ostringstream message;
    message << "NR workset request rejected (device=" << (device != nullptr)
            << ", key=" << (key != nullptr) << ", in=" << input_width << 'x'
            << input_height << ", out=" << output_width << 'x' << output_height
            << ")";
    Log(reshade::log::level::error, message.str());
    return nullptr;
  }
  const WorksetKey workset_key{owner, key};
  if (const auto stream = features.find(owner); stream != features.end()) {
    if (stream->second.last_workset_key != nullptr
        && stream->second.last_workset_key != key) {
      ++game_window.alternations;
    }
    stream->second.last_workset_key = key;
  }
  // The resolved source-units contract (NRSourceEncoding / NRLinearUnitNits
  // folded into the format inference) participates in the reuse check: a
  // workset snapshots `units` once and every per-frame divisor/anchor
  // decision reads that snapshot, so an override change against the same
  // handle/resource/geometry would otherwise keep silently testing the old
  // color contract.
  const codec::SourceUnits current_units = InterpretSourceUnits(hdr_mode);
  // Stable governor (mode 2): a rebuild of the SAME stream - stack depth,
  // working resolution, output format - keeps its committed divisor instead
  // of re-priming, so a settings change cannot step the normalization.  The
  // texel is carried only between relative worksets of one encoding (the
  // value is in those units); index into retired_final_resources, see the
  // swap after the last fallible allocation below.
  size_t carried_commit = SIZE_MAX;
  uint32_t look_alloc = look_surfaces;
  const auto contract_matches = [&](const FinalResources& resources) {
    return resources.original != nullptr
        && resources.width == output_width
        && resources.height == output_height
        && resources.input_width == input_width
        && resources.input_height == input_height
        && resources.format == format
        && resources.with_pre_sr_color == create_pre_sr_color
        && resources.stack_passes == requested_stack
        && resources.hdr_mode == hdr_mode
        && resources.units.encoding == current_units.encoding
        && resources.units.absolute == current_units.absolute
        && resources.units.unit_nits == current_units.unit_nits
        && resources.block_mean != nullptr
        && (hdr_mode == 0
            || (resources.work0 != nullptr && resources.norm_scale != nullptr
                && resources.norm_commit != nullptr));
  };
  if (const auto existing = worksets.find(workset_key); existing != worksets.end()) {
    FinalResources& resources = existing->second;
    const bool same_contract = contract_matches(resources);
    if (same_contract) look_alloc |= resources.look_surfaces;
    if (same_contract && resources.look_surfaces == look_alloc) {
      MarkWorksetUsed(&resources);
      return &resources;
    }
    const bool carry = norm_governor.load(std::memory_order_relaxed) >= 2u
        && hdr_mode != 0 && resources.hdr_mode != 0
        && !resources.units.absolute && !current_units.absolute
        && resources.units.encoding == current_units.encoding
        && resources.norm_commit != nullptr && resources.norm_commit_primed;
    const size_t retired_before = retired_final_resources.size();
    RetireWorkset(workset_key);
    if (carry && retired_final_resources.size() == retired_before + 1) {
      carried_commit = retired_before;
    }
  }
  // Enforce the pool budget by evicting the least recently used workset. CPU
  // present age is not proof that the GPU finished a command list, so every
  // evicted workset enters fence-backed retirement instead of being freed
  // directly.
  //
  // Counted, because until v6.8.0-alpha23 nothing could say whether this loop
  // had ever run.  Measured across every lane: worksets peak at ONE live (two
  // created, in `swapchain_rebuild`) against a budget of four, so the loop -
  // and with it the only path that hands a workset that is still current to
  // fence-backed retirement, rather than one the game has already stopped
  // using - was dead in the whole harness.  `worksets[... evicted=N]` is what
  // the `workset_churn` lane asserts is non-zero.
  while (worksets.size() >= max_worksets.load()) {
    auto lru = worksets.begin();
    for (auto it = worksets.begin(); it != worksets.end(); ++it) {
      if (it->second.last_used_generation < lru->second.last_used_generation) {
        lru = it;
      }
    }
    // Recycle before evicting (PLAN_REHAB_V7 section 8 item 14).  Every
    // surface in a workset is the addon's own - `original` is a scratch copy,
    // and the one view of a game resource (the pre-SR source) is rewritten by
    // SetSourceView on change - so a workset of the same feature and
    // contract that no recording can still reach takes the new output as it
    // is.  Memory stays at the NRMaxWorksets budget.  Not this present's: the
    // AdmitOutputPass rule lets one feature process two outputs per present,
    // and those must never share scratch.  When the tracker cannot prove the
    // workset idle, the eviction below runs exactly as before.
    if (lru->first.handle == owner
        && lru->second.last_used_generation < present_generation
        && contract_matches(lru->second)
        && (look_surfaces & ~lru->second.look_surfaces) == 0u
        && queue_tracking_active.load(std::memory_order_relaxed)
        && WorksetRecordingsComplete(lru->second)) {
      auto node = worksets.extract(lru);
      node.key() = workset_key;
      FinalResources& recycled = worksets.insert(std::move(node)).position->second;
      // LIFE-01: the recycled workset belongs to a new output. The tracker
      // above proved all its recordings complete, so drop the previous
      // pre-SR colour and its cached binding before an address is reused.
      if (recycled.pre_sr_source != nullptr) {
        for (auto& bound : recycled.bound_sources) {
          if (bound == recycled.pre_sr_source) bound = nullptr;
        }
        ReleaseCom(recycled.pre_sr_source);
      }
      MarkWorksetUsed(&recycled);
      if (!logged_workset_recycle) {
        logged_workset_recycle = true;
        Log(reshade::log::level::info,
            "NR workset pool: the game rotates more outputs than NRMaxWorksets"
            " holds; an idle workset of the same feature now takes the new"
            " output instead of being rebuilt (memory stays at the budget)");
      }
      return &recycled;
    }
    RetireWorksetResources(std::move(lru->second));
    worksets.erase(lru);
    ++worksets_evicted;
    // Say so, once, when the pool is thrashing.  A game whose renderer cycles
    // the upscaled target through more distinct resources than the budget
    // evicts the workset it is about to need again, and a workset is ~13
    // committed textures at output resolution plus a descriptor heap - so it
    // is rebuilt every frame.  Measured on the T2 `output_churn` lane, a ring
    // of six against the default four: 239 worksets built over 240 frames,
    // 3737 us of NR CPU per evaluate against 358 us, 39.7 fps against 86.4.
    //
    // This is the whole reason the line exists.  Without it that session is a
    // bug report that says "it is slow", which is not actionable; with it the
    // log names the cause and the one setting that answers it.  Since 7.0.0
    // the recycle above answers the common case; this fires only when the
    // tracker cannot prove the least-recently-used workset idle.
    //
    // Threshold: evicting on more than half of all presents.  A game that
    // legitimately runs several NGX passes per frame evicts on SOME presents;
    // only a working set larger than the budget evicts on nearly all of them.
    if (!logged_workset_thrash && worksets_evicted > 30
        && worksets_evicted > present_generation / 2) {
      logged_workset_thrash = true;
      std::ostringstream thrash;
      thrash << "NR workset pool is thrashing: " << worksets_evicted
             << " evictions over " << present_generation
             << " presents, budget " << max_worksets.load()
             << ".  This game cycles its upscaled output through more targets"
                " than the pool holds, so NR rebuilds its working surfaces"
                " every frame - expect roughly half the frame rate NR would"
                " otherwise cost.  Raising NRMaxWorksets (1-8) to at least the"
                " number of targets the game rotates through removes it.";
      Log(reshade::log::level::warning, thrash.str());
    }
  }
  // The signed NR runtime consumes FP16/FP32-like color.  Keep that working
  // surface independent from the game's swap/output format; this allows
  // Streamline and native NGX titles that expose R8/R10/R11 or other typed
  // D3D12 outputs while retaining the exact game resource for replacement.
  constexpr DXGI_FORMAT kWorkingFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
  const DXGI_FORMAT concrete_format = ConcreteResourceFormat(format);
  const DXGI_FORMAT codec_format = CodecViewFormat(concrete_format);
  if (!SupportsCodecFormat(device, codec_format)
      || !SupportsCodecFormat(device, kWorkingFormat)) {
    // One-shot: the v5 auto-retry re-attempts failed features periodically
    // (FeatureFailureExpired backoff), and re-logging the same unsupported
    // format every interval is noise.  The latest NGX result stays visible
    // in the overlay.
    static std::atomic<bool> logged_unsupported_format{false};
    if (!logged_unsupported_format.exchange(true)) {
      std::ostringstream message;
      message << "DLSS output format " << static_cast<uint32_t>(format)
              << " is not a supported typed codec format (requires shader sampling and typed UAV support)";
      Log(reshade::log::level::error, message.str());
    }
    return nullptr;
  }

  FinalResources resources;
  resources.width = output_width;
  resources.height = output_height;
  resources.input_width = input_width;
  resources.input_height = input_height;
  resources.stack_passes = requested_stack;
  resources.format = format;
  resources.resource_format = concrete_format;
  resources.working_format = kWorkingFormat;
  resources.with_pre_sr_color = create_pre_sr_color;
  resources.hdr_mode = hdr_mode;
  MarkWorksetUsed(&resources);
  // The NR feature runs 1:1 at the working resolution (the low-resolution
  // input contract is rejected by shipped runtimes); the decode stage upscales.
  const uint32_t neural_width = input_width;
  const uint32_t neural_height = input_height;
  if (!CreateScratchTexture(
          device,
          output_width,
          output_height,
          concrete_format,
          D3D12_RESOURCE_FLAG_NONE,
           D3D12_RESOURCE_STATE_COPY_DEST,
           &resources.original,
           L"DLSS5 Generic original DLSS output")
      || !CreateScratchTexture(
           device,
           input_width,
           input_height,
           kWorkingFormat,
           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          &resources.proxy,
          L"DLSS5 Generic SDR proxy")
      || (create_pre_sr_color
          && !CreateScratchTexture(
              device,
              output_width,
              output_height,
              concrete_format,
              D3D12_RESOURCE_FLAG_NONE,
              D3D12_RESOURCE_STATE_COPY_DEST,
              &resources.pre_sr_color,
              L"DLSS5 Generic pre-SR NR color"))
      || !CreateScratchTexture(
           device,
           neural_width,
           neural_height,
           kWorkingFormat,
           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          &resources.nr_output,
          L"DLSS5 Generic raw neural output")
      || !CreateScratchTexture(
          device,
          output_width,
          output_height,
          concrete_format,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          &resources.decoded,
          L"DLSS5 Generic decoded DLSS output")
      || (hdr_mode == 0 && requested_stack > 1
          && !CreateScratchTexture(
              device,
              output_width,
              output_height,
              concrete_format,
              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              &resources.decoded_alt,
              L"DLSS5 Generic decoded DLSS output (stack ping-pong)"))
      || !CreateScratchTexture(
          device,
          (output_width + 31) / 32,
          (output_height + 31) / 32,
          DXGI_FORMAT_R32G32B32A32_FLOAT,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          &resources.block_mean,
          L"DLSS5 Generic NR block means")) {
    ReleaseFinalResources(resources);
    return nullptr;
  }
  // HDR v6 stays entirely on the GPU.  work0 is the untouched linear source;
  // norm_scale is the single divisor texel produced from work0 and consumed
  // in the same command list before NR evaluates.
  if (hdr_mode != 0) {
    const DXGI_FORMAT work_format =
        concrete_format == DXGI_FORMAT_R32G32B32A32_FLOAT
            ? DXGI_FORMAT_R32G32B32A32_FLOAT
            : DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!CreateScratchTexture(
            device, output_width, output_height, work_format,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            &resources.work0, L"DLSS5 Generic v6 linear source")
        || !CreateScratchTexture(
               device, output_width, output_height, work_format,
               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               &resources.work_a, L"DLSS5 Generic v6 resolve ping-pong A")
        || (requested_stack > 1
            && !CreateScratchTexture(
                   device, output_width, output_height, work_format,
                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   &resources.work_b, L"DLSS5 Generic v6 resolve ping-pong B"))
        || !CreateScratchTexture(
               device, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT,
               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               &resources.norm_scale, L"DLSS5 Generic v6 frame scale")
        || !CreateScratchTexture(
               device, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT,
               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               &resources.norm_commit,
               L"DLSS5 Generic v6 committed frame scale")) {
      ReleaseFinalResources(resources);
      return nullptr;
    }
    resources.units = InterpretSourceUnits(hdr_mode);
  }
  // The NR look stage's surfaces.  A failure here turns the look off for the
  // session and keeps the workset: NR itself does not depend on them.
  const uint32_t band_width = (input_width + 1) / 2;
  const uint32_t band_height = (input_height + 1) / 2;
  if (look_alloc != 0
      && (!CreateScratchBuffer(
              device, look::kTraceWords * sizeof(uint32_t),
              &resources.look_trace_histogram, L"DLSS5 Generic look trace histogram")
          || ((look_alloc & kLookSurfaceOutput) != 0
              && !CreateScratchTexture(
                  device, input_width, input_height, kWorkingFormat,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &resources.look_output,
                  L"DLSS5 Generic look output"))
          || ((look_alloc & kLookSurfaceBands) != 0
              && (!CreateScratchTexture(
                      device, band_width, band_height, DXGI_FORMAT_R32G32B32A32_FLOAT,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &resources.look_band_a,
                      L"DLSS5 Generic look band A")
                  || !CreateScratchTexture(
                      device, band_width, band_height, DXGI_FORMAT_R32G32B32A32_FLOAT,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &resources.look_band_b,
                      L"DLSS5 Generic look band B")
                  || !CreateScratchTexture(
                      device, band_width, band_height, DXGI_FORMAT_R32_FLOAT,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &resources.look_band_max_a,
                      L"DLSS5 Generic look band peak A")
                  || !CreateScratchTexture(
                      device, band_width, band_height, DXGI_FORMAT_R32_FLOAT,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &resources.look_band_max_b,
                      L"DLSS5 Generic look band peak B")))
          || ((look_alloc & kLookSurfaceTransport) != 0
              && (!CreateScratchTexture(
                      device, output_width, output_height, kWorkingFormat,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &resources.look_up_proxy,
                      L"DLSS5 Generic look transported proxy")
                  || !CreateScratchTexture(
                      device, output_width, output_height, kWorkingFormat,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &resources.look_up_neural,
                      L"DLSS5 Generic look transported output")))
          || ((look_alloc & kLookSurfaceTrace) != 0
              && !CreateScratchTexture(
                  device, (input_width + look::kTraceBlock - 1) / look::kTraceBlock,
                  (input_height + look::kTraceBlock - 1) / look::kTraceBlock,
                  DXGI_FORMAT_R32G32B32A32_FLOAT,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &resources.look_trace_blocks,
                  L"DLSS5 Generic look trace blocks")))) {
    for (ID3D12Resource** surface :
         {&resources.look_trace_histogram, &resources.look_output, &resources.look_band_a,
          &resources.look_band_b, &resources.look_band_max_a, &resources.look_band_max_b,
          &resources.look_up_proxy, &resources.look_up_neural,
          &resources.look_trace_blocks}) {
      ReleaseCom(*surface);
    }
    look_unavailable = true;
    look_alloc = 0;
    Log(reshade::log::level::error,
        "NR look stage off for this session: its surfaces could not be created;"
        " Neural Rendering continues without it");
  }
  resources.look_surfaces = look_alloc;

  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  // Per-pass SRV sets + contiguous UAV region - see the kDescriptor* layout
  // comment.  Only the sets for passes this workset can run are populated,
  // and every descriptor is written exactly once here; nothing may rewrite
  // heap slots while a command list that reads them is being recorded.
  heap_desc.NumDescriptors = kDescriptorHeapSize;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  const HRESULT heap_result = device->CreateDescriptorHeap(
      &heap_desc, IID_PPV_ARGS(&resources.descriptors));
  if (FAILED(heap_result) || resources.descriptors == nullptr) {
    std::ostringstream message;
    message << "v6 workset descriptor heap creation failed (hr=0x" << std::hex
            << static_cast<uint32_t>(heap_result) << std::dec
            << ", count=" << heap_desc.NumDescriptors << ")";
    Log(reshade::log::level::error, message.str());
    ReleaseFinalResources(resources);
    return nullptr;
  }
  resources.descriptors->SetName(L"renodx-dlss5.workset.descriptors");
  if (carried_commit != SIZE_MAX && resources.norm_commit != nullptr) {
    // Nothing below can fail, so the carry is safe to commit: SWAP with the
    // retired set rather than share.  The retired set then releases the fresh
    // (never-used) texel through its own fence-backed retirement, while the
    // carried one - possibly still referenced by the old set's in-flight
    // lists - stays owned here; its tracked uses cover both sets' recordings.
    FinalResources& retired = retired_final_resources[carried_commit].resources;
    std::swap(resources.norm_commit, retired.norm_commit);
    resources.norm_commit_primed = true;
    resources.norm_commit_epoch = retired.norm_commit_epoch;
    resources.norm_last_ns = retired.norm_last_ns;
  }
  resources.descriptor_size = device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const D3D12_CPU_DESCRIPTOR_HANDLE heap_start =
      resources.descriptors->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  for (uint32_t pass_set = 0; pass_set < requested_stack; ++pass_set) {
    // SDR: pass 0 views the copied game output (the pre-SR path rebinds its
    // own color over slots 0/3 once per frame, before recording dispatches);
    // pass k>=1 views the previous pass's decoded result - pass 1 and 3
    // read `decoded`, pass 2 the ping-pong alt.
    // HDR: the stack ping-pongs in linear space instead - pass 0 reads the
    // linearized work0, pass k>=1 the surface pass k-1 wrote (odd passes
    // read work_a, even passes work_b).
    ID3D12Resource* const reference =
        hdr_mode != 0
            ? (pass_set == 0 ? resources.work0
                             : ((pass_set & 1) != 0 ? resources.work_a
                                                    : resources.work_b))
            : (pass_set == 0
                   ? resources.original
                   : ((pass_set & 1) != 0 ? resources.decoded
                                          : resources.decoded_alt));
    const DXGI_FORMAT reference_format =
        hdr_mode != 0
            ? (concrete_format == DXGI_FORMAT_R32G32B32A32_FLOAT
                   ? DXGI_FORMAT_R32G32B32A32_FLOAT
                   : DXGI_FORMAT_R16G16B16A16_FLOAT)
            : codec_format;
    const uint32_t set_base = pass_set * kCodecSrvSetStride;
    resources.bound_sources[set_base + 0] = reference;
    resources.bound_sources[set_base + 1] = resources.proxy;
    resources.bound_sources[set_base + 2] = resources.nr_output;
    const bool gpu_frame_scale = hdr_mode != 0 && !resources.units.absolute;
    resources.bound_sources[set_base + 3] = gpu_frame_scale ? resources.norm_scale : reference;
    srv.Format = reference_format;
    device->CreateShaderResourceView(
        reference,
        &srv,
        DescriptorHeapSlot(heap_start, set_base + 0, resources.descriptor_size));
    srv.Format = kWorkingFormat;
    device->CreateShaderResourceView(
        resources.proxy,
        &srv,
        DescriptorHeapSlot(heap_start, set_base + 1, resources.descriptor_size));
    device->CreateShaderResourceView(
        resources.nr_output,
        &srv,
        DescriptorHeapSlot(heap_start, set_base + 2, resources.descriptor_size));
    srv.Format = gpu_frame_scale ? DXGI_FORMAT_R32G32B32A32_FLOAT : reference_format;
    device->CreateShaderResourceView(
        gpu_frame_scale ? resources.norm_scale : reference,
        &srv,
        DescriptorHeapSlot(heap_start, set_base + 3, resources.descriptor_size));
    // This pass's look and transport sets: its codec set with the look's
    // surfaces in the proxy and neural slots (kDescriptorLookSet note).
    const struct {
      uint32_t set;
      ID3D12Resource* proxy_view;
      ID3D12Resource* neural_view;
    } look_sets[] = {
        {kDescriptorLookSet, resources.proxy, resources.look_output},
        {kDescriptorTransportSet, resources.look_up_proxy, resources.look_up_neural}};
    for (const auto& look_set : look_sets) {
      if (look_set.neural_view == nullptr) continue;
      const uint32_t look_base = look_set.set + set_base;
      ID3D12Resource* const viewed[] = {
          reference, look_set.proxy_view, look_set.neural_view,
          gpu_frame_scale ? resources.norm_scale : reference};
      const DXGI_FORMAT view_formats[] = {
          reference_format, kWorkingFormat, kWorkingFormat,
          gpu_frame_scale ? DXGI_FORMAT_R32G32B32A32_FLOAT : reference_format};
      for (uint32_t slot = 0; slot < kCodecSrvSetStride; ++slot) {
        resources.bound_sources[look_base + slot] = viewed[slot];
        srv.Format = view_formats[slot];
        device->CreateShaderResourceView(
            viewed[slot], &srv,
            DescriptorHeapSlot(heap_start, look_base + slot, resources.descriptor_size));
      }
    }
  }
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  uav.Format = kWorkingFormat;
  device->CreateUnorderedAccessView(
      resources.proxy,
      nullptr,
      &uav,
      DescriptorHeapSlot(
          heap_start, kDescriptorProxyUav, resources.descriptor_size));
  uav.Format = codec_format;
  device->CreateUnorderedAccessView(
      resources.decoded,
      nullptr,
      &uav,
      DescriptorHeapSlot(
          heap_start, kDescriptorDecodedUav, resources.descriptor_size));
  uav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  device->CreateUnorderedAccessView(
      resources.block_mean,
      nullptr,
      &uav,
      DescriptorHeapSlot(
          heap_start, kDescriptorBlockMeanUav, resources.descriptor_size));
  if (resources.decoded_alt != nullptr) {
    uav.Format = codec_format;
    device->CreateUnorderedAccessView(
        resources.decoded_alt,
        nullptr,
        &uav,
        DescriptorHeapSlot(
            heap_start, kDescriptorDecodedAltUav, resources.descriptor_size));
    uav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device->CreateUnorderedAccessView(
        resources.block_mean,
        nullptr,
        &uav,
        DescriptorHeapSlot(
            heap_start,
            kDescriptorBlockMeanAltUav,
            resources.descriptor_size));
  }
  if (hdr_mode != 0) {
    // Fixed v6 sets. Commit views keep their final surface at t0 and work0
    // at t1 and t3; t2 is norm_scale on relative sources (only the feed v2
    // commit reads it, as its dark-gate unit) and work0 otherwise. The
    // autoscale set is work0 in all four slots; its shader only reads t0.
    // Pass SRV sets above use t3=norm_scale for relative-HDR encode.
    const DXGI_FORMAT work_view_format =
        concrete_format == DXGI_FORMAT_R32G32B32A32_FLOAT
            ? DXGI_FORMAT_R32G32B32A32_FLOAT
            : DXGI_FORMAT_R16G16B16A16_FLOAT;
    ID3D12Resource* const commit_final[2] = {resources.work_a, resources.work_b};
    const uint32_t commit_sets = resources.work_b != nullptr ? 2u : 1u;
    const bool commit_scale = !resources.units.absolute;
    for (uint32_t set = 0; set < commit_sets; ++set) {
      const uint32_t set_base =
          set == 0 ? kDescriptorCommitASet : kDescriptorCommitBSet;
      for (uint32_t slot = 0; slot < 4; ++slot) {
        const bool scale_slot = slot == 2 && commit_scale;
        ID3D12Resource* const viewed = slot == 0 ? commit_final[set]
            : scale_slot                         ? resources.norm_scale
                                                 : resources.work0;
        resources.bound_sources[set_base + slot] = viewed;
        srv.Format =
            scale_slot ? DXGI_FORMAT_R32G32B32A32_FLOAT : work_view_format;
        device->CreateShaderResourceView(
            viewed, &srv,
            DescriptorHeapSlot(heap_start, set_base + slot, resources.descriptor_size));
      }
    }
    for (uint32_t slot = 0; slot < 4; ++slot) {
      resources.bound_sources[kDescriptorLinearizeSet + slot] = resources.original;
      srv.Format = codec_format;
      device->CreateShaderResourceView(
          resources.original, &srv,
          DescriptorHeapSlot(
              heap_start, kDescriptorLinearizeSet + slot, resources.descriptor_size));
    }
    for (uint32_t slot = 0; slot < 4; ++slot) {
      resources.bound_sources[kDescriptorMeterSet + slot] = resources.work0;
      srv.Format = work_view_format;
      device->CreateShaderResourceView(
          resources.work0, &srv,
          DescriptorHeapSlot(
              heap_start, kDescriptorMeterSet + slot, resources.descriptor_size));
    }
    uav.Format = work_view_format;
    device->CreateUnorderedAccessView(
        resources.work0, nullptr, &uav,
        DescriptorHeapSlot(heap_start, kDescriptorWork0Uav, resources.descriptor_size));
    device->CreateUnorderedAccessView(
        resources.work_a, nullptr, &uav,
        DescriptorHeapSlot(heap_start, kDescriptorWorkAUav, resources.descriptor_size));
    if (resources.work_b != nullptr) {
      device->CreateUnorderedAccessView(
          resources.work_b, nullptr, &uav,
          DescriptorHeapSlot(heap_start, kDescriptorWorkBUav, resources.descriptor_size));
    }
    uav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device->CreateUnorderedAccessView(
        resources.norm_scale, nullptr, &uav,
        DescriptorHeapSlot(
            heap_start, kDescriptorNormScaleUav, resources.descriptor_size));
    device->CreateUnorderedAccessView(
        resources.norm_commit, nullptr, &uav,
        DescriptorHeapSlot(
            heap_start, kDescriptorNormCommitUav, resources.descriptor_size));
    // Valid u1 neighbor for a root signature whose UAV table always spans two
    // entries.  No shader in this path reads or writes it.
    device->CreateUnorderedAccessView(
        nullptr, nullptr, &uav,
        DescriptorHeapSlot(heap_start, kDescriptorUavPadding, resources.descriptor_size));

  }
  if (resources.look_surfaces != 0) {
    // The look UAV table (look::kUav* order).  A surface this workset did
    // not allocate gets a null view of the declared dimension.
    const struct {
      ID3D12Resource* surface;
      DXGI_FORMAT format;
    } look_uavs[] = {
        {resources.look_output, kWorkingFormat},
        {resources.look_band_a, DXGI_FORMAT_R32G32B32A32_FLOAT},
        {resources.look_band_b, DXGI_FORMAT_R32G32B32A32_FLOAT},
        {resources.look_band_max_a, DXGI_FORMAT_R32_FLOAT},
        {resources.look_band_max_b, DXGI_FORMAT_R32_FLOAT},
        {resources.look_up_proxy, kWorkingFormat},
        {resources.look_up_neural, kWorkingFormat},
        {resources.look_trace_histogram, DXGI_FORMAT_R32_TYPELESS},
        {resources.look_trace_blocks, DXGI_FORMAT_R32G32B32A32_FLOAT}};
    static_assert(std::size(look_uavs) == look::kUavCount);
    for (uint32_t index = 0; index < look::kUavCount; ++index) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC look_uav{};
      look_uav.Format = look_uavs[index].format;
      if (index == look::kUavTraceHistogram) {
        look_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        look_uav.Buffer.NumElements = look::kTraceWords;
        look_uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
      } else {
        look_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      }
      device->CreateUnorderedAccessView(
          look_uavs[index].surface, nullptr, &look_uav,
          DescriptorHeapSlot(
              heap_start, kDescriptorLookUav + index, resources.descriptor_size));
    }
  }
  resources.descriptors->SetName(L"DLSS5 Generic Control codec descriptors");
  resources.id = next_workset_id++;
  for (ID3D12Resource* surface :
       {resources.original, resources.proxy, resources.nr_output,
        resources.decoded, resources.decoded_alt, resources.pre_sr_color,
        resources.block_mean, resources.work0, resources.work_a,
        resources.work_b, resources.norm_scale, resources.norm_commit,
        resources.look_output, resources.look_band_a, resources.look_band_b,
        resources.look_band_max_a, resources.look_band_max_b, resources.look_up_proxy,
        resources.look_up_neural, resources.look_trace_histogram,
        resources.look_trace_blocks}) {
    if (surface == nullptr) continue;
    const D3D12_RESOURCE_DESC surface_desc = surface->GetDesc();
    resources.allocated_bytes +=
        device->GetResourceAllocationInfo(0, 1, &surface_desc).SizeInBytes;
  }
  ++worksets_created;

  std::ostringstream message;
  message << "created inline NR resources ws" << resources.id << ' '
          << input_width << 'x' << input_height
          << " -> " << output_width << 'x' << output_height
          << (input_width == output_width && input_height == output_height
                  ? " (native 1:1)"
                  : " (scaled working res)")
          << " format=" << static_cast<uint32_t>(format);
  if (hdr_mode != 0) {
    message << " hdr=v6"
            << (resources.units.encoding == codec::Encoding::Pq ? "/PQ"
                                                                : "/linear")
            << (resources.units.absolute ? " absolute" : " relative");
  }
  message << " (workset " << (worksets.size() + 1) << '/'
          << max_worksets.load() << ", "
          << (resources.allocated_bytes >> 20) << " MiB)";
  // Not the stack passes (those share one workset): a second game resource
  // or NR stream - rotating output buffers, a second view - so the line
  // says so, where it read "multi-pass" through rc3 at NRPasses=1.  Only
  // when another workset served within the last second: through rc4 any
  // pooled workset counted, so a Look toggle rebuilding the one live
  // workset beside idle ones said the game had a second output (AW2 rc4).
  const bool second_workset = std::any_of(
      worksets.begin(), worksets.end(), [&](const auto& other) {
        return resources.last_used_ns - other.second.last_used_ns < 1'000'000'000;
      });
  if (worksets.emplace(workset_key, std::move(resources)).second && second_workset
      && !logged_second_workset) {
    logged_second_workset = true;
    Log(
        reshade::log::level::info,
        "second NR workset live: the game evaluates into more than one output"
        " (rotating buffers or a second DLSS stream); each keeps its own"
        " scratch surfaces, scale and temporal state");
  }
  Log(reshade::log::level::info, message.str());
  if (look_alloc != 0) {
    std::ostringstream look_message;
    look_message << "NR look surfaces ws" << (next_workset_id - 1) << ':'
                 << ((look_alloc & kLookSurfaceOutput) != 0 ? " output" : "")
                 << ((look_alloc & kLookSurfaceBands) != 0 ? " bands" : "")
                 << ((look_alloc & kLookSurfaceTransport) != 0 ? " transport" : "")
                 << ((look_alloc & kLookSurfaceTrace) != 0 ? " trace" : "");
    Log(reshade::log::level::info, look_message.str());
  }
  return &worksets.find(workset_key)->second;
}

inline bool EnsureNrFeature(
    ID3D12GraphicsCommandList* command_list,
    FeatureState& state,
    uint32_t slot_index,
    const FinalResources& resources,
    uint32_t input_width,
    uint32_t input_height,
    uint32_t output_width,
    uint32_t output_height) {
  NrFeatureSlot& slot = state.slots[slot_index];
  if (slot.handle != nullptr) return true;
  if (command_list == nullptr || slot.failed) return false;
  if (!EnsureDirectRuntime(command_list)) return false;

  if (slot.parameters == nullptr) {
    const NVSDK_NGX_Result allocate_result =
        core_allocate_parameters(&slot.parameters);
    last_result = static_cast<uint32_t>(allocate_result);
    if (NVSDK_NGX_FAILED(allocate_result) || slot.parameters == nullptr) {
      slot.failed = true;
      slot.failed_ns = SteadyNowNs();
      Log(reshade::log::level::error, "DLSS-NR AllocateParameters failed");
      return false;
    }
  }
  NVSDK_NGX_Parameter* parameters = slot.parameters;
  // Keep the standard NGX dimensions coherent with the private DLSSNR
  // dimensions.  The probe uses render Width/Height for input and OutWidth /
  // OutHeight for native output, while DLSSNR.Width/Height identify the
  // runtime's native working target.
  NVSDK_NGX_Parameter_SetUI(
      parameters, NVSDK_NGX_Parameter_Width, input_width);
  NVSDK_NGX_Parameter_SetUI(
      parameters, NVSDK_NGX_Parameter_Height, input_height);
  NVSDK_NGX_Parameter_SetUI(
      parameters, NVSDK_NGX_Parameter_OutWidth, output_width);
  NVSDK_NGX_Parameter_SetUI(
      parameters, NVSDK_NGX_Parameter_OutHeight, output_height);
  // The private runtime's Width/Height describe the native output surface;
  // ScalingRatio and ColorSubrect carry the low-resolution input contract.
  // This mirrors the standalone probe that successfully creates feature 18.
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.Width", output_width);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.Height", output_height);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.InputWidth", input_width);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.InputHeight", input_height);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.OutputWidth", output_width);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.OutputHeight", output_height);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.Output.Width", output_width);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.Output.Height", output_height);
  NVSDK_NGX_Parameter_SetF(parameters, "DLSSNR.ScalingRatio", 1.f);
  NVSDK_NGX_Parameter_SetF(parameters, "DLSSNR.Scale", 1.f);
  parameters->Set(
      "DLSSNRComputeScalingRatioCallback",
      reinterpret_cast<void*>(&DlssNrScalingRatioCallback));
  NVSDK_NGX_Parameter_SetUI(
      parameters, "DLSSNR.Hint.Render.Preset", preset.load());
  // Control creates the native-resolution NR pass with default quality, independent
  // of the game's DLSS SR quality mode.
  NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_PerfQualityValue, 0);
  // HDR NR always uses the validated feature contract.  The private proxy is
  // normalized before the model, so exposure is intentionally identity here.
  // What runtime 310.8 can read of it (string probe of nvngx_dlssnr.dll and
  // the driver's _nvngx.dll, 2026-09-23; PLAN_FEED_V2.md F7): neither binary
  // names DLSS.Pre.Exposure, DLSS.Exposure.Scale or Jitter/JitterOffset, and
  // the NR runtime names none of DLSSNR.InputWidth/OutputWidth/Output.Width/
  // Scale or GlobalToneStrength either - inert, like the Global tone slider
  // measured (bit-identical at 0, 1, 2; removed in rc5).  The core does name
  // DLSS.Feature.Create.Flags, so the flags stay as validated; keys the NR
  // runtime names (DLSSNR.Width, ScalingRatio, MVecScale - the last one
  // measured live, F8) can be read.  A future runtime that names "Exposure" makes this a live contract
  // again, and feed v2 would then have to stop sending AutoExposure (F7).
  const bool forced_contract = resources.hdr_mode != 0;
  uint32_t nr_create_flags = state.create_flags;
  if (forced_contract) {
    nr_create_flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR
                    | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
  }
  NVSDK_NGX_Parameter_SetI(
      parameters,
      NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
      static_cast<int32_t>(nr_create_flags));
  if (forced_contract) {
    NVSDK_NGX_Parameter_SetF(
        parameters, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    NVSDK_NGX_Parameter_SetF(
        parameters, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
  }
  NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_CreationNodeMask, 1);
  NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);

  NVSDK_NGX_Result result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
  {
    DirectCallScope ngx_direct_call;
    std::optional<lastgasp::InsideCall> crash_call;
    if (lastgasp::dumps_on.load(std::memory_order_relaxed)) crash_call.emplace("nr-create");
    try {
      if (parameter_runtime_via_core.load() && core_create_feature != nullptr) {
        result = core_create_feature(
            command_list,
            kFeatureDlssNr,
            parameters,
            &slot.handle);
        slot.via_core = NVSDK_NGX_SUCCEED(result) && slot.handle != nullptr;
      }
      if (!slot.via_core) {
        slot.handle = nullptr;
        result = direct_api.create(
            command_list,
            kFeatureDlssNr,
            parameters,
            &slot.handle);
      }
    } catch (...) {
      // The shared failure path below latches the feature and releases the
      // parameter block; the exception only substitutes the result code.
      Log(reshade::log::level::error, "feature 18 create raised an exception");
      slot.via_core = false;
      slot.handle = nullptr;
      result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    }
  }
  last_result = static_cast<uint32_t>(result);
  if (NVSDK_NGX_FAILED(result) || slot.handle == nullptr) {
    slot.failed = true;
    slot.failed_ns = SteadyNowNs();
    slot.handle = nullptr;
    if (slot.parameters != nullptr) {
      core_destroy_parameters(slot.parameters);
      slot.parameters = nullptr;
    }
    std::ostringstream message;
    message << "feature 18 create failed with 0x" << std::hex
            << static_cast<uint32_t>(result)
            << (slot_index != 0 ? " (stack pass)" : "");
    Log(reshade::log::level::error, message.str());
    return false;
  }
  RefreshActiveFeatureCount();
  ++nr_features_created;
  // Create may record initialization work into the game's list; it is the
  // handle's first tracked use (see RetiredNrFeature::tracker_proof).
  // TrackUse first: it arms the inert queue-detour gate on this session's
  // very first NR create, and the flag must then read TRUE - this handle's
  // submissions carry exact proofs from that moment, not lease fallbacks.
  submission::TrackUse(command_list, slot.handle);
  slot.tracked_since_create =
      queue_tracking_active.load(std::memory_order_relaxed);
  slot.configuration_generation = configuration_generation.load();
  slot.nr_hdr_mode = resources.hdr_mode;
  slot.pending_reset = true;
  // fail_count is NOT cleared here: only a successful evaluate proves the
  // slot works.  A contract that creates but fails its evaluate would
  // otherwise re-create (a 100-250 ms stall) on every frame.
  std::ostringstream message;
  message << "feature 18 created via "
          << (slot.via_core ? "the NGX core" : "the signed snippet")
          << " after " << SourceFeatureName(state.source_feature)
          << " for NR input " << std::dec << input_width << 'x'
          << input_height << " -> output " << output_width << 'x'
          // The game's create-time render size, labelled as such: through rc4
          // it read "with guides", but each evaluate sizes the guides from its
          // own parameters (Alan Wake 2 rc4: frame contracts read
          // in=2227x1253 while every create line said 1280x720).
          << output_height << ", game DLSS created at "
          << (state.create_input_width != 0 ? state.create_input_width
                                            : state.input_width)
          << 'x'
          << (state.create_input_height != 0 ? state.create_input_height
                                             : state.input_height)
          << (slot_index != 0 ? " (stack pass " + std::to_string(slot_index + 1) + ")" : "");
  Log(reshade::log::level::info, message.str());
  return true;
}

// Runtime 310.8 treats a negative Skin value as "follow Structure". With
// Character mask enabled it reads Skin separately, but Structure above 1
// suppresses that control. Keep the legacy values when this selectable mode
// is off or the mask is absent; otherwise send stock Skin as 1 and cap the
// Structure value sent to the model. The player's stored values are unchanged.
struct SkinSteering {
  float structure;
  float skin;
};
inline SkinSteering SentSkinSteering(float structure, float skin, bool auto_mask) {
  if (!auto_mask || !skin_independent.load()) return {structure, skin};
  return {(std::min)(structure, 1.f), skin < 0.f ? 1.f : skin};
}

inline void SetEvaluationParameters(
    FeatureState& state,
    NrFeatureSlot& slot,
    uint32_t slot_index,
    uint64_t chain_reset_epoch,
    FinalResources& resources,
    const EvaluationContract& frame,
    uint32_t input_width,
    uint32_t input_height,
    uint32_t output_width,
    uint32_t output_height) {
  NVSDK_NGX_Parameter* parameters = slot.parameters;
  parameters->Set("DLSSNR.Color", resources.proxy);
  parameters->Set("DLSSNR.Output", resources.nr_output);
  parameters->Set("DLSSNR.MVec", frame.motion);
  parameters->Set("DLSSNR.Depth", frame.depth);
  NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_Width, input_width);
  NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_Height, input_height);
  NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_OutWidth, output_width);
  NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_OutHeight, output_height);

  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.ColorSubrectBaseX", 0);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.ColorSubrectBaseY", 0);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.ColorSubrectWidth", input_width);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.ColorSubrectHeight", input_height);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.DepthSubrectBaseX", frame.depth_x);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.DepthSubrectBaseY", frame.depth_y);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.DepthSubrectWidth", frame.input_width);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.DepthSubrectHeight", frame.input_height);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.MVecSubrectBaseX", frame.motion_x);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.MVecSubrectBaseY", frame.motion_y);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.MVecSubrectWidth", frame.input_width);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.MVecSubrectHeight", frame.input_height);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.OutputSubrectBaseX", 0);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.OutputSubrectBaseY", 0);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.OutputSubrectWidth", output_width);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.OutputSubrectHeight", output_height);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.InputWidth", input_width);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.InputHeight", input_height);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.OutputWidth", output_width);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.OutputHeight", output_height);
  NVSDK_NGX_Parameter_SetF(parameters, "DLSSNR.ScalingRatio", 1.f);
  NVSDK_NGX_Parameter_SetF(parameters, "DLSSNR.Scale", 1.f);
  // Keep the HDR feature contract identical at create and evaluate time.
  if (resources.hdr_mode != 0) {
    NVSDK_NGX_Parameter_SetI(
        parameters,
        NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
        static_cast<int32_t>(state.create_flags
            | NVSDK_NGX_DLSS_Feature_Flags_IsHDR
            | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure));
    NVSDK_NGX_Parameter_SetF(
        parameters, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    NVSDK_NGX_Parameter_SetF(
        parameters, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
  }
  parameters->Set(
      "DLSSNRComputeScalingRatioCallback",
      reinterpret_cast<void*>(&DlssNrScalingRatioCallback));
  NVSDK_NGX_Parameter_SetF(
      parameters,
      "DLSSNR.MVecScaleX",
      frame.motion_scale_x * motion_scale_x_multiplier.load());
  NVSDK_NGX_Parameter_SetF(
      parameters,
      "DLSSNR.MVecScaleY",
      frame.motion_scale_y * motion_scale_y_multiplier.load());
  if (resources.foveation.active() && resources.fov_guides_valid) {
    // Foveated grid (RecordFoveatedGuides): the guides are already on the
    // warped working grid, 1:1 with Color, and the vectors are in its pixels.
    parameters->Set("DLSSNR.MVec", resources.fov_motion);
    parameters->Set("DLSSNR.Depth", resources.fov_depth);
    NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.DepthSubrectBaseX", 0);
    NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.DepthSubrectBaseY", 0);
    NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.DepthSubrectWidth", input_width);
    NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.DepthSubrectHeight", input_height);
    NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.MVecSubrectBaseX", 0);
    NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.MVecSubrectBaseY", 0);
    NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.MVecSubrectWidth", input_width);
    NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.MVecSubrectHeight", input_height);
    NVSDK_NGX_Parameter_SetF(parameters, "DLSSNR.MVecScaleX", 1.f);
    NVSDK_NGX_Parameter_SetF(parameters, "DLSSNR.MVecScaleY", 1.f);
  }
  // Jitter is forwarded only when the game actually supplied one this frame:
  // presence, not value - an absent jitter stays unset rather than pinned to
  // zero (Streamline carries it per frame via Constants; native NGX titles
  // set Jitter_Offset_X/Y on the evaluate block when they jitter at all).
  // Unknown-name tolerance is the runtime's documented behavior.  Runtime
  // 310.8 names no jitter key at all (see EnsureNrFeature's contract note),
  // so on the after path, whose input DLSS already de-jittered, it cannot
  // misplace the image.
  if (frame.has_jitter) {
    NVSDK_NGX_Parameter_SetF(parameters, "DLSSNR.JitterOffsetX", frame.jitter_x);
    NVSDK_NGX_Parameter_SetF(parameters, "DLSSNR.JitterOffsetY", frame.jitter_y);
  }
  bool depth_inverted =
      (frame.create_flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
  if (depth_mode.load() == 1) depth_inverted = false;
  if (depth_mode.load() == 2) depth_inverted = true;
  NVSDK_NGX_Parameter_SetI(
      parameters,
      "DLSSNR.DepthInverted",
      depth_inverted ? 1 : 0);
  NVSDK_NGX_Parameter_SetI(parameters, "DLSSNR.Enabled", 1);
  // Reset semantics with stacking (v5.3 epoch model; see reset_epoch.hpp):
  // the chain's reset epoch is resolved once before the pass loop and every
  // pass of the chain evaluates against that same value - never inside a
  // short-circuit expression, which is how the v5.2.2 global latch stuck
  // true and reset pass 1 on every stacked frame.  A slot resets when it
  // has not yet acknowledged the current epoch (startup, globally signaled
  // scene cut, codec/style/config change), when the game flagged Reset for
  // this frame, or when it was freshly created (pending_reset).  With
  // NRChainedHistory off (legacy diagnostic stacking), passes 2+ are still
  // deliberately stateless - reset on every frame.
  const bool do_reset = StackSlotNeedsReset(
      chained_temporal_history.load(),
      slot_index,
      slot.pending_reset,
      frame.frame_reset,
      slot.acknowledged_reset_epoch,
      chain_reset_epoch);
  slot.pending_reset = false;
  slot.acknowledged_reset_epoch = chain_reset_epoch;
  NVSDK_NGX_Parameter_SetI(parameters, "DLSSNR.Reset", do_reset ? 1 : 0);
  slot.model_reset = do_reset;
  NVSDK_NGX_Parameter_SetF(
      parameters, "DLSSNR.Intensity", PassIntensity(slot_index));
  // The model steering: the Look section's values, or a stack pass's own
  // once its "Same as pass 1" is off (group G).  Each is read once, so the
  // log line below names exactly what this evaluate sent.
  const uint32_t model_style = PassModel(slot_index, style, pass_style);
  const float model_local_tone =
      PassModel(slot_index, local_tone_strength, pass_local_tone);
  const float model_structure =
      PassModel(slot_index, local_structure_strength, pass_local_structure);
  const float model_skin =
      PassModel(slot_index, skin_structure_strength, pass_skin_structure);
  const bool model_auto_mask = PassModel(slot_index, use_auto_mask, pass_auto_mask);
  const bool model_ui_correction =
      PassModel(slot_index, ui_correction, pass_ui_correction);
  const SkinSteering sent = SentSkinSteering(model_structure, model_skin, model_auto_mask);
  // There is no global tone key: runtime 310.8 names none (string probe), its
  // output was bit-identical at the old slider's 0, 1 and 2, and the owner
  // confirmed it dead in the field.  The slider and NRGlobalTone were
  // removed in rc5; a stored key is ignored.
  NVSDK_NGX_Parameter_SetF(parameters, "DLSSNR.LocalToneStrength", model_local_tone);
  NVSDK_NGX_Parameter_SetF(parameters, "DLSSNR.LocalStructureStrength", sent.structure);
  NVSDK_NGX_Parameter_SetF(parameters, "DLSSNR.SkinStructureStrength", sent.skin);
  NVSDK_NGX_Parameter_SetI(parameters, "DLSSNR.UseAutoMask", model_auto_mask ? 1 : 0);
  NVSDK_NGX_Parameter_SetUI(parameters, "DLSSNR.Style", model_style);
  NVSDK_NGX_Parameter_SetI(parameters, "DLSSNR.UICorrection", model_ui_correction ? 1 : 0);
  // Observe-only: a slot's steering when it first goes out and whenever it
  // changes, so a support log can tell what each stack pass asked the model
  // for (a pass whose "Same as pass 1" is off sends its own).  At most one
  // line a second per slot: a dragged slider changes it every frame (186
  // lines in one 3-minute AW2 session).  The settled value still logs, on
  // the first evaluate after the second is up.
  const float model[9] = {
      static_cast<float>(model_style), model_local_tone, model_structure,
      model_skin, model_auto_mask ? 1.f : 0.f, model_ui_correction ? 1.f : 0.f,
      skin_independent.load() ? 1.f : 0.f, sent.structure, sent.skin};
  const int64_t model_now_ns = SteadyNowNs();
  if (std::memcmp(model, slot.logged_model, sizeof(model)) != 0
      && model_now_ns - slot.model_logged_ns >= 1'000'000'000) {
    std::memcpy(slot.logged_model, model, sizeof(model));
    slot.model_logged_ns = model_now_ns;
    std::ostringstream message;
    message << "NR pass " << (slot_index + 1) << " model: style=" << model_style
            << " local_tone=" << model_local_tone
            << " structure=" << model_structure << " skin=" << model_skin
            << " auto_mask=" << (model_auto_mask ? 1 : 0)
            << " ui_correction=" << (model_ui_correction ? 1 : 0)
            << " skin_independent=" << (skin_independent.load() ? 1 : 0)
            << " sent_structure=" << sent.structure << " sent_skin=" << sent.skin;
    Log(reshade::log::level::info, message.str());
  }
  NVSDK_NGX_Parameter_SetI(parameters, "DLSS.Indicator.Invert.X.Axis", 0);
  NVSDK_NGX_Parameter_SetI(parameters, "DLSS.Indicator.Invert.Y.Axis", 0);
}

inline void Transition(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after,
    UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
  if (before == after) return;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = subresource;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  command_list->ResourceBarrier(1, &barrier);
}

inline void UavBarrier(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* resource) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = resource;
  command_list->ResourceBarrier(1, &barrier);
}

// CopyResource requires identical resource descriptions, so a game output with
// a mip chain cannot be copied whole.  NR only consumes and replaces mip 0:
// this copies exactly that subresource between a single-mip scratch surface
// and the game's output.
inline void CopyMip0(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* destination,
    ID3D12Resource* source) {
  D3D12_TEXTURE_COPY_LOCATION locations[2]{};
  locations[0].pResource = destination;
  locations[0].Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  locations[1].pResource = source;
  locations[1].Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  command_list->CopyTextureRegion(&locations[0], 0, 0, 0, &locations[1], nullptr);
}

// Select the normalization contract for this frame.  Absolute sources use a
// fixed divisor derived from their calibrated unit contract.  Relative HDR
// returns zero here; the shaders then read the divisor produced from this same
// frame by PrepareFrameScale.  No CPU normalization history participates.

// Display codec selection (codec_gain.hpp for the curve math): explicit
// NRCodecMode=3, plus the Auto heuristic for engine-relative float HDR -
// no declared nits means no trustworthy fixed anchor, so relative HDR uses
// the same-frame GPU-derived divisor.  Calibrated absolute
// sources keep the divisor family: their nits are real and Classic/Anchored
// already honor them (generic-mod heuristic: per format, with manual
// override).
inline bool FrameDisplayCodec(const FinalResources& resources) {
  if (resources.hdr_mode == 0) return false;
  const uint32_t mode = codec_mode.load();
  if (mode == 3u) return true;
  if (mode == 2u) {
    return !(resources.units.absolute && resources.units.unit_nits > 0.f);
  }
  return false;
}

// Whether v6_commit removes the model's dark pedestal on this frame (its
// PedestalGate).  On a Display frame HDR Transfer Strength scales nothing
// else: the resolve blends by Color Strength alone.  The removal's gate
// (DarkGate 0.005) and cap are absolute source units, and a relative-unit
// frame has none.  Measured on a KCD2 capture (field report, rc6): the scene
// median was 0.0002, so the "near-black" gate sat 4.6 stops above it and
// gated 69 % of pixels.  NR's lift there is zero-mean noise (p05/p95
// +/-8e-5), and the >= 0 clamp turned it into per-block darkening (p95 -0.43
// stop at strength 1): the shadows flickered, and strength <= 0.25 hid it.
// The pedestal the removal was built for (v6.0.x's uniform +3 %) went away
// with v6.1.0's normalization.  So Auto (NRDisplayPedestal=0) skips it on
// relative Display frames, and 1 keeps it everywhere.  A calibrated absolute
// source forced onto the Display codec keeps it, as does every divisor-family
// frame.
inline bool FramePedestalRemoval(const FinalResources& resources) {
  if (!FrameDisplayCodec(resources)) return true;
  if (display_pedestal.load(std::memory_order_relaxed) != 0u) return true;
  return resources.units.absolute && resources.units.unit_nits > 0.f;
}

// Whether any pass of the chain changes the image (the zero-strength bypass,
// handoff rule 3).  Where the frame skips the pedestal removal, transfer
// strength scales nothing, so only color strength counts.
inline bool ChainActive(const FinalResources& resources, uint32_t stack) {
  const bool transfer_acts = FramePedestalRemoval(resources);
  for (uint32_t pass = 0; pass < stack; ++pass) {
    if ((transfer_acts && PassTransferStrength(pass) > 0.f)
        || PassColorStrength(pass) > 0.f) {
      return true;
    }
  }
  return false;
}

inline const char* ExposureContractText(const codec::ExposureEvidence& e) {
  if (e.auto_exposure && (e.has_texture || e.has_scale)) return "ambiguous";
  if (e.has_texture && e.has_scale) return "tex+scale";
  if (e.has_texture) return "explicit_tex";
  if (e.has_scale) return "explicit_scale";
  if (e.has_pre_exposure) return "pre_exposure";
  if (e.auto_exposure) return "model_auto";
  return "none";
}

inline const char* FeedText(FeedSource feed) {
  switch (feed) {
    case FeedSource::kTexture: return "v2-exposure";
    case FeedSource::kFixed: return "v2-fixed";
    default: return "v1";
  }
}

// The frame's NR input scale source (NRFeedMode, defaults::kFeedMode).  The
// feed only exists where the scale is the GPU texel: relative HDR.  Auto is
// the generic-mod heuristic - v2 needs positive evidence, and any absent or
// ambiguous signal keeps v1:
//   - a readable exposure texture on a stream WITHOUT the AutoExposure flag
//     (with the flag, DLSS ignores the texture, so its contents are
//     unproven - KCD2 sends R16F plus the flag).
// A pre-exposure other than 1 (UE5; Silent Hill 2 at ~0.001) is NOT such
// evidence: it says the buffer is pre-exposed, not that it fits the proxy.
// Through rc5 Auto sent those streams the fixed scale 1, which has no
// over-range guard, and a bright frame then reached NR with its median past
// the encode shoulder: NR boiled (SH2, rc5).  Measured on the e2e host
// (static scene, 640x360 -> 1280x720, the real runtime), pixels changing
// > 2 % per frame with NR on: v1 7.2 % at every brightness; fixed 7.7 %,
// 10.8 %, 14.1 % at mean 0.3, 0.95, 1.9 (v1 committed 11.3 there, gate
// fraction 0.70); DLSS alone 16 %.  v1 never brightens a frame, so on an
// in-range pre-exposed frame it IS the fixed scale; it differs only where
// the fixed scale fails.  NRFeedMode=2 still forces v2 (the texture when
// readable, else fixed 1) and 1 forces v1.  Logs once per stream per
// decision.
inline FeedSource FrameFeed(
    FeatureState* feature,
    const FeatureState& current,
    const FinalResources& res,
    ID3D12Resource* exposure) {
  if (current.has_pre_exposure && current.pre_exposure != 1.f
      && current.pre_exposure > 0.f && std::isfinite(current.pre_exposure)) {
    feature->pre_exposure_seen = true;
  }
  const uint32_t mode = feed_mode.load(std::memory_order_relaxed);
  const bool readable = exposure != nullptr
      && ExposureViewFormat(exposure->GetDesc()) != DXGI_FORMAT_UNKNOWN;
  const bool auto_flag =
      ((feature->create_flags | current.create_flags)
       & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure)
      != 0;
  FeedSource feed = FeedSource::kV1;
  if (res.hdr_mode == 0 || res.units.absolute || res.norm_scale == nullptr
      || codec_pipeline.exposure_scale == nullptr
      || codec_pipeline.commit_exposure == nullptr || mode == 1u) {
    feed = FeedSource::kV1;
  } else if (mode == 2u) {
    feed = readable ? FeedSource::kTexture : FeedSource::kFixed;
  } else if (readable && !auto_flag) {
    feed = FeedSource::kTexture;
  }
  if (feature->logged_feed != static_cast<uint32_t>(feed)) {
    feature->logged_feed = static_cast<uint32_t>(feed);
    std::ostringstream message;
    message << "NR feed: " << FeedText(feed) << " (NRFeedMode=" << mode
            << "; exposure texture="
            << (exposure == nullptr ? "none"
                : readable          ? "readable"
                                    : "unreadable")
            << " fmt=" << (exposure != nullptr
                               ? static_cast<uint32_t>(exposure->GetDesc().Format)
                               : 0u)
            << " auto_exposure=" << (auto_flag ? 1 : 0)
            << " pre_exposure=" << current.pre_exposure
            << (feature->pre_exposure_seen ? " (seen !=1)" : "")
            << " exposure_scale=" << current.exposure.scale
            << " hdr=" << static_cast<uint32_t>(res.hdr_mode)
            << " units=" << (res.units.absolute ? "absolute" : "relative") << ')';
    Log(reshade::log::level::info, message.str());
  }
  return feed;
}

// v6 uses one of two normalization contracts:
//   * relative HDR: divisor == 0, which tells the shader to consume the
//     same-frame GPU autoscale texel;
//   * calibrated absolute HDR: a deterministic fixed divisor from the declared
//     unit contract.  No scene history, CPU readback, or temporal adaptation
//     participates in this decision.
inline float FrameCodecDivisor(const FinalResources& resources) {
  if (resources.hdr_mode == 0) return 1.f;
  if (!resources.units.absolute || !(resources.units.unit_nits > 0.f)) return 0.f;

  const uint32_t mode = codec_mode.load(std::memory_order_relaxed);
  if (FrameDisplayCodec(resources)) {
    return codec::DisplayAnchorLinear(
        resources.units, proxy_anchor_nits.load(), diffuse_white_nits.load());
  }
  if (mode == 1u) {
    return codec::AnchoredDivisor(proxy_anchor_nits.load(), resources.units);
  }
  return resources.units.encoding == codec::Encoding::Pq
      ? codec::ClassicPqDivisor(
            diffuse_white_nits.load(), paper_white_scale.load(), pq_calibration.load())
      : codec::ClassicLinearDivisor(paper_white_scale.load());
}

// Compact field contract.  The useful distinction is whether normalization is
// fixed from calibrated units or computed from THIS frame on the GPU; all of
// the former CPU normalization/health state has intentionally been removed.
inline void LogFrameContract(
    const NVSDK_NGX_Parameter* game_parameters,
    const FeatureState& current,
    const FinalResources& res,
    float frame_divisor,
    FeedSource feed,
    const char* path) {
  static std::mutex contract_mutex;
  static std::string last_key;
  static std::chrono::steady_clock::time_point last_dump;
  // Steady-state fast gate on the exact key components: resource identity
  // covers both formats (D3D12 resource descs are immutable), the scalars
  // cover the rest. The common evaluate then pays two parameter lookups
  // instead of two GetDesc calls and an ostringstream.
  static ID3D12Resource* last_color = nullptr;
  static ID3D12Resource* last_output = nullptr;
  static uint32_t last_out_w = 0;
  static uint32_t last_out_h = 0;
  static uint32_t last_flags = 0;
  static uint32_t last_encoding = 0;
  static uint32_t last_absolute = 0;
  static float last_unit_nits = 0.f;
  static uint32_t last_codec_mode = 0;
  static uint32_t last_fixed = 0;
  static uint32_t last_governor = 0;
  static FeedSource last_feed = FeedSource::kV1;
  static bool last_pedestal = true;
  const auto now = std::chrono::steady_clock::now();
  const bool pedestal = FramePedestalRemoval(res);
  ID3D12Resource* color_resource =
      GetD3D12Resource(game_parameters, NVSDK_NGX_Parameter_Color);
  ID3D12Resource* output_resource =
      GetD3D12Resource(game_parameters, NVSDK_NGX_Parameter_Output);
  {
    std::lock_guard<std::mutex> lock(contract_mutex);
    if (color_resource == last_color && output_resource == last_output
        && current.output_width == last_out_w
        && current.output_height == last_out_h
        && current.create_flags == last_flags
        && static_cast<uint32_t>(res.units.encoding) == last_encoding
        && static_cast<uint32_t>(res.units.absolute) == last_absolute
        && res.units.unit_nits == last_unit_nits
        && codec_mode.load(std::memory_order_relaxed) == last_codec_mode
        && static_cast<uint32_t>(frame_divisor > 0.f) == last_fixed
        && norm_governor.load(std::memory_order_relaxed) == last_governor
        && feed == last_feed
        && pedestal == last_pedestal
        && now - last_dump < std::chrono::seconds(15)) {
      return;
    }
  }
  const D3D12_RESOURCE_DESC color_desc = color_resource->GetDesc();
  const D3D12_RESOURCE_DESC output_desc = output_resource->GetDesc();

  std::ostringstream key;
  key << std::hex << color_desc.Format << '|' << output_desc.Format << '|'
      << std::dec << current.output_width << 'x' << current.output_height << '|'
      << current.create_flags << '|' << static_cast<uint32_t>(res.units.encoding)
      << '|' << res.units.absolute << '|' << res.units.unit_nits << '|'
      << codec_mode.load(std::memory_order_relaxed) << '|'
      << (frame_divisor > 0.f ? 1 : 0) << '|'
      << norm_governor.load(std::memory_order_relaxed) << '|'
      << static_cast<uint32_t>(feed) << '|' << pedestal;
  const std::string key_text = key.str();
  {
    std::lock_guard<std::mutex> lock(contract_mutex);
    if (key_text == last_key && now - last_dump < std::chrono::seconds(15)) return;
    last_key = key_text;
    last_dump = now;
    last_color = color_resource;
    last_output = output_resource;
    last_out_w = current.output_width;
    last_out_h = current.output_height;
    last_flags = current.create_flags;
    last_encoding = static_cast<uint32_t>(res.units.encoding);
    last_absolute = static_cast<uint32_t>(res.units.absolute);
    last_unit_nits = res.units.unit_nits;
    last_codec_mode = codec_mode.load(std::memory_order_relaxed);
    last_fixed = static_cast<uint32_t>(frame_divisor > 0.f);
    last_governor = norm_governor.load(std::memory_order_relaxed);
    last_feed = feed;
    last_pedestal = pedestal;
  }

  // The governor only exists for a metered (v1) GPU-frame scale.
  const bool governed = !(frame_divisor > 0.f) && feed == FeedSource::kV1;
  std::ostringstream message;
  message << "frame contract path=" << path
          << " in=" << current.input_width << 'x' << current.input_height
          << " out=" << current.output_width << 'x' << current.output_height
          << " hdr=" << static_cast<uint32_t>(res.hdr_mode)
          << " encoding="
          << (res.units.encoding == codec::Encoding::Pq ? "pq"
              : res.units.encoding == codec::Encoding::Linear ? "linear" : "sdr")
          << " units=" << (res.units.absolute ? "absolute" : "relative")
          << " unit_nits=" << res.units.unit_nits
          << " normalization=" << (frame_divisor > 0.f ? "fixed" : "gpu-frame")
          << " divisor=" << frame_divisor
          << " transfer="
          << (transfer_mode.load(std::memory_order_relaxed) == 1u
                  ? "consistent" : "ratio")
          << " governor="
          << (!governed ? "n/a"
              : norm_governor.load(std::memory_order_relaxed) == 0u ? "off"
              : norm_governor.load(std::memory_order_relaxed) == 1u ? "slew"
                                                                    : "stable")
          << " slew="
          << (!governed
                  || norm_governor.load(std::memory_order_relaxed) != 1u
                      ? 0.f
                      : norm_slew_stops.load(std::memory_order_relaxed))
          << " attack_release="
          << (!governed
                  || norm_governor.load(std::memory_order_relaxed) < 2u
                      ? 0.f
                      : norm_attack_stops.load(std::memory_order_relaxed))
          << '/'
          << (!governed
                  || norm_governor.load(std::memory_order_relaxed) < 2u
                      ? 0.f
                      : norm_release_stops.load(std::memory_order_relaxed))
          << " snaps=" << norm_snap_count.load(std::memory_order_relaxed)
          << "{prime:" << norm_snap_prime.load(std::memory_order_relaxed)
          << ",epoch:" << norm_snap_epoch.load(std::memory_order_relaxed)
          << ",reset:" << norm_snap_reset.load(std::memory_order_relaxed)
          << "} ws=" << res.id
          << " curve=" << (FrameDisplayCodec(res) ? 1u : 0u)
          << " exposure=" << ExposureContractText(current.exposure)
          << " feed=" << FeedText(feed)
          << " stack=" << res.stack_passes
          << " pedestal=" << (pedestal ? "on" : "off");
  Log(reshade::log::level::info, message.str());
}

inline void BindCodec(
    ID3D12GraphicsCommandList* command_list,
    const FinalResources& resources,
    ID3D12PipelineState* pipeline,
    // Root table 0 (t0-t3) start within the workset heap: the calling stack
    // pass's SRV set (pass * kCodecSrvSetStride) so every pass reads its own
    // never-rewritten descriptors.
    uint32_t srv_table_start,
    uint32_t uav_index,
    uint32_t width,
    uint32_t height,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t source_base_x,
    uint32_t source_base_y,
    uint32_t proxy_width,
    uint32_t proxy_height,
    uint32_t neural_width,
    uint32_t neural_height,
    // Snapshot once per frame by the caller: the present thread updates the
    // adaptive gain while the game renders, and the encode and decode of one
    // frame must observe the exact same value.
    float paper_white,
    // PQ bridge anchor snapshot (nits), same per-frame snapshot discipline;
    // consumed only by the hdr_mode == 2 normalization branches.
    float diffuse_white,
    // Per-stack-pass decode blend strengths (PassTransferStrength /
    // PassColorStrength); the encode dispatches ignore them.
    float pass_transfer,
    float pass_color) {
  ID3D12DescriptorHeap* heaps[] = {resources.descriptors};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(codec_pipeline.root_signature);
  command_list->SetPipelineState(pipeline);
  const D3D12_GPU_DESCRIPTOR_HANDLE heap_start =
      resources.descriptors->GetGPUDescriptorHandleForHeapStart();
  command_list->SetComputeRootDescriptorTable(
      0,
      DescriptorHeapSlot(
          heap_start, srv_table_start, resources.descriptor_size));
  command_list->SetComputeRootDescriptorTable(
      1,
      DescriptorHeapSlot(heap_start, uav_index, resources.descriptor_size));
  struct Constants {
    uint32_t width;
    uint32_t height;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t source_base_x;
    uint32_t source_base_y;
    uint32_t proxy_width;
    uint32_t proxy_height;
    uint32_t neural_width;
    uint32_t neural_height;
    float paper_white_scale;
    float transfer_strength;
    float color_strength;
    uint32_t hdr;
    float diffuse_white_nits;
    // Padded to dword 24: SetComputeRoot32BitConstants copies the full
    // count, so the host struct must be at least that large.  The legacy
    // shaders never read dwords 15..23, so they stay zero here apart from
    // the codec-mode flag below (the v6 map is V6CodecConstants).
    float padding[9];
    uint32_t foveation[2];      // dwords 24..25, foveation.hlsli
    float foveation_gmax[2];    // dwords 26..27
  } constants{
      width,
      height,
      source_width,
      source_height,
      source_base_x,
      source_base_y,
      proxy_width,
      proxy_height,
      neural_width,
      neural_height,
      paper_white,
      pass_transfer,
      pass_color,
      static_cast<uint32_t>(resources.hdr_mode),
      diffuse_white,
      {codec_mode.load() != 0 ? 1.f : 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f,
       0.f},
      {resources.foveation.x.packed, resources.foveation.y.packed},
      {resources.foveation.x.gmax, resources.foveation.y.gmax}};
  static_assert(
      sizeof(constants) == kCodecRootConstants * sizeof(uint32_t),
      "the legacy view fills the codec root signature's constant block");
  static_assert(
      offsetof(Constants, foveation)
          == kCodecFoveationDword * sizeof(uint32_t),
      "the foveated grid starts at dword 24 in every codec view");
  command_list->SetComputeRoot32BitConstants(2, kCodecRootConstants, &constants, 0);
}

// The v6 codec constant buffer, mirroring `cbuffer CodecConstants` in
// v6_common.hlsli field for field.  Hoisted out of BindCodecV6 so a dispatch
// that needs a field the shared bind does not thread - today only the
// autoscale governor - can overwrite exactly its dwords by offsetof instead
// of widening a helper that eleven call sites pass positionally.
struct V6CodecConstants {
  uint32_t width;
  uint32_t height;
  uint32_t source_width;
  uint32_t source_height;
  uint32_t source_base_x;
  uint32_t source_base_y;
  uint32_t proxy_width;
  uint32_t proxy_height;
  uint32_t neural_width;
  uint32_t neural_height;
  float divisor;
  float pedestal_cap;
  float transfer_strength;
  float color_strength;
  uint32_t encoding;
  float dark_gate;
  uint32_t curve;
  float pedestal_gate;
  float slew_stops;
  float snap_now;
  float chroma_clamp_stops;
  float release_stops;
  uint32_t governor_mode;
  uint32_t transfer_mode;
  // Foveated working grid (foveation.hlsli): packed X/Y (0 = uniform axis)
  // and the host-computed G(1) per axis.
  uint32_t foveation[2];
  float foveation_gmax[2];
};
static_assert(
    sizeof(V6CodecConstants) == kCodecRootConstants * sizeof(uint32_t),
    "the codec root signature declares exactly 28 32-bit constants");
static_assert(
    offsetof(V6CodecConstants, foveation) == kCodecFoveationDword * sizeof(uint32_t),
    "the foveated grid starts at dword 24 in every codec view");

// v6-family bind: same root signature and table layout as BindCodec, but the
// constants follow the v6 cbuffer view (Divisor / PedestalCap / Encoding /
// DarkGate).  The divisor snapshot discipline matches the legacy gain: the
// caller computes it once per frame so the encode and resolve of one frame
// observe the exact same value.
inline void BindCodecV6(
    ID3D12GraphicsCommandList* command_list,
    const FinalResources& resources,
    ID3D12PipelineState* pipeline,
    uint32_t srv_table_start,
    uint32_t uav_index,
    uint32_t width,
    uint32_t height,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t source_base_x,
    uint32_t source_base_y,
    uint32_t proxy_width,
    uint32_t proxy_height,
    uint32_t neural_width,
    uint32_t neural_height,
    float divisor,
    float pedestal_cap,
    float transfer_strength,
    float color_strength,
    uint32_t encoding,
    float dark_gate,
    float chroma_clamp_stops) {
  ID3D12DescriptorHeap* heaps[] = {resources.descriptors};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(codec_pipeline.root_signature);
  command_list->SetPipelineState(pipeline);
  const D3D12_GPU_DESCRIPTOR_HANDLE heap_start =
      resources.descriptors->GetGPUDescriptorHandleForHeapStart();
  command_list->SetComputeRootDescriptorTable(
      0,
      DescriptorHeapSlot(
          heap_start, srv_table_start, resources.descriptor_size));
  command_list->SetComputeRootDescriptorTable(
      1,
      DescriptorHeapSlot(heap_start, uav_index, resources.descriptor_size));
  V6CodecConstants constants{
      width,
      height,
      source_width,
      source_height,
      source_base_x,
      source_base_y,
      proxy_width,
      proxy_height,
      neural_width,
      neural_height,
      divisor,
      pedestal_cap,
      transfer_strength,
      color_strength,
      encoding,
      dark_gate,
      FrameDisplayCodec(resources)                               ? 1u
      : neural_floor_guard.load(std::memory_order_relaxed) ? 2u
                                                           : 0u,
      // PedestalGate: the commit's dark-pedestal removal on this frame.
      FramePedestalRemoval(resources) ? 1.f : 0.f,
      0.f,
      0.f,
      chroma_clamp_stops,
      0.f,
      0u,
      transfer_mode.load(std::memory_order_relaxed),
      {resources.foveation.x.packed, resources.foveation.y.packed},
      {resources.foveation.x.gmax, resources.foveation.y.gmax}};
  command_list->SetComputeRoot32BitConstants(2, kCodecRootConstants, &constants, 0);
}

// Derive the relative-HDR normalization from the SAME frame that is about to
// enter NR.  This is deliberately a GPU-only producer/consumer chain:
// work0 -> 64x36 reduction -> 1x1 candidate -> governor -> encode.  There is
// no cadence, readback, confidence gate, or CPU-held value to go stale.
//
// The one piece of cross-frame state is the committed divisor in
// res.norm_commit, which the dispatch reads and rewrites in place (see the
// governor note in v6_autoscale.cs_5_1.hlsl).  Estimation stays stateless
// and per-frame so it can never freeze; only the commitment is
// rate-limited, so it can never step.  `game_reset` is the game's own NGX
// Reset flag.  Under Slew (mode 1) a cut or any reset-epoch advance
// invalidates the carried value and snaps.  Stable (mode 2) snaps only a
// workset's first frame: the epoch also advances on every overlay edit and
// feature recreate, none of which changes the source's radiometry, and a
// cut is followed by the game's own exposure rather than a step here - the
// fast attack limit covers the one direction that clips.
//
// Feed v2 (frame.feed != kV1, see FrameFeed) replaces the whole chain above
// with v6_exposure_scale: the texel is the game's own exposure (or a fixed
// 1), read on the GPU from the exposure texture inside the evaluate window,
// with no estimator and no rate limit; the only carried value is the scale
// it holds through an unreadable frame.  Either feed snaps when the workset
// last ran under the other one.
// Returns the snap reasons (norm_trace::kSnap* bits, 0 = slewed frame).
inline uint32_t PrepareFrameScale(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Device* device,
    FinalResources& res,
    const EvaluationContract& frame) {
  if (res.units.absolute || res.norm_scale == nullptr
      || res.norm_commit == nullptr) {
    return 0u;
  }

  // Snap conditions are all CPU-knowable; the shader additionally snaps on an
  // uninitialized (zero) texel, which covers a workset whose commit resource
  // was recreated without this bookkeeping noticing.
  const uint32_t governor_mode = norm_governor.load(std::memory_order_relaxed);
  const bool stable = governor_mode >= 2u;
  const uint64_t epoch = history_reset_epoch.load(std::memory_order_relaxed);
  const uint32_t snap_reasons =
      (!res.norm_commit_primed ? norm_trace::kSnapPrime : 0u)
      | (!stable && res.norm_commit_primed && res.norm_commit_epoch != epoch
             ? norm_trace::kSnapEpoch
             : 0u)
      | (!stable && frame.frame_reset != 0 ? norm_trace::kSnapReset : 0u)
      | (res.norm_commit_primed && res.norm_feed != frame.feed
             ? norm_trace::kSnapFeed
             : 0u);
  const bool snap = snap_reasons != 0u;
  res.norm_commit_primed = true;
  res.norm_commit_epoch = epoch;
  res.norm_feed = frame.feed;
  // Stable mode's per-frame limits come from wall-clock rates, so the
  // settling speed no longer depends on the NR frame rate (frame generation
  // halves it).  The step is clamped: a stall or loading screen must not
  // turn into one unbounded jump, and a first frame gets a nominal 60 Hz.
  const int64_t now_ns = SteadyNowNs();
  const float frame_seconds = res.norm_last_ns != 0
      ? std::clamp((now_ns - res.norm_last_ns) / 1e9f, 1.f / 240.f, 0.1f)
      : 1.f / 60.f;
  res.norm_last_ns = now_ns;
  if (snap) {
    norm_snap_count.fetch_add(1, std::memory_order_relaxed);
    ++res.snaps;
    if ((snap_reasons & norm_trace::kSnapPrime) != 0u) {
      norm_snap_prime.fetch_add(1, std::memory_order_relaxed);
    }
    if ((snap_reasons & norm_trace::kSnapEpoch) != 0u) {
      norm_snap_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    if ((snap_reasons & norm_trace::kSnapReset) != 0u) {
      norm_snap_reset.fetch_add(1, std::memory_order_relaxed);
    }
  }

  Transition(
      command_list, res.norm_scale, res.norm_scale_state,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  res.norm_scale_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  // Order this frame's read of the committed texel against the previous
  // frame's write.  Same queue, but UAV accesses across command lists still
  // need the barrier to be ordered.  norm_commit never changes state, so this
  // is the only synchronization it ever needs.
  UavBarrier(command_list, res.norm_commit);

  if (frame.feed != FeedSource::kV1) {
    FeedSource source = frame.feed;
    uint32_t table = kDescriptorMeterSet;  // valid, and unread without a texture
    if (source == FeedSource::kTexture) {
      // One set per distinct exposure texture, never rewritten while a
      // recorded list may read it (the motion ring's discipline).  A full
      // ring recycles an entry the submission tracker proves no list can
      // still execute or replay (every bind of a set tracks its texture),
      // so a game that recreates its exposure texture on loads keeps the
      // feed instead of holding one scale for the workset's life.  With no
      // provable entry, or no exact proofs at all (queue hooks not live),
      // the frame keeps the committed scale (counted).
      uint32_t ring = 0;
      while (ring < kExposureRingSize && res.exposure_views[ring] != nullptr
             && res.exposure_views[ring] != frame.exposure) {
        ++ring;
      }
      if (ring == kExposureRingSize && queue_tracking_active.load()) {
        ring = 0;
        while (ring < kExposureRingSize
               && !submission::ResourceReleasable(res.exposure_views[ring])) {
          ++ring;
        }
        if (ring < kExposureRingSize) {
          ReleaseCom(res.exposure_views[ring]);
          feed_exposure_ring_recycled.fetch_add(1, std::memory_order_relaxed);
        }
      }
      if (ring == kExposureRingSize) {
        feed_exposure_ring_full.fetch_add(1, std::memory_order_relaxed);
        source = FeedSource::kHold;
      } else {
        const uint32_t set = kDescriptorExposureRing + ring * kCodecSrvSetStride;
        if (res.exposure_views[ring] == nullptr) {
          frame.exposure->AddRef();
          res.exposure_views[ring] = frame.exposure;
          D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
          srv.Format = ExposureViewFormat(frame.exposure->GetDesc());
          srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
          srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
          srv.Texture2D.MipLevels = 1;
          for (uint32_t slot = 0; slot < kCodecSrvSetStride; ++slot) {
            res.bound_sources[set + slot] = frame.exposure;
            device->CreateShaderResourceView(
                frame.exposure, &srv,
                DescriptorHeapSlot(
                    res.descriptors->GetCPUDescriptorHandleForHeapStart(),
                    set + slot, res.descriptor_size));
          }
        }
        submission::TrackUse(command_list, frame.exposure);
        table = set;
      }
    }
    (source == FeedSource::kFixed ? feed_fixed_frames : feed_texture_frames)
        .fetch_add(1, std::memory_order_relaxed);
    BindCodecV6(
        command_list, res, codec_pipeline.exposure_scale,
        table, kDescriptorNormScaleUav,
        1, 1, 1, 1,
        0, 0, 1, 1, 1, 1,
        0.f, 0.f, 0.f, 0.f, 1u, 0.f, 0.f);
    // v6_exposure_scale's own cbuffer view: dwords 0..3.
    const uint32_t feed_constants[4] = {
        static_cast<uint32_t>(source),
        std::bit_cast<uint32_t>(frame.pre_exposure),
        std::bit_cast<uint32_t>(frame.exposure_scale),
        std::bit_cast<uint32_t>(snap ? 1.f : 0.f)};
    command_list->SetComputeRoot32BitConstants(2, 4, feed_constants, 0);
  } else {
    BindCodecV6(
        command_list, res, codec_pipeline.autoscale,
        kDescriptorMeterSet, kDescriptorNormScaleUav,
        1, 1, res.width, res.height,
        0, 0, 1, 1, 1, 1,
        0.f, 0.f, 0.f, 0.f, 1u, 0.f, 0.f);
    // Governor fields, overwriting exactly their dwords (slew_stops through
    // governor_mode) of what BindCodecV6 just wrote.  Off (mode 0) sends a
    // zero limit, which the shader reads as "adopt the candidate" - the
    // v6.1.0 path, bit for bit; chroma_clamp_stops sits inside the range and
    // stays 0 like BindCodecV6 left it for this dispatch.
    const float attack_step = stable
        ? norm_attack_stops.load(std::memory_order_relaxed) * frame_seconds
        : norm_slew_stops.load(std::memory_order_relaxed);
    const float release_step = stable
        ? norm_release_stops.load(std::memory_order_relaxed) * frame_seconds
        : 0.f;
    const uint32_t governor[5] = {
        std::bit_cast<uint32_t>(governor_mode == 0u ? 0.f : attack_step),
        std::bit_cast<uint32_t>(snap ? 1.f : 0.f),
        std::bit_cast<uint32_t>(0.f),
        std::bit_cast<uint32_t>(release_step),
        governor_mode};
    static_assert(
        offsetof(V6CodecConstants, governor_mode)
            == offsetof(V6CodecConstants, slew_stops) + 4 * sizeof(uint32_t),
        "the governor dword run is slew_stops..governor_mode");
    command_list->SetComputeRoot32BitConstants(
        2, 5, governor,
        offsetof(V6CodecConstants, slew_stops) / sizeof(uint32_t));
  }
  command_list->Dispatch(1, 1, 1);
  UavBarrier(command_list, res.norm_scale);
  UavBarrier(command_list, res.norm_commit);
  Transition(
      command_list, res.norm_scale, res.norm_scale_state,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  res.norm_scale_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  return snap_reasons;
}

// Proper-mode post-decode conditioning: estimate the NR model's dark
// pedestal per 32x32 block (reduce) and subtract it from the decoded image
// (apply).  Both passes reuse the codec root signature with the UAV table
// rooted at the calling pass's write surface: u0 = decoded / decoded_alt,
// u1 = its adjacent block-mean view (see the kDescriptor* layout comment).
// decoded stays in UAV state across both passes; block_mean lives its whole
// life in UAV state.  SDR worksets only (and only outside classic mode):
// HDR runs the v6 pipeline, whose commit pass applies the linear-domain
// pedestal with the DarkGate and transfer-strength scaling the legacy pair
// lacks.
inline void RunBlackLevelRestore(
    ID3D12GraphicsCommandList* command_list,
    const FinalResources& resources,
    // The calling stack pass's SRV set (pass * kCodecSrvSetStride) and write
    // UAV slot (kDescriptorDecodedUav / kDescriptorDecodedAltUav).
    uint32_t srv_table_start,
    uint32_t write_uav_index,
    uint32_t width,
    uint32_t height,
    float paper_white,
    float diffuse_white,
    float pass_transfer,
    float pass_color,
    // The look shaped this pass (LookRoute::shaped): measure NR's own lift
    // from the unshaped model output instead of the shaped write surface.
    bool measure_unshaped) {
  if (codec_mode.load() == 0 || resources.hdr_mode != 0) return;
  if (codec_pipeline.block_mean_reduce == nullptr
      || codec_pipeline.black_restore_apply == nullptr
      || resources.block_mean == nullptr) {
    return;
  }
  const uint32_t blocks_x = (width + 31) / 32;
  const uint32_t blocks_y = (height + 31) / 32;
  // The reduce reads the WRITE surface (u0) and the pre-NR reference
  // (t3 of this pass's SRV set), and the apply writes the write surface:
  // order both against the actual ping-pong target, not `decoded`
  // unconditionally.
  ID3D12Resource* const write_surface =
      write_uav_index == kDescriptorDecodedAltUav ? resources.decoded_alt
                                                  : resources.decoded;
  // The unshaped reduce resolves this pass's codec set (P and the unshaped N
  // at the NR resolution) exactly as the decode does; the classic one reads
  // neither extent.
  BindCodec(
      command_list,
      resources,
      measure_unshaped && codec_pipeline.block_mean_reduce_unshaped != nullptr
          ? codec_pipeline.block_mean_reduce_unshaped
          : codec_pipeline.block_mean_reduce,
      srv_table_start,
      write_uav_index,
      width,
      height,
      width,
      height,
      0,
      0,
      resources.input_width,
      resources.input_height,
      resources.input_width,
      resources.input_height,
      paper_white,
      diffuse_white,
      pass_transfer,
      pass_color);
  // One 32x32 block per 16x16-thread group (kReduceMain): the block grid IS
  // the dispatch. The historical /16 shape launched 16x fewer groups than
  // the grid, leaving all but a corner of the block-mean texture at zero -
  // the pedestal estimate (and with it most of the black-level restore)
  // silently never applied.
  command_list->Dispatch(blocks_x, blocks_y, 1);
  UavBarrier(command_list, resources.block_mean);
  UavBarrier(command_list, write_surface);
  BindCodec(
      command_list,
      resources,
      codec_pipeline.black_restore_apply,
      srv_table_start,
      write_uav_index,
      width,
      height,
      width,
      height,
      0,
      0,
      width,
      height,
      width,
      height,
      paper_white,
      diffuse_white,
      pass_transfer,
      pass_color);
  command_list->Dispatch((width + 15) / 16, (height + 15) / 16, 1);
  UavBarrier(command_list, write_surface);
}

// Writes one SRV into a workset's shader-visible heap, but only when the
// slot's bound resource actually changes: the heap may still be read by
// command lists recorded for previous frames, and rewriting a shader-visible
// slot while it is in flight is unsynchronized even when the bytes are
// identical.  Legal only from EnsureWorkset (creation) and - for the pre-SR
// path's pass-0 reference, the game color, which does not exist as a scratch
// surface at heap creation - before any dispatch of the frame is recorded.
// The workset is keyed by (handle, color), so after the first frame every
// rebinding targets identical content and becomes a no-op; what this must
// never do is move later in a recording, where it would alias the slot for
// every earlier dispatch once the GPU executes the finished list.
inline void SetSourceView(
    ID3D12Device* device,
    FinalResources& resources,
    ID3D12Resource* source,
    uint32_t slot = 0) {
  if (device == nullptr || source == nullptr || resources.descriptors == nullptr) {
    return;
  }
  if (slot >= std::size(resources.bound_sources)) return;
  if (resources.bound_sources[slot] == source) return;
  resources.bound_sources[slot] = source;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = CodecViewFormat(ConcreteResourceFormat(source->GetDesc().Format));
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(
      source,
      &srv,
      DescriptorHeapSlot(
          resources.descriptors->GetCPUDescriptorHandleForHeapStart(),
          slot,
          resources.descriptor_size));
}

// ---------------------------------------------------------------------------
// Foveated NR guides (foveation.hpp, shaders/fov_guides.cs_5_1.hlsl).
//
// DLSSNR maps the depth/MVec subrects it is given uniformly onto its own grid.
// On a warped working grid that would misplace the guides by hundreds of
// pixels toward the sides, so a foveated frame hands NR its own depth and
// motion, resampled onto the warped grid (MVecScale 1, subrects = the working
// surface).  One dispatch per evaluate, shared by every stack pass.

// SRV format for a game depth buffer (depth formats are typeless or
// depth-only; the view must name the readable channel).
inline DXGI_FORMAT FovDepthViewFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
      return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
      return format;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

// SRV format for game motion vectors, or UNKNOWN when a float view cannot
// read them (integer encodings: the guide pass then stands down).
inline DXGI_FORMAT FovMotionViewFormat(DXGI_FORMAT format) {
  const DXGI_FORMAT concrete = ConcreteResourceFormat(format);
  switch (concrete) {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
      return concrete;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

// The ring slot whose SRV set views (depth, motion), writing it on first use.
// Write-once like the motion ring: a set is never rewritten while a recorded
// list may read it.  -1 when the ring is full or a format is unreadable.
inline int FovGuideRingSlot(
    ID3D12Device* device, FinalResources& res, ID3D12Resource* depth,
    ID3D12Resource* motion) {
  for (uint32_t ring = 0; ring < kFovGuideRingSize; ++ring) {
    if (res.fov_depth_views[ring] == depth && res.fov_motion_views[ring] == motion) {
      return static_cast<int>(ring);
    }
    if (res.fov_depth_views[ring] != nullptr || res.fov_motion_views[ring] != nullptr) {
      continue;
    }
    const DXGI_FORMAT depth_format = FovDepthViewFormat(depth->GetDesc().Format);
    const DXGI_FORMAT motion_format = FovMotionViewFormat(motion->GetDesc().Format);
    if (depth_format == DXGI_FORMAT_UNKNOWN || motion_format == DXGI_FORMAT_UNKNOWN) {
      static std::atomic<bool> logged_format{false};
      if (!logged_format.exchange(true)) {
        std::ostringstream message;
        message << "foveated NR: guide formats not readable (depth "
                << static_cast<uint32_t>(depth->GetDesc().Format) << ", motion "
                << static_cast<uint32_t>(motion->GetDesc().Format)
                << "); running with the game's guides";
        Log(reshade::log::level::warning, message.str());
      }
      return -1;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu =
        res.descriptors->GetCPUDescriptorHandleForHeapStart();
    const uint32_t base = kDescriptorFovGuideRing + ring * kCodecSrvSetStride;
    for (uint32_t entry = 0; entry < kCodecSrvSetStride; ++entry) {
      const bool is_depth = (entry & 1u) == 0u;
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
      srv.Format = is_depth ? depth_format : motion_format;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels = 1;
      ID3D12Resource* source = is_depth ? depth : motion;
      device->CreateShaderResourceView(
          source, &srv, DescriptorHeapSlot(heap_cpu, base + entry, res.descriptor_size));
      res.bound_sources[base + entry] = source;
    }
    depth->AddRef();
    motion->AddRef();
    res.fov_depth_views[ring] = depth;
    res.fov_motion_views[ring] = motion;
    return static_cast<int>(ring);
  }
  static std::atomic<bool> logged_full{false};
  if (!logged_full.exchange(true)) {
    Log(reshade::log::level::warning,
        "foveated NR: the game cycles more depth/motion pairs than the guide ring"
        " holds; those frames run with the game's guides");
  }
  return -1;
}

// The guide pair at the workset's working size, allocated once per workset
// with its two UAV descriptors (fresh slots, never referenced before).
inline bool EnsureFovGuideSurfaces(ID3D12Device* device, FinalResources& res) {
  if (res.fov_motion != nullptr && res.fov_depth != nullptr) return true;
  if (res.fov_guides_failed) return false;
  if (!CreateScratchTexture(
          device, res.input_width, res.input_height, DXGI_FORMAT_R16G16_FLOAT,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &res.fov_motion,
          L"DLSS5 Generic foveated motion")
      || !CreateScratchTexture(
          device, res.input_width, res.input_height, DXGI_FORMAT_R32_FLOAT,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &res.fov_depth,
          L"DLSS5 Generic foveated depth")) {
    ReleaseCom(res.fov_motion);
    ReleaseCom(res.fov_depth);
    res.fov_guides_failed = true;
    return false;
  }
  res.fov_guides_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  const D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu =
      res.descriptors->GetCPUDescriptorHandleForHeapStart();
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  uav.Format = DXGI_FORMAT_R16G16_FLOAT;
  device->CreateUnorderedAccessView(
      res.fov_motion, nullptr, &uav,
      DescriptorHeapSlot(heap_cpu, kDescriptorFovGuideUav, res.descriptor_size));
  uav.Format = DXGI_FORMAT_R32_FLOAT;
  device->CreateUnorderedAccessView(
      res.fov_depth, nullptr, &uav,
      DescriptorHeapSlot(heap_cpu, kDescriptorFovGuideUav + 1, res.descriptor_size));
  return true;
}

// fov_guides' cbuffer view, dword for dword.
struct FovGuideConstants {
  uint32_t size[2];
  uint32_t motion_grid[2];
  uint32_t motion_base[2];
  uint32_t depth_grid[2];
  uint32_t depth_base[2];
  float motion_scale[2];
  uint32_t flags;
  uint32_t reserved[11];
  uint32_t foveation[2];
  float foveation_gmax[2];
};
static_assert(sizeof(FovGuideConstants) == kCodecRootConstants * sizeof(uint32_t),
              "fov_guides fills the codec root signature's constant block");
static_assert(offsetof(FovGuideConstants, foveation)
                  == kCodecFoveationDword * sizeof(uint32_t),
              "the foveated grid starts at dword 24 in every codec view");

// Records the guide resample for this evaluate.  Leaves the pair in
// NON_PIXEL_SHADER_RESOURCE for NR.  False = hand NR the game's guides (the
// frame still runs, warped colour against unwarped guides, logged once).
inline bool RecordFoveatedGuides(
    ID3D12GraphicsCommandList* command_list, ID3D12Device* device, FinalResources& res,
    const EvaluationContract& frame) {
  if (!res.foveation.active() || frame.depth == nullptr || frame.motion == nullptr) {
    return false;
  }
  // A failed PSO stays failed for the session instead of retrying (and
  // logging) every frame.
  static std::atomic<bool> pso_failed{false};
  bool ok = !pso_failed.load() && EnsureCodecRootSignature(device);
  if (ok && codec_pipeline.fov_guides == nullptr) {
    ok = CreateCodecPipelineState(
        device, __fov_guides, &codec_pipeline.fov_guides,
        L"DLSS5 Generic foveated guides", "foveated guides");
    if (!ok) pso_failed = true;
  }
  ok = ok && EnsureFovGuideSurfaces(device, res);
  const int ring = ok ? FovGuideRingSlot(device, res, frame.depth, frame.motion) : -1;
  if (!ok || ring < 0) {
    static std::atomic<bool> logged{false};
    if (!ok && !logged.exchange(true)) {
      Log(reshade::log::level::warning,
          "foveated NR: guide pass unavailable; running with the game's guides");
    }
    return false;
  }

  const bool low_res =
      (frame.create_flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
  FovGuideConstants constants{};
  constants.size[0] = res.input_width;
  constants.size[1] = res.input_height;
  constants.motion_grid[0] = std::max(low_res ? frame.input_width : frame.output_width, 1u);
  constants.motion_grid[1] = std::max(low_res ? frame.input_height : frame.output_height, 1u);
  constants.motion_base[0] = frame.motion_x;
  constants.motion_base[1] = frame.motion_y;
  constants.depth_grid[0] = std::max(frame.input_width, 1u);
  constants.depth_grid[1] = std::max(frame.input_height, 1u);
  constants.depth_base[0] = frame.depth_x;
  constants.depth_base[1] = frame.depth_y;
  // Exactly the scale the uniform path sends NR as DLSSNR.MVecScaleX/Y.
  constants.motion_scale[0] = frame.motion_scale_x * motion_scale_x_multiplier.load();
  constants.motion_scale[1] = frame.motion_scale_y * motion_scale_y_multiplier.load();
  constants.flags = 3u;  // depth and motion bound
  constants.foveation[0] = res.foveation.x.packed;
  constants.foveation[1] = res.foveation.y.packed;
  constants.foveation_gmax[0] = res.foveation.x.gmax;
  constants.foveation_gmax[1] = res.foveation.y.gmax;

  submission::TrackUse(command_list, frame.depth);
  submission::TrackUse(command_list, frame.motion);
  submission::TrackUse(command_list, res.fov_motion);
  submission::TrackUse(command_list, res.fov_depth);
  Transition(command_list, res.fov_motion, res.fov_guides_state,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Transition(command_list, res.fov_depth, res.fov_guides_state,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  res.fov_guides_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

  ID3D12DescriptorHeap* heaps[] = {res.descriptors};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(codec_pipeline.root_signature);
  command_list->SetPipelineState(codec_pipeline.fov_guides);
  const D3D12_GPU_DESCRIPTOR_HANDLE heap_gpu =
      res.descriptors->GetGPUDescriptorHandleForHeapStart();
  command_list->SetComputeRootDescriptorTable(
      0, DescriptorHeapSlot(
             heap_gpu, kDescriptorFovGuideRing + static_cast<uint32_t>(ring) * kCodecSrvSetStride,
             res.descriptor_size));
  command_list->SetComputeRootDescriptorTable(
      1, DescriptorHeapSlot(heap_gpu, kDescriptorFovGuideUav, res.descriptor_size));
  command_list->SetComputeRoot32BitConstants(2, kCodecRootConstants, &constants, 0);
  command_list->Dispatch((res.input_width + 15) / 16, (res.input_height + 15) / 16, 1);
  UavBarrier(command_list, res.fov_motion);
  UavBarrier(command_list, res.fov_depth);
  Transition(command_list, res.fov_motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(command_list, res.fov_depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  res.fov_guides_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

  static std::atomic<bool> logged_active{false};
  if (!logged_active.exchange(true)) {
    const foveation::Grid& g = res.foveation;
    std::ostringstream message;
    message << "foveated NR active: working " << res.input_width << 'x' << res.input_height
            << " for output " << res.width << 'x' << res.height << " (centre "
            << g.x.center_px << 'x' << g.y.center_px << ", fall-off on screen "
            << g.x.falloff_px << '/' << g.y.falloff_px << " px, edge density "
            << g.x.edge_reached << '/' << g.y.edge_reached << ", "
            << static_cast<int>(std::lround(g.PixelFactor() * 100.0)) << "% of the NR pixels)";
    Log(reshade::log::level::info, message.str());
  }
  return true;
}

// The look surfaces a chain needs (kLookSurface* bits): the output with Shape
// result on or under the transport (the compose writes G_low beside N'), the
// bands for Tone, Detail, Halo or Stabilize, the transport pair for
// Edge-aware upsampling below the output resolution, the block statistics
// for NREditTrace.  0 when none of them runs.  The programs are created
// here, before the workset is sized; if they cannot be, the look and the
// trace are off for the session (NR runs on without them).
inline uint32_t LookSurfacesWanted(
    ID3D12Device* device, const look::Settings& settings, uint32_t nr_width,
    uint32_t nr_height, uint32_t full_width, uint32_t full_height) {
  // Settings are neutral with the look off (LookSettingsSnapshot), so the
  // bands follow from them alone.
  const bool transport =
      look::TransportWanted(settings, nr_width, nr_height, full_width, full_height);
  const uint32_t wanted =
      (look_mode.load() || transport ? kLookSurfaceOutput : 0u)
      | (look::BandsWanted(settings) ? kLookSurfaceBands : 0u)
      | (transport ? kLookSurfaceTransport : 0u)
      | (edit_trace_enabled.load() ? kLookSurfaceTrace : 0u);
  if (wanted == 0 || look_unavailable.load()) return 0;
  if (!EnsureLookPipeline(device)) {
    look_unavailable = true;
    Log(reshade::log::level::error,
        "NR look stage off for this session: its programs could not be created;"
        " Neural Rendering continues without it");
    return 0;
  }
  return wanted;
}

// One chain's look inputs, fixed before the pass loop so every pass of a
// frame shapes with the same values while the overlay may change them.
struct LookChain {
  look::Settings settings;
  uint32_t wanted = 0;  // LookSurfacesWanted
  // The game's own motion-vector scale for this frame (before the NR
  // working-grid factor and the user multiplier).
  float motion_scale[2] = {1.f, 1.f};
};

// Where a pass's resolve reads P and N: its SRV set and their extents.
struct LookRoute {
  uint32_t srv_set = 0;
  uint32_t proxy_width = 0;
  uint32_t proxy_height = 0;
  uint32_t neural_width = 0;
  uint32_t neural_height = 0;
  // The compose wrote N' for this pass: the restores measure NR's own lift
  // from the codec set's unshaped N (RunBlackLevelRestore, the v6 reduce).
  bool shaped = false;
};

// The NR look stage for one pass, recorded after the model's output is in a
// shader-resource state and before the pass's resolve (look_stage.hpp):
//   - NREditTrace first, on the codec set, so it measures the RAW edit
//     whether or not the look then reshapes it (observe-only);
//   - Shape result: the bands, the temporal filter on the slot's history,
//     and every gain into look_output (N'); the resolve then reads the look
//     set;
//   - Edge-aware upsampling below the output resolution: P and N' at the
//     output resolution, and the resolve reads the transport set 1:1.
// Returns the pass's codec route, unchanged, whenever the stage does not
// shape: every gain the identity, no temporal filter and no transport means
// no dispatch, and the resolve reads the model's output bit for bit.
inline LookRoute RunLookStage(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Device* device,
    FinalResources& res,
    NrFeatureSlot& slot,
    uint32_t pass,
    const LookChain& chain,
    const EvaluationContract& frame) {
  const uint32_t codec_set = pass * kCodecSrvSetStride;
  const LookRoute unchanged{
      codec_set, res.input_width, res.input_height, res.input_width, res.input_height};
  const look::Settings& settings = chain.settings;
  // What this chain wanted AND the workset holds: a failed allocation leaves
  // nothing, a sticky workset may hold more than is wanted now.
  const uint32_t live = chain.wanted & res.look_surfaces;
  const bool transport = (live & kLookSurfaceTransport) != 0;
  const bool shape =
      (live & kLookSurfaceOutput) != 0 && look::Dispatched(settings, transport);
  bool temporal = shape && settings.stabilize != 0;
  LookHistory& history = slot.look_history;
  if (history.buffers[0] != nullptr
      && (!temporal || history.width != res.input_width
          || history.height != res.input_height)) {
    // Stabilize went off (or the look did), or the NR resolution changed:
    // the pair retires like a handle, behind this slot's tracked uses.
    retired_nr_features.push_back({
        .generation = present_generation,
        .lease = AcquireGpuLease(),
        .tracker_proof = slot.tracked_since_create
            && queue_tracking_active.load(std::memory_order_relaxed),
        .look_history = {history.buffers[0], history.buffers[1]},
    });
    history = {};
  }
  if (live == 0 || codec_pipeline.look.compose == nullptr) return unchanged;

  const D3D12_GPU_DESCRIPTOR_HANDLE heap_gpu =
      res.descriptors->GetGPUDescriptorHandleForHeapStart();
  // Unused history and motion bind something valid of the declared kind:
  // the trace histogram (a raw buffer) and the codec set (never read).
  look::Bindings bindings{
      .heap = res.descriptors,
      .codec_set = DescriptorHeapSlot(heap_gpu, codec_set, res.descriptor_size),
      .look_set = DescriptorHeapSlot(
          heap_gpu, kDescriptorLookSet + codec_set, res.descriptor_size),
      .uav_table = DescriptorHeapSlot(heap_gpu, kDescriptorLookUav, res.descriptor_size),
      .motion = DescriptorHeapSlot(heap_gpu, codec_set, res.descriptor_size),
      .history_in = res.look_trace_histogram->GetGPUVirtualAddress(),
      .history_out = res.look_trace_histogram->GetGPUVirtualAddress(),
  };
  look::Frame look_frame{
      .nr_width = res.input_width,
      .nr_height = res.input_height,
      .full_width = res.width,
      .full_height = res.height,
      .reference_linear = res.hdr_mode != 0,
      .transport = transport,
      .foveation = {res.foveation.x.packed, res.foveation.y.packed},
      .foveation_gmax = {res.foveation.x.gmax, res.foveation.y.gmax},
  };

  if ((live & kLookSurfaceTrace) != 0) {
    look::RecordTrace(
        command_list, codec_pipeline.look, bindings,
        look::MakeConstants(settings, look_frame));
    edit_trace::Record(
        command_list, device, res.look_trace_histogram, res.look_trace_blocks,
        {.generation = present_generation,
         .time_ns = SteadyNowNs(),
         .workset = res.id,
         .pass = pass + 1,
         .nr_width = res.input_width,
         .nr_height = res.input_height});
    look_traced_passes.fetch_add(1, std::memory_order_relaxed);
  }
  if (!shape) {
    if ((live & kLookSurfaceTrace) != 0) {
      gpu_timers::Mark(command_list, gpu_timers::PassLookEnd(pass));
    }
    return unchanged;
  }

  const int64_t now_ns = SteadyNowNs();
  if (temporal && history.buffers[0] == nullptr) {
    const uint64_t bytes = static_cast<uint64_t>(res.input_width) * res.input_height
        * look::kHistoryBytesPerPixel;
    if (CreateScratchBuffer(device, bytes, &history.buffers[0], L"DLSS5 Generic look history")
        && CreateScratchBuffer(
            device, bytes, &history.buffers[1], L"DLSS5 Generic look history")) {
      history.width = res.input_width;
      history.height = res.input_height;
    } else {
      // No history this frame: shape without the temporal filter and try
      // again next frame (a failed allocation is logged by the helper).
      for (ID3D12Resource*& buffer : history.buffers) ReleaseCom(buffer);
      temporal = false;
    }
  }
  if (temporal) {
    const float gap = history.last_ns != 0 ? (now_ns - history.last_ns) / 1e9f : 0.f;
    // The history restarts exactly when the model's own does (DLSSNR.Reset:
    // startup, a cut, a settings change, the game's Reset), and after a stall.
    const bool restart =
        !history.valid || slot.model_reset || gap > look::kHistoryMaxGapSeconds;
    if (restart && history.valid) {
      look_history_restarts.fetch_add(1, std::memory_order_relaxed);
    }
    look_frame.history_valid = !restart;
    look_frame.frame_seconds = history.last_ns != 0
        ? std::clamp(gap, 1.f / 240.f, look::kHistoryMaxGapSeconds)
        : 1.f / 60.f;
    bindings.history_in = history.buffers[history.written]->GetGPUVirtualAddress();
    bindings.history_out = history.buffers[history.written ^ 1u]->GetGPUVirtualAddress();
    submission::TrackUse(command_list, history.buffers[0]);
    submission::TrackUse(command_list, history.buffers[1]);
    if (settings.stabilize == 2u && frame.motion != nullptr) {
      // A write-once view per distinct motion-vector resource: a view is
      // never rewritten while a recorded list may read it.  A game cycling
      // more resources than the ring holds runs static on the rest (counted).
      uint32_t ring = 0;
      while (ring < kMotionRingSize && res.motion_views[ring] != nullptr
             && res.motion_views[ring] != frame.motion) {
        ++ring;
      }
      if (ring == kMotionRingSize) {
        look_motion_ring_full.fetch_add(1, std::memory_order_relaxed);
      } else {
        if (res.motion_views[ring] == nullptr) {
          frame.motion->AddRef();
          res.motion_views[ring] = frame.motion;
          SetSourceView(device, res, frame.motion, kDescriptorMotionRing + ring);
        }
        submission::TrackUse(command_list, frame.motion);
        // DLSS motion vectors (Programming Guide 3.6): MV * MVecScale is the
        // displacement to the previous position in pixels of the grid the
        // vectors are rendered at - the render resolution with MVLowRes,
        // the output resolution without it.  The history lives on the NR
        // grid, so the scale carries that ratio and the user multiplier.
        const bool low_res =
            (frame.create_flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
        const uint32_t grid_width = low_res ? frame.input_width : frame.output_width;
        const uint32_t grid_height = low_res ? frame.input_height : frame.output_height;
        look_frame.motion = grid_width != 0 && grid_height != 0;
        look_frame.motion_scale[0] = chain.motion_scale[0]
            * motion_scale_x_multiplier.load()
            * static_cast<float>(res.input_width) / static_cast<float>(grid_width);
        look_frame.motion_scale[1] = chain.motion_scale[1]
            * motion_scale_y_multiplier.load()
            * static_cast<float>(res.input_height) / static_cast<float>(grid_height);
        look_frame.motion_base[0] = frame.motion_x;
        look_frame.motion_base[1] = frame.motion_y;
        look_frame.motion_grid[0] = grid_width;
        look_frame.motion_grid[1] = grid_height;
        bindings.motion = DescriptorHeapSlot(
            heap_gpu, kDescriptorMotionRing + ring, res.descriptor_size);
      }
    }
    // Orders this frame's history read after the previous frame's write.
    look::UavBarrierAll(command_list);
  }

  // The filter flag follows `temporal`, not the setting (v7.0.0-rc9).  After
  // a failed history allocation above, both history bindings are still the
  // trace histogram, and HistoryOut is a root UAV - no bounds check - so a
  // compose that kept Stabilize's flag stored a whole NR frame of history
  // past the end of that small buffer.
  look::Settings shaping = settings;
  if (!temporal) shaping.stabilize = 0;
  const look::Constants constants = look::MakeConstants(shaping, look_frame);
  Transition(
      command_list, res.look_output, res.look_output_state,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  look::RecordCompose(command_list, codec_pipeline.look, bindings, constants);
  Transition(
      command_list, res.look_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  res.look_output_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  if (temporal) {
    history.written ^= 1u;
    history.valid = true;
    history.last_ns = now_ns;
  }
  if (look_shaped_passes.fetch_add(1, std::memory_order_relaxed) == 0) {
    std::ostringstream message;
    message << "NR look stage shaped its first pass: ws" << res.id << " pass " << (pass + 1)
            << ' ' << res.input_width << 'x' << res.input_height << " flags=0x" << std::hex
            << constants.flags;
    Log(reshade::log::level::info, message.str());
  }
  if (!transport) {
    gpu_timers::Mark(command_list, gpu_timers::PassLookEnd(pass));
    return {kDescriptorLookSet + codec_set, res.input_width, res.input_height,
            res.input_width, res.input_height, true};
  }
  for (ID3D12Resource* surface : {res.look_up_proxy, res.look_up_neural}) {
    Transition(
        command_list, surface, res.look_up_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  look::RecordUpsample(command_list, codec_pipeline.look, bindings, constants);
  for (ID3D12Resource* surface : {res.look_up_proxy, res.look_up_neural}) {
    Transition(
        command_list, surface, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  res.look_up_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  look_transport_passes.fetch_add(1, std::memory_order_relaxed);
  gpu_timers::Mark(command_list, gpu_timers::PassLookEnd(pass));
  return {kDescriptorTransportSet + codec_set, res.width, res.height, res.width,
          res.height, true};
}

inline bool SubrectFits(
    uint32_t base_x,
    uint32_t base_y,
    uint32_t width,
    uint32_t height,
    const D3D12_RESOURCE_DESC& resource) {
  return width != 0 && height != 0
      && static_cast<uint64_t>(base_x) + width <= resource.Width
      && static_cast<uint64_t>(base_y) + height <= resource.Height;
}

// Finds the DLSS contract state for `handle`, registering it lazily from the
// evaluate contract when the create was never intercepted (titles that create
// DLSS before the hooks install, e.g. Control).  `context` distinguishes the
// insertion points in the one-shot log.  Callers that cross an
// EnsureDirectRuntime device-change teardown MUST re-invoke this afterwards:
// a device change releases every feature state (the map is cleared), so any
// FeatureState& bound before the call would dangle.
inline std::unordered_map<const NVSDK_NGX_Handle*, FeatureState>::iterator
FindOrRegisterFeature(
    const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* game_parameters,
    const char* context) {
  if (auto entry = features.find(handle); entry != features.end()) return entry;
  if (const auto contract = create_contracts.find(handle);
      contract != create_contracts.end()) {
    return features.emplace(handle, contract->second).first;
  }
  // Two very different reasons for a handle to be missing, and only one of
  // them is a warning.  After a teardown (the swapchain on the device NR was
  // initialized on went away) the registry is cleared by design while the
  // game keeps its own DLSS feature, so the next evaluate MUST find nothing
  // and re-register - that is the re-arm working, not a title that created
  // DLSS before the hooks installed.  Saying the second when the first
  // happened is the kind of log line that sends a triage the wrong way.
  const uint64_t teardowns =
      destroy_swapchain_matches.load(std::memory_order_relaxed)
      + destroy_device_matches.load(std::memory_order_relaxed);
  if (teardowns != 0) {
    static std::atomic<bool> logged_rearm_registration{false};
    if (!logged_rearm_registration.exchange(true)) {
      Log(
          reshade::log::level::info,
          std::string(context) + "NR: re-registering DLSS feature handle "
              + std::to_string(reinterpret_cast<uintptr_t>(handle))
              + " after a teardown; the game kept its feature across it, so"
                " NR re-arms from the evaluate contract");
    }
  } else {
    static std::atomic<bool> logged_lazy_registration{false};
    if (!logged_lazy_registration.exchange(true)) {
      Log(
          reshade::log::level::warning,
          std::string(context) + "NR: DLSS feature handle "
              + std::to_string(reinterpret_cast<uintptr_t>(handle))
              + " not in registry (CreateFeature was not intercepted - game"
                " created DLSS before hooks installed); registering lazily from"
                " evaluate contract");
    }
  }
  FeatureState state = DeriveFeatureState(game_parameters);
  state.source_feature = kFeatureDlss;
  return features.emplace(handle, state).first;
}

// NR working resolution from the resolution controls: follow the game's
// guide (render) resolution, or a 33-100% fraction of the reference
// resolution (the DLSS output for the after path, the NGX color for pre-SR;
// 1.0 = native).  Shared by both insertion points.
inline void ComputeNrWorkingResolution(
    uint32_t reference_width,
    uint32_t reference_height,
    uint32_t guide_width,
    uint32_t guide_height,
    uint32_t& nr_width,
    uint32_t& nr_height,
    // Foveated grid (foveation.hpp), null = uniform.  The uniform result
    // above sets the CENTRE density; each warped extent is that times the
    // axis's mean density, so the centre keeps it exactly.
    const foveation::Grid* grid = nullptr) {
  if (nr_follow_input_res.load() && guide_width != 0 && guide_height != 0) {
    nr_width = std::min(guide_width, reference_width);
    nr_height = std::min(guide_height, reference_height);
  } else {
    const float scale = std::clamp(nr_resolution_scale.load(), 0.33f, 1.f);
    nr_width = std::max(1u, static_cast<uint32_t>(
        std::lround(static_cast<double>(reference_width) * scale)));
    nr_height = std::max(1u, static_cast<uint32_t>(
        std::lround(static_cast<double>(reference_height) * scale)));
  }
  if (grid != nullptr && grid->active()) {
    nr_width = std::min(foveation::WarpedExtent(nr_width, grid->x), reference_width);
    nr_height = std::min(foveation::WarpedExtent(nr_height, grid->y), reference_height);
  }
}

// Contract debounce shared by both insertion points: a NEW working-resolution
// contract commits only after it has held for kContractDebounceFrames
// consecutive frames - or immediately when the user committed a change from
// the overlay (nr_resolution_commit_now).  Until commit, nr_width/height are
// snapped back to the applied contract so the frame evaluates consistently.
// Titles that alternate contracts per frame never trigger a rebuild at all -
// the documented recreate-storm/VRAM-churn failure mode.  Returns true when
// the new contract commits this frame.
inline bool CommitNrResolution(
    FeatureState& feature,
    uint32_t& nr_width,
    uint32_t& nr_height) {
  if (feature.output_width == 0) return false;
  if (feature.input_width == nr_width && feature.input_height == nr_height) {
    // An armed overlay commit whose dims are already applied is satisfied -
    // consume it here so it cannot skip the settle window for a later,
    // unrelated game-driven change.
    nr_resolution_commit_now.exchange(false);
    feature.debounce_width = 0;
    feature.debounce_height = 0;
    feature.debounce_frames = 0;
    // First contact counts as applied: the stored dims already ARE the
    // working resolution this stream runs at.
    feature.working_resolution_committed = true;
    return false;
  }
  if (!feature.working_resolution_committed) {
    // First evaluation: commit immediately instead of running the debounce
    // window against stale stored render dims.  Debouncing a startup would
    // evaluate the first ~kContractDebounceFrames frames at the stored dims
    // and then recreate the features/workset once settled - the startup
    // double-workset the v5.3 field log showed one second apart.  Debounce
    // still guards every LATER change (sliders, DRS, viewport switches).
    feature.working_resolution_committed = true;
    feature.debounce_width = 0;
    feature.debounce_height = 0;
    feature.debounce_frames = 0;
    Log(
        reshade::log::level::info,
        "NR working resolution initialized on first evaluation: "
            + std::to_string(nr_width) + "x" + std::to_string(nr_height));
    return true;
  }
  if (nr_resolution_commit_now.exchange(false)) {
    feature.debounce_width = 0;
    feature.debounce_height = 0;
    feature.debounce_frames = 0;
    Log(
        reshade::log::level::info,
        "NR working resolution committed immediately (overlay): "
            + std::to_string(nr_width) + "x" + std::to_string(nr_height));
    return true;
  }
  if (feature.debounce_width == nr_width
      && feature.debounce_height == nr_height) {
    if (++feature.debounce_frames >= kContractDebounceFrames) {
      feature.debounce_width = 0;
      feature.debounce_height = 0;
      feature.debounce_frames = 0;
      Log(
          reshade::log::level::info,
          "NR working resolution committed after settle: "
              + std::to_string(nr_width) + "x" + std::to_string(nr_height));
      return true;
    }
  } else {
    feature.debounce_width = nr_width;
    feature.debounce_height = nr_height;
    feature.debounce_frames = 0;
  }
  // Keep evaluating at the applied contract until the new one settles.  A
  // partially-registered contract can carry an output size without guide
  // dims; snapping to a zero pair would fail the workset and latch the
  // feature, so the computed dims stand until a real applied contract exists.
  if (feature.input_width != 0 && feature.input_height != 0) {
    nr_width = feature.input_width;
    nr_height = feature.input_height;
  }
  return false;
}

// One-shot notice for a committed sub-native working grid (both insertion
// points share the latch: the message describes the mechanism, not a path).
inline void LogNrWorkingResolutionReduced(
    uint32_t reference_width,
    uint32_t reference_height,
    uint32_t nr_width,
    uint32_t nr_height) {
  static std::atomic<bool> logged_subnative{false};
  if (nr_width >= reference_width || logged_subnative.exchange(true)) return;
  Log(
      reshade::log::level::info,
      "NR working resolution reduced (" + std::to_string(reference_width)
          + "x" + std::to_string(reference_height) + " -> "
          + std::to_string(nr_width) + "x" + std::to_string(nr_height)
          + "); the decode stage upscales, and NR reads the game's motion"
            " vectors at their own scale (through rc3 they were shortened by"
            " the working-res fraction - the likely cause of the pan flicker"
            " reported below ~85%). Edge-aware upsampling (Look) keeps edges"
            " sharper at this resolution");
}

// Runs NR immediately after a successful game DLSS evaluation. This insertion
// point is the only engine-independent place where color, motion, depth, reset,
// subrect, and scale metadata are all available under one documented NGX
// contract. The game's original result is retained unless every NR step works.
inline bool IsSupportedInjectionCommandList(
    ID3D12GraphicsCommandList* command_list) {
  if (command_list == nullptr) return false;
  const D3D12_COMMAND_LIST_TYPE type = command_list->GetType();
  if (type == D3D12_COMMAND_LIST_TYPE_DIRECT
      || type == D3D12_COMMAND_LIST_TYPE_COMPUTE) {
    return true;
  }
  static std::atomic_bool logged_unsupported_command_list{false};
  if (!logged_unsupported_command_list.exchange(true)) {
    Log(
        reshade::log::level::warning,
        "NR skipped: unsupported D3D12 command-list type "
            + std::to_string(static_cast<unsigned>(type))
            + "; injection requires a direct or compute list");
  }
  return false;
}

inline bool ProcessInline(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* game_parameters,
    bool* command_list_touched = nullptr) {
  if (!enabled.load() || command_list == nullptr || handle == nullptr
      || game_parameters == nullptr
      || !IsSupportedInjectionCommandList(command_list)) {
    return false;
  }
  if (command_list_touched != nullptr) *command_list_touched = false;
  InjectedCommandScope injected_command_scope;
  if (!IsDlssEvaluation(game_parameters)) {
    if (!logged_native_decline.exchange(true)) {
      const bool has_color =
          GetD3D12Resource(game_parameters, NVSDK_NGX_Parameter_Color) != nullptr;
      const bool has_output =
          GetD3D12Resource(game_parameters, NVSDK_NGX_Parameter_Output) != nullptr;
      const bool has_motion =
          GetD3D12Resource(game_parameters, NVSDK_NGX_Parameter_MotionVectors)
          != nullptr;
      const bool has_depth =
          GetD3D12Resource(game_parameters, NVSDK_NGX_Parameter_Depth) != nullptr;
      Log(
          reshade::log::level::info,
          "NR declined an evaluate: parameter block has color="
              + std::to_string(has_color)
              + " output=" + std::to_string(has_output)
              + " motion=" + std::to_string(has_motion)
              + " depth=" + std::to_string(has_depth)
              + "; the guides are supplied outside the NGX block (Streamline tags)."
                " To fix: set EnableHooks=1 in the [RenoDX.DLSS5] section of"
                " ReShade.ini and restart the game");
    }
    CountNrDecline(NrDeclineReason::kNgxNotDlssEvaluation);
    streamline_escalation_deficits.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  auto* output = GetD3D12Resource(game_parameters, NVSDK_NGX_Parameter_Output);
  auto* motion = GetD3D12Resource(game_parameters, NVSDK_NGX_Parameter_MotionVectors);
  auto* depth = GetD3D12Resource(game_parameters, NVSDK_NGX_Parameter_Depth);
  auto* exposure =
      GetD3D12Resource(game_parameters, NVSDK_NGX_Parameter_ExposureTexture);
  if (output == nullptr || motion == nullptr || depth == nullptr) {
    CountNrDecline(NrDeclineReason::kNgxMissingGuides);
    streamline_escalation_deficits.fetch_add(1, std::memory_order_relaxed);
    if (!logged_ngx_missing_guides.exchange(true)) {
      Log(reshade::log::level::warning,
          "NR declined an evaluate: the NGX DLSS parameter block lacks "
          "Output/Motion/Depth pointers (color-only integration); frames run "
          "without NR");
    }
    return false;
  }

  // Multi-pass: a different pass from the same source family may chain into
  // the same output in one present; mirrored duplicates and NGX/SL mirror
  // pairs are declined (see OutputPassAllowed).  The pass is recorded on
  // success, at the bottom of this function.
  if (!AdmitOutputPass(output, handle)) return false;

  const D3D12_RESOURCE_DESC output_desc = output->GetDesc();
  const D3D12_RESOURCE_DESC motion_desc = motion->GetDesc();
  const D3D12_RESOURCE_DESC depth_desc = depth->GetDesc();
  if (output_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
      || output_desc.DepthOrArraySize != 1
      || output_desc.SampleDesc.Count != 1) {
    CountNrDecline(NrDeclineReason::kNgxOutputGeometry);
    if (!logged_ngx_output_geometry.exchange(true)) {
      Log(reshade::log::level::warning,
          "NR declined an evaluate: output geometry is unsupported (dim="
              + std::to_string(static_cast<unsigned>(output_desc.Dimension))
              + " array=" + std::to_string(output_desc.DepthOrArraySize)
              + " samples=" + std::to_string(output_desc.SampleDesc.Count)
              + "); requires a single-sample 2D texture");
    }
    last_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
    return false;
  }
  if (output_desc.MipLevels != 1 && !logged_multimip_output.exchange(true)) {
    Log(reshade::log::level::info,
        "DLSS output has " + std::to_string(output_desc.MipLevels)
            + " mip levels; NR consumes and replaces mip 0 only");
  }

  // Initialize the direct runtime before the feature lookup: a device change
  // inside EnsureDirectRuntime releases every feature state, so the
  // FeatureState& bound below must not exist yet (see FindOrRegisterFeature).
  if (!EnsureDirectRuntime(command_list)) {
    // Named, not silent: with no nvngx_dlssnr.dll on the machine this is
    // where EVERY evaluate ends, and until v6.8.0-alpha23 it ended without
    // counting anything.  The verdict said UNAVAILABLE and was right; the
    // funnel under it read `unaccounted=239` out of 240 (measured by the
    // `no_nr_runtime` lane on its first run), which is T-SILENT's own
    // definition of a leak.  Only the two evaluate-path terminals count:
    // EnsureNrFeature reaches its copy of this check inside an injection
    // that already passed one, so counting there would double-count.
    CountNrDecline(NrDeclineReason::kNrRuntimeUnavailable);
    return false;
  }

  ID3D12Device* device = nullptr;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    last_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
    return false;
  }

  auto entry = FindOrRegisterFeature(handle, game_parameters, "");
  FeatureState& feature = entry->second;
  // HDR transfer mode must be current before the NR codec/resources are
  // dimensioned.  Infer it from the actual DLSS output format (linear FP16/R11G11B10
  // -> scene-linear, R10G10B10A2 -> PQ) and promote a bare SDR format to linear
  // when the game set the NGX IsHDR create flag.
  const DXGI_FORMAT output_format = ConcreteResourceFormat(output_desc.Format);
  uint8_t hdr_mode = InferHdrMode(output_format);
  if ((feature.create_flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0) {
    // Game explicitly signalled HDR: promote a bare SDR format to linear-HDR.
    if (hdr_mode == 0) hdr_mode = 1;
  } else if (output_format == DXGI_FORMAT_R10G10B10A2_UNORM) {
    // No HDR flag: a 10-bit R10G10B10A2 output is SDR (e.g. Control), NOT PQ.
    // Only treat R10G10B10A2 as PQ when the game set the HDR flag.
    hdr_mode = 0;
  }
  FeatureState current = DeriveFeatureState(game_parameters);
  ObserveGameFrame(current);
  // KCD2 (and most titles) send the full DLSS subrect/size contract only at
  // CreateFeature, not on every EvaluateFeature.  Trust the create-captured
  // contract stored in `feature` for the guide (input) dimensions and subrects;
  // adopt a per-evaluate value only when the game actually supplied one. Output
  // dimensions come from the real output resource (ground truth).
  if (current.input_width == 0) {
    current.input_width = feature.create_input_width != 0
        ? feature.create_input_width
        : feature.input_width;
  }
  if (current.input_height == 0) {
    current.input_height = feature.create_input_height != 0
        ? feature.create_input_height
        : feature.input_height;
  }
  if (current.motion_x == 0 && current.motion_y == 0) {
    current.motion_x = feature.motion_x;
    current.motion_y = feature.motion_y;
  }
  if (current.depth_x == 0 && current.depth_y == 0) {
    current.depth_x = feature.depth_x;
    current.depth_y = feature.depth_y;
  }
  // Absent per-axis motion scales default to the FINAL guide dims (the
  // create-contract fallback above has already applied), never to the zero
  // dims a per-evaluate block can still carry at DeriveFeatureState time.
  if (!current.has_motion_scale_x) {
    current.motion_scale_x = static_cast<float>(current.input_width);
  }
  if (!current.has_motion_scale_y) {
    current.motion_scale_y = static_cast<float>(current.input_height);
  }
  const uint32_t width = static_cast<uint32_t>(output_desc.Width);
  const uint32_t height = static_cast<uint32_t>(output_desc.Height);
  if (current.input_width == 0 || current.input_height == 0) {
    // Last-resort derive: the NGX Color (the DLSS input image) is
    // render-resolution by definition, so when neither the per-evaluate
    // block nor the create-captured contract carries guide dims, its
    // resource desc IS the guide contract (mirrors the pre-SR path's use of
    // Color geometry).  Games that set sizes only on a CreateFeature our
    // hook never saw used to decline forever.
    auto* color_resource =
        GetD3D12Resource(game_parameters, NVSDK_NGX_Parameter_Color);
    if (color_resource != nullptr) {
      const D3D12_RESOURCE_DESC color_desc = color_resource->GetDesc();
      if (color_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        if (current.input_width == 0) {
          current.input_width = static_cast<uint32_t>(color_desc.Width);
        }
        if (current.input_height == 0) {
          current.input_height = color_desc.Height;
        }
      }
    }
  }
  if (current.input_width == 0 || current.input_height == 0) {
    last_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (!logged_no_guide_dims.exchange(true)) {
      Log(reshade::log::level::warning,
          "NR skipped: the game's NGX contract has no guide (input) dimensions."
          " If the game uses NVIDIA Streamline, set EnableHooks=1 in the"
          " [RenoDX.DLSS5] section of ReShade.ini and restart; otherwise this"
          " title is not supported and NR stays off");
    }
    device->Release();
    return false;
  }
  // Soft contract check: a subrect that does not fit the guide is logged once but
  // no longer rejects the frame.  v2.5 engaged NR without this hard gate, and the
  // NR runtime keys off the actual resources, so a discrepancy is at worst a
  // quality issue, not a correctness blocker.
  if (!logged_contract_note.exchange(true)) {
    const bool motion_fits = SubrectFits(
        current.motion_x, current.motion_y, current.input_width, current.input_height,
        motion_desc);
    const bool depth_fits = SubrectFits(
        current.depth_x, current.depth_y, current.input_width, current.input_height,
        depth_desc);
    if (!motion_fits || !depth_fits) {
      Log(
          reshade::log::level::info,
          "NR contract note: subrect fit motion=" + std::to_string(motion_fits)
              + " depth=" + std::to_string(depth_fits)
              + " (guide " + std::to_string(current.input_width) + "x"
              + std::to_string(current.input_height) + ", motion "
              + std::to_string(motion_desc.Width) + "x" + std::to_string(motion_desc.Height)
              + ", depth " + std::to_string(depth_desc.Width) + "x"
              + std::to_string(depth_desc.Height) + ")");
    }
  }

  // NR working resolution (v5).  Shared controls with the pre-SR path; the
  // reference here is the DLSS output resolution.  The NR feature always
  // runs 1:1 at this working resolution and the decode stage upscales,
  // which sidesteps the low-res Color contract the signed runtime rejects.
  uint32_t nr_input_width = width;
  uint32_t nr_input_height = height;
  // One foveation snapshot for the whole evaluate (working size, guides,
  // encode, NR, look, resolve).
  const foveation::Grid frame_foveation = CurrentFoveation(width, height);
  ComputeNrWorkingResolution(
      width, height, current.input_width, current.input_height,
      nr_input_width, nr_input_height, &frame_foveation);
  const uint32_t requested_stack = std::clamp(stack_passes.load(), 1u, kMaxNrPasses);
  const bool dims_committed =
      CommitNrResolution(feature, nr_input_width, nr_input_height);
  LogNrWorkingResolutionReduced(width, height, nr_input_width, nr_input_height);
  // The game's MVecScale goes to NR unchanged at every working resolution
  // (PLAN_FEED_V2.md F8). The runtime reads MV x MVecScale in pixels of the
  // MVec subrect it is given - the render grid here - and maps that onto its
  // own grid itself. Measured in the e2e host (853x480 -> 1280x720, 3 output
  // px/frame pan, MVLowRes) by sweeping NRMVecScaleX: at 100%, 75% and 50%
  // working resolution the sharpest output and the smallest edit sit at an
  // effective factor of 1.0. Through rc3 this multiplied by the applied
  // working-res fraction, which shortened every vector below 100%, and the
  // field reported flicker on pans below ~85%.
  const bool dimensions_changed = dims_committed;
  // A new warp re-registers every pixel of the working grid, so the temporal
  // histories restart.  Deliberately a history reset, not a recreate: a
  // slider drag changes the warp every tick, and per-tick recreation is the
  // documented recreate-storm/VRAM-churn failure.  A changed working WIDTH
  // still recreates, through the contract debounce above.
  if (feature.slots[0].handle != nullptr && feature.foveation != frame_foveation) {
    RequestHistoryReset();
  }
  const bool configuration_changed = feature.slots[0].handle != nullptr
      && feature.slots[0].configuration_generation
          != configuration_generation.load();
  // The NR features are created with the HDR/AutoExposure create flags baked
  // in; recreate them all if the HDR mode changes so the flags match the
  // content (stack passes share one HDR mode).
  const bool hdr_changed = feature.slots[0].handle != nullptr
      && feature.slots[0].nr_hdr_mode != hdr_mode;
  if (dimensions_changed || configuration_changed || hdr_changed) {
    // Contract-level change: every stack pass's feature must be recreated.
    // Deliberately NOT triggered by a null slot-0 handle: after a failure the
    // handle is released but the failure latch must survive so the bounded
    // retry interval holds (EnsureNrFeature recreates the handle in-loop).
    // Lowering the pass count frees the retired passes' handles through the
    // overlay's RecreateFeatures call.  The recreated slots reset themselves
    // via pending_reset; this stream's contract change must not advance the
    // global reset epoch (other streams' histories are unaffected).
    ReleaseAllNrSlots(feature, true);
    for (auto& slot : feature.slots) {
      slot.failed = false;
      slot.fail_count = 0;
    }
  }

  feature.input_width = nr_input_width;
  feature.input_height = nr_input_height;
  feature.output_width = width;
  feature.output_height = height;
  feature.foveation = frame_foveation;
  feature.motion_x = current.motion_x;
  feature.motion_y = current.motion_y;
  feature.depth_x = current.depth_x;
  feature.depth_y = current.depth_y;
  int32_t evaluation_flags = 0;
  if (NVSDK_NGX_SUCCEED(game_parameters->Get(
          NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
          &evaluation_flags))) {
    feature.create_flags = static_cast<uint32_t>(evaluation_flags);
  }
  feature.motion_scale_x = current.motion_scale_x;
  feature.motion_scale_y = current.motion_scale_y;
  feature.perf_quality = current.perf_quality;
  feature.frame_reset = current.frame_reset;
  // All-or-nothing failure gating across the stack: any slot that failed and
  // has not exhausted its retry interval skips NR for the whole chain this
  // frame; expired latches are cleared here so the bounded retry re-enters.
  for (uint32_t gate = 0; gate < requested_stack; ++gate) {
    NrFeatureSlot& slot = feature.slots[gate];
    if (slot.failed) {
      // Bounded automatic retry (v5 compatibility): a transient failure -
      // driver reset, a one-off rejected contract, a level-load dimension
      // change - self-heals instead of requiring the overlay reset; a
      // permanently rejected contract re-fails at most once per interval.
      if (!FeatureFailureExpired(slot)) {
        CountNrDecline(NrDeclineReason::kFailureBackoff);
        device->Release();
        return false;
      }
      slot.failed = false;
      ++slot.fail_count;
      static std::atomic<bool> logged_auto_retry{false};
      if (!logged_auto_retry.exchange(true)) {
        Log(
            reshade::log::level::info,
            "NR feature automatically retrying after an earlier failure "
            "(exponential backoff from 1 s up to ~30 s; use the overlay"
            " reset for an immediate one)");
      }
    }
  }

  const bool codec_ok = hdr_mode != 0 ? EnsureV6CodecPipeline(device)
                                      : EnsureLegacyCodecPipeline(device);
  // The NR look stage (look_stage.hpp): this chain's look values and the
  // surfaces they need, fixed before the workset is sized.
  LookChain look_chain{
      .settings = LookSettingsSnapshot(),
      .motion_scale = {current.motion_scale_x, current.motion_scale_y},
  };
  if (codec_ok) {
    look_chain.wanted = LookSurfacesWanted(
        device, look_chain.settings, nr_input_width, nr_input_height, width, height);
  }
  FinalResources* workset = codec_ok
      ? EnsureWorkset(
              device,
              handle,
              output,
              nr_input_width,
              nr_input_height,
              width,
              height,
              output_desc.Format,
              hdr_mode,
              requested_stack,
              look_chain.wanted)
      : nullptr;
  // The NR features themselves are ensured per stack pass inside the chain
  // below; this gate covers the codec pipeline and the scratch surfaces only.
  const bool setup_ok = workset != nullptr;
  // Exact submission-use proof (issue 01): this evaluate's recordings on the
  // game's list can reference the workset's GPU objects.
  if (setup_ok) TrackWorksetRecording(command_list, *workset);
  bool capture_this_eval = false;
  if (setup_ok && screenshot::IsArmed()) {
    // Re-record on every DLSS-family evaluate within the capture-frame so the
    // LAST evaluate (the visible camera compose, after any reflection/secondary
    // denoise that feeds it) wins instead of the first one - EXCEPT that a
    // SMALLER output never supersedes a larger pending one: a half-res
    // secondary pass composing after the main pass must not steal the capture
    // (the "black frame with a single object" failure).  Equal-or-larger area
    // always supersedes.
    uint32_t capture_x = GetUInt(
        game_parameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0);
    uint32_t capture_y = GetUInt(
        game_parameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0);
    uint32_t capture_w = width;
    uint32_t capture_h = height;
    // Engines that render DLSS into a subrect of a larger output resource
    // (DLSSG-era pipelines) leave unrendered black margins around the actual
    // image; crop the capture to the engine-declared output rect when it is
    // smaller than the resource.  Absent/zero params capture the full
    // resource exactly as before.
    if (current.output_width != 0 && current.output_width < capture_w) {
      capture_w = current.output_width;
    }
    if (current.output_height != 0 && current.output_height < capture_h) {
      capture_h = current.output_height;
    }
    const uint64_t capture_area =
        static_cast<uint64_t>(capture_w) * capture_h;
    if (!screenshot::HasPending()
        || capture_area >= screenshot::PendingCaptureArea()) {
      if (screenshot::PrepareCapture(
              device,
              workset->original,
              capture_x,
              capture_y,
              capture_w,
              capture_h,
              workset->resource_format,
              workset->hdr_mode,
              present_generation)) {
        capture_this_eval = true;
        if (workset->hdr_mode != 0) {
          // Float evidence for the v6 chain: the untouched linearized source
          // (work0), prepared here before the chain runs.  The proxy, neural,
          // and work_final planes are prepared at their record sites instead
          // - under a multi-pass stack each pass records its own
          // "<name>_p<pass>" plane (RecordStackPlaneCopy).
          screenshot::PrepareDiagnosticPlane(
              device, "work0", workset->work0,
              workset->width, workset->height);
        }
      } else {
        // Allocations can fail deterministically (e.g. an engine subrect the
        // capture rejects), so this must stay one-shot or it floods the log
        // every evaluate until the arm times out.
        static std::atomic<bool> logged_capture_alloc_failure{false};
        if (!logged_capture_alloc_failure.exchange(true)) {
          Log(reshade::log::level::error,
              "NR screenshot buffers could not be allocated; capture will retry on the next evaluate");
        }
      }
    }
  }
  if (!setup_ok) {
    if (!codec_ok) {
      Log(reshade::log::level::error,
          "NR inline path failed: embedded codec pipeline creation failed");
    }
    // EnsureWorkset's failure sites log their own reason and descriptor.
    CountNrDecline(NrDeclineReason::kWorksetSetupFailed);
    device->Release();
    feature.slots[0].failed = true;
    feature.slots[0].failed_ns = SteadyNowNs();
    return false;
  }
  FinalResources& res = *workset;
  // Stage A: hand the raw exposure evidence to the workset so the present-
  // side normalization lock reads the same contract this evaluate saw.

  // NGX requires the game's DLSS output as an UAV. Copy it out and restore that
  // exact state before doing any private work. From this point onward an early
  // return must restore the host compute state captured around the real evaluate.
  if (command_list_touched != nullptr) *command_list_touched = true;
  // Per-stage GPU timestamps (NRGpuTimers): begin the frame segment before
  // the copy-in so the copies are inside the measured total.
  const bool timers_active = gpu_timers_enabled.load(std::memory_order_relaxed);
  if (timers_active) {
    gpu_timers::Begin(command_list, device, "after", width, height);
  }
  // The game output may carry a mip chain; the addon reads/writes mip 0 only
  // (CopyMip0), so its transitions stay scoped to subresource 0 - an
  // ALL_SUBRESOURCES barrier would assert a before-state for mips the addon
  // never observed and corrupt engines that track them independently.
  Transition(
      command_list, output,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_COPY_SOURCE,
      0u);
  Transition(
      command_list, res.original,
      res.original_state,
      D3D12_RESOURCE_STATE_COPY_DEST);
  CopyMip0(command_list, res.original, output);
  Transition(
      command_list, output,
      D3D12_RESOURCE_STATE_COPY_SOURCE,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      0u);
  if (capture_this_eval) {
    // original now holds the pre-NR DLSS output; snapshot it for the pair.
    Transition(
        command_list, res.original,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    screenshot::RecordPreCopy(command_list, res.original);
  }
  Transition(
      command_list, res.original,
      capture_this_eval ? D3D12_RESOURCE_STATE_COPY_SOURCE
                        : D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  res.original_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  if (timers_active) {
    gpu_timers::Mark(command_list, gpu_timers::kSlotCopyInEnd);
  }

  EvaluationContract frame;
  frame.source_handle = handle;
  frame.motion = motion;
  frame.depth = depth;
  // Guide subrects remain in the game's render resolution.  They are
  // intentionally independent from the NR color surface dimensions in native
  // fallback mode, matching the existing NGX contract.
  frame.input_width = current.input_width;
  frame.input_height = current.input_height;
  frame.output_width = width;
  frame.output_height = height;
  frame.motion_x = current.motion_x;
  frame.motion_y = current.motion_y;
  frame.depth_x = current.depth_x;
  frame.depth_y = current.depth_y;
  frame.create_flags = feature.create_flags;
  frame.motion_scale_x = current.motion_scale_x;
  frame.motion_scale_y = current.motion_scale_y;
  frame.jitter_x = current.jitter_x;
  frame.jitter_y = current.jitter_y;
  frame.has_jitter = current.has_jitter;
  frame.frame_reset = current.frame_reset;
  frame.exposure = exposure;
  frame.pre_exposure = current.pre_exposure > 0.f ? current.pre_exposure : 1.f;
  frame.exposure_scale =
      current.exposure.scale > 0.f ? current.exposure.scale : 1.f;
  frame.feed = FrameFeed(&feature, current, res, exposure);

  // Foveated working grid: from here every codec and look bind of this
  // evaluate forwards res.foveation, and NR gets guides warped the same way.
  // Per axis: an axis the debounce still holds at the full extent runs
  // uniform this frame (a warp over an unshrunk axis would only cost).
  res.foveation = frame_foveation;
  if (res.input_width >= res.width) res.foveation.x = {};
  if (res.input_height >= res.height) res.foveation.y = {};
  res.fov_guides_valid = res.foveation.active()
      && RecordFoveatedGuides(command_list, device, res, frame);
  if (res.foveation.active() && !res.fov_guides_valid) {
    static std::atomic<bool> logged_unwarped{false};
    if (!logged_unwarped.exchange(true)) {
      Log(reshade::log::level::warning,
          "foveated NR: this frame's guides are not warped; edits toward the"
          " sides may smear until the guide pass runs");
    }
  }

  // One gain snapshot for the whole frame's encode, decode, and black
  // restore.  SDR worksets run the legacy codec family: Classic uses the
  // paper-white scale, the anchored family the fixed anchor knee over 60
  // (the v5 proper-mode gain; SDR has no metering).  HDR worksets run the
  // v6 family and snapshot the mode-policy divisor once instead - the
  // encode and resolve of one frame must observe the exact same value.
  const float frame_paper_white =
      codec_mode.load() == 0u
          ? std::max(0.0001f, paper_white_scale.load())
          : std::max(0.0002f, proxy_anchor_nits.load() / 60.f);
  // PQ bridge anchor snapshot (nits): consumed only by the hdr_mode == 2
  // legacy codec branches; see NRDiffuseWhiteNits.
  const float frame_diffuse_white = diffuse_white_nits.load();
  // v6 constant snapshots (HDR worksets): the commit encoding (1 = linear
  // multiply, 2 = PQ), the 1-nit dark gate expressed in source units, and
  // the calibrated pedestal cap.
  const uint32_t v6_encoding =
      res.units.encoding == codec::Encoding::Pq ? 2u : 1u;
  const float v6_dark_gate =
      res.units.absolute && res.units.unit_nits > 0.f
          ? 1.f / res.units.unit_nits
          : 0.005f;
  const float v6_pedestal_cap = codec::BlackPedestalCap(res.units);
  const float frame_divisor =
      res.hdr_mode != 0 ? FrameCodecDivisor(res) : 1.f;
  // Resolve-side chroma bound.  NR transfer otherwise runs at full authority.
  const float v6_chroma_clamp = chroma_clamp_stops.load(std::memory_order_relaxed);
  LogFrameContract(game_parameters, current, res, frame_divisor, frame.feed, "inline");
  if (capture_this_eval) {
    std::ostringstream meta;
    meta << "path=inline"
         << "\nencoding="
         << (res.units.encoding == codec::Encoding::Pq ? "pq"
             : res.units.encoding == codec::Encoding::Linear ? "linear" : "sdr")
         << "\nunits=" << (res.units.absolute ? "absolute" : "relative")
         << "\nunit_nits=" << res.units.unit_nits
         << "\nhdr_mode=" << static_cast<uint32_t>(res.hdr_mode)
         << "\ncodec_mode=" << codec_mode.load()
         << "\ncurve=" << (FrameDisplayCodec(res) ? 1u : 0u)
         << "\nnormalization=" << (frame_divisor > 0.f ? "fixed" : "gpu-frame")
         << "\ndivisor=" << frame_divisor
         << "\npedestal_cap=" << v6_pedestal_cap
         << "\ndark_gate=" << v6_dark_gate
         << "\nstack=" << requested_stack
         << "\nchroma_clamp=" << chroma_clamp_stops.load(std::memory_order_relaxed)
         << "\nplanes=work0:f32-rgba,proxy[:p<pass>]:f32-rgba,"
            "neural[:p<pass>]:f32-rgba,work_final[:p<pass>]:f32-rgba"
            "(post-transfer,pre-pedestal)\n";
    screenshot::SetPendingDiagnosticMeta(meta.str());
  }
  // Exact zero-transfer bypass (handoff rule 3): with every pass's transfer
  // and color strength at zero the chain contributes nothing, so an HDR
  // frame records nothing at all - no linearize, no autoscale, no evaluate, and
  // above all no encode-back - and the game output passes through
  // bit-identical (no PQ roundtrip, no clamping).
  transfer_strength_inert.store(!FramePedestalRemoval(res), std::memory_order_relaxed);
  if (res.hdr_mode != 0 && !ChainActive(res, requested_stack)) {
    // Explicit zero strength is the only v6 image-path bypass.
    if (timers_active) {
      gpu_timers::MarkBypassed();
      gpu_timers::End(command_list);
    }
    ++bypassed_evaluations;
    // Named, not silent: an evaluate that ends here is one the funnel
    // must account for, or the engagement ratio is a guess (T-SILENT).
    CountNrDecline(NrDeclineReason::kZeroStrengthPassthrough);
    device->Release();
    RecordOutputPass(output, handle);
    return true;
  }
  // The NR feature runs 1:1 at the working resolution on every mode
  // (including sub-native NR resolutions; the decode stage upscales).
  const uint32_t feature_out_width = nr_input_width;
  const uint32_t feature_out_height = nr_input_height;
  // Stacking ping-pong: pass 0 writes `decoded`, pass k>=2 reads the previous
  // result and writes the alternate surface, so a decode never samples and
  // writes the same texture.  Pass 1 (stack of 1) matches the historical
  // single-pass behavior exactly.
  ID3D12Resource* const stack_surfaces[2] = {res.decoded, res.decoded_alt};
  D3D12_RESOURCE_STATES* const stack_states[2] = {
      &res.decoded_state, &res.decoded_alt_state};
  // v6 HDR ping-pong: pass k writes work_out_surfaces[k & 1] (pass 0 ->
  // work_a, pass 1 -> work_b, ...) and pass k+1 reads it back, matching the
  // fixed per-pass SRV sets filled at workset creation.
  ID3D12Resource* const work_out_surfaces[2] = {res.work_a, res.work_b};
  D3D12_RESOURCE_STATES* const work_out_states[2] = {
      &res.work_a_state, &res.work_b_state};
  uint32_t frame_snap = 0u;

  if (res.hdr_mode != 0) {
    // v6 linearize: the copied game output (SRV) becomes the whole-frame
    // linear working copy work0 - the metering input, the pedestal
    // reference, and pass 0's encode source.  PQ content decodes here,
    // once; everything downstream until the single commit runs linear.
    Transition(
        command_list,
        res.work0,
        res.work0_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    res.work0_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    BindCodecV6(
        command_list,
        res,
        codec_pipeline.linearize,
        kDescriptorLinearizeSet,
        kDescriptorWork0Uav,
        width,
        height,
        width,
        height,
        0,
        0,
        width,
        height,
        width,
        height,
        frame_divisor,
        v6_pedestal_cap,
        0.f,
        0.f,
        v6_encoding,
        v6_dark_gate,
        0.f);
    command_list->Dispatch((width + 15) / 16, (height + 15) / 16, 1);
    UavBarrier(command_list, res.work0);
    Transition(
        command_list,
        res.work0,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    res.work0_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (capture_this_eval) {
      // Diagnostics: the untouched linearized source the whole chain is
      // referenced against.
      Transition(
          command_list,
          res.work0,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          D3D12_RESOURCE_STATE_COPY_SOURCE);
      screenshot::RecordDiagnosticPlaneCopy(command_list, "work0", res.work0);
      Transition(
          command_list,
          res.work0,
          D3D12_RESOURCE_STATE_COPY_SOURCE,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    // Derive the relative-HDR divisor from this exact linearized frame before
    // it enters the model.  The producer and consumer stay on this command list.
    frame_snap = PrepareFrameScale(command_list, device, res, frame);
    if (timers_active) {
      gpu_timers::Mark(command_list, gpu_timers::kSlotLinearizeEnd);
    }
  }

  NVSDK_NGX_Result stack_failure = NVSDK_NGX_Result_Success;
  uint32_t failed_pass = 0;
  // Last pass that evaluated AND decoded this frame (-1 = none).  Drives the
  // post-loop write-back: a fresh slot is created but not evaluated (frame-
  // ahead maturation), so the loop can end early and the write-back must
  // target the partial stack's last executed pass instead of requested_stack.
  // Resolved once per chain (v5.3): every pass of this chain evaluates
  // against the same reset epoch, so a mid-chain advance cannot split the
  // chain's resets across two epochs.
  const uint64_t chain_reset_epoch =
      history_reset_epoch.load(std::memory_order_relaxed);
  int32_t executed = -1;
  // v6 audit issue 23: `executed` alone cannot distinguish a frame that ran
  // the model from a rule-3 bypass frame whose passes only copied through.
  bool model_evaluated = false;
  // Strengths of the last EXECUTED pass, consumed by the v6 pedestal and
  // commit dispatches after the loop.
  float final_pass_transfer = 0.f;
  float final_pass_color = 0.f;
  bool final_pass_shaped = false;
  for (uint32_t pass = 0; pass < requested_stack; ++pass) {
    // Encode source and decode reference for this pass: the copied DLSS
    // output for the first pass, the previous pass's decoded result after.
    // Each pass re-normalizes the running image into the proxy, so the model
    // sees a display-referred input every time.  v6 HDR swaps in the linear
    // work surfaces instead: work0 for pass 0, the previous pass's work
    // output after (odd passes read work_a, even passes work_b - the fixed
    // set fill's parity).
    ID3D12Resource* const decode_ref =
        res.hdr_mode != 0
            ? (pass == 0 ? res.work0
                         : ((pass & 1) != 0 ? res.work_a : res.work_b))
            : (pass == 0 ? res.original : stack_surfaces[(pass - 1) & 1]);
    D3D12_RESOURCE_STATES* const ref_state =
        res.hdr_mode != 0
            ? (pass == 0 ? &res.work0_state
                         : ((pass & 1) != 0 ? &res.work_a_state
                                            : &res.work_b_state))
            : (pass == 0 ? &res.original_state
                         : stack_states[(pass - 1) & 1]);
    ID3D12Resource* const write_surface =
        res.hdr_mode != 0 ? work_out_surfaces[pass & 1]
                          : stack_surfaces[pass & 1];
    D3D12_RESOURCE_STATES* const write_state =
        res.hdr_mode != 0 ? work_out_states[pass & 1]
                          : stack_states[pass & 1];
    const uint32_t write_uav_index =
        res.hdr_mode != 0
            ? ((pass & 1) != 0 ? kDescriptorWorkBUav : kDescriptorWorkAUav)
            : ((pass & 1) != 0 ? kDescriptorDecodedAltUav
                               : kDescriptorDecodedUav);
    // This pass's SRV set and write UAV slot were fixed at workset creation
    // (kDescriptor* layout) - no descriptor is rewritten mid-recording, so
    // earlier dispatches cannot read a later pass's surfaces.
    const uint32_t srv_table_start = pass * kCodecSrvSetStride;
    const float pass_transfer = PassTransferStrength(pass);
    const float pass_color = PassColorStrength(pass);

    if (res.hdr_mode != 0
        && pass_transfer <= 0.f && pass_color <= 0.f) {
      // Explicit zero-strength pass: carry the running image across exactly
      // while preserving the ping-pong parity expected by later stack passes.
      Transition(
          command_list,
          decode_ref,
          *ref_state,
          D3D12_RESOURCE_STATE_COPY_SOURCE);
      *ref_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
      Transition(
          command_list,
          write_surface,
          *write_state,
          D3D12_RESOURCE_STATE_COPY_DEST);
      *write_state = D3D12_RESOURCE_STATE_COPY_DEST;
      CopyMip0(command_list, write_surface, decode_ref);
      // work0 returns to SRV (the pedestal and commit still read it); a
      // mid-stack work input is spent for the frame and rests in UAV.
      const D3D12_RESOURCE_STATES ref_rest =
          pass == 0 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                    : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      Transition(
          command_list,
          decode_ref,
          D3D12_RESOURCE_STATE_COPY_SOURCE,
          ref_rest);
      *ref_state = ref_rest;
      Transition(
          command_list,
          write_surface,
          D3D12_RESOURCE_STATE_COPY_DEST,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      *write_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      executed = static_cast<int32_t>(pass);
      final_pass_transfer = 0.f;
      final_pass_color = 0.f;
      continue;
    }

    if (feature.slots[pass].handle == nullptr) {
      // Frame-ahead maturation (v5.2): a slot created THIS frame is never
      // evaluated on the same command list - same-frame create+evaluate
      // produces garbage frames and GPU hangs (field evidence from the
      // public forks).  Create it now; it starts evaluating next frame, and
      // the write-back below serves the already-matured passes' result.
      if (!EnsureNrFeature(
              command_list,
              feature,
              pass,
              res,
              nr_input_width,
              nr_input_height,
              feature_out_width,
              feature_out_height)) {
        stack_failure = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
        failed_pass = pass;
      }
      break;
    }
    Transition(
        command_list, decode_ref,
        *ref_state,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    *ref_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Encode the running image into the proxy at the NR working resolution.
    // Source extent is the full-res reference surface (the copied DLSS output
    // for pass 0, the previous decoded result afterwards); the encode shader
    // downsamples via the scaled mapping.  v6 HDR encodes the LINEAR work
    // surface instead: area-filtered downsample, one divisor, hue shoulder,
    // sRGB - matched to the resolve's bilinear reconstruction.
    const uint32_t encode_src_w = width;
    const uint32_t encode_src_h = height;
    Transition(
        command_list, res.proxy,
        res.proxy_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    res.proxy_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (res.hdr_mode != 0) {
      BindCodecV6(
          command_list,
          res,
          codec_pipeline.encode_v6,
          srv_table_start,
          kDescriptorProxyUav,
          nr_input_width,
          nr_input_height,
          encode_src_w,
          encode_src_h,
          0,
          0,
          nr_input_width,
          nr_input_height,
          nr_input_width,
          nr_input_height,
          frame_divisor,
          v6_pedestal_cap,
          pass_transfer,
          pass_color,
          v6_encoding,
          v6_dark_gate,
          0.f);
    } else {
      BindCodec(
          command_list,
          res,
          codec_pipeline.encode,
          srv_table_start,
          kDescriptorProxyUav,
          nr_input_width,
          nr_input_height,
          encode_src_w,
          encode_src_h,
          0,
          0,
          nr_input_width,
          nr_input_height,
          nr_input_width,
          nr_input_height,
          frame_paper_white,
          frame_diffuse_white,
          pass_transfer,
          pass_color);
    }
    if (timers_active) {
      gpu_timers::Mark(
          command_list, gpu_timers::PassEncodeStart(pass));
    }
    command_list->Dispatch((nr_input_width + 15) / 16, (nr_input_height + 15) / 16, 1);
    UavBarrier(command_list, res.proxy);
    if (timers_active) {
      gpu_timers::Mark(command_list, gpu_timers::PassEncodeEnd(pass));
    }
    if (capture_this_eval) {
      // Diagnostics: copy the sRGB proxy the model is about to see.
      Transition(
          command_list, res.proxy,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_COPY_SOURCE);
      screenshot::RecordStackPlaneCopy(
          device, command_list, res.proxy,
          res.input_width, res.input_height,
          "proxy", pass, requested_stack);
      Transition(
          command_list, res.proxy,
          D3D12_RESOURCE_STATE_COPY_SOURCE,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    } else {
      Transition(
          command_list, res.proxy,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    res.proxy_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    Transition(
        command_list, res.nr_output,
        res.nr_output_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    res.nr_output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    SetEvaluationParameters(
        feature,
        feature.slots[pass],
        pass,
        chain_reset_epoch,
        res,
        frame,
        nr_input_width,
        nr_input_height,
        feature_out_width,
        feature_out_height);

    if (timers_active) {
      gpu_timers::Mark(command_list, gpu_timers::PassEvalStart(pass));
    }
    // The NGX runtime records this handle's work into the game's list: a
    // tracked use, so a later retirement waits for exactly this submission.
    submission::TrackUse(command_list, feature.slots[pass].handle);
    NVSDK_NGX_Result nr_result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    {
      DirectCallScope ngx_direct_call;
      try {
        nr_result = feature.slots[pass].via_core
            ? core_evaluate_feature(
                  command_list,
                  feature.slots[pass].handle,
                  feature.slots[pass].parameters,
                  nullptr)
            : direct_api.evaluate(
                  command_list,
                  feature.slots[pass].handle,
                  feature.slots[pass].parameters,
                  nullptr);
      } catch (...) {
        // The shared failure path below retains the game DLSS output,
        // releases the handle, and arms the correct retry policy; the
        // exception only substitutes the result code.
        Log(reshade::log::level::error, "feature 18 evaluate raised an exception");
        nr_result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
      }
    }
    last_result = static_cast<uint32_t>(nr_result);
    if (NVSDK_NGX_FAILED(nr_result)) {
      stack_failure = nr_result;
      failed_pass = pass;
      break;
    }
    // Decode pass: blend the neural result over this pass's reference image
    // and upscale from the NR working resolution to the output resolution.
    // v6 HDR resolves in LINEAR source units onto the work ping-pong
    // surface (no transfer function until the single commit below).
    UavBarrier(command_list, res.nr_output);
    if (timers_active) {
      gpu_timers::Mark(command_list, gpu_timers::PassEvalEnd(pass));
    }
    if (capture_this_eval) {
      // Diagnostics: the RAW neural output, before any transfer - evidence
      // of what the model actually returns for the conditioned proxy.  With
      // a multi-pass stack each pass records its own plane.
      Transition(
          command_list, res.nr_output,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_COPY_SOURCE);
      screenshot::RecordStackPlaneCopy(
          device, command_list, res.nr_output,
          res.input_width, res.input_height,
          "neural", pass, requested_stack);
      Transition(
          command_list, res.nr_output,
          D3D12_RESOURCE_STATE_COPY_SOURCE,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    } else {
      Transition(
          command_list, res.nr_output,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    res.nr_output_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // The look stage reshapes the model's edit (or measures it) before the
    // resolve, which then reads the route it returns.
    const LookRoute route =
        RunLookStage(command_list, device, res, feature.slots[pass], pass, look_chain, frame);
    Transition(
        command_list, write_surface,
        *write_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    *write_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (res.hdr_mode != 0) {
      BindCodecV6(
          command_list,
          res,
          codec_pipeline.resolve_v6,
          route.srv_set,
          write_uav_index,
          width,
          height,
          width,
          height,
          0,
          0,
          route.proxy_width,
          route.proxy_height,
          route.neural_width,
          route.neural_height,
          frame_divisor,
          v6_pedestal_cap,
          pass_transfer,
          pass_color,
          v6_encoding,
          v6_dark_gate,
          v6_chroma_clamp);
    } else {
      BindCodec(
          command_list,
          res,
          codec_pipeline.decode,
          route.srv_set,
          write_uav_index,
          width,
          height,
          width,
          height,
          0,
          0,
          route.proxy_width,
          route.proxy_height,
          route.neural_width,
          route.neural_height,
          frame_paper_white,
          frame_diffuse_white,
          pass_transfer,
          pass_color);
    }
    command_list->Dispatch((width + 15) / 16, (height + 15) / 16, 1);
    UavBarrier(command_list, write_surface);
    RunBlackLevelRestore(
        command_list,
        res,
        srv_table_start,
        write_uav_index,
        width,
        height,
        frame_paper_white,
        frame_diffuse_white,
        pass_transfer,
        pass_color,
        route.shaped);
    if (timers_active) {
      gpu_timers::Mark(command_list, gpu_timers::PassResolveEnd(pass));
    }
    // Intermediate results stay in the ping-pong surfaces for the next pass
    // (resting in UAV, their tracked state); the write-back to the game
    // output happens once, after the loop, on the last executed pass.
    model_evaluated = true;
    executed = static_cast<int32_t>(pass);
    final_pass_transfer = pass_transfer;
    final_pass_color = pass_color;
    final_pass_shaped = route.shaped;
  }

  // Write back the LAST EXECUTED pass's result: the full stack on a normal
  // frame, or the already-matured partial stack when the loop above broke on
  // a freshly created slot (frame-ahead maturation).  With executed == -1
  // (pass 0 itself was just created) nothing ran and NO write-back happens:
  // the game output still holds the game's own DLSS result, which is the
  // correct flicker-free behavior for this frame.  A failed pass also keeps
  // the write-back off - the failure path below retains the game output.
  if (executed >= 0 && !NVSDK_NGX_FAILED(stack_failure)) {
    if (res.hdr_mode != 0) {
      // v6 single encode-back: estimate the model's dark pedestal per block
      // against the untouched work0, then commit - bounded, dark-gated
      // pedestal removal and the one transfer into the destination encoding
      // - into `decoded`, which is the surface copied over the game output.
      ID3D12Resource* const work_final = work_out_surfaces[executed & 1];
      D3D12_RESOURCE_STATES* const work_final_state =
          work_out_states[executed & 1];
      const uint32_t commit_set =
          (executed & 1) != 0 ? kDescriptorCommitBSet : kDescriptorCommitASet;
      Transition(
          command_list, work_final,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      *work_final_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      if (capture_this_eval) {
        // Diagnostics: copy the resolved linear surface the commit consumes
        // (the executed pass's output under a multi-pass stack).
        Transition(
            command_list, work_final,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        screenshot::RecordStackPlaneCopy(
            device, command_list, work_final,
            res.width, res.height,
            "work_final", executed, requested_stack);
        Transition(
            command_list, work_final,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      }
      if (timers_active) {
        gpu_timers::Mark(command_list, gpu_timers::kSlotCommitStart);
      }
      // A single shaped pass: pass 0's codec set [work0, P, N, scale] gives
      // the reduce NR's own lift.  A stack keeps the classic measurement -
      // its earlier passes are already shaped into the later references.
      // A frame without the pedestal removal skips the reduce: the commit's
      // PedestalGate 0 multiplies the block means it would write by zero.
      const bool measure_unshaped = executed == 0 && final_pass_shaped
          && codec_pipeline.pedestal_reduce_unshaped != nullptr;
      if (FramePedestalRemoval(res)) {
        BindCodecV6(
            command_list,
            res,
            measure_unshaped ? codec_pipeline.pedestal_reduce_unshaped
                             : codec_pipeline.pedestal_reduce,
            measure_unshaped ? 0u : commit_set,
            kDescriptorBlockMeanUav,
            width,
            height,
            width,
            height,
            0,
            0,
            res.input_width,
            res.input_height,
            res.input_width,
            res.input_height,
            frame_divisor,
            v6_pedestal_cap,
            final_pass_transfer,
            final_pass_color,
            v6_encoding,
            v6_dark_gate,
            v6_chroma_clamp);
        command_list->Dispatch((width + 31) / 32, (height + 31) / 32, 1);
        UavBarrier(command_list, res.block_mean);
      }
      Transition(
          command_list, res.decoded,
          res.decoded_state,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      res.decoded_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      BindCodecV6(
          command_list,
          res,
          frame.feed != FeedSource::kV1 ? codec_pipeline.commit_exposure
                                        : codec_pipeline.commit,
          commit_set,
          kDescriptorDecodedUav,
          width,
          height,
          width,
          height,
          0,
          0,
          width,
          height,
          width,
          height,
          frame_divisor,
          v6_pedestal_cap,
          final_pass_transfer,
          final_pass_color,
          v6_encoding,
          v6_dark_gate,
          0.f);
      command_list->Dispatch((width + 15) / 16, (height + 15) / 16, 1);
      UavBarrier(command_list, res.decoded);
      if (timers_active) {
        gpu_timers::Mark(command_list, gpu_timers::kSlotCommitEnd);
      }
      if (norm_trace_enabled.load(std::memory_order_relaxed)) {
        norm_trace::Record(
            command_list, device, res.norm_scale, res.norm_scale_state,
            res.block_mean, width, height,
            {present_generation, SteadyNowNs(), res.id, frame_snap,
             static_cast<uint32_t>(frame.feed)});
      }
      Transition(
          command_list, res.decoded,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_COPY_SOURCE);
      if (capture_this_eval) {
        // The commit has completed and decoded is COPY_SOURCE until it is
        // written back.  Both copies land on this same command list, so the
        // pair always describes this full NR stack.
        screenshot::RecordPostCopy(command_list, res.decoded);
      }
      Transition(
          command_list, output,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_COPY_DEST,
          0u);
      CopyMip0(command_list, output, res.decoded);
      Transition(
          command_list, output,
          D3D12_RESOURCE_STATE_COPY_DEST,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          0u);
      if (timers_active) {
        gpu_timers::Mark(command_list, gpu_timers::kSlotCopyBackEnd);
      }
      Transition(
          command_list, res.decoded,
          D3D12_RESOURCE_STATE_COPY_SOURCE,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      res.decoded_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      // Park the read work surface back to its UAV resting state; work0
      // stays SRV until the next frame's linearize transition reclaims it.
      Transition(
          command_list, work_final,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      *work_final_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    } else {
      ID3D12Resource* const final_surface = stack_surfaces[executed & 1];
      D3D12_RESOURCE_STATES* const final_state = stack_states[executed & 1];
      Transition(
          command_list, final_surface,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_COPY_SOURCE);
      if (capture_this_eval) {
        // Decode has completed and the final surface is COPY_SOURCE until it
        // is written back.  Both copies land on this same command list, so
        // the pair always describes this full NR stack.
        screenshot::RecordPostCopy(command_list, final_surface);
      }
      Transition(
          command_list, output,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_COPY_DEST,
          0u);
      CopyMip0(command_list, output, final_surface);
      Transition(
          command_list, output,
          D3D12_RESOURCE_STATE_COPY_DEST,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          0u);
      if (timers_active) {
        gpu_timers::Mark(command_list, gpu_timers::kSlotCopyBackEnd);
      }
      Transition(
          command_list, final_surface,
          D3D12_RESOURCE_STATE_COPY_SOURCE,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      *final_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
  }

  if (timers_active) {
    gpu_timers::End(command_list);
  }
  device->Release();
  if (NVSDK_NGX_FAILED(stack_failure)) {
    // Park the tracked states: the proxy is post-encode (SRV-readable) and
    // returns to its UAV resting state; nr_output is already parked; the
    // pass's reference surface stays SRV-readable with its tracked state, so
    // every next-frame transition remains valid.  The game DLSS output was
    // never modified - the image is unchanged.
    Transition(
        command_list, res.proxy,
        res.proxy_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    res.proxy_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // Tracked transition (no-op when the evaluate failed after the pre-
    // evaluate UAV transition already landed): nr_output can also break here
    // from a slot-create failure while still in its SRV resting state.
    Transition(
        command_list, res.nr_output,
        res.nr_output_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    res.nr_output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    NrFeatureSlot& failed_slot = feature.slots[failed_pass];
    failed_slot.failed = true;
    failed_slot.failed_ns = SteadyNowNs();
    ReleaseNrSlot(feature, failed_pass, true);
    // The chain broke mid-stack: passes after the failure skipped an input
    // frame, so their temporal history is discontinuous - mark them to
    // reset on the next attempt (the failed slot resets itself via
    // pending_reset when recreated).  Passes before the failure evaluated
    // normally and keep their history.
    for (uint32_t later = failed_pass + 1; later < kMaxNrPasses; ++later) {
      feature.slots[later].pending_reset = true;
    }
    std::ostringstream message;
    message << "feature 18 evaluate failed with 0x" << std::hex
            << static_cast<uint32_t>(stack_failure)
            << " (stack pass " << (failed_pass + 1) << '/'
            << requested_stack << ")"
            << "; the game DLSS output was retained (the image is unchanged)"
            << "; NR retries on the next frame, then after 1 s, doubling up"
               " to 30 s while it keeps failing ('Reset NR feature and clear"
               " failure latch' in the overlay retries at once)";
    Log(reshade::log::level::error, message.str());
    // Do not abort the capture here: a failed evaluate (e.g. an intermediate
    // reflection denoise) should let the next evaluate in the same frame
    // re-record the pair so the last one still wins. The frame's present
    // disarms and, if nothing completed, drops the capture.
    return false;
  }

  // The output was claimed for this present either way (a mirror evaluate
  // must not re-enter), but only a frame NR actually ran on counts as a
  // successful NR evaluation - a fresh-slot maturation frame passes the game
  // image through untouched.
  RecordOutputPass(output, handle);
  if (executed < 0) {
    // Fresh-slot maturation: NR did not run, the image passed through.
    CountNrDecline(NrDeclineReason::kSlotWarmupMaturation);
    return true;
  }
  ++captured_guides;
  if (!model_evaluated) {
    ++bypassed_evaluations;
    CountNrDecline(NrDeclineReason::kSlotWarmupMaturation);
    return true;
  }
  for (auto& slot : feature.slots) slot.fail_count = 0;
  const uint64_t count = ++successful_evaluations;
  streamline_escalation_deficits.store(0, std::memory_order_relaxed);
  last_input_width = nr_input_width;
  last_input_height = nr_input_height;
  last_output_width = width;
  last_output_height = height;
  if (count == 1 || count == 60 || count == 600) {
    std::ostringstream message;
    message << "inline feature 18 evaluation succeeded (count=" << count
            << ", NR input " << nr_input_width << 'x' << nr_input_height
            << " (guides " << current.input_width << 'x' << current.input_height
            << "), output " << width << 'x' << height
            << ", " << requested_stack << " stack pass(es) [native])";
    Log(reshade::log::level::info, message.str());
  }
  return true;
}

// ---------------------------------------------------------------------------
// Pre-SR insertion point.
//
// The "after" path above enhances the game's finished DLSS output: excellent
// when color and guides share a resolution, but out of contract for titles
// whose DLSS runs at a lower render resolution than its guides-less output
// (KCD2: 3840x2160 post-upscale color against 2560x1440 internal depth/motion).
// The pre-SR path instead intercepts the game's evaluate BEFORE it runs:
//
//   1. the game's NGX Color (render-res, e.g. 2560x1440) is encoded into the
//      proxy exactly like the after path,
//   2. feature 18 runs native (render-res in, render-res out) against the
//      game's own depth/motion - which now match the color resolution exactly,
//   3. the decode blends the neural result back over the game color,
//   4. the result is copied into a persistent stand-in texture and the NGX
//      Color parameter is pointed at it for the duration of the game's
//      evaluate, so the game's own DLSS upscales an already-enhanced image,
//   5. the parameter is restored immediately after the evaluate returns.
//
// The game's own buffers are never written; NGX documents that input resources
// must be shader-readable at evaluate (its own network reads them as SRVs), so
// binding the game color as an SRV needs no state transition.
// ---------------------------------------------------------------------------

inline std::atomic_uint64_t successful_pre_sr_evaluations = 0;

// The pre-SR stand-in's state while NGX reads it.
inline constexpr D3D12_RESOURCE_STATES kPreSrReadable =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Original game color pointer carried across the game's evaluate; restored by
// RestorePreSrColor immediately after real() returns.
struct PreSrSwap {
  ID3D12Resource* color = nullptr;
  // NR's result, the stand-in `color` was swapped for (kPreSrReadable).  The
  // D3D11 bridge copies it into its color twin: a D3D11 block cannot be
  // pointed at a D3D12 resource.
  ID3D12Resource* result = nullptr;
};

inline void RestorePreSrColor(
    const NVSDK_NGX_Parameter* game_parameters,
    PreSrSwap& swap) {
  if (swap.color == nullptr) return;
  NVSDK_NGX_Parameter_SetVoidPointer(
      const_cast<NVSDK_NGX_Parameter*>(game_parameters),
      NVSDK_NGX_Parameter_Color,
      swap.color);
  swap.color = nullptr;
}

inline bool ProcessInlinePreSR(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* game_parameters,
    PreSrSwap& swap,
    // Set to true once any GPU command has been recorded onto the list (the
    // wrapper's restore envelope must apply even on failure exits after
    // recording started - the bool return alone conflates "injected" with
    // "declined before recording").
    bool* commands_recorded = nullptr) {
  if (!enabled.load() || !nr_before_upscale.load() || command_list == nullptr
      || handle == nullptr || game_parameters == nullptr
      || !IsSupportedInjectionCommandList(command_list)) {
    return false;
  }
  InjectedCommandScope injected_command_scope;
  if (!IsDlssEvaluation(game_parameters)) return false;

  auto* color = GetD3D12Resource(game_parameters, NVSDK_NGX_Parameter_Color);
  auto* motion = GetD3D12Resource(game_parameters, NVSDK_NGX_Parameter_MotionVectors);
  auto* depth = GetD3D12Resource(game_parameters, NVSDK_NGX_Parameter_Depth);
  auto* exposure =
      GetD3D12Resource(game_parameters, NVSDK_NGX_Parameter_ExposureTexture);
  if (color == nullptr || motion == nullptr || depth == nullptr) return false;

  // One NR pass per color surface per present per source handle
  // (mirrors the after path's admission; pre-SR never mixes with the
  // Streamline synthetic path, so the mirror rule reduces to same-handle).
  if (!AdmitOutputPass(color, handle)) return false;

  const D3D12_RESOURCE_DESC color_desc = color->GetDesc();
  const D3D12_RESOURCE_DESC motion_desc = motion->GetDesc();
  const D3D12_RESOURCE_DESC depth_desc = depth->GetDesc();
  if (color_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
      || color_desc.MipLevels != 1
      || color_desc.DepthOrArraySize != 1
      || color_desc.SampleDesc.Count != 1) {
    CountNrDecline(NrDeclineReason::kPreSrGeometry);
    if (!logged_pre_sr_geometry.exchange(true)) {
      Log(reshade::log::level::warning,
          "pre-SR NR declined: color geometry is unsupported (dim="
              + std::to_string(static_cast<unsigned>(color_desc.Dimension))
              + " mips=" + std::to_string(color_desc.MipLevels)
              + " array=" + std::to_string(color_desc.DepthOrArraySize)
              + " samples=" + std::to_string(color_desc.SampleDesc.Count)
              + "); requires a single-mip single-sample 2D texture");
    }
    return false;
  }
  const uint32_t color_w = static_cast<uint32_t>(color_desc.Width);
  const uint32_t color_h = color_desc.Height;
  // Region acceptance (v6.2): engines allocate guides at source OR target
  // region geometry (DirectSR semantics: motion vectors may match the source
  // or the target region). Source semantics: the guide at or below the color
  // extent, within 1/8 of it - the captured field case is color/depth
  // 3416x964 with motion 3416x961 (3 rows of allocation padding). Target
  // semantics: a guide larger than color is accepted when it sits within 1/8
  // of the declared output region - the game evaluates its own SR feature on
  // the same guides with the same declared MV scale, which this path forwards
  // unchanged, so the pair stays consistent (the Silent Hill 2 shape: color
  // 2296x964, motion 3440x1440 = the DLSS output). Without a declared output
  // there is nothing to anchor a larger guide to, so that shape stays a
  // counted decline.
  const uint32_t declared_out_w =
      GetUInt(game_parameters, NVSDK_NGX_Parameter_OutWidth);
  const uint32_t declared_out_h =
      GetUInt(game_parameters, NVSDK_NGX_Parameter_OutHeight);
  const auto guide_in_region =
      [color_w, color_h, declared_out_w, declared_out_h](
          uint32_t guide_w, uint32_t guide_h) {
        const bool source_region =
            guide_w <= color_w && guide_w * 8u >= color_w
            && guide_h <= color_h && guide_h * 8u >= color_h;
        const bool target_region =
            declared_out_w != 0 && declared_out_h != 0
            && guide_w * 8u >= declared_out_w * 7u
            && guide_w * 8u <= declared_out_w * 9u
            && guide_h * 8u >= declared_out_h * 7u
            && guide_h * 8u <= declared_out_h * 9u;
        return source_region || target_region;
      };
  const bool guides_in_region =
      guide_in_region(motion_desc.Width, motion_desc.Height)
      && guide_in_region(depth_desc.Width, depth_desc.Height);
  if (!guides_in_region) {
    CountNrDecline(NrDeclineReason::kPreSrGeometry);
    static std::atomic<bool> logged_guide_mismatch{false};
    if (!logged_guide_mismatch.exchange(true)) {
      Log(
          reshade::log::level::warning,
          "pre-SR NR declined: guides match neither the color region nor the"
          " declared output region (color "
              + std::to_string(color_w) + "x" + std::to_string(color_h)
              + ", output " + std::to_string(declared_out_w) + "x"
              + std::to_string(declared_out_h)
              + ", motion " + std::to_string(motion_desc.Width) + "x"
              + std::to_string(motion_desc.Height) + ", depth "
              + std::to_string(depth_desc.Width) + "x"
              + std::to_string(depth_desc.Height)
              + "); the frame runs without NR");
    }
    return false;
  }
  const uint32_t width = static_cast<uint32_t>(std::min<UINT64>(
      std::min<UINT64>(motion_desc.Width, depth_desc.Width), color_w));
  const uint32_t height = static_cast<uint32_t>(std::min<UINT64>(
      std::min<UINT64>(motion_desc.Height, depth_desc.Height), color_h));
  if (width != color_w || height != color_h) {
    static std::atomic<bool> logged_region_clamp{false};
    if (!logged_region_clamp.exchange(true)) {
      Log(
          reshade::log::level::info,
          "pre-SR NR region clamped to the common guide region "
              + std::to_string(width) + "x" + std::to_string(height)
              + " (color " + std::to_string(color_w) + "x"
              + std::to_string(color_h) + ")");
    }
  }

  // Initialize the direct runtime before the feature lookup: a device change
  // inside EnsureDirectRuntime releases every feature state, so the
  // FeatureState& bound below must not exist yet (see FindOrRegisterFeature).
  if (!EnsureDirectRuntime(command_list)) {
    // Named, not silent: with no nvngx_dlssnr.dll on the machine this is
    // where EVERY evaluate ends, and until v6.8.0-alpha23 it ended without
    // counting anything.  The verdict said UNAVAILABLE and was right; the
    // funnel under it read `unaccounted=239` out of 240 (measured by the
    // `no_nr_runtime` lane on its first run), which is T-SILENT's own
    // definition of a leak.  Only the two evaluate-path terminals count:
    // EnsureNrFeature reaches its copy of this check inside an injection
    // that already passed one, so counting there would double-count.
    CountNrDecline(NrDeclineReason::kNrRuntimeUnavailable);
    return false;
  }
  ID3D12Device* device = nullptr;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    last_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
    return false;
  }
  auto entry = FindOrRegisterFeature(handle, game_parameters, "pre-SR ");
  FeatureState& feature = entry->second;
  FeatureState current = DeriveFeatureState(game_parameters);
  ObserveGameFrame(current);
  if (current.input_width == 0) {
    current.input_width = feature.create_input_width != 0
        ? feature.create_input_width
        : feature.input_width;
  }
  if (current.input_height == 0) {
    current.input_height = feature.create_input_height != 0
        ? feature.create_input_height
        : feature.input_height;
  }
  if (current.motion_x == 0 && current.motion_y == 0) {
    current.motion_x = feature.motion_x;
    current.motion_y = feature.motion_y;
  }
  if (current.depth_x == 0 && current.depth_y == 0) {
    current.depth_x = feature.depth_x;
    current.depth_y = feature.depth_y;
  }
  // Same presence-aware default as the after path: only after the final guide
  // dims are known (the synthetic pre-SR block always sets MV scales, but a
  // native block that reached this path without them still defaults sanely).
  if (!current.has_motion_scale_x) {
    current.motion_scale_x = static_cast<float>(current.input_width);
  }
  if (!current.has_motion_scale_y) {
    current.motion_scale_y = static_cast<float>(current.input_height);
  }
  if (!SupportsCodecFormat(
          device, CodecViewFormat(ConcreteResourceFormat(color_desc.Format)))) {
    static std::atomic<bool> logged_color_format{false};
    if (!logged_color_format.exchange(true)) {
      Log(
          reshade::log::level::warning,
          "pre-SR NR declined: the NGX Color format is not shader-readable"
          " (requires sampling + typed UAV support)");
    }
    device->Release();
    return false;
  }

  // Evidence probe: does this title anchor its color buffer through NGX?  A
  // game that applies exposure BEFORE DLSS passes DLSS_Pre_Exposure != 1, and
  // the buffer statistics then wander with its eye-adaptation state - which a
  // fixed paper-white calibration cannot track.  Logged on first sight and
  // whenever the anchor moves by more than 25% - rate-limited to once per
  // ~1800 presents, or a title that pumps a wandering anchor per frame floods
  // the log.
  const float pre_exposure = GetFloat(
      game_parameters, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.f);
  const float exposure_scale = GetFloat(
      game_parameters, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.f);
  {
    static std::atomic<float> logged_pre_exposure{-1.f};
    static std::atomic<uint64_t> logged_pre_exposure_generation{0};
    const float previous = logged_pre_exposure.load();
    const bool moved = previous >= 0.f
        && std::fabs(pre_exposure / std::max(previous, 1e-6f) - 1.f) > 0.25f;
    const bool interval_elapsed = present_generation
        >= logged_pre_exposure_generation.load() + 1800ull;
    if (previous < 0.f || (moved && interval_elapsed)) {
      logged_pre_exposure = pre_exposure;
      logged_pre_exposure_generation = present_generation;
      std::ostringstream message;
      message << "game NGX exposure anchor: DLSS_Pre_Exposure=" << pre_exposure
              << ", DLSS_Exposure_Scale=" << exposure_scale
              << (pre_exposure == 1.f && exposure_scale == 1.f
                      ? " (no exposure applied before DLSS)"
                      : " (exposure applied before DLSS - buffer statistics move"
                        " with adaptation)");
      Log(reshade::log::level::info, message.str());
    }
  }

  int32_t evaluation_flags = 0;
  if (NVSDK_NGX_SUCCEED(game_parameters->Get(
          NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
          &evaluation_flags))) {
    feature.create_flags = static_cast<uint32_t>(evaluation_flags);
  }
  // The codec HDR mode is derived from the COLOR surface - it is what gets
  // encoded, not the (here never-written) output copy.  Computed before the
  // adapt/recreate checks so the NR feature's create flags track it.
  const DXGI_FORMAT color_format = ConcreteResourceFormat(color_desc.Format);
  uint8_t hdr_mode = InferHdrMode(color_format);
  if ((feature.create_flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0) {
    if (hdr_mode == 0) hdr_mode = 1;
  } else if (color_format == DXGI_FORMAT_R10G10B10A2_UNORM) {
    // No HDR flag: a 10-bit R10G10B10A2 color is SDR (e.g. Control), not PQ.
    hdr_mode = 0;
  }
  const uint32_t requested_stack = std::clamp(stack_passes.load(), 1u, kMaxNrPasses);
  // NR working resolution (v5.1): the same controls as the after path, with
  // the NGX color surface as the reference - pre-SR has no separate output
  // resolution, the game's own DLSS upscales after us.  Scaled mode runs the
  // feature below the render resolution against the game's full-res
  // depth/motion guides with the game's MVecScale unchanged: the same
  // guide/color arrangement, and the same F8 rule, as the after path.
  uint32_t nr_input_width = width;
  uint32_t nr_input_height = height;
  ComputeNrWorkingResolution(
      width, height, current.input_width, current.input_height,
      nr_input_width, nr_input_height);
  const bool dims_committed =
      CommitNrResolution(feature, nr_input_width, nr_input_height);
  LogNrWorkingResolutionReduced(width, height, nr_input_width, nr_input_height);
  const bool dimensions_changed = dims_committed;
  const bool configuration_changed = feature.slots[0].handle != nullptr
      && feature.slots[0].configuration_generation
          != configuration_generation.load();
  const bool hdr_changed = feature.slots[0].handle != nullptr
      && feature.slots[0].nr_hdr_mode != hdr_mode;
  if (dimensions_changed || configuration_changed || hdr_changed) {
    // Contract-level change: every stack pass's feature must be recreated,
    // and slots beyond the current stack depth are freed (via the overlay's
    // RecreateFeatures on pass-count changes).  A null slot-0 handle after a
    // failure must NOT trigger this: the failure latch has to survive so the
    // bounded retry interval holds.  As on the post-SR path, the recreated
    // slots reset themselves via pending_reset without advancing the global
    // reset epoch.
    ReleaseAllNrSlots(feature, true);
    for (auto& slot : feature.slots) {
      slot.failed = false;
      slot.fail_count = 0;
    }
  }
  feature.input_width = nr_input_width;
  feature.input_height = nr_input_height;
  feature.foveation = {};
  feature.output_width = nr_input_width;
  feature.output_height = nr_input_height;
  feature.motion_x = current.motion_x;
  feature.motion_y = current.motion_y;
  feature.depth_x = current.depth_x;
  feature.depth_y = current.depth_y;
  feature.motion_scale_x = current.motion_scale_x;
  feature.motion_scale_y = current.motion_scale_y;
  feature.perf_quality = current.perf_quality;
  feature.frame_reset = current.frame_reset;
  for (uint32_t gate = 0; gate < requested_stack; ++gate) {
    NrFeatureSlot& slot = feature.slots[gate];
    if (slot.failed) {
      // Bounded automatic retry, same policy as the after path.
      if (!FeatureFailureExpired(slot)) {
        CountNrDecline(NrDeclineReason::kFailureBackoff);
        device->Release();
        return false;
      }
      slot.failed = false;
      ++slot.fail_count;
    }
  }

  const bool codec_ok = hdr_mode != 0 ? EnsureV6CodecPipeline(device)
                                      : EnsureLegacyCodecPipeline(device);
  // The NR look stage (look_stage.hpp): this chain's look values and the
  // surfaces they need, fixed before the workset is sized.
  LookChain look_chain{
      .settings = LookSettingsSnapshot(),
      .motion_scale = {current.motion_scale_x, current.motion_scale_y},
  };
  if (codec_ok) {
    look_chain.wanted = LookSurfacesWanted(
        device, look_chain.settings, nr_input_width, nr_input_height, width, height);
  }
  FinalResources* workset = codec_ok
      ? EnsureWorkset(
              device,
              handle,
              color,
              nr_input_width,
              nr_input_height,
              width,
              height,
              color_desc.Format,
              hdr_mode,
              requested_stack,
              look_chain.wanted,
              true)
      : nullptr;
  if (!workset) {
    if (!codec_ok) {
      Log(reshade::log::level::error,
          "NR pre-SR path failed: embedded codec pipeline creation failed");
    }
    // EnsureWorkset's failure sites log their own reason and descriptor.
    CountNrDecline(NrDeclineReason::kWorksetSetupFailed);
    feature.slots[0].failed = true;
    feature.slots[0].failed_ns = SteadyNowNs();
    device->Release();
    return false;
  }
  FinalResources& res = *workset;
  // Pre-SR keeps the uniform working grid (foveation is after-upscale only).
  res.foveation = {};
  res.fov_guides_valid = false;
  if (res.pre_sr_source == nullptr) {
    color->AddRef();
    res.pre_sr_source = color;
  }
  // Exact submission-use proof (issue 01) - see the post-SR path.
  TrackWorksetRecording(command_list, res);
  // Stage A: hand the raw exposure evidence to the workset so the present-
  // side normalization lock reads the same contract this evaluate saw.
  // One gain snapshot for this frame's encode, decode, and black restore.
  // SDR worksets run the legacy codec family (Classic: the paper-white
  // scale; anchored family: the fixed anchor knee over 60); HDR worksets
  // run v6 and snapshot the mode-policy divisor once instead.
  const float frame_paper_white =
      codec_mode.load() == 0u
          ? std::max(0.0001f, paper_white_scale.load())
          : std::max(0.0002f, proxy_anchor_nits.load() / 60.f);
  // PQ bridge anchor snapshot (nits): consumed only by the hdr_mode == 2
  // legacy codec branches; see NRDiffuseWhiteNits.
  const float frame_diffuse_white = diffuse_white_nits.load();
  // v6 constant snapshots (HDR worksets); see ProcessInline.
  const uint32_t v6_encoding =
      res.units.encoding == codec::Encoding::Pq ? 2u : 1u;
  const float v6_dark_gate =
      res.units.absolute && res.units.unit_nits > 0.f
          ? 1.f / res.units.unit_nits
          : 0.005f;
  const float v6_pedestal_cap = codec::BlackPedestalCap(res.units);
  const float frame_divisor =
      res.hdr_mode != 0 ? FrameCodecDivisor(res) : 1.f;
  // v9 Phase 6 transfer snapshots and catastrophic gate (see ProcessInline).
  const float v6_chroma_clamp = chroma_clamp_stops.load(std::memory_order_relaxed);
  const FeedSource feed = FrameFeed(&feature, current, res, exposure);
  LogFrameContract(game_parameters, current, res, frame_divisor, feed, "preSR");
  // Exact zero-transfer bypass (handoff rule 3): an HDR frame with every
  // pass at zero strength records nothing at all - the game's evaluate runs
  // on its own color, bit-identical to no-addon.
  transfer_strength_inert.store(!FramePedestalRemoval(res), std::memory_order_relaxed);
  if (res.hdr_mode != 0 && !ChainActive(res, requested_stack)) {
    // Explicit zero strength is the only v6 image-path bypass; the game's
    // own evaluate runs on its own color below.
    ++bypassed_evaluations;
    CountNrDecline(NrDeclineReason::kZeroStrengthPassthrough);
    RecordOutputPass(color, handle);
    device->Release();
    return false;
  }
  if (commands_recorded != nullptr) *commands_recorded = true;

  EvaluationContract frame;
  frame.source_handle = handle;
  frame.motion = motion;
  frame.depth = depth;
  // The guides stay at the game's render resolution even when the NR
  // working grid is reduced: SetEvaluationParameters describes the color
  // subrect at the working dims and the guide subrects at the full surface,
  // and the game's MVecScale is corrected by the applied fraction (same
  // guide/color arrangement as the after path).
  frame.input_width = width;
  frame.input_height = height;
  frame.output_width = width;
  frame.output_height = height;
  frame.motion_x = current.motion_x;
  frame.motion_y = current.motion_y;
  frame.depth_x = current.depth_x;
  frame.depth_y = current.depth_y;
  frame.create_flags = feature.create_flags;
  frame.motion_scale_x = current.motion_scale_x;
  frame.motion_scale_y = current.motion_scale_y;
  frame.jitter_x = current.jitter_x;
  frame.jitter_y = current.jitter_y;
  frame.has_jitter = current.has_jitter;
  frame.frame_reset = current.frame_reset;
  frame.exposure = exposure;
  frame.pre_exposure = current.pre_exposure > 0.f ? current.pre_exposure : 1.f;
  frame.exposure_scale =
      current.exposure.scale > 0.f ? current.exposure.scale : 1.f;
  frame.feed = feed;

  // Stacking ping-pong (see ProcessInline): pass 0 runs NR on the game's
  // render-res color; pass k>=2 re-encodes the previous decoded result.
  ID3D12Resource* const stack_surfaces[2] = {res.decoded, res.decoded_alt};
  D3D12_RESOURCE_STATES* const stack_states[2] = {
      &res.decoded_state, &res.decoded_alt_state};
  // v6 HDR ping-pong (see ProcessInline): pass k writes
  // work_out_surfaces[k & 1], matching the fixed per-pass SRV sets.
  ID3D12Resource* const work_out_surfaces[2] = {res.work_a, res.work_b};
  D3D12_RESOURCE_STATES* const work_out_states[2] = {
      &res.work_a_state, &res.work_b_state};
  uint32_t frame_snap = 0u;

  // Per-stage GPU timestamps (NRGpuTimers): the pre-SR chain records nothing
  // before this point (no copy-in; the bypass return above never started a
  // segment), so this is the frame-start marker for both HDR and SDR.
  const bool timers_active = gpu_timers_enabled.load(std::memory_order_relaxed);
  if (timers_active) {
    gpu_timers::Begin(command_list, device, "preSR", width, height);
  }

  if (res.hdr_mode != 0) {
    // v6 linearize: the game's own color (SRV by the NGX contract, same
    // rebind discipline as the SDR pass-0 reference) becomes the whole-frame
    // linear working copy work0 - the metering input, the pedestal
    // reference, and pass 0's encode source.
    SetSourceView(device, res, color, kDescriptorLinearizeSet);
    Transition(
        command_list,
        res.work0,
        res.work0_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    res.work0_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    BindCodecV6(
        command_list,
        res,
        codec_pipeline.linearize,
        kDescriptorLinearizeSet,
        kDescriptorWork0Uav,
        width,
        height,
        width,
        height,
        0,
        0,
        width,
        height,
        width,
        height,
        frame_divisor,
        v6_pedestal_cap,
        0.f,
        0.f,
        v6_encoding,
        v6_dark_gate,
        0.f);
    command_list->Dispatch((width + 15) / 16, (height + 15) / 16, 1);
    UavBarrier(command_list, res.work0);
    Transition(
        command_list,
        res.work0,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    res.work0_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // Derive the relative-HDR divisor from this exact linearized frame before
    // it enters the model.
    frame_snap = PrepareFrameScale(command_list, device, res, frame);
    if (timers_active) {
      gpu_timers::Mark(command_list, gpu_timers::kSlotLinearizeEnd);
    }
  }

  NVSDK_NGX_Result stack_failure = NVSDK_NGX_Result_Success;
  uint32_t failed_pass = 0;
  // Last pass that evaluated AND decoded this frame (-1 = none).  Drives the
  // post-loop stand-in copy: a fresh slot is created but not evaluated
  // (frame-ahead maturation), so the loop can end early and the copy must
  // target the partial stack's last executed pass instead of requested_stack.
  // Resolved once per chain (v5.3): every pass of this chain evaluates
  // against the same reset epoch, so a mid-chain advance cannot split the
  // chain's resets across two epochs.
  const uint64_t chain_reset_epoch =
      history_reset_epoch.load(std::memory_order_relaxed);
  int32_t executed = -1;
  // v6 audit issue 23: see ProcessInline - a rule-3 bypass frame sets
  // `executed` via copies only and must not count as a successful NR frame.
  bool model_evaluated = false;
  // Strengths of the last EXECUTED pass, consumed by the v6 pedestal and
  // commit dispatches after the loop.
  float final_pass_transfer = 0.f;
  float final_pass_color = 0.f;
  bool final_pass_shaped = false;
  for (uint32_t pass = 0; pass < requested_stack; ++pass) {
    // v6 HDR swaps the linear work surfaces in (work0 for pass 0, the
    // previous pass's work output after - the fixed set fill's parity);
    // SDR keeps the game color / decoded ping-pong arrangement.
    ID3D12Resource* const decode_ref =
        res.hdr_mode != 0
            ? (pass == 0 ? res.work0
                         : ((pass & 1) != 0 ? res.work_a : res.work_b))
            : (pass == 0 ? color : stack_surfaces[(pass - 1) & 1]);
    // Pass 0 reads the game's color on SDR, which pre-SR never state-tracks
    // (NGX reads it as an SRV by contract); every other surface is tracked.
    D3D12_RESOURCE_STATES* const ref_state =
        res.hdr_mode != 0
            ? (pass == 0 ? &res.work0_state
                         : ((pass & 1) != 0 ? &res.work_a_state
                                            : &res.work_b_state))
            : (pass == 0 ? nullptr : stack_states[(pass - 1) & 1]);
    ID3D12Resource* const write_surface =
        res.hdr_mode != 0 ? work_out_surfaces[pass & 1]
                          : stack_surfaces[pass & 1];
    D3D12_RESOURCE_STATES* const write_state =
        res.hdr_mode != 0 ? work_out_states[pass & 1]
                          : stack_states[pass & 1];
    const uint32_t write_uav_index =
        res.hdr_mode != 0
            ? ((pass & 1) != 0 ? kDescriptorWorkBUav : kDescriptorWorkAUav)
            : ((pass & 1) != 0 ? kDescriptorDecodedAltUav
                               : kDescriptorDecodedUav);
    // This pass's SRV set and write UAV slot were fixed at workset creation
    // (kDescriptor* layout) - see ProcessInline's loop.
    const uint32_t srv_table_start = pass * kCodecSrvSetStride;
    const float pass_transfer = PassTransferStrength(pass);
    const float pass_color = PassColorStrength(pass);

    if (res.hdr_mode != 0
        && pass_transfer <= 0.f && pass_color <= 0.f) {
      // Zero-transfer pass (rule 3): exact copy across, preserving the
      // ping-pong parity the fixed per-pass SRV sets assume.  Only
      // HARD-invalid states take this path (see ProcessInline).
      Transition(
          command_list,
          decode_ref,
          *ref_state,
          D3D12_RESOURCE_STATE_COPY_SOURCE);
      *ref_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
      Transition(
          command_list,
          write_surface,
          *write_state,
          D3D12_RESOURCE_STATE_COPY_DEST);
      *write_state = D3D12_RESOURCE_STATE_COPY_DEST;
      CopyMip0(command_list, write_surface, decode_ref);
      const D3D12_RESOURCE_STATES ref_rest =
          pass == 0 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                    : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      Transition(
          command_list,
          decode_ref,
          D3D12_RESOURCE_STATE_COPY_SOURCE,
          ref_rest);
      *ref_state = ref_rest;
      Transition(
          command_list,
          write_surface,
          D3D12_RESOURCE_STATE_COPY_DEST,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      *write_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      executed = static_cast<int32_t>(pass);
      final_pass_transfer = 0.f;
      final_pass_color = 0.f;
      continue;
    }

    if (feature.slots[pass].handle == nullptr) {
      // Frame-ahead maturation (v5.2): a slot created THIS frame is never
      // evaluated on the same command list - same-frame create+evaluate
      // produces garbage frames and GPU hangs (field evidence from the
      // public forks).  Create it now; it starts evaluating next frame, and
      // the stand-in copy below serves the already-matured passes' result.
      if (!EnsureNrFeature(
              command_list,
              feature,
              pass,
              res,
              nr_input_width,
              nr_input_height,
              nr_input_width,
              nr_input_height)) {
        stack_failure = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
        failed_pass = pass;
      }
      break;
    }

    if (pass == 0 && res.hdr_mode == 0) {
      // Pass 0's reference is the game's color - no scratch surface exists
      // for it at heap creation.  The workset is keyed by (handle, color),
      // so after the first frame these rebinds write identical content and
      // SetSourceView's change guard turns them into no-ops, keeping the
      // shader-visible slots immutable while previous frames' command lists
      // may still be in flight.  The decode restores the pre-NR reference's
      // alpha from t3.  (HDR instead rebinds the LINEARIZE set's source once
      // per frame, before the loop - pass 0's encode reads work0.)
      SetSourceView(device, res, color, 0);
      SetSourceView(device, res, color, 3);
      // The look and transport sets carry the same reference slots.
      if ((res.look_surfaces & kLookSurfaceOutput) != 0) {
        SetSourceView(device, res, color, kDescriptorLookSet);
        SetSourceView(device, res, color, kDescriptorLookSet + 3);
      }
      if ((res.look_surfaces & kLookSurfaceTransport) != 0) {
        SetSourceView(device, res, color, kDescriptorTransportSet);
        SetSourceView(device, res, color, kDescriptorTransportSet + 3);
      }
    }
    if (ref_state != nullptr) {
      Transition(
          command_list,
          decode_ref,
          *ref_state,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      *ref_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    Transition(
        command_list,
        res.proxy,
        res.proxy_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    res.proxy_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // Encode the reference into the proxy at the NR working resolution:
    // the shader downsamples the full-res source (the game's color for pass
    // 0, the previous decoded result afterwards) via the scaled mapping.
    // v6 HDR encodes the LINEAR work surface instead (area-filtered
    // downsample, one divisor, hue shoulder, sRGB).
    if (res.hdr_mode != 0) {
      BindCodecV6(
          command_list,
          res,
          codec_pipeline.encode_v6,
          srv_table_start,
          kDescriptorProxyUav,
          nr_input_width,
          nr_input_height,
          width,
          height,
          0,
          0,
          nr_input_width,
          nr_input_height,
          nr_input_width,
          nr_input_height,
          frame_divisor,
          v6_pedestal_cap,
          pass_transfer,
          pass_color,
          v6_encoding,
          v6_dark_gate,
          0.f);
    } else {
      BindCodec(
          command_list,
          res,
          codec_pipeline.encode,
          srv_table_start,
          kDescriptorProxyUav,
          nr_input_width,
          nr_input_height,
          width,
          height,
          0,
          0,
          nr_input_width,
          nr_input_height,
          nr_input_width,
          nr_input_height,
          frame_paper_white,
          frame_diffuse_white,
          pass_transfer,
          pass_color);
    }
    if (timers_active) {
      gpu_timers::Mark(
          command_list, gpu_timers::PassEncodeStart(pass));
    }
    command_list->Dispatch(
        (nr_input_width + 15) / 16, (nr_input_height + 15) / 16, 1);
    UavBarrier(command_list, res.proxy);
    if (timers_active) {
      gpu_timers::Mark(command_list, gpu_timers::PassEncodeEnd(pass));
    }
    Transition(
        command_list,
        res.proxy,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    res.proxy_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    Transition(
        command_list,
        res.nr_output,
        res.nr_output_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    res.nr_output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    SetEvaluationParameters(
        feature,
        feature.slots[pass],
        pass,
        chain_reset_epoch,
        res,
        frame,
        nr_input_width,
        nr_input_height,
        nr_input_width,
        nr_input_height);

    if (timers_active) {
      gpu_timers::Mark(command_list, gpu_timers::PassEvalStart(pass));
    }
    // The NGX runtime records this handle's work into the game's list: a
    // tracked use, so a later retirement waits for exactly this submission.
    submission::TrackUse(command_list, feature.slots[pass].handle);
    NVSDK_NGX_Result nr_result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    {
      DirectCallScope ngx_direct_call;
      try {
        nr_result = feature.slots[pass].via_core
            ? core_evaluate_feature(
                  command_list,
                  feature.slots[pass].handle,
                  feature.slots[pass].parameters,
                  nullptr)
            : direct_api.evaluate(
                  command_list,
                  feature.slots[pass].handle,
                  feature.slots[pass].parameters,
                  nullptr);
      } catch (...) {
        Log(reshade::log::level::error, "pre-SR feature 18 evaluate raised an exception");
        nr_result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
      }
    }
    last_result = static_cast<uint32_t>(nr_result);
    if (NVSDK_NGX_FAILED(nr_result)) {
      stack_failure = nr_result;
      failed_pass = pass;
      break;
    }
    UavBarrier(command_list, res.nr_output);
    if (timers_active) {
      gpu_timers::Mark(command_list, gpu_timers::PassEvalEnd(pass));
    }
    Transition(
        command_list,
        res.nr_output,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    res.nr_output_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // The look stage, as on the after path.
    const LookRoute route =
        RunLookStage(command_list, device, res, feature.slots[pass], pass, look_chain, frame);
    Transition(
        command_list,
        write_surface,
        *write_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    *write_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // Decode at the full color resolution, sampling the working-res neural
    // result through the scaled mapping and blending over the full-res
    // reference (t3).  v6 HDR resolves in LINEAR source units onto the work
    // ping-pong surface (no transfer function until the single commit).
    if (res.hdr_mode != 0) {
      BindCodecV6(
          command_list,
          res,
          codec_pipeline.resolve_v6,
          route.srv_set,
          write_uav_index,
          width,
          height,
          width,
          height,
          0,
          0,
          route.proxy_width,
          route.proxy_height,
          route.neural_width,
          route.neural_height,
          frame_divisor,
          v6_pedestal_cap,
          pass_transfer,
          pass_color,
          v6_encoding,
          v6_dark_gate,
          v6_chroma_clamp);
    } else {
      BindCodec(
          command_list,
          res,
          codec_pipeline.decode,
          route.srv_set,
          write_uav_index,
          width,
          height,
          width,
          height,
          0,
          0,
          route.proxy_width,
          route.proxy_height,
          route.neural_width,
          route.neural_height,
          frame_paper_white,
          frame_diffuse_white,
          pass_transfer,
          pass_color);
    }
    command_list->Dispatch((width + 15) / 16, (height + 15) / 16, 1);
    UavBarrier(command_list, write_surface);
    RunBlackLevelRestore(
        command_list,
        res,
        srv_table_start,
        write_uav_index,
        width,
        height,
        frame_paper_white,
        frame_diffuse_white,
        pass_transfer,
        pass_color,
        route.shaped);
    if (timers_active) {
      gpu_timers::Mark(command_list, gpu_timers::PassResolveEnd(pass));
    }
    // Intermediate results stay in the ping-pong surfaces for the next pass
    // (resting in UAV, their tracked state); the stand-in copy happens once,
    // after the loop, from the last executed pass's write surface.
    model_evaluated = true;
    executed = static_cast<int32_t>(pass);
    final_pass_transfer = pass_transfer;
    final_pass_color = pass_color;
    final_pass_shaped = route.shaped;
  }

  if (NVSDK_NGX_FAILED(stack_failure)) {
    // Leave the game its own color rather than feeding it a broken image.
    // The proxy is post-encode and returns to its UAV resting state via a
    // tracked transition; nr_output can break here from a slot-create
    // failure while still in its SRV resting state (an earlier pass's decode
    // transitioned it), so it is parked the same tracked way - the next
    // attempt's transitions must stay valid.  Reference surfaces carry
    // consistent tracked states.
    Transition(
        command_list,
        res.proxy,
        res.proxy_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    res.proxy_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Transition(
        command_list,
        res.nr_output,
        res.nr_output_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    res.nr_output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    NrFeatureSlot& failed_slot = feature.slots[failed_pass];
    failed_slot.failed = true;
    failed_slot.failed_ns = SteadyNowNs();
    ReleaseNrSlot(feature, failed_pass, true);
    // The chain broke mid-stack: passes after the failure skipped an input
    // frame, so their temporal history is discontinuous - mark them to
    // reset on the next attempt (the failed slot resets itself via
    // pending_reset when recreated).  Passes before the failure evaluated
    // normally and keep their history.
    for (uint32_t later = failed_pass + 1; later < kMaxNrPasses; ++later) {
      feature.slots[later].pending_reset = true;
    }
    std::ostringstream message;
    message << "pre-SR feature 18 evaluate failed with 0x" << std::hex
            << static_cast<uint32_t>(stack_failure)
            << " (stack pass " << (failed_pass + 1) << '/' << requested_stack
            << ")"
            << "; the frame runs without NR - it retries on the next frame,"
               " then after 1 s, doubling up to 30 s while it keeps failing";
    Log(reshade::log::level::error, message.str());
    if (timers_active) {
      gpu_timers::End(command_list);
    }
    device->Release();
    return false;
  }

  if (executed < 0) {
    // Frame-ahead maturation frame: pass 0 itself was just created and
    // NR did not run.  Park the codec surfaces and decline - the
    // game's evaluate runs on its own color, and the stand-in
    // pointer swap must not happen.  The handle is still recorded so a
    // mirrored second evaluate of the same handle in this present cannot
    // evaluate the freshly created slot on the same command list.
    CountNrDecline(NrDeclineReason::kSlotWarmupMaturation);
    RecordOutputPass(color, handle);
    Transition(
        command_list,
        res.proxy,
        res.proxy_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    res.proxy_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Transition(
        command_list,
        res.nr_output,
        res.nr_output_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    res.nr_output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (timers_active) {
      gpu_timers::End(command_list);
    }
    device->Release();
    return false;
  }

  // The stand-in copy serves the LAST EXECUTED pass: the full stack on a
  // normal frame, or the already-matured partial stack when the loop above
  // broke on a freshly created slot (frame-ahead maturation).
  ID3D12Resource* final_surface = nullptr;
  D3D12_RESOURCE_STATES* final_state = nullptr;
  ID3D12Resource* work_final = nullptr;
  D3D12_RESOURCE_STATES* work_final_state = nullptr;
  if (res.hdr_mode != 0) {
    // v6 single encode-back (see ProcessInline): pedestal estimate against
    // the untouched work0, then the one bounded, dark-gated removal and
    // transfer into the destination encoding, landing in `decoded` - the
    // surface the stand-in copy reads.
    work_final = work_out_surfaces[executed & 1];
    work_final_state = work_out_states[executed & 1];
    const uint32_t commit_set =
        (executed & 1) != 0 ? kDescriptorCommitBSet : kDescriptorCommitASet;
    Transition(
        command_list,
        work_final,
        *work_final_state,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    *work_final_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (timers_active) {
      gpu_timers::Mark(command_list, gpu_timers::kSlotCommitStart);
    }
    // See the post-SR commit: a single shaped pass measures NR's own lift,
    // and a frame without the pedestal removal skips the reduce.
    const bool measure_unshaped = executed == 0 && final_pass_shaped
        && codec_pipeline.pedestal_reduce_unshaped != nullptr;
    if (FramePedestalRemoval(res)) {
      BindCodecV6(
          command_list,
          res,
          measure_unshaped ? codec_pipeline.pedestal_reduce_unshaped
                           : codec_pipeline.pedestal_reduce,
          measure_unshaped ? 0u : commit_set,
          kDescriptorBlockMeanUav,
          width,
          height,
          width,
          height,
          0,
          0,
          res.input_width,
          res.input_height,
          res.input_width,
          res.input_height,
          frame_divisor,
          v6_pedestal_cap,
          final_pass_transfer,
          final_pass_color,
          v6_encoding,
          v6_dark_gate,
          v6_chroma_clamp);
      command_list->Dispatch((width + 31) / 32, (height + 31) / 32, 1);
      UavBarrier(command_list, res.block_mean);
    }
    Transition(
        command_list,
        res.decoded,
        res.decoded_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    res.decoded_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    BindCodecV6(
        command_list,
        res,
        frame.feed != FeedSource::kV1 ? codec_pipeline.commit_exposure
                                      : codec_pipeline.commit,
        commit_set,
        kDescriptorDecodedUav,
        width,
        height,
        width,
        height,
        0,
        0,
        width,
        height,
        width,
        height,
        frame_divisor,
        v6_pedestal_cap,
        final_pass_transfer,
        final_pass_color,
        v6_encoding,
        v6_dark_gate,
        0.f);
    command_list->Dispatch((width + 15) / 16, (height + 15) / 16, 1);
    UavBarrier(command_list, res.decoded);
    if (timers_active) {
      gpu_timers::Mark(command_list, gpu_timers::kSlotCommitEnd);
    }
    if (norm_trace_enabled.load(std::memory_order_relaxed)) {
      norm_trace::Record(
          command_list, device, res.norm_scale, res.norm_scale_state,
          res.block_mean, width, height,
          {present_generation, SteadyNowNs(), res.id, frame_snap,
             static_cast<uint32_t>(frame.feed)});
    }
    final_surface = res.decoded;
    final_state = &res.decoded_state;
  } else {
    final_surface = stack_surfaces[executed & 1];
    final_state = stack_states[executed & 1];
  }
  Transition(
      command_list,
      final_surface,
      *final_state,
      D3D12_RESOURCE_STATE_COPY_SOURCE);
  *final_state = D3D12_RESOURCE_STATE_COPY_SOURCE;

  // Copy the NR result into the persistent stand-in and hand that to
  // the game's evaluate.  The stand-in lives in SRV-readable state while NGX
  // sees it and is moved back to COPY_DEST at the start of the next frame.
  Transition(
      command_list,
      res.pre_sr_color,
      res.pre_sr_color_state,
      D3D12_RESOURCE_STATE_COPY_DEST);
  CopyMip0(command_list, res.pre_sr_color, final_surface);
  Transition(
      command_list,
      res.pre_sr_color,
      D3D12_RESOURCE_STATE_COPY_DEST,
      kPreSrReadable);
  res.pre_sr_color_state = kPreSrReadable;
  if (timers_active) {
    gpu_timers::Mark(command_list, gpu_timers::kSlotCopyBackEnd);
  }
  Transition(
      command_list,
      final_surface,
      *final_state,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  *final_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  if (work_final != nullptr) {
    // Park the read v6 work surface back to its UAV resting state; work0
    // stays SRV until the next frame's linearize transition reclaims it.
    Transition(
        command_list,
        work_final,
        *work_final_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    *work_final_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  Transition(
      command_list,
      res.proxy,
      res.proxy_state,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  res.proxy_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  Transition(
      command_list,
      res.nr_output,
      res.nr_output_state,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  res.nr_output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

  // Point the game's NGX Color at the NR stand-in for the duration of
  // the real evaluate below; RestorePreSrColor puts the game's pointer back.
  swap.color = color;
  swap.result = res.pre_sr_color;
  NVSDK_NGX_Parameter_SetVoidPointer(
      const_cast<NVSDK_NGX_Parameter*>(game_parameters),
      NVSDK_NGX_Parameter_Color,
      res.pre_sr_color);
  RecordOutputPass(color, handle);
  if (!model_evaluated) {
    ++bypassed_evaluations;
    CountNrDecline(NrDeclineReason::kSlotWarmupMaturation);
    if (timers_active) {
      gpu_timers::MarkBypassed();
      gpu_timers::End(command_list);
    }
    device->Release();
    return true;
  }
  for (auto& slot : feature.slots) slot.fail_count = 0;
  const uint64_t count = ++successful_pre_sr_evaluations;
  streamline_escalation_deficits.store(0, std::memory_order_relaxed);
  last_input_width = nr_input_width;
  last_input_height = nr_input_height;
  last_output_width = width;
  last_output_height = height;
  if (count == 1 || count == 60 || count == 600) {
    std::ostringstream message;
    message << "pre-SR feature 18 evaluation succeeded (count=" << count
            << ", NR input " << nr_input_width << 'x' << nr_input_height
            << " (guides " << width << 'x' << height << "), output "
            << width << 'x' << height << ", " << requested_stack
            << " stack pass(es) [pre-SR])";
    Log(reshade::log::level::info, message.str());
  }
  if (timers_active) {
    gpu_timers::End(command_list);
  }
  device->Release();
  return true;
}

// Streamline's public API normally delegates DLSS/DLSSD to NGX.  A few
// shipped integrations keep that bridge private, however.  This fallback
// builds the same small NGX parameter contract from Streamline's mandatory
// tags and runs the signed runtime directly.  It is intentionally opt-in at
// the call site and fails closed when any resource or command-list invariant
// is missing.
inline bool ProcessStreamlineInline(
    ID3D12GraphicsCommandList* command_list,
    sl::Feature streamline_feature,
    const StreamlineCapture& capture,
    bool* command_list_touched = nullptr) {
  // The Streamline layer is an "after" insertion point: NR enhances the
  // finished DLSS output.  With the pre-SR insertion point selected the two
  // are mutually exclusive, exactly as for the NGX evaluate path - except for
  // Ray Reconstruction, which keeps the after insertion point (PreSrTakes).
  if (!enabled.load()
      || (nr_before_upscale.load() && streamline_feature != sl::kFeatureDLSS_RR)
      || command_list == nullptr
      || !IsSupportedInjectionCommandList(command_list)) {
    return false;
  }
  if (capture.color.resource == nullptr
      || capture.output.resource == nullptr || capture.motion.resource == nullptr
      || capture.depth.resource == nullptr) {
    CountNrDecline(NrDeclineReason::kStreamlineCaptureIncomplete);
    if (!logged_streamline_capture.exchange(true)) {
      Log(reshade::log::level::warning,
          "Streamline NR declined: the capture lacks one of "
          "color/output/motion/depth; frames run without NR");
    }
    return false;
  }
  const auto is_shader_readable = [](uint32_t state) {
    return state == UINT_MAX
        || (state & (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                     | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) != 0;
  };
  if ((capture.output.state != UINT_MAX
       && (capture.output.state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) == 0)
      || !is_shader_readable(capture.color.state)
      || !is_shader_readable(capture.motion.state)
      || !is_shader_readable(capture.depth.state)) {
    // Streamline supplies explicit states.  Refuse to inject when the command
    // list would need an untracked transition; this is the safety boundary
    // that keeps private integrations from corrupting the game's graph.
    CountNrDecline(NrDeclineReason::kStreamlineResourceStates);
    if (!logged_streamline_states.exchange(true)) {
      Log(reshade::log::level::warning,
          "Streamline NR declined: captured resource states are not "
          "injectable (untracked transition would be needed)");
    }
    return false;
  }
  if (!EnsureDirectRuntime(command_list) || core_allocate_parameters == nullptr
      || core_destroy_parameters == nullptr) {
    CountNrDecline(NrDeclineReason::kStreamlineRuntimeUnavailable);
    return false;
  }

  NVSDK_NGX_Parameter* parameters = nullptr;
  const NVSDK_NGX_Result allocate_result = core_allocate_parameters(&parameters);
  last_result = static_cast<uint32_t>(allocate_result);
  if (NVSDK_NGX_FAILED(allocate_result) || parameters == nullptr) return false;

  const auto desc_for = [](const StreamlineResourceRef& resource) {
    return resource.resource->GetDesc();
  };
  const D3D12_RESOURCE_DESC color_desc = desc_for(capture.color);
  const D3D12_RESOURCE_DESC output_desc = desc_for(capture.output);
  const uint32_t input_width = capture.color.extent.width != 0
      ? capture.color.extent.width
      : static_cast<uint32_t>(color_desc.Width);
  const uint32_t input_height = capture.color.extent.height != 0
      ? capture.color.extent.height
      : color_desc.Height;
  const uint32_t output_width = capture.output.extent.width != 0
      ? capture.output.extent.width
      : static_cast<uint32_t>(output_desc.Width);
  const uint32_t output_height = capture.output.extent.height != 0
      ? capture.output.extent.height
      : output_desc.Height;
  if (input_width == 0 || input_height == 0 || output_width == 0 || output_height == 0
      || output_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
      || output_desc.DepthOrArraySize != 1
      || output_desc.SampleDesc.Count != 1) {
    core_destroy_parameters(parameters);
    last_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
    return false;
  }

  parameters->Set(NVSDK_NGX_Parameter_Color, capture.color.resource);
  parameters->Set(NVSDK_NGX_Parameter_Output, capture.output.resource);
  parameters->Set(NVSDK_NGX_Parameter_MotionVectors, capture.motion.resource);
  parameters->Set(NVSDK_NGX_Parameter_Depth, capture.depth.resource);
  NVSDK_NGX_Parameter_SetUI(
      parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, input_width);
  NVSDK_NGX_Parameter_SetUI(
      parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, input_height);
  NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_Width, input_width);
  NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_Height, input_height);
  NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_OutWidth, output_width);
  NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_OutHeight, output_height);
  NVSDK_NGX_Parameter_SetUI(
      parameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
      capture.color.extent.left);
  NVSDK_NGX_Parameter_SetUI(
      parameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
      capture.color.extent.top);
  NVSDK_NGX_Parameter_SetUI(
      parameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
      capture.depth.extent.left);
  NVSDK_NGX_Parameter_SetUI(
      parameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
      capture.depth.extent.top);
  NVSDK_NGX_Parameter_SetUI(
      parameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
      capture.motion.extent.left);
  NVSDK_NGX_Parameter_SetUI(
      parameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
      capture.motion.extent.top);
  NVSDK_NGX_Parameter_SetUI(
      parameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
      capture.output.extent.left);
  NVSDK_NGX_Parameter_SetUI(
      parameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y,
      capture.output.extent.top);
  NVSDK_NGX_Parameter_SetF(
      parameters, NVSDK_NGX_Parameter_MV_Scale_X,
      capture.motion_scale_x * static_cast<float>(input_width));
  NVSDK_NGX_Parameter_SetF(
      parameters, NVSDK_NGX_Parameter_MV_Scale_Y,
      capture.motion_scale_y * static_cast<float>(input_height));
  // Jitter presence mirrors the SL Constants block: a frame without constants
  // leaves jitter unset (missing), never pinned to a guessed zero.  The NR
  // path downstream reads it presence-aware (DeriveFeatureState probes
  // Jitter_Offset_X/Y), so the contract keeps missing-versus-zero semantics.
  if (capture.has_constants) {
    NVSDK_NGX_Parameter_SetF(
        parameters, NVSDK_NGX_Parameter_Jitter_Offset_X, capture.jitter_x);
    NVSDK_NGX_Parameter_SetF(
        parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y, capture.jitter_y);
  }
  NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_PerfQualityValue, 0);
  NVSDK_NGX_Parameter_SetI(
      parameters, NVSDK_NGX_Parameter_Reset, capture.reset ? 1 : 0);
  const bool likely_hdr = capture.has_options
      ? capture.hdr
      : (output_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
         || output_desc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT
         || output_desc.Format == DXGI_FORMAT_R11G11B10_FLOAT);
  // SR must run in perceptual/exposure space for HDR: assert IsHDR + AutoExposure
  // and pin an explicit identity exposure on the game-SR evaluate as well.
  const uint32_t sr_create_flags =
      (capture.depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0)
      | (likely_hdr ? (NVSDK_NGX_DLSS_Feature_Flags_IsHDR
                       | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure)
                    : 0);
  NVSDK_NGX_Parameter_SetI(
      parameters,
      NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
      static_cast<int32_t>(sr_create_flags));
  if (likely_hdr) {
    NVSDK_NGX_Parameter_SetF(
        parameters, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    NVSDK_NGX_Parameter_SetF(
        parameters, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
  }

  // A stable map-node address is used solely as an opaque key; it is never
  // dereferenced as an NGX handle.  Keeping the FeatureState between calls is
  // important because the NR runtime's temporal history lives in that handle.
  const uint64_t stream_key =
      (static_cast<uint64_t>(streamline_feature) << 32) | capture.viewport;
  uint8_t& synthetic_handle_storage = streamline_handle_keys[stream_key];
  const auto* synthetic_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(
      &synthetic_handle_storage);
  // Recorded so OutputPassAllowed can recognize this pass as the Streamline
  // side of a mirroring pair on one output.
  synthetic_handles.insert(&synthetic_handle_storage);
  auto [entry, inserted] = features.try_emplace(synthetic_handle);
  if (inserted) {
    entry->second.source_feature = streamline_feature == sl::kFeatureDLSS_RR
        ? kFeatureDlssd
        : kFeatureDlss;
  }
  const bool processed = ProcessInline(
      command_list, synthetic_handle, parameters, command_list_touched);
  core_destroy_parameters(parameters);
  if (processed) {
    // Count-cadence logging (1/60/600, like the native success lines): a
    // per-evaluate line flooded a full session into thousands of entries.
    const uint64_t count = ++streamline_direct_fallback_successes;
    if (count == 1 || count == 60 || count == 600) {
      Log(
          reshade::log::level::info,
          (streamline_feature == sl::kFeatureDLSS_RR
               ? "Streamline DLSSD/RR evaluated through direct NGX fallback"
               : "Streamline DLSS evaluated through direct NGX fallback")
              + std::string(" (count=") + std::to_string(count) + ")");
    }
  }
  return processed;
}

// Per-slot wrapper templates.  Each detoured NGX module copy is assigned one
// slot (see kMaxNgxSlots) and its hook functions call that slot's own original
// pointer, never a shared global.  A slot is just an index into ngx_slot_real;
// the wrapper table below maps a runtime slot to these instantiations.
// NR owns only DLSS/DLSSD evaluates.  created_feature_ids is authoritative for
// every handle whose CreateFeature we intercepted: anything else (DLSSG
// multi-frame generation, DeepDVC, ...) must never reach ProcessInline even
// when its evaluate parameter block looks DLSS-shaped - claiming a frame
// generation evaluate makes NR run on generated frames.  Handles missing from
// both records keep the lazy contract path (titles that create DLSS before the
// hooks install, e.g. Control).  Caller must hold runtime_mutex.
inline bool KnownHandleFeature(
    const NVSDK_NGX_Handle* handle, NVSDK_NGX_Feature* feature) {
  if (const auto created = created_feature_ids.find(handle);
      created != created_feature_ids.end()) {
    *feature = created->second;
    return true;
  }
  if (const auto entry = features.find(handle); entry != features.end()) {
    *feature = entry->second.source_feature;
    return true;
  }
  return false;
}

inline bool RegisteredDlssEvaluate(const NVSDK_NGX_Handle* handle) {
  NVSDK_NGX_Feature feature = kFeatureDlss;
  if (!KnownHandleFeature(handle, &feature) || feature == kFeatureDlss
      || feature == kFeatureDlssd) {
    return true;
  }
  const uint32_t id_bit = static_cast<uint32_t>(feature) < 32
      ? (1u << static_cast<uint32_t>(feature))
      : 0;
  if (id_bit != 0 && (logged_skip_ids.fetch_or(id_bit) & id_bit) != 0) return false;
  Log(
      reshade::log::level::info,
      "skipping NR on NGX evaluate: feature "
          + std::to_string(static_cast<int>(feature)) + " ("
          + SourceFeatureName(feature)
          + ") is not DLSS/DLSSD; frame generation and other NGX features"
            " are untouched");
  return false;
}

// Whether this evaluate takes the pre-SR insertion point (NRPreUpscale).
// Pre-SR runs NR on the Color the game hands its DLSS evaluate.  For Ray
// Reconstruction that Color is the ray-traced lighting before RR has
// denoised it, and NR enhances whatever it is given: it would sharpen the
// noise.  So a DLSSD/RR handle keeps the after-upscale insertion point while
// pre-SR is selected; DLSS/DLAA and handles of unknown feature take pre-SR as
// before.  Caller must hold runtime_mutex.
inline std::atomic_bool logged_pre_sr_rr = false;
inline bool PreSrTakes(const NVSDK_NGX_Handle* handle) {
  if (!nr_before_upscale.load()) return false;
  NVSDK_NGX_Feature feature = kFeatureDlss;
  if (!KnownHandleFeature(handle, &feature) || feature != kFeatureDlssd) return true;
  if (!logged_pre_sr_rr.exchange(true)) {
    Log(reshade::log::level::info,
        "pre-SR is selected, but this evaluate is Ray Reconstruction"
        " (DLSSD/RR): its Color is still-noisy ray-traced lighting that RR cleans,"
        " so NR runs on RR's finished output after upscaling instead");
  }
  return false;
}

// P4 A1 (defined beside the queue-completion installer, which lives below
// the submission tracker): called from the D3D12 NGX entry wrappers - the
// one place a foreign session's device is in hand.
inline void EnsureForeignEntryInstalls(
    ID3D12GraphicsCommandList* command_list);
// P4 A2 (defined at the teardown section): the foreign teardown triggers,
// and the pending swapchain teardown (ServicePendingTeardown).
inline bool Shutdown(bool proof_gated = false);

// A-1 (v7.0.0): the NGX entry wrappers' first-argument gate
// (arg_plausibility.hpp).  Tier 1 is enforced: a first argument that is not a
// readable object is never dereferenced by this addon.  Tier 2 (a readable
// object whose vtable lies outside every loaded image) is counted only.
inline std::atomic_uint64_t ngx_implausible_args{0};
inline std::atomic_uint64_t ngx_vtable_not_image{0};
inline std::atomic_bool logged_implausible_arg{false};

// A-2 (v7.0.0), observe-only: which implausible arguments arrived through a
// detoured module copy that is not the NGX core.  The Skyrim SE caller was
// ENB AA's Streamline-shipped nvngx_dlss.dll, a second copy the scan detours
// because its name contains "nvngx".  Whether such copies should be detoured
// at all is an open policy question (docs/memory/legacy-crash-classes.md);
// this count is the data for it, and nothing acts on it.
inline std::atomic_bool ngx_slot_noncore[kMaxNgxSlots] = {};
inline std::atomic_uint64_t ngx_noncore_implausible_args{0};

// True when the wrapper may go on to dereference `command_list`; false means
// it must hand the call to the real export untouched.  The vtable span is the
// highest command-list slot the addon ever reads (kCmdSlotMax); the D3D11
// family passes its context span and its own slot flags.
inline bool NgxFirstArgumentUsable(
    const void* command_list, const char* entry, int slot,
    std::size_t vtable_bytes =
        (static_cast<std::size_t>(kCmdSlotMax) + 1) * sizeof(void*),
    const std::atomic_bool* noncore_slots = ngx_slot_noncore) {
  const ArgPlausibility plausibility =
      ClassifyComArgument(command_list, vtable_bytes);
  if (plausibility == ArgPlausibility::kPlausible) return true;
  if (plausibility == ArgPlausibility::kVtableNotImage) {
    ngx_vtable_not_image.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  ngx_implausible_args.fetch_add(1, std::memory_order_relaxed);
  const bool noncore = noncore_slots[slot].load(std::memory_order_relaxed);
  if (noncore) {
    ngx_noncore_implausible_args.fetch_add(1, std::memory_order_relaxed);
  }
  if (!logged_implausible_arg.exchange(true)) {
    char address[32];
    std::snprintf(address, sizeof(address), "0x%llX",
                  static_cast<unsigned long long>(
                      reinterpret_cast<std::uintptr_t>(command_list)));
    Log(reshade::log::level::warning,
        std::string("NGX ") + entry
            + " entered with a first argument that is not a readable object ("
            + address + ") via detoured module copy [" + std::to_string(slot)
            + (noncore ? "] (not the NGX core)" : "]")
            + "; the call goes to the real export untouched and this addon"
              " does not touch it");
  }
  return false;
}

// The gate's telemetry group.  Built only in a session where the gate ever
// spoke, so every other session's telemetry line stays byte-identical.
inline std::string ArgGateTelemetry() {
  const std::uint64_t implausible =
      ngx_implausible_args.load(std::memory_order_relaxed);
  const std::uint64_t not_image =
      ngx_vtable_not_image.load(std::memory_order_relaxed);
  if (implausible == 0 && not_image == 0) return {};
  return " args[implausible=" + std::to_string(implausible)
         + " vtable_not_image=" + std::to_string(not_image) + " noncore="
         + std::to_string(
             ngx_noncore_implausible_args.load(std::memory_order_relaxed))
         + "]";
}

// A game feature create both API families record.  Records the true
// feature ID before the DLSS-only contract registration: the evaluate-side
// gate (RegisteredDlssEvaluate) needs to recognize every other NGX feature
// (DLSSG, DeepDVC, ...) as not-ours.  The insert shares runtime_mutex with
// every other created_feature_ids access (readers: RegisteredDlssEvaluate;
// erasers: the release hooks and Shutdown); an unsynchronized insert could
// race a concurrent evaluate's find and corrupt the map when a game creates
// features on a worker thread.  Caller holds runtime_mutex.
inline void RegisterCreatedFeatureLocked(const NVSDK_NGX_Handle* handle,
                                         NVSDK_NGX_Feature feature,
                                         const NVSDK_NGX_Parameter* parameters) {
  if (feature == kFeatureDlssNr) {
    NoteForeignNr(true, "a feature-18 (DLSSNR) create that is not this"
                        " addon's succeeded");
  }
  created_feature_ids[handle] = feature;
  if (feature != kFeatureDlss && feature != kFeatureDlssd) return;
  FeatureState state = DeriveFeatureState(parameters);
  state.source_feature = feature;
  create_contracts[handle] = state;
  features[handle] = state;
}

// A game feature release both API families record.  Retire, never free
// here: the game's evaluates referencing our NR handles may still be in
// flight on a submitted command list, and freeing now would race the GPU
// (the P0 "source-feature release frees NR handles immediately" audit
// finding).  The retired slots release once their GPU lease completes.  The
// released stream's scratch can never be evaluated again (worksets are
// keyed by source handle); left in the pool it held a full set of
// output-resolution surfaces until LRU pressure evicted it - games that
// rebuild DLSS on loads or settings changes (Silent Hill 2: two rebuilds in
// a 2-minute session) accumulated orphans up to the cap.  Returns whether
// that was the last DLSS feature.  Caller holds runtime_mutex.
inline bool ForgetSourceHandleLocked(const NVSDK_NGX_Handle* handle) {
  const auto entry = features.find(handle);
  if (entry != features.end()) {
    ReleaseAllNrSlots(entry->second, true);
    features.erase(entry);
  }
  created_feature_ids.erase(handle);
  create_contracts.erase(handle);
  for (auto it = worksets.begin(); it != worksets.end();) {
    if (it->first.handle == handle) {
      RetireWorksetResources(std::move(it->second));
      it = worksets.erase(it);
    } else {
      ++it;
    }
  }
  return features.empty();
}

// The tail of a game create's one-shot result line when the create FAILED.
// A game whose own DLSS never starts draws its UI over a frame nothing
// wrote, which reads as "NR output is black" - Borderlands 4 (2026-09-23)
// returned NotInitialized (0xBAD00007) on every DLSS and DLSSG create, with
// NR on and with NR off, and the first diagnosis blamed NR.  The line says
// whose failure it is, so the next report does not need a disassembler.
inline std::string GameCreateFailureNote(NVSDK_NGX_Result result) {
  std::ostringstream note;
  note << " 0x" << std::hex << std::uppercase << static_cast<uint32_t>(result)
       << (result == NVSDK_NGX_Result_FAIL_NotInitialized ? " (NotInitialized)" : "")
       << ": the GAME's own NGX feature did not start, so it writes no output"
          " and whatever the game composites its UI over is not DLSS's or NR's"
          " image; NR cannot run on a feature the game never created (check"
          " the NVIDIA driver and the game's DLSS DLLs)";
  return note.str();
}

template <int Slot>
inline NVSDK_NGX_Result NVSDK_CONV HookedCreateFeatureSlot(
    ID3D12GraphicsCommandList* command_list,
    NVSDK_NGX_Feature feature,
    NVSDK_NGX_Parameter* parameters,
    NVSDK_NGX_Handle** output_handle) {
  debug::TouchNgxCreate();
  ngx_entry_create[Slot].fetch_add(1, std::memory_order_relaxed);
  auto real = reinterpret_cast<decltype(&NVSDK_NGX_D3D12_CreateFeature)>(
      ngx_slot_real[Slot].create);
  CallbackScope callback_scope;
  if (!callback_scope) return real(command_list, feature, parameters, output_handle);
  ++intercepted_creates;
  // P4 A1: the foreign-source queue-completion install (observe-only; a
  // no-op in every D3D12 session - see the definition).  It is the create
  // wrapper's only dereference of command_list, so A-1 gates exactly it.
  // Not on the native D3D11 route: the tool's D3D12 work is not served
  // there, so there is nothing to prove completion for.
  if (!InsideDirectCall() && !ForeignD3D12SourceIgnored()
      && NgxFirstArgumentUsable(command_list, "create", Slot)) {
    EnsureForeignEntryInstalls(command_list);
  }
  const uint32_t id_bit = static_cast<uint32_t>(feature) < 32
      ? (1u << static_cast<uint32_t>(feature))
      : 0;
  if (id_bit != 0 && (logged_create_ids.fetch_or(id_bit) & id_bit) == 0) {
    Log(
        reshade::log::level::info,
        "NGX feature create intercepted: feature="
            + std::to_string(static_cast<int>(feature)) + " ("
            + SourceFeatureName(feature) + "), slot=" + std::to_string(Slot));
  }
  const NVSDK_NGX_Result result = real(command_list, feature, parameters, output_handle);
  // One-shot result line per feature ID (see logged_create_result_ids).  Our
  // own direct creates skip this wrapper's bookkeeping via
  // InsideDirectCall() and log their own success/failure lines; the game's
  // creates - especially features NR never processes, like DLSSG, whose
  // result no other line ever reports - get exactly one.
  if (!InsideDirectCall()) {
    const uint32_t result_bit = static_cast<uint32_t>(feature) < 32
        ? (1u << static_cast<uint32_t>(feature))
        : 0;
    if (result_bit != 0
        && (logged_create_result_ids.fetch_or(result_bit) & result_bit) == 0) {
      std::ostringstream result_text;
      result_text << "NGX feature create returned: feature="
                  << static_cast<int>(feature) << " ("
                  << SourceFeatureName(feature) << "), result="
                  << static_cast<int>(result)
                  << (NVSDK_NGX_FAILED(result) ? " (failed)" : " (success)");
      if (!NVSDK_NGX_FAILED(result) && output_handle != nullptr
          && *output_handle != nullptr) {
        result_text << ", handle=0x" << std::hex
                    << reinterpret_cast<uintptr_t>(*output_handle);
      }
      if (NVSDK_NGX_FAILED(result)) result_text << GameCreateFailureNote(result);
      Log(NVSDK_NGX_FAILED(result) ? reshade::log::level::warning
                                   : reshade::log::level::info,
          result_text.str());
    }
  }
  if (InsideDirectCall() || NVSDK_NGX_FAILED(result)
      || output_handle == nullptr || *output_handle == nullptr || parameters == nullptr) {
    return result;
  }
  RuntimeLock lock(runtime_mutex);
  RegisterCreatedFeatureLocked(*output_handle, feature, parameters);
  return result;
}

// ---------------------------------------------------------------------------
// Compute-state shadow (v5.2.1).
//
// The injected NR work binds OUR codec root signature, descriptor heaps, and
// pipeline onto the game's command list; when the hook returns, the game
// records its next dispatches against those foreign bindings.  On bindless
// engines that do not rebind between passes (RE Engine - Resident Evil
// Requiem, Dragon's Dogma 2, Monster Hunter Wilds - plus Pragmata and 007
// First Light) that faults the GPU and the device is removed.  Dagherbou's
// OptiScaler_DLSSNR fork shipped this exact fix (v0.1.1.5) for exactly this
// game list; our own crash log (RE Requiem, v5.0) matches it.
//
// D3D12 command lists expose NO getters for bound state, so the state to
// resume with must be captured.  This module detours three ID3D12Graphics-
// CommandList method bodies (vtable-resolved addresses, the OptiScaler
// technique) and, while the REAL NGX evaluate runs, records every bind it
// makes - last-write-wins per component.  After our injected work, the
// captured binds are re-applied, leaving the list exactly as a vanilla NGX
// evaluate does.  This is correct regardless of whether the NGX runtime
// restores the game's state before returning (then the last binds of the
// window ARE the game's) or leaves its own (vanilla post-evaluate state,
// which every game demonstrably tolerates).
//
// Vtable slots live in cmd_slots.hpp together with kCmdHookSlots, the exact
// list install/validate/unhook iterate (validated against SDK 10.0.26100.0:
// base 9 = IUnknown 3 + ID3D12Object 4 + GetDevice + GetType; later
// ID3D12GraphicsCommandListN interfaces only append methods, so the slots
// are stable across versions).

// The observed-state model, capture windows, the persistent per-list
// shadow, and the exact restore sequence live in command_state.hpp
// (dependency-free so test/dlss5_gpu runs sentinel restoration cases
// against a real device with the debug layer).


inline void* real_cmd_reset = nullptr;
inline void* real_cmd_clear_state = nullptr;
inline void* real_cmd_set_pipeline_state = nullptr;
inline void* real_cmd_execute_bundle = nullptr;
inline void* real_cmd_execute_indirect = nullptr;
inline void* real_cmd_set_descriptor_heaps = nullptr;
// The record-vs-replay heap trace (PLAN_REHAB_V7.md 7, R8 hazard row):
// "the implementation the detours go on MUST be the one the restore calls
// through; a heap trace (record vs replay list pointer) tells the two apart
// in one line each.  Any change here ships with that trace."  Env-armed
// (RENODX_NR_TEST_HEAP_TRACE=1) so field logs stay quiet; the A3 bypass lane
// arms it.  One line each per session: the first heap set the shadow
// recorded, and the first restore that replayed heaps - both naming the
// list pointer, so a record on one implementation replayed through another
// (the 0xbaadf00d shape) is one grep apart.
inline std::atomic_bool heap_trace_record_logged{false};
inline std::atomic_bool heap_trace_replay_logged{false};

inline bool HeapTraceArmed() {
  static const bool armed = [] {
    char buffer[16] = {};
    size_t length = 0;
    if (getenv_s(&length, buffer, sizeof(buffer),
                 "RENODX_NR_TEST_HEAP_TRACE") != 0
        || length == 0) {
      return false;
    }
    return buffer[0] != '0';
  }();
  return armed;
}
inline void* real_cmd_set_compute_root_signature = nullptr;
inline void* real_cmd_set_compute_root_descriptor_table = nullptr;
inline void* real_cmd_set_compute_root_32bit_constant = nullptr;
inline void* real_cmd_set_compute_root_32bit_constants = nullptr;
inline void* real_cmd_set_compute_root_constant_buffer_view = nullptr;
inline void* real_cmd_set_compute_root_shader_resource_view = nullptr;
inline void* real_cmd_set_compute_root_unordered_access_view = nullptr;
inline std::mutex cmd_hook_mutex;
inline bool cmd_hooks_installed = false;
// Same latch, same reason as ngx_ever_hooked: the funnel's list_hooks_live
// rung asks whether the stage happened in this session, and a teardown
// unhooks the shadow until the next install.
inline std::atomic_bool cmd_hooks_ever_installed = false;
inline bool cmd_hooks_attempted = false;
// Which implementation the list detours sit on, set with cmd_hooks_installed:
// ReShade's proxy command list or d3d12's own (see
// InstallCommandListStateHooks).  The shadow holds what THAT implementation
// is handed, so an evaluate on a list of the other world restores state
// recorded in a different currency - noted once per direction by the
// envelope, observe-only.
inline std::atomic_bool cmd_hooks_on_reshade_proxy = false;
inline std::atomic_bool logged_world_native_list = false;
inline std::atomic_bool logged_world_proxy_list = false;
// Retry state for a failed shadow install (cmd_hook_mutex-guarded).  A
// session-permanent decline turned a transient startup condition (busy
// vtable targets during engine init, a Detours hiccup) into "mod silently
// does nothing"; installs now retry with exponential backoff.  The deficit
// counts evaluates to skip between attempts because every attempt enlists
// the full process thread list and must not run per-evaluate.
inline uint32_t cmd_hook_fail_streak = 0;
inline uint32_t cmd_hook_retry_deficit = 0;

// Re-arms the install for a later evaluate after a failure
// (cmd_hook_mutex-guarded): 1, 3, 7, ... evaluates between attempts, capped
// at 4095 so a permanently-broken vtable retries a few times per minute at
// most while a transient startup condition recovers within a frame or two.
inline void DeferCmdHookRetry() {
  cmd_hook_fail_streak = std::min(cmd_hook_fail_streak + 1u, 12u);
  cmd_hook_retry_deficit = (1u << cmd_hook_fail_streak) - 1u;
  cmd_hooks_attempted = false;
}
// One-shot decline notices. File scope (not function statics): the evaluate
// wrappers are per-slot templates, so function statics would log once PER
// SLOT instead of once per process.
inline std::atomic_bool logged_state_shadow_skip = false;
inline std::atomic_bool logged_state_shadow_skip_c = false;
inline std::atomic_bool logged_clear_state_inject = false;
inline std::atomic_bool logged_presr_state_skip = false;
inline std::atomic_bool logged_streamline_shadow_skip = false;
inline std::atomic_bool logged_state_target_armed = false;
// The missing-aspect set the gate last warned about.  Starts at a value no
// real mask can take (bit 31) so the first decline always warns.
inline std::atomic_uint32_t logged_missing_mask{0x80000000u};
// How many of kCmdHookSlots were PROVEN rerouted at install time.
inline std::atomic_uint32_t cmd_hooks_detoured{0};

// Birth-time observation (R2).  See InstallDeviceStateHooks below for why the
// device, and not ReShade's init_command_list event, is where a list's
// baseline has to come from.
inline void* real_device_create_command_list = nullptr;
inline void* real_device_create_command_signature = nullptr;
inline std::mutex device_hook_mutex;
inline bool device_hooks_installed = false;
inline bool device_hooks_attempted = false;
// Birth-time observation is installed once, from init_device, and until
// v6.8.0-alpha31 a failure there ended it for the whole device generation:
// list baselines fall back to first sight, which is precisely the shape a
// no-seed host (007 First Light, Alan Wake 2 with rs=0) declines on.  The
// present handler retries it on the same schedule as the other two.
inline uint32_t device_hook_attempts = 0;
inline uint64_t device_hook_next_attempt_present = 0;
inline bool signature_hook_installed = false;
// Whether the device hook was PROVEN rerouted at install time.
inline std::atomic_uint32_t device_hooks_detoured{0};
inline std::atomic_uint32_t signature_hook_detoured{0};
// Lists whose compute state is KNOWN from birth rather than from first sight.
inline std::atomic_uint64_t list_birth_baselines{0};
// Command signatures that carry their own description (v6.8.0-alpha8).  A
// title with GPU-driven rendering and zero of these has created its
// signatures before the device hook could install, and its ExecuteIndirect
// calls will all count as unknown_sig.
inline std::atomic_uint64_t command_signatures_described{0};

using CmdResetFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using CmdClearStateFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using CmdSetPipelineStateFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using CmdExecuteBundleFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);
using CmdSetDescriptorHeapsFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using CmdSetComputeRootSignatureFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using CmdSetComputeRootDescriptorTableFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using CmdSetComputeRoot32BitConstantFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using CmdSetComputeRoot32BitConstantsFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
using CmdSetComputeRootViewFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using CmdExecuteIndirectFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*,
    UINT64, ID3D12Resource*, UINT64);
static_assert(
    std::is_same_v<decltype(&ID3D12GraphicsCommandList::ExecuteIndirect),
                   void (STDMETHODCALLTYPE ID3D12GraphicsCommandList::*)(
                       ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64,
                       ID3D12Resource*, UINT64)>,
    "ID3D12GraphicsCommandList::ExecuteIndirect ABI must stay (signature,"
    " max_command_count, argument_buffer, argument_buffer_offset,"
    " count_buffer, count_buffer_offset)");

// ---------------------------------------------------------------------------
// Nothing below may let an exception reach the game
// ---------------------------------------------------------------------------
//
// These functions are called by the game's renderer, with the game's frames
// directly beneath ours.  A C++ exception thrown in our bookkeeping unwinds
// through those frames - through code compiled without exceptions in mind,
// holding D3D12 objects it will never release - and the process dies with the
// mod on the stack.  Every one of them allocates somewhere: the shadow's
// vectors and maps, an ostringstream for a log line, a std::string.
//
// It is not hypothetical.  An archived Alan Wake 2 dump
// (log-preservation/AW2-crashes-9080/RenoDX-DLSS5-crash-35568) is exactly this
// shape: `KERNELBASE!RaiseException` with exception code 0xE06D7363 - the MSVC
// C++ throw - and two `renodx_dlss5` frames beneath it.  Found by
// tools/field/triage-dump.ps1; unreadable in detail, because that build
// predates symbols (B8).
//
// The shadow's own internals were hardened with catch blocks of their own, but
// that is a function-by-function argument and the guarantee wanted here is a
// boundary one: NOTHING escapes a hook, stated once, where the boundary is.
//
// The forward to the real call is deliberately OUTSIDE the guard.  Our
// bookkeeping failing must never turn into the game's call not happening -
// that would be a far worse failure than a stale shadow, which is a state the
// gate already models (the restore declines and the image passes through).
inline std::atomic_uint64_t hook_exceptions = 0;
inline std::atomic_bool logged_hook_exception{false};

// Fault injection, for the one thing a guard cannot be trusted about: that it
// actually catches.  A test asserting `hook_exceptions=0` passes just as well
// against a guard that is never reached, so the harness arms this and checks
// the count, the log line, and - the part that matters - that the frame still
// rendered and the host's compute state is intact.
//
// Read once from the environment, which no user has set and no ini exposes.
// The throw is raised here rather than inside a hook body because the claim
// under test is this function's: catch, count, say so once, and let the
// forward to the real call (which is outside every GuardHook) happen anyway.
inline uint32_t TestThrowHookBudget() {
  static const uint32_t budget = [] {
    char buffer[16] = {};
    size_t length = 0;
    if (getenv_s(&length, buffer, sizeof(buffer), "RENODX_NR_TEST_THROW_HOOKS")
            != 0
        || length == 0) {
      return 0u;
    }
    return static_cast<uint32_t>(strtoul(buffer, nullptr, 10));
  }();
  return budget;
}
inline std::atomic_uint32_t test_throws_remaining{0};
inline std::atomic_bool test_throw_armed{false};

template <typename Work>
inline void GuardHook(Work&& work) noexcept {
  try {
    if (TestThrowHookBudget() != 0) {
      if (!test_throw_armed.exchange(true)) {
        test_throws_remaining.store(TestThrowHookBudget(),
                                    std::memory_order_relaxed);
      }
      uint32_t left = test_throws_remaining.load(std::memory_order_relaxed);
      while (left != 0
             && !test_throws_remaining.compare_exchange_weak(
                    left, left - 1, std::memory_order_acq_rel)) {
      }
      if (left != 0) {
        throw std::runtime_error(
            "RENODX_NR_TEST_THROW_HOOKS: injected hook fault");
      }
    }
    work();
  } catch (...) {
    hook_exceptions.fetch_add(1, std::memory_order_relaxed);
    if (!logged_hook_exception.exchange(true)) {
      Log(reshade::log::level::error,
          "NR hook swallowed a C++ exception: the addon's own bookkeeping threw"
          " inside a call the game made.  The game's call still ran and the"
          " image is untouched; NR may decline until the list rebinds.  Count"
          " is on the telemetry line as hook_exceptions=.");
    }
  }
}

// Every hook below observes HOST state into (a) the armed capture window on
// this thread and (b) the persistent per-list shadow, then forwards to the
// real call.  Both writes go through command_state.hpp's On* methods so the
// shipped state machine and the sentinel tests run identical code.  Our own
// codec/internal-NGX traffic is suppressed from observation (it runs through
// these same detours and would otherwise overwrite the state to restore).

inline HRESULT STDMETHODCALLTYPE HookedCmdReset(
    ID3D12GraphicsCommandList* command_list,
    ID3D12CommandAllocator* allocator,
    ID3D12PipelineState* initial_state) {
  NoteCmdHookEntered(kCmdSlotReset);
  const HRESULT result = reinterpret_cast<CmdResetFn>(real_cmd_reset)(
      command_list, allocator, initial_state);
  GuardHook([&] {
    if (SUCCEEDED(result)) {
      // A successful Reset discards the recording and returns the list to its
      // initial state: the submission-use generation closes (replay
      // impossible; earlier submitted generations keep waiting for their
      // completion proofs - see submission_tracker.hpp), and the shadow
      // records the deterministic post-Reset baseline (PSO = the argument,
      // everything else unbound) instead of dropping to unknown.
      submission::OnCommandListReset(command_list);
      WriteShadowBaseline(command_list, initial_state, "ResetBaseline");
    } else {
      // A failed Reset leaves the list state undefined from the observer's
      // perspective; unknown is the only safe shadow.
      EraseListShadow(command_list);
    }
  });
  return result;
}

inline void STDMETHODCALLTYPE HookedCmdClearState(
    ID3D12GraphicsCommandList* command_list,
    ID3D12PipelineState* pipeline_state) {
  NoteCmdHookEntered(kCmdSlotClearState);
  GuardHook([&] {
    if (!SuppressCommandStateObservation()) {
      // ClearState unbinds everything deterministically; both the shadow and
      // every matching capture window (windows nest through NGX module
      // forwarding) record that exact aftermath as KNOWN state, so a later
      // host rebind simply updates it (audit issue 02: the old coarse flag
      // forced a null restore even after valid rebinds).
      WriteShadowBaseline(command_list, pipeline_state, "ClearStateBaseline");
      for (CapturedComputeState* capture = compute_state_capture;
           capture != nullptr; capture = capture->outer_capture) {
        if (capture->command_list == nullptr
            || capture->command_list == command_list) {
          capture->OnClearState(pipeline_state);
          capture->observed_clear_state = true;
        }
      }
    }
  });
  reinterpret_cast<CmdClearStateFn>(real_cmd_clear_state)(
      command_list, pipeline_state);
}

inline void STDMETHODCALLTYPE HookedCmdSetPipelineState(
    ID3D12GraphicsCommandList* command_list,
    ID3D12PipelineState* pipeline_state) {
  NoteCmdHookEntered(kCmdSlotSetPipelineState);
  GuardHook([&] {
    if (!SuppressCommandStateObservation()) {
      RecordShadowPipelineState(command_list, pipeline_state);
      if (auto* capture = compute_state_capture) {
        if (capture->command_list == nullptr || capture->command_list == command_list) {
          capture->OnSetPipelineState(pipeline_state);
        }
      }
    }
  });
  reinterpret_cast<CmdSetPipelineStateFn>(real_cmd_set_pipeline_state)(
      command_list, pipeline_state);
}

// A bundle's bindings persist into the parent list after ExecuteBundle, and
// the bundle recorded them on its own object, so they are absent from this
// list's shadow.  Injecting under that stale shadow restored the pre-bundle
// pipeline over the host's live one and the host's next dispatch ran the wrong
// shader - measured on the trunk, 119 of 120 frames (T2 `bundles`).  Dropping
// the knowledge bits declines the injection instead, and any host re-bind
// completes the target again without needing a Reset.
inline void STDMETHODCALLTYPE HookedCmdExecuteBundle(
    ID3D12GraphicsCommandList* command_list,
    ID3D12GraphicsCommandList* bundle) {
  NoteCmdHookEntered(kCmdSlotExecuteBundle);
  GuardHook([&] {
    if (!SuppressCommandStateObservation()) {
      const BundleDelta* delta =
          RecordShadowExecutedBundle(command_list, bundle);
      for (CapturedComputeState* capture = compute_state_capture;
           capture != nullptr; capture = capture->outer_capture) {
        if (capture->command_list == nullptr
            || capture->command_list == command_list) {
          // A capture window sees the same bundle the shadow did: when the
          // recording was observed the window takes its delta, so a bundle
          // executed inside an evaluate does not void the restore either.
          if (delta != nullptr) {
            capture->MergeExecutedBundle(*delta);
          } else {
            capture->OnExecuteBundle();
          }
        }
      }
    }
  });
  reinterpret_cast<CmdExecuteBundleFn>(real_cmd_execute_bundle)(
      command_list, bundle);
}

// The GUID under which a command signature carries its own description.
//
// The alternative was a process-wide pointer-keyed map, which is what v6.7.4
// built: it took a global mutex on a path Alan Wake 2 calls ~57k times per
// telemetry window, and it was never pruned, so a released signature's
// address could be reused and answer for the new one.  Private data has
// neither problem - the description lives on the object, dies with it, and
// needs no lock of ours - and ID3D12CommandSignature inherits SetPrivateData
// from ID3D12Object like every other device child.
// {0FB6B5B7-4A6C-4F0E-9E35-6E7A1D9C2E41}, per loaded copy (module_guid.hpp):
// another version's description may have another layout.
inline const GUID kIndirectSignatureEffectGuid = ModuleScopedGuid(
    {0x0fb6b5b7, 0x4a6c, 0x4f0e, {0x9e, 0x35, 0x6e, 0x7a, 0x1d, 0x9c, 0x2e, 0x41}});

// Bumped by every CreateCommandSignature in the process, which invalidates
// every thread's memo below.  Detours patches the function BODY, not a
// vtable entry, so one install covers every ID3D12DeviceN interface and
// every device object: there is no second path by which a command signature
// can come into existence unseen, and that is what makes a pointer-keyed
// memo sound here.  Without it, a signature could be destroyed and a new one
// allocated at the same address, and the memo would answer for the wrong
// one.
inline std::atomic_uint64_t command_signature_epoch{0};

enum class IndirectEffectClass : std::uint8_t {
  kUnknown,  // no description, or one this model will not act on
  kInert,    // cannot change a compute root argument
  kApplies,  // names compute root arguments; the aftermath is exact
};

// Per-thread answer cache.  ExecuteIndirect is the hottest call a GPU-driven
// engine makes - Alan Wake 2 issues ~57k per telemetry window - and the
// obvious implementation, GetPrivateData per call, takes the signature
// object's own runtime lock.  Engines share one signature across every
// recording thread, so that lock would serialise all of them on the exact
// path v6.7.4 was faulted for taking a global mutex on.  Measured on the T2
// `gpu_driven_indirect` profile: 89 ns per call through GetPrivateData,
// single-threaded, before any contention.
//
// Four entries, direct-mapped: engines issue long runs of one signature and
// alternate between a handful.  Nothing here holds a COM reference - a memo
// that AddRef'd would keep a game object alive past its release, which is
// the lifetime class this addon has already shipped a crash for.
constexpr unsigned kIndirectMemoEntries = 4;

struct IndirectMemoEntry {
  const void* signature = nullptr;
  IndirectEffectClass effect_class = IndirectEffectClass::kUnknown;
  IndirectSignatureEffect effect;
};

struct IndirectThreadState {
  uint64_t epoch = ~0ull;
  IndirectMemoEntry entries[kIndirectMemoEntries];
  // Counter traffic, batched.  A shared atomic incremented on every indirect
  // is itself a contended line across recording threads; these publish in
  // blocks instead.  The two classes that matter for a diagnosis - a signature
  // we could not describe, and one whose aftermath we applied - publish
  // immediately, so nothing a reader acts on is ever stale.
  uint32_t total = 0;
  uint32_t applied = 0;
  uint32_t inert = 0;
  uint32_t unknown_sig = 0;
};

inline thread_local IndirectThreadState indirect_thread_state;
constexpr uint32_t kIndirectTallyFlush = 256;

inline void FlushIndirectTally(IndirectThreadState& state) {
  if (state.total != 0) {
    list_shadow_indirects.fetch_add(state.total, std::memory_order_relaxed);
    state.total = 0;
  }
  if (state.applied != 0) {
    list_shadow_indirects_applied.fetch_add(state.applied,
                                            std::memory_order_relaxed);
    state.applied = 0;
  }
  if (state.inert != 0) {
    list_shadow_indirects_inert.fetch_add(state.inert, std::memory_order_relaxed);
    state.inert = 0;
  }
  if (state.unknown_sig != 0) {
    list_shadow_indirects_unknown_sig.fetch_add(state.unknown_sig,
                                                std::memory_order_relaxed);
    state.unknown_sig = 0;
  }
}

// Resolves what a signature names, through the memo.  The only call that can
// reach the D3D12 runtime is the one that fills a memo slot.
inline const IndirectMemoEntry& LookupIndirectEffect(
    IndirectThreadState& state, ID3D12CommandSignature* signature) {
  const uint64_t epoch =
      command_signature_epoch.load(std::memory_order_acquire);
  if (state.epoch != epoch) {
    state.epoch = epoch;
    for (IndirectMemoEntry& entry : state.entries) entry.signature = nullptr;
  }
  IndirectMemoEntry& entry = state.entries
      [(reinterpret_cast<uintptr_t>(signature) >> 4) % kIndirectMemoEntries];
  if (entry.signature == signature) return entry;
  entry.signature = signature;
  entry.effect_class = IndirectEffectClass::kUnknown;
  UINT size = sizeof(IndirectSignatureEffect);
  if (signature != nullptr
      && SUCCEEDED(signature->GetPrivateData(kIndirectSignatureEffectGuid, &size,
                                             &entry.effect))
      && size == sizeof(IndirectSignatureEffect) && entry.effect.complete) {
    entry.effect_class = entry.effect.NoComputeEffect()
                             ? IndirectEffectClass::kInert
                             : IndirectEffectClass::kApplies;
  }
  return entry;
}

// "No command signature state leaks back to the command list" (MS, Indirect
// Drawing).  The shadow applies exactly the arguments the signature names,
// which is the only reading that keeps both halves: a GPU-driven engine
// still engages (v6.7.0's blanket invalidation declined 912/912 evaluates in
// Alan Wake 2), and a host that relies on the documented zero is not handed
// our stale observation instead (v6.5.3 corrupted 239 of 240 frames in the
// T2 `gpu_driven_indirect` profile).
inline void STDMETHODCALLTYPE HookedCmdExecuteIndirect(
    ID3D12GraphicsCommandList* command_list,
    ID3D12CommandSignature* command_signature,
    UINT max_command_count,
    ID3D12Resource* argument_buffer,
    UINT64 argument_buffer_offset,
    ID3D12Resource* count_buffer,
    UINT64 count_buffer_offset) {
  NoteCmdHookEntered(kCmdSlotExecuteIndirect);
  GuardHook([&] {
    if (!SuppressCommandStateObservation()) {
      IndirectThreadState& state = indirect_thread_state;
      ++state.total;
      const IndirectMemoEntry& entry =
          LookupIndirectEffect(state, command_signature);
      switch (entry.effect_class) {
        case IndirectEffectClass::kInert:
          // A draw-style indirect, or a dispatch that names no root argument.
          // Nothing to do at all, which is the whole point of reading the
          // signature: this is the bulk of a GPU-driven engine's volume.
          ++state.inert;
          if (state.total >= kIndirectTallyFlush) FlushIndirectTally(state);
          break;
        case IndirectEffectClass::kApplies:
          ++state.applied;
          RecordShadowExecuteIndirect(command_list, entry.effect);
          for (CapturedComputeState* capture = compute_state_capture;
               capture != nullptr; capture = capture->outer_capture) {
            if (capture->command_list == nullptr
                || capture->command_list == command_list) {
              capture->OnExecuteIndirect(entry.effect);
            }
          }
          FlushIndirectTally(state);
          break;
        case IndirectEffectClass::kUnknown:
          // No description (the signature predates the device hook), or one the
          // model will not act on.  Leaving the shadow alone keeps engagement,
          // and this is the counter that says how often the exact aftermath did
          // not reach this title - so it publishes at once.
          ++state.unknown_sig;
          FlushIndirectTally(state);
          break;
      }
    }
  });
  reinterpret_cast<CmdExecuteIndirectFn>(real_cmd_execute_indirect)(
      command_list, command_signature, max_command_count, argument_buffer,
      argument_buffer_offset, count_buffer, count_buffer_offset);
}

inline void STDMETHODCALLTYPE HookedCmdSetDescriptorHeaps(
    ID3D12GraphicsCommandList* command_list,
    UINT num_descriptor_heaps,
    ID3D12DescriptorHeap* const* descriptor_heaps) {
  NoteCmdHookEntered(kCmdSlotSetDescriptorHeaps);
  GuardHook([&] {
    if (!SuppressCommandStateObservation()) {
      RecordShadowHeaps(command_list, num_descriptor_heaps, descriptor_heaps);
      if (HeapTraceArmed()
          && !heap_trace_record_logged.exchange(true)) {
        std::ostringstream trace;
        trace << "heap trace: record list=0x" << std::hex
              << reinterpret_cast<uintptr_t>(command_list) << " heaps="
              << std::dec << num_descriptor_heaps;
        for (UINT i = 0; i < num_descriptor_heaps && i < 2; ++i) {
          trace << " 0x" << std::hex
                << reinterpret_cast<uintptr_t>(descriptor_heaps[i]);
        }
        Log(reshade::log::level::info, trace.str());
      }
      if (auto* capture = compute_state_capture) {
        if (capture->command_list == nullptr || capture->command_list == command_list) {
          // Return value discarded on purpose: RecordShadowHeaps above saw the
          // same call and counted the drop, and counting it twice would make
          // the telemetry read as two host heap changes where there was one.
          (void)capture->OnSetDescriptorHeaps(
              num_descriptor_heaps, descriptor_heaps);
        }
      }
    }
  });
  reinterpret_cast<CmdSetDescriptorHeapsFn>(real_cmd_set_descriptor_heaps)(
      command_list, num_descriptor_heaps, descriptor_heaps);
}

inline void STDMETHODCALLTYPE HookedCmdSetComputeRootSignature(
    ID3D12GraphicsCommandList* command_list,
    ID3D12RootSignature* root_signature) {
  NoteCmdHookEntered(kCmdSlotSetComputeRootSignature);
  GuardHook([&] {
    if (!SuppressCommandStateObservation()) {
      RecordShadowComputeRootSignature(command_list, root_signature);
      if (auto* capture = compute_state_capture) {
        if (capture->command_list == nullptr || capture->command_list == command_list) {
          // An identical-signature rebind preserves bound root arguments in
          // D3D12; OnSetComputeRootSignature only invalidates them on a layout
          // change (audit issue 03).
          capture->OnSetComputeRootSignature(root_signature);
        }
      }
    }
  });
  reinterpret_cast<CmdSetComputeRootSignatureFn>(
      real_cmd_set_compute_root_signature)(command_list, root_signature);
}

inline void STDMETHODCALLTYPE HookedCmdSetComputeRootDescriptorTable(
    ID3D12GraphicsCommandList* command_list,
    UINT root_parameter_index,
    D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) {
  NoteCmdHookEntered(kCmdSlotSetComputeRootDescriptorTable);
  GuardHook([&] {
    if (!SuppressCommandStateObservation()) {
      RecordShadowRootArguments(
          command_list, root_parameter_index,
          &ComputeStateFields::OnSetComputeRootDescriptorTable, base_descriptor.ptr);
      if (auto* capture = compute_state_capture) {
        if (capture->command_list == nullptr || capture->command_list == command_list) {
          capture->OnSetComputeRootDescriptorTable(
              root_parameter_index, base_descriptor.ptr);
        }
      }
    }
  });
  reinterpret_cast<CmdSetComputeRootDescriptorTableFn>(
      real_cmd_set_compute_root_descriptor_table)(
          command_list, root_parameter_index, base_descriptor);
}

// The singular-constant setter, back in the production hook set in 7.0.0
// (see kCmdHookSlots for its history).  Without it a host constant set one
// DWORD at a time was invisible to the shadow and the restore replayed the
// stale plural value.  The NGX runtime's own constants never reach the
// shadow: SuppressCommandStateObservation (NgxRuntimeCommandScope).
inline void STDMETHODCALLTYPE HookedCmdSetComputeRoot32BitConstant(
    ID3D12GraphicsCommandList* command_list,
    UINT root_parameter_index,
    UINT src_data,
    UINT dest_offset) {
  NoteCmdHookEntered(kCmdSlotSetComputeRoot32BitConstant);
  GuardHook([&] {
    if (!SuppressCommandStateObservation()) {
      RecordShadowRootConstants(
          command_list, root_parameter_index, 1, &src_data, dest_offset);
      if (auto* capture = compute_state_capture) {
        if (capture->command_list == nullptr || capture->command_list == command_list) {
          capture->OnSetComputeRoot32BitConstants(
              root_parameter_index, 1, &src_data, dest_offset);
        }
      }
    }
  });
  reinterpret_cast<CmdSetComputeRoot32BitConstantFn>(
      real_cmd_set_compute_root_32bit_constant)(
          command_list, root_parameter_index, src_data, dest_offset);
}

inline void STDMETHODCALLTYPE HookedCmdSetComputeRoot32BitConstants(
    ID3D12GraphicsCommandList* command_list,
    UINT root_parameter_index,
    UINT num_32bit_values_to_set,
    const void* src_data,
    UINT dest_offset) {
  NoteCmdHookEntered(kCmdSlotSetComputeRoot32BitConstants);
  GuardHook([&] {
    if (!SuppressCommandStateObservation()) {
      RecordShadowRootConstants(
          command_list, root_parameter_index, num_32bit_values_to_set, src_data,
          dest_offset);
      if (auto* capture = compute_state_capture) {
        if (capture->command_list == nullptr || capture->command_list == command_list) {
          capture->OnSetComputeRoot32BitConstants(
              root_parameter_index, num_32bit_values_to_set, src_data, dest_offset);
        }
      }
    }
  });
  reinterpret_cast<CmdSetComputeRoot32BitConstantsFn>(
      real_cmd_set_compute_root_32bit_constants)(
          command_list,
          root_parameter_index,
          num_32bit_values_to_set,
          src_data,
          dest_offset);
}

inline void STDMETHODCALLTYPE HookedCmdSetComputeRootConstantBufferView(
    ID3D12GraphicsCommandList* command_list,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location) {
  NoteCmdHookEntered(kCmdSlotSetComputeRootConstantBufferView);
  GuardHook([&] {
    if (!SuppressCommandStateObservation()) {
      RecordShadowRootArguments(
          command_list, root_parameter_index,
          &ComputeStateFields::OnSetComputeRootConstantBufferView, buffer_location);
      if (auto* capture = compute_state_capture) {
        if (capture->command_list == nullptr || capture->command_list == command_list) {
          capture->OnSetComputeRootConstantBufferView(
              root_parameter_index, buffer_location);
        }
      }
    }
  });
  reinterpret_cast<CmdSetComputeRootViewFn>(
      real_cmd_set_compute_root_constant_buffer_view)(
          command_list, root_parameter_index, buffer_location);
}

inline void STDMETHODCALLTYPE HookedCmdSetComputeRootShaderResourceView(
    ID3D12GraphicsCommandList* command_list,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location) {
  NoteCmdHookEntered(kCmdSlotSetComputeRootShaderResourceView);
  GuardHook([&] {
    if (!SuppressCommandStateObservation()) {
      RecordShadowRootArguments(
          command_list, root_parameter_index,
          &ComputeStateFields::OnSetComputeRootShaderResourceView, buffer_location);
      if (auto* capture = compute_state_capture) {
        if (capture->command_list == nullptr || capture->command_list == command_list) {
          capture->OnSetComputeRootShaderResourceView(
              root_parameter_index, buffer_location);
        }
      }
    }
  });
  reinterpret_cast<CmdSetComputeRootViewFn>(
      real_cmd_set_compute_root_shader_resource_view)(
          command_list, root_parameter_index, buffer_location);
}

inline void STDMETHODCALLTYPE HookedCmdSetComputeRootUnorderedAccessView(
    ID3D12GraphicsCommandList* command_list,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location) {
  NoteCmdHookEntered(kCmdSlotSetComputeRootUnorderedAccessView);
  GuardHook([&] {
    if (!SuppressCommandStateObservation()) {
      RecordShadowRootArguments(
          command_list, root_parameter_index,
          &ComputeStateFields::OnSetComputeRootUnorderedAccessView, buffer_location);
      if (auto* capture = compute_state_capture) {
        if (capture->command_list == nullptr || capture->command_list == command_list) {
          capture->OnSetComputeRootUnorderedAccessView(
              root_parameter_index, buffer_location);
        }
      }
    }
  });
  reinterpret_cast<CmdSetComputeRootViewFn>(
      real_cmd_set_compute_root_unordered_access_view)(
          command_list, root_parameter_index, buffer_location);
}

// Lifetime untracking (audit issue 04: destruction must close shadow entries
// and submission generations) is deliberately NOT a raw IUnknown::Release
// detour.  The v6audit3 field build (2026-09-16) hooked Release - the hottest
// COM method on these objects, executed by every thread in the process - and
// the same build froze KCD2's first NR frame (GPU TDR) and hung AW2 inside
// the signed runtime's feature create; a KCD2 boot-canary bisect over the
// deployed backup trail isolated the freeze to exactly this build, whose only
// hook-set delta was Release + the singular root-constant setter.  Installing
// a Release detour mid-frame also patches a function body other threads are
// executing, the same unsynchronized-patch hazard as the 08:09 boot freezes.
// Destruction is tracked through ReShade's own destroy_command_list event
// instead (OnDestroyCommandListEvent below), which ReShade serializes with
// its device lifetime and which never detours a hot COM method.  That
// sentence was aspirational until v6.8.0: the handler was written and the
// event was never registered, so no build of this line learned that a list
// had died.  See the handler for what it cost and for the three things that
// had to be true before it could be turned on.

inline void LogShadowStripeParked(const char* site, unsigned cooldown_ms) {
  char message[192];
  std::snprintf(
      message, sizeof(message),
      "list-compute shadow stripe parked for a %ums cooldown: lock unavailable"
      " at %s tid=%lu (entries drop until re-observed)",
      cooldown_ms, site,
      static_cast<unsigned long>(GetCurrentThreadId()));
  Log(reshade::log::level::warning, message);
}

// One-time install, resolved from the vtable of the first command list an
// evaluate hands us (NGX export or Streamline fallback).  Detouring the
// function body (not a vtable entry) covers every instance and every
// ID3D12GraphicsCommandListN version OF THAT IMPLEMENTATION - and under
// ReShade there are two.  The game holds ReShade's proxy lists, so the first
// list is usually a proxy and the detours land on ReShade's
// D3D12GraphicsCommandList methods: they see what the game hands the proxy,
// heap proxies included (the full add-on build wraps CBV_SRV_UAV and SAMPLER
// heaps), and they do NOT see calls made straight on the native list, which
// is what the Streamline layer injects on after unwrapping.  When the first
// list is native, the detours land on d3d12's own methods, which the proxies
// forward into with native arguments.  (Until v7.0.0-alpha47 this comment
// said the body detour intercepts proxy calls in every case; that holds only
// for the second shape.)  cmd_hooks_on_reshade_proxy records which, the
// envelope logs an evaluate from the other world, and the restore unwraps
// shadowed heaps for a native list (ApplyCapturedComputeState).
inline bool InstallCommandListStateHooks(ID3D12GraphicsCommandList* command_list) {
  // The nullptr sentinel (re-entrant wrapper entries pass no list) must be
  // checked before BOTH fast paths: returning true for nullptr would arm a
  // capture window whose restore dereferences a null list, and consuming the
  // one-shot attempt on a null list would poison the install for the session.
  if (command_list == nullptr) return false;
  OnShadowStripeParked = &LogShadowStripeParked;
  if (cmd_hooks_installed) return true;
  std::lock_guard<std::mutex> lock(cmd_hook_mutex);
  if (cmd_hooks_attempted) return cmd_hooks_installed;
  if (cmd_hook_retry_deficit != 0) {
    --cmd_hook_retry_deficit;
    return false;
  }
  cmd_hooks_attempted = true;
  void** vtable = *reinterpret_cast<void***>(command_list);
  // One list drives both: every slot in kCmdHookSlots is resolved here and
  // validated below, so a new hook can never be half-wired.
  void* targets[kCmdSlotMax + 1] = {};
  for (const std::uint32_t slot : kCmdHookSlots) {
    targets[slot] = vtable[slot];
  }
  // Validate and consume exactly the hooked slots: `targets` is
  // slot-index-sized and sparse by construction, so scanning the whole array
  // would read unfilled entries as unresolved (the v5.3.0-dev field
  // regression that declined the shadow on every game).
  for (std::uint32_t slot : kCmdHookSlots) {
    if (targets[slot] == nullptr) {
      Log(reshade::log::level::warning,
          "compute-state shadow: command list vtable slots unresolved; NR "
          "declines frames until the shadow installs (retrying with backoff)");
      DeferCmdHookRetry();
      return false;
    }
  }
  real_cmd_reset = targets[kCmdSlotReset];
  real_cmd_clear_state = targets[kCmdSlotClearState];
  real_cmd_set_pipeline_state = targets[kCmdSlotSetPipelineState];
  real_cmd_execute_bundle = targets[kCmdSlotExecuteBundle];
  real_cmd_set_descriptor_heaps = targets[kCmdSlotSetDescriptorHeaps];
  real_cmd_set_compute_root_signature =
      targets[kCmdSlotSetComputeRootSignature];
  real_cmd_set_compute_root_descriptor_table =
      targets[kCmdSlotSetComputeRootDescriptorTable];
  real_cmd_set_compute_root_32bit_constant =
      targets[kCmdSlotSetComputeRoot32BitConstant];
  real_cmd_set_compute_root_32bit_constants =
      targets[kCmdSlotSetComputeRoot32BitConstants];
  real_cmd_set_compute_root_constant_buffer_view =
      targets[kCmdSlotSetComputeRootConstantBufferView];
  real_cmd_set_compute_root_shader_resource_view =
      targets[kCmdSlotSetComputeRootShaderResourceView];
  real_cmd_set_compute_root_unordered_access_view =
      targets[kCmdSlotSetComputeRootUnorderedAccessView];
  real_cmd_execute_indirect = targets[kCmdSlotExecuteIndirect];
  std::lock_guard<std::mutex> transaction(
      renodx::utils::vtable::TransactionMutex());
  bool detours_installed = renodx::utils::vtable::BeginTransaction();
  if (detours_installed) {
    DetourUpdateThread(GetCurrentThread());
    detours_installed =
        DetourAttach(&real_cmd_reset, &HookedCmdReset) == NO_ERROR
        && DetourAttach(&real_cmd_clear_state, &HookedCmdClearState) == NO_ERROR
        && DetourAttach(&real_cmd_set_pipeline_state, &HookedCmdSetPipelineState)
               == NO_ERROR
        && DetourAttach(&real_cmd_execute_bundle, &HookedCmdExecuteBundle)
               == NO_ERROR
        && DetourAttach(&real_cmd_set_descriptor_heaps,
                        &HookedCmdSetDescriptorHeaps)
               == NO_ERROR
        && DetourAttach(&real_cmd_set_compute_root_signature,
                        &HookedCmdSetComputeRootSignature)
               == NO_ERROR
        && DetourAttach(&real_cmd_set_compute_root_descriptor_table,
                        &HookedCmdSetComputeRootDescriptorTable)
               == NO_ERROR
        && DetourAttach(&real_cmd_set_compute_root_32bit_constant,
                        &HookedCmdSetComputeRoot32BitConstant)
               == NO_ERROR
        && DetourAttach(&real_cmd_set_compute_root_32bit_constants,
                        &HookedCmdSetComputeRoot32BitConstants)
               == NO_ERROR
        && DetourAttach(&real_cmd_set_compute_root_constant_buffer_view,
                        &HookedCmdSetComputeRootConstantBufferView)
               == NO_ERROR
        && DetourAttach(&real_cmd_set_compute_root_shader_resource_view,
                        &HookedCmdSetComputeRootShaderResourceView)
               == NO_ERROR
        && DetourAttach(&real_cmd_set_compute_root_unordered_access_view,
                        &HookedCmdSetComputeRootUnorderedAccessView)
               == NO_ERROR
        && DetourAttach(&real_cmd_execute_indirect, &HookedCmdExecuteIndirect)
               == NO_ERROR
        && DetourTransactionCommit() == NO_ERROR;
  }
  if (!detours_installed) {
    DetourTransactionAbort();
    real_cmd_reset = nullptr;
    real_cmd_clear_state = nullptr;
    real_cmd_set_pipeline_state = nullptr;
    real_cmd_execute_bundle = nullptr;
    real_cmd_set_descriptor_heaps = nullptr;
    real_cmd_set_compute_root_signature = nullptr;
    real_cmd_set_compute_root_descriptor_table = nullptr;
    real_cmd_set_compute_root_32bit_constant = nullptr;
    real_cmd_set_compute_root_32bit_constants = nullptr;
    real_cmd_set_compute_root_constant_buffer_view = nullptr;
    real_cmd_set_compute_root_shader_resource_view = nullptr;
    real_cmd_set_compute_root_unordered_access_view = nullptr;
    real_cmd_execute_indirect = nullptr;
    Log(reshade::log::level::warning,
        "compute-state shadow: Detours install failed; NR declines frames "
        "until the shadow installs (retrying with backoff)");
    DeferCmdHookRetry();
    return false;
  }
  cmd_hook_fail_streak = 0;
  ID3D12GraphicsCommandList* native_list = command_list;
  cmd_hooks_on_reshade_proxy.store(
      renodx::utils::directx::NativeFromReShadeProxy(&native_list),
      std::memory_order_relaxed);
  cmd_hooks_installed = true;

  // Install-time positive control, per hook (R2).  A commit that returns
  // NO_ERROR says the transaction succeeded, not that every function was
  // actually rerouted - and "the hooks installed" was believed for six
  // releases while NR engaged on nothing.  Detours rewrites each pointer it
  // attaches to point at its trampoline, so a pointer still equal to the
  // address read out of the vtable is a hook that did not take.  Counting
  // them costs eleven comparisons once per session and turns "installed"
  // from an assumption into a measurement that the load line reports.
  unsigned detoured = 0;
  const void* const post_commit[] = {
      real_cmd_reset,
      real_cmd_clear_state,
      real_cmd_set_pipeline_state,
      real_cmd_execute_bundle,
      real_cmd_set_descriptor_heaps,
      real_cmd_set_compute_root_signature,
      real_cmd_set_compute_root_descriptor_table,
      real_cmd_set_compute_root_32bit_constant,
      real_cmd_set_compute_root_32bit_constants,
      real_cmd_set_compute_root_constant_buffer_view,
      real_cmd_set_compute_root_shader_resource_view,
      real_cmd_set_compute_root_unordered_access_view,
      real_cmd_execute_indirect,
  };
  static_assert(
      std::size(post_commit) == std::size(kCmdHookSlots),
      "the positive control must cover exactly the hooked slots - a hook "
      "missing from this list is a hook nothing proves fires");
  for (size_t i = 0; i < std::size(post_commit); ++i) {
    if (post_commit[i] != nullptr
        && post_commit[i] != targets[kCmdHookSlots[i]]) {
      ++detoured;
    }
  }
  cmd_hooks_detoured = detoured;
  char installed[320];
  std::snprintf(
      installed, sizeof(installed),
      "compute-state shadow installed on the D3D12 command list vtable:"
      " %u of %zu hooks verified detoured (Reset, ClearState,"
      " SetPipelineState, ExecuteBundle, SetDescriptorHeaps,"
      " SetComputeRootSignature, root descriptor tables, root constants"
      " plural, root CBV/SRV/UAV, ExecuteIndirect; no Release detour and no"
      " list-destruction event - v6audit2 field-proven config; see"
      " OnDestroyCommandListEvent and cmd_slots.hpp)",
      detoured, std::size(kCmdHookSlots));
  // Anything short of all of them is a partial shadow, which is the state
  // that produces "NR is on and nothing happens".
  Log(
      detoured == std::size(kCmdHookSlots) ? reshade::log::level::info
                                           : reshade::log::level::warning,
      installed);
  Log(reshade::log::level::info,
      cmd_hooks_on_reshade_proxy.load(std::memory_order_relaxed)
          ? "compute-state shadow world: ReShade's proxy command list - the"
            " shadow records what the game hands its proxy, descriptor-heap"
            " proxies included"
          : "compute-state shadow world: the native D3D12 command list -"
            " ReShade's proxies forward into it, so the shadow records native"
            " descriptor heaps");
  return true;
}

inline void UnhookCommandListStateHooks() {
  std::lock_guard<std::mutex> lock(cmd_hook_mutex);
  if (!cmd_hooks_installed) return;
  std::lock_guard<std::mutex> transaction(
      renodx::utils::vtable::TransactionMutex());
  bool detours_detached = renodx::utils::vtable::BeginTransaction();
  if (detours_detached) {
    DetourUpdateThread(GetCurrentThread());
    detours_detached =
        DetourDetach(&real_cmd_reset, &HookedCmdReset) == NO_ERROR
        && DetourDetach(&real_cmd_clear_state, &HookedCmdClearState) == NO_ERROR
        && DetourDetach(&real_cmd_set_pipeline_state, &HookedCmdSetPipelineState)
               == NO_ERROR
        && DetourDetach(&real_cmd_execute_bundle, &HookedCmdExecuteBundle)
               == NO_ERROR
        && DetourDetach(&real_cmd_set_descriptor_heaps,
                        &HookedCmdSetDescriptorHeaps)
               == NO_ERROR
        && DetourDetach(&real_cmd_set_compute_root_signature,
                        &HookedCmdSetComputeRootSignature)
               == NO_ERROR
        && DetourDetach(&real_cmd_set_compute_root_descriptor_table,
                        &HookedCmdSetComputeRootDescriptorTable)
               == NO_ERROR
        && DetourDetach(&real_cmd_set_compute_root_32bit_constant,
                        &HookedCmdSetComputeRoot32BitConstant)
               == NO_ERROR
        && DetourDetach(&real_cmd_set_compute_root_32bit_constants,
                        &HookedCmdSetComputeRoot32BitConstants)
               == NO_ERROR
        && DetourDetach(&real_cmd_set_compute_root_constant_buffer_view,
                        &HookedCmdSetComputeRootConstantBufferView)
               == NO_ERROR
        && DetourDetach(&real_cmd_set_compute_root_shader_resource_view,
                        &HookedCmdSetComputeRootShaderResourceView)
               == NO_ERROR
        && DetourDetach(&real_cmd_set_compute_root_unordered_access_view,
                        &HookedCmdSetComputeRootUnorderedAccessView)
               == NO_ERROR
        && DetourDetach(&real_cmd_execute_indirect, &HookedCmdExecuteIndirect)
               == NO_ERROR
        && DetourTransactionCommit() == NO_ERROR;
  }
  if (detours_detached) {
    // Reached only from DetachLite: the list detours are function-body
    // patches on code that outlives any device, so a device-rebuild teardown
    // keeps them (and the shadow) installed.  Detaching them there nulled
    // every real_cmd_* under a game thread still inside a hook body, which
    // then called through null.
    cmd_hooks_installed = false;
    cmd_hooks_attempted = false;
    real_cmd_reset = nullptr;
    real_cmd_clear_state = nullptr;
    real_cmd_set_pipeline_state = nullptr;
    real_cmd_execute_bundle = nullptr;
    real_cmd_set_descriptor_heaps = nullptr;
    real_cmd_set_compute_root_signature = nullptr;
    real_cmd_set_compute_root_descriptor_table = nullptr;
    real_cmd_set_compute_root_32bit_constant = nullptr;
    real_cmd_set_compute_root_32bit_constants = nullptr;
    real_cmd_set_compute_root_constant_buffer_view = nullptr;
    real_cmd_set_compute_root_shader_resource_view = nullptr;
    real_cmd_set_compute_root_unordered_access_view = nullptr;
    real_cmd_execute_indirect = nullptr;
  } else {
    DetourTransactionAbort();
  }
}

// ---------------------------------------------------------------------------
// Birth-time observation (R2)
// ---------------------------------------------------------------------------
//
// The shadow can only restore state it KNOWS, and a list first seen part-way
// through its recording is a list that may already have had a root signature,
// descriptor heaps and root arguments bound.  Nothing distinguishes that from
// a list on which nothing was ever bound, so the restore target never
// completes and every evaluate declines - correctly, and forever.  That is
// the no-seed shape: 007 First Light and AW2 with rs=0 report 100 % of
// evaluates declining on `state_target`, `missing=root_signature`, on hosts
// that were never going to bind anything before the evaluate in the first
// place.  The T2 `no_seed` profile reproduces it exactly.
//
// Watching the list be born settles the question.  CreateCommandList returns
// a list ALREADY RECORDING in the aftermath of a Reset (MS,
// "ID3D12Device::CreateCommandList": the list is created in the recording
// state, and Reset's own documentation gives that state as no descriptor
// heaps, no root signature, no root arguments, pipeline state = the
// pInitialState argument).  Every aspect the restore has to write is
// therefore KNOWN from the list's first instruction, and "we observed
// nothing bound" becomes "nothing was bound".
//
// The hook has to be on the device call, not on ReShade's init_command_list
// event, which fires at the same moment but does not carry pInitialState.  A
// baseline that guessed the pipeline state would restore a pipeline the host
// never bound - strictly worse than declining, and against the one rule this
// rehab is built on.
//
// What this does NOT do is move where the command-list detours are installed,
// and that limit was measured, not assumed.  Installing them here first -
// from the vtable of the list CreateCommandList just returned - looked like
// it also closed the install race (R7) for free.  It does not: the list this
// hook receives is the NATIVE one, while the object the addon restores
// through is the one the host holds and hands to NGX, which under ReShade is
// its wrapper.  Detouring the native vtable made the shadow record native
// descriptor-heap pointers and the restore replay them through the wrapper's
// SetDescriptorHeaps, which reads each pointer as one of its own wrappers.
// Measured on the T2 host: rebind_all, no_rebind, cry_heapswap and bundles
// all died on the first injection, an access violation in
// CGraphicsCommandList::SetDescriptorHeaps on 0xbaadf00d.  A trace of the two
// builds says it plainly - before, every record and every replay names one
// list pointer; after, they name two.
// The shadow itself joins the two worlds correctly and always did: in the
// default object-attached mode the entry lives on the list through
// SetPrivateDataInterface, and D3D12 proxies forward private data to the
// native object, so a baseline written here on the native list IS the entry
// the wrapper resolves.  That is why the baseline alone is enough, and why
// which vtable is detoured has to stay exactly where the evaluate put it.
// Choosing that implementation deliberately is its own change, with its own
// measurement (R8: "which list implementation gets detoured depends on the
// first list seen").

using DeviceCreateCommandListFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
    ID3D12PipelineState*, REFIID, void**);
// The slot indices are pinned against the SDK in test/dlss5/vtable_slots.c;
// these pin the ARGUMENT ORDER, which a slot index cannot.  Between them a
// detour cannot drift from the ABI in either of the two ways it can drift.
static_assert(
    std::is_same_v<decltype(&ID3D12Device::CreateCommandList),
                   HRESULT (STDMETHODCALLTYPE ID3D12Device::*)(
                       UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
                       ID3D12PipelineState*, REFIID, void**)>,
    "ID3D12Device::CreateCommandList ABI must stay (nodeMask, type, allocator,"
    " initial_state, riid, ppCommandList)");
static_assert(
    std::is_same_v<decltype(&ID3D12Device4::CreateCommandList1),
                   HRESULT (STDMETHODCALLTYPE ID3D12Device4::*)(
                       UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS,
                       REFIID, void**)>,
    "ID3D12Device4::CreateCommandList1 ABI must stay (nodeMask, type, flags,"
    " riid, ppCommandList)");

// Records the deterministic birth baseline for a list that CreateCommandList
// just handed back, and takes the opportunity to install the list hooks from
// the earliest vtable in the process.
inline void SeedListAtBirth(
    void* raw_list,
    D3D12_COMMAND_LIST_TYPE type,
    ID3D12PipelineState* initial_state) {
  if (raw_list == nullptr) return;
  // COPY and VIDEO_* lists have no compute pipeline to shadow.  BUNDLE does:
  // a bundle's own recording is what MergeExecutedBundle replays into the
  // parent, and its baseline is the same one Reset writes for it today.
  if (type != D3D12_COMMAND_LIST_TYPE_DIRECT
      && type != D3D12_COMMAND_LIST_TYPE_COMPUTE
      && type != D3D12_COMMAND_LIST_TYPE_BUNDLE) {
    return;
  }
  // A baseline is only worth writing if everything that happens AFTER it is
  // observed.  Until the list hooks are live, the host's binds between birth
  // and the first evaluate would go unseen, and the entry would claim KNOWN
  // state that no longer holds - which is worse than declining, because the
  // gate would open on it.  Lists born before that point keep today's
  // behaviour exactly.
  if (!cmd_hooks_installed) return;
  // The entry has to reach the list the restore runs on.  Object-attached
  // mode puts it on the list itself, and proxies forward private data to the
  // native object, so the native pointer this hook holds and the wrapper the
  // host holds resolve to the same entry.  The striped table keys on the
  // pointer instead, where they are two different lists and a baseline
  // written here would sit in an entry nothing ever reads.
  if (list_state_mode.load(std::memory_order_relaxed)
      != kListStateObjectAttached) {
    return;
  }
  // riid may be any of the ID3D12CommandList interfaces - ask for the one the
  // shadow is keyed on rather than assuming the caller wanted it.
  ID3D12GraphicsCommandList* list = nullptr;
  if (FAILED(static_cast<IUnknown*>(raw_list)->QueryInterface(IID_PPV_ARGS(&list)))
      || list == nullptr) {
    return;
  }
  WriteShadowBaseline(list, initial_state, "BirthBaseline");
  list_birth_baselines.fetch_add(1, std::memory_order_relaxed);
  list->Release();
}

inline HRESULT STDMETHODCALLTYPE HookedDeviceCreateCommandList(
    ID3D12Device* device,
    UINT node_mask,
    D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator* command_allocator,
    ID3D12PipelineState* initial_state,
    REFIID riid,
    void** command_list) {
  device_create_list_entries.fetch_add(1, std::memory_order_relaxed);
  const HRESULT result =
      reinterpret_cast<DeviceCreateCommandListFn>(real_device_create_command_list)(
          device, node_mask, type, command_allocator, initial_state, riid,
          command_list);
  // One relaxed load is the whole cost in a title where NR is off or the user
  // turned it off.  The baseline is deliberately NOT gated on having seen a
  // DLSS feature: it has to be recorded BEFORE the first evaluate to be worth
  // anything, and waiting for one would guarantee that the first evaluates on
  // a freshly created list decline for want of it.
  GuardHook([&] {
    if (SUCCEEDED(result) && command_list != nullptr
        && enabled.load(std::memory_order_relaxed)
        && !SuppressCommandStateObservation()) {
      SeedListAtBirth(*command_list, type, initial_state);
    }
  });
  return result;
}

using DeviceCreateCommandSignatureFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*,
    REFIID, void**);
static_assert(
    std::is_same_v<decltype(&ID3D12Device::CreateCommandSignature),
                   HRESULT (STDMETHODCALLTYPE ID3D12Device::*)(
                       const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*,
                       REFIID, void**)>,
    "ID3D12Device::CreateCommandSignature ABI must stay (desc, root_signature,"
    " riid, ppvCommandSignature)");

// Records what a command signature names, on the signature itself, so that
// ExecuteIndirect can apply the exact documented aftermath without a lookup
// table, a lock or a pruning policy.  The description is derived by a pure
// function of the SDK descriptor (indirect_signature.hpp), which is where
// test/dlss5 pins it row by row.
inline HRESULT STDMETHODCALLTYPE HookedDeviceCreateCommandSignature(
    ID3D12Device* device,
    const D3D12_COMMAND_SIGNATURE_DESC* desc,
    ID3D12RootSignature* root_signature,
    REFIID riid,
    void** command_signature) {
  const HRESULT result =
      reinterpret_cast<DeviceCreateCommandSignatureFn>(
          real_device_create_command_signature)(
          device, desc, root_signature, riid, command_signature);
  // Unlike the list baseline, this one is recorded whether or not NR is
  // enabled right now: signatures are created once at load and the user may
  // switch NR on at any time afterwards, and a description that was never
  // taken cannot be recovered without the game restarting.  The cost is one
  // SetPrivateData per signature per session.
  GuardHook([&] {
    if (SUCCEEDED(result) && desc != nullptr && command_signature != nullptr
        && *command_signature != nullptr) {
      ID3D12CommandSignature* signature = nullptr;
      if (SUCCEEDED(static_cast<IUnknown*>(*command_signature)
                        ->QueryInterface(IID_PPV_ARGS(&signature)))
          && signature != nullptr) {
        const IndirectSignatureEffect effect = DescribeCommandSignature(*desc);
        signature->SetPrivateData(kIndirectSignatureEffectGuid, sizeof(effect),
                                  &effect);
        signature->Release();
        command_signatures_described.fetch_add(1, std::memory_order_relaxed);
      }
      // Release order matters: publishing the epoch AFTER the private data is
      // written is what makes a memo refill see the description.  Every
      // thread's memo is dropped on the next indirect, which is the price of
      // a signature being created - once, at load, in every engine seen so
      // far.
      command_signature_epoch.fetch_add(1, std::memory_order_release);
    }
  });
  return result;
}

// CreateCommandList1 is deliberately NOT hooked.  Its list is born CLOSED and
// it takes no initial pipeline state, so it is owed no baseline: the Reset the
// host must call before recording writes one through the Reset hook, which is
// the path that already works.  Its slot and ABI stay asserted above and in
// test/dlss5/vtable_slots.c so the day it is needed the number is not
// re-derived by hand.

// Caller holds device_hook_mutex.
inline void AnnounceDeviceHookRetry(const char* why) {
  bool announce = false;
  const uint64_t delay = NoteHookInstallFailure(
      device_hook_attempts, device_hook_next_attempt_present, announce);
  if (!announce) return;
  std::ostringstream failure;
  failure << "birth-time observation: " << why
          << "; list baselines fall back to first sight (no-seed hosts"
             " will decline). Attempt " << device_hook_attempts
          << ", retrying in " << delay << " presents"
          << (delay == kNgxRetryCeilingPresents
                  ? " and hourly after that" : "");
  Log(reshade::log::level::warning, failure.str());
}

// Installs the device-level hooks.  Called from init_device, which ReShade
// fires before the application has created anything on the device, and
// retried from the present handler for as long as it has not taken.
inline bool InstallDeviceStateHooks(ID3D12Device* device) {
  if (device == nullptr) return false;
  if (!hooks_enabled.load(std::memory_order_relaxed)) return false;
  if (device_hooks_installed) return true;
  std::lock_guard<std::mutex> lock(device_hook_mutex);
  if (device_hooks_installed) return true;
  if (present_generation < device_hook_next_attempt_present) return false;
  device_hooks_attempted = true;

  void** const device_vtable = *reinterpret_cast<void***>(device);
  void* const create_target = device_vtable[kDeviceSlotCreateCommandList];
  if (create_target == nullptr) {
    device_hooks_attempted = false;
    AnnounceDeviceHookRetry("ID3D12Device::CreateCommandList unresolved");
    return false;
  }

  real_device_create_command_list = create_target;
  // Each transaction gets its OWN scope: the mutex is not recursive, and
  // this function takes it twice.
  bool installed;
  {
    std::lock_guard<std::mutex> transaction(
        renodx::utils::vtable::TransactionMutex());
    installed = renodx::utils::vtable::BeginTransaction();
    if (installed) {
      DetourUpdateThread(GetCurrentThread());
      installed = DetourAttach(&real_device_create_command_list,
                               &HookedDeviceCreateCommandList)
              == NO_ERROR
          && DetourTransactionCommit() == NO_ERROR;
    }
    if (!installed) DetourTransactionAbort();
  }
  if (!installed) {
    real_device_create_command_list = nullptr;
    device_hooks_attempted = false;
    AnnounceDeviceHookRetry("Detours install on ID3D12Device failed");
    return false;
  }
  if (device_hook_attempts != 0) {
    std::ostringstream recovered;
    recovered << "birth-time observation RECOVERED after "
              << device_hook_attempts
              << " failed install attempt(s); lists created from here on"
                 " carry a known compute-state baseline again";
    Log(reshade::log::level::warning, recovered.str());
    device_hook_attempts = 0;
    device_hook_next_attempt_present = 0;
  }
  device_hooks_installed = true;

  // CreateCommandSignature goes in its OWN transaction.  A Detours
  // transaction is all-or-nothing, and the list baseline is what `no_seed`
  // depends on: losing it because a second, less critical hook could not
  // attach would trade a working shape for a better-described one.
  if (void* const signature_target =
          device_vtable[kDeviceSlotCreateCommandSignature]) {
    real_device_create_command_signature = signature_target;
    bool signature_installed;
    {
      std::lock_guard<std::mutex> transaction(
          renodx::utils::vtable::TransactionMutex());
      signature_installed = renodx::utils::vtable::BeginTransaction();
      if (signature_installed) {
        DetourUpdateThread(GetCurrentThread());
        signature_installed =
            DetourAttach(&real_device_create_command_signature,
                         &HookedDeviceCreateCommandSignature)
                == NO_ERROR
            && DetourTransactionCommit() == NO_ERROR;
      }
      if (!signature_installed) DetourTransactionAbort();
    }
    if (signature_installed) {
      signature_hook_installed = true;
      signature_hook_detoured.store(
          real_device_create_command_signature != signature_target ? 1u : 0u,
          std::memory_order_relaxed);
    } else {
      real_device_create_command_signature = nullptr;
    }
  }
  if (!signature_hook_installed) {
    Log(reshade::log::level::warning,
        "ExecuteIndirect: ID3D12Device::CreateCommandSignature could not be"
        " detoured; indirect calls are counted but their aftermath is not"
        " applied (GPU-driven titles may restore a stale root constant)");
  }

  // The same install-time positive control the list hooks carry: Detours
  // rewrites the pointer it attaches to point at its trampoline, so a pointer
  // still equal to the one read out of the vtable is a hook that did not take.
  const bool detoured = real_device_create_command_list != create_target;
  device_hooks_detoured.store(detoured ? 1u : 0u, std::memory_order_relaxed);
  Log(detoured ? reshade::log::level::info : reshade::log::level::warning,
      detoured
          ? "birth-time observation installed on the D3D12 device vtable:"
            " ID3D12Device::CreateCommandList verified detoured - lists"
            " created from here on carry a KNOWN compute-state baseline from"
            " their first instruction"
          : "birth-time observation: ID3D12Device::CreateCommandList did not"
            " take the detour; list baselines fall back to first sight"
            " (no-seed hosts will decline)");
  return true;
}

inline void UnhookDeviceStateHooks() {
  std::lock_guard<std::mutex> lock(device_hook_mutex);
  if (signature_hook_installed) {
    bool signature_detached;
    {
      std::lock_guard<std::mutex> transaction(
          renodx::utils::vtable::TransactionMutex());
      signature_detached = renodx::utils::vtable::BeginTransaction();
      if (signature_detached) {
        DetourUpdateThread(GetCurrentThread());
        signature_detached =
            DetourDetach(&real_device_create_command_signature,
                         &HookedDeviceCreateCommandSignature)
                == NO_ERROR
            && DetourTransactionCommit() == NO_ERROR;
      }
      if (!signature_detached) DetourTransactionAbort();
    }
    if (signature_detached) {
      signature_hook_installed = false;
      real_device_create_command_signature = nullptr;
      signature_hook_detoured.store(0, std::memory_order_relaxed);
    }
  }
  if (!device_hooks_installed) return;
  std::lock_guard<std::mutex> transaction(
      renodx::utils::vtable::TransactionMutex());
  bool detached = renodx::utils::vtable::BeginTransaction();
  if (detached) {
    DetourUpdateThread(GetCurrentThread());
    detached = DetourDetach(&real_device_create_command_list,
                            &HookedDeviceCreateCommandList)
            == NO_ERROR
        && DetourTransactionCommit() == NO_ERROR;
  }
  if (detached) {
    // Reached only from DetachLite, like the queue and command-list
    // families: a device rebuild keeps this detour, whose body outlives the
    // device, and so keeps observing the new device's list births.
    device_hooks_installed = false;
    device_hooks_attempted = false;
    device_hook_attempts = 0;
    device_hook_next_attempt_present = 0;
    real_device_create_command_list = nullptr;
    device_hooks_detoured.store(0, std::memory_order_relaxed);
  } else {
    DetourTransactionAbort();
  }
}

// ---------------------------------------------------------------------------
// Submission-fence tracker (drives GpuLease retirement).  Executes on the
// real command-queue vtable; kQueueSlotExecuteCommandLists lives in
// cmd_slots.hpp with every other slot index, where test/dlss5/vtable_slots.c
// checks it against the SDK.

// SDK order is (NumCommandLists, ppCommandLists): count precedes the array.
// The static_assert pins the signature to the local d3d12.h declaration so
// this detour cannot silently drift from the ABI again.
using QueueExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(
    ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static_assert(std::is_same_v<decltype(&ID3D12CommandQueue::ExecuteCommandLists),
                             void (STDMETHODCALLTYPE ID3D12CommandQueue::*)(
                                 UINT, ID3D12CommandList* const*)>,
              "ID3D12CommandQueue::ExecuteCommandLists ABI must stay "
              "(count, command_lists)");

inline void* real_queue_execute_command_lists = nullptr;
// Near-zero detour cost when no lease can be handed out (tracker not yet
// installed or already unhooked).  The flag itself is declared next to the
// retirement paths that consume it (see RetireWorksetResources).
inline std::mutex queue_hook_mutex;
inline bool queue_hook_installed = false;
// The install is retried, not attempted once.  Until v6.8.0-alpha30 a
// single failure - another of this addon's own threads holding the
// Detours transaction was enough, since Detours keeps one per linked copy
// (see vtable::TransactionMutex, alpha31) - set an `attempted` flag that
// was never cleared, so the queue completion
// tracker stayed off for the whole session and every workset waited for
// teardown to be freed.  Same latch shape as the NGX one, in a second
// place, with a memory consequence instead of an engagement one.  Backs
// off exactly like it: 1, 3, 7 ... presents, capped, two log lines.
inline uint32_t queue_hook_attempts = 0;
inline uint64_t queue_hook_next_attempt_present = 0;

// Requires queue_completion_mutex. Creates the queue's fence on first sight.
// A known queue without a healthy fence makes new leases incomplete: retirement
// then retains unproven resources until teardown instead of using present age.
inline QueueCompletion* ResolveQueueCompletionLocked(ID3D12CommandQueue* queue) {
  auto [entry, inserted] = queue_completions.try_emplace(queue);
  if (inserted) {
    ID3D12Device* device = nullptr;
    if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr) {
      ID3D12Fence* fence = nullptr;
      if (SUCCEEDED(device->CreateFence(
              0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        entry->second.fence = fence;
      }
      device->Release();
    }
    if (entry->second.fence == nullptr) {
      Log(reshade::log::level::warning,
          "queue completion fence unavailable; retirement will retain "
          "unproven resources until teardown");
    }
  }
  return entry->second.fence != nullptr ? &entry->second : nullptr;
}

inline void STDMETHODCALLTYPE HookedQueueExecuteCommandLists(
    ID3D12CommandQueue* queue,
    UINT count,
    ID3D12CommandList* const* command_lists) {
  queue_submit_entries.fetch_add(1, std::memory_order_relaxed);
  // Phase 1 of the exact submission proof (v6 audit issue 01): reserve
  // pending submission slots on the submitted lists' current recording
  // generations BEFORE forwarding.  The reservation exists before this call
  // returns, so a Reset issued right after the submit can never close a
  // generation ahead of its proof.
  //
  // v6.5.3 inert gate (007 First Light field log): INERT until the addon
  // recorded its first tracked GPU use (sticky - submission_tracker.hpp).
  // A passive session - NR declined by the restore-target gate while the
  // game initializes frame generation - previously still appended a fence
  // signal to every foreign submit, and the first captured session with
  // that ordering (tracker armed 4.4 s before the DLSSG create) died inside
  // NGX frame-generation initialization.  Foreign submits now reserve and
  // append nothing: no per-queue fence, no signal.  The post-forward
  // re-read keeps even the FIRST armed submission signaled when the arming
  // TrackUse landed on another thread while this submit was in flight - a
  // submit's uses are recorded before its Close, so the pre-forward check
  // alone would only miss under that race, and an extra monotonic signal
  // is harmless where a missed one would strand a generation's proof.
  //
  // Guarded in two halves around the forward, because BeginSubmission
  // allocates: if it throws, `pending_submissions` stays empty, which is
  // exactly the untracked shape the `!tracking` path below already handles -
  // the generations retain until teardown rather than being guessed.  What
  // must not happen either way is the game's submit not being made.
  bool tracking = false;
  std::vector<submission::PendingSubmission> pending_submissions;
  GuardHook([&] {
    tracking = queue_tracking_active.load(std::memory_order_relaxed)
        && submission::HasEverTrackedUses();
    if (tracking) {
      pending_submissions = submission::BeginSubmission(count, command_lists);
    }
  });
  reinterpret_cast<QueueExecuteCommandListsFn>(
      real_queue_execute_command_lists)(queue, count, command_lists);
  // Guarded: everything below is the addon's own bookkeeping, and the
  // game's submit has already been made.  A throw here costs a proof,
  // never the frame.
  GuardHook([&] {
    if (!tracking && !submission::HasEverTrackedUses()) {
      queue_detour_inert_submits.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // The funnel's last rung: a submission that actually carried our recorded
    // work reached the queue.  Injection recorded but never submitted is a
    // distinct failure from injection never recorded, and until R1 the two
    // were indistinguishable in a field log.
    if (!pending_submissions.empty()) {
      submitted_injections.fetch_add(1, std::memory_order_relaxed);
    }
    // Post-forward only: the fence value must be assigned after the real
    // submission is queued, and ReShade's pre-submit execute_command_list
    // event must never grow a signal.  Concurrent submits to one queue are
    // legal D3D12; the value assignment and Signal share the tracker lock so
    // two threads cannot queue a lower fence value after a higher one (D3D12
    // fences must advance monotonically).  The fence is co-owned before the
    // lock drops: a teardown on another thread releases the tracker's
    // reference (ReleaseQueueCompletions), and CompleteSubmission below would
    // otherwise AddRef a released fence.
    FenceRef completion_fence;
    uint64_t completion_value = 0;
    {
      std::lock_guard<std::mutex> lock(queue_completion_mutex);
      QueueCompletion* tracker = ResolveQueueCompletionLocked(queue);
      if (tracker != nullptr) {
        tracker->last_submit_ns.store(SteadyNowNs(), std::memory_order_relaxed);
      }
      if (tracker != nullptr && !tracker->faulted) {
        const uint64_t value =
            tracker->last_assigned.fetch_add(1, std::memory_order_relaxed) + 1;
        if (SUCCEEDED(queue->Signal(tracker->fence, value))) {
          if (!pending_submissions.empty()) {
            completion_fence = FenceRef(tracker->fence);
          }
          completion_value = value;
        } else {
          // The fence can no longer prove completions. Keep the queue entry marked
          // faulted so future leases are incomplete and retirement fails closed.
          tracker->faulted = true;
          Log(reshade::log::level::warning,
              "queue completion tracker: Signal failed; future retirement leases "
              "are incomplete and unproven resources stay alive until teardown");
        }
      }
    }
    // Phase 2: attach the exact proof.  A null fence (faulted Signal) leaves
    // the reservations unprovable - their generations retain until teardown,
    // never guessed.
    submission::CompleteSubmission(
        pending_submissions, completion_fence.fence, completion_value);
  });
}

// Fault injection for the queue-hook install.  A retry nothing ever makes
// retry is indistinguishable from the latch it replaced, so the lane that
// covers it has to be able to produce the failure.  Caller holds
// queue_hook_mutex, so no atomics.
inline uint32_t TestQueueHookFailureBudget() {
  static const uint32_t budget = [] {
    char buffer[16] = {};
    size_t length = 0;
    if (getenv_s(&length, buffer, sizeof(buffer),
                 "RENODX_NR_TEST_FAIL_QUEUE_HOOK") != 0
        || length == 0) {
      return 0u;
    }
    return static_cast<uint32_t>(strtoul(buffer, nullptr, 10));
  }();
  return budget;
}
inline uint32_t test_queue_hook_failures_left = 0;
inline bool test_queue_hook_armed = false;

inline bool ConsumeTestQueueHookFailure() {
  if (!test_queue_hook_armed) {
    test_queue_hook_armed = true;
    test_queue_hook_failures_left = TestQueueHookFailureBudget();
  }
  if (test_queue_hook_failures_left == 0) return false;
  --test_queue_hook_failures_left;
  return true;
}

// Caller holds queue_hook_mutex.  Same backoff and the same
// two-lines-per-session log budget as the NGX module retry.
inline void ScheduleQueueHookRetry(const char* why) {
  bool announce = false;
  const uint64_t delay = NoteHookInstallFailure(
      queue_hook_attempts, queue_hook_next_attempt_present, announce);
  if (announce) {
    std::ostringstream failure;
    failure << "queue completion tracker: " << why
            << "; retirement without a GPU completion proof stays"
               " deferred to teardown. Attempt " << queue_hook_attempts
            << ", retrying in " << delay << " presents"
            << (delay == kNgxRetryCeilingPresents
                    ? " and hourly after that" : "");
    Log(reshade::log::level::warning, failure.str());
  }
}

// Installed from the present handler and retried there, resolved from the
// vtable of the present queue.  Like the command-list detours this attaches
// the function body, covering every queue instance and queue type of the
// device.
inline bool InstallQueueCompletionHooks(ID3D12CommandQueue* queue) {
  if (queue == nullptr) return false;
  if (queue_hook_installed) {
    // A device-rebuild teardown keeps the detour and disarms tracking
    // (Shutdown); the first install call after it re-arms it here.
    if (!queue_tracking_active.load(std::memory_order_relaxed)
        && !shutting_down.load(std::memory_order_acquire)) {
      queue_tracking_active.store(true, std::memory_order_release);
    }
    return true;
  }
  std::lock_guard<std::mutex> lock(queue_hook_mutex);
  if (queue_hook_installed) return true;
  // Called from the present handler, so present_generation is this
  // thread’s own clock.
  if (present_generation < queue_hook_next_attempt_present) return false;
  void** vtable = *reinterpret_cast<void***>(queue);
  void* target = vtable[kQueueSlotExecuteCommandLists];
  if (target == nullptr) {
    ScheduleQueueHookRetry("ExecuteCommandLists slot unresolved");
    return false;
  }
  real_queue_execute_command_lists = target;
  // Before any transaction is begun, so the injected failure needs no
  // abort.  It models the real shape: the Detours transaction is
  // process-wide and single-threaded, and this install runs on the
  // present thread while the command-list install runs on the game’s
  // recording thread - see vtable::TransactionMutex.
  if (TestQueueHookFailureBudget() != 0 && ConsumeTestQueueHookFailure()) {
    real_queue_execute_command_lists = nullptr;
    ScheduleQueueHookRetry(
        "RENODX_NR_TEST_FAIL_QUEUE_HOOK: injected install failure");
    return false;
  }
  std::lock_guard<std::mutex> transaction(
      renodx::utils::vtable::TransactionMutex());
  bool detours_installed = renodx::utils::vtable::BeginTransaction();
  if (detours_installed) {
    DetourUpdateThread(GetCurrentThread());
    detours_installed =
        DetourAttach(
            &real_queue_execute_command_lists,
            &HookedQueueExecuteCommandLists) == NO_ERROR
        && DetourTransactionCommit() == NO_ERROR;
  }
  if (!detours_installed) {
    DetourTransactionAbort();
    real_queue_execute_command_lists = nullptr;
    ScheduleQueueHookRetry("Detours install failed");
    return false;
  }
  if (queue_hook_attempts != 0) {
    std::ostringstream recovered;
    recovered << "queue completion tracker RECOVERED after "
              << queue_hook_attempts
              << " failed install attempt(s); resource retirement has a"
                 " GPU completion proof again";
    Log(reshade::log::level::warning, recovered.str());
    queue_hook_attempts = 0;
    queue_hook_next_attempt_present = 0;
  }
  queue_hook_installed = true;
  queue_tracking_active.store(true, std::memory_order_release);
  Log(reshade::log::level::info,
      "queue completion tracker installed on the D3D12 command queue "
      "(post-submit signal fences drive resource retirement)");
  return true;
}

inline void UnhookQueueCompletionHooks() {
  std::lock_guard<std::mutex> lock(queue_hook_mutex);
  if (!queue_hook_installed) return;
  queue_tracking_active.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> transaction(
      renodx::utils::vtable::TransactionMutex());
  bool detours_detached = renodx::utils::vtable::BeginTransaction();
  if (detours_detached) {
    DetourUpdateThread(GetCurrentThread());
    detours_detached =
        DetourDetach(
            &real_queue_execute_command_lists,
            &HookedQueueExecuteCommandLists) == NO_ERROR
        && DetourTransactionCommit() == NO_ERROR;
  }
  if (detours_detached) {
    // Reached only from DetachLite, for the command-list family's reason
    // (UnhookCommandListStateHooks): detaching at a device rebuild nulled
    // the forward pointer under game threads still inside this detour, and
    // every engine submits from threads the teardown does not stop.
    queue_hook_installed = false;
    queue_hook_attempts = 0;
    queue_hook_next_attempt_present = 0;
    real_queue_execute_command_lists = nullptr;
  } else {
    DetourTransactionAbort();
    // Stays flagged installed: the trampoline may still fire, but it is
    // inert (tracking off) and Shutdown keeps the fences alive until every
    // lease holder is flushed and the map is cleared.
  }
}

// ---------------------------------------------------------------------------
// P4 Path A, A1+A3 (PLAN_DX11_V68.md 4.5; PLAN_REHAB_V7.md 8 row 25).
//
// A D3D12 NGX entry that arrives while the session looks FOREIGN - D3D11
// presents flowing, no D3D12 present ever seen - comes from a third-party
// tool's D3D12 device evaluating inside a D3D11-presenting process.  The
// only queue-hook install site used to be the present path, gated on the
// present queue's device being D3D12 (alpha33's API gate, which stays
// byte-identical), so such a session never got a completion proof and every
// retired NR resource waited for teardown ("D3D12 retirement deferred until
// teardown: no GPU completion fence was available" - observed verbatim in
// the P3 foreign runs, WITNESS 3.5).  A1 resolves the ExecuteCommandLists
// vtable from a THROWAWAY NATIVE QUEUE created on the entry's device and
// runs the existing InstallQueueCompletionHooks: no new install mechanism,
// the same queue_hook_mutex + vtable::TransactionMutex ordering (alpha31)
// and the same fail-retry backoff (alpha30, presents still flow in a
// D3D11-presenting process).
//
// The queue is created on the NATIVE device (the unwrap is
// renodx::utils::directx::NativeFromReShadeProxy, the same helper the
// direct-load path uses): a wrapped device hands back a wrapped queue whose
// vtable is ReShade's, while the tracker must see the submits the wrapper
// forwards to the driver's own function body.  Like the command-list
// detours, the queue detour attaches that body, so every queue of the
// device is covered; the throwaway queue itself is released immediately.
//
// A3 rides the same entry: a foreign device that never went through
// ReShade's init_device (created BESIDE the hooked D3D12CreateDevice) has
// no device/list-state hooks - its gate is its own: no device hooks and no
// init_device for D3D12 at all.  A D3D12 session is untouched even when its
// birth-time install failed (the present-path retry owns that case), and in
// the wrapped foreign lane init_device already installed the hooks.  Any
// change to this path ships with the record-vs-replay heap trace
// (PLAN_REHAB_V7.md 7): RENODX_NR_TEST_HEAP_TRACE=1 prints one line for the
// list the shadow recorded on and one for the list the restore replayed
// through, which is how the two implementations are told apart.
//
// Observe-only: A1 enables proofs and names the foreign source; it declines
// nothing (the 4.4 bridge_* declines belong to the P5 bridge, and the
// double-processing ride this session serves is the intended Path A
// behavior - any gate on it is P5 B7, observe-only first).  The gates keep
// every standard D3D12 session untouched: d3d12_present_seen makes the
// queue path unreachable there, so the present path stays the only
// installer and the log vocabulary does not change.
inline std::atomic_bool foreign_queue_install_logged{false};
inline std::atomic_uint64_t foreign_ngx_entries{0};
inline std::atomic_uint64_t foreign_queue_install_failures{0};
inline std::atomic_bool foreign_device_hooks_logged{false};

inline void EnsureForeignEntryInstalls(
    ID3D12GraphicsCommandList* command_list) {
  if (command_list == nullptr) return;
  // ---- A1: queue completion proofs (the foreign-session gate) ------------
  if (!queue_tracking_active.load(std::memory_order_relaxed)
      && d3d11_present_seen.load(std::memory_order_relaxed)
      && !d3d12_present_seen.load(std::memory_order_relaxed)) {
    foreign_ngx_entries.fetch_add(1, std::memory_order_relaxed);
    ID3D12Device* device = nullptr;
    if (SUCCEEDED(command_list->GetDevice(IID_PPV_ARGS(&device)))
        && device != nullptr) {
      ID3D12Device* native = device;
      // Reference truth table for the pair below (GetDevice handed us one
      // reference on `device`):
      //   unwrap SUCCEEDS: native is a BORROWED inner pointer (the helper QIs
      //     the native object and immediately releases its own reference) -
      //     take our own ref, then release each pointer once.  Skipping the
      //     AddRef over-releases the native device (the freed-device AV the
      //     foreign-wrapped lane caught on this branch's first build).
      //   unwrap FAILS - the device was never wrapped, e.g. created beside
      //     the hooked D3D12CreateDevice export (the bypass arm): native
      //     ALIASES device and GetDevice's single reference must be released
      //     exactly once.  Releasing both aliases costs the foreign device
      //     one reference - the stub's device died at teardown and its child
      //     releases AVed in d3d12!CUseCountedObject::Release (the bypass
      //     lane caught this side of the same coin).
      const bool unwrapped =
          renodx::utils::directx::NativeFromReShadeProxy(&native);
      if (unwrapped) native->AddRef();
      D3D12_COMMAND_QUEUE_DESC queue_desc = {};
      queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
      ID3D12CommandQueue* throwaway = nullptr;
      if (SUCCEEDED(native->CreateCommandQueue(&queue_desc,
                                               IID_PPV_ARGS(&throwaway)))
          && throwaway != nullptr) {
        if (InstallQueueCompletionHooks(throwaway)) {
          if (!foreign_queue_install_logged.exchange(true)) {
            Log(reshade::log::level::info,
                "foreign D3D12 NGX source: queue completion tracker"
                " installed from a throwaway native queue on the entry's"
                " device (D3D11-presenting process; retired NR resources"
                " gain GPU completion proofs - observe-only,"
                " PLAN_DX11_V68.md 4.5 A1)");
          }
        } else {
          foreign_queue_install_failures.fetch_add(
              1, std::memory_order_relaxed);
        }
        throwaway->Release();
      } else {
        foreign_queue_install_failures.fetch_add(1, std::memory_order_relaxed);
      }
      if (unwrapped) native->Release();
      device->Release();
    }
  }
  // ---- A3: device birth hooks (the never-init_device gate) ---------------
  if (!device_hooks_installed
      && !d3d12_device_seen.load(std::memory_order_relaxed)) {
    ID3D12Device* device = nullptr;
    if (SUCCEEDED(command_list->GetDevice(IID_PPV_ARGS(&device)))
        && device != nullptr) {
      ID3D12Device* native = device;
      // Same reference truth table as the A1 half above: AddRef (and release)
      // the inner pointer only when the helper actually unwrapped; when it
      // did not, native aliases device and one release covers both.
      const bool unwrapped =
          renodx::utils::directx::NativeFromReShadeProxy(&native);
      if (unwrapped) native->AddRef();
      if (InstallDeviceStateHooks(native)
          && !foreign_device_hooks_logged.exchange(true)) {
        Log(reshade::log::level::info,
            "foreign D3D12 NGX source: device/list-state hooks installed"
            " from the first NGX entry (this device never went through"
            " init_device - PLAN_DX11_V68.md 4.5 A3)");
      }
      if (unwrapped) native->Release();
      device->Release();
    }
  }
}

// RAII envelope around the REAL NGX evaluate: seeds the restore target from
// the list's persistent shadow (the HOST's state), keeps the window armed for
// anything the host itself records, stops recording before our injected NR
// work runs, and re-applies the captured state only if addon GPU commands were
// actually recorded. Avoiding restore on no-op/declined frames minimizes state
// perturbation in engines with unusual root/descriptor management.
// Until v6.8.0-alpha9 the window also recorded the NGX runtime's own binds
// during the evaluate and restored those; see NgxRuntimeCommandScope for what
// that replayed and what the debug layer said about it.
struct ComputeStateEnvelope {
  // Borrowed from a per-thread pool, not held by value: this object is
  // constructed on the stack the GAME gave the evaluate, and the state is
  // 18 KB, which overflowed a 64 KB job-system worker (capture_arena.hpp).
  CapturedComputeState* captured;
  ID3D12GraphicsCommandList* command_list;
  bool capturing = false;
  bool capture_valid = false;
  bool injection_commands_recorded = false;

  explicit ComputeStateEnvelope(ID3D12GraphicsCommandList* list)
      : captured(AcquireCaptureSlot()), command_list(list) {
    // No slot means no restore target, which declines the evaluate.  It
    // takes an exhausted heap to get here, and injecting blind would be
    // the one unrecoverable answer.
    capture_valid = captured != nullptr && InstallCommandListStateHooks(list);
    capturing = capture_valid;
    if (!capturing) return;
    BeginComputeStateCapture(captured, list);
    // The list and the hooked implementation in different worlds (observe
    // only; see InstallCommandListStateHooks).  A native list restores
    // correctly - ApplyCapturedComputeState unwraps shadowed heap proxies for
    // it.  A proxy list over a native-world shadow has no conversion: ReShade
    // keeps no native-to-proxy map for heaps, and its full add-on build casts
    // whatever heap it is handed to its own proxy class.
    ID3D12GraphicsCommandList* native_list = list;
    const bool proxy_list =
        renodx::utils::directx::NativeFromReShadeProxy(&native_list);
    if (proxy_list == cmd_hooks_on_reshade_proxy.load(std::memory_order_relaxed)
        || (proxy_list ? logged_world_proxy_list : logged_world_native_list)
               .exchange(true)) {
      return;
    }
    Log(reshade::log::level::warning,
        proxy_list
            ? "NR evaluate on a ReShade proxy command list while the"
              " compute-state shadow observes the native implementation: the"
              " restore hands ReShade native descriptor heaps, which its full"
              " add-on build cannot take - if the game stops after this line,"
              " this is the shape"
            : "NR evaluate on a native D3D12 command list while the"
              " compute-state shadow observes ReShade's proxy (the Streamline"
              " layer unwraps lists and can be enabled mid-session): the"
              " restore unwraps the shadowed descriptor heaps for this list");
  }
  // Called once the real NGX evaluate has returned, BEFORE ProcessInline
  // records our own binds (nested outer windows keep recording - correct,
  // see the module-chain note above).  The evaluate itself now runs under
  // NgxRuntimeCommandScope, so between construction and here the window only
  // ever records what the HOST records, which on every engine measured so
  // far is nothing.
  void StopCapture() {
    if (capturing) {
      EndComputeStateCapture(captured);
      capturing = false;
    }
  }
  // ProcessInline always changes the shader-visible descriptor heap, and the
  // restore replays the captured signature, arguments, and pipeline - so a
  // capture is safe for injection only when EVERY overwritten aspect is
  // KNOWN (ComputeStateFields::RestoreTargetComplete): heaps, the root
  // signature, the root-argument set, and the PSO, observed or seeded,
  // including the deterministic baseline a host Reset / ClearState leaves
  // behind (a known null PSO counts - the host must set one before its next
  // dispatch).  An unknown aspect has no restore target: injecting would
  // leave the codec's signature/PSO/arguments bound for the host, whose next
  // dispatch then runs with foreign bindings (the bindless device-removal /
  // GPU-hang class).  v6.0.0-v6.3.0 shipped a heaps-only gate here; the NGX
  // evaluate's own SetDescriptorHeaps always satisfied it, and RE Requiem
  // (v6.2.0 field log) died on the first frame after the hooks installed
  // mid-recording.  Astra's state contract: "unknown means skip injection" -
  // the frame declines (counted) and recovers on its own: the list's next
  // Reset is observed and completes the target.
  bool CanInject() const noexcept {
    return capture_valid && captured->RestoreTargetComplete();
  }
  bool ClearStateInvalidated() const noexcept {
    return captured != nullptr && captured->observed_clear_state;
  }
  // Structured decline evidence: which aspects of the target were observed.
  std::string DescribeKnowledge() const {
    if (captured == nullptr) return "hooks=0 capture_storage=unavailable";
    char text[128];
    std::snprintf(
        text, sizeof(text),
        "hooks=%d heaps=%d root_signature=%d root_arguments=%d pso=%d",
        capture_valid ? 1 : 0,
        captured->heaps_known ? 1 : 0,
        captured->root_signature_known ? 1 : 0,
        captured->root_arguments_known ? 1 : 0,
        captured->pipeline_state_known ? 1 : 0);
    return text;
  }
  // Bit per GateAspect: set = this aspect was never observed, so a restore
  // could not put it back.
  std::uint32_t MissingAspects() const noexcept {
    std::uint32_t mask = 0;
    auto bit = [](GateAspect aspect) {
      return 1u << static_cast<std::uint32_t>(aspect);
    };
    if (!capture_valid) mask |= bit(GateAspect::kHooks);
    if (captured == nullptr) {
      // Nothing was observed, so nothing could be put back.
      return mask | bit(GateAspect::kHeaps) | bit(GateAspect::kRootSignature)
          | bit(GateAspect::kRootArguments) | bit(GateAspect::kPipelineState);
    }
    if (!captured->heaps_known) mask |= bit(GateAspect::kHeaps);
    if (!captured->root_signature_known) mask |= bit(GateAspect::kRootSignature);
    if (!captured->root_arguments_known) mask |= bit(GateAspect::kRootArguments);
    if (!captured->pipeline_state_known) mask |= bit(GateAspect::kPipelineState);
    return mask;
  }

  // The injection gate every path shares (after-upscale, pre-SR, Streamline
  // fallback).  Every decline is counted per missing aspect; the first
  // admission is logged once as the gate's positive control, so a field log
  // proves the gate ran before any NR work.
  bool Admit(
      const char* path, NrDeclineReason reason, std::atomic_bool* logged_decline) {
    if (!CanInject()) {
      CountNrDecline(reason);
      const std::uint32_t missing = MissingAspects();
      CountGateMissing(missing);
      // The WARN re-arms whenever the SET of missing aspects changes (R1).
      // A one-shot warning cannot tell "the same aspect has been missing for
      // ten minutes" from "it was heaps at boot and is root arguments now",
      // and those are different faults with different fixes.  Rate-limited by
      // the mask rather than by time, so a stable session stays quiet.
      const std::uint32_t previous =
          logged_missing_mask.exchange(missing, std::memory_order_relaxed);
      if (!logged_decline->exchange(true) || previous != missing) {
        Log(
            reshade::log::level::warning,
            std::string("NR skipped (") + path
                + "): compute-state restore target incomplete ("
                + DescribeKnowledge()
                + "); injecting would leave our bindings bound for the game."
                  " Retried every evaluate - the list's next observed Reset"
                  " completes it");
      }
      return false;
    }
    // The gate opened at least once this session.  This is what the verdict
    // reads, not the log flag below: whether the restore target ever
    // completed must not depend on the log level (R1).
    gate_ever_opened.store(true, std::memory_order_relaxed);
    if (!logged_state_target_armed.exchange(true)) {
      const uint64_t declined =
          nr_decline_counts[static_cast<size_t>(
                                NrDeclineReason::kStateShadowUnavailable)]
              .load()
          + nr_decline_counts[static_cast<size_t>(
                                  NrDeclineReason::kPreSrNoStateTarget)]
                .load();
      Log(
          reshade::log::level::info,
          std::string("compute-state restore target complete (") + path + ": "
              + DescribeKnowledge() + "); injection admitted after "
              + std::to_string(declined) + " incomplete-target decline(s)");
    }
    return true;
  }
  void MarkInjectionCommandsRecorded() noexcept {
    injection_commands_recorded = true;
  }
  ~ComputeStateEnvelope() {
    StopCapture();
    if (capture_valid && injection_commands_recorded) {
      if (HeapTraceArmed() && !heap_trace_replay_logged.exchange(true)) {
        std::ostringstream trace;
        trace << "heap trace: replay list=0x" << std::hex
              << reinterpret_cast<uintptr_t>(command_list) << " heaps="
              << std::dec << captured->heap_count;
        for (UINT i = 0; i < captured->heap_count && i < 2; ++i) {
          trace << " 0x" << std::hex
                << reinterpret_cast<uintptr_t>(captured->heaps[i]);
        }
        Log(reshade::log::level::info, trace.str());
      }
      ApplyCapturedComputeState(command_list, *captured, cmd_hooks_installed);
    }
    ReleaseCaptureSlot(captured);
  }
  ComputeStateEnvelope(const ComputeStateEnvelope&) = delete;
  ComputeStateEnvelope& operator=(const ComputeStateEnvelope&) = delete;
};

// The native D3D11 route's D3D12 terminal: the call is forwarded untouched
// and counted once per game call - the raised wrapper flag makes every
// further layer it reaches (the C export's C++ delegation, a plugin copy
// forwarding into the core) take its nested path, which counts nothing.
template <typename Real>
inline NVSDK_NGX_Result PassSourceOther(Real&& real) {
  const EvaluateInFlightScope in_flight(true);
  ++intercepted_evaluations;
  CountNrDecline(NrDeclineReason::kSourceOther);
  if (!logged_source_other.exchange(true)) {
    Log(reshade::log::level::info,
        "a Direct3D 12 NGX evaluate arrived in a Direct3D 11 session that the"
        " Direct3D 11 bridge serves (DX11Source native route): it belongs to"
        " a third-party tool and passes through untouched (source_other)");
  }
  const bool outer = inside_game_evaluate_wrapper;
  inside_game_evaluate_wrapper = true;
  const NVSDK_NGX_Result result = [&] {
    const NgxRuntimeCommandScope runtime_scope;
    return real();
  }();
  inside_game_evaluate_wrapper = outer;
  return result;
}

template <int Slot>
inline NVSDK_NGX_Result NVSDK_CONV HookedEvaluateFeatureSlot(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters,
    PFN_NVSDK_NGX_ProgressCallback callback) {
  debug::TouchNgxEval();
  ngx_entry_evaluate[Slot].fetch_add(1, std::memory_order_relaxed);
  auto real = reinterpret_cast<decltype(&NVSDK_NGX_D3D12_EvaluateFeature)>(
      ngx_slot_real[Slot].evaluate);
  CallbackScope callback_scope;
  if (!callback_scope) {
    const NgxRuntimeCommandScope runtime_scope;
    return real(command_list, handle, parameters, callback);
  }
  // A-1: everything below dereferences command_list (the foreign installs,
  // the pre-SR hooks, the state envelope, the NR pass).  A first argument
  // that is not a readable object goes to the real export untouched and is
  // named as an ineligible terminal.  Our own evaluates carry our own list,
  // and a C-export wrapper around this one has already judged it.
  if (!InsideDirectCall() && !inside_game_evaluate_wrapper
      && !NgxFirstArgumentUsable(command_list, "evaluate", Slot)) {
    const EvaluateInFlightScope in_flight(true);
    ++intercepted_evaluations;
    CountNrDecline(NrDeclineReason::kImplausibleArgument);
    const NgxRuntimeCommandScope runtime_scope;
    return real(command_list, handle, parameters, callback);
  }
  // The native D3D11 route: a tool's D3D12 evaluate is not ours to inject.
  if (!InsideDirectCall() && !inside_game_evaluate_wrapper
      && ForeignD3D12SourceIgnored()) {
    return PassSourceOther(
        [&] { return real(command_list, handle, parameters, callback); });
  }
  const EvaluateChainScope chain_scope;
  const bool nested_in_c_wrapper = inside_game_evaluate_wrapper;
  // Counted in `seen` from here until this evaluate names its terminal.
  const EvaluateInFlightScope in_flight(!nested_in_c_wrapper);
  if (!nested_in_c_wrapper) {
    ++intercepted_evaluations;
    // The clean denominator subtracts our own NR evaluate, which
    // re-enters this very export through the detour (R1).
    if (InsideDirectCall()) {
      own_evaluations.fetch_add(1, std::memory_order_relaxed);
    }
  }
  // Outermost game evaluate: candidate lifecycle tick while presents starve
  // (see NgxLifecycleTick; no-op while present events flow).
  if (!nested_in_c_wrapper && !InsideDirectCall()) NgxLifecycleTick();
  // P4 A1: the foreign-source queue-completion install (observe-only; a
  // no-op in every D3D12 session - see the definition).  Before any lock
  // work: the helper takes queue_hook_mutex and a Detours transaction, and
  // must never do so under runtime_mutex.
  if (!nested_in_c_wrapper && !InsideDirectCall()) {
    EnsureForeignEntryInstalls(command_list);
  }
  // Ask the loader for what this evaluate is about to need, HERE, before
  // either of the two runtime_mutex scopes below.  Holding that mutex across
  // the loader lock is the deadlock in runtime_lock.hpp, and this path is how
  // the addon used to reach it.  Free once the symbols are resolved.
  if (!nested_in_c_wrapper && !InsideDirectCall()) PrimeNgxLoaderSymbols();
  if (!logged_first_evaluate.exchange(true)) {
    Log(
        reshade::log::level::info,
        "first NGX evaluate intercepted (slot=" + std::to_string(Slot) + ")");
  }
  // Pre-SR insertion point: run NR on the game's render-res NGX Color BEFORE
  // its own DLSS evaluate runs and hand the game the enhanced stand-in for the
  // duration of real() (RestorePreSrColor puts the game's pointer back).
  // Engages only in the outermost wrapper so a C-export delegation does not
  // run the pass twice, under runtime_mutex like every feature-map access,
  // and only while the pre-SR mode is selected - the two insertion points are
  // mutually exclusive, so a declined pre-SR frame is simply left as rendered
  // rather than silently re-contracting the after path.
  // The re-entrancy check gates the LOCK, not just the pass: internal NR
  // evaluates via the NGX core re-enter this wrapper through the detoured
  // export while the calling thread already holds runtime_mutex, and the
  // non-recursive mutex would self-deadlock (the create/release wrappers
  // have always checked before locking).
  PreSrSwap pre_sr;
  if (!nested_in_c_wrapper && !InsideDirectCall()) {
    RuntimeLock lock(runtime_mutex);
    if (enabled.load()
        && handle != nullptr && parameters != nullptr
        && PreSrTakes(handle) && !NrYieldsToForeign()
        && !NrWaitsForTeardown(command_list)
        && RegisteredDlssEvaluate(handle)
        && InstallCommandListStateHooks(command_list)) {
      // Pre-SR injects BEFORE the real evaluate's capture window opens, so
      // nothing else restores the host state its binds clobbered (the real
      // evaluate may leave them in place on engines that rebind little).
      // A seed-only envelope around the injection restores exactly the
      // shadowed host state afterwards (audit issue 03): the window is
      // disarmed before recording so our own binds cannot land in it.  The
      // seed must be a complete restore target; the first frame after hook
      // installation therefore declines safely.
      ComputeStateEnvelope pre_sr_envelope(command_list);
      pre_sr_envelope.StopCapture();
      if (pre_sr_envelope.Admit(
              "pre-SR", NrDeclineReason::kPreSrNoStateTarget,
              &logged_presr_state_skip)) {
        bool pre_sr_recorded = false;
        const int64_t started_ns = SteadyNowNs();
        ProcessInlinePreSR(
            command_list, handle, parameters, pre_sr, &pre_sr_recorded);
        RecordInjectionCpu(started_ns);
        if (pre_sr_recorded) pre_sr_envelope.MarkInjectionCommandsRecorded();
      }
    }
  }
  // State capture window (see ComputeStateEnvelope): armed by the OUTERMOST
  // wrapper only.  In a plugin->core module chain the inner hook entry opens
  // a nested window that resumes the outer one on close, so the innermost
  // entry's restore lands in the outermost capture as the last writes and
  // every level's re-apply stays a semantic no-op.  Outer evaluates whose
  // injection would double-run are dedup-skipped inside ProcessInline.
  ComputeStateEnvelope state_envelope(
      (!nested_in_c_wrapper && !InsideDirectCall()) ? command_list : nullptr);
  const NVSDK_NGX_Result result = [&] {
    const NgxRuntimeCommandScope runtime_scope;
    return real(command_list, handle, parameters, callback);
  }();
  state_envelope.StopCapture();
  RestorePreSrColor(parameters, pre_sr);
  // Item 19: name every terminal.  This compound early-out used to return
  // without counting anything, so an NR-off session's evaluates (7807 in
  // the MSFS2024 field log of an otherwise healthy session) and a failed
  // game evaluate both landed in `unaccounted`, and the session's ratio
  // read 0.31 forever.  InsideDirectCall and the nested C wrapper are
  // accounted elsewhere (own_evaluations; the outermost wrapper takes this
  // evaluate's terminal below).
  if (InsideDirectCall() || nested_in_c_wrapper) {
    return result;
  }
  if (NVSDK_NGX_FAILED(result)) {
    CountNrDecline(NrDeclineReason::kGameEvaluateFailed);
    return result;
  }
  if (!enabled.load()) {
    CountNrDecline(NrDeclineReason::kNrDisabledEvaluation);
    return result;
  }
  if (handle == nullptr || parameters == nullptr) {
    CountNrDecline(NrDeclineReason::kMalformedEvaluate);
    return result;
  }

  RuntimeLock lock(runtime_mutex);
  // The taxonomy always had kNgxNotDlssEvaluation and `eligible` always
  // subtracted it - but no call site ever counted it, so every DLSSG
  // evaluate on an MFG-generated frame was eligible-but-unnamed (~6744 in
  // the same field log).  Counted here only, in both insertion modes: the
  // pre-SR gate above runs first and skips its pass silently, and this is
  // the site every evaluate reaches exactly once.
  if (!RegisteredDlssEvaluate(handle)) {
    CountNrDecline(NrDeclineReason::kNgxNotDlssEvaluation);
    return result;
  }
  // Both insertion modes: the pre-SR block above skipped its pass silently.
  if (NrYieldsToForeign()) {
    CountNrDecline(NrDeclineReason::kForeignNr);
    return result;
  }
  if (NrWaitsForTeardown(command_list)) {
    CountNrDecline(NrDeclineReason::kTeardownPending);
    return result;
  }
  if (PreSrTakes(handle)) return result;
  // Without a complete restore target the injected binds would be the last
  // thing the game records against - the bindless-engine device-removal
  // class.  Decline the frame instead of polluting it (the game's own image
  // passes through; the same policy OptiScaler ships).  A host ClearState
  // does not decline: its aftermath is deterministic, completes the target
  // and is restored exactly (ApplyCapturedComputeState).
  if (!state_envelope.Admit(
          "after-upscale", NrDeclineReason::kStateShadowUnavailable,
          &logged_state_shadow_skip)) {
    return result;
  }
  if (state_envelope.ClearStateInvalidated()
      && !logged_clear_state_inject.exchange(true)) {
    Log(
        reshade::log::level::info,
        "NR injected across a host ClearState; deterministic default-state "
        "restore applied");
  }
  bool command_list_touched = false;
  const int64_t started_ns = SteadyNowNs();
  ProcessInline(command_list, handle, parameters, &command_list_touched);
  RecordInjectionCpu(started_ns);
  if (command_list_touched) state_envelope.MarkInjectionCommandsRecorded();
  return result;
}

template <int Slot>
inline NVSDK_NGX_Result NVSDK_CONV HookedEvaluateFeatureCSlot(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters,
    PFN_NVSDK_NGX_ProgressCallback_C callback) {
  debug::TouchNgxEval();
  ngx_entry_evaluate_c[Slot].fetch_add(1, std::memory_order_relaxed);
  auto real = reinterpret_cast<decltype(&NVSDK_NGX_D3D12_EvaluateFeature_C)>(
      ngx_slot_real[Slot].evaluate_c);
  CallbackScope callback_scope;
  if (!callback_scope) {
    const NgxRuntimeCommandScope runtime_scope;
    return real(command_list, handle, parameters, callback);
  }
  // A-1: as in the C++ wrapper.  The flag stays raised across real() so a
  // C++ evaluate the C export delegates to takes its nested path, which
  // dereferences nothing and counts nothing.
  if (!InsideDirectCall()
      && !NgxFirstArgumentUsable(command_list, "evaluate_c", Slot)) {
    const EvaluateInFlightScope in_flight(true);
    ++intercepted_evaluations;
    CountNrDecline(NrDeclineReason::kImplausibleArgument);
    inside_game_evaluate_wrapper = true;
    const NVSDK_NGX_Result result = [&] {
      const NgxRuntimeCommandScope runtime_scope;
      return real(command_list, handle, parameters, callback);
    }();
    inside_game_evaluate_wrapper = false;
    return result;
  }
  if (!InsideDirectCall() && ForeignD3D12SourceIgnored()) {
    return PassSourceOther(
        [&] { return real(command_list, handle, parameters, callback); });
  }
  const EvaluateChainScope chain_scope;
  const EvaluateInFlightScope in_flight(true);
  ++intercepted_evaluations;
  if (InsideDirectCall()) {
    own_evaluations.fetch_add(1, std::memory_order_relaxed);
  } else {
    // P4 A1: the foreign-source queue-completion install (observe-only; a
    // no-op in every D3D12 session - see the definition).  Outside every
    // lock, like the other wrapper's call.
    EnsureForeignEntryInstalls(command_list);
  }
  // Outermost game evaluate (C export): candidate lifecycle tick while
  // presents starve - see NgxLifecycleTick / HookedEvaluateFeatureSlot.
  if (!InsideDirectCall()) NgxLifecycleTick();
  // Loader work before the lock, for the same reason as the D3D12 wrapper.
  if (!InsideDirectCall()) PrimeNgxLoaderSymbols();
  // Outermost wrapper of the C-export path: run the pre-SR pass here, before
  // the nested C++ evaluate (which the wrapper flag below makes skip it).
  // The re-entrancy check gates the LOCK - see HookedEvaluateFeatureSlot.
  PreSrSwap pre_sr;
  if (!InsideDirectCall()) {
    RuntimeLock lock(runtime_mutex);
    if (enabled.load()
        && handle != nullptr && parameters != nullptr
        && PreSrTakes(handle) && !NrYieldsToForeign()
        && !NrWaitsForTeardown(command_list)
        && RegisteredDlssEvaluate(handle)
        && InstallCommandListStateHooks(command_list)) {
      // Seed-only restore envelope around the injection, gated on a complete
      // target - see the C++ wrapper's pre-SR block (audit issue 03).
      ComputeStateEnvelope pre_sr_envelope(command_list);
      pre_sr_envelope.StopCapture();
      if (pre_sr_envelope.Admit(
              "pre-SR, C export", NrDeclineReason::kPreSrNoStateTarget,
              &logged_presr_state_skip)) {
        bool pre_sr_recorded = false;
        const int64_t started_ns = SteadyNowNs();
        ProcessInlinePreSR(
            command_list, handle, parameters, pre_sr, &pre_sr_recorded);
        RecordInjectionCpu(started_ns);
        if (pre_sr_recorded) pre_sr_envelope.MarkInjectionCommandsRecorded();
      }
    }
  }
  inside_game_evaluate_wrapper = true;
  ComputeStateEnvelope state_envelope(
      InsideDirectCall() ? nullptr : command_list);
  const NVSDK_NGX_Result result = [&] {
    const NgxRuntimeCommandScope runtime_scope;
    return real(command_list, handle, parameters, callback);
  }();
  inside_game_evaluate_wrapper = false;
  state_envelope.StopCapture();
  RestorePreSrColor(parameters, pre_sr);
  if (InsideDirectCall() || NVSDK_NGX_FAILED(result) || !enabled.load()
      || handle == nullptr || parameters == nullptr) {
    return result;
  }

  RuntimeLock lock(runtime_mutex);
  if (!RegisteredDlssEvaluate(handle)) return result;
  if (NrYieldsToForeign()) {
    CountNrDecline(NrDeclineReason::kForeignNr);
    return result;
  }
  if (NrWaitsForTeardown(command_list)) {
    CountNrDecline(NrDeclineReason::kTeardownPending);
    return result;
  }
  if (PreSrTakes(handle)) return result;
  if (!state_envelope.Admit(
          "after-upscale, C export", NrDeclineReason::kStateShadowUnavailable,
          &logged_state_shadow_skip_c)) {
    return result;
  }
  if (state_envelope.ClearStateInvalidated()
      && !logged_clear_state_inject.exchange(true)) {
    Log(
        reshade::log::level::info,
        "NR injected across a host ClearState; deterministic default-state "
        "restore applied");
  }
  bool command_list_touched = false;
  const int64_t started_ns = SteadyNowNs();
  ProcessInline(command_list, handle, parameters, &command_list_touched);
  RecordInjectionCpu(started_ns);
  if (command_list_touched) state_envelope.MarkInjectionCommandsRecorded();
  return result;
}

template <int Slot>
inline NVSDK_NGX_Result NVSDK_CONV HookedReleaseFeatureSlot(NVSDK_NGX_Handle* handle) {
  ngx_entry_release[Slot].fetch_add(1, std::memory_order_relaxed);
  auto real = reinterpret_cast<decltype(&NVSDK_NGX_D3D12_ReleaseFeature)>(
      ngx_slot_real[Slot].release);
  bool foreign_last_feature = false;
  {
    CallbackScope callback_scope;
    if (callback_scope && !InsideDirectCall()) {
      RuntimeLock lock(runtime_mutex);
      // P4 A2: was this the last DLSS feature bound on the NR device?  Read
      // under the lock that just erased; the teardown itself runs after
      // real() below, outside every lock.
      foreign_last_feature = ForgetSourceHandleLocked(handle);
    }
  }
  const NVSDK_NGX_Result release_result = real(handle);
  // P4 A2 (row 25 gap 2, PLAN_DX11_V68.md 4.5): in a FOREIGN session
  // (D3D11-presenting; no D3D12 swapchain will ever exist to trigger
  // OnDestroySwapchain's D3D12 gate) the last DLSS feature release on the
  // NR device is the teardown trigger - the tool released its feature, so
  // NR must shut down, emit its final verdict and let its retirement drain
  // instead of holding everything until process exit.  Scoped to the
  // foreign shape, so a D3D12 game's mid-session release/recreate (Silent
  // Hill 2's settings-change rebuilds) behaves exactly as before.  Re-arm
  // is lazy, like every teardown: hooks reinstall on the next present, the
  // runtime reloads on the next evaluate.
  // Never on the native D3D11 route: NR there belongs to the game's D3D11
  // DLSS, and a tool releasing its D3D12 feature must not tear it down.
  if (foreign_last_feature && direct_device != nullptr
      && !Dx11NativeRoute()
      && !shutting_down.load(std::memory_order_acquire)
      && d3d11_present_seen.load(std::memory_order_relaxed)
      && !d3d12_present_seen.load(std::memory_order_relaxed)) {
    Log(reshade::log::level::info,
        "the last DLSS feature on the NR device was released in a foreign"
        " (D3D11-presenting) session; releasing NR state for re-arm");
    Shutdown();
    RuntimeLock lock(runtime_mutex);
    screenshot::AbortPending();
  }
  return release_result;
}

// Slot -> wrapper function-pointer tables.  MakeNgxHookItems(slot) pairs each
// export name with its slot's wrapper and that slot's own real-storage pointer.
using NgxCreateFn = decltype(&NVSDK_NGX_D3D12_CreateFeature);
using NgxEvaluateFn = decltype(&NVSDK_NGX_D3D12_EvaluateFeature);
using NgxEvaluateCFn = decltype(&NVSDK_NGX_D3D12_EvaluateFeature_C);
using NgxReleaseFn = decltype(&NVSDK_NGX_D3D12_ReleaseFeature);

inline NgxCreateFn const kNgxCreateWrappers[kMaxNgxSlots] = {
    HookedCreateFeatureSlot<0>, HookedCreateFeatureSlot<1>, HookedCreateFeatureSlot<2>,
    HookedCreateFeatureSlot<3>, HookedCreateFeatureSlot<4>, HookedCreateFeatureSlot<5>,
    HookedCreateFeatureSlot<6>, HookedCreateFeatureSlot<7>,
};
inline NgxEvaluateFn const kNgxEvaluateWrappers[kMaxNgxSlots] = {
    HookedEvaluateFeatureSlot<0>, HookedEvaluateFeatureSlot<1>, HookedEvaluateFeatureSlot<2>,
    HookedEvaluateFeatureSlot<3>, HookedEvaluateFeatureSlot<4>, HookedEvaluateFeatureSlot<5>,
    HookedEvaluateFeatureSlot<6>, HookedEvaluateFeatureSlot<7>,
};
inline NgxEvaluateCFn const kNgxEvalCWrappers[kMaxNgxSlots] = {
    HookedEvaluateFeatureCSlot<0>, HookedEvaluateFeatureCSlot<1>, HookedEvaluateFeatureCSlot<2>,
    HookedEvaluateFeatureCSlot<3>, HookedEvaluateFeatureCSlot<4>, HookedEvaluateFeatureCSlot<5>,
    HookedEvaluateFeatureCSlot<6>, HookedEvaluateFeatureCSlot<7>,
};
inline NgxReleaseFn const kNgxReleaseWrappers[kMaxNgxSlots] = {
    HookedReleaseFeatureSlot<0>, HookedReleaseFeatureSlot<1>, HookedReleaseFeatureSlot<2>,
    HookedReleaseFeatureSlot<3>, HookedReleaseFeatureSlot<4>, HookedReleaseFeatureSlot<5>,
    HookedReleaseFeatureSlot<6>, HookedReleaseFeatureSlot<7>,
};

inline std::vector<renodx::utils::vtable::HookItem> MakeNgxHookItems(int slot) {
  return {
      {"NVSDK_NGX_D3D12_CreateFeature",
       reinterpret_cast<void**>(&ngx_slot_real[slot].create),
       reinterpret_cast<void*>(kNgxCreateWrappers[slot])},
      {"NVSDK_NGX_D3D12_EvaluateFeature",
       reinterpret_cast<void**>(&ngx_slot_real[slot].evaluate),
       reinterpret_cast<void*>(kNgxEvaluateWrappers[slot])},
      {"NVSDK_NGX_D3D12_EvaluateFeature_C",
       reinterpret_cast<void**>(&ngx_slot_real[slot].evaluate_c),
       reinterpret_cast<void*>(kNgxEvalCWrappers[slot])},
      {"NVSDK_NGX_D3D12_ReleaseFeature",
       reinterpret_cast<void**>(&ngx_slot_real[slot].release),
       reinterpret_cast<void*>(kNgxReleaseWrappers[slot])},
  };
}

// The NGX core is identified by the module's own file name, not the whole
// path: GetModuleFileNameW always returns an absolute path for a loaded
// module, so comparing the full path against L"nvngx.dll" can never match (a
// System32-core-only process used to report "core present: no" in the overlay
// even though its hooks worked).
inline bool IsCoreNgxModulePath(const std::wstring& lower_path) {
  const size_t slash = lower_path.find_last_of(L"\\/");
  const std::wstring name = slash == std::wstring::npos
      ? lower_path
      : lower_path.substr(slash + 1);
  return name == L"_nvngx.dll" || name == L"nvngx.dll";
}

// Detours one NGX module's exports into the given free slot.  The module is
// recorded so Shutdown can release exactly the modules we patched, and the core
// module is named in the log because it is the authoritative copy.
// Fault injection for the NGX hook install, read once from the environment
// like RENODX_NR_TEST_THROW_HOOKS.  A retry that is never made to retry is
// indistinguishable from the latch it replaced, so the lane that covers it
// has to be able to produce the failure.
inline uint32_t TestNgxHookFailureBudget() {
  static const uint32_t budget = [] {
    char buffer[16] = {};
    size_t length = 0;
    if (getenv_s(&length, buffer, sizeof(buffer),
                 "RENODX_NR_TEST_FAIL_NGX_HOOK") != 0
        || length == 0) {
      return 0u;
    }
    return static_cast<uint32_t>(strtoul(buffer, nullptr, 10));
  }();
  return budget;
}
inline uint32_t test_ngx_hook_failures_left = 0;
inline bool test_ngx_hook_armed = false;

// Caller holds runtime_mutex (every TryHookNgxModule caller does), so this
// needs no atomics.
inline bool ConsumeTestNgxHookFailure() {
  if (!test_ngx_hook_armed) {
    test_ngx_hook_armed = true;
    test_ngx_hook_failures_left = TestNgxHookFailureBudget();
  }
  if (test_ngx_hook_failures_left == 0) return false;
  --test_ngx_hook_failures_left;
  return true;
}

// Arming control for `detour_contended=`.  A counter asserted zero in every
// lane proves nothing until something makes it non-zero, and the case worth
// modelling is the one our own transaction mutex CANNOT fix: a thread
// outside that mutex holding the process-wide Detours transaction while our
// installs run.  RENODX_NR_TEST_HOLD_DETOUR_TX_MS=<n> spawns exactly that,
// waits until it actually owns the transaction, and lets go after n ms.
// What the lane then requires is not that the installs succeed immediately
// but that the session still engages - which is the retry doing its job.
inline uint32_t TestDetourHoldMs() {
  static const uint32_t ms = [] {
    char buffer[16] = {};
    size_t length = 0;
    if (getenv_s(&length, buffer, sizeof(buffer),
                 "RENODX_NR_TEST_HOLD_DETOUR_TX_MS") != 0
        || length == 0) {
      return 0u;
    }
    return static_cast<uint32_t>(strtoul(buffer, nullptr, 10));
  }();
  return ms;
}

inline std::atomic_bool test_detour_hold_started{false};
inline std::atomic_bool test_detour_hold_owned{false};

inline void ArmForeignDetourTransaction() {
  const uint32_t ms = TestDetourHoldMs();
  if (ms == 0 || test_detour_hold_started.exchange(true)) return;
  std::thread([ms] {
    // Deliberately NOT under vtable::TransactionMutex: this is the
    // foreign holder, which is what the counter is for.
    if (DetourTransactionBegin() != NO_ERROR) return;
    test_detour_hold_owned.store(true, std::memory_order_release);
    // Row 22: the fixed window starts at the FIRST COLLISION, not at
    // ownership. Under machine load the phase-1 module scan between the arm
    // and the addon's first BeginTransaction can outlast a blind hold, the
    // collision never happens, and the arming lane fails all three arms with
    // detour_contended=0 (the alpha35 gate failure, preserved as
    // gate.json.fail-detour_contention). transaction_contended rising IS the
    // collision; from that moment the window runs exactly as it always did,
    // so the installs near it still lose and still recover through the
    // backoff, however long the scan took. The cap exists only so a scan
    // that never finishes cannot hold the transaction forever; firing it
    // fails the lane's asserts loudly rather than silently passing.
    const uint64_t contended_at_arm =
        renodx::utils::vtable::transaction_contended.load(
            std::memory_order_relaxed);
    const auto no_collision_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (renodx::utils::vtable::transaction_contended.load(
               std::memory_order_relaxed) == contended_at_arm
           && std::chrono::steady_clock::now() < no_collision_deadline) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    DetourTransactionAbort();
  }).detach();
  // Do not race the race: only return once the transaction is genuinely
  // held, so the collision is a fact rather than a hope.  The budget is a
  // DEADLINE, not a sleep count - sleep_for(1ms) rounds up to the system
  // timer tick (~15.6 ms by default), so 2000 iterations is half a minute
  // of wall clock and not the two seconds it reads as.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!test_detour_hold_owned.load(std::memory_order_acquire)
         && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  std::ostringstream armed;
  armed << "RENODX_NR_TEST_HOLD_DETOUR_TX_MS: a foreign thread holds the"
           " Detours transaction for " << ms
        << " ms from the first collision against it; hook installs in that"
           " window must be counted as contended and retried";
  Log(reshade::log::level::warning, armed.str());
}

// `module_path` (original case, for the log) is resolved by the caller:
// GetModuleFileNameW takes the loader lock and must not run under
// runtime_mutex (see the locking note in InstallHooks).
// Caller must hold runtime_mutex.
inline bool TryHookNgxModule(
    HMODULE module,
    int slot,
    bool primary,
    const std::wstring& module_path) {
  if (module == nullptr || slot < 0 || slot >= kMaxNgxSlots
      || ngx_hooked_modules.count(module) || ngx_slot_used[slot]) {
    return false;
  }
  // A module that failed before is retried, but not on every scan.
  const auto backing_off = ngx_failed_modules.find(module);
  const bool retrying = backing_off != ngx_failed_modules.end();
  if (retrying && present_generation < backing_off->second.next_attempt_present) {
    return false;
  }
  auto items = MakeNgxHookItems(slot);
  // A latched write-off cannot be told apart from "it worked" by any test
  // that never sees a failure, so the harness can make the next N attempts
  // fail without touching the module: RENODX_NR_TEST_FAIL_NGX_HOOK=<n>.
  const bool forced_failure = TestNgxHookFailureBudget() != 0
                              && ConsumeTestNgxHookFailure();
  if (forced_failure || !renodx::utils::vtable::Hook(module, items)) {
    // A silent false here left the module unhooked with no trace: the game
    // runs without NR while the log looks healthy.  Name the module so a
    // debug report pinpoints the failed interception.  The plain export
    // addresses Detours leaves behind are cleared so the slot stays
    // re-hookable (vtable::Hook requires unset real pointers).
    ngx_slot_real[slot] = {};
    NgxHookRetry& retry = ngx_failed_modules[module];
    bool announce = false;
    const uint64_t delay = NoteHookInstallFailure(
        retry.attempts, retry.next_attempt_present, announce);
    if (announce) {
      std::ostringstream failure;
      failure << "detouring NGX module FAILED for "
              << NarrowPath(module_path.c_str())
              << "; its evaluates pass through without NR. Attempt "
              << retry.attempts << ", retrying in " << delay
              << " presents"
              << (delay == kNgxRetryCeilingPresents
                      ? " and hourly after that" : "");
      Log(reshade::log::level::warning, failure.str());
    }
    return false;
  }
  if (retrying) {
    // The whole point of the backoff: say so, so a session that recovered is
    // distinguishable in the log from one that never failed.
    std::ostringstream recovered;
    recovered << "detouring NGX module RECOVERED for "
              << NarrowPath(module_path.c_str()) << " after "
              << backing_off->second.attempts
              << " failed attempt(s); Neural Rendering is live on it";
    Log(reshade::log::level::warning, recovered.str());
    ngx_failed_modules.erase(module);
  }
  ngx_slot_used[slot] = true;
  ngx_slot_module[slot] = module;
  {
    std::wstring lower = module_path;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
    ngx_slot_noncore[slot].store(!IsCoreNgxModulePath(lower),
                                 std::memory_order_relaxed);
  }
  ngx_hooked_modules.insert(module);
  Log(
      reshade::log::level::info,
      "detoured NGX module copy [" + std::to_string(slot) + "] "
          + NarrowPath(module_path.c_str())
          + (primary ? " (core)" : ""));
  if (primary) {
    hooked_ngx_module = module;
  }
  return true;
}

// Presents seen by the hook installer, and the ones on which it took a full
// Toolhelp module snapshot.  The pair is a gate, not a note: the snapshot is
// O(modules) with a large constant (measured 2026-09-20 at 450 modules:
// ~15.5 M thread cycles, about 2 ms, per present), so a build in which the
// second number tracks the first is one that has quietly gone back to
// scanning every frame.  T2 `late_ngx` asserts the ratio.
inline std::atomic_uint64_t module_scan_presents{0};
inline std::atomic_uint64_t module_scans{0};

// Did the process gain or lose a module since the last present?
//
// EnumProcessModules fills as much of the array as fits and reports what it
// WOULD have needed either way - "the number of bytes required to store all
// module handles in the lphModule array" - so a one-element array is enough
// to read 8 bytes per loaded module and nothing else (MS,
// EnumProcessModules).  Only the COUNT is used: the handles it writes are
// not reference-counted and the module behind one can be unloaded before it
// is looked at, and the same page warns the list can change underneath the
// call - both of which make the handles unsafe and neither of which matters
// to a count.
//
// A load and an unload between two presents cancel out and this returns
// false; that is what the every-90-presents scan is still there for.  On
// any failure it returns true, because the expensive-but-correct direction
// is the safe one.  PSAPI_VERSION is 2 on every supported target, so this
// is K32EnumProcessModules in kernel32 - no new import library.
inline bool ProcessModuleSetChanged() {
  static std::atomic<DWORD> last_bytes{0};
  HMODULE first[1] = {};
  DWORD needed = 0;
  NoteLoaderCall("ProcessModuleSetChanged/EnumProcessModules",
                 LoaderCallSafety::kUnsafeUnderLoaderLock);
  if (EnumProcessModules(GetCurrentProcess(), first, sizeof(first), &needed)
      == FALSE) {
    return true;
  }
  return last_bytes.exchange(needed, std::memory_order_relaxed) != needed;
}

// Path B: the D3D11 NGX detours and the Direct3D 11 bridge.
#include "d3d11_bridge.hpp"

inline void InstallHooks() {
  ArmForeignDetourTransaction();
  if (!hooks_enabled.load(std::memory_order_acquire)) return;
  // A-6: DX11Source=off detours nothing until the process shows a D3D12
  // swapchain or present (Dx11SourceOffDefers).  The one-shot line is the
  // witness that the policy was applied, not only parsed.
  if (Dx11SourceOffDefers()) {
    if (D3D11OnlySession() && !logged_dx11_source_off_inert.exchange(true)) {
      Log(reshade::log::level::info,
          "DX11Source=off applied: this process presents through Direct3D 11,"
          " so no NGX export is detoured and Neural Rendering stays off here");
    }
    return;
  }
  // Robust NGX detouring.  We no longer rely on a hard-coded name list alone:
  // we first try the well-known copies, then scan every loaded module matching
  // an allowlist - the NGX core (_nvngx.dll / nvngx.dll) plus DLSS/DLSSD
  // plugins (nvngx_dlss*) - excluding the signed NR runtime nvngx_dlssnr.dll
  // and every frame-generation plugin (nvngx_dlssg*), which NR never processes
  // and which must stay undetoured for the game's DLSSG path.
  // This catches game-local plugin copies and any renamed or driver-variant
  // module (e.g. _nvngx_dlss.dll) automatically.
  // Unrelated NGX plugins stay undetoured: DeepDVC and friends sit on the
  // frame-generation path, and a detour there buys NR nothing.
  // Each detoured module gets a private slot so it always calls its own
  // original, and a plugin forwarder terminates at the true core instead of
  // looping back into the hook.  Older DLSS 2.x titles (e.g. Control) implement
  // NGX entirely inside nvngx_dlss.dll and never route create through the
  // _nvngx.dll core, so only scanning the real plugin copy lets their DLSS
  // create be intercepted.
  //
  // Locking: discovery (phase 1) runs WITHOUT runtime_mutex because
  // GetModuleHandleW, the toolhelp snapshot, and GetModuleFileNameW take the
  // loader lock - holding runtime_mutex across such a wait can deadlock with
  // this addon's own DLL_PROCESS_ATTACH, which holds the loader lock for the
  // whole of DllMain while its InstallHooks call waits on runtime_mutex below.
  // Phase 2 then hooks under runtime_mutex (serialized against Shutdown's
  // unhook and every hook entry point) using only loader-lock-free calls
  // (GetProcAddress + Detours transactions).  try_lock: a contended mutex
  // means an evaluate is in flight; installation just retries on a later
  // present.
  static std::atomic<uint32_t> present_count{0};
  const uint32_t count =
      present_count.fetch_add(1, std::memory_order_relaxed) + 1;

  // One-time diagnostic: dump every loaded module mentioning "ngx" so a missing
  // or unexpectedly-named plugin is visible in the log instead of failing silently.
  {
    static std::atomic<bool> diagnostics_logged{false};
    // Not from DllMain: Tool Help synchronises with the loader from the
    // outside, and the FIRST call into this function is the one from
    // DLL_PROCESS_ATTACH, which holds the loader lock.  Short-circuit, so
    // the latch is not spent either - the snapshot is taken on the first
    // present instead, where it is free to walk the loader list.
    if (!InsideDllMain() && !diagnostics_logged.exchange(true)) {
      std::string found = "(none)";
      NoteLoaderCall("InstallHooks/diagnostic-snapshot",
                     LoaderCallSafety::kUnsafeUnderLoaderLock);
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
      if (snap != INVALID_HANDLE_VALUE) {
        found = "";
        MODULEENTRY32W me; me.dwSize = sizeof(me);
        for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
          std::wstring w(me.szModule);
          std::transform(w.begin(), w.end(), w.begin(),
                         [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
          if (w.find(L"ngx") != std::wstring::npos) {
            found += "  " + NarrowPath(me.szModule) + "\n";
          }
        }
        CloseHandle(snap);
      }
      Log(reshade::log::level::info, "NGX module scan (loaded copies):\n" + found);
    }
  }

  // Phase 1 - module discovery (no runtime_mutex; see the locking note above).
  struct NgxCandidate {
    HMODULE module;
    bool is_core;
    bool by_scan;
    std::wstring log_path;  // original-case full path, for the detour log
    std::wstring display;   // module name, for the by-scan / slot-full logs
  };
  static thread_local std::vector<NgxCandidate> candidates;
  candidates.clear();
  const auto push_candidate = [&](HMODULE module, bool by_scan, const wchar_t* display) {
    if (module == nullptr) return;
    for (const auto& existing : candidates) {
      if (existing.module == module) return;
    }
    std::wstring path(MAX_PATH + 1, L'\0');
    const DWORD written = GetModuleFileNameW(module, path.data(), MAX_PATH);
    if (written == 0) return;
    path.resize(written);
    std::wstring lower = path;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
    candidates.push_back(
        {module, IsCoreNgxModulePath(lower), by_scan, std::move(path), display});
  };

  // 1) Well-known names (cheap, every present).
  constexpr const wchar_t* kNgxModuleNames[] = {
      L"_nvngx.dll",
      L"nvngx.dll",
      L"nvngx_dlss.dll",
      L"nvngx_dlssd.dll",
      L"_nvngx_dlss.dll",
      L"_nvngx_dlssd.dll",
  };
  NoteLoaderCall("InstallHooks/GetModuleHandleW");
  for (const wchar_t* name : kNgxModuleNames) {
    push_candidate(GetModuleHandleW(name), false, name);
  }

  // 2) Full module scan: the first few presents (catch a plugin that loads
  //    during start-up), every 90 presents thereafter, and any present on
  //    which the process gained or lost a module.
  //
  //    The scan is a Toolhelp snapshot, and the runtime copies a
  //    MODULEENTRY32W - 1080 bytes, two embedded path buffers - for EVERY
  //    module in the process.  Until v6.8.0-alpha10 the condition ended in
  //    `|| !ngx_any_hooked`, i.e. it ran on EVERY present for as long as
  //    nothing was hooked: every present before the player turns DLSS on,
  //    and every present of every session in which they never do.
  //
  //    Measured 2026-09-20 on the T2 `late_ngx` profile, in a process
  //    ballasted to 450 modules (a game-sized module list; SynthHost alone
  //    carries ~90), three runs per arm, alternated.  Per present, p50
  //    thread cycles - the presenting thread blocks, so wall time cannot
  //    see this and cycles can:
  //
  //      no addon         993,580 / 1,098,390 / 1,088,990    1.76-1.81 ms
  //      addon, before 16,413,340 / 17,394,700 / 17,688,920  3.55-3.82 ms
  //      addon, after   2,091,500 /  2,105,130 /  2,255,530  1.71-1.78 ms
  //
  //    The addon cost +16,305,710 cycles per present - sixteen times the
  //    present itself, about 2 ms on every frame of a game that has not
  //    loaded DLSS yet - and costs +791,480 after, which is 95% less and
  //    puts its wall time back inside the no-addon arm.  (The residual is
  //    the rest of the present handler plus this probe; it has not been
  //    split, and the probe is itself O(modules).)
  //
  //    Dropping the clause outright and keeping only the schedule is not
  //    the fix, but NOT for the reason first assumed, and the assumption
  //    was wrong twice before it was measured.  It does not cost
  //    engagement: built that way `late_ngx` scores seen=240 of 240,
  //    identical to the real fix, because step 1 above is
  //    GetModuleHandleW over six well-known names and runs on every
  //    present whatever this clause says - so a module called
  //    nvngx_dlss.dll is hooked the present after it loads either way.
  //    What the snapshot is FOR is the copies that lookup cannot name:
  //    renamed and driver-variant nvngx modules.  Its duty is therefore
  //    to notice that the process gained a module, and on the schedule
  //    alone it notices up to 90 presents late.  Measured on the same
  //    two binaries, alternated real/naive/real, 12 modules loaded one
  //    per present during the run: this build makes 14 scans it was not
  //    scheduled to make (both real runs), the schedule-only build
  //    makes 0 (T-PROBE, test/dlss5_e2e/tests.cpp).
  //    So the clause is replaced by the cheap question it was really
  //    asking: did anything get loaded?
  //    The probe is ordered LAST on purpose.  It walks the loader list, and
  //    the first few calls here can arrive from DLL_PROCESS_ATTACH, which
  //    holds the loader lock on this very thread (see the locking note
  //    above) - so on any present that is going to take the full snapshot
  //    anyway, short-circuit evaluation means the probe does not run at
  //    all.  `last_bytes` still means "the count as of the last probe", so
  //    nothing goes stale: a module that loads during a scheduled scan is
  //    found by that scan, and one that loads after it changes the count.
  //
  //    Never from DllMain, and that leading term is why the probe is not
  //    merely ordered last but excluded: EnumProcessModules is the same
  //    family as the snapshot it guards.  Nothing is lost by skipping the
  //    attach call - `count <= 5` means presents 1-4 scan instead, and at
  //    attach time the game has not created DLSS yet, so the modules a
  //    scan would find are not loaded.  What the attach call still does is
  //    the well-known-name lookups and the Detours transactions, which are
  //    legal there (runtime_lock.hpp says why, from the Detours source).
  const bool do_full_scan =
      !InsideDllMain()
      && ((count <= 5) || (count % 90 == 0) || ProcessModuleSetChanged());
  module_scan_presents.fetch_add(1, std::memory_order_relaxed);
  if (do_full_scan) module_scans.fetch_add(1, std::memory_order_relaxed);
  if (do_full_scan) {
    NoteLoaderCall("InstallHooks/CreateToolhelp32Snapshot",
                   LoaderCallSafety::kUnsafeUnderLoaderLock);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
      MODULEENTRY32W me; me.dwSize = sizeof(me);
      for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        std::wstring w(me.szModule);
        std::transform(w.begin(), w.end(), w.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        // DX11Source=auto's one signal: a DX11 bridge add-on of another
        // project keeps the session on the foreign route it was built for.
        if ((w.rfind(L"dlss5-bridge", 0) == 0 || w.rfind(L"dlss5-feed", 0) == 0)
            && w.find(L".addon") != std::wstring::npos) {
          foreign_dx11_tool_seen.store(true, std::memory_order_relaxed);
        }
        if (w.find(L"nvngx") == std::wstring::npos) continue;   // only NGX copies
        if (w.find(L"dlssnr") != std::wstring::npos) continue;   // never hook our NR runtime
        if (w.find(L"dlssg") != std::wstring::npos) continue;    // never detour the frame-generation plugin
        if (w.find(L"deepdvc") != std::wstring::npos) continue;  // frame-generation pipeline plugin
        // Compatibility-first allowlist (executive decision): any OTHER
        // nvngx* module is a hook candidate - driver variants, renamed or
        // versioned plugin copies the exact-name list cannot know.  A module
        // that implements only some of the four exports still gets those
        // intercepted (vtable::Hook attaches what resolves); the absolute
        // exclusions above are the plugins whose detour would break their
        // pipeline or re-enter our own runtime.
        push_candidate(me.hModule, true, me.szModule);
      }
      CloseHandle(snap);
    }
  }
  // After the scan (auto's tool detection needs one), before the empty
  // check: a D3D11 process that never loads NGX still states its route.
  DecideDx11Route();
  if (candidates.empty()) return;

  // Phase 2 - hook the candidates under runtime_mutex.  Already-hooked
  // modules are rejected by TryHookNgxModule's own dedup check.
  RuntimeTryLock lock(runtime_mutex);
  if (!lock.owns_lock()) return;
  if (shutting_down.load(std::memory_order_acquire)) return;
  bool primary_set = (hooked_ngx_module != nullptr);
  bool hooked_any = false;
  for (const auto& candidate : candidates) {
    int slot = -1;
    for (int s = 0; s < kMaxNgxSlots; ++s) {
      if (!ngx_slot_used[s]) { slot = s; break; }
    }
    if (slot < 0) {
      Log(
          reshade::log::level::error,
          "all NGX detour slots in use; cannot hook "
              + NarrowPath(candidate.display.c_str()));
      break;
    }
    const bool primary = candidate.is_core && !primary_set;
    if (TryHookNgxModule(candidate.module, slot, primary, candidate.log_path)) {
      hooked_any = true;
      if (primary) primary_set = true;
      if (candidate.by_scan) {
        Log(reshade::log::level::info,
            "hooked NGX module by scan: " + NarrowPath(candidate.display.c_str()));
      }
    }
  }
  if (hooked_any && !ngx_any_hooked.load(std::memory_order_relaxed)) {
    ngx_any_hooked.store(true, std::memory_order_relaxed);
    int modules = 0;
    for (int s = 0; s < kMaxNgxSlots; ++s) if (ngx_slot_used[s]) ++modules;
    // A-3 (row 24): in a process that presents through D3D11 these are still
    // the D3D12 exports - they serve a third-party tool's D3D12 evaluates
    // on the foreign route, and pass them through on the native one, never
    // the game's own D3D11 DLSS.  Said here so the line cannot read as "the
    // game's DLSS is hooked".  D3D11OnlySession, not presents alone: the
    // swapchain's init now runs this installer before the first present.
    Log(
        reshade::log::level::info,
        "D3D12 NGX hooks installed across " + std::to_string(modules)
            + " module copy(ies); inline DLSS contract capture armed"
            + (!D3D11OnlySession() ? std::string()
               : Dx11NativeRoute()
                   ? std::string(
                         " (this process presents through Direct3D 11 and is"
                         " served on the native route: a third-party tool's"
                         " D3D12 evaluates pass through untouched)")
                   : std::string(
                         " (this process presents through Direct3D 11: the"
                         " hooks serve a third-party tool's D3D12 evaluates;"
                         " the game's own D3D11 DLSS is not detoured)")));
  }
  // Path B: the game's own D3D11 DLSS, on the native route only.
  if (Dx11NativeRoute()) {
    bool hooked11 = false;
    for (const auto& candidate : candidates) {
      if (TryHookNgx11Module(candidate.module, candidate.log_path)) hooked11 = true;
    }
    if (hooked11 && !ngx11_any_hooked.exchange(true, std::memory_order_relaxed)) {
      int modules = 0;
      for (int s = 0; s < kMaxNgxSlots; ++s) if (ngx11_slot_used[s]) ++modules;
      Log(reshade::log::level::info,
          "D3D11 NGX hooks installed across " + std::to_string(modules)
              + " module copy(ies): Neural Rendering reaches the game's own"
                " Direct3D 11 DLSS through the Direct3D 11 bridge, before or"
                " after its upscale (NRPreUpscale)");
    }
  }
  hooks_installed = ngx_any_hooked.load(std::memory_order_relaxed)
                    || ngx11_any_hooked.load(std::memory_order_relaxed);
}

// Endfield: the vendor may call SL tagging while NR create owns the plain
// runtime mutex. Internal callbacks only forward; they must never acquire it.
inline std::atomic_uint64_t streamline_internal_callbacks{0};
inline bool StreamlineInternalCallback() {
  if (!InsideDirectCall() && !runtime_lock::State::HeldByThisThread()) return false;
  streamline_internal_callbacks.fetch_add(1, std::memory_order_relaxed);
  return true;
}

inline sl::Result HookedStreamlineEvaluate(
    sl::Feature feature,
    const sl::FrameToken& frame,
    const sl::BaseStructure** inputs,
    uint32_t num_inputs,
    sl::CommandBuffer* command_buffer) {
  CallbackScope callback_scope;
  // ENV-02: SL owns the proxy/native choice. Forward its original buffer;
  // unwrapping before the real call breaks another interposer's contract.
  if (!callback_scope || StreamlineInternalCallback()
      || real_streamline_evaluate == nullptr) {
    return real_streamline_evaluate != nullptr
        ? real_streamline_evaluate(
              feature, frame, inputs, num_inputs, command_buffer)
        : sl::Result::eErrorInvalidParameter;
  }
  const EvaluateChainScope chain_scope;
  ++intercepted_streamline_evaluations;
  if (feature == sl::kFeatureDLSS || feature == sl::kFeatureDLSS_RR) {
    ++intercepted_streamline_dlss_evaluations;
  }
  const uint64_t nr_before = successful_evaluations.load();
  const sl::Result result = real_streamline_evaluate(
      feature, frame, inputs, num_inputs, command_buffer);
  if (result != sl::Result::eOk
      || (feature != sl::kFeatureDLSS && feature != sl::kFeatureDLSS_RR)
      || NrYieldsToForeign()
      // Per-evaluate mode: a nested NGX evaluate that owns the stream keeps it
      // on frames it declined too (warm-up, backoff) - the global counter
      // read those as unhandled and engaged a second feature 18 under a
      // synthetic handle, and raced other threads' evaluates.
      || (dedupe_mode.load(std::memory_order_relaxed) == kDedupePerEvaluate
              ? evaluate_chain.ngx_owned
              : successful_evaluations.load() != nr_before)) {
    return result;
  }

  if (command_buffer == nullptr) return result;
  auto* unknown = reinterpret_cast<IUnknown*>(command_buffer);
  ID3D12GraphicsCommandList* command_list = nullptr;
  if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&command_list)))
      || command_list == nullptr) {
    return result;
  }
  // Loader work before the lock, as on the NGX evaluate paths.
  PrimeNgxLoaderSymbols();
  RuntimeLock lock(runtime_mutex);
  if (NrWaitsForTeardown(command_list)) {
    CountNrDecline(NrDeclineReason::kTeardownPending);
    command_list->Release();
    return result;
  }
  const StreamlineCapture capture = BuildStreamlineCapture(frame, inputs, num_inputs);
  ++streamline_direct_fallback_attempts;
  // Same decline-instead-of-pollute gate as the NGX paths.  The list only
  // arrives after the real evaluate, so the restore target is its persistent
  // shadow read now: the host's own state, which since v6.8.0-alpha9 is all
  // the shadow holds - the runtime's binds inside an evaluate raise
  // NgxRuntimeCommandScope and are not recorded.
  // (Until v6.3.0 this path armed a thread-wide capture window around the
  // evaluate, which can never see the host's earlier binds.)  The envelope
  // also installs the hooks from this list, arming future evaluates.  The
  // restore runs at the end of the block, before the QI reference drops.
  {
    ComputeStateEnvelope state_envelope(command_list);
    state_envelope.StopCapture();
    if (state_envelope.Admit(
            "Streamline fallback", NrDeclineReason::kStateShadowUnavailable,
            &logged_streamline_shadow_skip)) {
      bool command_list_touched = false;
      ProcessStreamlineInline(
          command_list, feature, capture, &command_list_touched);
      if (command_list_touched) state_envelope.MarkInjectionCommandsRecorded();
    }
  }
  command_list->Release();
  return result;
}

inline sl::Result HookedStreamlineSetTag(
    const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags,
    uint32_t num_tags,
    sl::CommandBuffer* command_buffer) {
  CallbackScope callback_scope;
  if (!callback_scope || StreamlineInternalCallback()) {
    return real_streamline_set_tag(viewport, tags, num_tags, command_buffer);
  }
  ++intercepted_streamline_tag_calls;
  RecordStreamlineTagBatch(
      num_tags, StreamlineTagMask(tags, num_tags));
  {
    RuntimeLock lock(runtime_mutex);
    CaptureStreamlineTagSet(
        static_cast<uint32_t>(viewport), tags, num_tags, present_generation);
  }
  return real_streamline_set_tag(viewport, tags, num_tags, command_buffer);
}

inline sl::Result HookedStreamlineSetTagForFrame(
    const sl::FrameToken& frame,
    const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags,
    uint32_t num_tags,
    sl::CommandBuffer* command_buffer) {
  CallbackScope callback_scope;
  if (!callback_scope || StreamlineInternalCallback()) {
    return real_streamline_set_tag_for_frame(
        frame, viewport, tags, num_tags, command_buffer);
  }
  ++intercepted_streamline_tag_calls;
  RecordStreamlineTagBatch(
      num_tags, StreamlineTagMask(tags, num_tags));
  {
    RuntimeLock lock(runtime_mutex);
    CaptureStreamlineTagSet(
        static_cast<uint32_t>(viewport), tags, num_tags,
        static_cast<uint32_t>(frame));
  }
  return real_streamline_set_tag_for_frame(
      frame, viewport, tags, num_tags, command_buffer);
}

inline const std::vector<renodx::utils::vtable::HookItem> kStreamlineHooks = {
    {"slEvaluateFeature",
     reinterpret_cast<void**>(&real_streamline_evaluate),
     reinterpret_cast<void*>(&HookedStreamlineEvaluate)},
    {"slSetTag",
     reinterpret_cast<void**>(&real_streamline_set_tag),
     reinterpret_cast<void*>(&HookedStreamlineSetTag)},
    {"slSetTagForFrame",
     reinterpret_cast<void**>(&real_streamline_set_tag_for_frame),
     reinterpret_cast<void*>(&HookedStreamlineSetTagForFrame)},
};

inline void InstallStreamlineHooks() {
  if (!hooks_enabled.load(std::memory_order_acquire)
      || !streamline_hooks_enabled.load(std::memory_order_acquire)
      || Dx11SourceOffDefers()) {
    return;
  }
  if (streamline_hooks_installed.load(std::memory_order_relaxed)) return;
  // ENV-02: hook the implementation only. sl.interposer is a forwarder
  // shared with other injected mods; detouring it can corrupt that stack.
  // The independent NGX hooks remain available when sl.common is absent.
  constexpr const wchar_t* kStreamlineModuleNames[] = {
      L"sl.common.dll",
  };
  // Discovery without runtime_mutex (GetModuleHandleW takes the loader lock;
  // see the locking note in InstallHooks); the hook attempt itself runs under
  // the mutex so it serializes against Shutdown's unhook.
  HMODULE found[2] = {};
  int found_count = 0;
  for (const wchar_t* name : kStreamlineModuleNames) {
    NoteLoaderCall("InstallStreamlineHooks/GetModuleHandleW");
    HMODULE module = GetModuleHandleW(name);
    if (module != nullptr && found_count < 2) found[found_count++] = module;
  }
  for (int i = 0; i < found_count; ++i) {
    RuntimeTryLock lock(runtime_mutex);
    if (!lock.owns_lock()) return;
    if (shutting_down.load(std::memory_order_acquire)) return;
    if (streamline_hooks_installed.load(std::memory_order_relaxed)) return;
    // Marker so a crash during installation is identifiable from the log.
    Log(
        reshade::log::level::info,
        "installing Streamline hooks into "
            + NarrowPath(kStreamlineModuleNames[i]) + "...");
    if (renodx::utils::vtable::Hook(found[i], kStreamlineHooks)) {
      streamline_hooks_installed.store(true, std::memory_order_relaxed);
      hooked_streamline_module = found[i];
      Log(
          reshade::log::level::info,
          "Streamline hooks installed in " + NarrowPath(kStreamlineModuleNames[i])
              + "; NGX/Streamline deduplication armed");
      return;
    }
    // A failed commit leaves the plain export addresses in the real
    // pointers; clear all three so the per-present retry (and the other
    // module candidate) starts from unset pointers.
    real_streamline_evaluate = nullptr;
    real_streamline_set_tag = nullptr;
    real_streamline_set_tag_for_frame = nullptr;
  }
  // Retried per present until found; report the absence once (~10 s in) so
  // direct-NGX games like KCD2 are identifiable from the log.
  const uint32_t attempt = streamline_hook_attempts.fetch_add(1) + 1;
  if (attempt == 600u && !logged_no_streamline.exchange(true)) {
    Log(
        reshade::log::level::info,
        "no Streamline common module loaded; Streamline fallback hooks "
        "skipped (game calls NGX directly)");
  }
}

// List-destruction untracking (audit issue 04) via ReShade's own event
// instead of a raw IUnknown::Release detour.  FIELD STATUS (2026-09-16
// boot-canary bisect): the C-series freezes were later attributed to the
// historical typed-SRV exposure probe, NOT to this handler - but
// no canary ever ran with the handler registered AND the probe gone, so it
// is still unregistered in the field-proven configuration (C9).  Re-landing
// it requires a fresh boot-canary pass; the teardown drains the shadow and
// the v6audit2 configuration lived without destruction tracking.  Kept
// compiled for the harness.
// What going without it costs (harness S12, 2026-09-18): D3D12 recycles list
// addresses readily, and a list created at a dead list's address inherits
// its shadow entry - a same-signature rebind keeps the dead list's root
// arguments by the first-sight rule - until that list's first observed
// Reset/ClearState rewrites the baseline.  Engines Reset a list before
// recording into it, which is why the field has not shown it; a title that
// records into a freshly created list at a recycled address would restore
// stale arguments.
// The addon's ONLY list-destruction signal, and until v6.8.0 it was written
// and then never registered - the handler below existed, the comment beside
// InstallCommandListStateHooks said destruction "is tracked through" it, and
// nothing called it in any session.  What that cost is measured, on a named
// field shape: `no_seed` (007 First Light / AW2 rs=0) records each evaluate
// on a list CREATED that frame, so the tracker gained one entry and one
// OPEN generation per frame and could drop neither - an open generation is
// never prunable, and an entry is erased only once it is `destroyed` AND has
// no generations left.  Measured 2026-09-20 on the T2 lane at frame 140:
// `tracker[lists=140 gens=140 open=140]`, against `lists=1 gens=1` on a
// profile that reuses one list.  `PruneCompletedGenerations` walks every
// entry from the present path, so the walk grew with the session too, and
// every resource those generations used stayed retained for good.
//
// Three things make re-registering it safe, and all three are why the old
// body could not be:
//
//   * ReShade raises this from `~D3D12GraphicsCommandList`, so the object is
//     mid-destruction.  `submission::OnCommandListDestroyed` resolves its
//     key through a pointer table and never touches the list (see
//     identity_by_pointer).  `EraseListShadow` is NOT called: in the default
//     object-attached mode it would `GetPrivateData` on a dying list, and
//     that mode needs no help - D3D12 releases the attached state object as
//     part of the same destruction.  The striped mode keys its entries by
//     the pointer the RECORD side used, which is not the one below, and has
//     its own idle sweep.
//   * The event fires for every graphics API ReShade supports.  A D3D11
//     deferred context reinterpret_cast to ID3D12GraphicsCommandList* is a
//     call through an unrelated vtable; the API gate is what stops it.
//   * `get_native()` is ReShade's `_orig`, the NATIVE list - which is the
//     alias the queue detour recorded for every list it ever submitted
//     (reshade_api_object_impl.hpp:79: `return (uint64_t)_orig;`).  The old
//     body cast the ReShade `api::command_list*` itself, which is a
//     different base of the same object and therefore a key nothing stored.
//   * Lock order: the handler takes `tracker_mutex` and nothing else, adds
//     no edge to the order runtime_lock.hpp owns, and makes no call into
//     the Windows loader.  ReShade raises this event from
//     ~D3D12GraphicsCommandList, on whatever thread the game destroys the
//     list on, so what matters is whether anything reachable under that
//     mutex can re-enter this addon.  Nothing can: the only ReShade code
//     any tracker_mutex holder reaches is
//     D3D12GraphicsCommandList::{Get,Set}PrivateData (from ListIdentity,
//     which TrackUse calls under the lock), and those forward straight to
//     `_orig` without raising an addon event
//     (external/reshade/source/d3d12/d3d12_command_list.cpp:129-139).
//     This handler does not even do that - the list may already be dead,
//     so it goes through the pointer alias table instead.
inline void OnDestroyCommandListEvent(reshade::api::command_list* cmd_list) {
  if (cmd_list == nullptr) return;
  reshade::api::device* device = cmd_list->get_device();
  if (device == nullptr
      || device->get_api() != reshade::api::device_api::d3d12) {
    return;
  }
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(
      static_cast<uintptr_t>(cmd_list->get_native()));
  if (native == nullptr) return;
  submission::OnCommandListDestroyed(native);
}

// Runs a pending teardown once its proof holds.  Called where the game is
// not inside a hook of ours: presents, swapchain destroys and queue destroys
// (never NgxLifecycleTick, which runs inside an evaluate's CallbackScope and
// would wait on itself in Shutdown's drain).  ReShade raises present and
// destroy_swapchain holding the present queue's lock (dxgi_swapchain.cpp,
// on_present and on_reset), so a game thread submitting to that queue waits
// out the drain - which is why Shutdown bounds it by the clock.  Returns
// whether it released.
inline bool ServicePendingTeardown() {
  if (!teardown_pending.load(std::memory_order_acquire)
      || !TeardownProofHolds() || !Shutdown(/*proof_gated=*/true)) {
    return false;
  }
  RuntimeLock lock(runtime_mutex);
  screenshot::AbortPending();
  return true;
}

// A destroyed queue leaves the lease tracker (gpu_lease.hpp,
// ForgetQueueCompletion); ReShade raises this before the final Release.
// It is also a teardown service point: a game exiting destroys its queues
// after its last present, and by then its submissions have landed.
inline void OnDestroyCommandQueueEvent(reshade::api::command_queue* queue) {
  GuardHook([&] {
    if (queue == nullptr) return;
    reshade::api::device* device = queue->get_device();
    if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12) {
      return;
    }
    if (ForgetQueueCompletion(reinterpret_cast<ID3D12CommandQueue*>(
            static_cast<uintptr_t>(queue->get_native())))) {
      Log(reshade::log::level::info,
          "a tracked command queue was destroyed; it leaves the completion"
          " tracker and its fence settles once its last submission completes");
    }
  });
  if (!shutting_down.load(std::memory_order_acquire)) {
    GuardHook([] { ServicePendingTeardown(); });
  }
}

// The D3D12 half of device init: the birth-time device hooks and the
// DirectX library prime.  Factored out at A-6 so DX11Source=off can run it
// at the first D3D12 swapchain instead (OnInitSwapchain); every step is
// idempotent.
inline void ServeD3D12Device(reshade::api::device* device) {
  d3d12_device_seen.store(true, std::memory_order_release);
  // Row 17: pay the DirectX library burst (renodx::utils::directx::
  // Initialize's first call loads up to seven system DLLs) here, where no
  // runtime_mutex is held.  The codec pipeline and telemetry's
  // QueryVideoMemory reach the same call from under the lock; after this
  // prime their call no-ops behind the utility's initialized guard, so the
  // instrumented note names this site for the burst and never theirs.
  // Gated on hooks_enabled like the NR-runtime load below: with hooks off
  // the addon is contracted to make zero loader calls, and no code path
  // that needs these libraries will run.
  if (hooks_enabled.load()) {
    NoteFirstDirectxInitialize();
    renodx::utils::directx::Initialize();
  }
  InstallDeviceStateHooks(reinterpret_cast<ID3D12Device*>(device->get_native()));
}

// Eagerly load the signed NR runtime at device init, OUTSIDE the detoured
// evaluate call chain. Loading nvngx_dlssnr.dll (and patching its caller
// identity IAT) from inside Hooked*Slot -> ProcessInline runs the load while
// a hooked NGX export is on the stack; if the runtime's init calls back into
// a detoured NGX export this deadlocks on the loader lock (freeze on boot,
// no ReShade.log). v2.5 initialized here instead, which is why it was stable.
// Item 21: the load is for the D3D12 injection paths alone, and the device
// hooks have carried exactly this gate since alpha7, so the callers pass a
// D3D12 device only.  A D3D11-only process (GTA5 legacy field shape) no
// longer takes the load and the IAT patch it could never use; a D3D12 game
// whose launcher d3d11 device inits first still gets the eager load, on its
// d3d12 device init, before any evaluate can exist.
inline void PreloadNrRuntime() {
  if (!hooks_enabled.load()) return;
  // The load state machine fails fast after the first attempt, so only
  // report the outcome when it actually changed - one line, not one per
  // device.
  //
  // Until v6.8.0 this took runtime_mutex, because "LoadDirectApi writes
  // non-atomic state (direct_api, direct_load_state, the sha256 string)
  // that EnsureDirectRuntime reads under runtime_mutex" - true, and it put
  // LoadLibraryW under the mutex, which is the inversion this release
  // removes.  The loader half now runs first with NO lock held and
  // publishes atomically; LoadDirectApi keeps the mutex and only adopts it.
  const DirectLoadState load_state_before = direct_load_state;
  PrimeNgxLoaderSymbols();
  RuntimeLock lock(runtime_mutex);
  if (LoadDirectApi()) {
    if (load_state_before != DirectLoadState::Ready) {
      Log(reshade::log::level::info, "signed NR runtime (nvngx_dlssnr.dll) pre-loaded at device init");
    }
  } else if (load_state_before != DirectLoadState::Failed) {
    Log(
        reshade::log::level::warning,
        "signed NR runtime pre-load failed; will retry lazily on first evaluate");
  }
}

// A-6: set when DX11Source=off held a D3D12 device's init back; the first
// D3D12 swapchain runs it (OnInitSwapchain).
inline std::atomic_bool dx11_source_off_device_deferred{false};

inline void OnInitDevice(reshade::api::device* device) {
  // The Direct3D 11 bridge's own private device (ReShade wraps it like any
  // other): not a D3D12 game, so none of the D3D12 half below applies.
  if (bridge_creating_device) return;
  debug::Mark("init_device:begin");
  InstallHooks();
  const bool d3d12_device =
      device != nullptr && device->get_api() == reshade::api::device_api::d3d12;
  // A-6: under DX11Source=off a D3D12 device is not yet a D3D12 game - a
  // third-party tool in a D3D11 game creates one too - so its half waits
  // for a D3D12 swapchain.
  const bool defer_d3d12 = d3d12_device
                           && hooks_enabled.load(std::memory_order_acquire)
                           && Dx11SourceOffDefers();
  if (defer_d3d12) {
    dx11_source_off_device_deferred.store(true, std::memory_order_release);
    static std::atomic_bool logged_deferral{false};
    if (!logged_deferral.exchange(true)) {
      Log(reshade::log::level::info,
          "DX11Source=off: a Direct3D 12 device was created before any"
          " Direct3D 12 swapchain; its hooks and the NR runtime wait for one"
          " (a third-party tool's device in a Direct3D 11 game never gets"
          " them)");
    }
  }
  // Birth-time observation (R2): the device hooks go on HERE, before the
  // application has created a single command list - the one moment at which
  // patching those function bodies races with no recording thread, and the
  // only way every list in the session can carry a KNOWN baseline from its
  // first instruction.  Games create several devices (a d3d12 one plus a
  // launcher or overlay d3d11 one); only a D3D12 device has the vtable these
  // slot indices refer to, and reading slot 12 off a d3d11 device would
  // detour whatever happens to live there.
  if (d3d12_device && !defer_d3d12) {
    ServeD3D12Device(device);
  }
  InstallStreamlineHooks();
  if (d3d12_device && !defer_d3d12) {
    PreloadNrRuntime();
  }
  // A new device means any in-flight capture's readback buffers belong to
  // the old device. Abort it - the screenshot worker releases the buffers
  // after the settle window, or immediately on shutdown.
  RuntimeLock lock(runtime_mutex);
  screenshot::AbortPending();
}

// A-6: the swapchain's API is the earliest reliable answer to "which API
// does this game present through" - before the first present, and before a
// foreign tool's present callback can evaluate.  Under DX11Source=off the
// first D3D12 swapchain also runs the device half held back at init_device.
inline void OnInitSwapchain(reshade::api::swapchain* swapchain, bool /*resize*/) {
  if (swapchain == nullptr) return;
  reshade::api::device* const device = swapchain->get_device();
  if (device == nullptr) return;
  if (device->get_api() == reshade::api::device_api::d3d11) {
    d3d11_swapchain_seen.store(true, std::memory_order_relaxed);
    // The earliest moment the D3D11 route can be decided, and the D3D11
    // detours installed: before the game's first frame and, in most games,
    // before its DLSS create.
    InstallHooks();
    return;
  }
  if (device->get_api() != reshade::api::device_api::d3d12) {
    other_api_seen.store(true, std::memory_order_relaxed);
    return;
  }
  d3d12_swapchain_seen.store(true, std::memory_order_relaxed);
  if (dx11_source.load(std::memory_order_relaxed) != kDx11SourceOff) return;
  InstallHooks();
  if (dx11_source_off_device_deferred.exchange(false, std::memory_order_acq_rel)) {
    Log(reshade::log::level::info,
        "DX11Source=off: a Direct3D 12 swapchain appeared, so this is served"
        " as a Direct3D 12 game; the device hooks held back at device init go"
        " on now");
    ServeD3D12Device(device);
    InstallStreamlineHooks();
    PreloadNrRuntime();
  }
}

// Human-readable name for a virtual-key code (overlay buttons, rebind log).
inline std::string VirtualKeyName(uint32_t virtual_key) {
  char name[64] = {};
  const UINT scan_code =
      MapVirtualKeyA(static_cast<UINT>(virtual_key), MAPVK_VK_TO_VSC);
  if (scan_code != 0
      && GetKeyNameTextA(scan_code << 16, name, static_cast<int>(sizeof(name))) > 0
      && name[0] != '\0') {
    return name;
  }
  return "0x" + std::to_string(virtual_key);
}

// Maps an ImGui keyboard key to the Windows virtual-key code stored in the
// config and polled with GetAsyncKeyState on the present thread, or 0 for keys
// that are not rebindable (mouse and gamepad keys have no virtual key here).
// ImGuiKey_0..9, A..Z, F1..F24 and Keypad0..9 are sequential ranges.
inline uint32_t VkFromImGuiKey(ImGuiKey key) {
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9) return uint32_t('0') + (key - ImGuiKey_0);
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return uint32_t('A') + (key - ImGuiKey_A);
  if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) return VK_F1 + (key - ImGuiKey_F1);
  if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9)
    return VK_NUMPAD0 + (key - ImGuiKey_Keypad0);
  switch (key) {
    case ImGuiKey_Tab: return VK_TAB;
    case ImGuiKey_LeftArrow: return VK_LEFT;
    case ImGuiKey_RightArrow: return VK_RIGHT;
    case ImGuiKey_UpArrow: return VK_UP;
    case ImGuiKey_DownArrow: return VK_DOWN;
    case ImGuiKey_PageUp: return VK_PRIOR;
    case ImGuiKey_PageDown: return VK_NEXT;
    case ImGuiKey_Home: return VK_HOME;
    case ImGuiKey_End: return VK_END;
    case ImGuiKey_Insert: return VK_INSERT;
    case ImGuiKey_Delete: return VK_DELETE;
    case ImGuiKey_Backspace: return VK_BACK;
    case ImGuiKey_Space: return VK_SPACE;
    case ImGuiKey_Enter: return VK_RETURN;
    case ImGuiKey_KeypadEnter: return VK_RETURN;
    case ImGuiKey_LeftCtrl: return VK_LCONTROL;
    case ImGuiKey_RightCtrl: return VK_RCONTROL;
    case ImGuiKey_LeftShift: return VK_LSHIFT;
    case ImGuiKey_RightShift: return VK_RSHIFT;
    case ImGuiKey_LeftAlt: return VK_LMENU;
    case ImGuiKey_RightAlt: return VK_RMENU;
    case ImGuiKey_LeftSuper: return VK_LWIN;
    case ImGuiKey_RightSuper: return VK_RWIN;
    case ImGuiKey_Menu: return VK_APPS;
    case ImGuiKey_Apostrophe: return VK_OEM_7;
    case ImGuiKey_Comma: return VK_OEM_COMMA;
    case ImGuiKey_Minus: return VK_OEM_MINUS;
    case ImGuiKey_Period: return VK_OEM_PERIOD;
    case ImGuiKey_Slash: return VK_OEM_2;
    case ImGuiKey_Semicolon: return VK_OEM_1;
    case ImGuiKey_Equal: return VK_OEM_PLUS;
    case ImGuiKey_LeftBracket: return VK_OEM_4;
    case ImGuiKey_Backslash: return VK_OEM_5;
    case ImGuiKey_RightBracket: return VK_OEM_6;
    case ImGuiKey_GraveAccent: return VK_OEM_3;
    case ImGuiKey_CapsLock: return VK_CAPITAL;
    case ImGuiKey_ScrollLock: return VK_SCROLL;
    case ImGuiKey_NumLock: return VK_NUMLOCK;
    case ImGuiKey_PrintScreen: return VK_PRINT;
    case ImGuiKey_Pause: return VK_PAUSE;
    case ImGuiKey_KeypadDecimal: return VK_DECIMAL;
    case ImGuiKey_KeypadDivide: return VK_DIVIDE;
    case ImGuiKey_KeypadMultiply: return VK_MULTIPLY;
    case ImGuiKey_KeypadSubtract: return VK_SUBTRACT;
    case ImGuiKey_KeypadAdd: return VK_ADD;
    default: return 0;
  }
}

inline void RecreateFeatures();

// One press of the NR toggle key, from either reader below.  Same semantics
// as the overlay checkbox: persist, and reset the failure latches when
// switching NR back on.  The line names the key, and whether the overlay's
// reader took it.
inline void ToggleNrFromHotkey(uint32_t toggle_key, bool overlay_open) {
  const bool next = !enabled.load();
  enabled = next;
  reshade::set_config_value(nullptr, kConfigSection, "NeuralUplift", next);
  Log(
      reshade::log::level::info,
      "NR toggled " + std::string(next ? "ON" : "OFF") + " via "
          + VirtualKeyName(toggle_key) + (overlay_open ? " (overlay open)" : ""));
  if (next) RecreateFeatures();
}

// Edge-polled once per present, after the generation bump. Toggles NR with the
// configured key and forwards the screenshot key to the screenshot module.
// These gameplay polls use GetAsyncKeyState, which ReShade's input hook
// answers with "up" while the overlay blocks game input, so while the overlay
// is open OnOverlayHotkeys reads the keys and rebinding runs in OnOverlay.
inline void PollHotkeys() {
  const uint32_t toggle_key = toggle_hotkey.load(std::memory_order_relaxed);
  // GetAsyncKeyState is system-wide: without the focus check a key pressed
  // in another application toggled NR (each ON re-creates the features).
  DWORD foreground_process = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process);
  const bool focused = foreground_process == GetCurrentProcessId();
  // With the overlay open the key reads as up here, which also releases the
  // latch OnOverlayHotkeys shares: one reader per state, so an InputProcessing
  // mode that lets the key through to the game cannot toggle twice per press.
  const bool toggle_down =
      !hud_overlay_open.load(std::memory_order_relaxed) && focused
      && (GetAsyncKeyState(static_cast<int>(toggle_key)) & 0x8000) != 0;
  if (!toggle_down) {
    toggle_hotkey_was_down.store(false, std::memory_order_relaxed);
  } else if (!toggle_hotkey_was_down.exchange(true, std::memory_order_relaxed)) {
    ToggleNrFromHotkey(toggle_key, false);
  }

  screenshot::PollHotkey(screenshot_hotkey.load(std::memory_order_relaxed),
                         focused);
}

// The hotkeys while ReShade's overlay is open, where PollHotkeys cannot see
// them.  They are read from the runtime's own input - what ReShade reads its
// own shortcuts from, never blocked - as a once-per-press edge
// (is_key_pressed skips auto-repeat).  OnPresent registers this only while
// the overlay is open, when ReShade builds an ImGui frame anyway.  Like
// ReShade's shortcuts it ignores keys while an item is active (typing into a
// field), and while a rebind waits for its key.  The edge latches are the
// gameplay polls': PollHotkeys releases them each present while the key reads
// up, so a rebind's own keystroke - primed by DrawSectionHotkeys, which
// ReShade draws earlier in the same frame - does not fire the new binding.
// The screenshot latch is shared with a poll that still runs; a second
// request for one press is ignored while the first capture is armed.
inline void OnOverlayHotkeys(reshade::api::effect_runtime* runtime) {
  if (ImGui::IsAnyItemActive()
      || hotkey_capture_target.load(std::memory_order_relaxed) != 0) {
    return;
  }
  const uint32_t toggle_key = toggle_hotkey.load(std::memory_order_relaxed);
  if (runtime->is_key_pressed(toggle_key)
      && !toggle_hotkey_was_down.exchange(true, std::memory_order_relaxed)) {
    ToggleNrFromHotkey(toggle_key, true);
  }
  if (runtime->is_key_pressed(screenshot_hotkey.load(std::memory_order_relaxed))
      && !screenshot::internal::hotkey_was_down.exchange(
          true, std::memory_order_relaxed)) {
    screenshot::RequestCapture();
  }
}

// ---------------------------------------------------------------------------
// The one verdict (R1)
// ---------------------------------------------------------------------------

// Fills the funnel from this session's counters.  Everything here is a load;
// the decision itself lives in funnel.hpp, where it can be tested against
// sessions that cannot be staged on a dev box.
inline FunnelInputs CollectFunnelInputs() {
  // Item 19: terminals that make an evaluate INELIGIBLE (never ours to
  // inject, or not a usable frame) rather than a decline that needs
  // explaining.  They are subtracted from `eligible` and excluded from the
  // named sum, so an NR-off or frame-generation evaluate neither dilutes the
  // engagement ratio nor competes for the top-decline tag.  Shared by the
  // denominator and both loops below so the sets cannot drift apart.
  auto nr_terminal_is_ineligible = [](std::size_t reason_index) {
    switch (static_cast<NrDeclineReason>(reason_index)) {
      case NrDeclineReason::kNgxNotDlssEvaluation:
      case NrDeclineReason::kNestedMirror:
      case NrDeclineReason::kNrDisabledEvaluation:
      case NrDeclineReason::kGameEvaluateFailed:
      case NrDeclineReason::kMalformedEvaluate:
      // P4 (plan 4.3): a foreign tool's D3D12 evaluate is ineligible under
      // DX11Source=native - not ours to inject, never a decline to explain.
      case NrDeclineReason::kSourceOther:
      // A-1: a first argument that is not a readable object is not a frame.
      case NrDeclineReason::kImplausibleArgument:
      // Standing down beside another NR producer is deliberate, like NR off.
      case NrDeclineReason::kForeignNr:
        return true;
      default:
        return false;
    }
  };

  FunnelInputs in;
  in.hooks_enabled = hooks_enabled.load(std::memory_order_relaxed);
  in.nr_enabled = enabled.load(std::memory_order_relaxed);
  // Unknown is not unavailable: the runtime loads lazily, so before the
  // first attempt the honest answer is "no reason to think otherwise".
  in.runtime_available = direct_load_state != DirectLoadState::Failed;
  // The three rungs below are latched, like every other rung in the ladder:
  // the funnel asks "did this stage happen in this session", and a teardown
  // legitimately unhooks everything for the frames between releasing NR and
  // re-arming.  Measured on the `swapchain_rebuild` lane: reading the live
  // state put a WARN "NOT_ENGAGED stage=ngx_hooked" in the middle of a
  // session that engaged on 235 of 239 evaluates and recovered by itself
  // two frames later.  Whether NR is still running RIGHT NOW is the
  // telemetry line's per-interval nr[evals=] and the overlay's job, not a
  // rung's - a rung that flickers is a warning that stops being read.
  for (int slot = 0; slot < kMaxNgxSlots; ++slot) {
    if (ngx_slot_used[slot]) {
      ngx_ever_hooked.store(true, std::memory_order_relaxed);
      break;
    }
  }
  // Path B: the D3D11 detours are the NGX hooks of a bridged session, and
  // the bridge's own list is the list NR records on.
  in.ngx_hooked = ngx_ever_hooked.load(std::memory_order_relaxed)
                  || ngx11_ever_hooked.load(std::memory_order_relaxed);
  if (cmd_hooks_installed) {
    cmd_hooks_ever_installed.store(true, std::memory_order_relaxed);
  }
  in.list_hooks_live = cmd_hooks_ever_installed.load(std::memory_order_relaxed)
                       || bridge_ever_live.load(std::memory_order_relaxed);
  in.dlss_feature_seen = intercepted_creates.load(std::memory_order_relaxed) > 0
                         || ngx11_dlss_evaluate_seen.load(std::memory_order_relaxed);
  in.d3d11_bridge = ngx11_ever_hooked.load(std::memory_order_relaxed);
  in.nr_feature_ready = active_features.load(std::memory_order_relaxed) > 0
      || nr_feature_ever_ready.load(std::memory_order_relaxed);
  in.gate_ever_opened = gate_ever_opened.load(std::memory_order_relaxed);
  in.d3d11_only = d3d11_present_seen.load(std::memory_order_relaxed)
      && !d3d12_present_seen.load(std::memory_order_relaxed);
  in.dx11_source_off = Dx11SourceOffHere();
  in.foreign_nr = NrYieldsToForeign();
  in.api = D3D12SessionKnown()    ? SessionApi::kD3D12
           : D3D11OnlySession()   ? SessionApi::kD3D11
           : other_api_seen.load(std::memory_order_relaxed)
               ? SessionApi::kOther
               : SessionApi::kUnknown;

  in.seen = intercepted_evaluations.load(std::memory_order_relaxed);
  in.injected = successful_evaluations.load(std::memory_order_relaxed)
      + successful_pre_sr_evaluations.load(std::memory_order_relaxed);
  in.submitted_ok = submitted_injections.load(std::memory_order_relaxed);

  // eligible = seen - (never ours) - (our own re-entry) - (mirrored layers)
  // - (item 19: disabled, game-failed and malformed evaluates).  Saturating:
  // these are read across threads without a snapshot, so a subtraction can
  // transiently exceed `seen`, and a denominator that wrapped to 18
  // quintillion would read as a healthy 0.00 ratio forever.
  std::uint64_t ineligible = own_evaluations.load(std::memory_order_relaxed);
  for (std::size_t i = 0; i < static_cast<std::size_t>(NrDeclineReason::kCount); ++i) {
    if (nr_terminal_is_ineligible(i)) {
      ineligible += nr_decline_counts[i].load(std::memory_order_relaxed);
    }
  }
  in.eligible = in.seen > ineligible ? in.seen - ineligible : 0;

  // The decline that ended the most evaluates, named by its short tag.  Only
  // the ones that mean "we could have injected and did not" compete: the
  // ineligible terminals above are not a reason NR is off.
  std::uint64_t best = 0;
  for (std::size_t i = 0; i < static_cast<std::size_t>(NrDeclineReason::kCount); ++i) {
    if (nr_terminal_is_ineligible(i)) continue;
    const std::uint64_t count = nr_decline_counts[i].load(std::memory_order_relaxed);
    if (count > best) {
      best = count;
      in.top_decline_tag = kNrDeclineTags[i];
      in.top_decline_count = count;
    }
  }

  // When the gate is the story, which aspect it was waiting on.
  in.top_missing_aspect = TopMissingGateAspect();

  // T-SILENT: every eligible evaluate either injected or named a terminal.
  // Whatever is left over is unexplained, and an unexplained remainder makes
  // the ratio a guess rather than a measurement.
  std::uint64_t named = in.injected;
  for (std::size_t i = 0; i < static_cast<std::size_t>(NrDeclineReason::kCount); ++i) {
    if (nr_terminal_is_ineligible(i)) continue;
    named += nr_decline_counts[i].load(std::memory_order_relaxed);
  }
  // An evaluate still running has not reached a terminal yet and must not
  // read as one that never will.  The verdict is emitted from inside an
  // evaluate whenever presents have stalled (NgxLifecycleTick), which made
  // `present_starved` report a funnel leak of exactly one.
  named += evaluations_in_flight.load(std::memory_order_relaxed);
  in.unaccounted = in.eligible > named ? in.eligible - named : 0;
  return in;
}

// The last verdict emitted, so a change can be reported the moment it
// happens rather than at the next interval.  Stage and reason are part of
// "changed", not just state: a session that never engages holds one state
// for its whole life while climbing several rungs, and emitting only on
// state would leave the log holding the verdict from the first present -
// "stage=ngx_hooked seen=0", which is the one moment the addon knows
// nothing. Measured on the T2 `no_seed` lane, where that was the only line.
inline std::atomic<NrState> last_emitted_state{NrState::kNotEngaged};
inline std::atomic<FunnelStage> last_emitted_stage{FunnelStage::kCount};
inline std::atomic<const char*> last_emitted_reason{nullptr};
inline std::atomic_bool emitted_any_verdict = false;

// The grep-able line.  One line, one schema version, every field a reporter
// would otherwise be asked for by hand.  Session totals used to print only
// inside Shutdown(), which a crashing or Alt-F4'd game never reaches - so
// the sessions that most needed explaining were the ones that explained
// nothing.
inline void EmitNrVerdict(const char* trigger) {
  const NrVerdict verdict = ComputeNrVerdict(CollectFunnelInputs());
  char line[512];
  FormatNrVerdictLine(
      line, sizeof(line), verdict,
      submitted_injections.load(std::memory_order_relaxed),
      presents_observed.load(std::memory_order_relaxed), kAddonVersion,
      trigger);
  // NOT_ENGAGED is the state the field needs to see without being asked to
  // raise the log level; OFF and UNAVAILABLE are working as intended.  But
  // a session that has not had its chance yet is not a fault: before the
  // game has created a DLSS feature and evaluated it, NOT_ENGAGED is simply
  // "too early", and warning about it at every launch is how a warning stops
  // being read.
  const bool had_its_chance =
      intercepted_creates.load(std::memory_order_relaxed) > 0
      && verdict.seen > 0;
  Log(
      verdict.state == NrState::kNotEngaged && had_its_chance
          ? reshade::log::level::warning
          : reshade::log::level::info,
      line);
  last_emitted_state.store(verdict.state, std::memory_order_relaxed);
  last_emitted_stage.store(verdict.stage, std::memory_order_relaxed);
  last_emitted_reason.store(verdict.reason, std::memory_order_relaxed);
  emitted_any_verdict.store(true, std::memory_order_relaxed);
}

// Called from the lifecycle tick: emits on a state change, and otherwise on
// the telemetry interval, so a long healthy session says so periodically and
// a session that breaks says so at the moment it breaks.
// When NR was switched on, and whether this enable has had its guaranteed
// verdict yet.  Latched here rather than at every toggle site so no future
// toggle path can forget to arm it.
inline int64_t nr_enabled_since_ns = 0;
inline bool emitted_settled_verdict = false;
// Long enough for a title to create its feature and render, short enough
// that a reporter who alt-tabs out after half a minute already has the line.
inline constexpr int64_t kVerdictSettleNs = 30LL * 1'000'000'000LL;

// Returns the verdict it computed, so the status card reads the same one.
inline NrVerdict MaybeEmitNrVerdict() {
  const NrVerdict verdict = ComputeNrVerdict(CollectFunnelInputs());
  // The reason strings are static literals, so comparing pointers is
  // comparing identity - which is what "the same reason" means here.
  if (!emitted_any_verdict.load(std::memory_order_relaxed)
      || verdict.state != last_emitted_state.load(std::memory_order_relaxed)
      || verdict.stage != last_emitted_stage.load(std::memory_order_relaxed)
      || verdict.reason != last_emitted_reason.load(std::memory_order_relaxed)) {
    EmitNrVerdict("change");
    return verdict;
  }
  // One guaranteed line per enable, 30 s in.  Without it a session that
  // reaches its final state early and then holds it says nothing more, and
  // the line a reporter pastes carries the counts from the first second
  // rather than from a settled session.
  if (!enabled.load(std::memory_order_relaxed)) {
    nr_enabled_since_ns = 0;
    emitted_settled_verdict = false;
    return verdict;
  }
  const int64_t now_ns = SteadyNowNs();
  if (nr_enabled_since_ns == 0) {
    nr_enabled_since_ns = now_ns;
    return verdict;
  }
  if (!emitted_settled_verdict
      && now_ns - nr_enabled_since_ns >= kVerdictSettleNs) {
    emitted_settled_verdict = true;
    EmitNrVerdict("settled");
  }
  return verdict;
}

// ---- the user-readable DLSS hint (alpha40) --------------------------------
// The verdict line is a machine contract and the ladder's rungs name funnel
// stages; neither is a sentence a player can act on.  Two MSFS2024 field
// sessions (2026-09-21, both v6.8.0-alpha32) made the gap concrete: one ran
// NR for four minutes with only DLSSG (frame generation) flowing - every
// evaluate politely skipped as "not DLSS/DLSSD", the panel claiming "NO NR
// FEATURE MATCHED" - and nothing anywhere said the fix is a game-settings
// toggle.  The hint is computed from the same atoms the verdict reads, in
// plain language, logged once per state change and shown by the overlay
// ladder.
//
//   1  NR on, NGX hooked, no DLSS/DLSSD create, nothing else reaching the
//      hooks, and no Streamline bridge explaining the silence: DLSS
//      upscaling is simply off in the game's settings.
//   2  as above, but other NGX features ARE evaluating (in the field, DLSSG
//      frame generation): the more specific, more surprising shape.
// State 1 stays quiet when sl.common is present with nothing intercepted -
// that is the private-Streamline shape and the EnableHooks=1 hint above owns
// it; two warnings teach users to read none.
//
//   3  (v7.0.0, A-4) state 1's conditions, in a process that never presented
//      through D3D12 and either presented through D3D11 or has a DLSS
//      provider (nvngx_dlss.dll / nvngx_dlssd.dll) loaded.  The addon hooks
//      only the NGX core's D3D12 exports, so "no DLSS create" there says
//      nothing about the game's DLSS: God of War ran DLSS Quality through the
//      D3D11 exports while state 1 told the player DLSS was off
//      (log-preservation/god-of-war-dx11-alpha40/).  Gray, not amber: the
//      player did nothing wrong and has nothing to change.  U1's DX11-only
//      card reads this state.
inline std::atomic_uint32_t dlss_user_hint{0};
inline std::atomic_int64_t dlss_hint_armed_ns{0};
inline std::atomic_uint32_t dlss_hint_emitted{0};

// Players get 60 s - long enough for a settings-menu DLSS toggle or a level
// load to land - and lanes shrink it through the environment, because a
// lane that lasts seconds would never reach the player value.
inline int64_t DlssHintGraceNs() {
  static const int64_t grace = [] {
    char buffer[32] = {};
    if (GetEnvironmentVariableA("RENODX_NR_TEST_DLSS_HINT_GRACE_MS", buffer,
                                sizeof(buffer))
            != 0
        && buffer[0] != '\0') {
      return static_cast<int64_t>(std::strtoul(buffer, nullptr, 10))
             * 1'000'000LL;
    }
    return 60LL * 1'000'000'000LL;
  }();
  return grace;
}

inline void UpdateDlssUserHint(int64_t now_ns) {
  // A present-path poll, like the escalation polls this sits beside: cheap
  // atomics at most every 250 ms, and the one GetModuleHandleW only when a
  // state is about to be claimed.  EnableHooks=0 returns before anything
  // runs, so safe mode's zero-loader-call contract is untouched.
  static std::atomic<int64_t> last_poll_ns{0};
  if (!enabled.load(std::memory_order_relaxed)
      || !hooks_enabled.load(std::memory_order_relaxed)
      || (!ngx_any_hooked.load(std::memory_order_relaxed)
          && !ngx11_any_hooked.load(std::memory_order_relaxed))) {
    dlss_user_hint.store(0, std::memory_order_relaxed);
    dlss_hint_armed_ns.store(0, std::memory_order_relaxed);
    dlss_hint_emitted.store(0, std::memory_order_relaxed);
    return;
  }
  const int64_t last = last_poll_ns.load(std::memory_order_relaxed);
  if (last != 0 && now_ns - last < 250'000'000) return;
  last_poll_ns.store(now_ns, std::memory_order_relaxed);

  const uint32_t dlss_bits =
      (static_cast<uint32_t>(kFeatureDlss) < 32
           ? 1u << static_cast<uint32_t>(kFeatureDlss)
           : 0u)
      | (static_cast<uint32_t>(kFeatureDlssd) < 32
             ? 1u << static_cast<uint32_t>(kFeatureDlssd)
             : 0u);
  // Either API's create counts, and so does a bridged DLSS evaluate whose
  // create slipped past the hooks.
  const uint32_t creates = logged_create_ids.load(std::memory_order_relaxed)
                           | logged_create11_ids.load(std::memory_order_relaxed);
  int state = 0;
  if ((creates & dlss_bits) == 0
      && !ngx11_dlss_evaluate_seen.load(std::memory_order_relaxed)) {
    if (logged_skip_ids.load(std::memory_order_relaxed) != 0) {
      state = 2;
    } else if (intercepted_creates.load(std::memory_order_relaxed) != 0
               || intercepted_evaluations.load(std::memory_order_relaxed) != 0
               || GetModuleHandleW(L"sl.common.dll") == nullptr) {
      state = 1;
    }
  }
  // A-4: the D3D12 hooks cannot see this game's DLSS, so they cannot say it
  // is off.  The provider lookup runs only when state 1 is about to be
  // claimed in a process with no D3D12 present, never on a D3D12 game.  With
  // the D3D11 exports detoured (the native route) they can, and state 1 is
  // the truth.
  if (state == 1 && !d3d12_present_seen.load(std::memory_order_relaxed)
      && !ngx11_any_hooked.load(std::memory_order_relaxed)) {
    bool not_d3d12 = d3d11_present_seen.load(std::memory_order_relaxed);
    if (!not_d3d12) {
      NoteLoaderCall("UpdateDlssUserHint/GetModuleHandleW");
      not_d3d12 = GetModuleHandleW(L"nvngx_dlss.dll") != nullptr
                  || GetModuleHandleW(L"nvngx_dlssd.dll") != nullptr;
    }
    if (not_d3d12) state = 3;
  }
  if (state == 0) {
    dlss_user_hint.store(0, std::memory_order_relaxed);
    dlss_hint_armed_ns.store(0, std::memory_order_relaxed);
    dlss_hint_emitted.store(0, std::memory_order_relaxed);
    return;
  }
  const int64_t armed = dlss_hint_armed_ns.load(std::memory_order_relaxed);
  if (armed == 0) {
    dlss_hint_armed_ns.store(now_ns, std::memory_order_relaxed);
    // A zero grace (the lanes) claims the state this instant; any real
    // grace waits for its window from this anchor.
    if (DlssHintGraceNs() > 0) return;
  } else if (now_ns - armed < DlssHintGraceNs()) {
    return;
  }
  const uint32_t state_id = static_cast<uint32_t>(state);
  dlss_user_hint.store(state_id, std::memory_order_relaxed);
  if (dlss_hint_emitted.exchange(state_id, std::memory_order_relaxed)
      == state_id) {
    return;
  }
  if (state == 3) {
    Log(reshade::log::level::info,
        d3d11_present_seen.load(std::memory_order_relaxed)
            ? "Neural Rendering is on and idle: this game presents through"
              " Direct3D 11 and the session is on the foreign route"
              " (DX11Source=foreign, or a Direct3D 11 bridge add-on of"
              " another project is loaded), where Neural Rendering serves only"
              " a third-party tool's Direct3D 12 DLSS. Nothing is wrong; set"
              " DX11Source=native in [RenoDX.DLSS5] to serve the game's own"
              " DLSS instead."
            : "Neural Rendering is on and idle: this game runs a DLSS"
              " provider without presenting through Direct3D 12 or Direct3D"
              " 11, the two APIs Neural Rendering serves. Nothing is wrong;"
              " the addon stays loaded and out of the way.");
    return;
  }
  Log(
      reshade::log::level::warning,
      state == 2
          ? "Neural Rendering is on, but this game is only running DLSS"
            " Frame Generation, which is a different feature. Turn on DLSS"
            " or DLAA upscaling in the game's graphics settings to use"
            " Neural Rendering."
          : "Neural Rendering is on, but DLSS upscaling is not running in"
            " this game. Turn on DLSS or DLAA in the game's graphics"
            " settings; Neural Rendering improves that upscaler's image and"
            " stays idle while it is off.");
}

// ---- the status card (v7.0.0, PLAN_UI_V7.md 3) ----------------------------
// The overlay's answer to "is it working?", decided in ui/state_card.hpp from
// the verdict plus the atoms it does not carry.  The NR-CARD line names the
// card a session reached, so the lanes grade what a player would have seen.
//
// When NR was last switched on (0 = off), for the starting grace.  Written by
// the lifecycle tick, read by the overlay.
inline std::atomic_int64_t nr_on_since_ns{0};

inline std::atomic_bool card_partial_latched{false};

inline ui::CardInputs CollectCardInputs(const NrVerdict& verdict,
                                        int64_t now_ns) {
  ui::CardInputs in;
  in.partial_latched = card_partial_latched.load(std::memory_order_relaxed);
  in.verdict = verdict;
  in.dlss_hint = dlss_user_hint.load(std::memory_order_relaxed);
  in.active_features = active_features.load(std::memory_order_relaxed);
  // Ray Reconstruction keeps the after path under pre-SR (PreSrTakes).
  in.pre_sr = nr_before_upscale.load(std::memory_order_relaxed)
      && !logged_pre_sr_rr.load(std::memory_order_relaxed);
  in.streamline_hooks_on = streamline_hooks_enabled.load(std::memory_order_relaxed);
  in.streamline_shape = static_cast<ui::StreamlineShape>(
      streamline_hint_shape.load(std::memory_order_relaxed));
  in.runtime_fault = static_cast<ui::RuntimeFault>(
      nr_runtime_fault.load(std::memory_order_relaxed));
  // The same grace the hint machine waits out: a card must not call a
  // session idle before the hint would be allowed to say why.
  in.starting = ui::InStartingWindow(
      now_ns, nr_on_since_ns.load(std::memory_order_relaxed), DlssHintGraceNs());
  in.input_width = last_input_width.load(std::memory_order_relaxed);
  in.input_height = last_input_height.load(std::memory_order_relaxed);
  in.output_width = last_output_width.load(std::memory_order_relaxed);
  in.output_height = last_output_height.load(std::memory_order_relaxed);
  return in;
}

inline std::atomic_uint8_t last_emitted_card{0xff};
// A token bucket, not a spacing: a lane's first change (starting -> active)
// must be logged at once, and a session whose ratio hovers at the 0.99 line
// must not write a line every tick.  Eight at once, one more every 5 s.
// Lifecycle tick only (runtime_mutex).
inline double card_line_tokens = 8.0;
inline int64_t card_line_refill_ns = 0;
inline constexpr double kCardLineBurst = 8.0;
inline constexpr int64_t kCardLineRefillNs = 5LL * 1'000'000'000LL;

inline void EmitStatusCard(const ui::StatusCard& card, const char* trigger) {
  char line[128];
  ui::FormatCardLine(line, sizeof(line), card, trigger);
  Log(reshade::log::level::info, line);
  last_emitted_card.store(static_cast<uint8_t>(card.id), std::memory_order_relaxed);
}

// U4, the HUD (PLAN_UI_V7.md 4): a pill in the top-left corner while NR is
// not working (ui::ShowsOnHud), and a 4-second toast when the card turns
// green after a pill or for the first time in a session.  Both hide while
// ReShade's overlay is open, where the card itself is.  The tick below sets
// the two; OnPresent listens to reshade_overlay only while one is due, so a
// working session keeps ReShade's no-ImGui-frame early-out: zero cost by
// construction when green or gray.
inline std::atomic_bool hud_pill{false};
inline std::atomic_int64_t hud_toast_until_ns{0};
inline constexpr int64_t kHudToastNs = 4'000'000'000LL;

inline void MaybeEmitStatusCard(const NrVerdict& verdict, int64_t now_ns) {
  const ui::StatusCard card =
      ui::ComputeStatusCard(CollectCardInputs(verdict, now_ns));
  card_partial_latched.store(card.id == ui::CardId::kPartlyActive, std::memory_order_relaxed);
  // Every tick, ahead of the line's dedupe and its token bucket.  A gray
  // flap (waiting_for_dlss across a resolution change) and amber (the pill
  // already said it) raise no toast.
  if (card.tone != ui::CardTone::kGood) {
    hud_pill.store(ui::ShowsOnHud(card), std::memory_order_relaxed);
  } else if (hud_pill.exchange(false, std::memory_order_relaxed)
             || hud_toast_until_ns.load(std::memory_order_relaxed) == 0) {
    hud_toast_until_ns.store(now_ns + kHudToastNs, std::memory_order_relaxed);
  }
  if (card_line_refill_ns != 0) {
    card_line_tokens = std::min(
        kCardLineBurst,
        card_line_tokens
            + static_cast<double>(now_ns - card_line_refill_ns)
                  / static_cast<double>(kCardLineRefillNs));
  }
  card_line_refill_ns = now_ns;
  if (static_cast<uint8_t>(card.id)
      == last_emitted_card.load(std::memory_order_relaxed)) {
    return;
  }
  if (card_line_tokens < 1.0) return;
  card_line_tokens -= 1.0;
  EmitStatusCard(card, "change");
}

// ReShade does not tell an add-on about an open it asked for itself; the
// capture lane sets hud_overlay_open by hand for that reason.
inline bool OnReshadeOpenOverlay(reshade::api::effect_runtime* /*runtime*/,
                                 bool open,
                                 reshade::api::input_source /*source*/) {
  hud_overlay_open.store(open, std::memory_order_relaxed);
  return false;
}

// ReShade docks an add-on's overlay window beside its own tabs only while it
// builds its dock layout, which it does once, when no saved layout exists
// (runtime_gui.cpp, init_window_layout).  A user whose ReShade layout was
// saved before this addon was installed gets the window floating, and at a
// few pixels: the panel is a BeginChild(0, 0) that fills its parent, so
// ImGui's first auto-fit measures nothing.  It then sits over whichever tab
// is open (field report: over another RenoDX add-on's "Tone Mapping").
// The fix runs from ReShade's reshade_overlay pass, after every overlay
// window has ended: re-opening ReShade's Home window ("###home", its ID in
// every language) reads the dock node it lives in, and SetNextWindowDockID
// on a re-open of this window docks it there next frame (ImGui applies a
// dock request on any Begin).  With Home not docked the window gets a
// usable size instead.  The next frame selects the new tab: the user was
// looking at this window, and a docked tab that is not selected is never
// drawn (Begin returns false).  Only in the frame OnOverlay saw the window
// stranded: this pass also runs on the frame the overlay closes, when
// re-opening Home would flash it.  Once per session, so a window the user
// floats and sizes on purpose stays where they put it.
inline void OnDockOverlayWindow(reshade::api::effect_runtime* /*runtime*/) {
  const uint32_t step = overlay_dock_step.load(std::memory_order_relaxed);
  if (step == 1) {
    ImGui::SetWindowFocus(kOverlayTitle);
    overlay_dock_step.store(2, std::memory_order_relaxed);
    overlay_window_stranded.store(-1, std::memory_order_relaxed);
    return;
  }
  if (step != 0
      || overlay_window_stranded.load(std::memory_order_relaxed) != ImGui::GetFrameCount()) {
    return;
  }
  ImGui::Begin("###home", nullptr, ImGuiWindowFlags_NoFocusOnAppearing);
  const ImGuiID home_dock = ImGui::GetWindowDockID();
  ImGui::End();
  if (home_dock != 0) {
    ImGui::SetNextWindowDockID(home_dock, ImGuiCond_Always);
  } else {
    const float font = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(
        ImVec2(font * (ui::tokens::kColumnEm + 2.f), font * 40.f), ImGuiCond_Always);
  }
  ImGui::Begin(kOverlayTitle, nullptr, ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::End();
  overlay_dock_step.store(home_dock != 0 ? 1 : 2, std::memory_order_relaxed);
  overlay_window_stranded.store(-1, std::memory_order_relaxed);
  Log(reshade::log::level::info,
      home_dock != 0 ? "overlay window was floating at a few pixels; docked beside ReShade's Home tab"
                     : "overlay window was floating at a few pixels and Home is not docked; resized");
}

// UiLanguage (v7.0.0): 0 is Auto, ReShade's own UI language; otherwise one
// plus the index into ui::kLanguages.  Stored as the language's code, or
// "auto".
inline std::atomic_int ui_language_choice{0};

// Sets UiLanguage and writes it: the Language row, its reset and
// PersistConfig.
inline void StoreUiLanguage(int choice) {
  ui_language_choice = choice;
  reshade::set_config_value(nullptr, kConfigSection, "UiLanguage",
                            choice == 0 ? "auto" : ui::kLanguages[choice - 1].code);
}

// Selects the panel's language for this overlay frame.  Auto reads the
// thread's preferred UI languages: ReShade sets its own language there while
// add-ons draw (Settings > Language, or Windows' language when it has none),
// and loads the font for that language only, so following it keeps every
// glyph drawable.  One line in ReShade.log per change.
inline void RefreshUiLanguage() {
  const int choice = ui_language_choice.load(std::memory_order_relaxed);
  std::string tag;
  if (choice == 0) {
    ULONG count = 0;
    ULONG size = 0;
    if (GetThreadPreferredUILanguages(MUI_LANGUAGE_NAME | MUI_UI_FALLBACK, &count,
                                      nullptr, &size)
        && size != 0) {
      std::wstring tags(size, L'\0');
      if (GetThreadPreferredUILanguages(MUI_LANGUAGE_NAME | MUI_UI_FALLBACK, &count,
                                        tags.data(), &size)
          && count != 0) {
        for (const wchar_t c : tags) {
          if (c == L'\0') break;
          tag += static_cast<char>(c < 0x80 ? c : '?');
        }
      }
    }
  }
  // ENV-14: ReShade loads the font for its OWN language (micross for the
  // Cyrillic and Thai languages, the CJK fonts only for theirs), and a
  // Font= override replaces it, so a language picked here - or followed by
  // Auto under a custom font - can lack glyphs, and the panel drew '?' where
  // text should be.  The language's tip for the Language row is probed
  // against the font the panel draws with; a missing glyph keeps the panel
  // in English (index 0), latched for this language and font so the probe
  // and its line run once.
  static int unfontable_index = -1;
  static ImFont* probed_font = nullptr;
  ImFont* font = ImGui::GetFont();
  int index = choice == 0 ? ui::MatchLanguage(tag) : choice - 1;
  if (index == unfontable_index && font == probed_font) index = 0;
  if (ui::SelectLanguage(index)) {
    if (index != 0 && font != nullptr) {
      const char* probe = ui::Tr(ui::text::kLanguageTip);
      const int length = MultiByteToWideChar(CP_UTF8, 0, probe, -1, nullptr, 0);
      std::wstring wide(static_cast<size_t>(std::max(length, 0)), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, probe, -1, wide.data(), length);
      for (const wchar_t c : wide) {
        if (c < 0x80 || (c >= 0xD800 && c <= 0xDFFF)) continue;
        if (font->IsGlyphInFont(static_cast<ImWchar>(c))) continue;
        unfontable_index = index;
        probed_font = font;
        ui::SelectLanguage(0);
        char text[320] = {};
        snprintf(text, sizeof(text),
                 "UI language %s%s: ReShade's font has no glyph for U+%04X, so"
                 " the panel stays in English; set ReShade's own language to"
                 " the same one (ReShade's Settings tab), or pick English",
                 ui::kLanguages[index].code, choice == 0 ? " (Auto)" : " (UiLanguage)",
                 static_cast<unsigned>(c));
        Log(reshade::log::level::warning, text);
        return;
      }
    }
    Log(reshade::log::level::info,
        std::string("UI language: ") + ui::kLanguages[ui::CurrentLanguage()].code
            + (choice == 0 ? " (Auto, preferred " + (tag.empty() ? "none" : tag) + ")"
                           : std::string(" (UiLanguage)"))
            + (ui::DroppedTranslations() != 0
                   ? ", " + std::to_string(ui::DroppedTranslations())
                         + " entries dropped for mismatched printf conversions"
                   : std::string()));
  }
}

// The card is recomputed here rather than carried from the tick, as the
// panel does: atomics only, and the pill never shows a card the tick has
// since left.
inline void OnHudOverlay(reshade::api::effect_runtime* /*runtime*/) {
  RefreshUiLanguage();
  const int64_t now_ns = SteadyNowNs();
  const ui::StatusCard card = ui::ComputeStatusCard(
      CollectCardInputs(ComputeNrVerdict(CollectFunnelInputs()), now_ns));
  const bool pill = ui::ShowsOnHud(card);
  const int64_t toast_ns =
      hud_toast_until_ns.load(std::memory_order_relaxed) - now_ns;
  if (!pill && (toast_ns <= 0 || card.tone != ui::CardTone::kGood)) return;
  // One line per kind and session: the proof the HUD was entered, which no
  // counter of the listener's registration can give.
  static std::atomic_bool logged_pill{false};
  static std::atomic_bool logged_toast{false};
  if (!(pill ? logged_pill : logged_toast).exchange(true)) {
    Log(reshade::log::level::info,
        std::string("status HUD drawn: ") + (pill ? "pill" : "toast")
            + " card=" + ui::CardSlug(card.id));
  }
  ui::StyleScope hud;
  hud.Var(ImGuiStyleVar_Alpha,
          pill ? 1.f : (std::min)(1.f, static_cast<float>(toast_ns) / 1e9f))
      .Var(ImGuiStyleVar_WindowRounding, ui::tokens::kRadiusCard)
      .Var(ImGuiStyleVar_WindowPadding,
           ImVec2(ui::tokens::kCardPadding, ui::tokens::kRowGap))
      .Color(ImGuiCol_WindowBg, ui::tokens::kBgCard)
      .Color(ImGuiCol_Border, ui::tokens::kStroke)
      .Color(ImGuiCol_Text, ui::tokens::kTextPrimary);
  ImGui::SetNextWindowPos(ImVec2(ui::tokens::kRowGap, ui::tokens::kRowGap),
                          ImGuiCond_Always);
  if (ImGui::Begin("##nr_hud", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
                       | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings
                       | ImGuiWindowFlags_NoDocking
                       | ImGuiWindowFlags_NoFocusOnAppearing
                       | ImGuiWindowFlags_AlwaysAutoResize)) {
    ui::CardHeading(card);
    if (pill) {
      ui::StyleScope hint;
      hint.FontSize(ui::SmallFontSize())
          .Color(ImGuiCol_Text, ui::tokens::kTextSecondary);
      ImGui::Text(ui::Tr(ui::text::kHudHint), kOverlayTitle,
                  VirtualKeyName(toggle_hotkey.load()).c_str());
    }
  }
  ImGui::End();
}

// `force` emits regardless of how much of the window has elapsed: the
// session's last telemetry line, from Shutdown().  Everything the verdict
// does not carry - the tracker, the shadow, the hook entry tallies - lives
// on this line only, and on an interval the last one in the log is up to
// NRTelemetrySeconds stale.  Measured 2026-09-21: every T2 lane ended with
// `ngx[... release=0]` although SynthHost releases its DLSS feature at
// teardown, because that call lands after the last interval - a counter
// reading zero for a call that demonstrably happened.  This is the same
// defect the shutdown VERDICT fixed one field-set earlier.
//
// A window that never started is not forced: with no presents there are no
// rates to state, and the verdict already says presents=0.
//
// The latest line is kept for the support report (U3), behind its own lock:
// the report is built on the overlay thread and must not take runtime_mutex.
inline std::mutex last_telemetry_mutex;
inline std::string last_telemetry_line;

inline void EmitTelemetry(int64_t now_ns, bool force = false) {
  const uint32_t interval_s = telemetry_seconds.load(std::memory_order_relaxed);
  if (interval_s == 0) return;
  const uint64_t evals = successful_evaluations.load(std::memory_order_relaxed)
      + successful_pre_sr_evaluations.load(std::memory_order_relaxed);
  if (telemetry_window_start_ns == 0) {
    telemetry_session_start_ns = now_ns;
    telemetry_window_start_ns = now_ns;
    telemetry_window_start_evals = evals;
    return;
  }
  const int64_t window_ns = now_ns - telemetry_window_start_ns;
  if (!force
      && window_ns < static_cast<int64_t>(interval_s) * 1'000'000'000) {
    return;
  }
  const double window_s = window_ns / 1e9;

  // NGX export entry tallies (see ngx_entry_* for why entry and
  // interception are different claims).  `copies` is the module copies
  // currently detoured, `entered` how many of ALL slots ever saw a call:
  // copies>entered is a detoured copy the game never routes through, which
  // is a finding, not noise.
  unsigned ngx_copies = 0;
  unsigned ngx_entered = 0;
  // Exports no detoured copy actually had.  Without this, `evalc=0` reads
  // as "the detour is installed and dead" when the truth is that nothing
  // was installed: vtable::Hook SKIPS an export the module does not
  // provide, and its own comment says most nvngx copies do not implement
  // the _C evaluate variant.  Measured 2026-09-21 - that is exactly the
  // wrong conclusion this tally led to the first time it was read.
  bool ngx_have[4] = {false, false, false, false};
  uint64_t ngx_create_entries = 0;
  uint64_t ngx_eval_entries = 0;
  uint64_t ngx_evalc_entries = 0;
  uint64_t ngx_release_entries = 0;
  for (int slot = 0; slot < kMaxNgxSlots; ++slot) {
    if (ngx_slot_used[slot]) ++ngx_copies;
    const uint64_t c = ngx_entry_create[slot].load(std::memory_order_relaxed);
    const uint64_t e = ngx_entry_evaluate[slot].load(std::memory_order_relaxed);
    const uint64_t ec =
        ngx_entry_evaluate_c[slot].load(std::memory_order_relaxed);
    const uint64_t r = ngx_entry_release[slot].load(std::memory_order_relaxed);
    if ((c | e | ec | r) != 0) ++ngx_entered;
    if (ngx_slot_used[slot]) {
      const NgxModuleReal& real = ngx_slot_real[slot];
      if (real.create != nullptr) ngx_have[0] = true;
      if (real.evaluate != nullptr) ngx_have[1] = true;
      if (real.evaluate_c != nullptr) ngx_have[2] = true;
      if (real.release != nullptr) ngx_have[3] = true;
    }
    ngx_create_entries += c;
    ngx_eval_entries += e;
    ngx_evalc_entries += ec;
    ngx_release_entries += r;
  }
  std::string ngx_absent;
  if (ngx_copies != 0) {
    static constexpr const char* kNgxExportTags[4] = {
        "create", "eval", "evalc", "release"};
    for (int i = 0; i < 4; ++i) {
      if (ngx_have[i]) continue;
      if (!ngx_absent.empty()) ngx_absent += ',';
      ngx_absent += kNgxExportTags[i];
    }
  }
  uint64_t live_bytes = 0;
  uint32_t orphans = 0;
  std::ostringstream worksets_text;
  for (const auto& [key, res] : worksets) {
    live_bytes += res.allocated_bytes;
    const bool orphan = features.count(key.handle) == 0;
    orphans += orphan ? 1u : 0u;
    worksets_text << (worksets_text.tellp() > 0 ? "," : "") << "ws" << res.id
                  << ':' << res.width << 'x' << res.height << ":s"
                  << res.stack_passes << ":idle"
                  << (present_generation - res.last_used_generation)
                  << ":snaps" << res.snaps << (orphan ? ":orphan" : "");
  }
  uint64_t retired_bytes = 0;
  uint64_t oldest_retired = 0;
  // Retention census (P4, observe-only): how much of the retired set has NO
  // completion proof at all - the DrainRetiredResources empty-lease case,
  // which retains until teardown.  The v6.7.1 unproven-retention admission
  // budget stays REJECTED as a decline (PLAN_REHAB_V7.md 8 do-not-port):
  // this is the census it returns as, a measurement the B15 bound
  // ("retained bytes bounded over 1200 frames") is read from.  Additive
  // members only; nothing here declines.
  uint64_t unproven_bytes = 0;
  uint32_t unproven_ws_count = 0;
  uint32_t unproven_nr_count = 0;
  uint64_t oldest_unproven = 0;
  for (const auto& retired : retired_final_resources) {
    retired_bytes += retired.resources.allocated_bytes;
    oldest_retired =
        std::max(oldest_retired, present_generation - retired.generation);
    if (!retired.tracker_proof && retired.lease.empty()) {
      unproven_bytes += retired.resources.allocated_bytes;
      ++unproven_ws_count;
      oldest_unproven =
          std::max(oldest_unproven, present_generation - retired.generation);
    }
  }
  for (const auto& retired : retired_nr_features) {
    oldest_retired =
        std::max(oldest_retired, present_generation - retired.generation);
    if (!retired.tracker_proof && retired.lease.empty()) {
      ++unproven_nr_count;
      oldest_unproven =
          std::max(oldest_unproven, present_generation - retired.generation);
    }
  }
  uint32_t queues = 0;
  uint32_t idle_queues = 0;
  uint32_t faulted_queues = 0;
  {
    std::lock_guard<std::mutex> lock(queue_completion_mutex);
    for (const auto& [queue, tracker] : queue_completions) {
      ++queues;
      if (tracker.faulted) ++faulted_queues;
      if (now_ns - tracker.last_submit_ns.load(std::memory_order_relaxed)
          > 2'000'000'000) {
        ++idle_queues;
      }
    }
  }
  const char* probe = "none";
  if (telemetry_probe_taken) {
    const GpuLeaseState state = QueryGpuLease(telemetry_probe_lease);
    probe = telemetry_probe_lease.empty()        ? "unproven"
            : state == GpuLeaseState::kCompleted ? "done"
            : state == GpuLeaseState::kPending   ? "STARVED"
                                                 : "device-removed";
  }
  telemetry_probe_lease = AcquireGpuLease();
  telemetry_probe_taken = true;
  const submission::TrackerStats tracker = submission::Snapshot();
  const ListShadowStats shadow = SnapshotListShadowStats();
  // P4 A1: the foreign-source observation group, built only for sessions
  // that looked foreign (D3D11 presents, no D3D12 present ever) or that saw
  // foreign NGX entries - the empty string keeps D3D12 telemetry
  // byte-identical.  Observe-only: nothing here declines; the
  // double-processing ride a foreign session serves is the intended Path A
  // behavior, and any gate on it is P5 B7 (observe-only first).
  std::string foreign_text;
  if ((d3d11_present_seen.load(std::memory_order_relaxed)
       && !d3d12_present_seen.load(std::memory_order_relaxed))
      || foreign_ngx_entries.load(std::memory_order_relaxed) != 0) {
    std::ostringstream foreign;
    foreign << " foreign[src=1 entries="
            << foreign_ngx_entries.load(std::memory_order_relaxed)
            << " qinst="
            << (foreign_queue_install_logged.load(std::memory_order_relaxed)
                    ? 1
                    : 0)
            << " qinst_fail="
            << foreign_queue_install_failures.load(std::memory_order_relaxed)
            << ']';
    foreign_text = foreign.str();
  }
  // Every reason that has fired, cumulative: the session totals only print
  // at Shutdown, which a killed or crashed game never reaches, and a decline
  // that recurs is otherwise invisible after its one-shot log line.
  std::ostringstream declines_text;
  for (size_t i = 0; i < static_cast<size_t>(NrDeclineReason::kCount); ++i) {
    const uint64_t total = nr_decline_counts[i].load(std::memory_order_relaxed);
    if (total == 0) continue;
    if (declines_text.tellp() > 0) declines_text << ' ';
    declines_text << kNrDeclineTags[i] << '=' << total;
  }
  const telemetry::VideoMemory vram = telemetry::QueryVideoMemory(direct_device);
  uint32_t live_slots = 0;
  for (const auto& [_, state] : features) {
    for (const auto& slot : state.slots) {
      if (slot.handle != nullptr) ++live_slots;
    }
  }

  std::ostringstream line;
  line.setf(std::ios::fixed);
  line.precision(2);
  line << "telemetry t=" << (now_ns - telemetry_session_start_ns) / 1e9
       << "s win=" << window_s << "s presents=" << telemetry_pacing.count
       << " ticks=" << telemetry_window_ticks << " fps="
       << telemetry_pacing.count / window_s << " frame_ms[avg="
       << (telemetry_pacing.count != 0
               ? telemetry_pacing.sum_ms / telemetry_pacing.count
               : 0.0)
       << " p50=" << telemetry_pacing.Percentile(0.50)
       << " p99=" << telemetry_pacing.Percentile(0.99)
       << " max=" << telemetry_pacing.max_ms << "] nr[evals="
       << (evals - telemetry_window_start_evals) << " cpu_us_avg="
       << (telemetry_window_nr_calls != 0
               ? telemetry_window_nr_ns / 1e3 / telemetry_window_nr_calls
               : 0.0)
       << " cpu_us_max=" << telemetry_window_nr_max_ns / 1e3 << "]";
  if (vram.valid) {
    line << " vram_mib[local=" << (vram.local_usage >> 20) << '/'
         << (vram.local_budget >> 20)
         << " nonlocal=" << (vram.nonlocal_usage >> 20) << ']';
  }
  line << " own_mib[live=" << (live_bytes >> 20)
       << " retired=" << (retired_bytes >> 20)
       << " unproven=" << (unproven_bytes >> 20) << "] worksets[live="
       << worksets.size() << " orphan=" << orphans << " evicted="
       << worksets_evicted << ' '
       << worksets_text.str() << "] retired[ws="
       << retired_final_resources.size()
       << " nr=" << retired_nr_features.size()
       << " oldest_ticks=" << oldest_retired
       << " unproven_ws=" << unproven_ws_count
       << " unproven_nr=" << unproven_nr_count
       << " oldest_unproven_ticks=" << oldest_unproven << "] lifetime[ws +"
       << worksets_created << " -" << worksets_retired << " ~"
       << worksets_released << " nr +" << nr_features_created << " -"
       << nr_features_retired << " ~" << nr_features_released
       << "] features=" << features.size() << " slots=" << live_slots
       << " queues=" << queues << "[idle2s=" << idle_queues
       << " faulted=" << faulted_queues << "] lease_probe=" << probe
       << " tracker[lists=" << tracker.lists << " gens=" << tracker.generations
       << " open=" << tracker.open_generations << " uses=" << tracker.live_uses
       << " unprovable=" << tracker.unprovable_submissions
       << " fallbacks=" << tracker.identity_fallbacks << " inert="
       << queue_detour_inert_submits.load(std::memory_order_relaxed)
       << "] shadow[mode="
       << list_state_mode.load(std::memory_order_relaxed)
       // Command-list detours ENTERED this session, out of the ones
       // installed.  The slot numbers are proven against the SDK headers
       // by test/dlss5/vtable_slots.c and the installer proves Detours
       // accepted them; neither proves the game ever routed a call
       // through one.  A title that records on a list implementation the
       // addon never detoured shows the same empty shadow as a title that
       // records nothing - until this number separates them.
       << " cmd_hooks=" << CmdHookEntryReport()
       << " attached=" << shadow.attached
       << " attach_failed=" << shadow.attach_failures << " lists="
       << shadow.entries << " stripe_max=" << shadow.longest_stripe
       << " parks=" << shadow.parks << " waits=" << shadow.waits
       << " evicted=" << shadow.idle_evictions
       << " bundles=" << shadow.bundles
       << " bundles_merged=" << shadow.bundles_merged
       << " bundles_opaque=" << shadow.bundles_opaque
       // GPU-driven titles: `indirect` is the volume, `applied` the calls
       // whose signature named a compute root argument, `inert` the ones
       // that could not change compute state at all, and `unknown_sig` the
       // ones created before the device hook - the number that says whether
       // the exact aftermath is reaching this title.  `sigs` counts the
       // signatures described at creation.
       << " indirect=" << shadow.indirects
       << " indirect_applied=" << shadow.indirects_applied
       << " indirect_inert=" << shadow.indirects_inert
       << " indirect_unknown_sig=" << shadow.indirects_unknown_sig
       << " sigs=" << command_signatures_described.load(std::memory_order_relaxed)
       // Table arguments the documented heap-change aftermath took away.
       // Climbing while the image looks wrong after an evaluate is the
       // signature of a host that changes heaps and then relies on its
       // tables surviving - which D3D12 says they do not.
       << " heap_table_drops=" << shadow.heap_table_drops
       << " heap_sets_illegal=" << shadow.heap_sets_illegal
       << "] ngx[copies=" << ngx_copies << " entered=" << ngx_entered
       << " create=" << ngx_create_entries
       << " eval=" << ngx_eval_entries
       << " evalc=" << ngx_evalc_entries
       << " release=" << ngx_release_entries
       << (ngx_absent.empty() ? std::string() : (" absent=" + ngx_absent))
       << "] entries[create_list="
       << device_create_list_entries.load(std::memory_order_relaxed)
       << " submit=" << queue_submit_entries.load(std::memory_order_relaxed)
       << ']' << foreign_text << ArgGateTelemetry() << BridgeTelemetry()
       << " seen="
       << intercepted_evaluations.load(std::memory_order_relaxed)
       << " bypassed=" << bypassed_evaluations.load(std::memory_order_relaxed)
       // Should be 0 in every session.  Anything else is the addon's own
       // bookkeeping throwing inside a call the game made; the game's call
       // still ran, but a non-zero here is the first thing to triage.
       << " hook_exceptions="
       << hook_exceptions.load(std::memory_order_relaxed)
       << " detour_contended="
       << renodx::utils::vtable::transaction_contended.load(
              std::memory_order_relaxed)
       << " declines[" << declines_text.str() << "] epochs{game:"
       << history_reset_counts[static_cast<size_t>(HistoryResetSource::kGameReset)]
       << ",setting:"
       << history_reset_counts[static_cast<size_t>(HistoryResetSource::kSetting)]
       << ",recreate:"
       << history_reset_counts[static_cast<size_t>(HistoryResetSource::kRecreate)]
       << "} modules[presents="
       << module_scan_presents.load(std::memory_order_relaxed)
       << " scans=" << module_scans.load(std::memory_order_relaxed) << ']'
       // Device teardowns seen this session, of which D3D12, of which
       // ours.  A game that rebuilds its device mid-session (alt-tab,
       // fullscreen toggle, resolution change) is the case where a
       // missed teardown leaks NR state into the next generation.
       << " devices[destroys="
       << destroy_device_events.load(std::memory_order_relaxed)
       << " d3d12=" << destroy_device_d3d12_events.load(std::memory_order_relaxed)
       << " matched=" << destroy_device_matches.load(std::memory_order_relaxed)
       << " swapchains=" << destroy_swapchain_events.load(std::memory_order_relaxed)
       << " swapchains_ours="
       << destroy_swapchain_matches.load(std::memory_order_relaxed)
       << ']'
       // Lock order and contention (runtime_lock.hpp, which owns the field
       // names).  `loader_under` is the invariant: calls into the Windows
       // loader made while this addon held runtime_mutex, which is the
       // deadlock-with-DllMain shape.  It is zero, and a T2 lane says so.
       << ' ' << LockOrderReport()
       << " snaps=" << norm_snap_count.load(std::memory_order_relaxed)
       << "{prime:" << norm_snap_prime.load(std::memory_order_relaxed)
       << ",epoch:" << norm_snap_epoch.load(std::memory_order_relaxed)
       << ",reset:" << norm_snap_reset.load(std::memory_order_relaxed) << '}';
  if (norm_trace_enabled.load(std::memory_order_relaxed)) {
    line << " trace[published=" << norm_trace::Published()
         << " dropped=" << norm_trace::Dropped()
         << " unexecuted=" << norm_trace::Unexecuted()
         << " lease_starved=" << norm_trace::LeaseStarved() << ']';
  }
  // The look stage, once it has done anything (look_stage.hpp).
  const uint64_t look_shaped = look_shaped_passes.load(std::memory_order_relaxed);
  const uint64_t look_traced = look_traced_passes.load(std::memory_order_relaxed);
  if (look_shaped != 0 || look_traced != 0) {
    line << " look[shaped=" << look_shaped
         << " transport=" << look_transport_passes.load(std::memory_order_relaxed)
         << " restarts=" << look_history_restarts.load(std::memory_order_relaxed)
         << " ring_full=" << look_motion_ring_full.load(std::memory_order_relaxed)
         << " traced=" << look_traced << " published=" << edit_trace::Published()
         << " dropped=" << edit_trace::Dropped()
         << " unexecuted=" << edit_trace::Unexecuted()
         << " lease_starved=" << edit_trace::LeaseStarved() << ']';
  }
  // NRGpuTimers' own delivery: a sample dropped or never executed is
  // otherwise silent (the timers printed nothing from v6.1.1 to alpha45).
  if (gpu_timers_enabled.load(std::memory_order_relaxed)) {
    line << " timers[published=" << gpu_timers::published_segments
         << " dropped=" << gpu_timers::dropped_segments
         << " unexecuted=" << gpu_timers::unexecuted_segments
         << " lease_starved=" << gpu_timers::lease_starved_segments << ']';
  }
  // Feed v2 (NRFeedMode), once it has run a frame: frames on the exposure-
  // texture feed, frames fed at a fixed 1 (pre-exposed), exposure view ring
  // entries recycled for a new texture, and texture frames that held the
  // last scale because no ring entry was provably idle.
  const uint64_t feed_texture = feed_texture_frames.load(std::memory_order_relaxed);
  const uint64_t feed_fixed = feed_fixed_frames.load(std::memory_order_relaxed);
  if (feed_texture != 0 || feed_fixed != 0) {
    line << " feed[texture=" << feed_texture << " fixed=" << feed_fixed
         << " recycled="
         << feed_exposure_ring_recycled.load(std::memory_order_relaxed)
         << " ring_full=" << feed_exposure_ring_full.load(std::memory_order_relaxed)
         << ']';
  }
  if (const auto count = streamline_internal_callbacks.load(std::memory_order_relaxed);
      count != 0) {
    line << " sl_internal_callbacks=" << count;
  }
  // What the game sent this window (GameWindow).
  if (game_window.frames != 0) {
    line << " game[pe=" << game_window.pe_last << '(' << game_window.pe_min << ".."
         << game_window.pe_max << ") es=" << game_window.es_last << '('
         << game_window.es_min << ".." << game_window.es_max
         << ") resets=" << game_window.resets << " bursts=" << game_window.bursts
         << " alternations=" << game_window.alternations << ']';
  }
  Log(reshade::log::level::info, line.str());
  // The settings that decide what NR does, on their own line whenever they
  // differ from the last one logged (section 7): a support log then says
  // what ran without the reader replaying every slider change.
  {
    std::ostringstream settings;
    settings << "NR effective settings: enabled=" << enabled.load()
             << " intensity=" << intensity.load() << " passes=" << stack_passes.load()
             << " chained_history=" << chained_temporal_history.load() << " path="
             << (!nr_before_upscale.load() ? "after"
                 : logged_pre_sr_rr.load() ? "pre-SR (after for RR)"
                                           : "pre-SR")
             << " resolution_scale=" << nr_resolution_scale.load()
             << " foveation=" << nr_foveation.load() << ':' << nr_foveation_width.load()
             << 'x' << nr_foveation_height.load() << '/' << nr_foveation_falloff.load()
             << '/' << nr_foveation_edge.load()
             << " follow_input=" << nr_follow_input_res.load()
             << " look=" << look_mode.load() << " stabilize=" << look_stabilize.load()
             << " upsample=" << look_upsample.load() << " codec=" << codec_mode.load()
             << " feed=" << feed_mode.load() << " governor=" << norm_governor.load();
    static std::string logged_settings;
    if (settings.str() != logged_settings) {
      logged_settings = settings.str();
      Log(reshade::log::level::info, logged_settings);
    }
  }
  {
    std::lock_guard<std::mutex> lock(last_telemetry_mutex);
    last_telemetry_line = line.str();
  }
  // One verdict beside every telemetry line, so a log opened at any point in
  // a session answers "is NR running" without the reader deriving it from
  // fifteen counters.
  EmitNrVerdict("interval");

  telemetry_pacing = {};
  game_window = {};
  telemetry_window_start_ns = now_ns;
  telemetry_window_ticks = 0;
  telemetry_window_start_evals = evals;
  telemetry_window_nr_calls = 0;
  telemetry_window_nr_ns = 0;
  telemetry_window_nr_max_ns = 0;
}

// Seconds a command list's shadow entry may go unobserved before it is
// dropped (see SweepIdleListShadows).  Recording lists are touched every
// frame; the bound only has to outlast a pooled list's idle spell cheaply,
// since a dropped entry costs one declined injection at most.
constexpr uint32_t kListShadowIdleSeconds = 30;

// Advances the shadow's idle clock and sweeps once per second.  Outside
// runtime_mutex: the sweep releases COM pins.
inline void SweepListShadowsOncePerSecond() {
  static const int64_t start_ns = SteadyNowNs();
  static std::atomic_uint32_t last_sweep_second{0};
  const uint32_t second =
      static_cast<uint32_t>((SteadyNowNs() - start_ns) / 1000000000ll) + 1u;
  if (last_sweep_second.exchange(second, std::memory_order_relaxed) == second) {
    return;
  }
  list_shadow_tick.store(second, std::memory_order_relaxed);
  SweepIdleListShadows(kListShadowIdleSeconds);
}

// Per-frame lifecycle shared by the present path and the NGX starvation
// fallback: advance the generation clock, drop the per-frame dedup state,
// drain retired resources, poll the hotkeys (NR toggle + screenshot), settle
// meters, and drain any settled capture (atomic fast path when idle; no QI,
// no fence, no file IO).  Must not be called with runtime_mutex held.
// `present_ns` is the present's timestamp, 0 for an NGX-driven tick.
inline void RunLifecycleTick(int64_t present_ns) {
  uint64_t generation = 0;
  {
    RuntimeLock lock(runtime_mutex);
    generation = ++present_generation;
    processed_outputs.clear();
    streamline_viewports.clear();
    streamline_tag_mask_this_present.store(0, std::memory_order_relaxed);
    streamline_tag_max_batch.store(0, std::memory_order_relaxed);
    SettleRetiredQueueFences();
    RetireSupersededWorksets(SteadyNowNs());
    DrainRetiredResources();
    gpu_timers::Poll(queue_tracking_active.load());
    norm_trace::Poll(queue_tracking_active.load());
    edit_trace::Poll(queue_tracking_active.load());
    if (present_ns != 0) {
      if (telemetry_previous_present_ns != 0) {
        telemetry_pacing.Add((present_ns - telemetry_previous_present_ns) / 1e6);
      }
      telemetry_previous_present_ns = present_ns;
    }
    ++telemetry_window_ticks;
    EmitTelemetry(present_ns != 0 ? present_ns : SteadyNowNs());
    // The verdict is emitted the moment engagement changes, not only on the
    // telemetry interval: the session that needs explaining is usually the
    // one that stopped working part way through, and waiting out an interval
    // (or Shutdown, which a crash never reaches) is how that went unrecorded
    // for six releases.  A present-rate check of ~20 relaxed loads.
    const NrVerdict verdict = MaybeEmitNrVerdict();
    const int64_t tick_ns = SteadyNowNs();
    if (!enabled.load(std::memory_order_relaxed)) {
      nr_on_since_ns.store(0, std::memory_order_relaxed);
    } else if (nr_on_since_ns.load(std::memory_order_relaxed) == 0) {
      nr_on_since_ns.store(tick_ns, std::memory_order_relaxed);
    }
    MaybeEmitStatusCard(verdict, tick_ns);
  }
  SweepListShadowsOncePerSecond();
  PollHotkeys();
  screenshot::OnPresent(generation);
}

// Present-starvation fallback: when present events stop while NGX keeps
// evaluating once per frame, evaluate entry becomes the real frame cadence,
// so the lifecycle re-keys onto it.  No-op while presents flow (gated on a
// 250 ms present gap); install attempts stay present-only because they need
// the present's queue, and a session that starves mid-run already installed
// them while presents were flowing.  Call only from an OUTERMOST game
// evaluate (not InsideDirectCall(), not a nested C-wrapper re-entry): our
// internal NR evaluates run under runtime_mutex, which this would re-lock.
inline void NgxLifecycleTick() {
  if (shutting_down.load(std::memory_order_acquire)) return;
  const int64_t now_ns = SteadyNowNs();
  const int64_t last_present =
      last_present_steady_ns.load(std::memory_order_relaxed);
  if (last_present != 0 && now_ns - last_present < kPresentStarvationNs) {
    return;
  }
  int64_t expected = last_synthetic_tick_ns.load(std::memory_order_relaxed);
  if (now_ns - expected < kSyntheticTickMinIntervalNs
      || !last_synthetic_tick_ns.compare_exchange_strong(
          expected, now_ns, std::memory_order_relaxed)) {
    return;
  }
  if (!synthetic_tick_active.exchange(true, std::memory_order_relaxed)) {
    debug::Mark("lifecycle:ngx-tick-engaged");
    Log(
        reshade::log::level::warning,
        "present events starved while NGX evaluates continue; driving the"
        " per-frame lifecycle (hotkeys, captures, retirement) from"
        " NGX evaluate ticks");
  }
  RunLifecycleTick(0);
}

inline void OnPresent(
    reshade::api::command_queue* queue,
    reshade::api::swapchain*,
    const reshade::api::rect*,
    const reshade::api::rect*,
    uint32_t,
    const reshade::api::rect*) {
  if (shutting_down.load(std::memory_order_acquire)) return;
  // Counted before ANY early return below it: the question this answers is
  // "did the addon get ticked", not "did it get far".
  presents_observed.fetch_add(1, std::memory_order_relaxed);
  // The foreign-session discriminator (P4 A1): the presenting queue's device
  // API.  Two relaxed loads and two stores on the present thread; the sticky
  // flags are what keep every foreign-path install out of D3D12 sessions.
  if (queue != nullptr) {
    reshade::api::device* const present_device = queue->get_device();
    if (present_device != nullptr) {
      if (present_device->get_api() == reshade::api::device_api::d3d11) {
        d3d11_present_seen.store(true, std::memory_order_relaxed);
      } else if (present_device->get_api()
                 == reshade::api::device_api::d3d12) {
        d3d12_present_seen.store(true, std::memory_order_relaxed);
      } else {
        other_api_seen.store(true, std::memory_order_relaxed);
      }
    }
  }
  // A pending swapchain teardown (OnDestroySwapchain) is serviced on
  // whichever swapchain still presents: the one that replaced it, or another
  // device's.  After a release the rest of this present re-arms as usual.
  if (teardown_pending.load(std::memory_order_acquire)) {
    GuardHook([] { ServicePendingTeardown(); });
  }
  debug::TouchPresent();
  const int64_t present_ns = SteadyNowNs();
  last_present_steady_ns.store(present_ns, std::memory_order_relaxed);
  if (synthetic_tick_active.exchange(false, std::memory_order_relaxed)) {
    debug::Mark("lifecycle:present-resumed");
    Log(
        reshade::log::level::info,
        "present events resumed; NGX-driven lifecycle ticks retired");
  }
  RunLifecycleTick(present_ns);
  // Listen for ReShade's overlay pass only while a listener has work: U4's
  // HUD while a pill or toast is due, the hotkey reader while the overlay is
  // open.  Here and nowhere else: ReShade walks its listener list unlocked,
  // and this is the present thread, just before that walk.
  {
    const auto listen = [](std::atomic_bool* listening, bool due,
                           void (*callback)(reshade::api::effect_runtime*)) {
      if (listening->exchange(due, std::memory_order_relaxed) == due) return;
      if (due) {
        reshade::register_event<reshade::addon_event::reshade_overlay>(callback);
      } else {
        reshade::unregister_event<reshade::addon_event::reshade_overlay>(callback);
      }
    };
    static std::atomic_bool hud_listening{false};
    static std::atomic_bool hotkeys_listening{false};
    const bool overlay_open = hud_overlay_open.load(std::memory_order_relaxed);
    listen(&hud_listening,
           !overlay_open
               && (hud_pill.load(std::memory_order_relaxed)
                   || present_ns < hud_toast_until_ns.load(std::memory_order_relaxed)),
           OnHudOverlay);
    listen(&hotkeys_listening, overlay_open, OnOverlayHotkeys);
    // A dock whose tab-select frame never came (the overlay closed first)
    // is done: the next open must not pull this tab to the front.
    if (!overlay_open) {
      uint32_t selecting = 1;
      overlay_dock_step.compare_exchange_strong(selecting, 2, std::memory_order_relaxed);
    }
    static std::atomic_bool dock_listening{false};
    listen(&dock_listening,
           overlay_open
               && (overlay_window_stranded.load(std::memory_order_relaxed) >= 0
                   || overlay_dock_step.load(std::memory_order_relaxed) == 1),
           OnDockOverlayWindow);
  }
  InstallHooks();
  InstallStreamlineHooks();
  // The present path is already the addon's designated place for loader work
  // (see the Streamline escalation poll below), so the symbol prime belongs
  // here too: a game that loads its NGX core long after the addon - DLSS
  // switched on from the graphics menu, the `late_ngx` profile - has it
  // resolved before the first evaluate rather than during one.
  //
  // Gated on hooks_enabled, because EnableHooks=0 is documented as "fully
  // inert (safe mode: no NGX/Streamline hooks, no NR pass)" and a user in
  // safe mode is there to find out whether this addon is what broke their
  // game.  Before this prime existed the runtime could only be loaded from
  // the evaluate path (which safe mode has no hooks for) or from
  // OnInitDevice's eager load (already gated the same way), so an ungated
  // prime here would make safe mode map nvngx_dlssnr.dll for the first time
  // - the opposite of what it promises.
  //
  // Row 23 (item 21 reopened at alpha36's review): and gated on the process
  // needing the D3D12 runtime at all.  hooks_enabled defaults ON, so the
  // hook-only gate above still mapped and hashed the ~165 MB runtime at the
  // first present of every DX11-only game - through a path that prints
  // neither pre-load line, which is exactly how alpha36's fix passed its
  // lane (log lines, not the mechanism) while the behaviour continued.  The
  // honest gate: a D3D12 device has been seen (every D3D12 game's
  // init_device sets it long before any present), or a D3D12 NGX entry has
  // been seen (a session that was already evaluating when the hooks
  // arrived).  A DX11-only process has none of those and keeps none of the
  // cost.  The d3d11 smoke lane holds the mechanism witness: the HOST
  // reports its own module census, and the census must be 0.
  //
  // The D3D11 bridge (alpha45) is not a reason either: its own evaluates
  // bump the same entry counters, but it primes for itself when NR has a
  // frame to run (Ngx11Evaluate, EnsureBridgeUp), so an NR-off session on
  // the native route maps nothing - the e2e11_native control's census.
  if (hooks_enabled.load(std::memory_order_acquire)
      && (d3d12_device_seen.load(std::memory_order_acquire)
          || (!Dx11NativeRoute()
              && (intercepted_creates.load(std::memory_order_relaxed) != 0
                  || intercepted_evaluations.load(std::memory_order_relaxed)
                         != 0)))) {
    PrimeNgxLoaderSymbols();
  }
  // Streamline auto-escalation (see streamline_escalation_deficits): polled
  // on a slow cadence so the present path stays cheap; GetModuleHandleW
  // needs the loader lock, which is why it runs here, outside runtime_mutex.
  static std::atomic<uint32_t> escalation_poll{0};
  if (!streamline_hooks_enabled.load(std::memory_order_relaxed)
      && !streamline_auto_escalated.load(std::memory_order_relaxed)
      && streamline_escalation_allowed.load(std::memory_order_relaxed)
      && streamline_escalation_deficits.load(std::memory_order_relaxed) >= 60u
      && escalation_poll.fetch_add(1, std::memory_order_relaxed) % 30u == 29u
      && GetModuleHandleW(L"sl.common.dll") != nullptr) {
    streamline_auto_escalated.store(true, std::memory_order_relaxed);
    streamline_hooks_enabled.store(true, std::memory_order_release);
    Log(
        reshade::log::level::warning,
        "auto-enabling the Streamline hook layer for this session: 60+ NGX"
        " evaluates could not take NR through the NGX exports while"
        " sl.common.dll is loaded (private Streamline bridge); set"
        " EnableHooks=2 in [RenoDX.DLSS5] to force NGX-only if this"
        " misbehaves");
    InstallStreamlineHooks();
  }
  // D3D12 machinery on a D3D12 device only (item 18).  ReShade fires this
  // event for every graphics API it supports, and on a D3D11 present
  // `queue->get_native()` is an ID3D11DeviceContext*: queue slot 10 there is
  // DrawIndexed and device slot 12 is CreateUnorderedAccessView, so an
  // ungated install detours the wrong methods with mismatched-signature
  // wrappers on the first present.  Field: GTA5 legacy (D3D11),
  // v6.8.0-alpha32, 2026-09-21 - both "installed" lines at presents=1 and
  // the log ends 15 ms later mid-boot; the queue half of that shipped as far
  // back as v6.5.3, the device half arrived with alpha31's retry here.
  // OnInitDevice has gated its half since alpha7 with a comment naming this
  // exact hazard; this path had no gate at all, and no lane had ever
  // presented a non-D3D12 frame to expose it (dlss5_e2e_d3d11_smoke now
  // does, and its red run is what this change turns green).
  if (hooks_enabled.load(std::memory_order_relaxed) && queue != nullptr) {
    reshade::api::device* const api_device = queue->get_device();
    if (api_device != nullptr
        && api_device->get_api() == reshade::api::device_api::d3d12) {
      InstallQueueCompletionHooks(
          reinterpret_cast<ID3D12CommandQueue*>(queue->get_native()));
      // The device hooks go on at init_device; this is the retry for the
      // session where that one attempt failed.  Without it a single lost
      // install costs the birth-time baseline for the whole device
      // generation - and the hosts that need it most are the ones that
      // decline outright without it.
      if (!device_hooks_installed) {
        InstallDeviceStateHooks(
            reinterpret_cast<ID3D12Device*>(api_device->get_native()));
      }
    }
  }
  // One-shot compatibility hint (v5): NGX hooks see creates and evaluates but
  // NR never engaged while an sl.common module is loaded.  In a private
  // Streamline bridge the guide buffers arrive as SL tags, which NGX-only mode
  // cannot read.  Polled on a slow cadence so the present path stays cheap.
  //
  // TWO shapes, and through v6.7 only one of them could ever say this.
  //
  //   evaluates intercepted, none engaged - the title creates DLSS through
  //     NGX and supplies the guides as SL tags.  This is what the gate used
  //     to require, and it still is the stronger evidence, so it keeps its
  //     own wording.
  //   NOTHING intercepted - the title drives DLSS entirely through
  //     Streamline, so the NGX exports this addon detoured are never
  //     entered.  `intercepted_creates > 0 && intercepted_evaluations > 0`
  //     made the hint UNREACHABLE in exactly that case: measured on two
  //     v6.7.3-recovery1 field sessions (Forza Horizon 6, 405 s and 922
  //     presents per window with `nvngx_dlss.dll` hooked and creates=0; GTA V
  //     Enhanced, whose process also ran slInit with DLSS in featuresToLoad).
  //     The session that most needs "set EnableHooks=1" was the one the addon
  //     could not say it in.
  //
  // Both still require the module to be present, the hooks to be installed
  // and the slow poll, so a title that simply has no DLSS stays quiet: the
  // sl.common check is the whole discriminator, and it is the same one the
  // original gate trusted.
  static std::atomic<uint32_t> hint_poll{0};
  // Item 20: NR must be ON before this hint can fire.  With NR off, "not
  // engaged" is the design working, and the advice told users to edit
  // ReShade.ini and restart for titles (MSFS2024 field log, 2026-09-21)
  // whose NGX creates arrived minutes into the session and engaged unaided
  // the moment NR was switched on - 12+ warnings, every one while
  // state=OFF.  The FH6/G4 shapes the hint exists for all had NR on.
  if (enabled.load(std::memory_order_relaxed)
      && hooks_enabled.load(std::memory_order_relaxed)
      && !streamline_hooks_enabled.load(std::memory_order_relaxed)
      && ngx_any_hooked.load(std::memory_order_relaxed)
      && successful_evaluations.load() == 0
      && successful_pre_sr_evaluations.load() == 0
      && [&] {
           // First at ~2 s of play, then every 1800.  Thirty seconds of
           // "the mod does nothing" is how a user closes the log; and a
           // 240-frame lane could never reach the old cadence at all, so
           // the hint had no test either.
           const uint32_t poll =
               hint_poll.fetch_add(1, std::memory_order_relaxed);
           return poll == 119u || poll % 1800u == 1799u;
         }()
      && GetModuleHandleW(L"sl.common.dll") != nullptr
      && [&] {
           // "Nothing reached the hooks" is a claim from absence, and a
           // Streamline title that calls NGX directly for DLSS (KCD2: DLSS
           // through the NGX hooks, sl.common for frame generation) is
           // silent too while it sits in its menu.  The v8.0.1 KCD2 log told
           // the owner at 2 s to edit ReShade.ini; the create reached the
           // hooks 37 s in.  That shape waits out the DLSS-off hint's grace
           // (60 s, lanes 0) from its first poll.  Evaluates that reach the
           // hooks without engaging are evidence, and keep the 2 s cadence.
           static std::atomic<int64_t> silent_since_ns{0};
           int64_t since = 0;
           silent_since_ns.compare_exchange_strong(since, present_ns,
                                                   std::memory_order_relaxed);
           return (intercepted_creates.load() > 0
                   && intercepted_evaluations.load() > 0)
                  || present_ns - (since != 0 ? since : present_ns)
                         >= DlssHintGraceNs();
         }()) {
    const uint64_t creates = intercepted_creates.load();
    const uint64_t evaluates = intercepted_evaluations.load();
    streamline_hint_shape.store(
        static_cast<uint8_t>(creates > 0 && evaluates > 0
                                 ? ui::StreamlineShape::kGuidesViaTags
                                 : ui::StreamlineShape::kNothingIntercepted),
        std::memory_order_relaxed);
    if (creates > 0 && evaluates > 0) {
      Log(
          reshade::log::level::warning,
          "NGX evaluates are intercepted but NR has not engaged while"
          " sl.common.dll is loaded; this title may supply its guides through"
          " Streamline tags. To try the Streamline fallback, set EnableHooks=1"
          " in the [RenoDX.DLSS5] section of ReShade.ini and restart the game");
    } else {
      Log(
          reshade::log::level::warning,
          "sl.common.dll is loaded and the NGX hooks are installed, but not one"
          " DLSS create or evaluate has reached them (creates="
              + std::to_string(creates) + " evaluates="
              + std::to_string(evaluates)
              + "); this title drives DLSS through Streamline, so the NGX"
                " exports this addon detoured are never entered. Set"
                " EnableHooks=1 in the [RenoDX.DLSS5] section of ReShade.ini"
                " and restart the game");
    }
  }
  // The plain-language DLSS hint (alpha40): same poll discipline as the
  // blocks above, one log line per state change, and it deliberately
  // disagrees with nothing here - when sl.common explains the silence, the
  // EnableHooks=1 hint already fired and this one stays quiet.
  UpdateDlssUserHint(present_ns);
}

inline void RecreateFeatures() {
  RuntimeLock lock(runtime_mutex);
  ++configuration_generation;
  RequestHistoryReset(HistoryResetSource::kRecreate);
  processed_outputs.clear();
  streamline_viewports.clear();
  if (direct_api.module == nullptr) direct_load_state = DirectLoadState::Unknown;
  direct_runtime_failed = false;
  for (auto& [_, feature] : features) {
    ReleaseAllNrSlots(feature, true);
    for (auto& slot : feature.slots) {
      slot.failed = false;
      slot.fail_count = 0;
    }
  }
}

// The panel layout (UiMode, U2): modern by default, classic behind the
// Developer view toggle.  A setting, not a mode of the injection path, so a
// plain flag; stored as "modern" or "classic".
inline std::atomic_bool ui_classic_layout{false};
// UiWelcomed (U5): the first-run welcome card is dismissed.  Written by its
// "Got it" button only - not a Setting, so Restore all never brings the
// welcome back.
inline std::atomic_bool ui_welcomed{false};

// ---- settings (alpha46) ------------------------------------------------------
// One user setting as the panel sees it: its config key, its value, its
// built-in default and what a change must restart.  The controls, the
// per-row reset buttons, the section resets, the Defaults view, Restore all
// and PersistConfig all go through it, so a key is paired with its value and
// its default once.  Every Setting sits in exactly one of the lists in
// kSettingSections, which is what Restore all and PersistConfig walk; gate
// step 2g (tools/field/check_ui_reachability.py) holds that, and that every
// key LoadConfiguration reads has a control in both layouts.
enum class SettingEffect : uint8_t { kNone, kHistory, kRecreate };

inline void ApplySettingEffect(SettingEffect effect) {
  if (effect == SettingEffect::kRecreate) {
    RecreateFeatures();
  } else if (effect == SettingEffect::kHistory) {
    RequestHistoryReset();
  }
}

struct Setting {
  constexpr Setting(const char* name, std::atomic<float>* value, float stock,
                    SettingEffect change)
      : key(name), f(value), fallback(stock), effect(change) {}
  constexpr Setting(const char* name, std::atomic_uint32_t* value, uint32_t stock,
                    SettingEffect change)
      : key(name), u(value), fallback(static_cast<float>(stock)), effect(change) {}
  constexpr Setting(const char* name, std::atomic_bool* value, bool stock,
                    SettingEffect change)
      : key(name), b(value), fallback(stock ? 1.f : 0.f), effect(change) {}

  float Get() const {
    if (f != nullptr) return f->load();
    if (u != nullptr) return static_cast<float>(u->load());
    return b->load() ? 1.f : 0.f;
  }
  bool Modified() const {
    return std::abs(Get() - fallback) > 1e-4f * (std::max)(1.f, std::abs(fallback));
  }
  // The value alone: no config write, no restart (the Defaults view).
  void Store(float value) const {
    if (f != nullptr) {
      f->store(value);
    } else if (u != nullptr) {
      u->store(static_cast<uint32_t>(std::lround(value)));
    } else {
      b->store(value != 0.f);
    }
  }
  // Always to the global config file, as PersistConfig explains.
  void Persist() const {
    if (f != nullptr) {
      reshade::set_config_value(nullptr, kConfigSection, key, f->load());
    } else if (u != nullptr) {
      reshade::set_config_value(nullptr, kConfigSection, key, u->load());
    } else {
      reshade::set_config_value(nullptr, kConfigSection, key, b->load());
    }
  }
  // A player's change: the value, its config write and its restart.
  void Commit(float value) const {
    Store(value);
    Persist();
    ApplySettingEffect(effect);
  }

  const char* key;
  std::atomic<float>* f = nullptr;
  std::atomic_uint32_t* u = nullptr;
  std::atomic_bool* b = nullptr;
  float fallback;
  SettingEffect effect;
};

inline constexpr Setting kSetEnabled{
    "NeuralUplift", &enabled, defaults::kEnabled, SettingEffect::kNone};
inline constexpr Setting kSetIntensity{
    "NRIntensity", &intensity, defaults::kIntensity, SettingEffect::kHistory};

inline constexpr Setting kSetPreset{
    "NRPreset", &preset, defaults::kPreset, SettingEffect::kRecreate};
inline constexpr Setting kSetStyle{
    "NRStyle", &style, defaults::kStyle, SettingEffect::kHistory};
inline constexpr Setting kSetLocalTone{
    "NRLocalTone", &local_tone_strength, defaults::kLocalTone, SettingEffect::kHistory};
inline constexpr Setting kSetLocalStructure{
    "NRLocalStructure", &local_structure_strength, defaults::kLocalStructure,
    SettingEffect::kHistory};
inline constexpr Setting kSetSkinStructure{
    "NRSkinStructure", &skin_structure_strength, defaults::kSkinStructure,
    SettingEffect::kHistory};
inline constexpr Setting kSetSkinIndependent{
    "NRSkinIndependent", &skin_independent, defaults::kSkinIndependent,
    SettingEffect::kHistory};
inline constexpr Setting kSetAutoMask{
    "NRAutoMask", &use_auto_mask, defaults::kAutoMask, SettingEffect::kHistory};
inline constexpr Setting kSetUiCorrection{
    "NRUICorrection", &ui_correction, defaults::kUiCorrection, SettingEffect::kHistory};

// The two insertion points use differently-dimensioned resource sets and NR
// features, so a flip retires all temporal state.  The working resolution
// settles through the present path's contract debounce; the panel's
// discrete choices skip it (CommitResolutionMode).
inline constexpr Setting kSetPreSr{
    "NRPreUpscale", &nr_before_upscale, defaults::kPreSr, SettingEffect::kRecreate};
inline constexpr Setting kSetFollowInputRes{
    "NRFollowInputRes", &nr_follow_input_res, defaults::kFollowInputRes,
    SettingEffect::kNone};
inline constexpr Setting kSetResolutionScale{
    "NRResolutionScale", &nr_resolution_scale, defaults::kResolutionScale,
    SettingEffect::kNone};
// Foveated NR (after upscale).  The working width follows from the warp and
// settles through the same contract debounce as the resolution scale; a new
// warp resets the temporal histories by itself (the after path compares
// FeatureState::foveation), so no effect is needed here.
inline constexpr Setting kSetFoveation{
    "NRFoveation", &nr_foveation, defaults::kFoveation, SettingEffect::kNone};
inline constexpr Setting kSetFoveationWidth{
    "NRFoveationWidth", &nr_foveation_width, defaults::kFoveationWidth,
    SettingEffect::kNone};
inline constexpr Setting kSetFoveationHeight{
    "NRFoveationHeight", &nr_foveation_height, defaults::kFoveationHeight,
    SettingEffect::kNone};
inline constexpr Setting kSetFoveationFalloff{
    "NRFoveationFalloff", &nr_foveation_falloff, defaults::kFoveationFalloff,
    SettingEffect::kNone};
inline constexpr Setting kSetFoveationEdge{
    "NRFoveationEdge", &nr_foveation_edge, defaults::kFoveationEdge, SettingEffect::kNone};

// Lowering the pass count must free the retired passes' NR feature handles;
// raising it starts the new passes from clean temporal state.  The per-pass
// strengths are passes 2..kMaxNrPasses: pass 1 IS the global controls.
inline constexpr Setting kSetStackPasses{
    "NRPasses", &stack_passes, defaults::kStackPasses, SettingEffect::kRecreate};
inline constexpr Setting kSetPassIntensity[] = {
    {"NRPass2Intensity", &pass_intensity[0], defaults::kPassStrength,
     SettingEffect::kHistory},
    {"NRPass3Intensity", &pass_intensity[1], defaults::kPassStrength,
     SettingEffect::kHistory},
    {"NRPass4Intensity", &pass_intensity[2], defaults::kPassStrength,
     SettingEffect::kHistory}};
// A pass's Transfer and Color act in its resolve, after NR, like pass 1's
// NRTransferStrength / NRColorStrength: no history restart.
inline constexpr Setting kSetPassTransfer[] = {
    {"NRPass2Transfer", &pass_transfer_strength[0], defaults::kPassStrength,
     SettingEffect::kNone},
    {"NRPass3Transfer", &pass_transfer_strength[1], defaults::kPassStrength,
     SettingEffect::kNone},
    {"NRPass4Transfer", &pass_transfer_strength[2], defaults::kPassStrength,
     SettingEffect::kNone}};
inline constexpr Setting kSetPassColor[] = {
    {"NRPass2Color", &pass_color_strength[0], defaults::kPassStrength,
     SettingEffect::kNone},
    {"NRPass3Color", &pass_color_strength[1], defaults::kPassStrength,
     SettingEffect::kNone},
    {"NRPass4Color", &pass_color_strength[2], defaults::kPassStrength,
     SettingEffect::kNone}};
static_assert(std::size(kSetPassIntensity) == kMaxNrPasses - 1
              && std::size(kSetPassTransfer) == kMaxNrPasses - 1
              && std::size(kSetPassColor) == kMaxNrPasses - 1);
// Per-pass model steering (group G): each steers the model, so a change
// resets the temporal history like its pass-1 counterpart does.
inline constexpr Setting kSetPassFollow[] = {
    {"NRPass2FollowPass1", &pass_follow[0], defaults::kPassFollow,
     SettingEffect::kHistory},
    {"NRPass3FollowPass1", &pass_follow[1], defaults::kPassFollow,
     SettingEffect::kHistory},
    {"NRPass4FollowPass1", &pass_follow[2], defaults::kPassFollow,
     SettingEffect::kHistory}};
inline constexpr Setting kSetPassStyle[] = {
    {"NRPass2Style", &pass_style[0], defaults::kStyle,
     SettingEffect::kHistory},
    {"NRPass3Style", &pass_style[1], defaults::kStyle,
     SettingEffect::kHistory},
    {"NRPass4Style", &pass_style[2], defaults::kStyle,
     SettingEffect::kHistory}};
inline constexpr Setting kSetPassLocalTone[] = {
    {"NRPass2LocalTone", &pass_local_tone[0], defaults::kLocalTone,
     SettingEffect::kHistory},
    {"NRPass3LocalTone", &pass_local_tone[1], defaults::kLocalTone,
     SettingEffect::kHistory},
    {"NRPass4LocalTone", &pass_local_tone[2], defaults::kLocalTone,
     SettingEffect::kHistory}};
inline constexpr Setting kSetPassStructure[] = {
    {"NRPass2Structure", &pass_local_structure[0], defaults::kLocalStructure,
     SettingEffect::kHistory},
    {"NRPass3Structure", &pass_local_structure[1], defaults::kLocalStructure,
     SettingEffect::kHistory},
    {"NRPass4Structure", &pass_local_structure[2], defaults::kLocalStructure,
     SettingEffect::kHistory}};
inline constexpr Setting kSetPassSkin[] = {
    {"NRPass2Skin", &pass_skin_structure[0], defaults::kSkinStructure,
     SettingEffect::kHistory},
    {"NRPass3Skin", &pass_skin_structure[1], defaults::kSkinStructure,
     SettingEffect::kHistory},
    {"NRPass4Skin", &pass_skin_structure[2], defaults::kSkinStructure,
     SettingEffect::kHistory}};
inline constexpr Setting kSetPassAutoMask[] = {
    {"NRPass2AutoMask", &pass_auto_mask[0], defaults::kAutoMask,
     SettingEffect::kHistory},
    {"NRPass3AutoMask", &pass_auto_mask[1], defaults::kAutoMask,
     SettingEffect::kHistory},
    {"NRPass4AutoMask", &pass_auto_mask[2], defaults::kAutoMask,
     SettingEffect::kHistory}};
inline constexpr Setting kSetPassUiCorrection[] = {
    {"NRPass2UICorrection", &pass_ui_correction[0], defaults::kUiCorrection,
     SettingEffect::kHistory},
    {"NRPass3UICorrection", &pass_ui_correction[1], defaults::kUiCorrection,
     SettingEffect::kHistory},
    {"NRPass4UICorrection", &pass_ui_correction[2], defaults::kUiCorrection,
     SettingEffect::kHistory}};

// The model-steering settings of one stack pass, in panel order: pass 1's
// are the Look section's own (DrawModelSteering draws either).
struct ModelSteering {
  const Setting& style;
  const Setting& local_tone;
  const Setting& structure;
  const Setting& skin;
  const Setting& auto_mask;
  const Setting& ui_correction;
};
inline constexpr ModelSteering kModelSteering[kMaxNrPasses] = {
    {kSetStyle, kSetLocalTone, kSetLocalStructure, kSetSkinStructure,
     kSetAutoMask, kSetUiCorrection},
    {kSetPassStyle[0], kSetPassLocalTone[0], kSetPassStructure[0],
     kSetPassSkin[0], kSetPassAutoMask[0], kSetPassUiCorrection[0]},
    {kSetPassStyle[1], kSetPassLocalTone[1], kSetPassStructure[1],
     kSetPassSkin[1], kSetPassAutoMask[1], kSetPassUiCorrection[1]},
    {kSetPassStyle[2], kSetPassLocalTone[2], kSetPassStructure[2],
     kSetPassSkin[2], kSetPassAutoMask[2], kSetPassUiCorrection[2]}};

inline constexpr Setting kSetToggleKey{
    "NRToggleKey", &toggle_hotkey, defaults::kToggleHotkey, SettingEffect::kNone};
inline constexpr Setting kSetScreenshotKey{
    "NRScreenshotKey", &screenshot_hotkey, defaults::kScreenshotHotkey,
    SettingEffect::kNone};
inline constexpr Setting kSetScreenshotFormat{
    "NRScreenshotFormat", &screenshot::file_format, 0u, SettingEffect::kNone};
inline constexpr Setting kSetScreenshotLayout{
    "NRScreenshotLayout", &screenshot::layout, 0u, SettingEffect::kNone};
inline constexpr Setting kSetScreenshotJpegQuality{
    "NRScreenshotJpegQuality", &screenshot::jpeg_quality, 95u, SettingEffect::kNone};
inline constexpr Setting kSetScreenshotMaxMB{
    "NRScreenshotMaxMB", &screenshot::max_megabytes, 10u, SettingEffect::kNone};
inline constexpr Setting kSetGpuTimers{
    "NRGpuTimers", &gpu_timers_enabled, defaults::kGpuTimers, SettingEffect::kNone};
inline constexpr Setting kSetEditTrace{
    "NREditTrace", &edit_trace_enabled, defaults::kEditTrace, SettingEffect::kNone};

// The look stage's Result group (PLAN_NR_LOOK_V71.md 3): it reshapes what the
// model returned, so no change restarts the model's history.  The chain reads
// them every frame; the switch, Stabilize and Upsampling also decide what the
// workset allocates, which the next evaluate settles on its own.
inline constexpr Setting kSetLookMode{
    "NRLookMode", &look_mode, defaults::kLookMode, SettingEffect::kNone};
inline constexpr Setting kSetLookStrength{
    "NRLookStrength", &look_strength, defaults::kLook.strength, SettingEffect::kNone};
inline constexpr Setting kSetLookBrighten{
    "NRLookBrighten", &look_brighten, defaults::kLook.brighten, SettingEffect::kNone};
inline constexpr Setting kSetLookDarken{
    "NRLookDarken", &look_darken, defaults::kLook.darken, SettingEffect::kNone};
inline constexpr Setting kSetLookMaxBrighten{
    "NRLookMaxBrighten", &look_max_brighten, defaults::kLook.max_brighten,
    SettingEffect::kNone};
inline constexpr Setting kSetLookMaxDarken{
    "NRLookMaxDarken", &look_max_darken, defaults::kLook.max_darken,
    SettingEffect::kNone};
inline constexpr Setting kSetLookColour{
    "NRLookColour", &look_colour, defaults::kLook.colour, SettingEffect::kNone};
inline constexpr Setting kSetLookHue{
    "NRLookHue", &look_hue, defaults::kLook.hue, SettingEffect::kNone};
inline constexpr Setting kSetLookMaxColour{
    "NRLookMaxColour", &look_max_colour, defaults::kLook.max_colour,
    SettingEffect::kNone};
inline constexpr Setting kSetLookShadows{
    "NRLookShadows", &look_shadows, defaults::kLook.shadows, SettingEffect::kNone};
inline constexpr Setting kSetLookMidtones{
    "NRLookMidtones", &look_midtones, defaults::kLook.midtones, SettingEffect::kNone};
inline constexpr Setting kSetLookHighlights{
    "NRLookHighlights", &look_highlights, defaults::kLook.highlights,
    SettingEffect::kNone};
inline constexpr Setting kSetLookTone{
    "NRLookTone", &look_tone, defaults::kLook.tone, SettingEffect::kNone};
inline constexpr Setting kSetLookDetail{
    "NRLookDetail", &look_detail, defaults::kLook.detail, SettingEffect::kNone};
inline constexpr Setting kSetLookDetailRadius{
    "NRLookDetailRadius", &look_detail_radius, defaults::kLook.detail_radius,
    SettingEffect::kNone};
inline constexpr Setting kSetLookHalo{
    "NRLookHalo", &look_halo, defaults::kLook.halo, SettingEffect::kNone};
inline constexpr Setting kSetLookStabilize{
    "NRLookStabilize", &look_stabilize, defaults::kLook.stabilize, SettingEffect::kNone};
inline constexpr Setting kSetLookStabilizeMs{
    "NRLookStabilizeMs", &look_stabilize_ms, defaults::kLook.stabilize_ms,
    SettingEffect::kNone};
inline constexpr Setting kSetLookStabilizeDetail{
    "NRLookStabilizeDetail", &look_stabilize_detail, defaults::kLook.stabilize_detail,
    SettingEffect::kNone};
inline constexpr Setting kSetLookUpsample{
    "NRLookUpsample", &look_upsample, defaults::kLook.upsample, SettingEffect::kNone};

// The HDR colour bridge: an Encoding or Linear Unit change recreates the
// worksets, because the linear working surfaces and descriptor views are
// built from the unit contract; knobs that change what NR is fed (the codec,
// PQ calibration, the paper-white scale, the governor) reset the temporal
// history; knobs read only by the resolve after NR (Chroma clamp, the
// transfer mode, the neural-floor guard - v6_common's ChromaClampStops,
// TransferMode and Curve 2) apply on the next frame, as the transfer and
// colour strengths always did.
inline constexpr Setting kSetCodecMode{
    "NRCodecMode", &codec_mode, defaults::kCodecMode, SettingEffect::kHistory};
inline constexpr Setting kSetProxyAnchor{
    "NRProxyAnchor", &proxy_anchor_nits, defaults::kProxyAnchorNits,
    SettingEffect::kNone};
inline constexpr Setting kSetSourceEncoding{
    "NRSourceEncoding", &source_encoding, defaults::kSourceEncoding,
    SettingEffect::kRecreate};
inline constexpr Setting kSetLinearUnitNits{
    "NRLinearUnitNits", &linear_unit_nits, defaults::kLinearUnitNits,
    SettingEffect::kRecreate};
inline constexpr Setting kSetSourcePrimaries{
    "NRSourcePrimaries", &source_primaries, defaults::kSourcePrimaries,
    SettingEffect::kNone};
inline constexpr Setting kSetPqCalibration{
    "NRPQCalibration", &pq_calibration, defaults::kPqCalibration,
    SettingEffect::kHistory};
inline constexpr Setting kSetDiffuseWhite{
    "NRDiffuseWhiteNits", &diffuse_white_nits, defaults::kDiffuseWhiteNits,
    SettingEffect::kNone};
inline constexpr Setting kSetPaperWhiteScale{
    "NRPaperWhiteScale", &paper_white_scale, defaults::kPaperWhiteScale,
    SettingEffect::kHistory};
inline constexpr Setting kSetTransferStrength{
    "NRTransferStrength", &transfer_strength, defaults::kTransferStrength,
    SettingEffect::kNone};
inline constexpr Setting kSetColorStrength{
    "NRColorStrength", &color_strength, defaults::kColorStrength, SettingEffect::kNone};
inline constexpr Setting kSetChromaClamp{
    "NRChromaClamp", &chroma_clamp_stops, defaults::kChromaClampStops,
    SettingEffect::kNone};
inline constexpr Setting kSetTransferMode{
    "NRTransferMode", &transfer_mode, defaults::kTransferMode, SettingEffect::kNone};
inline constexpr Setting kSetDisplayPedestal{
    "NRDisplayPedestal", &display_pedestal, defaults::kDisplayPedestal,
    SettingEffect::kNone};
inline constexpr Setting kSetNeuralFloorGuard{
    "NRNeuralFloorGuard", &neural_floor_guard, defaults::kNeuralFloorGuard,
    SettingEffect::kNone};
// The feed changes what NR is given, so a change restarts its history.
inline constexpr Setting kSetFeedMode{
    "NRFeedMode", &feed_mode, defaults::kFeedMode, SettingEffect::kHistory};
inline constexpr Setting kSetNormGovernor{
    "NRNormGovernor", &norm_governor, defaults::kNormGovernor, SettingEffect::kHistory};
inline constexpr Setting kSetNormAttack{
    "NRNormAttackStops", &norm_attack_stops, defaults::kNormAttackStops,
    SettingEffect::kNone};
inline constexpr Setting kSetNormRelease{
    "NRNormReleaseStops", &norm_release_stops, defaults::kNormReleaseStops,
    SettingEffect::kNone};
inline constexpr Setting kSetNormSlew{
    "NRNormSlewStops", &norm_slew_stops, defaults::kNormSlewStops,
    SettingEffect::kHistory};
// The guides: every one steers what the model is fed, so a change resets
// the temporal history.
inline constexpr Setting kSetDepthMode{
    "NRDepthMode", &depth_mode, defaults::kDepthMode, SettingEffect::kHistory};
inline constexpr Setting kSetMvecScaleX{
    "NRMVecScaleX", &motion_scale_x_multiplier, defaults::kMvecScale,
    SettingEffect::kHistory};
inline constexpr Setting kSetMvecScaleY{
    "NRMVecScaleY", &motion_scale_y_multiplier, defaults::kMvecScale,
    SettingEffect::kHistory};
inline constexpr Setting kSetChainedHistory{
    "NRChainedHistory", &chained_temporal_history, defaults::kChainedHistory,
    SettingEffect::kHistory};

// The sections, as the modern layout groups them.
using SettingGroup = std::span<const Setting* const>;
inline constexpr const Setting* kCoreSettings[] = {&kSetEnabled};
inline constexpr const Setting* kStrengthSettings[] = {&kSetIntensity};
inline constexpr const Setting* kLookSettings[] = {
    &kSetPreset,         &kSetStyle,         &kSetLocalTone,
    &kSetLocalStructure, &kSetSkinStructure, &kSetSkinIndependent,
    &kSetAutoMask,       &kSetUiCorrection};
inline constexpr const Setting* kLookResultSettings[] = {
    &kSetLookMode,        &kSetLookStrength,     &kSetLookBrighten,
    &kSetLookDarken,      &kSetLookColour,       &kSetLookHue,
    &kSetLookShadows,     &kSetLookMidtones,     &kSetLookHighlights,
    &kSetLookMaxBrighten, &kSetLookMaxDarken,    &kSetLookMaxColour,
    &kSetLookTone,        &kSetLookDetail,       &kSetLookDetailRadius,
    &kSetLookHalo,        &kSetLookStabilize,    &kSetLookStabilizeMs,
    &kSetLookStabilizeDetail};
inline constexpr const Setting* kPerformanceSettings[] = {
    &kSetPreSr,           &kSetFollowInputRes, &kSetResolutionScale,
    &kSetFoveation,       &kSetFoveationWidth, &kSetFoveationHeight,
    &kSetFoveationFalloff, &kSetFoveationEdge, &kSetLookUpsample};
inline constexpr const Setting* kStackingSettings[] = {
    &kSetStackPasses,     &kSetPassIntensity[0], &kSetPassIntensity[1],
    &kSetPassIntensity[2], &kSetPassTransfer[0], &kSetPassTransfer[1],
    &kSetPassTransfer[2], &kSetPassColor[0],     &kSetPassColor[1],
    &kSetPassColor[2],    &kSetPassFollow[0],    &kSetPassFollow[1],
    &kSetPassFollow[2],   &kSetPassStyle[0],     &kSetPassStyle[1],
    &kSetPassStyle[2],    &kSetPassLocalTone[0], &kSetPassLocalTone[1],
    &kSetPassLocalTone[2], &kSetPassStructure[0], &kSetPassStructure[1],
    &kSetPassStructure[2], &kSetPassSkin[0],      &kSetPassSkin[1],
    &kSetPassSkin[2],     &kSetPassAutoMask[0],  &kSetPassAutoMask[1],
    &kSetPassAutoMask[2], &kSetPassUiCorrection[0], &kSetPassUiCorrection[1],
    &kSetPassUiCorrection[2]};
inline constexpr const Setting* kHotkeySettings[] = {
    &kSetToggleKey, &kSetScreenshotKey, &kSetScreenshotFormat, &kSetScreenshotLayout,
    &kSetScreenshotJpegQuality, &kSetScreenshotMaxMB};
inline constexpr const Setting* kDiagnosticsSettings[] = {&kSetGpuTimers, &kSetEditTrace};
inline constexpr const Setting* kFixesSettings[] = {
    &kSetCodecMode,       &kSetProxyAnchor,      &kSetSourceEncoding,
    &kSetLinearUnitNits,  &kSetSourcePrimaries,  &kSetPqCalibration,
    &kSetDiffuseWhite,    &kSetPaperWhiteScale,  &kSetTransferStrength,
    &kSetColorStrength,   &kSetChromaClamp,      &kSetTransferMode,
    &kSetDisplayPedestal, &kSetNeuralFloorGuard, &kSetFeedMode,
    &kSetNormGovernor,    &kSetNormAttack,       &kSetNormRelease,
    &kSetNormSlew,        &kSetDepthMode,        &kSetMvecScaleX,
    &kSetMvecScaleY,      &kSetChainedHistory};
inline constexpr SettingGroup kSettingSections[] = {
    kCoreSettings,        kStrengthSettings, kLookSettings,
    kLookResultSettings,  kPerformanceSettings, kStackingSettings,
    kHotkeySettings,      kDiagnosticsSettings, kFixesSettings};

inline bool AnyModified(std::initializer_list<SettingGroup> groups) {
  for (const SettingGroup& group : groups) {
    for (const Setting* setting : group) {
      if (setting->Modified()) return true;
    }
  }
  return false;
}

// A section's reset link: every changed setting in it back to its default
// and persisted, then the strongest restart any of them needs.
inline void ResetSettings(std::initializer_list<SettingGroup> groups) {
  SettingEffect effect = SettingEffect::kNone;
  for (const SettingGroup& group : groups) {
    for (const Setting* setting : group) {
      if (!setting->Modified()) continue;
      setting->Store(setting->fallback);
      setting->Persist();
      effect = (std::max)(effect, setting->effect);
    }
  }
  ApplySettingEffect(effect);
}

// The Defaults view (alpha46, owner request): the look runs at its built-in
// values so the player can compare in game, while their own values wait in
// ui_defaults_saved.  The look is Strength, the Look section and stacking;
// resolution, pre-SR, hotkeys and the Fixes stay the player's, because they
// fit the game and the GPU rather than style the image.  Session-only:
// nothing is written to the config while it is on, the modern layout greys
// its controls, and Restore all or a switch to the classic layout ends it.
inline constexpr SettingGroup kLookCompare[] = {
    kStrengthSettings, kLookSettings, kLookResultSettings, kStackingSettings};
inline bool ui_defaults_view = false;
inline float ui_defaults_saved[std::size(kStrengthSettings) + std::size(kLookSettings)
                               + std::size(kLookResultSettings)
                               + std::size(kStackingSettings)] = {};

inline void SetDefaultsView(bool on) {
  if (on == ui_defaults_view) return;
  SettingEffect effect = SettingEffect::kNone;
  size_t index = 0;
  for (const SettingGroup& group : kLookCompare) {
    for (const Setting* setting : group) {
      if (on) ui_defaults_saved[index] = setting->Get();
      const float target = on ? setting->fallback : ui_defaults_saved[index];
      ++index;
      if (setting->Get() == target) continue;
      setting->Store(target);
      effect = (std::max)(effect, setting->effect);
    }
  }
  ui_defaults_view = on;
  ApplySettingEffect(effect);
  Log(reshade::log::level::info,
      on ? "Defaults view on: the look runs at the built-in values; the player's"
           " values are kept and nothing is saved"
         : "Defaults view off: the player's values are back");
}

// Persists every user setting under kConfigSection with its CURRENT runtime
// value (runtime == nullptr writes the global ReShade.ini, which is what the
// attach-time LoadConfiguration reads).  Used by the config breaker and by
// the stale-config re-stamp in LoadConfiguration; the overlay's controls
// write their own key as it changes (Setting::Commit).
// Every write goes to the GLOBAL config file, and it has to: the addon reads
// its settings back with `get_config_value(nullptr, ...)` in all 47 places,
// and ReShade's own header states what the first argument selects - "either
// the global config file (ReShade.ini next to the application executable), or
// one local to an effect runtime (ReShade[index].ini in the base path)"
// (external/reshade/include/reshade.hpp:201-204).  Passing the runtime here
// therefore saved a user's settings to a file this addon never opens the
// moment ReShade had more than one runtime - a second window, or one
// swapchain per eye in VR.  The settings appeared to take effect for the
// session and were gone at the next launch.
inline void PersistConfig() {
  reshade::set_config_value(nullptr, kConfigSection, "ConfigVersion", kConfigVersion);
  for (const SettingGroup& section : kSettingSections) {
    for (const Setting* setting : section) setting->Persist();
  }
  reshade::set_config_value(nullptr, kConfigSection, "NRMaxWorksets", max_worksets.load());
  reshade::set_config_value(
      nullptr, kConfigSection, "UiMode",
      ui_classic_layout.load() ? "classic" : "modern");
  StoreUiLanguage(ui_language_choice.load());
}

// Config breaker: every user setting back to the defaults namespace,
// persisted, with NR feature/workset state recreated so the new settings
// start from clean temporal state.  EnableHooks is deliberately untouched -
// it is a per-game support knob that applies at attach, not a user setting.
// It also ends the Defaults view: its values are the defaults now.
inline void ResetToDefaults(reshade::api::effect_runtime* runtime) {
  ui_defaults_view = false;
  for (const SettingGroup& section : kSettingSections) {
    for (const Setting* setting : section) setting->Store(setting->fallback);
  }
  max_worksets = kDefaultMaxWorksets;
  ui_classic_layout = false;
  ui_language_choice = 0;
  RecreateFeatures();
  PersistConfig();
  Log(
      reshade::log::level::info,
      "all settings restored to the built-in defaults");
}

// The test-only panel capture (U0): see OnUiCapturePresent.
inline std::filesystem::path ui_capture_dir;  // set once at attach, then read-only
// This frame's card as (tone << 8) | id, 0xffff when the panel was not drawn.
inline std::atomic_uint16_t ui_drawn_card{0xffff};
inline std::atomic_uint32_t ui_panel_draws{0};
inline std::atomic_int ui_style_depth_worst{0};
inline std::atomic_uint64_t ui_panel_ns{0};
// The reset buttons the last panel frame drew (ui::reset_buttons_drawn).
inline std::atomic_int ui_panel_resets{0};
// RENODX_NR_TEST_UI_VIEW=defaults: after the first capture the Defaults
// view goes on and every card is captured again (the tuned lane).
inline bool ui_capture_defaults_view = false;
inline bool ui_capture_tooltip = false;

// Set by the card's "Open Diagnostics" button; consumed by that section.
inline bool ui_open_diagnostics = false;

// The switch, in one place: the checkbox and the Off card's button both
// turn NR on or off through here.
inline void SetNrEnabledFromUi(bool on) {
  enabled = on;
  reshade::set_config_value(nullptr, kConfigSection, "NeuralUplift", on);
  if (on) RecreateFeatures();
}

// i18n: begin - display strings (tools/i18n/ui_strings.py catalog)
// ---- panel sections (U2, PLAN_UI_V7.md 2; reworked in alpha46) --------------
// The controls live in DrawSection* functions and the two layouts arrange
// them; every section is called from BOTH layouts, and every key
// LoadConfiguration reads is written by a control here or listed ini-only.
// Gate step 2g (tools/field/check_ui_reachability.py) holds both.  The
// classic layout draws the alpha44 widgets under their alpha44 labels; the
// modern one draws label-left rows with a reset button.  The labels that
// differ come from the caller's ui::PanelText.
//
// A collapsed section only skips its widgets for that frame, which is safe:
// every control acts on change, and the atomics keep the last value while a
// hidden slider's live-write does not run.

// A dragged slider applies its value live; its config write and the restart
// its setting needs wait for the mouse button to come up.  Restarting on
// every drag frame kept NR's history (or, for a re-create, NR itself) in
// warm-up for the whole drag.  IsItemDeactivatedAfterEdit never fires under
// the ReShade overlay (v5.1.1), so the release is read from the mouse;
// keyboard and gamepad steps have no button down and apply at once.
// OnOverlay settles once more after the panel, so a slider that stops being
// drawn mid-drag still applies.
inline const Setting* slider_pending = nullptr;
inline void SettleSlider(bool now) {
  if (slider_pending != nullptr && (now || !ImGui::IsMouseDown(0))) {
    slider_pending->Persist();
    ApplySettingEffect(slider_pending->effect);
    slider_pending = nullptr;
  }
}

// The controls over a Setting, one per kind: the classic layout's ImGui
// widget, or the modern row - label, control, and the reset button while
// the setting differs from its default.  A change goes through
// Setting::Commit: the value, its config write and its restart (a slider
// defers the last two to its release, above). A negative stored skin value
// is a stock sentinel: the control shows what that value sends to the model.
inline void SliderSetting(const ui::PanelText& text, const Setting& setting,
                          const char* label, float min, float max, const char* format,
                          const char* tooltip, const char* sentinel_format = nullptr,
                          float sentinel_shown = 0.f) {
  ImGui::PushID(setting.key);
  float value = setting.Get();
  const char* shown_format = format;
  if (sentinel_format != nullptr && value < 0.f) {
    value = sentinel_shown;
    shown_format = sentinel_format;
  }
  bool changed = false;
  if (text.modern) {
    ui::RowLabel(label, tooltip);
    changed = ui::Slider("##value", &value, min, max, shown_format, tooltip);
  } else {
    changed = ImGui::SliderFloat(label, &value, min, max, shown_format);
    if (tooltip != nullptr) ui::ItemTooltip("%s", tooltip);
  }
  if (changed) {
    if (slider_pending != &setting) SettleSlider(true);
    setting.Store(value);
    slider_pending = &setting;
  }
  SettleSlider(false);
  if (text.modern && setting.Modified()) {
    char stock[32];
    if (sentinel_format != nullptr && setting.fallback < 0.f) {
      std::snprintf(stock, sizeof(stock), ui::Tr(sentinel_format), sentinel_shown);
    } else {
      std::snprintf(stock, sizeof(stock), ui::Tr(format), setting.fallback);
    }
    if (ui::ResetButton("reset", stock)) setting.Commit(setting.fallback);
  }
  ImGui::PopID();
}

// A setting whose stored value is the index of its choice: a dropdown in
// the classic layout, segments in the modern one (a dropdown there too when
// the row is too narrow for the labels).
inline void ChoiceSetting(const ui::PanelText& text, const Setting& setting,
                          const char* label, std::span<const char* const> items,
                          const char* tooltip) {
  ImGui::PushID(setting.key);
  int index = static_cast<int>(setting.Get());
  const int count = static_cast<int>(items.size());
  bool changed = false;
  if (text.modern) {
    ui::RowLabel(label, tooltip);
    const int clicked = ui::Segmented("##value", items.data(), count, index, tooltip);
    changed = clicked >= 0 && clicked != index;
    if (changed) index = clicked;
  } else {
    changed = ImGui::Combo(label, &index, items.data(), count);
    if (tooltip != nullptr) ui::ItemTooltip("%s", tooltip);
  }
  if (changed) setting.Commit(static_cast<float>(index));
  if (text.modern && setting.Modified()
      && ui::ResetButton("reset", items[static_cast<size_t>(setting.fallback)])) {
    setting.Commit(setting.fallback);
  }
  ImGui::PopID();
}

// An on/off setting: a checkbox in the classic layout, a switch in the
// modern one.
inline void ToggleSetting(const ui::PanelText& text, const Setting& setting,
                          const char* label, const char* tooltip) {
  ImGui::PushID(setting.key);
  bool value = setting.Get() != 0.f;
  bool changed = false;
  if (text.modern) {
    ui::RowLabel(label, tooltip);
    changed = ui::Switch("##value", &value);
  } else {
    changed = ImGui::Checkbox(label, &value);
  }
  if (tooltip != nullptr) ui::ItemTooltip("%s", text.Tr(tooltip));
  if (changed) setting.Commit(value ? 1.f : 0.f);
  if (text.modern && setting.Modified()
      && ui::ResetButton("reset", setting.fallback != 0.f ? "on" : "off")) {
    setting.Commit(setting.fallback);
  }
  ImGui::PopID();
}

inline void DrawSectionNrCore(const ui::PanelText& text) {
  bool ui_enabled = enabled.load();
  if (text.modern) ui::RowLabel(text.nr_enable, ui::text::kNrEnableTip);
  if (text.modern ? ui::Switch("##nr_enable", &ui_enabled)
                  : ImGui::Checkbox(text.nr_enable, &ui_enabled)) {
    SetNrEnabledFromUi(ui_enabled);
  }
  if (text.modern) {
    // The hotkey beside what it toggles, then the Defaults view, which
    // decides what every row below it shows.
    ImGui::SameLine();
    ImGui::TextDisabled("%s", VirtualKeyName(toggle_hotkey.load()).c_str());
    ui::RowLabel(ui::text::kSettingsView, ui::text::kSettingsViewTip);
    const int view = ui::Segmented(
        "##settings_view", ui::text::kSettingsViewLabels,
        static_cast<int>(std::size(ui::text::kSettingsViewLabels)),
        ui_defaults_view ? 0 : 1, ui::text::kSettingsViewTip);
    if (view >= 0) SetDefaultsView(view == 0);
    if (ui_defaults_view) {
      ui::StyleScope note;
      note.FontSize(ui::SmallFontSize()).Color(ImGuiCol_Text, ui::tokens::kAccent);
      ImGui::PushTextWrapPos(0.f);
      ImGui::TextUnformatted(ui::Tr(ui::text::kDefaultsViewNote));
      ImGui::PopTextWrapPos();
    }
  }

  // Overall Intensity is primary (v5.3 UI policy): master strength of the
  // neural pass belongs next to the enable switch, not under styling.
  ImGui::BeginDisabled(ui_defaults_view);
  SliderSetting(text, kSetIntensity, text.intensity, 0.f, 2.f, "%.2f",
                ui::text::kIntensityTip);
  ImGui::EndDisabled();
}

// The model's steering rows of one stack pass (PLAN_NR_LOOK_V71.md 3 G):
// pass 1's are the Look section's own; passes 2-4 draw theirs under
// Performance once they stop following pass 1.  Each steers the model, so a
// change restarts its history.
inline void DrawModelSteering(const ui::PanelText& text, uint32_t pass) {
  constexpr const char* kStyles[] = {"Default", "Natural", "Cinematic"};
  ChoiceSetting(
      text, kModelSteering[pass].style, text.Pick("NR Style", "Style"), kStyles,
      "Default = the runtime's own choice; Natural and Cinematic force that"
      " look explicitly.");
  SliderSetting(
      text, kModelSteering[pass].local_tone,
      text.Pick("Local Tone Intensity", "Local tone"), 0.f, 2.f, "%.2f",
      "Tone strength of the neural pass region by region.");
  SliderSetting(
      text, kModelSteering[pass].structure,
      text.Pick("Structure Intensity", "Structure"), 0.f, 2.f, "%.2f",
      "Strength of the neural pass on fine structure and detail.  It changes"
      " what the model makes and restarts it; Fine detail below reshapes what"
      " it made, live.");
  // Runtime 310.8 reads Skin only when Character mask is on. A stored -1
  // means stock, shown as 1 with independent steering or as Structure in
  // the legacy mode. The 0..1 range has no false "negative smooths" half.
  const bool masked = kModelSteering[pass].auto_mask.Get() != 0.f;
  const bool independent = skin_independent.load();
  const float structure = kModelSteering[pass].structure.Get();
  if (masked && independent && structure > 1.f) {
    if (text.modern) ui::RowLabel("");
    ImGui::TextDisabled(text.Tr("sent as 1.00: higher values are ignored"));
  }
  ImGui::BeginDisabled(!masked);
  SliderSetting(
      text, kModelSteering[pass].skin,
      text.Pick("Character/Skin Structure", "Character / skin"), 0.f, 1.f, "%.2f",
      masked ? "How much the model reworks the skin of the people the Character"
               " mask finds: 0 is the least, 1 is stock.  Rough skin gains"
               " texture, smooth skin is evened out."
             : "Needs the Character mask: without it the model finds no skin"
               " and ignores this setting.",
      independent ? "%.2f (stock)" : "%.2f = Structure",
      independent ? 1.f : structure);
  ImGui::EndDisabled();
  ToggleSetting(
      text, kModelSteering[pass].auto_mask,
      text.Pick("Automatic / Character Mask", "Character mask"),
      "Lets the runtime find people, so Character / skin applies to their"
      " skin.  It also changes the rest of the picture slightly: brightness"
      " by about 2 percent on game frames.");
  if (pass == 0) {
    ToggleSetting(
        text, kSetSkinIndependent,
        text.Pick("Skin Independent of Structure", "Independent skin"),
        "With the Character mask on, skin keeps its Character / skin setting"
        " when Structure changes, in every stack pass.  Structure is then sent"
        " at most 1, because above 1 the model ignores Character / skin; below"
        " 1 it still reaches faces through the model itself.  Off sends both"
        " as they are, as before rc11.");
  }
  ToggleSetting(
      text, kModelSteering[pass].ui_correction,
      text.Pick("NR UI Correction", "UI correction"),
      "The NR runtime's UI correction.  Leave it off unless HUD elements look"
      " wrong with Neural Rendering on.");
}

// The Look's Model group: the model's styling knobs.  The modern layout shows
// them open under the switch, because they are the settings players tune
// most (owner direction, alpha46: "those are the most important settings");
// the classic layout keeps them under its collapsed "Model styling
// (advanced)".
inline void DrawSectionModelStyling(const ui::PanelText& text) {
  if (text.modern) ui::SubCaption(ui::text::kLookModel);
  constexpr const char* kPresets[] = {
      "Default", "Preset #1", "Preset #2", "Preset #3"};
  ChoiceSetting(
      text, kSetPreset, text.Pick("NR Preset", "Preset"), kPresets,
      "Render-preset hint passed to the NR runtime; Default lets the"
      " runtime choose.");
  DrawModelSteering(text, 0);
}

// The Look's Result group (PLAN_NR_LOOK_V71.md 3 A-F): the look stage
// reshapes what the model returned, live, so no row here restarts NR's
// history.  The rows stay drawn while the switch is off, disabled, so the
// player sees what it offers; off, with Upsampling on Classic, the stage is
// absent and the image is bit-identical to a build without it.
inline void DrawLookGateHint(const ui::PanelText& text) {
  if (look_mode.load()) return;
  if (text.modern) ui::RowLabel("");
  ImGui::TextDisabled(
      "%s", text.Tr("Inert while Adjust NR's look is off."));
}

inline void DrawSectionLookResult(const ui::PanelText& text) {
  if (text.modern) ui::SubCaption(ui::text::kLookResult);
  ToggleSetting(
      text, kSetLookMode, "Adjust NR's look",
      "Reshapes what Neural Rendering returned, live: how far it brightens,"
      " darkens and recolours, by tonal range and by scale.  Off leaves its"
      " output exactly as it is.");
  DrawLookGateHint(text);
  if (look_unavailable.load()) {
    if (text.modern) ui::RowLabel("");
    ImGui::TextDisabled("%s", text.Tr("unavailable this session: ReShade.log says why"));
  }
  ImGui::BeginDisabled(!look_mode.load());
  SliderSetting(
      text, kSetLookStrength, "Edit strength", 0.f, 2.f, "%.2f",
      "Scales the whole change Neural Rendering made, in stops: 0 takes it"
      " away, 1 keeps it, 2 doubles it.  Strength steers the model; this"
      " reshapes its result.");
  SliderSetting(
      text, kSetLookBrighten, "Brightening", 0.f, 2.f, "%.2f",
      "How much of the model's brightening is kept: 0 drops it, 2 doubles it.");
  SliderSetting(
      text, kSetLookDarken, "Darkening", 0.f, 2.f, "%.2f",
      "How much of the model's darkening is kept, the dark halos it can leave"
      " around bright objects included.");
  SliderSetting(
      text, kSetLookColour, "Colour", 0.f, 2.f, "%.2f",
      "How far the model moves colours toward or away from grey: 0 keeps the"
      " game's saturation, 2 doubles the change.");
  SliderSetting(
      text, kSetLookHue, "Hue shift", 0.f, 2.f, "%.2f",
      "How far the model turns hues (its tint): 0 keeps the game's hues.");
  SliderSetting(
      text, kSetLookShadows, "Shadows", 0.f, 2.f, "%.2f",
      "Scales the change where the game's image is dark.");
  SliderSetting(
      text, kSetLookMidtones, "Midtones", 0.f, 2.f, "%.2f",
      "Scales the change in the game's midtones.");
  SliderSetting(
      text, kSetLookHighlights, "Highlights", 0.f, 2.f, "%.2f",
      "Scales the change where the game's image is bright.");
  ImGui::EndDisabled();
  // The fold opens with Shape result off too, and says when it holds a
  // changed value: through rc4 it was greyed out with the switch, so a
  // stored Halo suppression of 1 (Alan Wake 2, rc4) lifted the image near
  // bright objects the moment the switch went on, from a fold the player
  // could not open to find it.  Only its controls follow the switch.
  constexpr const Setting* kLimits[] = {
      &kSetLookMaxBrighten, &kSetLookMaxDarken, &kSetLookMaxColour,
      &kSetLookTone,        &kSetLookDetail,    &kSetLookDetailRadius,
      &kSetLookHalo,        &kSetLookStabilize, &kSetLookStabilizeMs,
      &kSetLookStabilizeDetail};
  const bool limits_changed = AnyModified({kLimits});
  if (ui::expand_all_sections) ImGui::SetNextItemOpen(true);
  if (limits_changed) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
  if (ImGui::TreeNodeEx(
          "look_limits", 0, "%s",
          text.Tr(limits_changed ? ui::text::kLookLimitsChanged : ui::text::kLookLimits))) {
    ImGui::BeginDisabled(!look_mode.load());
    DrawLookGateHint(text);
    SliderSetting(
        text, kSetLookMaxBrighten, "Max brightening", 0.f, 4.f, "%.2f stops",
        "A soft ceiling on how far the model may brighten a pixel; 0 is no"
        " ceiling.");
    SliderSetting(
        text, kSetLookMaxDarken, "Max darkening", 0.f, 4.f, "%.2f stops",
        "A soft floor on how far the model may darken a pixel; 0 is no floor.");
    SliderSetting(
        text, kSetLookMaxColour, "Max colour change", 0.f, 2.f, "%.2f stops",
        "A soft limit on how far the model may move a colour; 0 is no limit.");
    SliderSetting(
        text, kSetLookTone, "Large-scale tone", 0.f, 2.f, "%.2f",
        "Scales the model's broad lighting change; Fine detail scales the"
        " small-scale one.");
    SliderSetting(
        text, kSetLookDetail, "Fine detail", 0.f, 2.f, "%.2f",
        "Scales the model's small-scale change, live.  Structure changes what"
        " the model makes and restarts it; this reshapes what it made.");
    SliderSetting(
        text, kSetLookDetailRadius, "Detail radius", 0.25f, 4.f, "%.2f%%",
        "Where broad ends and fine begins, as a share of the image height.");
    SliderSetting(
        text, kSetLookHalo, "Halo suppression", 0.f, 1.f, "%.2f",
        "Reduces the dark halos Neural Rendering can leave next to bright"
        " objects.  Darkening spread evenly over a surface, and a shadow"
        " beside a dark object, are left alone.  A larger Detail radius"
        " reaches wider halos.");
    constexpr const char* kStabilizeModes[] = {"Off", "Static", "Motion"};
    ChoiceSetting(
        text, kSetLookStabilize, "Stabilize", kStabilizeModes,
        "Smooths the model's broad change over time.  Static suits still"
        " views; Motion follows the game's motion vectors.  Either adds a"
        " little lag to lighting changes.");
    ImGui::BeginDisabled(look_stabilize.load() == 0u);
    SliderSetting(
        text, kSetLookStabilizeMs, "Stabilize time", 10.f, 250.f, "%.0f ms",
        "How long the smoothing remembers: longer is steadier and slower to"
        " follow a lighting change.");
    ToggleSetting(
        text, kSetLookStabilizeDetail, "Stabilize detail",
        "Smooths the small-scale change too, not only the broad one.");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::TreePop();
  }
}

// A discrete working-resolution change (the classic mode combo, the modern
// presets): persists NRFollowInputRes and NRResolutionScale as they now
// stand, and the next evaluation applies the new working dimensions at once,
// skipping the game-driven contract debounce.  `mode` is the classic combo's
// index (0 native, 1 follow render resolution, 2 scaled), which the frozen
// log line below names.
inline void CommitResolutionMode(int mode) {
  kSetFollowInputRes.Persist();
  kSetResolutionScale.Persist();
  nr_resolution_commit_now = true;
  Log(
      reshade::log::level::info,
      "NR working resolution mode " + std::to_string(mode)
          + " (scale " + std::to_string(nr_resolution_scale.load())
          + "); applies on the next frame");
}

// The reset button of the modern layout's two resolution rows: back to
// Full, applied at once.
inline void ResolutionResetButton(const char* id) {
  if ((kSetFollowInputRes.Modified() || kSetResolutionScale.Modified())
      && ui::ResetButton(id, ui::text::kResolutionLabels[0])) {
    nr_follow_input_res = false;
    nr_resolution_scale = 1.f;
    CommitResolutionMode(0);
  }
}

inline void DrawSectionStackingResolution(const ui::PanelText& text) {
  // NR insertion point.  After (default): NR runs on the game's finished DLSS
  // output at display resolution - correct whenever the game feeds DLSS a
  // display-res color.  Before (pre-SR): NR runs on the game's DLSS INPUT
  // color at render resolution against the game's own depth/motion guides, so
  // color and guides share a resolution by construction; the game's DLSS then
  // upscales an already-enhanced image.  For render-resolution DLSS titles
  // (e.g. KCD2: 1440p color in, 4K out, 1440p guides) pre-SR is the in-spec
  // arrangement; the after path is out of contract there.
  ToggleSetting(
      text, kSetPreSr, text.pre_sr,
      text.modern ? "Runs Neural Rendering on the game's image before DLSS"
                    " upscales it (pre-SR), against the game's own depth and"
                    " motion.  The right choice for games whose DLSS renders"
                    " below the display resolution.  Games that upscale with"
                    " Ray Reconstruction keep running after it: its input is"
                    " still noisy."
                  : nullptr);

  // NR stacking (v5.0.1): chained feature-18 passes.  Pass 1 IS the global
  // sliders above (and the existing config keys) and carries the model's
  // temporal history.  Passes 2..kMaxNrPasses are reset on every frame -
  // stateless within-frame refinement passes - so stacking strengthens the
  // per-frame effect without cross-frame accumulation.  Their strength
  // controls appear below as the count is raised.
  ImGui::BeginDisabled(ui_defaults_view);
  // ImGui::Combo's current item is a 0-based index into the labels while
  // stack_passes is the 1-based pass count the labels name; convert in both
  // directions or the preview shows the label one row below the real state
  // (selecting "1 (single pass)" displays "2 passes").
  int ui_passes = static_cast<int>(stack_passes.load()) - 1;
  constexpr const char* kPassCounts[] = {
      "1 (single pass)", "2 passes", "3 passes", "4 passes"};  // i18n: skip (Developer view)
  constexpr const char* kPassSegments[] = {"1", "2", "3", "4"};
  // Every pass keeps its own history by default (kChainedHistory); through
  // rc6 this tip said passes 2+ were stateless, the pre-v5.0.1 behaviour.
  const char* const passes_tip = text.Pick(
      "Every pass keeps its own temporal history by default (Chained temporal"
      " history); each extra pass costs one more NR evaluate.",
      "Runs Neural Rendering again on its own result: a stronger effect, and"
      " each extra pass costs the GPU time of one more pass.  Every pass keeps"
      " its own history (Chained history, under Fixes).");
  bool passes_changed = false;
  if (text.modern) {
    ui::RowLabel(text.stack_passes, passes_tip);
    const int clicked = ui::Segmented("##passes", kPassSegments,
                                      static_cast<int>(kMaxNrPasses), ui_passes, passes_tip);
    passes_changed = clicked >= 0 && clicked != ui_passes;
    if (passes_changed) ui_passes = clicked;
  } else {
    ImGui::TextUnformatted("NR stacking");  // i18n: skip (Developer view)
    passes_changed = ImGui::Combo(
        text.stack_passes,
        &ui_passes,
        kPassCounts,
        static_cast<int>(kMaxNrPasses));
    ui::ItemTooltip("%s", passes_tip);
  }
  if (passes_changed) {
    kSetStackPasses.Commit(static_cast<float>(
        std::clamp(ui_passes + 1, 1, static_cast<int>(kMaxNrPasses))));
  }
  if (text.modern && kSetStackPasses.Modified()
      && ui::ResetButton("passes_reset", kPassSegments[0])) {
    kSetStackPasses.Commit(kSetStackPasses.fallback);
  }
  if (!text.modern && stack_passes.load() > 1) {
    ImGui::TextDisabled(
        "%s",  // i18n: skip (Developer view)
        chained_temporal_history.load()
            ? "Every pass keeps its own temporal history (Chained temporal history)."  // i18n: skip (Developer view)
            : "Pass 1 keeps temporal history; passes 2+ are reset every frame"  // i18n: skip (Developer view)
              " (stateless refinement - no accumulation).");
  }
  for (uint32_t pass = 1; pass < stack_passes.load() && pass < kMaxNrPasses;
       ++pass) {
    ImGui::PushID(static_cast<int>(pass));
    char caption[64];
    std::snprintf(caption, sizeof(caption), text.Tr(text.Pick("Stack pass %u", "Pass %u")),
                  pass + 1);
    if (text.modern) {
      ui::SubCaption(caption);
    } else {
      ImGui::TextUnformatted(caption);
    }
    SliderSetting(text, kSetPassIntensity[pass - 1], text.Pick("Intensity", "Strength"),
                  0.f, 2.f, "%.2f", ui::text::kIntensityTip);
    // Greyed out like pass 1's (DrawSectionHdrDetails), the value still shown.
    const bool transfer_inert = transfer_strength_inert.load(std::memory_order_relaxed);
    ImGui::BeginDisabled(transfer_inert);
    SliderSetting(text, kSetPassTransfer[pass - 1],
                  text.Pick("HDR Transfer Strength", "HDR transfer"), 0.f, 1.f, "%.2f",
                  transfer_inert ? ui::text::kTransferInertTip
                  : text.modern  ? ui::text::kTransferTip
                                 : nullptr);
    ImGui::EndDisabled();
    SliderSetting(text, kSetPassColor[pass - 1], text.Pick("Color Strength", "Color"),
                  0.f, 1.f, "%.2f", text.modern ? ui::text::kColorTip : nullptr);
    ToggleSetting(
        text, kSetPassFollow[pass - 1], "Same as pass 1",
        "This pass steers the model with pass 1's Look settings.  Off gives it"
        " its own, below.");
    if (!pass_follow[pass - 1].load()) DrawModelSteering(text, pass);
    ImGui::PopID();
  }
  ImGui::EndDisabled();

  // NR working resolution (v5.1).  One control for both insertion points:
  // native, following the game's render (guide) resolution, or a scaled
  // fraction of the reference - the DLSS output for the after path, the NGX
  // color for pre-SR.  The NR feature always runs 1:1 at the working
  // resolution; the decode stage upscales, motion vectors pass unchanged (F8).
  // The on-disk settings stay NRFollowInputRes / NRResolutionScale; the
  // classic combo and the modern presets only derive their selection from
  // them, and the modern layout keeps its presets at the top of the panel.
  int ui_res_mode = nr_follow_input_res.load() ? 1
      : (nr_resolution_scale.load() < 0.999f ? 2 : 0);
  bool resolution_changed = false;
  if (!text.modern) {
    ImGui::TextUnformatted("NR working resolution");  // i18n: skip (Developer view)
    constexpr const char* kResModes[] = {
        "Native (100%)", "Follow render resolution", "Scaled"};  // i18n: skip (Developer view)
    if (ImGui::Combo("Resolution mode", &ui_res_mode, kResModes, 3)) {  // i18n: skip (Developer view)
      resolution_changed = true;
      nr_follow_input_res = ui_res_mode == 1;
      if (ui_res_mode == 0) {
        nr_resolution_scale = 1.f;
      } else if (ui_res_mode == 2 && nr_resolution_scale.load() >= 0.999f) {
        // Entering Scaled from Native snaps to a meaningful starting point so
        // the mode change has an immediate visible effect.
        nr_resolution_scale = 0.66f;
      }
    }
  }
  constexpr const char* kScaleTip =
      "The working resolution as a share of the full one.  Lower costs less"
      " GPU time.";
  if (text.modern ? !nr_follow_input_res.load() : ui_res_mode == 2) {
    // Written LIVE on every change: the value settles through the
    // present-path contract debounce, so a drag costs exactly one rebuild
    // once the value holds for kContractDebounceFrames (~0.5 s). Deliberately
    // NOT gated on widget-active transitions - ReShade's overlay input does
    // not reliably deliver them for drags, which silently pinned the slider
    // to its entry value. Per-tick rebuilds remain impossible because the
    // debounce counter resets while the dragged dims keep changing.
    float ui_res_pct = nr_resolution_scale.load() * 100.f;
    if (text.modern) {
      ui::RowLabel("Resolution scale", kScaleTip);
      ui::Slider("##resolution_scale", &ui_res_pct, 33.f, 100.f, "%.0f%%", kScaleTip);
    } else {
      ImGui::SliderFloat("Resolution scale", &ui_res_pct, 33.f, 100.f, "%.0f%%");
    }
    ui_res_pct = std::clamp(ui_res_pct, 33.f, 100.f);
    const float live_scale = ui_res_pct / 100.f;
    if (live_scale != nr_resolution_scale.load()) {
      nr_resolution_scale = live_scale;
      kSetResolutionScale.Persist();
    }
  } else if (text.modern) {
    ui::RowLabel("Resolution scale", kScaleTip);
    ImGui::TextDisabled("%s", ui::Tr("follows the game (Match game)"));
  }
  if (text.modern) ResolutionResetButton("resolution_scale_reset");
  if (resolution_changed) CommitResolutionMode(ui_res_mode);
  // Beside the resolution since rc5, independent of Shape result: it is how
  // a reduced working resolution's edit reaches the output.
  constexpr const char* kUpsampleModes[] = {"Classic", "Edge-aware"};
  ChoiceSetting(
      text, kSetLookUpsample, "Upsampling", kUpsampleModes,
      "How Neural Rendering's change reaches the screen when it works below"
      " the output resolution (no effect at 100%).  Edge-aware keeps the"
      " change on the edges it belongs to; Classic is the upsampling of"
      " earlier versions.");
  // Foveated resolution (foveation.hpp): after-upscale only.  The centre zone
  // keeps the resolution above; outside it each axis falls to Edge along a
  // smoothstep over the fall-off.  Sizes are output pixels: the slider ranges
  // follow the running output, and every frame clamps them again, so one
  // setting is valid at any resolution.
  {
    const bool was_foveated = nr_foveation.load();
    ImGui::BeginDisabled(nr_before_upscale.load());
    ToggleSetting(
        text, kSetFoveation, "Foveated resolution",
        "After upscaling only.  Keeps the resolution above inside a centre"
        " zone and lowers it toward every screen edge, so Neural Rendering"
        " costs less.  Sizes are in screen pixels and adapt to any display.");
    if (nr_foveation.load() != was_foveated) nr_resolution_commit_now = true;
    if (nr_foveation.load()) {
      // Ranges from the running output; before the first frame, generous
      // defaults (the evaluate clamps regardless).
      const uint32_t out_w = last_output_width.load();
      const uint32_t out_h = last_output_height.load();
      const float max_w = static_cast<float>(out_w != 0 ? out_w : 7680u);
      const float max_h = static_cast<float>(out_h != 0 ? out_h : 4320u);
      const float max_falloff = std::max(max_w, max_h) * 0.5f;
      // A stored value above the running output (set on a larger display)
      // is kept as is - the evaluate clamps it and the readout below shows
      // the effective size - so switching displays never loses a setting.
      SliderSetting(
          text, kSetFoveationWidth, "Centre width", 0.f, max_w, "%.0f px",
          "Width of the full-resolution centre zone, in screen pixels.");
      SliderSetting(
          text, kSetFoveationHeight, "Centre height", 0.f, max_h, "%.0f px",
          "Height of the full-resolution centre zone, in screen pixels.  At the"
          " full screen height only the sides are lowered.");
      SliderSetting(
          text, kSetFoveationFalloff, "Fall-off", 0.f, max_falloff, "%.0f px",
          "Distance over which the resolution blends from the centre to the"
          " edge value, on every side.  0 is a hard step.  Longer than the"
          " space to the screen edge simply never reaches the edge value.");
      SliderSetting(
          text, kSetFoveationEdge, "Edge resolution", foveation::kMinEdge,
          foveation::kMaxEdge, "%.2f",
          "Resolution beyond the fall-off, per axis, relative to the centre"
          " (0.50 = half; corners get both).");
      if (out_w != 0 && out_h != 0) {
        // What the running output actually gets.
        foveation::Inactive why = foveation::Inactive::kNone;
        const foveation::Grid grid = CurrentFoveation(out_w, out_h, &why);
        const foveation::Axis ax = foveation::MakeAxis(
            out_w, nr_foveation_width.load(), nr_foveation_falloff.load(),
            nr_foveation_edge.load());
        const foveation::Axis ay = foveation::MakeAxis(
            out_h, nr_foveation_height.load(), nr_foveation_falloff.load(),
            nr_foveation_edge.load());
        char effective[160];
        if (why == foveation::Inactive::kNone) {
          std::snprintf(
              effective, sizeof(effective),
              "%ux%u: centre %ux%u, fall-off %u/%u px, edges %.2f/%.2f, %.0f%% of the"
              " NR pixels",
              out_w, out_h, ax.center_px, ay.center_px, ax.falloff_px, ay.falloff_px,
              ax.edge_reached, ay.edge_reached, grid.PixelFactor() * 100.0);
        } else {
          std::snprintf(
              effective, sizeof(effective), "%ux%u: inactive (%s)", out_w, out_h,
              why == foveation::Inactive::kCentreCoversScreen
                  ? "the centre covers the screen"
                  : "would save under 5% - not worth the extra pass");
        }
        if (text.modern) {
          ui::RowLabel("Foveation");
          ImGui::TextWrapped("%s", effective);
        } else {
          ImGui::TextWrapped("Foveation at %s", effective);  // i18n: skip (Developer view)
        }
      }
    }
    ImGui::EndDisabled();
  }
  if (last_input_width.load() != 0) {
    if (text.modern) {
      ui::RowLabel("Working at");
      ImGui::Text("%ux%u", last_input_width.load(), last_input_height.load());
    } else {
      ImGui::Text(
          "Applied NR working resolution: %ux%u",  // i18n: skip (Developer view)
          last_input_width.load(),
          last_input_height.load());
    }
  }

  // The workset pool budget is automatic (kDefaultMaxWorksets, ini-only
  // override via NRMaxWorksets).  It is an internal memory bound for games
  // that evaluate several NGX passes per frame - NOT an NR stacking control,
  // so it deliberately has no menu entry.
}

inline void DrawSectionCodecCombo(const ui::PanelText& text) {
  // Codec operating mode - applies on the next frame, no NR feature restart.
  //  Auto (v6 default): linear working-space codec.  Calibrated absolute
  //  sources keep the Classic gain; relative scene-linear sources
  //  (KCD2-class pre-exposure) track the metered scene divisor.
  //  Classic: the fixed legacy gain; the paper-white scale slider below is
  //  the knob.
  //  Anchored: proxy-shoulder knee over calibrated units; the NR dark
  //  pedestal is subtracted at commit.
  constexpr const char* kCodecModes[] = {
      "Auto (recommended)", "Classic (manual)", "Anchored (manual)",
      "Display (fixed)"};
  constexpr uint32_t kCodecValues[] = {2u, 0u, 1u, 3u};
  const uint32_t current_codec = codec_mode.load();
  int ui_codec_mode = current_codec == 0u ? 1
                      : current_codec == 1u ? 2
                      : current_codec == 3u ? 3
                                            : 0;
  constexpr const char* kCodecTip =
      "How the game's HDR image is prepared for Neural Rendering and brought"
      " back.  Auto is right for nearly every game.";
  if (text.modern) ui::RowLabel(text.codec, kCodecTip);
  const char* codec_items[std::size(kCodecModes)];
  for (size_t i = 0; i < std::size(kCodecModes); ++i) codec_items[i] = text.Tr(kCodecModes[i]);
  if (ImGui::Combo(
          text.modern ? "##codec" : text.codec,
          &ui_codec_mode,
          codec_items,
          static_cast<int>(std::size(kCodecModes)))) {
    kSetCodecMode.Commit(static_cast<float>(kCodecValues[ui_codec_mode]));
  }
  if (text.modern) {
    ui::ItemTooltip("%s", ui::Tr(kCodecTip));
    if (kSetCodecMode.Modified() && ui::ResetButton("codec_reset", kCodecModes[0])) {
      kSetCodecMode.Commit(kSetCodecMode.fallback);
    }
  }
}

inline void DrawSectionHdrDetails(const ui::PanelText& text) {
  const uint32_t current_codec = codec_mode.load();
  // v6 normalization is intentionally not a user-tunable state machine.
  // Relative HDR is normalized from the current frame on the GPU; calibrated
  // absolute HDR uses its fixed source-unit contract.
  bool have_hdr_workset = false;
  codec::SourceUnits ui_units = {};
  {
    RuntimeLock lock(runtime_mutex);
    for (const auto& [_, res] : worksets) {
      if (res.hdr_mode == 0) continue;
      have_hdr_workset = true;
      ui_units = res.units;
      break;
    }
  }
  if (have_hdr_workset) {
    // A readout, not the Normalization choice further down.
    if (text.modern) ui::RowLabel("Scaling now");
    if (ui_units.absolute && ui_units.unit_nits > 0.f) {
      ImGui::Text(
          text.Tr(text.Pick("Normalization: fixed calibrated source (%.0f nits per unit).",
                            "fixed, %.0f nits per unit")),
          ui_units.unit_nits);
    } else {
      ImGui::Text(
          text.Tr(text.Pick("Normalization: same-frame GPU autoscale, governor %s.",
                            "GPU autoscale, governor %s")),
          text.Tr(norm_governor.load() == 0u ? "off (raw candidate)" : "slew"));
    }
  }
  if (current_codec == 1u || current_codec == 3u) {
    SliderSetting(
        text, kSetProxyAnchor, text.Pick("Proxy Anchor (nits)", "Proxy anchor (nits)"),
        0.5f, 32.f, text.Pick("%.1f nits", "%.1f"),
        "Used only for calibrated absolute HDR. Relative HDR uses the current"
        " frame's GPU-derived scale.");
  }
  if (ImGui::TreeNodeEx(
          "source_interpretation", 0, "%s",
          text.Tr(text.Pick("Source interpretation (advanced)", "Source interpretation")))) {
    // Overrides are for mis-declaring engines only; Auto reads the swapchain
    // format evidence.
    constexpr const char* kSourceEncodings[] = {
        "Auto (format evidence)",
        "SDR / relative",
        "Linear",
        "PQ / 10,000 nits"};
    ChoiceSetting(text, kSetSourceEncoding, text.Pick("Source Encoding", "Encoding"),
                  kSourceEncodings,
                  text.modern ? "What the game's image is.  Auto reads it from the"
                                " image format; override only an engine that"
                                " declares its format wrongly."
                              : nullptr);
    SliderSetting(
        text, kSetLinearUnitNits, text.Pick("Linear Unit (nits)", "Linear unit (nits)"),
        0.f, 10000.f, "%.0f",
        "Nits per 1.0 of a linear source: 0 keeps it relative (GPU autoscale); a"
        " positive value marks it calibrated/absolute.");
    constexpr const char* kSourcePrimaries[] = {
        "Auto", "BT.709", "BT.2020", "AP1 (ACEScg)"};
    ChoiceSetting(
        text, kSetSourcePrimaries, text.Pick("Source Primaries", "Primaries"),
        kSourcePrimaries,
        "Advisory label only - the neural model works in wide gamut and no"
        " conversion is applied.");
    SliderSetting(
        text, kSetPqCalibration, text.Pick("PQ Calibration", "PQ calibration"), 0.005f,
        16.f, "%.4f",
        "Extra scene-linear gain for PQ sources (classic and Auto over"
        " absolute units).  1 = the 203-nit BT.2408 reference; the pre-v6"
        " default was 2.5375 and migrated configs keep it.");
    ImGui::TreePop();
  }
  SliderSetting(
      text, kSetDiffuseWhite, text.diffuse_white, 80.f, 1000.f, "%.0f",
      text.Pick("PQ HDR bridge anchor: the nit level treated as diffuse white"
                " (BT.2408 reference is 203).  Ignored on the SDR path; the"
                " linear HDR path uses Paper-White Scale instead.",
                ui::text::kPaperWhiteTip));
  SliderSetting(
      text, kSetPaperWhiteScale,
      text.Pick("Scene Paper-White Scale", "Scene white scale"), 0.005f, 16.f, "%.3f",
      text.Pick("Classic and Auto-over-linear codec gain (2.5375 = 203-nit"
                " diffuse white); PQ sources use Diffuse White and PQ"
                " Calibration instead.",
                ui::text::kSceneWhiteTip));
  constexpr const char* kDisplayPedestals[] = {"Auto (recommended)", "Always"};
  ChoiceSetting(
      text, kSetDisplayPedestal, "Dark pedestal removal", kDisplayPedestals,
      "Float-HDR games: whether the dark lift the model adds in near-black"
      " areas is removed; HDR Transfer Strength sets how much.  Auto skips"
      " the removal where the game's brightness is relative (Kingdom Come"
      " Deliverance II, Alan Wake 2, GTA V): there it reads shadow noise as a"
      " lift and darkens shadows block by block, which flickers.  Always"
      " removes it in every HDR game, as releases up to rc7 did.");
  // While the evaluated frame skips the removal, HDR Transfer Strength has
  // nothing to scale: greyed out with the reason as its tooltip, the value
  // still shown.
  const bool transfer_inert = transfer_strength_inert.load(std::memory_order_relaxed);
  ImGui::BeginDisabled(transfer_inert);
  SliderSetting(
      text, kSetTransferStrength, text.Pick("HDR Transfer Strength", "HDR transfer"),
      0.f, 1.f, "%.2f",
      transfer_inert ? ui::text::kTransferInertTip
      : text.modern  ? ui::text::kTransferTip
                     : nullptr);
  ImGui::EndDisabled();
  SliderSetting(
      text, kSetColorStrength, text.Pick("Color Strength", "Color"), 0.f, 1.f, "%.2f",
      text.modern ? ui::text::kColorTip : nullptr);
  // v9 Phase 6 transfer bounds (see defaults).  Both steer the resolve's
  // math, so a change resets the temporal history like the other codec
  // knobs.
  SliderSetting(
      text, kSetChromaClamp, text.Pick("Chroma clamp (stops)", "Chroma clamp"),
      0.25f, 2.f, "%.2f",
      "Bound of the chroma part of the neural transfer, in stops either"
      " side of the luma gain.  1.00 keeps the validated default; the"
      " luma gain itself stays bounded to +/-2 stops.");
  constexpr const char* kTransferModes[] = {
      "Bounded ratio", "Consistent (curve-aware)"};
  ChoiceSetting(
      text, kSetTransferMode, "Neural transfer", kTransferModes,
      "How the model's edit is carried back to HDR on the Display codec."
      "  Bounded ratio applies the proxy-domain change as-is, which"
      " under-transfers brightness edits where the proxy curve bends."
      "  Consistent also undoes the curve's gain change, damped where the"
      " curve is too flat to invert.  The model sees the same input either"
      " way.");
  ToggleSetting(
      text, kSetNeuralFloorGuard,
      text.Pick("Neural-floor chroma guard", "Near-black colour guard"),
      "Divisor-codec HDR (Classic, and PQ or other absolute sources in"
      " Auto): where the model returns a near-black value under a bright"
      " original, keep the original's colour at the new brightness instead"
      " of amplifying the model's noise into green or grey specks.  Off is"
      " the previous math.");

  // The feed: where a relative HDR source's NR input scale comes from.
  constexpr const char* kFeedModes[] = {
      "Auto (recommended)", "v1 (frame meter)", "v2 (game exposure)"};
  ChoiceSetting(
      text, kSetFeedMode, text.Pick("NR feed", "Brightness source"), kFeedModes,
      "Where the brightness NR is fed at comes from on relative HDR"
      " sources.  v2 follows the game's own exposure (its DLSS exposure"
      " texture, or a pre-exposed buffer as it is), so NR's input holds"
      " still while the game's eye adaptation swings.  v1 meters each"
      " frame's content and adapts at its own rates, which can pulse"
      " slowly against the game's adaptation.  Auto uses v2 when the game"
      " provides an exposure texture and v1 otherwise: a pre-exposed buffer"
      " fed as it is lets a bright scene overload NR's input, and NR boils."
      "  Calibrated/absolute sources ignore this.");

  // Normalization governor.  Only relative (GPU-autoscale) sources have a
  // per-frame candidate to govern; calibrated sources already run a fixed
  // divisor, so the controls say so rather than pretending to apply.
  constexpr const char* kGovernorModes[] = {
      "Off (raw per-frame)", "Slew (v6.1.2)", "Stable (recommended)"};
  ChoiceSetting(
      text, kSetNormGovernor, text.Pick("Normalization Governor", "Normalization"),
      kGovernorModes,
      "How the same-frame autoscale candidate becomes the applied divisor."
      "  Stable uses a continuous estimate, settles at fixed rates per"
      " second (fast when brightening toward clipping, slow when"
      " relaxing) and holds still inside a small band, so neither a"
      " flickering scene statistic nor a settings edit can pump the"
      " image.  Slew is the v6.1.2 per-frame limit; Off applies the raw"
      " candidate (v6.1.0).  Calibrated/absolute sources ignore all.");
  if (norm_governor.load() >= 2u) {
    // One concept, two values (the U2 rename row): the modern layout names
    // the concept once and the direction per row.
    if (text.modern) ui::SubCaption(ui::text::kAdaptationSpeed);
    SliderSetting(
        text, kSetNormAttack, text.governor_attack, 0.1f, 64.f, "%.2f",
        "How fast the divisor may rise when the frame gets brighter"
        " (toward the proxy's clipping shoulder).");
    SliderSetting(
        text, kSetNormRelease, text.governor_release, 0.05f, 16.f, "%.2f",
        "How fast the divisor may relax when the frame gets darker.  Slow"
        " by design: a darker proxy is harmless, a pumping one is not.");
  } else if (norm_governor.load() != 0u) {
    SliderSetting(
        text, kSetNormSlew, text.Pick("Governor slew (stops/frame)", "Slew (stops/frame)"),
        0.f, 0.5f, "%.3f",
        "Largest step the committed divisor may take in one frame."
        "  0.020 is ~1.2 stops/s at 60 Hz: faster than engine eye"
        " adaptation, far slower than the frame-to-frame oscillation it"
        " damps.  Scene cuts, config changes and feature rebuilds snap"
        " instead of slewing, so raising this only affects tracking lag."
        "  0 behaves as Off.");
  }
}

inline void DrawSectionGuideOverrides(const ui::PanelText& text) {
  if (text.guide_overrides_note != nullptr) {
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextUnformatted(text.guide_overrides_note);
    ImGui::PopTextWrapPos();
  }
  constexpr const char* kDepthModes[] = {
      "Use game NGX flag", "Force normal depth", "Force inverted depth"};
  ChoiceSetting(
      text, kSetDepthMode, text.Pick("Depth Convention", "Depth"), kDepthModes,
      text.modern ? "Which way the game's depth runs.  The game's own NGX flag is"
                    " right unless the game declares it wrongly."
                  : nullptr);
  constexpr const char* kMotionTip =
      "Scales the game's motion vectors before the model sees them.  1.000 is"
      " right unless the game reports them at another scale; a negative value"
      " flips an axis.";
  SliderSetting(
      text, kSetMvecScaleX, text.Pick("Motion Scale X Multiplier", "Motion scale X"),
      -2.f, 2.f, "%.3f", text.modern ? kMotionTip : nullptr);
  SliderSetting(
      text, kSetMvecScaleY, text.Pick("Motion Scale Y Multiplier", "Motion scale Y"),
      -2.f, 2.f, "%.3f", text.modern ? kMotionTip : nullptr);
  // A change rebuilds every pass's history so the toggle applies from clean
  // temporal state instead of mixing chained and stateless frames.
  ToggleSetting(
      text, kSetChainedHistory, text.Pick("Chained temporal history", "Chained history"),
      "ON (default): every stacked pass keeps its own temporal history"
      " (reset only on scene cuts/contract changes). OFF: legacy diagnostic"
      " mode - passes 2+ are stateless (Reset every frame).  NVIDIA documents"
      " Reset-per-frame as a flicker/aliasing risk; only turn chained OFF to"
      " A/B a suspected ghosting compounding.");
}

// i18n: end
// The alpha44 counters, verbatim: the classic Guides & Diagnostics body and
// the modern Diagnostics' Raw details.
// i18n: begin - eligible decline explanations
inline constexpr const char* kSkipReasonText[] = {
    nullptr,
    "The game's DLSS call lacked its output, depth or motion",
    "The game's DLSS output has a size or layout NR cannot use",
    "The game's image before upscaling has a size or layout NR cannot use",
    "Streamline did not hand over every image NR needs",
    "Streamline's images were in a state NR cannot use",
    "The NR runtime was not ready for Streamline",
    "The frame was already enhanced (the game ran DLSS twice)",
    "The game's GPU state could not be restored safely",
    "The game's GPU state could not be restored safely",
    "NR could not set up its working images",
    "Warm-up after a start or a settings change",
    "Strength is 0 in every pass",
    "Waiting to retry after a failure",
    nullptr,
    "The NR runtime is not available",
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    "The Direct3D 11 bridge was not running",
    "This frame's images cannot cross the Direct3D 11 bridge",
    "The Direct3D 11 bridge was still busy on the GPU",
    "A Direct3D 11 bridge frame stalled and was released",
    "The Direct3D 11 bridge was lost",
    "The game ran DLSS on a deferred Direct3D 11 context",
    nullptr,
    "Waiting for a closed window's GPU work to finish",
};
// i18n: end
static_assert(std::size(kSkipReasonText) == static_cast<size_t>(NrDeclineReason::kCount));

inline void DrawSkippedReasons(bool player) {
  size_t order[static_cast<size_t>(NrDeclineReason::kCount)] = {};
  uint64_t counts[std::size(order)] = {};
  size_t listed = 0;
  for (size_t i = 0; i < std::size(order); ++i) {
    if (kSkipReasonText[i] == nullptr) continue;
    counts[i] = nr_decline_counts[i].load(std::memory_order_relaxed);
    if (counts[i] != 0) order[listed++] = i;
  }
  std::sort(order, order + listed, [&counts](size_t a, size_t b) { return counts[a] > counts[b]; });
  ImGui::BeginGroup();
  if (listed == 0) ImGui::TextUnformatted(player ? ui::Tr(ui::text::kNoneYet) : "none");
  for (size_t rank = 0; rank < (std::min)(listed, size_t{3}); ++rank) {
    const size_t reason = order[rank];
    const auto count = static_cast<unsigned long long>(counts[reason]);
    if (player) {
      ImGui::TextWrapped("%llu | %s", count, ui::Tr(kSkipReasonText[reason]));
    } else {
      ImGui::TextWrapped("%llu | %s | %s", count, kNrDeclineTags[reason],
                         kNrDeclineNames[reason]);
    }
  }
  ImGui::EndGroup();
}

inline void DrawSectionDiagnosticsRaw() {
  ImGui::TextUnformatted("Skipped because:");
  DrawSkippedReasons(false);
  // Stacking spends N neural evaluates on the base rendered frame; with
  // DLSSG active that competes directly with the frame-generation budget
  // (field reports: MFG drops from 3x/4x to 2x, worst case device hangs).
  {
    bool dlssg_present = false;
    {
      RuntimeLock lock(runtime_mutex);
      for (const auto& [_, created_id] : created_feature_ids) {
        if (created_id == NVSDK_NGX_Feature_FrameGeneration) {
          dlssg_present = true;
          break;
        }
      }
    }
    if (dlssg_present && stack_passes.load() > 1) {
      ImGui::TextColored(
          ImVec4(1.f, 0.8f, 0.2f, 1.f),
          "WARNING: Frame generation detected with NR stacking enabled. Each extra"
          " stack pass costs a full neural evaluate on the base frame and can push"
          " the game out of its frame-generation budget (MFG drops or hangs)."
          " Consider fewer passes or a lower NR resolution.");
    }
  }

  uint32_t workset_count = 0;
  {
    RuntimeLock lock(runtime_mutex);
    workset_count = static_cast<uint32_t>(worksets.size());
  }
  const float res_scale = nr_resolution_scale.load();
  const std::string res_text = nr_follow_input_res.load()
      ? "input res"
      : (res_scale >= 0.995f
             ? "native (100%)"
             : std::to_string(static_cast<int>(res_scale * 100.f + 0.5f)) + "%");
  if (nr_before_upscale.load() && logged_pre_sr_rr.load()) {
    ImGui::TextUnformatted(
        "Active path: after - pre-SR is selected, but the game upscales with"
        " Ray Reconstruction, whose input is still-noisy lighting; NR runs on"
        " RR's finished output instead.  UI remains downstream.");
  } else if (nr_before_upscale.load()) {
    ImGui::TextUnformatted(
        "Active path: pre-SR - before the game's NGX DLSS evaluate; NR runs"
        " at the configured working resolution on the game's own depth/motion"
        " guides and the game's DLSS upscales the enhanced color.  UI remains"
        " downstream.");
  } else {
    ImGui::TextUnformatted(
        "Active path: after - immediately following the game's NGX DLSS"
        " output; UI remains downstream.");
  }
  ImGui::Text("Working resolution: %s", res_text.c_str());
  ImGui::Text("Stack passes: %u", stack_passes.load());
  ImGui::Text("NR worksets: %u", workset_count);
  ImGui::Text(
      "Successful NR frames: %llu",
      static_cast<unsigned long long>(successful_evaluations.load()
          + successful_pre_sr_evaluations.load()));
  ImGui::Text(
      "Bypassed NR frames: %llu",
      static_cast<unsigned long long>(bypassed_evaluations.load()));
  ImGui::Text(
      "Last NR input: %ux%u", last_input_width.load(), last_input_height.load());
  ImGui::Text(
      "Last NR output: %ux%u", last_output_width.load(), last_output_height.load());
  const uint32_t result = last_result.load();
  if (result == NVSDK_NGX_Result_Success) {
    ImGui::Text("Latest NR NGX result: 0x%08X (ok)", result);
  } else if (result == NVSDK_NGX_Result_FAIL_InvalidParameter) {
    ImGui::Text(
        "Latest NR NGX result: 0x%08X (the NR contract was rejected by this"
        " runtime build)",
        result);
  } else {
    ImGui::Text(
        "Latest NR NGX result: 0x%08X (NGX failure; ReShade.log names the"
        " failing step and the fix)",
        result);
  }
  ImGui::Text(
      "Backend: %s",
      parameter_runtime_via_core.load() ? "NGX core" : "signed runtime");
  if (!direct_runtime_sha256.empty()) {
    ImGui::Text(
        "Runtime sha256: %s%s",
        direct_runtime_sha256.c_str(),
        direct_runtime_reference_match ? " (reference match)" : " (custom build)");
  }
  int ngx_hooked_count = 0;
  for (int s = 0; s < kMaxNgxSlots; ++s) if (ngx_slot_used[s]) ++ngx_hooked_count;
  ImGui::Text("NGX modules detoured: %d", ngx_hooked_count);
  ImGui::Text("NGX core present: %s", hooked_ngx_module != nullptr ? "yes" : "no");
  ImGui::Text(
      "NGX hooks - creates: %llu",
      static_cast<unsigned long long>(intercepted_creates.load()));
  ImGui::Text(
      "NGX hooks - evaluations: %llu",
      static_cast<unsigned long long>(intercepted_evaluations.load()));
  ImGui::Text(
      "Streamline - DLSS/DLSSD evaluations: %llu",
      static_cast<unsigned long long>(intercepted_streamline_dlss_evaluations.load()));
  ImGui::Text(
      "Streamline - all evaluations: %llu",
      static_cast<unsigned long long>(intercepted_streamline_evaluations.load()));
  ImGui::Text(
      "Streamline - tag calls: %llu",
      static_cast<unsigned long long>(intercepted_streamline_tag_calls.load()));
  ImGui::Text(
      "Streamline - largest tag batch: %u", streamline_tag_max_batch.load());
  ImGui::Text(
      "Streamline - frame mask: 0x%02X", streamline_tag_mask_this_present.load());
  ImGui::Text(
      "Streamline - direct fallback attempts: %llu",
      static_cast<unsigned long long>(streamline_direct_fallback_attempts.load()));
  ImGui::Text(
      "Streamline - direct fallback successes: %llu",
      static_cast<unsigned long long>(streamline_direct_fallback_successes.load()));
  ImGui::TextUnformatted(
      "Codec: linear working space; commit re-encodes to the source format.");
  if (codec_mode.load() == 1u) {
    ImGui::Text(
        "Codec mode: anchored (%.1f-nit shoulder); pedestal capped at"
        " commit.",
        proxy_anchor_nits.load());
  } else if (codec_mode.load() == 2u) {
    ImGui::TextUnformatted(
        "Codec mode: auto - calibrated sources use fixed units; relative"
        " HDR uses same-frame GPU autoscale.");
  } else {
    ImGui::TextUnformatted(
        "Codec mode: classic paper-white gain.");
  }

  bool ui_gpu_timers = gpu_timers_enabled.load();
  if (ImGui::Checkbox("GPU stage timers (profiling)", &ui_gpu_timers)) {
    kSetGpuTimers.Commit(ui_gpu_timers ? 1.f : 0.f);
  }
  bool ui_edit_trace = edit_trace_enabled.load();
  if (ImGui::Checkbox("NR edit trace (observe-only)", &ui_edit_trace)) {
    kSetEditTrace.Commit(ui_edit_trace ? 1.f : 0.f);
  }
  ui::ItemTooltip(
      "Measures what Neural Rendering changes each frame, Look controls on or"
      " off, and writes a line a second to ReShade.log and"
      " RenoDX-DLSS5-edittrace.csv.  It changes nothing on screen.");
  {
    RuntimeLock lock(runtime_mutex);
    if (gpu_timers::stats.valid) {
      const gpu_timers::Stats& t = gpu_timers::stats;
      ImGui::Text(
          "GPU stage timings [%s %ux%u]: total %.2fms, model %.2fms, own"
          " overhead %.2fms",
          t.path, t.width, t.height, t.total_us / 1000.f,
          t.eval_us / 1000.f, t.own_us / 1000.f);
      ImGui::Text(
          "  encode %.3f | resolve %.3f | linearize+scale %.3f | commit"
          " %.3f | copies+park %.3f (ms)",
          t.encode_us / 1000.f, t.resolve_us / 1000.f,
          t.linearize_us / 1000.f, t.commit_us / 1000.f,
          t.copies_us / 1000.f);
    }
  }

  if (ImGui::Button("Reset NR feature and clear failure latch")) {
    RecreateFeatures();
  }
}

// i18n: begin - display strings (tools/i18n/ui_strings.py catalog)
// Config breaker (two-step confirm): every setting back to the built-in
// defaults, including hotkeys; NR feature/workset state is recreated.
inline void DrawSectionRestoreDefaults(reshade::api::effect_runtime* runtime,
                                       const ui::PanelText& text) {
  static bool confirm_restore_defaults = false;
  if (!confirm_restore_defaults) {
    if (ImGui::Button(text.Tr(text.Pick("Restore all settings to defaults", ui::text::kRestoreAll)))) {
      confirm_restore_defaults = true;
    }
  } else {
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextUnformatted(text.Tr(text.Pick(
        "Restore erases every setting for this game - strengths, codec,"
        " resolution, passes, and hotkeys.",
        ui::text::kRestoreConfirm)));
    ImGui::PopTextWrapPos();
    if (ImGui::Button(text.Tr(text.Pick("Confirm: restore defaults", "Restore")))) {
      ResetToDefaults(runtime);
      confirm_restore_defaults = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(text.Tr("Cancel"))) {
      confirm_restore_defaults = false;
    }
  }
}

// The hotkeys are edge-polled once per frame: by RunLifecycleTick (present,
// or NGX evaluate while presents starve) during gameplay, and by
// OnOverlayHotkeys while the overlay is open; the buttons below are the
// discoverable paths.
inline void DrawSectionHotkeys(const ui::PanelText& text) {
  // Click a key button, then press the desired key to rebind it. The capture
  // scans ImGui's key state here, not GetAsyncKeyState: while the overlay is
  // open ReShade can block game input (InputProcessing=2), which hides key
  // presses from async key state, while ImGui receives every key through
  // ReShade's own input pipeline. Escape or a second click cancels; a key
  // already bound to the other action is skipped, and keys without a virtual
  // key (mouse, gamepad) cannot bind.
  const auto hotkey_button = [&text](const char* label, const Setting& setting,
                                     const Setting& other, uint32_t target) {
    const bool capturing =
        hotkey_capture_target.load(std::memory_order_relaxed) == target;
    const std::string key = VirtualKeyName(static_cast<uint32_t>(setting.Get()));
    if (text.modern) {
      ui::RowLabel(label, ui::text::kRebindTip);
      const std::string shown =
          (capturing ? std::string(ui::Tr("Press a key (Esc cancels)")) : key) + "###"
          + setting.key;
      if (ImGui::Button(shown.c_str(), ImVec2(ImGui::CalcItemWidth(), 0.f))) {
        hotkey_capture_target = capturing ? 0 : target;
      }
      ui::ItemTooltip("%s", ui::Tr(ui::text::kRebindTip));
      // A reset never binds one key to both actions.
      if (!capturing && setting.Modified() && other.Get() != setting.fallback
          && ui::ResetButton(
              setting.key, VirtualKeyName(static_cast<uint32_t>(setting.fallback)).c_str())) {
        setting.Commit(setting.fallback);
      }
      return;
    }
    if (capturing) {
      if (ImGui::Button(
              (std::string(label) + ": press a key... (Esc or click to cancel)")  // i18n: skip (Developer view)
                  .c_str())) {
        hotkey_capture_target = 0;
      }
      return;
    }
    if (ImGui::Button((std::string(label) + ": " + key).c_str())) {
      hotkey_capture_target = target;
    }
  };
  hotkey_button(text.Pick("NR Toggle Key", "Toggle NR"), kSetToggleKey, kSetScreenshotKey, 1);
  hotkey_button(text.Pick("Screenshot Key", "Screenshot"), kSetScreenshotKey, kSetToggleKey,
                2);
  const uint32_t capture_target = hotkey_capture_target.load(std::memory_order_relaxed);
  if (capture_target != 0) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      hotkey_capture_target = 0;
    } else {
      const uint32_t other = capture_target == 1
          ? screenshot_hotkey.load(std::memory_order_relaxed)
          : toggle_hotkey.load(std::memory_order_relaxed);
      for (int key_index = ImGuiKey_NamedKey_BEGIN; key_index < ImGuiKey_NamedKey_END;
           ++key_index) {
        const ImGuiKey key = static_cast<ImGuiKey>(key_index);
        if (!ImGui::IsKeyPressed(key, false)) continue;
        const uint32_t vk = VkFromImGuiKey(key);
        if (vk == 0 || vk == other) continue;
        if (capture_target == 1) {
          toggle_hotkey = vk;
          // The rebind keystroke is still physically held; prime the edge
          // latch so the new binding does not fire itself immediately.
          toggle_hotkey_was_down.store(true, std::memory_order_relaxed);
        } else {
          screenshot_hotkey = vk;
          screenshot::internal::hotkey_was_down.store(
              true, std::memory_order_relaxed);
        }
        reshade::set_config_value(
            nullptr, kConfigSection,
            capture_target == 1 ? "NRToggleKey" : "NRScreenshotKey", vk);
        Log(
            reshade::log::level::info,
            std::string("hotkey rebound: ")
                + (capture_target == 1 ? "NR toggle" : "screenshot") + " = "
                + VirtualKeyName(vk));
        hotkey_capture_target = 0;
        break;
      }
    }
  }

  constexpr const char* kFileFormats[] = {"PNG", "JPEG", "PNG + JPEG"};
  ChoiceSetting(text, kSetScreenshotFormat, "File format", kFileFormats,
                "PNG is lossless. JPEG makes smaller files at full resolution. HDR uses "
                "the same SDR preview as rc10.");
  constexpr const char* kLayouts[] = {"Pair", "Pair + side by side"};
  ChoiceSetting(text, kSetScreenshotLayout, "Comparison layout", kLayouts,
                "Side by side adds one file showing NR off (left) and NR on (right), "
                "for a single upload.");
  if (screenshot::file_format.load() != static_cast<uint32_t>(screenshot::FileFormat::kPng)) {
    SliderSetting(text, kSetScreenshotJpegQuality, "JPEG quality", 80.f, 100.f, "%.0f",
                  "95 is visually lossless. If a file is over the size limit, the quality steps "
                  "down (not below 85); the image is never scaled down.");
    SliderSetting(text, kSetScreenshotMaxMB, "JPEG size limit (MB)", 0.f, 50.f, "%.0f",
                  "10 is Discord's upload limit without Nitro. 0 turns the limit off.");
  }
  const std::string capture_key = VirtualKeyName(screenshot_hotkey.load());
  const char* const status = screenshot::HasPending()
      ? "waiting for GPU completion"
      : screenshot::IsArmed() ? "armed, waiting for a successful evaluation"
                              : "idle";
  if (text.modern) {
    ui::RowLabel("Capture");
    if (ImGui::Button(ui::Tr("Take a screenshot now"), ImVec2(ImGui::CalcItemWidth(), 0.f))) {
      screenshot::RequestCapture();
    }
    ui::RowLabel("Screenshot status");
    ImGui::TextWrapped("%s", ui::Tr(status));
    return;
  }
  if (ImGui::Button(("Capture Screenshot (" + capture_key + ")").c_str())) {  // i18n: skip (Developer view)
    screenshot::RequestCapture();
  }
  ImGui::Text("Screenshot (%s): %s", capture_key.c_str(), status);  // i18n: skip (Developer view)
}

// Developer view: the classic layout, persisted as UiMode.  Drawn at the
// foot of both layouts, so either can reach the other.  The classic layout
// has no Defaults view, so a switch ends it and the player's values return.
inline void DrawSectionLayoutToggle() {
  bool classic = ui_classic_layout.load();
  if (ImGui::Checkbox(
          (std::string(ui::Tr(ui::text::kDeveloperView)) + "###developer_view").c_str(),
          &classic)) {
    SetDefaultsView(false);
    ui_classic_layout = classic;
    reshade::set_config_value(
        nullptr, kConfigSection, "UiMode", classic ? "classic" : "modern");
  }
  ui::ItemTooltip("%s", ui::Tr(ui::text::kDeveloperViewTip));
}

// The panel's language (UiLanguage): Auto, or one of ui::kLanguages.  The
// choices are drawn in English, never translated: every ReShade font draws
// Latin, so a language picked by mistake, in a script ReShade's font lacks,
// still leaves the way back readable.  Drawn in both layouts.
inline void DrawSectionLanguage(const ui::PanelText& text) {
  const int choice = ui_language_choice.load(std::memory_order_relaxed);
  const auto name = [](int item) {
    return item == 0 ? ui::text::kLanguageAuto : ui::kLanguages[item - 1].name;
  };
  if (text.modern) ui::RowLabel(ui::text::kLanguage, ui::text::kLanguageTip);
  if (ImGui::BeginCombo(text.modern ? "##ui_language" : ui::text::kLanguage, name(choice))) {
    for (int item = 0; item <= ui::kLanguageCount; ++item) {
      if (ImGui::Selectable(name(item), item == choice) && item != choice) {
        StoreUiLanguage(item);
      }
    }
    ImGui::EndCombo();
  }
  ui::ItemTooltip("%s", text.Tr(ui::text::kLanguageTip));
  if (text.modern && choice != 0
      && ui::ResetButton("ui_language_reset", ui::text::kLanguageAuto)) {
    StoreUiLanguage(0);
  }
}

// A header the card's "Open Diagnostics" button opens and scrolls to: the
// classic layout's "Guides & Diagnostics", the modern Diagnostics section.
inline bool DiagnosticsHeader(const ui::PanelText& text) {
  if (ui_open_diagnostics) ImGui::SetNextItemOpen(true);
  bool reset = false;
  const bool open =
      text.modern
          ? ui::SectionHeader(ui::text::kDiagnostics, ui::text::kDiagnosticsHint,
                              ui::text::kResetDiagnostics,
                              AnyModified({kDiagnosticsSettings}), &reset)
          : ImGui::CollapsingHeader("Guides & Diagnostics", ImGuiTreeNodeFlags_DefaultOpen);  // i18n: skip (Developer view)
  if (reset) ResetSettings({kDiagnosticsSettings});
  if (open && ui_open_diagnostics) {
    ImGui::SetScrollHereY(0.f);
    ui_open_diagnostics = false;
  }
  return open;
}

// i18n: end
// ---- diagnostics and the support report (U3, PLAN_UI_V7.md 4 and 6) --------

// The game's executable FILE name, for the footer and the support report: a
// report never carries a folder path.
inline const std::string& GameExeName() {
  static const std::string name = [] {
    std::wstring exe(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (length == 0 || length >= exe.size()) return std::string("unknown");
    exe.resize(length);
    const std::wstring file = std::filesystem::path(exe).filename().wstring();
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, file.c_str(), static_cast<int>(file.size()), nullptr, 0,
        nullptr, nullptr);
    std::string utf8(static_cast<size_t>(std::max(bytes, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, file.c_str(), static_cast<int>(file.size()),
                        utf8.data(), bytes, nullptr, nullptr);
    return utf8;
  }();
  return name;
}

// What a support thread asks a reporter for, in one paste: the version, the
// NR-VERDICT and NR-CARD lines exactly as the log states them (so
// log_verdict.py scores a report like a log), the card's title, the settings
// that change behaviour, the counters and the latest telemetry line.
// Redacted by construction - the game's file name, no path, no hardware id.
// Atomics and the telemetry line's own lock only: no runtime_mutex, no loader
// call, so the button is safe from any frame.
inline const char* NrRuntimeVersionText() {
  const auto& loader = NgxLoaderView();
  return loader.nr_file_version.empty() ? "not loaded" : loader.nr_file_version.c_str();
}

inline std::string BuildSupportReport() {
  const NrVerdict verdict = ComputeNrVerdict(CollectFunnelInputs());
  char verdict_line[512];
  FormatNrVerdictLine(
      verdict_line, sizeof(verdict_line), verdict,
      submitted_injections.load(std::memory_order_relaxed),
      presents_observed.load(std::memory_order_relaxed), kAddonVersion,
      "report");
  const ui::StatusCard card =
      ui::ComputeStatusCard(CollectCardInputs(verdict, SteadyNowNs()));
  char card_line[128];
  ui::FormatCardLine(card_line, sizeof(card_line), card, "report");
  std::string telemetry;
  {
    std::lock_guard<std::mutex> lock(last_telemetry_mutex);
    telemetry = last_telemetry_line;
  }
  const uint32_t source = dx11_source.load();
  char result[16];
  std::snprintf(result, sizeof(result), "0x%08X", last_result.load());
  std::ostringstream report;
  report << "RenoDX DLSS5 Generic support report v1\n"
         << "version: " << kAddonVersion << "\n"
         << "game: " << GameExeName() << "\n"
         << "card: " << card.title << "\n"
         << verdict_line << "\n"
         << card_line << "\n"
         << "hint: " << dlss_user_hint.load() << "\n"
         << "runtime: "
         << (direct_runtime_sha256.empty()
                 ? std::string("not loaded")
                 : "sha256 " + direct_runtime_sha256
                       + (direct_runtime_reference_match ? " (reference match)"
                                                         : " (custom build)"))
         << "\n"
         << "settings: NeuralUplift=" << enabled.load()
         << " NRIntensity=" << intensity.load()
         << " NRPasses=" << stack_passes.load()
         << " NRPreUpscale=" << nr_before_upscale.load()
         << " NRCodecMode=" << codec_mode.load()
         << " NRFollowInputRes=" << nr_follow_input_res.load()
         << " NRResolutionScale=" << nr_resolution_scale.load()
         << " NRFoveation=" << nr_foveation.load()
         << " NRFoveationWidth=" << nr_foveation_width.load()
         << " NRFoveationHeight=" << nr_foveation_height.load()
         << " NRFoveationFalloff=" << nr_foveation_falloff.load()
         << " NRFoveationEdge=" << nr_foveation_edge.load()
         << " EnableHooks="
         << (!hooks_enabled.load() ? 0 : streamline_hooks_enabled.load() ? 1 : 2)
         << " DX11Source="
         << (source == kDx11SourceForeign  ? "foreign"
             : source == kDx11SourceNative ? "native"
             : source == kDx11SourceOff    ? "off"
                                           : "auto")
         << " UiMode=" << (ui_classic_layout.load() ? "classic" : "modern")
         << " UiLanguage="
         << (ui_language_choice.load() == 0
                 ? "auto"
                 : ui::kLanguages[ui_language_choice.load() - 1].code)
         << "\n"
         << "counters: enhanced="
         << successful_evaluations.load() + successful_pre_sr_evaluations.load()
         << " bypassed=" << bypassed_evaluations.load()
         << " creates=" << intercepted_creates.load()
         << " evaluations=" << intercepted_evaluations.load()
         << " last_result=" << result << " input=" << last_input_width.load()
         << "x" << last_input_height.load() << " output="
         << last_output_width.load() << "x" << last_output_height.load() << "\n";
  if (!telemetry.empty()) report << "telemetry: " << telemetry << "\n";
  return report.str();
}

// Open log folder: ShellExecute loads shell extensions into the game, so it
// runs on a thread of its own - never under runtime_mutex, never inside the
// overlay's present.  The thread holds a reference on this module and ends
// through FreeLibraryAndExitThread, so an unload cannot pull the code out
// from under it.  ReShade writes ReShade.log beside ReShade.ini, the folder
// AddonDirectory() names (BackupConfigFile relies on the same).
inline DWORD WINAPI OpenLogFolderMain(void* module) {
  const HRESULT com =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  ShellExecuteW(nullptr, L"open", AddonDirectory().c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
  if (SUCCEEDED(com)) CoUninitialize();
  FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

// i18n: begin - display strings (tools/i18n/ui_strings.py catalog)
// Diagnostics, translated (U3): what the counters mean, one row each, then
// the report buttons; the alpha44 counters stay one click down.
inline void DrawDiagnostics(const NrVerdict& verdict) {
  const uint32_t result = last_result.load();
  float gpu_ms = 0.f;
  {
    RuntimeLock lock(runtime_mutex);
    if (gpu_timers::stats.valid) gpu_ms = gpu_timers::stats.total_us / 1000.f;
  }
  ui::RowLabel(ui::text::kLastFrame);
  if (result == NVSDK_NGX_Result_Success) {
    if (gpu_ms > 0.f) {
      ImGui::Text(ui::Tr("ok | %.1f ms GPU"), gpu_ms);
    } else {
      ImGui::TextUnformatted(ui::Tr("ok"));
    }
  } else if (result == NVSDK_NGX_Result_FAIL_NotInitialized) {
    ImGui::TextUnformatted(ui::Tr(ui::text::kNoneYet));
  } else {
    ImGui::TextColored(ui::tokens::kBad, ui::Tr("failed (0x%08X)"), result);
  }
  // The values wrap inside the panel: a narrow panel ends them at its edge.
  ui::RowLabel(ui::text::kFramesThisSession);
  ImGui::TextWrapped(
      ui::Tr("%llu enhanced | %llu bypassed"),
      static_cast<unsigned long long>(successful_evaluations.load()
                                      + successful_pre_sr_evaluations.load()),
      static_cast<unsigned long long>(bypassed_evaluations.load()));
  ui::RowLabel(ui::text::kSkippedBecause, ui::text::kSkippedBecauseTip);
  DrawSkippedReasons(true);
  ui::RowLabel(ui::text::kWorkingResolution);
  if (last_input_width.load() != 0) {
    ImGui::TextWrapped("%ux%u -> %ux%u", last_input_width.load(), last_input_height.load(),
                       last_output_width.load(), last_output_height.load());
  } else {
    ImGui::TextUnformatted(ui::Tr(ui::text::kNoneYet));
  }
  ui::RowLabel(ui::text::kBackend);
  const bool bridge = std::strcmp(verdict.path, "bridge") == 0;
  const bool inline_path = std::strcmp(verdict.path, "inline") == 0;
  ImGui::TextWrapped(
      "%s | %s", ui::Tr(parameter_runtime_via_core.load() ? "NGX core" : "signed runtime"),
      ui::Tr(bridge ? (nr_before_upscale.load() ? "Direct3D 11 bridge, before upscaling"
                                                : "Direct3D 11 bridge, after upscaling")
             : inline_path ? (nr_before_upscale.load() && !logged_pre_sr_rr.load()
                                  ? "Direct3D 12, before upscaling"
                                  : "Direct3D 12, after upscaling")
             : std::strcmp(verdict.path, "foreign") == 0
                 ? "Direct3D 12, from a DX11 bridge tool"
                 : "not running"));

  static int64_t copied_ns = 0;
  if (ImGui::Button(ui::Tr(ui::text::kCopySupportReport))) {
    ImGui::SetClipboardText(BuildSupportReport().c_str());
    copied_ns = SteadyNowNs();
  }
  ui::ItemTooltip("%s", ui::Tr(ui::text::kReportTip));
  ui::SameLineIfFits(ImGui::CalcTextSize(ui::Tr(ui::text::kOpenLogFolder)).x
                     + ImGui::GetStyle().FramePadding.x * 2.f);
  if (ImGui::Button(ui::Tr(ui::text::kOpenLogFolder))) {
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&OpenLogFolderMain), &self)) {
      HANDLE thread = CreateThread(nullptr, 0, OpenLogFolderMain, self, 0, nullptr);
      if (thread != nullptr) {
        CloseHandle(thread);
      } else {
        FreeLibrary(self);
      }
    }
  }
  if (copied_ns != 0 && SteadyNowNs() - copied_ns < 4'000'000'000LL) {
    ImGui::TextColored(ui::tokens::kGood, "%s", ui::Tr(ui::text::kReportCopied));
  }
  // Raw details stays English: it is what a support thread reads.
  if (AnyModified({kDiagnosticsSettings})) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
  if (ImGui::TreeNodeEx("raw_details", 0, "%s", ui::Tr(ui::text::kRawDetails))) {
    DrawSectionDiagnosticsRaw();
    ImGui::TreePop();
  }
}

// The modern Resolution row: presets over the same two keys the classic
// mode combo writes (NRFollowInputRes, NRResolutionScale).  "Match game"
// leaves the stored scale alone, as the combo's follow mode does.  A stored
// pair no preset names reads as Custom.
struct ResolutionPreset {
  bool follow;
  float scale;
  int mode;  // the classic combo index CommitResolutionMode logs
};
inline constexpr ResolutionPreset kResolutionPresets[] = {
    {false, 1.00f, 0}, {false, 0.67f, 2}, {false, 0.58f, 2},
    {false, 0.50f, 2}, {true, 1.00f, 1}};
static_assert(std::size(kResolutionPresets) == std::size(ui::text::kResolutionLabels));

// i18n: end
#include "report_flow.hpp"

// The alpha44 layout, verbatim: Developer view (UiMode=classic).
inline void DrawClassicLayout(reshade::api::effect_runtime* runtime) {
  const ui::PanelText& text = ui::kClassicText;
  // Sections default to open.
  static constexpr ImGuiTreeNodeFlags kSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
  if (ImGui::CollapsingHeader("Neural Rendering", kSectionFlags)) {
    DrawSectionNrCore(text);
    if (ImGui::CollapsingHeader("Model styling (advanced)")) {
      DrawSectionModelStyling(text);
    }
    if (ImGui::CollapsingHeader("Look result (advanced)")) {
      DrawSectionLookResult(text);
    }
  }
  if (ImGui::CollapsingHeader("Stacking & Resolution", kSectionFlags)) {
    DrawSectionStackingResolution(text);
  }
  if (ImGui::CollapsingHeader("HDR Colour Bridge", kSectionFlags)) {
    DrawSectionCodecCombo(text);
    DrawSectionHdrDetails(text);
  }
  if (DiagnosticsHeader(text)) {
    DrawSectionGuideOverrides(text);
    ImGui::Separator();
    DrawSectionDiagnosticsRaw();
    DrawSectionRestoreDefaults(runtime, text);
  }
  if (ImGui::CollapsingHeader("Hotkeys", kSectionFlags)) {
    DrawSectionHotkeys(text);
  }
  ImGui::Separator();
  DrawSectionLanguage(text);
  DrawSectionLayoutToggle();
}

// i18n: begin - display strings (tools/i18n/ui_strings.py catalog)
// The default layout (PLAN_UI_V7.md 2, reworked in alpha46 by owner
// direction): the card answers "is it working?"; the switch, the Defaults
// view, Strength and Resolution follow; the Look - the settings players tune
// most - is open below them; Performance, Hotkeys, Diagnostics and Fixes
// (HDR and guides: "only change if you know what you are doing") are
// collapsed, one click away, never gone.  Every row is label, control, reset
// button; every section with a changed setting offers its own reset.
inline void DrawModernLayout(reshade::api::effect_runtime* runtime,
                             const NrVerdict& verdict) {
  const ui::PanelText& text = ui::kModernText;
  // U5: under the status card until dismissed; the Developer view has none.
  if (!ui_welcomed.load()) {
    ImGui::PushID("welcome");  // DrawStatusCard's child id is fixed
    if (ui::DrawStatusCard({.tone = ui::CardTone::kIdle,
                            .glyph = ui::CardGlyph::kInfo,
                            .title = ui::text::kWelcomeTitle,
                            .body = ui::text::kWelcomeBody,
                            .fix = {ui::text::kWelcomeSteps[0],
                                    ui::text::kWelcomeSteps[1],
                                    ui::text::kWelcomeSteps[2]},
                            .fix_count = std::size(ui::text::kWelcomeSteps),
                            .action = ui::CardAction::kDismiss})) {
      ui_welcomed = true;
      reshade::set_config_value(nullptr, kConfigSection, "UiWelcomed", true);
    }
    ImGui::PopID();
    ui::SectionGap();
  }
  DrawSectionNrCore(text);

  const bool follow = nr_follow_input_res.load();
  const float scale = nr_resolution_scale.load();
  int preset = -1;
  for (int i = 0; i < static_cast<int>(std::size(kResolutionPresets)); ++i) {
    const ResolutionPreset& candidate = kResolutionPresets[i];
    if (candidate.follow ? follow
                         : !follow && std::abs(scale - candidate.scale) < 0.005f) {
      preset = i;
      break;
    }
  }
  ui::RowLabel(ui::text::kResolution, ui::text::kResolutionTip);
  const int clicked = ui::Segmented(
      "##resolution", ui::text::kResolutionLabels,
      static_cast<int>(std::size(ui::text::kResolutionLabels)), preset,
      ui::text::kResolutionTip);
  if (clicked >= 0 && clicked != preset) {
    const ResolutionPreset& chosen = kResolutionPresets[clicked];
    nr_follow_input_res = chosen.follow;
    if (!chosen.follow) nr_resolution_scale = chosen.scale;
    CommitResolutionMode(chosen.mode);
  }
  ResolutionResetButton("resolution_reset");
  if (preset < 0) {
    ui::RowLabel("");
    ImGui::TextDisabled(ui::Tr(ui::text::kCustomResolution), scale * 100.f);
  }

  // A section's reset link is hidden in the Defaults view: the look is
  // stock then, and the player's own values come back with Mine.
  if (ui::SectionCaption(
          ui::text::kLook, ui::text::kResetLook,
          !ui_defaults_view && AnyModified({kLookSettings, kLookResultSettings}))) {
    ResetSettings({kLookSettings, kLookResultSettings});
  }
  ImGui::BeginDisabled(ui_defaults_view);
  DrawSectionModelStyling(text);
  ui::SectionGap();
  DrawSectionLookResult(text);
  ImGui::EndDisabled();
  ui::SectionGap();

  bool reset = false;
  if (ui::SectionHeader(
          ui::text::kPerformance, ui::text::kPerformanceHint, ui::text::kResetPerformance,
          !ui_defaults_view && AnyModified({kPerformanceSettings, kStackingSettings}),
          &reset)) {
    DrawSectionStackingResolution(text);
  }
  if (reset) ResetSettings({kPerformanceSettings, kStackingSettings});

  char hotkeys[128];
  std::snprintf(hotkeys, sizeof(hotkeys), ui::Tr(ui::text::kHotkeysHint),
                VirtualKeyName(toggle_hotkey.load()).c_str(),
                VirtualKeyName(screenshot_hotkey.load()).c_str());
  if (ui::SectionHeader(ui::text::kHotkeys, hotkeys, ui::text::kResetHotkeys,
                        AnyModified({kHotkeySettings}), &reset)) {
    DrawSectionHotkeys(text);
  }
  if (reset) ResetSettings({kHotkeySettings});

  if (DiagnosticsHeader(text)) {
    DrawDiagnostics(verdict);
  }

  if (ui::SectionHeader(ui::text::kFixes, ui::text::kFixesHint, ui::text::kResetFixes,
                        AnyModified({kFixesSettings}), &reset)) {
    {
      ui::StyleScope warning;
      warning.Color(ImGuiCol_Text, ui::tokens::kWarn);
      ImGui::TextUnformatted(ui::icons::kWarning);
      ImGui::SameLine();
      ImGui::PushTextWrapPos(0.f);
      ImGui::TextUnformatted(ui::Tr(ui::text::kFixesWarning));
      ImGui::PopTextWrapPos();
    }
    ui::SubCaption(ui::text::kFixesHdr);
    DrawSectionCodecCombo(text);
    DrawSectionHdrDetails(text);
    ui::SubCaption(ui::text::kFixesGuides);
    DrawSectionGuideOverrides(text);
  }
  if (reset) ResetSettings({kFixesSettings});

  ui::SectionGap();
  DrawSectionLanguage(text);
  ImGui::Separator();
  {
    ui::StyleScope footer;
    footer.FontSize(ui::SmallFontSize())
        .Color(ImGuiCol_Text, ui::tokens::kTextSecondary);
    ImGui::PushTextWrapPos(0.f);
    ImGui::Text(ui::text::kFooter, kAddonVersion, NrRuntimeVersionText(), GameExeName().c_str());
    ImGui::PopTextWrapPos();
  }
  DrawSectionRestoreDefaults(runtime, text);
  ui::SameLineIfFits(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x
                     + ImGui::CalcTextSize(ui::Tr(ui::text::kDeveloperView)).x);
  DrawSectionLayoutToggle();
}

inline void DrawPanelBody(reshade::api::effect_runtime* runtime) {
  // The status card comes first (PLAN_UI_V7.md 3): the one question a
  // player opens this panel with - "is it working?" - is answered before any
  // control.  The card is the verdict's, never a second opinion (R1): through
  // alpha43 a ladder here printed funnel rungs, and v6.6.0 through v6.7.2
  // said ACTIVE while every evaluate declined.  ComputeStatusCard is the
  // same function the NR-CARD line states, and test/dlss5 pins every row.
  const NrVerdict verdict = ComputeNrVerdict(CollectFunnelInputs());
  const bool classic = ui_classic_layout.load();
  {
    const ui::StatusCard card =
        ui::ComputeStatusCard(CollectCardInputs(verdict, SteadyNowNs()));
    if (!ui_capture_dir.empty()) {
      ui_drawn_card.store(
          static_cast<uint16_t>((static_cast<unsigned>(card.tone) << 8)
                                | static_cast<unsigned>(card.id)),
          std::memory_order_relaxed);
    }
    if (ui::DrawStatusCard(card)) {
      if (card.action == ui::CardAction::kTurnOn) SetNrEnabledFromUi(true);
      if (card.action == ui::CardAction::kOpenDiagnostics) ui_open_diagnostics = true;
    }
    DrawIssueReport();
    if (classic) {
      ui::StyleScope footer;
      footer.FontSize(ui::SmallFontSize())
          .Color(ImGuiCol_Text, ui::tokens::kTextSecondary);
      ImGui::Text(ui::text::kFooter, kAddonVersion, NrRuntimeVersionText(), GameExeName().c_str());
    }
    if (streamline_hooks_enabled.load() && hooks_enabled.load()) {
      ImGui::PushTextWrapPos(0.f);
      ImGui::TextColored(
          ui::tokens::kWarn, "%s",
          ui::Tr("Streamline hooks are on (EnableHooks=1). If the game crashes at boot,"
                 " set EnableHooks=2: NGX-only still covers Streamline's DLSS calls."));
      ImGui::PopTextWrapPos();
    }
    ui::SectionGap();
  }
  if (classic) {
    DrawClassicLayout(runtime);
  } else {
    DrawModernLayout(runtime, verdict);
  }
}

// i18n: end

// The overlay callback: the theme and the dark background around the panel.
// Every push is owned by a StyleScope, so nothing reaches ReShade's own tabs
// or another add-on's window; ui::style_depth is back to zero here or the
// capture lane fails.  The modern layout is one column at most
// tokens::kColumnEm wide (a one-column table, so headers, wrapped text and
// the card end where the controls do); the classic layout keeps the full
// width it always had.
inline void OnOverlay(reshade::api::effect_runtime* runtime) {
  const int64_t started = ui_capture_dir.empty() ? 0 : SteadyNowNs();
  RefreshUiLanguage();
  ui::reset_buttons_drawn = 0;
  overlay_window_stranded.store(
      !ImGui::IsWindowDocked()
              && (std::min)(ImGui::GetWindowWidth(), ImGui::GetWindowHeight())
                     < ImGui::GetFontSize() * 8.f
          ? ImGui::GetFrameCount()
          : -1,
      std::memory_order_relaxed);
  {
    ui::StyleScope theme;
    ui::PushPanelTheme(theme);
    theme.Var(ImGuiStyleVar_WindowPadding,
              ImVec2(ui::tokens::kCardPadding, ui::tokens::kCardPadding))
        .Var(ImGuiStyleVar_CellPadding, ImVec2(0.f, 0.f));
    if (ImGui::BeginChild("##nr_panel", ImVec2(0.f, 0.f),
                          ImGuiChildFlags_AlwaysUseWindowPadding)) {
      ImGui::PushTextWrapPos(0.f);
      if (ui_classic_layout.load()) {
        DrawPanelBody(runtime);
      } else {
        const float column = (std::min)(ImGui::GetContentRegionAvail().x,
                                        ImGui::GetFontSize() * ui::tokens::kColumnEm);
        if (ImGui::BeginTable("##nr_column", 1, ImGuiTableFlags_None, ImVec2(column, 0.f))) {
          ImGui::TableSetupColumn("##column", ImGuiTableColumnFlags_WidthFixed, column);
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          DrawPanelBody(runtime);
          ImGui::EndTable();
        }
      }
      ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
  }
  SettleSlider(false);
  if (!ui_capture_dir.empty()) {
    if (ui_capture_tooltip) {
      ImGui::SetNextWindowPos(ImVec2(ImGui::GetFontSize() * 38.f, ImGui::GetFontSize() * 8.f));
      ui::force_next_tooltip = true;
      ui::ItemTooltip("%s", ui::Tr(ui::text::kTransferTip));
    }
    ui_panel_draws.fetch_add(1, std::memory_order_relaxed);
    ui_panel_ns.fetch_add(static_cast<uint64_t>(SteadyNowNs() - started),
                          std::memory_order_relaxed);
    ui_panel_resets.store(ui::reset_buttons_drawn, std::memory_order_relaxed);
    if (ui::style_depth != 0) {
      ui_style_depth_worst.store(ui::style_depth, std::memory_order_relaxed);
    }
  }
}

// ---------------------------------------------------------------------------
// The panel capture (test only, U0)
// ---------------------------------------------------------------------------
// RENODX_NR_TEST_UI_CAPTURE=<dir>, relative to the addon's folder, makes a
// session open ReShade's overlay by itself, select this addon's window, and
// write one PNG per distinct card once that card has been drawn on
// kUiCaptureSettleFrames frames in a row (an auto-sized child takes its
// height on the frame after it appears).  Each capture states one line:
//   NR-UI v1 capture card=<slug> tone=<tone> file=<name> w= h= tone_px=
//     control_px= draws= style_depth= panel_us= layout=<modern|classic>
//     view=<mine|defaults> resets=<n>
// tone_px counts back-buffer pixels of exactly the card's tone colour - the
// stripe and glyph the widget drew - read back after ReShade rendered its
// overlay (reshade_present), so the lane grades pixels, not a claim.
// control_px counts a tone the card does not use (red, or green on a red
// card): the same count over the same frame, so a background that happens
// to hold the tone cannot pass for a drawn card.
// resets counts the reset buttons that frame drew: one per visible row
// whose setting differs from its default, so a lane that stages changed
// settings can tell the rows were read and drawn.  view names whether the
// Defaults view was on (RENODX_NR_TEST_UI_VIEW=defaults turns it on after
// the first capture, and every card is captured again as ui-<card>-defaults).
// RENODX_NR_TEST_UI_EXPAND=1 opens every modern section, so the controls a
// player reaches only by expanding one are drawn - and counted - too.
// Unset, which is every shipped session, none of this is registered.
inline constexpr uint32_t kUiCaptureSettleFrames = 10;

inline bool InitUiCapture() {
  char buffer[512] = {};
  size_t length = 0;
  if (getenv_s(&length, buffer, sizeof(buffer), "RENODX_NR_TEST_UI_CAPTURE") != 0
      || length <= 1) {
    return false;
  }
  std::filesystem::path dir(buffer);
  if (dir.is_relative()) dir = AddonDirectory() / dir;
  std::error_code error;
  std::filesystem::create_directories(dir, error);
  ui_capture_dir = dir;
  length = 0;
  ui_capture_tooltip =
      getenv_s(&length, buffer, sizeof(buffer), "RENODX_NR_TEST_UI_TOOLTIP") == 0
      && std::strcmp(buffer, "1") == 0;
  length = 0;
  ui_capture_defaults_view =
      getenv_s(&length, buffer, sizeof(buffer), "RENODX_NR_TEST_UI_VIEW") == 0
      && std::strcmp(buffer, "defaults") == 0;
  length = 0;
  ui::expand_all_sections =
      getenv_s(&length, buffer, sizeof(buffer), "RENODX_NR_TEST_UI_EXPAND") == 0
      && std::strcmp(buffer, "1") == 0;
  return true;
}

inline void OnUiCaptureOverlay(reshade::api::effect_runtime* /*runtime*/) {
  // Selects this addon's tab in ReShade's dock; until then Begin() for a
  // docked tab that is not selected returns false and OnOverlay never runs.
  if (ui_panel_draws.load(std::memory_order_relaxed) == 0) {
    ImGui::SetWindowFocus(kOverlayTitle);
  }
}

inline void OnUiCapturePresent(reshade::api::effect_runtime* runtime) {
  static uint32_t frames = 0;
  static uint16_t last_card = 0xffff;
  static uint32_t held = 0;
  static bool captured[ui::kCardIdCount] = {};
  if (++frames == 2) {
    runtime->open_overlay(true, reshade::api::input_source::none);
    hud_overlay_open = true;  // see OnReshadeOpenOverlay
  }

  const uint16_t drawn = ui_drawn_card.exchange(0xffff, std::memory_order_relaxed);
  const auto id = static_cast<uint8_t>(drawn & 0xff);
  if (drawn == 0xffff || id >= ui::kCardIdCount) {
    held = 0;
    return;
  }
  if (drawn != last_card) {
    last_card = drawn;
    held = 0;
  }
  if (++held != kUiCaptureSettleFrames || captured[id]) return;
  captured[id] = true;

  ui::StatusCard card;
  card.id = static_cast<ui::CardId>(id);
  card.tone = static_cast<ui::CardTone>(drawn >> 8);
  const char* slug = ui::CardSlug(card.id);
  const std::string file =
      std::string("ui-") + slug + (ui_defaults_view ? "-defaults" : "") + ".png";

  uint32_t width = 0;
  uint32_t height = 0;
  runtime->get_screenshot_width_and_height(&width, &height);
  const reshade::api::format format = reshade::api::format_to_default_typed(
      runtime->get_device()->get_resource_desc(runtime->get_current_back_buffer())
          .texture.format);
  const bool bgra = format == reshade::api::format::b8g8r8a8_unorm
                    || format == reshade::api::format::b8g8r8x8_unorm;
  const bool rgba = format == reshade::api::format::r8g8b8a8_unorm
                    || format == reshade::api::format::r8g8b8x8_unorm;
  std::vector<uint8_t> pixels;
  bool ok = (bgra || rgba) && width != 0 && height != 0;
  if (ok) {
    pixels.resize(static_cast<size_t>(width) * height * 4);
    ok = runtime->capture_screenshot(pixels.data());
  }
  uint32_t tone_px = 0;
  uint32_t control_px = 0;
  if (ok) {
    const uint32_t rgb = ui::ToneRgb(card.tone);
    const uint32_t control = ui::ToneRgb(
        card.tone == ui::CardTone::kBad ? ui::CardTone::kGood : ui::CardTone::kBad);
    for (size_t at = 0; at < pixels.size(); at += 4) {
      if (bgra) std::swap(pixels[at], pixels[at + 2]);
      pixels[at + 3] = 255;
      const uint32_t value = (static_cast<uint32_t>(pixels[at]) << 16)
                             | (static_cast<uint32_t>(pixels[at + 1]) << 8)
                             | pixels[at + 2];
      if (value == rgb) ++tone_px;
      if (value == control) ++control_px;
    }
    ok = screenshot::internal::WritePngRgba8(ui_capture_dir / file, width, height,
                                             pixels);
  }
  const uint32_t draws = ui_panel_draws.load(std::memory_order_relaxed);
  const double panel_us =
      draws == 0 ? 0.0
                 : static_cast<double>(ui_panel_ns.load(std::memory_order_relaxed))
                       / draws / 1000.0;
  // The support report beside the PNGs, rewritten at every capture so the
  // last one describes the card the session ended on (U3: the lane greps it
  // for redaction and hands it to log_verdict.py as a fixture).
  {
    std::ofstream report(ui_capture_dir / "support-report.txt", std::ios::binary);
    report << BuildSupportReport();
  }
  char line[384];
  std::snprintf(
      line, sizeof(line),
      "NR-UI v1 capture card=%s tone=%s file=%s w=%u h=%u tone_px=%u"
      " control_px=%u draws=%u style_depth=%d panel_us=%.1f layout=%s view=%s"
      " resets=%d%s",
      slug, ui::CardToneName(card.tone), ok ? file.c_str() : "-", width, height,
      tone_px, control_px, draws,
      ui_style_depth_worst.load(std::memory_order_relaxed), panel_us,
      ui_classic_layout.load() ? "classic" : "modern",
      ui_defaults_view ? "defaults" : "mine",
      ui_panel_resets.load(std::memory_order_relaxed), ok ? "" : " failed=readback");
  Log(ok ? reshade::log::level::info : reshade::log::level::warning, line);
  if (ui_capture_defaults_view && !ui_defaults_view) {
    SetDefaultsView(true);
    std::fill(std::begin(captured), std::end(captured), false);
    last_card = 0xffff;
    held = 0;
  }
}

// One timestamped backup beside this game's global ReShade.ini,
// taken immediately before a config migration rewrites keys inside it.
// Failures are silent by design: a missing/unreadable INI must not block
// loading, and the migration itself stays correct without the backup.
inline void BackupConfigFile(const char* suffix) {
  try {
    const std::filesystem::path ini = ReShadeBaseDirectory() / "ReShade.ini";
    if (!std::filesystem::exists(ini)) {
      Log(reshade::log::level::warning,
          std::string("settings migration (") + suffix
              + "): no ReShade.ini in ReShade's base folder, so no backup was taken");
      return;
    }
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[24]{};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    const std::string name =
        std::string("ReShade.ini.renodx-dlss5-") + suffix + "-" + stamp + ".bak";
    std::error_code error;
    std::filesystem::copy_file(ini, ReShadeBaseDirectory() / name,
                               std::filesystem::copy_options::none, error);
    Log(error ? reshade::log::level::warning : reshade::log::level::info,
        error ? "settings migration (" + std::string(suffix) + "): backup " + name
                    + " not written (" + error.message() + "); migrating without one"
              : "settings migration (" + std::string(suffix) + "): backup " + name);
  } catch (...) {
    Log(reshade::log::level::warning,
        std::string("settings migration (") + suffix + "): backup failed; migrating without one");
  }
}

// Finite-value validation precedes range clamping (v5.3): a NaN/Inf or an
// out-of-domain enum falls back to the documented default, instead of being
// clamped into a plausible-looking wrong value.  One diagnostic per process
// names the first offending key; every later offender still defaults, it
// just does not re-announce itself.
inline std::atomic_bool config_value_rejected_logged{false};

inline float SanitizeFloatKey(
    float value,
    float fallback,
    float lo,
    float hi,
    const char* key) {
  if (std::isfinite(value)) return std::clamp(value, lo, hi);
  if (!config_value_rejected_logged.exchange(true)) {
    Log(reshade::log::level::warning,
        std::string("config key ") + key
            + " is NaN/Inf; using the documented default");
  }
  return fallback;
}

inline uint32_t SanitizeEnumKey(
    uint32_t value,
    uint32_t fallback,
    uint32_t min_value,
    uint32_t max_value,
    const char* key) {
  if (value >= min_value && value <= max_value) return value;
  if (!config_value_rejected_logged.exchange(true)) {
    Log(reshade::log::level::warning,
        std::string("config key ") + key + " has value "
            + std::to_string(value) + " outside its valid range; using the"
            " documented default");
  }
  return fallback;
}

inline void LoadConfiguration() {
  bool saved_crash_dump = false;
  reshade::get_config_value(nullptr, kConfigSection, "NRCrashDump", saved_crash_dump);
  lastgasp::dumps_on.store(saved_crash_dump, std::memory_order_relaxed);
  char report_hosts[256]{};
  size_t report_hosts_length = sizeof(report_hosts);
  if (reshade::get_config_value(nullptr, kConfigSection, "NRReportHost", report_hosts, &report_hosts_length)) {
    issue_report::host_setting = report_hosts;
  }

  // Config schema marker (kConfigVersion at the top of this file).  v5.3
  // replaces the wholesale stale-config reset - the v5 failure mode that
  // silently erased user tuning - with TARGETED key-wise migration: reads are
  // unconditional, a schema <4 config migrates exactly the two inherited
  // codec/history defaults once (after a timestamped file backup), and a
  // future-version file is read tolerantly and never rewritten.
  // EnableHooks stays exempt - it is a per-game support knob, not a user
  // setting, and a migration must never flip a game's needed hook policy.
  uint32_t stored_config_version = 0;
  const bool version_present = reshade::get_config_value(
      nullptr, kConfigSection, "ConfigVersion", stored_config_version);
  const bool future_config = version_present && stored_config_version > kConfigVersion;
  const bool migrate_config = !future_config && stored_config_version != kConfigVersion;

  // Saved values start AT the defaults, so a stale config simply loads
  // nothing and the apply section below persists a fresh default state.
  bool saved_enabled = defaults::kEnabled;
  bool saved_pre_upscale = defaults::kPreSr;
  uint32_t saved_codec_mode = defaults::kCodecMode;
  float saved_anchor = defaults::kProxyAnchorNits;
  uint32_t saved_preset = defaults::kPreset;
  uint32_t saved_style = defaults::kStyle;
  float saved_intensity = defaults::kIntensity;
  float saved_local_tone = defaults::kLocalTone;
  float saved_local_structure = defaults::kLocalStructure;
  float saved_skin_structure = defaults::kSkinStructure;
  bool saved_skin_independent = defaults::kSkinIndependent;
  bool saved_auto_mask = defaults::kAutoMask;
  bool saved_ui_correction = defaults::kUiCorrection;
  uint32_t saved_depth_mode = defaults::kDepthMode;
  float saved_motion_x = defaults::kMvecScale;
  float saved_motion_y = defaults::kMvecScale;
  float saved_paper_white = defaults::kPaperWhiteScale;
  float saved_diffuse_white = defaults::kDiffuseWhiteNits;
  float saved_pq_calibration = defaults::kPqCalibration;
  uint32_t saved_source_encoding = defaults::kSourceEncoding;
  uint32_t saved_source_primaries = defaults::kSourcePrimaries;
  float saved_linear_unit_nits = defaults::kLinearUnitNits;
  float saved_transfer = defaults::kTransferStrength;
  float saved_color = defaults::kColorStrength;
  float saved_chroma_clamp = defaults::kChromaClampStops;
  uint32_t saved_transfer_mode = defaults::kTransferMode;
  uint32_t saved_display_pedestal = defaults::kDisplayPedestal;
  uint32_t saved_feed_mode = defaults::kFeedMode;
  uint32_t saved_norm_governor = defaults::kNormGovernor;
  float saved_norm_slew = defaults::kNormSlewStops;
  float saved_norm_attack = defaults::kNormAttackStops;
  float saved_norm_release = defaults::kNormReleaseStops;
  bool saved_chained_history = defaults::kChainedHistory;
  bool saved_neural_floor_guard = defaults::kNeuralFloorGuard;
  uint32_t saved_toggle_key = defaults::kToggleHotkey;
  uint32_t saved_screenshot_key = defaults::kScreenshotHotkey;
  uint32_t saved_worksets = kDefaultMaxWorksets;
  uint32_t saved_passes = defaults::kStackPasses;
  float saved_pass_values[kMaxNrPasses - 1][3] = {};
  for (uint32_t pass = 0; pass < kMaxNrPasses - 1; ++pass) {
    saved_pass_values[pass][0] = defaults::kPassStrength;
    saved_pass_values[pass][1] = defaults::kPassStrength;
    saved_pass_values[pass][2] = defaults::kPassStrength;
  }
  bool saved_follow_input = defaults::kFollowInputRes;
  float saved_res_scale = defaults::kResolutionScale;
  bool saved_gpu_timers = defaults::kGpuTimers;
  bool saved_edit_trace = defaults::kEditTrace;
  // The look stage (PLAN_NR_LOOK_V71.md 3): every value starts at the
  // identity, so an ini without the keys loads a look that changes nothing.
  bool saved_look_mode = defaults::kLookMode;
  look::Settings saved_look = defaults::kLook;
  // Per-pass model steering (group G), passes 2 onward.
  struct SavedSteering {
    bool follow = defaults::kPassFollow;
    uint32_t style = defaults::kStyle;
    float local_tone = defaults::kLocalTone;
    float structure = defaults::kLocalStructure;
    float skin = defaults::kSkinStructure;
    bool auto_mask = defaults::kAutoMask;
    bool ui_correction = defaults::kUiCorrection;
  } saved_steering[kMaxNrPasses - 1];

  // Reads are unconditional (targeted migration, not reset): presence of the
  // first-party keys drives the migration decisions below.
  const bool has_stored_uplift = reshade::get_config_value(
      nullptr, kConfigSection, "NeuralUplift", saved_enabled);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRPreUpscale", saved_pre_upscale);
  const bool has_stored_codec = reshade::get_config_value(
      nullptr, kConfigSection, "NRCodecMode", saved_codec_mode);
  reshade::get_config_value(nullptr, kConfigSection, "NRProxyAnchor", saved_anchor);
  reshade::get_config_value(nullptr, kConfigSection, "NRPreset", saved_preset);
  reshade::get_config_value(nullptr, kConfigSection, "NRStyle", saved_style);
  reshade::get_config_value(nullptr, kConfigSection, "NRIntensity", saved_intensity);
  reshade::get_config_value(nullptr, kConfigSection, "NRLocalTone", saved_local_tone);
  // NRGlobalTone is retired (rc5: runtime 310.8 has no global tone key) but
  // still read.  The third-party Feeder detects this addon by the string and
  // writes the key (tools/field/check_feeder_contract.py), so a stored value
  // is named in the log instead of silently doing nothing.
  if (float retired_global_tone = 1.f;
      reshade::get_config_value(
          nullptr, kConfigSection, "NRGlobalTone", retired_global_tone)
      && retired_global_tone != 1.f) {
    Log(reshade::log::level::info,
        "NRGlobalTone=" + std::to_string(retired_global_tone)
            + " is stored but has no effect: NR runtime 310.8 reads no global"
              " tone key, and the setting was removed in rc5");
  }
  reshade::get_config_value(
      nullptr, kConfigSection, "NRLocalStructure", saved_local_structure);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRSkinStructure", saved_skin_structure);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRSkinIndependent", saved_skin_independent);
  reshade::get_config_value(nullptr, kConfigSection, "NRAutoMask", saved_auto_mask);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRUICorrection", saved_ui_correction);
  reshade::get_config_value(nullptr, kConfigSection, "NRDepthMode", saved_depth_mode);
  reshade::get_config_value(nullptr, kConfigSection, "NRMVecScaleX", saved_motion_x);
  reshade::get_config_value(nullptr, kConfigSection, "NRMVecScaleY", saved_motion_y);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRPaperWhiteScale", saved_paper_white);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRDiffuseWhiteNits", saved_diffuse_white);
  const bool has_stored_pq_calibration = reshade::get_config_value(
      nullptr, kConfigSection, "NRPQCalibration", saved_pq_calibration);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRSourceEncoding", saved_source_encoding);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRSourcePrimaries", saved_source_primaries);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRLinearUnitNits", saved_linear_unit_nits);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRTransferStrength", saved_transfer);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRColorStrength", saved_color);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRChromaClamp", saved_chroma_clamp);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRTransferMode", saved_transfer_mode);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRDisplayPedestal", saved_display_pedestal);
  uint32_t saved_dedupe_mode = defaults::kDedupeMode;
  reshade::get_config_value(
      nullptr, kConfigSection, "DedupeMode", saved_dedupe_mode);
  uint32_t saved_list_state_mode = defaults::kListStateMode;
  reshade::get_config_value(
      nullptr, kConfigSection, "ListStateMode", saved_list_state_mode);
  reshade::get_config_value(nullptr, kConfigSection, "NRFeedMode", saved_feed_mode);
  const bool has_stored_governor = reshade::get_config_value(
      nullptr, kConfigSection, "NRNormGovernor", saved_norm_governor);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRNormSlewStops", saved_norm_slew);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRNormAttackStops", saved_norm_attack);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRNormReleaseStops", saved_norm_release);
  const bool has_stored_chained = reshade::get_config_value(
      nullptr, kConfigSection, "NRChainedHistory", saved_chained_history);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRNeuralFloorGuard", saved_neural_floor_guard);
  // Hotkeys: persisted virtual-key codes.  Anything outside the defined range
  // (a corrupted or hand-edited line) falls back to the defaults.
  reshade::get_config_value(nullptr, kConfigSection, "NRToggleKey", saved_toggle_key);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRScreenshotKey", saved_screenshot_key);
  uint32_t saved_screenshot_format = 0;
  reshade::get_config_value(
      nullptr, kConfigSection, "NRScreenshotFormat", saved_screenshot_format);
  screenshot::file_format =
      SanitizeEnumKey(saved_screenshot_format, 0u, 0u, 2u, "NRScreenshotFormat");
  uint32_t saved_screenshot_layout = 0;
  reshade::get_config_value(
      nullptr, kConfigSection, "NRScreenshotLayout", saved_screenshot_layout);
  screenshot::layout =
      SanitizeEnumKey(saved_screenshot_layout, 0u, 0u, 1u, "NRScreenshotLayout");
  uint32_t saved_jpeg_quality = 95;
  reshade::get_config_value(
      nullptr, kConfigSection, "NRScreenshotJpegQuality", saved_jpeg_quality);
  screenshot::jpeg_quality =
      SanitizeEnumKey(saved_jpeg_quality, 95u, 80u, 100u, "NRScreenshotJpegQuality");
  uint32_t saved_max_megabytes = 10;
  reshade::get_config_value(
      nullptr, kConfigSection, "NRScreenshotMaxMB", saved_max_megabytes);
  screenshot::max_megabytes =
      SanitizeEnumKey(saved_max_megabytes, 10u, 0u, 500u, "NRScreenshotMaxMB");
  uint32_t saved_worksets_read = saved_worksets;
  reshade::get_config_value(nullptr, kConfigSection, "NRMaxWorksets", saved_worksets_read);
  saved_worksets = saved_worksets_read;
  // v5 stacking + NR resolution.
  uint32_t saved_passes_read = saved_passes;
  reshade::get_config_value(nullptr, kConfigSection, "NRPasses", saved_passes_read);
  saved_passes = saved_passes_read;
  for (uint32_t pass = 1; pass < kMaxNrPasses; ++pass) {
    const std::string prefix = "NRPass" + std::to_string(pass + 1);
    reshade::get_config_value(
        nullptr, kConfigSection, (prefix + "Intensity").c_str(),
        saved_pass_values[pass - 1][0]);
    reshade::get_config_value(
        nullptr, kConfigSection, (prefix + "Transfer").c_str(),
        saved_pass_values[pass - 1][1]);
    reshade::get_config_value(
        nullptr, kConfigSection, (prefix + "Color").c_str(),
        saved_pass_values[pass - 1][2]);
    SavedSteering& steering = saved_steering[pass - 1];
    reshade::get_config_value(
        nullptr, kConfigSection, (prefix + "FollowPass1").c_str(), steering.follow);
    reshade::get_config_value(
        nullptr, kConfigSection, (prefix + "Style").c_str(), steering.style);
    reshade::get_config_value(
        nullptr, kConfigSection, (prefix + "LocalTone").c_str(), steering.local_tone);
    reshade::get_config_value(
        nullptr, kConfigSection, (prefix + "Structure").c_str(), steering.structure);
    reshade::get_config_value(
        nullptr, kConfigSection, (prefix + "Skin").c_str(), steering.skin);
    reshade::get_config_value(
        nullptr, kConfigSection, (prefix + "AutoMask").c_str(), steering.auto_mask);
    reshade::get_config_value(
        nullptr, kConfigSection, (prefix + "UICorrection").c_str(), steering.ui_correction);
  }
  reshade::get_config_value(
      nullptr, kConfigSection, "NRFollowInputRes", saved_follow_input);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRResolutionScale", saved_res_scale);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRGpuTimers", saved_gpu_timers);
  reshade::get_config_value(nullptr, kConfigSection, "NREditTrace", saved_edit_trace);
  reshade::get_config_value(nullptr, kConfigSection, "NRLookMode", saved_look_mode);
  reshade::get_config_value(nullptr, kConfigSection, "NRLookStrength", saved_look.strength);
  reshade::get_config_value(nullptr, kConfigSection, "NRLookBrighten", saved_look.brighten);
  reshade::get_config_value(nullptr, kConfigSection, "NRLookDarken", saved_look.darken);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRLookMaxBrighten", saved_look.max_brighten);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRLookMaxDarken", saved_look.max_darken);
  reshade::get_config_value(nullptr, kConfigSection, "NRLookColour", saved_look.colour);
  reshade::get_config_value(nullptr, kConfigSection, "NRLookHue", saved_look.hue);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRLookMaxColour", saved_look.max_colour);
  reshade::get_config_value(nullptr, kConfigSection, "NRLookShadows", saved_look.shadows);
  reshade::get_config_value(nullptr, kConfigSection, "NRLookMidtones", saved_look.midtones);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRLookHighlights", saved_look.highlights);
  reshade::get_config_value(nullptr, kConfigSection, "NRLookTone", saved_look.tone);
  reshade::get_config_value(nullptr, kConfigSection, "NRLookDetail", saved_look.detail);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRLookDetailRadius", saved_look.detail_radius);
  reshade::get_config_value(nullptr, kConfigSection, "NRLookHalo", saved_look.halo);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRLookStabilize", saved_look.stabilize);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRLookStabilizeMs", saved_look.stabilize_ms);
  reshade::get_config_value(
      nullptr, kConfigSection, "NRLookStabilizeDetail", saved_look.stabilize_detail);
  reshade::get_config_value(nullptr, kConfigSection, "NRLookUpsample", saved_look.upsample);
  // Diagnostics, read-only from the ini (never persisted): the telemetry line
  // interval (0 = off) and the per-frame normalization trace.
  uint32_t saved_telemetry_seconds = telemetry_seconds.load();
  reshade::get_config_value(
      nullptr, kConfigSection, "NRTelemetrySeconds", saved_telemetry_seconds);
  telemetry_seconds = std::min(saved_telemetry_seconds, 3600u);
  bool saved_norm_trace = false;
  reshade::get_config_value(
      nullptr, kConfigSection, "NRNormTrace", saved_norm_trace);
  norm_trace_enabled = saved_norm_trace;
  if (saved_norm_trace) {
    norm_trace::Configure(AddonDirectory() / "RenoDX-DLSS5-normtrace.csv");
  }
  bool saved_screenshot_planes = false;
  reshade::get_config_value(
      nullptr, kConfigSection, "NRScreenshotPlanes", saved_screenshot_planes);
  screenshot::diagnostic_planes_enabled = saved_screenshot_planes;
  // NREditTrace can be switched on in the Developer view at any time, so its
  // CSV path is set whether or not it starts on.
  edit_trace::Configure(AddonDirectory() / "RenoDX-DLSS5-edittrace.csv");
  // Read unconditionally: hook policy is a per-game support knob (see the
  // comment at the flag definitions) and must survive a config reset.
  // 0 = inert, 1 = NGX + Streamline, 2 = NGX only (default).  NGX-only keeps
  // sl.interposer/sl.common unpatched - the most contested patch site, and
  // installing both hook layers caused intermittent boot crashes in
  // Streamline-heavy games (FH6, Cyberpunk 2077).  Set EnableHooks=1 only for
  // games whose DLSS/DLSSD is reached solely through a private Streamline
  // bridge that never touches the NGX exports.
  uint32_t saved_hook_mode = 2;
  const bool hook_mode_stored = reshade::get_config_value(
      nullptr, kConfigSection, "EnableHooks", saved_hook_mode);

  hooks_enabled = saved_hook_mode != 0;
  streamline_hooks_enabled = saved_hook_mode == 1;
  // Auto-escalation only applies to the untouched default: an explicitly
  // stored EnableHooks is a user decision and is honored as written.
  streamline_escalation_allowed = !hook_mode_stored;
  streamline_auto_escalated = false;
  streamline_escalation_deficits = 0;

  // UiMode (U2): "classic" is the Developer view; anything else, absent
  // included, is the modern layout.  Read before the re-stamp below, which
  // persists it.
  {
    char saved_ui_mode[16] = {};
    size_t saved_ui_mode_length = sizeof(saved_ui_mode) - 1;
    ui_classic_layout =
        reshade::get_config_value(nullptr, kConfigSection, "UiMode", saved_ui_mode,
                                  &saved_ui_mode_length)
        && _stricmp(saved_ui_mode, "classic") == 0;
  }
  {
    bool saved_welcomed = false;
    reshade::get_config_value(nullptr, kConfigSection, "UiWelcomed", saved_welcomed);
    ui_welcomed = saved_welcomed;
  }
  // UiLanguage (v7.0.0): a language code from ui::kLanguages; "auto", absent
  // or a code this build does not know follows ReShade's language.
  {
    char saved_language[16] = {};
    size_t saved_language_length = sizeof(saved_language) - 1;
    int language = 0;
    if (reshade::get_config_value(nullptr, kConfigSection, "UiLanguage", saved_language,
                                  &saved_language_length)) {
      for (int i = 0; i < ui::kLanguageCount; ++i) {
        if (_stricmp(saved_language, ui::kLanguages[i].code) == 0) language = i + 1;
      }
    }
    ui_language_choice = language;
  }

  // P4: DX11Source (see the constants above for the policy).  Read with the
  // global-file target (nullptr) like every key in this function, so gate
  // step 2c (check_config_target.py) stays green, and never written back -
  // the addon does not rewrite a Feeder user's ini.  One-shot line at the
  // first load names the effective value and where it came from.
  {
    char saved_dx11_source[32] = {};
    size_t saved_dx11_source_length = sizeof(saved_dx11_source) - 1;
    const bool dx11_source_stored = reshade::get_config_value(
        nullptr, kConfigSection, "DX11Source", saved_dx11_source,
        &saved_dx11_source_length);
    static std::atomic_bool dx11_source_line_logged{false};
    uint32_t effective = kDx11SourceAuto;
    const char* origin = "default";
    if (dx11_source_stored) {
      std::string stored(saved_dx11_source);
      for (char& c : stored) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      origin = "stored";
      if (stored == "foreign") {
        effective = kDx11SourceForeign;
      } else if (stored == "native") {
        effective = kDx11SourceNative;
      } else if (stored == "auto") {
        effective = kDx11SourceAuto;
      } else if (stored == "off") {
        // A-6 (v7.0.0): inert in a D3D11-presenting process
        // (Dx11SourceOffDefers).
        effective = kDx11SourceOff;
      } else {
        // Unknown: auto, whose own fallback is the existing foreign
        // behavior whenever a DX11 bridge add-on is loaded.
        effective = kDx11SourceAuto;
        origin = "fallback";
      }
    }
    dx11_source = effective;
    if (dx11_source_line_logged.exchange(true)) {
      // One line per process.
    } else if (effective == kDx11SourceOff) {
      Log(reshade::log::level::info,
          "DX11Source=off (stored): in a process that presents through"
          " Direct3D 11 the addon stays inert - no NGX export is detoured,"
          " no evaluate is touched and the NR runtime is not loaded; a"
          " Direct3D 12 game is served once its swapchain appears");
    } else {
      std::ostringstream dx11_line;
      dx11_line << "DX11Source="
                << (effective == kDx11SourceForeign  ? "foreign"
                    : effective == kDx11SourceNative ? "native"
                                                     : "auto")
                << " (" << origin;
      if (origin == std::string_view("fallback")) {
        dx11_line << " from '" << saved_dx11_source << "': not a known value";
      }
      dx11_line
          << (effective == kDx11SourceForeign
                  ? "): D3D12 NGX evaluates from a third-party tool are"
                    " served; no D3D11 export is ever detoured"
              : effective == kDx11SourceNative
                  ? "): in a process that presents through Direct3D 11 the"
                    " game's own Direct3D 11 DLSS is served through the"
                    " Direct3D 11 bridge"
                  : "): in a process that presents through Direct3D 11 the"
                    " game's own Direct3D 11 DLSS is served through the"
                    " Direct3D 11 bridge, unless a DX11 bridge add-on of"
                    " another project is loaded, which keeps the foreign"
                    " route");
      Log(reshade::log::level::info, dx11_line.str());
    }
  }
  // Motion transport is an ini-only bridge compatibility selector. This
  // function runs at DLL_PROCESS_ATTACH; changing the key requires an addon
  // reload, which tears down and rebuilds its bridge surface sets. Auto
  // preserves direct sharing first, then tries the lossless RGBA32F route and
  // the explicit FP16 precision fallback for exact R32G32_FLOAT inputs.
  {
    char saved_motion_transport[32] = {};
    size_t saved_motion_transport_length = sizeof(saved_motion_transport) - 1;
    const bool stored_value = reshade::get_config_value(
        nullptr, kConfigSection, "DX11MotionTransport", saved_motion_transport,
        &saved_motion_transport_length);
    BridgeMotionTransportPreference effective =
        BridgeMotionTransportPreference::kAuto;
    const char* origin = stored_value ? "stored" : "default";
    std::string stored(saved_motion_transport);
    for (char& c : stored) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (stored_value) {
      if (stored == "direct") {
        effective = BridgeMotionTransportPreference::kDirect;
      } else if (stored == "lossless") {
        effective = BridgeMotionTransportPreference::kLossless;
      } else if (stored == "fp16") {
        effective = BridgeMotionTransportPreference::kFp16;
      } else if (stored != "auto") {
        origin = "fallback";
      }
    }
    bridge_motion_transport_preference.store(
        static_cast<uint32_t>(effective), std::memory_order_relaxed);
    Log(reshade::log::level::info,
        std::string("DX11MotionTransport=")
            + BridgeMotionPreferenceName(effective) + " (" + origin
            + "): applies to R32G32_FLOAT motion; auto tries direct, lossless"
              " RGBA32F, then FP16 RGBA16F; fp16 rounds through binary16");
  }
  // ForeignNr (ini only): yield (default, and any unknown value) stands the
  // addon down once another producer creates a feature-18 instance; observe
  // keeps NR running and only logs it (see foreign_nr_seen).
  {
    char saved_foreign_nr[16] = {};
    size_t saved_foreign_nr_length = sizeof(saved_foreign_nr) - 1;
    foreign_nr_yield = !(reshade::get_config_value(
                             nullptr, kConfigSection, "ForeignNr",
                             saved_foreign_nr, &saved_foreign_nr_length)
                         && _stricmp(saved_foreign_nr, "observe") == 0);
  }

  uint32_t retired_hook = saved_pre_upscale ? 1u : 0u;
  const bool has_retired_hook = reshade::get_config_value(
      nullptr, kConfigSection, "NRHookPoint", retired_hook);

  // Targeted migration (v4 schema).  Exactly the two inherited defaults the
  // handoff names, applied once, after a timestamped file backup; everything
  // else the user stored loads untouched.  A stored false/anchored value was
  // written by PersistConfig echoing the OLD defaults - it is inherited, not
  // chosen - so it migrates; the backup preserves it for rollback, and a
  // later explicit user write (post-v4 marker) is always honored.
  if (migrate_config && (has_stored_uplift || version_present || has_retired_hook)) {
    BackupConfigFile(
        (stored_config_version < 4
             ? std::string("pre-v4-migration")
             : "pre-v" + std::to_string(stored_config_version + 1)
                   + "-migration")
            .c_str());
    if (stored_config_version < 4) {
      // The v2 -> v3 default change only: a stored NRCodecMode=1 was written
      // by PersistConfig echoing the anchored default - inherited, not
      // chosen - so it migrates to the classic default.  Classic (0) and any
      // manual selection survive; v6's Auto default applies solely to configs
      // that never stored the key.
      if (saved_codec_mode == 1u) {
        saved_codec_mode = 0u;
      }
      if (!saved_chained_history) {
        saved_chained_history = defaults::kChainedHistory;
      }
      Log(
          reshade::log::level::info,
          "config schema v" + std::to_string(stored_config_version)
              + " migrated key-wise to v" + std::to_string(kConfigVersion)
              + " (inherited NRCodecMode/NRChainedHistory defaults adopted;"
                " the original ReShade.ini was backed up beside the game config)");
    }
    if (stored_config_version < 5) {
      // NRPQCalibration changed meaning with the v6 codec math: 2.5375 was
      // the pre-v5 default whose effective PQ divisor (~515 nits) defined the
      // Classic look.  A versioned config without an explicit value ran with
      // that number, so the migration pins it and the image stays unchanged;
      // fresh configs default to 1 (the 203-nit reference).  Manual
      // NRCodecMode 0/1 selections are preserved - only defaultless configs
      // land on Auto.
      if (!has_stored_pq_calibration) {
        saved_pq_calibration = codec::kLegacyPaperWhite;
      }
      std::ostringstream message;
      message << "config schema v" << stored_config_version
              << " migrated key-wise to v" << kConfigVersion
              << " (NRPQCalibration pinned at " << saved_pq_calibration
              << "; manual NRCodecMode selections preserved)";
      Log(reshade::log::level::info, message.str());
    }
    if (stored_config_version < 6 && has_stored_governor
        && saved_norm_governor == 1u) {
      // v6 schema: Stable (2) becomes the governor default.  A stored 1 was
      // the v6.1.2 default echoed by PersistConfig - inherited, not chosen
      // (the same rule the v4 migration applied to NRCodecMode) - so it
      // migrates; an explicit Off (0) is a user decision and stays.
      saved_norm_governor = defaults::kNormGovernor;
      Log(reshade::log::level::info,
          "config schema v" + std::to_string(stored_config_version)
              + " migrated key-wise to v" + std::to_string(kConfigVersion)
              + " (inherited NRNormGovernor=1 adopts the Stable governor;"
                " set NRNormGovernor=1 to return to Slew)");
    }
  }

  if (stored_config_version < 8u) {
    // rc10 has the original before/after-SR paths. Present is not a v7.5 mode.
    if (has_retired_hook) {
      saved_pre_upscale = retired_hook == 1u;
      Log(reshade::log::level::info,
          "NRHookPoint migrated to NRPreUpscale; Present maps to Upscaled in v7.5");
    }
    // Explicit Edge-aware has the same meaning in both schemas. New v8
    // upsampling modes and SDR pedestal modes fall back to rc10's defaults.
    if (saved_look.upsample != 1u) saved_look.upsample = 0u;
    if (saved_display_pedestal > 1u) saved_display_pedestal = 0u;
  }
  std::string ignored_keys;
  for (const char* key : {"NRAutoFgFallback", "NRScreenshotSource", "NROutputRect",
                          "NRDetailStability", "NRLookHaloBrightSide",
                          "NRSdrBlackRestore", "NRStyleHighlightGuard"}) {
    char value[256]{};
    size_t size = sizeof(value);
    if (reshade::get_config_value(nullptr, kConfigSection, key, value, &size)) {
      if (!ignored_keys.empty()) ignored_keys += ", ";
      ignored_keys += key;
    }
  }
  if (!ignored_keys.empty()) {
    Log(reshade::log::level::info, "v7.5 ignores v8-only settings: " + ignored_keys);
  }

  max_worksets = SanitizeEnumKey(
      saved_worksets, kDefaultMaxWorksets, 1u, 8u, "NRMaxWorksets");
  stack_passes = SanitizeEnumKey(
      saved_passes, defaults::kStackPasses, 1u, kMaxNrPasses, "NRPasses");
  for (uint32_t pass = 1; pass < kMaxNrPasses; ++pass) {
    pass_intensity[pass - 1] = SanitizeFloatKey(
        saved_pass_values[pass - 1][0], defaults::kPassStrength, 0.f, 2.f,
        "NRPassIntensity");
    pass_transfer_strength[pass - 1] = SanitizeFloatKey(
        saved_pass_values[pass - 1][1], defaults::kTransferStrength, 0.f, 1.f,
        "NRPassTransfer");
    pass_color_strength[pass - 1] = SanitizeFloatKey(
        saved_pass_values[pass - 1][2], defaults::kColorStrength, 0.f, 1.f,
        "NRPassColor");
    // The per-pass steering takes its pass-1 counterpart's range.
    const SavedSteering& steering = saved_steering[pass - 1];
    pass_follow[pass - 1] = steering.follow;
    pass_style[pass - 1] =
        SanitizeEnumKey(steering.style, defaults::kStyle, 0u, 2u, "NRPassStyle");
    pass_local_tone[pass - 1] = SanitizeFloatKey(
        steering.local_tone, defaults::kLocalTone, 0.f, 2.f, "NRPassLocalTone");
    pass_local_structure[pass - 1] = SanitizeFloatKey(
        steering.structure, defaults::kLocalStructure, 0.f, 2.f, "NRPassStructure");
    pass_skin_structure[pass - 1] = SanitizeFloatKey(
        steering.skin, defaults::kSkinStructure, -1.f, 1.f, "NRPassSkin");
    pass_auto_mask[pass - 1] = steering.auto_mask;
    pass_ui_correction[pass - 1] = steering.ui_correction;
  }
  nr_follow_input_res = saved_follow_input;
  nr_resolution_scale = SanitizeFloatKey(
      saved_res_scale, defaults::kResolutionScale, 0.33f, 1.f,
      "NRResolutionScale");
  {
    // Foveated NR (foveation.hpp).
    bool saved_foveation = defaults::kFoveation;
    uint32_t saved_width = defaults::kFoveationWidth;
    uint32_t saved_height = defaults::kFoveationHeight;
    uint32_t saved_falloff = defaults::kFoveationFalloff;
    float saved_edge = defaults::kFoveationEdge;
    reshade::get_config_value(nullptr, kConfigSection, "NRFoveation", saved_foveation);
    reshade::get_config_value(nullptr, kConfigSection, "NRFoveationWidth", saved_width);
    reshade::get_config_value(nullptr, kConfigSection, "NRFoveationHeight", saved_height);
    reshade::get_config_value(nullptr, kConfigSection, "NRFoveationFalloff", saved_falloff);
    reshade::get_config_value(nullptr, kConfigSection, "NRFoveationEdge", saved_edge);
    nr_foveation = saved_foveation;
    // Pixel values are only bounded here; the output resolution clamps them
    // every frame (foveation::MakeAxis), so one setting serves any display.
    nr_foveation_width = SanitizeEnumKey(
        saved_width, defaults::kFoveationWidth, 0u, defaults::kFoveationMaxPixels,
        "NRFoveationWidth");
    nr_foveation_height = SanitizeEnumKey(
        saved_height, defaults::kFoveationHeight, 0u, defaults::kFoveationMaxPixels,
        "NRFoveationHeight");
    nr_foveation_falloff = SanitizeEnumKey(
        saved_falloff, defaults::kFoveationFalloff, 0u, defaults::kFoveationMaxPixels,
        "NRFoveationFalloff");
    nr_foveation_edge = SanitizeFloatKey(
        saved_edge, defaults::kFoveationEdge, foveation::kMinEdge, foveation::kMaxEdge,
        "NRFoveationEdge");
  }
  gpu_timers_enabled = saved_gpu_timers;
  edit_trace_enabled = saved_edit_trace;
  look_mode = saved_look_mode;
  look_strength = SanitizeFloatKey(
      saved_look.strength, defaults::kLook.strength, 0.f, 2.f, "NRLookStrength");
  look_brighten = SanitizeFloatKey(
      saved_look.brighten, defaults::kLook.brighten, 0.f, 2.f, "NRLookBrighten");
  look_darken = SanitizeFloatKey(
      saved_look.darken, defaults::kLook.darken, 0.f, 2.f, "NRLookDarken");
  look_max_brighten = SanitizeFloatKey(
      saved_look.max_brighten, defaults::kLook.max_brighten, 0.f, 4.f,
      "NRLookMaxBrighten");
  look_max_darken = SanitizeFloatKey(
      saved_look.max_darken, defaults::kLook.max_darken, 0.f, 4.f, "NRLookMaxDarken");
  look_colour = SanitizeFloatKey(
      saved_look.colour, defaults::kLook.colour, 0.f, 2.f, "NRLookColour");
  look_hue = SanitizeFloatKey(saved_look.hue, defaults::kLook.hue, 0.f, 2.f, "NRLookHue");
  look_max_colour = SanitizeFloatKey(
      saved_look.max_colour, defaults::kLook.max_colour, 0.f, 2.f, "NRLookMaxColour");
  look_shadows = SanitizeFloatKey(
      saved_look.shadows, defaults::kLook.shadows, 0.f, 2.f, "NRLookShadows");
  look_midtones = SanitizeFloatKey(
      saved_look.midtones, defaults::kLook.midtones, 0.f, 2.f, "NRLookMidtones");
  look_highlights = SanitizeFloatKey(
      saved_look.highlights, defaults::kLook.highlights, 0.f, 2.f, "NRLookHighlights");
  look_tone = SanitizeFloatKey(saved_look.tone, defaults::kLook.tone, 0.f, 2.f, "NRLookTone");
  look_detail = SanitizeFloatKey(
      saved_look.detail, defaults::kLook.detail, 0.f, 2.f, "NRLookDetail");
  look_detail_radius = SanitizeFloatKey(
      saved_look.detail_radius, defaults::kLook.detail_radius, 0.25f, 4.f,
      "NRLookDetailRadius");
  look_halo = SanitizeFloatKey(saved_look.halo, defaults::kLook.halo, 0.f, 1.f, "NRLookHalo");
  look_stabilize = SanitizeEnumKey(
      saved_look.stabilize, defaults::kLook.stabilize, 0u, 2u, "NRLookStabilize");
  look_stabilize_ms = SanitizeFloatKey(
      saved_look.stabilize_ms, defaults::kLook.stabilize_ms, 10.f, 250.f,
      "NRLookStabilizeMs");
  look_stabilize_detail = saved_look.stabilize_detail;
  look_upsample = SanitizeEnumKey(
      saved_look.upsample, defaults::kLook.upsample, 0u, 1u, "NRLookUpsample");
  enabled = saved_enabled;
  nr_before_upscale = saved_pre_upscale;
  codec_mode = SanitizeEnumKey(
      saved_codec_mode, defaults::kCodecMode, 0u, 3u, "NRCodecMode");
  proxy_anchor_nits = SanitizeFloatKey(
      saved_anchor, defaults::kProxyAnchorNits, 0.5f, 32.f, "NRProxyAnchor");
  preset = SanitizeEnumKey(saved_preset, defaults::kPreset, 0u, 3u, "NRPreset");
  style = SanitizeEnumKey(saved_style, defaults::kStyle, 0u, 2u, "NRStyle");
  intensity = SanitizeFloatKey(
      saved_intensity, defaults::kIntensity, 0.f, 2.f, "NRIntensity");
  local_tone_strength = SanitizeFloatKey(
      saved_local_tone, defaults::kLocalTone, 0.f, 2.f, "NRLocalTone");
  local_structure_strength = SanitizeFloatKey(
      saved_local_structure, defaults::kLocalStructure, 0.f, 2.f,
      "NRLocalStructure");
  skin_structure_strength = SanitizeFloatKey(
      saved_skin_structure, defaults::kSkinStructure, -1.f, 1.f,
      "NRSkinStructure");
  skin_independent = saved_skin_independent;
  use_auto_mask = saved_auto_mask;
  ui_correction = saved_ui_correction;
  depth_mode = SanitizeEnumKey(
      saved_depth_mode, defaults::kDepthMode, 0u, 2u, "NRDepthMode");
  motion_scale_x_multiplier = SanitizeFloatKey(
      saved_motion_x, defaults::kMvecScale, -2.f, 2.f, "NRMVecScaleX");
  motion_scale_y_multiplier = SanitizeFloatKey(
      saved_motion_y, defaults::kMvecScale, -2.f, 2.f, "NRMVecScaleY");
  paper_white_scale = SanitizeFloatKey(
      saved_paper_white, defaults::kPaperWhiteScale, 0.005f, 16.f,
      "NRPaperWhiteScale");
  diffuse_white_nits = SanitizeFloatKey(
      saved_diffuse_white, defaults::kDiffuseWhiteNits, 80.f, 1000.f,
      "NRDiffuseWhiteNits");
  pq_calibration = SanitizeFloatKey(
      saved_pq_calibration, defaults::kPqCalibration, 0.005f, 16.f,
      "NRPQCalibration");
  source_encoding = SanitizeEnumKey(
      saved_source_encoding, defaults::kSourceEncoding, 0u, 3u,
      "NRSourceEncoding");
  // Primaries only feeds the interpretation readout; the enum guard keeps a
  // corrupted line from drifting the label.
  source_primaries = SanitizeEnumKey(
      saved_source_primaries, defaults::kSourcePrimaries, 0u, 3u,
      "NRSourcePrimaries");
  linear_unit_nits = SanitizeFloatKey(
      saved_linear_unit_nits, defaults::kLinearUnitNits, 0.f, 10000.f,
      "NRLinearUnitNits");
  transfer_strength = SanitizeFloatKey(
      saved_transfer, defaults::kTransferStrength, 0.f, 1.f,
      "NRTransferStrength");
  color_strength = SanitizeFloatKey(
      saved_color, defaults::kColorStrength, 0.f, 1.f, "NRColorStrength");
  chroma_clamp_stops = SanitizeFloatKey(
      saved_chroma_clamp, defaults::kChromaClampStops, 0.25f, 2.f,
      "NRChromaClamp");
  transfer_mode = SanitizeEnumKey(
      saved_transfer_mode, defaults::kTransferMode, 0u, 1u, "NRTransferMode");
  display_pedestal = SanitizeEnumKey(
      saved_display_pedestal, defaults::kDisplayPedestal, 0u, 1u, "NRDisplayPedestal");
  dedupe_mode = SanitizeEnumKey(
      saved_dedupe_mode, defaults::kDedupeMode, 0u, 1u, "DedupeMode");
  list_state_mode.store(static_cast<int>(SanitizeEnumKey(
      saved_list_state_mode, defaults::kListStateMode, 0u, 1u,
      "ListStateMode")));
  feed_mode = SanitizeEnumKey(
      saved_feed_mode, defaults::kFeedMode, 0u, 2u, "NRFeedMode");
  norm_governor = SanitizeEnumKey(
      saved_norm_governor, defaults::kNormGovernor, 0u, 2u, "NRNormGovernor");
  // 0 is meaningful here (a zero limit is "adopt the candidate"), so the
  // range starts at 0 rather than at a minimum useful slew.
  norm_slew_stops = SanitizeFloatKey(
      saved_norm_slew, defaults::kNormSlewStops, 0.f, 0.5f, "NRNormSlewStops");
  norm_attack_stops = SanitizeFloatKey(
      saved_norm_attack, defaults::kNormAttackStops, 0.1f, 64.f,
      "NRNormAttackStops");
  norm_release_stops = SanitizeFloatKey(
      saved_norm_release, defaults::kNormReleaseStops, 0.05f, 16.f,
      "NRNormReleaseStops");
  chained_temporal_history = saved_chained_history;
  neural_floor_guard = saved_neural_floor_guard;
  toggle_hotkey =
      saved_toggle_key >= 0x08 && saved_toggle_key <= 0xFE ? saved_toggle_key : VK_F6;
  screenshot_hotkey = saved_screenshot_key >= 0x08 && saved_screenshot_key <= 0xFE
      ? saved_screenshot_key
      : VK_F5;
  // Stamp the schema marker.  A migrated config persists its (possibly
  // rewritten) values coherently with the new marker so a later incremental
  // widget save cannot resurrect pre-migration values around it.  A
  // FUTURE-version file is never rewritten: no stamp, no persist - the
  // newer schema owns the file and this build only reads what it knows.
  if (future_config) return;
  if (migrate_config) {
    PersistConfig();
  } else {
    reshade::set_config_value(nullptr, kConfigSection, "ConfigVersion", kConfigVersion);
  }
}

inline bool Shutdown(bool proof_gated);

// A game can rebuild its D3D12 device (alt-tab driver resets, fullscreen
// toggles).  Only tear the addon lifecycle down when the device the signed NR
// runtime was initialized on is the one going away; every resource, feature
// handle, and hook re-arms lazily afterwards.
inline void OnDestroyDevice(reshade::api::device* device) {
  // The bridge releasing its private device's ReShade proxy.
  if (bridge_releasing_device) return;
  if (shutting_down.load(std::memory_order_acquire)) return;
  if (device == nullptr) return;
  destroy_device_events.fetch_add(1, std::memory_order_relaxed);
  const bool is_d3d12 = device->get_api() == reshade::api::device_api::d3d12;
  if (is_d3d12) {
    destroy_device_d3d12_events.fetch_add(1, std::memory_order_relaxed);
  }
  // The counters above ride the telemetry line, which is emitted on a
  // PRESENT - and device teardown happens after the last one, so a session
  // that ends normally never prints them.  One line here, once, is the only
  // way a log can say the event arrived at all.
  const auto tracked = reinterpret_cast<uint64_t>(
      direct_device_native != nullptr ? direct_device_native
          : direct_device != nullptr ? direct_device
                                     : last_nr_device_native);
  static std::atomic_bool logged_first_destroy{false};
  if (!logged_first_destroy.exchange(true)) {
    std::ostringstream message;
    message << "first destroy_device observed: api="
            << (is_d3d12 ? "d3d12" : "other") << " native=0x" << std::hex
            << static_cast<uint64_t>(device->get_native()) << " tracked=0x"
            << tracked;
    Log(reshade::log::level::info, message.str());
  }
  if (!is_d3d12) {
    // NR's tracked device is always a D3D12 one, so a D3D11 device is never
    // compared with it (alpha45 did, and a D3D11 device born at a released
    // one's address "matched").  The D3D11 device that counts is the one the
    // bridge serves, by native identity: the backstop for a device the game
    // rebuilds without destroying its swapchain first.
    if (device->get_api() == reshade::api::device_api::d3d11
        && bridge_device_live.load(std::memory_order_acquire)
        && renodx::utils::directx::NativeIdentity(
               reinterpret_cast<IUnknown*>(device->get_native()))
               == bridge_device11_identity.load(std::memory_order_acquire)) {
      destroy_device_matches.fetch_add(1, std::memory_order_relaxed);
      Log(reshade::log::level::info,
          "the D3D11 device the bridge serves is being destroyed; releasing NR"
          " state for re-arm (Direct3D 11 bridge)");
      Shutdown();
      RuntimeLock lock(runtime_mutex);
      screenshot::AbortPending();
      created_feature_ids.clear();
      create_contracts.clear();
    }
    return;
  }
  if (tracked == 0) {
    // The session's last word for a session that never armed NR.
    //
    // Shutdown() states it otherwise, and Shutdown() is unreachable from
    // here: both of its callers first require the device NR was initialized
    // on, and there is no such device.  So the sessions that most need
    // explaining were the ones that explained themselves least - the verdict
    // is emitted on CHANGE, the funnel settles within two presents when no
    // DLSS feature is ever created, and the 30 s "settled" line only arms
    // while NR is enabled.  Measured 2026-09-20 on the T2 `streamline_only`
    // lane: 240 presents, and the last verdict in the log said presents=2.
    // A reporter pasting that log showed an addon that looked dead on
    // arrival, in exactly the shape (PLAN_REHAB_V7.md 10.1, G2-G4) where the
    // difference between "never ticked" and "ticked 900 times and was never
    // asked to do anything" is the whole diagnosis.
    //
    // One line, at most once, and only for D3D12: this is the API the addon
    // is for, and a game that spins up a D3D11 device for its launcher must
    // not spend the session's one final verdict on it.
    static std::atomic_bool emitted_final_verdict{false};
    if (is_d3d12 && !emitted_final_verdict.exchange(true)) {
      // Same flush as Shutdown()'s, for the sessions Shutdown() cannot
      // reach - which, as the comment above says, is exactly the set this
      // branch exists for.  The telemetry line carries everything the
      // verdict does not, including the counters five lines up and the
      // hook entry tallies, and without this a session shorter than one
      // telemetry window states none of it.  That is not hypothetical: the
      // `streamline_only` lane emitted a telemetry line in some runs and
      // not others, so every harness assertion gated on one was silently
      // skipped at random.  Before the verdict, because EmitTelemetry ends
      // by stating its own `interval` verdict and the log's last verdict
      // has to be the final one.
      {
        RuntimeLock lock(runtime_mutex);
        EmitTelemetry(SteadyNowNs(), /*force=*/true);
      }
      EmitNrVerdict("teardown");
    }
    return;
  }
  const auto native = static_cast<uint64_t>(device->get_native());
  if (native != tracked) {
    // Games create several devices, so one that is not ours is normal and
    // this stays one-shot and quiet.  What is NOT normal is every D3D12
    // teardown of a session missing while NR is initialized, which is what
    // the counters above make visible.
    static std::atomic_bool logged_mismatch{false};
    if (is_d3d12 && !logged_mismatch.exchange(true)) {
      std::ostringstream message;
      message << "a D3D12 device was destroyed that is not the one NR was initialized on (destroyed 0x" << std::hex << native
              << ", tracked 0x" << tracked << ")";
      Log(reshade::log::level::info, message.str());
    }
    return;
  }
  destroy_device_matches.fetch_add(1, std::memory_order_relaxed);
  {
    RuntimeLock lock(runtime_mutex);
    created_feature_ids.clear();
    create_contracts.clear();
  }
  if (direct_device == nullptr) {
    // Already released, by the swapchain teardown below: this event is the
    // confirmation that the device really did go away afterwards, which is
    // the invariant the e2e exit check reads.  Nothing left to do.
    Log(reshade::log::level::info,
        "the device NR was initialized on has been destroyed; NR state was"
        " already released");
    return;
  }
  Log(reshade::log::level::info,
      "tracked D3D12 device is being destroyed; releasing NR state for re-arm");
  Shutdown();
  RuntimeLock lock(runtime_mutex);
  screenshot::AbortPending();
}

// The teardown trigger.
//
// destroy_device cannot be it: ReShade fires that from its device object's
// destructor, which cannot run while NGX holds a reference to the device it
// was initialized on, and the only thing that drops that reference is
// Shutdown() - the function destroy_device would have called.  Measured: no
// destroy_device in 242 of 242 engaged runs across every build of this line,
// against 37 of 37 for an upstream build that initializes no runtime.
//
// destroy_swapchain has no such cycle, and ReShade invokes it from
// DXGISwapChain::on_reset right after the "Destroyed runtime environment"
// line that used to be the last thing an engaged session ever logged.
// `resize` is false only when the swapchain is really going away, so a
// ResizeBuffers (which keeps the device) costs nothing.
//
// The release waits for the submission tracker's proof (v7.0.0-rc9).
// ReShade raises this event from DXGISwapChain::on_reset while it holds the
// present queue's lock (D3D12CommandQueue::_mutex,
// external/reshade/source/dxgi/dxgi_swapchain.cpp:933), and its
// ExecuteCommandLists takes that same lock before forwarding
// (d3d12_command_queue.cpp:182).  A game that submits from its own thread -
// Unreal Engine's RHI thread - can therefore be parked inside a submit that
// carries an evaluate with NR work while this callback runs, and no wait in
// here can ever see that submission.  From alpha12 to rc8 this released NR
// state on the spot (with a 1 s wait that covered only work already
// submitted); the parked list then ran against freed features, heaps and
// pipelines: a GPU page fault, a TDR the driver could not recover from, a
// PC that needed its power button.  Now the destroy only marks the teardown
// pending, and the release runs when every recording carrying NR work was
// submitted and has completed - at once when that is already true (a game
// that idled the GPU first), otherwise on the next present or queue destroy.
// An evaluate on the same device in between resumes NR on the state it has
// (NrWaitsForTeardown).  Without the proof - no queue detour - the state is
// kept, as v6 always did.
//
// Known limitation: NR is device-scoped, and so is the match here - a game
// that destroys a SECOND swapchain on the same device tears NR down with it
// and re-arms on the next evaluate, which costs a hitch rather than the
// session.  `swapchains=` / `swapchains_ours=` in the telemetry line are what
// a field log would show it by.
inline void OnDestroySwapchain(reshade::api::swapchain* swapchain, bool resize) {
  if (resize || swapchain == nullptr) return;
  if (shutting_down.load(std::memory_order_acquire)) return;
  destroy_swapchain_events.fetch_add(1, std::memory_order_relaxed);
  // The bridge can hold its private device before NR itself ever
  // initialized on it (no evaluate reached the runtime yet).
  const bool bridge_live = bridge_device_live.load(std::memory_order_acquire);
  if (direct_device == nullptr && !bridge_live) return;
  reshade::api::device* device = swapchain->get_device();
  // P4 A2 (row 25 gap 2, plan 4.3): the D3D11 branch, AHEAD of the d3d12
  // gate below (which early-returns on a D3D11 device).  In a foreign
  // session the swapchain that dies is the game's D3D11 one; NR state was
  // initialized on the foreign tool's D3D12 device, whose children pin it,
  // so the same teardown the D3D12 path gets runs here.  On the native
  // route it is the bridge's device, which pins the game's D3D11 device in
  // turn.  Scoped to the D3D11 shape: a D3D12 session's launcher-D3D11
  // swapchain changes nothing.
  if (device != nullptr
      && device->get_api() == reshade::api::device_api::d3d11) {
    if (bridge_live) {
      // Native route: only a swapchain of the device the bridge serves,
      // matched by native identity as the D3D12 branch below matches its
      // device.  alpha45 took any D3D11 swapchain (a video player's, a second
      // window's) as the game's, and ignored the game's own whenever
      // anything had presented through D3D12 - leaving a rebuilt device to
      // decline every evaluate as a "second immediate context".
      if (renodx::utils::directx::NativeIdentity(
              reinterpret_cast<IUnknown*>(device->get_native()))
          != bridge_device11_identity.load(std::memory_order_acquire)) {
        return;
      }
    } else if (!d3d11_present_seen.load(std::memory_order_relaxed)
               || d3d12_present_seen.load(std::memory_order_relaxed)) {
      // Foreign route: NR lives on the other tool's D3D12 device, so there
      // is no D3D11 identity to match; the session's shape decides.
      return;
    }
    destroy_swapchain_matches.fetch_add(1, std::memory_order_relaxed);
    Log(reshade::log::level::info,
        bridge_live
            ? "the D3D11 swapchain is being destroyed; releasing NR state for"
              " re-arm (Direct3D 11 bridge)"
            : "the D3D11 swapchain is being destroyed; releasing NR state for"
              " re-arm (foreign D3D12 source)");
    Shutdown();
    RuntimeLock lock(runtime_mutex);
    screenshot::AbortPending();
    return;
  }
  if (direct_device == nullptr) return;
  if (device == nullptr
      || device->get_api() != reshade::api::device_api::d3d12) {
    return;
  }
  const auto tracked = reinterpret_cast<uint64_t>(
      direct_device_native != nullptr ? direct_device_native : direct_device);
  if (static_cast<uint64_t>(device->get_native()) != tracked) return;
  destroy_swapchain_matches.fetch_add(1, std::memory_order_relaxed);
  teardown_pending_since_ns.store(SteadyNowNs(), std::memory_order_relaxed);
  teardown_pending.store(true, std::memory_order_release);
  if (ServicePendingTeardown()) return;
  Log(reshade::log::level::info,
      "the swapchain on the device NR was initialized on is being destroyed"
      " while recorded NR work has not completed (a game thread can still"
      " submit it); NR state is released once it has, and an evaluate on the"
      " same device resumes NR instead");
}

// Memory-ownership teardown shared by every detach flavor: remove the NGX
// slot detours and the Streamline detours and reset the dispatch pointers.
// Pure instruction-patch work in mapped modules - no NVIDIA calls, no locks
// beyond the caller's.  The command-list, device and queue detours are not
// in this set: they patch D3D12 runtime code that outlives any device, game
// threads call them throughout a teardown, and they stay installed across
// device rebuilds and come off in DetachLite only.
inline void UnhookInstalledDetours() {
  UnhookNgx11Detours();
  if (streamline_hooks_installed.load(std::memory_order_relaxed)) {
    if (hooked_streamline_module != nullptr) {
      renodx::utils::vtable::Unhook(hooked_streamline_module, kStreamlineHooks);
    }
    streamline_hooks_installed.store(false, std::memory_order_relaxed);
    hooked_streamline_module = nullptr;
    real_streamline_evaluate = nullptr;
    real_streamline_set_tag = nullptr;
    real_streamline_set_tag_for_frame = nullptr;
  }
  if (ngx_any_hooked.load(std::memory_order_relaxed)) {
    for (int s = 0; s < kMaxNgxSlots; ++s) {
      if (!ngx_slot_used[s]) continue;
      auto items = MakeNgxHookItems(s);
      renodx::utils::vtable::Unhook(ngx_slot_module[s], items);
    }
    for (int s = 0; s < kMaxNgxSlots; ++s) {
      ngx_slot_used[s] = false;
      ngx_slot_module[s] = nullptr;
      ngx_slot_real[s] = {};
      ngx_slot_noncore[s].store(false, std::memory_order_relaxed);
    }
    ngx_hooked_modules.clear();
    ngx_failed_modules.clear();
    ngx_any_hooked.store(false, std::memory_order_relaxed);
    hooks_installed = false;
    hooked_ngx_module = nullptr;
  }
}

// DLL_PROCESS_DETACH teardown.  The loader lock is held here, and on process
// termination every other thread - the game's, ReShade's, and the NVIDIA
// runtimes' worker pools - is already gone.  Calling into nvngx (feature
// release, Shutdown1) or FreeLibrary from this context is a known hang and
// exit-crash source, so this path only unmaps the detours and restores the
// caller-identity patch; the OS reclaims every handle, GPU object, module
// reference, and NR feature on its own.  A mid-session ReShade addon reload
// (the boot probe cycles FreeLibrary the addon too) gets the same treatment:
// detours must die with the DLL that owns them.
inline void DetachLite() {
  shutting_down.store(true, std::memory_order_release);
  UnhookCommandListStateHooks();
  UnhookDeviceStateHooks();
  UnhookQueueCompletionHooks();
  UnhookInstalledDetours();
  RestoreCallerIdentity();
  // Flag the screenshot worker without touching its mutexes: the worker may
  // be mid-job holding them, and blocking here would stall the loader lock.
  screenshot::SignalShutdown();
}

// Device-rebuild teardown: live threads, no loader lock, so the full release
// sequence runs here.  Process detach uses DetachLite instead.  Everything
// re-arms lazily afterwards - `shutting_down` is lowered again at the end of
// this function, the hooks reinstall on the next present and the signed
// runtime reloads on the next evaluate.  The command-list detours and their
// shadow, the device detour and the queue detour stay installed throughout
// (see UnhookInstalledDetours).
//
// Reached from OnDestroySwapchain, and from OnDestroyDevice only when the
// swapchain teardown has not already run: while NR is engaged, NGX holds a
// reference to the device, so ReShade's device object cannot reach a
// refcount of zero and destroy_device cannot fire until after this has run.
//
// `proof_gated` is the swapchain teardown (ServicePendingTeardown): after the
// callback drain below no new NR work can be recorded, so the proof is
// checked again there, and a failed check - an evaluate that was in flight
// when the caller looked, or one that resumed on the same device - lowers
// `shutting_down` and releases nothing.  Returns whether it released.
inline bool Shutdown(bool proof_gated) {
  // One teardown at a time.  A swapchain teardown that finds another one
  // running leaves it to that one (and the next present retries); any other
  // caller is a device or feature going away, so it waits for the owner and
  // then runs - on state the owner either released or kept.  No caller holds
  // runtime_mutex or a CallbackScope here, so the owner never waits on it.
  for (bool owned = false;
       !teardown_owned.compare_exchange_strong(owned, true,
                                               std::memory_order_acq_rel);
       owned = false) {
    if (proof_gated) return false;
    Sleep(1);
  }
  // Released on every exit, a throw included: a waiter spins on it.
  struct OwnershipRelease {
    ~OwnershipRelease() {
      teardown_owned.store(false, std::memory_order_release);
    }
  } ownership_release;
  // Refuse new hook work first, then let in-flight callbacks leave addon state
  // before anything is released.  The wait is bounded by the clock, not by a
  // count of Sleep(1)s: at the default 15.6 ms timer resolution 500 of those
  // were 7.8 s, and a swapchain teardown runs under ReShade's lock on the
  // present queue.  A swapchain teardown that still sees a callback inside
  // addon state releases nothing and is retried; any other caller proceeds
  // under runtime_mutex, which still serializes against a slow callback,
  // where an unbounded spin could hang the game at exit.
  shutting_down.store(true, std::memory_order_seq_cst);
  const int64_t drain_deadline_ns = SteadyNowNs() + 500 * 1000000LL;
  while (callbacks_in_flight.load(std::memory_order_seq_cst) != 0
         && SteadyNowNs() < drain_deadline_ns) {
    Sleep(1);
  }
  const bool drained = callbacks_in_flight.load(std::memory_order_seq_cst) == 0;
  if (proof_gated
      && (!drained || !teardown_pending.load(std::memory_order_acquire)
          || !TeardownProofHolds())) {
    shutting_down.store(false, std::memory_order_release);
    return false;
  }
  if (!drained) {
    Log(reshade::log::level::warning,
        "a hook callback was still inside NR state 500 ms into the teardown;"
        " releasing under the runtime lock");
  }
  if (teardown_pending.exchange(false, std::memory_order_acq_rel)
      && proof_gated) {
    const int64_t waited_ms =
        (SteadyNowNs()
         - teardown_pending_since_ns.load(std::memory_order_relaxed))
        / 1000000;
    Log(reshade::log::level::info,
        "the swapchain on the device NR was initialized on was destroyed and"
        " every recording that carries NR work has completed ("
            + std::to_string(waited_ms)
            + " ms after the destroy); releasing NR state for re-arm");
  }
  if (lastgasp::dumps_on.load()) lastgasp::Phase("shutdown:begin", true);
  debug::Mark("shutdown:begin");
  // BEFORE the shutdown verdict, not after: EmitTelemetry ends by stating
  // its own `trigger=interval` verdict, and the last verdict in the log has
  // to be the final one.
  {
    RuntimeLock lock(runtime_mutex);
    EmitTelemetry(SteadyNowNs(), /*force=*/true);
  }
  // The session's LAST word, before anything is torn down.  The periodic
  // verdict covers the sessions that never reach here (a crash, an Alt-F4),
  // but where the addon does shut down cleanly the log used to end on a
  // line up to NRTelemetrySeconds old, and a reader had no way to tell a
  // stale sample from a final total.  Measured 2026-09-20 on the T2
  // `late_ngx` lane: the host ran 240 evaluates and the last line in the
  // log said seen=197, because the run ended mid-interval - 43 frames that
  // no reader, and no test, could account for.
  EmitNrVerdict("shutdown");
  // And the card that verdict shows, outside the rate limit: the lanes read
  // it as the session's last card.
  {
    const int64_t now_ns = SteadyNowNs();
    EmitStatusCard(
        ui::ComputeStatusCard(CollectCardInputs(
            ComputeNrVerdict(CollectFunnelInputs()), now_ns)),
        "shutdown");
  }
  const uint64_t session_bypassed =
      bypassed_evaluations.load(std::memory_order_relaxed);
  if (session_bypassed != 0) {
    Log(
        reshade::log::level::info,
        "NR session engagement: bypassed (exact passthrough) frames x "
            + std::to_string(session_bypassed));
  }
  // Session decline totals (NrDeclineReason): one line per reason that
  // actually fired, so a user report names the gate that ate the frames
  // instead of "NR just did nothing".
  for (size_t i = 0; i < static_cast<size_t>(NrDeclineReason::kCount); ++i) {
    const uint64_t total =
        nr_decline_counts[i].load(std::memory_order_relaxed);
    if (total != 0) {
      Log(reshade::log::level::info,
          std::string("NR session declines: ") + kNrDeclineNames[i] + " x "
              + std::to_string(total));
    }
  }
  if (const uint64_t unproven =
          nr_runtime_reinit_unproven.load(std::memory_order_relaxed)) {
    Log(reshade::log::level::warning,
        "NR session re-initializations with recorded NR work not proven"
        " complete x " + std::to_string(unproven));
  }
  // No bridged evaluate can start now, and the ones that ran are drained
  // off the GPU before their worksets and features are released below.
  DrainBridgeQueue();
  // Features and worksets are released below without a retirement proof, and
  // a swapchain rebuilt on the SAME device (a fullscreen toggle done by
  // recreate) keeps that GPU running.  So the work already submitted on every
  // tracked queue is waited for first, bounded per queue.
  if (!WaitForTrackedQueues()) {
    Log(reshade::log::level::warning,
        "teardown: a tracked queue had not finished its submitted work"
        " after 1 s; NR state is released anyway");
  }
  // The runtime is unmapped AFTER this scope: FreeLibrary takes the Windows
  // loader lock, and runtime_mutex must not be held across it
  // (runtime_lock.hpp).  `shutting_down` stays raised until the unmap is
  // done, so no thread can adopt the module while it is going away.
  HMODULE unload = nullptr;
  {
    RuntimeLock lock(runtime_mutex);
    UnhookInstalledDetours();
    // The queue detour stays installed; retirement falls back to the lease
    // proof until the next present re-arms it (InstallQueueCompletionHooks).
    queue_tracking_active.store(false, std::memory_order_release);
    ReleaseFeatureStates();
    // Captured before the teardown below clears direct_device.
    const bool runtime_was_initialized = direct_device != nullptr;
    // Device rebuild: the readbacks belong to the dying device, but the
    // encoding worker must survive for the next capture.
    screenshot::AbortPending();
    ReleaseAllWorksets();
    FlushRetiredResources();
    ReleaseCodecPipeline();
    gpu_timers::ReleaseAll();
    norm_trace::ReleaseAll();
    edit_trace::ReleaseAll();
    telemetry::ReleaseAdapter();
    telemetry_probe_lease = GpuLease{};
    telemetry_probe_taken = false;
    if (direct_device != nullptr && direct_api.shutdown != nullptr) {
      direct_api.shutdown(direct_device);
      direct_device->Release();
      direct_device = nullptr;
      direct_device_native = nullptr;
    }
    RestoreCallerIdentity();
    if (direct_api.module != nullptr) {
      if (runtime_was_initialized) {
        unload = direct_api.module;
      } else {
        // The runtime was loaded but never initialized: boot probe devices can
        // load and destroy a device within milliseconds (seen on Dead Space
        // 2023).  Unloading an NVIDIA runtime that young is a known
        // delayed-crash source when its attach cycle spawned threads, so keep
        // it mapped for the process lifetime; a later load just bumps the
        // module refcount.
        Log(reshade::log::level::info,
            "nvngx_dlssnr.dll was loaded but never initialized; leaving it mapped");
      }
    }
    direct_api = {};
    ngx_core_module = nullptr;
    parameter_runtime_ready = false;
    parameter_runtime_via_core = false;
    core_allocate_parameters = nullptr;
    core_destroy_parameters = nullptr;
    core_create_feature = nullptr;
    core_evaluate_feature = nullptr;
    core_release_feature = nullptr;
    direct_load_state = DirectLoadState::Unknown;
    processed_outputs.clear();
    synthetic_handles.clear();
    streamline_viewports.clear();
    streamline_handle_keys.clear();
    direct_runtime_sha256.clear();
    direct_runtime_reference_match = false;
    // Fences last: every lease holder was flushed above, so nothing can query
    // them after this point.  Submission-use bookkeeping goes with them - the
    // device generation it described is gone, and stale pointers must not leak
    // into the next generation's tracking.  Bookkeeping first: its reset
    // disarms the queue detour, which stays installed, so a submit racing
    // this teardown does not put a fresh fence into the emptied map.
    submission::ResetAll();
    ReleaseQueueCompletions();
  }
  // Outside runtime_mutex, still refusing new work: drop the resolved symbols
  // and unmap the runtime if this session owns the reference.
  ReleaseNgxLoaderSymbols(unload);
  // The bridge's private device goes after the NR runtime that was
  // initialized on it.
  ReleaseBridge();
  // The session's last word on lock order, AFTER the unmap - the one loader
  // call the periodic telemetry line can never cover, because it happens
  // during teardown.  Same fields, same owner, so the lane that parses the
  // telemetry line parses this one too.
  Log(reshade::log::level::info, LockOrderReport());
  // A device-rebuild shutdown re-arms lazily (hooks reinstall on the next
  // present, the signed runtime reloads on the next evaluate); the process
  // itself is ended only by DetachLite, which never releases addon state.
  shutting_down.store(false, std::memory_order_release);
  return true;
}

}  // namespace internal

inline void Use(DWORD reason, HMODULE module, LPVOID reserved) {
  // Windows runs every one of these arms holding the loader lock.  See
  // runtime_lock.hpp for which calls that makes illegal and which it does
  // not: the Detours transactions below are fine, the Tool Help scan is
  // not, and the difference is counted rather than trusted.
  runtime_lock::DllMainScope dll_main;
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      internal::addon_module = module;
      debug::Init(module);
      debug::Mark("attach:begin");
      internal::LoadConfiguration();
      if (lastgasp::dumps_on.load(std::memory_order_relaxed)) {
        lastgasp::Install(module, internal::kAddonVersion);
        lastgasp::Mark("attach:config-loaded");
      }
      if (internal::InitIssueReportTest()) {
        reshade::register_event<reshade::addon_event::reshade_present>(internal::OnIssueReportTestPresent);
      }
      debug::Mark("attach:config-loaded");
      internal::Log(
          reshade::log::level::info,
          std::string("DLSS5 active settings: intensity=")
              + std::to_string(internal::intensity.load())
              + " color_strength=" + std::to_string(internal::color_strength.load())
              + " transfer=" + std::to_string(internal::transfer_strength.load())
              + " paper_white=" + std::to_string(internal::paper_white_scale.load())
              + " preset=" + std::to_string(internal::preset.load())
              + " style=" + std::to_string(internal::style.load())
              + " enabled=" + (internal::enabled.load() ? "ON" : "OFF"));
      if (!internal::enabled.load()) {
        internal::Log(
            reshade::log::level::info,
            "NR is currently OFF. To turn it on: open the ReShade overlay,"
            " go to Add-ons -> DLSS 5 Neural Rendering, and tick 'Enable DLSS"
            " Neural Rendering' (or press the NR toggle hotkey in gameplay).");
      }
      internal::Log(
          reshade::log::level::info,
          std::string("RenoDX DLSS5 Generic ") + internal::kAddonVersion
              + " (build " __DATE__ " " __TIME__ ") loaded (hotkeys: NR toggle "
              + internal::VirtualKeyName(internal::toggle_hotkey.load())
              + ", screenshot "
              + internal::VirtualKeyName(internal::screenshot_hotkey.load()) + ")"
              + " | ListStateMode="
              + std::to_string(list_state_mode.load())
              + " DedupeMode=" + std::to_string(internal::dedupe_mode.load())
              + (internal::hooks_enabled.load()
                     ? (internal::streamline_hooks_enabled.load()
                            ? ""
                            : " | EnableHooks=2: NGX hooks only, "
                              "Streamline modules left unpatched")
                     : " | SAFE MODE: EnableHooks=0, all hooks off (no NR)"));
      internal::Log(
          reshade::log::level::info,
          "For support, share this ReShade.log and a screenshot of the addon's"
          " overlay status text.");
      reshade::register_event<reshade::addon_event::init_device>(internal::OnInitDevice);
      reshade::register_event<reshade::addon_event::destroy_device>(internal::OnDestroyDevice);
      reshade::register_event<reshade::addon_event::init_swapchain>(internal::OnInitSwapchain);
      reshade::register_event<reshade::addon_event::destroy_swapchain>(internal::OnDestroySwapchain);
      reshade::register_event<reshade::addon_event::present>(internal::OnPresent);
      reshade::register_event<reshade::addon_event::destroy_command_list>(
          internal::OnDestroyCommandListEvent);
      reshade::register_event<reshade::addon_event::destroy_command_queue>(
          internal::OnDestroyCommandQueueEvent);
      reshade::register_overlay(internal::kOverlayTitle, internal::OnOverlay);
      reshade::register_event<reshade::addon_event::reshade_open_overlay>(
          internal::OnReshadeOpenOverlay);
      if (internal::InitUiCapture()) {
        reshade::register_event<reshade::addon_event::reshade_overlay>(
            internal::OnUiCaptureOverlay);
        reshade::register_event<reshade::addon_event::reshade_present>(
            internal::OnUiCapturePresent);
      }
      internal::InstallHooks();
      debug::Mark("attach:hooks-installed");
      break;
    case DLL_PROCESS_DETACH:
      debug::Mark("detach:begin");
      debug::Shutdown();
      reshade::unregister_event<reshade::addon_event::init_device>(internal::OnInitDevice);
      reshade::unregister_event<reshade::addon_event::destroy_device>(internal::OnDestroyDevice);
      reshade::unregister_event<reshade::addon_event::init_swapchain>(internal::OnInitSwapchain);
      reshade::unregister_event<reshade::addon_event::destroy_swapchain>(internal::OnDestroySwapchain);
      reshade::unregister_event<reshade::addon_event::present>(internal::OnPresent);
      reshade::unregister_event<reshade::addon_event::destroy_command_list>(
          internal::OnDestroyCommandListEvent);
      reshade::unregister_event<reshade::addon_event::destroy_command_queue>(
          internal::OnDestroyCommandQueueEvent);
      reshade::unregister_overlay(internal::kOverlayTitle, internal::OnOverlay);
      reshade::unregister_event<reshade::addon_event::reshade_open_overlay>(
          internal::OnReshadeOpenOverlay);
      // The HUD's, the overlay hotkeys' and the window dock's listeners, if a
      // pill or toast was due or the overlay open at unload; removing one
      // that is not there does nothing.
      reshade::unregister_event<reshade::addon_event::reshade_overlay>(
          internal::OnHudOverlay);
      reshade::unregister_event<reshade::addon_event::reshade_overlay>(
          internal::OnOverlayHotkeys);
      reshade::unregister_event<reshade::addon_event::reshade_overlay>(
          internal::OnDockOverlayWindow);
      if (!internal::issue_report::test_reason.empty()) {
        reshade::unregister_event<reshade::addon_event::reshade_present>(
            internal::OnIssueReportTestPresent);
      }
      if (!internal::ui_capture_dir.empty()) {
        reshade::unregister_event<reshade::addon_event::reshade_overlay>(
            internal::OnUiCaptureOverlay);
        reshade::unregister_event<reshade::addon_event::reshade_present>(
            internal::OnUiCapturePresent);
      }
      // Both detach flavors (FreeLibrary unload and process termination) run
      // under the loader lock; see DetachLite for why the full teardown must
      // never run here.  Device rebuilds go through OnDestroyDevice, which
      // has live threads and no loader lock and uses the full Shutdown.
      internal::DetachLite();
      // FreeLibrary unload only (process termination frees nothing): D3D12
      // must not keep pointers into this module's vtable, and the shadow's
      // references die with the module.
      if (reserved == nullptr) ClearListShadowAtTeardown();
      break;
  }
}

}  // namespace renodx::addons::dlss5

#undef NVSDK_NGX_Parameter_SetF
#undef NVSDK_NGX_Parameter_SetUI
#undef NVSDK_NGX_Parameter_SetI
#undef NVSDK_NGX_Parameter_GetF
#undef NVSDK_NGX_Parameter_GetUI
#undef NVSDK_NGX_Parameter_GetI
#undef DLSS5_NGX_SET_F
#undef DLSS5_NGX_SET_UI
#undef DLSS5_NGX_SET_I
#undef DLSS5_NGX_GET_F
#undef DLSS5_NGX_GET_UI
#undef DLSS5_NGX_GET_I
