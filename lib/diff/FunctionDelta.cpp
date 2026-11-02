//===- FunctionDelta.cpp - Blocks and instructions of a matched pair ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pairing the basic blocks of two corresponding functions, aligning the
/// instructions of paired blocks, and classifying what changed.
///
//===----------------------------------------------------------------------===//

#include "neverd/diff/FunctionDelta.h"

#include "Features.h"
#include "Matching.h"

#include "neverd/diff/Limits.h"
#include "neverd/diff/StableHash.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <tuple>
#include <unordered_map>

namespace neverd::diff {

namespace {

struct DeltaInterrupted {
  ComparisonState State;
};

/// What identifies one block among its function's blocks.
struct BlockKeys {
  uint64_t Bytes = 0;
  uint64_t Body = 0;
  uint64_t Prime = 0;
  uint64_t Calls = 0;
  uint64_t References = 0;
  uint64_t TopDown = 0;
  uint64_t BottomUp = 0;
  uint32_t Count = 0;
  bool Exit = false;
};

/// One function as the delta reads it.
struct Side {
  const Program &P;
  const Function &F;
  llvm::ArrayRef<uint64_t> MnemonicKeys;
  const ComparisonControl &Control;
  GraphShape Graph;
  std::vector<BlockKeys> Keys;
  /// Only instructions with address-taking operands need stored signatures.
  /// An empty signature retains an unresolved target, rather than inventing
  /// equality from a generic operand token or a reused address.
  llvm::DenseMap<const Instruction *, std::optional<uint64_t>> AddressKeys;
  /// Outgoing and incoming edges, ordered by the opposite block's index.
  Adjacency Out;
  Adjacency In;

  Side(const Program &P, const Function &F,
       llvm::ArrayRef<uint64_t> MnemonicKeys, const ComparisonControl &Control)
      : P(P), F(F), MnemonicKeys(MnemonicKeys), Control(Control),
        Graph(graphShape(P, F, Control)) {}
  void poll() const {
    if (const auto State = Control.stop())
      throw DeltaInterrupted{*State};
  }

  const Edge &edge(uint32_t Index) const { return P.edges(F)[Index]; }
  llvm::ArrayRef<Instruction> instructions(uint32_t Block) const {
    return P.instructions(P.blocks(F)[Block]);
  }
  uint32_t firstInstruction(uint32_t Block) const {
    return P.blocks(F)[Block].FirstInstruction;
  }
  uint64_t mnemonic(const Instruction &I) const {
    return MnemonicKeys[I.Mnemonic];
  }
  std::optional<uint64_t> addressKey(const Instruction &I) const {
    if (I.UnresolvedCallee)
      return {};
    const auto It = AddressKeys.find(&I);
    return It == AddressKeys.end() ? std::optional<uint64_t>(0) : It->second;
  }
  std::optional<uint64_t> shape(const Instruction &I) const {
    const auto Addresses = addressKey(I);
    if (!Addresses)
      return {};
    return StableHash().add(I.Shape).add(*Addresses).value();
  }
};

Adjacency edgeAdjacency(const Program &P, const Function &F, bool Incoming) {
  std::vector<std::pair<uint32_t, uint32_t>> Pairs;
  const auto Edges = P.edges(F);
  for (uint32_t I = 0; I < Edges.size(); ++I)
    Pairs.emplace_back(Incoming ? Edges[I].To : Edges[I].From, I);
  Adjacency A;
  A.Start.assign(F.BlockCount + 1, 0);
  for (const auto &Pair : Pairs)
    ++A.Start[Pair.first + 1];
  for (uint32_t I = 0; I < F.BlockCount; ++I)
    A.Start[I + 1] += A.Start[I];
  A.Targets.resize(Pairs.size());
  std::vector<uint32_t> Fill(A.Start.begin(), A.Start.end() - 1);
  for (const auto &[Block, Edge] : Pairs)
    A.Targets[Fill[Block]++] = Edge;
  for (uint32_t I = 0; I < F.BlockCount; ++I)
    llvm::sort(llvm::MutableArrayRef(A.Targets).slice(
                   A.Start[I], A.Start[I + 1] - A.Start[I]),
               [&](uint32_t X, uint32_t Y) {
                 return std::pair{Incoming ? Edges[X].From : Edges[X].To, X} <
                        std::pair{Incoming ? Edges[Y].From : Edges[Y].To, Y};
               });
  return A;
}

/// The id a call reaches, the same on both sides when the callees
/// correspond: the secondary function's index.  NoIndex when the primary
/// callee has no counterpart.
uint32_t callId(const Instruction &I, bool Primary, const Correspondence &Map) {
  if (I.Callee == NoIndex)
    return NoIndex;
  if (!Primary)
    return I.Callee;
  return I.Callee < Map.PrimaryToSecondary.size()
             ? Map.PrimaryToSecondary[I.Callee]
             : NoIndex;
}

std::optional<uint64_t> referenceId(const Reference &R, const Program &P,
                                    bool Primary, const Correspondence &Map) {
  if (R.Kind != ReferenceKind::Function)
    return R.Key;
  const auto Target = P.functionAt(R.Key);
  if (!Target)
    return {};
  if (!Primary)
    return *Target;
  if (*Target >= Map.PrimaryToSecondary.size() ||
      Map.PrimaryToSecondary[*Target] == NoIndex)
    return {};
  return Map.PrimaryToSecondary[*Target];
}

/// Unknown operands must not conceal an independently known retargeting.
/// This linear walk is used for aligned rows, not the alignment's DP cells.
bool knownAddressDifference(const Program &P, const Instruction &X,
                            const Program &S, const Instruction &Y,
                            const Correspondence &Map) {
  const auto A = P.addressReferences(X), B = S.addressReferences(Y);
  size_t I = 0, J = 0;
  while (I < A.size() || J < B.size()) {
    if (J == B.size() || (I < A.size() && A[I].Operand < B[J].Operand)) {
      if (!Y.HasUnresolvedData)
        return true;
      ++I;
    } else if (I == A.size() || B[J].Operand < A[I].Operand) {
      if (!X.HasUnresolvedData)
        return true;
      ++J;
    } else {
      if (A[I].Kind != ReferenceKind::UnresolvedData &&
          B[J].Kind != ReferenceKind::UnresolvedData) {
        const auto Left = referenceId({A[I].Kind, A[I].Key, 0}, P, true, Map);
        const auto Right = referenceId({B[J].Kind, B[J].Key, 0}, S, false, Map);
        if (Left && Right && (A[I].Kind != B[J].Kind || Left != Right))
          return true;
      }
      ++I;
      ++J;
    }
  }
  return false;
}

void computeKeys(Side &S, bool Primary, const Correspondence &Map) {
  S.poll();
  S.Out = edgeAdjacency(S.P, S.F, false);
  S.In = edgeAdjacency(S.P, S.F, true);
  S.Keys.resize(S.F.BlockCount);
  // Bucket references once. Scanning every function reference for every block
  // makes large switch/flattened functions quadratic.
  std::vector<StableHash> ReferenceKeys(S.F.BlockCount);
  std::vector<unsigned> ReferenceCounts(S.F.BlockCount, 0);
  std::vector<bool> ReferencesKnown(S.F.BlockCount, true);
  std::vector<std::tuple<uint32_t, ReferenceKind, uint64_t>> References;
  References.reserve(S.F.ReferenceCount);
  for (const Reference &R : S.P.references(S.F)) {
    S.poll();
    if (R.Block < S.F.BlockCount && R.Kind != ReferenceKind::Constant) {
      const auto Key = referenceId(R, S.P, Primary, Map);
      ReferencesKnown[R.Block] = ReferencesKnown[R.Block] && Key.has_value();
      if (Key)
        References.emplace_back(R.Block, R.Kind, *Key);
      ++ReferenceCounts[R.Block];
    }
  }
  llvm::sort(References);
  for (const auto &[Block, Kind, Key] : References)
    ReferenceKeys[Block].add(uint64_t(Kind)).add(Key);
  for (uint32_t B = 0; B < S.F.BlockCount; ++B) {
    S.poll();
    BlockKeys &K = S.Keys[B];
    StableHash Bytes, Body, Calls;
    uint64_t Prime = 1;
    bool CallsKnown = true, BodyKnown = true;
    unsigned CallCount = 0;
    for (const Instruction &I : S.instructions(B)) {
      S.poll();
      Bytes.add(S.P.bytes(I));
      Body.add(I.Shape);
      if (I.AddressReferenceCount || I.Callee != NoIndex) {
        StableHash Addresses;
        bool Known = true;
        for (const auto &Address : S.P.addressReferences(I)) {
          if (Address.Kind != ReferenceKind::Function)
            continue;
          const auto Target = referenceId(
              {ReferenceKind::Function, Address.Key, B}, S.P, Primary, Map);
          Known &= Target.has_value();
          if (Target)
            Addresses.add(Address.Operand).add(*Target);
        }
        if (I.Callee != NoIndex) {
          const uint32_t Target = callId(I, Primary, Map);
          Known &= Target != NoIndex;
          Addresses.add(NoIndex).add(Target);
        }
        S.AddressKeys[&I] =
            Known ? std::optional<uint64_t>(Addresses.value()) : std::nullopt;
      }
      const auto Addresses = S.addressKey(I);
      BodyKnown &= Addresses.has_value();
      if (Addresses)
        Body.add(*Addresses);
      Prime *= oddFactor(S.mnemonic(I));
      if (I.Callee != NoIndex) {
        const uint32_t Id = callId(I, Primary, Map);
        CallsKnown &= Id != NoIndex;
        Calls.add(Id);
        ++CallCount;
      }
      ++K.Count;
    }
    K.Bytes = BodyKnown ? Bytes.value() : 0;
    K.Body = BodyKnown ? Body.value() : 0;
    K.Prime = Prime;
    if (CallCount && CallsKnown)
      K.Calls = Calls.value();
    if (ReferenceCounts[B] && ReferencesKnown[B])
      K.References = ReferenceKeys[B].value();
    // A block's place: its level and degrees, and those of where it leads.
    const auto Place = [&](const std::vector<uint32_t> &Level) {
      std::vector<uint64_t> Next;
      for (uint32_t E : S.Out.of(B)) {
        const uint32_t To = S.edge(E).To;
        Next.push_back(edgeIndex(Level[B], S.Graph.In[B], S.Graph.Out[B],
                                 S.Graph.In[To], S.Graph.Out[To]));
      }
      llvm::sort(Next);
      StableHash H(0xcbbb9d5dc1059ed8ULL);
      H.add(Level[B]).add(S.Graph.In[B]).add(S.Graph.Out[B]);
      for (uint64_t N : Next)
        H.add(N);
      return H.value();
    };
    K.TopDown = Place(S.Graph.TopLevel);
    K.BottomUp = Place(S.Graph.BottomLevel);
    K.Exit = S.Graph.Out[B] == 0;
  }
}

/// Pairs the blocks of two functions.
class BlockMatcher {
public:
  BlockMatcher(const Side &A, const Side &B)
      : A(A), B(B), AMatch(A.F.BlockCount, NoIndex),
        BMatch(B.F.BlockCount, NoIndex), AStep(A.F.BlockCount) {}

