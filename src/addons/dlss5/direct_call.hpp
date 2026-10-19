/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// "This thread is currently inside the signed NGX runtime."
//
// The addon detours four NGX exports, and it also CALLS those exports itself
// (init, create, evaluate, release, shutdown).  Without a fence, its own call
// re-enters its own detour: the wrapper would try to re-lock the
// non-recursive runtime_mutex on this thread and deadlock, and the
// command-state shadow would record the runtime's own binds as if the host
// had made them.  The fence is this per-thread flag.
//
// Until v6.8.0 it was a bare `inline thread_local bool` raised and lowered by
// hand at seven call sites, and three of them (feature release, retired
// feature release, direct Shutdown) had no exception handling at all.  The
// failure that costs is silent and permanent: a C++ exception escaping the
// runtime skips the `= false`, the flag stays raised for the LIFE OF THE
// THREAD, and from then on SuppressCommandStateObservation() is true on every
// call - so the shadow stops learning, every evaluate on that thread declines
// for a missing restore target, and nothing says why.  The mod appears to
// switch itself off mid-session on one thread and keep working on the others
// (PLAN_REHAB_V7.md, R8).
//
// Two things are fixed by moving it here rather than by adding four more
// try/catch blocks:
//
//  1. The reset is a destructor, so it runs on every exit from the scope -
//     return, break, or a C++ exception - without the site having to
//     remember.  (It is NOT a defence against an access violation in the
//     runtime: this addon is built /EHsc, where `catch (...)` does not catch
//     SEH and a structured unwind is not required to run C++ destructors.
//     The four sites that carry `catch (...)` never caught the driver faults
//     in the crash reports either - see the R8 row - and no claim to the
//     contrary belongs in the README.)
//  2. The flag is PRIVATE, with Scope its only friend, so `inside_direct_call
//     = true` no longer compiles.  The invariant is held by the compiler
//     rather than by a reviewer noticing the eighth site - which is the same
//     reason vtable_slots.c is a pile of _Static_asserts.
//
// A depth counter, not a bool, for the same reason InjectedCommandScope is
// one: an inner scope must not lower the fence the outer scope raised.
//
// Dependency-free on purpose, so test/dlss5 can include it and pin the
// behaviour that matters - that the fence falls when the scope is left by an
// exception, and that nesting restores rather than clears.

#pragma once

#include <cstdint>

namespace renodx::addons::dlss5 {

namespace direct_call {

class Scope;

// The state, with no public way to write it.
class State {
 public:
  static bool Active() noexcept { return depth_ != 0; }
  // Exposed for tests and for telemetry; a non-zero depth outside a call is a
  // leak, and one that is visible can be reported.
  static std::uint32_t Depth() noexcept { return depth_; }

 private:
  friend class Scope;
  static inline thread_local std::uint32_t depth_ = 0;
};

// Raise the fence for as long as this object lives.
class Scope {
 public:
  Scope() noexcept { ++State::depth_; }
  ~Scope() noexcept {
    if (State::depth_ != 0) --State::depth_;
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  Scope(Scope&&) = delete;
  Scope& operator=(Scope&&) = delete;
};

}  // namespace direct_call

// The two names the addon uses.  `DirectCallScope guard;` at the top of the
// smallest block that contains the runtime call, and InsideDirectCall()
// wherever the old flag was read.
using DirectCallScope = direct_call::Scope;

inline bool InsideDirectCall() noexcept { return direct_call::State::Active(); }

inline std::uint32_t DirectCallDepth() noexcept {
  return direct_call::State::Depth();
}

}  // namespace renodx::addons::dlss5
