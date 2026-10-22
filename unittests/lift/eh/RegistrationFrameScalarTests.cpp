//===- RegistrationFrameScalarTests.cpp - PE32 relative scalar proofs ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve exact scalar results while rejecting ambiguous frame provenance.

#include "../../../lib/ir/low/X86/RegistrationFrame.h"
#include "gtest/gtest.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

namespace {
using namespace neverd;
using namespace neverd::registration_state;

FrameValue coordinate(unsigned Space, int32_t Offset) {
  return Space == 0   ? FrameValue::frame(Offset)
         : Space == 1 ? FrameValue::entryFrame(Offset)
                      : FrameValue::callbackFrame(0x1800, Offset);
}

LowOp operation(NdOp Opcode) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = NdVar::tmp(1, Opcode == NdOp::INT_SUB ? 4 : 1);
  Op.NumInputs = 2;
  Op.Inputs[0] = NdVar::reg(x86reg::RAX, 4);
  Op.Inputs[1] = NdVar::reg(x86reg::RCX, 4);
  return Op;
}

FrameValue evaluate(FrameValue Left, FrameValue Right, const LowOp &Op) {
  FrameState Frame;
  Frame.Registers[x86reg::RAX / x86reg::GeneralRegStride] = Left;
  Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride] = Right;
  return FrameTransfer(Frame, -16).evaluate(Op, true);
}

TEST(RegistrationFrameScalar, ExactCoordinatesPreserveRelocatedResults) {
  for (unsigned Space = 0; Space != 3; ++Space)
    for (int32_t Left : {-64, -16, 0, INT32_MIN, INT32_MAX})
      for (int32_t Right : {-64, -16, 0, INT32_MIN, INT32_MAX})
        for (NdOp Opcode :
             {NdOp::INT_SUB, NdOp::INT_EQUAL, NdOp::INT_NOTEQUAL}) {
          SCOPED_TRACE(::testing::Message()
                       << Space << ':' << Left << ':' << Right << ':'
                       << unsigned(Opcode));
          const auto Result =
              evaluate(coordinate(Space, Left), coordinate(Space, Right),
                       operation(Opcode));
          ASSERT_TRUE(Result.Constant);
          EXPECT_FALSE(Result.MayBeFrame);
          // Check the observable result at different native frame bases,
          // including wraparound in the PE32 integer address domain.
          for (uint32_t Base : {0u, 0x10000u, 0x14000000u, UINT32_MAX}) {
            const uint32_t L = Base + uint32_t(Left);
            const uint32_t R = Base + uint32_t(Right);
            const uint32_t Expected = Opcode == NdOp::INT_SUB     ? L - R
                                      : Opcode == NdOp::INT_EQUAL ? L == R
                                                                  : L != R;
            EXPECT_EQ(*Result.Constant, Expected);
          }
        }
}

TEST(RegistrationFrameScalar, DistinctOrAmbiguousRootsRetainTaint) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    FrameValue Left = FrameValue::frame(-16), Right = Left;
    switch (Mutation) {
    case 0:
      Right = FrameValue::entryFrame(-16);
      break;
    case 1:
      Right = FrameValue::callbackFrame(0x1800, -16);
      break;
    case 2:
      Left = FrameValue::entryFrame(-16);
      Right = FrameValue::callbackFrame(0x1800, -16);
      break;
    case 3:
      Left = FrameValue::callbackFrame(0x1800, -16);
      Right = FrameValue::callbackFrame(0x1900, -16);
      break;
    case 4:
      Right = join(Right, FrameValue::frame(-20));
      break;
    case 5:
      Right = join(Right, {});
      break;
    case 6:
      Right = FrameValue::exceptionObject(0, 0);
      Left = Right;
      break;
    case 7:
      Right.SecurityCookie = true;
      break;
    case 8:
      Right.ReturnPC = true;
      break;
    case 9:
      Right = FrameValue::taint(true, true);
      break;
    }
    for (NdOp Opcode : {NdOp::INT_SUB, NdOp::INT_EQUAL, NdOp::INT_NOTEQUAL}) {
      const auto Result = evaluate(Left, Right, operation(Opcode));
      EXPECT_FALSE(Result.Constant);
      EXPECT_TRUE(Result.MayBeFrame);
    }
  }
}

