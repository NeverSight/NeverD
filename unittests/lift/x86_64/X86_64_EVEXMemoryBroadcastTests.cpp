//===- X86_64_EVEXMemoryBroadcastTests.cpp - EVEX memory semantics ------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
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

BinaryImage makeMemoryImage(uint64_t Address,
                            const std::vector<uint8_t> &Bytes) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Data;
  Data.VA = Address;
  Data.Size = Bytes.size();
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data = Bytes;
  Image.Segments.push_back(std::move(Data));
  return Image;
}

void setLane(std::vector<uint8_t> &Bytes, unsigned Lane, unsigned ElementSize,
             uint64_t Value) {
  ASSERT_LE(ElementSize, sizeof(Value));
  ASSERT_LE((Lane + 1) * ElementSize, Bytes.size());
  std::memcpy(Bytes.data() + Lane * ElementSize, &Value, ElementSize);
}

uint64_t getLane(const std::vector<uint8_t> &Bytes, unsigned Lane,
                 unsigned ElementSize) {
  uint64_t Value = 0;
  EXPECT_LE(ElementSize, sizeof(Value));
  EXPECT_LE((Lane + 1) * ElementSize, Bytes.size());
  std::memcpy(&Value, Bytes.data() + Lane * ElementSize, ElementSize);
  return Value;
}

std::vector<uint8_t> scalarBytes(uint64_t Value, unsigned ElementSize) {
  std::vector<uint8_t> Bytes(ElementSize);
  std::memcpy(Bytes.data(), &Value, ElementSize);
  return Bytes;
}

TEST(X86EVEXMemoryBroadcast, VpadddBroadcastLoadsOneScalarForEveryZmmLane) {
  // vpaddd zmm0, zmm2, dword ptr [rax]{1to16}
  const std::vector<LowOp> Ops = liftX64({0x62, 0xf1, 0x6d, 0x58, 0xfe, 0x00});
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Address = 0x4000;
  constexpr uint32_t Scalar = UINT32_C(0x10203040);
  std::vector<uint8_t> ScalarBytes(sizeof(Scalar));
  std::memcpy(ScalarBytes.data(), &Scalar, sizeof(Scalar));
  std::vector<uint8_t> Left(64);
  std::vector<uint8_t> Expected(64);
  for (unsigned Lane = 0; Lane < 16; ++Lane) {
    setLane(Left, Lane, 4, UINT32_C(0x01010101) * Lane);
    setLane(Expected, Lane, 4,
            static_cast<uint32_t>(getLane(Left, Lane, 4)) + Scalar);
  }

  BinaryImage Image = makeMemoryImage(Address, ScalarBytes);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::RAX, Address);
  Emulator.setRegisterBytes(x86reg::vectorReg(2), Left);
  Emulator.setRegisterBytes(x86reg::vectorReg(0),
                            std::vector<uint8_t>(64, 0xcc));
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(0)), Expected);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86EVEXMemoryBroadcast,
     VpcmpdBroadcastSuppressesZeroMaskAndLoadsOneActiveScalar) {
  // vpcmpltd k1 {k2}, zmm2, dword ptr [rax]{1to16}
  const std::vector<LowOp> Ops =
      liftX64({0x62, 0xf3, 0x6d, 0x5a, 0x1f, 0x08, 0x01});
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Address = 0x5000;
  constexpr uint32_t Scalar = UINT32_C(0x40000000);
  std::vector<uint8_t> Left(64);
  for (unsigned Lane = 0; Lane < 16; ++Lane)
    setLane(Left, Lane, 4, Scalar + ((Lane & 1) == 0 ? -1 : 1));

  BinaryImage Unmapped;
  Unmapped.Arch = Arch::X64;
  Unmapped.Bits = Bitness::Bits64;
  NdOpEmulator Suppressed(Unmapped);
  Suppressed.setStrictMode(true);
  Suppressed.setLoadCollect(true);
  Suppressed.setRegister(x86reg::RAX, Address);
  Suppressed.setRegister(x86reg::K2, 0);
  Suppressed.setRegister(x86reg::K1, UINT64_MAX);
  Suppressed.setRegisterBytes(x86reg::vectorReg(2), Left);
  ASSERT_EQ(Suppressed.run(Ops), Ops.size());
  EXPECT_EQ(Suppressed.getRegister(x86reg::K1), 0u);
  EXPECT_TRUE(Suppressed.getLoadRecords().empty());
  EXPECT_FALSE(Suppressed.skips().any());

  std::vector<uint8_t> ScalarBytes(sizeof(Scalar));
  std::memcpy(ScalarBytes.data(), &Scalar, sizeof(Scalar));
  BinaryImage Image = makeMemoryImage(Address, ScalarBytes);
  NdOpEmulator Active(Image);
  Active.setStrictMode(true);
  Active.setLoadCollect(true);
  Active.setRegister(x86reg::RAX, Address);
  Active.setRegister(x86reg::K2, UINT64_C(0xffff));
  Active.setRegister(x86reg::K1, UINT64_MAX);
  Active.setRegisterBytes(x86reg::vectorReg(2), Left);
  ASSERT_EQ(Active.run(Ops), Ops.size());
  EXPECT_EQ(Active.getRegister(x86reg::K1), UINT64_C(0x5555));
  ASSERT_EQ(Active.getLoadRecords().size(), 1u);
  EXPECT_EQ(Active.getLoadRecords()[0].Addr, Address);
  EXPECT_EQ(Active.getLoadRecords()[0].Size, 4u);
  EXPECT_FALSE(Active.skips().any());
}

