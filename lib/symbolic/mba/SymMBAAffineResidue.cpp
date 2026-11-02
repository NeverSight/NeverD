//===- SymMBAAffineResidue.cpp - Normalize modular factor residues --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Remove fixed powers of two and odd units from affine factors before
/// proving a product zero. Bounded low-bit prefixes give conservative
/// valuations; complete small-domain tables can also synthesize nonzero
/// products after checking every coefficient of their common affine base.
///
//===----------------------------------------------------------------------===//

#include "Limits.h"
#include "SymMBADetail.h"

#include "llvm/ADT/DenseSet.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::symbolic::detail {

namespace {

using Atom = std::vector<uint32_t>;
using Coefficients = std::map<Atom, llvm::APInt>;

struct Factor {
  Coefficients Terms;
  llvm::APInt Offset;
  unsigned Zeros = 0;
  bool FixedValuation = false;
};

} // namespace

struct ResidueMemo {
  struct Attempt {
    SymRef Result;
    size_t AvailableStorage;
    bool Multiple;
  };
  struct ParityPrefix {
    SymRef Root;
    std::vector<Factor> Factors;
  };
  std::map<std::pair<uint32_t, bool>, Attempt> Attempts;
  std::vector<std::pair<uint32_t, uint32_t>> Generated;
  std::optional<ParityPrefix> PreparedParity;
};

namespace {

// Retry a storage-limited attempt when more storage becomes available. Other
// completed attempts are reusable across callers' scratch allowances. A
// declined attempt is a search result, not an equivalence certificate.
template <typename Search>
SymRef reuseResidueAttempt(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                           size_t &Storage, bool Projected, Search Run) {
  const bool Cacheable = Budget.MemoOwner == &Ctx && Budget.MemoCapacity;
  const auto Key = std::make_pair(Root.index(), Projected);
  if (Cacheable && Budget.Residues) {
    const size_t LookupWork =
        1 + 2 * std::bit_width(Budget.Residues->Attempts.size() >> 1);
    if (Budget.canConsume(LookupWork)) {
      Budget.consume(LookupWork);
      auto It = Budget.Residues->Attempts.find(Key);
      if (It != Budget.Residues->Attempts.end() &&
          Storage <= It->second.AvailableStorage) {
        return It->second.Result;
      }
    }
  }
  const size_t Available = Storage;
  const size_t BeforeMultiple = Budget.MultipleResidueAttempts;
  bool StorageLimited = false;
  auto Result = Run(StorageLimited);
  const size_t Bytes =
      sizeof(decltype(ResidueMemo::Attempts)::value_type) + 4 * sizeof(void *) +
      (Budget.Residues ? 0 : sizeof(ResidueMemo) + 2 * sizeof(void *));
  const size_t InsertWork =
      1 + (Budget.Residues
               ? 2 * std::bit_width(Budget.Residues->Attempts.size() >> 1)
               : 0);
  if (Cacheable && !Budget.exhausted() && Budget.canConsume(InsertWork) &&
      Bytes <= Storage &&
      (!Budget.Residues ||
       Budget.Residues->Attempts.size() < Budget.MemoCapacity)) {
    Budget.consume(InsertWork);
    Storage -= Bytes;
    if (!Budget.Residues)
      Budget.Residues = std::make_shared<ResidueMemo>();
    Budget.Residues->Attempts.insert_or_assign(
        Key,
        ResidueMemo::Attempt{
            Result,
            StorageLimited ? Available : std::numeric_limits<size_t>::max(),
            !StorageLimited &&
                BeforeMultiple != Budget.MultipleResidueAttempts});
  }
  return Result;
}

// Keep the existing unit-offset proof cheap. This bounded hint does not
// inspect bitwise interiors or grant any equality.
bool hasScaledOffsets(const SymContext &Ctx, SymRef Root) {
  unsigned Scaled = 0;
  for (auto F : Ctx.operands(Root)) {
    if (Ctx.op(F) != SymOp::Add ||
        Ctx.numOperands(F) > limits::kMaxResidueBaseTerms ||
        !Ctx.isConst(Ctx.operand(F, 0)))
      continue;
    for (auto T : Ctx.operands(F)) {
      if (Ctx.op(T) == SymOp::Mul && Ctx.isConst(Ctx.operand(T, 0))) {
        if (++Scaled == limits::kMinAffineResidueScaledFactors)
          return true;
        break;
      }
    }
  }
  return false;
}

struct Resources {
  WorkBudget &Budget;
  size_t &Storage;
  size_t Words;
  size_t SourceWords;
  bool Failed = false;
  std::optional<size_t> OptionalStart;
  bool *StorageLimited = nullptr;

  size_t optionalRemaining() const {
    if (!OptionalStart)
      return limits::kMaxResidueDecisionWork;
    return limits::kMaxMultipleResidueWork -
           std::min(limits::kMaxMultipleResidueWork,
                    Budget.used() - *OptionalStart);
  }

  bool charge(size_t Work, size_t Bytes = 0) {
    if (Bytes > Storage && StorageLimited)
      *StorageLimited = true;
    if (OptionalStart &&
        (Work > optionalRemaining() || !Budget.canConsume(Work))) {
      Failed = true;
      return false;
    }
    if (Bytes > Storage || !Budget.consume(Work)) {
      Failed = true;
      return false;
    }
    Storage -= Bytes;
    return true;
  }

  bool scalar(size_t Work, size_t CopyWords = 0) {
    // A peeled extract can reveal a wider source than the product itself.
    // Charge copies cumulatively, even after scratch dies.
    return charge(Work,
                  4 * (sizeof(llvm::APInt) +
                       std::max(SourceWords, CopyWords) * sizeof(uint64_t)));
  }
};

// Precision is a search order, never evidence. An even factor or a correlated
// subgroup can make a smaller prefix useful even when the balanced estimate
// misses it, so retain the other bounded precisions after the first attempt.
template <typename Prover>
bool proveAtResiduePrecisions(size_t Count, unsigned MaxBits, unsigned Needed,
                              Resources &Res, Prover Prove,
                              const bool *Nonzero = nullptr) {
  unsigned First = MaxBits;
  size_t Potential = 0;
  for (unsigned Bits = 1; Bits < MaxBits; ++Bits) {
    Potential += Count >> Bits;
    if (Potential >= Needed) {
      First = Bits;
      break;
    }
  }
  if (Prove(First))
    return true;
  for (unsigned Bits = 1; Bits <= MaxBits; ++Bits) {
    if (Res.Failed || Res.Budget.exhausted() || (Nonzero && *Nonzero))
      break;
    if (Bits != First && Prove(Bits))
      return true;
  }
  return false;
}

// One concrete input is sufficient to reject a zero identity, never to grant
// a replacement. First try all-odd assignments cheaply. If those fail, count
// the exact trailing zeros of even factors at a few complete assignments.
// Correlated subexpressions always see the same actual input values.
bool hasNonzeroProductWitness(SymContext &Ctx, llvm::ArrayRef<SymRef> Factors,
                              Resources &Res, unsigned Needed,
                              llvm::ArrayRef<Factor> Prefixes = {}) {
  std::map<uint32_t, std::optional<bool>> Values;
  size_t Work = 0;
  uint64_t VariableValue = 1;
  auto Read = [&](auto &&Self, SymRef R,
                  unsigned Depth) -> std::optional<bool> {
    if (Work >= limits::kMaxResidueWitnessWork ||
        Depth >= limits::kMaxAffineResidueDepth || !Res.charge(1))
      return std::nullopt;
    ++Work;
    const auto Op = Ctx.op(R);
    // Native literals and assigned input leaves need neither an APInt copy
    // nor a map entry. Their parity is already stored inline.
    if (Op == SymOp::Var)
      return bool(VariableValue & 1);
    if (Ctx.isConst(R) && Ctx.width(R) <= std::numeric_limits<uint64_t>::digits)
      return bool(Ctx.node(R).Aux & 1);
    if (auto It = Values.find(R.index()); It != Values.end())
      return It->second;
    auto Evaluate = [&]() -> std::optional<bool> {
      if (Ctx.isConst(R)) {
        const size_t Words = (size_t(Ctx.width(R)) - 1) / 64 + 1;
        if (Words > limits::kMaxResidueWitnessWork - Work ||
            !Res.scalar(Words, Words))
          return std::nullopt;
        Work += Words;
        return Ctx.constValue(R)[0];
      }
      if (Op == SymOp::ZExt || Op == SymOp::SExt ||
          (Op == SymOp::Extract && !Ctx.node(R).Aux))
        return Self(Self, Ctx.operand(R, 0), Depth + 1);
      if (Op == SymOp::Not) {
        auto V = Self(Self, Ctx.operand(R, 0), Depth + 1);
        return V ? std::optional<bool>(!*V) : std::nullopt;
      }
      if (Op == SymOp::Shl && Ctx.isConst(Ctx.operand(R, 1))) {
        const size_t Words = (size_t(Ctx.width(R)) - 1) / 64 + 1;
        if (Words > limits::kMaxResidueWitnessWork - Work ||
            !Res.scalar(Words, Words))
          return std::nullopt;
        Work += Words;
        return Ctx.constValue(Ctx.operand(R, 1)).isZero()
                   ? Self(Self, Ctx.operand(R, 0), Depth + 1)
                   : std::optional<bool>(false);
      }
      if (Op != SymOp::Add && Op != SymOp::Mul && Op != SymOp::And &&
          Op != SymOp::Or && Op != SymOp::Xor)
        return std::nullopt;
      const bool Conjunction = Op == SymOp::Mul || Op == SymOp::And;
      if (Ctx.numOperands(R) == 2 && Ctx.isConst(Ctx.operand(R, 0)) &&
          Ctx.width(R) <= std::numeric_limits<uint64_t>::digits) {
        const bool Bit = Ctx.node(Ctx.operand(R, 0)).Aux & 1;
        if ((Conjunction && !Bit) || (Op == SymOp::Or && Bit))
          return Bit;
        auto Value = Self(Self, Ctx.operand(R, 1), Depth + 1);
        if (Value && Bit && (Op == SymOp::Add || Op == SymOp::Xor))
          *Value = !*Value;
        return Value;
      }
      std::optional<bool> Result = Conjunction;
      for (auto Child : Ctx.operands(R)) {
        auto V = Self(Self, Child, Depth + 1);
        if ((Conjunction && V && !*V) || (Op == SymOp::Or && V && *V))
          return V;
        if (!V || !Result) {
          Result = std::nullopt;
          continue;
        }
        if (Op == SymOp::Add || Op == SymOp::Xor)
          *Result ^= *V;
      }
      return Result;
    };
    auto Result = Evaluate();
    if (!Res.Failed && Res.charge(1, sizeof(decltype(Values)::value_type) +
                                         4 * sizeof(void *)))
      Values.emplace(R.index(), Result);
    return Result;
  };
  // Completed coefficient maps already describe the original factors at
  // their proved precision. Evaluate just their parity at the same concrete
  // input assignment, rather than walking the discarded high expressions.
  // An opaque atom still needs its actual value; independent atom choices
  // would not be a witness for correlated factors.
  auto Parity = [&](size_t I) -> std::optional<bool> {
    if (Prefixes.empty())
      return Read(Read, Factors[I], 0);
    if (Work >= limits::kMaxResidueWitnessWork || !Res.charge(1))
      return std::nullopt;
    ++Work;
    const auto &F = Prefixes[I];
    bool Value = F.Offset[0];
    for (const auto &[Atom, Coeff] : F.Terms) {
      if (Work >= limits::kMaxResidueWitnessWork || !Res.charge(1))
        return std::nullopt;
      ++Work;
      if (!Coeff[0])
        continue;
      bool Term = true;
      for (auto Id : Atom) {
        const auto Bit = Read(Read, SymRef(Id), 0);
        if (!Bit)
          return std::nullopt;
        if (!*Bit) {
          Term = false;
          break;
        }
      }
      Value ^= Term;
    }
    return Value;
  };
  for (bool Input : {true, false}) {
    VariableValue = Input;
    Values.clear();
    bool AllOdd = true;
    for (size_t I = 0; I < Factors.size(); ++I) {
      auto V = Parity(I);
      if (!V || !*V || Res.Failed) {
        AllOdd = false;
        break;
      }
    }
    if (AllOdd)
      return true;
    if (Res.Failed || Work >= limits::kMaxResidueWitnessWork)
      return false;
  }
  auto ZerosAt = [&](SymRef Factor) -> std::optional<unsigned> {
    llvm::SmallVector<SymRef, 16> Pending{Factor};
    llvm::SmallDenseSet<uint32_t, 16> Seen;
    while (!Pending.empty()) {
      const auto R = Pending.pop_back_val();
      if (Ctx.width(R) > 64 || Work >= limits::kMaxResidueWitnessWork ||
          !Res.charge(1))
        return std::nullopt;
      ++Work;
      if (Seen.contains(R.index()))
        continue;
      const auto Count = Ctx.numOperands(R);
      const size_t Units = 3 + Count;
      if (Units > limits::kMaxResidueWitnessWork - Work ||
          !Res.charge(
              Units,
              (sizeof(SymNode) + 8 * sizeof(uint32_t) + 12 * sizeof(void *)) +
                  Count * (2 * sizeof(SymRef) + sizeof(uint32_t))))
        return std::nullopt;
      Work += Units;
      Seen.insert(R.index());
      Pending.append(Ctx.operands(R).begin(), Ctx.operands(R).end());
    }
    SymEvalPlan Plan(Ctx, Factor);
    const auto Value =
        Plan.evalU64With([&](uint32_t) { return VariableValue; });
    return unsigned(std::countr_zero(Value));
  };
  for (uint64_t Input : {uint64_t(0), uint64_t(1), uint64_t(2)}) {
    VariableValue = Input;
    Values.clear();
    unsigned TotalZeros = 0;
    bool Complete = true;
    for (size_t I = 0; I < Factors.size(); ++I) {
      auto V = Parity(I);
      if (!V || Res.Failed) {
        Complete = false;
        break;
      }
      if (!*V) {
        auto Zeros = ZerosAt(Factors[I]);
        if (!Zeros || *Zeros >= Needed - TotalZeros) {
          Complete = false;
          break;
        }
        TotalZeros += *Zeros;
      }
    }
    if (Complete)
      return true;
    if (Res.Failed || Work >= limits::kMaxResidueWitnessWork)
      return false;
  }
  return false;
}

// Retain partial facts while computing a prefix. Unknown bits are always zero
// in Value; Known marks the bits that can participate in a proof. In
// particular, two independently even operands have a product divisible by
// four even though neither complete input prefix is constant.
struct PrefixValue {
  uint64_t Value = 0;
  uint64_t Known = 0;
};

// Cache completed prefix queries, including conservative unknown results.
// Exhausted queries never enter the cache. Partial facts never directly enter
// an affine coefficient map: get() still requires a complete constant prefix.
struct PrefixConstants {
  SymContext &Ctx;
  Resources &Res;
  std::map<std::pair<uint32_t, unsigned>, PrefixValue> Values;
  bool ExpandTopXors = false;

