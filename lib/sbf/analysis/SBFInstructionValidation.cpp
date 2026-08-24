//===- SBFInstructionValidation.cpp - Shared requisite rules -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SBFInstructionValidation.h"

namespace neverd::sbf::validation_detail {

InstructionValidation validateInstruction(llvm::ArrayRef<uint8_t> Text,
                                          Version TheVersion,
                                          const InstructionView &Instruction) {
  InstructionValidation Result;
  Result.DiagnosticSlot = Instruction.Slot;
  if (!Instruction.Info) {
    Result.Rule = ValidationRule::UnknownOpcode;
    return Result;
  }

  const size_t InstructionCount = Text.size() / kInstructionSize;
  const OpcodeInfo &Info = *Instruction.Info;
  if (Info.ID == Opcode::LDDW) {
    if (Instruction.Slot + 1 >= InstructionCount) {
      Result.Rule = ValidationRule::MissingLDDWContinuation;
      return Result;
    }
    const uint8_t *Continuation =
        Text.data() + (Instruction.Slot + 1) * kInstructionSize;
    if (Continuation[kOpcodeOffset] != 0) {
      Result.Rule = ValidationRule::NonZeroLDDWContinuation;
      return Result;
    }
    Result.HasLDDWContinuation = true;
  }

  const SemanticTraits Traits = semanticTraits(Info, TheVersion);
  if (Info.Form == OperandForm::DstImm &&
      hasFaultPolicy(Traits.Faults, FaultPolicy::DivideByZero) &&
      Instruction.Immediate == 0) {
    Result.Rule = ValidationRule::ImmediateDivisionByZero;
    return Result;
  }
  if ((Info.Op == Operation::LSh || Info.Op == Operation::RSh ||
       Info.Op == Operation::ARSh) &&
      Info.Form == OperandForm::DstImm &&
      (Instruction.Immediate < 0 || Instruction.Immediate >= Info.Width)) {
    Result.Rule = ValidationRule::ImmediateShiftOutOfRange;
    return Result;
  }
  if ((Info.Op == Operation::EndianLE || Info.Op == Operation::EndianBE) &&
      Instruction.Immediate != kHalfWordBitWidth &&
      Instruction.Immediate != kWordBitWidth &&
      Instruction.Immediate != kDoubleWordBitWidth) {
    Result.Rule = ValidationRule::InvalidEndianImmediate;
    return Result;
  }

  const bool Store = Info.writesMemory();
  const bool ManualFrameBump =
      Info.ID == Opcode::ADD64_IMM &&
      versionHasFeature(TheVersion, VersionFeature::ManualStackFrames);
  if (ManualFrameBump && Instruction.Dst == kFramePointerRegister &&
      Instruction.Immediate %
              static_cast<int32_t>(kDynamicStackFrameAlignment) !=
          0) {
    Result.Rule = ValidationRule::MisalignedFrameAdjustment;
    return Result;
  }

  if (Info.isBranch()) {
    const int64_t Target = static_cast<int64_t>(Instruction.Slot) + 1 +
                           static_cast<int64_t>(Instruction.Offset);
    if (Target < 0 || static_cast<uint64_t>(Target) >= InstructionCount) {
      Result.Rule = ValidationRule::BranchOutOfRange;
      return Result;
    }
    if (Text[static_cast<size_t>(Target) * kInstructionSize + kOpcodeOffset] ==
        0) {
      Result.Rule = ValidationRule::BranchToLDDWContinuation;
      return Result;
    }
    Result.BranchTarget = static_cast<size_t>(Target);
  }

  if (Info.ID == Opcode::CALL_REG) {
    const int64_t Register = callxRegisterIndex(
        TheVersion, Instruction.Dst, Instruction.Src, Instruction.Immediate);
    if (Register < 0 || Register >= kFramePointerRegister) {
      Result.Rule = ValidationRule::InvalidCallXRegister;
      return Result;
    }
    Result.CallXRegister = static_cast<uint8_t>(Register);
  }

  if (Info.ID == Opcode::LDDW)
    Result.DiagnosticSlot = Instruction.Slot + 1;
  if (Instruction.Src >= kRegisterCount) {
    Result.Rule = ValidationRule::InvalidSourceRegister;
    return Result;
  }
  if (Instruction.Dst >= kRegisterCount) {
    Result.Rule = ValidationRule::InvalidDestinationRegister;
    return Result;
  }
  if (Instruction.Dst == kFramePointerRegister && !Store && !ManualFrameBump) {
    Result.Rule = ValidationRule::FramePointerWrite;
    return Result;
  }

  return Result;
}

} // namespace neverd::sbf::validation_detail
