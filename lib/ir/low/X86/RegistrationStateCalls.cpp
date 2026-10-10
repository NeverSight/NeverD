//===- RegistrationStateCalls.cpp - x86 EH call transfers -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {
std::optional<RegistrationStateSolver::CallTransfer>
RegistrationStateSolver::transferCall(size_t I, Domain &After,
                                      const LowOp &Op) {
  if (!CheckCalls || (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL))
    return std::nullopt;
  const LowBlock &Block = Function.Blocks[I];
  const auto Identity = std::make_pair(Op.Addr, Op.Seq);
  const auto Boundary = Boundaries.find(Op.Addr);
  const auto Callee = Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
                              Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4
                          ? CalleeIndices.find(Op.Inputs[0].Offset)
                          : CalleeIndices.end();
  const auto StackPointer =
      After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  const auto SP = parentStackOffset(After);
  bool Valid = !After.Unknown && !Facts[I].Invalid && Op.Seq >= 0 &&
               Boundary != Boundaries.end() &&
               Boundary->second.first == Block.Id &&
               Boundary->second.second.Control == LowInstructionControl::Call &&
               Callee != CalleeIndices.end() && SP;
  RegistrationCallFrameEffect Effect;
  if (Valid) {
    const auto &Contract = Result.CalleeContracts[Callee->second];
    // A rethrow carries the CRT's current exception, not an initialized new
    // object. It is valid only in a proved live catch invocation.
    if (Contract.isRuntimeThrow()) {
      Effect.RuntimeThrow =
          runtimeThrowArguments(After, Contract, Effect.FrameReads);
      Valid = Effect.RuntimeThrow.has_value();
    }
    if (Contract.isRethrow() ||
        (Effect.RuntimeThrow && Effect.RuntimeThrow->isRethrow()))
      Valid &= KnownCxx && !After.Parent && After.Callback &&
               !After.OtherCallback && After.CxxCatchStacks.size() == 1 &&
               !After.CxxCatchStacks.begin()->empty();
    Effect.Address = Op.Addr;
    Effect.EndAddress =
        Boundary->second.second.Address + Boundary->second.second.Size;
    Effect.OpSeq = Op.Seq;
    Effect.Target = Contract.Target;
    Effect.CalleeIndex = Callee->second;
    Effect.StackPopBytes = Contract.StackPopBytes;
    Effect.DoesNotReturn = Contract.DoesNotReturn;
    const bool BorrowsObject =
        !Contract.ECXReads.empty() || !Contract.ECXWrites.empty();
    if (BorrowsObject)
      Effect.ECXFrameOffset =
          After.Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride].Offset;
    Valid &= !BorrowsObject || Effect.ECXFrameOffset.has_value();
    if (Valid && BorrowsObject) {
      Valid &= projectFrameObject(After, *Effect.ECXFrameOffset, SP,
                                  Contract.ECXReads, Effect.FrameReads, true);
      Valid &=
          projectFrameObject(After, *Effect.ECXFrameOffset, SP,
                             Contract.ECXWrites, Effect.FrameWrites, false);
    }
    if (Valid &&
        !charge(Contract.ImageReads.size() + Effect.FrameWrites.size() + 1))
      Valid = false;
    if (Valid) {
      for (const auto &Read : Contract.ImageReads)
        ImageReads.emplace(Read.Begin, Read.End);
      for (const auto &Write : Effect.FrameWrites) {
        if (!charge(After.Frame.cellCount())) {
          Valid = false;
          break;
        }
        // Callee writes are may-effects. They invalidate value facts
        // without inventing definite initialization on untaken paths.
        for (auto It = After.Frame.Cells.begin();
             It != After.Frame.Cells.end();)
          if (int64_t(It->first) < Write.End &&
              Write.Begin < int64_t(It->first) + 4) {
            if (It->second.MayBeFrame && (Write.Begin > It->first ||
                                          int64_t(It->first) + 4 > Write.End)) {
              It->second = {{}, {}, false, true};
              ++It;
            } else
              It = After.Frame.Cells.erase(It);
          } else
            ++It;
      }
    }
  }
  if (Valid && !InvalidCalls.count(Identity)) {
    const auto [It, Inserted] = CallEffects.emplace(Identity, Effect);
    Valid = Inserted || It->second == Effect;
  } else
    Valid = false;
  if (!Valid) {
    InvalidCalls.insert(Identity);
    CallEffects.erase(Identity);
    CompleteCalls = CompleteImageReads = false;
    After.InitializedFrameBytes.clear();
    // A callee without a checked borrow may change cells reached by an
    // escaped register or stack pointer. Keep taint but drop identities.
    if (charge(After.Frame.cellCount()))
      After.Frame.forgetCellValues();
    return std::nullopt;
  }
  return CallTransfer{StackPointer, Effect.DoesNotReturn, Effect.EndAddress};
}

} // namespace neverd::registration_state
