//===- LanguageEHTests.cpp - Non-Windows exception model tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// Coverage for the exception models that are not the Windows table model:
/// DWARF call frame information, the Itanium language-specific data area, and
/// the language-runtime identification that decides how a landing pad should
/// be described.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/loader/DWARF/ItaniumEH.h"
#include "neverd/loader/DWARF/LSDA.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/Rust/RustEH.h"

#include <cstring>

using namespace neverd;
using namespace neverd::dwarf_eh;

namespace {

//===----------------------------------------------------------------------===//
// Byte-buffer builder
//===----------------------------------------------------------------------===//

/// Assembles the exact byte layouts the decoders read.  Building the tables
/// here rather than compiling a fixture keeps every encoding branch reachable,
/// including the malformed ones no real toolchain emits.
class ByteBuilder {
public:
  void u8(uint8_t V) { Bytes.push_back(V); }
  void u16(uint16_t V) { append(&V, sizeof(V)); }
  void u32(uint32_t V) { append(&V, sizeof(V)); }
  void u64(uint64_t V) { append(&V, sizeof(V)); }
  void i32(int32_t V) { append(&V, sizeof(V)); }

  void uleb(uint64_t V) {
    do {
      uint8_t Byte = V & 0x7f;
      V >>= 7;
      if (V)
        Byte |= 0x80;
      Bytes.push_back(Byte);
    } while (V);
  }

  void sleb(int64_t V) {
    bool More = true;
    while (More) {
      uint8_t Byte = V & 0x7f;
      V >>= 7;
      if ((V == 0 && !(Byte & 0x40)) || (V == -1 && (Byte & 0x40)))
        More = false;
      else
        Byte |= 0x80;
      Bytes.push_back(Byte);
    }
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
  std::vector<uint8_t> &data() { return Bytes; }

  /// Overwrite a previously reserved 32-bit slot.
  void patch32(size_t Offset, uint32_t Value) {
    std::memcpy(Bytes.data() + Offset, &Value, sizeof(Value));
  }

private:
  void append(const void *P, size_t N) {
    const auto *B = static_cast<const uint8_t *>(P);
    Bytes.insert(Bytes.end(), B, B + N);
  }
  std::vector<uint8_t> Bytes;
};

constexpr va_t kTextVA = 0x400000;
constexpr va_t kDataVA = 0x500000;

/// A minimal 64-bit image with one executable segment and one data segment.
BinaryImage makeImage(size_t TextSize = 0x1000, size_t DataSize = 0x1000) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::ELF;
  Img.Bits = Bitness::Bits64;
  Img.Base = kTextVA;
  Img.Entry = kTextVA;

  Segment Text;
  Text.Name = ".text";
  Text.VA = kTextVA;
  Text.Size = TextSize;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(TextSize, 0x90);
  Img.Segments.push_back(std::move(Text));

