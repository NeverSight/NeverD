//===- X86_64_EVEXPackedConvertTests.cpp - EVEX packed conversions -------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kAddress = 0x1000;

std::vector<LowOp> liftX64(const std::vector<uint8_t> &Bytes,
                           unsigned ExpectedId) {
  Decoder Dec;
  if (!Dec.init(Arch::X64)) {
    ADD_FAILURE() << "x86-64 decoder initialization failed";
    return {};
  }
  DecodedInsn Insn{};
  if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn) !=
      static_cast<int>(Bytes.size())) {
    ADD_FAILURE() << "instruction decode failed";
    return {};
  }
  if (!Insn.Raw || Insn.Raw->id != ExpectedId) {
    ADD_FAILURE() << "unexpected instruction id";
    return {};
  }
  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

template <typename T>
std::vector<uint8_t> laneBytes(const std::vector<T> &Lanes) {
  std::vector<uint8_t> Bytes(Lanes.size() * sizeof(T));
  std::memcpy(Bytes.data(), Lanes.data(), Bytes.size());
  return Bytes;
}

enum class MaskMode : uint8_t { None, Merge, Zero };

struct ConvertFamily {
  const char *Name;
  unsigned Id;
  uint8_t Opcode;
  uint8_t MandatoryPrefix;
  unsigned SourceElementSize;
  unsigned DestinationElementSize;
  bool W;
  bool Unsigned;
  bool IntegerToFloat;
};

constexpr ConvertFamily kFamilies[] = {
    {"vcvtdq2ps", X86_INS_VCVTDQ2PS, 0x5b, 0, 4, 4, false, false, true},
    {"vcvtdq2pd", X86_INS_VCVTDQ2PD, 0xe6, 2, 4, 8, false, false, true},
    {"vcvtudq2ps", X86_INS_VCVTUDQ2PS, 0x7a, 3, 4, 4, false, true, true},
    {"vcvtudq2pd", X86_INS_VCVTUDQ2PD, 0x7a, 2, 4, 8, false, true, true},
    {"vcvtqq2ps", X86_INS_VCVTQQ2PS, 0x5b, 0, 8, 4, true, false, true},
    {"vcvtqq2pd", X86_INS_VCVTQQ2PD, 0xe6, 2, 8, 8, true, false, true},
    {"vcvtuqq2ps", X86_INS_VCVTUQQ2PS, 0x7a, 3, 8, 4, true, true, true},
    {"vcvtuqq2pd", X86_INS_VCVTUQQ2PD, 0x7a, 2, 8, 8, true, true, true},
    {"vcvttps2dq", X86_INS_VCVTTPS2DQ, 0x5b, 2, 4, 4, false, false, false},
    {"vcvttpd2dq", X86_INS_VCVTTPD2DQ, 0xe6, 1, 8, 4, true, false, false},
    {"vcvttps2udq", X86_INS_VCVTTPS2UDQ, 0x78, 0, 4, 4, false, true, false},
    {"vcvttpd2udq", X86_INS_VCVTTPD2UDQ, 0x78, 0, 8, 4, true, true, false},
    {"vcvttps2qq", X86_INS_VCVTTPS2QQ, 0x7a, 1, 4, 8, false, false, false},
    {"vcvttpd2qq", X86_INS_VCVTTPD2QQ, 0x7a, 1, 8, 8, true, false, false},
    {"vcvttps2uqq", X86_INS_VCVTTPS2UQQ, 0x78, 1, 4, 8, false, true, false},
    {"vcvttpd2uqq", X86_INS_VCVTTPD2UQQ, 0x78, 1, 8, 8, true, true, false},
};

std::vector<uint8_t> registerEncoding(const ConvertFamily &Family,
                                      unsigned WideSize, MaskMode Mode) {
  const uint8_t Length = WideSize == 16 ? 0 : (WideSize == 32 ? 0x20 : 0x40);
  const uint8_t Mask = Mode == MaskMode::None ? 0 : 7;
  const uint8_t Zero = Mode == MaskMode::Zero ? 0x80 : 0;
  // Destination 30, source 29, and (when present) writemask K7.
  return {0x62,
          0x01,
          static_cast<uint8_t>(0x7c | Family.MandatoryPrefix |
                               (Family.W ? 0x80 : 0)),
          static_cast<uint8_t>(Length | 0x08 | Mask | Zero),
          Family.Opcode,
          0xf5};
}

void setRawLane(std::vector<uint8_t> &Bytes, unsigned Lane,
                unsigned ElementSize, uint64_t Value) {
  ASSERT_LE(ElementSize, sizeof(Value));
  ASSERT_LE((Lane + 1) * ElementSize, Bytes.size());
  std::memcpy(Bytes.data() + Lane * ElementSize, &Value, ElementSize);
}