TEST(X86EVEXMemoryBroadcast,
     VpcmpFullMemoryFamilySuppressesInactiveLanesAndLoadsOneActiveLane) {
  struct CompareCase {
    const char *Name;
    uint8_t P1;
    uint8_t Opcode;
    unsigned ElementSize;
  };
  constexpr std::array<CompareCase, 8> Cases = {{
      {"signed-byte", 0x6d, 0x3f, 1},
      {"unsigned-byte", 0x6d, 0x3e, 1},
      {"signed-word", 0xed, 0x3f, 2},
      {"unsigned-word", 0xed, 0x3e, 2},
      {"signed-dword", 0x6d, 0x1f, 4},
      {"unsigned-dword", 0x6d, 0x1e, 4},
      {"signed-qword", 0xed, 0x1f, 8},
      {"unsigned-qword", 0xed, 0x1e, 8},
  }};

  for (unsigned CaseIndex = 0; CaseIndex < Cases.size(); ++CaseIndex) {
    const CompareCase &Case = Cases[CaseIndex];
    SCOPED_TRACE(testing::Message() << "family=" << Case.Name);
    // vpcmp{lt} k1 {k2}, zmm2, zmmword ptr [rax]
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf3, Case.P1, 0x4a, Case.Opcode, 0x08, 0x01});
    ASSERT_FALSE(Ops.empty());

    const unsigned LaneCount = 64 / Case.ElementSize;
    const unsigned ActiveLane = LaneCount - 1;
    const uint64_t ActiveBit = UINT64_C(1) << ActiveLane;
    const uint64_t BaseAddress = UINT64_C(0x6000) + CaseIndex * 0x100;
    const uint64_t ActiveAddress = BaseAddress + ActiveLane * Case.ElementSize;
    std::vector<uint8_t> Left(64, 0);

    BinaryImage Unmapped;
    Unmapped.Arch = Arch::X64;
    Unmapped.Bits = Bitness::Bits64;
    NdOpEmulator Suppressed(Unmapped);
    Suppressed.setStrictMode(true);
    Suppressed.setLoadCollect(true);
    Suppressed.setRegister(x86reg::RAX, BaseAddress);
    Suppressed.setRegister(x86reg::K2, 0);
    Suppressed.setRegister(x86reg::K1, UINT64_MAX);
    Suppressed.setRegisterBytes(x86reg::vectorReg(2), Left);
    ASSERT_EQ(Suppressed.run(Ops), Ops.size());
    EXPECT_EQ(Suppressed.getRegister(x86reg::K1), 0u);
    EXPECT_TRUE(Suppressed.getLoadRecords().empty());
    EXPECT_FALSE(Suppressed.skips().any());

    BinaryImage Image =
        makeMemoryImage(ActiveAddress, scalarBytes(1, Case.ElementSize));
    NdOpEmulator Active(Image);
    Active.setStrictMode(true);
    Active.setLoadCollect(true);
    Active.setRegister(x86reg::RAX, BaseAddress);
    Active.setRegister(x86reg::K2, ActiveBit);
    Active.setRegister(x86reg::K1, UINT64_MAX);
    Active.setRegisterBytes(x86reg::vectorReg(2), Left);
    ASSERT_EQ(Active.run(Ops), Ops.size());
    EXPECT_EQ(Active.getRegister(x86reg::K1), ActiveBit);
    ASSERT_EQ(Active.getLoadRecords().size(), 1u);
    EXPECT_EQ(Active.getLoadRecords()[0].Addr, ActiveAddress);
    EXPECT_EQ(Active.getLoadRecords()[0].Size, Case.ElementSize);
    EXPECT_FALSE(Active.skips().any());
  }
}

