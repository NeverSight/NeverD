#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

using namespace neverd;
namespace {

BinaryImage image(const std::vector<uint8_t> &Bytes) {
  BinaryImage Result;
  Result.Arch = Arch::X64;
  Result.Bits = Bitness::Bits64;
  Segment Data;
  Data.VA = 0x4000;
  Data.Size = Bytes.size();
  Data.Flags = SegmentFlags::Readable;
  Data.Data = Bytes;
  Result.Segments.push_back(std::move(Data));
  return Result;
}

void putDword(std::vector<uint8_t> &Bytes, size_t Offset, uint32_t Value) {
  std::memcpy(Bytes.data() + Offset, &Value, sizeof(Value));
}

uint32_t getDword(const std::vector<uint8_t> &Bytes, size_t Offset) {
  uint32_t Value = 0;
  std::memcpy(&Value, Bytes.data() + Offset, sizeof(Value));
  return Value;
}

LowOp vp4(Intrinsic Id, uint64_t Address, uint64_t GroupBase, uint64_t Mask,
          bool Zeroing) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::reg(mapCapstoneReg(X86_REG_ZMM1).Offset, 64);
  Op.addInput(NdVar::cst(static_cast<uint64_t>(Id), 2));
  Op.addInput(NdVar::cst(Address, 8));
  Op.addInput(NdVar::reg(mapCapstoneReg(X86_REG_ZMM1).Offset, 64));
  Op.addInput(NdVar::cst(GroupBase, 1));
  Op.addInput(NdVar::cst(Mask, 2));
  Op.addInput(NdVar::cst(Zeroing ? 1 : 0, 1));
  return Op;
}

void setSourceBlock(NdOpEmulator &Emulator, unsigned GroupBase, int16_t LowWord,
                    int16_t HighWord) {
  for (unsigned Register = 0; Register < 4; ++Register) {
    std::vector<uint8_t> Value(64, 0);
    for (unsigned Lane = 0; Lane < 16; ++Lane) {
      std::memcpy(Value.data() + Lane * 4, &LowWord, sizeof(LowWord));
      std::memcpy(Value.data() + Lane * 4 + 2, &HighWord, sizeof(HighWord));
    }
    Emulator.setRegisterBytes(
        mapCapstoneReg(
            static_cast<x86_reg>(X86_REG_ZMM0 + GroupBase + Register))
            .Offset,
        Value);
  }
}

std::vector<LowOp> lift(std::initializer_list<uint8_t> Bytes) {
  const std::vector<uint8_t> Code(Bytes);
  Decoder DecoderInstance;
  EXPECT_TRUE(DecoderInstance.init(Arch::X64));
  DecodedInsn Instruction{};
  EXPECT_EQ(DecoderInstance.decodeOneForLift(Code.data(), Code.size(), 0x1000,
                                             Instruction),
            static_cast<int>(Code.size()));
  std::vector<LowOp> Ops;
  DecoderInstance.liftToLow(Instruction, Ops);
  return Ops;
}

TEST(X86VP4DPW, CanonicalLiftOwnsMemoryAndFourRegisterSourceBlock) {
  const auto Ops = lift({0x62, 0xf2, 0x5f, 0xc2, 0x52, 0x08});
  EXPECT_TRUE(std::none_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE;
  }));
  const auto IntrinsicOp =
      std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
        return Op.Opcode == NdOp::INTRINSIC;
      });
  ASSERT_NE(IntrinsicOp, Ops.end());
  EXPECT_EQ(std::count_if(
                Ops.begin(), Ops.end(),
                [](const LowOp &Op) { return Op.Opcode == NdOp::INTRINSIC; }),
            1);
  ASSERT_EQ(IntrinsicOp->NumInputs, 6);
  ASSERT_TRUE(IntrinsicOp->Inputs[0].isConst());
  EXPECT_EQ(static_cast<Intrinsic>(IntrinsicOp->Inputs[0].Offset),
            Intrinsic::X86VP4DPWSSD);
  EXPECT_EQ(IntrinsicOp->Output.Size, 64);
  EXPECT_EQ(IntrinsicOp->Inputs[1].Size, 8);
  EXPECT_EQ(IntrinsicOp->Inputs[2].Size, 64);
  ASSERT_TRUE(IntrinsicOp->Inputs[3].isConst());
  EXPECT_EQ(IntrinsicOp->Inputs[3].Offset, 20u);
  EXPECT_EQ(IntrinsicOp->Inputs[4].Size, 2);
  ASSERT_TRUE(IntrinsicOp->Inputs[5].isConst());
  EXPECT_EQ(IntrinsicOp->Inputs[5].Offset, 1u);
}