  std::optional<uint64_t> get(SymRef R, unsigned Bits) {
    auto Result = infer(R, Bits);
    return Result.Known == (uint64_t(1) << Bits) - 1
               ? std::optional<uint64_t>(Result.Value)
               : std::nullopt;
  }

  PrefixValue infer(SymRef R, unsigned Bits, unsigned Depth = 0) {
    if (!Res.charge(1))
      return {};
    const auto Op = Ctx.op(R);
    // These leaf facts need no recursive inference or cache entry. Reading
    // the already-normalized native literal also avoids an APInt copy for
    // every first prefix query; a free variable contributes no known bits.
    if (Op == SymOp::Var)
      return {};
    const uint64_t Mask = (uint64_t(1) << Bits) - 1;
    if (Op == SymOp::Const && Ctx.width(R) <= 64)
      return {Ctx.node(R).Aux & Mask, Mask};
    auto Key = std::make_pair(R.index(), Bits);
    if (auto It = Values.find(Key); It != Values.end())
      return It->second;
    if (Depth >= limits::kMaxAffineResidueDepth)
      return {};
    auto Evaluate = [&]() -> PrefixValue {
      if (Ctx.isConst(R)) {
        const size_t Words = (size_t(Ctx.width(R)) - 1) / 64 + 1;
        if (!Res.scalar(Words, Words))
          return {};
        return {Ctx.constValue(R).zextOrTrunc(Bits).getZExtValue(), Mask};
      }
      if ((Op == SymOp::ZExt || Op == SymOp::SExt) &&
          Ctx.width(Ctx.operand(R, 0)) >= Bits)
        return infer(Ctx.operand(R, 0), Bits, Depth + 1);
      if (Op == SymOp::Extract && !Ctx.node(R).Aux)
        return infer(Ctx.operand(R, 0), Bits, Depth + 1);
      if (Op == SymOp::Not) {
        auto V = infer(Ctx.operand(R, 0), Bits, Depth + 1);
        return {~V.Value & V.Known, V.Known};
      }
      if (Op == SymOp::Shl && Ctx.isConst(Ctx.operand(R, 1))) {
        if (!Res.scalar(Res.SourceWords))
          return {};
        auto Count = Ctx.constValue(Ctx.operand(R, 1)).getLimitedValue(Bits);
        if (Count >= Bits)
          return {0, Mask};
        auto V = infer(Ctx.operand(R, 0), Bits - Count, Depth + 1);
        return {V.Value << Count,
                (V.Known << Count) | ((uint64_t(1) << Count) - 1)};
      }
      if (Op != SymOp::Add && Op != SymOp::Mul && Op != SymOp::And &&
          Op != SymOp::Or && Op != SymOp::Xor)
        return {};
      if (Ctx.numOperands(R) > limits::kMaxResidueBaseTerms)
        return {};
      PrefixValue Result{Op == SymOp::And   ? Mask
                         : Op == SymOp::Mul ? 1u
                                            : 0u,
                         Mask};
      for (auto Child : Ctx.operands(R)) {
        auto V = infer(Child, Bits, Depth + 1);
        if (Res.Failed)
          return {};
        if (Op == SymOp::And) {
          Result.Known = (Result.Known & V.Known) |
                         (Result.Known & ~Result.Value) | (V.Known & ~V.Value);
          Result.Value &= V.Value;
        } else if (Op == SymOp::Or) {
          Result.Known = (Result.Known & V.Known) | Result.Value | V.Value;
          Result.Value |= V.Value;
        } else if (Op == SymOp::Xor) {
          Result.Known &= V.Known;
          Result.Value = (Result.Value ^ V.Value) & Result.Known;
        } else {
          unsigned Known = Bits;
          if (Result.Known != Mask || V.Known != Mask) {
            const unsigned A = std::countr_one(Result.Known);
            const unsigned B = std::countr_one(V.Known);
            Known = std::min(A, B);
            if (Op == SymOp::Mul) {
              // Write each word as its known low prefix plus 2^K times
              // an arbitrary integer. Every unknown cross term has at
              // least this many trailing zeros; no independence is assumed.
              Known = std::min({Bits, A + B,
                                A + unsigned(std::countr_zero(V.Value)),
                                B + unsigned(std::countr_zero(Result.Value))});
            }
          }
          Result.Known = (uint64_t(1) << Known) - 1;
          Result.Value = (Op == SymOp::Add ? Result.Value + V.Value
                                           : Result.Value * V.Value) &
                         Result.Known;
        }
        if (Result.Known == Mask &&
            (((Op == SymOp::And || Op == SymOp::Mul) && !Result.Value) ||
             (Op == SymOp::Or && Result.Value == Mask)))
          return Result;
      }
      return Result;
    };
    auto Value = Evaluate();
    if (Res.Failed || !Res.charge(1, sizeof(decltype(Values)::value_type) +
                                         4 * sizeof(void *)))
      return {};
    Values.emplace(Key, Value);
    return Value;
  }
};

// A common low-zero prefix commutes with AND/OR/XOR. Reduce the remaining
// scalar units modulo the observed prefix. This is a congruence, not equality
// of the complete words: only the coefficient reader may use the result.
// Keeping the opaque operands at their native widths also avoids introducing
// unsupported sub-byte bitwise operations into source synthesis.
SymRef projectShiftedBitwise(PrefixConstants &Prefixes, SymRef R,
                             unsigned Bits) {
  auto &Ctx = Prefixes.Ctx;
  auto &Res = Prefixes.Res;
  llvm::SmallVector<SymRef, 16> Inputs(Ctx.operands(R));
  unsigned Shift = Bits;
  size_t Terms = 0;
  for (auto Child : Inputs) {
    if (Ctx.numOperands(Child) > limits::kMaxResidueBaseTerms)
      return {};
    Terms += 1 + Ctx.numOperands(Child);
    auto Literal = Ctx.isConst(Child) ? Child : SymRef{};
    if (Ctx.op(Child) == SymOp::Mul && Ctx.isConst(Ctx.operand(Child, 0)))
      Literal = Ctx.operand(Child, 0);
    if (!Literal || !Res.scalar(Res.SourceWords))
      return {};
    Shift = std::min(Shift, Ctx.constValue(Literal).countr_zero());
  }
  if (!Shift || Shift >= Bits)
    return {};
  if (!Res.charge(Terms * (1 + std::bit_width(Terms) + Res.SourceWords),
                  Terms * (4 * sizeof(SymNode) + 8 * sizeof(SymRef) +
                           4 * sizeof(llvm::APInt) +
                           4 * Res.SourceWords * sizeof(uint64_t))))
    return {};
  auto Unit = [&](SymRef Literal) {
    return Ctx.mkConst(Ctx.constValue(Literal)
                           .lshr(Shift)
                           .zextOrTrunc(Bits - Shift)
                           .zextOrTrunc(Ctx.width(R)));
  };
  llvm::SmallVector<SymRef, 16> Values;
  for (auto Child : Inputs) {
    SymRef Value;
    if (Ctx.isConst(Child)) {
      Value = Unit(Child);
    } else {
      llvm::SmallVector<SymRef, 16> Factors(Ctx.operands(Child));
      Factors[0] = Unit(Factors[0]);
      Value = Ctx.mkMul(Factors);
    }
    Values.push_back(Value);
  }
  auto Value = Ctx.op(R) == SymOp::And  ? Ctx.mkAnd(Values)
               : Ctx.op(R) == SymOp::Or ? Ctx.mkOr(Values)
                                        : Ctx.mkXor(Values);
  return Ctx.mkMul(Ctx.mkConst(llvm::APInt(Ctx.width(R), 1).shl(Shift)), Value);
}

bool readFactor(const SymContext &Ctx, SymRef R, const llvm::APInt &Scale,
                Factor &Out, Resources &Res, size_t &Edges, unsigned Depth = 0,
                PrefixConstants *Prefixes = nullptr) {
  // Ordinary affine recursion keeps the source width. Only prefix peeling
  // can expose another width, so leave the established fast path untouched.
  const size_t SourceWords =
      Prefixes ? (size_t(Ctx.width(R)) - 1) / 64 + 1 : Res.SourceWords;
  if (++Edges > limits::kMaxAffineResidueEdges ||
      !Res.scalar(1 + (Prefixes ? std::max(Res.Words, SourceWords) : Res.Words),
                  SourceWords))
    return false;
  if (Scale.isZero())
    return true;
  if (Ctx.isConst(R)) {
    if (!Res.charge(Scale.isOne() ? Res.Words
                                  : Res.Words * Res.Words + Res.Words))
      return false;
    auto Value = Ctx.constValue(R).zextOrTrunc(Scale.getBitWidth());
    if (!Scale.isOne())
      Value *= Scale;
    Out.Offset += Value;
    return true;
  }
  if (Prefixes && Depth < limits::kMaxAffineResidueDepth) {
    const auto Op = Ctx.op(R);
    const unsigned Bits = Scale.getBitWidth();
    auto Read = [&](SymRef Child, const llvm::APInt &Coefficient) {
      return readFactor(Ctx, Child, Coefficient, Out, Res, Edges, Depth + 1,
                        Prefixes);
    };
    if ((Op == SymOp::ZExt || Op == SymOp::SExt) &&
        Ctx.width(Ctx.operand(R, 0)) >= Bits)
      return Read(Ctx.operand(R, 0), Scale);
    if (Op == SymOp::Extract && !Ctx.node(R).Aux)
      return Read(Ctx.operand(R, 0), Scale);
    if (Op == SymOp::Not) {
      Out.Offset -= Scale;
      return Read(Ctx.operand(R, 0), -Scale);
    }
    if (Op == SymOp::Shl && Ctx.isConst(Ctx.operand(R, 1))) {
      if (!Res.scalar(Res.SourceWords))
        return false;
      auto Count = Ctx.constValue(Ctx.operand(R, 1)).getLimitedValue(Bits);
      return Count >= Bits || Read(Ctx.operand(R, 0), Scale.shl(Count));
    }
    if ((Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor) &&
        Ctx.numOperands(R) <= limits::kMaxResidueBaseTerms) {
      const uint64_t Mask = (uint64_t(1) << Bits) - 1;
      uint64_t Constant = Op == SymOp::And ? Mask : 0;
      SymRef Unknown;
      unsigned Unknowns = 0;
      for (auto Child : Ctx.operands(R)) {
        auto Value = Prefixes->get(Child, Bits);
        if (Res.Failed)
          return false;
        if (!Value) {
          Unknown = Child;
          ++Unknowns;
        } else if (Op == SymOp::And)
          Constant &= *Value;
        else if (Op == SymOp::Or)
          Constant |= *Value;
        else
          Constant ^= *Value;
      }
      if (!Unknowns || (Op == SymOp::And && !Constant) ||
          (Op == SymOp::Or && Constant == Mask)) {
        Out.Offset += Scale * Constant;
        return true;
      }
      if (Unknowns == 1) {
        if ((Op == SymOp::And && Constant == Mask) ||
            (Op != SymOp::And && !Constant))
          return Read(Unknown, Scale);
        if (Op == SymOp::Xor && Constant == Mask) {
          Out.Offset -= Scale;
          return Read(Unknown, -Scale);
        }
      }
      if (Op == SymOp::Xor && Prefixes->ExpandTopXors) {
        // XOR is additive modulo this precision when any overlap is confined
        // to its top bit: every resulting carry lies outside the word.
        uint64_t Seen = 0, Overlap = 0;
        for (auto Child : Ctx.operands(R)) {
          const auto Known = Prefixes->infer(Child, Bits);
          if (Res.Failed)
            return false;
          const auto Possible = Mask & ~(Known.Known & ~Known.Value);
          Overlap |= Seen & Possible;
          Seen |= Possible;
        }
        if (!(Overlap & (Mask >> 1))) {
          for (auto Child : Ctx.operands(R))
            if (!Read(Child, Scale))
              return false;
          return true;
        }
      }
      if (auto Projected = projectShiftedBitwise(*Prefixes, R, Bits))
        return Read(Projected, Scale);
      if (Res.Failed)
        return false;
    }
  }
  if (Depth < limits::kMaxAffineResidueDepth && Ctx.op(R) == SymOp::Add &&
      Ctx.numOperands(R) <= limits::kMaxResidueBaseTerms) {
    llvm::SmallVector<SymRef, 16> Terms(Ctx.operands(R));
    for (auto Term : Terms)
      if (!readFactor(Ctx, Term, Scale, Out, Res, Edges, Depth + 1, Prefixes))
        return false;
    return true;
  }

  auto Coeff = Scale;
  auto Ops = Ctx.operands(R);
  const bool Product =
      Ctx.op(R) == SymOp::Mul && Ops.size() <= limits::kMaxResidueBaseTerms;
  if (Product && Ctx.isConst(Ops.front())) {
    auto Value = Ctx.constValue(Ops.front()).zextOrTrunc(Scale.getBitWidth());
    if (Scale.isOne()) {
      if (!Res.charge(Res.Words))
        return false;
      Coeff = std::move(Value);
    } else {
      if (!Res.charge(Res.Words))
        return false;
      const bool Small = Value.getActiveBits() <= 64;
      if (!Res.charge(Small ? 2 * Res.Words : Res.Words * Res.Words))
        return false;
      Coeff = Small ? Scale * Value.getZExtValue() : Scale * Value;
    }
    Ops = Ops.drop_front();
    if (Ops.size() == 1 && Depth < limits::kMaxAffineResidueDepth)
      return readFactor(Ctx, Ops.front(), Coeff, Out, Res, Edges, Depth + 1,
                        Prefixes);
  }
  // A nonlinear atom may have a constant prefix even though its factors
  // only supply partial bits. Keep it opaque unless the whole queried
  // prefix is proved; a partial mask is not an affine coefficient.
  if (Prefixes && Product && Depth < limits::kMaxAffineResidueDepth &&
      !std::all_of(
          Ops.begin(), Ops.end(),
          [&](SymRef Child) { return Ctx.op(Child) == SymOp::Var; })) {
    // Unrestricted variable factors provide no known prefix. Keep native
    // monomials on the complete-affine path instead of querying and caching
    // the same unknown facts for every independently scaled occurrence.
    auto Value = Prefixes->get(R, Scale.getBitWidth());
    if (Res.Failed)
      return false;
    if (Value) {
      Out.Offset += Scale * *Value;
      return true;
    }
    const unsigned Bits = Scale.getBitWidth();
    unsigned First = Bits, Second = Bits;
    llvm::SmallVector<uint64_t, 16> Constants;
    for (auto Child : Ops) {
      auto Known = Prefixes->infer(Child, Bits);
      if (Res.Failed)
        return false;
      const unsigned Precision = std::countr_one(Known.Known);
      if (Precision < First) {
        Second = First;
        First = Precision;
      } else {
        Second = std::min(Second, Precision);
      }
      Constants.push_back(Known.Value);
      if (First + Second < Bits)
        break;
    }
    if (Constants.size() == Ops.size() && First + Second >= Bits) {
      // Write each operand as C + D with D divisible by its known prefix.
      // When every pair of D terms vanishes at this precision, the complete
      // product is affine in its original operands. No division by C is used.
      const size_t N = Ops.size();
      if (!Res.charge(N * (6 + Res.Words),
                      N * (sizeof(SymRef) + 3 * sizeof(uint64_t))))
        return false;
      llvm::SmallVector<SymRef, 16> Children(Ops);
      llvm::SmallVector<uint64_t, 16> Before(N, 1), After(N, 1);
      const uint64_t Mask = (uint64_t(1) << Bits) - 1;
      for (size_t I = 1; I < N; ++I)
        Before[I] = Before[I - 1] * Constants[I - 1] & Mask;
      for (size_t I = N - 1; I > 0; --I)
        After[I - 1] = After[I] * Constants[I] & Mask;
      Out.Offset -= Coeff * ((N - 1) * Before.back() * Constants.back());
      for (size_t I = 0; I < N; ++I)
        if (!readFactor(Ctx, Children[I], Coeff * (Before[I] * After[I]), Out,
                        Res, Edges, Depth + 1, Prefixes))
          return false;
      return true;
    }
  }
  const size_t Count = Product ? Ops.size() : 1;
  const size_t Bytes = sizeof(Coefficients::value_type) + 4 * sizeof(void *) +
                       Count * sizeof(uint32_t) + Res.Words * sizeof(uint64_t);
  if (!Res.charge(Count * (1 + std::bit_width(limits::kMaxResidueBaseTerms)) +
                      Res.Words,
                  Bytes))
    return false;
  Atom Key;
  Key.reserve(Count);
  if (Product) {
    for (auto Op : Ops)
      Key.push_back(Op.index());
  } else {
    Key.push_back(R.index());
  }
  // A multi-element atom is the complete flattened product of its handles;
  // a singleton is that exact opaque expression. No new DAG nodes are built.
  auto [It, Inserted] = Out.Terms.try_emplace(std::move(Key), Coeff);
  if (!Inserted)
    It->second += Coeff;
  if (It->second.isZero())
    Out.Terms.erase(It);
  return Out.Terms.size() <= limits::kMaxResidueBaseTerms;
}

bool proveAffineResidues(const std::vector<Factor> &Factors, unsigned Bits,
                         unsigned Needed, Resources &Res) {
  if (!Bits)
    return false;
  const size_t N = Factors.size();
  const uint64_t Mask = ~uint64_t(0) >> (64 - Bits);
  using Groups = std::map<std::vector<uint64_t>, std::vector<llvm::APInt>>;
  Groups Families;
  for (const auto &F : Factors) {
    if (F.FixedValuation)
      continue;
    size_t KeySize = 0;
    for (const auto &[Key, Coeff] : F.Terms) {
      (void)Coeff;
      KeySize += Key.size() + 2;
    }
    const size_t Bytes = 2 * sizeof(Groups::value_type) + 8 * sizeof(void *) +
                         4 * sizeof(llvm::APInt) +
                         2 * KeySize * sizeof(uint64_t);
    if (!Res.charge(KeySize * (1 + std::bit_width(N)) +
                        F.Terms.size() * Res.Words,
                    Bytes))
      return false;
    uint64_t Pivot = 0;
    for (const auto &[Key, Coeff] : F.Terms) {
      (void)Key;
      auto Low = Coeff.extractBitsAsZExtValue(Bits, F.Zeros);
      if (Low & 1) {
        Pivot = Low;
        break;
      }
    }
    if (!Pivot)
      return false;
    uint64_t Inverse = 1;
    for (unsigned Known = 1; Known < Bits; Known *= 2) {
      if (!Res.charge(3))
        return false;
      Inverse *= 2 - Pivot * Inverse;
    }
    // Verify the unit before using its low-bit inverse in a proof key.
    if ((Pivot * Inverse & Mask) != 1)
      return false;
    std::vector<uint64_t> Key;
    Key.reserve(KeySize);
    for (const auto &[Atom, Coeff] : F.Terms) {
      const uint64_t Low =
          Coeff.extractBitsAsZExtValue(Bits, F.Zeros) * Inverse & Mask;
      if (!Low)
        continue;
      Key.push_back(Atom.size());
      Key.insert(Key.end(), Atom.begin(), Atom.end());
      Key.push_back(Low);
    }
    const uint64_t Offset =
        F.Offset.extractBitsAsZExtValue(Bits, F.Zeros) * Inverse & Mask;
    Families[std::move(Key)].emplace_back(Bits, Offset);
  }
  for (auto &[Key, Offsets] : Families) {
    (void)Key;
    if (Offsets.size() < 2)
      continue;
    auto Zeros = minimumProductResidueZeros(Offsets, Bits, Needed, Res.Budget);
    if (!Zeros)
      return false;
    Needed -= *Zeros;
    if (!Needed)
      return true;
  }
  return false;
}

// Each row is a complete congruence for its original factor modulo 2^Bits.
// In a residue branch, replacing factor i by its known low word changes the
// product by a multiple of 2^(Bits + sum_{j != i} zeros[j]). Retain the
// precision of every factor that still contributes, proving separately that
// all products of two or more unobserved remainders vanish.
SymRef synthesizeResidualResidueTable(
    SymContext &Ctx, SymRef Root, llvm::ArrayRef<Factor> Factors,
    llvm::ArrayRef<SymRef> Originals, const llvm::APInt &Scale, unsigned Bits,
    unsigned Needed, Resources &Res, bool &DecisionAttempted,
    SymRef &DecisionResult, bool &DecisionRequested, bool &IncompatibleBase,
    bool &DecisionComplete) {
  if (!Bits || Bits > limits::kMaxAffineResidueTableBits || !Needed ||
      Needed > limits::kMaxAffineResidueTableBits ||
      Factors.size() != Originals.size() || Factors.empty() ||
      Factors.size() > limits::kMaxResidueProductFactors ||
      Bits > Ctx.width(Root) || Scale.getBitWidth() != Ctx.width(Root) ||
      Needed != Ctx.width(Root) - Scale.countr_zero())
    return Root;
  const unsigned Width = Ctx.width(Root);
  const uint64_t Mask = (uint64_t(1) << Bits) - 1;
  const uint64_t ResultMask = (uint64_t(1) << Needed) - 1;
  auto Low = [&](const llvm::APInt &V) {
    return V.zextOrTrunc(Bits).getZExtValue();
  };
  const Factor *Basis = nullptr;
  const Atom *Pivot = nullptr;
  for (const auto &F : Factors) {
    if (F.Offset.getBitWidth() != Bits)
      return Root;
    for (const auto &[Key, Coeff] : F.Terms) {
      if (Coeff.getBitWidth() != Bits)
        return Root;
      if (!Res.scalar(Res.Words))
        return Root;
      if (Low(Coeff) & 1) {
        Basis = &F;
        Pivot = &Key;
        break;
      }
    }
    if (Basis)
      break;
  }
  if (!Basis)
    return Root;
  const uint64_t Odd = Low(Basis->Terms.at(*Pivot));
  uint64_t Inverse = 1;
  for (unsigned Known = 1; Known < Bits; Known *= 2) {
    if (!Res.charge(3))
      return Root;
    Inverse *= 2 - Odd * Inverse;
  }
  if ((Odd * Inverse & Mask) != 1)
    return Root;
  std::map<Atom, uint64_t> Base;
  for (const auto &[Key, Coeff] : Basis->Terms) {
    if (!Res.scalar(Res.Words) ||
        !Res.charge(Key.size(), sizeof(decltype(Base)::value_type) +
                                    4 * sizeof(void *) +
                                    Key.size() * sizeof(uint32_t)))
      return Root;
    const auto C = Low(Coeff) * Inverse & Mask;
    if (C)
      Base.emplace(Key, C);
  }
  std::vector<std::pair<uint64_t, uint64_t>> Rows;
  if (!Res.charge(Factors.size(), Factors.size() * sizeof(Rows.front())))
    return Root;
  Rows.reserve(Factors.size());
  for (const auto &F : Factors) {
    if (F.Offset.getBitWidth() != Bits)
      return Root;
    for (const auto &[Key, C] : F.Terms) {
      (void)Key;
      if (C.getBitWidth() != Bits)
        return Root;
    }
    if (!Res.scalar(2 * Res.Words))
      return Root;
    auto It = F.Terms.find(*Pivot);
    const uint64_t A = It == F.Terms.end() ? 0 : Low(It->second);
    for (const auto &[Key, C] : Base) {
      if (!Res.scalar(Res.Words +
                      Key.size() * (1 + std::bit_width(F.Terms.size()))))
        return Root;
      auto Term = F.Terms.find(Key);
      const uint64_t Actual = Term == F.Terms.end() ? 0 : Low(Term->second);
      if ((A * C & Mask) != Actual) {
        IncompatibleBase = true;
        return Root;
      }
    }
    for (const auto &[Key, C] : F.Terms) {
      if (!Res.scalar(Res.Words +
                      Key.size() * (1 + std::bit_width(Base.size()))))
        return Root;
      if (!Base.contains(Key) && Low(C)) {
        IncompatibleBase = true;
        return Root;
      }
    }
    Rows.emplace_back(A, Low(F.Offset));
  }

  const size_t Count = size_t(1) << Bits;
  const size_t N = Factors.size();
  // One coefficient table per retained factor, plus a constant-only row.
  const size_t Cells = (N + 1) * Count;
  const size_t NodeBytes = sizeof(SymNode) + 4 * sizeof(SymRef) +
                           sizeof(llvm::APInt) +
                           Res.SourceWords * sizeof(uint64_t);
  if (!Res.charge(
          Count * (5 * N + 1),
          Cells * sizeof(uint64_t) + (N + 1) * sizeof(std::vector<uint64_t>) +
              N * (sizeof(uint64_t) + 2 * sizeof(unsigned) + sizeof(SymRef))))
    return Root;
  std::vector<std::vector<uint64_t>> Tables(N + 1,
                                            std::vector<uint64_t>(Count));
  std::vector<uint64_t> Values(N);
  std::vector<unsigned> Zeros(N), Required(N);
  bool MultipleResiduals = false;
  for (size_t X = 0; X < Count; ++X) {
    size_t Total = 0;
    for (size_t I = 0; I < N; ++I) {
      Values[I] = (Rows[I].first * X + Rows[I].second) & Mask;
      Zeros[I] = Values[I] ? unsigned(std::countr_zero(Values[I])) : Bits;
      Total += Zeros[I];
    }
    if (Total >= Needed)
      continue;
    size_t Residual = N;
    size_t ResidualCount = 0;
    unsigned Largest = 0, SecondLargest = 0;
    for (size_t I = 0; I < N; ++I) {
      // Here Total < Needed; this subtraction cannot underflow.
      if (Needed - (Total - Zeros[I]) <= Bits)
        continue;
      Residual = I;
      ++ResidualCount;
      if (Zeros[I] >= Largest) {
        SecondLargest = Largest;
        Largest = Zeros[I];
      } else {
        SecondLargest = std::max(SecondLargest, Zeros[I]);
      }
    }
    if (ResidualCount > 1) {
      DecisionRequested = true;
      if (!Res.OptionalStart) {
        Res.OptionalStart = Res.Budget.used();
        ++Res.Budget.MultipleResidueAttempts;
      }
      if (!DecisionAttempted) {
        DecisionAttempted = true;
        const auto Exact = foldResidueDecisions(
            Ctx, Root, Res.Budget, Res.Storage,
            Needed > limits::kMaxUnconditionedResidueDecisionBits,
            Res.optionalRemaining(), Res.StorageLimited, &DecisionComplete);
        if (Exact != Root) {
          DecisionResult = Exact;
          return Exact;
        }
      }
      // Write each retained factor as v_i + d_i, with 2^Bits dividing d_i.
      // Every term containing two or more d_i vanishes if the least such
      // valuation covers Needed. Replacing further v_i by d_i cannot lower
      // that valuation because Zeros[i] <= Bits. Never divide by v_i: a row
      // may contain zero or an even, noninvertible value.
      if (2 * Bits + Total - Largest - SecondLargest < Needed)
        return Root;
      MultipleResiduals = true;
      if (!Res.charge(6 * N + 1, (N + 1) * sizeof(uint64_t)))
        return Root;
      std::vector<uint64_t> Before(N + 1, 1);
      for (size_t I = 0; I < N; ++I)
        Before[I + 1] = Before[I] * Values[I] & ResultMask;
      uint64_t After = 1;
      for (size_t I = N; I-- != 0;) {
        const unsigned Precision = Needed - (Total - Zeros[I]);
        if (Precision > Bits) {
          Tables[I][X] =
              (Before[I] * After) & ((uint64_t(1) << (Needed - Zeros[I])) - 1);
          Required[I] = std::max(Required[I], Precision);
        }
        After = After * Values[I] & ResultMask;
      }
      // sum_i F_i*prod_{j!=i}(v_j) counts the constant product once for
      // every retained factor. Restore its single occurrence exactly.
      Tables[N][X] = (1 - ResidualCount) * Before[N] & ResultMask;
      continue;
    }
    uint64_t Coefficient = 1;
    for (size_t I = 0; I < N; ++I)
      if (I != Residual)
        Coefficient = Coefficient * Values[I] & ResultMask;
    if (Residual != N) {
      // This row also proves low zeros of the retained factor. Discard only
      // coefficient bits whose product with those zeros is unobservable.
      Coefficient &= (uint64_t(1) << (Needed - Zeros[Residual])) - 1;
      Required[Residual] = std::max(
          Required[Residual], unsigned(Needed - (Total - Zeros[Residual])));
    }
    Tables[Residual][X] = Coefficient;
  }
  size_t Active = 0;
  for (const auto &Table : Tables)
    Active += std::any_of(Table.begin(), Table.end(),
                          [](uint64_t V) { return V != 0; });
  if (!Active)
    return Res.charge(1) ? Ctx.mkZero(Width) : Root;
  // Reserve both bit-lookup and arithmetic decision candidates, their score
  // walks, and the final sum before creating any expression nodes.
  const size_t Lookups = MultipleResiduals ? 1 : Active;
  if (!Res.charge(Lookups * Count * (2 * Needed + 32) + Ctx.dagSize(Root),
                  Lookups * Count * (sizeof(uint64_t) + 32 * NodeBytes)))
    return Root;
  llvm::SmallVector<SymRef, 16> Terms;
  for (const auto &[Key, C] : Base) {
    llvm::SmallVector<SymRef, 16> Ops;
    for (auto Id : Key) {
      auto Value = SymRef(Id);
      if (Ctx.width(Value) != Width) {
        const auto Nodes = Ctx.dagSize(Value);
        const auto CopyWords = (size_t(Ctx.width(Value)) - 1) / 64 + 1;
        if (!Res.charge(Nodes * (1 + CopyWords),
                        Nodes * (4 * sizeof(SymNode) + 8 * sizeof(SymRef) +
                                 4 * sizeof(llvm::APInt) +
                                 4 * CopyWords * sizeof(uint64_t))))
          return Root;
        Value = Ctx.mkZExtOrTrunc(Value, Width);
      }
      Ops.push_back(Value);
    }
    Terms.push_back(Ctx.mkMul(
        Ctx.mkConst(llvm::APInt(Bits, C).sextOrTrunc(Width)), Ctx.mkMul(Ops)));
  }
  const auto Word = Ctx.mkAdd(Terms);
  const unsigned LookupWidth = std::max(Width, 64u);
  auto Lookup = [&](const std::vector<uint64_t> &Table) -> SymRef {
    auto Build = [&](auto &&Self, size_t Start, size_t Step,
                     unsigned Bit) -> SymRef {
      const auto V = Table[Start];
      bool Constant = true;
      for (size_t J = Start; J < Count; J += Step)
        Constant &= Table[J] == V;
      if (Constant)
        return Ctx.mkConst(Width, V);
      auto Lo = Self(Self, Start, 2 * Step, Bit + 1);
      auto Hi = Self(Self, Start + Step, 2 * Step, Bit + 1);
      if (Lo == Hi)
        return Lo;
      auto Input = Ctx.mkAnd(Ctx.mkLShr(Word, Ctx.mkConst(Width, Bit)),
                             Ctx.mkConst(Width, 1));
      return Ctx.mkAdd(Lo, Ctx.mkMul(Input, Ctx.mkAdd(Hi, Ctx.mkNeg(Lo))));
    };
    auto Decision = Build(Build, 0, 1, 0);
    llvm::SmallVector<SymRef, 8> OutputBits;
    bool Packed = true;
    for (unsigned Bit = 0; Bit < Needed; ++Bit) {
      size_t Period = Count;
      auto At = [&](size_t X) { return (Table[X] >> Bit) & 1; };
      while (Period > 1) {
        bool Same = true;
        for (size_t X = 0; X < Period / 2; ++X)
          Same &= At(X) == At(X + Period / 2);
        if (!Same)
          break;
        Period /= 2;
      }
      if (Period > LookupWidth) {
        Packed = false;
        break;
      }
      llvm::APInt Pattern(LookupWidth, 0);
      for (size_t X = 0; X < Period; ++X)
        if (At(X))
          Pattern.setBit(X);
      if (Pattern.isZero())
        continue;
      auto Index = Ctx.mkZExt(Ctx.mkAnd(Word, Ctx.mkConst(Width, Period - 1)),
                              LookupWidth);
      auto Value = Ctx.mkZExt(
          Ctx.mkExtract(Ctx.mkLShr(Ctx.mkConst(Pattern), Index), 0, 1), Width);
      OutputBits.push_back(
          Ctx.mkMul(Ctx.mkConst(Width, uint64_t(1) << Bit), Value));
    }
    auto Coefficient = Decision;
    if (Packed) {
      if (OutputBits.empty())
        return Ctx.mkZero(Width);
      auto Lookup = Ctx.mkOr(OutputBits);
      if (readingScore(Ctx, Lookup) < readingScore(Ctx, Coefficient))
        Coefficient = Lookup;
    }
    // Fit the whole finite table, including its wrap period, before emitting
    // an affine low-word reading. Selecting offsets often needs only -2*Word
    // modulo eight rather than two separately reconstructed output bits.
    for (unsigned Precision = 1; Precision <= Needed; ++Precision) {
      const uint64_t Mask = (uint64_t(1) << Precision) - 1;
      const uint64_t B = Table.front();
      const uint64_t A = (Table[1] - B) & Mask;
      bool Equal = true;
      for (size_t X = 0; X < Count; ++X)
        Equal &= ((A * X + B) & Mask) == Table[X];
      if (!Equal)
        continue;
      auto Input = (A * Count & Mask)
                       ? Ctx.mkAnd(Word, Ctx.mkConst(Width, Count - 1))
                       : Word;
      auto Affine = Ctx.mkAnd(
          Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(
                                  llvm::APInt(Precision, A).sextOrTrunc(Width)),
                              Input),
                    Ctx.mkConst(llvm::APInt(Precision, B).sextOrTrunc(Width))),
          Ctx.mkConst(Width, Mask));
      if (readingScore(Ctx, Affine) < readingScore(Ctx, Coefficient))
        Coefficient = Affine;
    }
    return Coefficient;
  };
  auto EmitAtom = [&](const Atom &Key) -> SymRef {
    llvm::SmallVector<SymRef, 16> Atoms;
    if (!Res.charge(Key.size() * (1 + Res.SourceWords), Key.size() * NodeBytes))
      return {};
    for (auto Id : Key) {
      auto Value = SymRef(Id);
      if (Ctx.width(Value) != Width) {
        const auto Nodes = Ctx.dagSize(Value);
        const auto Words = (size_t(Ctx.width(Value)) - 1) / 64 + 1;
        if (!Res.charge(Nodes * (1 + Words),
                        Nodes * (4 * sizeof(SymNode) + 8 * sizeof(SymRef) +
                                 4 * sizeof(llvm::APInt) +
                                 4 * Words * sizeof(uint64_t))))
          return {};
        Value = Ctx.mkZExtOrTrunc(Value, Width);
      }
      Atoms.push_back(Value);
    }
    return Key.empty() ? Ctx.mkConst(Width, 1) : Ctx.mkMul(Atoms);
  };
  llvm::SmallVector<SymRef, 16> FactoredTerms;
  if (!MultipleResiduals)
    FactoredTerms.push_back(Ctx.mkMul(Ctx.mkConst(Scale), Lookup(Tables[N])));
  // Distribute the residue coefficient tables through each projected affine
  // map. Identical opaque atoms receive one complete coefficient table, so
  // shared affine coordinates are emitted once rather than once per factor.
  std::map<Atom, std::vector<uint64_t>> Columns;
  auto AddColumn = [&](const Atom &Key, const llvm::APInt &Coeff,
                       const std::vector<uint64_t> &Table) {
    if (!Res.charge(Count + Key.size() * (1 + std::bit_width(Columns.size()))))
      return false;
    auto It = Columns.find(Key);
    if (It == Columns.end()) {
      if (!Res.charge(Count, sizeof(decltype(Columns)::value_type) +
                                 4 * sizeof(void *) +
                                 Key.size() * sizeof(uint32_t) +
                                 Count * sizeof(uint64_t)))
        return false;
      It = Columns.emplace(Key, std::vector<uint64_t>(Count)).first;
    }
    // Required may differ between factors. Either representative of a
    // coefficient modulo 2^Required is valid only after multiplication by
    // this factor's table, whose nonzero rows prove the missing low zeros.
    const uint64_t C = Coeff.sextOrTrunc(Needed).getZExtValue();
    for (size_t X = 0; X < Count; ++X)
      It->second[X] = (It->second[X] + C * Table[X]) & ResultMask;
    return true;
  };
  if (!AddColumn({}, llvm::APInt(Needed, 1), Tables[N]))
    return Root;
  PrefixConstants Prefixes{Ctx, Res, {}, MultipleResiduals};
  for (size_t I = 0; I < N; ++I) {
    const auto &Table = Tables[I];
    if (std::none_of(Table.begin(), Table.end(), [](uint64_t V) { return V; }))
      continue;
    Factor Projected{{}, llvm::APInt(Required[I], 0)};
    size_t Edges = 0;
    if (!readFactor(Ctx, Originals[I], llvm::APInt(Required[I], 1), Projected,
                    Res, Edges, 0, &Prefixes))
      return Root;
    if (!MultipleResiduals) {
      if (!Res.charge(1 + 4 * Projected.Terms.size(),
                      (1 + 4 * Projected.Terms.size()) * NodeBytes))
        return Root;
      llvm::SmallVector<SymRef, 16> Addends{
          Ctx.mkConst(Projected.Offset.sextOrTrunc(Width))};
      for (const auto &[Key, C] : Projected.Terms) {
        auto Value = EmitAtom(Key);
        if (!Value)
          return Root;
        Addends.push_back(Ctx.mkMul(Ctx.mkConst(C.sextOrTrunc(Width)), Value));
      }
      auto Value = Ctx.mkAdd(Addends);
      if (readingScore(Ctx, Originals[I]) < readingScore(Ctx, Value))
        Value = Originals[I];
      FactoredTerms.push_back(
          Ctx.mkMul({Ctx.mkConst(Scale), Lookup(Table), Value}));
    }
    // Eliminate the pivot coordinate in favor of Word. Its low Bits bits
    // equal X in this row, so higher coefficient bits can move exactly into
    // the row's constant. Other coordinates retain their complete differences.
    auto PivotTerm = Projected.Terms.find(*Pivot);
    const auto A = PivotTerm == Projected.Terms.end()
                       ? llvm::APInt(Required[I], 0)
                       : PivotTerm->second;
    if (PivotTerm != Projected.Terms.end())
      Projected.Terms.erase(PivotTerm);
    for (const auto &[Key, C] : Base) {
      if (Key == *Pivot)
        continue;
      if (!Res.charge(Key.size() + 1, sizeof(Coefficients::value_type) +
                                          4 * sizeof(void *) +
                                          Key.size() * sizeof(uint32_t)))
        return Root;
      auto It = Projected.Terms.try_emplace(Key, Required[I], 0).first;
      It->second -= A * llvm::APInt(Bits, C).sextOrTrunc(Required[I]);
    }
    const unsigned CoeffBits = Required[I] > Bits ? Required[I] - Bits : 0;
    const uint64_t Reduced =
        A.getZExtValue() & ((uint64_t(1) << CoeffBits) - 1);
    const uint64_t Difference = A.getZExtValue() - Reduced;
    if (!AddColumn(Atom{Word.index()}, llvm::APInt(Needed, Reduced), Table) ||
        !AddColumn({}, Projected.Offset, Table) || !Res.charge(Count))
      return Root;
    auto &Constants = Columns.at(Atom{});
    for (size_t X = 0; X < Count; ++X)
      Constants[X] = (Constants[X] + Table[X] * Difference * X) & ResultMask;
    for (const auto &[Key, C] : Projected.Terms)
      if (!C.isZero() && !AddColumn(Key, C, Table))
        return Root;
  }
  auto Best = Root;
  if (!MultipleResiduals) {
    auto Factored = Ctx.mkAdd(FactoredTerms);
    if (readingScore(Ctx, Factored) < readingScore(Ctx, Root))
      Best = Factored;
  }
  // An atom with K known low bits equals V + 2^K*Q. Reduce its
  // coefficients modulo 2^(Needed-K), retaining the discarded part times V
  // in the constant column. In particular, cofactor tables multiplying a
  // high-bit disturbance need not preserve unobservable odd units.
  auto &Constant = Columns.at(Atom{});
  for (auto &[Key, Table] : Columns) {
    if (Key.size() != 1 || Ctx.width(SymRef(Key.front())) < Needed)
      continue;
    const auto Known = Prefixes.infer(SymRef(Key.front()), Needed);
    if (Res.Failed || !Res.charge(2 * Count))
      return Best;
    const unsigned Precision =
        std::min(Needed, unsigned(std::countr_one(Known.Known)));
    const uint64_t CoeffMask = (uint64_t(1) << (Needed - Precision)) - 1;
    for (size_t X = 0; X < Count; ++X) {
      const auto Reduced = Table[X] & CoeffMask;
      Constant[X] =
          (Constant[X] + (Table[X] - Reduced) * Known.Value) & ResultMask;
      Table[X] = Reduced;
    }
  }
  if (MultipleResiduals) {
    // Several retained factors often have the same complete coefficient
    // table. Factor that table once before emitting their sum, avoiding both
    // repeated lookups and a quadratic search for disjoint supports.
    using Groups = std::map<std::vector<uint64_t>, std::vector<const Atom *>>;
    Groups EqualTables;
    for (const auto &[Key, Table] : Columns) {
      if (!Res.charge(Count * (1 + std::bit_width(EqualTables.size())),
                      sizeof(const Atom *)))
        return Best;
      if (std::none_of(Table.begin(), Table.end(),
                       [](uint64_t V) { return V; }))
        continue;
      auto It = EqualTables.find(Table);
      if (It == EqualTables.end()) {
        if (!Res.charge(Count, sizeof(Groups::value_type) + 4 * sizeof(void *) +
                                   Count * sizeof(uint64_t)))
          return Best;
        It = EqualTables.emplace(Table, std::vector<const Atom *>{}).first;
      }
      It->second.push_back(&Key);
    }
    if (!Res.charge(EqualTables.size() * Count * (2 * Needed + 32),
                    EqualTables.size() * Count *
                        (sizeof(uint64_t) + 32 * NodeBytes)))
      return Best;
    llvm::SmallVector<SymRef, 16> Summands;
    for (const auto &[Table, Keys] : EqualTables) {
      llvm::SmallVector<SymRef, 16> Values;
      for (const auto *Key : Keys) {
        auto Value = EmitAtom(*Key);
        if (!Value)
          return Best;
        Values.push_back(Value);
      }
      Summands.push_back(Ctx.mkMul(Lookup(Table), Ctx.mkAdd(Values)));
    }
    bool EvenZero = true, OddZero = true;
    for (const auto &[Key, Table] : Columns) {
      (void)Key;
      if (!Res.charge(Count))
        return Best;
      for (size_t X = 0; X < Count; ++X) {
        EvenZero &= (X & 1) || !Table[X];
        OddZero &= !(X & 1) || !Table[X];
      }
    }
    auto Sum = Summands.empty() ? Ctx.mkZero(Width) : Ctx.mkAdd(Summands);
    auto Unconditional = Ctx.mkMul(Ctx.mkConst(Scale), Sum);
    if (EvenZero != OddZero) {
      auto Selector = Ctx.mkAnd(Word, Ctx.mkConst(Width, 1));
      if (OddZero)
        Selector = Ctx.mkXor(Selector, Ctx.mkConst(Width, 1));
      Sum = Ctx.mkMul(Selector, Sum);
    }
    auto Result = Ctx.mkMul(Ctx.mkConst(Scale), Sum);

    // Rebuilding an algebraically identical word cannot add information to
    // a finished decision search over independent variables. Keep this
    // alternative's arithmetic spelling, without repeating that bit proof.
    auto Reduced =
        DecisionComplete
            ? Result
            : foldResidueDecisions(
                  Ctx, Result, Res.Budget, Res.Storage,
                  Needed > limits::kMaxUnconditionedResidueDecisionBits,
                  Res.optionalRemaining(), Res.StorageLimited);

    if (Reduced != Result &&
        readingScore(Ctx, Reduced) < readingScore(Ctx, Root)) {
      DecisionResult = Reduced;
      return Reduced;
    }
    if (readingScore(Ctx, Unconditional) < readingScore(Ctx, Best)) {
      DecisionResult = Unconditional;
      return Unconditional;
    }
    return Best;
  }
  // Column combination may split factors into more table-bearing atoms.
  // Account for both candidate forms and their score walks before emission.
  if (!Res.charge(Columns.size() * Count * (2 * Needed + 32),
                  Columns.size() * Count * (sizeof(uint64_t) + 32 * NodeBytes)))
    return Best;
  // Disjoint residue supports can select structurally shared expressions
  // before applying their common coefficient. Align commutative operands by
  // shape; the signature orders a search only and never proves equality.
  // Only arithmetic and bitwise nodes transport the low-word congruence;
  // shifts, division and other unsupported boundaries remain opaque.
  std::map<uint32_t, uint64_t> Shapes;
  auto Shape = [&](auto &&Self, SymRef R, unsigned Depth) -> uint64_t {
    if (Res.Failed)
      return 0;
    if (auto It = Shapes.find(R.index()); It != Shapes.end())
      return It->second;
    if (!Res.charge(1,
                    sizeof(decltype(Shapes)::value_type) + 4 * sizeof(void *)))
      return 0;
    if (Res.Failed)
      return 0;
    if (Depth >= limits::kMaxAffineResidueDepth ||
        Ctx.numOperands(R) > limits::kMaxResidueBaseTerms) {
      Shapes.emplace(R.index(), R.index());
      return R.index();
    }
    uint64_t H =
        (uint64_t(Ctx.op(R)) + 1) * 0x9e3779b97f4a7c15ULL + Ctx.width(R);
    if (Ctx.isConst(R))
      H = 0;
    else if (!Ctx.numOperands(R))
      H ^= R.index();
    else {
      std::vector<uint64_t> Children;
      for (auto Child : Ctx.operands(R)) {
        if (Ctx.op(R) == SymOp::Add && Ctx.isConst(Child))
          continue;
        Children.push_back(Self(Self, Child, Depth + 1));
      }
      if (!Res.charge(Children.size() * (1 + std::bit_width(Children.size())),
                      Children.size() * sizeof(uint64_t)))
        return 0;
      std::sort(Children.begin(), Children.end());
      if (Ctx.op(R) == SymOp::Add && Children.size() == 1)
        H = Children.front();
      else
        for (auto C : Children)
          H = (H ^ C) * 0x9e3779b97f4a7c15ULL;
    }
    Shapes.emplace(R.index(), H);
    return H;
  };
  auto Select = [&](auto &&Self, llvm::ArrayRef<SymRef> Values,
                    unsigned Depth) -> SymRef {
    if (Depth >= limits::kMaxAffineResidueDepth ||
        !Res.charge(Count * (2 * Needed + 32),
                    Count * (sizeof(uint64_t) + 32 * NodeBytes)))
      return {};
    if (std::all_of(Values.begin(), Values.end(),
                    [&](SymRef R) { return R == Values.front(); }))
      return Values.front();
    if (std::all_of(Values.begin(), Values.end(),
                    [&](SymRef R) { return Ctx.isConst(R); })) {
      std::vector<uint64_t> Table;
      for (auto R : Values)
        Table.push_back(Ctx.constValue(R).zextOrTrunc(Needed).getZExtValue());
      return Lookup(Table);
    }
    // An absent literal in an addition is a zero literal. This permits
    // selecting among U, U+2, U+4, ... without repeating U in each branch.
    const bool Add = std::any_of(Values.begin(), Values.end(), [&](SymRef R) {
      return Ctx.op(R) == SymOp::Add;
    });
    const auto Op = Add ? SymOp::Add : Ctx.op(Values.front());
    if (Op != SymOp::Add && Op != SymOp::Mul && !isBitwise(Op))
      return {};
    std::vector<std::vector<SymRef>> Rows;
    size_t Arity = 0;
    for (auto R : Values) {
      if (Ctx.width(R) != Width || (!Add && Ctx.op(R) != Op))
        return {};
      std::vector<SymRef> Row;
      SymRef Literal = Ctx.mkZero(Width);
      if (Add && Ctx.op(R) != SymOp::Add)
        Row.push_back(R);
      else {
        llvm::SmallVector<SymRef, 16> Inputs(Ctx.operands(R));
        if (Inputs.size() > limits::kMaxResidueBaseTerms)
          return {};
        for (auto Child : Inputs) {
          if (Add && Ctx.isConst(Child))
            Literal = Child;
          else
            Row.push_back(Child);
        }
      }
      for (auto Child : Row)
        Shape(Shape, Child, 0);
      if (Res.Failed ||
          !Res.charge(Row.size() * (1 + std::bit_width(Row.size())),
                      (Row.size() + 1) * sizeof(SymRef)))
        return {};
      std::stable_sort(Row.begin(), Row.end(), [&](SymRef A, SymRef B) {
        return Shapes.at(A.index()) < Shapes.at(B.index());
      });
      if (Add)
        Row.insert(Row.begin(), Literal);
      if (Rows.empty())
        Arity = Row.size();
      else if (Row.size() != Arity)
        return {};
      Rows.push_back(std::move(Row));
    }
    llvm::SmallVector<SymRef, 16> Children;
    for (size_t I = 0; I < Arity; ++I) {
      llvm::SmallVector<SymRef, 16> Column;
      for (const auto &Row : Rows)
        Column.push_back(Row[I]);
      auto Value = Self(Self, Column, Depth + 1);
      if (!Value)
        return {};
      Children.push_back(Value);
    }
    if (Op == SymOp::Add)
      return Ctx.mkAdd(Children);
    if (Op == SymOp::Mul)
      return Ctx.mkMul(Children);
    if (Op == SymOp::And)
      return Ctx.mkAnd(Children);
    if (Op == SymOp::Or)
      return Ctx.mkOr(Children);
    if (Op == SymOp::Xor)
      return Ctx.mkXor(Children);
    return Ctx.mkNot(Children.front());
  };
  std::vector<const Atom *> Combined;
  llvm::SmallVector<SymRef, 16> Summands;
  for (const auto &[Key, Table] : Columns) {
    if (Key.size() != 1 ||
        std::find(Combined.begin(), Combined.end(), &Key) != Combined.end())
      continue;
    const auto RootAtom = SymRef(Key.front());
    const auto Signature = Shape(Shape, RootAtom, 0);
    uint64_t Coeff = 0;
    bool Uniform = true;
    for (auto V : Table) {
      if (!V)
        continue;
      if (Coeff && Coeff != V)
        Uniform = false;
      Coeff = V;
    }
    if (!Coeff || !Uniform || Res.Failed)
      continue;
    if (!Res.charge(Columns.size() * (1 + Combined.size()) + Count,
                    Count * (sizeof(uint64_t) + sizeof(SymRef))))
      return Best;
    std::vector<uint64_t> Support = Table;
    std::vector<SymRef> Values(Count, RootAtom);
    std::vector<const Atom *> Members{&Key};
    for (const auto &[Other, Weights] : Columns) {
      if (Other.size() != 1 || &Other == &Key ||
          std::find(Combined.begin(), Combined.end(), &Other) !=
              Combined.end() ||
          Shape(Shape, SymRef(Other.front()), 0) != Signature)
        continue;
      if (!Res.charge(Count, Count * (sizeof(uint64_t) + sizeof(SymRef))))
        return Best;
      bool Disjoint = true, Active = false;
      for (size_t X = 0; X < Count; ++X) {
        Active |= Weights[X] != 0;
        Disjoint &= !Weights[X] || (Weights[X] == Coeff && !Support[X]);
      }
      if (!Active || !Disjoint)
        continue;
      Members.push_back(&Other);
      for (size_t X = 0; X < Count; ++X)
        if (Weights[X]) {
          Values[X] = SymRef(Other.front());
          Support[X] = Coeff;
        }
    }
    if (Members.size() < 2)
      continue;
    auto Merged = Select(Select, Values, 0);
    if (Res.Failed)
      return Best;
    if (!Merged)
      continue;
    auto Term = Ctx.mkMul({Ctx.mkConst(Scale), Lookup(Support), Merged});
    Summands.push_back(Term);
    Combined.insert(Combined.end(), Members.begin(), Members.end());
  }
  for (const auto &[Key, Table] : Columns) {
    if (std::find(Combined.begin(), Combined.end(), &Key) != Combined.end())
      continue;
    if (std::none_of(Table.begin(), Table.end(), [](uint64_t V) { return V; }))
      continue;
    auto Coefficient = Lookup(Table);
    auto Term = Ctx.mkMul(Ctx.mkConst(Scale), Coefficient);
    if (!Key.empty()) {
      auto Value = EmitAtom(Key);
      if (!Value)
        return Best;
      Term = Ctx.mkMul(Term, Value);
    }
    Summands.push_back(Term);
  }
  auto Result = Ctx.mkAdd(Summands);
  return readingScore(Ctx, Result) < readingScore(Ctx, Best) ? Result : Best;
}

