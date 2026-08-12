//===- ARMEHABITests.cpp - ARM EHABI unwinding table tests ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// Coverage for the exception model 32-bit ARM uses instead of DWARF: a sorted
/// `.ARM.exidx` index over the whole image, the `.ARM.extab` descriptors it
/// points into, and the Itanium language-specific data area a C++ frame keeps
/// appended to its descriptor rather than in a section of its own.
///
/// The tables are assembled here byte by byte.  A real toolchain emits three
/// of the four index shapes and one of the three type-table conventions, so a
/// fixture would leave the rest of the encoding -- including everything
/// malformed -- unreachable.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/ELF/ARMEHABI.h"

#include "llvm/BinaryFormat/ELF.h"

#include <cstring>

using namespace neverd;
using namespace neverd::arm_ehabi;

namespace {

//===----------------------------------------------------------------------===//
// Image and table construction
//===----------------------------------------------------------------------===//

constexpr va_t kTextVA = 0x1000;
constexpr uint64_t kTextSize = 0x1000;
constexpr va_t kTableVA = 0x3000;
constexpr uint64_t kTableSize = 0x1000;
constexpr va_t kDataVA = 0x5000;
constexpr uint64_t kDataSize = 0x1000;

/// Where each table lands inside the readable segment at \ref kTableVA.
constexpr va_t kExTabVA = kTableVA;
constexpr va_t kExIdxVA = kTableVA + 0x800;

/// A `std::type_info` and the mangled name behind it.
constexpr va_t kTypeInfoVA = kDataVA + 0x100;
constexpr va_t kTypeNameVA = kDataVA + 0x200;
constexpr va_t kTypeCellVA = kDataVA + 0x300;
constexpr const char *kTypeName = "15CxxProbeError";

/// The personality routine a generic entry names.  Placed in code because that
/// is where one lives -- a PLT veneer on a dynamically linked image, the
/// routine itself on a static one.
constexpr va_t kPersonalityVA = kTextVA + 0x800;

class ByteBuilder {
public:
  void u8(uint8_t V) { Bytes.push_back(V); }
  void u32(uint32_t V) {
    for (unsigned I = 0; I < 4; ++I)
      Bytes.push_back(static_cast<uint8_t>((V >> (I * 8)) & 0xFF));
  }
  void uleb(uint64_t V) {
    do {
      uint8_t Byte = V & 0x7F;
      V >>= 7;
      if (V)
        Byte |= 0x80;
      Bytes.push_back(Byte);
    } while (V);
  }
  void str(const char *S) {
    while (*S)
      Bytes.push_back(static_cast<uint8_t>(*S++));
    Bytes.push_back(0);
  }
  void pad(size_t Count, uint8_t Fill = 0) {
    Bytes.insert(Bytes.end(), Count, Fill);
  }
  size_t size() const { return Bytes.size(); }
  const std::vector<uint8_t> &data() const { return Bytes; }

private:
  std::vector<uint8_t> Bytes;
};

/// Encode a `prel31`: the 31-bit signed distance from the word that holds it
/// to \p Target.
uint32_t prel31(va_t Target, va_t FieldVA) {
  return static_cast<uint32_t>(
      (static_cast<uint64_t>(Target) - static_cast<uint64_t>(FieldVA)) &
      0x7FFFFFFFull);
}

BinaryImage makeARMImage() {
  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Format = BinaryFormat::ELF;
  Img.Bits = Bitness::Bits32;
  Img.Base = kTextVA;
  Img.Entry = kTextVA;

  Segment Text;
  Text.Name = ".text";
  Text.VA = kTextVA;
  Text.Size = kTextSize;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(kTextSize, 0x00);
  Img.Segments.push_back(std::move(Text));

  Segment Tables;
  Tables.Name = ".rodata";
  Tables.VA = kTableVA;
  Tables.Size = kTableSize;
  Tables.Flags = SegmentFlags::Readable;
  Tables.Data.assign(kTableSize, 0);
  Img.Segments.push_back(std::move(Tables));

  Segment Data;
  Data.Name = ".data.rel.ro";
  Data.VA = kDataVA;
  Data.Size = kDataSize;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.assign(kDataSize, 0);
  Img.Segments.push_back(std::move(Data));

  // The last index entry's function runs to the end of the section it starts
  // in, so a text section has to exist for that extent to be recoverable.
  Section Text2;
  Text2.Name = ".text";
  Text2.VA = kTextVA;
  Text2.Size = kTextSize;
  Text2.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Text2));
  return Img;
}