  void run();

  const Side &A;
  const Side &B;
  std::vector<uint32_t> AMatch;
  std::vector<uint32_t> BMatch;
  std::vector<BlockStep> AStep;

private:
  bool CheckNeighbors = false;
  bool pair(uint32_t X, uint32_t Y, BlockStep Step) {
    if (AMatch[X] != NoIndex || BMatch[Y] != NoIndex)
      return false;
    AMatch[X] = Y;
    BMatch[Y] = X;
    AStep[X] = Step;
    return true;
  }
  /// Pairs the blocks of \p Xs and \p Ys whose key no other unpaired block
  /// of either list shares.  Returns how many pairs formed.
  template <typename KeyFn>
  unsigned unique(llvm::ArrayRef<uint32_t> Xs, llvm::ArrayRef<uint32_t> Ys,
                  KeyFn Key, BlockStep Step);
  unsigned uniqueAll(BlockStep Step);
  void runPass(llvm::ArrayRef<std::pair<uint32_t, uint32_t>> CallExits);
  unsigned uniqueAmong(llvm::ArrayRef<uint32_t> Xs,
                       llvm::ArrayRef<uint32_t> Ys);
  unsigned propagate();
  unsigned byOrder();
  double closeness(uint32_t X, uint32_t Y) const;
  static bool callOnly(const Side &S, uint32_t Block) {
    const auto &K = S.Keys[Block];
    if (!K.Calls || K.Count != 1)
      return false;
    const auto Flow = S.instructions(Block).front().Flow;
    return Flow == InstructionFlow::Call ||
           Flow == InstructionFlow::IndirectCall;
  }
};

template <typename KeyFn>
unsigned BlockMatcher::unique(llvm::ArrayRef<uint32_t> Xs,
                              llvm::ArrayRef<uint32_t> Ys, KeyFn Key,
                              BlockStep Step) {
  struct Entry {
    uint64_t Key;
    uint8_t Side;
    uint32_t Block;
  };
  std::vector<Entry> Entries;
  for (uint32_t X : Xs)
    if (AMatch[X] == NoIndex)
      if (const uint64_t K = Key(A, X))
        Entries.push_back({K, 0, X});
  for (uint32_t Y : Ys)
    if (BMatch[Y] == NoIndex)
      if (const uint64_t K = Key(B, Y))
        Entries.push_back({K, 1, Y});
  llvm::sort(Entries, [](const Entry &L, const Entry &R) {
    return std::tie(L.Key, L.Side, L.Block) < std::tie(R.Key, R.Side, R.Block);
  });
  unsigned Paired = 0;
  for (size_t I = 0; I < Entries.size();) {
    A.poll();
    size_t J = I;
    while (J < Entries.size() && Entries[J].Key == Entries[I].Key)
      ++J;
    if (J - I == 2 && Entries[I].Side == 0 && Entries[I + 1].Side == 1)
      Paired += pair(Entries[I].Block, Entries[I + 1].Block, Step);
    I = J;
  }
  return Paired;
}

unsigned BlockMatcher::uniqueAll(BlockStep Step) {
  std::vector<uint32_t> Xs(A.F.BlockCount), Ys(B.F.BlockCount);
  for (uint32_t I = 0; I < Xs.size(); ++I)
    Xs[I] = I;
  for (uint32_t I = 0; I < Ys.size(); ++I)
    Ys[I] = I;
  const uint32_t Min = limits::diff::kMinBlockHashInstructions;
  switch (Step) {
  case BlockStep::BlockBytes:
    return unique(
        Xs, Ys,
        [&](const Side &S, uint32_t X) {
          return S.Keys[X].Count >= Min ? S.Keys[X].Bytes : 0;
        },
        Step);
  case BlockStep::BlockBody:
    return unique(
        Xs, Ys,
        [&](const Side &S, uint32_t X) {
          return S.Keys[X].Count >= Min ? S.Keys[X].Body : 0;
        },
        Step);
  case BlockStep::BlockPrime:
    return unique(
        Xs, Ys,
        [&](const Side &S, uint32_t X) {
          return S.Keys[X].Count >= Min
                     ? StableHash(S.Keys[X].Prime).add(S.Keys[X].Count).value()
                     : 0;
        },
        Step);
  case BlockStep::BlockCalls:
    return unique(
        Xs, Ys, [&](const Side &S, uint32_t X) { return S.Keys[X].Calls; },
        Step);
  case BlockStep::BlockReferences:
    return unique(
        Xs, Ys, [&](const Side &S, uint32_t X) { return S.Keys[X].References; },
        Step);
  case BlockStep::BlockTopDown:
    return unique(
        Xs, Ys, [&](const Side &S, uint32_t X) { return S.Keys[X].TopDown; },
        Step);
  case BlockStep::BlockBottomUp:
    return unique(
        Xs, Ys, [&](const Side &S, uint32_t X) { return S.Keys[X].BottomUp; },
        Step);
  default:
    return 0;
  }
}

unsigned BlockMatcher::uniqueAmong(llvm::ArrayRef<uint32_t> Xs,
                                   llvm::ArrayRef<uint32_t> Ys) {
  // Among a matched pair's neighbors even a short block's body is telling.
  unsigned Paired = unique(
      Xs, Ys, [](const Side &S, uint32_t X) { return S.Keys[X].Bytes; },
      BlockStep::BlockBytes);
  Paired += unique(
      Xs, Ys, [](const Side &S, uint32_t X) { return S.Keys[X].Body; },
      BlockStep::BlockBody);
  Paired += unique(
      Xs, Ys,
      [](const Side &S, uint32_t X) {
        return StableHash(S.Keys[X].Prime).add(S.Keys[X].Count).value();
      },
      BlockStep::BlockPrime);
  return Paired;
}

/// How alike two blocks' instructions are: mostly the normalized
/// instructions they share, partly the mnemonics.
double BlockMatcher::closeness(uint32_t X, uint32_t Y) const {
  const auto Dice = [](std::vector<uint64_t> L, std::vector<uint64_t> R) {
    if (L.empty() && R.empty())
      return 1.0;
    llvm::sort(L);
    llvm::sort(R);
    size_t Common = 0;
    for (size_t I = 0, J = 0; I < L.size() && J < R.size();) {
      if (L[I] == R[J]) {
        ++Common;
        ++I;
        ++J;
      } else if (L[I] < R[J]) {
        ++I;
      } else {
        ++J;
      }
    }
    return 2.0 * Common / (L.size() + R.size());
  };
  std::vector<uint64_t> LS, RS, LM, RM;
  for (const Instruction &I : A.instructions(X)) {
    A.poll();
    LS.push_back(I.Shape);
    LM.push_back(A.mnemonic(I));
  }
  for (const Instruction &I : B.instructions(Y)) {
    B.poll();
    RS.push_back(I.Shape);
    RM.push_back(B.mnemonic(I));
  }
  return 0.7 * Dice(std::move(LS), std::move(RS)) +
         0.3 * Dice(std::move(LM), std::move(RM));
}

unsigned BlockMatcher::propagate() {
  unsigned Total = 0;
  for (bool Changed = true; Changed;) {
    A.poll();
    Changed = false;
    for (uint32_t X = 0; X < A.F.BlockCount; ++X) {
      A.poll();
      const uint32_t Y = AMatch[X];
      if (Y == NoIndex)
        continue;
      for (bool Forward : {true, false}) {
        const auto XEdges = Forward ? A.Out.of(X) : A.In.of(X);
        const auto YEdges = Forward ? B.Out.of(Y) : B.In.of(Y);
        const auto Other = [&](const Side &S, uint32_t E) {
          return Forward ? S.edge(E).To : S.edge(E).From;
        };
        std::vector<uint32_t> Xs, Ys;
        for (uint32_t E : XEdges)
          if (AMatch[Other(A, E)] == NoIndex)
            Xs.push_back(Other(A, E));
        for (uint32_t E : YEdges)
          if (BMatch[Other(B, E)] == NoIndex)
            Ys.push_back(Other(B, E));
        if (Xs.empty() || Ys.empty())
          continue;
        llvm::sort(Xs);
        Xs.erase(std::unique(Xs.begin(), Xs.end()), Xs.end());
        llvm::sort(Ys);
        Ys.erase(std::unique(Ys.begin(), Ys.end()), Ys.end());
        if (const unsigned N = uniqueAmong(Xs, Ys)) {
          Total += N;
          Changed = true;
          continue;
        }
        // A removed guard can leave the entry blocks corresponding while
        // their successors no longer do. Known neighbors must agree before
        // the remaining edges can supply weaker positional evidence.
        const auto Agrees = [&](const Side &L, llvm::ArrayRef<uint32_t> LEdges,
                                const std::vector<uint32_t> &Match,
                                const Side &R,
                                llvm::ArrayRef<uint32_t> REdges) {
          for (uint32_t E : LEdges) {
            const uint32_t M = Match[Other(L, E)];
            if (M == NoIndex)
              continue;
            const auto It = llvm::lower_bound(
                REdges, M, [&](uint32_t RIndex, uint32_t Block) {
                  return Other(R, RIndex) < Block;
                });
            if (It == REdges.end() || Other(R, *It) != M)
              return false;
          }
          return true;
        };
        if (CheckNeighbors && (!Agrees(A, XEdges, AMatch, B, YEdges) ||
                               !Agrees(B, YEdges, BMatch, A, XEdges)))
          continue;
        // One edge of each kind left on both sides: they correspond, or,
        // for the two arms of a conditional branch, cross over.
        const auto Single = [&](const Side &S, llvm::ArrayRef<uint32_t> Edges,
                                const std::vector<uint32_t> &Match,
                                EdgeKind Kind) {
          uint32_t Found = NoIndex;
          for (uint32_t E : Edges) {
            const uint32_t To = Other(S, E);
            if (S.edge(E).Kind != Kind || Match[To] != NoIndex)
              continue;
            if (Found != NoIndex && Found != To)
              return NoIndex;
            Found = To;
          }
          return Found;
        };
        const uint32_t XT = Single(A, XEdges, AMatch, EdgeKind::Taken);
        const uint32_t XN = Single(A, XEdges, AMatch, EdgeKind::NotTaken);
        const uint32_t YT = Single(B, YEdges, BMatch, EdgeKind::Taken);
        const uint32_t YN = Single(B, YEdges, BMatch, EdgeKind::NotTaken);
        if (Forward && XT != NoIndex && XN != NoIndex && YT != NoIndex &&
            YN != NoIndex && XT != XN && YT != YN) {
          const double Straight = closeness(XT, YT) + closeness(XN, YN);
          const double Crossed = closeness(XT, YN) + closeness(XN, YT);
          if (Crossed > Straight) {
            Total += pair(XT, YN, BlockStep::BlockEdge);
            Total += pair(XN, YT, BlockStep::BlockEdge);
            Changed = true;
            continue;
          }
        }
        for (EdgeKind Kind :
             {EdgeKind::Fallthrough, EdgeKind::Taken, EdgeKind::NotTaken,
              EdgeKind::Jump, EdgeKind::Switch, EdgeKind::Exception}) {
          const uint32_t XK = Single(A, XEdges, AMatch, Kind);
          const uint32_t YK = Single(B, YEdges, BMatch, Kind);
          if (XK != NoIndex && YK != NoIndex &&
              pair(XK, YK, BlockStep::BlockEdge)) {
            ++Total;
            Changed = true;
          }
        }
        // Unconditional flow may be laid out as a fall-through or a jump.
        if (Xs.size() == 1 && Ys.size() == 1 &&
            pair(Xs[0], Ys[0], BlockStep::BlockEdge)) {
          ++Total;
          Changed = true;
        }
      }
    }
  }
  return Total;
}

unsigned BlockMatcher::byOrder() {
  // Between consecutive matched blocks in address order, equally many
  // unmatched blocks on both sides pair in order when they look alike.
  unsigned Total = 0;
  std::vector<uint32_t> XOrder(A.F.BlockCount), YOrder(B.F.BlockCount);
  for (uint32_t I = 0; I < XOrder.size(); ++I)
    XOrder[I] = I;
  for (uint32_t I = 0; I < YOrder.size(); ++I)
    YOrder[I] = I;
  const auto ByAddress = [](const Side &S) {
    return [&S](uint32_t L, uint32_t R) {
      return S.P.blocks(S.F)[L].Address < S.P.blocks(S.F)[R].Address;
    };
  };
  llvm::sort(XOrder, ByAddress(A));
  llvm::sort(YOrder, ByAddress(B));
  std::vector<uint32_t> YPosition(B.F.BlockCount);
  for (uint32_t I = 0; I < YOrder.size(); ++I)
    YPosition[YOrder[I]] = I;
  size_t XStart = 0;
  size_t YStart = 0;
  const auto Flush = [&](size_t XEnd, size_t YEnd) {
    if (XEnd > XStart && XEnd - XStart == YEnd - YStart)
      for (size_t I = 0; I < XEnd - XStart; ++I) {
        const uint32_t X = XOrder[XStart + I], Y = YOrder[YStart + I];
        if (AMatch[X] == NoIndex && BMatch[Y] == NoIndex &&
            closeness(X, Y) >= 0.5)
          Total += pair(X, Y, BlockStep::BlockOrder);
      }
  };
  for (size_t I = 0; I < XOrder.size(); ++I) {
    A.poll();
    const uint32_t Y = AMatch[XOrder[I]];
    if (Y == NoIndex)
      continue;
    const size_t YAt = YPosition[Y];
    if (YAt >= YStart) {
      Flush(I, YAt);
      YStart = YAt + 1;
    }
    XStart = I + 1;
  }
  Flush(XOrder.size(), YOrder.size());
  return Total;
}

void BlockMatcher::run() {
  runPass({});
  // Refine only when the ordinary alignment displaced a known call-only
  // block at an exit on at least one side. Its other occurrence may have a
  // modeled fallthrough after layout changes; preserve that edge difference.
  // Larger blocks can mix moved statements with calls; their call sequence
  // alone must not change the precedence of body evidence.
  const auto Displaced = [](const Side &L, const Side &R,
                            const std::vector<uint32_t> &Match) {
    for (uint32_t X = 0; X < L.F.BlockCount; ++X) {
      L.poll();
      if (!callOnly(L, X) || !L.Keys[X].Exit)
        continue;
      const uint32_t Y = Match[X];
      if (Y == NoIndex || !callOnly(R, Y) || R.Keys[Y].Calls != L.Keys[X].Calls)
        return true;
    }
    return false;
  };
  if (!Displaced(A, B, AMatch) && !Displaced(B, A, BMatch))
    return;
  struct Entry {
    uint64_t Key;
    bool Secondary;
    uint32_t Block;
  };
  std::vector<Entry> Entries;
  for (bool Secondary : {false, true}) {
    const Side &S = Secondary ? B : A;
    for (uint32_t I = 0; I < S.F.BlockCount; ++I) {
      S.poll();
      if (S.Keys[I].Calls)
        Entries.push_back({S.Keys[I].Calls, Secondary, I});
    }
  }
  llvm::sort(Entries, [](const Entry &L, const Entry &R) {
    return std::tie(L.Key, L.Secondary, L.Block) <
           std::tie(R.Key, R.Secondary, R.Block);
  });
  std::vector<std::pair<uint32_t, uint32_t>> CallExits;
  bool Refine = false;
  for (size_t I = 0; I < Entries.size();) {
    A.poll();
    size_t J = I + 1;
    while (J < Entries.size() && Entries[J].Key == Entries[I].Key)
      ++J;
    if (J - I == 2 && !Entries[I].Secondary && Entries[I + 1].Secondary) {
      const uint32_t X = Entries[I].Block, Y = Entries[I + 1].Block;
      if (callOnly(A, X) && callOnly(B, Y) &&
          (A.Keys[X].Exit || B.Keys[Y].Exit)) {
        CallExits.emplace_back(X, Y);
        Refine |= AMatch[X] != Y;
      }
    }
    I = J;
  }
  if (!Refine)
    return;
  std::fill(AMatch.begin(), AMatch.end(), NoIndex);
  std::fill(BMatch.begin(), BMatch.end(), NoIndex);
  CheckNeighbors = true;
  runPass(CallExits);
}

void BlockMatcher::runPass(
    llvm::ArrayRef<std::pair<uint32_t, uint32_t>> CallExits) {
  A.poll();
  if (!A.F.BlockCount || !B.F.BlockCount)
    return;
  uniqueAll(BlockStep::BlockBytes);
  uniqueAll(BlockStep::BlockBody);
  for (const auto &[X, Y] : CallExits)
    pair(X, Y, BlockStep::BlockCalls);
  if (A.F.EntryBlock != NoIndex && B.F.EntryBlock != NoIndex)
    pair(A.F.EntryBlock, B.F.EntryBlock, BlockStep::BlockEntry);
  for (BlockStep Step : {BlockStep::BlockPrime, BlockStep::BlockCalls,
                         BlockStep::BlockReferences, BlockStep::BlockTopDown,
                         BlockStep::BlockBottomUp}) {
    uniqueAll(Step);
    propagate();
  }
  // The only exits left on both sides.
  uint32_t XExit = NoIndex, YExit = NoIndex;
  unsigned XExits = 0, YExits = 0;
  for (uint32_t X = 0; X < A.F.BlockCount; ++X)
    if (AMatch[X] == NoIndex && A.Keys[X].Exit) {
      XExit = X;
      ++XExits;
    }
  for (uint32_t Y = 0; Y < B.F.BlockCount; ++Y)
    if (BMatch[Y] == NoIndex && B.Keys[Y].Exit) {
      YExit = Y;
      ++YExits;
    }
  if (XExits == 1 && YExits == 1 && closeness(XExit, YExit) >= 0.5)
    pair(XExit, YExit, BlockStep::BlockExit);
  propagate();
  if (byOrder())
    propagate();
}

/// Aligns the instructions of two paired blocks.
class Aligner {
public:
  Aligner(const Side &A, const Side &B, const Correspondence &Map)
      : A(A), B(B), Map(Map) {}