  Segment Data;
  Data.Name = ".data";
  Data.VA = kDataVA;
  Data.Size = DataSize;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.assign(DataSize, 0);
  Img.Segments.push_back(std::move(Data));
  return Img;
}

void writeData(BinaryImage &Img, va_t VA, const std::vector<uint8_t> &Bytes) {
  ASSERT_TRUE(Img.writeVA(VA, Bytes.data(), Bytes.size()));
}

/// Build a `.eh_frame` holding one CIE and one FDE.
///
/// \param Augmentation  CIE augmentation string.
/// \param LSDAOffset    When the CIE declares 'L', the FDE's LSDA pointer is
///                      emitted as this pcrel/sdata4 displacement.
struct FrameBytes {
  std::vector<uint8_t> Bytes;
  size_t CIEOffset = 0;
  size_t FDEOffset = 0;
};

FrameBytes buildSimpleFrame(va_t SectionVA, va_t FuncVA, uint64_t FuncSize,
                            const char *Augmentation, va_t PersonalityVA,
                            va_t LSDAVA) {
  FrameBytes Result;
  ByteBuilder B;

  const bool HasPersonality = std::strchr(Augmentation, 'P') != nullptr;
  const bool HasLSDA = std::strchr(Augmentation, 'L') != nullptr;

  // --- CIE ---
  Result.CIEOffset = 0;
  const size_t CIELengthSlot = B.size();
  B.u32(0);
  const size_t CIEBodyStart = B.size();
  B.u32(0); // CIE id
  B.u8(1);  // version
  B.str(Augmentation);
  B.uleb(1);  // code alignment factor
  B.sleb(-8); // data alignment factor
  B.u8(16);   // return address register

  if (Augmentation[0] == 'z') {
    ByteBuilder Aug;
    for (const char *P = Augmentation + 1; *P; ++P) {
      switch (*P) {
      case 'L':
        Aug.u8(0x1b); // DW_EH_PE_pcrel | DW_EH_PE_sdata4
        break;
      case 'P': {
        Aug.u8(0x00); // DW_EH_PE_absptr
        Aug.u64(PersonalityVA);
        break;
      }
      case 'R':
        Aug.u8(0x1b);
        break;
      default:
        break;
      }
    }
    B.uleb(Aug.size());
    for (uint8_t Byte : Aug.data())
      B.u8(Byte);
  }
  B.u8(0x0c); // DW_CFA_def_cfa
  B.uleb(7);  // rsp
  B.uleb(8);
  B.u8(0x90); // DW_CFA_offset | 16
  B.uleb(1);
  while ((B.size() - CIEBodyStart) % 8 != 0)
    B.u8(0); // DW_CFA_nop padding
  B.patch32(CIELengthSlot, static_cast<uint32_t>(B.size() - CIEBodyStart));

  // --- FDE ---
  Result.FDEOffset = B.size();
  const size_t FDELengthSlot = B.size();
  B.u32(0);
  const size_t FDEBodyStart = B.size();
  B.u32(static_cast<uint32_t>(FDEBodyStart - Result.CIEOffset)); // CIE pointer

  // initial_location, pcrel|sdata4 relative to its own address.
  const size_t InitialLocSlot = B.size();
  const va_t InitialLocVA = SectionVA + InitialLocSlot;
  B.i32(static_cast<int32_t>(static_cast<int64_t>(FuncVA) -
                             static_cast<int64_t>(InitialLocVA)));
  B.u32(static_cast<uint32_t>(FuncSize));

  if (Augmentation[0] == 'z') {
    ByteBuilder Aug;
    if (HasLSDA) {
      const va_t LSDASlotVA = SectionVA + B.size() + 1;
      Aug.i32(LSDAVA == 0
                  ? 0
                  : static_cast<int32_t>(static_cast<int64_t>(LSDAVA) -
                                         static_cast<int64_t>(LSDASlotVA)));
    }
    B.uleb(Aug.size());
    for (uint8_t Byte : Aug.data())
      B.u8(Byte);
  }
  (void)HasPersonality;

  B.u8(0x41); // DW_CFA_advance_loc 1
  B.u8(0x0e); // DW_CFA_def_cfa_offset
  B.uleb(16);
  while ((B.size() - FDEBodyStart) % 8 != 0)
    B.u8(0);
  B.patch32(FDELengthSlot, static_cast<uint32_t>(B.size() - FDEBodyStart));

  B.u32(0); // section terminator
  Result.Bytes = B.data();
  return Result;
}

//===----------------------------------------------------------------------===//
// LEB128
//===----------------------------------------------------------------------===//

TEST(DwarfEHPrimitives, ReadsCanonicalULEB128) {
  const uint8_t Bytes[] = {0xe5, 0x8e, 0x26};
  size_t Cursor = 0;
  uint64_t Value = 0;
  ASSERT_TRUE(readULEB128(Bytes, sizeof(Bytes), Cursor, Value));
  EXPECT_EQ(Value, 624485u);
  EXPECT_EQ(Cursor, 3u);
}

TEST(DwarfEHPrimitives, ReadsNegativeSLEB128) {
  const uint8_t Bytes[] = {0x9b, 0xf1, 0x59};
  size_t Cursor = 0;
  int64_t Value = 0;
  ASSERT_TRUE(readSLEB128(Bytes, sizeof(Bytes), Cursor, Value));
  EXPECT_EQ(Value, -624485);
  EXPECT_EQ(Cursor, 3u);
}

TEST(DwarfEHPrimitives, RejectsUnterminatedLEB128) {
  const uint8_t Bytes[] = {0x80, 0x80, 0x80};
  size_t Cursor = 0;
  uint64_t Value = 0;
  EXPECT_FALSE(readULEB128(Bytes, sizeof(Bytes), Cursor, Value));
  EXPECT_EQ(Cursor, 0u);
}

TEST(DwarfEHPrimitives, RejectsOverlongLEB128) {
  // Twelve continuation bytes cannot encode a 64-bit value; accepting them
  // would silently drop the high bits of a crafted length field.
  const uint8_t Bytes[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                           0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
  size_t Cursor = 0;
  uint64_t Value = 0;
  EXPECT_FALSE(readULEB128(Bytes, sizeof(Bytes), Cursor, Value));
  EXPECT_EQ(Cursor, 0u);
}

//===----------------------------------------------------------------------===//
// Encoded pointers
//===----------------------------------------------------------------------===//

TEST(DwarfEHPointer, ResolvesPCRelativeSData4) {
  ByteBuilder B;
  B.i32(-0x20);
  size_t Cursor = 0;
  va_t Out = 0;
  BinaryImage Img = makeImage();
  ASSERT_TRUE(readEncodedPointer(B.data().data(), B.size(), Cursor, 0x401000,
                                 0x1b, PointerBases{}, 8, &Img, Out));
  EXPECT_EQ(Out, 0x400fe0u);
  EXPECT_EQ(Cursor, 4u);
}

TEST(DwarfEHPointer, FollowsIndirectionAndReportsItsSlot) {
  BinaryImage Img = makeImage();
  ByteBuilder Slot;
  Slot.u64(0x401234);
  writeData(Img, kDataVA + 0x40, Slot.data());

  ByteBuilder B;
  B.u32(static_cast<uint32_t>(kDataVA + 0x40));
  size_t Cursor = 0;
  va_t Out = 0;
  va_t SlotVA = 0;
  // DW_EH_PE_indirect | DW_EH_PE_udata4
  ASSERT_TRUE(readEncodedPointer(B.data().data(), B.size(), Cursor, kDataVA,
                                 0x83, PointerBases{}, 8, &Img, Out, &SlotVA));
  EXPECT_EQ(SlotVA, kDataVA + 0x40);
  EXPECT_EQ(Out, 0x401234u);
}

TEST(DwarfEHPointer, RefusesRelativeEncodingWithoutItsBase) {
  ByteBuilder B;
  B.u32(0x10);
  size_t Cursor = 0;
  va_t Out = 0;
  BinaryImage Img = makeImage();
  // DW_EH_PE_datarel | DW_EH_PE_udata4 with no data base supplied.
  EXPECT_FALSE(readEncodedPointer(B.data().data(), B.size(), Cursor, kDataVA,
                                  0x33, PointerBases{}, 8, &Img, Out));
}

TEST(DwarfEHPointer, TreatsZeroAsAbsentRatherThanBaseRelative) {
  ByteBuilder B;
  B.u32(0);
  size_t Cursor = 0;
  va_t Out = 0xdeadbeef;
  BinaryImage Img = makeImage();
  PointerBases Bases;
  Bases.Data = kDataVA;
  ASSERT_TRUE(readEncodedPointer(B.data().data(), B.size(), Cursor, kDataVA,
                                 0x33, Bases, 8, &Img, Out));
  EXPECT_EQ(Out, 0u);
}

//===----------------------------------------------------------------------===//
// Call frame information
//===----------------------------------------------------------------------===//

TEST(DwarfEHFrame, DecodesCIEAndFDE) {
  BinaryImage Img = makeImage();
  const va_t SectionVA = kDataVA;
  FrameBytes Frame =
      buildSimpleFrame(SectionVA, kTextVA + 0x100, 0x40, "zR", 0, 0);
  writeData(Img, SectionVA, Frame.Bytes);

  FrameSection Sec{Frame.Bytes.data(), Frame.Bytes.size(), SectionVA};
  ParseResult Result = parseEHFrame(Img, Sec, PointerBases{});

  ASSERT_EQ(Result.CIEs.size(), 1u);
  ASSERT_EQ(Result.FDEs.size(), 1u);
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);

  const DwarfCIE &CIE = Result.CIEs[0];
  EXPECT_EQ(CIE.Version, 1);
  EXPECT_EQ(CIE.Augmentation, "zR");
  EXPECT_EQ(CIE.CodeAlignmentFactor, 1u);
  EXPECT_EQ(CIE.DataAlignmentFactor, -8);
  EXPECT_EQ(CIE.ReturnAddressRegister, 16u);
  EXPECT_TRUE(CIE.HasAugmentationData);
  EXPECT_EQ(CIE.FDEPointerEncoding, 0x1b);

  const DwarfFDE &FDE = Result.FDEs[0];
  EXPECT_EQ(FDE.InitialLocation, kTextVA + 0x100);
  EXPECT_EQ(FDE.AddressRange, 0x40u);
  EXPECT_EQ(FDE.CIESectionOffset, Frame.CIEOffset);
}

TEST(DwarfEHFrame, ScalesInitialInstructionsByAlignmentFactors) {
  BinaryImage Img = makeImage();
  FrameBytes Frame =
      buildSimpleFrame(kDataVA, kTextVA + 0x100, 0x40, "zR", 0, 0);
  writeData(Img, kDataVA, Frame.Bytes);
  FrameSection Sec{Frame.Bytes.data(), Frame.Bytes.size(), kDataVA};
  ParseResult Result = parseEHFrame(Img, Sec, PointerBases{});

  ASSERT_EQ(Result.CIEs.size(), 1u);
  const auto &Ops = Result.CIEs[0].InitialInstructions;
  ASSERT_GE(Ops.size(), 2u);
  EXPECT_EQ(Ops[0].Kind, CFIOpKind::DefCFA);
  EXPECT_EQ(Ops[0].Register, 7u);
  EXPECT_EQ(Ops[0].Offset, 8);
  EXPECT_EQ(Ops[1].Kind, CFIOpKind::Offset);
  EXPECT_EQ(Ops[1].Register, 16u);
  // A factored offset of 1 with a data alignment factor of -8 is -8 bytes,
  // and a consumer must not have to re-apply the factor.
  EXPECT_EQ(Ops[1].Offset, -8);
}

TEST(DwarfEHFrame, TracksLocationAcrossAdvanceInstructions) {
  BinaryImage Img = makeImage();
  FrameBytes Frame =
      buildSimpleFrame(kDataVA, kTextVA + 0x100, 0x40, "zR", 0, 0);
  writeData(Img, kDataVA, Frame.Bytes);
  FrameSection Sec{Frame.Bytes.data(), Frame.Bytes.size(), kDataVA};
  ParseResult Result = parseEHFrame(Img, Sec, PointerBases{});

  ASSERT_EQ(Result.FDEs.size(), 1u);
  const auto &Ops = Result.FDEs[0].Instructions;
  ASSERT_GE(Ops.size(), 2u);
  EXPECT_EQ(Ops[0].Kind, CFIOpKind::AdvanceLoc);
  EXPECT_EQ(Ops[1].Kind, CFIOpKind::DefCFAOffset);
  // The rule established after a one-byte advance takes effect one byte in.
  EXPECT_EQ(Ops[1].CodeOffset, 1u);
}

TEST(DwarfEHFrame, ResolvesPersonalityAndLSDAThroughAugmentation) {
  BinaryImage Img = makeImage();
  const va_t PersonalityVA = kTextVA + 0x800;
  const va_t LSDAVA = kDataVA + 0x400;
  FrameBytes Frame = buildSimpleFrame(kDataVA, kTextVA + 0x100, 0x40, "zPLR",
                                      PersonalityVA, LSDAVA);
  writeData(Img, kDataVA, Frame.Bytes);
  FrameSection Sec{Frame.Bytes.data(), Frame.Bytes.size(), kDataVA};
  ParseResult Result = parseEHFrame(Img, Sec, PointerBases{});

  ASSERT_EQ(Result.CIEs.size(), 1u);
  ASSERT_EQ(Result.FDEs.size(), 1u);
  EXPECT_EQ(Result.CIEs[0].PersonalityVA, PersonalityVA);
  EXPECT_EQ(Result.CIEs[0].LSDAPointerEncoding, 0x1b);
  EXPECT_EQ(Result.FDEs[0].LSDAVA, LSDAVA);
}

TEST(DwarfEHFrame, StopsAtZeroLengthTerminator) {
  BinaryImage Img = makeImage();
  FrameBytes Frame =
      buildSimpleFrame(kDataVA, kTextVA + 0x100, 0x40, "zR", 0, 0);
  // Append garbage past the terminator the builder already wrote.
  Frame.Bytes.insert(Frame.Bytes.end(), 64, 0xcc);
  writeData(Img, kDataVA, Frame.Bytes);
  FrameSection Sec{Frame.Bytes.data(), Frame.Bytes.size(), kDataVA};
  ParseResult Result = parseEHFrame(Img, Sec, PointerBases{});
  EXPECT_EQ(Result.FDEs.size(), 1u);
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(DwarfEHFrame, RejectsEntryLengthPastTheSection) {
  BinaryImage Img = makeImage();
  ByteBuilder B;
  B.u32(0x7fffffff);
  B.u32(0);
  B.pad(8);
  writeData(Img, kDataVA, B.data());
  FrameSection Sec{B.data().data(), B.size(), kDataVA};
  ParseResult Result = parseEHFrame(Img, Sec, PointerBases{});
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Malformed);
  EXPECT_TRUE(Result.FDEs.empty());
}

TEST(DwarfEHFrame, RejectsFDEWhoseCIEPointerEscapesTheSection) {
  BinaryImage Img = makeImage();
  ByteBuilder B;
  const size_t LengthSlot = B.size();
  B.u32(0);
  const size_t BodyStart = B.size();
  B.u32(0xffffff); // CIE pointer far before the section
  B.u32(0);
  B.u32(0);
  B.patch32(LengthSlot, static_cast<uint32_t>(B.size() - BodyStart));
  B.u32(0);
  writeData(Img, kDataVA, B.data());
  FrameSection Sec{B.data().data(), B.size(), kDataVA};
  ParseResult Result = parseEHFrame(Img, Sec, PointerBases{});
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Malformed);
  EXPECT_TRUE(Result.FDEs.empty());
}

