//===- SwiftMetadata.cpp - Bounded stored-property metadata recovery
//-------===//
#include "neverd/loader/Swift/SwiftMetadata.h"

#include "../ObjC/ObjCRuntimeData.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace neverd {
namespace {
struct Unsupported : std::runtime_error {
  using std::runtime_error::runtime_error;
};

bool identifier(const std::string &Name) {
  if (Name.empty() ||
      !(std::isalpha(static_cast<unsigned char>(Name[0])) || Name[0] == '_'))
    return false;
  return std::all_of(Name.begin(), Name.end(), [](unsigned char C) {
    return C < 128 && (std::isalnum(C) || C == '_');
  });
}

class Reader {
  const BinaryImage &Image;
  objc::RuntimeData Data;
  ImportStorageSlotCollection Imports;
  std::map<va_t, std::set<va_t>> ClassMetadata;
  std::map<va_t, std::set<va_t>> StructMetadata;

  uint32_t u32(va_t Address) const {
    auto Value = Data.u32(Address);
    if (!Value)
      throw Unsupported(
          "Swift metadata extends beyond readable file-backed bytes");
    return *Value;
  }
  uint64_t u64(va_t Address) const {
    auto Bytes = Data.bytes(Address, 8);
    if (!Bytes)
      throw Unsupported(
          "Swift metadata extends beyond readable file-backed bytes");
    return llvm::support::endian::read64le(Bytes);
  }
  void requirePlainEmptyStructLayout(va_t Metadata) const {
    if (Metadata < 8 || Metadata % 8)
      throw Unsupported("Swift empty struct metadata header is misaligned");
    if (Image.MachOChainedFixupsAmbiguous)
      throw Unsupported("Swift empty struct value-witness table has "
                        "ambiguous fixups");
    const va_t Slot = Metadata - 8;
    if (!Data.bytes(Slot, 8))
      throw Unsupported(
          "Swift empty struct value-witness table is unavailable");
    const auto Binding = Image.DyldBindSlots.find(Slot);
    const auto Canonical = Imports.Slots.find(Slot);
    // Model the ABI declared by a strong two-level reference to the standard
    // runtime's empty-tuple witnesses, which are also used for empty structs.
    // Both the provider and canonical binding must belong to this exact slot.
    if (!Imports.Conflicts.count(Slot) && Image.MachOTwoLevelNamespace &&
        Binding != Image.DyldBindSlots.end() &&
        Binding->second.Name == "_$sytWV" && Binding->second.Addend == 0 &&
        Binding->second.Module == "/usr/lib/swift/libswiftCore.dylib" &&
        !Binding->second.WeakImport && Canonical != Imports.Slots.end() &&
        Canonical->second.Name == Binding->second.Name &&
        Canonical->second.Addend == Binding->second.Addend &&
        Canonical->second.Evidence == ImportStorageEvidence::LoaderBind)
      return;
    // An imported slot's on-disk payload is not a local table address, even
    // when its numeric value happens to point at readable bytes. Other or
    // conflicting imports cannot fall through to local table interpretation.
    if (Imports.Conflicts.count(Slot) || Imports.Slots.count(Slot) ||
        Image.ImportStorageSlots.count(Slot) ||
        Image.ImportPtrSlots.count(Slot) || Image.DyldBindSlots.count(Slot))
      throw Unsupported("Swift empty struct value-witness table is external "
                        "or ambiguous");
    const auto Table = Data.pointer(Slot);
    if (!Table || !*Table || *Table % 8 || !Data.bytes(*Table, 88))
      throw Unsupported(
          "Swift empty struct value-witness table is unavailable");
    // The 64-bit ABI places size, stride, flags, and extra-inhabitant count
    // after eight required function pointers. Validate the declared storage
    // and value traits; this does not prove those functions' implementations.
    if (u64(*Table + 64) != 0 || u64(*Table + 72) != 1 ||
        u32(*Table + 80) != 0 || u32(*Table + 84) != 0)
      throw Unsupported("Swift empty struct value-witness layout is not the "
                        "plain empty struct contract");
  }
  va_t relative(va_t Address) const {
    const int64_t Delta = static_cast<int32_t>(u32(Address));
    if (!Delta)
      throw Unsupported("required Swift metadata reference is null");
    if ((Delta < 0 && Address < static_cast<uint64_t>(-Delta)) ||
        (Delta > 0 && Address > InvalidVA - Delta))
      throw Unsupported("Swift metadata relative address overflows");
    return Delta < 0 ? Address - static_cast<uint64_t>(-Delta)
                     : Address + Delta;
  }
  std::string text(va_t Address) const {
    auto Value = Data.string(Address);
    if (!Value || !identifier(*Value))
      throw Unsupported("unsafe or unavailable Swift metadata identifier");
    return *Value;
  }
  SwiftSourceType primitive(std::string Encoding) const {
    while (!Encoding.empty() && Encoding[0] == '_')
      Encoding.erase(0, 1);
    if (Encoding.starts_with("$s"))
      Encoding.erase(0, 2);
    if (Encoding.ends_with("Mn"))
      Encoding.resize(Encoding.size() - 2);
    std::string Name;
    const std::map<std::string, std::string> Short{{"Si", "Int"},
                                                   {"Su", "UInt"},
                                                   {"Sb", "Bool"},
                                                   {"Sf", "Float"},
                                                   {"Sd", "Double"}};
    if (auto It = Short.find(Encoding); It != Short.end())
      Name = It->second;
    else {
      for (const std::string Candidate :
           {"Int8", "Int16", "Int32", "Int64", "UInt8", "UInt16", "UInt32",
            "UInt64"})
        if (Encoding ==
            "s" + std::to_string(Candidate.size()) + Candidate + "V")
          Name = Candidate;
    }
    if (Name.empty())
      throw Unsupported(
          "stored-property type is not an established Swift scalar");
    if (Name == "Bool")
      return {SwiftSourceType::Kind::Boolean, Name, 1, false, nullptr};
    if (Name == "Float" || Name == "Double")
      return {SwiftSourceType::Kind::Floating, Name,
              Name == "Float" ? 32u : 64u, false, nullptr};
    const bool Signed = Name.starts_with("Int");
    const auto Suffix = Name.substr(Signed ? 3 : 4);
    const unsigned Bits =
        Suffix.empty() ? 64 : static_cast<unsigned>(std::stoul(Suffix));
    return {SwiftSourceType::Kind::Integer, Name, Bits, Signed, nullptr};
  }
  SwiftSourceType fieldType(va_t Address) const {
    const uint8_t *First = Data.bytes(Address, 1);
    if (!First)
      throw Unsupported("Swift stored-property type is unavailable");
    if (*First == 2) {
      // A symbolic indirect context reference names an exact loader-bound
      // pointer slot. The binding's identity, not its raw chained payload,
      // establishes a standard-library nominal descriptor.
      if (!Data.bytes(Address, 6) || *Data.bytes(Address + 5, 1) != 0)
        throw Unsupported(
            "composite symbolic Swift field types are unsupported");
      const auto Slot = relative(Address + 1);
      auto It = Imports.Slots.find(Slot);
      if (Imports.Conflicts.count(Slot) || It == Imports.Slots.end() ||
          It->second.Addend != 0)
        throw Unsupported("Swift symbolic field type has no exact imported "
                          "descriptor binding");
      return primitive(It->second.Name);
    }
    if (*First < 0x20)
      throw Unsupported("unsupported Swift symbolic field-type reference kind");
    auto Value = Data.string(Address);
    if (!Value)
      throw Unsupported("invalid Swift field type encoding");
    return primitive(*Value);
  }

