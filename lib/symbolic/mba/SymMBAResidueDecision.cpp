//===- SymMBAResidueDecision.cpp - Exact bounded low-word decisions -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Construct reduced ordered Boolean decisions for the observed low bits of
/// a scaled product. Every supported operation is evaluated exactly; other
/// words remain independent opaque inputs. Only a complete smaller word is
/// published, and all attempts spend the caller's cumulative resources.
///
//===----------------------------------------------------------------------===//

#include "Limits.h"
#include "SymMBADetail.h"

#include "neverd/symbolic/Limits.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Compiler.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace neverd::symbolic::detail {
namespace {

// A short prefix of a known parity input precedes the other words. Higher
// bits return to interleaved order, keeping mixed-word carries bounded.
constexpr uint64_t SecondaryWord = uint64_t(1) << 63;

struct Decision {
  uint64_t Variable;
  uint32_t Low;
  uint32_t High;
  bool operator==(const Decision &) const = default;
};

// Input identity, observed bit and ordering flag fit beside both bounded
// edges. Store this complete key, not only its hash: exact equality is kept
// while the unique table needs two machine words per bucket instead of three.
uint64_t decisionKey(const Decision &D) {
  constexpr unsigned EdgeBits =
      std::bit_width(2 * limits::kMaxResidueDecisionNodes - 1);
  constexpr unsigned BitBits =
      std::bit_width(limits::kMaxAffineResidueTableBits - 1);
  static_assert(32 + BitBits + 1 + 2 * EdgeBits < 64);
  const uint64_t Input =
      (D.Variable & ~SecondaryWord) | ((D.Variable >> 63) << (32 + BitBits));
  return (Input << (2 * EdgeBits)) | (uint64_t(D.Low) << EdgeBits) | D.High;
}

class LowWordDecisions {
  using Bits = llvm::SmallVector<uint32_t, 8>;
  using Polynomial = llvm::SmallVector<uint64_t, 8>;
  enum Operation : uint64_t { And, Or, Xor };
  static unsigned variableBit(uint64_t Variable) {
    return unsigned((Variable & ~SecondaryWord) >> 32);
  }

  SymContext &Ctx;
  WorkBudget &Budget;
  size_t &Storage;
  size_t MaxWork;
  bool *StorageLimited;
  size_t Work = 0;
  bool Failed = false;
  unsigned RootWidth = 0;
  bool PlainDecisionInputs = true;
  bool BalanceCarryColumns = false;
  std::optional<std::pair<uint64_t, bool>> Assumption;
  std::optional<uint32_t> PriorityInput;
  // The low edge bit denotes complement. Negation never duplicates a graph;
  // zero and one are the two edges of the single terminal node.
  std::vector<Decision> Nodes{{UINT64_MAX, 0, 0}};
  llvm::DenseMap<uint64_t, uint32_t> Unique;
  llvm::DenseMap<uint64_t, uint32_t> Applied;
  // Retain all three bounded edges in one exact key, including complements.
  // High bits remain unused, so no valid key aliases a dense-map sentinel.
  static uint64_t carryKey(uint32_t A, uint32_t B, uint32_t C) {
    constexpr unsigned EdgeBits =
        std::bit_width(2 * limits::kMaxResidueDecisionNodes - 1);
    static_assert(3 * EdgeBits < 64);
    return (uint64_t(A) << (2 * EdgeBits)) | (uint64_t(B) << EdgeBits) | C;
  }
  llvm::DenseMap<uint64_t, std::pair<uint32_t, uint32_t>> Adders;
  llvm::DenseMap<uint64_t, uint32_t> Parities;
  std::unordered_map<uint64_t, Bits> Words;
  std::unordered_map<uint32_t, unsigned> Zeros;
  struct WordOperation {
    SymOp Op;
    Bits A, B;
    bool operator==(const WordOperation &) const = default;
  };
  struct WordOperationHash {
    size_t operator()(const WordOperation &K) const {
      return size_t(llvm::hash_combine(
          K.Op, llvm::hash_combine_range(K.A.begin(), K.A.end()),
          llvm::hash_combine_range(K.B.begin(), K.B.end())));
    }
  };
  std::unordered_map<WordOperation, Bits, WordOperationHash> Arithmetic;

  bool charge(size_t Units = 1, size_t Bytes = 0) {
    if (Failed)
      return false;
    if (Bytes > Storage && StorageLimited)
      *StorageLimited = true;
    if (Units > MaxWork - Work || Bytes > Storage) {
      Failed = true;
      return false;
    }
    Work += Units;
    Storage -= Bytes;
    return true;
  }

  // This owner alone spends its captured allowance. Commit the exact admitted
  // work before another budget consumer runs and on every exit, including a
  // refused proof. Node operations need only update the local counter.
  void commitWork() {
    Budget.consume(Work);
    Work = 0;
  }

  // These maps never erase individual keys. Prepay the complete bucket
  // allocation at the dense map's 3/4 load boundary, including its initial
  // table. Existing per-entry charges still cover keys and node storage.
  template <typename Map> bool reserveEntry(Map &Cache) {
    const auto Buckets =
        Cache.getMemorySize() / sizeof(typename Map::value_type);
    if ((Cache.size() + 1) * 4 < Buckets * 3)
      return true;
    const auto Next =
        std::max(limits::kResidueDecisionMinMapBuckets, 2 * Buckets);
    if (!charge(0, Next * sizeof(typename Map::value_type)))
      return false;
    Cache.reserve(Cache.size() + 1);
    return true;
  }

  Decision decision(uint32_t Edge) const {
    auto D = Nodes[Edge >> 1];
    D.Low ^= Edge & 1;
    D.High ^= Edge & 1;
    return D;
  }

  std::optional<llvm::APInt> literal(SymRef R) {
    const size_t Words = (size_t(Ctx.width(R)) - 1) / 64 + 1;
    if (!charge(Words, sizeof(llvm::APInt) + Words * sizeof(uint64_t)))
      return std::nullopt;
    return Ctx.constValue(R);
  }

  bool isOne(SymRef R) {
    if (!Ctx.isConst(R))
      return false;
    if (Ctx.width(R) <= 64)
      return Ctx.node(R).Aux == 1;
    auto Value = literal(R);
    return Value && Value->isOne();
  }

  // Existing decisions need neither arena growth nor its saved registers.
  // Keep admission and allocation together after the complete identity miss.
  LLVM_ATTRIBUTE_NOINLINE
  uint32_t appendNode(Decision D, uint32_t Complement, uint64_t Key) {
    if (Nodes.size() >= limits::kMaxResidueDecisionNodes ||
        !charge(1, sizeof(Decision) + sizeof(Key) + sizeof(uint32_t) +
                       6 * sizeof(void *)) ||
        !reserveEntry(Unique)) {
      Failed = true;
      return 0;
    }
    const auto Id = uint32_t(Nodes.size() << 1);
    Nodes.push_back(D);
    Unique.try_emplace(Key, Id);
    return Id ^ Complement;
  }

  uint32_t node(uint64_t Variable, uint32_t Low, uint32_t High) {
    if (!charge() || Low == High)
      return Low;
    if (Assumption && Variable == Assumption->first)
      return Assumption->second ? High : Low;
    const uint32_t Complement = Low & 1;
    Low ^= Complement;
    High ^= Complement;
    const Decision D{Variable, Low, High};
    const auto Key = decisionKey(D);
    if (auto It = Unique.find(Key); It != Unique.end())
      return It->second ^ Complement;
    return appendNode(D, Complement, Key);
  }

  // Only a missing application needs a recursive frontier and cache insertion.
  // The caller has already normalized its edges and charged the same lookup.
  LLVM_ATTRIBUTE_NOINLINE
  uint32_t expandApplication(Operation Op, uint32_t A, uint32_t B,
                             unsigned Depth, uint64_t Key,
                             uint32_t Complement) {
    // Recursive insertion may reallocate Nodes; copy both decisions first.
    const auto Left = decision(A), Right = decision(B);
    const auto Variable = std::min(Left.Variable, Right.Variable);
    const auto Low =
        apply(Op, Left.Variable == Variable ? Left.Low : A,
              Right.Variable == Variable ? Right.Low : B, Depth + 1);
    const auto High =
        apply(Op, Left.Variable == Variable ? Left.High : A,
              Right.Variable == Variable ? Right.High : B, Depth + 1);
    const auto Result = node(Variable, Low, High);
    if (!charge(1, sizeof(Key) + sizeof(uint32_t) + 4 * sizeof(void *)) ||
        !reserveEntry(Applied))
      return 0;
    Applied.try_emplace(Key, Result);
    return Result ^ Complement;
  }

  uint32_t apply(Operation Op, uint32_t A, uint32_t B, unsigned Depth = 0) {
    if (!charge())
      return 0;
    uint32_t Complement = 0;
    if (Op == Or) {
      Op = And;
      A ^= 1;
      B ^= 1;
      Complement = 1;
    } else if (Op == Xor) {
      Complement = (A ^ B) & 1;
      A &= ~uint32_t(1);
      B &= ~uint32_t(1);
    }
    if (A > B)
      std::swap(A, B);
    if (Op == And) {
      if ((A ^ B) == 1)
        return Complement;
      if (!A || A == B)
        return A ^ Complement;
      if (A == 1)
        return B ^ Complement;
    } else {
      if (!A)
        return B ^ Complement;
      if (A == B)
        return Complement;
    }
    if (Depth >= limits::kMaxResidueDecisionDepth) {
      Failed = true;
      return 0;
    }
    // Edges use twice the bounded node index. Bit 31 of the low word is free
    // for AND/XOR. Unlike bit 63, it participates in the integer map's hash;
    // the same operands with different operations need not share a bucket.
    static_assert(limits::kMaxResidueDecisionNodes <= UINT32_MAX / 4);
    const uint64_t Key = (uint64_t(A) << 32) | (uint64_t(Op == Xor) << 31) | B;
    if (auto It = Applied.find(Key); It != Applied.end())
      return It->second ^ Complement;
    return expandApplication(Op, A, B, Depth, Key, Complement);
  }

  Bits calculate(SymOp Op, const Bits &A, const Bits &B) {
    if (!charge(1 + A.size()))
      return Bits(A.size(), 0);
    auto Zero = [](const Bits &Value) {
      return std::all_of(Value.begin(), Value.end(),
                         [](uint32_t V) { return !V; });
    };
    if (Zero(A))
      return Op == SymOp::Add ? B : A;
    if (Zero(B))
      return Op == SymOp::Add ? A : B;
    auto Zeros = [](const Bits &Value) {
      return size_t(std::find_if(Value.begin(), Value.end(),
                                 [](uint32_t V) { return V != 0; }) -
                    Value.begin());
    };
    const size_t ZA = Zeros(A), ZB = Zeros(B);
    const size_t Shift = Op == SymOp::Mul ? ZA + ZB : std::min(ZA, ZB);
    if (Shift) {
      Bits Result(A.size(), 0);
      if (Shift >= A.size())
        return Result;
      const size_t Count = A.size() - Shift;
      const size_t StartA = Op == SymOp::Mul ? ZA : Shift;
      const size_t StartB = Op == SymOp::Mul ? ZB : Shift;
      Bits Left(A.begin() + StartA, A.begin() + StartA + Count);
      Bits Right(B.begin() + StartB, B.begin() + StartB + Count);
      auto Narrow = calculate(Op, Left, Right);
      std::copy(Narrow.begin(), Narrow.end(), Result.begin() + Shift);
      return Result;
    }
    if (Op == SymOp::Add && A == B) {
      Bits Result(A.size(), 0);
      std::copy(A.begin(), A.end() - 1, Result.begin() + 1);
      return Result;
    }
    if (Op == SymOp::Mul) {
      auto One = [](const Bits &V) {
        return V.front() == 1 && std::all_of(V.begin() + 1, V.end(),
                                             [](uint32_t B) { return !B; });
      };
      if (One(A))
        return B;
      if (One(B))
        return A;
    }
    const WordOperation Key =
        A < B ? WordOperation{Op, A, B} : WordOperation{Op, B, A};
    if (auto It = Arithmetic.find(Key); It != Arithmetic.end())
      return It->second;
    Bits Result;
    if (Op == SymOp::Add) {
      Result = add(A, B);
    } else if (A == B) {
      Result = square(A);
    } else {
      size_t Common = 0;
      while (Common < A.size() && A[Common] == B[Common])
        ++Common;
      if (2 * Common >= A.size()) {
        // A=L+2^K*U and B=L+2^K*V. Their unobserved cross product
        // vanishes when 2K covers the observed word. Exact decision identity,
        // not sampled equality, establishes the shared low word L.
        Bits Low(A.size(), 0);
        std::copy(A.begin(), A.begin() + Common, Low.begin());
        Result = calculate(SymOp::Mul, Low, Low);
        Bits UpperA(A.begin() + Common, A.end());
        Bits UpperB(B.begin() + Common, B.end());
        Bits NarrowLow(A.begin(), A.begin() + A.size() - Common);
        auto Upper = calculate(SymOp::Mul, NarrowLow,
                               calculate(SymOp::Add, UpperA, UpperB));
        Bits Shifted(A.size(), 0);
        std::copy(Upper.begin(), Upper.end(), Shifted.begin() + Common);
        Result = calculate(SymOp::Add, Result, Shifted);
      } else {
        Result = multiply(A, B);
      }
    }
    if (!charge(1, sizeof(WordOperation) + sizeof(Bits) + 4 * sizeof(void *)))
      return Bits(A.size(), 0);
    Arithmetic.emplace(Key, Result);
    return Result;
  }

  // Recurse over the common decision frontier once for both output bits.
  // The sum and majority are both self-dual: complementing all three inputs
  // complements both results. One normalized cache entry therefore shares
  // their traversal without retaining an intermediate two-input XOR graph.
  std::pair<uint32_t, uint32_t> fullAdder(uint32_t A, uint32_t B, uint32_t C,
                                          unsigned Depth = 0) {
    if (!charge())
      return {};
    if (A == B)
      return {C, A};
    if (A == C)
      return {B, A};
    if (B == C)
      return {A, B};
    if ((A ^ B) == 1)
      return {C ^ 1, C};
    if ((A ^ C) == 1)
      return {B ^ 1, B};
    if ((B ^ C) == 1)
      return {A ^ 1, A};
    if (A > B)
      std::swap(A, B);
    if (B > C)
      std::swap(B, C);
    if (A > B)
      std::swap(A, B);
    if (A <= 1)
      return {apply(Xor, B, C) ^ A, apply(A ? Or : And, B, C)};
    const uint32_t Complement = A & 1;
    A ^= Complement;
    B ^= Complement;
    C ^= Complement;
    const auto Key = carryKey(A, B, C);
    if (auto It = Adders.find(Key); It != Adders.end())
      return {It->second.first ^ Complement, It->second.second ^ Complement};
    if (Depth >= limits::kMaxResidueDecisionDepth) {
      Failed = true;
      return {};
    }
    const auto DA = decision(A), DB = decision(B), DC = decision(C);
    const auto V = std::min({DA.Variable, DB.Variable, DC.Variable});
    const auto L =
        fullAdder(DA.Variable == V ? DA.Low : A, DB.Variable == V ? DB.Low : B,
                  DC.Variable == V ? DC.Low : C, Depth + 1);
    const auto H = fullAdder(DA.Variable == V ? DA.High : A,
                             DB.Variable == V ? DB.High : B,
                             DC.Variable == V ? DC.High : C, Depth + 1);
    const std::pair<uint32_t, uint32_t> R{node(V, L.first, H.first),
                                          node(V, L.second, H.second)};
    if (!charge(1, sizeof(Key) + sizeof(R) + 4 * sizeof(void *)) ||
        !reserveEntry(Adders))
      return {};
    Adders.try_emplace(Key, R);
    return {R.first ^ Complement, R.second ^ Complement};
  }

  // A final column needs only parity. Normalize each input complement into
  // one output flag and construct the three-way XOR directly.
  uint32_t parity(uint32_t A, uint32_t B, uint32_t C, unsigned Depth = 0) {
    if (!charge())
      return 0;
    const uint32_t Complement = (A ^ B ^ C) & 1;
    A &= ~uint32_t(1);
    B &= ~uint32_t(1);
    C &= ~uint32_t(1);
    if (A == B)
      return C ^ Complement;
    if (A == C)
      return B ^ Complement;
    if (B == C)
      return A ^ Complement;
    if (A > B)
      std::swap(A, B);
    if (B > C)
      std::swap(B, C);
    if (A > B)
      std::swap(A, B);
    if (!A)
      return apply(Xor, B, C) ^ Complement;
    const auto Key = carryKey(A, B, C);
    if (auto It = Parities.find(Key); It != Parities.end())
      return It->second ^ Complement;
    if (Depth >= limits::kMaxResidueDecisionDepth) {
      Failed = true;
      return 0;
    }
    const auto DA = decision(A), DB = decision(B), DC = decision(C);
    const auto V = std::min({DA.Variable, DB.Variable, DC.Variable});
    const auto L =
        parity(DA.Variable == V ? DA.Low : A, DB.Variable == V ? DB.Low : B,
               DC.Variable == V ? DC.Low : C, Depth + 1);
    const auto H =
        parity(DA.Variable == V ? DA.High : A, DB.Variable == V ? DB.High : B,
               DC.Variable == V ? DC.High : C, Depth + 1);
    const auto R = node(V, L, H);
    if (!charge(1, sizeof(Key) + sizeof(uint32_t) + 4 * sizeof(void *)) ||
        !reserveEntry(Parities))
      return 0;
    Parities.try_emplace(Key, R);
    return R ^ Complement;
  }