TEST(DwarfEHFrame, ReportsUnsupportedCIEVersion) {
  BinaryImage Img = makeImage();
  ByteBuilder B;
  const size_t LengthSlot = B.size();
  B.u32(0);
  const size_t BodyStart = B.size();
  B.u32(0);
  B.u8(9); // no such CIE version
  B.str("");
  B.uleb(1);
  B.sleb(-8);
  B.u8(16);
  B.patch32(LengthSlot, static_cast<uint32_t>(B.size() - BodyStart));
  B.u32(0);
  writeData(Img, kDataVA, B.data());
  FrameSection Sec{B.data().data(), B.size(), kDataVA};
  ParseResult Result = parseEHFrame(Img, Sec, PointerBases{});
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Malformed);
  EXPECT_TRUE(Result.CIEs.empty());
}

//===----------------------------------------------------------------------===//
// Itanium LSDA
//===----------------------------------------------------------------------===//

/// Build a `.gcc_except_table` with one catch handler and one cleanup.
///
/// Layout mirrors what clang emits for
///   `try { may_throw(); } catch (int) { ... }` with a live destructor.
struct LSDABytes {
  std::vector<uint8_t> Bytes;
  va_t TypeInfoVA = 0;
};

LSDABytes buildCatchLSDA(va_t LSDAVA, va_t FuncVA, va_t TypeInfoVA) {
  ByteBuilder B;
  B.u8(0xff); // DW_EH_PE_omit: landing pad base defaults to the function
  B.u8(0x00); // DW_EH_PE_absptr type table entries

  // The type-table offset is measured from just after the uleb field.  The
  // table base sits past the action table; compute it after the body is laid
  // out by building the body separately.
  ByteBuilder Body;
  {
    ByteBuilder CallSites;
    // Region 1: protected call, landing pad with an action chain.
    CallSites.u32(0x10); // start
    CallSites.u32(0x10); // length
    CallSites.u32(0x40); // landing pad
    CallSites.uleb(1);   // first action (1-based)
    // Region 2: cleanup-only region with a landing pad and no action.
    CallSites.u32(0x20);
    CallSites.u32(0x08);
    CallSites.u32(0x60);
    CallSites.uleb(0);
    // Region 3: unprotected tail.
    CallSites.u32(0x30);
    CallSites.u32(0x08);
    CallSites.u32(0);
    CallSites.uleb(0);

    Body.u8(0x03); // DW_EH_PE_udata4 call sites
    Body.uleb(CallSites.size());
    for (uint8_t Byte : CallSites.data())
      Body.u8(Byte);

    // Action table: catch type 1, then chain to a cleanup.  The chain link is
    // self-relative to its own position, which is table offset 1, so reaching
    // the record at offset 2 is a displacement of +1.
    Body.sleb(1);
    Body.sleb(1);
    // Entry at offset 2: cleanup, end of chain.
    Body.sleb(0);
    Body.sleb(0);
  }

  // The type-table base is the address just past the last entry, and entries
  // grow downward from it.  Place it immediately after the body.
  const size_t HeaderPrefix = B.size();
  // Emit a one-byte uleb for the offset; the layout below keeps it under 128.
  const size_t OffsetFieldSize = 1;
  const size_t TypeTableBaseOffset = Body.size() + 8; // one 8-byte entry
  B.uleb(TypeTableBaseOffset);
  EXPECT_EQ(B.size() - HeaderPrefix, OffsetFieldSize);

  const size_t AfterOffsetField = B.size();
  for (uint8_t Byte : Body.data())
    B.u8(Byte);
  // Entry 1 lives at base - 8, which is exactly here.
  B.u64(TypeInfoVA);

  EXPECT_EQ(AfterOffsetField + TypeTableBaseOffset, B.size());
  (void)LSDAVA;
  (void)FuncVA;

  LSDABytes Result;
  Result.Bytes = B.data();
  Result.TypeInfoVA = TypeInfoVA;
  return Result;
}

