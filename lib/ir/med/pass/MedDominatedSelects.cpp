//===- MedDominatedSelects.cpp - Selections a branch decides --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fold a selection whose condition a dominating conditional branch decides.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedDominatedSelects.h"

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {
namespace {

using ValueKey = std::pair<int, int>;

/// An operand of a condition: a constant, or an SSA value.
struct Operand {
  bool Constant = false;
  uint64_t Value = 0;
  uint16_t Size = 0;
  ValueKey Key{0, 0};

  bool operator==(const Operand &Other) const {
    return Constant == Other.Constant && Size == Other.Size &&
           (Constant ? Value == Other.Value : Key == Other.Key);
  }
  bool operator<(const Operand &Other) const {
    return std::tie(Constant, Size, Value, Key) <
           std::tie(Other.Constant, Other.Size, Other.Value, Other.Key);
  }
};

/// A condition in a form two equal conditions share: a comparison of two
/// operands, or a value that is none, and whether it is negated.
struct Condition {
  NdOp Opcode = NdOp::NOP;
  Operand Left, Right;
  bool Negated = false;

  bool sameTest(const Condition &Other) const {
    return Opcode == Other.Opcode && Left == Other.Left && Right == Other.Right;
  }
};

/// A branch's value on entry to a block: \p Cond holds \p Value there.
struct Fact {
  Condition Cond;
  bool Value = false;
};

class Folder {
public:
  explicit Folder(MedFunc &Func) : Func(Func) {
    for (const MedBlock &Block : Func.Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Output.Size && Op.Output.Kind != MedVar::Const)
          Defs.try_emplace({Op.Output.Id, Op.Output.SSAVer}, &Op);
  }

  bool run() {
    if (!buildDominators())
      return false;
    collectFacts();
    if (Facts.empty())
      return false;
    bool Changed = false;
    for (size_t Index = 0; Index < Func.Blocks.size(); ++Index) {
      if (Order[Index] < 0)
        continue;
      MedBlock &Block = Func.Blocks[Index];
      llvm::SmallVector<const Fact *, 8> Known;
      for (int Up = static_cast<int>(Index);;) {
        if (const auto Found = Facts.find(Up); Found != Facts.end())
          for (const Fact &F : Found->second)
            Known.push_back(&F);
        if (Idom[Up] == Up)
          break;
        Up = Idom[Up];
      }
      if (Known.empty())
        continue;
      for (MedOp &Op : Block.Ops) {
        if (Op.Opcode != NdOp::SELECT || Op.NumInputs != 3 ||
            Op.Inputs[0].isConst())
          continue;
        const Condition Selects = canonical(Op.Inputs[0]);
        for (const Fact *F : Known) {
          if (!Selects.sameTest(F->Cond))
            continue;
          const bool Holds = F->Value != (Selects.Negated != F->Cond.Negated);
          const MedVar Taken = Op.Inputs[Holds ? 1 : 2];
          Op.Opcode = NdOp::COPY;
          Op.Inputs[0] = Taken;
          Op.NumInputs = 1;
          Changed = true;
          break;
        }
      }
    }
    return Changed;
  }

private:
  MedFunc &Func;
  llvm::DenseMap<ValueKey, const MedOp *> Defs;
  llvm::DenseMap<int, size_t> IndexOfId;
  /// Each block's reverse post-order number from the entry, -1 where no
  /// path from the entry reaches it.
  std::vector<int> Order;
  /// Each reached block's immediate dominator, by index.
  std::vector<int> Idom;
  llvm::DenseMap<int, llvm::SmallVector<Fact, 2>> Facts;

  const MedOp *definition(const MedVar &V) const {
    if (V.isConst())
      return nullptr;
    const auto Found = Defs.find({V.Id, V.SSAVer});
    return Found == Defs.end() ? nullptr : Found->second;
  }

  /// \p V past the copies and the idempotent operations that only pass it
  /// on (`test rdx, rdx` compares `rdx & rdx`).
  Operand operand(MedVar V) const {
    for (unsigned Depth = 0; Depth < 8 && !V.isConst(); ++Depth) {
      const MedOp *Def = definition(V);
      if (!Def)
        break;
      if (Def->Opcode == NdOp::COPY && Def->NumInputs == 1 &&
          Def->Inputs[0].Size == V.Size) {
        V = Def->Inputs[0];
        continue;
      }
      if ((Def->Opcode == NdOp::INT_AND || Def->Opcode == NdOp::INT_OR) &&
          Def->NumInputs == 2 && !Def->Inputs[0].isConst() &&
          Def->Inputs[0] == Def->Inputs[1] && Def->Inputs[0].Size == V.Size) {
        V = Def->Inputs[0];
        continue;
      }
      break;
    }
    Operand Result;
    Result.Size = V.Size;
    if (V.isConst()) {
      Result.Constant = true;
      Result.Value = V.ConstVal;
    } else {
      Result.Key = {V.Id, V.SSAVer};
    }
    return Result;
  }

  Condition canonical(MedVar V) const {
    Condition Result;
    for (unsigned Depth = 0; Depth < 8; ++Depth) {
      const MedOp *Def = definition(V);
      if (!Def || Def->NumInputs < 1)
        break;
      if (Def->Opcode == NdOp::BOOL_NOT && Def->NumInputs == 1) {
        Result.Negated = !Result.Negated;
        V = Def->Inputs[0];
        continue;
      }
      if (Def->Opcode == NdOp::COPY && Def->NumInputs == 1 &&
          Def->Inputs[0].Size == V.Size && !Def->Inputs[0].isConst()) {
        V = Def->Inputs[0];
        continue;
      }
      break;
    }
    const MedOp *Def = definition(V);
    switch (Def ? Def->Opcode : NdOp::NOP) {
    case NdOp::INT_EQUAL:
    case NdOp::INT_NOTEQUAL:
    case NdOp::INT_LESS:
    case NdOp::INT_SLESS:
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESSEQUAL:
      if (Def->NumInputs == 2) {
        Result.Opcode = Def->Opcode;
        Result.Left = operand(Def->Inputs[0]);
        Result.Right = operand(Def->Inputs[1]);
        // Equality is one test whichever way round, and inequality its
        // negation.
        if (Result.Opcode == NdOp::INT_NOTEQUAL) {
          Result.Opcode = NdOp::INT_EQUAL;
          Result.Negated = !Result.Negated;
        }
        if (Result.Opcode == NdOp::INT_EQUAL && Result.Right < Result.Left)
          std::swap(Result.Left, Result.Right);
        return Result;
      }
      break;
    default:
      break;
    }
    Result.Left = operand(V);
    return Result;
  }

  /// Reverse post-order from the first block, and the immediate dominators
  /// of the blocks it reaches (Cooper, Harvey and Kennedy).  An exceptional
  /// edge enters its handler as a normal one does.
  bool buildDominators() {
    const size_t N = Func.Blocks.size();
    if (N < 2)
      return false;
    for (size_t I = 0; I < N; ++I)
      if (!IndexOfId.try_emplace(Func.Blocks[I].Id, I).second)
        return false;
    const auto Successors = [&](size_t I) {
      llvm::SmallVector<size_t, 4> Result;
      const MedBlock &Block = Func.Blocks[I];
      for (int Id : Block.Succs)
        if (const auto Found = IndexOfId.find(Id); Found != IndexOfId.end())
          Result.push_back(Found->second);
      for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs)
        if (const auto Found = IndexOfId.find(Edge.BlockId);
            Found != IndexOfId.end())
          Result.push_back(Found->second);
      return Result;
    };
    Order.assign(N, -1);
    std::vector<size_t> PostOrder;
    std::vector<char> Seen(N, 0);
    std::vector<std::pair<size_t, llvm::SmallVector<size_t, 4>>> Stack;
    Seen[0] = 1;
    Stack.emplace_back(0, Successors(0));
    while (!Stack.empty()) {
      auto &[Block, Next] = Stack.back();
      if (Next.empty()) {
        PostOrder.push_back(Block);
        Stack.pop_back();
        continue;
      }
      const size_t Succ = Next.back();
      Next.pop_back();
      if (!Seen[Succ]) {
        Seen[Succ] = 1;
        Stack.emplace_back(Succ, Successors(Succ));
      }
    }
    std::vector<size_t> Rpo(PostOrder.rbegin(), PostOrder.rend());
    for (size_t I = 0; I < Rpo.size(); ++I)
      Order[Rpo[I]] = static_cast<int>(I);
    std::vector<llvm::SmallVector<size_t, 4>> Predecessors(N);
    for (size_t I : Rpo)
      for (size_t Succ : Successors(I))
        Predecessors[Succ].push_back(I);
    Idom.assign(N, -1);
    Idom[0] = 0;
    const auto Intersect = [&](int A, int B) {
      while (A != B) {
        while (Order[A] > Order[B])
          A = Idom[A];
        while (Order[B] > Order[A])
          B = Idom[B];
      }
      return A;
    };
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (size_t I : llvm::drop_begin(Rpo)) {
        int New = -1;
        for (size_t Pred : Predecessors[I]) {
          if (Idom[Pred] < 0)
            continue;
          New = New < 0 ? static_cast<int>(Pred)
                        : Intersect(static_cast<int>(Pred), New);
        }
        if (New >= 0 && Idom[I] != New) {
          Idom[I] = New;
          Changed = true;
        }
      }
    }
    return true;
  }

  /// The value each conditional branch gives its condition in a successor
  /// that only its edge enters.
  void collectFacts() {
    for (size_t I = 0; I < Func.Blocks.size(); ++I) {
      const MedBlock &Block = Func.Blocks[I];
      if (Order[I] < 0 || Block.Ops.empty())
        continue;
      const MedOp &Branch = Block.Ops.back();
      if (Branch.Opcode != NdOp::COND_BR || Branch.NumInputs < 2 ||
          !Branch.Inputs[0].isConst() || Branch.Inputs[1].isConst())
        continue;
      llvm::SmallVector<size_t, 2> Succs;
      for (int Id : Block.Succs)
        if (const auto Found = IndexOfId.find(Id);
            Found != IndexOfId.end() &&
            !llvm::is_contained(Succs, Found->second))
          Succs.push_back(Found->second);
      if (Succs.size() != 2)
        continue;
      const va_t Target = Branch.Inputs[0].ConstVal;
      const bool FirstTaken = Func.Blocks[Succs[0]].StartAddr == Target;
      const bool SecondTaken = Func.Blocks[Succs[1]].StartAddr == Target;
      if (FirstTaken == SecondTaken)
        continue;
      const Condition Cond = canonical(Branch.Inputs[1]);
      for (size_t K = 0; K < 2; ++K) {
        const MedBlock &Succ = Func.Blocks[Succs[K]];
        if (Succ.Preds.size() != 1 || Succ.Preds.front() != Block.Id ||
            !Succ.ExceptionalPreds.empty())
          continue;
        Facts[static_cast<int>(Succs[K])].push_back(
            {Cond, K == 0 ? FirstTaken : SecondTaken});
      }
    }
  }
};

} // namespace

bool foldDominatedSelects(MedFunc &Func) {
  if (Func.SkippedSSA)
    return false;
  bool HasSelect = false, HasBranch = false;
  for (const MedBlock &Block : Func.Blocks) {
    for (const MedOp &Op : Block.Ops)
      HasSelect |= Op.Opcode == NdOp::SELECT;
    HasBranch |= !Block.Ops.empty() && Block.Ops.back().Opcode == NdOp::COND_BR;
  }
  if (!HasSelect || !HasBranch)
    return false;
  return Folder(Func).run();
}

} // namespace neverd