  Bits add(const Bits &A, const Bits &B) {
    Bits Result;
    uint32_t Carry = 0;
    for (size_t I = 0; I < A.size(); ++I) {
      if (I + 1 == A.size())
        Result.push_back(parity(A[I], B[I], Carry));
      else {
        const auto [Sum, Next] = fullAdder(A[I], B[I], Carry);
        Result.push_back(Sum);
        Carry = Next;
      }
    }
    return Result;
  }

  Bits square(const Bits &A) {
    // For an odd word A=1+2U, A*A=1+4*(U*U+U). Only N-2 bits
    // of U survive, so avoid constructing the unobserved upper carries.
    if (A.size() > 2 && A.front() == 1) {
      if (!charge(A.size(), 2 * sizeof(Bits)))
        return Bits(A.size(), 0);
      Bits U(A.begin() + 1, A.end() - 1);
      const auto Correction =
          calculate(SymOp::Add, U, calculate(SymOp::Mul, U, U));
      Bits Result(A.size(), 0);
      Result.front() = 1;
      std::copy(Correction.begin(), Correction.end(), Result.begin() + 2);
      return Result;
    }
    Bits Result(A.size(), 0);
    for (size_t I = 0; I < A.size() && !Failed; ++I) {
      Bits Row(A.size(), 0);
      if (2 * I < A.size())
        Row[2 * I] = A[I];
      for (size_t J = I + 1; I + J + 1 < A.size(); ++J)
        Row[I + J + 1] = apply(And, A[I], A[J]);
      Result = calculate(SymOp::Add, Result, Row);
    }
    return Result;
  }

  Bits multiply(const Bits &A, const Bits &B) {
    Bits Result(A.size(), 0);
    for (size_t I = 0; I < B.size() && !Failed; ++I) {
      if (!B[I])
        continue;
      Bits Row(A.size(), 0);
      for (size_t J = 0; I + J < A.size(); ++J)
        Row[I + J] = apply(And, A[J], B[I]);
      Result = calculate(SymOp::Add, Result, Row);
    }
    return Result;
  }

  std::optional<unsigned> lookupWidth(SymRef R) {
    if (Ctx.op(R) != SymOp::LShr || !Ctx.isConst(Ctx.operand(R, 0)))
      return std::nullopt;
    auto Index = Ctx.operand(R, 1);
    if (Ctx.op(Index) == SymOp::ZExt)
      Index = Ctx.operand(Index, 0);
    if (Ctx.op(Index) != SymOp::And || !Ctx.isConst(Ctx.operand(Index, 0)))
      return std::nullopt;
    const auto Mask = literal(Ctx.operand(Index, 0));
    if (!Mask)
      return std::nullopt;
    const unsigned Bits = Mask->getActiveBits();
    return Bits && Bits <= limits::kMaxAffineResidueTableBits
               ? std::optional<unsigned>(Bits)
               : std::nullopt;
  }

  bool transparent(SymRef R, unsigned Count) {
    const auto Op = Ctx.op(R);
    if (Op == SymOp::Add || Op == SymOp::Mul || Op == SymOp::And ||
        Op == SymOp::Or || Op == SymOp::Xor || Op == SymOp::Not ||
        Op == SymOp::ZExt || Op == SymOp::SExt)
      return true;
    if (Op == SymOp::Extract)
      return Ctx.node(R).Aux <= limits::kMaxAffineResidueTableBits - Count;
    if (Op == SymOp::Shl)
      return Ctx.isConst(Ctx.operand(R, 1));
    if (Op != SymOp::LShr)
      return false;
    if (Ctx.isConst(Ctx.operand(R, 1))) {
      const auto Value = literal(Ctx.operand(R, 1));
      if (!Value)
        return false;
      const auto Shift = Value->getLimitedValue();
      return Shift >= Ctx.width(R) ||
             Shift <= limits::kMaxAffineResidueTableBits - Count;
    }
    return lookupWidth(R).has_value();
  }

  unsigned knownZeros(SymRef R, unsigned Depth = 0) {
    if (!charge() || Depth >= limits::kMaxResidueDecisionDepth)
      return 0;
    const auto Op = Ctx.op(R);
    // These exact leaves require neither a cached fact nor an APInt reading.
    // Wide constants retain their accounted limb traversal below.
    if (Op == SymOp::Var)
      return 0;
    if (Op == SymOp::Const &&
        Ctx.width(R) <= std::numeric_limits<uint64_t>::digits)
      return std::min({Ctx.width(R), limits::kMaxAffineResidueTableBits,
                       unsigned(std::countr_zero(Ctx.node(R).Aux))});
    if (auto It = Zeros.find(R.index()); It != Zeros.end())
      return It->second;
    const unsigned Ceiling =
        std::min(Ctx.width(R), limits::kMaxAffineResidueTableBits);
    unsigned Result = 0;
    if (Ctx.isConst(R)) {
      const auto Value = literal(R);
      if (!Value)
        return 0;
      Result = std::min(Ceiling, Value->countr_zero());
    } else if (Op == SymOp::ZExt || Op == SymOp::SExt) {
      Result = knownZeros(Ctx.operand(R, 0), Depth + 1);
    } else if (Op == SymOp::Shl && Ctx.isConst(Ctx.operand(R, 1))) {
      const auto Shift = literal(Ctx.operand(R, 1));
      if (!Shift)
        return 0;
      Result = std::min<uint64_t>(Ceiling,
                                  Shift->getLimitedValue(Ceiling) +
                                      knownZeros(Ctx.operand(R, 0), Depth + 1));
    } else if (Op == SymOp::Add || Op == SymOp::Or || Op == SymOp::Xor) {
      Result = Ceiling;
      for (auto Child : Ctx.operands(R)) {
        Result = std::min(Result, knownZeros(Child, Depth + 1));
        if (!Result)
          break;
      }
    } else if (Op == SymOp::Mul || Op == SymOp::And) {
      for (auto Child : Ctx.operands(R)) {
        const auto ChildZeros = knownZeros(Child, Depth + 1);
        Result = Op == SymOp::Mul ? std::min(Ceiling, Result + ChildZeros)
                                  : std::max(Result, ChildZeros);
        if (Result == Ceiling)
          break;
      }
    }
    if (charge(1, 2 * sizeof(uint32_t) + 4 * sizeof(void *)))
      Zeros.emplace(R.index(), Result);
    return Result;
  }

  // These representatives preserve only Count low bits. They are internal to
  // a scaled product proof; the original full-width expression is never
  // replaced by one of them. Normalize ring and bitwise operations recursively
  // so high constants do not give identical low words distinct outer nodes.
  SymRef projectLowWord(SymRef R, unsigned Count,
                        std::unordered_map<uint64_t, SymRef> &Cache,
                        unsigned Depth = 0) {
    if (!charge() || Depth >= limits::kMaxResidueDecisionDepth) {
      Failed = true;
      return R;
    }
    const uint64_t Key = (uint64_t(R.index()) << 8) | Count;
    if (auto It = Cache.find(Key); It != Cache.end())
      return It->second;
    const auto Op = Ctx.op(R);
    auto Result = R;
    if (Ctx.isConst(R)) {
      const auto Value = literal(R);
      if (!Value)
        return R;
      const auto Low = Value->getLoBits(Count).getZExtValue();
      if (*Value != Low) {
        const size_t Limbs = (size_t(Ctx.width(R)) - 1) / 64 + 1;
        if (!charge(Limbs, sizeof(SymNode) + Limbs * sizeof(uint64_t)))
          return R;
        Result = Ctx.mkConst(Ctx.width(R), Low);
      }
    } else if (Op == SymOp::Add || Op == SymOp::Mul || Op == SymOp::And ||
               Op == SymOp::Or || Op == SymOp::Xor || Op == SymOp::Not) {
      const size_t N = Ctx.numOperands(R);
      if (!charge(N, N * sizeof(SymRef)))
        return R;
      llvm::SmallVector<SymRef, 8> Operands(Ctx.operands(R));
      // A product does not observe bits above Count minus the other factors'
      // proved zeros. Normalize each operand at that exact precision, so
      // repeated source offsets share the complete projected product too.
      unsigned TotalZeros = 0;
      if (Op == SymOp::Mul) {
        for (auto Child : Operands)
          TotalZeros = std::min(Count, TotalZeros + knownZeros(Child));
        if (Failed)
          return R;
        if (TotalZeros == Count) {
          const size_t Limbs = (size_t(Ctx.width(R)) - 1) / 64 + 1;
          if (!charge(Limbs, sizeof(SymNode) + Limbs * sizeof(uint64_t) +
                                 sizeof(uint64_t) + sizeof(SymRef) +
                                 4 * sizeof(void *)))
            return R;
          Result = Ctx.mkZero(Ctx.width(R));
          Cache.emplace(Key, Result);
          return Result;
        }
      }
      bool Changed = false;
      size_t Expanded = 0, Out = 0;
      for (auto Child : Operands) {
        const unsigned Precision =
            Op == SymOp::Mul ? Count - TotalZeros + knownZeros(Child) : Count;
        if (Ctx.isConst(Child) && Op != SymOp::Not) {
          const auto Value = literal(Child);
          if (!Value)
            return R;
          const auto Low = Value->getLoBits(Precision).getZExtValue();
          // Remove neutral residues before allocating their projected nodes
          // and reconstructing an operation that would return its sole child.
          if (((Op == SymOp::Add || Op == SymOp::Or || Op == SymOp::Xor) &&
               Low == 0) ||
              (Op == SymOp::Mul && Low == 1) ||
              (Op == SymOp::And && Low == (uint64_t(1) << Count) - 1)) {
            Changed = true;
            continue;
          }
        }
        const auto Projected =
            projectLowWord(Child, Precision, Cache, Depth + 1);
        if (Failed)
          return R;
        Changed |= Child != Projected;
        Operands[Out++] = Projected;
        Expanded += Ctx.op(Projected) == Op ? Ctx.numOperands(Projected) : 1;
      }
      Operands.resize(Out);
      if (Changed && Operands.size() == 1 && Op != SymOp::Not) {
        Result = Operands.front();
      } else if (Changed) {
        // Construction can flatten associative children and fold wide
        // constants. Charge that complete input before changing the arena.
        const size_t Limbs = (size_t(Ctx.width(R)) - 1) / 64 + 1;
        if (!charge((1 + Expanded) * Limbs, 2 * sizeof(SymNode) +
                                                4 * Expanded * sizeof(SymRef) +
                                                2 * Limbs * sizeof(uint64_t)))
          return R;
        Result = Op == SymOp::Add   ? Ctx.mkAdd(Operands)
                 : Op == SymOp::Mul ? Ctx.mkMul(Operands)
                 : Op == SymOp::And ? Ctx.mkAnd(Operands)
                 : Op == SymOp::Or  ? Ctx.mkOr(Operands)
                 : Op == SymOp::Xor ? Ctx.mkXor(Operands)
                                    : Ctx.mkNot(Operands.front());
      }
    }
    if (charge(1, sizeof(uint64_t) + sizeof(SymRef) + 4 * sizeof(void *)))
      Cache.emplace(Key, Result);
    return Result;
  }

  // References into unordered_map elements survive recursive insertions.
  // Cached words are immutable until the parity assumption changes; no view
  // escapes that phase. Cache hits pay for lookup, without copying each bit.
  const Bits &word(SymRef R, unsigned Count, unsigned Depth = 0) {
    static const auto EmptyWords = [] {
      std::array<Bits, limits::kMaxAffineResidueTableBits + 1> Result;
      for (unsigned I = 0; I < Result.size(); ++I)
        Result[I].resize(I, 0);
      return Result;
    }();
    const auto &Empty = EmptyWords[Count];
    if (!charge())
      return Empty;
    // This cache describes only Count low bits. Canonicalize native constant
    // operands in that quotient before interning its key, so different source
    // offsets/masks reuse the same low-word view. The original expression and
    // every wider cache entry retain their complete constant. Native constants
    // are inline; wide constants keep the existing APInt accounting below.
    const auto InitialOp = Ctx.op(R);
    if ((InitialOp == SymOp::Add || InitialOp == SymOp::And ||
         InitialOp == SymOp::Or || InitialOp == SymOp::Xor) &&
        Ctx.numOperands(R) == 2 &&
        Ctx.width(R) <= std::numeric_limits<uint64_t>::digits &&
        Ctx.isConst(Ctx.operand(R, 0))) {
      const auto Constant = Ctx.operand(R, 0), Other = Ctx.operand(R, 1);
      const uint64_t Value = Ctx.node(Constant).Aux;
      const uint64_t Low = Value & ((uint64_t(1) << Count) - 1);
      if (Low != Value) {
        if (!charge(2, 2 * (sizeof(SymNode) + 4 * sizeof(SymRef))))
          return Empty;
        const auto K = Ctx.mkConst(Ctx.width(R), Low);
        R = InitialOp == SymOp::Add   ? Ctx.mkAdd(K, Other)
            : InitialOp == SymOp::And ? Ctx.mkAnd(K, Other)
            : InitialOp == SymOp::Or  ? Ctx.mkOr(K, Other)
                                      : Ctx.mkXor(K, Other);
      }
    }
    const uint64_t Key = (uint64_t(R.index()) << 8) | Count;
    if (auto It = Words.find(Key); It != Words.end())
      return It->second;
    if (!charge(Count) || Depth >= limits::kMaxResidueDecisionDepth) {
      Failed = true;
      return Empty;
    }
    const auto Op = Ctx.op(R);
    Bits Result(Count, 0);
    if (Ctx.isConst(R)) {
      const auto Value = literal(R);
      if (!Value)
        return Empty;
      for (unsigned I = 0; I < Count; ++I)
        Result[I] = (*Value)[I];
    } else if (Op == SymOp::ZExt || Op == SymOp::SExt) {
      const auto Child = Ctx.operand(R, 0);
      const auto ChildBits = std::min(Count, Ctx.width(Child));
      const auto &Value = word(Child, ChildBits, Depth + 1);
      std::copy(Value.begin(), Value.end(), Result.begin());
      if (Op == SymOp::SExt)
        std::fill(Result.begin() + ChildBits, Result.end(), Value.back());
    } else if (Op == SymOp::Extract && transparent(R, Count)) {
      const auto Offset = unsigned(Ctx.node(R).Aux);
      const auto &Value = word(Ctx.operand(R, 0), Count + Offset, Depth + 1);
      std::copy(Value.begin() + Offset, Value.end(), Result.begin());
    } else if (Op == SymOp::Not) {
      Result = word(Ctx.operand(R, 0), Count, Depth + 1);
      for (auto &Bit : Result)
        Bit = apply(Xor, Bit, 1);
    } else if (Op == SymOp::Shl && Ctx.isConst(Ctx.operand(R, 1))) {
      const auto Value = literal(Ctx.operand(R, 1));
      if (!Value)
        return Empty;
      const auto Shift = Value->getLimitedValue(Count);
      if (Shift < Count) {
        const auto &Value = word(Ctx.operand(R, 0), Count - Shift, Depth + 1);
        std::copy(Value.begin(), Value.end(), Result.begin() + Shift);
      }
    } else if (Op == SymOp::LShr && transparent(R, Count)) {
      if (Ctx.isConst(Ctx.operand(R, 1))) {
        const auto Value = literal(Ctx.operand(R, 1));
        if (!Value)
          return Empty;
        const auto Shift = Value->getLimitedValue();
        if (Shift < Ctx.width(R)) {
          const unsigned Take = std::min<uint64_t>(Count, Ctx.width(R) - Shift);
          const auto &Value = word(Ctx.operand(R, 0), Take + Shift, Depth + 1);
          std::copy(Value.begin() + Shift, Value.end(), Result.begin());
        }
      } else {
        const auto Bounded = lookupWidth(R);
        if (!Bounded)
          return Empty;
        const unsigned IndexBits = *Bounded;
        const size_t Entries = size_t(1) << IndexBits;
        if (!charge(Count * Entries, Entries * sizeof(uint32_t)))
          return Empty;
        const auto &Index = word(Ctx.operand(R, 1), IndexBits, Depth + 1);
        const auto Pattern = literal(Ctx.operand(R, 0));
        if (!Pattern)
          return Empty;
        for (unsigned I = 0; I < Count && !Failed; ++I) {
          std::vector<uint32_t> Values(Entries);
          for (size_t J = 0; J < Entries; ++J)
            Values[J] = I + J < Pattern->getBitWidth() ? (*Pattern)[I + J] : 0;
          size_t Size = Entries;
          for (unsigned Bit = 0; Bit < IndexBits; ++Bit) {
            for (size_t J = 0; J < Size / 2; ++J) {
              const auto Low = Values[2 * J], High = Values[2 * J + 1];
              Values[J] = apply(Xor, Low,
                                apply(And, Index[Bit], apply(Xor, Low, High)));
            }
            Size /= 2;
          }
          Result[I] = Values.front();
        }
      }
    } else if ((Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor) &&
               Ctx.numOperands(R) == 2 && Ctx.isConst(Ctx.operand(R, 0))) {
      const auto Literal = literal(Ctx.operand(R, 0));
      if (!Literal)
        return Empty;
      const unsigned Needed =
          Op == SymOp::And ? std::min(Count, Literal->getActiveBits()) : Count;
      if (Needed) {
        const auto &Value = word(Ctx.operand(R, 1), Needed, Depth + 1);
        // Constant bitwise operands select or complement existing edges;
        // they need neither a constant decision word nor an apply-cache entry.
        for (unsigned I = 0; I < Count; ++I) {
          const bool Bit = (*Literal)[I];
          Result[I] = Op == SymOp::And  ? (Bit ? Value[I] : 0)
                      : Op == SymOp::Or ? (Bit ? 1 : Value[I])
                                        : (Value[I] ^ uint32_t(Bit));
        }
      }
    } else if (Op == SymOp::Add && Ctx.numOperands(R) == 2 &&
               Ctx.isConst(Ctx.operand(R, 0))) {
      const auto Literal = literal(Ctx.operand(R, 0));
      if (!Literal)
        return Empty;
      const auto &Value = word(Ctx.operand(R, 1), Count, Depth + 1);
      // A constant carry is AND or OR, rather than a three-input majority.
      // Avoid constructing a constant word and caching a generic word sum.
      uint32_t Carry = 0;
      for (unsigned I = 0; I < Count && !Failed; ++I) {
        Result[I] = apply(Xor, Value[I], Carry ^ uint32_t((*Literal)[I]));
        if (I + 1 < Count)
          Carry = apply((*Literal)[I] ? Or : And, Value[I], Carry);
      }
    } else if (Op == SymOp::Add || Op == SymOp::Mul || Op == SymOp::And ||
               Op == SymOp::Or || Op == SymOp::Xor) {
      if (Op == SymOp::Mul)
        Result.front() = 1;
      if (Op == SymOp::And)
        std::fill(Result.begin(), Result.end(), 1);
      // Emission and recursive interning must not invalidate operand views.
      llvm::SmallVector<SymRef, 16> Operands(Ctx.operands(R));
      unsigned TotalZeros = 0;
      llvm::SmallVector<unsigned, 16> FactorZeros;
      if (Op == SymOp::Mul && Count > 1) {
        for (auto Child : Operands) {
          const auto Z = std::min(Count, knownZeros(Child));
          FactorZeros.push_back(Z);
          TotalZeros = std::min(Count, TotalZeros + Z);
        }
        if (Failed)
          return Empty;
        if (TotalZeros == Count)
          return Empty;
      }
      if (TotalZeros) {
        Bits Narrow(Count - TotalZeros, 0);
        Narrow.front() = 1;
        for (size_t I = 0; I < Operands.size(); ++I) {
          const auto Z = FactorZeros[I];
          const auto &Value =
              word(Operands[I], Count - TotalZeros + Z, Depth + 1);
          Narrow = calculate(SymOp::Mul, Narrow,
                             Bits(Value.begin() + Z, Value.end()));
        }
        std::copy(Narrow.begin(), Narrow.end(), Result.begin() + TotalZeros);
        Result.front() = 0;
      } else {
        for (auto Child : Operands) {
          const auto &Value = word(Child, Count, Depth + 1);
          if (Op == SymOp::Add)
            Result = calculate(SymOp::Add, Result, Value);
          else if (Op == SymOp::Mul)
            Result = calculate(SymOp::Mul, Result, Value);
          else
            for (unsigned I = 0; I < Count; ++I)
              Result[I] = apply(Op == SymOp::And  ? And
                                : Op == SymOp::Or ? Or
                                                  : Xor,
                                Result[I], Value[I]);
          if (Failed)
            return Empty;
          if ((Op == SymOp::Mul || Op == SymOp::And) &&
              std::all_of(Result.begin(), Result.end(),
                          [](uint32_t Bit) { return Bit == 0; }))
            break;
          if (Op == SymOp::Or &&
              std::all_of(Result.begin(), Result.end(),
                          [](uint32_t Bit) { return Bit == 1; }))
            break;
        }
      }
    } else {
      // Treat an unsupported word as an exact opaque input, without assuming
      // a relation between it and any other input word or path condition.
      // Record the inputs actually used by the decisions. Transparency can
      // depend on the requested bit count, so the earlier admission walk is
      // insufficient to establish this property for nested shifts/extracts.
      PlainDecisionInputs &=
          Ctx.isVar(R) && Ctx.width(R) == RootWidth && Count <= Ctx.width(R);
      for (unsigned I = 0; I < Count; ++I)
        Result[I] =
            node(((PriorityInput && (R.index() != *PriorityInput ||
                                     I >= limits::kResidueDecisionPriorityBits))
                      ? SecondaryWord
                      : 0) |
                     (uint64_t(I) << 32) | R.index(),
                 0, 1);
    }
    if (!charge(Count, sizeof(uint64_t) + sizeof(Bits) +
                           2 * Count * sizeof(uint32_t) + 4 * sizeof(void *)))
      return Empty;
    return Words.emplace(Key, std::move(Result)).first->second;
  }

