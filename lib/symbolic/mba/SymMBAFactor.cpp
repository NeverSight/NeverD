//===- SymMBAFactor.cpp - Recovering complete shared factors -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "Limits.h"
#include "SymMBADetail.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <numeric>
#include <set>

namespace neverd::symbolic::detail {
namespace {

bool isBitwise(const SymContext &Ctx, SymRef R) {
  SymOp Op = Ctx.op(R);
  return Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor ||
         Op == SymOp::Not;
}

// The optional quotient search needs interaction between its terms. A sum of
// shallow, already canonical bitwise terms on disjoint free variables has no
// shared input for that search to eliminate. Keep this eligibility check local:
// arithmetic, opaque nodes, complements, and larger frontiers use the solver.
bool hasIndependentTerms(const SymContext &Ctx, SymRef E) {
  if (Ctx.width(E) == 1 || Ctx.op(E) != SymOp::Add)
    return false;
  std::array<uint32_t, 64> Inputs;
  size_t Count = 0;
  auto AddInput = [&](SymRef R) {
    if (!Ctx.isVar(R) || Count == Inputs.size())
      return false;
    Inputs[Count++] = R.index();
    return true;
  };
  for (SymRef Term : Ctx.operands(E)) {
    if (Ctx.isConst(Term))
      continue;
    if (Ctx.isVar(Term)) {
      if (!AddInput(Term))
        return false;
      continue;
    }
    SymOp Op = Ctx.op(Term);
    if (Op != SymOp::And && Op != SymOp::Or && Op != SymOp::Xor)
      return false;
    for (SymRef Input : Ctx.operands(Term))
      if (!AddInput(Input))
        return false;
  }
  std::sort(Inputs.begin(), Inputs.begin() + Count);
  return std::adjacent_find(Inputs.begin(), Inputs.begin() + Count) ==
         Inputs.begin() + Count;
}

struct FactorUses {
  unsigned Count = 0;
  bool BitwiseQuotient = false;
};

struct PartitionCubeForm {
  SymRef Value, Distribution;
};

// Optimize the complete coefficient tensor of a small binary partition cube.
// The raw circuit uses the original sums; the value circuit substitutes only
// separately proved partition identities. An independent polynomial expansion
// of the raw circuit checks the complete result before either is accepted.
PartitionCubeForm
factorPartitionCube(SymContext &Ctx, llvm::ArrayRef<std::array<SymRef, 4>> Axes,
                    llvm::ArrayRef<llvm::APInt> Coefficients,
                    WorkBudget &Budget, size_t &Storage) {
  struct Key {
    unsigned Mask;
    std::vector<llvm::APInt> Values;
    bool operator<(const Key &Other) const {
      if (Mask != Other.Mask)
        return Mask < Other.Mask;
      return std::lexicographical_compare(
          Values.begin(), Values.end(), Other.Values.begin(),
          Other.Values.end(),
          [](const auto &A, const auto &B) { return A.ult(B); });
    }
  };
  std::map<Key, PartitionCubeForm> Memo;
  const size_t Words = Coefficients.front().getNumWords();
  auto Charge = [&](size_t Work, size_t Slots) {
    const size_t Bytes = 128 + 4 * Words * sizeof(uint64_t);
    if (Slots > Storage / Bytes || !Budget.consume(Work))
      return false;
    Storage -= Slots * Bytes;
    return true;
  };
  auto Solve = [&](auto &&Self, unsigned Mask,
                   llvm::ArrayRef<llvm::APInt> Values) -> PartitionCubeForm {
    if (!Charge(1 + Values.size() * Words, 1 + Values.size()))
      return {};
    Key K{Mask, {Values.begin(), Values.end()}};
    if (auto It = Memo.find(K); It != Memo.end())
      return It->second;
    PartitionCubeForm Best;
    if (llvm::all_of(Values,
                     [&](const auto &V) { return V == Values.front(); })) {
      if (!Charge(1 + 2 * std::popcount(Mask), 4 + 2 * std::popcount(Mask)))
        return {};
      llvm::SmallVector<SymRef, 4> Value{Ctx.mkConst(Values.front())},
          Raw(Value);
      for (unsigned I = 0; I != Axes.size(); ++I)
        if (Mask & (1u << I)) {
          Value.push_back(Axes[I][3]);
          Raw.push_back(Axes[I][2]);
        }
      Best = {Ctx.mkMul(Value), Ctx.mkMul(Raw)};
    } else {
      unsigned Position = 0;
      for (unsigned I = 0; I != Axes.size(); ++I) {
        if (!(Mask & (1u << I)))
          continue;
        if (!Charge(Values.size() * (Words + 1), 3 * Values.size()))
          return {};
        std::vector<llvm::APInt> Low, High, Difference;
        for (unsigned J = 0; J != Values.size(); ++J)
          ((J >> Position) & 1 ? High : Low).push_back(Values[J]);
        ++Position;
        for (unsigned J = 0; J != Low.size(); ++J)
          Difference.push_back(High[J] - Low[J]);
        const unsigned Rest = Mask & ~(1u << I);
        auto L = Self(Self, Rest, Low), H = Self(Self, Rest, High);
        auto D = Self(Self, Rest, Difference);
        if (!L.Value.isValid() || !H.Value.isValid() || !D.Value.isValid())
          return {};
        if (!Charge(24, 48))
          return {};
        auto Consider = [&](SymRef Value, SymRef Raw) {
          if (!Best.Value.isValid() ||
              readingScore(Ctx, Value) < readingScore(Ctx, Best.Value))
            Best = {Value, Raw};
        };
        const auto &A = Axes[I];
        Consider(Ctx.mkAdd(Ctx.mkMul(A[0], L.Value), Ctx.mkMul(A[1], H.Value)),
                 Ctx.mkAdd(Ctx.mkMul(A[0], L.Distribution),
                           Ctx.mkMul(A[1], H.Distribution)));
        Consider(Ctx.mkAdd(Ctx.mkMul(A[3], L.Value), Ctx.mkMul(A[1], D.Value)),
                 Ctx.mkAdd(Ctx.mkMul(A[2], L.Distribution),
                           Ctx.mkMul(A[1], D.Distribution)));
        Consider(Ctx.mkSub(Ctx.mkMul(A[3], H.Value), Ctx.mkMul(A[0], D.Value)),
                 Ctx.mkSub(Ctx.mkMul(A[2], H.Distribution),
                           Ctx.mkMul(A[0], D.Distribution)));
      }
    }
    Memo.emplace(std::move(K), Best);
    return Best;
  };
  return Solve(Solve, (1u << Axes.size()) - 1, Coefficients);
}

} // namespace

static SymRef solveProductCofactorsImpl(
    SymContext &Ctx, SymRef E, const MBAOptions &Opts, WorkBudget &Budget,
    SolveReport &Rep, bool Compress,
    const std::unordered_map<uint32_t, SymRef> *Coordinates = nullptr) {
  auto EffectiveDegree = [&](llvm::ArrayRef<SymRef> Factors) {
    if (!Coordinates)
      return size_t(Factors.size());
    size_t Degree = 0;
    for (SymRef Factor : Factors) {
      auto It = Coordinates->find(Factor.index());
      Degree += It != Coordinates->end() && Ctx.op(It->second) == SymOp::Mul
                    ? Ctx.numOperands(It->second) -
                          unsigned(Ctx.isConst(Ctx.operand(It->second, 0)))
                    : 1;
    }
    return Degree;
  };
  const unsigned MinDegree = limits::kMinProductCofactorDegree;
  // Bound this optional frontier before allocating or distributing enclosing
  // scales. Low-degree regions keep their existing reconstruction path.
  constexpr size_t
      MaxTerms = ::neverd::symbolic::detail::limits::kProductCofactorMaxTerms,
      MaxEdges = ::neverd::symbolic::detail::limits::kProductCofactorMaxEdges,
      MinCompletionTerms = ::neverd::symbolic::detail::limits::
          kProductCofactorMinCompletionTerms;
  std::array<SymRef, MaxTerms> Pending;
  size_t Top = 1, TermsSeen = 0, Edges = 0, Visits = 0;
  unsigned Higher = 0;
  SymRef LastHigher;
  bool NestedSum = false;
  Pending[0] = E;
  while (Top) {
    SymRef R = Pending[--Top];
    if (++Visits > MaxEdges)
      return E;
    if (Ctx.op(R) == SymOp::Mul && Ctx.numOperands(R) == 2 &&
        Ctx.isConst(Ctx.operand(R, 0)))
      R = Ctx.operand(R, 1);
    if (Ctx.op(R) == SymOp::Add) {
      if (Ctx.numOperands(R) > Pending.size() - Top)
        return E;
      for (SymRef Term : Ctx.operands(R))
        Pending[Top++] = Term;
      continue;
    }
    if (++TermsSeen > MaxTerms)
      return E;
    if (Ctx.op(R) != SymOp::Mul)
      continue;
    auto Factors = Ctx.operands(R);
    if (Ctx.isConst(Factors.front()))
      Factors = Factors.drop_front();
    if (Factors.size() > MaxEdges - Edges)
      return E;
    Edges += Factors.size();
    if (EffectiveDegree(Factors) >= MinDegree) {
      ++Higher;
      LastHigher = R;
    }
    NestedSum |= llvm::any_of(
        Factors, [&](SymRef F) { return Ctx.op(F) == SymOp::Add; });
  }
  if (Budget.exhausted())
    return E;
  if (Higher == 1 && hasCompletedResidueReading(Ctx, LastHigher, Budget))
    return E;
  if (Higher < 2)
    return NestedSum ? solveNestedProductCofactors(Ctx, E, Opts, Budget, Rep)
                     : E;

  size_t Storage = Opts.MaxTableBytes;
  const size_t WordBytes =
      ((size_t(Ctx.width(E)) - 1) / 64 + 1) * sizeof(uint64_t);
  auto Charge = [&](size_t Work, size_t Slots = 0) {
    const size_t SlotBytes = 128 + 4 * WordBytes;
    if (Slots > Storage / SlotBytes || !Budget.consume(Work)) {
      Rep.BudgetExhausted = true;
      return false;
    }
    Storage -= Slots * SlotBytes;
    return true;
  };
  auto ChargeOperands = [&](SymOp Op, llvm::ArrayRef<SymRef> Operands,
                            size_t Limit) {
    size_t Count = 0;
    for (SymRef R : Operands) {
      const size_t N = Ctx.op(R) == Op ? Ctx.numOperands(R) : 1;
      if (N > Limit - Count) {
        Rep.BudgetExhausted = true;
        return false;
      }
      Count += N;
    }
    return Charge(Count, Count);
  };
  if (!Charge(Visits + Edges * (Coordinates ? 2 : 1), 2 * (TermsSeen + Edges)))
    return E;
  llvm::DenseMap<uint32_t, SymRef> RestoredNodes;
  auto Actual = [&](SymRef R) {
    if (!Coordinates)
      return R;
    auto It = RestoredNodes.find(R.index());
    if (It != RestoredNodes.end())
      return It->second;
    const size_t Nodes = Ctx.dagSize(R);
    if (!Charge(Nodes, Nodes))
      return SymRef();
    SymRef Restored = Ctx.substitute(R, *Coordinates);
    RestoredNodes.try_emplace(R.index(), Restored);
    return Restored;
  };
  auto Score = [&](SymRef R) {
    SymRef Restored = Actual(R);
    return Restored.isValid()
               ? readingScore(Ctx, Restored)
               : SymReadability{std::numeric_limits<size_t>::max(),
                                std::numeric_limits<size_t>::max()};
  };
  auto FoldParity = [&](SymRef R) {
    SymRef Restored = Actual(R);
    if (!Restored.isValid())
      return R;
    SymRef Folded = foldProductParity(Ctx, Restored, Budget, Storage);
    // A coordinate is an exact opaque abbreviation in this invocation.
    // Instantiate it before using correlations or known low bits. Keep the
    // compact spelling when that extra semantic reading finds no change.
    return Folded == Restored ? R : Folded;
  };
  auto Parsed = splitIntoTerms(Ctx, E);
  if (!Parsed)
    return E;
  auto Terms = std::move(*Parsed);
  auto Write = [&](const PolyTerm &Term) {
    llvm::SmallVector<SymRef, 8> Factors(Term.Factors.begin(),
                                         Term.Factors.end());
    Factors.push_back(Ctx.mkConst(Term.Coeff));
    return Ctx.mkMul(Factors);
  };
  auto Read = [&](SymRef R) {
    PolyTerm Term{llvm::APInt(Ctx.width(E), 1), {}};
    if (Ctx.isConst(R)) {
      Term.Coeff = Ctx.constValue(R);
    } else if (Ctx.op(R) == SymOp::Mul) {
      auto Factors = Ctx.operands(R);
      if (Ctx.isConst(Factors.front())) {
        Term.Coeff = Ctx.constValue(Factors.front());
        Factors = Factors.drop_front();
      }
      Term.Factors.append(Factors.begin(), Factors.end());
    } else {
      Term.Factors.push_back(R);
    }
    return Term;
  };
  llvm::DenseMap<uint32_t, unsigned> Degrees;
  if (Coordinates)
    for (const auto &[Id, Value] : *Coordinates)
      Degrees[Id] = Ctx.op(Value) == SymOp::Mul
                        ? Ctx.numOperands(Value) -
                              unsigned(Ctx.isConst(Ctx.operand(Value, 0)))
                        : 1;
  llvm::SmallVector<uint64_t, 16> ScaleClasses;
  auto ScaleClassOf = [&](const PolyTerm &Term) {
    return Coordinates ? ScaleClasses[&Term - Terms.data()]
                       : uint64_t(Term.Factors.size()) << 32;
  };
  auto Eligible = [&](const PolyTerm &Term) {
    return (ScaleClassOf(Term) >> 32) >= MinDegree;
  };
  SymRef Best = E;
  SymRef Current = E;
  std::map<uint32_t, std::pair<SymRef, unsigned>> QuotientCache;
  auto SolveQuotient = [&](SymRef Q, const MBAOptions &Inner,
                           SolveReport &Local) {
    if (Ctx.op(Q) == SymOp::Add && Ctx.numOperands(Q) == 2) {
      const SymRef A = Ctx.operand(Q, 0), B = Ctx.operand(Q, 1);
      if (isBitwise(Ctx, A) && isBitwise(Ctx, B)) {
        const size_t Nodes = Ctx.dagSize(Q);
        if (Charge(4 * Nodes, Nodes)) {
          SymRef Candidate = isBitwiseComplement(Ctx, A, B)
                                 ? Ctx.mkOnes(Ctx.width(Q))
                                 : foldPartitionedMaskSum(
                                       Ctx, A, B, llvm::APInt(Ctx.width(Q), 0));
          if (Candidate.isValid() &&
              readingScore(Ctx, Candidate) < readingScore(Ctx, Q)) {
            Local.Outcome = MBAOutcome::Rewritten;
            Local.Evidence = MBAEvidence::Derivation;
            return Candidate;
          }
        }
      }
    }
    return solveOneRegion(Ctx, Q, Inner, Budget, Local);
  };
  unsigned Omitted = 1;
  std::array<bool, 2> TriedCompletion{};
  while (!Budget.exhausted()) {
    if (Coordinates) {
      if (!Charge(Terms.size(), Terms.size()))
        break;
      ScaleClasses.clear();
      for (const auto &Term : Terms) {
        if (!Charge(Term.Factors.size()))
          return Best;
        unsigned Degree = 0, Coordinate = 0;
        for (SymRef Factor : Term.Factors) {
          auto It = Degrees.find(Factor.index());
          Degree += It == Degrees.end() ? 1 : It->second;
          if (It != Degrees.end())
            Coordinate = Factor.index();
        }
        ScaleClasses.push_back((uint64_t(Degree) << 32) | Coordinate);
      }
    }

    if (llvm::count_if(Terms, Eligible) < 2)
      break;
    // Try linear quotients first, then quadratic ones only when a pass makes
    // no progress. Neither requires expanding the shared higher product.
    std::map<Monomial, llvm::SmallVector<unsigned, 2>> Groups;
    using PowerKey = llvm::SmallVector<std::pair<uint32_t, unsigned>, 8>;
    llvm::SmallVector<std::pair<PowerKey, llvm::SmallVector<unsigned, 2>>, 16>
        Deferred;
    const bool Defer = Compress && Omitted == 1;
    std::vector<unsigned> Connected;
    if (Defer) {
      Connected.resize(Terms.size());
      std::iota(Connected.begin(), Connected.end(), 0);
    }
    auto FindConnected = [&](unsigned I) {
      while (Connected[I] != I) {
        Connected[I] = Connected[Connected[I]];
        I = Connected[I];
      }
      return I;
    };
    bool SkippedConnectedOmissions = false;
    struct Omission {
      unsigned Term, First, Second;
      llvm::SmallVector<unsigned, 2> Indices;
    };
    llvm::SmallVector<Omission, 16> Omissions;
    std::map<uint64_t, llvm::SmallVector<unsigned, 1>> Buckets;
    auto Hash = [](uint32_t Id) {
      uint64_t H = uint64_t(Id) + 0x9e3779b97f4a7c15ULL;
      H = (H ^ (H >> 30)) * 0xbf58476d1ce4e5b9ULL;
      H = (H ^ (H >> 27)) * 0x94d049bb133111ebULL;
      return H ^ (H >> 31);
    };
    bool Complete = true;
    // After completion only a few long residuals may remain. Intersect
    // term pairs instead of enumerating every one/two-factor omission of each
    // term. The shared keys and their lexicographic order are unchanged.
    const bool Pairwise =
        Terms.size() <= 16 && llvm::any_of(Terms, [](const PolyTerm &T) {
          return T.Factors.size() >= 8;
        });
    if (Pairwise) {
      using Powers = llvm::SmallVector<std::pair<uint32_t, unsigned>, 8>;
      llvm::SmallVector<Powers, 16> Compressed;
      for (const auto &Term : Terms) {
        if (!Charge(Term.Factors.size(), Term.Factors.size() + 1)) {
          Complete = false;
          break;
        }
        Powers P;
        for (SymRef Factor : Term.Factors) {
          if (!P.empty() && P.back().first == Factor.index())
            ++P.back().second;
          else
            P.emplace_back(Factor.index(), 1);
        }
        Compressed.push_back(std::move(P));
      }
      std::map<Powers, llvm::SmallVector<unsigned, 2>> Shared;
      for (unsigned A = 0; A != Terms.size() && Complete; ++A)
        for (unsigned B = A + 1; B != Terms.size() && Complete; ++B) {
          if (!Charge(1)) {
            Complete = false;
            break;
          }
          const size_t Degree = Terms[A].Factors.size();
          if (!Eligible(Terms[A]) || Degree < Omitted ||
              Degree != Terms[B].Factors.size())
            continue;
          const auto &Left = Compressed[A], &Right = Compressed[B];
          if (!Charge(Left.size() + Right.size(), Left.size())) {
            Complete = false;
            break;
          }
          Powers Common;
          size_t I = 0, J = 0, Count = 0;
          while (I != Left.size() && J != Right.size()) {
            if (Left[I].first < Right[J].first)
              ++I;
            else if (Right[J].first < Left[I].first)
              ++J;
            else {
              unsigned Power = std::min(Left[I].second, Right[J].second);
              Common.emplace_back(Left[I].first, Power);
              Count += Power;
              ++I;
              ++J;
            }
          }
          const size_t Required = Degree - Omitted;
          if (Count < Required)
            continue;
          const size_t Extra = Count - Required;
          auto Insert = [&](size_t I, size_t J) {
            if (!Charge(Common.size() + 2, Common.size() + 4))
              return false;
            Powers Key;
            for (size_t K = 0; K != Common.size(); ++K) {
              unsigned Power =
                  Common[K].second - unsigned(K == I) - unsigned(K == J);
              if (Power)
                Key.emplace_back(Common[K].first, Power);
            }
            auto &Indices = Shared[std::move(Key)];
            Indices.push_back(A);
            Indices.push_back(B);
            return true;
          };
          if (Extra == 0) {
            Complete = Insert(Common.size(), Common.size());
          } else {
            for (size_t I = 0; I != Common.size() && Complete; ++I) {
              if (Extra == 1) {
                Complete = Insert(I, Common.size());
                continue;
              }
              for (size_t J = I; J != Common.size() && Complete; ++J) {
                if (I == J && Common[I].second < 2)
                  continue;
                Complete = Insert(I, J);
              }
            }
          }
        }
      // Each compressed multiset expands once, after pair comparisons. The
      // ordinary keys preserve the solver's established lexicographic order.
      for (auto &[Powers, Indices] : Shared) {
        size_t Degree = 0;
        for (const auto &[Id, Count] : Powers)
          Degree += Count;
        if (!Complete ||
            !Charge(Indices.size() + (Defer ? Powers.size() : Degree),
                    (Defer ? Powers.size() : Degree) + 2)) {
          Complete = false;
          break;
        }
        llvm::sort(Indices);
        Indices.erase(std::unique(Indices.begin(), Indices.end()),
                      Indices.end());
        if (Defer) {
          Deferred.emplace_back(Powers, std::move(Indices));
          continue;
        }
        Monomial Key;
        for (const auto &[Id, Count] : Powers)
          Key.append(Count, Id);
        Groups.emplace(std::move(Key), std::move(Indices));
      }
    } else {
      using Powers = llvm::SmallVector<std::pair<uint32_t, unsigned>, 8>;
      if (!Charge(Terms.size(), Terms.size()))
        break;
      std::vector<Powers> Compressed(Terms.size());
      for (unsigned T = 0; T != Terms.size() && Complete; ++T) {
        const auto &Factors = Terms[T].Factors;
        if (!Eligible(Terms[T]) || Factors.size() < Omitted)
          continue;
        // Short keys cost less to copy than to fingerprint and compare again.
        if (Factors.size() < 12) {
          for (unsigned I = 0; I != Factors.size() && Complete; ++I) {
            if (I && Factors[I] == Factors[I - 1])
              continue;
            for (unsigned J = Omitted == 1 ? I : I + 1; J != Factors.size();
                 ++J) {
              if (J > I + 1 && Factors[J] == Factors[J - 1])
                continue;
              if (!Charge(Factors.size(), Factors.size() + 2)) {
                Complete = false;
                break;
              }
              Monomial Key;
              for (unsigned K = 0; K != Factors.size(); ++K)
                if (K != I && (Omitted == 1 || K != J))
                  Key.push_back(Factors[K].index());
              Groups[std::move(Key)].push_back(T);
              if (Omitted == 1)
                break;
            }
          }
          continue;
        }
        if (!Charge(Factors.size(), Factors.size())) {
          Complete = false;
          break;
        }
        auto &Runs = Compressed[T];
        for (SymRef Factor : Factors) {
          if (!Runs.empty() && Runs.back().first == Factor.index())
            ++Runs.back().second;
          else
            Runs.emplace_back(Factor.index(), 1);
        }
        if (!Charge(2 * Runs.size(), Runs.size())) {
          Complete = false;
          break;
        }
        llvm::SmallVector<uint64_t, 16> Hashes;
        uint64_t Fingerprint = 0;
        for (const auto &[Id, Count] : Runs) {
          Hashes.push_back(Hash(Id));
          Fingerprint += Hashes.back() * Count;
        }
        for (unsigned I = 0; I != Runs.size(); ++I) {
          for (unsigned J = I; J != Runs.size(); ++J) {
            if (Omitted == 2 && I == J && Runs[I].second < 2)
              continue;
            if (!Charge(3, 4)) {
              Complete = false;
              break;
            }
            const auto Key =
                Fingerprint - Hashes[I] - (Omitted == 2 ? Hashes[J] : 0);
            auto &Bucket = Buckets[Key];
            unsigned Match = Omissions.size();
            for (unsigned Index : Bucket) {
              const auto &Previous = Omissions[Index];
              const auto &Other = Compressed[Previous.Term];
              if (!Charge(1)) {
                Complete = false;
                break;
              }
              if (Factors.size() != Terms[Previous.Term].Factors.size())
                continue;
              if (Defer && FindConnected(T) == FindConnected(Previous.Term)) {
                SkippedConnectedOmissions = true;
                continue;
              }
              // A hash only selects comparisons. Compare exact remaining
              // powers, including two removals from the same repeated factor.
              // Equal runs advance together. Only the at most Omitted empty
              // runs on the opposite side can require additional iterations.
              if (!Charge(std::max(Runs.size(), Other.size()) + Omitted)) {
                Complete = false;
                break;
              }
              unsigned A = 0, B = 0;
              bool Equal = true;
              while (A != Runs.size() || B != Other.size()) {
                unsigned AC = A == Runs.size()
                                  ? 0
                                  : Runs[A].second - unsigned(A == I) -
                                        unsigned(Omitted == 2 && A == J);
                unsigned BC =
                    B == Other.size()
                        ? 0
                        : Other[B].second - unsigned(B == Previous.First) -
                              unsigned(Omitted == 2 && B == Previous.Second);
                if (!AC && A != Runs.size()) {
                  ++A;
                  continue;
                }
                if (!BC && B != Other.size()) {
                  ++B;
                  continue;
                }
                if (AC != BC || !AC || Runs[A].first != Other[B].first) {
                  Equal = false;
                  break;
                }
                ++A;
                ++B;
              }
              if (Equal) {
                Match = Index;
                if (Defer)
                  Connected[FindConnected(T)] = FindConnected(Previous.Term);
                break;
              }
            }
            if (!Complete)
              break;
            if (Match == Omissions.size()) {
              Bucket.push_back(Match);
              Omissions.push_back({T, I, J, {T}});
            } else if (Omissions[Match].Indices.back() != T) {
              Omissions[Match].Indices.push_back(T);
            }
            if (Omitted == 1)
              break;
          }
          if (!Complete)
            break;
        }
      }
      // Singleton omissions cannot supply a relation or a cofactor rewrite.
      // Materialize only shared keys, preserving the former lexicographic
      // order.
      for (auto &Entry : Omissions) {
        if (!Complete)
          break;
        if (Entry.Indices.size() < 2)
          continue;
        const auto &Factors = Terms[Entry.Term].Factors;
        if (Defer) {
          const auto &Runs = Compressed[Entry.Term];
          if (!Charge(Runs.size(), Runs.size() + 2)) {
            Complete = false;
            break;
          }
          PowerKey Key;
          for (unsigned K = 0; K != Runs.size(); ++K) {
            const unsigned Count = Runs[K].second - unsigned(K == Entry.First);
            if (Count)
              Key.emplace_back(Runs[K].first, Count);
          }
          Deferred.emplace_back(std::move(Key), std::move(Entry.Indices));
          continue;
        }
        if (!Charge(Factors.size(), Factors.size() + 2)) {
          Complete = false;
          break;
        }
        Monomial Key;
        const auto &Runs = Compressed[Entry.Term];
        for (unsigned K = 0; K != Runs.size(); ++K) {
          const unsigned Count = Runs[K].second - unsigned(K == Entry.First) -
                                 unsigned(Omitted == 2 && K == Entry.Second);
          Key.append(Count, Runs[K].first);
        }
        Groups.emplace(std::move(Key), std::move(Entry.Indices));
      }
    }
    // Preserve the lexicographic order of expanded keys without materializing
    // repeated powers just to connect their term families.
    llvm::sort(Deferred, [](const auto &L, const auto &R) {
      const auto &A = L.first, &B = R.first;
      size_t I = 0, J = 0;
      while (I != A.size() && J != B.size()) {
        if (A[I].first != B[J].first)
          return A[I].first < B[J].first;
        if (A[I].second != B[J].second)
          return A[I].second < B[J].second ? I + 1 == A.size()
                                           : J + 1 != B.size();
        ++I;
        ++J;
      }
      return I == A.size() && J != B.size();
    });
    // A connected family of one-factor quotients has an invariant common
    // monomial. Give that monomial one independent coordinate while solving
    // the varying frontier, and substitute its exact value back afterwards.
    // The coordinate proof holds for every full-width value of the monomial.
    if (Compress && Omitted == 1 && Complete) {
      Compress = false;
      std::vector<unsigned> Parents = std::move(Connected);
      auto Find = [&](unsigned I) {
        while (Parents[I] != I) {
          Parents[I] = Parents[Parents[I]];
          I = Parents[I];
        }
        return I;
      };
      for (const auto &[Key, Indices] : Groups) {
        if (!Charge(Indices.size()))
          return Best;
        for (unsigned I : llvm::ArrayRef(Indices).drop_front())
          Parents[Find(I)] = Find(Indices.front());
      }
      for (const auto &[Key, Indices] : Deferred) {
        if (!Charge(Indices.size()))
          return Best;
        for (unsigned I : llvm::ArrayRef(Indices).drop_front())
          Parents[Find(I)] = Find(Indices.front());
      }
      std::map<unsigned, llvm::SmallVector<unsigned, 8>> Components;
      for (unsigned I = 0; I != Terms.size(); ++I)
        Components[Find(I)].push_back(I);
      std::unordered_map<uint32_t, SymRef> Restore;
      llvm::SmallVector<SymRef, 16> ShortTerms, ShortComponents;
      unsigned CompressedComponents = 0;
      bool KeepsCompletionSearch = true;
      for (const auto &[Root, Indices] : Components) {
        if (Terms[Indices.front()].Factors.size() >= MinDegree &&
            Indices.size() < MinCompletionTerms)
          KeepsCompletionSearch = false;
        const size_t Begin = ShortTerms.size();
        llvm::SmallVector<SymRef, 16> Common;
        if (Indices.size() >= 2) {
          Common.append(Terms[Indices.front()].Factors);
          for (unsigned I : llvm::ArrayRef(Indices).drop_front()) {
            if (!Charge(Common.size() + Terms[I].Factors.size(), Common.size()))
              return Best;
            llvm::SmallVector<SymRef, 16> Next;
            std::set_intersection(
                Common.begin(), Common.end(), Terms[I].Factors.begin(),
                Terms[I].Factors.end(), std::back_inserter(Next));
            Common = std::move(Next);
            if (Common.size() < limits::kMinCompressedCommonFactors)
              break;
          }
        }
        if (Common.size() < limits::kMinCompressedCommonFactors) {
          for (unsigned I : Indices)
            ShortTerms.push_back(Write(Terms[I]));
          ShortComponents.push_back(
              Ctx.mkAdd(llvm::ArrayRef(ShortTerms).drop_front(Begin)));
          continue;
        }
        // A sole remaining sum will be flattened by quotient construction.
        // Refuse an oversized frontier before allocating proxy expressions.
        for (unsigned I : Indices) {
          const auto &T = Terms[I];
          if (T.Factors.size() != Common.size() + 1 ||
              T.Coeff != Terms[Indices.front()].Coeff)
            continue;
          if (!Charge(T.Factors.size()))
            return Best;
          size_t J = 0;
          for (SymRef F : T.Factors) {
            if (J != Common.size() && Common[J] == F) {
              ++J;
            } else if (Ctx.op(F) == SymOp::Add &&
                       Ctx.numOperands(F) > MaxTerms) {
              Rep.BudgetExhausted = true;
              return Best;
            }
          }
        }
        if (!Charge(Common.size(), Common.size() + 2))
          return Best;
        const SymRef Proxy = Ctx.mkFreshVar(Ctx.width(E), "mba.fixed");
        Restore.emplace(Proxy.index(), Ctx.mkMul(Common));
        for (unsigned I : Indices) {
          if (!Charge(Terms[I].Factors.size(), Terms[I].Factors.size() + 2))
            return Best;
          PolyTerm Short{Terms[I].Coeff, {}};
          std::set_difference(Terms[I].Factors.begin(), Terms[I].Factors.end(),
                              Common.begin(), Common.end(),
                              std::back_inserter(Short.Factors));
          Short.Factors.push_back(Proxy);
          ShortTerms.push_back(Write(Short));
        }
        ++CompressedComponents;
        ShortComponents.push_back(
            Ctx.mkAdd(llvm::ArrayRef(ShortTerms).drop_front(Begin)));
      }
      if (!Restore.empty()) {
        if (!Charge(ShortTerms.size(), ShortTerms.size()))
          return Best;
        const SymRef Short = Ctx.mkAdd(ShortTerms);
        const SymRef Restored = Ctx.substitute(Short, Restore);
        if (Restored != Current &&
            !proveArithmeticIdentity(Ctx, Current, Restored, Budget, Storage,
                                     false))
          return Best;
        SolveReport Local;
        MBAOptions Inner = Opts;
        Inner.MaxTableBytes = Storage;
        SymRef Reduced;
        if (CompressedComponents >= 2 && KeepsCompletionSearch) {
          llvm::SmallVector<SymRef, 16> Parts;
          for (SymRef Part : ShortComponents) {
            SolveReport PartReport;
            const SymRef Value = solveProductCofactorsImpl(
                Ctx, Part, Inner, Budget, PartReport, false, &Restore);
            Parts.push_back(Value);
            Local.BudgetExhausted |= PartReport.BudgetExhausted;
            Local.NumAtoms = std::max(Local.NumAtoms, PartReport.NumAtoms);
            if (Value != Part)
              Local.Evidence = MBAEvidence::Derivation;
          }
          Reduced = Ctx.mkAdd(Parts);
        } else {
          Reduced = solveProductCofactorsImpl(Ctx, Short, Inner, Budget, Local,
                                              false, &Restore);
        }
        Rep.BudgetExhausted |= Local.BudgetExhausted;
        if (Reduced != Short && Local.Evidence == MBAEvidence::Derivation) {
          SymRef Candidate = Ctx.substitute(Reduced, Restore);
          if (Score(Candidate) < Score(Best)) {
            Rep.NumAtoms = std::max(Rep.NumAtoms, Local.NumAtoms);
            Rep.Outcome = MBAOutcome::Rewritten;
            Rep.Evidence = MBAEvidence::Derivation;
            return Candidate;
          }
        }
      }
      if (SkippedConnectedOmissions)
        continue;
    }
    for (auto &[Powers, Indices] : Deferred) {
      size_t Degree = 0;
      for (const auto &[Id, Count] : Powers)
        Degree += Count;
      if (!Charge(Degree, Degree + 2)) {
        Complete = false;
        break;
      }
      Monomial Key;
      for (const auto &[Id, Count] : Powers)
        Key.append(Count, Id);
      Groups.emplace(std::move(Key), std::move(Indices));
    }
    // Reserve the complete reconstruction before any group can commit.
    if (!Complete || !Charge(Terms.size() + Edges, Terms.size() + Edges))
      break;

    // Reconstruct a complete product in a proved local factor basis, keeping
    // every missing or disturbed coefficient as an exact residual. Completing
    // the whole Cartesian block avoids committing a temporarily larger partial
    // transport when several independent partitions contain the same hole.
    // Keep small frontiers on the cheaper local cofactor path.
    if (!TriedCompletion[Omitted - 1] &&
        llvm::count_if(Terms, Eligible) >= MinCompletionTerms) {
      TriedCompletion[Omitted - 1] = true;
      struct Relation {
        SymRef Sum, Reduced;
        std::array<llvm::SmallVector<SymRef, 2>, 2> Pieces;
      };
      llvm::SmallVector<Relation, 8> Relations;
      std::set<uint32_t> Seen;
      for (const auto &[Key, Indices] : Groups) {
        if (Indices.size() != 2 || Budget.exhausted())
          continue;
        if (!Charge(2 * (Key.size() + Omitted + 1),
                    2 * (Key.size() + Omitted + 3)))
          break;
        Relation Rel;
        for (unsigned I = 0; I != 2; ++I) {
          size_t K = 0;
          for (SymRef Factor : Terms[Indices[I]].Factors)
            if (K != Key.size() && Factor.index() == Key[K])
              ++K;
            else
              Rel.Pieces[I].push_back(Factor);
          assert(K == Key.size() && Rel.Pieces[I].size() == Omitted &&
                 "completion relation must retain exact multiplicities");
        }
        auto Left = Ctx.mkMul(Rel.Pieces[0]), Right = Ctx.mkMul(Rel.Pieces[1]);
        if (!ChargeOperands(SymOp::Add, {Left, Right}, MaxTerms))
          break;
        Rel.Sum = Ctx.mkAdd(Left, Right);
        if (!Seen.insert(Rel.Sum.index()).second)
          continue;
        if (auto It = QuotientCache.find(Rel.Sum.index());
            It != QuotientCache.end())
          Rel.Reduced = It->second.first;
        else {
          SolveReport Local;
          MBAOptions Inner = Opts;
          Inner.MaxTableBytes = Storage;
          if (Omitted == 2) {
            for (unsigned I = 0; I != 2 && !Rel.Reduced.isValid(); ++I)
              for (unsigned J = 0; J != 2 && !Rel.Reduced.isValid(); ++J) {
                if (Rel.Pieces[0][I] != Rel.Pieces[1][J])
                  continue;
                if (!Charge(4, 4))
                  break;
                SymRef Common = Rel.Pieces[0][I];
                SymRef Sum =
                    Ctx.mkAdd(Rel.Pieces[0][1 - I], Rel.Pieces[1][1 - J]);
                auto Known = QuotientCache.find(Sum.index());
                if (Known == QuotientCache.end() || Known->second.first == Sum)
                  continue;
                // Lift an already proved quotient under its exact common
                // factor. Check distribution independently before composing
                // the stored full-width quotient identity.
                if (proveArithmeticIdentity(Ctx, Rel.Sum,
                                            Ctx.mkMul(Common, Sum), Budget,
                                            Storage, false)) {
                  Rel.Reduced = Ctx.mkMul(Common, Known->second.first);
                  Local.NumAtoms = Known->second.second;
                  Local.Evidence = MBAEvidence::Derivation;
                }
              }
          }
          if (!Rel.Reduced.isValid())
            Rel.Reduced = SolveQuotient(Rel.Sum, Inner, Local);
          Rep.BudgetExhausted |= Local.BudgetExhausted;
          // Completed failures are reusable too: this call's options stay
          // fixed and its available work/storage only decrease. Do not cache
          // a refusal or an incomplete search as a negative result.
          if (Rel.Reduced == Rel.Sum && !Local.BudgetExhausted &&
              !Budget.exhausted())
            QuotientCache.try_emplace(Rel.Sum.index(), Rel.Sum, 0);
          if (Rel.Reduced == Rel.Sum ||
              Local.Evidence != MBAEvidence::Derivation)
            continue;
          QuotientCache.try_emplace(Rel.Sum.index(), Rel.Reduced,
                                    Local.NumAtoms);
          Rep.NumAtoms = std::max(Rep.NumAtoms, Local.NumAtoms);
        }
        if (Rel.Reduced == Rel.Sum)
          continue;
        Relations.push_back(std::move(Rel));
      }
      // Keep the first relation in the established order for each exact
      // piece. A seed probes the factor pairs it actually contains instead
      // of scanning every relation with the same first factor.
      std::map<std::pair<uint32_t, uint32_t>, unsigned> PieceIndex;
      for (unsigned R = 0; R != Relations.size(); ++R)
        for (unsigned Side = 0; Side != 2; ++Side) {
          const auto &Piece = Relations[R].Pieces[Side];
          if (!Charge(Piece.size(), 2))
            break;
          PieceIndex.try_emplace(
              std::make_pair(Piece.front().index(),
                             Omitted == 2 ? Piece.back().index() : 0),
              R);
        }
      SymRef Completion;
      Seen.clear();
      std::map<uint64_t, std::pair<llvm::APInt, unsigned>> Scales;
      struct Frequency {
        uint64_t Degree;
        llvm::APInt Coefficient;
        unsigned Count;
      };
      llvm::SmallVector<Frequency, 8> Frequencies;
      for (const auto &Term : Terms) {
        const uint64_t Degree = ScaleClassOf(Term);
        if (!Eligible(Term))
          continue;
        bool Counted = false;
        for (auto &Entry : Frequencies) {
          if (!Charge(1 + WordBytes / 8))
            return Best;
          if (Entry.Degree == Degree && Entry.Coefficient == Term.Coeff) {
            ++Entry.Count;
            Counted = true;
            break;
          }
        }
        if (!Counted) {
          if (!Charge(1, 2))
            return Best;
          Frequencies.push_back({Degree, Term.Coeff, 1});
        }
      }
      // Count each distinct scale once. Recounting the complete frontier for
      // every term made a uniform 64-product expansion pay quadratically.
      for (const auto &Entry : Frequencies) {
        if (!Charge(1 + WordBytes / 8, 2))
          return Best;
        auto It = Scales.find(Entry.Degree);
        if (It == Scales.end() || Entry.Count > It->second.second)
          Scales.insert_or_assign(
              Entry.Degree, std::make_pair(Entry.Coefficient, Entry.Count));
      }
      for (const auto &Seed : Terms) {
        if (!Eligible(Seed) || Seed.Factors.size() < Omitted ||
            Relations.size() < 2 || Budget.exhausted() ||
            !Scales.count(ScaleClassOf(Seed)))
          continue;
        const llvm::APInt *Scale = &Scales.at(ScaleClassOf(Seed)).first;
        if (!Charge(Seed.Factors.size(), Seed.Factors.size()))
          break;
        llvm::SmallVector<std::pair<SymRef, unsigned>, 16> Available;
        for (SymRef Factor : Seed.Factors) {
          if (!Available.empty() && Available.back().first == Factor)
            ++Available.back().second;
          else
            Available.emplace_back(Factor, 1);
        }
        llvm::SmallVector<SymRef, 16> Folded, Expanded;
        llvm::SmallVector<std::pair<const Relation *, SymRef>, 16> Steps;
        bool Fits = true;
        unsigned Axes = 0;
        for (unsigned I = 0; I != Available.size() && Fits;) {
          auto &[Factor, Count] = Available[I];
          if (!Count) {
            ++I;
            continue;
          }
          unsigned Chosen = Relations.size(), Second = I;
          if (Omitted == 1) {
            if (!Charge(1)) {
              Fits = false;
              break;
            }
            auto It = PieceIndex.find({Factor.index(), 0});
            if (It != PieceIndex.end())
              Chosen = It->second;
          } else {
            for (unsigned J = I; J != Available.size(); ++J) {
              if (!Charge(1)) {
                Fits = false;
                break;
              }
              if (Available[J].second <= unsigned(J == I))
                continue;
              auto It =
                  PieceIndex.find({Factor.index(), Available[J].first.index()});
              if (It != PieceIndex.end() && It->second < Chosen) {
                Chosen = It->second;
                Second = J;
              }
            }
          }
          if (!Fits)
            break;
          --Count;
          if (Chosen != Relations.size()) {
            if (Omitted == 2)
              --Available[Second].second;
            const Relation *Rel = &Relations[Chosen];
            Steps.emplace_back(Rel, Factor);
            Folded.push_back(Rel->Reduced);
            ++Axes;
            // Later relations can only add axes. Once this complete product
            // cannot fit the existing Cartesian bound, no remaining factor
            // can make the seed admissible again.
            if (Axes >= std::numeric_limits<size_t>::digits ||
                (size_t(1) << Axes) > MaxTerms)
              Fits = false;
          } else {
            Steps.emplace_back(nullptr, Factor);
            Folded.push_back(Factor);
          }
        }
        // Every relation doubles the Cartesian expansion. Refuse a complete
        // unaffordable plan before copying prefixes that cannot be checked.
        if (!Fits || Axes < 2 || Axes >= std::numeric_limits<size_t>::digits ||
            (size_t(1) << Axes) > MaxTerms ||
            !ChargeOperands(SymOp::Mul, Folded, MaxEdges))
          continue;
        Folded.push_back(Ctx.mkConst(*Scale));
        SymRef Product = Ctx.mkMul(Folded);
        if (!Seen.insert(Product.index()).second)
          continue;
        // Duplicate folded products need no distribution spelling.
        for (const auto &Step : Steps)
          Expanded.push_back(Step.first ? Step.first->Sum : Step.second);
        if (!ChargeOperands(SymOp::Mul, Expanded, MaxEdges))
          continue;
        Expanded.push_back(Ctx.mkConst(*Scale));
        Product = FoldParity(Product);
        std::map<Monomial, llvm::APInt> Differences;
        auto Accumulate = [&](Monomial Key, const llvm::APInt &Coefficient) {
          if (!Charge(Key.size() + WordBytes / 8 + 1, Key.size() + 2))
            return false;
          llvm::sort(Key);
          auto It =
              Differences
                  .try_emplace(std::move(Key), llvm::APInt(Ctx.width(E), 0))
                  .first;
          It->second += Coefficient;
          return true;
        };
        for (const auto &Term : Terms) {
          Monomial Key;
          for (SymRef F : Term.Factors)
            Key.push_back(F.index());
          Fits &= Accumulate(std::move(Key), Term.Coeff);
          if (!Fits)
            break;
        }
        // Signed copies and changes confined to the high bit benefit from
        // the parity-aware completion below. The tensor grammar is useful
        // when coefficients also differ in their lower magnitude bits.
        bool Cube = Omitted == 1 && Axes <= limits::kMaxPartitionCubeAxes &&
                    Terms.size() <= (size_t(1) << Axes);
        if (Cube) {
          if (!Charge(4 * Terms.size() * WordBytes / 8, 4 * Terms.size())) {
            Fits = false;
            break;
          }
          const auto LowMask =
              llvm::APInt::getLowBitsSet(Ctx.width(E), Ctx.width(E) - 1);
          Cube = llvm::any_of(Terms, [&](const PolyTerm &T) {
            return !T.Coeff.isZero() &&
                   (T.Coeff & LowMask) != (*Scale & LowMask) &&
                   (T.Coeff & LowMask) != ((-*Scale) & LowMask);
          });
        }
        unsigned Covered = 0;
        std::vector<llvm::APInt> CubeCoefficients;
        std::set<Monomial> CubeKeys;
        // Materialize each complete corner once. Growing every prefix would
        // repeatedly copy unchanged factors, especially in long products with
        // only a few varying axes. The corner bits retain all multiplicities.
        for (size_t Corner = 0; Fits && Corner != (size_t(1) << Axes);
             ++Corner) {
          if (!Charge(Steps.size() + Seed.Factors.size(),
                      Seed.Factors.size())) {
            Fits = false;
            break;
          }
          Monomial Key;
          Key.reserve(Seed.Factors.size());
          unsigned Axis = 0;
          for (const auto &Step : Steps) {
            if (Step.first) {
              for (SymRef F : Step.first->Pieces[(Corner >> Axis) & 1])
                Key.push_back(F.index());
              ++Axis;
            } else {
              Key.push_back(Step.second.index());
            }
          }
          if (Cube) {
            if (!Charge(Key.size() + WordBytes / 8 + 1, Key.size() + 2)) {
              Fits = false;
              break;
            }
            llvm::sort(Key);
            if (!CubeKeys.insert(Key).second)
              Cube = false;
            auto It = Differences.find(Key);
            Covered += It != Differences.end();
            CubeCoefficients.push_back(It == Differences.end()
                                           ? llvm::APInt(Ctx.width(E), 0)
                                           : It->second);
          }
          Fits = Accumulate(std::move(Key), -*Scale);
        }
        if (!Fits)
          break;
        SymRef CubeAlternative;
        if (Cube && Covered == Terms.size() &&
            CubeCoefficients.size() == (size_t(1) << Axes)) {
          if (!Charge(Steps.size(), 8 * Steps.size()))
            break;
          llvm::SmallVector<std::array<SymRef, 4>, 3> CubeAxes;
          llvm::SmallVector<SymRef, 16> Common;
          for (const auto &Step : Steps) {
            if (Step.first) {
              const auto &R = *Step.first;
              CubeAxes.push_back(
                  {R.Pieces[0][0], R.Pieces[1][0], R.Sum, R.Reduced});
            } else {
              Common.push_back(Step.second);
            }
          }
          auto CubeResult = factorPartitionCube(Ctx, CubeAxes, CubeCoefficients,
                                                Budget, Storage);
          if (!CubeResult.Value.isValid())
            Rep.BudgetExhausted = true;
          if (CubeResult.Value.isValid()) {
            Common.push_back(CubeResult.Value);
            if (!ChargeOperands(SymOp::Mul, Common, MaxEdges))
              break;
            const SymRef Candidate = Ctx.mkMul(Common);
            Common.back() = CubeResult.Distribution;
            if (!ChargeOperands(SymOp::Mul, Common, MaxEdges))
              break;
            const SymRef Distribution = Ctx.mkMul(Common);
            if (Score(Candidate) < Score(Best) &&
                proveArithmeticIdentity(Ctx, Current, Distribution, Budget,
                                        Storage, false)) {
              CubeAlternative = Candidate;
            }
          }
        }
        llvm::SmallVector<SymRef, 16> Residuals;
        for (const auto &[Key, Coefficient] : Differences) {
          if (Coefficient.isZero())
            continue;
          PolyTerm Term{Coefficient, {}};
          for (auto Id : Key)
            Term.Factors.push_back(SymRef(Id));
          Residuals.push_back(Write(Term));
        }
        llvm::SmallVector<SymRef, 16> DistributionResiduals(Residuals);
        // Complete the original distribution first. Parity projection is an
        // independent exact step and must not erase factors from that proof.
        for (SymRef &Residual : Residuals)
          Residual = FoldParity(Residual);
        Residuals.push_back(Product);
        if (!ChargeOperands(SymOp::Add, Residuals, MaxTerms))
          continue;
        SymRef Candidate = Ctx.mkAdd(Residuals);
        if (CubeAlternative.isValid() &&
            Score(CubeAlternative) < Score(Candidate)) {
          Rep.Outcome = MBAOutcome::Rewritten;
          Rep.Evidence = MBAEvidence::Derivation;
          Rep.BudgetExhausted |= Budget.exhausted();
          return CubeAlternative;
        }
        if (Score(Candidate) >= Score(Current) ||
            (Completion.isValid() && Score(Candidate) >= Score(Completion)))
          continue;
        DistributionResiduals.push_back(Ctx.mkMul(Expanded));
        if (!ChargeOperands(SymOp::Add, DistributionResiduals, MaxTerms))
          continue;
        if (proveArithmeticIdentity(Ctx, Current,
                                    Ctx.mkAdd(DistributionResiduals), Budget,
                                    Storage, /*ExpandComplements=*/false))
          Completion = Candidate;
      }
      if (Completion.isValid()) {
        Current = Completion;
        if (Score(Current) < Score(Best))
          Best = Current;
        Rep.Outcome = MBAOutcome::Rewritten;
        Rep.Evidence = MBAEvidence::Derivation;
        auto CompletedTerms = splitIntoTerms(Ctx, Current);
        if (!CompletedTerms)
          break;
        Terms = std::move(*CompletedTerms);
        Omitted = 1;
        continue;
      }
    }
    std::vector<bool> Used(Terms.size(), false);
    llvm::SmallVector<PolyTerm, 8> Next;
    llvm::SmallVector<SymRef, 16> Distribution;
    for (const auto &[Key, Indices] : Groups) {
      if (Budget.exhausted())
        break;
      if (llvm::count_if(Indices, [&](unsigned T) { return !Used[T]; }) < 2)
        continue;
      if (!Charge(Indices.size() * (Key.size() + 2),
                  Indices.size() * (Key.size() + 4)))
        break;
      llvm::SmallVector<SymRef, 8> Common, Quotients, Originals;
      for (uint32_t Id : Key)
        Common.push_back(SymRef(Id));
      auto First = llvm::find_if(Indices, [&](unsigned T) { return !Used[T]; });
      const auto &Scale = Terms[*First].Coeff;
      const bool SameScale = llvm::all_of(Indices, [&](unsigned T) {
        return Used[T] || Terms[T].Coeff == Scale;
      });
      if (SameScale)
        Common.push_back(Ctx.mkConst(Scale));
      for (unsigned T : Indices) {
        if (Used[T])
          continue;
        const auto &Term = Terms[T];
        llvm::SmallVector<SymRef, 8> Remaining;
        size_t K = 0;
        for (SymRef Factor : Term.Factors)
          if (K != Key.size() && Factor.index() == Key[K])
            ++K;
          else
            Remaining.push_back(Factor);
        assert(K == Key.size() && "cofactor must retain exact multiplicities");
        if (!SameScale)
          Remaining.push_back(Ctx.mkConst(Term.Coeff));
        Quotients.push_back(Ctx.mkMul(Remaining));
        Originals.push_back(Write(Term));
      }
      if (!ChargeOperands(SymOp::Add, Quotients, MaxTerms) ||
          !ChargeOperands(SymOp::Add, Originals, MaxTerms) ||
          !ChargeOperands(SymOp::Mul, Common, MaxEdges))
        break;
      const SymRef WeightedQuotient = Ctx.mkAdd(Quotients);
      SymRef Quotient = WeightedQuotient;
      SolveReport Local;
      SymRef Reduced = Quotient;
      auto Solve = [&] {
        Local = {};
        Reduced = Quotient;
        if (!Charge(1, 2))
          return false;
        if (auto It = QuotientCache.find(Quotient.index());
            It != QuotientCache.end()) {
          Reduced = It->second.first;
          Local.NumAtoms = It->second.second;
          Local.Evidence = MBAEvidence::Derivation;
        } else {
          MBAOptions Inner = Opts;
          Inner.MaxTableBytes = Storage;
          Reduced = SolveQuotient(Quotient, Inner, Local);
          Rep.BudgetExhausted |= Local.BudgetExhausted;
          if (Reduced != Quotient && Local.Evidence == MBAEvidence::Derivation)
            QuotientCache.try_emplace(Quotient.index(), Reduced,
                                      Local.NumAtoms);
          else if (Reduced == Quotient && !Local.BudgetExhausted &&
                   !Budget.exhausted())
            QuotientCache.try_emplace(Quotient.index(), Quotient, 0);
        }
        return Reduced != Quotient && Local.Evidence == MBAEvidence::Derivation;
      };
      llvm::SmallVector<PolyTerm, 4> Residuals;
      llvm::SmallVector<SymRef, 4> ResidualExprs;
      if (!SameScale) {
        // A changed coefficient need not hide the common identity. Write
        // sum(c_i*q_i) = k*sum(q_i) + sum((c_i-k)*q_i), retaining every
        // difference modulo the word width. No coefficient is divided.
        // Prefer a scale repeated elsewhere at this degree, so successive
        // groups can expose the same product even across a sparse disturbance.
        if (!Charge(Indices.size() * Terms.size() * (WordBytes / 8),
                    Indices.size() * (Key.size() + Omitted + 4)))
          break;
        unsigned ScaleIndex = *First, Frequency = 0;
        for (unsigned T : Indices) {
          if (Used[T])
            continue;
          unsigned Count = llvm::count_if(Terms, [&](const PolyTerm &Term) {
            return ScaleClassOf(Term) == ScaleClassOf(Terms[T]) &&
                   Term.Coeff == Terms[T].Coeff;
          });
          if (Count > Frequency) {
            Frequency = Count;
            ScaleIndex = T;
          }
        }
        const auto &CommonScale = Terms[ScaleIndex].Coeff;
        Common.push_back(Ctx.mkConst(CommonScale));
        Quotients.clear();
        for (unsigned T : Indices) {
          if (Used[T])
            continue;
          const auto &Term = Terms[T];
          llvm::SmallVector<SymRef, 8> Remaining;
          size_t K = 0;
          for (SymRef Factor : Term.Factors)
            if (K != Key.size() && Factor.index() == Key[K])
              ++K;
            else
              Remaining.push_back(Factor);
          Quotients.push_back(Ctx.mkMul(Remaining));
          auto Difference = Term.Coeff - CommonScale;
          if (!Difference.isZero()) {
            Residuals.push_back({std::move(Difference), Term.Factors});
            ResidualExprs.push_back(Write(Residuals.back()));
          }
        }
        if (!ChargeOperands(SymOp::Add, Quotients, MaxTerms) ||
            !ChargeOperands(SymOp::Mul, Common, MaxEdges))
          break;
        Quotient = Ctx.mkAdd(Quotients);
      }
      auto Accept = [&] {
        SymRef Original = Ctx.mkAdd(Originals);
        SymRef Cofactor = Ctx.mkMul(Common);
        if (!ChargeOperands(SymOp::Mul, {Cofactor, Reduced}, MaxEdges) ||
            !ChargeOperands(SymOp::Mul, {Cofactor, Quotient}, MaxEdges))
          return false;
        SymRef Candidate = Ctx.mkMul(Cofactor, Reduced);
        llvm::SmallVector<SymRef, 4> Trial(ResidualExprs);
        Trial.push_back(Candidate);
        if (!ChargeOperands(SymOp::Add, Trial, MaxTerms) ||
            Score(Ctx.mkAdd(Trial)) >= Score(Original))
          return false;
        Next.push_back(Read(Candidate));
        Next.append(Residuals.begin(), Residuals.end());
        Distribution.push_back(Ctx.mkMul(Cofactor, Quotient));
        Distribution.append(ResidualExprs.begin(), ResidualExprs.end());
        for (unsigned T : Indices)
          Used[T] = true;
        Rep.NumAtoms = std::max(Rep.NumAtoms, Local.NumAtoms);
        return true;
      };
      if (Solve() && Accept())
        continue;
      if (!SameScale && !Budget.exhausted()) {
        // If the unit-coefficient identity does not shorten this group,
        // preserve the original weighted quotient's independent opportunity.
        Common.pop_back();
        Residuals.clear();
        ResidualExprs.clear();
        Quotient = WeightedQuotient;
        if (Solve())
          Accept();
      }
    }
    if (Next.empty()) {
      if (Omitted++ == 1 && !Rep.BudgetExhausted)
        continue;
      break;
    }
    Omitted = 1;
    for (unsigned T = 0; T != Terms.size(); ++T)
      if (!Used[T]) {
        Distribution.push_back(Write(Terms[T]));
        Next.push_back(std::move(Terms[T]));
      }
    // Check the complete distribution, including all untouched terms, before
    // committing any quotient substitutions. A single proof shares the
    // normalizer's exact-node memo across groups. The independently proved
    // quotients then compose under their unchanged cofactors.
    if (!ChargeOperands(SymOp::Add, Distribution, MaxTerms) ||
        !proveArithmeticIdentity(Ctx, Current, Ctx.mkAdd(Distribution), Budget,
                                 Storage,
                                 /*ExpandComplements=*/false))
      break;
    Terms = std::move(Next);
    llvm::SmallVector<SymRef, 16> Summands;
    for (const auto &Term : Terms)
      Summands.push_back(Write(Term));
    if (!ChargeOperands(SymOp::Add, Summands, MaxTerms))
      break;
    Current = Ctx.mkAdd(Summands);
    if (Score(Current) < Score(Best)) {
      Best = Current;
      Rep.Outcome = MBAOutcome::Rewritten;
      Rep.Evidence = MBAEvidence::Derivation;
    }
  }
  Rep.BudgetExhausted |= Budget.exhausted();
  return Best;
}

SymRef solveProductCofactors(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                             WorkBudget &Budget, SolveReport &Rep) {
  if (Ctx.op(E) == SymOp::Add &&
      Ctx.numOperands(E) <= limits::kProductCofactorMaxTerms) {
    std::array<std::pair<unsigned, unsigned>, limits::kProductCofactorMaxTerms>
        Counts{};
    size_t Count = 0;
    bool Flat = true;
    size_t Edges = 0;
    for (SymRef Term : Ctx.operands(E)) {
      unsigned Degree = 0;
      if (Ctx.op(Term) == SymOp::Mul) {
        const auto Factors = Ctx.operands(Term);
        Degree = Factors.size() - unsigned(Ctx.isConst(Factors.front()));
        if (Factors.size() > limits::kProductCofactorMaxEdges - Edges) {
          Flat = false;
          break;
        }
        Edges += Factors.size();
        Flat &= !llvm::any_of(Factors, [&](SymRef F) {
          return Ctx.op(F) == SymOp::Add || Ctx.op(F) == SymOp::Mul;
        });
      }
      if (Degree >= limits::kMinProductCofactorDegree) {
        size_t I = 0;
        while (I < Count && Counts[I].first != Degree)
          ++I;
        if (I == Count)
          Counts[Count++] = {Degree, 0};
        ++Counts[I].second;
      }
    }
    if (Flat && Edges <= limits::kProductCofactorMaxEdges && Count >= 2 &&
        llvm::all_of(
            llvm::ArrayRef(Counts).take_front(Count), [](const auto &Entry) {
              return Entry.second >= limits::kProductCofactorMinCompletionTerms;
            })) {
      const size_t Bytes =
          128 * Ctx.numOperands(E) + Edges * 4 * sizeof(SymRef);
      if (Bytes > Opts.MaxTableBytes ||
          !Budget.consume(Ctx.numOperands(E) + Edges)) {
        Rep.BudgetExhausted = true;
        return E;
      }
      std::map<unsigned, llvm::SmallVector<SymRef, 16>> Groups;
      for (SymRef Term : Ctx.operands(E)) {
        const unsigned Degree =
            Ctx.op(Term) == SymOp::Mul
                ? Ctx.numOperands(Term) -
                      unsigned(Ctx.isConst(Ctx.operand(Term, 0)))
                : 0;
        Groups[Degree >= limits::kMinProductCofactorDegree ? Degree : 0]
            .push_back(Term);
      }
      MBAOptions Inner = Opts;
      Inner.MaxTableBytes -= Bytes;
      llvm::SmallVector<SymRef, 8> Parts;
      for (const auto &[Degree, Terms] : Groups) {
        const SymRef Part = Ctx.mkAdd(Terms);
        if (Degree == 0 || Terms.size() < 2) {
          Parts.push_back(Part);
          continue;
        }
        SolveReport Local;
        Parts.push_back(
            solveProductCofactorsImpl(Ctx, Part, Inner, Budget, Local, true));
        Rep.BudgetExhausted |= Local.BudgetExhausted;
        Rep.NumAtoms = std::max(Rep.NumAtoms, Local.NumAtoms);
      }
      const SymRef Candidate = Ctx.mkAdd(Parts);
      if (readingScore(Ctx, Candidate) < readingScore(Ctx, E)) {
        Rep.Outcome = MBAOutcome::Rewritten;
        Rep.Evidence = MBAEvidence::Derivation;
        return Candidate;
      }
    }
  }
  return solveProductCofactorsImpl(Ctx, E, Opts, Budget, Rep, true);
}

SymRef solveStructuralFactors(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                              WorkBudget &Budget, SolveReport &Rep) {
  // This optional reading only inspects immediate sums and products. Purely
  // arithmetic factoring belongs to the polynomial normalizer. A small fixed
  // frontier also keeps unsuccessful scans independent of shared tail depth.
  constexpr size_t
      MaxTerms = ::neverd::symbolic::detail::limits::kStructuralFactorMaxTerms,
      MaxEdges = ::neverd::symbolic::detail::limits::kStructuralFactorMaxEdges;
  if (Ctx.op(E) != SymOp::Add || Ctx.numOperands(E) < 3 ||
      Ctx.numOperands(E) > MaxTerms || Budget.exhausted())
    return E;
  unsigned Products = 0;
  bool Bitwise = false;
  size_t Edges = 0;
  for (SymRef Term : Ctx.operands(E)) {
    if (Ctx.op(Term) != SymOp::Mul)
      continue;
    llvm::ArrayRef<SymRef> Factors = Ctx.operands(Term);
    if (Factors.size() > MaxEdges - Edges)
      return E;
    Edges += Factors.size();
    Products += Factors.size() - unsigned(Ctx.isConst(Factors.front())) > 1;
    for (SymRef Factor : Factors)
      Bitwise |= isBitwise(Ctx, Factor);
  }
  if (Products < 2 || !Bitwise)
    return E;

  SymRef Best = E;
  size_t Storage = Opts.MaxTableBytes;
  const size_t WordBytes =
      (size_t(Ctx.width(E)) / 64 + (Ctx.width(E) % 64 != 0)) * sizeof(uint64_t);
  const size_t SlotBytes = 128 + 3 * WordBytes;
  auto Charge = [&](size_t Work, size_t Slots) {
    if (Slots > Storage / SlotBytes || !Budget.consume(Work)) {
      Rep.BudgetExhausted = true;
      return false;
    }
    Storage -= Slots * SlotBytes;
    return true;
  };
  auto ChargeOperands = [&](SymOp Op, llvm::ArrayRef<SymRef> Operands,
                            size_t Limit) {
    size_t Arity = 0;
    for (SymRef R : Operands) {
      const size_t N = Ctx.op(R) == Op ? Ctx.numOperands(R) : 1;
      if (N > Limit - Arity) {
        Rep.BudgetExhausted = true;
        return false;
      }
      Arity += N;
    }
    return Charge(Arity, Arity);
  };
  // Counts, the original term snapshot, and scratch operand buffers are all
  // charged before allocation. Each scan below is bounded by this frontier.
  const size_t Count = Ctx.numOperands(E);
  if (!Charge(Count + Edges, Count + Edges))
    return Best;
  llvm::SmallVector<SymRef, 16> Terms(Ctx.operands(E).begin(),
                                      Ctx.operands(E).end());
  std::map<uint32_t, FactorUses> Uses;
  for (SymRef Term : Terms) {
    if (Ctx.isConst(Term))
      continue;
    if (Ctx.op(Term) != SymOp::Mul) {
      ++Uses[Term.index()].Count;
      continue;
    }
    llvm::ArrayRef<SymRef> Factors = Ctx.operands(Term);
    const unsigned BitwiseFactors =
        std::count_if(Factors.begin(), Factors.end(),
                      [&](SymRef F) { return isBitwise(Ctx, F); });
    SymRef Previous;
    // Canonical products sort nonconstant factors. A repeated factor counts
    // once per term, while removing one copy can leave another in the quotient.
    for (SymRef Factor : Factors) {
      if (Ctx.isConst(Factor) || Factor == Previous)
        continue;
      Previous = Factor;
      auto &Use = Uses[Factor.index()];
      ++Use.Count;
      Use.BitwiseQuotient |= BitwiseFactors > unsigned(isBitwise(Ctx, Factor));
    }
  }
  llvm::SmallVector<uint32_t, 2> Choices;
  for (const auto &[Id, Use] : Uses) {
    if (Use.Count < 2 ||
        (!Use.BitwiseQuotient && Ctx.op(SymRef(Id)) != SymOp::Not))
      continue;
    auto Position =
        std::find_if(Choices.begin(), Choices.end(),
                     [&](uint32_t I) { return Use.Count > Uses.at(I).Count; });
    Choices.insert(Position, Id);
    if (Choices.size() > 2)
      Choices.pop_back();
  }

  for (uint32_t Id : Choices) {
    if (!Charge(Count + Edges, Count + Edges + 4))
      break;
    SymRef Factor(Id);
    llvm::SmallVector<SymRef, 16> Common, Intersection;
    bool First = true;
    for (SymRef Term : Terms) {
      if (Term == Factor) {
        Common.assign(1, Factor);
        break;
      }
      if (Ctx.op(Term) != SymOp::Mul)
        continue;
      auto Factors = Ctx.operands(Term);
      if (!llvm::is_contained(Factors, Factor))
        continue;
      if (Ctx.isConst(Factors.front()))
        Factors = Factors.drop_front();
      if (!Charge(Factors.size() + Common.size(), Factors.size()))
        return Best;
      if (First) {
        Common.assign(Factors.begin(), Factors.end());
        First = false;
      } else {
        Intersection.clear();
        std::set_intersection(Common.begin(), Common.end(), Factors.begin(),
                              Factors.end(), std::back_inserter(Intersection));
        Common.swap(Intersection);
      }
    }
    if (Budget.exhausted() || Common.empty())
      break;
    llvm::SmallVector<SymRef, 16> Quotients, Rest;
    for (SymRef Term : Terms) {
      if (Term == Factor) {
        Quotients.push_back(Ctx.mkOne(Ctx.width(E)));
        continue;
      }
      if (Ctx.op(Term) != SymOp::Mul) {
        Rest.push_back(Term);
        continue;
      }
      llvm::ArrayRef<SymRef> Factors = Ctx.operands(Term);
      auto Found = std::find(Factors.begin(), Factors.end(), Factor);
      if (Found == Factors.end()) {
        Rest.push_back(Term);
        continue;
      }
      llvm::SmallVector<SymRef, 8> Remaining;
      if (Ctx.isConst(Factors.front())) {
        Remaining.push_back(Factors.front());
        Factors = Factors.drop_front();
      }
      std::set_difference(Factors.begin(), Factors.end(), Common.begin(),
                          Common.end(), std::back_inserter(Remaining));
      Quotients.push_back(Remaining.empty() ? Ctx.mkOne(Ctx.width(E))
                                            : Ctx.mkMul(Remaining));
    }
    Factor = Ctx.mkMul(Common);
    // Every original term is placed once, and exactly one identical factor
    // is removed from each selected term. Distributivity in the original word
    // ring therefore proves E = Factor * sum(Quotients) + sum(Rest), without
    // treating related factors as independent or inferring anything from
    // samples.
    // Removing a factor can expose an arbitrarily wide sum. Account for the
    // actual canonical flattening before any builder copies its operands.
    if (!ChargeOperands(SymOp::Add, Quotients, MaxTerms))
      continue;
    // These quotients were created after the deep walk's original frontier.
    // Apply its exact parity projection before searching an unobservable
    // product. Keep the higher-product recovery's original factor basis.
    for (SymRef &Part : Quotients)
      Part = foldProductParity(Ctx, Part, Budget, Storage);
    SymRef Quotient = Ctx.mkAdd(Quotients);
    auto Consider = [&](SymRef Value, unsigned NumAtoms) {
      if (!ChargeOperands(SymOp::Mul, {Factor, Value}, MaxEdges))
        return;
      // Multiplication by one can expose the complete factor as an Add.
      // Check that degeneracy before constructing either candidate node. Rest
      // comes from the canonical original Add and has no direct Add child.
      const size_t Added = Ctx.isConst(Value) &&
                                   Ctx.constValue(Value).isOne() &&
                                   Ctx.op(Factor) == SymOp::Add
                               ? Ctx.numOperands(Factor)
                               : 1;
      if (Added > MaxTerms || Rest.size() > MaxTerms - Added) {
        Rep.BudgetExhausted = true;
        return;
      }
      bool Fits = Charge(Rest.size() + Added, Rest.size() + Added);
      if (!Fits)
        return;
      Rest.push_back(Ctx.mkMul(Factor, Value));
      SymRef Candidate = Ctx.mkAdd(Rest);
      Rest.pop_back();
      if (readingScore(Ctx, Candidate) < readingScore(Ctx, Best)) {
        Best = Candidate;
        Rep.NumAtoms = NumAtoms;
        Rep.Outcome = MBAOutcome::Rewritten;
        Rep.Evidence = MBAEvidence::Derivation;
      }
    };
    // Distributivity already proves this candidate. A failed optional quotient
    // search must not discard a strictly better completed factorization.
    Consider(Quotient, 0);
    if (hasIndependentTerms(Ctx, Quotient))
      continue;
    MBAOptions Inner = Opts;
    Inner.MaxTableBytes = Storage;
    SolveReport Local;
    SymRef Reduced = solveOneRegion(Ctx, Quotient, Inner, Budget, Local);
    Rep.BudgetExhausted |= Local.BudgetExhausted;
    if (Reduced == Quotient || Local.Evidence != MBAEvidence::Derivation)
      continue;
    // solveOneRegion independently proved the quotient replacement. Reusing
    // it under this unchanged factor instantiates that exact identity.
    Consider(Reduced, Local.NumAtoms);
    if (Budget.exhausted())
      break;
  }
  return Best;
}

} // namespace neverd::symbolic::detail
