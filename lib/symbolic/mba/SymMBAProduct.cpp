//===- SymMBAProduct.cpp - Recovering a product from its expansion --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the search for a product of bitwise factors whose expansion over
/// the minterm basis is exactly the higher-degree part SymMBAPoly.cpp computed,
/// and the rewrite of a region as one such product plus a linear remainder.
///
/// See the theory note at the head of SymMBAPoly.cpp for why the expansion pins
/// a product down where the corner measurement cannot.
///
//===----------------------------------------------------------------------===//

#include "Limits.h"
#include "SymMBADetail.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <optional>
#include <utility>

namespace neverd::symbolic::detail {

namespace {

/// Multiply \p Factors out and count how often each monomial is reached, or
/// report that they cannot be the product being looked for.
///
/// Failing at the first monomial the target does not name is what keeps the
/// search far cheaper than its bound: almost every candidate is wrong, and
/// almost every wrong one is wrong on its first monomial.
bool expandProduct(llvm::ArrayRef<TruthTable> Factors,
                   const HigherCoefficients &Higher,
                   std::unordered_map<Monomial, unsigned, MonomialHash> &Counts,
                   WorkBudget &Budget) {
  Counts.clear();
  llvm::SmallVector<llvm::SmallVector<MintermIndex, 8>, 4> Selected;
  for (const TruthTable &Table : Factors) {
    Selected.push_back(selectedMinterms(Table));
    if (Selected.back().empty())
      return false;
  }

  bool Explains = true;
  forEachMonomial(Selected, Budget, [&](const Monomial &Key) {
    if (!Higher.count(Key)) {
      Explains = false;
      return false;
    }
    ++Counts[Key];
    return true;
  });
  // Reaching only monomials the target names is not enough; it has to reach all
  // of them, or the target holds something this product does not account for.
  return !Budget.exhausted() && Explains && Counts.size() == Higher.size();
}

/// Visit every product of \p Degree bitwise factors whose expansion is exactly
/// \p Higher, until \p Budget is spent.
///
/// The search is over factors rather than over all functions of the inputs,
/// because the target says a great deal about what the factors must be.  A
/// factor can only select a minterm the target names, and between them the
/// factors must name all of them; and a minterm repeated to the full degree is
/// a monomial exactly when *every* factor selects it, so those are known before
/// the search starts.  What is left to guess is a subset of the remainder,
/// which is what brings a degree-three product over three inputs -- the shape
/// polynomial obfuscation reaches for -- down from an unaffordable enumeration
/// to a few tens of thousands of candidates.
///
/// More than one product can match, and which reads best is not the order they
/// are found in, so the caller ranks every match reached within the budget.
/// Combination generation is iterative: product degree changes memory use, not
/// call-stack depth.
template <typename FnT>
void forEachProductMatch(const HigherCoefficients &Higher, unsigned Degree,
                         unsigned NumAtoms, uint32_t Width,
                         size_t MaxCandidates, size_t MaxBytes,
                         WorkBudget &Budget, FnT Visit) {
  const std::optional<size_t> Corners = cornerCount(NumAtoms);
  if (!Corners || Degree < 2)
    return;

  TruthTable Active = TruthTable::zero(NumAtoms);
  for (const auto &[Key, Coeff] : Higher)
    Active |= monomialSupport(Key, NumAtoms);

  TruthTable Shared = TruthTable::zero(NumAtoms);
  for (size_t M = 0; M < *Corners; ++M) {
    Monomial Repeat(Degree, static_cast<MintermIndex>(M));
    if (Higher.count(Repeat))
      Shared.set(M);
  }

  if (Shared.isZero() && Degree <= 4) {
    if (auto Support =
            recoverProductSupport(Higher, Degree, NumAtoms, Budget, MaxBytes)) {
      if (Support->empty())
        return;
      // Support recovery proposes the only factor supports. Check every
      // coefficient, including modular zeros absent from the input, separately.
      llvm::SmallVector<llvm::SmallVector<MintermIndex, 8>, 4> Choices;
      const size_t EntryBytes = 128 + 2 * Degree * sizeof(MintermIndex);
      size_t ProductSize = 1;
      for (const auto &Factor : *Support) {
        Choices.push_back(selectedMinterms(Factor));
        if (Choices.back().empty() ||
            ProductSize > MaxBytes / EntryBytes / Choices.back().size())
          return;
        ProductSize *= Choices.back().size();
      }
      if (!Budget.canConsume(ProductSize))
        return;
      std::unordered_map<Monomial, unsigned, MonomialHash> Counts;
      forEachMonomial(Choices, Budget, [&](const Monomial &Key) {
        ++Counts[Key];
        return true;
      });
      if (Budget.exhausted())
        return;
      const llvm::APInt *Scale = nullptr;
      for (const auto &[Key, Count] : Counts) {
        if (!Budget.consume(Degree + 1))
          return;
        if (Count == 1) {
          auto It = Higher.find(Key);
          if (It == Higher.end())
            return;
          Scale = &It->second;
          break;
        }
      }
      if (!Scale)
        return;
      size_t Found = 0;
      const size_t Words = (size_t(Width) - 1) / 64 + 1;
      for (const auto &[Key, Count] : Counts) {
        if (!Budget.consume(Degree + 1 + Words * Words))
          return;
        auto Expected = *Scale * llvm::APInt(Width, Count, false, true);
        auto It = Higher.find(Key);
        if (It == Higher.end() ? !Expected.isZero() : It->second != Expected)
          return;
        Found += It != Higher.end();
      }
      if (Found == Higher.size())
        Visit(*Scale, *Support);
      return;
    }
  }
  llvm::SmallVector<TruthTable, 64> Candidates;
  const TruthTable Free = Active & ~Shared;
  if (Degree >= 3) {
    // No candidate is visited until this list is complete. Do not construct
    // an unaffordable prefix only to discard it without testing a product.
    const size_t FreeBits = Free.count();
    if (FreeBits >= std::numeric_limits<size_t>::digits)
      return;
    const size_t Count = (size_t(1) << FreeBits) - unsigned(Shared.isZero());
    if (Degree > 3 && (Count > MaxCandidates || !Budget.canConsume(Count)))
      return;

    // An anchor fixes the scale. Its adjacent coefficients must be the scale
    // times a factor-membership count. This is only a necessary condition:
    // candidates still need their complete expansion and independent proof.
    std::optional<MintermIndex> Anchor;
    const llvm::APInt *Scale = nullptr;
    for (size_t M = 0; M < *Corners; ++M) {
      if (!Shared.at(M))
        continue;
      if (!Budget.consume(Degree + 1))
        return;
      const auto &Value = Higher.at(Monomial(Degree, MintermIndex(M)));
      if (!Anchor) {
        Anchor = M;
        Scale = &Value;
      } else if (Value != *Scale) {
        return;
      }
    }
    if (Anchor) {
      const size_t Words = (size_t(Width) - 1) / 64 + 1;
      const unsigned ScaleBits = Width - Scale->countr_zero();
      const bool UniqueCounts =
          ScaleBits >= 32 || Degree < (uint64_t(1) << ScaleBits);
      // Pair constraints need unique membership counts and bounded arithmetic.
      // Declining this optional pruning never grants a product match.
      constexpr size_t CornerBytes =
          2 * sizeof(MintermIndex) + sizeof(unsigned);
      const size_t Scratch = 256 + 2 * size_t(Degree) * sizeof(MintermIndex) +
                             4 * Words * sizeof(uint64_t);
      const bool CheckPairs = Degree <= 64 && UniqueCounts &&
                              Scratch <= MaxBytes &&
                              *Corners <= (MaxBytes - Scratch) / CornerBytes &&
                              Budget.canConsume(*Corners);
      std::vector<unsigned> Membership;
      if (CheckPairs) {
        if (!Budget.consume(*Corners))
          return;
        Membership.assign(*Corners, Degree);
      }
      for (size_t M = 0; M < *Corners; ++M) {
        if (!Active.at(M) || M == *Anchor)
          continue;
        if (!Budget.consume(Degree + 1))
          return;
        Monomial Key(Degree, *Anchor);
        Key.back() = M;
        llvm::sort(Key);
        auto It = Higher.find(Key);
        const auto Value =
            It == Higher.end() ? llvm::APInt(Width, 0) : It->second;
        llvm::APInt Expected(Width, 0);
        bool Possible = false;
        for (unsigned N = 1; N <= Degree; ++N) {
          if (!Budget.consume(Words))
            return;
          Expected += *Scale;
          if (Expected == Value) {
            Possible = true;
            if (CheckPairs)
              Membership[M] = N;
          }
        }
        if (!Possible)
          return;
      }
      if (CheckPairs) {
        // The a^(d-2)*m*n coefficient counts ordered placements in distinct
        // factors: |m|*|n| - |m intersect n|. Intersection bounds reject an
        // impossible product without enumerating its factor subsets. For m=n,
        // unordered placements instead give choose(|m|, 2).
        const auto Selected = selectedMinterms(Active);
        for (size_t I = 0; I != Selected.size(); ++I) {
          const auto M = Selected[I];
          if (M == *Anchor)
            continue;
          for (size_t J = I; J != Selected.size(); ++J) {
            const auto N = Selected[J];
            if (N == *Anchor)
              continue;
            if (!Budget.consume(Degree + Words + 1))
              return;
            Monomial Key(Degree, *Anchor);
            Key[Degree - 2] = M;
            Key[Degree - 1] = N;
            llvm::sort(Key);
            auto It = Higher.find(Key);
            const auto Value =
                It == Higher.end() ? llvm::APInt(Width, 0) : It->second;
            const unsigned A = Membership[M], B = Membership[N];
            const unsigned Lower =
                M == N ? A * (A - 1) / 2 : A * B - std::min(A, B);
            const unsigned Upper =
                M == N ? Lower : A * B - (A + B > Degree ? A + B - Degree : 0);
            bool Possible = false;
            for (unsigned Count = Lower; Count <= Upper; ++Count) {
              if (!Budget.consume(Words))
                return;
              Possible |=
                  *Scale * llvm::APInt(Width, Count, false, true) == Value;
            }
            if (!Possible)
              return;
          }
        }
      }
      if (!productCoefficientsFitScale(Higher, Degree, *Scale, Budget))
        return;
    }
    if (!Budget.canConsume(Count))
      return;
  }
  for (TruthTable Sub = Free;; Sub = Sub.nextSubsetBelow(Free)) {
    TruthTable Table = Shared | Sub;
    if (!Table.isZero()) {
      if (!Budget.consume())
        return;
      // A candidate is a table of one bit per corner and the walk below is
      // over tuples of them, so a list nobody bounds is how a wide region
      // turns a search into an allocation failure rather than a long wait.
      if (Candidates.size() >= MaxCandidates)
        return;
      Candidates.push_back(std::move(Table));
    }
    if (Sub.isZero())
      break;
  }
  if (Candidates.empty())
    return;

  llvm::SmallVector<size_t, 4> Choice(Degree, 0);
  llvm::SmallVector<TruthTable, 4> Factors(Degree);
  std::unordered_map<Monomial, unsigned, MonomialHash> Counts;
  // Nondecreasing tuples, because a product does not care what order its
  // factors are written in.
  for (;;) {
    if (!Budget.consume())
      return;
    TruthTable Union = TruthTable::zero(NumAtoms);
    for (size_t I = 0; I < Degree; ++I) {
      Factors[I] = Candidates[Choice[I]];
      Union |= Factors[I];
    }

    if (Union == Active && expandProduct(Factors, Higher, Counts, Budget)) {
      // Pin the coefficient on any monomial the product reaches exactly once.
      // Where every monomial is reached more than once no single coefficient
      // can be read off. Every coefficient is checked below, so choosing a
      // different singleton cannot change whether a product matches.
      const llvm::APInt *Coeff = nullptr;
      for (const auto &[Key, Count] : Counts)
        if (Count == 1) {
          Coeff = &Higher.find(Key)->second;
          break;
        }
      bool Matches = Coeff && !Coeff->isZero();
      if (Matches) {
        for (const auto &[Key, Count] : Counts) {
          llvm::APInt Predicted =
              *Coeff * llvm::APInt(Width, Count, /*isSigned=*/false,
                                   /*implicitTrunc=*/true);
          if (Predicted != Higher.find(Key)->second) {
            Matches = false;
            break;
          }
        }
      }
      if (Matches && !Visit(*Coeff, Factors))
        return;
    }

    if (Budget.exhausted())
      return;

    size_t Pos = Degree;
    while (Pos > 0 && Choice[Pos - 1] + 1 == Candidates.size())
      --Pos;
    if (Pos == 0)
      return;
    const size_t Next = Choice[Pos - 1] + 1;
    for (size_t I = Pos - 1; I < Degree; ++I) {
      Choice[I] = Next;
    }
  }
}

} // namespace

std::optional<llvm::SmallVector<TruthTable, 4>>
recoverProductSupport(const HigherCoefficients &Higher, unsigned Degree,
                      unsigned NumAtoms, WorkBudget &Budget, size_t MaxBytes) {
  const auto Corners = cornerCount(NumAtoms);
  if (!Corners || Higher.empty() || Degree < 2 || Degree > 8 ||
      *Corners > MaxBytes / 16 || !Budget.consume(*Corners))
    return std::nullopt;
  TruthTable Active = TruthTable::zero(NumAtoms);
  for (const auto &[Key, Coefficient] : Higher) {
    if (!Budget.consume(Degree + 1) || Key.size() != Degree ||
        !llvm::is_sorted(Key) || Key.back() >= *Corners || Coefficient.isZero())
      return std::nullopt;
    for (auto M : Key)
      Active.set(M);
  }
  const auto Selected = selectedMinterms(Active);
  if (Selected.size() > 8)
    return std::nullopt;
  const size_t Subsets = size_t(1) << Selected.size();
  const size_t PerTerm = (size_t(1) << Degree) + Degree * Selected.size();
  const size_t TailWork = Subsets * (2 * Selected.size() + Degree + 2);
  if (MaxBytes < 512 || *Corners > (MaxBytes - 512) / (Degree + 8))
    return std::nullopt;
  const size_t Scratch = 512 + *Corners * (Degree + 8);
  if (Subsets >
          (MaxBytes - Scratch) / (3 * sizeof(unsigned) + sizeof(int64_t)) ||
      Higher.size() >
          (std::numeric_limits<size_t>::max() - TailWork) / PerTerm ||
      !Budget.canConsume(Higher.size() * PerTerm + TailWork))
    return std::nullopt;

  // The largest degree in variables from S counts factors intersecting S.
  // A lexicographic vertex attaining it has multiplicity one, so modular
  // cancellation cannot hide that degree, even for an even/high-bit scale.
  std::vector<unsigned> Maximum(Subsets, 0), Sum(Subsets, 0), Mask(Subsets, 0);
  for (const auto &[Key, Coefficient] : Higher) {
    if (!Budget.consume(PerTerm))
      return std::nullopt;
    std::array<unsigned, 8> Counts{}, Positions{};
    unsigned Used = 0;
    for (size_t I = 0; I != Key.size(); ++I) {
      if (I && Key[I] == Key[I - 1]) {
        ++Counts[Used - 1];
        continue;
      }
      Positions[Used] = llvm::lower_bound(Selected, Key[I]) - Selected.begin();
      Counts[Used++] = 1;
    }
    // Only subsets of this monomial can contribute a new degree. A maximum
    // subset transform below extends these observations to the full domain.
    for (size_t S = 1; S != (size_t(1) << Used); ++S) {
      const auto Bit = std::countr_zero(S);
      const auto Rest = S & (S - 1);
      Sum[S] = Sum[Rest] + Counts[Bit];
      Mask[S] = Mask[Rest] | (1u << Positions[Bit]);
      Maximum[Mask[S]] = std::max(Maximum[Mask[S]], Sum[S]);
    }
  }
  if (!Budget.consume(TailWork))
    return std::nullopt;
  for (size_t Bit = 1; Bit < Subsets; Bit <<= 1)
    for (size_t S = 0; S != Subsets; ++S)
      if (S & Bit)
        Maximum[S] = std::max(Maximum[S], Maximum[S ^ Bit]);
  // d-rank(complement(S)) counts factors wholly contained in S. Boolean
  // lattice inversion recovers how many factors have each exact support.
  std::vector<int64_t> Multiplicity(Subsets);
  for (size_t S = 0; S != Subsets; ++S)
    Multiplicity[S] = Degree - Maximum[(Subsets - 1) ^ S];
  for (size_t Bit = 1; Bit < Subsets; Bit <<= 1)
    for (size_t S = 0; S != Subsets; ++S)
      if (S & Bit)
        Multiplicity[S] -= Multiplicity[S ^ Bit];
  llvm::SmallVector<TruthTable, 4> Factors;
  unsigned Total = 0;
  for (size_t S = 0; S != Subsets; ++S) {
    auto Count = Multiplicity[S];
    if (Count < 0 || Count > Degree - Total || (!S && Count))
      return Factors;
    Total += Count;
  }
  if (Total != Degree)
    return Factors;
  for (size_t S = 1; S != Subsets; ++S) {
    if (!Multiplicity[S])
      continue;
    auto Table = TruthTable::zero(NumAtoms);
    for (size_t Bit = 0; Bit != Selected.size(); ++Bit)
      if (S & (size_t(1) << Bit))
        Table.set(Selected[Bit]);
    for (int64_t I = 0; I != Multiplicity[S]; ++I)
      Factors.push_back(Table);
  }
  return Factors;
}

bool productCoefficientsFitScale(const HigherCoefficients &Higher,
                                 unsigned Degree, const llvm::APInt &Scale,
                                 WorkBudget &Budget) {
  if (Scale.isZero())
    return false;
  const unsigned Width = Scale.getBitWidth();
  const unsigned Zeros = Scale.countr_zero(), Bits = Width - Zeros;
  const size_t Words = (size_t(Width) - 1) / 64 + 1;
  if (Words > (std::numeric_limits<size_t>::max() - Words) / (2 * Words) ||
      !Budget.consume(4 * Words))
    return false;
  auto Odd = Scale.lshr(Zeros).zextOrTrunc(Bits);
  llvm::APInt Inverse(Bits, 1);
  // Newton lifting doubles the known low bits of an odd modular inverse.
  for (unsigned Known = 1;
       Known<Bits; Known = Known> Bits / 2 ? Bits : 2 * Known) {
    if (!Budget.consume(2 * Words * Words + Words))
      return false;
    Inverse *= llvm::APInt(Bits, 2, false, true) - Odd * Inverse;
  }
  for (const auto &[Key, Coefficient] : Higher) {
    if (!Budget.consume(Key.size()) ||
        !Budget.consume(Words * Words + 2 * Words + 1) ||
        Key.size() != Degree || Coefficient.getBitWidth() != Width ||
        Coefficient.countr_zero() < Zeros)
      return false;
    // A sorted monomial can be reached at most d! / product(repeats!) times.
    // Saturation only declines this optional rejection; it grants no match.
    uint64_t Bound = 1, Repeat = 0;
    bool Bounded = true;
    for (size_t I = 0; I != Key.size(); ++I) {
      Repeat = I && Key[I] == Key[I - 1] ? Repeat + 1 : 1;
      if (Bound > std::numeric_limits<uint64_t>::max() / (I + 1)) {
        Bounded = false;
        break;
      }
      Bound = Bound * (I + 1) / Repeat;
    }
    if (!Bounded)
      continue;
    auto Count = Coefficient.lshr(Zeros).zextOrTrunc(Bits) * Inverse;
    if (Count.getActiveBits() > 64 || Count.getZExtValue() > Bound)
      return false;
  }
  return true;
}

std::optional<QuadraticProduct>
recoverQuadraticProduct(const HigherCoefficients &Higher, unsigned NumAtoms,
                        uint32_t Width, WorkBudget &Budget, size_t MaxBytes) {
  const auto Corners = cornerCount(NumAtoms);
  // Three bitsets, the selected indices, container overhead and APInt scratch.
  // Reserve before constructing any table, including for unlimited-work
  // callers.
  const size_t Words = Width ? (size_t(Width) - 1) / 64 + 1 : 0;
  constexpr size_t Overhead = 256;
  if (!Width || Higher.empty() || !Corners || MaxBytes < Overhead ||
      Words > (MaxBytes - Overhead) / (4 * sizeof(uint64_t)))
    return std::nullopt;
  const size_t Scratch = Overhead + Words * 4 * sizeof(uint64_t);
  if (*Corners > (MaxBytes - Scratch) / (2 * sizeof(MintermIndex) + 1) ||
      !Budget.consume(*Corners))
    return std::nullopt;

  TruthTable Active = TruthTable::zero(NumAtoms);
  for (const auto &[Key, Coeff] : Higher) {
    if (!Budget.consume(2 + Words) || Key.size() != 2 || Key[0] > Key[1] ||
        Key[1] >= *Corners || Coeff.getBitWidth() != Width || Coeff.isZero())
      return std::nullopt;
    Active.set(Key[0]);
    Active.set(Key[1]);
  }
  const auto Selected = selectedMinterms(Active);
  QuadraticProduct Result{
      llvm::APInt(Width, 0),
      {TruthTable::zero(NumAtoms), TruthTable::zero(NumAtoms)}};
  auto &Left = Result.Factors[0];
  auto &Right = Result.Factors[1];
  std::optional<MintermIndex> Anchor;
  for (MintermIndex M : Selected) {
    if (!Budget.consume(1 + Words))
      return std::nullopt;
    if (auto It = Higher.find(Monomial{M, M}); It != Higher.end()) {
      // A diagonal occurs exactly once iff both factors select this minterm.
      // It fixes the scale even when twice that scale wraps to zero.
      if (Result.Coefficient.isZero())
        Result.Coefficient = It->second;
      Left.set(M);
      Right.set(M);
    } else if (!Anchor) {
      Anchor = M;
    }
  }
  if (Result.Coefficient.isZero()) {
    // With no diagonal the factors are disjoint; every nonzero off-diagonal
    // coefficient is the scale. Pick in minterm order for stable work counts.
    for (MintermIndex M : Selected) {
      if (!Budget.consume(1 + Words))
        return std::nullopt;
      auto It = Higher.find(Monomial{Selected.front(), M});
      if (It != Higher.end()) {
        Result.Coefficient = It->second;
        break;
      }
    }
  }
  if (Result.Coefficient.isZero())
    return std::nullopt;
  if (Anchor) {
    // Every remaining minterm belongs to just one factor. Relative to one
    // anchor, an edge means the other factor and no edge means the same one.
    // Swapping the two factors is the only other possible assignment.
    for (MintermIndex M : Selected) {
      if (!Budget.consume())
        return std::nullopt;
      if (Left.at(M))
        continue;
      Monomial Edge{std::min(M, *Anchor), std::max(M, *Anchor)};
      (Higher.count(Edge) ? Right : Left).set(M);
    }
  }

  // Check the entire matrix, including absent edges and modular zeros.
  // Matching existing edges alone could accept a disconnected or incomplete
  // bipartite graph. This also rejects inconsistent scales and diagonals.
  for (size_t I = 0; I != Selected.size(); ++I) {
    const auto A = Selected[I];
    for (size_t J = I; J != Selected.size(); ++J) {
      if (!Budget.consume(1 + Words))
        return std::nullopt;
      const auto B = Selected[J];
      unsigned Count = unsigned(Left.at(A) && Right.at(B));
      if (I != J)
        Count += unsigned(Left.at(B) && Right.at(A));
      llvm::APInt Expected = Count == 2   ? Result.Coefficient.shl(1)
                             : Count == 1 ? Result.Coefficient
                                          : llvm::APInt(Width, 0);
      auto It = Higher.find(Monomial{A, B});
      if (It == Higher.end() ? !Expected.isZero() : It->second != Expected)
        return std::nullopt;
    }
  }
  return Result;
}

std::optional<CubicProduct>
recoverCubicProduct(const HigherCoefficients &Higher, unsigned NumAtoms,
                    uint32_t Width, WorkBudget &Budget, size_t MaxBytes,
                    bool *Determined) {
  if (Determined)
    *Determined = false;
  const auto Corners = cornerCount(NumAtoms);
  const size_t Words = Width ? (size_t(Width) - 1) / 64 + 1 : 0;
  constexpr size_t Overhead = 512;
  // Four tables, membership counts, selected indices (including vector
  // capacity growth) and APInt temporaries.
  constexpr size_t CornerBytes = 8 * sizeof(MintermIndex) + 2;
  if (!Width || Higher.empty() || !Corners || MaxBytes < Overhead ||
      Words > (MaxBytes - Overhead) / (4 * sizeof(uint64_t)))
    return std::nullopt;
  size_t Scratch = Overhead + Words * 4 * sizeof(uint64_t);
  if (*Corners > (MaxBytes - Scratch) / CornerBytes ||
      !Budget.consume(*Corners))
    return std::nullopt;
  Scratch += *Corners * CornerBytes;

  TruthTable Active = TruthTable::zero(NumAtoms);
  std::optional<MintermIndex> Anchor;
  llvm::APInt Scale(Width, 0);
  for (const auto &[Key, Coeff] : Higher) {
    if (!Budget.consume(3 + Words) || Key.size() != 3 || Key[0] > Key[1] ||
        Key[1] > Key[2] || Key[2] >= *Corners || Coeff.getBitWidth() != Width ||
        Coeff.isZero())
      return std::nullopt;
    for (auto M : Key)
      Active.set(M);
    if (Key[0] == Key[2] && (!Anchor || Key[0] < *Anchor)) {
      Anchor = Key[0];
      Scale = Coeff;
    }
  }
  // The coefficient of a^3 is the scale when all three factors select a.
  // Distinguishing membership counts 0..3 requires additive order >= four.
  if (!Anchor || (Scale + Scale).isZero())
    return std::nullopt;

  if (Determined)
    *Determined = true;

  auto coefficient = [&](MintermIndex A, MintermIndex B, MintermIndex C) {
    Monomial Key{A, B, C};
    llvm::sort(Key);
    auto It = Higher.find(Key);
    return It == Higher.end() ? llvm::APInt(Width, 0) : It->second;
  };
  auto scaled = [&](unsigned Count) {
    return Scale * llvm::APInt(Width, Count, false, true);
  };
  const auto Selected = selectedMinterms(Active);
  std::vector<uint8_t> Membership(*Corners, 0);
  CubicProduct Result{Scale,
                      {TruthTable::zero(NumAtoms), TruthTable::zero(NumAtoms),
                       TruthTable::zero(NumAtoms)}};
  llvm::SmallVector<MintermIndex, 3> Representatives;
  for (auto M : Selected) {
    if (!Budget.consume(1 + 4 * Words))
      return std::nullopt;
    unsigned Count = M == *Anchor ? 3 : 0;
    if (!Count) {
      // The coefficient of a^2*m counts factors selecting m.
      auto Value = coefficient(*Anchor, *Anchor, M);
      for (unsigned N = 1; N <= 3; ++N)
        if (Value == scaled(N))
          Count = N;
    }
    if (!Count)
      return std::nullopt;
    Membership[M] = Count;
    unsigned Class = 0;
    if (Count != 3) {
      // A singleton identifies its selected factor; a pair identifies its
      // omitted factor. The coefficient of a*m*n distinguishes equal and
      // unequal identities using c*(|m|*|n| - |m intersect n|).
      for (; Class < Representatives.size(); ++Class) {
        if (!Budget.consume(1 + 3 * Words))
          return std::nullopt;
        auto N = Representatives[Class];
        unsigned Other = Membership[N];
        auto Value = coefficient(*Anchor, M, N);
        unsigned Same = Count * Other - (Count == Other ? Count : 0);
        unsigned Different = Count * Other - unsigned(Count == 2 || Other == 2);
        if (Value == scaled(Same))
          break;
        if (Value != scaled(Different))
          return std::nullopt;
      }
      if (Class == Representatives.size()) {
        if (Class == 3)
          return std::nullopt;
        Representatives.push_back(M);
      }
    }
    for (unsigned F = 0; F != 3; ++F)
      if (Count == 3 || (Count == 1 ? F == Class : F != Class))
        Result.Factors[F].set(M);
  }

  // The anchor slice only proposes the factors. Expand all of them, including
  // monomials absent from the input and multiplicities vanishing modulo 2^w.
  // Bound the count map before allocating; a partial match is never a result.
  llvm::SmallVector<llvm::SmallVector<MintermIndex, 8>, 3> Choices;
  constexpr size_t EntryBytes =
      sizeof(Monomial) + sizeof(unsigned) + 6 * sizeof(void *);
  size_t ProductSize = 1;
  for (const auto &Factor : Result.Factors) {
    Choices.push_back(selectedMinterms(Factor));
    if (Choices.back().empty() ||
        ProductSize > (MaxBytes - Scratch) / EntryBytes / Choices.back().size())
      return std::nullopt;
    ProductSize *= Choices.back().size();
  }
  if (!Budget.canConsume(ProductSize))
    return std::nullopt;
  std::unordered_map<Monomial, unsigned, MonomialHash> Counts;
  forEachMonomial(Choices, Budget, [&](const Monomial &Key) {
    ++Counts[Key];
    return true;
  });
  if (Budget.exhausted())
    return std::nullopt;
  size_t Found = 0;
  for (const auto &[Key, Count] : Counts) {
    if (!Budget.consume(1 + Words))
      return std::nullopt;
    auto Expected = scaled(Count);
    auto It = Higher.find(Key);
    if (It == Higher.end() ? !Expected.isZero() : It->second != Expected)
      return std::nullopt;
    Found += It != Higher.end();
  }
  return Found == Higher.size() ? std::optional(std::move(Result))
                                : std::nullopt;
}

std::optional<SymRef> solvePolynomial(SymContext &Ctx, SymRef Body,
                                      llvm::ArrayRef<uint32_t> AtomIds,
                                      llvm::ArrayRef<SymRef> Atoms,
                                      const MBAOptions &Opts,
                                      WorkBudget &Budget) {
  // The caller's synthesis arity is a budget: tabulating a factor over this
  // many inputs is what it agreed to pay for.  Minterm indices use the same
  // 32-bit range as the truth-table container, so no narrower storage ceiling
  // silently removes the polynomial reading.
  const SolverLimits Limits = resolveLimits(Opts);
  const auto NumAtoms = static_cast<unsigned>(AtomIds.size());
  if (NumAtoms > Limits.MaxSynthesisAtoms)
    return std::nullopt;
  const std::optional<size_t> Corners = cornerCount(NumAtoms);
  if (!Corners)
    return std::nullopt;

  std::optional<llvm::SmallVector<PolyTerm, 8>> Terms =
      splitIntoTerms(Ctx, Body);
  if (!Terms)
    return std::nullopt;
  // The expansion cannot contribute a candidate without a nonlinear term.
  // Check its factors before allocating and evaluating whole minterm tables.
  if (std::none_of(Terms->begin(), Terms->end(), [](const PolyTerm &Term) {
        return Term.Factors.size() > 1;
      }))
    return std::nullopt;

  const uint32_t Width = Ctx.width(Body);
  // A product contributes monomials of exactly its own degree. Split before
  // expansion so a degree's minterm basis contains only its own inputs. A
  // linear opaque mask otherwise doubles every factor's table and multiplies
  // a cubic expansion's work by eight, although it cannot change that product.
  std::map<unsigned, llvm::SmallVector<PolyTerm, 8>> ByDegree;
  llvm::SmallVector<PolyTerm, 8> LinearTerms;
  for (auto &Term : *Terms)
    (Term.Factors.size() < 2 ? LinearTerms : ByDegree[Term.Factors.size()])
        .push_back(std::move(Term));

  // The linear tail has its own support too. Inputs used only by products
  // duplicate every coefficient in this table and can make a small linear
  // reading exceed its synthesis allowance. Project before expanding, keeping
  // the caller's coordinate order and the complete polynomial proof below.
  llvm::SmallVector<uint32_t, 16> LinearVariables, LinearIds;
  for (const auto &Term : LinearTerms)
    if (!Term.Factors.empty()) {
      const SymRef Factor = Term.Factors.front();
      if (!Budget.consume(Ctx.dagSize(Factor)))
        return std::nullopt;
      if (Ctx.op(Factor) == SymOp::Var)
        LinearVariables.push_back(Ctx.varId(Factor));
      else
        Ctx.collectVars(Factor, LinearVariables);
    }
  llvm::SmallVector<SymRef, 16> LinearAtoms;
  for (size_t I = 0; I != AtomIds.size(); ++I)
    if (llvm::is_contained(LinearVariables, AtomIds[I])) {
      LinearIds.push_back(AtomIds[I]);
      LinearAtoms.push_back(Atoms[I]);
    }
  auto LinearForm =
      expandOverMinterms(Ctx, LinearTerms, LinearIds, Width, Budget);
  if (!LinearForm)
    return std::nullopt;

  const size_t TermCeiling = termBudget(Ctx, Body, Opts);
  llvm::SmallVector<SymRef, 4> Linear;
  if (LinearAtoms.empty())
    Linear.push_back(Ctx.mkConst(-LinearForm->Linear.front()));
  else
    linearCandidates(Ctx, std::move(LinearForm->Linear), LinearAtoms,
                     TermCeiling, Limits, Linear);
  SymRef Remainder = cheapestOf(Ctx, Linear);
  if (!Remainder.isValid())
    return std::nullopt;

  const BitwiseSynthesisLimits Synthesis = Limits.synthesis(TermCeiling);
  llvm::SmallVector<SymRef, 4> Parts;
  for (auto &[SourceDegree, DegreeTerms] : ByDegree) {
    auto Common =
        extractPolynomialCofactor(DegreeTerms, Budget, Opts.MaxTableBytes);
    unsigned Degree = SourceDegree - Common.size();
    auto Append = [&](SymRef Part) {
      if (Common.empty()) {
        Parts.push_back(Part);
        return true;
      }
      const auto Terms = Ctx.op(Part) == SymOp::Add
                             ? Ctx.operands(Part)
                             : llvm::ArrayRef<SymRef>(Part);
      const size_t MaxSlots = Opts.MaxTableBytes / (4 * sizeof(SymRef));
      size_t Slots = Terms.size();
      if (Slots > MaxSlots)
        return false;
      for (SymRef Term : Terms) {
        const size_t Count =
            Common.size() +
            (Ctx.op(Term) == SymOp::Mul ? Ctx.numOperands(Term) : 1);
        if (Count > MaxSlots - Slots)
          return false;
        Slots += Count;
      }
      if (!Budget.consume(Slots))
        return false;
      // Own the sum's operands before interning can move the arena. Charge
      // the restored common edges as well as the small quotient's own edges.
      llvm::SmallVector<SymRef, 8> Owned(Terms), Summands;
      for (SymRef Term : Owned) {
        llvm::SmallVector<SymRef, 8> Factors(Common);
        Factors.push_back(Term);
        Summands.push_back(Ctx.mkMul(Factors));
      }
      Parts.push_back(Ctx.mkAdd(Summands));
      return true;
    };
    auto Retain = [&]() {
      const size_t MaxSlots = Opts.MaxTableBytes / (2 * sizeof(SymRef));
      size_t Slots = DegreeTerms.size();
      if (Slots > MaxSlots)
        return false;
      for (const auto &Term : DegreeTerms) {
        if (Term.Factors.size() + Common.size() > MaxSlots - Slots)
          return false;
        Slots += Term.Factors.size() + Common.size();
      }
      if (!Budget.consume(Slots))
        return false;
      llvm::SmallVector<SymRef, 8> Summands;
      for (const auto &Term : DegreeTerms) {
        llvm::SmallVector<SymRef, 8> Factors(Term.Factors.begin(),
                                             Term.Factors.end());
        Factors.append(Common);
        Factors.push_back(Ctx.mkConst(Term.Coeff));
        Summands.push_back(Ctx.mkMul(Factors));
      }
      Parts.push_back(Ctx.mkAdd(Summands));
      return true;
    };
    // An unchanged, already factored higher-degree component needs no factor
    // search. Preserve its exact coefficient and multiplicities while other
    // degrees improve. The caller still proves the complete recombination.
    if (Degree >= 3 && DegreeTerms.size() == 1) {
      if (!Retain())
        return std::nullopt;
      continue;
    }
    llvm::SmallVector<uint32_t, 16> DegreeIds;
    llvm::DenseSet<uint32_t> Seen;
    for (const auto &Term : DegreeTerms)
      for (SymRef Factor : Term.Factors)
        if (Seen.insert(Factor.index()).second) {
          if (!Budget.consume(Ctx.dagSize(Factor)))
            return std::nullopt;
          Ctx.collectVars(Factor, DegreeIds);
        }
    // Factoring can expose a previously unaffordable search. Keep the original
    // bounded reading for wider residuals; the row reconstruction handles
    // broad quadratic quotients, while cubic enumeration stays on few inputs.
    if (!Common.empty() &&
        (Degree > limits::kMaxCofactorSearchDegree ||
         (Degree == 3 &&
          DegreeIds.size() > limits::kMaxCubicCofactorSearchAtoms))) {
      for (SymRef Factor : Common) {
        if (!Budget.consume(Ctx.dagSize(Factor)))
          return std::nullopt;
        Ctx.collectVars(Factor, DegreeIds);
      }
      for (auto &Term : DegreeTerms) {
        Term.Factors.append(Common);
        llvm::sort(Term.Factors);
      }
      Common.clear();
      Degree = SourceDegree;
    }
    llvm::SmallVector<SymRef, 16> DegreeAtoms;
    for (uint32_t Id : DegreeIds)
      DegreeAtoms.push_back(Ctx.varRef(Id));
    // More than one product can match, and which reads best is not the order
    // they are found in, so every match reached within the budget is ranked.
    // Ranking the product alone rather than the whole sum is the same order:
    // the remainder is the same term under every candidate.
    SymRef Best;
    SymReadability BestCost;
    auto Consider = [&](const llvm::APInt &Coeff,
                        llvm::ArrayRef<TruthTable> Match,
                        llvm::ArrayRef<SymRef> KnownFactors = {}) {
      llvm::SmallVector<SymRef, 8> Factors;
      Factors.reserve(Match.size() + 1);
      Factors.push_back(Ctx.mkConst(Coeff));
      for (size_t I = 0; I != Match.size(); ++I) {
        auto FactorLimits = Synthesis;
        if (!KnownFactors.empty())
          FactorLimits.MaxCost =
              std::min(FactorLimits.MaxCost, readingCost(Ctx, KnownFactors[I]));
        std::optional<SymRef> Written =
            synthesizeBitwise(Ctx, Match[I], DegreeAtoms, FactorLimits);
        // Single-term recovery retains these exact input tables. Their known
        // spelling bounds synthesis and remains available when an optional
        // cover cannot improve it. The complete result is still proved later.
        if (!KnownFactors.empty() &&
            (!Written ||
             readingScore(Ctx, KnownFactors[I]) < readingScore(Ctx, *Written)))
          Written = KnownFactors[I];
        if (!Written)
          return true;
        Factors.push_back(*Written);
      }
      SymRef Candidate = Ctx.mkMul(Factors);
      const SymReadability Cost = readingScore(Ctx, Candidate);
      if (!Best.isValid() || Cost < BestCost) {
        Best = Candidate;
        BestCost = Cost;
      }
      return true;
    };
    if (Degree == 2) {
      bool Determined = false;
      auto Product =
          recoverQuadraticTerms(Ctx, DegreeTerms, DegreeIds, Width, Budget,
                                Opts.MaxTableBytes, Determined);
      if (Product) {
        Consider(Product->Coefficient, Product->Factors,
                 DegreeTerms.size() == 1
                     ? llvm::ArrayRef<SymRef>(DegreeTerms.front().Factors)
                     : llvm::ArrayRef<SymRef>{});
        if (!Best.isValid())
          return std::nullopt;
        if (!Append(Best))
          return std::nullopt;
        continue;
      }
      if (Determined && SourceDegree >= 3 && !Budget.exhausted() && Retain())
        continue;
      if (Determined || Budget.exhausted())
        return std::nullopt;
    }
    auto Form = expandOverMinterms(Ctx, DegreeTerms, DegreeIds, Width, Budget,
                                   /*Optional=*/Degree >= 3,
                                   /*SingleProduct=*/Degree >= 3);
    if (!Form) {
      if (SourceDegree >= 3 && !Budget.exhausted() && Retain())
        continue;
      return std::nullopt;
    }
    if (Degree < 2) {
      llvm::SmallVector<SymRef, 4> Forms;
      linearCandidates(Ctx, std::move(Form->Linear), DegreeAtoms, TermCeiling,
                       Limits, Forms);
      SymRef Best = cheapestOf(Ctx, Forms);
      if (!Best.isValid())
        return std::nullopt;
      if (!Append(Best))
        return std::nullopt;
      continue;
    }
    // Exact cancellation can remove a degree altogether.
    if (Form->Higher.empty())
      continue;
    const auto &Part = Form->Higher;
    const unsigned DegreeArity = DegreeIds.size();
    const size_t MaxCandidates =
        std::max<size_t>(1, Limits.SynthesisWork / *cornerCount(DegreeArity));
    if (Degree == 2) {
      if (auto Product = recoverQuadraticProduct(Part, DegreeArity, Width,
                                                 Budget, Opts.MaxTableBytes))
        Consider(Product->Coefficient, Product->Factors);
    } else if (Degree == 3) {
      bool Determined = false;
      if (auto Product = recoverCubicProduct(Part, DegreeArity, Width, Budget,
                                             Opts.MaxTableBytes, &Determined))
        Consider(Product->Coefficient, Product->Factors);
      else if (!Determined)
        forEachProductMatch(Part, Degree, DegreeArity, Width, MaxCandidates,
                            Opts.MaxTableBytes, Budget, Consider);
    } else {
      forEachProductMatch(Part, Degree, DegreeArity, Width, MaxCandidates,
                          Opts.MaxTableBytes, Budget, Consider);
    }
    // Preserve an unrecovered higher degree exactly. Other degrees may still
    // improve; dropping the residual would silently change the expression.
    if (!Best.isValid()) {
      if (SourceDegree >= 3 && !Budget.exhausted() && Retain())
        continue;
      return std::nullopt;
    }
    if (!Append(Best))
      return std::nullopt;
  }

  Parts.push_back(Remainder);
  return Ctx.mkAdd(Parts);
}

} // namespace neverd::symbolic::detail