uint64_t integerSourceBits(unsigned ElementSize, bool Unsigned, unsigned Lane) {
  constexpr uint64_t Signed32[] = {
      0, 1, 0xfffffffe, 0x7fffffff, 0x80000000, 0x01000001, 0xff000001, 42};
  constexpr uint64_t Unsigned32[] = {
      0, 1, 0xffffffff, 0x80000000, 0x01000001, 0x00ffffff, 0xf0000001, 42};
  constexpr uint64_t Signed64[] = {0,
                                   1,
                                   UINT64_C(0xfffffffffffffffe),
                                   UINT64_C(0x7fffffffffffffff),
                                   UINT64_C(0x8000000000000000),
                                   UINT64_C(0x0020000000000001),
                                   UINT64_C(0xff00000000000001),
                                   42};
  constexpr uint64_t Unsigned64[] = {0,
                                     1,
                                     UINT64_MAX,
                                     UINT64_C(0x8000000000000000),
                                     UINT64_C(0x0020000000000001),
                                     UINT64_C(0x001fffffffffffff),
                                     UINT64_C(0xf000000000000001),
                                     42};
  if (ElementSize == 4)
    return (Unsigned ? Unsigned32 : Signed32)[Lane % 8];
  return (Unsigned ? Unsigned64 : Signed64)[Lane % 8];
}

void setIntegerToFloatResult(std::vector<uint8_t> &Bytes, unsigned Lane,
                             const ConvertFamily &Family, uint64_t Raw) {
  if (Family.DestinationElementSize == 4) {
    const float Value =
        Family.SourceElementSize == 4
            ? (Family.Unsigned ? static_cast<float>(static_cast<uint32_t>(Raw))
                               : static_cast<float>(std::bit_cast<int32_t>(
                                     static_cast<uint32_t>(Raw))))
            : (Family.Unsigned
                   ? static_cast<float>(Raw)
                   : static_cast<float>(std::bit_cast<int64_t>(Raw)));
    std::memcpy(Bytes.data() + Lane * sizeof(Value), &Value, sizeof(Value));
    return;
  }
  const double Value =
      Family.SourceElementSize == 4
          ? (Family.Unsigned ? static_cast<double>(static_cast<uint32_t>(Raw))
                             : static_cast<double>(std::bit_cast<int32_t>(
                                   static_cast<uint32_t>(Raw))))
          : (Family.Unsigned
                 ? static_cast<double>(Raw)
                 : static_cast<double>(std::bit_cast<int64_t>(Raw)));
  std::memcpy(Bytes.data() + Lane * sizeof(Value), &Value, sizeof(Value));
}

long double setFloatSource(std::vector<uint8_t> &Bytes, unsigned Lane,
                           unsigned ElementSize) {
  const double Values[] = {
      0.0,
      1.75,
      -2.75,
      std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN(),
      std::nextafter(std::ldexp(1.0, 31), 0.0),
      std::nextafter(std::ldexp(1.0, 64), 0.0),
      -0.0,
  };
  if (ElementSize == 4) {
    const float Value = static_cast<float>(Values[Lane % 8]);
    std::memcpy(Bytes.data() + Lane * sizeof(Value), &Value, sizeof(Value));
    return static_cast<long double>(Value);
  }
  const double Value = Values[Lane % 8];
  std::memcpy(Bytes.data() + Lane * sizeof(Value), &Value, sizeof(Value));
  return static_cast<long double>(Value);
}

uint64_t truncationResult(long double Value, unsigned Bits, bool Unsigned) {
  const long double Upper = std::ldexp(1.0L, Unsigned ? Bits : Bits - 1);
  const long double Lower = Unsigned ? 0.0L : -Upper;
  if (!std::isfinite(Value) || Value < Lower || Value >= Upper)
    return Unsigned ? (Bits == 64 ? UINT64_MAX : UINT32_MAX)
                    : UINT64_C(1) << (Bits - 1);
  const long double Truncated = std::trunc(Value);
  if (Unsigned)
    return Bits == 64 ? static_cast<uint64_t>(Truncated)
                      : static_cast<uint32_t>(Truncated);
  return Bits == 64 ? static_cast<uint64_t>(static_cast<int64_t>(Truncated))
                    : static_cast<uint32_t>(static_cast<int32_t>(Truncated));
}

void expectMalformedShapeRejected(const std::vector<uint8_t> &Bytes,
                                  unsigned ExpectedId,
                                  const std::function<void(cs_x86 &)> &Mutate) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn),
            static_cast<int>(Bytes.size()));
  ASSERT_NE(Insn.Raw, nullptr);
  ASSERT_EQ(Insn.Raw->id, ExpectedId);
  ASSERT_NE(Insn.Raw->detail, nullptr);
  Mutate(Insn.Raw->detail->x86);
  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

