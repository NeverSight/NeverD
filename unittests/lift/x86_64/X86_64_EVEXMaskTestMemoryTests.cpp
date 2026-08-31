//===- X86_64_EVEXMaskTestMemoryTests.cpp - EVEX mask-test memory -------===//

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
constexpr uint64_t kMemoryAddress = 0x6000;
constexpr std::array<uint16_t, 3> kVectorSizes = {16, 32, 64};

struct MaskTestCase {
  const char *Name;
  std::array<uint8_t, 6> RegisterBytes;
  std::array<uint8_t, 6> MemoryBytes;
  uint16_t ElementSize;
  bool Negated;
};

constexpr std::array<MaskTestCase, 8> kMaskTestCases = {{
    {"byte", {0x62, 0x92, 0x1d, 0x42, 0x26, 0xcd},
     {0x62, 0xf2, 0x1d, 0x42, 0x26, 0x08}, 1, false},
    {"word", {0x62, 0x92, 0x9d, 0x42, 0x26, 0xcd},
     {0x62, 0xf2, 0x9d, 0x42, 0x26, 0x08}, 2, false},
    {"dword", {0x62, 0x92, 0x1d, 0x42, 0x27, 0xcd},
     {0x62, 0xf2, 0x1d, 0x42, 0x27, 0x08}, 4, false},
    {"qword", {0x62, 0x92, 0x9d, 0x42, 0x27, 0xcd},
     {0x62, 0xf2, 0x9d, 0x42, 0x27, 0x08}, 8, false},
    {"not-byte", {0x62, 0x92, 0x1e, 0x42, 0x26, 0xcd},
     {0x62, 0xf2, 0x1e, 0x42, 0x26, 0x08}, 1, true},
    {"not-word", {0x62, 0x92, 0x9e, 0x42, 0x26, 0xcd},
     {0x62, 0xf2, 0x9e, 0x42, 0x26, 0x08}, 2, true},
    {"not-dword", {0x62, 0x92, 0x1e, 0x42, 0x27, 0xcd},
     {0x62, 0xf2, 0x1e, 0x42, 0x27, 0x08}, 4, true},
    {"not-qword", {0x62, 0x92, 0x9e, 0x42, 0x27, 0xcd},
     {0x62, 0xf2, 0x9e, 0x42, 0x27, 0x08}, 8, true},
}};

struct BroadcastCase {
  const char *Name;
  std::array<uint8_t, 6> Bytes;
  uint16_t ElementSize;
  bool Negated;
};

constexpr std::array<BroadcastCase, 4> kBroadcastCases = {{
    {"dword", {0x62, 0xf2, 0x1d, 0x52, 0x27, 0x08}, 4, false},
    {"qword", {0x62, 0xf2, 0x9d, 0x52, 0x27, 0x08}, 8, false},
    {"not-dword", {0x62, 0xf2, 0x1e, 0x52, 0x27, 0x08}, 4, true},
    {"not-qword", {0x62, 0xf2, 0x9e, 0x52, 0x27, 0x08}, 8, true},
}};

BinaryImage emptyImage() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  return Image;
}

void addReadableValue(BinaryImage &Image, uint64_t Address, uint64_t Value,
                      uint16_t Size) {
  Segment Memory;
  Memory.VA = Address;
  Memory.Size = Size;
  Memory.Flags = SegmentFlags::Readable;
  Memory.Data.resize(Size);
  std::memcpy(Memory.Data.data(), &Value, Size);
  Image.Segments.push_back(std::move(Memory));
}

