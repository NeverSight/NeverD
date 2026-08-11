//===- RustEmitter.cpp - Solana SBF to safe Rust backend ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/RustEmitter.h"

#include "neverd/sbf/StructuredCFG.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <map>

namespace neverd::sbf {
namespace {

std::string word(uint64_t Value) {
  return "0x" + llvm::utohexstr(Value, true) + "u64";
}

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

const MedInstruction *
blockTerminator(const BasicBlock &Block,
                const std::map<size_t, const MedInstruction *> &BySlot) {
  const MedInstruction *Last = nullptr;
  for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
    auto It = BySlot.find(Slot);
    if (It != BySlot.end())
      Last = It->second;
  }
  return Last;
}

bool emitStructuredBlock(llvm::raw_ostream &OS, const SBFProgram &Program,
                         const std::map<size_t, const MedInstruction *> &BySlot,
                         size_t BlockID, llvm::StringRef Indent) {
  if (BlockID >= Program.Low.Blocks.size())
    return false;
  const BasicBlock &Block = Program.Low.Blocks[BlockID];
  const MedInstruction *Terminator = blockTerminator(Block, BySlot);
  if (!Terminator)
    return false;

  OS << Indent << "// block_" << BlockID << "\n";
  for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
    auto It = BySlot.find(Slot);
    if (It == BySlot.end())
      continue;
    const MedInstruction &Instruction = *It->second;
    const bool IsTerminator = &Instruction == Terminator;
    if (IsTerminator && (Instruction.Op == Operation::Jump ||
                         Instruction.BranchTarget.has_value()))
      continue;
    if (Instruction.Op == Operation::Exit) {
      OS << Indent << "return Ok(r[RETURN_REGISTER]);\n";
      continue;
    }
    if (!emitLinearInstruction(OS, Instruction, Indent))
      return false;
  }
  return true;
}

bool emitStructuredNodes(llvm::raw_ostream &OS, const SBFProgram &Program,
                         const std::map<size_t, const MedInstruction *> &BySlot,
                         const std::vector<StructuredNode> &Nodes,
                         llvm::StringRef Indent) {
  for (const StructuredNode &Node : Nodes) {
    if (Node.Block >= Program.Low.Blocks.size())
      return false;
    const BasicBlock &Block = Program.Low.Blocks[Node.Block];
    const MedInstruction *Terminator = blockTerminator(Block, BySlot);
    if (!Terminator)
      return false;
    if (Node.Kind == StructuredNodeKind::Block) {
      if (!emitStructuredBlock(OS, Program, BySlot, Node.Block, Indent))
        return false;
      continue;
    }

    if (Node.Kind == StructuredNodeKind::If) {
      if (!emitStructuredBlock(OS, Program, BySlot, Node.Block, Indent))
        return false;
      OS << Indent << "if " << comparison(*Terminator) << " {\n";
      if (!emitStructuredNodes(OS, Program, BySlot, Node.Body,
                               (Indent + "    ").str()))
        return false;
      OS << Indent << "} else {\n";
      if (!emitStructuredNodes(OS, Program, BySlot, Node.Alternative,
                               (Indent + "    ").str()))
        return false;
      OS << Indent << "}\n";
      continue;
    }

    OS << Indent << "loop {\n";
    const std::string Inner = (Indent + "    ").str();
    if (!emitStructuredBlock(OS, Program, BySlot, Node.Block, Inner))
      return false;
    if (Node.ConditionTrueEntersBody)
      OS << Inner << "if !(" << comparison(*Terminator) << ") { break; }\n";
    else
      OS << Inner << "if " << comparison(*Terminator) << " { break; }\n";
    if (!emitStructuredNodes(OS, Program, BySlot, Node.Body, Inner))
      return false;
    OS << Indent << "}\n";
  }
  return true;
}

} // namespace

