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
/// Distinct mapped entry calls also expose possible caller-inline boundaries;
/// the comparison owner measures those proposals before deferring identity.
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

#include <algorithm>
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
  DataCoordinateWitness,
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
    if (Mode == ContextMode::DataCoordinateWitness && !Site.Coordinate)
      return {};
    H.add(Site.Instruction).add(Site.Operand).add(unsigned(Site.Kind));
    if (Site.Kind != ReferenceKind::UnresolvedData) {
      H.add(Site.Key);
      continue;
    }
    Unknown = true;
    if (Mode == ContextMode::DataWitness ||
        Mode == ContextMode::DataCoordinateWitness)
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
  const bool Eligible = Mode == ContextMode::DataCoordinateWitness
                            ? !F.DataSites.empty()
                        : Mode == ContextMode::CallWitness     ? ProvisionalCall
                        : Mode == ContextMode::FunctionWitness ? FunctionOperand
                                                               : Unknown;
  return Eligible ? std::optional<uint64_t>(H.value()) : std::nullopt;
}

/// Hash complete witness layouts in parallel. The ordered merge and disjoint
/// address intervals visit each observed slot once; no function-pair scan is
/// needed. Conflicting coordinates remain in the ordering as barriers.
std::vector<std::pair<uint32_t, uint32_t>> dataInitializerCandidates(
    const Program &P, const ProgramFeatures &PF,
    llvm::ArrayRef<uint32_t> PMatch, llvm::ArrayRef<uint8_t> PTrusted,
    const Program &S, const ProgramFeatures &SF,
    llvm::ArrayRef<uint32_t> SMatch, llvm::ArrayRef<uint8_t> STrusted,
    llvm::ArrayRef<uint32_t> PTargets, llvm::ArrayRef<uint32_t> STargets,
    const ComparisonControl &Control) {
  if (P.Identity.Bits != S.Identity.Bits || PTargets.empty() ||
      STargets.empty())
    return {};
  std::vector<uint8_t> Witnesses(P.Functions.size(), 0);
  parallelForEach(Witnesses.size(), [&](auto Claim, size_t Count) {
    for (size_t A = Claim(); A < Count; A = Claim()) {
      if (Control.stop())
        return;
      const uint32_t B = PMatch[A];
      if (!PTrusted[A] || B == NoIndex || !STrusted[B])
        continue;
      const auto X = referenceKey(P, PF, A, PMatch, false,
                                  ContextMode::DataCoordinateWitness, nullptr,
                                  nullptr, Control);
      if (X)
        Witnesses[A] = X == referenceKey(S, SF, B, SMatch, true,
                                         ContextMode::DataCoordinateWitness,
                                         nullptr, nullptr, Control);
    }
  });
  Relations Forward, Reverse;
  for (uint32_t A = 0; A < Witnesses.size(); ++A) {
    if (Control.stop())
      return {};
    if (!Witnesses[A])
      continue;
    const auto &X = PF.Functions[A].DataSites;
    const auto &Y = SF.Functions[PMatch[A]].DataSites;
    if (X.size() != Y.size())
      continue;
    if (!llvm::all_of(llvm::zip(X, Y), [](const auto &Sites) {
          const auto &[L, R] = Sites;
          return L.Instruction == R.Instruction && L.Operand == R.Operand &&
                 L.Kind == R.Kind;
        }))
      continue;
    for (size_t I = 0; I < X.size(); ++I) {
      recordRelation(Forward, *X[I].Coordinate, *Y[I].Coordinate);
      recordRelation(Reverse, *Y[I].Coordinate, *X[I].Coordinate);
    }
  }
  std::vector<uint64_t> Primary, Secondary;
  Primary.reserve(Forward.size());
  Secondary.reserve(Reverse.size());
  for (const auto &[A, Relation] : Forward)
    Primary.push_back(A);
  for (const auto &[B, Relation] : Reverse)
    Secondary.push_back(B);
  llvm::sort(Primary);
  llvm::sort(Secondary);
  const auto Unique = [&](uint64_t A) -> std::optional<uint64_t> {
    const auto &R = Forward.find(A)->second;
    const auto Back = Reverse.find(R.Other);
    if (R.Conflict || Back == Reverse.end() || Back->second.Conflict ||
        Back->second.Other != A)
      return {};
    return R.Other;
  };
  const auto Range = [](const Program &Image, uint64_t Begin, uint64_t End) {
    const auto &Slots = Image.CodeInitializers;
    const auto Find = [&](uint64_t Address) {
      return std::lower_bound(Slots.begin(), Slots.end(), Address,
                              [](const auto &Slot, uint64_t Value) {
                                return Slot.Slot < Value;
                              }) -
             Slots.begin();
    };
    return std::pair<size_t, size_t>(Find(Begin), Find(End));
  };
  Relations Proposed, ProposedBack;
  const uint64_t Width = P.Identity.Bits / CHAR_BIT;
  for (size_t I = 1; I < Primary.size(); ++I) {
    if (Control.stop())
      return {};
    const uint64_t Begin = Primary[I - 1], End = Primary[I];
    const auto Other = Unique(Begin), OtherEnd = Unique(End);
    if (!Other || !OtherEnd || *OtherEnd <= *Other ||
        *OtherEnd - *Other != End - Begin)
      continue;
    const auto Position = llvm::lower_bound(Secondary, *Other);
    if (Position + 1 == Secondary.end() || Position[1] != *OtherEnd)
      continue;
    const auto [A, AE] = Range(P, Begin, End);
    const auto [B, BE] = Range(S, *Other, *OtherEnd);
    if (A == AE || AE - A != BE - B)
      continue;
    bool Compatible = true;
    for (size_t N = 0; Compatible && N < AE - A; ++N) {
      if (Control.stop())
        return {};
      const uint32_t X = PTargets[A + N], Y = STargets[B + N];
      const auto &L = P.CodeInitializers[A + N];
      const auto &R = S.CodeInitializers[B + N];
      Compatible =
          X != NoIndex && Y != NoIndex && L.Slot - Begin == R.Slot - *Other &&
          Width <= End - L.Slot && Width <= *OtherEnd - R.Slot &&
          !(A + N && L.Slot - P.CodeInitializers[A + N - 1].Slot < Width) &&
          !(B + N && R.Slot - S.CodeInitializers[B + N - 1].Slot < Width);
      if (Compatible && (PMatch[X] != NoIndex || SMatch[Y] != NoIndex))
        Compatible = PMatch[X] == Y && SMatch[Y] == X;
    }
    if (!Compatible)
      continue;
    for (size_t N = 0; N < AE - A; ++N) {
      if (Control.stop())
        return {};
      const uint32_t X = PTargets[A + N], Y = STargets[B + N];
      if (PMatch[X] == NoIndex && SMatch[Y] == NoIndex) {
        recordRelation(Proposed, X, Y);
        recordRelation(ProposedBack, Y, X);
      }
    }
  }
  std::vector<std::pair<uint32_t, uint32_t>> Result;
  for (const auto &[A, Relation] : Proposed) {
    if (Control.stop())
      return {};
    const uint32_t B = Relation.Other;
    const auto Back = ProposedBack.find(B);
    if (!Relation.Conflict && Back != ProposedBack.end() &&
        !Back->second.Conflict && Back->second.Other == A &&
        P.Functions[A].hasBody() && S.Functions[B].hasBody() &&
        !P.Functions[A].has(TraitIncomplete) &&
        !S.Functions[B].has(TraitIncomplete))
      Result.emplace_back(A, B);
  }
  llvm::sort(Result);
  return Result;
}

} // namespace

