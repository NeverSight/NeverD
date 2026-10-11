//===- MedSSA.cpp - SSA construction for MedIR -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSA construction: live-in analysis, dominance computation
/// (Cooper-Harvey-Kennedy), dominance frontier calculation, phi insertion,
/// and iterative SSA renaming over the dominator tree.
///
//===----------------------------------------------------------------------===//

#include "../X86/RegistrationRoots.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/SEHFrameProof.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>

#define DEBUG_TYPE "neverd-med-ssa"

namespace neverd {

namespace {

/// Windows resumes an in-function __except body with its establisher SP, not
/// with the entry SP used for an independent ordinary machine-code root.
/// Certify the fixed-frame case against both normalized unwind actions and the
/// decoded instructions. FrameSize is storage sizing, not this proof.  A frame
/// register leaves that SP unchanged: the unwinder restores it as a
/// nonvolatile, and it is certified only as UWOP_SET_FPREG describes it.
/// A frame-register function may also move SP after its prologue (alloca)
/// outside the protected range; its frame is then established from the
/// frame register, and the handler resumes with the SP the range holds,
/// which no protected block may write.  That case returns nullopt.
std::optional<uint64_t> proveSEHEstablisherFrame(
    const LowFunc &Low, const MedFunc &Med, va_t Handler,
    const TargetRegInfo &TRI, bool OrdinaryEntry,
    llvm::StringRef LocalUnwindDigest = {}, bool CxxContinuation = false,
    std::vector<int> *ProtectedSources = nullptr,
    std::map<int, std::set<va_t>> *ProtectedCalls = nullptr) {
  auto Fail = [](const std::string &Why) -> void {
    throw LowToMedConversionError(
        std::string("Windows SEH establisher frame: ") + Why);
  };
  const ExceptionFunction &EH = *Low.ExceptionMetadata;
  auto IsCxxContinuation = [&](va_t VA) {
    return llvm::any_of(Low.CxxContinuationEntries,
                        [&](const auto &Entry) { return Entry.Target == VA; });
  };
  if (CxxContinuation &&
      (!EH.Cxx || !EH.Cxx->hasValidStateGraph() ||
       EH.Cxx->HasDynamicStackAlignment || !IsCxxContinuation(Handler)))
    Fail("unsupported C++ continuation frame contract");
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
    Fail("unsupported unwind or frame-register contract");
  if (Low.Blocks.empty() || Low.Blocks.front().StartAddr != Low.Entry ||
      EH.PrologueSize > EH.CodeRange.End - Low.Entry)
    Fail("missing decoded prologue");
  const va_t PrologueEnd = Low.Entry + EH.PrologueSize;
  if (Low.Blocks.front().EndAddr < PrologueEnd)
    Fail("nonlinear prologue is not certified");

  const size_t N = Low.Blocks.size();
  const bool SynchronousCxx = CxxContinuation && EH.Cxx->IsSynchronous;
  std::map<int, std::set<va_t>> Calls;
  std::vector<std::vector<int>> Preds(N);
  int HandlerId = -1;
  for (size_t B = 0; B < N; ++B) {
    if (Low.Blocks[B].StartAddr == Handler)
      HandlerId = static_cast<int>(B);
    for (int S : Low.Blocks[B].Succs) {
      if (S < 0 || static_cast<size_t>(S) >= N)
        Fail("invalid ordinary CFG");
      Preds[S].push_back(static_cast<int>(B));
    }
  }
  if (!Preds[0].empty())
    Fail("ordinary control flow re-enters the prologue");
  if (HandlerId <= 0 || (!OrdinaryEntry && !Preds[HandlerId].empty()))
    Fail("handler also has an ordinary entry role");

  std::map<int, std::vector<std::pair<int, int64_t>>> LocalUnwindSources;
  std::vector<std::vector<int>> LocalUnwindSuccessors(N);
  if (!CxxContinuation)
    for (const auto &Entry : Low.SEHLocalUnwindContinuations) {
      int Target = -1, Source = -1;
      for (size_t B = 0; B < N; ++B) {
        if (Low.Blocks[B].StartAddr == Entry.Target)
          Target = static_cast<int>(B);
        for (size_t I = 0; I < Low.Blocks[B].Ops.size(); ++I) {
          const LowOp &Op = Low.Blocks[B].Ops[I];
          if (Op.Addr == Entry.CallAddr && Op.Seq == Entry.CallSeq &&
              Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
              Op.Inputs[0].isConst() && Op.Inputs[0].Size == 8 &&
              Op.Inputs[0].Offset == Entry.Callee) {
            if (Source >= 0 || Entry.OperationDigest.empty() ||
                Entry.OperationDigest != LocalUnwindDigest)
              Fail("stale local-unwind argument proof");
            Source = static_cast<int>(B);
          }
        }
      }
      if (Target <= 0 || Source < 0 || Entry.FrameOffset >= 0)
        Fail("stale local-unwind continuation certificate");
      LocalUnwindSources[Target].emplace_back(Source, Entry.FrameOffset);
      LocalUnwindSuccessors[Source].push_back(Target);
    }
  const bool IsLocalUnwindTarget = LocalUnwindSources.count(HandlerId);
  if (!CxxContinuation && Low.OrdinaryModuleAnalysisRoots.count(Handler) &&
      !IsLocalUnwindTarget)
    Fail("handler also has an ordinary entry role");

  // Every ordinary predecessor path into a protected block participates. A
  // stack adjustment before the guarded interval matters just as much as one
  // inside it. Independent sources cannot inherit the entry prologue proof.
  // An __except body of this function is the one other source: the unwinder
  // resumes it with the SP of its own protected scope, so when its flow
  // reaches this scope, that scope joins the certificate as well.
  std::vector<bool> Relevant(N, false);
  std::vector<bool> ProtectedBlock(N, false);
  std::vector<int> Work;
  auto AddProtectedBlocks = [&](va_t ScopeHandler) {
    const size_t Before = Work.size();
    if (CxxContinuation) {
      // CFGBuilder owns the state-to-protected-block relation. For a catch
      // nested inside another catch, follow that catch's containing try to
      // reach the parent's protected frame, rather than borrowing the nested
      // funclet's own prologue.
      const auto &Tries = EH.Cxx->TryBlocks;
      std::set<size_t> Regions;
      for (const auto &Entry : Low.CxxContinuationEntries) {
        if (Entry.Target != ScopeHandler)
          continue;
        for (size_t I = 0; I < Tries.size(); ++I)
          if (llvm::any_of(Tries[I].Handlers, [&](const auto &Catch) {
                return Catch.HandlerVA == Entry.SourceEntry;
              }))
            Regions.insert(I);
      }
      bool Changed = true;
      while (Changed) {
        Changed = false;
        const auto Current = Regions;
        for (size_t I : Current)
          for (size_t J = 0; J < Tries.size(); ++J)
            if (Tries[I].TryLow > Tries[J].TryHigh &&
                Tries[I].CatchHigh <= Tries[J].CatchHigh)
              Changed |= Regions.insert(J).second;
      }
      for (size_t B = 0; B < N; ++B) {
        bool HasEdge = false;
        for (const ExceptionalEdge &Edge : Low.Blocks[B].ExceptionalSuccs) {
          if (Edge.Kind != ExceptionalEdgeKind::CxxCatch ||
              !Regions.count(Edge.RegionIndex))
            continue;
          if (SynchronousCxx) {
            const auto &Insns = Low.Blocks[B].InstructionBoundaries;
            const auto Call = llvm::find_if(Insns, [&](const auto &Insn) {
              return Insn.Address == Edge.SourceVA &&
                     Insn.Control == LowInstructionControl::Call;
            });
            if (Call == Insns.end())
              Fail("synchronous catch lacks a decoded throwing call");
            if (Call->Address < PrologueEnd)
              Fail("protected call overlaps an incomplete prologue");
            Calls[static_cast<int>(B)].insert(Call->Address);
          }
          HasEdge = true;
        }
        if (HasEdge) {
          if (!SynchronousCxx && Low.Blocks[B].StartAddr < PrologueEnd &&
              Low.Blocks[B].EndAddr > Low.Entry)
            Fail("protected scope overlaps an incomplete prologue");
          Work.push_back(static_cast<int>(B));
          ProtectedBlock[B] = true;
        }
      }
      return Work.size() != Before;
    }
    for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
      if (Scope.HandlerVA != ScopeHandler)
        continue;
      // A cold fragment can sit below the entry; only the prologue itself
      // is out of bounds.
      auto Range = getSemanticSEHGuardedRange(Scope, Arch::X64, EH);
      if (Scope.ParseStatus != ExceptionParseStatus::Complete || !Range ||
          (Range->Begin < PrologueEnd && Range->End > Low.Entry))
        Fail("protected scope overlaps an incomplete prologue");
      for (size_t B = 0; B < N; ++B)
        if (Low.Blocks[B].StartAddr < Range->End &&
            Low.Blocks[B].EndAddr > Range->Begin) {
          Work.push_back(static_cast<int>(B));
          ProtectedBlock[B] = true;
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
           llvm::any_of(EH.SEH->Scopes, [&](const SEHScopeRecord &Scope) {
             return Scope.HandlerVA == Start;
           });
  };
  if (!AddProtectedBlocks(Handler) && !IsLocalUnwindTarget)
    Fail("handler has no decoded protected scope");
  if (IsLocalUnwindTarget)
    for (const auto &[Source, Offset] : LocalUnwindSources.at(HandlerId))
      Work.push_back(Source);
  // Ordinary flow into a shared handler must keep the same frame.
  if (OrdinaryEntry)
    Work.insert(Work.end(), Preds[HandlerId].begin(), Preds[HandlerId].end());
  std::vector<int> Sources{0};
  while (!Work.empty()) {
    const int B = Work.back();
    Work.pop_back();
    if (Relevant[B])
      continue;
    Relevant[B] = true;
    Work.insert(Work.end(), Preds[B].begin(), Preds[B].end());
    if (auto It = LocalUnwindSources.find(B); It != LocalUnwindSources.end())
      for (const auto &[Source, Offset] : It->second)
        Work.push_back(Source);
    if (IsHandlerSource(B)) {
      Sources.push_back(B);
      if (!AddProtectedBlocks(Low.Blocks[B].StartAddr))
        Fail("handler has no decoded protected scope");
    }
  }
  std::vector<bool> Reachable(N, false);
  Work = Sources;
  while (!Work.empty()) {
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
  for (size_t B = 0; B < N; ++B)
    if (Relevant[B] &&
        (!Reachable[B] ||
         (B != 0 &&
          Low.OrdinaryModuleAnalysisRoots.count(Low.Blocks[B].StartAddr) &&
          !LocalUnwindSources.count(static_cast<int>(B)) &&
          !(CxxContinuation && IsCxxContinuation(Low.Blocks[B].StartAddr)))))
      Fail("protected scope has an independent ordinary entry at " +
           std::to_string(Low.Blocks[B].StartAddr));

  if (ProtectedSources)
    for (size_t B = 0; B < N; ++B)
      if (ProtectedBlock[B])
        ProtectedSources->push_back(static_cast<int>(B));
  if (ProtectedCalls)
    *ProtectedCalls = Calls;

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

  // X64 CodeOffset is the byte offset of the prologue instruction end,
  // not an index in the native unwind slot array.
  std::map<uint32_t, uint64_t> ExpectedAdjustments;
  std::optional<uint32_t> SetFramePointerOffset;
  uint32_t PreviousOffset = EH.PrologueSize;
  uint64_t FrameBytes = 0;
  // SP moves outside the protected range after a frame-register prologue.
  bool DynamicSP = false;
  for (const UnwindOperation &Op : EH.UnwindOperations) {
    // A version 2 epilog descriptor locates an epilog for the unwinder. It
    // is not a prologue action and its offset is not a prologue position;
    // the decoded protected blocks below still prove SP stable.
    if (UnwindV2 && Op.Kind == UnwindOperationKind::Epilog)
      continue;
    if (!Op.CodeOffset || Op.CodeOffset > PreviousOffset)
      Fail("invalid unwind instruction offset");
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
        Fail("invalid fixed allocation");
      break;
    case UnwindOperationKind::SaveNonVolatile:
    case UnwindOperationKind::SaveNonVolatileFar:
    case UnwindOperationKind::SaveXMM128:
    case UnwindOperationKind::SaveXMM128Far:
      break;
    case UnwindOperationKind::SetFramePointer:
      if (!HasFrameRegister || SetFramePointerOffset)
        Fail("frame register disagrees with UWOP_SET_FPREG");
      SetFramePointerOffset = Op.CodeOffset;
      break;
    default:
      Fail("unwind action has no certified fixed SP effect");
    }
    if (Bytes) {
      if (!ExpectedAdjustments.emplace(Op.CodeOffset, Bytes).second ||
          Bytes > uint64_t(std::numeric_limits<int64_t>::max()) - FrameBytes)
        Fail("conflicting or overflowing allocation");
      FrameBytes += Bytes;
    }
  }

  if (HasFrameRegister && !SetFramePointerOffset)
    Fail("frame register disagrees with UWOP_SET_FPREG");

  auto Overlaps = [](const NdVar &V, uint64_t Reg) {
    return V.isReg() && V.Size && V.Offset < Reg + 8 && V.Offset + V.Size > Reg;
  };
  auto OverlapsSP = [&](const NdVar &V) {
    return Overlaps(V, TRI.StackPointer);
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
    if (auto Error = validateLowInstructionBoundaries(
            Block, LowInstructionBoundaryRequirement::Required)) {
      llvm::consumeError(std::move(Error));
      Fail("missing or invalid instruction provenance");
    }
    for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries) {
      if (Boundary.Address >= CheckedEnd[B])
        continue;
      for (size_t I = Boundary.FirstOp; I < Boundary.FirstOp + Boundary.OpCount;
           ++I) {
        const LowOp &Op = Block.Ops[I];
        if (HasFrameRegister && Overlaps(Op.Output, FrameRegister)) {
          const uint64_t End = Boundary.Address + Boundary.Size;
          if (B != 0 || End > PrologueEnd ||
              End - Low.Entry != *SetFramePointerOffset ||
              !SetsFramePointer(Block, Boundary, I))
            Fail("frame register is not set once by the prologue");
          ++FramePointerWrites;
          continue;
        }
        if (!OverlapsSP(Op.Output))
          continue;
        const bool AfterPrologue =
            B != 0 || Boundary.Address + Boundary.Size > PrologueEnd;
        if (HasFrameRegister && AfterPrologue && !ProtectedBlock[B]) {
          DynamicSP = true;
          continue;
        }
        if (B != 0 || Boundary.Address + Boundary.Size > PrologueEnd ||
            Op.Opcode != NdOp::INT_SUB || Op.Output.Size != 8 ||
            Op.Output.Offset != TRI.StackPointer || Op.NumInputs != 2 ||
            Op.Inputs[0] != NdVar::reg(TRI.StackPointer, 8) ||
            !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 8)
          Fail("SP is not stable from prologue through protected scope at " +
               std::to_string(Boundary.Address));
        const uint32_t Offset =
            static_cast<uint32_t>(Boundary.Address + Boundary.Size - Low.Entry);
        if (!ActualAdjustments.emplace(Offset, Op.Inputs[1].Offset).second)
          Fail("multiple SP definitions at an unwind instruction");
        DecodedAdjustments.emplace(Boundary.Address, Op.Inputs[1].Offset);
      }
    }
  }
  // Low->Med may add ABI effects (for example a known callee's ret-pop).
  // Such effects are absent from decoded LowOps and must not bypass the SP
  // stability certificate, whether in the prologue or a protected prefix.
  std::map<va_t, uint64_t> ConvertedAdjustments;
  for (const MedBlock &Block : Med.Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (Op.Output.Kind != MedVar::Reg ||
          Op.Output.RegOff != TRI.StackPointer || !Op.Output.Size)
        continue;
      for (size_t B = 0; B < N; ++B) {
        if (!Relevant[B] || Op.Addr >= CheckedEnd[B] ||
            Op.Addr < Low.Blocks[B].StartAddr ||
            Op.Addr >= Low.Blocks[B].EndAddr)
          continue;
        if (HasFrameRegister && !ProtectedBlock[B] &&
            (B != 0 || Op.Addr >= PrologueEnd)) {
          DynamicSP = true;
          continue;
        }
        if (Op.Addr >= PrologueEnd || Op.Opcode != NdOp::INT_SUB ||
            Op.Output.Size != 8 || Op.NumInputs != 2 ||
            Op.Inputs[0].Kind != MedVar::Reg ||
            Op.Inputs[0].RegOff != TRI.StackPointer || Op.Inputs[0].Size != 8 ||
            !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 8 ||
            !ConvertedAdjustments.emplace(Op.Addr, Op.Inputs[1].ConstVal)
                 .second)
          Fail("converted ABI effect changes the protected SP");
      }
    }
  if (HasFrameRegister && FramePointerWrites != 1)
    Fail("frame register is not set once by the prologue");
  if (ConvertedAdjustments != DecodedAdjustments)
    Fail("converted prologue disagrees with decoded SP effects");
  if (ExpectedAdjustments != ActualAdjustments)
    Fail("decoded prologue disagrees with unwind allocation");
  for (const auto &[Target, Sources] : LocalUnwindSources)
    if (Relevant[Target] || (IsLocalUnwindTarget && Target == HandlerId))
      for (const auto &[Source, Offset] : Sources)
        if (DynamicSP || !Relevant[Source] || !Reachable[Source] ||
            Offset != -static_cast<int64_t>(FrameBytes))
          Fail("local-unwind call does not carry the protected frame");
  // Ordinary flow into a shared handler is not proven to bring the SP the
  // dispatcher resumes with once SP moves on the way in.
  if (DynamicSP && OrdinaryEntry)
    Fail("SP moves before a handler that ordinary flow also enters");
  if (DynamicSP)
    return std::nullopt;
  return FrameBytes;
}

} // namespace

void LowToMedConverter::buildSsa(MedFunc &Func, const LowFunc &Low) {
  if (Func.Blocks.empty())
    return;
  Func.CallClobbers.clear();

  int N = static_cast<int>(Func.Blocks.size());
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const bool UseItaniumExceptionalFlow =
      Func.ExceptionMetadata && Func.ExceptionMetadata->Itanium &&
      Func.ExceptionMetadata->Itanium->IsCallSiteAddressForm;

  // The machine exceptional edge carries the current frame state into a
  // landing pad just as surely as an ordinary branch carries SSA values.  Keep
  // that edge in the SSA-only graph while leaving MedBlock::Succs untouched:
  // the LLVM emitter will turn the protected call into an invoke later.
  // A real machine function may have several independently enterable code
  // roots which later converge.  Model those sources under one synthetic
  // entry for dominance only.  Treating each DFS tree as an unrelated
  // component loses an edge when an earlier source has already visited the
  // shared join, which in turn suppresses the PHI required at that join.
  const int VirtualRoot = N;
  const int GraphN = N + 1;
  std::vector<std::vector<int>> FlowSuccs(GraphN), FlowPreds(GraphN);
  std::vector<bool> IsExceptionalTarget(N, false);
  auto AddFlowEdge = [&](int From, int To) {
    if (From < 0 || From >= GraphN || To < 0 || To >= GraphN)
      return;
    auto &Succs = FlowSuccs[From];
    if (std::find(Succs.begin(), Succs.end(), To) == Succs.end())
      Succs.push_back(To);
  };
  for (int B = 0; B < N; ++B) {
    for (int S : Func.Blocks[B].Succs)
      AddFlowEdge(B, S);
    if (UseItaniumExceptionalFlow)
      for (const ExceptionalEdge &Edge : Func.Blocks[B].ExceptionalSuccs)
        if (Edge.BlockId >= 0 && Edge.BlockId < N) {
          AddFlowEdge(B, Edge.BlockId);
          IsExceptionalTarget[Edge.BlockId] = true;
        }
  }
  for (int B = 0; B < N; ++B)
    for (int S : FlowSuccs[B])
      FlowPreds[S].push_back(B);

  // Root one representative of every source SCC in the real CFG, plus the
  // architectural function entry even if malformed input gives it an incoming
  // edge.  Choosing the first merely-unreached block is order-dependent: a
  // downstream SCC can have a smaller block id than its true source and would
  // then acquire a fictitious independent-entry role.
  std::vector<int> FinishOrder;
  std::vector<bool> VisitedForSCC(N, false);
  for (int Start = 0; Start < N; ++Start) {
    if (VisitedForSCC[Start])
      continue;
    std::vector<std::pair<int, size_t>> Worklist;
    VisitedForSCC[Start] = true;
    Worklist.push_back({Start, 0});
    while (!Worklist.empty()) {
      const int B = Worklist.back().first;
      size_t &Next = Worklist.back().second;
      if (Next < FlowSuccs[B].size()) {
        const int S = FlowSuccs[B][Next++];
        if (S >= 0 && S < N && !VisitedForSCC[S]) {
          VisitedForSCC[S] = true;
          Worklist.push_back({S, 0});
        }
        continue;
      }
      FinishOrder.push_back(B);
      Worklist.pop_back();
    }
  }

  std::vector<int> SccOf(N, -1);
  std::vector<std::vector<int>> SccBlocks;
  for (auto It = FinishOrder.rbegin(); It != FinishOrder.rend(); ++It) {
    const int Start = *It;
    if (SccOf[Start] != -1)
      continue;
    const int Scc = static_cast<int>(SccBlocks.size());
    SccBlocks.emplace_back();
    std::vector<int> Worklist{Start};
    SccOf[Start] = Scc;
    while (!Worklist.empty()) {
      const int B = Worklist.back();
      Worklist.pop_back();
      SccBlocks[Scc].push_back(B);
      for (int P : FlowPreds[B])
        if (P >= 0 && P < N && SccOf[P] == -1) {
          SccOf[P] = Scc;
          Worklist.push_back(P);
        }
    }
  }

  std::vector<bool> SccHasIncoming(SccBlocks.size(), false);
  for (int B = 0; B < N; ++B)
    for (int S : FlowSuccs[B])
      if (S >= 0 && S < N && SccOf[B] != SccOf[S])
        SccHasIncoming[SccOf[S]] = true;

  std::vector<int> Roots;
  // Variables some root other than the entry reads on arrival.
  std::set<int> NonEntryRootLiveIns;
  std::vector<bool> IsRoot(N, false);
  auto AddRoot = [&](int Root) {
    if (Root < 0 || Root >= N || IsRoot[Root])
      return;
    IsRoot[Root] = true;
    Roots.push_back(Root);
  };
  AddRoot(0);
  for (size_t Scc = 0; Scc < SccBlocks.size(); ++Scc) {
    if (SccHasIncoming[Scc] || SccBlocks[Scc].empty())
      continue;
    AddRoot(*std::min_element(SccBlocks[Scc].begin(), SccBlocks[Scc].end()));
  }

  for (int Root : Roots)
    AddFlowEdge(VirtualRoot, Root);
  for (auto &Preds : FlowPreds)
    Preds.clear();
  for (int B = 0; B < GraphN; ++B)
    for (int S : FlowSuccs[B])
      FlowPreds[S].push_back(B);

  // Reverse post-order + immediate dominators (Cooper-Harvey-Kennedy), computed
  // up front because both the live-in analysis (Step 0) and the dominance
  // frontiers (Step 2) need them.
  std::vector<int> RPO;
  std::vector<int> RPONum(GraphN, -1);
  {
    std::vector<bool> Visited(GraphN, false);
    std::vector<std::pair<int, size_t>> Stk;
    Stk.reserve(GraphN);
    Visited[VirtualRoot] = true;
    Stk.push_back({VirtualRoot, 0});
    while (!Stk.empty()) {
      int B = Stk.back().first;
      size_t I = Stk.back().second;
      auto &Succs = FlowSuccs[B];
      if (I < Succs.size()) {
        Stk.back().second = I + 1;
        int S = Succs[I];
        if (S < 0 || S >= GraphN)
          continue;
        if (!Visited[S]) {
          Visited[S] = true;
          Stk.push_back({S, 0});
        }
      } else {
        RPO.push_back(B);
        Stk.pop_back();
      }
    }
    std::reverse(RPO.begin(), RPO.end());

    for (int I = 0; I < static_cast<int>(RPO.size()); ++I)
      RPONum[RPO[I]] = I;
  }

  std::vector<int> IDom(GraphN, -1);
  IDom[VirtualRoot] = VirtualRoot;

  auto Intersect = [&](int B1, int B2) -> int {
    int F1 = B1, F2 = B2;
    while (F1 != F2) {
      while (RPONum[F1] > RPONum[F2])
        F1 = IDom[F1];
      while (RPONum[F2] > RPONum[F1])
        F2 = IDom[F2];
    }
    return F1;
  };

  {
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (int B : RPO) {
        if (B == VirtualRoot)
          continue;
        int NewIDom = -1;
        for (int P : FlowPreds[B]) {
          if (P < 0 || P >= GraphN)
            continue;
          if (IDom[P] == -1)
            continue;
          if (NewIDom == -1)
            NewIDom = P;
          else
            NewIDom = Intersect(NewIDom, P);
        }
        if (NewIDom != -1 && IDom[B] != NewIDom) {
          IDom[B] = NewIDom;
          Changed = true;
        }
      }
    }
  }