llvm::Expected<std::string> emitRust(const SBFProgram &Program,
                                     const RustEmitterOptions &Options) {
  if (llvm::Error Error = validateVMConfig(Program.Config))
    return std::move(Error);
  if (Program.Med.Instructions.empty())
    return llvm::make_error<llvm::StringError>(
        "sbf: cannot emit Rust for an empty MedIR",
        llvm::inconvertibleErrorCode());

  std::map<size_t, const MedInstruction *> BySlot;
  for (const MedInstruction &Instruction : Program.Med.Instructions)
    BySlot[Instruction.Slot] = &Instruction;
  const std::optional<StructuredControlFlow> Structured =
      Options.PreferStructuredControlFlow
          ? buildStructuredControlFlow(Program)
          : std::optional<StructuredControlFlow>{};

  bool NeedsSignExtension = false;
  bool NeedsCallFrames = false;
  bool NeedsIndirectCall = false;
  for (const MedInstruction &Instruction : Program.Med.Instructions) {
    NeedsSignExtension |=
        Instruction.Semantics.Result == ResultExtension::Sign32;
    NeedsCallFrames |= Instruction.Call == CallKind::Internal ||
                       Instruction.Op == Operation::CallX;
    NeedsIndirectCall |= Instruction.Op == Operation::CallX;
  }

  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  OS << "// Generated by NeverD from "
     << versionDisplayName(Program.Low.TheVersion)
     << ".\n"
        "#![allow(clippy::unusual_byte_groupings)]\n\n"
        "#[derive(Clone, Copy, Debug, Eq, PartialEq)]\n"
        "pub enum SbfError { InvalidInstruction, MemoryAccess, DivideByZero, "
        "DivideOverflow, CallDepth, UnknownSyscall, UnknownFunction, "
        "ExecutionOverrun }\n\n"
        "pub trait SbfEnvironment {\n"
        "    fn load(&mut self, address: u64, width: u8) -> Result<u64, "
        "SbfError>;\n"
        "    fn store(&mut self, address: u64, width: u8, value: u64) -> "
        "Result<(), SbfError>;\n"
        "    fn syscall(&mut self, hash: u32, args: [u64; "
     << kArgumentRegisterCount
     << "]) -> Result<u64, SbfError>;\n"
        "}\n\n"
        "const REGISTER_COUNT: usize = "
     << kRegisterCount
     << ";\nconst RETURN_REGISTER: usize = " << kReturnRegister
     << ";\nconst INPUT_REGISTER: usize = " << kFirstArgumentRegister
     << ";\nconst FRAME_POINTER: usize = " << kFramePointerRegister << ";\n";
  if (!Structured)
    OS << "const FIRST_SAVED: usize = " << kFirstCalleeSavedRegister
       << ";\nconst SAVED_REGISTERS: usize = " << kCalleeSavedRegisterCount
       << ";\nconst MAX_CALL_DEPTH: usize = " << Program.Config.MaxCallDepth
       << ";\n";
  if (NeedsIndirectCall)
    OS << "const INSTRUCTION_SIZE: u64 = " << kInstructionSize
       << ";\nconst TEXT_ADDRESS: u64 = " << word(Program.Low.TextAddress)
       << ";\nconst INSTRUCTION_COUNT: usize = "
       << Program.Low.Instructions.size() << ";\n";
  OS << "const STACK_START: u64 = " << word(kStackStart) << ";\n";
  if (versionHasFeature(Program.Low.TheVersion,
                        VersionFeature::ManualStackFrames))
    OS << "const STACK_SIZE: u64 = " << stackSize(Program.Config) << "u64;\n\n";
  else
    OS << "const STACK_FRAME_SIZE: u64 = " << Program.Config.StackFrameSize
       << "u64;\n\n";
  if (NeedsSignExtension)
    OS << "#[inline] fn sext32(value: u32) -> u64 { (value as i32 as i64) as "
          "u64 }\n\n";

  if (Options.IncludeAnalysisComments) {
    OS << "// Recovered " << Program.High.Functions.size() << " function(s), "
       << Program.High.Syscalls.size() << " syscall site(s), and "
       << Program.High.Regions.size() << " structured region(s).\n";
    for (const Region &Region : Program.High.Regions)
      OS << "// " << (Region.Kind == RegionKind::Loop ? "loop" : "if")
         << " at block_" << Region.HeaderBlock << "\n";
  }

  OS << "pub fn " << Options.FunctionName
     << "<E: SbfEnvironment>(env: &mut E, input: u64) -> Result<u64, SbfError> "
        "{\n"
        "    let _ = &mut *env;\n"
        "    let mut r = [0u64; REGISTER_COUNT];\n";
  if (!Structured) {
    OS << "    let ";
    if (NeedsCallFrames)
      OS << "mut ";
    OS << "saved = [[0u64; SAVED_REGISTERS]; MAX_CALL_DEPTH];\n"
          "    let ";
    if (NeedsCallFrames)
      OS << "mut ";
    OS << "saved_fp = [0u64; MAX_CALL_DEPTH];\n"
          "    let ";
    if (NeedsCallFrames)
      OS << "mut ";
    OS << "return_pc = [0usize; MAX_CALL_DEPTH];\n"
          "    let mut depth = 0usize;\n"
          "    let mut pc = "
       << Program.Low.EntrySlot << "usize;\n";
  }
  OS << "    r[INPUT_REGISTER] = input; r[FRAME_POINTER] = STACK_START + "
     << (versionHasFeature(Program.Low.TheVersion,
                           VersionFeature::ManualStackFrames)
             ? "STACK_SIZE"
             : "STACK_FRAME_SIZE")
     << ";\n";
  if (Structured) {
    if (!emitStructuredNodes(OS, Program, BySlot, Structured->Body, "    "))
      return llvm::make_error<llvm::StringError>(
          "sbf: structured Rust emission rejected its validated control-flow "
          "plan",
          llvm::inconvertibleErrorCode());
    OS << "}\n";
    return Buffer;
  }

  OS << "    loop {\n        match pc {\n";
  for (size_t Slot = 0; Slot < Program.Low.Instructions.size(); ++Slot) {
    if (Program.Low.Instructions[Slot].IsContinuation) {
      if (NeedsIndirectCall)
        OS << "            " << Slot
           << " => return Err(SbfError::InvalidInstruction),\n";
      continue;
    }
    auto It = BySlot.find(Slot);
    if (It == BySlot.end()) {
      OS << "            " << Slot
         << " => return Err(SbfError::InvalidInstruction),\n";
      continue;
    }
    emitInstruction(OS, *It->second, Program);
  }
  OS << "            _ => return Err(SbfError::ExecutionOverrun),\n"
        "        }\n    }\n}\n";
  return Buffer;
}

} // namespace neverd::sbf
