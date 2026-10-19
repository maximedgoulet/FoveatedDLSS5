/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Private-data keys owned by ONE loaded copy of this addon (ENV-03, rc11).
//
// D3D12 private data lives on the object and is process-wide: every module
// that asks for a GUID gets the same slot.  Two copies of this addon in one
// game (a Debug build beside the release one, or two versions under
// different names) used the same GUIDs, so each read the other's entries as
// its own.  The list-state object is a C++ object: one copy used and freed
// the other's with its own layout, and the game died at exit with heap
// corruption (0xC0000374) - 2 of 2 runs with both copies and NR off, 0 of 2
// with either copy alone, 0 of 2 with both copies on the striped table
// (NRListStateMode=0), which keeps no private data (hostile harness,
// env03-debug-beside-release).  Folding the module's load address into the
// key gives every loaded copy its own slots; a copy's keys never change
// while it is loaded.

#pragma once

#include <windows.h>

#include <cstdint>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace renodx::addons::dlss5 {

inline GUID ModuleScopedGuid(GUID guid) {
  const auto base = reinterpret_cast<uint64_t>(&__ImageBase);
  for (int i = 0; i < 8; ++i) guid.Data4[i] ^= static_cast<uint8_t>(base >> (8 * i));
  return guid;
}

}  // namespace renodx::addons::dlss5
