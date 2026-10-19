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

#include "neverd/emulation/DriverProfile.h"
#include "neverd/emulation/Registers.h"
#include "neverd/emulation/X64BranchModel.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <array>
#include <capstone/capstone.h>
#include <cstdint>
#include <memory>
#include <optional>

namespace neverd::emulation {

class WindowsX64ExecutionPolicy {
public:
  enum class Purpose { Driver, ReferenceInitialization };
  WindowsX64ExecutionPolicy() = default;
  WindowsX64ExecutionPolicy(const WindowsX64ExecutionPolicy &) = delete;
  WindowsX64ExecutionPolicy &
  operator=(const WindowsX64ExecutionPolicy &) = delete;
  ~WindowsX64ExecutionPolicy();
  llvm::Error initialize(X64BranchModel Model = X64BranchModel::Intel,
                         bool EnableCPUID = false,
                         bool EnablePageTables = false,
                         Purpose Use = Purpose::Driver, bool EnableMSR = false,
                         bool EnableTimestamp = false);
  struct Action {
    enum class Kind {
      ReadIRQL,
      WriteIRQL,
      DisableInterrupts,
      EnableInterrupts,
      ReadTimestamp,
      ReadCurrentThread,
      ReadCPUID,
      ReadCR3,
      ReadCR4,
      ReadMSR,
      ReadTimestampAndProcessor,
      ReadDisabledShadowStack
    };
    Kind Source;
    std::optional<X64Register> Destination;
    std::optional<X64Register> Input;
    bool operator==(const Action &) const = default;
  };
  /// Environment reads require an exact model action. CR8 names its full-width
  /// destination; current-thread reads require the modeled processor field.
  /// Timestamp reads use the scheduler clock and implicit EDX:EAX/ECX outputs.
  /// Shadow-stack reads preserve all state in this fixed CET-disabled profile.
  /// Ordinary admitted instructions return nullopt and execute in the backend.
  llvm::Expected<std::optional<Action>> inspect(llvm::ArrayRef<uint8_t> Bytes,
                                                uint64_t PC);
  llvm::Error validate(llvm::ArrayRef<uint8_t> Bytes, uint64_t PC);
  /// Classify an ordinary admitted near JMP r64 with no memory/OS action.
  /// The caller separately authenticates code bytes and thread-entry scope.
  llvm::Expected<bool> isReferenceThreadTransfer(llvm::ArrayRef<uint8_t> Bytes,
                                                 uint64_t PC);

private:
  struct Admission {
    uint64_t PC = 0;
    std::array<uint8_t, profile::MaxInstructionSize> Bytes{};
    uint8_t Size = 0;
    std::optional<Action> Result;
  };
  static constexpr size_t CacheEntries = 4096;
  using AdmissionCache = std::array<Admission, CacheEntries>;
  std::unique_ptr<AdmissionCache> Cache;
  llvm::Expected<std::optional<Action>>
  inspectUncached(llvm::ArrayRef<uint8_t> Bytes, uint64_t PC);
  csh Handle = 0;
  bool CPUIDEnabled = false;
  bool PageTablesEnabled = false;
  Purpose Use = Purpose::Driver;
  bool MSREnabled = false;
  bool TimestampEnabled = false;
};

} // namespace neverd::emulation
#endif // NEVERD_EMULATION_WINDOWS_X64EXECUTIONPOLICY_H
