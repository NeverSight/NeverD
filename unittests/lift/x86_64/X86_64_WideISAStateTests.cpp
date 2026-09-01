//===- X86_64_WideISAStateTests.cpp - Wide x86 state semantics ----------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kAddress = 0x1000;
constexpr uint64_t kInvalidRegisterOffset = 0xffff;

std::vector<LowOp> liftX64(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  if (!Dec.init(Arch::X64)) {
    ADD_FAILURE() << "x86-64 decoder initialization failed";
    return {};
  }

  DecodedInsn Insn{};
  const int Size =
      Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn);
  if (Size != static_cast<int>(Bytes.size())) {
    ADD_FAILURE() << "x86-64 instruction decode failed";
    return {};
  }

  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

void expectStrictlyUnlifted(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));

  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn),
            static_cast<int>(Bytes.size()));

  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

testing::AssertionResult hasOnlyMappedRegisters(const std::vector<LowOp> &Ops) {
  auto Check = [](const NdVar &Var) {
    return !Var.isReg() ||
           (Var.Offset != kInvalidRegisterOffset && Var.Size != 0);
  };
  for (const LowOp &Op : Ops) {
    if (!Check(Op.Output))
      return testing::AssertionFailure()
             << "unmapped output at instruction 0x" << std::hex << Op.Addr;
    for (unsigned I = 0; I < Op.NumInputs; ++I)
      if (!Check(Op.Inputs[I]))
        return testing::AssertionFailure()
               << "unmapped input " << I << " at instruction 0x" << std::hex
               << Op.Addr;
  }
  return testing::AssertionSuccess();
}

void setDwordLane(std::vector<uint8_t> &Value, size_t Lane,
                  uint32_t LaneValue) {
  const size_t Offset = Lane * sizeof(LaneValue);
  for (size_t Byte = 0; Byte < sizeof(LaneValue); ++Byte)
    Value[Offset + Byte] = static_cast<uint8_t>(LaneValue >> (Byte * 8));
}

uint32_t getDwordLane(const std::vector<uint8_t> &Value, size_t Lane) {
  const size_t Offset = Lane * sizeof(uint32_t);
  uint32_t Result = 0;
  for (size_t Byte = 0; Byte < sizeof(Result); ++Byte)
    Result |= static_cast<uint32_t>(Value[Offset + Byte]) << (Byte * 8);
  return Result;
}

void setIntegerLane(std::vector<uint8_t> &Value, size_t Lane, size_t LaneSize,
                    uint64_t LaneValue) {
  const size_t Offset = Lane * LaneSize;
  for (size_t Byte = 0; Byte < LaneSize; ++Byte)
    Value[Offset + Byte] = static_cast<uint8_t>(LaneValue >> (Byte * 8));
}

uint64_t getIntegerLane(const std::vector<uint8_t> &Value, size_t Lane,
                        size_t LaneSize) {
  const size_t Offset = Lane * LaneSize;
  uint64_t Result = 0;
  for (size_t Byte = 0; Byte < LaneSize; ++Byte)
    Result |= static_cast<uint64_t>(Value[Offset + Byte]) << (Byte * 8);
  return Result;
}

TEST(X86WideISAState, VectorAliasesCoverEveryEvexRegisterWithoutOverlap) {
  const RegInfo Xmm0 = mapCapstoneReg(X86_REG_XMM0);
  const RegInfo Ymm0 = mapCapstoneReg(X86_REG_YMM0);
  const RegInfo Zmm0 = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Xmm31 = mapCapstoneReg(X86_REG_XMM31);
  const RegInfo Ymm31 = mapCapstoneReg(X86_REG_YMM31);
  const RegInfo Zmm31 = mapCapstoneReg(X86_REG_ZMM31);

  EXPECT_EQ(Xmm0.Size, 16u);
  EXPECT_EQ(Ymm0.Size, 32u);
  EXPECT_EQ(Zmm0.Size, 64u);
  EXPECT_EQ(Xmm0.Offset, Ymm0.Offset);
  EXPECT_EQ(Ymm0.Offset, Zmm0.Offset);

  EXPECT_EQ(Xmm31.Size, 16u);
  EXPECT_EQ(Ymm31.Size, 32u);
  EXPECT_EQ(Zmm31.Size, 64u);
  EXPECT_EQ(Xmm31.Offset, Ymm31.Offset);
  EXPECT_EQ(Ymm31.Offset, Zmm31.Offset);

  const RegInfo Zmm1 = mapCapstoneReg(X86_REG_ZMM1);
  EXPECT_GE(Zmm1.Offset, Zmm0.Offset + Zmm0.Size);
  EXPECT_GE(Zmm31.Offset, Zmm1.Offset + Zmm1.Size);

  const RegInfo K0 = mapCapstoneReg(X86_REG_K0);
  const RegInfo K7 = mapCapstoneReg(X86_REG_K7);
  EXPECT_EQ(K0.Size, 8u);
  EXPECT_EQ(K7.Size, 8u);
  EXPECT_GE(K0.Offset, Zmm31.Offset + Zmm31.Size);
  EXPECT_GE(K7.Offset, K0.Offset + K0.Size);
  EXPECT_GE(x86reg::ST0, K7.Offset + K7.Size);
}

TEST(X86WideISAState, ApxJmpabsLiftsAnExactAbsoluteControlTarget) {
  constexpr uint64_t Target = UINT64_C(0x8877665544332211);
  const std::vector<uint8_t> Bytes = {0xd5, 0x00, 0xa1, 0x11, 0x22, 0x33,
                                      0x44, 0x55, 0x66, 0x77, 0x88};
  Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Arch::X64));
  DecodedInsn Instruction{};
  ASSERT_EQ(Decoder.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress,
                                     Instruction),
            static_cast<int>(Bytes.size()));
  ASSERT_NE(Instruction.Raw, nullptr);
  EXPECT_EQ(Instruction.Id, X86_INS_JMPABS);
  EXPECT_TRUE(Decoder.isFunctionTerminator(Instruction));

  const std::vector<LowOp> Ops = liftX64(Bytes);
  ASSERT_EQ(Ops.size(), 1u);
  ASSERT_EQ(Ops[0].Opcode, NdOp::BRANCH);
  ASSERT_EQ(Ops[0].NumInputs, 1u);
  EXPECT_TRUE(Ops[0].Inputs[0].isConst());
  EXPECT_EQ(Ops[0].Inputs[0].Size, 8u);
  EXPECT_EQ(Ops[0].Inputs[0].Offset, Target);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  // A direct branch is an exact control-flow terminator, so the linear
  // emulator stops at it without executing a fall-through operation.
  EXPECT_EQ(Emulator.run(Ops), 0u);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86WideISAState, LiftedZmmMovePreservesAllBitsAndHighRegisterBank) {
  // vmovdqu64 zmm31, zmm20
  const std::vector<LowOp> Ops = liftX64({0x62, 0x21, 0xfe, 0x48, 0x6f, 0xfc});
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM20);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM31);
  ASSERT_EQ(Source.Size, 64u);
  ASSERT_EQ(Destination.Size, 64u);
  ASSERT_NE(Source.Offset, Destination.Offset);

  std::vector<uint8_t> Value(64);
  for (size_t I = 0; I < Value.size(); ++I)
    Value[I] = static_cast<uint8_t>((I * 37u + 11u) & 0xffu);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegisterBytes(Source.Offset, Value);
  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, Value);
}

TEST(X86WideISAState, EvexAlignUsesBothSourcesAndImmediate) {
  enum class MaskMode : uint8_t { None, Merge, Zero };
  struct AlignCase {
    std::vector<uint8_t> Bytes;
    uint16_t ElementSize;
    uint16_t VectorSize;
    MaskMode Mode;
    uint64_t MaskValue;
  };
  const AlignCase Cases[] = {
      // valignd xmm1, xmm2, xmm3, 1
      {{0x62, 0xf3, 0x6d, 0x08, 0x03, 0xcb, 0x01}, 4, 16, MaskMode::None, 0},
      // valignd zmm1, zmm2, zmm3, 1
      {{0x62, 0xf3, 0x6d, 0x48, 0x03, 0xcb, 0x01}, 4, 64, MaskMode::None, 0},
      // valignd zmm1, zmm2, zmm3, 17 (count wraps to 1)
      {{0x62, 0xf3, 0x6d, 0x48, 0x03, 0xcb, 0x11}, 4, 64, MaskMode::None, 0},
      // valignq zmm1, zmm2, zmm3, 1
      {{0x62, 0xf3, 0xed, 0x48, 0x03, 0xcb, 0x01}, 8, 64, MaskMode::None, 0},
      // valignq zmm1, zmm2, zmm3, 9 (count wraps to 1)
      {{0x62, 0xf3, 0xed, 0x48, 0x03, 0xcb, 0x09}, 8, 64, MaskMode::None, 0},
      // valignd ymm1 {k2}, ymm2, ymm3, 1
      {{0x62, 0xf3, 0x6d, 0x2a, 0x03, 0xcb, 0x01},
       4,
       32,
       MaskMode::Merge,
       UINT64_C(0x55)},
      // valignq ymm1 {k2} {z}, ymm2, ymm3, 1
      {{0x62, 0xf3, 0xed, 0xaa, 0x03, 0xcb, 0x01},
       8,
       32,
       MaskMode::Zero,
       UINT64_C(0x5)},
  };

  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM1);
  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K2);
  for (const AlignCase &Current : Cases) {
    SCOPED_TRACE(testing::Message()
                 << "element size " << Current.ElementSize << ", vector size "
                 << Current.VectorSize << ", immediate "
                 << static_cast<unsigned>(Current.Bytes.back()));
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const size_t LaneCount = Current.VectorSize / Current.ElementSize;
    std::vector<uint8_t> Source1Value(Destination.Size);
    std::vector<uint8_t> Source2Value(Destination.Size);
    std::vector<uint8_t> OldDestination(Destination.Size, 0xa5);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      setIntegerLane(Source1Value, Lane, Current.ElementSize, 0x1000 + Lane);
      setIntegerLane(Source2Value, Lane, Current.ElementSize, 0x2000 + Lane);
      setIntegerLane(OldDestination, Lane, Current.ElementSize, 0x3000 + Lane);
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(Source1.Offset, Source1Value);
    Emulator.setRegisterBytes(Source2.Offset, Source2Value);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    if (Current.Mode != MaskMode::None)
      Emulator.setRegister(Mask.Offset, Current.MaskValue);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_FALSE(Emulator.skips().any());

    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    ASSERT_EQ(Result->size(), Destination.Size);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Aligned = Lane + 1 < LaneCount ? 0x2001 + Lane : 0x1000;
      const bool Active = Current.Mode == MaskMode::None ||
                          (Current.MaskValue & (UINT64_C(1) << Lane)) != 0;
      const uint64_t Expected = Active ? Aligned
                                : Current.Mode == MaskMode::Merge
                                    ? 0x3000 + Lane
                                    : 0;
      EXPECT_EQ(getIntegerLane(*Result, Lane, Current.ElementSize), Expected);
    }
    EXPECT_TRUE(std::all_of(Result->begin() + Current.VectorSize, Result->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
  }

  {
    // valignd ymm1, ymm2, ymmword ptr [rax + 0x30], 1
    constexpr uint64_t MemoryBase = UINT64_C(0x6000);
    constexpr uint64_t MemoryDisplacement = UINT64_C(0x30);
    constexpr uint16_t VectorSize = 32;
    constexpr uint16_t ElementSize = 4;
    const std::vector<LowOp> Ops = liftX64(
        {0x62, 0xf3, 0x6d, 0x28, 0x03, 0x88, 0x30, 0x00, 0x00, 0x00, 0x01});
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const size_t LaneCount = VectorSize / ElementSize;
    std::vector<uint8_t> Source1Value(Destination.Size);
    std::vector<uint8_t> MemoryValue(VectorSize);
    std::vector<uint8_t> OldDestination(Destination.Size, 0xa5);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      setIntegerLane(Source1Value, Lane, ElementSize, 0x1000 + Lane);
      setIntegerLane(MemoryValue, Lane, ElementSize, 0x4000 + Lane);
      setIntegerLane(OldDestination, Lane, ElementSize, 0x3000 + Lane);
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = MemoryBase + MemoryDisplacement;
    Data.Size = MemoryValue.size();
    Data.Flags = SegmentFlags::Readable;
    Data.Data = MemoryValue;
    Image.Segments.push_back(std::move(Data));

    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, MemoryBase);
    Emulator.setRegisterBytes(Source1.Offset, Source1Value);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_FALSE(Emulator.skips().any());

    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    ASSERT_EQ(Result->size(), Destination.Size);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Expected = Lane + 1 < LaneCount ? 0x4001 + Lane : 0x1000;
      EXPECT_EQ(getIntegerLane(*Result, Lane, ElementSize), Expected);
    }
    EXPECT_TRUE(std::all_of(Result->begin() + VectorSize, Result->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
    ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr,
              MemoryBase + MemoryDisplacement);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Size, VectorSize);
  }

  // EVEX.aaa=0 means no writemask, so setting EVEX.z in that form is invalid.
  expectStrictlyUnlifted({0x62, 0xf3, 0x6d, 0xa8, 0x03, 0xcb, 0x01});
  // Broadcast and masked-memory forms remain fail-closed until their memory
  // access and fault-suppression contracts are modeled and tested directly.
  expectStrictlyUnlifted({0x62, 0xf3, 0x6d, 0x38, 0x03, 0x08, 0x01});
  expectStrictlyUnlifted({0x62, 0xf3, 0x6d, 0x2a, 0x03, 0x08, 0x01});
}

TEST(X86WideISAState, EvexShuffleQuartersUseBothSourcesAndImmediate) {
  struct ShuffleCase {
    std::vector<uint8_t> Bytes;
    uint16_t VectorSize;
  };
  const ShuffleCase Cases[] = {
      // vshuff32x4 ymm1, ymm2, ymm3, 0xab
      {{0x62, 0xf3, 0x6d, 0x28, 0x23, 0xcb, 0xab}, 32},
      // vshuff32x4 zmm1, zmm2, zmm3, 0xab
      {{0x62, 0xf3, 0x6d, 0x48, 0x23, 0xcb, 0xab}, 64},
      // vshuff64x2 ymm1, ymm2, ymm3, 0xab
      {{0x62, 0xf3, 0xed, 0x28, 0x23, 0xcb, 0xab}, 32},
      // vshuff64x2 zmm1, zmm2, zmm3, 0xab
      {{0x62, 0xf3, 0xed, 0x48, 0x23, 0xcb, 0xab}, 64},
      // vshufi32x4 ymm1, ymm2, ymm3, 0xab
      {{0x62, 0xf3, 0x6d, 0x28, 0x43, 0xcb, 0xab}, 32},
      // vshufi32x4 zmm1, zmm2, zmm3, 0xab
      {{0x62, 0xf3, 0x6d, 0x48, 0x43, 0xcb, 0xab}, 64},
      // vshufi64x2 ymm1, ymm2, ymm3, 0xab
      {{0x62, 0xf3, 0xed, 0x28, 0x43, 0xcb, 0xab}, 32},
      // vshufi64x2 zmm1, zmm2, zmm3, 0xab
      {{0x62, 0xf3, 0xed, 0x48, 0x43, 0xcb, 0xab}, 64},
  };
  constexpr std::array<size_t, 2> YmmQuarterSelectors = {1, 1};
  constexpr std::array<size_t, 4> ZmmQuarterSelectors = {3, 2, 2, 2};
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM1);
  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM3);
  for (const ShuffleCase &Current : Cases) {
    SCOPED_TRACE(testing::Message()
                 << "vector size " << Current.VectorSize << ", opcode 0x"
                 << std::hex << static_cast<unsigned>(Current.Bytes[4]));
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    std::vector<uint8_t> Source1Value(Destination.Size);
    std::vector<uint8_t> Source2Value(Destination.Size);
    std::vector<uint8_t> OldDestination(Destination.Size, 0xa5);
    for (size_t Lane = 0; Lane < 16; ++Lane) {
      setDwordLane(Source1Value, Lane, 0x1000 + Lane);
      setDwordLane(Source2Value, Lane, 0x2000 + Lane);
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(Source1.Offset, Source1Value);
    Emulator.setRegisterBytes(Source2.Offset, Source2Value);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_FALSE(Emulator.skips().any());

    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    ASSERT_EQ(Result->size(), Destination.Size);
    const size_t QuarterCount = Current.VectorSize / 16;
    for (size_t Lane = 0; Lane < Current.VectorSize / 4; ++Lane) {
      const size_t Quarter = Lane / 4;
      const size_t Selector = Current.VectorSize == 32
                                  ? YmmQuarterSelectors[Quarter]
                                  : ZmmQuarterSelectors[Quarter];
      const uint32_t SourceBase = Quarter < QuarterCount / 2 ? 0x1000 : 0x2000;
      EXPECT_EQ(getDwordLane(*Result, Lane),
                SourceBase + static_cast<uint32_t>(Selector * 4 + Lane % 4));
    }
    EXPECT_TRUE(std::all_of(Result->begin() + Current.VectorSize, Result->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
  }

  {
    // vshuff32x4 zmm1 {k2}, zmm2, zmm3, 0x1b
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf3, 0x6d, 0x4a, 0x23, 0xcb, 0x1b});
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const RegInfo Mask = mapCapstoneReg(X86_REG_K2);
    constexpr uint64_t MaskValue = UINT64_C(0xa55a);
    std::vector<uint8_t> Source1Value(Destination.Size);
    std::vector<uint8_t> Source2Value(Destination.Size);
    std::vector<uint8_t> OldDestination(Destination.Size);
    for (size_t Lane = 0; Lane < 16; ++Lane) {
      setDwordLane(Source1Value, Lane, 0x1000 + Lane);
      setDwordLane(Source2Value, Lane, 0x2000 + Lane);
      setDwordLane(OldDestination, Lane, 0x3000 + Lane);
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(Source1.Offset, Source1Value);
    Emulator.setRegisterBytes(Source2.Offset, Source2Value);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, MaskValue);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_FALSE(Emulator.skips().any());
    EXPECT_EQ(Emulator.getRegister(Mask.Offset), MaskValue);

    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    constexpr std::array<uint32_t, 16> Expected = {
        0x3000, 0x100d, 0x3002, 0x100f, 0x1008, 0x3005, 0x100a, 0x3007,
        0x2004, 0x3009, 0x2006, 0x300b, 0x300c, 0x2001, 0x300e, 0x2003};
    for (size_t Lane = 0; Lane < 16; ++Lane) {
      EXPECT_EQ(getDwordLane(*Result, Lane), Expected[Lane]);
    }
  }

  {
    // vshuff64x2 ymm1 {k2} {z}, ymm2, ymm3, 0x1b
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf3, 0xed, 0xaa, 0x23, 0xcb, 0x1b});
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const RegInfo Mask = mapCapstoneReg(X86_REG_K2);
    constexpr uint64_t MaskValue = UINT64_C(0xf5);
    std::vector<uint8_t> Source1Value(Destination.Size);
    std::vector<uint8_t> Source2Value(Destination.Size);
    std::vector<uint8_t> OldDestination(Destination.Size, 0xa5);
    for (size_t Lane = 0; Lane < 8; ++Lane) {
      setIntegerLane(Source1Value, Lane, 8, 0x1000 + Lane);
      setIntegerLane(Source2Value, Lane, 8, 0x2000 + Lane);
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(Source1.Offset, Source1Value);
    Emulator.setRegisterBytes(Source2.Offset, Source2Value);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, MaskValue);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_FALSE(Emulator.skips().any());
    EXPECT_EQ(Emulator.getRegister(Mask.Offset), MaskValue);

    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    EXPECT_EQ(getIntegerLane(*Result, 0, 8), UINT64_C(0x1002));
    EXPECT_EQ(getIntegerLane(*Result, 1, 8), UINT64_C(0));
    EXPECT_EQ(getIntegerLane(*Result, 2, 8), UINT64_C(0x2002));
    EXPECT_EQ(getIntegerLane(*Result, 3, 8), UINT64_C(0));
    EXPECT_TRUE(std::all_of(Result->begin() + 32, Result->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
  }

  {
    constexpr uint64_t MemoryBase = UINT64_C(0x6000);
    constexpr uint64_t MemoryDisplacement = UINT64_C(0x40);
    enum class MemoryMaskMode : uint8_t { None, Merge };
    struct MemoryCase {
      std::vector<uint8_t> Bytes;
      uint16_t VectorSize;
      uint16_t ElementSize;
      MemoryMaskMode Mode;
      uint64_t MaskValue;
    };
    const MemoryCase MemoryCases[] = {
        // vshuff32x4 zmm1 {k2}, zmm2, [rax + 0x40], 0x1b
        {{0x62, 0xf3, 0x6d, 0x4a, 0x23, 0x48, 0x01, 0x1b},
         64,
         4,
         MemoryMaskMode::Merge,
         UINT64_C(0xa55a)},
        // vshuff64x2 ymm1 {k2}, ymm2, [rax + 0x40], 0x1b
        {{0x62, 0xf3, 0xed, 0x2a, 0x23, 0x48, 0x02, 0x1b},
         32,
         8,
         MemoryMaskMode::Merge,
         UINT64_C(0x5)},
        // vshufi32x4 zmm1, zmm2, [rax + 0x40], 0x1b
        {{0x62, 0xf3, 0x6d, 0x48, 0x43, 0x48, 0x01, 0x1b},
         64,
         4,
         MemoryMaskMode::None,
         0},
        // vshufi64x2 ymm1, ymm2, [rax + 0x40], 0x1b
        {{0x62, 0xf3, 0xed, 0x28, 0x43, 0x48, 0x02, 0x1b},
         32,
         8,
         MemoryMaskMode::None,
         0},
    };
    constexpr std::array<size_t, 2> YmmQuarterSelectors = {1, 1};
    constexpr std::array<size_t, 4> ZmmQuarterSelectors = {3, 2, 1, 0};
    const RegInfo Mask = mapCapstoneReg(X86_REG_K2);
    for (const MemoryCase &Current : MemoryCases) {
      SCOPED_TRACE(testing::Message()
                   << "memory vector size " << Current.VectorSize
                   << ", element size " << Current.ElementSize << ", opcode 0x"
                   << std::hex << static_cast<unsigned>(Current.Bytes[4]));
      const std::vector<LowOp> Ops = liftX64(Current.Bytes);
      ASSERT_FALSE(Ops.empty());
      ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

      const size_t LaneCount = Current.VectorSize / Current.ElementSize;
      const size_t ElementsPerQuarter = 16 / Current.ElementSize;
      const size_t QuarterCount = Current.VectorSize / 16;
      std::vector<uint8_t> Source1Value(Destination.Size);
      std::vector<uint8_t> MemoryValue(Current.VectorSize);
      std::vector<uint8_t> OldDestination(Destination.Size, 0xa5);
      for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
        setIntegerLane(Source1Value, Lane, Current.ElementSize, 0x1000 + Lane);
        setIntegerLane(MemoryValue, Lane, Current.ElementSize, 0x4000 + Lane);
        setIntegerLane(OldDestination, Lane, Current.ElementSize,
                       0x3000 + Lane);
      }

      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      Segment Data;
      Data.VA = MemoryBase + MemoryDisplacement;
      Data.Size = MemoryValue.size();
      Data.Flags = SegmentFlags::Readable;
      Data.Data = MemoryValue;
      Image.Segments.push_back(std::move(Data));

      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setLoadCollect(true);
      Emulator.setRegister(x86reg::RAX, MemoryBase);
      Emulator.setRegisterBytes(Source1.Offset, Source1Value);
      Emulator.setRegisterBytes(Destination.Offset, OldDestination);
      if (Current.Mode != MemoryMaskMode::None)
        Emulator.setRegister(Mask.Offset, Current.MaskValue);
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      EXPECT_FALSE(Emulator.skips().any());
      if (Current.Mode != MemoryMaskMode::None)
        EXPECT_EQ(Emulator.getRegister(Mask.Offset), Current.MaskValue);

      const auto Result = Emulator.getRegisterBytes(Destination.Offset);
      ASSERT_TRUE(Result);
      for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
        const size_t Quarter = Lane / ElementsPerQuarter;
        const size_t Selector = Current.VectorSize == 32
                                    ? YmmQuarterSelectors[Quarter]
                                    : ZmmQuarterSelectors[Quarter];
        const uint64_t SourceBase =
            Quarter < QuarterCount / 2 ? 0x1000 : 0x4000;
        const uint64_t Shuffled = SourceBase + Selector * ElementsPerQuarter +
                                  Lane % ElementsPerQuarter;
        const uint64_t Expected =
            Current.Mode == MemoryMaskMode::None ||
                    (Current.MaskValue & (UINT64_C(1) << Lane)) != 0
                ? Shuffled
                : 0x3000 + Lane;
        EXPECT_EQ(getIntegerLane(*Result, Lane, Current.ElementSize), Expected);
      }
      EXPECT_TRUE(std::all_of(Result->begin() + Current.VectorSize,
                              Result->end(),
                              [](uint8_t Byte) { return Byte == 0; }));
      ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
      EXPECT_EQ(Emulator.getLoadRecords()[0].Addr,
                MemoryBase + MemoryDisplacement);
      EXPECT_EQ(Emulator.getLoadRecords()[0].Size, Current.VectorSize);
    }
  }

  {
    // A full memory tuple is read even when every writemask bit is clear.
    constexpr uint64_t MemoryBase = UINT64_C(0x6000);
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf3, 0x6d, 0xca, 0x23, 0x48, 0x01, 0x1b});
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const RegInfo Mask = mapCapstoneReg(X86_REG_K2);
    std::vector<uint8_t> Source1Value(Destination.Size, 0x5a);
    std::vector<uint8_t> OldDestination(Destination.Size, 0xa5);
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, MemoryBase);
    Emulator.setRegisterBytes(Source1.Offset, Source1Value);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, 0);
    EXPECT_LT(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), OldDestination);
    EXPECT_EQ(Emulator.getRegister(Mask.Offset), UINT64_C(0));
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());
  }

  // EVEX.z without a nonzero mask selector is invalid.
  expectStrictlyUnlifted({0x62, 0xf3, 0x6d, 0xc8, 0x23, 0xcb, 0x1b});
  // Broadcast forms remain fail-closed until their tuple semantics are
  // modeled directly.
  expectStrictlyUnlifted({0x62, 0xf3, 0x6d, 0x38, 0x23, 0x08, 0x1b});
  expectStrictlyUnlifted({0x62, 0xf3, 0xed, 0x38, 0x23, 0x08, 0x1b});
  expectStrictlyUnlifted({0x62, 0xf3, 0x6d, 0x38, 0x43, 0x08, 0x1b});
  expectStrictlyUnlifted({0x62, 0xf3, 0xed, 0x38, 0x43, 0x08, 0x1b});
  expectStrictlyUnlifted({0x62, 0xf3, 0x6d, 0x58, 0x23, 0x08, 0x1b});
  expectStrictlyUnlifted({0x62, 0xf3, 0xed, 0x58, 0x23, 0x08, 0x1b});
  expectStrictlyUnlifted({0x62, 0xf3, 0x6d, 0x58, 0x43, 0x08, 0x1b});
  expectStrictlyUnlifted({0x62, 0xf3, 0xed, 0x58, 0x43, 0x08, 0x1b});

  // EVEX.L'L=11 is reserved even though the decoder accepts it as ZMM.
  expectStrictlyUnlifted({0x62, 0xf3, 0x6d, 0x68, 0x23, 0xcb, 0x1b});
  expectStrictlyUnlifted({0x62, 0xf3, 0xed, 0x68, 0x23, 0xcb, 0x1b});
  expectStrictlyUnlifted({0x62, 0xf3, 0x6d, 0x68, 0x43, 0xcb, 0x1b});
  expectStrictlyUnlifted({0x62, 0xf3, 0xed, 0x68, 0x43, 0xcb, 0x1b});

  const std::array<std::array<uint8_t, 7>, 4> InvalidLl00 = {{
      {{0x62, 0xf3, 0x6d, 0x08, 0x23, 0xcb, 0x1b}},
      {{0x62, 0xf3, 0xed, 0x08, 0x23, 0xcb, 0x1b}},
      {{0x62, 0xf3, 0x6d, 0x08, 0x43, 0xcb, 0x1b}},
      {{0x62, 0xf3, 0xed, 0x08, 0x43, 0xcb, 0x1b}},
  }};
  Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Arch::X64));
  for (const auto &Bytes : InvalidLl00) {
    DecodedInsn Insn{};
    EXPECT_NE(
        Decoder.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn),
        static_cast<int>(Bytes.size()));
  }
}

TEST(X86WideISAState, LiftedKandqUsesDistinctFullWidthOpmaskRegisters) {
  // kandq k3, k1, k2
  const std::vector<LowOp> Ops = liftX64({0xc4, 0xe1, 0xf4, 0x41, 0xda});
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

  const RegInfo Left = mapCapstoneReg(X86_REG_K1);
  const RegInfo Right = mapCapstoneReg(X86_REG_K2);
  const RegInfo Destination = mapCapstoneReg(X86_REG_K3);
  ASSERT_EQ(Left.Size, 8u);
  ASSERT_EQ(Right.Size, 8u);
  ASSERT_EQ(Destination.Size, 8u);
  ASSERT_NE(Left.Offset, Right.Offset);
  ASSERT_NE(Left.Offset, Destination.Offset);
  ASSERT_NE(Right.Offset, Destination.Offset);

  constexpr uint64_t LeftValue = UINT64_C(0xf0f0aa55deadbeef);
  constexpr uint64_t RightValue = UINT64_C(0x0ff0ffff12345678);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegister(Left.Offset, LeftValue);
  Emulator.setRegister(Right.Offset, RightValue);
  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  ASSERT_TRUE(Emulator.getRegister(Destination.Offset));
  EXPECT_EQ(*Emulator.getRegister(Destination.Offset), LeftValue & RightValue);
}

TEST(X86WideISAState, Vex128WriteClearsTheWholeZmmUpperState) {
  // vmovdqa xmm1, xmm0
  const std::vector<LowOp> Ops = liftX64({0xc5, 0xf9, 0x6f, 0xc8});
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM1);
  std::vector<uint8_t> SourceValue(64);
  std::vector<uint8_t> DestinationValue(64, 0xa5);
  for (size_t I = 0; I < SourceValue.size(); ++I)
    SourceValue[I] = static_cast<uint8_t>(0x30u + I);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegisterBytes(Source.Offset, SourceValue);
  Emulator.setRegisterBytes(Destination.Offset, DestinationValue);
  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  ASSERT_EQ(Result->size(), 64u);
  EXPECT_TRUE(
      std::equal(Result->begin(), Result->begin() + 16, SourceValue.begin()));
  EXPECT_TRUE(std::all_of(Result->begin() + 16, Result->end(),
                          [](uint8_t Byte) { return Byte == 0; }));
}

TEST(X86WideISAState, Vex256WriteClearsTheZmmHighHalf) {
  // vmovdqa ymm1, ymm0
  const std::vector<LowOp> Ops = liftX64({0xc5, 0xfd, 0x6f, 0xc8});
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM1);
  std::vector<uint8_t> SourceValue(64);
  std::vector<uint8_t> DestinationValue(64, 0x5a);
  for (size_t I = 0; I < SourceValue.size(); ++I)
    SourceValue[I] = static_cast<uint8_t>(0xe0u - I);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegisterBytes(Source.Offset, SourceValue);
  Emulator.setRegisterBytes(Destination.Offset, DestinationValue);
  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  ASSERT_EQ(Result->size(), 64u);
  EXPECT_TRUE(
      std::equal(Result->begin(), Result->begin() + 32, SourceValue.begin()));
  EXPECT_TRUE(std::all_of(Result->begin() + 32, Result->end(),
                          [](uint8_t Byte) { return Byte == 0; }));
}

TEST(X86WideISAState, LegacyXmmWriteDoesNotClaimToClearUpperState) {
  // movdqa xmm1, xmm0
  const std::vector<LowOp> Ops = liftX64({0x66, 0x0f, 0x6f, 0xc8});
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(hasOnlyMappedRegisters(Ops));
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM1);
  EXPECT_TRUE(std::any_of(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
    return Op.Output.isReg() && Op.Output.Offset == Destination.Offset &&
           Op.Output.Size == 16;
  }));
  EXPECT_FALSE(std::any_of(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
    return Op.Opcode == NdOp::INT_ZEXT && Op.Output.isReg() &&
           Op.Output.Offset == Destination.Offset && Op.Output.Size == 64;
  }));
}

TEST(X86WideISAState, VzeroUpperPreservesLowXmmAndClearsTheRest) {
  const std::vector<LowOp> Ops = liftX64({0xc5, 0xf8, 0x77});
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

  const RegInfo Zmm0 = mapCapstoneReg(X86_REG_ZMM0);
  std::vector<uint8_t> Initial(64);
  for (size_t I = 0; I < Initial.size(); ++I)
    Initial[I] = static_cast<uint8_t>(I + 1);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegisterBytes(Zmm0.Offset, Initial);
  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(Zmm0.Offset);
  ASSERT_TRUE(Result);
  ASSERT_EQ(Result->size(), 64u);
  EXPECT_TRUE(
      std::equal(Result->begin(), Result->begin() + 16, Initial.begin()));
  EXPECT_TRUE(std::all_of(Result->begin() + 16, Result->end(),
                          [](uint8_t Byte) { return Byte == 0; }));
}