/// Build the `.gcc_except_table` shape rustc emits for a function that owns a
/// value needing `Drop`, calls `catch_unwind`, and crosses an `extern "C"`
/// boundary.  All three pad kinds appear, which is the whole point: they are
/// spelled with the same structures C++ uses and are told apart only by what
/// the action chain selects.
std::vector<uint8_t> buildRustLSDA() {
  ByteBuilder B;
  B.u8(0xff); // landing pad base defaults to the function start
  B.u8(0x00); // DW_EH_PE_absptr type table entries

  ByteBuilder Body;
  {
    ByteBuilder CallSites;
    // Cleanup only: a landing pad with no action runs `Drop` glue and resumes.
    CallSites.u32(0x10);
    CallSites.u32(0x10);
    CallSites.u32(0x40);
    CallSites.uleb(0);
    // `catch_unwind`: names the action at table offset 0, a catch on the null
    // type-table slot.
    CallSites.u32(0x20);
    CallSites.u32(0x10);
    CallSites.u32(0x50);
    CallSites.uleb(1);
    // Nounwind boundary: names the action at table offset 2, an empty filter.
    CallSites.u32(0x30);
    CallSites.u32(0x10);
    CallSites.u32(0x60);
    CallSites.uleb(3);

    Body.u8(0x03); // DW_EH_PE_udata4 call sites
    Body.uleb(CallSites.size());
    for (uint8_t Byte : CallSites.data())
      Body.u8(Byte);

    // Offset 0: catch type-table slot 1, end of chain.
    Body.sleb(1);
    Body.sleb(0);
    // Offset 2: exception-specification list 1, end of chain.
    Body.sleb(-1);
    Body.sleb(0);
  }

  // One 8-byte type-table entry below the base, and the specification list
  // above it.
  const size_t TypeTableBaseOffset = Body.size() + 8;
  B.uleb(TypeTableBaseOffset);
  const size_t AfterOffsetField = B.size();
  for (uint8_t Byte : Body.data())
    B.u8(Byte);
  B.u64(0); // slot 1: a null `std::type_info *`, which is the catch-all
  EXPECT_EQ(AfterOffsetField + TypeTableBaseOffset, B.size());
  B.uleb(0); // specification list 1: empty, so nothing may propagate
  return B.data();
}

/// Assemble an image whose single function carries the Rust LSDA above, with
/// \p PanicName defined at \p PanicVA and a direct call to it.
BinaryImage makeRustImage(va_t FuncVA, va_t PanicVA, const char *PanicName) {
  BinaryImage Img = makeImage();
  const va_t SectionVA = kDataVA;
  const va_t PersonalityVA = kTextVA + 0x800;
  const va_t LSDAVA = kDataVA + 0x400;

  Symbol Personality;
  Personality.Name = "rust_eh_personality";
  Personality.Addr = PersonalityVA;
  Personality.IsFunc = true;
  Img.Symbols.push_back(std::move(Personality));

  Symbol Core;
  Core.Name = "_ZN4core9panicking5panic17h0123456789abcdefE";
  Core.Addr = kTextVA + 0x900;
  Core.IsFunc = true;
  Img.Symbols.push_back(std::move(Core));

  Symbol Panic;
  Panic.Name = PanicName;
  Panic.Addr = PanicVA;
  Panic.IsFunc = true;
  Img.Symbols.push_back(std::move(Panic));

  FrameBytes Frame = buildSimpleFrame(SectionVA, FuncVA, 0x80, "zPLR",
                                      PersonalityVA, LSDAVA);
  writeData(Img, SectionVA, Frame.Bytes);

  Section EhFrame;
  EhFrame.Name = ".eh_frame";
  EhFrame.VA = SectionVA;
  EhFrame.Size = Frame.Bytes.size();
  EhFrame.Data = Frame.Bytes;
  Img.Sections.push_back(std::move(EhFrame));

  writeData(Img, LSDAVA, buildRustLSDA());

  // A direct `call rel32` at the start of the function body.
  ByteBuilder Call;
  Call.u8(0xe8);
  Call.i32(static_cast<int32_t>(static_cast<int64_t>(PanicVA) -
                               static_cast<int64_t>(FuncVA + 5)));
  writeData(Img, FuncVA, Call.data());
  return Img;
}

TEST(RustEH, ClassifiesEveryLandingPadKindItSharesWithCxx) {
  const va_t FuncVA = kTextVA + 0x100;
  BinaryImage Img =
      makeRustImage(FuncVA, kTextVA + 0x900,
                    "_ZN4core9panicking5panic17h0123456789abcdefE");
  parseItaniumExceptions(Img);
  rust_eh::parseRustExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  ASSERT_TRUE(F.Rust.has_value());
  ASSERT_EQ(F.Rust->LandingPads.size(), 3u);
  EXPECT_EQ(F.Rust->LandingPads[0].Kind, RustLandingPadKind::DropGlue);
  EXPECT_EQ(F.Rust->LandingPads[0].PadVA, FuncVA + 0x40);
  EXPECT_EQ(F.Rust->LandingPads[1].Kind, RustLandingPadKind::CatchUnwind);
  EXPECT_EQ(F.Rust->LandingPads[1].PadVA, FuncVA + 0x50);
  EXPECT_EQ(F.Rust->LandingPads[2].Kind, RustLandingPadKind::NoUnwindGuard);
  EXPECT_EQ(F.Rust->LandingPads[2].PadVA, FuncVA + 0x60);

  EXPECT_TRUE(F.Rust->runsDropGlue());
  EXPECT_TRUE(F.Rust->catchesUnwind());
  EXPECT_TRUE(F.Rust->guardsAgainstUnwind());
  EXPECT_FALSE(F.Rust->UsesMSVCTables);

  ASSERT_TRUE(Img.ExceptionMetadata.RustRuntime.has_value());
  const RustRuntimeInfo &RT = *Img.ExceptionMetadata.RustRuntime;
  EXPECT_EQ(RT.Strategy, RustPanicStrategy::Unwind);
  EXPECT_EQ(RT.CleanupFrames, 1u);
  EXPECT_EQ(RT.CatchUnwindFrames, 1u);
  EXPECT_EQ(RT.NoUnwindGuardFrames, 1u);
  EXPECT_FALSE(RT.UsesMSVCUnwinding);
}

