/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// NR look stage (PLAN_NR_LOOK_V71.md): reshapes the network's EDIT
// e = log2(n / p) at NR resolution and writes N' = n * 2^(t * (e' - e)),
// which every resolve then consumes unchanged, so one set of controls covers
// the SDR, scRGB and PQ codecs alike.
//
// DLSSNR is a Neural Rendering enhancer; nothing here denoises.
//
// This header is the stage's API boundary: the root signature, the constants
// the shaders read (shaders/look_common.hlsli), the per-frame constants
// derived from the settings, and the dispatch recording.  Resources, their
// states and the descriptor heap belong to the caller (dlssnr.hpp's
// workset), so test/dlss5_gpu drives exactly this code against the same
// shaders.

#pragma once

#include <d3d12.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace renodx::addons::dlss5::look {

// Constants.Flags bits; equal to look_common.hlsli.
constexpr uint32_t kFlagBands = 1u << 0;
constexpr uint32_t kFlagTemporal = 1u << 1;
constexpr uint32_t kFlagMotion = 1u << 2;
constexpr uint32_t kFlagTemporalDetail = 1u << 3;
constexpr uint32_t kFlagTransport = 1u << 4;
constexpr uint32_t kFlagRefLinear = 1u << 5;
constexpr uint32_t kFlagHistoryValid = 1u << 6;

// Root parameters.
enum : uint32_t {
  kRootSrvSet = 0,      // t0..t3: a codec per-pass set or the pass's look set
  kRootUavTable = 1,    // u0..u8: kUav* below
  kRootConstants = 2,   // b0: Constants
  kRootHistoryIn = 3,   // u10: root UAV
  kRootHistoryOut = 4,  // u9: root UAV
  kRootMotion = 5,      // t4: one motion-vector view
  kRootParameterCount = 6,
};

// The UAV table, relative to its base.
enum : uint32_t {
  kUavOutput = 0,       // N' at NR res (RGBA16F), alpha = G_low with transport
  kUavBandA = 1,        // half NR res, RGBA32F
  kUavBandB = 2,        // half NR res, RGBA32F
  kUavBandMaxA = 3,     // half NR res, R32F
  kUavBandMaxB = 4,     // half NR res, R32F
  kUavUpProxy = 5,      // output res, RGBA16F
  kUavUpNeural = 6,     // output res, RGBA16F
  kUavTraceHistogram = 7,  // raw buffer, kTraceWords uints
  kUavTraceBlocks = 8,  // one RGBA32F texel per 32x32 NR block
  kUavCount = 9,
};

constexpr uint32_t kTraceBins = 128;
// 128 bins of a over [-4, 4) at 1/16 stop, then the pixel count, |a| > 1,
// halo pixels, and one spare word (look_trace.cs_5_1.hlsl).
constexpr uint32_t kTraceWords = kTraceBins + 4;
constexpr float kTraceLow = -4.f;
constexpr float kTraceStep = 1.f / 16.f;
constexpr uint32_t kTraceBlock = 32;
constexpr uint32_t kHistoryBytesPerPixel = 12;
// dt above this breaks the history (a stall, a loading screen, a pause).
constexpr float kHistoryMaxGapSeconds = 0.25f;

// cbuffer LookConstants, dword for dword.
struct Constants {
  uint32_t size[2];
  uint32_t nr_size[2];
  uint32_t full_size[2];
  uint32_t flags;
  uint32_t mode;
  float strength;
  float brighten;
  float darken;
  float max_brighten;
  float max_darken;
  float colour;
  float hue;
  float max_colour;
  float shadows;
  float midtones;
  float highlights;
  float tone;
  float detail;
  float halo;
  uint32_t radius;
  float alpha;
  float motion_scale[2];
  uint32_t motion_base[2];
  uint32_t motion_grid[2];
  float gf_eps;
  uint32_t padding0;
  // Foveated working grid (foveation.hlsli), dwords 32..35: packed X/Y
  // (0 = uniform axis) and the host-computed G(1) per axis.  HLSL keeps the
  // uint2 on a 16-byte register, hence padding0.
  uint32_t foveation[2];
  float foveation_gmax[2];
};
static_assert(
    sizeof(Constants) == 36 * sizeof(uint32_t),
    "the look root signature declares exactly 36 32-bit constants");