std::vector<LowOp> liftX64(const uint8_t *Bytes, size_t Size) {
  Decoder Dec;
  if (!Dec.init(Arch::X64)) {
    ADD_FAILURE() << "failed to initialize x86-64 decoder";
    return {};
  }
  DecodedInsn Insn{};
  if (Dec.decodeOneForLift(Bytes, Size, kInstructionAddress, Insn) !=
      static_cast<int>(Size)) {
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

bool runExactly(NdOpEmulator &Emulator, const std::vector<LowOp> &Ops) {
  for (size_t Index = 0; Index < Ops.size(); ++Index) {
    if (Emulator.step(Ops[Index]))
      continue;
    ADD_FAILURE() << "strict emulation stopped at LowOp " << Index << " ("
                  << ndOpName(Ops[Index].Opcode) << ")";
    return false;
  }
  return true;
}

template <typename Mutator>
void expectMutatedLiftFailsClosed(const std::array<uint8_t, 6> &Bytes,
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

bool isMaskOperand(const cs_x86_op &Operand) {
  return Operand.type == X86_OP_REG && Operand.reg >= X86_REG_K0 &&
         Operand.reg <= X86_REG_K7;
}

std::array<uint8_t, 6> withVectorSize(std::array<uint8_t, 6> Bytes,
                                     uint16_t VectorSize) {
  const uint8_t Length = VectorSize == 16 ? 0 : (VectorSize == 32 ? 0x20 : 0x40);
  Bytes[3] = static_cast<uint8_t>((Bytes[3] & ~0x60) | Length);
  return Bytes;
}

x86_avx_bcast broadcastForLaneCount(unsigned LaneCount) {
  switch (LaneCount) {
  case 2:
    return X86_AVX_BCAST_2;
  case 4:
    return X86_AVX_BCAST_4;
  case 8:
    return X86_AVX_BCAST_8;
  case 16:
    return X86_AVX_BCAST_16;
  default:
    return X86_AVX_BCAST_INVALID;
  }
}

uint64_t relevantMask(uint16_t VectorSize, uint16_t ElementSize) {
  const unsigned LaneCount = VectorSize / ElementSize;
  return LaneCount == 64 ? UINT64_MAX
                         : ((UINT64_C(1) << LaneCount) - 1);
}

uint64_t laneValue(const std::vector<uint8_t> &Bytes, unsigned Lane,
                   uint16_t ElementSize) {
  uint64_t Value = 0;
  std::memcpy(&Value, Bytes.data() + Lane * ElementSize, ElementSize);
  return Value;
}

void setLane(std::vector<uint8_t> &Bytes, unsigned Lane, uint16_t ElementSize,
             uint64_t Value) {
  ASSERT_LE((Lane + 1) * ElementSize, Bytes.size());
  std::memcpy(Bytes.data() + Lane * ElementSize, &Value, ElementSize);
}

uint64_t expectedMask(const std::vector<uint8_t> &First,
                      const std::vector<uint8_t> &Second,
                      uint16_t ElementSize, bool Negated,
                      uint64_t WriteMask) {
  if (First.size() != Second.size() || First.size() % ElementSize != 0)
    return 0;
  uint64_t Result = 0;
  const unsigned LaneCount = First.size() / ElementSize;
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    const bool IsZero =
        (laneValue(First, Lane, ElementSize) &
         laneValue(Second, Lane, ElementSize)) == 0;
    if (IsZero == Negated)
      Result |= UINT64_C(1) << Lane;
  }
  return Result & WriteMask &
         relevantMask(static_cast<uint16_t>(First.size()), ElementSize);
}

TEST(X86EVEXMaskTestMemory,
     DecoderReportsArchitecturalMaskAndMemoryTupleWidths) {
  for (const MaskTestCase &Case : kMaskTestCases) {
    for (uint16_t VectorSize : kVectorSizes) {
      SCOPED_TRACE(testing::Message() << Case.Name << '/' << VectorSize);
      const auto Bytes = withVectorSize(Case.MemoryBytes, VectorSize);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Arch::X64));
      DecodedInsn Insn{};
      ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                     kInstructionAddress, Insn),
                static_cast<int>(Bytes.size()));
      ASSERT_NE(Insn.Raw, nullptr);
      ASSERT_NE(Insn.Raw->detail, nullptr);
      const cs_x86 &X86 = Insn.Raw->detail->x86;
      ASSERT_EQ(X86.op_count, 4u);
      ASSERT_TRUE(isMaskOperand(X86.operands[0]));
      ASSERT_TRUE(isMaskOperand(X86.operands[1]));
      const uint16_t MaskSize = static_cast<uint16_t>(
          std::max(1u,
                   (VectorSize / Case.ElementSize + 7u) / 8u));
      EXPECT_EQ(X86.operands[0].size, MaskSize);
      EXPECT_EQ(X86.operands[1].size, MaskSize);
      EXPECT_EQ(X86.operands[2].size, VectorSize);
      ASSERT_EQ(X86.operands[3].type, X86_OP_MEM);
      EXPECT_EQ(X86.operands[3].size, VectorSize);
      EXPECT_EQ(X86.operands[3].avx_bcast, X86_AVX_BCAST_INVALID);
    }
  }

  for (const BroadcastCase &Case : kBroadcastCases) {
    for (uint16_t VectorSize : kVectorSizes) {
      SCOPED_TRACE(testing::Message() << Case.Name << '/' << VectorSize);
      const auto Bytes = withVectorSize(Case.Bytes, VectorSize);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Arch::X64));
      DecodedInsn Insn{};
      ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                     kInstructionAddress, Insn),
                static_cast<int>(Bytes.size()));
      ASSERT_NE(Insn.Raw, nullptr);
      ASSERT_NE(Insn.Raw->detail, nullptr);
      const cs_x86 &X86 = Insn.Raw->detail->x86;
      ASSERT_EQ(X86.op_count, 4u);
      const uint16_t MaskSize = static_cast<uint16_t>(
          std::max(1u,
                   (VectorSize / Case.ElementSize + 7u) / 8u));
      EXPECT_EQ(X86.operands[0].size, MaskSize);
      EXPECT_EQ(X86.operands[1].size, MaskSize);
      EXPECT_EQ(X86.operands[2].size, VectorSize);
      ASSERT_EQ(X86.operands[3].type, X86_OP_MEM);
      EXPECT_EQ(X86.operands[3].size, Case.ElementSize);
      EXPECT_EQ(X86.operands[3].avx_bcast,
                broadcastForLaneCount(VectorSize / Case.ElementSize));
    }
  }
}

