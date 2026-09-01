//===- X86_64_EVEXFloatArithTests.cpp - EVEX float arithmetic semantics -===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
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

void expectExactlyOneFPArith(const std::vector<uint8_t> &Bytes) {
  const std::vector<LowOp> Ops = liftX64(Bytes);
  ASSERT_FALSE(Ops.empty());
  EXPECT_EQ(
      std::count_if(Ops.begin(), Ops.end(),
                    [](const LowOp &Op) {
                      return Op.Opcode == NdOp::INTRINSIC &&
                             Op.NumInputs != 0 && Op.Inputs[0].isConst() &&
                             Op.Inputs[0].Offset ==
                                 static_cast<uint64_t>(Intrinsic::X86FPArith);
                    }),
      1);
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

std::vector<uint8_t> dwordBitsVector(const std::vector<uint32_t> &Lanes) {
  std::vector<uint8_t> Bytes(Lanes.size() * sizeof(uint32_t));
  std::memcpy(Bytes.data(), Lanes.data(), Bytes.size());
  return Bytes;
}

std::vector<uint8_t> qwordBitsVector(const std::vector<uint64_t> &Lanes) {
  std::vector<uint8_t> Bytes(Lanes.size() * sizeof(uint64_t));
  std::memcpy(Bytes.data(), Lanes.data(), Bytes.size());
  return Bytes;
}

enum class PackedOperation : uint8_t {
  Add = 0x58,
  Sub = 0x5c,
  Mul = 0x59,
  Div = 0x5e
};

float apply(PackedOperation Operation, float Left, float Right) {
  switch (Operation) {
  case PackedOperation::Add:
    return Left + Right;
  case PackedOperation::Sub:
    return Left - Right;
  case PackedOperation::Mul:
    return Left * Right;
  case PackedOperation::Div:
    return Left / Right;
  }
  return 0.0f;
}

double apply(PackedOperation Operation, double Left, double Right) {
  switch (Operation) {
  case PackedOperation::Add:
    return Left + Right;
  case PackedOperation::Sub:
    return Left - Right;
  case PackedOperation::Mul:
    return Left * Right;
  case PackedOperation::Div:
    return Left / Right;
  }
  return 0.0;
}

void expectPackedPs(const std::vector<uint8_t> &Encoding, size_t VectorBytes,
                    PackedOperation Operation) {
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());

  std::vector<float> Left(16), Right(16), Expected(16, 0.0f);
  for (size_t Lane = 0; Lane < Left.size(); ++Lane) {
    Left[Lane] = static_cast<float>((Lane + 1) * 12);
    Right[Lane] = static_cast<float>((Lane % 4) + 1);
    if (Lane * sizeof(float) < VectorBytes)
      Expected[Lane] = apply(Operation, Left[Lane], Right[Lane]);
  }

  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source1.Offset, floatVector(Left));
  Emulator.setRegisterBytes(Source2.Offset, floatVector(Right));
  Emulator.setRegisterBytes(Destination.Offset, std::vector<uint8_t>(64, 0xa5));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, floatVector(Expected));
}

void expectPackedPd(const std::vector<uint8_t> &Encoding, size_t VectorBytes,
                    PackedOperation Operation) {
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());

  std::vector<double> Left(8), Right(8), Expected(8, 0.0);
  for (size_t Lane = 0; Lane < Left.size(); ++Lane) {
    Left[Lane] = static_cast<double>((Lane + 1) * 12);
    Right[Lane] = static_cast<double>((Lane % 4) + 1);
    if (Lane * sizeof(double) < VectorBytes)
      Expected[Lane] = apply(Operation, Left[Lane], Right[Lane]);
  }

  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source1.Offset, doubleVector(Left));
  Emulator.setRegisterBytes(Source2.Offset, doubleVector(Right));
  Emulator.setRegisterBytes(Destination.Offset, std::vector<uint8_t>(64, 0xa5));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, doubleVector(Expected));
}

void expectMaskedPs(const std::vector<uint8_t> &Encoding, size_t VectorBytes,
                    PackedOperation Operation, bool ZeroMask) {
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Mask = UINT64_C(0xa55a);
  std::vector<float> Left(16), Right(16), Destination(16), Expected(16, 0.0f);
  for (size_t Lane = 0; Lane < Left.size(); ++Lane) {
    Left[Lane] = static_cast<float>((Lane + 1) * 12);
    Right[Lane] = static_cast<float>((Lane % 4) + 1);
    Destination[Lane] = static_cast<float>(-1000 - Lane);
    if (Lane * sizeof(float) >= VectorBytes)
      continue;
    if (Mask & (UINT64_C(1) << Lane))
      Expected[Lane] = apply(Operation, Left[Lane], Right[Lane]);
    else if (!ZeroMask)
      Expected[Lane] = Destination[Lane];
  }

  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K1);
  const RegInfo DestinationReg = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source1.Offset, floatVector(Left));
  Emulator.setRegisterBytes(Source2.Offset, floatVector(Right));
  Emulator.setRegister(WriteMask.Offset, Mask);
  Emulator.setRegisterBytes(DestinationReg.Offset, floatVector(Destination));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(DestinationReg.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, floatVector(Expected));
}

