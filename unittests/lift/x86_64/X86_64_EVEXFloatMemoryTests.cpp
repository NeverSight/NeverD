//===- X86_64_EVEXFloatMemoryTests.cpp - EVEX FP memory semantics -------===//

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

template <typename T> std::vector<uint8_t> bytes(const std::vector<T> &Values) {
  std::vector<uint8_t> Result(Values.size() * sizeof(T));
  std::memcpy(Result.data(), Values.data(), Result.size());
  return Result;
}

enum class PackedFloatOperation { Subtract, Multiply, Divide };

struct PackedFloatMemoryCase {
  const char *Name;
  uint8_t Opcode;
  bool IsDouble;
  PackedFloatOperation Operation;
};

constexpr PackedFloatMemoryCase kNewPackedFloatMemoryCases[] = {
    {"vsubps", 0x5c, false, PackedFloatOperation::Subtract},
    {"vsubpd", 0x5c, true, PackedFloatOperation::Subtract},
    {"vmulps", 0x59, false, PackedFloatOperation::Multiply},
    {"vmulpd", 0x59, true, PackedFloatOperation::Multiply},
    {"vdivps", 0x5e, false, PackedFloatOperation::Divide},
    {"vdivpd", 0x5e, true, PackedFloatOperation::Divide},
};

template <typename T>
T applyPackedFloatOperation(PackedFloatOperation Operation, T Left, T Right) {
  switch (Operation) {
  case PackedFloatOperation::Subtract:
    return Left - Right;
  case PackedFloatOperation::Multiply:
    return Left * Right;
  case PackedFloatOperation::Divide:
    return Left / Right;
  }
  return T{};
}

template <typename T>
T leftLaneValue(PackedFloatOperation Operation, unsigned Lane) {
  if (Operation == PackedFloatOperation::Divide)
    return static_cast<T>(2 * (Lane + 1));
  return static_cast<T>(Lane + 1);
}

template <typename T>
T rightLaneValue(PackedFloatOperation Operation, unsigned Lane) {
  if (Operation == PackedFloatOperation::Subtract)
    return static_cast<T>(Lane) / static_cast<T>(2);
  return static_cast<T>(2);
}

std::vector<uint8_t> packedMemoryEncoding(const PackedFloatMemoryCase &TestCase,
                                          uint8_t P2, uint8_t Disp8,
                                          bool FsAddr32 = false) {
  std::vector<uint8_t> Encoding;
  if (FsAddr32) {
    Encoding.push_back(0x64);
    Encoding.push_back(0x67);
  }
  Encoding.insert(Encoding.end(),
                  {0x62, 0xf1,
                   static_cast<uint8_t>(TestCase.IsDouble ? 0xed : 0x6c), P2,
                   TestCase.Opcode, 0x40, Disp8});
  return Encoding;
}

