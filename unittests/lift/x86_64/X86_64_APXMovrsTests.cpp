//===- X86_64_APXMovrsTests.cpp - APX MOVRS semantics --------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;
constexpr va_t kDataAddress = 0x6000;

std::vector<LowOp> lift(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  EXPECT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                 kInstructionAddress, Insn),
            static_cast<int>(Bytes.size()));
  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
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

BinaryImage imageWithValue() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Segment Data;
  Data.VA = kDataAddress;
  Data.Size = 8;
  Data.Flags = SegmentFlags::Readable;
  Data.Data = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
  Image.Segments.push_back(std::move(Data));
  return Image;
}

TEST(X86ApxMovrs, RestrictedSpeculationHintHasOrdinaryLoadStateSemantics) {
  struct Form {
    std::vector<uint8_t> Bytes;
    x86_reg Destination;
    unsigned Width;
    uint64_t Expected;
  };
  const std::array<Form, 4> Forms = {{
      {{0x62, 0xec, 0x7c, 0x08, 0x8a, 0x11},
       X86_REG_R18B,
       1,
       0xaabbccddeeff0088ULL},
      {{0x62, 0xec, 0x7d, 0x08, 0x8b, 0x11},
       X86_REG_R18W,
       2,
       0xaabbccddeeff7788ULL},
      {{0x62, 0xec, 0x7c, 0x08, 0x8b, 0x11},
       X86_REG_R18D,
       4,
       0x0000000055667788ULL},
      {{0x62, 0xec, 0xfc, 0x08, 0x8b, 0x11},
       X86_REG_R18,
       8,
       0x1122334455667788ULL},
  }};

  for (const Form &F : Forms) {
    SCOPED_TRACE(F.Width);
    const auto Ops = lift(F.Bytes);
    ASSERT_FALSE(Ops.empty());
    auto Load = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
      return Op.Opcode == NdOp::LOAD;
    });
    auto Write = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
      return Op.Opcode == NdOp::COPY && Op.Output.isReg();
    });
    ASSERT_NE(Load, Ops.end());
    ASSERT_NE(Write, Ops.end());
    EXPECT_LT(Load, Write);
    EXPECT_EQ(Load->Output.Size, F.Width);

    BinaryImage Image = imageWithValue();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mapCapstoneReg(X86_REG_R17).Offset, kDataAddress);
    Emulator.setRegister(mapCapstoneReg(X86_REG_R18).Offset,
                         0xaabbccddeeff0011ULL);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result =
        Emulator.getRegister(mapCapstoneReg(X86_REG_R18).Offset);
    ASSERT_TRUE(Result);
    EXPECT_EQ(*Result, F.Expected);
  }
}

TEST(X86ApxMovrs, SegmentAddr32AndExtendedAddressRegistersRemainExact) {
  // movrs r26d, dword ptr fs:[r29d + r14d*4 + 0x20]
  const auto Ops =
      lift({0x64, 0x67, 0x62, 0x0c, 0x7c, 0x08, 0x8b, 0x54, 0xb5, 0x20});
  ASSERT_FALSE(Ops.empty());
  auto Load = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return Op.Opcode == NdOp::LOAD;
  });
  ASSERT_NE(Load, Ops.end());
  EXPECT_EQ(Load->MemoryAddressSpace, NdMemoryAddressSpace::X86FS);
  EXPECT_EQ(Load->Output.Size, 4u);

  BinaryImage Image = imageWithValue();
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS,
                                                 kDataAddress - 0x30));
  Emulator.setRegister(mapCapstoneReg(X86_REG_R29D).Offset, 0x08);
  Emulator.setRegister(mapCapstoneReg(X86_REG_R14D).Offset, 0x02);
  Emulator.setRegister(mapCapstoneReg(X86_REG_R26).Offset,
                       0xffffffffffffffffULL);
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegister(mapCapstoneReg(X86_REG_R26).Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, 0x0000000055667788ULL);
}

