//===- X86_64_APXEVEXExistingGprTests.cpp - APX EVEX GPR semantics ------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
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

template <typename T> std::vector<uint8_t> bytes(const std::vector<T> &Values) {
  std::vector<uint8_t> Result(Values.size() * sizeof(T));
  std::memcpy(Result.data(), Values.data(), Result.size());
  return Result;
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

const cs_x86_op *findMemoryOperand(const cs_x86 &X86) {
  for (unsigned Index = 0; Index < X86.op_count; ++Index)
    if (X86.operands[Index].type == X86_OP_MEM)
      return &X86.operands[Index];
  return nullptr;
}

size_t bitCount(uint64_t Value) {
  size_t Count = 0;
  while (Value != 0) {
    Value &= Value - 1;
    ++Count;
  }
  return Count;
}

void expectGenericRex2DecodeLiftAndEmulate();
void expectPackedConvertControlAndMemoryForms();
void expectExplicitMsrInvalidateAndHighCFailClosed();

void expectMemoryDetail(const std::vector<uint8_t> &Bytes, x86_reg Base,
                        x86_reg Index, int Scale, int64_t Displacement) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                 kInstructionAddress, Insn),
            static_cast<int>(Bytes.size()));
  ASSERT_NE(Insn.Raw, nullptr);
  ASSERT_NE(Insn.Raw->detail, nullptr);
  const cs_x86 &X86 = Insn.Raw->detail->x86;
  const cs_x86_op *Memory = findMemoryOperand(X86);
  ASSERT_NE(Memory, nullptr);
  EXPECT_EQ(Memory->mem.base, Base);
  EXPECT_EQ(Memory->mem.index, Index);
  EXPECT_EQ(Memory->mem.scale, Scale);
  EXPECT_EQ(Memory->mem.disp, Displacement);
}