TEST(X86EVEXMemoryBroadcast,
     VpcmpDwordQwordBroadcastFamilyUsesAtMostOneScalarLoad) {
  struct BroadcastCase {
    const char *Name;
    uint8_t P1;
    uint8_t Opcode;
    unsigned ElementSize;
  };
  constexpr std::array<BroadcastCase, 4> Cases = {{
      {"signed-dword", 0x6d, 0x1f, 4},
      {"unsigned-dword", 0x6d, 0x1e, 4},
      {"signed-qword", 0xed, 0x1f, 8},
      {"unsigned-qword", 0xed, 0x1e, 8},
  }};

  for (unsigned CaseIndex = 0; CaseIndex < Cases.size(); ++CaseIndex) {
    const BroadcastCase &Case = Cases[CaseIndex];
    SCOPED_TRACE(testing::Message() << "family=" << Case.Name);
    // vpcmp{lt} k1 {k2}, zmm2, [rax]{1toN}
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf3, Case.P1, 0x5a, Case.Opcode, 0x08, 0x01});
    ASSERT_FALSE(Ops.empty());

    constexpr uint64_t Scalar = UINT64_C(0x40000000);
    const unsigned LaneCount = 64 / Case.ElementSize;
    const uint64_t ActiveMask = (UINT64_C(1) << LaneCount) - 1;
    uint64_t Expected = 0;
    std::vector<uint8_t> Left(64);
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
      const bool Matches = (Lane & 1) == 0;
      setLane(Left, Lane, Case.ElementSize, Scalar + (Matches ? -1 : 1));
      if (Matches)
        Expected |= UINT64_C(1) << Lane;
    }

    const uint64_t Address = UINT64_C(0x7000) + CaseIndex * 0x100;
    BinaryImage Unmapped;
    Unmapped.Arch = Arch::X64;
    Unmapped.Bits = Bitness::Bits64;
    NdOpEmulator Suppressed(Unmapped);
    Suppressed.setStrictMode(true);
    Suppressed.setLoadCollect(true);
    Suppressed.setRegister(x86reg::RAX, Address);
    Suppressed.setRegister(x86reg::K2, 0);
    Suppressed.setRegister(x86reg::K1, UINT64_MAX);
    Suppressed.setRegisterBytes(x86reg::vectorReg(2), Left);
    ASSERT_EQ(Suppressed.run(Ops), Ops.size());
    EXPECT_EQ(Suppressed.getRegister(x86reg::K1), 0u);
    EXPECT_TRUE(Suppressed.getLoadRecords().empty());
    EXPECT_FALSE(Suppressed.skips().any());

    BinaryImage Image =
        makeMemoryImage(Address, scalarBytes(Scalar, Case.ElementSize));
    NdOpEmulator Active(Image);
    Active.setStrictMode(true);
    Active.setLoadCollect(true);
    Active.setRegister(x86reg::RAX, Address);
    Active.setRegister(x86reg::K2, ActiveMask);
    Active.setRegister(x86reg::K1, UINT64_MAX);
    Active.setRegisterBytes(x86reg::vectorReg(2), Left);
    ASSERT_EQ(Active.run(Ops), Ops.size());
    EXPECT_EQ(Active.getRegister(x86reg::K1), Expected);
    ASSERT_EQ(Active.getLoadRecords().size(), 1u);
    EXPECT_EQ(Active.getLoadRecords()[0].Addr, Address);
    EXPECT_EQ(Active.getLoadRecords()[0].Size, Case.ElementSize);
    EXPECT_FALSE(Active.skips().any());
  }
}

