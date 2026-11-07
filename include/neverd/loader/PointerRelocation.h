//===- PointerRelocation.h - Shared absolute-pointer provenance -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Normalizes container-specific full-width pointer relocations into the
/// format-neutral slot and target provenance carried by BinaryImage.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_POINTERRELOCATION_H
#define NEVERD_LOADER_POINTERRELOCATION_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

namespace neverd {

/// Whether storage is writable after loader relocation processing completes.
/// Mapping permissions and normalized loader protection own this decision;
/// a section name or a finer section flag cannot protect writable storage.
inline bool isRuntimeWritableAddress(const BinaryImage &Img, va_t Address) {
  // Unknown ownership is not evidence of immutability.
  return !Img.readOnlyAfterRelocations(Address, 1);
}

/// One full-width absolute pointer relocation: the slot that holds the
/// pointer, the target it names, and the address inside the section or
/// segment that owns the target (the target itself unless the relocation
/// names another, as a one-past address does).
struct AbsolutePointerRelocation {
  va_t SlotVA = 0;
  va_t TargetVA = 0;
  va_t TargetOwnerVA = InvalidVA;
};

/// What recording one absolute pointer relocation does to a BinaryImage.  It
/// depends only on where the slot and the target lie -- never on the
/// relocations recorded before -- so a batch can decide every relocation's
/// effect before applying any.
struct AbsolutePointerRelocationEffect {
  enum class Kind : uint8_t {
    /// Nothing: an import slot, an unmapped address, a target outside its
    /// owner, or a target that is neither code nor readable data.
    None,
    /// An operand in code that names code.
    CodeOperandToCode,
    /// A pointer to code stored in data.
    DataSlotToCode,
    /// An operand in code that names data.
    CodeOperandToData,
    /// A pointer to data stored in data.
    DataSlotToData,
    /// A data target, from a slot that is neither code nor readable data.
    DataTargetOnly,
  };

  Kind What = Kind::None;
  va_t SlotVA = 0;
  va_t TargetVA = 0;
  /// For a code target, the target normalized as a code address.
  va_t CodeTargetVA = 0;
  /// The start of the section or segment that owns the target.
  va_t TargetOwnerBegin = 0;
  /// A data target stays writable after relocation.
  bool TargetWritable = false;

