#include "ObjCRuntimeData.h"

#include "neverd/loader/ObjC/ObjCMethods.h"

#include <algorithm>
#include <limits>
#include <set>

namespace neverd {
namespace {

constexpr uint64_t MaxRecords = 65536;

void diagnostic(BinaryImage &Image, llvm::StringRef Message) {
  auto &Diagnostics = Image.ObjCMetadataDiagnostics;
  if (Diagnostics.size() < 64 &&
      std::find(Diagnostics.begin(), Diagnostics.end(), Message) ==
          Diagnostics.end())
    Diagnostics.push_back(Message.str());
}

bool readIvars(ObjCClass &Class, const objc::RuntimeData &Data,
               uint64_t &Remaining) {
  auto RO = Data.classRO(Class.Address);
  if (!RO)
    return false;
  auto Start = Data.u32(*RO + 4);
  auto Size = Data.u32(*RO + 8);
  auto List = Data.pointer(*RO + 48);
  if (!Start || !Size || !List || *Start > *Size || *Size > (1U << 24))
    return false;
  Class.InstanceStart = *Start;
  Class.InstanceSize = *Size;
  if (!*List)
    return true;
  auto EntrySize = Data.u32(*List);
  auto Count = *List <= InvalidVA - 4 ? Data.u32(*List + 4) : std::nullopt;
  if (!EntrySize || !Count || *EntrySize < 32 || *EntrySize > 4096 ||
      *Count > Remaining ||
      !Data.bytes(*List, 8 + uint64_t(*EntrySize) * *Count))
    return false;
  Remaining -= *Count;
  std::set<std::string> Names;
  std::vector<ObjCIvar> Ivars;
  for (uint32_t Index = 0; Index < *Count; ++Index) {
    const va_t Record = *List + 8 + uint64_t(Index) * *EntrySize;
    auto OffsetSlot = Data.pointer(Record);
    auto NameSlot = Data.pointer(Record + 8);
    auto TypeSlot = Data.pointer(Record + 16);
    auto Align = Data.u32(Record + 24);
    auto Width = Data.u32(Record + 28);
    auto Offset = OffsetSlot ? Data.u32(*OffsetSlot) : std::nullopt;
    auto Name = NameSlot ? Data.string(*NameSlot) : std::nullopt;
    auto Type = TypeSlot ? Data.string(*TypeSlot) : std::nullopt;
    if (!Offset || !Name || !Type || !Align || !Width || !*Width ||
        !Names.insert(*Name).second || *Offset < *Start ||
        !rangeInBounds(*Offset, *Width, *Size) ||
        (*Align > 16 && *Align != std::numeric_limits<uint32_t>::max()))
      return false;
    const uint32_t Alignment =
        *Align == std::numeric_limits<uint32_t>::max() ? 8 : 1U << *Align;
    if (*Offset % Alignment)
      return false;
    for (const auto &Other : Ivars)
      if (*Offset < uint64_t(Other.Offset) + Other.Size &&
          Other.Offset < uint64_t(*Offset) + *Width)
        return false;
    Ivars.push_back(
        {*Name, *Type, Record, *OffsetSlot, *Offset, *Width, Alignment});
  }
  Class.Ivars = std::move(Ivars);
  return true;
}

std::optional<ObjCSourceReference> readReference(const BinaryImage &Image,
                                                 const objc::RuntimeData &Data,
                                                 const Section &Section,
                                                 va_t Slot) {
  ObjCSourceReference Reference;
  Reference.Address = Slot;
  if (Section.Name == "__objc_selrefs") {
    auto Pointer = Data.pointer(Slot);
    auto Name = Pointer ? Data.string(*Pointer) : std::nullopt;
    if (!Name)
      return std::nullopt;
    Reference.Name = *Name;
    return Reference;
  }
  if (auto Bound = Image.DyldBindSlots.find(Slot);
      Bound != Image.DyldBindSlots.end()) {
    if (Bound->second.Addend != 0)
      return std::nullopt;
    llvm::StringRef Name(Bound->second.Name);
    if (Name.consume_front("_OBJC_CLASS_$_"))
      Reference.TheKind = ObjCSourceReference::Kind::Class;
    else if (Name.consume_front("_OBJC_METACLASS_$_"))
      Reference.TheKind = ObjCSourceReference::Kind::Metaclass;
    else
      return std::nullopt;
    if (Name.empty())
      return std::nullopt;
    Reference.Name = Name.str();
    return Reference;
  }
  auto Pointer = Data.pointer(Slot);
  auto RO = Pointer ? Data.classRO(*Pointer) : std::nullopt;
  auto Flags = RO ? Data.u32(*RO) : std::nullopt;
  auto Name = Pointer ? Data.className(*Pointer) : std::nullopt;
  if (!Flags || !Name)
    return std::nullopt;
  Reference.TheKind = (*Flags & 1) ? ObjCSourceReference::Kind::Metaclass
                                   : ObjCSourceReference::Kind::Class;
  Reference.Name = *Name;
  return Reference;
}

} // namespace

void parseObjCStorage(BinaryImage &Image) {
  Image.ObjCSourceReferences.clear();
  if (Image.Format != BinaryFormat::MachO || Image.Bits != Bitness::Bits64 ||
      Image.IsRelocatable)
    return;
  objc::RuntimeData Data(Image);
  uint64_t Remaining = MaxRecords;
  std::set<va_t> Conflicts;
  auto Publish = [&](ObjCSourceReference Reference) {
    if (Conflicts.count(Reference.Address))
      return;
    auto [Position, Inserted] =
        Image.ObjCSourceReferences.emplace(Reference.Address, Reference);
    if (!Inserted && (Position->second.TheKind != Reference.TheKind ||
                      Position->second.Size != Reference.Size ||
                      Position->second.Name != Reference.Name ||
                      Position->second.ClassName != Reference.ClassName)) {
      Conflicts.insert(Reference.Address);
      Image.ObjCSourceReferences.erase(Position);
      diagnostic(Image, "Conflicting Objective-C source reference identities");
    }
  };
  for (auto &Class : Image.ObjCClasses) {
    Class.Ivars.clear();
    Class.IvarStatus = "unresolved";
    if (!readIvars(Class, Data, Remaining)) {
      diagnostic(Image, "Objective-C instance storage layout is incomplete");
      continue;
    }
    Class.IvarStatus = "recovered";
    for (const auto &Ivar : Class.Ivars) {
      uint16_t Width = 4;
      if (Image.Arch == Arch::X64) {
        const auto *Bytes = Data.bytes(Ivar.OffsetAddress, 8);
        if (!Bytes || llvm::support::endian::read64le(Bytes) != Ivar.Offset) {
          diagnostic(Image, "Objective-C ivar offset carrier is unresolved");
          continue;
        }
        Width = 8;
      }
      Publish({ObjCSourceReference::Kind::IvarOffset, Ivar.OffsetAddress, Width,
               Ivar.Name, Class.Name});
    }
  }
  for (const auto &Section : Image.Sections) {
    if (Section.Name != "__objc_selrefs" &&
        Section.Name != "__objc_classrefs" &&
        Section.Name != "__objc_superrefs")
      continue;
    if (Section.Size % 8 || Section.Size / 8 > Remaining ||
        Image.getSectionFor(Section.VA) != &Section ||
        !Data.bytes(Section.VA, Section.Size)) {
      diagnostic(Image, "Objective-C runtime reference section is incomplete");
      continue;
    }
    Remaining -= Section.Size / 8;
    for (uint64_t Offset = 0; Offset < Section.Size; Offset += 8) {
      auto Reference = readReference(Image, Data, Section, Section.VA + Offset);
      if (Reference)
        Publish(std::move(*Reference));
      else
        diagnostic(Image, "Objective-C runtime reference slot is unresolved");
    }
  }
}

} // namespace neverd