TEST(X86ApxMovrs, UBitSelectsR28ForAddr64AndAddr32Memory) {
  const std::array<std::vector<uint8_t>, 2> Forms = {{
      {0x64, 0x62, 0x0c, 0x78, 0x08, 0x8b, 0x54, 0xa5, 0x20},
      {0x67, 0x64, 0x62, 0x0c, 0x78, 0x08, 0x8b, 0x54, 0xa5, 0x20},
  }};
  for (unsigned Address32 = 0; Address32 != Forms.size(); ++Address32) {
    SCOPED_TRACE(Address32);
    const auto Ops = lift(Forms[Address32]);
    ASSERT_FALSE(Ops.empty());

    BinaryImage Image = imageWithValue();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS,
                                                   kDataAddress - 0x30));
    Emulator.setRegister(mapCapstoneReg(X86_REG_R29).Offset,
                         Address32 ? UINT64_C(0xaaaaaaaa00000008) : 8);
    Emulator.setRegister(mapCapstoneReg(X86_REG_R28).Offset,
                         Address32 ? UINT64_C(0xbbbbbbbb00000002) : 2);
    Emulator.setRegister(mapCapstoneReg(X86_REG_R26).Offset, UINT64_MAX);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(mapCapstoneReg(X86_REG_R26).Offset),
              UINT64_C(0x55667788));
    ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, kDataAddress);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Size, 4u);

    expectMutatedLiftRejected(Forms[Address32], [Address32](cs_insn &Raw) {
      Raw.detail->x86.operands[1].mem.index =
          Address32 ? X86_REG_R14D : X86_REG_R14;
    });
    expectMutatedLiftRejected(Forms[Address32], [](cs_insn &Raw) {
      Raw.bytes[Raw.detail->x86.encoding.modrm_offset - 3] |= 0x04;
    });
  }
}

TEST(X86ApxMovrs, IllegalRegisterSourceIsRejectedAndFaultDoesNotWriteTarget) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  const std::vector<uint8_t> RegisterSource = {0x62, 0xec, 0x7c,
                                               0x08, 0x8b, 0xd1};
  DecodedInsn Invalid{};
  EXPECT_NE(Dec.decodeOneForLift(RegisterSource.data(), RegisterSource.size(),
                                 kInstructionAddress, Invalid),
            static_cast<int>(RegisterSource.size()));
  const std::vector<uint8_t> RegisterSourceU0 = {0x62, 0xec, 0x78,
                                                 0x08, 0x8b, 0xd1};
  EXPECT_NE(Dec.decodeOneForLift(RegisterSourceU0.data(),
                                 RegisterSourceU0.size(), kInstructionAddress,
                                 Invalid),
            static_cast<int>(RegisterSourceU0.size()));

  // Reject duplicated segment/address-size prefixes at whichever strict layer
  // observes them first.  A decoder-level rejection is preferable, while the
  // lifter still fails closed if a decoder normalizes the duplicate.
  for (const std::vector<uint8_t> &Duplicate :
       {std::vector<uint8_t>{0x64, 0x65, 0x62, 0xec, 0x7c, 0x08, 0x8b, 0x11},
        std::vector<uint8_t>{0x67, 0x67, 0x62, 0xec, 0x7c, 0x08, 0x8b, 0x11}}) {
    DecodedInsn Insn{};
    const int Decoded = Dec.decodeOneForLift(Duplicate.data(), Duplicate.size(),
                                             kInstructionAddress, Insn);
    if (Decoded != static_cast<int>(Duplicate.size())) {
      EXPECT_EQ(Decoded, 0);
      continue;
    }
    std::vector<LowOp> InvalidOps;
    EXPECT_THROW(Dec.liftToLow(Insn, InvalidOps), UnliftedInstruction);
    EXPECT_TRUE(InvalidOps.empty());
  }

  const std::vector<uint8_t> ReservedP2 = {0x62, 0xec, 0x7c, 0x09, 0x8b, 0x11};
  DecodedInsn Reserved{};
  EXPECT_NE(Dec.decodeOneForLift(ReservedP2.data(), ReservedP2.size(),
                                 kInstructionAddress, Reserved),
            static_cast<int>(ReservedP2.size()));

  const auto Ops = lift({0x62, 0xec, 0xfc, 0x08, 0x8b, 0x11});
  BinaryImage Empty;
  Empty.Arch = Arch::X64;
  Empty.Bits = Bitness::Bits64;
  NdOpEmulator Fault(Empty);
  Fault.setStrictMode(true);
  Fault.setRegister(mapCapstoneReg(X86_REG_R17).Offset, 0xdead0000);
  Fault.setRegister(mapCapstoneReg(X86_REG_R18).Offset, 0xfeedfacecafebeefULL);
  EXPECT_LT(Fault.run(Ops), Ops.size());
  const auto Result = Fault.getRegister(mapCapstoneReg(X86_REG_R18).Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, 0xfeedfacecafebeefULL);
}

TEST(X86ApxMovrs, VirtualizationInvalidationInstructionsRemainUnlifted) {
  const std::array<std::vector<uint8_t>, 2> Instructions = {{
      {0x62, 0xec, 0x7e, 0x08, 0xf0, 0x11},
      {0x62, 0xec, 0x7e, 0x08, 0xf1, 0x11},
  }};
  for (const auto &Bytes : Instructions) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                   kInstructionAddress, Insn),
              static_cast<int>(Bytes.size()));
    std::vector<LowOp> Ops;
    EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  }
}

} // namespace
