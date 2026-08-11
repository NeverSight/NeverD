//===- CompactUnwindTests.cpp - Darwin __unwind_info decoding tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// Coverage for the Darwin compact unwind encodings: the saved-register sets
/// each architecture packs into the encoding word, the permutation the x86
/// frameless forms compress their register order into, and the frameless
/// form whose frame size lives in the function's prologue instead of in the
/// word.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/MachO/CompactUnwind.h"

#include <algorithm>
#include <vector>

using namespace neverd;
using namespace neverd::macho_unwind;

namespace {

//===----------------------------------------------------------------------===//
// Byte-buffer builder
//===----------------------------------------------------------------------===//

/// Assembles the exact byte layouts the decoder reads.  Building the tables
/// here rather than compiling a fixture keeps every encoding branch reachable,
/// including the malformed ones no linker emits.
class ByteBuilder {
public:
  void u16(uint16_t V) { append(&V, sizeof(V)); }
  void u32(uint32_t V) { append(&V, sizeof(V)); }

  size_t size() const { return Bytes.size(); }
  const std::vector<uint8_t> &data() const { return Bytes; }

private:
  void append(const void *P, size_t N) {
    const auto *B = static_cast<const uint8_t *>(P);
    Bytes.insert(Bytes.end(), B, B + N);
  }
  std::vector<uint8_t> Bytes;
};

//===----------------------------------------------------------------------===//
// Encoding construction
//===----------------------------------------------------------------------===//

/// The compact encoding's own register numbers, which are neither the
/// machine's nor DWARF's.
enum CompactReg : uint32_t {
  CU_RBX = 1,
  CU_R12 = 2,
  CU_R13 = 3,
  CU_R14 = 4,
  CU_R15 = 5,
  CU_RBP = 6,
};

/// Machine register numbers the decoder is expected to report.
enum MachineReg : uint16_t {
  RBX = 3,
  RBP = 5,
  R12 = 12,
  R13 = 13,
  R14 = 14,
  R15 = 15,
};

/// The reference permutation encoder, transcribed from `permute_encode` in
/// `third_party/llvm-project/libunwind/include/mach-o/compact_unwind_encoding.h`.
/// Driving the decoder with output from the authority's own encoder is what
/// makes a passing round trip evidence rather than a restatement of the
/// decoder's assumptions.
///
/// \param Regs saved registers in the order the prologue saved them, in the
///             compact encoding's numbering.
uint32_t permuteEncode(const std::vector<uint32_t> &Regs) {
  const size_t Count = Regs.size();
  uint32_t R[6] = {};
  for (size_t I = 0; I < Count; ++I)
    R[6 - Count + I] = Regs[I];

  uint32_t Renum[6] = {};
  for (size_t I = 6 - Count; I < 6; ++I) {
    uint32_t CountLess = 0;
    for (size_t J = 6 - Count; J < I; ++J)
      if (R[J] < R[I])
        ++CountLess;
    Renum[I] = R[I] - CountLess - 1;
  }

  switch (Count) {
  case 6:
    return 120 * Renum[0] + 24 * Renum[1] + 6 * Renum[2] + 2 * Renum[3] +
           Renum[4];
  case 5:
    return 120 * Renum[1] + 24 * Renum[2] + 6 * Renum[3] + 2 * Renum[4] +
           Renum[5];
  case 4:
    return 60 * Renum[2] + 12 * Renum[3] + 3 * Renum[4] + Renum[5];
  case 3:
    return 20 * Renum[3] + 4 * Renum[4] + Renum[5];
  case 2:
    return 5 * Renum[4] + Renum[5];
  case 1:
    return Renum[5];
  default:
    return 0;
  }
}

/// Pack five three-bit slots, lowest-addressed slot first.  A zero names a
/// slot the prologue left empty.
uint32_t frameRegisters(std::initializer_list<uint32_t> Slots) {
  uint32_t Encoding = 0;
  unsigned Index = 0;
  for (uint32_t Slot : Slots)
    Encoding |= (Slot & 0x7) << (3 * Index++);
  return Encoding;
}

uint32_t x86_64FrameEncoding(uint32_t SavedRegisterCount,
                             std::initializer_list<uint32_t> Slots) {
  return kX86_64ModeRBPFrame | (SavedRegisterCount << 16) |
         frameRegisters(Slots);
}

uint32_t x86_64FramelessEncoding(uint32_t Mode, uint32_t Field16,
                                 uint32_t Adjust,
                                 const std::vector<uint32_t> &Regs) {
  return Mode | (Field16 << 16) | (Adjust << 13) |
         (static_cast<uint32_t>(Regs.size()) << 10) | permuteEncode(Regs);
}

/// The machine number the decoder should report for a compact register
/// number.
int machineNumber(uint32_t CompactNumber) {
  static const int Table[] = {0, RBX, R12, R13, R14, R15, RBP};
  return Table[CompactNumber];
}

std::vector<int> machineNumbers(const std::vector<uint32_t> &Regs) {
  std::vector<int> Result;
  for (uint32_t Reg : Regs)
    Result.push_back(machineNumber(Reg));
  return Result;
}

/// The machine register numbers of every slot, in slot order.  An empty slot
/// is reported as -1 so that a gap is visible in a failure message.
std::vector<int> slotRegisters(const CompactUnwindEntry &Entry) {
  std::vector<int> Result;
  for (const CompactUnwindRegisterSlot &Slot : Entry.SavedRegisterSlots)
    Result.push_back(Slot.RegisterClass == UnwindRegisterClass::None
                         ? -1
                         : static_cast<int>(Slot.Register));
  return Result;
}

uint32_t gprMask(std::initializer_list<unsigned> Registers) {
  uint32_t Mask = 0;
  for (unsigned Reg : Registers)
    Mask |= uint32_t(1) << Reg;
  return Mask;
}

//===----------------------------------------------------------------------===//
// Synthetic image
//===----------------------------------------------------------------------===//

constexpr va_t kImageBase = 0x100000000ull;
constexpr uint64_t kTextSize = 0x4000;

/// A Mach-O image with one `__TEXT` segment, which is what makes its virtual
/// address the image base every `functionOffset` is measured from.
BinaryImage makeImage(Arch A = Arch::X64) {
  BinaryImage Img;
  Img.Arch = A;
  Img.Format = BinaryFormat::MachO;
  Img.Bits = Bitness::Bits64;
  Img.Base = kImageBase;
  Img.Entry = kImageBase;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = kImageBase;
  Text.Size = kTextSize;
  Text.FileOff = 0;
  Text.FileSz = kTextSize;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(kTextSize, 0x90);
  Img.Segments.push_back(std::move(Text));
  return Img;
}

struct RawEntry {
  uint32_t FunctionOffset;
  uint32_t Encoding;
};

/// Build an `__unwind_info` section holding one regular second-level page.
std::vector<uint8_t> buildUnwindInfo(const std::vector<RawEntry> &Entries,
                                     uint32_t SentinelOffset,
                                     uint32_t Version = 1,
                                     uint32_t IndexCount = 2) {
  const uint32_t IndexOffset = 28;
  const uint32_t PageOffset = IndexOffset + 24;
  const uint32_t EntriesOffset = PageOffset + 8;
  const uint32_t LSDAOffset =
      EntriesOffset + 8 * static_cast<uint32_t>(Entries.size());

  ByteBuilder B;
  B.u32(Version);
  B.u32(IndexOffset); // common encodings array, empty
  B.u32(0);
  B.u32(IndexOffset); // personality array, empty
  B.u32(0);
  B.u32(IndexOffset);
  B.u32(IndexCount);

  B.u32(Entries.empty() ? SentinelOffset : Entries.front().FunctionOffset);
  B.u32(PageOffset);
  B.u32(LSDAOffset);
  B.u32(SentinelOffset);
  B.u32(0);
  B.u32(LSDAOffset);

  B.u32(2); // UNWIND_SECOND_LEVEL_REGULAR
  B.u16(8);
  B.u16(static_cast<uint16_t>(Entries.size()));
  for (const RawEntry &E : Entries) {
    B.u32(E.FunctionOffset);
    B.u32(E.Encoding);
  }
  return B.data();
}

void attachUnwindInfo(BinaryImage &Img, std::vector<uint8_t> Bytes) {
  Section Sec;
  Sec.Name = "__unwind_info";
  Sec.SegmentName = "__TEXT";
  Sec.VA = kImageBase + 0x2000;
  Sec.Size = Bytes.size();
  Sec.Flags = SegmentFlags::Readable;
  Sec.Data = std::move(Bytes);
  Img.Sections.push_back(std::move(Sec));
}

/// Write `subq $Immediate, %rsp` at \p Offset into the text segment.  The
/// encoding's indirect form points at the immediate field of exactly this
/// instruction, three bytes past its start.
void writeStackSubtract(BinaryImage &Img, uint32_t Offset, uint32_t Immediate) {
  const uint8_t Opcode[3] = {0x48, 0x81, 0xec};
  ASSERT_TRUE(Img.writeVA(kImageBase + Offset, Opcode, sizeof(Opcode)));
  ASSERT_TRUE(Img.writeVA(kImageBase + Offset + 3,
                          reinterpret_cast<const uint8_t *>(&Immediate),
                          sizeof(Immediate)));
}

//===----------------------------------------------------------------------===//
// x86-64 frame pointer
//===----------------------------------------------------------------------===//

TEST(CompactUnwindX86_64Frame, DecodesSavedRegisterSlots) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(
      Arch::X64, x86_64FrameEncoding(3, {CU_RBX, CU_R14, CU_R15}), Entry));

  EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramePointer);
  EXPECT_EQ(slotRegisters(Entry), (std::vector<int>{RBX, R14, R15}));
  EXPECT_EQ(Entry.SavedGPRMask, gprMask({RBX, R14, R15}));
  EXPECT_EQ(Entry.SavedFPRMask, 0u);
  // Three saved registers put slot zero three pointers below the frame
  // pointer, which is where the deepest of them sits.
  EXPECT_EQ(Entry.FrameOffset, 24u);
  // A frame form establishes no frame size of its own.
  EXPECT_FALSE(Entry.HasStackSize);
}