// Nonzero neighbors can still be much simpler functions of one affine word.
// Check every coefficient before using that word, then enumerate its complete
// observed domain. Independent opaque atoms need not be independent inputs.
SymRef synthesizeResidueTable(SymContext &Ctx, SymRef Root,
                              const std::vector<Factor> &Factors,
                              unsigned Observed, Resources &Res,
                              bool *CompleteBase) {
  if (Observed > limits::kMaxAffineResidueTableBits)
    return Root;
  const Factor *Basis = nullptr;
  const Atom *Pivot = nullptr;
  unsigned Shift = Ctx.width(Root);
  for (const auto &F : Factors)
    for (const auto &[Key, Coeff] : F.Terms) {
      if (!Res.charge(Res.Words))
        return Root;
      const unsigned Zeros = Coeff.countr_zero() - F.Zeros;
      if (Zeros < Shift) {
        Shift = Zeros;
        Basis = &F;
        Pivot = &Key;
      }
    }
  if (!Basis || Shift >= Observed)
    return Root;
  const uint64_t Mask = (uint64_t(1) << Observed) - 1;
  auto Low = [&](const llvm::APInt &V, unsigned Zeros) {
    return V.lshr(Zeros).zextOrTrunc(Observed).getZExtValue();
  };
  if (!Res.scalar(Res.Words))
    return Root;
  const uint64_t Odd = Low(Basis->Terms.at(*Pivot), Basis->Zeros + Shift);
  uint64_t Inverse = 1;
  for (unsigned Bits = 1; Bits < Observed; Bits *= 2) {
    if (!Res.charge(3))
      return Root;
    Inverse *= 2 - Odd * Inverse;
  }
  if ((Odd * Inverse & Mask) != 1)
    return Root;
  std::map<Atom, uint64_t> Base;
  for (const auto &[Key, Coeff] : Basis->Terms) {
    if (!Res.scalar(Res.Words) ||
        !Res.charge(Key.size(), sizeof(decltype(Base)::value_type) +
                                    4 * sizeof(void *) +
                                    Key.size() * sizeof(uint32_t)))
      return Root;
    const auto C = Low(Coeff, Basis->Zeros + Shift) * Inverse & Mask;
    if (C)
      Base.emplace(Key, C);
  }
  std::vector<std::pair<uint64_t, uint64_t>> Rows;
  if (!Res.charge(Factors.size(), Factors.size() * sizeof(Rows.front())))
    return Root;
  Rows.reserve(Factors.size());
  for (const auto &F : Factors) {
    if (!Res.scalar(Res.Words))
      return Root;
    auto It = F.Terms.find(*Pivot);
    const uint64_t A = It == F.Terms.end() ? 0 : Low(It->second, F.Zeros);
    // Base's pivot is exactly one. Compare the complete union of atom keys;
    // absent terms and extra nonzero terms must also agree modulo Observed.
    for (const auto &[Key, C] : Base) {
      if (!Res.scalar(Res.Words +
                      Key.size() * (1 + std::bit_width(F.Terms.size()))))
        return Root;
      auto Term = F.Terms.find(Key);
      const auto Actual =
          Term == F.Terms.end() ? 0 : Low(Term->second, F.Zeros);
      if ((A * C & Mask) != Actual)
        return Root;
    }
    for (const auto &[Key, C] : F.Terms) {
      if (!Res.scalar(Res.Words +
                      Key.size() * (1 + std::bit_width(Base.size()))))
        return Root;
      if (!Base.contains(Key) && Low(C, F.Zeros))
        return Root;
    }
    Rows.emplace_back(A, Low(F.Offset, F.Zeros));
  }

  if (CompleteBase)
    *CompleteBase = true;
  const size_t Count = size_t(1) << Observed;
  // Reserve all table, decision, lookup, polynomial and factored candidates
  // before emission. Output constants retain the source word width even when
  // the coefficient proof only needed a narrow prefix.
  const size_t NodeBytes = sizeof(SymNode) + 4 * sizeof(SymRef) +
                           sizeof(llvm::APInt) +
                           Res.SourceWords * sizeof(uint64_t);
  if (Res.Storage / Count < sizeof(uint64_t) ||
      NodeBytes > (Res.Storage / Count - sizeof(uint64_t)) / 20 ||
      !Res.charge(Count * (3 * Rows.size() + Observed + 20),
                  Count * (sizeof(uint64_t) + 20 * NodeBytes)) ||
      !Res.charge(Ctx.dagSize(Root) + 10 * Rows.size(),
                  2 * Rows.size() * sizeof(SymRef) +
                      (Observed + 1) * sizeof(uint64_t)))
    return Root;
  std::vector<uint64_t> Values(Count, 1);
  for (size_t X = 0; X < Count; ++X)
    for (auto [A, B] : Rows)
      Values[X] = Values[X] * (A * X + B) & Mask;

  const unsigned Width = Ctx.width(Root);
  llvm::SmallVector<SymRef, 16> Terms;
  for (const auto &[Key, C] : Base) {
    llvm::SmallVector<SymRef, 16> Ops;
    for (auto Id : Key) {
      auto Value = SymRef(Id);
      if (Ctx.width(Value) != Width) {
        // Prefix atoms can come from peeled casts. Restore the word width
        // before emitting the full-width polynomial or lookup expression.
        const auto Nodes = Ctx.dagSize(Value);
        const auto CopyWords = (size_t(Ctx.width(Value)) - 1) / 64 + 1;
        if (!Res.charge(Nodes * (1 + CopyWords),
                        Nodes * (4 * sizeof(SymNode) + 8 * sizeof(SymRef) +
                                 4 * sizeof(llvm::APInt) +
                                 4 * CopyWords * sizeof(uint64_t))))
          return Root;
        Value = Ctx.mkZExtOrTrunc(Value, Width);
      }
      Ops.push_back(Value);
    }
    auto Coeff = Ctx.mkConst(llvm::APInt(Observed, C).sextOrTrunc(Width));
    Terms.push_back(Ctx.mkMul(Coeff, Ctx.mkMul(Ops)));
  }
  const auto Word = Ctx.mkAdd(Terms);
  // Remove repeated periods and a common low-bit support condition. The
  // remaining output bits often fit in one native-width lookup constant.
  size_t Period = Count;
  while (Period > 1 && std::equal(Values.begin(), Values.begin() + Period / 2,
                                  Values.begin() + Period / 2))
    Period /= 2;
  size_t First = 0;
  while (First < Period && !Values[First])
    ++First;
  if (First == Period)
    return Ctx.mkZero(Width);
  unsigned Prefix = 0;
  while ((size_t(1) << (Prefix + 1)) <= Period &&
         (size_t(1) << (Prefix + 1)) <= Width) {
    const size_t LowMask = (size_t(1) << (Prefix + 1)) - 1;
    bool Same = true;
    for (size_t I = 0; I < Period; ++I)
      Same &= !Values[I] || (I & LowMask) == (First & LowMask);
    if (!Same)
      break;
    ++Prefix;
  }
  // Each output bit can have a smaller period than the whole word. Native
  // lookup words may widen to 64 bits so byte-sized inputs need not expand
  // a 64-entry bit table into dozens of arithmetic selectors.
  llvm::SmallVector<SymRef, 8> OutputBits;
  const unsigned LookupWidth = std::max(Width, 64u);
  bool Packed = true;
  for (unsigned Bit = 0; Bit < Observed; ++Bit) {
    size_t BitPeriod = Period;
    auto BitAt = [&](size_t I) { return (Values[I] >> Bit) & 1; };
    while (BitPeriod > 1) {
      bool Same = true;
      for (size_t I = 0; I < BitPeriod / 2; ++I)
        Same &= BitAt(I) == BitAt(I + BitPeriod / 2);
      if (!Same)
        break;
      BitPeriod /= 2;
    }
    if (BitPeriod > LookupWidth) {
      Packed = false;
      break;
    }
    llvm::APInt Pattern(LookupWidth, 0);
    for (size_t I = 0; I < BitPeriod; ++I)
      if (BitAt(I))
        Pattern.setBit(I);
    if (Pattern.isZero())
      continue;
    auto Index = Ctx.mkZExt(Ctx.mkAnd(Word, Ctx.mkConst(Width, BitPeriod - 1)),
                            LookupWidth);
    auto Value = Ctx.mkZExt(
        Ctx.mkExtract(Ctx.mkLShr(Ctx.mkConst(Pattern), Index), 0, 1), Width);
    OutputBits.push_back(Ctx.mkMul(
        Ctx.mkConst(llvm::APInt(Width, 1).shl(Width - Observed + Bit)), Value));
  }
  SymRef Result = Packed ? Ctx.mkOr(OutputBits) : SymRef{};
  auto Build = [&](auto &&Self, size_t Start, size_t Step,
                   unsigned Bit) -> SymRef {
    const auto V = Values[Start];
    bool Constant = true;
    for (size_t I = Start; I < Count; I += Step)
      Constant &= Values[I] == V;
    if (Constant)
      return Ctx.mkConst(llvm::APInt(Width, V).shl(Width - Observed));
    auto Lo = Self(Self, Start, 2 * Step, Bit + 1);
    auto Hi = Self(Self, Start + Step, 2 * Step, Bit + 1);
    if (Lo == Hi)
      return Lo;
    auto Input = Ctx.mkAnd(Ctx.mkLShr(Word, Ctx.mkConst(Width, Bit)),
                           Ctx.mkConst(Width, 1));
    return Ctx.mkAdd(Lo, Ctx.mkMul(Input, Ctx.mkAdd(Hi, Ctx.mkNeg(Lo))));
  };
  if (!Result)
    Result = Build(Build, 0, 1, 0);
  // On the common support, Word = Support + 2^Prefix*T. Every degree D
  // coefficient then contains 2^(Prefix*D), so higher terms vanish exactly.
  if (Prefix) {
    const size_t Support = First & ((size_t(1) << Prefix) - 1);
    const unsigned Degree = (Observed - 1) / Prefix;
    std::vector<uint64_t> Poly(Degree + 1, 0);
    Poly[0] = 1;
    for (auto [A, B] : Rows) {
      B = A * Support + B;
      A <<= Prefix;
      for (unsigned I = Degree; I; --I)
        Poly[I] = (B * Poly[I] + A * Poly[I - 1]) & Mask;
      Poly[0] = B * Poly[0] & Mask;
    }
    auto T = Ctx.mkLShr(Word, Ctx.mkConst(Width, Prefix));
    auto Polynomial = Ctx.mkZero(Width);
    for (unsigned I = Degree + 1; I--;) {
      auto C = Ctx.mkConst(llvm::APInt(Width, Poly[I]).shl(Width - Observed));
      Polynomial = Ctx.mkAdd(C, Ctx.mkMul(T, Polynomial));
    }
    auto Index = Ctx.mkAnd(Word, Ctx.mkConst(Width, (size_t(1) << Prefix) - 1));
    auto Gate = Ctx.mkAnd(
        Ctx.mkLShr(Ctx.mkConst(llvm::APInt(Width, 1).shl(Support)), Index),
        Ctx.mkConst(Width, 1));
    Polynomial = Ctx.mkMul(Polynomial, Gate);
    if (readingScore(Ctx, Polynomial) < readingScore(Ctx, Result))
      Result = Polynomial;
  }
  llvm::SmallVector<SymRef, 32> Normalized;
  uint64_t Unit = 1;
  for (auto [A, B] : Rows) {
    if (!A) {
      Unit = Unit * B & Mask;
      continue;
    }
    const auto Odd = A >> std::countr_zero(A);
    uint64_t Inv = 1;
    for (unsigned I = 1; I < Observed; I *= 2)
      Inv *= 2 - Odd * Inv;
    Unit = Unit * Odd & Mask;
    auto Slope = Ctx.mkConst(llvm::APInt(Observed, A * Inv).sextOrTrunc(Width));
    auto Offset =
        Ctx.mkConst(llvm::APInt(Observed, B * Inv).sextOrTrunc(Width));
    Normalized.push_back(Ctx.mkAdd(Ctx.mkMul(Slope, Word), Offset));
  }
  Normalized.push_back(
      Ctx.mkConst(llvm::APInt(Width, Unit).shl(Width - Observed)));
  auto Factored = Ctx.mkMul(Normalized);
  return readingScore(Ctx, Result) <
                 std::min(readingScore(Ctx, Root), readingScore(Ctx, Factored))
             ? Result
             : Root;
}

} // namespace

