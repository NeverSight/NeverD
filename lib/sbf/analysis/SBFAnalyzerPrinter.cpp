//===- SBFAnalyzerPrinter.cpp - Textual dumps of the SBF IR stages --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Renders the staged SBF intermediate representations as text: one
/// instruction, the LowIR blocks and edges, the MedIR stream, and the HighIR
/// summary together with the recovered Solana model.
///
//===----------------------------------------------------------------------===//

#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/solana/SBFSolanaRecovery.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <string>

namespace neverd::sbf {
namespace {

llvm::StringRef edgeKindName(EdgeKind Kind) {
  return getEdgeKindInfo(Kind).IRName;
}

llvm::StringRef operationName(Operation Op) {
  switch (Op) {
#define SBF_OPERATION_CASE(NAME)                                               \
  case Operation::NAME:                                                        \
    return #NAME
    SBF_OPERATION_CASE(LoadImm);
    SBF_OPERATION_CASE(Load);
    SBF_OPERATION_CASE(Store);
    SBF_OPERATION_CASE(Add);
    SBF_OPERATION_CASE(Sub);
    SBF_OPERATION_CASE(Mul);
    SBF_OPERATION_CASE(UHighMul);
    SBF_OPERATION_CASE(SHighMul);
    SBF_OPERATION_CASE(UDiv);
    SBF_OPERATION_CASE(URem);
    SBF_OPERATION_CASE(SDiv);
    SBF_OPERATION_CASE(SRem);
    SBF_OPERATION_CASE(Or);
    SBF_OPERATION_CASE(And);
    SBF_OPERATION_CASE(Xor);
    SBF_OPERATION_CASE(LSh);
    SBF_OPERATION_CASE(RSh);
    SBF_OPERATION_CASE(ARSh);
    SBF_OPERATION_CASE(Neg);
    SBF_OPERATION_CASE(Mov);
    SBF_OPERATION_CASE(EndianLE);
    SBF_OPERATION_CASE(EndianBE);
    SBF_OPERATION_CASE(HighOr);
    SBF_OPERATION_CASE(Jump);
    SBF_OPERATION_CASE(Eq);
    SBF_OPERATION_CASE(Ne);
    SBF_OPERATION_CASE(UGt);
    SBF_OPERATION_CASE(UGe);
    SBF_OPERATION_CASE(ULt);
    SBF_OPERATION_CASE(ULe);
    SBF_OPERATION_CASE(SGt);
    SBF_OPERATION_CASE(SGe);
    SBF_OPERATION_CASE(SLt);
    SBF_OPERATION_CASE(SLe);
    SBF_OPERATION_CASE(Set);
    SBF_OPERATION_CASE(Call);
    SBF_OPERATION_CASE(CallX);
    SBF_OPERATION_CASE(Exit);
    SBF_OPERATION_CASE(Invalid);
#undef SBF_OPERATION_CASE
  }
  return "Invalid";
}

} // namespace

std::string formatInstruction(const LowInstruction &Instruction) {
  if (Instruction.IsContinuation)
    return ".lddw.cont";
  if (Instruction.isInvalid()) {
    const ValidationRuleInfo RuleInfo =
        getValidationRuleInfo(Instruction.InvalidReason);
    return (llvm::Twine("invalid[") + RuleInfo.StableID + "] .byte 0x" +
            llvm::utohexstr(Instruction.RawOpcode))
        .str();
  }
  if (!Instruction.Info)
    return ".byte 0x" + llvm::utohexstr(Instruction.RawOpcode);
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  auto PrintBranchTarget = [&] {
    if (Instruction.BranchTarget)
      OS << "block_" << *Instruction.BranchTarget;
    else
      OS << "<invalid-target>";
  };
  OS << Instruction.Info->Mnemonic;
  switch (Instruction.Info->Form) {
  case OperandForm::None:
    break;
  case OperandForm::Dst:
    OS << " r" << unsigned(Instruction.Dst);
    break;
  case OperandForm::DstImm:
    OS << " r" << unsigned(Instruction.Dst) << ", "
       << static_cast<int64_t>(Instruction.RawImmediate);
    break;
  case OperandForm::DstSrc:
    OS << " r" << unsigned(Instruction.Dst) << ", r"
       << unsigned(Instruction.Src);
    break;
  case OperandForm::LDDW:
    OS << " r" << unsigned(Instruction.Dst) << ", 0x"
       << llvm::utohexstr(Instruction.Immediate);
    break;
  case OperandForm::Load:
    OS << " r" << unsigned(Instruction.Dst) << ", [r"
       << unsigned(Instruction.Src) << (Instruction.Offset < 0 ? " - " : " + ")
       << std::abs(static_cast<int>(Instruction.Offset)) << "]";
    break;
  case OperandForm::StoreImm:
    OS << " [r" << unsigned(Instruction.Dst)
       << (Instruction.Offset < 0 ? " - " : " + ")
       << std::abs(static_cast<int>(Instruction.Offset)) << "], "
       << static_cast<int64_t>(Instruction.RawImmediate);
    break;
  case OperandForm::StoreReg:
    OS << " [r" << unsigned(Instruction.Dst)
       << (Instruction.Offset < 0 ? " - " : " + ")
       << std::abs(static_cast<int>(Instruction.Offset)) << "], r"
       << unsigned(Instruction.Src);
    break;
  case OperandForm::Endian:
    OS << " r" << unsigned(Instruction.Dst) << ", " << Instruction.RawImmediate;
    break;
  case OperandForm::Branch:
    OS << " ";
    PrintBranchTarget();
    break;
  case OperandForm::BranchImm:
    OS << " r" << unsigned(Instruction.Dst) << ", "
       << static_cast<int64_t>(Instruction.RawImmediate) << ", ";
    PrintBranchTarget();
    break;
  case OperandForm::BranchReg:
    OS << " r" << unsigned(Instruction.Dst) << ", r"
       << unsigned(Instruction.Src) << ", ";
    PrintBranchTarget();
    break;
  case OperandForm::CallImm:
    OS << " "
       << (Instruction.ResolvedName.empty()
               ? std::to_string(Instruction.RawImmediate)
               : Instruction.ResolvedName);
    break;
  case OperandForm::CallReg:
    OS << " r" << unsigned(Instruction.CallRegister);
    break;
  }
  return Buffer;
}

std::string dumpLowIR(const LowIR &IR) {
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  OS << "; SBF LowIR " << versionDisplayName(IR.TheVersion) << " text=0x"
     << llvm::utohexstr(IR.TextAddress) << " entry=" << IR.EntrySlot << "\n";
  for (const BasicBlock &Block : IR.Blocks) {
    OS << "block_" << Block.ID << ": ; slots [" << Block.StartSlot << ", "
       << Block.EndSlot << ")" << (Block.Reachable ? "" : " unreachable")
       << "\n";
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
      const LowInstruction &Instruction = IR.Instructions[Slot];
      OS << "  " << llvm::format_hex(Instruction.Address, 18) << "  "
         << formatInstruction(Instruction) << "\n";
    }
    OS << "  successors:";
    for (size_t Successor : Block.Successors)
      OS << " block_" << Successor;
    OS << "\n";
  }
  for (const CFGEdge &Edge : IR.Edges) {
    OS << "; edge block_" << Edge.From << " " << edgeKindName(Edge.Kind);
    if (Edge.To)
      OS << " block_" << *Edge.To;
    OS << "\n";
  }
  for (const Diagnostic &Diagnostic : IR.Diagnostics)
    OS << "; "
       << (Diagnostic.Severity == DiagnosticSeverity::Error ? "error"
                                                            : "warning")
       << " slot " << Diagnostic.Slot << ": " << Diagnostic.Message << "\n";
  return Buffer;
}