void expectMaskedPd(const std::vector<uint8_t> &Encoding, size_t VectorBytes,
                    PackedOperation Operation, bool ZeroMask) {
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Mask = UINT64_C(0xa5);
  std::vector<double> Left(8), Right(8), Destination(8), Expected(8, 0.0);
  for (size_t Lane = 0; Lane < Left.size(); ++Lane) {
    Left[Lane] = static_cast<double>((Lane + 1) * 12);
    Right[Lane] = static_cast<double>((Lane % 4) + 1);
    Destination[Lane] = static_cast<double>(-1000 - Lane);
    if (Lane * sizeof(double) >= VectorBytes)
      continue;
    if (Mask & (UINT64_C(1) << Lane))
      Expected[Lane] = apply(Operation, Left[Lane], Right[Lane]);
    else if (!ZeroMask)
      Expected[Lane] = Destination[Lane];
  }

  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K1);
  const RegInfo DestinationReg = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source1.Offset, doubleVector(Left));
  Emulator.setRegisterBytes(Source2.Offset, doubleVector(Right));
  Emulator.setRegister(WriteMask.Offset, Mask);
  Emulator.setRegisterBytes(DestinationReg.Offset, doubleVector(Destination));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(DestinationReg.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, doubleVector(Expected));
}

void expectSqrtPs(const std::vector<uint8_t> &Encoding, size_t VectorBytes,
                  bool Masked, bool ZeroMask) {
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Mask = UINT64_C(0xa55a);
  std::vector<float> Input(16), Destination(16), Expected(16, 0.0f);
  for (size_t Lane = 0; Lane < Input.size(); ++Lane) {
    Input[Lane] =
        Lane == 0 ? -0.0f : static_cast<float>((Lane + 1) * (Lane + 1));
    Destination[Lane] = static_cast<float>(-1000 - Lane);
    if (Lane * sizeof(float) >= VectorBytes)
      continue;
    if (!Masked || (Mask & (UINT64_C(1) << Lane)))
      Expected[Lane] = Lane == 0 ? -0.0f : static_cast<float>(Lane + 1);
    else if (!ZeroMask)
      Expected[Lane] = Destination[Lane];
  }

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K1);
  const RegInfo DestinationReg = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source.Offset, floatVector(Input));
  if (Masked)
    Emulator.setRegister(WriteMask.Offset, Mask);
  Emulator.setRegisterBytes(DestinationReg.Offset, floatVector(Destination));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(DestinationReg.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, floatVector(Expected));
}

void expectSqrtPd(const std::vector<uint8_t> &Encoding, size_t VectorBytes,
                  bool Masked, bool ZeroMask) {
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Mask = UINT64_C(0xa5);
  std::vector<double> Input(8), Destination(8), Expected(8, 0.0);
  for (size_t Lane = 0; Lane < Input.size(); ++Lane) {
    Input[Lane] =
        Lane == 0 ? -0.0 : static_cast<double>((Lane + 1) * (Lane + 1));
    Destination[Lane] = static_cast<double>(-1000 - Lane);
    if (Lane * sizeof(double) >= VectorBytes)
      continue;
    if (!Masked || (Mask & (UINT64_C(1) << Lane)))
      Expected[Lane] = Lane == 0 ? -0.0 : static_cast<double>(Lane + 1);
    else if (!ZeroMask)
      Expected[Lane] = Destination[Lane];
  }

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K1);
  const RegInfo DestinationReg = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source.Offset, doubleVector(Input));
  if (Masked)
    Emulator.setRegister(WriteMask.Offset, Mask);
  Emulator.setRegisterBytes(DestinationReg.Offset, doubleVector(Destination));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(DestinationReg.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, doubleVector(Expected));
}

void expectMinMaxPs(const std::vector<uint8_t> &Encoding, size_t VectorBytes,
                    bool IsMin, bool Masked, bool ZeroMask) {
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Mask = UINT64_C(0xa55a);
  std::vector<float> Left(16), Right(16), Destination(16), Expected(16, 0.0f);
  for (size_t Lane = 0; Lane < Left.size(); ++Lane) {
    Left[Lane] = static_cast<float>(Lane + 1);
    Right[Lane] = static_cast<float>(16 - Lane);
    Destination[Lane] = static_cast<float>(-1000 - Lane);
    if (Lane * sizeof(float) >= VectorBytes)
      continue;
    if (!Masked || (Mask & (UINT64_C(1) << Lane))) {
      Expected[Lane] =
          IsMin ? (Left[Lane] < Right[Lane] ? Left[Lane] : Right[Lane])
                : (Right[Lane] < Left[Lane] ? Left[Lane] : Right[Lane]);
    } else if (!ZeroMask) {
      Expected[Lane] = Destination[Lane];
    }
  }

  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K1);
  const RegInfo DestinationReg = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source1.Offset, floatVector(Left));
  Emulator.setRegisterBytes(Source2.Offset, floatVector(Right));
  if (Masked)
    Emulator.setRegister(WriteMask.Offset, Mask);
  Emulator.setRegisterBytes(DestinationReg.Offset, floatVector(Destination));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(DestinationReg.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, floatVector(Expected));
}

void expectMinMaxPd(const std::vector<uint8_t> &Encoding, size_t VectorBytes,
                    bool IsMin, bool Masked, bool ZeroMask) {
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Mask = UINT64_C(0xa5);
  std::vector<double> Left(8), Right(8), Destination(8), Expected(8, 0.0);
  for (size_t Lane = 0; Lane < Left.size(); ++Lane) {
    Left[Lane] = static_cast<double>(Lane + 1);
    Right[Lane] = static_cast<double>(8 - Lane);
    Destination[Lane] = static_cast<double>(-1000 - Lane);
    if (Lane * sizeof(double) >= VectorBytes)
      continue;
    if (!Masked || (Mask & (UINT64_C(1) << Lane))) {
      Expected[Lane] =
          IsMin ? (Left[Lane] < Right[Lane] ? Left[Lane] : Right[Lane])
                : (Right[Lane] < Left[Lane] ? Left[Lane] : Right[Lane]);
    } else if (!ZeroMask) {
      Expected[Lane] = Destination[Lane];
    }
  }

  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K1);
  const RegInfo DestinationReg = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source1.Offset, doubleVector(Left));
  Emulator.setRegisterBytes(Source2.Offset, doubleVector(Right));
  if (Masked)
    Emulator.setRegister(WriteMask.Offset, Mask);
  Emulator.setRegisterBytes(DestinationReg.Offset, doubleVector(Destination));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(DestinationReg.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, doubleVector(Expected));
}

