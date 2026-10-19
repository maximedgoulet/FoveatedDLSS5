/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// runtime_mutex, with the lock order it is supposed to keep made visible.
//
// One non-recursive mutex serializes the whole addon: the feature map, the
// hook installs, the per-present lifecycle, device init and shutdown.  Three
// places in dlssnr.hpp already say, in comments, that it must not be held
// across a call into the Windows loader --
//
//   "discovery (phase 1) runs WITHOUT runtime_mutex because GetModuleHandleW
//    ... take[s] the loader lock - holding runtime_mutex across such a wait
//    can deadlock with a DllMain on another thread"           (InstallHooks)
//   "Discovery without runtime_mutex (GetModuleHandleW takes the loader
//    lock; ...)"                                    (InstallStreamlineHooks)
//   "GetModuleHandleW needs the loader lock, which is why it runs here,
//    outside runtime_mutex"                                      (OnPresent)
//
// -- and the evaluate path does exactly what those three avoid.  The first
// engaged evaluate of a session runs
//
//   HookedEvaluateFeature -> scoped_lock(runtime_mutex) -> ProcessInline
//     -> EnsureDirectRuntime -> LoadDirectApi -> LoadLibraryW(nvngx_dlssnr)
//
// so thread A holds runtime_mutex and waits on the loader lock, while any
// thread B inside a DllMain (which Windows runs holding the loader lock) that
// reaches OnPresent, a hooked NGX export, OnInitDevice or Shutdown waits on
// runtime_mutex.  That is the textbook inversion, and both halves are on
// paths a game takes without doing anything unusual: games load modules on
// worker threads all session long, and DLSS itself is loaded from one.
//
// The rule cannot be enforced by the type system the way direct_call.hpp's
// can, because the loader is reached through the Win32 API.  So it is
// enforced the next best way: it is COUNTED.  Every call into the loader made
// while this thread holds runtime_mutex increments
// `runtime_lock_loader_calls` and records its site name; the telemetry line
// carries both, and a T2 lane fails on a non-zero count.  An invariant with a
// number attached is one a regression trips over.
//
// The same wrapper answers the other half of the R8 row - how long the lock
// is actually held - without paying for a clock on every acquisition.  An
// uncontended acquisition costs what std::scoped_lock cost (one atomic
// exchange, via try_lock) and reads no clock at all; only an acquisition that
// really had to block takes timestamps, and there the two clock reads are
// dwarfed by the wait being measured.  So `runtime_lock_wait_max_ns` is the
// worst stall a thread took waiting for this mutex, measured in the field, at
// no cost to sessions where the lock is never contended.
//
// Dependency-free so test/dlss5 can exercise the depth and the counter.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>

namespace renodx::addons::dlss5 {

// Session diagnostics, reported in the telemetry line.

// Calls into the Windows loader (LoadLibraryW, GetModuleHandleW,
// GetProcAddress, FreeLibrary) that this addon makes AT ALL, whether or not
// the lock is held.  This is the positive control for the counter below: an
// "under=0" that comes from the notes having been deleted, or from the
// loader paths never running in a given profile, looks exactly like a
// passing invariant.  T-LOCK requires this one to be non-zero before it
// believes the other one, for the same reason a null result needs a
// did-our-code-run arm.
inline std::atomic_uint64_t runtime_lock_loader_calls_total{0};
// ...and, of those, the ones made while this thread held runtime_mutex.
// The lock-order rule says this is zero; a T2 lane pins it there.
inline std::atomic_uint64_t runtime_lock_loader_calls{0};
// The site of the most recent one, so a field log names the offender instead
// of only counting it.  A string literal with static storage duration, so the
// pointer is always safe to print.
inline std::atomic<const char*> runtime_lock_loader_site{nullptr};

// Work that had to be REFUSED because the caller already held runtime_mutex -
// today, a symbol prime (dlssnr.hpp PrimeNgxLoaderSymbols).  Zero by design,
// because every prime site is outside the lock; refusing rather than
// performing is how a future caller in the wrong place costs a frame of NR
// instead of hanging the game.
inline std::atomic_uint64_t ngx_loader_prime_inversions{0};

// Loader calls made from inside this addon's own DllMain.  Windows runs
// DllMain holding the loader lock, so the families differ there:
//
//   GetModuleHandleW / GetProcAddress / GetModuleFileNameW re-enter a lock
//   this thread already owns, which is legal.
//
//   Tool Help (CreateToolhelp32Snapshot / Module32*), psapi
//   (EnumProcessModules) and LoadLibraryW / FreeLibrary are not: they
//   synchronise with the loader from the outside and can load a module of
//   their own.  This is the classic hang-at-launch shape, and a game with
//   other mods loading DLLs on worker threads is where it bites.
//
//   Detours transactions are fine and stay: Detours' own sample installs
//   from DLL_PROCESS_ATTACH (external/Detours/samples/simple/simple.cpp,
//   the DLL_PROCESS_ATTACH arm of DllMain), and DetourUpdateThread
//   silently drops an attempt to suspend the calling thread
//   (external/Detours/src/detours.cpp, "Silently (and safely) drop any
//   attempt to suspend our own thread"), so passing only GetCurrentThread()
//   suspends nothing and the suspend-a-thread-holding-the-loader-lock
//   deadlock cannot arise from ours at all.
//
// So the invariant is not "no hooks from DllMain" - it is "no loader call
// of the unsafe families from DllMain", and like the one above it is
// counted rather than asserted in a comment.
inline std::atomic_uint64_t loader_calls_in_dll_main{0};
// ...and, of those, the ones from a family that must not run there.  Zero.
inline std::atomic_uint64_t loader_unsafe_calls_in_dll_main{0};
// The site of the most recent unsafe one, so a field log names it.
inline std::atomic<const char*> loader_dll_main_site{nullptr};

// Acquisitions that had to block, and the longest one.  These two say whether
// the single mutex is a contention problem in a real game, rather than the
// question being settled by reading the code.
inline std::atomic_uint64_t runtime_lock_contended{0};
inline std::atomic_uint64_t runtime_lock_wait_max_ns{0};

namespace runtime_lock {

class Scope;
class TryScope;

// Held-ness, with no public way to write it.
class State {
 public:
  static bool HeldByThisThread() noexcept { return depth_ != 0; }
  static std::uint32_t Depth() noexcept { return depth_; }