void recordResidueReading(SymContext &Ctx, size_t First, WorkBudget &Budget,
                          size_t &Storage) {
  const size_t Bytes =
      2 * sizeof(std::pair<uint32_t, uint32_t>) +
      (Budget.Residues ? 0 : sizeof(ResidueMemo) + 2 * sizeof(void *));
  if (Budget.MemoOwner != &Ctx || !Budget.MemoCapacity ||
      First >= Ctx.numNodes() || Ctx.numNodes() > UINT32_MAX ||
      Bytes > Storage || !Budget.canConsume(1) ||
      (Budget.Residues &&
       Budget.Residues->Generated.size() >= Budget.MemoCapacity))
    return;
  Budget.consume();
  Storage -= Bytes;
  if (!Budget.Residues)
    Budget.Residues = std::make_shared<ResidueMemo>();
  Budget.Residues->Generated.emplace_back(uint32_t(First),
                                          uint32_t(Ctx.numNodes()));
}

bool hasCompletedResidueReading(const SymContext &Ctx, SymRef Root,
                                WorkBudget &Budget, bool GeneratedOnly) {
  if (Budget.MemoOwner != &Ctx || !Budget.Residues)
    return false;
  const auto &Memo = *Budget.Residues;
  if (Memo.Generated.empty() &&
      (GeneratedOnly || !Budget.MultipleResidueAttempts))
    return false;
  const size_t Work = 1 + std::bit_width(Memo.Generated.size()) +
                      std::bit_width(Memo.Attempts.size());
  if (!Budget.canConsume(Work))
    return false;
  Budget.consume(Work);
  auto Range = std::upper_bound(
      Memo.Generated.begin(), Memo.Generated.end(), Root.index(),
      [](uint32_t Index, const auto &R) { return Index < R.first; });
  if (Range != Memo.Generated.begin() &&
      Root.index() < std::prev(Range)->second)
    return true;
  if (GeneratedOnly || Ctx.op(Root) != SymOp::Mul)
    return false;
  const auto It = Memo.Attempts.find({Root.index(), false});
  return It != Memo.Attempts.end() && It->second.Multiple;
}

