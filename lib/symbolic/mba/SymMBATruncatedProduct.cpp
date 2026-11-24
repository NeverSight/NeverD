//===- SymMBATruncatedProduct.cpp - Modular offset polynomials -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// Build short Horner forms for repeated word factors modulo the observed
/// width. Distinct words and the factors outside each group remain exact
/// multipliers, so they may overlap or depend on each other arbitrarily.
///
//===----------------------------------------------------------------------===//

#include "../SymFunnel.h"
#include "Limits.h"
#include "SymMBADetail.h"

#include "neverd/symbolic/Limits.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace neverd::symbolic::detail {

namespace {

struct Factor {
  SymRef Ref, Word, Weight, Offset;
  unsigned Zeros, Content, Copies;
  bool Complement;
};

// Concatenation and either extension retain their low operand verbatim.
SymRef lowOperand(const SymContext &Ctx, SymRef R) {
  switch (Ctx.op(R)) {
  case SymOp::Concat:
    return Ctx.operands(R).back();
  case SymOp::ZExt:
  case SymOp::SExt:
    return Ctx.operand(R, 0);
  default:
    return {};
  }
}

struct ProductField {
  SymRef Low;
  unsigned Start = 0, Size = 0;
};

// An extension or concatenation preserves its low operand. A canonical
// fixed motion preserves its source suffix up to the fill or wrap boundary.
ProductField productField(const SymContext &Ctx, SymRef Word) {
  if (const auto Low = lowOperand(Ctx, Word))
    return {Low, 0, Ctx.width(Low)};
  if (const unsigned Start = detail::sourceSuffixStart(Ctx, Word))
    return {{}, Start, Ctx.width(Word) - Start};
  return {};
}

// A compiler can distribute a shift into a sum of scaled words. Each
// multiple of 2^End is still zero throughout the observed low prefix.
bool hasZeroPrefix(const SymContext &Ctx, SymRef Term, unsigned End,
                   size_t Words, WorkBudget &Budget) {
  auto Test = [&](SymRef V) {
    if (Ctx.op(V) == SymOp::Mul || Ctx.op(V) == SymOp::And)
      V = Ctx.operand(V, 0);
    return Ctx.isConst(V) && Budget.consume(1 + Words) &&
           Ctx.constValue(V).countr_zero() >= End;
  };
  if (Ctx.op(Term) != SymOp::Add)
    return Test(Term);
  const auto Addends = Ctx.operands(Term);
  return Budget.consume(Addends.size()) && llvm::all_of(Addends, Test);
}

// Pick a word whose low bits equal this slice. Rotations move every bit.
// A shift moves only its source interval: a window touching zero/sign
// padding (including a window wrapped by an outer rotate) must retain it.
SymRef widenWindow(SymContext &Ctx, SymRef Source, uint64_t Offset,
                   unsigned Size, unsigned Width, WorkBudget &Budget,
                   size_t &Storage) {
  const size_t Words = llvm::APInt::getNumWords(Width);
  for (;;) {
    const auto Op = Ctx.op(Source);
    if (Op == SymOp::Rol || Op == SymOp::Ror) {
      const auto Count = Ctx.operand(Source, 1);
      if (!Ctx.isConst(Count))
        break;
      if (!Budget.consume(1 + llvm::APInt::getNumWords(Ctx.width(Count))))
        return {};
      Offset = (Offset + Ctx.node(Source).Aux) % Width;
      Source = Ctx.operand(Source, 0);
      continue;
    }
    const uint64_t End = Offset + Size;
    const bool Join = Op == SymOp::Or || Op == SymOp::Xor || Op == SymOp::Add;
    if (const auto Shifts = FunnelShifts::match(Ctx, Source)) {
      if (!Budget.consume(2 + 2 * Words))
        return {};
      if (const auto Window = Shifts->project(Ctx, Offset, Size)) {
        Source = Window->Word;
        Offset = Window->Offset;
        continue;
      }
    }
    if (Join && End <= Width) {
      auto RightShift = [&](SymRef V) {
        return Ctx.op(V) == SymOp::LShr || Ctx.op(V) == SymOp::AShr;
      };
      auto Terms = Ctx.operands(Source);
      if (Terms.size() > 2 && !Budget.consume(Terms.size()))
        return {};
      auto Shifted = llvm::find_if(Terms, RightShift);
      auto Zero = [&](SymRef V) {
        return hasZeroPrefix(Ctx, V, End, Words, Budget);
      };
      if (Shifted != Terms.end() && std::all_of(Terms.begin(), Shifted, Zero) &&
          std::all_of(Shifted + 1, Terms.end(), Zero)) {
        Source = *Shifted;
        continue;
      }
      if (Budget.exhausted())
        return {};
    }
    if (Op == SymOp::LShr || Op == SymOp::AShr) {
      const auto Count = Ctx.operand(Source, 1);
      if (!Ctx.isConst(Count))
        break;
      if (!Budget.consume(1 + llvm::APInt::getNumWords(Ctx.width(Count))))
        return {};
      const uint64_t Shift = Ctx.constValue(Count).getLimitedValue(Width);
      if (Offset + Size + Shift > Width)
        break;
      Offset += Shift;
      Source = Ctx.operand(Source, 0);
      continue;
    }
    const bool LeftShift = Op == SymOp::Mul;
    // Constant left shifts are canonicalized as power-of-two products.
    // Opposite shifts may also have combined into a mask.
    if ((!LeftShift && Op != SymOp::And) ||
        !Ctx.isConst(Ctx.operand(Source, 0)))
      break;
    if (!Budget.consume(1 + (LeftShift ? Words : 3 * Words)))
      return {};
    const auto Literal = Ctx.constValue(Ctx.operand(Source, 0));
    if (LeftShift) {
      if (End <= Literal.countr_zero())
        return Ctx.mkZero(Width);
      const int32_t Shift = Literal.exactLogBase2();
      if (Shift < 0 || Offset < static_cast<unsigned>(Shift) ||
          Offset + Size > Width)
        break;
      Offset -= Shift;
    } else {
      const auto Mask = llvm::APInt::getBitsSetWithWrap(
          Width, Offset, (Offset + Size) % Width);
      if (!Mask.isSubsetOf(Literal))
        break;
    }
    const auto Tail = Ctx.operands(Source).drop_front();
    if (Tail.size() == 1) {
      Source = Tail.front();
    } else {
      const size_t Bytes = sizeof(SymNode) + 2 * Tail.size() * sizeof(SymRef);
      if (Bytes > Storage ||
          !Budget.consume(Tail.size() * (2 + std::bit_width(Tail.size()))))
        return {};
      Storage -= Bytes;
      Source = Ctx.rebuild(Source, Tail);
    }
  }
  if (!Offset)
    return Source;
  const size_t Extra = sizeof(SymNode) + 2 * sizeof(SymRef);
  if (Storage < Extra || !Budget.consume(2 + Words))
    return {};
  Storage -= Extra;
  // An instruction combiner can spell the field with shifts and masks.
  // Once their coordinates are known, the same narrow join represents every
  // copy when the selected bits wrap across bit zero.
  const auto Kind = Ctx.op(Source);
  if (Offset + Size > Width && (Kind == SymOp::Or || Kind == SymOp::Xor)) {
    const size_t Arms = Ctx.numOperands(Source);
    const size_t PerArm =
        5 * sizeof(SymNode) + 10 * sizeof(SymRef) +
        (Words + 2 * llvm::APInt::getNumWords(Size)) * sizeof(uint64_t);
    const size_t ArmWork = 6 + 3 * Words;
    if (Arms > 2 &&
        Arms - 1 <= ::neverd::limits::symbolic::kSymExprMaskedFieldMaxSplits &&
        Arms <= Storage / PerArm && Arms <= Budget.available() / ArmWork) {
      Budget.consume(Arms * ArmWork);
      if (const auto Field =
              Ctx.projectMaskedBitwiseField(Source, Offset, Size)) {
        Storage -= Arms * PerArm;
        return Ctx.mkZExt(Field, Width);
      }
    }
  }
  return Ctx.mkRor(Source, Ctx.mkConst(Width, Offset));
}

// A narrow sum, product or bitwise expression is congruent to the same
// operation on word-sized representatives modulo its original width. Pick
// common representatives so different high lanes can share a polynomial.
SymRef widenCoordinate(SymContext &Ctx, SymRef Low, unsigned Width,
                       llvm::DenseMap<uint32_t, SymRef> &Widened,
                       WorkBudget &Budget, size_t &Storage) {
  if (const auto Known = Widened.lookup(Low.index()))
    return Known;
  llvm::SmallVector<std::pair<SymRef, bool>, 16> Stack{{Low, false}};
  const size_t Words = llvm::APInt::getNumWords(Width);
  while (!Stack.empty()) {
    const auto [V, Visited] = Stack.pop_back_val();
    if (Widened.contains(V.index()))
      continue;
    const auto Op = Ctx.op(V);
    const bool Modular = Op == SymOp::Add || Op == SymOp::Mul || isBitwise(Op);
    const auto Children = Ctx.operands(V);
    if (!Visited) {
      const size_t Count = Modular ? Children.size() : 0;
      const size_t Entry =
          sizeof(SymNode) + 4 * sizeof(SymRef) + Words * sizeof(uint64_t);
      if (Storage < Entry || Count > (Storage - Entry) / (4 * sizeof(SymRef)) ||
          !Budget.consume(1 + Words + Count))
        return {};
      Storage -= Entry + Count * 4 * sizeof(SymRef);
      if (Modular) {
        Stack.emplace_back(V, true);
        for (SymRef Child : Children)
          Stack.emplace_back(Child, false);
        continue;
      }
    }
    SymRef Wide;
    if (Modular) {
      llvm::SmallVector<SymRef, 8> Args;
      size_t Count = 0;
      for (SymRef Child : Children) {
        auto Value = Widened.lookup(Child.index());
        Count += Ctx.op(Value) == Op ? Ctx.numOperands(Value) : 1;
        Args.push_back(Value);
      }
      if (Count > Storage / sizeof(SymRef) ||
          Count > Budget.available() / (2 + Words))
        return {};
      Storage -= Count * sizeof(SymRef);
      Budget.consume(Count * (2 + Words));
      Wide = Ctx.rebuild(V, Args);
    } else if (Op == SymOp::Extract) {
      auto Source = Ctx.operand(V, 0);
      const auto Offset = Ctx.node(V).Aux;
      const auto Size = Ctx.width(V);
      if (Ctx.op(Source) == SymOp::ZExt || Ctx.op(Source) == SymOp::SExt) {
        const auto Inner = Ctx.operand(Source, 0);
        if (Ctx.width(Inner) == Width && Offset + Size <= Width)
          Source = Inner;
      }
      Wide = Ctx.width(Source) == Width ? widenWindow(Ctx, Source, Offset, Size,
                                                      Width, Budget, Storage)
                                        : Ctx.mkZExt(V, Width);
      if (!Wide)
        return {};
    } else if (Ctx.isConst(V)) {
      Wide = Ctx.mkConst(Ctx.constValue(V).sext(Width));
    } else {
      Wide = Ctx.mkZExt(V, Width);
    }
    Widened.try_emplace(V.index(), Wide);
  }
  return Widened.lookup(Low.index());
}

// Lowering can mask both sides of a rotation to the bits used by a factor.
// The logical right shift fills exactly the bottom Cut bits. The two halves
// are disjoint, so a sum, OR and XOR yield the same observed prefix.
SymRef maskedRotatePrefix(SymContext &Ctx, SymRef Word, unsigned Bits,
                          WorkBudget &Budget, size_t &Storage) {
  const auto Op = Ctx.op(Word);
  if ((Op != SymOp::Or && Op != SymOp::Xor && Op != SymOp::Add) ||
      Ctx.numOperands(Word) != 2)
    return Word;
  auto High = Ctx.operand(Word, 0), Low = Ctx.operand(Word, 1);
  if (Ctx.op(High) != SymOp::Mul)
    std::swap(High, Low);
  if (Ctx.op(High) != SymOp::Mul || Ctx.numOperands(High) != 2 ||
      !Ctx.isConst(Ctx.operand(High, 0)) || Ctx.op(Low) != SymOp::And ||
      Ctx.numOperands(Low) != 2 || !Ctx.isConst(Ctx.operand(Low, 0)))
    return Word;
  const auto Left = Ctx.operand(High, 1), Right = Ctx.operand(Low, 1);
  if (Ctx.op(Left) != SymOp::And || !Ctx.isConst(Ctx.operand(Left, 0)) ||
      Ctx.op(Right) != SymOp::LShr || !Ctx.isConst(Ctx.operand(Right, 1)))
    return Word;
  const auto Base = Ctx.operand(Right, 0);
  const auto Tail = Ctx.operands(Left).drop_front();
  const unsigned Width = Ctx.width(Word);
  if (!Budget.consume(4 * llvm::APInt::getNumWords(Width) + Tail.size()))
    return {};
  if (Tail.size() == 1
          ? Tail.front() != Base
          : Ctx.op(Base) != SymOp::And || Tail != Ctx.operands(Base))
    return Word;
  const int32_t Cut = Ctx.constValue(Ctx.operand(High, 0)).exactLogBase2();
  const auto Shift = Ctx.operand(Right, 1);
  if (Cut <= 0 || unsigned(Cut) >= Bits ||
      Ctx.constWords(Shift).front() != Width - unsigned(Cut) ||
      Ctx.constValue(Ctx.operand(Left, 0)).countr_one() <
          Bits - unsigned(Cut) ||
      Ctx.constValue(Ctx.operand(Low, 0)).countr_one() < unsigned(Cut))
    return Word;
  const size_t Bytes = sizeof(SymNode) + 2 * sizeof(SymRef);
  if (Bytes > Storage)
    return {};
  Storage -= Bytes;
  return Ctx.mkRor(Base, Shift);
}

bool readTruncatedCoordinate(SymContext &Ctx, Factor &Row, unsigned Observed,
                             WorkBudget &Budget, size_t &Storage,
                             llvm::DenseMap<uint32_t, SymRef> &Widened) {
  const unsigned Width = Ctx.width(Row.Word);
  const size_t Words = llvm::APInt::getNumWords(Width);
  auto &Word = Row.Word;
  auto &Weight = Row.Weight;
  auto &Offset = Row.Offset;
  // Read nested affine coordinates without distributing the product. Only
  // the observed bits can survive multiplication by the outer coefficient.
  auto HasScalar = [&](SymRef V) {
    return Ctx.numOperands(V) == 2 && Ctx.isConst(Ctx.operand(V, 0));
  };
  auto Scaled = [&](SymRef V) {
    return (Ctx.op(V) == SymOp::Mul || Ctx.op(V) == SymOp::And) &&
           Ctx.numOperands(V) >= 2 && Ctx.isConst(Ctx.operand(V, 0));
  };
  // The neutral prefix depends on the consuming operator: zero for OR/XOR
  // and all ones for AND. A complemented zero prefix is an all-ones prefix.
  auto NeutralMask = [&](SymOp Op, SymRef Term) -> SymRef {
    if (Op == SymOp::And) {
      if (Ctx.op(Term) == SymOp::Or && Ctx.isConst(Ctx.operand(Term, 0)))
        return Ctx.operand(Term, 0);
      if (Ctx.op(Term) != SymOp::Not)
        return {};
      Term = Ctx.operand(Term, 0);
    }
    return Scaled(Term) ? Ctx.operand(Term, 0) : SymRef{};
  };
  unsigned Zeros = std::min(Observed, Row.Zeros);
  // A live bit in the first observed limb rules out dropping this term.
  // Leave longer matching prefixes to the complete check when rebuilding.
  auto MayDrop = [&](SymOp Op, SymRef Term) {
    const auto Mask = NeutralMask(Op, Term);
    if (!Mask)
      return false;
    auto Low = Ctx.constWords(Mask).front();
    if (Op == SymOp::And && Ctx.op(Term) == SymOp::Or)
      Low = ~Low;
    return unsigned(std::countr_zero(Low)) >= std::min(Observed - Zeros, 64u);
  };
  auto CanRead = [&] {
    const auto Op = Ctx.op(Word);
    if (Op == SymOp::Concat || Op == SymOp::ZExt || Op == SymOp::SExt)
      return true;
    if (HasScalar(Word) && (Op == SymOp::Add || Op == SymOp::Mul))
      return true;
    if (Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor) {
      auto Terms = Ctx.operands(Word);
      if (Ctx.isConst(Terms.front()))
        return true;
      // Plain binary words are common coordinates. Inspect them directly
      // before reserving work or storage for a reconstruction.
      if (Terms.size() == 2)
        return MayDrop(Op, Terms[0]) || MayDrop(Op, Terms[1]);
      if (!Budget.canConsume(Terms.size()))
        return false;
      Budget.consume(Terms.size());
      return llvm::any_of(Terms,
                          [&](SymRef T) { return bool(NeutralMask(Op, T)); });
    }
    if (Op != SymOp::Add)
      return false;
    auto Terms = Ctx.operands(Word);
    if (Terms.size() == 2)
      return MayDrop(Op, Terms[0]) || MayDrop(Op, Terms[1]);
    if (Terms.size() > Budget.available() / (1 + Words))
      return false;
    Budget.consume(Terms.size() * (1 + Words));
    return llvm::any_of(Terms, [&](SymRef T) {
      return Scaled(T) && !Ctx.constValue(Ctx.operand(T, 0))[0];
    });
  };
  // A direct splice needs no affine arithmetic. Keep the original weight
  // and offset when its low word has no further coordinate to read.
  if (const auto Low = lowOperand(Ctx, Word)) {
    if (!Budget.consume())
      return false;
    if (Ctx.width(Low) < Observed - Zeros)
      return true;
    const auto Wide =
        widenCoordinate(Ctx, Low, Width, Widened, Budget, Storage);
    if (!Wide)
      return false;
    if (Wide == Word)
      return true;
    Word = Wide;
  }
  if (CanRead()) {
    const size_t ReadWords = llvm::APInt::getNumWords(Observed);
    // The updated coefficient and offset are interned at the carrier width.
    const size_t ReadBytes =
        2 * (sizeof(llvm::APInt) + Words * sizeof(uint64_t));
    if (ReadBytes > Storage || !Budget.consume(2 + 2 * ReadWords))
      return false;
    Storage -= ReadBytes;
    auto A =
        llvm::APInt(Observed, Ctx.constWords(Offset).take_front(ReadWords));
    auto B =
        llvm::APInt(Observed, Ctx.constWords(Weight).take_front(ReadWords));
    do {
      const auto Op = Ctx.op(Word);
      if (HasScalar(Word) && (Op == SymOp::Add || Op == SymOp::Mul)) {
        const auto Digits =
            Ctx.constWords(Ctx.operand(Word, 0)).take_front(ReadWords);
        const uint64_t Low = Digits.front();
        const uint64_t Fill = uint64_t(0) - (Low >> 63);
        // Only the observed prefix determines whether a signed multiplier
        // fits one limb. The last stored limb may extend past that prefix.
        const bool Small =
            ReadWords > 1 &&
            ((Digits.back() ^ Fill) << ((64 - Observed % 64) % 64)) == 0 &&
            llvm::all_of(Digits.drop_front().drop_back(),
                         [&](uint64_t V) { return V == Fill; });
        if (Small) {
          const uint64_t Magnitude = Fill ? uint64_t(0) - Low : Low;
          if (!Budget.consume(1 + ReadWords + (Magnitude != 1 ? ReadWords : 0)))
            return false;
          if (Op == SymOp::Mul) {
            if (Magnitude != 1)
              B *= Magnitude;
            if (Fill)
              B.negate();
          } else if (Magnitude == 1) {
            if (Fill)
              A -= B;
            else
              A += B;
          } else {
            const auto Product = B * Magnitude;
            if (Fill)
              A -= Product;
            else
              A += Product;
          }
          if (Op == SymOp::Mul && !(Low & 1))
            Zeros += Low ? std::min(Observed - Zeros,
                                    unsigned(std::countr_zero(Low)))
                         : Observed - Zeros;
        } else {
          if (!Budget.consume(3 + ReadWords + ReadWords * ReadWords))
            return false;
          const llvm::APInt K(Observed, Digits);
          if (Op == SymOp::Add)
            A += B * K;
          else {
            B *= K;
            if (!(Low & 1))
              Zeros += std::min(Observed - Zeros, K.countr_zero());
          }
        }
        Word = Ctx.operand(Word, 1);
        continue;
      }
      // B annihilates every bit above this prefix. A literal XOR of
      // all ones in the prefix is -1 - word there.
      const unsigned Bits = Observed - Zeros;
      if (const auto Low = lowOperand(Ctx, Word)) {
        if (!Budget.consume())
          return false;
        if (Ctx.width(Low) < Bits)
          break;
        auto Wide = widenCoordinate(Ctx, Low, Width, Widened, Budget, Storage);
        if (!Wide)
          return false;
        if (Wide == Word)
          break;
        Word = Wide;
        if (Ctx.op(Word) == SymOp::ZExt)
          break;
        continue;
      }
      if (!Budget.consume(2 + 2 * Words))
        return false;
      if (Op != SymOp::Add && Ctx.isConst(Ctx.operand(Word, 0))) {
        const auto K = Ctx.constValue(Ctx.operand(Word, 0));
        const bool Flip = Op == SymOp::Xor && K.countr_one() >= Bits;
        const bool Identity =
            Op == SymOp::And ? K.countr_one() >= Bits : K.countr_zero() >= Bits;
        const auto Tail = Ctx.operands(Word).drop_front();
        if (Identity || Flip) {
          if (Tail.size() > Storage / (2 * sizeof(SymRef)) ||
              !Budget.consume(Tail.size()))
            return false;
          Storage -= Tail.size() * 2 * sizeof(SymRef);
          if (Flip) {
            A -= B;
            B.negate();
          }
          Word = Op == SymOp::And  ? Ctx.mkAnd(Tail)
                 : Op == SymOp::Or ? Ctx.mkOr(Tail)
                                   : Ctx.mkXor(Tail);
          continue;
        }
        // The literal changes a live bit. Keep it, but inspect the other
        // operands for a neutral prefix before reserving a new operand list.
        if (Tail.size() > 1 && !Budget.consume(Tail.size()))
          return false;
        if (llvm::none_of(Tail,
                          [&](SymRef T) { return bool(NeutralMask(Op, T)); }))
          break;
      }
      const auto Terms = Ctx.operands(Word);
      if (Terms.size() > Storage / (2 * sizeof(SymRef)) ||
          !Budget.consume(Terms.size() * (2 + Words)))
        return false;
      Storage -= Terms.size() * 2 * sizeof(SymRef);
      llvm::SmallVector<SymRef, 8> Live;
      for (auto Term : Terms) {
        if (Op == SymOp::Add && Ctx.isConst(Term)) {
          if (!Budget.consume(ReadWords * ReadWords))
            return false;
          A += B * llvm::APInt(Observed,
                               Ctx.constWords(Term).take_front(ReadWords));
          continue;
        }
        // Algebraic sums and OR/XOR discard a zero prefix. For AND, an OR
        // mask can prove an all-ones prefix, as can a complemented zero mask.
        const auto Mask = Op == SymOp::Add
                              ? (Scaled(Term) ? Ctx.operand(Term, 0) : SymRef{})
                              : NeutralMask(Op, Term);
        if (Mask) {
          const auto K = Ctx.constValue(Mask);
          const auto NeutralBits = Op == SymOp::And && Ctx.op(Term) == SymOp::Or
                                       ? K.countr_one()
                                       : K.countr_zero();
          if (NeutralBits >= Bits)
            continue;
        }
        Live.push_back(Term);
      }
      if (Live.size() == Terms.size())
        break;
      if (Live.empty())
        Word = Op == SymOp::And ? Ctx.mkOnes(Width) : Ctx.mkZero(Width);
      else
        Word = Ctx.rebuild(Word, Live);
    } while (CanRead());
    Offset = Ctx.mkConst(A.sextOrTrunc(Width));
    Weight = Ctx.mkConst(B.sextOrTrunc(Width));
    Row.Zeros = B.isZero() ? Width : B.countr_zero();
    Row.Content = std::min(Row.Zeros, A.isZero() ? Width : A.countr_zero());
  }
  // An instruction combiner may leave the word shifted and replace its
  // narrow extension with a mask. Its observable prefix has the same
  // coordinate as an explicit extract, including any copied rotate bits.
  // Constant left rotations use the same right-rotated representative.
  const unsigned Bits = Observed - std::min(Observed, Row.Zeros);
  if (Bits && Bits < Width &&
      (Ctx.op(Word) == SymOp::LShr || Ctx.op(Word) == SymOp::AShr ||
       Ctx.op(Word) == SymOp::Rol) &&
      Ctx.isConst(Ctx.operand(Word, 1))) {
    Word = widenWindow(Ctx, Word, 0, Bits, Width, Budget, Storage);
    if (!Word)
      return false;
  }
  if (Bits && Bits < Width) {
    Word = maskedRotatePrefix(Ctx, Word, Bits, Budget, Storage);
    if (!Word)
      return false;
  }
  // Rotations permute bits. If lowering distributed the same rotation
  // through a bitwise field, its unrotated operands are one coordinate.
  const auto Op = Ctx.op(Word);
  if ((Op == SymOp::Or || Op == SymOp::And || Op == SymOp::Xor) &&
      Ctx.op(Ctx.operand(Word, 0)) == SymOp::Ror) {
    const auto Parts = Ctx.operands(Word);
    const auto Count = Ctx.operand(Parts.front(), 1);
    if (!Budget.consume(Parts.size()))
      return false;
    if (llvm::all_of(Parts, [&](SymRef P) {
          return Ctx.op(P) == SymOp::Ror && Ctx.operand(P, 1) == Count;
        })) {
      const size_t Extra = 2 * sizeof(SymNode) + 4 * sizeof(SymRef);
      if (Storage < Extra)
        return false;
      const size_t MaxEdges = (Storage - Extra) / (4 * sizeof(SymRef));
      size_t Edges = 0;
      for (const auto P : Parts) {
        const auto Source = Ctx.operand(P, 0);
        const size_t N = Ctx.op(Source) == Op ? Ctx.numOperands(Source) : 1;
        if (N > MaxEdges - Edges)
          return false;
        Edges += N;
      }
      const size_t Work = 2 + std::bit_width(Edges);
      if (Edges > Budget.available() / Work)
        return false;
      Budget.consume(Edges * Work);
      Storage -= Extra + Edges * 4 * sizeof(SymRef);
      llvm::SmallVector<SymRef, 4> Sources;
      for (const auto P : Parts)
        Sources.push_back(Ctx.operand(P, 0));
      Word = Ctx.mkRor(Ctx.rebuild(Word, Sources), Count);
    }
  }
  Row.Complement = Ctx.op(Word) == SymOp::Not;
  if (Row.Complement)
    Word = Ctx.operand(Word, 0);
  return true;
}

SymRef emitTruncatedPolynomial(SymContext &Ctx, SymRef Word, SymRef Scale,
                               llvm::SmallVectorImpl<llvm::APInt> &Coefficients,
                               size_t Bytes, WorkBudget &Budget,
                               size_t &Storage) {
  const unsigned Width = Ctx.width(Word);
  const unsigned Precision = Coefficients.front().getBitWidth();
  const size_t Words = llvm::APInt::getNumWords(Width);
  auto Literal = [&](const llvm::APInt &C) {
    auto K = C.sextOrTrunc(Width);
    if (Scale)
      K *= Ctx.constValue(Scale);
    return Ctx.mkConst(K);
  };
  auto Horner = [&](llvm::ArrayRef<llvm::APInt> C) {
    auto V = Literal(C.back());
    for (size_t K = C.size() - 1; K != 0; --K)
      V = Ctx.mkAdd(Literal(C[K - 1]), Ctx.mkMul(Word, V));
    return V;
  };
  while (Coefficients.size() > 1 && Coefficients.back().isZero())
    Coefficients.pop_back();
  auto V = Horner(Coefficients);
  const size_t D = Coefficients.size() - 1;
  // The j-th Newton basis element is a product of j consecutive integers.
  // Its j! divisor removes j - popcount(j) coefficient bits at every word.
  const size_t FactorialZeros = D - std::popcount(D);
  if (D >= 2 &&
      Coefficients.back().countr_zero() >=
          Precision - std::min<size_t>(Precision, FactorialZeros) &&
      D + 1 <= Storage / Bytes &&
      D + 1 <= Budget.available() / (D + 1) / (2 * Words + 3)) {
    Budget.consume((D + 1) * (D + 1) * (2 * Words + 3));
    Storage -= (D + 1) * Bytes;
    // Successive synthetic divisions by (x - J) give the Newton coefficients.
    for (size_t J = 0; J <= D; ++J) {
      for (size_t K = D; J && K > J; --K)
        Coefficients[K - 1] += Coefficients[K] * J;
      const unsigned Bits =
          Precision - std::min<size_t>(Precision, J - std::popcount(J));
      auto &C = Coefficients[J];
      if (!Bits)
        C.clearAllBits();
      else if (Bits != Precision)
        C = C.trunc(Bits).sext(Precision);
    }
    while (Coefficients.size() > 1 && Coefficients.back().isZero())
      Coefficients.pop_back();
    // Multiply the surviving Newton terms back into the monomial basis.
    for (size_t J = Coefficients.size() - 1; J > 1; --J)
      for (size_t K = J - 1; K + 1 < Coefficients.size(); ++K)
        Coefficients[K] -= Coefficients[K + 1] * (J - 1);
    const auto Reduced = Horner(Coefficients);
    if (readingScore(Ctx, Reduced) < readingScore(Ctx, V))
      V = Reduced;
  }
  return V;
}

// Multiply affine factors modulo 2^Precision. The coefficient of x^k is
// divided by 2^(k*Shift) and only needs the remaining low limbs. The two
// rows are disjoint, so a partial product can accumulate into its destination
// without allocating an APInt for each multiplication and truncated operand.
void multiplyProjectedFactors(
    SymContext &Ctx, llvm::ArrayRef<Factor> Group, unsigned Precision,
    unsigned Shift, size_t Degree,
    llvm::SmallVectorImpl<llvm::APInt> &Coefficients) {
  const unsigned W = llvm::APInt::getNumWords(Precision);
  llvm::SmallVector<size_t, 8> Offsets{0};
  for (size_t K = 0; K <= Degree; ++K)
    Offsets.push_back(Offsets.back() +
                      llvm::APInt::getNumWords(Precision - K * Shift));
  llvm::SmallVector<uint64_t, 32> Rows(2 * Offsets.back(), 0);
  auto *Previous = Rows.data(), *Next = Previous + Offsets.back();
  Previous[0] = 1;
  const unsigned BW = llvm::APInt::getNumWords(Precision - Shift);
  llvm::SmallVector<uint64_t, 8> Quotient(BW);
  for (size_t J = 0; J < Group.size(); ++J) {
    const auto &F = Group[J];
    auto A = Ctx.constWords(F.Offset).take_front(W);
    auto B = Ctx.constWords(F.Weight).take_front(W);
    llvm::APInt ComplementOffset, ComplementWeight;
    if (F.Complement) {
      ComplementWeight = llvm::APInt(Precision, B);
      ComplementOffset = llvm::APInt(Precision, A) - ComplementWeight;
      ComplementWeight.negate();
      A = {ComplementOffset.getRawData(), W};
      B = {ComplementWeight.getRawData(), W};
    }
    // Digits beyond P-k*s cannot flow into a retained prefix. Read the
    // immutable offset in place and shift only the live digits of the weight.
    const unsigned Whole = Shift / 64, Part = Shift % 64;
    for (unsigned I = 0; I < BW; ++I) {
      Quotient[I] = B[I + Whole] >> Part;
      if (Part && I + Whole + 1 < W)
        Quotient[I] |= B[I + Whole + 1] << (64 - Part);
    }
    for (size_t K = 0; K <= std::min(Degree, J + 1); ++K) {
      const unsigned N = Offsets[K + 1] - Offsets[K];
      auto *Dst = Next + Offsets[K];
      for (unsigned I = 0; I < N; ++I) {
        // Higher digits cannot reach this coefficient. Overwrite on the
        // first digit, then add directly to the unfilled high end.
        llvm::APInt::tcMultiplyPart(Dst + I, Previous + Offsets[K], A[I], 0,
                                    N - I, N - I, I != 0);
        if (K)
          llvm::APInt::tcMultiplyPart(Dst + I, Previous + Offsets[K - 1],
                                      Quotient[I], 0, N - I, N - I, true);
      }
    }
    std::swap(Next, Previous);
  }
  for (size_t K = 0; K <= Degree; ++K) {
    const auto Bits = Precision - K * Shift;
    llvm::APInt C(Bits, llvm::ArrayRef(Previous + Offsets[K],
                                       Offsets[K + 1] - Offsets[K]));
    Coefficients.push_back(C.zextOrTrunc(Precision).shl(K * Shift));
  }
}

struct OffsetPower {
  SymRef Offset;
  unsigned Count = 0;
  llvm::APInt Product{1, 0}, Cofactor{1, 0};
};

size_t offsetPowerWork(unsigned Count, size_t Words, size_t LowProduct) {
  // Square and multiply for a^(m-1), then form a^m and update the full
  // product. The three prefix/suffix products use only the surviving limbs.
  const unsigned Exponent = Count - 1;
  const size_t Multiplies = 2 + std::popcount(Exponent) +
                            (Exponent ? std::bit_width(Exponent) - 1 : 0);
  return 6 + 4 * Words + Multiplies * Words * Words + 3 * LowProduct;
}

struct OffsetPowers {
  llvm::SmallDenseMap<uint32_t, unsigned, 8> ByOffset;
  llvm::SmallVector<OffsetPower, 8> Groups;

