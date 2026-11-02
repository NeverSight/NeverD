//===- ObjCStorage.cpp - Objective-C storage declarations ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read storage declarations separately from native object reconstruction.
//===----------------------------------------------------------------------===//

#include "../MachO/DarwinRuntimeImport.h"
#include "ObjCRuntimeData.h"

#include "neverd/loader/Limits.h"
#include "neverd/loader/ObjC/ObjCMethods.h"

#include <algorithm>
#include <limits>
#include <set>

namespace neverd {
namespace {
#define NEVERD_OBJC_STORAGE_VALUE(Name, Value)                                 \
  inline constexpr uint64_t Name = Value;
#define NEVERD_OBJC_STORAGE_TEXT(Name, Text)                                   \
  inline constexpr char Name[] = Text;
#include "ObjCStorage.def"

void diagnostic(BinaryImage &Image, llvm::StringRef Message) {
  auto &Diagnostics = Image.ObjCMetadataDiagnostics;
  if (Diagnostics.size() <
          ::neverd::limits::loader::kObjCStorageDiagnosticsSizeLimit &&
      std::find(Diagnostics.begin(), Diagnostics.end(), Message) ==
          Diagnostics.end())
    Diagnostics.push_back(Message.str());
}

bool readIvars(ObjCClass &Class, const objc::RuntimeData &Data,
               uint64_t &Remaining, uint16_t OffsetWidth, bool AllowEmptyType,
               bool MetadataOnly) {
  auto RO = Data.classRO(Class.Address);
  if (!RO)
    return false;
  auto Start = Data.u32(*RO + kInstanceStartOffset);
  auto Size = Data.u32(*RO + kInstanceSizeOffset);
  auto List = Data.pointer(*RO + kIvarsOffset);
  if (!Start || !Size || !List || *Start > *Size || *Size > kMaxInstanceBytes)
    return false;
  Class.InstanceStart = *Start;
  Class.InstanceSize = *Size;
  if (!*List)
    return true;
  auto EntrySize = Data.u32(*List);
  auto Count = *List <= InvalidVA - kWordBytes ? Data.u32(*List + kWordBytes)
                                               : std::nullopt;
  if (!EntrySize || !Count || *EntrySize < kIvarBytes ||
      *EntrySize >
          ::neverd::limits::loader::kObjCStorageReadIvarsEntrySizeLimit ||
      *Count > Remaining ||
      !Data.bytes(*List, kListHeaderBytes + uint64_t(*EntrySize) * *Count))
    return false;
  Remaining -= *Count;
  std::set<std::string> Names;
  std::set<va_t> OffsetSlots;
  std::vector<ObjCIvar> Ivars;
  for (uint32_t Index = 0; Index < *Count; ++Index) {
    const va_t Record = *List + kListHeaderBytes + uint64_t(Index) * *EntrySize;
    auto OffsetSlot = Data.pointer(Record);
    auto NameSlot = Data.pointer(Record + kIvarNameOffset);
    auto TypeSlot = Data.pointer(Record + kIvarTypeOffset);
    auto Align = Data.u32(Record + kIvarAlignmentOffset);
    auto Width = Data.u32(Record + kIvarSizeOffset);
    const auto *OffsetBytes = OffsetSlot && *OffsetSlot
                                  ? Data.bytes(*OffsetSlot, OffsetWidth)
                                  : nullptr;
    std::optional<uint64_t> Offset;
    if (OffsetBytes)
      Offset = OffsetWidth == kPointerBytes
                   ? llvm::support::endian::read64le(OffsetBytes)
                   : llvm::support::endian::read32le(OffsetBytes);
    const bool RuntimeOffset = !Offset && AllowEmptyType &&
                               OffsetWidth == kPointerBytes && OffsetSlot &&
                               Data.runtimeOffsetSlot(*OffsetSlot);
    auto Name = NameSlot ? Data.string(*NameSlot) : std::nullopt;
    auto Type = TypeSlot && *TypeSlot ? Data.string(*TypeSlot, AllowEmptyType)
                                      : std::nullopt;
    // An analysis declaration retains the metadata's byte size and offset.
    // Bitfields can overlap or be unaligned, trailing arrays can have size
    // zero, and Swift can omit an offset global. None proves native storage.
    const bool MissingMetadataOffset =
        MetadataOnly && AllowEmptyType && OffsetSlot && !*OffsetSlot;
    if ((!Offset && !RuntimeOffset && !MissingMetadataOffset) || !Name ||
        !Type || !Align || !Width ||
        (!MetadataOnly && !*Width && !AllowEmptyType) ||
        *Width > kMaxInstanceBytes || !Names.insert(*Name).second ||
        !OffsetSlot ||
        (*OffsetSlot && !OffsetSlots.insert(*OffsetSlot).second) ||
        (Offset && *Offset > std::numeric_limits<uint32_t>::max()) ||
        (!MetadataOnly && Offset &&
         (*Offset < *Start || !rangeInBounds(*Offset, *Width, *Size))) ||
        (*Align > kMaxAlignmentShift &&
         *Align != std::numeric_limits<uint32_t>::max()))
      return false;
    const uint32_t Alignment = *Align == std::numeric_limits<uint32_t>::max()
                                   ? kPointerBytes
                                   : 1U << *Align;
    if (!MetadataOnly && Offset && *Offset % Alignment)
      return false;
    for (const auto &Other : Ivars)
      if (!MetadataOnly && Offset && Other.Offset && *Width && Other.Size &&
          *Offset < uint64_t(*Other.Offset) + Other.Size &&
          *Other.Offset < uint64_t(*Offset) + *Width)
        return false;
    Ivars.push_back(
        {*Name, *Type, Record, *OffsetSlot,
         Offset ? std::optional(static_cast<uint32_t>(*Offset)) : std::nullopt,
         *Width, Alignment});
  }
  Class.Ivars = std::move(Ivars);
  return true;
}

std::optional<ObjCSourceReference>
importedClassReference(va_t Slot, llvm::StringRef Name) {
  ObjCSourceReference Reference;
  Reference.Address = Slot;
  if (Name.consume_front(kClassPrefix))
    Reference.TheKind = ObjCSourceReference::Kind::Class;
  else if (Name.consume_front(kMetaclassPrefix))
    Reference.TheKind = ObjCSourceReference::Kind::Metaclass;
  else
    return std::nullopt;
  if (Name.empty())
    return std::nullopt;
  Reference.Name = Name.str();
  return Reference;
}

std::optional<ObjCSourceReference> readReference(const BinaryImage &Image,
                                                 const objc::RuntimeData &Data,
                                                 const Section &Section,
                                                 va_t Slot) {
  ObjCSourceReference Reference;
  Reference.Address = Slot;
  if (Section.Name == kSelectorReferences) {
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
    return importedClassReference(Slot, Bound->second.Name);
  }
  auto Pointer = Data.pointer(Slot);
  auto RO = Pointer ? Data.classRO(*Pointer) : std::nullopt;
  auto Flags = RO ? Data.u32(*RO) : std::nullopt;
  auto Name = Pointer ? Data.className(*Pointer) : std::nullopt;
  if (!Flags || !Name)
    return std::nullopt;
  Reference.TheKind = (*Flags & kMetaClassFlag)
                          ? ObjCSourceReference::Kind::Metaclass
                          : ObjCSourceReference::Kind::Class;
  Reference.Name = *Name;
  return Reference;
}

} // namespace

void parseObjCStorage(BinaryImage &Image) {
  Image.ObjCSourceReferences.clear();
  Image.ObjCMetadataReferences.clear();
  if (Image.Format != BinaryFormat::MachO || Image.Bits != Bitness::Bits64 ||
      Image.IsRelocatable)
    return;
  const bool MetadataOnly = Image.MachOSharedCache || Image.isArm64e() ||
                            !Image.MachOAuthenticatedPointers.empty();
  objc::RuntimeData Data(Image, MetadataOnly
                                    ? objc::PointerView::MetadataTarget
                                    : objc::PointerView::PlainRuntimeValue);
  auto &References =
      MetadataOnly ? Image.ObjCMetadataReferences : Image.ObjCSourceReferences;
  uint64_t Remaining = ::neverd::limits::loader::kObjCStorageMaxRecords;
  std::set<va_t> Conflicts;
  auto Publish = [&](ObjCSourceReference Reference) {
    if (Conflicts.count(Reference.Address))
      return;
    auto [Position, Inserted] =
        References.emplace(Reference.Address, Reference);
    if (!Inserted && (Position->second.TheKind != Reference.TheKind ||
                      Position->second.Size != Reference.Size ||
                      Position->second.Name != Reference.Name ||
                      Position->second.ClassName != Reference.ClassName)) {
      Conflicts.insert(Reference.Address);
      References.erase(Position);
      diagnostic(Image, kConflictingIdentity);
    }
  };
  // Linkers can coalesce a class reference into an ordinary imported pointer
  // slot. The exact import identifies the class; a section spelling does not.
  for (const auto &[Slot, Name] : Image.ImportPtrSlots) {
    auto Reference = importedClassReference(Slot, Name);
    if (!Reference)
      continue;
    if (!Remaining) {
      diagnostic(Image, kImportedReferenceLimit);
      break;
    }
    --Remaining;
    const auto *Section = Image.getSectionFor(Slot);
    const auto *Segment = Image.getSegmentFor(Slot);
    if (Slot % kPointerBytes || !Section || !Segment ||
        Section->isExecutable() || Segment->isExecutable() ||
        !Data.bytes(Slot, kPointerBytes) || !darwinRuntimeImport(Image, Slot))
      continue;
    Publish(std::move(*Reference));
  }
  for (auto &Class : Image.ObjCClasses) {
    Class.Ivars.clear();
    Class.IvarStatus = kUnresolved;
    const auto ClassBits = Data.pointer(Class.Address + kClassBitsOffset);
    const bool SwiftStable =
        ClassBits && (*ClassBits & kSwiftBitsMask) == kSwiftStableBits;
    // Swift's stable class metadata uses pointer-sized field-offset globals,
    // including on arm64. Its unexposed fields retain an empty type encoding;
    // their byte layout is still explicit. Preserve that missing type fact.
    // Ordinary arm64 Objective-C classes use 32-bit offset globals instead.
    const uint16_t OffsetWidth =
        Image.Arch == Arch::X64 || SwiftStable ? kPointerBytes : kWordBytes;
    if (!readIvars(Class, Data, Remaining, OffsetWidth, SwiftStable,
                   MetadataOnly)) {
      diagnostic(Image, kIncompleteLayout);
      continue;
    }
    Class.IvarStatus = MetadataOnly ? kMetadataOnly
                       : std::all_of(Class.Ivars.begin(), Class.Ivars.end(),
                                     [](const ObjCIvar &Ivar) {
                                       return Ivar.Offset && Ivar.Size;
                                     })
                           ? kRecovered
                           : kRuntime;
    for (const auto &Ivar : Class.Ivars) {
      if (!Ivar.OffsetAddress)
        continue;
      Publish({ObjCSourceReference::Kind::IvarOffset, Ivar.OffsetAddress,
               OffsetWidth, Ivar.Name, Class.Name});
    }
  }
  std::map<va_t, std::string> Protocols;
  std::set<std::string> AmbiguousProtocols;
  for (const auto &Protocol : Image.ObjCProtocols) {
    if (!Remaining) {
      Protocols.clear();
      diagnostic(Image, kProtocolIdentityLimit);
      break;
    }
    --Remaining;
    // objc4 remapProtocolRef resolves every local protocol reference through
    // the registered name, including duplicate compiler declarations. This
    // establishes identity only; selector ABI merging still inspects every
    // declaration and cannot choose a winner from this name.
    // https://github.com/apple-oss-distributions/objc4/blob/main/runtime/objc-runtime-new.mm
    if (Protocol.Status != kRecovered || Protocol.Name.empty())
      AmbiguousProtocols.insert(Protocol.Name);
    if (Protocol.Status == kRecovered) {
      auto [It, Inserted] = Protocols.emplace(Protocol.Address, Protocol.Name);
      if (!Inserted) {
        AmbiguousProtocols.insert(It->second);
        AmbiguousProtocols.insert(Protocol.Name);
      }
    }
  }
  for (const auto &Section : Image.Sections) {
    if (Section.Name != kSelectorReferences &&
        Section.Name != kClassReferences &&
        Section.Name != kSuperclassReferences &&
        Section.Name != kProtocolReferences)
      continue;
    if (Section.Size % kPointerBytes ||
        Section.Size / kPointerBytes > Remaining ||
        Image.getSectionFor(Section.VA) != &Section ||
        !Data.bytes(Section.VA, Section.Size)) {
      diagnostic(Image, kIncompleteSection);
      continue;
    }
    Remaining -= Section.Size / kPointerBytes;
    for (uint64_t Offset = 0; Offset < Section.Size; Offset += kPointerBytes) {
      const auto Slot = Section.VA + Offset;
      if (Section.Name == kProtocolReferences) {
        const auto Address = Data.localPointer(Slot);
        const auto Found = Address ? Protocols.find(*Address) : Protocols.end();
        if (Slot % kPointerBytes || Section.isExecutable() ||
            !Image.getSegmentFor(Slot) ||
            Image.getSegmentFor(Slot)->isExecutable() || !Address ||
            Found == Protocols.end() ||
            AmbiguousProtocols.count(Found->second)) {
          diagnostic(Image, kUnresolvedProtocol);
          continue;
        }
        Publish({ObjCSourceReference::Kind::Protocol,
                 Slot,
                 kPointerBytes,
                 Found->second,
                 {}});
        continue;
      }
      auto Reference = readReference(Image, Data, Section, Slot);
      if (Reference)
        Publish(std::move(*Reference));
      else
        diagnostic(Image, kUnresolvedReference);
    }
  }
}

} // namespace neverd
