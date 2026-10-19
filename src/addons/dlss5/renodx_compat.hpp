// Compatibility shims for building this add-on against the PUBLIC RenoDX
// `main` checkout.
//
// The v7.5.0-rc5 add-on snapshot was written against a RenoDX revision whose
// utils carry five helpers that public `main` (checked 2026-10-18) does not:
//   renodx::utils::directx::NativeIdentity / SameNativeObject
//   renodx::utils::vtable::TransactionMutex / BeginTransaction /
//   transaction_contended
// They are small and their contract is fully determined by how the add-on
// uses them, so they are provided here.  If your RenoDX checkout already has
// them, define DLSS5_NO_RENODX_COMPAT (or delete this include) to avoid a
// redefinition.
#pragma once

#ifndef DLSS5_NO_RENODX_COMPAT

#include <atomic>
#include <cstdint>
#include <mutex>

#include <unknwn.h>
#include <detours.h>

namespace renodx::utils::directx {

// The canonical COM identity of an object: its IUnknown pointer.  Two
// interface pointers name the same object exactly when their identities are
// equal (COM rules).  The reference QueryInterface adds is released at once:
// the identity is a comparison key, never dereferenced.
inline IUnknown* NativeIdentity(IUnknown* object) {
  if (object == nullptr) return nullptr;
  IUnknown* identity = nullptr;
  if (FAILED(object->QueryInterface(__uuidof(IUnknown),
                                    reinterpret_cast<void**>(&identity)))
      || identity == nullptr) {
    return object;
  }
  identity->Release();
  return identity;
}

inline bool SameNativeObject(IUnknown* a, IUnknown* b) {
  if (a == nullptr || b == nullptr) return a == b;
  return NativeIdentity(a) == NativeIdentity(b);
}

}  // namespace renodx::utils::directx

namespace renodx::utils::vtable {

// Serializes this module's Detours transactions (Detours allows one pending
// transaction per process; callers hold this around Begin..Commit/Abort).
inline std::mutex& TransactionMutex() {
  static std::mutex mutex;
  return mutex;
}

// Times a transaction could not begin because another one (another thread,
// another module) was pending.  Telemetry and test gates read it.
inline std::atomic_uint64_t transaction_contended{0};

// DetourTransactionBegin with the contention counted.  False means no
// transaction is open; callers then skip the install and retry on their
// existing backoff.
inline bool BeginTransaction() {
  const LONG result = DetourTransactionBegin();
  if (result == NO_ERROR) return true;
  if (result == ERROR_INVALID_OPERATION) {
    transaction_contended.fetch_add(1, std::memory_order_relaxed);
  }
  return false;
}

}  // namespace renodx::utils::vtable

#endif  // DLSS5_NO_RENODX_COMPAT