TEST(X86APXEVEXExistingGpr,
     PackedFloatMemoryUsesExtendedBaseAndOrdinaryIndexExactly) {
  struct AddressCase {
    const char *Name;
    std::vector<uint8_t> Encoding;
    bool HasIndex;
  };
  const std::vector<AddressCase> Cases = {
      {"base", {0x62, 0xd9, 0x64, 0x4d, 0x58, 0x55, 0x01}, false},
      {"base-index", {0x62, 0x99, 0x60, 0x4d, 0x58, 0x54, 0x75, 0x01}, true},
  };
  constexpr uint64_t Base = UINT64_C(0x5000);
  constexpr uint64_t Index = UINT64_C(0x20);
  constexpr uint64_t Mask = UINT64_C(0xa55a);

  std::vector<float> Left(16), Right(16), OldDestination(16), Expected(16);
  for (unsigned Lane = 0; Lane < Left.size(); ++Lane) {
    Left[Lane] = static_cast<float>(Lane + 1);
    Right[Lane] = static_cast<float>(Lane + 17);
    OldDestination[Lane] = static_cast<float>(-100 - static_cast<int>(Lane));
    Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                         ? Left[Lane] + Right[Lane]
                         : OldDestination[Lane];
  }

  for (const AddressCase &Test : Cases) {
    SCOPED_TRACE(Test.Name);
    const uint64_t Address = Base + 64 + (Test.HasIndex ? Index * 2 : 0);
    expectMemoryDetail(Test.Encoding, X86_REG_R29,
                       Test.HasIndex ? X86_REG_R30 : X86_REG_INVALID,
                       Test.HasIndex ? 2 : 1, 64);
    const std::vector<LowOp> Ops = liftX64(Test.Encoding);
    ASSERT_FALSE(Ops.empty());

    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, Right.data(),
                     Right.size() * sizeof(float));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::R29, Base);
    Emulator.setRegister(x86reg::R30, Index);
    Emulator.setRegister(x86reg::K5, Mask);
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Left));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(OldDestination));

    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM2), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), bitCount(Mask));
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXEVEXExistingGpr,
     MapThreeCompareUsesExtendedBaseAndIndexWithMaskedLoads) {
  // vpcmpled k6 {k7}, zmm4, [r29 + r30*8 + 64]
  const std::vector<uint8_t> Encoding = {0x62, 0x9b, 0x59, 0x4f, 0x1f,
                                         0x74, 0xf5, 0x01, 0x02};
  constexpr uint64_t Base = UINT64_C(0x7000);
  constexpr uint64_t Index = UINT64_C(0x10);
  constexpr uint64_t Mask = UINT64_C(0xa55a);
  constexpr uint64_t Address = Base + Index * 8 + 64;
  std::vector<int32_t> Left(16), Right(16);
  uint64_t Expected = 0;
  for (unsigned Lane = 0; Lane < Left.size(); ++Lane) {
    Left[Lane] = static_cast<int32_t>(Lane) - 8;
    Right[Lane] = Lane % 3 == 0 ? Left[Lane] - 1 : Left[Lane] + 1;
    if ((Mask & (UINT64_C(1) << Lane)) != 0 && Left[Lane] <= Right[Lane])
      Expected |= UINT64_C(1) << Lane;
  }

  expectMemoryDetail(Encoding, X86_REG_R29, X86_REG_R30, 8, 64);
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());
  BinaryImage Image = emptyImage();
  addReadableBytes(Image, Address, Right.data(),
                   Right.size() * sizeof(int32_t));
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::R29, Base);
  Emulator.setRegister(x86reg::R30, Index);
  Emulator.setRegister(x86reg::K7, Mask);
  Emulator.setRegister(x86reg::K6, UINT64_MAX);
  Emulator.setRegisterBytes(x86reg::XMM4, bytes(Left));

  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  ASSERT_TRUE(Emulator.getRegister(x86reg::K6));
  EXPECT_EQ(*Emulator.getRegister(x86reg::K6), Expected);
  EXPECT_EQ(Emulator.getLoadRecords().size(), bitCount(Mask));
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXEVEXExistingGpr,
     ScalarConversionReadsExtendedGprAndPreservesMergeSource) {
  // vcvtsi2ss xmm1, xmm2, r29d
  const std::vector<uint8_t> Encoding = {0x62, 0xd9, 0x6e, 0x08, 0x2a, 0xcd};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Encoding.data(), Encoding.size(),
                                 kInstructionAddress, Insn),
            static_cast<int>(Encoding.size()));
  ASSERT_NE(Insn.Raw, nullptr);
  ASSERT_NE(Insn.Raw->detail, nullptr);
  ASSERT_EQ(Insn.Raw->detail->x86.op_count, 3u);
  EXPECT_EQ(Insn.Raw->detail->x86.operands[2].reg, X86_REG_R29D);
  EXPECT_EQ(Insn.Raw->detail->x86.operands[2].size, 4u);

  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());
  std::vector<uint8_t> MergeSource(64, 0);
  for (unsigned Index = 0; Index < 16; ++Index)
    MergeSource[Index] = static_cast<uint8_t>(0x40 + Index);
  std::vector<uint8_t> Expected(64, 0);
  std::copy(MergeSource.begin() + 4, MergeSource.begin() + 16,
            Expected.begin() + 4);
  const float Converted = -13.0f;
  std::memcpy(Expected.data(), &Converted, sizeof(Converted));

  BinaryImage Image = emptyImage();
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::R29,
                       static_cast<uint64_t>(static_cast<int64_t>(-13)));
  Emulator.setRegisterBytes(x86reg::XMM2, MergeSource);
  Emulator.setRegisterBytes(x86reg::XMM1, std::vector<uint8_t>(64, 0xcc));
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), Expected);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXEVEXExistingGpr,
     VsibKeepsVectorIndexEncodingWhileExtendingOnlyTheBase) {
  // vpgatherdd zmm1 {k2}, [r29 + zmm30*4]
  const std::vector<uint8_t> Encoding = {0x62, 0x9a, 0x79, 0x42,
                                         0x90, 0x4c, 0xb5, 0x00};
  constexpr uint64_t Base = UINT64_C(0x9000);
  constexpr uint64_t Mask = UINT64_C(0x8421);
  std::vector<int32_t> Indices(16);
  std::vector<uint32_t> OldDestination(16), Expected(16);
  std::vector<uint8_t> Memory(256, 0);
  for (unsigned Lane = 0; Lane < Indices.size(); ++Lane) {
    Indices[Lane] = static_cast<int32_t>(Lane * 4);
    OldDestination[Lane] = UINT32_C(0xa0000000) + Lane;
    const uint32_t Loaded = UINT32_C(0x50000000) + Lane;
    std::memcpy(Memory.data() + Lane * 16, &Loaded, sizeof(Loaded));
    Expected[Lane] =
        (Mask & (UINT64_C(1) << Lane)) != 0 ? Loaded : OldDestination[Lane];
  }

  expectMemoryDetail(Encoding, X86_REG_R29, X86_REG_ZMM30, 4, 0);
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());
  BinaryImage Image = emptyImage();
  addReadableBytes(Image, Base, Memory.data(), Memory.size());
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::R29, Base);
  Emulator.setRegisterBytes(x86reg::XMM30, bytes(Indices));
  Emulator.setRegister(x86reg::K2, UINT64_C(0xfedc000000000000) | Mask);
  Emulator.setRegisterBytes(x86reg::XMM1, bytes(OldDestination));

  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
  EXPECT_EQ(Emulator.getRegister(x86reg::K2), 0u);
  EXPECT_EQ(Emulator.getLoadRecords().size(), bitCount(Mask));
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXEVEXExistingGpr,
     CryptoMemoryFormUsesExtendedOrdinaryAddressRegisters) {
  // vaesdec zmm1, zmm2, [r29 + r30*2 + 64]
  const std::vector<uint8_t> Encoding = {0x62, 0x9a, 0x69, 0x48,
                                         0xde, 0x4c, 0x75, 0x01};
  constexpr uint64_t Base = UINT64_C(0xb000);
  constexpr uint64_t Index = UINT64_C(0x20);
  constexpr uint64_t Address = Base + Index * 2 + 64;
  expectMemoryDetail(Encoding, X86_REG_R29, X86_REG_R30, 2, 64);
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());

  const std::vector<uint8_t> Zeros(64, 0);
  BinaryImage Image = emptyImage();
  addReadableBytes(Image, Address, Zeros.data(), Zeros.size());
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::R29, Base);
  Emulator.setRegister(x86reg::R30, Index);
  Emulator.setRegisterBytes(x86reg::XMM2, Zeros);
  Emulator.setRegisterBytes(x86reg::XMM1, std::vector<uint8_t>(64, 0xa5));
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1),
            std::vector<uint8_t>(64, 0x52));
  ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
  EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Address);
  EXPECT_EQ(Emulator.getLoadRecords()[0].Size, 64u);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXEVEXExistingGpr,
     MaskedMoveUsesExtendedAddressAndSuppressesInactiveLoads) {
  // vmovdqu32 zmm1 {k3}{z}, [r29 + r30*4 + 64]
  const std::vector<uint8_t> Encoding = {0x62, 0x99, 0x7a, 0xcb,
                                         0x6f, 0x4c, 0xb5, 0x01};
  constexpr uint64_t Base = UINT64_C(0xd000);
  constexpr uint64_t Index = UINT64_C(0x20);
  constexpr uint64_t Address = Base + Index * 4 + 64;
  constexpr uint64_t Mask = UINT64_C(0x9249);
  std::vector<uint32_t> Memory(16), Expected(16, 0);
  for (unsigned Lane = 0; Lane < Memory.size(); ++Lane) {
    Memory[Lane] = UINT32_C(0x70000000) + Lane;
    if ((Mask & (UINT64_C(1) << Lane)) != 0)
      Expected[Lane] = Memory[Lane];
  }

  expectMemoryDetail(Encoding, X86_REG_R29, X86_REG_R30, 4, 64);
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());
  BinaryImage Image = emptyImage();
  addReadableBytes(Image, Address, Memory.data(),
                   Memory.size() * sizeof(uint32_t));
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::R29, Base);
  Emulator.setRegister(x86reg::R30, Index);
  Emulator.setRegister(x86reg::K3, Mask);
  Emulator.setRegisterBytes(x86reg::XMM1, std::vector<uint8_t>(64, 0xa5));
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
  EXPECT_EQ(Emulator.getLoadRecords().size(), bitCount(Mask));
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXEVEXExistingGpr,
     FullVectorMovesPreserveMaskFaultAndAlignmentSemantics) {
  struct LoadCase {
    const char *Name;
    std::vector<uint8_t> Encoding;
    uint16_t ElementSize;
    bool RequiresAlignment;
  };
  const std::vector<LoadCase> Loads = {
      {"dqu8", {0x62, 0xf1, 0x7f, 0xcb, 0x6f, 0x08}, 1, false},
      {"dqu16", {0x62, 0xf1, 0xff, 0xcb, 0x6f, 0x08}, 2, false},
      {"dqu32", {0x62, 0xf1, 0x7e, 0xcb, 0x6f, 0x08}, 4, false},
      {"dqu64", {0x62, 0xf1, 0xfe, 0xcb, 0x6f, 0x08}, 8, false},
      {"dqa32", {0x62, 0xf1, 0x7d, 0xcb, 0x6f, 0x08}, 4, true},
      {"dqa64", {0x62, 0xf1, 0xfd, 0xcb, 0x6f, 0x08}, 8, true},
      {"aps", {0x62, 0xf1, 0x7c, 0xcb, 0x28, 0x08}, 4, true},
      {"apd", {0x62, 0xf1, 0xfd, 0xcb, 0x28, 0x08}, 8, true},
      {"ups", {0x62, 0xf1, 0x7c, 0xcb, 0x10, 0x08}, 4, false},
      {"upd", {0x62, 0xf1, 0xfd, 0xcb, 0x10, 0x08}, 8, false},
  };
  constexpr uint64_t AlignedAddress = UINT64_C(0x26000);
  constexpr uint64_t UnalignedAddress = UINT64_C(0x26003);
  constexpr uint64_t Mask = UINT64_C(0x9249249249249249);
  std::vector<uint8_t> Memory(64), OldDestination(64, 0xa5);
  for (unsigned Index = 0; Index < Memory.size(); ++Index)
    Memory[Index] = static_cast<uint8_t>(Index * 3 + 1);

  for (const LoadCase &Test : Loads) {
    SCOPED_TRACE(Test.Name);
    const uint64_t Address =
        Test.RequiresAlignment ? AlignedAddress : UnalignedAddress;
    std::vector<uint8_t> Expected(64, 0);
    const unsigned LaneCount = 64 / Test.ElementSize;
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
      if ((Mask & (UINT64_C(1) << Lane)) == 0)
        continue;
      const size_t Offset = static_cast<size_t>(Lane) * Test.ElementSize;
      std::copy_n(Memory.begin() + Offset, Test.ElementSize,
                  Expected.begin() + Offset);
    }

    const std::vector<LowOp> Ops = liftX64(Test.Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, Memory.data(), Memory.size());
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, OldDestination);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), Expected);
    const uint64_t RelevantMask =
        LaneCount == 64 ? Mask : Mask & ((UINT64_C(1) << LaneCount) - 1);
    EXPECT_EQ(Emulator.getLoadRecords().size(), bitCount(RelevantMask));
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Any active VMOVDQA32 lane retains the full-vector alignment requirement,
  // and the check must precede the first memory effect or destination update.
  const std::vector<uint8_t> AlignedLoad = {0x62, 0xf1, 0x7d, 0xcb, 0x6f, 0x08};
  const std::vector<LowOp> AlignedLoadOps = liftX64(AlignedLoad);
  ASSERT_FALSE(AlignedLoadOps.empty());
  {
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, UnalignedAddress, Memory.data(), Memory.size());
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, UnalignedAddress);
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, OldDestination);
    EXPECT_LT(Emulator.run(AlignedLoadOps), AlignedLoadOps.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), OldDestination);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());
  }

  // With every lane masked off, AVX-512 suppresses the memory operation and
  // its possible fault. Zero-masking still clears the destination lanes.
  {
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);

    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, UnalignedAddress);
    Emulator.setRegister(x86reg::K3, 0);
    Emulator.setRegisterBytes(x86reg::XMM1, OldDestination);
    ASSERT_EQ(Emulator.run(AlignedLoadOps), AlignedLoadOps.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1),
              std::vector<uint8_t>(64, 0));
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vmovaps [rax] {k3}, zmm19 performs element-granular masked stores while
  // retaining the aligned-move contract.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xe1, 0x7c, 0x4b, 0x29, 0x18};
    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    std::vector<uint32_t> Initial(16), Source(16), Expected(16);
    for (unsigned Lane = 0; Lane < Initial.size(); ++Lane) {
      Initial[Lane] = UINT32_C(0x10000000) + Lane;
      Source[Lane] = UINT32_C(0x70000000) + Lane;
      Expected[Lane] =
          (Mask & (UINT64_C(1) << Lane)) != 0 ? Source[Lane] : Initial[Lane];
    }
    BinaryImage Image = emptyImage();
    Segment Mapping;
    Mapping.VA = AlignedAddress;
    Mapping.Size = 64;
    Mapping.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Mapping.Data = bytes(Initial);
    Image.Segments.push_back(std::move(Mapping));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::RAX, AlignedAddress);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM19, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());

    std::vector<uint8_t> Actual(64);
    for (unsigned Offset = 0; Offset < Actual.size(); Offset += 8) {
      LowOp Load;
      Load.Opcode = NdOp::LOAD;
      Load.Output = NdVar::tmp(UINT64_C(0x7e000000) + Offset, 8);
      Load.addInput(NdVar::cst(0, 8));
      Load.addInput(NdVar::cst(AlignedAddress + Offset, 8));
      ASSERT_TRUE(Emulator.step(Load));
      const std::optional<uint64_t> Value =
          Emulator.getRegister(UINT64_C(0x7e000000) + Offset);
      ASSERT_TRUE(Value);
      std::memcpy(Actual.data() + Offset, &*Value, 8);
    }
    EXPECT_EQ(Actual, bytes(Expected));
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Register forms use the same architectural writemask but have no memory
  // or alignment side effects.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xb1, 0x7d, 0xcb, 0x6f, 0xcb};
    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    std::vector<uint32_t> Old(16), Source(16), Expected(16);
    for (unsigned Lane = 0; Lane < Expected.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x20000000) + Lane;
      Source[Lane] = UINT32_C(0x80000000) + Lane;
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0 ? Source[Lane] : 0;
    }
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM19, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXEVEXExistingGpr, EvexF16CRespectsMasksMemoryFaultsRoundingAndSae) {
  constexpr uint64_t Base = UINT64_C(0x2a000);
  constexpr uint64_t Index = UINT64_C(0x10);
  constexpr uint64_t Address = Base + Index * 2 + 32;

  // vcvtph2ps zmm1 {k3}{z}, [r29 + r30*2 + 32]. Masked-off signaling
  // NaNs neither load nor raise the invalid exception.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0x9a, 0x79, 0xcb,
                                           0x13, 0x4c, 0x75, 0x01};
    constexpr uint64_t Mask =
        (UINT64_C(1) << 0) | (UINT64_C(1) << 2) | (UINT64_C(1) << 15);
    std::vector<uint16_t> Memory(16, UINT16_C(0x7c01));
    Memory[0] = UINT16_C(0x3c00);
    Memory[2] = UINT16_C(0xc000);
    Memory[15] = UINT16_C(0x4200);
    std::vector<uint32_t> Expected(16, 0);
    Expected[0] = UINT32_C(0x3f800000);
    Expected[2] = UINT32_C(0xc0000000);
    Expected[15] = UINT32_C(0x40400000);

    expectMemoryDetail(Encoding, X86_REG_R29, X86_REG_R30, 2, 32);
    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, Memory.data(), Memory.size() * 2);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setMXCSR(0x1f00); // Invalid is unmasked.
    Emulator.setRegister(x86reg::R29, Base);
    Emulator.setRegister(x86reg::R30, Index);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, std::vector<uint8_t>(64, 0xa5));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f00U);
    EXPECT_EQ(Emulator.getLoadRecords().size(), bitCount(Mask));
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Register-source {sae} suppresses the active signaling-NaN exception but
  // still produces the architecturally quieted result.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xb2, 0x7d, 0x9b, 0x13, 0xcb};
    std::vector<uint16_t> Source(16, 0);
    Source[0] = UINT16_C(0x7c01);
    std::vector<uint32_t> Expected(16, 0);
    Expected[0] = UINT32_C(0x7fc02000);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00);
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM19, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vcvtps2ph ymm1 {k3}{z}, zmm19, 2 uses round-up and suppresses conversion
  // exceptions from every inactive source lane.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xe3, 0x7d, 0xcb,
                                           0x1d, 0xd9, 0x02};
    constexpr uint64_t Mask =
        (UINT64_C(1) << 0) | (UINT64_C(1) << 3) | (UINT64_C(1) << 15);
    std::vector<uint32_t> Source(16, UINT32_C(0x7f800001));
    Source[0] = UINT32_C(0x3f801000);
    Source[3] = UINT32_C(0xbf801000);
    Source[15] = UINT32_C(0x40400000);
    std::vector<uint16_t> Expected(16, 0);
    Expected[0] = UINT16_C(0x3c01);
    Expected[3] = UINT16_C(0xbc00);
    Expected[15] = UINT16_C(0x4200);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM19, bytes(Source));
    Emulator.setRegisterBytes(x86reg::XMM1, std::vector<uint8_t>(64, 0xa5));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    std::vector<uint8_t> ExpectedBytes = bytes(Expected);
    ExpectedBytes.resize(64, 0);
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), ExpectedBytes);
    EXPECT_EQ(Emulator.getMXCSR() & 1U, 0U);
    EXPECT_NE(Emulator.getMXCSR() & 0x20U, 0U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vcvtps2ph [r29 + r30*2 + 32] {k3}, zmm19, 2 stores only active half
  // elements and leaves masked-off memory bytes untouched.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0x8b, 0x79, 0x4b, 0x1d,
                                           0x5c, 0x75, 0x01, 0x02};
    constexpr uint64_t Mask =
        (UINT64_C(1) << 0) | (UINT64_C(1) << 5) | (UINT64_C(1) << 15);
    std::vector<uint32_t> Source(16, UINT32_C(0x3f800000));
    Source[5] = UINT32_C(0xc0000000);
    Source[15] = UINT32_C(0x40400000);
    std::vector<uint16_t> Initial(16), Expected(16);
    for (unsigned Lane = 0; Lane < Initial.size(); ++Lane) {
      Initial[Lane] = static_cast<uint16_t>(0x1000 + Lane);
      Expected[Lane] = Initial[Lane];
    }
    Expected[0] = UINT16_C(0x3c00);
    Expected[5] = UINT16_C(0xc000);
    Expected[15] = UINT16_C(0x4200);

    expectMemoryDetail(Encoding, X86_REG_R29, X86_REG_R30, 2, 32);
    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    Segment Mapping;
    Mapping.VA = Address;
    Mapping.Size = Initial.size() * 2;
    Mapping.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Mapping.Data = bytes(Initial);
    Image.Segments.push_back(std::move(Mapping));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::R29, Base);
    Emulator.setRegister(x86reg::R30, Index);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM19, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());

    std::vector<uint8_t> Actual(32);
    for (unsigned Offset = 0; Offset < Actual.size(); Offset += 8) {
      LowOp Load;
      Load.Opcode = NdOp::LOAD;
      Load.Output = NdVar::tmp(UINT64_C(0x7d000000) + Offset, 8);
      Load.addInput(NdVar::cst(0, 8));
      Load.addInput(NdVar::cst(Address + Offset, 8));
      ASSERT_TRUE(Emulator.step(Load));
      const std::optional<uint64_t> Value =
          Emulator.getRegister(UINT64_C(0x7d000000) + Offset);
      ASSERT_TRUE(Value);
      std::memcpy(Actual.data() + Offset, &*Value, 8);
    }
    EXPECT_EQ(Actual, bytes(Expected));
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXEVEXExistingGpr,
     EvexArithmeticUsesArchitecturalRoundingSaeAndActiveLanes) {
  // vaddps zmm1 {k3}, zmm2, zmm3, {rd-sae}.  The exact sum is halfway
  // between -1 and its next representable value toward -infinity.  Embedded
  // round-down must override MXCSR round-up, and SAE must suppress both the
  // precision status and an active signaling-NaN exception.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6c, 0x3b, 0x58, 0xcb};
    std::vector<uint32_t> Left(16, UINT32_C(0xbf800000));
    std::vector<uint32_t> Right(16, UINT32_C(0xb3800000));
    std::vector<uint32_t> Expected(16, UINT32_C(0xbf800001));
    Right[1] = UINT32_C(0x7f800001);
    Expected[1] = UINT32_C(0x7fc00001);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x5f00); // Round up; invalid is unmasked.
    Emulator.setRegister(x86reg::K3, UINT64_MAX);
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Right));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x5f00U);
    EXPECT_FALSE(Emulator.skips().any());

    expectMutatedLiftFailsClosed(Encoding, [](cs_insn &, cs_x86 &X86) {
      X86.avx_rm = X86_AVX_RM_RN;
      return true;
    });
    expectMutatedLiftFailsClosed(Encoding, [](cs_insn &, cs_x86 &X86) {
      X86.avx_sae = false;
      return true;
    });
  }

  // vaddps zmm1 {k3}, zmm2, zmm3 uses MXCSR round-up.  A signaling NaN in
  // an inactive lane must not be evaluated, while the active inexact lane
  // sets only the precision status bit.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6c, 0x4b, 0x58, 0xcb};
    std::vector<uint32_t> Left(16, UINT32_C(0x3f800000));
    std::vector<uint32_t> Right(16, UINT32_C(0x7f800001));
    Right[0] = UINT32_C(0x33800000);
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0xa0000000) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = UINT32_C(0x3f800001);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x5f00); // Round up; invalid is unmasked.
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Right));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR() & 1U, 0U);
    EXPECT_NE(Emulator.getMXCSR() & 0x20U, 0U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vfmadd213ps zmm8 {k1}, zmm9, zmm10, {rz-sae}.  The fused result is the
  // same halfway value as above; round-toward-zero must win over MXCSR.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0x52, 0x35, 0x79, 0xa8, 0xc2};
    std::vector<uint32_t> Destination(16, UINT32_C(0x3f800000));
    std::vector<uint32_t> Multiplier(16, UINT32_C(0x3f800000));
    std::vector<uint32_t> Addend(16, UINT32_C(0x33800000));
    std::vector<uint32_t> Expected(16, UINT32_C(0x3f800000));

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x5f00); // Round up; invalid is unmasked.
    Emulator.setRegister(x86reg::K1, UINT64_MAX);
    Emulator.setRegisterBytes(x86reg::XMM8, bytes(Destination));
    Emulator.setRegisterBytes(x86reg::XMM9, bytes(Multiplier));
    Emulator.setRegisterBytes(x86reg::XMM10, bytes(Addend));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM8), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x5f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vaddss xmm1 {k3}, xmm2, xmm3, {rn-sae} computes only the low element,
  // copies the remaining XMM elements from the first source, and clears the
  // architectural upper vector state.  Its embedded round-to-nearest must
  // override MXCSR round-up.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6e, 0x1b, 0x58, 0xcb};
    std::vector<uint32_t> Left = {UINT32_C(0x3f800000), UINT32_C(0x11223344),
                                  UINT32_C(0x55667788), UINT32_C(0x99aabbcc)};
    std::vector<uint32_t> Right(4, 0);
    Right[0] = UINT32_C(0x33800000);
    std::vector<uint32_t> ExpectedLow = Left;
    ExpectedLow[0] = UINT32_C(0x3f800000);
    std::vector<uint8_t> Expected = bytes(ExpectedLow);
    Expected.resize(64, 0);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x5f00);
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Right));
    Emulator.setRegisterBytes(x86reg::XMM1, std::vector<uint8_t>(64, 0xa5));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), Expected);
    EXPECT_EQ(Emulator.getMXCSR(), 0x5f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // An active signaling NaN with invalid unmasked faults before committing
  // the destination.  The same NaN was deliberately inactive in the earlier
  // case, proving that the compact mask guards arithmetic as well as memory.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6c, 0x4b, 0x58, 0xcb};
    std::vector<uint32_t> Left(16, UINT32_C(0x3f800000));
    std::vector<uint32_t> Right(16, UINT32_C(0x7f800001));
    std::vector<uint32_t> Old(16, UINT32_C(0x76543210));
    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00); // Invalid is unmasked.
    Emulator.setRegister(x86reg::K3, 2);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Right));
    EXPECT_LT(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Old));
    EXPECT_NE(Emulator.getMXCSR() & 1U, 0U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vmulps zmm1 {k3}, zmm2, zmm3 produces an exact tiny result.  With FTZ
  // and masked underflow, x86 flushes it to signed zero and records both
  // underflow and precision.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6c, 0x4b, 0x59, 0xcb};
    std::vector<uint32_t> Left(16, 0), Right(16, 0), Old(16), Expected(16);
    Left[0] = UINT32_C(0x00800000);
    Right[0] = UINT32_C(0x3f000000);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x12340000) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = 0;

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x9f80); // Default masks plus FTZ.
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Right));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_NE(Emulator.getMXCSR() & (1U << 4), 0U);
    EXPECT_NE(Emulator.getMXCSR() & (1U << 5), 0U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vsqrtps zmm1 {k3}, zmm2, {ru-sae}.  This covers directed rounding, exact
  // values, signed zero, infinity, NaN quieting, negative inputs, and DAZ in
  // active lanes.  SAE must leave MXCSR untouched for every case.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x7c, 0x5b, 0x51, 0xca};
    std::vector<uint32_t> Source(16, UINT32_C(0x40000000));
    Source[1] = UINT32_C(0x40800000);
    Source[2] = 0;
    Source[3] = UINT32_C(0x80000000);
    Source[4] = UINT32_C(0x7f800000);
    Source[5] = UINT32_C(0x7f800001);
    Source[6] = UINT32_C(0xbf800000);
    Source[7] = 1;
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x24680000) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = UINT32_C(0x3fb504f4);
    Expected[1] = UINT32_C(0x40000000);
    Expected[2] = 0;
    Expected[3] = UINT32_C(0x80000000);
    Expected[4] = UINT32_C(0x7f800000);
    Expected[5] = UINT32_C(0x7fc00001);
    Expected[6] = UINT32_C(0xffc00000);
    Expected[7] = 0;

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x3f40); // Round down, DAZ; invalid is unmasked.
    Emulator.setRegister(x86reg::K3, 0xff);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x3f40U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vsqrtpd zmm4 {k5}{z}, zmm6, {rz-sae}.  sqrt(2) rounds below the normal
  // nearest double, while zero-masking clears all inactive lanes.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0xfd, 0xfd, 0x51, 0xe6};
    std::vector<uint64_t> Source(8, UINT64_C(0x4000000000000000));
    std::vector<uint64_t> Old(8, UINT64_C(0xa5a5a5a5a5a5a5a5));
    std::vector<uint64_t> Expected(8, 0);
    Expected[0] = UINT64_C(0x3ff6a09e667f3bcc);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x5f00); // Round up; overridden by embedded rounding.
    Emulator.setRegister(x86reg::K5, 1);
    Emulator.setRegisterBytes(x86reg::XMM4, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM6, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM4), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x5f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vsqrtss xmm1 {k3}{z}, xmm2, xmm3, {rd-sae}.  Scalar EVEX copies bits
  // 127:32 from src1, computes only the low lane, and clears vector state
  // above XMM.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6e, 0xbb, 0x51, 0xcb};
    const std::vector<uint32_t> Merge = {
        UINT32_C(0xdeadbeef), UINT32_C(0x11223344), UINT32_C(0x55667788),
        UINT32_C(0x99aabbcc)};
    std::vector<uint32_t> Source(4, 0);
    Source[0] = UINT32_C(0x40000000);
    std::vector<uint32_t> ExpectedLow = Merge;
    ExpectedLow[0] = UINT32_C(0x3fb504f3);
    std::vector<uint8_t> Expected = bytes(ExpectedLow);
    Expected.resize(64, 0);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x5f00);
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, std::vector<uint8_t>(64, 0xa5));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Merge));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), Expected);
    EXPECT_EQ(Emulator.getMXCSR(), 0x5f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // A broadcast memory source is read once when any destination lane is
  // active, and not touched at all when every lane is masked off.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x7c, 0x5b, 0x51, 0x08};
    constexpr uint64_t Address = UINT64_C(0x2a000);
    constexpr uint64_t Mask = UINT64_C(0xa55a);
    const uint32_t Memory = UINT32_C(0x40800000);
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x13570000) + Lane;
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? UINT32_C(0x40000000)
                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, &Memory, sizeof(Memory));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());

    BinaryImage EmptyImage = emptyImage();
    NdOpEmulator MaskedOff(EmptyImage);
    MaskedOff.setStrictMode(true);
    MaskedOff.setLoadCollect(true);
    MaskedOff.setRegister(x86reg::RAX, Address);
    MaskedOff.setRegister(x86reg::K3, 0);
    MaskedOff.setRegisterBytes(x86reg::XMM1, bytes(Old));
    ASSERT_EQ(MaskedOff.run(Ops), Ops.size());
    EXPECT_EQ(MaskedOff.getRegisterBytes(x86reg::XMM1), bytes(Old));
    EXPECT_TRUE(MaskedOff.getLoadRecords().empty());
    EXPECT_FALSE(MaskedOff.skips().any());
  }

  // Without SAE, an active negative operand raises invalid and prevents the
  // architectural destination write.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x7c, 0x4b, 0x51, 0xca};
    std::vector<uint32_t> Source(16, UINT32_C(0xbf800000));
    std::vector<uint32_t> Old(16, UINT32_C(0x76543210));
    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00); // Invalid is unmasked.
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    EXPECT_LT(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Old));
    EXPECT_NE(Emulator.getMXCSR() & 1U, 0U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vminps zmm1 {k3}, zmm2, zmm3, {sae}.  x86 MIN selects the second
  // operand for equal zeros and whenever either operand is NaN.  SAE also
  // suppresses the signaling-NaN invalid exception.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6c, 0x1b, 0x5d, 0xcb};
    std::vector<uint32_t> Left(16, 0), Right(16, 0), Old(16), Expected(16);
    Left[0] = 0;
    Right[0] = UINT32_C(0x80000000);
    Left[1] = UINT32_C(0x7fc01234);
    Right[1] = UINT32_C(0x40400000);
    Left[2] = UINT32_C(0x40800000);
    Right[2] = UINT32_C(0x7fc05678);
    Left[3] = UINT32_C(0x7f801234);
    Right[3] = UINT32_C(0x40a00000);
    Left[4] = UINT32_C(0x40000000);
    Right[4] = UINT32_C(0x3f800000);
    Left[5] = UINT32_C(0xc0400000);
    Right[5] = UINT32_C(0xc0000000);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x35790000) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = UINT32_C(0x80000000);
    Expected[1] = UINT32_C(0x40400000);
    Expected[2] = UINT32_C(0x7fc05678);
    Expected[3] = UINT32_C(0x40a00000);
    Expected[4] = UINT32_C(0x3f800000);
    Expected[5] = UINT32_C(0xc0400000);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00); // Invalid is unmasked, but SAE suppresses it.
    Emulator.setRegister(x86reg::K3, 0x3f);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Right));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vmaxsd xmm4 {k5}{z}, xmm6, xmm7, {sae}.  Equal signed zeros select
  // src2, while scalar EVEX still copies the remaining XMM bits from src1.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0xcf, 0x9d, 0x5f, 0xe7};
    const std::vector<uint64_t> Merge = {UINT64_C(0x8000000000000000),
                                         UINT64_C(0x1122334455667788)};
    const std::vector<uint64_t> Source = {0, UINT64_C(0x99aabbccddeeff00)};
    std::vector<uint64_t> ExpectedLow = Merge;
    ExpectedLow[0] = 0;
    std::vector<uint8_t> Expected = bytes(ExpectedLow);
    Expected.resize(64, 0);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00);
    Emulator.setRegister(x86reg::K5, 1);
    Emulator.setRegisterBytes(x86reg::XMM4, std::vector<uint8_t>(64, 0xa5));
    Emulator.setRegisterBytes(x86reg::XMM6, bytes(Merge));
    Emulator.setRegisterBytes(x86reg::XMM7, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM4), Expected);
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // EVEX.b on a memory MIN source is broadcast, not SAE.  One scalar load
  // feeds all active lanes, and an all-zero writemask suppresses that load.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6c, 0x5b, 0x5d, 0x08};
    constexpr uint64_t Address = UINT64_C(0x2b000);
    constexpr uint64_t Mask = UINT64_C(0xa55a);
    const uint32_t Memory = UINT32_C(0x40a00000);
    std::vector<uint32_t> Left(16), Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Left.size(); ++Lane) {
      Left[Lane] = UINT32_C(0x41200000); // 10.0f
      Old[Lane] = UINT32_C(0x579b0000) + Lane;
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0 ? Memory : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, &Memory, sizeof(Memory));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());

    BinaryImage EmptyImage = emptyImage();
    NdOpEmulator MaskedOff(EmptyImage);
    MaskedOff.setStrictMode(true);
    MaskedOff.setLoadCollect(true);
    MaskedOff.setRegister(x86reg::RAX, Address);
    MaskedOff.setRegister(x86reg::K3, 0);
    MaskedOff.setRegisterBytes(x86reg::XMM1, bytes(Old));
    MaskedOff.setRegisterBytes(x86reg::XMM2, bytes(Left));
    ASSERT_EQ(MaskedOff.run(Ops), Ops.size());
    EXPECT_EQ(MaskedOff.getRegisterBytes(x86reg::XMM1), bytes(Old));
    EXPECT_TRUE(MaskedOff.getLoadRecords().empty());
    EXPECT_FALSE(MaskedOff.skips().any());
  }

  // The same signaling NaN must fault and preserve the destination when SAE
  // is absent and invalid is unmasked.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6c, 0x4b, 0x5d, 0xcb};
    std::vector<uint32_t> Left(16, UINT32_C(0x7f801234));
    std::vector<uint32_t> Right(16, UINT32_C(0x3f800000));
    std::vector<uint32_t> Old(16, UINT32_C(0x67890000));
    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00);
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Right));
    EXPECT_LT(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Old));
    EXPECT_NE(Emulator.getMXCSR() & 1U, 0U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // For scalar memory arithmetic, only writemask bit zero may trigger the
  // memory access.  Higher K bits are architecturally irrelevant.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6e, 0x0b, 0x58, 0x08};
    constexpr uint64_t Address = UINT64_C(0x2c000);
    const std::vector<uint32_t> Merge = {
        UINT32_C(0x3f800000), UINT32_C(0x11223344), UINT32_C(0x55667788),
        UINT32_C(0x99aabbcc)};
    const std::vector<uint32_t> Old = {
        UINT32_C(0xdeadbeef), UINT32_C(0xa5a5a5a5), UINT32_C(0xa5a5a5a5),
        UINT32_C(0xa5a5a5a5)};
    std::vector<uint32_t> ExpectedLow = Merge;
    ExpectedLow[0] = Old[0];
    std::vector<uint8_t> Expected = bytes(ExpectedLow);
    Expected.resize(64, 0);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, 2);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Merge));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), Expected);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vcvtdq2ps zmm1 {k3}, zmm2, {rd-sae}.  2^24+1 lies exactly halfway
  // between adjacent positive f32 values; embedded round-down must override
  // MXCSR round-up and SAE must suppress precision status.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x7c, 0x3b, 0x5b, 0xca};
    std::vector<uint32_t> Source(16, UINT32_C(0x01000001));
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x24681300) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = UINT32_C(0x4b800000);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x5f00); // Round up; overridden by embedded rounding.
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x5f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vcvtps2dq zmm1 {k3}, zmm2, {ru-sae}.  Directed conversion applies to
  // both signs, while invalid inputs produce the signed indefinite value and
  // SAE prevents any MXCSR update.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x7d, 0x5b, 0x5b, 0xca};
    std::vector<uint32_t> Source(16, 0), Old(16), Expected(16);
    Source[0] = UINT32_C(0x3fa00000); // 1.25f
    Source[1] = UINT32_C(0xbfa00000); // -1.25f
    Source[2] = UINT32_C(0x7f800001); // signaling NaN
    Source[3] = UINT32_C(0x7f800000); // +infinity
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x13572400) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = 2;
    Expected[1] = UINT32_MAX;
    Expected[2] = UINT32_C(0x80000000);
    Expected[3] = UINT32_C(0x80000000);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00);
    Emulator.setRegister(x86reg::K3, 0x0f);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vcvttps2udq zmm1 {k3}, zmm2, {sae}.  Truncation is fixed toward zero;
  // unsigned invalid results use all-one bits.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x7c, 0x1b, 0x78, 0xca};
    std::vector<uint32_t> Source(16, 0), Old(16), Expected(16);
    Source[0] = UINT32_C(0x40700000); // 3.75f
    Source[1] = UINT32_C(0xbf000000); // -0.5f
    Source[2] = UINT32_C(0x7f800000);
    Source[3] = UINT32_C(0x7f800001);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x11220000) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = 3;
    Expected[1] = 0;
    Expected[2] = UINT32_MAX;
    Expected[3] = UINT32_MAX;

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00);
    Emulator.setRegister(x86reg::K3, 0x0f);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Without SAE, an active invalid conversion faults before the destination
  // is committed.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x7e, 0x4b, 0x5b, 0xca};
    std::vector<uint32_t> Source(16, UINT32_C(0x7f800001));
    std::vector<uint32_t> Old(16, UINT32_C(0x789a0000));
    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00);
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    EXPECT_LT(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Old));
    EXPECT_NE(Emulator.getMXCSR() & 1U, 0U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // A broadcast integer source obeys MXCSR rounding and is read once.  With
  // no active lanes it must not touch unmapped memory.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x7c, 0x5b, 0x5b, 0x08};
    constexpr uint64_t Address = UINT64_C(0x2d000);
    constexpr uint64_t Mask = UINT64_C(0xa55a);
    const uint32_t Memory = UINT32_C(0x01000001);
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x42420000) + Lane;
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? UINT32_C(0x4b800001)
                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, &Memory, sizeof(Memory));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setMXCSR(0x5f80); // Masked exceptions, round toward +infinity.
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_NE(Emulator.getMXCSR() & (1U << 5), 0U);
    EXPECT_FALSE(Emulator.skips().any());

    BinaryImage EmptyImage = emptyImage();
    NdOpEmulator MaskedOff(EmptyImage);
    MaskedOff.setStrictMode(true);
    MaskedOff.setLoadCollect(true);
    MaskedOff.setRegister(x86reg::RAX, Address);
    MaskedOff.setRegister(x86reg::K3, 0);
    MaskedOff.setRegisterBytes(x86reg::XMM1, bytes(Old));
    ASSERT_EQ(MaskedOff.run(Ops), Ops.size());
    EXPECT_EQ(MaskedOff.getRegisterBytes(x86reg::XMM1), bytes(Old));
    EXPECT_TRUE(MaskedOff.getLoadRecords().empty());
    EXPECT_FALSE(MaskedOff.skips().any());
  }

  // Widening EVEX conversions use a memory tuple narrower than the zmm
  // result.  Both the full m256 tuple and m32 broadcast must retain exact
  // lane-granular fault suppression.
  {
    struct MemoryCase {
      std::vector<uint8_t> Encoding;
      bool Broadcast;
    };
    const std::vector<MemoryCase> Cases = {
        {{0x62, 0xf1, 0x7e, 0x4b, 0xe6, 0x08}, false},
        {{0x62, 0xf1, 0x7e, 0x5b, 0xe6, 0x08}, true},
    };
    constexpr uint64_t Address = UINT64_C(0x2e000);
    constexpr uint64_t Mask = UINT64_C(0x55);
    const std::vector<int32_t> FullMemory = {1, -2, 3, -4, 5, -6, 7, -8};
    const int32_t BroadcastMemory = 9;
    std::vector<double> Old(8);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane)
      Old[Lane] = -100.0 - Lane;

    for (const MemoryCase &Test : Cases) {
      SCOPED_TRACE(Test.Broadcast ? "broadcast" : "full");
      std::vector<double> Expected = Old;
      for (unsigned Lane = 0; Lane < Expected.size(); ++Lane)
        if ((Mask & (UINT64_C(1) << Lane)) != 0)
          Expected[Lane] = static_cast<double>(
              Test.Broadcast ? BroadcastMemory : FullMemory[Lane]);

      const std::vector<LowOp> Ops = liftX64(Test.Encoding);
      ASSERT_FALSE(Ops.empty());
      BinaryImage Image = emptyImage();
      if (Test.Broadcast)
        addReadableBytes(Image, Address, &BroadcastMemory,
                         sizeof(BroadcastMemory));
      else
        addReadableBytes(Image, Address, FullMemory.data(),
                         FullMemory.size() * sizeof(int32_t));
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setLoadCollect(true);
      Emulator.setRegister(x86reg::RAX, Address);
      Emulator.setRegister(x86reg::K3, Mask);
      Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
      EXPECT_EQ(Emulator.getLoadRecords().size(),
                Test.Broadcast ? 1U : bitCount(Mask));
      EXPECT_FALSE(Emulator.skips().any());
    }
  }

  // vcvtpd2ps ymm1 {k3}, zmm2, {rd-sae}.  The source is exactly halfway
  // between 1.0f and its successor, so embedded round-down must produce 1.0f
  // and SAE must leave MXCSR unchanged.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0xfd, 0x3b, 0x5a, 0xca};
    std::vector<uint64_t> Source(8, UINT64_C(0x3ff0000010000000));
    std::vector<uint32_t> Old(8), ExpectedLow(8);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x24680000) + Lane;
      ExpectedLow[Lane] = Old[Lane];
    }
    ExpectedLow[0] = UINT32_C(0x3f800000);
    std::vector<uint8_t> Expected = bytes(ExpectedLow);
    Expected.resize(64, 0);
    std::vector<uint8_t> Initial = bytes(Old);
    Initial.resize(64, 0xa5);

    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Encoding.data(), Encoding.size(),
                                   kInstructionAddress, Insn),
              static_cast<int>(Encoding.size()));
    ASSERT_NE(Insn.Raw, nullptr);
    ASSERT_NE(Insn.Raw->detail, nullptr);
    const cs_x86 &X86 = Insn.Raw->detail->x86;
    ASSERT_EQ(Insn.Raw->id, X86_INS_VCVTPD2PS);
    ASSERT_EQ(X86.op_count, 3U);
    EXPECT_EQ(X86.operands[0].size, 32U);
    EXPECT_EQ(X86.operands[1].size, 1U);
    EXPECT_EQ(X86.operands[2].size, 64U);
    EXPECT_TRUE(X86.avx_sae);
    EXPECT_EQ(X86.avx_rm, X86_AVX_RM_RD);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x5f00);
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, Initial);
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), Expected);
    EXPECT_EQ(Emulator.getMXCSR(), 0x5f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vcvtsd2ss xmm1 {k3}, xmm2, xmm3, {ru-sae}.  The low double is
  // halfway between adjacent single-precision values, while bits 127:32
  // come from the merge source and all higher vector state is cleared.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0xef, 0x5b, 0x5a, 0xcb};
    const std::vector<uint32_t> Merge = {
        UINT32_C(0xdeadbeef), UINT32_C(0x11223344), UINT32_C(0x55667788),
        UINT32_C(0x99aabbcc)};
    const std::vector<uint64_t> Source = {UINT64_C(0x3ff0000010000000),
                                          UINT64_C(0xa5a5a5a5a5a5a5a5)};
    std::vector<uint32_t> ExpectedLow = Merge;
    ExpectedLow[0] = UINT32_C(0x3f800001);
    std::vector<uint8_t> Expected = bytes(ExpectedLow);
    Expected.resize(64, 0);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x3f00); // Round down; overridden by embedded rounding.
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, std::vector<uint8_t>(64, 0xa5));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Merge));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), Expected);
    EXPECT_EQ(Emulator.getMXCSR(), 0x3f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vcvts[s]2sd with SAE quiets an active signaling NaN without exposing
  // the invalid exception, preserving the upper qword from the merge source.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6e, 0x1b, 0x5a, 0xcb};
    const std::vector<uint64_t> Merge = {UINT64_C(0xdeadbeefdeadbeef),
                                         UINT64_C(0x1122334455667788)};
    const std::vector<uint32_t> Source = {
        UINT32_C(0x7f800001), UINT32_C(0xa5a5a5a5), UINT32_C(0xa5a5a5a5),
        UINT32_C(0xa5a5a5a5)};
    const std::vector<uint64_t> ExpectedLow = {UINT64_C(0x7ff8000020000000),
                                               Merge[1]};
    std::vector<uint8_t> Expected = bytes(ExpectedLow);
    Expected.resize(64, 0);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00); // Invalid is unmasked, but SAE suppresses it.
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, std::vector<uint8_t>(64, 0xa5));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Merge));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), Expected);
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Only k3[0] controls a scalar memory source.  Higher mask bits must not
  // trigger a load, while merge masking keeps the old low element and still
  // copies the upper XMM bytes from src1.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0xef, 0x0b, 0x5a, 0x08};
    constexpr uint64_t Address = UINT64_C(0x2b000);
    const std::vector<uint32_t> Old = {
        UINT32_C(0x13572468), UINT32_C(0xa5a5a5a5), UINT32_C(0xa5a5a5a5),
        UINT32_C(0xa5a5a5a5)};
    const std::vector<uint32_t> Merge = {
        UINT32_C(0xdeadbeef), UINT32_C(0x11223344), UINT32_C(0x55667788),
        UINT32_C(0x99aabbcc)};
    std::vector<uint32_t> ExpectedLow = Merge;
    ExpectedLow[0] = Old[0];
    std::vector<uint8_t> Initial = bytes(Old);
    Initial.resize(64, 0xa5);
    std::vector<uint8_t> Expected = bytes(ExpectedLow);
    Expected.resize(64, 0);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, 2);
    Emulator.setRegisterBytes(x86reg::XMM1, Initial);
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Merge));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), Expected);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vcvtsi2ss xmm1, xmm2, {rd-sae}, rax converts 2^24+1 downward to
  // exactly 2^24.  Embedded rounding overrides MXCSR, SAE hides precision,
  // and the rest of XMM comes from src1.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0xee, 0x38, 0x2a, 0xc8};
    const std::vector<uint32_t> Merge = {
        UINT32_C(0xdeadbeef), UINT32_C(0x11223344), UINT32_C(0x55667788),
        UINT32_C(0x99aabbcc)};
    std::vector<uint32_t> ExpectedLow = Merge;
    ExpectedLow[0] = UINT32_C(0x4b800000);
    std::vector<uint8_t> Expected = bytes(ExpectedLow);
    Expected.resize(64, 0);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x5f00); // Round up; overridden by embedded rounding.
    Emulator.setRegister(x86reg::RAX, UINT64_C(0x01000001));
    Emulator.setRegisterBytes(x86reg::XMM1, std::vector<uint8_t>(64, 0xa5));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Merge));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), Expected);
    EXPECT_EQ(Emulator.getMXCSR(), 0x5f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vcvtusi2sd xmm1, xmm2, {ru-sae}, rax must interpret the high-bit value
  // as unsigned and round it to the next representable double.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0xef, 0x58, 0x7b, 0xc8};
    const std::vector<uint64_t> Merge = {UINT64_C(0xdeadbeefdeadbeef),
                                         UINT64_C(0x1122334455667788)};
    const std::vector<uint64_t> ExpectedLow = {UINT64_C(0x43e0000000000001),
                                               Merge[1]};
    std::vector<uint8_t> Expected = bytes(ExpectedLow);
    Expected.resize(64, 0);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x3f00); // Round down; overridden by embedded rounding.
    Emulator.setRegister(x86reg::RAX, UINT64_C(0x8000000000000001));
    Emulator.setRegisterBytes(x86reg::XMM1, std::vector<uint8_t>(64, 0xa5));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Merge));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), Expected);
    EXPECT_EQ(Emulator.getMXCSR(), 0x3f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vcvtsd2si rax, xmm2, {rd-sae} rounds 1.75 down to one and leaves MXCSR
  // untouched even though its configured mode is round-up.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0xff, 0x38, 0x2d, 0xc2};
    const std::vector<uint64_t> Source = {UINT64_C(0x3ffc000000000000),
                                          UINT64_C(0xa5a5a5a5a5a5a5a5)};

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x5f00);
    Emulator.setRegister(x86reg::RAX, UINT64_C(0xdeadbeefdeadbeef));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::RAX), 1U);
    EXPECT_EQ(Emulator.getMXCSR(), 0x5f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vcvttsd2si rax, xmm2, {sae} always truncates toward zero.  A negative
  // fractional input therefore becomes -1 independently of MXCSR.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0xff, 0x18, 0x2c, 0xc2};
    const std::vector<uint64_t> Source = {UINT64_C(0xbffc000000000000),
                                          UINT64_C(0xa5a5a5a5a5a5a5a5)};

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x5f00);
    Emulator.setRegister(x86reg::RAX, UINT64_C(0xdeadbeefdeadbeef));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::RAX), UINT64_MAX);
    EXPECT_EQ(Emulator.getMXCSR(), 0x5f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vcvtss2usi eax, xmm2, {ru-sae} exercises unsigned conversion and the
  // architectural zero-extension of a 32-bit GPR destination.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x7e, 0x58, 0x79, 0xc2};
    const std::vector<uint32_t> Source = {
        UINT32_C(0x3fa00000), UINT32_C(0xa5a5a5a5), UINT32_C(0xa5a5a5a5),
        UINT32_C(0xa5a5a5a5)};

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x3f00);
    Emulator.setRegister(x86reg::RAX, UINT64_MAX);
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::RAX), 2U);
    EXPECT_EQ(Emulator.getMXCSR(), 0x3f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vrndscaleps zmm1 {k3}, zmm2, {sae}, 1 rounds active lanes down to
  // integral values.  LL is ignored in this SAE-only encoding, and inactive
  // lanes neither evaluate nor alter the merging destination.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0x7d, 0x1b,
                                           0x08, 0xca, 0x01};
    std::vector<uint32_t> Source(16, UINT32_C(0x7f800001));
    Source[0] = UINT32_C(0x3fe00000);
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x24680000) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = UINT32_C(0x3f800000);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x5f00); // Invalid unmasked; SAE suppresses all status.
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x5f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vreducess xmm1 {k3}, xmm2, xmm3, {sae}, 2 computes
  // 1.75-roundUp(1.75) = -0.25 in the low lane and copies the remaining XMM
  // bytes from src1.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0x6d, 0x1b,
                                           0x57, 0xcb, 0x02};
    const std::vector<uint32_t> Merge = {
        UINT32_C(0xdeadbeef), UINT32_C(0x11223344), UINT32_C(0x55667788),
        UINT32_C(0x99aabbcc)};
    const std::vector<uint32_t> Source = {
        UINT32_C(0x3fe00000), UINT32_C(0xa5a5a5a5), UINT32_C(0xa5a5a5a5),
        UINT32_C(0xa5a5a5a5)};
    std::vector<uint32_t> ExpectedLow = Merge;
    ExpectedLow[0] = UINT32_C(0xbe800000);
    std::vector<uint8_t> Expected = bytes(ExpectedLow);
    Expected.resize(64, 0);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x3f00);
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, std::vector<uint8_t>(64, 0xa5));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Merge));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), Expected);
    EXPECT_EQ(Emulator.getMXCSR(), 0x3f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Without SAE, RNDSCALE reports precision when an active value changes.
  // With the precision mask cleared the instruction faults before any
  // destination lane is committed, while the sticky status bit is retained.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0x7d, 0x4b,
                                           0x08, 0xca, 0x01};
    std::vector<uint32_t> Source(16, UINT32_C(0x3fe00000));
    std::vector<uint32_t> Old(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane)
      Old[Lane] = UINT32_C(0x13570000) + Lane;

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x0f80); // Precision is unmasked.
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    EXPECT_LT(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Old));
    EXPECT_NE(Emulator.getMXCSR() & 0x20U, 0U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // imm8.SPE suppresses the same precision condition independently of SAE.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0x7d, 0x4b,
                                           0x08, 0xca, 0x09};
    std::vector<uint32_t> Source(16, UINT32_C(0x7f800001));
    Source[0] = UINT32_C(0x3fe00000);
    std::vector<uint32_t> Expected(16, 0);
    Expected[0] = UINT32_C(0x3f800000);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x0f80); // Precision remains unmasked.
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR() & 0x20U, 0U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // A zero writemask suppresses every lane load of the full-width memory
  // form.  The address is deliberately unmapped; zeroing mask semantics must
  // still complete without touching it.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0x7d, 0xcb,
                                           0x08, 0x08, 0x08};
    const std::vector<uint32_t> Expected(16, 0);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, UINT64_C(0xdead0000));
    Emulator.setRegister(x86reg::K3, 0);
    Emulator.setRegisterBytes(x86reg::XMM1, std::vector<uint8_t>(64, 0xa5));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());
  }

  // VREDUCE applies MXCSR.DAZ before its transformation.  The smallest
  // positive denormal therefore becomes +0, while inactive signaling NaNs do
  // not raise invalid or contribute precision status.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0x7d, 0x4b,
                                           0x56, 0xca, 0x08};
    std::vector<uint32_t> Source(16, UINT32_C(0x7f800001));
    Source[0] = 1;
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x579b0000) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = 0;

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1fc0); // Default masks plus DAZ.
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR() & 0x21U, 0U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Intel defines VREDUCE of either infinity as +0.0.  The invalid status is
  // still sticky when the exception is masked.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0x7d, 0x4b,
                                           0x56, 0xca, 0x08};
    std::vector<uint32_t> Source(16, UINT32_C(0x7f800001));
    Source[0] = UINT32_C(0x7f800000);
    Source[1] = UINT32_C(0xff800000);
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x579b0000) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = 0;
    Expected[1] = 0;

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f80); // Default masks; infinity reduction is invalid.
    Emulator.setRegister(x86reg::K3, 3);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f81U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // A scalar memory form consults only k[0].  Higher set mask bits must not
  // access memory and must preserve the old low destination while copying the
  // upper XMM bytes from src1.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0x6d, 0x0b,
                                           0x57, 0x08, 0x00};
    const std::vector<uint32_t> Old = {
        UINT32_C(0xdeadbeef), UINT32_C(0xa5a5a5a5), UINT32_C(0xa5a5a5a5),
        UINT32_C(0xa5a5a5a5)};
    const std::vector<uint32_t> Merge = {
        UINT32_C(0x01020304), UINT32_C(0x11223344), UINT32_C(0x55667788),
        UINT32_C(0x99aabbcc)};
    std::vector<uint32_t> ExpectedLow = Merge;
    ExpectedLow[0] = Old[0];
    std::vector<uint8_t> Expected = bytes(ExpectedLow);
    Expected.resize(64, 0);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, UINT64_C(0xdead0000));
    Emulator.setRegister(x86reg::K3, 2);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Merge));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), Expected);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vgetmantps with interval 01 and sign-control 01 clears the input sign
  // and maps an even-exponent -6.0 significand to +1.5.  Inactive signaling
  // NaNs must remain unevaluated.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0x7d, 0x4b,
                                           0x26, 0xca, 0x05};
    std::vector<uint32_t> Source(16, UINT32_C(0x7f800001));
    Source[0] = UINT32_C(0xc0c00000);
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x6a5b0000) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = UINT32_C(0x3fc00000);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00); // Invalid is unmasked.
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR() & 1U, 0U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vgetmantsd sign-control 10 rejects a negative input.  SAE converts the
  // result to the architectural indefinite QNaN without updating MXCSR, and
  // scalar upper bytes still come from src1.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0xed, 0x1b,
                                           0x27, 0xcb, 0x08};
    const std::vector<uint64_t> Merge = {UINT64_C(0x0102030405060708),
                                         UINT64_C(0x1122334455667788)};
    const std::vector<uint64_t> Source = {UINT64_C(0xfff0000000000000),
                                          UINT64_C(0xa5a5a5a5a5a5a5a5)};
    std::vector<uint64_t> ExpectedLow = {
        UINT64_C(0xfff8000000000000), Merge[1]};
    std::vector<uint8_t> Expected = bytes(ExpectedLow);
    Expected.resize(64, 0);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00);
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Merge));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), Expected);
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vgetexpps normalizes a denormal before extracting floor(log2(abs(x))).
  // The minimum positive Float32 denormal therefore produces -149.0; SAE
  // suppresses the otherwise raised denormal status.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0x7d,
                                           0x1b, 0x42, 0xca};
    std::vector<uint32_t> Source(16, UINT32_C(0x7f800001));
    Source[0] = 1;
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x75310000) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = UINT32_C(0xc3150000);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1e80); // Denormal is unmasked; SAE must suppress it.
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x1e80U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Scalar VGETEXP quiets an active signaling NaN while SAE suppresses its
  // invalid status and the upper XMM bytes are copied from src1.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0xed,
                                           0x1b, 0x43, 0xcb};
    const std::vector<uint64_t> Merge = {UINT64_C(0x0102030405060708),
                                         UINT64_C(0x1122334455667788)};
    const std::vector<uint64_t> Source = {UINT64_C(0x7ff0000000000001),
                                          UINT64_C(0xa5a5a5a5a5a5a5a5)};
    std::vector<uint64_t> ExpectedLow = {
        UINT64_C(0x7ff8000000000001), Merge[1]};
    std::vector<uint8_t> Expected = bytes(ExpectedLow);
    Expected.resize(64, 0);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00);
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Merge));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), Expected);
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vrangeps with MAX and comparison-result sign selection covers the
  // architecture's special opposite-zero, qNaN, and sNaN rules.  SAE keeps
  // the active sNaN from updating MXCSR; inactive lanes preserve the old
  // destination without inspecting their signaling NaNs.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0x6d, 0x1b,
                                           0x50, 0xcb, 0x05};
    std::vector<uint32_t> Left(16, UINT32_C(0x7f800001));
    std::vector<uint32_t> Right(16, UINT32_C(0x7f800001));
    Left[0] = 0;
    Right[0] = UINT32_C(0x80000000);
    Left[1] = UINT32_C(0x40a00000);
    Right[1] = UINT32_C(0xc0a00000);
    Left[2] = UINT32_C(0x7fc01234);
    Right[2] = UINT32_C(0x40400000);
    Left[3] = UINT32_C(0xc0800000);
    Right[3] = UINT32_C(0xffc05678);
    Left[4] = UINT32_C(0x7fc01234);
    Right[4] = UINT32_C(0xffc05678);
    Left[5] = UINT32_C(0x7f801111);
    Right[5] = UINT32_C(0x3f800000);
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x42a50000) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = 0;
    Expected[1] = UINT32_C(0x40a00000);
    Expected[2] = UINT32_C(0x40400000);
    Expected[3] = UINT32_C(0xc0800000);
    Expected[4] = UINT32_C(0x7fc01234);
    Expected[5] = UINT32_C(0x7fc01111);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00);
    Emulator.setRegister(x86reg::K3, 0x3f);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Right));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // VFIXUPIMM uses the classification source to select a nibble from the
  // per-lane table.  Its requested invalid/zero flags are always masked: even
  // with every MXCSR exception mask clear, the destination is committed and
  // the sticky status bits are reported.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0x6d, 0x4b,
                                           0x54, 0xcb, 0xff};
    std::vector<uint32_t> Source(16, UINT32_C(0x7f800001));
    Source[0] = UINT32_C(0xffc01234);
    Source[1] = UINT32_C(0x7f801111);
    Source[2] = 0;
    Source[3] = UINT32_C(0x3f800000);
    Source[4] = UINT32_C(0xff800000);
    Source[5] = UINT32_C(0x7f800000);
    Source[6] = UINT32_C(0xc0000000);
    Source[7] = UINT32_C(0x40000000);
    Source[8] = UINT32_C(0x40400000);
    std::vector<uint32_t> Table(16, 0);
    Table[0] = UINT32_C(0x00000002);
    Table[1] = UINT32_C(0x00000020);
    Table[2] = UINT32_C(0x00000400);
    Table[3] = UINT32_C(0x0000c000);
    Table[4] = UINT32_C(0x00060000);
    Table[5] = UINT32_C(0x00d00000);
    Table[6] = UINT32_C(0x0e000000);
    Table[7] = UINT32_C(0xf0000000);
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x42a50000) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = UINT32_C(0xffc01234);
    Expected[1] = UINT32_C(0x7fc01111);
    Expected[2] = UINT32_C(0xff800000);
    Expected[3] = UINT32_C(0x42b40000);
    Expected[4] = UINT32_C(0xff800000);
    Expected[5] = UINT32_C(0x3fc90fdb);
    Expected[6] = UINT32_C(0x7f7fffff);
    Expected[7] = UINT32_C(0xff7fffff);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0);
    Emulator.setRegister(x86reg::K3, 0x1ff);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Table));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 5U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // DAZ converts a negative denormal classification source to negative zero;
  // the source-response table entry makes the preserved sign observable. SAE
  // suppresses both exception status bits requested by the immediate.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0x6d, 0x1b,
                                           0x54, 0xcb, 0x03};
    std::vector<uint32_t> Source(16, UINT32_C(0x7f800001));
    std::vector<uint32_t> Table(16, 0);
    std::vector<uint32_t> Old(16), Expected(16);
    Source[0] = UINT32_C(0x80000001);
    Table[0] = UINT32_C(0x00000100);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x42a50000) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = UINT32_C(0x80000000);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(1U << 6);
    Emulator.setRegister(x86reg::K3, 1);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Table));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 1U << 6);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // VSCALEF floors its second source before applying an exact power-of-two
  // scale.  Special values, exact and inexact subnormal results, overflow,
  // source-one denormals, and inactive signaling NaNs all have distinct
  // architectural behavior.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0x6d,
                                           0x4b, 0x2c, 0xcb};
    std::vector<uint32_t> Left(16, UINT32_C(0x7f800001));
    std::vector<uint32_t> Right(16, UINT32_C(0x7f800001));
    Left[0] = UINT32_C(0x3fc00000);
    Right[0] = UINT32_C(0x40200000);
    Left[1] = UINT32_C(0xbfc00000);
    Right[1] = UINT32_C(0xbfc00000);
    Left[2] = 0;
    Right[2] = UINT32_C(0x7f800000);
    Left[3] = UINT32_C(0xff800000);
    Right[3] = UINT32_C(0xff800000);
    Left[4] = UINT32_C(0xffc01234);
    Right[4] = UINT32_C(0x7f800000);
    Left[5] = UINT32_C(0x7fc05678);
    Right[5] = UINT32_C(0xff800000);
    Left[6] = UINT32_C(0x3f800000);
    Right[6] = UINT32_C(0xff801234);
    Left[7] = UINT32_C(0x7f801111);
    Right[7] = 0;
    Left[8] = UINT32_C(0x00800000);
    Right[8] = UINT32_C(0xbf800000);
    Left[9] = 1;
    Right[9] = UINT32_C(0xbf800000);
    Left[10] = UINT32_C(0x7f7fffff);
    Right[10] = UINT32_C(0x3f800000);
    Left[11] = 1;
    Right[11] = 0;
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x42a50000) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = UINT32_C(0x40c00000);
    Expected[1] = UINT32_C(0xbec00000);
    Expected[2] = UINT32_C(0xffc00000);
    Expected[3] = UINT32_C(0xffc00000);
    Expected[4] = UINT32_C(0x7f800000);
    Expected[5] = 0;
    Expected[6] = UINT32_C(0xffc01234);
    Expected[7] = UINT32_C(0x7fc01111);
    Expected[8] = UINT32_C(0x00400000);
    Expected[9] = 0;
    Expected[10] = UINT32_C(0x7f800000);
    Expected[11] = 1;

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f80);
    Emulator.setRegister(x86reg::K3, 0xfff);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Right));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x1fbbU);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Embedded round-up plus SAE distinguishes the two signs of a half-way
  // minimum-denormal result while suppressing DE/UE/PE even with all MXCSR
  // masks clear.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0x6d,
                                           0x5b, 0x2c, 0xcb};
    std::vector<uint32_t> Left(16, UINT32_C(0x7f800001));
    std::vector<uint32_t> Right(16, UINT32_C(0x7f800001));
    Left[0] = 1;
    Left[1] = UINT32_C(0x80000001);
    Right[0] = UINT32_C(0xbf800000);
    Right[1] = UINT32_C(0xbf800000);
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x42a50000) + Lane;
      Expected[Lane] = Old[Lane];
    }
    Expected[0] = 1;
    Expected[1] = UINT32_C(0x80000000);

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0);
    Emulator.setRegister(x86reg::K3, 3);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Right));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // A zero writemask suppresses the packed broadcast memory access entirely.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0x6d,
                                           0x5b, 0x2c, 0x08};
    std::vector<uint32_t> Old(16), Left(16, UINT32_C(0x3f800000));
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane)
      Old[Lane] = UINT32_C(0x42a50000) + Lane;

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::K3, 0);
    Emulator.setRegister(x86reg::RAX, UINT64_C(0xdead0000));
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Old));
    EXPECT_FALSE(Emulator.skips().any());
  }

  // EVEX VANDN consumes the two vector sources after the explicit writemask;
  // the mask is not an ordinary data operand.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6c,
                                           0x4b, 0x55, 0xcb};
    constexpr uint64_t Mask = UINT64_C(0xa55a);
    std::vector<uint32_t> Left(16), Right(16), Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Left.size(); ++Lane) {
      Left[Lane] = UINT32_C(0x0f0f0000) + Lane;
      Right[Lane] = UINT32_C(0xff00ff00) ^ Lane;
      Old[Lane] = UINT32_C(0x42420000) + Lane;
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? ~Left[Lane] & Right[Lane]
                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Right));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_FALSE(Emulator.skips().any());
  }

  // A masked EVEX bitwise broadcast reads its scalar memory source once and
  // applies it independently to every active dword.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6c,
                                           0x5b, 0x56, 0x08};
    constexpr uint64_t Address = UINT64_C(0x31000);
    constexpr uint64_t Mask = UINT64_C(0xa55a);
    const uint32_t Memory = UINT32_C(0x00ff00ff);
    std::vector<uint32_t> Left(16), Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Left.size(); ++Lane) {
      Left[Lane] = UINT32_C(0x55000000) + Lane;
      Old[Lane] = UINT32_C(0x42420000) + Lane;
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? Left[Lane] | Memory
                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, &Memory, sizeof(Memory));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Integer AND-NOT has the same masked broadcast contract and keeps its
  // element-granular merge behavior.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6d,
                                           0x5b, 0xdf, 0x08};
    constexpr uint64_t Address = UINT64_C(0x32000);
    constexpr uint64_t Mask = UINT64_C(0x5aa5);
    const uint32_t Memory = UINT32_C(0x00ff00ff);
    std::vector<uint32_t> Left(16), Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Left.size(); ++Lane) {
      Left[Lane] = UINT32_C(0x55000000) + Lane;
      Old[Lane] = UINT32_C(0x42420000) + Lane;
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? ~Left[Lane] & Memory
                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, &Memory, sizeof(Memory));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Unary packed integer operations retain the same broadcast and merge
  // contract; every active lane receives the scalar popcount.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0x7d,
                                           0x5b, 0x55, 0x08};
    constexpr uint64_t Address = UINT64_C(0x33000);
    constexpr uint64_t Mask = UINT64_C(0xa55a);
    const uint32_t Memory = UINT32_C(0xf0f00f03);
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x42420000) + Lane;
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? static_cast<uint32_t>(bitCount(Memory))
                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, &Memory, sizeof(Memory));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Immediate rotates use EVEX.vvvv for the destination. Broadcast memory is
  // read once when at least one dword lane is active, then merged per lane.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x75, 0x5b,
                                           0x72, 0x08, 0x05};
    constexpr uint64_t Address = UINT64_C(0x34000);
    constexpr uint64_t Mask = UINT64_C(0x965a);
    const uint32_t Memory = UINT32_C(0x81c0ff03);
    const uint32_t Rotated = (Memory << 5) | (Memory >> 27);
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x51510000) + Lane;
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0 ? Rotated
                                                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, &Memory, sizeof(Memory));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Variable rotates use the memory operand as per-lane counts; a broadcast
  // count is fault-suppressed and replicated only for active lanes.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0x6d,
                                           0x5b, 0x14, 0x08};
    constexpr uint64_t Address = UINT64_C(0x35000);
    constexpr uint64_t Mask = UINT64_C(0xa669);
    const uint32_t MemoryCount = 37;
    std::vector<uint32_t> Source(16), Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Source.size(); ++Lane) {
      Source[Lane] = UINT32_C(0x81000003) + Lane * UINT32_C(0x10101);
      Old[Lane] = UINT32_C(0x61610000) + Lane;
      const uint32_t Rotated =
          (Source[Lane] >> 5) | (Source[Lane] << 27);
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0 ? Rotated
                                                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, &MemoryCount, sizeof(MemoryCount));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Out-of-range arithmetic variable shifts saturate to a sign fill. The
  // broadcast count is loaded only when at least one qword lane is active.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0xed,
                                           0x5b, 0x46, 0x08};
    constexpr uint64_t Address = UINT64_C(0x36000);
    constexpr uint64_t Mask = UINT64_C(0xad);
    const uint64_t MemoryCount = 70;
    std::vector<uint64_t> Source(8), Old(8), Expected(8);
    for (unsigned Lane = 0; Lane < Source.size(); ++Lane) {
      Source[Lane] = Lane % 2 == 0
                         ? UINT64_C(0x8000000000000000) + Lane
                         : UINT64_C(0x4000000000000000) + Lane;
      Old[Lane] = UINT64_C(0x7171000000000000) + Lane;
      const uint64_t SignFill = Lane % 2 == 0 ? UINT64_MAX : 0;
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0 ? SignFill
                                                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, &MemoryCount, sizeof(MemoryCount));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Uniform immediate shifts still apply the writemask per element. Logical
  // counts outside the lane width produce zero, including broadcast inputs.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x75, 0x5b,
                                           0x72, 0x10, 0x28};
    constexpr uint64_t Address = UINT64_C(0x37000);
    constexpr uint64_t Mask = UINT64_C(0x6c99);
    const uint32_t Memory = UINT32_C(0xf0f00f03);
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT32_C(0x81810000) + Lane;
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0 ? 0 : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, &Memory, sizeof(Memory));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // VPMULLQ is an EVEX packed multiply, not an insert/move. Its scalar
  // broadcast participates in ordinary qword masking and modular arithmetic.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0xed,
                                           0x5b, 0x40, 0x08};
    constexpr uint64_t Address = UINT64_C(0x38000);
    constexpr uint64_t Mask = UINT64_C(0xad);
    const uint64_t Memory = 3;
    std::vector<uint64_t> Source(8), Old(8), Expected(8);
    for (unsigned Lane = 0; Lane < Source.size(); ++Lane) {
      Source[Lane] = UINT64_C(0x7000000000000000) + Lane * 17;
      Old[Lane] = UINT64_C(0x9191000000000000) + Lane;
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? Source[Lane] * Memory
                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, &Memory, sizeof(Memory));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // The xmm/m128 form has one shared count source that is read before the
  // destination mask is applied, even when every output lane is inactive.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6d,
                                           0x4b, 0xf2, 0x08};
    constexpr uint64_t Address = UINT64_C(0x39000);
    const std::array<uint64_t, 2> MemoryCount = {
        5, UINT64_C(0xdeadbeefcafebabe)};
    std::vector<uint32_t> Source(16), Old(16);
    for (unsigned Lane = 0; Lane < Source.size(); ++Lane) {
      Source[Lane] = UINT32_C(0x10000001) + Lane;
      Old[Lane] = UINT32_C(0xa1a10000) + Lane;
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, MemoryCount.data(),
                     sizeof(MemoryCount));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, 0);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Old));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Widening signed multiplication consumes the low dword of each qword
  // source lane. A broadcast therefore reads one qword, not one dword.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0xed,
                                           0x5b, 0x28, 0x08};
    constexpr uint64_t Address = UINT64_C(0x3a000);
    constexpr uint64_t Mask = UINT64_C(0xb6);
    const uint64_t Memory = UINT64_C(0xfeedfacefffffffd);
    std::vector<uint32_t> SourceDwords(16);
    std::vector<uint64_t> Old(8), Expected(8);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      SourceDwords[Lane * 2] =
          static_cast<uint32_t>(static_cast<int32_t>(Lane) * 7 - 20);
      SourceDwords[Lane * 2 + 1] = UINT32_C(0xcccc0000) + Lane;
      Old[Lane] = UINT64_C(0xb1b1000000000000) + Lane;
      const int64_t Product =
          static_cast<int64_t>(static_cast<int32_t>(SourceDwords[Lane * 2])) *
          INT64_C(-3);
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? static_cast<uint64_t>(Product)
                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, &Memory, sizeof(Memory));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(SourceDwords));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // VNNI byte dot products treat the first source as unsigned bytes and the
  // broadcast memory dword as four signed byte multipliers per result lane.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0x6d,
                                           0x5b, 0x50, 0x08};
    constexpr uint64_t Address = UINT64_C(0x3b000);
    constexpr uint64_t Mask = UINT64_C(0x9a65);
    const std::array<int8_t, 4> Memory = {1, -2, 3, -4};
    std::vector<uint8_t> Source(64);
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      int64_t Dot = 0;
      for (unsigned Element = 0; Element < 4; ++Element) {
        const uint8_t Value =
            static_cast<uint8_t>(Lane * 9 + Element * 17 + 1);
        Source[Lane * 4 + Element] = Value;
        Dot += static_cast<int64_t>(Value) * Memory[Element];
      }
      Old[Lane] = UINT32_C(0xc1c10000) + Lane;
      Expected[Lane] =
          (Mask & (UINT64_C(1) << Lane)) != 0
              ? Old[Lane] + static_cast<uint32_t>(Dot)
              : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, Memory.data(), sizeof(Memory));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, Source);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());

    NdOpEmulator ZeroMaskEmulator(emptyImage());
    ZeroMaskEmulator.setStrictMode(true);
    ZeroMaskEmulator.setLoadCollect(true);
    ZeroMaskEmulator.setRegister(x86reg::RAX, Address);
    ZeroMaskEmulator.setRegister(x86reg::K3, 0);
    ZeroMaskEmulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    ZeroMaskEmulator.setRegisterBytes(x86reg::XMM2, Source);
    ASSERT_EQ(ZeroMaskEmulator.run(Ops), Ops.size());
    EXPECT_EQ(ZeroMaskEmulator.getRegisterBytes(x86reg::XMM1), bytes(Old));
    EXPECT_TRUE(ZeroMaskEmulator.getLoadRecords().empty());
    EXPECT_FALSE(ZeroMaskEmulator.skips().any());
  }

  // The signed-saturating byte form clamps after adding all four products to
  // the signed dword accumulator, while inactive lanes retain their value.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0x6d,
                                           0x4b, 0x51, 0xcb};
    constexpr uint64_t Mask = UINT64_C(0xd36b);
    std::vector<uint8_t> First(64);
    std::vector<int8_t> Second(64);
    std::vector<int32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      int64_t Dot = 0;
      for (unsigned Element = 0; Element < 4; ++Element) {
        First[Lane * 4 + Element] =
            static_cast<uint8_t>(180 + ((Lane + Element) % 60));
        if (Lane % 3 == 0)
          Second[Lane * 4 + Element] = 127;
        else if (Lane % 3 == 1)
          Second[Lane * 4 + Element] = -128;
        else
          Second[Lane * 4 + Element] =
              static_cast<int8_t>(Element % 2 == 0 ? Element + 1
                                                   : -int(Element + 1));
        Dot += static_cast<int64_t>(First[Lane * 4 + Element]) *
               Second[Lane * 4 + Element];
      }
      Old[Lane] = Lane % 3 == 0
                      ? INT32_MAX - 4
                      : (Lane % 3 == 1 ? INT32_MIN + 4
                                       : static_cast<int32_t>(1000 + Lane));
      const int64_t Sum = static_cast<int64_t>(Old[Lane]) + Dot;
      const int32_t Saturated = static_cast<int32_t>(std::clamp<int64_t>(
          Sum, static_cast<int64_t>(INT32_MIN),
          static_cast<int64_t>(INT32_MAX)));
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? Saturated
                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, First);
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Second));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Signed word pairs produce one wrapping dword accumulation per output
  // lane, including negative products and positive overflow.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0x6d,
                                           0x4b, 0x52, 0xcb};
    constexpr uint64_t Mask = UINT64_C(0xa95b);
    std::vector<int16_t> First(32), Second(32);
    std::vector<uint32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      const int16_t A0 = static_cast<int16_t>(30000 - Lane * 113);
      const int16_t A1 = static_cast<int16_t>(-29000 + Lane * 97);
      const int16_t B0 = static_cast<int16_t>(20000 - Lane * 71);
      const int16_t B1 = static_cast<int16_t>(-21000 + Lane * 59);
      First[Lane * 2] = A0;
      First[Lane * 2 + 1] = A1;
      Second[Lane * 2] = B0;
      Second[Lane * 2 + 1] = B1;
      Old[Lane] = UINT32_C(0x70000000) + Lane * UINT32_C(0x01010101);
      const int64_t Dot = static_cast<int64_t>(A0) * B0 +
                          static_cast<int64_t>(A1) * B1;
      const uint32_t Accumulated =
          static_cast<uint32_t>(static_cast<int64_t>(
                                    static_cast<int32_t>(Old[Lane])) +
                                Dot);
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? Accumulated
                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(First));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Second));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_FALSE(Emulator.skips().any());
  }

  // The signed-saturating word form clamps both overflow directions after
  // the two products have been accumulated.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0x6d,
                                           0x4b, 0x53, 0xcb};
    constexpr uint64_t Mask = UINT64_C(0x6db7);
    std::vector<int16_t> First(32), Second(32);
    std::vector<int32_t> Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      const bool Positive = Lane % 2 == 0;
      First[Lane * 2] = 32767;
      First[Lane * 2 + 1] = 32767;
      Second[Lane * 2] = Positive ? 32767 : -32768;
      Second[Lane * 2 + 1] = Positive ? 32767 : -32768;
      Old[Lane] = Positive ? INT32_MAX - 8 : INT32_MIN + 8;
      const int64_t Dot =
          static_cast<int64_t>(First[Lane * 2]) * Second[Lane * 2] +
          static_cast<int64_t>(First[Lane * 2 + 1]) *
              Second[Lane * 2 + 1];
      const int64_t Sum = static_cast<int64_t>(Old[Lane]) + Dot;
      const int32_t Saturated = static_cast<int32_t>(std::clamp<int64_t>(
          Sum, static_cast<int64_t>(INT32_MIN),
          static_cast<int64_t>(INT32_MAX)));
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? Saturated
                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(First));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Second));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_FALSE(Emulator.skips().any());
  }

  // The low IFMA form consumes only the low 52 bits of each qword and a
  // broadcast memory source is fault-suppressed when no qword is active.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0xed,
                                           0x5b, 0xb4, 0x08};
    constexpr uint64_t Address = UINT64_C(0x3c000);
    constexpr uint64_t Mask = UINT64_C(0xad);
    constexpr uint64_t OperandMask = UINT64_C(0x000fffffffffffff);
    const uint64_t Memory = UINT64_C(0xfedabcde12345678);
    std::vector<uint64_t> First(8), Old(8), Expected(8);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      First[Lane] = UINT64_C(0xabc1234567890) + Lane * 19;
      Old[Lane] = UINT64_C(0xf1f1000000000000) + Lane;
      const uint64_t Contribution =
          ((First[Lane] & OperandMask) * (Memory & OperandMask)) &
          OperandMask;
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? Old[Lane] + Contribution
                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, &Memory, sizeof(Memory));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(First));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());

    NdOpEmulator ZeroMaskEmulator(emptyImage());
    ZeroMaskEmulator.setStrictMode(true);
    ZeroMaskEmulator.setLoadCollect(true);
    ZeroMaskEmulator.setRegister(x86reg::RAX, Address);
    ZeroMaskEmulator.setRegister(x86reg::K3, 0);
    ZeroMaskEmulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    ZeroMaskEmulator.setRegisterBytes(x86reg::XMM2, bytes(First));
    ASSERT_EQ(ZeroMaskEmulator.run(Ops), Ops.size());
    EXPECT_EQ(ZeroMaskEmulator.getRegisterBytes(x86reg::XMM1), bytes(Old));
    EXPECT_TRUE(ZeroMaskEmulator.getLoadRecords().empty());
    EXPECT_FALSE(ZeroMaskEmulator.skips().any());
  }

  // The high IFMA form adds bits 103:52 of the unsigned 52-by-52 product.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0xed,
                                           0x4b, 0xb5, 0xcb};
    constexpr uint64_t Mask = UINT64_C(0xb6);
    constexpr uint64_t OperandMask = UINT64_C(0x000fffffffffffff);
    constexpr uint64_t HalfMask = (UINT64_C(1) << 26) - 1;
    auto highProductHalf = [=](uint64_t A, uint64_t B) {
      A &= OperandMask;
      B &= OperandMask;
      const uint64_t A0 = A & HalfMask;
      const uint64_t A1 = A >> 26;
      const uint64_t B0 = B & HalfMask;
      const uint64_t B1 = B >> 26;
      const uint64_t Carry = (A0 * B0) >> 26;
      const uint64_t Cross = A0 * B1 + A1 * B0 + Carry;
      return (A1 * B1 + (Cross >> 26)) & OperandMask;
    };
    std::vector<uint64_t> First(8), Second(8), Old(8), Expected(8);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      First[Lane] = UINT64_C(0xffedcba987654321) + Lane * 13;
      Second[Lane] = UINT64_C(0xeeabcde123456789) + Lane * 17;
      Old[Lane] = UINT64_C(0xd1d1000000000000) + Lane;
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? Old[Lane] +
                                 highProductHalf(First[Lane], Second[Lane])
                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(First));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Second));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_FALSE(Emulator.skips().any());
  }

  // An immediate left double shift may broadcast its second dword source;
  // inactive result lanes neither consume memory nor change their old value.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0x6d, 0x5b,
                                           0x71, 0x08, 0x0d};
    constexpr uint64_t Address = UINT64_C(0x3d000);
    constexpr uint64_t Mask = UINT64_C(0x96a5);
    constexpr unsigned Count = 13;
    const uint32_t Memory = UINT32_C(0x89abcdef);
    std::vector<uint32_t> First(16), Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      First[Lane] = UINT32_C(0x10203040) + Lane * UINT32_C(0x01020408);
      Old[Lane] = UINT32_C(0xe1e10000) + Lane;
      const uint32_t Shifted =
          static_cast<uint32_t>((First[Lane] << Count) |
                                (Memory >> (32 - Count)));
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? Shifted
                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, &Memory, sizeof(Memory));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(First));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Variable right double shifts read one count per active qword. A zero
  // count preserves the primary destination lane instead of shifting by 64.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0xed,
                                           0x4b, 0x73, 0x08};
    constexpr uint64_t Address = UINT64_C(0x3e000);
    constexpr uint64_t Mask = UINT64_C(0xad);
    const std::vector<uint64_t> Counts = {0, 1, 63, 64, 65, 17, 127, 32};
    std::vector<uint64_t> Source(8), Old(8), Expected(8);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Source[Lane] = UINT64_C(0x0123456789abcdef) + Lane * 31;
      Old[Lane] = UINT64_C(0xfedcba9876543210) - Lane * 29;
      const unsigned Count = static_cast<unsigned>(Counts[Lane] & 63);
      const uint64_t Shifted =
          Count == 0 ? Old[Lane]
                     : (Old[Lane] >> Count) | (Source[Lane] << (64 - Count));
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? Shifted
                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, Counts.data(),
                     Counts.size() * sizeof(uint64_t));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), bitCount(Mask));
    EXPECT_FALSE(Emulator.skips().any());

    NdOpEmulator ZeroMaskEmulator(emptyImage());
    ZeroMaskEmulator.setStrictMode(true);
    ZeroMaskEmulator.setLoadCollect(true);
    ZeroMaskEmulator.setRegister(x86reg::RAX, Address);
    ZeroMaskEmulator.setRegister(x86reg::K3, 0);
    ZeroMaskEmulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    ZeroMaskEmulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(ZeroMaskEmulator.run(Ops), Ops.size());
    EXPECT_EQ(ZeroMaskEmulator.getRegisterBytes(x86reg::XMM1), bytes(Old));
    EXPECT_TRUE(ZeroMaskEmulator.getLoadRecords().empty());
    EXPECT_FALSE(ZeroMaskEmulator.skips().any());
  }

  // VPERMW has a full-vector, non-fault-suppressing memory source. The
  // writemask controls output words, but a zero mask still reads the tuple.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0xed,
                                           0x4b, 0x8d, 0x08};
    constexpr uint64_t Address = UINT64_C(0x3f000);
    constexpr uint64_t Mask = UINT64_C(0x96a5c33c);
    std::vector<uint16_t> Indices(32), Data(32), Old(32), Expected(32);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Indices[Lane] = static_cast<uint16_t>(Lane * 11 + 37);
      Data[Lane] = static_cast<uint16_t>(UINT16_C(0x2100) + Lane * 23);
      Old[Lane] = static_cast<uint16_t>(UINT16_C(0xe100) + Lane);
    }
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane)
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? Data[Indices[Lane] & 31]
                           : Old[Lane];

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, Data.data(), Data.size() * sizeof(Data[0]));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Indices));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());

    NdOpEmulator ZeroMaskEmulator(Image);
    ZeroMaskEmulator.setStrictMode(true);
    ZeroMaskEmulator.setLoadCollect(true);
    ZeroMaskEmulator.setRegister(x86reg::RAX, Address);
    ZeroMaskEmulator.setRegister(x86reg::K3, 0);
    ZeroMaskEmulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    ZeroMaskEmulator.setRegisterBytes(x86reg::XMM2, bytes(Indices));
    ASSERT_EQ(ZeroMaskEmulator.run(Ops), Ops.size());
    EXPECT_EQ(ZeroMaskEmulator.getRegisterBytes(x86reg::XMM1), bytes(Old));
    EXPECT_EQ(ZeroMaskEmulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(ZeroMaskEmulator.skips().any());
  }

  // VPMULTISHIFTQB also has non-fault-suppressing memory semantics. A
  // broadcast qword supplies the wrapped 8-bit window for every output byte.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0xed,
                                           0x5b, 0x83, 0x08};
    constexpr uint64_t Address = UINT64_C(0x40000);
    constexpr uint64_t Mask = UINT64_C(0xa5c33c9669f00f5a);
    const uint64_t Data = UINT64_C(0xfedcba9876543210);
    std::vector<uint8_t> Controls(64), Old(64), Expected(64);
    for (unsigned Byte = 0; Byte < Old.size(); ++Byte) {
      Controls[Byte] = static_cast<uint8_t>(Byte * 13 + 67);
      Old[Byte] = static_cast<uint8_t>(0xc0 + Byte);
      const unsigned Count = Controls[Byte] & 63;
      const uint64_t Rotated =
          Count == 0 ? Data : (Data >> Count) | (Data << (64 - Count));
      Expected[Byte] = (Mask & (UINT64_C(1) << Byte)) != 0
                           ? static_cast<uint8_t>(Rotated)
                           : Old[Byte];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, &Data, sizeof(Data));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, Old);
    Emulator.setRegisterBytes(x86reg::XMM2, Controls);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), Expected);
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());

    NdOpEmulator ZeroMaskEmulator(Image);
    ZeroMaskEmulator.setStrictMode(true);
    ZeroMaskEmulator.setLoadCollect(true);
    ZeroMaskEmulator.setRegister(x86reg::RAX, Address);
    ZeroMaskEmulator.setRegister(x86reg::K3, 0);
    ZeroMaskEmulator.setRegisterBytes(x86reg::XMM1, Old);
    ZeroMaskEmulator.setRegisterBytes(x86reg::XMM2, Controls);
    ASSERT_EQ(ZeroMaskEmulator.run(Ops), Ops.size());
    EXPECT_EQ(ZeroMaskEmulator.getRegisterBytes(x86reg::XMM1), Old);
    EXPECT_EQ(ZeroMaskEmulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(ZeroMaskEmulator.skips().any());
  }

  // VPSHUFBITQMB writes one mask bit per control byte. Its optional mask is
  // zeroing-only and suppresses the corresponding byte loads from memory.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0x6d,
                                           0x4b, 0x8f, 0x08};
    constexpr uint64_t Address = UINT64_C(0x41000);
    constexpr uint64_t Mask = UINT64_C(0xa55ac33c9669f00f);
    std::vector<uint64_t> Data(8);
    std::vector<uint8_t> Controls(64);
    uint64_t Expected = 0;
    for (unsigned Qword = 0; Qword < Data.size(); ++Qword)
      Data[Qword] = UINT64_C(0x8040201008040201) ^
                    (UINT64_C(0x0101010101010101) * Qword);
    for (unsigned Bit = 0; Bit < Controls.size(); ++Bit) {
      Controls[Bit] = static_cast<uint8_t>(Bit * 19 + 71);
      if ((Mask & (UINT64_C(1) << Bit)) != 0 &&
          ((Data[Bit / 8] >> (Controls[Bit] & 63)) & 1) != 0)
        Expected |= UINT64_C(1) << Bit;
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, Controls.data(), Controls.size());
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K1, UINT64_MAX);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Data));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::K1), Expected);
    EXPECT_EQ(Emulator.getLoadRecords().size(), bitCount(Mask));
    EXPECT_FALSE(Emulator.skips().any());

    NdOpEmulator ZeroMaskEmulator(emptyImage());
    ZeroMaskEmulator.setStrictMode(true);
    ZeroMaskEmulator.setLoadCollect(true);
    ZeroMaskEmulator.setRegister(x86reg::RAX, Address);
    ZeroMaskEmulator.setRegister(x86reg::K1, UINT64_MAX);
    ZeroMaskEmulator.setRegister(x86reg::K3, 0);
    ZeroMaskEmulator.setRegisterBytes(x86reg::XMM2, bytes(Data));
    ASSERT_EQ(ZeroMaskEmulator.run(Ops), Ops.size());
    EXPECT_EQ(ZeroMaskEmulator.getRegister(x86reg::K1), 0U);
    EXPECT_TRUE(ZeroMaskEmulator.getLoadRecords().empty());
    EXPECT_FALSE(ZeroMaskEmulator.skips().any());
  }

  // VPBLENDMD uses its opmask as a source selector. With EVEX.z, unselected
  // lanes become zero, while selected lanes consume one broadcast dword.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0x6d,
                                           0xdb, 0x64, 0x08};
    constexpr uint64_t Address = UINT64_C(0x42000);
    constexpr uint64_t Mask = UINT64_C(0x96a5);
    const uint32_t Memory = UINT32_C(0x89abcdef);
    std::vector<uint32_t> First(16), Old(16), Expected(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      First[Lane] = UINT32_C(0x11110000) + Lane;
      Old[Lane] = UINT32_C(0xe2e20000) + Lane;
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0 ? Memory : 0;
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, &Memory, sizeof(Memory));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(First));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(), 1U);
    EXPECT_FALSE(Emulator.skips().any());

    NdOpEmulator ZeroMaskEmulator(emptyImage());
    ZeroMaskEmulator.setStrictMode(true);
    ZeroMaskEmulator.setLoadCollect(true);
    ZeroMaskEmulator.setRegister(x86reg::RAX, Address);
    ZeroMaskEmulator.setRegister(x86reg::K3, 0);
    ZeroMaskEmulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    ZeroMaskEmulator.setRegisterBytes(x86reg::XMM2, bytes(First));
    ASSERT_EQ(ZeroMaskEmulator.run(Ops), Ops.size());
    EXPECT_EQ(ZeroMaskEmulator.getRegisterBytes(x86reg::XMM1),
              bytes(std::vector<uint32_t>(16, 0)));
    EXPECT_TRUE(ZeroMaskEmulator.getLoadRecords().empty());
    EXPECT_FALSE(ZeroMaskEmulator.skips().any());
  }

  // VPSCATTERDD computes every active VSIB address independently, stores only
  // active dwords, clears the completed mask, and suppresses all memory access
  // when the mask is zero.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0x7d, 0x49,
                                           0xa0, 0x4c, 0x90, 0x08};
    constexpr uint64_t Base = UINT64_C(0x43000);
    constexpr uint64_t Mask = UINT64_C(0xa55a);
    constexpr uint64_t Displacement = 32;
    std::vector<int32_t> Indices(16);
    std::vector<uint32_t> Source(16);
    std::vector<uint8_t> Initial(Displacement + 16 * 16, 0xcc);
    for (unsigned Lane = 0; Lane < Indices.size(); ++Lane) {
      Indices[Lane] = static_cast<int32_t>(Lane * 4);
      Source[Lane] = UINT32_C(0x71820000) + Lane;
      const uint32_t Old = UINT32_C(0x31420000) + Lane;
      std::memcpy(Initial.data() + Displacement + Lane * 16, &Old,
                  sizeof(Old));
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    Segment Memory;
    Memory.VA = Base;
    Memory.Size = Initial.size();
    Memory.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Memory.Data = Initial;
    Image.Segments.push_back(std::move(Memory));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::RAX, Base);
    Emulator.setRegister(x86reg::K1, UINT64_C(0xcafe000000000000) | Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Source));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Indices));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::K1), 0U);
    for (unsigned Lane = 0; Lane < Indices.size(); ++Lane) {
      LowOp Load;
      Load.Opcode = NdOp::LOAD;
      Load.Output = NdVar::tmp(UINT64_C(0x7c000000) + Lane, 4);
      Load.addInput(NdVar::cst(0, 8));
      Load.addInput(NdVar::cst(Base + Displacement + Lane * 16, 8));
      ASSERT_TRUE(Emulator.step(Load));
      const std::optional<uint64_t> Actual =
          Emulator.getRegister(UINT64_C(0x7c000000) + Lane);
      ASSERT_TRUE(Actual);
      const uint32_t Expected = (Mask & (UINT64_C(1) << Lane)) != 0
                                    ? Source[Lane]
                                    : UINT32_C(0x31420000) + Lane;
      EXPECT_EQ(*Actual, Expected);
    }
    EXPECT_FALSE(Emulator.skips().any());

    NdOpEmulator ZeroMaskEmulator(emptyImage());
    ZeroMaskEmulator.setStrictMode(true);
    ZeroMaskEmulator.setRegister(x86reg::RAX, UINT64_C(0xdead0000));
    ZeroMaskEmulator.setRegister(x86reg::K1, 0);
    ZeroMaskEmulator.setRegisterBytes(x86reg::XMM1, bytes(Source));
    ZeroMaskEmulator.setRegisterBytes(x86reg::XMM2, bytes(Indices));
    ASSERT_EQ(ZeroMaskEmulator.run(Ops), Ops.size());
    EXPECT_EQ(ZeroMaskEmulator.getRegister(x86reg::K1), 0U);
    EXPECT_FALSE(ZeroMaskEmulator.skips().any());
  }

  // VUNPCKHPD interleaves independently inside every 128-bit lane before
  // applying its qword writemask.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0xed,
                                           0x4b, 0x15, 0xcb};
    constexpr uint64_t Mask = UINT64_C(0xb5);
    const std::vector<uint64_t> Left = {
        10, 11, 20, 21, 30, 31, 40, 41,
    };
    const std::vector<uint64_t> Right = {
        110, 111, 120, 121, 130, 131, 140, 141,
    };
    const std::vector<uint64_t> Raw = {
        11, 111, 21, 121, 31, 131, 41, 141,
    };
    std::vector<uint64_t> Old(8), Expected(8);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane) {
      Old[Lane] = UINT64_C(0x4242000000000000) + Lane;
      Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                           ? Raw[Lane]
                           : Old[Lane];
    }

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(Right));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    EXPECT_FALSE(Emulator.skips().any());
  }

  // A zero writemask also suppresses an unpack broadcast source when no
  // destination element consumes that source.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6c,
                                           0x5b, 0x14, 0x08};
    std::vector<uint32_t> Left(16, UINT32_C(0x3f800000));
    std::vector<uint32_t> Old(16);
    for (unsigned Lane = 0; Lane < Old.size(); ++Lane)
      Old[Lane] = UINT32_C(0x42420000) + Lane;

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::K3, 0);
    Emulator.setRegister(x86reg::RAX, UINT64_C(0xdead0000));
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(Old));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Old));
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());
  }

  // VCMPPS accepts all 256 immediate bytes: only imm8[4:0] selects the
  // predicate.  Lift and emulate every encoding while also checking the
  // quiet/signaling NaN split and the four relation classes.
  {
    static constexpr uint8_t Truth[16] = {
        0x2, 0x1, 0x3, 0x8, 0xd, 0xe, 0xc, 0x7,
        0xa, 0x9, 0xb, 0x0, 0x5, 0x6, 0x4, 0xf,
    };
    static constexpr bool SignalsOnQuietNaN[32] = {
        false, true,  true,  false, false, true,  true,  false,
        false, true,  true,  false, false, true,  true,  false,
        true,  false, false, true,  true,  false, false, true,
        true,  false, false, true,  true,  false, false, true,
    };
    std::vector<uint32_t> Left(16), Right(16);
    Left[0] = UINT32_C(0x3f800000);  // less
    Right[0] = UINT32_C(0x40000000);
    Left[1] = UINT32_C(0x40000000);  // equal
    Right[1] = UINT32_C(0x40000000);
    Left[2] = UINT32_C(0x40400000);  // greater
    Right[2] = UINT32_C(0x40000000);
    Left[3] = UINT32_C(0x7fc00001);  // unordered quiet NaN
    Right[3] = UINT32_C(0x3f800000);

    for (unsigned Immediate = 0; Immediate < 256; ++Immediate) {
      SCOPED_TRACE(Immediate);
      const std::vector<uint8_t> Encoding = {
          0x62, 0xf1, 0x6c, 0x4a, 0xc2, 0xcb,
          static_cast<uint8_t>(Immediate)};
      const std::vector<LowOp> Ops = liftX64(Encoding);
      ASSERT_FALSE(Ops.empty());
      BinaryImage Image = emptyImage();
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setMXCSR(0x1f80);
      Emulator.setRegister(x86reg::K1, UINT64_MAX);
      Emulator.setRegister(x86reg::K2, 0x0f);
      Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
      Emulator.setRegisterBytes(x86reg::XMM3, bytes(Right));
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      EXPECT_EQ(Emulator.getRegister(x86reg::K1),
                static_cast<uint64_t>(Truth[Immediate & 0x0f]));
      EXPECT_EQ((Emulator.getMXCSR() & 1U) != 0,
                SignalsOnQuietNaN[Immediate & 0x1f]);
      EXPECT_FALSE(Emulator.skips().any());
    }
  }

  // A fully masked VCMP memory operand performs no address validation or
  // load.  Its mask destination is still zeroed.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf1, 0x6c, 0x4a,
                                           0xc2, 0x08, 0x00};
    std::vector<uint32_t> Left(16, UINT32_C(0x3f800000));
    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, UINT64_C(0x0100000000000000));
    Emulator.setRegister(x86reg::K1, UINT64_MAX);
    Emulator.setRegister(x86reg::K2, 0);
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Left));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::K1), 0U);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Keep the strict scalar-folding regressions in this single focused test:
  // x86 DIV/IDIV lifting can present 128- or 256-bit concatenated dividends
  // to the lightweight emulator.  Every operand and result remains full-width
  // rather than being silently truncated through its low 64-bit view.
  {
    auto arithmetic = [](NdOp Opcode, uint64_t Output, uint16_t OutputSize,
                         uint64_t Left, uint16_t LeftSize, uint64_t Right,
                         uint16_t RightSize) {
      LowOp Op;
      Op.Opcode = Opcode;
      Op.Output = NdVar::reg(Output, OutputSize);
      Op.addInput(NdVar::reg(Left, LeftSize));
      Op.addInput(NdVar::reg(Right, RightSize));
      return Op;
    };
    auto limbs = [](std::initializer_list<uint64_t> Values) {
      std::vector<uint64_t> Words(Values);
      return bytes(Words);
    };

    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);

    // Exercise every architectural DIV/IDIV width from decoded bytes.  The
    // precondition must precede target-independent division, accept both
    // signed endpoints, and stop before AL/AH, AX/DX, EAX/EDX, or RAX/RDX is
    // changed when the quotient lies one step outside its range.
    const auto RunLiftedDiv = [&](const char *Name,
                                  const std::vector<uint8_t> &Encoding,
                                  uint16_t Size, uint64_t Low, uint64_t High,
                                  uint64_t Divisor, bool ExpectSuccess,
                                  uint64_t ExpectedQuotient) {
      SCOPED_TRACE(Name);
      const std::vector<LowOp> Ops = liftX64(Encoding);
      ASSERT_FALSE(Ops.empty());
      const auto Guard = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
        return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 4 &&
               Op.Inputs[0].isConst() &&
               static_cast<Intrinsic>(Op.Inputs[0].Offset) ==
                   Intrinsic::X86RequireDivPrecondition;
      });
      const auto Divide = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
        return Op.Opcode == NdOp::INT_DIV || Op.Opcode == NdOp::INT_SDIV;
      });
      ASSERT_NE(Guard, Ops.end());
      ASSERT_NE(Divide, Ops.end());
      EXPECT_LT(std::distance(Ops.begin(), Guard),
                std::distance(Ops.begin(), Divide));

      const unsigned Bits = Size * 8;
      const uint64_t Mask =
          Bits == 64 ? UINT64_MAX : (UINT64_C(1) << Bits) - 1;
      uint64_t RaxBefore = UINT64_C(0x5a5a5a5a5a5a5a5a);
      uint64_t RdxBefore = UINT64_C(0xa5a5a5a5a5a5a5a5);
      if (Size == 1) {
        RaxBefore = (RaxBefore & ~UINT64_C(0xffff)) |
                    ((High & Mask) << 8) | (Low & Mask);
      } else {
        RaxBefore = (RaxBefore & ~Mask) | (Low & Mask);
        RdxBefore = (RdxBefore & ~Mask) | (High & Mask);
      }
      const uint64_t RcxBefore =
          (UINT64_C(0x3c3c3c3c3c3c3c3c) & ~Mask) | (Divisor & Mask);

      NdOpEmulator Lifted(Image);
      Lifted.setStrictMode(true);
      Lifted.setRegister(x86reg::RAX, RaxBefore);
      Lifted.setRegister(x86reg::RDX, RdxBefore);
      Lifted.setRegister(x86reg::RCX, RcxBefore);
      Lifted.setRegister(x86reg::CF, 1);
      Lifted.setRegister(x86reg::PF, 0);
      Lifted.setRegister(x86reg::AF, 1);
      Lifted.setRegister(x86reg::ZF, 0);
      Lifted.setRegister(x86reg::SF, 1);
      Lifted.setRegister(x86reg::OF, 0);

      const size_t Completed = Lifted.run(Ops);
      if (!ExpectSuccess) {
        EXPECT_LT(Completed, Ops.size());
        EXPECT_EQ(Lifted.getRegister(x86reg::RAX), RaxBefore);
        EXPECT_EQ(Lifted.getRegister(x86reg::RDX), RdxBefore);
        EXPECT_EQ(Lifted.getRegister(x86reg::RCX), RcxBefore);
        EXPECT_EQ(Lifted.getRegister(x86reg::CF), 1U);
        EXPECT_EQ(Lifted.getRegister(x86reg::PF), 0U);
        EXPECT_EQ(Lifted.getRegister(x86reg::AF), 1U);
        EXPECT_EQ(Lifted.getRegister(x86reg::ZF), 0U);
        EXPECT_EQ(Lifted.getRegister(x86reg::SF), 1U);
        EXPECT_EQ(Lifted.getRegister(x86reg::OF), 0U);
      } else {
        ASSERT_EQ(Completed, Ops.size());
        const std::optional<uint64_t> RaxAfter =
            Lifted.getRegister(x86reg::RAX);
        ASSERT_TRUE(RaxAfter.has_value());
        if (Size == 1) {
          EXPECT_EQ(*RaxAfter & UINT64_C(0xffff),
                    ExpectedQuotient & Mask);
        } else {
          const std::optional<uint64_t> RdxAfter =
              Lifted.getRegister(x86reg::RDX);
          ASSERT_TRUE(RdxAfter.has_value());
          EXPECT_EQ(*RaxAfter & Mask, ExpectedQuotient & Mask);
          EXPECT_EQ(*RdxAfter & Mask, 0U);
        }
      }
      EXPECT_FALSE(Lifted.skips().any());
    };

    struct DivWidthCase {
      const char *Name;
      uint16_t Size;
      std::vector<uint8_t> UnsignedEncoding;
      std::vector<uint8_t> SignedEncoding;
    };
    const std::vector<DivWidthCase> DivWidths = {
        {"byte", 1, {0xf6, 0xf1}, {0xf6, 0xf9}},
        {"word", 2, {0x66, 0xf7, 0xf1}, {0x66, 0xf7, 0xf9}},
        {"dword", 4, {0xf7, 0xf1}, {0xf7, 0xf9}},
        {"qword", 8, {0x48, 0xf7, 0xf1}, {0x48, 0xf7, 0xf9}},
    };
    for (const DivWidthCase &Width : DivWidths) {
      const unsigned Bits = Width.Size * 8;
      const uint64_t Mask =
          Bits == 64 ? UINT64_MAX : (UINT64_C(1) << Bits) - 1;
      const uint64_t SignedLimit = UINT64_C(1) << (Bits - 1);
      RunLiftedDiv(Width.Name, Width.UnsignedEncoding, Width.Size, Mask, 0, 1,
                   true, Mask);
      RunLiftedDiv(Width.Name, Width.UnsignedEncoding, Width.Size, 0, 1, 1,
                   false, 0);
      RunLiftedDiv(Width.Name, Width.UnsignedEncoding, Width.Size, 0, 0, 0,
                   false, 0);
      RunLiftedDiv(Width.Name, Width.SignedEncoding, Width.Size,
                   SignedLimit - 1, 0, 1, true, SignedLimit - 1);
      RunLiftedDiv(Width.Name, Width.SignedEncoding, Width.Size, SignedLimit,
                   0, 1, false, 0);
      RunLiftedDiv(Width.Name, Width.SignedEncoding, Width.Size, SignedLimit,
                   Mask, 1, true, SignedLimit);
      RunLiftedDiv(Width.Name, Width.SignedEncoding, Width.Size,
                   (SignedLimit - 1) & Mask, Mask, 1, false, 0);
      RunLiftedDiv(Width.Name, Width.SignedEncoding, Width.Size, SignedLimit,
                   Mask, Mask, false, 0);
    }

    // Byte DIV writes the quotient to AL and the non-zero remainder to AH.
    // Keep the upper six bytes intact while proving the CONCAT/writeback path
    // does not accidentally discard AH.
    {
      const std::vector<LowOp> Ops = liftX64({0xf6, 0xf1});
      ASSERT_FALSE(Ops.empty());
      NdOpEmulator ByteDiv(Image);
      ByteDiv.setStrictMode(true);
      constexpr uint64_t RaxBefore = UINT64_C(0x5a5a5a5a5a5a0101);
      constexpr uint64_t RaxAfter = UINT64_C(0x5a5a5a5a5a5a0180);
      constexpr uint64_t RcxBefore = UINT64_C(0x3c3c3c3c3c3c3c02);
      ByteDiv.setRegister(x86reg::RAX, RaxBefore);
      ByteDiv.setRegister(x86reg::RCX, RcxBefore);
      ASSERT_EQ(ByteDiv.run(Ops), Ops.size());
      EXPECT_EQ(ByteDiv.getRegister(x86reg::RAX), RaxAfter);
      EXPECT_EQ(ByteDiv.getRegister(x86reg::RCX), RcxBefore);
      EXPECT_FALSE(ByteDiv.skips().any());
    }

    // (2^64 + 5) / 10 has a quotient that is not representable by an
    // accidental low-64-bit dividend, so this checks both 128-bit results.
    const auto Dividend128 = limbs({5, 1});
    const auto Divisor128 = limbs({10, 0});
    Emulator.setRegisterBytes(0x71000000, Dividend128);
    Emulator.setRegisterBytes(0x71000010, Divisor128);
    ASSERT_TRUE(Emulator.step(arithmetic(NdOp::INT_DIV, 0x71000020, 16,
                                         0x71000000, 16, 0x71000010, 16)));
    EXPECT_EQ(Emulator.getRegisterBytes(0x71000020),
              limbs({UINT64_C(1844674407370955162), 0}));
    ASSERT_TRUE(Emulator.step(arithmetic(NdOp::INT_REM, 0x71000030, 16,
                                         0x71000000, 16, 0x71000010, 16)));
    EXPECT_EQ(Emulator.getRegisterBytes(0x71000030), limbs({1, 0}));

    // 2^192 + 123 divided by 2^64 exercises bits in all four 256-bit limbs.
    const auto Dividend256 = limbs({123, 0, 0, 1});
    const auto Divisor256 = limbs({0, 1, 0, 0});
    Emulator.setRegisterBytes(0x71000100, Dividend256);
    Emulator.setRegisterBytes(0x71000120, Divisor256);
    ASSERT_TRUE(Emulator.step(arithmetic(NdOp::INT_DIV, 0x71000140, 32,
                                         0x71000100, 32, 0x71000120, 32)));
    EXPECT_EQ(Emulator.getRegisterBytes(0x71000140),
              limbs({0, 0, 1, 0}));
    ASSERT_TRUE(Emulator.step(arithmetic(NdOp::INT_REM, 0x71000160, 32,
                                         0x71000100, 32, 0x71000120, 32)));
    EXPECT_EQ(Emulator.getRegisterBytes(0x71000160),
              limbs({123, 0, 0, 0}));

    // Signed division truncates toward zero and the remainder keeps the
    // dividend's sign at both wide operand sizes.
    for (const uint16_t Width : {uint16_t{16}, uint16_t{32}}) {
      SCOPED_TRACE(Width);
      const auto NegativeFifteen =
          Width == 16 ? limbs({UINT64_C(0xfffffffffffffff1), UINT64_MAX})
                      : limbs({UINT64_C(0xfffffffffffffff1), UINT64_MAX,
                               UINT64_MAX, UINT64_MAX});
      const auto Four = Width == 16 ? limbs({4, 0}) : limbs({4, 0, 0, 0});
      const auto NegativeThree =
          Width == 16 ? limbs({UINT64_C(0xfffffffffffffffd), UINT64_MAX})
                      : limbs({UINT64_C(0xfffffffffffffffd), UINT64_MAX,
                               UINT64_MAX, UINT64_MAX});
      const uint64_t Base = UINT64_C(0x71000200) + Width;
      Emulator.setRegisterBytes(Base, NegativeFifteen);
      Emulator.setRegisterBytes(Base + 0x40, Four);
      ASSERT_TRUE(Emulator.step(arithmetic(NdOp::INT_SDIV, Base + 0x80,
                                           Width, Base, Width, Base + 0x40,
                                           Width)));
      EXPECT_EQ(Emulator.getRegisterBytes(Base + 0x80), NegativeThree);
      ASSERT_TRUE(Emulator.step(arithmetic(NdOp::INT_SREM, Base + 0xc0,
                                           Width, Base, Width, Base + 0x40,
                                           Width)));
      EXPECT_EQ(Emulator.getRegisterBytes(Base + 0xc0), NegativeThree);
    }

    // The same signed rules apply below 64 bits.  Host int64_t casts must not
    // reinterpret a negative i16 operand as a positive scalar.
    Emulator.setRegister(0x71000300, UINT64_C(0xfff1)); // i16 -15
    Emulator.setRegister(0x71000310, 4);
    ASSERT_TRUE(Emulator.step(arithmetic(NdOp::INT_SDIV, 0x71000320, 2,
                                         0x71000300, 2, 0x71000310, 2)));
    EXPECT_EQ(Emulator.getRegister(0x71000320), UINT64_C(0xfffd));
    ASSERT_TRUE(Emulator.step(arithmetic(NdOp::INT_SREM, 0x71000330, 2,
                                         0x71000300, 2, 0x71000310, 2)));
    EXPECT_EQ(Emulator.getRegister(0x71000330), UINT64_C(0xfffd));
    Emulator.setRegister(0x71000340, UINT64_C(0x8000));
    Emulator.setRegister(0x71000350, UINT64_C(0xffff));
    EXPECT_FALSE(Emulator.step(arithmetic(NdOp::INT_SDIV, 0x71000360, 2,
                                          0x71000340, 2, 0x71000350, 2)));
    EXPECT_FALSE(Emulator.getRegister(0x71000360).has_value());

    // Architectural divide errors and malformed mixed-width LowIR fail
    // closed without materializing a destination value.
    const auto Zero256 = limbs({0, 0, 0, 0});
    Emulator.setRegisterBytes(0x71000400, Zero256);
    EXPECT_FALSE(Emulator.step(arithmetic(NdOp::INT_DIV, 0x71000420, 32,
                                          0x71000100, 32, 0x71000400, 32)));
    EXPECT_FALSE(Emulator.getRegisterBytes(0x71000420).has_value());

    const auto SignedMinimum256 =
        limbs({0, 0, 0, UINT64_C(0x8000000000000000)});
    const auto NegativeOne256 =
        limbs({UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX});
    Emulator.setRegisterBytes(0x71000440, SignedMinimum256);
    Emulator.setRegisterBytes(0x71000460, NegativeOne256);
    EXPECT_FALSE(Emulator.step(arithmetic(NdOp::INT_SDIV, 0x71000480, 32,
                                          0x71000440, 32, 0x71000460, 32)));
    EXPECT_FALSE(Emulator.getRegisterBytes(0x71000480).has_value());
    EXPECT_FALSE(Emulator.step(arithmetic(NdOp::INT_SREM, 0x710004a0, 32,
                                          0x71000440, 32, 0x71000460, 32)));
    EXPECT_FALSE(Emulator.getRegisterBytes(0x710004a0).has_value());
    EXPECT_FALSE(Emulator.step(arithmetic(NdOp::INT_DIV, 0x710004c0, 16,
                                          0x71000100, 32, 0x71000010, 16)));
    EXPECT_FALSE(Emulator.getRegisterBytes(0x710004c0).has_value());
    EXPECT_FALSE(Emulator.skips().any());
  }

  // x87 remainder loops depend on persistent C0/C1/C2/C3 state, while
  // FNINIT/FNCLEX and status reads are used by real control-flow probes.
  // Exercise those operations directly so strict emulation cannot regress to
  // an opaque intrinsic that merely preserves stale values.
  {
    auto x87Value = [](uint64_t Significand, uint16_t SignExponent) {
      std::vector<uint8_t> Value(x86reg::FPURegSize, 0);
      std::memcpy(Value.data(), &Significand, sizeof(Significand));
      std::memcpy(Value.data() + sizeof(Significand), &SignExponent,
                  sizeof(SignExponent));
      return Value;
    };
    auto intrinsic = [](Intrinsic Id) {
      LowOp Op;
      Op.Opcode = NdOp::INTRINSIC;
      Op.addInput(NdVar::cst(static_cast<uint64_t>(Id), 8));
      return Op;
    };
    auto remainder = [&](Intrinsic Id) {
      LowOp Op = intrinsic(Id);
      Op.Output = NdVar::reg(x86reg::ST0, x86reg::FPURegSize);
      Op.addInput(NdVar::reg(x86reg::ST0, x86reg::FPURegSize));
      Op.addInput(NdVar::reg(x86reg::ST1, x86reg::FPURegSize));
      return Op;
    };

    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    EXPECT_EQ(Emulator.getRegister(x86reg::FPU_CW), UINT64_C(0x037f));
    EXPECT_EQ(Emulator.getRegister(x86reg::FPU_SW), 0U);

    Emulator.setRegister(x86reg::FPU_CW, 0x0c7f);
    Emulator.setRegister(x86reg::FPU_SW, 0xffff);
    Emulator.setRegisterBytes(
        x86reg::ST0, x87Value(UINT64_C(0x8000000000000000), 0x3fff));
    const std::vector<LowOp> FninitOps = liftX64({0xdb, 0xe3});
    ASSERT_FALSE(FninitOps.empty());
    ASSERT_EQ(Emulator.run(FninitOps), FninitOps.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::FPU_CW), UINT64_C(0x037f));
    EXPECT_EQ(Emulator.getRegister(x86reg::FPU_SW), 0U);
    EXPECT_FALSE(Emulator.getRegisterBytes(x86reg::ST0).has_value());

    // FNSTCW must expose the same reset state through a lifted memory store.
    {
      constexpr uint64_t ControlAddress = UINT64_C(0x73000000);
      BinaryImage MemoryImage = emptyImage();
      Segment Mapping;
      Mapping.VA = ControlAddress;
      Mapping.Size = 2;
      Mapping.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Mapping.Data.resize(2);
      MemoryImage.Segments.push_back(std::move(Mapping));
      NdOpEmulator MemoryEmulator(MemoryImage);
      MemoryEmulator.setStrictMode(true);
      MemoryEmulator.setRegister(x86reg::RDI, ControlAddress);
      const std::vector<LowOp> FnstcwOps = liftX64({0xd9, 0x3f});
      ASSERT_FALSE(FnstcwOps.empty());
      ASSERT_EQ(MemoryEmulator.run(FnstcwOps), FnstcwOps.size());
      LowOp LoadControl;
      LoadControl.Opcode = NdOp::LOAD;
      LoadControl.Output = NdVar::tmp(UINT64_C(0x73000010), 2);
      LoadControl.addInput(NdVar::cst(0, 8));
      LoadControl.addInput(NdVar::cst(ControlAddress, 8));
      ASSERT_TRUE(MemoryEmulator.step(LoadControl));
      EXPECT_EQ(MemoryEmulator.getRegister(UINT64_C(0x73000010)),
                UINT64_C(0x037f));
    }

    Emulator.setRegister(x86reg::FPU_SW, 0xffff);
    const std::vector<LowOp> FnclexOps = liftX64({0xdb, 0xe2});
    ASSERT_FALSE(FnclexOps.empty());
    ASSERT_EQ(Emulator.run(FnclexOps), FnclexOps.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::FPU_SW), UINT64_C(0x7f00));

    // 5 / 3 distinguishes truncating FPREM (+2) from nearest-even FPREM1
    // (-1), and also distinguishes their quotient condition-code encodings.
    Emulator.reset();
    Emulator.setRegisterBytes(
        x86reg::ST0, x87Value(UINT64_C(0xa000000000000000), 0x4001));
    Emulator.setRegisterBytes(
        x86reg::ST1, x87Value(UINT64_C(0xc000000000000000), 0x4000));
    const std::vector<LowOp> FpremOps = liftX64({0xd9, 0xf8});
    ASSERT_FALSE(FpremOps.empty());
    ASSERT_EQ(Emulator.run(FpremOps), FpremOps.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::ST0),
              x87Value(UINT64_C(0x8000000000000000), 0x4000));
    EXPECT_EQ(*Emulator.getRegister(x86reg::FPU_SW) & UINT64_C(0x4700),
              UINT64_C(0x0200));

    Emulator.reset();
    Emulator.setRegisterBytes(
        x86reg::ST0, x87Value(UINT64_C(0xa000000000000000), 0x4001));
    Emulator.setRegisterBytes(
        x86reg::ST1, x87Value(UINT64_C(0xc000000000000000), 0x4000));
    const std::vector<LowOp> Fprem1Ops = liftX64({0xd9, 0xf5});
    ASSERT_FALSE(Fprem1Ops.empty());
    ASSERT_EQ(Emulator.run(Fprem1Ops), Fprem1Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::ST0),
              x87Value(UINT64_C(0x8000000000000000), 0xbfff));
    EXPECT_EQ(*Emulator.getRegister(x86reg::FPU_SW) & UINT64_C(0x4700),
              UINT64_C(0x4000));

    // A large exponent delta performs a deterministic partial reduction and
    // reports C2=1.  X87ReadStatus must return that exact architectural word.
    Emulator.reset();
    Emulator.setRegisterBytes(
        x86reg::ST0, x87Value(UINT64_C(0x8000000000000001), 0x7ffe));
    Emulator.setRegisterBytes(
        x86reg::ST1, x87Value(UINT64_C(0x8000000000000003), 0xffbe));
    ASSERT_EQ(Emulator.run(FpremOps), FpremOps.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::ST0),
              x87Value(UINT64_C(0xc000000000000000), 0x7f82));
    EXPECT_EQ(*Emulator.getRegister(x86reg::FPU_SW) & UINT64_C(0x4700),
              UINT64_C(0x0400));
    LowOp ReadStatus = intrinsic(Intrinsic::X87ReadStatus);
    ReadStatus.Output = NdVar::tmp(0x72000000, 2);
    ASSERT_TRUE(Emulator.step(ReadStatus));
    EXPECT_EQ(Emulator.getRegister(0x72000000), UINT64_C(0x0400));

    // Unsupported pseudo-denormals and a zero divisor are rejected before
    // either ST0 or the saved status word is modified.
    const auto Invalid = x87Value(UINT64_C(0x4000000000000000), 0x3fff);
    const auto Zero = x87Value(0, 0);
    Emulator.reset();
    Emulator.setRegisterBytes(x86reg::ST0, Invalid);
    Emulator.setRegisterBytes(x86reg::ST1, Zero);
    Emulator.setRegister(x86reg::FPU_SW, 0x4200);
    EXPECT_FALSE(Emulator.step(remainder(Intrinsic::X87Fprem)));
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::ST0), Invalid);
    EXPECT_EQ(Emulator.getRegister(x86reg::FPU_SW), UINT64_C(0x4200));
    EXPECT_FALSE(Emulator.skips().any());
  }

  // ENQCMD/ENQCMDS are one ordered system-memory transaction.  A concrete
  // emulator must be given authenticated CPL/PASID state, must read and
  // validate the whole command before checking the portal, and must return
  // retry without changing ordinary writable memory.
  {
    struct EnqueueCase {
      std::vector<uint8_t> Encoding;
      Intrinsic Id;
      uint8_t CPL;
      uint32_t IA32Pasid;
      uint32_t Header;
    };
    const std::vector<EnqueueCase> Cases = {
        {{0xf2, 0x0f, 0x38, 0xf8, 0x44, 0x8b, 0x20},
         Intrinsic::Enqcmd,
         3,
         UINT32_C(0x8000002a),
         0},
        {{0xf3, 0x0f, 0x38, 0xf8, 0x44, 0x8b, 0x20},
         Intrinsic::Enqcmds,
         0,
         0,
         UINT32_C(0x80054321)},
    };
    constexpr uint64_t SourceAddress = UINT64_C(0x2c000);
    constexpr uint64_t PortalAddress = UINT64_C(0x2d000);
    const std::vector<uint8_t> OldPortal(64, 0xa5);

    auto imageWith = [&](const std::vector<uint8_t> *Command,
                         bool WritablePortal) {
      BinaryImage Image = emptyImage();
      if (Command)
        addReadableBytes(Image, SourceAddress, Command->data(),
                         Command->size());
      if (WritablePortal) {
        Segment Portal;
        Portal.VA = PortalAddress;
        Portal.Size = OldPortal.size();
        Portal.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
        Portal.Data = OldPortal;
        Image.Segments.push_back(std::move(Portal));
      }
      return Image;
    };
    auto seedRegisters = [](NdOpEmulator &Emulator, uint64_t Portal) {
      Emulator.setRegister(x86reg::RAX, Portal);
      Emulator.setRegister(x86reg::RBX, SourceAddress - 0x20);
      Emulator.setRegister(x86reg::RCX, 0);
      Emulator.setRegister(x86reg::CF, 1);
      Emulator.setRegister(x86reg::PF, 1);
      Emulator.setRegister(x86reg::AF, 1);
      Emulator.setRegister(x86reg::ZF, 0);
      Emulator.setRegister(x86reg::SF, 1);
      Emulator.setRegister(x86reg::OF, 1);
    };
    auto expectUnchangedFlags = [](const NdOpEmulator &Emulator) {
      EXPECT_EQ(Emulator.getRegister(x86reg::CF), 1U);
      EXPECT_EQ(Emulator.getRegister(x86reg::PF), 1U);
      EXPECT_EQ(Emulator.getRegister(x86reg::AF), 1U);
      EXPECT_EQ(Emulator.getRegister(x86reg::ZF), 0U);
      EXPECT_EQ(Emulator.getRegister(x86reg::SF), 1U);
      EXPECT_EQ(Emulator.getRegister(x86reg::OF), 1U);
    };

    for (const EnqueueCase &Test : Cases) {
      SCOPED_TRACE(static_cast<unsigned>(Test.Id));
      std::vector<uint8_t> Command(64);
      for (unsigned Index = 4; Index < Command.size(); ++Index)
        Command[Index] = static_cast<uint8_t>(0x31 + Index);
      std::memcpy(Command.data(), &Test.Header, sizeof(Test.Header));
      const std::vector<LowOp> Ops = liftX64(Test.Encoding);
      ASSERT_FALSE(Ops.empty());
      const LowOp *Effect = nullptr;
      for (const LowOp &Op : Ops)
        if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 3 &&
            Op.Inputs[0].isConst() &&
            Op.Inputs[0].Offset == static_cast<uint64_t>(Test.Id)) {
          Effect = &Op;
          break;
        }
      ASSERT_NE(Effect, nullptr);
      EXPECT_EQ(Effect->Output, NdVar::reg(x86reg::ZF, 1));
      EXPECT_EQ(Effect->Inputs[1].Size, 8U);
      EXPECT_EQ(Effect->Inputs[2].Size, 8U);
      EXPECT_EQ(std::count_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
                  return Op.Opcode == NdOp::LOAD;
                }),
                0);

      BinaryImage Image = imageWith(&Command, true);
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setLoadCollect(true);
      ASSERT_TRUE(Emulator.setX86EnqueueContext(Test.CPL, Test.IA32Pasid, 48));
      seedRegisters(Emulator, PortalAddress);
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      ASSERT_EQ(Emulator.getLoadRecords().size(), 1U);
      EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, SourceAddress);
      EXPECT_EQ(Emulator.getLoadRecords()[0].Size, 64U);
      EXPECT_EQ(Emulator.getRegister(x86reg::ZF), 1U);
      EXPECT_EQ(Emulator.getRegister(x86reg::CF), 0U);
      EXPECT_EQ(Emulator.getRegister(x86reg::PF), 0U);
      EXPECT_EQ(Emulator.getRegister(x86reg::AF), 0U);
      EXPECT_EQ(Emulator.getRegister(x86reg::SF), 0U);
      EXPECT_EQ(Emulator.getRegister(x86reg::OF), 0U);
      Emulator.setLoadCollect(false);
      for (uint64_t Offset = 0; Offset != 64; Offset += 8) {
        LowOp Load;
        Load.Opcode = NdOp::LOAD;
        Load.Output = NdVar::tmp(UINT64_C(0x73000000) + Offset, 8);
        Load.addInput(NdVar::cst(PortalAddress + Offset, 8));
        ASSERT_TRUE(Emulator.step(Load));
        EXPECT_EQ(Emulator.getRegister(UINT64_C(0x73000000) + Offset),
                  UINT64_C(0xa5a5a5a5a5a5a5a5));
      }
      EXPECT_FALSE(Emulator.skips().any());

      // Missing/unauthorized context stops before the source read and leaves
      // every status flag untouched.
      NdOpEmulator Unconfigured(Image);
      Unconfigured.setStrictMode(true);
      Unconfigured.setLoadCollect(true);
      seedRegisters(Unconfigured, PortalAddress);
      EXPECT_LT(Unconfigured.run(Ops), Ops.size());
      EXPECT_TRUE(Unconfigured.getLoadRecords().empty());
      expectUnchangedFlags(Unconfigured);
      EXPECT_FALSE(Unconfigured.skips().any());

      NdOpEmulator Unauthorized(Image);
      Unauthorized.setStrictMode(true);
      Unauthorized.setLoadCollect(true);
      ASSERT_TRUE(Unauthorized.setX86EnqueueContext(
          Test.Id == Intrinsic::Enqcmds ? 3 : Test.CPL,
          Test.Id == Intrinsic::Enqcmd ? 0 : Test.IA32Pasid, 48));
      seedRegisters(Unauthorized, PortalAddress);
      EXPECT_LT(Unauthorized.run(Ops), Ops.size());
      EXPECT_TRUE(Unauthorized.getLoadRecords().empty());
      expectUnchangedFlags(Unauthorized);
      EXPECT_FALSE(Unauthorized.skips().any());
    }

    const std::vector<uint8_t> Enqcmd = {0xf2, 0x0f, 0x38, 0xf8,
                                         0x44, 0x8b, 0x20};
    const std::vector<LowOp> Ops = liftX64(Enqcmd);
    ASSERT_FALSE(Ops.empty());
    std::vector<uint8_t> Command(64, 0x5a);
    std::fill_n(Command.begin(), 4, 0);

    // With valid PASID state, a missing source fails before any command load
    // is recorded and before portal/status state is touched.
    BinaryImage MissingSourceImage = imageWith(nullptr, true);
    NdOpEmulator MissingSource(MissingSourceImage);
    MissingSource.setStrictMode(true);
    MissingSource.setLoadCollect(true);
    ASSERT_TRUE(MissingSource.setX86EnqueueContext(
        3, UINT32_C(0x8000002a), 48));
    seedRegisters(MissingSource, PortalAddress);
    EXPECT_LT(MissingSource.run(Ops), Ops.size());
    EXPECT_TRUE(MissingSource.getLoadRecords().empty());
    expectUnchangedFlags(MissingSource);

    // Header and destination faults occur after the complete source read but
    // still before flags or ordinary destination memory can change.
    std::vector<uint8_t> InvalidCommand = Command;
    InvalidCommand[0] = 1;
    BinaryImage InvalidHeaderImage = imageWith(&InvalidCommand, true);
    NdOpEmulator InvalidHeader(InvalidHeaderImage);
    InvalidHeader.setStrictMode(true);
    InvalidHeader.setLoadCollect(true);
    ASSERT_TRUE(InvalidHeader.setX86EnqueueContext(
        3, UINT32_C(0x8000002a), 48));
    seedRegisters(InvalidHeader, PortalAddress);
    EXPECT_LT(InvalidHeader.run(Ops), Ops.size());
    ASSERT_EQ(InvalidHeader.getLoadRecords().size(), 1U);
    EXPECT_EQ(InvalidHeader.getLoadRecords()[0].Addr, SourceAddress);
    expectUnchangedFlags(InvalidHeader);

    BinaryImage MisalignedImage = imageWith(&Command, true);
    NdOpEmulator Misaligned(MisalignedImage);
    Misaligned.setStrictMode(true);
    Misaligned.setLoadCollect(true);
    ASSERT_TRUE(Misaligned.setX86EnqueueContext(
        3, UINT32_C(0x8000002a), 48));
    seedRegisters(Misaligned, PortalAddress + 1);
    EXPECT_LT(Misaligned.run(Ops), Ops.size());
    ASSERT_EQ(Misaligned.getLoadRecords().size(), 1U);
    expectUnchangedFlags(Misaligned);

    BinaryImage UnmappedPortalImage = imageWith(&Command, false);
    NdOpEmulator UnmappedPortal(UnmappedPortalImage);
    UnmappedPortal.setStrictMode(true);
    UnmappedPortal.setLoadCollect(true);
    ASSERT_TRUE(UnmappedPortal.setX86EnqueueContext(
        3, UINT32_C(0x8000002a), 48));
    seedRegisters(UnmappedPortal, PortalAddress);
    EXPECT_LT(UnmappedPortal.run(Ops), Ops.size());
    ASSERT_EQ(UnmappedPortal.getLoadRecords().size(), 1U);
    expectUnchangedFlags(UnmappedPortal);
    EXPECT_FALSE(UnmappedPortal.skips().any());
  }

  expectPackedConvertControlAndMemoryForms();
  expectGenericRex2DecodeLiftAndEmulate();
  expectExplicitMsrInvalidateAndHighCFailClosed();
}