bool hasProjectedResidueFactors(const SymContext &Ctx, SymRef Root) {
  if (Ctx.op(Root) != SymOp::Mul ||
      Ctx.numOperands(Root) < limits::kMinResidueProductFactors ||
      Ctx.numOperands(Root) > limits::kMaxResidueProductFactors)
    return false;
  auto Offset = [&](SymRef R) {
    return Ctx.op(R) == SymOp::Add && Ctx.isConst(Ctx.operand(R, 0));
  };
  auto BitwiseTerm = [&](SymRef R, unsigned &Remaining) {
    if (isBitwise(Ctx.op(R)))
      return true;
    if (Ctx.op(R) != SymOp::Mul ||
        Ctx.numOperands(R) > limits::kMaxResidueBaseTerms)
      return false;
    for (auto Factor : Ctx.operands(R)) {
      if (!Remaining)
        return false;
      --Remaining;
      if (isBitwise(Ctx.op(Factor)))
        return true;
    }
    return false;
  };
  auto Wrapped = [&](auto &&Self, SymRef R, unsigned Depth,
                     unsigned &Remaining) -> bool {
    if (!Remaining)
      return false;
    --Remaining;
    const auto Op = Ctx.op(R);
    if (Ctx.numOperands(R) > limits::kMaxResidueBaseTerms)
      return false;
    if (Offset(R)) {
      for (auto Child : Ctx.operands(R))
        if (BitwiseTerm(Child, Remaining) ||
            (Ctx.op(Child) == SymOp::Mul &&
             Ctx.isConst(Ctx.operand(Child, 0)) &&
             (Ctx.numOperands(Child) > 2 || Ctx.width(Child) > 64)))
          return true;
      // Native scalar affine coordinates already have a cheap complete-map
      // reader. Do not run a second projection search on their nonzero
      // residuals; nonlinear monomials and wide coefficients benefit from it.
      return false;
    }
    // Reassociation may leave A*(B+c) under a bitwise wrapper. A literal
    // scale/shift is transparent to this hint; the reader still proves every
    // coefficient and accounts for any fixed zeros before using its prefix.
    const bool Scaled = Ctx.numOperands(R) == 2 &&
                        ((Op == SymOp::Mul && Ctx.isConst(Ctx.operand(R, 0))) ||
                         (Op == SymOp::Shl && Ctx.isConst(Ctx.operand(R, 1))));
    // A scalar may retain its offset internally: A*(B+c) + disturbance.
    // Only bitwise-bearing sums need the projected path; ordinary native
    // affine sums retain their complete-map fast path.
    const bool BitwiseSum =
        Op == SymOp::Add &&
        std::any_of(
            Ctx.operands(R).begin(), Ctx.operands(R).end(),
            [&](SymRef Child) { return BitwiseTerm(Child, Remaining); });
    if (!isBitwise(Op) && Op != SymOp::ZExt && Op != SymOp::SExt &&
        Op != SymOp::Extract && !Scaled && !BitwiseSum)
      return false;
    for (auto Child : Ctx.operands(R))
      if (Offset(Child) || (Depth && Self(Self, Child, Depth - 1, Remaining)))
        return true;
    return false;
  };
  for (auto F : Ctx.operands(Root)) {
    unsigned Remaining = limits::kMaxResiduePrefixHintNodes;
    if (Wrapped(Wrapped, F, limits::kMaxResiduePrefixHintDepth, Remaining))
      return true;
  }
  return false;
}

