//===- LowScalarFiniteExpression.cpp - Finite scalar covers ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bound arithmetic branch values without assuming correlations or paths.

#include "LowScalarExpression.h"

#include "neverd/Limits.h"
#include "neverd/symbolic/SymKnownBits.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

std::optional<std::vector<uint64_t>> finiteScalarExpression(
    llvm::ArrayRef<LowOp> Operations, const NdVar &Value, va_t QueryInstruction,
    size_t &RemainingWork,
    llvm::function_ref<std::optional<uint64_t>(const LowOp &, uint64_t)>
        ReadImage) {
  symbolic::SymContext Ctx;
  const auto Expression = symbolicScalarExpression(
      Ctx, Operations, Value, QueryInstruction, RemainingWork, ReadImage);
  if (!Expression)
    return {};
  symbolic::SymKnownBits Known(Ctx);
  using Values = std::vector<uint64_t>;
  llvm::DenseMap<uint32_t, std::optional<Values>> Cache;
  // The expression context is immutable during the walk. Reserving every
  // possible node keeps cached child domains stable across recursive inserts.
  Cache.reserve(Ctx.numNodes());
  const auto Spend = [&](size_t Work) {
    if (Work > RemainingWork) {
      RemainingWork = 0;
      return false;
    }
    RemainingWork -= Work;
    return true;
  };
  const auto Visit = [&](auto &&Self, symbolic::SymRef Ref,
                         unsigned Depth) -> const std::optional<Values> & {
    static const std::optional<Values> Unknown;
    if (!Spend(1) || Depth >= limits::kMaxScalarBranchFiniteDepth)
      return Unknown;
    const auto Found = Cache.find(Ref.index());
    if (Found != Cache.end())
      return Found->second;
    auto &Result = Cache[Ref.index()];
    const auto &Node = Ctx.node(Ref);
    if (!Node.Width || Node.Width > 64)
      return Result;
    if (const auto Constant = Ctx.asConst(Ref)) {
      Result = Values{Constant->getZExtValue()};
      return Result;
    }
    // Exact operator evaluation over independent complete child covers is a
    // complete (possibly larger) cover. Never sample an unbounded input.
    const auto Operands = Ctx.operands(Ref);
    llvm::SmallVector<const Values *, 4> Domains;
    size_t Products = 1;
    bool Complete = !Operands.empty() &&
                    Operands.size() <= limits::kMaxScalarBranchFiniteDepth;
    if (Complete)
      for (const auto Child : Operands) {
        const auto &Domain = Self(Self, Child, Depth + 1);
        if (!Domain || Domain->empty() ||
            Products >
                limits::kMaxScalarBranchFiniteProducts / Domain->size()) {
          Complete = false;
          break;
        }
        Products *= Domain->size();
        Domains.push_back(&*Domain);
      }
    if (Complete && Spend(Products * (1 + Operands.size()))) {
      std::set<uint64_t> Cover;
      llvm::SmallVector<llvm::APInt, 4> Args;
      const auto Product = [&](auto &&Next, size_t Index) -> bool {
        if (Index == Domains.size()) {
          Cover.insert(symbolic::evalNodeAP(Ctx, Ref, Args).getZExtValue());
          return Cover.size() <= limits::kMaxScalarBranchFiniteValues;
        }
        for (uint64_t Bits : *Domains[Index]) {
          Args.emplace_back(Ctx.width(Operands[Index]), Bits);
          const bool Bounded = Next(Next, Index + 1);
          Args.pop_back();
          if (!Bounded)
            return false;
        }
        return true;
      };
      if (Product(Product, 0)) {
        Result = Values(Cover.begin(), Cover.end());
        return Result;
      }
    }
    // Masks, extracts and comparisons can have finite results even when a
    // child is unbounded. Enumerate *all* assignments to the unknown bits.
    unsigned Budget = static_cast<unsigned>(
        std::min<size_t>(RemainingWork, std::numeric_limits<unsigned>::max()));
    const unsigned Before = Budget;
    const auto Bits = Known.query(Ref, Budget);
    if (!Spend(Before - Budget) || !Bits)
      return Result;
    const uint64_t Free = ~(Bits->Zero | Bits->One).getZExtValue() &
                          (UINT64_MAX >> (64 - Node.Width));
    size_t Count = 1;
    for (uint64_t Rest = Free; Rest; Rest &= Rest - 1) {
      if (Count > limits::kMaxScalarBranchFiniteValues / 2)
        return Result;
      Count *= 2;
    }
    if (!Spend(Count))
      return Result;
    Values Cover;
    Cover.reserve(Count);
    uint64_t Subset = 0;
    do {
      Cover.push_back(Bits->One.getZExtValue() | Subset);
      Subset = (Subset - Free) & Free;
    } while (Subset);
    Result = std::move(Cover);
    return Result;
  };
  return Visit(Visit, Expression, 0);
}

} // namespace neverd
