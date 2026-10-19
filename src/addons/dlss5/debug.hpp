/*
 * SPDX-License-Identifier: MIT
 *
 * Crash + freeze diagnostics for the DLSS5 generic addon.  Everything here is
 * compiled ONLY when RENODX_DLSS5_DEBUG is defined (the debug variant build);
 * without it every entry point folds away to an empty inline stub, so call
 * sites in dlssnr.hpp need no preprocessor guards.
 *
 * GATING RULE: RENODX_DLSS5_DEBUG may only gate code whose ONLY outputs are
 * logs, trace files, or crash dumps.  Anything that writes a field the image
 * path reads (FinalResources norm_ and nr_ authority, evidence, health,
 * metered_, slewed_divisor, est_ fields, or any GPU pass feeding them) must
 * behave identically in both builds - the debug build is Release plus
 * diagnostics, never a different regulator.  codec_gain.hpp (the unit-tested
 * control kernel) must stay macro-free.
 *
 * Facilities:
 *  - RenoDX-DLSS5-debug.log next to the game exe: append-only, flush-per-line
 *    trace of addon lifecycle stages plus a 5 s watchdog heartbeat with
 *    present/NGX counters.  A freeze shows up as STALLED heartbeats naming the
 *    last stage; the final line before silence localizes the wedge.
 *  - Minidump + symbolized text stack on any unhandled exception:
 *    RenoDX-DLSS5-crash-<pid>-<tick>.dmp/.txt next to the game exe.
 *  - The log falls back to %TEMP% when the game directory is not writable.
 */
#pragma once

#if defined(RENODX_DLSS5_DEBUG)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dbghelp.h>
#include <string>
#include <thread>

#pragma comment(lib, "dbghelp.lib")

namespace renodx::addons::dlss5 {
namespace debug {

inline std::atomic<uint64_t> presents{0};
inline std::atomic<uint64_t> ngx_creates{0};
inline std::atomic<uint64_t> ngx_evals{0};
inline std::atomic<const char*> last_stage{nullptr};
inline std::atomic<bool> stop_watchdog{false};
inline std::atomic<bool> filter_entered{false};
inline HANDLE trace_file = nullptr;
inline CRITICAL_SECTION trace_lock;
inline bool trace_lock_ready = false;
inline LARGE_INTEGER start_qpc = {};
inline double qpc_inv_ms = 0.0;
inline std::wstring trace_path;
inline std::wstring dump_dir;
inline std::thread watchdog;
inline bool symbols_ready = false;

inline int64_t ElapsedMs() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return static_cast<int64_t>(
      static_cast<double>(now.QuadPart - start_qpc.QuadPart) * qpc_inv_ms);
}

// Best-effort append: takes the lock only when free, so a thread crashing
// while holding it (or detach under the loader lock) cannot deadlock the
// tracer; contended lines are dropped instead of written.
inline void Tracef(const char* fmt, ...) {
  HANDLE file = trace_file;
  if (file == nullptr || !trace_lock_ready) return;
  if (!TryEnterCriticalSection(&trace_lock)) return;

  char line[1280];
  const int64_t ms = ElapsedMs();
  int header_len = snprintf(
      line, sizeof(line), "[+%.3fs][tid %lu] ",
      static_cast<double>(ms) / 1000.0, GetCurrentThreadId());
  int used = header_len > 0 ? header_len : 0;
  if (used < static_cast<int>(sizeof(line))) {
    va_list args;
    va_start(args, fmt);
    int body_len = vsnprintf(line + used, sizeof(line) - static_cast<size_t>(used), fmt, args);
    va_end(args);
    if (body_len > 0) used += body_len;
  }
  if (used >= static_cast<int>(sizeof(line))) used = sizeof(line) - 1;
  if (used < static_cast<int>(sizeof(line)) - 1) line[used++] = '\n';
  DWORD written = 0;
  WriteFile(file, line, static_cast<DWORD>(used), &written, nullptr);
  FlushFileBuffers(file);
  LeaveCriticalSection(&trace_lock);
}

inline void Mark(const char* stage) {
  last_stage.store(stage, std::memory_order_relaxed);
  Tracef("stage %s", stage);
}

inline void TouchPresent() {
  presents.fetch_add(1, std::memory_order_relaxed);
}
inline void TouchNgxCreate() {
  ngx_creates.fetch_add(1, std::memory_order_relaxed);
}
inline void TouchNgxEval() {
  ngx_evals.fetch_add(1, std::memory_order_relaxed);
}

inline void AppendStringToFile(HANDLE file, const char* text, size_t length) {
  DWORD written = 0;
  WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
}

inline void DescribeAddress(DWORD64 address, char* out, size_t out_size) {
  HMODULE module = nullptr;
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(address), &module) &&
      module != nullptr) {
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = path;
    for (const wchar_t* c = path; *c != L'\0'; ++c) {
      if (*c == L'\\' || *c == L'/') name = c + 1;
    }
    const DWORD64 base = reinterpret_cast<DWORD64>(module);
    snprintf(out, out_size, "%ls!+0x%llx", name,
             static_cast<unsigned long long>(address - base));
    return;
  }
  snprintf(out, out_size, "0x%llx", static_cast<unsigned long long>(address));
}