  // Count equal and complementary bits before carry propagation. A pair of
  // complements contributes one, while repeated copies move to higher columns.
  Bits sumWords(llvm::ArrayRef<Bits> Values, unsigned Count,
                llvm::ArrayRef<size_t> Multiplicities = {}) {
    using Column = std::map<uint32_t, size_t>;
    if (!charge(Count, Count * sizeof(Column)))
      return Bits(Count, 0);
    std::vector<Column> Columns(Count);
    auto Insert = [&](unsigned Bit, uint32_t Edge, size_t Copies = 1) {
      if (!charge())
        return false;
      if (!Edge)
        return true;
      auto &C = Columns[Bit];
      if (!charge(1 + std::bit_width(C.size())))
        return false;
      auto It = C.find(Edge);
      if (It == C.end()) {
        if (!charge(1, sizeof(Column::value_type) + 4 * sizeof(void *)))
          return false;
        It = C.emplace(Edge, 0).first;
      }
      It->second += Copies;
      return true;
    };
    for (size_t V = 0; V < Values.size(); ++V)
      for (unsigned I = 0; I < Count; ++I)
        if (!Insert(I, Values[V][I],
                    Multiplicities.empty() ? 1 : Multiplicities[V]))
          return Bits(Count, 0);
    Bits Left(Count, 0), Right(Count, 0);
    for (unsigned I = 0; I < Count && !Failed; ++I) {
      auto &C = Columns[I];
      if (!charge(C.size(), 2 * C.size() * sizeof(uint32_t)))
        break;
      Bits Remaining;
      Remaining.reserve(C.size());
      size_t Constants = 0;
      for (auto It = C.begin(); It != C.end(); ++It) {
        auto [Id, Copies] = *It;
        if (!charge(1 + std::bit_width(Copies)))
          break;
        if (!Copies)
          continue;
        if (Id == 1) {
          Constants += Copies;
          continue;
        }
        if (!(Id & 1)) {
          auto Other = C.find(Id ^ 1);
          if (Other != C.end()) {
            const auto Common = std::min(Copies, Other->second);
            Copies -= Common;
            Other->second -= Common;
            Constants += Common;
          }
        }
        if (Copies & 1)
          Remaining.push_back(Id);
        Copies >>= 1;
        for (unsigned J = I + 1; Copies && J < Count; ++J, Copies >>= 1)
          if ((Copies & 1) && !Insert(J, Id))
            break;
      }
      if (Constants & 1)
        Remaining.push_back(1);
      Constants >>= 1;
      for (unsigned J = I + 1; Constants && J < Count; ++J, Constants >>= 1)
        if ((Constants & 1) && !Insert(J, 1))
          break;
      while (Remaining.size() > 2 && !Failed) {
        // Nonlinear factors keep the established streaming schedule: their
        // mixed carries can grow under regrouping. Affine/bitwise factors
        // use balanced columns to avoid long intermediate prefix graphs.
        if (!BalanceCarryColumns) {
          const auto A = Remaining.pop_back_val();
          const auto B = Remaining.pop_back_val();
          const auto C = Remaining.pop_back_val();
          const auto [Sum, Carry] =
              I + 1 < Count ? fullAdder(A, B, C)
                            : std::make_pair(parity(A, B, C), uint32_t(0));
          if (Failed)
            return Bits(Count, 0);
          if (Sum)
            Remaining.push_back(Sum);
          if (I + 1 < Count && !Insert(I + 1, Carry))
            return Bits(Count, 0);
          continue;
        }
        // Reduce independent triples before revisiting their sums. Reusing
        // each new sum immediately builds a long prefix graph; balanced
        // columns retain fewer intermediate carry/parity decisions.
        size_t Next = 0, J = 0;
        for (; J + 2 < Remaining.size(); J += 3) {
          const auto A = Remaining[J], B = Remaining[J + 1],
                     C = Remaining[J + 2];
          const auto [Sum, Carry] =
              I + 1 < Count ? fullAdder(A, B, C)
                            : std::make_pair(parity(A, B, C), uint32_t(0));
          if (Failed)
            return Bits(Count, 0);
          if (Sum)
            Remaining[Next++] = Sum;
          if (I + 1 < Count && !Insert(I + 1, Carry))
            return Bits(Count, 0);
        }
        for (; J < Remaining.size(); ++J)
          Remaining[Next++] = Remaining[J];
        Remaining.resize(Next);
      }
      if (!Remaining.empty())
        Left[I] = Remaining[0];
      if (Remaining.size() > 1)
        Right[I] = Remaining[1];
    }
    return calculate(SymOp::Add, Left, Right);
  }

  Bits power(Bits Value, size_t Exponent) {
    const size_t Width = Value.size();
    if (Exponent >= Width) {
      // Words have at most kMaxAffineResidueTableBits bits. Odd residues have
      // period 2^(Width-2), with periods one and two for widths one and two.
      // Even residues vanish only once the exponent reaches Width: retain
      // that lower bound instead of reducing the exponent modulo the period.
      const size_t Period = residueUnitPeriod(Width);
      const size_t Residue = Exponent % Period;
      if (!Residue) {
        if (!charge(1 + Width))
          return Bits(Width, 0);
        std::fill(Value.begin() + 1, Value.end(), 0);
        return Value;
      }
      if (Residue == 1) {
        // On odd inputs this is the original word; on even inputs it is
        // zero. Gate every bit rather than treating all inputs as units.
        if (!charge(1 + Width))
          return Bits(Width, 0);
        for (size_t I = 1; I < Width && !Failed; ++I)
          Value[I] = apply(And, Value.front(), Value[I]);
        return Value;
      }
      const size_t Reduced =
          Value.front() == 1 ? Residue : Width + (Exponent - Width) % Period;
      if (Reduced < Exponent) {
        if (!charge())
          return Bits(Width, 0);
        Exponent = Reduced;
      }
    }
    Bits Term(Value.size(), 0);
    Term.front() = 1;
    for (; Exponent && !Failed; Exponent >>= 1) {
      if (Exponent & 1)
        Term = calculate(SymOp::Mul, Term, Value);
      if (Exponent > 1)
        Value = calculate(SymOp::Mul, Value, Value);
    }
    return Term;
  }

  Bits quadraticLowSum(llvm::ArrayRef<Bits> Values, unsigned Count) {
    // The caller requests only one or two observed bits of e2.
    // For H_i=a_i+2*b_i, let K=sum(a_i). Modulo four,
    // e2(H)=choose(K,2)+2*((sum a_i)*(sum b_i)-sum(a_i*b_i)).
    // choose(K,2) has bits K_1 and K_2 XOR (K_0 AND K_1).
    // This retains the exact division by two without constructing any squares.
    if (!charge(Values.size() * (Count + 2), Values.size() * sizeof(Bits)))
      return {};
    std::vector<Bits> Ones;
    Ones.reserve(Values.size());
    for (const auto &V : Values) {
      Bits Row(Count + 1, 0);
      Row[0] = V[0];
      Ones.push_back(std::move(Row));
    }
    const auto K = sumWords(Ones, Count + 1);
    if (Failed)
      return {};
    Bits Result{K[1]};
    if (Count == 2) {
      uint32_t HighParity = 0, MixedParity = 0;
      for (const auto &V : Values) {
        if (K[0])
          HighParity = apply(Xor, HighParity, V[1]);
        MixedParity = apply(Xor, MixedParity, apply(And, V[0], V[1]));
      }
      Result.push_back(
          apply(Xor, apply(Xor, K[2], apply(And, K[0], K[1])),
                apply(Xor, MixedParity, apply(And, K[0], HighParity))));
    }
    return Result;
  }

  LLVM_ATTRIBUTE_NOINLINE
  llvm::SmallVector<unsigned, 16>
  countPolynomialFrequencies(const Polynomial &P, unsigned NumVars) {
    llvm::SmallVector<unsigned, 16> Counts;
    if (!charge(NumVars, NumVars * sizeof(unsigned)))
      return {};
    Counts.resize(NumVars, 0);
    if (P.size() > 4 * NumVars) {
      // Count all variable incidences together. Each bit position is
      // an independent counter; carry-save columns combine three masks
      // without a per-variable scan of every monomial.
      constexpr unsigned Levels =
          std::bit_width(limits::kMaxResidueDecisionPolynomialTerms);
      if (!charge(Levels, Levels * (2 * sizeof(uint64_t) + 1)))
        return {};
      std::array<std::array<uint64_t, 2>, Levels> Columns{};
      std::array<unsigned char, Levels> Occupied{};
      for (auto Term : P) {
        for (unsigned Level = 0; Term; ++Level) {
          if (!charge())
            return {};
          auto &Column = Columns[Level];
          auto &Count = Occupied[Level];
          if (Count < 2) {
            Column[Count++] = Term;
            break;
          }
          const uint64_t Different = Column[0] ^ Column[1];
          const uint64_t Carry = (Column[0] & Column[1]) | (Different & Term);
          Column[0] = Different ^ Term;
          Count = 1;
          Term = Carry;
        }
      }
      for (unsigned Level = 0; Level < Levels; ++Level)
        for (unsigned I = 0; I < Occupied[Level]; ++I) {
          auto Term = Columns[Level][I];
          while (Term) {
            if (!charge())
              return {};
            Counts[std::countr_zero(Term)] += 1u << Level;
            Term &= Term - 1;
          }
        }
    } else {
      for (auto Term : P)
        while (Term) {
          if (!charge())
            return {};
          ++Counts[std::countr_zero(Term)];
          Term &= Term - 1;
        }
    }
    return Counts;
  }

  Bits product(llvm::ArrayRef<SymRef> Factors, unsigned Observed,
               bool Coalesce = false) {
    if (Factors.empty()) {
      Bits One(Observed, 0);
      One.front() = 1;
      return One;
    }
    if (Factors.size() == 1)
      return word(Factors.front(), Observed);
    // Write every group as (L + 2^Split H_i). Since 3*Split >= Observed,
    // only the constant, linear and quadratic elementary sums survive.
    // Group equality comes from complete decision words, never from samples.
    const unsigned Split = (Observed + 2) / 3;
    struct ProductGroup {
      std::vector<Bits> Values;
      std::vector<size_t> Multiplicities;
      size_t Count = 0;
    };
    std::map<Bits, ProductGroup> Groups;
    std::unordered_map<uint32_t, size_t> Copies;
    const bool Weighted =
        Coalesce || Factors.size() > limits::kMaxResidueProductFactors;
    if (Weighted) {
      for (auto Factor : Factors) {
        if (!charge())
          return {};
        auto It = Copies.find(Factor.index());
        if (It == Copies.end()) {
          if (!charge(1,
                      sizeof(uint32_t) + sizeof(size_t) + 4 * sizeof(void *)))
            return {};
          It = Copies.emplace(Factor.index(), 0).first;
        }
        ++It->second;
      }
    }
    for (auto Factor : Factors) {
      if (Ctx.isConst(Factor))
        continue;
      size_t Multiplicity = 1;
      if (Weighted) {
        if (!charge())
          return {};
        Multiplicity = std::exchange(Copies[Factor.index()], 0);
        if (!Multiplicity)
          continue;
      }
      const auto &Value = word(Factor, Observed);
      if (Failed ||
          !charge(Observed * (1 + std::bit_width(Groups.size())),
                  sizeof(decltype(Groups)::value_type) + 4 * sizeof(void *) +
                      (2 * Observed * sizeof(uint32_t) + 2 * sizeof(Bits))))
        return Bits{};
      Bits Low(Value.begin(), Value.begin() + Split);
      auto &Group = Groups[Low];
      Group.Count += Multiplicity;
      Group.Values.emplace_back(Value.begin() + Split, Value.end());
      if (Weighted) {
        if (!charge(1, 2 * sizeof(size_t)))
          return {};
        Group.Multiplicities.push_back(Multiplicity);
      }
    }
    std::vector<Bits> Products;
    if (!charge(Groups.size(), Groups.size() * sizeof(Bits)))
      return Bits{};
    for (const auto &[Low, Entry] : Groups) {
      const auto &Group = Entry.Values;
      const auto Count = Entry.Count;
      const auto High = sumWords(Group, Observed - Split, Entry.Multiplicities);
      if (Failed)
        return Bits{};
      auto Wide = Low;
      Wide.resize(Observed, 0);
      auto Term = power(Wide, Count);
      if (Observed != Split) {
        Bits Narrow(Wide.begin(), Wide.begin() + Observed - Split);
        auto Correction = calculate(SymOp::Mul, power(Narrow, Count - 1), High);
        Bits Upper(Observed, 0);
        std::copy(Correction.begin(), Correction.end(), Upper.begin() + Split);
        Term = calculate(SymOp::Add, Term, Upper);
      }
      if (2 * Split < Observed && Count > 1) {
        // 2*e2(H) = (sum H)^2 - sum(H^2). Compute one additional bit
        // before exact division by two; two is not invertible in this ring.
        const unsigned N = Observed - 2 * Split + 1;
        Bits Elementary;
        if (N <= 3 && !Weighted) {
          Elementary = quadraticLowSum(Group, N - 1);
        } else {
          if (!charge(Group.size(), Group.size() * sizeof(Bits)))
            return Bits{};
          std::vector<Bits> Squares;
          Squares.reserve(Group.size());
          for (const auto &H : Group) {
            Bits V(H.begin(), H.begin() + N);
            Squares.push_back(calculate(SymOp::Mul, V, V));
          }
          auto SumSquares = sumWords(Squares, N, Entry.Multiplicities);
          auto Sum = Bits(High.begin(), High.begin() + N);
          auto SumSquare = calculate(SymOp::Mul, Sum, Sum);
          for (auto &Bit : SumSquares)
            Bit = apply(Xor, Bit, 1);
          Bits One(N, 0);
          One[0] = 1;
          auto Difference = calculate(SymOp::Add, SumSquare,
                                      calculate(SymOp::Add, SumSquares, One));
          if (Failed || Difference.front() != 0)
            return Bits{};
          Elementary.assign(Difference.begin() + 1, Difference.end());
        }
        if (Failed || Elementary.empty())
          return Bits{};
        Bits Narrow(Low.begin(), Low.begin() + N - 1);
        auto Correction =
            calculate(SymOp::Mul, power(Narrow, Count - 2), Elementary);
        Bits Upper(Observed, 0);
        std::copy(Correction.begin(), Correction.end(),
                  Upper.begin() + 2 * Split);
        Term = calculate(SymOp::Add, Term, Upper);
      }
      Products.push_back(std::move(Term));
    }
    // Pair equal low prefixes first. Balanced products avoid constructing
    // transient decisions for high carries that a nearby factor cancels.
    while (Products.size() > 1 && !Failed) {
      if (!charge(Products.size() * Observed *
                  (1 + std::bit_width(Products.size()))))
        return Bits{};
      std::sort(Products.begin(), Products.end());
      size_t Out = 0;
      for (size_t I = 0; I < Products.size(); I += 2)
        Products[Out++] =
            I + 1 == Products.size()
                ? Products[I]
                : calculate(SymOp::Mul, Products[I], Products[I + 1]);
      Products.resize(Out);
    }
    if (Failed || Products.empty()) {
      return Bits{};
    }
    return std::move(Products.front());
  }

