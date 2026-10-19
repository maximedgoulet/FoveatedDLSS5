/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The Direct3D 11 bridge: Path B, hybrid form (PLAN_DX11_V68.md 4.2, decided
// 2026-09-22; ledger PLAN_REHAB_V7.md 12 item 5).
//
// Included by dlssnr.hpp INSIDE namespace internal, just before InstallHooks,
// so it sees the whole D3D12 pipeline it feeds.  It is not a standalone
// header and must never be included anywhere else.
//
// Why a bridge at all.  The signed NR runtime has no D3D11 path: its D3D11
// init returns 0xBAD00001 (FeatureNotSupported, probe K5) and the NGX core's
// D3D11 create of feature 18 returns 0xBAD00012 (NotImplemented, K4).  So NR
// in a D3D11 game means a D3D12 device in the same process, and the frame
// crossing the API boundary through shared textures and shared fences.  That
// is forced by the runtime; the choices below are the ones that were open.
//
// What runs, per frame, inside the game's own D3D11 DLSS evaluate (after
// upscaling, the default):
//
//   1. the game's DLSS runs first, untouched (real());
//   2. its Output, MotionVectors and Depth are copied into shared twins on
//      the game's immediate context (depth with stencil goes through a
//      cs_5_0 pass, because R24G8/R32G8X24 cannot be shared - probe X1);
//   3. the context signals a shared fence and flushes;
//   4. an addon-owned D3D12 queue waits on it and executes one command list
//      that ProcessInline recorded - the SAME after-upscale pipeline D3D12
//      games get, reading the game's NGX block through bridge::ParameterView,
//      which answers the resource keys with the D3D12 twins;
//   5. the queue signals the return fence; the D3D11 context waits on it on
//      the GPU (no CPU wait anywhere) and copies the twin back over the
//      game's Output.
//
// Before upscaling (NRPreUpscale=1) the same handshake runs BEFORE real():
// the game's Color is copied into a twin instead of its Output, the list is
// ProcessInlinePreSR's (the D3D12 games' pre-SR pipeline) and ends by
// copying NR's result back into that twin, and after the GPU wait the game's
// Color key points at the twin for the duration of real() only
// (bridge::ScopedD3D11Redirect).  The game's DLSS then upscales NR's image.
//
// Both modes copy.  Redirecting the game's Output to a twin instead (probe
// X3, "direct mode") would save one copy each way, and is not built: on an
// idle GPU the whole bridge costs 0.2-0.25 ms per frame over NR itself (the
// 8-9 ms first quoted for alpha45 was measured under a GPU miner's load,
// which doubled NR's own time too), so the saving does not pay for a second
// way of writing into the game's DLSS block.  NR runs before the game's
// HUD, once per DLSS evaluate.  The game's DLSS is never replaced (the
// community bridge's route) - if anything here fails, the frame keeps the
// image the game's own DLSS produced.
//
// Ownership and lifetime:
//
//   - the private device is created on the D3D11 device's adapter.  D3D12
//     devices are per-adapter singletons, so a third-party tool's device on
//     the same adapter is the same object; ReShade wraps it and fires
//     init_device, which bridge_creating_device keeps this addon from
//     treating as a D3D12 game.  Every bridge object lives on the NATIVE
//     device, so ReShade never sees the bridge's queue, lists or resources;
//     the proxy is kept only so ReShade's own reference stays balanced, and
//     is released last;
//   - a ring of kBridgeRing allocator/list pairs, reused only once the
//     private done fence proves the GPU finished with them (else
//     bridge_busy - the frame keeps the game's image, nothing waits);
//   - a watchdog thread: a D3D12 submission that has not completed
//     kBridgeWatchdogNs after its input fence arrived has its return fence
//     CPU-signalled, so the game's D3D11 GPU never waits on a hung D3D12
//     queue for longer than that (2000 ms was the plan's first value and
//     equals the default TdrDelay; BridgeWatchdogMain says why the clock
//     starts at the input fence);
//   - teardown drains the queue before NR state is released and releases the
//     device after it (Shutdown).
//
// Every way a bridged evaluate can end without NR is a named decline
// (declines.hpp: bridge_down, bridge_format, bridge_busy, bridge_timeout,
// bridge_lost, d3d11_deferred), so T-SILENT holds on D3D11 as on D3D12.

#pragma once

// ---------------------------------------------------------------------------
// Storage: the D3D11 NGX detours
// ---------------------------------------------------------------------------

// One slot per detoured module copy, like the D3D12 family (each copy calls
// its own original - see ngx_slot_real), and separate from it: a module that
// exports both APIs is patched by two independent transactions.
inline NgxModuleReal ngx11_slot_real[kMaxNgxSlots];
inline bool ngx11_slot_used[kMaxNgxSlots] = {};
inline HMODULE ngx11_slot_module[kMaxNgxSlots] = {};
inline std::unordered_map<HMODULE, NgxHookRetry> ngx11_failed_modules;
// Candidate copies that export no D3D11 evaluate at all (the DLSS plugin
// copies of some SDK versions): checked once, then skipped.
inline std::unordered_set<HMODULE> ngx11_absent_modules;
inline std::atomic_bool ngx11_any_hooked{false};
// Latched like ngx_ever_hooked, for the funnel and the telemetry gate.
inline std::atomic_bool ngx11_ever_hooked{false};
// Entry tallies, incremented as the first statement of every wrapper (the
// "installed is not entered" rule, see ngx_entry_create).
inline std::atomic_uint64_t ngx11_entry_create[kMaxNgxSlots];
inline std::atomic_uint64_t ngx11_entry_evaluate[kMaxNgxSlots];
inline std::atomic_uint64_t ngx11_entry_evaluate_c[kMaxNgxSlots];
inline std::atomic_uint64_t ngx11_entry_release[kMaxNgxSlots];
inline std::atomic_uint32_t logged_create11_ids{0};
inline std::atomic_uint32_t logged_create11_result_ids{0};
// A registered DLSS evaluate reached the bridge: the user hint's "DLSS was
// seen" for a session whose create slipped past the hooks.
inline std::atomic_bool ngx11_dlss_evaluate_seen{false};

// A plugin copy forwarding into the core copy re-enters the detour.  Only
// the outermost entry on a thread does any work; every inner one forwards.
inline thread_local uint32_t ngx11_call_depth = 0;
struct Ngx11Nesting {
  bool outermost;
  Ngx11Nesting() noexcept : outermost(ngx11_call_depth++ == 0) {}
  ~Ngx11Nesting() noexcept { --ngx11_call_depth; }
  Ngx11Nesting(const Ngx11Nesting&) = delete;
  Ngx11Nesting& operator=(const Ngx11Nesting&) = delete;
};

// The span of an ID3D11DeviceContext vtable this addon may read through the
// game's pointer (A-1): IUnknown 3 + ID3D11DeviceChild 4 + 108 methods.
inline constexpr std::size_t kD3D11ContextVtableBytes = 115 * sizeof(void*);

// ---------------------------------------------------------------------------
// Storage: the bridge
// ---------------------------------------------------------------------------

inline constexpr uint32_t kBridgeRing = 6;
inline constexpr int64_t kBridgeWatchdogNs = 500'000'000;
// The bridge is lost when the watchdog released kBridgeMaxTimeouts stalled
// submissions within kBridgeLostWindowNs: a stall rate, not a session total,
// so three unrelated hitches far apart do not switch NR off for the session.
inline constexpr uint32_t kBridgeMaxTimeouts = 3;
inline constexpr int64_t kBridgeLostWindowNs = 60'000'000'000;

enum class BridgeState : uint8_t { kDown = 0, kStarting, kUp, kLost };
enum class BridgeMotionTransportMode : uint32_t {
  kUnselected = 0,
  kDirect,
  kLosslessRgba32f,
  kReducedRgba16f,
};
enum class BridgeMotionTransportPreference : uint32_t {
  kAuto = 0,
  kDirect,
  kLossless,
  kFp16,
};
enum class BridgeMotionTransportStep : uint32_t {
  kNone = 0,
  kD3D11CheckSourceFormat,
  kD3D11CheckTransportFormat,
  kD3D11CreateSourceStage,
  kD3D11CreateSourceSrv,
  kD3D11CompileConversionShader,
  kD3D11CreateConversionShader,
  kD3D11CreateConversionStage,
  kD3D11CreateConversionUav,
  kD3D11CreateSharedTexture,
  kD3D11QuerySharedResource,
  kD3D11CreateSharedHandle,
  kD3D12OpenSharedHandle,
  kD3D12CheckTransportFormat,
  kD3D12CheckMotionFormat,
  kD3D12CreateMotionTexture,
  kD3D12CreateDescriptorHeap,
  kD3D12CompileReconstructionShader,
  kD3D12SerializeConversionRootSignature,
  kD3D12CreateConversionRootSignature,
  kD3D12CreateConversionPipeline,
};

struct BridgeMotionDiagnosticSnapshot {
  bool consistent = false;
  uint32_t source_format = DXGI_FORMAT_UNKNOWN;
  uint32_t last_failed_transport_format = DXGI_FORMAT_UNKNOWN;
  uint32_t last_failed_bind_flags = 0;
  BridgeMotionTransportStep failed_step = BridgeMotionTransportStep::kNone;
  uint32_t hresult = S_OK;
  uint32_t shared_resource_tier = 0;
  uint32_t shared_resource_tier_hresult = S_OK;
  BridgeMotionTransportMode selected_mode =
      BridgeMotionTransportMode::kUnselected;
  uint32_t selected_transport_format = DXGI_FORMAT_UNKNOWN;
  uint32_t selected_bind_flags = 0;
  uint32_t failure_count = 0;
};

// The bridge setter is serialized by runtime_mutex. Atomic fields plus an
// odd/even sequence let the issue-report worker read one consistent snapshot
// without taking runtime_mutex or probing any GPU object.
inline std::atomic_uint64_t bridge_motion_diagnostic_sequence{0};
inline std::atomic_uint32_t bridge_motion_source_format{DXGI_FORMAT_UNKNOWN};
inline std::atomic_uint32_t bridge_motion_failed_transport_format{DXGI_FORMAT_UNKNOWN};
inline std::atomic_uint32_t bridge_motion_failed_bind_flags{0};
inline std::atomic_uint32_t bridge_motion_failed_step{
    static_cast<uint32_t>(BridgeMotionTransportStep::kNone)};
inline std::atomic_uint32_t bridge_motion_failed_hresult{S_OK};
inline std::atomic_uint32_t bridge_motion_shared_resource_tier{0};
inline std::atomic_uint32_t bridge_motion_shared_resource_tier_hresult{S_OK};
inline std::atomic_uint32_t bridge_motion_selected_mode{
    static_cast<uint32_t>(BridgeMotionTransportMode::kUnselected)};
inline std::atomic_uint32_t bridge_motion_selected_transport_format{DXGI_FORMAT_UNKNOWN};
inline std::atomic_uint32_t bridge_motion_selected_bind_flags{0};
inline std::atomic_uint32_t bridge_motion_failure_count{0};

inline const char* BridgeMotionModeName(BridgeMotionTransportMode mode) {
  switch (mode) {
    case BridgeMotionTransportMode::kDirect: return "direct";
    case BridgeMotionTransportMode::kLosslessRgba32f: return "lossless_rgba32f";
    case BridgeMotionTransportMode::kReducedRgba16f: return "fp16_rgba16f";
    default: return "unselected";
  }
}

inline const char* BridgeMotionPreferenceName(
    BridgeMotionTransportPreference preference) {
  switch (preference) {
    case BridgeMotionTransportPreference::kDirect: return "direct";
    case BridgeMotionTransportPreference::kLossless: return "lossless";
    case BridgeMotionTransportPreference::kFp16: return "fp16";
    default: return "auto";
  }
}

inline const char* BridgeMotionStepName(BridgeMotionTransportStep step) {
  switch (step) {
    case BridgeMotionTransportStep::kD3D11CheckSourceFormat: return "D3D11.CheckFormatSupport(source)";
    case BridgeMotionTransportStep::kD3D11CheckTransportFormat: return "D3D11.CheckFormatSupport(transport)";
    case BridgeMotionTransportStep::kD3D11CreateSourceStage: return "D3D11.CreateTexture2D(source stage)";
    case BridgeMotionTransportStep::kD3D11CreateSourceSrv: return "D3D11.CreateShaderResourceView(source stage)";
    case BridgeMotionTransportStep::kD3D11CompileConversionShader: return "D3DCompile(D3D11 motion conversion)";
    case BridgeMotionTransportStep::kD3D11CreateConversionShader: return "D3D11.CreateComputeShader(motion conversion)";
    case BridgeMotionTransportStep::kD3D11CreateConversionStage: return "D3D11.CreateTexture2D(conversion UAV)";
    case BridgeMotionTransportStep::kD3D11CreateConversionUav: return "D3D11.CreateUnorderedAccessView(conversion stage)";
    case BridgeMotionTransportStep::kD3D11CreateSharedTexture: return "D3D11.CreateTexture2D(shared twin)";
    case BridgeMotionTransportStep::kD3D11QuerySharedResource: return "D3D11.QueryInterface(IDXGIResource1)";
    case BridgeMotionTransportStep::kD3D11CreateSharedHandle: return "IDXGIResource1.CreateSharedHandle";
    case BridgeMotionTransportStep::kD3D12OpenSharedHandle: return "D3D12.OpenSharedHandle";
    case BridgeMotionTransportStep::kD3D12CheckTransportFormat: return "D3D12.CheckFeatureSupport(transport SRV)";
    case BridgeMotionTransportStep::kD3D12CheckMotionFormat: return "D3D12.CheckFeatureSupport(R32G32_FLOAT UAV)";
    case BridgeMotionTransportStep::kD3D12CreateMotionTexture: return "D3D12.CreateCommittedResource(R32G32_FLOAT)";
    case BridgeMotionTransportStep::kD3D12CreateDescriptorHeap: return "D3D12.CreateDescriptorHeap(motion conversion)";
    case BridgeMotionTransportStep::kD3D12CompileReconstructionShader: return "D3DCompile(D3D12 motion reconstruction)";
    case BridgeMotionTransportStep::kD3D12SerializeConversionRootSignature: return "D3D12SerializeRootSignature(motion conversion)";
    case BridgeMotionTransportStep::kD3D12CreateConversionRootSignature: return "D3D12.CreateRootSignature(motion conversion)";
    case BridgeMotionTransportStep::kD3D12CreateConversionPipeline: return "D3D12.CreateComputePipelineState(motion conversion)";
    default: return "none";
  }
}

inline void BeginBridgeMotionDiagnostic(uint32_t source_format) {
  bridge_motion_diagnostic_sequence.fetch_add(1, std::memory_order_acq_rel);
  bridge_motion_source_format.store(source_format, std::memory_order_relaxed);
  bridge_motion_failed_transport_format.store(DXGI_FORMAT_UNKNOWN, std::memory_order_relaxed);
  bridge_motion_failed_bind_flags.store(0, std::memory_order_relaxed);
  bridge_motion_failed_step.store(
      static_cast<uint32_t>(BridgeMotionTransportStep::kNone),
      std::memory_order_relaxed);
  bridge_motion_failed_hresult.store(S_OK, std::memory_order_relaxed);
  bridge_motion_selected_mode.store(
      static_cast<uint32_t>(BridgeMotionTransportMode::kUnselected),
      std::memory_order_relaxed);
  bridge_motion_selected_transport_format.store(DXGI_FORMAT_UNKNOWN, std::memory_order_relaxed);
  bridge_motion_selected_bind_flags.store(0, std::memory_order_relaxed);
  bridge_motion_failure_count.store(0, std::memory_order_relaxed);
  bridge_motion_diagnostic_sequence.fetch_add(1, std::memory_order_release);
}

inline void RecordBridgeMotionFailure(
    uint32_t transport_format, uint32_t bind_flags,
    BridgeMotionTransportStep step, HRESULT hr) {
  bridge_motion_diagnostic_sequence.fetch_add(1, std::memory_order_acq_rel);
  bridge_motion_failed_transport_format.store(transport_format, std::memory_order_relaxed);
  bridge_motion_failed_bind_flags.store(bind_flags, std::memory_order_relaxed);
  bridge_motion_failed_step.store(static_cast<uint32_t>(step), std::memory_order_relaxed);
  bridge_motion_failed_hresult.store(static_cast<uint32_t>(hr), std::memory_order_relaxed);
  bridge_motion_failure_count.fetch_add(1, std::memory_order_relaxed);
  bridge_motion_diagnostic_sequence.fetch_add(1, std::memory_order_release);
}

inline void RecordBridgeMotionSharedResourceTier(uint32_t tier, HRESULT hr) {
  bridge_motion_diagnostic_sequence.fetch_add(1, std::memory_order_acq_rel);
  bridge_motion_shared_resource_tier.store(tier, std::memory_order_relaxed);
  bridge_motion_shared_resource_tier_hresult.store(static_cast<uint32_t>(hr), std::memory_order_relaxed);
  bridge_motion_diagnostic_sequence.fetch_add(1, std::memory_order_release);
}

inline void SelectBridgeMotionTransport(
    BridgeMotionTransportMode mode, uint32_t transport_format,
    uint32_t bind_flags) {
  bridge_motion_diagnostic_sequence.fetch_add(1, std::memory_order_acq_rel);
  bridge_motion_selected_mode.store(static_cast<uint32_t>(mode), std::memory_order_relaxed);
  bridge_motion_selected_transport_format.store(transport_format, std::memory_order_relaxed);
  bridge_motion_selected_bind_flags.store(bind_flags, std::memory_order_relaxed);
  bridge_motion_diagnostic_sequence.fetch_add(1, std::memory_order_release);
}

inline BridgeMotionDiagnosticSnapshot ReadBridgeMotionDiagnostic() {
  BridgeMotionDiagnosticSnapshot snapshot;
  for (uint32_t retry = 0; retry < 8; ++retry) {
    const uint64_t before =
        bridge_motion_diagnostic_sequence.load(std::memory_order_acquire);
    if ((before & 1u) != 0u) continue;
    snapshot.source_format = bridge_motion_source_format.load(std::memory_order_relaxed);
    snapshot.last_failed_transport_format =
        bridge_motion_failed_transport_format.load(std::memory_order_relaxed);
    snapshot.last_failed_bind_flags = bridge_motion_failed_bind_flags.load(std::memory_order_relaxed);
    snapshot.failed_step = static_cast<BridgeMotionTransportStep>(
        bridge_motion_failed_step.load(std::memory_order_relaxed));
    snapshot.hresult = bridge_motion_failed_hresult.load(std::memory_order_relaxed);
    snapshot.shared_resource_tier = bridge_motion_shared_resource_tier.load(std::memory_order_relaxed);
    snapshot.shared_resource_tier_hresult = bridge_motion_shared_resource_tier_hresult.load(std::memory_order_relaxed);
    snapshot.selected_mode = static_cast<BridgeMotionTransportMode>(
        bridge_motion_selected_mode.load(std::memory_order_relaxed));
    snapshot.selected_transport_format =
        bridge_motion_selected_transport_format.load(std::memory_order_relaxed);
    snapshot.selected_bind_flags = bridge_motion_selected_bind_flags.load(std::memory_order_relaxed);
    snapshot.failure_count = bridge_motion_failure_count.load(std::memory_order_relaxed);
    if (before == bridge_motion_diagnostic_sequence.load(std::memory_order_acquire)) {
      snapshot.consistent = true;
      return snapshot;
    }
  }
  return snapshot;
}

inline std::atomic_uint32_t bridge_motion_transport_preference{
    static_cast<uint32_t>(BridgeMotionTransportPreference::kAuto)};

inline const char* BridgeStateName(BridgeState state) {
  switch (state) {
    case BridgeState::kDown: return "down";
    case BridgeState::kStarting: return "starting";
    case BridgeState::kUp: return "up";
    case BridgeState::kLost: return "lost";
  }
  return "down";
}

struct BridgeSlot {
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  // The fence value of the last submission recorded on this pair.
  uint64_t value = 0;
};

struct Bridge {
  // The game's immediate context with ReShade's wrapper peeled off, for
  // identity only (every evaluate compares against it).
  ID3D11DeviceContext* context_native = nullptr;
  ID3D11DeviceContext4* context11 = nullptr;
  ID3D11Device* device11_base = nullptr;
  ID3D11Device5* device11 = nullptr;
  ID3D12Device* proxy12 = nullptr;
  ID3D12Device* device12 = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  BridgeSlot ring[kBridgeRing];
  // in: D3D11 -> D3D12 (the twins hold this frame's inputs); out: D3D12 ->
  // D3D11 (the output twin holds NR's result); done: private, the proof the
  // D3D12 work itself finished - `out` can be CPU-signalled by the
  // watchdog, `done` never is, so ring reuse and retirement key on it.
  ID3D12Fence* fence_in12 = nullptr;
  ID3D12Fence* fence_out12 = nullptr;
  ID3D12Fence* fence_done12 = nullptr;
  ID3D11Fence* fence_in11 = nullptr;
  ID3D11Fence* fence_out11 = nullptr;
  ID3D11ComputeShader* depth_cs = nullptr;
  ID3D11ComputeShader* motion_convert_cs11 = nullptr;
  ID3D12RootSignature* motion_convert_root12 = nullptr;
  ID3D12PipelineState* motion_convert_pso12 = nullptr;
  bool motion_conversion_pipeline_attempted = false;
  BridgeMotionTransportStep motion_conversion_pipeline_failed_step =
      BridgeMotionTransportStep::kNone;
  HRESULT motion_conversion_pipeline_hr = S_OK;
  HANDLE watchdog = nullptr;
};
inline Bridge bridge;

inline std::atomic<BridgeState> bridge_state{BridgeState::kDown};
// The bridge objects exist (state up or lost) - the swapchain teardown's
// "is there anything to release" question.
inline std::atomic_bool bridge_device_live{false};
// The native COM identity (utils::directx::NativeIdentity) of the game's
// D3D11 device while the bridge serves it: what the destroy_swapchain and
// destroy_device handlers match against, on whatever thread they run.  A
// value to compare, never dereferenced, so a teardown racing ReleaseBridge
// reads nothing that is being released.
inline std::atomic<IUnknown*> bridge_device11_identity{nullptr};
// Latched: the bridge came up at least once (the funnel's list_hooks_live
// rung on D3D11 - the bridge's own list is what NR is recorded on).
inline std::atomic_bool bridge_ever_live{false};
// Set around the bridge's D3D12CreateDevice / final proxy release, so
// init_device / destroy_device for the private device are not taken for a
// D3D12 game's.
inline thread_local bool bridge_creating_device = false;
inline thread_local bool bridge_releasing_device = false;
inline uint32_t bridge_attempts = 0;
inline uint64_t bridge_next_attempt_present = 0;

// The last fence value handed to the queue, published after the submit.
inline std::atomic_uint64_t bridge_submitted_value{0};
// Session total of watchdog releases (telemetry `timeouts=`); the lost
// decision reads bridge_trip_burst, the windowed rate.
inline std::atomic_uint32_t bridge_timeouts{0};
inline std::atomic_bool bridge_trip_burst{false};
inline std::atomic_bool bridge_tripped{false};
inline std::atomic_bool bridge_device_removed{false};
inline std::atomic_bool bridge_watchdog_stop{false};

// T-BRIDGE-SYNC's counters: signal = D3D11 fence signals, wait = D3D12 queue
// waits, submit = D3D12 submissions, wait11 = D3D11 GPU waits on the return
// fence.  skew = a step of the handshake failed after the frame's terminal
// was already named (the frame keeps the game's image).
inline std::atomic_uint64_t bridge_signals{0};
inline std::atomic_uint64_t bridge_waits{0};
inline std::atomic_uint64_t bridge_submits{0};
inline std::atomic_uint64_t bridge_waits11{0};
inline std::atomic_uint64_t bridge_skew{0};

// One set of twins per source DLSS feature, rebuilt when the game's surfaces
// change shape.  runtime_mutex guards the map and the retired list.
struct BridgeSurfaceSet {
  D3D11_TEXTURE2D_DESC descs[bridge::kResourceKeyCount] = {};
  bool has_exposure = false;
  // The insertion point the set was built for (NRPreUpscale): it decides
  // which of Color and Output is a shared twin and which a stand-in.
  bool pre = false;
  // false = the build failed for these descs; the set is kept so the next
  // evaluate with the same shapes declines at once instead of rebuilding.
  bool usable = false;
  // A refusal that is not a property of the shape (LIFE-11: an allocation
  // or sharing failure, not an unsupported format): the build is retried
  // at present `retry_present` on the shared install-failure schedule, and
  // `attempts` carries across the retries.  0 = latched for the shape.
  uint64_t retry_present = 0;
  uint32_t attempts = 0;
  bool depth_convert = false;
  bool motion_convert = false;
  BridgeMotionTransportMode motion_mode = BridgeMotionTransportMode::kUnselected;
  DXGI_FORMAT motion_transport_format = DXGI_FORMAT_UNKNOWN;
  // The surface NR runs on is a shared twin; the other one is a desc-only
  // D3D12 stand-in (a null D3D11 side), because both pipelines read the
  // descs of Color and Output but only their own surface's contents
  // (dlssnr.hpp DeriveFeatureState / IsDlssEvaluation).  After upscaling:
  // the Output twin, a Color stand-in.  Before: the Color twin, which also
  // carries NR's result back to the game's DLSS, and an Output stand-in.
  // The exposure texture's contents are read (feed v2, v6_exposure_scale),
  // so it is a shared twin copied every evaluate like the guides - or
  // absent when it cannot be shared, which feed v2 reads as "no texture".
  ID3D11Texture2D* color11 = nullptr;
  ID3D12Resource* color12 = nullptr;
  ID3D11Texture2D* output11 = nullptr;
  ID3D12Resource* output12 = nullptr;
  ID3D11Texture2D* motion11 = nullptr;
  ID3D11Texture2D* motion_source_stage = nullptr;
  ID3D11ShaderResourceView* motion_source_srv = nullptr;
  ID3D11Texture2D* motion_conversion_stage = nullptr;
  ID3D11UnorderedAccessView* motion_conversion_uav = nullptr;
  ID3D12Resource* motion12 = nullptr;
  ID3D12Resource* motion_transport12 = nullptr;
  ID3D12DescriptorHeap* motion_conversion_heap = nullptr;
  D3D12_GPU_DESCRIPTOR_HANDLE motion_conversion_srv{};
  D3D12_GPU_DESCRIPTOR_HANDLE motion_conversion_uav_gpu{};
  ID3D11Texture2D* depth11 = nullptr;
  ID3D12Resource* depth12 = nullptr;
  ID3D11Texture2D* depth_stage = nullptr;
  ID3D11ShaderResourceView* depth_srv = nullptr;
  ID3D11UnorderedAccessView* depth_uav = nullptr;
  ID3D11Texture2D* exposure11 = nullptr;
  ID3D12Resource* exposure12 = nullptr;
  uint64_t bytes = 0;
  uint64_t last_value = 0;
};
inline std::unordered_map<const NVSDK_NGX_Handle*, BridgeSurfaceSet> bridge_sets;

struct BridgeRetired {
  std::vector<ID3D12Resource*> resources;
  std::vector<ID3D12DescriptorHeap*> descriptor_heaps;
  uint64_t value = 0;
};
inline std::vector<BridgeRetired> bridge_retired;
inline uint64_t bridge_sets_built = 0;

// ---------------------------------------------------------------------------
// Route decision (DX11Source)
// ---------------------------------------------------------------------------

// A DX11 bridge add-on of another project is loaded (the P4 foreign shape).
// Written by InstallHooks' full module scan.
inline std::atomic_bool foreign_dx11_tool_seen{false};

// Latches the D3D11 route once the process is known to present through
// D3D11 (its swapchain or first present).  auto = native unless a DX11
// bridge add-on of another project is loaded, which keeps every foreign
// session exactly what alpha44 made it; auto waits for one full module scan
// so that answer is not a guess.  PLAN_REHAB_V7.md D1 named foreign as the
// default; this build defaults to auto because foreign gives a D3D11 DLSS
// game no NR without an ini edit, which is the silent-nothing failure the
// compatibility policy ranks worst.
inline void DecideDx11Route() {
  if (dx11_route.load(std::memory_order_acquire) != kDx11RouteUndecided
      || !D3D11OnlySession()) {
    return;
  }
  const uint32_t source = dx11_source.load(std::memory_order_relaxed);
  if (source == kDx11SourceOff) return;
  if (source == kDx11SourceAuto
      && module_scans.load(std::memory_order_relaxed) == 0) {
    return;
  }
  const bool tool = foreign_dx11_tool_seen.load(std::memory_order_relaxed);
  const uint8_t route =
      source == kDx11SourceNative    ? kDx11RouteNative
      : source == kDx11SourceForeign ? kDx11RouteForeign
      : tool                         ? kDx11RouteForeign
                                     : kDx11RouteNative;
  uint8_t expected = kDx11RouteUndecided;
  if (!dx11_route.compare_exchange_strong(expected, route,
                                          std::memory_order_acq_rel)) {
    return;
  }
  const char* const source_name = source == kDx11SourceNative  ? "native"
                                  : source == kDx11SourceForeign ? "foreign"
                                                                 : "auto";
  Log(reshade::log::level::info,
      std::string("DX11Source=") + source_name
          + (route == kDx11RouteNative
                 ? " resolved to the native route: this process presents"
                   " through Direct3D 11, so the game's own Direct3D 11 DLSS"
                   " is served through the Direct3D 11 bridge"
                 : std::string(" resolved to the foreign route: a third-party"
                               " tool's Direct3D 12 evaluates are served and"
                               " the game's own Direct3D 11 DLSS is not"
                               " detoured")
                       + (source == kDx11SourceAuto
                              ? " (a Direct3D 11 bridge add-on of another"
                                " project is loaded)"
                              : "")));
}

// ---------------------------------------------------------------------------
// Declines and loss
// ---------------------------------------------------------------------------

inline std::atomic_uint32_t logged_bridge_declines{0};

// Counts a bridged evaluate's terminal and states the first of each kind.
// The line carries the tag in brackets so T-EXPLAIN (RENODX_E2E_EXPECT=
// decline:<tag>) can match it.
inline void DeclineBridgedEvaluate(NrDeclineReason reason, const char* why) {
  CountNrDecline(reason);
  const uint32_t bit = 1u << (static_cast<uint32_t>(reason) & 31u);
  if ((logged_bridge_declines.fetch_or(bit) & bit) != 0) return;
  Log(reshade::log::level::warning,
      std::string("NR declined an evaluate [")
          + kNrDeclineTags[static_cast<size_t>(reason)] + "]: " + why
          + "; the frame keeps the image the game's own DLSS produced");
}

inline void MarkBridgeLost(const char* why) {
  if (bridge_state.exchange(BridgeState::kLost, std::memory_order_acq_rel)
      == BridgeState::kLost) {
    return;
  }
  Log(reshade::log::level::error,
      std::string("D3D11 bridge lost: ") + why
          + "; Neural Rendering stays off for the rest of this Direct3D 11"
            " session and the game's own DLSS image is untouched");
}

// ---------------------------------------------------------------------------
// Surface sets
// ---------------------------------------------------------------------------

// Caller holds runtime_mutex.  D3D11 objects go at once (the D3D11 runtime
// defers destruction until its GPU work is done); D3D12 resources wait for
// the done fence to pass the last value that used them.
inline void RetireBridgeSet(BridgeSurfaceSet&& set) {
  ReleaseCom(set.color11);
  ReleaseCom(set.output11);
  ReleaseCom(set.motion11);
  ReleaseCom(set.motion_source_srv);
  ReleaseCom(set.motion_source_stage);
  ReleaseCom(set.motion_conversion_uav);
  ReleaseCom(set.motion_conversion_stage);
  ReleaseCom(set.depth11);
  ReleaseCom(set.depth_stage);
  ReleaseCom(set.depth_srv);
  ReleaseCom(set.depth_uav);
  ReleaseCom(set.exposure11);
  BridgeRetired retired;
  retired.value = set.last_value;
  for (ID3D12Resource* resource :
       {set.color12, set.output12, set.motion12, set.motion_transport12,
        set.depth12, set.exposure12}) {
    if (resource != nullptr) retired.resources.push_back(resource);
  }
  if (set.motion_conversion_heap != nullptr) {
    retired.descriptor_heaps.push_back(set.motion_conversion_heap);
  }
  if (!retired.resources.empty() || !retired.descriptor_heaps.empty()) {
    bridge_retired.push_back(std::move(retired));
  }
}

inline void RetireBridgeSetLocked(const NVSDK_NGX_Handle* handle) {
  const auto found = bridge_sets.find(handle);
  if (found == bridge_sets.end()) return;
  RetireBridgeSet(std::move(found->second));
  bridge_sets.erase(found);
}

inline void DrainBridgeRetired(uint64_t done) {
  for (auto it = bridge_retired.begin(); it != bridge_retired.end();) {
    if (it->value <= done) {
      for (ID3D12Resource* resource : it->resources) resource->Release();
      for (ID3D12DescriptorHeap* heap : it->descriptor_heaps) heap->Release();
      it = bridge_retired.erase(it);
    } else {
      ++it;
    }
  }
}

// A D3D11 texture shared with the private D3D12 device and opened there
// (the X1 11-to-12 share: SHARED_NTHANDLE | SHARED and GENERIC_ALL). Returns
// the failing step's HRESULT, which tells an unsupported format from an
// allocation failure (LIFE-11).
struct BridgeSharedTwinFailure {
  BridgeMotionTransportStep step = BridgeMotionTransportStep::kNone;
  HRESULT hr = S_OK;
  HRESULT transient_hr = S_OK;
};

inline HRESULT CreateSharedTwin(const D3D11_TEXTURE2D_DESC& source,
                                DXGI_FORMAT format, UINT bind,
                                ID3D11Texture2D** twin11,
                                ID3D12Resource** twin12,
                                BridgeSharedTwinFailure* failure = nullptr) {
  if (failure != nullptr) *failure = {};
  if (twin11 == nullptr || twin12 == nullptr) return E_POINTER;
  *twin11 = nullptr;
  *twin12 = nullptr;
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = source.Width;
  desc.Height = source.Height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = bind;
  desc.MiscFlags =
      D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
  HRESULT hr = bridge.device11->CreateTexture2D(&desc, nullptr, twin11);
  if (FAILED(hr) || *twin11 == nullptr) {
    if (failure != nullptr) {
      failure->step = BridgeMotionTransportStep::kD3D11CreateSharedTexture;
      failure->hr = FAILED(hr) ? hr : E_FAIL;
      failure->transient_hr = failure->hr != E_INVALIDARG
                                      && failure->hr != DXGI_ERROR_UNSUPPORTED
                                  ? failure->hr
                                  : S_OK;
    }
    ReleaseCom(*twin11);
    return FAILED(hr) ? hr : E_FAIL;
  }
  IDXGIResource1* dxgi = nullptr;
  HANDLE shared = nullptr;
  hr = (*twin11)->QueryInterface(IID_PPV_ARGS(&dxgi));
  if (SUCCEEDED(hr) && dxgi == nullptr) hr = E_NOINTERFACE;
  if (FAILED(hr) && failure != nullptr) {
    failure->step = BridgeMotionTransportStep::kD3D11QuerySharedResource;
    failure->hr = hr;
    failure->transient_hr = hr != E_INVALIDARG && hr != DXGI_ERROR_UNSUPPORTED
                                ? hr
                                : S_OK;
  }
  if (SUCCEEDED(hr)) {
    hr = dxgi->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared);
    if (SUCCEEDED(hr) && shared == nullptr) hr = E_HANDLE;
    if (FAILED(hr) && failure != nullptr) {
      failure->step = BridgeMotionTransportStep::kD3D11CreateSharedHandle;
      failure->hr = hr;
      failure->transient_hr = hr != E_INVALIDARG && hr != DXGI_ERROR_UNSUPPORTED
                                  ? hr
                                  : S_OK;
    }
  }
  ReleaseCom(dxgi);
  if (SUCCEEDED(hr)) {
    hr = bridge.device12->OpenSharedHandle(shared, IID_PPV_ARGS(twin12));
    if (SUCCEEDED(hr) && *twin12 == nullptr) hr = E_FAIL;
    if (FAILED(hr) && failure != nullptr) {
      failure->step = BridgeMotionTransportStep::kD3D12OpenSharedHandle;
      failure->hr = hr;
      failure->transient_hr = hr != E_INVALIDARG && hr != DXGI_ERROR_UNSUPPORTED
                                  ? hr
                                  : S_OK;
    }
  }
  if (shared != nullptr) CloseHandle(shared);
  if (SUCCEEDED(hr) && *twin12 != nullptr) return S_OK;
  ReleaseCom(*twin11);
  ReleaseCom(*twin12);
  return FAILED(hr) ? hr : E_FAIL;
}