TEST(X86WideISAState, KandbClearsBitsAboveTheEncodedMaskWidth) {
  // kandb k3, k1, k2
  const std::vector<LowOp> Ops = liftX64({0xc5, 0xf5, 0x41, 0xda});
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

  const RegInfo Left = mapCapstoneReg(X86_REG_K1);
  const RegInfo Right = mapCapstoneReg(X86_REG_K2);
  const RegInfo Destination = mapCapstoneReg(X86_REG_K3);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegister(Left.Offset, UINT64_C(0xfffffffffffffff3));
  Emulator.setRegister(Right.Offset, UINT64_C(0xaaaaaaaaaaaaaa5f));
  Emulator.setRegister(Destination.Offset, UINT64_MAX);
  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  ASSERT_TRUE(Emulator.getRegister(Destination.Offset));
  EXPECT_EQ(*Emulator.getRegister(Destination.Offset), UINT64_C(0x53));
}

TEST(X86WideISAState, FloatingPointCallReturnStaysAtXmmAbiWidth) {
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);
  ASSERT_EQ(TRI.VecRegWidth, 64u);
  ASSERT_EQ(TRI.FPABIRegWidth, 16u);

  LowFunc Low;
  Low.Entry = kAddress;
  Low.Name = "wide_vector_fp_return";
  Low.Blocks.resize(1);
  LowBlock &Block = Low.Blocks.front();
  Block.Id = 0;
  Block.StartAddr = kAddress;
  Block.EndAddr = kAddress + 0x10;

  LowOp Seed;
  Seed.Opcode = NdOp::COPY;
  Seed.Addr = kAddress;
  Seed.Output = NdVar::reg(x86reg::XMM0, TRI.VecRegWidth);
  Seed.addInput(NdVar::cst(0, TRI.VecRegWidth));
  Block.Ops.push_back(Seed);

  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = kAddress + 4;
  Call.Output = NdVar::reg(x86reg::RAX, TRI.PointerSize);
  Call.addInput(NdVar::cst(0x2000, TRI.PointerSize));
  Block.Ops.push_back(Call);

  LowOp Consume;
  Consume.Opcode = NdOp::COPY;
  Consume.Addr = kAddress + 8;
  Consume.Output = NdVar::tmp(1, 8);
  Consume.addInput(NdVar::reg(x86reg::XMM0, 8));
  Block.Ops.push_back(Consume);

  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = kAddress + 12;
  Return.addInput(Consume.Output);
  Block.Ops.push_back(Return);

  const MedFunc Med = LowToMedConverter().convert(Low, Arch::X64);
  ASSERT_EQ(Med.Blocks.size(), 1u);
  const auto It =
      std::find_if(Med.Blocks.front().Ops.begin(), Med.Blocks.front().Ops.end(),
                   [](const MedOp &Op) { return Op.Opcode == NdOp::CALL; });
  ASSERT_NE(It, Med.Blocks.front().Ops.end());
  EXPECT_EQ(It->Output.Kind, MedVar::Reg);
  EXPECT_EQ(It->Output.RegOff, TRI.FPReturnReg);
  EXPECT_EQ(It->Output.Size, TRI.FPABIRegWidth);
}

TEST(X86WideISAState, ApxGeneralRegistersExposeEverySubregisterWidth) {
  const RegInfo R16 = mapCapstoneReg(X86_REG_R16);
  const RegInfo R31 = mapCapstoneReg(X86_REG_R31);
  EXPECT_EQ(R16.Size, 8u);
  EXPECT_EQ(R31.Size, 8u);
  EXPECT_NE(R16.Offset, R31.Offset);
  const RegInfo R16D = mapCapstoneReg(X86_REG_R16D);
  const RegInfo R16W = mapCapstoneReg(X86_REG_R16W);
  const RegInfo R16B = mapCapstoneReg(X86_REG_R16B);
  const RegInfo R31D = mapCapstoneReg(X86_REG_R31D);
  const RegInfo R31W = mapCapstoneReg(X86_REG_R31W);
  const RegInfo R31B = mapCapstoneReg(X86_REG_R31B);
  EXPECT_EQ(R16D.Offset, R16.Offset);
  EXPECT_EQ(R16W.Offset, R16.Offset);
  EXPECT_EQ(R16B.Offset, R16.Offset);
  EXPECT_EQ(R16D.Size, 4u);
  EXPECT_EQ(R16W.Size, 2u);
  EXPECT_EQ(R16B.Size, 1u);
  EXPECT_EQ(R31D.Offset, R31.Offset);
  EXPECT_EQ(R31W.Offset, R31.Offset);
  EXPECT_EQ(R31B.Offset, R31.Offset);
  EXPECT_EQ(R31D.Size, 4u);
  EXPECT_EQ(R31W.Size, 2u);
  EXPECT_EQ(R31B.Size, 1u);
}

TEST(X86WideISAState, LiftedRex2MovePreservesFullApxRegisterValue) {
  // mov r26, r25
  const std::vector<LowOp> Ops = liftX64({0xd5, 0x5d, 0x89, 0xca});
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

  const RegInfo Source = mapCapstoneReg(X86_REG_R25);
  const RegInfo Destination = mapCapstoneReg(X86_REG_R26);
  ASSERT_EQ(Source.Size, 8u);
  ASSERT_EQ(Destination.Size, 8u);
  ASSERT_NE(Source.Offset, Destination.Offset);

  constexpr uint64_t Value = UINT64_C(0xa55ac33cf00f9669);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegister(Source.Offset, Value);
  Emulator.setRegister(Destination.Offset, 0);
  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  ASSERT_TRUE(Emulator.getRegister(Destination.Offset));
  EXPECT_EQ(*Emulator.getRegister(Destination.Offset), Value);
}

TEST(X86WideISAState, LiftedRex2AddUpdatesApxRegisterAndFlags) {
  // add r31, r24
  const std::vector<LowOp> Ops = liftX64({0xd5, 0x5d, 0x01, 0xc7});
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

  const RegInfo Source = mapCapstoneReg(X86_REG_R24);
  const RegInfo Destination = mapCapstoneReg(X86_REG_R31);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegister(Source.Offset, 1);
  Emulator.setRegister(Destination.Offset, UINT64_MAX);
  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  ASSERT_TRUE(Emulator.getRegister(Destination.Offset));
  EXPECT_EQ(*Emulator.getRegister(Destination.Offset), 0u);
  ASSERT_TRUE(Emulator.getRegister(x86reg::CF));
  ASSERT_TRUE(Emulator.getRegister(x86reg::ZF));
  EXPECT_EQ(*Emulator.getRegister(x86reg::CF), 1u);
  EXPECT_EQ(*Emulator.getRegister(x86reg::ZF), 1u);
}

TEST(X86WideISAState, ApxPromotedAluHonorsNddNfAndSubregisterWrites) {
  struct Operation {
    uint8_t BaseOpcode;
    NdOp Opcode;
    bool Subtract;
    bool Logic;
  };
  const std::array<Operation, 5> Operations = {{
      {0x00, NdOp::INT_ADD, false, false},
      {0x08, NdOp::INT_OR, false, true},
      {0x20, NdOp::INT_AND, false, true},
      {0x28, NdOp::INT_SUB, true, false},
      {0x30, NdOp::INT_XOR, false, true},
  }};
  const std::array<uint8_t, 4> Widths = {1, 2, 4, 8};
  const std::array<x86_reg, 32> Registers8 = {
      X86_REG_AL,   X86_REG_CL,   X86_REG_DL,   X86_REG_BL,   X86_REG_SPL,
      X86_REG_BPL,  X86_REG_SIL,  X86_REG_DIL,  X86_REG_R8B,  X86_REG_R9B,
      X86_REG_R10B, X86_REG_R11B, X86_REG_R12B, X86_REG_R13B, X86_REG_R14B,
      X86_REG_R15B, X86_REG_R16B, X86_REG_R17B, X86_REG_R18B, X86_REG_R19B,
      X86_REG_R20B, X86_REG_R21B, X86_REG_R22B, X86_REG_R23B, X86_REG_R24B,
      X86_REG_R25B, X86_REG_R26B, X86_REG_R27B, X86_REG_R28B, X86_REG_R29B,
      X86_REG_R30B, X86_REG_R31B};
  const std::array<x86_reg, 32> Registers16 = {
      X86_REG_AX,   X86_REG_CX,   X86_REG_DX,   X86_REG_BX,   X86_REG_SP,
      X86_REG_BP,   X86_REG_SI,   X86_REG_DI,   X86_REG_R8W,  X86_REG_R9W,
      X86_REG_R10W, X86_REG_R11W, X86_REG_R12W, X86_REG_R13W, X86_REG_R14W,
      X86_REG_R15W, X86_REG_R16W, X86_REG_R17W, X86_REG_R18W, X86_REG_R19W,
      X86_REG_R20W, X86_REG_R21W, X86_REG_R22W, X86_REG_R23W, X86_REG_R24W,
      X86_REG_R25W, X86_REG_R26W, X86_REG_R27W, X86_REG_R28W, X86_REG_R29W,
      X86_REG_R30W, X86_REG_R31W};
  const std::array<x86_reg, 32> Registers32 = {
      X86_REG_EAX,  X86_REG_ECX,  X86_REG_EDX,  X86_REG_EBX,  X86_REG_ESP,
      X86_REG_EBP,  X86_REG_ESI,  X86_REG_EDI,  X86_REG_R8D,  X86_REG_R9D,
      X86_REG_R10D, X86_REG_R11D, X86_REG_R12D, X86_REG_R13D, X86_REG_R14D,
      X86_REG_R15D, X86_REG_R16D, X86_REG_R17D, X86_REG_R18D, X86_REG_R19D,
      X86_REG_R20D, X86_REG_R21D, X86_REG_R22D, X86_REG_R23D, X86_REG_R24D,
      X86_REG_R25D, X86_REG_R26D, X86_REG_R27D, X86_REG_R28D, X86_REG_R29D,
      X86_REG_R30D, X86_REG_R31D};
  const std::array<x86_reg, 32> Registers64 = {
      X86_REG_RAX, X86_REG_RCX, X86_REG_RDX, X86_REG_RBX, X86_REG_RSP,
      X86_REG_RBP, X86_REG_RSI, X86_REG_RDI, X86_REG_R8,  X86_REG_R9,
      X86_REG_R10, X86_REG_R11, X86_REG_R12, X86_REG_R13, X86_REG_R14,
      X86_REG_R15, X86_REG_R16, X86_REG_R17, X86_REG_R18, X86_REG_R19,
      X86_REG_R20, X86_REG_R21, X86_REG_R22, X86_REG_R23, X86_REG_R24,
      X86_REG_R25, X86_REG_R26, X86_REG_R27, X86_REG_R28, X86_REG_R29,
      X86_REG_R30, X86_REG_R31};
  auto RegisterFor = [&](uint8_t Width, unsigned Number) {
    if (Width == 1)
      return Registers8[Number];
    if (Width == 2)
      return Registers16[Number];
    if (Width == 4)
      return Registers32[Number];
    return Registers64[Number];
  };
  auto Encode = [](const Operation &Op, uint8_t Width, bool Reverse, bool Ndd,
                   bool Nf, unsigned Destination, unsigned Source1,
                   unsigned Source2) {
    const unsigned Reg = Reverse ? Source1 : Source2;
    const unsigned Rm = Reverse ? Source2 : Source1;
    const unsigned NddRegister = Ndd ? Destination : 0;
    const uint8_t Opcode = static_cast<uint8_t>(
        Op.BaseOpcode + (Reverse ? 2 : 0) + (Width == 1 ? 0 : 1));
    const uint8_t Pp = Width == 2 ? 1 : 0;
    const uint8_t W = Width == 8 ? 0x80 : 0;
    return std::vector<uint8_t>{
        0x62,
        static_cast<uint8_t>(0x44 | ((Reg & 8) ? 0 : 0x80) |
                             ((Rm & 8) ? 0 : 0x20) | ((Reg & 16) ? 0 : 0x10) |
                             ((Rm & 16) ? 0x08 : 0)),
        static_cast<uint8_t>(W | (((~NddRegister) & 0xf) << 3) | 0x04 | Pp),
        static_cast<uint8_t>((Ndd ? 0x10 : 0) | (Nf ? 0x04 : 0) |
                             ((NddRegister & 16) ? 0 : 0x08)),
        Opcode,
        static_cast<uint8_t>(0xc0 | ((Reg & 7) << 3) | (Rm & 7))};
  };
  auto EvenParity = [](uint8_t Value) {
    unsigned Ones = 0;
    for (unsigned Bit = 0; Bit < 8; ++Bit)
      Ones += (Value >> Bit) & 1u;
    return (Ones & 1u) == 0;
  };

  constexpr unsigned DestinationNumber = 31;
  constexpr unsigned NddSourceNumber = 30;
  constexpr unsigned SecondSourceNumber = 29;
  for (const Operation &Operation : Operations) {
    for (const uint8_t Width : Widths) {
      const unsigned Bits = Width * 8;
      const uint64_t WidthMask =
          Width == 8 ? UINT64_MAX : (UINT64_C(1) << Bits) - 1;
      const uint64_t A = WidthMask >> 1;
      const uint64_t B = 1;
      uint64_t RawResult = 0;
      switch (Operation.Opcode) {
      case NdOp::INT_ADD:
        RawResult = A + B;
        break;
      case NdOp::INT_SUB:
        RawResult = A - B;
        break;
      case NdOp::INT_AND:
        RawResult = A & B;
        break;
      case NdOp::INT_OR:
        RawResult = A | B;
        break;
      case NdOp::INT_XOR:
        RawResult = A ^ B;
        break;
      default:
        FAIL() << "unexpected test operation";
      }
      const uint64_t Result = RawResult & WidthMask;

      for (const bool Reverse : {false, true}) {
        for (const bool Ndd : {false, true}) {
          for (const bool Nf : {false, true}) {
            const unsigned Source1Number =
                Ndd ? NddSourceNumber : DestinationNumber;
            const std::vector<LowOp> Ops = liftX64(
                Encode(Operation, Width, Reverse, Ndd, Nf, DestinationNumber,
                       Source1Number, SecondSourceNumber));
            ASSERT_FALSE(Ops.empty());
            ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

            const RegInfo Destination =
                mapCapstoneReg(RegisterFor(Width, DestinationNumber));
            const RegInfo Source1 =
                mapCapstoneReg(RegisterFor(Width, Source1Number));
            const RegInfo Source2 =
                mapCapstoneReg(RegisterFor(Width, SecondSourceNumber));
            const uint64_t DestinationBefore =
                (UINT64_C(0xa55ac33cf00f0000) & ~WidthMask) |
                (Ndd ? UINT64_C(0x55) : A);
            const uint64_t Source1Value =
                (UINT64_C(0x123456789abc0000) & ~WidthMask) | A;
            const uint64_t Source2Value =
                (UINT64_C(0xfedcba9876540000) & ~WidthMask) | B;

            BinaryImage Image;
            Image.Arch = Arch::X64;
            Image.Bits = Bitness::Bits64;
            NdOpEmulator Emulator(Image);
            Emulator.setRegister(Destination.Offset, DestinationBefore);
            if (Ndd)
              Emulator.setRegister(Source1.Offset, Source1Value);
            Emulator.setRegister(Source2.Offset, Source2Value);
            Emulator.setRegister(x86reg::CF, 1);
            Emulator.setRegister(x86reg::ZF, 1);
            Emulator.setRegister(x86reg::SF, 0);
            Emulator.setRegister(x86reg::OF, 0);
            Emulator.setRegister(x86reg::PF, 0);
            Emulator.setRegister(x86reg::AF, 0);
            EXPECT_EQ(Emulator.run(Ops), Ops.size());

            uint64_t ExpectedDestination = Result;
            if (Width <= 2)
              ExpectedDestination = (DestinationBefore & ~WidthMask) | Result;
            EXPECT_EQ(*Emulator.getRegister(Destination.Offset),
                      ExpectedDestination);
            EXPECT_EQ(*Emulator.getRegister(Source2.Offset), Source2Value);
            if (Ndd)
              EXPECT_EQ(*Emulator.getRegister(Source1.Offset), Source1Value);

            if (Nf) {
              EXPECT_EQ(*Emulator.getRegister(x86reg::CF), 1u);
              EXPECT_EQ(*Emulator.getRegister(x86reg::ZF), 1u);
              EXPECT_EQ(*Emulator.getRegister(x86reg::SF), 0u);
              EXPECT_EQ(*Emulator.getRegister(x86reg::OF), 0u);
              EXPECT_EQ(*Emulator.getRegister(x86reg::PF), 0u);
              EXPECT_EQ(*Emulator.getRegister(x86reg::AF), 0u);
              continue;
            }

            EXPECT_EQ(*Emulator.getRegister(x86reg::ZF), Result == 0);
            EXPECT_EQ(*Emulator.getRegister(x86reg::SF),
                      (Result >> (Bits - 1)) & 1u);
            EXPECT_EQ(*Emulator.getRegister(x86reg::PF),
                      EvenParity(static_cast<uint8_t>(Result)));
            EXPECT_EQ(
                *Emulator.getRegister(x86reg::CF),
                Operation.Logic
                    ? 0
                    : (Operation.Subtract ? A < B : Result < (A & WidthMask)));
            EXPECT_EQ(
                *Emulator.getRegister(x86reg::OF),
                Operation.Logic
                    ? 0
                    : (Operation.Subtract
                           ? (((A ^ B) & (A ^ Result)) >> (Bits - 1)) & 1u
                           : ((~(A ^ B) & (A ^ Result)) >> (Bits - 1)) & 1u));
            if (!Operation.Logic)
              EXPECT_EQ(*Emulator.getRegister(x86reg::AF),
                        ((A ^ B ^ Result) >> 4) & 1u);
          }
        }
      }
    }
  }
}

TEST(X86WideISAState, EvexMoveMergeMaskPreservesInactiveQwordLanes) {
  // vmovdqu64 zmm31 {k3}, zmm20
  const std::vector<LowOp> Ops = liftX64({0x62, 0x21, 0xfe, 0x4b, 0x6f, 0xfc});
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM20);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM31);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K3);
  std::vector<uint8_t> SourceValue(64);
  std::vector<uint8_t> OldDestination(64);
  for (size_t I = 0; I < 64; ++I) {
    SourceValue[I] = static_cast<uint8_t>(I * 3u + 1u);
    OldDestination[I] = static_cast<uint8_t>(0xf0u - I);
  }
  constexpr uint64_t MaskValue = UINT64_C(0xa5);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegisterBytes(Source.Offset, SourceValue);
  Emulator.setRegisterBytes(Destination.Offset, OldDestination);
  Emulator.setRegister(Mask.Offset, MaskValue);
  EXPECT_EQ(Emulator.run(Ops), Ops.size());

  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  ASSERT_EQ(Result->size(), 64u);
  for (size_t Lane = 0; Lane < 8; ++Lane) {
    const auto &Expected =
        (MaskValue & (UINT64_C(1) << Lane)) != 0 ? SourceValue : OldDestination;
    EXPECT_TRUE(std::equal(Result->begin() + Lane * 8,
                           Result->begin() + (Lane + 1) * 8,
                           Expected.begin() + Lane * 8));
  }
}

TEST(X86WideISAState, EvexMoveZeroMaskClearsInactiveQwordLanes) {
  // vmovdqu64 zmm31 {k3} {z}, zmm20
  const std::vector<LowOp> Ops = liftX64({0x62, 0x21, 0xfe, 0xcb, 0x6f, 0xfc});
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM20);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM31);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K3);
  std::vector<uint8_t> SourceValue(64);
  std::vector<uint8_t> OldDestination(64, 0xcc);
  for (size_t I = 0; I < 64; ++I)
    SourceValue[I] = static_cast<uint8_t>(0x80u + I);
  constexpr uint64_t MaskValue = UINT64_C(0x5a);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegisterBytes(Source.Offset, SourceValue);
  Emulator.setRegisterBytes(Destination.Offset, OldDestination);
  Emulator.setRegister(Mask.Offset, MaskValue);
  EXPECT_EQ(Emulator.run(Ops), Ops.size());

  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  ASSERT_EQ(Result->size(), 64u);
  for (size_t Lane = 0; Lane < 8; ++Lane) {
    const bool Active = (MaskValue & (UINT64_C(1) << Lane)) != 0;
    for (size_t Byte = 0; Byte < 8; ++Byte)
      EXPECT_EQ((*Result)[Lane * 8 + Byte],
                Active ? SourceValue[Lane * 8 + Byte] : 0u);
  }
}

TEST(X86WideISAState, EvexVpadddAddsEveryDwordLane) {
  // vpaddd zmm0, zmm2, zmm3
  const std::vector<LowOp> Ops = liftX64({0x62, 0xf1, 0x6d, 0x48, 0xfe, 0xc3});
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

  const RegInfo Left = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Right = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  std::vector<uint8_t> LeftValue(64), RightValue(64);
  for (size_t Lane = 0; Lane < 16; ++Lane) {
    setDwordLane(LeftValue, Lane, static_cast<uint32_t>(Lane));
    setDwordLane(RightValue, Lane, static_cast<uint32_t>(0x100u + Lane));
  }

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegisterBytes(Left.Offset, LeftValue);
  Emulator.setRegisterBytes(Right.Offset, RightValue);
  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  ASSERT_EQ(Result->size(), 64u);
  for (size_t Lane = 0; Lane < 16; ++Lane)
    EXPECT_EQ(getDwordLane(*Result, Lane), 0x100u + 2u * Lane);
}

TEST(X86WideISAState, EvexVpadddMergeMaskPreservesInactiveDwordLanes) {
  // vpaddd zmm0 {k1}, zmm2, zmm3
  const std::vector<LowOp> Ops = liftX64({0x62, 0xf1, 0x6d, 0x49, 0xfe, 0xc3});
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

  const RegInfo Left = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Right = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  std::vector<uint8_t> LeftValue(64), RightValue(64), OldDestination(64);
  for (size_t Lane = 0; Lane < 16; ++Lane) {
    setDwordLane(LeftValue, Lane, static_cast<uint32_t>(Lane));
    setDwordLane(RightValue, Lane, static_cast<uint32_t>(0x100u + Lane));
    setDwordLane(OldDestination, Lane,
                 static_cast<uint32_t>(0xd0000000u + Lane));
  }
  constexpr uint64_t MaskValue = UINT64_C(0x5555);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegisterBytes(Left.Offset, LeftValue);
  Emulator.setRegisterBytes(Right.Offset, RightValue);
  Emulator.setRegisterBytes(Destination.Offset, OldDestination);
  Emulator.setRegister(Mask.Offset, MaskValue);
  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  ASSERT_EQ(Result->size(), 64u);
  for (size_t Lane = 0; Lane < 16; ++Lane) {
    const uint32_t Expected = (MaskValue & (UINT64_C(1) << Lane)) != 0
                                  ? static_cast<uint32_t>(0x100u + 2u * Lane)
                                  : static_cast<uint32_t>(0xd0000000u + Lane);
    EXPECT_EQ(getDwordLane(*Result, Lane), Expected);
  }
}

TEST(X86WideISAState, EvexVpadddZeroMaskClearsInactiveDwordLanes) {
  // vpaddd zmm0 {k1} {z}, zmm2, zmm3
  const std::vector<LowOp> Ops = liftX64({0x62, 0xf1, 0x6d, 0xc9, 0xfe, 0xc3});
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

  const RegInfo Left = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Right = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  std::vector<uint8_t> LeftValue(64), RightValue(64), OldDestination(64, 0xcc);
  for (size_t Lane = 0; Lane < 16; ++Lane) {
    setDwordLane(LeftValue, Lane, static_cast<uint32_t>(Lane));
    setDwordLane(RightValue, Lane, static_cast<uint32_t>(0x100u + Lane));
  }
  constexpr uint64_t MaskValue = UINT64_C(0x5555);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegisterBytes(Left.Offset, LeftValue);
  Emulator.setRegisterBytes(Right.Offset, RightValue);
  Emulator.setRegisterBytes(Destination.Offset, OldDestination);
  Emulator.setRegister(Mask.Offset, MaskValue);
  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  ASSERT_EQ(Result->size(), 64u);
  for (size_t Lane = 0; Lane < 16; ++Lane) {
    const uint32_t Expected = (MaskValue & (UINT64_C(1) << Lane)) != 0
                                  ? static_cast<uint32_t>(0x100u + 2u * Lane)
                                  : 0u;
    EXPECT_EQ(getDwordLane(*Result, Lane), Expected);
  }
}

TEST(X86WideISAState, EvexPackedIntegerAddSubMasksEverySupportedElementWidth) {
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t LaneSize;
    bool IsSubtract;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf1, 0x6d, 0x49, 0xfc, 0xc3}, 1, false}, // vpaddb
      {{0x62, 0xf1, 0x6d, 0x49, 0xfd, 0xc3}, 2, false}, // vpaddw
      {{0x62, 0xf1, 0x6d, 0x49, 0xfe, 0xc3}, 4, false}, // vpaddd
      {{0x62, 0xf1, 0xed, 0x49, 0xd4, 0xc3}, 8, false}, // vpaddq
      {{0x62, 0xf1, 0x6d, 0x49, 0xf8, 0xc3}, 1, true},  // vpsubb
      {{0x62, 0xf1, 0x6d, 0x49, 0xf9, 0xc3}, 2, true},  // vpsubw
      {{0x62, 0xf1, 0x6d, 0x49, 0xfa, 0xc3}, 4, true},  // vpsubd
      {{0x62, 0xf1, 0xed, 0x49, 0xfb, 0xc3}, 8, true},  // vpsubq
  };

  const RegInfo Left = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Right = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0x5555555555555555);

  for (const Case &Current : Cases) {
    SCOPED_TRACE(testing::Message() << "lane bytes=" << Current.LaneSize
                                    << " subtract=" << Current.IsSubtract);
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const size_t LaneCount = 64 / Current.LaneSize;
    std::vector<uint8_t> LeftValue(64), RightValue(64), OldDestination(64);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      setIntegerLane(LeftValue, Lane, Current.LaneSize, 0x40u + Lane);
      setIntegerLane(RightValue, Lane, Current.LaneSize, 0x10u + Lane);
      setIntegerLane(OldDestination, Lane, Current.LaneSize, 0xa0u + Lane);
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Left.Offset, LeftValue);
    Emulator.setRegisterBytes(Right.Offset, RightValue);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    ASSERT_EQ(Result->size(), 64u);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t ActiveValue =
          Current.IsSubtract ? UINT64_C(0x30) : UINT64_C(0x50) + 2 * Lane;
      const uint64_t Expected = (MaskValue & (UINT64_C(1) << Lane)) != 0
                                    ? ActiveValue
                                    : UINT64_C(0xa0) + Lane;
      EXPECT_EQ(getIntegerLane(*Result, Lane, Current.LaneSize), Expected);
    }
  }
}

TEST(X86WideISAState, EvexIntegerAverageMinAndSaturationHonorWriteMasks) {
  const RegInfo Left = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Right = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0x5555555555555555);

  {
    // vpavgb zmm0 {k1} {z}, zmm2, zmm3
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf1, 0x6d, 0xc9, 0xe0, 0xc3});
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));
    std::vector<uint8_t> LeftValue(64), RightValue(64);
    for (size_t Lane = 0; Lane < 64; ++Lane) {
      LeftValue[Lane] = static_cast<uint8_t>(10u + Lane);
      RightValue[Lane] = static_cast<uint8_t>(21u + Lane);
    }
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Left.Offset, LeftValue);
    Emulator.setRegisterBytes(Right.Offset, RightValue);
    Emulator.setRegisterBytes(Destination.Offset,
                              std::vector<uint8_t>(64, 0xcc));
    Emulator.setRegister(Mask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    for (size_t Lane = 0; Lane < 64; ++Lane)
      EXPECT_EQ((*Result)[Lane], (MaskValue & (UINT64_C(1) << Lane)) != 0
                                     ? static_cast<uint8_t>(16u + Lane)
                                     : 0u);
  }

  {
    // vpminsd zmm0 {k1} {z}, zmm2, zmm3
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf2, 0x6d, 0xc9, 0x39, 0xc3});
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));
    std::vector<uint8_t> LeftValue(64), RightValue(64);
    for (size_t Lane = 0; Lane < 16; ++Lane) {
      setDwordLane(LeftValue, Lane,
                   static_cast<uint32_t>(static_cast<int32_t>(-100) +
                                         static_cast<int32_t>(Lane)));
      setDwordLane(RightValue, Lane, static_cast<uint32_t>(50u - Lane));
    }
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Left.Offset, LeftValue);
    Emulator.setRegisterBytes(Right.Offset, RightValue);
    Emulator.setRegisterBytes(Destination.Offset,
                              std::vector<uint8_t>(64, 0xcc));
    Emulator.setRegister(Mask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    for (size_t Lane = 0; Lane < 16; ++Lane) {
      const uint32_t Expected =
          (MaskValue & (UINT64_C(1) << Lane)) != 0
              ? static_cast<uint32_t>(static_cast<int32_t>(-100) +
                                      static_cast<int32_t>(Lane))
              : 0u;
      EXPECT_EQ(getDwordLane(*Result, Lane), Expected);
    }
  }

  {
    // vpaddusb zmm0 {k1}, zmm2, zmm3
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf1, 0x6d, 0x49, 0xdc, 0xc3});
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));
    std::vector<uint8_t> LeftValue(64, 250), RightValue(64, 10);
    std::vector<uint8_t> OldDestination(64);
    for (size_t Lane = 0; Lane < 64; ++Lane)
      OldDestination[Lane] = static_cast<uint8_t>(0x20u + Lane);
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Left.Offset, LeftValue);
    Emulator.setRegisterBytes(Right.Offset, RightValue);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    for (size_t Lane = 0; Lane < 64; ++Lane)
      EXPECT_EQ((*Result)[Lane], (MaskValue & (UINT64_C(1) << Lane)) != 0
                                     ? UINT8_MAX
                                     : OldDestination[Lane]);
  }
}

TEST(X86WideISAState, EvexBitwiseDwordAndQwordOperationsHonorWriteMasks) {
  enum class Operation { And, Or, Xor, AndNot };
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t LaneSize;
    Operation Op;
    bool ZeroInactive;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf1, 0x6d, 0x49, 0xef, 0xc3}, 4, Operation::Xor, false},
      {{0x62, 0xf1, 0x6d, 0xc9, 0xdb, 0xc3}, 4, Operation::And, true},
      {{0x62, 0xf1, 0xed, 0x49, 0xeb, 0xc3}, 8, Operation::Or, false},
      {{0x62, 0xf1, 0xed, 0xc9, 0xdf, 0xc3}, 8, Operation::AndNot, true},
  };

  const RegInfo Left = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Right = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0x5555);
  for (const Case &Current : Cases) {
    SCOPED_TRACE(testing::Message()
                 << "lane bytes=" << Current.LaneSize
                 << " op=" << static_cast<unsigned>(Current.Op));
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const size_t LaneCount = 64 / Current.LaneSize;
    std::vector<uint8_t> LeftValue(64), RightValue(64), OldDestination(64);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      setIntegerLane(LeftValue, Lane, Current.LaneSize,
                     UINT64_C(0x0f0f00f0) + Lane);
      setIntegerLane(RightValue, Lane, Current.LaneSize,
                     UINT64_C(0x3355aa55) ^ Lane);
      setIntegerLane(OldDestination, Lane, Current.LaneSize,
                     UINT64_C(0xa5000000) + Lane);
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Left.Offset, LeftValue);
    Emulator.setRegisterBytes(Right.Offset, RightValue);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);

    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t A = UINT64_C(0x0f0f00f0) + Lane;
      const uint64_t B = UINT64_C(0x3355aa55) ^ Lane;
      uint64_t Active = 0;
      switch (Current.Op) {
      case Operation::And:
        Active = A & B;
        break;
      case Operation::Or:
        Active = A | B;
        break;
      case Operation::Xor:
        Active = A ^ B;
        break;
      case Operation::AndNot:
        Active = ~A & B;
        break;
      }
      const uint64_t Expected =
          (MaskValue & (UINT64_C(1) << Lane)) != 0
              ? Active
              : (Current.ZeroInactive ? 0 : UINT64_C(0xa5000000) + Lane);
      EXPECT_EQ(getIntegerLane(*Result, Lane, Current.LaneSize), Expected);
    }
  }
}

