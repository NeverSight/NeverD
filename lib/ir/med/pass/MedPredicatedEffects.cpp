//===- MedPredicatedEffects.cpp - Materialize ARM predicates -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Splits a flattened ARM instruction-local predicate guard into ordinary
/// guard and effect blocks before SSA construction.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/LowToMed.h"

#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {
namespace {

bool isObservableEffect(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::LOAD:
  case NdOp::STORE:
  case NdOp::ATOMIC_XCHG:
  case NdOp::ATOMIC_ADD:
  case NdOp::ATOMIC_CMPXCHG:
  case NdOp::INTRINSIC:
  case NdOp::BRANCH:
  case NdOp::INDIR_BR:
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
  case NdOp::RETURN:
    return true;
  default:
    return false;
  }
}

bool isTerminalEffect(NdOp Opcode) {
  return Opcode == NdOp::BRANCH || Opcode == NdOp::INDIR_BR ||
         Opcode == NdOp::RETURN;
}

bool isNoReturnCall(const MedOp &Op) {
  return Op.DoesNotReturn &&
         (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL);
}

va_t blockAddress(const MedBlock &Block) {
  if (Block.StartAddr != 0 || Block.Ops.empty())
    return Block.StartAddr;
  return Block.Ops.front().Addr;
}

std::optional<int> blockAtAddress(const MedFunc &Func, va_t Address) {
  for (const MedBlock &Block : Func.Blocks)
    if (blockAddress(Block) == Address)
      return Block.Id;
  return std::nullopt;
}

std::optional<int> successorAtAddress(const MedFunc &Func,
                                      const MedBlock &Block, va_t Address) {
  for (int Successor : Block.Succs)
    if (Successor >= 0 && static_cast<size_t>(Successor) < Func.Blocks.size() &&
        blockAddress(Func.Blocks[Successor]) == Address)
      return Successor;
  return std::nullopt;
}

} // namespace