// A desc-only D3D12 stand-in for a D3D11 surface the pipeline reads the
// shape of but never the contents of: a reserved resource (no memory), or a
// committed one where tiled resources are unavailable for the format.
// Returns the committed attempt's HRESULT on failure.
inline HRESULT CreateStandIn12(const D3D11_TEXTURE2D_DESC& source,
                               ID3D12Resource** resource) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = source.Width;
  desc.Height = source.Height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = source.Format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
  if (SUCCEEDED(bridge.device12->CreateReservedResource(
          &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
          IID_PPV_ARGS(resource)))) {
    return S_OK;
  }
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
  return bridge.device12->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
      nullptr, IID_PPV_ARGS(resource));
}

inline bool SameSurface(const D3D11_TEXTURE2D_DESC& a,
                        const D3D11_TEXTURE2D_DESC& b) {
  return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format
         && a.SampleDesc.Count == b.SampleDesc.Count
         && a.BindFlags == b.BindFlags;
}

inline std::string DescribeSurface(const D3D11_TEXTURE2D_DESC& desc) {
  char text[96];
  std::snprintf(text, sizeof(text), "%ux%u fmt=%u samples=%u bind=0x%X",
                desc.Width, desc.Height, static_cast<unsigned>(desc.Format),
                desc.SampleDesc.Count, desc.BindFlags);
  return text;
}

