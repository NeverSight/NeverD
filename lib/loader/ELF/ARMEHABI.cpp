//===- ARMEHABI.cpp - ARM EHABI exception recovery driver -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/ELF/ARMEHABI.h"

#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/DwarfEH.h"
#include "neverd/loader/DWARF/LSDA.h"
#include "neverd/loader/LanguageRuntime.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <optional>

#define DEBUG_TYPE "neverd-arm-ehabi"

namespace neverd::arm_ehabi {
namespace {

using namespace dweh;

/// Every index entry is two words: a function address and its description.
constexpr uint64_t kIndexEntrySize = 8;

/// The reserved description meaning "this frame may not be unwound through".
constexpr uint32_t kCantUnwind = 1;

/// Set in a description word that carries its own descriptor rather than a
/// displacement to one.
constexpr uint32_t kCompactBit = 0x80000000u;

/// Bits 30-28 of a compact word select the vendor that defined the remaining
/// bits.  Only zero -- ARM's own -- has ever been defined.
constexpr uint32_t kCompactVendorMask = 0x70000000u;

/// Bits 27-24 of a compact word: which personality routine it selects.
constexpr uint32_t kCompactIndexShift = 24;
constexpr uint32_t kCompactIndexMask = 0xF;

/// Bits 23-16 hold the count of opcode words past the first, for the two
/// personality routines whose descriptors can need more than one word.
constexpr uint32_t kExtraWordShift = 16;
constexpr uint32_t kGenericExtraWordShift = 24;
constexpr uint32_t kExtraWordMask = 0xFF;

/// Ceiling on how far one entry's opcodes may run.  The count is eight bits,
/// so 255 extra words is what the encoding itself permits; stating it keeps
/// the arithmetic below obviously bounded rather than only provably so.
constexpr uint64_t kMaxEntryBytes = (2 + 255) * 4;

/// Ceiling on index entries read from one image.  A `.ARM.exidx` covers every
/// function, and the largest shipped ARM32 objects are far below this.
constexpr size_t kMaxIndexEntries = 1u << 22;

/// ARM register numbers the opcodes name.
constexpr uint16_t kFirstPoppedReg = 4;  // r4
constexpr uint16_t kLinkRegister = 14;   // r14, spelled `lr`
constexpr uint16_t kProgramCounter = 15; // r15, spelled `pc`

/// A slot is one word on this target, in every table EHABI defines.
constexpr uint64_t kWordSize = 4;

/// Resolve a `prel31` field: a 31-bit signed displacement from the address of
/// the word that holds it.  The result is taken modulo the address size, as
/// the ABI defines it and as every unwinder computes it.
va_t resolvePrel31(uint32_t Word, va_t FieldVA) {
  int32_t Displacement = static_cast<int32_t>(Word << 1) >> 1;
  return static_cast<va_t>(
      (static_cast<uint64_t>(FieldVA) + static_cast<uint64_t>(Displacement)) &
      0xFFFFFFFFull);
}

/// A section of an image, located by name and read through whichever of its
/// bytes the loader kept.
struct TableSection {
  const uint8_t *Data = nullptr;
  size_t Size = 0;
  va_t VA = 0;
};

/// Bytes of \p Sec, preferring the section's own copy and falling back to the
/// segment it was mapped into.  A section that survives only as a mapping
/// still has to be readable: an image loaded from memory has no section bytes
/// at all.
bool readSection(const BinaryImage &Img, const Section &Sec, TableSection &Out) {
  if (Sec.Size == 0)
    return false;
  if (!Sec.Data.empty()) {
    Out.Data = Sec.Data.data();
    Out.Size = std::min<size_t>(Sec.Data.size(), static_cast<size_t>(Sec.Size));
    Out.VA = Sec.VA;
    return Out.Size >= kIndexEntrySize;
  }
  if (Sec.VA == 0 || Sec.Size > std::numeric_limits<size_t>::max())
    return false;
  const uint8_t *Bytes = Img.readVA(Sec.VA, static_cast<size_t>(Sec.Size));
  if (!Bytes)
    return false;
  Out.Data = Bytes;
  Out.Size = static_cast<size_t>(Sec.Size);
  Out.VA = Sec.VA;
  return Out.Size >= kIndexEntrySize;
}

/// Every `.ARM.exidx` in the image, in address order.
///
/// There is usually one, but a link that was told to keep per-function
/// sections leaves a run of them; each is internally sorted and they are laid
/// out in address order, so reading them in that order yields one index.
///
/// The section type is checked alongside the name because ARM gave the index a
/// type of its own.  A name is a convention and an index under an unexpected
/// one is still an index; the type is what the ABI reserved.
std::vector<TableSection> findIndexSections(const BinaryImage &Img) {
  std::vector<TableSection> Result;
  for (const Section &Sec : Img.Sections) {
    llvm::StringRef Name(Sec.Name);
    if (Sec.Type != llvm::ELF::SHT_ARM_EXIDX &&
        Name != section_names::elf::ArmExIdx &&
        !Name.starts_with(section_names::elf::ArmExIdxPrefix))
      continue;
    TableSection Table;
    if (readSection(Img, Sec, Table))
      Result.push_back(Table);
  }
  std::sort(Result.begin(), Result.end(),
            [](const TableSection &A, const TableSection &B) {
              return A.VA < B.VA;
            });
  return Result;
}

/// Where the code an index covers stops.
///
/// An index entry states only where its function begins; the next entry's
/// function is where it ends.  The last entry has no next, so its extent is
/// bounded by the executable section it starts in -- which is what the
/// unwinder does too, having nothing else to go on.
va_t executableEndFor(const BinaryImage &Img, va_t Address) {
  va_t End = 0;
  for (const Section &Sec : Img.Sections) {
    if (Sec.Size == 0 || Address < Sec.VA || Address >= Sec.VA + Sec.Size)
      continue;
    const Segment *Seg = Img.getSegmentFor(Sec.VA);
    if (!Seg || !Seg->isExecutable())
      continue;
    End = static_cast<va_t>(Sec.VA + Sec.Size);
    break;
  }
  if (End != 0)
    return End;
  if (const Segment *Seg = Img.getSegmentFor(Address);
      Seg && Seg->isExecutable())
    return static_cast<va_t>(Seg->VA + Seg->Size);
  return 0;
}

//===----------------------------------------------------------------------===//
// Unwind opcodes
//===----------------------------------------------------------------------===//

/// Accumulates decoded operations and their position in the opcode stream.
class OpcodeBuilder {
public:
  explicit OpcodeBuilder(std::vector<UnwindOperation> &Out) : Operations(Out) {}