TEST(X86APXEVEXExistingGpr,
     ExpandUsesExtendedAddressAndCompressedDisplacementExactly) {
  // vexpandps zmm1 {k2}{z}, [r29 + r30*2 + 64]
  const std::vector<uint8_t> Encoding = {0x62, 0x9a, 0x79, 0xca,
                                         0x88, 0x4c, 0x75, 0x10};
  constexpr uint64_t Base = UINT64_C(0xf000);
  constexpr uint64_t Index = UINT64_C(0x20);
  constexpr uint64_t Address = Base + Index * 2 + 64;
  constexpr uint64_t Mask = UINT64_C(0x25);
  const std::vector<uint32_t> Packed = {
      UINT32_C(0x3f800000), UINT32_C(0x40000000), UINT32_C(0x40400000)};
  std::vector<uint32_t> Expected(16, 0);
  Expected[0] = Packed[0];
  Expected[2] = Packed[1];
  Expected[5] = Packed[2];

  expectMemoryDetail(Encoding, X86_REG_R29, X86_REG_R30, 2, 64);
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());
  BinaryImage Image = emptyImage();
  addReadableBytes(Image, Address, Packed.data(),
                   Packed.size() * sizeof(uint32_t));
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::R29, Base);
  Emulator.setRegister(x86reg::R30, Index);
  Emulator.setRegister(x86reg::K2, Mask);
  Emulator.setRegisterBytes(x86reg::XMM1, std::vector<uint8_t>(64, 0xa5));

  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
  ASSERT_EQ(Emulator.getLoadRecords().size(), Packed.size());
  for (size_t Element = 0; Element < Packed.size(); ++Element) {
    EXPECT_EQ(Emulator.getLoadRecords()[Element].Addr,
              Address + Element * sizeof(uint32_t));
    EXPECT_EQ(Emulator.getLoadRecords()[Element].Size, sizeof(uint32_t));
  }
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXEVEXExistingGpr,
     TernaryLogicUsesExtendedAddressAndMaskedMemoryExactly) {
  // vpternlogd zmm1 {k3}, zmm2, [r29 + r30*2 + 64], 0x96
  const std::vector<uint8_t> Encoding = {0x62, 0x9b, 0x69, 0x4b, 0x25,
                                         0x4c, 0x75, 0x01, 0x96};
  constexpr uint64_t Base = UINT64_C(0x11000);
  constexpr uint64_t Index = UINT64_C(0x20);
  constexpr uint64_t Address = Base + Index * 2 + 64;
  constexpr uint64_t Mask = UINT64_C(0xa55a);
  std::vector<uint32_t> OldDestination(16), FirstSource(16), Memory(16);
  std::vector<uint32_t> Expected(16);
  for (unsigned Lane = 0; Lane < Expected.size(); ++Lane) {
    OldDestination[Lane] = UINT32_C(0x01020304) + Lane;
    FirstSource[Lane] = UINT32_C(0xf0f00ff0) + Lane * 3;
    Memory[Lane] = UINT32_C(0x0ff0f00f) + Lane * 5;
    Expected[Lane] =
        (Mask & (UINT64_C(1) << Lane)) != 0
            ? OldDestination[Lane] ^ FirstSource[Lane] ^ Memory[Lane]
            : OldDestination[Lane];
  }

  expectMemoryDetail(Encoding, X86_REG_R29, X86_REG_R30, 2, 64);
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());
  BinaryImage Image = emptyImage();
  addReadableBytes(Image, Address, Memory.data(),
                   Memory.size() * sizeof(uint32_t));
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::R29, Base);
  Emulator.setRegister(x86reg::R30, Index);
  Emulator.setRegister(x86reg::K3, Mask);
  Emulator.setRegisterBytes(x86reg::XMM1, bytes(OldDestination));
  Emulator.setRegisterBytes(x86reg::XMM2, bytes(FirstSource));

  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
  EXPECT_EQ(Emulator.getLoadRecords().size(), bitCount(Mask));
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXEVEXExistingGpr,
     UnusedAddressExtensionsDoNotChangeMaskConversionOperands) {
  // vpmovm2d zmm29, k7, with the otherwise-unused B4 and U bits set.
  const std::vector<uint8_t> Encoding = {0x62, 0x6a, 0x7a, 0x48, 0x38, 0xef};
  constexpr uint64_t Mask = UINT64_C(0xa55a);
  std::vector<uint32_t> Expected(16);
  for (unsigned Lane = 0; Lane < Expected.size(); ++Lane)
    Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0 ? UINT32_MAX : 0;

  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());
  BinaryImage Image = emptyImage();
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::K7, Mask);
  Emulator.setRegisterBytes(x86reg::XMM29, std::vector<uint8_t>(64, 0xa5));
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM29), bytes(Expected));
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXEVEXExistingGpr,
     UnusedAddressExtensionsDoNotChangeVnniVectorOperands) {
  // vpdpbusd zmm0 {k1}{z}, zmm2, zmm3, with unused B4 and U set.
  const std::vector<uint8_t> Encoding = {0x62, 0xfa, 0x69, 0xc9, 0x50, 0xc3};
  std::vector<uint32_t> OldDestination(16), Expected(16);
  for (unsigned Lane = 0; Lane < Expected.size(); ++Lane) {
    OldDestination[Lane] = UINT32_C(0x1000) + Lane;
    Expected[Lane] = OldDestination[Lane] + 24;
  }

  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());
  BinaryImage Image = emptyImage();
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::K1, UINT64_C(0xffff));
  Emulator.setRegisterBytes(x86reg::XMM0, bytes(OldDestination));
  Emulator.setRegisterBytes(x86reg::XMM2, std::vector<uint8_t>(64, 2));
  Emulator.setRegisterBytes(x86reg::XMM3, std::vector<uint8_t>(64, 3));
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM0), bytes(Expected));
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXEVEXExistingGpr,
     TupleBroadcastUsesExtendedAddressAndCompressedDisplacement) {
  // vbroadcastf32x2 zmm1, [r29 + r30*2 + 64]
  const std::vector<uint8_t> Encoding = {0x62, 0x9a, 0x79, 0x48,
                                         0x19, 0x4c, 0x75, 0x08};
  constexpr uint64_t Base = UINT64_C(0x13000);
  constexpr uint64_t Index = UINT64_C(0x20);
  constexpr uint64_t Address = Base + Index * 2 + 64;
  const std::vector<uint32_t> Tuple = {UINT32_C(0x3f800000),
                                       UINT32_C(0xc0200000)};
  std::vector<uint32_t> Expected(16);
  for (unsigned Lane = 0; Lane < Expected.size(); ++Lane)
    Expected[Lane] = Tuple[Lane % Tuple.size()];

  expectMemoryDetail(Encoding, X86_REG_R29, X86_REG_R30, 2, 64);
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());
  BinaryImage Image = emptyImage();
  addReadableBytes(Image, Address, Tuple.data(),
                   Tuple.size() * sizeof(uint32_t));
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::R29, Base);
  Emulator.setRegister(x86reg::R30, Index);
  Emulator.setRegisterBytes(x86reg::XMM1, std::vector<uint8_t>(64, 0xa5));
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
  ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
  EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Address);
  EXPECT_EQ(Emulator.getLoadRecords()[0].Size, 8u);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXEVEXExistingGpr,
     ReciprocalApproximationUsesExtendedAddressAndMaskedLoads) {
  // vrcp14ps zmm1 {k3}, [r29 + r30*2 + 64]
  const std::vector<uint8_t> Encoding = {0x62, 0x9a, 0x79, 0x4b,
                                         0x4c, 0x4c, 0x75, 0x01};
  constexpr uint64_t Base = UINT64_C(0x15000);
  constexpr uint64_t Index = UINT64_C(0x20);
  constexpr uint64_t Address = Base + Index * 2 + 64;
  constexpr uint64_t Mask = UINT64_C(0xa55a);
  std::vector<uint32_t> Memory(16, UINT32_C(0x3f800000));
  std::vector<uint32_t> OldDestination(16), Expected(16);
  for (unsigned Lane = 0; Lane < Expected.size(); ++Lane) {
    OldDestination[Lane] = UINT32_C(0x42000000) + Lane;
    Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0 ? UINT32_C(0x3f800000)
                                                         : OldDestination[Lane];
  }

  expectMemoryDetail(Encoding, X86_REG_R29, X86_REG_R30, 2, 64);
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());
  BinaryImage Image = emptyImage();
  addReadableBytes(Image, Address, Memory.data(),
                   Memory.size() * sizeof(uint32_t));
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::R29, Base);
  Emulator.setRegister(x86reg::R30, Index);
  Emulator.setRegister(x86reg::K3, Mask);
  Emulator.setRegisterBytes(x86reg::XMM1, bytes(OldDestination));
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
  EXPECT_EQ(Emulator.getLoadRecords().size(), bitCount(Mask));
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXEVEXExistingGpr,
     FloatingClassTestUsesExtendedAddressAndMaskedLoads) {
  // vfpclassps k3 {k1}, [r29 + r30*2 + 64], 0xff
  const std::vector<uint8_t> Encoding = {0x62, 0x9b, 0x79, 0x49, 0x66,
                                         0x5c, 0x75, 0x01, 0xff};
  constexpr uint64_t Base = UINT64_C(0x17000);
  constexpr uint64_t Index = UINT64_C(0x20);
  constexpr uint64_t Address = Base + Index * 2 + 64;
  std::vector<uint32_t> Memory(16);
  uint64_t Expected = 0;
  for (unsigned Lane = 0; Lane < Memory.size(); ++Lane) {
    const bool Negative = (Lane & 1) == 0;
    Memory[Lane] = Negative ? UINT32_C(0xbf800000) : UINT32_C(0x3f800000);
    if (Negative)
      Expected |= UINT64_C(1) << Lane;
  }

  expectMemoryDetail(Encoding, X86_REG_R29, X86_REG_R30, 2, 64);
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());
  BinaryImage Image = emptyImage();
  addReadableBytes(Image, Address, Memory.data(),
                   Memory.size() * sizeof(uint32_t));
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::R29, Base);
  Emulator.setRegister(x86reg::R30, Index);
  Emulator.setRegister(x86reg::K1, UINT64_C(0xffff));
  Emulator.setRegister(x86reg::K3, UINT64_MAX);
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegister(x86reg::K3), Expected);
  EXPECT_EQ(Emulator.getLoadRecords().size(), Memory.size());
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXEVEXExistingGpr,
     FusedArithmeticSupportsExtendedFullAndBroadcastMemoryTuples) {
  struct MemoryCase {
    const char *Name;
    std::vector<uint8_t> Encoding;
    bool Broadcast;
  };
  const std::vector<MemoryCase> Cases = {
      {"full", {0x62, 0x9a, 0x69, 0x4b, 0x98, 0x44, 0x75, 0x01}, false},
      {"broadcast", {0x62, 0x9a, 0x69, 0x5b, 0x98, 0x44, 0x75, 0x10}, true},
  };
  constexpr uint64_t Base = UINT64_C(0x19000);
  constexpr uint64_t Index = UINT64_C(0x20);
  constexpr uint64_t Address = Base + Index * 2 + 64;
  constexpr uint64_t Mask = UINT64_C(0xa55a);
  std::vector<float> OldDestination(16), FirstSource(16), FullMemory(16);
  for (unsigned Lane = 0; Lane < OldDestination.size(); ++Lane) {
    OldDestination[Lane] = static_cast<float>(Lane + 2);
    FirstSource[Lane] = static_cast<float>(Lane * 3 + 1);
    FullMemory[Lane] = static_cast<float>(Lane + 5);
  }

  for (const MemoryCase &Test : Cases) {
    SCOPED_TRACE(Test.Name);
    expectMemoryDetail(Test.Encoding, X86_REG_R29, X86_REG_R30, 2, 64);
    const std::vector<LowOp> Ops = liftX64(Test.Encoding);
    ASSERT_FALSE(Ops.empty());
    const std::vector<float> Memory =
        Test.Broadcast ? std::vector<float>{5.0f} : FullMemory;
    std::vector<float> Expected = OldDestination;
    for (unsigned Lane = 0; Lane < Expected.size(); ++Lane)
      if ((Mask & (UINT64_C(1) << Lane)) != 0)
        Expected[Lane] = std::fma(OldDestination[Lane],
                                  Test.Broadcast ? Memory[0] : Memory[Lane],
                                  FirstSource[Lane]);

    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, Memory.data(),
                     Memory.size() * sizeof(float));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::R29, Base);
    Emulator.setRegister(x86reg::R30, Index);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM0, bytes(OldDestination));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(FirstSource));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM0), bytes(Expected));
    EXPECT_EQ(Emulator.getLoadRecords().size(),
              Test.Broadcast ? 1u : bitCount(Mask));
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXEVEXExistingGpr,
     UnusedAddressExtensionsDoNotChangeFusedVectorOperands) {
  // vfmadd132ps zmm0 {k3}, zmm2, zmm19, with unused B4 and U set.
  const std::vector<uint8_t> Encoding = {0x62, 0xba, 0x69, 0x4b, 0x98, 0xc3};
  constexpr uint64_t Mask = UINT64_C(0xa55a);
  std::vector<float> OldDestination(16), FirstSource(16), SecondSource(16);
  std::vector<float> Expected(16);
  for (unsigned Lane = 0; Lane < Expected.size(); ++Lane) {
    OldDestination[Lane] = static_cast<float>(Lane + 2);
    FirstSource[Lane] = static_cast<float>(Lane * 3 + 1);
    SecondSource[Lane] = static_cast<float>(Lane + 5);
    Expected[Lane] = (Mask & (UINT64_C(1) << Lane)) != 0
                         ? std::fma(OldDestination[Lane], SecondSource[Lane],
                                    FirstSource[Lane])
                         : OldDestination[Lane];
  }

  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());
  BinaryImage Image = emptyImage();
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::K3, Mask);
  Emulator.setRegisterBytes(x86reg::XMM0, bytes(OldDestination));
  Emulator.setRegisterBytes(x86reg::XMM2, bytes(FirstSource));
  Emulator.setRegisterBytes(x86reg::XMM19, bytes(SecondSource));
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM0), bytes(Expected));
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXEVEXExistingGpr,
     AlternatingFusedArithmeticUsesExtendedMaskedMemoryExactly) {
  // vfmaddsub132ps zmm0 {k3}, zmm2, [r29 + r30*2 + 64]
  const std::vector<uint8_t> Encoding = {0x62, 0x9a, 0x69, 0x4b,
                                         0x96, 0x44, 0x75, 0x01};
  constexpr uint64_t Base = UINT64_C(0x1b000);
  constexpr uint64_t Index = UINT64_C(0x20);
  constexpr uint64_t Address = Base + Index * 2 + 64;
  constexpr uint64_t Mask = UINT64_C(0xa55a);
  std::vector<float> OldDestination(16), FirstSource(16), Memory(16);
  std::vector<float> Expected(16);
  for (unsigned Lane = 0; Lane < Expected.size(); ++Lane) {
    OldDestination[Lane] = static_cast<float>(Lane + 2);
    FirstSource[Lane] = static_cast<float>(Lane * 3 + 1);
    Memory[Lane] = static_cast<float>(Lane + 5);
    if ((Mask & (UINT64_C(1) << Lane)) == 0) {
      Expected[Lane] = OldDestination[Lane];
      continue;
    }
    const float Addend =
        (Lane & 1) == 0 ? -FirstSource[Lane] : FirstSource[Lane];
    Expected[Lane] = std::fma(OldDestination[Lane], Memory[Lane], Addend);
  }

  expectMemoryDetail(Encoding, X86_REG_R29, X86_REG_R30, 2, 64);
  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());
  BinaryImage Image = emptyImage();
  addReadableBytes(Image, Address, Memory.data(),
                   Memory.size() * sizeof(float));
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::R29, Base);
  Emulator.setRegister(x86reg::R30, Index);
  Emulator.setRegister(x86reg::K3, Mask);
  Emulator.setRegisterBytes(x86reg::XMM0, bytes(OldDestination));
  Emulator.setRegisterBytes(x86reg::XMM2, bytes(FirstSource));
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM0), bytes(Expected));
  EXPECT_EQ(Emulator.getLoadRecords().size(), bitCount(Mask));
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXEVEXExistingGpr,
     FourSourceFmaUsesEverySourceAndCopiesScalarUpperLanes) {
  const std::vector<uint8_t> PackedEncoding = {0xc4, 0xe3, 0x71,
                                               0x68, 0xd3, 0x4f};
  std::vector<float> FirstSource = {1.25f, -2.0f, 3.5f, -4.25f};
  std::vector<float> SecondSource = {2.0f, 3.0f, -4.0f, -5.0f};
  std::vector<float> ThirdSource = {7.0f, -11.0f, 13.0f, -17.0f};
  std::vector<float> OldDestination = {101.0f, 102.0f, 103.0f, 104.0f};
  std::vector<float> Expected(4);
  for (unsigned Lane = 0; Lane < Expected.size(); ++Lane)
    Expected[Lane] =
        std::fma(FirstSource[Lane], SecondSource[Lane], ThirdSource[Lane]);

  const std::vector<LowOp> PackedOps = liftX64(PackedEncoding);
  ASSERT_FALSE(PackedOps.empty());
  BinaryImage PackedImage = emptyImage();
  NdOpEmulator PackedEmulator(PackedImage);
  PackedEmulator.setStrictMode(true);
  PackedEmulator.setRegisterBytes(x86reg::XMM1, bytes(FirstSource));
  PackedEmulator.setRegisterBytes(x86reg::XMM2, bytes(OldDestination));
  PackedEmulator.setRegisterBytes(x86reg::XMM3, bytes(SecondSource));
  PackedEmulator.setRegisterBytes(x86reg::XMM4, bytes(ThirdSource));
  ASSERT_EQ(PackedEmulator.run(PackedOps), PackedOps.size());
  std::vector<uint8_t> ExpectedBytes = bytes(Expected);
  ExpectedBytes.resize(64, 0);
  EXPECT_EQ(PackedEmulator.getRegisterBytes(x86reg::XMM2), ExpectedBytes);
  EXPECT_FALSE(PackedEmulator.skips().any());

  // The low is4 nibble is architecturally ignored. Scalar FMA4 copies bits
  // 127:32 from src1, not from the old independent destination.
  const std::vector<uint8_t> ScalarEncoding = {0xc4, 0xe3, 0x71,
                                               0x6a, 0xd3, 0x4f};
  Expected = FirstSource;
  Expected[0] = std::fma(FirstSource[0], SecondSource[0], ThirdSource[0]);
  const std::vector<LowOp> ScalarOps = liftX64(ScalarEncoding);
  ASSERT_FALSE(ScalarOps.empty());
  BinaryImage ScalarImage = emptyImage();
  NdOpEmulator ScalarEmulator(ScalarImage);
  ScalarEmulator.setStrictMode(true);
  ScalarEmulator.setRegisterBytes(x86reg::XMM1, bytes(FirstSource));
  ScalarEmulator.setRegisterBytes(x86reg::XMM2, bytes(OldDestination));
  ScalarEmulator.setRegisterBytes(x86reg::XMM3, bytes(SecondSource));
  ScalarEmulator.setRegisterBytes(x86reg::XMM4, bytes(ThirdSource));
  ASSERT_EQ(ScalarEmulator.run(ScalarOps), ScalarOps.size());
  ExpectedBytes = bytes(Expected);
  ExpectedBytes.resize(64, 0);
  EXPECT_EQ(ScalarEmulator.getRegisterBytes(x86reg::XMM2), ExpectedBytes);
  EXPECT_FALSE(ScalarEmulator.skips().any());
}

