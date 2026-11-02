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

#include "neverd/sbf/emit/SBFSourceStatus.h"

#include "llvm/ADT/StringExtras.h"

#include <cstdlib>
#include <set>
#include <string>

namespace neverd::sbf {

using namespace rust_emitter_detail;

namespace {

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
    return "((" + Expression.str() + ") as u32) as u64";
  case ResultExtension::Sign32:
    return "sext32((" + Expression.str() + ") as u32)";
  case ResultExtension::None:
    return "(" + Expression.str() + ") as u64";
  }
  return Expression.str();
}

std::string loadHelperName(ResultExtension Extension) {
  return "nd_load_" + std::to_string(static_cast<unsigned>(Extension));
}

void assign(llvm::raw_ostream &OS, const MedInstruction &Instruction,
            llvm::StringRef Expression,
            llvm::StringRef Indent = "                ") {
  OS << Indent << reg(Instruction.Dst) << " = "
     << applyExtension(Instruction, Expression) << ";\n";
}

void advance(llvm::raw_ostream &OS, const MedInstruction &Instruction,
             size_t Count, bool InPartition) {
  const size_t Next = Instruction.Slot + Instruction.SlotWidth;
  if (Next < Count) {
    if (!InPartition)
      OS << "                pc = " << Next << ";\n";
    else if (Instruction.SlotWidth > 1)
      OS << "                pc += " << Instruction.SlotWidth - 1 << ";\n";
  } else {
    if (InPartition)
      OS << "                if let Some(error) = fault { return Err(error); "
            "}\n";
    OS << "                return Err("
       << rustSourceErrorName(FaultCode::ExecutionOverrun) << ");\n";
  }
}

void emitPushFrameBody(llvm::raw_ostream &OS, llvm::StringRef ReturnPC,
                       const SBFProgram &Program, bool RecordFault = false) {
  OS << "                if depth + 1 >= MAX_CALL_DEPTH { ";
  if (RecordFault)
    OS << "*fault = Some(" << rustSourceErrorName(FaultCode::CallDepth)
       << "); return; }\n";
  else
    OS << "return Err(" << rustSourceErrorName(FaultCode::CallDepth)
       << "); }\n";
  OS << "                return_pc[depth] = " << ReturnPC << ";\n";
  for (unsigned Index = 0; Index < kCalleeSavedRegisterCount; ++Index)
    OS << "                saved[depth][" << Index
       << "] = " << reg(kFirstCalleeSavedRegister + Index) << ";\n";
  OS << "                saved_fp[depth] = " << reg(kFramePointerRegister)
     << ";\n                depth += 1;\n";
  if (!versionHasFeature(Program.Low.TheVersion,
                         VersionFeature::ManualStackFrames)) {
    OS << "                " << reg(kFramePointerRegister) << " = "
       << reg(kFramePointerRegister) << ".wrapping_add("
       << automaticFrameStride(Program.Low.TheVersion, Program.Config)
       << "u64);\n";
  }
}

void emitPopFrameBody(llvm::raw_ostream &OS) {
  OS << "                depth -= 1;\n";
  for (unsigned Index = 0; Index < kCalleeSavedRegisterCount; ++Index)
    OS << "                " << reg(kFirstCalleeSavedRegister + Index)
       << " = saved[depth][" << Index << "];\n";
  OS << "                " << reg(kFramePointerRegister)
     << " = saved_fp[depth];\n";
}

void emitLegacyRuntimeBody(llvm::raw_ostream &OS, llvm::StringRef Hash) {
  OS << "                let runtime_features = env.runtime_features()"
        ".unwrap_or(PROGRAM_RUNTIME_FEATURES);\n"
        "                match env.syscall_with_features("
        "SbfSyscallInvocation { hash: "
     << Hash << ", args: " << argumentRegisters()
     << ", runtime_features }) {\n"
        "                    SbfSyscallOutcomeV2::Returned(value) => "
     << reg(kReturnRegister)
     << " = value,\n"
        "                    SbfSyscallOutcomeV2::Unregistered => {},\n"
        "                    SbfSyscallOutcomeV2::Fault("
     << rustSourceErrorName(FaultCode::UnknownSyscall)
     << ") => {},\n"
        "                    SbfSyscallOutcomeV2::Fault(error) => return "
        "Err(error),\n"
        "                }\n";
}