static_assert(offsetof(Constants, foveation) == 32 * sizeof(uint32_t),
              "LookConstants.Foveation is dword 32");

// The player's look values.  Every default is the identity.
struct Settings {
  float strength = 1.f;
  float brighten = 1.f;
  float darken = 1.f;
  float max_brighten = 0.f;  // stops; 0 = off
  float max_darken = 0.f;
  float colour = 1.f;
  float hue = 1.f;
  float max_colour = 0.f;
  float shadows = 1.f;
  float midtones = 1.f;
  float highlights = 1.f;
  float tone = 1.f;
  float detail = 1.f;
  float halo = 0.f;
  float detail_radius = 1.f;  // percent of the NR height
  uint32_t stabilize = 0;     // 0 off, 1 static, 2 motion-compensated
  float stabilize_ms = 60.f;
  bool stabilize_detail = false;
  uint32_t upsample = 0;      // 0 classic, 1 edge-aware
};

// Every per-pixel gain is the identity: the compose would return N.
inline bool GainsNeutral(const Settings& s) {
  return s.strength == 1.f && s.brighten == 1.f && s.darken == 1.f
      && s.max_brighten <= 0.f && s.max_darken <= 0.f && s.colour == 1.f
      && s.hue == 1.f && s.max_colour <= 0.f && s.shadows == 1.f
      && s.midtones == 1.f && s.highlights == 1.f && s.tone == 1.f
      && s.detail == 1.f && s.halo <= 0.f;
}

inline bool BandsWanted(const Settings& s) {
  return s.tone != 1.f || s.detail != 1.f || s.halo > 0.f || s.stabilize != 0;
}

// Edge-aware transport only exists where NR runs below the output.
inline bool TransportWanted(
    const Settings& s, uint32_t nr_width, uint32_t nr_height,
    uint32_t full_width, uint32_t full_height) {
  return s.upsample == 1u && (nr_width != full_width || nr_height != full_height);
}

// Neutral means NOT dispatched: with every gain the identity and no temporal
// or transport work the resolve reads N itself, bit for bit (gate 3b).
inline bool Dispatched(const Settings& s, bool transport) {
  return !GainsNeutral(s) || s.stabilize != 0 || transport;
}

// Per-frame inputs the settings cannot know.
struct Frame {
  uint32_t nr_width = 0;
  uint32_t nr_height = 0;
  uint32_t full_width = 0;
  uint32_t full_height = 0;
  bool reference_linear = false;  // HDR worksets reference linear surfaces
  bool transport = false;
  float frame_seconds = 1.f / 60.f;
  bool history_valid = false;
  bool motion = false;
  float motion_scale[2] = {0.f, 0.f};  // MV texel -> NR pixels
  uint32_t motion_base[2] = {0, 0};
  uint32_t motion_grid[2] = {1, 1};
  // Foveated working grid (foveation.hpp): packed X/Y, 0 = uniform axis, and
  // G(1) per axis.  Kept last so the designated initializers that build a
  // Frame stay in member order.
  uint32_t foveation[2] = {0, 0};
  float foveation_gmax[2] = {1.f, 1.f};
};

inline uint32_t BandRadius(const Settings& s, uint32_t nr_height) {
  const float half_pixels = s.detail_radius * 0.01f * static_cast<float>(nr_height) * 0.5f;
  return std::clamp(static_cast<uint32_t>(std::lround(half_pixels)), 1u, 64u);
}

