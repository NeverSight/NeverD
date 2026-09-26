//===- KernelSEH.h - Checked x64 C exception dispatch ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pure execution plans over the loader's authoritative x64 unwind records.
/// Guest execution and complete CPU state remain owned by the session.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_WINDOWS_KERNELSEH_H
#define NEVERD_EMULATION_WINDOWS_KERNELSEH_H

#include "neverd/loader/ExceptionTable.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <vector>

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
    /// Scope lookup uses return address minus one after a virtual return.
    bool FromReturnAddress = false;
    uint32_t Flags = 0;
    uint16_t CS = 0, SS = 0;
    std::array<std::array<uint64_t, seh::XmmWordCount>,
               seh::NonvolatileXmmCount>
        Xmm{};
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
  enum class ActionKind {
    Filter,
    Finally,
    Handler,
    ContinueExecution,
    Unhandled
  };
  struct Action {
    ActionKind Kind = ActionKind::Unhandled;
    Transfer State;
    Stack Bounds;
    size_t SegmentIndex = 0, ScopeIndex = 0;
    uint32_t ExceptionFlags = 0;
  };
  struct Exception {
    Context Registers;
    uint32_t Code = 0, Flags = 0;
    uint64_t Address = 0;
    std::vector<uint64_t> Parameters;
    uint64_t PreviousRecord = 0;
  };
  /// A suspended search retains the original exception context independently
  /// of the virtual unwind state and of registers clobbered by guest callbacks.
  class Dispatch {
    friend class KernelSEH;
    struct Segment {
      Context Registers;
      Stack Bounds;
      size_t ScopeIndex = 0;
      uint64_t NestedFrame = 0;
    };
    Context Original, Current;
    Stack Bounds;
    std::vector<Segment> Path;
    size_t SegmentIndex = 0, ScopeFloor = 0;
    uint32_t Code = 0;
    uint64_t Depth = 0, OriginalPC = 0, Establisher = 0;
    size_t ScopeIndex = 0, CleanupIndex = 0;
    const ExceptionFunction *Frame = nullptr;
    std::vector<const ExceptionFunction *> UnwindFrames;
    bool FrameActive = false, Complete = false, InPrologue = false;
    uint64_t ControlOffset = 0;
    std::set<std::pair<uint64_t, uint64_t>> Seen;
    std::optional<Transfer> FilterCandidate, Selected;
    std::vector<Action> Cleanups;
  };
  using ReadStack64 = std::function<llvm::Expected<uint64_t>(uint64_t)>;
  using IsExecutable = std::function<bool(uint64_t)>;
  using ReadCode =
      std::function<llvm::Error(uint64_t, llvm::MutableArrayRef<uint8_t>)>;

  /// Metadata retains preferred-base VAs. It must outlive the planner and
  /// remain immutable. Stack reads must be side-effect-free checked reads.
  KernelSEH(const ExceptionInfo &Metadata, uint64_t PreferredBase,
            uint64_t ActualBase, uint64_t ImageSize, ReadStack64 ReadStack,
            IsExecutable Executable, ReadCode Code = {});

  /// Caller is a local copy after the modeled raising API's return-address
  /// pop. Its control PC is the checked saved return address minus one.
  /// No CPU or memory state is committed here. A missing handler returns
  /// nullopt; an encountered malformed or unsupported contract returns Error.
  llvm::Expected<std::optional<Transfer>>
  plan(uint32_t ExceptionCode, const Context &Caller, Stack Bounds) const;

  Dispatch begin(uint32_t ExceptionCode, const Context &Caller,
                 Stack Bounds) const;
  /// Extend the callback's physical stack with its suspended logical stack.
  /// A nested search revisits the original protected scopes. A collided unwind
  /// starts after the termination scope already entered, without rerunning it.
  llvm::Expected<Dispatch> beginNested(uint32_t ExceptionCode,
                                       const Context &Caller, Stack Bounds,
                                       const Dispatch &Suspended,
                                       const Action &Callback) const;
  /// Filter actions require the actual low-32-bit signed guest result on the
  /// next advance. Finally actions advance after their guest call returns.
  /// Errors leave the dispatch cursor unchanged; no guest state is written.
  llvm::Expected<Action>
  advance(Dispatch &State, std::optional<int32_t> FilterResult = {}) const;
  /// Validate the guest records before accepting any filter disposition.
  /// Allowed integer edits remain in guest storage for possible continuation;
  /// they do not replace the independent frame-search/unwind context.
  llvm::Expected<Action> finishFilter(Dispatch &State, int32_t FilterResult,
                                      llvm::ArrayRef<uint8_t> Records,
                                      const Exception &Raised,
                                      uint64_t Storage) const;
  static llvm::Expected<std::vector<uint8_t>>
  encodeRecords(const Exception &Raised, uint64_t Storage);
  llvm::Expected<Context> continuation(llvm::ArrayRef<uint8_t> Records,
                                       const Exception &Raised,
                                       uint64_t Storage, Stack Bounds) const;

private:
  const ExceptionInfo &Metadata;
  uint64_t PreferredBase;
  uint64_t ActualBase;
  uint64_t ImageSize;
  ReadStack64 ReadStack;
  IsExecutable Executable;
  ReadCode Code;
  llvm::Expected<std::optional<Context>>
  unwindEpilogue(const ExceptionFunction &Frame, const Context &Current,
                 Stack Bounds) const;
  llvm::Expected<Action> advanceImpl(Dispatch &State,
                                     std::optional<int32_t> FilterResult) const;
  llvm::Expected<Context> validateRecords(llvm::ArrayRef<uint8_t> Records,
                                          const Exception &Raised,
                                          uint64_t Storage) const;
};

} // namespace neverd::emulation
#endif // NEVERD_EMULATION_WINDOWS_KERNELSEH_H