std::string dumpMedIR(const MedIR &IR) {
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  OS << "; SBF MedIR " << versionDisplayName(IR.TheVersion) << "\n";
  for (const MedBlock &Block : IR.Blocks) {
    OS << "block_" << Block.ID << ":\n";
    for (const MedInstruction &Instruction : IR.Instructions) {
      if (Instruction.Slot < Block.StartSlot ||
          Instruction.Slot >= Block.EndSlot)
        continue;
      OS << "  %pc" << Instruction.Slot << " = "
         << operationName(Instruction.Op) << "." << unsigned(Instruction.Width)
         << " r" << unsigned(Instruction.Dst);
      if (Instruction.Form == OperandForm::DstSrc ||
          Instruction.Form == OperandForm::Load ||
          Instruction.Form == OperandForm::StoreReg ||
          Instruction.Form == OperandForm::BranchReg)
        OS << ", r" << unsigned(Instruction.Src);
      else if (Instruction.Form == OperandForm::DstImm ||
               Instruction.Form == OperandForm::StoreImm ||
               Instruction.Form == OperandForm::BranchImm ||
               Instruction.Form == OperandForm::LDDW)
        OS << ", 0x" << llvm::utohexstr(Instruction.Immediate);
      if (Instruction.Semantics.SwapOperands)
        OS << " [swapped]";
      if (Instruction.BranchTarget)
        OS << " -> slot " << *Instruction.BranchTarget;
      if (Instruction.Syscall)
        OS << " @" << Instruction.Syscall->Name;
      OS << "\n";
    }
  }
  return Buffer;
}

std::string dumpHighIR(const HighIR &IR) {
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  OS << "; SBF HighIR\n";
  for (const Function &Function : IR.Functions) {
    OS << "function " << Function.Name << " @ 0x"
       << llvm::utohexstr(Function.Address) << " {";
    for (size_t Block : IR.ownedBlocks(Function))
      OS << " block_" << Block;
    OS << " }\n";
  }
  for (const Region &Region : IR.Regions) {
    OS << (Region.Kind == RegionKind::Loop ? "loop"
           : Region.Kind == RegionKind::If ? "if"
                                           : "irreducible")
       << " block_" << Region.HeaderBlock;
    if (Region.ExitBlock)
      OS << " -> block_" << *Region.ExitBlock;
    OS << "\n";
  }
  for (const SyscallUse &Use : IR.Syscalls)
    OS << "syscall slot " << Use.Slot << " "
       << (Use.Info ? Use.Info->Name : kUnknownSyscallName) << " (0x"
       << llvm::utohexstr(Use.Hash) << ")\n";
  for (const RecoveredString &String : IR.Strings)
    OS << "string 0x" << llvm::utohexstr(String.Address) << " \""
       << String.Value << "\"\n";
  OS << "; cpi=" << IR.UsesCPI << " account-memory=" << IR.UsesAccounts << "\n";
  OS << dumpSolanaModel(IR.Solana);
  return Buffer;
}

} // namespace neverd::sbf
