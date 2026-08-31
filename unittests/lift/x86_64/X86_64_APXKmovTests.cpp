//===- X86_64_APXKmovTests.cpp - APX KMOV semantics ---------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;
constexpr va_t kDataAddress = 0x5a00;

struct LiftedInstruction {
  unsigned Id = X86_INS_INVALID;
  std::vector<LowOp> Ops;
};

LiftedInstruction liftX64(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  if (!Dec.init(Arch::X64)) {
    ADD_FAILURE() << "failed to initialize x86-64 decoder";
    return {};
  }
  DecodedInsn Insn{};
  if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kInstructionAddress,
                           Insn) != static_cast<int>(Bytes.size())) {
    ADD_FAILURE() << "failed to decode complete KMOV instruction";
    return {};
  }

  LiftedInstruction Result;
  Result.Id = Insn.Id;
  try {
    Dec.liftToLow(Insn, Result.Ops);
  } catch (const UnliftedInstruction &) {
    ADD_FAILURE() << "KMOV was not lifted";
  }
  return Result;
}

template <typename Mutator>
void expectMutatedLiftRejected(const std::vector<uint8_t> &Bytes,
                               Mutator Mutate) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                 kInstructionAddress, Insn),
            static_cast<int>(Bytes.size()));
  ASSERT_NE(Insn.Raw, nullptr);
  ASSERT_NE(Insn.Raw->detail, nullptr);
  Mutate(*Insn.Raw);
  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

BinaryImage makeImage() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Data;
  Data.VA = kDataAddress;
  Data.Size = 4;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data = {0, 0, 0, 0};
  Image.Segments.push_back(std::move(Data));
  return Image;
}

TEST(X86APXKmov, UBitSelectsR28ForAddr64AndAddr32Memory) {
  struct Form {
    std::vector<uint8_t> Load;
    std::vector<uint8_t> Store;
    bool Address32;
  };
  const std::array<Form, 2> Forms = {{
      {{0x64, 0x62, 0x99, 0xf9, 0x08, 0x90, 0x54, 0xa5, 0x20},
       {0x64, 0x62, 0x99, 0xf9, 0x08, 0x91, 0x5c, 0xa5, 0x20},
       false},
      {{0x67, 0x64, 0x62, 0x99, 0xf9, 0x08, 0x90, 0x54, 0xa5, 0x20},
       {0x67, 0x64, 0x62, 0x99, 0xf9, 0x08, 0x91, 0x5c, 0xa5, 0x20},
       true},
  }};

  for (const Form &F : Forms) {
    SCOPED_TRACE(F.Address32);
    const LiftedInstruction Store = liftX64(F.Store);
    const LiftedInstruction Load = liftX64(F.Load);
    ASSERT_EQ(Store.Id, X86_INS_KMOVD);
    ASSERT_EQ(Load.Id, X86_INS_KMOVD);
    ASSERT_NE(
        std::find_if(Store.Ops.begin(), Store.Ops.end(),
                     [](const LowOp &Op) { return Op.Opcode == NdOp::STORE; }),
        Store.Ops.end());
    ASSERT_NE(
        std::find_if(Load.Ops.begin(), Load.Ops.end(),
                     [](const LowOp &Op) { return Op.Opcode == NdOp::LOAD; }),
        Load.Ops.end());

    std::vector<LowOp> Ops = Store.Ops;
    Ops.insert(Ops.end(), Load.Ops.begin(), Load.Ops.end());
    BinaryImage Image = makeImage();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS,
                                                   kDataAddress - 0x30));
    Emulator.setRegister(x86reg::R29, F.Address32 ? UINT64_C(0xaaaaaaaa00000008)
                                                  : UINT64_C(8));
    Emulator.setRegister(x86reg::R28, F.Address32 ? UINT64_C(0xbbbbbbbb00000002)
                                                  : UINT64_C(2));
    Emulator.setRegister(x86reg::K3, UINT64_C(0xdeadbeef89abcdef));
    Emulator.setRegister(x86reg::K2, UINT64_MAX);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::K2), UINT64_C(0x89abcdef));
    EXPECT_EQ(Emulator.getRegister(x86reg::K3), UINT64_C(0xdeadbeef89abcdef));
    ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, kDataAddress);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Size, 4u);
    EXPECT_FALSE(Emulator.skips().any());

    expectMutatedLiftRejected(F.Load, [Address32 = F.Address32](cs_insn &Raw) {
      Raw.detail->x86.operands[1].mem.index =
          Address32 ? X86_REG_R14D : X86_REG_R14;
    });
    expectMutatedLiftRejected(F.Load, [](cs_insn &Raw) {
      Raw.detail->x86.operands[1].access = CS_AC_WRITE;
    });
    expectMutatedLiftRejected(
        F.Load, [](cs_insn &Raw) { Raw.detail->x86.operands[1].size = 8; });
    expectMutatedLiftRejected(F.Load, [](cs_insn &Raw) {
      Raw.detail->x86.operands[1].avx_bcast = X86_AVX_BCAST_2;
    });
    expectMutatedLiftRejected(F.Load, [](cs_insn &Raw) {
      Raw.bytes[Raw.detail->x86.encoding.modrm_offset - 3] |= 0x04;
    });
    expectMutatedLiftRejected(F.Store, [](cs_insn &Raw) {
      Raw.detail->x86.operands[0].mem.base = X86_REG_R13;
    });
  }
}

