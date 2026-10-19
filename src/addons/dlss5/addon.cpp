/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#if defined(RENODX_DLSS5_DEBUG)
// Debug variant: enable ReShade-side utils debug logging alongside the
// addon's own trace log + crash dumps (see debug.hpp).
#define DEBUG_LEVEL_2
#else
#define DEBUG_LEVEL_0
#endif

#include <windows.h>

#include "dlssnr.hpp"

#if defined(RENODX_DLSS5_DEBUG)
extern "C" __declspec(dllexport) constexpr const char* NAME =
    "DLSS 5 Neural Rendering (Debug)";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION =
    "Generic experimental DLSS Neural Rendering post-pass for DX12 games using "
    "NGX or Streamline DLSS (DEBUG BUILD: trace log + crash minidumps - see "
    "RenoDX-DLSS5-debug.log next to the game exe)";
#else
extern "C" __declspec(dllexport) constexpr const char* NAME = "DLSS 5 Neural Rendering";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION =
    "Generic experimental DLSS Neural Rendering post-pass for DX12 games using NGX or Streamline DLSS";
#endif

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    if (!reshade::register_addon(module)) return FALSE;
  }

  renodx::addons::dlss5::Use(reason, module, reserved);

  if (reason == DLL_PROCESS_DETACH) {
    reshade::unregister_addon(module);
    if (renodx::addons::dlss5::lastgasp::dumps_on.load()) {
      renodx::addons::dlss5::lastgasp::Exit(reserved != nullptr);
      if (reserved == nullptr) renodx::addons::dlss5::lastgasp::Uninstall();
    }
  }
  return TRUE;
}
