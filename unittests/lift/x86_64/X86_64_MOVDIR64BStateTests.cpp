//===- X86_64_MOVDIR64BStateTests.cpp - direct-store semantics -----------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <cstdint>
#include <exception>
#include <optional>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;

std::vector<LowOp> liftX64(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  if (!Dec.init(Arch::X64)) {
    ADD_FAILURE() << "failed to initialize x86-64 decoder";
    return {};
  }
  DecodedInsn Insn{};
  if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kInstructionAddress,
                           Insn) != static_cast<int>(Bytes.size())) {
    ADD_FAILURE() << "failed to decode complete MOVDIR64B";
    return {};
  }
  if (Insn.Id != X86_INS_MOVDIR64B) {
    ADD_FAILURE() << "decoded the wrong instruction";
    return {};
  }
  std::vector<LowOp> Ops;
  try {
    Dec.liftToLow(Insn, Ops);
  } catch (const std::exception &Error) {
    ADD_FAILURE() << Error.what();
    return {};
  }
  return Ops;
}

void addSegment(BinaryImage &Image, uint64_t Address, std::vector<uint8_t> Data,
                SegmentFlags Flags) {
  Segment S;
  S.VA = Address;
  S.Size = Data.size();
  S.Flags = Flags;
  S.Data = std::move(Data);
  Image.Segments.push_back(std::move(S));
}

std::optional<std::vector<uint8_t>> readVector(NdOpEmulator &Emulator,
                                               uint64_t Address) {
  LowOp Load;
  Load.Addr = kInstructionAddress + 1;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::reg(x86reg::vectorReg(31), 64);
  Load.addInput(NdVar::cst(Address, 8));
  if (!Emulator.step(Load))
    return std::nullopt;
  return Emulator.getRegisterBytes(x86reg::vectorReg(31));
}

std::optional<uint64_t> readQword(NdOpEmulator &Emulator, uint64_t Address) {
  LowOp Load;
  Load.Addr = kInstructionAddress + 1;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::reg(x86reg::RAX, 8);
  Load.addInput(NdVar::cst(Address, 8));
  if (!Emulator.step(Load))
    return std::nullopt;
  return Emulator.getRegister(x86reg::RAX);
}

std::vector<uint8_t> pattern(uint8_t Seed) {
  std::vector<uint8_t> Bytes(64);
  for (size_t Index = 0; Index < Bytes.size(); ++Index)
    Bytes[Index] = static_cast<uint8_t>(Seed + Index * 13);
  return Bytes;
}