inline Constants MakeConstants(const Settings& s, const Frame& f) {
  Constants c{};
  c.size[0] = f.nr_width;
  c.size[1] = f.nr_height;
  c.nr_size[0] = f.nr_width;
  c.nr_size[1] = f.nr_height;
  c.full_size[0] = f.full_width;
  c.full_size[1] = f.full_height;
  const bool temporal = s.stabilize != 0;
  c.flags = (BandsWanted(s) ? kFlagBands : 0u)
      | (temporal ? kFlagTemporal : 0u)
      | (temporal && s.stabilize == 2u && f.motion ? kFlagMotion : 0u)
      | (temporal && s.stabilize_detail ? kFlagTemporalDetail : 0u)
      | (f.transport ? kFlagTransport : 0u)
      | (f.reference_linear ? kFlagRefLinear : 0u)
      | (temporal && f.history_valid ? kFlagHistoryValid : 0u);
  c.strength = s.strength;
  c.brighten = s.brighten;
  c.darken = s.darken;
  c.max_brighten = s.max_brighten;
  c.max_darken = s.max_darken;
  c.colour = s.colour;
  c.hue = s.hue;
  c.max_colour = s.max_colour;
  c.shadows = s.shadows;
  c.midtones = s.midtones;
  c.highlights = s.highlights;
  c.tone = s.tone;
  c.detail = s.detail;
  c.halo = s.halo;
  c.radius = BandRadius(s, f.nr_height);
  // Framerate-independent exponential blend: the same time constant gives
  // the same response in milliseconds at 30 and at 120 fps.
  c.alpha = 1.f - std::exp(-f.frame_seconds / std::max(s.stabilize_ms * 1e-3f, 1e-3f));
  c.motion_scale[0] = f.motion_scale[0];
  c.motion_scale[1] = f.motion_scale[1];
  c.motion_base[0] = f.motion_base[0];
  c.motion_base[1] = f.motion_base[1];
  c.motion_grid[0] = std::max(f.motion_grid[0], 1u);
  c.motion_grid[1] = std::max(f.motion_grid[1], 1u);
  c.gf_eps = 0.01f;
  c.foveation[0] = f.foveation[0];
  c.foveation[1] = f.foveation[1];
  c.foveation_gmax[0] = f.foveation_gmax[0];
  c.foveation_gmax[1] = f.foveation_gmax[1];
  return c;
}

// Builds and serializes the look root signature.  `serialize` is the
// caller's D3D12SerializeRootSignature (the addon resolves it at runtime).
inline HRESULT SerializeRootSignature(
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize,
    ID3DBlob** blob,
    ID3DBlob** errors) {
  D3D12_DESCRIPTOR_RANGE ranges[3]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 4;
  ranges[0].BaseShaderRegister = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = kUavCount;
  ranges[1].BaseShaderRegister = 0;
  ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[2].NumDescriptors = 1;
  ranges[2].BaseShaderRegister = 4;
  D3D12_ROOT_PARAMETER parameters[kRootParameterCount]{};
  parameters[kRootSrvSet].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[kRootSrvSet].DescriptorTable.NumDescriptorRanges = 1;
  parameters[kRootSrvSet].DescriptorTable.pDescriptorRanges = &ranges[0];
  parameters[kRootUavTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[kRootUavTable].DescriptorTable.NumDescriptorRanges = 1;
  parameters[kRootUavTable].DescriptorTable.pDescriptorRanges = &ranges[1];
  parameters[kRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[kRootConstants].Constants.Num32BitValues = sizeof(Constants) / 4;
  parameters[kRootConstants].Constants.ShaderRegister = 0;
  parameters[kRootHistoryIn].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[kRootHistoryIn].Descriptor.ShaderRegister = 10;
  parameters[kRootHistoryOut].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[kRootHistoryOut].Descriptor.ShaderRegister = 9;
  parameters[kRootMotion].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[kRootMotion].DescriptorTable.NumDescriptorRanges = 1;
  parameters[kRootMotion].DescriptorTable.pDescriptorRanges = &ranges[2];
  D3D12_ROOT_SIGNATURE_DESC desc{};
  desc.NumParameters = kRootParameterCount;
  desc.pParameters = parameters;
  return serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob, errors);
}

struct Pipeline {
  ID3D12RootSignature* root_signature = nullptr;
  ID3D12PipelineState* compose = nullptr;
  ID3D12PipelineState* band = nullptr;
  ID3D12PipelineState* upsample = nullptr;
  ID3D12PipelineState* trace = nullptr;
};

// Everything one pass's look dispatches bind.  Every table and root
// descriptor must be valid even where a program's path does not read it
// (the shaders declare the full layout): the caller points unused history at
// any buffer in UAV state and unused motion at any SRV.
struct Bindings {
  ID3D12DescriptorHeap* heap = nullptr;
  D3D12_GPU_DESCRIPTOR_HANDLE codec_set{};  // [ref, P, N, ref|norm]
  D3D12_GPU_DESCRIPTOR_HANDLE look_set{};   // [ref, P, N', ref|norm]
  D3D12_GPU_DESCRIPTOR_HANDLE uav_table{};  // kUav* region
  D3D12_GPU_DESCRIPTOR_HANDLE motion{};
  D3D12_GPU_VIRTUAL_ADDRESS history_in = 0;
  D3D12_GPU_VIRTUAL_ADDRESS history_out = 0;
};

inline void Bind(
    ID3D12GraphicsCommandList* list, const Pipeline& pipeline,
    ID3D12PipelineState* program, const Bindings& bindings,
    D3D12_GPU_DESCRIPTOR_HANDLE srv_set, const Constants& constants) {
  ID3D12DescriptorHeap* heaps[] = {bindings.heap};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(pipeline.root_signature);
  list->SetPipelineState(program);
  list->SetComputeRootDescriptorTable(kRootSrvSet, srv_set);
  list->SetComputeRootDescriptorTable(kRootUavTable, bindings.uav_table);
  list->SetComputeRoot32BitConstants(
      kRootConstants, sizeof(Constants) / 4, &constants, 0);
  list->SetComputeRootUnorderedAccessView(kRootHistoryIn, bindings.history_in);
  list->SetComputeRootUnorderedAccessView(kRootHistoryOut, bindings.history_out);
  list->SetComputeRootDescriptorTable(kRootMotion, bindings.motion);
}

// Every look pass reads what the previous one wrote through UAVs only, so a
// global UAV barrier orders them without naming each surface.
inline void UavBarrierAll(ID3D12GraphicsCommandList* list) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  list->ResourceBarrier(1, &barrier);
}