template <typename T>
void checkFullTupleMaskedMerge(const PackedFloatMemoryCase &TestCase) {
  // v{sub,mul,div}p{s,d} zmm0 {k1}, zmm2,
  // zmmword ptr fs:[eax + 0x80]
  const std::vector<LowOp> Ops =
      liftX64(packedMemoryEncoding(TestCase, 0x49, 0x02, true));
  ASSERT_FALSE(Ops.empty());

  constexpr unsigned LaneCount = 64 / sizeof(T);
  constexpr uint64_t FsBase = UINT64_C(0x100000000);
  constexpr uint32_t Eax = UINT32_C(0xfffff000);
  constexpr uint64_t TupleOffset = UINT64_C(0xfffff080);
  constexpr uint64_t LinearAddress = FsBase + TupleOffset;
  const uint64_t Mask = (UINT64_C(1) << 0) | (UINT64_C(1) << (LaneCount / 2)) |
                        (UINT64_C(1) << (LaneCount - 1));

  std::vector<T> Left(LaneCount), OldDestination(LaneCount),
      Expected(LaneCount);
  BinaryImage Image = emptyImage();
  std::vector<uint64_t> ExpectedLoads;
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    Left[Lane] = leftLaneValue<T>(TestCase.Operation, Lane);
    OldDestination[Lane] = static_cast<T>(-100 - static_cast<int>(Lane));
    Expected[Lane] = OldDestination[Lane];
    if ((Mask & (UINT64_C(1) << Lane)) == 0)
      continue;
    const T Right = rightLaneValue<T>(TestCase.Operation, Lane);
    Expected[Lane] =
        applyPackedFloatOperation(TestCase.Operation, Left[Lane], Right);
    const uint64_t LaneAddress = LinearAddress + Lane * sizeof(T);
    addReadableBytes(Image, LaneAddress, &Right, sizeof(Right));
    ExpectedLoads.push_back(LaneAddress);
  }

  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  ASSERT_TRUE(
      Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS, FsBase));
  Emulator.setRegister(x86reg::RAX, UINT64_C(0xaaaaaaaa00000000) | Eax);
  Emulator.setRegister(x86reg::K1, Mask);
  Emulator.setRegisterBytes(x86reg::vectorReg(2), bytes(Left));
  Emulator.setRegisterBytes(x86reg::vectorReg(0), bytes(OldDestination));

  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(0)), bytes(Expected));
  ASSERT_EQ(Emulator.getLoadRecords().size(), ExpectedLoads.size());
  for (unsigned Index = 0; Index < ExpectedLoads.size(); ++Index) {
    EXPECT_EQ(Emulator.getLoadRecords()[Index].Addr, ExpectedLoads[Index]);
    EXPECT_EQ(Emulator.getLoadRecords()[Index].Size, sizeof(T));
  }
  EXPECT_FALSE(Emulator.skips().any());
}

template <typename T>
void checkBroadcastMaskedZero(const PackedFloatMemoryCase &TestCase) {
  // v{sub,mul,div}p{s,d} zmm0 {k1}{z}, zmm2, [rax + disp8]{1toN}
  const std::vector<LowOp> Ops =
      liftX64(packedMemoryEncoding(TestCase, 0xd9, 0x02));
  ASSERT_FALSE(Ops.empty());

  constexpr unsigned LaneCount = 64 / sizeof(T);
  constexpr uint64_t Base = UINT64_C(0x5000);
  constexpr uint64_t Address = Base + 2 * sizeof(T);
  const uint64_t Mask = (UINT64_C(1) << 0) | (UINT64_C(1) << (LaneCount / 2)) |
                        (UINT64_C(1) << (LaneCount - 1));
  const T Scalar = static_cast<T>(2);
  std::vector<T> Left(LaneCount), Expected(LaneCount, static_cast<T>(0));
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    Left[Lane] = leftLaneValue<T>(TestCase.Operation, Lane);
    if (Mask & (UINT64_C(1) << Lane))
      Expected[Lane] =
          applyPackedFloatOperation(TestCase.Operation, Left[Lane], Scalar);
  }

  BinaryImage Image = emptyImage();
  addReadableBytes(Image, Address, &Scalar, sizeof(Scalar));
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::RAX, Base);
  Emulator.setRegister(x86reg::K1, Mask);
  Emulator.setRegisterBytes(x86reg::vectorReg(2), bytes(Left));
  Emulator.setRegisterBytes(x86reg::vectorReg(0),
                            std::vector<uint8_t>(64, 0xcc));

  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(0)), bytes(Expected));
  ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
  EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Address);
  EXPECT_EQ(Emulator.getLoadRecords()[0].Size, sizeof(T));
  EXPECT_FALSE(Emulator.skips().any());
}