TEST(X86APXEVEXExistingGpr,
     FourSourceFmaSupportsBothArchitecturalMemoryPositions) {
  struct MemoryCase {
    std::vector<uint8_t> Encoding;
    bool MemoryIsSecondSource;
  };
  const std::vector<MemoryCase> Cases = {
      {{0x64, 0xc4, 0xa3, 0x75, 0x68, 0x44, 0xb5, 0x20, 0x70}, true},
      {{0x64, 0xc4, 0xa3, 0xf5, 0x68, 0x44, 0xb5, 0x20, 0x70}, false},
  };
  constexpr uint64_t SegmentBase = UINT64_C(0x20000);
  constexpr uint64_t Base = UINT64_C(0x100);
  constexpr uint64_t Index = UINT64_C(4);
  constexpr uint64_t Address = SegmentBase + Base + Index * 4 + 32;
  std::vector<float> FirstSource(8), RegisterSource(8), MemorySource(8);
  for (unsigned Lane = 0; Lane < FirstSource.size(); ++Lane) {
    FirstSource[Lane] = static_cast<float>(Lane + 1);
    RegisterSource[Lane] = static_cast<float>(Lane * 2 + 3);
    MemorySource[Lane] = static_cast<float>(20 - static_cast<int>(Lane));
  }

  for (const MemoryCase &Test : Cases) {
    SCOPED_TRACE(Test.MemoryIsSecondSource ? "src2-memory" : "src3-memory");
    expectMemoryDetail(Test.Encoding, X86_REG_RBP, X86_REG_R14, 4, 32);
    const std::vector<LowOp> Ops = liftX64(Test.Encoding);
    ASSERT_FALSE(Ops.empty());
    std::vector<float> Expected(8);
    for (unsigned Lane = 0; Lane < Expected.size(); ++Lane) {
      const float Second =
          Test.MemoryIsSecondSource ? MemorySource[Lane] : RegisterSource[Lane];
      const float Third =
          Test.MemoryIsSecondSource ? RegisterSource[Lane] : MemorySource[Lane];
      Expected[Lane] = std::fma(FirstSource[Lane], Second, Third);
    }

    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, MemorySource.data(),
                     MemorySource.size() * sizeof(float));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS,
                                       SegmentBase);
    Emulator.setRegister(x86reg::RBP, Base);
    Emulator.setRegister(x86reg::R14, Index);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(FirstSource));
    Emulator.setRegisterBytes(x86reg::XMM7, bytes(RegisterSource));
    Emulator.setRegisterBytes(x86reg::XMM0, std::vector<uint8_t>(32, 0xcc));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    std::vector<uint8_t> ExpectedBytes = bytes(Expected);
    ExpectedBytes.resize(64, 0);
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM0), ExpectedBytes);
    ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Address);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Size, 32u);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXEVEXExistingGpr,
     FourSourceAlternatingFmaUsesOneFusedOperationPerLane) {
  struct AlternatingCase {
    uint8_t Opcode;
    bool SubtractEven;
  };
  const std::vector<AlternatingCase> Cases = {
      {0x5c, true},
      {0x5e, false},
  };
  std::vector<float> FirstSource(8), SecondSource(8), ThirdSource(8);
  for (unsigned Lane = 0; Lane < FirstSource.size(); ++Lane) {
    FirstSource[Lane] = static_cast<float>(Lane + 1);
    SecondSource[Lane] = static_cast<float>(Lane + 3);
    ThirdSource[Lane] = static_cast<float>(Lane * 2 + 1);
  }

  for (const AlternatingCase &Test : Cases) {
    SCOPED_TRACE(Test.SubtractEven ? "addsub" : "subadd");
    const std::vector<uint8_t> Encoding = {0xc4,        0xe3, 0x75,
                                           Test.Opcode, 0xd3, 0x40};
    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    std::vector<float> Expected(8);
    for (unsigned Lane = 0; Lane < Expected.size(); ++Lane) {
      const bool Subtract = ((Lane & 1) == 0) == Test.SubtractEven;
      Expected[Lane] =
          std::fma(FirstSource[Lane], SecondSource[Lane],
                   Subtract ? -ThirdSource[Lane] : ThirdSource[Lane]);
    }

    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(FirstSource));
    Emulator.setRegisterBytes(x86reg::XMM2, std::vector<uint8_t>(32, 0xcc));
    Emulator.setRegisterBytes(x86reg::XMM3, bytes(SecondSource));
    Emulator.setRegisterBytes(x86reg::XMM4, bytes(ThirdSource));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    std::vector<uint8_t> ExpectedBytes = bytes(Expected);
    ExpectedBytes.resize(64, 0);
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM2), ExpectedBytes);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXEVEXExistingGpr,
     MaskedLaneMemoryFormsSuppressInactiveElementAccesses) {
  constexpr uint64_t Address = UINT64_C(0x24000);

  // vbroadcastf32x4 zmm1 {k3}, [rax]. Only tuple elements referenced by an
  // active repeated destination lane may be read.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf2, 0x7d, 0x4b, 0x1a, 0x08};
    const uint64_t Mask =
        (UINT64_C(1) << 0) | (UINT64_C(1) << 5) | (UINT64_C(1) << 10);
    const std::vector<uint32_t> Memory = {101, 202, 303};
    std::vector<uint32_t> OldDestination(16), Expected(16);
    for (unsigned Lane = 0; Lane < OldDestination.size(); ++Lane)
      OldDestination[Lane] = 1000 + Lane;
    Expected = OldDestination;
    Expected[0] = Memory[0];
    Expected[5] = Memory[1];
    Expected[10] = Memory[2];

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, Memory.data(),
                     Memory.size() * sizeof(uint32_t));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(OldDestination));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    ASSERT_EQ(Emulator.getLoadRecords().size(), 3u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Address);
    EXPECT_EQ(Emulator.getLoadRecords()[1].Addr, Address + 4);
    EXPECT_EQ(Emulator.getLoadRecords()[2].Addr, Address + 8);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vinsertf32x4 zmm1 {k3}, zmm2, [rax], 3. The selected destination lane
  // maps mask bits 12..15 back to memory tuple elements 0..3.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0x6d, 0x4b,
                                           0x18, 0x08, 0x03};
    const uint64_t Mask =
        (UINT64_C(1) << 0) | (UINT64_C(1) << 12) | (UINT64_C(1) << 14);
    const std::vector<uint32_t> Memory = {111, 222, 333};
    std::vector<uint32_t> OldDestination(16), BaseSource(16), Expected(16);
    for (unsigned Lane = 0; Lane < Expected.size(); ++Lane) {
      OldDestination[Lane] = 2000 + Lane;
      BaseSource[Lane] = 3000 + Lane;
    }
    Expected = OldDestination;
    Expected[0] = BaseSource[0];
    Expected[12] = Memory[0];
    Expected[14] = Memory[2];

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Address, Memory.data(),
                     Memory.size() * sizeof(uint32_t));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM1, bytes(OldDestination));
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(BaseSource));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM1), bytes(Expected));
    ASSERT_EQ(Emulator.getLoadRecords().size(), 2u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Address);
    EXPECT_EQ(Emulator.getLoadRecords()[1].Addr, Address + 8);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vextractf32x4 [rax] {k3}, zmm2, 2. Inactive destination elements retain
  // their prior memory bytes while active elements store the selected tuple.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0xf3, 0x7d, 0x4b,
                                           0x19, 0x10, 0x02};
    constexpr uint64_t Mask = (UINT64_C(1) << 0) | (UINT64_C(1) << 2);
    std::vector<uint32_t> Source(16);
    for (unsigned Lane = 0; Lane < Source.size(); ++Lane)
      Source[Lane] = 4000 + Lane;
    const std::vector<uint32_t> Initial = {11, 22, 33, 44};
    const std::vector<uint32_t> Expected = {Source[8], Initial[1], Source[10],
                                            Initial[3]};

    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = emptyImage();
    Segment Memory;
    Memory.VA = Address;
    Memory.Size = Initial.size() * sizeof(uint32_t);
    Memory.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Memory.Data = bytes(Initial);
    Image.Segments.push_back(std::move(Memory));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::RAX, Address);
    Emulator.setRegister(x86reg::K3, Mask);
    Emulator.setRegisterBytes(x86reg::XMM2, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());

    std::vector<uint8_t> Actual(sizeof(uint32_t) * Initial.size());
    for (unsigned Offset = 0; Offset < Actual.size(); Offset += 8) {
      LowOp Load;
      Load.Opcode = NdOp::LOAD;
      Load.Output = NdVar::tmp(UINT64_C(0x7f000000) + Offset, 8);
      Load.addInput(NdVar::cst(0, 8));
      Load.addInput(NdVar::cst(Address + Offset, 8));
      ASSERT_TRUE(Emulator.step(Load));
      const std::optional<uint64_t> Value =
          Emulator.getRegister(UINT64_C(0x7f000000) + Offset);
      ASSERT_TRUE(Value);
      std::memcpy(Actual.data() + Offset, &*Value, 8);
    }
    EXPECT_EQ(Actual, bytes(Expected));
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXEVEXExistingGpr,
     RawAndStructuredRegisterMismatchesFailBeforeAnyLowIrIsEmitted) {
  const std::vector<uint8_t> MemoryEncoding = {0x62, 0x99, 0x60, 0x4d,
                                               0x58, 0x54, 0x75, 0x01};
  expectMutatedLiftFailsClosed(MemoryEncoding, [](cs_insn &, cs_x86 &X86) {
    for (unsigned Index = 0; Index < X86.op_count; ++Index) {
      if (X86.operands[Index].type != X86_OP_MEM)
        continue;
      X86.operands[Index].mem.base = X86_REG_R28;
      return true;
    }
    return false;
  });
  expectMutatedLiftFailsClosed(MemoryEncoding, [](cs_insn &Insn, cs_x86 &) {
    Insn.bytes[1] ^= 0x08;
    return true;
  });

  const std::vector<uint8_t> RegisterEncoding = {0x62, 0xd9, 0x6e,
                                                 0x08, 0x2a, 0xcd};
  expectMutatedLiftFailsClosed(RegisterEncoding, [](cs_insn &, cs_x86 &X86) {
    if (X86.op_count != 3)
      return false;
    X86.operands[2].reg = X86_REG_R28D;
    return true;
  });

  const std::vector<uint8_t> VsibEncoding = {0x62, 0x9a, 0x79, 0x42,
                                             0x90, 0x4c, 0xb5, 0x00};
  expectMutatedLiftFailsClosed(VsibEncoding, [](cs_insn &, cs_x86 &X86) {
    for (unsigned Index = 0; Index < X86.op_count; ++Index) {
      if (X86.operands[Index].type != X86_OP_MEM)
        continue;
      X86.operands[Index].mem.index = X86_REG_ZMM29;
      X86.sib_index = X86_REG_ZMM29;
      return true;
    }
    return false;
  });

  const std::vector<uint8_t> MaskedMoveEncoding = {0x62, 0x99, 0x7a, 0xcb,
                                                   0x6f, 0x4c, 0xb5, 0x01};
  expectMutatedLiftFailsClosed(MaskedMoveEncoding, [](cs_insn &, cs_x86 &X86) {
    for (unsigned Index = 0; Index < X86.op_count; ++Index) {
      if (X86.operands[Index].type != X86_OP_MEM)
        continue;
      X86.operands[Index].mem.base = X86_REG_R28;
      return true;
    }
    return false;
  });

  const std::vector<uint8_t> ExpandEncoding = {0x62, 0x9a, 0x79, 0xca,
                                               0x88, 0x4c, 0x75, 0x10};
  expectMutatedLiftFailsClosed(ExpandEncoding, [](cs_insn &, cs_x86 &X86) {
    for (unsigned Index = 0; Index < X86.op_count; ++Index) {
      if (X86.operands[Index].type != X86_OP_MEM)
        continue;
      X86.operands[Index].mem.base = X86_REG_R28;
      return true;
    }
    return false;
  });

  const std::vector<uint8_t> TernaryEncoding = {0x62, 0x9b, 0x69, 0x4b, 0x25,
                                                0x4c, 0x75, 0x01, 0x96};
  expectMutatedLiftFailsClosed(TernaryEncoding, [](cs_insn &, cs_x86 &X86) {
    for (unsigned Index = 0; Index < X86.op_count; ++Index) {
      if (X86.operands[Index].type != X86_OP_MEM)
        continue;
      X86.operands[Index].mem.index = X86_REG_R28;
      X86.sib_index = X86_REG_R28;
      return true;
    }
    return false;
  });

  const std::vector<uint8_t> MaskConversionEncoding = {0x62, 0x6a, 0x7a,
                                                       0x48, 0x38, 0xef};
  expectMutatedLiftFailsClosed(MaskConversionEncoding,
                               [](cs_insn &, cs_x86 &X86) {
                                 if (X86.op_count != 2)
                                   return false;
                                 X86.operands[0].reg = X86_REG_ZMM28;
                                 return true;
                               });

  const std::vector<uint8_t> VnniEncoding = {0x62, 0xfa, 0x69,
                                             0xc9, 0x50, 0xc3};
  expectMutatedLiftFailsClosed(VnniEncoding, [](cs_insn &, cs_x86 &X86) {
    if (X86.op_count != 4)
      return false;
    X86.operands[3].reg = X86_REG_ZMM4;
    return true;
  });

  const std::vector<std::vector<uint8_t>> FloatMemoryEncodings = {
      {0x62, 0x9a, 0x79, 0x48, 0x19, 0x4c, 0x75, 0x08},
      {0x62, 0x9a, 0x79, 0x4b, 0x4c, 0x4c, 0x75, 0x01},
      {0x62, 0x9b, 0x79, 0x49, 0x66, 0x5c, 0x75, 0x01, 0xff},
  };
  for (const std::vector<uint8_t> &Encoding : FloatMemoryEncodings) {
    expectMutatedLiftFailsClosed(Encoding, [](cs_insn &, cs_x86 &X86) {
      for (unsigned Index = 0; Index < X86.op_count; ++Index) {
        if (X86.operands[Index].type != X86_OP_MEM)
          continue;
        X86.operands[Index].mem.base = X86_REG_R28;
        return true;
      }
      return false;
    });
  }

  const std::vector<uint8_t> FmaMemoryEncoding = {0x62, 0x9a, 0x69, 0x4b,
                                                  0x98, 0x44, 0x75, 0x01};
  expectMutatedLiftFailsClosed(FmaMemoryEncoding, [](cs_insn &, cs_x86 &X86) {
    for (unsigned Index = 0; Index < X86.op_count; ++Index) {
      if (X86.operands[Index].type != X86_OP_MEM)
        continue;
      X86.operands[Index].mem.index = X86_REG_R28;
      X86.sib_index = X86_REG_R28;
      return true;
    }
    return false;
  });

  const std::vector<uint8_t> FmaRegisterEncoding = {0x62, 0xba, 0x69,
                                                    0x4b, 0x98, 0xc3};
  expectMutatedLiftFailsClosed(FmaRegisterEncoding, [](cs_insn &, cs_x86 &X86) {
    if (X86.op_count != 4)
      return false;
    X86.operands[3].reg = X86_REG_ZMM18;
    return true;
  });

  const std::vector<uint8_t> AlternatingFmaEncoding = {0x62, 0x9a, 0x69, 0x4b,
                                                       0x96, 0x44, 0x75, 0x01};
  expectMutatedLiftFailsClosed(
      AlternatingFmaEncoding, [](cs_insn &, cs_x86 &X86) {
        for (unsigned Index = 0; Index < X86.op_count; ++Index) {
          if (X86.operands[Index].type != X86_OP_MEM)
            continue;
          X86.operands[Index].mem.base = X86_REG_R28;
          return true;
        }
        return false;
      });

  const std::vector<uint8_t> Fma4RegisterEncoding = {0xc4, 0xe3, 0x71,
                                                     0x68, 0xd3, 0x4f};
  expectMutatedLiftFailsClosed(Fma4RegisterEncoding,
                               [](cs_insn &, cs_x86 &X86) {
                                 if (X86.op_count != 4)
                                   return false;
                                 X86.operands[3].reg = X86_REG_XMM5;
                                 return true;
                               });
  expectMutatedLiftFailsClosed(Fma4RegisterEncoding,
                               [](cs_insn &Insn, cs_x86 &) {
                                 Insn.bytes[Insn.size - 1] ^= 0x10;
                                 return true;
                               });

  const std::vector<uint8_t> Fma4MemoryEncoding = {0x64, 0xc4, 0xa3, 0x75, 0x68,
                                                   0x44, 0xb5, 0x20, 0x70};
  expectMutatedLiftFailsClosed(Fma4MemoryEncoding, [](cs_insn &, cs_x86 &X86) {
    for (unsigned Index = 0; Index < X86.op_count; ++Index) {
      if (X86.operands[Index].type != X86_OP_MEM)
        continue;
      X86.operands[Index].mem.index = X86_REG_R13;
      X86.sib_index = X86_REG_R13;
      return true;
    }
    return false;
  });
}