void write(BinaryImage &Img, va_t VA, const std::vector<uint8_t> &Bytes) {
  ASSERT_TRUE(Img.writeVA(VA, Bytes.data(), Bytes.size()));
}

/// Write the `std::type_info` the type tables below dispatch on, plus the cell
/// an image would reach it through if it were linked with `R_ARM_TARGET2` as
/// `R_ARM_GOT_PREL`.
void writeTypeInfo(BinaryImage &Img) {
  ByteBuilder Info;
  Info.u32(0x12345678); // vtable pointer, unread but never zero in practice
  Info.u32(static_cast<uint32_t>(kTypeNameVA));
  write(Img, kTypeInfoVA, Info.data());

  ByteBuilder Name;
  Name.str(kTypeName);
  write(Img, kTypeNameVA, Name.data());

  ByteBuilder Cell;
  Cell.u32(static_cast<uint32_t>(kTypeInfoVA));
  write(Img, kTypeCellVA, Cell.data());
}

/// A `.gcc_except_table` with one region that catches a type and one that only
/// cleans up.
///
/// Twenty bytes, laid out exactly as a producer emits them: a header whose
/// type-table offset is measured from the byte after itself, a call-site table
/// whose entries are displacements from the landing-pad base, the action chain
/// the call sites name, and one type-table slot below the base the header
/// pointed at.
std::vector<uint8_t> buildLSDA(uint8_t TypeTableEncoding, uint32_t SlotValue) {
  ByteBuilder B;
  B.u8(0xFF);               // landing-pad base omitted: the function start
  B.u8(TypeTableEncoding);  //
  B.uleb(17);               // type-table base, measured from the next byte
  B.u8(0x01);               // call sites are ULEB128 displacements
  B.u8(8);                  // call-site table length
  B.uleb(0x10);             // region one: [+0x10, +0x18)
  B.uleb(0x08);             //
  B.uleb(0x40);             // landing pad at +0x40
  B.uleb(1);                // first action record, 1-based
  B.uleb(0x18);             // region two: [+0x18, +0x38), no landing pad
  B.uleb(0x20);             //
  B.uleb(0x00);             //
  B.uleb(0x00);             //
  B.u8(0x01);               // action: catch on type-table slot 1
  B.u8(0x00);               // end of the chain
  B.pad(1);                 // to the word the type table grows down from
  B.u32(SlotValue);
  return B.data();
}

/// Assembles an index and installs it as `.ARM.exidx`.
class IndexBuilder {
public:
  void cantUnwind(va_t Function) { add(Function, 1); }
  void inlineCompact(va_t Function, uint32_t Descriptor) {
    add(Function, Descriptor);
  }
  void tableRef(va_t Function, va_t Table) {
    const va_t EntryVA = kExIdxVA + B.size();
    B.u32(prel31(Function, EntryVA));
    B.u32(prel31(Table, EntryVA + 4));
  }
  void install(BinaryImage &Img) const {
    Section Sec;
    Sec.Name = ".ARM.exidx";
    Sec.VA = kExIdxVA;
    Sec.Size = B.size();
    Sec.Data = B.data();
    Img.Sections.push_back(std::move(Sec));
  }

private:
  void add(va_t Function, uint32_t Word) {
    const va_t EntryVA = kExIdxVA + B.size();
    B.u32(prel31(Function, EntryVA));
    B.u32(Word);
  }
  ByteBuilder B;
};

/// A `.ARM.extab` entry naming its personality routine by address, which is
/// the only form that can carry language data.
///
/// \p Word1 holds the count of further opcode words in its top byte and the
/// first three opcode bytes below it, exactly as the ABI packs them.
std::vector<uint8_t> buildGenericEntry(va_t TableVA, va_t PersonalityVA,
                                       uint32_t Word1,
                                       const std::vector<uint32_t> &ExtraWords,
                                       const std::vector<uint8_t> &HandlerData) {
  ByteBuilder B;
  B.u32(prel31(PersonalityVA, TableVA));
  B.u32(Word1);
  for (uint32_t Word : ExtraWords)
    B.u32(Word);
  for (uint8_t Byte : HandlerData)
    B.u8(Byte);
  return B.data();
}

