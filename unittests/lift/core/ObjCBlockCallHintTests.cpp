#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCBlockCallHints.h"

using namespace neverd;

namespace {
LowOp op(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs,
         va_t Address) {
  LowOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  Result.Addr = Address;
  for (const auto &V : Inputs)
    Result.addInput(V);
  return Result;
}

struct Fixture {
  BinaryImage Image;
  LowFunc Low;
  SourceFunctionTypeHint Entry;
  uint64_t R0, R1, R2, R3, Target;

  explicit Fixture(Arch Architecture) {
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::MachO;
    const auto &TRI = getTargetRegInfo(Architecture);
    R0 = TRI.IntParamRegs[0];
    R1 = TRI.IntParamRegs[1];
    R2 = TRI.IntParamRegs[2];
    R3 = TRI.IntParamRegs[3];
    Target = Architecture == Arch::AArch64 ? 64 : 0;
    auto Pointer = NdType::makePtr(NdType::makeVoid());
    Entry.ReturnType = NdType::makeInt(4);
    Entry.Parameters = {{"self", Pointer},
                        {"cmd", Pointer},
                        {"block", Pointer},
                        {"value", NdType::makeInt(4)}};
    std::string Error;
    EXPECT_TRUE(assignDarwinObjCSourceABI(Entry, Architecture, Error)) << Error;
    Low.Name = "arbitrary_method_identity";
    Low.Entry = 0x1000;
    Low.Blocks.emplace_back();
    auto &B = Low.Blocks[0];
    B.Id = 0;
    B.StartAddr = 0x1000;
    B.EndAddr = 0x1020;
    B.Ops = {op(NdOp::COPY, NdVar::reg(R1, 8), {NdVar::reg(R3, 8)}, 0x1000),
             op(NdOp::COPY, NdVar::reg(R0, 8), {NdVar::reg(R2, 8)}, 0x1004),
             op(NdOp::INT_ADD, NdVar::tmp(0, 8),
                {NdVar::reg(R2, 8), NdVar::cst(16, 8)}, 0x1008),
             op(NdOp::LOAD, NdVar::reg(Target, 8), {NdVar::tmp(0, 8)}, 0x1008),
             op(NdOp::INDIR_CALL, NdVar::reg(TRI.IntReturnReg, 8),
                {NdVar::reg(Target, 8)}, 0x100c),
             op(NdOp::RETURN, {}, {NdVar::reg(TRI.IntReturnReg, 4)}, 0x1010)};
  }
  std::map<va_t, SourceCallTypeHint> hints() const {
    return buildObjCBlockCallHints(Image, Low, &Entry);
  }
};

TEST(ObjCBlockCallHints, ProvesSameReceiverAndKeepsOnlyWrittenScalarArguments) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F(Architecture);
    const auto Hints = F.hints();
    ASSERT_EQ(Hints.size(), 1U);
    const auto &Hint = Hints.at(0x100c);
    EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::BlockInvoke);
    EXPECT_EQ(Hint.Signature.Origin,
              SourceFunctionTypeHint::OriginKind::NativeAnalysis);
    ASSERT_EQ(Hint.Signature.Parameters.size(), 2U);
    EXPECT_EQ(Hint.Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Hint.Signature.Parameters[1].Type->Size, 4U);
    EXPECT_EQ(Hint.Signature.Parameters[1].Location.RegisterOffset, F.R1);
    EXPECT_EQ(Hint.Signature.ReturnType->Size, 4U);
    EXPECT_EQ(Hint.TargetAddress, 0U);
  }
}

TEST(ObjCBlockCallHints,
     RejectsDifferentReceiverWrongOffsetWidthAndOrderedLoad) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
      Fixture F(Architecture);
      auto &Ops = F.Low.Blocks[0].Ops;
      if (Mutation == 0)
        Ops[1].Inputs[0] = NdVar::reg(F.R3, 8);
      if (Mutation == 1)
        Ops[2].Inputs[1] = NdVar::cst(8, 8);
      if (Mutation == 2)
        Ops[3].Output.Size = 4;
      if (Mutation == 3)
        Ops[3].MemoryOrdering = NdMemoryOrdering::Acquire;
      EXPECT_TRUE(F.hints().empty()) << int(Architecture) << ":" << Mutation;
    }
  }
}

