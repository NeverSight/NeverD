//===- ARMEHABITypeTable.cpp - ARM EHABI type-table convention ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decides which of the three readings EHABI's `R_ARM_TARGET2` relocation can
/// have been linked to mean -- absolute, PC-relative, or PC-relative indirect
/// -- for the type-table slots of an image's Itanium language data, and maps
/// that decision onto the DWARF encoding the LSDA reader takes.
///
//===----------------------------------------------------------------------===//

#include "ARMEHABIDetail.h"

#include "neverd/loader/DWARF/LSDA.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/DwarfEH.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"

#include <string>

namespace neverd::arm_ehabi {

using namespace dweh;

namespace {

/// Whether \p Name reads as the Itanium mangling of a type.
///
/// `std::type_info::__type_name` holds that mangling, which makes it the one
/// field of an RTTI object whose *shape* a reader can check.  Checking it
/// matters because the test below is applied to addresses that are not RTTI,
/// and the commonest of those is a pointer cell: two cells in a row produce a
/// "name" out of whatever the second one holds, and what that looks like is a
/// short run of raw pointer bytes.
bool readsAsMangledTypeName(llvm::StringRef Name) {
  if (Name.empty())
    return false;
  for (char C : Name)
    if (!llvm::isAlnum(C) && C != '_' && C != '$' && C != '.')
      return false;
  return true;
}

/// Whether \p Address is where an Itanium `std::type_info` was defined.
///
/// The object's own layout is the evidence, and the only evidence that
/// distinguishes the three readings: a vtable pointer followed by a pointer to
/// the mangled name.  A symbol or a relocation is deliberately not accepted in
/// its place -- the cell an image keeps for an imported type carries a
/// relocation naming that very type, so a name-based test cannot tell the
/// pointer to a type from the type.
bool namesTypeInfo(const BinaryImage &Img, va_t Address) {
  if (Address == 0)
    return false;
  const Segment *Seg = Img.getSegmentFor(Address);
  if (!Seg || Seg->isExecutable())
    return false;
  return readsAsMangledTypeName(dwarf_eh::readItaniumTypeName(Img, Address));
}

/// Whether the cell at \p Address is one this image binds to a type.
///
/// A position-independent object leaves every such cell empty in the file and
/// has the loader write it, so the pointer it will hold is not there to read
/// and no amount of following it reaches the type.  What is there is the
/// relocation, and the relocation names the type -- which is the same evidence
/// the type-table reader itself falls back on when it meets an unbound slot.
///
/// Only an `_ZTI` name counts.  Every RTTI object also carries a relocation of
/// its own, for the vtable pointer in its first word, and that one names the
/// vtable; accepting any name here would make the type indistinguishable from
/// the cell that points at it.
bool bindsATypeInfo(const BinaryImage &Img, va_t Address) {
  if (Address == 0)
    return false;
  const Segment *Seg = Img.getSegmentFor(Address);
  if (!Seg || Seg->isExecutable())
    return false;
  const std::string Bound = resolveRoutineName(Img, 0, Address);
  llvm::StringRef Name(Bound);
  // Itanium spells an RTTI symbol `_ZTI<type>`.  How many underscores precede
  // the mangling marker is a property of the container rather than of the
  // mangling: Mach-O adds one of its own to every C symbol.
  while (Name.consume_front("_"))
    ;
  return Name.starts_with("ZTI");
}

} // namespace

namespace detail {

uint8_t encodingFor(ARMTypeTableConvention Convention) {
  switch (Convention) {
  case ARMTypeTableConvention::PCRelative:
    return PCRel | Udata4;
  case ARMTypeTableConvention::PCRelativeIndirect:
    return Indirect | PCRel | Udata4;
  case ARMTypeTableConvention::Absolute:
  case ARMTypeTableConvention::Unknown:
    break;
  }
  return Absptr;
}

ARMTypeTableConvention proveTypeTableConvention(const BinaryImage &Img,
                                                const ItaniumEHInfo &Info) {
  if (Info.TypeTableVA == 0 || Info.TypeTable.empty())
    return ARMTypeTableConvention::Unknown;

  unsigned AbsoluteHits = 0;
  unsigned PCRelativeHits = 0;
  unsigned IndirectHits = 0;
  for (const ItaniumTypeEntry &Entry : Info.TypeTable) {
    // Parsed with the declared bare `absptr`, so the entry's address is the
    // word the slot holds and the slot's own address is recoverable from the
    // index the entry carries.
    const uint64_t Raw = Entry.TypeInfoVA;
    if (Raw == 0 || Entry.Index == 0 ||
        Entry.Index > Info.TypeTableVA / kWordSize)
      continue;
    const va_t SlotVA =
        static_cast<va_t>(Info.TypeTableVA - Entry.Index * kWordSize);
    const va_t Relative = static_cast<va_t>((SlotVA + Raw) & 0xFFFFFFFFull);

    if (namesTypeInfo(Img, static_cast<va_t>(Raw)))
      ++AbsoluteHits;
    if (namesTypeInfo(Img, Relative))
      ++PCRelativeHits;
    const uint8_t *Cell = Img.readVA(Relative, kWordSize);
    if ((Cell &&
         namesTypeInfo(Img, static_cast<va_t>(readLE<uint32_t>(Cell)))) ||
        bindsATypeInfo(Img, Relative))
      ++IndirectHits;
  }

  // A reading has to beat both of the others outright.  A tie is what a
  // record whose types are all defined in another shared object produces --
  // no reading reaches them from here -- and answering it either way would
  // commit the whole image on the strength of the record that had the least
  // to say.
  if (IndirectHits > AbsoluteHits && IndirectHits > PCRelativeHits)
    return ARMTypeTableConvention::PCRelativeIndirect;
  if (PCRelativeHits > AbsoluteHits && PCRelativeHits > IndirectHits)
    return ARMTypeTableConvention::PCRelative;
  if (AbsoluteHits > PCRelativeHits && AbsoluteHits > IndirectHits)
    return ARMTypeTableConvention::Absolute;
  return ARMTypeTableConvention::Unknown;
}

} // namespace detail
} // namespace neverd::arm_ehabi
