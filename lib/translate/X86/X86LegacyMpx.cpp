//===- X86LegacyMpx.cpp - Protected bounds execution ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve original segmentation, paging effects and completed BDE reads.
//===----------------------------------------------------------------------===//

#include "X86LegacyMpx.h"

#include "neverd/lift/X86/X86MpxExecution.h"

#include "llvm/Support/Errc.h"

#include <algorithm>
#include <array>
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

class MpxMemory final : public X86MpxMemoryProvider {
public:
  MpxMemory(X86LegacyExecutionState &State, X86PagedMemory &Memory,
            llvm::ArrayRef<uint8_t> Bytes,
            const X86LegacyExecutionOptions &Options)
      : S(State), M(Memory), Bytes(Bytes), O(Options) {}

  X86MpxMemoryCompletion access(const X86MpxMemoryAccess &A) override {
    if (!S.PendingMpx) {
      S.PendingMpx.emplace();
      auto &P = *S.PendingMpx;
      P.Entry = S.Core;
      P.Controls = M.controls();
      P.Mpx = S.Mpx;
      P.EIP = S.EIP;
      P.EFlags = S.EFlags;
      P.Instruction.assign(Bytes.begin(), Bytes.end());
    }
    const bool Directory = A.Operation == X86MpxMemoryOperation::LoadDirectory;
    if (Directory && S.PendingMpx->Directory) {
      const auto &Done = *S.PendingMpx->Directory;
      if (Done.Address != A.Address) {
        Failure = invalid("legacy MPX directory continuation does not match");
        return {};
      }
      return {X86MpxMemoryOutcome::Completed, {Done.Value, 0, 0}};
    }
    const auto Outcome = accessRAM(A);
    if (Directory && Outcome.Outcome == X86MpxMemoryOutcome::Completed)
      S.PendingMpx->Directory = X86LegacyMpxContinuation::DirectoryRead{
          uint32_t(A.Address), uint32_t(Outcome.Words[0])};
    return Outcome;
  }

  llvm::Expected<X86MemoryAccessResult> finish(X86MpxResult Status) {
    if (Failure)
      return std::move(*Failure);
    if (Status == X86MpxResult::BoundsFault ||
        Status == X86MpxResult::UndefinedOpcode) {
      Result.Status = X86PageWalkStatus::Exception;
      Result.Exception = X86MemoryException{
          uint8_t(Status == X86MpxResult::BoundsFault ? 5 : 6), 0, {}};
    } else if (Status != X86MpxResult::Completed &&
               Status != X86MpxResult::Disabled &&
               !(Status == X86MpxResult::MemoryFault && Result.Exception) &&
               !(Status == X86MpxResult::UnknownMemory &&
                 Result.BackingFault)) {
      return invalid("legacy MPX requires known operands and memory semantics");
    }
    if (!Result.BackingFault)
      S.PendingMpx.reset();
    return std::move(Result);
  }

private:
  X86LegacyExecutionState &S;
  X86PagedMemory &M;
  llvm::ArrayRef<uint8_t> Bytes;
  const X86LegacyExecutionOptions &O;
  X86MemoryAccessResult Result;
  std::optional<llvm::Error> Failure;

  bool accept(const X86MemoryAccessResult &Got) {
    Result.SelfModifications.insert(Result.SelfModifications.end(),
                                    Got.SelfModifications.begin(),
                                    Got.SelfModifications.end());
    Result.Status = Got.Status;
    Result.Exception = Got.Exception;
    Result.BackingFault = Got.BackingFault;
    return Got.Status == X86PageWalkStatus::Completed;
  }

  X86MpxMemoryCompletion fault(uint8_t Vector) {
    Result.Status = X86PageWalkStatus::Exception;
    Result.Exception = X86MemoryException{Vector, 0, {}};
    return {X86MpxMemoryOutcome::Fault};
  }

  X86MpxMemoryCompletion stopped() const {
    return {Result.BackingFault ? X86MpxMemoryOutcome::Suspended
                                : X86MpxMemoryOutcome::Fault};
  }