 private:
  friend class Scope;
  friend class TryScope;
  static inline thread_local std::uint32_t depth_ = 0;
};

inline void NoteWait(std::chrono::steady_clock::time_point started) noexcept {
  const auto waited = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count();
  const auto ns = waited > 0 ? static_cast<std::uint64_t>(waited) : 0ull;
  runtime_lock_contended.fetch_add(1, std::memory_order_relaxed);
  std::uint64_t seen = runtime_lock_wait_max_ns.load(std::memory_order_relaxed);
  while (ns > seen
         && !runtime_lock_wait_max_ns.compare_exchange_weak(
             seen, ns, std::memory_order_relaxed)) {
  }
}

// Blocking acquisition.  Replaces `std::scoped_lock lock(runtime_mutex);`.
class Scope {
 public:
  explicit Scope(std::mutex& mutex) noexcept : mutex_(mutex) {
    // try_lock first so the uncontended path reads no clock: it is the same
    // single atomic exchange std::scoped_lock would have done.
    if (!mutex_.try_lock()) {
      const auto started = std::chrono::steady_clock::now();
      mutex_.lock();
      NoteWait(started);
    }
    ++State::depth_;
  }
  ~Scope() noexcept {
    if (State::depth_ != 0) --State::depth_;
    mutex_.unlock();
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  Scope(Scope&&) = delete;
  Scope& operator=(Scope&&) = delete;

 private:
  std::mutex& mutex_;
};

// Non-blocking acquisition.  Replaces
// `std::unique_lock lock(runtime_mutex, std::try_to_lock);`, including the
// early `lock.unlock()` those sites use.
class TryScope {
 public:
  explicit TryScope(std::mutex& mutex) noexcept : mutex_(mutex) {
    owns_ = mutex_.try_lock();
    if (owns_) ++State::depth_;
  }
  ~TryScope() noexcept { unlock(); }
  [[nodiscard]] bool owns_lock() const noexcept { return owns_; }
  void unlock() noexcept {
    if (!owns_) return;
    owns_ = false;
    if (State::depth_ != 0) --State::depth_;
    mutex_.unlock();
  }
  TryScope(const TryScope&) = delete;
  TryScope& operator=(const TryScope&) = delete;
  TryScope(TryScope&&) = delete;
  TryScope& operator=(TryScope&&) = delete;

 private:
  std::mutex& mutex_;
  bool owns_ = false;
};

// Held for the body of DllMain, so NoteLoaderCall can tell a call made
// under the loader lock from the same call made from a present.  A depth
// rather than a flag because DLL_PROCESS_ATTACH and DLL_PROCESS_DETACH are
// separate entries and a future one could nest.
class DllMainScope {
 public:
  DllMainScope() noexcept { ++depth_; }
  ~DllMainScope() noexcept {
    if (depth_ != 0) --depth_;
  }
  static bool Active() noexcept { return depth_ != 0; }
  DllMainScope(const DllMainScope&) = delete;
  DllMainScope& operator=(const DllMainScope&) = delete;
  DllMainScope(DllMainScope&&) = delete;
  DllMainScope& operator=(DllMainScope&&) = delete;

