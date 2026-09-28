//===- FiniteQueryCache.cpp - Bounded finite-proof reuse ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "FiniteQueryCache.h"

#include <algorithm>

namespace neverd::analysis::detail {
namespace {
using namespace symbolic;
using Key = std::vector<uint64_t>;

bool validRef(const SymContext &Ctx, SymRef Value) {
  return Value && Value.index() < Ctx.numNodes() && Ctx.width(Value);
}

// Match the operator semantics consumed by BitBlaster::encodeNode. Future
// operators miss until their semantic payload and shape have been reviewed.
bool supportedShape(const SymContext &Ctx, SymRef Value) {
  const auto &Node = Ctx.node(Value);
  const auto Args = Ctx.operands(Value);
  for (SymRef Arg : Args)
    if (!validRef(Ctx, Arg) || Arg.index() >= Value.index())
      return false;
  const auto SameWidth = [&] {
    return std::all_of(Args.begin(), Args.end(), [&](SymRef Arg) {
      return Ctx.width(Arg) == Node.Width;
    });
  };
  switch (Node.Op) {
  case SymOp::Const:
    return Args.empty();
  case SymOp::Var:
    return Args.empty() && Node.Aux < Ctx.numVars() &&
           Ctx.varInfo(static_cast<uint32_t>(Node.Aux)).Width == Node.Width;
  case SymOp::Add:
  case SymOp::Mul:
  case SymOp::And:
  case SymOp::Or:
  case SymOp::Xor:
    return !Args.empty() && SameWidth();
  case SymOp::Not:
    return Args.size() == 1 && SameWidth();
  case SymOp::Shl:
  case SymOp::LShr:
  case SymOp::AShr:
  case SymOp::Rol:
  case SymOp::Ror:
    return Args.size() == 2 && Ctx.width(Args[0]) == Node.Width;
  case SymOp::UDiv:
  case SymOp::SDiv:
  case SymOp::URem:
  case SymOp::SRem:
    return Args.size() == 2 && SameWidth();
  case SymOp::Extract:
    return Args.size() == 1 && Node.Aux <= Ctx.width(Args[0]) &&
           Node.Width <= Ctx.width(Args[0]) - Node.Aux;
  case SymOp::Concat: {
    uint64_t Width = 0;
    for (SymRef Arg : Args)
      Width += Ctx.width(Arg);
    return !Args.empty() && Width == Node.Width;
  }
  case SymOp::ZExt:
  case SymOp::SExt:
    return Args.size() == 1 && Ctx.width(Args[0]) < Node.Width;
  case SymOp::Ite:
    return Args.size() == 3 && Ctx.width(Args[0]) == 1 &&
           Ctx.width(Args[1]) == Node.Width && Ctx.width(Args[2]) == Node.Width;
  case SymOp::Eq:
  case SymOp::Ult:
  case SymOp::Ule:
  case SymOp::Slt:
  case SymOp::Sle:
    return Node.Width == 1 && Args.size() == 2 &&
           Ctx.width(Args[0]) == Ctx.width(Args[1]);
  }
  return false;
}

std::optional<Key> makeKey(const SymContext &Ctx, SymRef Predicate,
                           llvm::ArrayRef<SymRef> Values, uint32_t Limit,
                           uint64_t MaxWords) {
  if (!Limit || !validRef(Ctx, Predicate) || Ctx.width(Predicate) != 1 ||
      MaxWords < 3 || Values.size() > MaxWords - 3)
    return std::nullopt;
  for (SymRef Value : Values)
    if (!validRef(Ctx, Value) || Ctx.width(Value) > 64)
      return std::nullopt;

  // Each root/operand reserves its eventual two-word reference token. New
  // nodes replace that token with a definition and charge the additional
  // header, payload and operand work before growing any traversal storage.
  uint64_t Charged = 2;
  const auto Charge = [&](uint64_t Words) {
    if (Words > MaxWords - Charged)
      return false;
    Charged += Words;
    return true;
  };
  if (Values.size() >= (MaxWords - Charged) / 2 ||
      !Charge(2 * (Values.size() + 1)))
    return std::nullopt;
  Key Result{Limit, Values.size()};
  std::vector<SymRef> Pending(Values.rbegin(), Values.rend());
  Pending.push_back(Predicate);
  std::map<uint32_t, uint64_t> Seen;
  uint64_t Variables = 0;
  while (!Pending.empty()) {
    const SymRef Value = Pending.back();
    Pending.pop_back();
    const auto Found = Seen.find(Value.index());
    if (Found != Seen.end()) {
      Result.insert(Result.end(), {0, Found->second});
      continue;
    }
    const auto &Node = Ctx.node(Value);
    const uint64_t PayloadWords =
        Node.Op == SymOp::Const ? (uint64_t(Node.Width) + 63) / 64 : 1;
    if (!Charge(4 + PayloadWords) || !Charge(uint64_t(Node.NumOperands) * 2) ||
        !supportedShape(Ctx, Value))
      return std::nullopt;
    const uint64_t Id = Seen.size();
    Seen.emplace(Value.index(), Id);
    Result.insert(Result.end(), {1, Id, static_cast<uint64_t>(Node.Op),
                                 Node.Width, Node.NumOperands, PayloadWords});
    if (Node.Op == SymOp::Const) {
      // Aux indexes a context-local pool for wide constants. Serialize the
      // actual bits, including high words, never that storage index.
      const auto Constant = Ctx.constValue(Value);
      Result.insert(Result.end(), Constant.getRawData(),
                    Constant.getRawData() + Constant.getNumWords());
    } else if (Node.Op == SymOp::Var) {
      // BitBlaster allocates unconstrained bits per distinct variable node.
      // Freshness, diagnostic names and input provenance do not change them.
      Result.push_back(Variables++);
    } else {
      Result.push_back(Node.Aux);
    }
    const auto Args = Ctx.operands(Value);
    Pending.insert(Pending.end(), Args.rbegin(), Args.rend());
  }
  return Result;
}

std::optional<uint64_t> resultWords(const SymContext &Ctx,
                                    llvm::ArrayRef<SymRef> Values,
                                    uint32_t Limit, const FiniteValues &Result,
                                    uint64_t Budget) {
  // Account for entry/result containers as well as numeric tuple words, so
  // empty results and empty projections cannot create unbounded entries.
  uint64_t Words = 8;
  if (Budget < Words)
    return std::nullopt;
  if (Result.Status == FiniteValueStatus::TooManyValues)
    return Result.Tuples.empty() ? std::optional<uint64_t>(Words)
                                 : std::nullopt;
  if (Result.Status != FiniteValueStatus::Complete ||
      Result.Tuples.size() > Limit)
    return std::nullopt;
  for (const auto &Tuple : Result.Tuples) {
    if (Tuple.size() != Values.size() || Budget - Words < 3 ||
        Tuple.size() > Budget - Words - 3)
      return std::nullopt;
    Words += 3 + Tuple.size();
    for (size_t I = 0; I < Tuple.size(); ++I)
      if (Ctx.width(Values[I]) < 64 && (Tuple[I] >> Ctx.width(Values[I])) != 0)
        return std::nullopt;
  }
  return Words;
}
} // namespace

std::optional<FiniteValues> FiniteQueryCache::lookup(
    const symbolic::SymContext &Ctx, symbolic::SymRef Predicate,
    llvm::ArrayRef<symbolic::SymRef> Values, uint32_t Limit) const {
  const auto Key = makeKey(Ctx, Predicate, Values, Limit, MaxWords);
  if (!Key)
    return std::nullopt;
  const auto Found = Entries.find(*Key);
  return Found == Entries.end() ? std::nullopt
                                : std::optional<FiniteValues>(Found->second);
}

void FiniteQueryCache::store(const symbolic::SymContext &Ctx,
                             symbolic::SymRef Predicate,
                             llvm::ArrayRef<symbolic::SymRef> Values,
                             uint32_t Limit, const FiniteValues &Result) {
  if (Result.Status != FiniteValueStatus::Complete &&
      Result.Status != FiniteValueStatus::TooManyValues)
    return;
  auto Key = makeKey(Ctx, Predicate, Values, Limit, MaxWords);
  if (!Key || Entries.contains(*Key))
    return;
  const uint64_t Available = MaxWords - StoredWords;
  if (Key->size() > Available)
    return;
  const auto Words =
      resultWords(Ctx, Values, Limit, Result, Available - Key->size());
  if (!Words)
    return;
  StoredWords += Key->size() + *Words;
  Entries.emplace(std::move(*Key), Result);
}

} // namespace neverd::analysis::detail
