//===- X86_64_EVEXScalarBroadcastTests.cpp - EVEX scalar broadcast -------===//

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
constexpr uint64_t kMemoryAddress = 0x5000;

struct BroadcastCase {
  const char *Name;
  std::array<uint8_t, 6> MemoryBytes;
  std::array<uint8_t, 6> RegisterBytes;
  uint16_t ElementSize;
};

constexpr std::array<BroadcastCase, 6> kBroadcastCases = {{
    {"single", {0x62, 0x62, 0x7d, 0xcf, 0x18, 0x30},
     {0x62, 0x02, 0x7d, 0x4f, 0x18, 0xf5}, 4},
    {"double", {0x62, 0x62, 0xfd, 0xcf, 0x19, 0x30},
     {0x62, 0x02, 0xfd, 0x4f, 0x19, 0xf5}, 8},
    {"byte", {0x62, 0x62, 0x7d, 0xcf, 0x78, 0x30},
     {0x62, 0x02, 0x7d, 0x4f, 0x78, 0xf5}, 1},
    {"word", {0x62, 0x62, 0x7d, 0xcf, 0x79, 0x30},
     {0x62, 0x02, 0x7d, 0x4f, 0x79, 0xf5}, 2},
    {"dword", {0x62, 0x62, 0x7d, 0xcf, 0x58, 0x30},
     {0x62, 0x02, 0x7d, 0x4f, 0x58, 0xf5}, 4},
    {"qword", {0x62, 0x62, 0xfd, 0xcf, 0x59, 0x30},
     {0x62, 0x02, 0xfd, 0x4f, 0x59, 0xf5}, 8},
}};

BinaryImage emptyImage() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  return Image;
}

BinaryImage scalarImage(uint64_t Value, uint16_t Size) {
  BinaryImage Image = emptyImage();
  Segment Memory;
  Memory.VA = kMemoryAddress;
  Memory.Size = Size;
  Memory.Flags = SegmentFlags::Readable;
  Memory.Data.resize(Size);
  std::memcpy(Memory.Data.data(), &Value, Size);
  Image.Segments.push_back(std::move(Memory));
  return Image;
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

uint64_t relevantMask(uint16_t ElementSize) {
  const unsigned LaneCount = 64 / ElementSize;
  return LaneCount == 64 ? UINT64_MAX
                         : ((UINT64_C(1) << LaneCount) - 1);
}

bool isMaskOperand(const cs_x86_op &Operand) {
  return Operand.type == X86_OP_REG && Operand.reg >= X86_REG_K0 &&
         Operand.reg <= X86_REG_K7;
}

uint64_t sparseMask(uint16_t ElementSize) {
  const unsigned LaneCount = 64 / ElementSize;
  return UINT64_C(1) | (UINT64_C(1) << (LaneCount / 2)) |
         (UINT64_C(1) << (LaneCount - 1));
}

std::vector<uint8_t> expectedBroadcast(uint64_t Scalar, uint16_t ElementSize,
                                       uint64_t Mask,
                                       const std::vector<uint8_t> &Inactive) {
  std::vector<uint8_t> Result = Inactive;
  for (unsigned Lane = 0; Lane < Result.size() / ElementSize; ++Lane)
    if ((Mask & (UINT64_C(1) << Lane)) != 0)
      std::memcpy(Result.data() + Lane * ElementSize, &Scalar, ElementSize);
  return Result;
}

bool runExactly(NdOpEmulator &Emulator, const std::vector<LowOp> &Ops) {
  for (size_t Index = 0; Index < Ops.size(); ++Index) {
    if (Emulator.step(Ops[Index]))
      continue;
    ADD_FAILURE() << "strict emulation stopped at LowOp " << Index << " ("
                  << ndOpName(Ops[Index].Opcode) << "), output size "
                  << Ops[Index].Output.Size << ", inputs "
                  << static_cast<unsigned>(Ops[Index].NumInputs);
    return false;
  }
  return true;
}

TEST(X86EVEXScalarBroadcast,
     DecoderReportsArchitecturalMaskWidthForEveryElementSize) {
  for (const BroadcastCase &Case : kBroadcastCases) {
    SCOPED_TRACE(Case.Name);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Case.MemoryBytes.data(),
                                   Case.MemoryBytes.size(),
                                   kInstructionAddress, Insn),
              static_cast<int>(Case.MemoryBytes.size()));
    ASSERT_NE(Insn.Raw, nullptr);
    ASSERT_NE(Insn.Raw->detail, nullptr);
    const cs_x86 &X86 = Insn.Raw->detail->x86;
    ASSERT_EQ(X86.op_count, 3u);
    ASSERT_TRUE(isMaskOperand(X86.operands[1]));
    EXPECT_EQ(X86.operands[1].size,
              std::max<unsigned>(1, (64 / Case.ElementSize + 7) / 8));
  }
}