const ExceptionFunction *frameAt(const BinaryImage &Img, va_t Address) {
  for (const ExceptionFunction &F : Img.ExceptionMetadata.Functions)
    if (F.CodeRange.Begin == Address)
      return &F;
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Index shapes
//===----------------------------------------------------------------------===//

TEST(ARMEHABI, ReadsEveryShapeAnIndexEntryTakes) {
  BinaryImage Img = makeARMImage();
  // Personality routine 0 with three `finish` bytes, which is what the linker
  // leaves for a frame that saves nothing.
  const uint32_t InlineDescriptor = 0x80B0B0B0u;
  write(Img, kExTabVA,
        buildGenericEntry(kExTabVA, kPersonalityVA, 0x00B0B0B0u, {}, {}));
  // Personality routine 1: one further opcode word, and the descriptor list
  // EHABI defines for it, here empty.
  ByteBuilder Compact;
  Compact.u32(0x8101B0B0u);
  Compact.u32(0xB0B0B0B0u);
  Compact.u32(0x00000000u);
  write(Img, kExTabVA + 0x100, Compact.data());

  IndexBuilder Index;
  Index.cantUnwind(kTextVA + 0x000);
  Index.inlineCompact(kTextVA + 0x100, InlineDescriptor);
  Index.tableRef(kTextVA + 0x200, kExTabVA);
  Index.tableRef(kTextVA + 0x300, kExTabVA + 0x100);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  const ExceptionInfo &EH = Img.ExceptionMetadata;
  ASSERT_EQ(EH.Functions.size(), 4u);
  EXPECT_TRUE(EH.hasModel(ExceptionModel::ARMEHABI));
  EXPECT_EQ(EH.ParseStatus, ExceptionParseStatus::Complete);

  const ExceptionFunction *Refuses = frameAt(Img, kTextVA + 0x000);
  ASSERT_NE(Refuses, nullptr);
  ASSERT_TRUE(Refuses->ARMEHABI.has_value());
  EXPECT_EQ(Refuses->ARMEHABI->Kind, ARMEHABIEntryKind::CantUnwind);
  EXPECT_EQ(Refuses->Encoding, ExceptionEncoding::ARMEHABICantUnwind);
  EXPECT_EQ(Refuses->model(), ExceptionModel::ARMEHABI);

  const ExceptionFunction *Inline = frameAt(Img, kTextVA + 0x100);
  ASSERT_NE(Inline, nullptr);
  ASSERT_TRUE(Inline->ARMEHABI.has_value());
  EXPECT_EQ(Inline->ARMEHABI->Kind, ARMEHABIEntryKind::InlineCompact);
  EXPECT_EQ(Inline->Encoding, ExceptionEncoding::ARMEHABIInline);
  EXPECT_EQ(Inline->Personality, ExceptionPersonality::AeabiUnwindCppPr0);
  // Nothing points anywhere: the index word is the whole descriptor.
  EXPECT_EQ(Inline->ARMEHABI->TableEntryVA, 0u);

  const ExceptionFunction *Generic = frameAt(Img, kTextVA + 0x200);
  ASSERT_NE(Generic, nullptr);
  ASSERT_TRUE(Generic->ARMEHABI.has_value());
  EXPECT_EQ(Generic->ARMEHABI->Kind, ARMEHABIEntryKind::Generic);
  EXPECT_EQ(Generic->Encoding, ExceptionEncoding::ARMEHABIGeneric);
  EXPECT_EQ(Generic->ARMEHABI->TableEntryVA, kExTabVA);
  EXPECT_EQ(Generic->PersonalityVA, kPersonalityVA);
  EXPECT_FALSE(Generic->ARMEHABI->PersonalityIndex.has_value());

  const ExceptionFunction *Compacted = frameAt(Img, kTextVA + 0x300);
  ASSERT_NE(Compacted, nullptr);
  ASSERT_TRUE(Compacted->ARMEHABI.has_value());
  EXPECT_EQ(Compacted->ARMEHABI->Kind, ARMEHABIEntryKind::Compact);
  EXPECT_EQ(Compacted->Encoding, ExceptionEncoding::ARMEHABICompact);
  EXPECT_EQ(Compacted->Personality, ExceptionPersonality::AeabiUnwindCppPr1);
  EXPECT_EQ(Compacted->ARMEHABI->ExtraWordCount, 1u);
  // An ARM-defined routine takes scope descriptors, not an LSDA, so nothing
  // may read a call-site table out of the words after its opcodes.
  EXPECT_FALSE(Compacted->Itanium.has_value());
}

TEST(ARMEHABI, TakesEachFunctionsExtentFromTheEntryAfterIt) {
  BinaryImage Img = makeARMImage();
  IndexBuilder Index;
  Index.cantUnwind(kTextVA + 0x000);
  Index.cantUnwind(kTextVA + 0x040);
  Index.cantUnwind(kTextVA + 0x0C0);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 3u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].CodeRange.End, kTextVA + 0x040);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[1].CodeRange.End, kTextVA + 0x0C0);
  // Nothing follows the last entry, so its extent is what the section it
  // starts in can hold.
  EXPECT_EQ(Img.ExceptionMetadata.Functions[2].CodeRange.End,
            kTextVA + kTextSize);
}

