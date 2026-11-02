//===- SymMBAArithmetic.cpp - Arithmetic over fixed-width words ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "Limits.h"
#include "SymMBADetail.h"

#include "llvm/ADT/ScopeExit.h"

#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <numeric>
#include <set>
#include <utility>
#include <vector>

namespace neverd::symbolic::detail {

using ArithmeticFactor = std::pair<uint32_t, uint32_t>;
using ArithmeticMonomial = std::vector<ArithmeticFactor>;
using ArithmeticPolynomial = std::map<ArithmeticMonomial, llvm::APInt>;

struct ArithmeticMemo {
  struct Entry {
    SymRef Result;
    SolveReport Report;
    size_t TableBytes;
    unsigned Samples;
  };
  llvm::DenseMap<uint64_t, Entry> Entries, WholeEntries;
  struct Basis {
    uint32_t Width;
    bool ExpandComplements;
    std::map<uint32_t, SymRef> Coordinates;
    std::map<uint32_t, ArithmeticPolynomial> Polynomials;
  };
  std::vector<std::unique_ptr<Basis>> Bases;
  struct RootEntry {
    Basis *Coordinates;
    unsigned Atoms;
    bool HasAtoms;
    bool ExpandedInput;
  };
  llvm::DenseMap<uint64_t, RootEntry> Roots;
  std::map<std::pair<unsigned, uint64_t>, SymRef> Factorings;
  size_t PolynomialBytes = 0;
};

namespace {

using Factor = ArithmeticFactor;
using Monomial = ArithmeticMonomial;
using Polynomial = ArithmeticPolynomial;

struct CoefficientLess {
  bool operator()(const llvm::APInt &A, const llvm::APInt &B) const {
    return A.ult(B);
  }
};

// Allocation charges are conservative and cumulative for one attempt. Keeping
// even released storage charged bounds all temporary tables and candidate
// construction together, without making reclamation order part of the policy.
class ArithmeticResources {
  WorkBudget &Work;
  size_t Bytes;
  bool Stopped = false;

public:
  ArithmeticResources(WorkBudget &Work, size_t Bytes)
      : Work(Work), Bytes(Bytes) {}

  bool charge(size_t Units, size_t Storage = 0) {
    if (Stopped)
      return false;
    if (!Work.consume(Units) || Storage > Bytes) {
      Stopped = true;
      return false;
    }
    Bytes -= Storage;
    return true;
  }

  bool stop() {
    Stopped = true;
    return false;
  }

  bool array(size_t Count, size_t ElementBytes) {
    if (ElementBytes &&
        Count > std::numeric_limits<size_t>::max() / ElementBytes)
      return stop();
    return charge(Count, Count * ElementBytes);
  }

  bool stopped() const { return Stopped; }
  size_t remainingBytes() const { return Bytes; }
};

class ArithmeticNormalizer {
  SymContext &Ctx;
  ArithmeticResources &Resources;
  uint32_t Width;
  size_t Words;
  bool ExpandComplements;
  // Completed polynomials belong to this one context, width and complement
  // policy. std::map keeps returned references stable while later roots grow
  // the memo. Candidate emission never writes these checked coefficients.
  std::map<uint32_t, const Polynomial *> Memo;
  std::map<uint32_t, Polynomial> Owned;
  WorkBudget *SharedBudget;
  ArithmeticMemo::Basis *SharedBasis = nullptr;
  // Choose one independent coordinate for an opaque word and its complement.
  // Repeated complements need not expand into powers of (-1-X) when the
  // uncomplemented word has no arithmetic occurrence in the original region.
  // Keep this basis for the independent candidate re-expansion as well.
  std::map<uint32_t, SymRef> ComplementCoordinates;
  bool CoordinatesReady = false;
  bool ExpandedInput = false;

  void selectSharedBasis() {
    if (!SharedBudget || SharedBudget->MemoOwner != &Ctx ||
        !SharedBudget->MemoCapacity)
      return;
    if (!SharedBudget->Arithmetic)
      SharedBudget->Arithmetic = std::make_shared<ArithmeticMemo>();
    auto &Cache = *SharedBudget->Arithmetic;
    for (const auto &Basis : Cache.Bases)
      if (Basis->Width == Width &&
          Basis->ExpandComplements == ExpandComplements &&
          Basis->Coordinates == ComplementCoordinates) {
        SharedBasis = Basis.get();
        return;
      }
    const size_t Limit =
        SharedBudget->MemoCapacity * limits::kCompletedMemoEntryBytes;
    const size_t Bytes = 256 + ComplementCoordinates.size() * 128;
    if (Bytes > Limit - Cache.PolynomialBytes || !SharedBudget->canConsume(1))
      return;
    SharedBudget->consume();
    Cache.PolynomialBytes += Bytes;
    Cache.Bases.push_back(
        std::make_unique<ArithmeticMemo::Basis>(ArithmeticMemo::Basis{
            Width, ExpandComplements, ComplementCoordinates, {}}));
    SharedBasis = Cache.Bases.back().get();
  }

  const Polynomial *known(uint32_t Index) {
    auto It = Memo.find(Index);
    if (It != Memo.end())
      return It->second;
    if (SharedBasis) {
      auto Shared = SharedBasis->Polynomials.find(Index);
      if (Shared != SharedBasis->Polynomials.end())
        return &Shared->second;
    }
    // An opaque-complement polynomial is already in this basis when every
    // remaining coordinate stays opaque here. Reuse its exact coefficients;
    // no substitution or equivalence guess is involved. Cancelled coordinates
    // need not be revisited. Final emitted candidates still expand separately.
    if (SharedBasis && ExpandComplements && CoordinatesReady) {
      for (const auto &Basis : SharedBudget->Arithmetic->Bases) {
        if (Basis->ExpandComplements || Basis->Width != Width)
          continue;
        auto Found = Basis->Polynomials.find(Index);
        if (Found == Basis->Polynomials.end())
          continue;
        bool Compatible = true;
        for (const auto &[Key, Coefficient] : Found->second) {
          if (!Resources.charge(1 + Key.size()))
            return nullptr;
          for (const auto &[Id, Power] : Key)
            if (expandsComplement(SymRef(Id)) ||
                ComplementCoordinates.contains(Id)) {
              Compatible = false;
              break;
            }
          if (!Compatible)
            break;
        }
        if (Compatible) {
          if (!Resources.charge(1, 128))
            return nullptr;
          Memo.try_emplace(Index, &Found->second);
          return &Found->second;
        }
      }
    }
    return nullptr;
  }

  void remember(uint32_t Index, Polynomial P) {
    if (SharedBasis) {
      auto &Cache = *SharedBudget->Arithmetic;
      const size_t Limit =
          SharedBudget->MemoCapacity * limits::kCompletedMemoEntryBytes;
      size_t Bytes = 128;
      bool Fits = Bytes <= Limit;
      for (const auto &[Key, Coefficient] : P) {
        const size_t Entry =
            128 + Key.capacity() * sizeof(Factor) + Words * sizeof(uint64_t);
        if (Entry > Limit - std::min(Limit, Bytes)) {
          Fits = false;
          break;
        }
        Bytes += Entry;
      }
      if (Fits && Bytes <= Limit - Cache.PolynomialBytes &&
          SharedBudget->canConsume(1)) {
        SharedBudget->consume();
        Cache.PolynomialBytes += Bytes;
        auto It = SharedBasis->Polynomials.emplace(Index, std::move(P)).first;
        Memo.emplace(Index, &It->second);
        return;
      }
    }
    auto It = Owned.emplace(Index, std::move(P)).first;
    Memo.emplace(Index, &It->second);
  }

  bool chooseCoordinates(const std::map<uint32_t, bool> &Reachable) {
    CoordinatesReady = true;
    if (ComplementCoordinates.empty())
      return true;
    // Both coordinates remain algebraically valid. Prefer the old basis if
    // the uncomplemented word also appears as an arithmetic operand.
    for (const auto &[Index, FlatProduct] : Reachable) {
      SymRef R(Index);
      if (Ctx.op(R) != SymOp::Add && Ctx.op(R) != SymOp::Mul)
        continue;
      if (!Resources.charge(Ctx.numOperands(R)))
        return false;
      for (SymRef Child : Ctx.operands(R))
        ComplementCoordinates.erase(Child.index());
      if (ComplementCoordinates.empty())
        break;
    }
    return true;
  }

  bool expandsComplement(SymRef R) const {
    return ExpandComplements && Ctx.op(R) == SymOp::Not &&
           (!CoordinatesReady ||
            !ComplementCoordinates.contains(Ctx.operand(R, 0).index()));
  }

public:
  ArithmeticNormalizer(SymContext &Ctx, ArithmeticResources &Resources,
                       uint32_t Width, bool ExpandComplements = true,
                       WorkBudget *SharedBudget = nullptr)
      : Ctx(Ctx), Resources(Resources), Width(Width),
        Words((size_t(Width) + 63) / 64), ExpandComplements(ExpandComplements),
        SharedBudget(SharedBudget) {}

  bool add(Polynomial &To, const Monomial &Key,
           const llvm::APInt &Coefficient) {
    if (!Resources.charge(1 + Key.size() + Words))
      return false;
    if (Coefficient.isZero())
      return true;
    auto It = To.find(Key);
    if (It != To.end()) {
      It->second += Coefficient;
      if (It->second.isZero())
        To.erase(It);
      return true;
    }
    if (!Resources.charge(1, 128) ||
        !Resources.array(Key.size(), sizeof(Factor)) ||
        !Resources.array(Words, sizeof(uint64_t)))
      return false;
    To.emplace(Key, Coefficient);
    return true;
  }

  // These keys and coefficients have already been allocated and charged by
  // multiplication or flat-monomial construction. Transfer their ownership
  // instead of allocating and copying the same polynomial entry again.
  bool addOwned(Polynomial &To, Monomial Key, llvm::APInt Coefficient) {
    if (!Resources.charge(1 + Key.size() + Words))
      return false;
    if (Coefficient.isZero())
      return true;
    auto It = To.lower_bound(Key);
    if (It != To.end() && It->first == Key) {
      It->second += Coefficient;
      if (It->second.isZero())
        To.erase(It);
      return true;
    }
    if (!Resources.charge(1, 128))
      return false;
    To.emplace_hint(It, std::move(Key), std::move(Coefficient));
    return true;
  }