// The RGBA32F route keeps ordinary finite R32G32_FLOAT motion components at
// binary32 precision; shader float load/store does not promise bitwise NaN
// payload or denormal preservation.
inline constexpr char kBridgeMotionToTransportShader[] = R"HLSL(
Texture2D<float2> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  uint width, height;
  Destination.GetDimensions(width, height);
  if (dispatch_id.x >= width || dispatch_id.y >= height) return;
  Destination[dispatch_id.xy] = float4(Source.Load(int3(dispatch_id.xy, 0)), 0.0, 0.0);
}
)HLSL";

inline constexpr char kBridgeMotionFromTransportShader[] = R"HLSL(
Texture2D<float4> Source : register(t0);
RWTexture2D<float2> Destination : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  uint width, height;
  Destination.GetDimensions(width, height);
  if (dispatch_id.x >= width || dispatch_id.y >= height) return;
  Destination[dispatch_id.xy] = Source.Load(int3(dispatch_id.xy, 0)).xy;
}
)HLSL";

struct BridgeMotionCandidate {
  ID3D11Texture2D* source_stage = nullptr;
  ID3D11ShaderResourceView* source_srv = nullptr;
  ID3D11Texture2D* conversion_stage = nullptr;
  ID3D11UnorderedAccessView* conversion_uav = nullptr;
  ID3D11Texture2D* shared11 = nullptr;
  ID3D12Resource* shared12 = nullptr;
  ID3D12Resource* motion12 = nullptr;
  ID3D12DescriptorHeap* descriptors = nullptr;
  D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu{};
  UINT shared_bind = 0;
};

inline void ReleaseBridgeMotionCandidate(BridgeMotionCandidate* candidate) {
  if (candidate == nullptr) return;
  ReleaseCom(candidate->source_srv);
  ReleaseCom(candidate->source_stage);
  ReleaseCom(candidate->conversion_uav);
  ReleaseCom(candidate->conversion_stage);
  ReleaseCom(candidate->shared11);
  ReleaseCom(candidate->shared12);
  ReleaseCom(candidate->motion12);
  ReleaseCom(candidate->descriptors);
  *candidate = {};
}

inline void LogBridgeMotionFailure(
    DXGI_FORMAT source_format, DXGI_FORMAT transport_format, UINT bind_flags,
    BridgeMotionTransportMode mode, BridgeMotionTransportStep step,
    HRESULT hr) {
  RecordBridgeMotionFailure(static_cast<uint32_t>(transport_format), bind_flags,
                            step, hr);
  char message[320];
  std::snprintf(message, sizeof(message),
                "D3D11 bridge motion transport attempt failed: mode=%s"
                " source_fmt=%u transport_fmt=%u bind=0x%X step=%s hr=0x%08X",
                BridgeMotionModeName(mode),
                static_cast<uint32_t>(source_format),
                static_cast<uint32_t>(transport_format), bind_flags,
                BridgeMotionStepName(step), static_cast<uint32_t>(hr));
  Log(reshade::log::level::warning, message);
}

inline HRESULT CheckD3D11BridgeFormat(
    DXGI_FORMAT format, UINT required_support) {
  UINT support = 0;
  const HRESULT hr = bridge.device11_base->CheckFormatSupport(format, &support);
  if (FAILED(hr)) return hr;
  if ((support & required_support) != required_support) {
    return DXGI_ERROR_UNSUPPORTED;
  }
  return S_OK;
}