TEST(ObjCBlockCallHints,
     InterveningMemoryOrCallsInvalidateLoadedTargetIdentity) {
  for (unsigned Mutation = 0; Mutation < 2; ++Mutation) {
    Fixture F(Arch::AArch64);
    auto &Ops = F.Low.Blocks[0].Ops;
    auto Invalidate = Mutation == 0
                          ? op(NdOp::STORE, {},
                               {NdVar::reg(F.R2, 8), NdVar::cst(0, 8)}, 0x100a)
                          : op(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)}, 0x100a);
    Ops.insert(Ops.begin() + 4, Invalidate);
    EXPECT_TRUE(F.hints().empty()) << Mutation;
  }
}

TEST(ObjCBlockCallHints, RejectsUnmodelledFloatingBankGapsAndControlFlow) {
  for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
    Fixture F(Arch::AArch64);
    auto &Ops = F.Low.Blocks[0].Ops;
    if (Mutation == 0)
      Ops.insert(
          Ops.begin(),
          op(NdOp::COPY,
             NdVar::reg(getTargetRegInfo(F.Image.Arch).FPParamRegs[0], 8),
             {NdVar::cst(0, 8)}, 0xff0));
    if (Mutation == 1)
      Ops.insert(Ops.begin(), op(NdOp::COPY, NdVar::reg(F.R3, 8),
                                 {NdVar::cst(3, 8)}, 0xff0));
    if (Mutation == 2)
      F.Low.Blocks[0].Succs.push_back(0);
    EXPECT_TRUE(F.hints().empty()) << Mutation;
  }
}

TEST(ObjCBlockCallHints, TypedEntryDoesNotMakeAnUnwrittenRegisterAnArgument) {
  Fixture F(Arch::AArch64);
  F.Low.Blocks[0].Ops.erase(F.Low.Blocks[0].Ops.begin() + 1);
  EXPECT_TRUE(F.hints().empty());
  F.Entry.HasExplicitABI = false;
  EXPECT_TRUE(F.hints().empty());
}

TEST(ObjCBlockCallHints, ExactPrivateSpillsPreserveHiddenObjectAndNarrowValue) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F(Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    auto &Ops = F.Low.Blocks[0].Ops;
    Ops.erase(Ops.begin(), Ops.begin() + 2);
    Ops[0].Inputs[0] = NdVar::reg(F.R0, 8);
    const std::vector<LowOp> Prefix = {
        op(NdOp::INT_ADD, NdVar::tmp(0, 8),
           {NdVar::reg(TRI.StackPointer, 8), NdVar::cst(-16, 8)}, 0xff0),
        op(NdOp::STORE, {}, {NdVar::tmp(0, 8), NdVar::reg(F.R0, 8)}, 0xff0),
        op(NdOp::INT_ADD, NdVar::tmp(0, 8),
           {NdVar::reg(TRI.StackPointer, 8), NdVar::cst(-4, 8)}, 0xff4),
        op(NdOp::STORE, {}, {NdVar::tmp(0, 8), NdVar::reg(F.R1, 4)}, 0xff4),
        op(NdOp::INT_ADD, NdVar::tmp(0, 8),
           {NdVar::reg(TRI.StackPointer, 8), NdVar::cst(-16, 8)}, 0xff8),
        op(NdOp::LOAD, NdVar::reg(F.R0, 8), {NdVar::tmp(0, 8)}, 0xff8),
        op(NdOp::INT_ADD, NdVar::tmp(0, 8),
           {NdVar::reg(TRI.StackPointer, 8), NdVar::cst(-4, 8)}, 0xffc),
        op(NdOp::LOAD, NdVar::reg(F.R1, 4), {NdVar::tmp(0, 8)}, 0xffc),
        op(NdOp::INT_ZEXT, NdVar::reg(F.R1, 8), {NdVar::reg(F.R1, 4)}, 0xffc)};
    Ops.insert(Ops.begin(), Prefix.begin(), Prefix.end());
    auto Hints = buildObjCBlockCallHints(F.Image, F.Low);
    ASSERT_EQ(Hints.size(), 1U) << int(Architecture);
    EXPECT_EQ(Hints.begin()->second.Signature.Parameters[1].Type->Size, 4U);
    Ops.insert(Ops.begin() + 4,
               op(NdOp::STORE, {},
                  {NdVar::reg(F.R2, 8), NdVar::reg(TRI.StackPointer, 8)},
                  0xff6));
    EXPECT_TRUE(buildObjCBlockCallHints(F.Image, F.Low).empty());
  }
}

