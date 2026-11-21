//===- MedMIPSReturnTargets.cpp - Exact entry link identity --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind source return targets to captured incoming RA through complete SSA.
//===----------------------------------------------------------------------===//

#include "../../../../safety/StackSlotFlow.h"
#include "../../abi/MedABIPassDetail.h"
#include "MedMIPSRecordFrame.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MIPS/MIPSIntrinsicShapes.h"
#include "neverd/ir/med/MIPS/MIPSReturnTargets.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace neverd {

namespace {

/// Incoming loop edges can assume the previous value of a header PHI only
/// when every path to their predecessor has already visited the header.
std::set<int> loopPredecessors(const MedFunc &F, const MedBlock &Header,
                               size_t &Remaining) {
  if (F.Blocks.size() > Remaining)
    return {};
  Remaining -= F.Blocks.size();
  enum : unsigned { FromEntry = 1, FromHeader = 2 };
  std::vector<unsigned> Reached(F.Blocks.size());
  llvm::SmallVector<int, 8> Work;
  auto visit = [&](int Id, unsigned Paths) {
    if ((Reached[Id] | Paths) != Reached[Id]) {
      Reached[Id] |= Paths;
      Work.push_back(Id);
    }
  };
  visit(0, FromEntry);
  visit(Header.Id, FromHeader);
  while (!Work.empty()) {
    const int Id = Work.pop_back_val();
    const auto &Block = F.Blocks[Id];
    if (Block.Id != Id || Remaining < 1 + Block.Succs.size())
      return {};
    Remaining -= 1 + Block.Succs.size();
    for (int Succ : Block.Succs) {
      if (Succ < 0 || size_t(Succ) >= F.Blocks.size())
        return {};
      if (Succ != Header.Id)
        visit(Succ, Reached[Id]);
    }
  }
  std::set<int> Back;
  for (int Pred : Header.Preds) {
    if (Pred < 0 || size_t(Pred) >= Reached.size() || !Reached[Pred])
      return {};
    if (Reached[Pred] == FromHeader)
      Back.insert(Pred);
  }
  return Back.size() != Header.Preds.size() ? Back : std::set<int>{};
}

using NativeCallSites =
    llvm::SmallDenseMap<const MedBlock *, llvm::SmallVector<size_t, 1>, 4>;

/// The input with exactly the output's bits, including the ordinary MIPS
/// register-move aliases. A word operation on an n32 GPR is not an identity.
const MedVar *copiedWord(const MedOp &Op) {
  const MedVar *Input = nullptr;
  if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
    Input = &Op.Inputs[0];
  else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_OR ||
            Op.Opcode == NdOp::INT_XOR) &&
           Op.NumInputs == 2) {
    for (unsigned I : {0u, 1u})
      if (Op.Inputs[I].isConst() && Op.Inputs[I].ConstVal == 0) {
        Input = &Op.Inputs[1 - I];
        break;
      }
  }
  return Input && Input->Size == Op.Output.Size ? Input : nullptr;
}

struct GuardedCall {
  const MedOp *Producer = nullptr;
  uint32_t Registers = 0;
};
using GuardedCalls = llvm::SmallDenseMap<const MedOp *, GuardedCall, 4>;

bool copiesCallResult(MedVar V, const MedOp &Producer,
                      MedDefSiteIndex &Definitions, size_t &Remaining) {
  for (unsigned Depth = 0; Remaining && !V.isConst() &&
                           Depth < limits::ir::kMIPSReturnIdentityDepth;
       ++Depth) {
    --Remaining;
    const auto D = Definitions.find(V);
    if (D.Ambiguous || !D.Op || V.Size != Producer.Output.Size ||
        D.Op->Output.Size != V.Size || D.Op->Output != V)
      break;
    if (D.Op == &Producer)
      return true;
    const auto *Source = copiedWord(*D.Op);
    if (!Source || *Source == V)
      break;
    V = *Source;
  }
  return false;
}