void expectScalarSs(const std::vector<uint8_t> &Encoding,
                    PackedOperation Operation, bool MaskBit, bool ZeroMask) {
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());

  std::vector<float> Source1(16), Source2(16), Destination(16), Expected(16);
  for (size_t Lane = 0; Lane < Source1.size(); ++Lane) {
    Source1[Lane] = static_cast<float>(12 + Lane);
    Source2[Lane] = static_cast<float>(3 + Lane);
    Destination[Lane] = static_cast<float>(100 + Lane);
  }
  if (MaskBit)
    Expected[0] = apply(Operation, Source1[0], Source2[0]);
  else if (!ZeroMask)
    Expected[0] = Destination[0];
  Expected[1] = Source1[1];
  Expected[2] = Source1[2];
  Expected[3] = Source1[3];

  const RegInfo Source1Reg = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2Reg = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K1);
  const RegInfo DestinationReg = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source1Reg.Offset, floatVector(Source1));
  Emulator.setRegisterBytes(Source2Reg.Offset, floatVector(Source2));
  Emulator.setRegister(WriteMask.Offset,
                       MaskBit ? UINT64_C(0xff) : UINT64_C(0xfe));
  Emulator.setRegisterBytes(DestinationReg.Offset, floatVector(Destination));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(DestinationReg.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, floatVector(Expected));
}

void expectScalarSd(const std::vector<uint8_t> &Encoding,
                    PackedOperation Operation, bool MaskBit, bool ZeroMask) {
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());

  std::vector<double> Source1(8), Source2(8), Destination(8), Expected(8);
  for (size_t Lane = 0; Lane < Source1.size(); ++Lane) {
    Source1[Lane] = static_cast<double>(12 + Lane);
    Source2[Lane] = static_cast<double>(3 + Lane);
    Destination[Lane] = static_cast<double>(100 + Lane);
  }
  if (MaskBit)
    Expected[0] = apply(Operation, Source1[0], Source2[0]);
  else if (!ZeroMask)
    Expected[0] = Destination[0];
  Expected[1] = Source1[1];

  const RegInfo Source1Reg = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2Reg = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K1);
  const RegInfo DestinationReg = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source1Reg.Offset, doubleVector(Source1));
  Emulator.setRegisterBytes(Source2Reg.Offset, doubleVector(Source2));
  Emulator.setRegister(WriteMask.Offset,
                       MaskBit ? UINT64_C(0xff) : UINT64_C(0xfe));
  Emulator.setRegisterBytes(DestinationReg.Offset, doubleVector(Destination));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(DestinationReg.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, doubleVector(Expected));
}

TEST(X86EVEXFloatArith, ZmmAddpsComputesEveryLane) {
  // vaddps zmm0, zmm2, zmm3
  const std::vector<LowOp> Ops = liftX64({0x62, 0xf1, 0x6c, 0x48, 0x58, 0xc3});
  ASSERT_FALSE(Ops.empty());

  std::vector<float> Left(16), Right(16), Expected(16);
  for (size_t Lane = 0; Lane < Left.size(); ++Lane) {
    Left[Lane] = static_cast<float>(Lane + 1);
    Right[Lane] = static_cast<float>((Lane + 1) * 2);
    Expected[Lane] = Left[Lane] + Right[Lane];
  }

  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source1.Offset, floatVector(Left));
  Emulator.setRegisterBytes(Source2.Offset, floatVector(Right));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, floatVector(Expected));
}

TEST(X86EVEXFloatArith,
     PackedSingleRegisterFormsCoverEveryWidthAndBasicOperation) {
  struct WidthCase {
    uint8_t EvexLength;
    size_t VectorBytes;
  };
  constexpr WidthCase Widths[] = {
      {0x08, 16},
      {0x28, 32},
      {0x48, 64},
  };
  constexpr PackedOperation Operations[] = {
      PackedOperation::Add,
      PackedOperation::Sub,
      PackedOperation::Mul,
      PackedOperation::Div,
  };

  for (const WidthCase &Width : Widths) {
    for (PackedOperation Operation : Operations) {
      SCOPED_TRACE(testing::Message()
                   << "vector_bytes=" << Width.VectorBytes << " opcode=0x"
                   << std::hex << static_cast<unsigned>(Operation));
      // v{add,sub,mul,div}ps {x,y,z}mm0, {x,y,z}mm2, {x,y,z}mm19
      expectPackedPs({0x62, 0xb1, 0x6c, Width.EvexLength,
                      static_cast<uint8_t>(Operation), 0xc3},
                     Width.VectorBytes, Operation);
    }
  }
}

TEST(X86EVEXFloatArith,
     PackedDoubleRegisterFormsCoverEveryWidthAndBasicOperation) {
  struct WidthCase {
    uint8_t EvexLength;
    size_t VectorBytes;
  };
  constexpr WidthCase Widths[] = {
      {0x08, 16},
      {0x28, 32},
      {0x48, 64},
  };
  constexpr PackedOperation Operations[] = {
      PackedOperation::Add,
      PackedOperation::Sub,
      PackedOperation::Mul,
      PackedOperation::Div,
  };

  for (const WidthCase &Width : Widths) {
    for (PackedOperation Operation : Operations) {
      SCOPED_TRACE(testing::Message()
                   << "vector_bytes=" << Width.VectorBytes << " opcode=0x"
                   << std::hex << static_cast<unsigned>(Operation));
      // v{add,sub,mul,div}pd {x,y,z}mm0, {x,y,z}mm2, {x,y,z}mm19
      expectPackedPd({0x62, 0xb1, 0xed, Width.EvexLength,
                      static_cast<uint8_t>(Operation), 0xc3},
                     Width.VectorBytes, Operation);
    }
  }
}

