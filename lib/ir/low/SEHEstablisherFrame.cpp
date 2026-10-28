//===- SEHEstablisherFrame.cpp - Checked Windows parent frames -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Owns the decoded prologue and protected-frame proof shared by LowIR and SSA.

#include "SEHFrameProofDetail.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/X86/X87Context.h"
#include "neverd/ir/low/SEHFrameProof.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Errc.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>

namespace neverd {
namespace {
// Lifetime is one immutable function query. Never retained on a LowFunc or
// shared with a later, potentially changed snapshot.
struct FrameCFG {
  std::vector<std::vector<int>> Preds;
  std::map<va_t, int> Starts;
  std::map<va_t, std::vector<size_t>> HandlerScopes;
  std::map<size_t, std::vector<int>> GuardedBlocks;
};

bool touchesFunctionTemporary(const LowFunc &F, const NdVar &V) {
  if (!V.isTemp() || !V.Size)
    return false;
  if (V.Offset > std::numeric_limits<uint64_t>::max() - (V.Size - 1))
    return true;
  return llvm::any_of(F.FunctionTemporaries, [&](const auto &Range) {
    return V.Offset <= Range.Offset + (Range.Bytes - 1) &&
           Range.Offset <= V.Offset + (V.Size - 1);
  });
}

bool isWholeFunctionTemporary(const LowFunc &F, const NdVar &V) {
  return V.isTemp() &&
         llvm::any_of(F.FunctionTemporaries, [&](const auto &Range) {
           return Range.Offset == V.Offset && Range.Bytes == V.Size;
         });
}

bool chargeOperation(const LowOp &Op, size_t &WorkRemaining) {
  const size_t Cost = size_t(Op.NumInputs) + 1;
  if (Cost > WorkRemaining) {
    WorkRemaining = 0;
    return false;
  }
  WorkRemaining -= Cost;
  return true;
}
} // namespace

std::optional<bool>
lowFunctionTemporariesAreX87ContextHandles(const LowFunc &F,
                                           size_t &WorkRemaining) {
  if (F.FunctionTemporaries.empty())
    return true;
  for (const LowBlock &Block : F.Blocks)
    for (const LowOp &Op : Block.Ops) {
      if (!chargeOperation(Op, WorkRemaining))
        return std::nullopt;
      const NdVar *Handle = x87ContextHandleOperand(Op);
      auto Admitted = [&](const NdVar &V) {
        return !touchesFunctionTemporary(F, V) ||
               (&V == Handle && isWholeFunctionTemporary(F, V));
      };
      if (!Admitted(Op.Output))
        return false;
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (!Admitted(Op.Inputs[I]))
          return false;
    }
  return true;
}

std::optional<bool> lowBlockBindsFunctionTemporaries(const LowFunc &F,
                                                     const LowBlock &Block,
                                                     size_t &WorkRemaining) {
  std::set<uint64_t> Bound;
  for (const LowOp &Op : Block.Ops) {
    if (Bound.size() == F.FunctionTemporaries.size())
      break;
    if (!chargeOperation(Op, WorkRemaining))
      return std::nullopt;
    for (unsigned I = 0; I < Op.NumInputs; ++I)
      if (touchesFunctionTemporary(F, Op.Inputs[I]) &&
          (!isWholeFunctionTemporary(F, Op.Inputs[I]) ||
           !Bound.count(Op.Inputs[I].Offset)))
        return false;
    if (touchesFunctionTemporary(F, Op.Output)) {
      if (!isWholeFunctionTemporary(F, Op.Output))
        return false;
      Bound.insert(Op.Output.Offset);
    }
  }
  return Bound.size() == F.FunctionTemporaries.size();
}

/// Windows resumes an in-function __except body with its establisher SP, not
/// with the entry SP used for an independent ordinary machine-code root.
/// Certify the fixed-frame case against both normalized unwind actions and the
/// decoded instructions. FrameSize is storage sizing, not this proof.  A frame
/// register leaves that SP unchanged: the unwinder restores it as a
/// nonvolatile, and it is certified only as UWOP_SET_FPREG describes it.
/// A frame-register function may also move SP after its prologue (alloca)
/// outside the protected range; its frame is then established from the
/// frame register, and the handler resumes with the SP the range holds,
/// which no protected block may write. That case has no fixed-SP seed.
static llvm::Expected<LowSEHEstablisherFrame>
proveInFrameCFG(const LowFunc &Low, va_t Handler, const TargetRegInfo &TRI,
                bool OrdinaryEntry, size_t &WorkRemaining,
                llvm::StringRef LocalUnwindDigest, bool CxxContinuation,
                std::optional<FrameCFG> &CachedCFG, LowSEHFrameUse Use) {
  auto Fail = [](const std::string &Why) {
    return llvm::createStringError(llvm::errc::not_supported, Why);
  };
  auto Charge = [&](size_t Amount = 1) {
    if (Amount > WorkRemaining) {
      WorkRemaining = 0;
      return false;
    }
    WorkRemaining -= Amount;
    return true;
  };
  if (TRI.TheArch != Arch::X64 || TRI.PointerSize != 8 ||
      !Low.ExceptionMetadata ||
      (!CxxContinuation && !Low.ExceptionMetadata->SEH))
    return Fail("unsupported architecture or missing exception metadata");
  LowSEHEstablisherFrame Result;
  const ExceptionFunction &EH = *Low.ExceptionMetadata;
  auto IsCxxContinuation = [&](va_t VA) {
    return llvm::any_of(Low.CxxContinuationEntries,
                        [&](const auto &Entry) { return Entry.Target == VA; });
  };
  if (CxxContinuation && EH.Cxx &&
      !Charge(EH.Cxx->UnwindMap.size() + EH.Cxx->IPMap.size() +
              EH.Cxx->TryBlocks.size() + 1))
    return Fail("frame propagation budget exhausted");
  if (CxxContinuation &&
      (!EH.Cxx || !EH.Cxx->hasValidStateGraph() ||
       EH.Cxx->HasDynamicStackAlignment || !IsCxxContinuation(Handler)))
    return Fail("unsupported C++ continuation frame contract");
  const bool UnwindV1 =
      EH.Encoding == ExceptionEncoding::X64UnwindV1 && EH.UnwindVersion == 1;
  const bool UnwindV2 =
      EH.Encoding == ExceptionEncoding::X64UnwindV2 && EH.UnwindVersion == 2;
  const bool HasFrameRegister = EH.FrameRegister != 0;
  const uint64_t FrameRegister = x86reg::generalReg(EH.FrameRegister);
  if (EH.ParseStatus != ExceptionParseStatus::Complete ||
      (!UnwindV1 && !UnwindV2) || EH.Kind != RuntimeFunctionKind::Primary ||
      EH.ChainedPrimaryRange || EH.ChainedUnwindInfoRVA ||
      EH.PrimaryFunctionIndex || (EH.UnwindFlags & ~3u) ||
      (!HasFrameRegister && EH.FrameOffset) ||
      (HasFrameRegister && FrameRegister == TRI.StackPointer) ||
      EH.CodeRange.Begin != Low.Entry || !EH.ownsCode(Handler))
    return Fail("unsupported unwind or frame-register contract");
  if (!Low.hasCompleteLiftCoverage() || Low.Blocks.empty() ||
      Low.Blocks.front().StartAddr != Low.Entry ||
      EH.PrologueSize > EH.CodeRange.End - Low.Entry)
    return Fail("missing decoded prologue");
  if (!Charge(Low.FunctionTemporaries.size()))
    return Fail("frame propagation budget exhausted");
  const std::optional<bool> X87Handles =
      lowFunctionTemporariesAreX87ContextHandles(Low, WorkRemaining);
  if (!X87Handles)
    return Fail("frame propagation budget exhausted");
  if (!*X87Handles)
    return Fail("function temporary other than the x87 context handle");
  const va_t PrologueEnd = Low.Entry + EH.PrologueSize;
  if (Low.Blocks.front().EndAddr < PrologueEnd)
    return Fail("nonlinear prologue is not certified");

  const size_t N = Low.Blocks.size();
  if (!CachedCFG) {
    std::map<int, size_t> Positions;
    for (size_t B = 0; B < N; ++B) {
      if (!Charge())
        return Fail("frame propagation budget exhausted");
      if (Low.Blocks[B].Id != static_cast<int>(B))
        return Fail("noncanonical frame CFG block identity");
      Positions.emplace(Low.Blocks[B].Id, B);
    }
    if (!completeLowSEHFrameCFG(Low, Positions, WorkRemaining,
                                Use == LowSEHFrameUse::SourceProjection))
      return Fail("incomplete decoded frame CFG");
    FrameCFG CFG;
    CFG.Preds.resize(N);
    for (size_t B = 0; B < N; ++B) {
      if (!Charge(Low.Blocks[B].Succs.size() + 1))
        return Fail("frame propagation budget exhausted");
      CFG.Starts.emplace(Low.Blocks[B].StartAddr, static_cast<int>(B));
      for (int S : Low.Blocks[B].Succs) {
        if (S < 0 || static_cast<size_t>(S) >= N)
          return Fail("invalid ordinary CFG");
        CFG.Preds[S].push_back(static_cast<int>(B));
      }
    }
    if (EH.SEH)
      for (size_t I = 0; I < EH.SEH->Scopes.size(); ++I) {
        if (!Charge())
          return Fail("frame propagation budget exhausted");
        CFG.HandlerScopes[EH.SEH->Scopes[I].HandlerVA].push_back(I);
      }
    CachedCFG = std::move(CFG);
  }
  if (!Charge(N + Low.CxxContinuationEntries.size() + 1))
    return Fail("frame propagation budget exhausted");
  const bool SynchronousCxx = CxxContinuation && EH.Cxx->IsSynchronous;
  std::map<int, std::set<va_t>> Calls;
  const auto &Preds = CachedCFG->Preds;
  const auto HandlerPosition = CachedCFG->Starts.find(Handler);
  const int HandlerId =
      HandlerPosition == CachedCFG->Starts.end() ? -1 : HandlerPosition->second;
  if (!Preds[0].empty())
    return Fail("ordinary control flow re-enters the prologue");
  if (HandlerId <= 0 || (!OrdinaryEntry && !Preds[HandlerId].empty()))
    return Fail("handler also has an ordinary entry role");

  std::map<int, std::vector<std::pair<int, int64_t>>> LocalUnwindSources;
  std::vector<std::vector<int>> LocalUnwindSuccessors(N);
  if (!CxxContinuation)
    for (const auto &Entry : Low.SEHLocalUnwindContinuations) {
      if (!Charge(N + 1))
        return Fail("frame propagation budget exhausted");
      int Target = -1, Source = -1;
      for (size_t B = 0; B < N; ++B) {
        if (Low.Blocks[B].StartAddr == Entry.Target)
          Target = static_cast<int>(B);
        if (!Charge(Low.Blocks[B].Ops.size()))
          return Fail("frame propagation budget exhausted");
        for (size_t I = 0; I < Low.Blocks[B].Ops.size(); ++I) {
          const LowOp &Op = Low.Blocks[B].Ops[I];
          if (Op.Addr == Entry.CallAddr && Op.Seq == Entry.CallSeq &&
              Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
              Op.Inputs[0].isConst() && Op.Inputs[0].Size == 8 &&
              Op.Inputs[0].Offset == Entry.Callee) {
            if (Source >= 0 || Entry.OperationDigest.empty() ||
                Entry.OperationDigest != LocalUnwindDigest)
              return Fail("stale local-unwind argument proof");
            Source = static_cast<int>(B);
          }
        }
      }
      if (Target <= 0 || Source < 0 || Entry.FrameOffset >= 0)
        return Fail("stale local-unwind continuation certificate");
      LocalUnwindSources[Target].emplace_back(Source, Entry.FrameOffset);
      LocalUnwindSuccessors[Source].push_back(Target);
    }
  const bool IsLocalUnwindTarget = LocalUnwindSources.count(HandlerId);
  if (!CxxContinuation && Low.OrdinaryModuleAnalysisRoots.count(Handler) &&
      !IsLocalUnwindTarget)
    return Fail("handler also has an ordinary entry role");

  // Every ordinary predecessor path into a protected block participates. A
  // stack adjustment before the guarded interval matters just as much as one
  // inside it. Independent sources cannot inherit the entry prologue proof.
  // An __except body of this function is the one other source: the unwinder
  // resumes it with the SP of its own protected scope, so when its flow
  // reaches this scope, that scope joins the certificate as well.
  std::vector<bool> Relevant(N, false);
  std::vector<bool> ProtectedBlock(N, false);
  // The end of the furthest SEH guarded range each protected block overlaps.
  std::vector<va_t> GuardedEnd(N, 0);
  std::vector<int> Work;
  auto AddProtectedBlocks = [&](va_t ScopeHandler) -> llvm::Expected<bool> {
    const size_t Before = Work.size();
    if (!Charge())
      return Fail("frame propagation budget exhausted");
    if (CxxContinuation) {
      // CFGBuilder owns the state-to-protected-block relation. For a catch
      // nested inside another catch, follow that catch's containing try to
      // reach the parent's protected frame, rather than borrowing the nested
      // funclet's own prologue.
      const auto &Tries = EH.Cxx->TryBlocks;
      std::set<size_t> Regions;
      for (const auto &Entry : Low.CxxContinuationEntries) {
        if (!Charge())
          return Fail("frame propagation budget exhausted");
        if (Entry.Target != ScopeHandler)
          continue;
        for (size_t I = 0; I < Tries.size(); ++I) {
          if (!Charge(Tries[I].Handlers.size() + 1))
            return Fail("frame propagation budget exhausted");
          if (llvm::any_of(Tries[I].Handlers, [&](const auto &Catch) {
                return Catch.HandlerVA == Entry.SourceEntry;
              }))
            Regions.insert(I);
        }
      }
      bool Changed = true;
      while (Changed) {
        Changed = false;
        if (Tries.size() && (Regions.size() > WorkRemaining / Tries.size() ||
                             !Charge(Regions.size() * Tries.size())))
          return Fail("frame propagation budget exhausted");
        const auto Current = Regions;
        for (size_t I : Current)
          for (size_t J = 0; J < Tries.size(); ++J)
            if (Tries[I].TryLow > Tries[J].TryHigh &&
                Tries[I].CatchHigh <= Tries[J].CatchHigh)
              Changed |= Regions.insert(J).second;
      }
      for (size_t B = 0; B < N; ++B) {
        if (!Charge(Low.Blocks[B].ExceptionalSuccs.size() + 1))
          return Fail("frame propagation budget exhausted");
        bool HasEdge = false;
        for (const ExceptionalEdge &Edge : Low.Blocks[B].ExceptionalSuccs) {
          if (Edge.Kind != ExceptionalEdgeKind::CxxCatch)
            continue;
          if (!Charge(Edge.contexts().size() - 1))
            return Fail("frame propagation budget exhausted");
          if (std::none_of(Edge.contexts().begin(), Edge.contexts().end(),
                           [&](const auto &Context) {
                             return Regions.count(Context.RegionIndex);
                           }))
            continue;
          if (SynchronousCxx) {
            const auto &Insns = Low.Blocks[B].InstructionBoundaries;
            if (!Charge(Insns.size() + 1))
              return Fail("frame propagation budget exhausted");
            const auto Call = llvm::find_if(Insns, [&](const auto &Insn) {
              return Insn.Address == Edge.SourceVA &&
                     Insn.Control == LowInstructionControl::Call;
            });
            if (Call == Insns.end())
              return Fail("synchronous catch lacks a decoded throwing call");
            if (Call->Address < PrologueEnd)
              return Fail("protected call overlaps an incomplete prologue");
            Calls[static_cast<int>(B)].insert(Call->Address);
          }
          HasEdge = true;
        }
        if (HasEdge) {
          if (!SynchronousCxx && Low.Blocks[B].StartAddr < PrologueEnd &&
              Low.Blocks[B].EndAddr > Low.Entry)
            return Fail("protected scope overlaps an incomplete prologue");
          Work.push_back(static_cast<int>(B));
          ProtectedBlock[B] = true;
        }
      }
      return Work.size() != Before;
    }
    const auto Scopes = CachedCFG->HandlerScopes.find(ScopeHandler);
    if (Scopes == CachedCFG->HandlerScopes.end())
      return false;
    for (size_t Index : Scopes->second) {
      if (!Charge())
        return Fail("frame propagation budget exhausted");
      const SEHScopeRecord &Scope = EH.SEH->Scopes[Index];
      // Finally callbacks enter on their own invocation stack. Their guarded
      // parent does not establish the callback SP that ordinary edges carry.
      if (Scope.Kind == SEHScopeKind::Finally)
        return Fail("finally callback has a separate invocation frame");
      // A cold fragment can sit below the entry; only the prologue itself
      // is out of bounds.
      auto Range = getSemanticSEHGuardedRange(Scope, Arch::X64, EH);
      if (Scope.ParseStatus != ExceptionParseStatus::Complete || !Range ||
          (Range->Begin < PrologueEnd && Range->End > Low.Entry))
        return Fail("protected scope overlaps an incomplete prologue");
      auto Guarded = CachedCFG->GuardedBlocks.find(Index);
      if (Guarded == CachedCFG->GuardedBlocks.end()) {
        if (!Charge(N))
          return Fail("frame propagation budget exhausted");
        std::vector<int> Blocks;
        for (size_t B = 0; B < N; ++B)
          if (Low.Blocks[B].StartAddr < Range->End &&
              Low.Blocks[B].EndAddr > Range->Begin)
            Blocks.push_back(static_cast<int>(B));
        Guarded =
            CachedCFG->GuardedBlocks.emplace(Index, std::move(Blocks)).first;
      }
      if (!Charge(Guarded->second.size()))
        return Fail("frame propagation budget exhausted");
      for (int B : Guarded->second) {
        Work.push_back(B);
        ProtectedBlock[B] = true;
        GuardedEnd[B] = std::max(GuardedEnd[B], Range->End);
      }
    }
    return Work.size() != Before;
  };
  auto IsHandlerSource = [&](int B) {
    const va_t Start = Low.Blocks[B].StartAddr;
    if (CxxContinuation)
      return B != 0 && Preds[B].empty() && IsCxxContinuation(Start);
    return B != 0 && Preds[B].empty() &&
           !Low.OrdinaryModuleAnalysisRoots.count(Start) &&
           CachedCFG->HandlerScopes.count(Start);
  };
  auto Added = AddProtectedBlocks(Handler);
  if (!Added)
    return Added.takeError();
  if (!*Added && !IsLocalUnwindTarget)
    return Fail("handler has no decoded protected scope");
  if (IsLocalUnwindTarget)
    for (const auto &[Source, Offset] : LocalUnwindSources.at(HandlerId))
      Work.push_back(Source);
  // Ordinary flow into a shared handler must keep the same frame.
  if (OrdinaryEntry)
    Work.insert(Work.end(), Preds[HandlerId].begin(), Preds[HandlerId].end());
  std::vector<int> Sources{0};
  while (!Work.empty()) {
    if (!Charge())
      return Fail("frame propagation budget exhausted");
    const int B = Work.back();
    Work.pop_back();
    if (Relevant[B])
      continue;
    Relevant[B] = true;
    Work.insert(Work.end(), Preds[B].begin(), Preds[B].end());
    if (auto It = LocalUnwindSources.find(B); It != LocalUnwindSources.end())
      for (const auto &[Source, Offset] : It->second)
        Work.push_back(Source);
    const bool IsIndependentSource = B != 0 && Preds[B].empty();
    if (!Charge(1 + (IsIndependentSource && CxxContinuation
                         ? Low.CxxContinuationEntries.size()
                         : 0)))
      return Fail("frame propagation budget exhausted");
    if (IsHandlerSource(B)) {
      Sources.push_back(B);
      auto More = AddProtectedBlocks(Low.Blocks[B].StartAddr);
      if (!More)
        return More.takeError();
      if (!*More)
        return Fail("handler has no decoded protected scope");
    }
  }
  // No ordinary path brings a function temporary to a root the dispatcher
  // enters, so each such root must bind them before it reads one.
  if (!Low.FunctionTemporaries.empty()) {
    std::set<int> DispatcherRoots(std::next(Sources.begin()), Sources.end());
    DispatcherRoots.insert(HandlerId);
    for (int Root : DispatcherRoots) {
      const std::optional<bool> Binds = lowBlockBindsFunctionTemporaries(
          Low, Low.Blocks[Root], WorkRemaining);
      if (!Binds)
        return Fail("frame propagation budget exhausted");
      if (!*Binds)
        return Fail("handler reads a function temporary before binding it");
    }
  }
  std::vector<bool> Reachable(N, false);
  Work = Sources;
  // Source projection seeds only published ordinary entries. An unrelated
  // disconnected component has no entry-state contract and still refuses.
  if (Use == LowSEHFrameUse::SourceProjection)
    for (size_t B = 1; B < N; ++B) {
      if (!Charge())
        return Fail("frame propagation budget exhausted");
      if (Low.OrdinaryModuleAnalysisRoots.count(Low.Blocks[B].StartAddr))
        Work.push_back(static_cast<int>(B));
    }
  while (!Work.empty()) {
    if (!Charge())
      return Fail("frame propagation budget exhausted");
    int B = Work.back();
    Work.pop_back();
    if (Reachable[B])
      continue;
    Reachable[B] = true;
    Work.insert(Work.end(), Low.Blocks[B].Succs.begin(),
                Low.Blocks[B].Succs.end());
    Work.insert(Work.end(), LocalUnwindSuccessors[B].begin(),
                LocalUnwindSuccessors[B].end());
  }
  bool IndependentSP = false;
  for (size_t B = 0; B < N; ++B)
    if (Relevant[B] &&
        (!Reachable[B] ||
         (B != 0 &&
          Low.OrdinaryModuleAnalysisRoots.count(Low.Blocks[B].StartAddr) &&
          !LocalUnwindSources.count(static_cast<int>(B)) &&
          !(CxxContinuation && IsCxxContinuation(Low.Blocks[B].StartAddr))))) {
      if (!Reachable[B] || Use != LowSEHFrameUse::SourceProjection ||
          CxxContinuation || HasFrameRegister || IsLocalUnwindTarget)
        return Fail("protected scope has an independent ordinary entry at " +
                    std::to_string(Low.Blocks[B].StartAddr));
      IndependentSP = true;
    }
  // A source projection may retain the SP of the protected execution instead
  // of identifying it with this function's entry allocation. The prologue
  // still has to match the unwind record; no saved cell or local-unwind frame
  // argument receives a fact from this less precise coordinate.
  if (IndependentSP)
    Relevant[0] = true;

  for (size_t B = 0; B < N; ++B)
    if (ProtectedBlock[B])
      Result.ProtectedSources.push_back(static_cast<int>(B));
  Result.ProtectedCalls = Calls;

  // A synchronous edge leaves at its call, not at the end of the basic
  // block. Check the entire ordinary prefix, but an epilogue after the last
  // throwing call cannot invalidate the frame at that earlier call. A block
  // that leads to another relevant block still contributes all its effects.
  std::vector<va_t> CheckedEnd(N, InvalidVA);
  for (const auto &[B, Sites] : Calls) {
    if (llvm::any_of(Low.Blocks[B].Succs, [&](int S) { return Relevant[S]; }))
      continue;
    const va_t Last = *Sites.rbegin();
    for (const auto &Insn : Low.Blocks[B].InstructionBoundaries)
      if (Insn.Address == Last)
        CheckedEnd[B] = Insn.Address + Insn.Size;
  }
  // An SEH scope guards the instructions that start inside its range, such
  // as the one at a protected call's return address that clang-cl includes.
  // A fault after the range belongs to no handler of this scope, so the
  // block's later epilogue is likewise left unchecked.
  for (size_t B = 0; B < N; ++B) {
    if (!GuardedEnd[B] || CheckedEnd[B] != InvalidVA ||
        llvm::any_of(Low.Blocks[B].Succs, [&](int S) { return Relevant[S]; }))
      continue;
    if (!Charge(Low.Blocks[B].InstructionBoundaries.size()))
      return Fail("frame propagation budget exhausted");
    va_t Last = InvalidVA;
    for (const auto &Insn : Low.Blocks[B].InstructionBoundaries)
      if (Insn.Address < GuardedEnd[B])
        Last = Insn.Address + Insn.Size;
    if (Last != InvalidVA && Last < Low.Blocks[B].EndAddr)
      CheckedEnd[B] = Last;
  }

  // X64 CodeOffset is the byte offset of the prologue instruction end,
  // not an index in the native unwind slot array.
  std::map<uint32_t, uint64_t> ExpectedAdjustments;
  std::optional<uint32_t> SetFramePointerOffset;
  uint32_t PreviousOffset = EH.PrologueSize;
  uint64_t FrameBytes = 0;
  // SP moves outside the protected range after a frame-register prologue.
  bool DynamicSP = IndependentSP;
  for (const UnwindOperation &Op : EH.UnwindOperations) {
    if (!Charge())
      return Fail("frame propagation budget exhausted");
    // A version 2 epilog descriptor locates an epilog for the unwinder. It
    // is not a prologue action and its offset is not a prologue position;
    // the decoded protected blocks below still prove SP stable.
    if (UnwindV2 && Op.Kind == UnwindOperationKind::Epilog)
      continue;
    if (!Op.CodeOffset || Op.CodeOffset > PreviousOffset)
      return Fail("invalid unwind instruction offset");
    PreviousOffset = Op.CodeOffset;
    uint64_t Bytes = 0;
    switch (Op.Kind) {
    case UnwindOperationKind::PushNonVolatile:
      Bytes = 8;
      break;
    case UnwindOperationKind::AllocateSmall:
    case UnwindOperationKind::AllocateLarge:
      Bytes = Op.StackOffset;
      if (!Bytes || Bytes % 8)
        return Fail("invalid fixed allocation");
      break;
    case UnwindOperationKind::SaveNonVolatile:
    case UnwindOperationKind::SaveNonVolatileFar:
    case UnwindOperationKind::SaveXMM128:
    case UnwindOperationKind::SaveXMM128Far:
      break;
    case UnwindOperationKind::SetFramePointer:
      if (!HasFrameRegister || SetFramePointerOffset)
        return Fail("frame register disagrees with UWOP_SET_FPREG");
      SetFramePointerOffset = Op.CodeOffset;
      break;
    default:
      return Fail("unwind action has no certified fixed SP effect");
    }
    if (Bytes) {
      if (!ExpectedAdjustments.emplace(Op.CodeOffset, Bytes).second ||
          Bytes > uint64_t(std::numeric_limits<int64_t>::max()) - FrameBytes)
        return Fail("conflicting or overflowing allocation");
      FrameBytes += Bytes;
    }
  }

  if (HasFrameRegister && !SetFramePointerOffset)
    return Fail("frame register disagrees with UWOP_SET_FPREG");

  auto Overlaps = [](const NdVar &V, uint64_t Reg) {
    return V.isReg() && V.Size && V.Offset < Reg + 8 && V.Offset + V.Size > Reg;
  };
  auto OverlapsSP = [&](const NdVar &V) {
    return Overlaps(V, TRI.StackPointer);
  };
  // Source calls do not explicitly read the physical return-slot address.
  // They still require a separate range proof before frame loads can be
  // projected. Keep actual handler SP reads unsupported when the fault can
  // leave more than one SP, including a return before restoring the frame.
  auto HandlerRestoresSPBeforeReading = [&]() -> std::optional<bool> {
    if (!Charge(N))
      return std::nullopt;
    std::vector<bool> Seen(N, false);
    std::vector<int> Pending{HandlerId};
    while (!Pending.empty()) {
      if (!Charge())
        return std::nullopt;
      const int B = Pending.back();
      Pending.pop_back();
      if (Seen[B])
        continue;
      Seen[B] = true;
      bool Restored = false;
      for (const LowOp &Op : Low.Blocks[B].Ops) {
        if (!Charge(Op.NumInputs + 1))
          return std::nullopt;
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          if (OverlapsSP(Op.Inputs[I]))
            return false;
        if (Op.Opcode == NdOp::RETURN)
          return false;
        if (Op.Output == NdVar::reg(TRI.StackPointer, TRI.PointerSize)) {
          Restored = true;
          break;
        }
      }
      if (!Restored)
        Pending.insert(Pending.end(), Low.Blocks[B].Succs.begin(),
                       Low.Blocks[B].Succs.end());
    }
    return true;
  };
  // `lea fp, [rsp + FrameOffset]` (`mov fp, rsp` for a zero offset): one
  // full-width copy of a value the same instruction computes as SP plus that
  // constant.
  auto SetsFramePointer = [&](const LowBlock &Block,
                              const LowInstructionBoundary &Boundary,
                              size_t Index) {
    const LowOp &Op = Block.Ops[Index];
    if (Op.Opcode != NdOp::COPY || Op.Output != NdVar::reg(FrameRegister, 8) ||
        Op.NumInputs != 1)
      return false;
    const NdVar SP = NdVar::reg(TRI.StackPointer, 8);
    std::map<uint64_t, std::optional<uint64_t>> TempDisplacement;
    auto displacement = [&](const NdVar &V) -> std::optional<uint64_t> {
      if (V == SP)
        return 0;
      if (!V.isTemp() || V.Size != 8)
        return std::nullopt;
      auto It = TempDisplacement.find(V.Offset);
      return It == TempDisplacement.end() ? std::nullopt : It->second;
    };
    for (size_t I = Boundary.FirstOp; I < Index; ++I) {
      const LowOp &Def = Block.Ops[I];
      if (!Def.Output.isTemp())
        continue;
      std::optional<uint64_t> Value;
      if (Def.Output.Size == 8 && Def.Opcode == NdOp::COPY &&
          Def.NumInputs == 1) {
        Value = displacement(Def.Inputs[0]);
      } else if (Def.Output.Size == 8 &&
                 (Def.Opcode == NdOp::INT_ADD || Def.Opcode == NdOp::INT_SUB) &&
                 Def.NumInputs == 2 && Def.Inputs[1].isConst() &&
                 Def.Inputs[1].Size == 8) {
        if (auto Base = displacement(Def.Inputs[0]))
          Value = Def.Opcode == NdOp::INT_ADD ? *Base + Def.Inputs[1].Offset
                                              : *Base - Def.Inputs[1].Offset;
      }
      TempDisplacement[Def.Output.Offset] = Value;
    }
    const std::optional<uint64_t> Displacement = displacement(Op.Inputs[0]);
    return Displacement && *Displacement == EH.FrameOffset;
  };
  unsigned FramePointerWrites = 0;
  std::map<uint32_t, uint64_t> ActualAdjustments;
  std::map<va_t, uint64_t> DecodedAdjustments;
  for (size_t B = 0; B < N; ++B) {
    if (!Relevant[B])
      continue;
    const LowBlock &Block = Low.Blocks[B];
    if (!Charge(Block.Ops.size() + Block.InstructionBoundaries.size() + 1))
      return Fail("frame propagation budget exhausted");
    if (auto Error = validateLowInstructionBoundaries(
            Block, LowInstructionBoundaryRequirement::Required)) {
      llvm::consumeError(std::move(Error));
      return Fail("missing or invalid instruction provenance");
    }
    for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries) {
      if (Boundary.Address >= CheckedEnd[B])
        continue;
      for (size_t I = Boundary.FirstOp; I < Boundary.FirstOp + Boundary.OpCount;
           ++I) {
        const LowOp &Op = Block.Ops[I];
        if (HasFrameRegister && Overlaps(Op.Output, FrameRegister)) {
          const uint64_t End = Boundary.Address + Boundary.Size;
          if (!Charge(I - Boundary.FirstOp + 1))
            return Fail("frame propagation budget exhausted");
          if (B != 0 || End > PrologueEnd ||
              End - Low.Entry != *SetFramePointerOffset ||
              !SetsFramePointer(Block, Boundary, I))
            return Fail("frame register is not set once by the prologue");
          ++FramePointerWrites;
          continue;
        }
        if (!OverlapsSP(Op.Output))
          continue;
        const bool AfterPrologue =
            B != 0 || Boundary.Address + Boundary.Size > PrologueEnd;
        if (HasFrameRegister && AfterPrologue &&
            (ProtectedBlock[B] || OrdinaryEntry) &&
            Use == LowSEHFrameUse::SourceProjection &&
            !Result.HasVaryingEntrySP) {
          const auto Unobserved = HandlerRestoresSPBeforeReading();
          if (!Unobserved)
            return Fail("frame propagation budget exhausted");
          Result.HasVaryingEntrySP = *Unobserved;
        }
        if (HasFrameRegister && AfterPrologue &&
            (!ProtectedBlock[B] || Result.HasVaryingEntrySP)) {
          DynamicSP = true;
          continue;
        }
        if (B != 0 || Boundary.Address + Boundary.Size > PrologueEnd ||
            Op.Opcode != NdOp::INT_SUB || Op.Output.Size != 8 ||
            Op.Output.Offset != TRI.StackPointer || Op.NumInputs != 2 ||
            Op.Inputs[0] != NdVar::reg(TRI.StackPointer, 8) ||
            !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 8)
          return Fail(
              "SP is not stable from prologue through protected scope at " +
              std::to_string(Boundary.Address));
        const uint32_t Offset =
            static_cast<uint32_t>(Boundary.Address + Boundary.Size - Low.Entry);
        if (!ActualAdjustments.emplace(Offset, Op.Inputs[1].Offset).second)
          return Fail("multiple SP definitions at an unwind instruction");
        DecodedAdjustments.emplace(Boundary.Address, Op.Inputs[1].Offset);
      }
    }
  }
  if (HasFrameRegister && FramePointerWrites != 1)
    return Fail("frame register is not set once by the prologue");
  if (ExpectedAdjustments != ActualAdjustments)
    return Fail("decoded prologue disagrees with unwind allocation");
  for (const auto &[Target, Sources] : LocalUnwindSources)
    if (Relevant[Target] || (IsLocalUnwindTarget && Target == HandlerId)) {
      Result.HasRelevantLocalUnwind = true;
      for (const auto &[Source, Offset] : Sources)
        if (DynamicSP || !Relevant[Source] || !Reachable[Source] ||
            Offset != -static_cast<int64_t>(FrameBytes))
          return Fail("local-unwind call does not carry the protected frame");
    }
  // Ordinary flow into a shared handler is not proven to bring the SP the
  // dispatcher resumes with once SP moves on the way in.
  if (DynamicSP && OrdinaryEntry && !IndependentSP && !Result.HasVaryingEntrySP)
    return Fail("SP moves before a handler that ordinary flow also enters");
  Result.FrameBytes = FrameBytes;
  Result.DynamicSP = DynamicSP;
  Result.HasIndependentEntries = IndependentSP;
  Result.RelevantBlocks = std::move(Relevant);
  Result.ProtectedBlocks = std::move(ProtectedBlock);
  Result.CheckedEnd = std::move(CheckedEnd);
  Result.DecodedAdjustments = std::move(DecodedAdjustments);
  return Result;
}

