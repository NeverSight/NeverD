//===- CEmitter.cpp - Solana SBF to portable C backend ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/CEmitter.h"

#include "neverd/sbf/Analyzer.h"
#include "neverd/sbf/StructuredCFG.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <map>

namespace neverd::sbf {
namespace {

std::string word(uint64_t Value) {
  return "UINT64_C(0x" + llvm::utohexstr(Value) + ")";
}

std::string reg(unsigned Register) {
  return "r[" + std::to_string(Register) + "]";
}

std::string immediate(const MedInstruction &Instruction) {
  switch (Instruction.ImmediateMode) {
  case ImmediateExtension::Zero32:
    return word(static_cast<uint32_t>(Instruction.Immediate));
  case ImmediateExtension::Full64:
    return word(Instruction.Immediate);
  case ImmediateExtension::Sign32:
    return word(static_cast<uint64_t>(
        static_cast<int64_t>(static_cast<int32_t>(Instruction.Immediate))));
  }
  return word(Instruction.Immediate);
}

std::string source(const MedInstruction &Instruction) {
  switch (Instruction.Form) {
  case OperandForm::DstSrc:
  case OperandForm::BranchReg:
  case OperandForm::StoreReg:
    return reg(Instruction.Src);
  default:
    return immediate(Instruction);
  }
}

void emitAssign(llvm::raw_ostream &OS, const MedInstruction &Instruction,
                llvm::StringRef Expression,
                llvm::StringRef Indent = "        ") {
  OS << Indent << reg(Instruction.Dst) << " = ";
  switch (Instruction.Extension) {
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

std::string comparison(const MedInstruction &Instruction) {
  const std::string L = reg(Instruction.Dst);
  const std::string R = source(Instruction);
  const bool Width32 = Instruction.Width == 32;
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
               Instruction.SwapOperands ? S + " - " + D : D + " - " + S,
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
               (Instruction.Width == 32 ? "(uint32_t)" : "") + D +
                   " >> ((uint32_t)" + S + " & " +
                   std::to_string(Instruction.Width - 1) + "u)",
               Indent);
    break;
  case Operation::ARSh:
    emitAssign(
        OS, Instruction,
        std::string(Instruction.Width == 32 ? "nd_ashr32(" : "nd_ashr64(") +
            (Instruction.Width == 32 ? "(uint32_t)" : "") + D + ", " + S + ")",
        Indent);
    break;
  case Operation::UDiv:
  case Operation::URem:
    OS << Indent << "if ((" << (Instruction.Width == 32 ? "(uint32_t)" : "")
       << S << ") == 0) return NEVERD_SBF_DIVIDE_BY_ZERO;\n";
    emitAssign(OS, Instruction,
               (Instruction.Width == 32 ? "(uint32_t)" : "") + D +
                   (Instruction.Op == Operation::UDiv ? " / " : " % ") +
                   (Instruction.Width == 32 ? "(uint32_t)" : "") + S,
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
    if (Instruction.Immediate == 16)
      emitAssign(OS, Instruction, "(uint16_t)" + D, Indent);
    else if (Instruction.Immediate == 32)
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
                   " << 32)",
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
       << llvm::utohexstr(Instruction.SyscallHash)
       << "), r[1], r[2], r[3], r[4], r[5], &value) != 0) return "
          "NEVERD_SBF_UNKNOWN_SYSCALL; r[0] = value; }\n";
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

void emitAdvance(llvm::raw_ostream &OS, const MedInstruction &Instruction,
                 size_t InstructionCount) {
  const size_t Next =
      Instruction.Slot + (Instruction.SourceOpcode == Opcode::LDDW ? 2 : 1);
  if (Next < InstructionCount)
    OS << "        pc = " << Next << "; continue;\n";
  else
    OS << "        return NEVERD_SBF_EXECUTION_OVERRUN;\n";
}

void emitPushFrame(llvm::raw_ostream &OS, const MedInstruction &Instruction,
                   Version Version) {
  OS << "        if (depth + 1 >= NEVERD_SBF_MAX_CALL_DEPTH) return "
        "NEVERD_SBF_CALL_DEPTH;\n"
     << "        return_pc[depth] = " << Instruction.Slot + 1 << ";\n"
     << "        for (i = 0; i < NEVERD_SBF_SAVED_REGISTERS; ++i) "
        "saved[depth][i] = r[NEVERD_SBF_FIRST_SAVED_REGISTER + i];\n"
     << "        saved_fp[depth] = r[NEVERD_SBF_FRAME_POINTER];\n"
     << "        ++depth;\n";
  if (!versionHasFeature(Version, VersionFeature::ManualStackFrames)) {
    OS << "        r[NEVERD_SBF_FRAME_POINTER] += UINT64_C("
       << automaticFrameStride(Version) << ");\n";
  }
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
      emitPushFrame(OS, Instruction, Program.Low.TheVersion);
      OS << "        pc = " << *Instruction.CallTarget << "; continue;\n";
      return;
    }
    OS << "        return NEVERD_SBF_UNKNOWN_FUNCTION;\n";
    return;
  case Operation::CallX:
    emitPushFrame(OS, Instruction, Program.Low.TheVersion);
    OS << "        { uint64_t target = r[" << unsigned(Instruction.CallRegister)
       << "]; if (target < NEVERD_SBF_TEXT_ADDRESS) return "
          "NEVERD_SBF_UNKNOWN_FUNCTION; pc = (uint32_t)((target - "
          "NEVERD_SBF_TEXT_ADDRESS) / NEVERD_SBF_INSTRUCTION_SIZE); "
          "if (pc >= NEVERD_SBF_INSTRUCTION_COUNT) return "
          "NEVERD_SBF_UNKNOWN_FUNCTION; continue; }\n";
    return;
  case Operation::Exit:
    OS << "        if (depth == 0) { if (result) *result = r[0]; return "
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

  OS << Indent << "/* block_" << BlockID << " */\n";
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
      OS << Indent << "if (result) *result = r[0];\n"
         << Indent << "return NEVERD_SBF_OK;\n";
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
      OS << Indent << "if (" << comparison(*Terminator) << ") {\n";
      if (!emitStructuredNodes(OS, Program, BySlot, Node.Body,
                               (Indent + "  ").str()))
        return false;
      OS << Indent << "} else {\n";
      if (!emitStructuredNodes(OS, Program, BySlot, Node.Alternative,
                               (Indent + "  ").str()))
        return false;
      OS << Indent << "}\n";
      continue;
    }

    OS << Indent << "while (1) {\n";
    const std::string Inner = (Indent + "  ").str();
    if (!emitStructuredBlock(OS, Program, BySlot, Node.Block, Inner))
      return false;
    if (Node.ConditionTrueEntersBody)
      OS << Inner << "if (!(" << comparison(*Terminator) << ")) break;\n";
    else
      OS << Inner << "if (" << comparison(*Terminator) << ") break;\n";
    if (!emitStructuredNodes(OS, Program, BySlot, Node.Body, Inner))
      return false;
    OS << Indent << "}\n";
  }
  return true;
}

} // namespace

llvm::Expected<std::string> emitC(const SBFProgram &Program,
                                  const CEmitterOptions &Options) {
  if (Program.Med.Instructions.empty())
    return llvm::make_error<llvm::StringError>(
        "sbf: cannot emit C for an empty MedIR",
        llvm::inconvertibleErrorCode());

  std::map<size_t, const MedInstruction *> BySlot;
  for (const MedInstruction &Instruction : Program.Med.Instructions)
    BySlot[Instruction.Slot] = &Instruction;
  const std::optional<StructuredControlFlow> Structured =
      Options.PreferStructuredControlFlow
          ? buildStructuredControlFlow(Program)
          : std::optional<StructuredControlFlow>{};

  bool NeedsSignExtension = false;
  bool NeedsSignedCompare32 = false;
  bool NeedsSignedCompare64 = false;
  bool NeedsArithmeticShift32 = false;
  bool NeedsArithmeticShift64 = false;
  bool NeedsUnsignedHighMultiply = false;
  bool NeedsSignedHighMultiply = false;
  bool NeedsByteSwap = false;
  bool NeedsSignedDivision = false;
  for (const MedInstruction &Instruction : Program.Med.Instructions) {
    NeedsSignExtension |= Instruction.Extension == ResultExtension::Sign32;
    const bool SignedCompare =
        Instruction.Op == Operation::SGt || Instruction.Op == Operation::SGe ||
        Instruction.Op == Operation::SLt || Instruction.Op == Operation::SLe;
    NeedsSignedCompare32 |= SignedCompare && Instruction.Width == 32;
    NeedsSignedCompare64 |= SignedCompare && Instruction.Width == 64;
    NeedsArithmeticShift32 |=
        Instruction.Op == Operation::ARSh && Instruction.Width == 32;
    NeedsArithmeticShift64 |=
        Instruction.Op == Operation::ARSh && Instruction.Width == 64;
    NeedsUnsignedHighMultiply |= Instruction.Op == Operation::UHighMul ||
                                 Instruction.Op == Operation::SHighMul;
    NeedsSignedHighMultiply |= Instruction.Op == Operation::SHighMul;
    NeedsByteSwap |= Instruction.Op == Operation::EndianBE;
    NeedsSignedDivision |=
        Instruction.Op == Operation::SDiv || Instruction.Op == Operation::SRem;
  }

  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  OS << "/* Generated by NeverD from "
     << versionDisplayName(Program.Low.TheVersion)
     << ". */\n"
        "#include <stddef.h>\n#include <stdint.h>\n#include <limits.h>\n\n"
        "typedef enum neverd_sbf_status {\n"
        "  NEVERD_SBF_OK = 0, NEVERD_SBF_INVALID_INSTRUCTION,\n"
        "  NEVERD_SBF_MEMORY_ACCESS, NEVERD_SBF_DIVIDE_BY_ZERO,\n"
        "  NEVERD_SBF_DIVIDE_OVERFLOW, NEVERD_SBF_CALL_DEPTH,\n"
        "  NEVERD_SBF_UNKNOWN_SYSCALL, NEVERD_SBF_UNKNOWN_FUNCTION,\n"
        "  NEVERD_SBF_EXECUTION_OVERRUN\n"
        "} neverd_sbf_status;\n\n"
        "typedef struct neverd_sbf_environment {\n"
        "  void *context;\n"
        "  int (*load)(void *, uint64_t, uint32_t, uint64_t *);\n"
        "  int (*store)(void *, uint64_t, uint32_t, uint64_t);\n"
        "  int (*syscall)(void *, uint32_t, uint64_t, uint64_t, uint64_t, "
        "uint64_t, uint64_t, uint64_t *);\n"
        "} neverd_sbf_environment;\n\n"
        "enum { NEVERD_SBF_REGISTER_COUNT = "
     << kRegisterCount
     << ", NEVERD_SBF_FRAME_POINTER = " << kFramePointerRegister
     << ", NEVERD_SBF_FIRST_SAVED_REGISTER = " << kFirstCalleeSavedRegister
     << ", NEVERD_SBF_SAVED_REGISTERS = " << kCalleeSavedRegisterCount
     << ", NEVERD_SBF_MAX_CALL_DEPTH = " << kDefaultMaxCallDepth
     << " };\n"
        "#define NEVERD_SBF_TEXT_ADDRESS "
     << word(Program.Low.TextAddress)
     << "\n"
        "#define NEVERD_SBF_INSTRUCTION_SIZE UINT64_C("
     << kInstructionSize
     << ")\n"
        "#define NEVERD_SBF_INSTRUCTION_COUNT UINT32_C("
     << Program.Low.Instructions.size()
     << ")\n"
        "#define NEVERD_SBF_STACK_START "
     << word(kStackStart)
     << "\n"
        "#define NEVERD_SBF_STACK_FRAME_SIZE UINT64_C("
     << kDefaultStackFrameSize
     << ")\n"
        "#define NEVERD_SBF_STACK_SIZE UINT64_C("
     << kDefaultStackSize << ")\n\n";

  if (NeedsSignExtension)
    OS << "static uint64_t nd_sext32(uint32_t v) { return (v & "
          "UINT32_C(0x80000000)) ? (UINT64_C(0xffffffff00000000) | v) : v; }\n";
  if (NeedsSignedCompare32)
    OS << "static int nd_sgt32(uint32_t a, uint32_t b) { return (a ^ "
          "UINT32_C(0x80000000)) > (b ^ UINT32_C(0x80000000)); }\n";
  if (NeedsSignedCompare64)
    OS << "static int nd_sgt64(uint64_t a, uint64_t b) { return (a ^ "
          "UINT64_C(0x8000000000000000)) > (b ^ "
          "UINT64_C(0x8000000000000000)); }\n";
  if (NeedsArithmeticShift32)
    OS << "static uint32_t nd_ashr32(uint32_t v, uint64_t s) { unsigned n = "
          "(unsigned)s & 31u; if (!n) return v; return (v >> n) | ((v & "
          "UINT32_C(0x80000000)) ? (~UINT32_C(0) << (32u - n)) : 0); }\n";
  if (NeedsArithmeticShift64)
    OS << "static uint64_t nd_ashr64(uint64_t v, uint64_t s) { unsigned n = "
          "(unsigned)s & 63u; if (!n) return v; return (v >> n) | ((v & "
          "UINT64_C(0x8000000000000000)) ? (~UINT64_C(0) << (64u - n)) : 0); "
          "}\n";
  if (NeedsUnsignedHighMultiply)
    OS << "static uint64_t nd_umulh64(uint64_t a, uint64_t b) { uint64_t a0 "
          "= (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32; "
          "uint64_t w0 = a0*b0, t = a1*b0 + (w0 >> 32), w1 = (uint32_t)t, "
          "w2 = t >> 32; w1 += a0*b1; return a1*b1 + w2 + (w1 >> 32); }\n";
  if (NeedsSignedHighMultiply)
    OS << "static uint64_t nd_smulh64(uint64_t a, uint64_t b) { return "
          "nd_umulh64(a,b) - ((a >> 63) ? b : 0) - ((b >> 63) ? a : 0); }\n";
  if (NeedsByteSwap)
    OS << "static uint64_t nd_bswap(uint64_t v, unsigned bits) { if (bits == "
          "16) return ((v & 0xffu) << 8) | ((v >> 8) & 0xffu); if (bits == "
          "32) { v = ((v & UINT64_C(0x00ff00ff)) << 8) | ((v >> 8) & "
          "UINT64_C(0x00ff00ff)); return (v << 16) | (v >> 16); } v = ((v "
          "& UINT64_C(0x00ff00ff00ff00ff)) << 8) | ((v >> 8) & "
          "UINT64_C(0x00ff00ff00ff00ff)); v = ((v & "
          "UINT64_C(0x0000ffff0000ffff)) << 16) | ((v >> 16) & "
          "UINT64_C(0x0000ffff0000ffff)); return (v << 32) | (v >> 32); }\n";
  if (NeedsSignedDivision)
    OS << "static uint64_t nd_sdivrem(uint64_t a, uint64_t b, unsigned bits, "
          "int rem, int *fault) { uint64_t mask = bits == 32 ? UINT32_MAX : "
          "UINT64_MAX, sign = bits == 32 ? (UINT64_C(1)<<31) : "
          "(UINT64_C(1)<<63); a &= mask; b &= mask; if (!b) { *fault = "
          "NEVERD_SBF_DIVIDE_BY_ZERO; return 0; } if (a == sign && b == mask) "
          "{ *fault = NEVERD_SBF_DIVIDE_OVERFLOW; return 0; } { int na = "
          "(a&sign)!=0, nb=(b&sign)!=0; uint64_t ua=na?((~a+1)&mask):a, "
          "ub=nb?((~b+1)&mask):b, value=rem?(ua%ub):(ua/ub); if ((rem?na:"
          "(na!=nb)) && value) value=(~value+1)&mask; return value; } }\n";
  OS << "\n";

  if (Options.IncludeAnalysisComments) {
    OS << "/* Recovered: " << Program.High.Functions.size() << " function(s), "
       << Program.High.Syscalls.size() << " syscall site(s), "
       << Program.High.Regions.size() << " structured region(s). */\n";
    for (const Region &Region : Program.High.Regions)
      OS << "/* " << (Region.Kind == RegionKind::Loop ? "loop" : "if")
         << " at block_" << Region.HeaderBlock << " */\n";
  }

  OS << "neverd_sbf_status " << Options.FunctionName
     << "(neverd_sbf_environment *env, uint64_t input, uint64_t *result) {\n"
        "  (void)env;\n"
        "  (void)result;\n"
        "  uint64_t r[NEVERD_SBF_REGISTER_COUNT] = {0};\n";
  if (!Structured)
    OS << "  uint64_t "
          "saved[NEVERD_SBF_MAX_CALL_DEPTH][NEVERD_SBF_SAVED_REGISTERS] = "
          "{{0}};\n"
          "  uint64_t saved_fp[NEVERD_SBF_MAX_CALL_DEPTH] = {0};\n"
          "  uint32_t return_pc[NEVERD_SBF_MAX_CALL_DEPTH] = {0};\n"
          "  size_t depth = 0, i = 0; uint32_t pc = "
       << Program.Low.EntrySlot << ";\n";
  OS << "  r[1] = input; r[NEVERD_SBF_FRAME_POINTER] = NEVERD_SBF_STACK_START "
        "+ "
     << (versionHasFeature(Program.Low.TheVersion,
                           VersionFeature::ManualStackFrames)
             ? "NEVERD_SBF_STACK_SIZE"
             : "NEVERD_SBF_STACK_FRAME_SIZE")
     << ";\n";
  if (Structured) {
    if (!emitStructuredNodes(OS, Program, BySlot, Structured->Body, "  "))
      return llvm::make_error<llvm::StringError>(
          "sbf: structured C emission rejected its validated control-flow plan",
          llvm::inconvertibleErrorCode());
    OS << "  return NEVERD_SBF_EXECUTION_OVERRUN;\n}\n";
    return Buffer;
  }

  OS << "  for (;;) {\n    switch (pc) {\n";
  for (size_t Slot = 0; Slot < Program.Low.Instructions.size(); ++Slot) {
    if (Program.Low.Instructions[Slot].IsContinuation)
      continue;
    auto It = BySlot.find(Slot);
    if (It == BySlot.end()) {
      OS << "      case " << Slot
         << ": return NEVERD_SBF_INVALID_INSTRUCTION;\n";
      continue;
    }
    emitInstruction(OS, *It->second, Program);
  }
  OS << "      default: return NEVERD_SBF_EXECUTION_OVERRUN;\n"
        "    }\n  }\n}\n";
  return Buffer;
}

} // namespace neverd::sbf
