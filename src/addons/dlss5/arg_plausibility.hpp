/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// A-1 (PLAN_V7_RELEASE.md §3): may an NGX entry's first argument be
// dereferenced?
//
// The Skyrim SE crash (docs/memory/legacy-crash-classes.md, class A) was this
// addon's detour executing `mov rcx,[rcx]` on a first argument of 0xFFFFFFFF.
// A third-party caller had passed it to a D3D12 NGX export in a D3D11 game.
// Since alpha41 the D3D11-presenting branch of EnsureForeignEntryInstalls
// calls GetDevice on that argument too, and it only checks for null. A detour
// must never crash on its caller's garbage: the real export decides what the
// argument means and returns its own error for a bad one.
//
// Tier 1, enforced: the object and the part of its vtable the addon reads
// must lie in committed, readable, non-guard memory. If not, the wrapper
// skips every dereference of its own and passes the call to the real export
// untouched.
// Tier 2, observe-only: the vtable lies outside every loaded image. A COM
// vtable lives in a module's read-only data, so this is counted, never acted
// on.
//
// Only VirtualQuery is used. It makes no loader call, so the check is safe
// under any lock the caller holds, and it costs two kernel calls per entry.
// It cannot tell a well-formed stale pointer from a live one; the class it
// closes is the sentinel and the unmapped address.

#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace renodx::addons::dlss5 {

enum class ArgPlausibility : std::uint8_t {
  kPlausible = 0,   // object and vtable readable, vtable inside an image
  kVtableNotImage,  // Tier 2: readable, but the vtable is not in an image
  kImplausible,     // Tier 1: must not be dereferenced
};

// True when every byte of [address, address + bytes) is committed, readable
// and not a guard page. `in_image` reports whether the first byte belongs to
// a mapped image.
//
// The span may cross regions. VirtualQuery ends a region wherever the page
// attributes change, so a legitimate vtable straddles two regions when some
// tool has re-protected one of its pages (an overlay patching a vtable in
// .rdata; a written copy-on-write page becomes READWRITE). Rejecting that
// would skip a real evaluate, so each region the span touches is checked in
// turn.
inline bool ReadableSpan(const void* address, std::size_t bytes,
                         bool* in_image = nullptr) {
  if (address == nullptr || bytes == 0) return false;
  const auto begin = reinterpret_cast<std::uintptr_t>(address);
  const std::uintptr_t end = begin + bytes;
  if (end < begin) return false;
  constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
      | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  std::uintptr_t cursor = begin;
  // A vtable span is at most a few pages; the cap bounds the walk.
  for (int regions = 0; cursor < end && regions < 8; ++regions) {
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info))
        != sizeof(info)) {
      return false;
    }
    if (info.State != MEM_COMMIT) return false;
    if ((info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    if ((info.Protect & kReadable) == 0) return false;
    if (cursor == begin && in_image != nullptr) {
      *in_image = info.Type == MEM_IMAGE;
    }
    const auto region_end =
        reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (region_end <= cursor) return false;
    cursor = region_end;
  }
  return cursor >= end;
}

// `vtable_bytes` is how much of the vtable the caller goes on to read: its
// highest slot plus one, times the pointer size.
inline ArgPlausibility ClassifyComArgument(const void* object,
                                           std::size_t vtable_bytes) {
  if (!ReadableSpan(object, sizeof(void*))) return ArgPlausibility::kImplausible;
  const void* vtable = *static_cast<const void* const*>(object);
  bool in_image = false;
  if (!ReadableSpan(vtable, vtable_bytes, &in_image)) {
    return ArgPlausibility::kImplausible;
  }
  return in_image ? ArgPlausibility::kPlausible
                  : ArgPlausibility::kVtableNotImage;
}

}  // namespace renodx::addons::dlss5