  bool copy(const Polynomial &From, Polynomial &To) {
    for (const auto &[Key, Coefficient] : From)
      if (!add(To, Key, Coefficient))
        return false;
    return true;
  }

  bool multiply(const Polynomial &A, const Polynomial &B, Polynomial &Out) {
    for (const auto &[AK, AC] : A) {
      for (const auto &[BK, BC] : B) {
        if (Words > std::numeric_limits<size_t>::max() / Words ||
            AK.size() > std::numeric_limits<size_t>::max() - BK.size())
          return Resources.stop();
        if (!Resources.charge(Words * Words) ||
            !Resources.array(Words, sizeof(uint64_t)) ||
            !Resources.array(AK.size() + BK.size(), sizeof(Factor)))
          return false;
        llvm::APInt Coefficient = AC * BC;
        if (Coefficient.isZero())
          continue;
        Monomial Key;
        Key.reserve(AK.size() + BK.size());
        size_t I = 0, J = 0;
        while (I != AK.size() || J != BK.size()) {
          if (J == BK.size() || (I != AK.size() && AK[I].first < BK[J].first))
            Key.push_back(AK[I++]);
          else if (I == AK.size() || BK[J].first < AK[I].first)
            Key.push_back(BK[J++]);
          else {
            const uint64_t Power = uint64_t(AK[I].second) + BK[J].second;
            if (Power > std::numeric_limits<uint32_t>::max())
              return Resources.stop();
            Key.emplace_back(AK[I].first, static_cast<uint32_t>(Power));
            ++I;
            ++J;
          }
        }
        if (!addOwned(Out, std::move(Key), std::move(Coefficient)))
          return false;
      }
    }
    return true;
  }

  bool unitProduct(llvm::ArrayRef<const Polynomial *> Inputs,
                   const Monomial &Common, const llvm::APInt &Scale,
                   Polynomial &Out, bool &Applied) {
    Applied = false;
    if (Inputs.size() >= std::numeric_limits<size_t>::digits ||
        (size_t(1) << Inputs.size()) > limits::kSymMBAArithmeticMaxTerms)
      return true;
    if (!Resources.charge(0, (2 * Inputs.size() + Common.size()) * 128))
      return false;
    std::map<uint32_t, unsigned> Positions;
    for (const auto *Input : Inputs) {
      if (Input->size() != 2)
        return true;
      for (const auto &[Key, Coefficient] : *Input) {
        if (!Resources.charge(1 + Words))
          return false;
        if (Key.size() != 1 || Key.front().second != 1 || !Coefficient.isOne())
          return true;
        if (!Positions.try_emplace(Key.front().first, 0).second)
          return true;
      }
    }
    for (const auto &[Id, Power] : Common)
      Positions.try_emplace(Id, 0);
    if (Positions.size() > limits::kMaxUnitProductCoordinates)
      return true;
    if (!Resources.charge(Positions.size()) ||
        !Resources.array(Inputs.size(), 2 * sizeof(unsigned)))
      return false;
    unsigned Next = 0;
    for (auto &[Id, Index] : Positions)
      Index = Next++;
    std::array<uint32_t, limits::kMaxUnitProductCoordinates> Powers{};
    uint64_t Active = 0;
    for (const auto &[Id, Power] : Common) {
      const unsigned Index = Positions.at(Id);
      Powers[Index] = Power;
      Active |= uint64_t(1) << Index;
    }
    llvm::SmallVector<std::array<unsigned, 2>, 8> Choices;
    for (const auto *Input : Inputs) {
      auto It = Input->begin();
      const unsigned A = Positions.at(It++->first.front().first);
      const unsigned B = Positions.at(It->first.front().first);
      if (Powers[A] == std::numeric_limits<uint32_t>::max() ||
          Powers[B] == std::numeric_limits<uint32_t>::max())
        return Resources.stop();
      Choices.push_back({A, B});
      ++Powers[A];
      Active |= uint64_t(1) << A;
    }
    std::array<uint32_t, limits::kMaxUnitProductCoordinates> Ids{};
    for (const auto &[Id, Index] : Positions)
      Ids[Index] = Id;
    Applied = true;
    const size_t Products = size_t(1) << Inputs.size();
    for (size_t I = 0; I != Products; ++I) {
      const size_t Count = std::popcount(Active);
      if (!Resources.array(Count, sizeof(Factor)) ||
          !Resources.array(Words, sizeof(uint64_t)))
        return false;
      Monomial Key;
      Key.reserve(Count);
      uint64_t Bits = Active;
      while (Bits) {
        const unsigned Index = std::countr_zero(Bits);
        Bits &= Bits - 1;
        Key.emplace_back(Ids[Index], Powers[Index]);
      }
      if (!addOwned(Out, std::move(Key), Scale))
        return false;
      if (I + 1 == Products)
        break;
      // Reflected Gray order changes exactly one factor choice. Distinct
      // supports make every full monomial unique, even under the same common
      // monomial. Unit coefficients need no repeated APInt multiplication.
      const unsigned Axis = std::countr_zero(I + 1);
      const size_t Gray = I ^ (I >> 1);
      const unsigned Side = (Gray >> Axis) & 1;
      const auto From = Choices[Axis][Side], To = Choices[Axis][Side ^ 1];
      if (!Resources.charge(2))
        return false;
      if (--Powers[From] == 0)
        Active &= ~(uint64_t(1) << From);
      ++Powers[To];
      Active |= uint64_t(1) << To;
    }
    return true;
  }

  bool isFlatProduct(SymRef R) {
    if (Ctx.numOperands(R) <
        limits::kSymMBAArithmeticNormalizeCtxNumOperandsRLimit)
      return false;
    for (SymRef Child : Ctx.operands(R)) {
      if (!Resources.charge(1))
        return false;
      SymOp Op = Ctx.op(Child);
      if (Op == SymOp::Add || Op == SymOp::Mul || expandsComplement(Child) ||
          ComplementCoordinates.contains(Child.index()))
        return false;
    }
    return true;
  }

