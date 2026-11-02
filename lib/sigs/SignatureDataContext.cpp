//===- SignatureDataContext.cpp - Frozen external-data context -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Derives data bindings from independent complete ordinary candidates. New
/// data-dependent candidates never change their anchors or establish bindings.
///
//===----------------------------------------------------------------------===//

#include "DataReference.h"
#include "LocalDataReference.h"
#include "X86/X86DataReference.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sigs/SignatureDB.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neverd::sigs {

std::optional<uint64_t> localDataReferenceBase(const BinaryImage &Img,
                                               uint64_t Target,
                                               const LocalDataAnchor &Anchor) {
  const int64_t Delta = Anchor.Delta;
  if ((Delta > 0 && Target < uint64_t(Delta)) ||
      (Delta < 0 && Target > InvalidVA - uint64_t(-Delta)))
    return std::nullopt;
  const uint64_t Base = Target - Delta;
  auto SameRegion = [&](const auto &Regions) {
    return llvm::any_of(Regions, [&](const auto &Region) {
      return Base >= Region.VA && Base - Region.VA < Region.Size &&
             Target >= Region.VA && Target - Region.VA < Region.Size;
    });
  };
  if (!SameRegion(Img.Segments) ||
      (!Img.Sections.empty() && !SameRegion(Img.Sections)))
    return std::nullopt;
  return Base;
}

std::optional<uint64_t> dataReferenceTarget(const BinaryImage &Img,
                                            uint64_t Start, uint32_t Offset,
                                            uint32_t Length) {
  if (!Img.isCOFF() || Img.Arch != Arch::X64 || Img.IsRelocatable)
    return std::nullopt;
  if (Offset > Length || Length - Offset < sizeof(uint32_t))
    return std::nullopt;
  // Restrict the decode to this complete source body, including an immediate.
  const size_t Available = static_cast<size_t>(
      std::min<uint64_t>(Length, uint64_t(Offset) + 2 * sizeof(uint32_t)));
  if (Start > InvalidVA - Available)
    return std::nullopt;
  const uint8_t *Bytes = Img.readVA(Start, Available);
  if (!Bytes)
    return std::nullopt;
  const auto Field = x64DataReferenceField({Bytes, Available}, Offset);
  if (!Field)
    return std::nullopt;
  const uint64_t Next = Start + Field->End;
  const int64_t Displacement = readLE<int32_t>(Bytes + Offset);
  if ((Displacement < 0 && Next < uint64_t(-Displacement)) ||
      (Displacement >= 0 && Next > InvalidVA - uint64_t(Displacement)))
    return std::nullopt;
  const uint64_t Target = Next + Displacement;
  if (Target > InvalidVA - Field->Width)
    return std::nullopt;
  const Segment *Owner = nullptr;
  for (const Segment &Seg : Img.Segments) {
    if (Seg.VA > InvalidVA - Seg.Size)
      return std::nullopt;
    if (Seg.Size == 0 || Target >= Seg.VA + Seg.Size ||
        Target + Field->Width <= Seg.VA)
      continue;
    if (Owner || !Seg.isReadable() || Seg.isExecutable() || Target < Seg.VA ||
        Field->Width > Seg.Size - (Target - Seg.VA))
      return std::nullopt;
    Owner = &Seg;
  }
  if (!Owner)
    return std::nullopt;
  if (!Img.Sections.empty()) {
    const Section *SectionOwner = nullptr;
    for (const Section &Sec : Img.Sections) {
      if (Sec.VA > InvalidVA - Sec.Size)
        return std::nullopt;
      if (Sec.Size == 0 || Target >= Sec.VA + Sec.Size ||
          Target + Field->Width <= Sec.VA)
        continue;
      if (SectionOwner || !Sec.isReadable() || Sec.isExecutable() ||
          Target < Sec.VA || Field->Width > Sec.Size - (Target - Sec.VA))
        return std::nullopt;
      SectionOwner = &Sec;
    }
    if (!SectionOwner)
      return std::nullopt;
  }
  return Target;
}