TEST(X86EVEXScalarBroadcast,
     MaskedRegisterFamilyBroadcastsLowElementAndMergesInactiveLanes) {
  constexpr uint64_t Scalar = UINT64_C(0x8877665544332211);
  for (const BroadcastCase &Case : kBroadcastCases) {
    SCOPED_TRACE(Case.Name);
    const std::vector<LowOp> Ops =
        liftX64(Case.RegisterBytes.data(), Case.RegisterBytes.size());
    ASSERT_FALSE(Ops.empty());

    const uint64_t Mask = sparseMask(Case.ElementSize);
    std::vector<uint8_t> OldDestination(64);
    std::vector<uint8_t> Source(64, 0xcc);
    for (unsigned Index = 0; Index < OldDestination.size(); ++Index)
      OldDestination[Index] = static_cast<uint8_t>(0x40 + Index);
    std::memcpy(Source.data(), &Scalar, sizeof(Scalar));

    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(x86reg::XMM30, OldDestination);
    Emulator.setRegisterBytes(x86reg::XMM29, Source);
    Emulator.setRegister(x86reg::K7, Mask);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM30),
              expectedBroadcast(Scalar, Case.ElementSize, Mask,
                                OldDestination));
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86EVEXScalarBroadcast,
     MaskedMemoryFamilySuppressesZeroMaskAndLoadsOneActiveScalar) {
  constexpr uint64_t Scalar = UINT64_C(0x1020304050607080);
  for (const BroadcastCase &Case : kBroadcastCases) {
    SCOPED_TRACE(Case.Name);
    const std::vector<LowOp> Ops =
        liftX64(Case.MemoryBytes.data(), Case.MemoryBytes.size());
    ASSERT_FALSE(Ops.empty());

    BinaryImage Unmapped = emptyImage();
    NdOpEmulator Suppressed(Unmapped);
    Suppressed.setStrictMode(true);
    Suppressed.setLoadCollect(true);
    Suppressed.setRegister(x86reg::RAX, kMemoryAddress);
    Suppressed.setRegister(x86reg::K7, 0);
    Suppressed.setRegisterBytes(x86reg::XMM30,
                                std::vector<uint8_t>(64, 0xa5));
    ASSERT_EQ(Suppressed.run(Ops), Ops.size());
    EXPECT_EQ(Suppressed.getRegisterBytes(x86reg::XMM30),
              std::vector<uint8_t>(64, 0));
    EXPECT_TRUE(Suppressed.getLoadRecords().empty());
    EXPECT_FALSE(Suppressed.skips().any());

    const uint64_t Mask = sparseMask(Case.ElementSize);
    BinaryImage Image = scalarImage(Scalar, Case.ElementSize);
    NdOpEmulator Active(Image);
    Active.setStrictMode(true);
    Active.setLoadCollect(true);
    Active.setRegister(x86reg::RAX, kMemoryAddress);
    Active.setRegister(x86reg::K7, Mask & relevantMask(Case.ElementSize));
    Active.setRegisterBytes(x86reg::XMM30,
                            std::vector<uint8_t>(64, 0xa5));
    ASSERT_TRUE(runExactly(Active, Ops));
    EXPECT_EQ(Active.getRegisterBytes(x86reg::XMM30),
              expectedBroadcast(Scalar, Case.ElementSize, Mask,
                                std::vector<uint8_t>(64, 0)));
    ASSERT_EQ(Active.getLoadRecords().size(), 1u);
    EXPECT_EQ(Active.getLoadRecords()[0].Addr, kMemoryAddress);
    EXPECT_EQ(Active.getLoadRecords()[0].Size, Case.ElementSize);
    EXPECT_FALSE(Active.skips().any());
  }
}

