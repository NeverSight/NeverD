//===- ReferenceContext.cpp - Independently paired address uses -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Equal instruction layouts in independently matched functions align address
/// operands. Consistent bidirectional reference relations can distinguish
/// otherwise equal candidate bodies without equating unknown data objects.
///
//===----------------------------------------------------------------------===//

#include "ReferenceContext.h"

#include "Features.h"

#include "neverd/diff/Limits.h"
#include "neverd/diff/Program.h"
#include "neverd/diff/StableHash.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <climits>
#include <optional>
#include <tuple>

namespace neverd::diff {
namespace {

struct Relation {
  uint64_t Other;
  bool Conflict = false;
};
using Relations = llvm::DenseMap<uint64_t, Relation>;

enum class ContextMode {
  DataWitness,
  DataCandidate,
  FunctionWitness,
  CallWitness
};

void recordRelation(Relations &Map, uint64_t A, uint64_t B) {
  auto [It, Inserted] = Map.try_emplace(A, Relation{B});
  if (!Inserted && It->second.Other != B)
    It->second.Conflict = true;
}

/// A complete occurrence signature. Before relations exist, unknown data keys
/// are omitted only to align anchors that already correspond independently.
std::optional<uint64_t>
referenceKey(const Program &P, const ProgramFeatures &PF, uint32_t FI,
             llvm::ArrayRef<uint32_t> Match, bool Secondary, ContextMode Mode,
             const Relations *Forward, const Relations *Reverse,
             const ComparisonControl &Control,
             llvm::ArrayRef<uint32_t> Weak = {}) {
  const auto &F = PF.Functions[FI];
  if (F.Instructions < limits::diff::kMinDataReferenceInstructions ||
      P.Functions[FI].has(TraitIncomplete) || !F.DataSitesComplete ||
      F.UnresolvedCallee || PF.Referents.of(FI).size() != F.FunctionReferences)
    return {};
  StableHash H(F.ReferenceLayout);
  H.add(F.AddressSites.size()).add(F.DataSites.size());
  bool FunctionOperand = false, ProvisionalCall = false;
  for (const auto &Site : F.AddressSites) {
    if (Control.stop() || Site.Target == NoIndex)
      return {};
    H.add(Site.Instruction).add(Site.Operand);
    if (Mode == ContextMode::CallWitness && Site.Operand == NoIndex &&
        Site.Target != FI &&
        (Match[Site.Target] == NoIndex || Weak[Site.Target] != NoIndex)) {
      ProvisionalCall = true;
      H.add("provisional_call");
      continue;
    }
    if (Mode == ContextMode::FunctionWitness && Site.Operand != NoIndex) {
      FunctionOperand = true;
      if (Match[Site.Target] == NoIndex) {
        H.add("unmapped_function");
        continue;
      }
    }
    if (Site.Target == FI) {
      H.add("self");
      continue;
    }
    if (Match[Site.Target] == NoIndex)
      return {};
    H.add("mapped").add(Secondary ? Site.Target : Match[Site.Target]);
  }
  bool Unknown = false;
  for (const auto &Site : F.DataSites) {
    if (Control.stop())
      return {};
    H.add(Site.Instruction).add(Site.Operand).add(unsigned(Site.Kind));
    if (Site.Kind != ReferenceKind::UnresolvedData) {
      H.add(Site.Key);
      continue;
    }
    Unknown = true;
    if (Mode == ContextMode::DataWitness)
      continue;
    // A call-role disagreement must not erase unknown data differences.
    if (Mode == ContextMode::CallWitness)
      return {};
    const auto It = Forward->find(Site.Key);
    if (It == Forward->end() || It->second.Conflict)
      return {};
    const auto Back = Reverse->find(It->second.Other);
    if (Back == Reverse->end() || Back->second.Conflict ||
        Back->second.Other != Site.Key)
      return {};
    H.add(Secondary ? Site.Key : It->second.Other);
  }
  const bool Eligible = Mode == ContextMode::CallWitness       ? ProvisionalCall
                        : Mode == ContextMode::FunctionWitness ? FunctionOperand
                                                               : Unknown;
  return Eligible ? std::optional<uint64_t>(H.value()) : std::nullopt;
}

} // namespace

ReferenceCandidates
referenceCandidates(const Program &P, const ProgramFeatures &PF,
                    llvm::ArrayRef<uint32_t> PMatch, const Program &S,
                    const ProgramFeatures &SF, llvm::ArrayRef<uint32_t> SMatch,
                    const ComparisonControl &Control) {
  // This normalization records complete x86 address operands. In particular,
  // AArch64 ADRP page fragments are not object-reference witnesses.
  const bool DataOperands =
      P.Identity.Normalization == NativeX86Normalization &&
      S.Identity.Normalization == NativeX86Normalization;
  const size_t PrimaryCount = P.Functions.size();
  const size_t Total = PrimaryCount + S.Functions.size();
  std::vector<std::optional<uint64_t>> Keys(Total);
  const auto Compute = [&](ContextMode Mode, const Relations *Forward,
                           const Relations *Reverse) {
    parallelForEach(Total, [&](auto Claim, size_t Count) {
      for (size_t I = Claim(); I < Count; I = Claim()) {
        if (Control.stop())
          return;
        const bool Secondary = I >= PrimaryCount;
        const auto Map = Secondary ? SMatch : PMatch;
        const uint32_t F = Secondary ? I - PrimaryCount : I;
        Keys[I].reset();
        // First inspect the settled anchors, then only remaining functions.
        if ((Map[F] == NoIndex) != (Mode == ContextMode::DataCandidate) ||
            (!DataOperands && Mode != ContextMode::FunctionWitness))
          continue;
        Keys[I] = referenceKey(Secondary ? S : P, Secondary ? SF : PF, F, Map,
                               Secondary, Mode, Secondary ? Reverse : Forward,
                               Secondary ? Forward : Reverse, Control);
      }
    });
  };
  Compute(ContextMode::DataWitness, nullptr, nullptr);
  if (Control.stop())
    return {};
  Relations Forward, Reverse;
  // Merge occurrence observations once, outside per-function work. No
  // Cartesian product of functions sharing a popular global is materialized.
  for (uint32_t A = 0; A < PrimaryCount; ++A) {
    if (Control.stop())
      return {};
    const uint32_t B = PMatch[A];
    if (B == NoIndex || !Keys[A] || Keys[A] != Keys[PrimaryCount + B])
      continue;
    const auto &ASites = PF.Functions[A].DataSites;
    const auto &BSites = SF.Functions[B].DataSites;
    if (ASites.size() != BSites.size())
      continue;
    for (size_t I = 0; I < ASites.size(); ++I) {
      if (Control.stop())
        return {};
      if (ASites[I].Kind != ReferenceKind::UnresolvedData)
        continue;
      recordRelation(Forward, ASites[I].Key, BSites[I].Key);
      recordRelation(Reverse, BSites[I].Key, ASites[I].Key);
    }
  }
  ReferenceCandidates Candidates;
  // Address-taking operands are separate from calls. Their aligned roles can
  // propose callbacks even when several bodies remain indistinguishable.
  // Already mapped targets must agree; unknown data needs the frozen relation
  // above before it can support this second kind of witness.
  Compute(ContextMode::FunctionWitness, &Forward, &Reverse);
  if (Control.stop())
    return {};
  Relations FunctionForward, FunctionReverse;
  for (uint32_t A = 0; A < PrimaryCount; ++A) {
    if (Control.stop())
      return {};
    const uint32_t B = PMatch[A];
    if (B == NoIndex || !Keys[A] || Keys[A] != Keys[PrimaryCount + B])
      continue;
    const auto &ASites = PF.Functions[A].AddressSites;
    const auto &BSites = SF.Functions[B].AddressSites;
    if (ASites.size() != BSites.size())
      continue;
    for (size_t I = 0; I < ASites.size(); ++I) {
      if (Control.stop())
        return {};
      if (ASites[I].Operand == NoIndex)
        continue;
      recordRelation(FunctionForward, ASites[I].Target, BSites[I].Target);
      recordRelation(FunctionReverse, BSites[I].Target, ASites[I].Target);
    }
  }
  for (uint32_t A = 0; A < PrimaryCount; ++A) {
    if (Control.stop())
      return {};
    const auto It = FunctionForward.find(A);
    if (PMatch[A] != NoIndex || It == FunctionForward.end() ||
        It->second.Conflict)
      continue;
    const uint32_t B = It->second.Other;
    const auto Back = FunctionReverse.find(B);
    if (SMatch[B] == NoIndex && Back != FunctionReverse.end() &&
        !Back->second.Conflict && Back->second.Other == A &&
        P.Functions[A].hasBody() && S.Functions[B].hasBody())
      Candidates.Functions.emplace_back(A, B);
  }
  if (Forward.empty())
    return Candidates;
  Compute(ContextMode::DataCandidate, &Forward, &Reverse);
  if (Control.stop())
    return {};
  struct Entry {
    uint64_t Key;
    size_t Index;
  };
  std::vector<Entry> Entries;
  for (size_t I = 0; I < Keys.size(); ++I)
    if (Keys[I])
      Entries.push_back({*Keys[I], I});
  llvm::sort(Entries, [](const Entry &A, const Entry &B) {
    return std::tie(A.Key, A.Index) < std::tie(B.Key, B.Index);
  });
  for (size_t I = 0; I < Entries.size();) {
    if (Control.stop())
      return {};
    size_t J = I + 1;
    while (J < Entries.size() && Entries[J].Key == Entries[I].Key)
      ++J;
    if (J - I == 2 && Entries[I].Index < PrimaryCount &&
        Entries[I + 1].Index >= PrimaryCount)
      Candidates.Data.emplace_back(Entries[I].Index,
                                   Entries[I + 1].Index - PrimaryCount);
    I = J;
  }
  llvm::sort(Candidates.Data);
  return Candidates;
}

InitializerCandidates initializerCandidates(const Program &P,
                                            llvm::ArrayRef<uint32_t> PMatch,
                                            llvm::ArrayRef<uint8_t> PTrusted,
                                            const Program &S,
                                            llvm::ArrayRef<uint32_t> SMatch,
                                            llvm::ArrayRef<uint8_t> STrusted,
                                            const ComparisonControl &Control) {
  struct Run {
    size_t Begin, End;
    bool Secondary;
    uint64_t Stride;
    uint64_t Key = 0;
  };
  std::vector<Run> Runs;
  std::vector<uint32_t> Targets[2];
  for (bool Secondary : {false, true}) {
    const auto &Program = Secondary ? S : P;
    if (Program.Identity.Bits != 32 && Program.Identity.Bits != 64)
      continue;
    const uint64_t Width = Program.Identity.Bits / CHAR_BIT;
    const auto &Slots = Program.CodeInitializers;
    Targets[Secondary].assign(Slots.size(), NoIndex);
    parallelForEach(Slots.size(), [&](auto Claim, size_t Count) {
      for (size_t I = Claim(); I < Count; I = Claim()) {
        if (Control.stop())
          return;
        if (Slots[I].Target)
          Targets[Secondary][I] =
              Program.functionAt(*Slots[I].Target).value_or(NoIndex);
      }
    });
    if (Control.stop())
      return {};
    const auto Add = [&](size_t I, size_t J, uint64_t Stride) {
      if (J - I > limits::diff::kMinInitializerAnchors &&
          !(I && Slots[I].Slot - Slots[I - 1].Slot < Width) &&
          !(J < Slots.size() && Slots[J].Slot - Slots[J - 1].Slot < Width))
        Runs.push_back({I, J, Secondary, Stride});
    };
    for (size_t I = 0; I < Slots.size();) {
      if (Control.stop())
        return {};
      if (Targets[Secondary][I] == NoIndex) {
        ++I;
        continue;
      }
      size_t J = I + 1;
      while (J < Slots.size() && Targets[Secondary][J] != NoIndex &&
             Slots[J].Slot - Slots[J - 1].Slot == Width) {
        if (Control.stop())
          return {};
        ++J;
      }
      Add(I, J, Width);
      I = J;
    }
    // Keep contiguous neighborhoods intact. A wider repeated spacing is a
    // separate, weaker source; unknown coordinates are explicit barriers.
    for (size_t I = 0; I + 1 < Slots.size();) {
      if (Control.stop())
        return {};
      const uint64_t Stride = Slots[I + 1].Slot - Slots[I].Slot;
      if (Stride <= Width || Targets[Secondary][I] == NoIndex ||
          Targets[Secondary][I + 1] == NoIndex) {
        ++I;
        continue;
      }
      size_t J = I + 2;
      while (J < Slots.size() && Targets[Secondary][J] != NoIndex &&
             Slots[J].Slot - Slots[J - 1].Slot == Stride) {
        if (Control.stop())
          return {};
        ++J;
      }
      Add(I, J, Stride);
      I = J - 1;
    }
  }
  parallelForEach(Runs.size(), [&](auto Claim, size_t Count) {
    for (size_t R = Claim(); R < Count; R = Claim()) {
      auto &Run = Runs[R];
      const auto &Program = Run.Secondary ? S : P;
      const auto Map = Run.Secondary ? SMatch : PMatch;
      const auto Trusted = Run.Secondary ? STrusted : PTrusted;
      StableHash H;
      H.add(Program.Identity.Bits).add(Run.Stride).add(Run.End - Run.Begin);
      llvm::SmallVector<uint32_t, limits::diff::kMinInitializerAnchors> Known;
      bool Unknown = false;
      for (size_t I = Run.Begin; I < Run.End; ++I) {
        if (Control.stop())
          return;
        const uint32_t F = Targets[Run.Secondary][I];
        if (Trusted[F] && Map[F] != NoIndex) {
          const uint32_t Id = Run.Secondary ? F : Map[F];
          H.add("known").add(Id);
          if (Known.size() < limits::diff::kMinInitializerAnchors &&
              !llvm::is_contained(Known, Id))
            Known.push_back(Id);
        } else {
          H.add("unmapped");
          Unknown |= Map[F] == NoIndex;
        }
      }
      if (Unknown && Known.size() >= limits::diff::kMinInitializerAnchors)
        Run.Key = H.value();
    }
  });
  if (Control.stop())
    return {};
  llvm::erase_if(Runs, [](const auto &R) { return !R.Key; });
  llvm::sort(Runs, [](const Run &A, const Run &B) {
    return std::tie(A.Key, A.Secondary, A.Begin) <
           std::tie(B.Key, B.Secondary, B.Begin);
  });
  Relations Forward[2], Reverse[2];
  for (size_t I = 0; I < Runs.size();) {
    if (Control.stop())
      return {};
    size_t J = I + 1;
    while (J < Runs.size() && Runs[J].Key == Runs[I].Key)
      ++J;
    if (J - I == 2 && !Runs[I].Secondary && Runs[I + 1].Secondary) {
      const auto &A = Runs[I], &B = Runs[I + 1];
      bool Compatible = A.End - A.Begin == B.End - B.Begin &&
                        A.Stride == B.Stride &&
                        P.Identity.Bits == S.Identity.Bits;
      const bool Strided = A.Stride > P.Identity.Bits / CHAR_BIT;
      for (size_t N = 0; Compatible && N < A.End - A.Begin; ++N) {
        if (Control.stop())
          return {};
        const uint32_t X = Targets[0][A.Begin + N], Y = Targets[1][B.Begin + N];
        if (PTrusted[X] || STrusted[Y])
          Compatible = PTrusted[X] && STrusted[Y] && PMatch[X] == Y;
      }
      if (Compatible)
        for (size_t N = 0; N < A.End - A.Begin; ++N) {
          if (Control.stop())
            return {};
          const uint32_t X = Targets[0][A.Begin + N],
                         Y = Targets[1][B.Begin + N];
          if (!PTrusted[X] && !STrusted[Y]) {
            recordRelation(Forward[Strided], X, Y);
            recordRelation(Reverse[Strided], Y, X);
          }
        }
    }
    I = J;
  }
  InitializerCandidates Result;
  for (bool Strided : {false, true}) {
    auto &Candidates = Strided ? Result.Strided : Result.Contiguous;
    for (const auto &[A, Relation] : Forward[Strided]) {
      if (Control.stop())
        return {};
      const uint32_t B = Relation.Other;
      const auto Back = Reverse[Strided].find(B);
      if (Relation.Conflict || Back == Reverse[Strided].end() ||
          Back->second.Conflict || Back->second.Other != A ||
          PMatch[A] != NoIndex || SMatch[B] != NoIndex ||
          !P.Functions[A].hasBody() || !S.Functions[B].hasBody() ||
          P.Functions[A].has(TraitIncomplete) ||
          S.Functions[B].has(TraitIncomplete))
        continue;
      Candidates.emplace_back(A, B);
    }
    llvm::sort(Candidates);
  }
  return Result;
}

std::vector<std::pair<uint32_t, uint32_t>> conflictingCallCandidates(
    const Program &P, const ProgramFeatures &PF,
    llvm::ArrayRef<uint32_t> PMatch, llvm::ArrayRef<uint32_t> WeakP,
    const Program &S, const ProgramFeatures &SF,
    llvm::ArrayRef<uint32_t> SMatch, llvm::ArrayRef<uint32_t> WeakS,
    const ComparisonControl &Control) {
  const size_t PrimaryCount = P.Functions.size();
  const size_t Total = PrimaryCount + S.Functions.size();
  std::vector<std::optional<uint64_t>> Keys(Total);
  parallelForEach(Total, [&](auto Claim, size_t Count) {
    for (size_t I = Claim(); I < Count; I = Claim()) {
      if (Control.stop())
        return;
      const bool Secondary = I >= PrimaryCount;
      const auto Map = Secondary ? SMatch : PMatch;
      const uint32_t F = Secondary ? I - PrimaryCount : I;
      if (Map[F] != NoIndex)
        Keys[I] = referenceKey(Secondary ? S : P, Secondary ? SF : PF, F, Map,
                               Secondary, ContextMode::CallWitness, nullptr,
                               nullptr, Control, Secondary ? WeakS : WeakP);
    }
  });
  // Merge aligned occurrences once. Even a caller agreeing with the current
  // body match contributes: contradictory observations cannot choose a winner.
  Relations Forward, Reverse;
  for (uint32_t A = 0; A < PrimaryCount; ++A) {
    if (Control.stop())
      return {};
    const uint32_t B = PMatch[A];
    if (B == NoIndex || !Keys[A] || Keys[A] != Keys[PrimaryCount + B])
      continue;
    const auto &ASites = PF.Functions[A].AddressSites;
    const auto &BSites = SF.Functions[B].AddressSites;
    if (ASites.size() != BSites.size())
      continue;
    for (size_t I = 0; I < ASites.size(); ++I) {
      if (Control.stop())
        return {};
      if (ASites[I].Operand != NoIndex || ASites[I].Target == A)
        continue;
      recordRelation(Forward, ASites[I].Target, BSites[I].Target);
      recordRelation(Reverse, BSites[I].Target, ASites[I].Target);
    }
  }
  std::vector<std::pair<uint32_t, uint32_t>> Candidates;
  for (uint32_t A = 0; A < PrimaryCount; ++A) {
    if (Control.stop())
      return {};
    const auto It = Forward.find(A);
    if (It == Forward.end() || It->second.Conflict)
      continue;
    const uint32_t B = It->second.Other;
    const auto Back = Reverse.find(B);
    if (Back == Reverse.end() || Back->second.Conflict ||
        Back->second.Other != A)
      continue;
    // Only reopen a weak match against one still-unmatched endpoint. Stable
    // identities and two already paired endpoints cannot be displaced here.
    if (((PMatch[A] == NoIndex && WeakS[B] != NoIndex) ||
         (SMatch[B] == NoIndex && WeakP[A] != NoIndex)) &&
        P.Functions[A].hasBody() && S.Functions[B].hasBody())
      Candidates.emplace_back(A, B);
  }
  return Candidates;
}

} // namespace neverd::diff
