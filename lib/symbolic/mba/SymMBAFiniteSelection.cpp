//===- SymMBAFiniteSelection.cpp - Exact small lookup words --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recover bitwise fields and shifted bits from complete literal selections,
/// retaining their defaults and the caller's shared resource limits.
//===----------------------------------------------------------------------===//

#include "Limits.h"
#include "SymMBADetail.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <bit>
#include <map>
#include <optional>

namespace neverd::symbolic::detail {
namespace {

constexpr unsigned MaxRows = 1u << limits::kMaxResidualSynthesisAtoms;

struct LiteralTable {
  llvm::SmallVector<SymRef, limits::kMaxResidualSynthesisAtoms> Atoms;
  llvm::SmallVector<llvm::APInt, MaxRows> Values;
  SymRef Selector, Default;
  unsigned SelectorBits = 0;
};

bool chargeValues(unsigned Width, size_t Rows, WorkBudget &Budget,
                  size_t &Storage) {
  const size_t Words = (Width - 1) / 64 + 1;
  const size_t Bytes = (Rows + 1) * Words * sizeof(uint64_t);
  if (Bytes > Storage || !Budget.consume(Rows * (size_t(Width) + Words)))
    return false;
  Storage -= Bytes;
  return true;
}

std::optional<LiteralTable> ordinalTable(SymContext &Ctx, SymRef Root,
                                         const SolverLimits &Limits,
                                         WorkBudget &Budget, size_t &Storage) {
  struct Arm {
    unsigned Index;
    SymRef Value;
  };
  llvm::SmallVector<Arm, MaxRows> Arms;
  bool Seen[MaxRows]{};
  unsigned Largest = 0;
  SymRef Selector, Tail = Root;
  while (Ctx.op(Tail) == SymOp::Ite) {
    if (Arms.size() == MaxRows || !Budget.consume())
      return {};
    const auto Condition = Ctx.operand(Tail, 0);
    if (Ctx.op(Condition) != SymOp::Eq || !Ctx.isConst(Ctx.operand(Tail, 1)))
      return {};
    auto Input = Ctx.operand(Condition, 0);
    auto Literal = Ctx.operand(Condition, 1);
    if (Ctx.isConst(Input))
      std::swap(Input, Literal);
    if (!Ctx.isConst(Literal) || (Selector && Selector != Input))
      return {};
    const size_t KeyWords = (Ctx.width(Literal) - 1) / 64 + 1;
    if (KeyWords > Storage / sizeof(uint64_t) || !Budget.consume(KeyWords))
      return {};
    const auto Key = Ctx.constValue(Literal).getLimitedValue(MaxRows);
    // Sparse lists and duplicates retain their existing branch priorities.
    if (Key >= MaxRows || Seen[Key])
      return {};
    Selector = Input;
    Seen[Key] = true;
    Largest = std::max(Largest, unsigned(Key));
    Arms.push_back({unsigned(Key), Ctx.operand(Tail, 1)});
    Tail = Ctx.operand(Tail, 2);
  }
  // Two cases leave one selector bit beside the range guard. That reading is
  // a negated or shifted bit and never shorter than the two tests.
  if (Arms.size() < 3 || Largest + 1 != Arms.size() || !Ctx.isConst(Tail))
    return {};
  const unsigned Bits = std::bit_width(Largest);
  if (Bits > limits::kMaxResidualSynthesisAtoms ||
      Bits > Limits.MaxSynthesisAtoms)
    return {};
  const size_t Rows = size_t{1} << Bits;
  if (!chargeValues(Ctx.width(Root), Rows, Budget, Storage))
    return {};
  LiteralTable Table;
  Table.Values.assign(Rows, Ctx.constValue(Tail));
  for (const auto &Arm : Arms)
    Table.Values[Arm.Index] = Ctx.constValue(Arm.Value);
  Table.Selector = Selector;
  Table.SelectorBits = Bits;
  Table.Default = Tail;
  for (unsigned Bit = 0; Bit < Bits; ++Bit)
    Table.Atoms.push_back(Ctx.mkExtract(Selector, Bit, 1));
  return Table;
}

std::optional<LiteralTable> predicateTable(SymContext &Ctx, SymRef Root,
                                           const SolverLimits &Limits,
                                           WorkBudget &Budget,
                                           size_t &Storage) {
  LiteralTable Table;
  llvm::SmallVector<SymRef, MaxRows> Pending{Root}, Choices;
  while (!Pending.empty()) {
    const auto Value = Pending.pop_back_val();
    if (!Budget.consume())
      return {};
    if (Ctx.isConst(Value))
      continue;
    if (Ctx.op(Value) != SymOp::Ite)
      return {};
    if (llvm::is_contained(Choices, Value))
      continue;
    if (Choices.size() == MaxRows - 1)
      return {};
    Choices.push_back(Value);
    const auto Condition = Ctx.operand(Value, 0);
    if (!llvm::is_contained(Table.Atoms, Condition)) {
      if (Table.Atoms.size() == limits::kMaxResidualSynthesisAtoms ||
          Table.Atoms.size() == Limits.MaxSynthesisAtoms)
        return {};
      Table.Atoms.push_back(Condition);
    }
    Pending.push_back(Ctx.operand(Value, 1));
    Pending.push_back(Ctx.operand(Value, 2));
  }
  // `c ? 0xFF : 0` is already the shortest source spelling of a one-condition
  // table; reading it as fields prints `(uint8_t)(0 - (uint32_t)c)`.
  if (Table.Atoms.size() < 2)
    return {};
  const size_t Rows = size_t{1} << Table.Atoms.size();
  if (!chargeValues(Ctx.width(Root), Rows, Budget, Storage))
    return {};
  for (size_t Row = 0; Row < Rows; ++Row) {
    auto Value = Root;
    while (Ctx.op(Value) == SymOp::Ite) {
      if (!Budget.consume())
        return {};
      const auto It = llvm::find(Table.Atoms, Ctx.operand(Value, 0));
      const bool Condition = Row & (size_t{1} << (It - Table.Atoms.begin()));
      Value = Ctx.operand(Value, Condition ? 1 : 2);
    }
    Table.Values.push_back(Ctx.constValue(Value));
  }
  return Table;
}

std::optional<SymRef> packFields(SymContext &Ctx,
                                 llvm::ArrayRef<llvm::APInt> Values,
                                 llvm::ArrayRef<SymRef> Atoms,
                                 const SolverLimits &Limits, size_t MaxCost,
                                 WorkBudget &Budget, size_t &Storage) {
  const unsigned Width = Values.front().getBitWidth();
  const size_t Words = (Width - 1) / 64 + 1;
  llvm::APInt Fixed(Width, 0);
  std::map<uint64_t, llvm::APInt> Fields;
  for (unsigned Bit = 0; Bit < Width; ++Bit) {
    auto Table = TruthTable::zero(Atoms.size());
    for (size_t Row = 0; Row < Values.size(); ++Row)
      Table.setValue(Row, Values[Row][Bit]);
    if (Table.isZero())
      continue;
    if (Table.isOnes()) {
      Fixed.setBit(Bit);
      continue;
    }
    const auto Key = Table.packed();
    auto It = Fields.find(Key);
    if (It == Fields.end()) {
      const size_t FieldBytes = Words * sizeof(uint64_t) + 2 * sizeof(SymRef);
      if (FieldBytes > Storage || (Fields.size() + 1) * 3 >= MaxCost)
        return {};
      Storage -= FieldBytes;
      It = Fields.try_emplace(Key, Width, 0).first;
    }
    It->second.setBit(Bit);
  }
  llvm::SmallVector<SymRef, MaxRows> Terms;
  if (!Fixed.isZero())
    Terms.push_back(Ctx.mkConst(Fixed));
  for (const auto &[Pattern, Mask] : Fields) {
    auto Table = TruthTable::zero(Atoms.size());
    for (size_t Row = 0; Row < Values.size(); ++Row)
      Table.setValue(Row, Pattern & (uint64_t{1} << Row));
    const auto Work =
        bitwiseSynthesisWork(std::popcount(truthTableSupport(Table)));
    if (Work > Limits.SynthesisWork || !Budget.consume(Work))
      return {};
    auto Synthesis = Limits.synthesis(MaxCost);
    Synthesis.MaxWork = Work;
    const auto Predicate = synthesizeBitwise(Ctx, Table, Atoms, Synthesis);
    if (!Predicate)
      return {};
    Terms.push_back(
        Ctx.mkMul(Ctx.mkZExt(*Predicate, Width), Ctx.mkConst(Mask)));
  }
  return Terms.empty() ? Ctx.mkZero(Width) : Ctx.mkOr(Terms);
}

} // namespace

SymRef foldFiniteSelection(SymContext &Ctx, SymRef Root,
                           const MBAOptions &Options, WorkBudget &Budget,
                           size_t &Storage) {
  if (Ctx.op(Root) != SymOp::Ite)
    return Root;
  const auto Limits = resolveLimits(Options);
  auto Table = ordinalTable(Ctx, Root, Limits, Budget, Storage);
  if (!Table)
    Table = predicateTable(Ctx, Root, Limits, Budget, Storage);
  if (!Table)
    return Root;
  const auto Before = readingScore(Ctx, Root);
  auto Best = Root;
  auto Score = Before;
  auto Offer = [&](SymRef Value) {
    if (Table->Selector && Ctx.width(Table->Selector) > Table->SelectorBits) {
      const auto Within = Ctx.mkUlt(
          Table->Selector, Ctx.mkConst(Ctx.width(Table->Selector),
                                       uint64_t{1} << Table->SelectorBits));
      Value = Ctx.mkIte(Within, Value, Table->Default);
    }
    const auto Cost = readingScore(Ctx, Value);
    if (Cost < Score) {
      Score = Cost;
      Best = Value;
    }
  };

  // A selection of one-hot values is an exact shifted bit, optionally beside
  // common fixed bits. Synthesize its small exponent instead of expanding
  // every output bit to an independent predicate. All exponents are proved
  // in range by the complete table; a raw ordinal keeps its outer default.
  auto Common = Table->Values.front();
  for (const auto &Value : Table->Values)
    Common &= Value;
  const unsigned Width = Ctx.width(Root);
  if (chargeValues(Width, Table->Values.size(), Budget, Storage)) {
    llvm::SmallVector<llvm::APInt, MaxRows> Exponents;
    for (const auto &Value : Table->Values) {
      const auto Bit = Value ^ Common;
      if (!Bit.isPowerOf2())
        break;
      Exponents.emplace_back(Width, Bit.logBase2());
    }
    if (Exponents.size() == Table->Values.size())
      if (const auto Exponent = packFields(Ctx, Exponents, Table->Atoms, Limits,
                                           Before.Nodes, Budget, Storage))
        Offer(Ctx.mkOr(Ctx.mkConst(Common),
                       Ctx.mkShl(Ctx.mkOne(Width), *Exponent)));
  }
  if (Budget.consume(Table->Values.size() * size_t(Width)))
    if (const auto Value = packFields(Ctx, Table->Values, Table->Atoms, Limits,
                                      Before.Nodes, Budget, Storage))
      Offer(*Value);
  return Best;
}
} // namespace neverd::symbolic::detail