TEST(ARMEHABI, SeedsFunctionDiscoveryFromTheIndex) {
  BinaryImage Img = makeARMImage();
  IndexBuilder Index;
  Index.cantUnwind(kTextVA + 0x000);
  Index.cantUnwind(kTextVA + 0x040);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  // The index covers every function the linker placed, which is the strongest
  // evidence a stripped image of this target has about where they begin.
  ASSERT_EQ(Img.Symbols.size(), 2u);
  EXPECT_EQ(Img.Symbols[0].Addr, kTextVA + 0x000);
  EXPECT_TRUE(Img.Symbols[0].IsFunc);
  EXPECT_EQ(Img.Symbols[0].Size, 0x40u);
  EXPECT_EQ(Img.Symbols[1].Addr, kTextVA + 0x040);
}

TEST(ARMEHABI, SortsAnIndexThatArrivedOutOfOrder) {
  BinaryImage Img = makeARMImage();
  IndexBuilder Index;
  Index.cantUnwind(kTextVA + 0x080);
  Index.cantUnwind(kTextVA + 0x000);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  // An unwinder binary-searches the index, so one that is not sorted is one no
  // unwinder could use.  Recovering the extents anyway means saying so.
  EXPECT_EQ(Img.ExceptionMetadata.ParseStatus, ExceptionParseStatus::Partial);
  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 2u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].CodeRange.Begin, kTextVA);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].CodeRange.End, kTextVA + 0x080);
}

TEST(ARMEHABI, FindsAnIndexByTheSectionTypeTheABIReservedForIt) {
  BinaryImage Img = makeARMImage();
  IndexBuilder Index;
  Index.cantUnwind(kTextVA);
  Index.install(Img);
  // A name is a convention; the type is what ARM reserved, and an index under
  // an unexpected name is still an index.
  Img.Sections.back().Name = ".unexpected";
  Img.Sections.back().Type = llvm::ELF::SHT_ARM_EXIDX;

  parseARMEHABIExceptions(Img);

  EXPECT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
}

TEST(ARMEHABI, ReadsNoIndexOutOfAnObjectTheLinkerHasNotSeen) {
  BinaryImage Img = makeARMImage();
  Img.IsRelocatable = true;
  IndexBuilder Index;
  Index.cantUnwind(kTextVA);
  Index.cantUnwind(kTextVA + 0x40);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  // Every field is owed by a relocation that has not been applied, so each
  // entry's displacement reads as zero and names its own address.  Frames
  // built from that would land on whatever sits at the bottom of the
  // synthesized layout.
  EXPECT_TRUE(Img.ExceptionMetadata.Functions.empty());
  EXPECT_TRUE(Img.Symbols.empty());
}

TEST(ARMEHABI, LeavesAnImageOfAnotherMachineAlone) {
  BinaryImage Img = makeARMImage();
  Img.Arch = Arch::AArch64;
  IndexBuilder Index;
  Index.cantUnwind(kTextVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  // The section name is not reserved to this machine, and its words only mean
  // what they mean at this pointer size.
  EXPECT_TRUE(Img.ExceptionMetadata.Functions.empty());
  EXPECT_FALSE(Img.ExceptionMetadata.hasModel(ExceptionModel::ARMEHABI));
}

//===----------------------------------------------------------------------===//
// Malformed entries
//===----------------------------------------------------------------------===//

TEST(ARMEHABI, RefusesACompactEntryFromAnUndefinedVendor) {
  BinaryImage Img = makeARMImage();
  ByteBuilder Entry;
  Entry.u32(0x90B0B0B0u); // bits 30-28 select a vendor that does not exist
  write(Img, kExTabVA, Entry.data());

  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].ParseStatus,
            ExceptionParseStatus::Malformed);
  EXPECT_TRUE(Img.ExceptionMetadata.Functions[0].UnwindOperations.empty());
}

TEST(ARMEHABI, RefusesAPersonalityIndexTheABIDoesNotDefine) {
  BinaryImage Img = makeARMImage();
  ByteBuilder Entry;
  Entry.u32(0x8300B0B0u); // ARM defined routines 0, 1 and 2, and no others
  write(Img, kExTabVA, Entry.data());

  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].ParseStatus,
            ExceptionParseStatus::Malformed);
}

