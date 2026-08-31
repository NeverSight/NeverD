//===- X86_64_EVEXFMATests.cpp - EVEX fused arithmetic semantics --------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kAddress = 0x1000;

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

std::vector<uint8_t> floatVector(const std::vector<float> &Lanes) {
  std::vector<uint8_t> Bytes(Lanes.size() * sizeof(float));
  std::memcpy(Bytes.data(), Lanes.data(), Bytes.size());
  return Bytes;
}

std::vector<uint8_t> doubleVector(const std::vector<double> &Lanes) {
  std::vector<uint8_t> Bytes(Lanes.size() * sizeof(double));
  std::memcpy(Bytes.data(), Lanes.data(), Bytes.size());
  return Bytes;
}

enum class FmaFamily : uint8_t { Add = 0, Sub = 2, NegAdd = 4, NegSub = 6 };
enum class FmaOrder : uint8_t { Order132, Order213, Order231 };

uint8_t fmaOpcode(FmaFamily Family, FmaOrder Order, bool Scalar) {
  uint8_t Opcode = 0x98;
  if (Order == FmaOrder::Order213)
    Opcode = 0xa8;
  else if (Order == FmaOrder::Order231)
    Opcode = 0xb8;
  return Opcode + static_cast<uint8_t>(Family) + (Scalar ? 1 : 0);
}

std::vector<uint8_t> fmaEncoding(FmaFamily Family, FmaOrder Order,
                                 bool IsDouble, size_t VectorBytes,
                                 bool Masked = false, bool ZeroMask = false) {
  uint8_t Length = VectorBytes == 16 ? 0x08 : VectorBytes == 32 ? 0x28 : 0x48;
  if (Masked)
    Length |= 0x01;
  if (ZeroMask)
    Length |= 0x80;
  return {0x62,
          0xb2,
          static_cast<uint8_t>(IsDouble ? 0xed : 0x6d),
          Length,
          fmaOpcode(Family, Order, false),
          0xc3};
}

std::vector<uint8_t> scalarFmaEncoding(FmaFamily Family, FmaOrder Order,
                                       bool IsDouble, bool Masked,
                                       bool ZeroMask) {
  uint8_t Prefix = 0x08;
  if (Masked)
    Prefix |= 0x01;
  if (ZeroMask)
    Prefix |= 0x80;
  return {0x62,
          0xb2,
          static_cast<uint8_t>(IsDouble ? 0xed : 0x6d),
          Prefix,
          fmaOpcode(Family, Order, true),
          0xc3};
}

template <typename Float>
Float applyFma(FmaFamily Family, Float A, Float B, Float C) {
  if (Family == FmaFamily::NegAdd || Family == FmaFamily::NegSub)
    A = -A;
  if (Family == FmaFamily::Sub || Family == FmaFamily::NegSub)
    C = -C;
  return std::fma(A, B, C);
}

template <typename Float>
Float expectedFma(FmaFamily Family, FmaOrder Order, Float Destination,
                  Float Source1, Float Source2) {
  switch (Order) {
  case FmaOrder::Order132:
    return applyFma(Family, Destination, Source2, Source1);
  case FmaOrder::Order213:
    return applyFma(Family, Source1, Destination, Source2);
  case FmaOrder::Order231:
    return applyFma(Family, Source1, Source2, Destination);
  }
  return Float{};
}

template <typename Float>
void expectPackedFma(FmaFamily Family, FmaOrder Order, size_t VectorBytes,
                     bool Masked = false, bool ZeroMask = false) {
  const bool IsDouble = sizeof(Float) == sizeof(double);
  const std::vector<LowOp> Ops = liftX64(
      fmaEncoding(Family, Order, IsDouble, VectorBytes, Masked, ZeroMask));
  ASSERT_FALSE(Ops.empty());

  const uint64_t Mask = IsDouble ? UINT64_C(0xa5) : UINT64_C(0xa55a);
  const size_t Lanes = 64 / sizeof(Float);
  std::vector<Float> Destination(Lanes), Source1(Lanes), Source2(Lanes);
  std::vector<Float> Expected(Lanes, Float{});
  for (size_t Lane = 0; Lane < Lanes; ++Lane) {
    Destination[Lane] = static_cast<Float>(Lane + 2);
    Source1[Lane] = static_cast<Float>(Lane * 3 + 1);
    Source2[Lane] = static_cast<Float>(Lane + 5);
    if (Lane * sizeof(Float) >= VectorBytes)
      continue;
    if (!Masked || (Mask & (UINT64_C(1) << Lane)))
      Expected[Lane] = expectedFma(Family, Order, Destination[Lane],
                                   Source1[Lane], Source2[Lane]);
    else if (!ZeroMask)
      Expected[Lane] = Destination[Lane];
  }

  const RegInfo DestinationReg = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Source1Reg = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2Reg = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo WriteMaskReg = mapCapstoneReg(X86_REG_K1);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  const auto ToBytes = [](const std::vector<Float> &Values) {
    std::vector<uint8_t> Bytes(Values.size() * sizeof(Float));
    std::memcpy(Bytes.data(), Values.data(), Bytes.size());
    return Bytes;
  };
  Emulator.setRegisterBytes(DestinationReg.Offset, ToBytes(Destination));
  Emulator.setRegisterBytes(Source1Reg.Offset, ToBytes(Source1));
  Emulator.setRegisterBytes(Source2Reg.Offset, ToBytes(Source2));
  if (Masked)
    Emulator.setRegister(WriteMaskReg.Offset, Mask);

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(DestinationReg.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, ToBytes(Expected));
}

