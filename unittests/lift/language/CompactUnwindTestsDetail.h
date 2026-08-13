//===- CompactUnwindTestsDetail.h - compact unwind test harness -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The register tables, encoders and __unwind_info assembler shared by
// the CompactUnwind* translation units.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_LANGUAGE_COMPACTUNWINDTESTSDETAIL_H
#define NEVERD_UNITTESTS_LIFT_LANGUAGE_COMPACTUNWINDTESTSDETAIL_H


#include "gtest/gtest.h"

#include "neverd/loader/MachO/CompactUnwind.h"

#include <algorithm>
#include <vector>

namespace neverd::compact_unwind_test {

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
inline uint32_t permuteEncode(const std::vector<uint32_t> &Regs) {
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
inline uint32_t frameRegisters(std::initializer_list<uint32_t> Slots) {
  uint32_t Encoding = 0;
  unsigned Index = 0;
  for (uint32_t Slot : Slots)
    Encoding |= (Slot & 0x7) << (3 * Index++);
  return Encoding;
}

inline uint32_t x86_64FrameEncoding(uint32_t SavedRegisterCount,
                             std::initializer_list<uint32_t> Slots) {
  return kX86_64ModeRBPFrame | (SavedRegisterCount << 16) |
         frameRegisters(Slots);
}

inline uint32_t x86_64FramelessEncoding(uint32_t Mode, uint32_t Field16,
                                 uint32_t Adjust,
                                 const std::vector<uint32_t> &Regs) {
  return Mode | (Field16 << 16) | (Adjust << 13) |
         (static_cast<uint32_t>(Regs.size()) << 10) | permuteEncode(Regs);
}

/// The machine number the decoder should report for a compact register
/// number.
inline int machineNumber(uint32_t CompactNumber) {
  static const int Table[] = {0, RBX, R12, R13, R14, R15, RBP};
  return Table[CompactNumber];
}

inline std::vector<int> machineNumbers(const std::vector<uint32_t> &Regs) {
  std::vector<int> Result;
  for (uint32_t Reg : Regs)
    Result.push_back(machineNumber(Reg));
  return Result;
}

/// The machine register numbers of every slot, in slot order.  An empty slot
/// is reported as -1 so that a gap is visible in a failure message.
inline std::vector<int> slotRegisters(const CompactUnwindEntry &Entry) {
  std::vector<int> Result;
  for (const CompactUnwindRegisterSlot &Slot : Entry.SavedRegisterSlots)
    Result.push_back(Slot.RegisterClass == UnwindRegisterClass::None
                         ? -1
                         : static_cast<int>(Slot.Register));
  return Result;
}

inline uint32_t gprMask(std::initializer_list<unsigned> Registers) {
  uint32_t Mask = 0;
  for (unsigned Reg : Registers)
    Mask |= uint32_t(1) << Reg;
  return Mask;
}

//===----------------------------------------------------------------------===//
// Synthetic image
//===----------------------------------------------------------------------===//

inline constexpr va_t kImageBase = 0x100000000ull;
inline constexpr uint64_t kTextSize = 0x4000;

/// A Mach-O image with one `__TEXT` segment, which is what makes its virtual
/// address the image base every `functionOffset` is measured from.
inline BinaryImage makeImage(Arch A = Arch::X64) {
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
inline std::vector<uint8_t> buildUnwindInfo(const std::vector<RawEntry> &Entries,
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

inline void attachUnwindInfo(BinaryImage &Img, std::vector<uint8_t> Bytes) {
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
inline void writeStackSubtract(BinaryImage &Img, uint32_t Offset, uint32_t Immediate) {
  const uint8_t Opcode[3] = {0x48, 0x81, 0xec};
  ASSERT_TRUE(Img.writeVA(kImageBase + Offset, Opcode, sizeof(Opcode)));
  ASSERT_TRUE(Img.writeVA(kImageBase + Offset + 3,
                          reinterpret_cast<const uint8_t *>(&Immediate),
                          sizeof(Immediate)));
}

//===----------------------------------------------------------------------===//
// x86-64 frame pointer
//===----------------------------------------------------------------------===//

} // namespace neverd::compact_unwind_test

#endif // NEVERD_UNITTESTS_LIFT_LANGUAGE_COMPACTUNWINDTESTSDETAIL_H
