//===- SBFRustEmitterInstruction.cpp - SBF instruction to Rust ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Renders one normalized MedIR instruction at a time as safe Rust: operand
/// and width handling, the wrapping arithmetic and checked division forms,
/// the memory and syscall forms, and the software call-frame protocol.
///
//===----------------------------------------------------------------------===//

#include "SBFRustEmitterDetail.h"

#include "llvm/ADT/StringExtras.h"

#include <cstdlib>
#include <string>

namespace neverd::sbf {

using namespace rust_emitter_detail;

namespace {

std::string reg(unsigned Register) {
  return "r[" + std::to_string(Register) + "]";
}

std::string argumentRegisters() {
  std::string Result = "[";
  for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index) {
    if (Index != 0)
      Result += ", ";
    Result += reg(kFirstArgumentRegister + Index);
  }
  Result += "]";
  return Result;
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

std::string applyExtension(const MedInstruction &Instruction,
                           llvm::StringRef Expression) {
  switch (Instruction.Semantics.Result) {
  case ResultExtension::Zero32:
    return "(" + Expression.str() + " as u32) as u64";
  case ResultExtension::Sign32:
    return "sext32(" + Expression.str() + " as u32)";
  case ResultExtension::None:
    return "(" + Expression.str() + ") as u64";
  }
  return Expression.str();
}

void assign(llvm::raw_ostream &OS, const MedInstruction &Instruction,
            llvm::StringRef Expression,
            llvm::StringRef Indent = "                ") {
  OS << Indent << reg(Instruction.Dst) << " = "
     << applyExtension(Instruction, Expression) << ";\n";
}

void advance(llvm::raw_ostream &OS, const MedInstruction &Instruction,
             size_t Count) {
  const size_t Next = Instruction.Slot + Instruction.SlotWidth;
  if (Next < Count)
    OS << "                pc = " << Next << ";\n";
  else
    OS << "                return Err(SbfError::ExecutionOverrun);\n";
}

void pushFrame(llvm::raw_ostream &OS, const MedInstruction &Instruction,
               const SBFProgram &Program) {
  OS << "                if depth + 1 >= MAX_CALL_DEPTH { return "
        "Err(SbfError::CallDepth); }\n"
     << "                return_pc[depth] = " << Instruction.Slot + 1 << ";\n"
     << "                "
        "saved[depth].copy_from_slice(&r[FIRST_SAVED..FIRST_SAVED + "
        "SAVED_REGISTERS]);\n"
     << "                saved_fp[depth] = r[FRAME_POINTER];\n"
     << "                depth += 1;\n";
  if (!versionHasFeature(Program.Low.TheVersion,
                         VersionFeature::ManualStackFrames)) {
    OS << "                r[FRAME_POINTER] = r[FRAME_POINTER].wrapping_add("
       << automaticFrameStride(Program.Low.TheVersion, Program.Config)
       << "u64);\n";
  }
}

} // namespace

namespace rust_emitter_detail {

std::string word(uint64_t Value) {
  return "0x" + llvm::utohexstr(Value, true) + "u64";
}

std::string comparison(const MedInstruction &Instruction) {
  const std::string L = reg(Instruction.Dst);
  const std::string R = source(Instruction);
  const std::string LU =
      Instruction.Width == kWordBitWidth ? "(" + L + " as u32)" : L;
  const std::string RU =
      Instruction.Width == kWordBitWidth ? "(" + R + " as u32)" : R;
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
    return Instruction.Width == kWordBitWidth
               ? LU + " as i32 > " + RU + " as i32"
               : LU + " as i64 > " + RU + " as i64";
  case Operation::SGe:
    return Instruction.Width == kWordBitWidth
               ? LU + " as i32 >= " + RU + " as i32"
               : LU + " as i64 >= " + RU + " as i64";
  case Operation::SLt:
    return Instruction.Width == kWordBitWidth
               ? LU + " as i32 < " + RU + " as i32"
               : LU + " as i64 < " + RU + " as i64";
  case Operation::SLe:
    return Instruction.Width == kWordBitWidth
               ? LU + " as i32 <= " + RU + " as i32"
               : LU + " as i64 <= " + RU + " as i64";
  case Operation::Set:
    return "(" + LU + " & " + RU + ") != 0";
  default:
    return "false";
  }
}

bool emitLinearInstruction(llvm::raw_ostream &OS,
                           const MedInstruction &Instruction,
                           llvm::StringRef Indent) {
  const std::string D = reg(Instruction.Dst);
  const std::string S = source(Instruction);
  switch (Instruction.Op) {
  case Operation::LoadImm:
    assign(OS, Instruction, immediate(Instruction), Indent);
    break;
  case Operation::Mov:
    assign(OS, Instruction, S, Indent);
    break;
  case Operation::Add:
    assign(OS, Instruction,
           Instruction.Width == kWordBitWidth
               ? "(" + D + " as u32).wrapping_add(" + S + " as u32)"
               : D + ".wrapping_add(" + S + ")",
           Indent);
    break;
  case Operation::Sub:
    if (Instruction.Width == kWordBitWidth)
      assign(OS, Instruction,
             Instruction.Semantics.SwapOperands
                 ? "(" + S + " as u32).wrapping_sub(" + D + " as u32)"
                 : "(" + D + " as u32).wrapping_sub(" + S + " as u32)",
             Indent);
    else
      assign(OS, Instruction,
             Instruction.Semantics.SwapOperands
                 ? S + ".wrapping_sub(" + D + ")"
                 : D + ".wrapping_sub(" + S + ")",
             Indent);
    break;
  case Operation::Mul:
    assign(OS, Instruction,
           Instruction.Width == kWordBitWidth
               ? "(" + D + " as u32).wrapping_mul(" + S + " as u32)"
               : D + ".wrapping_mul(" + S + ")",
           Indent);
    break;
  case Operation::UHighMul:
    assign(OS, Instruction,
           "(((" + D + " as u128) * (" + S + " as u128)) >> 64) as u64",
           Indent);
    break;
  case Operation::SHighMul:
    assign(OS, Instruction,
           "(((" + D + " as i64 as i128).wrapping_mul(" + S +
               " as i64 as i128)) >> 64) as u64",
           Indent);
    break;
  case Operation::Or:
    assign(OS, Instruction, D + " | " + S, Indent);
    break;
  case Operation::And:
    assign(OS, Instruction, D + " & " + S, Indent);
    break;
  case Operation::Xor:
    assign(OS, Instruction, D + " ^ " + S, Indent);
    break;
  case Operation::Neg:
    assign(OS, Instruction, "0u64.wrapping_sub(" + D + ")", Indent);
    break;
  case Operation::LSh:
    assign(OS, Instruction,
           Instruction.Width == kWordBitWidth
               ? "(" + D + " as u32).wrapping_shl(" + S + " as u32)"
               : D + ".wrapping_shl(" + S + " as u32)",
           Indent);
    break;
  case Operation::RSh:
    assign(OS, Instruction,
           Instruction.Width == kWordBitWidth
               ? "(" + D + " as u32).wrapping_shr(" + S + " as u32)"
               : D + ".wrapping_shr(" + S + " as u32)",
           Indent);
    break;
  case Operation::ARSh:
    assign(OS, Instruction,
           Instruction.Width == kWordBitWidth
               ? "(" + D + " as i32).wrapping_shr(" + S + " as u32) as u32"
               : "(" + D + " as i64).wrapping_shr(" + S + " as u32) as u64",
           Indent);
    break;
  case Operation::UDiv:
  case Operation::URem: {
    const std::string Divisor =
        Instruction.Width == kWordBitWidth ? "(" + S + " as u32)" : S;
    const std::string Dividend =
        Instruction.Width == kWordBitWidth ? "(" + D + " as u32)" : D;
    OS << Indent << "if " << Divisor
       << " == 0 { return Err(SbfError::DivideByZero); }\n";
    assign(OS, Instruction,
           Dividend + (Instruction.Op == Operation::UDiv ? " / " : " % ") +
               Divisor,
           Indent);
    break;
  }
  case Operation::SDiv:
  case Operation::SRem: {
    const bool Width32 = Instruction.Width == kWordBitWidth;
    const std::string SignedD =
        Width32 ? "(" + D + " as i32)" : "(" + D + " as i64)";
    const std::string SignedS =
        Width32 ? "(" + S + " as i32)" : "(" + S + " as i64)";
    OS << Indent << "if " << SignedS
       << " == 0 { return Err(SbfError::DivideByZero); }\n"
       << Indent << "let value = " << SignedD
       << (Instruction.Op == Operation::SDiv ? ".checked_div("
                                             : ".checked_rem(")
       << SignedS << ").ok_or(SbfError::DivideOverflow)?;\n";
    assign(OS, Instruction, Width32 ? "value as u32" : "value as u64", Indent);
    break;
  }
  case Operation::EndianLE:
    if (Instruction.Immediate == kHalfWordBitWidth)
      assign(OS, Instruction, D + " as u16", Indent);
    else if (Instruction.Immediate == kWordBitWidth)
      assign(OS, Instruction, D + " as u32", Indent);
    else
      assign(OS, Instruction, D, Indent);
    break;
  case Operation::EndianBE:
    if (Instruction.Immediate == kHalfWordBitWidth)
      assign(OS, Instruction, "(" + D + " as u16).swap_bytes()", Indent);
    else if (Instruction.Immediate == kWordBitWidth)
      assign(OS, Instruction, "(" + D + " as u32).swap_bytes()", Indent);
    else
      assign(OS, Instruction, D + ".swap_bytes()", Indent);
    break;
  case Operation::HighOr:
    assign(OS, Instruction,
           D + " | (" + word(static_cast<uint32_t>(Instruction.Immediate)) +
               " << " + std::to_string(kWordBitWidth) + ")",
           Indent);
    break;
  case Operation::Load: {
    const std::string Address =
        reg(Instruction.Src) +
        (Instruction.Offset < 0 ? ".wrapping_sub(" : ".wrapping_add(") +
        word(static_cast<uint64_t>(
            std::abs(static_cast<int>(Instruction.Offset)))) +
        ")";
    assign(OS, Instruction,
           "env.load(" + Address + ", " + std::to_string(Instruction.Width) +
               ")?",
           Indent);
    break;
  }
  case Operation::Store: {
    const std::string Address =
        reg(Instruction.Dst) +
        (Instruction.Offset < 0 ? ".wrapping_sub(" : ".wrapping_add(") +
        word(static_cast<uint64_t>(
            std::abs(static_cast<int>(Instruction.Offset)))) +
        ")";
    OS << Indent << "env.store(" << Address << ", "
       << unsigned(Instruction.Width) << ", " << S << ")?;\n";
    break;
  }
  case Operation::Call:
    if (Instruction.Call != CallKind::Syscall)
      return false;
    OS << Indent << "r[RETURN_REGISTER] = env.syscall(0x"
       << llvm::utohexstr(Instruction.SyscallHash, true) << "u32, "
       << argumentRegisters() << ")?;\n";
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
  OS << "            " << Instruction.Slot << " => { // "
     << opcodeName(Instruction.SourceOpcode) << " @ 0x"
     << llvm::utohexstr(Instruction.Address) << "\n";
  if (emitLinearInstruction(OS, Instruction, "                ")) {
    advance(OS, Instruction, Program.Low.Instructions.size());
    OS << "            }\n";
    return;
  }

  switch (Instruction.Op) {
  case Operation::Jump:
    OS << "                pc = " << Instruction.BranchTarget.value_or(0)
       << ";\n";
    OS << "            }\n";
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
    OS << "                pc = if " << comparison(Instruction) << " { "
       << Instruction.BranchTarget.value_or(0) << " } else { "
       << Instruction.Slot + 1 << " };\n"
       << "            }\n";
    return;
  case Operation::Call:
    if (Instruction.Call == CallKind::Internal && Instruction.CallTarget) {
      pushFrame(OS, Instruction, Program);
      OS << "                pc = " << *Instruction.CallTarget << ";\n"
         << "            }\n";
      return;
    }
    OS << "                return Err(SbfError::UnknownSyscall);\n"
       << "            }\n";
    return;
  case Operation::CallX:
    pushFrame(OS, Instruction, Program);
    OS << "                let target = r["
       << unsigned(Instruction.CallRegister)
       << "];\n"
          "                if target < TEXT_ADDRESS { return "
          "Err(SbfError::UnknownFunction); }\n"
          "                pc = ((target - TEXT_ADDRESS) / INSTRUCTION_SIZE) "
          "as usize;\n"
          "                if pc >= INSTRUCTION_COUNT { return "
          "Err(SbfError::UnknownFunction); }\n"
          "            }\n";
    return;
  case Operation::Exit:
    OS << "                if depth == 0 { return Ok(r[RETURN_REGISTER]); }\n"
          "                depth -= 1;\n"
          "                r[FIRST_SAVED..FIRST_SAVED + SAVED_REGISTERS]"
          ".copy_from_slice(&saved[depth]);\n"
          "                r[FRAME_POINTER] = saved_fp[depth];\n"
          "                pc = return_pc[depth];\n"
          "            }\n";
    return;
  case Operation::Invalid:
    OS << "                return Err(SbfError::InvalidInstruction);\n"
       << "            }\n";
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

} // namespace rust_emitter_detail
} // namespace neverd::sbf