void checkXmmZeroMaskSuppressesMemoryAndClearsUpperZmm(
    const PackedFloatMemoryCase &TestCase) {
  // v{sub,mul,div}p{s,d} xmm0 {k1}, xmm2, [rax]{1toN}
  const std::vector<uint8_t> Encoding = {
      0x62,
      0xf1,
      static_cast<uint8_t>(TestCase.IsDouble ? 0xed : 0x6c),
      0x19,
      TestCase.Opcode,
      0x00};
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());

  std::vector<uint8_t> Destination(64, 0xcc);
  std::vector<uint8_t> Expected(64, 0);
  std::copy_n(Destination.begin(), 16, Expected.begin());

  BinaryImage Unmapped = emptyImage();
  NdOpEmulator Emulator(Unmapped);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::RAX, UINT64_C(0x9000));
  Emulator.setRegister(x86reg::K1, 0);
  Emulator.setRegisterBytes(x86reg::vectorReg(2),
                            std::vector<uint8_t>(64, 0x31));
  Emulator.setRegisterBytes(x86reg::vectorReg(0), Destination);
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(0)), Expected);
  EXPECT_TRUE(Emulator.getLoadRecords().empty());
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86EVEXFloatMemory,
     VaddpsFullTupleUsesFsAddr32Disp8AndLoadsOnlyActiveLanes) {
  // vaddps zmm0 {k1}, zmm2, zmmword ptr fs:[eax + 0x80]
  const std::vector<uint8_t> Encoding = {0x64, 0x67, 0x62, 0xf1, 0x6c,
                                         0x49, 0x58, 0x40, 0x02};
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t FsBase = UINT64_C(0x100000000);
  constexpr uint32_t Eax = UINT32_C(0xfffff000);
  constexpr uint64_t TupleOffset = UINT64_C(0xfffff080);
  constexpr uint64_t LinearAddress = FsBase + TupleOffset;
  constexpr uint64_t Mask =
      (UINT64_C(1) << 0) | (UINT64_C(1) << 5) | (UINT64_C(1) << 15);

  std::vector<float> Left(16), OldDestination(16), Expected(16);
  BinaryImage Image = emptyImage();
  for (unsigned Lane = 0; Lane < 16; ++Lane) {
    Left[Lane] = static_cast<float>(Lane + 1);
    OldDestination[Lane] = static_cast<float>(-100 - Lane);
    Expected[Lane] = OldDestination[Lane];
    if ((Mask & (UINT64_C(1) << Lane)) == 0)
      continue;
    const float Right = static_cast<float>(1000 + Lane);
    Expected[Lane] = Left[Lane] + Right;
    addReadableBytes(Image, LinearAddress + Lane * sizeof(float), &Right,
                     sizeof(Right));
  }

  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  ASSERT_TRUE(
      Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS, FsBase));
  Emulator.setRegister(x86reg::RAX, UINT64_C(0xaaaaaaaa00000000) | Eax);
  Emulator.setRegister(x86reg::K1, Mask);
  Emulator.setRegisterBytes(x86reg::vectorReg(2), bytes(Left));
  Emulator.setRegisterBytes(x86reg::vectorReg(0), bytes(OldDestination));

  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(0)), bytes(Expected));
  ASSERT_EQ(Emulator.getLoadRecords().size(), 3u);
  EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, LinearAddress);
  EXPECT_EQ(Emulator.getLoadRecords()[1].Addr,
            LinearAddress + 5 * sizeof(float));
  EXPECT_EQ(Emulator.getLoadRecords()[2].Addr,
            LinearAddress + 15 * sizeof(float));
  for (const auto &Load : Emulator.getLoadRecords())
    EXPECT_EQ(Load.Size, sizeof(float));
  EXPECT_FALSE(Emulator.skips().any());

  BinaryImage Unmapped = emptyImage();
  NdOpEmulator Suppressed(Unmapped);
  Suppressed.setStrictMode(true);
  Suppressed.setLoadCollect(true);
  ASSERT_TRUE(Suppressed.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS,
                                                   FsBase));
  Suppressed.setRegister(x86reg::RAX, UINT64_C(0xaaaaaaaa00000000) | Eax);
  Suppressed.setRegister(x86reg::K1, 0);
  Suppressed.setRegisterBytes(x86reg::vectorReg(2), bytes(Left));
  Suppressed.setRegisterBytes(x86reg::vectorReg(0), bytes(OldDestination));
  ASSERT_EQ(Suppressed.run(Ops), Ops.size());
  EXPECT_EQ(Suppressed.getRegisterBytes(x86reg::vectorReg(0)),
            bytes(OldDestination));
  EXPECT_TRUE(Suppressed.getLoadRecords().empty());
  EXPECT_FALSE(Suppressed.skips().any());
}