TEST(RustEH, ClassifiesPanicSitesByWhatTheCheckIs) {
  struct Case {
    const char *Symbol;
    RustPanicKind Kind;
  } const Cases[] = {
      {"_ZN4core9panicking5panic17h0123456789abcdefE",
       RustPanicKind::Explicit},
      {"_ZN4core9panicking18panic_bounds_check17h0123456789abcdefE",
       RustPanicKind::BoundsCheck},
      {"_ZN4core9panicking11panic_const23panic_const_div_by_zero17h012345678"
       "9abcdefE",
       RustPanicKind::Arithmetic},
      {"_ZN4core9panicking19panic_cannot_unwind17h0123456789abcdefE",
       RustPanicKind::NoUnwind},
      {"_Unwind_Resume", RustPanicKind::Resume},
  };

  for (const Case &C : Cases) {
    const va_t FuncVA = kTextVA + 0x100;
    const va_t PanicVA = kTextVA + 0x940;
    BinaryImage Img = makeRustImage(FuncVA, PanicVA, C.Symbol);
    parseItaniumExceptions(Img);
    rust_eh::parseRustExceptions(Img);

    ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u) << C.Symbol;
    const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
    ASSERT_TRUE(F.Rust.has_value()) << C.Symbol;
    ASSERT_EQ(F.Rust->Panics.size(), 1u) << C.Symbol;
    EXPECT_EQ(F.Rust->Panics[0].Kind, C.Kind) << C.Symbol;
    EXPECT_EQ(F.Rust->Panics[0].CallVA, FuncVA) << C.Symbol;
    EXPECT_EQ(F.Rust->Panics[0].TargetVA, PanicVA) << C.Symbol;
  }
}

TEST(RustEH, DoesNotTreatPanicBookkeepingAsAPanicOrigin) {
  // `std::panicking` is full of helpers that inspect a panic already in
  // flight.  Matching the module rather than the function would put a raise
  // edge on every one of them.
  const va_t FuncVA = kTextVA + 0x100;
  BinaryImage Img = makeRustImage(
      FuncVA, kTextVA + 0x940,
      "_ZN3std9panicking12catch_unwind7cleanup17h0123456789abcdefE");
  parseItaniumExceptions(Img);
  rust_eh::parseRustExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  ASSERT_TRUE(F.Rust.has_value());
  EXPECT_TRUE(F.Rust->Panics.empty());
}

TEST(RustEH, LeavesANonRustFrameAlone) {
  // The same tables under the C++ personality mean `catch (...)` and a
  // `throw()` specification, which are not Rust's semantics at all.
  const va_t FuncVA = kTextVA + 0x100;
  BinaryImage Img = makeRustImage(
      FuncVA, kTextVA + 0x940,
      "_ZN4core9panicking5panic17h0123456789abcdefE");
  for (Symbol &S : Img.Symbols)
    if (S.Name == "rust_eh_personality")
      S.Name = "__gxx_personality_v0";
  parseItaniumExceptions(Img);
  rust_eh::parseRustExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  EXPECT_FALSE(Img.ExceptionMetadata.Functions[0].Rust.has_value());
  EXPECT_TRUE(Img.ExceptionMetadata.Functions[0].Itanium.has_value());
}

TEST(RustEH, ReadsAPanicAbortImageAsAborting) {
  // Nothing that can raise or continue an unwind, and no landing pad: the
  // image cannot have been built to unwind whatever else it contains.
  BinaryImage Img = makeImage();
  Symbol Core;
  Core.Name = "_ZN4core9panicking5panic17h0123456789abcdefE";
  Core.Addr = kTextVA + 0x900;
  Core.IsFunc = true;
  Img.Symbols.push_back(std::move(Core));

  rust_eh::parseRustExceptions(Img);
  ASSERT_TRUE(Img.ExceptionMetadata.RustRuntime.has_value());
  EXPECT_EQ(Img.ExceptionMetadata.RustRuntime->Strategy,
            RustPanicStrategy::Abort);
}

TEST(RustEH, IgnoresAnImageWithoutTheRustRuntime) {
  BinaryImage Img = makeImage();
  rust_eh::parseRustExceptions(Img);
  EXPECT_FALSE(Img.ExceptionMetadata.RustRuntime.has_value());
}

TEST(ItaniumLSDA, DecodesCallSitesActionsAndTypes) {
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x200;
  const va_t TypeInfoVA = kDataVA + 0x600;

  // A `std::type_info` is a vptr followed by a pointer to the mangled name.
  ByteBuilder TypeInfo;
  TypeInfo.u64(kDataVA + 0x700); // vptr
  TypeInfo.u64(kDataVA + 0x680); // name pointer
  writeData(Img, TypeInfoVA, TypeInfo.data());
  const char kName[] = "i";
  writeData(Img, kDataVA + 0x680,
            std::vector<uint8_t>(kName, kName + sizeof(kName)));

  LSDABytes Table = buildCatchLSDA(LSDAVA, FuncVA, TypeInfoVA);
  writeData(Img, LSDAVA, Table.Bytes);

  LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = FuncVA;
  Req.FunctionEnd = FuncVA + 0x100;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

  ASSERT_TRUE(Result.Info.has_value());
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);
  const ItaniumEHInfo &Info = *Result.Info;

  EXPECT_EQ(Info.LandingPadBase, FuncVA);
  ASSERT_EQ(Info.CallSites.size(), 3u);

  EXPECT_EQ(Info.CallSites[0].GuardedRange.Begin, FuncVA + 0x10);
  EXPECT_EQ(Info.CallSites[0].GuardedRange.End, FuncVA + 0x20);
  EXPECT_EQ(Info.CallSites[0].LandingPadVA, FuncVA + 0x40);
  ASSERT_TRUE(Info.CallSites[0].FirstActionOffset.has_value());
  EXPECT_EQ(*Info.CallSites[0].FirstActionOffset, 0u);

  EXPECT_EQ(Info.CallSites[1].LandingPadVA, FuncVA + 0x60);
  EXPECT_FALSE(Info.CallSites[1].FirstActionOffset.has_value());

  EXPECT_EQ(Info.CallSites[2].LandingPadVA, 0u);

  ASSERT_EQ(Info.Actions.size(), 2u);
  EXPECT_TRUE(Info.Actions[0].isCatch());
  EXPECT_EQ(Info.Actions[0].TypeFilter, 1);
  ASSERT_TRUE(Info.Actions[0].NextActionOffset.has_value());
  EXPECT_EQ(*Info.Actions[0].NextActionOffset, 2u);
  EXPECT_TRUE(Info.Actions[1].isCleanup());
  EXPECT_FALSE(Info.Actions[1].NextActionOffset.has_value());

  ASSERT_EQ(Info.TypeTable.size(), 1u);
  EXPECT_EQ(Info.TypeTable[0].Index, 1u);
  EXPECT_EQ(Info.TypeTable[0].TypeInfoVA, TypeInfoVA);
  EXPECT_EQ(Info.TypeTable[0].TypeName, "i");
  EXPECT_FALSE(Info.TypeTable[0].IsCatchAll);
  EXPECT_FALSE(Info.isCleanupOnly());
}