  void align(uint32_t XBlock, uint32_t YBlock, std::vector<DeltaRow> &Rows);
  bool refineOrderedSpans(std::vector<DeltaRow> &Rows, bool SingleBlockOnly);

private:
  const Side &A;
  const Side &B;
  const Correspondence &Map;
  std::vector<uint32_t> Score;
  using InstructionPairs = std::vector<std::pair<uint32_t, uint32_t>>;

  struct Locations {
    uint32_t Base;
    uint32_t Block;
    llvm::ArrayRef<uint32_t> Blocks;
    uint32_t block(size_t I) const {
      return Blocks.empty() ? Block : Blocks[I];
    }
  };
  void appendRows(llvm::ArrayRef<Instruction> Xs,
                  llvm::ArrayRef<Instruction> Ys, Locations X, Locations Y,
                  llvm::ArrayRef<std::pair<uint32_t, uint32_t>> Pairs,
                  std::vector<DeltaRow> &Rows) const;

  bool sameAddresses(const Instruction &X, const Instruction &Y) const {
    const auto XK = A.addressKey(X), YK = B.addressKey(Y);
    return XK && YK && XK == YK;
  }
  bool sameShape(const Instruction &X, const Instruction &Y) const {
    return X.Shape == Y.Shape && sameAddresses(X, Y);
  }

