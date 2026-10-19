/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// What ExecuteIndirect leaves behind, derived from the command signature.
//
// The whole of it is one documented rule, quoted from Microsoft Learn,
// "Indirect Drawing" (learn.microsoft.com/windows/win32/direct3d12/
// indirect-drawing), fetched 2026-09-20:
//
//     "No command signature state leaks back to the command list."
//
// and, for the arguments a signature does name, the same page's aftermath
// table: a root constant the signature sets reads 0 after the call, a root
// view it sets is a NULL view, a vertex or index buffer it binds is NULL.
// The page also states that a signature is either a graphics signature or a
// compute signature - it "must contain exactly one draw or dispatch
// argument" - so a draw-style indirect cannot touch compute root state at
// all.
//
// That makes the aftermath EXACT, and exactness is the whole point: v6.7.0
// shipped the safe-looking approximation ("ExecuteIndirect invalidates all
// compute root knowledge") and Alan Wake 2 declined 912 of 912 evaluates,
// because a GPU-driven engine issues tens of thousands of indirects between
// its last bind and the evaluate.  Reproduced without a game by the T2
// `gpu_driven_indirect` profile, where v6.7.0 and v6.7.3-recovery1 engage on
// 0 % of evaluates.  The opposite approximation - v6.5.3's, which is to not
// watch ExecuteIndirect at all - is not safe either: the same profile shows
// the addon restoring its own stale observation over a root constant D3D12
// had zeroed, on 239 of 240 frames.  Only the exact rule passes both halves.
//
// This header is deliberately free of addon dependencies (d3d12.h and the
// standard library only) so test/dlss5 can include it and pin the decision
// as a pure function of the SDK's own descriptor.

#pragma once

#include <cstdint>

#include <d3d12.h>

namespace renodx::addons::dlss5 {

// Root arguments a single signature may name before it is treated as
// unknown.  A signature is limited by D3D12 to one draw/dispatch plus the
// arguments it sets; real ones name a handful.  A signature that needs more
// than this is counted, not guessed at.
constexpr unsigned int kMaxNamedIndirectArguments = 16;

// One root parameter a command signature writes.  `constant_count == 0`
// means a root view (CBV/SRV/UAV): its aftermath is a NULL view, which the
// model expresses by forgetting the slot, so that the restore leaves the
// NULL in place instead of replaying an address D3D12 has already dropped.
struct NamedIndirectArgument {
  std::uint8_t root_parameter_index = 0;
  std::uint8_t constant_offset = 0;
  std::uint8_t constant_count = 0;
};

struct IndirectSignatureEffect {
  enum class Class : std::uint8_t {
    kUnknown,   // no terminal draw/dispatch argument found - do not act on it
    kGraphics,  // DRAW / DRAW_INDEXED / DISPATCH_MESH: no compute effect
    kCompute,   // DISPATCH / DISPATCH_RAYS: named compute root arguments reset
  };

  Class signature_class = Class::kUnknown;
  // False when something in the descriptor did not fit the fixed shape above
  // (too many named arguments, a root parameter index or constant range out
  // of the range D3D12 allows).  The caller counts these and changes
  // nothing: acting on a partial description would reset some slots and
  // silently keep others stale, which is worse than either whole answer.
  bool complete = false;
  std::uint8_t named_count = 0;
  NamedIndirectArgument named[kMaxNamedIndirectArguments] = {};

  // True when this call cannot change any compute root argument, so the hot
  // path has nothing to do.  A graphics signature and a dispatch-only
  // compute signature are both this case, and together they are the large
  // majority of a GPU-driven engine's indirects.
  bool NoComputeEffect() const noexcept {
    return complete
        && (signature_class == Class::kGraphics || named_count == 0);
  }
};

// Pure decision over the SDK's own descriptor.  Nothing here reads or writes
// device state, which is what lets test/dlss5 pin every row of it.
inline IndirectSignatureEffect DescribeCommandSignature(
    const D3D12_COMMAND_SIGNATURE_DESC& desc) {
  IndirectSignatureEffect effect;
  if (desc.pArgumentDescs == nullptr) return effect;
  bool fits = true;
  for (UINT i = 0; i < desc.NumArgumentDescs; ++i) {
    const D3D12_INDIRECT_ARGUMENT_DESC& argument = desc.pArgumentDescs[i];
    switch (argument.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:
        effect.signature_class = IndirectSignatureEffect::Class::kGraphics;
        break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS:
        // DispatchRays reads its global root arguments from the COMPUTE root
        // signature (MS, "Direct3D 12 Raytracing - bindings"), so a
        // ray-dispatch signature names the same slots a dispatch one does.
        effect.signature_class = IndirectSignatureEffect::Class::kCompute;
        break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
      case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:
        // Graphics-only bindings; they cannot appear in a compute signature
        // and the compute shadow models nothing of them.
        break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: {
        if (effect.named_count >= kMaxNamedIndirectArguments) {
          fits = false;
          break;
        }
        // The three view types share one union member layout
        // (RootParameterIndex first); CONSTANT carries the range as well.
        const UINT root_parameter_index =
            argument.Type == D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT
                ? argument.Constant.RootParameterIndex
                : argument.ConstantBufferView.RootParameterIndex;
        if (root_parameter_index >= 64) {
          fits = false;
          break;
        }
        NamedIndirectArgument& named = effect.named[effect.named_count];
        named.root_parameter_index =
            static_cast<std::uint8_t>(root_parameter_index);
        if (argument.Type == D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT) {
          const UINT offset = argument.Constant.DestOffsetIn32BitValues;
          const UINT count = argument.Constant.Num32BitValuesToSet;
          // A root signature is bounded at 64 DWORDs, so a legal constant
          // range always fits; anything else is a descriptor this code has
          // no business interpreting.
          if (count == 0 || count > 64 || offset >= 64 || offset + count > 64) {
            fits = false;
            break;
          }
          named.constant_offset = static_cast<std::uint8_t>(offset);
          named.constant_count = static_cast<std::uint8_t>(count);
        } else {
          named.constant_count = 0;  // a root view: aftermath is a NULL view
        }
        ++effect.named_count;
        break;
      }
      default:
        // An argument type this SDK did not know about: say so rather than
        // assume it changes nothing.
        fits = false;
        break;
    }
    if (!fits) break;
  }
  effect.complete =
      fits && effect.signature_class != IndirectSignatureEffect::Class::kUnknown;
  return effect;
}

}  // namespace renodx::addons::dlss5