TEST(CompactUnwindX86_64Frame, KeepsInteriorGapButNotTrailingOnes) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(
      Arch::X64, x86_64FrameEncoding(3, {CU_RBX, 0, CU_R12}), Entry));

  // The gap has to survive: r12 sits two pointers above rbx, not one, and
  // dropping the empty slot would move it.
  EXPECT_EQ(slotRegisters(Entry), (std::vector<int>{RBX, -1, R12}));
  EXPECT_EQ(Entry.SavedGPRMask, gprMask({RBX, R12}));
}

TEST(CompactUnwindX86_64Frame, EmptyEncodingSavesNothing) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(Arch::X64, x86_64FrameEncoding(0, {}), Entry));

  EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramePointer);
  EXPECT_TRUE(Entry.SavedRegisterSlots.empty());
  EXPECT_EQ(Entry.SavedGPRMask, 0u);
  EXPECT_EQ(Entry.FrameOffset, 0u);
}

TEST(CompactUnwindX86_64Frame, RejectsFramePointerInASavedSlot) {
  CompactUnwindEntry Entry;
  // The frame form has already spent rbp on the frame, so a slot claiming to
  // hold it describes a frame no unwinder can walk.
  EXPECT_FALSE(decodeEncoding(Arch::X64,
                              x86_64FrameEncoding(2, {CU_RBX, CU_RBP}), Entry));

  EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramePointer);
  EXPECT_TRUE(Entry.SavedRegisterSlots.empty());
  EXPECT_EQ(Entry.SavedGPRMask, 0u);
}