TEST(X86EVEXFloatMemory, VaddpdBroadcastZeroMaskUsesOneDisp8ScaledScalarLoad) {
  // vaddpd zmm0 {k1}{z}, zmm2, qword ptr [rax + 0x10]{1to8}
  const std::vector<LowOp> Ops =
      liftX64({0x62, 0xf1, 0xed, 0xd9, 0x58, 0x40, 0x02});
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Base = UINT64_C(0x5000);
  constexpr uint64_t Address = Base + 0x10;
  constexpr uint64_t Mask = UINT64_C(0x81);
  const double Scalar = 0.5;
  std::vector<double> Left(8), Expected(8, 0.0);
  for (unsigned Lane = 0; Lane < Left.size(); ++Lane) {
    Left[Lane] = static_cast<double>(Lane + 1);
    if (Mask & (UINT64_C(1) << Lane))
      Expected[Lane] = Left[Lane] + Scalar;
  }

  BinaryImage Image = emptyImage();
  addReadableBytes(Image, Address, &Scalar, sizeof(Scalar));
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::RAX, Base);
  Emulator.setRegister(x86reg::K1, Mask);
  Emulator.setRegisterBytes(x86reg::vectorReg(2), bytes(Left));
  Emulator.setRegisterBytes(x86reg::vectorReg(0),
                            std::vector<uint8_t>(64, 0xcc));
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(0)), bytes(Expected));
  ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
  EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Address);
  EXPECT_EQ(Emulator.getLoadRecords()[0].Size, sizeof(double));
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86EVEXFloatMemory,
     VaddpsXmmZeroMaskSuppressesUnmappedBroadcastAndClearsUpperZmm) {
  // vaddps xmm0 {k1}, xmm2, dword ptr [rax]{1to4}
  const std::vector<LowOp> Ops = liftX64({0x62, 0xf1, 0x6c, 0x19, 0x58, 0x00});
  ASSERT_FALSE(Ops.empty());

  std::vector<uint8_t> Destination(64, 0xcc);
  std::vector<uint8_t> Expected(64, 0);
  std::copy_n(Destination.begin(), 16, Expected.begin());

  BinaryImage Unmapped = emptyImage();
  NdOpEmulator Emulator(Unmapped);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::RAX, UINT64_C(0x9000));
  Emulator.setRegister(x86reg::K1, 0);
  Emulator.setRegisterBytes(x86reg::vectorReg(2),
                            std::vector<uint8_t>(64, 0x31));
  Emulator.setRegisterBytes(x86reg::vectorReg(0), Destination);
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(0)), Expected);
  EXPECT_TRUE(Emulator.getLoadRecords().empty());
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86EVEXFloatMemory, SubMulDivFullTupleMaskedMergeReadsOnlyActiveLanes) {
  for (const PackedFloatMemoryCase &TestCase : kNewPackedFloatMemoryCases) {
    SCOPED_TRACE(TestCase.Name);
    if (TestCase.IsDouble)
      checkFullTupleMaskedMerge<double>(TestCase);
    else
      checkFullTupleMaskedMerge<float>(TestCase);
  }
}

TEST(X86EVEXFloatMemory,
     SubMulDivBroadcastMaskedZeroUsesOneDisp8ScaledScalarLoad) {
  for (const PackedFloatMemoryCase &TestCase : kNewPackedFloatMemoryCases) {
    SCOPED_TRACE(TestCase.Name);
    if (TestCase.IsDouble)
      checkBroadcastMaskedZero<double>(TestCase);
    else
      checkBroadcastMaskedZero<float>(TestCase);
  }
}

TEST(X86EVEXFloatMemory,
     SubMulDivXmmZeroMaskSuppressesMemoryAndClearsUpperZmm) {
  for (const PackedFloatMemoryCase &TestCase : kNewPackedFloatMemoryCases) {
    SCOPED_TRACE(TestCase.Name);
    checkXmmZeroMaskSuppressesMemoryAndClearsUpperZmm(TestCase);
  }
}