void expectPackedConvertControlAndMemoryForms() {
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM30);
  const RegInfo SourceZmm = mapCapstoneReg(X86_REG_ZMM29);
  const RegInfo SourceYmm = mapCapstoneReg(X86_REG_YMM29);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K7);
  ASSERT_NE(Destination.Offset, UINT64_C(0xffff));
  ASSERT_NE(SourceZmm.Offset, UINT64_C(0xffff));
  ASSERT_NE(SourceYmm.Offset, UINT64_C(0xffff));
  ASSERT_NE(WriteMask.Offset, UINT64_C(0xffff));

  // vcvtuqq2pd zmm30 {k7}{z}, zmm29, {ru-sae}.  Embedded rounding must
  // override MXCSR and SAE must suppress the precision status update.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0x01, 0xfe,
                                           0xdf, 0x7a, 0xf5};
    const std::vector<uint64_t> Source(8, UINT64_C(0x0020000000000001));
    const std::vector<uint64_t> Expected(8, UINT64_C(0x4340000000000001));
    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());

    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x3f00); // Round down; overridden by embedded rounding.
    Emulator.setRegister(WriteMask.Offset, UINT64_C(0xff));
    Emulator.setRegisterBytes(SourceZmm.Offset, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x3f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // vcvttps2uqq zmm30 {k7}{z}, ymm29, {sae}.  Truncation remains fixed
  // toward zero, invalid lanes use the unsigned indefinite value, and SAE
  // hides both invalid and precision status.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0x01, 0x7d,
                                           0x9f, 0x78, 0xf5};
    const std::vector<uint32_t> Source = {
        UINT32_C(0x40700000), UINT32_C(0xbf000000),
        UINT32_C(0x7f800000), UINT32_C(0x7f800001), 0, 0, 0, 0};
    const std::vector<uint64_t> Expected = {
        3, 0, UINT64_MAX, UINT64_MAX, 0, 0, 0, 0};
    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());

    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f00); // Invalid is unmasked, but SAE suppresses it.
    Emulator.setRegister(WriteMask.Offset, UINT64_C(0x0f));
    Emulator.setRegisterBytes(SourceYmm.Offset, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), bytes(Expected));
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f00U);
    EXPECT_FALSE(Emulator.skips().any());
  }

  // Full-tuple and broadcast memory forms share the same lane conversion,
  // while the broadcast form performs only one physical memory read.
  {
    struct MemoryCase {
      std::vector<uint8_t> Encoding;
      bool Broadcast;
    };
    const std::vector<MemoryCase> Cases = {
        {{0x62, 0x61, 0xfd, 0xcf, 0x78, 0x30}, false},
        {{0x62, 0x61, 0xfd, 0xdf, 0x78, 0x30}, true},
    };
    constexpr uint64_t Address = UINT64_C(0x31000);
    const std::vector<double> FullSource = {1.75, 2.75, 3.75, 4.75,
                                            5.75, 6.75, 7.75, 8.75};
    const double BroadcastSource = 9.75;

    for (const MemoryCase &Test : Cases) {
      SCOPED_TRACE(Test.Broadcast ? "broadcast" : "full tuple");
      std::vector<uint64_t> Expected(8);
      for (unsigned Lane = 0; Lane < Expected.size(); ++Lane)
        Expected[Lane] =
            Test.Broadcast ? 9 : static_cast<uint64_t>(Lane + 1);

      const std::vector<LowOp> Ops = liftX64(Test.Encoding);
      ASSERT_FALSE(Ops.empty());
      BinaryImage Image = emptyImage();
      if (Test.Broadcast)
        addReadableBytes(Image, Address, &BroadcastSource,
                         sizeof(BroadcastSource));
      else
        addReadableBytes(Image, Address, FullSource.data(),
                         FullSource.size() * sizeof(double));
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setLoadCollect(true);
      Emulator.setRegister(x86reg::RAX, Address);
      Emulator.setRegister(WriteMask.Offset, UINT64_C(0xff));
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset),
                bytes(Expected));
      EXPECT_EQ(Emulator.getLoadRecords().size(), Test.Broadcast ? 1U : 8U);
      EXPECT_FALSE(Emulator.skips().any());
    }
  }

  // vcvtps2dq zmm30 {k7}{z}, zmm29 follows MXCSR when EVEX.b is clear.
  {
    const std::vector<uint8_t> Encoding = {0x62, 0x01, 0x7d,
                                           0xcf, 0x5b, 0xf5};
    std::vector<uint32_t> Source(16, 0);
    Source[0] = UINT32_C(0x3fa00000); // 1.25f
    Source[1] = UINT32_C(0xbfa00000); // -1.25f
    std::vector<uint32_t> Expected(16, 0);
    Expected[0] = 2;
    Expected[1] = UINT32_MAX;
    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_FALSE(Ops.empty());

    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x5f80); // Masked exceptions, round toward +infinity.
    Emulator.setRegister(WriteMask.Offset, 3);
    Emulator.setRegisterBytes(SourceZmm.Offset, bytes(Source));
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), bytes(Expected));
    EXPECT_NE(Emulator.getMXCSR() & (1U << 5), 0U);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

