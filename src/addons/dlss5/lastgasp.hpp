/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The release-build "last gasp" crash reporter.  Every build includes this
// header; debug.hpp's crash facility (minidumps, symbols, watchdog) stays
// DEBUG-only and is NOT enabled here - only the pieces a field report needs:
// one line in RenoDX-DLSS5-crash.log next to the game exe naming the
// faulting module, and the last release marks (the release stage the addon
// died in).
//
// Why a last-chance SetUnhandledExceptionFilter, not a vectored handler: the
// filter runs exactly once, for the exception that is about to kill the
// process.  A vectored handler (AddVectoredExceptionHandler) fires for every
// first-chance exception in the process, which in a game means the engine's
// own C++ EH (0xE06D7363), breakpoints, guard pages and heap probes - each
// one would need continuation semantics we do not own, and one misjudged
// benign code either floods the log or interferes with the game's recovery.
// At last chance there are no false positives: anything that arrives here is
// a real death, so every code is recorded (the owner's set - 0xC0000005,
// 0xC0000409, 0xC000001D, 0xC00000FD, 0xC0000374 - and anything else that
// got this far).  Known gaps, stated so support can recognize them: a
// genuine __fastfail (int 29h) never reaches user-mode dispatch at all
// (neither filter nor vectored; WER only), and a host that replaces the
// process's unhandled filter after we install ours takes the slot with it.
//
// Handler constraints (domain, not style): the crashing thread's heap, CRT
// file layer and possibly its locks are dead or held, so the handler only
// formats into a stack buffer (snprintf does not allocate) and writes with
// WriteFile/FlushFileBuffers on the handle pre-opened at install.  The file
// write comes BEFORE the best-effort reshade::log line, because that call
// can deadlock on ReShade's log mutex if the crashing thread held it - the
// file is the guarantee, the log line is a bonus.  The handler never
// swallows a crash: it hands it to the filter that was installed before it
// (the host's own crash reporter - RE Engine and most engines register one
// at start-up, before any add-on loads), or continues the search to WER when
// there was none.  Until v8.0.2 it returned EXCEPTION_CONTINUE_SEARCH
// unconditionally, which at last chance does NOT reach the previous filter:
// installing this reporter took the host's crash handling away.
//
// v8.0.2: the first-chance recorder beside it.  The filter slot is one per
// process and the host decides who holds it: Unreal Engine installs its crash
// reporter after the addon has loaded, Special K keeps the slot for its own
// handler - and the v8.0.1 field crash log from Code Vein II (UE5) carried the
// session line and nothing else.  The vectored list cannot be taken over
// that way, and a vectored handler sees the fault on the faulting thread
// before any frame handler or filter decides its fate.  The noise the
// paragraph above warns about is kept out by scope, not by judgement: the
// recorder only RECORDS (always EXCEPTION_CONTINUE_SEARCH, so nothing about
// the host's handling changes), only fatal-class codes, only faults inside
// this addon, an NGX or Streamline image, or an execute of an address that
// lies in no image (a freed trampoline, an unloaded module); faults this
// addon's own SEH wraps catch on purpose are skipped, repeats of the last
// address are not rewritten, and the process gets kFirstChanceLines lines.
// Since v8.5.0-rc9 one more code is in scope: a C++ throw on the thread
// inside an InsideCall (FirstChanceHandler).
// A "[first-chance]" line says the fault HAPPENED; a host that handled it
// leaves the line and a live process, so the last line before a session's
// end is the one to read.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Types only: MiniDumpWriteDump is resolved at run time (PrepareDumps).
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>

#include <include/reshade.hpp>