  // Equal identities denote the same word except for the highest observed
  // bit. Flip is an exact Boolean decision for that bit's difference from
  // the shared representative; it need not be a constant.
  struct ResidueSignature {
    uint32_t Identity = 0;
    uint32_t Flip = 0;
  };
  using SignatureKey = llvm::SmallVector<uint64_t, 8>;
  struct SignatureHash {
    size_t operator()(const SignatureKey &Key) const {
      return size_t(llvm::hash_combine_range(Key.begin(), Key.end()));
    }
  };
  struct WideSignature {
    uint32_t Identity = 0;
    std::array<uint32_t, 2> Delta{};
    // Identity probes cannot supply a correction until the entry is complete.
    bool Complete = true;
  };
  struct WidePairingState {
    std::unordered_map<SignatureKey, uint32_t, SignatureHash> Ids;
    std::vector<const SignatureKey *> Keys;
    std::unordered_map<uint64_t, WideSignature> Words;
    unsigned NarrowPrecision = 0;
  };
  // Ordinary low-word searches do not need this state. Construct it only
  // after pairing admission, and keep it within the active odd assumption.
  struct PairingState {
    std::unordered_map<SignatureKey, uint32_t, SignatureHash> Ids;
    std::vector<const SignatureKey *> Keys;
    std::unordered_map<uint64_t, ResidueSignature> Words;
    std::unique_ptr<WidePairingState> Wide;
  };

  template <typename SignatureState>
  uint32_t internSignature(SignatureState &State, SignatureKey Key) {
    if (!charge(1 + Key.size()))
      return 0;
    if (auto It = State.Ids.find(Key); It != State.Ids.end())
      return It->second;
    if (!charge(1, sizeof(SignatureKey) + sizeof(uint32_t) +
                       Key.size() * sizeof(uint64_t) + 6 * sizeof(void *)))
      return 0;
    if (State.Keys.size() == State.Keys.capacity()) {
      const size_t Capacity = std::max(size_t(1), 2 * State.Keys.capacity());
      if (!charge(1, Capacity * sizeof(const SignatureKey *)))
        return 0;
      State.Keys.reserve(Capacity);
    }
    const auto Identity = uint32_t(State.Ids.size() + 1);
    const auto It = State.Ids.emplace(std::move(Key), Identity).first;
    // Unordered-map rehash preserves references to these immutable keys.
    State.Keys.push_back(&It->first);
    return Identity;
  }

  template <typename SignatureState>
  bool appendSignatureXor(SignatureState &State, SignatureKey &Children,
                          uint64_t &Literal, unsigned Count, uint32_t Child) {
    // The enclosing operand visit is already charged. Only expansion adds
    // entries and sorting work. A shifted child can expose a formerly nested
    // XOR; flatten its exact representative, keeping its correction separate.
    const auto &Key = *State.Keys[Child - 1];
    if (Key[1] == Count && !Key[0]) {
      Literal ^= Key[2];
    } else if (Key[1] == Count && Key[0] == uint64_t(SymOp::Xor) + 1) {
      const size_t Size = Key.size() - 3;
      if (!charge(Size * (1 + std::bit_width(Children.size() + Size)),
                  2 * Size * sizeof(uint64_t)))
        return false;
      Literal ^= Key[2];
      Children.append(Key.begin() + 3, Key.end());
    } else {
      Children.push_back(Child);
    }
    return true;
  }

  // Recursive low-word construction can grow the context operand pool.
  // Reacquire operands by stable node/index instead of retaining its spans.
  ResidueSignature residueSignature(PairingState &State, SymRef R,
                                    unsigned Count, unsigned Depth = 0) {
    if (!charge() || Depth >= limits::kMaxResidueDecisionDepth) {
      Failed = true;
      return {};
    }
    const uint64_t Key = (uint64_t(R.index()) << 8) | Count;
    if (auto It = State.Words.find(Key); It != State.Words.end())
      return It->second;
    const auto Op = Ctx.op(R);
    const uint64_t Top = uint64_t(1) << (Count - 1);
    ResidueSignature Result;
    if (Ctx.isConst(R)) {
      auto Value = literal(R);
      if (!Value)
        return {};
      const auto Low = Value->getLoBits(Count).getZExtValue();
      Result = {internSignature(State, {0, Count, Low & (Top - 1)}),
                uint32_t(bool(Low & Top))};
    } else if (Op == SymOp::Add || Op == SymOp::Xor) {
      const size_t OperandCount = Ctx.numOperands(R);
      if (!charge(OperandCount * (1 + std::bit_width(OperandCount)),
                  2 * (OperandCount + 3) * sizeof(uint64_t)))
        return {};
      uint64_t Literal = 0;
      uint32_t Flip = 0;
      SignatureKey Children;
      Children.reserve(OperandCount + 3);
      for (size_t I = 0; I < OperandCount; ++I) {
        const auto Child = Ctx.operand(R, I);
        if (Ctx.isConst(Child)) {
          auto Value = literal(Child);
          if (!Value)
            return {};
          const auto Low = Value->getLoBits(Count).getZExtValue();
          if (Op == SymOp::Add)
            Literal += Low;
          else
            Literal ^= Low;
        } else {
          const auto Value = residueSignature(State, Child, Count, Depth + 1);
          if (Failed)
            return {};
          Flip = apply(Xor, Flip, Value.Flip);
          if (Op == SymOp::Xor) {
            if (!appendSignatureXor(State, Children, Literal, Count,
                                    Value.Identity))
              return {};
          } else {
            Children.push_back(Value.Identity);
          }
        }
      }
      Flip = apply(Xor, Flip, bool(Literal & Top));
      Literal &= Top - 1;
      std::sort(Children.begin(), Children.end());
      if (Op == SymOp::Xor) {
        size_t Out = 0;
        for (size_t I = 0; I < Children.size();) {
          size_t End = I + 1;
          while (End < Children.size() && Children[End] == Children[I])
            ++End;
          if ((End - I) & 1)
            Children[Out++] = Children[I];
          I = End;
        }
        Children.resize(Out);
      }
      if (Children.empty())
        Result = {internSignature(State, {0, Count, Literal}), Flip};
      else if (Children.size() == 1 && !Literal)
        Result = {uint32_t(Children.front()), Flip};
      else {
        Children.insert(Children.begin(), {uint64_t(Op) + 1, Count, Literal});
        Result = {internSignature(State, std::move(Children)), Flip};
      }
    } else if (Op == SymOp::And && Ctx.numOperands(R) == 2 &&
               Ctx.isConst(Ctx.operand(R, 0))) {
      auto Value = literal(Ctx.operand(R, 0));
      if (!Value)
        return {};
      const auto Mask = Value->getLoBits(Count).getZExtValue();
      if (!Mask)
        Result = {internSignature(State, {0, Count, 0}), 0};
      else {
        const auto Child =
            residueSignature(State, Ctx.operand(R, 1), Count, Depth + 1);
        Result = Mask == 2 * Top - 1
                     ? Child
                     : ResidueSignature{
                           internSignature(State, {uint64_t(Op) + 1, Count,
                                                   Mask, Child.Identity}),
                           Mask & Top ? Child.Flip : 0};
      }
    } else if (Op == SymOp::Mul) {
      const size_t OperandCount = Ctx.numOperands(R);
      if (!charge(OperandCount * (1 + std::bit_width(OperandCount)),
                  2 * (OperandCount + 3) * sizeof(uint64_t) +
                      2 * (4 * OperandCount + 1) * sizeof(uint32_t)))
        return {};
      unsigned TotalZeros = 0;
      llvm::SmallVector<unsigned, 16> Zeros;
      for (size_t I = 0; I < OperandCount; ++I) {
        const auto Child = Ctx.operand(R, I);
        const auto Zero = std::min(Count, knownZeros(Child));
        Zeros.push_back(Zero);
        TotalZeros = std::min(Count, TotalZeros + Zero);
      }
      if (TotalZeros == Count)
        Result = {internSignature(State, {0, Count, 0}), 0};
      else if (Count - TotalZeros >= 2) {
        SignatureKey Children;
        Children.reserve(OperandCount + 2);
        Bits Flips;
        for (size_t I = 0; I < OperandCount; ++I) {
          const auto Value =
              residueSignature(State, Ctx.operand(R, I),
                               Count - TotalZeros + Zeros[I], Depth + 1);
          Children.push_back(Value.Identity);
          Flips.push_back(Value.Flip);
        }
        uint32_t Flip = 0;
        if (std::any_of(Flips.begin(), Flips.end(),
                        [](uint32_t V) { return V != 0; })) {
          // Remove only proven zeros. The remaining precision is at least
          // two bits, so products of two highest-bit disturbances vanish.
          // Prefix and suffix parities retain every linear contribution,
          // including even normalized operands, without division.
          Bits Parities, Prefix(OperandCount + 1, 1);
          for (size_t I = 0; I < OperandCount; ++I) {
            Parities.push_back(word(Ctx.operand(R, I), Zeros[I] + 1)[Zeros[I]]);
            Prefix[I + 1] = apply(And, Prefix[I], Parities.back());
          }
          uint32_t Suffix = 1;
          for (size_t I = OperandCount; I-- != 0;) {
            if (Flips[I])
              Flip = apply(Xor, Flip,
                           apply(And, Flips[I], apply(And, Prefix[I], Suffix)));
            Suffix = apply(And, Suffix, Parities[I]);
          }
        }
        std::sort(Children.begin(), Children.end());
        Children.insert(Children.begin(), {uint64_t(Op) + 1, Count});
        Result = {internSignature(State, std::move(Children)), Flip};
      } else {
        // A one-bit normalized product can retain cross terms. Keep its
        // exact original identity instead of using the linear formula.
        Result = {internSignature(State, {UINT64_MAX, Count, R.index()}), 0};
      }
    } else {
      // Equality of opaque representatives is structural, never sampled.
      Result = {internSignature(State, {UINT64_MAX, Count, R.index()}), 0};
    }
    if (!charge(1, sizeof(uint64_t) + sizeof(ResidueSignature) +
                       4 * sizeof(void *)))
      return {};
    State.Words.emplace(Key, Result);
    return Result;
  }

  uint32_t highestBit(SymRef R, unsigned Count, unsigned Depth = 0) {
    if (!charge() || Depth >= limits::kMaxResidueDecisionDepth) {
      Failed = true;
      return 0;
    }
    const uint64_t Key = (uint64_t(R.index()) << 8) | Count;
    if (auto It = Words.find(Key); It != Words.end())
      return It->second.back();
    if (Ctx.numOperands(R) == 2 && Ctx.isConst(Ctx.operand(R, 0))) {
      if (Ctx.op(R) == SymOp::And || Ctx.op(R) == SymOp::Add) {
        const auto Literal = literal(Ctx.operand(R, 0));
        if (!Literal)
          return 0;
        const auto Other = Ctx.operand(R, 1);
        if (Ctx.op(R) == SymOp::And)
          return (*Literal)[Count - 1] ? highestBit(Other, Count, Depth + 1)
                                       : 0;
        // A constant addend only needs the carry into the requested bit.
        // Do not construct all lower sum bits when no caller observes them.
        const auto &Value = word(Other, Count, Depth + 1);
        uint32_t Carry = 0;
        for (unsigned I = 0; I + 1 < Count; ++I)
          Carry = apply((*Literal)[I] ? Or : And, Value[I], Carry);
        return apply(Xor, Value.back(),
                     Carry ^ uint32_t((*Literal)[Count - 1]));
      }
    }
    return word(R, Count, Depth + 1).back();
  }

  Bits difference(const Bits &A, const Bits &B) {
    if (!charge())
      return Bits(2, 0);
    if (A == B)
      return {0, 0};
    if (!B[0] && !B[1])
      return A;
    if (A[0] == B[0])
      return {0, apply(Xor, A[1], B[1])};
    return {apply(Xor, A[0], B[0]),
            parity(A[1], B[1], apply(And, A[0] ^ 1, B[0]))};
  }

  Bits sumCorrection(const Bits &A, const Bits &B) {
    if (!charge())
      return Bits(2, 0);
    if (!A[0] && !A[1])
      return B;
    if (!B[0] && !B[1])
      return A;
    if (A == B)
      return {0, A[0]};
    if (A[0] == B[0])
      return {0, parity(A[1], B[1], A[0])};
    return {apply(Xor, A[0], B[0]), parity(A[1], B[1], apply(And, A[0], B[0]))};
  }

  Bits multiplyCorrection(const Bits &A, const Bits &B) {
    if (!charge())
      return Bits(2, 0);
    if ((!A[0] && !A[1]) || (!B[0] && !B[1]))
      return {0, 0};
    if (A[0] == 1 && !A[1])
      return B;
    if (B[0] == 1 && !B[1])
      return A;
    if (!A[0])
      return {0, apply(And, A[1], B[0])};
    if (!B[0])
      return {0, apply(And, A[0], B[1])};
    if (A[0] == 1 && B[0] == 1)
      return {1, apply(Xor, A[1], B[1])};
    return {apply(And, A[0], B[0]),
            apply(Xor, apply(And, A[0], B[1]), apply(And, A[1], B[0]))};
  }