TEST(X86EVEXFloatArith, ZmmAddpsMergeMaskPreservesInactiveDestinationLanes) {
  // vaddps zmm0 {k1}, zmm2, zmm19
  const std::vector<LowOp> Ops = liftX64({0x62, 0xb1, 0x6c, 0x49, 0x58, 0xc3});
  ASSERT_FALSE(Ops.empty());

  std::vector<float> Left(16), Right(16), Destination(16), Expected(16);
  constexpr uint64_t Mask = UINT64_C(0xa55a);
  for (size_t Lane = 0; Lane < Left.size(); ++Lane) {
    Left[Lane] = static_cast<float>((Lane + 1) * 4);
    Right[Lane] = static_cast<float>(Lane + 1);
    Destination[Lane] = static_cast<float>(-1000 - Lane);
    Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) ? Left[Lane] + Right[Lane]
                                                    : Destination[Lane];
  }

  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K1);
  const RegInfo DestinationReg = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source1.Offset, floatVector(Left));
  Emulator.setRegisterBytes(Source2.Offset, floatVector(Right));
  Emulator.setRegister(WriteMask.Offset, Mask);
  Emulator.setRegisterBytes(DestinationReg.Offset, floatVector(Destination));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(DestinationReg.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, floatVector(Expected));
}

TEST(X86EVEXFloatArith,
     PackedSingleMergeAndZeroMasksCoverEveryWidthAndBasicOperation) {
  struct WidthCase {
    uint8_t EvexLength;
    size_t VectorBytes;
  };
  constexpr WidthCase Widths[] = {
      {0x08, 16},
      {0x28, 32},
      {0x48, 64},
  };
  constexpr PackedOperation Operations[] = {
      PackedOperation::Add,
      PackedOperation::Sub,
      PackedOperation::Mul,
      PackedOperation::Div,
  };

  for (const WidthCase &Width : Widths) {
    for (PackedOperation Operation : Operations) {
      for (bool ZeroMask : {false, true}) {
        SCOPED_TRACE(testing::Message()
                     << "vector_bytes=" << Width.VectorBytes << " opcode=0x"
                     << std::hex << static_cast<unsigned>(Operation)
                     << " zero=" << ZeroMask);
        const uint8_t MaskedLength = Width.EvexLength | UINT8_C(0x01) |
                                     (ZeroMask ? UINT8_C(0x80) : UINT8_C(0));
        // v{add,sub,mul,div}ps {x,y,z}mm0 {k1}{/z}, {x,y,z}mm2,
        // {x,y,z}mm19
        expectMaskedPs({0x62, 0xb1, 0x6c, MaskedLength,
                        static_cast<uint8_t>(Operation), 0xc3},
                       Width.VectorBytes, Operation, ZeroMask);
      }
    }
  }
}

TEST(X86EVEXFloatArith,
     PackedDoubleMergeAndZeroMasksCoverEveryWidthAndBasicOperation) {
  struct WidthCase {
    uint8_t EvexLength;
    size_t VectorBytes;
  };
  constexpr WidthCase Widths[] = {
      {0x08, 16},
      {0x28, 32},
      {0x48, 64},
  };
  constexpr PackedOperation Operations[] = {
      PackedOperation::Add,
      PackedOperation::Sub,
      PackedOperation::Mul,
      PackedOperation::Div,
  };

  for (const WidthCase &Width : Widths) {
    for (PackedOperation Operation : Operations) {
      for (bool ZeroMask : {false, true}) {
        SCOPED_TRACE(testing::Message()
                     << "vector_bytes=" << Width.VectorBytes << " opcode=0x"
                     << std::hex << static_cast<unsigned>(Operation)
                     << " zero=" << ZeroMask);
        const uint8_t MaskedLength = Width.EvexLength | UINT8_C(0x01) |
                                     (ZeroMask ? UINT8_C(0x80) : UINT8_C(0));
        // v{add,sub,mul,div}pd {x,y,z}mm0 {k1}{/z}, {x,y,z}mm2,
        // {x,y,z}mm19
        expectMaskedPd({0x62, 0xb1, 0xed, MaskedLength,
                        static_cast<uint8_t>(Operation), 0xc3},
                       Width.VectorBytes, Operation, ZeroMask);
      }
    }
  }
}

TEST(X86EVEXFloatArith,
     BroadcastSaeAndEmbeddedRoundingFormsLiftToExactFPArith) {
  // vaddps zmm0, zmm2, dword ptr [rax] {1to16}
  expectExactlyOneFPArith({0x62, 0xf1, 0x6c, 0x58, 0x58, 0x00});
  // vaddpd zmm0, zmm2, qword ptr [rax] {1to8}
  expectExactlyOneFPArith({0x62, 0xf1, 0xed, 0x58, 0x58, 0x00});
  // vaddps zmm0, zmm2, zmm3, {rn-sae}
  expectExactlyOneFPArith({0x62, 0xf1, 0x6c, 0x18, 0x58, 0xc3});
  // vaddpd zmm0, zmm2, zmm3, {rz-sae}
  expectExactlyOneFPArith({0x62, 0xf1, 0xed, 0x78, 0x58, 0xc3});
  // vaddps zmm0 {k1}, zmm2, zmmword ptr [rax]
  expectExactlyOneFPArith({0x62, 0xf1, 0x6c, 0x49, 0x58, 0x00});
}