TEST(X86EVEXFloatMemory,
     SubMulDivRawDetailAndEnvironmentContractsFailClosedAtomically) {
  for (const PackedFloatMemoryCase &TestCase : kNewPackedFloatMemoryCases) {
    SCOPED_TRACE(TestCase.Name);
    const std::vector<uint8_t> FullTuple =
        packedMemoryEncoding(TestCase, 0x49, 0x02, true);
    const std::vector<uint8_t> Broadcast =
        packedMemoryEncoding(TestCase, 0xd9, 0x02);

    expectMutatedLiftFailsClosed(FullTuple, [](cs_insn &Insn, cs_x86 &) {
      Insn.bytes[6] = 0x58;
      return true;
    });
    expectMutatedLiftFailsClosed(FullTuple, [](cs_insn &Insn, cs_x86 &) {
      Insn.bytes[8] = 0x03;
      return true;
    });
    expectMutatedLiftFailsClosed(FullTuple, [](cs_insn &Insn, cs_x86 &) {
      Insn.bytes[0] = 0x65;
      return true;
    });
    expectMutatedLiftFailsClosed(FullTuple, [](cs_insn &Insn, cs_x86 &) {
      Insn.bytes[1] = 0x66;
      return true;
    });
    expectMutatedLiftFailsClosed(Broadcast, [](cs_insn &, cs_x86 &X86) {
      for (uint8_t Index = 0; Index < X86.op_count; ++Index) {
        if (X86.operands[Index].type != X86_OP_MEM)
          continue;
        X86.operands[Index].avx_bcast = X86_AVX_BCAST_INVALID;
        return true;
      }
      return false;
    });
    expectMutatedLiftFailsClosed(Broadcast, [](cs_insn &, cs_x86 &X86) {
      X86.avx_sae = true;
      return true;
    });

    std::vector<uint8_t> Register = {
        0x62,
        0xf1,
        static_cast<uint8_t>(TestCase.IsDouble ? 0xed : 0x6c),
        0x48,
        TestCase.Opcode,
        0xc3};
    expectMutatedLiftFailsClosed(Register, [](cs_insn &Insn, cs_x86 &) {
      Insn.bytes[3] |= 0x10;
      return true;
    });
  }
}

TEST(X86EVEXFloatMemory,
     RawAndDecodedMemoryContractsFailClosedBeforePartialLowIR) {
  const std::vector<uint8_t> FullTuple = {0x64, 0x67, 0x62, 0xf1, 0x6c,
                                          0x49, 0x58, 0x40, 0x02};
  const std::vector<uint8_t> Broadcast = {0x62, 0xf1, 0xed, 0xd9,
                                          0x58, 0x40, 0x02};

  expectMutatedLiftFailsClosed(FullTuple, [](cs_insn &Insn, cs_x86 &) {
    Insn.bytes[6] = 0x59;
    return true;
  });
  expectMutatedLiftFailsClosed(FullTuple, [](cs_insn &Insn, cs_x86 &) {
    Insn.bytes[8] = 0x03;
    return true;
  });
  expectMutatedLiftFailsClosed(FullTuple, [](cs_insn &Insn, cs_x86 &) {
    Insn.bytes[0] = 0x65;
    return true;
  });
  expectMutatedLiftFailsClosed(FullTuple, [](cs_insn &Insn, cs_x86 &) {
    Insn.bytes[1] = 0x66;
    return true;
  });
  expectMutatedLiftFailsClosed(Broadcast, [](cs_insn &, cs_x86 &X86) {
    for (uint8_t Index = 0; Index < X86.op_count; ++Index) {
      if (X86.operands[Index].type != X86_OP_MEM)
        continue;
      X86.operands[Index].avx_bcast = X86_AVX_BCAST_INVALID;
      return true;
    }
    return false;
  });
  expectMutatedLiftFailsClosed(Broadcast, [](cs_insn &, cs_x86 &X86) {
    for (uint8_t Index = 0; Index < X86.op_count; ++Index) {
      if (X86.operands[Index].type != X86_OP_MEM)
        continue;
      X86.operands[Index].size = 64;
      return true;
    }
    return false;
  });

  // vaddps zmm0, zmm2, zmm19: EVEX.b on a register source is ER/SAE, not
  // broadcast, and remains unsupported until its FP-environment semantics are
  // modeled explicitly.
  expectMutatedLiftFailsClosed({0x62, 0xb1, 0x6c, 0x48, 0x58, 0xc3},
                               [](cs_insn &Insn, cs_x86 &) {
                                 Insn.bytes[3] |= 0x10;
                                 return true;
                               });
}

} // namespace