TEST(X86WideISAState, EvexPerElementCountAndAbsoluteValueHonorWriteMasks) {
  enum class Operation { Popcount, LeadingZeros, Absolute };
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t LaneSize;
    Operation Op;
    bool ZeroInactive;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf2, 0x7d, 0x49, 0x54, 0xc2}, 1, Operation::Popcount, false},
      {{0x62, 0xf2, 0xfd, 0xc9, 0x54, 0xc2}, 2, Operation::Popcount, true},
      {{0x62, 0xf2, 0x7d, 0xc9, 0x55, 0xc2}, 4, Operation::Popcount, true},
      {{0x62, 0xf2, 0xfd, 0x49, 0x55, 0xc2}, 8, Operation::Popcount, false},
      {{0x62, 0xf2, 0x7d, 0xc9, 0x44, 0xc2}, 4, Operation::LeadingZeros, true},
      {{0x62, 0xf2, 0xfd, 0x49, 0x44, 0xc2}, 8, Operation::LeadingZeros, false},
      {{0x62, 0xf2, 0xfd, 0xc9, 0x1f, 0xc2}, 8, Operation::Absolute, true},
  };

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0x5555555555555555);
  for (const Case &Current : Cases) {
    SCOPED_TRACE(testing::Message()
                 << "lane bytes=" << Current.LaneSize
                 << " op=" << static_cast<unsigned>(Current.Op));
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const size_t LaneCount = 64 / Current.LaneSize;
    const unsigned LaneBits = static_cast<unsigned>(Current.LaneSize * 8);
    const uint64_t LaneMask =
        Current.LaneSize == 8 ? UINT64_MAX : (UINT64_C(1) << LaneBits) - 1;
    std::vector<uint8_t> SourceValue(64), OldDestination(64);
    std::vector<uint64_t> ActiveValues(LaneCount);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      uint64_t Input = 0;
      uint64_t Expected = 0;
      if (Current.Op == Operation::Popcount) {
        if (Lane % 5 != 0)
          Input = UINT64_C(1) | (UINT64_C(1) << ((Lane * 7u) % LaneBits));
        uint64_t Bits = Input;
        while (Bits != 0) {
          Expected += Bits & 1u;
          Bits >>= 1;
        }
      } else if (Current.Op == Operation::LeadingZeros) {
        if (Lane % 5 != 0)
          Input = UINT64_C(1) << ((Lane * 7u) % LaneBits);
        if (Input == 0) {
          Expected = LaneBits;
        } else {
          unsigned Highest = 0;
          for (unsigned Bit = 0; Bit < LaneBits; ++Bit)
            if ((Input & (UINT64_C(1) << Bit)) != 0)
              Highest = Bit;
          Expected = LaneBits - Highest - 1;
        }
      } else {
        Input = Lane == 0
                    ? UINT64_C(0x8000000000000000)
                    : static_cast<uint64_t>(-static_cast<int64_t>(10 + Lane));
        Expected = UINT64_C(0) - Input;
      }
      setIntegerLane(SourceValue, Lane, Current.LaneSize, Input & LaneMask);
      setIntegerLane(OldDestination, Lane, Current.LaneSize,
                     (UINT64_C(0xa5000000) + Lane) & LaneMask);
      ActiveValues[Lane] = Expected & LaneMask;
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Source.Offset, SourceValue);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Expected =
          (MaskValue & (UINT64_C(1) << Lane)) != 0
              ? ActiveValues[Lane]
              : (Current.ZeroInactive
                     ? 0
                     : (UINT64_C(0xa5000000) + Lane) & LaneMask);
      EXPECT_EQ(getIntegerLane(*Result, Lane, Current.LaneSize), Expected);
    }
  }
}

TEST(X86WideISAState, EvexPackedMultiplyHonorsLaneWidthsAndWriteMasks) {
  enum class Operation {
    LowWord,
    LowDword,
    HighSignedWord,
    HighUnsignedWord,
    WidenSignedDword,
    WidenUnsignedDword,
  };
  struct Case {
    std::vector<uint8_t> Bytes;
    Operation Op;
    bool ZeroInactive;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf1, 0x6d, 0x49, 0xd5, 0xc3}, Operation::LowWord, false},
      {{0x62, 0xf2, 0x6d, 0xc9, 0x40, 0xc3}, Operation::LowDword, true},
      {{0x62, 0xf1, 0x6d, 0x49, 0xe5, 0xc3}, Operation::HighSignedWord, false},
      {{0x62, 0xf1, 0x6d, 0xc9, 0xe4, 0xc3}, Operation::HighUnsignedWord, true},
      {{0x62, 0xf2, 0xed, 0x49, 0x28, 0xc3},
       Operation::WidenSignedDword,
       false},
      {{0x62, 0xf1, 0xed, 0xc9, 0xf4, 0xc3},
       Operation::WidenUnsignedDword,
       true},
  };

  const RegInfo Left = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Right = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0x55555555);
  for (const Case &Current : Cases) {
    SCOPED_TRACE(testing::Message()
                 << "op=" << static_cast<unsigned>(Current.Op));
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const bool IsWidening = Current.Op == Operation::WidenSignedDword ||
                            Current.Op == Operation::WidenUnsignedDword;
    const size_t ResultSize =
        IsWidening ? 8 : (Current.Op == Operation::LowDword ? 4 : 2);
    const uint64_t ResultMask =
        ResultSize == 8 ? UINT64_MAX : (UINT64_C(1) << (ResultSize * 8)) - 1;
    const size_t LaneCount = 64 / ResultSize;
    std::vector<uint8_t> LeftValue(64), RightValue(64), OldDestination(64);
    std::vector<uint64_t> ActiveValues(LaneCount);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      uint64_t Expected = 0;
      if (Current.Op == Operation::LowWord) {
        const uint16_t A = static_cast<uint16_t>(300u + Lane);
        const uint16_t B = 7;
        setIntegerLane(LeftValue, Lane, 2, A);
        setIntegerLane(RightValue, Lane, 2, B);
        Expected = static_cast<uint16_t>(A * B);
      } else if (Current.Op == Operation::LowDword) {
        const uint32_t A = static_cast<uint32_t>(0x10000u + Lane);
        const uint32_t B = static_cast<uint32_t>(0x10000u - Lane);
        setIntegerLane(LeftValue, Lane, 4, A);
        setIntegerLane(RightValue, Lane, 4, B);
        Expected = static_cast<uint32_t>(A * B);
      } else if (Current.Op == Operation::HighSignedWord) {
        const int16_t A = static_cast<int16_t>(-1000 - static_cast<int>(Lane));
        const int16_t B = 300;
        setIntegerLane(LeftValue, Lane, 2, static_cast<uint16_t>(A));
        setIntegerLane(RightValue, Lane, 2, static_cast<uint16_t>(B));
        const int32_t Product = static_cast<int32_t>(A) * B;
        Expected = (static_cast<uint32_t>(Product) >> 16) & UINT64_C(0xffff);
      } else if (Current.Op == Operation::HighUnsignedWord) {
        const uint16_t A = static_cast<uint16_t>(60000u - Lane);
        const uint16_t B = 2;
        setIntegerLane(LeftValue, Lane, 2, A);
        setIntegerLane(RightValue, Lane, 2, B);
        Expected = (static_cast<uint32_t>(A) * B) >> 16;
      } else if (Current.Op == Operation::WidenSignedDword) {
        const int32_t A =
            static_cast<int32_t>(-100000 - static_cast<int>(Lane));
        const int32_t B = static_cast<int32_t>(300 + Lane);
        setIntegerLane(LeftValue, Lane * 2, 4, static_cast<uint32_t>(A));
        setIntegerLane(RightValue, Lane * 2, 4, static_cast<uint32_t>(B));
        Expected = static_cast<uint64_t>(static_cast<int64_t>(A) * B);
      } else {
        const uint32_t A = static_cast<uint32_t>(0xf0000000u + Lane);
        const uint32_t B = static_cast<uint32_t>(3u + Lane);
        setIntegerLane(LeftValue, Lane * 2, 4, A);
        setIntegerLane(RightValue, Lane * 2, 4, B);
        Expected = static_cast<uint64_t>(A) * B;
      }
      setIntegerLane(OldDestination, Lane, ResultSize,
                     UINT64_C(0xa5000000) + Lane);
      ActiveValues[Lane] = Expected;
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Left.Offset, LeftValue);
    Emulator.setRegisterBytes(Right.Offset, RightValue);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Expected =
          (MaskValue & (UINT64_C(1) << Lane)) != 0
              ? ActiveValues[Lane] & ResultMask
              : (Current.ZeroInactive
                     ? 0
                     : (UINT64_C(0xa5000000) + Lane) & ResultMask);
      EXPECT_EQ(getIntegerLane(*Result, Lane, ResultSize), Expected);
    }
  }
}

TEST(X86WideISAState, EvexPackedRotateAndVariableShiftHonorLaneSemantics) {
  enum class Operation {
    RotateLeftImmediate,
    RotateRightImmediate,
    RotateLeftVariable,
    RotateRightVariable,
    ShiftLeftVariable,
    ShiftRightArithmeticVariable,
  };
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t LaneSize;
    Operation Op;
    unsigned Immediate;
    bool ZeroInactive;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf1, 0x7d, 0xc9, 0x72, 0xca, 0x07},
       4,
       Operation::RotateLeftImmediate,
       7,
       true},
      {{0x62, 0xf1, 0xfd, 0x49, 0x72, 0xc2, 0x0b},
       8,
       Operation::RotateRightImmediate,
       11,
       false},
      {{0x62, 0xf2, 0x6d, 0xc9, 0x15, 0xc3},
       4,
       Operation::RotateLeftVariable,
       0,
       true},
      {{0x62, 0xf2, 0xed, 0x49, 0x14, 0xc3},
       8,
       Operation::RotateRightVariable,
       0,
       false},
      {{0x62, 0xf2, 0xed, 0xc9, 0x12, 0xc3},
       2,
       Operation::ShiftLeftVariable,
       0,
       true},
      {{0x62, 0xf2, 0xed, 0x49, 0x46, 0xc3},
       8,
       Operation::ShiftRightArithmeticVariable,
       0,
       false},
  };

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Counts = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0x5555555555555555);
  for (const Case &Current : Cases) {
    SCOPED_TRACE(testing::Message()
                 << "op=" << static_cast<unsigned>(Current.Op));
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const size_t LaneCount = 64 / Current.LaneSize;
    const unsigned LaneBits = static_cast<unsigned>(Current.LaneSize * 8);
    const uint64_t LaneMask =
        Current.LaneSize == 8 ? UINT64_MAX : (UINT64_C(1) << LaneBits) - 1;
    std::vector<uint8_t> SourceValue(64), CountValue(64), OldDestination(64);
    std::vector<uint64_t> ActiveValues(LaneCount);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      uint64_t Value = UINT64_C(0x8102030405060708) + Lane;
      Value &= LaneMask;
      const uint64_t Count = Lane == 0 ? LaneBits : Lane * 7u + 3u;
      setIntegerLane(SourceValue, Lane, Current.LaneSize, Value);
      setIntegerLane(CountValue, Lane, Current.LaneSize, Count);
      setIntegerLane(OldDestination, Lane, Current.LaneSize,
                     (UINT64_C(0xa5000000) + Lane) & LaneMask);

      uint64_t Expected = 0;
      if (Current.Op == Operation::RotateLeftImmediate ||
          Current.Op == Operation::RotateRightImmediate ||
          Current.Op == Operation::RotateLeftVariable ||
          Current.Op == Operation::RotateRightVariable) {
        const bool IsLeft = Current.Op == Operation::RotateLeftImmediate ||
                            Current.Op == Operation::RotateLeftVariable;
        const uint64_t Effective =
            (Current.Op == Operation::RotateLeftImmediate ||
             Current.Op == Operation::RotateRightImmediate)
                ? Current.Immediate & (LaneBits - 1)
                : Count & (LaneBits - 1);
        if (Effective == 0) {
          Expected = Value;
        } else if (IsLeft) {
          Expected =
              ((Value << Effective) | (Value >> (LaneBits - Effective))) &
              LaneMask;
        } else {
          Expected =
              ((Value >> Effective) | (Value << (LaneBits - Effective))) &
              LaneMask;
        }
      } else if (Current.Op == Operation::ShiftLeftVariable) {
        Expected = Count >= LaneBits ? 0 : (Value << Count) & LaneMask;
      } else {
        const unsigned Effective =
            Count >= LaneBits ? LaneBits - 1 : static_cast<unsigned>(Count);
        Expected =
            static_cast<uint64_t>(static_cast<int64_t>(Value) >> Effective);
      }
      ActiveValues[Lane] = Expected & LaneMask;
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Source.Offset, SourceValue);
    Emulator.setRegisterBytes(Counts.Offset, CountValue);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Expected =
          (MaskValue & (UINT64_C(1) << Lane)) != 0
              ? ActiveValues[Lane]
              : (Current.ZeroInactive
                     ? 0
                     : (UINT64_C(0xa5000000) + Lane) & LaneMask);
      EXPECT_EQ(getIntegerLane(*Result, Lane, Current.LaneSize), Expected);
    }
  }
}

TEST(X86WideISAState, EvexLegacyPackedShiftsHonorWriteMasks) {
  enum class Operation { LeftLogical, RightLogical, RightArithmetic };
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t LaneSize;
    Operation Op;
    unsigned Immediate;
    bool ZeroInactive;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf2, 0x6d, 0xc9, 0x47, 0xc3},
       4,
       Operation::LeftLogical,
       0,
       true},
      {{0x62, 0xf2, 0x6d, 0x49, 0x46, 0xc3},
       4,
       Operation::RightArithmetic,
       0,
       false},
      {{0x62, 0xf2, 0xed, 0xc9, 0x45, 0xc3},
       8,
       Operation::RightLogical,
       0,
       true},
      {{0x62, 0xf1, 0x7d, 0xc9, 0x72, 0xe2, 0x07},
       4,
       Operation::RightArithmetic,
       7,
       true},
  };

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Counts = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0x5555555555555555);
  for (const Case &Current : Cases) {
    SCOPED_TRACE(testing::Message()
                 << "op=" << static_cast<unsigned>(Current.Op));
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const unsigned LaneBits = static_cast<unsigned>(Current.LaneSize * 8);
    const size_t LaneCount = 64 / Current.LaneSize;
    const uint64_t LaneMask =
        Current.LaneSize == 8 ? UINT64_MAX : (UINT64_C(1) << LaneBits) - 1;
    std::vector<uint8_t> SourceValue(64), CountValue(64), OldDestination(64);
    std::vector<uint64_t> ActiveValues(LaneCount);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Value =
          (UINT64_C(0x8102030405060708) + Lane * UINT64_C(0x101)) & LaneMask;
      const uint64_t VariableCount = Lane == 0 ? LaneBits : Lane * 5u + 1u;
      const uint64_t Count =
          Current.Immediate != 0 ? Current.Immediate : VariableCount;
      setIntegerLane(SourceValue, Lane, Current.LaneSize, Value);
      setIntegerLane(CountValue, Lane, Current.LaneSize, VariableCount);
      setIntegerLane(OldDestination, Lane, Current.LaneSize,
                     (UINT64_C(0xa5000000) + Lane) & LaneMask);

      uint64_t Expected = 0;
      if (Current.Op == Operation::LeftLogical) {
        Expected = Count >= LaneBits ? 0 : (Value << Count) & LaneMask;
      } else if (Current.Op == Operation::RightLogical) {
        Expected = Count >= LaneBits ? 0 : Value >> Count;
      } else if (Count >= LaneBits) {
        Expected =
            (Value & (UINT64_C(1) << (LaneBits - 1))) != 0 ? LaneMask : 0;
      } else if (Count == 0) {
        Expected = Value;
      } else {
        Expected = Value >> Count;
        if ((Value & (UINT64_C(1) << (LaneBits - 1))) != 0)
          Expected |= LaneMask << (LaneBits - Count);
        Expected &= LaneMask;
      }
      ActiveValues[Lane] = Expected;
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Source.Offset, SourceValue);
    Emulator.setRegisterBytes(Counts.Offset, CountValue);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Expected =
          (MaskValue & (UINT64_C(1) << Lane)) != 0
              ? ActiveValues[Lane]
              : (Current.ZeroInactive
                     ? 0
                     : (UINT64_C(0xa5000000) + Lane) & LaneMask);
      EXPECT_EQ(getIntegerLane(*Result, Lane, Current.LaneSize), Expected);
    }
  }
}

TEST(X86WideISAState, EvexPackedFloatLogicHonorsElementWriteMasks) {
  enum class Operation { And, Xor };
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t LaneSize;
    Operation Op;
    bool ZeroInactive;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf1, 0x6c, 0x49, 0x54, 0xc3}, 4, Operation::And, false},
      {{0x62, 0xf1, 0xed, 0xc9, 0x57, 0xc3}, 8, Operation::Xor, true},
  };

  const RegInfo Left = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Right = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0x5555555555555555);
  for (const Case &Current : Cases) {
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const size_t LaneCount = 64 / Current.LaneSize;
    const uint64_t LaneMask =
        Current.LaneSize == 8 ? UINT64_MAX : UINT64_C(0xffffffff);
    std::vector<uint8_t> LeftValue(64), RightValue(64), OldDestination(64);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      setIntegerLane(LeftValue, Lane, Current.LaneSize,
                     (UINT64_C(0xf0f00ff0aa550000) + Lane) & LaneMask);
      setIntegerLane(RightValue, Lane, Current.LaneSize,
                     (UINT64_C(0x0ff0ffff12345678) + Lane * 3) & LaneMask);
      setIntegerLane(OldDestination, Lane, Current.LaneSize,
                     (UINT64_C(0xa5000000) + Lane) & LaneMask);
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Left.Offset, LeftValue);
    Emulator.setRegisterBytes(Right.Offset, RightValue);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t A = getIntegerLane(LeftValue, Lane, Current.LaneSize);
      const uint64_t B = getIntegerLane(RightValue, Lane, Current.LaneSize);
      const uint64_t Active = Current.Op == Operation::And ? A & B : A ^ B;
      const uint64_t Expected =
          (MaskValue & (UINT64_C(1) << Lane)) != 0
              ? Active
              : (Current.ZeroInactive
                     ? 0
                     : (UINT64_C(0xa5000000) + Lane) & LaneMask);
      EXPECT_EQ(getIntegerLane(*Result, Lane, Current.LaneSize), Expected);
    }
  }
}

TEST(X86WideISAState, EvexTernaryLogicUsesOldDestinationAndWriteMask) {
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t LaneSize;
    bool ZeroInactive;
  };
  std::vector<Case> Cases = {
      {{0x62, 0xf3, 0x6d, 0xc9, 0x25, 0xc3, 0x00}, 4, true},
      {{0x62, 0xf3, 0xed, 0x49, 0x25, 0xc3, 0x00}, 8, false},
  };

  const RegInfo Left = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Right = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0x5555555555555555);
  constexpr uint8_t TruthTables[] = {0x00, 0xff, 0x96, 0xca, 0xe8};
  for (Case Current : Cases) {
    for (uint8_t TruthTable : TruthTables) {
      Current.Bytes.back() = TruthTable;
      SCOPED_TRACE(testing::Message()
                   << "lane-size=" << Current.LaneSize
                   << " truth-table=" << static_cast<unsigned>(TruthTable));
      const std::vector<LowOp> Ops = liftX64(Current.Bytes);
      ASSERT_FALSE(Ops.empty());
      ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

      const size_t LaneCount = 64 / Current.LaneSize;
      const unsigned LaneBits = static_cast<unsigned>(Current.LaneSize * 8);
      const uint64_t LaneMask =
          Current.LaneSize == 8 ? UINT64_MAX : (UINT64_C(1) << LaneBits) - 1;
      std::vector<uint8_t> LeftValue(64), RightValue(64), OldDestination(64);
      std::vector<uint64_t> ActiveValues(LaneCount);
      for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
        const uint64_t Old =
            (UINT64_C(0x0123456789abcdef) + Lane * 17) & LaneMask;
        const uint64_t A = (UINT64_C(0xf0f00ff0aa550000) + Lane * 3) & LaneMask;
        const uint64_t B = (UINT64_C(0x0ff0ffff12345678) + Lane * 5) & LaneMask;
        setIntegerLane(OldDestination, Lane, Current.LaneSize, Old);
        setIntegerLane(LeftValue, Lane, Current.LaneSize, A);
        setIntegerLane(RightValue, Lane, Current.LaneSize, B);
        uint64_t Expected = 0;
        for (unsigned Bit = 0; Bit < LaneBits; ++Bit) {
          const unsigned Index =
              static_cast<unsigned>(((Old >> Bit) & 1) << 2) |
              static_cast<unsigned>(((A >> Bit) & 1) << 1) |
              static_cast<unsigned>((B >> Bit) & 1);
          Expected |= static_cast<uint64_t>((TruthTable >> Index) & 1) << Bit;
        }
        ActiveValues[Lane] = Expected;
      }

      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      NdOpEmulator Emulator(Image);
      Emulator.setRegisterBytes(Left.Offset, LeftValue);
      Emulator.setRegisterBytes(Right.Offset, RightValue);
      Emulator.setRegisterBytes(Destination.Offset, OldDestination);
      Emulator.setRegister(Mask.Offset, MaskValue);
      EXPECT_EQ(Emulator.run(Ops), Ops.size());
      const auto Result = Emulator.getRegisterBytes(Destination.Offset);
      ASSERT_TRUE(Result);
      for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
        const uint64_t Expected =
            (MaskValue & (UINT64_C(1) << Lane)) != 0
                ? ActiveValues[Lane]
                : (Current.ZeroInactive ? 0
                                        : getIntegerLane(OldDestination, Lane,
                                                         Current.LaneSize));
        EXPECT_EQ(getIntegerLane(*Result, Lane, Current.LaneSize), Expected);
      }
    }
  }
}

TEST(X86WideISAState, EvexPackedTestProducesMaskedLaneBitmap) {
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t VectorSize;
    size_t LaneSize;
    bool Negated;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf2, 0x6d, 0x49, 0x26, 0xd3}, 64, 1, false},
      {{0x62, 0xf2, 0xed, 0x49, 0x26, 0xd3}, 64, 2, false},
      {{0x62, 0xf2, 0x6d, 0x49, 0x27, 0xd3}, 64, 4, false},
      {{0x62, 0xf2, 0xed, 0x49, 0x27, 0xd3}, 64, 8, false},
      {{0x62, 0xf2, 0x6e, 0x49, 0x26, 0xd3}, 64, 1, true},
      {{0x62, 0xf2, 0xee, 0x49, 0x26, 0xd3}, 64, 2, true},
      {{0x62, 0xf2, 0x6e, 0x49, 0x27, 0xd3}, 64, 4, true},
      {{0x62, 0xf2, 0xee, 0x49, 0x27, 0xd3}, 64, 8, true},
      {{0x62, 0xf2, 0x6d, 0x09, 0x27, 0xd3}, 16, 4, false},
      {{0x62, 0xf2, 0x6d, 0x29, 0x27, 0xd3}, 32, 4, false},
  };

  const RegInfo Left = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Right = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_K2);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0xb6db6db6db6db6db);
  for (const Case &Current : Cases) {
    SCOPED_TRACE(testing::Message() << "vector-size=" << Current.VectorSize
                                    << " lane-size=" << Current.LaneSize
                                    << " negated=" << Current.Negated);
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const size_t LaneCount = Current.VectorSize / Current.LaneSize;
    const unsigned LaneBits = static_cast<unsigned>(Current.LaneSize * 8);
    const uint64_t LaneMask =
        Current.LaneSize == 8 ? UINT64_MAX : (UINT64_C(1) << LaneBits) - 1;
    std::vector<uint8_t> LeftValue(64), RightValue(64);
    uint64_t Expected = 0;
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const bool AndIsNonzero = Lane % 2 == 0;
      const uint64_t A = UINT64_C(0xaaaaaaaaaaaaaaaa) & LaneMask;
      const uint64_t B = (AndIsNonzero ? UINT64_C(1) << (LaneBits - 1)
                                       : UINT64_C(0x5555555555555555)) &
                         LaneMask;
      setIntegerLane(LeftValue, Lane, Current.LaneSize, A);
      setIntegerLane(RightValue, Lane, Current.LaneSize, B);
      if ((Current.Negated ? !AndIsNonzero : AndIsNonzero) &&
          (MaskValue & (UINT64_C(1) << Lane)) != 0)
        Expected |= UINT64_C(1) << Lane;
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Left.Offset, LeftValue);
    Emulator.setRegisterBytes(Right.Offset, RightValue);
    Emulator.setRegister(Destination.Offset, UINT64_MAX);
    Emulator.setRegister(WriteMask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    ASSERT_TRUE(Emulator.getRegister(Destination.Offset));
    EXPECT_EQ(*Emulator.getRegister(Destination.Offset), Expected);
  }
}

TEST(X86WideISAState, EvexPackedBlendSelectsBetweenBothSources) {
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t VectorSize;
    size_t LaneSize;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf2, 0x6d, 0x49, 0x66, 0xc3}, 64, 1},
      {{0x62, 0xf2, 0xed, 0x49, 0x66, 0xc3}, 64, 2},
      {{0x62, 0xf2, 0x6d, 0x49, 0x64, 0xc3}, 64, 4},
      {{0x62, 0xf2, 0xed, 0x49, 0x64, 0xc3}, 64, 8},
      {{0x62, 0xf2, 0x6d, 0x09, 0x64, 0xc3}, 16, 4},
      {{0x62, 0xf2, 0x6d, 0x29, 0x64, 0xc3}, 32, 4},
  };

  const RegInfo Left = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Right = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0xb6db6db6db6db6db);
  for (const Case &Current : Cases) {
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const size_t LaneCount = Current.VectorSize / Current.LaneSize;
    const unsigned LaneBits = static_cast<unsigned>(Current.LaneSize * 8);
    const uint64_t LaneMask =
        Current.LaneSize == 8 ? UINT64_MAX : (UINT64_C(1) << LaneBits) - 1;
    std::vector<uint8_t> LeftValue(64), RightValue(64),
        OldDestination(64, 0xa5);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      setIntegerLane(LeftValue, Lane, Current.LaneSize,
                     (UINT64_C(0x1111111111111100) + Lane) & LaneMask);
      setIntegerLane(RightValue, Lane, Current.LaneSize,
                     (UINT64_C(0xeeeeeeeeeeeeee00) + Lane * 3) & LaneMask);
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Left.Offset, LeftValue);
    Emulator.setRegisterBytes(Right.Offset, RightValue);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const std::vector<uint8_t> &Selected =
          (MaskValue & (UINT64_C(1) << Lane)) != 0 ? RightValue : LeftValue;
      EXPECT_EQ(getIntegerLane(*Result, Lane, Current.LaneSize),
                getIntegerLane(Selected, Lane, Current.LaneSize));
    }
    EXPECT_TRUE(std::all_of(Result->begin() + Current.VectorSize, Result->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
  }
}

TEST(X86WideISAState, EvexConflictDetectionReportsPriorEqualLanes) {
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t VectorSize;
    size_t LaneSize;
    bool ZeroInactive;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf2, 0x7d, 0x09, 0xc4, 0xc2}, 16, 4, false},
      {{0x62, 0xf2, 0x7d, 0xa9, 0xc4, 0xc2}, 32, 4, true},
      {{0x62, 0xf2, 0x7d, 0x49, 0xc4, 0xc2}, 64, 4, false},
      {{0x62, 0xf2, 0xfd, 0x89, 0xc4, 0xc2}, 16, 8, true},
      {{0x62, 0xf2, 0xfd, 0x29, 0xc4, 0xc2}, 32, 8, false},
      {{0x62, 0xf2, 0xfd, 0xc9, 0xc4, 0xc2}, 64, 8, true},
  };

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0xb6db);
  for (const Case &Current : Cases) {
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const size_t LaneCount = Current.VectorSize / Current.LaneSize;
    std::vector<uint8_t> SourceValue(64), OldDestination(64, 0xa5);
    std::vector<uint64_t> ActiveValues(LaneCount);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Value = UINT64_C(0x12340000) + Lane % 3;
      setIntegerLane(SourceValue, Lane, Current.LaneSize, Value);
      uint64_t ConflictBits = 0;
      for (size_t Prior = 0; Prior < Lane; ++Prior)
        if (Prior % 3 == Lane % 3)
          ConflictBits |= UINT64_C(1) << Prior;
      ActiveValues[Lane] = ConflictBits;
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Source.Offset, SourceValue);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Expected =
          (MaskValue & (UINT64_C(1) << Lane)) != 0
              ? ActiveValues[Lane]
              : (Current.ZeroInactive
                     ? 0
                     : getIntegerLane(OldDestination, Lane, Current.LaneSize));
      EXPECT_EQ(getIntegerLane(*Result, Lane, Current.LaneSize), Expected);
    }
    EXPECT_TRUE(std::all_of(Result->begin() + Current.VectorSize, Result->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
  }
}

TEST(X86WideISAState, EvexIfmaAccumulatesLowAndHighProductHalves) {
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t VectorSize;
    bool HighHalf;
    bool ZeroInactive;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf2, 0xed, 0x09, 0xb4, 0xc3}, 16, false, false},
      {{0x62, 0xf2, 0xed, 0xa9, 0xb4, 0xc3}, 32, false, true},
      {{0x62, 0xf2, 0xed, 0x49, 0xb4, 0xc3}, 64, false, false},
      {{0x62, 0xf2, 0xed, 0x89, 0xb5, 0xc3}, 16, true, true},
      {{0x62, 0xf2, 0xed, 0x29, 0xb5, 0xc3}, 32, true, false},
      {{0x62, 0xf2, 0xed, 0xc9, 0xb5, 0xc3}, 64, true, true},
  };

  constexpr uint64_t OperandMask = UINT64_C(0x000fffffffffffff);
  constexpr uint64_t HalfMask = (UINT64_C(1) << 26) - 1;
  auto HighProductHalf = [=](uint64_t A, uint64_t B) {
    const uint64_t A0 = A & HalfMask;
    const uint64_t A1 = A >> 26;
    const uint64_t B0 = B & HalfMask;
    const uint64_t B1 = B >> 26;
    const uint64_t Carry = (A0 * B0) >> 26;
    const uint64_t Cross = A0 * B1 + A1 * B0 + Carry;
    return (A1 * B1 + (Cross >> 26)) & OperandMask;
  };

  const RegInfo Left = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Right = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0xb6);
  for (const Case &Current : Cases) {
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const size_t LaneCount = Current.VectorSize / 8;
    std::vector<uint8_t> LeftValue(64), RightValue(64), OldDestination(64);
    std::vector<uint64_t> ActiveValues(LaneCount);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t A =
          (UINT64_C(0x000fedcba9876543) + Lane * 13) & OperandMask;
      const uint64_t B =
          (UINT64_C(0x000abcde12345678) + Lane * 17) & OperandMask;
      const uint64_t Old = UINT64_C(0xfedcba9876543210) + Lane;
      setIntegerLane(LeftValue, Lane, 8, A);
      setIntegerLane(RightValue, Lane, 8, B);
      setIntegerLane(OldDestination, Lane, 8, Old);
      const uint64_t Contribution =
          Current.HighHalf ? HighProductHalf(A, B) : (A * B) & OperandMask;
      ActiveValues[Lane] = Old + Contribution;
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Left.Offset, LeftValue);
    Emulator.setRegisterBytes(Right.Offset, RightValue);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Expected =
          (MaskValue & (UINT64_C(1) << Lane)) != 0
              ? ActiveValues[Lane]
              : (Current.ZeroInactive
                     ? 0
                     : getIntegerLane(OldDestination, Lane, 8));
      EXPECT_EQ(getIntegerLane(*Result, Lane, 8), Expected);
    }
    EXPECT_TRUE(std::all_of(Result->begin() + Current.VectorSize, Result->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
  }
}