  X86MpxMemoryCompletion accessRAM(const X86MpxMemoryAccess &A) {
    const bool Bounds = A.Operation == X86MpxMemoryOperation::LoadBounds ||
                        A.Operation == X86MpxMemoryOperation::StoreBounds;
    const bool Directory = A.Operation == X86MpxMemoryOperation::LoadDirectory;
    const unsigned Size = A.wordBytes() * A.wordCount();
    const bool Write = A.writesMemory();
    uint32_t Linear = A.Address;
    if (!Directory) {
      // Table addresses are already linear. Admission binds a flat DS base;
      // the original MIB segment affected translation, never these accesses.
      const auto Segment = Bounds ? A.Segment : X86SegmentRegister::DS;
      auto Checked = checkX86LegacySegmentAccess(
          S.Core.Segments[unsigned(Segment)],
          {Segment,
           Write ? X86SegmentAccessKind::Write : X86SegmentAccessKind::Read,
           uint32_t(A.Address), Size, O.Segments.FullLimitChoice,
           bool(S.EFlags & 0x20000)});
      if (!Checked) {
        Failure = Checked.takeError();
        return {};
      }
      if (Checked->FaultVector)
        return fault(*Checked->FaultVector);
      Linear = Checked->Linear;
    }
    const bool AC = Bounds && S.Core.CPL == 3 && (M.controls().CR0 & 0x40000) &&
                    (S.EFlags & 0x40000) && (Linear & (Size - 1));
    if (AC &&
        O.Segments.AlignmentPriority == X86AlignmentPriority::BeforePaging)
      return fault(17);
    const X86LinearAccess Access{Linear,
                                 Write ? MemoryAccessKind::Write
                                       : MemoryAccessKind::Read,
                                 S.Core.CPL,
                                 false,
                                 A.usesStackSegment(),
                                 true};
    // Only 32-bit MPX reaches this adapter: two bounds fields or three BTE
    // fields, each four bytes. The reserved fourth table field is untouched.
    std::array<uint8_t, 3 * sizeof(uint32_t)> Buffer{};
    auto Span = llvm::MutableArrayRef(Buffer).take_front(Size);
    if (Write) {
      auto Prepared = M.prepareStore(Access, Size);
      if (!Prepared) {
        Failure = Prepared.takeError();
        return {};
      }
      if (!accept(*Prepared))
        return stopped();
      if (AC)
        return fault(17);
      for (unsigned I = 0; I != Size; ++I)
        Buffer[I] = A.Words[I / 4] >> ((I % 4) * 8);
      M.commitStore(*Prepared, Span);
      return {X86MpxMemoryOutcome::Completed};
    }
    auto Loaded = M.load(Access, Span);
    if (!Loaded) {
      Failure = Loaded.takeError();
      return {};
    }
    if (!accept(*Loaded))
      return stopped();
    if (AC)
      return fault(17);
    X86MpxMemoryCompletion Done{X86MpxMemoryOutcome::Completed};
    for (unsigned I = 0; I != Size; ++I)
      Done.Words[I / 4] |= uint64_t(Buffer[I]) << ((I % 4) * 8);
    return Done;
  }
};

} // namespace

llvm::Error validateLegacyMpxContinuation(const X86LegacyExecutionState &S,
                                          const X86PagedMemory &M,
                                          llvm::ArrayRef<uint8_t> Bytes) {
  if (!S.PendingMpx)
    return llvm::Error::success();
  const auto &P = *S.PendingMpx;
  const auto Paging = [](const X86PagingState &C) {
    return std::tie(C.CR0, C.CR3, C.CR4, C.EFER, C.RFlags, C.PKRU, C.PKRS,
                    C.PDPTE);
  };
  if (S.PendingStack || S.PendingEvent || S.EIP != P.EIP ||
      S.EFlags != P.EFlags || S.Core.GPR != P.Entry.GPR ||
      S.Core.CPL != P.Entry.CPL || S.Mpx != P.Mpx ||
      Paging(M.controls()) != Paging(P.Controls) ||
      !std::equal(S.Core.Segments.begin(), S.Core.Segments.end(),
                  P.Entry.Segments.begin(), sameCache) ||
      Bytes != llvm::ArrayRef(P.Instruction) || (P.DataBreakpoints & ~15u))
    return invalid("resume the retained legacy MPX instruction and state");
  return llvm::Error::success();
}