TEST(CompactUnwindX86_64Frame, RejectsSlotNumberWithNoRegister) {
  CompactUnwindEntry Entry;
  EXPECT_FALSE(decodeEncoding(Arch::X64, x86_64FrameEncoding(1, {7}), Entry));
  EXPECT_EQ(Entry.SavedGPRMask, 0u);
}

//===----------------------------------------------------------------------===//
// x86-64 frameless permutation
//===----------------------------------------------------------------------===//

class CompactUnwindPermutation
    : public ::testing::TestWithParam<std::vector<uint32_t>> {};

TEST_P(CompactUnwindPermutation, RoundTripsThroughTheReferenceEncoder) {
  const std::vector<uint32_t> &Regs = GetParam();
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(
      Arch::X64, x86_64FramelessEncoding(kX86_64ModeStackImmediate, 8, 0, Regs),
      Entry));

  EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramelessImmediate);
  EXPECT_EQ(slotRegisters(Entry), machineNumbers(Regs));
  EXPECT_EQ(Entry.StackSize, 64u);
  EXPECT_TRUE(Entry.HasStackSize);
}

INSTANTIATE_TEST_SUITE_P(
    SavedRegisterOrders, CompactUnwindPermutation,
    ::testing::Values(
        std::vector<uint32_t>{CU_RBX}, std::vector<uint32_t>{CU_RBP},
        std::vector<uint32_t>{CU_RBP, CU_R12},
        std::vector<uint32_t>{CU_R12, CU_RBP},
        std::vector<uint32_t>{CU_RBX, CU_R12, CU_R13},
        std::vector<uint32_t>{CU_R13, CU_RBX, CU_R12},
        // The worked example in LLVM's own encoder comment.
        std::vector<uint32_t>{CU_RBP, CU_R12, CU_R14, CU_R15},
        std::vector<uint32_t>{CU_R15, CU_R14, CU_R13, CU_R12},
        std::vector<uint32_t>{CU_RBX, CU_R12, CU_R13, CU_R14, CU_R15},
        std::vector<uint32_t>{CU_R15, CU_R14, CU_R13, CU_R12, CU_RBX},
        // Six registers encode only five digits: the last one is
        // whatever the others did not claim.
        std::vector<uint32_t>{CU_RBX, CU_R12, CU_R13, CU_R14, CU_R15, CU_RBP},
        std::vector<uint32_t>{CU_RBP, CU_R15, CU_R14, CU_R13, CU_R12, CU_RBX}));