TEST(ItaniumLSDA, FollowsAnActionChainThatPointsBackwards) {
  // Clang emits action records in reverse and links each to the one *before*
  // it, so a chain link is routinely negative.  Reading the link as an offset
  // from the table base instead of from its own position would send this
  // chain off the front of the table.
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x200;

  ByteBuilder B;
  B.u8(0xff);
  B.u8(0xff);
  ByteBuilder CallSites;
  CallSites.u32(0x00);
  CallSites.u32(0x20);
  CallSites.u32(0x40);
  CallSites.uleb(3); // names the record at table offset 2
  B.u8(0x03);
  B.uleb(CallSites.size());
  for (uint8_t Byte : CallSites.data())
    B.u8(Byte);
  // Offset 0: catch type 1, end of chain.
  B.sleb(1);
  B.sleb(0);
  // Offset 2: catch type 2, chaining back to offset 0.  The link field is at
  // offset 3, so the displacement is -3.
  B.sleb(2);
  B.sleb(-3);
  writeData(Img, LSDAVA, B.data());

  LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = FuncVA;
  Req.FunctionEnd = FuncVA + 0x100;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

  ASSERT_TRUE(Result.Info.has_value());
  ASSERT_EQ(Result.Info->Actions.size(), 2u);
  EXPECT_EQ(Result.Info->Actions[0].TableOffset, 0u);
  EXPECT_EQ(Result.Info->Actions[0].TypeFilter, 1);
  EXPECT_FALSE(Result.Info->Actions[0].NextActionOffset.has_value());
  EXPECT_EQ(Result.Info->Actions[1].TableOffset, 2u);
  EXPECT_EQ(Result.Info->Actions[1].TypeFilter, 2);
  ASSERT_TRUE(Result.Info->Actions[1].NextActionOffset.has_value());
  EXPECT_EQ(*Result.Info->Actions[1].NextActionOffset, 0u);
}

TEST(ItaniumLSDA, ReportsCleanupOnlyRecords) {
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x200;

  ByteBuilder B;
  B.u8(0xff); // omit landing pad base
  B.u8(0xff); // omit type table
  ByteBuilder CallSites;
  CallSites.u32(0x00);
  CallSites.u32(0x20);
  CallSites.u32(0x40);
  CallSites.uleb(0);
  B.u8(0x03);
  B.uleb(CallSites.size());
  for (uint8_t Byte : CallSites.data())
    B.u8(Byte);
  writeData(Img, LSDAVA, B.data());

  LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = FuncVA;
  Req.FunctionEnd = FuncVA + 0x100;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

  ASSERT_TRUE(Result.Info.has_value());
  EXPECT_TRUE(Result.Info->isCleanupOnly());
  ASSERT_EQ(Result.Info->CallSites.size(), 1u);
  EXPECT_EQ(Result.Info->CallSites[0].LandingPadVA, FuncVA + 0x40);
  EXPECT_TRUE(Result.Info->TypeTable.empty());
}

TEST(ItaniumLSDA, RecognizesCatchAllTypeSlot) {
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x200;
  LSDABytes Table = buildCatchLSDA(LSDAVA, FuncVA, /*TypeInfoVA=*/0);
  writeData(Img, LSDAVA, Table.Bytes);

  LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = FuncVA;
  Req.FunctionEnd = FuncVA + 0x100;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

  ASSERT_TRUE(Result.Info.has_value());
  ASSERT_EQ(Result.Info->TypeTable.size(), 1u);
  EXPECT_TRUE(Result.Info->TypeTable[0].IsCatchAll);
  EXPECT_TRUE(Result.Info->TypeTable[0].TypeName.empty());
}

TEST(ItaniumLSDA, FlagsCallSiteRangesThatLeaveTheirFunction) {
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x200;

  ByteBuilder B;
  B.u8(0xff);
  B.u8(0xff);
  ByteBuilder CallSites;
  CallSites.u32(0x00);
  CallSites.u32(0x800); // far past the declared function end
  CallSites.u32(0x40);
  CallSites.uleb(0);
  B.u8(0x03);
  B.uleb(CallSites.size());
  for (uint8_t Byte : CallSites.data())
    B.u8(Byte);
  writeData(Img, LSDAVA, B.data());

  LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = FuncVA;
  Req.FunctionEnd = FuncVA + 0x100;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Partial);
  ASSERT_TRUE(Result.Info.has_value());
  EXPECT_EQ(Result.Info->CallSites.size(), 1u);
}

TEST(ItaniumLSDA, RejectsCallSiteTablePastMappedData) {
  BinaryImage Img = makeImage();
  const va_t LSDAVA = kDataVA + 0x200;
  ByteBuilder B;
  B.u8(0xff);
  B.u8(0xff);
  B.u8(0x03);
  B.uleb(0x100000); // longer than the record, and than the segment
  writeData(Img, LSDAVA, B.data());

  LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = kTextVA + 0x100;
  Req.FunctionEnd = kTextVA + 0x200;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Malformed);
  EXPECT_FALSE(Result.Info.has_value());
}

TEST(ItaniumLSDA, DecodesExceptionSpecificationLists) {
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x200;
  const va_t TypeInfoVA = kDataVA + 0x600;

  ByteBuilder B;
  B.u8(0xff);
  B.u8(0x00); // absptr type table

  ByteBuilder Body;
  ByteBuilder CallSites;
  CallSites.u32(0x00);
  CallSites.u32(0x20);
  CallSites.u32(0x40);
  CallSites.uleb(1);
  Body.u8(0x03);
  Body.uleb(CallSites.size());
  for (uint8_t Byte : CallSites.data())
    Body.u8(Byte);
  // A negative filter selects exception-specification list 1.
  Body.sleb(-1);
  Body.sleb(0);

  const size_t TypeTableBaseOffset = Body.size() + 8;
  B.uleb(TypeTableBaseOffset);
  const size_t AfterOffsetField = B.size();
  for (uint8_t Byte : Body.data())
    B.u8(Byte);
  B.u64(TypeInfoVA); // type-table entry 1, below the base
  ASSERT_EQ(AfterOffsetField + TypeTableBaseOffset, B.size());
  // The specification list grows upward from the base: index 1 is at base + 0.
  B.uleb(1);
  B.uleb(0);
  writeData(Img, LSDAVA, B.data());

  ByteBuilder TypeInfo;
  TypeInfo.u64(kDataVA + 0x700);
  TypeInfo.u64(kDataVA + 0x680);
  writeData(Img, TypeInfoVA, TypeInfo.data());
  const char kName[] = "St9exception";
  writeData(Img, kDataVA + 0x680,
            std::vector<uint8_t>(kName, kName + sizeof(kName)));

  LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = FuncVA;
  Req.FunctionEnd = FuncVA + 0x100;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

  ASSERT_TRUE(Result.Info.has_value());
  ASSERT_EQ(Result.Info->Actions.size(), 1u);
  EXPECT_TRUE(Result.Info->Actions[0].isExceptionSpecification());
  ASSERT_EQ(Result.Info->ExceptionSpecs.size(), 1u);
  EXPECT_EQ(Result.Info->ExceptionSpecs[0].Index, 1u);
  ASSERT_EQ(Result.Info->ExceptionSpecs[0].TypeIndices.size(), 1u);
  EXPECT_EQ(Result.Info->ExceptionSpecs[0].TypeIndices[0], 1u);
}

