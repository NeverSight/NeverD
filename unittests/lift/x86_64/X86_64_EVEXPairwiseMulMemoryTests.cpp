//===- X86_64_EVEXPairwiseMulMemoryTests.cpp - EVEX pairwise multiply ----===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;

BinaryImage emptyImage() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  return Image;
}

void addReadableBytes(BinaryImage &Image, uint64_t Address, const void *Data,
                      size_t Size) {
  Segment Memory;
  Memory.VA = Address;
  Memory.Size = Size;
  Memory.Flags = SegmentFlags::Readable;
  Memory.Data.resize(Size);
  std::memcpy(Memory.Data.data(), Data, Size);
  Image.Segments.push_back(std::move(Memory));
}

std::vector<LowOp> liftX64(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  if (!Dec.init(Arch::X64)) {
    ADD_FAILURE() << "failed to initialize x86-64 decoder";
    return {};
  }

  DecodedInsn Insn{};
  if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kInstructionAddress,
                           Insn) != static_cast<int>(Bytes.size())) {
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

template <typename Value>
void setLane(std::vector<uint8_t> &Bytes, unsigned Lane, Value LaneValue) {
  ASSERT_LE((Lane + 1) * sizeof(Value), Bytes.size());
  std::memcpy(Bytes.data() + Lane * sizeof(Value), &LaneValue, sizeof(Value));
}

template <typename Mutator>
void expectMutatedLiftFailsClosed(const std::vector<uint8_t> &Bytes,
                                  Mutator Mutate) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                 kInstructionAddress, Insn),
            static_cast<int>(Bytes.size()));
  ASSERT_NE(Insn.Raw, nullptr);
  ASSERT_NE(Insn.Raw->detail, nullptr);
  ASSERT_TRUE(Mutate(*Insn.Raw, Insn.Raw->detail->x86));

  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

enum class PairwiseKind {
  MaddWords,
  MaddUnsignedSignedBytes,
  MulHighRoundWords,
};

void fillPairwiseInputs(PairwiseKind Kind, std::vector<uint8_t> &Left,
                        std::vector<uint8_t> &Right) {
  if (Kind == PairwiseKind::MaddWords) {
    for (unsigned Lane = 0; Lane < 16; ++Lane) {
      setLane(Left, Lane * 2, static_cast<int16_t>(Lane + 1));
      setLane(Left, Lane * 2 + 1, static_cast<int16_t>(-2 * (Lane + 1)));
      setLane(Right, Lane * 2, static_cast<int16_t>(3));
      setLane(Right, Lane * 2 + 1, static_cast<int16_t>(-5));
    }
    return;
  }

  if (Kind == PairwiseKind::MaddUnsignedSignedBytes) {
    for (unsigned Lane = 0; Lane < 32; ++Lane) {
      setLane(Left, Lane * 2, static_cast<uint8_t>(Lane + 1));
      setLane(Left, Lane * 2 + 1, static_cast<uint8_t>(Lane + 3));
      setLane(Right, Lane * 2, static_cast<int8_t>(-3));
      setLane(Right, Lane * 2 + 1, static_cast<int8_t>(5));
    }
    return;
  }

  for (unsigned Lane = 0; Lane < 32; ++Lane) {
    setLane(Left, Lane, static_cast<int16_t>(1000 + Lane * 137));
    setLane(Right, Lane, static_cast<int16_t>(-16000 + Lane * 211));
  }
}

uint32_t pairwiseResult(PairwiseKind Kind, const uint8_t *Left,
                        const uint8_t *Right) {
  if (Kind == PairwiseKind::MaddWords) {
    int16_t L0, L1, R0, R1;
    std::memcpy(&L0, Left, sizeof(L0));
    std::memcpy(&L1, Left + 2, sizeof(L1));
    std::memcpy(&R0, Right, sizeof(R0));
    std::memcpy(&R1, Right + 2, sizeof(R1));
    const int64_t Sum =
        static_cast<int64_t>(L0) * R0 + static_cast<int64_t>(L1) * R1;
    return static_cast<uint32_t>(Sum);
  }

  if (Kind == PairwiseKind::MaddUnsignedSignedBytes) {
    const int32_t Sum =
        static_cast<int32_t>(Left[0]) * static_cast<int8_t>(Right[0]) +
        static_cast<int32_t>(Left[1]) * static_cast<int8_t>(Right[1]);
    return static_cast<uint16_t>(
        static_cast<int16_t>(std::clamp(Sum, static_cast<int32_t>(INT16_MIN),
                                        static_cast<int32_t>(INT16_MAX))));
  }

  int16_t L, R;
  std::memcpy(&L, Left, sizeof(L));
  std::memcpy(&R, Right, sizeof(R));
  const int32_t Product = static_cast<int32_t>(L) * R;
  return static_cast<uint16_t>(
      static_cast<int16_t>((Product + INT32_C(0x4000)) >> 15));
}