TEST(CompactUnwindX86_64Frameless, ExhaustivelyRoundTripsEveryOrder) {
  // Every ordered subset of the six registers: the prefixes of every
  // permutation of all six enumerate exactly those.  A divisor row that is
  // wrong for one register count, or a re-expansion that mis-ranks one digit,
  // cannot survive this.
  std::vector<uint32_t> All = {1, 2, 3, 4, 5, 6};
  do {
    for (unsigned Count = 1; Count <= All.size(); ++Count) {
      const std::vector<uint32_t> Regs(All.begin(), All.begin() + Count);
      CompactUnwindEntry Entry;
      ASSERT_TRUE(decodeEncoding(
          Arch::X64,
          x86_64FramelessEncoding(kX86_64ModeStackImmediate, 0, 0, Regs),
          Entry));
      ASSERT_EQ(slotRegisters(Entry), machineNumbers(Regs));
    }
  } while (std::next_permutation(All.begin(), All.end()));
}

TEST(CompactUnwindX86_64Frameless, RejectsRegisterCountWithNoEncoding) {
  CompactUnwindEntry Entry;
  // Seven fits the three-bit count field but names more registers than the
  // permutation alphabet has.
  EXPECT_FALSE(
      decodeEncoding(Arch::X64, kX86_64ModeStackImmediate | (7u << 10), Entry));
  EXPECT_TRUE(Entry.SavedRegisterSlots.empty());
}