TEST(ARMEHABI, RefusesAnInlineEntryNamingARoutineThatCannotFitInline) {
  BinaryImage Img = makeARMImage();
  IndexBuilder Index;
  // Routine 1 needs a word count, and the index word has no field for one.
  Index.inlineCompact(kTextVA, 0x8101B0B0u);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].ParseStatus,
            ExceptionParseStatus::Malformed);
}

TEST(ARMEHABI, RefusesAnEntryDeclaringOpcodeWordsItDoesNotHave) {
  BinaryImage Img = makeARMImage();
  // Placed so that the words the count claims run past the end of the segment
  // rather than merely past the end of the entry.
  const va_t EdgeVA = kTableVA + kTableSize - 8;
  ByteBuilder Entry;
  Entry.u32(prel31(kPersonalityVA, EdgeVA));
  Entry.u32(0xFF000000u); // 255 further opcode words
  write(Img, EdgeVA, Entry.data());

  IndexBuilder Index;
  Index.tableRef(kTextVA, EdgeVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].ParseStatus,
            ExceptionParseStatus::Malformed);
}

//===----------------------------------------------------------------------===//
// Unwind opcodes
//===----------------------------------------------------------------------===//

TEST(ARMEHABI, DecodesTheFrameOpcodesAGenericEntryCarries) {
  BinaryImage Img = makeARMImage();
  // 04 9b 84 80 a8 b0 b0
  write(Img, kExTabVA,
        buildGenericEntry(kExTabVA, kPersonalityVA, 0x01049B84u,
                          {0x80A8B0B0u}, {}));
  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  ASSERT_EQ(F.UnwindOperations.size(), 5u);

  // Every operation is stated the way the prologue performed it, though the
  // opcodes describe undoing it.
  EXPECT_EQ(F.UnwindOperations[0].Kind, UnwindOperationKind::AllocateStack);
  EXPECT_EQ(F.UnwindOperations[0].StackOffset, 20u);

  EXPECT_EQ(F.UnwindOperations[1].Kind,
            UnwindOperationKind::SetStackPointerFromRegister);
  EXPECT_EQ(F.UnwindOperations[1].Register, 11u);

  // `1000iiii iiiiiiii` names r4 through r15 under a mask, and the two bits
  // set here are the frame pointer and the link register.
  EXPECT_EQ(F.UnwindOperations[2].Kind,
            UnwindOperationKind::SaveRegisterPairPreIndexed);
  EXPECT_EQ(F.UnwindOperations[2].RegisterClass,
            UnwindRegisterClass::GeneralPurpose);
  EXPECT_EQ(F.UnwindOperations[2].RegisterMask, (1u << 11) | (1u << 14));
  EXPECT_EQ(F.UnwindOperations[2].Register, 11u);
  EXPECT_EQ(F.UnwindOperations[2].StackOffset, 8u);

  // `10101nnn` names r4 upward plus the link register.
  EXPECT_EQ(F.UnwindOperations[3].RegisterMask, (1u << 4) | (1u << 14));
  EXPECT_EQ(F.UnwindOperations[3].Register, 4u);

  EXPECT_EQ(F.UnwindOperations[4].Kind, UnwindOperationKind::End);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(F.NativeUnwindBytes.size(), 7u);
}

TEST(ARMEHABI, DecodesTheWideAllocationAndFloatingPointOpcodes) {
  BinaryImage Img = makeARMImage();
  // 42 b2 01 b3 21 b0
  ByteBuilder Entry;
  Entry.u32(0x810142B2u);
  Entry.u32(0x01B321B0u);
  write(Img, kExTabVA, Entry.data());

  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  ASSERT_EQ(F.UnwindOperations.size(), 4u);

  // `01xxxxxx` moves the stack pointer the other way: the frame the prologue
  // left is smaller than the one it was entered with.
  EXPECT_EQ(F.UnwindOperations[0].Kind, UnwindOperationKind::DeallocateStack);
  EXPECT_EQ(F.UnwindOperations[0].StackOffset, 12u);

  // `10110010` carries a ULEB128 so that a frame larger than the six-bit form
  // can reach can still be described.
  EXPECT_EQ(F.UnwindOperations[1].Kind, UnwindOperationKind::AllocateStack);
  EXPECT_EQ(F.UnwindOperations[1].StackOffset, 0x208u);

  // `10110011 sssscccc` pops double-precision registers with the `FSTMFDX`
  // layout, which leaves a spare word above them.
  EXPECT_EQ(F.UnwindOperations[2].RegisterClass,
            UnwindRegisterClass::FloatingPoint);
  EXPECT_EQ(F.UnwindOperations[2].Register, 2u);
  EXPECT_EQ(F.UnwindOperations[2].RegisterMask, (1u << 2) | (1u << 3));
  EXPECT_EQ(F.UnwindOperations[2].StackOffset, 2u * 8u + 4u);

  EXPECT_EQ(F.UnwindOperations[3].Kind, UnwindOperationKind::End);
}