  UnwindOperation &add(UnwindOperationKind Kind, size_t Offset, size_t Bytes) {
    Operations.emplace_back();
    UnwindOperation &Op = Operations.back();
    Op.Kind = Kind;
    Op.CodeOffset = static_cast<uint32_t>(Offset);
    Op.SlotCount = static_cast<uint8_t>(Bytes);
    return Op;
  }

private:
  std::vector<UnwindOperation> &Operations;
};

bool isMaskableRegister(uint16_t Reg) { return Reg < 32; }

/// Record that \p Op restores every register \p Mask names, where bit \p I of
/// the mask stands for register \p FirstRegister + I.
///
/// Every EHABI register opcode is a pop, so all of them move the stack pointer
/// by as many words as they name.  The distinction the normalized kinds draw
/// is only between one register and several, which is what tells a consumer
/// whether \ref UnwindOperation::Register alone describes the operation.
void setPoppedRegisters(UnwindOperation &Op, UnwindRegisterClass Class,
                        uint16_t FirstRegister, uint32_t Mask,
                        uint64_t SlotBytes) {
  Op.RegisterClass = Class;
  uint16_t Lowest = 0;
  bool HaveLowest = false;
  unsigned Count = 0;
  for (unsigned Bit = 0; Bit < 32; ++Bit) {
    if ((Mask & (uint32_t(1) << Bit)) == 0)
      continue;
    const uint16_t Reg = static_cast<uint16_t>(FirstRegister + Bit);
    if (!isMaskableRegister(Reg))
      continue;
    Op.RegisterMask |= uint32_t(1) << Reg;
    ++Count;
    if (!HaveLowest) {
      Lowest = Reg;
      HaveLowest = true;
    }
  }
  Op.Register = Lowest;
  Op.StackOffset = Count * SlotBytes;
  Op.Kind = Count > 1 ? UnwindOperationKind::SaveRegisterPairPreIndexed
                      : UnwindOperationKind::SaveRegisterPreIndexed;
}

/// Mask of \p Count registers starting at bit zero.
uint32_t contiguousMask(unsigned Count) {
  return Count >= 32 ? 0xFFFFFFFFu : ((uint32_t(1) << Count) - 1);
}

/// Decode the EHABI unwind opcode stream in \p Bytes.
///
/// The opcodes describe unwinding -- restoring the caller's frame -- while
/// \ref UnwindOperation is defined in the saving direction, so a `pop` here
/// becomes a save and `vsp = vsp + N` becomes the allocation the prologue made
/// to put it there.  Both describe one frame; stating them the same way as
/// every other target is what lets a consumer read them without knowing which
/// table they came out of.
///
/// Returns false when the last instruction was cut short, which means the
/// entry's declared word count and its opcodes disagree.  Running out exactly
/// on an instruction boundary is not that: the encoding pads with `finish`
/// only where a word has slack left, so a sequence that fills its words ends
/// without one and is well formed.
bool decodeUnwindOpcodes(llvm::ArrayRef<uint8_t> Bytes,
                         std::vector<UnwindOperation> &Out,
                         bool &RefusesToUnwind) {
  OpcodeBuilder Builder(Out);
  size_t I = 0;
  const size_t N = Bytes.size();
  auto next = [&](uint8_t &Value) {
    if (I >= N)
      return false;
    Value = Bytes[I++];
    return true;
  };

  while (I < N) {
    const size_t Offset = I;
    uint8_t B0 = 0;
    if (!next(B0))
      break;

    if ((B0 & 0xC0) == 0x00) { // 00xxxxxx: vsp = vsp + (x << 2) + 4
      Builder.add(UnwindOperationKind::AllocateStack, Offset, 1).StackOffset =
          (uint64_t(B0 & 0x3F) << 2) + 4;
      continue;
    }
    if ((B0 & 0xC0) == 0x40) { // 01xxxxxx: vsp = vsp - (x << 2) - 4
      Builder.add(UnwindOperationKind::DeallocateStack, Offset, 1).StackOffset =
          (uint64_t(B0 & 0x3F) << 2) + 4;
      continue;
    }

    if ((B0 & 0xF0) == 0x80) { // 1000iiii iiiiiiii: pop r4-r15 under a mask
      uint8_t B1 = 0;
      if (!next(B1))
        return false;
      const uint32_t Mask = (uint32_t(B0 & 0x0F) << 8) | B1;
      if (Mask == 0) {
        // The one reserved encoding: an entry that says outright that the
        // frame may not be unwound through, spelled in opcodes rather than in
        // the index.
        RefusesToUnwind = true;
        UnwindOperation &Op = Builder.add(UnwindOperationKind::Opaque, Offset, 2);
        Op.OperandBytes = {B0, B1};
        continue;
      }
      UnwindOperation &Op = Builder.add(UnwindOperationKind::Opaque, Offset, 2);
      setPoppedRegisters(Op, UnwindRegisterClass::GeneralPurpose,
                         kFirstPoppedReg, Mask, kWordSize);
      continue;
    }

    if ((B0 & 0xF0) == 0x90) { // 1001nnnn: vsp = r[nnnn]
      const uint16_t Reg = B0 & 0x0F;
      // 13 is sp itself, which would say nothing, and 15 is pc, which cannot
      // hold a stack pointer.  Both are reserved as prefixes rather than as
      // register numbers.
      if (Reg == 13 || Reg == 15) {
        Builder.add(UnwindOperationKind::Opaque, Offset, 1).OperandBytes = {B0};
        continue;
      }
      UnwindOperation &Op = Builder.add(
          UnwindOperationKind::SetStackPointerFromRegister, Offset, 1);
      Op.RegisterClass = UnwindRegisterClass::GeneralPurpose;
      Op.Register = Reg;
      Op.RegisterMask = uint32_t(1) << Reg;
      continue;
    }

    if ((B0 & 0xF8) == 0xA0) { // 10100nnn: pop r4-r[4+nnn]
      UnwindOperation &Op = Builder.add(UnwindOperationKind::Opaque, Offset, 1);
      setPoppedRegisters(Op, UnwindRegisterClass::GeneralPurpose,
                         kFirstPoppedReg, contiguousMask((B0 & 0x07) + 1),
                         kWordSize);
      continue;
    }
    if ((B0 & 0xF8) == 0xA8) { // 10101nnn: pop r4-r[4+nnn], r14
      const uint32_t Mask = contiguousMask((B0 & 0x07) + 1) |
                            (uint32_t(1) << (kLinkRegister - kFirstPoppedReg));
      UnwindOperation &Op = Builder.add(UnwindOperationKind::Opaque, Offset, 1);
      setPoppedRegisters(Op, UnwindRegisterClass::GeneralPurpose,
                         kFirstPoppedReg, Mask, kWordSize);
      continue;
    }

    if (B0 == 0xB0) { // finish
      Builder.add(UnwindOperationKind::End, Offset, 1);
      return true;
    }

    if (B0 == 0xB1) { // 10110001 0000iiii: pop r0-r3 under a mask
      uint8_t B1 = 0;
      if (!next(B1))
        return false;
      if (B1 == 0 || (B1 & 0xF0) != 0) {
        UnwindOperation &Op =
            Builder.add(UnwindOperationKind::Opaque, Offset, 2);
        Op.OperandBytes = {B0, B1};
        continue;
      }
      UnwindOperation &Op = Builder.add(UnwindOperationKind::Opaque, Offset, 2);
      setPoppedRegisters(Op, UnwindRegisterClass::GeneralPurpose, 0, B1 & 0x0F,
                         kWordSize);
      continue;
    }

    if (B0 == 0xB2) { // 10110010 uleb128: vsp = vsp + 0x204 + (uleb << 2)
      uint64_t Value = 0;
      unsigned Shift = 0;
      bool Complete = false;
      const size_t Start = I;
      while (I < N) {
        const uint8_t Byte = Bytes[I++];
        if (Shift < 64)
          Value |= uint64_t(Byte & 0x7F) << Shift;
        Shift += 7;
        if ((Byte & 0x80) == 0) {
          Complete = true;
          break;
        }
      }
      if (!Complete)
        return false;
      Builder.add(UnwindOperationKind::AllocateStack, Offset,
                  1 + (I - Start))
          .StackOffset = 0x204 + (Value << 2);
      continue;
    }

    // 10110011 sssscccc and 10111nnn: pop VFP double registers with the
    // `FSTMFDX` layout, which writes a spare word after the registers.
    if (B0 == 0xB3 || (B0 & 0xF8) == 0xB8) {
      unsigned First = 8;
      unsigned Count = 0;
      size_t Bytes2 = 1;
      if (B0 == 0xB3) {
        uint8_t B1 = 0;
        if (!next(B1))
          return false;
        First = B1 >> 4;
        Count = (B1 & 0x0F) + 1;
        Bytes2 = 2;
      } else {
        Count = (B0 & 0x07) + 1;
      }
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::Opaque, Offset, Bytes2);
      setPoppedRegisters(Op, UnwindRegisterClass::FloatingPoint,
                         static_cast<uint16_t>(First), contiguousMask(Count),
                         8);
      Op.StackOffset += kWordSize;
      continue;
    }

    // 11001000/11001001 sssscccc and 11010nnn: the same pops with the `VPUSH`
    // layout, which writes no spare word.
    if (B0 == 0xC8 || B0 == 0xC9 || (B0 & 0xF8) == 0xD0) {
      unsigned First = 8;
      unsigned Count = 0;
      size_t Bytes2 = 1;
      if (B0 == 0xC8 || B0 == 0xC9) {
        uint8_t B1 = 0;
        if (!next(B1))
          return false;
        First = (B1 >> 4) + (B0 == 0xC8 ? 16u : 0u);
        Count = (B1 & 0x0F) + 1;
        Bytes2 = 2;
      } else {
        Count = (B0 & 0x07) + 1;
      }
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::Opaque, Offset, Bytes2);
      setPoppedRegisters(Op, UnwindRegisterClass::FloatingPoint,
                         static_cast<uint16_t>(First), contiguousMask(Count),
                         8);
      continue;
    }