  // Equal identities share a full-width representative. Delta is its exact
  // additive correction in the two highest observed bits. Highest input bits
  // cancel when deriving an XOR correction; only the next bit is needed.
  // Full-block admission needs only identities; defer Boolean corrections
  // until pairing consumes them. Partial blocks keep their complete probes.
  template <bool IdentityOnly = false>
  WideSignature wideSignature(WidePairingState &State, SymRef R, unsigned Count,
                              unsigned Depth = 0) {
    if (!charge() || Depth >= limits::kMaxResidueDecisionDepth) {
      Failed = true;
      return {};
    }
    const uint64_t Key = (uint64_t(R.index()) << 8) | Count;
    if (auto It = State.Words.find(Key);
        It != State.Words.end() && (IdentityOnly || It->second.Complete))
      return It->second;
    const uint64_t Unit = uint64_t(1) << (Count - 2);
    const auto Op = Ctx.op(R);
    WideSignature Result;
    auto Delta = [](const WideSignature &S) {
      return Bits(S.Delta.begin(), S.Delta.end());
    };
    auto Save = [&](uint32_t Identity, const Bits &D) {
      Result = {Identity, {D[0], D[1]}};
    };
    if (Ctx.isConst(R)) {
      auto Value = literal(R);
      if (!Value)
        return {};
      const auto Low = Value->getLoBits(Count).getZExtValue();
      Result = {internSignature(State, {0, Count, Low & (Unit - 1)}),
                {uint32_t(bool(Low & Unit)), uint32_t(bool(Low & (2 * Unit)))}};
    } else if (Op == SymOp::Add || Op == SymOp::Xor) {
      const size_t OperandCount = Ctx.numOperands(R);
      const auto Nonconstant = OperandCount - Ctx.isConst(Ctx.operand(R, 0));
      if (!charge(OperandCount + Nonconstant * std::bit_width(Nonconstant),
                  2 * (OperandCount + 3) * sizeof(uint64_t) +
                      2 * OperandCount * sizeof(WideSignature)))
        return {};
      SignatureKey Children;
      llvm::SmallVector<WideSignature, 8> Values;
      uint64_t Literal = 0;
      for (size_t I = 0; I < OperandCount; ++I) {
        const auto Child = Ctx.operand(R, I);
        if (Ctx.isConst(Child)) {
          auto Value = literal(Child);
          if (!Value)
            return {};
          const auto Low = Value->getLoBits(Count).getZExtValue();
          Literal = Op == SymOp::Add ? Literal + Low : Literal ^ Low;
          Values.push_back({});
        } else {
          auto Value =
              wideSignature<IdentityOnly>(State, Child, Count, Depth + 1);
          if (Failed)
            return {};
          if (Op == SymOp::Xor) {
            if (!appendSignatureXor(State, Children, Literal, Count,
                                    Value.Identity))
              return {};
          } else {
            Children.push_back(Value.Identity);
          }
          Values.push_back(Value);
        }
      }
      Bits D{uint32_t(bool(Literal & Unit)),
             uint32_t(bool(Literal & (2 * Unit)))};
      if (Op == SymOp::Add) {
        if (!IdentityOnly)
          for (const auto &Value : Values)
            if (Value.Delta[0] || Value.Delta[1])
              D = sumCorrection(D, Delta(Value));
      } else if (!IdentityOnly) {
        // For XOR, the additive correction's upper bit contains only cross
        // terms d_i * a_j (i != j). A word's own low correction cancels its
        // own bit, so do not build that word merely to subtract it again.
        for (const auto &Value : Values) {
          if (Value.Delta[0])
            D[0] = apply(Xor, D[0], Value.Delta[0]);
          if (Value.Delta[1])
            D[1] = apply(Xor, D[1], Value.Delta[1]);
        }
        for (size_t I = 0; I < OperandCount; ++I) {
          if (Ctx.isConst(Ctx.operand(R, I)))
            continue;
          const auto Coefficient =
              Values[I].Delta[0] ? apply(Xor, D[0], Values[I].Delta[0]) : D[0];
          if (Coefficient)
            D[1] = apply(Xor, D[1],
                         apply(And, Coefficient,
                               highestBit(Ctx.operand(R, I), Count - 1)));
        }
        if (Literal & Unit)
          D[1] = apply(Xor, D[1], D[0] ^ 1);
      }
      Literal &= Unit - 1;
      std::sort(Children.begin(), Children.end());
      if (Op == SymOp::Xor) {
        size_t Out = 0;
        for (size_t I = 0; I < Children.size();) {
          size_t End = I + 1;
          while (End < Children.size() && Children[End] == Children[I])
            ++End;
          if ((End - I) & 1)
            Children[Out++] = Children[I];
          I = End;
        }
        Children.resize(Out);
      }
      if (Children.empty())
        Save(internSignature(State, {0, Count, Literal}), D);
      else if (Children.size() == 1 && !Literal)
        Save(uint32_t(Children.front()), D);
      else {
        Children.insert(Children.begin(), {uint64_t(Op) + 1, Count, Literal});
        Save(internSignature(State, std::move(Children)), D);
      }
    } else if (Op == SymOp::And && Ctx.numOperands(R) == 2 &&
               Ctx.isConst(Ctx.operand(R, 0))) {
      auto Value = literal(Ctx.operand(R, 0));
      if (!Value)
        return {};
      const auto Mask = Value->getLoBits(Count).getZExtValue();
      if (!Mask)
        Result = {internSignature(State, {0, Count, 0}), {0, 0}};
      else {
        const auto Child = Ctx.operand(R, 1);
        auto S = wideSignature<IdentityOnly>(State, Child, Count, Depth + 1);
        if (Mask == 4 * Unit - 1)
          Result = S;
        else {
          const bool LowMask = Mask & Unit, HighMask = Mask & (2 * Unit);
          Bits D{LowMask ? S.Delta[0] : 0, HighMask ? S.Delta[1] : 0};
          if (!IdentityOnly && LowMask != HighMask && S.Delta[0])
            D[1] =
                apply(Xor, D[1],
                      apply(And, highestBit(Child, Count - 1) ^ 1, S.Delta[0]));
          Save(internSignature(State,
                               {uint64_t(Op) + 1, Count, Mask, S.Identity}),
               D);
        }
      }
    } else if (Op == SymOp::Mul) {
      const size_t OperandCount = Ctx.numOperands(R);
      if (!charge(OperandCount * (1 + std::bit_width(OperandCount)),
                  2 * (OperandCount + 3) * sizeof(uint64_t) +
                      2 * (4 * OperandCount + 1) * sizeof(Bits)))
        return {};
      unsigned TotalZeros = 0;
      llvm::SmallVector<unsigned, 16> Zeros;
      for (size_t I = 0; I < OperandCount; ++I) {
        const auto Child = Ctx.operand(R, I);
        const auto Z = std::min(Count, knownZeros(Child));
        Zeros.push_back(Z);
        TotalZeros = std::min(Count, TotalZeros + Z);
      }
      if (TotalZeros == Count)
        Result = {internSignature(State, {0, Count, 0}), {0, 0}};
      else if (Count - TotalZeros >= 4) {
        SignatureKey Children;
        llvm::SmallVector<Bits, 16> Deltas, Low, Prefix;
        bool Changed = false;
        for (size_t I = 0; I < OperandCount; ++I) {
          auto S = wideSignature<IdentityOnly>(State, Ctx.operand(R, I),
                                               Count - TotalZeros + Zeros[I],
                                               Depth + 1);
          Children.push_back(S.Identity);
          Deltas.push_back(Delta(S));
          Changed |= S.Delta[0] || S.Delta[1];
        }
        Bits D(2, 0);
        if (!IdentityOnly && Changed) {
          Prefix.push_back({1, 0});
          for (size_t I = 0; I < OperandCount; ++I) {
            const auto &V = word(Ctx.operand(R, I), Zeros[I] + 2);
            Low.push_back({V[Zeros[I]], V[Zeros[I] + 1]});
            Prefix.push_back(multiplyCorrection(Prefix.back(), Low.back()));
          }
          Bits Suffix{1, 0};
          for (size_t I = OperandCount; I-- != 0;) {
            D = sumCorrection(
                D, multiplyCorrection(Deltas[I],
                                      multiplyCorrection(Prefix[I], Suffix)));
            Suffix = multiplyCorrection(Suffix, Low[I]);
          }
        }
        std::sort(Children.begin(), Children.end());
        Children.insert(Children.begin(), {uint64_t(Op) + 1, Count});
        Save(internSignature(State, std::move(Children)), D);
      } else if (Count == State.NarrowPrecision) {
        // Fewer than four effective product bits can retain cross terms
        // between the corrections. Compute these bits exactly instead of
        // assuming linear corrections or hiding the whole product as an
        // opaque identity. Equal lower decisions share a zero-extended
        // representative; the two actual upper bits are its additive delta.
        const auto &Value = word(R, IdentityOnly ? Count - 2 : Count);
        if (Failed || !charge(Count, 2 * Count * sizeof(uint64_t)))
          return {};
        SignatureKey Identity{UINT64_MAX - 1, Count};
        uint64_t Literal = 0;
        bool Constant = true;
        for (unsigned I = 0; I < Count - 2; ++I) {
          Identity.push_back(Value[I]);
          Constant &= Value[I] <= 1;
          Literal |= uint64_t(Value[I] & 1) << I;
        }
        Bits D(2, 0);
        if constexpr (!IdentityOnly)
          D = {Value[Count - 2], Value[Count - 1]};
        Save(Constant ? internSignature(State, {0, Count, Literal})
                      : internSignature(State, std::move(Identity)),
             D);
      } else
        Result = {internSignature(State, {UINT64_MAX, Count, R.index()}),
                  {0, 0}};
    } else
      Result = {internSignature(State, {UINT64_MAX, Count, R.index()}), {0, 0}};
    Result.Complete = !IdentityOnly;
    if (auto It = State.Words.find(Key); It != State.Words.end()) {
      if (!charge())
        return {};
      It->second = Result;
    } else {
      if (!charge(1, sizeof(uint64_t) + sizeof(WideSignature) +
                         4 * sizeof(void *)))
        return {};
      State.Words.emplace(Key, Result);
    }
    return Result;
  }

  // With U=2^(N-2), matched odd factors satisfy B=A+U*d. Thus
  // AB=A^2+U*A*d. For N>=4, U^2 vanishes and every odd square is 1 mod 4:
  // all paired corrections add modulo 4, multiplied only by unpaired factors.
  // The product of representatives can be squared from N-1 observed bits.
  template <bool BatchCorrections>
  Bits widePairedProduct(PairingState &State, llvm::ArrayRef<SymRef> Factors,
                         unsigned Count,
                         llvm::SmallVectorImpl<SymRef> *Retained = nullptr) {
    if (Count < 4)
      return product(Factors, Count);
    if (!State.Wide) {
      if (!charge(1, sizeof(WidePairingState)))
        return {};
      State.Wide = std::make_unique<WidePairingState>();
    }
    if (!charge(Factors.size(), 2 * Factors.size() * sizeof(SymRef)))
      return {};
    std::map<uint32_t, std::pair<SymRef, WideSignature>> Unpaired;
    llvm::SmallVector<SymRef, 16> Pairs, Rest;
    Bits Correction(2, 0);
    llvm::SmallVector<Bits, 16> Corrections;
    unsigned LiteralCorrection = 0;
    if constexpr (BatchCorrections) {
      if (!charge(1, (Factors.size() / 2) * sizeof(Bits)))
        return {};
      Corrections.reserve(Factors.size() / 2);
    }
    for (auto Factor : Factors) {
      auto S = wideSignature(*State.Wide, Factor, Count);
      if (Failed || !charge(1 + std::bit_width(Unpaired.size())))
        return {};
      auto It = Unpaired.find(S.Identity);
      if (It == Unpaired.end()) {
        if (!charge(1, sizeof(decltype(Unpaired)::value_type) +
                           4 * sizeof(void *)))
          return {};
        Unpaired.emplace(S.Identity, std::make_pair(Factor, S));
      } else {
        auto A = It->second.first;
        const auto &D = It->second.second.Delta;
        auto Change = difference(Bits(S.Delta.begin(), S.Delta.end()),
                                 Bits(D.begin(), D.end()));
        // Both factors were proved odd. Their low word is only needed for
        // the upper cross term when the correction's low bit is nonzero.
        if (Change[0])
          Change[1] =
              apply(Xor, Change[1], apply(And, Change[0], word(A, 2)[1]));
        if constexpr (BatchCorrections) {
          if (!charge())
            return {};
          if (Change[0] <= 1 && Change[1] <= 1)
            LiteralCorrection =
                (LiteralCorrection + Change[0] + 2 * Change[1]) & 3;
          else
            Corrections.push_back(std::move(Change));
        } else {
          Correction = sumCorrection(Correction, Change);
        }
        Pairs.push_back(A);
        Unpaired.erase(It);
      }
    }
    if (Pairs.empty())
      return product(Factors, Count);
    if constexpr (BatchCorrections) {
      // Combine all two-bit corrections before creating carry decisions.
      // Equal/complementary columns cancel or carry exactly in sumWords.
      if (LiteralCorrection) {
        if (!charge())
          return {};
        Corrections.push_back({LiteralCorrection & 1, LiteralCorrection >> 1});
      }
      Correction = Corrections.empty()       ? Bits(2, 0)
                   : Corrections.size() == 1 ? Corrections.front()
                   : Corrections.size() == 2
                       ? sumCorrection(Corrections[0], Corrections[1])
                       : sumWords(Corrections, 2);
    }
    auto Inner = widePairedProduct<BatchCorrections>(State, Pairs, Count - 1);
    if (Failed || Inner.empty())
      return {};
    Inner.push_back(0);
    auto Result = calculate(SymOp::Mul, Inner, Inner);
    for (const auto &[Identity, Value] : Unpaired)
      Rest.push_back(Value.first);
    if (Retained) {
      if (!charge(Rest.size(), Rest.size() * sizeof(SymRef)))
        return {};
      Retained->append(Rest.begin(), Rest.end());
      Rest.clear();
    }
    auto Other = product(Rest, Count);
    if (Failed || Other.empty())
      return {};
    Result = calculate(SymOp::Mul, Result, Other);
    Correction =
        multiplyCorrection(Correction, Bits(Other.begin(), Other.begin() + 2));
    Bits Upper(Count, 0);
    std::copy(Correction.begin(), Correction.end(), Upper.end() - 2);
    return calculate(SymOp::Add, Result, Upper);
  }

  // The caller has proved every factor odd in the active assumption. For
  // A = L + 2^(N-1)*a and B = L + 2^(N-1)*b, AB is L^2 plus
  // 2^(N-1)*(a XOR b). The other odd factors preserve that correction.
  // Pull all squares into one product and recurse at N-1 bits. No original
  // highest bit is needed for its square, and recursion strictly decreases N.
  Bits pairedProduct(PairingState &State, llvm::ArrayRef<SymRef> Factors,
                     unsigned Count) {
    if (Count <= 2 || Factors.size() < 2)
      return product(Factors, Count);
    if (!charge(Factors.size(), 2 * Factors.size() * sizeof(SymRef)))
      return {};
    std::map<uint32_t, std::pair<SymRef, uint32_t>> Unpaired;
    llvm::SmallVector<SymRef, 16> Pairs, Rest;
    uint32_t Correction = 0;
    for (auto Factor : Factors) {
      const auto Signature = residueSignature(State, Factor, Count);
      if (Failed || !charge(1 + std::bit_width(Unpaired.size())))
        return {};
      auto It = Unpaired.find(Signature.Identity);
      if (It == Unpaired.end()) {
        if (!charge(1, sizeof(decltype(Unpaired)::value_type) +
                           4 * sizeof(void *)))
          return {};
        Unpaired.emplace(Signature.Identity,
                         std::make_pair(Factor, Signature.Flip));
        continue;
      }
      Pairs.push_back(Factor);
      Correction =
          apply(Xor, Correction, apply(Xor, It->second.second, Signature.Flip));
      Unpaired.erase(It);
    }
    if (Pairs.empty())
      return product(Factors, Count);
    auto Inner = pairedProduct(State, Pairs, Count - 1);
    if (Failed || Inner.empty())
      return {};
    Inner.push_back(0);
    auto Result = calculate(SymOp::Mul, Inner, Inner);
    for (const auto &[Identity, Value] : Unpaired)
      Rest.push_back(Value.first);
    auto Other = product(Rest, Count);
    if (Failed || Other.empty())
      return {};
    Result = calculate(SymOp::Mul, Result, Other);
    Result.back() = apply(Xor, Result.back(), Correction);
    return Result;
  }

public:
  LowWordDecisions(SymContext &Ctx, WorkBudget &Budget, size_t &Storage,
                   size_t MaxWork, bool *StorageLimited)
      : Ctx(Ctx), Budget(Budget), Storage(Storage),
        MaxWork(std::min(
            {MaxWork, limits::kMaxResidueDecisionWork, Budget.available()})),
        StorageLimited(StorageLimited) {}

  ~LowWordDecisions() { commitWork(); }