TEST(X86EVEXMaskTestMemory,
     RegisterFamilyProducesExactZeroMaskedKBits) {
  constexpr uint64_t WriteMask = UINT64_C(0xd6b5a39c7e4d291b);
  for (const MaskTestCase &Case : kMaskTestCases) {
    for (uint16_t VectorSize : kVectorSizes) {
      SCOPED_TRACE(testing::Message() << Case.Name << '/' << VectorSize);
      const auto Bytes = withVectorSize(Case.RegisterBytes, VectorSize);
      const std::vector<LowOp> Ops = liftX64(Bytes.data(), Bytes.size());
      ASSERT_FALSE(Ops.empty());

      std::vector<uint8_t> First(VectorSize);
      std::vector<uint8_t> Second(VectorSize);
      for (unsigned Lane = 0; Lane < VectorSize / Case.ElementSize; ++Lane) {
        setLane(First, Lane, Case.ElementSize, (Lane & 1) == 0 ? 3 : 1);
        setLane(Second, Lane, Case.ElementSize, Lane % 3 == 0 ? 2 : 1);
      }

      BinaryImage Image = emptyImage();
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setRegisterBytes(x86reg::XMM28, First);
      Emulator.setRegisterBytes(x86reg::XMM29, Second);
      Emulator.setRegister(x86reg::K2, WriteMask);
      Emulator.setRegister(x86reg::K1, UINT64_MAX);
      ASSERT_TRUE(runExactly(Emulator, Ops));
      EXPECT_EQ(Emulator.getRegister(x86reg::K1),
                expectedMask(First, Second, Case.ElementSize, Case.Negated,
                             WriteMask));
      EXPECT_FALSE(Emulator.skips().any());
    }
  }
}

TEST(X86EVEXMaskTestMemory,
     FullMemoryFamilySuppressesInactiveLanesAndLoadsOnlyActiveElement) {
  for (const MaskTestCase &Case : kMaskTestCases) {
    for (uint16_t VectorSize : kVectorSizes) {
      SCOPED_TRACE(testing::Message() << Case.Name << '/' << VectorSize);
      const auto Bytes = withVectorSize(Case.MemoryBytes, VectorSize);
      const std::vector<LowOp> Ops = liftX64(Bytes.data(), Bytes.size());
      ASSERT_FALSE(Ops.empty());

      std::vector<uint8_t> First(VectorSize, 0);
      const unsigned ActiveLane = (VectorSize / Case.ElementSize) / 2;
      setLane(First, ActiveLane, Case.ElementSize, 1);

      BinaryImage Unmapped = emptyImage();
      NdOpEmulator Suppressed(Unmapped);
      Suppressed.setStrictMode(true);
      Suppressed.setLoadCollect(true);
      Suppressed.setRegister(x86reg::RAX, kMemoryAddress);
      Suppressed.setRegisterBytes(x86reg::XMM28, First);
      Suppressed.setRegister(x86reg::K2, 0);
      Suppressed.setRegister(x86reg::K1, UINT64_MAX);
      ASSERT_TRUE(runExactly(Suppressed, Ops));
      EXPECT_EQ(Suppressed.getRegister(x86reg::K1), 0u);
      EXPECT_TRUE(Suppressed.getLoadRecords().empty());
      EXPECT_FALSE(Suppressed.skips().any());

      const uint64_t ActiveBit = UINT64_C(1) << ActiveLane;
      const uint64_t MemoryValue = Case.Negated ? 0 : 1;
      BinaryImage Image = emptyImage();
      addReadableValue(Image, kMemoryAddress + ActiveLane * Case.ElementSize,
                       MemoryValue, Case.ElementSize);
      NdOpEmulator Active(Image);
      Active.setStrictMode(true);
      Active.setLoadCollect(true);
      Active.setRegister(x86reg::RAX, kMemoryAddress);
      Active.setRegisterBytes(x86reg::XMM28, First);
      Active.setRegister(x86reg::K2, ActiveBit);
      Active.setRegister(x86reg::K1, UINT64_MAX);
      ASSERT_TRUE(runExactly(Active, Ops));
      EXPECT_EQ(Active.getRegister(x86reg::K1), ActiveBit);
      ASSERT_EQ(Active.getLoadRecords().size(), 1u);
      EXPECT_EQ(Active.getLoadRecords()[0].Addr,
                kMemoryAddress + ActiveLane * Case.ElementSize);
      EXPECT_EQ(Active.getLoadRecords()[0].Size, Case.ElementSize);
      EXPECT_FALSE(Active.skips().any());
    }
  }
}