TEST(ObjCBlockCallHints, ForwardedA64ReturnKeepsFullMachineCarrier) {
  Fixture F(Arch::AArch64);
  F.Low.Blocks[0].Ops.back().Inputs[0] =
      NdVar::reg(240, 8); // LR, RET's branch target.
  auto Hints = F.hints();
  ASSERT_EQ(Hints.size(), 1U);
  EXPECT_EQ(Hints.begin()->second.Signature.ReturnType->Size, 8U);
}

TEST(ObjCBlockCallHints,
     LaterWideReadBeforeDefinitionPreventsFalseNarrowReturn) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F(Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    auto &Ops = F.Low.Blocks[0].Ops;
    Ops.insert(Ops.end() - 1, op(NdOp::COPY, NdVar::tmp(0, 4),
                                 {NdVar::reg(TRI.IntReturnReg, 4)}, 0x100e));
    Ops.back().Inputs[0].Size = 8;
    auto Hints = F.hints();
    ASSERT_EQ(Hints.size(), 1U);
    EXPECT_EQ(Hints.begin()->second.Signature.ReturnType->Size, 8U);
    Ops.insert(Ops.end() - 1, op(NdOp::COPY, NdVar::tmp(0, 8),
                                 {NdVar::reg(TRI.FPReturnReg, 8)}, 0x100f));
    EXPECT_TRUE(F.hints().empty());
    Ops.insert(Ops.end() - 2, op(NdOp::COPY, NdVar::reg(TRI.IntReturnReg, 8),
                                 {NdVar::cst(0, 8)}, 0x100e));
    EXPECT_TRUE(
        F.hints().empty()); // Overwriting the GPR does not kill the FP result.
  }
}

TEST(ObjCBlockCallHints, SourceOnlyPreSSAArgumentsAndReturnReachHighIR) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F(Architecture);
    std::map<va_t, SourceFunctionTypeHint> Hints{{F.Low.Entry, F.Entry}};
    LowToMedConverter Converter;
    Converter.setBinaryImage(&F.Image);
    Converter.setSourceCallHintsEnabled(true);
    Converter.setSourceCalleeTypeHints(&Hints);
    auto Med = Converter.convert(F.Low, Architecture, BinaryFormat::MachO);
    recoverCallAbi(Med, Architecture, {}, &F.Image);
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    ASSERT_TRUE(Med.CallInfos[0].SourceCallHint);
    ASSERT_EQ(Med.CallInfos[0].Args.size(), 2U);
    EXPECT_EQ(Med.CallInfos[0].Args[1].Size, 4U);
    auto High = MedToHighConverter().convert(Med, Architecture);
    unsigned Calls = 0;
    walkStmts(High.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &Root) {
        std::vector<const HighExpr *> Work{Root.get()};
        while (!Work.empty()) {
          const auto *E = Work.back();
          Work.pop_back();
          if (!E)
            continue;
          if (E->SourceCallHint) {
            ++Calls;
            EXPECT_EQ(E->Operands.size(), 2U);
            EXPECT_EQ(E->Type->Size, 4U);
          }
          for (const auto &Operand : E->Operands)
            Work.push_back(Operand.get());
        }
      });
    });
    EXPECT_GT(Calls, 0U);
    Converter.setSourceCallHintsEnabled(false);
    auto Disabled = Converter.convert(F.Low, Architecture, BinaryFormat::MachO);
    for (const auto &B : Disabled.Blocks)
      for (const auto &Operation : B.Ops)
        EXPECT_FALSE(Operation.SourceCallHint);
  }
}
} // namespace