template <typename Float>
void expectScalarFma(FmaFamily Family, FmaOrder Order, bool Masked,
                     bool ZeroMask, bool MaskBit) {
  const bool IsDouble = sizeof(Float) == sizeof(double);
  const std::vector<LowOp> Ops =
      liftX64(scalarFmaEncoding(Family, Order, IsDouble, Masked, ZeroMask));
  ASSERT_FALSE(Ops.empty());

  const size_t Lanes = 64 / sizeof(Float);
  const size_t XmmLanes = 16 / sizeof(Float);
  std::vector<Float> Destination(Lanes), Source1(Lanes), Source2(Lanes);
  std::vector<Float> Expected(Lanes, Float{});
  for (size_t Lane = 0; Lane < Lanes; ++Lane) {
    Destination[Lane] = static_cast<Float>(-1000 - static_cast<int>(Lane));
    Source1[Lane] = static_cast<Float>(100 + Lane);
    Source2[Lane] = static_cast<Float>(2 + Lane);
  }
  if (!Masked || MaskBit)
    Expected[0] =
        expectedFma(Family, Order, Destination[0], Source1[0], Source2[0]);
  else if (!ZeroMask)
    Expected[0] = Destination[0];
  for (size_t Lane = 1; Lane < XmmLanes; ++Lane)
    Expected[Lane] = Source1[Lane];

  const auto ToBytes = [](const std::vector<Float> &Values) {
    std::vector<uint8_t> Bytes(Values.size() * sizeof(Float));
    std::memcpy(Bytes.data(), Values.data(), Bytes.size());
    return Bytes;
  };
  const RegInfo DestinationReg = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Source1Reg = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2Reg = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo WriteMaskReg = mapCapstoneReg(X86_REG_K1);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(DestinationReg.Offset, ToBytes(Destination));
  Emulator.setRegisterBytes(Source1Reg.Offset, ToBytes(Source1));
  Emulator.setRegisterBytes(Source2Reg.Offset, ToBytes(Source2));
  if (Masked)
    Emulator.setRegister(WriteMaskReg.Offset, MaskBit ? 0xff : 0xfe);

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(DestinationReg.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, ToBytes(Expected));
}

TEST(X86EVEXFMA, Vfmadd132psZmmUsesEveryLaneAndSingleRounding) {
  // vfmadd132ps zmm0, zmm2, zmm19
  const std::vector<LowOp> Ops = liftX64({0x62, 0xb2, 0x6d, 0x48, 0x98, 0xc3});
  ASSERT_FALSE(Ops.empty());

  std::vector<float> Destination(16), Source1(16), Source2(16), Expected(16);
  for (size_t Lane = 0; Lane < Destination.size(); ++Lane) {
    Destination[Lane] = static_cast<float>(Lane + 2);
    Source1[Lane] = static_cast<float>(Lane * 3 + 1);
    Source2[Lane] = static_cast<float>(Lane + 5);
    Expected[Lane] = std::fma(Destination[Lane], Source2[Lane], Source1[Lane]);
  }

  Destination[0] = std::bit_cast<float>(UINT32_C(0x3f800001));
  Source2[0] = std::bit_cast<float>(UINT32_C(0x3f7fffff));
  Source1[0] = -1.0f;
  Expected[0] = std::fma(Destination[0], Source2[0], Source1[0]);
  const volatile float RoundedProduct = Destination[0] * Source2[0];
  ASSERT_NE(Expected[0], RoundedProduct + Source1[0]);

  const RegInfo DestinationReg = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Source1Reg = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2Reg = mapCapstoneReg(X86_REG_ZMM19);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(DestinationReg.Offset, floatVector(Destination));
  Emulator.setRegisterBytes(Source1Reg.Offset, floatVector(Source1));
  Emulator.setRegisterBytes(Source2Reg.Offset, floatVector(Source2));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(DestinationReg.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, floatVector(Expected));
}