static SymRef foldProjectedProductResiduesImpl(SymContext &Ctx, SymRef Root,
                                               WorkBudget &Budget,
                                               size_t &Storage, bool Admitted,
                                               bool &StorageLimited) {
  if (Budget.exhausted() || Ctx.op(Root) != SymOp::Mul ||
      Ctx.numOperands(Root) < limits::kMinResidueProductFactors ||
      Ctx.numOperands(Root) > limits::kMaxResidueProductFactors ||
      (!Admitted && !hasProjectedResidueFactors(Ctx, Root)))
    return Root;
  const unsigned Width = Ctx.width(Root);
  llvm::SmallVector<SymRef, 16> Inputs(Ctx.operands(Root));
  auto Operands = llvm::ArrayRef<SymRef>(Inputs);
  const size_t SourceWords = (size_t(Width) - 1) / 64 + 1;
  Resources Res{Budget, Storage, 1, SourceWords};
  Res.StorageLimited = &StorageLimited;
  unsigned Needed = Width;
  if (Ctx.isConst(Operands.front())) {
    if (!Res.scalar(SourceWords))
      return Root;
    Needed -= Ctx.constValue(Operands.front()).countr_zero();
    Operands = Operands.drop_front();
  }
  if (!Needed)
    return Res.charge(1) ? Ctx.mkZero(Width) : Root;
  const unsigned MaxBits =
      std::min(Width, unsigned(std::bit_width(Operands.size() - 1)));
  // A balanced set of offsets can already establish enough zeros at a much
  // coarser precision. Start there so unrelated higher bits cannot obscure
  // the useful congruence. This estimate only orders attempts: every rewrite
  // still requires complete coefficient maps and an exact residue bound.
  PrefixConstants Prefixes{Ctx, Res, {}};
  bool WitnessChecked = false;
  bool Nonzero = false;
  auto Prove = [&](unsigned Bits) {
    if (!Bits || !Res.charge(Operands.size(), Operands.size() * sizeof(Factor)))
      return false;
    std::vector<Factor> Factors;
    Factors.reserve(Operands.size());
    unsigned Available = Bits;
    unsigned Remaining = Needed;
    for (auto R : Operands) {
      if (!Res.scalar(1))
        return false;
      Factor F{{}, llvm::APInt(Bits, 0)};
      size_t Edges = 0;
      if (!readFactor(Ctx, R, llvm::APInt(Bits, 1), F, Res, Edges, 0,
                      &Prefixes))
        return false;
      auto SetValuation = [&]() {
        unsigned CoeffZeros = F.Offset.getBitWidth();
        for (const auto &[Key, Coeff] : F.Terms) {
          (void)Key;
          if (!Res.charge(1))
            return false;
          CoeffZeros = std::min(CoeffZeros, Coeff.countr_zero());
        }
        const unsigned OffsetZeros = F.Offset.countr_zero();
        F.Zeros = std::min(CoeffZeros, OffsetZeros);
        F.FixedValuation = F.Terms.empty() || OffsetZeros < CoeffZeros;
        return true;
      };
      if (!SetValuation())
        return false;
      // Removing fixed zeros also removes observed bits. Widen just this
      // factor, leaving odd factors at their original (provable) precision.
      const unsigned ExtraBits =
          std::min({Width, Bits + F.Zeros, limits::kMaxResiduePrefixBits});
      if ((!F.FixedValuation || F.Offset.isZero()) && ExtraBits > Bits) {
        F = Factor{{}, llvm::APInt(ExtraBits, 0)};
        Edges = 0;
        if (!Res.scalar(1) ||
            !readFactor(Ctx, R, llvm::APInt(ExtraBits, 1), F, Res, Edges, 0,
                        &Prefixes) ||
            !SetValuation())
          return false;
      }
      if (F.Zeros >= Remaining)
        return true;
      Remaining -= F.Zeros;
      // A zero prefix guarantees only Bits zeros, not a zero full-width factor.
      if (!F.FixedValuation)
        Available = std::min(Available, F.Offset.getBitWidth() - F.Zeros);
      Factors.push_back(std::move(F));
    }
    if (!WitnessChecked && !Res.Failed &&
        Needed <= limits::kMaxAffineResidueTableBits) {
      WitnessChecked = true;
      Nonzero = hasNonzeroProductWitness(Ctx, Operands, Res, Needed, Factors);
    }
    if (Nonzero && Bits == 1 && Remaining == Needed &&
        Budget.MemoOwner == &Ctx && Budget.MemoCapacity) {
      // These complete parity maps are the next residual reader's first
      // precision. No factor was widened or stripped of fixed zeros. Move
      // their already charged storage across that boundary, retaining only
      // one pending product in this request's context-owned memo.
      const size_t Bytes =
          Budget.Residues ? 0 : sizeof(ResidueMemo) + 2 * sizeof(void *);
      if (Bytes <= Storage && Budget.canConsume(1)) {
        Budget.consume();
        Storage -= Bytes;
        if (!Budget.Residues)
          Budget.Residues = std::make_shared<ResidueMemo>();
        Budget.Residues->PreparedParity =
            ResidueMemo::ParityPrefix{Root, std::move(Factors)};
      }
    }
    if (Nonzero || Res.Failed)
      return false;
    return proveAffineResidues(Factors, Available, Remaining, Res);
  };
  // Failed precision attempts share the same cumulative resource account;
  // the cache only contains completed queries keyed by their exact width.
  if (proveAtResiduePrecisions(
          Operands.size(), MaxBits, Needed, Res,
          [&](unsigned Bits) {
            if (Prove(Bits))
              return true;
            if (!WitnessChecked && !Res.Failed &&
                Needed <= limits::kMaxAffineResidueTableBits) {
              WitnessChecked = true;
              Nonzero = hasNonzeroProductWitness(Ctx, Operands, Res, Needed);
            }
            return false;
          },
          &Nonzero))
    return Res.charge(1) ? Ctx.mkZero(Width) : Root;
  return Root;
}

