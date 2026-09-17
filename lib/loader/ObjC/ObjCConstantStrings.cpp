#include "neverd/loader/ObjC/ObjCConstantStrings.h"

#include "ObjCRuntimeData.h"

#include <algorithm>

namespace neverd {
namespace {
bool overlaps(va_t A, uint64_t AS, va_t B, uint64_t BS) {
  return AS && BS && (A <= B ? B - A < AS : A - B < BS);
}

bool uniqueMapping(const BinaryImage &Image, va_t Address, uint64_t Size) {
  unsigned Sections = 0, Segments = 0;
  for (const auto &S : Image.Sections)
    Sections += overlaps(Address, Size, S.VA, S.Size);
  for (const auto &S : Image.Segments)
    Segments += overlaps(Address, Size, S.VA, S.Size);
  return Sections == 1 && Segments == 1;
}
} // namespace

std::optional<ObjCConstantString>
readObjCConstantString(const BinaryImage &Image, va_t Address) {
  const objc::RuntimeData Data(Image);
  if (!Data.supportsPlainObjectPointers())
    return std::nullopt;
  const auto *Section = Image.getSectionFor(Address);
  if (!Section || Section->Name != "__cfstring" ||
      (Section->SegmentName != "__DATA" &&
       Section->SegmentName != "__DATA_CONST") ||
      Section->isExecutable() || Section->Size % 32 || Address % 8 ||
      (Address - Section->VA) % 32 || !Data.bytes(Address, 32) ||
      !uniqueMapping(Image, Address, 32))
    return std::nullopt;

  // Clang's Darwin constant-string ABI uses an imported class object followed
  // by flags, a content pointer and a signed pointer-sized unit count. Require
  // the actual loader binding, not a section name or a plausible first word.
  // https://github.com/llvm/llvm-project/blob/main/clang/lib/CodeGen/CodeGenModule.cpp
  const auto Isa = Image.DyldBindSlots.find(Address);
  constexpr llvm::StringLiteral ClassName("___CFConstantStringClassReference");
  if (Isa == Image.DyldBindSlots.end() || Isa->second.Name != ClassName ||
      Isa->second.Addend || Isa->second.WeakImport ||
      Image.ConflictingImportStorageSlots.count(Address))
    return std::nullopt;
  if (auto I = Image.ImportStorageSlots.find(Address);
      I != Image.ImportStorageSlots.end() &&
      (I->second.Name != ClassName || I->second.Addend))
    return std::nullopt;
  if (auto I = Image.ImportPtrSlots.find(Address);
      I != Image.ImportPtrSlots.end() && I->second != ClassName)
    return std::nullopt;

  const auto *Record = Data.bytes(Address, 32);
  const uint64_t Flags = llvm::support::endian::read64le(Record + 8);
  const uint64_t Length = llvm::support::endian::read64le(Record + 24);
  const auto Contents = Data.pointer(Address + 16);
  if ((Flags != 0x7c8 && Flags != 0x7d0) || Length > 1024 * 1024 || !Contents ||
      !*Contents)
    return std::nullopt;
  const bool UTF16 = Flags == 0x7d0;
  const uint64_t Width = UTF16 ? 2 : 1;
  const uint64_t Bytes = (Length + 1) * Width;
  const auto *PayloadSection = Image.getSectionFor(*Contents);
  const auto *PayloadSegment = Image.getSegmentFor(*Contents);
  const auto *Payload = Data.bytes(*Contents, Bytes);
  if (!PayloadSection || PayloadSection->isWritable() || !PayloadSegment ||
      PayloadSegment->isWritable() ||
      (PayloadSection->Type & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                               llvm::MachO::S_ATTR_SOME_INSTRUCTIONS)) ||
      !Payload || !uniqueMapping(Image, *Contents, Bytes) ||
      (UTF16 ? (*Contents % 2 || PayloadSection->Name != "__ustring" ||
                (PayloadSection->Type & llvm::MachO::SECTION_TYPE) !=
                    llvm::MachO::S_REGULAR)
             : (PayloadSection->Type & llvm::MachO::SECTION_TYPE) !=
                   llvm::MachO::S_CSTRING_LITERALS))
    return std::nullopt;

  // No fixup may reinterpret flags, length or character bytes. Only the two
  // pointer fields of the proven record can contain pointer relocations.
  auto InvalidSlot = [&](va_t Slot) {
    return overlaps(Slot, 8, *Contents, Bytes) ||
           (overlaps(Slot, 8, Address, 32) && Slot != Address &&
            Slot != Address + 16);
  };
  auto Touching = [&](const auto &Slots, auto Key, auto Invalid) {
    for (const auto &[Base, Size] :
         {std::pair{Address, uint64_t(32)}, std::pair{*Contents, Bytes}}) {
      auto It = Slots.lower_bound(Base >= 7 ? Base - 7 : 0);
      for (; It != Slots.end() && (Key(*It) < Base || Key(*It) - Base < Size);
           ++It)
        if (Invalid(Key(*It)))
          return true;
    }
    return false;
  };
  auto BadSet = [&](const auto &Slots) {
    return Touching(Slots, [](va_t Slot) { return Slot; }, InvalidSlot);
  };
  if (Touching(
          Image.CodePtrRelocSlots, [](va_t Slot) { return Slot; },
          [](va_t) { return true; }) ||
      Touching(
          Image.RelCodeRelocSlots, [](va_t Slot) { return Slot; },
          [](va_t) { return true; }) ||
      BadSet(Image.DataPtrRelocSlots) || BadSet(Image.RelDataPtrRelocSlots) ||
      BadSet(Image.MachOResolvedChainedPointerSlots) ||
      Touching(
          Image.ConflictingImportStorageSlots, [](va_t Slot) { return Slot; },
          [](va_t) { return true; }))
    return std::nullopt;
  auto BadImports = [&](const auto &Slots) {
    return Touching(
        Slots, [](const auto &Slot) { return Slot.first; },
        [&](va_t Slot) {
          return InvalidSlot(Slot) ||
                 (Slot != Address && overlaps(Slot, 8, Address, 32));
        });
  };
  if (BadImports(Image.DyldBindSlots) || BadImports(Image.ImportStorageSlots) ||
      BadImports(Image.ImportPtrSlots))
    return std::nullopt;
  for (const auto &R : Image.Relocations)
    if (InvalidSlot(R.Address))
      return std::nullopt;
  for (const auto &R : Image.BaseRelocations)
    if (InvalidSlot(R.Address))
      return std::nullopt;

  ObjCConstantString Result;
  Result.UTF16 = UTF16;
  Result.ContentsAddress = *Contents;
  for (uint64_t I = 0; I <= Length; ++I) {
    const uint16_t Unit =
        UTF16 ? llvm::support::endian::read16le(Payload + I * 2) : Payload[I];
    if ((!UTF16 && Unit > 127) || (I == Length && Unit))
      return std::nullopt;
    if (I != Length)
      Result.Units.push_back(Unit);
  }
  return Result;
}
} // namespace neverd