TEST(X86EVEXPairwiseMulMemory,
     VpmaddwdMaskedRegisterUsesEncodedSourcesAndMergeMask) {
  // vpmaddwd zmm30 {k7}, zmm28, zmm29
  const std::vector<LowOp> Ops = liftX64({0x62, 0x01, 0x1d, 0x47, 0xf5, 0xf5});
  ASSERT_FALSE(Ops.empty());

  constexpr uint16_t ActiveLanes = UINT16_C(0x8489);
  std::vector<uint8_t> Destination(64);
  std::vector<uint8_t> Left(64);
  std::vector<uint8_t> Right(64);
  std::vector<uint8_t> Expected(64);
  for (unsigned Lane = 0; Lane < 16; ++Lane) {
    const uint32_t OldValue = UINT32_C(0xa0000000) + Lane;
    setLane(Destination, Lane, OldValue);
    setLane(Expected, Lane, OldValue);

    const int16_t LeftLow = static_cast<int16_t>(Lane + 1);
    const int16_t LeftHigh = static_cast<int16_t>(-2 * (Lane + 1));
    const int16_t RightLow = 3;
    const int16_t RightHigh = -5;
    setLane(Left, Lane * 2, LeftLow);
    setLane(Left, Lane * 2 + 1, LeftHigh);
    setLane(Right, Lane * 2, RightLow);
    setLane(Right, Lane * 2 + 1, RightHigh);

    if ((ActiveLanes & (UINT16_C(1) << Lane)) != 0) {
      const uint32_t Result =
          static_cast<uint32_t>(static_cast<int32_t>(LeftLow) * RightLow) +
          static_cast<uint32_t>(static_cast<int32_t>(LeftHigh) * RightHigh);
      setLane(Expected, Lane, Result);
    }
  }

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(x86reg::XMM30, Destination);
  Emulator.setRegisterBytes(x86reg::XMM28, Left);
  Emulator.setRegisterBytes(x86reg::XMM29, Right);
  Emulator.setRegister(x86reg::K7, ActiveLanes);

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM30), Expected);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86EVEXPairwiseMulMemory,
     VpmaddubswMaskedRegisterSaturatesAndZeroMasksPerWord) {
  // vpmaddubsw zmm30 {k7} {z}, zmm28, zmm29
  const std::vector<LowOp> Ops = liftX64({0x62, 0x02, 0x1d, 0xc7, 0x04, 0xf5});
  ASSERT_FALSE(Ops.empty());

  constexpr uint32_t ActiveLanes = UINT32_C(0x84210841);
  std::vector<uint8_t> Destination(64, 0xa5);
  std::vector<uint8_t> Left(64);
  std::vector<uint8_t> Right(64);
  std::vector<uint8_t> Expected(64);
  for (unsigned Lane = 0; Lane < 32; ++Lane) {
    uint8_t LeftLow = static_cast<uint8_t>(Lane + 1);
    uint8_t LeftHigh = static_cast<uint8_t>(Lane + 3);
    int8_t RightLow = -3;
    int8_t RightHigh = 5;
    if (Lane % 3 == 0) {
      LeftLow = UINT8_MAX;
      LeftHigh = UINT8_MAX;
      RightLow = INT8_MAX;
      RightHigh = INT8_MAX;
    } else if (Lane % 3 == 1) {
      LeftLow = UINT8_MAX;
      LeftHigh = UINT8_MAX;
      RightLow = INT8_MIN;
      RightHigh = INT8_MIN;
    }
    setLane(Left, Lane * 2, LeftLow);
    setLane(Left, Lane * 2 + 1, LeftHigh);
    setLane(Right, Lane * 2, RightLow);
    setLane(Right, Lane * 2 + 1, RightHigh);

    if ((ActiveLanes & (UINT32_C(1) << Lane)) != 0) {
      const int32_t Sum = static_cast<int32_t>(LeftLow) * RightLow +
                          static_cast<int32_t>(LeftHigh) * RightHigh;
      const int16_t Result =
          static_cast<int16_t>(std::clamp(Sum, static_cast<int32_t>(INT16_MIN),
                                          static_cast<int32_t>(INT16_MAX)));
      setLane(Expected, Lane, Result);
    }
  }

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(x86reg::XMM30, Destination);
  Emulator.setRegisterBytes(x86reg::XMM28, Left);
  Emulator.setRegisterBytes(x86reg::XMM29, Right);
  Emulator.setRegister(x86reg::K7, ActiveLanes);

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM30), Expected);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86EVEXPairwiseMulMemory, VpmulhrswMaskedRegisterRoundsAndMergesPerWord) {
  // vpmulhrsw zmm30 {k7}, zmm28, zmm29
  const std::vector<LowOp> Ops = liftX64({0x62, 0x02, 0x1d, 0x47, 0x0b, 0xf5});
  ASSERT_FALSE(Ops.empty());

  constexpr uint32_t ActiveLanes = UINT32_C(0x84210841);
  std::vector<uint8_t> Destination(64);
  std::vector<uint8_t> Left(64);
  std::vector<uint8_t> Right(64);
  std::vector<uint8_t> Expected(64);
  for (unsigned Lane = 0; Lane < 32; ++Lane) {
    const int16_t OldValue = static_cast<int16_t>(-2000 - Lane);
    const int16_t LeftValue =
        Lane == 0 ? INT16_MIN : static_cast<int16_t>(1000 + Lane * 137);
    const int16_t RightValue =
        Lane == 0 ? INT16_MIN : static_cast<int16_t>(-16000 + Lane * 211);
    setLane(Destination, Lane, OldValue);
    setLane(Expected, Lane, OldValue);
    setLane(Left, Lane, LeftValue);
    setLane(Right, Lane, RightValue);
    if ((ActiveLanes & (UINT32_C(1) << Lane)) != 0) {
      const int32_t Product =
          static_cast<int32_t>(LeftValue) * static_cast<int32_t>(RightValue);
      const int16_t Result =
          static_cast<int16_t>((Product + INT32_C(0x4000)) >> 15);
      setLane(Expected, Lane, Result);
    }
  }

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(x86reg::XMM30, Destination);
  Emulator.setRegisterBytes(x86reg::XMM28, Left);
  Emulator.setRegisterBytes(x86reg::XMM29, Right);
  Emulator.setRegister(x86reg::K7, ActiveLanes);

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM30), Expected);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86EVEXPairwiseMulMemory,
     SparseMaskedMemoryReadsOnlyActiveOutputGroupsAndMerges) {
  struct TestCase {
    PairwiseKind Kind;
    std::vector<uint8_t> Encoding;
    uint16_t OutputElementSize;
  };
  const std::vector<TestCase> Cases = {
      {PairwiseKind::MaddWords,
       {0x64, 0x67, 0x62, 0x61, 0x1d, 0x47, 0xf5, 0x70, 0x02},
       4},
      {PairwiseKind::MaddUnsignedSignedBytes,
       {0x64, 0x67, 0x62, 0x62, 0x1d, 0x47, 0x04, 0x70, 0x02},
       2},
      {PairwiseKind::MulHighRoundWords,
       {0x64, 0x67, 0x62, 0x62, 0x1d, 0x47, 0x0b, 0x70, 0x02},
       2},
  };

  constexpr uint64_t FsBase = UINT64_C(0x100000000);
  constexpr uint32_t Eax = UINT32_C(0xfffff000);
  constexpr uint64_t LinearAddress = FsBase + UINT64_C(0xfffff080);
  for (const TestCase &Case : Cases) {
    SCOPED_TRACE(static_cast<unsigned>(Case.Kind));
    const std::vector<LowOp> Ops = liftX64(Case.Encoding);
    ASSERT_FALSE(Ops.empty());

    const unsigned LaneCount = 64 / Case.OutputElementSize;
    const uint64_t Mask = UINT64_C(1) | (UINT64_C(1) << (LaneCount - 1));
    std::vector<uint8_t> Destination(64, 0xa5);
    std::vector<uint8_t> Left(64);
    std::vector<uint8_t> Right(64);
    std::vector<uint8_t> Expected = Destination;
    fillPairwiseInputs(Case.Kind, Left, Right);

    BinaryImage Image = emptyImage();
    for (unsigned Lane : {0u, LaneCount - 1}) {
      const unsigned Offset = Lane * Case.OutputElementSize;
      addReadableBytes(Image, LinearAddress + Offset, Right.data() + Offset,
                       Case.OutputElementSize);
      const uint32_t Result = pairwiseResult(Case.Kind, Left.data() + Offset,
                                             Right.data() + Offset);
      if (Case.OutputElementSize == 4)
        setLane(Expected, Lane, Result);
      else
        setLane(Expected, Lane, static_cast<uint16_t>(Result));
    }

    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS,
                                                   FsBase));
    Emulator.setRegister(x86reg::RAX, UINT64_C(0xaaaaaaaa00000000) | Eax);
    Emulator.setRegister(x86reg::K7, Mask);
    Emulator.setRegisterBytes(x86reg::XMM28, Left);
    Emulator.setRegisterBytes(x86reg::XMM30, Destination);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM30), Expected);
    ASSERT_EQ(Emulator.getLoadRecords().size(), 2u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, LinearAddress);
    EXPECT_EQ(Emulator.getLoadRecords()[1].Addr,
              LinearAddress + (LaneCount - 1) * Case.OutputElementSize);
    for (const auto &Load : Emulator.getLoadRecords())
      EXPECT_EQ(Load.Size, Case.OutputElementSize);
    EXPECT_FALSE(Emulator.skips().any());

    NdOpEmulator Suppressed(emptyImage());
    Suppressed.setStrictMode(true);
    Suppressed.setLoadCollect(true);
    ASSERT_TRUE(Suppressed.setMemoryAddressSpaceBase(
        NdMemoryAddressSpace::X86FS, FsBase));
    Suppressed.setRegister(x86reg::RAX, UINT64_C(0xaaaaaaaa00000000) | Eax);
    Suppressed.setRegister(x86reg::K7, 0);
    Suppressed.setRegisterBytes(x86reg::XMM28, Left);
    Suppressed.setRegisterBytes(x86reg::XMM30, Destination);
    ASSERT_EQ(Suppressed.run(Ops), Ops.size());
    EXPECT_EQ(Suppressed.getRegisterBytes(x86reg::XMM30), Destination);
    EXPECT_TRUE(Suppressed.getLoadRecords().empty());
    EXPECT_FALSE(Suppressed.skips().any());
  }
}