TEST(X86EVEXMemoryBroadcast,
     VpcmpRawOpcodeAndElementWidthMutationsFailClosedBeforeIr) {
  const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0x6d, 0x5a,
                                         0x1f, 0x08, 0x01};

  expectMutatedLiftFailsClosed(Encoding, [](cs_insn &Insn, cs_x86 &) {
    if (Insn.size <= 4)
      return false;
    // Turn signed VPCMPD into the unsigned opcode without updating the
    // decoded instruction family.
    Insn.bytes[4] = 0x1e;
    return true;
  });

  expectMutatedLiftFailsClosed(Encoding, [](cs_insn &, cs_x86 &X86) {
    for (uint8_t Index = 0; Index < X86.op_count; ++Index) {
      if (X86.operands[Index].type != X86_OP_MEM)
        continue;
      // Raw EVEX.b describes a dword broadcast; a qword detail tuple must
      // never be trusted as an alternate interpretation.
      X86.operands[Index].size = 8;
      return true;
    }
    return false;
  });
}

TEST(X86EVEXMemoryBroadcast,
     VpunpckldqBroadcastLoadsOnlyForSelectedMemoryResultLanes) {
  // vpunpckldq zmm1 {k2}{z}, zmm3, dword ptr [rax]{1to16}
  const std::vector<LowOp> Ops = liftX64({0x62, 0xf1, 0x65, 0xda, 0x62, 0x08});
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Address = UINT64_C(0x8000);
  constexpr uint32_t Scalar = UINT32_C(0x55667788);
  std::vector<uint8_t> Left(64);
  for (unsigned Lane = 0; Lane < 16; ++Lane)
    setLane(Left, Lane, 4, UINT32_C(0x1000) + Lane);

  BinaryImage Unmapped;
  Unmapped.Arch = Arch::X64;
  Unmapped.Bits = Bitness::Bits64;

  NdOpEmulator ZeroMask(Unmapped);
  ZeroMask.setStrictMode(true);
  ZeroMask.setLoadCollect(true);
  ZeroMask.setRegister(x86reg::RAX, Address);
  ZeroMask.setRegister(x86reg::K2, 0);
  ZeroMask.setRegisterBytes(x86reg::vectorReg(3), Left);
  ZeroMask.setRegisterBytes(x86reg::vectorReg(1),
                            std::vector<uint8_t>(64, 0xcc));
  ASSERT_EQ(ZeroMask.run(Ops), Ops.size());
  EXPECT_EQ(ZeroMask.getRegisterBytes(x86reg::vectorReg(1)),
            std::vector<uint8_t>(64, 0));
  EXPECT_TRUE(ZeroMask.getLoadRecords().empty());
  EXPECT_FALSE(ZeroMask.skips().any());

  std::vector<uint8_t> ExpectedEven(64, 0);
  for (unsigned LaneBase = 0; LaneBase < 16; LaneBase += 4) {
    setLane(ExpectedEven, LaneBase, 4, getLane(Left, LaneBase, 4));
    setLane(ExpectedEven, LaneBase + 2, 4, getLane(Left, LaneBase + 1, 4));
  }
  NdOpEmulator RegisterOnly(Unmapped);
  RegisterOnly.setStrictMode(true);
  RegisterOnly.setLoadCollect(true);
  RegisterOnly.setRegister(x86reg::RAX, Address);
  RegisterOnly.setRegister(x86reg::K2, UINT64_C(0x5555));
  RegisterOnly.setRegisterBytes(x86reg::vectorReg(3), Left);
  RegisterOnly.setRegisterBytes(x86reg::vectorReg(1),
                                std::vector<uint8_t>(64, 0xcc));
  ASSERT_EQ(RegisterOnly.run(Ops), Ops.size());
  EXPECT_EQ(RegisterOnly.getRegisterBytes(x86reg::vectorReg(1)), ExpectedEven);
  EXPECT_TRUE(RegisterOnly.getLoadRecords().empty());
  EXPECT_FALSE(RegisterOnly.skips().any());

  std::vector<uint8_t> ExpectedMemory(64, 0);
  for (unsigned Lane = 1; Lane < 16; Lane += 2)
    setLane(ExpectedMemory, Lane, 4, Scalar);
  BinaryImage Image = makeMemoryImage(Address, scalarBytes(Scalar, 4));
  NdOpEmulator MemoryOnly(Image);
  MemoryOnly.setStrictMode(true);
  MemoryOnly.setLoadCollect(true);
  MemoryOnly.setRegister(x86reg::RAX, Address);
  MemoryOnly.setRegister(x86reg::K2, UINT64_C(0xaaaa));
  MemoryOnly.setRegisterBytes(x86reg::vectorReg(3), Left);
  MemoryOnly.setRegisterBytes(x86reg::vectorReg(1),
                              std::vector<uint8_t>(64, 0xcc));
  ASSERT_EQ(MemoryOnly.run(Ops), Ops.size());
  EXPECT_EQ(MemoryOnly.getRegisterBytes(x86reg::vectorReg(1)), ExpectedMemory);
  ASSERT_EQ(MemoryOnly.getLoadRecords().size(), 1u);
  EXPECT_EQ(MemoryOnly.getLoadRecords()[0].Addr, Address);
  EXPECT_EQ(MemoryOnly.getLoadRecords()[0].Size, 4u);
  EXPECT_FALSE(MemoryOnly.skips().any());
}