TEST(ARMEHABI, KeepsAnOpcodeItCannotModelRatherThanDroppingIt) {
  BinaryImage Img = makeARMImage();
  // c6 21 is an Intel Wireless MMX pop, which this decoder does not model.
  write(Img, kExTabVA,
        buildGenericEntry(kExTabVA, kPersonalityVA, 0x00C621B0u, {}, {}));
  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  ASSERT_EQ(F.UnwindOperations.size(), 2u);
  EXPECT_EQ(F.UnwindOperations[0].Kind, UnwindOperationKind::Opaque);
  EXPECT_EQ(F.UnwindOperations[0].OperandBytes,
            (std::vector<uint8_t>{0xC6, 0x21}));
  EXPECT_EQ(F.UnwindOperations[1].Kind, UnwindOperationKind::End);
}

//===----------------------------------------------------------------------===//
// Language data
//===----------------------------------------------------------------------===//

/// Build an image whose one frame carries an LSDA appended to its descriptor.
BinaryImage makeLSDAImage(uint8_t TypeTableEncoding, uint32_t SlotValue) {
  BinaryImage Img = makeARMImage();
  writeTypeInfo(Img);

  Symbol Personality;
  Personality.Name = "__gxx_personality_v0";
  Personality.Addr = kPersonalityVA;
  Personality.IsFunc = true;
  Img.Symbols.push_back(std::move(Personality));

  write(Img, kExTabVA,
        buildGenericEntry(kExTabVA, kPersonalityVA, 0x00B0B0B0u, {},
                          buildLSDA(TypeTableEncoding, SlotValue)));

  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);
  return Img;
}

/// The LSDA's first byte, which is where the type-table slot's own address is
/// measured from.  The header is five bytes and the call-site and action
/// tables are ten, leaving the slot one padding byte later.
constexpr va_t kLSDAVA = kExTabVA + 8;
constexpr va_t kTypeSlotVA = kLSDAVA + 16;

TEST(ARMEHABI, RecoversTheCallSiteGraphAppendedToAGenericEntry) {
  BinaryImage Img =
      makeLSDAImage(/*TypeTableEncoding=*/0x00,
                    /*SlotValue=*/static_cast<uint32_t>(kTypeInfoVA));
  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(F.Personality, ExceptionPersonality::GxxPersonalityV0);
  EXPECT_EQ(F.PersonalityName, "__gxx_personality_v0");
  // The language data has no section of its own: it begins where the unwind
  // opcodes stop.
  EXPECT_EQ(F.HandlerDataVA, kLSDAVA);

  ASSERT_TRUE(F.Itanium.has_value());
  const ItaniumEHInfo &LSDA = *F.Itanium;
  EXPECT_EQ(LSDA.LSDAVA, kLSDAVA);
  // The base is the function the index named, which is what makes every
  // displacement in the table resolvable at all.
  EXPECT_EQ(LSDA.LandingPadBase, kTextVA);
  ASSERT_EQ(LSDA.CallSites.size(), 2u);
  EXPECT_EQ(LSDA.CallSites[0].GuardedRange.Begin, kTextVA + 0x10);
  EXPECT_EQ(LSDA.CallSites[0].GuardedRange.End, kTextVA + 0x18);
  EXPECT_EQ(LSDA.CallSites[0].LandingPadVA, kTextVA + 0x40);
  EXPECT_EQ(LSDA.CallSites[1].LandingPadVA, 0u);

  ASSERT_EQ(LSDA.Actions.size(), 1u);
  EXPECT_TRUE(LSDA.Actions[0].isCatch());
  EXPECT_EQ(LSDA.Actions[0].TypeFilter, 1);
  EXPECT_FALSE(LSDA.isCleanupOnly());

  ASSERT_EQ(LSDA.TypeTable.size(), 1u);
  EXPECT_EQ(LSDA.TypeTable[0].TypeInfoVA, kTypeInfoVA);
  EXPECT_EQ(LSDA.TypeTable[0].TypeName, kTypeName);
  EXPECT_FALSE(LSDA.TypeTable[0].IsCatchAll);
  ASSERT_TRUE(F.ARMEHABI.has_value());
  EXPECT_EQ(F.ARMEHABI->TypeTableConvention,
            ARMTypeTableConvention::Absolute);
}