/// A native call's result is still opaque. On the equality arm alone its
/// actual word equals the other operand of the comparison. Qualify copies
/// reaching the next call on that arm, including a move in the branch slot.
GuardedCalls
guardedResultArguments(const MedFunc &F, const NativeCallSites &Native,
                       const TargetRegInfo &TRI, BinaryFormat Format,
                       const safety::detail::ExactPhiGraph &Graph,
                       MedDefSiteIndex &Definitions, size_t &Remaining,
                       llvm::function_ref<bool(const MedVar &)> IsNonFrame) {
  GuardedCalls Proven;
  const auto GPRs = TRI.integerArgumentLayout(Format);
  const auto IsCall = [](const MedOp &O) {
    return O.Opcode == NdOp::CALL || O.Opcode == NdOp::INDIR_CALL;
  };
  for (const auto &[Arm, Sites] : Native) {
    if (!Remaining)
      return GuardedCalls{};
    --Remaining;
    if (Arm->Role != MedBlockRole::Machine || Arm->isFunctionEntry(F.Entry) ||
        Arm->Preds.size() != 1 || !Graph.hasCompleteNormalPredecessors(*Arm))
      continue;
    const auto *Pred = Graph.blockById(Arm->Preds.front());
    const auto Prev = Native.find(Pred);
    if (Prev == Native.end() || Pred->Role != MedBlockRole::Machine ||
        Pred->Ops.empty() || Pred->Succs.size() != 2 ||
        !Graph.hasCompleteNormalPredecessors(*Pred))
      continue;
    const auto &Br = Pred->Ops.back();
    if (Br.Opcode != NdOp::COND_BR || Br.NumInputs != 2 ||
        !Br.Inputs[0].isConst() || Br.Inputs[1].Size != 1)
      continue;
    const auto *Other = Graph.blockById(
        Pred->Succs[0] == Arm->Id ? Pred->Succs[1] : Pred->Succs[0]);
    const bool Taken = Br.Inputs[0].ConstVal == Arm->StartAddr;
    if (!Other || Other == Arm ||
        (Br.Inputs[0].ConstVal == Other->StartAddr) == Taken)
      continue;
    const auto Cond = Definitions.find(Br.Inputs[1]);
    const size_t Start = Prev->second.back();
    const auto &Producer = Pred->Ops[Start];
    if (Cond.Ambiguous || !Cond.Op || Cond.Block != Pred ||
        Cond.Op <= &Producer || Cond.Op->NumInputs != 2 ||
        Cond.Op->Output.Size != Br.Inputs[1].Size ||
        Cond.Op->Opcode != (Taken ? NdOp::INT_EQUAL : NdOp::INT_NOTEQUAL) ||
        Producer.Output.Kind != MedVar::Reg ||
        Producer.Output.RegOff != TRI.IntReturnReg ||
        Producer.Output.Size != TRI.FullRegWidth ||
        Cond.Op->Inputs[0].Size != TRI.FullRegWidth ||
        Cond.Op->Inputs[1].Size != TRI.FullRegWidth)
      continue;
    const auto Prefix = llvm::ArrayRef(Arm->Ops).take_front(Sites.front());
    const auto Suffix = llvm::ArrayRef(Pred->Ops).drop_front(Start + 1);
    if (Remaining < Pred->Ops.size() + Prefix.size())
      return GuardedCalls{};
    Remaining -= Pred->Ops.size() + Prefix.size();
    if (llvm::any_of(Suffix, IsCall) || llvm::any_of(Prefix, IsCall))
      continue;

    // Before ABI clobbers are materialized, an old SSA name in a volatile
    // register is not evidence of its bits after the producer. A literal
    // established afterwards, or a non-frame word saved before the first
    // call at entry in an ABI-preserved register, has independent provenance.
    const auto PeerIsNonFrame = [&](MedVar V) {
      while (Remaining && !V.isConst()) {
        --Remaining;
        const auto D = Definitions.find(V);
        if (D.Ambiguous || !D.Op || D.Block != Pred || D.Op >= Cond.Op ||
            D.Op->Output.Size != V.Size || D.Op->Output != V)
          return false;
        if (D.Op <= &Producer)
          return V.Kind == MedVar::Reg &&
                 TRI.isCallPreserved(V.RegOff, V.Size) &&
                 Pred->isFunctionEntry(F.Entry) && Pred->Preds.empty() &&
                 llvm::none_of(llvm::ArrayRef(Pred->Ops).take_front(Start),
                               IsCall) &&
                 IsNonFrame(V);
        const auto *Source = copiedWord(*D.Op);
        if (!Source || *Source == V)
          return false;
        V = *Source;
      }
      return V.isConst();
    };
    const auto &A = Cond.Op->Inputs[0];
    const auto &B = Cond.Op->Inputs[1];
    if (!(copiesCallResult(A, Producer, Definitions, Remaining) &&
          PeerIsNonFrame(B)) &&
        !(copiesCallResult(B, Producer, Definitions, Remaining) &&
          PeerIsNonFrame(A)))
      continue;

    GuardedCall &Fact = Proven[&Arm->Ops[Sites.front()]];
    Fact.Producer = &Producer;
    // Only the most recent complete physical write may discharge a register
    // argument. Earlier aliases, partial writes and other arms remain live.
    uint32_t Pending = (uint32_t{1} << GPRs.Registers.size()) - 1;
    const auto Observe = [&](const MedVar &V, const MedVar *Source) {
      if (V.Kind != MedVar::Reg || !V.Size)
        return;
      for (unsigned I = 0; I < GPRs.Registers.size(); ++I) {
        const uint32_t Bit = uint32_t{1} << I;
        const auto Reg = GPRs.Registers[I];
        if (!(Pending & Bit) || V.RegOff >= Reg + TRI.FullRegWidth ||
            V.RegOff + V.Size <= Reg)
          continue;
        Pending &= ~Bit;
        if (Source && Reg == V.RegOff && V.Size == TRI.FullRegWidth &&
            copiesCallResult(*Source, Producer, Definitions, Remaining))
          Fact.Registers |= Bit;
      }
    };
    for (const auto Ops : {Prefix, Suffix})
      for (const auto &O : llvm::reverse(Ops)) {
        for (const auto &V : O.IntrinsicOutputs)
          Observe(V, nullptr);
        Observe(O.Output, copiedWord(O));
      }
  }
  return Proven;
}

