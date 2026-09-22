//===- X64ExecutionPolicy.h - Driver CPU execution policy -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Instruction admission policy for the bounded driver environment.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_X64EXECUTIONPOLICY_H
#define NEVERD_EMULATION_X64EXECUTIONPOLICY_H

#include "X64Registers.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <capstone/capstone.h>
#include <cstdint>
#include <optional>

namespace neverd::emulation {

class X64ExecutionPolicy {
public:
  X64ExecutionPolicy() = default;
  X64ExecutionPolicy(const X64ExecutionPolicy &) = delete;
  X64ExecutionPolicy &operator=(const X64ExecutionPolicy &) = delete;
  ~X64ExecutionPolicy();
  llvm::Error initialize();
  /// An admitted CR8 read requires an exact model action. The decoder names
  /// its full-width destination; the executor supplies the authoritative CR8.
  /// Ordinary admitted instructions return nullopt and execute in the backend.
  llvm::Expected<std::optional<X64Register>>
  inspect(llvm::ArrayRef<uint8_t> Bytes, uint64_t PC);
  llvm::Error validate(llvm::ArrayRef<uint8_t> Bytes, uint64_t PC);

private:
  csh Handle = 0;
};

} // namespace neverd::emulation
#endif // NEVERD_EMULATION_X64EXECUTIONPOLICY_H
