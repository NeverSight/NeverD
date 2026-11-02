//===- SignatureCodeLayoutContext.cpp - Source code contributions --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Queries complete nonsplittable source layouts through independently settled
/// ordinary members. Shared data relations stay local to one observation.
///
//===----------------------------------------------------------------------===//

#include "CodeLayoutContext.h"
#include "DataReference.h"
#include "X86/X86DataReference.h"
#include "X86/X86ImportReference.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sigs/SignatureDB.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace neverd::sigs {

uint32_t codeLayoutMemberEnd(const StoredModule &Mod, uint32_t Offset) {
  uint32_t End = Mod.TotalLen;
  for (const auto &Name : Mod.publicNames())
    if (Name.Offset > Offset)
      End = std::min(End, Name.Offset);
  return End;
}

llvm::SmallVector<size_t, 4> codeLayoutAnchors(const StoredModule &Mod) {
  llvm::SmallVector<size_t, 4> Result;
  if (!Mod.IsCodeLayout || Mod.CRCLen ||
      uint64_t(Mod.LeadingCount) + Mod.TailCount != Mod.TotalLen)
    return Result;
  for (size_t I = 0; I < Mod.PublicNameCount; ++I) {
    const uint32_t Begin = Mod.publicNames()[I].Offset;
    uint32_t End = codeLayoutMemberEnd(Mod, Begin);
    // Source section extents can include alignment. Padding cannot make an
    // otherwise weak getter an independent query root.
    while (End > Begin && Mod.isStated(End - 1) &&
           (Mod.Bytes[End - 1] == 0xcc || Mod.Bytes[End - 1] == 0x90))
      --End;
    size_t Fixed = 0;
    for (uint32_t At = Begin; At < End; ++At)
      Fixed += Mod.isStated(At);
    if (Fixed >= SignatureLimits::MinStatedBytes &&
        llvm::any_of(Mod.references(), [&](const StoredName &Ref) {
          return !Ref.IsData && Ref.Offset >= Begin && Ref.Offset < End;
        }))
      Result.push_back(I);
  }
  return Result;
}

bool isValidCodeLayoutContext(const StoredModule &Mod) {
  if (!Mod.IsCodeLayout || Mod.IsLoadConfig || Mod.IsVTable ||
      Mod.RequireCaller || Mod.RequireReferences || Mod.CRCLen || Mod.CRC16 ||
      !Mod.TotalLen || Mod.TotalLen > SignatureLimits::MaxCodeLayoutBytes ||
      uint64_t(Mod.LeadingCount) + Mod.TailCount != Mod.TotalLen ||
      Mod.PublicNameCount < 2 ||
      Mod.PublicNameCount > SignatureLimits::MaxCodeLayoutPublicNames ||
      !Mod.ReferenceCount)
    return false;
  std::set<uint32_t> Starts;
  std::set<std::string_view> Names;
  for (const auto &Name : Mod.publicNames()) {
    if (Name.IsData || Name.IsLocalData || Name.Offset >= Mod.TotalLen ||
        Name.Name.empty() || !Names.insert(Name.Name).second)
      return false;
    Starts.insert(Name.Offset);
  }
  if (Starts.size() < 2 || *Starts.begin() != 0)
    return false;
  llvm::SmallVector<bool, 256> Fields(Mod.TotalLen, false);
  std::map<std::string_view, size_t> DataCounts;
  bool HasImport = false;
  const llvm::ArrayRef<uint8_t> Code(Mod.Bytes, Mod.TotalLen);
  for (const auto &Ref : Mod.references()) {
    if (Ref.IsLocalData || Ref.Name.empty() || Ref.Offset >= Mod.TotalLen)
      return false;
    auto Member = Starts.upper_bound(Ref.Offset);
    if (Member == Starts.begin())
      return false;
    const uint32_t Begin = *std::prev(Member);
    const uint32_t End = Member == Starts.end() ? Mod.TotalLen : *Member;
    const uint32_t Offset = Ref.Offset - Begin;
    const auto Bytes = Code.slice(Begin, End - Begin);
    if (!Ref.IsData) {
      if (!llvm::StringRef(Ref.Name).starts_with(COFFImportPointerPrefix) ||
          Ref.Name.size() == COFFImportPointerPrefix.size() ||
          !isX86ImportReferenceField(Bytes, Offset, true))
        return false;
      HasImport = true;
    } else {
      // Indirect calls are imports here, not anonymous shared storage.
      if (isX86ImportReferenceField(Bytes, Offset, true) ||
          !x64DataReferenceField(Bytes, Offset))
        return false;
      ++DataCounts[Ref.Name];
    }
    if (End - Ref.Offset < sizeof(uint32_t))
      return false;
    for (uint32_t At = Ref.Offset; At < Ref.Offset + sizeof(uint32_t); ++At) {
      if (Fields[At] || Mod.isStated(At))
        return false;
      Fields[At] = true;
    }
  }
  for (uint32_t At = 0; At < Mod.TotalLen; ++At)
    if (Fields[At] == Mod.isStated(At))
      return false; // Every wildcard belongs to exactly one declared field.
  return HasImport && !DataCounts.empty() &&
         llvm::all_of(DataCounts,
                      [](const auto &Entry) { return Entry.second >= 2; }) &&
         !codeLayoutAnchors(Mod).empty();
}

