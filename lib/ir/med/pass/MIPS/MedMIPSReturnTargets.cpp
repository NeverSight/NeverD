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

#include "neverd/ir/Limits.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MIPS/MIPSIntrinsicShapes.h"
#include "neverd/ir/med/MIPS/MIPSReturnTargets.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace neverd {

bool hasMIPSSourceReturnTargets(const LowFunc &Low, const MedFunc &Function,
                                Arch Target, BinaryFormat Format) {
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
  for (const auto &Block : Low.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries) {
      // A synthetic return does not prove the link supplied to the tail
      // callee. Qualify that pre-transfer identity before admitting tails.
      if (Boundary.Control == LowInstructionControl::TailCall)
        return false;
      for (size_t I = Boundary.FirstOp; I < Boundary.FirstOp + Boundary.OpCount;
           ++I)
        if (const auto &Op = Block.Ops[I]; Op.Opcode == NdOp::RETURN)
          Required.emplace(Op.Addr, Op.Seq);
    }
  if (Required.empty())
    return true;
  using Key = std::tuple<int, int, int, uint64_t, uint16_t>;
  const auto key = [](const MedVar &V) -> Key {
    return {int(V.Kind), V.Id, V.SSAVer, V.RegOff, V.Size};
  };
  std::vector<MedVar> MemoryAddresses;
  std::map<const MedBlock *, int> BlockIndices;
  bool QualifiedMemory = true;
  bool HasCalls = false;
  std::map<const MedOp *, FloatingCallArguments> Calls;
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
           mips::isBarrier(static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) ||
           mips::isDSPControlIntrinsic(
               static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) ||
           mips::isPureScalarValue(
               static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) ||
           mips::isR6FPArithmetic(
               static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) ||
           mips::isR6FPControl(
               static_cast<Intrinsic>(Op.Inputs[0].ConstVal))) &&
          mipsScalarMedShapeIsValid(Op, Target);
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) && Op.NumInputs)
        MemoryAddresses.push_back(Op.Inputs[0]);
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        HasCalls = true;
        const auto Signature = boundFloatingCallArguments(Op, Target, Format);
        if (!Signature)
          QualifiedMemory = false;
        else
          Calls.emplace(&Op, *Signature);
      }
      if ((Op.Opcode == NdOp::INTRINSIC && !NoDataWrite &&
           (Op.NumInputs != 3 || !Op.Inputs[0].isConst() ||
            Op.Inputs[0].ConstVal != uint64_t(Intrinsic::RequireAligned))) ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Op.MemoryOrdering != NdMemoryOrdering::None)
        QualifiedMemory = false;
    }
  }
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
  std::map<Key, bool> Results;
  std::set<Key> Active;
  const auto &TRI = getTargetRegInfo(Target);
  using SliceKey = std::tuple<Key, unsigned, unsigned>;
  std::map<SliceKey, bool> NonFrame;
  std::set<SliceKey> NonFrameActive;
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
    if (const auto It = NonFrame.find(K); It != NonFrame.end())
      return It->second;
    if (!Remaining ||
        NonFrameActive.size() >= limits::ir::kMIPSReturnIdentityDepth ||
        !NonFrameActive.insert(K).second)
      return false;
    --Remaining;
    const auto D = Definitions.find(V);
    bool Safe = false;
    if (!D.Ambiguous && safety::detail::isAuthenticatedNonFrameEntryRegister(
                            Function, V, TRI.StackPointer, TRI.FramePointer)) {
      Safe = true;
    } else if (!D.Ambiguous && D.Phi &&
               Graph.hasCompleteIncoming(BlockIndices.at(D.Block), *D.Phi)) {
      Safe = llvm::all_of(D.Phi->Args, [&](const auto &A) {
        return NonFrameBytes(A.second, First, Bytes);
      });
    } else if (!D.Ambiguous && D.Op) {
      const auto &Op = *D.Op;
      // Only explicit value producers can exclude frame-derived bits. Unknown
      // loads, calls and unknown state have no non-frame provenance here.
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
        if (Op.NumInputs == 1 && QualifiedMemory)
          if (const auto At = Offset(Op.Inputs[0]);
              At && *At < 0 && *At + Op.Output.Size <= 0) {
            // Fixed stack arguments are loaded into explicit call operands.
            // Trace their exact reaching stores; a frame address stored there
            // is still an escape, and an intervening unknown call invalidates
            // the proof. Only the bytes this operand reads matter.
            const auto Reaching = safety::detail::reachingStackValues(
                Function, D.Block->Id, int(&Op - D.Block->Ops.data()), *At,
                Op.Output.Size, Offset,
                [&](const MedVar &Address) { return !IsNonFrame(Address); },
                [](const MedVar &) { return true; }, true);
            Safe = Reaching.Complete && Reaching.Reachable &&
                   !Reaching.MayBeUninitialized && !Reaching.HasUnknownWrites &&
                   !Reaching.Values.empty() &&
                   llvm::all_of(Reaching.Values, [&](const MedVar &Stored) {
                     return NonFrameBytes(Stored, First, Bytes);
                   });
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
            mips::isR6FPArithmetic(
                static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) &&
            mipsScalarMedShapeIsValid(Op, Target) &&
            llvm::all_of(llvm::ArrayRef(Op.Inputs.data() + 1, Op.NumInputs - 1),
                         IsNonFrame);
        break;
      default:
        break;
      }
    }
    NonFrameActive.erase(K);
    NonFrame.emplace(K, Safe);
    return Safe;
  };
  const auto PreservesCall = [&](const MedOp &Call, int64_t At, uint16_t Size) {
    const auto S = Calls.find(&Call);
    const auto P = CallSP.find(&Call);
    if (S == Calls.end() || P == CallSP.end())
      return false;
    const auto SP = Offset(P->second);
    if (!SP || *SP % (isMIPS64(Target) ? 16 : 8))
      return false;
    const int64_t SlotBytes = isMIPS64(Target) ? 8 : 4;
    int64_t Extent = isMIPS64(Target) ? 0 : 16;
    for (const auto &A : S->second.Scalars)
      if (A.Kind == SourceABICarrierKind::Stack) {
        const auto End = A.StackOffset + A.Bytes;
        // A big-endian n64 float occupies the first word, but the callee
        // still owns its complete eight-byte argument slot.
        Extent =
            std::max(Extent, (End + SlotBytes - 1) / SlotBytes * SlotBytes);
      }
    // The callee owns everything below SP and its complete outgoing area.
    if (*SP < -limits::ir::kSourceFrameAnalysisMaxFrame || At < *SP + Extent ||
        At + Size > 0)
      return false;
    if (!IsNonFrame(Call.Inputs[0]))
      return false;
    for (size_t I = 0; I != S->second.Scalars.size(); ++I)
      if (!NonFrameBytes(Call.Inputs[I + 1], 0, S->second.Scalars[I].Bytes))
        return false;
    return true;
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
      const auto Zero = [&](unsigned I) {
        return Op.Inputs[I].isConst() && Op.Inputs[I].ConstVal == 0;
      };
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
        Valid = Prove(Op.Inputs[0]);
      else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_OR ||
                Op.Opcode == NdOp::INT_XOR) &&
               Op.NumInputs == 2)
        Valid = (Zero(0) && Prove(Op.Inputs[1])) ||
                (Zero(1) && Prove(Op.Inputs[0]));
      else if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
               QualifiedMemory) {
        const auto At = Offset(Op.Inputs[0]);
        if (At && *At <= -int64_t(LinkBytes) &&
            *At >= -limits::ir::kSourceFrameAnalysisMaxFrame) {
          const auto Reaching = safety::detail::reachingStackValues(
              Function, Definition.Block->Id,
              int(&Op - Definition.Block->Ops.data()), *At, LinkBytes, Offset,
              [&](const MedVar &V) {
                return HasCalls ? !IsNonFrame(V) : !V.isConst();
              },
              [](const MedVar &) { return true; }, true, HasCalls,
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
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::RETURN) {
        const Site Origin{Op.Addr, Op.OriginSeq};
        if (!Required.erase(Origin) || Op.NumInputs != 1 ||
            !Prove(Op.Inputs[0]))
          return false;
      }
  return Required.empty();
}

} // namespace neverd
