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

#include <optional>
#include <tuple>

namespace neverd::diff {
namespace {

struct Relation {
  uint64_t Other;
  bool Conflict = false;
};
using Relations = llvm::DenseMap<uint64_t, Relation>;

enum class ContextMode { DataWitness, DataCandidate, FunctionWitness };

/// A complete occurrence signature. Before relations exist, unknown data keys
/// are omitted only to align anchors that already correspond independently.
std::optional<uint64_t>
referenceKey(const Program &P, const ProgramFeatures &PF, uint32_t FI,
             llvm::ArrayRef<uint32_t> Match, bool Secondary, ContextMode Mode,
             const Relations *Forward, const Relations *Reverse,
             const ComparisonControl &Control) {
  const auto &F = PF.Functions[FI];
  if (F.Instructions < limits::diff::kMinDataReferenceInstructions ||
      P.Functions[FI].has(TraitIncomplete) || !F.DataSitesComplete ||
      F.UnresolvedCallee || PF.Referents.of(FI).size() != F.FunctionReferences)
    return {};
  StableHash H(F.ReferenceLayout);
  H.add(F.AddressSites.size()).add(F.DataSites.size());
  bool FunctionOperand = false;
  for (const auto &Site : F.AddressSites) {
    if (Control.stop() || Site.Target == NoIndex)
      return {};
    H.add(Site.Instruction).add(Site.Operand);
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
    const auto It = Forward->find(Site.Key);
    if (It == Forward->end() || It->second.Conflict)
      return {};
    const auto Back = Reverse->find(It->second.Other);
    if (Back == Reverse->end() || Back->second.Conflict ||
        Back->second.Other != Site.Key)
      return {};
    H.add(Secondary ? Site.Key : It->second.Other);
  }
  const bool Eligible =
      Mode == ContextMode::FunctionWitness ? FunctionOperand : Unknown;
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
  const auto Record = [](Relations &Map, uint64_t A, uint64_t B) {
    auto [It, Inserted] = Map.try_emplace(A, Relation{B});
    if (!Inserted && It->second.Other != B)
      It->second.Conflict = true;
  };
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
      Record(Forward, ASites[I].Key, BSites[I].Key);
      Record(Reverse, BSites[I].Key, ASites[I].Key);
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
      Record(FunctionForward, ASites[I].Target, BSites[I].Target);
      Record(FunctionReverse, BSites[I].Target, ASites[I].Target);
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

} // namespace neverd::diff