// Group D (and E, which filters the bands): the five band passes at half NR
// resolution, then the compose, which leaves N' in kUavOutput (UAV state;
// the caller transitions it for the resolve).
inline void RecordCompose(
    ID3D12GraphicsCommandList* list, const Pipeline& pipeline,
    const Bindings& bindings, Constants constants) {
  if ((constants.flags & (kFlagBands | kFlagTemporal)) != 0u) {
    Constants band = constants;
    band.size[0] = (constants.nr_size[0] + 1) / 2;
    band.size[1] = (constants.nr_size[1] + 1) / 2;
    for (uint32_t mode = 0; mode < 5; ++mode) {
      band.mode = mode;
      Bind(list, pipeline, pipeline.band, bindings, bindings.codec_set, band);
      list->Dispatch((band.size[0] + 7) / 8, (band.size[1] + 7) / 8, 1);
      UavBarrierAll(list);
    }
  }
  constants.mode = 0;
  constants.size[0] = constants.nr_size[0];
  constants.size[1] = constants.nr_size[1];
  Bind(list, pipeline, pipeline.compose, bindings, bindings.codec_set, constants);
  list->Dispatch((constants.size[0] + 15) / 16, (constants.size[1] + 15) / 16, 1);
  UavBarrierAll(list);
}

// Group F: P_up and N'_up at the output resolution, read through the look
// set (N' must be in a shader-resource state by now).
inline void RecordUpsample(
    ID3D12GraphicsCommandList* list, const Pipeline& pipeline,
    const Bindings& bindings, Constants constants) {
  constants.mode = 0;
  constants.size[0] = constants.full_size[0];
  constants.size[1] = constants.full_size[1];
  Bind(list, pipeline, pipeline.upsample, bindings, bindings.look_set, constants);
  list->Dispatch((constants.size[0] + 15) / 16, (constants.size[1] + 15) / 16, 1);
  UavBarrierAll(list);
}

// Group I: clear, then reduce the raw edit into the histogram and the block
// statistics (observe-only; runs with the look off too).
inline void RecordTrace(
    ID3D12GraphicsCommandList* list, const Pipeline& pipeline,
    const Bindings& bindings, Constants constants) {
  constants.size[0] = constants.nr_size[0];
  constants.size[1] = constants.nr_size[1];
  constants.mode = 0;
  Bind(list, pipeline, pipeline.trace, bindings, bindings.codec_set, constants);
  list->Dispatch(1, 1, 1);
  UavBarrierAll(list);
  constants.mode = 1;
  list->SetComputeRoot32BitConstants(
      kRootConstants, sizeof(Constants) / 4, &constants, 0);
  list->Dispatch(
      (constants.nr_size[0] + kTraceBlock - 1) / kTraceBlock,
      (constants.nr_size[1] + kTraceBlock - 1) / kTraceBlock, 1);
  UavBarrierAll(list);
}

}  // namespace renodx::addons::dlss5::look