TEST(X86EVEXPairwiseMulMemory,
     XmmZeroMaskSuppressesUnmappedAndAppliesEvexUpperZeroing) {
  struct TestCase {
    std::vector<uint8_t> Encoding;
    bool ZeroMasked;
  };
  const std::vector<TestCase> Cases = {
      {{0x62, 0x61, 0x1d, 0x07, 0xf5, 0x30}, false},
      {{0x62, 0x62, 0x1d, 0x87, 0x04, 0x30}, true},
      {{0x62, 0x62, 0x1d, 0x07, 0x0b, 0x30}, false},
  };

  for (const TestCase &Case : Cases) {
    const std::vector<LowOp> Ops = liftX64(Case.Encoding);
    ASSERT_FALSE(Ops.empty());
    std::vector<uint8_t> Destination(64, 0xcc);
    std::vector<uint8_t> Expected(64, 0);
    if (!Case.ZeroMasked)
      std::copy_n(Destination.begin(), 16, Expected.begin());

    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, UINT64_C(0x9000));
    Emulator.setRegister(x86reg::K7, 0);
    Emulator.setRegisterBytes(x86reg::XMM28, std::vector<uint8_t>(64, 0x31));
    Emulator.setRegisterBytes(x86reg::XMM30, Destination);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM30), Expected);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86EVEXPairwiseMulMemory,
     RawAndDecodedContractsFailClosedBeforePartialLowIR) {
  const std::vector<std::vector<uint8_t>> Encodings = {
      {0x64, 0x67, 0x62, 0x61, 0x1d, 0x47, 0xf5, 0x70, 0x02},
      {0x64, 0x67, 0x62, 0x62, 0x1d, 0x47, 0x04, 0x70, 0x02},
      {0x64, 0x67, 0x62, 0x62, 0x1d, 0x47, 0x0b, 0x70, 0x02},
  };
  for (const std::vector<uint8_t> &Encoding : Encodings) {
    expectMutatedLiftFailsClosed(Encoding, [](cs_insn &Insn, cs_x86 &) {
      Insn.bytes[6] ^= 0x01;
      return true;
    });
    expectMutatedLiftFailsClosed(Encoding, [](cs_insn &Insn, cs_x86 &) {
      Insn.bytes[8] = 0x03;
      return true;
    });
    expectMutatedLiftFailsClosed(Encoding, [](cs_insn &Insn, cs_x86 &) {
      Insn.bytes[0] = 0x65;
      return true;
    });
    expectMutatedLiftFailsClosed(Encoding, [](cs_insn &Insn, cs_x86 &) {
      Insn.bytes[1] = 0x66;
      return true;
    });
    expectMutatedLiftFailsClosed(Encoding, [](cs_insn &, cs_x86 &X86) {
      for (uint8_t Index = 0; Index < X86.op_count; ++Index) {
        if (X86.operands[Index].type != X86_OP_MEM)
          continue;
        X86.operands[Index].size = 32;
        return true;
      }
      return false;
    });
    expectMutatedLiftFailsClosed(Encoding, [](cs_insn &, cs_x86 &X86) {
      X86.avx_sae = true;
      return true;
    });
  }

  expectMutatedLiftFailsClosed({0x62, 0x01, 0x1d, 0x47, 0xf5, 0xf5},
                               [](cs_insn &Insn, cs_x86 &) {
                                 Insn.bytes[3] |= 0x10;
                                 return true;
                               });
}

} // namespace