SymRef foldProjectedProductResidues(SymContext &Ctx, SymRef Root,
                                    WorkBudget &Budget, size_t &Storage,
                                    bool Admitted) {
  if (!Admitted && !hasProjectedResidueFactors(Ctx, Root))
    return Root;
  return reuseResidueAttempt(Ctx, Root, Budget, Storage, true,
                             [&](bool &Limited) {
                               return foldProjectedProductResiduesImpl(
                                   Ctx, Root, Budget, Storage, true, Limited);
                             });
}

static SymRef foldResidualProductResiduesImpl(SymContext &Ctx, SymRef Root,
                                              WorkBudget &Budget,
                                              size_t &Storage,
                                              bool &StorageLimited) {
  if (Budget.exhausted() || Ctx.op(Root) != SymOp::Mul ||
      Ctx.numOperands(Root) < limits::kMinResidueProductFactors)
    return Root;
  const unsigned Width = Ctx.width(Root);
  const auto First = Ctx.operand(Root, 0);
  const bool HasScale = Ctx.isConst(First);
  const size_t SourceWords = (size_t(Width) - 1) / 64 + 1;
  Resources Res{Budget, Storage, 1, SourceWords};
  Res.StorageLimited = &StorageLimited;
  unsigned Needed = Width;
  if (HasScale) {
    if (!Res.scalar(SourceWords))
      return Root;
    Needed -= Ctx.constValue(First).countr_zero();
  }
  if (Ctx.numOperands(Root) > limits::kMaxResidueProductFactors) {
    const size_t Factors = Ctx.numOperands(Root) - HasScale;
    // Large products do not enter affine expansion. The bounded low-word
    // proof can isolate a dominant prefix group while retaining unmatched
    // factors. Its internal views coalesce repeated residues.
    if (!Needed || Needed > limits::kMaxAffineResidueTableBits ||
        Factors <= Needed)
      return Root;
    ++Budget.MultipleResidueAttempts;
    return foldResidueDecisions(Ctx, Root, Budget, Storage, true,
                                limits::kMaxMultipleResidueWork,
                                &StorageLimited);
  }
  llvm::SmallVector<SymRef, 16> Inputs(Ctx.operands(Root));
  auto Operands = llvm::ArrayRef<SymRef>(Inputs).drop_front(HasScale);
  PrefixConstants Prefixes{Ctx, Res, {}};
  SymRef Best = Root;
  bool DecisionAttempted = false;
  bool DecisionComplete = false;
  bool DecisionRequested = false;
  SymRef DecisionResult;
  if (Needed <= limits::kMaxAffineResidueTableBits) {
    const auto Scale = Ctx.isConst(Inputs.front())
                           ? Ctx.constValue(Inputs.front())
                           : llvm::APInt(Width, 1);
    for (unsigned Bits = 1; Bits <= Needed && !Res.Failed; ++Bits) {
      std::vector<Factor> Factors;
      bool Complete = true;
      if (Bits == 1 && Budget.MemoOwner == &Ctx && Budget.Residues &&
          Budget.Residues->PreparedParity &&
          Budget.Residues->PreparedParity->Root == Root) {
        if (!Res.charge(1))
          break;
        Factors = std::move(Budget.Residues->PreparedParity->Factors);
        Budget.Residues->PreparedParity.reset();
      } else {
        if (!Res.charge(Operands.size(), Operands.size() * sizeof(Factor)))
          break;
        Factors.reserve(Operands.size());
        for (auto R : Operands) {
          Factor F{{}, llvm::APInt(Bits, 0)};
          size_t Edges = 0;
          if (!readFactor(Ctx, R, llvm::APInt(Bits, 1), F, Res, Edges, 0,
                          &Prefixes)) {
            Complete = false;
            break;
          }
          Factors.push_back(std::move(F));
        }
      }
      if (!Complete)
        break;
      bool IncompatibleBase = false;
      auto Result = synthesizeResidualResidueTable(
          Ctx, Root, Factors, Operands, Scale, Bits, Needed, Res,
          DecisionAttempted, DecisionResult, DecisionRequested,
          IncompatibleBase, DecisionComplete);
      if (IncompatibleBase && !DecisionAttempted && Operands.size() > Needed &&
          (Operands.size() - 1) % residueUnitPeriod(Needed) <= 1) {
        // A different affine coordinate can still multiply a parity group
        // whose even branch vanishes. Complete unit periods and their next
        // exponents have cheap power readings. This is only a search hint;
        // the bounded proof retains every unmatched word.
        DecisionAttempted = DecisionRequested = true;
        if (!Res.OptionalStart) {
          Res.OptionalStart = Res.Budget.used();
          ++Res.Budget.MultipleResidueAttempts;
        }
        const auto Exact = foldResidueDecisions(
            Ctx, Root, Res.Budget, Res.Storage, true, Res.optionalRemaining(),
            Res.StorageLimited, &DecisionComplete);
        if (Exact != Root)
          DecisionResult = Exact;
      }
      if (DecisionResult &&
          readingScore(Ctx, DecisionResult) < readingScore(Ctx, Best))
        Best = DecisionResult;
      // A tiny observed word may have a shorter spelling over its shared
      // affine coordinate than over separate input bits. Keep that bounded
      // cofactor alternative even when the first decision reading improves.
      if (DecisionResult &&
          Needed > limits::kMaxUnconditionedResidueDecisionBits)
        return Best;
      if (Result != Root && readingScore(Ctx, Result) < readingScore(Ctx, Best))
        Best = Result;
      // Once the retained disturbances break the common affine coordinate,
      // stop this optional widening search. This grants no replacement;
      // other arithmetic and child-region searches retain their allowance.
      if (DecisionRequested && IncompatibleBase)
        break;
    }
  }
  return Best;
}