TEST(X86WideISAState, EvexDoubleShiftsConcatenateTheSpecifiedLaneSources) {
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t LaneSize;
    bool Left;
    bool Variable;
    unsigned Immediate;
    bool ZeroInactive;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf3, 0xed, 0xc9, 0x70, 0xc3, 0x07}, 2, true, false, 7, true},
      {{0x62, 0xf3, 0x6d, 0x49, 0x71, 0xc3, 0x0d}, 4, true, false, 13, false},
      {{0x62, 0xf3, 0xed, 0xc9, 0x71, 0xc3, 0x1d}, 8, true, false, 29, true},
      {{0x62, 0xf3, 0xed, 0x49, 0x72, 0xc3, 0x07}, 2, false, false, 7, false},
      {{0x62, 0xf3, 0x6d, 0xc9, 0x73, 0xc3, 0x0d}, 4, false, false, 13, true},
      {{0x62, 0xf3, 0xed, 0x49, 0x73, 0xc3, 0x1d}, 8, false, false, 29, false},
      {{0x62, 0xf2, 0xed, 0xc9, 0x70, 0xc3}, 2, true, true, 0, true},
      {{0x62, 0xf2, 0x6d, 0x49, 0x71, 0xc3}, 4, true, true, 0, false},
      {{0x62, 0xf2, 0xed, 0xc9, 0x71, 0xc3}, 8, true, true, 0, true},
      {{0x62, 0xf2, 0xed, 0x49, 0x72, 0xc3}, 2, false, true, 0, false},
      {{0x62, 0xf2, 0x6d, 0xc9, 0x73, 0xc3}, 4, false, true, 0, true},
      {{0x62, 0xf2, 0xed, 0x49, 0x73, 0xc3}, 8, false, true, 0, false},
  };

  const RegInfo First = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo SecondOrCounts = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0xb6db6db6db6db6db);
  for (const Case &Current : Cases) {
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const unsigned LaneBits = static_cast<unsigned>(Current.LaneSize * 8);
    const size_t LaneCount = 64 / Current.LaneSize;
    const uint64_t LaneMask =
        Current.LaneSize == 8 ? UINT64_MAX : (UINT64_C(1) << LaneBits) - 1;
    std::vector<uint8_t> FirstValue(64), SecondValue(64), OldDestination(64);
    std::vector<uint64_t> ActiveValues(LaneCount);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t A = (UINT64_C(0x0123456789abcdef) + Lane * 5) & LaneMask;
      const uint64_t B = (UINT64_C(0xfedcba9876543210) + Lane * 7) & LaneMask;
      const uint64_t Old =
          (UINT64_C(0x8102030405060708) + Lane * 11) & LaneMask;
      const uint64_t CountSource = Lane == 0 ? LaneBits : Lane * 9u + 3u;
      setIntegerLane(FirstValue, Lane, Current.LaneSize, A);
      setIntegerLane(SecondValue, Lane, Current.LaneSize,
                     Current.Variable ? CountSource : B);
      setIntegerLane(OldDestination, Lane, Current.LaneSize, Old);
      const uint64_t Count =
          (Current.Variable ? CountSource : Current.Immediate) & (LaneBits - 1);
      const uint64_t Primary = Current.Variable ? Old : A;
      const uint64_t Secondary = Current.Variable ? A : B;
      uint64_t Expected = Primary;
      if (Count != 0) {
        if (Current.Left)
          Expected = ((Primary << Count) | (Secondary >> (LaneBits - Count))) &
                     LaneMask;
        else
          Expected = ((Primary >> Count) | (Secondary << (LaneBits - Count))) &
                     LaneMask;
      }
      ActiveValues[Lane] = Expected;
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(First.Offset, FirstValue);
    Emulator.setRegisterBytes(SecondOrCounts.Offset, SecondValue);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Expected =
          (MaskValue & (UINT64_C(1) << Lane)) != 0
              ? ActiveValues[Lane]
              : (Current.ZeroInactive
                     ? 0
                     : getIntegerLane(OldDestination, Lane, Current.LaneSize));
      EXPECT_EQ(getIntegerLane(*Result, Lane, Current.LaneSize), Expected);
    }
  }
}

TEST(X86WideISAState, EvexMultishiftSelectsWrappedBitWindowsPerByte) {
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t VectorSize;
    bool ZeroInactive;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf2, 0xed, 0x09, 0x83, 0xc3}, 16, false},
      {{0x62, 0xf2, 0xed, 0xa9, 0x83, 0xc3}, 32, true},
      {{0x62, 0xf2, 0xed, 0x49, 0x83, 0xc3}, 64, false},
  };

  const RegInfo Controls = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Data = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0xb6db6db6db6db6db);
  constexpr uint8_t Counts[8] = {0, 1, 7, 8, 31, 56, 57, 63};
  for (const Case &Current : Cases) {
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    std::vector<uint8_t> ControlValue(64), DataValue(64),
        OldDestination(64, 0xa5);
    std::vector<uint8_t> ActiveValues(Current.VectorSize);
    for (size_t Qword = 0; Qword < Current.VectorSize / 8; ++Qword) {
      const uint64_t Source =
          UINT64_C(0x8102030405060708) + Qword * UINT64_C(0x11111111);
      setIntegerLane(DataValue, Qword, 8, Source);
      for (size_t Byte = 0; Byte < 8; ++Byte) {
        const size_t Lane = Qword * 8 + Byte;
        const unsigned Count = Counts[Byte];
        ControlValue[Lane] = static_cast<uint8_t>(Count | (Qword << 6));
        const uint64_t Rotated =
            Count == 0 ? Source : (Source >> Count) | (Source << (64 - Count));
        ActiveValues[Lane] = static_cast<uint8_t>(Rotated);
      }
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Controls.Offset, ControlValue);
    Emulator.setRegisterBytes(Data.Offset, DataValue);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    for (size_t Lane = 0; Lane < Current.VectorSize; ++Lane) {
      const uint8_t Expected =
          (MaskValue & (UINT64_C(1) << Lane)) != 0
              ? ActiveValues[Lane]
              : (Current.ZeroInactive ? 0 : OldDestination[Lane]);
      EXPECT_EQ((*Result)[Lane], Expected);
    }
    EXPECT_TRUE(std::all_of(Result->begin() + Current.VectorSize, Result->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
  }
}

TEST(X86WideISAState, EvexByteAndWordPermutesUseMaskedLaneIndices) {
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t VectorSize;
    size_t ElementSize;
    bool ZeroInactive;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf2, 0x6d, 0x09, 0x8d, 0xc3}, 16, 1, false},
      {{0x62, 0xf2, 0x6d, 0xa9, 0x8d, 0xc3}, 32, 1, true},
      {{0x62, 0xf2, 0x6d, 0x49, 0x8d, 0xc3}, 64, 1, false},
      {{0x62, 0xf2, 0xed, 0x89, 0x8d, 0xc3}, 16, 2, true},
      {{0x62, 0xf2, 0xed, 0x29, 0x8d, 0xc3}, 32, 2, false},
      {{0x62, 0xf2, 0xed, 0xc9, 0x8d, 0xc3}, 64, 2, true},
  };

  const RegInfo Indices = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Data = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0xb6db6db6db6db6db);
  for (const Case &Current : Cases) {
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    const size_t LaneCount = Current.VectorSize / Current.ElementSize;
    std::vector<uint8_t> IndexValue(64), DataValue(64),
        OldDestination(64, 0xa5);
    std::vector<uint64_t> ActiveValues(LaneCount);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Index = Lane * 7 + LaneCount + 3;
      setIntegerLane(IndexValue, Lane, Current.ElementSize, Index);
      setIntegerLane(DataValue, Lane, Current.ElementSize,
                     UINT64_C(0x5100) + Lane * 13);
    }
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Index =
          getIntegerLane(IndexValue, Lane, Current.ElementSize) &
          (LaneCount - 1);
      ActiveValues[Lane] =
          getIntegerLane(DataValue, Index, Current.ElementSize);
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Indices.Offset, IndexValue);
    Emulator.setRegisterBytes(Data.Offset, DataValue);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Expected =
          (MaskValue & (UINT64_C(1) << Lane)) != 0
              ? ActiveValues[Lane]
              : (Current.ZeroInactive ? 0
                                      : getIntegerLane(OldDestination, Lane,
                                                       Current.ElementSize));
      EXPECT_EQ(getIntegerLane(*Result, Lane, Current.ElementSize), Expected);
    }
    EXPECT_TRUE(std::all_of(Result->begin() + Current.VectorSize, Result->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
  }
}

TEST(X86WideISAState, EvexBitShufflePacksSelectedQwordBitsIntoMask) {
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t VectorSize;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf2, 0x6d, 0x09, 0x8f, 0xd3}, 16},
      {{0x62, 0xf2, 0x6d, 0x29, 0x8f, 0xd3}, 32},
      {{0x62, 0xf2, 0x6d, 0x49, 0x8f, 0xd3}, 64},
  };

  const RegInfo Data = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Controls = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_K2);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0xb6db6db6db6db6db);
  constexpr uint8_t Counts[8] = {0, 1, 7, 8, 31, 56, 57, 63};
  for (const Case &Current : Cases) {
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    std::vector<uint8_t> DataValue(64), ControlValue(64);
    uint64_t Expected = 0;
    for (size_t Qword = 0; Qword < Current.VectorSize / 8; ++Qword) {
      const uint64_t Source =
          UINT64_C(0x8102030405060708) + Qword * UINT64_C(0x11111111);
      setIntegerLane(DataValue, Qword, 8, Source);
      for (size_t Byte = 0; Byte < 8; ++Byte) {
        const size_t Lane = Qword * 8 + Byte;
        const unsigned Count = Counts[Byte];
        ControlValue[Lane] = static_cast<uint8_t>(Count | (Qword << 6));
        if (((Source >> Count) & 1) != 0 &&
            (MaskValue & (UINT64_C(1) << Lane)) != 0)
          Expected |= UINT64_C(1) << Lane;
      }
    }

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Data.Offset, DataValue);
    Emulator.setRegisterBytes(Controls.Offset, ControlValue);
    Emulator.setRegister(Destination.Offset, UINT64_MAX);
    Emulator.setRegister(WriteMask.Offset, MaskValue);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    ASSERT_TRUE(Emulator.getRegister(Destination.Offset));
    EXPECT_EQ(*Emulator.getRegister(Destination.Offset), Expected);
  }
}

TEST(X86WideISAState, EvexBroadcastMaskReplicatesOnlyArchitecturalSourceBits) {
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t VectorSize;
    size_t ElementSize;
    uint64_t ElementValue;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf2, 0xfe, 0x08, 0x2a, 0xc1}, 16, 8, UINT64_C(0xef)},
      {{0x62, 0xf2, 0xfe, 0x28, 0x2a, 0xc1}, 32, 8, UINT64_C(0xef)},
      {{0x62, 0xf2, 0xfe, 0x48, 0x2a, 0xc1}, 64, 8, UINT64_C(0xef)},
      {{0x62, 0xf2, 0x7e, 0x08, 0x3a, 0xc1}, 16, 4, UINT64_C(0xbeef)},
      {{0x62, 0xf2, 0x7e, 0x28, 0x3a, 0xc1}, 32, 4, UINT64_C(0xbeef)},
      {{0x62, 0xf2, 0x7e, 0x48, 0x3a, 0xc1}, 64, 4, UINT64_C(0xbeef)},
  };

  const RegInfo Source = mapCapstoneReg(X86_REG_K1);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  constexpr uint64_t SourceValue = UINT64_C(0x76543210deadbeef);
  for (const Case &Current : Cases) {
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegister(Source.Offset, SourceValue);
    Emulator.setRegisterBytes(Destination.Offset,
                              std::vector<uint8_t>(64, 0xa5));
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    for (size_t Lane = 0; Lane < Current.VectorSize / Current.ElementSize;
         ++Lane)
      EXPECT_EQ(getIntegerLane(*Result, Lane, Current.ElementSize),
                Current.ElementValue);
    EXPECT_TRUE(std::all_of(Result->begin() + Current.VectorSize, Result->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
  }
}

TEST(X86WideISAState, EvexCompressExpandRegistersHonorTopologyAndTailPolicy) {
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t VectorSize;
    size_t ElementSize;
    bool Compress;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf2, 0x7d, 0x89, 0x63, 0xd0}, 16, 1, true},
      {{0x62, 0xf2, 0x7d, 0xa9, 0x63, 0xd0}, 32, 1, true},
      {{0x62, 0xf2, 0x7d, 0xc9, 0x63, 0xd0}, 64, 1, true},
      {{0x62, 0xf2, 0xfd, 0x89, 0x63, 0xd0}, 16, 2, true},
      {{0x62, 0xf2, 0xfd, 0xa9, 0x63, 0xd0}, 32, 2, true},
      {{0x62, 0xf2, 0xfd, 0xc9, 0x63, 0xd0}, 64, 2, true},
      {{0x62, 0xf2, 0x7d, 0x89, 0x8b, 0xd0}, 16, 4, true},
      {{0x62, 0xf2, 0x7d, 0xa9, 0x8b, 0xd0}, 32, 4, true},
      {{0x62, 0xf2, 0x7d, 0xc9, 0x8b, 0xd0}, 64, 4, true},
      {{0x62, 0xf2, 0xfd, 0x89, 0x8b, 0xd0}, 16, 8, true},
      {{0x62, 0xf2, 0xfd, 0xa9, 0x8b, 0xd0}, 32, 8, true},
      {{0x62, 0xf2, 0xfd, 0xc9, 0x8b, 0xd0}, 64, 8, true},
      {{0x62, 0xf2, 0x7d, 0x89, 0x62, 0xc2}, 16, 1, false},
      {{0x62, 0xf2, 0x7d, 0xa9, 0x62, 0xc2}, 32, 1, false},
      {{0x62, 0xf2, 0x7d, 0xc9, 0x62, 0xc2}, 64, 1, false},
      {{0x62, 0xf2, 0xfd, 0x89, 0x62, 0xc2}, 16, 2, false},
      {{0x62, 0xf2, 0xfd, 0xa9, 0x62, 0xc2}, 32, 2, false},
      {{0x62, 0xf2, 0xfd, 0xc9, 0x62, 0xc2}, 64, 2, false},
      {{0x62, 0xf2, 0x7d, 0x89, 0x89, 0xc2}, 16, 4, false},
      {{0x62, 0xf2, 0x7d, 0xa9, 0x89, 0xc2}, 32, 4, false},
      {{0x62, 0xf2, 0x7d, 0xc9, 0x89, 0xc2}, 64, 4, false},
      {{0x62, 0xf2, 0xfd, 0x89, 0x89, 0xc2}, 16, 8, false},
      {{0x62, 0xf2, 0xfd, 0xa9, 0x89, 0xc2}, 32, 8, false},
      {{0x62, 0xf2, 0xfd, 0xc9, 0x89, 0xc2}, 64, 8, false},
      {{0x62, 0xf2, 0x7d, 0x89, 0x8a, 0xd0}, 16, 4, true},
      {{0x62, 0xf2, 0x7d, 0xa9, 0x8a, 0xd0}, 32, 4, true},
      {{0x62, 0xf2, 0x7d, 0xc9, 0x8a, 0xd0}, 64, 4, true},
      {{0x62, 0xf2, 0xfd, 0x89, 0x8a, 0xd0}, 16, 8, true},
      {{0x62, 0xf2, 0xfd, 0xa9, 0x8a, 0xd0}, 32, 8, true},
      {{0x62, 0xf2, 0xfd, 0xc9, 0x8a, 0xd0}, 64, 8, true},
      {{0x62, 0xf2, 0x7d, 0x89, 0x88, 0xc2}, 16, 4, false},
      {{0x62, 0xf2, 0x7d, 0xa9, 0x88, 0xc2}, 32, 4, false},
      {{0x62, 0xf2, 0x7d, 0xc9, 0x88, 0xc2}, 64, 4, false},
      {{0x62, 0xf2, 0xfd, 0x89, 0x88, 0xc2}, 16, 8, false},
      {{0x62, 0xf2, 0xfd, 0xa9, 0x88, 0xc2}, 32, 8, false},
      {{0x62, 0xf2, 0xfd, 0xc9, 0x88, 0xc2}, 64, 8, false},
  };

  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0xd6b5a96d5ab5696d);
  for (const Case &Current : Cases) {
    for (const bool ZeroInactive : {false, true}) {
      std::vector<uint8_t> Bytes = Current.Bytes;
      Bytes[3] = static_cast<uint8_t>((Bytes[3] & ~0x80u) |
                                      (ZeroInactive ? 0x80u : 0));
      const std::vector<LowOp> Ops = liftX64(Bytes);
      ASSERT_FALSE(Ops.empty());
      ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

      const size_t LaneCount = Current.VectorSize / Current.ElementSize;
      std::vector<uint8_t> SourceValue(64), OldDestination(64, 0xa5);
      for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
        const uint64_t LaneValue =
            Current.ElementSize == 1
                ? UINT64_C(0x80) + Lane
                : (Current.ElementSize == 2
                       ? UINT64_C(0x8100) + Lane * 17
                       : UINT64_C(0x8102030405060708) + Lane * 29);
        setIntegerLane(SourceValue, Lane, Current.ElementSize, LaneValue);
        setIntegerLane(OldDestination, Lane, Current.ElementSize,
                       UINT64_C(0x5a5a5a5a5a5a5a5a) + Lane);
      }

      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setRegisterBytes(Source.Offset, SourceValue);
      Emulator.setRegisterBytes(Destination.Offset, OldDestination);
      Emulator.setRegister(Mask.Offset, MaskValue);
      EXPECT_EQ(Emulator.run(Ops), Ops.size());
      const auto Result = Emulator.getRegisterBytes(Destination.Offset);
      ASSERT_TRUE(Result);

      std::vector<uint64_t> Expected(LaneCount);
      if (Current.Compress) {
        size_t OutputLane = 0;
        for (size_t SourceLane = 0; SourceLane < LaneCount; ++SourceLane)
          if ((MaskValue & (UINT64_C(1) << SourceLane)) != 0)
            Expected[OutputLane++] =
                getIntegerLane(SourceValue, SourceLane, Current.ElementSize);
        for (; OutputLane < LaneCount; ++OutputLane)
          Expected[OutputLane] =
              ZeroInactive ? 0
                           : getIntegerLane(OldDestination, OutputLane,
                                            Current.ElementSize);
      } else {
        size_t SourceLane = 0;
        for (size_t OutputLane = 0; OutputLane < LaneCount; ++OutputLane) {
          if ((MaskValue & (UINT64_C(1) << OutputLane)) != 0)
            Expected[OutputLane] =
                getIntegerLane(SourceValue, SourceLane++, Current.ElementSize);
          else
            Expected[OutputLane] =
                ZeroInactive ? 0
                             : getIntegerLane(OldDestination, OutputLane,
                                              Current.ElementSize);
        }
      }
      for (size_t Lane = 0; Lane < LaneCount; ++Lane)
        EXPECT_EQ(getIntegerLane(*Result, Lane, Current.ElementSize),
                  Expected[Lane]);
      EXPECT_TRUE(std::all_of(Result->begin() + Current.VectorSize,
                              Result->end(),
                              [](uint8_t Byte) { return Byte == 0; }));
      EXPECT_FALSE(Emulator.skips().any());
    }
  }

  struct RegisterCase {
    std::vector<uint8_t> Bytes;
    x86_reg Destination;
    x86_reg Source;
    x86_reg Mask;
    size_t VectorSize;
    size_t ElementSize;
    bool Compress;
    bool ZeroInactive;
  };
  const std::vector<RegisterCase> RegisterCases = {
      {{0x62, 0x22, 0x7d, 0xce, 0x8a, 0xf1},
       X86_REG_ZMM17,
       X86_REG_ZMM30,
       X86_REG_K6,
       64,
       4,
       true,
       true},
      {{0x62, 0x22, 0xfd, 0x2d, 0x88, 0xe8},
       X86_REG_YMM29,
       X86_REG_YMM16,
       X86_REG_K5,
       32,
       8,
       false,
       false},
      {{0x62, 0x22, 0x7d, 0x48, 0x8a, 0xf1},
       X86_REG_ZMM17,
       X86_REG_ZMM30,
       X86_REG_INVALID,
       64,
       4,
       true,
       false},
      {{0x62, 0x22, 0xfd, 0x28, 0x88, 0xe8},
       X86_REG_YMM29,
       X86_REG_YMM16,
       X86_REG_INVALID,
       32,
       8,
       false,
       false},
  };
  for (const RegisterCase &Current : RegisterCases) {
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));
    const size_t LaneCount = Current.VectorSize / Current.ElementSize;
    std::vector<uint8_t> SourceValue(64), OldDestination(64, 0xa5);
    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      setIntegerLane(SourceValue, Lane, Current.ElementSize,
                     UINT64_C(0x1122334455667788) + Lane * 37);
      setIntegerLane(OldDestination, Lane, Current.ElementSize,
                     UINT64_C(0x8877665544332211) + Lane * 19);
    }
    constexpr uint64_t HighMaskValue = UINT64_C(0xa5b6d);
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    const RegInfo DestinationInfo = mapCapstoneReg(Current.Destination);
    const RegInfo SourceInfo = mapCapstoneReg(Current.Source);
    Emulator.setRegisterBytes(DestinationInfo.Offset, OldDestination);
    Emulator.setRegisterBytes(SourceInfo.Offset, SourceValue);
    if (Current.Mask != X86_REG_INVALID)
      Emulator.setRegister(mapCapstoneReg(Current.Mask).Offset, HighMaskValue);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(DestinationInfo.Offset);
    ASSERT_TRUE(Result);

    std::vector<uint64_t> Expected(LaneCount);
    if (Current.Mask == X86_REG_INVALID) {
      for (size_t Lane = 0; Lane < LaneCount; ++Lane)
        Expected[Lane] = getIntegerLane(SourceValue, Lane, Current.ElementSize);
    } else if (Current.Compress) {
      size_t OutputLane = 0;
      for (size_t SourceLane = 0; SourceLane < LaneCount; ++SourceLane)
        if ((HighMaskValue & (UINT64_C(1) << SourceLane)) != 0)
          Expected[OutputLane++] =
              getIntegerLane(SourceValue, SourceLane, Current.ElementSize);
      for (; OutputLane < LaneCount; ++OutputLane)
        Expected[OutputLane] = Current.ZeroInactive
                                   ? 0
                                   : getIntegerLane(OldDestination, OutputLane,
                                                    Current.ElementSize);
    } else {
      size_t SourceLane = 0;
      for (size_t OutputLane = 0; OutputLane < LaneCount; ++OutputLane) {
        if ((HighMaskValue & (UINT64_C(1) << OutputLane)) != 0)
          Expected[OutputLane] =
              getIntegerLane(SourceValue, SourceLane++, Current.ElementSize);
        else
          Expected[OutputLane] =
              Current.ZeroInactive ? 0
                                   : getIntegerLane(OldDestination, OutputLane,
                                                    Current.ElementSize);
      }
    }
    for (size_t Lane = 0; Lane < LaneCount; ++Lane)
      EXPECT_EQ(getIntegerLane(*Result, Lane, Current.ElementSize),
                Expected[Lane]);
    EXPECT_TRUE(std::all_of(Result->begin() + Current.VectorSize, Result->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
    EXPECT_FALSE(Emulator.skips().any());
  }

}

TEST(X86WideISAState, EvexVnniDotProductsUseExactSignednessAndSaturation) {
  struct Case {
    std::vector<uint8_t> Bytes;
    size_t VectorSize;
    bool Words;
    bool Saturating;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0xf2, 0x6d, 0x89, 0x50, 0xc3}, 16, false, false},
      {{0x62, 0xf2, 0x6d, 0xa9, 0x50, 0xc3}, 32, false, false},
      {{0x62, 0xf2, 0x6d, 0xc9, 0x50, 0xc3}, 64, false, false},
      {{0x62, 0xf2, 0x6d, 0x89, 0x51, 0xc3}, 16, false, true},
      {{0x62, 0xf2, 0x6d, 0xa9, 0x51, 0xc3}, 32, false, true},
      {{0x62, 0xf2, 0x6d, 0xc9, 0x51, 0xc3}, 64, false, true},
      {{0x62, 0xf2, 0x6d, 0x89, 0x52, 0xc3}, 16, true, false},
      {{0x62, 0xf2, 0x6d, 0xa9, 0x52, 0xc3}, 32, true, false},
      {{0x62, 0xf2, 0x6d, 0xc9, 0x52, 0xc3}, 64, true, false},
      {{0x62, 0xf2, 0x6d, 0x89, 0x53, 0xc3}, 16, true, true},
      {{0x62, 0xf2, 0x6d, 0xa9, 0x53, 0xc3}, 32, true, true},
      {{0x62, 0xf2, 0x6d, 0xc9, 0x53, 0xc3}, 64, true, true},
  };

  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo FirstSource = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo SecondSource = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  constexpr uint64_t MaskValue = UINT64_C(0xb6db);
  for (const Case &Current : Cases) {
    for (unsigned MaskMode = 0; MaskMode < 3; ++MaskMode) {
      const bool HasMask = MaskMode != 0;
      const bool ZeroInactive = MaskMode == 2;
      std::vector<uint8_t> Bytes = Current.Bytes;
      Bytes[3] = static_cast<uint8_t>((Bytes[3] & 0x78u) | (HasMask ? 1u : 0u) |
                                      (ZeroInactive ? 0x80u : 0u));
      const std::vector<LowOp> Ops = liftX64(Bytes);
      ASSERT_FALSE(Ops.empty());
      ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

      const size_t LaneCount = Current.VectorSize / 4;
      std::vector<uint8_t> DestinationValue(64, 0xa5), FirstValue(64),
          SecondValue(64);
      std::vector<int64_t> Contributions(LaneCount);
      for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
        int32_t Accumulator;
        if (Lane % 3 == 0)
          Accumulator = INT32_MAX - 10;
        else if (Lane % 3 == 1)
          Accumulator = INT32_MIN + 10;
        else
          Accumulator = static_cast<int32_t>(0x1020304 + Lane * 31);
        setDwordLane(DestinationValue, Lane,
                     static_cast<uint32_t>(Accumulator));

        int64_t Dot = 0;
        const size_t ElementsPerLane = Current.Words ? 2 : 4;
        for (size_t Element = 0; Element < ElementsPerLane; ++Element) {
          const size_t Index = Lane * ElementsPerLane + Element;
          if (Current.Words) {
            const int16_t First =
                Lane % 3 == 0   ? INT16_MAX
                : Lane % 3 == 1 ? INT16_MIN
                                : static_cast<int16_t>(-311 + Element * 97);
            const int16_t Second =
                Lane % 3 == 0   ? INT16_MAX
                : Lane % 3 == 1 ? INT16_MAX
                                : static_cast<int16_t>(257 - Element * 61);
            setIntegerLane(FirstValue, Index, 2, static_cast<uint16_t>(First));
            setIntegerLane(SecondValue, Index, 2,
                           static_cast<uint16_t>(Second));
            Dot += static_cast<int64_t>(First) * Second;
          } else {
            const uint8_t First = Lane % 3 < 2
                                      ? UINT8_MAX
                                      : static_cast<uint8_t>(17 + Element * 13);
            const int8_t Second = Lane % 3 == 0 ? INT8_MAX
                                  : Lane % 3 == 1
                                      ? INT8_MIN
                                      : static_cast<int8_t>(-31 + Element * 11);
            FirstValue[Index] = First;
            SecondValue[Index] = static_cast<uint8_t>(Second);
            Dot += static_cast<int64_t>(First) * Second;
          }
        }
        Contributions[Lane] = Dot;
      }

      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      NdOpEmulator Emulator(Image);
      Emulator.setRegisterBytes(Destination.Offset, DestinationValue);
      Emulator.setRegisterBytes(FirstSource.Offset, FirstValue);
      Emulator.setRegisterBytes(SecondSource.Offset, SecondValue);
      Emulator.setRegister(Mask.Offset, MaskValue);
      EXPECT_EQ(Emulator.run(Ops), Ops.size());
      const auto Result = Emulator.getRegisterBytes(Destination.Offset);
      ASSERT_TRUE(Result);
      for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
        const bool Active =
            !HasMask || (MaskValue & (UINT64_C(1) << Lane)) != 0;
        uint32_t Expected;
        if (!Active) {
          Expected = ZeroInactive ? 0 : getDwordLane(DestinationValue, Lane);
        } else {
          const int64_t Accumulator =
              static_cast<int32_t>(getDwordLane(DestinationValue, Lane));
          int64_t Sum = Accumulator + Contributions[Lane];
          if (Current.Saturating)
            Sum = std::clamp(Sum, static_cast<int64_t>(INT32_MIN),
                             static_cast<int64_t>(INT32_MAX));
          Expected = static_cast<uint32_t>(Sum);
        }
        EXPECT_EQ(getDwordLane(*Result, Lane), Expected);
      }
      EXPECT_TRUE(std::all_of(Result->begin() + Current.VectorSize,
                              Result->end(),
                              [](uint8_t Byte) { return Byte == 0; }));
    }
  }
}

TEST(X86WideISAState, AmxTileRegistersHaveIndependentMaximumSizeContainers) {
  const RegInfo Tmm0 = mapCapstoneReg(X86_REG_TMM0);
  const RegInfo Tmm7 = mapCapstoneReg(X86_REG_TMM7);
  EXPECT_EQ(Tmm0.Size, 1024u);
  EXPECT_EQ(Tmm7.Size, 1024u);
  EXPECT_GE(Tmm7.Offset, Tmm0.Offset + Tmm0.Size);
  EXPECT_STREQ(getX86RegName(Tmm0.Offset, Tmm0.Size), "TMM0");
  EXPECT_STREQ(getX86RegName(Tmm7.Offset, Tmm7.Size), "TMM7");
}

TEST(X86WideISAState, LiftedTilezeroClearsTheWholePhysicalTileContainer) {
  // tilezero tmm6
  const std::vector<LowOp> Ops = liftX64({0xc4, 0xe2, 0x7b, 0x49, 0xf0});
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

  const RegInfo Destination = mapCapstoneReg(X86_REG_TMM6);
  ASSERT_EQ(Destination.Size, 1024u);
  std::vector<uint8_t> Initial(Destination.Size);
  for (size_t I = 0; I < Initial.size(); ++I)
    Initial[I] = static_cast<uint8_t>(I * 29u + 7u);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  std::vector<uint8_t> Config(x86reg::TileConfigSize, 0);
  Config[0] = 1; // palette 1
  Config[1] = 1; // TILEZERO must clear restart progress on success
  Config[16 + 6 * 2] = 8;
  Config[48 + 6] = 2;
  Emulator.setRegisterBytes(x86reg::TileConfig, Config);
  Emulator.setRegisterBytes(Destination.Offset, Initial);
  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  ASSERT_EQ(Result->size(), Destination.Size);
  EXPECT_TRUE(std::all_of(Result->begin(), Result->end(),
                          [](uint8_t Byte) { return Byte == 0; }));
  const auto ResultConfig = Emulator.getRegisterBytes(x86reg::TileConfig);
  ASSERT_TRUE(ResultConfig);
  EXPECT_EQ((*ResultConfig)[1], 0);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86WideISAState, PextrImmediateHighBitsUseArchitecturalLaneMask) {
  struct Case {
    std::vector<uint8_t> Bytes;
    uint64_t Expected;
  };
  const std::vector<Case> Cases = {
      // The immediate's unused high bits are architecturally ignored.  0xff
      // therefore selects the last byte/word/dword/qword lane respectively.
      {{0x66, 0x0f, 0x3a, 0x14, 0xc8, 0xff}, UINT64_C(0x0f)},
      {{0x66, 0x0f, 0xc5, 0xc1, 0xff}, UINT64_C(0x0f0e)},
      {{0x66, 0x0f, 0x3a, 0x16, 0xc8, 0xff}, UINT64_C(0x0f0e0d0c)},
      {{0x66, 0x48, 0x0f, 0x3a, 0x16, 0xc8, 0xff},
       UINT64_C(0x0f0e0d0c0b0a0908)},
      {{0xc4, 0xe3, 0x79, 0x14, 0xc8, 0xff}, UINT64_C(0x0f)},
      {{0xc5, 0xf9, 0xc5, 0xc1, 0xff}, UINT64_C(0x0f0e)},
      {{0xc4, 0xe3, 0x79, 0x16, 0xc8, 0xff}, UINT64_C(0x0f0e0d0c)},
      {{0xc4, 0xe3, 0xf9, 0x16, 0xc8, 0xff}, UINT64_C(0x0f0e0d0c0b0a0908)},
  };

  const RegInfo Source = mapCapstoneReg(X86_REG_XMM1);
  const RegInfo Destination = mapCapstoneReg(X86_REG_RAX);
  std::vector<uint8_t> SourceValue(16);
  for (size_t I = 0; I < SourceValue.size(); ++I)
    SourceValue[I] = static_cast<uint8_t>(I);

  for (const Case &Current : Cases) {
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(Source.Offset, SourceValue);
    Emulator.setRegister(Destination.Offset, UINT64_MAX);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    ASSERT_TRUE(Emulator.getRegister(Destination.Offset));
    EXPECT_EQ(*Emulator.getRegister(Destination.Offset), Current.Expected);
  }
}

TEST(X86WideISAState, LegacyByteShiftsPreserveFullXmmWidth) {
  struct Case {
    std::vector<uint8_t> Bytes;
    bool ShiftLeft;
    unsigned Count;
  };
  const std::vector<Case> Cases = {
      {{0x66, 0x0f, 0x73, 0xf9, 0x04}, true, 4},
      {{0x66, 0x0f, 0x73, 0xd9, 0x04}, false, 4},
      {{0x66, 0x0f, 0x73, 0xf9, 0x14}, true, 20},
      {{0x66, 0x0f, 0x73, 0xd9, 0x14}, false, 20},
  };

  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM1);
  std::vector<uint8_t> Initial(64);
  for (size_t I = 0; I < Initial.size(); ++I)
    Initial[I] = static_cast<uint8_t>(I + 1);

  for (const Case &Current : Cases) {
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(Destination.Offset, Initial);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    ASSERT_EQ(Result->size(), Initial.size());

    std::vector<uint8_t> Expected = Initial;
    for (unsigned Byte = 0; Byte < 16; ++Byte) {
      const int SourceByte =
          Current.ShiftLeft
              ? static_cast<int>(Byte) - static_cast<int>(Current.Count)
              : static_cast<int>(Byte) + static_cast<int>(Current.Count);
      Expected[Byte] = SourceByte >= 0 && SourceByte < 16
                           ? Initial[static_cast<unsigned>(SourceByte)]
                           : 0;
    }
    EXPECT_EQ(*Result, Expected);
  }
}