  const Polynomial *normalize(SymRef Root, unsigned *AtomCount = nullptr) {
    ExpandedInput = false;
    if (!Resources.array(Words, sizeof(uint64_t)))
      return nullptr;
    const bool Initial = !CoordinatesReady;
    const uint64_t RootKey =
        (uint64_t(Root.index()) << 1) | unsigned(ExpandComplements);
    if (Initial && SharedBudget && SharedBudget->MemoOwner == &Ctx &&
        SharedBudget->Arithmetic) {
      auto &Cache = *SharedBudget->Arithmetic;
      auto Found = Cache.Roots.find(RootKey);
      if (Found != Cache.Roots.end()) {
        const auto &Entry = Found->second;
        if (!Resources.charge(Entry.Coordinates->Coordinates.size() + 1,
                              Entry.Coordinates->Coordinates.size() * 128))
          return nullptr;
        ComplementCoordinates = Entry.Coordinates->Coordinates;
        CoordinatesReady = true;
        SharedBasis = Entry.Coordinates;
        const Polynomial *Cached = known(Root.index());
        assert(Cached && "cached root must retain its completed polynomial");
        Memo.emplace(Root.index(), Cached);
        ExpandedInput = Entry.ExpandedInput;
        if (AtomCount) {
          if (Entry.HasAtoms) {
            *AtomCount = Entry.Atoms;
          } else {
            std::set<uint32_t> Inputs;
            for (const auto &[Key, Coefficient] : *Cached) {
              if (!Resources.charge(Key.size(), Key.size() * 64))
                return nullptr;
              for (const auto &[Id, Power] : Key)
                Inputs.insert(Id);
            }
            *AtomCount = Inputs.size();
            Found->second.Atoms = *AtomCount;
            Found->second.HasAtoms = true;
          }
        }
        return Cached;
      }
    }
    auto RememberRoot = [&](unsigned Atoms) {
      if (Initial && SharedBasis &&
          SharedBasis->Polynomials.contains(Root.index())) {
        auto &Cache = *SharedBudget->Arithmetic;
        const size_t Limit =
            SharedBudget->MemoCapacity * limits::kCompletedMemoEntryBytes;
        auto Found = Cache.Roots.find(RootKey);
        if (Found != Cache.Roots.end()) {
          if (AtomCount) {
            Found->second.Atoms = Atoms;
            Found->second.HasAtoms = true;
          }
        } else if (limits::kCompletedMemoEntryBytes <=
                       Limit - Cache.PolynomialBytes &&
                   SharedBudget->canConsume(1)) {
          SharedBudget->consume();
          Cache.PolynomialBytes += limits::kCompletedMemoEntryBytes;
          Cache.Roots.try_emplace(
              RootKey,
              ArithmeticMemo::RootEntry{SharedBasis, Atoms,
                                        AtomCount != nullptr, ExpandedInput});
        }
      }
    };
    if (CoordinatesReady && !AtomCount)
      if (const Polynomial *Cached = known(Root.index())) {
        if (!Resources.charge(1))
          return nullptr;
        return Cached;
      }
    // Opaque complements require no coordinate discovery. Proof calls can
    // reuse completed children before walking their repeated product edges.
    if (!CoordinatesReady && !ExpandComplements) {
      CoordinatesReady = true;
      selectSharedBasis();
    }
    if (!ExpandComplements && Ctx.op(Root) == SymOp::Add &&
        Ctx.numOperands(Root) >= limits::kMinFlatArithmeticSumTerms) {
      bool Flat = true;
      for (SymRef Term : Ctx.operands(Root)) {
        if (!Resources.charge(1))
          return nullptr;
        if (Ctx.op(Term) != SymOp::Mul)
          continue;
        if (!Resources.charge(Ctx.numOperands(Term)))
          return nullptr;
        for (SymRef Factor : Ctx.operands(Term))
          if (Ctx.op(Factor) == SymOp::Add || Ctx.op(Factor) == SymOp::Mul) {
            Flat = false;
            break;
          }
        if (!Flat)
          break;
      }
      if (Flat) {
        Polynomial Sum;
        std::set<uint32_t> Atoms;
        for (SymRef Term : Ctx.operands(Root)) {
          llvm::ArrayRef<SymRef> Factors =
              Ctx.op(Term) == SymOp::Mul ? Ctx.operands(Term)
                                         : llvm::ArrayRef<SymRef>(&Term, 1);
          if (!Resources.array(Factors.size(), sizeof(Factor)) ||
              !Resources.array(Words, sizeof(uint64_t)))
            return nullptr;
          Monomial Key;
          llvm::APInt Coefficient(Width, 1);
          for (SymRef F : Factors) {
            if (Ctx.isConst(F)) {
              if (!Resources.charge(Words * Words))
                return nullptr;
              Coefficient *= Ctx.constValue(F);
            } else if (!Key.empty() && Key.back().first == F.index()) {
              ++Key.back().second;
            } else {
              Key.emplace_back(F.index(), 1);
              if (AtomCount) {
                if (!Resources.charge(1, 64))
                  return nullptr;
                Atoms.insert(F.index());
              }
            }
          }
          if (!addOwned(Sum, std::move(Key), std::move(Coefficient)))
            return nullptr;
        }
        remember(Root.index(), std::move(Sum));
        ExpandedInput = true;
        if (AtomCount)
          *AtomCount = Atoms.size();
        RememberRoot(Atoms.size());
        return Memo.at(Root.index());
      }
    }
    const bool ReuseMemo = !Memo.empty();
    bool ReusedPolynomial = false;
    std::map<uint32_t, bool> Reachable;
    std::vector<SymRef> Pending;
    if (!Resources.charge(1, 128))
      return nullptr;
    Pending.push_back(Root);
    while (!Pending.empty()) {
      SymRef R = Pending.back();
      Pending.pop_back();
      if (!Resources.charge(1))
        return nullptr;
      if (Reachable.contains(R.index()))
        continue;
      if (ReuseMemo || SharedBasis)
        if (const Polynomial *Cached = known(R.index())) {
          if (!Resources.charge(1, 128))
            return nullptr;
          Memo.try_emplace(R.index(), Cached);
          ReusedPolynomial |= Ctx.op(R) == SymOp::Add ||
                              Ctx.op(R) == SymOp::Mul || expandsComplement(R);
          continue;
        }
      if (!Resources.charge(1, 128))
        return nullptr;
      auto Position = Reachable.emplace(R.index(), false).first;
      if (Ctx.width(R) != Width)
        return nullptr;
      if (Ctx.op(R) != SymOp::Add && Ctx.op(R) != SymOp::Mul &&
          !expandsComplement(R))
        continue;
      llvm::ArrayRef<SymRef> Children = Ctx.operands(R);
      // Flat products build their monomial directly from operand identities.
      // A proof does not count atoms, so these leaves need neither separate
      // memo entries nor repeated stack slots for every product occurrence.
      if (Ctx.op(R) == SymOp::Mul && isFlatProduct(R)) {
        Position->second = true;
        continue;
      }
      if (!Resources.array(Children.size(), sizeof(SymRef)))
        return nullptr;
      if (!CoordinatesReady && ExpandComplements && Ctx.op(R) == SymOp::Mul) {
        SymRef Previous;
        for (SymRef Child : Children) {
          if (Child == Previous && Ctx.op(Child) == SymOp::Not) {
            SymRef Inner = Ctx.operand(Child, 0);
            SymOp Op = Ctx.op(Inner);
            if (Op != SymOp::Add && Op != SymOp::Mul && Op != SymOp::Not &&
                !ComplementCoordinates.contains(Inner.index())) {
              if (!Resources.charge(1, 128))
                return nullptr;
              ComplementCoordinates.emplace(Inner.index(), Child);
            }
          }
          Previous = Child;
        }
      }
      // Canonical operands are sorted; repeated powers need one visit to
      // discover their coordinate, while normalization retains multiplicity.
      SymRef Last;
      for (SymRef Child : Children) {
        if (Child != Last)
          Pending.push_back(Child);
        Last = Child;
      }
    }

    if (!CoordinatesReady && !chooseCoordinates(Reachable))
      return nullptr;
    if (!SharedBasis)
      selectSharedBasis();

    // Node identities are topological: an interned node is newer than every
    // operand. Opaque nodes deliberately stop the walk, even if their own
    // subexpressions contain arithmetic or have a different width.
    unsigned Atoms = 0;
    ExpandedInput = !ReusedPolynomial;
    for (const auto &[Index, FlatProduct] : Reachable) {
      SymRef Node(Index);
      ExpandedInput &= !(Index != Root.index() && Ctx.op(Node) == SymOp::Add) &&
                       !expandsComplement(Node) &&
                       !ComplementCoordinates.contains(Index);
      if (const Polynomial *Cached = known(Index)) {
        if (!Resources.charge(1, 128))
          return nullptr;
        Memo.try_emplace(Index, Cached);
        SymOp Op = Ctx.op(SymRef(Index));
        Atoms += !ComplementCoordinates.contains(Index) &&
                 !Ctx.isConst(SymRef(Index)) && Op != SymOp::Add &&
                 Op != SymOp::Mul && !expandsComplement(SymRef(Index));
        continue;
      }
      if (!Resources.charge(1, 128))
        return nullptr;
      SymRef R(Index);
      Polynomial P;
      if (auto Coordinate = ComplementCoordinates.find(Index);
          Coordinate != ComplementCoordinates.end()) {
        // The selected coordinate is ~R, hence R = -1 - ~R at full width.
        if (!add(P, {}, llvm::APInt::getAllOnes(Width)) ||
            !add(P, {{Coordinate->second.index(), 1}},
                 llvm::APInt::getAllOnes(Width)))
          return nullptr;
      } else if (Ctx.isConst(R)) {
        if (!add(P, {}, Ctx.constValue(R)))
          return nullptr;
      } else if (Ctx.op(R) == SymOp::Add) {
        for (SymRef Child : Ctx.operands(R))
          if (!copy(*Memo.at(Child.index()), P))
            return nullptr;
      } else if (Ctx.op(R) == SymOp::Mul) {
        // Long flat products already name a sorted monomial. Read it once
        // instead of multiplying unit coefficients and merging one atom at
        // a time. Arithmetic children retain the general expansion below.
        // Proof traversal already checked this product's complete structure.
        // Atom-counting walks retain their leaves and classify it here.
        if (FlatProduct || (AtomCount && isFlatProduct(R))) {
          if (!Resources.array(Ctx.numOperands(R), sizeof(Factor)) ||
              !Resources.array(Words, sizeof(uint64_t)))
            return nullptr;
          Monomial Key;
          Key.reserve(Ctx.numOperands(R));
          llvm::APInt Coefficient(Width, 1);
          for (SymRef Child : Ctx.operands(R)) {
            if (Ctx.isConst(Child)) {
              if (Words > std::numeric_limits<size_t>::max() / Words ||
                  !Resources.charge(Words * Words))
                return nullptr;
              Coefficient *= Ctx.constValue(Child);
            } else if (!Key.empty() && Key.back().first == Child.index()) {
              if (Key.back().second == std::numeric_limits<uint32_t>::max())
                return nullptr;
              ++Key.back().second;
            } else {
              Key.emplace_back(Child.index(), 1);
            }
          }
          if (!addOwned(P, std::move(Key), std::move(Coefficient)))
            return nullptr;
          remember(Index, std::move(P));
          continue;
        }
        // Assemble monomial operands once. Repeatedly multiplying one more
        // atom copies the growing key at every step, making a flat product's
        // distribution proof quadratic in its number of factors.
        llvm::APInt Scale(Width, 1);
        std::map<uint32_t, uint32_t> Powers;
        unsigned PolynomialOperands = 0;
        const auto Operands = Ctx.operands(R);
        for (size_t I = 0; I != Operands.size();) {
          SymRef Child = Operands[I++];
          const auto &Operand = *Memo.at(Child.index());
          if (Operand.empty()) {
            Scale.clearAllBits();
            break;
          }
          if (Operand.size() != 1) {
            ++PolynomialOperands;
            continue;
          }
          // Canonical multiplication sorts its factors. Merge a repeated
          // monomial operand once, retaining its exact exponent; repeated
          // lookups and map allocations otherwise dominate long residuals.
          size_t Count = 1;
          while (I != Operands.size() && Operands[I] == Child) {
            ++I;
            ++Count;
          }
          if (!Resources.charge(Count - 1))
            return nullptr;
          const auto &[Key, Coefficient] = *Operand.begin();
          if (!Resources.charge(Words))
            return nullptr;
          if (!Coefficient.isOne()) {
            auto Power = Coefficient;
            size_t Exponent = Count;
            while (Exponent) {
              if (Words > std::numeric_limits<size_t>::max() / Words ||
                  !Resources.charge(Words * Words) ||
                  !Resources.array(Words, sizeof(uint64_t)))
                return nullptr;
              if (Exponent & 1)
                Scale *= Power;
              Exponent >>= 1;
              if (Exponent) {
                if (!Resources.charge(Words * Words) ||
                    !Resources.array(Words, sizeof(uint64_t)))
                  return nullptr;
                Power *= Power;
              }
            }
          }
          for (const auto &[Id, Power] : Key) {
            if (!Resources.charge(1))
              return nullptr;
            auto It = Powers.find(Id);
            if (It == Powers.end()) {
              if (!Resources.charge(0, 128))
                return nullptr;
              It = Powers.emplace(Id, 0).first;
            }
            auto &Total = It->second;
            if (Count > (std::numeric_limits<uint32_t>::max() - Total) / Power)
              return nullptr;
            Total += Power * Count;
          }
        }
        if (!Resources.array(Powers.size(), sizeof(Factor)))
          return nullptr;
        Monomial Key(Powers.begin(), Powers.end());
        if (!ExpandComplements &&
            PolynomialOperands >= limits::kMinUnitProductOperands) {
          if (!Resources.charge(
                  0, PolynomialOperands * sizeof(const Polynomial *) + 128))
            return nullptr;
          llvm::SmallVector<const Polynomial *, 8> Inputs;
          for (SymRef Child : Ctx.operands(R))
            if (Memo.at(Child.index())->size() > 1)
              Inputs.push_back(Memo.at(Child.index()));
          bool Applied = false;
          if (!unitProduct(Inputs, Key, Scale, P, Applied))
            return nullptr;
          if (Applied) {
            remember(Index, std::move(P));
            continue;
          }
        }
        // Multiply the polynomial children before attaching a long common
        // monomial. Its powers then need copying only once per final term.
        // Keep the scale early so modular zero coefficients still prune work.
        const bool DelayCommon =
            Powers.size() >= limits::kMinDelayedCommonAtoms &&
            PolynomialOperands >= limits::kMinDelayedPolynomialOperands;
        if (!add(P, DelayCommon ? Monomial{} : Key, Scale))
          return nullptr;
        for (SymRef Child : Ctx.operands(R)) {
          if (P.empty())
            break;
          if (Memo.at(Child.index())->size() == 1)
            continue;
          Polynomial Next;
          if (!multiply(P, *Memo.at(Child.index()), Next))
            return nullptr;
          P = std::move(Next);
          if (P.empty())
            break;
        }
        if (DelayCommon && !P.empty()) {
          Polynomial Common, Next;
          if (!add(Common, Key, llvm::APInt(Width, 1)) ||
              !multiply(P, Common, Next))
            return nullptr;
          P = std::move(Next);
        }
      } else if (expandsComplement(R)) {
        // A word complement is exactly -1-X in the modular ring. Only
        // arithmetic paths reach this node: bitwise consumers remain opaque.
        if (!Resources.array(Words, sizeof(uint64_t)) ||
            !add(P, {}, llvm::APInt::getAllOnes(Width)))
          return nullptr;
        for (const auto &[Key, Coefficient] :
             *Memo.at(Ctx.operand(R, 0).index())) {
          if (!Resources.array(Words, sizeof(uint64_t)) ||
              !add(P, Key, -Coefficient))
            return nullptr;
        }
      } else {
        ++Atoms;
        if (!add(P, {{Index, 1}}, llvm::APInt(Width, 1)))
          return nullptr;
      }
      remember(Index, std::move(P));
    }
    if (AtomCount) {
      // Count the independent coordinates actually retained in the proved
      // polynomial. This does not require traversing a cached subtree again.
      std::set<uint32_t> Inputs;
      for (const auto &[Key, Coefficient] : *Memo.at(Root.index())) {
        if (!Resources.charge(Key.size(), Key.size() * 64))
          return nullptr;
        for (const auto &[Id, Power] : Key)
          Inputs.insert(Id);
      }
      Atoms = Inputs.size();
      *AtomCount = Atoms;
    }
    RememberRoot(Atoms);
    return Memo.at(Root.index());
  }

