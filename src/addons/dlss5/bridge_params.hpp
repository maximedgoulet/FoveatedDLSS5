/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The DX11 bridge's NGX parameter view (PLAN_DX11_V68.md 4.3).
//
// The D3D12 pipeline (ProcessInline, DeriveFeatureState) reads the game's
// NGX block only through the C++ virtual Get overloads, and reads resources
// only as ID3D12Resource**.  A D3D11 game's block holds ID3D11Resource
// pointers instead, and asking the real NGX block for a D3D12 resource under
// a key the game set as D3D11 is a type confusion nobody specified.  So the
// bridge never hands the game's block to the D3D12 pipeline.  It hands this
// view:
//
//   - scalar Gets (dimensions, subrects, jitter, motion scales, reset, create
//     flags, exposure scalars) forward to the game's D3D11 block unchanged;
//   - Get(ID3D12Resource**) answers the five resource keys with the bridge's
//     D3D12 twins and fails every other key - it never forwards, because the
//     game's block has no D3D12 resources to give;
//   - Get(void**) answers the same five keys with the twins and forwards
//     everything else (callbacks and other pointer-valued keys);
//   - every Set is ignored and counted.  The after-upscale pipeline writes
//     only its own NR block; a Set arriving here would mean D3D12 pointers
//     were about to land in a D3D11 block, which must never happen.  The
//     before-upscale pipeline's Color redirect (ProcessInlinePreSR) is one
//     such Set, and dropping it is the point: the bridge points the game's
//     D3D11 block at its own D3D11 twin instead (ScopedD3D11Redirect).
//
// The same header holds the other parameter block the addon provides itself,
// OwnedParameters (bottom): the NR feature's own block when the signed
// runtime is driven without a public NGX core, as the bridge's private
// device always is.
//
// Dependency-free apart from the NGX headers, so test/dlss5 pins it.

#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <type_traits>

#include <d3d11.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>

