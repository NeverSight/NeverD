#include "neverd/loader/ObjC/ObjCConstantObjects.h"

#include "../MachO/DarwinRuntimeImport.h"
#include "../MachO/DarwinSourceDeclarations.h"
#include "ObjCRuntimeData.h"

#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/Support/ConvertUTF.h"

#include <algorithm>
#include <set>

namespace neverd {
namespace {
class Reader {
  const BinaryImage &Image;
  const objc::RuntimeData Data;
  ObjCConstantObjectGraph Objects;
  std::map<va_t, unsigned> Heights;
  std::set<va_t> Active;
  size_t BytesLeft = 1024 * 1024;
  size_t EdgesLeft = 16384;

  bool charge(size_t Bytes) {
    if (Bytes > BytesLeft)
      return false;
    BytesLeft -= Bytes;
    return true;
  }

  std::optional<uint64_t> word(va_t Address) const {
    const auto Bytes = readImmutableImageBytes(Image, Address, 8);
    return Bytes ? std::optional(llvm::support::endian::read64le(Bytes->data()))
                 : std::nullopt;
  }

  bool record(va_t Address, uint64_t Size, llvm::StringRef SectionName,
              llvm::StringRef ClassName) const {
    const auto *Section = Image.getSectionFor(Address);
    const auto *Segment = Image.getSegmentFor(Address);
    if (!Section || !Segment || Section->Name != SectionName ||
        (Section->SegmentName != "__DATA" &&
         Section->SegmentName != "__DATA_CONST") ||
        Section->isExecutable() ||
        (Section->Type & llvm::MachO::SECTION_TYPE) != llvm::MachO::S_REGULAR ||
        (!Segment->ReadOnlyAfterRelocations &&
         (Section->isWritable() || Segment->isWritable())) ||
        Address % 8 || (Address - Section->VA) % Size || Section->Size % Size ||
        !Data.bytes(Address, Size))
      return false;
    auto Overlaps = [&](va_t Base, uint64_t Extent) {
      return Extent && (Base <= Address ? Address - Base < Extent
                                        : Base - Address < Size);
    };
    for (const auto &Other : Image.Sections)
      if (&Other != Section && Overlaps(Other.VA, Other.Size))
        return false;
    for (const auto &Other : Image.Segments)
      if (&Other != Segment && Overlaps(Other.VA, Other.Size))
        return false;
    const auto Isa = Image.DyldBindSlots.find(Address);
    if (Isa == Image.DyldBindSlots.end() || Isa->second.Name != ClassName ||
        Isa->second.WeakImport || Isa->second.Addend)
      return false;
    constexpr llvm::StringLiteral Modules(
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation|"
        "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/"
        "CoreFoundation|"
        "/System/Library/Frameworks/Foundation.framework/Foundation|"
        "/System/Library/Frameworks/Foundation.framework/Versions/C/"
        "Foundation");
    if (!darwinExportModuleMatches(Modules, Isa->second.Module))
      return false;
    if (auto It = Image.ImportStorageSlots.find(Address);
        It != Image.ImportStorageSlots.end() &&
        (It->second.Name != ClassName || It->second.Addend))
      return false;
    if (auto It = Image.ImportPtrSlots.find(Address);
        It != Image.ImportPtrSlots.end() && It->second != ClassName)
      return false;
    // Only the exact imported isa slot is allowed here. Scalar fields and
    // outgoing edges are checked separately by the immutable byte/pointer
    // readers. A nearby fixup overlapping isa is not an independent slot.
    auto Touches = [&](const auto &Slots, auto Key, bool AllowExact = false) {
      auto It = Slots.lower_bound(Address >= 7 ? Address - 7 : 0);
      for (;
           It != Slots.end() && (Key(*It) < Address || Key(*It) - Address < 8);
           ++It)
        if (!AllowExact || Key(*It) != Address)
          return true;
      return false;
    };
    auto Set = [](va_t Slot) { return Slot; };
    auto Map = [](const auto &Slot) { return Slot.first; };
    if (Touches(Image.DyldBindSlots, Map, true) ||
        Touches(Image.ImportStorageSlots, Map, true) ||
        Touches(Image.ImportPtrSlots, Map, true) ||
        Touches(Image.MachOResolvedChainedPointerSlots, Set, true) ||
        Touches(Image.DataPtrRelocSlots, Set) ||
        Touches(Image.DataPtrRelocTargetOwners, Map) ||
        Touches(Image.CodePtrRelocSlots, Set) ||
        Touches(Image.RelCodeRelocSlots, Set) ||
        Touches(Image.RelDataPtrRelocSlots, Set) ||
        Touches(Image.ConflictingImportStorageSlots, Set) ||
        Touches(Image.ObjCSourceReferences, Map) ||
        Touches(Image.DataAddressRelocOperands, Map) ||
        Touches(Image.CodeAddressRelocOperands, Map))
      return false;
    auto IsaOverlap = [&](va_t Slot) {
      return Slot <= Address ? Address - Slot < 8 : Slot - Address < 8;
    };
    for (const auto &R : Image.Relocations)
      if (IsaOverlap(R.Address))
        return false;
    for (const auto &R : Image.BaseRelocations)
      if (IsaOverlap(R.Address))
        return false;
    for (const auto &Slot : Image.RuntimeCallablePointerSlots)
      if (IsaOverlap(Slot.SlotVA))
        return false;
    return true;
  }