TEST(X86EVEXScalarBroadcast,
     GprSourceFamilyUsesLowElementWithMergeAndZeroMasks) {
  struct GprCase {
    const char *Name;
    std::array<uint8_t, 6> Bytes;
    uint16_t ElementSize;
    bool Zeroing;
  };
  constexpr std::array<GprCase, 4> Cases = {{
      {"byte", {0x62, 0x42, 0x7d, 0xcf, 0x7a, 0xf1}, 1, true},
      {"word", {0x62, 0x42, 0x7d, 0x4f, 0x7b, 0xf1}, 2, false},
      {"dword", {0x62, 0x42, 0x7d, 0xcf, 0x7c, 0xf1}, 4, true},
      {"qword", {0x62, 0x42, 0xfd, 0x4f, 0x7c, 0xf1}, 8, false},
  }};
  constexpr uint64_t Scalar = UINT64_C(0x8877665544332211);

  for (const GprCase &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    const std::vector<LowOp> Ops = liftX64(Case.Bytes.data(), Case.Bytes.size());
    ASSERT_FALSE(Ops.empty());
    const uint64_t Mask = sparseMask(Case.ElementSize);
    std::vector<uint8_t> OldDestination(64);
    for (unsigned Index = 0; Index < OldDestination.size(); ++Index)
      OldDestination[Index] = static_cast<uint8_t>(0x90 + Index);
    const std::vector<uint8_t> Inactive =
        Case.Zeroing ? std::vector<uint8_t>(64, 0) : OldDestination;

    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(x86reg::XMM30, OldDestination);
    Emulator.setRegister(x86reg::R9, Scalar);
    Emulator.setRegister(x86reg::K7, Mask);
    ASSERT_TRUE(runExactly(Emulator, Ops));
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM30),
              expectedBroadcast(Scalar, Case.ElementSize, Mask, Inactive));
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86EVEXScalarBroadcast,
     ExtendedGprSourceFamilyUsesArchitecturalB4AcrossVectorLengths) {
  struct ExtendedGprCase {
    const char *Name;
    std::array<uint8_t, 6> Bytes;
    uint16_t VectorSize;
    uint16_t ElementSize;
    bool HasMask;
    bool Zeroing;
  };
  constexpr std::array<ExtendedGprCase, 4> Cases = {{
      {"byte-xmm", {0x62, 0x4a, 0x7d, 0x08, 0x7a, 0xf5}, 16, 1, false,
       false},
      {"word-ymm", {0x62, 0x4a, 0x7d, 0x2f, 0x7b, 0xf5}, 32, 2, true,
       false},
      {"dword-zmm", {0x62, 0x4a, 0x7d, 0xcf, 0x7c, 0xf5}, 64, 4, true,
       true},
      {"qword-zmm", {0x62, 0x4a, 0xfd, 0x4f, 0x7c, 0xf5}, 64, 8, true,
       false},
  }};
  constexpr uint64_t Scalar = UINT64_C(0x8877665544332211);

  for (const ExtendedGprCase &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    const std::vector<LowOp> Ops = liftX64(Case.Bytes.data(), Case.Bytes.size());
    ASSERT_FALSE(Ops.empty());

    const uint64_t Mask =
        Case.HasMask ? sparseMask(Case.ElementSize) : UINT64_MAX;
    std::vector<uint8_t> OldDestination(64);
    for (unsigned Index = 0; Index < OldDestination.size(); ++Index)
      OldDestination[Index] = static_cast<uint8_t>(0xb0 + Index);
    std::vector<uint8_t> Inactive(
        OldDestination.begin(), OldDestination.begin() + Case.VectorSize);
    if (Case.Zeroing || !Case.HasMask)
      std::fill(Inactive.begin(), Inactive.end(), 0);
    std::vector<uint8_t> Expected =
        expectedBroadcast(Scalar, Case.ElementSize, Mask, Inactive);
    Expected.resize(64, 0);

    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(x86reg::XMM30, OldDestination);
    Emulator.setRegister(x86reg::R29, Scalar);
    if (Case.HasMask)
      Emulator.setRegister(x86reg::K7, Mask);
    ASSERT_TRUE(runExactly(Emulator, Ops));
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM30), Expected);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86EVEXScalarBroadcast, RawDetailMismatchFailsClosedAtomically) {
  constexpr std::array<uint8_t, 6> ExtendedDword = {
      0x62, 0x4a, 0x7d, 0xcf, 0x7c, 0xf5};
  expectMutatedLiftFailsClosed(
      kBroadcastCases[4].RegisterBytes,
      [](cs_insn &, cs_x86 &X86) {
        if (X86.op_count != 3 || X86.operands[2].type != X86_OP_REG)
          return false;
        X86.operands[2].reg = X86_REG_XMM28;
        return true;
      });
  expectMutatedLiftFailsClosed(
      kBroadcastCases[0].MemoryBytes,
      [](cs_insn &, cs_x86 &X86) {
        if (X86.op_count != 3 || !isMaskOperand(X86.operands[1]))
          return false;
        X86.operands[1].avx_zero_opmask = false;
        return true;
      });
  expectMutatedLiftFailsClosed(
      ExtendedDword,
      [](cs_insn &, cs_x86 &X86) {
        if (X86.op_count != 3 || X86.operands[2].type != X86_OP_REG ||
            X86.operands[2].reg != X86_REG_R29D)
          return false;
        X86.operands[2].reg = X86_REG_R28D;
        return true;
      });
}

} // namespace