/// Follow the physical arguments observed at each unbound call back to their
/// reaching writes. At a join any successor may observe an incoming value.
bool nativeArgumentsAreNonFrame(
    const NativeCallSites &NativeCalls, const GuardedCalls &Guarded,
    const TargetRegInfo &TRI, BinaryFormat Format,
    const safety::detail::ExactPhiGraph &Graph, size_t &Remaining,
    llvm::function_ref<bool(const MedVar &)> IsNonFrame) {
  if (NativeCalls.empty())
    return true;
  // One bit per physical argument register, at most eight GPRs and eight
  // FPRs. A full write kills the previous value on this path. A partial
  // write leaves earlier bytes observable, as does an unmodified branch.
  llvm::SmallVector<std::pair<uint64_t, uint16_t>, 16> Arguments;
  for (uint64_t Reg : TRI.integerArgumentLayout(Format).Registers)
    Arguments.emplace_back(Reg, TRI.FullRegWidth);
  for (uint64_t Reg : TRI.floatingParamRegs(Format))
    Arguments.emplace_back(Reg, TRI.FPABIRegWidth);
  const uint32_t All = (uint32_t{1} << Arguments.size()) - 1;
  llvm::SmallDenseMap<const MedBlock *, uint32_t, 4> LiveOut;
  llvm::SmallSetVector<const MedBlock *, 8> Pending;
  for (const auto &Call : NativeCalls)
    Pending.insert(Call.first);
  while (!Pending.empty()) {
    const auto *Block = Pending.pop_back_val();
    if (!Graph.hasCompleteNormalPredecessors(*Block))
      return false;
    uint32_t Live = LiveOut.lookup(Block);
    llvm::ArrayRef<size_t> Sites;
    if (const auto It = NativeCalls.find(Block); It != NativeCalls.end())
      Sites = It->second;
    const auto Check = [&](const MedVar &V, uint32_t *Before) {
      if (V.Kind != MedVar::Reg || !V.Size)
        return true;
      for (unsigned I = 0; I < Arguments.size(); ++I) {
        const auto [Reg, Size] = Arguments[I];
        const uint32_t Bit = uint32_t{1} << I;
        if (!(Live & Bit) || V.RegOff >= Reg + Size || V.RegOff + V.Size <= Reg)
          continue;
        if (!IsNonFrame(V))
          return false;
        if (Before && V.RegOff <= Reg && V.RegOff + V.Size >= Reg + Size)
          *Before &= ~Bit;
      }
      return true;
    };
    if (Remaining < 1 + Block->Preds.size())
      return false;
    Remaining -= 1 + Block->Preds.size();
    size_t I = Block->Ops.size();
    while (I) {
      if (!Live) {
        if (Sites.empty())
          break;
        I = Sites.back() + 1;
      }
      if (!Remaining)
        return false;
      --Remaining;
      const auto &Op = Block->Ops[--I];
      uint32_t Before = Live;
      if (Live && (!Check(Op.Output, &Before) ||
                   !llvm::all_of(Op.IntrinsicOutputs, [&](const MedVar &V) {
                     return Check(V, &Before);
                   })))
        return false;
      if (!Sites.empty() && I == Sites.back()) {
        Live = Before | (All & ~Guarded.lookup(&Op).Registers);
        Sites = Sites.drop_back();
      } else {
        Live = Before;
      }
    }
    if (!Live)
      continue;
    // A PHI describes the incoming register. Only a physical write in a
    // predecessor can discard bytes that still reach the block entry.
    for (const auto &Phi : Block->Phis)
      if (!Check(Phi.Output, nullptr))
        return false;
    for (int Id : Block->Preds) {
      const auto *Pred = Graph.blockById(Id);
      uint32_t &Out = LiveOut[Pred];
      if (Live & ~Out) {
        Out |= Live;
        Pending.insert(Pred);
      }
    }
  }
  return true;
}

} // namespace