llvm::Expected<LowSEHEstablisherFrame>
proveLowSEHEstablisherFrame(const LowFunc &Low, va_t Handler,
                            const TargetRegInfo &TRI, bool OrdinaryEntry,
                            size_t &WorkRemaining,
                            llvm::StringRef LocalUnwindDigest,
                            bool CxxContinuation, LowSEHFrameUse Use) {
  std::optional<FrameCFG> CFG;
  return proveInFrameCFG(Low, Handler, TRI, OrdinaryEntry, WorkRemaining,
                         LocalUnwindDigest, CxxContinuation, CFG, Use);
}

std::map<va_t, LowSEHEstablisherFrame>
proveLowSEHHandlerFrames(const LowFunc &Low, llvm::ArrayRef<va_t> Handlers,
                         const TargetRegInfo &TRI, size_t &WorkRemaining,
                         llvm::StringRef LocalUnwindDigest) {
  std::optional<FrameCFG> CFG;
  std::map<va_t, LowSEHEstablisherFrame> Results;
  for (va_t Handler : Handlers) {
    if (Low.Blocks.size() > WorkRemaining) {
      WorkRemaining = 0;
      return {};
    }
    WorkRemaining -= Low.Blocks.size();
    const auto Block = llvm::find_if(
        Low.Blocks, [&](const LowBlock &B) { return B.StartAddr == Handler; });
    if (Block == Low.Blocks.end())
      continue;
    auto Frame = proveInFrameCFG(Low, Handler, TRI, !Block->Preds.empty(),
                                 WorkRemaining, LocalUnwindDigest, false, CFG,
                                 LowSEHFrameUse::InvocationIdentity);
    if (!Frame) {
      llvm::consumeError(Frame.takeError());
      if (!WorkRemaining)
        return {};
      continue;
    }
    Results.emplace(Handler, std::move(*Frame));
  }
  return Results;
}

} // namespace neverd
