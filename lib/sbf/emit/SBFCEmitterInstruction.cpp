//===- SBFCEmitterInstruction.cpp - SBF instruction to C rendering --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Renders one normalized MedIR instruction at a time as C: operand and
/// width handling, the guarded arithmetic and memory forms, the branch and
/// call forms, and the software call-frame protocol.
///
//===----------------------------------------------------------------------===//

#include "SBFCEmitterDetail.h"

#include "llvm/ADT/StringExtras.h"

#include <cstdlib>
#include <string>

namespace neverd::sbf {

using namespace c_emitter_detail;

namespace {

std::string reg(unsigned Register) {
  return "r[" + std::to_string(Register) + "]";
}

void emitArgumentRegisters(llvm::raw_ostream &OS) {
  for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index)
    OS << ", " << reg(kFirstArgumentRegister + Index);
}

std::string immediate(const MedInstruction &Instruction) {
  return word(normalizeImmediate(Instruction.Immediate,
                                 Instruction.Semantics.Immediate));
}

std::string source(const MedInstruction &Instruction) {
  switch (Instruction.Semantics.Source) {
  case OperandSourceKind::SourceRegister:
    return reg(Instruction.Src);
  case OperandSourceKind::None:
  case OperandSourceKind::Immediate:
  case OperandSourceKind::VersionedCallRegister:
  default:
    return immediate(Instruction);
  }
}

void emitAssign(llvm::raw_ostream &OS, const MedInstruction &Instruction,
                llvm::StringRef Expression,
                llvm::StringRef Indent = "        ") {
  OS << Indent << reg(Instruction.Dst) << " = ";
  switch (Instruction.Semantics.Result) {
  case ResultExtension::Zero32:
    OS << "(uint64_t)(uint32_t)(" << Expression << ")";
    break;
  case ResultExtension::Sign32:
    OS << "nd_sext32((uint32_t)(" << Expression << "))";
    break;
  case ResultExtension::None:
    OS << "(uint64_t)(" << Expression << ")";
    break;
  }
  OS << ";\n";
}

void emitAdvance(llvm::raw_ostream &OS, const MedInstruction &Instruction,
                 size_t InstructionCount) {
  const size_t Next = Instruction.Slot + Instruction.SlotWidth;
  if (Next < InstructionCount)
    OS << "        pc = " << Next << "; continue;\n";
  else
    OS << "        return NEVERD_SBF_EXECUTION_OVERRUN;\n";
}

void emitPushFrame(llvm::raw_ostream &OS, const MedInstruction &Instruction,
                   const SBFProgram &Program) {
  OS << "        if (depth + 1 >= NEVERD_SBF_MAX_CALL_DEPTH) return "
        "NEVERD_SBF_CALL_DEPTH;\n"
     << "        return_pc[depth] = " << Instruction.Slot + 1 << ";\n"
     << "        for (i = 0; i < NEVERD_SBF_SAVED_REGISTERS; ++i) "
        "saved[depth][i] = r[NEVERD_SBF_FIRST_SAVED_REGISTER + i];\n"
     << "        saved_fp[depth] = r[NEVERD_SBF_FRAME_POINTER];\n"
     << "        ++depth;\n";
  if (!versionHasFeature(Program.Low.TheVersion,
                         VersionFeature::ManualStackFrames)) {
    OS << "        r[NEVERD_SBF_FRAME_POINTER] += UINT64_C("
       << automaticFrameStride(Program.Low.TheVersion, Program.Config)
       << ");\n";
  }
}

} // namespace