TEST(X86EVEXFloatArith,
     EmbeddedRoundingOverridesMXCSRAndSuppressesInexactState) {
  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;

  {
    // vaddps zmm0, zmm2, zmm3, {rn-sae}.  Add half an ULP to an
    // odd-significand value: ties-to-even rounds up despite MXCSR selecting
    // round-toward-zero, and SAE leaves the inexact flag untouched.
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf1, 0x6c, 0x18, 0x58, 0xc3});
    ASSERT_FALSE(Ops.empty());
    const std::vector<uint32_t> Left(16, UINT32_C(0x3f800001));
    const std::vector<uint32_t> Right(16, UINT32_C(0x33800000));
    const std::vector<uint32_t> Expected(16, UINT32_C(0x3f800002));
    constexpr uint32_t InitialMXCSR = UINT32_C(0x7f80);

    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(InitialMXCSR);
    Emulator.setRegisterBytes(Source1.Offset, dwordBitsVector(Left));
    Emulator.setRegisterBytes(Source2.Offset, dwordBitsVector(Right));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset),
              dwordBitsVector(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), InitialMXCSR);
  }

  {
    // vaddpd zmm0, zmm2, zmm3, {rz-sae}.  The same tie remains at the
    // lower odd-significand value even though MXCSR selects nearest-even.
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf1, 0xed, 0x78, 0x58, 0xc3});
    ASSERT_FALSE(Ops.empty());
    const std::vector<uint64_t> Left(8, UINT64_C(0x3ff0000000000001));
    const std::vector<uint64_t> Right(8, UINT64_C(0x3ca0000000000000));
    const std::vector<uint64_t> Expected(8, UINT64_C(0x3ff0000000000001));
    constexpr uint32_t InitialMXCSR = UINT32_C(0x1f80);

    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(InitialMXCSR);
    Emulator.setRegisterBytes(Source1.Offset, qwordBitsVector(Left));
    Emulator.setRegisterBytes(Source2.Offset, qwordBitsVector(Right));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset),
              qwordBitsVector(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), InitialMXCSR);
  }
}

TEST(X86EVEXFloatArith, ZmmSqrtpsComputesEveryLaneAndPreservesNegativeZero) {
  // vsqrtps zmm0, zmm19
  const std::vector<LowOp> Ops = liftX64({0x62, 0xb1, 0x7c, 0x48, 0x51, 0xc3});
  ASSERT_FALSE(Ops.empty());

  std::vector<float> Input(16), Expected(16);
  Input[0] = -0.0f;
  Expected[0] = -0.0f;
  for (size_t Lane = 1; Lane < Input.size(); ++Lane) {
    Input[Lane] = static_cast<float>(Lane * Lane);
    Expected[Lane] = static_cast<float>(Lane);
  }

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source.Offset, floatVector(Input));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, floatVector(Expected));
}

TEST(X86EVEXFloatArith, ZmmSqrtpsMergeMaskPreservesInactiveLanes) {
  // vsqrtps zmm0 {k1}, zmm19
  const std::vector<LowOp> Ops = liftX64({0x62, 0xb1, 0x7c, 0x49, 0x51, 0xc3});
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Mask = UINT64_C(0xa55a);
  std::vector<float> Input(16), Destination(16), Expected(16);
  for (size_t Lane = 0; Lane < Input.size(); ++Lane) {
    Input[Lane] = static_cast<float>((Lane + 1) * (Lane + 1));
    Destination[Lane] = static_cast<float>(-1000 - Lane);
    Expected[Lane] = (Mask & (UINT64_C(1) << Lane))
                         ? static_cast<float>(Lane + 1)
                         : Destination[Lane];
  }

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K1);
  const RegInfo DestinationReg = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source.Offset, floatVector(Input));
  Emulator.setRegister(WriteMask.Offset, Mask);
  Emulator.setRegisterBytes(DestinationReg.Offset, floatVector(Destination));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(DestinationReg.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, floatVector(Expected));
}

TEST(X86EVEXFloatArith, PackedSqrtRegisterFormsCoverEveryWidth) {
  struct WidthCase {
    uint8_t EvexLength;
    size_t VectorBytes;
  };
  constexpr WidthCase Widths[] = {
      {0x08, 16},
      {0x28, 32},
      {0x48, 64},
  };

  for (const WidthCase &Width : Widths) {
    SCOPED_TRACE(testing::Message() << "ps vector_bytes=" << Width.VectorBytes);
    // vsqrtps {x,y,z}mm0, {x,y,z}mm19
    expectSqrtPs({0x62, 0xb1, 0x7c, Width.EvexLength, 0x51, 0xc3},
                 Width.VectorBytes, false, false);

    SCOPED_TRACE(testing::Message() << "pd vector_bytes=" << Width.VectorBytes);
    // vsqrtpd {x,y,z}mm0, {x,y,z}mm19
    expectSqrtPd({0x62, 0xb1, 0xfd, Width.EvexLength, 0x51, 0xc3},
                 Width.VectorBytes, false, false);
  }
}