TEST(CompactUnwindX86_64Frameless, RejectsPermutationOutOfRange) {
  CompactUnwindEntry Entry;
  // One register can only be one of six, so a digit of six has no register to
  // land on and every register after it would be renamed by a guess.
  EXPECT_FALSE(decodeEncoding(
      Arch::X64, kX86_64ModeStackImmediate | (1u << 10) | 6u, Entry));
  EXPECT_TRUE(Entry.SavedRegisterSlots.empty());
  EXPECT_EQ(Entry.SavedGPRMask, 0u);
}

//===----------------------------------------------------------------------===//
// i386
//===----------------------------------------------------------------------===//

TEST(CompactUnwindX86, UsesItsOwnRegisterTable) {
  CompactUnwindEntry Entry;
  // Slot numbers two and three name ecx and edx here, where the 64-bit table
  // has r12 and r13 at the same numbers.
  ASSERT_TRUE(decodeEncoding(
      Arch::X86, kX86ModeEBPFrame | (2u << 16) | frameRegisters({1, 2}),
      Entry));

  EXPECT_EQ(slotRegisters(Entry), (std::vector<int>{3 /* ebx */, 1 /* ecx */}));
  // An i386 slot is four bytes, not eight.
  EXPECT_EQ(Entry.FrameOffset, 8u);
}

TEST(CompactUnwindX86, ScalesTheImmediateStackSizeByFour) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(
      decodeEncoding(Arch::X86, kX86ModeStackImmediate | (10u << 16), Entry));
  EXPECT_EQ(Entry.StackSize, 40u);
  EXPECT_TRUE(Entry.HasStackSize);
}

//===----------------------------------------------------------------------===//
// ARM64
//===----------------------------------------------------------------------===//

TEST(CompactUnwindARM64, DecodesFrameRegisterPairs) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(Arch::AArch64,
                             kARM64ModeFrame | kARM64FrameX19X20Pair |
                                 kARM64FrameX23X24Pair | kARM64FrameD8D9Pair,
                             Entry));

  EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramePointer);
  EXPECT_EQ(slotRegisters(Entry), (std::vector<int>{19, 20, 23, 24, 8, 9}));
  EXPECT_EQ(Entry.SavedGPRMask, gprMask({19, 20, 23, 24}));
  // d8 and d9 share their numbers with x8 and x9, which is why the two files
  // cannot be reported in one mask.
  EXPECT_EQ(Entry.SavedFPRMask, gprMask({8, 9}));
  EXPECT_EQ(Entry.FrameOffset, 0u);
}

TEST(CompactUnwindARM64, ReportsEveryPair) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(Arch::AArch64,
                             kARM64ModeFrame | kARM64FrameX19X20Pair |
                                 kARM64FrameX21X22Pair | kARM64FrameX23X24Pair |
                                 kARM64FrameX25X26Pair | kARM64FrameX27X28Pair |
                                 kARM64FrameD8D9Pair | kARM64FrameD10D11Pair |
                                 kARM64FrameD12D13Pair | kARM64FrameD14D15Pair,
                             Entry));

  EXPECT_EQ(slotRegisters(Entry),
            (std::vector<int>{19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 8, 9, 10,
                              11, 12, 13, 14, 15}));
  EXPECT_EQ(Entry.SavedGPRMask,
            gprMask({19, 20, 21, 22, 23, 24, 25, 26, 27, 28}));
  EXPECT_EQ(Entry.SavedFPRMask, gprMask({8, 9, 10, 11, 12, 13, 14, 15}));
}

TEST(CompactUnwindARM64, FramelessLeafStillReportsItsSavedPairs) {
  CompactUnwindEntry Entry;
  // A leaf that saves callee-saved registers without establishing a frame
  // pointer is still frameless, and LLVM's ARM64 backend emits exactly this
  // combination of a stack size and pair bits.
  ASSERT_TRUE(decodeEncoding(Arch::AArch64,
                             kARM64ModeFrameless | (3u << 12) |
                                 kARM64FrameX19X20Pair | kARM64FrameD14D15Pair,
                             Entry));

  EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramelessImmediate);
  EXPECT_EQ(Entry.StackSize, 48u);
  EXPECT_TRUE(Entry.HasStackSize);
  EXPECT_EQ(slotRegisters(Entry), (std::vector<int>{19, 20, 14, 15}));
  EXPECT_EQ(Entry.SavedGPRMask, gprMask({19, 20}));
  EXPECT_EQ(Entry.SavedFPRMask, gprMask({14, 15}));
}