void pushFrame(llvm::raw_ostream &OS, const MedInstruction &Instruction,
               const SBFProgram &Program, bool InPartition) {
  if (!InPartition) {
    emitPushFrameBody(OS, std::to_string(Instruction.Slot + 1), Program);
    return;
  }
  const bool Legacy =
      Instruction.Op == Operation::Call &&
      Instruction.Dispatch == CallDispatchPolicy::LegacyRuntimeThenFunction;
  OS << "                "
     << (Legacy ? "nd_legacy_push_frame(env, r, stack, "
                : "nd_push_frame(r, stack, ")
     << Instruction.Slot + 1;
  if (Legacy)
    OS << ", 0x" << llvm::utohexstr(Instruction.SyscallHash, true) << "u32";
  OS << ", &mut fault);\n";
}

} // namespace

namespace rust_emitter_detail {

std::string reg(unsigned Register) { return "r.r" + std::to_string(Register); }

void emitMemoryHelpers(llvm::raw_ostream &OS, const SBFProgram &Program) {
  std::set<ResultExtension> Loads;
  bool Stores = false;
  for (const auto &Instruction : Program.Med.Instructions) {
    if (Instruction.Op == Operation::Load)
      Loads.insert(Instruction.Semantics.Result);
    Stores |= Instruction.Op == Operation::Store;
  }
  for (ResultExtension Extension : Loads) {
    MedInstruction Load;
    Load.Semantics.Result = Extension;
    OS << "#[inline(never)]\nfn " << loadHelperName(Extension)
       << "(env: &mut dyn SbfEnvironmentV2, address: u64, width: u8, "
          "target: &mut u64, fault: &mut Option<SbfErrorV2>) {\n"
          "    match env.load(address, width) {\n"
          "        Ok(value) => *target = "
       << applyExtension(Load, "value")
       << ",\n        Err(error) => *fault = Some(error),\n    }\n}\n\n";
  }
  if (Stores)
    OS << "#[inline(never)]\nfn nd_store(env: &mut dyn SbfEnvironmentV2, "
          "address: u64, width: u8, value: u64, fault: &mut "
          "Option<SbfErrorV2>) {\n"
          "    if let Err(error) = env.store(address, width, value) { "
          "*fault = Some(error); }\n}\n\n";
}

void emitCallHelpers(llvm::raw_ostream &OS, const SBFProgram &Program) {
  bool Needed = false;
  bool NeedsPop = false;
  for (const auto &Instruction : Program.Med.Instructions)
    NeedsPop |= Instruction.Op == Operation::Exit;
  for (const auto &Instruction : Program.Med.Instructions)
    Needed |=
        Instruction.Op == Operation::CallX ||
        (Instruction.Op == Operation::Call &&
         Instruction.Call == CallKind::Internal && Instruction.CallTarget);
  bool NeedsLegacyRuntime = false;
  for (const auto &Instruction : Program.Med.Instructions)
    NeedsLegacyRuntime |=
        Instruction.Op == Operation::Call &&
        Instruction.Call == CallKind::Internal && Instruction.CallTarget &&
        Instruction.Dispatch == CallDispatchPolicy::LegacyRuntimeThenFunction;
  if (NeedsLegacyRuntime) {
    OS << "#[inline(never)]\nfn nd_legacy_runtime_call("
          "env: &mut dyn SbfEnvironmentV2, r: &mut NdRegisters, "
          "hash: u32) -> "
          "Result<(), SbfErrorV2> {\n";
    emitLegacyRuntimeBody(OS, "hash");
    OS << "    Ok(())\n}\n\n";
  }
  if (NeedsPop) {
    OS << "#[inline(never)]\nfn nd_pop_frame(r: &mut NdRegisters, "
          "stack: &mut NdCallStack) -> usize {\n"
          "    let mut depth = stack.depth;\n"
          "    let saved = &stack.saved; let saved_fp = &stack.saved_fp;\n"
          "    let return_pc = &stack.return_pc;\n";
    emitPopFrameBody(OS);
    OS << "    stack.depth = depth; return_pc[depth]\n}\n\n";
  }
  if (!Needed)
    return;
  OS << "#[inline(never)]\nfn nd_push_frame(r: &mut NdRegisters, "
        "stack: &mut NdCallStack, next_pc: usize, "
        "fault: &mut Option<SbfErrorV2>) {\n"
        "    let mut depth = stack.depth;\n"
        "    let saved = &mut stack.saved; let saved_fp = &mut "
        "stack.saved_fp;\n"
        "    let return_pc = &mut stack.return_pc;\n";
  emitPushFrameBody(OS, "next_pc", Program, true);
  OS << "    stack.depth = depth;\n}\n\n";
  if (NeedsLegacyRuntime)
    OS << "#[inline(never)]\nfn nd_legacy_push_frame("
          "env: &mut dyn SbfEnvironmentV2, r: &mut NdRegisters, "
          "stack: &mut NdCallStack, next_pc: usize, hash: u32, "
          "fault: &mut Option<SbfErrorV2>) {\n"
          "    if let Err(error) = nd_legacy_runtime_call(env, r, hash) {\n"
          "        *fault = Some(error); return;\n"
          "    }\n"
          "    nd_push_frame(r, stack, next_pc, fault);\n}\n\n";
}

std::string word(uint64_t Value) {
  return "0x" + llvm::utohexstr(Value, true) + "u64";
}

std::string comparison(const MedInstruction &Instruction) {
  if (const auto Result = Instruction.reflexiveComparisonResult())
    return *Result ? "true" : "false";
  const std::string L = reg(Instruction.Dst);
  const std::string R = source(Instruction);
  const std::string LU =
      Instruction.Width == kWordBitWidth ? "(" + L + " as u32)" : L;
  const std::string RU =
      Instruction.Width == kWordBitWidth ? "(" + R + " as u32)" : R;
  const std::string SignedType =
      Instruction.Width == kWordBitWidth ? "i32" : "i64";
  const std::string LS = "(" + LU + " as " + SignedType + ")";
  const std::string RS = "(" + RU + " as " + SignedType + ")";
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
    return LS + " > " + RS;
  case Operation::SGe:
    return LS + " >= " + RS;
  case Operation::SLt:
    return LS + " < " + RS;
  case Operation::SLe:
    return LS + " <= " + RS;
  case Operation::Set:
    return "(" + LU + " & " + RU + ") != 0";
  default:
    return "false";
  }
}