// The header byte is not evidence.  EHABI hands a type-table slot to
// `_Unwind_decode_typeinfo_ptr`, which applies the platform's `R_ARM_TARGET2`
// convention and never reads the byte, so a producer is free to leave a bare
// `DW_EH_PE_absptr` there while emitting a relocation that means something
// else entirely -- and Clang does.  A decoder that believes the byte reports a
// displacement as though it were the address of a type.
TEST(ARMEHABI, ReadsATypeTableTheWayTheImageWasLinkedRatherThanAsDeclared) {
  BinaryImage Img =
      makeLSDAImage(/*TypeTableEncoding=*/0x00,
                    /*SlotValue=*/static_cast<uint32_t>(kTypeCellVA -
                                                        kTypeSlotVA));
  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  ASSERT_TRUE(F.Itanium.has_value());
  ASSERT_EQ(F.Itanium->TypeTable.size(), 1u);
  // Declared as though the slot held the pointer; linked so that it holds the
  // distance to a cell that does.
  EXPECT_EQ(F.Itanium->TypeTableEncoding, 0x00);
  EXPECT_EQ(F.Itanium->TypeTable[0].TypeInfoVA, kTypeInfoVA);
  EXPECT_EQ(F.Itanium->TypeTable[0].TypeInfoSlotVA, kTypeCellVA);
  EXPECT_EQ(F.Itanium->TypeTable[0].TypeName, kTypeName);
  ASSERT_TRUE(F.ARMEHABI.has_value());
  EXPECT_EQ(F.ARMEHABI->TypeTableConvention,
            ARMTypeTableConvention::PCRelativeIndirect);
}

TEST(ARMEHABI, HonoursATypeTableEncodingTheHeaderDoesSpell) {
  // `DW_EH_PE_indirect | DW_EH_PE_pcrel | DW_EH_PE_absptr` is what GCC writes
  // for the same relocation Clang leaves bare, and a header that says
  // something is left to say it.
  BinaryImage Img =
      makeLSDAImage(/*TypeTableEncoding=*/0x90,
                    /*SlotValue=*/static_cast<uint32_t>(kTypeCellVA -
                                                        kTypeSlotVA));
  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  ASSERT_TRUE(F.Itanium.has_value());
  ASSERT_EQ(F.Itanium->TypeTable.size(), 1u);
  EXPECT_EQ(F.Itanium->TypeTable[0].TypeInfoVA, kTypeInfoVA);
  EXPECT_EQ(F.Itanium->TypeTable[0].TypeName, kTypeName);
}

// Frames ahead of the one that proves the convention are read again once it
// exists.  The first frame of a real image routinely catches a type defined in
// another shared object, which no reading of the slot can reach from here, so
// the record that settles the question is rarely the first one.
TEST(ARMEHABI, RereadsTheTypeTablesDecodedBeforeTheConventionWasProven) {
  BinaryImage Img = makeARMImage();
  writeTypeInfo(Img);

  Symbol Personality;
  Personality.Name = "__gxx_personality_v0";
  Personality.Addr = kPersonalityVA;
  Personality.IsFunc = true;
  Img.Symbols.push_back(std::move(Personality));

  // The first frame's cell is empty in the file, as a position-independent
  // image leaves every cell the loader will write.  Nothing about it can be
  // followed, so it settles nothing.
  const va_t FirstEntryVA = kExTabVA;
  const va_t FirstSlotVA = FirstEntryVA + 8 + 16;
  const va_t EmptyCellVA = kDataVA + 0x400;
  write(Img, FirstEntryVA,
        buildGenericEntry(FirstEntryVA, kPersonalityVA, 0x00B0B0B0u, {},
                          buildLSDA(0x00, static_cast<uint32_t>(
                                              EmptyCellVA - FirstSlotVA))));

  const va_t SecondEntryVA = kExTabVA + 0x100;
  const va_t SecondSlotVA = SecondEntryVA + 8 + 16;
  write(Img, SecondEntryVA,
        buildGenericEntry(SecondEntryVA, kPersonalityVA, 0x00B0B0B0u, {},
                          buildLSDA(0x00, static_cast<uint32_t>(
                                              kTypeCellVA - SecondSlotVA))));

  IndexBuilder Index;
  Index.tableRef(kTextVA + 0x000, FirstEntryVA);
  Index.tableRef(kTextVA + 0x100, SecondEntryVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 2u);
  const ExceptionFunction &First = Img.ExceptionMetadata.Functions[0];
  ASSERT_TRUE(First.Itanium.has_value());
  ASSERT_EQ(First.Itanium->TypeTable.size(), 1u);
  // Read again with the answer the second frame supplied: the cell holds
  // nothing yet, but it is the cell the catch dispatches through.
  EXPECT_EQ(First.Itanium->TypeTable[0].TypeInfoSlotVA, EmptyCellVA);
  EXPECT_FALSE(First.Itanium->TypeTable[0].IsCatchAll);
  ASSERT_TRUE(First.ARMEHABI.has_value());
  EXPECT_EQ(First.ARMEHABI->TypeTableConvention,
            ARMTypeTableConvention::PCRelativeIndirect);

  const ExceptionFunction &Second = Img.ExceptionMetadata.Functions[1];
  ASSERT_TRUE(Second.Itanium.has_value());
  ASSERT_EQ(Second.Itanium->TypeTable.size(), 1u);
  EXPECT_EQ(Second.Itanium->TypeTable[0].TypeName, kTypeName);
}