TEST(X86EVEXFloatArith, PackedSqrtMergeAndZeroMasksCoverEveryWidth) {
  struct WidthCase {
    uint8_t EvexLength;
    size_t VectorBytes;
  };
  constexpr WidthCase Widths[] = {
      {0x08, 16},
      {0x28, 32},
      {0x48, 64},
  };

  for (const WidthCase &Width : Widths) {
    for (bool ZeroMask : {false, true}) {
      const uint8_t MaskedLength = Width.EvexLength | UINT8_C(0x01) |
                                   (ZeroMask ? UINT8_C(0x80) : UINT8_C(0));
      SCOPED_TRACE(testing::Message() << "vector_bytes=" << Width.VectorBytes
                                      << " zero=" << ZeroMask);
      // vsqrtps {x,y,z}mm0 {k1}{/z}, {x,y,z}mm19
      expectSqrtPs({0x62, 0xb1, 0x7c, MaskedLength, 0x51, 0xc3},
                   Width.VectorBytes, true, ZeroMask);
      // vsqrtpd {x,y,z}mm0 {k1}{/z}, {x,y,z}mm19
      expectSqrtPd({0x62, 0xb1, 0xfd, MaskedLength, 0x51, 0xc3},
                   Width.VectorBytes, true, ZeroMask);
    }
  }
}

TEST(X86EVEXFloatArith, PackedSqrtSpecialValuesFollowX86BitRules) {
  const auto F = [](float Value) { return std::bit_cast<uint32_t>(Value); };
  const std::vector<uint32_t> PsInput = {
      UINT32_C(0x00000000),
      UINT32_C(0x80000000),
      UINT32_C(0x7f800000),
      UINT32_C(0xff800000),
      UINT32_C(0x7fc12345),
      UINT32_C(0xffc54321),
      UINT32_C(0x7f812345),
      UINT32_C(0xff854321),
      F(-1.0f),
      F(-4.0f),
      F(4.0f),
      F(9.0f),
      F(16.0f),
      F(25.0f),
      F(36.0f),
      F(49.0f),
  };
  const std::vector<uint32_t> PsExpected = {
      UINT32_C(0x00000000),
      UINT32_C(0x80000000),
      UINT32_C(0x7f800000),
      UINT32_C(0xffc00000),
      UINT32_C(0x7fc12345),
      UINT32_C(0xffc54321),
      UINT32_C(0x7fc12345),
      UINT32_C(0xffc54321),
      UINT32_C(0xffc00000),
      UINT32_C(0xffc00000),
      F(2.0f),
      F(3.0f),
      F(4.0f),
      F(5.0f),
      F(6.0f),
      F(7.0f),
  };

  const auto D = [](double Value) { return std::bit_cast<uint64_t>(Value); };
  const std::vector<uint64_t> PdInput = {
      UINT64_C(0x0000000000000000),
      UINT64_C(0x8000000000000000),
      UINT64_C(0x7ff0000000000000),
      UINT64_C(0xfff0000000000000),
      UINT64_C(0x7ff8123456789abc),
      UINT64_C(0xfff0123456789abc),
      D(-1.0),
      D(4.0),
  };
  const std::vector<uint64_t> PdExpected = {
      UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000),
      UINT64_C(0x7ff0000000000000), UINT64_C(0xfff8000000000000),
      UINT64_C(0x7ff8123456789abc), UINT64_C(0xfff8123456789abc),
      UINT64_C(0xfff8000000000000), D(2.0),
  };

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);

  {
    // vsqrtps zmm0, zmm19
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xb1, 0x7c, 0x48, 0x51, 0xc3});
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(Source.Offset, dwordBitsVector(PsInput));
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    EXPECT_EQ(*Result, dwordBitsVector(PsExpected));
  }

  {
    // vsqrtpd zmm0, zmm19
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xb1, 0xfd, 0x48, 0x51, 0xc3});
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(Source.Offset, qwordBitsVector(PdInput));
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Destination.Offset);
    ASSERT_TRUE(Result);
    EXPECT_EQ(*Result, qwordBitsVector(PdExpected));
  }
}

TEST(X86EVEXFloatArith,
     ZmmMinpsReturnsSecondSourceForUnorderedAndSignedZeroTies) {
  // vminps zmm0, zmm2, zmm19
  const std::vector<LowOp> Ops = liftX64({0x62, 0xb1, 0x6c, 0x48, 0x5d, 0xc3});
  ASSERT_FALSE(Ops.empty());

  constexpr uint32_t QuietNaN1 = UINT32_C(0x7fc12345);
  constexpr uint32_t SignalingNaN2 = UINT32_C(0xff854321);
  constexpr uint32_t PositiveZero = UINT32_C(0x00000000);
  constexpr uint32_t NegativeZero = UINT32_C(0x80000000);
  const auto Bits = [](float Value) { return std::bit_cast<uint32_t>(Value); };
  const std::array<uint32_t, 8> LeftPattern = {
      QuietNaN1,  Bits(7.0f), PositiveZero, NegativeZero,
      Bits(1.0f), Bits(3.0f), Bits(-5.0f),  Bits(-7.0f),
  };
  const std::array<uint32_t, 8> RightPattern = {
      Bits(7.0f), SignalingNaN2, NegativeZero, PositiveZero,
      Bits(2.0f), Bits(2.0f),    Bits(-7.0f),  Bits(-5.0f),
  };
  const std::array<uint32_t, 8> ExpectedPattern = {
      Bits(7.0f), SignalingNaN2, NegativeZero, PositiveZero,
      Bits(1.0f), Bits(2.0f),    Bits(-7.0f),  Bits(-7.0f),
  };

  std::vector<uint32_t> Left, Right, Expected;
  for (unsigned Repeat = 0; Repeat < 2; ++Repeat) {
    Left.insert(Left.end(), LeftPattern.begin(), LeftPattern.end());
    Right.insert(Right.end(), RightPattern.begin(), RightPattern.end());
    Expected.insert(Expected.end(), ExpectedPattern.begin(),
                    ExpectedPattern.end());
  }

  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source1.Offset, dwordBitsVector(Left));
  Emulator.setRegisterBytes(Source2.Offset, dwordBitsVector(Right));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, dwordBitsVector(Expected));
}