inline void WriteCrashReport(
    EXCEPTION_POINTERS* info, const wchar_t* report_path) {
  HANDLE file = CreateFileW(
      report_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;

  char text[16384];
  size_t used = 0;
  auto append = [&](const char* part) {
    const size_t len = strlen(part);
    if (used + len < sizeof(text)) {
      memcpy(text + used, part, len);
      used += len;
    }
  };
  auto appendf = [&](const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (len > 0) append(buffer);
  };

  const EXCEPTION_RECORD* record =
      info != nullptr && info->ExceptionRecord != nullptr ? info->ExceptionRecord : nullptr;
  appendf(
      "RenoDX DLSS5 debug build crash report\npid=%lu tid=%lu up_ms=%lld\n",
      GetCurrentProcessId(), GetCurrentThreadId(),
      static_cast<long long>(ElapsedMs()));
  appendf(
      "exception code=0x%08lx address=%p flags=0x%lx\n",
      record != nullptr ? record->ExceptionCode : 0,
      record != nullptr ? record->ExceptionAddress : nullptr,
      record != nullptr ? record->ExceptionFlags : 0);
  if (record != nullptr && record->NumberParameters > 0) {
    append("exception parameters:");
    for (DWORD i = 0; i < record->NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i) {
      appendf("  [%lu] 0x%llx", i,
              static_cast<unsigned long long>(record->ExceptionInformation[i]));
    }
    append("\n");
  }

  char where[512];
  DescribeAddress(reinterpret_cast<DWORD64>(record != nullptr ? record->ExceptionAddress : 0), where, sizeof(where));
  appendf("faulting site: %s\n\n", where);

  append("stack (innermost first):\n");
  void* frames[62] = {};
  const WORD frame_count = CaptureStackBackTrace(0, 62, frames, nullptr);
  for (WORD i = 0; i < frame_count; ++i) {
    char where_frame[512];
    DescribeAddress(reinterpret_cast<DWORD64>(frames[i]), where_frame, sizeof(where_frame));
    char symbol_part[512] = "";
    alignas(SYMBOL_INFO) char symbol_buffer[sizeof(SYMBOL_INFO) + 256] = {};
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(frames[i]), &displacement, symbol) != FALSE) {
      IMAGEHLP_LINEW64 line_info = {};
      line_info.SizeOfStruct = sizeof(IMAGEHLP_LINEW64);
      DWORD line_displacement = 0;
      if (SymGetLineFromAddrW64(GetCurrentProcess(), reinterpret_cast<DWORD64>(frames[i]), &line_displacement, &line_info) != FALSE) {
        snprintf(symbol_part, sizeof(symbol_part), " %s (%ls:%lu)", symbol->Name, line_info.FileName, line_info.LineNumber);
      } else {
        snprintf(symbol_part, sizeof(symbol_part), " %s+0x%llx", symbol->Name,
                 static_cast<unsigned long long>(displacement));
      }
    }
    appendf("  %02u %p %s%s\n", i, frames[i], where_frame, symbol_part);
  }

  AppendStringToFile(file, text, used);
  FlushFileBuffers(file);
  CloseHandle(file);
  // Best-effort mirror into the trace log (lock-free path: the crashing
  // thread may already hold the tracer lock or the heap may be dead).
  if (trace_file != nullptr) {
    AppendStringToFile(trace_file, text, used);
    FlushFileBuffers(trace_file);
  }
}

