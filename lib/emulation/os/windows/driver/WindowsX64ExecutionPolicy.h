//===- WindowsX64ExecutionPolicy.h - Driver CPU execution policy --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Instruction admission policy for the bounded driver environment.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_WINDOWS_X64EXECUTIONPOLICY_H
#define NEVERD_EMULATION_WINDOWS_X64EXECUTIONPOLICY_H

#include "neverd/emulation/Registers.h"
#include "neverd/emulation/X64BranchModel.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <capstone/capstone.h>
#include <cstdint>
#include <optional>

namespace neverd::emulation {

class WindowsX64ExecutionPolicy {
public:
  WindowsX64ExecutionPolicy() = default;
  WindowsX64ExecutionPolicy(const WindowsX64ExecutionPolicy &) = delete;
  WindowsX64ExecutionPolicy &
  operator=(const WindowsX64ExecutionPolicy &) = delete;
  ~WindowsX64ExecutionPolicy();
  llvm::Error initialize(X64BranchModel Model = X64BranchModel::Intel);
  struct Action {
    enum class Kind {
      ReadIRQL,
      ReadCurrentThread,
      ReadCPUID,
      ReadTimestamp,
      ReadTimestampAndProcessor
    };
    Kind Source;
    std::optional<X64Register> Destination;
    bool operator==(const Action &) const = default;
  };
  /// Environment reads require an exact model action. CR8 names its full-width
  /// destination; current-thread reads require the modeled processor field.
  /// Timestamp reads use the scheduler clock and implicit EDX:EAX/ECX outputs.
  /// Ordinary admitted instructions return nullopt and execute in the backend.
  llvm::Expected<std::optional<Action>> inspect(llvm::ArrayRef<uint8_t> Bytes,
                                                uint64_t PC);
  llvm::Error validate(llvm::ArrayRef<uint8_t> Bytes, uint64_t PC);

private:
  csh Handle = 0;
};

} // namespace neverd::emulation
#endif // NEVERD_EMULATION_WINDOWS_X64EXECUTIONPOLICY_H