namespace {

bool immutableCode(const BinaryImage &Img, uint64_t Address, uint32_t Size) {
  if (Address > InvalidVA - Size || !Img.readVA(Address, Size))
    return false;
  auto OneOwner = [&](const auto &Regions) {
    bool Found = false;
    for (const auto &Region : Regions) {
      if (Region.VA > InvalidVA - Region.Size)
        return false;
      if (!Region.Size || Address >= Region.VA + Region.Size ||
          Address + Size <= Region.VA)
        continue;
      if (Found || !Region.isReadable() || !Region.isExecutable() ||
          Region.isWritable() || Address < Region.VA ||
          Address + Size > Region.VA + Region.Size)
        return false;
      Found = true;
    }
    return Found;
  };
  return OneOwner(Img.Segments) &&
         (Img.Sections.empty() || OneOwner(Img.Sections));
}

} // namespace

void SignatureDB::applyCodeLayoutContext(const BinaryImage &Img,
                                         llvm::ArrayRef<uint64_t> Entries) {
  if (!index().hasCodeLayouts() || Matches.empty() || !Img.isCOFF() ||
      Img.Arch != Arch::X64 || !Img.is64Bit() || Img.IsRelocatable)
    return;
  // Collect requests by indexed names before settling relevant addresses.
  // Length filtering in the index avoids hashing unrelated template names.
  std::map<uint64_t, llvm::SmallVector<const SigMatch *, 4>> Existing;
  using Root = std::pair<uint64_t, std::string_view>;
  std::map<std::pair<uint64_t, size_t>, llvm::SmallVector<Root, 2>> Requests;
  for (size_t I = 0; I < Matches.size(); ++I) {
    const auto &Match = Matches[I];
    if (MatchModules[I] == NoModule || Match.CallerConfirmed)
      continue;
    auto Query = [&](std::string_view Name) {
      const auto Sources = index().codeLayoutModules(Modules, Name);
      if (Sources.empty())
        return;
      const auto &Body = Modules[MatchModules[I]];
      if (Body.RequireCaller || Body.RequireReferences || Body.IsVTable ||
          Body.IsLoadConfig || Body.IsCodeLayout ||
          !SignatureMatcher::isFullyVerified(Body) ||
          SignatureMatcher::fixedByteCount(Body) <
              SignatureLimits::MinStatedBytes)
        return;
      for (const auto &Source : Sources) {
        const auto &Layout = Modules[Source.Module];
        const uint32_t Offset = Layout.publicNames()[Source.PublicName].Offset;
        const uint32_t Length = codeLayoutMemberEnd(Layout, Offset) - Offset;
        if (Match.Address < Offset || Body.TotalLen > Length ||
            llvm::any_of(Body.publicNames(),
                         [](const auto &N) { return N.Offset != 0; }))
          continue;
        Requests[{Match.Address - Offset, Source.Module}].emplace_back(
            Match.Address, Name);
        for (const auto &Member : Layout.publicNames())
          if (Match.Address - Offset <= InvalidVA - Member.Offset)
            Existing.try_emplace(Match.Address - Offset + Member.Offset);
      }
    };
    Query(Match.Name);
    for (const auto &Alias : Match.Aliases)
      Query(Alias);
  }
  if (Requests.empty())
    return;
  std::set<uint64_t> ContextDependent;
  for (size_t I = 0; I < Matches.size(); ++I) {
    const auto &Match = Matches[I];
    if (auto At = Existing.find(Match.Address); At != Existing.end()) {
      At->second.push_back(&Match);
      if (Match.CallerConfirmed || MatchModules[I] == NoModule) {
        ContextDependent.insert(Match.Address);
      } else {
        const auto &Mod = Modules[MatchModules[I]];
        if (Mod.RequireCaller || Mod.RequireReferences || Mod.IsVTable ||
            Mod.IsLoadConfig || Mod.IsCodeLayout)
          ContextDependent.insert(Match.Address);
      }
    }
  }
  std::map<uint64_t, llvm::SmallVector<std::string_view, 2>> Stated;
  for (const auto &Sym : Img.Symbols)
    if (Existing.contains(Sym.Addr) && Sym.IsFunc && !Sym.IsSection &&
        !Sym.IsBoundaryGuess && Sym.Origin == NameOrigin::Stated &&
        !Sym.Name.empty())
      Stated[Sym.Addr].push_back(Sym.Name);
  for (const auto &Exp : Img.Exports)
    if (Existing.contains(Exp.Addr) && !Exp.Name.empty())
      Stated[Exp.Addr].push_back(Exp.Name);
  std::map<uint64_t, const Import *> Imports;
  for (const auto &Import : Img.Imports)
    if (Import.IATAddr && Img.readVA(Import.IATAddr, sizeof(uint64_t))) {
      auto [At, Fresh] = Imports.emplace(Import.IATAddr, &Import);
      if (!Fresh)
        At->second = nullptr;
    }
  std::vector<uint64_t> SortedEntries;
  if (!std::is_sorted(Entries.begin(), Entries.end())) {
    SortedEntries.assign(Entries.begin(), Entries.end());
    llvm::sort(SortedEntries);
    Entries = SortedEntries;
  }
  std::vector<SigMatch> Added;
  for (const auto &[Request, Roots] : Requests) {
    const auto [Start, Module] = Request;
    const auto &Layout = Modules[Module];
    if (!immutableCode(Img, Start, Layout.TotalLen) ||
        !SignatureMatcher::matchPattern(
            Layout, Img.readVA(Start, Layout.TotalLen), Layout.TotalLen))
      continue;
    bool Valid = true, Rooted = false;
    for (const auto &[Address, Name] : Roots)
      if (!ContextDependent.contains(Address))
        if (const auto Known = settle(Existing.at(Address));
            Known && Known->hasName(Name))
          Rooted = true;
    std::map<std::string_view, uint64_t> Data;
    for (const auto &Ref : Layout.references()) {
      const auto Target =
          dataReferenceTarget(Img, Start, Ref.Offset, Layout.TotalLen);
      if (!Target) {
        Valid = false;
        break;
      }
      if (Ref.IsData) {
        const auto [At, Fresh] = Data.emplace(Ref.Name, *Target);
        if (!Fresh && At->second != *Target)
          Valid = false;
      } else {
        const auto At = Imports.find(*Target);
        if (At == Imports.end() || !At->second ||
            llvm::StringRef(Ref.Name).drop_front(
                COFFImportPointerPrefix.size()) != At->second->Name)
          Valid = false;
      }
    }
    for (const auto &Name : Layout.publicNames()) {
      const uint64_t Address = Start + Name.Offset;
      if (!std::binary_search(Entries.begin(), Entries.end(), Address)) {
        Valid = false;
        break;
      }
      if (const auto At = Stated.find(Address); At != Stated.end())
        if (llvm::none_of(At->second, [&](auto StatedName) {
              return llvm::any_of(Layout.publicNames(), [&](const auto &Alias) {
                return Alias.Offset == Name.Offset && Alias.Name == StatedName;
              });
            }))
          Valid = false;
      if (const auto At = Existing.find(Address); At != Existing.end())
        if (const auto Known = settle(At->second);
            Known &&
            llvm::none_of(Layout.publicNames(), [&](const auto &Alias) {
              return Alias.Offset == Name.Offset && Known->hasName(Alias.Name);
            }))
          Valid = false;
    }
    if (!Valid || !Rooted)
      continue;
    std::map<uint32_t, llvm::SmallVector<std::string_view, 2>> Names;
    for (const auto &Name : Layout.publicNames())
      Names[Name.Offset].push_back(Name.Name);
    for (auto &[Offset, Aliases] : Names) {
      // An existing observation is neither rewritten nor made stronger by a
      // sibling. New rows cannot feed any caller, data or layout pass.
      if (!Existing.at(Start + Offset).empty())
        continue;
      llvm::sort(Aliases, preferredAliasOrder);
      SigMatch Match;
      Match.Address = Start + Offset;
      Match.Name = Aliases.front();
      for (auto Alias : llvm::drop_begin(Aliases))
        Match.Aliases.emplace_back(Alias);
      Match.LibraryName = libraryNameOf(Module);
      Match.FuncLen = codeLayoutMemberEnd(Layout, Offset) - Offset;
      Match.Confirmed = true;
      Added.push_back(std::move(Match));
    }
  }
  for (auto &Match : Added) {
    Matches.push_back(std::move(Match));
    MatchModules.push_back(NoModule);
  }
}

} // namespace neverd::sigs