namespace renodx::addons::dlss5 {
namespace lastgasp {

// ---------------------------------------------------------------------------
// Release marks: a 16-entry ring of stage names, printed by the crash line.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kMarkSlots = 16;

// Every pointer stored here must have static storage duration (call sites
// pass string literals): the handler reads them after the crash, with no way
// to know whether a temporary's owner still lives.  Zero-initialized static
// storage gives the null slots.
inline std::atomic<const char*> marks[kMarkSlots] = {};
// Total marks ever recorded (wraps harmlessly: 2^32 is a multiple of 16).
inline std::atomic<uint32_t> mark_count{0};

inline void Record(const char* stage) noexcept {
  const uint32_t ticket = mark_count.fetch_add(1, std::memory_order_relaxed);
  marks[ticket & (kMarkSlots - 1)].store(stage, std::memory_order_release);
}

// A stage transition (fires once or rarely at its site).
inline void Mark(const char* stage) noexcept {
  Record(stage);
}

// A milestone whose site fires on every occurrence (per device, per
// evaluate): recorded the first time only, so the ring keeps the distinct
// stages instead of the repeats.  The scan is advisory - a concurrent first
// call can record twice - but the duplicates are the same literal.
inline void MarkOnce(const char* stage) noexcept {
  for (const auto& slot : marks) {
    if (slot.load(std::memory_order_relaxed) == stage) return;
  }
  Record(stage);
}

// v8.5.0-rc8: the foreign call the add-on is inside right now (a literal, or
// null) and the thread making it, printed on every fault line as
// " inside=<call> tid=<n>".  Endfield rc4/rc5 died of an unhandled C++ throw
// while one thread sat in NVIDIA's feature-18 create - no create outcome was
// ever logged - and the crash line could not say whether the dying thread was
// that one.  A slot, not marks: a create per resize would push the startup
// milestones out of the 16-entry ring.
inline std::atomic<const char*> inside_call{nullptr};
inline std::atomic<DWORD> inside_call_tid{0};

// Declared after the call's NGX turn (DirectCallScope), so the slot has one
// writer.  An exception that escapes the call reaches the filter before any
// unwind, with the slot still set.
struct InsideCall {
  explicit InsideCall(const char* call) noexcept {
    inside_call_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    inside_call.store(call, std::memory_order_release);
  }
  ~InsideCall() { inside_call.store(nullptr, std::memory_order_release); }
  InsideCall(const InsideCall&) = delete;
  InsideCall& operator=(const InsideCall&) = delete;
};

// ---------------------------------------------------------------------------
// Crash file + filter
// ---------------------------------------------------------------------------

inline HANDLE crash_file = nullptr;
// Where the crash log ended up (game exe folder, or %TEMP% when that folder
// is not writable): the support bundle carries the file.
inline std::wstring crash_log_path;
inline HMODULE addon_module = nullptr;
inline const char* addon_version = nullptr;
inline uint64_t install_tick64 = 0;
// Entered-filter latch: doubles as the re-entrancy guard (an exception
// raised INSIDE this handler re-enters via a fresh dispatch, fails the CAS
// and falls through to WER) and as the exactly-once guarantee.
inline std::atomic_bool filter_entered{false};
// The process's filter before ours (see the header comment): every crash is
// handed on to it.
inline LPTOP_LEVEL_EXCEPTION_FILTER previous_filter = nullptr;
// Resolved at install, never in a handler: whether ReShade's log export
// exists, so the filter's mirror line never calls through a null export.
inline bool reshade_log_ready = false;
inline PVOID first_chance_handle = nullptr;
inline constexpr uint32_t kFirstChanceLines = 8;
inline std::atomic_uint32_t first_chance_lines{0};
inline std::atomic<DWORD64> first_chance_last_address{0};
// Nonzero while this thread is inside one of the addon's own SEH wraps
// (dlssnr.hpp LoadLibraryWSeh and GatewayLookupSeh, counted as
// native_seh_faults= and logged by their callers; detour_guard.hpp ReadCode,
// whose access violation is how an unmapped module is recognized): the
// faults they catch on purpose are not first-chance records.  One guarded
// probe stays outside it: the statically linked Detours reads a PE header
// under its own __except (detours.cpp detour_is_imported) when it attaches;
// if that ever faults, the record is honest - it is in this image - and it
// counts against the line budget like any other.
inline thread_local uint32_t expected_fault_depth = 0;
// Re-entrancy guard: a fault inside the recorder dispatches again on the
// same thread.
inline thread_local bool in_first_chance = false;

// v8.5.0-rc9: minidumps, opt-in (NRCrashDump=1, dlssnr.hpp).  A dump holds
// the process's memory around every thread's stack, which can include the
// player's data, so the player turns it on for a crash they will send.  The
// lines name where a crash died; a dump lets a debugger open it: every
// thread, registers, the C++ exception object, each module's version.  A
// Debug-configuration build is no substitute - unoptimized code moves the
// timing a crash may depend on (Endfield's throw came 2.5 s into NVIDIA's
// create) and deepens frames past what the small-stack lanes measure.
//  - At last chance (CrashFilter), unless the fault is a stack overflow: the
//    dump needs far more stack than the guard leaves.
//  - At first chance for what the recorder records (the hosts that take the
//    filter slot: Unreal Engine, Special K) and for a C++ throw on the thread
//    inside an InsideCall - the throw at its origin, before any unwind, even
//    when a caller later catches it.
//  - One of each per process: a first-chance dump of the same exception
//    record the filter then receives is not written twice.
// Written on the faulting thread (no helper thread: ReShade unloads add-ons,
// and a thread of ours would outlive the image).  dbghelp is loaded ahead,
// outside DllMain and never in a handler; a crash before that point writes
// its lines and no dump.
inline std::atomic_bool dumps_on{false};
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);
inline std::atomic<MiniDumpWriteDumpFn> write_minidump{nullptr};
inline std::atomic_bool dumped_first_chance{false};
inline std::atomic_bool dumped_crash{false};
inline std::atomic<const EXCEPTION_RECORD*> dumped_record{nullptr};
inline std::atomic_uint32_t dump_serial{0};
// Threads, registers and the memory their stacks point at (the exception
// object included), module and unloaded-module lists, the address-space map
// and handles; no heap image.  Tens of MB for a game.
inline constexpr MINIDUMP_TYPE kDumpType = static_cast<MINIDUMP_TYPE>(
    MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo
    | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData
    | MiniDumpWithFullMemoryInfo | MiniDumpWithHandleData
    | MiniDumpIgnoreInaccessibleMemory);

inline void AppendLine(HANDLE file, const char* text, size_t length) noexcept {
  DWORD written = 0;
  WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
  FlushFileBuffers(file);
}

// v8.5.0-rc4: how a session ended.  Until rc4 a clean exit and a silent
// death - a TerminateProcess, a __fastfail, a hang that was killed - left
// the same file, the [session] line alone (Onimusha and KCD2 v8.5.0-rc1).
// Now the teardown writes each stage it enters ([phase], Phase), the detach
// writes [exit] (Exit), and the next session names a predecessor that wrote
// neither [exit] nor [crash] ([unclean], Install).  The last [phase] says
// whether a death came inside the addon's teardown or after it returned.
// While late_phase is raised on a thread (a teardown in progress on it, the
// process detach) the first-chance recorder takes that thread's faults in
// any image, not only its own scope.  Per thread: a mid-session teardown (a
// swapchain rebuild) must not spend the 8-line budget on another thread's
// handled faults (a managed runtime's null checks, anti-tamper probes).
inline thread_local bool late_phase = false;

// Written without a flush: WriteFile's data is the system's once it
// returns, whatever happens to the process next, and a teardown writes a
// handful of these.
inline void Phase(const char* stage, bool late) noexcept {
  Record(stage);
  late_phase = late;
  if (crash_file == nullptr) return;
  char line[256];
  const int length = snprintf(
      line, sizeof(line), "[phase] %s pid=%lu up_ms=%llu\n", stage, GetCurrentProcessId(),
      static_cast<unsigned long long>(GetTickCount64() - install_tick64));
  DWORD written = 0;
  if (length > 0) {
    WriteFile(crash_file, line, static_cast<DWORD>(std::min<size_t>(length, sizeof(line) - 1)),
              &written, nullptr);
  }
}

// From DLL_PROCESS_DETACH, before Uninstall.
inline void Exit(bool process_exit) noexcept {
  if (crash_file == nullptr) return;
  char line[256];
  const int length = snprintf(
      line, sizeof(line), "[exit] %s pid=%lu up_ms=%llu %s\n",
      addon_version != nullptr ? addon_version : "unknown", GetCurrentProcessId(),
      static_cast<unsigned long long>(GetTickCount64() - install_tick64),
      process_exit ? "process-exit" : "unload");
  if (length > 0) {
    AppendLine(crash_file, line, std::min<size_t>(length, sizeof(line) - 1));
  }
}

// The [unclean] line for the file's previous session, or "": the last
// [session] line in the file's tail whose pid wrote no [exit] or [crash]
// after it and is no longer running.  `last` is that pid's last line.
// Only a session line carrying kEndsLogged is judged: a build before
// v8.5.0-rc4 wrote [session] and never [exit], and its clean exit is no
// predecessor to name.  A pid running again under another image was
// reused, and its session is judged too.
inline constexpr char kEndsLogged[] = " ends=logged ";
inline std::string UncleanPredecessor(const std::wstring& path) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return "";
  // A session's [crash] and [first-chance] records fit many times over.
  std::string tail(65536, '\0');
  LARGE_INTEGER size = {};
  DWORD read = 0;
  if (GetFileSizeEx(file, &size) != FALSE) {
    LARGE_INTEGER from = {};
    from.QuadPart = size.QuadPart > static_cast<LONGLONG>(tail.size())
                        ? size.QuadPart - static_cast<LONGLONG>(tail.size())
                        : 0;
    if (SetFilePointerEx(file, from, nullptr, FILE_BEGIN) == FALSE
        || ReadFile(file, tail.data(), static_cast<DWORD>(tail.size()), &read, nullptr) == FALSE) {
      read = 0;
    }
  }
  CloseHandle(file);
  tail.resize(read);
  const size_t session = tail.rfind("[session] ");
  if (session == std::string::npos || (session != 0 && tail[session - 1] != '\n')) return "";
  const size_t pid_at = tail.find(" pid=", session);
  const size_t line_end = tail.find('\n', session);
  if (pid_at == std::string::npos || line_end == std::string::npos || pid_at > line_end) return "";
  if (const size_t marker = tail.find(kEndsLogged, session);
      marker == std::string::npos || marker > line_end) {
    return "";
  }
  const std::string pid = tail.substr(pid_at + 5, tail.find(' ', pid_at + 5) - pid_at - 5);
  std::string last;
  for (size_t at = line_end + 1; at < tail.size();) {
    const size_t end = std::min(tail.find('\n', at), tail.size());
    const std::string line = tail.substr(at, end - at);
    if (line.find(" pid=" + pid + " ") != std::string::npos) {
      if (line.rfind("[exit] ", 0) == 0 || line.rfind("[crash] ", 0) == 0) return "";
      last = line;
    }
    at = end + 1;
  }
  const DWORD id = static_cast<DWORD>(std::strtoul(pid.c_str(), nullptr, 10));
  if (const HANDLE process =
          OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
      process != nullptr) {
    bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    wchar_t image[MAX_PATH] = L"";
    DWORD image_length = MAX_PATH;
    const size_t exe_at = tail.find(" exe=", session);
    if (running && exe_at < line_end
        && QueryFullProcessImageNameW(process, 0, image, &image_length) != FALSE) {
      // exe= is the image's file name, written narrow: an ASCII
      // case-insensitive compare, where a character it cannot compare
      // matches (no [unclean] for a name it cannot judge).
      const std::string exe = tail.substr(exe_at + 5, line_end - exe_at - 5);
      const wchar_t* name = wcsrchr(image, L'\\');
      name = name != nullptr ? name + 1 : image;
      running = wcslen(name) == exe.size();
      for (size_t i = 0; running && i < exe.size(); ++i) {
        running = name[i] >= 0x80 || static_cast<unsigned char>(exe[i]) >= 0x80
                  || towlower(name[i]) == towlower(static_cast<unsigned char>(exe[i]));
      }
    }
    CloseHandle(process);
    if (running) return "";  // a concurrent session, not a dead one
  }
  return "[unclean] prev_pid=" + pid + " last="
         + (last.empty() ? tail.substr(session, line_end - session) : last).substr(0, 300) + "\n";
}

