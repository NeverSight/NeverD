//===- X86LegacyStackExecution.cpp - Ordered protected stack frames ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent data/stack widths, partial POPA and retained ENTER accesses.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalar.h"

#include "llvm/Support/Errc.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <tuple>
#include <utility>

namespace neverd::translate::detail {
namespace {

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

bool sameCache(const X86LegacySegmentCache &A, const X86LegacySegmentCache &B) {
  return std::tie(A.Base, A.Limit, A.Selector, A.Attributes, A.Unusable) ==
         std::tie(B.Base, B.Limit, B.Selector, B.Attributes, B.Unusable);
}

class StackExecution {
public:
  StackExecution(X86LegacyExecutionState &State, X86PagedMemory &Memory,
                 const X86LegacyScalarInstruction &Instruction,
                 const X86LegacyExecutionOptions &Options)
      : S(State), M(Memory), F(Instruction), O(Options), P(*S.PendingStack),
        GPR(P.Entry.GPR),
        StackMask(P.Entry.Segments[2].Attributes & 0x4000 ? UINT32_MAX
                                                          : UINT16_MAX) {}

  void execute() {
    using Op = X86LegacyScalarOperation;
    if (F.Operation == Op::PushAll) {
      for (unsigned Reg = 0; Reg != GPR.size(); ++Reg)
        if (!push(P.Entry.GPR[Reg]))
          return;
    } else if (F.Operation == Op::PopAll) {
      for (unsigned Reg = GPR.size(); Reg-- != 0;) {
        uint32_t Value = 0;
        if (!access(GPR[4] & StackMask, Value, false))
          return;
        if (Reg != 4) {
          writeRegister(Reg, Value);
          // Original CPUs retain each loaded register on a later fault.
          // The saved SP value is discarded, but its read still faults and
          // matches data comparators in the bound ordered-RAM profile.
          S.Core.GPR[Reg] = GPR[Reg];
          P.ExpectedGPR[Reg] = GPR[Reg];
        }
        adjustStack(F.Bytes);
      }
    } else {
      if (!push(GPR[5]))
        return;
      // The saved frame pointer uses operand-width subtraction of entry ESP,
      // even when a word stack wraps without changing architectural ESP high.
      const uint32_t Frame = P.Entry.GPR[4] - F.Bytes;
      uint32_t Cursor = GPR[5];
      for (unsigned I = 1; I < F.Nesting; ++I) {
        // The display address uses SS.B, including borrow across bit 16.
        // It is a temporary: final ENTERW keeps the original high EBP.
        Cursor = (Cursor - F.Bytes) & StackMask;
        uint32_t Value = 0;
        if (!access(Cursor, Value, false) || !push(Value))
          return;
      }
      if (F.Nesting && !push(Frame))
        return;
      adjustStack(-uint32_t(F.Discard));
      uint32_t Ignored = 0;
      if (!access(GPR[4] & StackMask, Ignored, true, true))
        return;
      writeRegister(5, Frame);
    }
    if (AccessIndex != P.CompletedAccesses.size()) {
      Failure =
          invalid("legacy stack continuation has excess completed accesses");
      return;
    }
    S.Core.GPR = GPR;
  }

  llvm::Expected<X86LegacyScalarResult> finish() {
    if (Failure)
      return std::move(*Failure);
    return std::move(Result);
  }

private:
  X86LegacyExecutionState &S;
  X86PagedMemory &M;
  const X86LegacyScalarInstruction &F;
  const X86LegacyExecutionOptions &O;
  X86LegacyStackContinuation &P;
  std::array<uint32_t, 8> GPR;
  uint32_t StackMask;
  unsigned AccessIndex = 0;
  X86LegacyScalarResult Result;
  std::optional<llvm::Error> Failure;

  void writeRegister(unsigned Reg, uint32_t Value) {
    const uint32_t Mask = F.Bytes == 4 ? UINT32_MAX : UINT16_MAX;
    GPR[Reg] = (GPR[Reg] & ~Mask) | (Value & Mask);
  }

  void adjustStack(uint32_t Amount) {
    GPR[4] = (GPR[4] & ~StackMask) | ((GPR[4] + Amount) & StackMask);
  }

  bool push(uint32_t Value) {
    adjustStack(-F.Bytes);
    return access(GPR[4] & StackMask, Value, true);
  }

  bool fault(uint8_t Vector) {
    Result.Status = X86PageWalkStatus::Exception;
    Result.Exception = X86MemoryException{Vector, 0, {}};
    return false;
  }

  bool accept(const X86MemoryAccessResult &Got) {
    Result.SelfModifications.insert(Result.SelfModifications.end(),
                                    Got.SelfModifications.begin(),
                                    Got.SelfModifications.end());
    Result.Status = Got.Status;
    Result.Exception = Got.Exception;
    Result.BackingFault = Got.BackingFault;
    return Got.Status == X86PageWalkStatus::Completed;
  }

