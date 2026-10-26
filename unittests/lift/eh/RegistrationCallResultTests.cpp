//===- RegistrationCallResultTests.cpp - PE32 result observations
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Classify exact source observations and reject malformed result transports.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86RegistrationCallResult.h"

using namespace neverd;

namespace {
MedVar value(int Id, uint16_t Size = 4) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = Size;
  V.SSAVer = 1;
  return V;
}

class RegistrationCallResult : public testing::Test {
protected:
  MedFunc Function;
  RegistrationCallFrameEffect Call;

  void SetUp() override {
    Call.Address = 0x401100;
    Call.Target = 0x401700;
    Call.OpSeq = 7;
    Function.Blocks.emplace_back();
    auto &Block = Function.Blocks.back();
    Block.Id = 0;
    MedOp Root;
    Root.Opcode = NdOp::CALL;
    Root.Addr = Call.Address;
    Root.OriginSeq = Call.OpSeq;
    Root.CallSiteId = 3;
    Root.Output = value(1);
    Root.addInput(MedVar::makeConst(Call.Target, 4));
    Block.Ops.push_back(Root);
  }

  void use(NdOp Opcode, MedVar Input, MedVar Output = {}) {
    MedOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    Op.addInput(Input);
    Function.Blocks[0].Ops.push_back(Op);
  }

  bool unused() {
    size_t Work = 0;
    return hasUnusedX86RegistrationCallResult(Function, Call, Work);
  }
};

TEST_F(RegistrationCallResult, AcceptsOnlyUnobservedSSATransports) {
  use(NdOp::COPY, value(1), value(2));
  use(NdOp::INT_ZEXT, value(2), value(3, 8));
  Function.Blocks[0].Phis.push_back({value(4, 8), {{0, value(3, 8)}}, {}});
  EXPECT_TRUE(unused());
  use(NdOp::RETURN, value(4, 8));
  EXPECT_FALSE(unused());
}

TEST_F(RegistrationCallResult, KeepsDirectAndIndirectOccurrencesDistinct) {
  auto &Root = Function.Blocks.front().Ops.front();
  Root.Opcode = NdOp::INDIR_CALL;
  EXPECT_FALSE(unused());
  size_t Work = 0;
  EXPECT_TRUE(hasUnusedX86RegistrationCallResult(Function, Call, Work, true));
  Root.Inputs[0] = value(5);
  Work = 0;
  EXPECT_FALSE(hasUnusedX86RegistrationCallResult(Function, Call, Work, true));
  Root.Inputs[0] = MedVar::makeConst(Call.Target, 4);
  use(NdOp::RETURN, value(1));
  Work = 0;
  EXPECT_FALSE(hasUnusedX86RegistrationCallResult(Function, Call, Work, true));
}

TEST_F(RegistrationCallResult, DoesNotConfuseSharedSourceProvenanceWithACall) {
  use(NdOp::COPY, MedVar::makeConst(0, 4), value(2));
  Function.Blocks[0].Ops.back().Addr = Call.Address;
  Function.Blocks[0].Ops.back().OriginSeq = Call.OpSeq;
  EXPECT_TRUE(unused());
  Function.Blocks[0].Ops.push_back(Function.Blocks[0].Ops.front());
  EXPECT_FALSE(unused());
}

TEST_F(RegistrationCallResult, RejectsObservableOperations) {
  for (auto Opcode :
       {NdOp::STORE, NdOp::RETURN, NdOp::BRANCH, NdOp::CALL, NdOp::INT_ADD}) {
    use(Opcode, value(1), value(2));
    EXPECT_FALSE(unused()) << unsigned(Opcode);
    Function.Blocks[0].Ops.pop_back();
  }
}

TEST_F(RegistrationCallResult, RejectsImplicitCallArgumentsAndPreservedInputs) {
  MedCallInfo Info;
  Info.Args.push_back(value(1));
  Function.CallInfos.push_back(Info);
  EXPECT_FALSE(unused());
  Function.CallInfos.clear();
  Function.CallClobbers.push_back({value(2), 4, value(1), 2});
  EXPECT_FALSE(unused());
  Function.CallClobbers.clear();
  Function.StructReturnCandidates.push_back({3, {value(1)}});
  EXPECT_FALSE(unused());
}