  std::map<uint64_t, std::set<int>> RegOffToIds;
  std::map<int, MedVar> RegVarOfId;
  for (const MedBlock &Blk : Func.Blocks) {
    for (const MedOp &Op : Blk.Ops) {
      auto AddReg = [&](const MedVar &V) {
        if (V.Kind != MedVar::Reg || V.Id < 0)
          return;
        RegOffToIds[V.RegOff].insert(V.Id);
        RegVarOfId.emplace(V.Id, V);
      };
      AddReg(Op.Output);
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        AddReg(Op.Inputs[I]);
    }
  }

  // An Itanium landing pad overwrites the target ABI's first two integer
  // result registers with {exception object, selector}.  Define those values
  // at every exceptional target before liveness/SSA construction so they kill
  // the protected call's ordinary register values, while all preserved frame
  // registers continue to flow over the exceptional edge.
  const uint64_t EHSelectorReg =
      TRI.IntReturnRegs.size() > 1 ? TRI.IntReturnRegs[1] : TRI.IntReturnReg2;
  auto InsertEHDefs = [&](MedBlock &Block, uint64_t RegOff,
                          MedVar::VarKind Kind) {
    auto IdsIt = RegOffToIds.find(RegOff);
    if (IdsIt == RegOffToIds.end())
      return;
    std::vector<MedOp> Defs;
    for (int Id : IdsIt->second) {
      auto VarIt = RegVarOfId.find(Id);
      if (VarIt == RegVarOfId.end())
        continue;
      MedOp Def;
      Def.Opcode = NdOp::COPY;
      Def.Output = VarIt->second;
      MedVar Input = VarIt->second;
      Input.Kind = Kind;
      Input.Id = -1;
      Input.SSAVer = 0;
      // The Itanium landing-pad pair has ABI-defined widths independent of
      // whichever register alias the recovered body reads.  In particular,
      // an AArch64 W0 live-in is the low view of the pointer in X0, not a
      // 32-bit exception object.  Keeping the pseudo-value pointer-sized lets
      // call-ABI recovery pass the full object to __cxa_begin_catch while the
      // COPY into W0 still truncates naturally.
      Input.Size = Kind == MedVar::EHException ? TRI.PointerSize : 4;
      Def.addInput(Input);
      Def.Addr = Block.StartAddr;
      Defs.push_back(Def);
    }
    Block.Ops.insert(Block.Ops.begin(), Defs.begin(), Defs.end());
  };
  if (UseItaniumExceptionalFlow) {
    for (int B = 0; B < N; ++B) {
      if (!IsExceptionalTarget[B])
        continue;
      // Insert selector first so the exception definition remains the first
      // recovered register copy; neither order is semantically significant.
      if (EHSelectorReg != 0)
        InsertEHDefs(Func.Blocks[B], EHSelectorReg, MedVar::EHSelector);
      InsertEHDefs(Func.Blocks[B], TRI.IntReturnReg, MedVar::EHException);
    }
  }

