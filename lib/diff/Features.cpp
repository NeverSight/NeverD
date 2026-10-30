//===- Features.cpp - What identifies a function or block -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Computing the exact features, flow-graph shapes, call graph and sketches
/// the comparison pairs functions by.
///
//===----------------------------------------------------------------------===//

#include "Features.h"

#include "neverd/diff/Limits.h"
#include "neverd/diff/StableHash.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <deque>
#include <utility>

namespace neverd::diff {

namespace {

/// How far a chain of thunks is followed to the function it leads to.
constexpr unsigned MaxThunkChain = 8;

/// Seeds of the sketch's hash functions.
constexpr uint64_t SketchSeed = 0x510e527fade682d1ULL;

Adjacency buildAdjacency(size_t Nodes,
                         std::vector<std::pair<uint32_t, uint32_t>> Pairs,
                         bool Distinct) {
  if (Distinct) {
    llvm::sort(Pairs);
    Pairs.erase(std::unique(Pairs.begin(), Pairs.end()), Pairs.end());
  } else {
    std::stable_sort(
        Pairs.begin(), Pairs.end(),
        [](const auto &A, const auto &B) { return A.first < B.first; });
  }
  Adjacency A;
  A.Start.assign(Nodes + 1, 0);
  for (const auto &[From, To] : Pairs)
    ++A.Start[From + 1];
  for (size_t I = 0; I < Nodes; ++I)
    A.Start[I + 1] += A.Start[I];
  A.Targets.reserve(Pairs.size());
  for (const auto &[From, To] : Pairs)
    A.Targets.push_back(To);
  return A;
}

/// Breadth-first levels from \p Roots over \p Next; nodes no root reaches
/// share the level after the deepest one reached.
template <typename NextFn>
std::vector<uint32_t> levels(size_t Nodes, llvm::ArrayRef<uint32_t> Roots,
                             NextFn Next, const ComparisonControl &Control) {
  constexpr uint32_t Unreached = NoIndex;
  std::vector<uint32_t> Level(Nodes, Unreached);
  std::deque<uint32_t> Queue;
  for (uint32_t Root : Roots)
    if (Level[Root] == Unreached) {
      Level[Root] = 0;
      Queue.push_back(Root);
    }
  uint32_t Deepest = 0;
  while (!Queue.empty()) {
    if (Control.stop())
      return Level;
    const uint32_t Node = Queue.front();
    Queue.pop_front();
    Deepest = std::max(Deepest, Level[Node]);
    Next(Node, [&](uint32_t To) {
      if (Level[To] == Unreached) {
        Level[To] = Level[Node] + 1;
        Queue.push_back(To);
      }
    });
  }
  for (uint32_t &L : Level)
    if (L == Unreached)
      L = Deepest + 1;
  return Level;
}

void addToSketch(FunctionFeatures &F, uint64_t Shingle) {
  for (unsigned I = 0; I < SketchSize; ++I) {
    const uint32_t V = static_cast<uint32_t>(
        mix64(Shingle ^ (SketchSeed + 0x9e3779b97f4a7c15ULL * (I + 1))) >> 32);
    F.Sketch[I] = std::min(F.Sketch[I], V);
  }
  F.Sketched = true;
}

} // namespace

uint64_t edgeIndex(uint32_t Level, uint32_t SourceIn, uint32_t SourceOut,
                   uint32_t TargetIn, uint32_t TargetOut) {
  return StableHash(0x9b05688c2b3e6c1fULL)
      .add(Level)
      .add(SourceIn)
      .add(SourceOut)
      .add(TargetIn)
      .add(TargetOut)
      .value();
}

GraphShape graphShape(const Program &P, const Function &F,
                      const ComparisonControl &Control) {
  GraphShape G;
  const uint32_t N = F.BlockCount;
  G.In.assign(N, 0);
  G.Out.assign(N, 0);
  const auto Edges = P.edges(F);
  std::vector<std::pair<uint32_t, uint32_t>> Forward, Backward;
  Forward.reserve(Edges.size());
  Backward.reserve(Edges.size());
  for (const Edge &E : Edges) {
    if (Control.stop())
      return G;
    ++G.Out[E.From];
    ++G.In[E.To];
    Forward.emplace_back(E.From, E.To);
    Backward.emplace_back(E.To, E.From);
  }
  const Adjacency Succ = buildAdjacency(N, Forward, false);
  const Adjacency Pred = buildAdjacency(N, Backward, false);
  std::vector<uint32_t> Entry;
  if (F.EntryBlock != NoIndex)
    Entry.push_back(F.EntryBlock);
  G.TopLevel = levels(
      N, Entry,
      [&](uint32_t Node, auto Visit) {
        for (uint32_t To : Succ.of(Node))
          Visit(To);
      },
      Control);
  std::vector<uint32_t> Exits;
  for (uint32_t I = 0; I < N; ++I)
    if (G.Out[I] == 0)
      Exits.push_back(I);
  G.BottomLevel = levels(
      N, Exits,
      [&](uint32_t Node, auto Visit) {
        for (uint32_t To : Pred.of(Node))
          Visit(To);
      },
      Control);
  // Back edges of a depth-first walk from the entry close loops.
  if (F.EntryBlock != NoIndex) {
    enum : uint8_t { White, Grey, Black };
    std::vector<uint8_t> Color(N, White);
    std::vector<std::pair<uint32_t, uint32_t>> Stack;
    Stack.emplace_back(F.EntryBlock, 0);
    Color[F.EntryBlock] = Grey;
    while (!Stack.empty()) {
      if (Control.stop())
        return G;
      auto &[Node, Next] = Stack.back();
      const auto Out = Succ.of(Node);
      if (Next == Out.size()) {
        Color[Node] = Black;
        Stack.pop_back();
        continue;
      }
      const uint32_t To = Out[Next++];
      if (Color[To] == Grey)
        ++G.Loops;
      else if (Color[To] == White) {
        Color[To] = Grey;
        Stack.emplace_back(To, 0);
      }
    }
  }
  return G;
}

double sketchSimilarity(const FunctionFeatures &A, const FunctionFeatures &B) {
  if (!A.Sketched || !B.Sketched)
    return 0;
  unsigned Same = 0;
  for (unsigned I = 0; I < SketchSize; ++I)
    Same += A.Sketch[I] == B.Sketch[I];
  return static_cast<double>(Same) / SketchSize;
}

uint64_t sketchBand(const FunctionFeatures &F, unsigned Band) {
  constexpr unsigned Rows = SketchSize / SketchBands;
  StableHash H(0x1f83d9abfb41bd6bULL + Band);
  for (unsigned I = Band * Rows; I < (Band + 1) * Rows; ++I)
    H.add(F.Sketch[I]);
  return H.value();
}

ProgramFeatures computeFeatures(const Program &P,
                                const ComparisonControl &Control) {
  ProgramFeatures R;
  const size_t N = P.Functions.size();
  R.MnemonicKeys.reserve(P.Mnemonics.size());
  for (const std::string &M : P.Mnemonics)
    R.MnemonicKeys.push_back(hashText(M));

  // Keep unresolved calls as holes in positional evidence; they are not
  // invented graph edges and must not collapse the surrounding call order.
  std::vector<std::pair<uint32_t, uint32_t>> Sites;
  for (uint32_t FI = 0; FI < N; ++FI)
    for (const Block &B : P.blocks(P.Functions[FI]))
      for (const Instruction &I : P.instructions(B)) {
        if (Control.stop())
          return R;
        if (I.Callee != NoIndex || I.UnresolvedCallee ||
            I.Flow == InstructionFlow::Call ||
            I.Flow == InstructionFlow::IndirectCall)
          Sites.emplace_back(FI, I.Callee);
      }
  R.CallSites = buildAdjacency(N, Sites, false);
  llvm::erase_if(Sites,
                 [](const auto &Site) { return Site.second == NoIndex; });
  R.Callees = buildAdjacency(N, Sites, true);
  for (auto &[From, To] : Sites)
    std::swap(From, To);
  R.Callers = buildAdjacency(N, std::move(Sites), true);

  std::vector<std::pair<uint32_t, uint32_t>> References;
  for (uint32_t FI = 0; FI < N; ++FI)
    for (const Reference &Ref : P.references(P.Functions[FI])) {
      if (Control.stop())
        return R;
      if (Ref.Kind == ReferenceKind::Function)
        if (const auto Target = P.functionAt(Ref.Key))
          References.emplace_back(FI, *Target);
    }
  R.Referents = buildAdjacency(N, References, true);
  for (auto &[From, To] : References)
    std::swap(From, To);
  R.Referrers = buildAdjacency(N, std::move(References), true);

  // A thunk stands for the one function it passes control to.
  R.Resolved.resize(N);
  for (uint32_t FI = 0; FI < N; ++FI) {
    if (Control.stop())
      return R;
    uint32_t Current = FI;
    for (unsigned Step = 0; Step < MaxThunkChain; ++Step) {
      const Function &F = P.Functions[Current];
      const auto Out = R.Callees.of(Current);
      if (!F.has(TraitThunk) || Out.size() != 1 || Out[0] == Current)
        break;
      Current = Out[0];
    }
    R.Resolved[FI] = Current;
  }

  R.Functions.resize(N);
  parallelForEach(N, [&](auto Claim, size_t Total) {
    std::vector<uint64_t> Keys;
    std::vector<uint64_t> Skeletons;
    for (size_t Index = Claim(); Index < Total; Index = Claim()) {
      if (Control.stop())
        return;
      const Function &F = P.Functions[Index];
      FunctionFeatures &Out = R.Functions[Index];
      if (F.Source != NameSource::Generated && !F.Name.empty() &&
          !F.has(TraitImport))
        Out.Name = hashText(F.Name);
      if (F.has(TraitImport))
        Out.Import = hashText(llvm::StringRef(F.Name).lower());
      if (!F.hasBody())
        continue;

      if (F.BlockCount == 1 && !F.EdgeCount && !F.has(TraitThunk)) {
        const auto Instructions = P.instructions(P.blocks(F).front());
        if (Instructions.size() > limits::diff::kMinTailPreludeContribution &&
            Instructions.back().Flow == InstructionFlow::Jump &&
            Instructions.back().Callee != NoIndex &&
            Instructions.back().Callee != Index &&
            llvm::all_of(Instructions.drop_back(), [](const Instruction &I) {
              return I.Flow == InstructionFlow::None && I.Callee == NoIndex &&
                     !I.UnresolvedCallee;
            }))
          Out.TailCallee = Instructions.back().Callee;
      }

      StableHash Bytes, Shape, Mnemonics, BodyWithoutPadding, BodyWithPadding,
          EntryBody;
      uint32_t BodyInstructions = 0, EntryInstructions = 0;
      bool EntryCalls = false;
      const Block *Entry =
          F.EntryBlock == NoIndex ? nullptr : &P.blocks(F)[F.EntryBlock];
      uint64_t Prime = 1;
      Skeletons.clear();
      for (const Block &B : P.blocks(F))
        for (const Instruction &I : P.instructions(B)) {
          if (Control.stop())
            return;
          Bytes.add(P.bytes(I));
          Shape.add(I.Shape);
          BodyWithPadding.add(I.AddressIndependentShape);
          const auto &Name = P.Mnemonics[I.Mnemonic];
          const bool Landing = Entry && I.Address == Entry->Address &&
                               (Name == "endbr32" || Name == "endbr64");
          if (I.Flow != InstructionFlow::None || (Name != "nop" && !Landing)) {
            BodyWithoutPadding.add(I.AddressIndependentShape);
            ++BodyInstructions;
            if (&B == Entry) {
              EntryBody.add(I.AddressIndependentShape);
              ++EntryInstructions;
              EntryCalls |= I.Flow == InstructionFlow::Call ||
                            I.Flow == InstructionFlow::IndirectCall;
            }
          }
          const uint64_t Mnemonic = R.MnemonicKeys[I.Mnemonic];
          Mnemonics.add(Mnemonic);
          Prime *= oddFactor(Mnemonic);
          Skeletons.push_back(I.Skeleton);
          Out.AddressEncodings |= I.Target.has_value() || I.Callee != NoIndex ||
                                  I.AddressReferenceCount != 0;
          for (const auto &Address : P.addressReferences(I))
            if (Address.Kind == ReferenceKind::Function)
              Out.AddressSites.push_back(
                  {P.functionAt(Address.Key).value_or(NoIndex),
                   Out.Instructions, Address.Operand});
            else
              Out.DataSites.push_back({Address.Key, Out.Instructions,
                                       Address.Operand, Address.Kind});
          if (I.HasUnresolvedData &&
              !llvm::any_of(P.addressReferences(I), [](const auto &Address) {
                return Address.Kind == ReferenceKind::UnresolvedData;
              }))
            Out.DataSitesComplete = false;
          if (I.Callee != NoIndex)
            Out.AddressSites.push_back({I.Callee, Out.Instructions, NoIndex});
          Out.UnresolvedCallee |= I.UnresolvedCallee;
          ++Out.Instructions;
          if (I.Flow == InstructionFlow::Call ||
              I.Flow == InstructionFlow::IndirectCall)
            ++Out.Calls;
        }
      // Equal normalized bodies have equal flow graphs too.
      Keys.clear();
      for (const Edge &E : P.edges(F))
        Keys.push_back((uint64_t(E.From) << 36) | (uint64_t(E.To) << 4) |
                       uint64_t(E.Kind));
      llvm::sort(Keys);
      StableHash ReferenceLayout(BodyWithPadding.value());
      ReferenceLayout.add(F.BlockCount).add(F.EdgeCount);
      for (const Block &B : P.blocks(F))
        ReferenceLayout.add(B.InstructionCount);
      for (uint64_t K : Keys) {
        Shape.add(K);
        ReferenceLayout.add(K);
      }
      Shape.add(F.EntryBlock);
      Out.ReferenceLayout = ReferenceLayout.add(F.EntryBlock).value();

      Out.Blocks = F.BlockCount;
      Out.Edges = F.EdgeCount;
      Out.Bytes = Bytes.value();
      Out.Shape = Shape.value();
      if (BodyInstructions >= limits::diff::kMinShapeInstructions)
        Out.BodyWithoutPadding = BodyWithoutPadding.value();
      Out.BodyWithPadding = BodyWithPadding.value();
      if (F.BlockCount > 1 && !EntryCalls &&
          EntryInstructions >= limits::diff::kMinBlockHashInstructions &&
          P.instructions(*Entry).back().Flow == InstructionFlow::Branch)
        Out.EntryBody = EntryBody.value();
      Out.Mnemonics = Mnemonics.value();
      Out.Prime = Prime;

      const GraphShape G = graphShape(P, F, Control);
      if (Control.stop())
        return;
      Out.Loops = G.Loops;
      if (F.EdgeCount) {
        uint64_t Top = 0, Bottom = 0;
        for (const Edge &E : P.edges(F)) {
          Top += edgeIndex(G.TopLevel[E.From], G.In[E.From], G.Out[E.From],
                           G.In[E.To], G.Out[E.To]);
          Bottom += edgeIndex(G.BottomLevel[E.From], G.In[E.From],
                              G.Out[E.From], G.In[E.To], G.Out[E.To]);
        }
        Out.GraphTopDown = Top ? Top : 1;
        Out.GraphBottomUp = Bottom ? Bottom : 1;
      }

      StableHash Strings, Constants;
      unsigned StringCount = 0, ConstantCount = 0;
      Keys.clear();
      for (const auto &Site : Out.DataSites)
        if (Site.Kind == ReferenceKind::UnresolvedData)
          Keys.push_back(Site.Key);
      llvm::sort(Keys);
      for (const Reference &Ref : P.references(F)) {
        if (Control.stop())
          return;
        if (Ref.Kind == ReferenceKind::String) {
          Strings.add(Ref.Key);
          ++StringCount;
        } else if (Ref.Kind == ReferenceKind::Constant) {
          Constants.add(Ref.Key);
          ++ConstantCount;
        } else if (Ref.Kind == ReferenceKind::Function) {
          ++Out.FunctionReferences;
        } else if (Ref.Kind == ReferenceKind::UnresolvedData) {
          Out.UnresolvedData = true;
          Out.DataSitesComplete &= llvm::binary_search(Keys, Ref.Key);
        }
      }
      if (StringCount)
        Out.Strings = Strings.value();
      if (ConstantCount >= limits::diff::kMinConstantSetSize)
        Out.Constants = Constants.value();

      Keys.clear();
      for (uint32_t Callee : R.CallSites.of(Index)) {
        if (Callee == NoIndex)
          continue;
        const Function &Target = P.Functions[R.Resolved[Callee]];
        if (Target.has(TraitImport))
          Keys.push_back(hashText(llvm::StringRef(Target.Name).lower()));
      }
      llvm::sort(Keys);
      Keys.erase(std::unique(Keys.begin(), Keys.end()), Keys.end());
      if (Keys.size() >= limits::diff::kMinImportCallSetSize) {
        StableHash Imports;
        for (uint64_t K : Keys)
          Imports.add(K);
        Out.ImportCalls = Imports.value();
      }

      // Runs of three skeletons, so a changed constant keeps the sketch.
      Out.Sketch.fill(UINT32_MAX);
      if (Skeletons.size() < 3) {
        for (uint64_t S : Skeletons)
          addToSketch(Out, S);
      } else {
        for (size_t I = 0; I + 2 < Skeletons.size(); ++I) {
          if (Control.stop())
            return;
          addToSketch(Out, StableHash()
                               .add(Skeletons[I])
                               .add(Skeletons[I + 1])
                               .add(Skeletons[I + 2])
                               .value());
        }
      }
    }
  });

  if (Control.stop())
    return R;
  // Features that look at neighbors, once every function has its own.
  std::vector<uint32_t> Roots, Leaves;
  std::vector<std::pair<uint32_t, uint32_t>> TailCalls;
  for (uint32_t FI = 0; FI < N; ++FI) {
    if (Control.stop())
      return R;
    if (R.Callers.of(FI).empty())
      Roots.push_back(FI);
    if (R.Callees.of(FI).empty())
      Leaves.push_back(FI);
    if (R.Functions[FI].TailCallee != NoIndex)
      TailCalls.emplace_back(R.Functions[FI].TailCallee, FI);
  }
  R.TailCallers = buildAdjacency(N, std::move(TailCalls), true);
  const std::vector<uint32_t> Top = levels(
      N, Roots,
      [&](uint32_t Node, auto Visit) {
        for (uint32_t To : R.Callees.of(Node))
          Visit(To);
      },
      Control);
  const std::vector<uint32_t> Bottom = levels(
      N, Leaves,
      [&](uint32_t Node, auto Visit) {
        for (uint32_t To : R.Callers.of(Node))
          Visit(To);
      },
      Control);
  parallelForEach(N, [&](auto Claim, size_t Total) {
    std::vector<uint64_t> Around;
    for (size_t Index = Claim(); Index < Total; Index = Claim()) {
      if (Control.stop())
        return;
      FunctionFeatures &Out = R.Functions[Index];
      if (!P.Functions[Index].hasBody())
        continue;
      const auto Callees = R.Callees.of(Index);
      const auto Callers = R.Callers.of(Index);
      if (!Callees.empty() || !Callers.empty())
        Out.CallGraph = StableHash(0x5be0cd19137e2179ULL)
                            .add(Top[Index])
                            .add(Bottom[Index])
                            .add(Callers.size())
                            .add(Callees.size())
                            .value();
      if (!Out.GraphTopDown || (Callees.empty() && Callers.empty()))
        continue;
      StableHash H(Out.GraphTopDown);
      for (auto Side : {Callees, Callers}) {
        Around.clear();
        for (uint32_t Other : Side)
          Around.push_back(R.Functions[Other].GraphTopDown);
        llvm::sort(Around);
        H.add(Around.size());
        for (uint64_t K : Around)
          H.add(K);
      }
      Out.Neighborhood = H.value();
    }
  });
  return R;
}

} // namespace neverd::diff