TEST(X86EVEXFloatArith,
     ZmmMaxpdReturnsSecondSourceForUnorderedAndSignedZeroTies) {
  // vmaxpd zmm0, zmm2, zmm19
  const std::vector<LowOp> Ops = liftX64({0x62, 0xb1, 0xed, 0x48, 0x5f, 0xc3});
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t QuietNaN1 = UINT64_C(0x7ff8123456789abc);
  constexpr uint64_t SignalingNaN2 = UINT64_C(0xfff0123456789abc);
  constexpr uint64_t PositiveZero = UINT64_C(0x0000000000000000);
  constexpr uint64_t NegativeZero = UINT64_C(0x8000000000000000);
  const auto Bits = [](double Value) { return std::bit_cast<uint64_t>(Value); };
  const std::vector<uint64_t> Left = {
      QuietNaN1, Bits(7.0), PositiveZero, NegativeZero,
      Bits(1.0), Bits(3.0), Bits(-5.0),   Bits(-7.0),
  };
  const std::vector<uint64_t> Right = {
      Bits(7.0), SignalingNaN2, NegativeZero, PositiveZero,
      Bits(2.0), Bits(2.0),     Bits(-7.0),   Bits(-5.0),
  };
  const std::vector<uint64_t> Expected = {
      Bits(7.0), SignalingNaN2, NegativeZero, PositiveZero,
      Bits(2.0), Bits(3.0),     Bits(-5.0),   Bits(-5.0),
  };

  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source1.Offset, qwordBitsVector(Left));
  Emulator.setRegisterBytes(Source2.Offset, qwordBitsVector(Right));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, qwordBitsVector(Expected));
}

TEST(X86EVEXFloatArith, ZmmMaxpdZeroMaskClearsInactiveLanes) {
  // vmaxpd zmm0 {k1} {z}, zmm2, zmm19
  const std::vector<LowOp> Ops = liftX64({0x62, 0xb1, 0xed, 0xc9, 0x5f, 0xc3});
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Mask = UINT64_C(0xa5);
  std::vector<double> Left(8), Right(8), Expected(8, 0.0);
  for (size_t Lane = 0; Lane < Left.size(); ++Lane) {
    Left[Lane] = static_cast<double>(Lane + 1);
    Right[Lane] = static_cast<double>(16 - Lane);
    if (Mask & (UINT64_C(1) << Lane))
      Expected[Lane] = Left[Lane] > Right[Lane] ? Left[Lane] : Right[Lane];
  }

  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K1);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source1.Offset, doubleVector(Left));
  Emulator.setRegisterBytes(Source2.Offset, doubleVector(Right));
  Emulator.setRegister(WriteMask.Offset, Mask);
  Emulator.setRegisterBytes(Destination.Offset, std::vector<uint8_t>(64, 0xa5));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, doubleVector(Expected));
}

TEST(X86EVEXFloatArith, PackedMinMaxRegisterFormsCoverEveryWidth) {
  struct WidthCase {
    uint8_t EvexLength;
    size_t VectorBytes;
  };
  constexpr WidthCase Widths[] = {
      {0x08, 16},
      {0x28, 32},
      {0x48, 64},
  };

  for (const WidthCase &Width : Widths) {
    for (bool IsMin : {true, false}) {
      const uint8_t Opcode = IsMin ? UINT8_C(0x5d) : UINT8_C(0x5f);
      SCOPED_TRACE(testing::Message()
                   << "vector_bytes=" << Width.VectorBytes << " min=" << IsMin);
      // v{min,max}ps {x,y,z}mm0, {x,y,z}mm2, {x,y,z}mm19
      expectMinMaxPs({0x62, 0xb1, 0x6c, Width.EvexLength, Opcode, 0xc3},
                     Width.VectorBytes, IsMin, false, false);
      // v{min,max}pd {x,y,z}mm0, {x,y,z}mm2, {x,y,z}mm19
      expectMinMaxPd({0x62, 0xb1, 0xed, Width.EvexLength, Opcode, 0xc3},
                     Width.VectorBytes, IsMin, false, false);
    }
  }
}

TEST(X86EVEXFloatArith, PackedMinMaxMergeAndZeroMasksCoverEveryWidth) {
  struct WidthCase {
    uint8_t EvexLength;
    size_t VectorBytes;
  };
  constexpr WidthCase Widths[] = {
      {0x08, 16},
      {0x28, 32},
      {0x48, 64},
  };

  for (const WidthCase &Width : Widths) {
    for (bool IsMin : {true, false}) {
      for (bool ZeroMask : {false, true}) {
        const uint8_t Opcode = IsMin ? UINT8_C(0x5d) : UINT8_C(0x5f);
        const uint8_t MaskedLength = Width.EvexLength | UINT8_C(0x01) |
                                     (ZeroMask ? UINT8_C(0x80) : UINT8_C(0));
        SCOPED_TRACE(testing::Message()
                     << "vector_bytes=" << Width.VectorBytes << " min=" << IsMin
                     << " zero=" << ZeroMask);
        // v{min,max}ps {x,y,z}mm0 {k1}{/z}, sources 2 and 19
        expectMinMaxPs({0x62, 0xb1, 0x6c, MaskedLength, Opcode, 0xc3},
                       Width.VectorBytes, IsMin, true, ZeroMask);
        // v{min,max}pd {x,y,z}mm0 {k1}{/z}, sources 2 and 19
        expectMinMaxPd({0x62, 0xb1, 0xed, MaskedLength, Opcode, 0xc3},
                       Width.VectorBytes, IsMin, true, ZeroMask);
      }
    }
  }
}