// The image a fault address lies in.  FROM_ADDRESS with UNCHANGED_REFCOUNT
// takes no reference, and neither call allocates.  A first-chance handler
// can run while ANOTHER thread holds the loader lock; measured 2026-09-24 on
// Windows 11 26100 (a thread parked in LdrLockLoaderLock, probe threads
// initialized beforehand): GetModuleHandleExW(FROM_ADDRESS) and
// GetModuleFileNameW both return, as do RtlPcToFileHeader and
// GetMappedFileNameW.  Stack only.
struct FaultImage {
  DWORD64 base = 0;
  wchar_t path[MAX_PATH] = L"";
  const wchar_t* name = path;
};

inline void ResolveFaultImage(DWORD64 address, FaultImage* image) noexcept {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(address), &module)
      || module == nullptr) {
    return;
  }
  image->base = reinterpret_cast<DWORD64>(module);
  if (GetModuleFileNameW(module, image->path, MAX_PATH) == 0) image->path[0] = L'\0';
  for (const wchar_t* c = image->path; *c != L'\0'; ++c) {
    if (*c == L'\\' || *c == L'/') image->name = c + 1;
  }
}

// The part of an image path below an \NVIDIA\NGX\ folder, or null.  The
// driver keeps its model and override copies there under hashed names -
// ...\NGX\models\sl_common_override_0\versions\134656\files\1B0_E658700.dll
// is Streamline's sl.common (Resident Evil Requiem field log) - so there the
// folder, not the basename, says what the image is.
inline const wchar_t* NgxFolderTail(const wchar_t* path) noexcept {
  constexpr wchar_t kFolder[] = L"\\nvidia\\ngx\\";
  for (const wchar_t* start = path; *start != L'\0'; ++start) {
    size_t i = 0;
    for (; kFolder[i] != L'\0'; ++i) {
      const wchar_t c = start[i] >= L'A' && start[i] <= L'Z'
                            ? static_cast<wchar_t>(start[i] + (L'a' - L'A'))
                            : start[i];
      if (c != kFolder[i]) break;  // also stops at the terminator
    }
    if (kFolder[i] == L'\0') return start + i;
  }
  return nullptr;
}

