//===- SignatureVTableContext.cpp - Primary table candidate context -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Combines source-declared slots, validated image RTTI and complete function
/// bodies without treating table metadata as an automatic source identity.
///
//===----------------------------------------------------------------------===//

#include "VTableContext.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RTTI.h"
#include "neverd/sigs/SignatureDB.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::sigs {

bool isValidVTableContext(const StoredModule &Mod) {
  if (!Mod.IsVTable || Mod.RequireCaller || Mod.RequireReferences ||
      Mod.PublicNameCount != 1 || !Mod.TotalLen ||
      Mod.TotalLen % VTablePointerBytes || Mod.CRCLen || Mod.CRC16 ||
      uint64_t(Mod.LeadingCount) + Mod.TailCount != Mod.TotalLen ||
      Mod.ReferenceCount < Mod.TotalLen / VTablePointerBytes)
    return false;
  const auto &Public = Mod.publicNames().front();
  if (Public.Offset || Public.IsData || Public.IsLocalData ||
      !coff_loader::primaryVTableTypeName(Public.Name))
    return false;
  for (size_t I = 0; I < Mod.TotalLen; ++I)
    if (Mod.isStated(I))
      return false;
  llvm::SmallVector<uint32_t, 8> Slots;
  for (const StoredName &Ref : Mod.references()) {
    if (Ref.IsData || Ref.IsLocalData || Ref.Name.empty() ||
        Ref.Offset >= Mod.TotalLen || Ref.Offset % VTablePointerBytes)
      return false;
    Slots.push_back(Ref.Offset);
  }
  llvm::sort(Slots);
  Slots.erase(std::unique(Slots.begin(), Slots.end()), Slots.end());
  // In-range aligned sites and the exact count prove that none is missing.
  return Slots.size() == Mod.TotalLen / VTablePointerBytes;
}

namespace {

bool hasTableStorage(const BinaryImage &Img, uint64_t Table, uint32_t Length) {
  if (Table < VTablePointerBytes || Table > InvalidVA - Length ||
      !Img.DataPtrRelocSlots.count(Table - VTablePointerBytes) ||
      Img.CodePtrRelocSlots.count(Table + Length))
    return false;
  const uint64_t Begin = Table - VTablePointerBytes;
  const uint64_t End = Table + Length;
  // The locator pointer and complete table must belong to one immutable
  // data mapping. Adjacent or overlapping owners cannot complete a table.
  const Segment *Owner = nullptr;
  for (const Segment &Seg : Img.Segments) {
    if (Seg.VA > InvalidVA - Seg.Size)
      return false;
    if (!Seg.Size || Begin >= Seg.VA + Seg.Size || End <= Seg.VA)
      continue;
    if (Owner || !Seg.isReadable() || Seg.isWritable() || Seg.isExecutable() ||
        Begin < Seg.VA || End > Seg.VA + Seg.Size)
      return false;
    Owner = &Seg;
  }
  if (!Owner || !Img.readVA(Begin, End - Begin))
    return false;
  if (!Img.Sections.empty()) {
    const Section *SectionOwner = nullptr;
    for (const Section &Sec : Img.Sections) {
      if (Sec.VA > InvalidVA - Sec.Size)
        return false;
      if (!Sec.Size || Begin >= Sec.VA + Sec.Size || End <= Sec.VA)
        continue;
      if (SectionOwner || !Sec.isReadable() || Sec.isWritable() ||
          Sec.isExecutable() || Begin < Sec.VA || End > Sec.VA + Sec.Size)
        return false;
      SectionOwner = &Sec;
    }
    if (!SectionOwner)
      return false;
  }
  for (uint64_t Offset = 0; Offset < Length; Offset += VTablePointerBytes)
    if (!Img.CodePtrRelocSlots.count(Table + Offset) ||
        !Img.isCodeAddress(
            readLE<uint64_t>(Img.readVA(Table + Offset, VTablePointerBytes))))
      return false;
  return true;
}

} // namespace

