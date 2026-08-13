//===- EVMHighAnalysisDispatch.cpp - EVM selector dispatcher reading ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMHighAnalysisDetail.h"

#include <vector>

namespace neverd::evm::detail {
namespace {

bool endsInRevert(const EVMLowIR &Low, uint64_t BlockPC) {
  const LowBlock *Block = Low.findBlock(BlockPC);
  if (!Block || Block->InstructionCount == 0)
    return false;
  return Low.Instructions[Block->FirstInstruction + Block->InstructionCount - 1]
      .is(Opcode::REVERT);
}

/// True when \p Operation does something rejecting an unrecognized call does
/// not need to do.
///
/// A dispatcher that recognizes nothing reads the context to find the selector,
/// branches on it, and reverts. Reaching anything else means some code accepted
/// the call.
bool exceedsRejection(const MedOperation &Operation) {
  // INVALID and STOP halt alike but do not mean alike: one is how the machine
  // refuses a byte it cannot run, the other is how a contract accepts a call
  // and returns nothing.
  if (Operation.Op == Opcode::INVALID)
    return false;
  switch (Operation.Effect) {
  case EffectKind::StorageRead:
  case EffectKind::StorageWrite:
  case EffectKind::TransientRead:
  case EffectKind::TransientWrite:
  case EffectKind::ExternalCall:
  case EffectKind::Create:
  case EffectKind::Log:
  case EffectKind::SelfDestruct:
  case EffectKind::Return:
  case EffectKind::Halt:
    return true;
  case EffectKind::None:
  case EffectKind::ContextRead:
  case EffectKind::Control:
  case EffectKind::Revert:
  // Nothing was established, so nothing is proven.
  case EffectKind::Unknown:
    return false;
  }
  return false;
}

} // namespace

bool reachesFallback(const EVMLowIR &Low, const ProducerIndex &Index,
                     SemanticClassifier &Classifier) {
  std::set<uint64_t> Seen{kEntryPC};
  std::vector<uint64_t> Worklist{kEntryPC};
  while (!Worklist.empty()) {
    const uint64_t BlockPC = Worklist.back();
    Worklist.pop_back();
    const LowBlock *LowBlock = Low.findBlock(BlockPC);
    const MedBlock *Block = Index.block(BlockPC);
    if (!LowBlock || !Block)
      continue;

    bool Dispatches = false;
    for (const MedOperation &Operation : Block->Operations) {
      if (exceedsRejection(Operation))
        return true;
      if (Operation.Op != Opcode::JUMPI || Operation.Inputs.size() != 2)
        continue;
      const SemanticKind Condition =
          Classifier.classify(Operation.Inputs[1]).Kind;
      Dispatches |= Condition == SemanticKind::SelectorEquality ||
                    Condition == SemanticKind::IsZeroCalldataSize;
    }

    for (const LowEdge &Edge : LowBlock->Successors) {
      if (!Edge.Target)
        continue;
      if (Dispatches && Edge.Kind == EdgeKind::ConditionalTrue)
        continue;
      if (Seen.insert(*Edge.Target).second)
        Worklist.push_back(*Edge.Target);
    }
  }
  return false;
}

std::set<uint64_t> nonPayableGuardReads(const EVMLowIR &Low,
                                        const ProducerIndex &Index,
                                        SemanticClassifier &Classifier,
                                        const std::set<uint64_t> &Blocks) {
  std::set<uint64_t> Reads;
  for (uint64_t BlockPC : Blocks) {
    const MedBlock *Block = Index.block(BlockPC);
    const LowBlock *LowBlock = Low.findBlock(BlockPC);
    if (!Block || !LowBlock)
      continue;
    for (const MedOperation &Operation : Block->Operations) {
      if (Operation.Op != Opcode::JUMPI || Operation.Inputs.size() != 2)
        continue;
      const SemanticValue Condition = Classifier.classify(Operation.Inputs[1]);
      if (Condition.Kind != SemanticKind::IsZeroCallValue)
        continue;
      for (const LowEdge &Edge : LowBlock->Successors)
        if (Edge.Kind == EdgeKind::ConditionalFalse && Edge.Target &&
            endsInRevert(Low, *Edge.Target))
          Reads.insert(Condition.OriginPCs.begin(), Condition.OriginPCs.end());
    }
  }
  return Reads;
}

} // namespace neverd::evm::detail