//===----------------------------------------------------------------------===//
// End-to-end normalization
//===----------------------------------------------------------------------===//

TEST(ItaniumEHDriver, NormalizesFrameSectionIntoExceptionMetadata) {
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t SectionVA = kDataVA;
  const va_t PersonalityVA = kTextVA + 0x800;
  const va_t LSDAVA = kDataVA + 0x400;

  Symbol Personality;
  Personality.Name = "__gxx_personality_v0";
  Personality.Addr = PersonalityVA;
  Personality.IsFunc = true;
  Img.Symbols.push_back(std::move(Personality));

  FrameBytes Frame =
      buildSimpleFrame(SectionVA, FuncVA, 0x80, "zPLR", PersonalityVA, LSDAVA);
  writeData(Img, SectionVA, Frame.Bytes);

  Section EhFrame;
  EhFrame.Name = ".eh_frame";
  EhFrame.VA = SectionVA;
  EhFrame.Size = Frame.Bytes.size();
  EhFrame.Data = Frame.Bytes;
  Img.Sections.push_back(std::move(EhFrame));

  LSDABytes Table = buildCatchLSDA(LSDAVA, FuncVA, /*TypeInfoVA=*/0);
  writeData(Img, LSDAVA, Table.Bytes);

  parseItaniumExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(F.Encoding, ExceptionEncoding::DwarfFDE);
  EXPECT_EQ(F.model(), ExceptionModel::Itanium);
  EXPECT_EQ(F.Personality, ExceptionPersonality::GxxPersonalityV0);
  EXPECT_EQ(F.PersonalityName, "__gxx_personality_v0");
  EXPECT_EQ(F.CodeRange.Begin, FuncVA);
  EXPECT_EQ(F.CodeRange.End, FuncVA + 0x80);
  ASSERT_TRUE(F.Dwarf.has_value());
  ASSERT_TRUE(F.Itanium.has_value());
  EXPECT_EQ(F.Itanium->CallSites.size(), 3u);
  EXPECT_TRUE(Img.ExceptionMetadata.hasModel(ExceptionModel::Itanium));
  EXPECT_FALSE(F.canRegenerateLanguageMetadata());

  // The FDE proves both the entry and the extent, which is stronger evidence
  // than any heuristic scan produces.
  const ExceptionFunction *Found =
      Img.ExceptionMetadata.findFunction(FuncVA + 0x10);
  ASSERT_NE(Found, nullptr);
  EXPECT_EQ(Found->CodeRange.Begin, FuncVA);
}

TEST(ItaniumEHDriver, IgnoresAnImageWithoutAFrameSection) {
  BinaryImage Img = makeImage();
  parseItaniumExceptions(Img);
  EXPECT_TRUE(Img.ExceptionMetadata.Functions.empty());
  EXPECT_FALSE(Img.ExceptionMetadata.hasModel(ExceptionModel::Itanium));
}

//===----------------------------------------------------------------------===//
// Language runtime identity
//===----------------------------------------------------------------------===//

TEST(LanguageRuntimeNames, ClassifiesEveryKnownPersonality) {
  EXPECT_EQ(classifyPersonalityName("__gxx_personality_v0"),
            ExceptionPersonality::GxxPersonalityV0);
  EXPECT_EQ(classifyPersonalityName("___gxx_personality_v0"),
            ExceptionPersonality::GxxPersonalityV0);
  EXPECT_EQ(classifyPersonalityName("__gcc_personality_v0"),
            ExceptionPersonality::GccPersonalityV0);
  EXPECT_EQ(classifyPersonalityName("rust_eh_personality"),
            ExceptionPersonality::RustEhPersonality);
  EXPECT_EQ(classifyPersonalityName("_except_handler3"),
            ExceptionPersonality::ExceptHandler3);
  EXPECT_EQ(classifyPersonalityName("_except_handler4"),
            ExceptionPersonality::ExceptHandler4);
  EXPECT_EQ(classifyPersonalityName("__CxxFrameHandler"),
            ExceptionPersonality::CxxFrameHandlerX86);
  EXPECT_EQ(classifyPersonalityName("__CxxFrameHandler3"),
            ExceptionPersonality::CxxFrameHandler3);
  EXPECT_EQ(classifyPersonalityName("__imp___CxxFrameHandler3"),
            ExceptionPersonality::CxxFrameHandler3);
  EXPECT_EQ(classifyPersonalityName("__C_specific_handler"),
            ExceptionPersonality::CSpecificHandler);
  EXPECT_EQ(classifyPersonalityName("__DelphiExceptionHandler"),
            ExceptionPersonality::DelphiExceptionHandler);
  EXPECT_EQ(classifyPersonalityName("@HandleAnyException"),
            ExceptionPersonality::DelphiX86Handler);
  EXPECT_EQ(classifyPersonalityName("not_a_personality"),
            ExceptionPersonality::Unknown);
  EXPECT_EQ(classifyPersonalityName(""), ExceptionPersonality::None);
}

TEST(LanguageRuntimeNames, MapsPersonalityToItsRuntime) {
  EXPECT_EQ(getPersonalityRuntime(ExceptionPersonality::RustEhPersonality),
            SourceLanguageRuntime::Rust);
  EXPECT_EQ(getPersonalityRuntime(ExceptionPersonality::GxxPersonalityV0),
            SourceLanguageRuntime::CxxItanium);
  EXPECT_EQ(getPersonalityRuntime(ExceptionPersonality::GccPersonalityV0),
            SourceLanguageRuntime::C);
  EXPECT_EQ(getPersonalityRuntime(ExceptionPersonality::CxxFrameHandler3),
            SourceLanguageRuntime::CxxMSVC);
  EXPECT_EQ(getPersonalityRuntime(ExceptionPersonality::DelphiX86Handler),
            SourceLanguageRuntime::Delphi);
  // A personality shared by several languages must not claim one of them.
  EXPECT_EQ(getPersonalityRuntime(ExceptionPersonality::CSpecificHandler),
            SourceLanguageRuntime::Unknown);
}

TEST(RustMangling, RecognizesBothManglingSchemes) {
  EXPECT_TRUE(
      isRustMangledName("_ZN4core3fmt9Formatter3pad17h0123456789abcdefE"));
  EXPECT_TRUE(isRustMangledName("_RNvCs1234_4core3foo"));
  EXPECT_FALSE(isRustMangledName("_ZNSt6vectorIiE9push_backERKi"));
  EXPECT_FALSE(isRustMangledName("plain_c_symbol"));
}

TEST(RustMangling, StripsTheLegacyDisambiguatorHash) {
  const std::string Demangled =
      demangleRustName("_ZN4core3fmt9Formatter3pad17h0123456789abcdefE");
  EXPECT_EQ(Demangled, "core::fmt::Formatter::pad");
}

