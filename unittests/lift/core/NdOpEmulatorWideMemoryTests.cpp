//===- NdOpEmulatorWideMemoryTests.cpp - exact wide memory semantics ------===//

#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/loader/BinaryImage.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

using namespace neverd;

namespace {

BinaryImage imageWithSegment(uint64_t Address, uint64_t Size,
                             SegmentFlags Flags, uint8_t Fill = 0) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Memory;
  Memory.VA = Address;
  Memory.Size = Size;
  Memory.Flags = Flags;
  Memory.Data.assign(Size, Fill);
  Image.Segments.push_back(std::move(Memory));
  return Image;
}

LowOp load(uint64_t Address, uint64_t Output, uint16_t Size) {
  LowOp Op;
  Op.Opcode = NdOp::LOAD;
  Op.Output = NdVar::reg(Output, Size);
  Op.addInput(NdVar::cst(0, 8));
  Op.addInput(NdVar::cst(Address, 8));
  return Op;
}

LowOp store(uint64_t Address, uint64_t Source, uint16_t Size) {
  LowOp Op;
  Op.Opcode = NdOp::STORE;
  Op.addInput(NdVar::cst(0, 8));
  Op.addInput(NdVar::cst(Address, 8));
  Op.addInput(NdVar::reg(Source, Size));
  return Op;
}

std::vector<uint8_t> sequence(uint16_t Size, uint8_t First) {
  std::vector<uint8_t> Bytes(Size);
  std::iota(Bytes.begin(), Bytes.end(), First);
  return Bytes;
}

TEST(NdOpEmulatorWideMemory, StoresAndReloads16_32_64BytesExactly) {
  constexpr uint64_t Base = 0x4000;
  BinaryImage Image = imageWithSegment(
      Base, 0x200, SegmentFlags::Readable | SegmentFlags::Writable, 0xa5);

  for (uint16_t Size : {uint16_t(16), uint16_t(32), uint16_t(64)}) {
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    const std::vector<uint8_t> Expected = sequence(Size, uint8_t(Size));
    Emulator.setRegisterBytes(0x100, Expected);

    ASSERT_TRUE(Emulator.step(store(Base + 0x40, 0x100, Size))) << Size;
    ASSERT_TRUE(Emulator.step(load(Base + 0x40, 0x200, Size))) << Size;
    const auto Actual = Emulator.getRegisterBytes(0x200);
    ASSERT_TRUE(Actual.has_value()) << Size;
    EXPECT_EQ(*Actual, Expected) << Size;
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(NdOpEmulatorWideMemory, FaultingWideStoreCommitsNoPrefixBytes) {
  constexpr uint64_t Base = 0x5000;
  BinaryImage Image = imageWithSegment(
      Base, 8, SegmentFlags::Readable | SegmentFlags::Writable, 0x5a);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(0x100, sequence(16, 0x80));

  EXPECT_FALSE(Emulator.step(store(Base, 0x100, 16)));
  ASSERT_TRUE(Emulator.step(load(Base, 0x200, 8)));
  EXPECT_EQ(Emulator.getRegister(0x200).value_or(0),
            UINT64_C(0x5a5a5a5a5a5a5a5a));
}

TEST(NdOpEmulatorWideMemory, WideAccessCannotWrapIntoLowMemory) {
  constexpr uint64_t High = std::numeric_limits<uint64_t>::max() - 7;
  BinaryImage Image = imageWithSegment(
      High, 8, SegmentFlags::Readable | SegmentFlags::Writable, 0x11);
  Segment Low;
  Low.VA = 0;
  Low.Size = 64;
  Low.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Low.Data.assign(64, 0x22);
  Image.Segments.push_back(std::move(Low));

  NdOpEmulator LoadEmulator(Image);
  LoadEmulator.setStrictMode(true);
  LoadEmulator.setRegisterBytes(0x200, std::vector<uint8_t>(16, 0xcc));
  EXPECT_FALSE(LoadEmulator.step(load(High, 0x200, 16)));
  EXPECT_EQ(LoadEmulator.getRegisterBytes(0x200),
            std::optional<std::vector<uint8_t>>(
                std::vector<uint8_t>(16, 0xcc)));

  NdOpEmulator StoreEmulator(Image);
  StoreEmulator.setStrictMode(true);
  StoreEmulator.setRegisterBytes(0x100, sequence(16, 0x40));
  EXPECT_FALSE(StoreEmulator.step(store(High, 0x100, 16)));
  ASSERT_TRUE(StoreEmulator.step(load(High, 0x300, 8)));
  ASSERT_TRUE(StoreEmulator.step(load(0, 0x308, 8)));
  EXPECT_EQ(StoreEmulator.getRegister(0x300).value_or(0),
            UINT64_C(0x1111111111111111));
  EXPECT_EQ(StoreEmulator.getRegister(0x308).value_or(0),
            UINT64_C(0x2222222222222222));
}

TEST(NdOpEmulatorWideMemory, SegmentBaseAdditionOverflowFailsClosed) {
  BinaryImage Image = imageWithSegment(
      0, 64, SegmentFlags::Readable | SegmentFlags::Writable, 0x33);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(
      NdMemoryAddressSpace::X86GS,
      std::numeric_limits<uint64_t>::max() - 3));

  LowOp Load = load(8, 0x200, 8);
  Load.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  EXPECT_FALSE(Emulator.step(Load));
  EXPECT_FALSE(Emulator.getRegister(0x200).has_value());
}

} // namespace