bool hasMIPSSourceReturnTargets(const LowFunc &Low, const MedFunc &Function,
                                Arch Target, BinaryFormat Format,
                                unsigned SourceResultOperands) {
  if (auto Error = validateLowInstructionBoundaries(
          Low, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return false;
  }
  if (Function.SkippedSSA || !Low.hasCompleteLiftCoverage() ||
      Low.ModuleAnalysisRoots.size() > 1 ||
      Low.OrdinaryModuleAnalysisRoots.size() > 1)
    return false;
  using Site = std::pair<va_t, int>;
  std::set<Site> Required;
  std::map<Site, Site> TailReturns;
  for (const auto &Block : Low.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries) {
      if (Boundary.Control == LowInstructionControl::TailCall) {
        // CFG lowering appends a synthetic result return. Its value is not
        // the physical link. First establish a complete, unframed leaf tail:
        // no earlier call, memory, opaque effect, or write of RA/SP can have
        // changed the incoming return context before the terminal transfer.
        if (Low.Blocks.size() != 1 || Boundary.OpCount < 2 ||
            Boundary.FirstOp + Boundary.OpCount != Block.Ops.size())
          return false;
        const auto &Call = Block.Ops[Block.Ops.size() - 2];
        const auto &Return = Block.Ops.back();
        if ((Call.Opcode != NdOp::CALL && Call.Opcode != NdOp::INDIR_CALL) ||
            Return.Opcode != NdOp::RETURN)
          return false;
        const unsigned Width = isMIPS64(Target) ? 8 : 4;
        for (size_t I = 0; I < Block.Ops.size(); ++I) {
          const auto &Op = Block.Ops[I];
          if (Op.Output.isReg() && Op.Output.Size)
            for (uint64_t Register : {mipsreg::RA, mipsreg::SP})
              if (Op.Output.Offset < Register + Width &&
                  Register < Op.Output.Offset + Op.Output.Size)
                return false;
          if (I + 2 >= Block.Ops.size())
            continue;
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
              Op.Opcode == NdOp::RETURN || Op.Opcode == NdOp::BRANCH ||
              Op.Opcode == NdOp::COND_BR || Op.Opcode == NdOp::INDIR_BR ||
              Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE ||
              Op.Opcode == NdOp::INTRINSIC)
            return false;
        }
        TailReturns.emplace(Site{Return.Addr, Return.Seq},
                            Site{Call.Addr, Call.Seq});
      } else {
        for (size_t I = Boundary.FirstOp;
             I < Boundary.FirstOp + Boundary.OpCount; ++I)
          if (const auto &Op = Block.Ops[I]; Op.Opcode == NdOp::RETURN)
            Required.emplace(Op.Addr, Op.Seq);
      }
    }
  if (Required.empty() && TailReturns.empty())
    return true;
  std::set<Site> QualifiedTails;
  using Key = std::tuple<int, int, int, uint64_t, uint16_t>;
  const auto key = [](const MedVar &V) -> Key {
    return {int(V.Kind), V.Id, V.SSAVer, V.RegOff, V.Size};
  };
  std::vector<MedVar> MemoryAddresses;
  std::map<const MedBlock *, int> BlockIndices;
  bool QualifiedMemory = true;
  bool HasMergeStores = false;
  size_t CallCount = 0;
  NativeCallSites NativeCalls;
  struct CallFrame {
    llvm::SmallVector<SourceABIValueLocation, 8> Inputs;
    unsigned NumericResultBytes = 0;
  };
  std::map<const MedOp *, CallFrame> Calls;
  size_t Remaining = limits::ir::kSourceFrameAnalysisRemaining;
  for (size_t I = 0; I < Function.Blocks.size(); ++I) {
    const auto &Block = Function.Blocks[I];
    if (!Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty())
      return false;
    BlockIndices.emplace(&Block, I);
    for (const auto &Op : Block.Ops) {
      const bool NoDataWrite =
          Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
          Op.Inputs[0].isConst() &&
          (Op.Inputs[0].ConstVal == uint64_t(Intrinsic::MIPSRequireWord) ||
           mips::mergeLoad(static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) ||
           mips::isBarrier(static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) ||
           mips::isDSPControlIntrinsic(
               static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) ||
           mips::isPureScalarValue(
               static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) ||
           mips::isScalarFPArithmetic(
               static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) ||
           mips::isScalarFPControl(
               static_cast<Intrinsic>(Op.Inputs[0].ConstVal))) &&
          mipsScalarMedShapeIsValid(Op, Target);
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) && Op.NumInputs)
        MemoryAddresses.push_back(Op.Inputs[0]);
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        ++CallCount;
        if (Op.SourceCallHint && Op.SourceCallHint->Signature.MIPSRecords) {
          const auto &Signature = Op.SourceCallHint->Signature;
          const auto Pieces = sourceABIParameters(Signature);
          const bool Hidden = Signature.ReturnLocation.Kind ==
                              SourceABICarrierKind::IndirectResultPointer;
          if (Op.NumInputs != Pieces.size() + 1 + unsigned(Hidden))
            return false;
          auto &Frame = Calls[&Op];
          for (const auto &P : Pieces)
            Frame.Inputs.push_back(P.Location);
          if (Hidden)
            Frame.Inputs.push_back(Signature.ReturnLocation);
          // A complete scalar-only object returned by a qualified call can
          // be spilled as data after its non-frame operands are proven. A
          // pointer field or padding cannot provide that byte provenance.
          const auto Members = sourceValueMembers(Signature.ReturnType);
          unsigned Covered = 0;
          for (const auto &M : Members) {
            if (M.ByteOffset != Covered || (M.Type->Kind != NdTypeKind::Int &&
                                            M.Type->Kind != NdTypeKind::Float))
              break;
            Covered += M.Type->Size;
          }
          if (Signature.ReturnType->Kind == NdTypeKind::Float ||
              ((Hidden || !Signature.ReturnComponents.empty()) &&
               Covered == Signature.ReturnType->Size))
            Frame.NumericResultBytes = Signature.ReturnType->Size;
        } else if (const auto Signature =
                       boundFloatingCallArguments(Op, Target, Format)) {
          auto &Frame = Calls[&Op];
          for (const auto &P : Signature->Scalars)
            Frame.Inputs.push_back(
                {P.Kind, P.Register, P.StackOffset, P.Bytes});
          if (Signature->ReturnKind == NdTypeKind::Float)
            Frame.NumericResultBytes = Signature->ReturnBytes;
        } else {
          NativeCalls[&Block].push_back(&Op - Block.Ops.data());
        }
        if (Op.IsTailCall && Calls.contains(&Op))
          QualifiedTails.emplace(Op.Addr, Op.OriginSeq);
      }
      if (Op.Opcode == NdOp::INTRINSIC && !NoDataWrite &&
          (Op.NumInputs != 3 || !Op.Inputs[0].isConst() ||
           Op.Inputs[0].ConstVal != uint64_t(Intrinsic::RequireAligned))) {
        if (Op.NumInputs && Op.Inputs[0].isConst() &&
            mips::mergeStore(static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) &&
            mipsScalarMedShapeIsValid(Op, Target))
          HasMergeStores = true;
        else
          QualifiedMemory = false;
      }
      if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Op.MemoryOrdering != NdMemoryOrdering::None)
        QualifiedMemory = false;
    }
  }
  const bool HasCalls = CallCount != 0;
  const auto CallSP = HasCalls && QualifiedMemory
                          ? sourceCallStackPointers(Function, Target, Remaining)
                          : std::map<const MedOp *, MedVar>{};
  for (const auto &[Call, SP] : CallSP)
    MemoryAddresses.push_back(SP);
  std::map<Key, std::optional<int64_t>> Offsets;
  if (QualifiedMemory) {
    const auto Values =
        entryStackOffsets(Function, MemoryAddresses, Target, Format);
    for (size_t I = 0; I < Values.size(); ++I)
      Offsets.emplace(key(MemoryAddresses[I]), Values[I]);
  }
  const auto Offset = [&](const MedVar &V) -> std::optional<int64_t> {
    const auto It = Offsets.find(key(V));
    return It == Offsets.end() ? std::nullopt : It->second;
  };
  MedDefSiteIndex Definitions(Function);
  safety::detail::ExactPhiGraph Graph(Function);
  // Only the byte-by-byte record walk tracks the addresses and register
  // lanes of a merge store. The whole-slot reaching-store proof cannot.
  const bool RecordFrameMemory = QualifiedMemory;
  QualifiedMemory &= !HasMergeStores;
  const auto BeforeFrameEscape = [&](const MedBlock &LoadBlock,
                                     size_t LoadIndex) {
    // An external read cannot recover this function's frame address before
    // any operation publishes it. Private stack saves are harmless here;
    // external stores and calls need a stronger memory contract. Visit every
    // predecessor, including a loop's later operations on its back edge.
    llvm::SmallPtrSet<const MedBlock *, 16> Seen;
    std::vector<std::pair<const MedBlock *, size_t>> Pending{
        {&LoadBlock, LoadIndex}};
    bool ReachesEntry = false;
    while (!Pending.empty()) {
      const auto [Block, End] = Pending.back();
      Pending.pop_back();
      if (End == Block->Ops.size() && !Seen.insert(Block).second)
        continue;
      if (!Graph.hasCompleteNormalPredecessors(*Block))
        return false;
      for (size_t I = 0; I < End; ++I) {
        if (!Remaining)
          return false;
        --Remaining;
        const auto &Op = Block->Ops[I];
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
          return false;
        if (Op.Opcode != NdOp::STORE)
          continue;
        const auto *Address = safety::detail::memoryAddress(Op);
        const auto *Value = safety::detail::storedValue(Op);
        const auto At = Address ? Offset(*Address) : std::nullopt;
        if (!Value || !At || *At >= 0 ||
            *At < -limits::ir::kSourceFrameAnalysisMaxFrame ||
            *At + Value->Size > 0)
          return false;
      }
      if (Block->isFunctionEntry(Function.Entry))
        ReachesEntry = true;
      else if (Block->Preds.empty())
        return false;
      for (int Id : Block->Preds) {
        const auto *Pred = Graph.blockById(Id);
        Pending.emplace_back(Pred, Pred->Ops.size());
      }
    }
    return ReachesEntry;
  };
  std::map<Key, bool> Results;
  std::set<Key> Active;
  const auto &TRI = getTargetRegInfo(Target);
  using SliceKey = std::tuple<Key, unsigned, unsigned>;
  std::set<SliceKey> ProvenNonFrame;
  std::set<std::pair<SliceKey, bool>> NonFrameActive;
  std::optional<Key> AssumedPhi;
  std::map<int, std::set<int>> LoopEdges;
  size_t Cycles = 0;
  std::function<bool(const MedVar &, unsigned, unsigned)> NonFrameBytes;
  const auto IsNonFrame = [&](const MedVar &V) {
    return NonFrameBytes(V, 0, V.Size);
  };
  NonFrameBytes = [&](const MedVar &V, unsigned First, unsigned Bytes) {
    if (!Bytes || First + Bytes > V.Size)
      return false;
    if (V.isConst())
      return true;
    const SliceKey K{key(V), First, Bytes};
    if (AssumedPhi == key(V) || ProvenNonFrame.count(K))
      return true;
    if (!Remaining ||
        NonFrameActive.size() >= limits::ir::kMIPSReturnIdentityDepth)
      return false;
    const auto Query = std::pair(K, AssumedPhi.has_value());
    if (!NonFrameActive.insert(Query).second) {
      ++Cycles;
      return false;
    }
    --Remaining;
    const auto D = Definitions.find(V);
    bool Safe = false;
    if (!D.Ambiguous && safety::detail::isAuthenticatedNonFrameEntryRegister(
                            Function, V, TRI.StackPointer, TRI.FramePointer)) {
      Safe = true;
    } else if (!D.Ambiguous && D.Phi &&
               Graph.hasCompleteIncoming(BlockIndices.at(D.Block), *D.Phi)) {
      const size_t Before = Cycles;
      Safe = llvm::all_of(D.Phi->Args, [&](const auto &A) {
        return NonFrameBytes(A.second, First, Bytes);
      });
      if (!Safe && !AssumedPhi && Cycles != Before &&
          D.Block->Id == BlockIndices.at(D.Block)) {
        auto [It, New] = LoopEdges.try_emplace(D.Block->Id);
        if (New)
          It->second = loopPredecessors(Function, *D.Block, Remaining);
        const auto &Back = It->second;
        // The initial value must be non-frame in full before an update may
        // use any of its bytes, even when the original query was a slice.
        if (!Back.empty() && llvm::all_of(D.Phi->Args, [&](const auto &A) {
              return Back.contains(A.first) || IsNonFrame(A.second);
            })) {
          AssumedPhi = key(V);
          Safe = llvm::all_of(D.Phi->Args, [&](const auto &A) {
            return !Back.contains(A.first) || IsNonFrame(A.second);
          });
          AssumedPhi.reset();
        }
      }
    } else if (!D.Ambiguous && D.Op) {
      const auto &Op = *D.Op;
      // External memory cannot contain a pointer into this newly allocated
      // frame unless it escaped. PreservesCall separately requires that no
      // store or call operand exposes it, including before the RA save.
      switch (Op.Opcode) {
      case NdOp::COPY:
      case NdOp::CAST:
        Safe = Op.NumInputs == 1 && NonFrameBytes(Op.Inputs[0], First, Bytes);
        break;
      case NdOp::SUBBYTES:
        Safe =
            Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
            Op.Inputs[1].ConstVal <= Op.Inputs[0].Size &&
            NonFrameBytes(Op.Inputs[0], First + Op.Inputs[1].ConstVal, Bytes);
        break;
      case NdOp::CONCAT:
        if (Op.NumInputs == 2) {
          const auto Low = Op.Inputs[1].Size;
          const auto End = First + Bytes;
          Safe = (First >= Low ||
                  NonFrameBytes(Op.Inputs[1], First,
                                std::min<unsigned>(End, Low) - First)) &&
                 (End <= Low ||
                  NonFrameBytes(Op.Inputs[0], First > Low ? First - Low : 0,
                                End - std::max<unsigned>(First, Low)));
        }
        break;
      case NdOp::LOAD:
        if (Op.NumInputs == 1 && QualifiedMemory) {
          if (HasCalls && IsNonFrame(Op.Inputs[0]))
            Safe = true;
          else if (const auto At = Offset(Op.Inputs[0]);
                   At && *At < 0 && *At + Op.Output.Size <= 0) {
            // Fixed stack arguments are loaded into explicit call operands.
            // Trace their exact reaching stores; a frame address stored there
            // is still an escape, and an intervening unknown call invalidates
            // the proof. Only the bytes this operand reads matter.
            const auto FromStores = [&](auto &&Self, unsigned Begin,
                                        unsigned Size) -> bool {
              const unsigned Start = std::max(Begin, First);
              const unsigned End = std::min(Begin + Size, First + Bytes);
              if (Start >= End)
                return true;
              if (!Remaining)
                return false;
              --Remaining;
              const unsigned MemoryByte = archLittleEndian(Target)
                                              ? Begin
                                              : Op.Output.Size - Begin - Size;
              const auto Reaching = safety::detail::reachingStackValues(
                  Function, D.Block->Id, int(&Op - D.Block->Ops.data()),
                  *At + MemoryByte, Size, Offset,
                  [&](const MedVar &Address) { return !IsNonFrame(Address); },
                  [](const MedVar &) { return true; }, true);
              if (!Reaching.Reachable)
                return false;
              if (Reaching.Complete)
                return llvm::all_of(Reaching.Values, [&](const MedVar &Stored) {
                  return NonFrameBytes(Stored, Start - Begin, End - Start);
                });
              // GCC can assemble one double from separate word stores.
              // Prove every requested byte on every predecessor, retaining
              // the existing rejection of unknown writes and frame escapes.
              if (Size == 1)
                return false;
              const unsigned Half = Size / 2;
              return Self(Self, Begin, Half) &&
                     Self(Self, Begin + Half, Size - Half);
            };
            Safe = FromStores(FromStores, 0, Op.Output.Size);
          } else if (!At && IsNonFrame(Op.Inputs[0])) {
            Safe = BeforeFrameEscape(*D.Block, &Op - D.Block->Ops.data());
          }
        }
        break;
      case NdOp::CALL:
      case NdOp::INDIR_CALL:
        // Follow a bound numerical result only when the target and every
        // argument have no provenance in this frame. Spilling that result
        // beside the saved link does not escape the link slot. Unknown and
        // pointer-returning calls still provide no such proof.
        if (const auto Call = Calls.find(&Op);
            Call != Calls.end() &&
            First + Bytes <= Call->second.NumericResultBytes) {
          Safe = IsNonFrame(Op.Inputs[0]);
          for (size_t I = 0; Safe && I < Call->second.Inputs.size(); ++I)
            Safe = NonFrameBytes(Op.Inputs[I + 1], 0,
                                 Call->second.Inputs[I].ValueBytes);
        }
        break;
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
      case NdOp::INT_AND:
      case NdOp::INT_OR:
      case NdOp::INT_XOR:
      case NdOp::INT_MULT:
      case NdOp::INT_LEFT:
      case NdOp::SELECT:
        Safe = Op.NumInputs &&
               llvm::all_of(llvm::ArrayRef(Op.Inputs.data(), Op.NumInputs),
                            IsNonFrame);
        break;
      case NdOp::INTRINSIC:
        Safe =
            Op.NumInputs && Op.Inputs[0].isConst() &&
            mips::isScalarFPArithmetic(
                static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) &&
            mipsScalarMedShapeIsValid(Op, Target) &&
            llvm::all_of(llvm::ArrayRef(Op.Inputs.data() + 1, Op.NumInputs - 1),
                         IsNonFrame);
        break;
      default:
        break;
      }
    }
    NonFrameActive.erase(Query);
    // A reaching-store walk can visit a later call-result spill while that
    // call's argument is still being proved. Such a recursive failure depends
    // on the active query; only completed positive proofs are reusable.
    if (Safe && !AssumedPhi)
      ProvenNonFrame.insert(K);
    return Safe;
  };
  const auto IntegerLayout = TRI.integerArgumentLayout(Format);
  const auto CallPrivateStart =
      [&](const MedOp &Call) -> std::optional<int64_t> {
    const auto P = CallSP.find(&Call);
    if (P == CallSP.end())
      return std::nullopt;
    const auto SP = Offset(P->second);
    if (!SP || *SP % (isMIPSO32(Target) ? 8 : 16) ||
        *SP < -limits::ir::kSourceFrameAnalysisMaxFrame)
      return std::nullopt;
    const int64_t SlotBytes = isMIPSO32(Target) ? 4 : 8;
    int64_t Extent = isMIPSO32(Target) ? 16 : 0;
    if (const auto S = Calls.find(&Call); S != Calls.end()) {
      for (const auto &A : S->second.Inputs)
        if (A.Kind == SourceABICarrierKind::Stack) {
          const auto End = A.EntryStackOffset + A.ValueBytes;
          // A big-endian n64 float occupies the first word, but the callee
          // still owns its complete eight-byte argument slot.
          Extent =
              std::max(Extent, (End + SlotBytes - 1) / SlotBytes * SlotBytes);
        }
    } else if (Call.CalleeStackArgs > int(IntegerLayout.Registers.size())) {
      Extent = std::max<int64_t>(
          Extent, IntegerLayout.CallStackBase +
                      (Call.CalleeStackArgs - IntegerLayout.Registers.size()) *
                          SlotBytes);
    }
    return *SP + Extent;
  };
  // A saved frame pointer does not publish its value when every call leaves
  // the complete spill slot private. Use the same outgoing-area boundary as
  // the saved-link proof, but require a complete argument extent before
  // admitting pointer spills. An unknown or open variadic call can read more
  // stack arguments than the ABI's minimum outgoing area.
  std::optional<int64_t> SharedPrivateStart;
  if (CallSP.size() == CallCount)
    for (const auto &Entry : CallSP) {
      const auto &Call = *Entry.first;
      const auto Start = CallPrivateStart(Call);
      if (!Start ||
          (!Calls.contains(&Call) &&
           (Call.CalleeStackArgs < 0 || Call.CalleeVariadicFrom >= 0 ||
            Call.CalleeVariadicRegisterFrom >= 0))) {
        SharedPrivateStart.reset();
        break;
      }
      SharedPrivateStart =
          SharedPrivateStart ? std::max(*SharedPrivateStart, *Start) : *Start;
    }
  const auto PrivateFrameStore = [&](const MedOp &Op) {
    const auto At = Offset(Op.Inputs[0]);
    return SharedPrivateStart && At && *At >= *SharedPrivateStart &&
           *At + Op.Inputs[1].Size <= 0;
  };
  // Native calls preserve the caller's allocated private frame under the
  // platform ABI even when the callee has no fixed declaration. Check every
  // possible argument definition that can reach a native call. Registers
  // merged after the last call cannot expose the frame to that callee.
  // Check all stores and calls so an escape before the RA save cannot be
  // forgotten when reachingStackValues sees the later exact store. Reloading
  // a private pointer into an argument still has to prove non-frame provenance.
  const auto FrameIsUnexposed = [&](const GuardedCalls &Guarded) {
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops) {
        if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2 &&
            !PrivateFrameStore(Op) && !IsNonFrame(Op.Inputs[1]))
          return false;
        if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
          continue;
        if (!Op.NumInputs || !IsNonFrame(Op.Inputs[0]))
          return false;
        const auto Signature = Calls.find(&Op);
        for (unsigned I = 1; I < Op.NumInputs; ++I) {
          const auto &Arg = Op.Inputs[I];
          const auto Bytes = Signature == Calls.end()
                                 ? Arg.Size
                                 : Signature->second.Inputs[I - 1].ValueBytes;
          if (!NonFrameBytes(Arg, 0, Bytes)) {
            const auto It = Guarded.find(&Op);
            if (It == Guarded.end() ||
                !copiesCallResult(Arg, *It->second.Producer, Definitions,
                                  Remaining))
              return false;
          }
        }
      }
    return nativeArgumentsAreNonFrame(NativeCalls, Guarded, TRI, Format, Graph,
                                      Remaining, IsNonFrame);
  };
  // Register-only returns never need a frame exposure proof. Cache it for
  // functions that restore the incoming link from more than one stack load.
  std::optional<bool> UnexposedFrame;
  const auto FrameIsPrivate = [&] {
    if (!UnexposedFrame) {
      UnexposedFrame = !HasCalls || FrameIsUnexposed(GuardedCalls{});
      if (!*UnexposedFrame && (isMIPSN32(Target) || isMIPSN64(Target)) &&
          NativeCalls.size() >= 2) {
        const auto Guarded =
            guardedResultArguments(Function, NativeCalls, TRI, Format, Graph,
                                   Definitions, Remaining, IsNonFrame);
        if (!Guarded.empty())
          UnexposedFrame = FrameIsUnexposed(Guarded);
      }
    }
    return *UnexposedFrame;
  };
  const uint16_t LinkBytes = isMIPS64(Target) ? 8 : 4;
  std::function<bool(const MedVar &)> Prove = [&](const MedVar &Value) {
    if (!Remaining || Value.isConst() || Value.Size != LinkBytes ||
        Active.size() >= limits::ir::kMIPSReturnIdentityDepth)
      return false;
    --Remaining;
    const Key Identity = key(Value);
    if (const auto It = Results.find(Identity); It != Results.end())
      return It->second;
    if (!Active.insert(Identity).second)
      return false;
    const auto Definition = Definitions.find(Value);
    bool Valid = false;
    if (!Definition.Ambiguous && Value.Kind == MedVar::Reg &&
        Value.RegOff == mipsreg::RA &&
        safety::detail::isAuthenticatedEntryRegisterLiveIn(Function, Value)) {
      Valid = true;
    } else if (!Definition.Ambiguous && Definition.Phi &&
               Graph.hasCompleteIncoming(BlockIndices.at(Definition.Block),
                                         *Definition.Phi)) {
      Valid = llvm::all_of(Definition.Phi->Args,
                           [&](const auto &Arg) { return Prove(Arg.second); });
    } else if (!Definition.Ambiguous && Definition.Op) {
      const auto &Op = *Definition.Op;
      if (const auto *Source = copiedWord(Op))
        Valid = Prove(*Source);
      else if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
               QualifiedMemory) {
        const auto At = Offset(Op.Inputs[0]);
        if (At && *At <= -int64_t(LinkBytes) &&
            *At >= -limits::ir::kSourceFrameAnalysisMaxFrame) {
          const bool Private = FrameIsPrivate();
          const auto PreservesCall = [&](const MedOp &Call, int64_t At,
                                         uint16_t Size) {
            if (!Private)
              return false;
            const auto Start = CallPrivateStart(Call);
            return Start && At >= *Start && At + Size <= 0;
          };
          const auto Reaching = safety::detail::reachingStackValues(
              Function, Definition.Block->Id,
              int(&Op - Definition.Block->Ops.data()), *At, LinkBytes, Offset,
              [&](const MedVar &V) {
                return HasCalls ? !IsNonFrame(V) : !V.isConst();
              },
              // The complete function proof already distinguishes private
              // pointer spills from escapes. Do not invalidate the same
              // slot merely because one of those private stores contains FP
              // or SP. Exact writes, overlaps and each call's outgoing area
              // are still checked by this walk and PreservesCall.
              [](const MedVar &) { return true; }, true, HasCalls && !Private,
              PreservesCall);
          // Exact stores and complete calls must preserve this private slot.
          // Escaped frame addresses, overlap and unresolved writes reject it.
          Valid = Reaching.Complete && Reaching.Reachable &&
                  !Reaching.MayBeUninitialized && !Reaching.HasUnknownWrites &&
                  !Reaching.Values.empty() &&
                  llvm::all_of(Reaching.Values, Prove);
        }
      }
    }
    Active.erase(Identity);
    Results.emplace(Identity, Valid);
    return Valid;
  };
  const auto RecordCall =
      [&](const MedOp &Call) -> std::optional<mips::RecordFrameCall> {
    const auto It = Calls.find(&Call);
    if (It == Calls.end() || !Call.SourceCallHint ||
        !Call.SourceCallHint->Signature.MIPSRecords)
      return std::nullopt;
    if (const auto Start = CallPrivateStart(Call))
      return mips::RecordFrameCall{*Start, It->second.NumericResultBytes};
    return std::nullopt;
  };
  const bool SingleRecordExit =
      RecordFrameMemory && Required.size() == 1 && TailReturns.empty() &&
      llvm::any_of(Calls, [](const auto &C) {
        return C.first->SourceCallHint &&
               C.first->SourceCallHint->Signature.MIPSRecords;
      });
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::RETURN) {
        const Site Origin{Op.Addr, Op.OriginSeq};
        if (const auto Tail = TailReturns.find(Origin);
            Tail != TailReturns.end()) {
          if (!QualifiedTails.erase(Tail->second))
            return false;
          TailReturns.erase(Tail);
          continue;
        }
        if (!Required.erase(Origin) || Op.NumInputs != SourceResultOperands + 1)
          return false;
        const auto &Link = Op.Inputs[SourceResultOperands];
        const auto RecordFrame = [&] {
          return SingleRecordExit &&
                 mips::preservesRecordFrameLink(Function, Op, Target,
                                                RecordCall, Remaining);
        };
        // A hidden result pointer can expose selected frame bytes to the
        // callee. Follow those bytes before spending the shared budget on a
        // general reaching-store proof that cannot preserve this call.
        const bool CheckRecordFirst =
            SingleRecordExit && (!QualifiedMemory || !FrameIsPrivate());
        const bool LinkPreserved = CheckRecordFirst
                                       ? RecordFrame() || Prove(Link)
                                       : Prove(Link) || RecordFrame();
        if (!LinkPreserved)
          return false;
      }
  return Required.empty() && TailReturns.empty() && QualifiedTails.empty();
}

} // namespace neverd
