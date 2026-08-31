//===- X86_64_EVEXMaskedMoveTests.cpp - EVEX masked memory moves ---------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;

std::vector<uint8_t> moveBytes(unsigned ElementBytes, unsigned VectorBytes,
                               bool Store, bool Zero = false) {
  uint8_t WidthByte = 0;
  switch (ElementBytes) {
  case 1:
    WidthByte = 0x7f;
    break;
  case 2:
    WidthByte = 0xff;
    break;
  case 4:
    WidthByte = 0x7e;
    break;
  case 8:
    WidthByte = 0xfe;
    break;
  default:
    ADD_FAILURE() << "invalid element width";
    return {};
  }
  uint8_t LengthMask = VectorBytes == 16   ? 0x09
                       : VectorBytes == 32 ? 0x29
                       : VectorBytes == 64 ? 0x49
                                           : 0;
  if (LengthMask == 0) {
    ADD_FAILURE() << "invalid vector width";
    return {};
  }
  if (Zero)
    LengthMask |= 0x80;
  // The disp8 is one full-vector tuple: 1*16, 1*32, or 1*64 bytes.
  return {0x62,
          0xf1,
          WidthByte,
          LengthMask,
          static_cast<uint8_t>(Store ? 0x7f : 0x6f),
          0x48,
          0x01};
}

std::vector<LowOp> liftX64(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  if (!Dec.init(Arch::X64)) {
    ADD_FAILURE() << "failed to initialize x86-64 decoder";
    return {};
  }
  DecodedInsn Insn{};
  const int Decoded = Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                           kInstructionAddress, Insn);
  if (Decoded != static_cast<int>(Bytes.size())) {
    ADD_FAILURE() << "failed to decode complete instruction";
    return {};
  }
  std::vector<LowOp> Ops;
  try {
    Dec.liftToLow(Insn, Ops);
  } catch (const UnliftedInstruction &) {
    ADD_FAILURE() << "instruction was not lifted";
    return {};
  }
  return Ops;
}

BinaryImage makeMemoryImage(uint64_t VA, size_t Size) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Data;
  Data.VA = VA;
  Data.Size = Size;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Size);
  Image.Segments.push_back(std::move(Data));
  return Image;
}

void writeImageBytes(BinaryImage &Image, uint64_t Address,
                     const std::vector<uint8_t> &Bytes) {
  ASSERT_EQ(Image.Segments.size(), 1u);
  Segment &Data = Image.Segments.front();
  ASSERT_GE(Address, Data.VA);
  const uint64_t Offset = Address - Data.VA;
  ASSERT_LE(Offset + Bytes.size(), Data.Data.size());
  std::memcpy(Data.Data.data() + Offset, Bytes.data(), Bytes.size());
}

uint64_t
probeValue(NdOpEmulator &Emulator, uint64_t Address, uint16_t Size,
           uint64_t Temp,
           NdMemoryAddressSpace AddressSpace = NdMemoryAddressSpace::Default) {
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::tmp(Temp, Size);
  Load.MemoryAddressSpace = AddressSpace;
  Load.addInput(NdVar::cst(0, 8));
  Load.addInput(NdVar::cst(Address, 8));
  EXPECT_TRUE(Emulator.step(Load));
  return Emulator.getRegister(Temp).value_or(UINT64_MAX);
}

std::vector<uint8_t> pattern(size_t Size, uint8_t Seed) {
  std::vector<uint8_t> Bytes(Size);
  for (size_t Index = 0; Index < Size; ++Index)
    Bytes[Index] = static_cast<uint8_t>(Seed + Index * 13u);
  return Bytes;
}

uint64_t alternatingMask(unsigned Lanes) {
  uint64_t Value = 0;
  for (unsigned Lane = 0; Lane < Lanes; Lane += 2)
    Value |= UINT64_C(1) << Lane;
  return Value;
}

void expectMalformedShapeRejected(const std::vector<uint8_t> &Bytes,
                                  const std::function<void(cs_x86 &)> &Mutate) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                 kInstructionAddress, Insn),
            static_cast<int>(Bytes.size()));
  ASSERT_NE(Insn.Raw, nullptr);
  ASSERT_NE(Insn.Raw->detail, nullptr);
  Mutate(Insn.Raw->detail->x86);
  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