TEST(X86EVEXMaskTestMemory,
     DwordAndQwordBroadcastLoadOneScalarAndTestEveryActiveLane) {
  for (const BroadcastCase &Case : kBroadcastCases) {
    for (uint16_t VectorSize : kVectorSizes) {
      SCOPED_TRACE(testing::Message() << Case.Name << '/' << VectorSize);
      const auto Bytes = withVectorSize(Case.Bytes, VectorSize);
      const std::vector<LowOp> Ops = liftX64(Bytes.data(), Bytes.size());
      ASSERT_FALSE(Ops.empty());

      std::vector<uint8_t> First(VectorSize);
      for (unsigned Lane = 0; Lane < VectorSize / Case.ElementSize; ++Lane)
        setLane(First, Lane, Case.ElementSize, (Lane & 1) == 0 ? 1 : 0);

      BinaryImage Unmapped = emptyImage();
      NdOpEmulator Suppressed(Unmapped);
      Suppressed.setStrictMode(true);
      Suppressed.setLoadCollect(true);
      Suppressed.setRegister(x86reg::RAX, kMemoryAddress);
      Suppressed.setRegisterBytes(x86reg::XMM28, First);
      Suppressed.setRegister(x86reg::K2, 0);
      Suppressed.setRegister(x86reg::K1, UINT64_MAX);
      ASSERT_TRUE(runExactly(Suppressed, Ops));
      EXPECT_EQ(Suppressed.getRegister(x86reg::K1), 0u);
      EXPECT_TRUE(Suppressed.getLoadRecords().empty());

      const unsigned LaneCount = VectorSize / Case.ElementSize;
      const uint64_t WriteMask =
          UINT64_C(3) | (UINT64_C(1) << (LaneCount - 1));
      BinaryImage Image = emptyImage();
      addReadableValue(Image, kMemoryAddress, 1, Case.ElementSize);
      NdOpEmulator Active(Image);
      Active.setStrictMode(true);
      Active.setLoadCollect(true);
      Active.setRegister(x86reg::RAX, kMemoryAddress);
      Active.setRegisterBytes(x86reg::XMM28, First);
      Active.setRegister(x86reg::K2, WriteMask);
      Active.setRegister(x86reg::K1, UINT64_MAX);
      ASSERT_TRUE(runExactly(Active, Ops));

      std::vector<uint8_t> Broadcast(VectorSize);
      for (unsigned Lane = 0; Lane < LaneCount; ++Lane)
        setLane(Broadcast, Lane, Case.ElementSize, 1);
      EXPECT_EQ(Active.getRegister(x86reg::K1),
                expectedMask(First, Broadcast, Case.ElementSize, Case.Negated,
                             WriteMask));
      ASSERT_EQ(Active.getLoadRecords().size(), 1u);
      EXPECT_EQ(Active.getLoadRecords()[0].Addr, kMemoryAddress);
      EXPECT_EQ(Active.getLoadRecords()[0].Size, Case.ElementSize);
      EXPECT_FALSE(Active.skips().any());
    }
  }
}

TEST(X86EVEXMaskTestMemory, RawDetailMismatchFailsClosedAtomically) {
  expectMutatedLiftFailsClosed(
      kMaskTestCases[0].RegisterBytes, [](cs_insn &, cs_x86 &X86) {
        if (X86.op_count != 4 || X86.operands[2].type != X86_OP_REG)
          return false;
        X86.operands[2].reg = X86_REG_ZMM27;
        return true;
      });
  expectMutatedLiftFailsClosed(
      kMaskTestCases[2].RegisterBytes, [](cs_insn &Insn, cs_x86 &) {
        Insn.bytes[3] ^= 0x20;
        return true;
      });
  expectMutatedLiftFailsClosed(
      kMaskTestCases[3].MemoryBytes, [](cs_insn &, cs_x86 &X86) {
        if (X86.op_count != 4 || X86.operands[3].type != X86_OP_MEM)
          return false;
        X86.operands[3].size = 8;
        return true;
      });
  expectMutatedLiftFailsClosed(
      kBroadcastCases[0].Bytes, [](cs_insn &, cs_x86 &X86) {
        if (X86.op_count != 4 || X86.operands[3].type != X86_OP_MEM)
          return false;
        X86.operands[3].avx_bcast = X86_AVX_BCAST_INVALID;
        return true;
      });
}

} // namespace