TEST(X86EVEXFMA, Vfmadd132pdZmmUsesSingleRounding) {
  // vfmadd132pd zmm0, zmm2, zmm19
  const std::vector<LowOp> Ops = liftX64({0x62, 0xb2, 0xed, 0x48, 0x98, 0xc3});
  ASSERT_FALSE(Ops.empty());

  std::vector<double> Destination(8), Source1(8), Source2(8), Expected(8);
  for (size_t Lane = 0; Lane < Destination.size(); ++Lane) {
    Destination[Lane] = static_cast<double>(Lane + 2);
    Source1[Lane] = static_cast<double>(Lane * 3 + 1);
    Source2[Lane] = static_cast<double>(Lane + 5);
    Expected[Lane] = std::fma(Destination[Lane], Source2[Lane], Source1[Lane]);
  }
  Destination[0] = std::bit_cast<double>(UINT64_C(0x3ff0000000000001));
  Source2[0] = std::bit_cast<double>(UINT64_C(0x3fefffffffffffff));
  Source1[0] = -1.0;
  Expected[0] = std::fma(Destination[0], Source2[0], Source1[0]);
  const volatile double RoundedProduct = Destination[0] * Source2[0];
  ASSERT_NE(Expected[0], RoundedProduct + Source1[0]);

  const RegInfo DestinationReg = mapCapstoneReg(X86_REG_ZMM0);
  const RegInfo Source1Reg = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2Reg = mapCapstoneReg(X86_REG_ZMM19);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(DestinationReg.Offset, doubleVector(Destination));
  Emulator.setRegisterBytes(Source1Reg.Offset, doubleVector(Source1));
  Emulator.setRegisterBytes(Source2Reg.Offset, doubleVector(Source2));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(DestinationReg.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, doubleVector(Expected));
}

TEST(X86EVEXFMA, PackedOrdersSignsTypesAndWidthsUseTheEncodedThreeSources) {
  constexpr FmaFamily Families[] = {
      FmaFamily::Add,
      FmaFamily::Sub,
      FmaFamily::NegAdd,
      FmaFamily::NegSub,
  };
  constexpr FmaOrder Orders[] = {
      FmaOrder::Order132,
      FmaOrder::Order213,
      FmaOrder::Order231,
  };
  constexpr size_t Widths[] = {16, 32, 64};

  for (FmaFamily Family : Families)
    for (FmaOrder Order : Orders)
      for (size_t Width : Widths) {
        SCOPED_TRACE(testing::Message()
                     << "family=" << static_cast<unsigned>(Family) << " order="
                     << static_cast<unsigned>(Order) << " bytes=" << Width);
        expectPackedFma<float>(Family, Order, Width);
        expectPackedFma<double>(Family, Order, Width);
      }
}

TEST(X86EVEXFMA, PackedMergeAndZeroMasksApplyAfterEveryFusedOperation) {
  constexpr FmaFamily Families[] = {
      FmaFamily::Add,
      FmaFamily::Sub,
      FmaFamily::NegAdd,
      FmaFamily::NegSub,
  };
  constexpr FmaOrder Orders[] = {
      FmaOrder::Order132,
      FmaOrder::Order213,
      FmaOrder::Order231,
  };
  constexpr size_t Widths[] = {16, 32, 64};

  for (FmaFamily Family : Families)
    for (FmaOrder Order : Orders)
      for (size_t Width : Widths)
        for (bool ZeroMask : {false, true}) {
          SCOPED_TRACE(testing::Message()
                       << "family=" << static_cast<unsigned>(Family)
                       << " order=" << static_cast<unsigned>(Order)
                       << " bytes=" << Width << " zero=" << ZeroMask);
          expectPackedFma<float>(Family, Order, Width, true, ZeroMask);
          expectPackedFma<double>(Family, Order, Width, true, ZeroMask);
        }
}

TEST(X86EVEXFMA,
     ScalarOrdersSignsAndMasksPreserveSource1XmmAndClearUpperVectorState) {
  constexpr FmaFamily Families[] = {
      FmaFamily::Add,
      FmaFamily::Sub,
      FmaFamily::NegAdd,
      FmaFamily::NegSub,
  };
  constexpr FmaOrder Orders[] = {
      FmaOrder::Order132,
      FmaOrder::Order213,
      FmaOrder::Order231,
  };

  for (FmaFamily Family : Families)
    for (FmaOrder Order : Orders) {
      SCOPED_TRACE(testing::Message()
                   << "family=" << static_cast<unsigned>(Family)
                   << " order=" << static_cast<unsigned>(Order));
      expectScalarFma<float>(Family, Order, false, false, true);
      expectScalarFma<double>(Family, Order, false, false, true);
      for (bool ZeroMask : {false, true})
        for (bool MaskBit : {false, true}) {
          SCOPED_TRACE(testing::Message()
                       << "zero=" << ZeroMask << " bit0=" << MaskBit);
          expectScalarFma<float>(Family, Order, true, ZeroMask, MaskBit);
          expectScalarFma<double>(Family, Order, true, ZeroMask, MaskBit);
        }
    }
}

TEST(X86EVEXFMA,
     UnsupportedEmbeddedRoundingFailsWithoutPartialLowIR) {
  // vfnmadd231pd zmm0, zmm2, zmm19, {rn-sae}
  expectStrictlyUnlifted({0x62, 0xb2, 0xed, 0x18, 0xbc, 0xc3});
  // vfnmsub132ss xmm0 {k1}, xmm2, xmm19, {rz-sae}
  expectStrictlyUnlifted({0x62, 0xb2, 0x6d, 0x79, 0x9f, 0xc3});
}

} // namespace