  bool count(SymRef Offset, size_t Limit, unsigned Multiplicity) {
    auto It = ByOffset.find(Offset.index());
    if (It != ByOffset.end()) {
      Groups[It->second].Count += Multiplicity;
      return true;
    }
    if (Groups.size() == Limit)
      return false;
    ByOffset.try_emplace(Offset.index(), Groups.size());
    Groups.push_back({Offset, Multiplicity});
    return true;
  }

  const llvm::APInt &cofactor(SymRef Offset) const {
    return Groups[ByOffset.lookup(Offset.index())].Cofactor;
  }
};

// Offsets occurring m times have the same leave-one-out product. Keep
// a^(m-1) even when a is even. Division by a would lose information.
void buildOffsetPowers(const SymContext &Ctx,
                       llvm::MutableArrayRef<OffsetPower> Groups,
                       unsigned Precision, unsigned Bits,
                       const llvm::APInt &Unit, llvm::APInt &Product) {
  const auto Words = llvm::APInt::getNumWords(Precision);
  for (auto &G : Groups) {
    llvm::APInt A(Precision, Ctx.constWords(G.Offset).take_front(Words));
    auto Others = llvm::APIntOps::pow(A, G.Count - 1);
    G.Cofactor = Product.trunc(Bits) * Others.trunc(Bits);
    Others *= A;
    G.Product = Others.trunc(Bits);
    Product *= Others;
  }
  auto Suffix = Unit.trunc(Bits);
  for (auto &G : llvm::reverse(Groups)) {
    G.Cofactor *= Suffix;
    Suffix *= G.Product;
  }
}

// If two perturbations cannot survive the outer scale, the coefficient of
// each word is its weight times all the other offsets. Prefix and suffix
// products avoid division, including when an offset is even or zero.
template <typename Coefficient>
SymRef buildLinearProduct(SymContext &Ctx, llvm::ArrayRef<Factor> Rows,
                          SymRef Outer, unsigned Precision, unsigned Shift,
                          WorkBudget &Budget, size_t &Storage,
                          llvm::ArrayRef<unsigned> Multiplicities = {},
                          llvm::ArrayRef<uint64_t> WeightSums = {}) {
  assert(Multiplicities.empty() || Multiplicities.size() == Rows.size());
  const unsigned Bits = Precision - Shift;
  constexpr bool Native = std::is_same_v<Coefficient, uint64_t>;
  assert(Bits && Shift >= Bits && Native == (Bits <= 64));
  const size_t Limbs = llvm::APInt::getNumWords(Bits);
  const size_t Words = llvm::APInt::getNumWords(Precision);
  assert(WeightSums.empty() || WeightSums.size() == Rows.size() * Words);
  const size_t PerRow = sizeof(Coefficient) +
                        4 * sizeof(std::pair<uint32_t, Coefficient>) +
                        (Native ? 0 : 5 * Limbs * sizeof(uint64_t));
  const unsigned Width = Ctx.width(Outer);
  const size_t WideWords = llvm::APInt::getNumWords(Width);
  const size_t FixedBytes =
      (4 * WideWords + 6 * Words + (Native ? 0 : 4 * Limbs)) * sizeof(uint64_t);
  const size_t FixedWork = 4 * WideWords + Words * Words;
  // Each coefficient needs three low products. The native case includes
  // those operations in its scalar cost.
  const size_t Work = 9 + 2 * Words + Words * Words +
                      (Native ? 0 : 3 * (Limbs * (Limbs + 1) / 2 - 1));
  const size_t LowProduct = Limbs * (Limbs + 1) / 2;
  const size_t GroupedRowWork =
      7 + Words + LowProduct + (Multiplicities.empty() ? 0 : 1 + Limbs);
  const bool Long = !Multiplicities.empty() ||
                    Rows.size() > limits::kMaxResidueProductFactors;
  if (FixedBytes > Storage || Rows.size() > (Storage - FixedBytes) / PerRow ||
      !Budget.canConsume(FixedWork))
    return {};
  const size_t Available = Budget.available() - FixedWork;
  if (Rows.size() > Available / (Long ? GroupedRowWork : Work))
    return {};
  const bool FlatFits =
      Multiplicities.empty() && Rows.size() <= Available / Work;
  const size_t Goal = FlatFits ? Rows.size() * Work : Available;
  // Include vector and hash bucket growth as well as the two stored low
  // residues per offset. The flat method remains available if a trial fails.
  const size_t GroupBytes = 4 * sizeof(OffsetPower) + 8 * sizeof(uint64_t) +
                            2 * Limbs * sizeof(uint64_t);
  const size_t RowBytes = FixedBytes + Rows.size() * PerRow;
  size_t GroupWork = Rows.size() * GroupedRowWork;
  const size_t MaxGroups =
      std::min((Storage - RowBytes) / GroupBytes,
               (Goal - GroupWork) / offsetPowerWork(1, Words, LowProduct));
  std::optional<OffsetPowers> Powers;
  if (Long && MaxGroups)
    Powers.emplace();
  if (!FlatFits && !Powers)
    return {};
  // Summed rows already name their field. Ordinary rows retain the complete
  // carrier. Count offsets only after checking the projected width.
  auto fieldOf = [&](const Factor &F) {
    return WeightSums.empty() ? lowOperand(Ctx, F.Word) : F.Word;
  };
  size_t Trial = 0;
  for (const auto &[I, F] : llvm::enumerate(Rows)) {
    const auto Low = fieldOf(F);
    if (!Low || Ctx.width(Low) != Bits) {
      Budget.consume(I + 1 + Trial);
      return {};
    }
    if (Powers) {
      ++Trial;
      const unsigned Count = Multiplicities.empty() ? 1 : Multiplicities[I];
      if (!Powers->count(F.Offset, MaxGroups, Count))
        Powers.reset();
    }
  }
  if (Powers) {
    for (const auto &G : Powers->Groups)
      GroupWork = llvm::SaturatingAdd(
          GroupWork, offsetPowerWork(G.Count, Words, LowProduct));
    if (GroupWork > Goal || (FlatFits && GroupWork == Goal))
      Powers.reset();
  }
  if (!Powers && (!FlatFits || Trial > Available - Goal)) {
    Budget.consume(Rows.size() + Trial);
    return {};
  }
  Budget.consume(FixedWork + (Powers ? GroupWork : Goal + Trial));
  Storage -= RowBytes + (Powers ? Powers->Groups.size() * GroupBytes : 0);

  auto Literal = [&](SymRef V) {
    return llvm::APInt(Precision, Ctx.constWords(V).take_front(Words));
  };
  auto Low = [&](const uint64_t *Digits) -> Coefficient {
    if constexpr (Native)
      return *Digits;
    else
      return llvm::APInt(Bits, llvm::ArrayRef(Digits, Limbs));
  };
  auto Value = [&](const Coefficient &C) -> llvm::APInt {
    if constexpr (Native)
      return llvm::APInt(Bits, C);
    else
      return C;
  };
  auto LiveWeight = [&](size_t I) -> Coefficient {
    const auto Digits = WeightSums.empty()
                            ? Ctx.constWords(Rows[I].Weight).take_front(Words)
                            : WeightSums.slice(I * Words, Words);
    const llvm::APInt Full(Precision, Digits);
    if constexpr (Native)
      return Full.extractBitsAsZExtValue(Bits, Shift);
    else
      return Full.extractBits(Bits, Shift);
  };
  llvm::SmallVector<Coefficient, 16> Prefix;
  llvm::APInt Product(Precision, 1);
  const auto Unit =
      Ctx.constValue(Outer).extractBits(Precision, Width - Precision);
  if (Powers) {
    buildOffsetPowers(Ctx, Powers->Groups, Precision, Bits, Unit, Product);
  } else {
    Prefix.reserve(Rows.size());
    for (const auto &F : Rows) {
      Prefix.push_back(Low(Product.getRawData()));
      Product *= Literal(F.Offset);
    }
  }
  // Once 2^Shift has been factored out, only Bits low bits of a
  // coefficient can survive. Unsigned limb arithmetic preserves all of them.
  llvm::SmallDenseMap<uint32_t, Coefficient, 8> Weights;
  Coefficient Suffix = Low(Unit.getRawData());
  Coefficient Correction = Low(llvm::APInt::getZero(Bits).getRawData());
  bool FoldedSignBit = false;
  for (size_t I = Rows.size(); I-- != 0;) {
    const auto &F = Rows[I];
    Coefficient C = LiveWeight(I);
    if (Powers) {
      C *= Low(Powers->cofactor(F.Offset).getRawData());
      if (WeightSums.empty() && !Multiplicities.empty() &&
          Multiplicities[I] != 1)
        C *= static_cast<uint64_t>(Multiplicities[I]);
    } else {
      C *= Prefix[I] * Suffix;
      Suffix *= Low(Ctx.constWords(F.Offset).data());
    }
    auto Word = fieldOf(F);
    if (Ctx.op(Word) == SymOp::Not) {
      Word = Ctx.operand(Word, 0);
      Correction += C;
      C = -C;
    }
    // Flipping the highest live bit adds half the modulus. Move this
    // constant into the offset before combining weights of the same field.
    if (Ctx.op(Word) == SymOp::Xor && Ctx.numOperands(Word) == 2 &&
        Ctx.isConst(Ctx.operand(Word, 0)) &&
        Ctx.constValue(Ctx.operand(Word, 0)).isSignMask()) {
      Word = Ctx.operand(Word, 1);
      Correction += C << (Bits - 1);
      FoldedSignBit = true;
    }
    auto [It, Inserted] = Weights.try_emplace(Word.index(), C);
    if (!Inserted)
      It->second += C;
  }
  Product *= Unit;
  Product -= Value(Correction).zext(Precision).shl(Shift);
  size_t Edges = 1;
  for (const auto &[Index, C] : Weights) {
    const SymRef V(Index);
    // A product may flatten a multiply. A unit coefficient may instead
    // expose an addition when the terms are collected.
    Edges += 1 + Ctx.numOperands(V);
  }
  const size_t PerEdge = 2 * sizeof(SymNode) + 5 * sizeof(SymRef);
  const size_t EdgeWork = 4 + std::bit_width(Edges);
  if (Edges > Storage / PerEdge || Edges > Budget.available() / EdgeWork)
    return {};
  Budget.consume(Edges * EdgeWork);
  Storage -= Edges * PerEdge;
  const auto Offset = Product.extractBits(Bits, Shift);
  llvm::SmallVector<SymRef, 8> Sum{Ctx.mkConst(Offset)};
  const bool MatchOffset = FoldedSignBit || !WeightSums.empty();
  SymRef Matched;
  size_t MatchIndex = 0;
  for (const auto &[Index, C] : Weights) {
    const auto K = Value(C);
    if (!K.isZero()) {
      if (MatchOffset && K == Offset) {
        Matched = SymRef(Index);
        MatchIndex = Sum.size();
      }
      Sum.push_back(Ctx.mkMul(Ctx.mkConst(K), SymRef(Index)));
    }
  }
  auto Upper = Ctx.mkAdd(Sum);
  if (MatchOffset && Sum.size() > 1 && (Matched || Offset.isAllOnes()) &&
      Edges <= Storage / PerEdge && Budget.canConsume(Edges * EdgeWork)) {
    Budget.consume(Edges * EdgeWork);
    Storage -= Edges * PerEdge;
    // An offset that matches a weight gives k + k*x = -k*~x. With
    // offset -1 the whole sum can be complemented.
    if (Matched)
      Sum[MatchIndex] = Ctx.mkMul(Ctx.mkConst(-Offset), Ctx.mkNot(Matched));
    else {
      Sum.resize(1);
      for (const auto &[Index, C] : Weights)
        if (auto K = Value(C); !K.isZero())
          Sum.push_back(Ctx.mkMul(Ctx.mkConst(-K), SymRef(Index)));
    }
    auto Alternative = Ctx.mkAdd(llvm::ArrayRef(Sum).drop_front());
    if (!Matched)
      Alternative = Ctx.mkNot(Alternative);
    if (readingScore(Ctx, Alternative) < readingScore(Ctx, Upper))
      Upper = Alternative;
  }
  // The bottom Shift bits are constant. The outer scale contributes the
  // remaining low zeros of the carrier, leaving just Bits arithmetic bits.
  const auto Tail =
      Product.trunc(Shift).zext(Width - Bits).shl(Width - Precision);
  return Ctx.mkConcat(Upper, Ctx.mkConst(Tail));
}

// The shared cofactor accounts for every occurrence, including equal factors
// whose unobserved upper bits differ. An even offset never needs division.
SymRef buildProjectedLinearProduct(SymContext &Ctx, llvm::ArrayRef<Factor> Rows,
                                   SymRef Outer, unsigned Precision,
                                   unsigned Shift, WorkBudget &Budget,
                                   size_t &Storage,
                                   llvm::ArrayRef<unsigned> Counts = {},
                                   llvm::ArrayRef<uint64_t> WeightSums = {}) {
  auto Build = Precision - Shift <= 64 ? buildLinearProduct<uint64_t>
                                       : buildLinearProduct<llvm::APInt>;
  return Build(Ctx, Rows, Outer, Precision, Shift, Budget, Storage, Counts,
               WeightSums);
}

// Isolate the high-weight subproduct. Every pair of its disturbances is zero
// modulo 2^Precision. The remaining factors multiply the complete sum, so
// their mixed terms and their sign or upper lanes remain observable.
SymRef buildMixedLinearProduct(SymContext &Ctx, llvm::ArrayRef<Factor> Rows,
                               SymRef Outer, unsigned Precision, unsigned Bits,
                               llvm::SmallVectorImpl<SymRef> &Parts,
                               WorkBudget &Budget, size_t &Storage) {
  using Key = std::tuple<uint32_t, uint32_t, uint32_t>;
  const size_t PerRow =
      2 * (sizeof(Factor) + sizeof(unsigned) + sizeof(SymRef)) +
      4 * sizeof(std::pair<Key, unsigned>);
  // Allow for the growing row arrays and hash buckets. The existing product
  // reader has already charged for its original factors and output operands.
  if (Rows.size() > Storage / PerRow || Rows.size() > Budget.available() / 3)
    return {};
  llvm::SmallDenseMap<Key, unsigned, 16> Index;
  llvm::SmallVector<Factor, 16> High;
  llvm::SmallVector<unsigned, 16> Counts;
  llvm::SmallVector<SymRef, 8> Remainder;
  const unsigned Half = Precision - Precision / 2;
  for (const auto &[I, F] : llvm::enumerate(Rows)) {
    if (F.Zeros < Half) {
      Remainder.push_back(F.Ref);
      continue;
    }
    const auto Field = projectedHighField(Ctx, F.Word, F.Zeros, Precision);
    if (!Field || Ctx.width(Field) != Bits) {
      Budget.consume(3 * (I + 1));
      return {};
    }
    const Key K{F.Offset.index(), F.Weight.index(), Field.index()};
    const auto [It, Inserted] = Index.try_emplace(K, High.size());
    if (Inserted) {
      High.push_back(F);
      Counts.push_back(1);
    } else {
      ++Counts[It->second];
    }
  }
  Budget.consume(3 * Rows.size());
  const auto Count = Rows.size() - Remainder.size();
  if (Count < limits::kMinResidueProductFactors || Count <= Remainder.size())
    return {};
  const size_t Temporary = Rows.size() * PerRow;
  Storage -= Temporary;
  const auto Sum = buildProjectedLinearProduct(
      Ctx, High, Outer, Precision, Precision - Bits, Budget, Storage,
      Count == High.size() ? llvm::ArrayRef<unsigned>{}
                           : llvm::ArrayRef(Counts));
  Storage += Temporary;
  if (Sum)
    Parts.append(Remainder.begin(), Remainder.end());
  return Sum;
}

// A row with weight b reads only Precision - ctz(b) low bits. Extracts
// starting at the same bit of the same source can therefore name a common
// coordinate even when their widths or the enclosing extensions differ.
SymRef buildSlicedLinearProduct(SymContext &Ctx, llvm::ArrayRef<Factor> Rows,
                                size_t RowCount, SymRef Outer,
                                unsigned Precision, unsigned Shift,
                                llvm::SmallVectorImpl<SymRef> &Parts,
                                WorkBudget &Budget, size_t &Storage) {
  using Key = std::tuple<uint32_t, uint32_t, uint32_t>;
  const unsigned Bits = Precision - Shift;
  const size_t Words = llvm::APInt::getNumWords(Precision);
  const size_t PerRow = 2 * (sizeof(Factor) + sizeof(unsigned) +
                             sizeof(SymRef) + Words * sizeof(uint64_t)) +
                        4 * sizeof(std::pair<Key, unsigned>);
  const size_t RowWork = 3 + Words;
  if (Rows.size() > Storage / PerRow ||
      Rows.size() > Budget.available() / RowWork)
    return {};
  llvm::SmallDenseMap<Key, unsigned, 16> Index;
  llvm::SmallVector<Factor, 16> Groups;
  llvm::SmallVector<unsigned, 16> Counts;
  llvm::SmallVector<uint64_t, 32> Weights;
  llvm::SmallVector<const Factor *, 8> Remainder;
  size_t Remaining = 0;
  size_t Temporary = Rows.size() * PerRow;
  Storage -= Temporary;
  const llvm::scope_exit Restore([&] { Storage += Temporary; });
  llvm::DenseMap<uint32_t, SymRef> Widened;
  using Coordinate = std::pair<uint32_t, unsigned>;
  llvm::SmallDenseMap<Coordinate, SymRef, 8> Wraps;
  Budget.consume(Rows.size() * RowWork);
  for (const auto &F : Rows) {
    if (F.Zeros < Shift) {
      Remainder.push_back(&F);
      Remaining += F.Copies;
      continue;
    }
    const auto Field = productField(Ctx, F.Word);
    auto Low = Field.Low;
    unsigned Start = Field.Start;
    auto Source = F.Word;
    const unsigned Live = Precision - std::min(Precision, F.Zeros);
    const bool Rotation =
        Ctx.op(Source) == SymOp::Rol || Ctx.op(Source) == SymOp::Ror;
    bool Circular = Rotation && Start;
    if (Field.Size < Live && !Circular) {
      // The complete carrier supplies any live sign or concatenated bits.
      Low = {};
      Start = 0;
    } else if (!Low) {
      if (Start)
        Source = Ctx.operand(F.Word, 0);
    } else if (Ctx.op(Low) == SymOp::Extract) {
      Source = Ctx.operand(Low, 0);
      Start = Ctx.node(Low).Aux;
      const auto Op = Ctx.op(Source);
      if ((Op == SymOp::Rol || Op == SymOp::Ror) && Ctx.node(Source).Aux) {
        Start = (uint64_t(Start) + Ctx.node(Source).Aux) % Ctx.width(Source);
        Source = Ctx.operand(Source, 0);
        Circular = true;
      }
    } else if (Ctx.op(Low) == SymOp::Add || Ctx.op(Low) == SymOp::Mul ||
               isBitwise(Ctx.op(Low))) {
      Source =
          widenCoordinate(Ctx, Low, Ctx.width(Outer), Widened, Budget, Storage);
      if (!Source)
        return {};
    } else {
      Source = Low;
    }
    // A higher weight can hide the wrap in one row while another offset
    // still observes it. All offsets must then use the same cyclic field.
    if (Circular && Live > Ctx.width(Source) - Start) {
      constexpr size_t Entry = 4 * sizeof(std::pair<Coordinate, SymRef>);
      if (!Budget.consume())
        return {};
      const Coordinate Position{Source.index(), Start};
      if (!Wraps.contains(Position)) {
        if (Storage < Entry)
          return {};
        Wraps.try_emplace(Position);
        Storage -= Entry;
        Temporary += Entry;
      }
    }
    const auto [It, Inserted] = Index.try_emplace(
        Key{F.Offset.index(), Source.index(), Start}, Groups.size());
    const size_t I = It->second;
    if (Inserted) {
      Groups.push_back(F);
      Groups.back().Word = {};
      Counts.push_back(0);
      Weights.resize(Weights.size() + Words, 0);
    }
    if (Low && Ctx.width(Low) == Bits)
      Groups[I].Word = Low;
    Counts[I] += F.Copies;
    // All these rows have the same offset and coordinate. Their cofactor
    // is shared even for an even offset. Add the weights without division.
    auto *Sum = Weights.data() + I * Words;
    const auto *Weight = Ctx.constWords(F.Weight).data();
    if (F.Copies == 1) {
      llvm::APInt::tcAdd(Sum, Weight, 0, Words);
    } else {
      if (!Budget.consume(Words))
        return {};
      llvm::APInt::tcMultiplyPart(Sum, Weight, F.Copies, 0, Words, Words, true);
    }
  }
  const auto Count = RowCount - Remaining;
  if (Count < limits::kMinResidueProductFactors || Count <= Remaining)
    return {};
  for (const auto &[K, I] : Index) {
    const SymRef Source(std::get<1>(K));
    const unsigned Start = std::get<2>(K);
    auto Wrap = Wraps.find({Source.index(), Start});
    const bool Crosses = Wrap != Wraps.end();
    if (Crosses)
      Groups[I].Word = Wrap->second;
    if (Groups[I].Word)
      continue;
    // A low extract can rebuild modular children. The printed tree count
    // also bounds repeated edges of a variadic source.
    const unsigned Available =
        std::min(Bits, Ctx.width(Source) - (Crosses ? 0 : Start));
    // When the observed slice reaches the top, its missing bits have zero
    // coefficient. A zero extension gives the group a common representative.
    const size_t Nodes = llvm::SaturatingAdd(Ctx.readabilityCost(Source),
                                             size_t(Available < Bits));
    const size_t Bytes = 2 * sizeof(SymNode) + 4 * sizeof(SymRef);
    if (Nodes > Storage / Bytes || !Budget.consume(2 * Nodes + 1))
      return {};
    Storage -= Nodes * Bytes;
    SymRef Input = Source;
    if (Crosses) {
      const size_t W = llvm::APInt::getNumWords(Ctx.width(Source));
      const size_t Extra =
          2 * sizeof(SymNode) + 2 * sizeof(SymRef) + 2 * W * sizeof(uint64_t);
      if (Storage < Extra || !Budget.consume(2 + W))
        return {};
      Storage -= Extra;
      Input = Ctx.mkRor(Source, Ctx.mkConst(Ctx.width(Source), Start));
    }
    const auto Field = Ctx.mkExtract(Input, Crosses ? 0 : Start, Available);
    Groups[I].Word = Available == Bits ? Field : Ctx.mkZExt(Field, Bits);
    if (Crosses)
      Wrap->second = Groups[I].Word;
  }
  const auto Sum = buildProjectedLinearProduct(
      Ctx, Groups, Outer, Precision, Shift, Budget, Storage,
      Count == Groups.size() ? llvm::ArrayRef<unsigned>{}
                             : llvm::ArrayRef(Counts),
      Weights);
  if (Sum) {
    if (!Budget.consume(Remaining - Remainder.size()))
      return {};
    for (const auto *Row : Remainder)
      Parts.append(Row->Copies, Row->Ref);
  }
  return Sum;
}

SymRef buildTruncatedGroup(SymContext &Ctx, llvm::ArrayRef<Factor> Group,
                           unsigned Precision, SymRef Scale, WorkBudget &Budget,
                           size_t &Storage) {
  // The d smallest valuations bound every coefficient of degree d.
  size_t Degree = 0;
  unsigned Bits = 0;
  for (size_t J = Group.size(); J != 0; --J) {
    if (Group[J - 1].Zeros >= Precision - Bits)
      break;
    Bits += Group[J - 1].Zeros;
    ++Degree;
  }
  const size_t Used = Group.size();
  const size_t Words = llvm::APInt::getNumWords(Ctx.width(Group.front().Word));
  const size_t Bytes =
      sizeof(llvm::APInt) + 4 * sizeof(SymRef) + 5 * Words * sizeof(uint64_t);
  if (Degree >= Used || Used < 2 || (Degree + 1) > Storage / Bytes)
    return {};
  const size_t W = llvm::APInt::getNumWords(Precision);
  const size_t Step = 3 + W + 2 * W * W;
  size_t Work = llvm::SaturatingMultiply(
      Used, llvm::SaturatingMultiply(Degree + 1, Step));
  unsigned Shift = 0;
  const unsigned MinimumZeros = Group.back().Zeros;
  // Every coefficient of x^k contains 2^(k*s) when every weight contains
  // 2^s. Keep its quotient at P-k*s bits if that saves word arithmetic.
  if (W > 1 && Degree && MinimumZeros &&
      llvm::APInt::getNumWords(Precision - Degree * MinimumZeros) < W) {
    // A low N-limb product visits N*(N+1)/2 digit pairs. Degree zero
    // has one product; every higher coefficient accumulates two.
    size_t PerFactor = 2 + llvm::APInt::getNumWords(Precision - MinimumZeros) +
                       W * (W + 1) / 2;
    for (size_t K = 1; K <= Degree; ++K) {
      const size_t KW = llvm::APInt::getNumWords(Precision - K * MinimumZeros);
      PerFactor = llvm::SaturatingAdd(PerFactor, 2 + 2 * KW + KW * (KW + 1));
    }
    const size_t Complements =
        llvm::count_if(Group, [](const Factor &F) { return F.Complement; });
    const size_t Projected = llvm::SaturatingMultiplyAdd(
        Used, PerFactor + 1,
        llvm::SaturatingMultiplyAdd(
            Complements, 3 * W, llvm::SaturatingMultiply(Degree, 2 + 2 * W)));
    if (Projected < Work) {
      Work = Projected;
      Shift = MinimumZeros;
    }
  }
  if (!Budget.canConsume(Work))
    return {};
  Budget.consume(Work);
  Storage -= (Degree + 1) * Bytes;
  llvm::SmallVector<llvm::APInt, 8> Coefficients;
  Coefficients.reserve(Degree + 1);
  if (Shift) {
    multiplyProjectedFactors(Ctx, Group, Precision, Shift, Degree,
                             Coefficients);
  } else {
    for (size_t K = 0; K <= Degree; ++K)
      Coefficients.emplace_back(Precision, 0);
    Coefficients[0] = 1;
    for (size_t J = 0; J < Used; ++J) {
      const auto &F = Group[J];
      auto A = llvm::APInt(Precision, Ctx.constWords(F.Offset).take_front(W));
      auto B = llvm::APInt(Precision, Ctx.constWords(F.Weight).take_front(W));
      if (F.Complement) {
        A -= B;
        B.negate();
      }
      for (size_t K = std::min(Degree, J + 1); K != 0; --K)
        Coefficients[K] = A * Coefficients[K] + B * Coefficients[K - 1];
      Coefficients[0] *= A;
    }
  }
  return emitTruncatedPolynomial(Ctx, Group.front().Word, Scale, Coefficients,
                                 Bytes, Budget, Storage);
}

} // namespace

