//===- RegistrationStateCalls.cpp - x86 EH call transfers -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bind source call effects while retaining independent native call receipts.
///
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {
std::optional<RegistrationStateSolver::CallTransfer>
RegistrationStateSolver::transferCall(size_t I, Domain &After, const LowOp &Op,
                                      const BlockFacts &Facts) {
  if (!CheckCalls || (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL))
    return std::nullopt;
  const LowBlock &Block = Function.Blocks[I];
  const auto Identity = std::make_pair(Op.Addr, Op.Seq);
  const auto Boundary = Boundaries.find(Op.Addr);
  const auto Callee = Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
                              Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4
                          ? CalleeIndices.find(Op.Inputs[0].Offset)
                          : CalleeIndices.end();
  // A private conditional query needs exact invocation return facts. Root
  // calls retain an existing native contract; only a missing contract may
  // fall back to source-only effects without inventing a native call receipt.
  if (Conditional || Callee == CalleeIndices.end())
    if (auto Call = transferConditionalCxxCall(After, Op, Facts))
      return Call;
  auto StackPointer =
      After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  std::optional<FrameValue> CallResult;
  const auto SP = parentStackOffset(After);
  bool Valid = !After.Unknown && !Facts.Invalid && Op.Seq >= 0 &&
               Boundary != Boundaries.end() &&
               Boundary->second.first == Block.Id &&
               Boundary->second.second.Control == LowInstructionControl::Call &&
               Callee != CalleeIndices.end() && SP;
  RegistrationCallFrameEffect Effect;
  if (Valid) {
    const auto &Contract = Result.CalleeContracts[Callee->second];
    Valid &=
        Contract.RequiredImports.empty() || After.Frame.ImportBindingsIntact;
    // A rethrow carries the CRT's current exception, not an initialized new
    // object. It is valid only in a proved live catch invocation.
    if (Contract.isRuntimeThrow()) {
      Effect.RuntimeThrow =
          runtimeThrowArguments(After, Contract, Effect.FrameReads);
      Valid &= Effect.RuntimeThrow.has_value();
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
    if (BorrowsObject && !Effect.ECXFrameOffset)
      Valid &= bindRuntimeObjectBorrow(After, Contract, Effect);
    if (Valid && Effect.ECXFrameOffset) {
      Valid &= projectFrameObject(After, *Effect.ECXFrameOffset, SP,
                                  Contract.ECXReads, Effect.FrameReads,
                                  !Contract.isInitializer());
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
        After.Frame.forgetCellValues(Write.Begin,
                                     uint32_t(Write.End - Write.Begin));
      }
    }
    if (Valid && Contract.isInitializer()) {
      FrameValue Object;
      Valid = transferInitializer(After, Contract, StackPointer, Object);
      if (Valid)
        CallResult = Object;
    } else {
      if (Valid && Contract.StackPopBytes)
        Valid = transferScalarArguments(After, Contract, StackPointer,
                                        Effect.ECXFrameOffset);
      if (Valid && Contract.HasIndependentScalarReturn &&
          hasX86ScalarCallOutput(Op)) {
        // The exact result has no frame provenance. Keep every other volatile
        // register unknown and grant neither returned-this nor must-writes.
        CallResult = FrameValue{};
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
  // A checked frame borrow can still overwrite image storage. Use the same
  // binding owner as explicit stores, without narrowing a merged write range
  // to one instruction's access width.
  for (const auto &Write :
       Result.CalleeContracts[Effect.CalleeIndex].ImageWrites) {
    if (!charge(StackImportSlots.size() + 1))
      return std::nullopt;
    for (va_t Slot : StackImportSlots)
      if (Write.Begin < Slot + 4 && Slot < Write.End)
        After.Frame.ImportBindingsIntact = false;
    invalidateJumpRuntimeBindings(After, FrameValue::constant(Write.Begin),
                                  Write.End - Write.Begin);
  }
  return CallTransfer{
      StackPointer, Effect.DoesNotReturn, Effect.EndAddress, CallResult, {},
      true};
}

} // namespace neverd::registration_state