TEST(X86WideISAState, PtestDefinesAllArchitecturalFlagsAcrossFullXmm) {
  const std::vector<LowOp> Ops =
      liftX64({0x66, 0x0f, 0x38, 0x17, 0xca}); // ptest xmm1, xmm2
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

  const RegInfo Left = mapCapstoneReg(X86_REG_XMM1);
  const RegInfo Right = mapCapstoneReg(X86_REG_XMM2);
  struct Case {
    uint8_t LeftHighByte;
    uint8_t RightHighByte;
    uint64_t ZF;
    uint64_t CF;
  };
  const Case Cases[] = {
      {0x01, 0x01, 0, 1}, // high-half intersection is non-zero
      {0x00, 0x01, 1, 0}, // high-half right bit is outside the left mask
  };

  for (const Case &Current : Cases) {
    std::vector<uint8_t> LeftValue(16, 0), RightValue(16, 0);
    LeftValue[8] = Current.LeftHighByte;
    RightValue[8] = Current.RightHighByte;

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(Left.Offset, LeftValue);
    Emulator.setRegisterBytes(Right.Offset, RightValue);
    for (uint64_t Flag : {x86reg::OF, x86reg::SF, x86reg::AF, x86reg::PF,
                          x86reg::ZF, x86reg::CF})
      Emulator.setRegister(Flag, 1);

    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    ASSERT_TRUE(Emulator.getRegister(x86reg::ZF));
    ASSERT_TRUE(Emulator.getRegister(x86reg::CF));
    EXPECT_EQ(*Emulator.getRegister(x86reg::ZF), Current.ZF);
    EXPECT_EQ(*Emulator.getRegister(x86reg::CF), Current.CF);
    for (uint64_t Flag : {x86reg::OF, x86reg::SF, x86reg::AF, x86reg::PF}) {
      ASSERT_TRUE(Emulator.getRegister(Flag));
      EXPECT_EQ(*Emulator.getRegister(Flag), 0u);
    }
  }
}

TEST(X86WideISAState, PshufdLegacyAndVexPreserveEncodingUpperState) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    unsigned VectorBytes;
    bool Legacy;
  };
  const Case Cases[] = {
      {"legacy-128", {0x66, 0x0f, 0x70, 0xd9, 0x1b}, 16, true},
      {"vex-128", {0xc5, 0xf9, 0x70, 0xd9, 0x1b}, 16, false},
      {"vex-256", {0xc5, 0xfd, 0x70, 0xd9, 0x1b}, 32, false},
  };

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM1);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM3);
  std::vector<uint8_t> SourceValue(64, 0);
  std::vector<uint8_t> InitialDestination(64, 0xa5);
  for (unsigned Lane = 0; Lane < 16; ++Lane)
    setDwordLane(SourceValue, Lane, UINT32_C(0x10203040) + Lane);

  for (const Case &Current : Cases) {
    SCOPED_TRACE(Current.Name);
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(Source.Offset, SourceValue);
    Emulator.setRegisterBytes(Destination.Offset, InitialDestination);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());

    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    ASSERT_EQ(Result->size(), 64u);
    std::vector<uint8_t> Expected = InitialDestination;
    if (!Current.Legacy)
      std::fill(Expected.begin(), Expected.end(), 0);
    for (unsigned Lane = 0; Lane < Current.VectorBytes / 4; ++Lane) {
      const unsigned LaneBase = (Lane / 4) * 4;
      const unsigned SourceLane = LaneBase + 3 - (Lane % 4);
      setDwordLane(Expected, Lane, getDwordLane(SourceValue, SourceLane));
    }
    EXPECT_EQ(*Result, Expected);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86WideISAState, PshuflwLegacyAndVexPreserveEncodingUpperState) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    unsigned VectorBytes;
    bool Legacy;
  };
  const Case Cases[] = {
      {"legacy-128", {0xf2, 0x0f, 0x70, 0xd9, 0x1b}, 16, true},
      {"vex-128", {0xc5, 0xfb, 0x70, 0xd9, 0x1b}, 16, false},
      {"vex-256", {0xc5, 0xff, 0x70, 0xd9, 0x1b}, 32, false},
  };

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM1);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM3);
  std::vector<uint8_t> SourceValue(64, 0);
  std::vector<uint8_t> InitialDestination(64, 0xa5);
  for (unsigned Lane = 0; Lane < 32; ++Lane)
    setIntegerLane(SourceValue, Lane, 2, UINT16_C(0x2100) + Lane);

  for (const Case &Current : Cases) {
    SCOPED_TRACE(Current.Name);
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(Source.Offset, SourceValue);
    Emulator.setRegisterBytes(Destination.Offset, InitialDestination);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());

    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    ASSERT_EQ(Result->size(), 64u);
    std::vector<uint8_t> Expected = InitialDestination;
    if (!Current.Legacy)
      std::fill(Expected.begin(), Expected.end(), 0);
    for (unsigned Lane = 0; Lane < Current.VectorBytes / 2; ++Lane) {
      const unsigned LaneBase = (Lane / 8) * 8;
      const unsigned InLane = Lane % 8;
      const unsigned SourceLane = InLane < 4 ? LaneBase + 3 - InLane : Lane;
      setIntegerLane(Expected, Lane, 2,
                     getIntegerLane(SourceValue, SourceLane, 2));
    }
    EXPECT_EQ(*Result, Expected);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86WideISAState, PshufhwLegacyAndVexPreserveEncodingUpperState) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    unsigned VectorBytes;
    bool Legacy;
  };
  const Case Cases[] = {
      {"legacy-128", {0xf3, 0x0f, 0x70, 0xd9, 0x1b}, 16, true},
      {"vex-128", {0xc5, 0xfa, 0x70, 0xd9, 0x1b}, 16, false},
      {"vex-256", {0xc5, 0xfe, 0x70, 0xd9, 0x1b}, 32, false},
  };

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM1);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM3);
  std::vector<uint8_t> SourceValue(64, 0);
  std::vector<uint8_t> InitialDestination(64, 0xa5);
  for (unsigned Lane = 0; Lane < 32; ++Lane)
    setIntegerLane(SourceValue, Lane, 2, UINT16_C(0x3100) + Lane);

  for (const Case &Current : Cases) {
    SCOPED_TRACE(Current.Name);
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(Source.Offset, SourceValue);
    Emulator.setRegisterBytes(Destination.Offset, InitialDestination);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());

    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    ASSERT_EQ(Result->size(), 64u);
    std::vector<uint8_t> Expected = InitialDestination;
    if (!Current.Legacy)
      std::fill(Expected.begin(), Expected.end(), 0);
    for (unsigned Lane = 0; Lane < Current.VectorBytes / 2; ++Lane) {
      const unsigned LaneBase = (Lane / 8) * 8;
      const unsigned InLane = Lane % 8;
      const unsigned SourceLane =
          InLane < 4 ? Lane : LaneBase + 7 - (InLane - 4);
      setIntegerLane(Expected, Lane, 2,
                     getIntegerLane(SourceValue, SourceLane, 2));
    }
    EXPECT_EQ(*Result, Expected);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86WideISAState, MmxAliasesOnlyTheLowEightBytesOfX87Slots) {
  const x86_reg MmxRegisters[] = {
      X86_REG_MM0, X86_REG_MM1, X86_REG_MM2, X86_REG_MM3,
      X86_REG_MM4, X86_REG_MM5, X86_REG_MM6, X86_REG_MM7,
  };
  const x86_reg X87Registers[] = {
      X86_REG_ST0, X86_REG_ST1, X86_REG_ST2, X86_REG_ST3,
      X86_REG_ST4, X86_REG_ST5, X86_REG_ST6, X86_REG_ST7,
  };
  const char *MmxNames[] = {"MM0", "MM1", "MM2", "MM3",
                            "MM4", "MM5", "MM6", "MM7"};

  for (unsigned I = 0; I < 8; ++I) {
    const RegInfo Mmx = mapCapstoneReg(MmxRegisters[I]);
    const RegInfo X87 = mapCapstoneReg(X87Registers[I]);
    EXPECT_EQ(Mmx.Offset, X87.Offset);
    EXPECT_EQ(Mmx.Size, 8u);
    EXPECT_EQ(X87.Size, x86reg::FPURegSize);
    EXPECT_STREQ(getX86RegName(Mmx.Offset, Mmx.Size), MmxNames[I]);
  }
}

TEST(X86WideISAState,
     PmovmskbDecodedFormsExtractMsbsZeroExtendAndPreserveSourceState) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    x86_reg OperandRegister;
    x86_reg StateRegister;
    unsigned MaskBytes;
  };
  const Case Cases[] = {
      {"legacy-mmx", {0x0f, 0xd7, 0xc1}, X86_REG_MM1, X86_REG_ST1, 8},
      {"legacy-xmm", {0x66, 0x0f, 0xd7, 0xc1}, X86_REG_XMM1, X86_REG_ZMM1, 16},
      {"vex-128", {0xc5, 0xf9, 0xd7, 0xc1}, X86_REG_XMM1, X86_REG_ZMM1, 16},
      {"vex-256", {0xc5, 0xfd, 0xd7, 0xc1}, X86_REG_YMM1, X86_REG_ZMM1, 32},
  };

  const RegInfo Destination = mapCapstoneReg(X86_REG_RAX);
  for (const Case &Current : Cases) {
    SCOPED_TRACE(Current.Name);
    const RegInfo Operand = mapCapstoneReg(Current.OperandRegister);
    const RegInfo State = mapCapstoneReg(Current.StateRegister);
    ASSERT_EQ(Operand.Offset, State.Offset);
    ASSERT_EQ(Operand.Size, Current.MaskBytes);

    std::vector<uint8_t> SourceValue(State.Size);
    uint32_t ExpectedMask = 0;
    for (unsigned Byte = 0; Byte < SourceValue.size(); ++Byte) {
      SourceValue[Byte] = static_cast<uint8_t>(Byte * 37u + 0x19u);
      if (Byte < Current.MaskBytes && (SourceValue[Byte] & 0x80) != 0)
        ExpectedMask |= UINT32_C(1) << Byte;
    }

    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(State.Offset, SourceValue);
    Emulator.setRegister(Destination.Offset, UINT64_MAX);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    ASSERT_TRUE(Emulator.getRegister(Destination.Offset));
    EXPECT_EQ(*Emulator.getRegister(Destination.Offset), ExpectedMask);

    const auto PreservedSource = Emulator.getRegisterBytes(State.Offset);
    ASSERT_TRUE(PreservedSource);
    EXPECT_EQ(*PreservedSource, SourceValue);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86WideISAState,
     PhminposuwReturnsFirstUnsignedMinimumAndDefinesUpperState) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    bool Legacy;
  };
  const Case Cases[] = {
      {"legacy-128", {0x66, 0x0f, 0x38, 0x41, 0xd9}, true},
      {"vex-128", {0xc4, 0xe2, 0x79, 0x41, 0xd9}, false},
  };

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM1);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM3);
  const uint16_t Words[] = {0x8000, 0xffff, 0x0003, 0x7fff,
                            0x1234, 0x0003, 0x0004, 0x9000};
  std::vector<uint8_t> SourceValue(64, 0x6d);
  std::vector<uint8_t> InitialDestination(64, 0xa5);
  for (unsigned I = 0; I < 8; ++I)
    setIntegerLane(SourceValue, I, 2, Words[I]);

  for (const Case &Current : Cases) {
    SCOPED_TRACE(Current.Name);
    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(Source.Offset, SourceValue);
    Emulator.setRegisterBytes(Destination.Offset, InitialDestination);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());

    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    ASSERT_EQ(Result->size(), 64u);
    std::vector<uint8_t> Expected = InitialDestination;
    std::fill(Expected.begin(), Expected.begin() + 16, 0);
    if (!Current.Legacy)
      std::fill(Expected.begin() + 16, Expected.end(), 0);
    setIntegerLane(Expected, 0, 2, 0x0003);
    setIntegerLane(Expected, 1, 2, 2);
    EXPECT_EQ(*Result, Expected);

    const auto PreservedSource = Emulator.getRegisterBytes(Source.Offset);
    ASSERT_TRUE(PreservedSource);
    EXPECT_EQ(*PreservedSource, SourceValue);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86WideISAState, PunpckFamiliesInterleavePerLaneAcrossLegacyAndVexWidths) {
  enum class Form { Mmx, LegacyXmm, Vex128, Vex256 };
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    unsigned ElementBytes;
    bool HighHalf;
    Form EncodingForm;
  };
  const Case Cases[] = {
      {"mmx-punpcklbw", {0x0f, 0x60, 0xd9}, 1, false, Form::Mmx},
      {"mmx-punpckhbw", {0x0f, 0x68, 0xd9}, 1, true, Form::Mmx},
      {"mmx-punpcklwd", {0x0f, 0x61, 0xd9}, 2, false, Form::Mmx},
      {"mmx-punpckhwd", {0x0f, 0x69, 0xd9}, 2, true, Form::Mmx},
      {"mmx-punpckldq", {0x0f, 0x62, 0xd9}, 4, false, Form::Mmx},
      {"mmx-punpckhdq", {0x0f, 0x6a, 0xd9}, 4, true, Form::Mmx},
      {"xmm-punpcklbw", {0x66, 0x0f, 0x60, 0xd9}, 1, false, Form::LegacyXmm},
      {"xmm-punpckhbw", {0x66, 0x0f, 0x68, 0xd9}, 1, true, Form::LegacyXmm},
      {"xmm-punpcklwd", {0x66, 0x0f, 0x61, 0xd9}, 2, false, Form::LegacyXmm},
      {"xmm-punpckhwd", {0x66, 0x0f, 0x69, 0xd9}, 2, true, Form::LegacyXmm},
      {"xmm-punpckldq", {0x66, 0x0f, 0x62, 0xd9}, 4, false, Form::LegacyXmm},
      {"xmm-punpckhdq", {0x66, 0x0f, 0x6a, 0xd9}, 4, true, Form::LegacyXmm},
      {"xmm-punpcklqdq", {0x66, 0x0f, 0x6c, 0xd9}, 8, false, Form::LegacyXmm},
      {"xmm-punpckhqdq", {0x66, 0x0f, 0x6d, 0xd9}, 8, true, Form::LegacyXmm},
      {"vex128-vpunpcklbw", {0xc5, 0xf1, 0x60, 0xda}, 1, false, Form::Vex128},
      {"vex128-vpunpckhbw", {0xc5, 0xf1, 0x68, 0xda}, 1, true, Form::Vex128},
      {"vex128-vpunpcklwd", {0xc5, 0xf1, 0x61, 0xda}, 2, false, Form::Vex128},
      {"vex128-vpunpckhwd", {0xc5, 0xf1, 0x69, 0xda}, 2, true, Form::Vex128},
      {"vex128-vpunpckldq", {0xc5, 0xf1, 0x62, 0xda}, 4, false, Form::Vex128},
      {"vex128-vpunpckhdq", {0xc5, 0xf1, 0x6a, 0xda}, 4, true, Form::Vex128},
      {"vex128-vpunpcklqdq", {0xc5, 0xf1, 0x6c, 0xda}, 8, false, Form::Vex128},
      {"vex128-vpunpckhqdq", {0xc5, 0xf1, 0x6d, 0xda}, 8, true, Form::Vex128},
      {"vex256-vpunpcklbw", {0xc5, 0xf5, 0x60, 0xda}, 1, false, Form::Vex256},
      {"vex256-vpunpckhbw", {0xc5, 0xf5, 0x68, 0xda}, 1, true, Form::Vex256},
      {"vex256-vpunpcklwd", {0xc5, 0xf5, 0x61, 0xda}, 2, false, Form::Vex256},
      {"vex256-vpunpckhwd", {0xc5, 0xf5, 0x69, 0xda}, 2, true, Form::Vex256},
      {"vex256-vpunpckldq", {0xc5, 0xf5, 0x62, 0xda}, 4, false, Form::Vex256},
      {"vex256-vpunpckhdq", {0xc5, 0xf5, 0x6a, 0xda}, 4, true, Form::Vex256},
      {"vex256-vpunpcklqdq", {0xc5, 0xf5, 0x6c, 0xda}, 8, false, Form::Vex256},
      {"vex256-vpunpckhqdq", {0xc5, 0xf5, 0x6d, 0xda}, 8, true, Form::Vex256},
  };

  auto Pattern = [](size_t Size, uint8_t Seed) {
    std::vector<uint8_t> Value(Size);
    for (size_t I = 0; I < Size; ++I)
      Value[I] = static_cast<uint8_t>(Seed + I * 13u);
    return Value;
  };

  for (const Case &Current : Cases) {
    SCOPED_TRACE(Current.Name);
    const bool IsMmx = Current.EncodingForm == Form::Mmx;
    const bool IsLegacy = IsMmx || Current.EncodingForm == Form::LegacyXmm;
    const unsigned VectorBytes = IsMmx                                  ? 8
                                 : Current.EncodingForm == Form::Vex256 ? 32
                                                                        : 16;
    const unsigned LaneBytes = IsMmx ? 8 : 16;

    const RegInfo DestinationState =
        mapCapstoneReg(IsMmx ? X86_REG_ST3 : X86_REG_ZMM3);
    const RegInfo LeftState = mapCapstoneReg(
        IsLegacy ? (IsMmx ? X86_REG_ST3 : X86_REG_ZMM3) : X86_REG_ZMM1);
    const RegInfo RightState = mapCapstoneReg(IsMmx      ? X86_REG_ST1
                                              : IsLegacy ? X86_REG_ZMM1
                                                         : X86_REG_ZMM2);
    if (IsLegacy)
      ASSERT_EQ(DestinationState.Offset, LeftState.Offset);
    else
      ASSERT_NE(DestinationState.Offset, LeftState.Offset);
    ASSERT_GE(DestinationState.Size, VectorBytes);
    ASSERT_GE(RightState.Size, VectorBytes);

    std::vector<uint8_t> LeftValue = Pattern(LeftState.Size, 0x11);
    std::vector<uint8_t> RightValue = Pattern(RightState.Size, 0x83);
    std::vector<uint8_t> InitialDestination =
        IsLegacy ? LeftValue : Pattern(DestinationState.Size, 0xd5);
    std::vector<uint8_t> Expected = InitialDestination;
    if (!IsLegacy)
      std::fill(Expected.begin() + VectorBytes, Expected.end(), 0);

    const unsigned ElementsPerLane = LaneBytes / Current.ElementBytes;
    const unsigned FirstElement = Current.HighHalf ? ElementsPerLane / 2 : 0;
    for (unsigned LaneBase = 0; LaneBase < VectorBytes; LaneBase += LaneBytes) {
      for (unsigned I = 0; I < ElementsPerLane / 2; ++I) {
        for (unsigned Byte = 0; Byte < Current.ElementBytes; ++Byte) {
          Expected[LaneBase + (I * 2) * Current.ElementBytes + Byte] =
              LeftValue[LaneBase + (FirstElement + I) * Current.ElementBytes +
                        Byte];
          Expected[LaneBase + (I * 2 + 1) * Current.ElementBytes + Byte] =
              RightValue[LaneBase + (FirstElement + I) * Current.ElementBytes +
                         Byte];
        }
      }
    }

    const std::vector<LowOp> Ops = liftX64(Current.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));
    EXPECT_TRUE(std::none_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
      return Op.Opcode == NdOp::INTRINSIC;
    }));

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    if (!IsLegacy)
      Emulator.setRegisterBytes(LeftState.Offset, LeftValue);
    Emulator.setRegisterBytes(RightState.Offset, RightValue);
    Emulator.setRegisterBytes(DestinationState.Offset, InitialDestination);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());

    const auto Result = Emulator.getRegisterBytes(DestinationState.Offset);
    ASSERT_TRUE(Result);
    EXPECT_EQ(*Result, Expected);
    const auto PreservedRight = Emulator.getRegisterBytes(RightState.Offset);
    ASSERT_TRUE(PreservedRight);
    EXPECT_EQ(*PreservedRight, RightValue);
    if (!IsLegacy) {
      const auto PreservedLeft = Emulator.getRegisterBytes(LeftState.Offset);
      ASSERT_TRUE(PreservedLeft);
      EXPECT_EQ(*PreservedLeft, LeftValue);
    }
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86WideISAState, EvexPunpckHonorsMasksMemoryFaultsAndUpperState) {
  enum class MaskMode { K0Unmasked, Merge, Zero };
  struct Family {
    const char *Name;
    uint8_t Opcode;
    unsigned ElementBytes;
    bool HighHalf;
  };
  const Family Families[] = {
      {"vpunpcklbw", 0x60, 1, false},  {"vpunpckhbw", 0x68, 1, true},
      {"vpunpcklwd", 0x61, 2, false},  {"vpunpckhwd", 0x69, 2, true},
      {"vpunpckldq", 0x62, 4, false},  {"vpunpckhdq", 0x6a, 4, true},
      {"vpunpcklqdq", 0x6c, 8, false}, {"vpunpckhqdq", 0x6d, 8, true},
  };

  auto Encoding = [](const Family &Current, unsigned VectorBytes, MaskMode Mode,
                     bool Memory, bool Broadcast = false) {
    const uint8_t Length = VectorBytes == 16   ? 0x08
                           : VectorBytes == 32 ? 0x28
                                               : 0x48;
    uint8_t Mask = Mode == MaskMode::K0Unmasked ? 0 : 1;
    if (Mode == MaskMode::Zero)
      Mask |= 0x80;
    if (Broadcast)
      Mask |= 0x10;
    const uint8_t Width = Current.ElementBytes == 8 ? 0xf5 : 0x75;
    return std::vector<uint8_t>{
        0x62,           0xf1,
        Width,          static_cast<uint8_t>(Length | Mask),
        Current.Opcode, static_cast<uint8_t>(Memory ? 0x18 : 0xda)};
  };

  auto LiftChecked = [&](const std::vector<uint8_t> &Bytes,
                         unsigned VectorBytes, MaskMode Mode, bool Memory) {
    Decoder Dec;
    if (!Dec.init(Arch::X64)) {
      ADD_FAILURE() << "x86-64 decoder initialization failed";
      return std::vector<LowOp>{};
    }
    DecodedInsn Insn{};
    if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn) !=
            static_cast<int>(Bytes.size()) ||
        !Insn.Raw || !Insn.Raw->detail) {
      ADD_FAILURE() << "EVEX unpack decode failed";
      return std::vector<LowOp>{};
    }

    const cs_x86 &X86 = Insn.Raw->detail->x86;
    const bool HasWriteMask = Mode != MaskMode::K0Unmasked;
    EXPECT_EQ(X86.op_count, HasWriteMask ? 4u : 3u);
    if (X86.op_count != (HasWriteMask ? 4u : 3u))
      return std::vector<LowOp>{};
    EXPECT_EQ(X86.operands[0].type, X86_OP_REG);
    EXPECT_EQ(X86.operands[0].size, VectorBytes);
    const unsigned LeftIndex = HasWriteMask ? 2 : 1;
    const unsigned RightIndex = HasWriteMask ? 3 : 2;
    EXPECT_EQ(X86.operands[LeftIndex].type, X86_OP_REG);
    EXPECT_EQ(X86.operands[LeftIndex].size, VectorBytes);
    EXPECT_EQ(X86.operands[RightIndex].type, Memory ? X86_OP_MEM : X86_OP_REG);
    EXPECT_EQ(X86.operands[RightIndex].size, VectorBytes);
    if (HasWriteMask) {
      EXPECT_EQ(X86.operands[1].type, X86_OP_REG);
      EXPECT_EQ(X86.operands[1].reg, X86_REG_K1);
      EXPECT_EQ(X86.operands[1].avx_zero_opmask, Mode == MaskMode::Zero);
    } else {
      for (unsigned I = 0; I < X86.op_count; ++I)
        EXPECT_FALSE(X86.operands[I].type == X86_OP_REG &&
                     X86.operands[I].reg >= X86_REG_K0 &&
                     X86.operands[I].reg <= X86_REG_K7);
    }

    std::vector<LowOp> Ops;
    Dec.liftToLow(Insn, Ops);
    return Ops;
  };

  auto Pattern = [](size_t Size, uint8_t Seed) {
    std::vector<uint8_t> Value(Size);
    for (size_t I = 0; I < Size; ++I)
      Value[I] = static_cast<uint8_t>(Seed + I * 13u);
    return Value;
  };

  auto IsActive = [](MaskMode Mode, uint64_t Mask, unsigned Element) {
    return Mode == MaskMode::K0Unmasked ||
           (Mask & (UINT64_C(1) << Element)) != 0;
  };

  auto ExpectedResult = [&](const Family &Current, unsigned VectorBytes,
                            MaskMode Mode, uint64_t Mask,
                            const std::vector<uint8_t> &Left,
                            const std::vector<uint8_t> &Right,
                            const std::vector<uint8_t> &OldDestination) {
    std::vector<uint8_t> Raw(VectorBytes);
    const unsigned ElementsPerLane = 16 / Current.ElementBytes;
    const unsigned FirstElement = Current.HighHalf ? ElementsPerLane / 2 : 0;
    for (unsigned LaneBase = 0; LaneBase < VectorBytes; LaneBase += 16) {
      const unsigned LaneBaseElement = LaneBase / Current.ElementBytes;
      for (unsigned I = 0; I < ElementsPerLane / 2; ++I) {
        for (unsigned Byte = 0; Byte < Current.ElementBytes; ++Byte) {
          Raw[(LaneBaseElement + I * 2) * Current.ElementBytes + Byte] =
              Left[(LaneBaseElement + FirstElement + I) * Current.ElementBytes +
                   Byte];
          Raw[(LaneBaseElement + I * 2 + 1) * Current.ElementBytes + Byte] =
              Right[(LaneBaseElement + FirstElement + I) *
                        Current.ElementBytes +
                    Byte];
        }
      }
    }

    std::vector<uint8_t> Expected = OldDestination;
    const unsigned ElementCount = VectorBytes / Current.ElementBytes;
    for (unsigned Element = 0; Element < ElementCount; ++Element) {
      const bool Active = IsActive(Mode, Mask, Element);
      for (unsigned Byte = 0; Byte < Current.ElementBytes; ++Byte) {
        const unsigned Offset = Element * Current.ElementBytes + Byte;
        if (Active)
          Expected[Offset] = Raw[Offset];
        else if (Mode == MaskMode::Zero)
          Expected[Offset] = 0;
      }
    }
    std::fill(Expected.begin() + VectorBytes, Expected.end(), 0);
    return Expected;
  };

  const RegInfo LeftState = mapCapstoneReg(X86_REG_ZMM1);
  const RegInfo RightState = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo DestinationState = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo MaskState = mapCapstoneReg(X86_REG_K1);
  const RegInfo K0State = mapCapstoneReg(X86_REG_K0);
  const RegInfo BaseState = mapCapstoneReg(X86_REG_RAX);
  const std::vector<uint8_t> LeftValue = Pattern(64, 0x11);
  const std::vector<uint8_t> RightValue = Pattern(64, 0x83);
  const std::vector<uint8_t> OldDestination = Pattern(64, 0xd5);
  constexpr uint64_t MaskValue = UINT64_C(0xb6db6db6db6db6db);
  constexpr uint64_t K0Value = UINT64_C(0x13579bdf2468ace0);

  // Capstone exposes both the scalar tuple width and broadcast decorator for
  // the legal D/Q forms. They must use the masked tuple-load path rather than
  // an eager vector load.
  for (unsigned FamilyIndex : {4u, 5u, 6u, 7u}) {
    const Family &Current = Families[FamilyIndex];
    const std::vector<uint8_t> Bytes =
        Encoding(Current, 64, MaskMode::Merge, true, true);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn),
              static_cast<int>(Bytes.size()));
    ASSERT_NE(Insn.Raw, nullptr);
    ASSERT_NE(Insn.Raw->detail, nullptr);
    const cs_x86 &X86 = Insn.Raw->detail->x86;
    ASSERT_EQ(X86.op_count, 4u);
    EXPECT_EQ(X86.operands[3].type, X86_OP_MEM);
    EXPECT_EQ(X86.operands[3].size, Current.ElementBytes);
    std::vector<LowOp> Ops;
    EXPECT_NO_THROW(Dec.liftToLow(Insn, Ops));
    EXPECT_FALSE(Ops.empty());
    EXPECT_TRUE(hasOnlyMappedRegisters(Ops));
    EXPECT_FALSE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
      return Op.Opcode == NdOp::LOAD;
    }));
    EXPECT_EQ(std::count_if(
                  Ops.begin(), Ops.end(),
                  [](const LowOp &Op) { return Op.Opcode == NdOp::INTRINSIC; }),
              1u);
  }

  // Register forms: all eight families, all EVEX vector lengths, aaa=0 and
  // both nonzero-mask policies.
  for (const Family &Current : Families) {
    for (unsigned VectorBytes : {16u, 32u, 64u}) {
      for (MaskMode Mode :
           {MaskMode::K0Unmasked, MaskMode::Merge, MaskMode::Zero}) {
        SCOPED_TRACE(Current.Name);
        SCOPED_TRACE(VectorBytes);
        SCOPED_TRACE(static_cast<unsigned>(Mode));
        const std::vector<LowOp> Ops =
            LiftChecked(Encoding(Current, VectorBytes, Mode, false),
                        VectorBytes, Mode, false);
        ASSERT_FALSE(Ops.empty());
        ASSERT_TRUE(hasOnlyMappedRegisters(Ops));
        EXPECT_EQ(std::count_if(Ops.begin(), Ops.end(),
                                [](const LowOp &Op) {
                                  return Op.Opcode == NdOp::INTRINSIC;
                                }),
                  0u);

        BinaryImage Image;
        Image.Arch = Arch::X64;
        Image.Bits = Bitness::Bits64;
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setRegisterBytes(LeftState.Offset, LeftValue);
        Emulator.setRegisterBytes(RightState.Offset, RightValue);
        Emulator.setRegisterBytes(DestinationState.Offset, OldDestination);
        Emulator.setRegister(MaskState.Offset, MaskValue);
        Emulator.setRegister(K0State.Offset, K0Value);
        ASSERT_EQ(Emulator.run(Ops), Ops.size());
        EXPECT_EQ(Emulator.getRegisterBytes(DestinationState.Offset),
                  ExpectedResult(Current, VectorBytes, Mode, MaskValue,
                                 LeftValue, RightValue, OldDestination));
        EXPECT_EQ(Emulator.getRegisterBytes(LeftState.Offset), LeftValue);
        EXPECT_EQ(Emulator.getRegisterBytes(RightState.Offset), RightValue);
        EXPECT_EQ(Emulator.getRegister(MaskState.Offset), MaskValue);
        EXPECT_EQ(Emulator.getRegister(K0State.Offset), K0Value);
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }

  // Full-width memory forms use an exact masked-load intrinsic rather than an
  // eager vector LOAD. The load log proves that only selected source elements
  // feeding active odd result elements are accessed.
  constexpr uint64_t MemoryBase = UINT64_C(0x4000);
  for (const Family &Current : Families) {
    for (unsigned VectorBytes : {16u, 32u, 64u}) {
      for (MaskMode Mode :
           {MaskMode::K0Unmasked, MaskMode::Merge, MaskMode::Zero}) {
        SCOPED_TRACE(Current.Name);
        SCOPED_TRACE(VectorBytes);
        SCOPED_TRACE(static_cast<unsigned>(Mode));
        const std::vector<LowOp> Ops =
            LiftChecked(Encoding(Current, VectorBytes, Mode, true), VectorBytes,
                        Mode, true);
        ASSERT_FALSE(Ops.empty());
        ASSERT_TRUE(hasOnlyMappedRegisters(Ops));
        EXPECT_FALSE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
          return Op.Opcode == NdOp::LOAD;
        }));
        EXPECT_EQ(std::count_if(Ops.begin(), Ops.end(),
                                [](const LowOp &Op) {
                                  return Op.Opcode == NdOp::INTRINSIC;
                                }),
                  1u);

        BinaryImage Image;
        Image.Arch = Arch::X64;
        Image.Bits = Bitness::Bits64;
        Segment Data;
        Data.VA = MemoryBase;
        Data.Size = VectorBytes;
        Data.Flags = SegmentFlags::Readable;
        Data.Data.assign(RightValue.begin(), RightValue.begin() + VectorBytes);
        Image.Segments.push_back(std::move(Data));

        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setLoadCollect(true);
        Emulator.setRegister(BaseState.Offset, MemoryBase);
        Emulator.setRegisterBytes(LeftState.Offset, LeftValue);
        Emulator.setRegisterBytes(DestinationState.Offset, OldDestination);
        Emulator.setRegister(MaskState.Offset, MaskValue);
        Emulator.setRegister(K0State.Offset, K0Value);
        ASSERT_EQ(Emulator.run(Ops), Ops.size());
        EXPECT_EQ(Emulator.getRegisterBytes(DestinationState.Offset),
                  ExpectedResult(Current, VectorBytes, Mode, MaskValue,
                                 LeftValue, RightValue, OldDestination));
        EXPECT_EQ(Emulator.getRegisterBytes(LeftState.Offset), LeftValue);
        EXPECT_EQ(Emulator.getRegister(MaskState.Offset), MaskValue);
        EXPECT_EQ(Emulator.getRegister(K0State.Offset), K0Value);
        EXPECT_FALSE(Emulator.skips().any());

        std::vector<LoadRecord> ExpectedLoads;
        const unsigned ElementsPerLane = 16 / Current.ElementBytes;
        const unsigned FirstElement =
            Current.HighHalf ? ElementsPerLane / 2 : 0;
        for (unsigned LaneBase = 0; LaneBase < VectorBytes; LaneBase += 16) {
          const unsigned LaneBaseElement = LaneBase / Current.ElementBytes;
          for (unsigned I = 0; I < ElementsPerLane / 2; ++I) {
            const unsigned ResultElement = LaneBaseElement + I * 2 + 1;
            if (!IsActive(Mode, MaskValue, ResultElement))
              continue;
            ExpectedLoads.push_back(
                {MemoryBase + (LaneBaseElement + FirstElement + I) *
                                  Current.ElementBytes,
                 static_cast<uint16_t>(Current.ElementBytes)});
          }
        }
        ASSERT_EQ(Emulator.getLoadRecords().size(), ExpectedLoads.size());
        for (size_t I = 0; I < ExpectedLoads.size(); ++I) {
          EXPECT_EQ(Emulator.getLoadRecords()[I].Addr, ExpectedLoads[I].Addr);
          EXPECT_EQ(Emulator.getLoadRecords()[I].Size, ExpectedLoads[I].Size);
        }
      }
    }
  }

  // A nonzero mask may activate only result elements sourced from the first
  // register. No memory element is then architecturally accessed, even at an
  // unmapped address.
  for (const Family &Current : Families) {
    const unsigned ElementCount = 64 / Current.ElementBytes;
    uint64_t RegisterOnlyMask = 0;
    for (unsigned Element = 0; Element < ElementCount; Element += 2)
      RegisterOnlyMask |= UINT64_C(1) << Element;
    const std::vector<LowOp> Ops =
        LiftChecked(Encoding(Current, 64, MaskMode::Merge, true), 64,
                    MaskMode::Merge, true);
    ASSERT_FALSE(Ops.empty());

    BinaryImage Empty;
    Empty.Arch = Arch::X64;
    Empty.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Empty);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(BaseState.Offset, UINT64_C(0xdead0000));
    Emulator.setRegisterBytes(LeftState.Offset, LeftValue);
    Emulator.setRegisterBytes(DestinationState.Offset, OldDestination);
    Emulator.setRegister(MaskState.Offset, RegisterOnlyMask);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(DestinationState.Offset),
              ExpectedResult(Current, 64, MaskMode::Merge, RegisterOnlyMask,
                             LeftValue, RightValue, OldDestination));
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());
  }

  // If any active source element faults, the architectural destination and
  // mask state remain untouched even after an earlier source element loaded.
  const Family &ByteLow = Families[0];
  const std::vector<LowOp> FaultOps = LiftChecked(
      Encoding(ByteLow, 16, MaskMode::Merge, true), 16, MaskMode::Merge, true);
  ASSERT_FALSE(FaultOps.empty());
  BinaryImage FaultImage;
  FaultImage.Arch = Arch::X64;
  FaultImage.Bits = Bitness::Bits64;
  Segment OneByte;
  OneByte.VA = MemoryBase;
  OneByte.Size = 1;
  OneByte.Flags = SegmentFlags::Readable;
  OneByte.Data = {RightValue[0]};
  FaultImage.Segments.push_back(std::move(OneByte));
  NdOpEmulator Fault(FaultImage);
  Fault.setStrictMode(true);
  Fault.setRegister(BaseState.Offset, MemoryBase);
  Fault.setRegisterBytes(LeftState.Offset, LeftValue);
  Fault.setRegisterBytes(DestinationState.Offset, OldDestination);
  Fault.setRegister(MaskState.Offset, UINT64_C(0x0a));
  EXPECT_LT(Fault.run(FaultOps), FaultOps.size());
  EXPECT_EQ(Fault.getRegisterBytes(DestinationState.Offset), OldDestination);
  EXPECT_EQ(Fault.getRegister(MaskState.Offset), UINT64_C(0x0a));
  EXPECT_FALSE(Fault.skips().any());
}