void expectGenericRex2DecodeLiftAndEmulate() {
  // mov r24, qword ptr [r31 + r30*4 - 32].  This is a generated-table
  // MAP0 REX2 instruction, rather than one of the dedicated APX decoders.
  // Decode detail, lifting, address formation, the memory read and the EGPR
  // write must all agree on the same five-bit register selections.
  const std::vector<uint8_t> Encoding = {0xd5, 0x7f, 0x8b,
                                         0x44, 0xb7, 0xe0};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Encoding.data(), Encoding.size(),
                                 kInstructionAddress, Insn),
            static_cast<int>(Encoding.size()));
  ASSERT_NE(Insn.Raw, nullptr);
  ASSERT_NE(Insn.Raw->detail, nullptr);
  const cs_x86 &X86 = Insn.Raw->detail->x86;
  EXPECT_EQ(X86.rex, 0U);
  EXPECT_EQ(X86.rex2, 0x7fU);
  EXPECT_EQ(X86.opcode[0], 0x8bU);
  ASSERT_EQ(X86.op_count, 2U);
  ASSERT_EQ(X86.operands[0].type, X86_OP_REG);
  EXPECT_EQ(X86.operands[0].reg, X86_REG_R24);
  EXPECT_EQ(X86.operands[0].size, 8U);
  ASSERT_EQ(X86.operands[1].type, X86_OP_MEM);
  EXPECT_EQ(X86.operands[1].mem.base, X86_REG_R31);
  EXPECT_EQ(X86.operands[1].mem.index, X86_REG_R30);
  EXPECT_EQ(X86.operands[1].mem.scale, 4);
  EXPECT_EQ(X86.operands[1].mem.disp, -32);

  const std::vector<LowOp> Ops = liftX64(Encoding);
  ASSERT_FALSE(Ops.empty());
  constexpr uint64_t Base = UINT64_C(0x2f000);
  constexpr uint64_t Index = UINT64_C(0x40);
  constexpr uint64_t Address = Base + Index * 4 - 32;
  constexpr uint64_t Value = UINT64_C(0x8877665544332211);
  BinaryImage Image = emptyImage();
  addReadableBytes(Image, Address, &Value, sizeof(Value));
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::R31, Base);
  Emulator.setRegister(x86reg::R30, Index);
  Emulator.setRegister(x86reg::R24, UINT64_C(0xa5a5a5a5a5a5a5a5));
  Emulator.setRegister(x86reg::CF, 1);
  Emulator.setRegister(x86reg::ZF, 0);
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegister(x86reg::R24), Value);
  EXPECT_EQ(Emulator.getRegister(x86reg::R31), Base);
  EXPECT_EQ(Emulator.getRegister(x86reg::R30), Index);
  EXPECT_EQ(Emulator.getRegister(x86reg::CF), 1U);
  EXPECT_EQ(Emulator.getRegister(x86reg::ZF), 0U);
  ASSERT_EQ(Emulator.getLoadRecords().size(), 1U);
  EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Address);
  EXPECT_EQ(Emulator.getLoadRecords()[0].Size, sizeof(Value));
  EXPECT_FALSE(Emulator.skips().any());

  // A legacy prefix after a REX byte invalidates that REX before REX2 is
  // decoded.  The resulting word move must preserve the upper destination
  // bits; an emulator must not turn this valid sequence into #UD.
  const std::vector<uint8_t> SeparatedRex = {0x4f, 0x66, 0xd5,
                                              0x55, 0x89, 0xc7};
  const std::vector<LowOp> SeparatedOps = liftX64(SeparatedRex);
  ASSERT_FALSE(SeparatedOps.empty());
  BinaryImage RegisterImage = emptyImage();
  NdOpEmulator RegisterEmulator(RegisterImage);
  RegisterEmulator.setStrictMode(true);
  RegisterEmulator.setRegister(x86reg::R24,
                               UINT64_C(0x0123456789abcdef));
  RegisterEmulator.setRegister(x86reg::R31,
                               UINT64_C(0xfedcba9876543210));
  RegisterEmulator.setRegister(x86reg::CF, 1);
  ASSERT_EQ(RegisterEmulator.run(SeparatedOps), SeparatedOps.size());
  EXPECT_EQ(RegisterEmulator.getRegister(x86reg::R24),
            UINT64_C(0x0123456789abcdef));
  EXPECT_EQ(RegisterEmulator.getRegister(x86reg::R31),
            UINT64_C(0xfedcba987654cdef));
  EXPECT_EQ(RegisterEmulator.getRegister(x86reg::CF), 1U);
  EXPECT_FALSE(RegisterEmulator.skips().any());

  // REX2 map 1 elides the legacy 0F map byte.  Verify that the generated
  // decode reaches the ordinary signed multiply lifter and that overflow is
  // derived from the full-width product.
  {
    const std::vector<uint8_t> Imul = {0xd5, 0xdd, 0xaf, 0xc7};
    const std::vector<LowOp> ImulOps = liftX64(Imul);
    ASSERT_FALSE(ImulOps.empty());
    BinaryImage ImulImage = emptyImage();
    NdOpEmulator ImulEmulator(ImulImage);
    ImulEmulator.setStrictMode(true);
    ImulEmulator.setRegister(x86reg::R24, INT64_MAX);
    ImulEmulator.setRegister(x86reg::R31, 2);
    ImulEmulator.setRegister(x86reg::CF, 0);
    ImulEmulator.setRegister(x86reg::OF, 0);
    ASSERT_EQ(ImulEmulator.run(ImulOps), ImulOps.size());
    EXPECT_EQ(ImulEmulator.getRegister(x86reg::R24), UINT64_C(-2));
    EXPECT_EQ(ImulEmulator.getRegister(x86reg::R31), 2U);
    EXPECT_EQ(ImulEmulator.getRegister(x86reg::CF), 1U);
    EXPECT_EQ(ImulEmulator.getRegister(x86reg::OF), 1U);
    EXPECT_FALSE(ImulEmulator.skips().any());
  }
}

