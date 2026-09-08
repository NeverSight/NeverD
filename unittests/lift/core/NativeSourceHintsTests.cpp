#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/pipeline/NativeSourceHints.h"
#include "neverd/pipeline/Pipeline.h"

using namespace neverd;

namespace {
struct NativeFixture {
  BinaryImage Image;
  MedFunc Med;
  HighFunc High;
  PipelineFunctionAudit Audit;

  explicit NativeFixture(Arch Architecture = Arch::AArch64) {
    Image.Arch = Architecture;
    Image.Format = BinaryFormat::MachO;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 0x100;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x100);
    Image.Segments.push_back(std::move(Text));
    const auto &TRI = getTargetRegInfo(Architecture);
    Med.Entry = High.Entry = Audit.Entry = 0x1000;
    Med.ReturnType = High.ReturnType = NdType::makeInt(4);
    Med.Blocks.emplace_back();
    Med.Blocks[0].Id = 0;
    for (unsigned Index = 0; Index < 2; ++Index) {
      MedVar Parameter;
      Parameter.Kind = MedVar::Param;
      Parameter.Id = Index;
      Parameter.RegOff = TRI.IntParamRegs[Index];
      Parameter.Size = 4;
      Parameter.TheArch = Architecture;
      Med.Params.push_back(Parameter);
      Med.TypedParams.push_back(
          {"arg" + std::to_string(Index), NdType::makeInt(4)});
      High.Params.push_back(
          {"arg" + std::to_string(Index), NdType::makeInt(4)});
    }
    MedOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Output.Kind = MedVar::Reg;
    Add.Output.Id = 10;
    Add.Output.SSAVer = 1;
    Add.Output.TheArch = Architecture;
    Add.Output.RegOff = TRI.IntReturnReg;
    Add.Output.Size = 4;
    Add.addInput(Med.Params[0]);
    Add.addInput(Med.Params[1]);
    Med.Blocks[0].Ops.push_back(Add);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Med.Blocks[0].Ops.push_back(Return);
    HighStmt Statement;
    Statement.Kind = StmtKind::Return;
    Statement.RetVal = HighExpr::makeConst(42, 4);
    High.Body.push_back(std::move(Statement));
    Audit.Disposition = PipelineFunctionDisposition::Accepted;
    Audit.HasLowIR = Audit.HasMedIR = Audit.MedIRVerified = true;
    Audit.DecodedInstructions = Audit.LiftedInstructions = 2;
  }

  std::optional<SourceFunctionTypeHint> infer(std::string &Error) const {
    return inferNativeSourceTypeHint(Image, Med, High, Audit, Error);
  }
};

TEST(NativeSourceHints, KeepsObservedIntegerLocationsWithoutUsingNames) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    Fixture.Med.Name = "unrelated_stripped_symbol";
    std::string Error;
    auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::NativeAnalysis);
    ASSERT_EQ(Hint->Parameters.size(), 2U);
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, TRI.IntParamRegs[0]);
    EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, TRI.IntParamRegs[1]);
    EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, TRI.IntReturnReg);
    EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 4U);
    EXPECT_EQ(Fixture.Med.ReturnValueEvidence, MedReturnValueEvidence::Unknown);
  }
}

TEST(NativeSourceHints, OmittedUnusedArgumentDoesNotShiftPhysicalRegister) {
  NativeFixture Fixture;
  Fixture.Med.Params[0].Id = -1;
  Fixture.Med.Blocks[0].Ops[0].Inputs[0] = MedVar::makeConst(3, 4);
  std::string Error;
  auto Hint = Fixture.infer(Error);
  ASSERT_TRUE(Hint) << Error;
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset,
            getTargetRegInfo(Arch::AArch64).IntParamRegs[1]);
}

TEST(NativeSourceHints, ConstantFunctionNeedsNoInventedArguments) {
  NativeFixture Fixture;
  Fixture.Med.Params.clear();
  Fixture.Med.TypedParams.clear();
  Fixture.High.Params.clear();
  auto &Op = Fixture.Med.Blocks[0].Ops[0];
  Op.Opcode = NdOp::COPY;
  Op.NumInputs = 1;
  Op.Inputs[0] = MedVar::makeConst(42, 4);
  std::string Error;
  auto Hint = Fixture.infer(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_TRUE(Hint->Parameters.empty());
}

TEST(NativeSourceHints, RejectsIncompleteAuditAndMismatchedFunctionIdentity) {
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    NativeFixture Fixture;
    if (Mutation == 0)
      Fixture.Audit.TruncatedPaths.push_back(0x1004);
    if (Mutation == 1)
      --Fixture.Audit.LiftedInstructions;
    if (Mutation == 2)
      Fixture.Audit.MedIRVerified = false;
    if (Mutation == 3)
      Fixture.High.Entry += 4;
    if (Mutation == 4)
      Fixture.Image.IsRelocatable = true;
    if (Mutation == 5)
      Fixture.Image.Segments[0].Flags = SegmentFlags::Readable;
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
    EXPECT_FALSE(Error.empty());
  }
}