  // Handler blocks that ordinary flow also enters, with their exception code.
  std::vector<std::pair<int, MedVar>> SharedHandlerCodes;
  // __C_specific_handler resumes a Windows x64 __except handler through
  // RtlUnwindEx with the exception code as the return value: EAX holds the
  // code and the upper half of RAX is zero.  Define those views at each
  // handler entry so the handler never reads an ordinary path's RAX.
  if (TargetArch == Arch::X64 && Low.ExceptionMetadata &&
      Low.ExceptionMetadata->SEH) {
    auto IdsIt = RegOffToIds.find(TRI.IntReturnReg);
    // Each handler entry receives its own code.  One shared value would let a
    // later handler's code stand in for an earlier one that is still live.
    std::vector<int> Handlers;
    for (int B = 0; B < N && IdsIt != RegOffToIds.end(); ++B) {
      const auto &Preds = Func.Blocks[B].ExceptionalPreds;
      if (std::any_of(Preds.begin(), Preds.end(), [](const ExceptionalEdge &E) {
            return E.Kind == ExceptionalEdgeKind::SEHHandler;
          }))
        Handlers.push_back(B);
    }
    std::stable_sort(Handlers.begin(), Handlers.end(), [&](int L, int R) {
      return Func.Blocks[L].StartAddr < Func.Blocks[R].StartAddr;
    });
    int Ordinal = 0;
    for (int B : Handlers) {
      MedVar Code;
      Code.Kind = MedVar::SEHExceptionCode;
      Code.TheArch = TargetArch;
      Code.Id = MedVar::SEHExceptionCodeId;
      Code.SSAVer = ++Ordinal;
      Code.Size = 4;
      Code.ConstVal = Func.Blocks[B].StartAddr;
      // A handler that ordinary flow also reaches keeps that flow's RAX: its
      // RAX views merge the two entries in PHIs instead (Step 3b).
      if (std::any_of(FlowPreds[B].begin(), FlowPreds[B].end(),
                      [&](int P) { return P != VirtualRoot; })) {
        SharedHandlerCodes.emplace_back(B, Code);
        continue;
      }
      std::vector<MedOp> Defs;
      for (int Id : IdsIt->second) {
        MedOp Def;
        Def.Output = RegVarOfId.at(Id);
        // A narrower view keeps the low bytes of the code, as COPY does.
        Def.Opcode = Def.Output.Size > Code.Size ? NdOp::INT_ZEXT : NdOp::COPY;
        Def.addInput(Code);
        Def.Addr = Func.Blocks[B].StartAddr;
        Defs.push_back(Def);
      }
      Func.Blocks[B].Ops.insert(Func.Blocks[B].Ops.begin(), Defs.begin(),
                                Defs.end());
    }
  }

