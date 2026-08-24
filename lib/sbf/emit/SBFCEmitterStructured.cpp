//===- SBFCEmitterStructured.cpp - SBF structured control flow in C -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Renders a validated structured control-flow plan as C `if` and `while`
/// statements, falling back to the dispatch switch when the plan contains a
/// node this backend cannot render.
///
//===----------------------------------------------------------------------===//

#include "SBFCEmitterDetail.h"

#include "neverd/sbf/emit/SBFSourceStatus.h"

#include "llvm/ADT/BitVector.h"

#include <algorithm>
#include <string>
#include <vector>

namespace neverd::sbf {

using namespace c_emitter_detail;

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
      OS << Indent << "if (result) *result = r[NEVERD_SBF_RETURN_REGISTER];\n"
         << Indent << "return " << cSourceStatusName(SourceStatus::Ok) << ";\n";
      continue;
    }
    if (!emitLinearInstruction(OS, Instruction, Indent))
      return false;
  }
  return true;
}

size_t maximumDisplayedDepth(size_t NodeCount) {
  size_t Depth = 0;
  while (NodeCount > 1) {
    NodeCount = NodeCount / 2 + NodeCount % 2;
    ++Depth;
  }
  return Depth;
}

std::string displayedIndent(llvm::StringRef Base, size_t Depth,
                            size_t MaximumDepth) {
  constexpr llvm::StringLiteral kIndentUnit = "  ";
  std::string Result = Base.str();
  for (size_t Level = 0; Level < std::min(Depth, MaximumDepth); ++Level)
    Result += kIndentUnit;
  return Result;
}

enum class EmitActionKind : uint8_t { Sequence, IfElse, CloseScope };

struct EmitAction {
  EmitActionKind Kind = EmitActionKind::Sequence;
  size_t Node = StructuredNode::NoNode;
  size_t Depth = 0;
};

} // namespace

namespace c_emitter_detail {

bool emitStructuredNodes(llvm::raw_ostream &OS, const SBFProgram &Program,
                         const std::map<size_t, const MedInstruction *> &BySlot,
                         const StructuredControlFlow &Plan,
                         llvm::StringRef Indent) {
  if (Plan.Entry == StructuredNode::NoNode)
    return Plan.Nodes.empty();
  const size_t MaximumDepth = maximumDisplayedDepth(Plan.Nodes.size());
  llvm::BitVector Emitted(Plan.Nodes.size());
  std::vector<EmitAction> Work{{EmitActionKind::Sequence, Plan.Entry, 0}};
  while (!Work.empty()) {
    const EmitAction Action = Work.back();
    Work.pop_back();
    const std::string CurrentIndent =
        displayedIndent(Indent, Action.Depth, MaximumDepth);
    if (Action.Kind == EmitActionKind::IfElse) {
      OS << CurrentIndent << "} else {\n";
      continue;
    }
    if (Action.Kind == EmitActionKind::CloseScope) {
      OS << CurrentIndent << "}\n";
      continue;
    }

    size_t NodeIndex = Action.Node;
    while (NodeIndex != StructuredNode::NoNode) {
      if (NodeIndex >= Plan.Nodes.size() || Emitted.test(NodeIndex))
        return false;
      Emitted.set(NodeIndex);
      const StructuredNode &Node = Plan.Nodes[NodeIndex];
      if (Node.Block >= Program.Low.Blocks.size())
        return false;
      const BasicBlock &Block = Program.Low.Blocks[Node.Block];
      const MedInstruction *Terminator = blockTerminator(Block, BySlot);
      if (!Terminator)
        return false;
      if (Node.Kind == StructuredNodeKind::Block) {
        if (!emitStructuredBlock(OS, Program, BySlot, Node.Block,
                                 CurrentIndent))
          return false;
        NodeIndex = Node.Next;
        continue;
      }

      if (Node.Kind == StructuredNodeKind::If) {
        if (!emitStructuredBlock(OS, Program, BySlot, Node.Block,
                                 CurrentIndent))
          return false;
        OS << CurrentIndent << "if (" << comparison(*Terminator) << ") {\n";
        Work.push_back({EmitActionKind::Sequence, Node.Next, Action.Depth});
        Work.push_back(
            {EmitActionKind::CloseScope, StructuredNode::NoNode, Action.Depth});
        Work.push_back(
            {EmitActionKind::Sequence, Node.Alternative, Action.Depth + 1});
        Work.push_back(
            {EmitActionKind::IfElse, StructuredNode::NoNode, Action.Depth});
        Work.push_back({EmitActionKind::Sequence, Node.Body, Action.Depth + 1});
        break;
      }

      OS << CurrentIndent << "while (1) {\n";
      const std::string Inner =
          displayedIndent(Indent, Action.Depth + 1, MaximumDepth);
      if (!emitStructuredBlock(OS, Program, BySlot, Node.Block, Inner))
        return false;
      if (Node.ConditionTrueEntersBody)
        OS << Inner << "if (!(" << comparison(*Terminator) << ")) break;\n";
      else
        OS << Inner << "if (" << comparison(*Terminator) << ") break;\n";
      Work.push_back({EmitActionKind::Sequence, Node.Next, Action.Depth});
      Work.push_back(
          {EmitActionKind::CloseScope, StructuredNode::NoNode, Action.Depth});
      Work.push_back({EmitActionKind::Sequence, Node.Body, Action.Depth + 1});
      break;
    }
  }
  return Emitted.count() == Plan.Nodes.size();
}

} // namespace c_emitter_detail
} // namespace neverd::sbf