TEST(X86EVEXMaskedMove, ExactMemoryMaskingAddressingAndFaultAtomicity) {
  uint64_t ProbeTemp = UINT64_C(0x71000000);

  // Every legal X/Y/Z and byte/word/dword/qword combination.  The encoded
  // disp8 is compressed by the full vector tuple, not a literal byte.
  for (unsigned ElementBytes : {1u, 2u, 4u, 8u}) {
    for (unsigned VectorBytes : {16u, 32u, 64u}) {
      const unsigned Lanes = VectorBytes / ElementBytes;
      const uint64_t Relevant =
          Lanes == 64 ? UINT64_MAX : ((UINT64_C(1) << Lanes) - 1);
      const uint64_t Mask =
          alternatingMask(Lanes) | (UINT64_C(0xa5a55a5af0f00f0f) & ~Relevant);
      const uint64_t Base = 0x4000;
      const uint64_t Address = Base + VectorBytes;
      const std::vector<uint8_t> Memory = pattern(VectorBytes, 0x31);
      const std::vector<uint8_t> Old = pattern(64, 0x91);
      const std::vector<uint8_t> Source = pattern(64, 0x51);

      for (bool Zero : {false, true}) {
        SCOPED_TRACE("load elem=" + std::to_string(ElementBytes) +
                     " vec=" + std::to_string(VectorBytes) +
                     " zero=" + std::to_string(Zero));
        BinaryImage Image = makeMemoryImage(0x4000, 0x200);
        writeImageBytes(Image, Address, Memory);
        const std::vector<LowOp> Ops =
            liftX64(moveBytes(ElementBytes, VectorBytes, false, Zero));
        ASSERT_FALSE(Ops.empty());
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setRegister(x86reg::RAX, Base);
        Emulator.setRegister(x86reg::K1, Mask);
        Emulator.setRegisterBytes(x86reg::XMM1, Old);
        ASSERT_EQ(Emulator.run(Ops), Ops.size());
        EXPECT_EQ(Emulator.getRegister(x86reg::K1), Mask);
        const auto Result = Emulator.getRegisterBytes(x86reg::XMM1);
        ASSERT_TRUE(Result);
        ASSERT_EQ(Result->size(), 64u);
        for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
          const bool Active = (Mask & (UINT64_C(1) << Lane)) != 0;
          for (unsigned Byte = 0; Byte < ElementBytes; ++Byte) {
            const unsigned Offset = Lane * ElementBytes + Byte;
            EXPECT_EQ((*Result)[Offset],
                      Active ? Memory[Offset] : (Zero ? 0 : Old[Offset]));
          }
        }
        EXPECT_TRUE(std::all_of(Result->begin() + VectorBytes, Result->end(),
                                [](uint8_t Byte) { return Byte == 0; }));
      }

      SCOPED_TRACE("store elem=" + std::to_string(ElementBytes) +
                   " vec=" + std::to_string(VectorBytes));
      BinaryImage Image = makeMemoryImage(0x4000, 0x200);
      writeImageBytes(Image, Address, Memory);
      const std::vector<LowOp> Ops =
          liftX64(moveBytes(ElementBytes, VectorBytes, true));
      ASSERT_FALSE(Ops.empty());
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setRegister(x86reg::RAX, Base);
      Emulator.setRegister(x86reg::K1, Mask);
      Emulator.setRegisterBytes(x86reg::XMM1, Source);
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      EXPECT_EQ(Emulator.getRegister(x86reg::K1), Mask);
      for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
        const bool Active = (Mask & (UINT64_C(1) << Lane)) != 0;
        const unsigned Offset = Lane * ElementBytes;
        uint64_t Expected = 0;
        const std::vector<uint8_t> &ExpectedBytes = Active ? Source : Memory;
        std::memcpy(&Expected, ExpectedBytes.data() + Offset, ElementBytes);
        EXPECT_EQ(
            probeValue(Emulator, Address + Offset, ElementBytes, ProbeTemp++),
            Expected);
      }
    }
  }

  // High vector registers plus a signed compressed displacement and SIB.
  const std::vector<uint8_t> HighSIB{0x62, 0x01, 0x7f, 0xcf,
                                     0x6f, 0x7c, 0xac, 0xff};
  BinaryImage SIBImage = makeMemoryImage(0x5000, 64);
  const std::vector<uint8_t> SIBMemory = pattern(64, 0x17);
  writeImageBytes(SIBImage, 0x5000, SIBMemory);
  NdOpEmulator SIB(SIBImage);
  SIB.setStrictMode(true);
  SIB.setRegister(x86reg::R12, 0x5000);
  SIB.setRegister(x86reg::R13, 16);
  SIB.setRegister(x86reg::K7, UINT64_MAX);
  SIB.setRegisterBytes(x86reg::vectorReg(31), pattern(64, 0xee));
  const std::vector<LowOp> SIBOps = liftX64(HighSIB);
  ASSERT_FALSE(SIBOps.empty());
  ASSERT_EQ(SIB.run(SIBOps), SIBOps.size());
  EXPECT_EQ(SIB.getRegisterBytes(x86reg::vectorReg(31)), SIBMemory);

  // RIP-relative memory uses next-IP plus the decoded displacement.
  const std::vector<uint8_t> RIPLoad{0x62, 0x61, 0xff, 0x4f, 0x6f,
                                     0x3d, 0x40, 0x00, 0x00, 0x00};
  const uint64_t RIPAddress = kInstructionAddress + RIPLoad.size() + 64;
  BinaryImage RIPImage = makeMemoryImage(kInstructionAddress, 0x200);
  const std::vector<uint8_t> RIPMemory = pattern(64, 0x24);
  writeImageBytes(RIPImage, RIPAddress, RIPMemory);
  NdOpEmulator RIP(RIPImage);
  RIP.setStrictMode(true);
  RIP.setRegister(x86reg::K7, UINT32_MAX);
  RIP.setRegisterBytes(x86reg::vectorReg(31), pattern(64, 0xaa));
  const std::vector<LowOp> RIPOps = liftX64(RIPLoad);
  ASSERT_FALSE(RIPOps.empty());
  ASSERT_EQ(RIP.run(RIPOps), RIPOps.size());
  EXPECT_EQ(RIP.getRegisterBytes(x86reg::vectorReg(31)), RIPMemory);

  // GS/FS plus addr32: EAX + the compressed displacement wraps to zero before
  // the segment base is added.  High RAX bits must not affect either access.
  const std::vector<uint8_t> GSLoad{0x65, 0x67, 0x62, 0x61, 0x7e,
                                    0x4f, 0x6f, 0x78, 0x01};
  BinaryImage GSImage = makeMemoryImage(0x7000, 64);
  const std::vector<uint8_t> GSMemory = pattern(64, 0x37);
  writeImageBytes(GSImage, 0x7000, GSMemory);
  NdOpEmulator GS(GSImage);
  GS.setStrictMode(true);
  ASSERT_TRUE(
      GS.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS, 0x7000));
  GS.setRegister(x86reg::RAX, UINT64_C(0x12345678ffffffc0));
  GS.setRegister(x86reg::K7, UINT16_MAX);
  GS.setRegisterBytes(x86reg::vectorReg(31), pattern(64, 0xbb));
  const std::vector<LowOp> GSOps = liftX64(GSLoad);
  ASSERT_FALSE(GSOps.empty());
  ASSERT_EQ(GS.run(GSOps), GSOps.size());
  EXPECT_EQ(GS.getRegisterBytes(x86reg::vectorReg(31)), GSMemory);

  const std::vector<uint8_t> FSStore{0x64, 0x67, 0x62, 0x61, 0xfe,
                                     0x4f, 0x7f, 0x78, 0x01};
  BinaryImage FSImage = makeMemoryImage(0x8000, 64);
  const std::vector<uint8_t> FSSource = pattern(64, 0x47);
  NdOpEmulator FS(FSImage);
  FS.setStrictMode(true);
  ASSERT_TRUE(
      FS.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS, 0x8000));
  FS.setRegister(x86reg::RAX, UINT64_C(0xabcdef01ffffffc0));
  FS.setRegister(x86reg::K7, UINT8_MAX);
  FS.setRegisterBytes(x86reg::vectorReg(31), FSSource);
  const std::vector<LowOp> FSOps = liftX64(FSStore);
  ASSERT_FALSE(FSOps.empty());
  ASSERT_EQ(FS.run(FSOps), FSOps.size());
  for (size_t Lane = 0; Lane < 8; ++Lane) {
    uint64_t Expected = 0;
    std::memcpy(&Expected, FSSource.data() + Lane * 8, 8);
    EXPECT_EQ(
        probeValue(FS, Lane * 8, 8, ProbeTemp++, NdMemoryAddressSpace::X86FS),
        Expected);
  }

  // A clear mask suppresses every access even when the address is wholly
  // unmapped.  Merge and zeroing still perform their normal register result.
  BinaryImage Empty;
  Empty.Arch = Arch::X64;
  Empty.Bits = Bitness::Bits64;
  const std::vector<uint8_t> Old = pattern(64, 0x62);
  for (bool Zero : {false, true}) {
    NdOpEmulator Suppressed(Empty);
    Suppressed.setStrictMode(true);
    Suppressed.setRegister(x86reg::RAX, UINT64_C(0xdead0000));
    Suppressed.setRegister(x86reg::K1, 0);
    Suppressed.setRegisterBytes(x86reg::XMM1, Old);
    const std::vector<LowOp> Ops = liftX64(moveBytes(1, 64, false, Zero));
    ASSERT_FALSE(Ops.empty());
    ASSERT_EQ(Suppressed.run(Ops), Ops.size());
    const auto Result = Suppressed.getRegisterBytes(x86reg::XMM1);
    ASSERT_TRUE(Result);
    EXPECT_EQ(*Result, Zero ? std::vector<uint8_t>(64, 0) : Old);
  }
  NdOpEmulator SuppressedStore(Empty);
  SuppressedStore.setStrictMode(true);
  SuppressedStore.setRegister(x86reg::RAX, UINT64_C(0xbeef0000));
  SuppressedStore.setRegister(x86reg::K1, 0);
  SuppressedStore.setRegisterBytes(x86reg::XMM1, pattern(64, 0x72));
  const std::vector<LowOp> SuppressedStoreOps = liftX64(moveBytes(8, 64, true));
  ASSERT_FALSE(SuppressedStoreOps.empty());
  EXPECT_EQ(SuppressedStore.run(SuppressedStoreOps), SuppressedStoreOps.size());

  // Any active load fault leaves the architectural destination and K state
  // untouched, even if an earlier active element was readable.
  BinaryImage FaultLoadImage = makeMemoryImage(0x9000, 1);
  FaultLoadImage.Segments.front().Data[0] = 0x11;
  const std::vector<uint8_t> FaultOld = pattern(64, 0x83);
  NdOpEmulator FaultLoad(FaultLoadImage);
  FaultLoad.setStrictMode(true);
  FaultLoad.setRegister(x86reg::RAX, 0x9000 - 64);
  FaultLoad.setRegister(x86reg::K1, 3);
  FaultLoad.setRegisterBytes(x86reg::XMM1, FaultOld);
  const std::vector<LowOp> FaultLoadOps = liftX64(moveBytes(1, 64, false));
  ASSERT_FALSE(FaultLoadOps.empty());
  EXPECT_LT(FaultLoad.run(FaultLoadOps), FaultLoadOps.size());
  EXPECT_EQ(FaultLoad.getRegisterBytes(x86reg::XMM1), FaultOld);
  EXPECT_EQ(FaultLoad.getRegister(x86reg::K1), 3u);

  // Stores are instruction-atomic in this strict model: validating a later
  // active element must happen before any earlier active element is written.
  BinaryImage FaultStoreImage = makeMemoryImage(0xa000, 1);
  FaultStoreImage.Segments.front().Data[0] = 0x5a;
  NdOpEmulator FaultStore(FaultStoreImage);
  FaultStore.setStrictMode(true);
  FaultStore.setRegister(x86reg::RAX, 0xa000 - 64);
  FaultStore.setRegister(x86reg::K1, 3);
  FaultStore.setRegisterBytes(x86reg::XMM1, pattern(64, 0x93));
  const std::vector<LowOp> FaultStoreOps = liftX64(moveBytes(1, 64, true));
  ASSERT_FALSE(FaultStoreOps.empty());
  EXPECT_LT(FaultStore.run(FaultStoreOps), FaultStoreOps.size());
  EXPECT_EQ(probeValue(FaultStore, 0xa000, 1, ProbeTemp++), 0x5a);
  EXPECT_EQ(FaultStore.getRegister(x86reg::K1), 3u);

  // One active word crossing the segment boundary faults before its first byte
  // is written; an element is never approximated as independent byte stores.
  BinaryImage SplitImage = makeMemoryImage(0xb000, 1);
  SplitImage.Segments.front().Data[0] = 0xc3;
  NdOpEmulator Split(SplitImage);
  Split.setStrictMode(true);
  Split.setRegister(x86reg::RAX, 0xb000 - 64);
  Split.setRegister(x86reg::K1, 1);
  Split.setRegisterBytes(x86reg::XMM1, pattern(64, 0xa3));
  const std::vector<LowOp> SplitOps = liftX64(moveBytes(2, 64, true));
  ASSERT_FALSE(SplitOps.empty());
  EXPECT_LT(Split.run(SplitOps), SplitOps.size());
  EXPECT_EQ(probeValue(Split, 0xb000, 1, ProbeTemp++), 0xc3);

  // Corrupt decoder details are an input boundary: no K0, zeroing store,
  // divergent memory width, or vector SIB index may emit preparatory LowIR.
  const std::vector<uint8_t> ValidLoad = moveBytes(1, 64, false);
  expectMalformedShapeRejected(
      ValidLoad, [](cs_x86 &X86) { X86.operands[1].reg = X86_REG_K0; });
  expectMalformedShapeRejected(
      ValidLoad, [](cs_x86 &X86) { X86.operands[1].reg = X86_REG_XMM2; });
  expectMalformedShapeRejected(ValidLoad,
                               [](cs_x86 &X86) { X86.operands[2].size = 32; });
  expectMalformedShapeRejected(
      ValidLoad, [](cs_x86 &X86) { X86.operands[2].mem.index = X86_REG_XMM2; });
  const std::vector<uint8_t> ValidStore = moveBytes(8, 64, true);
  expectMalformedShapeRejected(
      ValidStore, [](cs_x86 &X86) { X86.operands[1].avx_zero_opmask = true; });
}

} // namespace