  auto fullyPreserved = [&](uint64_t RegOff, uint16_t Size) {
    if (Size == 0)
      return false;
    if (TRI.callPreservedPrefixSize(RegOff, Size) >= Size)
      return true;
    // SysV callee-saves omit Win64 RSI/RDI and XMM6-15. Those live in
    // callPreservedRanges(COFF). A partial prefix (ZMM low 16) stays a clobber.
    for (const auto &Range : TRI.callPreservedRanges(TargetFormat)) {
      if (RegOff < Range.Offset || Size > Range.Bytes)
        continue;
      if (RegOff + static_cast<uint64_t>(Size) <=
          Range.Offset + static_cast<uint64_t>(Range.Bytes))
        return true;
    }
    return false;
  };

  auto CallClobberedIds = [&](const MedOp &Op) {
    std::set<int> Result;
    if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
        Op.PreservesCallerSaved)
      return Result;
    for (const auto &[RegOff, Ids] : RegOffToIds) {
      if (TRI.isFrameOrLinkReg(RegOff) || TRI.isStackPointer(RegOff) ||
          (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == RegOff))
        continue;
      if (Op.CallPreservedGPRs)
        if (auto Family = gprFamilyOf(TRI.TheArch, RegOff);
            Family && (Op.CallPreservedGPRs >> *Family) & 1)
          continue;
      for (int Id : Ids) {
        auto It = RegVarOfId.find(Id);
        if (It == RegVarOfId.end() || !fullyPreserved(RegOff, It->second.Size))
          Result.insert(Id);
      }
    }
    return Result;
  };

  // An x64 SEH handler is entered by the unwinder with the protected frame's
  // nonvolatile registers as they were when the exception was raised. Where
  // no protected block writes one, that is the value live throughout the
  // protected range, not the value the function was entered with.
  std::map<int, std::vector<int>> SEHProtected;
  std::map<int, std::map<int, std::set<va_t>>> CxxProtectedCalls;
  std::set<int> SEHTrackedIds;
  std::set<int> SEHHandlerRoots;
  // Handler roots of a frame-register function that moves SP after its
  // prologue: they resume with the SP live throughout the protected range.
  std::set<int> SEHProtectedSPRoots;
  // The calling convention's callee-saved registers, which the unwinder
  // restores as well. The stack pointer has its own frame proof.
  auto IsNonvolatile = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg && V.Size != 0 &&
           !TRI.isStackPointer(V.RegOff) && fullyPreserved(V.RegOff, V.Size);
  };
  // x86 EH3/EH4 restores the registration frame's EBP before entering a
  // handler; it does not restore arbitrary callee-saved GPRs from the fault.
  const x86_registration::RegistrationRoots RegistrationRoots(Low, TargetArch,
                                                              TargetFormat);
  const bool HasX86SEHFrame = RegistrationRoots.hasSEHFrame();
  const bool HasX86RegistrationFrame = RegistrationRoots.hasFrame();
  auto IsSEHRestored = [&](const MedVar &V) {
    if (TargetArch == Arch::X86)
      return HasX86RegistrationFrame && V.Kind == MedVar::Reg &&
             V.RegOff == TRI.FramePointer && V.Size == TRI.PointerSize;
    return IsNonvolatile(V);
  };
  auto IsFullStackPointer = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg && V.RegOff == TRI.StackPointer &&
           V.Size == TRI.PointerSize;
  };

  // Step 0: Insert implicit definitions for live-in variables in the entry
  // block.  A variable is live-in to the function when some path from entry
  // uses it before any definition — exactly the values the caller supplies.
  // This is a standard backward liveness fixpoint: a plain "defined earlier in
  // reverse post-order" test is unsound, since a definition in a sibling or
  // return block reaches no use here yet would mask a genuine live-in (e.g. a
  // pointer parameter first read in a loop preheader while a `n==0` exit block
  // also clears that register).
  {
    // Per-block upward-exposed uses (read before any local definition) and
    // kills (definitions, alias-expanded), plus a representative MedVar per Id
    // for materialising the self-copy.
    std::vector<std::set<int>> UEVar(N), VarKill(N);
    std::map<int, MedVar> VarOfId;
    for (int B = 0; B < N; ++B) {
      std::set<int> &Kill = VarKill[B];
      for (auto &Op : Func.Blocks[B].Ops) {
        for (uint8_t I = 0; I < Op.NumInputs; ++I) {
          const auto &Inp = Op.Inputs[I];
          if (Inp.Id >= 0 && !Kill.count(Inp.Id)) {
            UEVar[B].insert(Inp.Id);
            VarOfId.emplace(Inp.Id, Inp);
          }
        }
        // A partial call clobber reads the pre-call value to retain its ABI-
        // preserved low prefix.  Account for that hidden use before applying
        // the call's kill, otherwise a first-use v8-v15 Q view would lack its
        // live-in definition.
        for (int Id : CallClobberedIds(Op)) {
          auto It = RegVarOfId.find(Id);
          if (It == RegVarOfId.end())
            continue;
          uint16_t Prefix = TRI.callPreservedPrefixSize(
              It->second.RegOff, It->second.Size, TargetFormat);
          if (Prefix > 0 && Prefix < It->second.Size && !Kill.count(Id)) {
            UEVar[B].insert(Id);
            VarOfId.emplace(Id, It->second);
          }
        }
        if (Op.Output.Id >= 0 && Op.Output.Size > 0) {
          Kill.insert(Op.Output.Id);
          if (Op.Output.Kind == MedVar::Reg) {
            auto It = RegOffToIds.find(Op.Output.RegOff);
            if (It != RegOffToIds.end())
              for (int Id : It->second) {
                // A partial write (`mov r9w, ...`) leaves the upper bytes of
                // a wider view live: a later full read still takes them from
                // the caller.  Only x64 takes such a value as incoming; an
                // i386 register live on entry would become a regparm
                // parameter that a cdecl caller never passes.
                auto View = RegVarOfId.find(Id);
                if (TargetArch != Arch::X64 || View == RegVarOfId.end() ||
                    View->second.Size <= Op.Output.Size)
                  Kill.insert(Id);
              }
          }
        }
        for (const MedVar &Aux : Op.IntrinsicOutputs)
          if (Aux.Id >= 0 && Aux.Size > 0)
            Kill.insert(Aux.Id);
        std::set<int> Clobbered = CallClobberedIds(Op);
        Kill.insert(Clobbered.begin(), Clobbered.end());
      }
    }

    std::vector<std::set<int>> LiveIn(N), LiveOut(N);
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (auto It = RPO.rbegin(); It != RPO.rend(); ++It) {
        int B = *It;
        if (B == VirtualRoot)
          continue;
        std::set<int> NewOut;
        for (int S : FlowSuccs[B])
          if (S >= 0 && S < N)
            NewOut.insert(LiveIn[S].begin(), LiveIn[S].end());
        std::set<int> NewIn = UEVar[B];
        for (int V : NewOut)
          if (!VarKill[B].count(V))
            NewIn.insert(V);
        if (NewIn != LiveIn[B] || NewOut != LiveOut[B]) {
          LiveIn[B] = std::move(NewIn);
          LiveOut[B] = std::move(NewOut);
          Changed = true;
        }
      }
    }

    std::map<int, uint64_t> SEHFrameOffsets;
    std::set<int> CxxContinuationRoots;
    std::set<int> LocalUnwindRoots;
    if (TargetArch == Arch::X64 && Low.ExceptionMetadata &&
        Low.ExceptionMetadata->Cxx && !Low.CxxContinuationEntries.empty()) {
      for (int B = 1; B < N; ++B) {
        if (!llvm::any_of(Low.CxxContinuationEntries, [&](const auto &Entry) {
              return Entry.Target == Func.Blocks[B].StartAddr;
            }))
          continue;
        std::vector<int> Protected;
        std::map<int, std::set<va_t>> Calls;
        auto FrameBytes = proveSEHEstablisherFrame(
            Low, Func, Func.Blocks[B].StartAddr, TRI, !IsRoot[B],
            /*LocalUnwindDigest=*/{}, /*CxxContinuation=*/true, &Protected,
            &Calls);
        if (!IsRoot[B])
          continue;
        CxxContinuationRoots.insert(B);
        SEHProtected[B] = std::move(Protected);
        if (!Calls.empty())
          CxxProtectedCalls[B] = std::move(Calls);
        if (FrameBytes)
          SEHFrameOffsets[B] = *FrameBytes;
        else
          SEHProtectedSPRoots.insert(B);
        for (int Id : LiveIn[B])
          if (auto V = VarOfId.find(Id); V != VarOfId.end()) {
            if (TRI.isStackPointer(V->second.RegOff) &&
                V->second.Kind == MedVar::Reg && !IsFullStackPointer(V->second))
              throw LowToMedConversionError(
                  "Windows C++ continuation has a partial stack pointer");
            if (IsSEHRestored(V->second) ||
                (SEHProtectedSPRoots.count(B) && IsFullStackPointer(V->second)))
              SEHTrackedIds.insert(Id);
          }
      }
    }
    if (TargetArch == Arch::X64 && Low.ExceptionMetadata &&
        Low.ExceptionMetadata->SEH) {
      // LowIR is immutable throughout this conversion. Hash all predecessors
      // once, even when many handlers consume the same local-unwind receipts.
      const std::string LocalUnwindDigest =
          Low.SEHLocalUnwindContinuations.empty()
              ? std::string{}
              : lowSEHFrameDependencyDigest(
                    Low,
                    Low.SEHLocalUnwindContinuations.front().CalleeDependencies,
                    SEHFrameCallees);
      for (const auto &Evidence : Low.SEHLocalUnwindContinuations)
        if (LocalUnwindDigest.empty() ||
            Evidence.CalleeDependencies !=
                Low.SEHLocalUnwindContinuations.front().CalleeDependencies)
          throw LowToMedConversionError(
              "Windows SEH establisher frame: stale local-unwind callee proof");
      for (int B = 0; B < N; ++B) {
        const bool IsSEHHandler =
            std::any_of(Func.Blocks[B].ExceptionalPreds.begin(),
                        Func.Blocks[B].ExceptionalPreds.end(),
                        [](const ExceptionalEdge &E) {
                          return E.Kind == ExceptionalEdgeKind::SEHHandler;
                        });
        const bool IsLocalUnwindTarget = llvm::any_of(
            Low.SEHLocalUnwindContinuations, [&](const auto &Call) {
              return Call.Target == Func.Blocks[B].StartAddr;
            });
        if (!IsSEHHandler && !IsLocalUnwindTarget)
          continue;
        bool NeedsFrame = IsLocalUnwindTarget;
        for (int Id : LiveIn[B]) {
          auto V = VarOfId.find(Id);
          if (V == VarOfId.end() || V->second.Kind != MedVar::Reg ||
              V->second.RegOff != TRI.StackPointer)
            continue;
          if (V->second.Size != TRI.PointerSize)
            throw LowToMedConversionError(
                "Windows SEH establisher frame: handler is not an isolated "
                "full-width SP root");
          NeedsFrame = true;
        }
        if (!NeedsFrame)
          continue;
        // Ordinary flow and runtime resumption must agree on the same frame.
        // A runtime-only continuation is a real root: its SP is the proved
        // target frame, not a second ordinary function-entry SP.
        const auto FrameBytes = proveSEHEstablisherFrame(
            Low, Func, Func.Blocks[B].StartAddr, TRI,
            /*OrdinaryEntry=*/!IsRoot[B], LocalUnwindDigest);
        if (!IsRoot[B])
          continue;
        if (FrameBytes)
          SEHFrameOffsets[B] = *FrameBytes;
        else
          SEHProtectedSPRoots.insert(B);
        if (IsLocalUnwindTarget)
          LocalUnwindRoots.insert(B);
      }
    }

    for (int Root : Roots) {
      if (Root != 0)
        NonEntryRootLiveIns.insert(LiveIn[Root].begin(), LiveIn[Root].end());
      if (Root != 0 && (TargetArch == Arch::X64 || HasX86SEHFrame) &&
          Low.ExceptionMetadata &&
          (Low.ExceptionMetadata->SEH || HasX86SEHFrame)) {
        const ExceptionFunction &EH = *Low.ExceptionMetadata;
        std::vector<int> Protected;
        bool Complete = true;
        // CFGBuilder owns registration try-level interpretation. Reuse its
        // exceptional predecessor set rather than inventing x86 scope ranges.
        // The personality calls a filter and a finally block with the frame's
        // EBP as well, and resumes at an except handler with it.
        if (HasX86SEHFrame) {
          for (const ExceptionalEdge &Edge : Func.Blocks[Root].ExceptionalPreds)
            if (Edge.Kind == ExceptionalEdgeKind::SEHHandler ||
                Edge.Kind == ExceptionalEdgeKind::SEHFilter ||
                Edge.Kind == ExceptionalEdgeKind::SEHFinally) {
              if (Edge.BlockId < 0 || Edge.BlockId >= N) {
                Complete = false;
                break;
              }
              Protected.push_back(Edge.BlockId);
            }
        }
        if (!HasX86SEHFrame && EH.SEH)
          for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
            if (Scope.HandlerVA != Func.Blocks[Root].StartAddr)
              continue;
            auto Range = getSemanticSEHGuardedRange(Scope, TargetArch, EH);
            if (Scope.ParseStatus != ExceptionParseStatus::Complete || !Range) {
              Complete = false;
              break;
            }
            for (int B = 0; B < N; ++B)
              if (Func.Blocks[B].StartAddr < Range->End &&
                  Func.Blocks[B].EndAddr > Range->Begin)
                Protected.push_back(B);
          }
        if (Complete && !Protected.empty()) {
          SEHProtected[Root] = std::move(Protected);
          for (int Id : LiveIn[Root])
            if (auto V = VarOfId.find(Id);
                V != VarOfId.end() &&
                (IsSEHRestored(V->second) || (SEHProtectedSPRoots.count(Root) &&
                                              IsFullStackPointer(V->second))))
              SEHTrackedIds.insert(Id);
        }
      }
      std::vector<MedOp> InitOps;
      // An x86 filter or finally block is entered by the personality too, with
      // no callee-saved register of the faulting code.
      const bool IsCxxHandlerRoot =
          Root != 0 && RegistrationRoots.isCxxHandler(Func.Blocks[Root]);
      const auto RestoredCxxSP =
          Root == 0 ? std::nullopt
                    : RegistrationRoots.restoredStackOffset(Func.Blocks[Root]);
      const bool IsWindowsRuntimeRoot =
          CxxContinuationRoots.count(Root) || LocalUnwindRoots.count(Root) ||
          (Root != 0 &&
           (IsCxxHandlerRoot || RestoredCxxSP ||
            std::any_of(Func.Blocks[Root].ExceptionalPreds.begin(),
                        Func.Blocks[Root].ExceptionalPreds.end(),
                        [&](const ExceptionalEdge &E) {
                          return E.Kind == ExceptionalEdgeKind::SEHHandler ||
                                 (HasX86SEHFrame &&
                                  (E.Kind == ExceptionalEdgeKind::SEHFilter ||
                                   E.Kind == ExceptionalEdgeKind::SEHFinally));
                        })));
      if (IsWindowsRuntimeRoot)
        SEHHandlerRoots.insert(Root);
      const bool IsItaniumEHRoot =
          !Func.Blocks[Root].ExceptionalPreds.empty() &&
          Func.ExceptionMetadata && Func.ExceptionMetadata->Itanium &&
          Func.ExceptionMetadata->Itanium->IsCallSiteAddressForm;
      for (int Id : LiveIn[Root]) {
        auto VIt = VarOfId.find(Id);
        if (VIt == VarOfId.end())
          continue;
        MedOp Init;
        Init.Opcode = NdOp::COPY;
        Init.Output = VIt->second;
        MedVar Input = VIt->second;
        const bool Callback =
            IsCxxHandlerRoot ||
            std::any_of(Func.Blocks[Root].ExceptionalPreds.begin(),
                        Func.Blocks[Root].ExceptionalPreds.end(),
                        [](const ExceptionalEdge &Edge) {
                          return Edge.Kind == ExceptionalEdgeKind::SEHFilter ||
                                 Edge.Kind == ExceptionalEdgeKind::SEHFinally;
                        });
        RegistrationRoots.initialize(Init, IsWindowsRuntimeRoot, Callback,
                                     RestoredCxxSP);
        if (IsItaniumEHRoot && Input.Kind == MedVar::Reg &&
            Input.RegOff == TRI.IntReturnReg) {
          Input.Kind = MedVar::EHException;
          Input.Id = -1;
          Input.SSAVer = 0;
          Input.Size = TRI.PointerSize;
        } else if (IsItaniumEHRoot && EHSelectorReg != 0 &&
                   Input.Kind == MedVar::Reg && Input.RegOff == EHSelectorReg) {
          Input.Kind = MedVar::EHSelector;
          Input.Id = -1;
          Input.SSAVer = 0;
          Input.Size = 4;
        }
        // Both x86 calling conventions enter a function with DF clear, so
        // its incoming value is known rather than a parameter.  Only the
        // function entry has that guarantee; an exception landing pad does
        // not.
        if ((TargetArch == Arch::X86 || TargetArch == Arch::X64) &&
            (Input.Kind == MedVar::Reg || Input.Kind == MedVar::Flag) &&
            Input.RegOff == x86reg::DF &&
            Func.Blocks[Root].ExceptionalPreds.empty() && Root == 0)
          Input = MedVar::makeConst(0, Input.Size,
                                    ConstantAddressProvenance::Scalar);
        // The unwinder enters an SEH handler with its own values in the
        // flags and in the registers the calling convention does not
        // preserve; EAX, the exception code, is defined separately.
        if (IsWindowsRuntimeRoot &&
            ((Input.Kind == MedVar::Reg && !TRI.isStackPointer(Input.RegOff) &&
              !TRI.isFrameOrLinkReg(Input.RegOff) &&
              (HasX86RegistrationFrame || !IsNonvolatile(Input))) ||
             Input.Kind == MedVar::Flag))
          Input = MedVar::makeUnspecified(Input.Size, TargetArch);
        Init.addInput(Input);
        if (Input.Kind == MedVar::Reg && Input.RegOff == TRI.StackPointer) {
          auto Offset = SEHFrameOffsets.find(Root);
          if (Offset != SEHFrameOffsets.end()) {
            Init.Opcode = NdOp::INT_SUB;
            Init.addInput(MedVar::makeConst(Offset->second, TRI.PointerSize));
          }
        }
        Init.Addr = Func.Blocks[Root].StartAddr;
        InitOps.push_back(Init);
        LLVM_DEBUG(llvm::dbgs() << "  live-in: " << VIt->second.display()
                                << " (id=" << Id << ")\n");
      }
      auto &RootOps = Func.Blocks[Root].Ops;
      RootOps.insert(RootOps.begin(), InitOps.begin(), InitOps.end());
    }
  }

  // Step 2: Compute dominance frontiers
  std::vector<std::set<int>> DF(GraphN);
  for (int B = 0; B < N; ++B) {
    // A root's implicit machine-entry edge has no MedBlock predecessor from
    // which a PHI argument could be emitted.  Preserve the existing entry
    // semantics for a root SCC with a real backedge instead of constructing a
    // PHI that silently omits its initial value.  Ordinary multi-root joins are
    // not roots and therefore still receive their complete frontier PHIs.
    if (IsRoot[B])
      continue;
    if (FlowPreds[B].size() < 2)
      continue;
    for (int P : FlowPreds[B]) {
      if (P < 0 || P >= GraphN)
        continue;
      int Runner = P;
      while (Runner != IDom[B] && Runner != -1) {
        DF[Runner].insert(B);
        Runner = IDom[Runner];
      }
    }
  }

  // Step 3: Collect all variable definitions per block
  std::map<int, std::set<int>> VarDefs;
  for (int B = 0; B < N; ++B) {
    for (auto &Op : Func.Blocks[B].Ops) {
      if (Op.Output.Id >= 0 && Op.Output.Size > 0)
        VarDefs[Op.Output.Id].insert(B);
      for (const MedVar &Aux : Op.IntrinsicOutputs)
        if (Aux.Id >= 0 && Aux.Size > 0)
          VarDefs[Aux.Id].insert(B);
      for (int Id : CallClobberedIds(Op))
        VarDefs[Id].insert(B);
    }
  }

  // Step 3b: at a handler block ordinary flow also enters, each RAX view is
  // a PHI of the ordinary predecessors' values that takes the exception code
  // on the dispatcher's entry. It defines the view there, so the iterated
  // frontier below places the PHIs that merge it further on.
  if (auto IdsIt = RegOffToIds.find(TRI.IntReturnReg);
      IdsIt != RegOffToIds.end())
    for (const auto &[B, Code] : SharedHandlerCodes)
      for (int Id : IdsIt->second) {
        const MedVar &View = RegVarOfId.at(Id);
        // A view above the low byte would need a shifted code.
        if (View.RegOff != TRI.IntReturnReg)
          continue;
        PhiNode Phi;
        Phi.Output = View;
        for (int P : FlowPreds[B])
          if (P != VirtualRoot)
            Phi.Args.push_back({P, View});
        Phi.ExceptionalEntry = Code;
        Func.Blocks[B].Phis.push_back(std::move(Phi));
        VarDefs[Id].insert(B);
      }

  // Step 4: Insert phi nodes
  std::map<int, MedVar> VarIdToVar = RegVarOfId;
  for (auto &Blk : Func.Blocks)
    for (auto &Op : Blk.Ops) {
      if (Op.Output.Id >= 0 && Op.Output.Size > 0)
        VarIdToVar.emplace(Op.Output.Id, Op.Output);
      for (const MedVar &Aux : Op.IntrinsicOutputs)
        if (Aux.Id >= 0 && Aux.Size > 0)
          VarIdToVar.emplace(Aux.Id, Aux);
    }

  for (auto &[VarId, DefBlocks] : VarDefs) {
    std::set<int> PhiBlocks;
    std::queue<int> Worklist;
    for (int B : DefBlocks)
      Worklist.push(B);

    while (!Worklist.empty()) {
      int B = Worklist.front();
      Worklist.pop();
      for (int D : DF[B]) {
        if (D < 0 || D >= N)
          continue;
        if (PhiBlocks.insert(D).second) {
          Worklist.push(D);
          // Step 3b already placed this variable's PHI at a shared handler.
          const auto &Existing = Func.Blocks[D].Phis;
          if (std::any_of(
                  Existing.begin(), Existing.end(),
                  [&](const PhiNode &Phi) { return Phi.Output.Id == VarId; }))
            continue;
          auto VIt = VarIdToVar.find(VarId);
          MedVar PhiVar = (VIt != VarIdToVar.end()) ? VIt->second : MedVar{};

          PhiNode Phi;
          Phi.Output = PhiVar;
          for (int P : FlowPreds[D])
            if (P != VirtualRoot)
              Phi.Args.push_back({P, PhiVar});
          Func.Blocks[D].Phis.push_back(Phi);
        }
      }
    }
  }

  // Step 5: SSA renaming (assign version numbers)
  std::map<int, int> VarCounter;
  std::map<int, std::vector<int>> VarStack;
  // Version 0 is the value a variable holds when a root is entered.  A root
  // other than the entry reads it after the entry's subtree has been
  // renamed, so its definitions start at 1 instead of taking version 0 and
  // becoming what that root reads.
  for (int Id : NonEntryRootLiveIns)
    VarCounter[Id] = std::max(VarCounter[Id], 1);

  auto GetVersion = [&](int VarId) -> int {
    if (VarStack[VarId].empty())
      return 0;
    return VarStack[VarId].back();
  };
  auto NewVersion = [&](int VarId) -> int {
    int V = VarCounter[VarId]++;
    VarStack[VarId].push_back(V);
    return V;
  };

  std::vector<std::vector<int>> DomChildren(GraphN);
  for (int C = 0; C < GraphN; ++C) {
    if (C != VirtualRoot && IDom[C] != -1 && IDom[C] != C)
      DomChildren[IDom[C]].push_back(C);
  }

  std::set<int> SEHProtectedBlocks;
  for (const auto &[Root, Blocks] : SEHProtected)
    SEHProtectedBlocks.insert(Blocks.begin(), Blocks.end());
  std::map<int, std::map<int, int>> SEHEntryVersions;
  std::map<std::pair<int, va_t>, std::map<int, int>> CxxCallVersions;
  std::set<std::pair<int, va_t>> CxxThrowingCalls;
  for (const auto &[Root, Blocks] : CxxProtectedCalls)
    for (const auto &[B, Sites] : Blocks)
      for (va_t Site : Sites)
        CxxThrowingCalls.emplace(B, Site);

  struct Frame {
    int B;
    size_t ChildIdx;
    std::map<int, int> SavedSizes;
  };
  std::vector<Frame> Stk;
  Stk.reserve(N);

  auto ProcessBlock = [&](Frame &F) {
    if (F.B == VirtualRoot)
      return;
    if (F.B < 0 || F.B >= N)
      return;
    auto &Blk = Func.Blocks[F.B];
    for (auto &Phi : Blk.Phis) {
      Phi.Output.SSAVer = NewVersion(Phi.Output.Id);
      F.SavedSizes[Phi.Output.Id]++;
    }
    if (SEHProtectedBlocks.count(F.B))
      for (int Id : SEHTrackedIds)
        SEHEntryVersions[F.B][Id] = GetVersion(Id);
    for (auto &Op : Blk.Ops) {
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          CxxThrowingCalls.count({F.B, Op.Addr}))
        for (int Id : SEHTrackedIds)
          CxxCallVersions[{F.B, Op.Addr}][Id] = GetVersion(Id);
      for (uint8_t I = 0; I < Op.NumInputs; ++I) {
        if (Op.Inputs[I].Id >= 0)
          Op.Inputs[I].SSAVer = GetVersion(Op.Inputs[I].Id);
      }
      if (Op.Output.Id >= 0 && Op.Output.Size > 0) {
        Op.Output.SSAVer = NewVersion(Op.Output.Id);
        F.SavedSizes[Op.Output.Id]++;
      }
      for (MedVar &Aux : Op.IntrinsicOutputs) {
        if (Aux.Id < 0 || Aux.Size == 0)
          continue;
        Aux.SSAVer = NewVersion(Aux.Id);
        F.SavedSizes[Aux.Id]++;
      }
      for (int Id : CallClobberedIds(Op)) {
        auto It = RegVarOfId.find(Id);
        MedVar PreservedInput;
        uint16_t PreservedPrefixSize = 0;
        if (It != RegVarOfId.end()) {
          PreservedPrefixSize = TRI.callPreservedPrefixSize(
              It->second.RegOff, It->second.Size, TargetFormat);
          if (PreservedPrefixSize > 0 &&
              PreservedPrefixSize < It->second.Size) {
            PreservedInput = It->second;
            PreservedInput.SSAVer = GetVersion(Id);
          } else {
            PreservedPrefixSize = 0;
          }
        }
        int Version = NewVersion(Id);
        F.SavedSizes[Id]++;
        if (It != RegVarOfId.end()) {
          MedVar Clobber = It->second;
          Clobber.SSAVer = Version;
          Func.CallClobbers.push_back(
              {Clobber, Op.CallSiteId, PreservedInput, PreservedPrefixSize});
        }
      }
    }
    for (int S : FlowSuccs[F.B]) {
      if (S < 0 || S >= N)
        continue;
      for (auto &Phi : Func.Blocks[S].Phis) {
        for (auto &[Pred, Arg] : Phi.Args) {
          if (Pred == F.B)
            Arg.SSAVer = GetVersion(Arg.Id);
        }
      }
    }
  };

  Stk.push_back({VirtualRoot, 0, {}});
  ProcessBlock(Stk.back());
  while (!Stk.empty()) {
    auto &F = Stk.back();
    auto &Children = DomChildren[F.B];
    if (F.ChildIdx < Children.size()) {
      int C = Children[F.ChildIdx++];
      Stk.push_back({C, 0, {}});
      ProcessBlock(Stk.back());
    } else {
      for (auto &[VId, Cnt] : F.SavedSizes) {
        for (int I = 0; I < Cnt; ++I)
          VarStack[VId].pop_back();
      }
      Stk.pop_back();
    }
  }

  for (const auto &[Root, Blocks] : SEHProtected) {
    // Register bytes some protected operation writes.
    std::vector<std::pair<uint64_t, uint64_t>> Written;
    auto NoteWrite = [&](const MedVar &V) {
      if (V.Kind == MedVar::Reg && V.Size != 0)
        Written.emplace_back(V.RegOff, V.RegOff + V.Size);
    };
    for (int B : Blocks)
      for (const MedOp &Op : Func.Blocks[B].Ops) {
        NoteWrite(Op.Output);
        for (const MedVar &Aux : Op.IntrinsicOutputs)
          NoteWrite(Aux);
      }
    auto WrittenInRange = [&](const MedVar &V) {
      return std::any_of(Written.begin(), Written.end(), [&](const auto &W) {
        return W.first < V.RegOff + V.Size && V.RegOff < W.second;
      });
    };
    for (MedOp &Seed : Func.Blocks[Root].Ops) {
      // The root's seeds come first, all at its address.
      if (Seed.Addr != Func.Blocks[Root].StartAddr)
        break;
      if (Seed.Opcode != NdOp::COPY || Seed.NumInputs != 1)
        continue;
      if (Seed.RegistrationRoot != MedOp::RegistrationRootKind::None)
        continue;
      MedVar &In = Seed.Inputs[0];
      const bool ProtectedSP =
          SEHProtectedSPRoots.count(Root) && IsFullStackPointer(In);
      if ((!IsSEHRestored(In) && !ProtectedSP) || In.Id != Seed.Output.Id ||
          In.SSAVer != 0)
        continue;
      const auto Calls = CxxProtectedCalls.find(Root);
      const bool AtCalls = Calls != CxxProtectedCalls.end();
      // Full-width SSA definitions identify the value at a synchronous call.
      // A write through another register view still invalidates this proof.
      bool AliasedWrite = false;
      if (AtCalls)
        for (int B : Blocks)
          for (const MedOp &Op : Func.Blocks[B].Ops) {
            auto CheckAlias = [&](const MedVar &V) {
              AliasedWrite |= V.Kind == MedVar::Reg && V.Size &&
                              V.RegOff < In.RegOff + In.Size &&
                              In.RegOff < V.RegOff + V.Size && V.Id != In.Id;
            };
            CheckAlias(Op.Output);
            for (const MedVar &Aux : Op.IntrinsicOutputs)
              CheckAlias(Aux);
          }
      if ((!AtCalls && WrittenInRange(In)) || AliasedWrite) {
        In = MedVar::makeUnspecified(In.Size, TargetArch);
        continue;
      }
      std::optional<int> Live;
      bool Same = true;
      auto AddLive = [&](const std::map<int, int> &Versions) {
        auto It = Versions.find(In.Id);
        if (It == Versions.end() || (Live && *Live != It->second)) {
          Same = false;
          return;
        }
        Live = It->second;
      };
      if (AtCalls) {
        for (const auto &[B, Sites] : Calls->second)
          for (va_t Site : Sites)
            AddLive(CxxCallVersions[{B, Site}]);
      } else
        for (int B : Blocks)
          AddLive(SEHEntryVersions[B]);
      if (!Same || !Live)
        In = MedVar::makeUnspecified(In.Size, TargetArch);
      else if (*Live > 0)
        In.SSAVer = *Live;
    }
  }
  // Without a proven protected range, a callee-saved register's value at
  // the fault is not known either.
  for (int Root : SEHHandlerRoots) {
    for (MedOp &Seed : Func.Blocks[Root].Ops) {
      if (Seed.Addr != Func.Blocks[Root].StartAddr)
        break;
      if (Seed.Opcode != NdOp::COPY || Seed.NumInputs != 1)
        continue;
      if (Seed.RegistrationRoot != MedOp::RegistrationRootKind::None)
        continue;
      MedVar &In = Seed.Inputs[0];
      if (IsNonvolatile(In) && In.Id == Seed.Output.Id && In.SSAVer == 0 &&
          (!SEHProtected.count(Root) || !IsSEHRestored(In)))
        In = MedVar::makeUnspecified(In.Size, TargetArch);
    }
  }
}

} // namespace neverd