TEST(RustMangling, DemanglesV0Symbols) {
  // Taken from a `-C symbol-mangling-version=v0` build of a crate named `v0`.
  EXPECT_EQ(demangleRustName("_RNvCsjH1N12swBG2_2v04main"), "v0::main");
  // The same symbol as Darwin spells it, with the platform underscore added.
  EXPECT_EQ(demangleRustName("__RNvCsjH1N12swBG2_2v04main"), "v0::main");
  EXPECT_EQ(demangleRustName("_RNvCskdKJRKLKjqM_7___rustc17rust_begin_unwind"),
            "__rustc::rust_begin_unwind");
}

TEST(RustMangling, LeavesNonRustNamesAlone) {
  EXPECT_TRUE(demangleRustName("_ZNSt6vectorIiE9push_backERKi").empty());
  EXPECT_TRUE(demangleRustName("main").empty());
}

TEST(LanguageRuntimeDetection, IdentifiesRustFromStandardLibrarySymbols) {
  BinaryImage Img = makeImage();
  Symbol Sym;
  Sym.Name = "_ZN4core9panicking9panic_fmt17h0123456789abcdefE";
  Sym.Addr = kTextVA + 0x40;
  Sym.IsFunc = true;
  Img.Symbols.push_back(std::move(Sym));

  LanguageRuntimeInfo Info = detectLanguageRuntime(Img);
  EXPECT_TRUE(Info.is(SourceLanguageRuntime::Rust));
  EXPECT_FALSE(Info.Evidence.empty());
}

TEST(LanguageRuntimeDetection, IdentifiesGoFromItsFunctionTableSection) {
  BinaryImage Img = makeImage();
  Section Pcln;
  Pcln.Name = ".gopclntab";
  Pcln.VA = kDataVA + 0x800;
  Pcln.Size = 0x40;
  Img.Sections.push_back(std::move(Pcln));

  LanguageRuntimeInfo Info = detectLanguageRuntime(Img);
  EXPECT_EQ(Info.Runtime, SourceLanguageRuntime::Go);
}

TEST(LanguageRuntimeDetection, ReportsAMixedImage) {
  BinaryImage Img = makeImage();
  Section Pcln;
  Pcln.Name = ".gopclntab";
  Pcln.VA = kDataVA + 0x800;
  Pcln.Size = 0x40;
  Img.Sections.push_back(std::move(Pcln));
  Symbol Rust;
  Rust.Name = "_ZN3std2rt10lang_start17h0123456789abcdefE";
  Rust.Addr = kTextVA + 0x40;
  Rust.IsFunc = true;
  Img.Symbols.push_back(std::move(Rust));

  LanguageRuntimeInfo Info = detectLanguageRuntime(Img);
  EXPECT_TRUE(Info.IsMixed);
  EXPECT_TRUE(Info.is(SourceLanguageRuntime::Go));
  EXPECT_TRUE(Info.is(SourceLanguageRuntime::Rust));
}

TEST(LanguageRuntimeDetection, LeavesAPlainImageUnclassified) {
  BinaryImage Img = makeImage();
  LanguageRuntimeInfo Info = detectLanguageRuntime(Img);
  EXPECT_EQ(Info.Runtime, SourceLanguageRuntime::Unknown);
}

//===----------------------------------------------------------------------===//
// Direct branch decoding
//===----------------------------------------------------------------------===//

/// Decode the halfword pair \p Hw1, \p Hw2 as a Thumb branch sited at \p VA.
std::optional<va_t> decodeThumbAt(va_t VA, uint16_t Hw1, uint16_t Hw2) {
  const uint8_t Bytes[4] = {
      static_cast<uint8_t>(Hw1), static_cast<uint8_t>(Hw1 >> 8),
      static_cast<uint8_t>(Hw2), static_cast<uint8_t>(Hw2 >> 8)};
  size_t Length = 0;
  return decodeDirectBranchTarget(Arch::ARM, InstructionMode::Thumb, Bytes,
                                  sizeof(Bytes), VA, Length);
}

TEST(DirectBranch, DecodesThumbCallAndTailJump) {
  // `f000 f87e` is the `bl` a Thumb-2 assembler emits for a call 0x100 bytes
  // ahead, and `f000 b87e` the `b.w` for the same displacement.
  EXPECT_EQ(decodeThumbAt(0x1000, 0xF000, 0xF87E), va_t(0x1100));
  EXPECT_EQ(decodeThumbAt(0x1000, 0xF000, 0xB87E), va_t(0x1100));
}

TEST(DirectBranch, DecodesThumbBranchAcrossTheSignBoundary) {
  // `f7ff fffe` is the self-referential `bl` an unlinked object carries for a
  // call relocation; every J/I bit is set, so it exercises the whole
  // sign-recovery path.
  EXPECT_EQ(decodeThumbAt(0x1000, 0xF7FF, 0xFFFE), va_t(0x1000));
  EXPECT_EQ(decodeThumbAt(0x2000, 0xF7FE, 0xFFFE), va_t(0x1000));
}

TEST(DirectBranch, RoundsTheBaseDownForAnInterworkingCall) {
  // `blx` targets ARM state, so it is encoded against the word-aligned program
  // counter rather than the halfword-aligned one a `bl` uses.  Sited at an
  // address that is not word aligned, the two disagree.
  EXPECT_EQ(decodeThumbAt(0x1002, 0xF000, 0xE87E), va_t(0x1100));
}

TEST(DirectBranch, RejectsAConditionalThumbBranch) {
  // `f000 8000` is `beq.w`, which shares the leading halfword with `b.w` but
  // names a branch inside the function rather than a call edge.
  EXPECT_FALSE(decodeThumbAt(0x1000, 0xF000, 0x8000).has_value());
  // `f04f 0000` is `mov.w r0, #0` -- the same leading halfword again.
  EXPECT_FALSE(decodeThumbAt(0x1000, 0xF04F, 0x0000).has_value());
}

TEST(DirectBranch, KeepsDecodingARMStateBranches) {
  // `eb000000` is `bl` to the instruction eight bytes ahead, which is where
  // the ARM pipeline offset puts a zero displacement.
  const uint8_t Call[4] = {0x00, 0x00, 0x00, 0xEB};
  size_t Length = 0;
  EXPECT_EQ(decodeDirectBranchTarget(Arch::ARM, InstructionMode::ARM, Call,
                                     sizeof(Call), 0x1000, Length),
            va_t(0x1008));
  // A predicated branch is not a call edge, so it stays undecoded.
  const uint8_t Conditional[4] = {0x00, 0x00, 0x00, 0x0B};
  EXPECT_FALSE(decodeDirectBranchTarget(Arch::ARM, InstructionMode::ARM,
                                        Conditional, sizeof(Conditional),
                                        0x1000, Length)
                   .has_value());
}

TEST(DirectBranch, ScansThumbAtHalfwordGranularity) {
  // A 32-bit Thumb branch may begin at any halfword, so a word-granular scan
  // would step over half of them.
  EXPECT_EQ(getBranchScanStride(Arch::ARM, InstructionMode::Thumb), 2u);
  EXPECT_EQ(getBranchScanStride(Arch::ARM, InstructionMode::ARM), 4u);
  EXPECT_EQ(getBranchScanStride(Arch::X64, InstructionMode::Default), 1u);
}

} // namespace