llvm::Expected<X86MpxContext> bindLegacyMpx(const X86LegacyExecutionState &S,
                                            const X86PagedMemory &M) {
  auto Context = S.Mpx.value_or(X86MpxContext{});
  auto Configuration = Context.configuration();
  if (Configuration.InTransaction == true)
    return invalid("legacy MPX transaction requires an abort continuation");
  Configuration.OSXSAVE = bool(M.controls().CR4 & (uint64_t{1} << 18));
  Configuration.PrivilegeLevel = S.Core.CPL;
  if (!Context.configure(Configuration))
    return invalid("legacy MPX requires a valid configuration");
  const auto Status = Context.operationStatus(X86MpxMode::Bits32, 0);
  if (Status != X86MpxResult::Completed && Status != X86MpxResult::Disabled)
    return invalid("legacy MPX requires known nontransactional enable state");
  return Context;
}

llvm::Error admitLegacyMpxMemory(const X86MpxEncoding &E,
                                 const X86MpxContext &Context,
                                 const X86LegacyExecutionState &S,
                                 const X86PagedMemory &M,
                                 const X86LegacyExecutionOptions &O) {
  if (Context.enabled() == false || E.Detail.addr_size == 2)
    return llvm::Error::success();
  const bool Table = E.Id == X86_INS_BNDLDX || E.Id == X86_INS_BNDSTX;
  const bool Move =
      E.Id == X86_INS_BNDMOV && (E.Detail.operands[0].type == X86_OP_MEM ||
                                 E.Detail.operands[1].type == X86_OP_MEM);
  if ((Table || Move) && O.MpxMemory != X86LegacyMpxProfile::WholeOperandRAM)
    return invalid("legacy MPX memory requires a whole-operand RAM profile");
  if (Table && S.Core.Segments[unsigned(X86SegmentRegister::DS)].Base)
    return invalid("legacy MPX table profile requires a flat DS base");
  if (Move &&
      (O.Segments.Profile != X86LegacySegmentProfile::WordStackOffsetFirst ||
       (O.Segments.AlignmentPriority &&
        *O.Segments.AlignmentPriority != X86AlignmentPriority::BeforePaging &&
        *O.Segments.AlignmentPriority != X86AlignmentPriority::AfterPaging) ||
       (S.Core.CPL == 3 && (M.controls().CR0 & 0x40000) &&
        (S.EFlags & 0x40000) && !O.Segments.AlignmentPriority)))
    return invalid("legacy MPX move requires segment/alignment bindings");
  return llvm::Error::success();
}

llvm::Expected<X86MemoryAccessResult>
executeLegacyMpx(X86LegacyExecutionState &S, X86PagedMemory &M,
                 llvm::ArrayRef<uint8_t> Bytes, bool Code32,
                 X86MpxContext &Context, const X86LegacyExecutionOptions &O) {
  X86MpxExecutionInputs Inputs;
  for (unsigned I = 0; I != S.Core.GPR.size(); ++I)
    Inputs.GPR[I] = S.Core.GPR[I];
  for (unsigned I = 0; I != S.Core.Segments.size(); ++I)
    Inputs.SegmentBases[I] = S.Core.Segments[I].Base;
  MpxMemory Memory(S, M, Bytes, O);
  return Memory.finish(
      executeX86MpxInstruction(
          Bytes, Code32 ? X86MpxCodeMode::Legacy32 : X86MpxCodeMode::Legacy16,
          S.EIP, Context, Inputs, Memory)
          .Result);
}

} // namespace neverd::translate::detail