  bool hasExpandedInput() const { return ExpandedInput; }

  std::optional<SymRef> combine(SymOp Op, llvm::ArrayRef<SymRef> Terms) {
    size_t Count = 0;
    for (SymRef Term : Terms) {
      const size_t N = Ctx.op(Term) == Op ? Ctx.numOperands(Term) : 1;
      if (N > std::numeric_limits<size_t>::max() - Count)
        return std::nullopt;
      Count += N;
    }
    if (!Resources.charge(1, 128) || !Resources.array(Count, 32))
      return std::nullopt;
    return Op == SymOp::Add ? Ctx.mkAdd(Terms) : Ctx.mkMul(Terms);
  }

  std::optional<SymRef> term(const Monomial &Key,
                             const llvm::APInt &Coefficient) {
    size_t Count = !Coefficient.isOne();
    for (const Factor &F : Key) {
      if (F.second > std::numeric_limits<size_t>::max() - Count)
        return std::nullopt;
      Count += F.second;
    }
    if (!Resources.charge(1, 128) || !Resources.array(Count, sizeof(SymRef)))
      return std::nullopt;
    llvm::SmallVector<SymRef, 8> Factors;
    Factors.reserve(Count);
    if (!Coefficient.isOne() || Key.empty())
      Factors.push_back(Ctx.mkConst(Coefficient));
    for (const auto &[Id, Power] : Key)
      Factors.append(Power, SymRef(Id));
    return combine(SymOp::Mul, Factors);
  }

  std::optional<SymRef> expanded(const Polynomial &P) {
    if (!Resources.array(P.size(), sizeof(SymRef)))
      return std::nullopt;
    llvm::SmallVector<SymRef, 8> Terms;
    for (const auto &[Key, Coefficient] : P) {
      std::optional<SymRef> T = term(Key, Coefficient);
      if (!T)
        return std::nullopt;
      Terms.push_back(*T);
    }
    if (Terms.empty())
      return Ctx.mkZero(Width);
    return combine(SymOp::Add, Terms);
  }