TEST(X86APXKmov, MemoryFaultsDoNotCommitDestinationOrStore) {
  const std::vector<uint8_t> LoadBytes = {0x64, 0x62, 0x99, 0xf9, 0x08,
                                          0x90, 0x54, 0xa5, 0x20};
  const std::vector<uint8_t> StoreBytes = {0x64, 0x62, 0x99, 0xf9, 0x08,
                                           0x91, 0x5c, 0xa5, 0x20};
  const LiftedInstruction Load = liftX64(LoadBytes);
  const LiftedInstruction Store = liftX64(StoreBytes);

  BinaryImage Empty;
  Empty.Arch = Arch::X64;
  Empty.Bits = Bitness::Bits64;
  NdOpEmulator FaultingLoad(Empty);
  FaultingLoad.setStrictMode(true);
  FaultingLoad.setLoadCollect(true);
  ASSERT_TRUE(FaultingLoad.setMemoryAddressSpaceBase(
      NdMemoryAddressSpace::X86FS, kDataAddress - 0x30));
  FaultingLoad.setRegister(x86reg::R29, 8);
  FaultingLoad.setRegister(x86reg::R28, 2);
  FaultingLoad.setRegister(x86reg::K2, UINT64_C(0xfeedfacecafebeef));
  EXPECT_LT(FaultingLoad.run(Load.Ops), Load.Ops.size());
  EXPECT_EQ(FaultingLoad.getRegister(x86reg::K2), UINT64_C(0xfeedfacecafebeef));
  EXPECT_TRUE(FaultingLoad.getLoadRecords().empty());

  BinaryImage ReadOnly = makeImage();
  ReadOnly.Segments[0].Flags = SegmentFlags::Readable;
  NdOpEmulator FaultingStore(ReadOnly);
  FaultingStore.setStrictMode(true);
  ASSERT_TRUE(FaultingStore.setMemoryAddressSpaceBase(
      NdMemoryAddressSpace::X86FS, kDataAddress - 0x30));
  FaultingStore.setRegister(x86reg::R29, 8);
  FaultingStore.setRegister(x86reg::R28, 2);
  FaultingStore.setRegister(x86reg::K3, UINT64_C(0x1234567889abcdef));
  EXPECT_LT(FaultingStore.run(Store.Ops), Store.Ops.size());
  EXPECT_EQ(FaultingStore.getRegister(x86reg::K3),
            UINT64_C(0x1234567889abcdef));
  EXPECT_FALSE(FaultingStore.skips().any());
  FaultingStore.setLoadCollect(true);
  FaultingStore.setRegister(x86reg::K2, UINT64_MAX);
  ASSERT_EQ(FaultingStore.run(Load.Ops), Load.Ops.size());
  EXPECT_EQ(FaultingStore.getRegister(x86reg::K2), UINT64_C(0));
  ASSERT_EQ(FaultingStore.getLoadRecords().size(), 1u);
  EXPECT_EQ(FaultingStore.getLoadRecords()[0].Addr, kDataAddress);
}

TEST(X86APXKmov, RegisterUZeroIsRejectedAndVexRemainsSupported) {
  const std::vector<uint8_t> ValidApx = {0x62, 0xf9, 0x7d, 0x08, 0x92, 0xd9};
  expectMutatedLiftRejected(ValidApx, [](cs_insn &Raw) {
    Raw.bytes[Raw.detail->x86.encoding.modrm_offset - 3] &=
        static_cast<uint8_t>(~0x04);
  });
  expectMutatedLiftRejected(ValidApx,
                            [](cs_insn &Raw) { Raw.bytes[0] = 0xc5; });
  expectMutatedLiftRejected(ValidApx, [](cs_insn &Raw) {
    Raw.detail->x86.operands[1].reg = X86_REG_R18D;
  });
  expectMutatedLiftRejected(ValidApx, [](cs_insn &Raw) {
    Raw.detail->x86.operands[0].access = CS_AC_READ;
  });
  expectMutatedLiftRejected(ValidApx,
                            [](cs_insn &Raw) { Raw.detail->x86.op_count = 1; });

  const std::vector<uint8_t> RegisterU0 = {0x62, 0xf9, 0x79, 0x08, 0x92, 0xd9};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Invalid{};
  EXPECT_NE(Dec.decodeOneForLift(RegisterU0.data(), RegisterU0.size(),
                                 kInstructionAddress, Invalid),
            static_cast<int>(RegisterU0.size()));

  const LiftedInstruction Legacy =
      liftX64({0xc5, 0xf9, 0x90, 0xd9}); // kmovb k3, k1
  ASSERT_EQ(Legacy.Id, X86_INS_KMOVB);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::K1, UINT64_C(0x1234abcd));
  Emulator.setRegister(x86reg::K3, UINT64_MAX);
  ASSERT_EQ(Emulator.run(Legacy.Ops), Legacy.Ops.size());
  EXPECT_EQ(Emulator.getRegister(x86reg::K3), UINT64_C(0xcd));
  EXPECT_FALSE(Emulator.skips().any());
}

} // namespace