SymRef foldResidualProductResidues(SymContext &Ctx, SymRef Root,
                                   WorkBudget &Budget, size_t &Storage) {
  return reuseResidueAttempt(Ctx, Root, Budget, Storage, false,
                             [&](bool &Limited) {
                               return foldResidualProductResiduesImpl(
                                   Ctx, Root, Budget, Storage, Limited);
                             });
}

std::optional<SymRef>
foldAffineProductResidues(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                          size_t &Storage, std::optional<bool> ProjectedHint,
                          bool *CompleteBase) {
  if (CompleteBase)
    *CompleteBase = false;
  if (Ctx.op(Root) != SymOp::Mul ||
      Ctx.numOperands(Root) < limits::kMinResidueProductFactors ||
      Ctx.numOperands(Root) > limits::kMaxResidueProductFactors ||
      !hasScaledOffsets(Ctx, Root))
    return std::nullopt;
  const unsigned Width = Ctx.width(Root);
  const size_t SourceWords = (size_t(Width) - 1) / 64 + 1;
  if (SourceWords > Storage / (4 * sizeof(uint64_t)) ||
      !Budget.consume(SourceWords))
    return Root;
  Storage -= SourceWords * 4 * sizeof(uint64_t);
  auto Ops = Ctx.operands(Root);
  const auto Scale = Ctx.isConst(Ops.front()) ? Ctx.constValue(Ops.front())
                                              : llvm::APInt(Width, 1);
  if (Scale.isZero())
    return Budget.consume() ? Ctx.mkZero(Width) : Root;
  const unsigned ReadWidth = Width - Scale.countr_zero();
  const size_t Words = (size_t(ReadWidth) - 1) / 64 + 1;
  if (Words > (std::numeric_limits<size_t>::max() - Words) / Words ||
      Words > Storage / (4 * sizeof(uint64_t)))
    return Root;
  Resources Res{Budget, Storage, Words, SourceWords};
  const size_t N = Ctx.numOperands(Root);
  if (N > Storage / sizeof(Factor) || !Res.charge(N, N * sizeof(Factor)))
    return Root;
  std::vector<Factor> Factors;
  Factors.reserve(N);
  unsigned Needed = ReadWidth;
  PrefixConstants Prefixes{Ctx, Res, {}};
  const bool ProjectedFactors =
      ProjectedHint ? *ProjectedHint : hasProjectedResidueFactors(Ctx, Root);
  const bool Project =
      ReadWidth <= limits::kMaxResiduePrefixBits && ProjectedFactors;
  llvm::SmallVector<SymRef, 16> Inputs(Ctx.operands(Root));
  for (auto R : Inputs) {
    if (!Res.scalar(2 * Words))
      return Root;
    Factor F{{}, llvm::APInt(ReadWidth, 0)};
    size_t Edges = 0;
    if (Ctx.isConst(R))
      F.Offset = Scale.lshr(Width - ReadWidth).zextOrTrunc(ReadWidth);
    else if (!readFactor(Ctx, R, llvm::APInt(ReadWidth, 1), F, Res, Edges, 0,
                         Project ? &Prefixes : nullptr))
      return Root;
    unsigned CoeffZeros = ReadWidth;
    for (const auto &[Key, Coeff] : F.Terms) {
      (void)Key;
      if (!Res.charge(Words))
        return Root;
      CoeffZeros = std::min(CoeffZeros, Coeff.countr_zero());
    }
    const unsigned OffsetZeros = F.Offset.countr_zero();
    F.Zeros = std::min(CoeffZeros, OffsetZeros);
    if (F.Zeros >= Needed)
      return Res.charge(1) ? Ctx.mkZero(Width) : Root;
    Needed -= F.Zeros;
    F.FixedValuation = OffsetZeros < CoeffZeros;
    Factors.push_back(std::move(F));
  }

  // Examining only this prefix avoids full-width inversions. It can decline
  // a proof that needs deeper residues; it never overstates a valuation.
  const unsigned Bits = std::min(Needed, unsigned(std::bit_width(N)));
  if (proveAtResiduePrecisions(N, Bits, Needed, Res, [&](unsigned Precision) {
        return proveAffineResidues(Factors, Precision, Needed, Res);
      }))
    return Res.charge(1) ? Ctx.mkZero(Width) : Root;
  // Reassociation can absorb the enclosing scale into one additive factor.
  // Once all fixed zeros are known, normalize the exact coefficient maps at
  // their remaining precision. Never drop those zeros before they have been
  // included in Needed; the synthesized word restores their enclosing scale.
  if (Needed <= limits::kMaxAffineResidueTableBits &&
      (Needed < ReadWidth || !Project) && ProjectedFactors) {
    for (auto &F : Factors) {
      if (!Res.scalar(Words))
        return Root;
      Factor Reduced{{}, F.Offset.lshr(F.Zeros).zextOrTrunc(Needed)};
      size_t Edges = 0;
      for (const auto &[Key, C] : F.Terms) {
        if (!Res.scalar(Words))
          return Root;
        auto Coeff = C.lshr(F.Zeros).zextOrTrunc(Needed);
        if (Coeff.isZero())
          continue;
        if (Key.size() == 1) {
          if (!readFactor(Ctx, SymRef(Key.front()), Coeff, Reduced, Res, Edges,
                          0, &Prefixes))
            return Root;
        } else {
          if (!Res.charge(
                  Key.size() *
                      (1 + std::bit_width(limits::kMaxResidueBaseTerms)),
                  sizeof(Coefficients::value_type) + 4 * sizeof(void *) +
                      Key.size() * sizeof(uint32_t)))
            return Root;
          auto [It, Inserted] = Reduced.Terms.try_emplace(Key, Coeff);
          if (!Inserted)
            It->second += Coeff;
          if (It->second.isZero())
            Reduced.Terms.erase(It);
        }
      }
      if (Reduced.Terms.size() > limits::kMaxResidueBaseTerms)
        return Root;
      F = std::move(Reduced);
    }
  }
  return synthesizeResidueTable(Ctx, Root, Factors, Needed, Res, CompleteBase);
}

} // namespace neverd::symbolic::detail