TEST(X86EVEXPackedConvert, UnsignedQwordToDoubleHonorsZeroMask) {
  // vcvtuqq2pd zmm30 {k7} {z}, zmm29
  const std::vector<LowOp> Ops =
      liftX64({0x62, 0x01, 0xfe, 0xcf, 0x7a, 0xf5}, X86_INS_VCVTUQQ2PD);
  ASSERT_FALSE(Ops.empty());

  const std::vector<uint64_t> Source = {0,
                                        1,
                                        UINT64_C(0x7fffffffffffffff),
                                        UINT64_C(0x8000000000000000),
                                        UINT64_MAX,
                                        UINT64_C(0x0100000000000001),
                                        UINT64_C(0xf000000000000001),
                                        42};
  constexpr uint64_t Mask = UINT64_C(0xa5);
  std::vector<double> Expected(8, 0.0);
  for (unsigned Lane = 0; Lane < Source.size(); ++Lane)
    if ((Mask & (UINT64_C(1) << Lane)) != 0)
      Expected[Lane] = static_cast<double>(Source[Lane]);

  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM30);
  const RegInfo SourceReg = mapCapstoneReg(X86_REG_ZMM29);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K7);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(SourceReg.Offset, laneBytes(Source));
  Emulator.setRegister(WriteMask.Offset, Mask);
  Emulator.setRegisterBytes(Destination.Offset,
                            laneBytes(std::vector<double>(8, -1234.5)));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), laneBytes(Expected));
  EXPECT_EQ(Emulator.getRegisterBytes(SourceReg.Offset), laneBytes(Source));
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86EVEXPackedConvert, AllFamiliesWidthsAndMaskModesMatchReference) {
  constexpr unsigned WideSizes[] = {16, 32, 64};
  constexpr MaskMode MaskModes[] = {MaskMode::None, MaskMode::Merge,
                                    MaskMode::Zero};
  constexpr uint64_t Mask = UINT64_C(0xa5a5);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM30);
  const RegInfo SourceReg = mapCapstoneReg(X86_REG_ZMM29);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K7);

  for (const ConvertFamily &Family : kFamilies) {
    for (unsigned WideSize : WideSizes) {
      const unsigned LaneCount =
          WideSize /
          std::max(Family.SourceElementSize, Family.DestinationElementSize);
      for (MaskMode Mode : MaskModes) {
        SCOPED_TRACE(Family.Name);
        SCOPED_TRACE(WideSize);
        SCOPED_TRACE(static_cast<unsigned>(Mode));
        const std::vector<LowOp> Ops =
            liftX64(registerEncoding(Family, WideSize, Mode), Family.Id);
        ASSERT_FALSE(Ops.empty());

        std::vector<uint8_t> Source(64, 0xcc);
        std::vector<uint8_t> OldDestination(64, 0);
        std::vector<uint8_t> Expected(64, 0);
        for (unsigned Byte = 0; Byte < OldDestination.size(); ++Byte)
          OldDestination[Byte] = static_cast<uint8_t>(0x40 + Byte);
        for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
          const bool Active =
              Mode == MaskMode::None || (Mask & (UINT64_C(1) << Lane)) != 0;
          const size_t DestinationOffset =
              static_cast<size_t>(Lane) * Family.DestinationElementSize;
          if (!Active) {
            if (Mode == MaskMode::Merge)
              std::memcpy(Expected.data() + DestinationOffset,
                          OldDestination.data() + DestinationOffset,
                          Family.DestinationElementSize);
            continue;
          }
          if (Family.IntegerToFloat) {
            const uint64_t Raw = integerSourceBits(Family.SourceElementSize,
                                                   Family.Unsigned, Lane);
            setRawLane(Source, Lane, Family.SourceElementSize, Raw);
            setIntegerToFloatResult(Expected, Lane, Family, Raw);
          } else {
            const long double Value =
                setFloatSource(Source, Lane, Family.SourceElementSize);
            setRawLane(Expected, Lane, Family.DestinationElementSize,
                       truncationResult(Value,
                                        Family.DestinationElementSize * 8,
                                        Family.Unsigned));
          }
        }

        BinaryImage Image;
        Image.Arch = Arch::X64;
        Image.Bits = Bitness::Bits64;
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setRegisterBytes(SourceReg.Offset, Source);
        Emulator.setRegisterBytes(Destination.Offset, OldDestination);
        Emulator.setRegister(WriteMask.Offset, Mask);
        ASSERT_EQ(Emulator.run(Ops), Ops.size());
        EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), Expected);
        EXPECT_EQ(Emulator.getRegisterBytes(SourceReg.Offset), Source);
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }
}

