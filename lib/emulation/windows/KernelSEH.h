//===- KernelSEH.h - Checked x64 catch-all exception transfer -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pure execution plans over the loader's authoritative x64 unwind records.
/// This is not a decoder or a mechanism for resuming a faulted CPU.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_WINDOWS_KERNELSEH_H
#define NEVERD_EMULATION_WINDOWS_KERNELSEH_H

#include "neverd/loader/ExceptionTable.h"

#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace neverd::emulation {
namespace seh {
#define NEVERD_SEH_VALUE(Name, Value) inline constexpr uint64_t Name = Value;
#include "KernelSEHValues.def"
#undef NEVERD_SEH_VALUE
} // namespace seh

class KernelSEH final {
public:
  struct Context {
    /// Architectural register encoding: RAX, RCX, RDX, RBX, RSP, RBP, RSI,
    /// RDI, R8 through R15. PC is the control address used for scope lookup.
    std::array<uint64_t, seh::RegisterCount> GPR{};
    uint64_t PC = 0;
  };
  struct Stack {
    uint64_t Base = 0;
    uint64_t Size = 0;
  };
  struct Transfer {
    Context Registers;
    uint64_t EstablisherFrame = 0;
    uint64_t HandlerPC = 0;
    uint32_t ExceptionCode = 0;
  };
  using ReadStack64 = std::function<llvm::Expected<uint64_t>(uint64_t)>;
  using IsExecutable = std::function<bool(uint64_t)>;

  /// Metadata retains preferred-base VAs. It must outlive the planner and
  /// remain immutable. Stack reads must be side-effect-free checked reads.
  KernelSEH(const ExceptionInfo &Metadata, uint64_t PreferredBase,
            uint64_t ActualBase, uint64_t ImageSize, ReadStack64 ReadStack,
            IsExecutable Executable);

  /// Caller is a local copy after the modeled raising API's return-address
  /// pop. Its control PC is the checked saved return address minus one.
  /// No CPU or memory state is committed here. A missing handler returns
  /// nullopt; an encountered malformed or unsupported contract returns Error.
  llvm::Expected<std::optional<Transfer>>
  plan(uint32_t ExceptionCode, const Context &Caller, Stack Bounds) const;

private:
  const ExceptionInfo &Metadata;
  uint64_t PreferredBase;
  uint64_t ActualBase;
  uint64_t ImageSize;
  ReadStack64 ReadStack;
  IsExecutable Executable;
};

} // namespace neverd::emulation
#endif // NEVERD_EMULATION_WINDOWS_KERNELSEH_H