  bool importedBoolean(va_t Slot, unsigned Depth) {
    if (Depth > 64 || Objects.size() + Active.size() >= 4096 ||
        !isImmutableImageImportSlot(Image, Slot))
      return false;
    const auto &Bind = Image.DyldBindSlots.at(Slot);
    const llvm::StringRef Import(Bind.Name);
    if ((Import != "___kCFBooleanTrue" && Import != "___kCFBooleanFalse") ||
        !darwinDeclaredSourceDataExport(Image.Arch, Import, Bind.Module) ||
        !charge(8))
      return false;
    ObjCConstantObject Object;
    Object.TheKind = ObjCConstantObject::Kind::ImportedBoolean;
    Object.ImportName = Import.str();
    const auto [Found, Added] = Objects.emplace(Slot, std::move(Object));
    if (!Added &&
        (Found->second.TheKind != ObjCConstantObject::Kind::ImportedBoolean ||
         Found->second.ImportName != Import))
      return false;
    Heights[Slot] = 0;
    return true;
  }

  bool edges(va_t Slot, uint64_t Count, std::vector<va_t> &Values,
             unsigned Depth) {
    if (Count > EdgesLeft || Count > BytesLeft / 8)
      return false;
    EdgesLeft -= Count;
    if (!charge(Count * 8))
      return false;
    if (!Count)
      return word(Slot) == 0;
    const auto Table = readImmutableImagePointer(Image, Slot);
    if (!Table || *Table % 8 || Count > (InvalidVA - *Table) / 8)
      return false;
    for (uint64_t I = 0; I < Count; ++I) {
      const auto Value = readImmutableImagePointer(Image, *Table + I * 8);
      if (Value) {
        if (!object(*Value, Depth + 1))
          return false;
        Values.push_back(*Value);
      } else {
        const va_t ImportSlot = *Table + I * 8;
        if (!importedBoolean(ImportSlot, Depth + 1))
          return false;
        Values.push_back(ImportSlot);
      }
    }
    return true;
  }

  static std::optional<std::string> key(const ObjCConstantObject &Object) {
    if (Object.TheKind != ObjCConstantObject::Kind::String)
      return std::nullopt;
    std::string Result;
    if (Object.String.UTF16) {
      if (!llvm::convertUTF16ToUTF8String(Object.String.Units, Result))
        return std::nullopt;
    } else {
      for (auto Unit : Object.String.Units)
        Result.push_back(static_cast<char>(Unit));
    }
    return Result;
  }

