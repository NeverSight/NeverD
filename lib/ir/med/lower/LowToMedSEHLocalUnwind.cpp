//===- LowToMedSEHLocalUnwind.cpp - Local unwinds as handler calls -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A `return` or `goto` that leaves a Windows `__try` with a `__finally`
/// compiles to a call of the runtime's local unwind,
/// `_local_unwind(TargetFrame, TargetIp)`.  The runtime runs the termination
/// handler of each scope the jump leaves, innermost first, with the
/// abnormal-termination flag set and the EstablisherFrame, then resumes at
/// TargetIp in the same frame; the call never returns.  Read as an ordinary
/// call it would fall through into the compiler's normal-path code and pass
/// an interior code address.  Where the saved-frame protocol certifies the
/// call (SEHLocalUnwindProtocol.h), it becomes what it does, before SSA: the
/// handler calls, then a jump to the target.
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/SEHLocalUnwindProtocol.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExceptionFunction.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Sequence.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {
namespace {

using Handler = MedSEHLocalUnwindExit::Handler;

/// The termination handlers a target unwind of this frame from \p ControlPc
/// to \p TargetPc runs, innermost first, as `__C_specific_handler` walks the
/// scope table: only a scope guarding ControlPc takes part, and the walk
/// stops at the first one whose `__try` also guards TargetPc (any record with
/// its handler and jump target does, as one `__try` the compiler split over
/// several ranges) or whose `__except` handler is TargetPc; an `__except`
/// scope runs nothing.  \p CallPc must take the same part ControlPc does in
/// every scope.  Nullopt when a range is not exact or a handler is not an
/// out-of-line routine.
std::optional<std::vector<Handler>> unwindHandlers(const ExceptionFunction &EH,
                                                   Arch TargetArch, va_t CallPc,
                                                   va_t ControlPc,
                                                   va_t TargetPc) {
  const std::vector<SEHScopeRecord> &Scopes = EH.SEH->Scopes;
  if (Scopes.size() > limits::kMaxSEHEstablisherFrameWork)
    return std::nullopt;
  std::vector<ExceptionAddressRange> Ranges;
  Ranges.reserve(Scopes.size());
  for (const SEHScopeRecord &Scope : Scopes) {
    const std::optional<ExceptionAddressRange> Range =
        getSemanticSEHGuardedRange(Scope, TargetArch, EH);
    if (Scope.ParseStatus != ExceptionParseStatus::Complete || !Range)
      return std::nullopt;
    Ranges.push_back(*Range);
  }
  auto SameTry = [&](size_t A, size_t B) {
    return Scopes[A].Kind == Scopes[B].Kind &&
           Scopes[A].FilterOrFinallyVA == Scopes[B].FilterOrFinallyVA &&
           Scopes[A].HandlerVA == Scopes[B].HandlerVA;
  };
  std::vector<Handler> Handlers;
  for (size_t I = 0; I < Scopes.size(); ++I) {
    const bool Guards = Ranges[I].contains(ControlPc);
    if (Guards != Ranges[I].contains(CallPc))
      return std::nullopt;
    if (!Guards)
      continue;
    if (llvm::any_of(llvm::seq<size_t>(0, Scopes.size()), [&](size_t J) {
          return SameTry(I, J) && Ranges[J].contains(TargetPc);
        }))
      break;
    const SEHScopeRecord &Scope = Scopes[I];
    if (Scope.Kind != SEHScopeKind::Finally) {
      if (Scope.HandlerVA == TargetPc)
        break;
      continue;
    }
    if (!Scope.FilterOrFinallyVA || EH.ownsCode(Scope.FilterOrFinallyVA))
      return std::nullopt;
    Handlers.push_back({Scope.FilterOrFinallyVA, static_cast<uint32_t>(I)});
  }
  return Handlers;
}

} // namespace

void LowToMedConverter::modelSEHLocalUnwindExits(MedFunc &Func,
                                                 const LowFunc &Low) {
  if (!Image || !Low.ExceptionMetadata || !Low.ExceptionMetadata->SEH ||
      Func.Blocks.empty() || !callsLowSEHLocalUnwind(Low, *Image))
    return;
  const ExceptionFunction &EH = *Low.ExceptionMetadata;
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const llvm::ArrayRef<uint64_t> ArgRegs = TRI.integerParamRegs(TargetFormat);
  const uint16_t Pointer = static_cast<uint16_t>(TRI.PointerSize);
  if (ArgRegs.size() < 2 || !Pointer)
    return;
  static const std::map<va_t, const LowFunc *> NoBodies;
  size_t Work = limits::kMaxSEHEstablisherFrameWork;
  auto Sites = certifyLowSEHLocalUnwinds(
      Low, *Image, TRI, SEHFrameCallees ? *SEHFrameCallees : NoBodies, Work,
      CallUnreadGPRs);
  if (!Sites) {
    // An uncertified call keeps its runtime form.
    llvm::consumeError(Sites.takeError());
    return;
  }

  std::map<va_t, int> BlockAt;
  for (const MedBlock &Block : Func.Blocks) {
    if (Block.Id < 0 || static_cast<size_t>(Block.Id) >= Func.Blocks.size() ||
        &Func.Blocks[Block.Id] != &Block)
      return;
    BlockAt.emplace(Block.StartAddr, Block.Id);
  }

  // Each certified call this replaces: its block, its position there, the
  // target's block and the handlers the jump runs.  A target the exception
  // runtime enters as well keeps the runtime call: a block is one or the
  // other.
  struct Plan {
    const LowSEHLocalUnwind *Call = nullptr;
    int Site = -1;
    size_t Index = 0;
    int Target = -1;
    std::vector<Handler> Handlers;
  };
  std::vector<Plan> Plans;
  std::map<int, size_t> PlanAt;
  for (const LowSEHLocalUnwind &Call : *Sites) {
    const auto TargetBlock = BlockAt.find(Call.Target);
    std::optional<std::vector<Handler>> Handlers = unwindHandlers(
        EH, TargetArch, Call.CallAddr, Call.ControlPc, Call.Target);
    if (TargetBlock == BlockAt.end() || !Handlers ||
        !Func.Blocks[TargetBlock->second].ExceptionalPreds.empty())
      continue;
    // The runtime call, exactly once.
    Plan P;
    bool Unique = true;
    for (const MedBlock &Block : Func.Blocks)
      for (size_t I = 0; I < Block.Ops.size(); ++I) {
        const MedOp &Op = Block.Ops[I];
        if (Op.Opcode != NdOp::CALL || Op.Addr != Call.CallAddr ||
            Op.OriginSeq != Call.CallSeq)
          continue;
        Unique &= P.Site < 0 && !Op.IsTailCall;
        P.Site = Block.Id;
        P.Index = I;
      }
    if (P.Site < 0 || !Unique)
      continue;
    // A second call in the same block follows one that never returns.
    if (const auto Earlier = PlanAt.find(P.Site); Earlier != PlanAt.end()) {
      if (Plans[Earlier->second].Index < P.Index)
        continue;
      Plans[Earlier->second].Site = -1;
    }
    P.Call = &Call;
    P.Target = TargetBlock->second;
    P.Handlers = std::move(*Handlers);
    PlanAt[P.Site] = Plans.size();
    Plans.push_back(std::move(P));
  }
  std::erase_if(Plans, [](const Plan &P) { return P.Site < 0; });
  if (Plans.empty())
    return;
  PlanAt.clear();
  for (size_t K = 0; K < Plans.size(); ++K)
    PlanAt[Plans[K].Site] = K;

  // What runs: the blocks reached from the entry, from every block the
  // exception runtime or a code address enters, and from any other block no
  // edge enters, with each replaced call's block leading only to its target
  // once \p Cut.
  const size_t N = Func.Blocks.size();
  std::vector<bool> Independent(N, false);
  for (const MedBlock &Block : Func.Blocks)
    Independent[Block.Id] = Block.Id == 0 || Block.Preds.empty() ||
                            !Block.ExceptionalPreds.empty() ||
                            Low.ModuleAnalysisRoots.count(Block.StartAddr);
  auto SuccsOf = [&](int B, bool Cut) -> std::vector<int> {
    if (Cut)
      if (const auto It = PlanAt.find(B); It != PlanAt.end())
        return {Plans[It->second].Target};
    return Func.Blocks[B].Succs;
  };
  auto Reached = [&](bool Cut) {
    std::vector<bool> Seen(N, false);
    std::vector<int> Work;
    for (size_t B = 0; B < N; ++B)
      if (Independent[B])
        Work.push_back(static_cast<int>(B));
    while (!Work.empty()) {
      const int B = Work.back();
      Work.pop_back();
      if (B < 0 || static_cast<size_t>(B) >= N || Seen[B])
        continue;
      Seen[B] = true;
      for (int Succ : SuccsOf(B, Cut))
        Work.push_back(Succ);
      for (const ExceptionalEdge &Edge : Func.Blocks[B].ExceptionalSuccs)
        Work.push_back(Edge.BlockId);
    }
    return Seen;
  };
  const std::vector<bool> Before = Reached(false);
  const std::vector<bool> After = Reached(true);
  std::set<int> Dead;
  for (size_t B = 0; B < N; ++B)
    if (Before[B] && !After[B])
      Dead.insert(static_cast<int>(B));

  // The code after a call that never returned runs no more where nothing
  // else leads to it.  The proofs that read the decoded CFG still see it
  // enter its successors, so a block the runtime or a code address enters,
  // which only that code entered by an edge, would be a root here and not
  // there: such a function keeps its runtime calls.
  // The edges that enter each block from code that runs.
  auto Entering = [&](bool Cut, const std::vector<bool> &Runs) {
    std::vector<std::pair<unsigned, unsigned>> Edges(N);
    for (size_t B = 0; B < N; ++B) {
      if (!Runs[B])
        continue;
      for (int Succ : SuccsOf(static_cast<int>(B), Cut))
        if (Succ >= 0 && static_cast<size_t>(Succ) < N)
          ++Edges[Succ].first;
      for (const ExceptionalEdge &Edge : Func.Blocks[B].ExceptionalSuccs)
        if (Edge.BlockId >= 0 && static_cast<size_t>(Edge.BlockId) < N)
          ++Edges[Edge.BlockId].second;
    }
    return Edges;
  };
  const auto EnteredBefore = Entering(/*Cut=*/false, Before);
  const auto EnteredAfter = Entering(/*Cut=*/true, After);
  for (size_t B = 0; B < N; ++B)
    if (After[B] && Independent[B] &&
        ((EnteredBefore[B].first && !EnteredAfter[B].first) ||
         (EnteredBefore[B].second && !EnteredAfter[B].second)))
      return;

  for (const Plan &P : Plans) {
    const LowSEHLocalUnwind &Call = *P.Call;
    MedBlock &Site = Func.Blocks[P.Site];
    const uint32_t RemovedCallSite = Site.Ops[P.Index].CallSiteId;
    const auto At = [&](MedOp Op) {
      Op.Addr = Call.CallAddr;
      Op.OriginSeq = -1;
      return Op;
    };
    std::vector<MedOp> Ops(Site.Ops.begin(),
                           Site.Ops.begin() + static_cast<ptrdiff_t>(P.Index));
    const NdVar SP = NdVar::reg(TRI.StackPointer, Pointer);
    for (const Handler &H : P.Handlers) {
      // The arguments the LLVM route's cleanup passes too: the flag, then the
      // EstablisherFrame, which the call's own stack pointer locates.
      // Written as `mov ecx, 1` / `mov w0, #1` writes it: the low word, which
      // the full register then zero-extends.
      const uint16_t Word = std::min<uint16_t>(Pointer, 4);
      MedOp Flag;
      Flag.Opcode = NdOp::COPY;
      Flag.Output = ndVarToMedVar(NdVar::reg(ArgRegs[0], Word));
      Flag.addInput(
          MedVar::makeConst(1, Word, ConstantAddressProvenance::Scalar));
      Ops.push_back(At(std::move(Flag)));
      if (Word < Pointer) {
        MedOp Extend;
        Extend.Opcode = NdOp::INT_ZEXT;
        Extend.Output = ndVarToMedVar(NdVar::reg(ArgRegs[0], Pointer));
        Extend.addInput(ndVarToMedVar(NdVar::reg(ArgRegs[0], Word)));
        Ops.push_back(At(std::move(Extend)));
      }
      MedOp Frame;
      Frame.Opcode = Call.HandlerFrameFromSP ? NdOp::INT_ADD : NdOp::COPY;
      Frame.Output = ndVarToMedVar(NdVar::reg(ArgRegs[1], Pointer));
      Frame.addInput(ndVarToMedVar(SP));
      if (Call.HandlerFrameFromSP)
        Frame.addInput(
            MedVar::makeConst(static_cast<uint64_t>(Call.HandlerFrameFromSP),
                              Pointer, ConstantAddressProvenance::Scalar));
      Ops.push_back(At(std::move(Frame)));

      // The handler returns nothing the unwind passes on: the target reads
      // no result of it, so the call defines none.
      LowOp Source;
      Source.Opcode = NdOp::CALL;
      Source.Addr = Call.CallAddr;
      Source.addInput(NdVar::cst(H.Entry, Pointer));
      MedOp Invoke;
      Invoke.Opcode = NdOp::CALL;
      Invoke.CallSiteId = NextCallSiteId++;
      Invoke.addInput(ndVarToMedVar(Source.Inputs[0]));
      Invoke = At(std::move(Invoke));
      RegisterCallSlot = 0;
      CallArgumentsFrom = 0;
      DispatchCallDefinedArgs = 0;
      MedBlock Setup;
      applyCallRegisterEffect(Setup, Invoke, Source, /*TailCall=*/false);
      // A handler that ignores the flag still receives it: an unread slot's
      // placeholder spells the value the runtime passes.
      if (Invoke.NumInputs > 1 && Invoke.Inputs[1].isConst())
        Invoke.Inputs[1].ConstVal = 1;
      for (MedOp &Op : Setup.Ops)
        Ops.push_back(std::move(Op));
      Ops.push_back(std::move(Invoke));
    }
    if (Call.ResumeReturnValue) {
      MedOp Result;
      Result.Opcode = NdOp::COPY;
      Result.Output = ndVarToMedVar(NdVar::reg(TRI.IntReturnReg, Pointer));
      Result.addInput(MedVar::makeConst(*Call.ResumeReturnValue, Pointer,
                                        ConstantAddressProvenance::Scalar));
      Ops.push_back(At(std::move(Result)));
    }
    MedOp Jump;
    Jump.Opcode = NdOp::BRANCH;
    Jump.addInput(ndVarToMedVar(NdVar::cst(Call.Target, Pointer)));
    Ops.push_back(At(std::move(Jump)));
    Site.Ops = std::move(Ops);

    // The call never returns: its block now leads only to the target.
    for (int Succ : Site.Succs)
      std::erase(Func.Blocks[Succ].Preds, Site.Id);
    Site.Succs = {P.Target};
    Func.Blocks[P.Target].Preds.push_back(Site.Id);
    std::erase_if(Func.CallUnreadRegisters, [&](const auto &Entry) {
      return Entry.first == RemovedCallSite;
    });

    MedSEHLocalUnwindExit Exit;
    Exit.CallAddr = Call.CallAddr;
    Exit.CallSeq = Call.CallSeq;
    Exit.Target = Call.Target;
    Exit.TargetFrame = Call.TargetFrame;
    Exit.Handlers = P.Handlers;
    Func.SEHLocalUnwindExits.push_back(std::move(Exit));
  }

  for (MedBlock &Block : Func.Blocks) {
    if (Dead.count(Block.Id)) {
      Block.Ops.clear();
      Block.Succs.clear();
      Block.ExceptionalSuccs.clear();
      Block.Preds.clear();
      Block.ExceptionalPreds.clear();
      continue;
    }
    std::erase_if(Block.Preds, [&](int Id) { return Dead.count(Id); });
    std::erase_if(Block.ExceptionalPreds, [&](const ExceptionalEdge &Edge) {
      return Dead.count(Edge.BlockId);
    });
  }
  // A replaced call in code another one's jump left dead goes with it.
  std::erase_if(
      Func.SEHLocalUnwindExits, [&](const MedSEHLocalUnwindExit &Exit) {
        return llvm::any_of(Plans, [&](const Plan &P) {
          return P.Call->CallAddr == Exit.CallAddr && Dead.count(P.Site);
        });
      });
  std::sort(Func.SEHLocalUnwindExits.begin(), Func.SEHLocalUnwindExits.end(),
            [](const MedSEHLocalUnwindExit &A, const MedSEHLocalUnwindExit &B) {
              return A.CallAddr < B.CallAddr;
            });

  // A target only exits enter is no independent root: every certified call
  // to it is a live exit, only those calls name it, and no relocation does.
  if (!SharedCodePointerTargets && !CodePointerTargets)
    CodePointerTargets = codePointerTargetsOf(*Image);
  const std::vector<va_t> &PointerTargets = SharedCodePointerTargets
                                                ? *SharedCodePointerTargets
                                                : *CodePointerTargets;
  for (MedSEHLocalUnwindExit &Exit : Func.SEHLocalUnwindExits) {
    const va_t Target = Exit.Target;
    Exit.OwnsTarget =
        llvm::all_of(*Sites,
                     [&](const LowSEHLocalUnwind &Call) {
                       return Call.Target != Target ||
                              (Call.OnlyExitsName &&
                               llvm::any_of(Func.SEHLocalUnwindExits,
                                            [&](const auto &Live) {
                                              return Live.CallAddr ==
                                                     Call.CallAddr;
                                            }));
                     }) &&
        !std::binary_search(PointerTargets.begin(), PointerTargets.end(),
                            Target) &&
        llvm::none_of(Image->CodeAddressRelocOperands, [&](const auto &Field) {
          return Field.second.TargetVA == Target;
        });
    if (Exit.OwnsTarget)
      Func.ModuleAnalysisRoots.erase(Target);
  }
}

} // namespace neverd