namespace renodx::addons::dlss5::bridge {

// The resource keys the D3D12 pipeline reads.  The order is the order of
// ParameterView's resource table.
enum class ResourceKey : uint8_t {
  kColor = 0,
  kOutput,
  kMotionVectors,
  kDepth,
  kExposureTexture,
  kCount,
};
inline constexpr std::size_t kResourceKeyCount =
    static_cast<std::size_t>(ResourceKey::kCount);
inline constexpr const char* kResourceKeyNames[kResourceKeyCount] = {
    NVSDK_NGX_Parameter_Color,         NVSDK_NGX_Parameter_Output,
    NVSDK_NGX_Parameter_MotionVectors, NVSDK_NGX_Parameter_Depth,
    NVSDK_NGX_Parameter_ExposureTexture,
};

// The table index of `name`, or -1 when it is not a resource key.
inline int ResourceKeyIndex(const char* name) {
  if (name == nullptr) return -1;
  for (std::size_t i = 0; i < kResourceKeyCount; ++i) {
    if (std::strcmp(name, kResourceKeyNames[i]) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// A resource the game put into its D3D11 block: typed first, then as void*,
// the way Streamline's DLSS plugin sets them.
inline ID3D11Resource* GetD3D11Resource(const NVSDK_NGX_Parameter* parameters,
                                        const char* name) {
  if (parameters == nullptr) return nullptr;
  ID3D11Resource* typed = nullptr;
  if (NVSDK_NGX_SUCCEED(parameters->Get(name, &typed)) && typed != nullptr) {
    return typed;
  }
  void* untyped = nullptr;
  if (NVSDK_NGX_SUCCEED(parameters->Get(name, &untyped)) && untyped != nullptr) {
    return static_cast<ID3D11Resource*>(untyped);
  }
  return nullptr;
}

class ParameterView final : public NVSDK_NGX_Parameter {
 public:
  // `resources` is indexed by ResourceKey; a null entry answers "not set".
  ParameterView(const NVSDK_NGX_Parameter* game,
                ID3D12Resource* const* resources)
      : game_(game) {
    for (std::size_t i = 0; i < kResourceKeyCount; ++i) {
      resources_[i] = resources != nullptr ? resources[i] : nullptr;
    }
  }

  uint32_t IgnoredSets() const { return ignored_sets_; }

  void Set(const char*, unsigned long long) override { ++ignored_sets_; }
  void Set(const char*, float) override { ++ignored_sets_; }
  void Set(const char*, double) override { ++ignored_sets_; }
  void Set(const char*, unsigned int) override { ++ignored_sets_; }
  void Set(const char*, int) override { ++ignored_sets_; }
  void Set(const char*, ID3D11Resource*) override { ++ignored_sets_; }
  void Set(const char*, ID3D12Resource*) override { ++ignored_sets_; }
  void Set(const char*, void*) override { ++ignored_sets_; }

  NVSDK_NGX_Result Get(const char* name, unsigned long long* out) const override {
    return Forward(name, out);
  }
  NVSDK_NGX_Result Get(const char* name, float* out) const override {
    return Forward(name, out);
  }
  NVSDK_NGX_Result Get(const char* name, double* out) const override {
    return Forward(name, out);
  }
  NVSDK_NGX_Result Get(const char* name, unsigned int* out) const override {
    return Forward(name, out);
  }
  NVSDK_NGX_Result Get(const char* name, int* out) const override {
    return Forward(name, out);
  }
  NVSDK_NGX_Result Get(const char* name, ID3D11Resource** out) const override {
    return Forward(name, out);
  }
  NVSDK_NGX_Result Get(const char* name, ID3D12Resource** out) const override {
    const int index = ResourceKeyIndex(name);
    if (out == nullptr || index < 0 || resources_[index] == nullptr) {
      return NVSDK_NGX_Result_Fail;
    }
    *out = resources_[index];
    return NVSDK_NGX_Result_Success;
  }
  NVSDK_NGX_Result Get(const char* name, void** out) const override {
    const int index = ResourceKeyIndex(name);
    if (index < 0) return Forward(name, out);
    if (out == nullptr || resources_[index] == nullptr) {
      return NVSDK_NGX_Result_Fail;
    }
    *out = resources_[index];
    return NVSDK_NGX_Result_Success;
  }
  void Reset() override { ++ignored_sets_; }

 private:
  template <typename T>
  NVSDK_NGX_Result Forward(const char* name, T* out) const {
    return game_ != nullptr ? game_->Get(name, out) : NVSDK_NGX_Result_Fail;
  }

  const NVSDK_NGX_Parameter* game_ = nullptr;
  ID3D12Resource* resources_[kResourceKeyCount] = {};
  uint32_t ignored_sets_ = 0;
};

// Points one resource key of the game's D3D11 block at a bridge twin for the
// game's own evaluate and puts the game's pointer back when it goes out of
// scope - the D3D11 form of the D3D12 path's PreSrSwap / RestorePreSrColor.
// Written through the overload the game's value answers to (typed first, as
// GetD3D11Resource reads), so the block holds the kind of value it held.
// A key the game never set is left alone.
class ScopedD3D11Redirect {
 public:
  ScopedD3D11Redirect() = default;
  ScopedD3D11Redirect(const ScopedD3D11Redirect&) = delete;
  ScopedD3D11Redirect& operator=(const ScopedD3D11Redirect&) = delete;
  ~ScopedD3D11Redirect() { Restore(); }

  void Point(const NVSDK_NGX_Parameter* game, const char* name,
             ID3D11Resource* replacement) {
    Restore();
    if (game == nullptr || name == nullptr) return;
    ID3D11Resource* typed = nullptr;
    void* untyped = nullptr;
    if (NVSDK_NGX_SUCCEED(game->Get(name, &typed)) && typed != nullptr) {
      typed_ = true;
      original_ = typed;
    } else if (NVSDK_NGX_SUCCEED(game->Get(name, &untyped)) && untyped != nullptr) {
      typed_ = false;
      original_ = untyped;
    } else {
      return;
    }
    // The block is the game's, handed in as const; the D3D12 path writes it
    // the same way (RestorePreSrColor).
    game_ = const_cast<NVSDK_NGX_Parameter*>(game);
    name_ = name;
    Write(replacement);
  }

  void Restore() {
    if (game_ == nullptr) return;
    Write(original_);
    game_ = nullptr;
  }

 private:
  void Write(void* value) {
    if (typed_) {
      game_->Set(name_, static_cast<ID3D11Resource*>(value));
    } else {
      game_->Set(name_, value);
    }
  }

  NVSDK_NGX_Parameter* game_ = nullptr;
  const char* name_ = nullptr;
  void* original_ = nullptr;
  bool typed_ = false;
};

// A map-backed parameter block the addon owns.  The signed NR runtime
// (310.8.0, sha256 E16BCF15...) exports NVSDK_NGX_D3D12_Init/CreateFeature/
// EvaluateFeature/ReleaseFeature but no AllocateParameters or
// DestroyParameters - the parameter allocator lives in the NGX core.  On a
// device the core was never initialized for (the bridge's private device,
// or a D3D12 process whose NGX is bundled rather than public) the NR
// feature's block therefore has to come from here.  The shape is the D0
// probe's MapParameter verbatim (test/dlss5_e2e/nrapiprobe.cpp), the block
// arms K1 and K2 drove feature 18 through on the signed runtime
// (PLAN_DX11_V68.md 4.7): numbers are stored once and read back as any
// numeric type, pointers as any pointer type, and a missing key answers
// FAIL_MissingInput.
class OwnedParameters final : public NVSDK_NGX_Parameter {
 public:
  void Set(const char* name, unsigned long long value) override { Store(name, value); }
  void Set(const char* name, float value) override { Store(name, value); }
  void Set(const char* name, double value) override { Store(name, value); }
  void Set(const char* name, unsigned int value) override { Store(name, value); }
  void Set(const char* name, int value) override { Store(name, value); }
  void Set(const char* name, ID3D11Resource* value) override { Store(name, value); }
  void Set(const char* name, ID3D12Resource* value) override { Store(name, value); }
  void Set(const char* name, void* value) override { Store(name, value); }

  NVSDK_NGX_Result Get(const char* name, unsigned long long* out) const override {
    return Load(name, out);
  }
  NVSDK_NGX_Result Get(const char* name, float* out) const override {
    return Load(name, out);
  }
  NVSDK_NGX_Result Get(const char* name, double* out) const override {
    return Load(name, out);
  }
  NVSDK_NGX_Result Get(const char* name, unsigned int* out) const override {
    return Load(name, out);
  }
  NVSDK_NGX_Result Get(const char* name, int* out) const override {
    return Load(name, out);
  }
  NVSDK_NGX_Result Get(const char* name, ID3D11Resource** out) const override {
    return Load(name, out);
  }
  NVSDK_NGX_Result Get(const char* name, ID3D12Resource** out) const override {
    return Load(name, out);
  }
  NVSDK_NGX_Result Get(const char* name, void** out) const override {
    return Load(name, out);
  }
  void Reset() override { entries_.clear(); }

 private:
  struct Entry {
    double number = 0.0;
    void* pointer = nullptr;
    bool has_number = false;
    bool has_pointer = false;
  };

  template <typename T>
  void Store(const char* name, T value) {
    if (name == nullptr) return;
    Entry& entry = entries_[name];
    if constexpr (std::is_pointer_v<T>) {
      entry.has_pointer = true;
      entry.pointer = value;
    } else {
      entry.has_number = true;
      entry.number = static_cast<double>(value);
    }
  }

  template <typename T>
  NVSDK_NGX_Result Load(const char* name, T* out) const {
    if (name == nullptr || out == nullptr) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const auto it = entries_.find(name);
    if (it == entries_.end()) return NVSDK_NGX_Result_FAIL_MissingInput;
    if constexpr (std::is_pointer_v<T>) {
      if (!it->second.has_pointer) return NVSDK_NGX_Result_FAIL_MissingInput;
      *out = static_cast<T>(it->second.pointer);
    } else {
      if (!it->second.has_number) return NVSDK_NGX_Result_FAIL_MissingInput;
      *out = static_cast<T>(it->second.number);
    }
    return NVSDK_NGX_Result_Success;
  }

  std::map<std::string, Entry, std::less<>> entries_;
};

// The allocator pair EnsureDirectRuntime installs when the runtime exports
// none, with the NGX allocator's signatures.
inline NVSDK_NGX_Result NVSDK_CONV AllocateOwnedParameters(NVSDK_NGX_Parameter** out) {
  if (out == nullptr) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  *out = new (std::nothrow) OwnedParameters();
  // NGX names no host-memory failure; the generic one is the honest code.
  return *out != nullptr ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
}

inline NVSDK_NGX_Result NVSDK_CONV DestroyOwnedParameters(NVSDK_NGX_Parameter* parameters) {
  delete static_cast<OwnedParameters*>(parameters);
  return NVSDK_NGX_Result_Success;
}

}  // namespace renodx::addons::dlss5::bridge