void expectExplicitMsrInvalidateAndHighCFailClosed() {
  // Preserve the complete encoding family for every explicit-operand MSR
  // form.  The single opaque operation is intentionally the first effect:
  // feature/CPL/XCR0/bitmap/intercept checks and architectural faults belong
  // to an authenticated execution environment, not ordinary LowIR.
  {
    struct MsrCase {
      const char *Name;
      std::vector<uint8_t> Encoding;
      X86MsrAccessKind Kind;
      bool Write;
      bool Immediate;
      uint64_t BRegister;
      uint64_t Selector;
      uint32_t ImmediateValue;
    };
    const std::vector<MsrCase> Cases = {
        {"rdmsr-imm",
         {0x62, 0xff, 0xff, 0x08, 0xf6, 0xc1, 0x78, 0x56, 0x34, 0x12},
         X86MsrAccessKind::RdmsrImmediate, false, true, x86reg::R17, 0,
         UINT32_C(0x12345678)},
        {"wrmsrns-imm",
         {0x62, 0xff, 0xfe, 0x08, 0xf6, 0xc2, 0x78, 0x56, 0x34, 0x12},
         X86MsrAccessKind::WrmsrnsImmediate, true, true, x86reg::R18, 0,
         UINT32_C(0x12345678)},
        {"urdmsr-evex-imm",
         {0x62, 0xff, 0x7f, 0x08, 0xf8, 0xc3, 0x00, 0x1b, 0x00, 0x00},
         X86MsrAccessKind::UrdmsrEvexImmediate, false, true, x86reg::R19, 0,
         UINT32_C(0x1b00)},
        {"uwrmsr-evex-imm",
         {0x62, 0xff, 0x7e, 0x08, 0xf8, 0xc4, 0x01, 0x1b, 0x00, 0x00},
         X86MsrAccessKind::UwrmsrEvexImmediate, true, true, x86reg::R20, 0,
         UINT32_C(0x1b01)},
        {"urdmsr-evex-reg",
         {0x62, 0xec, 0x7f, 0x08, 0xf8, 0xf5},
         X86MsrAccessKind::UrdmsrEvexRegister, false, false, x86reg::R21,
         x86reg::R22, 0},
        {"uwrmsr-evex-reg",
         {0x62, 0xcc, 0x7e, 0x08, 0xf8, 0xf8},
         X86MsrAccessKind::UwrmsrEvexRegister, true, false, x86reg::R24,
         x86reg::R23, 0},
        {"urdmsr-legacy-reg",
         {0xf2, 0x45, 0x0f, 0x38, 0xf8, 0xc7},
         X86MsrAccessKind::UrdmsrLegacyRegister, false, false, x86reg::R15,
         x86reg::R8, 0},
        {"uwrmsr-legacy-reg",
         {0xf3, 0x45, 0x0f, 0x38, 0xf8, 0xc7},
         X86MsrAccessKind::UwrmsrLegacyRegister, true, false, x86reg::R15,
         x86reg::R8, 0},
        {"urdmsr-vex-imm",
         {0xc4, 0x07, 0x7b, 0xf8, 0xc7, 0x00, 0x1b, 0x00, 0x00},
         X86MsrAccessKind::UrdmsrVexImmediate, false, true, x86reg::R15, 0,
         UINT32_C(0x1b00)},
        {"uwrmsr-vex-imm",
         {0xc4, 0x07, 0x7a, 0xf8, 0xc7, 0x01, 0x1b, 0x00, 0x00},
         X86MsrAccessKind::UwrmsrVexImmediate, true, true, x86reg::R15, 0,
         UINT32_C(0x1b01)},
    };

    for (const MsrCase &Test : Cases) {
      SCOPED_TRACE(Test.Name);
      const std::vector<LowOp> Ops = liftX64(Test.Encoding);
      ASSERT_EQ(Ops.size(), 1U);
      const LowOp &Op = Ops.front();
      ASSERT_EQ(Op.Opcode, NdOp::INTRINSIC);
      ASSERT_EQ(Op.NumInputs, Test.Write ? 4 : 3);
      ASSERT_TRUE(Op.Inputs[0].isConst());
      EXPECT_EQ(static_cast<Intrinsic>(Op.Inputs[0].Offset),
                Intrinsic::X86MsrAccess);
      EXPECT_EQ(Op.Inputs[0].Size, 2U);
      ASSERT_TRUE(Op.Inputs[1].isConst());
      EXPECT_EQ(Op.Inputs[1].Offset, static_cast<uint64_t>(Test.Kind));
      EXPECT_EQ(Op.Inputs[1].Size, 1U);
      EXPECT_EQ(Op.MemoryOrdering, NdMemoryOrdering::None);
      EXPECT_EQ(Op.MemoryAddressSpace, NdMemoryAddressSpace::Default);
      EXPECT_TRUE(intrinsicX86MsrAccessShapeIsValid(
          Intrinsic::X86MsrAccess, x86MsrAccessLowShape(Op, Arch::X64)));
      EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::X86MsrAccess));

      if (Test.Immediate) {
        ASSERT_TRUE(Op.Inputs[2].isConst());
        EXPECT_EQ(Op.Inputs[2].Offset, Test.ImmediateValue);
        EXPECT_EQ(Op.Inputs[2].Size, 4U);
      } else {
        ASSERT_TRUE(Op.Inputs[2].isReg());
        EXPECT_EQ(Op.Inputs[2].Offset, Test.Selector);
        EXPECT_EQ(Op.Inputs[2].Size, 8U);
      }
      if (Test.Write) {
        EXPECT_EQ(Op.Output.Size, 0U);
        ASSERT_TRUE(Op.Inputs[3].isReg());
        EXPECT_EQ(Op.Inputs[3].Offset, Test.BRegister);
        EXPECT_EQ(Op.Inputs[3].Size, 8U);
      } else {
        ASSERT_TRUE(Op.Output.isReg());
        EXPECT_EQ(Op.Output.Offset, Test.BRegister);
        EXPECT_EQ(Op.Output.Size, 8U);
      }

      BinaryImage Image = emptyImage();
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setLoadCollect(true);
      constexpr uint64_t BBefore = UINT64_C(0xa55aa55aa55aa55a);
      constexpr uint64_t SelectorBefore = UINT64_C(0x5aa55aa55aa55aa5);
      constexpr uint64_t RaxBefore = UINT64_C(0x1111222233334444);
      constexpr uint64_t RcxBefore = UINT64_C(0x5555666677778888);
      constexpr uint64_t RdxBefore = UINT64_C(0x9999aaaabbbbcccc);
      Emulator.setRegister(Test.BRegister, BBefore);
      if (!Test.Immediate)
        Emulator.setRegister(Test.Selector, SelectorBefore);
      Emulator.setRegister(x86reg::RAX, RaxBefore);
      Emulator.setRegister(x86reg::RCX, RcxBefore);
      Emulator.setRegister(x86reg::RDX, RdxBefore);
      Emulator.setRegister(x86reg::CF, 1);
      Emulator.setRegister(x86reg::ZF, 0);
      EXPECT_EQ(Emulator.run(Ops), 0U);
      EXPECT_EQ(Emulator.getRegister(Test.BRegister), BBefore);
      if (!Test.Immediate)
        EXPECT_EQ(Emulator.getRegister(Test.Selector), SelectorBefore);
      EXPECT_EQ(Emulator.getRegister(x86reg::RAX), RaxBefore);
      EXPECT_EQ(Emulator.getRegister(x86reg::RCX), RcxBefore);
      EXPECT_EQ(Emulator.getRegister(x86reg::RDX), RdxBefore);
      EXPECT_EQ(Emulator.getRegister(x86reg::CF), 1U);
      EXPECT_EQ(Emulator.getRegister(x86reg::ZF), 0U);
      EXPECT_TRUE(Emulator.getLoadRecords().empty());
      EXPECT_FALSE(Emulator.skips().any());
    }

    expectMutatedLiftFailsClosed(Cases[0].Encoding,
                                 [](cs_insn &, cs_x86 &X86) {
                                   X86.operands[0].reg = X86_REG_R18;
                                   return true;
                                 });
    expectMutatedLiftFailsClosed(Cases[2].Encoding,
                                 [](cs_insn &Insn, cs_x86 &) {
                                   Insn.bytes[Insn.size - 1] ^= 1;
                                   return true;
                                 });
    expectMutatedLiftFailsClosed(Cases[6].Encoding,
                                 [](cs_insn &, cs_x86 &X86) {
                                   X86.rex ^= 1;
                                   return true;
                                 });
  }

  // INVPCID owns descriptor validation and the architectural invalidation in
  // one opaque effect.  A plain [rax] descriptor must remain an address input:
  // lifting it as a LOAD would fault before feature/CPL/type checks.
  {
    const std::vector<uint8_t> Encoding = {0x66, 0x0f, 0x38, 0x82, 0x10};
    const std::vector<LowOp> Ops = liftX64(Encoding);
    ASSERT_EQ(std::count_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
                return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs > 0 &&
                       Op.Inputs[0].isConst() &&
                       static_cast<Intrinsic>(Op.Inputs[0].Offset) ==
                           Intrinsic::X86Invalidate;
              }),
              1);
    ASSERT_FALSE(Ops.empty());
    const LowOp &Op = Ops.front();
    ASSERT_EQ(Op.Opcode, NdOp::INTRINSIC);
    ASSERT_EQ(Op.NumInputs, 4);
    ASSERT_TRUE(Op.Inputs[0].isConst());
    EXPECT_EQ(static_cast<Intrinsic>(Op.Inputs[0].Offset),
              Intrinsic::X86Invalidate);
    EXPECT_EQ(Op.Inputs[0].Size, 2U);
    ASSERT_TRUE(Op.Inputs[1].isReg());
    EXPECT_EQ(Op.Inputs[1].Offset, x86reg::RAX);
    EXPECT_EQ(Op.Inputs[1].Size, 8U);
    ASSERT_TRUE(Op.Inputs[2].isConst());
    EXPECT_EQ(Op.Inputs[2].Offset,
              static_cast<uint64_t>(X86InvalidateKind::Invpcid));
    EXPECT_EQ(Op.Inputs[2].Size, 1U);
    ASSERT_TRUE(Op.Inputs[3].isReg());
    EXPECT_EQ(Op.Inputs[3].Offset, x86reg::RDX);
    EXPECT_EQ(Op.Inputs[3].Size, 8U);
    EXPECT_EQ(Op.Output.Size, 0U);
    EXPECT_EQ(Op.MemoryOrdering, NdMemoryOrdering::None);
    EXPECT_EQ(Op.MemoryAddressSpace, NdMemoryAddressSpace::Default);
    EXPECT_TRUE(intrinsicX86InvalidateShapeIsValid(
        Intrinsic::X86Invalidate, x86InvalidateLowShape(Op, Arch::X64)));
    EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::X86Invalidate));
    EXPECT_TRUE(std::none_of(Ops.begin(), Ops.end(), [](const LowOp &Candidate) {
      return Candidate.Opcode == NdOp::LOAD;
    }));

    BinaryImage Image = emptyImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    constexpr uint64_t DescriptorAddress = UINT64_C(0x42000);
    constexpr uint64_t Type = UINT64_C(2);
    constexpr uint64_t Sentinel = UINT64_C(0xa55a0123456789ab);
    Emulator.setRegister(x86reg::RAX, DescriptorAddress);
    Emulator.setRegister(x86reg::RDX, Type);
    Emulator.setRegister(x86reg::R15, Sentinel);
    Emulator.setRegister(x86reg::CF, 1);
    Emulator.setRegister(x86reg::PF, 0);
    Emulator.setRegister(x86reg::AF, 1);
    Emulator.setRegister(x86reg::ZF, 0);
    Emulator.setRegister(x86reg::SF, 1);
    Emulator.setRegister(x86reg::OF, 0);
    EXPECT_EQ(Emulator.run(Ops), 0U);
    EXPECT_EQ(Emulator.getRegister(x86reg::RAX), DescriptorAddress);
    EXPECT_EQ(Emulator.getRegister(x86reg::RDX), Type);
    EXPECT_EQ(Emulator.getRegister(x86reg::R15), Sentinel);
    EXPECT_EQ(Emulator.getRegister(x86reg::CF), 1U);
    EXPECT_EQ(Emulator.getRegister(x86reg::PF), 0U);
    EXPECT_EQ(Emulator.getRegister(x86reg::AF), 1U);
    EXPECT_EQ(Emulator.getRegister(x86reg::ZF), 0U);
    EXPECT_EQ(Emulator.getRegister(x86reg::SF), 1U);
    EXPECT_EQ(Emulator.getRegister(x86reg::OF), 0U);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());

    expectMutatedLiftFailsClosed(Encoding, [](cs_insn &, cs_x86 &X86) {
      if (X86.op_count != 2 || X86.operands[1].type != X86_OP_MEM)
        return false;
      X86.operands[1].size = 8;
      return true;
    });
  }

  // These effects have no faithful standalone-C helper.  Ensure the High C
  // renderer stops rather than silently emitting an undeclared approximation.
  EXPECT_EQ(intrinsicCName(Intrinsic::X86MsrAccess), nullptr);
  EXPECT_EQ(intrinsicCName(Intrinsic::X86Invalidate), nullptr);
  EXPECT_EQ(intrinsicCName(Intrinsic::X86RequireDivPrecondition), nullptr);
  EXPECT_STREQ(x86HighCIntrinsicFatalReason(Intrinsic::X86MsrAccess),
               "x86 MSR access requires an authenticated architectural "
               "execution environment");
  EXPECT_STREQ(x86HighCIntrinsicFatalReason(Intrinsic::X86Invalidate),
               "x86 address-translation invalidation requires an "
               "authenticated architectural execution environment");
  EXPECT_STREQ(
      x86HighCIntrinsicFatalReason(Intrinsic::X86RequireDivPrecondition),
      "x86 division precondition requires an architectural fault environment");
  EXPECT_EQ(x86HighCIntrinsicFatalReason(Intrinsic::Cpuid), nullptr);
}

} // namespace