    // 11000110 and 11000111 take a second byte; the rest of the Intel Wireless
    // MMX space and every spare encoding do not.  Both are kept verbatim: a
    // consumer that meets one is better served by the bytes than by a guess.
    if (B0 == 0xC6 || B0 == 0xC7) {
      uint8_t B1 = 0;
      if (!next(B1))
        return false;
      Builder.add(UnwindOperationKind::Opaque, Offset, 2).OperandBytes = {B0,
                                                                          B1};
      continue;
    }
    Builder.add(UnwindOperationKind::Opaque, Offset, 1).OperandBytes = {B0};
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Type-table convention
//===----------------------------------------------------------------------===//

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

/// The three readings EHABI's `R_ARM_TARGET2` can have been linked to mean,
/// as a DWARF encoding this decoder can hand to the LSDA reader.
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

/// Decide which reading of \p Info's type-table slots the image was linked
/// with, from the slots themselves.
///
/// EHABI leaves this to the platform and the C++ runtime resolves it without
/// consulting the LSDA header, so the header byte is not evidence.  What is
/// evidence is where each reading lands: exactly one of them reaches an object
/// that looks like RTTI, and the other two reach code or a pointer cell.
///
/// Returns \ref ARMTypeTableConvention::Unknown when no slot settled it, which
/// leaves the caller to keep looking at later records rather than commit the
/// whole image to a guess made from one.
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
    if ((Cell && namesTypeInfo(Img, static_cast<va_t>(readLE<uint32_t>(Cell)))) ||
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

//===----------------------------------------------------------------------===//
// Entry decoding
//===----------------------------------------------------------------------===//

/// A record whose type table was read before the image's convention had been
/// proven.
///
/// The convention holds for the whole image but nothing in the image states
/// it, so it is proven from the first record whose types this decoder can
/// reach -- and the records ahead of that one were already read against the
/// header's bare `absptr`.  A type defined in another shared object reaches
/// nothing from here and settles nothing, which is exactly the case that makes
/// the first record the wrong one to ask.  Reading those records again once
/// the answer exists keeps a frame's types from depending on where in the
/// index it happened to sit.
struct DeferredTypeTable {
  size_t FunctionIndex = 0;
  dwarf_eh::LSDAParseRequest Request;
  dwarf_eh::PointerBases Bases;
};

/// One index entry, resolved but not yet decoded.
struct IndexEntry {
  va_t EntryVA = 0;
  va_t FunctionVA = 0;
  uint32_t Word = 0;
};

/// What an `.ARM.extab` entry declared, before its language data is read.
struct TableEntry {
  ARMEHABIEntryKind Kind = ARMEHABIEntryKind::Generic;
  std::optional<uint8_t> PersonalityIndex;
  va_t PersonalityVA = 0;
  uint32_t ExtraWordCount = 0;
  /// Unwind opcodes, in the order the unwinder executes them.
  std::vector<uint8_t> Opcodes;
  /// First byte past the opcodes, which is where the personality routine's
  /// own data begins.
  va_t HandlerDataVA = 0;
};

/// Split the three or two opcode bytes a descriptor's first word carries.
void appendWordOpcodes(std::vector<uint8_t> &Out, uint32_t Word,
                       unsigned Count) {
  for (unsigned Byte = Count; Byte-- > 0;)
    Out.push_back(static_cast<uint8_t>((Word >> (Byte * 8)) & 0xFF));
}

/// Read the `.ARM.extab` entry at \p TableVA.
///
/// The first word decides everything else: its top bit chooses between a
/// personality named by index and one named by address, and the fields that
/// follow differ between the two.  Returns false when the entry could not be
/// read or declares a shape the ABI does not define.
bool decodeTableEntry(const BinaryImage &Img, va_t TableVA, TableEntry &Out,
                      std::string &Diagnostic) {
  const uint8_t *First = Img.readVA(TableVA, kWordSize);
  if (!First) {
    Diagnostic = "ARM EHABI table entry is not mapped readable data";
    return false;
  }
  const uint32_t Word0 = readLE<uint32_t>(First);

  unsigned HeaderWords = 0;
  if ((Word0 & kCompactBit) != 0) {
    if ((Word0 & kCompactVendorMask) != 0) {
      Diagnostic = "ARM EHABI compact entry names an undefined vendor";
      return false;
    }
    const uint8_t Index =
        static_cast<uint8_t>((Word0 >> kCompactIndexShift) & kCompactIndexMask);
    if (Index > 2) {
      Diagnostic = "ARM EHABI compact entry names an undefined personality "
                   "routine index";
      return false;
    }
    Out.Kind = ARMEHABIEntryKind::Compact;
    Out.PersonalityIndex = Index;
    if (Index == 0) {
      // Routine 0 has no word count: its whole descriptor is the three opcode
      // bytes beside the index.
      appendWordOpcodes(Out.Opcodes, Word0, 3);
      HeaderWords = 1;
    } else {
      Out.ExtraWordCount = (Word0 >> kExtraWordShift) & kExtraWordMask;
      appendWordOpcodes(Out.Opcodes, Word0, 2);
      HeaderWords = 1 + Out.ExtraWordCount;
    }
  } else {
    const uint8_t *Second = Img.readVA(TableVA + kWordSize, kWordSize);
    if (!Second) {
      Diagnostic = "ARM EHABI generic entry is truncated";
      return false;
    }
    const uint32_t Word1 = readLE<uint32_t>(Second);
    Out.Kind = ARMEHABIEntryKind::Generic;
    Out.PersonalityVA = resolvePrel31(Word0, TableVA);
    Out.ExtraWordCount = (Word1 >> kGenericExtraWordShift) & kExtraWordMask;
    appendWordOpcodes(Out.Opcodes, Word1, 3);
    HeaderWords = 2 + Out.ExtraWordCount;
  }

  const uint64_t HeaderBytes = uint64_t(HeaderWords) * kWordSize;
  if (HeaderBytes > kMaxEntryBytes || TableVA > InvalidVA - HeaderBytes) {
    Diagnostic = "ARM EHABI entry declares more opcode words than it can hold";
    return false;
  }
  // The words past the first carry four opcode bytes each, most significant
  // first, which is the order the unwinder executes them in.
  for (uint32_t Word = 0; Word < Out.ExtraWordCount; ++Word) {
    const va_t WordVA =
        static_cast<va_t>(TableVA + (HeaderWords - Out.ExtraWordCount + Word) *
                                        kWordSize);
    const uint8_t *Bytes = Img.readVA(WordVA, kWordSize);
    if (!Bytes) {
      Diagnostic = "ARM EHABI entry declares opcode words it does not have";
      return false;
    }
    appendWordOpcodes(Out.Opcodes, readLE<uint32_t>(Bytes), 4);
  }
  Out.HandlerDataVA = static_cast<va_t>(TableVA + HeaderBytes);
  return true;
}

/// True when a generic-model entry's personality reads its handler data as an
/// Itanium LSDA.
///
/// The three ARM-defined routines do not: they take the scope descriptors
/// EHABI defines for them, which carry no type and stop nothing.  Everything
/// else on this target is a language personality that shares the Itanium
/// language-specific data area, including one this decoder cannot name -- a
/// static link leaves the routine unnamed, and refusing to read the table
/// because of that would lose the handlers of every stripped static binary.
bool readsAnItaniumLSDA(ExceptionPersonality Personality) {
  switch (Personality) {
  case ExceptionPersonality::AeabiUnwindCppPr0:
  case ExceptionPersonality::AeabiUnwindCppPr1:
  case ExceptionPersonality::AeabiUnwindCppPr2:
  case ExceptionPersonality::GoRuntimeDispatch:
  case ExceptionPersonality::GoSEHTrampoline:
    return false;
  default:
    return true;
  }
}

} // namespace

void parseARMEHABIExceptions(BinaryImage &Img) {
  // `.ARM.exidx` is a processor-specific section type with a processor-
  // specific meaning.  Reading one out of an image built for another machine
  // would decode its words against the wrong pointer size.
  if (Img.Arch != Arch::ARM)
    return;
  // Every field of an index in an unlinked object is owed by a relocation the
  // link step has not applied, so each entry's `prel31` reads as a
  // displacement of zero and names its own address.  That is not an index of
  // anything, and reading it would put a frame on whatever happens to sit at
  // the bottom of the synthesized layout.
  if (Img.IsRelocatable)
    return;
  std::vector<TableSection> Indexes = findIndexSections(Img);
  if (Indexes.empty())
    return;

  // --- Collect the index -------------------------------------------------
  std::vector<IndexEntry> Entries;
  size_t Unreadable = 0;
  for (const TableSection &Index : Indexes) {
    const size_t Count = std::min<size_t>(Index.Size / kIndexEntrySize,
                                          kMaxIndexEntries - Entries.size());
    for (size_t I = 0; I < Count; ++I) {
      const uint8_t *Bytes = Index.Data + I * kIndexEntrySize;
      const uint32_t Word0 = readLE<uint32_t>(Bytes);
      const uint32_t Word1 = readLE<uint32_t>(Bytes + 4);
      const va_t EntryVA = static_cast<va_t>(Index.VA + I * kIndexEntrySize);
      // The first word is a `prel31`, so its top bit is not part of the
      // displacement.  An entry that sets it is not an index entry.
      if ((Word0 & kCompactBit) != 0) {
        ++Unreadable;
        continue;
      }
      IndexEntry Entry;
      Entry.EntryVA = EntryVA;
      // The linker clears the Thumb bit in the index because the table is
      // searched by program counter, but a hand-written or rewritten one may
      // not have; a function address that names an odd byte names none.
      Entry.FunctionVA = static_cast<va_t>(
          clearThumbBit(resolvePrel31(Word0, EntryVA)));
      Entry.Word = Word1;
      Entries.push_back(Entry);
    }
    if (Entries.size() >= kMaxIndexEntries)
      break;
  }
  if (Entries.empty())
    return;

  ExceptionInfo &Out = Img.ExceptionMetadata;
  Out.addModel(ExceptionModel::ARMEHABI);
  if (Unreadable != 0) {
    Out.ParseStatus =
        mergeExceptionParseStatus(Out.ParseStatus, ExceptionParseStatus::Partial);
    Out.Diagnostics.push_back(
        std::to_string(Unreadable) +
        " .ARM.exidx entries do not begin with a prel31 function address");
  }

  // The index is defined to be sorted, and an unwinder binary-searches it on
  // that basis.  Sorting a table that arrived out of order is what lets the
  // extent of each function be taken from the entry after it; saying so keeps
  // a reordered table from silently producing overlapping frames.
  const bool WasSorted = std::is_sorted(
      Entries.begin(), Entries.end(),
      [](const IndexEntry &A, const IndexEntry &B) {
        return A.FunctionVA < B.FunctionVA;
      });
  if (!WasSorted) {
    std::sort(Entries.begin(), Entries.end(),
              [](const IndexEntry &A, const IndexEntry &B) {
                return A.FunctionVA < B.FunctionVA;
              });
    Out.ParseStatus = mergeExceptionParseStatus(Out.ParseStatus,
                                                ExceptionParseStatus::Partial);
    Out.Diagnostics.emplace_back(
        ".ARM.exidx is not sorted by function address, so the extent each "
        "entry covers was recovered by sorting it");
  }

  dwarf_eh::PointerBases Bases;
  for (const Segment &Seg : Img.Segments)
    if (Seg.isExecutable() && (Bases.Text == 0 || Seg.VA < Bases.Text))
      Bases.Text = Seg.VA;
  for (const char *Name : {section_names::elf::GotPlt, section_names::elf::Got})
    if (const Section *Sec = Img.getSectionByName(Name)) {
      Bases.Data = Sec->VA;
      break;
    }

  auto SeenSymbols = Img.getSymbolAddresses();
  ARMTypeTableConvention Convention = ARMTypeTableConvention::Unknown;
  std::vector<DeferredTypeTable> Undecided;
  size_t Added = 0;

  for (size_t I = 0; I < Entries.size(); ++I) {
    const IndexEntry &Entry = Entries[I];
    ExceptionFunction F;
    F.Kind = RuntimeFunctionKind::Primary;
    F.UnwindInfoVA = Entry.EntryVA;

    const va_t End = I + 1 < Entries.size()
                         ? Entries[I + 1].FunctionVA
                         : executableEndFor(Img, Entry.FunctionVA);
    if (End > Entry.FunctionVA) {
      F.CodeRange = {Entry.FunctionVA, End};
    } else {
      F.ParseStatus = ExceptionParseStatus::Malformed;
      F.Diagnostics.emplace_back(
          ".ARM.exidx entry describes an empty or inverted code range");
    }

    const Segment *Seg = Img.getSegmentFor(Entry.FunctionVA);
    if (!Seg || !Seg->isExecutable()) {
      F.ParseStatus = mergeExceptionParseStatus(F.ParseStatus,
                                                ExceptionParseStatus::Partial);
      F.Diagnostics.emplace_back(
          ".ARM.exidx entry names a function outside executable data");
    }

    ARMEHABIInfo EHABI;
    EHABI.IndexEntryVA = Entry.EntryVA;
    EHABI.IndexWord = Entry.Word;

    if (Entry.Word == kCantUnwind) {
      EHABI.Kind = ARMEHABIEntryKind::CantUnwind;
      F.Encoding = ExceptionEncoding::ARMEHABICantUnwind;
    } else if ((Entry.Word & kCompactBit) != 0) {
      EHABI.Kind = ARMEHABIEntryKind::InlineCompact;
      F.Encoding = ExceptionEncoding::ARMEHABIInline;
      if ((Entry.Word & kCompactVendorMask) != 0 ||
          ((Entry.Word >> kCompactIndexShift) & kCompactIndexMask) != 0) {
        // Only routine 0 fits in the index word: the others need a word count
        // this form has no field for.
        F.ParseStatus = mergeExceptionParseStatus(
            F.ParseStatus, ExceptionParseStatus::Malformed);
        F.Diagnostics.emplace_back(
            ".ARM.exidx inline entry names a personality routine that cannot "
            "be encoded inline");
      } else {
        EHABI.PersonalityIndex = 0;
        F.Personality = ExceptionPersonality::AeabiUnwindCppPr0;
        F.PersonalityName = getExceptionPersonalityName(F.Personality);
        std::vector<uint8_t> Opcodes;
        appendWordOpcodes(Opcodes, Entry.Word, 3);
        F.NativeUnwindBytes = Opcodes;
        bool Refuses = false;
        if (!decodeUnwindOpcodes(Opcodes, F.UnwindOperations, Refuses))
          F.Diagnostics.emplace_back(
              "ARM EHABI inline opcodes end without a finish");
        if (Refuses)
          F.Diagnostics.emplace_back(
              "ARM EHABI opcodes refuse to unwind this frame");
      }
    } else {
      const va_t TableVA =
          resolvePrel31(Entry.Word, Entry.EntryVA + kWordSize);
      EHABI.TableEntryVA = TableVA;
      TableEntry Table;
      std::string Diagnostic;
      if (!decodeTableEntry(Img, TableVA, Table, Diagnostic)) {
        F.ParseStatus = mergeExceptionParseStatus(
            F.ParseStatus, ExceptionParseStatus::Malformed);
        F.Diagnostics.push_back(std::move(Diagnostic));
        F.Encoding = ExceptionEncoding::ARMEHABIGeneric;
      } else {
        EHABI.Kind = Table.Kind;
        EHABI.PersonalityIndex = Table.PersonalityIndex;
        EHABI.ExtraWordCount = Table.ExtraWordCount;
        F.Encoding = Table.Kind == ARMEHABIEntryKind::Compact
                         ? ExceptionEncoding::ARMEHABICompact
                         : ExceptionEncoding::ARMEHABIGeneric;
        F.NativeUnwindBytes = Table.Opcodes;
        bool Refuses = false;
        if (!decodeUnwindOpcodes(Table.Opcodes, F.UnwindOperations, Refuses))
          F.Diagnostics.emplace_back(
              "ARM EHABI opcodes end without a finish");
        if (Refuses)
          F.Diagnostics.emplace_back(
              "ARM EHABI opcodes refuse to unwind this frame");

        if (Table.PersonalityIndex) {
          F.Personality = static_cast<ExceptionPersonality>(
              static_cast<uint8_t>(ExceptionPersonality::AeabiUnwindCppPr0) +
              *Table.PersonalityIndex);
          F.PersonalityName = getExceptionPersonalityName(F.Personality);
        } else {
          F.PersonalityVA = Table.PersonalityVA;
          F.PersonalityName = resolveRoutineName(Img, Table.PersonalityVA, 0);
          F.Personality = classifyPersonalityName(F.PersonalityName);
          if (F.Personality == ExceptionPersonality::Unknown ||
              F.Personality == ExceptionPersonality::None)
            F.Diagnostics.emplace_back(
                "unnamed ARM EHABI personality routine");
        }

        // --- Language data ---------------------------------------------
        // EHABI gives the personality routine everything after the opcodes
        // and says nothing about what is there.  For the three ARM-defined
        // routines that is a scope-descriptor list with no types in it; for
        // every language personality it is an ordinary Itanium LSDA, which is
        // why an ARM image can hold a complete call-site graph and no
        // `.gcc_except_table` at all.
        F.HandlerDataVA = Table.HandlerDataVA;
        if (Img.readVA(Table.HandlerDataVA, 1) &&
            readsAnItaniumLSDA(F.Personality)) {
          dwarf_eh::LSDAParseRequest Req;
          Req.LSDAVA = Table.HandlerDataVA;
          Req.FunctionStart = F.CodeRange.Begin;
          Req.FunctionEnd = F.CodeRange.End;
          dwarf_eh::PointerBases LSDABases = Bases;
          LSDABases.Func = F.CodeRange.Begin;

          // A bare `absptr` type table is the one place EHABI leaves the
          // encoding to the platform.  Whichever reading the image was linked
          // with holds for all of it, so the first record that proves one
          // settles the question for every record after it -- and, below, for
          // the ones before it too.  The override is offered unconditionally
          // because the LSDA reader applies it to the bare form alone.
          if (Convention != ARMTypeTableConvention::Unknown &&
              Convention != ARMTypeTableConvention::Absolute)
            Req.TypeTableEncodingOverride = encodingFor(Convention);
          dwarf_eh::LSDAParseResult LSDA =
              dwarf_eh::parseLSDA(Img, Req, LSDABases);

          if (Convention == ARMTypeTableConvention::Unknown && LSDA.Info &&
              LSDA.Info->TypeTableEncoding == Absptr &&
              !LSDA.Info->TypeTable.empty()) {
            Convention = proveTypeTableConvention(Img, *LSDA.Info);
            if (Convention == ARMTypeTableConvention::Unknown)
              Undecided.push_back({Out.Functions.size(), Req, LSDABases});
            else if (Convention != ARMTypeTableConvention::Absolute) {
              Req.TypeTableEncodingOverride = encodingFor(Convention);
              LSDA = dwarf_eh::parseLSDA(Img, Req, LSDABases);
            }
          }
          EHABI.TypeTableConvention = Convention;

          // Only a named personality promised that a table is there.  An
          // unnamed one -- which is what a static link that kept no symbol
          // for its routine leaves behind -- promised nothing, and the bytes
          // after the opcodes are then as likely to belong to the next entry.
          // They are still worth reading, because a stripped static binary is
          // exactly the image whose handlers nothing else can recover, but a
          // reading taken on no promise has to carry its own evidence: a
          // table that decoded cleanly and that describes this frame.  By the
          // same token, bytes that did not read as a table are not a fault in
          // an image that never claimed they were one.
          const bool Promised =
              F.Personality != ExceptionPersonality::Unknown &&
              F.Personality != ExceptionPersonality::None;
          const bool Usable =
              LSDA.Info &&
              (Promised ? LSDA.ParseStatus != ExceptionParseStatus::Malformed
                        : LSDA.ParseStatus == ExceptionParseStatus::Complete &&
                              !LSDA.Info->CallSites.empty());
          if (Usable)
            F.Itanium = std::move(*LSDA.Info);
          if (Promised) {
            F.ParseStatus =
                mergeExceptionParseStatus(F.ParseStatus, LSDA.ParseStatus);
            for (std::string &Diag : LSDA.Diagnostics)
              F.Diagnostics.push_back(std::move(Diag));
          } else if (!Usable) {
            F.HandlerDataVA = 0;
          }
        }
      }
    }

    F.ARMEHABI = std::move(EHABI);
    Out.ParseStatus = mergeExceptionParseStatus(Out.ParseStatus, F.ParseStatus);

    if (F.CodeRange.isValid()) {
      Img.KnownCodeRanges.emplace_back(F.CodeRange.Begin, F.CodeRange.End);
      // The index is the most complete function table a stripped ARM image
      // has: the linker puts an entry in it for every function it placed,
      // including the ones that unwind through no opcodes at all.
      if (Seg && Seg->isExecutable() &&
          F.ParseStatus != ExceptionParseStatus::Malformed &&
          SeenSymbols.insert(F.CodeRange.Begin).second) {
        Img.Symbols.push_back(
            Symbol::makeFunc(F.CodeRange.Begin, F.CodeRange.size()));
        ++Added;
      }
    }
    Out.Functions.push_back(std::move(F));
  }

  if (Convention != ARMTypeTableConvention::Unknown &&
      Convention != ARMTypeTableConvention::Absolute) {
    for (DeferredTypeTable &Deferred : Undecided) {
      ExceptionFunction &F = Out.Functions[Deferred.FunctionIndex];
      if (F.ARMEHABI)
        F.ARMEHABI->TypeTableConvention = Convention;
      if (!F.Itanium)
        continue;
      Deferred.Request.TypeTableEncodingOverride = encodingFor(Convention);
      dwarf_eh::LSDAParseResult LSDA =
          dwarf_eh::parseLSDA(Img, Deferred.Request, Deferred.Bases);
      if (LSDA.Info && LSDA.ParseStatus != ExceptionParseStatus::Malformed)
        F.Itanium = std::move(*LSDA.Info);
    }
  }

  std::sort(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end());
  Img.KnownCodeRanges.erase(
      std::unique(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end()),
      Img.KnownCodeRanges.end());
  Out.rebuildIndex();

  LLVM_DEBUG(llvm::dbgs() << "arm-ehabi: normalized " << Entries.size()
                          << " index entries (" << Added << " new funcs, "
                          << getARMTypeTableConventionName(Convention)
                          << " type table)\n");
}

} // namespace neverd::arm_ehabi