  SymRef simplify(SymRef Root, const llvm::APInt &Scale, unsigned Observed,
                  bool OddProductOnly, bool *Complete) {
    const auto Width = Ctx.width(Root);
    RootWidth = Width;
    const bool ProjectFactors =
        Ctx.numOperands(Root) > limits::kMaxResidueProductFactors;
    // Once a product spans an entire observed residue domain, repeated
    // low-word views can outnumber the complete domain. Normalize their
    // complete trees once so outer expressions share the same cached proof.
    const bool NormalizeFactors =
        ProjectFactors || Ctx.numOperands(Root) > (size_t(1) << Observed);
    llvm::SmallVector<SymRef, 16> Factors;
    if (NormalizeFactors) {
      // Fund both retained operand vectors before projecting. Domain admission
      // needs only the exact observed views, not a second traversal of high
      // subtrees whose values are already proved invisible to this product.
      if (!charge(Ctx.numOperands(Root),
                  2 * Ctx.numOperands(Root) * sizeof(SymRef)))
        return Root;
      Factors.assign(Ctx.operands(Root).begin(), Ctx.operands(Root).end());
      std::unordered_map<uint64_t, SymRef> Projections;
      for (auto &Factor : Factors) {
        if (Ctx.isConst(Factor))
          continue;
        const auto Projected = projectLowWord(Factor, Observed, Projections);
        if (Failed)
          return Root;
        // Only the original leading scalar belongs to Scale. A constant
        // residue of another factor must still be multiplied, so retain its
        // source word instead of introducing a second scalar in this view.
        if (!Ctx.isConst(Projected))
          Factor = Projected;
      }
    }
    // Refuse broad domains before constructing decisions. This is only a
    // search admission rule; opaque words still retain all their possible bits.
    std::unordered_set<uint32_t> Seen, Inputs;
    bool HasNonlinearFactor = false;
    llvm::SmallVector<SymRef, 16> Pending;
    if (NormalizeFactors)
      Pending.append(Factors.begin(), Factors.end());
    else
      Pending.push_back(Root);
    while (!Pending.empty()) {
      const auto R = Pending.pop_back_val();
      if (!charge(1, sizeof(uint32_t) + 6 * sizeof(void *)))
        return Root;
      if (!Seen.insert(R.index()).second || Ctx.isConst(R))
        continue;
      HasNonlinearFactor |=
          R != Root && Ctx.op(R) == SymOp::Mul &&
          Ctx.numOperands(R) - unsigned(Ctx.isConst(Ctx.operand(R, 0))) > 1;
      const bool Transparent = transparent(R, std::min(Observed, Ctx.width(R)));
      if (!Transparent) {
        if (Inputs.insert(R.index()).second &&
            Inputs.size() > limits::kMaxResidueDecisionInputs)
          return Root;
        continue;
      }
      if (!charge(Ctx.numOperands(R), Ctx.numOperands(R) * sizeof(SymRef)))
        return Root;
      Pending.append(Ctx.operands(R).begin(), Ctx.operands(R).end());
    }
    BalanceCarryColumns = !HasNonlinearFactor;
    if (!NormalizeFactors)
      Factors.assign(Ctx.operands(Root).begin(), Ctx.operands(Root).end());
    Bits Product(Observed, 0);
    Product.front() = 1;
    llvm::SmallVector<SymRef, 8> RetainedFactors;
    uint32_t Parity = 1;
    size_t Nonconstant = Factors.size() - Ctx.isConst(Factors.front());
    std::optional<uint32_t> Selector;
    bool AssumptionCoversFactors = false;
    for (auto F : Factors) {
      auto Base = F;
      if (Ctx.op(Base) == SymOp::Xor && Ctx.numOperands(Base) == 2 &&
          isOne(Ctx.operand(Base, 0)))
        Base = Ctx.operand(Base, 1);
      if (Ctx.op(Base) == SymOp::And && isOne(Ctx.operand(Base, 0))) {
        Selector = word(F, 1).front();
        break;
      }
    }
    if (Selector || Nonconstant >= Observed) {
      std::optional<uint32_t> Common;
      bool CoversAll = !Selector;
      if (Selector)
        Common = Selector;
      else
        for (auto Factor : Factors) {
          if (Ctx.isConst(Factor))
            continue;
          const auto Low = word(Factor, 1).front();
          if (Failed)
            return Root;
          if (Common && *Common != Low) {
            Common.reset();
            CoversAll = false;
            break;
          }
          Common = Low;
        }
      if (OddProductOnly && !Common && !Failed && Nonconstant > Observed) {
        // A dominant exact prefix supplies enough even factors to annihilate
        // the other branch. Keep every unmatched factor as a complete word;
        // bit-blasting their product can dwarf the proof of the group itself.
        // Small groups need only a shared parity, including multiple
        // independent residuals. Large projected groups keep the stronger
        // prefix that makes their coalesced power proof inexpensive.
        const unsigned Split = ProjectFactors ? (Observed + 2) / 3 : 1;
        // Both complete scans are mandatory. Decline before starting them
        // when even their minimum work cannot fit, preserving the allowance
        // for simpler expressions elsewhere in the same region.
        const size_t ScanUnit = ProjectFactors ? 2 : 2 * (1 + Split);
        if (Nonconstant > (MaxWork - Work) / ScanUnit)
          return Root;
        if (!charge(1, 2 * (sizeof(Bits) + Split * sizeof(uint32_t))))
          return Root;
        Bits Prefix;
        struct PrefixCopies {
          size_t Count = 0;
          bool Matches = false;
        };
        std::unordered_map<uint32_t, PrefixCopies> Copies;
        if (ProjectFactors) {
          // Projection often gives hundreds of factors the same exact word.
          // Count them once, then weight the vote and compare each word once.
          for (auto Factor : Factors) {
            if (Ctx.isConst(Factor))
              continue;
            if (!charge())
              return Root;
            auto It = Copies.find(Factor.index());
            if (It == Copies.end()) {
              if (!charge(1, sizeof(decltype(Copies)::value_type) +
                                 4 * sizeof(void *)))
                return Root;
              It = Copies.emplace(Factor.index(), PrefixCopies{}).first;
            }
            ++It->second.Count;
          }
          size_t Votes = 0;
          for (const auto &[Id, Entry] : Copies) {
            const auto &Low = word(SymRef(Id), Split);
            if (Failed || !charge(1 + Split))
              return Root;
            if (!Votes)
              Prefix = Low;
            if (Prefix == Low) {
              Votes += Entry.Count;
            } else if (Votes > Entry.Count) {
              Votes -= Entry.Count;
            } else {
              Votes = Entry.Count - Votes;
              Prefix = Low;
            }
          }
          for (auto &[Id, Entry] : Copies) {
            const auto &Low = word(SymRef(Id), Split);
            if (Failed || !charge(1 + Split))
              return Root;
            Entry.Matches = Low == Prefix;
          }
        } else {
          size_t Votes = 0;
          for (auto Factor : Factors) {
            if (Ctx.isConst(Factor))
              continue;
            const auto &Low = word(Factor, Split);
            if (Failed || !charge(1 + Split))
              return Root;
            if (!Votes)
              Prefix = Low;
            if (Prefix == Low)
              ++Votes;
            else
              --Votes;
          }
        }
        if (Prefix.front() <= 1)
          return Root;
        const auto D = decision(Prefix.front());
        if (D.Low > 1 || D.High > 1 ||
            !charge(1, 2 * Factors.size() * sizeof(SymRef)))
          return Root;
        size_t Count = 0, Out = 0;
        for (size_t I = 0; I < Factors.size(); ++I) {
          const auto Factor = Factors[I];
          if (Ctx.isConst(Factor)) {
            Factors[Out++] = Factor;
            continue;
          }
          bool Matches;
          if (ProjectFactors) {
            if (!charge())
              return Root;
            Matches = Copies.at(Factor.index()).Matches;
          } else {
            const auto &Low = word(Factor, Split);
            if (Failed || !charge(1 + Split))
              return Root;
            Matches = Low == Prefix;
          }
          if (Matches) {
            Factors[Out++] = Factor;
            ++Count;
          } else {
            RetainedFactors.push_back(Ctx.operand(Root, I));
          }
        }
        // The vote selects a candidate only. Exact equality and a complete
        // count prove the zero branch; residual words need no parity premise.
        if (Count <= Nonconstant / 2 || Count < Observed)
          return Root;
        Factors.resize(Out);
        Nonconstant = Count;
        Common = Prefix.front();
        CoversAll = true;
      }
      if (Common && *Common > 1) {
        const auto D = decision(*Common);
        if (D.Low <= 1 && D.High <= 1) {
          // The selected subgroup has at least Observed even factors in the
          // other branch, or an explicit selector makes that branch zero.
          // Restore its exact selector after multiplying all retained words.
          Parity = *Common;
          Assumption = std::make_pair(D.Variable, D.High == 1);
          AssumptionCoversFactors = CoversAll;
          PriorityInput = uint32_t(D.Variable);
          Words.clear();
          Arithmetic.clear();
          Applied.clear();
          Adders.clear();
          Parities.clear();
        }
      }
    }
    if (OddProductOnly && !Assumption)
      return Root;

    const unsigned ProductBits = Observed;
    // Residual factors stay complete, but their proved zeros can hide high
    // bits of the group. Establish every zero under the same assumption;
    // independent or merely sampled parity supplies no precision reduction.
    for (auto Factor : RetainedFactors) {
      unsigned Zeros = 0;
      while (Zeros < Observed) {
        const auto &Low = word(Factor, Zeros + 1);
        if (Failed || !charge())
          return Root;
        if (Low[Zeros])
          break;
        ++Zeros;
      }
      Observed -= Zeros;
      if (!Observed) {
        if (!charge(1, sizeof(SymNode) + sizeof(llvm::APInt) +
                           ((size_t(Width) - 1) / 64 + 1) * sizeof(uint64_t)))
          return Root;
        return Ctx.mkZero(Width);
      }
    }
    // Individually variable low bits can still annihilate each other. Prove
    // only the next unknown product bit, stopping as soon as it can be one;
    // all residual words remain intact in the emitted full-width product.
    if (RetainedFactors.size() > 1) {
      while (Observed) {
        const unsigned Precision = ProductBits - Observed + 1;
        if (!charge(Precision, Precision * sizeof(uint32_t)))
          return Root;
        Bits Low(Precision, 0);
        Low.front() = 1;
        for (auto Factor : RetainedFactors) {
          Low = calculate(SymOp::Mul, Low, word(Factor, Precision));
          if (Failed || !charge(Precision))
            return Root;
          if (std::all_of(Low.begin(), Low.end(),
                          [](uint32_t Bit) { return Bit == 0; }))
            break;
        }
        if (Low.back())
          break;
        --Observed;
      }
      if (!Observed) {
        if (!charge(1, sizeof(SymNode) + sizeof(llvm::APInt) +
                           ((size_t(Width) - 1) / 64 + 1) * sizeof(uint64_t)))
          return Root;
        return Ctx.mkZero(Width);
      }
    }

    // Full power-of-two blocks favor single-bit pairing. A partial block can
    // also use two-bit pairing when it spans a complete lower-precision block.
    // These are search hints: every pair needs an exact signature and every
    // factor must independently be odd under the current assumption.
    const size_t PairingBlock = size_t(1) << (Observed - 1);
    const size_t WideBlock = size_t(1) << (Observed - std::min(Observed, 2u));
    // Partial blocks can already contain pairs one smaller period apart.
    // The probe only admits a search; signatures still prove every pair.
    const size_t WideProbeBlock =
        WideBlock / limits::kResidueDecisionPartialPairingDivisor;
    const bool FullBlock =
        Nonconstant >= PairingBlock && !(Nonconstant % PairingBlock);
    bool AllOdd = (RetainedFactors.empty() || !ProjectFactors) && Assumption &&
                  Observed >= limits::kMinResidueDecisionPairingBits &&
                  (FullBlock || Nonconstant > WideProbeBlock);
    llvm::SmallVector<SymRef, 16> Nonconstants;
    bool XorFactors = true;
    if (AllOdd) {
      if (!charge(Factors.size(), Factors.size() * sizeof(SymRef)))
        return Root;
      for (auto Factor : Factors) {
        if (Ctx.isConst(Factor))
          continue;
        Nonconstants.push_back(Factor);
        XorFactors &= Ctx.op(Factor) == SymOp::Xor;
        // The common-parity scan already visited every factor before choosing
        // its odd branch. A standalone selector only proves its own value.
        if (!AssumptionCoversFactors && word(Factor, 1).front() != 1) {
          AllOdd = false;
          break;
        }
      }
    }
    if (AllOdd) {
      PairingState State;
      bool PreferSingle = false;
      if (FullBlock) {
        const auto A = residueSignature(State, Nonconstants.front(), Observed);
        const auto B = residueSignature(
            State, Nonconstants[Nonconstants.size() / 2], Observed);
        PreferSingle = A.Identity == B.Identity;
      }
      bool Wide = false;
      if (!PreferSingle &&
          Observed >= limits::kMinResidueDecisionWidePairingBits &&
          Nonconstants.size() > WideProbeBlock &&
          charge(1, sizeof(WidePairingState))) {
        State.Wide = std::make_unique<WidePairingState>();
        // Partial bitwise blocks may have equal narrow nonlinear residues
        // that their syntax cannot expose. Affine, complete and isolated
        // subgroup readings retain their existing search; recursive pairing
        // keeps structural corrections at lower precisions.
        State.Wide->NarrowPrecision =
            !FullBlock && XorFactors && RetainedFactors.empty() ? Observed : 0;
        const auto A =
            FullBlock
                ? wideSignature<true>(*State.Wide, Nonconstants.front(),
                                      Observed)
                : wideSignature(*State.Wide, Nonconstants.front(), Observed);
        const auto Probe =
            Nonconstants.size() > WideBlock ? WideBlock : WideProbeBlock;
        const auto B =
            FullBlock
                ? wideSignature<true>(*State.Wide, Nonconstants[Probe],
                                      Observed)
                : wideSignature(*State.Wide, Nonconstants[Probe], Observed);
        Wide = A.Identity == B.Identity;
        for (size_t I = 1;
             !Wide && FullBlock && !Failed && I < Nonconstants.size(); ++I) {
          if (I == Probe)
            continue;
          if (!charge())
            break;
          const auto Other =
              wideSignature<true>(*State.Wide, Nonconstants[I], Observed);
          Wide = A.Identity == Other.Identity;
        }
      }
      // An odd block leaves unmatched complete words. A partial even block
      // can do so too; retain nonlinear residual words instead of expanding
      // another mixed product after the pair proof. The original parity scan
      // already proved the complete product zero on the other branch.
      auto *Unpaired = !ProjectFactors && RetainedFactors.empty() &&
                               ((Nonconstants.size() & 1) ||
                                (!FullBlock && HasNonlinearFactor))
                           ? &RetainedFactors
                           : nullptr;
      if (Wide) {
        // Complete blocks and odd residual blocks combine their correction
        // columns. Partial even blocks retain streaming work so an unfinished
        // graph leaves the established allowance for ordinary child rewrites.
        Product = FullBlock || (Nonconstants.size() & 1)
                      ? widePairedProduct<true>(State, Nonconstants, Observed,
                                                Unpaired)
                      : widePairedProduct<false>(State, Nonconstants, Observed,
                                                 Unpaired);
      } else {
        Product = FullBlock ? pairedProduct(State, Nonconstants, Observed)
                            : product(Factors, Observed);
      }
    } else {
      Product =
          product(Factors, Observed,
                  Ctx.numOperands(Root) > limits::kMaxResidueProductFactors);
    }
    if (Failed || Product.empty())
      return Root;
    SymRef WholeWord;
    if (!RetainedFactors.empty()) {
      if (!charge(Observed))
        return Root;
      if (std::any_of(Product.begin(), Product.end(),
                      [](uint32_t B) { return B > 1; })) {
        const auto Representative = Factors[Ctx.isConst(Factors.front())];
        const auto &Value = word(Representative, Observed);
        if (Failed || !charge(Observed))
          return Root;
        // Equality holds under the same selector as the product proof. Scale
        // and the retained zeros discard every unobserved representative bit.
        if (Value == Product)
          WholeWord = Representative;
      }
    }
    Assumption.reset();
    // Every product bit was proved under the same parity condition. Keep it
    // outside the complete emitted word instead of repeating it in every bit.
    // The other branch is zero by the selector or factor-count proof above.

    // Equal output decisions share their complete modular coefficient.
    std::map<uint32_t, llvm::APInt> Coefficients;
    for (unsigned I = 0; !WholeWord && I < Observed; ++I) {
      if (!Product[I])
        continue;
      if (!charge(1, sizeof(decltype(Coefficients)::value_type) +
                         4 * sizeof(void *) + ((Width - 1) / 64 + 1) * 8))
        return Root;
      Coefficients.try_emplace(Product[I], Width, 0).first->second +=
          Scale.shl(I);
    }
    const size_t FirstEmitted = Ctx.numNodes();
    std::unordered_map<uint32_t, SymRef> Emitted;
    // Boolean operators commute with projection to bit zero. Build a
    // bitwise word using shifted inputs and all-ones Boolean constants, then
    // mask once at the root instead of masking every selector separately.
    auto Emit = [&](auto &&Self, uint32_t Id, unsigned Depth) -> SymRef {
      if (!charge())
        return {};
      if (Id <= 1)
        return Id ? Ctx.mkConst(llvm::APInt::getAllOnes(Width))
                  : Ctx.mkZero(Width);
      if (auto It = Emitted.find(Id); It != Emitted.end())
        return It->second;
      const auto D = decision(Id);
      const auto Input = SymRef(uint32_t(D.Variable));
      const size_t Words =
          (size_t(std::max(Width, Ctx.width(Input))) - 1) / 64 + 1;
      if (Depth >= limits::kMaxResidueDecisionDepth ||
          !charge(1,
                  sizeof(SymRef) + 4 * sizeof(void *) +
                      8 * (sizeof(SymNode) + 4 * sizeof(SymRef) +
                           sizeof(llvm::APInt) + Words * sizeof(uint64_t)))) {
        Failed = true;
        return {};
      }
      const auto Bit = variableBit(D.Variable);
      auto Selector = Ctx.mkZExtOrTrunc(
          Ctx.mkLShr(Input, Ctx.mkConst(Ctx.width(Input), Bit)), Width);
      auto Low = Self(Self, D.Low, Depth + 1);
      auto High = Self(Self, D.High, Depth + 1);
      if (!Low || !High || Failed)
        return {};
      SymRef Result;
      const auto One = Ctx.mkConst(llvm::APInt::getAllOnes(Width));
      if (!D.Low)
        Result = Ctx.mkAnd(Selector, High);
      else if (!D.High)
        Result = Ctx.mkAnd(Ctx.mkXor(Selector, One), Low);
      else if (D.Low == 1)
        Result = Ctx.mkOr(Ctx.mkXor(Selector, One), High);
      else if (D.High == 1)
        Result = Ctx.mkOr(Selector, Low);
      else if ((D.Low ^ D.High) == 1)
        Result = Ctx.mkXor(Selector, Low);
      else
        Result = Ctx.mkXor(Low, Ctx.mkAnd(Selector, Ctx.mkXor(Low, High)));
      Emitted.emplace(Id, Result);
      return Result;
    };
    auto Synthesize = [&](uint32_t RootId, SymRef Fallback) -> SymRef {
      if (RootId <= 1 ||
          (decision(RootId).Low <= 1 && decision(RootId).High <= 1))
        return Fallback;
      std::map<uint64_t, unsigned> Variables;
      std::unordered_set<uint32_t> Reachable;
      llvm::SmallVector<uint32_t, 16> Pending{RootId};
      while (!Pending.empty()) {
        const auto Id = Pending.pop_back_val();
        if (!charge())
          return Fallback;
        if (Id <= 1 || Reachable.contains(Id))
          continue;
        if (!charge(1, sizeof(decltype(Variables)::value_type) +
                           2 * sizeof(uint32_t) + 8 * sizeof(void *)))
          return Fallback;
        Reachable.insert(Id);
        const auto D = decision(Id);
        Variables.try_emplace(D.Variable, 0);
        // Admission counts the whole guarded function, including the bit
        // fixed by the assumption while constructing this cofactor.
        if (Variables.size() + unsigned(Parity != 1) >
            limits::kMaxResidueDecisionSynthesisInputs)
          return Fallback;
        Pending.push_back(D.Low);
        Pending.push_back(D.High);
      }
      const unsigned Arity = Variables.size();
      const size_t Count = size_t(1) << Arity;
      BitwiseSynthesisLimits Options;
      const size_t SynthesisWork = Arity <= Options.MaxOptimalAtoms
                                       ? bitwiseSynthesisWork(Arity)
                                       : Count * (Arity + 4);
      // Cover, parity and recipe construction each have their own bounded
      // sweep; prepay their combined ceiling in the shared request budget.
      size_t Words = (size_t(Width) - 1) / 64 + 1;
      for (const auto &[Key, Position] : Variables) {
        (void)Position;
        Words = std::max(
            Words, (size_t(Ctx.width(SymRef(uint32_t(Key)))) - 1) / 64 + 1);
      }
      const size_t Units = Count * (3 * Arity + 1) + 3 * SynthesisWork;
      const size_t Bytes = Count * (sizeof(uint64_t) +
                                    8 * (sizeof(SymNode) + sizeof(llvm::APInt) +
                                         Words * sizeof(uint64_t)) +
                                    16 * sizeof(SymRef));
      if (Bytes > Storage && StorageLimited)
        *StorageLimited = true;
      if (Units > MaxWork - Work || Bytes > Storage)
        return Fallback;
      if (!charge(Units, Bytes))
        return Fallback;
      llvm::SmallVector<SymRef, 8> Atoms;
      for (auto &[Key, Position] : Variables) {
        Position = Atoms.size();
        const auto Input = SymRef(uint32_t(Key));
        const auto Bit = variableBit(Key);
        // The recipe is purely bitwise, so projection commutes with every
        // operation. Keep shifted inputs intact and mask once at the result;
        // per-input masks otherwise obscure shorter shared Boolean readings.
        Atoms.push_back(Ctx.mkZExtOrTrunc(
            Ctx.mkLShr(Input, Ctx.mkConst(Ctx.width(Input), Bit)), Width));
      }
      auto Table = TruthTable::zero(Arity);
      for (size_t Pattern = 0; Pattern < Count; ++Pattern) {
        auto Id = RootId;
        while (Id > 1) {
          const auto D = decision(Id);
          Id = (Pattern >> Variables.at(D.Variable)) & 1 ? D.High : D.Low;
        }
        Table.setValue(Pattern, Id != 0);
      }
      Options.MaxWork = SynthesisWork;
      Options.MaxCost = readingCost(Ctx, Fallback);
      if (auto Value = synthesizeBitwise(Ctx, Table, Atoms, Options)) {
        const auto Bit = Ctx.mkAnd(*Value, Ctx.mkConst(Width, 1));
        if (readingScore(Ctx, Bit) < readingScore(Ctx, Fallback))
          return Bit;
      }
      return Fallback;
    };
    std::map<uint64_t, unsigned> PolynomialVariables;
    std::unordered_map<uint32_t, Polynomial> Polynomials;
    auto ReadPolynomial = [&](auto &&Self, uint32_t Id) -> const Polynomial * {
      if (!charge())
        return nullptr;
      static const std::array<Polynomial, 2> Terminals{
          {Polynomial{}, Polynomial{0}}};
      if (Id <= 1)
        return &Terminals[Id];
      if (auto It = Polynomials.find(Id); It != Polynomials.end())
        return &It->second;
      auto Store = [&](Polynomial Value) -> const Polynomial * {
        if (!charge(Value.size(), sizeof(decltype(Polynomials)::value_type) +
                                      4 * sizeof(void *) +
                                      Value.size() * sizeof(uint64_t)))
          return nullptr;
        return &Polynomials.emplace(Id, std::move(Value)).first->second;
      };
      const auto D = decision(Id);
      auto [It, Inserted] = PolynomialVariables.try_emplace(
          D.Variable, PolynomialVariables.size());
      if (It->second >= 64)
        return nullptr;
      if (Inserted &&
          !charge(1, sizeof(decltype(PolynomialVariables)::value_type) +
                         4 * sizeof(void *)))
        return nullptr;
      const auto Variable = uint64_t(1) << It->second;
      auto Low = Self(Self, D.Low), High = Self(Self, D.High & ~uint32_t(1));
      if (!Low || !High)
        return nullptr;
      // P = Low XOR x * (Low XOR High XOR complement). Neither child
      // contains x, so the two outer sets are disjoint. Merge their sorted
      // streams directly instead of allocating, shifting and re-merging an
      // intermediate difference polynomial.
      const bool Equal = Low == High;
      const size_t Size = Low->size() +
                          (Equal ? 0 : Low->size() + High->size()) +
                          unsigned(D.High & 1);
      if (!charge(Size, sizeof(Polynomial) + 2 * Size * sizeof(uint64_t)))
        return nullptr;
      Polynomial Result;
      Result.reserve(
          std::min(Size, limits::kMaxResidueDecisionPolynomialTerms + 1));
      size_t L = 0, H = 0, Copy = 0;
      auto Append = [&](uint64_t Term) {
        if (Term & Variable)
          return false;
        Term |= Variable;
        while (Copy < Low->size() && (*Low)[Copy] < Term) {
          Result.push_back((*Low)[Copy++]);
          if (Result.size() > limits::kMaxResidueDecisionPolynomialTerms)
            return false;
        }
        Result.push_back(Term);
        return Result.size() <= limits::kMaxResidueDecisionPolynomialTerms;
      };
      if ((D.High & 1) && !Append(0))
        return nullptr;
      while (!Equal && (L < Low->size() || H < High->size())) {
        uint64_t Term;
        if (L == Low->size())
          Term = (*High)[H++];
        else if (H == High->size())
          Term = (*Low)[L++];
        else if ((*Low)[L] == (*High)[H]) {
          ++L;
          ++H;
          continue;
        } else if ((*Low)[L] < (*High)[H])
          Term = (*Low)[L++];
        else
          Term = (*High)[H++];
        if (!Append(Term))
          return nullptr;
      }
      if (Result.size() + Low->size() - Copy >
          limits::kMaxResidueDecisionPolynomialTerms)
        return nullptr;
      Result.append(Low->begin() + Copy, Low->end());
      return Store(std::move(Result));
    };
    // Variable indices only grow during this reading, and Width is fixed.
    // A completed factored polynomial therefore keeps the same shifted inputs
    // across output bits, even when a later bit adds previously unseen inputs.
    std::map<Polynomial, SymRef> Factored;
    auto SynthesizePolynomial = [&](uint32_t Id, SymRef Fallback) -> SymRef {
      auto Form = ReadPolynomial(ReadPolynomial, Id & ~uint32_t(1));
      if (!Form)
        return Fallback;
      Polynomial Complement;
      if (Id & 1) {
        if (Form->size() >= limits::kMaxResidueDecisionPolynomialTerms ||
            !charge(Form->size() + 1, (Form->size() + 1) * sizeof(uint64_t)))
          return Fallback;
        Complement.push_back(0);
        Complement.append(Form->begin(), Form->end());
        Form = &Complement;
      }
      size_t Edges = Form->size();
      for (auto Term : *Form)
        Edges += std::popcount(Term);
      size_t Words = (size_t(Width) - 1) / 64 + 1;
      for (const auto &[Variable, Index] : PolynomialVariables) {
        (void)Index;
        Words = std::max(
            Words,
            (size_t(Ctx.width(SymRef(uint32_t(Variable)))) - 1) / 64 + 1);
      }
      const bool HasConstant = !Form->empty() && !Form->front();
      const bool LazyFlat =
          PlainDecisionInputs && Width > 1 &&
          PolynomialVariables.size() <= Edges &&
          Form->size() - unsigned(HasConstant) >
              ::neverd::limits::symbolic::kSymExprCompareMaxTerms;
      // Distinct monomials over plain equally-wide shifted free inputs have
      // no absorbing/complement pairs or extension contractions. Above the
      // comparison recognizer's arity, XOR also preserves their exact spelling.
      // Thus the flat form's reading score can be bounded without interning
      // its monomials. Keep every other input shape on the established route.
      const size_t ConstructionBytes =
          4 * sizeof(SymNode) + 8 * sizeof(SymRef) + 4 * sizeof(llvm::APInt) +
          4 * Words * sizeof(uint64_t);
      const size_t InitialEdges = LazyFlat ? PolynomialVariables.size() : Edges;
      if (!charge(InitialEdges * (1 + Words),
                  (InitialEdges + 1) * ConstructionBytes))
        return Fallback;
      llvm::SmallVector<SymRef, 16> Inputs(PolynomialVariables.size());
      for (const auto &[Variable, Index] : PolynomialVariables) {
        const auto Input = SymRef(uint32_t(Variable));
        Inputs[Index] = Ctx.mkZExtOrTrunc(
            Ctx.mkLShr(Input,
                       Ctx.mkConst(Ctx.width(Input), variableBit(Variable))),
            Width);
      }
      // Every plain input contributes at least one leaf and every distinct
      // monomial occurrence needs its Boolean operator in the flat reading.
      // Shifted inputs can only increase this bound. A factored result below
      // it wins without a second frequency scan or flat construction; other
      // results still use the established exact reading comparison.
      const size_t Incidences = Edges - Form->size();
      const SymReadability FlatLowerBound{1 + size_t(HasConstant) +
                                              2 * Incidences,
                                          size_t(HasConstant) + Incidences};
      auto BuildFlat = [&]() -> SymRef {
        // Fund only the construction that actually takes place. If the flat
        // expression is needed after factoring, cumulative construction
        // reservations still equal the original complete flat reservation.
        if (LazyFlat && !charge((Edges - InitialEdges) * (1 + Words),
                                (Edges - InitialEdges) * ConstructionBytes))
          return {};
        llvm::SmallVector<SymRef, 16> Terms;
        for (auto Term : *Form) {
          llvm::SmallVector<SymRef, 8> Factors;
          while (Term) {
            Factors.push_back(Inputs[std::countr_zero(Term)]);
            Term &= Term - 1;
          }
          Terms.push_back(Factors.empty()
                              ? Ctx.mkConst(llvm::APInt::getAllOnes(Width))
                              : Ctx.mkAnd(Factors));
        }
        return Ctx.mkAnd(Terms.empty() ? Ctx.mkZero(Width) : Ctx.mkXor(Terms),
                         Ctx.mkConst(Width, 1));
      };
      SymRef Result = LazyFlat ? SymRef{} : BuildFlat();
      auto Factor =
          [&](auto &&Self, const Polynomial &P, llvm::ArrayRef<SymRef> Basis,
              std::map<Polynomial, SymRef> &Cache, bool TryCoordinate,
              llvm::ArrayRef<unsigned> KnownFrequencies = {}) -> SymRef {
        if (!charge(1 + P.size()))
          return {};
        if (P.empty())
          return Ctx.mkZero(Width);
        if (P.size() == 1 && !P.front())
          return Ctx.mkConst(llvm::APInt::getAllOnes(Width));
        if (auto It = Cache.find(P); It != Cache.end())
          return It->second;
        // Cache hits allocate no map entry or copied polynomial. Reserve a
        // complete entry before constructing a new result; failed attempts
        // retain this conservative reservation and are never cached.
        if (!charge(0, sizeof(decltype(Factored)::value_type) +
                           4 * sizeof(void *) + P.size() * sizeof(uint64_t)))
          return {};
        llvm::SmallVector<unsigned, 16> CountedFrequencies;
        llvm::ArrayRef<unsigned> Frequencies = KnownFrequencies;
        if (Frequencies.empty()) {
          CountedFrequencies = countPolynomialFrequencies(P, Basis.size());
          if (Failed)
            return {};
          Frequencies = CountedFrequencies;
        }
        const auto Best =
            std::max_element(Frequencies.begin(), Frequencies.end());
        if (TryCoordinate && *Best != P.size()) {
          if (!charge(Basis.size()))
            return {};
          // A variable present in every term is already a common factor.
          // Test only the two most frequent remaining variables, using the
          // frequencies that the ordinary factorization already computed.
          unsigned First = Basis.size(), Second = Basis.size();
          for (unsigned I = 0; I < Frequencies.size(); ++I) {
            if (!Frequencies[I] || Frequencies[I] == P.size())
              continue;
            if (First == Basis.size() || Frequencies[I] > Frequencies[First]) {
              Second = First;
              First = I;
            } else if (Second == Basis.size() ||
                       Frequencies[I] > Frequencies[Second]) {
              Second = I;
            }
          }
          if (Second != Basis.size()) {
            const uint64_t IBit = uint64_t(1) << Second;
            const uint64_t JBit = uint64_t(1) << First;
            const uint64_t PairMask = IBit | JBit;
            // Each inline vector object owns its inline storage. A grown
            // SmallVector has capacity below twice its requested size; fund
            // that possible heap capacity before constructing each vector.
            if (!charge(P.size(), 4 * sizeof(Polynomial) +
                                      2 * P.size() * sizeof(uint64_t)))
              return {};
            Polynomial Groups[4];
            for (auto Term : P) {
              const unsigned Group =
                  unsigned(bool(Term & IBit)) + 2 * unsigned(bool(Term & JBit));
              Groups[Group].push_back(Term & ~PairMask);
            }
            // P=A XOR i*B XOR j*C XOR i*j*D. With t=i XOR j this is
            // A XOR t*B XOR j*(B XOR C XOR D) XOR t*j*D. Fixed-variable
            // projections preserve order, so both differences are linear.
            const size_t CombinedSize =
                Groups[1].size() + Groups[2].size() + Groups[3].size();
            if (!charge(CombinedSize, sizeof(Polynomial) +
                                          2 * CombinedSize * sizeof(uint64_t)))
              return {};
            Polynomial Combined;
            Combined.reserve(CombinedSize);
            std::array<size_t, 3> Position{};
            for (size_t Remaining = CombinedSize; Remaining;) {
              uint64_t Term = UINT64_MAX;
              for (unsigned I = 0; I < 3; ++I)
                if (Position[I] < Groups[I + 1].size())
                  Term = std::min(Term, Groups[I + 1][Position[I]]);
              bool Odd = false;
              for (unsigned I = 0; I < 3; ++I)
                if (Position[I] < Groups[I + 1].size() &&
                    Groups[I + 1][Position[I]] == Term) {
                  ++Position[I];
                  --Remaining;
                  Odd = !Odd;
                }
              if (Odd)
                Combined.push_back(Term);
            }
            // Demand a reduction in both one-variable cofactors, rather
            // than accepting a small advantage from their frequency order.
            if (Combined.size() < Groups[1].size() &&
                Combined.size() < Groups[2].size()) {
              const auto NewSize =
                  P.size() - Groups[2].size() + Combined.size();
              if (!charge(P.size() + Combined.size() + Basis.size() + 1,
                          sizeof(Polynomial) + 2 * NewSize * sizeof(uint64_t) +
                              sizeof(llvm::SmallVector<SymRef, 16>) +
                              2 * Basis.size() * sizeof(SymRef) +
                              sizeof(std::map<Polynomial, SymRef>) +
                              sizeof(SymNode) + 4 * sizeof(SymRef)))
                return {};
              Polynomial Transformed;
              Transformed.reserve(NewSize);
              size_t Original = 0, Replacement = 0;
              while (Original != P.size() || Replacement != Combined.size()) {
                while (Original != P.size() && (P[Original] & PairMask) == JBit)
                  ++Original;
                if (Replacement != Combined.size() &&
                    (Original == P.size() ||
                     (Combined[Replacement] | JBit) < P[Original]))
                  Transformed.push_back(Combined[Replacement++] | JBit);
                else if (Original != P.size())
                  Transformed.push_back(P[Original++]);
              }
              if (!charge(Frequencies.size(),
                          sizeof(llvm::SmallVector<unsigned, 16>) +
                              2 * Frequencies.size() * sizeof(unsigned)))
                return {};
              llvm::SmallVector<unsigned, 16> CoordinateFrequencies(
                  Frequencies);
              // The new polynomial changes only j*C into j*Combined.
              // Update the existing counts instead of rescanning every
              // unchanged term in the transformed root.
              const auto Removed =
                  countPolynomialFrequencies(Groups[2], Basis.size());
              const auto Added =
                  countPolynomialFrequencies(Combined, Basis.size());
              if (Failed || !charge(Frequencies.size()))
                return {};
              for (unsigned I = 0; I < Frequencies.size(); ++I) {
                CoordinateFrequencies[I] -= Removed[I];
                CoordinateFrequencies[I] += Added[I];
              }
              CoordinateFrequencies[First] -= Groups[2].size();
              CoordinateFrequencies[First] += Combined.size();
              llvm::SmallVector<SymRef, 16> CoordinateInputs(Basis);
              CoordinateInputs[Second] = Ctx.mkXor(Basis[Second], Basis[First]);
              // Polynomial keys only identify expressions within one basis.
              // Keep the changed basis separate; its final result again has
              // exactly P's original full-width bitwise semantics.
              std::map<Polynomial, SymRef> CoordinateCache;
              auto Value = Self(Self, Transformed, CoordinateInputs,
                                CoordinateCache, false, CoordinateFrequencies);
              if (Value)
                Cache.emplace(P, Value);
              return Value;
            }
          }
        }
        const unsigned Variable = Best - Frequencies.begin();
        const auto Bit = uint64_t(1) << Variable;
        Polynomial Low, High;
        uint64_t LowSupport = 0, HighSupport = 0;
        for (auto Term : P) {
          if (Term & Bit) {
            High.push_back(Term ^ Bit);
            HighSupport |= Term ^ Bit;
          } else {
            Low.push_back(Term);
            LowSupport |= Term;
          }
        }
        // Equal cofactors give Low AND NOT x. Cofactors differing only in
        // their constant term give Low OR x. Compare the canonical tails
        // directly before constructing quotient groups and their remainders.
        llvm::ArrayRef<uint64_t> LowTail(Low), HighTail(High);
        const bool LowConstant = !Low.empty() && !Low.front();
        const bool HighConstant = !High.empty() && !High.front();
        if (LowConstant)
          LowTail = LowTail.drop_front();
        if (HighConstant)
          HighTail = HighTail.drop_front();
        if (LowTail.size() == HighTail.size()) {
          if (!charge(1 + LowTail.size()))
            return {};
          if (LowTail == HighTail) {
            auto A = Self(Self, Low, Basis, Cache, false);
            const unsigned Nodes = LowConstant == HighConstant ? 2 : 1;
            if (!A ||
                !charge(Nodes, Nodes * (sizeof(SymNode) + 4 * sizeof(SymRef))))
              return {};
            auto Value = LowConstant == HighConstant
                             ? Ctx.mkAnd(A, Ctx.mkNot(Basis[Variable]))
                             : Ctx.mkOr(A, Basis[Variable]);
            Cache.emplace(P, Value);
            return Value;
          }
        }
        // P = Low XOR (x AND High). Partition Low over the variables not
        // present in High. A group sharing most of High can reuse that exact
        // cofactor: Low = R XOR (Q AND High), hence
        // P = R XOR ((x XOR Q) AND High). Symmetric difference proves the
        // remainder, including complemented and partially matching groups.
        // When Low has no variables outside High's support, its partition
        // has one group. The same exact difference needs no map or re-sort.
        const bool Reusable = Low.size() > High.size() / 2 &&
                              !(High.size() == 1 && !High.front());
        const bool SingleGroup = !(LowSupport & ~HighSupport);
        if (Reusable && SingleGroup) {
          const auto Size = Low.size() + High.size();
          if (!charge(Size, sizeof(Polynomial) + 2 * Size * sizeof(uint64_t)))
            return {};
          Polynomial Difference;
          Difference.reserve(Size);
          std::set_symmetric_difference(Low.begin(), Low.end(), High.begin(),
                                        High.end(),
                                        std::back_inserter(Difference));
          if (Difference.size() < Low.size()) {
            Polynomial Quotient{0, Bit};
            auto Q = Self(Self, Quotient, Basis, Cache, false),
                 H = Self(Self, High, Basis, Cache, false),
                 R = Self(Self, Difference, Basis, Cache, false);
            if (!Q || !H || !R ||
                !charge(2, 2 * (sizeof(SymNode) + 4 * sizeof(SymRef))))
              return {};
            auto Value = Ctx.mkXor(R, Ctx.mkAnd(Q, H));
            Cache.emplace(P, Value);
            return Value;
          }
        }
        if (Reusable && !SingleGroup) {
          const auto OutsideMask = LowSupport & ~HighSupport;
          const bool TwoGroups = std::has_single_bit(OutsideMask);
          const size_t Bytes =
              TwoGroups
                  ? 2 * sizeof(Polynomial) + 4 * Low.size() * sizeof(uint64_t)
                  : Low.size() * (sizeof(uint64_t) + sizeof(Polynomial) +
                                  4 * sizeof(void *) + 2 * sizeof(uint64_t));
          if (!charge(High.size() + Low.size(), Bytes))
            return {};
          Polynomial Quotient{Bit}, Remainder;
          bool Found = false;
          auto ReuseGroup = [&](uint64_t Outside, const Polynomial &Inside) {
            // |Inside XOR High| < |Inside| requires more than half of High
            // to occur in Inside. Smaller groups cannot reuse this cofactor;
            // retain them directly instead of scanning High for every group.
            if (Inside.size() <= High.size() / 2) {
              if (!charge(Inside.size()))
                return false;
              for (auto Term : Inside)
                Remainder.push_back(Term | Outside);
              return true;
            }
            const auto Size = Inside.size() + High.size();
            if (!charge(Size, sizeof(Polynomial) + 2 * Size * sizeof(uint64_t)))
              return false;
            Polynomial Difference;
            std::set_symmetric_difference(Inside.begin(), Inside.end(),
                                          High.begin(), High.end(),
                                          std::back_inserter(Difference));
            const bool Reuse = Difference.size() < Inside.size();
            if (Reuse) {
              Quotient.push_back(Outside);
              Found = true;
            }
            for (auto Term : Reuse ? Difference : Inside)
              Remainder.push_back(Term | Outside);
            return true;
          };
          if (TwoGroups) {
            // One outside variable selects exactly two possible groups.
            // Clearing that fixed bit preserves each group's term order.
            std::array<Polynomial, 2> Groups;
            for (auto Term : Low)
              Groups[bool(Term & OutsideMask)].push_back(Term & HighSupport);
            for (unsigned I = 0; I < Groups.size(); ++I)
              if (!ReuseGroup(I ? OutsideMask : 0, Groups[I]))
                return {};
          } else {
            std::map<uint64_t, Polynomial> Groups;
            for (auto Term : Low) {
              if (!charge(1 + std::bit_width(Groups.size())))
                return {};
              Groups[Term & ~HighSupport].push_back(Term & HighSupport);
            }
            for (const auto &[Outside, Inside] : Groups)
              if (!ReuseGroup(Outside, Inside))
                return {};
          }
          if (Found) {
            const auto Size = Quotient.size() + Remainder.size();
            if (!charge(Size * (1 + std::bit_width(Size)),
                        Size * sizeof(uint64_t)))
              return {};
            std::sort(Quotient.begin(), Quotient.end());
            std::sort(Remainder.begin(), Remainder.end());
            auto Q = Self(Self, Quotient, Basis, Cache, false),
                 H = Self(Self, High, Basis, Cache, false),
                 R = Self(Self, Remainder, Basis, Cache, false);
            if (!Q || !H || !R ||
                !charge(2, 2 * (sizeof(SymNode) + 4 * sizeof(SymRef))))
              return {};
            auto Value = Ctx.mkXor(R, Ctx.mkAnd(Q, H));
            Cache.emplace(P, Value);
            return Value;
          }
        }
        llvm::SmallVector<unsigned, 16> LowFrequencies, HighFrequencies;
        // These two lookups are additional work; each child still pays its
        // polynomial visit at the recursive entry, even on a cache hit.
        if (!Low.empty() && !charge(2))
          return {};
        if (!Low.empty() && !Cache.contains(Low) && !Cache.contains(High)) {
          LowFrequencies = countPolynomialFrequencies(Low, Basis.size());
          if (Failed || !charge(Frequencies.size(),
                                Frequencies.size() * sizeof(unsigned)))
            return {};
          HighFrequencies.assign(Frequencies.begin(), Frequencies.end());
          for (unsigned I = 0; I < Frequencies.size(); ++I)
            HighFrequencies[I] -= LowFrequencies[I];
          HighFrequencies[Variable] = 0;
        }
        auto A = Self(Self, Low, Basis, Cache, false, LowFrequencies);
        SymRef B;
        if (TryCoordinate && Low.empty()) {
          // All terms contain this common factor. Its removal changes only
          // that variable's frequency. Carry the single coordinate attempt
          // past common factors without recounting the untouched variables.
          if (!charge(Frequencies.size(),
                      sizeof(llvm::SmallVector<unsigned, 16>) +
                          2 * Frequencies.size() * sizeof(unsigned)))
            return {};
          llvm::SmallVector<unsigned, 16> HighFrequencies(Frequencies);
          HighFrequencies[Variable] = 0;
          B = Self(Self, High, Basis, Cache, true, HighFrequencies);
        } else {
          B = Self(Self, High, Basis, Cache, false, HighFrequencies);
        }
        if (!A || !B || !charge(3, 3 * (sizeof(SymNode) + 4 * sizeof(SymRef))))
          return {};
        auto Value = Ctx.mkXor(A, Ctx.mkAnd(Basis[Variable], B));
        Cache.emplace(P, Value);
        return Value;
      };
      if (auto Smaller = Factor(Factor, *Form, Inputs, Factored, true)) {
        Smaller = Ctx.mkAnd(Smaller, Ctx.mkConst(Width, 1));
        if (LazyFlat && readingScore(Ctx, Smaller) < FlatLowerBound) {
          Result = Smaller;
        } else {
          if (LazyFlat)
            Result = BuildFlat();
          if (Result && readingScore(Ctx, Smaller) < readingScore(Ctx, Result))
            Result = Smaller;
        }
      } else if (LazyFlat && !Failed) {
        Result = BuildFlat();
      }
      if (!Result)
        return Fallback;
      return !Fallback ||
                     readingScore(Ctx, Result) < readingScore(Ctx, Fallback)
                 ? Result
                 : Fallback;
    };
    auto ScaledBit = [&](const llvm::APInt &C, SymRef V) -> SymRef {
      // A highest-bit coefficient already discards every bit except bit zero.
      // Other coefficients must retain the explicit one-bit projection.
      if (C.countr_zero() == Width - 1 && Ctx.op(V) == SymOp::And &&
          isOne(Ctx.operand(V, 0))) {
        auto Args = Ctx.operands(V).drop_front();
        if (!charge(Args.size(),
                    sizeof(SymNode) + Args.size() * sizeof(SymRef)))
          return {};
        V = Ctx.mkAnd(Args);
      }
      // C = 2^(Width-1) is its own modular negation, so C*~V = C+C*V.
      // Finish this normalization here instead of sending the completed
      // Boolean reading through another scalar proof search.
      if (C.countr_zero() == Width - 1 && Ctx.op(V) == SymOp::Not) {
        if (!charge(3, 3 * (sizeof(SymNode) + 4 * sizeof(SymRef))))
          return {};
        auto Other = Ctx.operand(V, 0);
        auto Coefficient = Ctx.mkConst(C);
        return Ctx.mkAdd(Coefficient, Ctx.mkMul(Coefficient, Other));
      }
      return Ctx.mkMul(Ctx.mkConst(C), V);
    };
    llvm::SmallVector<SymRef, 8> Terms;
    if (WholeWord) {
      if (!charge(3, 3 * (sizeof(SymNode) + 4 * sizeof(SymRef) +
                          sizeof(llvm::APInt) +
                          ((size_t(Width) - 1) / 64 + 1) * 8)))
        return Root;
      Terms.push_back(Ctx.mkMul(Ctx.mkConst(Scale), WholeWord));
    }
    llvm::SmallVector<std::pair<uint32_t, SymRef>, 8> Values;
    llvm::SmallVector<bool, 8> PolynomialBuilt;
    for (const auto &[Id, Coefficient] : Coefficients) {
      if (Coefficient.isZero())
        continue;
      auto Value =
          Observed >= limits::kResidueDecisionPolynomialFirstBits &&
                  Nodes.size() >= limits::kResidueDecisionPolynomialFirstNodes
              ? SynthesizePolynomial(Id, {})
              : SymRef{};
      const bool HasPolynomial = Value.isValid();
      if (!Value && !Failed) {
        Value = Emit(Emit, Id, 0);
        if (Value)
          Value = Ctx.mkAnd(Value, Ctx.mkConst(Width, 1));
      }
      if (!Value || Failed) {
        return Root;
      }
      Values.emplace_back(Id, Value);
      PolynomialBuilt.push_back(HasPolynomial);
      auto Term = ScaledBit(Coefficient, Value);
      if (!Term)
        return Root;
      Terms.push_back(Term);
    }
    if (!charge(1 + 3 * (Ctx.numNodes() - Ctx.numCachedReadabilityNodes()),
                (Terms.size() + 1) * (sizeof(SymNode) + 4 * sizeof(SymRef)))) {
      return Root;
    }
    SymRef OutputGate;
    if (Parity != 1) {
      OutputGate = Emit(Emit, Parity, 0);
      if (!OutputGate || Failed ||
          !charge(3, 3 * (sizeof(SymNode) + 4 * sizeof(SymRef))))
        return Root;
      OutputGate = Ctx.mkAnd(OutputGate, Ctx.mkConst(Width, 1));
    }
    const bool HasRetainedFactors = !RetainedFactors.empty();
    if (HasRetainedFactors && OutputGate)
      RetainedFactors.push_back(OutputGate);
    auto Gate = [&](SymRef R) -> SymRef {
      if (!HasRetainedFactors)
        return OutputGate ? Ctx.mkMul(OutputGate, R) : R;
      if (!charge(RetainedFactors.size() + 1,
                  sizeof(SymNode) +
                      2 * (RetainedFactors.size() + 1) * sizeof(SymRef)))
        return {};
      RetainedFactors.push_back(R);
      auto Value = Ctx.mkMul(RetainedFactors);
      RetainedFactors.pop_back();
      return Value;
    };
    auto Result = Gate(Terms.empty() ? Ctx.mkZero(Width) : Ctx.mkAdd(Terms));
    if (!Result)
      return Root;
    // Finish the proved reading before optional Boolean recipe search.
    // A later refusal must retain that complete result, including when one
    // output bit has a more complicated support than the others.
    for (size_t I = 0; I < Values.size(); ++I) {
      const auto [Id, Value] = Values[I];
      // Reserve publication before optional refinement. Even if a later
      // attempt refuses, an already completed smaller bit remains usable.
      if (!charge(Terms.size() + 1,
                  (Terms.size() + 2) * (sizeof(SymNode) + 4 * sizeof(SymRef) +
                                        sizeof(llvm::APInt) +
                                        ((size_t(Width) - 1) / 64 + 1) * 8)))
        break;
      // The first construction already factored this exact polynomial. A
      // second pass would rebuild the same form under fewer resources.
      auto Shorter =
          PolynomialBuilt[I] ? Value : SynthesizePolynomial(Id, Value);
      if (!Failed && Shorter == Value)
        Shorter = Synthesize(Id, Shorter);
      if (Shorter != Value) {
        auto Term = ScaledBit(Coefficients.at(Id), Shorter);
        if (!Term)
          break;
        Terms[I] = Term;
        auto Next = Gate(Ctx.mkAdd(Terms));
        if (!Next)
          break;
        if (readingScore(Ctx, Next) < readingScore(Ctx, Result))
          Result = Next;
      }
      if (Failed)
        break;
    }

    // A reconstructed cofactor expression has the same complete word. Reuse
    // this finished search only when every input was a real free variable;
    // a second reading can otherwise reveal relations between opaque words.
    if (Complete && !Failed && charge(Inputs.size()) &&
        std::all_of(Inputs.begin(), Inputs.end(),
                    [&](uint32_t I) { return Ctx.isVar(SymRef(I)); }))
      *Complete = true;
    if (readingScore(Ctx, Result) >= readingScore(Ctx, Root))
      return Root;
    commitWork();
    recordResidueReading(Ctx, FirstEmitted, Budget, Storage);
    return Result;
  }
};

} // namespace