void LowToMedConverter::materializePredicatedEffects(MedFunc &Func) {
  if (TargetArch != Arch::ARM || Func.Blocks.empty())
    return;

  const size_t OriginalBlockCount = Func.Blocks.size();
  std::vector<MedBlock> AddedBlocks;
  std::map<va_t, int> SyntheticExits;

  auto nextBlockId = [&]() {
    return static_cast<int>(OriginalBlockCount + AddedBlocks.size());
  };

  auto getOrCreateExit = [&](va_t Address) -> int {
    if (std::optional<int> Existing = blockAtAddress(Func, Address))
      return *Existing;
    auto It = SyntheticExits.find(Address);
    if (It != SyntheticExits.end())
      return It->second;

    MedBlock Exit;
    Exit.Id = nextBlockId();
    Exit.StartAddr = Address;
    Exit.EndAddr = Address;
    const int Id = Exit.Id;
    AddedBlocks.push_back(std::move(Exit));
    SyntheticExits.emplace(Address, Id);
    return Id;
  };

  for (size_t BlockIndex = 0; BlockIndex < OriginalBlockCount; ++BlockIndex) {
    MedBlock &GuardBlock = Func.Blocks[BlockIndex];
    for (size_t GuardIndex = 0; GuardIndex < GuardBlock.Ops.size();
         ++GuardIndex) {
      const MedOp &Guard = GuardBlock.Ops[GuardIndex];
      if (Guard.Opcode != NdOp::COND_BR || Guard.NumInputs < 2 ||
          !Guard.Inputs[0].isConst())
        continue;

      size_t EffectEnd = GuardIndex + 1;
      while (EffectEnd < GuardBlock.Ops.size() &&
             GuardBlock.Ops[EffectEnd].Addr == Guard.Addr)
        ++EffectEnd;
      if (EffectEnd == GuardIndex + 1)
        continue;

      bool HasEffect = false;
      for (size_t I = GuardIndex + 1; I < EffectEnd; ++I)
        HasEffect |= isObservableEffect(GuardBlock.Ops[I].Opcode);
      if (!HasEffect)
        continue;

      // Recursive CFG construction ends a block at this synthetic COND_BR, so
      // a different-address tail would indicate malformed or legacy MedIR.
      // Refuse to guess where such operations belong.
      if (EffectEnd != GuardBlock.Ops.size())
        llvm::report_fatal_error(
            "predicated ARM effect is not the final guest instruction");

      const va_t SkipAddress = Guard.Inputs[0].ConstVal;
      const std::optional<int> ExistingSkip =
          successorAtAddress(Func, GuardBlock, SkipAddress);
      const int SkipId =
          ExistingSkip ? *ExistingSkip : getOrCreateExit(SkipAddress);

      MedBlock EffectBlock;
      EffectBlock.StartAddr = Guard.Addr;
      EffectBlock.EndAddr = GuardBlock.EndAddr;
      EffectBlock.ExceptionalSuccs = std::move(GuardBlock.ExceptionalSuccs);
      GuardBlock.ExceptionalSuccs.clear();
      EffectBlock.Ops.insert(
          EffectBlock.Ops.end(),
          std::make_move_iterator(GuardBlock.Ops.begin() + GuardIndex + 1),
          std::make_move_iterator(GuardBlock.Ops.begin() + EffectEnd));

      const MedOp *Terminal = nullptr;
      for (const MedOp &Op : EffectBlock.Ops)
        if (isTerminalEffect(Op.Opcode) || isNoReturnCall(Op))
          Terminal = &Op;

      if (!Terminal) {
        EffectBlock.Succs = {SkipId};
      } else if (Terminal->Opcode == NdOp::BRANCH) {
        if (Terminal->NumInputs < 1 || !Terminal->Inputs[0].isConst())
          llvm::report_fatal_error(
              "predicated direct branch has no constant destination");
        const va_t TargetAddress = Terminal->Inputs[0].ConstVal;
        std::optional<int> Target =
            successorAtAddress(Func, GuardBlock, TargetAddress);
        EffectBlock.Succs = {Target ? *Target : getOrCreateExit(TargetAddress)};
      } else if (Terminal->Opcode == NdOp::INDIR_BR) {
        for (int Successor : GuardBlock.Succs)
          if (Successor != SkipId &&
              std::find(EffectBlock.Succs.begin(), EffectBlock.Succs.end(),
                        Successor) == EffectBlock.Succs.end())
            EffectBlock.Succs.push_back(Successor);
      }

      EffectBlock.Id = nextBlockId();
      EffectBlock.Preds = {GuardBlock.Id};
      GuardBlock.Ops.erase(GuardBlock.Ops.begin() + GuardIndex + 1,
                           GuardBlock.Ops.begin() + EffectEnd);
      GuardBlock.Succs = {SkipId, EffectBlock.Id};
      AddedBlocks.push_back(std::move(EffectBlock));
      break;
    }
  }

  Func.Blocks.insert(Func.Blocks.end(),
                     std::make_move_iterator(AddedBlocks.begin()),
                     std::make_move_iterator(AddedBlocks.end()));

  // Predecessors are derived state.  Rebuild both graphs after adding the
  // synthetic blocks instead of trying to patch every prior list in place.
  for (MedBlock &Block : Func.Blocks) {
    Block.Preds.clear();
    Block.ExceptionalPreds.clear();
  }
  for (const MedBlock &Block : Func.Blocks) {
    for (int Successor : Block.Succs) {
      if (Successor < 0 || static_cast<size_t>(Successor) >= Func.Blocks.size())
        llvm::report_fatal_error(
            "predicated ARM effect has an invalid successor");
      Func.Blocks[Successor].Preds.push_back(Block.Id);
    }
    for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs) {
      if (Edge.BlockId < 0 ||
          static_cast<size_t>(Edge.BlockId) >= Func.Blocks.size())
        continue;
      ExceptionalEdge Predecessor = Edge;
      Predecessor.BlockId = Block.Id;
      auto &Preds = Func.Blocks[Edge.BlockId].ExceptionalPreds;
      if (std::find(Preds.begin(), Preds.end(), Predecessor) == Preds.end())
        Preds.push_back(Predecessor);
    }
  }
}

} // namespace neverd