  va_t metadata(va_t Descriptor, bool IsClass) const {
    const auto &Index = IsClass ? ClassMetadata : StructMetadata;
    auto It = Index.find(Descriptor);
    if (It == Index.end() || It->second.size() != 1)
      throw Unsupported("Swift type metadata address is absent or ambiguous");
    return *It->second.begin();
  }

  SwiftRecoveredType type(va_t Descriptor) const {
    SwiftRecoveredType Result;
    Result.Descriptor = Descriptor;
    try {
      const uint32_t Flags = u32(Descriptor);
      const unsigned Kind = Flags & 31;
      Result.Kind = Kind == 16   ? "class"
                    : Kind == 17 ? "struct"
                                 : "unsupported";
      if (Result.Kind == "unsupported")
        throw Unsupported(
            "only class and struct storage layouts are supported");
      const bool IsClass = Kind == 16;
      if (!Data.bytes(Descriptor, IsClass ? 44 : 28))
        throw Unsupported("Swift type descriptor is truncated");
      Result.Name = text(relative(Descriptor + 8));
      const uint32_t ParentOffset = u32(Descriptor + 4);
      if (ParentOffset & 1)
        throw Unsupported("indirect Swift parent contexts are unsupported");
      const auto Parent = relative(Descriptor + 4);
      if ((u32(Parent) & 31) != 0)
        throw Unsupported("nested Swift type contexts are unsupported");
      Result.Module = text(relative(Parent + 8));
      if (Flags & 0x80)
        throw Unsupported(
            "generic Swift storage layouts require instantiation metadata");
      if (IsClass &&
          ((Flags & (uint32_t(1) << 29)) || u32(Descriptor + 20) != 0))
        throw Unsupported(
            "Swift inherited or resilient class layouts are unsupported");
      if (IsClass && (Flags & (uint32_t(3) << 23)))
        throw Unsupported("Swift actor storage layout is unsupported");
      if ((Flags >> 16) & 3)
        throw Unsupported("Swift metadata requires dynamic initialization");
      const uint32_t Count = u32(Descriptor + (IsClass ? 36 : 20));
      const uint32_t VectorWords = u32(Descriptor + (IsClass ? 40 : 24));
      if (Count > 4096 || VectorWords > 1048576)
        throw Unsupported(
            "Swift field layout exceeds the bounded metadata budget");
      const va_t Fields = relative(Descriptor + 16);
      const uint32_t FieldHeader = u32(Fields + 8);
      if ((FieldHeader & 0xffff) != (IsClass ? 1u : 0u) ||
          (FieldHeader >> 16) != 12 || u32(Fields + 12) != Count ||
          !Data.bytes(Fields, 16 + uint64_t(Count) * 12))
        throw Unsupported(
            "Swift stored-property records disagree with the type descriptor");
      Result.Metadata = metadata(Descriptor, IsClass);
      if (Count && !VectorWords)
        throw Unsupported("Swift field-offset vector is absent");
      if (Result.Metadata > InvalidVA - uint64_t(VectorWords) * 8)
        throw Unsupported("Swift field-offset vector overflows");
      const va_t Vector = Result.Metadata + uint64_t(VectorWords) * 8;
      if (Count && !Data.bytes(Vector, uint64_t(Count) * (IsClass ? 8 : 4)))
        throw Unsupported("Swift field-offset vector is truncated");
      Result.Alignment = 1;
      uint64_t PreviousEnd = IsClass ? 16 : 0;
      std::set<std::string> Names;
      for (uint32_t Index = 0; Index < Count; ++Index) {
        const va_t Record = Fields + 16 + uint64_t(Index) * 12;
        const uint32_t FieldFlags = u32(Record);
        if (FieldFlags & ~uint32_t(2))
          throw Unsupported("unsupported Swift stored-property flags");
        SwiftStorageField Field;
        Field.Name = text(relative(Record + 8));
        if (!Names.insert(Field.Name).second)
          throw Unsupported("duplicate Swift stored-property names");
        Field.Type = fieldType(relative(Record + 4));
        Field.Offset = IsClass ? u64(Vector + uint64_t(Index) * 8)
                               : u32(Vector + uint64_t(Index) * 4);
        Field.IsMutable = (FieldFlags & 2) != 0;
        const uint64_t Size =
            Field.Type.TheKind == SwiftSourceType::Kind::Boolean
                ? 1
                : Field.Type.Bits / 8;
        if (Field.Offset < PreviousEnd || Field.Offset % Size ||
            Field.Offset > 1048576 - Size)
          throw Unsupported("Swift scalar fields overlap, misalign, or exceed "
                            "the layout bound");
        // A declaration can reproduce this scalar layout only when the actual
        // vector agrees with the platform's natural scalar placement.
        const uint64_t Expected = (PreviousEnd + Size - 1) & ~(Size - 1);
        if (Field.Offset != Expected)
          throw Unsupported("Swift storage contains an unexplained gap");
        PreviousEnd = Field.Offset + Size;
        Result.Alignment = std::max(Result.Alignment, Size);
        Result.Fields.push_back(std::move(Field));
      }
      Result.Size = PreviousEnd;
      if (!IsClass && Count == 0)
        requirePlainEmptyStructLayout(Result.Metadata);
      if (IsClass) {
        const uint32_t InstanceSize = u32(Result.Metadata + 48);
        const uint32_t AlignmentMask = u32(Result.Metadata + 52) & 0xffff;
        if (u32(Result.Metadata + 44) != 0 || InstanceSize != Result.Size ||
            AlignmentMask + 1 < Result.Alignment || AlignmentMask > 15)
          throw Unsupported("Swift class instance bounds disagree with "
                            "stored-property metadata");
        Result.Alignment = AlignmentMask + 1;
      }
      Result.Status = "recovered";
    } catch (const Unsupported &Error) {
      Result.Reason = Error.what();
    }
    return Result;
  }

public:
  explicit Reader(const BinaryImage &Image)
      : Image(Image), Data(Image), Imports(Image.collectImportStorageSlots()) {
    // Index each symbol once; a large type inventory must not rescan the
    // complete symbol table for every descriptor.
    for (const Symbol &Symbol : Image.Symbols) {
      if (Symbol.IsFunc || !Symbol.Addr || !Image.isDataAddress(Symbol.Addr) ||
          Symbol.Addr > InvalidVA - 72)
        continue;
      if (Data.bytes(Symbol.Addr, 16) && u64(Symbol.Addr) == 0x200) {
        if (auto Descriptor = Data.pointer(Symbol.Addr + 8);
            Descriptor && *Descriptor)
          StructMetadata[*Descriptor].insert(Symbol.Addr);
      }
      if (Data.bytes(Symbol.Addr, 72) && (u32(Symbol.Addr + 40) & 2)) {
        if (auto Descriptor = Data.pointer(Symbol.Addr + 64);
            Descriptor && *Descriptor)
          ClassMetadata[*Descriptor].insert(Symbol.Addr);
      }
    }
  }
  std::vector<SwiftRecoveredType> read() const {
    std::vector<SwiftRecoveredType> Result;
    const Section *Section = Image.getSectionByName("__swift5_types");
    if (!Section)
      return Result;
    if (Image.Format != BinaryFormat::MachO || Image.Bits != Bitness::Bits64 ||
        Image.IsRelocatable || Section->Size % 4 || Section->Size / 4 > 65536 ||
        !Data.bytes(Section->VA, Section->Size)) {
      SwiftRecoveredType Error;
      Error.Reason = "unsupported or malformed Swift type-reference section";
      Result.push_back(Error);
      return Result;
    }
    std::set<va_t> Seen;
    for (uint64_t Offset = 0; Offset < Section->Size; Offset += 4) {
      const va_t Slot = Section->VA + Offset;
      try {
        const uint32_t Ref = u32(Slot);
        if (!Ref)
          continue;
        if (Ref & 3)
          throw Unsupported("indirect or Objective-C Swift type-reference "
                            "entries are unsupported");
        const va_t Descriptor = relative(Slot);
        if (!Seen.insert(Descriptor).second)
          continue;
        Result.push_back(type(Descriptor));
      } catch (const Unsupported &Error) {
        SwiftRecoveredType Item;
        Item.Descriptor = Slot;
        Item.Reason = Error.what();
        Result.push_back(std::move(Item));
      }
    }
    return Result;
  }
};
} // namespace
std::vector<SwiftRecoveredType> recoverSwiftTypes(const BinaryImage &Image) {
  return Reader(Image).read();
}
} // namespace neverd