  void dense(llvm::ArrayRef<Instruction> Xs, llvm::ArrayRef<Instruction> Ys,
             uint32_t XBegin, uint32_t YBegin, InstructionPairs &Pairs);
  std::vector<uint64_t> frontier(llvm::ArrayRef<Instruction> Xs,
                                 llvm::ArrayRef<Instruction> Ys, bool Reverse);
  bool diagonal(llvm::ArrayRef<Instruction> Xs, llvm::ArrayRef<Instruction> Ys,
                uint32_t XBegin, uint32_t YBegin, InstructionPairs &Pairs);
  void match(llvm::ArrayRef<Instruction> Xs, llvm::ArrayRef<Instruction> Ys,
             uint32_t XBegin, uint32_t YBegin, InstructionPairs &Pairs);

  /// How well two instructions line up: 0 when they cannot.
  unsigned weight(const Instruction &X, const Instruction &Y) const {
    if (sameShape(X, Y))
      return 4;
    if (X.Skeleton == Y.Skeleton && sameAddresses(X, Y))
      return 3;
    if (A.mnemonic(X) == B.mnemonic(Y))
      return 2;
    return 0;
  }
  RowKind kind(const Instruction &X, const Instruction &Y) const {
    const bool Unknown =
        !A.addressKey(X) || !B.addressKey(Y) ||
        ((X.HasUnresolvedData || Y.HasUnresolvedData) && X.Shape != Y.Shape);
    const auto Callee = callId(X, true, Map);
    const bool DifferentCall =
        Callee != NoIndex && Y.Callee != NoIndex && Callee != Y.Callee;
    const bool DifferentReference = knownAddressDifference(A.P, X, B.P, Y, Map);
    if (Unknown && X.AddressIndependentShape == Y.AddressIndependentShape &&
        !DifferentCall && !DifferentReference)
      return RowKind::Uncertain;
    if (!DifferentCall && !DifferentReference) {
      if (sameShape(X, Y))
        return X.Size == Y.Size && llvm::equal(A.P.bytes(X), B.P.bytes(Y))
                   ? RowKind::Identical
                   : RowKind::Moved;
      if (X.Skeleton == Y.Skeleton && sameAddresses(X, Y))
        return RowKind::Constant;
    }
    if (A.mnemonic(X) == B.mnemonic(Y))
      return RowKind::Operand;
    return RowKind::Replaced;
  }
};

void Aligner::dense(llvm::ArrayRef<Instruction> Xs,
                    llvm::ArrayRef<Instruction> Ys, uint32_t XBegin,
                    uint32_t YBegin, InstructionPairs &Pairs) {
  const size_t N = Xs.size(), M = Ys.size();
  Score.assign((N + 1) * (M + 1), 0);
  const auto At = [&](size_t I, size_t J) -> uint32_t & {
    return Score[I * (M + 1) + J];
  };
  for (size_t I = N; I-- > 0;) {
    A.poll();
    for (size_t J = M; J-- > 0;) {
      if (J % limits::diff::kAlignmentPollCells == 0)
        A.poll();
      uint32_t Best = std::max(At(I + 1, J), At(I, J + 1));
      if (const unsigned W = weight(Xs[I], Ys[J]))
        Best = std::max(Best, At(I + 1, J + 1) + W);
      At(I, J) = Best;
    }
  }
  for (size_t I = 0, J = 0; I < N && J < M;) {
    A.poll();
    const unsigned W = weight(Xs[I], Ys[J]);
    if (W && At(I, J) == At(I + 1, J + 1) + W) {
      Pairs.emplace_back(XBegin + I, YBegin + J);
      ++I;
      ++J;
    } else if (At(I, J) == At(I + 1, J)) {
      ++I;
    } else {
      ++J;
    }
  }
}

std::vector<uint64_t> Aligner::frontier(llvm::ArrayRef<Instruction> Xs,
                                        llvm::ArrayRef<Instruction> Ys,
                                        bool Reverse) {
  std::vector<uint64_t> Scores(Ys.size() + 1, 0);
  for (size_t I = 0; I < Xs.size(); ++I) {
    A.poll();
    uint64_t Diagonal = 0;
    const auto &X = Xs[Reverse ? Xs.size() - I - 1 : I];
    for (size_t J = 0; J < Ys.size(); ++J) {
      if (J % limits::diff::kAlignmentPollCells == 0)
        A.poll();
      const auto &Y = Ys[Reverse ? Ys.size() - J - 1 : J];
      const uint64_t Previous = Scores[J + 1];
      Scores[J + 1] = std::max({Previous, Scores[J], Diagonal + weight(X, Y)});
      Diagonal = Previous;
    }
  }
  return Scores;
}

bool Aligner::diagonal(llvm::ArrayRef<Instruction> Xs,
                       llvm::ArrayRef<Instruction> Ys, uint32_t XBegin,
                       uint32_t YBegin, InstructionPairs &Pairs) {
  const size_t Count = std::min(Xs.size(), Ys.size());
  size_t Exact = 0;
  for (size_t I = 0; I < Count; ++I) {
    A.poll();
    if (sameShape(Xs[I], Ys[I]))
      ++Exact;
    else if (Xs[I].Skeleton != Ys[I].Skeleton || !sameAddresses(Xs[I], Ys[I]))
      return false;
  }
  // A non-exact pair scores at most three. Ignoring order, the multiset
  // intersection bounds the number of four-point pairs. If this diagonal
  // attains both bounds, no search can improve it. This keeps large sparse
  // constant edits linear without assuming that equal mnemonics are equal code.
  std::unordered_map<uint64_t, uint32_t> Shapes;
  for (const auto &Y : Ys) {
    A.poll();
    if (const auto Key = B.shape(Y))
      ++Shapes[*Key];
  }
  size_t Possible = 0;
  for (const auto &X : Xs) {
    A.poll();
    if (const auto Key = A.shape(X))
      if (auto It = Shapes.find(*Key); It != Shapes.end() && It->second) {
        --It->second;
        ++Possible;
      }
  }
  if (Exact != Possible)
    return false;
  for (size_t I = 0; I < Count; ++I) {
    A.poll();
    Pairs.emplace_back(XBegin + I, YBegin + I);
  }
  return true;
}

void Aligner::match(llvm::ArrayRef<Instruction> Xs,
                    llvm::ArrayRef<Instruction> Ys, uint32_t XBegin,
                    uint32_t YBegin, InstructionPairs &Pairs) {
  A.poll();
  if (Xs.empty() || Ys.empty())
    return;
  if (uint64_t(Xs.size()) + 1 <=
      limits::diff::kMaxAlignmentCells / (uint64_t(Ys.size()) + 1)) {
    dense(Xs, Ys, XBegin, YBegin, Pairs);
    return;
  }

  // Equal shapes receive the maximum weight, so taking common ends cannot
  // lower the optimum. Sparse edits in long blocks then need little search.
  size_t Prefix = 0;
  while (Prefix < Xs.size() && Prefix < Ys.size() &&
         sameShape(Xs[Prefix], Ys[Prefix])) {
    A.poll();
    Pairs.emplace_back(XBegin + Prefix, YBegin + Prefix);
    ++Prefix;
  }
  Xs = Xs.drop_front(Prefix);
  Ys = Ys.drop_front(Prefix);
  XBegin += Prefix;
  YBegin += Prefix;
  size_t Suffix = 0;
  while (Suffix < Xs.size() && Suffix < Ys.size() &&
         sameShape(Xs[Xs.size() - Suffix - 1], Ys[Ys.size() - Suffix - 1])) {
    A.poll();
    ++Suffix;
  }
  const auto XMiddle = Xs.drop_back(Suffix), YMiddle = Ys.drop_back(Suffix);
  if (Prefix || Suffix) {
    match(XMiddle, YMiddle, XBegin, YBegin, Pairs);
  } else if (diagonal(Xs, Ys, XBegin, YBegin, Pairs)) {
    return;
  } else if (Xs.size() == 1 || Ys.size() == 1) {
    // A very unequal pair can exceed the dense allowance even with one row.
    unsigned Best = 0;
    size_t BestX = 0, BestY = 0;
    for (size_t I = 0; I < Xs.size(); ++I)
      for (size_t J = 0; J < Ys.size(); ++J) {
        A.poll();
        if (const unsigned W = weight(Xs[I], Ys[J]); W > Best) {
          Best = W;
          BestX = I;
          BestY = J;
        }
      }
    if (Best)
      Pairs.emplace_back(XBegin + BestX, YBegin + BestY);
  } else {
    // Hirschberg decomposition of the same weighted recurrence as dense().
    // Every optimal path crosses the middle row. Its forward/backward scores
    // select a crossing without retaining the Cartesian table. Release both
    // frontiers before recursion; equal scores use the first crossing.
    const size_t Middle = Xs.size() / 2;
    size_t Split = 0;
    uint64_t Best = 0;
    {
      const auto Forward = frontier(Xs.take_front(Middle), Ys, false);
      const auto Backward = frontier(Xs.drop_front(Middle), Ys, true);
      for (size_t J = 0; J <= Ys.size(); ++J) {
        if (J % limits::diff::kAlignmentPollCells == 0)
          A.poll();
        if (const uint64_t Total = Forward[J] + Backward[Ys.size() - J];
            Total > Best) {
          Best = Total;
          Split = J;
        }
      }
    }
    if (Best) {
      match(Xs.take_front(Middle), Ys.take_front(Split), XBegin, YBegin, Pairs);
      match(Xs.drop_front(Middle), Ys.drop_front(Split), XBegin + Middle,
            YBegin + Split, Pairs);
    }
  }
  for (size_t I = 0; I < Suffix; ++I) {
    A.poll();
    Pairs.emplace_back(XBegin + XMiddle.size() + I,
                       YBegin + YMiddle.size() + I);
  }
}

void Aligner::align(uint32_t XBlock, uint32_t YBlock,
                    std::vector<DeltaRow> &Rows) {
  const auto Xs = A.instructions(XBlock);
  const auto Ys = B.instructions(YBlock);
  const uint32_t XBase = A.firstInstruction(XBlock);
  const uint32_t YBase = B.firstInstruction(YBlock);
  InstructionPairs Pairs;
  match(Xs, Ys, 0, 0, Pairs);
  appendRows(Xs, Ys, {XBase, XBlock, {}}, {YBase, YBlock, {}}, Pairs, Rows);
}

void Aligner::appendRows(llvm::ArrayRef<Instruction> Xs,
                         llvm::ArrayRef<Instruction> Ys, Locations X,
                         Locations Y,
                         llvm::ArrayRef<std::pair<uint32_t, uint32_t>> Pairs,
                         std::vector<DeltaRow> &Rows) const {
  const size_t N = Xs.size(), M = Ys.size();
  // Unaligned runs between aligned pairs are shown side by side.
  size_t I = 0, J = 0;
  const auto Gap = [&](size_t IEnd, size_t JEnd) {
    while (I < IEnd && J < JEnd) {
      A.poll();
      Rows.push_back({X.Base + uint32_t(I), Y.Base + uint32_t(J), X.block(I),
                      Y.block(J), RowKind::Replaced});
      ++I;
      ++J;
    }
    for (; I < IEnd; ++I) {
      A.poll();
      Rows.push_back({X.Base + uint32_t(I), NoIndex, X.block(I), NoIndex,
                      RowKind::Deleted});
    }
    for (; J < JEnd; ++J) {
      A.poll();
      Rows.push_back({NoIndex, Y.Base + uint32_t(J), NoIndex, Y.block(J),
                      RowKind::Inserted});
    }
  };
  for (const auto &[PI, PJ] : Pairs) {
    A.poll();
    Gap(PI, PJ);
    Rows.push_back({X.Base + PI, Y.Base + PJ, X.block(PI), Y.block(PJ),
                    kind(Xs[PI], Ys[PJ])});
    ++I;
    ++J;
  }
  Gap(N, M);
}

bool Aligner::refineOrderedSpans(std::vector<DeltaRow> &Rows,
                                 bool SingleBlockOnly) {
  if ((A.F.BlockCount <= 1 && B.F.BlockCount <= 1) ||
      (SingleBlockOnly && ((A.F.BlockCount != 1 && B.F.BlockCount != 1) ||
                           A.F.BlockCount == B.F.BlockCount)))
    return false;
  uint64_t OldWeight = 0;
  uint32_t OldEqual = 0;
  for (const auto &R : Rows) {
    A.poll();
    OldEqual += R.Kind == RowKind::Identical || R.Kind == RowKind::Moved;
    if (R.Primary != NoIndex && R.Secondary != NoIndex)
      OldWeight +=
          weight(A.P.Instructions[R.Primary], B.P.Instructions[R.Secondary]);
  }
  if (OldEqual >=
      std::min(A.P.instructionCount(A.F), B.P.instructionCount(B.F)))
    return false;
  struct Span {
    llvm::ArrayRef<Instruction> Instructions;
    uint32_t Base;
    std::vector<uint32_t> Blocks;
  };
  const auto Collect = [](const Side &S) -> std::optional<Span> {
    const auto Blocks = S.P.blocks(S.F);
    if (Blocks.empty())
      return {};
    const uint32_t Base = Blocks.front().FirstInstruction;
    size_t End = Base;
    for (uint32_t BI = 0; BI < Blocks.size(); ++BI) {
      S.poll();
      const auto &B = Blocks[BI];
      if (B.FirstInstruction != End ||
          (BI && B.Address <= Blocks[BI - 1].Address))
        return {};
      for (const auto &I : S.P.instructions(B)) {
        S.poll();
        if (I.Flow == InstructionFlow::Unknown ||
            I.Flow == InstructionFlow::IndirectCall ||
            I.Flow == InstructionFlow::IndirectJump)
          return {};
      }
      End += B.InstructionCount;
    }
    for (const auto &E : S.P.edges(S.F)) {
      S.poll();
      if (E.To <= E.From || E.Kind == EdgeKind::Exception ||
          E.Kind == EdgeKind::Switch)
        return {};
    }
    // Borrow original instructions: address signatures are keyed by their
    // addresses in the immutable Program, not by copied instruction values.
    Span Result{
        llvm::ArrayRef(S.P.Instructions).slice(Base, End - Base), Base, {}};
    if (Blocks.size() > 1) {
      Result.Blocks.reserve(End - Base);
      for (uint32_t BI = 0; BI < Blocks.size(); ++BI) {
        S.poll();
        Result.Blocks.insert(Result.Blocks.end(), Blocks[BI].InstructionCount,
                             BI);
      }
    }
    return Result;
  };
  const auto X = Collect(A), Y = Collect(B);
  if (!X || !Y)
    return false;
  InstructionPairs Pairs;
  match(X->Instructions, Y->Instructions, 0, 0, Pairs);
  uint64_t NewWeight = 0;
  uint32_t NewEqual = 0;
  for (const auto &[I, J] : Pairs) {
    A.poll();
    const auto &L = X->Instructions[I], &R = Y->Instructions[J];
    NewWeight += weight(L, R);
    const auto Kind = kind(L, R);
    NewEqual += Kind == RowKind::Identical || Kind == RowKind::Moved;
  }
  if (NewEqual <= OldEqual || NewWeight < OldWeight)
    return false;
  Rows.clear();
  appendRows(X->Instructions, Y->Instructions, {X->Base, 0, X->Blocks},
             {Y->Base, 0, Y->Blocks}, Pairs, Rows);
  return true;
}

bool unconditional(EdgeKind K) {
  return K == EdgeKind::Fallthrough || K == EdgeKind::Jump;
}

/// How \p E of one side fares among \p Other's edges, given each block's
/// counterpart.
EdgeState edgeState(const Edge &E, const std::vector<uint32_t> &Counterpart,
                    const Side &Other) {
  const uint32_t From = Counterpart[E.From], To = Counterpart[E.To];
  if (From == NoIndex || To == NoIndex)
    return EdgeState::Unmatched;
  bool Inverted = false;
  for (uint32_t Index : Other.Out.of(From)) {
    const Edge &O = Other.edge(Index);
    if (O.To != To)
      continue;
    if (O.Kind == E.Kind || (unconditional(O.Kind) && unconditional(E.Kind)))
      return EdgeState::Matched;
    if ((O.Kind == EdgeKind::Taken && E.Kind == EdgeKind::NotTaken) ||
        (O.Kind == EdgeKind::NotTaken && E.Kind == EdgeKind::Taken))
      Inverted = true;
  }
  return Inverted ? EdgeState::Inverted : EdgeState::Unmatched;
}

} // namespace

FunctionDelta compareFunctionsWithKeys(const Program &Primary, uint32_t PF,
                                       const Program &Secondary, uint32_t SF,
                                       const Correspondence &Map,
                                       llvm::ArrayRef<uint64_t> PrimaryKeys,
                                       llvm::ArrayRef<uint64_t> SecondaryKeys,
                                       const ComparisonControl &Control,
                                       InstructionAlignmentProfile Alignment) {
  try {
    FunctionDelta D;
    D.Alignment = Alignment;
    D.IdentityOnly =
        !Primary.Functions[PF].hasBody() && !Secondary.Functions[SF].hasBody();
    D.AnalysisComplete = Primary.Functions[PF].hasBody() &&
                         Secondary.Functions[SF].hasBody() &&
                         !Primary.Functions[PF].has(TraitIncomplete) &&
                         !Secondary.Functions[SF].has(TraitIncomplete);
    Side A(Primary, Primary.Functions[PF], PrimaryKeys, Control);
    A.poll();
    Side B(Secondary, Secondary.Functions[SF], SecondaryKeys, Control);
    B.poll();
    if (D.IdentityOnly)
      return D;
    computeKeys(A, true, Map);
    computeKeys(B, false, Map);
    BlockMatcher Matcher(A, B);
    Matcher.run();

    D.PrimaryBlockPair.assign(A.F.BlockCount, NoIndex);
    D.SecondaryBlockPair.assign(B.F.BlockCount, NoIndex);
    for (uint32_t X = 0; X < A.F.BlockCount; ++X)
      if (const uint32_t Y = Matcher.AMatch[X]; Y != NoIndex) {
        D.PrimaryBlockPair[X] = static_cast<uint32_t>(D.Blocks.size());
        D.SecondaryBlockPair[Y] = static_cast<uint32_t>(D.Blocks.size());
        D.Blocks.push_back({X, Y, Matcher.AStep[X], false});
      }

    // The listing: primary blocks in order, each secondary-only block after
    // the pair of the matched secondary block before it.
    std::vector<std::vector<uint32_t>> After(A.F.BlockCount + 1);
    {
      uint32_t Previous = A.F.BlockCount; // Before every primary block.
      std::vector<uint32_t> YOrder(B.F.BlockCount);
      for (uint32_t I = 0; I < YOrder.size(); ++I)
        YOrder[I] = I;
      llvm::sort(YOrder, [&](uint32_t L, uint32_t R) {
        return B.P.blocks(B.F)[L].Address < B.P.blocks(B.F)[R].Address;
      });
      for (uint32_t Y : YOrder) {
        if (const uint32_t X = Matcher.BMatch[Y]; X != NoIndex)
          Previous = X;
        else
          After[Previous].push_back(Y);
      }
    }
    Aligner Align(A, B, Map);
    const auto Inserted = [&](uint32_t Y) {
      const uint32_t Base = B.firstInstruction(Y);
      for (uint32_t I = 0; I < B.Keys[Y].Count; ++I)
        D.Rows.push_back({NoIndex, Base + I, NoIndex, Y, RowKind::Inserted});
    };
    for (uint32_t Y : After[A.F.BlockCount])
      Inserted(Y);
    for (uint32_t X = 0; X < A.F.BlockCount; ++X) {
      A.poll();
      if (const uint32_t P = D.PrimaryBlockPair[X]; P != NoIndex) {
        const size_t First = D.Rows.size();
        Align.align(X, D.Blocks[P].Secondary, D.Rows);
        D.Blocks[P].Unchanged = std::all_of(
            D.Rows.begin() + First, D.Rows.end(), [](const DeltaRow &R) {
              return R.Kind == RowKind::Identical || R.Kind == RowKind::Moved;
            });
        D.Blocks[P].Uncertain =
            !D.Blocks[P].Unchanged &&
            std::all_of(D.Rows.begin() + First, D.Rows.end(),
                        [](const DeltaRow &R) {
                          return R.Kind == RowKind::Identical ||
                                 R.Kind == RowKind::Moved ||
                                 R.Kind == RowKind::Uncertain;
                        });
      } else {
        const uint32_t Base = A.firstInstruction(X);
        for (uint32_t I = 0; I < A.Keys[X].Count; ++I)
          D.Rows.push_back({Base + I, NoIndex, X, NoIndex, RowKind::Deleted});
      }
      for (uint32_t Y : After[X])
        Inserted(Y);
    }

    if (D.AnalysisComplete &&
        Alignment != InstructionAlignmentProfile::PairedBlocks &&
        Align.refineOrderedSpans(
            D.Rows,
            Alignment == InstructionAlignmentProfile::SingleBlockSpans)) {
      // Equal block/edge counts do not preserve boundaries: an instruction
      // can move across an internal branch destination without changing
      // either count. Such a refined listing must never become unchanged.
      D.Changes |= ChangeBlocks;
      for (auto &Pair : D.Blocks)
        Pair.Unchanged = Pair.Uncertain = true;
      // Instruction rows can cross a whole-block correspondence. Such rows
      // never make either whole block unchanged or equate its CFG edges.
      for (const auto &Row : D.Rows) {
        A.poll();
        const uint32_t X = Row.PrimaryBlock == NoIndex
                               ? NoIndex
                               : D.PrimaryBlockPair[Row.PrimaryBlock];
        const uint32_t Y = Row.SecondaryBlock == NoIndex
                               ? NoIndex
                               : D.SecondaryBlockPair[Row.SecondaryBlock];
        for (uint32_t I : {X, Y}) {
          if (I == NoIndex)
            continue;
          auto &Pair = D.Blocks[I];
          const bool Both = Row.PrimaryBlock == Pair.Primary &&
                            Row.SecondaryBlock == Pair.Secondary;
          const bool Equal =
              Row.Kind == RowKind::Identical || Row.Kind == RowKind::Moved;
          Pair.Unchanged &= Both && Equal;
          Pair.Uncertain &= Both && (Equal || Row.Kind == RowKind::Uncertain);
        }
      }
      for (auto &Pair : D.Blocks)
        Pair.Uncertain &= !Pair.Unchanged;
    }

    for (const Edge &E : Primary.edges(A.F))
      D.PrimaryEdges.push_back(edgeState(E, Matcher.AMatch, B));
    for (const Edge &E : Secondary.edges(B.F))
      D.SecondaryEdges.push_back(edgeState(E, Matcher.BMatch, A));

    // What changed.
    uint32_t &C = D.Changes;
    unsigned Aligned = 0;
    for (const DeltaRow &R : D.Rows) {
      A.poll();
      switch (R.Kind) {
      case RowKind::Uncertain:
        D.ReferenceComparisonComplete = false;
        break;
      case RowKind::Identical:
        ++D.EqualInstructions;
        break;
      case RowKind::Moved:
        ++D.EqualInstructions;
        C |= ChangeBytes | ChangeRelocation;
        break;
      case RowKind::Constant:
        C |= ChangeConstants;
        break;
      case RowKind::Operand:
        C |= ChangeOperands;
        break;
      case RowKind::Replaced:
      case RowKind::Inserted:
      case RowKind::Deleted:
        C |= ChangeInstructions;
        break;
      }
      const Instruction *X =
          R.Primary != NoIndex ? &Primary.Instructions[R.Primary] : nullptr;
      const Instruction *Y = R.Secondary != NoIndex
                                 ? &Secondary.Instructions[R.Secondary]
                                 : nullptr;
      if (X && Y)
        ++Aligned;
      if (!X || !Y || !llvm::equal(Primary.bytes(*X), Secondary.bytes(*Y)))
        C |= ChangeBytes;
      if (X && Y &&
          (!A.addressKey(*X) || !B.addressKey(*Y) ||
           ((X->HasUnresolvedData || Y->HasUnresolvedData) &&
            X->Shape != Y->Shape)))
        D.ReferenceComparisonComplete = false;
      if (X && Y && knownAddressDifference(Primary, *X, Secondary, *Y, Map))
        C |= ChangeReferences;
      const bool UnknownCall =
          (X && X->UnresolvedCallee) || (Y && Y->UnresolvedCallee);
      if (UnknownCall)
        D.ReferenceComparisonComplete = false;
      const bool XCalls = X && X->Callee != NoIndex;
      const bool YCalls = Y && Y->Callee != NoIndex;
      if (!UnknownCall && XCalls != YCalls) {
        // A call that only one side makes, unless the other side's
        // instruction is not on this row at all and is a call elsewhere.
        if ((X && Y) || (XCalls && !Y) || (YCalls && !X))
          C |= ChangeCalls;
      } else if (!UnknownCall && XCalls && YCalls) {
        const auto Target = callId(*X, true, Map);
        if (Target == NoIndex)
          D.ReferenceComparisonComplete = false;
        else if (Target != Y->Callee)
          C |= ChangeCalls;
      }
    }
    if (D.Blocks.size() != A.F.BlockCount || D.Blocks.size() != B.F.BlockCount)
      C |= ChangeBytes | ChangeBlocks;
    for (auto States : {&D.PrimaryEdges, &D.SecondaryEdges})
      for (EdgeState S : *States) {
        if (S == EdgeState::Unmatched)
          C |= ChangeBytes | ChangeEdges;
        else if (S == EdgeState::Inverted)
          C |= ChangeBytes | ChangeBranchInversion;
      }
    for (EdgeState S : D.PrimaryEdges)
      D.MatchedEdges += S != EdgeState::Unmatched;
    D.PrimaryLoops = A.Graph.Loops;
    D.SecondaryLoops = B.Graph.Loops;
    if (D.PrimaryLoops != D.SecondaryLoops)
      C |= ChangeLoops;
    if (A.F.EntryBlock != NoIndex && B.F.EntryBlock != NoIndex) {
      const uint32_t P = D.PrimaryBlockPair[A.F.EntryBlock];
      if (P == NoIndex || D.Blocks[P].Secondary != B.F.EntryBlock ||
          (!D.Blocks[P].Unchanged && !D.Blocks[P].Uncertain))
        C |= ChangeEntry;
    } else if ((A.F.EntryBlock == NoIndex) != (B.F.EntryBlock == NoIndex)) {
      C |= ChangeEntry;
    }
    {
      struct ReferenceKeys {
        std::vector<std::pair<ReferenceKind, uint64_t>> Other;
        std::vector<uint64_t> Functions, UnknownData;
        bool FunctionGap = false;
      };
      const auto Named = [&](const Program &P, const Function &F,
                             bool IsPrimary) {
        ReferenceKeys Out;
        for (const Reference &R : P.references(F))
          if (R.Kind != ReferenceKind::Constant) {
            if (R.Kind == ReferenceKind::UnresolvedData) {
              Out.UnknownData.push_back(R.Key);
              continue;
            }
            const auto Key = referenceId(R, P, IsPrimary, Map);
            if (!Key)
              Out.FunctionGap = true;
            else if (R.Kind == ReferenceKind::Function)
              Out.Functions.push_back(*Key);
            else
              Out.Other.emplace_back(R.Kind, *Key);
          }
        llvm::sort(Out.Other);
        llvm::sort(Out.Functions);
        llvm::sort(Out.UnknownData);
        return Out;
      };
      const auto X = Named(Primary, A.F, true),
                 Y = Named(Secondary, B.F, false);
      const bool DataGap = X.UnknownData != Y.UnknownData;
      const bool FunctionGap = X.FunctionGap || Y.FunctionGap;
      if (DataGap || FunctionGap)
        D.ReferenceComparisonComplete = false;
      if (!DataGap &&
          (X.Other != Y.Other || (!FunctionGap && X.Functions != Y.Functions)))
        C |= ChangeReferences;
    }

    // Similarity from what lines up.
    const auto Ratio = [](double Same, double Total) {
      return Total == 0 ? 1.0 : Same / Total;
    };
    const uint64_t XInsns = Primary.instructionCount(A.F);
    const uint64_t YInsns = Secondary.instructionCount(B.F);
    const double Insns =
        Ratio(2.0 * D.EqualInstructions, double(XInsns + YInsns));
    const double Blocks =
        Ratio(2.0 * D.Blocks.size(), double(A.F.BlockCount + B.F.BlockCount));
    unsigned MatchedSecondaryEdges = 0;
    for (EdgeState S : D.SecondaryEdges)
      MatchedSecondaryEdges += S != EdgeState::Unmatched;
    const double Edges = Ratio(double(D.MatchedEdges + MatchedSecondaryEdges),
                               double(A.F.EdgeCount + B.F.EdgeCount));
    double Similarity = limits::diff::kSimilarityInstructionWeight * Insns +
                        limits::diff::kSimilarityBlockWeight * Blocks +
                        limits::diff::kSimilarityEdgeWeight * Edges;
    if ((C & ~MovementChanges) || !D.ReferenceComparisonComplete)
      Similarity = std::min(Similarity, limits::diff::kMaxChangedSimilarity);
    else
      Similarity = 1;
    D.Similarity = std::clamp(Similarity, 0.0, 1.0);
    (void)Aligned;
    A.poll();
    return D;
  } catch (const DeltaInterrupted &Stop) {
    FunctionDelta D;
    D.Alignment = Alignment;
    D.State = Stop.State;
    D.ComputationComplete = false;
    D.AnalysisComplete = false;
    D.ReferenceComparisonComplete = false;
    return D;
  }
}

std::vector<uint64_t> mnemonicKeys(const Program &P,
                                   const ComparisonControl &Control) {
  Control.check();
  std::vector<uint64_t> Keys;
  Keys.reserve(P.Mnemonics.size());
  for (const std::string &M : P.Mnemonics) {
    Control.check();
    Keys.push_back(hashText(M));
  }
  return Keys;
}

FunctionDelta compareFunctions(const Program &Primary, uint32_t PF,
                               const Program &Secondary, uint32_t SF,
                               const Correspondence &Map,
                               const ComparisonControl &Control,
                               InstructionAlignmentProfile Alignment) {
  const std::vector<uint64_t> PK = mnemonicKeys(Primary),
                              SK = mnemonicKeys(Secondary);
  return compareFunctionsWithKeys(Primary, PF, Secondary, SF, Map, PK, SK,
                                  Control, Alignment);
}

uint32_t loopCount(const Program &P, const Function &F) {
  return graphShape(P, F).Loops;
}

} // namespace neverd::diff