bool emitLinearInstruction(llvm::raw_ostream &OS,
                           const MedInstruction &Instruction,
                           llvm::StringRef Indent, bool SharedMemoryFault) {
  if (Instruction.Op == Operation::Invalid)
    return false;
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
    OS << Indent << "if " << Divisor << " == 0 { return Err("
       << rustSourceErrorName(FaultCode::DivideByZero) << "); }\n";
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
    OS << Indent << "if " << SignedS << " == 0 { return Err("
       << rustSourceErrorName(FaultCode::DivideByZero) << "); }\n"
       << Indent << "let value = " << SignedD
       << (Instruction.Op == Operation::SDiv ? ".checked_div("
                                             : ".checked_rem(")
       << SignedS << ").ok_or("
       << rustSourceErrorName(FaultCode::DivideOverflow) << ")?;\n";
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
    if (SharedMemoryFault)
      OS << Indent << loadHelperName(Instruction.Semantics.Result) << "(env, "
         << Address << ", " << unsigned(Instruction.Width) << ", &mut " << D
         << ", &mut fault);\n";
    else
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
    if (SharedMemoryFault)
      OS << Indent << "nd_store(env, " << Address << ", "
         << unsigned(Instruction.Width) << ", " << S << ", &mut fault);\n";
    else
      OS << Indent << "env.store(" << Address << ", "
         << unsigned(Instruction.Width) << ", " << S << ")?;\n";
    break;
  }
  case Operation::Call:
    if (Instruction.Call != CallKind::Syscall &&
        Instruction.Call != CallKind::Unresolved)
      return false;
    OS << Indent
       << "let runtime_features = env.runtime_features()"
          ".unwrap_or(PROGRAM_RUNTIME_FEATURES);\n"
       << Indent << "match env.syscall_with_features(SbfSyscallInvocation { "
       << "hash: 0x" << llvm::utohexstr(Instruction.SyscallHash, true)
       << "u32, args: " << argumentRegisters() << ", runtime_features }) {\n"
       << Indent << "    SbfSyscallOutcomeV2::Returned(value) => "
       << reg(kReturnRegister) << " = value,\n"
       << Indent << "    SbfSyscallOutcomeV2::Unregistered => return Err("
       << rustSourceErrorName(FaultCode::UnknownSyscall) << "),\n"
       << Indent
       << "    SbfSyscallOutcomeV2::Fault(error) => return Err(error),\n"
       << Indent << "}\n";
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
                     const SBFProgram &Program, bool InPartition) {
  OS << "            " << Instruction.Slot << " => { // "
     << opcodeName(Instruction.SourceOpcode) << " @ 0x"
     << llvm::utohexstr(Instruction.Address) << "\n";
  if (emitLinearInstruction(OS, Instruction, "                ", InPartition)) {
    advance(OS, Instruction, Program.Low.Instructions.size(), InPartition);
    OS << "            }\n";
    return;
  }

  switch (Instruction.Op) {
  case Operation::Jump:
    if (!Instruction.BranchTarget) {
      OS << "                return Err("
         << rustSourceErrorName(FaultCode::InvalidBranch) << ");\n"
         << "            }\n";
      return;
    }
    OS << "                pc = " << *Instruction.BranchTarget << ";\n";
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
    if (!Instruction.BranchTarget) {
      OS << "                return Err("
         << rustSourceErrorName(FaultCode::InvalidBranch) << ");\n"
         << "            }\n";
      return;
    }
    OS << "                pc = if " << comparison(Instruction) << " { "
       << *Instruction.BranchTarget << " } else { " << Instruction.Slot + 1
       << " };\n"
       << "            }\n";
    return;
  case Operation::Call:
    if (Instruction.Call == CallKind::Unsupported) {
      OS << "                return Err("
         << rustSourceErrorName(FaultCode::InvalidInstruction) << ");\n"
         << "            }\n";
      return;
    }
    if (Instruction.Call == CallKind::Internal && Instruction.CallTarget) {
      if (!InPartition && Instruction.Dispatch ==
                              CallDispatchPolicy::LegacyRuntimeThenFunction) {
        const std::string Hash =
            "0x" + llvm::utohexstr(Instruction.SyscallHash, true) + "u32";
        emitLegacyRuntimeBody(OS, Hash);
      }
      pushFrame(OS, Instruction, Program, InPartition);
      OS << "                pc = " << *Instruction.CallTarget << ";\n"
         << "            }\n";
      return;
    }
    OS << "                return Err("
       << rustSourceErrorName(FaultCode::UnknownSyscall) << ");\n"
       << "            }\n";
    return;
  case Operation::CallX:
    pushFrame(OS, Instruction, Program, InPartition);
    // Stack entry faults precede target validation. Direct calls consume
    // their recorded fault at the region's common post-step check.
    if (InPartition)
      OS << "                if let Some(error) = fault { return Err(error); "
            "}\n";
    OS << "                let target = " << reg(Instruction.CallRegister)
       << ";\n"
          "                if target < TEXT_ADDRESS { return Err("
       << rustSourceErrorName(FaultCode::UnknownIndirectCall)
       << "); }\n"
          "                let target_slot = (target - TEXT_ADDRESS) / "
          "INSTRUCTION_SIZE;\n"
          "                if target_slot >= INSTRUCTION_COUNT as u64 { "
          "return Err("
       << rustSourceErrorName(FaultCode::UnknownIndirectCall)
       << "); }\n"
          "                pc = target_slot as usize;\n"
          "            }\n";
    return;
  case Operation::Exit:
    OS << "                if " << (InPartition ? "stack.depth" : "depth")
       << " == 0 { return Ok(";
    if (InPartition)
      OS << "Some(";
    OS << reg(kReturnRegister);
    if (InPartition)
      OS << ")";
    OS << "); }\n";
    if (InPartition)
      OS << "                pc = nd_pop_frame(r, stack);\n";
    else {
      emitPopFrameBody(OS);
      OS << "                pc = return_pc[depth];\n";
    }
    OS << "            }\n";
    return;
  case Operation::Invalid:
    OS << "                return Err("
       << rustSourceErrorName(
              executionFaultForValidationRule(Instruction.InvalidReason))
       << ");\n"
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
