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
#include "../X86/SEHFrame.h"
#include "../X86/SEHSourceRoots.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceCallABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/SEHFrameProof.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MedCallLeftovers.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>

#define DEBUG_TYPE "neverd-med-ssa"

namespace neverd {

using x86_seh::proveSEHEstablisherFrame;

namespace {

/// The bytes an ARM64 prologue allocates, by its unwind codes: the frame its
/// body runs in, which the unwinder passes a scope's handler as the
/// establisher frame.  Nullopt when the codes do not fix that size.
std::optional<uint64_t> arm64PrologueAllocation(const ExceptionFunction &EH) {
  using K = UnwindOperationKind;
  if (EH.ParseStatus != ExceptionParseStatus::Complete ||
      EH.Kind != RuntimeFunctionKind::Primary ||
      (EH.Encoding != ExceptionEncoding::ARM64Unpacked &&
       EH.Encoding != ExceptionEncoding::ARM64Packed) ||
      EH.UnwindOperations.empty())
    return std::nullopt;
  uint64_t Bytes = 0;
  for (const UnwindOperation &Op : EH.UnwindOperations) {
    switch (Op.Kind) {
    case K::End:
      return Bytes;
    case K::AllocateStack:
    case K::SaveRegisterPreIndexed:
    case K::SaveRegisterPairPreIndexed:
      if (Op.StackOffset > std::numeric_limits<uint32_t>::max())
        return std::nullopt;
      Bytes += Op.StackOffset;
      break;
    case K::SaveRegister:
    case K::SaveRegisterPair:
    case K::SaveNextPair:
    case K::SetFramePointer:
    case K::AddFramePointer:
    case K::SignReturnAddress:
    case K::Nop:
      break;
    default:
      // A chained scope, an SVE-sized allocation, a stack pointer restored
      // from a register, or a custom frame: the size is not this sum.
      return std::nullopt;
    }
  }
  return Bytes;
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

  // Lowering assigns each call a distinct occurrence ID. Entry seeding may
  // move operations and SSA renames their values, but neither changes a
  // call's preservation contract or the register views collected above.
  // Reuse its complete clobber set across liveness, PHI placement and rename.
  llvm::DenseMap<uint32_t, llvm::SmallVector<int, 8>> CallClobbers;
  auto CallClobberedIds = [&](const MedOp &Op) -> llvm::ArrayRef<int> {
    if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
        Op.PreservesCallerSaved)
      return {};
    auto [Cached, Inserted] = CallClobbers.try_emplace(Op.CallSiteId);
    if (!Inserted)
      return Cached->second;
    auto &Result = Cached->second;
    for (const auto &[RegOff, Ids] : RegOffToIds)
      for (int Id : Ids)
        if (const auto It = RegVarOfId.find(Id);
            It != RegVarOfId.end() &&
            callClobbersRegister(Op, RegOff, It->second.Size, TRI,
                                 TargetFormat))
          Result.push_back(Id);
    llvm::sort(Result);
    Result.erase(std::unique(Result.begin(), Result.end()), Result.end());
    return Result;
  };

  // A Windows x64, ARM or AArch64 SEH handler is entered by the unwinder with
  // the protected frame's nonvolatile registers as they were when the
  // exception was raised. Where no protected block writes one, that is the
  // value live throughout the protected range, not the value the function was
  // entered with.
  std::map<int, std::vector<int>> SEHProtected;
  std::map<int, std::map<int, std::set<va_t>>> CxxProtectedCalls;
  std::set<int> SEHTrackedIds;
  std::set<int> SEHHandlerRoots;
  // Handler roots of a frame-register function that moves SP after its
  // prologue: they resume with the SP live throughout the protected range.
  std::set<int> SEHProtectedSPRoots;
  std::set<int> SharedSourceSPRoots;
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
        // A physical x86 CALL reads SP to write its return slot, even when
        // none of its source arguments mention SP. Keep that use live across
        // handler roots before a later explicit restore overwrites SP. The
        // existing SEH frame proof then supplies the handler's frame value.
        if (TRI.CallPushesReturnAddress &&
            (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)) {
          const auto Bias =
              sourceCallReturnAddressBias(Image, TargetArch, Op.Addr);
          if (!Bias || *Bias != 0)
            if (auto It = RegOffToIds.find(TRI.StackPointer);
                It != RegOffToIds.end())
              for (int Id : It->second) {
                const auto &View = RegVarOfId.at(Id);
                if (IsFullStackPointer(View) && !Kill.count(Id)) {
                  UEVar[B].insert(Id);
                  VarOfId.emplace(Id, View);
                }
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
        llvm::ArrayRef<int> Clobbered = CallClobberedIds(Op);
        Kill.insert(Clobbered.begin(), Clobbered.end());
      }
    }

