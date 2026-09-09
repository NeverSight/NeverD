//===- FunctionDiscoveryAlignmentTests.cpp - Candidate address checks -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

Segment executableSegment(va_t Address, llvm::ArrayRef<uint8_t> Bytes) {
  Segment Seg;
  Seg.VA = Address;
  Seg.Size = Bytes.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data.assign(Bytes.begin(), Bytes.end());
  return Seg;
}

TEST(FunctionDiscoveryAlignment, UsesVirtualAddressForEveryArchitecture) {
  struct Case {
    Arch Architecture;
    uint64_t Alignment;
    std::array<uint8_t, 4> Prologue;
  };
  const Case Cases[] = {
      {Arch::AArch64, 4, {0xff, 0x83, 0x00, 0xd1}}, // sub sp, sp, #32
      {Arch::ARM, 2, {0x10, 0xb5, 0x70, 0x47}},     // push; bx lr (Thumb)
      {Arch::X86, 1, {0x55, 0x89, 0xe5, 0xc3}},
      {Arch::X64, 1, {0x55, 0x48, 0x89, 0xe5}},
  };
  for (const Case &C : Cases) {
    for (va_t BaseOffset = 0; BaseOffset != 4; ++BaseOffset) {
      for (size_t Off = 0; Off != 8; ++Off) {
        SCOPED_TRACE(::testing::Message()
                     << "arch=" << static_cast<unsigned>(C.Architecture)
                     << " base offset=" << BaseOffset << " offset=" << Off);
        std::vector<uint8_t> Bytes(12, 0);
        std::copy(C.Prologue.begin(), C.Prologue.end(), Bytes.begin() + Off);
        const Segment Seg = executableSegment(0x1000 + BaseOffset, Bytes);
        EXPECT_EQ(checkPrologueAtOffset(Seg, Off, C.Architecture),
                  (Seg.VA + Off) % C.Alignment == 0);
      }
    }
  }
}

TEST(FunctionDiscoveryAlignment, RejectsWrappedAndOutOfBufferAddresses) {
  const std::array<uint8_t, 8> Bytes = {0xff, 0x83, 0x00, 0xd1,
                                        0xff, 0x83, 0x00, 0xd1};
  const Segment Seg = executableSegment(InvalidVA - 3, Bytes);
  EXPECT_TRUE(checkPrologueAtOffset(Seg, 0, Arch::AArch64));
  EXPECT_FALSE(checkPrologueAtOffset(Seg, 4, Arch::AArch64));
  EXPECT_FALSE(checkPrologueAtOffset(Seg, Bytes.size(), Arch::AArch64));
}

TEST(FunctionDiscoveryAlignment,
     PaddingDoesNotCreateEntriesInsideAArch64Instructions) {
  // Actual bytes at hello's 0x1000004e0: str wzr; adrp x8; add x8.
  // The zero run at 4e5/4e6 previously made 4e7's 90 08 01 15 look like B.
  constexpr va_t MainVA = 0x1000004c4;
  const std::array<uint8_t, 12> Instructions = {
      0xff, 0x0f, 0x00, 0xb9, 0x08, 0x00, 0x00, 0x90, 0x08, 0x01, 0x15, 0x91};
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    BinaryImage Img;
    Img.Arch = Arch::AArch64;
    Img.Bits = Bitness::Bits64;
    Img.Format = Format;
    std::vector<uint8_t> Bytes(0x40, 0xff);
    std::copy(Instructions.begin(), Instructions.end(), Bytes.begin() + 0x1c);
    Img.Segments.push_back(executableSegment(MainVA, Bytes));
    Img.Symbols.push_back(Symbol::makeFunc(MainVA));

    scanPaddingBoundaries(Img);

    ASSERT_EQ(Img.Symbols.size(), 1u);
    EXPECT_EQ(Img.Symbols.front().Addr, MainVA);
  }
}

TEST(FunctionDiscoveryAlignment, PreservesAlignedEntriesAfterPadding) {
  struct Case {
    Arch Architecture;
    size_t Offset;
    std::array<uint8_t, 4> Prologue;
  };
  const Case Cases[] = {
      {Arch::AArch64, 8, {0xff, 0x83, 0x00, 0xd1}},
      {Arch::ARM, 6, {0x10, 0xb5, 0x70, 0x47}},
      {Arch::X86, 5, {0x55, 0x89, 0xe5, 0xc3}},
      {Arch::X64, 5, {0x55, 0x48, 0x89, 0xe5}},
  };
  for (const Case &C : Cases) {
    SCOPED_TRACE(static_cast<unsigned>(C.Architecture));
    BinaryImage Img;
    Img.Arch = C.Architecture;
    std::vector<uint8_t> Bytes(C.Offset + 4, codePaddingByte(Img.Arch));
    // The scanner deliberately skips a segment's initial padding run.
    Bytes[0] = 0x12;
    std::copy(C.Prologue.begin(), C.Prologue.end(), Bytes.begin() + C.Offset);
    Img.Segments.push_back(executableSegment(0x1000, Bytes));

    scanPaddingBoundaries(Img);

    ASSERT_EQ(Img.Symbols.size(), 1u);
    EXPECT_EQ(Img.Symbols.front().Addr, 0x1000 + C.Offset);
  }
}

TEST(FunctionDiscoveryAlignment, DataPointersRejectMisalignedAArch64Entries) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  std::vector<uint8_t> Bytes(16, 0xff);
  writeLE<uint32_t>(Bytes.data() + 1, 0xd10083ffu);
  writeLE<uint32_t>(Bytes.data() + 8, 0xd10083ffu);
  Img.Segments.push_back(executableSegment(0x1000, Bytes));
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = 16;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(16);
  writeLE<uint64_t>(Data.Data.data(), 0x1001);
  writeLE<uint64_t>(Data.Data.data() + 8, 0x1008);
  Img.Segments.push_back(std::move(Data));

  scanDataFuncPointers(Img);

  ASSERT_EQ(Img.Symbols.size(), 1u);
  EXPECT_EQ(Img.Symbols.front().Addr, 0x1008u);
}

TEST(FunctionDiscoveryAlignment,
     PreservesTaggedThumbPointersAndOddDataSymbols) {
  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Mode = InstructionMode::Thumb;
  Img.Bits = Bitness::Bits32;
  const std::array<uint8_t, 4> Code = {0x10, 0xb5, 0x70, 0x47};
  Img.Segments.push_back(executableSegment(0x1002, Code));
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = 4;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(4);
  writeLE<uint32_t>(Data.Data.data(), 0x1003);
  Img.Segments.push_back(std::move(Data));
  Symbol Label;
  Label.Name = "odd_data";
  Label.Addr = 0x2001;
  Img.Symbols.push_back(Label);

  scanDataFuncPointers(Img);

  ASSERT_EQ(Img.Symbols.size(), 2u);
  EXPECT_EQ(Img.Symbols[0].Addr, 0x2001u);
  EXPECT_EQ(Img.Symbols[0].Name, "odd_data");
  EXPECT_FALSE(Img.Symbols[0].IsFunc);
  EXPECT_EQ(Img.Symbols[1].Addr, 0x1002u);
  EXPECT_TRUE(Img.Symbols[1].IsFunc);
}

} // namespace
