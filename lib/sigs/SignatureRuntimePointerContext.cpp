//===- SignatureRuntimePointerContext.cpp - Runtime pointer candidates ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Binds source defaults through loader-owned slots. Complete bodies and every
/// relocated operand must agree; metadata never certifies automatic identity.
///
//===----------------------------------------------------------------------===//

#include "DataReference.h"
#include "RuntimePointerContext.h"

#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/sigs/SignatureDB.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace neverd::sigs {

bool isValidRuntimePointerContext(const StoredModule &Mod) {
  if (!Mod.IsLoadConfig || Mod.IsVTable || Mod.RequireCaller ||
      Mod.RequireReferences || Mod.PublicNameCount != 1 ||
      !Mod.ReferenceCount || Mod.CRCLen || Mod.CRC16 ||
      uint64_t(Mod.LeadingCount) + Mod.TailCount != Mod.TotalLen)
    return false;
  const auto &Public = Mod.publicNames().front();
  if (Public.Offset || Public.IsData || Public.IsLocalData ||
      Public.Name != "_load_config_used")
    return false;
  for (size_t I = 0; I < Mod.TotalLen; ++I)
    if (Mod.isStated(I))
      return false;
  const auto Fields = coff_loader::loadConfigCallableFields(Bitness::Bits64);
  std::map<uint32_t, std::pair<unsigned, unsigned>> Counts;
  for (const auto &Ref : Mod.references()) {
    if (Ref.IsLocalData || Ref.Name.empty() || Ref.Offset > Mod.TotalLen ||
        Mod.TotalLen - Ref.Offset < sizeof(uint64_t) ||
        llvm::none_of(
            Fields, [&](const auto &F) { return F.Offset == Ref.Offset; }))
      return false;
    auto &Count = Counts[Ref.Offset];
    ++(Ref.IsData ? Count.first : Count.second);
  }
  return llvm::all_of(Counts, [](const auto &F) {
    return F.second.first == 1 && F.second.second == 1;
  });
}

namespace {

/// A whole pointer must have one immutable, raw-backed data owner.
bool immutablePointer(const BinaryImage &Img, uint64_t Address) {
  constexpr uint64_t Width = sizeof(uint64_t);
  if (Address % Width || Address > InvalidVA - Width ||
      !Img.readVA(Address, Width))
    return false;
  auto HasOwner = [&](const auto &Ranges) {
    bool Found = false;
    for (const auto &Range : Ranges) {
      if (Range.VA > InvalidVA - Range.Size)
        return false;
      if (!Range.Size || Address >= Range.VA + Range.Size ||
          Address + Width <= Range.VA)
        continue;
      if (Found || !Range.isReadable() || Range.isWritable() ||
          Range.isExecutable() || Address < Range.VA ||
          Address + Width > Range.VA + Range.Size)
        return false;
      Found = true;
    }
    return Found;
  };
  return HasOwner(Img.Segments) &&
         (Img.Sections.empty() || HasOwner(Img.Sections));
}

} // namespace