  std::optional<SymRef> factored(const Polynomial &Source) {
    struct Frame {
      Polynomial P, Rest;
      const Polynomial *Borrowed = nullptr;
      SymRef Factor, Product;
      unsigned Stage = 0;
      uint64_t Fingerprint = 0;
      bool Cacheable = false;
    };
    std::vector<Frame> Stack;
    Frame Initial;
    if (!Resources.charge(1, sizeof(Frame)))
      return std::nullopt;
    Initial.Borrowed = &Source;
    Stack.push_back(std::move(Initial));
    SymRef Result;
    auto Finish = [&](Frame &F) {
      if (!F.Cacheable || !SharedBasis || !SharedBudget->canConsume(1))
        return;
      auto &Cache = *SharedBudget->Arithmetic;
      const size_t Limit =
          SharedBudget->MemoCapacity * limits::kCompletedMemoEntryBytes;
      const auto Key = std::make_pair(Width, F.Fingerprint);
      const bool Present = Cache.Factorings.contains(Key);
      if (!Present &&
          limits::kCompletedMemoEntryBytes > Limit - Cache.PolynomialBytes)
        return;
      SharedBudget->consume();
      if (!Present)
        Cache.PolynomialBytes += limits::kCompletedMemoEntryBytes;
      Cache.Factorings.insert_or_assign(Key, Result);
    };
    while (!Stack.empty()) {
      Frame &F = Stack.back();
      if (F.Stage == 1) {
        auto Product = combine(SymOp::Mul, {F.Factor, Result});
        if (!Product)
          return std::nullopt;
        F.Product = *Product;
        if (F.Rest.empty()) {
          Result = F.Product;
          Finish(F);
          Stack.pop_back();
        } else {
          F.Stage = 2;
          Frame Next;
          Next.P = std::move(F.Rest);
          if (!Resources.charge(1, sizeof(Frame)))
            return std::nullopt;
          Stack.push_back(std::move(Next));
        }
        continue;
      }
      if (F.Stage == 2) {
        auto Sum = combine(SymOp::Add, {F.Product, Result});
        if (!Sum)
          return std::nullopt;
        Result = *Sum;
        Finish(F);
        Stack.pop_back();
        continue;
      }

      // Probe completed factor discoveries before copying a polynomial that
      // a cache hit will never mutate. Child frames already own their tables.
      const Polynomial &Input = F.Borrowed ? *F.Borrowed : F.P;
      if (SharedBasis && Input.size() >= limits::kMinArithmeticMemoTerms) {
        uint64_t Hash = 14695981039346656037ULL;
        auto Mix = [&](uint64_t Value) {
          Hash = (Hash ^ Value) * 1099511628211ULL;
        };
        for (const auto &[Key, Coefficient] : Input) {
          if (!Resources.charge(1 + 2 * Key.size() + Words))
            return std::nullopt;
          Mix(Key.size());
          for (const auto &[Id, Power] : Key) {
            Mix(Id);
            Mix(Power);
          }
          for (size_t I = 0; I != Words; ++I)
            Mix(Coefficient.getRawData()[I]);
        }
        F.Fingerprint = Hash;
        F.Cacheable = true;
        auto &Factorings = SharedBudget->Arithmetic->Factorings;
        auto Found = Factorings.find({Width, Hash});
        if (Found != Factorings.end()) {
          // A fingerprint only proposes a spelling. Its separately expanded
          // polynomial must match every coefficient and exponent before reuse.
          const Polynomial *Checked = known(Found->second.index());
          if (!Checked)
            Checked = normalize(Found->second);
          if (!Checked && Resources.stopped())
            return std::nullopt;
          if (Checked && Checked->size() == Input.size()) {
            bool Equal = true;
            auto It = Checked->begin();
            for (const auto &[Key, Coefficient] : Input) {
              if (!Resources.charge(1 + Key.size() + Words))
                return std::nullopt;
              if (Key != It->first || Coefficient != It->second) {
                Equal = false;
                break;
              }
              ++It;
            }
            if (Equal) {
              Result = Found->second;
              Stack.pop_back();
              continue;
            }
          }
        }
      }
      if (F.Borrowed) {
        if (!copy(*F.Borrowed, F.P))
          return std::nullopt;
        F.Borrowed = nullptr;
      }
      // Choose an atom shared by several terms. Removing the smallest power
      // among those terms is exact even when other terms do not contain it.
      std::map<uint32_t, std::pair<size_t, uint32_t>> Counts;
      for (const auto &[Key, Coefficient] : F.P) {
        (void)Coefficient;
        for (const auto &[Id, Power] : Key) {
          if (!Resources.charge(1, 64))
            return std::nullopt;
          auto [It, Inserted] = Counts.try_emplace(Id, 0, Power);
          ++It->second.first;
          It->second.second = std::min(It->second.second, Power);
        }
      }
      uint32_t BestId = 0, BestPower = 0;
      uint64_t BestScore = 0;
      for (const auto &[Id, Count] : Counts) {
        if (Count.first < 2)
          continue;
        const uint64_t N = Count.first - 1;
        const uint64_t Score =
            N > std::numeric_limits<uint64_t>::max() / Count.second
                ? std::numeric_limits<uint64_t>::max()
                : N * Count.second;
        if (Score > BestScore) {
          BestScore = Score;
          BestId = Id;
          BestPower = Count.second;
        }
      }
      if (!BestPower) {
        // Equal coefficients can be removed without division in the modular
        // ring, including even coefficients that have no multiplicative
        // inverse. Positive units need no factor; negative units retain the
        // established sign spelling used by the repeated region walk.
        std::map<llvm::APInt, size_t, CoefficientLess> Coefficients;
        for (const auto &[Key, Coefficient] : F.P) {
          if (Key.empty() || Coefficient.isOne() || Coefficient.isAllOnes())
            continue;
          if (!Resources.charge(1, 64) ||
              !Resources.array(Words, sizeof(uint64_t)))
            return std::nullopt;
          ++Coefficients[Coefficient];
        }
        auto Best = Coefficients.end();
        for (auto It = Coefficients.begin(); It != Coefficients.end(); ++It)
          if (It->second >= 2 &&
              (Best == Coefficients.end() || It->second > Best->second))
            Best = It;
        if (Best != Coefficients.end()) {
          Frame Next;
          if (!Resources.array(Words, sizeof(uint64_t)))
            return std::nullopt;
          const llvm::APInt One(Width, 1);
          for (auto It = F.P.begin(); It != F.P.end();) {
            if (!Resources.charge(1 + It->first.size() + Words))
              return std::nullopt;
            auto Node = F.P.extract(It++);
            if (Node.mapped() == Best->first) {
              Node.mapped() = One;
              Next.P.insert(Next.P.end(), std::move(Node));
            } else {
              F.Rest.insert(F.Rest.end(), std::move(Node));
            }
          }
          if (!Resources.charge(1, sizeof(Frame) + 128) ||
              !Resources.array(Words, sizeof(uint64_t)))
            return std::nullopt;
          F.Factor = Ctx.mkConst(Best->first);
          F.P.clear();
          F.Stage = 1;
          Stack.push_back(std::move(Next));
          continue;
        }
        auto Leaf = expanded(F.P);
        if (!Leaf)
          return std::nullopt;
        Result = *Leaf;
        Finish(F);
        Stack.pop_back();
        continue;
      }

      // Extract an entire shared monomial together. Removing just one atom
      // per frame repeatedly scans and copies the same long common powers.
      Monomial Divisor;
      if (Counts.at(BestId).first == F.P.size()) {
        for (const auto &[Id, Count] : Counts)
          if (Count.first == F.P.size()) {
            if (!Resources.array(1, sizeof(Factor)))
              return std::nullopt;
            Divisor.emplace_back(Id, Count.second);
          }
      } else {
        // Every selected term contains BestId. Extract their complete common
        // monomial together, including powers shared by just this partition.
        // This is exact in the modular ring and requires no division.
        bool First = true;
        for (const auto &[Key, Coefficient] : F.P) {
          auto It = std::lower_bound(Key.begin(), Key.end(), Factor{BestId, 0});
          if (It == Key.end() || It->first != BestId)
            continue;
          if (!Resources.array(Key.size(), sizeof(Factor)))
            return std::nullopt;
          if (First) {
            Divisor = Key;
            First = false;
          } else {
            Monomial Next;
            size_t I = 0, J = 0;
            while (I != Divisor.size() && J != Key.size()) {
              if (Divisor[I].first < Key[J].first)
                ++I;
              else if (Key[J].first < Divisor[I].first)
                ++J;
              else {
                Next.emplace_back(Key[J].first,
                                  std::min(Divisor[I].second, Key[J].second));
                ++I;
                ++J;
              }
            }
            Divisor = std::move(Next);
          }
        }
      }
      Frame Next;
      for (auto It = F.P.begin(); It != F.P.end();) {
        const auto &OriginalKey = It->first;
        if (!Resources.charge(1 + OriginalKey.size()))
          return std::nullopt;
        auto Found = std::lower_bound(OriginalKey.begin(), OriginalKey.end(),
                                      Factor{BestId, 0});
        const bool Selected =
            Found != OriginalKey.end() && Found->first == BestId;
        auto Node = F.P.extract(It++);
        if (!Selected) {
          // This is an ordered subsequence. Transfer the allocated key and
          // coefficient directly; the end hint avoids another tree search.
          F.Rest.insert(F.Rest.end(), std::move(Node));
          continue;
        }
        auto &Key = Node.key();
        if (!Resources.charge(Key.size() + Divisor.size() + 1))
          return std::nullopt;
        size_t I = 0, Written = 0;
        for (auto [Id, Power] : Key) {
          if (I != Divisor.size() && Divisor[I].first == Id)
            Power -= Divisor[I++].second;
          if (Power)
            Key[Written++] = {Id, Power};
        }
        Key.resize(Written);
        // Subtracting one common monomial is injective. Reuse the map node,
        // exponent storage and arbitrary-width coefficient without copying.
        auto Inserted = Next.P.insert(std::move(Node));
        assert(Inserted.inserted && "common-monomial subtraction is injective");
      }
      auto Common = term(Divisor, llvm::APInt(Width, 1));
      if (!Common || !Resources.charge(1, sizeof(Frame)))
        return std::nullopt;
      F.P.clear();
      F.Factor = *Common;
      F.Stage = 1;
      Stack.push_back(std::move(Next));
    }
    return Result;
  }
};

bool mayImproveArithmetic(const SymContext &Ctx, SymRef Root) {
  if (Ctx.op(Root) != SymOp::Add && Ctx.op(Root) != SymOp::Mul)
    return false;
  if (Ctx.op(Root) == SymOp::Mul)
    return llvm::any_of(Ctx.operands(Root),
                        [&](SymRef R) { return Ctx.op(R) == SymOp::Add; });
  // Linear regions already handle complements of arithmetic expressions.
  // Expanding them here as the sole reason for an attempt repeatedly reads
  // growing tails during a layered walk. A complemented atom can expose a
  // new cancellation; nonlinear products below still admit compound words.
  auto ExposesComplementAtom = [&](SymRef R) {
    if (Ctx.op(R) != SymOp::Not)
      return false;
    SymOp Inner = Ctx.op(Ctx.operand(R, 0));
    return Inner != SymOp::Add && Inner != SymOp::Mul;
  };
  for (SymRef Term : Ctx.operands(Root)) {
    if (ExposesComplementAtom(Term))
      return true;
    if (Ctx.op(Term) != SymOp::Mul)
      continue;
    unsigned NonConstants = 0;
    for (SymRef Factor : Ctx.operands(Term)) {
      if (Ctx.op(Factor) == SymOp::Add || ExposesComplementAtom(Factor))
        return true;
      if (!Ctx.isConst(Factor) && ++NonConstants == 2)
        return true;
    }
  }
  return false;
}

SymRef normalizeArithmeticImpl(SymContext &Ctx, SymRef E,
                               const MBAOptions &Opts, WorkBudget &Budget,
                               SolveReport &Rep,
                               bool ExpandComplements = true) {
  ArithmeticResources Resources(Budget, Opts.MaxTableBytes);
  ArithmeticNormalizer Normalizer(Ctx, Resources, Ctx.width(E),
                                  ExpandComplements, &Budget);
  auto Stop = [&]() {
    if (Resources.stopped() || Budget.exhausted()) {
      Rep.BudgetExhausted = true;
      Rep.Outcome = MBAOutcome::BudgetExhausted;
    }
    return E;
  };
  unsigned Atoms = 0;
  const auto *Original = Normalizer.normalize(E, &Atoms);
  if (!Original)
    return Stop();
  auto Expanded = Normalizer.hasExpandedInput()
                      ? std::optional<SymRef>(E)
                      : Normalizer.expanded(*Original);
  if (!Expanded)
    return Stop();
  SymRef Best = *Expanded;
  if (Original->size() > 1) {
    auto Factored = Normalizer.factored(*Original);
    if (!Factored)
      return Stop();
    if (readingScore(Ctx, *Factored) < readingScore(Ctx, Best))
      Best = *Factored;
  }
  if (Best == E || readingScore(Ctx, Best) >= readingScore(Ctx, E))
    return E;

  // Re-expand the chosen spelling: factor selection and emission are not
  // trusted to preserve the polynomial merely because they produced it.
  const auto *Check = Normalizer.normalize(Best);
  if (!Check)
    return Stop();
  if (*Check != *Original || !agreeOnSamples(Ctx, E, Best, Opts.VerifySamples))
    return E;
  Rep.NumAtoms = Atoms;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Best;
}

SymRef normalizeArithmetic(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                           WorkBudget &Budget, SolveReport &Rep,
                           bool ExpandComplements = true) {
  const bool Cacheable = Budget.MemoOwner == &Ctx && Budget.MemoCapacity &&
                         Rep.Outcome == MBAOutcome::NotApplicable &&
                         Rep.Evidence == MBAEvidence::None &&
                         !Rep.BudgetExhausted && !Rep.TooWide &&
                         Rep.NumAtoms == 0;
  const uint64_t Key = (uint64_t(E.index()) << 1) | unsigned(ExpandComplements);
  if (Cacheable && Budget.Arithmetic) {
    auto Found = Budget.Arithmetic->Entries.find(Key);
    if (Found != Budget.Arithmetic->Entries.end() &&
        Found->second.Samples == Opts.VerifySamples &&
        Found->second.TableBytes >= Opts.MaxTableBytes &&
        Budget.canConsume(1)) {
      Budget.consume();
      Rep = Found->second.Report;
      return Found->second.Result;
    }
  }
  const size_t Start = Budget.used();
  SymRef Result =
      normalizeArithmeticImpl(Ctx, E, Opts, Budget, Rep, ExpandComplements);
  if (Cacheable && !Budget.exhausted() && !Rep.BudgetExhausted &&
      Budget.used() - Start >= limits::kMinCompletedRegionCacheWork &&
      (!Budget.Arithmetic || Budget.Arithmetic->Entries.size() +
                                     Budget.Arithmetic->WholeEntries.size() <
                                 Budget.MemoCapacity) &&
      Budget.canConsume(1)) {
    Budget.consume();
    if (!Budget.Arithmetic)
      Budget.Arithmetic = std::make_shared<ArithmeticMemo>();
    Budget.Arithmetic->Entries.insert_or_assign(
        Key, ArithmeticMemo::Entry{Result, Rep, Opts.MaxTableBytes,
                                   Opts.VerifySamples});
  }
  return Result;
}

bool hasRepeatedCoefficients(const SymContext &Ctx, SymRef E) {
  if (Ctx.op(E) != SymOp::Add)
    return false;
  // Constant identities admit repeated coefficients without allocating a
  // table. Collisions only admit an extra budgeted normalization attempt.
  uint64_t Seen = 0;
  for (SymRef Term : Ctx.operands(E)) {
    if (Ctx.op(Term) != SymOp::Mul)
      continue;
    SymRef Factor = Ctx.operand(Term, 0);
    if (!Ctx.isConst(Factor))
      continue;
    // Wide values are not copied before the resource guard is established.
    if (Ctx.width(Factor) <= 64 &&
        (Ctx.constValue(Factor).isOne() || Ctx.constValue(Factor).isAllOnes()))
      continue;
    const uint64_t Bit = uint64_t(1) << (Factor.index() % 64);
    if (Seen & Bit)
      return true;
    Seen |= Bit;
  }
  return false;
}

// Strip an identical nonconstant monomial from both sides of a large
// distribution proof. This proves the sufficient identity between quotients;
// it never divides a word or assumes that a modular coefficient is invertible.
// Keeping complements opaque makes these factors formal polynomial atoms.
std::pair<SymRef, SymRef>
reduceDistributionProof(SymContext &Ctx, SymRef Before, SymRef After,
                        ArithmeticResources &Resources) {
  const auto Original = std::make_pair(Before, After);
  constexpr size_t
      MaxTerms = ::neverd::symbolic::detail::limits::kSymMBAArithmeticMaxTerms,
      MaxEdges = ::neverd::symbolic::detail::limits::kSymMBAArithmeticMaxEdges;
  if (Ctx.op(Before) != SymOp::Add ||
      Ctx.numOperands(Before) <
          ::neverd::symbolic::detail::limits::
              kSymMBAArithmeticReduceDistributionProofCtxNumOperandsBeforeLimit ||
      Ctx.numOperands(Before) > MaxTerms ||
      (Ctx.op(After) == SymOp::Add && Ctx.numOperands(After) > MaxTerms))
    return Original;
  size_t Count = Ctx.numOperands(Before) +
                 (Ctx.op(After) == SymOp::Add ? Ctx.numOperands(After) : 1);
  if (!Resources.array(Count * 4, sizeof(SymRef)))
    return Original;
  llvm::SmallVector<SymRef, 128> Left(Ctx.operands(Before));
  llvm::SmallVector<SymRef, 128> Right;
  if (Ctx.op(After) == SymOp::Add)
    Right.append(Ctx.operands(After).begin(), Ctx.operands(After).end());
  else
    Right.push_back(After);
  llvm::sort(Left);
  llvm::sort(Right);
  llvm::SmallVector<SymRef, 128> L, R;
  std::set_difference(Left.begin(), Left.end(), Right.begin(), Right.end(),
                      std::back_inserter(L));
  std::set_difference(Right.begin(), Right.end(), Left.begin(), Left.end(),
                      std::back_inserter(R));
  auto Residual = [&]() {
    if (L.size() == Left.size() && R.size() == Right.size())
      return Original;
    if (!Resources.charge(L.size() + R.size() + 2, 256))
      return Original;
    return std::make_pair(
        L.empty() ? Ctx.mkZero(Ctx.width(Before)) : Ctx.mkAdd(L),
        R.empty() ? Ctx.mkZero(Ctx.width(After)) : Ctx.mkAdd(R));
  };
  if (L.empty() || R.empty())
    return Residual();
  auto Factors = [&](SymRef Term) {
    auto F = Ctx.operands(Term);
    return Ctx.isConst(F.front()) ? F.drop_front() : F;
  };
  size_t Edges = 0;
  llvm::SmallVector<SymRef, 32> Common, Next;
  for (auto Terms : {llvm::ArrayRef<SymRef>(L), llvm::ArrayRef<SymRef>(R)})
    for (SymRef Term : Terms) {
      if (Ctx.op(Term) != SymOp::Mul)
        return Residual();
      auto F = Factors(Term);
      if (F.size() > MaxEdges - Edges)
        return Residual();
      if (!Resources.array(F.size() * 3, sizeof(SymRef)))
        return Residual();
      if (Edges == 0)
        Common.append(F.begin(), F.end());
      else {
        Next.clear();
        std::set_intersection(Common.begin(), Common.end(), F.begin(), F.end(),
                              std::back_inserter(Next));
        Common.swap(Next);
      }
      Edges += F.size();
      if (Common.size() < 4)
        return Residual();
    }
  // Snapshot every quotient before a builder can relocate the operand pool.
  if (!Resources.charge(Edges + Count, 128 * (L.size() + R.size())))
    return Original;
  llvm::SmallVector<llvm::SmallVector<SymRef, 16>, 8> Quotients;
  for (auto Terms : {llvm::ArrayRef<SymRef>(L), llvm::ArrayRef<SymRef>(R)})
    for (SymRef Term : Terms) {
      auto F = Factors(Term);
      Quotients.emplace_back();
      auto &Q = Quotients.back();
      auto First = Ctx.operand(Term, 0);
      if (Ctx.isConst(First))
        Q.push_back(First);
      std::set_difference(F.begin(), F.end(), Common.begin(), Common.end(),
                          std::back_inserter(Q));
    }
  Left.clear();
  Right.clear();
  for (size_t I = 0; I != Quotients.size(); ++I) {
    auto &Q = Quotients[I];
    SymRef Term = Q.empty() ? Ctx.mkOne(Ctx.width(Before)) : Ctx.mkMul(Q);
    (I < L.size() ? Left : Right).push_back(Term);
  }
  return {Ctx.mkAdd(Left), Ctx.mkAdd(Right)};
}

} // namespace

SymRef solveNestedProductCofactors(SymContext &Ctx, SymRef E,
                                   const MBAOptions &Opts, WorkBudget &Budget,
                                   SolveReport &Rep) {
  ArithmeticResources Resources(Budget, Opts.MaxTableBytes);
  auto MarkStopped = llvm::scope_exit([&] {
    Rep.BudgetExhausted |= Resources.stopped() || Budget.exhausted();
  });
  // Admit the syntactic distribution before allocating polynomial tables.
  // Repeated small sums often simplify cheaply in the normal deep walk; an
  // optional completion must not consume that walk's budget by expanding them.
  unsigned Visits = 0;
  bool HasProductSum = false;
  auto FrontierSize = [&](auto &&Self, SymRef R, unsigned Depth,
                          bool ProductOperand) -> unsigned {
    if (++Visits >
            ::neverd::symbolic::detail::limits::
                kSymMBAArithmeticSolveNestedProductCofactorsFrontierSizeVisitsLimit ||
        Depth >
            ::neverd::symbolic::detail::limits::
                kSymMBAArithmeticSolveNestedProductCofactorsFrontierSizeDepthLimit ||
        !Resources.charge(1))
      return 0;
    const SymOp Op = Ctx.op(R);
    if (Op != SymOp::Add && Op != SymOp::Mul)
      return 1;
    unsigned Terms = Op == SymOp::Add ? 0 : 1;
    for (SymRef Child : Ctx.operands(R)) {
      if (Op == SymOp::Add && ProductOperand && Ctx.op(Child) == SymOp::Mul &&
          Ctx.numOperands(Child) >= 2 &&
          Ctx.numOperands(Child) -
                  unsigned(Ctx.isConst(Ctx.operand(Child, 0))) >=
              2)
        HasProductSum = true;
      unsigned N = Self(Self, Child, Depth + 1, Op == SymOp::Mul);
      if (!N || (Op == SymOp::Add ? N > ::neverd::symbolic::detail::limits::
                                                kMaxNestedProductFrontierTerms -
                                            Terms
                                  : N > ::neverd::symbolic::detail::limits::
                                                kMaxNestedProductFrontierTerms /
                                            Terms))
        return 0;
      Terms = Op == SymOp::Add ? Terms + N : Terms * N;
    }
    return Terms;
  };
  if (!FrontierSize(FrontierSize, E, 0, false) || !HasProductSum)
    return E;
  ArithmeticNormalizer Normalizer(Ctx, Resources, Ctx.width(E), false, &Budget);
  auto Original = Normalizer.normalize(E);
  if (!Original ||
      Original->size() >
          ::neverd::symbolic::detail::limits::
              kSymMBAArithmeticSolveNestedProductCofactorsOriginalSizeLimit)
    return E;
  auto HigherAtoms = [&](const Polynomial &P, std::set<uint32_t> &Atoms) {
    size_t Edges = 0, Higher = 0;
    for (const auto &[Key, Coefficient] : P) {
      size_t Degree = 0;
      for (const auto &[Id, Power] : Key) {
        if (Power > ::neverd::symbolic::detail::limits::
                            kMaxNestedCofactorPowerEdges -
                        Edges ||
            !Resources.charge(1, 128))
          return size_t(0);
        Edges += Power;
        Degree += Power;
      }
      if (Degree >= 4) {
        ++Higher;
        for (const auto &[Id, Power] : Key)
          Atoms.insert(Id);
      }
    }
    return Higher;
  };
  std::set<uint32_t> Atoms;
  if (HigherAtoms(*Original, Atoms) < 2)
    return E;

  // A previous factorization can collapse some partition axes but leave
  // others split. Restore only an existing AND view of a whole product word:
  // W = (W & M) + (W & ~M), exactly at every bit width. Bitwise consumers are
  // opaque here, so this cannot recursively split the word inside its view.
  std::map<uint32_t, SymRef> Splits;
  for (uint32_t Id : Atoms) {
    SymRef View(Id);
    if (Ctx.op(View) != SymOp::And || Ctx.numOperands(View) < 2 ||
        Ctx.numOperands(View) >
            ::neverd::symbolic::detail::limits::
                kSymMBAArithmeticSolveNestedProductCofactorsCtxNumOperandsViewLimit)
      continue;
    if (!Resources.array(Ctx.numOperands(View), sizeof(SymRef)))
      return E;
    llvm::SmallVector<SymRef, 8> Inputs(Ctx.operands(View));
    for (SymRef Word : Inputs) {
      if (Splits.size() == 6 || !Atoms.contains(Word.index()) ||
          Splits.contains(Word.index()))
        continue;
      if (!Resources.array(Inputs.size(), 128 + 4 * sizeof(SymRef)))
        return E;
      llvm::SmallVector<SymRef, 8> Mask;
      for (SymRef Input : Inputs)
        if (Input != Word)
          Mask.push_back(Input);
      SymRef Other = Ctx.mkAnd(Word, Ctx.mkNot(Ctx.mkAnd(Mask)));
      Splits.emplace(Word.index(), Ctx.mkAdd(View, Other));
    }
  }
  llvm::SmallVector<SymRef, 16> Terms, OriginalTerms;
  for (const auto &[Key, Coefficient] : *Original) {
    llvm::SmallVector<SymRef, 16> Factors{Ctx.mkConst(Coefficient)};
    llvm::SmallVector<SymRef, 16> OriginalFactors(Factors);
    for (const auto &[Id, Power] : Key) {
      if (!Resources.array(Power, 4 * sizeof(SymRef)))
        return E;
      auto It = Splits.find(Id);
      Factors.append(Power, It == Splits.end() ? SymRef(Id) : It->second);
      OriginalFactors.append(Power, SymRef(Id));
    }
    Terms.push_back(Ctx.mkMul(Factors));
    OriginalTerms.push_back(Ctx.mkMul(OriginalFactors));
  }
  // Check coefficient and multiplicity emission independently, then apply the
  // exact word partitions and check the expanded spelling as well. Neither
  // this larger intermediate nor an incomplete completion is ever published.
  auto OriginalCheck = Normalizer.normalize(Ctx.mkAdd(OriginalTerms));
  if (!OriginalCheck || *OriginalCheck != *Original)
    return E;
  auto Distributed = Normalizer.normalize(Ctx.mkAdd(Terms));
  Atoms.clear();
  if (!Distributed ||
      Distributed->size() >
          ::neverd::symbolic::detail::limits::
              kSymMBAArithmeticSolveNestedProductCofactorsDistributedSizeLimit ||
      HigherAtoms(*Distributed, Atoms) < 6)
    return E;
  auto Expanded = Normalizer.expanded(*Distributed);
  if (!Expanded || *Expanded == E)
    return E;
  auto Check = Normalizer.normalize(*Expanded);
  if (!Check || *Check != *Distributed)
    return E;
  MBAOptions Inner = Opts;
  Inner.MaxTableBytes = Resources.remainingBytes();
  SolveReport Local;
  SymRef Candidate =
      solveProductCofactors(Ctx, *Expanded, Inner, Budget, Local);
  Rep.BudgetExhausted |= Local.BudgetExhausted || Resources.stopped();
  if (Candidate == *Expanded || Local.Evidence != MBAEvidence::Derivation ||
      readingScore(Ctx, Candidate) >= readingScore(Ctx, E))
    return E;
  Rep = Local;
  return Candidate;
}

bool proveArithmeticIdentity(SymContext &Ctx, SymRef Before, SymRef After,
                             WorkBudget &Budget, size_t MaxBytes,
                             bool ExpandComplements) {
  if (!Before.isValid() || !After.isValid() ||
      Ctx.width(Before) != Ctx.width(After))
    return false;
  ArithmeticResources Resources(Budget, MaxBytes);
  if (!ExpandComplements)
    std::tie(Before, After) =
        reduceDistributionProof(Ctx, Before, After, Resources);
  ArithmeticNormalizer Normalizer(Ctx, Resources, Ctx.width(Before),
                                  ExpandComplements, &Budget);
  auto Left = Normalizer.normalize(Before);
  if (!Left)
    return false;
  auto Right = Normalizer.normalize(After);
  if (!Right || Left->size() != Right->size())
    return false;
  const size_t Words = (size_t(Ctx.width(Before)) + 63) / 64;
  auto R = Right->begin();
  for (const auto &[Key, Coefficient] : *Left) {
    if (!Resources.charge(1 + Key.size() + Words) || Key != R->first ||
        Coefficient != R->second)
      return false;
    ++R;
  }
  return true;
}

static SymRef solveArithmeticImpl(SymContext &Ctx, SymRef E,
                                  const MBAOptions &Opts, WorkBudget &Budget,
                                  SolveReport &Rep, bool ExpandComplements) {
  // A literal plus a complemented atom is already a complete affine reading:
  // C + ~X = (C - 1) - X at the full word width. Avoid building polynomial
  // tables for each such leaf after a large product proof. Compound arithmetic
  // words retain normalization, which can expose further cancellations.
  if (ExpandComplements && Ctx.op(E) == SymOp::Add && Ctx.numOperands(E) == 2 &&
      Ctx.isConst(Ctx.operand(E, 0)) &&
      Ctx.op(Ctx.operand(E, 1)) == SymOp::Not) {
    const SymRef Atom = Ctx.operand(Ctx.operand(E, 1), 0);
    if (Ctx.op(Atom) != SymOp::Add && Ctx.op(Atom) != SymOp::Mul) {
      const size_t Words = (size_t(Ctx.width(E)) - 1) / 64 + 1;
      ArithmeticResources Resources(Budget, Opts.MaxTableBytes);
      if (!Resources.charge(
              2 + 3 * Words,
              3 * sizeof(SymNode) + 4 * sizeof(SymRef) +
                  3 * (sizeof(llvm::APInt) + Words * sizeof(uint64_t)))) {
        Rep.BudgetExhausted = true;
        Rep.Outcome = MBAOutcome::BudgetExhausted;
        return E;
      }
      auto Constant = Ctx.constValue(Ctx.operand(E, 0));
      --Constant;
      const SymRef Candidate = Ctx.mkSub(Ctx.mkConst(Constant), Atom);
      if (readingScore(Ctx, Candidate) < readingScore(Ctx, E) &&
          agreeOnSamples(Ctx, E, Candidate, Opts.VerifySamples)) {
        Rep.NumAtoms = 1;
        Rep.Outcome = MBAOutcome::Rewritten;
        Rep.Evidence = MBAEvidence::Derivation;
        return Candidate;
      }
      return E;
    }
  }
  // Keep a complete common monomial outside normalization. Its value may be
  // any fixed-width word, so proving the remaining quotient suffices without
  // expanding and re-factoring the same large powers in every parent.
  if (Ctx.op(E) == SymOp::Add &&
      Ctx.numOperands(E) <= limits::kSymMBAArithmeticMaxTerms &&
      llvm::all_of(Ctx.operands(E), [&](SymRef T) {
        return Ctx.op(T) == SymOp::Mul &&
               Ctx.numOperands(T) >=
                   limits::kMinArithmeticCommonProductOperands;
      })) {
    size_t Edges = 0;
    bool Fits = true;
    for (SymRef T : Ctx.operands(E)) {
      if (Ctx.numOperands(T) > limits::kSymMBAArithmeticMaxEdges - Edges) {
        Fits = false;
        break;
      }
      Edges += Ctx.numOperands(T);
    }
    if (Fits) {
      ArithmeticResources Resources(Budget, Opts.MaxTableBytes);
      if (!Resources.charge(0, Ctx.numOperands(E) * 256 +
                                   2 * Edges * sizeof(SymRef)) ||
          !Resources.array(Edges * 3, sizeof(SymRef))) {
        Rep.BudgetExhausted = true;
        Rep.Outcome = MBAOutcome::BudgetExhausted;
        return E;
      }
      llvm::SmallVector<SymRef, 16> Common, Next;
      bool First = true;
      for (SymRef T : Ctx.operands(E)) {
        auto F = Ctx.operands(T);
        if (Ctx.isConst(F.front()))
          F = F.drop_front();
        if (First) {
          Common.append(F.begin(), F.end());
          First = false;
        } else {
          Next.clear();
          std::set_intersection(Common.begin(), Common.end(), F.begin(),
                                F.end(), std::back_inserter(Next));
          Common.swap(Next);
        }
        if (Common.size() < limits::kMinArithmeticCommonFactors)
          break;
      }
      if (Common.size() >= limits::kMinArithmeticCommonFactors) {
        // Snapshot before builders can relocate the context operand pool.
        llvm::SmallVector<llvm::SmallVector<SymRef, 16>, 8> Quotients;
        for (SymRef T : Ctx.operands(E)) {
          auto F = Ctx.operands(T);
          Quotients.emplace_back();
          auto &Q = Quotients.back();
          if (Ctx.isConst(F.front())) {
            Q.push_back(F.front());
            F = F.drop_front();
          }
          std::set_difference(F.begin(), F.end(), Common.begin(), Common.end(),
                              std::back_inserter(Q));
        }
        llvm::SmallVector<SymRef, 8> Terms;
        for (const auto &Q : Quotients)
          Terms.push_back(Q.empty() ? Ctx.mkOne(Ctx.width(E)) : Ctx.mkMul(Q));
        const SymRef Quotient = Ctx.mkAdd(Terms);
        MBAOptions Inner = Opts;
        Inner.MaxTableBytes = Resources.remainingBytes();
        SolveReport Local;
        const SymRef Reduced = normalizeArithmetic(Ctx, Quotient, Inner, Budget,
                                                   Local, ExpandComplements);
        Rep.BudgetExhausted |= Local.BudgetExhausted;
        Common.push_back(Reduced);
        const SymRef Candidate = Ctx.mkMul(Common);
        if (readingScore(Ctx, Candidate) < readingScore(Ctx, E)) {
          Rep.NumAtoms = Local.NumAtoms;
          Rep.Outcome = MBAOutcome::Rewritten;
          Rep.Evidence = MBAEvidence::Derivation;
          return Candidate;
        }
      }
    }
  }
  if (Ctx.op(E) == SymOp::Add &&
      Ctx.numOperands(E) >= limits::kMinArithmeticPartitionTerms &&
      llvm::any_of(Ctx.operands(E), [&](SymRef Term) {
        return Ctx.op(Term) == SymOp::Mul &&
               llvm::any_of(Ctx.operands(Term), [&](SymRef Factor) {
                 return Ctx.op(Factor) == SymOp::Add;
               });
      })) {
    ArithmeticResources Resources(Budget, Opts.MaxTableBytes);
    const size_t Count = Ctx.numOperands(E);
    if (!Resources.array(Count, 128)) {
      Rep.BudgetExhausted = true;
      Rep.Outcome = MBAOutcome::BudgetExhausted;
      return E;
    }
    llvm::SmallVector<SymRef, 16> Terms(Ctx.operands(E));
    std::vector<unsigned> Parents(Count);
    std::iota(Parents.begin(), Parents.end(), 0);
    auto Find = [&](unsigned I) {
      while (Parents[I] != I) {
        Parents[I] = Parents[Parents[I]];
        I = Parents[I];
      }
      return I;
    };
    std::map<uint64_t, unsigned> Owners;
    auto Join = [&](uint64_t Key, unsigned I) {
      if (!Resources.charge(1, 64))
        return false;
      auto [It, Inserted] = Owners.try_emplace(Key, I);
      if (!Inserted)
        Parents[Find(I)] = Find(It->second);
      return true;
    };
    auto IsAtom = [&](SymRef R) {
      const auto Op = Ctx.op(R);
      return !Ctx.isConst(R) && Op != SymOp::Add && Op != SymOp::Mul &&
             (!ExpandComplements || Op != SymOp::Not);
    };
    for (unsigned I = 0; I != Count; ++I) {
      llvm::SmallVector<SymRef, 16> Pending{Terms[I]};
      std::set<uint32_t> Seen;
      while (!Pending.empty()) {
        const auto R = Pending.pop_back_val();
        if (!Resources.charge(1, 64)) {
          Rep.BudgetExhausted = true;
          Rep.Outcome = MBAOutcome::BudgetExhausted;
          return E;
        }
        if (!Seen.insert(R.index()).second || Ctx.isConst(R))
          continue;
        if (IsAtom(R)) {
          if (!Join(R.index(), I)) {
            Rep.BudgetExhausted = true;
            Rep.Outcome = MBAOutcome::BudgetExhausted;
            return E;
          }
          continue;
        }
        const auto Children = Ctx.operands(R);
        if (!Resources.array(Children.size(), sizeof(SymRef))) {
          Rep.BudgetExhausted = true;
          Rep.Outcome = MBAOutcome::BudgetExhausted;
          return E;
        }
        Pending.append(Children.begin(), Children.end());
      }
      // A literal or a product without an opaque factor may have a constant
      // monomial. Keep all such contributions together, including ~X=-1-X.
      const bool NoConstant =
          IsAtom(Terms[I]) || (Ctx.op(Terms[I]) == SymOp::Mul &&
                               llvm::any_of(Ctx.operands(Terms[I]), IsAtom));
      if (!NoConstant && !Join(uint64_t(1) << 32, I)) {
        Rep.BudgetExhausted = true;
        Rep.Outcome = MBAOutcome::BudgetExhausted;
        return E;
      }
    }
    std::map<unsigned, llvm::SmallVector<SymRef, 8>> Components;
    for (unsigned I = 0; I != Count; ++I)
      Components[Find(I)].push_back(Terms[I]);
    if (Components.size() > 1) {
      MBAOptions Inner = Opts;
      Inner.MaxTableBytes = Resources.remainingBytes();
      llvm::SmallVector<SymRef, 8> Parts;
      for (const auto &[Id, Component] : Components) {
        const SymRef Part = Ctx.mkAdd(Component);
        SolveReport Local;
        Parts.push_back(solveArithmetic(Ctx, Part, Inner, Budget, Local,
                                        ExpandComplements));
        Rep.BudgetExhausted |= Local.BudgetExhausted;
        Rep.NumAtoms = std::max(Rep.NumAtoms, Local.NumAtoms);
      }
      const SymRef Candidate = Ctx.mkAdd(Parts);
      if (readingScore(Ctx, Candidate) >= readingScore(Ctx, E)) {
        if (Rep.BudgetExhausted)
          Rep.Outcome = MBAOutcome::BudgetExhausted;
        return E;
      }
      Rep.Outcome = MBAOutcome::Rewritten;
      Rep.Evidence = MBAEvidence::Derivation;
      return Candidate;
    }
  }
  if (Ctx.op(E) == SymOp::Add && Ctx.numOperands(E) == 2 &&
      Ctx.isConst(Ctx.operand(E, 0))) {
    const SymRef Literal = Ctx.operand(E, 0), Product = Ctx.operand(E, 1);
    if (Ctx.op(Product) == SymOp::Mul &&
        llvm::any_of(Ctx.operands(Product), [&](SymRef Factor) {
          const auto Op = Ctx.op(Factor);
          return !Ctx.isConst(Factor) && Op != SymOp::Add && Op != SymOp::Mul &&
                 Op != SymOp::Not;
        })) {
      if (!Budget.consume(2 + Ctx.numOperands(Product))) {
        Rep.BudgetExhausted = true;
        Rep.Outcome = MBAOutcome::BudgetExhausted;
        return E;
      }
      // Every polynomial monomial of this product retains an opaque word.
      // The independent literal cannot cancel one or share its word factor.
      // Normalize the product once, preserving its already checked children.
      const SymRef Reduced =
          solveArithmetic(Ctx, Product, Opts, Budget, Rep, ExpandComplements);
      if (Reduced == Product)
        return E;
      const SymRef Candidate = Ctx.mkAdd(Literal, Reduced);
      if (readingScore(Ctx, Candidate) >= readingScore(Ctx, E)) {
        if (Rep.BudgetExhausted)
          Rep.Outcome = MBAOutcome::BudgetExhausted;
        return E;
      }
      Rep.Outcome = MBAOutcome::Rewritten;
      Rep.Evidence = MBAEvidence::Derivation;
      return Candidate;
    }
  }
  if (Ctx.op(E) == SymOp::Mul) {
    SymRef Sum, Scale;
    bool Eligible = true;
    auto Inputs = Ctx.operands(E);
    for (SymRef F : Inputs) {
      if (Ctx.isConst(F)) {
        Scale = F;
      } else if (Ctx.op(F) == SymOp::Add) {
        if (Sum.isValid())
          Eligible = false;
        Sum = F;
      } else if (Ctx.op(F) == SymOp::Mul) {
        Eligible = false;
      }
    }
    const bool Nonunit = Scale.isValid() && !Ctx.constValue(Scale)[0];
    if (Eligible && Sum.isValid() && (!Nonunit || Inputs.size() > 2)) {
      const size_t Bytes = Inputs.size() * 2 * sizeof(SymRef);
      if (Bytes > Opts.MaxTableBytes || !Budget.consume(Inputs.size())) {
        Rep.BudgetExhausted = true;
        Rep.Outcome = MBAOutcome::BudgetExhausted;
        return E;
      }
      llvm::SmallVector<SymRef, 16> Factors(Inputs);
      MBAOptions Inner = Opts;
      Inner.MaxTableBytes -= Bytes;
      SymRef Quotient = Sum;
      if (Nonunit) {
        const size_t Count = Ctx.numOperands(Sum);
        size_t Edges = Count;
        for (SymRef T : Ctx.operands(Sum))
          Edges += Ctx.op(T) == SymOp::Mul ? Ctx.numOperands(T) : 1;
        const size_t Bytes = 128 * Count + 4 * Edges * sizeof(SymRef);
        if (Bytes > Inner.MaxTableBytes || !Budget.consume(Count + Edges)) {
          Rep.BudgetExhausted = true;
          Rep.Outcome = MBAOutcome::BudgetExhausted;
          return E;
        }
        Inner.MaxTableBytes -= Bytes;
        llvm::SmallVector<SymRef, 8> Terms(Ctx.operands(Sum));
        for (SymRef &T : Terms)
          T = Ctx.mkMul(Scale, T);
        Quotient = Ctx.mkAdd(Terms);
        Factors.erase(llvm::find(Factors, Scale));
      }
      SymRef Reduced =
          solveArithmetic(Ctx, Quotient, Inner, Budget, Rep, ExpandComplements);
      if (Reduced == Sum)
        return E;
      for (SymRef &F : Factors)
        if (F == Sum)
          F = Reduced;
      SymRef Candidate = Ctx.mkMul(Factors);
      if (readingScore(Ctx, Candidate) >= readingScore(Ctx, E)) {
        if (Rep.BudgetExhausted)
          Rep.Outcome = MBAOutcome::BudgetExhausted;
        return E;
      }
      Rep.Evidence = MBAEvidence::Derivation;
      Rep.Outcome = MBAOutcome::Rewritten;
      return Candidate;
    }
  }
  return mayImproveArithmetic(Ctx, E)
             ? normalizeArithmetic(Ctx, E, Opts, Budget, Rep, ExpandComplements)
             : E;
}

SymRef solveArithmetic(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                       WorkBudget &Budget, SolveReport &Rep,
                       bool ExpandComplements) {
  const bool Cacheable = Budget.MemoOwner == &Ctx && Budget.MemoCapacity &&
                         Rep.Outcome == MBAOutcome::NotApplicable &&
                         Rep.Evidence == MBAEvidence::None &&
                         !Rep.BudgetExhausted && !Rep.TooWide &&
                         Rep.NumAtoms == 0;
  const uint64_t Key = (uint64_t(E.index()) << 1) | unsigned(ExpandComplements);
  if (Cacheable && Budget.Arithmetic) {
    auto Found = Budget.Arithmetic->WholeEntries.find(Key);
    if (Found != Budget.Arithmetic->WholeEntries.end() &&
        Found->second.Samples == Opts.VerifySamples &&
        Found->second.TableBytes >= Opts.MaxTableBytes &&
        Budget.canConsume(1)) {
      Budget.consume();
      Rep = Found->second.Report;
      return Found->second.Result;
    }
  }
  const size_t Start = Budget.used();
  SymRef Result =
      solveArithmeticImpl(Ctx, E, Opts, Budget, Rep, ExpandComplements);
  if (Cacheable && !Budget.exhausted() && !Rep.BudgetExhausted &&
      Budget.used() - Start >= limits::kMinCompletedRegionCacheWork &&
      (!Budget.Arithmetic || Budget.Arithmetic->WholeEntries.size() +
                                     Budget.Arithmetic->Entries.size() <
                                 Budget.MemoCapacity) &&
      Budget.canConsume(1)) {
    Budget.consume();
    if (!Budget.Arithmetic)
      Budget.Arithmetic = std::make_shared<ArithmeticMemo>();
    Budget.Arithmetic->WholeEntries.insert_or_assign(
        Key, ArithmeticMemo::Entry{Result, Rep, Opts.MaxTableBytes,
                                   Opts.VerifySamples});
  }
  return Result;
}

SymRef solveCoefficientFactors(SymContext &Ctx, SymRef E,
                               const MBAOptions &Opts, WorkBudget &Budget,
                               SolveReport &Rep) {
  return hasRepeatedCoefficients(Ctx, E)
             ? normalizeArithmetic(Ctx, E, Opts, Budget, Rep)
             : E;
}

} // namespace neverd::symbolic::detail