  bool object(va_t Address, unsigned Depth) {
    if (auto It = Heights.find(Address); It != Heights.end())
      return Objects.at(Address).TheKind !=
                 ObjCConstantObject::Kind::ImportedBoolean &&
             Depth <= 64 && It->second <= 64 - Depth;
    if (!Address || Depth > 64 || Objects.size() + Active.size() >= 4096 ||
        !Active.insert(Address).second)
      return false;
    ObjCConstantObject Object;
    using Kind = ObjCConstantObject::Kind;
    if (auto String = readObjCConstantString(Image, Address)) {
      if (!record(Address, 32, "__cfstring",
                  "___CFConstantStringClassReference") ||
          !word(Address + 8) || !word(Address + 24) ||
          !charge(32 + String->Units.size() * 2))
        return false;
      const auto Contents = readImmutableImagePointer(Image, Address + 16);
      if (!Contents || !readImmutableImageBytes(Image, *Contents,
                                                (String->Units.size() + 1) *
                                                    (String->UTF16 ? 2 : 1)))
        return false;
      Object.String = std::move(*String);
    } else if (record(Address, 24, "__objc_intobj",
                      "_OBJC_CLASS_$_NSConstantIntegerNumber")) {
      // Compiler-defined Darwin literal records, including the original
      // integer encoding and bit pattern, not guessed NSNumber conversions.
      // https://github.com/llvm/llvm-project/blob/main/clang/lib/CodeGen/CGObjCMac.cpp
      const auto Encoding = readImmutableImagePointer(Image, Address + 8);
      const auto Text = Encoding ? readImmutableImageBytes(Image, *Encoding, 2)
                                 : std::nullopt;
      const auto Bits = word(Address + 16);
      if (!Text || (*Text)[1] || !Bits || !charge(26))
        return false;
      const char Code = static_cast<char>((*Text)[0]);
      unsigned Width =
          Code == 'c' || Code == 'C'                                 ? 8
          : Code == 's' || Code == 'S'                               ? 16
          : Code == 'i' || Code == 'I'                               ? 32
          : Code == 'l' || Code == 'L' || Code == 'q' || Code == 'Q' ? 64
                                                                     : 0;
      if (!Width)
        return false;
      if (Width < 64) {
        const uint64_t Mask = (uint64_t(1) << Width) - 1;
        const bool Signed = Code == 'c' || Code == 's' || Code == 'i';
        const uint64_t Expected =
            (*Bits & Mask) |
            (Signed && (*Bits & (uint64_t(1) << (Width - 1))) ? ~Mask : 0);
        if (Expected != *Bits)
          return false;
      }
      Object.TheKind = Kind::Integer;
      Object.Encoding = Code;
      Object.Bits = *Bits;
    } else {
      const bool Array = record(Address, 24, "__objc_arrayobj",
                                "_OBJC_CLASS_$_NSConstantArray");
      const bool Dictionary =
          !Array && record(Address, 40, "__objc_dictobj",
                           "_OBJC_CLASS_$_NSConstantDictionary");
      if ((!Array && !Dictionary) || !charge(Array ? 24 : 40))
        return false;
      const auto Count = word(Address + (Array ? 8 : 16));
      if (!Count)
        return false;
      Object.TheKind = Array ? Kind::Array : Kind::Dictionary;
      if (Dictionary) {
        const auto Options = word(Address + 8);
        if (Options != 1 || !edges(Address + 24, *Count, Object.Keys, Depth))
          return false;
        Object.Options = *Options;
        std::optional<std::string> Previous;
        for (va_t Address : Object.Keys) {
          const auto Key = key(Objects.at(Address));
          if (!Key || (Previous && *Previous > *Key))
            return false;
          Previous = Key;
        }
      }
      if (!edges(Address + (Array ? 16 : 32), *Count, Object.Elements, Depth))
        return false;
    }
    // A cached subtree can be reached again through a longer path. Its full
    // height must still fit the root's depth budget, independent of edge order.
    unsigned Height = 0;
    for (const auto *Children : {&Object.Elements, &Object.Keys})
      for (va_t Child : *Children)
        Height = std::max(Height, Heights.at(Child) + 1);
    Heights.emplace(Address, Height);
    Active.erase(Address);
    Objects.emplace(Address, std::move(Object));
    return true;
  }

public:
  explicit Reader(const BinaryImage &Image) : Image(Image), Data(Image) {}
  std::optional<ObjCConstantObjectGraph> read(va_t Root) {
    if (!Data.supportsPlainObjectPointers() || !object(Root, 0))
      return std::nullopt;
    return std::move(Objects);
  }
};
} // namespace

std::optional<ObjCConstantObjectGraph>
readObjCConstantObjectGraph(const BinaryImage &Image, va_t Root) {
  return Reader(Image).read(Root);
}
} // namespace neverd