void SignatureDB::applyVTableContext(const BinaryImage &Img,
                                     llvm::ArrayRef<uint64_t> Entries) {
  if (!Img.isCOFF() || Img.Arch != Arch::X64 || Img.IsRelocatable ||
      Img.CodePtrRelocSlots.empty() || index().vtableModules().empty() ||
      !index().hasCallerModules())
    return;
  std::map<std::string_view, std::vector<size_t>> Sources;
  for (size_t I : index().vtableModules()) {
    const auto Type = coff_loader::primaryVTableTypeName(
        Modules[I].publicNames().front().Name);
    Sources[*Type].push_back(I);
  }
  using Field = std::pair<uint32_t, std::string_view>;
  struct TableClaim {
    uint64_t Address;
    std::vector<Field> Fields;
  };
  std::vector<TableClaim> Tables;
  std::map<uint64_t, llvm::SmallVector<const SigMatch *, 4>> Existing;
  // Relocation-backed pointer slots are already indexed by the loader. This
  // reads their RTTI only; it does not scan data sections or function bytes.
  auto Collect = [&](uint64_t Address) {
    const auto Type = coff_loader::readPrimaryVTableTypeName(Img, Address);
    if (!Type)
      return;
    const auto Found = Sources.find(*Type);
    if (Found == Sources.end())
      return;
    std::vector<Field> Agreed;
    bool First = true;
    bool Conflict = false;
    for (size_t I : Found->second) {
      const StoredModule &Mod = Modules[I];
      if (!hasTableStorage(Img, Address, Mod.TotalLen))
        continue;
      std::vector<Field> Fields;
      for (const StoredName &Ref : Mod.references())
        Fields.emplace_back(Ref.Offset, Ref.Name);
      llvm::sort(Fields);
      Fields.erase(std::unique(Fields.begin(), Fields.end()), Fields.end());
      if (First) {
        Agreed = std::move(Fields);
        First = false;
      } else if (Agreed != Fields) {
        Conflict = true;
        break;
      }
    }
    if (First || Conflict)
      return;
    for (const auto &[Offset, Name] : Agreed)
      Existing.try_emplace(
          readLE<uint64_t>(Img.readVA(Address + Offset, VTablePointerBytes)));
    Tables.push_back({Address, std::move(Agreed)});
  };
  // Large C++ images have many more virtual methods than table headers.
  // Intersect the smaller loader index with the other one without copying
  // either set or scanning the image's data bytes.
  if (Img.CodePtrRelocSlots.size() <= Img.DataPtrRelocSlots.size()) {
    for (uint64_t Address : Img.CodePtrRelocSlots)
      if (Address >= VTablePointerBytes &&
          Img.DataPtrRelocSlots.count(Address - VTablePointerBytes))
        Collect(Address);
  } else {
    for (uint64_t Header : Img.DataPtrRelocSlots)
      if (Header <= InvalidVA - VTablePointerBytes &&
          Img.CodePtrRelocSlots.count(Header + VTablePointerBytes))
        Collect(Header + VTablePointerBytes);
  }
  if (Tables.empty())
    return;
  for (const SigMatch &Match : Matches)
    if (const auto At = Existing.find(Match.Address); At != Existing.end())
      At->second.push_back(&Match);
  std::map<uint64_t, SettledRoutine> Known;
  for (const auto &[Address, Claims] : Existing)
    if (auto Name = settle(Claims))
      Known.emplace(Address, std::move(*Name));

  struct SlotRequest {
    uint64_t Target;
    std::vector<std::string_view> Names;
  };
  std::vector<SlotRequest> Requests;
  for (const TableClaim &Table : Tables) {
    std::vector<SlotRequest> Current;
    bool Contradicted = false;
    for (size_t I = 0; I < Table.Fields.size();) {
      size_t End = I + 1;
      while (End < Table.Fields.size() &&
             Table.Fields[End].first == Table.Fields[I].first)
        ++End;
      const uint64_t Target = readLE<uint64_t>(Img.readVA(
          Table.Address + Table.Fields[I].first, VTablePointerBytes));
      const auto Found = Known.find(Target);
      if (Found != Known.end()) {
        Contradicted |= !llvm::any_of(
            llvm::ArrayRef(Table.Fields).slice(I, End - I),
            [&](const Field &F) { return Found->second.hasName(F.second); });
      } else {
        SlotRequest Request{Target, {}};
        for (size_t N = I; N < End; ++N)
          Request.Names.push_back(Table.Fields[N].second);
        Current.push_back(std::move(Request));
      }
      I = End;
    }
    if (!Contradicted)
      for (auto &Request : Current)
        Requests.push_back(std::move(Request));
  }
  llvm::SmallVector<uint64_t> SortedEntries;
  if (!llvm::is_sorted(Entries)) {
    SortedEntries.assign(Entries.begin(), Entries.end());
    llvm::sort(SortedEntries);
    Entries = SortedEntries;
  }
  const size_t Frozen = Matches.size();
  std::set<std::tuple<uint64_t, size_t, std::string_view>> Added;
  for (const SlotRequest &Request : Requests) {
    if (!std::binary_search(Entries.begin(), Entries.end(), Request.Target))
      continue;
    std::map<std::string_view, std::vector<size_t>> Bodies;
    for (std::string_view Name : Request.Names)
      for (const auto &Query : index().callerModules(Modules, Name)) {
        const StoredModule &Mod = Modules[Query.Module];
        if (!Mod.RequireCaller || !SignatureMatcher::isFullyVerified(Mod) ||
            SignatureMatcher::fixedByteCount(Mod) <
                SignatureMatcher::MinStatedBytes)
          continue;
        const uint8_t *Bytes = Img.readVA(Request.Target, Mod.TotalLen);
        if (Bytes && SignatureMatcher::matchPattern(Mod, Bytes, Mod.TotalLen))
          Bodies[Name].push_back(Query.Module);
      }
    // Weak fallback names are alternatives, not unconditional aliases. Two
    // independently named matching bodies at this one slot remain ambiguous.
    // Separate validated slots may still establish folded candidate aliases.
    if (Bodies.size() != 1)
      continue;
    const auto &[Name, Candidates] = *Bodies.begin();
    for (size_t I : Candidates) {
      if (!Added.emplace(Request.Target, I, Name).second)
        continue;
      SigMatch Match;
      Match.Address = Request.Target;
      Match.Name = Name;
      Match.LibraryName = libraryNameOf(I);
      Match.FuncLen = Modules[I].TotalLen;
      Match.CallerConfirmed = true;
      Matches.push_back(std::move(Match));
      MatchModules.push_back(I);
    }
  }
  // Freeze even an empty original set. New table candidates cannot confirm
  // each other's outgoing references or feed an incoming fixed point.
  if (Matches.size() != Frozen)
    checkReferences(Img, Entries, {}, Frozen, Frozen);
}

} // namespace neverd::sigs