TEST(CompactUnwindARM64, DwarfModeCarriesNoRegisters) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(Arch::AArch64, kARM64ModeDwarf | 0x1234, Entry));
  EXPECT_EQ(Entry.Kind, CompactUnwindKind::DwarfFDE);
  EXPECT_EQ(Entry.DwarfFDEOffset, 0x1234u);
  EXPECT_TRUE(Entry.SavedRegisterSlots.empty());
  EXPECT_FALSE(Entry.HasStackSize);
}

//===----------------------------------------------------------------------===//
// Frameless indirect
//===----------------------------------------------------------------------===//

TEST(CompactUnwindIndirect, ReadsTheFrameSizeFromThePrologue) {
  BinaryImage Img = makeImage();
  writeStackSubtract(Img, 0x100, 0x1234);

  // Offset three is where `subq $imm32, %rsp` keeps its immediate; the adjust
  // field adds the pushes the linker counted on top of it.
  const uint32_t Encoding =
      x86_64FramelessEncoding(kX86_64ModeStackIndirect, 3, 2, {CU_RBX, CU_R15});
  attachUnwindInfo(Img, buildUnwindInfo({{0x100, Encoding}}, 0x140));

  ParseResult Result = parseCompactUnwind(Img);
  ASSERT_EQ(Result.Entries.size(), 1u);
  const CompactUnwindEntry &Entry = Result.Entries.front();

  EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramelessIndirect);
  EXPECT_TRUE(Entry.HasStackSize);
  EXPECT_EQ(Entry.StackSize, 0x1234u + 2u * 8u);
  EXPECT_EQ(slotRegisters(Entry), (std::vector<int>{RBX, R15}));
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(CompactUnwindIndirect, AddsNoAdjustmentWhenTheFieldIsZero) {
  BinaryImage Img = makeImage();
  writeStackSubtract(Img, 0x200, 0x8000);
  attachUnwindInfo(
      Img, buildUnwindInfo({{0x200, x86_64FramelessEncoding(
                                        kX86_64ModeStackIndirect, 3, 0, {})}},
                           0x280));

  ParseResult Result = parseCompactUnwind(Img);
  ASSERT_EQ(Result.Entries.size(), 1u);
  EXPECT_TRUE(Result.Entries.front().HasStackSize);
  EXPECT_EQ(Result.Entries.front().StackSize, 0x8000u);
}

TEST(CompactUnwindIndirect, DegradesWhenTheImmediateLeavesTheFunction) {
  BinaryImage Img = makeImage();
  // The range holds two bytes, so the four-byte immediate at offset three
  // would be read out of some neighbouring function.
  attachUnwindInfo(
      Img, buildUnwindInfo({{0x300, x86_64FramelessEncoding(
                                        kX86_64ModeStackIndirect, 3, 0, {})}},
                           0x302));

  ParseResult Result = parseCompactUnwind(Img);
  ASSERT_EQ(Result.Entries.size(), 1u);
  EXPECT_FALSE(Result.Entries.front().HasStackSize);
  EXPECT_EQ(Result.Entries.front().StackSize, 0u);
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Partial);
  ASSERT_FALSE(Result.Diagnostics.empty());
}

TEST(CompactUnwindIndirect, DegradesWhenTheTextIsNotMapped) {
  BinaryImage Img = makeImage();
  // A range past the end of the segment: the section still describes it, but
  // no bytes back it.
  attachUnwindInfo(
      Img, buildUnwindInfo({{0x8000, x86_64FramelessEncoding(
                                         kX86_64ModeStackIndirect, 3, 1, {})}},
                           0x8100));

  ParseResult Result = parseCompactUnwind(Img);
  ASSERT_EQ(Result.Entries.size(), 1u);
  EXPECT_FALSE(Result.Entries.front().HasStackSize);
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Partial);
}