 private:
  static inline thread_local std::uint32_t depth_ = 0;
};

}  // namespace runtime_lock

using RuntimeLock = runtime_lock::Scope;
using RuntimeTryLock = runtime_lock::TryScope;

inline bool HoldingRuntimeLock() noexcept {
  return runtime_lock::State::HeldByThisThread();
}

// True while this thread is inside the addon's DllMain, which Windows runs
// holding the loader lock.
inline bool InsideDllMain() noexcept {
  return runtime_lock::DllMainScope::Active();
}

// Call immediately before reaching into the Windows loader.  `site` must be a
// string literal; it names the offender in the telemetry line so a field log
// identifies the path without a debugger.  Free when the lock is not held,
// which is the case this is asking about.
// Which family the call belongs to, for the DllMain invariant above.
enum class LoaderCallSafety : std::uint8_t {
  // Re-enters a lock this thread already holds.  Legal from DllMain.
  kRecursive,
  // Tool Help, psapi, LoadLibrary/FreeLibrary.  Must not run there.
  kUnsafeUnderLoaderLock,
};

inline void NoteLoaderCall(
    const char* site,
    LoaderCallSafety safety = LoaderCallSafety::kRecursive) noexcept {
  runtime_lock_loader_calls_total.fetch_add(1, std::memory_order_relaxed);
  if (InsideDllMain()) {
    loader_calls_in_dll_main.fetch_add(1, std::memory_order_relaxed);
    if (safety == LoaderCallSafety::kUnsafeUnderLoaderLock) {
      loader_unsafe_calls_in_dll_main.fetch_add(1,
                                                std::memory_order_relaxed);
      loader_dll_main_site.store(site, std::memory_order_relaxed);
    }
  }
  if (!HoldingRuntimeLock()) return;
  runtime_lock_loader_calls.fetch_add(1, std::memory_order_relaxed);
  runtime_lock_loader_site.store(site, std::memory_order_relaxed);
}

// Row 17 (found at alpha32): renodx::utils::directx::Initialize() makes up to
// seven LoadLibraryW calls the first time a process runs it, and the addon
// reaches it from the codec pipeline (under runtime_mutex, on the evaluate
// path) and from telemetry's QueryVideoMemory - neither was visible to the
// instrument, so `loader_under=0` was blind to a real burst.  Note the FIRST
// Initialize of the process, wherever it lands: the burst happens once (later
// calls no-op behind the utility's initialized guard), so one noted call is
// full coverage, and the site line names the path that paid it.  OnInitDevice
// calls this before its own Initialize as the prime, which is what keeps the
// noted first call OFF the lock in ordinary sessions.
inline void NoteFirstDirectxInitialize() noexcept {
  static std::atomic<bool> noted{false};
  if (noted.exchange(true, std::memory_order_relaxed)) return;
  NoteLoaderCall("directx::Initialize/LoadLibraryW",
                 LoaderCallSafety::kUnsafeUnderLoaderLock);
}

// Printable form of the above, for the telemetry line.
inline const char* LoaderCallSiteName() noexcept {
  const char* site = runtime_lock_loader_site.load(std::memory_order_relaxed);
  return site != nullptr ? site : "-";
}

inline const char* DllMainLoaderSiteName() noexcept {
  const char* site = loader_dll_main_site.load(std::memory_order_relaxed);
  return site != nullptr ? site : "-";
}

// The whole lock-order/contention block, with ONE owner: the telemetry line
// and the teardown line print the same field names because they call the
// same function, and a lane that parses one parses the other.
//
//   loader_under      calls into the loader made holding runtime_mutex.  The
//                     invariant.  Zero.
//   loader_total      calls into the loader made at all.  The positive
//                     control: without it, "under=0" could just mean the
//                     instrument was never reached.
//   site              the most recent offender, or "-" for none.
//   prime_inversions  symbol primes REFUSED because the caller held the lock.
//   blocked           acquisitions that actually had to wait.
//   wait_max_us       the longest such wait.
//   dllmain           loader calls made from inside DllMain at all.  The
//                     positive control for the next one.
//   dllmain_unsafe    of those, ones from a family that must not run
//                     under the loader lock.  The invariant.  Zero.
//   dllmain_site      the most recent such offender, or "-" for none.
inline std::string LockOrderReport() {
  std::ostringstream out;
  out << "locks[loader_under="
      << runtime_lock_loader_calls.load(std::memory_order_relaxed)
      << " loader_total="
      << runtime_lock_loader_calls_total.load(std::memory_order_relaxed)
      << " site=" << LoaderCallSiteName() << " prime_inversions="
      << ngx_loader_prime_inversions.load(std::memory_order_relaxed)
      << " blocked=" << runtime_lock_contended.load(std::memory_order_relaxed)
      << " wait_max_us="
      << (runtime_lock_wait_max_ns.load(std::memory_order_relaxed) / 1000)
      << " dllmain="
      << loader_calls_in_dll_main.load(std::memory_order_relaxed)
      << " dllmain_unsafe="
      << loader_unsafe_calls_in_dll_main.load(std::memory_order_relaxed)
      << " dllmain_site=" << DllMainLoaderSiteName()
      << ']';
  return out.str();
}

}  // namespace renodx::addons::dlss5
