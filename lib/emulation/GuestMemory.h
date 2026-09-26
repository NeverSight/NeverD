//===- GuestMemory.h - Private emulator memory boundary -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private emulator memory boundary.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_GUESTMEMORY_H
#define NEVERD_EMULATION_GUESTMEMORY_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace neverd::emulation {
enum GuestPermission : unsigned { Read = 1, Write = 2, Execute = 4 };
/// A mapping cannot fit in the configured guest memory budget. Callers may
/// translate this specific shortage into their documented allocation result.
class GuestMemoryLimitError : public llvm::ErrorInfo<GuestMemoryLimitError> {
public:
  static char ID;
  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;
};
/// One device mapping. Validate is pure and receives the original transaction
/// before any read/write effect. Offsets are relative to the mapped page base.
/// Callbacks are shared by CPU contexts and live until successful unmap.
struct GuestMMIOCallbacks {
  std::function<llvm::Error(uint64_t, uint64_t, bool)> Validate;
  std::function<llvm::Expected<uint64_t>(uint64_t, unsigned)> Read;
  std::function<llvm::Error(uint64_t, unsigned, uint64_t)> Write;
};
class GuestMemory {
public:
  virtual ~GuestMemory() = default;
  virtual llvm::Error map(uint64_t Address, uint64_t Size,
                          unsigned Permissions) = 0;
  /// Map a second virtual range onto the same RAM pages. The source and alias
  /// are page aligned; the owner must separately govern their lifetimes.
  virtual llvm::Error mapAlias(uint64_t Address, uint64_t Source, uint64_t Size,
                               unsigned Permissions);
  virtual llvm::Error protect(uint64_t Address, uint64_t Size,
                              unsigned Permissions) = 0;
  /// Optional device access support; unrelated memory implementations reject
  /// it. Address and Size describe whole nonempty pages. Device pages are NX.
  virtual llvm::Error mapMMIO(uint64_t Address, uint64_t Size,
                              GuestMMIOCallbacks Callbacks);
  /// Retire exactly one complete device mapping, including its callbacks.
  virtual llvm::Error unmapMMIO(uint64_t Address, uint64_t Size);
  virtual llvm::Error read(uint64_t Address,
                           llvm::MutableArrayRef<uint8_t> Bytes) = 0;
  virtual llvm::Error write(uint64_t Address,
                            llvm::ArrayRef<uint8_t> Bytes) = 0;
  /// Pure whole-range validation for device access to existing RAM backing.
  /// This bypasses CPU permissions, never MMIO or mapping/lifetime checks.
  /// The model must separately authorize the exact live allocation and pins.
  virtual llvm::Error validateBacking(uint64_t Address, uint64_t Size) const;
  /// Pure CPU-permission preflight. False means an access would fault; no
  /// first-fault state may be latched by this query.
  virtual llvm::Expected<bool> canAccess(uint64_t Address, uint64_t Size,
                                         unsigned Permissions) const;
  /// Access the same RAM bytes without changing CPU permissions.
  /// Implementations validate the complete span before effects and reject
  /// running/faulted CPUs. Unexpected engine failures must prevent further
  /// execution or device access.
  virtual llvm::Error readBacking(uint64_t Address,
                                  llvm::MutableArrayRef<uint8_t> Bytes);
  virtual llvm::Error writeBacking(uint64_t Address,
                                   llvm::ArrayRef<uint8_t> Bytes);
  /// Diagnostic snapshot of existing RAM, including after a terminal fault.
  /// Requires a stopped CPU without an active device callback. Validate the
  /// complete span before copying; never invoke MMIO, change permissions or
  /// clear fault state. This observation does not authorize guest/device
  /// access.
  virtual llvm::Error snapshotBacking(uint64_t Address,
                                      llvm::MutableArrayRef<uint8_t> Bytes);
  llvm::Expected<uint64_t> readInteger(uint64_t Address, unsigned Size);
  llvm::Error writeInteger(uint64_t Address, uint64_t Value, unsigned Size);
};
} // namespace neverd::emulation
#endif