TEST(RegistrationFrameScalar, PartialWidthsAndOrderingAreNotRelativeProofs) {
  for (NdOp Opcode : {NdOp::INT_SUB, NdOp::INT_EQUAL, NdOp::INT_NOTEQUAL})
    for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
      SCOPED_TRACE(Mutation);
      LowOp Op = operation(Opcode);
      if (Mutation < 6)
        Op.Inputs[Mutation / 3].Size = Mutation % 3 == 0   ? 1
                                       : Mutation % 3 == 1 ? 2
                                                           : 8;
      else if (Mutation == 6)
        Op.Output.Size = Opcode == NdOp::INT_SUB ? 1 : 4;
      else if (Mutation == 7)
        Op.Output.Size = 8;
      else
        Op.Opcode = NdOp::INT_LESS;
      const auto Result =
          evaluate(FrameValue::frame(-16), FrameValue::frame(-16), Op);
      EXPECT_FALSE(Result.Constant);
      EXPECT_TRUE(Result.MayBeFrame);
    }
}

TEST(RegistrationFrameScalar, ReloadedChainComparisonIsAnExactScalar) {
  FrameState Frame;
  Frame.store(-36, 4, FrameValue::frame(-16));
  Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride] =
      FrameValue::frame(-36);
  FrameTransfer Transfer(Frame, -16);
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.NumInputs = 1;
  Load.Output = NdVar::reg(x86reg::RAX, 4);
  Load.Inputs[0] = NdVar::cst(0, 4);
  Load.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  Transfer.write(Load, Transfer.evaluate(Load, true));
  Load.Output = Load.Inputs[0] = NdVar::reg(x86reg::RCX, 4);
  Load.MemoryAddressSpace = NdMemoryAddressSpace::Default;
  Transfer.write(Load, Transfer.evaluate(Load, true));
  const LowOp Difference = operation(NdOp::INT_SUB);
  Transfer.write(Difference, Transfer.evaluate(Difference, true));
  LowOp Equal = operation(NdOp::INT_EQUAL);
  Equal.Inputs[0] = Difference.Output;
  Equal.Inputs[1] = NdVar::cst(0, 4);
  EXPECT_EQ(Transfer.evaluate(Equal, true), FrameValue::constant(1));

  Frame.store(-35, 1, {});
  Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride] =
      FrameValue::frame(-36);
  Transfer.write(Load, Transfer.evaluate(Load, true));
  const auto Partial = Transfer.evaluate(Difference, true);
  EXPECT_FALSE(Partial.Constant);
  EXPECT_TRUE(Partial.MayBeFrame);
}

TEST(RegistrationFrameScalar, CookieSuccessKeepsRegistersAndEqualCompareFlags) {
  FrameState Frame;
  for (unsigned I = 0; I != Frame.Registers.size(); ++I)
    Frame.Registers[I] = FrameValue::constant(0x12340000 + I);
  auto &Cookie = Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride];
  Cookie = {};
  Cookie.SecurityCookie = true;
  for (uint64_t Flag :
       {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF})
    Frame.OtherRegisterBytes[Flag] =
        FrameValue::constant(Flag != x86reg::PF && Flag != x86reg::ZF);
  Frame.OtherRegisterBytes[x86reg::DF] = FrameValue::constant(1);
  Frame.OtherRegisterBytes[x86reg::VectorBase] = FrameValue::constant(0x57);
  const auto Registers = Frame.Registers;
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Output = NdVar::reg(x86reg::RAX, 4);
  Call.NumInputs = 1;
  Call.Inputs[0] = NdVar::codeAddress(0x2000, 4);
  FrameTransfer Transfer(Frame, -12);
  ASSERT_TRUE(Transfer.isCookieCheck(Call, 0x2000));
  Transfer.write(Call, {}, 0x2000);
  EXPECT_EQ(Frame.Registers, Registers);
  for (uint64_t Flag :
       {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF})
    EXPECT_EQ(Frame.OtherRegisterBytes[Flag],
              FrameValue::constant(Flag == x86reg::PF || Flag == x86reg::ZF));
  EXPECT_EQ(Frame.OtherRegisterBytes[x86reg::DF], FrameValue::constant(1));
  EXPECT_EQ(Frame.OtherRegisterBytes[x86reg::VectorBase],
            FrameValue::constant(0x57));
  EXPECT_FALSE(Frame.OtherRegistersMayBeFrame);
}
} // namespace