void SignatureDB::applyRuntimePointerContext(const BinaryImage &Img,
                                             llvm::ArrayRef<uint64_t> Entries) {
  if (!Img.isCOFF() || Img.Arch != Arch::X64 || !Img.is64Bit() ||
      Img.IsRelocatable || Img.RuntimeCallablePointerSlots.empty() ||
      index().loadConfigModules().empty() || !index().hasCallerModules())
    return;
  using Names = std::pair<std::string_view, std::string_view>;
  std::map<uint32_t, Names> Sources;
  for (size_t I : index().loadConfigModules()) {
    std::map<uint32_t, Names> Current;
    for (const auto &Ref : Modules[I].references()) {
      auto &Pair = Current[Ref.Offset];
      (Ref.IsData ? Pair.first : Pair.second) = Ref.Name;
    }
    for (const auto &[Offset, Pair] : Current) {
      auto [At, Fresh] = Sources.try_emplace(Offset, Pair);
      if (!Fresh && At->second != Pair)
        return; // Conflicting source defaults cannot choose their own winner.
    }
  }
  std::map<uint64_t, std::string_view> Bindings;
  std::map<std::string_view, uint64_t> StorageByName;
  std::map<RuntimeCallablePointerSlotKind, uint64_t> SlotsByKind;
  for (const auto &Slot : Img.RuntimeCallablePointerSlots) {
    auto [At, Fresh] = SlotsByKind.try_emplace(Slot.Kind, Slot.SlotVA);
    if (!Fresh && At->second != Slot.SlotVA)
      return;
  }
  struct Request {
    uint64_t Target;
    std::string_view Name;
  };
  std::vector<Request> Requests;
  for (const auto &Field : coff_loader::loadConfigCallableFields(Img.Bits)) {
    const auto Source = Sources.find(Field.Offset);
    const auto Slot = SlotsByKind.find(Field.Kind);
    if (Source == Sources.end() || Slot == SlotsByKind.end())
      continue;
    const uint64_t Storage = Slot->second;
    if (!immutablePointer(Img, Storage) ||
        !Img.CodePtrRelocSlots.count(Storage))
      continue;
    const uint64_t Target =
        readLE<uint64_t>(Img.readVA(Storage, sizeof(uint64_t)));
    if (!Img.isCodeAddress(Target))
      continue;
    const auto &[Variable, Default] = Source->second;
    auto [Binding, Fresh] = Bindings.try_emplace(Storage, Variable);
    if (!Fresh && Binding->second != Variable)
      return;
    auto [Named, NewName] = StorageByName.try_emplace(Variable, Storage);
    if (!NewName && Named->second != Storage)
      return;
    Requests.push_back({Target, Default});
  }
  if (Requests.empty())
    return;
  llvm::SmallVector<uint64_t> Sorted;
  if (!llvm::is_sorted(Entries)) {
    Sorted.assign(Entries.begin(), Entries.end());
    llvm::sort(Sorted);
    Entries = Sorted;
  }
  std::map<uint64_t, llvm::SmallVector<const SigMatch *, 4>> Existing;
  for (const auto &Request : Requests)
    Existing.try_emplace(Request.Target);
  for (const SigMatch &Match : Matches)
    if (auto At = Existing.find(Match.Address); At != Existing.end())
      At->second.push_back(&Match);
  // A known contradictory default invalidates this source configuration.
  for (const auto &Request : Requests) {
    if (const auto Known = settle(Existing.at(Request.Target));
        Known && !Known->hasName(Request.Name))
      return;
    if (const auto *Sym = Img.findSymbolAt(Request.Target);
        Sym && Sym->IsFunc && !Sym->IsSection && !Sym->IsBoundaryGuess &&
        Sym->Origin == NameOrigin::Stated && !Sym->Name.empty() &&
        Sym->Name != Request.Name)
      return;
  }
  std::set<std::pair<uint64_t, size_t>> Added;
  for (const auto &Request : Requests) {
    if (!Existing.at(Request.Target).empty() ||
        !std::binary_search(Entries.begin(), Entries.end(), Request.Target))
      continue;
    for (const auto &Query : index().callerModules(Modules, Request.Name)) {
      const auto &Mod = Modules[Query.Module];
      if (!Mod.RequireCaller || !SignatureMatcher::isFullyVerified(Mod) ||
          Mod.CRCLen ||
          uint64_t(Mod.LeadingCount) + Mod.TailCount != Mod.TotalLen)
        continue;
      const auto *Bytes = Img.readVA(Request.Target, Mod.TotalLen);
      if (!Bytes || !SignatureMatcher::matchPattern(Mod, Bytes, Mod.TotalLen))
        continue;
      // This independent metadata gate may admit short bodies, but cannot
      // leave any wildcard or outgoing reference unexplained. Ordinary caller
      // matching retains its existing strength floor and reference policy.
      llvm::SmallVector<bool> Bound(Mod.TotalLen, false);
      bool Complete = true;
      for (const auto &Ref : Mod.references()) {
        if (!Ref.IsData || Ref.IsLocalData) {
          Complete = false;
          break;
        }
        const auto Target =
            dataReferenceTarget(Img, Request.Target, Ref.Offset, Mod.TotalLen);
        if (!Target) {
          Complete = false;
          break;
        }
        const auto At = Bindings.find(*Target);
        if (At == Bindings.end() || At->second != Ref.Name) {
          Complete = false;
          break;
        }
        for (size_t I = Ref.Offset; I < Ref.Offset + sizeof(uint32_t); ++I)
          Bound[I] = true;
      }
      for (size_t I = 0; Complete && I < Mod.TotalLen; ++I)
        Complete = Mod.isStated(I) || Bound[I];
      if (!Complete || !Added.emplace(Request.Target, Query.Module).second)
        continue;
      SigMatch Match;
      Match.Address = Request.Target;
      Match.Name = Request.Name;
      Match.LibraryName = libraryNameOf(Query.Module);
      Match.FuncLen = Mod.TotalLen;
      Match.CallerConfirmed = true;
      // All outgoing sites are data operands validated above against immutable
      // loader/source bindings. No provisional function can confirm a site, so
      // rebuilding the general reference graph would repeat this same proof.
      Match.Confirmed = Mod.ReferenceCount != 0;
      Matches.push_back(std::move(Match));
      MatchModules.push_back(Query.Module);
    }
  }
}

} // namespace neverd::sigs