// A resolved image's name for a crash line: its basename, or its path below
// \NVIDIA\NGX\ for the driver's hashed copies.  Narrowed by hand (non-ASCII
// becomes '?'): %ls would convert through the CRT locale.  Returns the
// length; 0 for an image without a path.
inline size_t ImageLineName(const FaultImage& image, char (&name)[MAX_PATH + 4]) noexcept {
  const wchar_t* const ngx_tail = NgxFolderTail(image.path);
  size_t length = 0;
  if (ngx_tail != nullptr) {
    memcpy(name, "NGX\\", 4);
    length = 4;
  }
  for (const wchar_t* c = ngx_tail != nullptr ? ngx_tail : image.name;
       *c != L'\0' && length + 1 < sizeof(name); ++c) {
    name[length++] = *c < 0x80 ? static_cast<char>(*c) : '?';
  }
  name[length] = '\0';
  return length;
}

// v8.5.0-rc9: what an MSVC C++ throw threw, from its exception record: the
// thrown type (" type=std::runtime_error"; the decorated name when it holds
// templates) and, for a std::exception, " what=\"...\"".  The Endfield rc4/rc5
// crash lines said c++-throw and nothing about what was thrown.  x64 layout
// (ehdata.h): ExceptionInformation[1] the object, [2] its ThrowInfo, [3] the
// throwing image's base; ThrowInfo's 4th int is the RVA of the catchable-type
// array (a count, then RVAs); a CatchableType's 2nd int is the RVA of its
// TypeDescriptor, whose name follows two pointers.  A std::exception keeps
// its message pointer right after the vtable.  Every read is guarded: a
// foreign runtime's record, or one whose image is gone, ends the text with
// " (decode faulted)".  Its own function: a __try frame cannot unwind C++
// objects.
inline size_t DescribeCxxThrow(const EXCEPTION_RECORD& record, char (&out)[320]) noexcept {
  volatile size_t used = 0;
  out[0] = '\0';
#if defined(_M_X64)
  const auto magic = record.ExceptionInformation[0];
  if (record.ExceptionCode != 0xE06D7363 || record.NumberParameters < 4
      || magic < 0x19930520 || magic > 0x19930522 || record.ExceptionInformation[3] == 0) {
    return 0;
  }
  const auto put = [&](char c) {
    if (used + 1 < sizeof(out)) out[used++] = c;
  };
  ++expected_fault_depth;
  __try {
    const uintptr_t base = static_cast<uintptr_t>(record.ExceptionInformation[3]);
    const auto* throw_info = reinterpret_cast<const int32_t*>(record.ExceptionInformation[2]);
    const auto* types = reinterpret_cast<const int32_t*>(base + throw_info[3]);
    bool std_exception = false;
    for (int32_t i = 0; i < types[0] && i < 16; ++i) {
      const auto* catchable = reinterpret_cast<const int32_t*>(base + types[1 + i]);
      const char* name = reinterpret_cast<const char*>(base + catchable[1] + 16);
      std_exception |= strcmp(name, ".?AVexception@std@@") == 0;
      if (i != 0) continue;
      for (const char* c = " type="; *c != '\0'; ++c) put(*c);
      // ".?AVname@ns@@" -> "ns::name"; anything else stays decorated.
      const char* body = name + 4;
      const bool plain = strncmp(name, ".?AV", 4) == 0 || strncmp(name, ".?AU", 4) == 0;
      const char* end = plain ? strstr(body, "@@") : nullptr;
      if (end == nullptr || strpbrk(body, "?$") != nullptr) {
        for (const char* c = name; *c != '\0' && c - name < 120; ++c) put(*c);
        continue;
      }
      for (const char* part_end = end; part_end > body;) {
        const char* part = part_end;
        while (part > body && part[-1] != '@') --part;
        for (const char* c = part; c < part_end; ++c) put(*c);
        part_end = part > body ? part - 1 : body;
        if (part_end > body) {
          put(':');
          put(':');
        }
      }
    }
    if (std_exception) {
      const char* what = *reinterpret_cast<const char* const*>(record.ExceptionInformation[1] + 8);
      if (what != nullptr) {
        for (const char* c = " what=\""; *c != '\0'; ++c) put(*c);
        for (int i = 0; what[i] != '\0' && i < 120; ++i) {
          const char c = what[i];
          put(c == '"' ? '\'' : c >= 0x20 && c < 0x7f ? c : '?');
        }
        put('"');
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    for (const char* c = " (decode faulted)"; *c != '\0'; ++c) put(*c);
  }
  --expected_fault_depth;
  out[used] = '\0';
#endif
  return used;
}

// One fault line, shared by the last-chance filter ("[crash]") and the
// first-chance recorder ("[first-chance]"): version, pid/tid, uptime, code
// and name, the access-violation detail, image+offset, whether the image is
// this addon, and the release marks.  The 2048-byte buffer covers the
// longest image name, the mark ring and the access-violation detail; on a
// stack overflow the remaining guard-released stack may be smaller than the
// caller's frame, the fault then re-enters a guarded handler and the process
// dies through WER - degraded, not swallowed.  Returns the length, newline
// included.
inline size_t FormatFaultLine(char (&line)[2048], const char* tag,
                              const EXCEPTION_RECORD& record,
                              const FaultImage& image) noexcept {
  const DWORD code = record.ExceptionCode;
  size_t used = 0;
  const auto append = [&](const char* text, size_t length) {
    if (length >= sizeof(line) - used) length = sizeof(line) - used - 1;
    memcpy(line + used, text, length);
    used += length;
  };
  const auto appendf = [&](const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    const int length = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (length > 0) append(buffer, static_cast<size_t>(length));
  };

  appendf(
      "%s %s pid=%lu tid=%lu up_ms=%llu code=0x%08lX", tag,
      addon_version != nullptr ? addon_version : "unknown",
      GetCurrentProcessId(), GetCurrentThreadId(),
      static_cast<unsigned long long>(GetTickCount64() - install_tick64),
      static_cast<unsigned long>(code));
  const char* code_name = nullptr;
  switch (code) {
    case 0xC0000005: code_name = "access-violation"; break;
    case 0xC0000409: code_name = "fastfail/stack-buffer-overrun"; break;
    case 0xC000001D: code_name = "illegal-instruction"; break;
    case 0xC00000FD: code_name = "stack-overflow"; break;
    case 0xC0000374: code_name = "heap-corruption"; break;
    case 0xE06D7363: code_name = "c++-throw"; break;
    default: break;
  }
  if (code_name != nullptr) appendf(" (%s)", code_name);
  if (code == 0xE06D7363) {
    char thrown[320];
    append(thrown, DescribeCxxThrow(record, thrown));
  }
  if (code == 0xC0000005 && record.NumberParameters >= 2) {
    appendf(
        " %s at 0x%llx",
        record.ExceptionInformation[0] == 0 ? "READ"
            : record.ExceptionInformation[0] == 1 ? "WRITE"
            : record.ExceptionInformation[0] == 8 ? "DEP" : "???",
        static_cast<unsigned long long>(record.ExceptionInformation[1]));
  }

  const DWORD64 address = reinterpret_cast<DWORD64>(record.ExceptionAddress);
  if (image.base != 0) {
    char name[MAX_PATH + 4];
    appendf(
        " addr=0x%llx at=%s+0x%llx addon-module=%s",
        static_cast<unsigned long long>(address),
        ImageLineName(image, name) != 0 ? name : "<unnamed>",
        static_cast<unsigned long long>(address - image.base),
        image.base == reinterpret_cast<DWORD64>(addon_module) ? "yes" : "no");
  } else {
    appendf(
        " addr=0x%llx at=<no-module> addon-module=no",
        static_cast<unsigned long long>(address));
  }
  if (const char* call = inside_call.load(std::memory_order_acquire); call != nullptr) {
    appendf(" inside=%s tid=%lu", call,
            static_cast<unsigned long>(inside_call_tid.load(std::memory_order_relaxed)));
  }

  // The mark ring, oldest-to-newest.  A writer between its ticket fetch and
  // its slot store can hand a null (skipped) or stale slot here; both are
  // harmless for triage.
  append(" marks=", 7);
  const uint32_t count = mark_count.load(std::memory_order_relaxed);
  const uint32_t first = count > kMarkSlots ? count - kMarkSlots : 0;
  for (uint32_t ticket = first; ticket < count; ++ticket) {
    const char* stage =
        marks[ticket & (kMarkSlots - 1)].load(std::memory_order_acquire);
    if (stage == nullptr) continue;
    const size_t length = strlen(stage);
    if (sizeof(line) - used < length + 3) break;
    if (ticket != first) append(",", 1);
    append(stage, length);
  }
  append("\n", 1);
  line[used] = '\0';
  return used;
}

// v8.5.0-rc7: the "[stack]" line under a "[crash]" line - the dying
// thread's frames, innermost first, each image+offset ('*' marks this
// add-on) or a bare address outside any image.  The [crash] line names only
// the image a fault is in: the NBA2K27 rc5 crash was a read inside the
// game's executable on its main thread, and whether a hook (this add-on's,
// a frame-generation unlocker's, ReShade's) was below it could not be
// told.  x64 unwind data only, nothing allocates: RtlLookupFunctionEntry and
// RtlVirtualUnwind, a frame without unwind data taken as a leaf (its return
// address at [Rsp]); a C++ throw's frames start in RaiseException and name
// the thrower below it.  The walk stops at kStackFrames, a zero return
// address or an Rsp outside the thread's stack.  A fault inside it ends the
// line with "(walk faulted)", inside expected_fault_depth so the
// first-chance recorder skips it.  Its own function: a __try frame cannot
// unwind C++ objects.
inline constexpr int kStackFrames = 16;
inline size_t FormatStackLine(char (&line)[2048], const CONTEXT& fault) noexcept {
  volatile size_t used = 0;
  const auto appendf = [&](const char* fmt, auto... args) {
    const int length = snprintf(line + used, sizeof(line) - used, fmt, args...);
    if (length > 0) used += std::min<size_t>(length, sizeof(line) - 2 - used);
  };
  appendf("[stack] %s pid=%lu tid=%lu", addon_version != nullptr ? addon_version : "unknown",
          GetCurrentProcessId(), GetCurrentThreadId());
#if defined(_M_X64)
  CONTEXT context = fault;
  const NT_TIB* const tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
  const DWORD64 stack_low = reinterpret_cast<DWORD64>(tib->StackLimit);
  const DWORD64 stack_high = reinterpret_cast<DWORD64>(tib->StackBase);
  ++expected_fault_depth;
  __try {
    for (int frame = 0; frame < kStackFrames && context.Rip != 0; ++frame) {
      FaultImage image;
      ResolveFaultImage(context.Rip, &image);
      char name[MAX_PATH + 4];
      if (image.base != 0 && ImageLineName(image, name) != 0) {
        appendf(" %s+0x%llx%s", name, static_cast<unsigned long long>(context.Rip - image.base),
                image.base == reinterpret_cast<DWORD64>(addon_module) ? "*" : "");
      } else {
        appendf(" 0x%llx", static_cast<unsigned long long>(context.Rip));
      }
      DWORD64 image_base = 0;
      const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
      if (function == nullptr) {
        if (context.Rsp < stack_low || context.Rsp + 8 > stack_high) break;
        context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
        context.Rsp += 8;
      } else {
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                         &handler_data, &establisher, nullptr);
      }
      if (context.Rsp < stack_low || context.Rsp >= stack_high) break;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    appendf(" (walk faulted)");
  }
  --expected_fault_depth;
#endif
  line[used] = '\n';
  line[used + 1] = '\0';
  return used + 1;
}

// Loads the system dbghelp and resolves MiniDumpWriteDump (the dumps_on
// comment): outside DllMain, never in a handler.  By full path - a game's
// own older dbghelp.dll may already be loaded under the same name.  True the
// first time it resolves.
inline bool PrepareDumps() noexcept {
  if (write_minidump.load(std::memory_order_acquire) != nullptr) return false;
  wchar_t path[MAX_PATH] = L"";
  const UINT length = GetSystemDirectoryW(path, MAX_PATH - 16);
  if (length == 0 || length >= MAX_PATH - 16) return false;
  wcscat_s(path, L"\\dbghelp.dll");
  const HMODULE dbghelp = LoadLibraryExW(path, nullptr, 0);
  if (dbghelp == nullptr) return false;
  const auto write =
      reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
  write_minidump.store(write, std::memory_order_release);
  return write != nullptr;
}

// One minidump beside the crash log, RenoDX-DLSS5-crash-<pid>-<n>.dmp, and a
// "[dump]" line naming it and why it was taken.  The faulting thread's own
// record goes in as the dump's exception stream, so a debugger's .ecxr lands
// on the fault, not in this function.  Faults dbghelp takes while it reads
// the process are its own: the recorder skips them.
inline void WriteDump(EXCEPTION_POINTERS* info, const char* reason) noexcept {
  const MiniDumpWriteDumpFn write = write_minidump.load(std::memory_order_acquire);
  if (write == nullptr || crash_file == nullptr || crash_log_path.size() < 4) return;
  wchar_t path[MAX_PATH + 48];
  swprintf(path, MAX_PATH + 48, L"%.*ls-%lu-%u.dmp",
           static_cast<int>(crash_log_path.size() - 4), crash_log_path.c_str(),
           GetCurrentProcessId(), dump_serial.fetch_add(1, std::memory_order_relaxed) + 1);
  const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  BOOL written = FALSE;
  DWORD error = GetLastError();
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION exception = {GetCurrentThreadId(), info, FALSE};
    const bool was_in_first_chance = in_first_chance;
    in_first_chance = true;
    written = write(GetCurrentProcess(), GetCurrentProcessId(), file, kDumpType, &exception,
                    nullptr, nullptr);
    error = GetLastError();
    in_first_chance = was_in_first_chance;
    CloseHandle(file);
  }
  // UTF-8 by hand: snprintf's %ls stops at the first character outside the C
  // locale, and player folder names are not ASCII.
  char utf8[3 * (MAX_PATH + 48)] = "";
  WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
  char line[sizeof(utf8) + 160];
  const int length =
      written != FALSE
          ? snprintf(line, sizeof(line), "[dump] %s pid=%lu tid=%lu reason=%s file=%s\n",
                     addon_version != nullptr ? addon_version : "unknown", GetCurrentProcessId(),
                     GetCurrentThreadId(), reason, utf8)
          : snprintf(line, sizeof(line),
                     "[dump] %s pid=%lu tid=%lu reason=%s failed=0x%08lx file=%s\n",
                     addon_version != nullptr ? addon_version : "unknown", GetCurrentProcessId(),
                     GetCurrentThreadId(), reason, static_cast<unsigned long>(error), utf8);
  if (length > 0) AppendLine(crash_file, line, std::min<size_t>(length, sizeof(line) - 1));
}

inline LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) noexcept {
  bool expected = false;
  if (!filter_entered.compare_exchange_strong(expected, true)) {
    // Re-entered: a fault inside this handler, or a previous filter that
    // calls back into ours.  Never chain from here - that could loop.
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (crash_file != nullptr && info != nullptr && info->ExceptionRecord != nullptr) {
    FaultImage image;
    ResolveFaultImage(reinterpret_cast<DWORD64>(info->ExceptionRecord->ExceptionAddress),
                      &image);
    char line[2048];
    AppendLine(crash_file, line,
               FormatFaultLine(line, "[crash]", *info->ExceptionRecord, image));
    // Not on a stack overflow: the filter runs on the pages the guard left.
    char stack[2048] = "";
    if (info->ContextRecord != nullptr
        && info->ExceptionRecord->ExceptionCode != 0xC00000FD) {
      AppendLine(crash_file, stack, FormatStackLine(stack, *info->ContextRecord));
    }
    // Best-effort mirror into the log the user already sends.  May fail or
    // deadlock during a crash; the file lines above are the guarantee.
    if (reshade_log_ready) {
      reshade::log::message(reshade::log::level::error, line);
      if (stack[0] != '\0') reshade::log::message(reshade::log::level::error, stack);
    }
    // After the lines: they are the guarantee, the dump the bonus.
    if (dumps_on.load(std::memory_order_relaxed)
        && info->ExceptionRecord->ExceptionCode != 0xC00000FD
        && dumped_record.load(std::memory_order_relaxed) != info->ExceptionRecord
        && !dumped_crash.exchange(true, std::memory_order_relaxed)) {
      WriteDump(info, "crash");
    }
  }
  // The host's crash handling runs exactly as it would have without this
  // reporter: its filter writes its dump, shows its dialog, or continues
  // the search to WER.
  return previous_filter != nullptr ? previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

// The first-chance recorder (header comment, v8.0.2).  Always returns
// EXCEPTION_CONTINUE_SEARCH.  Stack overflow is not recorded: a first-chance
// handler runs on the pages the guard left, and formatting there re-faults.
inline LONG CALLBACK FirstChanceHandler(EXCEPTION_POINTERS* info) noexcept {
  if (crash_file == nullptr || info == nullptr || info->ExceptionRecord == nullptr) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const EXCEPTION_RECORD& record = *info->ExceptionRecord;
  const DWORD code = record.ExceptionCode;
  // Access violations and illegal instructions are recorded by scope below.
  // Heap corruption and a fail-fast that reaches dispatch are recorded
  // wherever they are raised: they are never benign, and heap corruption is
  // raised inside ntdll whoever corrupted the heap.
  const bool scoped = code == 0xC0000005 || code == 0xC000001D;
  // v8.5.0-rc9: a C++ throw on the thread inside an InsideCall (NVIDIA's NR
  // create), recorded where it is raised, before any caller decides whether
  // to catch it: the Endfield rc4/rc5 throw was never seen unwinding.  A
  // throw anywhere else is the game's own C++ EH and stays unrecorded.
  const bool throw_in_call = code == 0xE06D7363
                             && inside_call.load(std::memory_order_acquire) != nullptr
                             && inside_call_tid.load(std::memory_order_relaxed)
                                    == GetCurrentThreadId();
  if (!scoped && !throw_in_call && code != 0xC0000374 && code != 0xC0000409) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if ((scoped && expected_fault_depth != 0) || in_first_chance
      || first_chance_lines.load(std::memory_order_relaxed) >= kFirstChanceLines) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const DWORD64 address = reinterpret_cast<DWORD64>(record.ExceptionAddress);
  if (first_chance_last_address.load(std::memory_order_relaxed) == address) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  in_first_chance = true;
  FaultImage image;
  ResolveFaultImage(address, &image);
  bool record_it = !scoped;
  if (scoped) {
    const auto name_starts = [&](const wchar_t* prefix) {
      for (size_t i = 0; prefix[i] != L'\0'; ++i) {
        const wchar_t c = image.name[i] >= L'A' && image.name[i] <= L'Z'
                              ? static_cast<wchar_t>(image.name[i] + (L'a' - L'A'))
                              : image.name[i];
        if (c != prefix[i]) return false;  // also stops at the terminator
      }
      return true;
    };
    record_it =
        late_phase
        || (image.base == 0
                // No image: only an execute of that very address - a call into
                // a freed trampoline or an unloaded module - not JIT or packed
                // code faulting on data.
                ? code == 0xC0000005 && record.NumberParameters >= 2
                      && record.ExceptionInformation[0] == 8
                : image.base == reinterpret_cast<DWORD64>(addon_module)
                      || name_starts(L"nvngx") || name_starts(L"_nvngx")
                      || name_starts(L"sl.") || NgxFolderTail(image.path) != nullptr);
  }
  if (record_it
      && first_chance_lines.fetch_add(1, std::memory_order_relaxed) < kFirstChanceLines) {
    first_chance_last_address.store(address, std::memory_order_relaxed);
    char line[2048];
    AppendLine(crash_file, line, FormatFaultLine(line, "[first-chance]", record, image));
    if (dumps_on.load(std::memory_order_relaxed)
        && !dumped_first_chance.exchange(true, std::memory_order_relaxed)) {
      dumped_record.store(&record, std::memory_order_relaxed);
      WriteDump(info, throw_in_call ? "throw-inside-call" : "first-chance");
    }
  }
  in_first_chance = false;
  return EXCEPTION_CONTINUE_SEARCH;
}

// From DLL_PROCESS_ATTACH (the CRT and the static initializers are up by
// then, the loader lock does not block CreateFileW or the filter install).
// Idempotent; the session header line makes the append-only file
// self-describing across sessions.
inline void Install(HMODULE module, const char* version) {
  if (crash_file != nullptr) return;
  addon_module = module;
  addon_version = version;
  install_tick64 = GetTickCount64();

  wchar_t exe[MAX_PATH] = L"";
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const wchar_t* exe_name = exe;
  for (const wchar_t* c = exe; *c != L'\0'; ++c) {
    if (*c == L'\\' || *c == L'/') exe_name = c + 1;
  }
  std::wstring dir(exe);
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) dir.resize(slash);
  crash_log_path = (slash != std::wstring::npos ? dir + L"\\" : L"")
                   + std::wstring(L"RenoDX-DLSS5-crash.log");
  crash_file = CreateFileW(
      crash_log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (crash_file == INVALID_HANDLE_VALUE) {
    // The game folder is not writable (rare): same %TEMP% fallback the debug
    // trace uses, per-process so concurrent sessions do not interleave.
    wchar_t temp[MAX_PATH] = L"";
    if (GetTempPathW(MAX_PATH, temp) != 0) {
      crash_log_path = std::wstring(temp) + L"RenoDX-DLSS5-crash-"
                       + std::to_wstring(GetCurrentProcessId()) + L".log";
      crash_file = CreateFileW(
          crash_log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
  }
  if (crash_file == INVALID_HANDLE_VALUE) {
    crash_file = nullptr;
    crash_log_path.clear();
    return;
  }

  const std::string unclean = UncleanPredecessor(crash_log_path);
  if (!unclean.empty()) AppendLine(crash_file, unclean.data(), unclean.size());
  char header[512];
  const int length = snprintf(
      header, sizeof(header), "[session] %s pid=%lu%sexe=%ls\n",
      version != nullptr ? version : "unknown", GetCurrentProcessId(), kEndsLogged, exe_name);
  if (length > 0) {
    AppendLine(crash_file, header, static_cast<size_t>(length));
  }
  if (const HMODULE reshade_module = reshade::internal::get_reshade_module_handle();
      reshade_module != nullptr) {
    reshade_log_ready = GetProcAddress(reshade_module, "ReShadeLogMessage") != nullptr;
  }
  previous_filter = SetUnhandledExceptionFilter(CrashFilter);
  // First in the vectored list: it only records, so running first changes
  // no other handler's outcome, and it sees the fault even when a later
  // handler ends the process from inside its own callback.
  first_chance_handle = AddVectoredExceptionHandler(1, FirstChanceHandler);
}

// From DLL_PROCESS_DETACH on an UNLOAD (FreeLibrary - ReShade unloads the
// add-ons it loaded when the last device goes away), never at process exit.
// Both handlers would otherwise point into unmapped code, and the vectored
// one is entered for EVERY exception in the process, a C++ throw included -
// an unloaded recorder would turn the next one into a crash.  The filter
// slot goes back to the previous owner only while we still hold it; a
// filter installed on top of ours keeps its slot.
inline void Uninstall() noexcept {
  if (crash_file == nullptr) return;
  if (first_chance_handle != nullptr) {
    RemoveVectoredExceptionHandler(first_chance_handle);
    first_chance_handle = nullptr;
  }
  const LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(previous_filter);
  if (current != CrashFilter) SetUnhandledExceptionFilter(current);
  CloseHandle(crash_file);
  crash_file = nullptr;
}

}  // namespace lastgasp
}  // namespace renodx::addons::dlss5
