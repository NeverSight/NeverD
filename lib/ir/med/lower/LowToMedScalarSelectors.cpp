//===- LowToMedScalarSelectors.cpp - Original-address selectors -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve original numeric coordinates for certified finite dispatches.

#include "neverd/Limits.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/ir/med/LowToMed.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {

void LowToMedConverter::projectOriginalAddressSelectors(MedFunc &Func) {
  llvm::DenseSet<va_t> Dispatches;
  for (const auto &Table : Func.JumpTables)
    if (Table.OriginalAddressSelector)
      Dispatches.insert(Table.InsnAddr);
  if (Dispatches.empty() || Func.SwitchSelectorPlans.empty())
    return;
  if (Func.SkippedSSA) {
    for (auto It = Func.SwitchSelectorPlans.begin();
         It != Func.SwitchSelectorPlans.end();)
      if (Dispatches.contains(It->first.first))
        It = Func.SwitchSelectorPlans.erase(It);
      else
        ++It;
    return;
  }

  using Key = std::pair<int, int>;
  struct Definition {
    size_t Block;
    size_t Index;
    bool IsPhi;
  };
  size_t Count = 0;
  for (const auto &Block : Func.Blocks)
    Count += Block.Phis.size() + Block.Ops.size();
  llvm::DenseMap<Key, Definition> Definitions;
  Definitions.reserve(Count);
  llvm::DenseMap<int, size_t> Blocks;
  Blocks.reserve(Func.Blocks.size());
  const auto KeyOf = [](const MedVar &Value) {
    return Key{Value.Id, Value.SSAVer};
  };
  for (size_t B = 0; B != Func.Blocks.size(); ++B) {
    const auto &Block = Func.Blocks[B];
    Blocks.try_emplace(Block.Id, B);
    for (size_t I = 0; I != Block.Phis.size(); ++I)
      Definitions.try_emplace(KeyOf(Block.Phis[I].Output),
                              Definition{B, I, true});
    for (size_t I = 0; I != Block.Ops.size(); ++I) {
      const auto &Op = Block.Ops[I];
      if (Op.Output.Id >= 0 && Op.Output.Size &&
          !(Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Op.Output == Op.Inputs[0]))
        Definitions.try_emplace(KeyOf(Op.Output), Definition{B, I, false});
    }
  }

  struct PendingBlock {
    std::vector<PhiNode> Phis;
    std::vector<std::pair<size_t, MedOp>> Ops;
  };
  std::vector<PendingBlock> Pending(Func.Blocks.size());
  llvm::DenseMap<Key, std::optional<MedVar>> Projected;
  Projected.reserve(Definitions.size());
  size_t Remaining = limits::kMaxScalarBranchEvidenceWork;
  const auto Fresh = [&](const MedVar &Value) {
    MedVar Result;
    Result.Kind = MedVar::Temp;
    Result.Id = allocVarId();
    Result.SSAVer = 1;
    Result.Size = Value.Size;
    Result.TheArch = TargetArch;
    return Result;
  };
  const auto Project = [&](auto &&Self, const MedVar &Value,
                           unsigned Depth) -> std::optional<MedVar> {
    if (!Remaining || Depth >= limits::kMaxScalarBranchFiniteDepth)
      return {};
    --Remaining;
    if (Value.isConst())
      return MedVar::makeConst(Value.ConstVal, Value.Size,
                               ConstantAddressProvenance::Scalar);
    const Key K = KeyOf(Value);
    if (auto It = Projected.find(K); It != Projected.end())
      return It->second;
    const auto Found = Definitions.find(K);
    if (Found == Definitions.end())
      return Value;
    // A cycle or an exhausted traversal supplies no selector certificate.
    Projected.try_emplace(K, std::nullopt);
    const Definition Def = Found->second;
    const auto &Block = Func.Blocks[Def.Block];
    if (Def.IsPhi) {
      PhiNode Copy = Block.Phis[Def.Index];
      if (Copy.ExceptionalEntry)
        return {};
      bool Changed = false;
      for (auto &[Pred, Arg] : Copy.Args) {
        auto Numeric = Self(Self, Arg, Depth + 1);
        if (!Numeric)
          return {};
        Changed |= *Numeric != Arg;
        Arg = *Numeric;
      }
      if (!Changed)
        return Projected[K] = Value;
      Copy.Output = Fresh(Value);
      const MedVar Result = Copy.Output;
      Pending[Def.Block].Phis.push_back(std::move(Copy));
      return Projected[K] = Result;
    }
    const MedOp &Op = Block.Ops[Def.Index];
    // Duplicate only total scalar operations, using the same shape authority
    // as LowIR recovery. Loads, calls and other observations keep their one
    // original evaluation; this pass never replays effects or dereferences.
    LowOp Shape;
    Shape.Opcode = Op.Opcode;
    Shape.Output = NdVar::tmp(0, Op.Output.Size);
    if (Op.NumInputs > std::size(Shape.Inputs) ||
        Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Op.RegistrationRoot != MedOp::RegistrationRootKind::None)
      return Projected[K] = Value;
    Shape.NumInputs = Op.NumInputs;
    for (unsigned I = 0; I != Op.NumInputs; ++I)
      Shape.Inputs[I] =
          Op.Inputs[I].isConst()
              ? NdVar::cst(Op.Inputs[I].ConstVal, Op.Inputs[I].Size)
              : NdVar::tmp(I, Op.Inputs[I].Size);
    if (!lowScalarConstantResult(Shape, [](const NdVar &Input) {
          return std::optional<uint64_t>(Input.isConst() ? Input.Offset : 0);
        }))
      return Projected[K] = Value;

    MedOp Copy = Op;
    bool Changed = false;
    for (unsigned I = 0; I != Copy.NumInputs; ++I) {
      auto Numeric = Self(Self, Copy.Inputs[I], Depth + 1);
      if (!Numeric)
        return {};
      Changed |= *Numeric != Copy.Inputs[I];
      Copy.Inputs[I] = *Numeric;
    }
    if (!Changed)
      return Projected[K] = Value;
    Copy.Output = Fresh(Value);
    // A synthetic numeric view is not a second authenticated machine op.
    Copy.OriginSeq = -1;
    const MedVar Result = Copy.Output;
    Pending[Def.Block].Ops.emplace_back(Def.Index, std::move(Copy));
    return Projected[K] = Result;
  };

  for (auto It = Func.SwitchSelectorPlans.begin();
       It != Func.SwitchSelectorPlans.end();) {
    const auto [Address, BlockId] = It->first;
    if (!Dispatches.contains(Address)) {
      ++It;
      continue;
    }
    auto &Plan = It->second;
    const auto Block = Blocks.find(BlockId);
    auto Numeric = Plan.PlanKind == MedSwitchSelectorPlan::Kind::Direct
                       ? Project(Project, Plan.Selector, 0)
                       : std::nullopt;
    if (!Numeric || Block == Blocks.end()) {
      It = Func.SwitchSelectorPlans.erase(It);
      continue;
    }
    for (auto &Op : Func.Blocks[Block->second].Ops)
      if (Op.Opcode == NdOp::INDIR_BR && Op.Addr == Address &&
          Op.NumInputs == 1 && Op.Inputs[0] == Plan.Selector)
        Op.Inputs[0] = *Numeric;
    Plan.Selector = *Numeric;
    ++It;
  }

  // Commit only after tracing, keeping definition pointers/indices stable.
  // Existing address uses retain their definitions. DCE removes unused views,
  // including speculative clones from a selector whose proof was incomplete.
  for (size_t B = 0; B != Func.Blocks.size(); ++B) {
    auto &Block = Func.Blocks[B];
    auto &Edits = Pending[B];
    for (auto &Phi : Edits.Phis)
      Block.Phis.push_back(std::move(Phi));
    std::sort(Edits.Ops.begin(), Edits.Ops.end(),
              [](const auto &A, const auto &B) { return A.first > B.first; });
    for (auto &[Index, Op] : Edits.Ops)
      Block.Ops.insert(Block.Ops.begin() + Index + 1, std::move(Op));
  }
  runDce(Func);
  debugVerifyMedFunc(Func, "projectOriginalAddressSelectors");
}

} // namespace neverd
