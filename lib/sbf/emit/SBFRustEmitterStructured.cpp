//===- SBFRustEmitterStructured.cpp - SBF structured control flow ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Renders a validated structured control-flow plan as Rust `if` and `loop`
/// expressions, falling back to the dispatch match when the plan contains a
/// node this backend cannot render.
///
//===----------------------------------------------------------------------===//

#include "SBFRustEmitterDetail.h"

#include <string>

namespace neverd::sbf {

using namespace rust_emitter_detail;

namespace {

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

} // namespace

namespace rust_emitter_detail {

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

} // namespace rust_emitter_detail
} // namespace neverd::sbf