SymRef projectedHighField(const SymContext &Ctx, SymRef Word, unsigned Zeros,
                          unsigned Precision) {
  const auto Low = lowOperand(Ctx, Word);
  if (!Low)
    return {};
  const unsigned Bits = Ctx.width(Low);
  return Bits <= Precision / 2 && Zeros < Precision && Zeros >= Precision - Bits
             ? Low
             : SymRef{};
}

unsigned slicedProductFieldWidth(const SymContext &Ctx, SymRef Perturbation) {
  if (!Perturbation || Ctx.op(Perturbation) != SymOp::Mul ||
      Ctx.numOperands(Perturbation) != 2)
    return 0;
  const auto Field = productField(Ctx, Ctx.operand(Perturbation, 1));
  if (!Field.Low)
    return Field.Size;
  const auto Op = Ctx.op(Field.Low);
  return Op == SymOp::Extract || Op == SymOp::Add || Op == SymOp::Mul ||
                 isBitwise(Op)
             ? Field.Size
             : 0;
}

bool hasJoinedLowField(const SymContext &Ctx, SymRef Perturbation,
                       unsigned Bits) {
  if (!Perturbation || Ctx.op(Perturbation) != SymOp::Mul ||
      Ctx.numOperands(Perturbation) != 2)
    return false;
  const auto Field = lowOperand(Ctx, Ctx.operand(Perturbation, 1));
  return Field && Ctx.width(Field) == Bits && Ctx.numOperands(Field) > 2 &&
         (Ctx.op(Field) == SymOp::Xor || Ctx.op(Field) == SymOp::Or);
}