TEST(X86EVEXPackedConvert, MXCSRRoundingControlsIntegerToFloat) {
  const std::vector<LowOp> Ops =
      liftX64({0x62, 0x01, 0xfe, 0x48, 0x7a, 0xf5}, X86_INS_VCVTUQQ2PD);
  ASSERT_FALSE(Ops.empty());
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM30);
  const RegInfo SourceReg = mapCapstoneReg(X86_REG_ZMM29);
  const std::vector<uint64_t> Source(8, UINT64_C(0x0020000000000001));

  auto Run = [&](uint32_t MXCSR) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(MXCSR);
    Emulator.setRegisterBytes(SourceReg.Offset, laneBytes(Source));
    Emulator.setRegisterBytes(Destination.Offset, std::vector<uint8_t>(64, 0));
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_FALSE(Emulator.skips().any());
    EXPECT_NE(Emulator.getMXCSR() & (1U << 5), 0U);
    return Emulator.getRegisterBytes(Destination.Offset);
  };

  const double Nearest = std::ldexp(1.0, 53);
  const double Upward =
      std::nextafter(Nearest, std::numeric_limits<double>::infinity());
  EXPECT_EQ(Run(0x1f80), laneBytes(std::vector<double>(8, Nearest)));
  EXPECT_EQ(Run(0x1f80 | (2U << 13)),
            laneBytes(std::vector<double>(8, Upward)));
}

TEST(X86EVEXPackedConvert, MaskedOffLanesSuppressConversionExceptions) {
  // vcvttpd2uqq zmm30 {k7} {z}, zmm29
  const std::vector<LowOp> Ops =
      liftX64({0x62, 0x01, 0xfd, 0xcf, 0x78, 0xf5}, X86_INS_VCVTTPD2UQQ);
  ASSERT_FALSE(Ops.empty());
  const std::vector<double> Source = {
      1.75,
      std::numeric_limits<double>::quiet_NaN(),
      -2.75,
      std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::denorm_min(),
      -std::numeric_limits<double>::infinity(),
      std::ldexp(1.0, 64),
      3.5,
  };
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM30);
  const RegInfo SourceReg = mapCapstoneReg(X86_REG_ZMM29);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K7);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(SourceReg.Offset, laneBytes(Source));
  Emulator.setRegister(WriteMask.Offset, 1);
  Emulator.setRegisterBytes(Destination.Offset, std::vector<uint8_t>(64, 0xa5));

  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset),
            laneBytes(std::vector<uint64_t>{1, 0, 0, 0, 0, 0, 0, 0}));
  // The active fractional lane raises precision. Inactive NaN, negative,
  // infinity, denormal, and overflow lanes must not add invalid/denormal.
  EXPECT_EQ(Emulator.getMXCSR() & 0x3fU, 1U << 5);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86EVEXPackedConvert, RoundingAndMemoryFormsLift) {
  static const struct {
    std::vector<uint8_t> Bytes;
    unsigned Id;
  } Cases[] = {
      {{0x62, 0x01, 0xfe, 0xdf, 0x7a, 0xf5}, X86_INS_VCVTUQQ2PD},
      {{0x62, 0x01, 0x7d, 0x9f, 0x78, 0xf5}, X86_INS_VCVTTPS2UQQ},
      {{0x62, 0x61, 0xfd, 0xcf, 0x78, 0x30}, X86_INS_VCVTTPD2UQQ},
      {{0x62, 0x61, 0xfd, 0xdf, 0x78, 0x30}, X86_INS_VCVTTPD2UQQ},
      {{0x62, 0x01, 0x7d, 0xcf, 0x5b, 0xf5}, X86_INS_VCVTPS2DQ},
  };

  for (const auto &Test : Cases) {
    SCOPED_TRACE(Test.Id);
    EXPECT_FALSE(liftX64(Test.Bytes, Test.Id).empty());
  }
}

TEST(X86EVEXPackedConvert, RawEncodingAndDecodedShapeMustAgree) {
  const std::vector<uint8_t> Encoding = {0x62, 0x01, 0xfe, 0xcf, 0x7a, 0xf5};
  expectMalformedShapeRejected(Encoding, X86_INS_VCVTUQQ2PD, [](cs_x86 &X86) {
    X86.operands[0].reg = X86_REG_ZMM0;
  });
  expectMalformedShapeRejected(Encoding, X86_INS_VCVTUQQ2PD, [](cs_x86 &X86) {
    X86.operands[1].avx_zero_opmask = false;
  });
}

} // namespace