TEST(X86WideISAState, F16CConversionsHonorFormatsRoundingAndMxcsr) {
  const std::vector<LowOp> Widen = liftX64({0xc4, 0xe2, 0x79, 0x13, 0xc1});
  ASSERT_FALSE(Widen.empty());
  std::vector<uint8_t> HalfSource(64, 0);
  setIntegerLane(HalfSource, 0, 2, 0x3c00);
  setIntegerLane(HalfSource, 1, 2, 0xc000);
  setIntegerLane(HalfSource, 2, 2, 0x0001);
  setIntegerLane(HalfSource, 3, 2, 0x7c00);
  std::vector<uint8_t> ExpectedWiden(64, 0);
  setDwordLane(ExpectedWiden, 0, 0x3f800000U);
  setDwordLane(ExpectedWiden, 1, 0xc0000000U);
  setDwordLane(ExpectedWiden, 2, 0x33800000U);
  setDwordLane(ExpectedWiden, 3, 0x7f800000U);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator WidenEmulator(Image);
  WidenEmulator.setStrictMode(true);
  WidenEmulator.setMXCSR(0x1fc0); // DAZ must not flush an FP16 subnormal.
  WidenEmulator.setRegisterBytes(x86reg::XMM1, HalfSource);
  WidenEmulator.setRegisterBytes(x86reg::XMM0, std::vector<uint8_t>(64, 0xa5));
  ASSERT_EQ(WidenEmulator.run(Widen), Widen.size());
  EXPECT_EQ(WidenEmulator.getRegisterBytes(x86reg::XMM0), ExpectedWiden);
  EXPECT_EQ(WidenEmulator.getMXCSR(), 0x1fc0U);
  EXPECT_FALSE(WidenEmulator.skips().any());

  auto RunNarrow = [&](uint8_t Immediate, bool ExpectPrecision) {
    const std::vector<LowOp> Narrow =
        liftX64({0xc4, 0xe3, 0x79, 0x1d, 0xc8, Immediate});
    EXPECT_FALSE(Narrow.empty());
    std::vector<uint8_t> FloatSource(64, 0);
    setDwordLane(FloatSource, 0, 0x3f801000U);
    setDwordLane(FloatSource, 1, 0xbf801000U);
    setDwordLane(FloatSource, 2, 0x33800000U);
    setDwordLane(FloatSource, 3, 0x7f800000U);
    std::vector<uint8_t> Expected(64, 0);
    setIntegerLane(Expected, 0, 2, 0x3c01);
    setIntegerLane(Expected, 1, 2, 0xbc00);
    setIntegerLane(Expected, 2, 2, 0x0001);
    setIntegerLane(Expected, 3, 2, 0x7c00);

    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f80);
    Emulator.setRegisterBytes(x86reg::XMM1, FloatSource);
    Emulator.setRegisterBytes(x86reg::XMM0, std::vector<uint8_t>(64, 0xa5));
    EXPECT_EQ(Emulator.run(Narrow), Narrow.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM0), Expected);
    EXPECT_EQ((Emulator.getMXCSR() & 0x20U) != 0, ExpectPrecision);
    EXPECT_FALSE(Emulator.skips().any());
  };
  RunNarrow(0x02, true);
  // The high five immediate bits are architecturally ignored for F16C.
  RunNarrow(0x0a, true);

  // Imm8[2] selects MXCSR.RC.  Round-down keeps the positive midpoint at 1.0.
  {
    const std::vector<LowOp> Narrow =
        liftX64({0xc4, 0xe3, 0x79, 0x1d, 0xc8, 0x04});
    std::vector<uint8_t> Source(64, 0);
    setDwordLane(Source, 0, 0x3f801000U);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x3f80); // Default masks, round toward negative.
    Emulator.setRegisterBytes(x86reg::XMM1, Source);
    ASSERT_EQ(Emulator.run(Narrow), Narrow.size());
    const auto Result = Emulator.getRegisterBytes(x86reg::XMM0);
    ASSERT_TRUE(Result.has_value());
    EXPECT_EQ((*Result)[0], 0x00);
    EXPECT_EQ((*Result)[1], 0x3c);
    EXPECT_NE(Emulator.getMXCSR() & 0x20U, 0U);
  }

  // F16C uses DAZ for its FP32 input, but ignores FTZ for its FP16 output.
  const std::vector<LowOp> Narrow =
      liftX64({0xc4, 0xe3, 0x79, 0x1d, 0xc8, 0x00});
  {
    std::vector<uint8_t> Source(64, 0);
    setDwordLane(Source, 0, 0x00000001U);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1fc0); // Default masks with DAZ.
    Emulator.setRegisterBytes(x86reg::XMM1, Source);
    ASSERT_EQ(Emulator.run(Narrow), Narrow.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM0),
              std::vector<uint8_t>(64, 0));
    EXPECT_EQ(Emulator.getMXCSR() & 0x3fU, 0U);
  }
  {
    std::vector<uint8_t> Source(64, 0);
    setDwordLane(Source, 0, 0x00000001U);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f80);
    Emulator.setRegisterBytes(x86reg::XMM1, Source);
    ASSERT_EQ(Emulator.run(Narrow), Narrow.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM0),
              std::vector<uint8_t>(64, 0));
    EXPECT_EQ(Emulator.getMXCSR() & 0x3fU, 0x32U);
  }
  {
    std::vector<uint8_t> Source(64, 0);
    setDwordLane(Source, 0, 0x33800000U); // Exact minimum FP16 subnormal.
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x9f80); // Default masks with FTZ.
    Emulator.setRegisterBytes(x86reg::XMM1, Source);
    ASSERT_EQ(Emulator.run(Narrow), Narrow.size());
    const auto Result = Emulator.getRegisterBytes(x86reg::XMM0);
    ASSERT_TRUE(Result.has_value());
    EXPECT_EQ((*Result)[0], 0x01);
    EXPECT_EQ((*Result)[1], 0x00);
    EXPECT_EQ(Emulator.getMXCSR() & 0x3fU, 0x10U);
  }

  // A signaling NaN is quieted when invalid is masked; when it is unmasked,
  // the destination is not partially committed.
  {
    std::vector<uint8_t> Source(64, 0);
    setIntegerLane(Source, 0, 2, 0x7c01);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f80);
    Emulator.setRegisterBytes(x86reg::XMM1, Source);
    ASSERT_EQ(Emulator.run(Widen), Widen.size());
    const auto Result = Emulator.getRegisterBytes(x86reg::XMM0);
    ASSERT_TRUE(Result.has_value());
    EXPECT_EQ((*Result)[0], 0x00);
    EXPECT_EQ((*Result)[1], 0x20);
    EXPECT_EQ((*Result)[2], 0xc0);
    EXPECT_EQ((*Result)[3], 0x7f);
    EXPECT_EQ(Emulator.getMXCSR() & 0x3fU, 0x01U);
  }
  {
    std::vector<uint8_t> Source(64, 0);
    setIntegerLane(Source, 0, 2, 0x7c01);
    const std::vector<uint8_t> OldDestination(64, 0xa5);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00); // Invalid exception unmasked.
    Emulator.setRegisterBytes(x86reg::XMM1, Source);
    Emulator.setRegisterBytes(x86reg::XMM0, OldDestination);
    EXPECT_LT(Emulator.run(Widen), Widen.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM0), OldDestination);
    EXPECT_EQ(Emulator.getMXCSR() & 0x01U, 0x01U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // The 256-bit forms convert all eight lanes, not one aggregate integer.
  {
    const std::vector<LowOp> Widen256 = liftX64({0xc4, 0xe2, 0x7d, 0x13, 0xc1});
    std::vector<uint8_t> Source(64, 0);
    std::vector<uint8_t> Expected(64, 0);
    for (unsigned Lane = 0; Lane < 8; ++Lane) {
      setIntegerLane(Source, Lane, 2, static_cast<uint16_t>(0x3c00U + Lane));
      setDwordLane(Expected, Lane, 0x3f800000U + Lane * 0x2000U);
    }
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(x86reg::XMM1, Source);
    ASSERT_EQ(Emulator.run(Widen256), Widen256.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM0), Expected);
  }
}

TEST(X86WideISAState, EvexReciprocal14UsesArchitecturalApproximation) {
  enum class MaskMode : uint8_t { None, Merge, Zero };
  struct Family {
    uint8_t Opcode;
    bool F64;
    bool ReciprocalSqrt;
  };
  constexpr std::array<Family, 4> PackedFamilies = {
      Family{0x4c, false, false},
      Family{0x4c, true, false},
      Family{0x4e, false, true},
      Family{0x4e, true, true},
  };
  constexpr std::array<uint16_t, 3> VectorSizes = {16, 32, 64};
  constexpr std::array<MaskMode, 3> MaskModes = {
      MaskMode::None, MaskMode::Merge, MaskMode::Zero};
  constexpr uint64_t MaskBits = UINT64_C(0x5a5a);

  constexpr std::array<uint32_t, 8> Inputs32 = {
      0x40400000U, // 3.0
      0x40800000U, // 4.0
      0x00000001U, // minimum subnormal
      0x7f800001U, // signaling NaN
      0xbf800000U, // -1.0
      0x00000000U, // +0.0
      0x80000000U, // -0.0
      0x7f800000U, // +infinity
  };
  constexpr std::array<uint32_t, 8> Rcp32 = {
      0x3eaaaa80U, 0x3e800000U, 0x7f800000U, 0x7fc00001U,
      0xbf800000U, 0x7f800000U, 0xff800000U, 0x00000000U,
  };
  constexpr std::array<uint32_t, 8> Rsqrt32 = {
      0x3f13cc80U, 0x3f000000U, 0x64b50280U, 0x7fc00001U,
      0xffc00000U, 0x7f800000U, 0xff800000U, 0x00000000U,
  };
  constexpr std::array<uint64_t, 8> Inputs64 = {
      UINT64_C(0x4008000000000000), UINT64_C(0x4010000000000000),
      UINT64_C(0x0000000000000001), UINT64_C(0x7ff0000000000001),
      UINT64_C(0xbff0000000000000), UINT64_C(0x0000000000000000),
      UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000),
  };
  constexpr std::array<uint64_t, 8> Rcp64 = {
      UINT64_C(0x3fd5555000000000), UINT64_C(0x3fd0000000000000),
      UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff8000000000001),
      UINT64_C(0xbff0000000000000), UINT64_C(0x7ff0000000000000),
      UINT64_C(0xfff0000000000000), UINT64_C(0x0000000000000000),
  };
  constexpr std::array<uint64_t, 8> Rsqrt64 = {
      UINT64_C(0x3fe2799000000000), UINT64_C(0x3fe0000000000000),
      UINT64_C(0x6180000000000000), UINT64_C(0x7ff8000000000001),
      UINT64_C(0xfff8000000000000), UINT64_C(0x7ff0000000000000),
      UINT64_C(0xfff0000000000000), UINT64_C(0x0000000000000000),
  };

  auto EncodePacked = [](const Family &Current, uint16_t VectorSize,
                         MaskMode Mode) {
    const uint8_t Length =
        VectorSize == 16 ? 0 : (VectorSize == 32 ? 0x20 : 0x40);
    const uint8_t Mask = Mode == MaskMode::None ? 0 : 3;
    const uint8_t Zero = Mode == MaskMode::Zero ? 0x80 : 0;
    return std::vector<uint8_t>{
        0x62,
        0xf2,
        static_cast<uint8_t>((Current.F64 ? 0x80 : 0) | 0x7d),
        static_cast<uint8_t>(0x08 | Length | Mask | Zero),
        Current.Opcode,
        0xca};
  };

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  const RegInfo PackedDestination = mapCapstoneReg(X86_REG_ZMM1);
  const RegInfo PackedSource = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K3);
  std::vector<uint8_t> OldDestination(64, 0);
  for (unsigned Byte = 0; Byte < OldDestination.size(); ++Byte)
    OldDestination[Byte] = static_cast<uint8_t>(0x80U + Byte);

  for (const Family &Current : PackedFamilies) {
    const size_t ElementSize = Current.F64 ? 8 : 4;
    std::vector<uint8_t> Source(64, 0);
    for (unsigned Lane = 0; Lane < 64 / ElementSize; ++Lane) {
      if (Current.F64)
        setIntegerLane(Source, Lane, ElementSize,
                       Inputs64[Lane % Inputs64.size()]);
      else
        setDwordLane(Source, Lane, Inputs32[Lane % Inputs32.size()]);
    }

    for (uint16_t VectorSize : VectorSizes) {
      for (MaskMode Mode : MaskModes) {
        const std::vector<LowOp> Ops =
            liftX64(EncodePacked(Current, VectorSize, Mode));
        ASSERT_FALSE(Ops.empty());
        ASSERT_TRUE(hasOnlyMappedRegisters(Ops));
        std::vector<uint8_t> Expected(64, 0);
        for (unsigned Lane = 0; Lane < VectorSize / ElementSize; ++Lane) {
          const bool Active =
              Mode == MaskMode::None || ((MaskBits >> Lane) & 1U) != 0;
          uint64_t Value = 0;
          if (Active) {
            if (Current.F64) {
              Value = Current.ReciprocalSqrt ? Rsqrt64[Lane % Rsqrt64.size()]
                                             : Rcp64[Lane % Rcp64.size()];
            } else {
              Value = Current.ReciprocalSqrt ? Rsqrt32[Lane % Rsqrt32.size()]
                                             : Rcp32[Lane % Rcp32.size()];
            }
          } else if (Mode == MaskMode::Merge) {
            Value = getIntegerLane(OldDestination, Lane, ElementSize);
          }
          setIntegerLane(Expected, Lane, ElementSize, Value);
        }

        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setMXCSR(0x1f80);
        Emulator.setRegisterBytes(PackedSource.Offset, Source);
        Emulator.setRegisterBytes(PackedDestination.Offset, OldDestination);
        Emulator.setRegister(Mask.Offset, MaskBits);
        ASSERT_EQ(Emulator.run(Ops), Ops.size());
        EXPECT_EQ(Emulator.getRegisterBytes(PackedDestination.Offset),
                  Expected);
        EXPECT_EQ(Emulator.getRegister(Mask.Offset), MaskBits);
        EXPECT_EQ(Emulator.getMXCSR(), 0x1f80U);
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }

  // Scalar forms copy their upper elements from src1, while only element zero
  // is writemasked.  The remaining physical ZMM state is cleared.
  const RegInfo ScalarDestination = mapCapstoneReg(X86_REG_ZMM5);
  const RegInfo PassThrough = mapCapstoneReg(X86_REG_ZMM6);
  const RegInfo ScalarSource = mapCapstoneReg(X86_REG_ZMM7);
  std::vector<uint8_t> PassValue(64, 0);
  std::vector<uint8_t> ScalarOld(64, 0);
  for (unsigned Byte = 0; Byte < 16; ++Byte) {
    PassValue[Byte] = static_cast<uint8_t>(0x20U + Byte);
    ScalarOld[Byte] = static_cast<uint8_t>(0xa0U + Byte);
  }
  for (const Family &Current : PackedFamilies) {
    const size_t ElementSize = Current.F64 ? 8 : 4;
    const uint8_t Opcode = Current.ReciprocalSqrt ? 0x4f : 0x4d;
    std::vector<uint8_t> Source(64, 0);
    setIntegerLane(Source, 0, ElementSize,
                   Current.F64 ? Inputs64[0] : Inputs32[0]);
    const uint64_t ActiveResult =
        Current.F64 ? (Current.ReciprocalSqrt ? Rsqrt64[0] : Rcp64[0])
                    : (Current.ReciprocalSqrt ? Rsqrt32[0] : Rcp32[0]);
    for (MaskMode Mode : MaskModes) {
      const uint8_t EncodedMask = Mode == MaskMode::None ? 0 : 3;
      const uint8_t Zero = Mode == MaskMode::Zero ? 0x80 : 0;
      const std::vector<uint8_t> Bytes = {
          0x62,
          0xf2,
          static_cast<uint8_t>((Current.F64 ? 0x80 : 0) | 0x4d),
          static_cast<uint8_t>(0x08 | EncodedMask | Zero),
          Opcode,
          0xef};
      const std::vector<LowOp> Ops = liftX64(Bytes);
      ASSERT_FALSE(Ops.empty());
      std::vector<uint8_t> Expected(64, 0);
      std::copy_n(PassValue.begin(), 16, Expected.begin());
      const bool Active = Mode == MaskMode::None;
      const uint64_t LowValue =
          Active ? ActiveResult
                 : (Mode == MaskMode::Merge
                        ? getIntegerLane(ScalarOld, 0, ElementSize)
                        : 0);
      setIntegerLane(Expected, 0, ElementSize, LowValue);

      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setMXCSR(0x1f80);
      Emulator.setRegisterBytes(ScalarDestination.Offset, ScalarOld);
      Emulator.setRegisterBytes(PassThrough.Offset, PassValue);
      Emulator.setRegisterBytes(ScalarSource.Offset, Source);
      Emulator.setRegister(Mask.Offset, 0);
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      EXPECT_EQ(Emulator.getRegisterBytes(ScalarDestination.Offset), Expected);
      EXPECT_EQ(Emulator.getRegister(Mask.Offset), 0U);
      EXPECT_FALSE(Emulator.skips().any());
    }
  }

  // Exercise all four inverted R/R' combinations. P0 bit 7 supplies vector
  // register bit 3 while P0 bit 4 supplies bit 4; swapping their weights makes
  // xmm1/xmm17 and xmm9/xmm25 silently exchange identities.
  struct DestinationBank {
    uint8_t P0;
    x86_reg Register;
  };
  constexpr std::array<DestinationBank, 4> DestinationBanks = {
      DestinationBank{0xb2, X86_REG_ZMM1},
      DestinationBank{0x32, X86_REG_ZMM9},
      DestinationBank{0xa2, X86_REG_ZMM17},
      DestinationBank{0x22, X86_REG_ZMM25},
  };
  for (const DestinationBank &Bank : DestinationBanks) {
    const std::vector<LowOp> Ops =
        liftX64({0x62, Bank.P0, 0xed, 0x03, 0x4f, 0xcb});
    const RegInfo Destination = mapCapstoneReg(Bank.Register);
    const RegInfo FirstSource = mapCapstoneReg(X86_REG_ZMM18);
    const RegInfo SecondSource = mapCapstoneReg(X86_REG_ZMM19);
    std::vector<uint8_t> FirstValue(64, 0);
    std::vector<uint8_t> SecondValue(64, 0);
    std::vector<uint8_t> Expected(64, 0);
    for (unsigned Byte = 0; Byte < 16; ++Byte)
      FirstValue[Byte] = static_cast<uint8_t>(0x40U + Byte);
    std::copy_n(FirstValue.begin(), 16, Expected.begin());
    setIntegerLane(SecondValue, 0, 8, Inputs64[0]);
    setIntegerLane(Expected, 0, 8, Rsqrt64[0]);

    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f80);
    Emulator.setRegisterBytes(Destination.Offset,
                              std::vector<uint8_t>(64, 0xa5));
    Emulator.setRegisterBytes(FirstSource.Offset, FirstValue);
    Emulator.setRegisterBytes(SecondSource.Offset, SecondValue);
    Emulator.setRegister(Mask.Offset, 1);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), Expected);
    EXPECT_EQ(Emulator.getRegister(Mask.Offset), 1U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // DAZ changes a denormal rsqrt input to signed zero; FTZ flushes a
  // denormal reciprocal result. Neither instruction raises SIMD exceptions.
  {
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf2, 0x7d, 0x08, 0x4e, 0xca});
    std::vector<uint8_t> Source(64, 0);
    setDwordLane(Source, 0, 0x00000001U);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1fc0);
    Emulator.setRegisterBytes(PackedSource.Offset, Source);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(PackedDestination.Offset);
    ASSERT_TRUE(Result.has_value());
    EXPECT_EQ(getDwordLane(*Result, 0), 0x7f800000U);
    EXPECT_EQ(Emulator.getMXCSR(), 0x1fc0U);
  }
  {
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf2, 0x7d, 0x08, 0x4c, 0xca});
    std::vector<uint8_t> Source(64, 0);
    setDwordLane(Source, 0, 0x7f000000U);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x9f80);
    Emulator.setRegisterBytes(PackedSource.Offset, Source);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(PackedDestination.Offset);
    ASSERT_TRUE(Result.has_value());
    EXPECT_EQ(getDwordLane(*Result, 0), 0x00000000U);
    EXPECT_EQ(Emulator.getMXCSR(), 0x9f80U);
  }

  // A masked packed memory source reads only active elements before applying
  // the architectural 14-bit approximation.
  {
    constexpr uint64_t MemoryBase = UINT64_C(0x4800);
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf2, 0x7d, 0x4b, 0x4c, 0x08});
    ASSERT_FALSE(Ops.empty());
    std::vector<uint8_t> Memory(64, 0);
    setDwordLane(Memory, 0, Inputs32[0]);
    setDwordLane(Memory, 2, Inputs32[1]);
    BinaryImage MemoryImage;
    MemoryImage.Arch = Arch::X64;
    MemoryImage.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = MemoryBase;
    Data.Size = Memory.size();
    Data.Flags = SegmentFlags::Readable;
    Data.Data = Memory;
    MemoryImage.Segments.push_back(std::move(Data));

    std::vector<uint8_t> Expected = OldDestination;
    setDwordLane(Expected, 0, Rcp32[0]);
    setDwordLane(Expected, 2, Rcp32[1]);
    NdOpEmulator Emulator(MemoryImage);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setMXCSR(0x1f80);
    Emulator.setRegister(x86reg::RAX, MemoryBase);
    Emulator.setRegisterBytes(PackedDestination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, 5);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(PackedDestination.Offset), Expected);
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f80U);
    ASSERT_EQ(Emulator.getLoadRecords().size(), 2u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, MemoryBase);
    EXPECT_EQ(Emulator.getLoadRecords()[1].Addr, MemoryBase + 8);
    EXPECT_FALSE(Emulator.skips().any());
  }

  constexpr uint64_t MatrixBase = UINT64_C(0x5200);
  auto Approximate = [&](const Family &Current, unsigned Lane) {
    if (Current.F64)
      return Current.ReciprocalSqrt ? Rsqrt64[Lane % Rsqrt64.size()]
                                    : Rcp64[Lane % Rcp64.size()];
    return static_cast<uint64_t>(Current.ReciprocalSqrt
                                     ? Rsqrt32[Lane % Rsqrt32.size()]
                                     : Rcp32[Lane % Rcp32.size()]);
  };

  // Full and broadcast memory tuples are legal at every packed vector length.
  // The memory access set is exactly the set selected by the writemask.
  for (const Family &Current : PackedFamilies) {
    const size_t ElementSize = Current.F64 ? 8 : 4;
    for (uint16_t VectorSize : VectorSizes) {
      const unsigned LaneCount = VectorSize / ElementSize;
      for (bool BroadcastSource : {false, true}) {
        for (MaskMode Mode : MaskModes) {
          SCOPED_TRACE(testing::Message()
                       << "14 memory opcode="
                       << static_cast<unsigned>(Current.Opcode)
                       << " f64=" << Current.F64 << " bytes=" << VectorSize
                       << " broadcast=" << BroadcastSource
                       << " mask=" << static_cast<unsigned>(Mode));
          const uint8_t Length =
              VectorSize == 16 ? 0 : (VectorSize == 32 ? 0x20 : 0x40);
          const uint8_t EncodedMask = Mode == MaskMode::None ? 0 : 3;
          const uint8_t Zero = Mode == MaskMode::Zero ? 0x80 : 0;
          const std::vector<LowOp> Ops =
              liftX64({0x62, 0xf2,
                       static_cast<uint8_t>((Current.F64 ? 0x80 : 0) | 0x7d),
                       static_cast<uint8_t>(0x08 | Length |
                                            (BroadcastSource ? 0x10 : 0) |
                                            EncodedMask | Zero),
                       Current.Opcode, 0x08});
          ASSERT_FALSE(Ops.empty());
          ASSERT_TRUE(hasOnlyMappedRegisters(Ops));
          EXPECT_FALSE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
            return Op.Opcode == NdOp::LOAD;
          }));

          std::vector<uint8_t> Memory(VectorSize, 0);
          for (unsigned Lane = 0; Lane < LaneCount; ++Lane)
            setIntegerLane(Memory, Lane, ElementSize,
                           Current.F64 ? Inputs64[Lane % Inputs64.size()]
                                       : Inputs32[Lane % Inputs32.size()]);
          if (BroadcastSource)
            Memory.resize(ElementSize);
          BinaryImage MemoryImage;
          MemoryImage.Arch = Arch::X64;
          MemoryImage.Bits = Bitness::Bits64;
          Segment Data;
          Data.VA = MatrixBase;
          Data.Size = Memory.size();
          Data.Flags = SegmentFlags::Readable;
          Data.Data = Memory;
          MemoryImage.Segments.push_back(std::move(Data));

          std::vector<uint8_t> Expected(64, 0);
          std::vector<LoadRecord> ExpectedLoads;
          bool AnyActive = false;
          for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
            const bool Active =
                Mode == MaskMode::None || ((MaskBits >> Lane) & 1U) != 0;
            AnyActive |= Active;
            uint64_t Value = 0;
            if (Active) {
              Value = Approximate(Current, BroadcastSource ? 0 : Lane);
              if (!BroadcastSource)
                ExpectedLoads.push_back({MatrixBase + Lane * ElementSize,
                                         static_cast<uint16_t>(ElementSize)});
            } else if (Mode == MaskMode::Merge) {
              Value = getIntegerLane(OldDestination, Lane, ElementSize);
            }
            setIntegerLane(Expected, Lane, ElementSize, Value);
          }
          if (BroadcastSource && AnyActive)
            ExpectedLoads.push_back(
                {MatrixBase, static_cast<uint16_t>(ElementSize)});

          NdOpEmulator Emulator(MemoryImage);
          Emulator.setStrictMode(true);
          Emulator.setLoadCollect(true);
          Emulator.setMXCSR(0x1f80);
          Emulator.setRegister(x86reg::RAX, MatrixBase);
          Emulator.setRegisterBytes(PackedDestination.Offset, OldDestination);
          Emulator.setRegister(Mask.Offset, MaskBits);
          ASSERT_EQ(Emulator.run(Ops), Ops.size());
          EXPECT_EQ(Emulator.getRegisterBytes(PackedDestination.Offset),
                    Expected);
          EXPECT_EQ(Emulator.getMXCSR(), 0x1f80U);
          ASSERT_EQ(Emulator.getLoadRecords().size(), ExpectedLoads.size());
          for (size_t Index = 0; Index < ExpectedLoads.size(); ++Index) {
            EXPECT_EQ(Emulator.getLoadRecords()[Index].Addr,
                      ExpectedLoads[Index].Addr);
            EXPECT_EQ(Emulator.getLoadRecords()[Index].Size,
                      ExpectedLoads[Index].Size);
          }
          EXPECT_FALSE(Emulator.skips().any());
        }
      }
    }
  }

  // Scalar full-memory forms are LLIG for LL=0/1/2. K[0] alone guards the
  // load, while src1 supplies the remaining elements of the XMM result.
  for (const Family &Current : PackedFamilies) {
    const size_t ElementSize = Current.F64 ? 8 : 4;
    const uint8_t Opcode = Current.ReciprocalSqrt ? 0x4f : 0x4d;
    std::vector<uint8_t> Memory(ElementSize, 0);
    setIntegerLane(Memory, 0, ElementSize,
                   Current.F64 ? Inputs64[0] : Inputs32[0]);
    for (uint8_t Length : {uint8_t{0}, uint8_t{0x20}, uint8_t{0x40}}) {
      for (MaskMode Mode : MaskModes) {
        const uint8_t EncodedMask = Mode == MaskMode::None ? 0 : 3;
        const uint8_t Zero = Mode == MaskMode::Zero ? 0x80 : 0;
        const std::vector<LowOp> Ops = liftX64(
            {0x62, 0xf2, static_cast<uint8_t>((Current.F64 ? 0x80 : 0) | 0x4d),
             static_cast<uint8_t>(0x08 | Length | EncodedMask | Zero), Opcode,
             0x28});
        ASSERT_FALSE(Ops.empty());

        BinaryImage MemoryImage;
        MemoryImage.Arch = Arch::X64;
        MemoryImage.Bits = Bitness::Bits64;
        Segment Data;
        Data.VA = MatrixBase;
        Data.Size = Memory.size();
        Data.Flags = SegmentFlags::Readable;
        Data.Data = Memory;
        MemoryImage.Segments.push_back(std::move(Data));

        const bool Active = Mode == MaskMode::None;
        std::vector<uint8_t> Expected(64, 0);
        std::copy_n(PassValue.begin(), 16, Expected.begin());
        setIntegerLane(Expected, 0, ElementSize,
                       Active ? Approximate(Current, 0)
                              : (Mode == MaskMode::Merge
                                     ? getIntegerLane(ScalarOld, 0, ElementSize)
                                     : 0));
        NdOpEmulator Emulator(MemoryImage);
        Emulator.setStrictMode(true);
        Emulator.setLoadCollect(true);
        Emulator.setMXCSR(0x1f80);
        Emulator.setRegister(x86reg::RAX, MatrixBase);
        Emulator.setRegisterBytes(ScalarDestination.Offset, ScalarOld);
        Emulator.setRegisterBytes(PassThrough.Offset, PassValue);
        Emulator.setRegister(Mask.Offset, 0);
        ASSERT_EQ(Emulator.run(Ops), Ops.size());
        EXPECT_EQ(Emulator.getRegisterBytes(ScalarDestination.Offset),
                  Expected);
        EXPECT_EQ(Emulator.getMXCSR(), 0x1f80U);
        ASSERT_EQ(Emulator.getLoadRecords().size(), Active ? 1u : 0u);
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }

  // All-zero masks suppress an unmapped broadcast. A later active-lane fault
  // commits neither an earlier staged load nor any destination/MXCSR state.
  {
    const std::vector<LowOp> Suppressed =
        liftX64({0x62, 0xf2, 0x7d, 0x5b, 0x4c, 0x08});
    BinaryImage EmptyImage;
    EmptyImage.Arch = Arch::X64;
    EmptyImage.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(EmptyImage);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setMXCSR(0x1f80);
    Emulator.setRegister(x86reg::RAX, UINT64_C(0xdead0000));
    Emulator.setRegisterBytes(PackedDestination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, 0);
    ASSERT_EQ(Emulator.run(Suppressed), Suppressed.size());
    EXPECT_EQ(Emulator.getRegisterBytes(PackedDestination.Offset),
              OldDestination);
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f80U);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
  }
  {
    const std::vector<LowOp> FaultOps =
        liftX64({0x62, 0xf2, 0x7d, 0x4b, 0x4c, 0x08});
    BinaryImage FaultImage;
    FaultImage.Arch = Arch::X64;
    FaultImage.Bits = Bitness::Bits64;
    Segment FirstLane;
    FirstLane.VA = MatrixBase;
    FirstLane.Size = 4;
    FirstLane.Flags = SegmentFlags::Readable;
    FirstLane.Data.resize(4);
    setDwordLane(FirstLane.Data, 0, Inputs32[0]);
    FaultImage.Segments.push_back(std::move(FirstLane));
    NdOpEmulator Emulator(FaultImage);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setMXCSR(0x1f80);
    Emulator.setRegister(x86reg::RAX, MatrixBase);
    Emulator.setRegisterBytes(PackedDestination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, 3);
    EXPECT_LT(Emulator.run(FaultOps), FaultOps.size());
    EXPECT_EQ(Emulator.getRegisterBytes(PackedDestination.Offset),
              OldDestination);
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f80U);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
  }

  // Capstone and the lifter both honor scalar LLIG and reject reserved b/LL.
  for (uint8_t Length : {uint8_t{0}, uint8_t{0x20}, uint8_t{0x40}})
    EXPECT_FALSE(liftX64({0x62, 0xf2, 0x4d, static_cast<uint8_t>(0x08 | Length),
                          0x4d, 0xef})
                     .empty());
  auto ExpectDecodeRejected = [](const std::vector<uint8_t> &Bytes) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    EXPECT_NE(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn),
              static_cast<int>(Bytes.size()));
  };
  ExpectDecodeRejected({0x62, 0xf2, 0x7d, 0x68, 0x4c, 0x08});
  ExpectDecodeRejected({0x62, 0xf2, 0x4d, 0x18, 0x4d, 0x28});
  ExpectDecodeRejected({0x62, 0xf2, 0x4d, 0x68, 0x4d, 0x28});
}