  bool access(uint32_t Offset, uint32_t &Value, bool Write,
              bool Probe = false) {
    if (AccessIndex < P.CompletedAccesses.size()) {
      const auto &Done = P.CompletedAccesses[AccessIndex++];
      if (Probe || Done.Offset != Offset || Done.Write != Write ||
          (Write && Done.Value != Value)) {
        Failure = invalid("legacy stack continuation access does not match");
        return false;
      }
      Value = Done.Value;
      return true;
    }
    auto Checked = checkX86LegacySegmentAccess(
        P.Entry.Segments[2],
        {X86SegmentRegister::SS,
         Write ? X86SegmentAccessKind::Write : X86SegmentAccessKind::Read,
         Offset, F.Bytes, O.Segments.FullLimitChoice,
         bool(S.EFlags & 0x20000)});
    if (!Checked) {
      Failure = Checked.takeError();
      return false;
    }
    if (Checked->FaultVector)
      return fault(*Checked->FaultVector);
    const bool AC = P.Entry.CPL == 3 && (M.controls().CR0 & 0x40000) &&
                    (P.EFlags & 0x40000) && (Checked->Linear & (F.Bytes - 1));
    if (AC &&
        O.Segments.AlignmentPriority == X86AlignmentPriority::BeforePaging)
      return fault(17);
    const X86LinearAccess Access{Checked->Linear,
                                 Write ? MemoryAccessKind::Write
                                       : MemoryAccessKind::Read,
                                 P.Entry.CPL,
                                 false,
                                 true,
                                 true};
    std::array<uint8_t, 4> Buffer{};
    auto Span = llvm::MutableArrayRef(Buffer).take_front(F.Bytes);
    if (Write) {
      auto Store = M.prepareStore(Access, F.Bytes);
      if (!Store) {
        Failure = Store.takeError();
        return false;
      }
      if (!accept(*Store))
        return false;
      if (AC)
        return fault(17);
      if (Probe)
        return true; // A/D and permissions, but no RAM store or watchpoint.
      for (unsigned I = 0; I != F.Bytes; ++I)
        Buffer[I] = Value >> (I * 8);
      M.commitStore(*Store, Span);
    } else {
      auto Loaded = M.load(Access, Span);
      if (!Loaded) {
        Failure = Loaded.takeError();
        return false;
      }
      if (!accept(*Loaded))
        return false;
      if (AC)
        return fault(17);
      Value = 0;
      for (unsigned I = 0; I != F.Bytes; ++I)
        Value |= uint32_t(Buffer[I]) << (I * 8);
    }
    P.CompletedAccesses.push_back({Offset, Value, Write});
    ++AccessIndex;
    return true;
  }
};

} // namespace

llvm::Error
validateLegacyStackContinuation(const X86LegacyExecutionState &State,
                                llvm::ArrayRef<uint8_t> Instruction) {
  if (!State.PendingStack)
    return llvm::Error::success();
  const auto &P = *State.PendingStack;
  const auto &C = State.Core;
  if (State.PendingEvent || P.EIP != State.EIP || P.EFlags != State.EFlags ||
      C.GPR != P.ExpectedGPR || C.CPL != P.Entry.CPL ||
      C.GDTRBase != P.Entry.GDTRBase || C.GDTRLimit != P.Entry.GDTRLimit ||
      !sameCache(C.LDTR, P.Entry.LDTR) ||
      !std::equal(C.Segments.begin(), C.Segments.end(),
                  P.Entry.Segments.begin(), sameCache) ||
      Instruction != llvm::ArrayRef(P.Instruction) ||
      (P.DataBreakpoints & ~15u))
    return invalid("resume the retained legacy stack instruction and state");
  return llvm::Error::success();
}

llvm::Expected<X86LegacyScalarResult>
executeLegacyStack(X86LegacyExecutionState &State, X86PagedMemory &Memory,
                   const X86LegacyScalarInstruction &Instruction,
                   llvm::ArrayRef<uint8_t> Bytes, uint32_t Fallthrough,
                   const X86LegacyExecutionOptions &Options) {
  if (!State.PendingStack) {
    State.PendingStack.emplace();
    auto &P = *State.PendingStack;
    P.Entry = State.Core;
    P.ExpectedGPR = State.Core.GPR;
    P.EIP = State.EIP;
    P.EFlags = State.EFlags;
    P.Instruction.assign(Bytes.begin(), Bytes.end());
  }
  StackExecution Execution(State, Memory, Instruction, Options);
  Execution.execute();
  auto Result = Execution.finish();
  if (Result && !Result->BackingFault) {
    State.PendingStack.reset();
    if (Result->Status == X86PageWalkStatus::Completed)
      State.EIP = Fallthrough;
  }
  return Result;
}

} // namespace neverd::translate::detail