namespace c_emitter_detail {

std::string word(uint64_t Value) {
  return "UINT64_C(0x" + llvm::utohexstr(Value) + ")";
}

std::string comparison(const MedInstruction &Instruction) {
  const std::string L = reg(Instruction.Dst);
  const std::string R = source(Instruction);
  const bool Width32 = Instruction.Width == kWordBitWidth;
  const std::string LU = Width32 ? "(uint32_t)" + L : L;
  const std::string RU = Width32 ? "(uint32_t)" + R : R;
  switch (Instruction.Op) {
  case Operation::Eq:
    return LU + " == " + RU;
  case Operation::Ne:
    return LU + " != " + RU;
  case Operation::UGt:
    return LU + " > " + RU;
  case Operation::UGe:
    return LU + " >= " + RU;
  case Operation::ULt:
    return LU + " < " + RU;
  case Operation::ULe:
    return LU + " <= " + RU;
  case Operation::SGt:
    return Width32 ? "nd_sgt32((uint32_t)" + L + ", (uint32_t)" + R + ")"
                   : "nd_sgt64(" + L + ", " + R + ")";
  case Operation::SGe:
    return Width32 ? "!nd_sgt32((uint32_t)" + R + ", (uint32_t)" + L + ")"
                   : "!nd_sgt64(" + R + ", " + L + ")";
  case Operation::SLt:
    return Width32 ? "nd_sgt32((uint32_t)" + R + ", (uint32_t)" + L + ")"
                   : "nd_sgt64(" + R + ", " + L + ")";
  case Operation::SLe:
    return Width32 ? "!nd_sgt32((uint32_t)" + L + ", (uint32_t)" + R + ")"
                   : "!nd_sgt64(" + L + ", " + R + ")";
  case Operation::Set:
    return "(" + LU + " & " + RU + ") != 0";
  default:
    return "0";
  }
}

bool emitLinearInstruction(llvm::raw_ostream &OS,
                           const MedInstruction &Instruction,
                           llvm::StringRef Indent) {
  const std::string D = reg(Instruction.Dst);
  const std::string S = source(Instruction);
  switch (Instruction.Op) {
  case Operation::LoadImm:
    emitAssign(OS, Instruction, immediate(Instruction), Indent);
    break;
  case Operation::Mov:
    emitAssign(OS, Instruction, S, Indent);
    break;
  case Operation::Add:
    emitAssign(OS, Instruction, D + " + " + S, Indent);
    break;
  case Operation::Sub:
    emitAssign(OS, Instruction,
               Instruction.Semantics.SwapOperands ? S + " - " + D
                                                  : D + " - " + S,
               Indent);
    break;
  case Operation::Mul:
    emitAssign(OS, Instruction, D + " * " + S, Indent);
    break;
  case Operation::UHighMul:
    emitAssign(OS, Instruction, "nd_umulh64(" + D + ", " + S + ")", Indent);
    break;
  case Operation::SHighMul:
    emitAssign(OS, Instruction, "nd_smulh64(" + D + ", " + S + ")", Indent);
    break;
  case Operation::Or:
    emitAssign(OS, Instruction, D + " | " + S, Indent);
    break;
  case Operation::And:
    emitAssign(OS, Instruction, D + " & " + S, Indent);
    break;
  case Operation::Xor:
    emitAssign(OS, Instruction, D + " ^ " + S, Indent);
    break;
  case Operation::Neg:
    emitAssign(OS, Instruction, "UINT64_C(0) - " + D, Indent);
    break;
  case Operation::LSh:
    emitAssign(OS, Instruction,
               D + " << ((uint32_t)" + S + " & " +
                   std::to_string(Instruction.Width - 1) + "u)",
               Indent);
    break;
  case Operation::RSh:
    emitAssign(OS, Instruction,
               (Instruction.Width == kWordBitWidth ? "(uint32_t)" : "") + D +
                   " >> ((uint32_t)" + S + " & " +
                   std::to_string(Instruction.Width - 1) + "u)",
               Indent);
    break;
  case Operation::ARSh:
    emitAssign(OS, Instruction,
               std::string(Instruction.Width == kWordBitWidth ? "nd_ashr32("
                                                              : "nd_ashr64(") +
                   (Instruction.Width == kWordBitWidth ? "(uint32_t)" : "") +
                   D + ", " + S + ")",
               Indent);
    break;
  case Operation::UDiv:
  case Operation::URem:
    OS << Indent << "if (("
       << (Instruction.Width == kWordBitWidth ? "(uint32_t)" : "") << S
       << ") == 0) return NEVERD_SBF_DIVIDE_BY_ZERO;\n";
    emitAssign(OS, Instruction,
               (Instruction.Width == kWordBitWidth ? "(uint32_t)" : "") + D +
                   (Instruction.Op == Operation::UDiv ? " / " : " % ") +
                   (Instruction.Width == kWordBitWidth ? "(uint32_t)" : "") + S,
               Indent);
    break;
  case Operation::SDiv:
  case Operation::SRem:
    OS << Indent << "{ int fault = 0; uint64_t value = nd_sdivrem(" << D << ", "
       << S << ", " << unsigned(Instruction.Width) << ", "
       << (Instruction.Op == Operation::SRem ? 1 : 0)
       << ", &fault); if (fault) return (neverd_sbf_status)fault;\n";
    emitAssign(OS, Instruction, "value", (Indent + "  ").str());
    OS << Indent << "}\n";
    break;
  case Operation::EndianLE:
    if (Instruction.Immediate == kHalfWordBitWidth)
      emitAssign(OS, Instruction, "(uint16_t)" + D, Indent);
    else if (Instruction.Immediate == kWordBitWidth)
      emitAssign(OS, Instruction, "(uint32_t)" + D, Indent);
    else
      emitAssign(OS, Instruction, D, Indent);
    break;
  case Operation::EndianBE:
    emitAssign(
        OS, Instruction,
        "nd_bswap(" + D + ", " +
            std::to_string(static_cast<uint32_t>(Instruction.Immediate)) + ")",
        Indent);
    break;
  case Operation::HighOr:
    emitAssign(OS, Instruction,
               D + " | (" + word(static_cast<uint32_t>(Instruction.Immediate)) +
                   " << " + std::to_string(kWordBitWidth) + ")",
               Indent);
    break;
  case Operation::Load: {
    const std::string Address =
        reg(Instruction.Src) + (Instruction.Offset < 0 ? " - " : " + ") +
        word(static_cast<uint64_t>(
            std::abs(static_cast<int>(Instruction.Offset))));
    OS << Indent
       << "{ uint64_t value = 0; if (!env || !env->load || "
          "env->load(env->context, "
       << Address << ", " << unsigned(Instruction.Width)
       << ", &value) != 0) return NEVERD_SBF_MEMORY_ACCESS;\n";
    emitAssign(OS, Instruction, "value", (Indent + "  ").str());
    OS << Indent << "}\n";
    break;
  }
  case Operation::Store: {
    const std::string Address =
        reg(Instruction.Dst) + (Instruction.Offset < 0 ? " - " : " + ") +
        word(static_cast<uint64_t>(
            std::abs(static_cast<int>(Instruction.Offset))));
    OS << Indent << "if (!env || !env->store || env->store(env->context, "
       << Address << ", " << unsigned(Instruction.Width) << ", " << S
       << ") != 0) return NEVERD_SBF_MEMORY_ACCESS;\n";
    break;
  }
  case Operation::Call:
    if (Instruction.Call != CallKind::Syscall)
      return false;
    OS << Indent
       << "{ uint64_t value = 0; if (!env || !env->syscall || "
          "env->syscall(env->context, UINT32_C(0x"
       << llvm::utohexstr(Instruction.SyscallHash) << ")";
    emitArgumentRegisters(OS);
    OS << ", &value) != 0) return "
          "NEVERD_SBF_UNKNOWN_SYSCALL; "
          "r[NEVERD_SBF_RETURN_REGISTER] = value; }\n";
    break;
  case Operation::Jump:
  case Operation::Eq:
  case Operation::Ne:
  case Operation::UGt:
  case Operation::UGe:
  case Operation::ULt:
  case Operation::ULe:
  case Operation::SGt:
  case Operation::SGe:
  case Operation::SLt:
  case Operation::SLe:
  case Operation::Set:
  case Operation::CallX:
  case Operation::Exit:
  case Operation::Invalid:
    return false;
  }
  return true;
}

void emitInstruction(llvm::raw_ostream &OS, const MedInstruction &Instruction,
                     const SBFProgram &Program) {
  OS << "      case " << Instruction.Slot << ": /* "
     << opcodeName(Instruction.SourceOpcode) << " @ 0x"
     << llvm::utohexstr(Instruction.Address) << " */\n";

  if (emitLinearInstruction(OS, Instruction, "        ")) {
    emitAdvance(OS, Instruction, Program.Low.Instructions.size());
    return;
  }

  switch (Instruction.Op) {
  case Operation::Jump:
    OS << "        pc = " << Instruction.BranchTarget.value_or(0)
       << "; continue;\n";
    return;
  case Operation::Eq:
  case Operation::Ne:
  case Operation::UGt:
  case Operation::UGe:
  case Operation::ULt:
  case Operation::ULe:
  case Operation::SGt:
  case Operation::SGe:
  case Operation::SLt:
  case Operation::SLe:
  case Operation::Set:
    OS << "        pc = (" << comparison(Instruction) << ") ? "
       << Instruction.BranchTarget.value_or(0) << " : " << Instruction.Slot + 1
       << "; continue;\n";
    return;
  case Operation::Call:
    if (Instruction.Call == CallKind::Internal && Instruction.CallTarget) {
      emitPushFrame(OS, Instruction, Program);
      OS << "        pc = " << *Instruction.CallTarget << "; continue;\n";
      return;
    }
    OS << "        return NEVERD_SBF_UNKNOWN_SYSCALL;\n";
    return;
  case Operation::CallX:
    emitPushFrame(OS, Instruction, Program);
    OS << "        { uint64_t target = r[" << unsigned(Instruction.CallRegister)
       << "]; if (target < NEVERD_SBF_TEXT_ADDRESS) return "
          "NEVERD_SBF_UNKNOWN_FUNCTION; pc = (uint32_t)((target - "
          "NEVERD_SBF_TEXT_ADDRESS) / NEVERD_SBF_INSTRUCTION_SIZE); "
          "if (pc >= NEVERD_SBF_INSTRUCTION_COUNT) return "
          "NEVERD_SBF_UNKNOWN_FUNCTION; continue; }\n";
    return;
  case Operation::Exit:
    OS << "        if (depth == 0) { if (result) *result = "
          "r[NEVERD_SBF_RETURN_REGISTER]; return "
          "NEVERD_SBF_OK; }\n"
       << "        --depth; for (i = 0; i < NEVERD_SBF_SAVED_REGISTERS; ++i) "
          "r[NEVERD_SBF_FIRST_SAVED_REGISTER + i] = saved[depth][i];\n"
       << "        r[NEVERD_SBF_FRAME_POINTER] = saved_fp[depth]; pc = "
          "return_pc[depth]; continue;\n";
    return;
  case Operation::Invalid:
    OS << "        return NEVERD_SBF_INVALID_INSTRUCTION;\n";
    return;
  case Operation::LoadImm:
  case Operation::Load:
  case Operation::Store:
  case Operation::Add:
  case Operation::Sub:
  case Operation::Mul:
  case Operation::UHighMul:
  case Operation::SHighMul:
  case Operation::UDiv:
  case Operation::URem:
  case Operation::SDiv:
  case Operation::SRem:
  case Operation::Or:
  case Operation::And:
  case Operation::Xor:
  case Operation::LSh:
  case Operation::RSh:
  case Operation::ARSh:
  case Operation::Neg:
  case Operation::Mov:
  case Operation::EndianLE:
  case Operation::EndianBE:
  case Operation::HighOr:
    return;
  }
}

} // namespace c_emitter_detail
} // namespace neverd::sbf