TEST(X86WideISAState, EvexApproximation28AndExp2UseReferenceSemantics) {
  enum class Operation : uint8_t { Reciprocal, ReciprocalSqrt, Exp2 };
  enum class MaskMode : uint8_t { None, Merge, Zero };
  struct Family {
    uint8_t Opcode;
    bool F64;
    Operation Op;
  };
  constexpr std::array<Family, 6> Families = {
      Family{0xca, false, Operation::Reciprocal},
      Family{0xca, true, Operation::Reciprocal},
      Family{0xcc, false, Operation::ReciprocalSqrt},
      Family{0xcc, true, Operation::ReciprocalSqrt},
      Family{0xc8, false, Operation::Exp2},
      Family{0xc8, true, Operation::Exp2},
  };
  constexpr std::array<MaskMode, 3> MaskModes = {
      MaskMode::None, MaskMode::Merge, MaskMode::Zero};
  constexpr uint64_t MaskBits = UINT64_C(0x5a5a);
  constexpr std::array<uint32_t, 8> Inputs32 = {
      0x40400000U, 0x40800000U, 0x00000001U, 0x7f800001U,
      0xc0000001U, 0x00000000U, 0x80000000U, 0x7f800000U,
  };
  constexpr std::array<uint64_t, 8> Inputs64 = {
      UINT64_C(0x4008000000000000), UINT64_C(0x4010000000000000),
      UINT64_C(0x0000000000000001), UINT64_C(0x7ff0000000000001),
      UINT64_C(0xc000000000000001), UINT64_C(0x0000000000000000),
      UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000),
  };
  constexpr std::array<uint32_t, 8> Rcp32 = {
      0x3eaaaaabU, 0x3e800000U, 0x7f800000U, 0x7fc00001U,
      0xbefffffeU, 0x7f800000U, 0xff800000U, 0x00000000U,
  };
  constexpr std::array<uint32_t, 8> Rsqrt32 = {
      0x3f13cd3aU, 0x3f000000U, 0x7f800000U, 0x7fc00001U,
      0xffc00000U, 0x7f800000U, 0xff800000U, 0x00000000U,
  };
  constexpr std::array<uint32_t, 8> Exp32 = {
      0x41000000U, 0x41800000U, 0x3f800000U, 0x7fc00001U,
      0x3e7ffffdU, 0x3f800000U, 0x3f800000U, 0x7f800000U,
  };
  constexpr std::array<uint64_t, 8> Rcp64 = {
      UINT64_C(0x3fd5555555100000), UINT64_C(0x3fd0000000000000),
      UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff8000000000001),
      UINT64_C(0xbfdfffffff000000), UINT64_C(0x7ff0000000000000),
      UINT64_C(0xfff0000000000000), UINT64_C(0x0000000000000000),
  };
  constexpr std::array<uint64_t, 8> Rsqrt64 = {
      UINT64_C(0x3fe279a745800000), UINT64_C(0x3fe0000000000000),
      UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff8000000000001),
      UINT64_C(0xfff8000000000000), UINT64_C(0x7ff0000000000000),
      UINT64_C(0xfff0000000000000), UINT64_C(0x0000000000000000),
  };
  constexpr std::array<uint64_t, 8> Exp64 = {
      UINT64_C(0x4020000000000000), UINT64_C(0x4030000000000000),
      UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff8000000000001),
      UINT64_C(0x3fd0000000000000), UINT64_C(0x3ff0000000000000),
      UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff0000000000000),
  };
  constexpr std::array<uint8_t, 8> RcpFlags = {0, 0, 4, 1, 0, 4, 4, 0};
  constexpr std::array<uint8_t, 8> RsqrtFlags = {0, 0, 4, 1, 1, 4, 4, 0};
  constexpr std::array<uint8_t, 8> ExpFlags = {0, 0, 0, 1, 0, 0, 0, 0};

  auto ExpectedValue = [&](const Family &Current, unsigned Lane) {
    const unsigned Index = Lane % Inputs32.size();
    if (Current.F64) {
      if (Current.Op == Operation::Reciprocal)
        return Rcp64[Index];
      if (Current.Op == Operation::ReciprocalSqrt)
        return Rsqrt64[Index];
      return Exp64[Index];
    }
    if (Current.Op == Operation::Reciprocal)
      return static_cast<uint64_t>(Rcp32[Index]);
    if (Current.Op == Operation::ReciprocalSqrt)
      return static_cast<uint64_t>(Rsqrt32[Index]);
    return static_cast<uint64_t>(Exp32[Index]);
  };
  auto ExpectedFlags = [&](const Family &Current, unsigned Lane) {
    const unsigned Index = Lane % RcpFlags.size();
    if (Current.Op == Operation::Reciprocal)
      return RcpFlags[Index];
    if (Current.Op == Operation::ReciprocalSqrt)
      return RsqrtFlags[Index];
    return ExpFlags[Index];
  };

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM1);
  const RegInfo SourceState = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K3);
  std::vector<uint8_t> OldDestination(64, 0);
  for (unsigned Byte = 0; Byte < OldDestination.size(); ++Byte)
    OldDestination[Byte] = static_cast<uint8_t>(0x90U + Byte);

  for (const Family &Current : Families) {
    const size_t ElementSize = Current.F64 ? 8 : 4;
    const unsigned LaneCount = 64 / ElementSize;
    std::vector<uint8_t> Source(64, 0);
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane)
      setIntegerLane(Source, Lane, ElementSize,
                     Current.F64 ? Inputs64[Lane % Inputs64.size()]
                                 : Inputs32[Lane % Inputs32.size()]);

    for (bool SuppressExceptions : {false, true}) {
      for (MaskMode Mode : MaskModes) {
        const uint8_t EncodedMask = Mode == MaskMode::None ? 0 : 3;
        const uint8_t Zero = Mode == MaskMode::Zero ? 0x80 : 0;
        const uint8_t LengthOrSae = SuppressExceptions ? 0x10 : 0x40;
        const std::vector<uint8_t> Bytes = {
            0x62,
            0xf2,
            static_cast<uint8_t>((Current.F64 ? 0x80 : 0) | 0x7d),
            static_cast<uint8_t>(0x08 | LengthOrSae | EncodedMask | Zero),
            Current.Opcode,
            0xca};
        const std::vector<LowOp> Ops = liftX64(Bytes);
        ASSERT_FALSE(Ops.empty());
        ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

        std::vector<uint8_t> Expected(64, 0);
        uint32_t Raised = 0;
        for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
          const bool Active =
              Mode == MaskMode::None || ((MaskBits >> Lane) & 1U) != 0;
          uint64_t Value = 0;
          if (Active) {
            Value = ExpectedValue(Current, Lane);
            Raised |= ExpectedFlags(Current, Lane);
          } else if (Mode == MaskMode::Merge) {
            Value = getIntegerLane(OldDestination, Lane, ElementSize);
          }
          setIntegerLane(Expected, Lane, ElementSize, Value);
        }

        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setMXCSR(0x1f80);
        Emulator.setRegisterBytes(SourceState.Offset, Source);
        Emulator.setRegisterBytes(Destination.Offset, OldDestination);
        Emulator.setRegister(Mask.Offset, MaskBits);
        ASSERT_EQ(Emulator.run(Ops), Ops.size());
        EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), Expected);
        EXPECT_EQ(Emulator.getRegister(Mask.Offset), MaskBits);
        EXPECT_EQ(Emulator.getMXCSR(),
                  SuppressExceptions ? 0x1f80U : (0x1f80U | Raised));
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }

  // Scalar 28-bit forms copy src1[127:element-size] and mask only lane zero.
  const RegInfo ScalarDestination = mapCapstoneReg(X86_REG_ZMM5);
  const RegInfo PassThrough = mapCapstoneReg(X86_REG_ZMM6);
  const RegInfo ScalarSource = mapCapstoneReg(X86_REG_ZMM7);
  std::vector<uint8_t> FirstValue(64, 0);
  std::vector<uint8_t> OldScalar(64, 0);
  for (unsigned Byte = 0; Byte < 16; ++Byte) {
    FirstValue[Byte] = static_cast<uint8_t>(0x30U + Byte);
    OldScalar[Byte] = static_cast<uint8_t>(0xb0U + Byte);
  }
  for (const Family &Current : Families) {
    if (Current.Op == Operation::Exp2)
      continue;
    const size_t ElementSize = Current.F64 ? 8 : 4;
    const uint8_t Opcode = Current.Op == Operation::Reciprocal ? 0xcb : 0xcd;
    std::vector<uint8_t> Source(64, 0);
    setIntegerLane(Source, 0, ElementSize,
                   Current.F64 ? Inputs64[0] : Inputs32[0]);
    for (bool SuppressExceptions : {false, true}) {
      for (MaskMode Mode : MaskModes) {
        const uint8_t EncodedMask = Mode == MaskMode::None ? 0 : 3;
        const uint8_t Zero = Mode == MaskMode::Zero ? 0x80 : 0;
        const std::vector<uint8_t> Bytes = {
            0x62,
            0xf2,
            static_cast<uint8_t>((Current.F64 ? 0x80 : 0) | 0x4d),
            static_cast<uint8_t>(0x08 | (SuppressExceptions ? 0x10 : 0) |
                                 EncodedMask | Zero),
            Opcode,
            0xef};
        const std::vector<LowOp> Ops = liftX64(Bytes);
        ASSERT_FALSE(Ops.empty());
        std::vector<uint8_t> Expected(64, 0);
        std::copy_n(FirstValue.begin(), 16, Expected.begin());
        const uint64_t LowValue =
            Mode == MaskMode::None
                ? ExpectedValue(Current, 0)
                : (Mode == MaskMode::Merge
                       ? getIntegerLane(OldScalar, 0, ElementSize)
                       : 0);
        setIntegerLane(Expected, 0, ElementSize, LowValue);

        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setMXCSR(0x1f80);
        Emulator.setRegisterBytes(ScalarDestination.Offset, OldScalar);
        Emulator.setRegisterBytes(PassThrough.Offset, FirstValue);
        Emulator.setRegisterBytes(ScalarSource.Offset, Source);
        Emulator.setRegister(Mask.Offset, 0);
        ASSERT_EQ(Emulator.run(Ops), Ops.size());
        EXPECT_EQ(Emulator.getRegisterBytes(ScalarDestination.Offset),
                  Expected);
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }

  // An unmasked #I updates MXCSR but commits no destination. SAE computes the
  // same quiet NaN while suppressing both the flag and the trap.
  std::vector<uint8_t> ExceptionalSource(64, 0);
  setDwordLane(ExceptionalSource, 0, 0x7f800001U);
  for (unsigned Lane = 1; Lane < 16; ++Lane)
    setDwordLane(ExceptionalSource, Lane, Inputs32[0]);
  {
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf2, 0x7d, 0x48, 0xca, 0xca});
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00);
    Emulator.setRegisterBytes(SourceState.Offset, ExceptionalSource);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    EXPECT_LT(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), OldDestination);
    EXPECT_EQ(Emulator.getMXCSR() & 1U, 1U);
    EXPECT_FALSE(Emulator.skips().any());
  }
  {
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf2, 0x7d, 0x18, 0xca, 0xca});
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00);
    Emulator.setRegisterBytes(SourceState.Offset, ExceptionalSource);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result.has_value());
    EXPECT_EQ(getDwordLane(*Result, 0), 0x7fc00001U);
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f00U);
  }

  // A masked full-width memory source reads only active elements.  The
  // approximation and architectural destination commit after every required
  // source element has loaded successfully.
  {
    constexpr uint64_t MemoryBase = UINT64_C(0x5000);
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf2, 0x7d, 0x4b, 0xca, 0x08});
    ASSERT_FALSE(Ops.empty());
    ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

    std::vector<uint8_t> Memory(64, 0);
    setDwordLane(Memory, 0, Inputs32[0]);
    setDwordLane(Memory, 2, Inputs32[1]);
    BinaryImage MemoryImage;
    MemoryImage.Arch = Arch::X64;
    MemoryImage.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = MemoryBase;
    Data.Size = Memory.size();
    Data.Flags = SegmentFlags::Readable;
    Data.Data = Memory;
    MemoryImage.Segments.push_back(std::move(Data));

    constexpr uint64_t ActiveLanes = UINT64_C(0x5);
    std::vector<uint8_t> Expected = OldDestination;
    setDwordLane(Expected, 0, Rcp32[0]);
    setDwordLane(Expected, 2, Rcp32[1]);
    NdOpEmulator Emulator(MemoryImage);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setMXCSR(0x1f80);
    Emulator.setRegister(x86reg::RAX, MemoryBase);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, ActiveLanes);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), Expected);
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f80U);
    ASSERT_EQ(Emulator.getLoadRecords().size(), 2u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, MemoryBase);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Size, 4u);
    EXPECT_EQ(Emulator.getLoadRecords()[1].Addr, MemoryBase + 8);
    EXPECT_EQ(Emulator.getLoadRecords()[1].Size, 4u);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // All packed 28-bit and EXP2 memory families share the same exact memory
  // topology: a full ZMM tuple or one broadcast element.  Sparse masks guard
  // the source lanes before the approximation is evaluated.
  constexpr uint64_t MatrixBase = UINT64_C(0x6000);
  for (const Family &Current : Families) {
    const size_t ElementSize = Current.F64 ? 8 : 4;
    const unsigned LaneCount = 64 / ElementSize;
    std::vector<uint8_t> FullSource(64, 0);
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane)
      setIntegerLane(FullSource, Lane, ElementSize,
                     Current.F64 ? Inputs64[Lane % Inputs64.size()]
                                 : Inputs32[Lane % Inputs32.size()]);

    for (bool BroadcastSource : {false, true}) {
      for (MaskMode Mode : MaskModes) {
        SCOPED_TRACE(
            testing::Message()
            << "memory opcode=" << static_cast<unsigned>(Current.Opcode)
            << " f64=" << Current.F64 << " broadcast=" << BroadcastSource
            << " mask=" << static_cast<unsigned>(Mode));
        const uint8_t EncodedMask = Mode == MaskMode::None ? 0 : 3;
        const uint8_t Zero = Mode == MaskMode::Zero ? 0x80 : 0;
        const uint8_t P2 = static_cast<uint8_t>(
            (BroadcastSource ? 0x58 : 0x48) | EncodedMask | Zero);
        const std::vector<LowOp> Ops = liftX64(
            {0x62, 0xf2, static_cast<uint8_t>((Current.F64 ? 0x80 : 0) | 0x7d),
             P2, Current.Opcode, 0x08});
        ASSERT_FALSE(Ops.empty());
        ASSERT_TRUE(hasOnlyMappedRegisters(Ops));
        EXPECT_FALSE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
          return Op.Opcode == NdOp::LOAD;
        }));

        std::vector<uint8_t> Memory = FullSource;
        if (BroadcastSource)
          Memory.resize(ElementSize);
        BinaryImage MemoryImage;
        MemoryImage.Arch = Arch::X64;
        MemoryImage.Bits = Bitness::Bits64;
        Segment Data;
        Data.VA = MatrixBase;
        Data.Size = Memory.size();
        Data.Flags = SegmentFlags::Readable;
        Data.Data = Memory;
        MemoryImage.Segments.push_back(std::move(Data));

        std::vector<uint8_t> Expected(64, 0);
        uint32_t Raised = 0;
        bool AnyActive = false;
        std::vector<LoadRecord> ExpectedLoads;
        for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
          const bool Active =
              Mode == MaskMode::None || ((MaskBits >> Lane) & 1U) != 0;
          AnyActive |= Active;
          uint64_t Value = 0;
          if (Active) {
            const unsigned SourceLane = BroadcastSource ? 0 : Lane;
            Value = ExpectedValue(Current, SourceLane);
            Raised |= ExpectedFlags(Current, SourceLane);
            if (!BroadcastSource)
              ExpectedLoads.push_back({MatrixBase + Lane * ElementSize,
                                       static_cast<uint16_t>(ElementSize)});
          } else if (Mode == MaskMode::Merge) {
            Value = getIntegerLane(OldDestination, Lane, ElementSize);
          }
          setIntegerLane(Expected, Lane, ElementSize, Value);
        }
        if (BroadcastSource && AnyActive)
          ExpectedLoads.push_back(
              {MatrixBase, static_cast<uint16_t>(ElementSize)});

        NdOpEmulator Emulator(MemoryImage);
        Emulator.setStrictMode(true);
        Emulator.setLoadCollect(true);
        Emulator.setMXCSR(0x1f80);
        Emulator.setRegister(x86reg::RAX, MatrixBase);
        Emulator.setRegisterBytes(Destination.Offset, OldDestination);
        Emulator.setRegister(Mask.Offset, MaskBits);
        ASSERT_EQ(Emulator.run(Ops), Ops.size());
        EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), Expected);
        EXPECT_EQ(Emulator.getMXCSR(), 0x1f80U | Raised);
        ASSERT_EQ(Emulator.getLoadRecords().size(), ExpectedLoads.size());
        for (size_t Index = 0; Index < ExpectedLoads.size(); ++Index) {
          EXPECT_EQ(Emulator.getLoadRecords()[Index].Addr,
                    ExpectedLoads[Index].Addr);
          EXPECT_EQ(Emulator.getLoadRecords()[Index].Size,
                    ExpectedLoads[Index].Size);
        }
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }

  // Scalar memory encodings are LLIG for LL=0/1/2, but b=1 and LL=3 are
  // reserved.  Only K[0] controls the load and the low destination element;
  // the remaining 128-bit elements come from src1.
  for (const Family &Current : Families) {
    if (Current.Op == Operation::Exp2)
      continue;
    const size_t ElementSize = Current.F64 ? 8 : 4;
    const uint8_t Opcode = Current.Op == Operation::Reciprocal ? 0xcb : 0xcd;
    std::vector<uint8_t> ScalarMemory(ElementSize, 0);
    setIntegerLane(ScalarMemory, 0, ElementSize,
                   Current.F64 ? Inputs64[0] : Inputs32[0]);
    for (uint8_t Length : {uint8_t{0}, uint8_t{0x20}, uint8_t{0x40}}) {
      for (MaskMode Mode : MaskModes) {
        SCOPED_TRACE(testing::Message()
                     << "scalar memory opcode=" << static_cast<unsigned>(Opcode)
                     << " f64=" << Current.F64
                     << " LL=" << static_cast<unsigned>(Length)
                     << " mask=" << static_cast<unsigned>(Mode));
        const uint8_t EncodedMask = Mode == MaskMode::None ? 0 : 3;
        const uint8_t Zero = Mode == MaskMode::Zero ? 0x80 : 0;
        const std::vector<LowOp> Ops = liftX64(
            {0x62, 0xf2, static_cast<uint8_t>((Current.F64 ? 0x80 : 0) | 0x4d),
             static_cast<uint8_t>(0x08 | Length | EncodedMask | Zero), Opcode,
             0x28});
        ASSERT_FALSE(Ops.empty());

        BinaryImage MemoryImage;
        MemoryImage.Arch = Arch::X64;
        MemoryImage.Bits = Bitness::Bits64;
        Segment Data;
        Data.VA = MatrixBase;
        Data.Size = ScalarMemory.size();
        Data.Flags = SegmentFlags::Readable;
        Data.Data = ScalarMemory;
        MemoryImage.Segments.push_back(std::move(Data));

        const bool Active = Mode == MaskMode::None;
        std::vector<uint8_t> Expected(64, 0);
        std::copy_n(FirstValue.begin(), 16, Expected.begin());
        setIntegerLane(Expected, 0, ElementSize,
                       Active ? ExpectedValue(Current, 0)
                              : (Mode == MaskMode::Merge
                                     ? getIntegerLane(OldScalar, 0, ElementSize)
                                     : 0));

        NdOpEmulator Emulator(MemoryImage);
        Emulator.setStrictMode(true);
        Emulator.setLoadCollect(true);
        Emulator.setMXCSR(0x1f80);
        Emulator.setRegister(x86reg::RAX, MatrixBase);
        Emulator.setRegisterBytes(ScalarDestination.Offset, OldScalar);
        Emulator.setRegisterBytes(PassThrough.Offset, FirstValue);
        Emulator.setRegister(Mask.Offset, 0);
        ASSERT_EQ(Emulator.run(Ops), Ops.size());
        EXPECT_EQ(Emulator.getRegisterBytes(ScalarDestination.Offset),
                  Expected);
        EXPECT_EQ(Emulator.getMXCSR(), 0x1f80U);
        ASSERT_EQ(Emulator.getLoadRecords().size(), Active ? 1u : 0u);
        if (Active) {
          EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, MatrixBase);
          EXPECT_EQ(Emulator.getLoadRecords()[0].Size, ElementSize);
        }
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }

  // An all-zero writemask suppresses even an unmapped broadcast source.
  {
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf2, 0x7d, 0x5b, 0xca, 0x08});
    BinaryImage EmptyImage;
    EmptyImage.Arch = Arch::X64;
    EmptyImage.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(EmptyImage);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setMXCSR(0x1f80);
    Emulator.setRegister(x86reg::RAX, UINT64_C(0xdead0000));
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, 0);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), OldDestination);
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f80U);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());
  }

  // A later active-lane fault discards earlier staged loads and happens before
  // either approximation exceptions or architectural destination writeback.
  {
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf2, 0x7d, 0x4b, 0xca, 0x08});
    BinaryImage FaultImage;
    FaultImage.Arch = Arch::X64;
    FaultImage.Bits = Bitness::Bits64;
    Segment FirstLane;
    FirstLane.VA = MatrixBase;
    FirstLane.Size = 4;
    FirstLane.Flags = SegmentFlags::Readable;
    FirstLane.Data.resize(4);
    setDwordLane(FirstLane.Data, 0, 0x7f800001U);
    FaultImage.Segments.push_back(std::move(FirstLane));

    NdOpEmulator Emulator(FaultImage);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setMXCSR(0x1f00); // Invalid exception unmasked.
    Emulator.setRegister(x86reg::RAX, MatrixBase);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegister(Mask.Offset, 3);
    EXPECT_LT(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), OldDestination);
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f00U);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Full-tuple and broadcast compressed displacements scale by their
  // architectural tuple sizes (64 and 4 bytes respectively).
  for (bool BroadcastSource : {false, true}) {
    const uint64_t EffectiveAddress = MatrixBase + (BroadcastSource ? 4 : 64);
    std::vector<uint8_t> Memory(BroadcastSource ? 4 : 64, 0);
    if (BroadcastSource) {
      setDwordLane(Memory, 0, Inputs32[0]);
    } else {
      for (unsigned Lane = 0; Lane < 16; ++Lane)
        setDwordLane(Memory, Lane, Inputs32[Lane % Inputs32.size()]);
    }
    BinaryImage MemoryImage;
    MemoryImage.Arch = Arch::X64;
    MemoryImage.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = EffectiveAddress;
    Data.Size = Memory.size();
    Data.Flags = SegmentFlags::Readable;
    Data.Data = Memory;
    MemoryImage.Segments.push_back(std::move(Data));
    const std::vector<LowOp> Ops = liftX64(
        {0x62, 0xf2, 0x7d, static_cast<uint8_t>(BroadcastSource ? 0x58 : 0x48),
         0xca, 0x48, 0x01});
    ASSERT_FALSE(Ops.empty());
    NdOpEmulator Emulator(MemoryImage);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, MatrixBase);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    ASSERT_FALSE(Emulator.getLoadRecords().empty());
    EXPECT_EQ(Emulator.getLoadRecords().front().Addr, EffectiveAddress);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Segment and address-size prefixes remain part of the exact memory
  // address space instead of being discarded at the EVEX validation boundary.
  {
    constexpr uint64_t GSBase = UINT64_C(0x700000);
    constexpr uint64_t Offset = UINT64_C(0x80);
    std::vector<uint8_t> Memory(64, 0);
    std::vector<uint8_t> Expected(64, 0);
    for (unsigned Lane = 0; Lane < 16; ++Lane) {
      setDwordLane(Memory, Lane, Inputs32[0]);
      setDwordLane(Expected, Lane, Rcp32[0]);
    }
    BinaryImage MemoryImage;
    MemoryImage.Arch = Arch::X64;
    MemoryImage.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = GSBase + Offset;
    Data.Size = Memory.size();
    Data.Flags = SegmentFlags::Readable;
    Data.Data = Memory;
    MemoryImage.Segments.push_back(std::move(Data));
    const std::vector<LowOp> Ops =
        liftX64({0x65, 0x62, 0xf2, 0x7d, 0x48, 0xca, 0x08});
    ASSERT_FALSE(Ops.empty());
    NdOpEmulator Emulator(MemoryImage);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS,
                                                   GSBase));
    Emulator.setRegister(x86reg::RAX, Offset);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), Expected);
    ASSERT_EQ(Emulator.getLoadRecords().size(), 16u);
    EXPECT_EQ(Emulator.getLoadRecords().front().Addr, GSBase + Offset);
    EXPECT_FALSE(Emulator.skips().any());
  }
  {
    constexpr uint64_t Address = UINT64_C(0x7200);
    std::vector<uint8_t> Memory(64, 0);
    for (unsigned Lane = 0; Lane < 16; ++Lane)
      setDwordLane(Memory, Lane, Inputs32[0]);
    BinaryImage MemoryImage;
    MemoryImage.Arch = Arch::X64;
    MemoryImage.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = Address;
    Data.Size = Memory.size();
    Data.Flags = SegmentFlags::Readable;
    Data.Data = Memory;
    MemoryImage.Segments.push_back(std::move(Data));
    const std::vector<LowOp> Ops =
        liftX64({0x67, 0x62, 0xf2, 0x7d, 0x48, 0xca, 0x08});
    ASSERT_FALSE(Ops.empty());
    NdOpEmulator Emulator(MemoryImage);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, UINT64_C(0xabcdef0000007200));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    ASSERT_EQ(Emulator.getLoadRecords().size(), 16u);
    EXPECT_EQ(Emulator.getLoadRecords().front().Addr, Address);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Register SAE forms are LLIG in all four LL encodings. Scalar non-SAE
  // forms are LLIG for LL=0/1/2. The decoder and lifter must agree on both.
  for (uint8_t Length :
       {uint8_t{0}, uint8_t{0x20}, uint8_t{0x40}, uint8_t{0x60}}) {
    EXPECT_FALSE(liftX64({0x62, 0xf2, 0x7d, static_cast<uint8_t>(0x18 | Length),
                          0xca, 0xca})
                     .empty());
    EXPECT_FALSE(liftX64({0x62, 0xf2, 0x4d, static_cast<uint8_t>(0x18 | Length),
                          0xcb, 0xef})
                     .empty());
    if (Length != 0x60)
      EXPECT_FALSE(liftX64({0x62, 0xf2, 0x4d,
                            static_cast<uint8_t>(0x08 | Length), 0xcb, 0xef})
                       .empty());
  }

  auto ExpectDecodeRejected = [](const std::vector<uint8_t> &Bytes) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    EXPECT_NE(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn),
              static_cast<int>(Bytes.size()));
  };
  ExpectDecodeRejected({0x62, 0xf2, 0x7d, 0x08, 0xca, 0x08});
  ExpectDecodeRejected({0x62, 0xf2, 0x7d, 0x38, 0xca, 0x08});
  ExpectDecodeRejected({0x62, 0xf2, 0x4d, 0x18, 0xcb, 0x28});
  ExpectDecodeRejected({0x62, 0xf2, 0x4d, 0x68, 0xcb, 0x28});
}