TEST(X86EVEXMemoryBroadcast,
     VpunpcklqdqBroadcastMergesInactiveLanesAndLoadsOneScalar) {
  // vpunpcklqdq zmm4 {k3}, zmm5, qword ptr [r8]{1to8}
  const std::vector<LowOp> Ops = liftX64({0x62, 0xd1, 0xd5, 0x5b, 0x6c, 0x20});
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Address = UINT64_C(0x9000);
  constexpr uint64_t Scalar = UINT64_C(0x1122334455667788);
  std::vector<uint8_t> Left(64);
  std::vector<uint8_t> OldDestination(64);
  for (unsigned Lane = 0; Lane < 8; ++Lane) {
    setLane(Left, Lane, 8, UINT64_C(0x1000000000000000) + Lane);
    setLane(OldDestination, Lane, 8, UINT64_C(0xa000000000000000) + Lane);
  }

  BinaryImage Unmapped;
  Unmapped.Arch = Arch::X64;
  Unmapped.Bits = Bitness::Bits64;

  NdOpEmulator ZeroMask(Unmapped);
  ZeroMask.setStrictMode(true);
  ZeroMask.setLoadCollect(true);
  ZeroMask.setRegister(x86reg::R8, Address);
  ZeroMask.setRegister(x86reg::K3, 0);
  ZeroMask.setRegisterBytes(x86reg::vectorReg(5), Left);
  ZeroMask.setRegisterBytes(x86reg::vectorReg(4), OldDestination);
  ASSERT_EQ(ZeroMask.run(Ops), Ops.size());
  EXPECT_EQ(ZeroMask.getRegisterBytes(x86reg::vectorReg(4)), OldDestination);
  EXPECT_TRUE(ZeroMask.getLoadRecords().empty());
  EXPECT_FALSE(ZeroMask.skips().any());

  std::vector<uint8_t> ExpectedEven = OldDestination;
  for (unsigned Lane = 0; Lane < 8; Lane += 2)
    setLane(ExpectedEven, Lane, 8, getLane(Left, Lane, 8));
  NdOpEmulator RegisterOnly(Unmapped);
  RegisterOnly.setStrictMode(true);
  RegisterOnly.setLoadCollect(true);
  RegisterOnly.setRegister(x86reg::R8, Address);
  RegisterOnly.setRegister(x86reg::K3, UINT64_C(0x55));
  RegisterOnly.setRegisterBytes(x86reg::vectorReg(5), Left);
  RegisterOnly.setRegisterBytes(x86reg::vectorReg(4), OldDestination);
  ASSERT_EQ(RegisterOnly.run(Ops), Ops.size());
  EXPECT_EQ(RegisterOnly.getRegisterBytes(x86reg::vectorReg(4)), ExpectedEven);
  EXPECT_TRUE(RegisterOnly.getLoadRecords().empty());
  EXPECT_FALSE(RegisterOnly.skips().any());

  std::vector<uint8_t> ExpectedMemory = OldDestination;
  for (unsigned Lane = 1; Lane < 8; Lane += 2)
    setLane(ExpectedMemory, Lane, 8, Scalar);
  BinaryImage Image = makeMemoryImage(Address, scalarBytes(Scalar, 8));
  NdOpEmulator MemoryOnly(Image);
  MemoryOnly.setStrictMode(true);
  MemoryOnly.setLoadCollect(true);
  MemoryOnly.setRegister(x86reg::R8, Address);
  MemoryOnly.setRegister(x86reg::K3, UINT64_C(0xaa));
  MemoryOnly.setRegisterBytes(x86reg::vectorReg(5), Left);
  MemoryOnly.setRegisterBytes(x86reg::vectorReg(4), OldDestination);
  ASSERT_EQ(MemoryOnly.run(Ops), Ops.size());
  EXPECT_EQ(MemoryOnly.getRegisterBytes(x86reg::vectorReg(4)), ExpectedMemory);
  ASSERT_EQ(MemoryOnly.getLoadRecords().size(), 1u);
  EXPECT_EQ(MemoryOnly.getLoadRecords()[0].Addr, Address);
  EXPECT_EQ(MemoryOnly.getLoadRecords()[0].Size, 8u);
  EXPECT_FALSE(MemoryOnly.skips().any());
}

TEST(X86EVEXMemoryBroadcast,
     VpunpckldqXmmMergeSuppressesLoadAndClearsUpperZmmState) {
  // vpunpckldq xmm1 {k2}, xmm3, dword ptr [rax]{1to4}
  const std::vector<LowOp> Ops = liftX64({0x62, 0xf1, 0x65, 0x1a, 0x62, 0x08});
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Address = UINT64_C(0xa000);
  std::vector<uint8_t> Destination(64, 0xcc);
  std::vector<uint8_t> Expected(64, 0);
  std::copy_n(Destination.begin(), 16, Expected.begin());

  BinaryImage Unmapped;
  Unmapped.Arch = Arch::X64;
  Unmapped.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Unmapped);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::RAX, Address);
  Emulator.setRegister(x86reg::K2, 0);
  Emulator.setRegisterBytes(x86reg::vectorReg(3),
                            std::vector<uint8_t>(64, 0x31));
  Emulator.setRegisterBytes(x86reg::vectorReg(1), Destination);
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(1)), Expected);
  EXPECT_TRUE(Emulator.getLoadRecords().empty());
  EXPECT_FALSE(Emulator.skips().any());
}

} // namespace