std::vector<std::pair<uint32_t, uint32_t>>
blockCallCandidates(const Program &P, llvm::ArrayRef<uint32_t> PMatch,
                    llvm::ArrayRef<uint8_t> PTrusted, const Program &S,
                    llvm::ArrayRef<uint32_t> SMatch,
                    llvm::ArrayRef<uint8_t> STrusted,
                    const ComparisonControl &Control) {
  struct BlockRole {
    uint64_t Key;
    bool Secondary;
    std::vector<uint32_t> Calls;
  };
  // Each worker owns one caller's observations. The ordered merge below
  // checks conflicts across callers before any proposal can be admitted.
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> Observations(
      P.Functions.size());
  parallelForEach(Observations.size(), [&](auto Claim, size_t Count) {
    for (size_t A = Claim(); A < Count; A = Claim()) {
      if (Control.stop())
        return;
      const uint32_t B = PMatch[A];
      if (!PTrusted[A] || B == NoIndex || !STrusted[B] ||
          P.Functions[A].has(TraitIncomplete) ||
          S.Functions[B].has(TraitIncomplete))
        continue;
      std::vector<BlockRole> Roles;
      for (bool Secondary : {false, true}) {
        const auto &Image = Secondary ? S : P;
        const auto Map = Secondary ? SMatch : PMatch;
        const uint32_t FI = Secondary ? B : A;
        const auto &F = Image.Functions[FI];
        for (uint32_t BI = 0; BI < F.BlockCount; ++BI) {
          if (Control.stop())
            return;
          const auto Instructions = Image.instructions(Image.blocks(F)[BI]);
          if (Instructions.size() < limits::diff::kMinBlockHashInstructions)
            continue;
          StableHash H;
          H.add(Instructions.size()).add(BI == F.EntryBlock);
          std::vector<uint32_t> Calls;
          bool Complete = true, Provisional = false;
          for (const auto &I : Instructions) {
            if (Control.stop())
              return;
            if (I.HasUnresolvedData || I.UnresolvedCallee ||
                I.Flow == InstructionFlow::Unknown ||
                I.Flow == InstructionFlow::IndirectCall ||
                I.Flow == InstructionFlow::IndirectJump) {
              Complete = false;
              break;
            }
            H.add(I.AddressIndependentShape).add(unsigned(I.Flow));
            for (const auto &Address : Image.addressReferences(I)) {
              H.add(Address.Operand).add(unsigned(Address.Kind));
              if (Address.Kind == ReferenceKind::UnresolvedData) {
                Complete = false;
                break;
              }
              if (Address.Kind != ReferenceKind::Function) {
                H.add(Address.Key);
                continue;
              }
              const auto Target = Image.functionAt(Address.Key);
              if (!Target || (*Target != FI && Map[*Target] == NoIndex)) {
                Complete = false;
                break;
              }
              if (*Target == FI)
                H.add("self");
              else
                H.add("mapped").add(Secondary ? *Target : Map[*Target]);
            }
            if (!Complete)
              break;
            if (I.Callee == NoIndex)
              continue;
            if (I.Flow == InstructionFlow::Call)
              Calls.push_back(I.Callee);
            if (I.Callee == FI) {
              H.add("self");
            } else if (Map[I.Callee] != NoIndex) {
              H.add("mapped").add(Secondary ? I.Callee : Map[I.Callee]);
            } else if (I.Flow == InstructionFlow::Call) {
              H.add("provisional_call");
              Provisional = true;
            } else {
              Complete = false;
              break;
            }
          }
          if (Complete && Provisional)
            Roles.push_back({H.value(), Secondary, std::move(Calls)});
        }
      }
      llvm::stable_sort(Roles, [](const auto &L, const auto &R) {
        return std::tie(L.Key, L.Secondary) < std::tie(R.Key, R.Secondary);
      });
      for (size_t I = 0; I < Roles.size();) {
        if (Control.stop())
          return;
        size_t J = I + 1;
        while (J < Roles.size() && Roles[J].Key == Roles[I].Key)
          ++J;
        if (J - I == 2 && !Roles[I].Secondary && Roles[I + 1].Secondary) {
          const auto &X = Roles[I].Calls, &Y = Roles[I + 1].Calls;
          if (X.size() == Y.size())
            for (size_t N = 0; N < X.size(); ++N)
              if (PMatch[X[N]] == NoIndex && SMatch[Y[N]] == NoIndex)
                Observations[A].emplace_back(X[N], Y[N]);
        }
        I = J;
      }
    }
  });
  Relations Forward, Reverse;
  for (const auto &Caller : Observations)
    for (const auto &[A, B] : Caller) {
      if (Control.stop())
        return {};
      recordRelation(Forward, A, B);
      recordRelation(Reverse, B, A);
    }
  std::vector<std::pair<uint32_t, uint32_t>> Result;
  for (const auto &[A, Relation] : Forward) {
    if (Control.stop())
      return {};
    const uint32_t B = Relation.Other;
    const auto Back = Reverse.find(B);
    if (!Relation.Conflict && Back != Reverse.end() && !Back->second.Conflict &&
        Back->second.Other == A && P.Functions[A].hasBody() &&
        S.Functions[B].hasBody() && !P.Functions[A].has(TraitIncomplete) &&
        !S.Functions[B].has(TraitIncomplete))
      Result.emplace_back(A, B);
  }
  llvm::sort(Result);
  return Result;
}

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