SymRef foldTruncatedProduct(SymContext &Ctx, SymRef Root, unsigned Observed,
                            WorkBudget &Budget, size_t &Storage,
                            bool *EmittedReading) {
  const unsigned Width = Ctx.width(Root);
  const size_t N = Ctx.numOperands(Root);
  const size_t PerFactor = sizeof(Factor) + 3 * sizeof(SymRef);
  if (N > Storage / PerFactor || N > Budget.available() / 2)
    return Root;
  Storage -= N * PerFactor;
  const size_t FirstGenerated = Ctx.numNodes();
  llvm::SmallVector<Factor, 16> Rows;
  llvm::SmallVector<SymRef, 16> Parts;
  unsigned TotalZeros = 0, MinimumZeros = Observed, LowerZeros = 0;
  const unsigned Half = Observed - Observed / 2;
  size_t FirstHigh = N;
  unsigned HighZeros = Observed, FirstFieldBits = 0;
  bool NeedsSlices = false;
  size_t HighFields = 0, RowCount = 0;
  const auto Outer =
      Ctx.isConst(Ctx.operand(Root, 0)) ? Ctx.operand(Root, 0) : SymRef{};
  for (size_t I = bool(Outer); I < N; ++I) {
    if (!Budget.consume(2))
      return Root;
    const auto F = Ctx.operand(Root, I);
    if (Ctx.op(F) != SymOp::Add || Ctx.numOperands(F) != 2 ||
        !Ctx.isConst(Ctx.operand(F, 0))) {
      Parts.push_back(F);
      continue;
    }
    const auto D = Ctx.operand(F, 1);
    if (Ctx.op(D) != SymOp::Mul || !Ctx.isConst(Ctx.operand(D, 0))) {
      Parts.push_back(F);
      continue;
    }
    auto Weight = Ctx.operand(D, 0);
    auto Offset = Ctx.operand(F, 0);
    // ctz(weight | offset) is their common power of two. Read both
    // literals together only as far as the first nonzero weight limb.
    // Content may stop earlier; the final limb may be partial.
    const auto Weights = Ctx.constWords(Weight),
               Offsets = Ctx.constWords(Offset);
    unsigned Zeros = 0, InitialContent = 0;
    for (size_t J = 0; J < Weights.size(); ++J) {
      if (!Budget.consume())
        return Root;
      const auto Remaining = Width - Zeros;
      if (InitialContent == Zeros)
        InitialContent += std::min(
            Remaining, unsigned(std::countr_zero(Weights[J] | Offsets[J])));
      Zeros += std::min(Remaining, unsigned(std::countr_zero(Weights[J])));
      if (Weights[J])
        break;
    }
    // Identical factors in a canonical product are consecutive. Their word,
    // weight and live window need decoding just once. Retain every factor for
    // later polynomial readers. Even factors keep the zero-prefix early exit.
    const size_t First = I;
    if (!InitialContent)
      while (I + 1 < N && Ctx.operand(Root, I + 1) == F)
        ++I;
    const size_t Count = I - First + 1;
    if (Count > 1 && !Budget.consume(Count - 1))
      return Root;
    MinimumZeros = std::min(MinimumZeros, Zeros);
    if (Zeros < Half)
      LowerZeros +=
          std::min<uint64_t>(Observed - LowerZeros, uint64_t(Zeros) * Count);
    else {
      HighZeros = std::min(HighZeros, Zeros);
      if (FirstHigh == N)
        FirstHigh = RowCount;
    }
    TotalZeros += std::min(Observed - TotalZeros, InitialContent);
    if (TotalZeros == Observed)
      return Ctx.mkZero(Width);
    const auto Tail = Ctx.operands(D).drop_front();
    if (Tail.size() > Storage / (3 * sizeof(SymRef)) ||
        !Budget.consume(Tail.size()))
      return Root;
    Storage -= Tail.size() * 3 * sizeof(SymRef);
    auto Word = Tail.size() == 1 ? Tail.front() : Ctx.mkMul(Tail);
    if (Outer && Zeros >= Half) {
      const auto Field = productField(Ctx, Word);
      if (Field.Size && Field.Size >= Observed - std::min(Observed, Zeros)) {
        HighFields += Count;
        NeedsSlices |=
            Field.Start || (FirstFieldBits && Field.Size != FirstFieldBits);
        FirstFieldBits = Field.Size;
      }
    }
    RowCount += Count;
    Rows.push_back({F, Word, Weight, Offset, Zeros, InitialContent,
                    static_cast<unsigned>(Count), false});
  }
  auto Finish = [&]() {
    const auto Candidate = Ctx.mkMul(Parts);
    if (!(readingScore(Ctx, Candidate) < readingScore(Ctx, Root)))
      return Root;
    recordResidueReading(Ctx, FirstGenerated, Budget, Storage);
    if (EmittedReading)
      *EmittedReading = true;
    return Candidate;
  };
  const bool Linear = Outer && MinimumZeros < Observed &&
                      MinimumZeros >= Observed - MinimumZeros;
  SymRef LinearPart;
  if (NeedsSlices && HighFields > RowCount / 2 && HighZeros < Observed &&
      RowCount >= limits::kMinResidueProductFactors)
    LinearPart = buildSlicedLinearProduct(Ctx, Rows, RowCount, Outer, Observed,
                                          HighZeros, Parts, Budget, Storage);
  if (!LinearPart && RowCount != Rows.size()) {
    // The general polynomial readers consume individual factors in their
    // original order. Expand only when a counted slice did not finish.
    const size_t Unique = Rows.size();
    if (!Budget.consume(RowCount - Unique))
      return Root;
    Rows.resize_for_overwrite(RowCount);
    size_t End = RowCount;
    for (size_t I = Unique; I-- != 0;) {
      const auto F = Rows[I];
      End -= F.Copies;
      std::fill_n(Rows.begin() + End, F.Copies, F);
    }
  }
  if (!LinearPart && Linear) {
    LinearPart = buildProjectedLinearProduct(Ctx, Rows, Outer, Observed,
                                             MinimumZeros, Budget, Storage);
  } else if (!LinearPart && Outer && LowerZeros < Observed &&
             FirstHigh < Rows.size() &&
             Rows.size() >= limits::kMinResidueProductFactors) {
    // If the lower-weight disturbances can themselves vanish together, let
    // the per-coordinate polynomial reader below reduce those groups too.
    const auto &F = Rows[FirstHigh];
    if (const auto Field = projectedHighField(Ctx, F.Word, F.Zeros, Observed))
      LinearPart = buildMixedLinearProduct(
          Ctx, Rows, Outer, Observed, Ctx.width(Field), Parts, Budget, Storage);
  }
  if (LinearPart) {
    Parts.push_back(LinearPart);
    return Finish();
  }
  // Admit the complete product's content before reading coordinates.
  // Once that prefix proves zero, none of their alternate spellings matter.
  llvm::DenseMap<uint32_t, SymRef> Widened;
  for (auto &Row : Rows) {
    const unsigned InitialContent = Row.Content;
    // Other factors annihilate high bits here too: instruction combining
    // can place the outer scale inside one offset instead of on the product.
    const unsigned LiveBits = Observed - (TotalZeros - InitialContent);
    if (!readTruncatedCoordinate(Ctx, Row, LiveBits, Budget, Storage, Widened))
      return Root;
    TotalZeros += std::min(Observed - TotalZeros, Row.Content - InitialContent);
    if (TotalZeros == Observed)
      return Ctx.mkZero(Width);
  }
  // A zero product needs only the prefix that proved it. Charge the sort
  // after that proof opportunity and only for the factors that will be sorted.
  const size_t SortWork = 2 + std::bit_width(Rows.size());
  if (Rows.size() > Budget.available() / SortWork)
    return Root;
  Budget.consume(Rows.size() * SortWork);
  llvm::sort(Rows, [](const Factor &A, const Factor &B) {
    return A.Word != B.Word ? A.Word < B.Word : A.Zeros > B.Zeros;
  });

  // The general group polynomial also covers a linear product with more
  // than one surviving coefficient limb. Otherwise a repeated middle word
  // must lose its highest-degree coefficient before this reader applies.
  bool TruncatesMiddle = Linear;
  for (size_t I = 0; !TruncatesMiddle && I < Rows.size();) {
    unsigned Bits = 0;
    size_t End = I;
    for (; End < Rows.size() && Rows[End].Word == Rows[I].Word; ++End)
      if (Rows[End].Zeros < Observed / 2 + Observed % 2)
        Bits += std::min(Observed - Bits, Rows[End].Zeros);
    TruncatesMiddle |= Bits == Observed;
    I = End;
  }
  if (!TruncatesMiddle)
    return Root;

  bool Changed = false;
  for (size_t I = 0; I < Rows.size();) {
    size_t End = I + 1;
    while (End < Rows.size() && Rows[End].Word == Rows[I].Word)
      ++End;
    unsigned OwnZeros = 0;
    for (size_t J = I; J < End; ++J)
      OwnZeros += Rows[J].Content;
    // Other factors annihilate the high bits of this polynomial. Its own
    // shared zeros survive reduction because the complete product is nonzero.
    const unsigned Precision = Observed - (TotalZeros - OwnZeros);
    const auto Group = llvm::ArrayRef(Rows).slice(I, End - I);
    if (const auto V =
            buildTruncatedGroup(Ctx, Group, Precision,
                                Changed ? SymRef{} : Outer, Budget, Storage)) {
      Parts.push_back(V);
      Changed = true;
    } else {
      for (const auto &F : Group)
        Parts.push_back(F.Ref);
    }
    I = End;
  }
  if (!Changed)
    return Root;
  return Finish();
}

} // namespace neverd::symbolic::detail