//===----------------------------------------------------------------------===//
// Whole-section decoding
//===----------------------------------------------------------------------===//

TEST(CompactUnwindSection, DecodesRegistersThroughTheSection) {
  BinaryImage Img = makeImage();
  attachUnwindInfo(
      Img,
      buildUnwindInfo({{0x100, x86_64FrameEncoding(2, {CU_RBX, CU_R15})},
                       {0x200, x86_64FramelessEncoding(
                                   kX86_64ModeStackImmediate, 4, 0, {CU_R12})}},
                      0x300));

  ParseResult Result = parseCompactUnwind(Img);
  ASSERT_EQ(Result.Entries.size(), 2u);

  EXPECT_EQ(Result.Entries[0].CodeRange.Begin, kImageBase + 0x100);
  EXPECT_EQ(Result.Entries[0].CodeRange.End, kImageBase + 0x200);
  EXPECT_EQ(Result.Entries[0].SavedGPRMask, gprMask({RBX, R15}));
  EXPECT_EQ(Result.Entries[0].FrameOffset, 16u);

  EXPECT_EQ(Result.Entries[1].SavedGPRMask, gprMask({R12}));
  EXPECT_EQ(Result.Entries[1].StackSize, 32u);
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(CompactUnwindSection, ReportsAnEntryWhoseRegistersDoNotDecode) {
  BinaryImage Img = makeImage();
  attachUnwindInfo(
      Img, buildUnwindInfo({{0x100, x86_64FrameEncoding(1, {CU_RBP})}}, 0x200));

  ParseResult Result = parseCompactUnwind(Img);
  ASSERT_EQ(Result.Entries.size(), 1u);
  EXPECT_TRUE(Result.Entries.front().SavedRegisterSlots.empty());
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Partial);
  ASSERT_FALSE(Result.Diagnostics.empty());
}

TEST(CompactUnwindSection, RejectsAnUnsupportedVersion) {
  BinaryImage Img = makeImage();
  attachUnwindInfo(Img, buildUnwindInfo({{0x100, kX86_64ModeRBPFrame}}, 0x200,
                                        /*Version=*/2));

  ParseResult Result = parseCompactUnwind(Img);
  EXPECT_TRUE(Result.Entries.empty());
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Malformed);
}

TEST(CompactUnwindSection, RejectsAnIndexWithNoSentinel) {
  BinaryImage Img = makeImage();
  attachUnwindInfo(Img, buildUnwindInfo({{0x100, kX86_64ModeRBPFrame}}, 0x200,
                                        /*Version=*/1, /*IndexCount=*/1));

  ParseResult Result = parseCompactUnwind(Img);
  EXPECT_TRUE(Result.Entries.empty());
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Malformed);
}

TEST(CompactUnwindSection, RejectsATruncatedSection) {
  BinaryImage Img = makeImage();
  std::vector<uint8_t> Bytes =
      buildUnwindInfo({{0x100, kX86_64ModeRBPFrame}}, 0x200);
  // Keep the header but cut the page it points at.
  Bytes.resize(40);
  attachUnwindInfo(Img, std::move(Bytes));

  ParseResult Result = parseCompactUnwind(Img);
  EXPECT_TRUE(Result.Entries.empty());
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Malformed);
}

TEST(CompactUnwindSection, ModeZeroIsAnAbsenceOfInformation) {
  BinaryImage Img = makeImage();
  attachUnwindInfo(Img, buildUnwindInfo({{0x100, 0}}, 0x200));

  ParseResult Result = parseCompactUnwind(Img);
  ASSERT_EQ(Result.Entries.size(), 1u);
  EXPECT_EQ(Result.Entries.front().Kind, CompactUnwindKind::None);
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);
}

} // namespace