TEST_F(RegistrationCallResult, RejectsAbsentOrAmbiguousDefinitions) {
  auto &Root = Function.Blocks[0].Ops.front();
  Root.Output.Size = 0;
  EXPECT_FALSE(unused());
  Root.Output = value(1);
  Function.CallClobbers.push_back({value(1), 3, {}, 0});
  EXPECT_FALSE(unused());
  Function.CallClobbers.clear();
  use(NdOp::COPY, MedVar::makeConst(0, 4), value(1));
  EXPECT_FALSE(unused());
}

TEST_F(RegistrationCallResult, RejectsInconsistentWidths) {
  use(NdOp::COPY, value(1, 2), value(2, 2));
  EXPECT_FALSE(unused());
  Function.Blocks[0].Ops.pop_back();
  use(NdOp::COPY, value(1), value(2, 8));
  EXPECT_FALSE(unused());
  Function.Blocks[0].Ops.pop_back();
  Function.Blocks[0].Phis.push_back({value(2), {}, value(1, 2)});
  EXPECT_FALSE(unused());
}

TEST_F(RegistrationCallResult, RejectsUnprovedOccurrencesAndExhaustedWork) {
  Function.SkippedSSA = true;
  EXPECT_FALSE(unused());
  Function.SkippedSSA = false;
  ++Call.Target;
  EXPECT_FALSE(unused());
  --Call.Target;
  Call.DoesNotReturn = true;
  EXPECT_FALSE(unused());
  Call.DoesNotReturn = false;
  size_t Work = limits::kMaxRegistrationEHStateWork;
  EXPECT_FALSE(hasUnusedX86RegistrationCallResult(Function, Call, Work));
}

TEST_F(RegistrationCallResult,
       ClassifiesObservationsWithoutAdmittingMalformedSSA) {
  using Use = X86RegistrationCallResultUse;
  use(NdOp::COPY, value(1), value(2));
  size_t Work = 0;
  EXPECT_EQ(getX86RegistrationCallResultUse(Function, Call, Work), Use::Unused);
  use(NdOp::RETURN, value(2));
  Work = 0;
  EXPECT_EQ(getX86RegistrationCallResultUse(Function, Call, Work),
            Use::Observed);
  EXPECT_FALSE(unused());
  const size_t Required = Work;
  Work = limits::kMaxRegistrationEHStateWork - Required;
  EXPECT_EQ(getX86RegistrationCallResultUse(Function, Call, Work),
            Use::Observed);
  EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
  Work = limits::kMaxRegistrationEHStateWork - Required + 1;
  EXPECT_FALSE(getX86RegistrationCallResultUse(Function, Call, Work));
  const auto Original = Function;
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    Function = Original;
    auto &Ops = Function.Blocks.front().Ops;
    if (Mutation == 0)
      Ops.push_back(Ops.front());
    if (Mutation == 1)
      Ops[1].Output.Size = 8;
    if (Mutation == 2)
      Ops.back().Inputs[0].Size = 2;
    if (Mutation == 3)
      Function.SkippedSSA = true;
    if (Mutation == 4)
      Ops[1].Output.SSAVer = -1;
    if (Mutation == 5)
      Ops.front().DoesNotReturn = true;
    if (Mutation == 6)
      Ops[1].Opcode = NdOp::INT_ZEXT;
    Work = 0;
    EXPECT_FALSE(getX86RegistrationCallResultUse(Function, Call, Work));
  }
}

TEST_F(RegistrationCallResult, ClassifiesImplicitObservationsWithExactWidths) {
  using Use = X86RegistrationCallResultUse;
  const auto Original = Function;
  for (unsigned Kind = 0; Kind != 3; ++Kind)
    for (bool Partial : {false, true}) {
      SCOPED_TRACE(Kind);
      SCOPED_TRACE(Partial);
      Function = Original;
      const auto Input = value(1, Partial ? 2 : 4);
      if (Kind == 0) {
        MedCallInfo Info;
        Info.Args.push_back(Input);
        Function.CallInfos.push_back(Info);
      }
      if (Kind == 1)
        Function.CallClobbers.push_back({value(2), 4, Input, 2});
      if (Kind == 2)
        Function.StructReturnCandidates.push_back({3, {Input}});
      size_t Work = 0;
      const auto Result = getX86RegistrationCallResultUse(Function, Call, Work);
      if (Partial)
        EXPECT_FALSE(Result);
      else
        EXPECT_EQ(Result, Use::Observed);
      EXPECT_FALSE(unused());
    }
}

} // namespace