TEST(X86WideISAState, EvexTernaryLogicMemoryUsesMaskedTruthTableSemantics) {
  enum class MaskMode { None, Merge, Zero };
  constexpr MaskMode MaskModes[] = {MaskMode::None, MaskMode::Merge,
                                    MaskMode::Zero};
  constexpr uint64_t MemoryBase = UINT64_C(0x8000);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM1);
  const RegInfo FirstSource = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K3);
  constexpr uint8_t TruthTable = 0x96; // three-input XOR

  // Cover both element widths, all vector lengths, full and broadcast tuples,
  // and each architectural writemask policy.  Full tuples read exactly the
  // active elements; a broadcast tuple reads its scalar at most once.
  for (size_t ElementSize : {size_t{4}, size_t{8}}) {
    for (size_t VectorSize : {size_t{16}, size_t{32}, size_t{64}}) {
      const uint8_t Length =
          VectorSize == 16 ? 0 : (VectorSize == 32 ? 0x20 : 0x40);
      const size_t LaneCount = VectorSize / ElementSize;
      const uint64_t LaneValueMask =
          ElementSize == 8 ? UINT64_MAX : UINT64_C(0xffffffff);
      const uint64_t MaskBits = LaneCount == 2 ? UINT64_C(1) : UINT64_C(0x5555);
      for (bool Broadcast : {false, true}) {
        const size_t MemorySize = Broadcast ? ElementSize : VectorSize;
        std::vector<uint8_t> Memory(MemorySize, 0);
        for (size_t Lane = 0; Lane < MemorySize / ElementSize; ++Lane)
          setIntegerLane(Memory, Lane, ElementSize,
                         (UINT64_C(0xaa00000012340000) + Lane * 11) &
                             LaneValueMask);

        BinaryImage Image;
        Image.Arch = Arch::X64;
        Image.Bits = Bitness::Bits64;
        Segment Data;
        Data.VA = MemoryBase;
        Data.Size = Memory.size();
        Data.Flags = SegmentFlags::Readable;
        Data.Data = Memory;
        Image.Segments.push_back(std::move(Data));

        for (MaskMode Mode : MaskModes) {
          SCOPED_TRACE(testing::Message()
                       << "element=" << ElementSize << " vector=" << VectorSize
                       << " broadcast=" << Broadcast
                       << " mask-mode=" << static_cast<unsigned>(Mode));
          const uint8_t EncodedMask = Mode == MaskMode::None ? 0 : 3;
          const uint8_t Zero = Mode == MaskMode::Zero ? 0x80 : 0;
          const std::vector<uint8_t> Bytes = {
              0x62,
              0xf3,
              static_cast<uint8_t>(ElementSize == 8 ? 0xed : 0x6d),
              static_cast<uint8_t>(0x08 | Length | EncodedMask | Zero |
                                   (Broadcast ? 0x10 : 0)),
              0x25,
              0x08,
              TruthTable};
          const std::vector<LowOp> Ops = liftX64(Bytes);
          ASSERT_FALSE(Ops.empty());
          ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

          std::vector<uint8_t> OldDestination(64, 0xa5);
          std::vector<uint8_t> First(64, 0);
          std::vector<uint8_t> Expected(64, 0);
          std::vector<LoadRecord> ExpectedLoads;
          for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
            const uint64_t Old =
                (UINT64_C(0x1111000012340000) + Lane * 3) & LaneValueMask;
            const uint64_t A =
                (UINT64_C(0x00ff0000abcd0000) + Lane * 5) & LaneValueMask;
            setIntegerLane(OldDestination, Lane, ElementSize, Old);
            setIntegerLane(First, Lane, ElementSize, A);
            const bool Active =
                Mode == MaskMode::None || ((MaskBits >> Lane) & 1U) != 0;
            uint64_t Value = 0;
            if (Active) {
              const size_t SourceLane = Broadcast ? 0 : Lane;
              Value = Old ^ A ^ getIntegerLane(Memory, SourceLane, ElementSize);
              if (!Broadcast)
                ExpectedLoads.push_back({MemoryBase + Lane * ElementSize,
                                         static_cast<uint16_t>(ElementSize)});
            } else if (Mode == MaskMode::Merge) {
              Value = Old;
            }
            setIntegerLane(Expected, Lane, ElementSize, Value);
          }
          if (Broadcast)
            ExpectedLoads.push_back(
                {MemoryBase, static_cast<uint16_t>(ElementSize)});

          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          Emulator.setLoadCollect(true);
          Emulator.setRegister(x86reg::RAX, MemoryBase);
          Emulator.setRegisterBytes(Destination.Offset, OldDestination);
          Emulator.setRegisterBytes(FirstSource.Offset, First);
          Emulator.setRegister(Mask.Offset, MaskBits);
          ASSERT_EQ(Emulator.run(Ops), Ops.size());
          EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), Expected);
          ASSERT_EQ(Emulator.getLoadRecords().size(), ExpectedLoads.size());
          for (size_t Index = 0; Index < ExpectedLoads.size(); ++Index) {
            EXPECT_EQ(Emulator.getLoadRecords()[Index].Addr,
                      ExpectedLoads[Index].Addr);
            EXPECT_EQ(Emulator.getLoadRecords()[Index].Size,
                      ExpectedLoads[Index].Size);
          }
          EXPECT_FALSE(Emulator.skips().any());
        }
      }
    }
  }

  // A zero mask suppresses an unmapped broadcast.  If a later active element
  // faults, earlier staged reads and the destination write are both discarded.
  std::vector<uint8_t> OldDestination(64, 0xa5);
  std::vector<uint8_t> First(64, 0x5a);
  {
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf3, 0x6d, 0x5b, 0x25, 0x08, TruthTable});
    ASSERT_FALSE(Ops.empty());
    BinaryImage EmptyImage;
    EmptyImage.Arch = Arch::X64;
    EmptyImage.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(EmptyImage);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, UINT64_C(0xdead0000));
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegisterBytes(FirstSource.Offset, First);
    Emulator.setRegister(Mask.Offset, 0);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), OldDestination);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());
  }
  {
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf3, 0x6d, 0x4b, 0x25, 0x08, TruthTable});
    ASSERT_FALSE(Ops.empty());
    BinaryImage FaultImage;
    FaultImage.Arch = Arch::X64;
    FaultImage.Bits = Bitness::Bits64;
    Segment FirstLane;
    FirstLane.VA = MemoryBase;
    FirstLane.Size = 4;
    FirstLane.Flags = SegmentFlags::Readable;
    FirstLane.Data = {0x11, 0x22, 0x33, 0x44};
    FaultImage.Segments.push_back(std::move(FirstLane));
    NdOpEmulator Emulator(FaultImage);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, MemoryBase);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegisterBytes(FirstSource.Offset, First);
    Emulator.setRegister(Mask.Offset, 3);
    EXPECT_LT(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), OldDestination);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Compressed disp8 uses the full-vector tuple scale for ordinary memory and
  // the scalar tuple scale for broadcast memory.
  for (bool Broadcast : {false, true}) {
    const uint64_t EffectiveAddress = MemoryBase + (Broadcast ? 4 : 64);
    std::vector<uint8_t> Memory(Broadcast ? 4 : 64, 0x3c);
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = EffectiveAddress;
    Data.Size = Memory.size();
    Data.Flags = SegmentFlags::Readable;
    Data.Data = Memory;
    Image.Segments.push_back(std::move(Data));
    const std::vector<LowOp> Ops = liftX64(
        {0x62, 0xf3, 0x6d, static_cast<uint8_t>(Broadcast ? 0x58 : 0x48), 0x25,
         0x48, 0x01, TruthTable});
    ASSERT_FALSE(Ops.empty());
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, MemoryBase);
    Emulator.setRegisterBytes(Destination.Offset, OldDestination);
    Emulator.setRegisterBytes(FirstSource.Offset, First);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    ASSERT_FALSE(Emulator.getLoadRecords().empty());
    EXPECT_EQ(Emulator.getLoadRecords().front().Addr, EffectiveAddress);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // EVEX high-register extensions must select the raw encoded ZMM bank, not
  // whichever aliases happen to appear in Capstone's operand list.
  {
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xa3, 0x6d, 0x43, 0x25, 0xcb, TruthTable});
    ASSERT_FALSE(Ops.empty());
    const RegInfo HighDestination = mapCapstoneReg(X86_REG_ZMM17);
    const RegInfo HighFirst = mapCapstoneReg(X86_REG_ZMM18);
    const RegInfo HighSecond = mapCapstoneReg(X86_REG_ZMM19);
    std::vector<uint8_t> Old(64), A(64), B(64), Expected(64);
    for (size_t Lane = 0; Lane < 16; ++Lane) {
      setDwordLane(Old, Lane, 0x11000000U + Lane);
      setDwordLane(A, Lane, 0x00220000U + Lane);
      setDwordLane(B, Lane, 0x00003300U + Lane);
      const uint32_t Value = getDwordLane(Old, Lane) ^ getDwordLane(A, Lane) ^
                             getDwordLane(B, Lane);
      setDwordLane(Expected, Lane,
                   (Lane & 1U) == 0 ? Value : getDwordLane(Old, Lane));
    }
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(HighDestination.Offset, Old);
    Emulator.setRegisterBytes(HighFirst.Offset, A);
    Emulator.setRegisterBytes(HighSecond.Offset, B);
    Emulator.setRegister(Mask.Offset, UINT64_C(0x5555));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(HighDestination.Offset), Expected);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Reserved LL, register-form b, wrong mandatory prefix, zeroing with K0,
  // and duplicate legacy prefixes must never silently acquire semantics.
  auto ExpectRejected = [](const std::vector<uint8_t> &Bytes) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn) !=
        static_cast<int>(Bytes.size()))
      return;
    std::vector<LowOp> Ops;
    EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  };
  ExpectRejected({0x62, 0xf3, 0x6d, 0x68, 0x25, 0x08, TruthTable});
  ExpectRejected({0x62, 0xf3, 0x6d, 0x58, 0x25, 0xc3, TruthTable});
  ExpectRejected({0x62, 0xf3, 0x6c, 0x48, 0x25, 0x08, TruthTable});
  ExpectRejected({0x62, 0xf3, 0x6d, 0xc8, 0x25, 0x08, TruthTable});
  ExpectRejected({0x65, 0x64, 0x62, 0xf3, 0x6d, 0x48, 0x25, 0x08, TruthTable});
}

TEST(X86WideISAState, EvexFPClassUsesArchitecturalCategoriesAndDaz) {
  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Destination = mapCapstoneReg(X86_REG_K3);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K1);
  const std::array<uint32_t, 10> Values32 = {
      0x7fc00001U, // quiet NaN
      0x00000000U, // +0
      0x80000000U, // -0
      0x7f800000U, // +infinity
      0xff800000U, // -infinity
      0x00000001U, // positive denormal
      0xbf800000U, // negative finite
      0x7f800001U, // signaling NaN
      0x3f800000U, // positive finite (no class bit)
      0x80000001U, // negative denormal (denormal and negative finite)
  };
  const std::array<uint64_t, 10> Values64 = {
      UINT64_C(0x7ff8000000000001), // quiet NaN
      UINT64_C(0x0000000000000000), // +0
      UINT64_C(0x8000000000000000), // -0
      UINT64_C(0x7ff0000000000000), // +infinity
      UINT64_C(0xfff0000000000000), // -infinity
      UINT64_C(0x0000000000000001), // positive denormal
      UINT64_C(0xbff0000000000000), // negative finite
      UINT64_C(0x7ff0000000000001), // signaling NaN
      UINT64_C(0x3ff0000000000000), // positive finite (no class bit)
      UINT64_C(0x8000000000000001), // negative denormal
  };

  auto IsSelected = [](uint64_t Bits, bool F64, uint8_t Immediate, bool Daz) {
    const unsigned FractionBits = F64 ? 52 : 23;
    const uint64_t FractionMask =
        F64 ? UINT64_C(0x000fffffffffffff) : UINT64_C(0x007fffff);
    const uint64_t ExponentMask =
        F64 ? UINT64_C(0x7ff0000000000000) : UINT64_C(0x7f800000);
    const uint64_t SignMask =
        F64 ? UINT64_C(0x8000000000000000) : UINT64_C(0x80000000);
    const bool Sign = (Bits & SignMask) != 0;
    const uint64_t Exponent = Bits & ExponentMask;
    uint64_t Fraction = Bits & FractionMask;
    if (Daz && Exponent == 0 && Fraction != 0)
      Fraction = 0;

    const bool ExponentAllOnes = Exponent == ExponentMask;
    const bool Zero = Exponent == 0 && Fraction == 0;
    const bool Denormal = Exponent == 0 && Fraction != 0;
    const bool Infinity = ExponentAllOnes && Fraction == 0;
    const bool NaN = ExponentAllOnes && Fraction != 0;
    const bool QuietNaN =
        NaN && (Fraction & (UINT64_C(1) << (FractionBits - 1))) != 0;
    const bool SignalingNaN = NaN && !QuietNaN;
    const bool NegativeFinite = Sign && !ExponentAllOnes && !Zero;
    return (QuietNaN && (Immediate & 0x01)) ||
           (!Sign && Zero && (Immediate & 0x02)) ||
           (Sign && Zero && (Immediate & 0x04)) ||
           (!Sign && Infinity && (Immediate & 0x08)) ||
           (Sign && Infinity && (Immediate & 0x10)) ||
           (Denormal && (Immediate & 0x20)) ||
           (NegativeFinite && (Immediate & 0x40)) ||
           (SignalingNaN && (Immediate & 0x80));
  };

  auto ValueForLane = [&](bool F64, size_t Lane) {
    return F64 ? Values64[Lane % Values64.size()]
               : static_cast<uint64_t>(Values32[Lane % Values32.size()]);
  };

  auto RelevantMask = [](unsigned LaneCount) {
    return LaneCount == 64 ? UINT64_MAX : (UINT64_C(1) << LaneCount) - 1;
  };

  // The eight immediate bits are independent architectural selectors. Keep
  // this oracle table literal so a production-side bit permutation cannot be
  // hidden by the broader all-category matrix below.
  for (bool F64 : {false, true}) {
    const std::array<uint64_t, 8> Categories =
        F64 ? std::array<uint64_t, 8>{
                  UINT64_C(0x7ff8000000000001),
                  UINT64_C(0x0000000000000000),
                  UINT64_C(0x8000000000000000),
                  UINT64_C(0x7ff0000000000000),
                  UINT64_C(0xfff0000000000000),
                  UINT64_C(0x0000000000000001),
                  UINT64_C(0xbff0000000000000),
                  UINT64_C(0x7ff0000000000001),
              }
            : std::array<uint64_t, 8>{
                  UINT64_C(0x7fc00001), UINT64_C(0x00000000),
                  UINT64_C(0x80000000), UINT64_C(0x7f800000),
                  UINT64_C(0xff800000), UINT64_C(0x00000001),
                  UINT64_C(0xbf800000), UINT64_C(0x7f800001),
              };
    for (unsigned Category = 0; Category < Categories.size(); ++Category) {
      const uint8_t Immediate = static_cast<uint8_t>(1U << Category);
      const std::vector<LowOp> Ops =
          liftX64({0x62, 0xf3, static_cast<uint8_t>(F64 ? 0xfd : 0x7d), 0x49,
                   0x66, 0xda, Immediate});
      ASSERT_FALSE(Ops.empty());
      std::vector<uint8_t> SourceValue(64, 0);
      for (size_t Lane = 0; Lane < Categories.size(); ++Lane)
        setIntegerLane(SourceValue, Lane, F64 ? 8 : 4, Categories[Lane]);
      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setRegisterBytes(Source.Offset, SourceValue);
      Emulator.setRegister(Destination.Offset, UINT64_MAX);
      Emulator.setRegister(WriteMask.Offset, UINT64_C(0xff));
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      EXPECT_EQ(Emulator.getRegister(Destination.Offset), UINT64_C(1)
                                                              << Category);
      EXPECT_FALSE(Emulator.skips().any());
    }
  }

  // Packed register forms cover every vector length and both element widths.
  for (bool F64 : {false, true}) {
    const uint8_t P1 = F64 ? 0xfd : 0x7d;
    const size_t ElementSize = F64 ? 8 : 4;
    for (unsigned LengthCode = 0; LengthCode != 3; ++LengthCode) {
      const size_t VectorSize = size_t{16} << LengthCode;
      const unsigned LaneCount = VectorSize / ElementSize;
      const uint8_t P2 = static_cast<uint8_t>(0x09 | (LengthCode << 5));
      const std::vector<LowOp> Ops =
          liftX64({0x62, 0xf3, P1, P2, 0x66, 0xda, 0xff});
      ASSERT_FALSE(Ops.empty());
      ASSERT_TRUE(hasOnlyMappedRegisters(Ops));

      std::vector<uint8_t> SourceValue(64, 0);
      uint64_t Expected = 0;
      for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
        const uint64_t Value = ValueForLane(F64, Lane);
        setIntegerLane(SourceValue, Lane, ElementSize, Value);
        if (IsSelected(Value, F64, 0xff, false))
          Expected |= UINT64_C(1) << Lane;
      }

      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setRegisterBytes(Source.Offset, SourceValue);
      Emulator.setRegister(Destination.Offset, UINT64_MAX);
      Emulator.setRegister(WriteMask.Offset, RelevantMask(LaneCount));
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      EXPECT_EQ(Emulator.getRegister(Destination.Offset), Expected);
      EXPECT_EQ(Emulator.getMXCSR(), 0x1f80U);
      EXPECT_FALSE(Emulator.skips().any());
    }
  }

  // Omitting the destination writemask makes every architectural lane active.
  {
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf3, 0x7d, 0x48, 0x66, 0xda, 0xff});
    std::vector<uint8_t> SourceValue(64, 0);
    uint64_t Expected = 0;
    for (size_t Lane = 0; Lane < 16; ++Lane) {
      const uint64_t Value = ValueForLane(false, Lane);
      setIntegerLane(SourceValue, Lane, 4, Value);
      if (IsSelected(Value, false, 0xff, false))
        Expected |= UINT64_C(1) << Lane;
    }
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(Source.Offset, SourceValue);
    Emulator.setRegister(Destination.Offset, UINT64_MAX);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(Destination.Offset), Expected);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Scalar LL is ignored for all three non-reserved encodings.
  for (bool F64 : {false, true}) {
    for (uint8_t Length : {uint8_t{0x00}, uint8_t{0x20}, uint8_t{0x40}}) {
      SCOPED_TRACE(testing::Message() << "scalar F64=" << F64 << " encoded LL="
                                      << unsigned(Length >> 5));
      const std::vector<LowOp> Ops =
          liftX64({0x62, 0xf3, static_cast<uint8_t>(F64 ? 0xfd : 0x7d),
                   static_cast<uint8_t>(0x09 | Length), 0x67, 0xda, 0x60});
      ASSERT_FALSE(Ops.empty());
      std::vector<uint8_t> SourceValue(64, 0);
      setIntegerLane(SourceValue, 0, F64 ? 8 : 4,
                     F64 ? UINT64_C(0x8000000000000001) : UINT64_C(0x80000001));
      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setRegisterBytes(Source.Offset, SourceValue);
      Emulator.setRegister(Destination.Offset, UINT64_MAX);
      Emulator.setRegister(WriteMask.Offset, 1);
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      EXPECT_EQ(Emulator.getRegister(Destination.Offset), 1U);
      EXPECT_FALSE(Emulator.skips().any());
    }
  }

  // DAZ reclassifies denormals as signed zero without changing MXCSR.
  std::vector<uint8_t> Denormals(64, 0);
  setDwordLane(Denormals, 0, 0x00000001U);
  setDwordLane(Denormals, 1, 0x80000001U);
  auto RunDazCase = [&](uint8_t Immediate, uint32_t MXCSR, uint64_t Expected) {
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf3, 0x7d, 0x49, 0x66, 0xda, Immediate});
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(MXCSR);
    Emulator.setRegisterBytes(Source.Offset, Denormals);
    Emulator.setRegister(Destination.Offset, UINT64_MAX);
    Emulator.setRegister(WriteMask.Offset, 3);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(Destination.Offset), Expected);
    EXPECT_EQ(Emulator.getMXCSR(), MXCSR);
    EXPECT_FALSE(Emulator.skips().any());
  };
  RunDazCase(0x20, 0x1f80, 3);
  RunDazCase(0x20, 0x1fc0, 0);
  RunDazCase(0x06, 0x1f80, 0);
  RunDazCase(0x06, 0x1fc0, 3);
  RunDazCase(0x40, 0x1f80, 2);
  RunDazCase(0x40, 0x1fc0, 0);

  // Packed full-tuple and broadcast memory forms share the same masked
  // classification, but only the elements actually selected by K1 may load.
  constexpr uint64_t MatrixBase = UINT64_C(0x7000);
  for (bool F64 : {false, true}) {
    const uint8_t P1 = F64 ? 0xfd : 0x7d;
    const size_t ElementSize = F64 ? 8 : 4;
    for (unsigned LengthCode = 0; LengthCode != 3; ++LengthCode) {
      const size_t VectorSize = size_t{16} << LengthCode;
      const unsigned LaneCount = VectorSize / ElementSize;
      const uint64_t Active = UINT64_C(1) | (UINT64_C(1) << (LaneCount - 1));
      std::vector<uint8_t> Tuple(VectorSize, 0);
      uint64_t Expected = 0;
      for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
        const uint64_t Value = ValueForLane(F64, Lane);
        setIntegerLane(Tuple, Lane, ElementSize, Value);
        if ((Active & (UINT64_C(1) << Lane)) != 0 &&
            IsSelected(Value, F64, 0xff, false))
          Expected |= UINT64_C(1) << Lane;
      }

      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      Segment Data;
      Data.VA = MatrixBase;
      Data.Size = Tuple.size();
      Data.Flags = SegmentFlags::Readable;
      Data.Data = Tuple;
      Image.Segments.push_back(std::move(Data));
      const uint8_t P2 = static_cast<uint8_t>(0x09 | (LengthCode << 5));
      const std::vector<LowOp> Ops =
          liftX64({0x62, 0xf3, P1, P2, 0x66, 0x18, 0xff});
      ASSERT_FALSE(Ops.empty());
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setLoadCollect(true);
      Emulator.setRegister(x86reg::RAX, MatrixBase);
      Emulator.setRegister(Destination.Offset, UINT64_MAX);
      Emulator.setRegister(WriteMask.Offset, Active);
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      EXPECT_EQ(Emulator.getRegister(Destination.Offset), Expected);
      ASSERT_EQ(Emulator.getLoadRecords().size(), LaneCount == 1 ? 1u : 2u);
      EXPECT_EQ(Emulator.getLoadRecords().front().Addr, MatrixBase);
      EXPECT_EQ(Emulator.getLoadRecords().back().Addr,
                MatrixBase + (LaneCount - 1) * ElementSize);
      EXPECT_FALSE(Emulator.skips().any());

      const uint64_t BroadcastValue =
          F64 ? UINT64_C(0xbff0000000000000) : UINT64_C(0xbf800000);
      std::vector<uint8_t> BroadcastData(ElementSize, 0);
      setIntegerLane(BroadcastData, 0, ElementSize, BroadcastValue);
      BinaryImage BroadcastImage;
      BroadcastImage.Arch = Arch::X64;
      BroadcastImage.Bits = Bitness::Bits64;
      Segment BroadcastSegment;
      BroadcastSegment.VA = MatrixBase;
      BroadcastSegment.Size = BroadcastData.size();
      BroadcastSegment.Flags = SegmentFlags::Readable;
      BroadcastSegment.Data = BroadcastData;
      BroadcastImage.Segments.push_back(std::move(BroadcastSegment));
      const std::vector<LowOp> BroadcastOps = liftX64(
          {0x62, 0xf3, P1, static_cast<uint8_t>(P2 | 0x10), 0x66, 0x18, 0xff});
      ASSERT_FALSE(BroadcastOps.empty());
      NdOpEmulator BroadcastEmulator(BroadcastImage);
      BroadcastEmulator.setStrictMode(true);
      BroadcastEmulator.setLoadCollect(true);
      BroadcastEmulator.setRegister(x86reg::RAX, MatrixBase);
      BroadcastEmulator.setRegister(Destination.Offset, UINT64_MAX);
      BroadcastEmulator.setRegister(WriteMask.Offset, Active);
      ASSERT_EQ(BroadcastEmulator.run(BroadcastOps), BroadcastOps.size());
      EXPECT_EQ(BroadcastEmulator.getRegister(Destination.Offset), Active);
      ASSERT_EQ(BroadcastEmulator.getLoadRecords().size(), 1u);
      EXPECT_EQ(BroadcastEmulator.getLoadRecords()[0].Addr, MatrixBase);
      EXPECT_EQ(BroadcastEmulator.getLoadRecords()[0].Size, ElementSize);
      EXPECT_FALSE(BroadcastEmulator.skips().any());
    }
  }

  // Scalar memory reads exactly one element when K1[0] is active. Other mask
  // bits neither classify nor make an otherwise unmapped source fault.
  for (bool F64 : {false, true}) {
    const size_t ElementSize = F64 ? 8 : 4;
    const uint64_t QuietNaN =
        F64 ? UINT64_C(0x7ff8000000000001) : UINT64_C(0x7fc00001);
    std::vector<uint8_t> ScalarData(ElementSize, 0);
    setIntegerLane(ScalarData, 0, ElementSize, QuietNaN);
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = MatrixBase;
    Data.Size = ScalarData.size();
    Data.Flags = SegmentFlags::Readable;
    Data.Data = ScalarData;
    Image.Segments.push_back(std::move(Data));
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf3, static_cast<uint8_t>(F64 ? 0xfd : 0x7d), 0x09,
                 0x67, 0x18, 0x01});
    ASSERT_FALSE(Ops.empty());
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, MatrixBase);
    Emulator.setRegister(WriteMask.Offset, 1);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(Destination.Offset), 1U);
    ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, MatrixBase);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Size, ElementSize);

    BinaryImage EmptyImage;
    EmptyImage.Arch = Arch::X64;
    EmptyImage.Bits = Bitness::Bits64;
    NdOpEmulator InactiveEmulator(EmptyImage);
    InactiveEmulator.setStrictMode(true);
    InactiveEmulator.setLoadCollect(true);
    InactiveEmulator.setRegister(x86reg::RAX, UINT64_C(0xdead0000));
    InactiveEmulator.setRegister(Destination.Offset, UINT64_MAX);
    InactiveEmulator.setRegister(WriteMask.Offset, 2);
    ASSERT_EQ(InactiveEmulator.run(Ops), Ops.size());
    EXPECT_EQ(InactiveEmulator.getRegister(Destination.Offset), 0U);
    EXPECT_TRUE(InactiveEmulator.getLoadRecords().empty());
    EXPECT_FALSE(InactiveEmulator.skips().any());
  }

  // A zero mask suppresses an unmapped tuple. A later active-lane fault is
  // atomic with respect to both the destination mask and collected loads.
  {
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf3, 0x7d, 0x49, 0x66, 0x18, 0xff});
    BinaryImage EmptyImage;
    EmptyImage.Arch = Arch::X64;
    EmptyImage.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(EmptyImage);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, UINT64_C(0xdead0000));
    Emulator.setRegister(Destination.Offset, UINT64_MAX);
    Emulator.setRegister(WriteMask.Offset, 0);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(Destination.Offset), 0U);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());

    BinaryImage FaultImage;
    FaultImage.Arch = Arch::X64;
    FaultImage.Bits = Bitness::Bits64;
    Segment FirstLane;
    FirstLane.VA = MatrixBase;
    FirstLane.Size = 4;
    FirstLane.Flags = SegmentFlags::Readable;
    FirstLane.Data = {0x01, 0x00, 0xc0, 0x7f};
    FaultImage.Segments.push_back(std::move(FirstLane));
    NdOpEmulator FaultEmulator(FaultImage);
    FaultEmulator.setStrictMode(true);
    FaultEmulator.setLoadCollect(true);
    FaultEmulator.setRegister(x86reg::RAX, MatrixBase);
    constexpr uint64_t OldDestination = UINT64_C(0x5aa55aa55aa55aa5);
    FaultEmulator.setRegister(Destination.Offset, OldDestination);
    FaultEmulator.setRegister(WriteMask.Offset, 5);
    EXPECT_LT(FaultEmulator.run(Ops), Ops.size());
    EXPECT_EQ(FaultEmulator.getRegister(Destination.Offset), OldDestination);
    EXPECT_TRUE(FaultEmulator.getLoadRecords().empty());
    EXPECT_FALSE(FaultEmulator.skips().any());
  }

  // Disp8 uses the full-tuple scale, and high EVEX register extensions select
  // the raw encoded vector bank.
  struct Disp8Case {
    uint8_t P1;
    uint8_t P2;
    uint8_t Opcode;
    uint8_t Scale;
    uint8_t ElementSize;
  };
  const std::vector<Disp8Case> Disp8Cases = {
      {0x7d, 0x09, 0x66, 16, 4}, {0x7d, 0x29, 0x66, 32, 4},
      {0x7d, 0x49, 0x66, 64, 4}, {0xfd, 0x09, 0x66, 16, 8},
      {0xfd, 0x29, 0x66, 32, 8}, {0xfd, 0x49, 0x66, 64, 8},
      {0x7d, 0x19, 0x66, 4, 4},  {0x7d, 0x39, 0x66, 4, 4},
      {0x7d, 0x59, 0x66, 4, 4},  {0xfd, 0x19, 0x66, 8, 8},
      {0xfd, 0x39, 0x66, 8, 8},  {0xfd, 0x59, 0x66, 8, 8},
      {0x7d, 0x09, 0x67, 4, 4},  {0xfd, 0x09, 0x67, 8, 8},
  };
  for (const Disp8Case &Current : Disp8Cases) {
    constexpr uint64_t Base = UINT64_C(0x8000);
    const uint64_t EffectiveAddress = Base + Current.Scale;
    const std::vector<LowOp> Ops = liftX64(
        {0x62, 0xf3, Current.P1, Current.P2, Current.Opcode, 0x58, 0x01, 0x01});
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = EffectiveAddress;
    Data.Size = Current.ElementSize;
    Data.Flags = SegmentFlags::Readable;
    Data.Data.assign(Current.ElementSize, 0);
    setIntegerLane(Data.Data, 0, Current.ElementSize,
                   Current.ElementSize == 8 ? UINT64_C(0x7ff8000000000001)
                                            : UINT64_C(0x7fc00001));
    Image.Segments.push_back(std::move(Data));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Base);
    Emulator.setRegister(WriteMask.Offset, 1);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(Destination.Offset), 1U);
    ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, EffectiveAddress);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Size, Current.ElementSize);
    EXPECT_FALSE(Emulator.skips().any());
  }
  struct RegisterBank {
    uint8_t P0;
    x86_reg Register;
  };
  for (const RegisterBank &Bank :
       {RegisterBank{0xf3, X86_REG_ZMM2}, RegisterBank{0xd3, X86_REG_ZMM10},
        RegisterBank{0xb3, X86_REG_ZMM18}, RegisterBank{0x93, X86_REG_ZMM26}}) {
    const std::vector<LowOp> Ops =
        liftX64({0x62, Bank.P0, 0x7d, 0x49, 0x66, 0xda, 0x40});
    std::vector<uint8_t> SourceValue(64, 0);
    setDwordLane(SourceValue, 0, 0xbf800000U);
    const RegInfo BankSource = mapCapstoneReg(Bank.Register);
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(BankSource.Offset, SourceValue);
    Emulator.setRegister(WriteMask.Offset, 1);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(Destination.Offset), 1U);
    EXPECT_FALSE(Emulator.skips().any());
  }
  for (uint8_t P2 : {uint8_t{0x09}, uint8_t{0x29}}) {
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0x93, 0x7d, P2, 0x66, 0xda, 0x40});
    std::vector<uint8_t> SourceValue(64, 0);
    setDwordLane(SourceValue, 0, 0xbf800000U);
    const RegInfo BankSource = mapCapstoneReg(X86_REG_ZMM26);
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(BankSource.Offset, SourceValue);
    Emulator.setRegister(WriteMask.Offset, 1);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(Destination.Offset), 1U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Malformed serialized LowIR must be rejected at the structural boundary,
  // before either strict emulation or backend lowering sees the intrinsic.
  auto MakeFPClassBlock = [&](uint8_t Control, uint16_t SourceSize,
                              uint16_t MaskSize, uint16_t OutputSize) {
    LowOp Op;
    Op.Addr = kAddress;
    Op.Opcode = NdOp::INTRINSIC;
    Op.Output = NdVar::tmp(UINT64_C(0x9200), OutputSize);
    Op.addInput(NdVar::cst(static_cast<uint64_t>(Intrinsic::X86FPClass), 2));
    Op.addInput(NdVar::cst(Control, 1));
    Op.addInput(NdVar::reg(Source.Offset, SourceSize));
    Op.addInput(NdVar::reg(WriteMask.Offset, MaskSize));
    Op.addInput(NdVar::cst(0xff, 1));

    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = kAddress;
    Block.EndAddr = kAddress + 1;
    Block.Ops.push_back(Op);
    LowInstructionBoundary Boundary;
    Boundary.Address = kAddress;
    Boundary.Size = 1;
    Boundary.FirstOp = 0;
    Boundary.OpCount = 1;
    Block.InstructionBoundaries.push_back(Boundary);
    return Block;
  };
  auto ValidationError = [](const LowBlock &Block) {
    return llvm::toString(validateLowInstructionBoundaries(
        Block, LowInstructionBoundaryRequirement::Required));
  };

  const LowBlock ValidShape = MakeFPClassBlock(0, 16, 1, 1);
  EXPECT_TRUE(ValidationError(ValidShape).empty());
  auto ExpectInvalidShape = [&](LowBlock Block) {
    EXPECT_FALSE(ValidationError(Block).empty());
  };
  {
    LowBlock Bad = ValidShape;
    Bad.Ops[0].NumInputs = 4;
    ExpectInvalidShape(std::move(Bad));
  }
  {
    LowBlock Bad = ValidShape;
    Bad.Ops[0].Inputs[1] = NdVar::reg(x86reg::RAX, 1);
    ExpectInvalidShape(std::move(Bad));
  }
  {
    LowBlock Bad = ValidShape;
    Bad.Ops[0].Inputs[1] = NdVar::cst(4, 1);
    ExpectInvalidShape(std::move(Bad));
  }
  ExpectInvalidShape(MakeFPClassBlock(0, 8, 1, 1));
  ExpectInvalidShape(MakeFPClassBlock(2, 32, 1, 1));
  ExpectInvalidShape(MakeFPClassBlock(0, 64, 1, 2));
  ExpectInvalidShape(MakeFPClassBlock(0, 16, 1, 2));
  {
    LowBlock Bad = ValidShape;
    Bad.Ops[0].Inputs[4] = NdVar::reg(x86reg::RAX, 1);
    ExpectInvalidShape(std::move(Bad));
  }
  {
    LowBlock Bad = ValidShape;
    Bad.Ops[0].MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    ExpectInvalidShape(std::move(Bad));
  }

  auto ExpectRejected = [](const std::vector<uint8_t> &Bytes) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn) !=
        static_cast<int>(Bytes.size()))
      return;
    std::vector<LowOp> Ops;
    EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  };
  ExpectRejected({0x62, 0xf3, 0x7d, 0x69, 0x66, 0xda, 0xff});
  ExpectRejected({0x62, 0xf3, 0x7d, 0x59, 0x66, 0xda, 0xff});
  ExpectRejected({0x62, 0xf3, 0x7d, 0x19, 0x67, 0xda, 0xff});
  ExpectRejected({0x62, 0xf3, 0x7d, 0xc9, 0x66, 0xda, 0xff});
  ExpectRejected({0x62, 0xf3, 0x75, 0x49, 0x66, 0xda, 0xff});
  ExpectRejected({0x62, 0xe3, 0x7d, 0x49, 0x66, 0xda, 0xff});
  ExpectRejected({0x62, 0xf3, 0x7c, 0x49, 0x66, 0xda, 0xff});
  ExpectRejected({0x67, 0x67, 0x62, 0xf3, 0x7d, 0x49, 0x66, 0x18, 0xff});
}

} // namespace