TEST(NativeSourceHints, RejectsRegisterBankTypeWidthAndDuplicateAmbiguity) {
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    NativeFixture Fixture;
    if (Mutation == 0)
      Fixture.Med.Params[0].RegOff =
          getTargetRegInfo(Arch::AArch64).FPParamRegs[0];
    if (Mutation == 1)
      Fixture.Med.TypedParams[0].Type = NdType::makeFloat(4);
    if (Mutation == 2)
      Fixture.Med.Params[0].Size = 8;
    if (Mutation == 3)
      Fixture.Med.Params[1].RegOff = Fixture.Med.Params[0].RegOff;
    if (Mutation == 4)
      Fixture.High.ReturnType = NdType::makeInt(8);
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
  }
}

TEST(NativeSourceHints, UnknownCallsCannotInventResultOrArgumentTypes) {
  NativeFixture Fixture;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(MedVar::makeConst(0x1020, 8));
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Call);
  std::string Error;
  EXPECT_FALSE(Fixture.infer(Error));
  EXPECT_NE(Error.find("without a source binding"), std::string::npos);
}

TEST(NativeSourceHints, EachReturnRequiresComputedCarrierNotStaleLiveIn) {
  for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
    NativeFixture Fixture;
    auto &Op = Fixture.Med.Blocks[0].Ops[0];
    if (Mutation == 0) {
      Op.Opcode = NdOp::COPY;
      Op.NumInputs = 1;
      Op.Inputs[0] = Op.Output;
    }
    if (Mutation == 1)
      Op.Output.Size = 2;
    if (Mutation == 2) {
      MedBlock Other;
      Other.Id = 1;
      Other.Ops.push_back(Fixture.Med.Blocks[0].Ops.back());
      Fixture.Med.Blocks.push_back(Other);
    }
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
  }
}

TEST(NativeSourceHints, KeepsCompleteStackSlotAndRejectsPackedSlices) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    auto &Parameter = Fixture.Med.Params[1];
    Parameter.RegOff = kNoParamReg;
    Parameter.Id = getTargetRegInfo(Architecture).IntParamRegs.size() + 2;
    Parameter.Size = 8;
    Fixture.Med.TypedParams[1].Type = NdType::makeInt(8);
    Fixture.Med.Blocks[0].Ops[0].Inputs[1] = Parameter;
    std::string Error;
    auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->Parameters[1].Location.EntryStackOffset,
              Architecture == Arch::X64 ? 24 : 16);
    auto &Op = Fixture.Med.Blocks[0].Ops[0];
    Op.Opcode = NdOp::SUBBYTES;
    Op.Inputs[0] = Parameter;
    Op.Inputs[1] = MedVar::makeConst(4, 4);
    EXPECT_FALSE(Fixture.infer(Error));
    EXPECT_NE(Error.find("partial"), std::string::npos);
  }
}

TEST(NativeSourceHints,
     GenericScalarAllocatorDoesNotAddObjectiveCHiddenValues) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SourceFunctionTypeHint Hint;
    Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Hint.ReturnType = NdType::makeFloat(8);
    Hint.Parameters = {{"integer", NdType::makeInt(4)},
                       {"floating", NdType::makeFloat(8)}};
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error))
        << Error;
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Hint.Parameters[0].Location.RegisterOffset, TRI.IntParamRegs[0]);
    EXPECT_EQ(Hint.Parameters[1].Location.RegisterOffset, TRI.FPParamRegs[0]);
    EXPECT_FALSE(assignDarwinObjCSourceABI(Hint, Architecture, Error));
  }
}

TEST(NativeSourceHints, InferenceSurvivesRealLowMedHighScalarPipeline) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    LowFunc Low;
    Low.Entry = Fixture.Med.Entry;
    Low.Name = "scalar_helper";
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = Low.Entry;
    Block.EndAddr = Low.Entry + 8;
    LowOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Addr = Low.Entry;
    Add.Output = NdVar::reg(TRI.IntReturnReg, 4);
    Add.addInput(NdVar::reg(TRI.IntParamRegs[0], 4));
    Add.addInput(NdVar::reg(TRI.IntParamRegs[1], 4));
    Block.Ops.push_back(Add);
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Low.Entry + 4;
    Return.addInput(NdVar::reg(TRI.IntReturnReg, 4));
    Block.Ops.push_back(Return);
    Low.Blocks.push_back(Block);
    Fixture.Med =
        LowToMedConverter().convert(Low, Architecture, BinaryFormat::MachO);
    inferMedTypes(Fixture.Med, Architecture);
    Fixture.High = MedToHighConverter().convert(Fixture.Med, Architecture);
    std::string Error;
    auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    ASSERT_EQ(Hint->Parameters.size(), 2U);
    Fixture.Med.SourceTypeHint = *Hint;
    inferMedTypes(Fixture.Med, Architecture);
    ASSERT_TRUE(Fixture.Med.SourceTypeHint);
    ASSERT_TRUE(Fixture.Med.SourceParametersBound);
    Fixture.High = MedToHighConverter().convert(Fixture.Med, Architecture);
    ASSERT_EQ(Fixture.High.Params.size(), 2U);
    EXPECT_EQ(Fixture.High.Params[0].Name, "native_arg0");
    EXPECT_EQ(Fixture.High.Params[1].Name, "native_arg1");
  }
}
} // namespace