inline HRESULT CheckD3D12BridgeFormat(
    DXGI_FORMAT format, UINT required_support1, UINT required_support2) {
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
  support.Format = format;
  const HRESULT hr = bridge.device12->CheckFeatureSupport(
      D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
  if (FAILED(hr)) return hr;
  if ((support.Support1 & required_support1) != required_support1
      || (support.Support2 & required_support2) != required_support2) {
    return DXGI_ERROR_UNSUPPORTED;
  }
  return S_OK;
}

inline HRESULT CompileBridgeMotionShader(const char* source, ID3DBlob** bytecode,
                                         std::string* compiler_errors) {
  if (bytecode == nullptr) return E_POINTER;
  *bytecode = nullptr;
  if (!renodx::utils::directx::Initialize()
      || renodx::utils::directx::pD3DCompile == nullptr) {
    return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
  }
  ID3DBlob* errors = nullptr;
  const HRESULT hr = renodx::utils::directx::pD3DCompile(
      source, std::strlen(source), "DLSS5 Generic D3D11 bridge motion", nullptr,
      nullptr, "main", "cs_5_0", 0, 0, bytecode, &errors);
  if (FAILED(hr) && errors != nullptr && compiler_errors != nullptr) {
    compiler_errors->assign(
        static_cast<const char*>(errors->GetBufferPointer()),
        errors->GetBufferSize());
  }
  ReleaseCom(errors);
  if (SUCCEEDED(hr) && *bytecode == nullptr) return E_FAIL;
  return hr;
}

inline bool EnsureBridgeMotionConversionPipeline(
    BridgeSharedTwinFailure* failure) {
  if (bridge.motion_convert_cs11 != nullptr
      && bridge.motion_convert_root12 != nullptr
      && bridge.motion_convert_pso12 != nullptr) {
    return true;
  }
  if (bridge.motion_conversion_pipeline_attempted) {
    if (failure != nullptr) {
      failure->step = bridge.motion_conversion_pipeline_failed_step;
      failure->hr = bridge.motion_conversion_pipeline_hr;
    }
    return false;
  }
  bridge.motion_conversion_pipeline_attempted = true;
  BridgeSharedTwinFailure failed{};
  ID3DBlob* to_transport = nullptr;
  ID3DBlob* from_transport = nullptr;
  ID3DBlob* serialized = nullptr;
  ID3DBlob* errors = nullptr;
  ID3D11ComputeShader* motion_cs11 = nullptr;
  ID3D12RootSignature* root12 = nullptr;
  ID3D12PipelineState* pso12 = nullptr;
  const auto fail = [&](BridgeMotionTransportStep step, HRESULT hr) {
    failed.step = step;
    failed.hr = hr;
    ReleaseCom(to_transport);
    ReleaseCom(from_transport);
    ReleaseCom(serialized);
    ReleaseCom(errors);
    ReleaseCom(motion_cs11);
    ReleaseCom(root12);
    ReleaseCom(pso12);
    bridge.motion_conversion_pipeline_attempted =
        hr == E_INVALIDARG || hr == DXGI_ERROR_UNSUPPORTED;
    bridge.motion_conversion_pipeline_failed_step = step;
    bridge.motion_conversion_pipeline_hr = hr;
    if (failure != nullptr) *failure = failed;
    return false;
  };

  std::string compiler_errors;
  HRESULT hr = CompileBridgeMotionShader(kBridgeMotionToTransportShader,
                                         &to_transport, &compiler_errors);
  if (FAILED(hr) || to_transport == nullptr) {
    if (!compiler_errors.empty()) {
      Log(reshade::log::level::warning,
          "D3D11 bridge motion conversion shader compiler: " + compiler_errors);
    }
    return fail(BridgeMotionTransportStep::kD3D11CompileConversionShader,
                FAILED(hr) ? hr : E_FAIL);
  }
  hr = bridge.device11->CreateComputeShader(
      to_transport->GetBufferPointer(), to_transport->GetBufferSize(), nullptr,
      &motion_cs11);
  if (FAILED(hr) || motion_cs11 == nullptr) {
    return fail(BridgeMotionTransportStep::kD3D11CreateConversionShader,
                FAILED(hr) ? hr : E_FAIL);
  }

  compiler_errors.clear();
  hr = CompileBridgeMotionShader(kBridgeMotionFromTransportShader,
                                 &from_transport, &compiler_errors);
  if (FAILED(hr) || from_transport == nullptr) {
    if (!compiler_errors.empty()) {
      Log(reshade::log::level::warning,
          "D3D12 bridge motion reconstruction shader compiler: "
              + compiler_errors);
    }
    return fail(BridgeMotionTransportStep::kD3D12CompileReconstructionShader,
                FAILED(hr) ? hr : E_FAIL);
  }
  if (!renodx::utils::directx::Initialize()
      || renodx::utils::directx::pD3D12SerializeRootSignature == nullptr) {
    return fail(BridgeMotionTransportStep::kD3D12SerializeConversionRootSignature,
                HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
  }
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 1;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  ranges[1].BaseShaderRegister = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  D3D12_ROOT_PARAMETER parameters[2]{};
  for (UINT i = 0; i < 2; ++i) {
    parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[i].DescriptorTable.NumDescriptorRanges = 1;
    parameters[i].DescriptorTable.pDescriptorRanges = &ranges[i];
    parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.NumParameters = 2;
  root_desc.pParameters = parameters;
  hr = renodx::utils::directx::pD3D12SerializeRootSignature(
      &root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
  if (FAILED(hr) || serialized == nullptr) {
    return fail(BridgeMotionTransportStep::kD3D12SerializeConversionRootSignature,
                FAILED(hr) ? hr : E_FAIL);
  }
  hr = bridge.device12->CreateRootSignature(
      0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
      IID_PPV_ARGS(&root12));
  if (FAILED(hr) || root12 == nullptr) {
    return fail(BridgeMotionTransportStep::kD3D12CreateConversionRootSignature,
                FAILED(hr) ? hr : E_FAIL);
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
  pso_desc.pRootSignature = root12;
  pso_desc.CS.pShaderBytecode = from_transport->GetBufferPointer();
  pso_desc.CS.BytecodeLength = from_transport->GetBufferSize();
  hr = bridge.device12->CreateComputePipelineState(
      &pso_desc, IID_PPV_ARGS(&pso12));
  if (FAILED(hr) || pso12 == nullptr) {
    return fail(BridgeMotionTransportStep::kD3D12CreateConversionPipeline,
                FAILED(hr) ? hr : E_FAIL);
  }
  root12->SetName(L"DLSS5 Generic D3D11 bridge motion conversion root signature");
  pso12->SetName(L"DLSS5 Generic D3D11 bridge motion reconstruction");
  bridge.motion_convert_cs11 = motion_cs11;
  bridge.motion_convert_root12 = root12;
  bridge.motion_convert_pso12 = pso12;
  ReleaseCom(to_transport);
  ReleaseCom(from_transport);
  ReleaseCom(serialized);
  ReleaseCom(errors);
  return true;
}

inline bool TryCreateBridgeMotionCandidate(
    const D3D11_TEXTURE2D_DESC& source, DXGI_FORMAT transport_format,
    BridgeMotionTransportMode mode, BridgeMotionCandidate* result,
    BridgeSharedTwinFailure* failure) {
  if (result == nullptr) return false;
  BridgeMotionCandidate candidate{};
  BridgeSharedTwinFailure failed{};
  const auto fail = [&](BridgeMotionTransportStep step, HRESULT hr, UINT bind) {
    failed.step = step;
    failed.hr = hr;
    failed.transient_hr = hr != E_INVALIDARG && hr != DXGI_ERROR_UNSUPPORTED
                              ? hr
                              : S_OK;
    if (failure != nullptr) *failure = failed;
    LogBridgeMotionFailure(source.Format, transport_format, bind, mode, step,
                           hr);
    ReleaseBridgeMotionCandidate(&candidate);
    return false;
  };

  if (!EnsureBridgeMotionConversionPipeline(&failed)) {
    return fail(failed.step, failed.hr, 0);
  }
  HRESULT hr = CheckD3D11BridgeFormat(
      source.Format, D3D11_FORMAT_SUPPORT_TEXTURE2D
                         | D3D11_FORMAT_SUPPORT_SHADER_LOAD);
  if (FAILED(hr)) {
    return fail(BridgeMotionTransportStep::kD3D11CheckSourceFormat, hr, 0);
  }
  hr = CheckD3D11BridgeFormat(
      transport_format, D3D11_FORMAT_SUPPORT_TEXTURE2D
                            | D3D11_FORMAT_SUPPORT_RENDER_TARGET
                            | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW);
  if (FAILED(hr)) {
    return fail(BridgeMotionTransportStep::kD3D11CheckTransportFormat, hr,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
  }
  hr = CheckD3D12BridgeFormat(
      transport_format,
      D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD, 0);
  if (FAILED(hr)) {
    return fail(BridgeMotionTransportStep::kD3D12CheckTransportFormat, hr,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
  }
  hr = CheckD3D12BridgeFormat(
      DXGI_FORMAT_R32G32_FLOAT,
      D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD
          | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW,
      D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
  if (FAILED(hr)) {
    return fail(BridgeMotionTransportStep::kD3D12CheckMotionFormat, hr, 0);
  }

  D3D11_TEXTURE2D_DESC source_stage = source;
  source_stage.MipLevels = 1;
  source_stage.ArraySize = 1;
  source_stage.SampleDesc.Count = 1;
  source_stage.SampleDesc.Quality = 0;
  source_stage.Usage = D3D11_USAGE_DEFAULT;
  source_stage.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  source_stage.CPUAccessFlags = 0;
  source_stage.MiscFlags = 0;
  hr = bridge.device11->CreateTexture2D(&source_stage, nullptr,
                                        &candidate.source_stage);
  if (FAILED(hr) || candidate.source_stage == nullptr) {
    return fail(BridgeMotionTransportStep::kD3D11CreateSourceStage,
                FAILED(hr) ? hr : E_FAIL, 0);
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC source_view{};
  source_view.Format = source.Format;
  source_view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  source_view.Texture2D.MipLevels = 1;
  hr = bridge.device11->CreateShaderResourceView(
      candidate.source_stage, &source_view, &candidate.source_srv);
  if (FAILED(hr) || candidate.source_srv == nullptr) {
    return fail(BridgeMotionTransportStep::kD3D11CreateSourceSrv,
                FAILED(hr) ? hr : E_FAIL, D3D11_BIND_SHADER_RESOURCE);
  }

  D3D11_TEXTURE2D_DESC converted = source_stage;
  converted.Format = transport_format;
  converted.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  hr = bridge.device11->CreateTexture2D(&converted, nullptr,
                                        &candidate.conversion_stage);
  if (FAILED(hr) || candidate.conversion_stage == nullptr) {
    return fail(BridgeMotionTransportStep::kD3D11CreateConversionStage,
                FAILED(hr) ? hr : E_FAIL, D3D11_BIND_UNORDERED_ACCESS);
  }
  D3D11_UNORDERED_ACCESS_VIEW_DESC conversion_view{};
  conversion_view.Format = transport_format;
  conversion_view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
  hr = bridge.device11->CreateUnorderedAccessView(
      candidate.conversion_stage, &conversion_view,
      &candidate.conversion_uav);
  if (FAILED(hr) || candidate.conversion_uav == nullptr) {
    return fail(BridgeMotionTransportStep::kD3D11CreateConversionUav,
                FAILED(hr) ? hr : E_FAIL, D3D11_BIND_UNORDERED_ACCESS);
  }

  constexpr UINT kSharedBinds[] = {
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
      D3D11_BIND_SHADER_RESOURCE,
  };
  bool shared = false;
  HRESULT shared_transient = S_OK;
  for (UINT bind : kSharedBinds) {
    BridgeSharedTwinFailure shared_failure{};
    hr = CreateSharedTwin(source, transport_format, bind,
                          &candidate.shared11, &candidate.shared12,
                          &shared_failure);
    if (SUCCEEDED(hr)) {
      candidate.shared_bind = bind;
      shared = true;
      break;
    }
    failed = shared_failure;
    if (FAILED(shared_failure.transient_hr)) {
      shared_transient = shared_failure.transient_hr;
    }
    LogBridgeMotionFailure(source.Format, transport_format, bind, mode,
                           shared_failure.step, shared_failure.hr);
  }
  if (!shared) {
    failed.transient_hr = shared_transient;
    if (failure != nullptr) *failure = failed;
    ReleaseBridgeMotionCandidate(&candidate);
    return false;
  }

  D3D12_RESOURCE_DESC motion_desc{};
  motion_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  motion_desc.Width = source.Width;
  motion_desc.Height = source.Height;
  motion_desc.DepthOrArraySize = 1;
  motion_desc.MipLevels = 1;
  motion_desc.Format = DXGI_FORMAT_R32G32_FLOAT;
  motion_desc.SampleDesc.Count = 1;
  motion_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  motion_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
  hr = bridge.device12->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &motion_desc,
      D3D12_RESOURCE_STATE_COMMON, nullptr,
      IID_PPV_ARGS(&candidate.motion12));
  if (FAILED(hr) || candidate.motion12 == nullptr) {
    return fail(BridgeMotionTransportStep::kD3D12CreateMotionTexture,
                FAILED(hr) ? hr : E_FAIL, 0);
  }
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = 2;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hr = bridge.device12->CreateDescriptorHeap(
      &heap_desc, IID_PPV_ARGS(&candidate.descriptors));
  if (FAILED(hr) || candidate.descriptors == nullptr) {
    return fail(BridgeMotionTransportStep::kD3D12CreateDescriptorHeap,
                FAILED(hr) ? hr : E_FAIL, 0);
  }

  D3D12_CPU_DESCRIPTOR_HANDLE cpu =
      candidate.descriptors->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC motion_srv{};
  motion_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  motion_srv.Format = transport_format;
  motion_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  motion_srv.Texture2D.MipLevels = 1;
  bridge.device12->CreateShaderResourceView(candidate.shared12, &motion_srv, cpu);
  const UINT descriptor_increment = bridge.device12->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  candidate.srv_gpu = candidate.descriptors->GetGPUDescriptorHandleForHeapStart();
  candidate.uav_gpu = candidate.srv_gpu;
  candidate.uav_gpu.ptr += descriptor_increment;
  cpu.ptr += descriptor_increment;
  D3D12_UNORDERED_ACCESS_VIEW_DESC motion_uav{};
  motion_uav.Format = DXGI_FORMAT_R32G32_FLOAT;
  motion_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  bridge.device12->CreateUnorderedAccessView(candidate.motion12, nullptr,
                                              &motion_uav, cpu);
  *result = candidate;
  if (failure != nullptr) *failure = {};
  return true;
}

// Fault injection for LIFE-11's retry, read once like
// RENODX_NR_TEST_FAIL_NGX_HOOK: the first <n> surface-set builds fail the way
// an E_OUTOFMEMORY does.  Caller holds runtime_mutex.
inline bool ConsumeTestBridgeSetFailure() {
  static uint32_t left = [] {
    char buffer[16] = {};
    size_t length = 0;
    return getenv_s(&length, buffer, sizeof(buffer),
                    "RENODX_NR_TEST_FAIL_BRIDGE_SET") == 0
                   && length != 0
               ? static_cast<uint32_t>(strtoul(buffer, nullptr, 10))
               : 0u;
  }();
  if (left == 0) return false;
  --left;
  return true;
}

// The watchdog's positive control (TIME-02), read once the same way:
// RENODX_NR_TEST_BRIDGE_STALL=<n> makes the n-th bridge submission's D3D12
// side wait for its own `out` value before its list runs.  Only the
// watchdog's CPU signal of `out` releases that wait, and `in` has arrived by
// then, so it is a D3D12-side stall of exactly the kind the watchdog exists
// for, whatever the host's CPU does meanwhile.  0 = off.
inline uint64_t TestBridgeStallValue() {
  static const uint64_t value = [] {
    char buffer[24] = {};
    size_t length = 0;
    return getenv_s(&length, buffer, sizeof(buffer),
                    "RENODX_NR_TEST_BRIDGE_STALL") == 0
                   && length != 0
               ? strtoull(buffer, nullptr, 10)
               : 0ull;
  }();
  return value;
}

// Caller holds runtime_mutex.  Returns the handle's usable set for these
// surfaces and insertion point, building (or rebuilding) it on a change;
// nullptr when the surfaces cannot cross, with `refusal` naming the decline:
// bridge_format for a refusal latched on the shape, bridge_down while an
// allocation failure waits out its retry (LIFE-11).
inline BridgeSurfaceSet* EnsureBridgeSet(const NVSDK_NGX_Handle* handle,
                                         ID3D11Texture2D* const* textures,
                                         bool pre, NrDeclineReason* refusal) {
  using bridge::ResourceKey;
  const auto key = [](ResourceKey k) { return static_cast<size_t>(k); };
  D3D11_TEXTURE2D_DESC descs[bridge::kResourceKeyCount] = {};
  for (size_t i = 0; i < bridge::kResourceKeyCount; ++i) {
    if (textures[i] != nullptr) textures[i]->GetDesc(&descs[i]);
  }
  const bool has_exposure = textures[key(ResourceKey::kExposureTexture)] != nullptr;
  uint32_t attempts = 0;
  if (const auto found = bridge_sets.find(handle); found != bridge_sets.end()) {
    const BridgeSurfaceSet& set = found->second;
    bool same = set.has_exposure == has_exposure && set.pre == pre;
    for (size_t i = 0; same && i < bridge::kResourceKeyCount; ++i) {
      if (i == key(ResourceKey::kExposureTexture) && !has_exposure) continue;
      same = SameSurface(set.descs[i], descs[i]);
    }
    if (same && set.usable) return &found->second;
    if (same && (set.retry_present == 0 || present_generation < set.retry_present)) {
      *refusal = set.retry_present == 0 ? NrDeclineReason::kBridgeFormat
                                        : NrDeclineReason::kBridgeDown;
      return nullptr;
    }
    // Same shape here is a transient refusal whose retry is due.
    if (same) attempts = set.attempts;
    // A retired twin's address can be reused by the next one; the workset
    // keyed by it (the surface NR runs on) must go with it, or the new twin
    // inherits scratch dimensioned for the old shape.
    RetireWorkset(WorksetKey{handle, set.pre ? set.color12 : set.output12});
    RetireBridgeSet(std::move(found->second));
    bridge_sets.erase(found);
  }

  BridgeSurfaceSet set;
  std::copy(std::begin(descs), std::end(descs), std::begin(set.descs));
  set.has_exposure = has_exposure;
  set.pre = pre;
  const D3D11_TEXTURE2D_DESC& color = descs[key(ResourceKey::kColor)];
  const D3D11_TEXTURE2D_DESC& output = descs[key(ResourceKey::kOutput)];
  const D3D11_TEXTURE2D_DESC& motion = descs[key(ResourceKey::kMotionVectors)];
  const D3D11_TEXTURE2D_DESC& depth = descs[key(ResourceKey::kDepth)];
  const char* failure = nullptr;
  std::string identities;
  // LIFE-11: E_INVALIDARG and DXGI_ERROR_UNSUPPORTED are the runtime saying
  // the format or binding cannot be created or shared - a property of the
  // shape, latched as before.  Any other failed create (E_OUTOFMEMORY under
  // VRAM pressure at a load or an alt-tab, a shared-handle failure) is not,
  // and makes the refusal a retry.
  HRESULT transient = S_OK;
  const auto created = [&transient](HRESULT hr) {
    if (FAILED(hr) && hr != E_INVALIDARG && hr != DXGI_ERROR_UNSUPPORTED) {
      transient = hr;
    }
    return SUCCEEDED(hr);
  };
  // Every surface the bridge copies must live on the device of the context
  // that copies it.  Compared by native identity: ReShade detours
  // ID3D11Resource::GetDevice to answer with its device PROXY (unless the
  // device has VIDEO_SUPPORT), while device11_base is the native device the
  // unwrapped context reports.  alpha45 compared the raw pointers and
  // declined every evaluate of God of War (Flags = 0) as bridge_format.
  for (const ResourceKey copied :
       {pre ? ResourceKey::kColor : ResourceKey::kOutput,
        ResourceKey::kMotionVectors, ResourceKey::kDepth}) {
    ID3D11Device* owner = nullptr;
    textures[key(copied)]->GetDevice(&owner);
    if (failure == nullptr
        && !renodx::utils::directx::SameNativeObject(owner, bridge.device11_base)) {
      failure = copied == ResourceKey::kOutput
                    ? "the DLSS output lives on a different D3D11 device than the"
                      " context that evaluated it"
                : copied == ResourceKey::kColor
                    ? "the DLSS input color lives on a different D3D11 device"
                      " than the context that evaluated it"
                    : "the motion vectors or the depth live on a different D3D11"
                      " device than the context that evaluated them";
      char text[160];
      std::snprintf(text, sizeof(text),
                    "; native identities: surface=%p reported=%p context=%p",
                    static_cast<void*>(renodx::utils::directx::NativeIdentity(owner)),
                    static_cast<void*>(owner),
                    static_cast<void*>(
                        renodx::utils::directx::NativeIdentity(bridge.device11_base)));
      identities = text;
    }
    ReleaseCom(owner);
  }
  if (failure == nullptr && (motion.SampleDesc.Count != 1 || depth.SampleDesc.Count != 1)) {
    failure = "multisampled motion vectors or depth cannot be copied into a"
              " single-sample twin";
  }
  if (failure == nullptr && pre && color.SampleDesc.Count != 1) {
    failure = "a multisampled input color cannot be copied into a single-sample"
              " twin";
  }
  if (failure == nullptr && ConsumeTestBridgeSetFailure()) {
    transient = E_OUTOFMEMORY;
    failure = "RENODX_NR_TEST_FAIL_BRIDGE_SET: injected allocation failure";
  }
  if (failure == nullptr && !pre
      && !created(CreateSharedTwin(
          output, output.Format,
          D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
          &set.output11, &set.output12))) {
    failure = "the output format cannot be shared as a UAV twin";
  }
  const auto motion_preference = static_cast<BridgeMotionTransportPreference>(
      bridge_motion_transport_preference.load(std::memory_order_relaxed));
  // The guides (and the pre-upscale color twin) are only read as shader
  // resources - and copied into - on either side; try the binds a format is
  // most likely to support first.
  constexpr UINT kGuideBinds[] = {
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
      D3D11_BIND_SHADER_RESOURCE,
  };
  if (failure == nullptr && pre) {
    bool made = false;
    for (UINT bind : kGuideBinds) {
      if ((made = created(CreateSharedTwin(color, color.Format, bind,
                                           &set.color11, &set.color12)))) {
        break;
      }
    }
    if (!made) failure = "the input color format cannot be shared";
  }
  if (failure == nullptr) {
    BeginBridgeMotionDiagnostic(static_cast<uint32_t>(motion.Format));
    const bool can_convert = motion.Format == DXGI_FORMAT_R32G32_FLOAT;
    const bool force_conversion =
        can_convert
        && (motion_preference == BridgeMotionTransportPreference::kLossless
            || motion_preference == BridgeMotionTransportPreference::kFp16);
    bool direct_shared = false;
    UINT direct_bind = 0;
    if (!force_conversion) {
      for (UINT bind : kGuideBinds) {
        BridgeSharedTwinFailure detail{};
        const HRESULT hr = CreateSharedTwin(
            motion, motion.Format, bind, &set.motion11, &set.motion12, &detail);
        if (created(hr)) {
          direct_shared = true;
          direct_bind = bind;
          break;
        }
        LogBridgeMotionFailure(motion.Format, motion.Format, bind,
                               BridgeMotionTransportMode::kDirect,
                               detail.step, detail.hr);
      }
    }
    if (direct_shared) {
      set.motion_mode = BridgeMotionTransportMode::kDirect;
      set.motion_transport_format = motion.Format;
      SelectBridgeMotionTransport(set.motion_mode,
                                  static_cast<uint32_t>(motion.Format),
                                  direct_bind);
    } else {
      bool converted = false;
      BridgeMotionTransportMode selected_mode =
          BridgeMotionTransportMode::kUnselected;
      const auto try_transport = [&](DXGI_FORMAT transport_format,
                                     BridgeMotionTransportMode mode) {
        BridgeMotionCandidate candidate{};
        BridgeSharedTwinFailure detail{};
        if (!TryCreateBridgeMotionCandidate(motion, transport_format, mode,
                                            &candidate, &detail)) {
          if (FAILED(detail.transient_hr)) transient = detail.transient_hr;
          return false;
        }
        set.motion_convert = true;
        set.motion_mode = mode;
        set.motion_transport_format = transport_format;
        set.motion11 = candidate.shared11;
        candidate.shared11 = nullptr;
        set.motion_source_stage = candidate.source_stage;
        candidate.source_stage = nullptr;
        set.motion_source_srv = candidate.source_srv;
        candidate.source_srv = nullptr;
        set.motion_conversion_stage = candidate.conversion_stage;
        candidate.conversion_stage = nullptr;
        set.motion_conversion_uav = candidate.conversion_uav;
        candidate.conversion_uav = nullptr;
        set.motion_transport12 = candidate.shared12;
        candidate.shared12 = nullptr;
        set.motion12 = candidate.motion12;
        candidate.motion12 = nullptr;
        set.motion_conversion_heap = candidate.descriptors;
        candidate.descriptors = nullptr;
        set.motion_conversion_srv = candidate.srv_gpu;
        set.motion_conversion_uav_gpu = candidate.uav_gpu;
        direct_bind = candidate.shared_bind;
        SelectBridgeMotionTransport(mode,
                                    static_cast<uint32_t>(transport_format),
                                    candidate.shared_bind);
        ReleaseBridgeMotionCandidate(&candidate);
        selected_mode = mode;
        return true;
      };
      if (can_convert && motion_preference != BridgeMotionTransportPreference::kDirect) {
        if (motion_preference == BridgeMotionTransportPreference::kLossless) {
          converted = try_transport(
              DXGI_FORMAT_R32G32B32A32_FLOAT,
              BridgeMotionTransportMode::kLosslessRgba32f);
        } else if (motion_preference == BridgeMotionTransportPreference::kFp16) {
          converted = try_transport(
              DXGI_FORMAT_R16G16B16A16_FLOAT,
              BridgeMotionTransportMode::kReducedRgba16f);
        } else {
          converted = try_transport(
              DXGI_FORMAT_R32G32B32A32_FLOAT,
              BridgeMotionTransportMode::kLosslessRgba32f);
          if (!converted) {
            converted = try_transport(
                DXGI_FORMAT_R16G16B16A16_FLOAT,
                BridgeMotionTransportMode::kReducedRgba16f);
          }
        }
      }
      if (!converted) {
        failure = can_convert
                      ? motion_preference == BridgeMotionTransportPreference::kDirect
                            ? "the direct motion-vector format cannot be shared"
                            : "no selected cross-API motion transport is available"
                      : "the motion-vector format cannot be shared";
      } else {
        char motion_transport_line[256];
        std::snprintf(
            motion_transport_line, sizeof(motion_transport_line),
            "D3D11 bridge motion transport selected: mode=%s source_fmt=%u"
            " transport_fmt=%u bind=0x%X",
            BridgeMotionModeName(selected_mode),
            static_cast<uint32_t>(motion.Format),
            static_cast<uint32_t>(set.motion_transport_format), direct_bind);
        Log(reshade::log::level::info,
            motion_transport_line);
      }
    }
  }
  if (failure == nullptr && (depth.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0) {
    // A depth-stencil surface: copy into a private stage of the same
    // format, read its depth plane through an SRV, write an R32_FLOAT twin
    // (probe X1's XD route).
    set.depth_convert = true;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    switch (depth.Format) {
      case DXGI_FORMAT_R24G8_TYPELESS: view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
      case DXGI_FORMAT_R32G8X24_TYPELESS: view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
      case DXGI_FORMAT_R32_TYPELESS: view = DXGI_FORMAT_R32_FLOAT; break;
      case DXGI_FORMAT_R16_TYPELESS: view = DXGI_FORMAT_R16_UNORM; break;
      default: break;
    }
    D3D11_TEXTURE2D_DESC stage = depth;
    stage.MipLevels = 1;
    stage.ArraySize = 1;
    stage.Usage = D3D11_USAGE_DEFAULT;
    stage.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    stage.CPUAccessFlags = 0;
    stage.MiscFlags = 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = view;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    if (view == DXGI_FORMAT_UNKNOWN) {
      failure = "the depth-stencil format is typed, so its depth plane has no"
                " shader view";
    } else if (bridge.depth_cs == nullptr) {
      failure = "the depth conversion shader could not be created";
    } else if (!created(bridge.device11->CreateTexture2D(&stage, nullptr,
                                                         &set.depth_stage))
               || !created(bridge.device11->CreateShaderResourceView(
                   set.depth_stage, &srv, &set.depth_srv))
               || !created(CreateSharedTwin(depth, DXGI_FORMAT_R32_FLOAT,
                                            D3D11_BIND_UNORDERED_ACCESS
                                                | D3D11_BIND_SHADER_RESOURCE,
                                            &set.depth11, &set.depth12))
               || !created(bridge.device11->CreateUnorderedAccessView(
                   set.depth11, &uav, &set.depth_uav))) {
      failure = "the depth conversion surfaces could not be created";
    }
  } else if (failure == nullptr) {
    bool made = false;
    for (UINT bind : kGuideBinds) {
      if ((made = created(CreateSharedTwin(depth, depth.Format, bind,
                                           &set.depth11, &set.depth12)))) {
        break;
      }
    }
    if (!made) failure = "the depth format cannot be shared";
  }
  if (failure == nullptr
      && !created(CreateStandIn12(pre ? output : color,
                                  pre ? &set.output12 : &set.color12))) {
    failure = "the color/output stand-in could not be created";
  }
  // The exposure twin is optional: every exposure outcome keeps the set, and
  // the build line names it.
  std::string exposure_note = has_exposure ? ", exposure shared" : "";
  if (failure == nullptr && has_exposure) {
    const D3D11_TEXTURE2D_DESC& exposure = descs[key(ResourceKey::kExposureTexture)];
    ID3D11Device* owner = nullptr;
    textures[key(ResourceKey::kExposureTexture)]->GetDevice(&owner);
    const bool same_device =
        renodx::utils::directx::SameNativeObject(owner, bridge.device11_base);
    ReleaseCom(owner);
    bool made = false;
    if (same_device && exposure.SampleDesc.Count == 1) {
      for (UINT bind : kGuideBinds) {
        if ((made = SUCCEEDED(CreateSharedTwin(exposure, exposure.Format, bind,
                                               &set.exposure11,
                                               &set.exposure12)))) {
          break;
        }
      }
    }
    if (!made) {
      exposure_note = std::string(", exposure not shared (")
          + (!same_device                       ? "another D3D11 device"
             : exposure.SampleDesc.Count != 1 ? "multisampled"
                                              : "format cannot be shared")
          + ": feed v2 reads no exposure texture on this shape)";
    }
  }
  if (failure != nullptr) {
    // Never submitted: last_value is 0, so the next drain releases it.
    RetireBridgeSet(std::move(set));
    BridgeSurfaceSet refused;
    std::copy(std::begin(descs), std::end(descs), std::begin(refused.descs));
    refused.has_exposure = has_exposure;
    // Before rc11 this stayed false, so no pre-upscale evaluate matched the
    // refused shape: each one rebuilt the set and logged this line again.
    refused.pre = pre;
    const std::string shape =
        std::string("D3D11 bridge: the surfaces cannot cross (") + failure
        + "): "
        + (pre ? "color " + DescribeSurface(color)
               : "output " + DescribeSurface(output))
        + ", motion " + DescribeSurface(motion) + ", depth "
        + DescribeSurface(depth) + identities;
    *refusal = FAILED(transient) ? NrDeclineReason::kBridgeDown
                                 : NrDeclineReason::kBridgeFormat;
    if (FAILED(transient)) {
      refused.attempts = attempts;
      bool announce = false;
      const uint64_t delay = NoteHookInstallFailure(
          refused.attempts, refused.retry_present, announce);
      if (announce) {
        char tail[160];
        std::snprintf(tail, sizeof(tail),
                      "; hr=0x%08X is not a format refusal, so the shape"
                      " declines as bridge_down and is built again in %llu"
                      " presents (attempt %u)",
                      static_cast<uint32_t>(transient),
                      static_cast<unsigned long long>(delay), refused.attempts);
        Log(reshade::log::level::warning, shape + tail);
      }
    } else {
      Log(reshade::log::level::warning,
          shape + "; this shape declines as bridge_format until it changes");
    }
    bridge_sets.emplace(handle, refused);
    return nullptr;
  }
  for (ID3D12Resource* shared :
       {pre ? set.color12 : set.output12, set.motion12, set.motion_transport12,
        set.depth12, set.exposure12}) {
    if (shared == nullptr) continue;
    const D3D12_RESOURCE_DESC desc = shared->GetDesc();
    set.bytes += bridge.device12->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
  }
  set.usable = true;
  ++bridge_sets_built;
  Log(reshade::log::level::info,
      "D3D11 bridge: surface set built: "
          + (pre ? "color " + DescribeSurface(color)
                 : "output " + DescribeSurface(output))
          + ", motion " + DescribeSurface(motion) + ", depth "
          + DescribeSurface(depth)
          + (set.depth_convert ? " (converted to R32_FLOAT)" : " (copied)")
          + ", motion transport=" + BridgeMotionModeName(set.motion_mode)
          + exposure_note
          + ", " + std::to_string((set.bytes + (1u << 20) - 1) >> 20)
          + " MiB shared, mode=" + (pre ? "pre" : "after")
          + (attempts != 0 ? ", after " + std::to_string(attempts)
                                 + " failed allocation attempt(s)"
                           : ""));
  return &bridge_sets.emplace(handle, set).first->second;
}

// ---------------------------------------------------------------------------
// Bring-up, watchdog, teardown
// ---------------------------------------------------------------------------

inline void PreloadNrRuntime();

struct BridgeWatchdogParams {
  ID3D12Fence* in = nullptr;
  ID3D12Fence* done = nullptr;
  ID3D12Fence* out = nullptr;
  HMODULE module = nullptr;
};

// The watchdog holds its own references to the fences and to this module,
// so neither a teardown that gives up waiting for it nor an add-on unload can
// pull anything out from under it; it leaves through FreeLibraryAndExitThread.
//
// It times the D3D12 side only (TIME-02).  The oldest pending submission,
// done + 1, is timed from the first poll that sees its `in` value arrived -
// the moment the D3D12 queue could start it - not from the CPU submit: the
// D3D11 context signals `in` only after all the game's GPU work queued ahead
// of it, up to the swapchain's frame latency in frames, so a GPU-bound game,
// a loading-screen upload burst or a GPU shared with another process put
// 500 ms between the CPU submit and `in` with nothing stalled at all.  Each
// such trip released the D3D11 wait early (a torn copy-back, or a stale
// color into the game's DLSS history) and three per session lost the bridge.
// The clock starts up to one 50 ms poll late, so a trip means at least
// kBridgeWatchdogNs of D3D12-side time.
inline DWORD WINAPI BridgeWatchdogMain(void* raw) {
  auto* params = static_cast<BridgeWatchdogParams*>(raw);
  uint64_t timed_value = 0;
  int64_t timed_since_ns = 0;
  int64_t trip_ns[kBridgeMaxTimeouts] = {};
  uint32_t trips = 0;
  while (!bridge_watchdog_stop.load(std::memory_order_acquire)) {
    Sleep(50);
    const uint64_t done = params->done->GetCompletedValue();
    if (done == UINT64_MAX) {
      bridge_device_removed.store(true, std::memory_order_release);
      break;
    }
    const uint64_t submitted =
        bridge_submitted_value.load(std::memory_order_acquire);
    if (done >= submitted) {
      bridge_tripped.store(false, std::memory_order_release);
      continue;
    }
    if (bridge_tripped.load(std::memory_order_relaxed)
        || params->in->GetCompletedValue() <= done) {
      continue;
    }
    const int64_t now_ns = SteadyNowNs();
    if (timed_value != done + 1) {
      timed_value = done + 1;
      timed_since_ns = now_ns;
      continue;
    }
    if (now_ns - timed_since_ns <= kBridgeWatchdogNs) continue;
    // Releases every D3D11 wait up to `submitted`.  The D3D12 work itself is
    // left to finish, or the device to be removed - `done` tells which.
    params->out->Signal(submitted);
    bridge_tripped.store(true, std::memory_order_release);
    bridge_timeouts.fetch_add(1, std::memory_order_relaxed);
    // trip_ns[trips % kBridgeMaxTimeouts] is now the oldest of the last
    // kBridgeMaxTimeouts trips.
    trip_ns[trips++ % kBridgeMaxTimeouts] = now_ns;
    if (trips >= kBridgeMaxTimeouts
        && now_ns - trip_ns[trips % kBridgeMaxTimeouts] <= kBridgeLostWindowNs) {
      bridge_trip_burst.store(true, std::memory_order_release);
    }
  }
  params->in->Release();
  params->done->Release();
  params->out->Release();
  const HMODULE module = params->module;
  delete params;
  FreeLibraryAndExitThread(module, 0);
}

// Destroys whatever the bring-up or the session created.  The caller made
// sure the GPU is done with it (DrainBridgeQueue) or never used it.  The
// ReShade proxy goes last, flagged, so its destroy_device is not taken for
// a D3D12 game's.
inline void ReleaseBridgeObjects() {
  for (BridgeSlot& slot : bridge.ring) {
    ReleaseCom(slot.list);
    ReleaseCom(slot.allocator);
    slot.value = 0;
  }
  ReleaseCom(bridge.queue);
  ReleaseCom(bridge.fence_in11);
  ReleaseCom(bridge.fence_out11);
  ReleaseCom(bridge.fence_in12);
  ReleaseCom(bridge.fence_out12);
  ReleaseCom(bridge.fence_done12);
  ReleaseCom(bridge.depth_cs);
  ReleaseCom(bridge.motion_convert_cs11);
  ReleaseCom(bridge.motion_convert_root12);
  ReleaseCom(bridge.motion_convert_pso12);
  bridge.motion_conversion_pipeline_attempted = false;
  bridge.motion_conversion_pipeline_failed_step = BridgeMotionTransportStep::kNone;
  bridge.motion_conversion_pipeline_hr = S_OK;
  ReleaseCom(bridge.context11);
  ReleaseCom(bridge.device11);
  ReleaseCom(bridge.device11_base);
  bridge.context_native = nullptr;
  ReleaseCom(bridge.device12);
  bridge_releasing_device = true;
  ReleaseCom(bridge.proxy12);
  bridge_releasing_device = false;
}

// Returned by EnsureBridgeUp when the bridge is ready.
inline constexpr NrDeclineReason kBridgeReady = NrDeclineReason::kCount;

// Called from the game's D3D11 evaluate, outside every lock: the bring-up
// loads DLLs (d3d12.dll, the NR runtime) and takes the Detours transaction.
// One thread brings the bridge up; any other evaluate meanwhile declines.
inline NrDeclineReason EnsureBridgeUp(ID3D11DeviceContext* context) {
  BridgeState state = bridge_state.load(std::memory_order_acquire);
  if (state == BridgeState::kUp) return kBridgeReady;
  if (state == BridgeState::kLost) return NrDeclineReason::kBridgeLost;
  if (state == BridgeState::kStarting) return NrDeclineReason::kBridgeDown;
  {
    RuntimeLock lock(runtime_mutex);
    if (present_generation < bridge_next_attempt_present) {
      return NrDeclineReason::kBridgeDown;
    }
  }
  if (!bridge_state.compare_exchange_strong(state, BridgeState::kStarting,
                                            std::memory_order_acq_rel)) {
    return state == BridgeState::kUp     ? kBridgeReady
           : state == BridgeState::kLost ? NrDeclineReason::kBridgeLost
                                         : NrDeclineReason::kBridgeDown;
  }
  // The runtime first: with no signed runtime there is nothing to bridge
  // to, and no reason to create a device.
  PreloadNrRuntime();
  {
    RuntimeLock lock(runtime_mutex);
    if (direct_load_state == DirectLoadState::Failed) {
      bridge_state.store(BridgeState::kDown, std::memory_order_release);
      return NrDeclineReason::kNrRuntimeUnavailable;
    }
  }
  HRESULT hr = S_OK;
  IDXGIAdapter* adapter = nullptr;
  const char* step = [&]() -> const char* {
    ID3D11DeviceContext* native = context;
    renodx::utils::directx::NativeFromReShadeProxy(&native);
    if (FAILED(hr = native->QueryInterface(IID_PPV_ARGS(&bridge.context11)))) {
      return "ID3D11DeviceContext4 (Windows 10 1703 or later)";
    }
    bridge.context_native = native;
    native->GetDevice(&bridge.device11_base);
    if (bridge.device11_base == nullptr) return "ID3D11DeviceContext::GetDevice";
    if (FAILED(hr = bridge.device11_base->QueryInterface(
                   IID_PPV_ARGS(&bridge.device11)))) {
      return "ID3D11Device5";
    }
    D3D11_FEATURE_DATA_D3D11_OPTIONS5 shared_options{};
    const HRESULT shared_tier_hr = bridge.device11_base->CheckFeatureSupport(
        D3D11_FEATURE_D3D11_OPTIONS5, &shared_options,
        sizeof(shared_options));
    RecordBridgeMotionSharedResourceTier(
        SUCCEEDED(shared_tier_hr)
            ? static_cast<uint32_t>(shared_options.SharedResourceTier)
            : 0u,
        shared_tier_hr);
    IDXGIDevice* dxgi = nullptr;
    if (FAILED(hr = bridge.device11_base->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
      return "IDXGIDevice";
    }
    hr = dxgi->GetAdapter(&adapter);
    dxgi->Release();
    if (FAILED(hr)) return "IDXGIDevice::GetAdapter";
    NoteFirstDirectxInitialize();
    if (!renodx::utils::directx::Initialize()
        || renodx::utils::directx::pD3D12CreateDevice == nullptr) {
      return "d3d12.dll";
    }
    ID3D12Device* created = nullptr;
    bridge_creating_device = true;
    hr = renodx::utils::directx::pD3D12CreateDevice(
        adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&created));
    if (FAILED(hr)) {
      hr = renodx::utils::directx::pD3D12CreateDevice(
          adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&created));
    }
    bridge_creating_device = false;
    if (FAILED(hr) || created == nullptr) return "D3D12CreateDevice";
    bridge.proxy12 = created;
    bridge.device12 = created;
    renodx::utils::directx::NativeFromReShadeProxy(&bridge.device12);
    bridge.device12->AddRef();
    const D3D12_COMMAND_QUEUE_DESC queue_desc = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (FAILED(hr = bridge.device12->CreateCommandQueue(
                   &queue_desc, IID_PPV_ARGS(&bridge.queue)))) {
      return "CreateCommandQueue";
    }
    for (BridgeSlot& slot : bridge.ring) {
      if (FAILED(hr = bridge.device12->CreateCommandAllocator(
                     D3D12_COMMAND_LIST_TYPE_DIRECT,
                     IID_PPV_ARGS(&slot.allocator)))) {
        return "CreateCommandAllocator";
      }
      if (FAILED(hr = bridge.device12->CreateCommandList(
                     0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator, nullptr,
                     IID_PPV_ARGS(&slot.list)))) {
        return "CreateCommandList";
      }
      if (FAILED(hr = slot.list->Close())) return "ID3D12GraphicsCommandList::Close";
    }
    // The pair crosses the APIs: created on D3D12, opened on D3D11.
    for (auto [fence12, fence11] :
         {std::pair{&bridge.fence_in12, &bridge.fence_in11},
          std::pair{&bridge.fence_out12, &bridge.fence_out11}}) {
      if (FAILED(hr = bridge.device12->CreateFence(
                     0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence12)))) {
        return "CreateFence (shared)";
      }
      HANDLE shared = nullptr;
      if (FAILED(hr = bridge.device12->CreateSharedHandle(
                     *fence12, nullptr, GENERIC_ALL, nullptr, &shared))) {
        return "CreateSharedHandle (fence)";
      }
      hr = bridge.device11->OpenSharedFence(shared, IID_PPV_ARGS(fence11));
      CloseHandle(shared);
      if (FAILED(hr)) return "ID3D11Device5::OpenSharedFence";
    }
    if (FAILED(hr = bridge.device12->CreateFence(
                   0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&bridge.fence_done12)))) {
      return "CreateFence";
    }
    auto* params = new (std::nothrow) BridgeWatchdogParams{
        bridge.fence_in12, bridge.fence_done12, bridge.fence_out12, nullptr};
    if (params == nullptr
        || GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                              reinterpret_cast<LPCWSTR>(&BridgeWatchdogMain),
                              &params->module)
               == FALSE) {
      delete params;
      return "GetModuleHandleExW (watchdog)";
    }
    params->in->AddRef();
    params->done->AddRef();
    params->out->AddRef();
    bridge_watchdog_stop.store(false, std::memory_order_release);
    bridge.watchdog =
        CreateThread(nullptr, 0, BridgeWatchdogMain, params, 0, nullptr);
    if (bridge.watchdog == nullptr) {
      params->in->Release();
      params->done->Release();
      params->out->Release();
      FreeLibrary(params->module);
      delete params;
      return "CreateThread (watchdog)";
    }
    return nullptr;
  }();
  if (step != nullptr) {
    if (bridge.watchdog != nullptr) {
      bridge_watchdog_stop.store(true, std::memory_order_release);
      WaitForSingleObject(bridge.watchdog, 2000);
      CloseHandle(bridge.watchdog);
      bridge.watchdog = nullptr;
    }
    ReleaseBridgeObjects();
    ReleaseCom(adapter);
    bool announce = false;
    uint64_t delay = 0;
    {
      RuntimeLock lock(runtime_mutex);
      delay = NoteHookInstallFailure(bridge_attempts,
                                     bridge_next_attempt_present, announce);
    }
    bridge_state.store(BridgeState::kDown, std::memory_order_release);
    if (announce) {
      std::ostringstream failure;
      failure << "D3D11 bridge could not start: " << step << " failed (hr=0x"
              << std::hex << static_cast<uint32_t>(hr) << std::dec
              << "); the game's DLSS image is untouched. Attempt "
              << bridge_attempts << ", retrying in " << delay << " presents"
              << (delay == kNgxRetryCeilingPresents ? " and hourly after that"
                                                    : "");
      Log(reshade::log::level::warning, failure.str());
    }
    return NrDeclineReason::kBridgeDown;
  }
  // Retirement proofs for the bridge's own submissions; outside every lock
  // (queue_hook_mutex, then the Detours transaction).
  InstallQueueCompletionHooks(bridge.queue);
  const bool depth_conversion = SUCCEEDED(bridge.device11->CreateComputeShader(
      __bridge_depth.data(), __bridge_depth.size(), nullptr, &bridge.depth_cs));
  // The NR feature is created through the signed runtime on this device: the
  // NGX core in this process was initialized for the game's D3D11 device.
  bridge_force_snippet.store(true, std::memory_order_release);
  DXGI_ADAPTER_DESC adapter_desc = {};
  adapter->GetDesc(&adapter_desc);
  ReleaseCom(adapter);
  {
    RuntimeLock lock(runtime_mutex);
    bridge_attempts = 0;
    bridge_next_attempt_present = 0;
  }
  bridge_device11_identity.store(
      renodx::utils::directx::NativeIdentity(bridge.device11_base),
      std::memory_order_release);
  bridge_device_live.store(true, std::memory_order_release);
  bridge_ever_live.store(true, std::memory_order_relaxed);
  bridge_state.store(BridgeState::kUp, std::memory_order_release);
  const HRESULT shared_tier_hr = static_cast<HRESULT>(
      bridge_motion_shared_resource_tier_hresult.load(std::memory_order_relaxed));
  const uint32_t shared_tier =
      bridge_motion_shared_resource_tier.load(std::memory_order_relaxed);
  char shared_tier_text[48] = {};
  if (SUCCEEDED(shared_tier_hr)) {
    std::snprintf(shared_tier_text, sizeof(shared_tier_text), "%u", shared_tier);
  } else {
    std::snprintf(shared_tier_text, sizeof(shared_tier_text),
                  "unknown (query hr=0x%08X)",
                  static_cast<uint32_t>(shared_tier_hr));
  }
  Log(reshade::log::level::info,
      "D3D11 bridge up: a private Direct3D 12 device on "
          + NarrowPath(adapter_desc.Description)
          + ", a " + std::to_string(kBridgeRing)
          + "-deep submission ring, a 500 ms watchdog, depth conversion "
          + (depth_conversion ? "available" : "UNAVAILABLE (depth-stencil"
                                              " surfaces decline as"
                                              " bridge_format)")
          + ", D3D11 shared-resource tier "
          + shared_tier_text
          + "; Neural Rendering runs "
          + (nr_before_upscale.load() ? "before" : "after")
          + " the game's own DLSS upscale (NRPreUpscale)");
  return kBridgeReady;
}

// Shutdown, before NR state is released: every submission must be off the
// GPU before its worksets and features go.  Bounded, like every teardown
// wait; a queue that never drains gets its fences CPU-signalled so no D3D11
// wait can outlive this.
inline void DrainBridgeQueue() {
  if (!bridge_device_live.load(std::memory_order_acquire)
      || bridge.fence_done12 == nullptr) {
    return;
  }
  const uint64_t target = bridge_submitted_value.load(std::memory_order_acquire);
  if (bridge.fence_done12->GetCompletedValue() >= target) return;
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (event == nullptr) return;
  if (SUCCEEDED(bridge.fence_done12->SetEventOnCompletion(target, event))
      && WaitForSingleObject(event, 5000) == WAIT_TIMEOUT) {
    bridge.fence_in12->Signal(target);
    bridge.fence_out12->Signal(target);
    const bool drained = WaitForSingleObject(event, 1000) == WAIT_OBJECT_0;
    Log(reshade::log::level::warning,
        std::string("D3D11 bridge: the Direct3D 12 queue had not drained 5 s"
                    " into teardown; its fences were CPU-signalled")
            + (drained ? " and it drained"
                       : " and it still has not drained; its objects are"
                         " released anyway"));
  }
  CloseHandle(event);
}

// Shutdown, after NR state and the runtime are gone.  Re-armable: the next
// D3D11 evaluate brings a new device up, unless the bridge was lost.
inline void ReleaseBridge() {
  if (!bridge_device_live.load(std::memory_order_acquire)) return;
  if (bridge.watchdog != nullptr) {
    bridge_watchdog_stop.store(true, std::memory_order_release);
    WaitForSingleObject(bridge.watchdog, 2000);
    CloseHandle(bridge.watchdog);
    bridge.watchdog = nullptr;
  }
  {
    RuntimeLock lock(runtime_mutex);
    for (auto& [_, set] : bridge_sets) RetireBridgeSet(std::move(set));
    bridge_sets.clear();
    // DrainBridgeQueue proved (or forced) completion of every submission.
    DrainBridgeRetired(UINT64_MAX);
  }
  if (bridge.fence_out12 != nullptr) {
    bridge.fence_out12->Signal(bridge_submitted_value.load(std::memory_order_acquire));
  }
  bridge_device11_identity.store(nullptr, std::memory_order_release);
  ReleaseBridgeObjects();
  bridge_force_snippet.store(false, std::memory_order_release);
  bridge_device_live.store(false, std::memory_order_release);
  BridgeState up = BridgeState::kUp;
  if (bridge_state.compare_exchange_strong(up, BridgeState::kDown,
                                           std::memory_order_acq_rel)) {
    // The next device's fences start at 0 again.
    bridge_submitted_value.store(0, std::memory_order_release);
    bridge_timeouts.store(0, std::memory_order_relaxed);
    bridge_trip_burst.store(false, std::memory_order_relaxed);
    bridge_tripped.store(false, std::memory_order_relaxed);
  }
  Log(reshade::log::level::info,
      "D3D11 bridge: private D3D12 device released ("
          + std::to_string(bridge_submits.load(std::memory_order_relaxed))
          + " frames bridged this session)");
}

// ---------------------------------------------------------------------------
// The per-frame path
// ---------------------------------------------------------------------------

// One bridged evaluate.  `redirect` null: after upscaling, called once the
// game's own D3D11 DLSS evaluate returned successfully.  Non-null: before
// upscaling, called before the game's evaluate runs; on success the game's
// Color key is left pointed at NR's result in `redirect`, which puts it back
// after the game's evaluate.  Names exactly one terminal for the evaluate: a
// decline here, or the pipeline's own (success included).  A handshake step
// that fails after the pipeline has named it is counted in bridge[skew=]
// instead - one terminal per evaluate is what keeps the funnel a partition.
inline void RunBridge(ID3D11DeviceContext* context,
                      const NVSDK_NGX_Handle* handle,
                      const NVSDK_NGX_Parameter* parameters,
                      bridge::ScopedD3D11Redirect* redirect) {
  using bridge::ResourceKey;
  const bool pre = redirect != nullptr;
  bool named = false;
  const auto name = [&named](NrDeclineReason reason) {
    CountNrDecline(reason);
    named = true;
  };
  const auto decline = [&named](NrDeclineReason reason, const char* why) {
    DeclineBridgedEvaluate(reason, why);
    named = true;
  };
  const auto at = [](ResourceKey k) { return static_cast<size_t>(k); };
  GuardHook([&] {
    if (context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED) {
      decline(NrDeclineReason::kD3D11Deferred,
              "the game evaluated DLSS on a deferred D3D11 context, which the"
              " bridge cannot order against its fences");
      return;
    }
    {
      RuntimeLock lock(runtime_mutex);
      if (!RegisteredDlssEvaluate(handle)) {
        name(NrDeclineReason::kNgxNotDlssEvaluation);
        return;
      }
      if (direct_load_state == DirectLoadState::Failed) {
        name(NrDeclineReason::kNrRuntimeUnavailable);
        return;
      }
    }
    ngx11_dlss_evaluate_seen.store(true, std::memory_order_relaxed);
    ID3D11Resource* resources[bridge::kResourceKeyCount] = {};
    ID3D11Texture2D* textures[bridge::kResourceKeyCount] = {};
    for (size_t i = 0; i < bridge::kResourceKeyCount; ++i) {
      resources[i] =
          bridge::GetD3D11Resource(parameters, bridge::kResourceKeyNames[i]);
      D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      if (resources[i] != nullptr) resources[i]->GetType(&dimension);
      if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        textures[i] = static_cast<ID3D11Texture2D*>(resources[i]);
      }
    }
    if (textures[at(ResourceKey::kColor)] == nullptr) {
      name(NrDeclineReason::kNgxNotDlssEvaluation);
      return;
    }
    if (resources[at(ResourceKey::kOutput)] == nullptr
        || resources[at(ResourceKey::kMotionVectors)] == nullptr
        || resources[at(ResourceKey::kDepth)] == nullptr) {
      decline(NrDeclineReason::kNgxMissingGuides,
              "the D3D11 DLSS parameter block lacks Output/Motion/Depth"
              " pointers");
      return;
    }
    ID3D11Texture2D* const output = textures[at(ResourceKey::kOutput)];
    D3D11_TEXTURE2D_DESC output_desc = {};
    if (output != nullptr) output->GetDesc(&output_desc);
    if (output == nullptr || output_desc.ArraySize != 1
        || output_desc.SampleDesc.Count != 1) {
      decline(NrDeclineReason::kNgxOutputGeometry,
              "the D3D11 DLSS output is not a single-sample, single-slice 2D"
              " texture");
      return;
    }
    if (textures[at(ResourceKey::kMotionVectors)] == nullptr
        || textures[at(ResourceKey::kDepth)] == nullptr) {
      decline(NrDeclineReason::kBridgeFormat,
              "the motion vectors or the depth are not 2D textures");
      return;
    }

    const NrDeclineReason up = EnsureBridgeUp(context);
    if (up == NrDeclineReason::kNrRuntimeUnavailable) {
      name(up);
      return;
    }
    if (up != kBridgeReady) {
      decline(up, up == NrDeclineReason::kBridgeLost
                      ? "the Direct3D 11 bridge was lost earlier in this"
                        " session"
                      : "the Direct3D 11 bridge is not up (starting, or"
                        " backing off after a failed start - the start line"
                        " names the step)");
      return;
    }
    ID3D11DeviceContext* native = context;
    renodx::utils::directx::NativeFromReShadeProxy(&native);
    if (native != bridge.context_native) {
      decline(NrDeclineReason::kBridgeDown,
              "DLSS was evaluated on a second D3D11 immediate context; the"
              " bridge serves the first one");
      return;
    }

    RuntimeLock lock(runtime_mutex);
    const uint64_t done = bridge.fence_done12->GetCompletedValue();
    if (done == UINT64_MAX
        || bridge_device_removed.load(std::memory_order_acquire)) {
      MarkBridgeLost("the private Direct3D 12 device was removed");
    } else if (bridge_trip_burst.load(std::memory_order_acquire)) {
      MarkBridgeLost("the watchdog released three stalled Direct3D 12"
                     " submissions within 60 s");
    }
    if (bridge_state.load(std::memory_order_acquire) == BridgeState::kLost) {
      decline(NrDeclineReason::kBridgeLost, "the Direct3D 11 bridge is lost");
      return;
    }
    // A trip holds NR off until the D3D12 queue has caught up with every
    // submission the watchdog released.  The watchdog clears the flag at its
    // next poll; `done` is read here anyway, so a caught-up queue serves
    // this evaluate instead of up to one poll later (the e2e11 native_field
    // lane counted 107 declines after one trip, frames without NR being
    // cheap).
    if (bridge_tripped.load(std::memory_order_acquire)) {
      if (done < bridge_submitted_value.load(std::memory_order_relaxed)) {
        decline(NrDeclineReason::kBridgeTimeout,
                "a Direct3D 12 submission did not finish within 500 ms, so the"
                " watchdog released the Direct3D 11 wait");
        return;
      }
      bridge_tripped.store(false, std::memory_order_release);
    }
    DrainBridgeRetired(done);
    const uint64_t value =
        bridge_submitted_value.load(std::memory_order_relaxed) + 1;
    BridgeSlot& slot = bridge.ring[value % kBridgeRing];
    if (done < slot.value) {
      decline(NrDeclineReason::kBridgeBusy,
              "every bridge submission in the ring is still on the GPU");
      return;
    }
    NrDeclineReason refusal = NrDeclineReason::kBridgeFormat;
    BridgeSurfaceSet* const set = EnsureBridgeSet(handle, textures, pre, &refusal);
    if (set == nullptr) {
      decline(refusal,
              refusal == NrDeclineReason::kBridgeFormat
                  ? "the DLSS surfaces cannot cross to Direct3D 12 (the bridge"
                    " line above names the shape)"
                  : "the bridge surfaces could not be allocated; they are"
                    " built again on a backoff (the bridge line above names"
                    " the step)");
      return;
    }
    if (FAILED(slot.allocator->Reset())
        || FAILED(slot.list->Reset(slot.allocator, nullptr))) {
      bridge_skew.fetch_add(1, std::memory_order_relaxed);
      decline(NrDeclineReason::kBridgeDown,
              "the bridge command list could not be reset");
      return;
    }
    submission::OnCommandListReset(slot.list);
    bool touched = false;
    // Before upscaling: the pipeline left NR's result in the game's Color
    // (only on a true return; a decline leaves the game's image alone).
    bool swapped = false;
    // The twin NR runs on, and the state the pipeline reads it in: NR writes
    // the Output twin after upscaling and reads the Color twin before it.
    ID3D12Resource* const nr_twin = pre ? set->color12 : set->output12;
    const D3D12_RESOURCE_STATES nr_twin_state =
        pre ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    {
      const InjectedCommandScope injected;
      // Shared resources arrive in COMMON and must leave in it: the D3D11
      // side reads and writes them between submissions.
      Transition(slot.list, nr_twin, D3D12_RESOURCE_STATE_COMMON, nr_twin_state);
      if (set->motion_convert) {
        Transition(slot.list, set->motion_transport12,
                   D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(slot.list, set->motion12, D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* const heaps[] = {set->motion_conversion_heap};
        slot.list->SetDescriptorHeaps(1, heaps);
        slot.list->SetComputeRootSignature(bridge.motion_convert_root12);
        slot.list->SetPipelineState(bridge.motion_convert_pso12);
        slot.list->SetComputeRootDescriptorTable(0, set->motion_conversion_srv);
        slot.list->SetComputeRootDescriptorTable(1, set->motion_conversion_uav_gpu);
        const D3D11_TEXTURE2D_DESC& motion_desc =
            set->descs[at(ResourceKey::kMotionVectors)];
        slot.list->Dispatch((motion_desc.Width + 7) / 8,
                            (motion_desc.Height + 7) / 8, 1);
        Transition(slot.list, set->motion12,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(slot.list, set->motion_transport12,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
      } else {
        Transition(slot.list, set->motion12, D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      }
      Transition(slot.list, set->depth12, D3D12_RESOURCE_STATE_COMMON,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      if (set->exposure12 != nullptr) {
        Transition(slot.list, set->exposure12, D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      }
      ID3D12Resource* const twins[bridge::kResourceKeyCount] = {
          set->color12, set->output12, set->motion12, set->depth12,
          set->exposure12};
      const bridge::ParameterView view(parameters, twins);
      // There is no host state on the bridge's own list to restore, so the
      // restore-target gate has nothing to wait for.
      gate_ever_opened.store(true, std::memory_order_relaxed);
      // From here the pipeline names the terminal (success or a decline).
      named = true;
      const int64_t started_ns = SteadyNowNs();
      if (pre) {
        PreSrSwap swap;
        swapped = ProcessInlinePreSR(slot.list, handle, &view, swap, &touched);
        if (swapped) {
          // NR's result into the Color twin, which the game's DLSS reads.
          Transition(slot.list, set->color12, nr_twin_state,
                     D3D12_RESOURCE_STATE_COPY_DEST);
          Transition(slot.list, swap.result, kPreSrReadable,
                     D3D12_RESOURCE_STATE_COPY_SOURCE);
          CopyMip0(slot.list, set->color12, swap.result);
          Transition(slot.list, swap.result, D3D12_RESOURCE_STATE_COPY_SOURCE,
                     kPreSrReadable);
          Transition(slot.list, set->color12, D3D12_RESOURCE_STATE_COPY_DEST,
                     nr_twin_state);
        }
      } else {
        ProcessInline(slot.list, handle, &view, &touched);
      }
      RecordInjectionCpu(started_ns);
      Transition(slot.list, nr_twin, nr_twin_state, D3D12_RESOURCE_STATE_COMMON);
      Transition(slot.list, set->motion12,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                 D3D12_RESOURCE_STATE_COMMON);
      Transition(slot.list, set->depth12,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                 D3D12_RESOURCE_STATE_COMMON);
      if (set->exposure12 != nullptr) {
        Transition(slot.list, set->exposure12,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
      }
    }
    const bool closed = SUCCEEDED(slot.list->Close());
    if (!touched) return;
    if (!closed) {
      bridge_skew.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    ID3D11DeviceContext4* const ctx = bridge.context11;
    if (set->motion_convert) {
      ctx->CopySubresourceRegion(
          set->motion_source_stage, 0, 0, 0, 0,
          textures[at(ResourceKey::kMotionVectors)], 0, nullptr);
    } else {
      ctx->CopySubresourceRegion(set->motion11, 0, 0, 0, 0,
                                 textures[at(ResourceKey::kMotionVectors)], 0,
                                 nullptr);
    }
    if (set->exposure11 != nullptr) {
      ctx->CopySubresourceRegion(set->exposure11, 0, 0, 0, 0,
                                 textures[at(ResourceKey::kExposureTexture)], 0,
                                 nullptr);
    }
    if (set->depth_convert) {
      ctx->CopySubresourceRegion(set->depth_stage, 0, 0, 0, 0,
                                 textures[at(ResourceKey::kDepth)], 0, nullptr);
    }
    if (set->motion_convert || set->depth_convert) {
      // The only compute state the pass touches is the shader, SRV 0 and
      // UAV 0; all three go back exactly (the shader with its class
      // instances, the UAV with its counter untouched).
      ID3D11ComputeShader* saved_shader = nullptr;
      ID3D11ClassInstance* saved_instances[256] = {};
      UINT saved_instance_count = 256;
      ID3D11ShaderResourceView* saved_srv = nullptr;
      ID3D11UnorderedAccessView* saved_uav = nullptr;
      ctx->CSGetShader(&saved_shader, saved_instances, &saved_instance_count);
      ctx->CSGetShaderResources(0, 1, &saved_srv);
      ctx->CSGetUnorderedAccessViews(0, 1, &saved_uav);
      const UINT keep_counter = static_cast<UINT>(-1);
      if (set->motion_convert) {
        ctx->CSSetShader(bridge.motion_convert_cs11, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &set->motion_source_srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &set->motion_conversion_uav,
                                       nullptr);
        const D3D11_TEXTURE2D_DESC& motion_desc =
            set->descs[at(ResourceKey::kMotionVectors)];
        ctx->Dispatch((motion_desc.Width + 7) / 8,
                      (motion_desc.Height + 7) / 8, 1);
        ID3D11ShaderResourceView* const null_srv = nullptr;
        ID3D11UnorderedAccessView* const null_uav = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, &keep_counter);
        ctx->CSSetShaderResources(0, 1, &null_srv);
        ctx->CopyResource(set->motion11, set->motion_conversion_stage);
      }
      if (set->depth_convert) {
        ctx->CSSetShader(bridge.depth_cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &set->depth_srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &set->depth_uav, nullptr);
        const D3D11_TEXTURE2D_DESC& depth =
            set->descs[at(ResourceKey::kDepth)];
        ctx->Dispatch((depth.Width + 7) / 8, (depth.Height + 7) / 8, 1);
      }
      ctx->CSSetUnorderedAccessViews(0, 1, &saved_uav, &keep_counter);
      ctx->CSSetShaderResources(0, 1, &saved_srv);
      ctx->CSSetShader(saved_shader, saved_instances, saved_instance_count);
      ReleaseCom(saved_uav);
      ReleaseCom(saved_srv);
      ReleaseCom(saved_shader);
      for (UINT i = 0; i < saved_instance_count && i < 256; ++i) {
        ReleaseCom(saved_instances[i]);
      }
    }
    if (!set->depth_convert) {
      ctx->CopySubresourceRegion(set->depth11, 0, 0, 0, 0,
                                 textures[at(ResourceKey::kDepth)], 0, nullptr);
    }
    if (pre) {
      ctx->CopySubresourceRegion(set->color11, 0, 0, 0, 0,
                                 textures[at(ResourceKey::kColor)], 0, nullptr);
    } else {
      ctx->CopySubresourceRegion(set->output11, 0, 0, 0, 0, output, 0, nullptr);
    }
    if (FAILED(ctx->Signal(bridge.fence_in11, value))) {
      bridge_skew.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    bridge_signals.fetch_add(1, std::memory_order_relaxed);
    ctx->Flush();
    // The value is consumed from here: whatever happens, the return fence
    // must reach it, or the next D3D11 wait never returns.
    bool submitted = SUCCEEDED(bridge.queue->Wait(bridge.fence_in12, value));
    if (submitted && value == TestBridgeStallValue()) {
      Log(reshade::log::level::warning,
          "RENODX_NR_TEST_BRIDGE_STALL: submission " + std::to_string(value)
              + " waits for its own return fence, which only the watchdog"
                " can signal");
      submitted = SUCCEEDED(bridge.queue->Wait(bridge.fence_out12, value));
    }
    if (submitted) {
      bridge_waits.fetch_add(1, std::memory_order_relaxed);
      ID3D12CommandList* const lists[] = {slot.list};
      bridge.queue->ExecuteCommandLists(1, lists);
      bridge_submits.fetch_add(1, std::memory_order_relaxed);
      submitted = SUCCEEDED(bridge.queue->Signal(bridge.fence_out12, value))
                  && SUCCEEDED(bridge.queue->Signal(bridge.fence_done12, value));
    }
    slot.value = value;
    set->last_value = value;
    bridge_submitted_value.store(value, std::memory_order_release);
    if (!submitted) {
      bridge.fence_out12->Signal(value);
      bridge.fence_done12->Signal(value);
      bridge_skew.fetch_add(1, std::memory_order_relaxed);
      MarkBridgeLost("a Direct3D 12 queue operation failed");
      return;
    }
    if (FAILED(ctx->Wait(bridge.fence_out11, value))) {
      bridge_skew.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    bridge_waits11.fetch_add(1, std::memory_order_relaxed);
    if (!pre) {
      ctx->CopySubresourceRegion(output, 0, 0, 0, 0, set->output11, 0, nullptr);
    } else if (swapped) {
      // The wait above orders the game's DLSS read of the twin after NR's
      // write; the game's own Color is never written.
      redirect->Point(parameters, NVSDK_NGX_Parameter_Color, set->color11);
    }
  });
  if (!named) CountNrDecline(NrDeclineReason::kBridgeDown);
}

// ---------------------------------------------------------------------------
// The D3D11 NGX detours
// ---------------------------------------------------------------------------

inline std::atomic_bool ngx11_slot_noncore[kMaxNgxSlots] = {};

// The body both evaluate exports share.  `real` forwards the call with its
// own callback type.
template <typename Real>
inline NVSDK_NGX_Result Ngx11Evaluate(int slot, const char* entry,
                                      ID3D11DeviceContext* context,
                                      const NVSDK_NGX_Handle* handle,
                                      const NVSDK_NGX_Parameter* parameters,
                                      Real&& real) {
  CallbackScope callback_scope;
  const Ngx11Nesting nesting;
  if (!callback_scope || !nesting.outermost || InsideDirectCall()) return real();
  // A-1: the bridge calls GetType on the context and unwraps it.
  if (!NgxFirstArgumentUsable(context, entry, slot, kD3D11ContextVtableBytes,
                              ngx11_slot_noncore)) {
    const EvaluateInFlightScope in_flight(true);
    ++intercepted_evaluations;
    CountNrDecline(NrDeclineReason::kImplausibleArgument);
    return real();
  }
  const EvaluateChainScope chain_scope;
  // Counted in `seen` from here until this evaluate names its terminal.
  const EvaluateInFlightScope in_flight(true);
  ++intercepted_evaluations;
  NgxLifecycleTick();
  if (!logged_first_evaluate.exchange(true)) {
    Log(reshade::log::level::info,
        "first NGX evaluate intercepted (D3D11 export, slot="
            + std::to_string(slot) + ")");
  }
  // The loader prime maps the signed runtime, so it waits until NR has a
  // frame to run: an NR-off D3D11 session maps nothing (row 23; the
  // e2e11_native control asserts it).  Outside the runtime lock, as the
  // prime requires.
  if (nr_before_upscale.load() && enabled.load() && !NrYieldsToForeign()
      && handle != nullptr && parameters != nullptr) {
    // Before upscaling: the bridge names this evaluate's terminal, and the
    // game's evaluate reads NR's result through the redirected Color key
    // until `redirect` goes out of scope - after real() returned.  A game
    // evaluate that fails after it is not counted a second time.
    PrimeNgxLoaderSymbols();
    bridge::ScopedD3D11Redirect redirect;
    RunBridge(context, handle, parameters, &redirect);
    return real();
  }
  const NVSDK_NGX_Result result = real();
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
  // Both insertion modes: the pre-upscale branch above did not run.
  if (NrYieldsToForeign()) {
    CountNrDecline(NrDeclineReason::kForeignNr);
    return result;
  }
  PrimeNgxLoaderSymbols();
  RunBridge(context, handle, parameters, nullptr);
  return result;
}

using Ngx11CreateFn = decltype(&NVSDK_NGX_D3D11_CreateFeature);
using Ngx11EvaluateFn = decltype(&NVSDK_NGX_D3D11_EvaluateFeature);
using Ngx11EvaluateCFn = decltype(&NVSDK_NGX_D3D11_EvaluateFeature_C);
using Ngx11ReleaseFn = decltype(&NVSDK_NGX_D3D11_ReleaseFeature);

template <int Slot>
inline NVSDK_NGX_Result NVSDK_CONV HookedCreateFeature11Slot(
    ID3D11DeviceContext* context,
    NVSDK_NGX_Feature feature,
    NVSDK_NGX_Parameter* parameters,
    NVSDK_NGX_Handle** output_handle) {
  ngx11_entry_create[Slot].fetch_add(1, std::memory_order_relaxed);
  const auto real = reinterpret_cast<Ngx11CreateFn>(ngx11_slot_real[Slot].create);
  CallbackScope callback_scope;
  const Ngx11Nesting nesting;
  if (!callback_scope || !nesting.outermost || InsideDirectCall()) {
    return real(context, feature, parameters, output_handle);
  }
  ++intercepted_creates;
  const uint32_t id_bit = static_cast<uint32_t>(feature) < 32
      ? (1u << static_cast<uint32_t>(feature))
      : 0;
  if (id_bit != 0 && (logged_create11_ids.fetch_or(id_bit) & id_bit) == 0) {
    Log(reshade::log::level::info,
        "NGX feature create intercepted (D3D11 export): feature="
            + std::to_string(static_cast<int>(feature)) + " ("
            + SourceFeatureName(feature) + "), slot=" + std::to_string(Slot));
  }
  const NVSDK_NGX_Result result = real(context, feature, parameters, output_handle);
  if (id_bit != 0
      && (logged_create11_result_ids.fetch_or(id_bit) & id_bit) == 0) {
    std::ostringstream result_text;
    result_text << "NGX feature create returned (D3D11 export): feature="
                << static_cast<int>(feature) << " ("
                << SourceFeatureName(feature) << "), result="
                << static_cast<int>(result)
                << (NVSDK_NGX_FAILED(result) ? " (failed)" : " (success)");
    if (NVSDK_NGX_FAILED(result)) result_text << GameCreateFailureNote(result);
    Log(NVSDK_NGX_FAILED(result) ? reshade::log::level::warning
                                 : reshade::log::level::info,
        result_text.str());
  }
  if (NVSDK_NGX_FAILED(result) || output_handle == nullptr
      || *output_handle == nullptr || parameters == nullptr) {
    return result;
  }
  // The create block holds D3D11 pointers; the pipeline reads it through a
  // view with no resources at all.
  const bridge::ParameterView view(parameters, nullptr);
  RuntimeLock lock(runtime_mutex);
  RegisterCreatedFeatureLocked(*output_handle, feature, &view);
  return result;
}

template <int Slot>
inline NVSDK_NGX_Result NVSDK_CONV HookedEvaluateFeature11Slot(
    ID3D11DeviceContext* context,
    const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters,
    PFN_NVSDK_NGX_ProgressCallback callback) {
  ngx11_entry_evaluate[Slot].fetch_add(1, std::memory_order_relaxed);
  const auto real =
      reinterpret_cast<Ngx11EvaluateFn>(ngx11_slot_real[Slot].evaluate);
  return Ngx11Evaluate(Slot, "evaluate (D3D11)", context, handle, parameters,
                       [&] { return real(context, handle, parameters, callback); });
}

template <int Slot>
inline NVSDK_NGX_Result NVSDK_CONV HookedEvaluateFeature11CSlot(
    ID3D11DeviceContext* context,
    const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters,
    PFN_NVSDK_NGX_ProgressCallback_C callback) {
  ngx11_entry_evaluate_c[Slot].fetch_add(1, std::memory_order_relaxed);
  const auto real =
      reinterpret_cast<Ngx11EvaluateCFn>(ngx11_slot_real[Slot].evaluate_c);
  return Ngx11Evaluate(Slot, "evaluate_c (D3D11)", context, handle, parameters,
                       [&] { return real(context, handle, parameters, callback); });
}

template <int Slot>
inline NVSDK_NGX_Result NVSDK_CONV HookedReleaseFeature11Slot(
    NVSDK_NGX_Handle* handle) {
  ngx11_entry_release[Slot].fetch_add(1, std::memory_order_relaxed);
  const auto real =
      reinterpret_cast<Ngx11ReleaseFn>(ngx11_slot_real[Slot].release);
  CallbackScope callback_scope;
  const Ngx11Nesting nesting;
  if (callback_scope && nesting.outermost && !InsideDirectCall()) {
    // Retire, never free: the stream's last bridged submission may still be
    // on the GPU (the done fence decides).
    RuntimeLock lock(runtime_mutex);
    ForgetSourceHandleLocked(handle);
    RetireBridgeSetLocked(handle);
  }
  return real(handle);
}

inline Ngx11CreateFn const kNgx11CreateWrappers[kMaxNgxSlots] = {
    HookedCreateFeature11Slot<0>, HookedCreateFeature11Slot<1>,
    HookedCreateFeature11Slot<2>, HookedCreateFeature11Slot<3>,
    HookedCreateFeature11Slot<4>, HookedCreateFeature11Slot<5>,
    HookedCreateFeature11Slot<6>, HookedCreateFeature11Slot<7>,
};
inline Ngx11EvaluateFn const kNgx11EvaluateWrappers[kMaxNgxSlots] = {
    HookedEvaluateFeature11Slot<0>, HookedEvaluateFeature11Slot<1>,
    HookedEvaluateFeature11Slot<2>, HookedEvaluateFeature11Slot<3>,
    HookedEvaluateFeature11Slot<4>, HookedEvaluateFeature11Slot<5>,
    HookedEvaluateFeature11Slot<6>, HookedEvaluateFeature11Slot<7>,
};
inline Ngx11EvaluateCFn const kNgx11EvalCWrappers[kMaxNgxSlots] = {
    HookedEvaluateFeature11CSlot<0>, HookedEvaluateFeature11CSlot<1>,
    HookedEvaluateFeature11CSlot<2>, HookedEvaluateFeature11CSlot<3>,
    HookedEvaluateFeature11CSlot<4>, HookedEvaluateFeature11CSlot<5>,
    HookedEvaluateFeature11CSlot<6>, HookedEvaluateFeature11CSlot<7>,
};
inline Ngx11ReleaseFn const kNgx11ReleaseWrappers[kMaxNgxSlots] = {
    HookedReleaseFeature11Slot<0>, HookedReleaseFeature11Slot<1>,
    HookedReleaseFeature11Slot<2>, HookedReleaseFeature11Slot<3>,
    HookedReleaseFeature11Slot<4>, HookedReleaseFeature11Slot<5>,
    HookedReleaseFeature11Slot<6>, HookedReleaseFeature11Slot<7>,
};

inline std::vector<renodx::utils::vtable::HookItem> MakeNgx11HookItems(int slot) {
  return {
      {"NVSDK_NGX_D3D11_CreateFeature",
       reinterpret_cast<void**>(&ngx11_slot_real[slot].create),
       reinterpret_cast<void*>(kNgx11CreateWrappers[slot])},
      {"NVSDK_NGX_D3D11_EvaluateFeature",
       reinterpret_cast<void**>(&ngx11_slot_real[slot].evaluate),
       reinterpret_cast<void*>(kNgx11EvaluateWrappers[slot])},
      {"NVSDK_NGX_D3D11_EvaluateFeature_C",
       reinterpret_cast<void**>(&ngx11_slot_real[slot].evaluate_c),
       reinterpret_cast<void*>(kNgx11EvalCWrappers[slot])},
      {"NVSDK_NGX_D3D11_ReleaseFeature",
       reinterpret_cast<void**>(&ngx11_slot_real[slot].release),
       reinterpret_cast<void*>(kNgx11ReleaseWrappers[slot])},
  };
}

// Caller holds runtime_mutex (InstallHooks' phase 2).  Same retry schedule
// and log budget as TryHookNgxModule.
inline bool TryHookNgx11Module(HMODULE module, const std::wstring& module_path) {
  if (module == nullptr || ngx11_absent_modules.count(module) != 0) return false;
  int slot = -1;
  for (int s = 0; s < kMaxNgxSlots; ++s) {
    if (ngx11_slot_used[s]) {
      if (ngx11_slot_module[s] == module) return false;
    } else if (slot < 0) {
      slot = s;
    }
  }
  if (slot < 0) return false;
  if (GetProcAddress(module, "NVSDK_NGX_D3D11_EvaluateFeature") == nullptr
      && GetProcAddress(module, "NVSDK_NGX_D3D11_EvaluateFeature_C") == nullptr) {
    ngx11_absent_modules.insert(module);
    return false;
  }
  const auto backing_off = ngx11_failed_modules.find(module);
  const bool retrying = backing_off != ngx11_failed_modules.end();
  if (retrying && present_generation < backing_off->second.next_attempt_present) {
    return false;
  }
  if (!renodx::utils::vtable::Hook(module, MakeNgx11HookItems(slot))) {
    ngx11_slot_real[slot] = {};
    NgxHookRetry& retry = ngx11_failed_modules[module];
    bool announce = false;
    const uint64_t delay = NoteHookInstallFailure(
        retry.attempts, retry.next_attempt_present, announce);
    if (announce) {
      std::ostringstream failure;
      failure << "detouring the NGX D3D11 exports FAILED for "
              << NarrowPath(module_path.c_str())
              << "; its D3D11 evaluates pass through without NR. Attempt "
              << retry.attempts << ", retrying in " << delay << " presents"
              << (delay == kNgxRetryCeilingPresents ? " and hourly after that"
                                                    : "");
      Log(reshade::log::level::warning, failure.str());
    }
    return false;
  }
  if (retrying) {
    Log(reshade::log::level::warning,
        "detouring the NGX D3D11 exports RECOVERED for "
            + NarrowPath(module_path.c_str()) + " after "
            + std::to_string(backing_off->second.attempts)
            + " failed attempt(s)");
    ngx11_failed_modules.erase(module);
  }
  ngx11_slot_used[slot] = true;
  ngx11_slot_module[slot] = module;
  std::wstring lower = module_path;
  for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
  ngx11_slot_noncore[slot].store(!IsCoreNgxModulePath(lower),
                                 std::memory_order_relaxed);
  ngx11_ever_hooked.store(true, std::memory_order_relaxed);
  Log(reshade::log::level::info,
      "detoured NGX D3D11 module copy [" + std::to_string(slot) + "] "
          + NarrowPath(module_path.c_str()));
  return true;
}

// UnhookInstalledDetours' D3D11 half.  Fully re-armable.
inline void UnhookNgx11Detours() {
  for (int s = 0; s < kMaxNgxSlots; ++s) {
    if (ngx11_slot_used[s]) {
      renodx::utils::vtable::Unhook(ngx11_slot_module[s], MakeNgx11HookItems(s));
    }
    ngx11_slot_used[s] = false;
    ngx11_slot_module[s] = nullptr;
    ngx11_slot_real[s] = {};
    ngx11_slot_noncore[s].store(false, std::memory_order_relaxed);
  }
  ngx11_failed_modules.clear();
  ngx11_absent_modules.clear();
  ngx11_any_hooked.store(false, std::memory_order_relaxed);
}

// The telemetry groups, stated only in a session whose D3D11 NGX exports
// were ever detoured, so every other session's line stays byte-identical.
// Caller holds runtime_mutex (EmitTelemetry).
inline std::string BridgeTelemetry() {
  if (!ngx11_ever_hooked.load(std::memory_order_relaxed)) return {};
  int copies = 0;
  uint64_t create = 0;
  uint64_t evaluate = 0;
  uint64_t evaluate_c = 0;
  uint64_t release = 0;
  for (int s = 0; s < kMaxNgxSlots; ++s) {
    if (ngx11_slot_used[s]) ++copies;
    create += ngx11_entry_create[s].load(std::memory_order_relaxed);
    evaluate += ngx11_entry_evaluate[s].load(std::memory_order_relaxed);
    evaluate_c += ngx11_entry_evaluate_c[s].load(std::memory_order_relaxed);
    release += ngx11_entry_release[s].load(std::memory_order_relaxed);
  }
  uint64_t bytes = 0;
  for (const auto& [_, set] : bridge_sets) bytes += set.bytes;
  size_t retired = 0;
  for (const BridgeRetired& entry : bridge_retired) {
    retired += entry.resources.size() + entry.descriptor_heaps.size();
  }
  const auto declines = [](NrDeclineReason reason) {
    return nr_decline_counts[static_cast<size_t>(reason)].load(
        std::memory_order_relaxed);
  };
  std::ostringstream out;
  out << " ngx11[copies=" << copies
      << " entered=" << (create + evaluate + evaluate_c + release)
      << " create=" << create << " eval=" << evaluate << " evalc=" << evaluate_c
      << " release=" << release
      << " deferred=" << declines(NrDeclineReason::kD3D11Deferred)
      << " absent=" << ngx11_absent_modules.size() << "]"
      << " bridge[wait=" << bridge_waits.load(std::memory_order_relaxed)
      << " signal=" << bridge_signals.load(std::memory_order_relaxed)
      << " submit=" << bridge_submits.load(std::memory_order_relaxed)
      << " timeouts=" << bridge_timeouts.load(std::memory_order_relaxed)
      << " skew=" << bridge_skew.load(std::memory_order_relaxed)
      << " wait11=" << bridge_waits11.load(std::memory_order_relaxed)
      << " state=" << BridgeStateName(bridge_state.load(std::memory_order_relaxed))
      << " busy=" << declines(NrDeclineReason::kBridgeBusy)
      << " sets=" << bridge_sets_built
      << " shared_mib=" << ((bytes + (1u << 20) - 1) >> 20) << " mode=copy]"
      << " surfaces[live=" << bridge_sets.size() << " retired=" << retired << "]";
  return out.str();
}