TEST(X86MOVDIR64BState, ReadsThenPerformsOneAlignedAtomicDestinationStore) {
  const std::vector<LowOp> Ops =
      liftX64({0x66, 0x0f, 0x38, 0xf8, 0x0b}); // rcx, [rbx]
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t SourceAddress = 0x4103;
  constexpr uint64_t DestinationAddress = 0x5000;
  const std::vector<uint8_t> Source = pattern(0x21);
  const std::vector<uint8_t> OldDestination(64, 0xa5);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  addSegment(Image, SourceAddress, Source, SegmentFlags::Readable);
  addSegment(Image, DestinationAddress, OldDestination,
             SegmentFlags::Readable | SegmentFlags::Writable);

  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::RBX, SourceAddress);
  Emulator.setRegister(x86reg::RCX, DestinationAddress);
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegister(x86reg::RCX), DestinationAddress);
  ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
  EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, SourceAddress);
  EXPECT_EQ(Emulator.getLoadRecords()[0].Size, 64);
  EXPECT_EQ(readVector(Emulator, DestinationAddress), Source);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86MOVDIR64BState, SnapshotsAnOverlappingSourceBeforeTheStore) {
  const std::vector<LowOp> Ops = liftX64({0x66, 0x0f, 0x38, 0xf8, 0x0b});
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Base = 0x6000;
  std::vector<uint8_t> Window(72);
  for (size_t Index = 0; Index < Window.size(); ++Index)
    Window[Index] = static_cast<uint8_t>(Index ^ 0x5a);
  const std::vector<uint8_t> Expected(Window.begin() + 8, Window.end());

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  addSegment(Image, Base, Window,
             SegmentFlags::Readable | SegmentFlags::Writable);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::RBX, Base + 8);
  Emulator.setRegister(x86reg::RCX, Base);
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(readVector(Emulator, Base), Expected);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86MOVDIR64BState, FaultsWithoutPartialDestinationWrite) {
  const std::vector<LowOp> Ops = liftX64({0x66, 0x0f, 0x38, 0xf8, 0x0b});
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t SourceAddress = 0x7000;
  constexpr uint64_t DestinationAddress = 0x8000;
  constexpr uint64_t OldFirstQword = UINT64_C(0x8877665544332211);
  std::vector<uint8_t> FirstQword(8);
  for (unsigned Index = 0; Index < 8; ++Index)
    FirstQword[Index] = static_cast<uint8_t>(OldFirstQword >> (Index * 8));

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  addSegment(Image, SourceAddress, pattern(0x37), SegmentFlags::Readable);
  addSegment(Image, DestinationAddress, FirstQword,
             SegmentFlags::Readable | SegmentFlags::Writable);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::RBX, SourceAddress);
  Emulator.setRegister(x86reg::RCX, DestinationAddress);
  EXPECT_LT(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(readQword(Emulator, DestinationAddress), OldFirstQword);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86MOVDIR64BState, SourceFaultLeavesTheDestinationUntouched) {
  const std::vector<LowOp> Ops = liftX64({0x66, 0x0f, 0x38, 0xf8, 0x0b});
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t DestinationAddress = 0x8800;
  const std::vector<uint8_t> OldDestination(64, 0x3d);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  addSegment(Image, DestinationAddress, OldDestination,
             SegmentFlags::Readable | SegmentFlags::Writable);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::RBX, UINT64_C(0xdead0000));
  Emulator.setRegister(x86reg::RCX, DestinationAddress);
  EXPECT_LT(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(readVector(Emulator, DestinationAddress), OldDestination);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86MOVDIR64BState, ChecksAlignmentAfterTheCompleteSourceRead) {
  const std::vector<LowOp> Ops = liftX64({0x66, 0x0f, 0x38, 0xf8, 0x0b});
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t SourceAddress = 0x9000;
  constexpr uint64_t DestinationBase = 0xa000;
  const std::vector<uint8_t> OldDestination(64, 0x6c);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  addSegment(Image, SourceAddress, pattern(0x49), SegmentFlags::Readable);
  addSegment(Image, DestinationBase, OldDestination,
             SegmentFlags::Readable | SegmentFlags::Writable);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::RBX, SourceAddress);
  Emulator.setRegister(x86reg::RCX, DestinationBase + 8);
  EXPECT_LT(Emulator.run(Ops), Ops.size());
  ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
  EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, SourceAddress);
  EXPECT_EQ(readVector(Emulator, DestinationBase), OldDestination);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86MOVDIR64BState, KeepsSourceSegmentAndDestinationAddressIndependent) {
  const std::vector<LowOp> Ops =
      liftX64({0x64, 0x66, 0x0f, 0x38, 0xf8, 0x0b}); // rcx, fs:[rbx]
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t FsBase = 0xb000;
  constexpr uint64_t SourceOffset = 0x80;
  constexpr uint64_t DestinationAddress = 0xc000;
  const std::vector<uint8_t> Source = pattern(0x73);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  addSegment(Image, FsBase + SourceOffset, Source, SegmentFlags::Readable);
  addSegment(Image, DestinationAddress, std::vector<uint8_t>(64),
             SegmentFlags::Readable | SegmentFlags::Writable);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  ASSERT_TRUE(
      Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS, FsBase));
  Emulator.setRegister(x86reg::RBX, SourceOffset);
  Emulator.setRegister(x86reg::RCX, DestinationAddress);
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(readVector(Emulator, DestinationAddress), Source);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86MOVDIR64BState, AddressSizeOverrideZeroExtendsBothAddressRegisters) {
  const std::vector<LowOp> Ops =
      liftX64({0x67, 0x66, 0x0f, 0x38, 0xf8, 0x0b}); // ecx, [ebx]
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t SourceAddress = 0xd000;
  constexpr uint64_t DestinationAddress = 0xe000;
  const std::vector<uint8_t> Source = pattern(0x91);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  addSegment(Image, SourceAddress, Source, SegmentFlags::Readable);
  addSegment(Image, DestinationAddress, std::vector<uint8_t>(64),
             SegmentFlags::Readable | SegmentFlags::Writable);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::RBX,
                       UINT64_C(0xaaaaaaaa00000000) | SourceAddress);
  Emulator.setRegister(x86reg::RCX,
                       UINT64_C(0xbbbbbbbb00000000) | DestinationAddress);
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(readVector(Emulator, DestinationAddress), Source);
  EXPECT_FALSE(Emulator.skips().any());
}

} // namespace