TEST(X86EVEXFloatArith,
     ScalarAddssMergeMaskKeepsOldLowLaneAndSrc1UpperXmmLanes) {
  // vaddss xmm0 {k1}, xmm2, xmm19
  const std::vector<LowOp> Ops = liftX64({0x62, 0xb1, 0x6e, 0x09, 0x58, 0xc3});
  ASSERT_FALSE(Ops.empty());

  std::vector<float> Source1(16), Source2(16), Destination(16), Expected(16);
  for (size_t Lane = 0; Lane < Source1.size(); ++Lane) {
    Source1[Lane] = static_cast<float>(10 + Lane);
    Source2[Lane] = static_cast<float>(20 + Lane);
    Destination[Lane] = static_cast<float>(100 + Lane);
  }
  Expected[0] = Destination[0];
  Expected[1] = Source1[1];
  Expected[2] = Source1[2];
  Expected[3] = Source1[3];

  const RegInfo Source1Reg = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2Reg = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K1);
  const RegInfo DestinationReg = mapCapstoneReg(X86_REG_ZMM0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Source1Reg.Offset, floatVector(Source1));
  Emulator.setRegisterBytes(Source2Reg.Offset, floatVector(Source2));
  Emulator.setRegister(WriteMask.Offset, 0);
  Emulator.setRegisterBytes(DestinationReg.Offset, floatVector(Destination));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(DestinationReg.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, floatVector(Expected));
}

TEST(X86EVEXFloatArith,
     ScalarArithmeticMasksUseOnlyBitZeroAndPreserveSrc1UpperXmm) {
  constexpr PackedOperation Operations[] = {
      PackedOperation::Add,
      PackedOperation::Sub,
      PackedOperation::Mul,
      PackedOperation::Div,
  };

  for (PackedOperation Operation : Operations) {
    for (bool ZeroMask : {false, true}) {
      for (bool MaskBit : {false, true}) {
        const uint8_t MaskByte = ZeroMask ? UINT8_C(0x89) : UINT8_C(0x09);
        SCOPED_TRACE(testing::Message()
                     << "opcode=0x" << std::hex
                     << static_cast<unsigned>(Operation) << " zero=" << ZeroMask
                     << " bit0=" << MaskBit);
        // v{add,sub,mul,div}ss xmm0 {k1}{/z}, xmm2, xmm19
        expectScalarSs(
            {0x62, 0xb1, 0x6e, MaskByte, static_cast<uint8_t>(Operation), 0xc3},
            Operation, MaskBit, ZeroMask);
        // v{add,sub,mul,div}sd xmm0 {k1}{/z}, xmm2, xmm19
        expectScalarSd(
            {0x62, 0xb1, 0xef, MaskByte, static_cast<uint8_t>(Operation), 0xc3},
            Operation, MaskBit, ZeroMask);
      }
    }
  }
}

TEST(X86EVEXFloatArith,
     NewFloatFamiliesMemorySaeAndRoundingLiftToExactFPArith) {
  // vsqrtps zmm0, zmmword ptr [rax]
  expectExactlyOneFPArith({0x62, 0xf1, 0x7c, 0x48, 0x51, 0x00});
  // vsqrtpd zmm0, zmmword ptr [rax]
  expectExactlyOneFPArith({0x62, 0xf1, 0xfd, 0x48, 0x51, 0x00});
  // vminps zmm0, zmm2, zmmword ptr [rax]
  expectExactlyOneFPArith({0x62, 0xf1, 0x6c, 0x48, 0x5d, 0x00});
  // vmaxpd zmm0, zmm2, zmmword ptr [rax]
  expectExactlyOneFPArith({0x62, 0xf1, 0xed, 0x48, 0x5f, 0x00});
  // vaddss xmm0 {k1}, xmm2, dword ptr [rax]
  expectExactlyOneFPArith({0x62, 0xf1, 0x6e, 0x09, 0x58, 0x00});
  // vaddsd xmm0 {k1}, xmm2, qword ptr [rax]
  expectExactlyOneFPArith({0x62, 0xf1, 0xef, 0x09, 0x58, 0x00});
  // vaddss xmm0, xmm18, dword ptr [rax]
  expectExactlyOneFPArith({0x62, 0xf1, 0x6e, 0x00, 0x58, 0x00});
  // vaddsd xmm0, xmm18, qword ptr [rax]
  expectExactlyOneFPArith({0x62, 0xf1, 0xef, 0x00, 0x58, 0x00});
  // vsqrtps zmm0, zmm19, {rn-sae}
  expectExactlyOneFPArith({0x62, 0xb1, 0x7c, 0x18, 0x51, 0xc3});
  // vsqrtpd zmm0, zmm19, {rz-sae}
  expectExactlyOneFPArith({0x62, 0xb1, 0xfd, 0x78, 0x51, 0xc3});
  // vminps zmm0, zmm2, zmm19, {sae}
  expectExactlyOneFPArith({0x62, 0xb1, 0x6c, 0x18, 0x5d, 0xc3});
  // vmaxpd zmm0, zmm2, zmm19, {sae}
  expectExactlyOneFPArith({0x62, 0xb1, 0xed, 0x18, 0x5f, 0xc3});
  // vaddss xmm0 {k1}, xmm2, xmm19, {rn-sae}
  expectExactlyOneFPArith({0x62, 0xb1, 0x6e, 0x19, 0x58, 0xc3});
  // vaddsd xmm0 {k1}, xmm2, xmm19, {rz-sae}
  expectExactlyOneFPArith({0x62, 0xb1, 0xef, 0x79, 0x58, 0xc3});
}

} // namespace