inline LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
  bool expected = false;
  if (!filter_entered.compare_exchange_strong(expected, true)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  wchar_t report_base[MAX_PATH] = L"";
  if (!dump_dir.empty()) {
    swprintf(
        report_base, MAX_PATH, L"%ls\\RenoDX-DLSS5-crash-%lu-%lld",
        dump_dir.c_str(), GetCurrentProcessId(),
        static_cast<long long>(GetTickCount64()));
  }
  if (report_base[0] != L'\0') {
    std::wstring txt = std::wstring(report_base) + L".txt";
    WriteCrashReport(info, txt.c_str());

    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(
        HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
        PMINIDUMP_EXCEPTION_INFORMATION,
        PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbghelp = GetModuleHandleW(L"dbghelp.dll");
    if (dbghelp == nullptr) dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (dbghelp != nullptr) {
      const auto write_dump =
          reinterpret_cast<MiniDumpWriteDumpFn>(
              GetProcAddress(dbghelp, "MiniDumpWriteDump"));
      if (write_dump != nullptr) {
        std::wstring dmp = std::wstring(report_base) + L".dmp";
        HANDLE dump_file = CreateFileW(
            dmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dump_file != INVALID_HANDLE_VALUE) {
          MINIDUMP_EXCEPTION_INFORMATION dump_info = {
              GetCurrentThreadId(), info, FALSE};
          write_dump(
              GetCurrentProcess(), GetCurrentProcessId(), dump_file,
              static_cast<MINIDUMP_TYPE>(
                  MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory |
                  MiniDumpWithProcessThreadData),
              &dump_info, nullptr, nullptr);
          CloseHandle(dump_file);
          Tracef("minidump written: %ls", dmp.c_str());
        }
      }
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

inline void WatchdogLoop() {
  uint64_t prev_presents = 0;
  uint64_t prev_evals = 0;
  int stalled_beats = 0;
  while (!stop_watchdog.load(std::memory_order_relaxed)) {
    for (int i = 0; i < 50 && !stop_watchdog.load(std::memory_order_relaxed); ++i) {
      Sleep(100);
    }
    if (stop_watchdog.load(std::memory_order_relaxed)) break;
    const uint64_t now_presents = presents.load(std::memory_order_relaxed);
    const uint64_t now_evals = ngx_evals.load(std::memory_order_relaxed);
    const char* stage = last_stage.load(std::memory_order_relaxed);
    if (now_presents == prev_presents) {
      ++stalled_beats;
      Tracef(
          "heartbeat up=%.0fs presents=%llu (STALLED %ds) ngx_evals=%llu "
          "creates=%llu stage=%s",
          ElapsedMs() / 1000.0,
          static_cast<unsigned long long>(now_presents),
          stalled_beats * 5,
          static_cast<unsigned long long>(now_evals),
          static_cast<unsigned long long>(ngx_creates.load()),
          stage != nullptr ? stage : "-");
    } else {
      stalled_beats = 0;
      Tracef(
          "heartbeat up=%.0fs presents=%llu (+%llu) ngx_evals=%llu (+%llu) "
          "creates=%llu stage=%s",
          ElapsedMs() / 1000.0,
          static_cast<unsigned long long>(now_presents),
          static_cast<unsigned long long>(now_presents - prev_presents),
          static_cast<unsigned long long>(now_evals),
          static_cast<unsigned long long>(now_evals - prev_evals),
          static_cast<unsigned long long>(ngx_creates.load()),
          stage != nullptr ? stage : "-");
    }
    prev_presents = now_presents;
    prev_evals = now_evals;
  }
  Tracef("watchdog exiting");
}

inline void Init(HMODULE module) {
  if (trace_file != nullptr) return;

  wchar_t exe[MAX_PATH] = L"";
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir(exe);
  const size_t slash = dir.find_last_of(L"\\/");
  dir = slash != std::wstring::npos ? dir.substr(0, slash) : L".";
  dump_dir = dir;

  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&start_qpc);
  qpc_inv_ms = frequency.QuadPart > 0
                   ? 1000.0 / static_cast<double>(frequency.QuadPart)
                   : 0.0;

  InitializeCriticalSection(&trace_lock);
  trace_lock_ready = true;

  trace_path = dir + L"\\RenoDX-DLSS5-debug.log";
  trace_file = CreateFileW(
      trace_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (trace_file == INVALID_HANDLE_VALUE) {
    wchar_t temp[MAX_PATH] = L"";
    GetTempPathW(MAX_PATH, temp);
    trace_path = std::wstring(temp) + L"RenoDX-DLSS5-debug-" +
                 std::to_wstring(GetCurrentProcessId()) + L".log";
    trace_file = CreateFileW(
        trace_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  if (trace_file == INVALID_HANDLE_VALUE) trace_file = nullptr;
  if (trace_file == nullptr) return;

  Tracef("=== RenoDX DLSS5 DEBUG build: session begin, pid=%lu ===", GetCurrentProcessId());
  Tracef("game exe: %ls", exe);
  wchar_t self[MAX_PATH] = L"";
  GetModuleFileNameW(module, self, MAX_PATH);
  Tracef("addon dll: %ls", self);
  Tracef("trace log: %ls", trace_path.c_str());

  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  symbols_ready = SymInitialize(GetCurrentProcess(), nullptr, FALSE) != FALSE;
  if (module != nullptr) {
    SymLoadModuleExW(
        GetCurrentProcess(), nullptr, self, nullptr,
        reinterpret_cast<DWORD64>(module), 0, nullptr, 0);
  }
  Tracef("symbol engine %s (PDB expected next to the addon dll)", symbols_ready ? "ready" : "FAILED");

  SetUnhandledExceptionFilter(CrashFilter);
  stop_watchdog.store(false);
  watchdog = std::thread(WatchdogLoop);
  watchdog.detach();
  Tracef("debug facilities armed: crash filter + 5s watchdog");
}

// Called from DLL_PROCESS_DETACH (loader lock): signal the watchdog to stop
// and leave a final trace line.  Never join or delete resources here - the
// process is going away.
inline void Shutdown() {
  if (trace_file == nullptr) return;
  Tracef("debug shutdown requested");
  stop_watchdog.store(true);
}

}  // namespace debug
}  // namespace renodx::addons::dlss5

#define RENODX_DLSS5_TRACE(...) (renodx::addons::dlss5::debug::Tracef(__VA_ARGS__))

#else  // !defined(RENODX_DLSS5_DEBUG)

namespace renodx::addons::dlss5 {
namespace debug {
inline constexpr void Init(HMODULE) {}
inline constexpr void Shutdown() {}
inline constexpr void Tracef(const char*, ...) {}
inline constexpr void Mark(const char*) {}
inline constexpr void TouchPresent() {}
inline constexpr void TouchNgxCreate() {}
inline constexpr void TouchNgxEval() {}
}  // namespace debug
}  // namespace renodx::addons::dlss5

#define RENODX_DLSS5_TRACE(...) ((void)0)

#endif