TEST(X86VP4DPW, SideeffectIDsAndFourSequentialWrapOrSaturateRounds) {
  EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::X86VP4DPWSSD));
  EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::X86VP4DPWSSDS));
  std::vector<uint8_t> Memory(16, 0);
  for (unsigned I = 0; I < 4; ++I)
    putDword(Memory, I * 4, UINT32_C(0x7fff7fff));

  for (Intrinsic Id : {Intrinsic::X86VP4DPWSSD, Intrinsic::X86VP4DPWSSDS}) {
    const BinaryImage Image = image(Memory);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    std::vector<uint8_t> Destination(64, 0);
    putDword(Destination, 0, UINT32_C(0x7ffffffe));
    Emulator.setRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset, Destination);
    setSourceBlock(Emulator, 4, 0x7fff, 0x7fff);
    const LowOp Op = vp4(Id, 0x4000, 4, 1, false);
    EXPECT_TRUE(Emulator.step(Op));
    const auto Result =
        Emulator.getRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset);
    ASSERT_TRUE(Result);
    const int64_t Product = 2LL * 0x7fff * 0x7fff;
    uint32_t Expected = UINT32_C(0x7ffffffe);
    for (unsigned I = 0; I < 4; ++I) {
      const int64_t Sum =
          static_cast<int64_t>(static_cast<int32_t>(Expected)) + Product;
      if (Id == Intrinsic::X86VP4DPWSSDS)
        Expected = static_cast<uint32_t>(static_cast<int32_t>(
            std::clamp<int64_t>(Sum, std::numeric_limits<int32_t>::min(),
                                std::numeric_limits<int32_t>::max())));
      else
        Expected = static_cast<uint32_t>(Sum);
    }
    EXPECT_EQ(getDword(*Result, 0), Expected);
  }
}

TEST(X86VP4DPW, ZeroMaskSuppressesTupleAndMergePreservesInactiveLanes) {
  const BinaryImage Empty = image({});
  NdOpEmulator Emulator(Empty);
  Emulator.setStrictMode(true);
  std::vector<uint8_t> Destination(64, 0xa5);
  Emulator.setRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset, Destination);
  setSourceBlock(Emulator, 4, 1, 1);
  EXPECT_TRUE(
      Emulator.step(vp4(Intrinsic::X86VP4DPWSSD, 0xdead0000, 4, 0, false)));
  auto Merged = Emulator.getRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset);
  ASSERT_TRUE(Merged);
  EXPECT_EQ(*Merged, Destination);

  Emulator.setRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset, Destination);
  EXPECT_TRUE(
      Emulator.step(vp4(Intrinsic::X86VP4DPWSSD, 0xdead0000, 4, 0, true)));
  auto Zeroed = Emulator.getRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset);
  ASSERT_TRUE(Zeroed);
  EXPECT_TRUE(std::all_of(Zeroed->begin(), Zeroed->end(),
                          [](uint8_t Byte) { return Byte == 0; }));
}

TEST(X86VP4DPW, ActiveMaskRequiresTheCompleteSixteenByteTuple) {
  std::vector<uint8_t> ShortTuple(15, 0);
  const BinaryImage Image = image(ShortTuple);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  std::vector<uint8_t> Destination(64, 0x5a);
  Emulator.setRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset, Destination);
  setSourceBlock(Emulator, 4, 1, 1);
  EXPECT_FALSE(
      Emulator.step(vp4(Intrinsic::X86VP4DPWSSD, 0x4000, 4, 1, false)));
  const auto Result =
      Emulator.getRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, Destination);
}

} // namespace