// A static link keeps no symbol for the personality routine it resolved, so
// the frames of a stripped static binary name a routine and nothing names it
// back.  Their tables are still there and still readable, and nothing else in
// such an image can recover a handler -- but a reading taken on no promise has
// to carry its own evidence.
TEST(ARMEHABI, ReadsATableBehindAnUnnamedPersonalityThatProvesItself) {
  BinaryImage Img = makeARMImage();
  writeTypeInfo(Img);
  write(Img, kExTabVA,
        buildGenericEntry(kExTabVA, kPersonalityVA, 0x00B0B0B0u, {},
                          buildLSDA(0x00, static_cast<uint32_t>(kTypeInfoVA))));
  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(F.Personality, ExceptionPersonality::None);
  ASSERT_TRUE(F.Itanium.has_value());
  EXPECT_EQ(F.Itanium->CallSites.size(), 2u);
  EXPECT_EQ(F.Itanium->TypeTable.size(), 1u);
}

TEST(ARMEHABI, InventsNoTableBehindAnUnnamedPersonalityFromBytesThatAreNotOne) {
  BinaryImage Img = makeARMImage();
  // Nothing follows the opcodes but the zero fill of the segment, which is
  // what an entry whose personality takes no data looks like.
  write(Img, kExTabVA,
        buildGenericEntry(kExTabVA, kPersonalityVA, 0x00B0B0B0u, {}, {}));
  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  EXPECT_FALSE(F.Itanium.has_value());
  EXPECT_EQ(F.HandlerDataVA, 0u);
  // Bytes that did not read as a table are not a fault in an image that never
  // claimed they were one; the frame's own unwinding decoded fine.
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(Img.ExceptionMetadata.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(ARMEHABI, ReadsNoLanguageDataForAnARMDefinedPersonality) {
  BinaryImage Img = makeARMImage();
  writeTypeInfo(Img);
  // The same bytes an LSDA would occupy, behind a routine that does not read
  // them as one.  EHABI gives personality routine 1 a scope-descriptor list,
  // and a decoder that reaches for a call-site table here invents one.
  ByteBuilder Entry;
  Entry.u32(0x8100B0B0u);
  write(Img, kExTabVA, Entry.data());
  write(Img, kExTabVA + 4,
        buildLSDA(0x00, static_cast<uint32_t>(kTypeInfoVA)));

  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(F.Personality, ExceptionPersonality::AeabiUnwindCppPr1);
  EXPECT_FALSE(F.Itanium.has_value());
  EXPECT_NE(F.ParseStatus, ExceptionParseStatus::Malformed);
}

TEST(ARMEHABI, KeepsTheDWARFTablesAnImageCarriesBesideItsIndex) {
  // `-fasynchronous-unwind-tables` leaves both, and each describes the same
  // frames from a different direction.  Neither reader may discard the other's
  // records.
  BinaryImage Img = makeARMImage();
  ExceptionFunction Existing;
  Existing.Encoding = ExceptionEncoding::DwarfFDE;
  Existing.CodeRange = {kTextVA, kTextVA + 0x40};
  Img.ExceptionMetadata.Functions.push_back(std::move(Existing));
  Img.ExceptionMetadata.addModel(ExceptionModel::Itanium);

  IndexBuilder Index;
  Index.cantUnwind(kTextVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 2u);
  EXPECT_TRUE(Img.ExceptionMetadata.hasModel(ExceptionModel::Itanium));
  EXPECT_TRUE(Img.ExceptionMetadata.hasModel(ExceptionModel::ARMEHABI));
}

} // namespace