void SignatureDB::applyDataContext(
    const BinaryImage &Img, llvm::ArrayRef<uint64_t> Entries,
    llvm::ArrayRef<SignatureMatcher::Hit> Hits,
    llvm::ArrayRef<DeferredCaller> DeferredCallers) {
  if ((Hits.empty() && DeferredCallers.empty()) || Matches.empty() ||
      !Img.isCOFF() || Img.Arch != Arch::X64 || Img.IsRelocatable)
    return;
  // Only pending addresses and source-anchor names need settlement. Retain
  // every competing claim at those addresses without sorting or allocating
  // names for unrelated functions in a large image.
  // Most image names are unrelated long template spellings. Compare lengths
  // before text so a small anchor set need not hash every byte of every name.
  auto ByLength = [](std::string_view A, std::string_view B) {
    return A.size() != B.size() ? A.size() < B.size() : A < B;
  };
  std::set<std::string_view, decltype(ByLength)> AnchorNames(ByLength);
  for (size_t Module : index().dataAnchorModules())
    for (const StoredName &Name : Modules[Module].publicNames())
      AnchorNames.insert(Name.Name);
  if (AnchorNames.empty())
    return;
  std::unordered_map<uint64_t, llvm::SmallVector<size_t, 4>> Relevant;
  for (const auto &Hit : Hits)
    Relevant.try_emplace(Hit.Address);
  for (const SigMatch &Match : Matches)
    if (AnchorNames.contains(Match.Name) ||
        llvm::any_of(Match.Aliases, [&](const std::string &Name) {
          return AnchorNames.contains(Name);
        }))
      Relevant.try_emplace(Match.Address);
  for (size_t I = 0; I < Matches.size(); ++I)
    if (auto At = Relevant.find(Matches[I].Address); At != Relevant.end())
      At->second.push_back(I);
  std::unordered_map<uint64_t, std::string> Names;
  for (const auto &[Address, Indices] : Relevant) {
    llvm::SmallVector<const SigMatch *, 4> Claims;
    for (size_t I : Indices)
      Claims.push_back(&Matches[I]);
    if (const auto Settled = settle(Claims))
      Names.emplace(Address, Settled->Name);
  }
  std::vector<SignatureMatcher::Hit> Pending;
  for (const auto &Hit : Hits) {
    if (Names.contains(Hit.Address))
      continue;
    Pending.push_back(Hit);
  }
  if (Pending.empty() && DeferredCallers.empty())
    return;

  const size_t Frozen = Matches.size();
  std::map<uint64_t, std::vector<size_t>> Ordinary;
  for (const auto &[Address, Indices] : Relevant) {
    const auto Name = Names.find(Address);
    if (Name == Names.end() || !AnchorNames.contains(Name->second))
      continue;
    for (size_t I : Indices) {
      if (MatchModules[I] == NoModule || Matches[I].CallerConfirmed)
        continue;
      const StoredModule &Mod = Modules[MatchModules[I]];
      if (!Mod.RequireReferences && !Mod.RequireCaller)
        Ordinary[Address].push_back(I);
    }
  }
  std::map<uint64_t, std::vector<size_t>> Refined;
  for (const auto &[Address, Proposals] : Ordinary) {
    const auto Existing = Names.find(Address);
    if (Existing == Names.end())
      continue;
    llvm::SmallVector<const SigMatch *, 4> Claims;
    bool Strong = true;
    for (size_t I : Proposals) {
      Claims.push_back(&Matches[I]);
      const StoredModule &Mod = Modules[MatchModules[I]];
      Strong &= SignatureMatcher::isFullyVerified(Mod) &&
                SignatureMatcher::fixedByteCount(Mod) >=
                    SignatureMatcher::MinStatedBytes &&
                llvm::all_of(Mod.publicNames(), [](const StoredName &Name) {
                  return Name.Offset == 0;
                });
    }
    const auto Settled = settle(Claims);
    if (!Strong || !Settled || !Settled->hasName(Existing->second))
      continue;
    std::vector<size_t> Sources;
    bool Useful = false;
    for (const auto &Candidate :
         index().callerModules(Modules, Existing->second)) {
      const StoredModule &Mod = Modules[Candidate.Module];
      if (!Mod.RequireCaller || llvm::any_of(Proposals, [&](size_t I) {
            return Modules[MatchModules[I]].TotalLen != Mod.TotalLen;
          }))
        continue;
      const uint8_t *Bytes = Img.readVA(Address, Mod.TotalLen);
      if (!Bytes || !SignatureMatcher::matchPattern(Mod, Bytes, Mod.TotalLen))
        continue;
      if (SignatureMatcher::fixedByteCount(Mod) <
          SignatureMatcher::MinStatedBytes) {
        Sources.clear();
        Useful = false;
        break;
      }
      Sources.push_back(Candidate.Module);
      Useful |= llvm::any_of(Mod.references(),
                             [&](const StoredName &Ref) { return Ref.IsData; });
    }
    if (!Useful)
      continue;
    Refined.emplace(Address, Sources);
    for (size_t Module : Sources) {
      SigMatch Match;
      Match.Address = Address;
      Match.Name = Existing->second;
      Matches.push_back(std::move(Match));
      MatchModules.push_back(Module);
    }
  }
  if (Matches.size() == Frozen)
    return;
  // Validate every refinement against the same original candidates. Even
  // one contradicted matching source variant invalidates that anchor.
  checkReferences(Img, Entries, {}, Frozen);
  std::set<std::pair<uint64_t, size_t>> Surviving;
  for (size_t I = Frozen; I < Matches.size(); ++I)
    Surviving.emplace(Matches[I].Address, MatchModules[I]);
  Matches.resize(Frozen);
  MatchModules.resize(Frozen);

  DataBindings Bindings;
  std::unordered_map<std::string_view, uint64_t> ByName[2];
  using Field = std::tuple<uint32_t, bool, std::string_view>;
  for (const auto &[Address, Sources] : Refined) {
    if (llvm::any_of(Sources, [&](size_t I) {
          return !Surviving.contains({Address, I});
        }))
      continue;
    std::vector<Field> Common;
    bool First = true;
    for (size_t I : Sources) {
      std::map<uint32_t, std::set<std::pair<bool, std::string_view>>> Sites;
      for (const StoredName &Ref : Modules[I].references())
        if (Ref.IsData)
          Sites[Ref.Offset].emplace(Ref.IsLocalData, Ref.Name);
      std::vector<Field> Current;
      for (const auto &[Offset, Alternatives] : Sites)
        if (Alternatives.size() == 1)
          Current.emplace_back(Offset, Alternatives.begin()->first,
                               Alternatives.begin()->second);
      if (First) {
        Common = std::move(Current);
        First = false;
      } else {
        std::vector<Field> Intersection;
        std::set_intersection(Common.begin(), Common.end(), Current.begin(),
                              Current.end(), std::back_inserter(Intersection));
        Common = std::move(Intersection);
      }
    }
    for (const auto &[Offset, IsLocal, Name] : Common) {
      if (IsLocal) {
        const auto Anchor = localDataAnchor(Name);
        // The source relation belongs to this exact independent operand.
        // Other uses of the same local object cannot establish the binding.
        if (Anchor->Delta || Anchor->Name != Names.at(Address) ||
            Anchor->Offset != Offset)
          continue;
      }
      // An explicit old code reference at this field cannot become data.
      if (llvm::any_of(Ordinary.at(Address), [&](size_t I) {
            return llvm::any_of(Modules[MatchModules[I]].references(),
                                [&](const StoredName &Ref) {
                                  return Ref.Offset == Offset && !Ref.IsData;
                                });
          }))
        continue;
      const auto Target = dataReferenceTarget(
          Img, Address, Offset, Modules[Sources.front()].TotalLen);
      if (!Target)
        continue;
      auto [NameAt, NewName] = ByName[IsLocal].try_emplace(Name, *Target);
      if (!NewName && NameAt->second != *Target)
        NameAt->second = InvalidVA;
      auto [At, Fresh] = Bindings.try_emplace(
          *Target, DataBinding{Name, Address, false, IsLocal});
      if (!Fresh) {
        if (At->second.Name != Name || At->second.IsLocalData != IsLocal)
          At->second.Name = {};
        At->second.MultipleParents |= At->second.Parent != Address;
      }
    }
  }
  std::erase_if(Bindings, [&](const auto &Entry) {
    return Entry.second.Name.empty() || ByName[Entry.second.IsLocalData].at(
                                            Entry.second.Name) != Entry.first;
  });
  if (Bindings.empty())
    return;
  for (const auto &Hit : Pending)
    appendModuleMatches(Hit.Address, Hit.Module);
  // Exactly one data pass. Newly admitted candidates cannot confirm one
  // another or retroactively establish their own data bindings.
  checkReferences(Img, Entries, {}, Frozen, Frozen, &Bindings);
  // An unrelated binding cannot resolve a prior query. Avoid reconstructing
  // its frozen caller index when none of its actual operands acquired evidence.
  std::vector<uint64_t> Revisit;
  for (const DeferredCaller &Caller : DeferredCallers)
    if (Bindings.contains(Caller.DataTarget))
      Revisit.push_back(Caller.Parent);
  llvm::sort(Revisit);
  Revisit.erase(std::unique(Revisit.begin(), Revisit.end()), Revisit.end());
  if (Matches.size() != Frozen || !Revisit.empty())
    applyCallerContext(Img, Entries, Frozen, &Bindings, Revisit);
}

} // namespace neverd::sigs