InitializerCandidates initializerCandidates(
    const Program &P, const ProgramFeatures &PF,
    llvm::ArrayRef<uint32_t> PMatch, llvm::ArrayRef<uint8_t> PTrusted,
    const Program &S, const ProgramFeatures &SF,
    llvm::ArrayRef<uint32_t> SMatch, llvm::ArrayRef<uint8_t> STrusted,
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
  Result.DataAddresses =
      dataInitializerCandidates(P, PF, PMatch, PTrusted, S, SF, SMatch,
                                STrusted, Targets[0], Targets[1], Control);
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

std::vector<std::pair<uint32_t, uint32_t>> inlineCallerCandidates(
    const Program &P, const ProgramFeatures &PF,
    llvm::ArrayRef<uint32_t> PMatch, llvm::ArrayRef<uint32_t> WeakP,
    const Program &S, const ProgramFeatures &SF,
    llvm::ArrayRef<uint32_t> SMatch, const ComparisonControl &Control) {
  // Entry calls use the frozen correspondence outside the possible inline
  // pair. This is ambiguity evidence, not a new function-identity key.
  const size_t PrimaryCount = P.Functions.size();
  struct EntryCalls {
    uint64_t Key = 0;
    llvm::SmallVector<uint32_t, limits::diff::kMinInlineCallerAnchors> Targets;
  };
  std::vector<EntryCalls> Entries(PrimaryCount + S.Functions.size());
  parallelForEach(Entries.size(), [&](auto Claim, size_t Count) {
    for (size_t I = Claim(); I < Count; I = Claim()) {
      if (Control.stop())
        return;
      const bool Secondary = I >= PrimaryCount;
      const auto &Image = Secondary ? S : P, &Other = Secondary ? P : S;
      const auto &Map = Secondary ? SMatch : PMatch;
      const uint32_t FI = Secondary ? I - PrimaryCount : I;
      const auto &F = Image.Functions[FI];
      if (!F.hasBody() || F.has(TraitIncomplete) || F.EntryBlock == NoIndex)
        continue;
      llvm::SmallVector<uint32_t, limits::diff::kMinInlineCallerAnchors> Calls;
      bool Complete = true;
      for (const auto &Op : Image.instructions(Image.blocks(F)[F.EntryBlock])) {
        if (Control.stop())
          return;
        if (Op.Flow == InstructionFlow::Unknown ||
            Op.Flow == InstructionFlow::IndirectCall ||
            Op.Flow == InstructionFlow::IndirectJump ||
            (Op.Callee != NoIndex && Op.Flow != InstructionFlow::Call)) {
          Complete = false;
          break;
        }
        if (Op.Flow != InstructionFlow::Call)
          continue;
        if (Op.UnresolvedCallee || Op.Callee == NoIndex || Op.Callee == FI ||
            Map[Op.Callee] == NoIndex ||
            Image.Functions[Op.Callee].has(TraitIncomplete) ||
            Other.Functions[Map[Op.Callee]].has(TraitIncomplete)) {
          Complete = false;
          break;
        }
        Calls.push_back(Secondary ? Op.Callee : Map[Op.Callee]);
      }
      if (!Complete || Calls.size() < limits::diff::kMinInlineCallerAnchors)
        continue;
      auto Distinct = Calls;
      llvm::sort(Distinct);
      Distinct.erase(std::unique(Distinct.begin(), Distinct.end()),
                     Distinct.end());
      if (Distinct.size() < limits::diff::kMinInlineCallerAnchors)
        continue;
      StableHash H;
      H.add(Calls.size());
      for (uint32_t Target : Calls)
        H.add(Target);
      Entries[I] = {H.value(), std::move(Distinct)};
    }
  });
  if (Control.stop())
    return {};

  std::vector<std::pair<uint32_t, uint32_t>> Result;
  for (uint32_t A = 0; A < WeakP.size(); ++A) {
    if (Control.stop())
      return {};
    if (WeakP[A] == NoIndex)
      continue;
    const uint32_t B = PMatch[A];
    for (bool Secondary : {false, true}) {
      const auto &Features = Secondary ? SF : PF;
      const auto &Image = Secondary ? S : P, &Other = Secondary ? P : S;
      const auto Map = Secondary ? SMatch : PMatch;
      const uint32_t Helper = Secondary ? B : A, Merged = Secondary ? A : B;
      const auto &Entry = Entries[Secondary ? Merged : PrimaryCount + Merged];
      if (!Entry.Key || Image.Functions[Helper].has(TraitIncomplete) ||
          Other.Functions[Merged].has(TraitIncomplete))
        continue;
      const auto Callees = Features.Callees.of(Helper);
      if (llvm::any_of(Entry.Targets, [&](uint32_t Target) {
            return std::binary_search(Callees.begin(), Callees.end(),
                                      Secondary ? Target : SMatch[Target]);
          }))
        continue; // The helper already explains one of these entry anchors.
      // Each helper's caller adjacency is visited once. Shared initialization
      // never creates a cross-product of otherwise unrelated functions.
      for (uint32_t Caller : Features.Callers.of(Helper))
        if (Map[Caller] == NoIndex &&
            Entries[Secondary ? PrimaryCount + Caller : Caller].Key ==
                Entry.Key)
          Result.emplace_back(Secondary ? Merged : Caller,
                              Secondary ? Caller : Merged);
    }
  }
  llvm::sort(Result);
  Result.erase(std::unique(Result.begin(), Result.end()), Result.end());
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