  /// The operand a code slot records.
  RelocatedAddressField field(const BinaryImage &Img) const {
    return {TargetVA, TargetVA, static_cast<uint8_t>(Img.getPointerSize()),
            TargetOwnerBegin};
  }
};

/// Where a relocation's slot or its target lies, as recording the relocation
/// needs to know it.
struct PointerRelocationAddressClass {
  bool Mapped = false;
  bool Readable = false;
  bool Writable = false;
  bool Executable = false;
  bool HasOwnerRange = false;
  va_t OwnerBegin = 0;
  va_t OwnerEnd = 0;
};

/// Classify \p Addr for recording a relocation: whether it is mapped,
/// readable, writable after relocation and code, and the section (or, in no
/// section, the segment; in no segment, the image's own headers) that owns
/// it.
inline PointerRelocationAddressClass
classifyPointerRelocationAddress(const BinaryImage &Img, va_t Addr) {
  PointerRelocationAddressClass C;
  const Segment *Seg = Img.getSegmentFor(Addr);
  if (!Seg) {
    // The image's own headers are its read-only data whether or not the
    // loader maps them (BinaryImage::imageBaseOffset).
    if (Img.imageBaseOffset(Addr)) {
      C.Mapped = C.Readable = C.HasOwnerRange = true;
      C.OwnerBegin = Img.Base;
      C.OwnerEnd = Img.Base + Img.PEHeaderBytes;
    }
    return C;
  }
  C.Mapped = true;
  if (const Section *Sec = Img.getSectionFor(Addr)) {
    C.Readable = Sec->isReadable();
    C.Writable = isRuntimeWritableAddress(Img, Addr);
    // Linked Mach-O sections inherit their enclosing segment protection, so
    // __TEXT,__cstring appears executable in Section::Flags. Its instruction
    // attributes are the exact authority. ELF/COFF sections carry their own
    // execute bit and can likewise override a coarse executable load segment.
    C.Executable = Img.hasExecutableCodeOwnerAt(Addr);
    if (Sec->Size <= InvalidVA - Sec->VA) {
      C.HasOwnerRange = true;
      C.OwnerBegin = Sec->VA;
      C.OwnerEnd = Sec->VA + Sec->Size;
    }
  } else {
    C.Readable = Seg->isReadable();
    C.Writable = isRuntimeWritableAddress(Img, Addr);
    // Keep absolute-relocation provenance aligned with BinaryImage's
    // format-aware code classifier. In particular, a mapped header or
    // alignment gap is relocatable data identity when section metadata
    // exists.
    C.Executable = Img.hasExecutableCodeOwnerAt(Addr);
    if (Seg->Size <= InvalidVA - Seg->VA) {
      C.HasOwnerRange = true;
      C.OwnerBegin = Seg->VA;
      C.OwnerEnd = Seg->VA + Seg->Size;
    }
  }
  return C;
}

/// Decide what recording one full-width absolute pointer relocation does.
/// Container formats spell these differently (Mach-O rebases, PE base
/// relocations, ELF absolute/RELATIVE relocations), but downstream code needs
/// one invariant: a pointer stored in data is a relocatable slot, while the
/// same relocation embedded in code proves the materialized target address
/// without turning instruction bytes into a pointer table.
///
/// \p Classify classifies an address as classifyPointerRelocationAddress
/// does.
template <typename ClassifyFn>
AbsolutePointerRelocationEffect
decideAbsolutePointerRelocation(const BinaryImage &Img, va_t SlotVA,
                                va_t TargetVA, va_t TargetOwnerVA,
                                ClassifyFn &&Classify) {
  using Kind = AbsolutePointerRelocationEffect::Kind;
  AbsolutePointerRelocationEffect Effect;
  Effect.SlotVA = SlotVA;
  Effect.TargetVA = TargetVA;
  if (Img.ImportStorageSlots.count(SlotVA) ||
      Img.MachOAuthenticatedPointers.count(SlotVA) ||
      Img.ConflictingImportStorageSlots.count(SlotVA) ||
      Img.ImportPtrSlots.count(SlotVA) || Img.DyldBindSlots.count(SlotVA))
    return Effect;

  if (TargetOwnerVA == InvalidVA)
    TargetOwnerVA = TargetVA;

  const PointerRelocationAddressClass Slot = Classify(SlotVA);
  const PointerRelocationAddressClass Target = Classify(TargetOwnerVA);
  if (!Slot.Mapped || !Target.Mapped)
    return Effect;
  // The relocation symbol/section owns the semantic role; the resolved addend
  // may legally name its one-past data address, which has no mapped byte and
  // may numerically coincide with the next section.  Code targets must still
  // name an owned byte, while data targets admit that one-past endpoint.
  if (!Target.HasOwnerRange || TargetVA < Target.OwnerBegin ||
      (Target.Executable ? TargetVA >= Target.OwnerEnd
                         : TargetVA > Target.OwnerEnd))
    return Effect;
  Effect.TargetOwnerBegin = Target.OwnerBegin;

  if (Target.Executable) {
    if (Slot.Executable) {
      Effect.What = Kind::CodeOperandToCode;
      Effect.CodeTargetVA = normalizeCodeAddress(TargetVA, Img.Arch, Img.Mode);
    } else if (Slot.Readable) {
      Effect.What = Kind::DataSlotToCode;
    }
    return Effect;
  }

  if (!Target.Readable)
    return Effect;
  Effect.TargetWritable = Target.Writable;
  if (Slot.Executable)
    Effect.What = Kind::CodeOperandToData;
  else if (Slot.Readable)
    Effect.What = Kind::DataSlotToData;
  else
    Effect.What = Kind::DataTargetOnly;
  return Effect;
}

/// \ref decideAbsolutePointerRelocation with classifyPointerRelocationAddress.
inline AbsolutePointerRelocationEffect
decideAbsolutePointerRelocation(const BinaryImage &Img, va_t SlotVA,
                                va_t TargetVA, va_t TargetOwnerVA = InvalidVA) {
  return decideAbsolutePointerRelocation(
      Img, SlotVA, TargetVA, TargetOwnerVA,
      [&](va_t Addr) { return classifyPointerRelocationAddress(Img, Addr); });
}

/// Apply \p Effect to \p Img, and say whether it changed anything.
inline bool
applyAbsolutePointerRelocation(BinaryImage &Img,
                               const AbsolutePointerRelocationEffect &Effect) {
  using Kind = AbsolutePointerRelocationEffect::Kind;
  const va_t SlotVA = Effect.SlotVA;
  bool Changed = false;
  auto RecordOperand = [&](auto &Occurrences) {
    const RelocatedAddressField Field = Effect.field(Img);
    auto It = Occurrences.find(SlotVA);
    if (It == Occurrences.end() ||
        It->second.EncodedValue != Field.EncodedValue ||
        It->second.TargetVA != Field.TargetVA ||
        It->second.Width != Field.Width ||
        It->second.TargetOwnerVA != Field.TargetOwnerVA) {
      Occurrences[SlotVA] = Field;
      Changed = true;
    }
  };
  switch (Effect.What) {
  case Kind::None:
    return false;
  case Kind::CodeOperandToCode:
    Changed |= Img.DataAddressRelocOperands.erase(SlotVA) != 0;
    Changed |= Img.CodeRefTargets.insert(Effect.CodeTargetVA).second;
    RecordOperand(Img.CodeAddressRelocOperands);
    return Changed;
  case Kind::DataSlotToCode:
    Changed |= Img.DataPtrRelocSlots.erase(SlotVA) != 0;
    Changed |= Img.DataPtrRelocTargetOwners.erase(SlotVA) != 0;
    Changed |= Img.CodePtrRelocSlots.insert(SlotVA).second;
    return Changed;
  case Kind::CodeOperandToData:
  case Kind::DataSlotToData:
  case Kind::DataTargetOnly:
    break;
  }

  if (Effect.TargetWritable)
    Changed |= Img.WritableRelocDataAddrs.insert(Effect.TargetVA).second;
  else
    Changed |= Img.RelocDataAddrs.insert(Effect.TargetVA).second;

  if (Effect.What == Kind::CodeOperandToData) {
    Changed |= Img.CodeAddressRelocOperands.erase(SlotVA) != 0;
    RecordOperand(Img.DataAddressRelocOperands);
  } else if (Effect.What == Kind::DataSlotToData) {
    Changed |= Img.CodePtrRelocSlots.erase(SlotVA) != 0;
    Changed |= Img.DataPtrRelocSlots.insert(SlotVA).second;
    auto OwnerIt = Img.DataPtrRelocTargetOwners.find(SlotVA);
    if (OwnerIt == Img.DataPtrRelocTargetOwners.end() ||
        OwnerIt->second != Effect.TargetOwnerBegin) {
      Img.DataPtrRelocTargetOwners[SlotVA] = Effect.TargetOwnerBegin;
      Changed = true;
    }
  }
  return Changed;
}

/// Record the provenance carried by one full-width absolute pointer
/// relocation (see \ref decideAbsolutePointerRelocation), and say whether it
/// changed anything.
inline bool recordAbsolutePointerRelocation(BinaryImage &Img, va_t SlotVA,
                                            va_t TargetVA,
                                            va_t TargetOwnerVA = InvalidVA) {
  return applyAbsolutePointerRelocation(
      Img,
      decideAbsolutePointerRelocation(Img, SlotVA, TargetVA, TargetOwnerVA));
}

/// Record \p Relocations, leaving \p Img exactly as recording each of them in
/// turn with \ref recordAbsolutePointerRelocation does.  A table of tens of
/// thousands of relocations -- a PE's base relocations -- is recorded in
/// slot order, each entry inserted next to the one before it, rather than
/// looked up from the root of every map it joins.
void recordAbsolutePointerRelocations(
    BinaryImage &Img, llvm::ArrayRef<AbsolutePointerRelocation> Relocations);

/// Record again, as pointers to data, the relocated pointers stored in data
/// whose targets lie in data objects.  The loader takes a pointer into an
/// executable section for a pointer to code, while debug information read
/// after it can place the target in data: a kernel image keeps string
/// literals and tables in PAGE and INIT.  \p InData says whether an address
/// lies in a data object; a target that the image's own metadata gives to
/// code (a function symbol, runtime function record, known code range or the
/// entry point) stays code.  A slot stays data wherever it lies: a run of
/// pointers among code is a table (recordRelocatedPointerRunsInCode), never
/// an instruction's operand.  Returns how many pointers changed.
unsigned recordDataPointerTargets(BinaryImage &Img,
                                  llvm::function_ref<bool(va_t)> InData);

/// Record in Img.DataInCode the starts of \p Objects, data objects that debug
/// information places in sections the image gives to code, that the image's
/// own metadata leaves to data: no function symbol or its extent, runtime
/// function record, known code range or the entry point covers one, and no
/// code ends at one (an assembly routine's end label is no data).  Returns
/// how many it recorded.
unsigned recordDataInCode(BinaryImage &Img, llvm::ArrayRef<va_t> Objects);

} // namespace neverd

#endif // NEVERD_LOADER_POINTERRELOCATION_H