SymRef foldResidueDecisions(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                            size_t &Storage, bool OddProductOnly,
                            size_t MaxWork, bool *StorageLimited,
                            bool *Complete) {
  if (Complete)
    *Complete = false;
  if (Ctx.op(Root) != SymOp::Mul || Ctx.numOperands(Root) < 2)
    return Root;
  const size_t Words = (size_t(Ctx.width(Root)) - 1) / 64 + 1;
  const size_t Bytes = sizeof(llvm::APInt) + Words * sizeof(uint64_t);
  if (Bytes > Storage && StorageLimited)
    *StorageLimited = true;
  if (Words > MaxWork || Bytes > Storage || !Budget.canConsume(Words))
    return Root;
  Budget.consume(Words);
  Storage -= Bytes;
  MaxWork -= Words;
  const auto First = Ctx.operand(Root, 0);
  const auto Scale = Ctx.isConst(First) ? Ctx.constValue(First)
                                        : llvm::APInt(Ctx.width(Root), 1);
  const unsigned Observed = Ctx.width(Root) - Scale.countr_zero();
  if (!Observed || Observed > limits::kMaxAffineResidueTableBits)
    return Root;
  return LowWordDecisions(Ctx, Budget, Storage, MaxWork, StorageLimited)
      .simplify(Root, Scale, Observed, OddProductOnly, Complete);
}

} // namespace neverd::symbolic::detail
