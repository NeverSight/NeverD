//===- MedCallingConvX86StackTests.cpp - i386 stack parameter recovery
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stack-address forwarding and independent cyclic offset queries.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedCallingConvDetail.h"
#include "neverd/ir/med/MedIR.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace {

using namespace neverd;
using med_calling_conv_detail::detectCdeclStackParams;

MedVar temp(int Id, int SSAVer, uint16_t Size, Arch TheArch) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.SSAVer = SSAVer;
  V.Size = Size;
  V.TheArch = TheArch;
  return V;
}

MedVar reg(int Id, int SSAVer, uint16_t Size, uint64_t RegOff, Arch TheArch) {
  auto V = temp(Id, SSAVer, Size, TheArch);
  V.Kind = MedVar::Reg;
  V.RegOff = RegOff;
  return V;
}

MedOp unary(NdOp Opcode, MedVar Output, MedVar Input) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.addInput(Input);
  return Op;
}

MedOp binary(NdOp Opcode, MedVar Output, MedVar Left, MedVar Right) {
  auto Op = unary(Opcode, Output, Left);
  Op.addInput(Right);
  return Op;
}

TEST(MedCallingConvX86Stack, ForwardedAddressesRequireAProvenOutput) {
  const auto &TRI = getTargetRegInfo(Arch::X86);
  for (auto Opcode : {NdOp::COPY, NdOp::INT_ADD, NdOp::INT_SUB, NdOp::SUBBYTES,
                      NdOp::INT_ZEXT, NdOp::INT_SEXT, NdOp::INT_XOR})
    for (bool RegisterOutput : {false, true})
      for (bool EarlierOpaqueDefinition : {false, true}) {
        SCOPED_TRACE(testing::Message() << int(Opcode) << ':' << RegisterOutput
                                        << ':' << EarlierOpaqueDefinition);
        MedFunc F;
        F.Blocks.resize(1);
        auto &B = F.Blocks[0];
        const auto SP = reg(1, 0, 4, TRI.StackPointer, Arch::X86);
        B.Ops.push_back(unary(NdOp::COPY, SP, SP));
        const auto Address = temp(2, 0, 4, Arch::X86);
        B.Ops.push_back(
            binary(NdOp::INT_ADD, Address, SP, MedVar::makeConst(4, 4)));
        B.Ops.push_back(unary(NdOp::LOAD, temp(3, 0, 4, Arch::X86), Address));
        const unsigned Bytes =
            Opcode == NdOp::INT_ZEXT || Opcode == NdOp::INT_SEXT ? 8 : 4;
        const auto Output = RegisterOutput
                                ? reg(4, 0, Bytes, TRI.IntReturnReg, Arch::X86)
                                : temp(4, 0, Bytes, Arch::X86);
        if (EarlierOpaqueDefinition)
          B.Ops.push_back(
              unary(NdOp::COPY, Output, MedVar::makeConst(0, Bytes)));
        auto Forward = unary(Opcode, Output, Address);
        if (Opcode == NdOp::INT_ADD || Opcode == NdOp::INT_SUB ||
            Opcode == NdOp::SUBBYTES || Opcode == NdOp::INT_XOR)
          Forward.addInput(MedVar::makeConst(0, 4));
        B.Ops.push_back(Forward);

        detectCdeclStackParams(F, Arch::X86, BinaryFormat::ELF);

        ASSERT_EQ(F.Params.size(), 1u);
        const bool Escapes = RegisterOutput || EarlierOpaqueDefinition ||
                             Opcode == NdOp::INT_XOR;
        if (Escapes) {
          EXPECT_EQ(F.MutableStackParamHomes,
                    (std::vector<std::pair<int, int64_t>>{{0, 4}}));
          EXPECT_EQ(B.Ops[2].Opcode, NdOp::LOAD);
        } else {
          EXPECT_TRUE(F.MutableStackParamHomes.empty());
          EXPECT_EQ(B.Ops[2].Opcode, NdOp::COPY);
          EXPECT_EQ(B.Ops[2].Inputs[0].Kind, MedVar::Param);
          EXPECT_EQ(B.Ops[2].Inputs[0].Id, 0);
        }
      }
}

TEST(MedCallingConvX86Stack, PhiCycleOffsetsRemainSpecificToEachQueryRoot) {
  const auto &TRI = getTargetRegInfo(Arch::X86);
  for (bool ReverseBlocks : {false, true}) {
    SCOPED_TRACE(ReverseBlocks);
    MedFunc F;
    F.Blocks.resize(2);
    auto &Entry = F.Blocks[0];
    Entry.Id = 0;
    const auto SP = reg(1, 0, 4, TRI.StackPointer, Arch::X86);
    Entry.Ops.push_back(unary(NdOp::COPY, SP, SP));
    const auto First = temp(4, 0, 4, Arch::X86);
    const auto Second = temp(5, 0, 4, Arch::X86);
    Entry.Ops.push_back(
        binary(NdOp::INT_ADD, First, SP, MedVar::makeConst(4, 4)));
    Entry.Ops.push_back(
        binary(NdOp::INT_ADD, Second, SP, MedVar::makeConst(8, 4)));
    auto &Body = F.Blocks[1];
    Body.Id = 1;
    const auto A = temp(2, 0, 4, Arch::X86);
    const auto B = temp(2, 1, 4, Arch::X86);
    PhiNode PhiA;
    PhiA.Output = A;
    PhiA.Args = {{1, B}, {0, First}};
    PhiNode PhiB;
    PhiB.Output = B;
    PhiB.Args = {{1, A}, {0, Second}};
    Body.Phis = {PhiA, PhiB};
    Body.Ops.push_back(unary(NdOp::LOAD, temp(6, 0, 4, Arch::X86), A));
    Body.Ops.push_back(unary(NdOp::LOAD, temp(7, 0, 4, Arch::X86), B));
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.addInput(MedVar::makeConst(0x2000, 4));
    Call.addInput(A);
    Call.addInput(B);
    Body.Ops.push_back(Call);
    if (ReverseBlocks)
      std::reverse(F.Blocks.begin(), F.Blocks.end());

    detectCdeclStackParams(F, Arch::X86, BinaryFormat::ELF);

    ASSERT_EQ(F.Params.size(), 2u);
    // Each root reaches the other PHI's outside arm after cutting its own
    // re-entry. Both passed addresses escape, so both homes remain writable.
    EXPECT_EQ(F.MutableStackParamHomes,
              (std::vector<std::pair<int, int64_t>>{{0, 4}, {1, 8}}));
    const auto &Result = F.Blocks[ReverseBlocks ? 0 : 1];
    EXPECT_EQ(Result.Ops[0].Opcode, NdOp::LOAD);
    EXPECT_EQ(Result.Ops[1].Opcode, NdOp::LOAD);
  }
}

} // namespace