    // Each live-in fact crosses a predecessor edge only once. The transfer
    // UEVar[B] union (LiveOut[B] - VarKill[B]) distributes over individual
    // variables, so propagating only new facts reaches the same least fixpoint
    // without rebuilding successor unions or revisiting unchanged facts.
    auto LiveIn = std::move(UEVar);
    // Self-edges cannot add another block's uses to a single-block function.
    if (N > 1) {
      std::queue<std::pair<int, int>> Pending;
      for (auto It = RPO.rbegin(); It != RPO.rend(); ++It) {
        const int B = *It;
        if (B == VirtualRoot)
          continue;
        for (int Id : LiveIn[B])
          Pending.emplace(B, Id);
      }
      while (!Pending.empty()) {
        const auto [B, Id] = Pending.front();
        Pending.pop();
        for (int P : FlowPreds[B]) {
          // The synthetic root joins dominance, not machine-entry values.
          if (P != VirtualRoot && !VarKill[P].count(Id) &&
              LiveIn[P].insert(Id).second)
            Pending.emplace(P, Id);
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
                    SEHFrameCallees,
                    Low.SEHLocalUnwindContinuations.front().UsesRuntimeMemory
                        ? SEHRuntimeMemory
                        : nullptr);
      for (const auto &Evidence : Low.SEHLocalUnwindContinuations)
        if (LocalUnwindDigest.empty() ||
            (Evidence.UsesRuntimeMemory && !SEHRuntimeMemory) ||
            Evidence.UsesRuntimeMemory !=
                Low.SEHLocalUnwindContinuations.front().UsesRuntimeMemory ||
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
        bool UnresolvedEntries = false;
        const auto FrameBytes = proveSEHEstablisherFrame(
            Low, Func, Func.Blocks[B].StartAddr, TRI,
            /*OrdinaryEntry=*/!IsRoot[B], LocalUnwindDigest,
            /*CxxContinuation=*/false, nullptr, nullptr,
            SEHSourceProjectionEnabled, &UnresolvedEntries);
        Func.HasUnresolvedSEHFrame |= UnresolvedEntries;
        if (!IsRoot[B]) {
          if (UnresolvedEntries)
            SharedSourceSPRoots.insert(B);
          continue;
        }
        if (FrameBytes)
          SEHFrameOffsets[B] = *FrameBytes;
        else
          SEHProtectedSPRoots.insert(B);
        if (IsLocalUnwindTarget)
          LocalUnwindRoots.insert(B);
      }
    }

    if (Func.HasUnresolvedSEHFrame)
      x86_seh::seedSourceEntries(Func, Low, LiveIn, VarOfId, TargetArch);

    // ARM and AArch64 have no decoded-frame proof here. Their unwinder
    // resumes an except handler with the protected frame's SP as well: the SP
    // live throughout the protected range, where no protected operation
    // moves it.
    const bool ARMTableSEH =
        (TargetArch == Arch::ARM || TargetArch == Arch::AArch64) &&
        Low.ExceptionMetadata && Low.ExceptionMetadata->SEH;
    if (ARMTableSEH)
      for (int Root : Roots)
        if (Root != 0 && llvm::any_of(Func.Blocks[Root].ExceptionalPreds,
                                      [](const ExceptionalEdge &E) {
                                        return E.Kind ==
                                               ExceptionalEdgeKind::SEHHandler;
                                      }))
          SEHProtectedSPRoots.insert(Root);

    for (int Root : Roots) {
      if (Root != 0)
        NonEntryRootLiveIns.insert(LiveIn[Root].begin(), LiveIn[Root].end());
      if (Root != 0 &&
          (TargetArch == Arch::X64 || ARMTableSEH || HasX86SEHFrame) &&
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
                    : RegistrationRoots.restoredStack(Func.Blocks[Root]);
      const auto SEHEntry = Root == 0
                                ? std::nullopt
                                : RegistrationRoots.sehEntry(Func.Blocks[Root]);
      const bool IsWindowsRuntimeRoot =
          SEHEntry.has_value() || CxxContinuationRoots.count(Root) ||
          LocalUnwindRoots.count(Root) ||
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
            SEHEntry.value_or(false) || IsCxxHandlerRoot ||
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

  RegistrationRoots.seedSEHParentFrames(Func);

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

  // The runtime SP of a shared source handler is a separate incoming value.
  // Its ordinary predecessors retain their actual SP; neither side grants a
  // constant offset from the function entry.
  for (int B : SharedSourceSPRoots)
    if (auto It = RegOffToIds.find(TRI.StackPointer); It != RegOffToIds.end())
      for (int Id : It->second) {
        x86_seh::mergeSourceHandlerStack(Func.Blocks[B], RegVarOfId.at(Id),
                                         FlowPreds[B], VirtualRoot, TargetArch);
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
  llvm::DenseMap<int, int> VarCounter;
  llvm::DenseMap<int, std::vector<int>> VarStack;
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

  // ARM and AArch64 termination handlers are separate functions that read
  // this one's callee-saved registers, which the unwinder restores to their
  // values where the exception struck. Track each full-width callee-saved
  // register through the blocks of every finally scope; a handler that ends
  // several scopes needs one value for all of them.
  const bool ARMSEHFunclets =
      (TargetArch == Arch::ARM || TargetArch == Arch::AArch64) &&
      Low.ExceptionMetadata && Low.ExceptionMetadata->SEH;
  std::map<va_t, std::vector<int>> SEHFuncletProtected;
  std::set<int> SEHFuncletTrackedIds;
  std::optional<int64_t> SEHFuncletFrame;
  if (ARMSEHFunclets) {
    const ExceptionFunction &EH = *Low.ExceptionMetadata;
    std::set<va_t> Refused;
    for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
      if (Scope.Kind != SEHScopeKind::Finally || !Scope.FilterOrFinallyVA ||
          EH.ownsCode(Scope.FilterOrFinallyVA))
        continue;
      const va_t Funclet = Scope.FilterOrFinallyVA;
      const auto Range = getSemanticSEHGuardedRange(Scope, TargetArch, EH);
      if (Scope.ParseStatus != ExceptionParseStatus::Complete || !Range) {
        Refused.insert(Funclet);
        continue;
      }
      auto &Blocks = SEHFuncletProtected[Funclet];
      const size_t Before = Blocks.size();
      for (int B = 0; B < N; ++B)
        if (Func.Blocks[B].StartAddr < Range->End &&
            Func.Blocks[B].EndAddr > Range->Begin)
          Blocks.push_back(B);
      if (Blocks.size() == Before)
        Refused.insert(Funclet);
    }
    for (va_t Funclet : Refused)
      SEHFuncletProtected.erase(Funclet);
    // AArch64 also passes the handler the establisher frame, the stack
    // pointer the prologue leaves for the body. Where the protected ranges
    // run on exactly that stack pointer, it is their stack pointer.
    if (TargetArch == Arch::AArch64 && !SEHFuncletProtected.empty())
      if (const auto Bytes = arm64PrologueAllocation(EH))
        SEHFuncletFrame = -static_cast<int64_t>(*Bytes);
    if (!SEHFuncletProtected.empty())
      for (const auto &[Id, V] : RegVarOfId)
        if (V.Size == TRI.PointerSize &&
            (TRI.isStackPointer(V.RegOff)
                 ? SEHFuncletFrame && IsFullStackPointer(V)
                 : TRI.isCalleeSaveReg(V.RegOff) ||
                       V.RegOff == TRI.FramePointer))
          SEHFuncletTrackedIds.insert(Id);
  }
  std::set<int> SEHFuncletBlocks;
  for (const auto &[Funclet, Blocks] : SEHFuncletProtected)
    SEHFuncletBlocks.insert(Blocks.begin(), Blocks.end());
  std::map<int, std::map<int, int>> SEHFuncletEntryVersions;

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
    /// The variables this block pushed a version of, once per push.
    std::vector<int> Pushed;
  };
  std::vector<Frame> Stk;
  Stk.reserve(N);

  // The arguments each block's PHIs take on an edge from each predecessor:
  // (predecessor, PHI, argument) in PHI and argument order.  Renaming does
  // not change a PHI's arguments' predecessors.
  struct PhiEdgeArg {
    int Pred;
    uint32_t Phi;
    uint32_t Arg;
  };
  std::vector<std::vector<PhiEdgeArg>> PhiEdgeArgs(N);
  for (int B = 0; B < N; ++B) {
    const auto &Phis = Func.Blocks[B].Phis;
    for (size_t P = 0; P < Phis.size(); ++P)
      for (size_t A = 0; A < Phis[P].Args.size(); ++A)
        PhiEdgeArgs[B].push_back({Phis[P].Args[A].first,
                                  static_cast<uint32_t>(P),
                                  static_cast<uint32_t>(A)});
    std::stable_sort(PhiEdgeArgs[B].begin(), PhiEdgeArgs[B].end(),
                     [](const PhiEdgeArg &L, const PhiEdgeArg &R) {
                       return L.Pred < R.Pred;
                     });
  }

  auto ProcessBlock = [&](Frame &F) {
    if (F.B == VirtualRoot)
      return;
    if (F.B < 0 || F.B >= N)
      return;
    auto &Blk = Func.Blocks[F.B];
    for (auto &Phi : Blk.Phis) {
      Phi.Output.SSAVer = NewVersion(Phi.Output.Id);
      F.Pushed.push_back(Phi.Output.Id);
    }
    if (SEHProtectedBlocks.count(F.B))
      for (int Id : SEHTrackedIds)
        SEHEntryVersions[F.B][Id] = GetVersion(Id);
    // -1 when no definition reaches here: version 0 is a register's first
    // definition unless it is live into the function.
    if (SEHFuncletBlocks.count(F.B))
      for (int Id : SEHFuncletTrackedIds)
        SEHFuncletEntryVersions[F.B][Id] =
            VarStack[Id].empty() ? -1 : VarStack[Id].back();
    for (auto &Op : Blk.Ops) {
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          CxxThrowingCalls.count({F.B, Op.Addr}))
        for (int Id : SEHTrackedIds)
          CxxCallVersions[{F.B, Op.Addr}][Id] = GetVersion(Id);
      const bool RuntimeCoordinate = hasValidRegistrationRootShape(Op);
      for (uint8_t I = 0; I < Op.NumInputs; ++I) {
        // A runtime coordinate defines a new frame value independently of an
        // incoming machine register, including at a resumed parent block.
        if (Op.Inputs[I].Id >= 0 && !RuntimeCoordinate)
          Op.Inputs[I].SSAVer = GetVersion(Op.Inputs[I].Id);
      }
      if (Op.Output.Id >= 0 && Op.Output.Size > 0) {
        Op.Output.SSAVer = NewVersion(Op.Output.Id);
        F.Pushed.push_back(Op.Output.Id);
        if (Func.HasUnresolvedSEHFrame && Op.Addr == InvalidVA &&
            SEHProtectedBlocks.count(F.B) && SEHTrackedIds.count(Op.Output.Id))
          SEHEntryVersions[F.B][Op.Output.Id] = Op.Output.SSAVer;
      }
      for (MedVar &Aux : Op.IntrinsicOutputs) {
        if (Aux.Id < 0 || Aux.Size == 0)
          continue;
        Aux.SSAVer = NewVersion(Aux.Id);
        F.Pushed.push_back(Aux.Id);
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
        F.Pushed.push_back(Id);
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
      const auto &EdgeArgs = PhiEdgeArgs[S];
      auto First = std::lower_bound(
          EdgeArgs.begin(), EdgeArgs.end(), F.B,
          [](const PhiEdgeArg &E, int Pred) { return E.Pred < Pred; });
      for (auto It = First; It != EdgeArgs.end() && It->Pred == F.B; ++It) {
        MedVar &Arg = Func.Blocks[S].Phis[It->Phi].Args[It->Arg].second;
        // A PHI can be placed for a register that no instruction reads on
        // this edge. With no reaching definition, version zero may name a
        // sibling's first write; publishing it would let propagation borrow
        // that sibling's constant for this path.
        if (VarStack[Arg.Id].empty())
          Arg = MedVar::makeUnspecified(Arg.Size, TargetArch);
        else
          Arg.SSAVer = GetVersion(Arg.Id);
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
      for (int VId : F.Pushed)
        VarStack[VId].pop_back();
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
  // Publish each termination handler's callee-saved register values. Each is
  // resolved here to a form later passes neither rename nor delete, through a
  // bounded chain of COPY and constant INT_ADD/INT_SUB definitions: no PHI web
  // is walked, and each SSA value is resolved once.
  if (!SEHFuncletProtected.empty()) {
    // Every definition, so version 0 with none is the entry value. A PHI or
    // an auxiliary output is a definition the trace does not follow.
    std::map<std::pair<int, int>, const MedOp *> Defs;
    for (const MedBlock &Blk : Func.Blocks) {
      for (const PhiNode &Phi : Blk.Phis)
        if (Phi.Output.Id >= 0)
          Defs.emplace(std::make_pair(Phi.Output.Id, Phi.Output.SSAVer),
                       nullptr);
      for (const MedOp &Op : Blk.Ops) {
        if (Op.Output.Id >= 0 && Op.Output.Size > 0)
          Defs.emplace(std::make_pair(Op.Output.Id, Op.Output.SSAVer), &Op);
        for (const MedVar &Aux : Op.IntrinsicOutputs)
          if (Aux.Id >= 0)
            Defs.emplace(std::make_pair(Aux.Id, Aux.SSAVer), nullptr);
      }
    }
    const IntegerArgumentLayout Arguments =
        TRI.integerArgumentLayout(TargetFormat);
    std::map<std::pair<int, int>, std::optional<SEHFuncletRegisterValue>>
        Resolved;
    auto Trace = [&](MedVar V) -> std::optional<SEHFuncletRegisterValue> {
      using Kind = SEHFuncletRegisterValue::Kind;
      int64_t Acc = 0;
      for (unsigned Hop = 0;
           Hop < limits::ir::kMedSSASEHFuncletRegisterChainLimit; ++Hop) {
        SEHFuncletRegisterValue Value;
        if (V.Kind == MedVar::Const) {
          // An offset from an address constant names no address by itself.
          const bool Scalar =
              V.Provenance == ConstantAddressProvenance::Unknown ||
              V.Provenance == ConstantAddressProvenance::Scalar;
          if (Acc && !Scalar)
            return std::nullopt;
          Value.ValueKind = Kind::Constant;
          Value.Value = V.ConstVal + static_cast<uint64_t>(Acc);
          if (TRI.PointerSize < 8)
            Value.Value &= (uint64_t(1) << (TRI.PointerSize * 8)) - 1;
          if (!Acc) {
            Value.Provenance = V.Provenance;
            Value.AddressOwnerVA = V.AddressOwnerVA;
          }
          return Value;
        }
        // A register's value on entry is its version 0 where it is live in,
        // defined by the entry block's self-copy; elsewhere version 0 is its
        // first definition, which is followed like any other.
        const auto Def = Defs.find({V.Id, V.SSAVer});
        auto SelfCopy = [&](const MedOp &Op) {
          return Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                 Op.Inputs[0].Kind == MedVar::Reg && Op.Inputs[0].Id == V.Id &&
                 Op.Inputs[0].SSAVer == 0;
        };
        if (V.Kind == MedVar::Reg && V.SSAVer == 0 &&
            (Def == Defs.end() || (Def->second && SelfCopy(*Def->second)))) {
          if (V.Size != TRI.PointerSize)
            return std::nullopt;
          if (TRI.isStackPointer(V.RegOff)) {
            Value.ValueKind = Kind::EntryStackOffset;
            Value.Offset = Acc;
            return Value;
          }
          if (TRI.ZeroRegister && V.RegOff == *TRI.ZeroRegister) {
            Value.ValueKind = Kind::Constant;
            Value.Value = static_cast<uint64_t>(Acc);
            Value.Provenance = ConstantAddressProvenance::Scalar;
            return Value;
          }
          if (Acc || Arguments.registerIndex(V.RegOff) < 0)
            return std::nullopt;
          Value.ValueKind = Kind::EntryArgument;
          Value.RegOff = V.RegOff;
          return Value;
        }
        if (Def == Defs.end() || !Def->second)
          return std::nullopt;
        const MedOp &Op = *Def->second;
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
          V = Op.Inputs[0];
          continue;
        }
        if ((Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB) ||
            Op.NumInputs != 2 || Op.Output.Size != TRI.PointerSize)
          return std::nullopt;
        const bool ConstantRight = Op.Inputs[1].isConst();
        if (!ConstantRight &&
            (Op.Opcode != NdOp::INT_ADD || !Op.Inputs[0].isConst()))
          return std::nullopt;
        const MedVar &Step = ConstantRight ? Op.Inputs[1] : Op.Inputs[0];
        if (!Step.Size || Step.Size > 8)
          return std::nullopt;
        int64_t Delta = llvm::SignExtend64(Step.ConstVal, Step.Size * 8);
        if (Op.Opcode == NdOp::INT_SUB &&
            Delta == std::numeric_limits<int64_t>::min())
          return std::nullopt;
        if (Op.Opcode == NdOp::INT_SUB)
          Delta = -Delta;
        if (llvm::AddOverflow(Acc, Delta, Acc))
          return std::nullopt;
        V = ConstantRight ? Op.Inputs[0] : Op.Inputs[1];
      }
      return std::nullopt;
    };
    auto Resolve = [&](const MedVar &V) {
      auto [It, Inserted] =
          Resolved.try_emplace(std::make_pair(V.Id, V.SSAVer));
      if (Inserted)
        It->second = Trace(V);
      return It->second;
    };
    for (const auto &[Funclet, Blocks] : SEHFuncletProtected) {
      // A PHI in a protected block is not among its operations, but it is
      // renamed before the block's entry versions are recorded, so blocks
      // that join different values record different versions.
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
      for (int Id : SEHFuncletTrackedIds) {
        const MedVar &V = RegVarOfId.at(Id);
        if (llvm::any_of(Written, [&](const auto &W) {
              return W.first < V.RegOff + V.Size && V.RegOff < W.second;
            }))
          continue;
        std::optional<int> Live;
        bool Same = true;
        for (int B : Blocks) {
          const auto &Versions = SEHFuncletEntryVersions[B];
          const auto It = Versions.find(Id);
          if (It == Versions.end() || (Live && *Live != It->second)) {
            Same = false;
            break;
          }
          Live = It->second;
        }
        if (!Same || !Live || *Live < 0)
          continue;
        MedVar At = V;
        At.SSAVer = *Live;
        const auto Value = Resolve(At);
        if (!Value)
          continue;
        // The stack pointer is published only as the establisher frame.
        if (TRI.isStackPointer(V.RegOff) &&
            (Value->ValueKind !=
                 SEHFuncletRegisterValue::Kind::EntryStackOffset ||
             Value->Offset != *SEHFuncletFrame))
          continue;
        Func.SEHFuncletRegisters[Funclet][V.RegOff] = *Value;
      }
      // By one reading the establisher frame is the body's stack pointer, by
      // another the frame pointer: clang-cl's handlers reach a parent slot at
      // `x29 - 8` as `x1 - 8`. Keep it only where the ranges run with the
      // frame pointer at that same frame, so both readings agree.
      if (auto Registers = Func.SEHFuncletRegisters.find(Funclet);
          Registers != Func.SEHFuncletRegisters.end()) {
        auto &Values = Registers->second;
        const auto SP = Values.find(TRI.StackPointer);
        const auto FP = Values.find(TRI.FramePointer);
        if (SP != Values.end() &&
            (FP == Values.end() || FP->second != SP->second))
          Values.erase(SP);
        if (Values.empty())
          Func.SEHFuncletRegisters.erase(Registers);
      }
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
