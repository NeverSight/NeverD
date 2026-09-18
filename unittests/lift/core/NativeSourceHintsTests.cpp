#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedSourceParameterUses.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/pipeline/NativeSourceHints.h"
#include "neverd/pipeline/Pipeline.h"

#include <algorithm>
#include <array>

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

NativeFixture nativePairFixture(Arch Architecture) {
  NativeFixture F(Architecture);
  const auto &TRI = getTargetRegInfo(Architecture);
  F.Med.ReturnType = F.High.ReturnType = NdType::makeInt(8, false);
  for (size_t I = 0; I < F.Med.Params.size(); ++I) {
    F.Med.Params[I].Size = 8;
    F.Med.TypedParams[I].Type = F.High.Params[I].Type = NdType::makeInt(8);
  }
  F.Med.Params[1].Id = -1;
  auto &Primary = F.Med.Blocks[0].Ops[0];
  Primary.Output.Size = 8;
  Primary.Inputs[0] = F.Med.Params[0];
  Primary.Inputs[1] = MedVar::makeConst(17, 8);
  auto Secondary = Primary;
  Secondary.Opcode = NdOp::INT_XOR;
  Secondary.Output.RegOff = TRI.IntReturnRegs[1];
  Secondary.Output.Id = 11;
  Secondary.Inputs[1] = MedVar::makeConst(UINT64_C(0xfedcba9876543210), 8);
  F.Med.Blocks[0].Ops.insert(F.Med.Blocks[0].Ops.begin() + 1, Secondary);
  return F;
}

TEST(NativeSourceHints, IntegerPairDemandStopsAtClobbersAndPartialReads) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
      SCOPED_TRACE(Mutation);
      const auto &TRI = getTargetRegInfo(Architecture);
      LowFunc F;
      F.Blocks.emplace_back();
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(NdVar::cst(0x1080, 8));
      LowOp Read;
      Read.Opcode = NdOp::COPY;
      Read.Output = NdVar::reg(TRI.IntReturnRegs[0], 8);
      Read.addInput(NdVar::reg(TRI.IntReturnRegs[1], 8));
      if (Mutation == 1)
        Read.Inputs[0].Size = 4;
      if (Mutation == 2)
        Call.Opcode = NdOp::INDIR_CALL;
      F.Blocks[0].Ops = {Call, Read};
      if (Mutation >= 3 && Mutation <= 5) {
        LowOp Stop;
        Stop.Opcode = Mutation == 3 ? NdOp::INTRINSIC : NdOp::COPY;
        Stop.Output = NdVar::reg(TRI.IntReturnRegs[1], Mutation == 4 ? 4 : 8);
        Stop.addInput(NdVar::cst(0, 8));
        F.Blocks[0].Ops.insert(F.Blocks[0].Ops.begin() + 1, Stop);
      }
      if (Mutation == 6) {
        F.Blocks[0].Ops.back().Opcode = NdOp::INT_XOR;
        F.Blocks[0].Ops.back().addInput(Read.Inputs[0]);
      }
      if (Mutation == 7) {
        F.Blocks[0].Ops.pop_back();
        F.Blocks.emplace_back();
        F.Blocks.back().Ops.push_back(Read);
      }
      EXPECT_EQ(observedNativeIntegerPairReturns(F, Architecture),
                Mutation == 0 ? std::set<va_t>{0x1080} : std::set<va_t>{});
    }
}

TEST(NativeSourceHints, IntegerPairsRequireBothCompleteReturnCarriers) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto F = nativePairFixture(Architecture);
      auto &Ops = F.Med.Blocks[0].Ops;
      if (Mutation == 1)
        Ops[1].Output.Size = 4;
      if (Mutation == 2)
        Ops.erase(Ops.begin() + 1);
      if (Mutation == 3)
        F.Med.Blocks[0].Preds = {99};
      if (Mutation == 4)
        F.Med.Blocks[0].ExceptionalSuccs.emplace_back();
      std::string Error;
      const auto Pair = inferNativeSourceTypeHint(
          F.Image, F.Med, F.High, F.Audit, Error, nullptr, true);
      if (Mutation) {
        EXPECT_TRUE(!Pair || Pair->ReturnType->Kind != NdTypeKind::Struct);
      } else {
        ASSERT_TRUE(Pair) << Error;
        EXPECT_EQ(Pair->ReturnType->Kind, NdTypeKind::Struct);
        EXPECT_EQ(Pair->ReturnType->Size, 16U);
        ASSERT_EQ(Pair->ReturnComponents.size(), 2U);
        for (unsigned I = 0; I < 2; ++I) {
          EXPECT_EQ(Pair->ReturnComponents[I].RegisterOffset,
                    getTargetRegInfo(Architecture).IntReturnRegs[I]);
          EXPECT_EQ(Pair->ReturnComponents[I].ValueBytes, 8U);
        }
        const auto Scalar = F.infer(Error);
        ASSERT_TRUE(Scalar) << Error;
        EXPECT_EQ(Scalar->ReturnType->Kind, NdTypeKind::Int);
      }
    }
}

TEST(NativeSourceHints,
     IntegerPairRefinementRequiresTheInferredNativeIdentity) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      auto F = nativePairFixture(Architecture);
      std::string Error;
      const auto Scalar = F.infer(Error);
      ASSERT_TRUE(Scalar) << Error;
      F.Med.SourceTypeHint = F.High.SourceTypeHint = *Scalar;
      F.Med.SourceParametersBound = true;
      if (Mutation == 1)
        F.Med.SourceTypeHint->Origin = F.High.SourceTypeHint->Origin =
            SourceFunctionTypeHint::OriginKind::DarwinSDK;
      if (Mutation == 2)
        F.Audit.Entry += 4;
      if (Mutation == 3)
        F.Med.SourceParametersBound = false;
      if (Mutation == 4)
        F.High.SourceTypeHint->ReturnType = NdType::makeInt(4);
      if (Mutation == 5)
        F.Med.Blocks[0].Ops[1].Output.Size = 4;
      const auto Pair =
          refineNativeIntegerPairReturnHint(F.Med, F.High, F.Audit);
      EXPECT_EQ(bool(Pair), Mutation == 0);
      if (Pair) {
        F.Med.SourceTypeHint = F.High.SourceTypeHint = *Pair;
        F.Med.ReturnType = F.High.ReturnType = Pair->ReturnType;
        EXPECT_FALSE(refineNativeIntegerPairReturnHint(F.Med, F.High, F.Audit));
      }
    }
}

TEST(NativeSourceHints, IntegerPairPathsMeetBothWordsAcrossJoinsAndBackedges) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Loop : {false, true})
      for (bool MissingWord : {false, true}) {
        auto F = nativePairFixture(Architecture);
        const auto Primary = F.Med.Blocks[0].Ops[0];
        const auto Secondary = F.Med.Blocks[0].Ops[1];
        const auto Return = F.Med.Blocks[0].Ops.back();
        F.Med.Blocks[0].Ops = {Primary};
        F.Med.Blocks[0].Succs = {1, 2};
        F.Med.Blocks.resize(4);
        for (int I = 1; I <= 3; ++I)
          F.Med.Blocks[I].Id = I;
        F.Med.Blocks[1].Preds =
            Loop ? std::vector<int>{0, 1} : std::vector<int>{0};
        F.Med.Blocks[1].Succs =
            Loop ? std::vector<int>{1, 3} : std::vector<int>{3};
        F.Med.Blocks[1].Ops = {Secondary};
        F.Med.Blocks[2].Preds = {0};
        F.Med.Blocks[2].Succs = {3};
        F.Med.Blocks[2].Ops =
            MissingWord ? std::vector<MedOp>{} : std::vector<MedOp>{Secondary};
        F.Med.Blocks[3].Preds = {1, 2};
        F.Med.Blocks[3].Ops = {Return};
        std::string Error;
        const auto Hint = inferNativeSourceTypeHint(
            F.Image, F.Med, F.High, F.Audit, Error, nullptr, true);
        ASSERT_TRUE(Hint) << Error;
        EXPECT_EQ(Hint->ReturnType->Kind == NdTypeKind::Struct, !MissingWord);
      }
}

TEST(NativeSourceHints, NonReturningContractsRequireTerminalFlowAndBoundCalls) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
      SCOPED_TRACE(Mutation);
      NativeFixture F(Architecture);
      F.Med.DoesNotReturn = F.High.DoesNotReturn = true;
      auto Binding = std::make_shared<SourceCallTypeHint>();
      Binding->CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
      Binding->TargetAddress = 0x3000;
      Binding->TargetName = "abort";
      Binding->DoesNotReturn = true;
      Binding->Signature.ReturnType = NdType::makeVoid();
      std::string Error;
      ASSERT_TRUE(
          assignDarwinScalarSourceABI(Binding->Signature, Architecture, Error));
      MedOp Call;
      Call.Opcode = NdOp::CALL;
      Call.DoesNotReturn = true;
      Call.SourceCallHint = Binding;
      Call.addInput(MedVar::makeConst(0x3000, 8));
      F.Med.Blocks[0].Ops = {Call};
      auto Expression = HighExpr::makeCall("abort", 0x3000, {});
      Expression->SourceCallHint = Binding;
      Expression->Type = NdType::makeVoid();
      HighStmt Statement;
      Statement.Kind = StmtKind::Call;
      Statement.CallExpr = Expression;
      F.High.Body = {Statement};
      if (Mutation == 1)
        F.Med.DoesNotReturn = false;
      if (Mutation == 2)
        F.High.DoesNotReturn = false;
      if (Mutation == 3) {
        F.Med.Blocks[0].Ops[0].DoesNotReturn = false;
        MedOp Return;
        Return.Opcode = NdOp::RETURN;
        F.Med.Blocks[0].Ops.push_back(Return);
      }
      if (Mutation == 4)
        F.Med.Blocks[0].Ops.clear();
      if (Mutation == 5)
        F.Med.Blocks[0].Ops[0].Opcode = NdOp::INTRINSIC;
      if (Mutation == 6)
        F.Med.Blocks[0].Ops[0].SourceCallHint.reset();
      if (Mutation == 7)
        Binding->DoesNotReturn = false;
      if (Mutation == 8)
        F.Med.Blocks[0].ExceptionalSuccs.emplace_back();
      if (Mutation == 9)
        F.Med.Blocks[0].Ops[0].addInput(MedVar::makeConst(0, 8));
      auto Hint = F.infer(Error);
      EXPECT_EQ(bool(Hint), Mutation == 0) << Error;
      if (Hint) {
        EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
        EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::None);
        EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 0U);
      }
    }
}

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

TEST(NativeSourceHints, LeafReturnsPreserveProvenFullMachineWidth) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    auto &Write = Fixture.Med.Blocks[0].Ops.front();
    Write.Opcode = NdOp::INT_ZEXT;
    Write.NumInputs = 1;
    Write.Output.Size = 8;
    Write.Inputs[0] = MedVar::makeConst(UINT32_MAX, 4);
    Fixture.High.Body.front().RetVal =
        HighExpr::makeUnary(NdOp::INT_ZEXT, HighExpr::makeConst(UINT32_MAX, 4));
    Fixture.High.Body.front().RetVal->Type = NdType::makeInt(8, false);
    std::string Error;
    const auto Full = Fixture.infer(Error);
    ASSERT_TRUE(Full) << Error;
    EXPECT_EQ(Full->ReturnType->Size, 8U);
    EXPECT_FALSE(Full->ReturnType->IsSigned);
    EXPECT_EQ(Full->ReturnLocation.ValueBytes, 8U);
    // A source expression alone cannot authorize a wider machine carrier.
    Write.Output.Size = 4;
    const auto Narrow = Fixture.infer(Error);
    ASSERT_TRUE(Narrow) << Error;
    EXPECT_EQ(Narrow->ReturnLocation.ValueBytes, 4U);
    Write.Output.Size = 8;
    Fixture.High.Body.front().RetVal = HighExpr::makeUndef(8);
    const auto Unknown = Fixture.infer(Error);
    ASSERT_TRUE(Unknown) << Error;
    EXPECT_EQ(Unknown->ReturnLocation.ValueBytes, 4U);
  }
}

TEST(NativeSourceHints, NarrowExternalResultsDoNotProveTheirUpperBits) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    auto &Write = Fixture.Med.Blocks[0].Ops.front();
    Write.Opcode = NdOp::CALL;
    Write.NumInputs = 1;
    Write.Inputs[0] = MedVar::makeConst(0x1020, 8);
    Write.Output.Size = 8;
    auto Call = std::make_shared<SourceCallTypeHint>();
    Call->Signature.ReturnType = NdType::makeInt(4, false);
    std::string Error;
    ASSERT_TRUE(
        assignDarwinScalarSourceABI(Call->Signature, Architecture, Error));
    Write.SourceCallHint = Call;
    Fixture.High.Body.front().RetVal = HighExpr::makeConst(UINT32_MAX, 8);
    const auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 4U);
  }
}

NativeFixture compilerRTPlatformVersionFixture(Arch Architecture) {
  NativeFixture Fixture(Architecture);
  const auto &TRI = getTargetRegInfo(Architecture);
  Fixture.Med.ReturnType = Fixture.High.ReturnType = NdType::makeInt(8);
  Fixture.Med.Blocks[0].Ops[0].Output.Size = 8;
  Fixture.High.Body[0].RetVal = HighExpr::makeConst(1, 8);
  for (unsigned Index = 2; Index < 4; ++Index) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.Id = Index;
    Parameter.RegOff = TRI.IntParamRegs[Index];
    Parameter.Size = 4;
    Parameter.TheArch = Architecture;
    Fixture.Med.Params.push_back(Parameter);
    Fixture.Med.TypedParams.push_back(
        {"arg" + std::to_string(Index), NdType::makeInt(4)});
    Fixture.High.Params.push_back(
        {"arg" + std::to_string(Index), NdType::makeInt(4)});
  }
  SourceCallTypeHint Availability;
  Availability.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  Availability.WeakImport = true;
  Availability.TargetAddress = 0x1080;
  Availability.TargetName = "_availability_version_check";
  Availability.Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
  Availability.Signature.ReturnType = NdType::makeInt(1, false);
  Availability.Signature.Parameters = {
      {"count", NdType::makeInt(4, false)},
      {"versions", NdType::makePtr(NdType::makeVoid())},
  };
  std::string Error;
  EXPECT_TRUE(
      assignDarwinFixedSourceABI(Availability.Signature, Architecture, Error))
      << Error;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.SourceCallHint =
      std::make_shared<const SourceCallTypeHint>(std::move(Availability));
  Call.addInput(MedVar::makeConst(0x1080, 8));
  Call.addInput(MedVar::makeConst(1, 4));
  Call.addInput(MedVar::makeConst(0x10a0, 8));
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Call);
  Fixture.Audit.DecodedInstructions = Fixture.Audit.LiftedInstructions = 3;
  Fixture.Image.Symbols.push_back(
      {"___isPlatformVersionAtLeast", Fixture.Med.Entry, 0x80, true});
  return Fixture;
}

TEST(NativeSourceHints,
     CompilerRTPlatformVersionHelperUsesExactNarrowContract) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Fixture = compilerRTPlatformVersionFixture(Architecture);
    std::string Error;
    const auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    ASSERT_TRUE(Hint->ReturnType);
    EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Int);
    EXPECT_EQ(Hint->ReturnType->Size, 4U);
    EXPECT_TRUE(Hint->ReturnType->IsSigned);
    ASSERT_EQ(Hint->Parameters.size(), 4U);
    const auto &TRI = getTargetRegInfo(Architecture);
    for (size_t I = 0; I < Hint->Parameters.size(); ++I) {
      EXPECT_EQ(Hint->Parameters[I].Type->Kind, NdTypeKind::Int);
      EXPECT_EQ(Hint->Parameters[I].Type->Size, 4U);
      EXPECT_FALSE(Hint->Parameters[I].Type->IsSigned);
      EXPECT_EQ(Hint->Parameters[I].Location.RegisterOffset,
                TRI.IntParamRegs[I]);
      EXPECT_EQ(Hint->Parameters[I].Location.ValueBytes, 4U);
    }
  }
}

TEST(NativeSourceHints,
     CompilerRTPlatformVersionHelperRejectsContradictoryEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      auto Fixture = compilerRTPlatformVersionFixture(Architecture);
      auto &Symbol = Fixture.Image.Symbols.back();
      auto Call = std::make_shared<SourceCallTypeHint>(
          *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
      Fixture.Med.Blocks[0].Ops[0].SourceCallHint = Call;
      if (Mutation == 0)
        Symbol.IsFunc = false;
      if (Mutation == 1)
        Fixture.Image.Symbols.push_back(Symbol);
      if (Mutation == 2)
        Call->WeakImport = false;
      if (Mutation == 3)
        Call->TargetName = "_different";
      if (Mutation == 4)
        Call->CallKind = SourceCallTypeHint::Kind::Native;
      if (Mutation == 5)
        Call->Signature.ReturnType = NdType::makeInt(4, false);
      std::string Error;
      EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
      EXPECT_FALSE(Error.empty()) << Mutation;
    }

    for (unsigned Mutation = 0; Mutation < 2; ++Mutation) {
      auto Unrelated = compilerRTPlatformVersionFixture(Architecture);
      if (Mutation == 0)
        Unrelated.Image.Symbols.back().Name = "unrelated_local_helper";
      else
        Unrelated.Image.Symbols.back().Addr += 4;
      std::string Error;
      const auto Generic = Unrelated.infer(Error);
      ASSERT_TRUE(Generic) << Mutation << ": " << Error;
      EXPECT_EQ(Generic->ReturnType->Size, 8U) << Mutation;
    }
  }
}

struct NativeVoidFixture : NativeFixture {
  LowFunc Low;
  NativeVoidFixture(Arch Architecture, bool Indirect = false)
      : NativeFixture(Architecture) {
    Med.Params[0].Size = 8;
    Med.TypedParams[0].Type = High.Params[0].Type = NdType::makeInt(8);
    Med.Params[1].Id = -1;
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall;
    Hint->Signature.ReturnType = NdType::makeVoid();
    Hint->Signature.Parameters.push_back(
        {"object", NdType::makePtr(NdType::makeVoid())});
    std::string Error;
    EXPECT_TRUE(
        assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error));
    MedOp Call;
    Call.Opcode = Indirect ? NdOp::INDIR_CALL : NdOp::CALL;
    Call.Addr = 0x1010;
    Call.OriginSeq = 7;
    Call.SourceCallHint = Hint;
    Call.addInput(MedVar::makeConst(0x1080, 8));
    Call.addInput(Med.Params[0]);
    auto Return = Med.Blocks[0].Ops.back();
    Return.Addr = Call.Addr;
    Med.Blocks[0].Ops = {Call, Return};
    Low.Entry = Med.Entry;
    Low.Blocks.emplace_back();
    Low.Blocks[0].Id = 0;
    LowOp NativeCall;
    NativeCall.Opcode = Call.Opcode;
    NativeCall.Addr = Call.Addr;
    NativeCall.Seq = Call.OriginSeq;
    NativeCall.Output =
        NdVar::reg(getTargetRegInfo(Architecture).IntReturnReg, 8);
    NativeCall.addInput(NdVar::cst(0x1080, 8));
    LowOp NativeReturn;
    NativeReturn.Opcode = NdOp::RETURN;
    NativeReturn.Addr = Call.Addr;
    Low.Blocks[0].Ops = {NativeCall, NativeReturn};
  }
  std::optional<SourceFunctionTypeHint> inferVoid(std::string &Error) const {
    return inferNativeSourceTypeHint(Image, Med, High, Audit, Error, &Low);
  }

  void useValueWitnessDestroy() {
    const auto &TRI = getTargetRegInfo(Image.Arch);
    auto Hint = swiftValueWitnessSourceCallHint(
        Image.Arch, SourceCallTypeHint::SwiftValueWitnessKind::Destroy);
    ASSERT_TRUE(Hint);
    auto &Call = Med.Blocks[0].Ops[0];
    ASSERT_TRUE(Call.Opcode == NdOp::CALL || Call.Opcode == NdOp::INDIR_CALL);
    MedVar Target;
    Target.Kind = MedVar::Reg;
    Target.Id = 20;
    Target.SSAVer = 1;
    Target.Size = 8;
    Target.TheArch = Image.Arch;
    Target.RegOff = TRI.IntReturnReg;
    Med.Params[1].Id = 1;
    Med.Params[1].Size = 8;
    Med.TypedParams[0].Type = High.Params[0].Type =
        NdType::makePtr(NdType::makeVoid());
    Med.TypedParams[1].Type = High.Params[1].Type =
        NdType::makePtr(NdType::makeVoid());
    Call.Opcode = NdOp::INDIR_CALL;
    Call.SourceCallHint =
        std::make_shared<const SourceCallTypeHint>(std::move(*Hint));
    Call.NumInputs = 0;
    Call.addInput(Target);
    Call.addInput(Med.Params[0]);
    Call.addInput(Med.Params[1]);

    LowOp *NativeCall = nullptr;
    for (auto &Block : Low.Blocks)
      for (auto &Operation : Block.Ops)
        if (Operation.Opcode == NdOp::CALL ||
            Operation.Opcode == NdOp::INDIR_CALL) {
          ASSERT_EQ(NativeCall, nullptr);
          NativeCall = &Operation;
        }
    ASSERT_NE(NativeCall, nullptr);
    NativeCall->Opcode = NdOp::INDIR_CALL;
    NativeCall->NumInputs = 0;
    NativeCall->addInput(NdVar::reg(TRI.IntReturnReg, 8));
  }

  void useValueWitnessStoreTag() {
    useValueWitnessDestroy();
    auto Hint = swiftValueWitnessSourceCallHint(
        Image.Arch,
        SourceCallTypeHint::SwiftValueWitnessKind::StoreEnumTagSinglePayload);
    ASSERT_TRUE(Hint);
    auto &Call = Med.Blocks[0].Ops[0];
    Call.SourceCallHint = std::make_shared<const SourceCallTypeHint>(*Hint);
    const auto Target = Call.Inputs[0];
    Call.NumInputs = 0;
    Call.addInput(Target);
    Med.Params.clear();
    Med.TypedParams.clear();
    High.Params.clear();
    for (const auto &P : Hint->Signature.Parameters) {
      MedVar Parameter;
      Parameter.Kind = MedVar::Param;
      Parameter.Id = Med.Params.size();
      Parameter.Size = P.Type->Size;
      Parameter.RegOff = P.Location.RegisterOffset;
      Parameter.TheArch = Image.Arch;
      Med.Params.push_back(Parameter);
      Med.TypedParams.push_back({P.Name, P.Type});
      High.Params.push_back({P.Name, P.Type});
      Call.addInput(Parameter);
    }
  }

  void useNativeVoidCallee() {
    auto Hint = std::make_shared<SourceCallTypeHint>(
        *Med.Blocks[0].Ops[0].SourceCallHint);
    Hint->CallKind = SourceCallTypeHint::Kind::Native;
    Hint->TargetAddress = 0x1080;
    Hint->TargetName = "native_void_callee";
    Hint->Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Med.Blocks[0].Ops[0].SourceCallHint = std::move(Hint);
  }
};

TEST(NativeSourceHints, VoidTailForwardersNeverSupplyAnUnprovenResult) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Indirect : {false, true}) {
      NativeVoidFixture Fixture(Architecture, Indirect);
      std::string Error;
      const auto Hint = Fixture.inferVoid(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
      EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::None);
      EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 0U);
      ASSERT_EQ(Hint->Parameters.size(), 1U);
      EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset,
                getTargetRegInfo(Architecture).IntParamRegs[0]);
      EXPECT_FALSE(Fixture.infer(Error));
      EXPECT_EQ(Fixture.Med.ReturnType->Kind, NdTypeKind::Int);
      EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                MedReturnValueEvidence::Unknown);

      // Keep the established leaf subset available when the general frame
      // proof rejects an otherwise harmless LowIR operation. The narrower
      // proof must still carry stack-address taint through that operation to
      // a declared call argument.
      NativeVoidFixture Compatibility(Architecture, Indirect);
      LowOp Intrinsic;
      Intrinsic.Opcode = NdOp::INTRINSIC;
      Intrinsic.Addr = 0x1004;
      Intrinsic.Output =
          NdVar::reg(getTargetRegInfo(Architecture).IntParamRegs[1], 8);
      Compatibility.Low.Blocks[0].Ops.insert(
          Compatibility.Low.Blocks[0].Ops.begin(), Intrinsic);
      EXPECT_TRUE(Compatibility.inferVoid(Error)) << Error;
      Compatibility.Low.Blocks[0].Ops[0].Output =
          NdVar::reg(getTargetRegInfo(Architecture).IntParamRegs[0], 8);
      Compatibility.Low.Blocks[0].Ops[0].addInput(
          NdVar::reg(getTargetRegInfo(Architecture).StackPointer, 8));
      EXPECT_FALSE(Compatibility.inferVoid(Error));
    }
}

TEST(NativeSourceHints, VoidContractsPropagateAcrossExactNativeCallees) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFixture Fixture(Architecture);
    Fixture.useNativeVoidCallee();
    std::string Error;
    const auto Hint = Fixture.inferVoid(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);

    auto Forged = std::make_shared<SourceCallTypeHint>(
        *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
    Forged->TargetAddress += 4;
    Fixture.Med.Blocks[0].Ops[0].SourceCallHint = std::move(Forged);
    EXPECT_FALSE(Fixture.inferVoid(Error));
  }
}

TEST(NativeSourceHints, VoidContractsAcceptExactSwiftStringBridgeCallBindings) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (auto Kind : {SourceCallTypeHint::Kind::SwiftStringBridge,
                      SourceCallTypeHint::Kind::SwiftStringFromNSString}) {
      NativeVoidFixture Fixture(Architecture);
      auto Hint = std::make_shared<SourceCallTypeHint>(
          *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
      Hint->CallKind = Kind;
      Hint->Signature.Origin =
          SourceFunctionTypeHint::OriginKind::SwiftStringBridge;
      Fixture.Med.Blocks[0].Ops[0].SourceCallHint = std::move(Hint);
      std::string Error;
      const auto Result = Fixture.inferVoid(Error);
      ASSERT_TRUE(Result) << Error;
      EXPECT_EQ(Result->ReturnType->Kind, NdTypeKind::Void);
    }
}

TEST(NativeSourceHints,
     VoidTailContractsRejectHiddenOutputsAndIncompleteEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 15; ++Mutation) {
      NativeVoidFixture Fixture(Architecture);
      const auto &TRI = getTargetRegInfo(Architecture);
      auto &Ops = Fixture.Low.Blocks[0].Ops;
      switch (Mutation) {
      case 0:
        ++Ops[1].Addr;
        break;
      case 1:
      case 2:
      case 3: {
        LowOp Write;
        Write.Opcode = NdOp::COPY;
        const auto Register = Mutation == 1                   ? TRI.StackPointer
                              : Mutation == 2                 ? TRI.FramePointer
                              : Architecture == Arch::AArch64 ? a64reg::X19
                                                              : x86reg::RBX;
        Write.Output = NdVar::reg(Register, 8);
        Write.addInput(NdVar::cst(0, 8));
        Ops.insert(Ops.begin(), Write);
        break;
      }
      case 4:
        Ops[0].Inputs[0].Offset += 8;
        break;
      case 5:
        Ops[0].Addr += 4;
        Ops[1].Addr += 4;
        break;
      case 6:
      case 7:
      case 8: {
        auto Hint = std::make_shared<SourceCallTypeHint>(
            *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
        if (Mutation == 6) {
          // A changed return type without its matching ABI is not a valid
          // source contract, even when the helper supplies no result.
          Hint->Signature.ReturnType = NdType::makeInt(8);
        } else if (Mutation == 7) {
          Hint->CallKind = SourceCallTypeHint::Kind::Native;
        } else {
          Hint->DoesNotReturn = true;
        }
        Fixture.Med.Blocks[0].Ops[0].SourceCallHint = Hint;
        break;
      }
      case 9:
        Fixture.Low.Entry += 4;
        break;
      case 10:
        Fixture.Low.Blocks.clear();
        break;
      case 11:
        Fixture.Med.Blocks[0].Preds = {17};
        break;
      case 12:
        Ops[0].Inputs[0] = NdVar::reg(TRI.IntParamRegs[0], 8);
        break;
      case 13:
        Ops[0].NumInputs = 7;
        break;
      case 14: {
        LowOp Escape;
        Escape.Opcode = NdOp::COPY;
        Escape.Addr = Ops[0].Addr;
        Escape.Output = NdVar::reg(TRI.IntParamRegs[0], 8);
        Escape.addInput(NdVar::reg(TRI.StackPointer, 8));
        Ops.insert(Ops.begin(), Escape);
        break;
      }
      }
      std::string Error;
      EXPECT_FALSE(Fixture.inferVoid(Error)) << Mutation << ": " << Error;
    }
}

TEST(NativeSourceHints,
     CanonicalDynamicValueWitnessDestroyHasAnExactVoidCallIdentity) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFixture Leaf(Architecture);
    Leaf.useValueWitnessDestroy();
    std::string Error;
    const auto LeafHint = Leaf.inferVoid(Error);
    ASSERT_TRUE(LeafHint) << Error;
    EXPECT_EQ(LeafHint->ReturnType->Kind, NdTypeKind::Void);
  }
}

TEST(NativeSourceHints, CanonicalSinglePayloadStorePreservesVoidCallEffects) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFixture Fixture(Architecture);
    Fixture.useValueWitnessStoreTag();
    std::string Error;
    const auto Hint = Fixture.inferVoid(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Hint->Parameters.size(), 4U);
    EXPECT_EQ(Hint->Parameters[1].Type->Size, 4U);
    EXPECT_EQ(Hint->Parameters[2].Type->Size, 4U);
  }
}

TEST(NativeSourceHints,
     DynamicVoidCallIdentityRejectsForgeryMismatchAndFrameTargets) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
      NativeVoidFixture Fixture(Architecture);
      Fixture.useValueWitnessDestroy();
      auto &MedCall = Fixture.Med.Blocks[0].Ops[0];
      auto &LowCall = Fixture.Low.Blocks[0].Ops[0];
      const auto &TRI = getTargetRegInfo(Architecture);
      switch (Mutation) {
      case 0: {
        auto Hint =
            std::make_shared<SourceCallTypeHint>(*MedCall.SourceCallHint);
        Hint->TargetName += "_forged";
        MedCall.SourceCallHint = std::move(Hint);
        break;
      }
      case 1: {
        auto Hint = swiftValueWitnessSourceCallHint(
            Architecture,
            SourceCallTypeHint::SwiftValueWitnessKind::InitializeWithCopy);
        ASSERT_TRUE(Hint);
        MedCall.SourceCallHint =
            std::make_shared<const SourceCallTypeHint>(std::move(*Hint));
        break;
      }
      case 2:
        MedCall.Opcode = NdOp::CALL;
        break;
      case 3:
        LowCall.Opcode = NdOp::CALL;
        break;
      case 4:
        LowCall.Inputs[0] = NdVar::cst(0x1080, 8);
        break;
      case 5:
        MedCall.Inputs[0] = MedVar::makeConst(0x1080, 8);
        break;
      case 6: {
        LowOp Derive;
        Derive.Opcode = NdOp::COPY;
        Derive.Addr = 0x1008;
        Derive.Output = NdVar::reg(TRI.IntReturnReg, 8);
        Derive.addInput(NdVar::reg(TRI.StackPointer, 8));
        Fixture.Low.Blocks[0].Ops.insert(Fixture.Low.Blocks[0].Ops.begin(),
                                         std::move(Derive));
        break;
      }
      case 7:
        LowCall.Inputs[0].Size = 4;
        break;
      case 8:
        ++LowCall.Seq;
        break;
      }
      std::string Error;
      EXPECT_FALSE(Fixture.inferVoid(Error))
          << unsigned(Architecture) << ": " << Mutation << ": " << Error;
    }
}

struct NativeVoidFrameFixture : NativeVoidFixture {
  Arch Architecture;
  std::vector<uint64_t> Saved;
  size_t CallIndex = 0;
  size_t RestoreIndex = 0;
  int64_t FrameBytes;

  static LowOp op(NdOp Opcode, NdVar Output,
                  std::initializer_list<NdVar> Inputs, va_t Address = 0x1004) {
    LowOp Result;
    Result.Opcode = Opcode;
    Result.Output = Output;
    Result.Addr = Address;
    for (auto Input : Inputs)
      Result.addInput(Input);
    return Result;
  }

  explicit NativeVoidFrameFixture(Arch Architecture)
      : NativeVoidFixture(Architecture), Architecture(Architecture),
        FrameBytes(Architecture == Arch::AArch64 ? 32 : 24) {
    const auto &TRI = getTargetRegInfo(Architecture);
    Saved = {Architecture == Arch::AArch64 ? a64reg::X19 : x86reg::RBX,
             TRI.FramePointer};
    if (Architecture == Arch::AArch64) {
      Saved.push_back(TRI.LinkRegister);
      Saved.push_back(TRI.VecRegBase + 8 * TRI.VecRegStride);
    }
    const auto Call = Low.Blocks[0].Ops[0];
    auto &Ops = Low.Blocks[0].Ops;
    const auto SP = NdVar::reg(TRI.StackPointer, 8);
    const auto Address = NdVar::tmp(TmpBase, 8);
    Ops = {op(NdOp::INT_SUB, SP, {SP, NdVar::cst(FrameBytes, 8)})};
    for (size_t I = 0; I < Saved.size(); ++I) {
      Ops.push_back(op(NdOp::INT_ADD, Address, {SP, NdVar::cst(I * 8, 8)}));
      Ops.push_back(op(NdOp::STORE, {}, {Address, NdVar::reg(Saved[I], 8)}));
      Ops.push_back(
          op(NdOp::COPY, NdVar::reg(Saved[I], 8), {NdVar::cst(0, 8)}));
    }
    CallIndex = Ops.size();
    Ops.push_back(Call);
    RestoreIndex = Ops.size();
    for (size_t I = 0; I < Saved.size(); ++I) {
      Ops.push_back(
          op(NdOp::INT_ADD, Address, {SP, NdVar::cst(I * 8, 8)}, 0x1020));
      Ops.push_back(op(NdOp::LOAD, NdVar::reg(Saved[I], 8), {Address}, 0x1020));
    }
    Ops.push_back(
        op(NdOp::INT_ADD, SP, {SP, NdVar::cst(FrameBytes, 8)}, 0x1020));
    Ops.push_back(op(
        NdOp::RETURN, {},
        {NdVar::reg(TRI.LinkRegister ? TRI.LinkRegister : TRI.IntReturnReg, 8)},
        0x1024));
  }

  void splitReturns(bool Loop = false) {
    auto &First = Low.Blocks[0];
    const std::vector<LowOp> Restore(First.Ops.begin() + RestoreIndex,
                                     First.Ops.end());
    First.Ops.resize(RestoreIndex);
    First.Succs = Loop ? std::vector<int>{1} : std::vector<int>{1, 2};
    LowBlock Left, Right;
    Left.Id = 1;
    Right.Id = 2;
    Left.Preds = Loop ? std::vector<int>{0, 1} : std::vector<int>{0};
    Right.Preds = Loop ? std::vector<int>{1} : std::vector<int>{0};
    Left.Ops = Loop ? std::vector<LowOp>{op(NdOp::NOP, {}, {})} : Restore;
    Right.Ops = Restore;
    if (Loop)
      Left.Succs = {1, 2};
    Low.Blocks.push_back(std::move(Left));
    Low.Blocks.push_back(std::move(Right));
  }

  size_t usePreindexedSpills() {
    auto &Ops = Low.Blocks[0].Ops;
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto SP = NdVar::reg(TRI.StackPointer, 8);
    const auto Base = NdVar::tmp(TmpBase + 64, 8);
    const auto Address = NdVar::tmp(TmpBase, 8);
    std::vector<LowOp> Prologue{
        op(NdOp::INT_SUB, Base, {SP, NdVar::cst(FrameBytes, 8)})};
    for (size_t I = 0; I < Saved.size(); ++I) {
      Prologue.push_back(
          op(NdOp::INT_ADD, Address, {Base, NdVar::cst(I * 8, 8)}));
      Prologue.push_back(
          op(NdOp::STORE, {}, {Address, NdVar::reg(Saved[I], 8)}));
      Prologue.push_back(
          op(NdOp::COPY, NdVar::reg(Saved[I], 8), {NdVar::cst(0, 8)}));
    }
    const size_t StackWrite = Prologue.size();
    Prologue.push_back(op(NdOp::COPY, SP, {Base}));
    Ops.erase(Ops.begin(), Ops.begin() + CallIndex);
    Ops.insert(Ops.begin(), Prologue.begin(), Prologue.end());
    CallIndex = Prologue.size();
    RestoreIndex++;
    return StackWrite;
  }
};

TEST(NativeSourceHints,
     CanonicalDynamicValueWitnessDestroyRestoresFramedState) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFrameFixture Fixture(Architecture);
    Fixture.useValueWitnessDestroy();
    std::string Error;
    const auto Hint = Fixture.inferVoid(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  }
}

TEST(NativeSourceHints, VoidFramesRestoreEntryBytesAcrossBranchesAndLoops) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Shape = 0; Shape < 4; ++Shape) {
      NativeVoidFrameFixture Fixture(Architecture);
      if (Shape == 3)
        Fixture.usePreindexedSpills();
      else if (Shape)
        Fixture.splitReturns(Shape == 2);
      std::string Error;
      const auto Hint = Fixture.inferVoid(Error);
      ASSERT_TRUE(Hint) << unsigned(Architecture) << ": " << Shape << ": "
                        << Error;
      EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
      EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::None);
      EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                MedReturnValueEvidence::Unknown);
    }
}

TEST(NativeSourceHints, BoundObjCDispatchRequiresCompleteFramedCallEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (auto Kind : {SourceCallTypeHint::Kind::ObjCMessage,
                      SourceCallTypeHint::Kind::ObjCSuper2})
      for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
        SCOPED_TRACE(Mutation);
        NativeVoidFrameFixture F(Architecture);
        auto &Call = F.Med.Blocks[0].Ops[0];
        auto Hint = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
        Hint->CallKind = Kind;
        Hint->Signature.Origin =
            SourceFunctionTypeHint::OriginKind::ObjCRuntime;
        Hint->Signature.Parameters.push_back(
            {"selector", NdType::makePtr(NdType::makeVoid())});
        std::string Error;
        ASSERT_TRUE(
            assignDarwinObjCSourceABI(Hint->Signature, Architecture, Error));
        Call.addInput(MedVar::makeConst(0x1030, 8));
        Call.SourceCallHint = Hint;
        if (Mutation == 1)
          Hint->Signature.Origin =
              SourceFunctionTypeHint::OriginKind::NativeAnalysis;
        if (Mutation == 2)
          Hint->Signature.HasExplicitABI = false;
        if (Mutation == 3)
          Hint->DoesNotReturn = true;
        if (Mutation == 4)
          ++Call.OriginSeq;
        EXPECT_EQ(bool(F.inferVoid(Error)), Mutation == 0) << Error;
      }
}

TEST(NativeSourceHints, VoidFramesKeepBoundCallResultsInsideTheHelper) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      SCOPED_TRACE(Mutation);
      NativeVoidFrameFixture Fixture(Architecture);
      const auto &TRI = getTargetRegInfo(Architecture);
      auto Hint = std::make_shared<SourceCallTypeHint>(
          *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
      Hint->Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
      std::string Error;
      ASSERT_TRUE(
          assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error));
      auto Call = Fixture.Med.Blocks[0].Ops[0];
      Call.Addr = 0x100c;
      Call.OriginSeq = 3;
      Call.SourceCallHint = Hint;
      Call.Output.Kind = MedVar::Reg;
      Call.Output.Id = 30;
      Call.Output.SSAVer = 1;
      Call.Output.TheArch = Architecture;
      Call.Output.RegOff = TRI.IntReturnReg;
      Call.Output.Size = 8;
      // The returned object is consumed by the following void release. It
      // cannot become a result of the enclosing helper after that call.
      Fixture.Med.Blocks[0].Ops[0].Inputs[1] = Call.Output;
      Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Call);
      auto NativeCall = Fixture.Low.Blocks[0].Ops[Fixture.CallIndex];
      NativeCall.Addr = Call.Addr;
      NativeCall.Seq = Call.OriginSeq;
      Fixture.Low.Blocks[0].Ops.insert(
          Fixture.Low.Blocks[0].Ops.begin() + Fixture.CallIndex, NativeCall);
      if (Mutation == 1)
        Hint->Signature.ReturnLocation.ValueBytes = 4;
      if (Mutation == 2)
        Fixture.Med.Blocks[0].Ops[0].SourceCallHint.reset();
      if (Mutation == 3)
        ++Fixture.Low.Blocks[0].Ops[Fixture.CallIndex].Seq;
      if (Mutation == 4)
        Hint->DoesNotReturn = true;
      if (Mutation == 5) {
        // A typed result does not justify passing the private frame to a
        // callee or skipping the existing preservation/escape proof.
        Fixture.Low.Blocks[0].Ops.insert(
            Fixture.Low.Blocks[0].Ops.begin() + Fixture.CallIndex,
            NativeVoidFrameFixture::op(NdOp::COPY,
                                       NdVar::reg(TRI.IntParamRegs[0], 8),
                                       {NdVar::reg(TRI.StackPointer, 8)}));
      }
      const auto Result = Fixture.inferVoid(Error);
      if (Mutation) {
        EXPECT_FALSE(Result) << Error;
      } else {
        ASSERT_TRUE(Result) << Error;
        EXPECT_EQ(Result->ReturnType->Kind, NdTypeKind::Void);
        EXPECT_EQ(Result->ReturnLocation.Kind, SourceABICarrierKind::None);
        EXPECT_EQ(Result->ReturnLocation.ValueBytes, 0U);
      }
    }
}

TEST(NativeSourceHints, VoidFramesKeepSpillsAcrossDisjointExternalStores) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFrameFixture Fixture(Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    Fixture.Low.Blocks[0].Ops.insert(
        Fixture.Low.Blocks[0].Ops.begin() + Fixture.CallIndex,
        NativeVoidFrameFixture::op(
            NdOp::STORE, {},
            {NdVar::reg(TRI.IntParamRegs[0], 8), NdVar::cst(0, 8)}));
    std::string Error;
    const auto Result = Fixture.inferVoid(Error);
    ASSERT_TRUE(Result) << unsigned(Architecture) << ": " << Error;
    EXPECT_EQ(Result->ReturnType->Kind, NdTypeKind::Void);
  }
}

TEST(NativeSourceHints, VoidFramesRejectClobbersEscapesAndStaleSpills) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 19; ++Mutation) {
      NativeVoidFrameFixture Fixture(Architecture);
      auto &Ops = Fixture.Low.Blocks[0].Ops;
      const auto &TRI = getTargetRegInfo(Architecture);
      const auto SP = NdVar::reg(TRI.StackPointer, 8);
      const auto Word = NdVar::reg(Fixture.Saved[0], 8);
      using F = NativeVoidFrameFixture;
      switch (Mutation) {
      case 0:
        Ops[Fixture.RestoreIndex + 1].Output.Size = 4;
        break;
      case 1:
        Ops.insert(Ops.begin() + Fixture.CallIndex,
                   F::op(NdOp::STORE, {}, {SP, NdVar::cst(0, 1)}));
        break;
      case 2: {
        // A partially copied stack address remains a possible private-frame
        // alias even though it is not a complete, exact frame address.
        Ops.insert(
            Ops.begin() + Fixture.CallIndex,
            {F::op(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 4),
                   {NdVar::reg(TRI.StackPointer, 4)}),
             F::op(NdOp::STORE, {},
                   {NdVar::reg(TRI.IntParamRegs[0], 8), NdVar::cst(0, 8)})});
        break;
      }
      case 3:
        Ops.insert(Ops.begin() + Fixture.CallIndex,
                   F::op(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8), {SP}));
        break;
      case 4:
        Ops.insert(Ops.begin() + Fixture.CallIndex,
                   F::op(NdOp::STORE, {}, {SP, SP}));
        break;
      case 5:
        Ops[Ops.size() - 2].Inputs[1].Offset -= 8;
        break;
      case 6:
        Ops[Fixture.RestoreIndex + 1] =
            F::op(NdOp::COPY, Word, {NdVar::reg(TRI.IntReturnReg, 8)}, 0x1020);
        break;
      case 7:
        Ops[1].Inputs[1].Offset = Fixture.FrameBytes;
        break;
      case 8:
        Ops[2].MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      case 9:
        // The spill address is an instruction-local temporary, not a value
        // available merely because a later instruction reuses its number.
        Ops[2].Addr += 4;
        break;
      case 10: {
        const int64_t Exposed =
            Fixture.FrameBytes - (Architecture == Arch::X64 ? 8 : 0);
        Ops.insert(Ops.begin() + Fixture.CallIndex,
                   F::op(NdOp::INT_ADD, SP, {SP, NdVar::cst(Exposed, 8)}));
        Ops.insert(
            Ops.begin() + Fixture.RestoreIndex + 1,
            F::op(NdOp::INT_SUB, SP, {SP, NdVar::cst(Exposed, 8)}, 0x1020));
        break;
      }
      case 11:
        Fixture.splitReturns();
        Fixture.Low.Blocks[2].Ops[1].Output.Size = 4;
        break;
      case 12:
        Fixture.splitReturns();
        Fixture.Low.Blocks[2].Preds.clear();
        break;
      case 13:
        Fixture.splitReturns(true);
        Fixture.Low.Blocks[1].Ops = {
            F::op(NdOp::INT_SUB, SP, {SP, NdVar::cst(8, 8)})};
        break;
      case 14:
        if (Architecture == Arch::AArch64)
          Ops.back().Inputs[0] = NdVar::reg(TRI.IntReturnReg, 8);
        else
          Ops.insert(Ops.end() - 1,
                     F::op(NdOp::STORE, {}, {SP, NdVar::cst(0, 8)}, 0x1020));
        break;
      case 15:
        // A four-byte constant is zero-extended by the eight-byte ADD;
        // interpreting its high bit as a signed frame delta invents a spill.
        Ops[0].Opcode = NdOp::INT_ADD;
        Ops[0].Inputs[1] = NdVar::cst(uint32_t(-Fixture.FrameBytes), 4);
        break;
      case 16:
        Ops.insert(
            Ops.begin() + Fixture.CallIndex,
            F::op(NdOp::INDIR_BR, {}, {NdVar::reg(TRI.IntParamRegs[0], 8)}));
        break;
      case 17:
        // A stack address below the current SP is not allocated storage.
        Ops[1].Inputs[1] = NdVar::cst(uint64_t(-8), 8);
        break;
      case 18: {
        // A later instruction cannot retroactively allocate a stack slot.
        const auto StackWrite = Fixture.usePreindexedSpills();
        Ops[StackWrite].Addr += 4;
        break;
      }
      }
      std::string Error;
      EXPECT_FALSE(Fixture.inferVoid(Error))
          << unsigned(Architecture) << ": " << Mutation << ": " << Error;
    }
}

TEST(NativeSourceHints, VoidFrameByteProofHonorsImplicitZeroExtensions) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFrameFixture Fixture(Architecture);
    auto &Ops = Fixture.Low.Blocks[0].Ops;
    // Leave the original upper bytes untouched before loading only the low
    // word. That load implicitly clears the upper word on both architectures.
    Ops[3] = NativeVoidFrameFixture::op(NdOp::NOP, {}, {});
    Ops[Fixture.RestoreIndex + 1].Output.Size = 4;
    std::string Error;
    EXPECT_FALSE(Fixture.inferVoid(Error));
  }
}

std::pair<HighFunc, PipelineFunctionAudit>
nativeVoidInputCandidate(Arch Architecture) {
  const auto &TRI = getTargetRegInfo(Architecture);
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.Architecture = Architecture;
  Hint.HasExplicitABI = true;
  Hint.ReturnType = NdType::makeVoid();
  const auto Auxiliary =
      Architecture == Arch::AArch64 ? a64reg::X8 : x86reg::RAX;
  Hint.Parameters = {
      {"ordinary",
       NdType::makeInt(8),
       {SourceABICarrierKind::IntegerRegister, TRI.IntParamRegs[0], 0, 8}},
      {"auxiliary",
       NdType::makeInt(8),
       {SourceABICarrierKind::IntegerRegister, Auxiliary, 0, 8}}};
  HighFunc Function;
  Function.Entry = 0x1000;
  Function.SourceTypeHint = Hint;
  Function.ReturnType = Hint.ReturnType;
  Function.Params = {{"ordinary", NdType::makeInt(8)},
                     {"auxiliary", NdType::makeInt(8)}};
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Function.Body.push_back(Return);
  PipelineFunctionAudit Audit;
  Audit.Entry = Function.Entry;
  Audit.Disposition = PipelineFunctionDisposition::Accepted;
  Audit.HasLowIR = Audit.HasMedIR = Audit.MedIRVerified = true;
  Audit.DecodedInstructions = Audit.LiftedInstructions = 1;
  return {std::move(Function), std::move(Audit)};
}

TEST(NativeSourceHints, RefinesOnlyUnusedAuxiliaryVoidSourceInputs) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto [Function, Audit] = nativeVoidInputCandidate(Architecture);
    const auto Refined = refineNativeSourceTypeHint(Function, Audit);
    ASSERT_TRUE(Refined);
    ASSERT_EQ(Refined->Parameters.size(), 1U);
    EXPECT_EQ(Refined->Parameters[0].Name, "ordinary");
    EXPECT_EQ(Refined->Parameters[0].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[0]);
    EXPECT_EQ(Function.SourceTypeHint->Parameters.size(), 2U);
    std::string Error;
    EXPECT_TRUE(validateSourceABI(*Refined, Error)) << Error;
  }
}

TEST(NativeSourceHints, ScalarInputRefinementPreservesReturnEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto [Function, Audit] = nativeVoidInputCandidate(Architecture);
      auto &Hint = *Function.SourceTypeHint;
      Hint.ReturnType = Function.ReturnType = NdType::makeInt(8);
      Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                             getTargetRegInfo(Architecture).IntReturnReg, 0, 8};
      auto &Value = Function.Body.front().RetVal;
      Value = HighExpr::makeConst(19, 8);
      if (Mutation == 1) {
        MedVar Input;
        Input.Kind = MedVar::Param;
        Input.Id = 1;
        Input.Size = 8;
        Value = HighExpr::makeVar(Input, NdType::makeInt(8));
      } else if (Mutation == 2) {
        Value.reset();
      } else if (Mutation == 3) {
        Value = HighExpr::makeUndef(8);
      } else if (Mutation == 4) {
        Value = HighExpr::makeConst(19, 4);
      }
      const auto Refined = refineNativeSourceTypeHint(Function, Audit);
      if (Mutation) {
        EXPECT_FALSE(Refined);
        continue;
      }
      ASSERT_TRUE(Refined);
      ASSERT_EQ(Refined->Parameters.size(), 1U);
      EXPECT_EQ(Refined->Parameters[0].Name, "ordinary");
      EXPECT_TRUE(equalSourceTypes(Refined->ReturnType, Hint.ReturnType));
      EXPECT_EQ(Refined->ReturnLocation.RegisterOffset,
                Hint.ReturnLocation.RegisterOffset);
    }
}

TEST(NativeSourceHints, DefinedLocalsDoNotRetainUnusedPhysicalInputRegisters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto [Function, Audit] = nativeVoidInputCandidate(Architecture);
      auto &Hint = *Function.SourceTypeHint;
      Hint.ReturnType = Function.ReturnType = NdType::makeInt(8);
      Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                             getTargetRegInfo(Architecture).IntReturnReg, 0, 8};
      MedVar Local;
      Local.Kind = MedVar::Reg;
      Local.Id = 50001;
      Local.RegOff = Hint.Parameters[1].Location.RegisterOffset;
      Local.Size = 8;
      Local.TheArch = Architecture;
      Function.Body.front().RetVal =
          HighExpr::makeVar(Local, NdType::makeInt(8));
      HighStmt Assign;
      Assign.Kind = StmtKind::Assign;
      auto Dst = Local;
      if (Mutation == 4)
        Dst.Size = 4;
      Assign.Dst = HighExpr::makeVar(Dst, NdType::makeInt(Dst.Size));
      Assign.Val = HighExpr::makeConst(19, Dst.Size);
      if (Mutation == 2 || Mutation == 3) {
        HighStmt Branch;
        Branch.Kind = StmtKind::If;
        auto Input = Local;
        Input.Kind = MedVar::Param;
        Input.Id = 0;
        Input.RegOff = Hint.Parameters[0].Location.RegisterOffset;
        Branch.Cond = HighExpr::makeVar(Input, NdType::makeInt(8));
        Branch.Body = {Assign};
        if (Mutation == 3)
          Branch.ElseBody = {Assign};
        Function.Body.insert(Function.Body.begin(), Branch);
      } else if (Mutation != 1) {
        Function.Body.insert(Function.Body.begin(), Assign);
      }
      const auto Refined = refineNativeSourceTypeHint(Function, Audit);
      EXPECT_EQ(bool(Refined), Mutation == 0);
      if (Refined)
        EXPECT_EQ(Refined->Parameters.size(), 1U);
    }
}

TEST(NativeSourceHints, SourceInputRefinementRetainsUsesAndIncompleteBodies) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 23; ++Mutation) {
      auto [Function, Audit] = nativeVoidInputCandidate(Architecture);
      MedVar Input;
      Input.Kind = MedVar::Param;
      Input.Id = 1;
      Input.Size = 8;
      Input.TheArch = Architecture;
      Input.RegOff =
          Function.SourceTypeHint->Parameters[1].Location.RegisterOffset;
      auto Use = HighExpr::makeVar(Input, NdType::makeInt(8));
      HighStmt Statement;
      Statement.Kind = StmtKind::ExprStmt;
      Statement.Val = Use;
      switch (Mutation) {
      case 0:
        Function.Body.insert(Function.Body.begin(), Statement);
        break;
      case 1:
        Statement.Kind = StmtKind::Assign;
        Statement.Dst = Use;
        Statement.Val = HighExpr::makeConst(0, 8);
        Function.Body.insert(Function.Body.begin(), Statement);
        break;
      case 2:
        Function.Body[0].DefaultBody.push_back(Statement);
        break;
      case 3:
        Function.Body[0].EHClauseBodies.push_back({Statement});
        break;
      case 4:
        Function.Body[0].Cond = HighExpr::makeConst(0, 8);
        Function.Body[0].Cond->IntrinsicOutputs.push_back(Input);
        break;
      case 5:
        Input.Kind = MedVar::Reg;
        Input.SSAVer = 2;
        Input.RegOff += 4;
        Input.Size = 4;
        Function.Body[0].Cond = HighExpr::makeVar(Input, NdType::makeInt(4));
        break;
      case 6:
        Input.Id = 4;
        Function.Body[0].Cond = HighExpr::makeVar(Input, NdType::makeInt(8));
        break;
      case 7:
        Function.Params[1].Type = NdType::makeInt(4);
        break;
      case 8:
        Function.SourceTypeHint->Origin =
            SourceFunctionTypeHint::OriginKind::ObjCSDK;
        break;
      case 9:
        Function.Body.clear();
        break;
      case 10:
        Function.StructuredExceptionRegions = 1;
        break;
      case 11:
        Function.Body[0].Kind = StmtKind::Nop;
        break;
      case 12:
        Function.ReturnType = NdType::makeInt(8);
        break;
      case 13:
        Function.Body[0].Cond = HighExpr::makeConst(0, 8);
        Function.Body[0].Cond->Kind = ExprKind::Undef;
        break;
      case 14:
        Function.SourceTypeHint.reset();
        break;
      case 15: {
        auto Expression = HighExpr::makeConst(0, 8);
        for (unsigned I = 0; I < 130; ++I) {
          auto Outer = std::make_shared<HighExpr>();
          Outer->Kind = ExprKind::Cast;
          Outer->Type = NdType::makeInt(8);
          Outer->Operands = {Expression};
          Expression = Outer;
        }
        Function.Body[0].Cond = Expression;
        break;
      }
      case 16:
        Audit.Disposition = PipelineFunctionDisposition::Candidate;
        break;
      case 17:
        Audit.HasLowIR = false;
        break;
      case 18:
        Audit.DecodeFailures.push_back(0x1000);
        break;
      case 19:
        ++Audit.Entry;
        break;
      case 20:
        Audit.TruncatedPaths.push_back(0x1000);
        break;
      case 21:
        Function.Body[0].Cases.resize(65537);
        break;
      case 22:
        Function.Body[0].Cond = HighExpr::makeConst(0, 8);
        Function.Body[0].Cond->IntrinsicOutputs.resize(65537);
        break;
      }
      EXPECT_FALSE(refineNativeSourceTypeHint(Function, Audit)) << Mutation;
    }
}

struct NativeFloatingFixture : NativeFixture {
  NativeFloatingFixture(Arch Architecture, unsigned Width, bool Wide = false)
      : NativeFixture(Architecture) {
    const auto &TRI = getTargetRegInfo(Architecture);
    Med.ReturnType = High.ReturnType = NdType::makeFloat(Width);
    for (unsigned I = 0; I < 2; ++I) {
      Med.Params[I].RegOff = TRI.FPParamRegs[I];
      Med.Params[I].Size = Wide ? 16 : Width;
      Med.TypedParams[I].Type = High.Params[I].Type =
          NdType::makeInt(Med.Params[I].Size);
    }
    auto &Ops = Med.Blocks[0].Ops;
    auto Sum = Ops.front();
    auto Return = Ops.back();
    Sum.Opcode = NdOp::FLOAT_ADD;
    Sum.Output.RegOff = TRI.FPReturnReg;
    Sum.Output.Size = Width;
    Sum.Inputs[0] = Med.Params[0];
    Sum.Inputs[1] = Med.Params[1];
    Ops.clear();
    if (Wide) {
      for (unsigned I = 0; I < 2; ++I) {
        MedOp Extract;
        Extract.Opcode = NdOp::SUBBYTES;
        Extract.Output = Sum.Output;
        Extract.Output.Kind = MedVar::Temp;
        Extract.Output.Id = 20 + I;
        Extract.addInput(Med.Params[I]);
        Extract.addInput(MedVar::makeConst(0, 8));
        Ops.push_back(Extract);
        Sum.Inputs[I] = Extract.Output;
      }
      Sum.Output.Kind = MedVar::Temp;
      Ops.push_back(Sum);
      auto Upper = Ops.front();
      Upper.Output.Id = 22;
      Upper.Output.Size = 16 - Width;
      Upper.Inputs[1].ConstVal = Width;
      Ops.push_back(Upper);
      MedOp Join;
      Join.Opcode = NdOp::CONCAT;
      Join.Output = Sum.Output;
      Join.Output.Kind = MedVar::Reg;
      Join.Output.Id = 23;
      Join.Output.Size = 16;
      Join.addInput(Upper.Output);
      Join.addInput(Sum.Output);
      Ops.push_back(Join);
    } else {
      Ops.push_back(Sum);
    }
    Ops.push_back(Return);
  }
};

TEST(NativeSourceHints, FloatingLanesPreserveScalarParametersAndResults) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Width : {4U, 8U})
      for (bool Wide : {false, true}) {
        NativeFloatingFixture Fixture(Architecture, Width, Wide);
        std::string Error;
        const auto Hint = Fixture.infer(Error);
        ASSERT_TRUE(Hint) << Error;
        EXPECT_EQ(Hint->ReturnLocation.Kind,
                  SourceABICarrierKind::FloatingRegister);
        EXPECT_EQ(Hint->ReturnLocation.RegisterOffset,
                  getTargetRegInfo(Architecture).FPReturnReg);
        EXPECT_EQ(Hint->ReturnLocation.ValueBytes, Width);
        ASSERT_EQ(Hint->Parameters.size(), 2U);
        const auto Bytes = observedMedSourceEntryBytes(Fixture.Med, *Hint);
        ASSERT_TRUE(Bytes);
        for (unsigned I = 0; I < 2; ++I) {
          EXPECT_EQ(Bytes->at(getTargetRegInfo(Architecture).FPParamRegs[I]),
                    (uint64_t(1) << Width) - 1);
          EXPECT_EQ(Hint->Parameters[I].Type->Kind, NdTypeKind::Float);
          EXPECT_EQ(Hint->Parameters[I].Location.RegisterOffset,
                    getTargetRegInfo(Architecture).FPParamRegs[I]);
          EXPECT_EQ(Hint->Parameters[I].Location.ValueBytes, Width);
          EXPECT_EQ(Fixture.Med.TypedParams[I].Type->Kind, NdTypeKind::Int);
          EXPECT_EQ(Fixture.Med.Params[I].Size, Wide ? 16 : Width);
        }
        EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                  MedReturnValueEvidence::Unknown);
      }
}

TEST(NativeSourceHints, FloatingReturnPathsRequireCompleteDefinedLanes) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      NativeFloatingFixture Fixture(Architecture, 8);
      auto &Ops = Fixture.Med.Blocks[0].Ops;
      switch (Mutation) {
      case 0:
        Ops[0].Output.Size = 4;
        break;
      case 1:
        Ops[0].Output.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
        Fixture.Med.Params[0].RegOff =
            getTargetRegInfo(Architecture).FPParamRegs[2];
        Ops[0].Inputs[0] = Fixture.Med.Params[0];
        break;
      case 2:
        Ops[0].Output.RegOff = getTargetRegInfo(Architecture).FPParamRegs[1];
        Fixture.Med.Params[0].RegOff =
            getTargetRegInfo(Architecture).FPParamRegs[2];
        Ops[0].Inputs[0] = Fixture.Med.Params[0];
        break;
      case 3: {
        MedOp Call;
        Call.Opcode = NdOp::CALL;
        Call.addInput(MedVar::makeConst(0x1080, 8));
        auto Hint = std::make_shared<SourceCallTypeHint>();
        Hint->Signature.ReturnType = NdType::makeVoid();
        std::string Error;
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error));
        Call.SourceCallHint = std::move(Hint);
        Ops.insert(Ops.end() - 1, Call);
        break;
      }
      case 4: {
        const auto Return = Ops.back();
        Fixture.Med.Blocks.emplace_back();
        Fixture.Med.Blocks.back().Id = 1;
        Fixture.Med.Blocks.back().Ops = {Return};
        break;
      }
      }
      std::string Error;
      EXPECT_FALSE(Fixture.infer(Error)) << Mutation << ": " << Error;
    }
}

TEST(NativeSourceHints, FloatingParametersRequireBoundedScalarEntryBytes) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      NativeFloatingFixture Fixture(Architecture, 8, true);
      auto &Ops = Fixture.Med.Blocks[0].Ops;
      switch (Mutation) {
      case 0: {
        MedOp Store;
        Store.Opcode = NdOp::STORE;
        Store.addInput(MedVar::makeConst(0x1080, 8));
        Store.addInput(Fixture.Med.Params[0]);
        Ops.insert(Ops.end() - 1, Store);
        break;
      }
      case 1:
        Ops[0].Inputs[1].ConstVal = 8;
        break;
      case 2:
        Fixture.Med.Params[0].RegOff =
            getTargetRegInfo(Architecture).IntParamRegs[0];
        break;
      case 3:
        Fixture.Med.Params[0].RegOff = Fixture.Med.Params[1].RegOff;
        break;
      case 4:
        Fixture.Med.TypedParams[0].Type = NdType::makePtr(NdType::makeVoid());
        break;
      case 5:
        Ops[0].Inputs[0].Size = 4;
        break;
      }
      std::string Error;
      EXPECT_FALSE(Fixture.infer(Error)) << Mutation << ": " << Error;
    }
}

struct NativeRecordResultFixture : NativeFixture {
  explicit NativeRecordResultFixture(Arch Architecture, bool Swift = false)
      : NativeFixture(Architecture) {
    Med.Params.clear();
    Med.TypedParams.clear();
    High.Params.clear();
    Med.ReturnType = High.ReturnType = NdType::makeInt(8, false);
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    Hint->Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
    Hint->Signature.ReturnType = NdType::makeStruct(
        {NdType::makePtr(NdType::makeVoid()), NdType::makeInt(8, false)});
    std::string Error;
    const bool Assigned =
        Swift
            ? assignDarwinSwiftSourceABI(Hint->Signature, Architecture, Error)
            : assignDarwinFixedSourceABI(Hint->Signature, Architecture, Error);
    EXPECT_TRUE(Assigned) << Error;
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Output.Kind = MedVar::Temp;
    Call.Output.Id = 100;
    Call.Output.SSAVer = 1;
    Call.Output.Size = 16;
    Call.Output.TheArch = Architecture;
    Call.SourceCallHint = Hint;
    Call.addInput(MedVar::makeConst(0x1080, 8));
    const auto Return = Med.Blocks[0].Ops.back();
    Med.Blocks[0].Ops = {Call};
    const auto &TRI = getTargetRegInfo(Architecture);
    for (unsigned I = 0; I < 2; ++I) {
      MedOp Extract;
      Extract.Opcode = NdOp::SUBBYTES;
      Extract.Output = Call.Output;
      Extract.Output.Kind = MedVar::Reg;
      Extract.Output.Id = 101 + I;
      Extract.Output.Size = 8;
      Extract.Output.RegOff = TRI.IntReturnRegs[I];
      Extract.addInput(Call.Output);
      Extract.addInput(MedVar::makeConst(I * 8, 8));
      Med.Blocks[0].Ops.push_back(Extract);
    }
    Med.Blocks[0].Ops.push_back(Return);
  }
};

TEST(NativeSourceHints, BoundRecordArgumentsUsePhysicalComponents) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool PairResult : {false, true})
      for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
        SCOPED_TRACE(Mutation);
        NativeRecordResultFixture F(Architecture);
        auto &Call = F.Med.Blocks[0].Ops[0];
        auto Binding =
            std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
        Binding->Signature.Parameters = {
            {"range", NdType::makeStruct({NdType::makeInt(8, false),
                                          NdType::makeInt(8, false)})}};
        std::string Error;
        ASSERT_TRUE(assignDarwinFixedSourceABI(Binding->Signature, Architecture,
                                               Error));
        Call.addInput(MedVar::makeConst(17, 8));
        if (Mutation != 1)
          Call.addInput(MedVar::makeConst(31, 8));
        if (Mutation == 2)
          Call.addInput(MedVar::makeConst(91, 8));
        if (Mutation == 3)
          Binding->Signature.Parameters[0].Components.pop_back();
        Call.SourceCallHint = Binding;
        const auto Hint = inferNativeSourceTypeHint(
            F.Image, F.Med, F.High, F.Audit, Error, nullptr, PairResult);
        EXPECT_EQ(bool(Hint), Mutation == 0) << Error;
        if (Hint)
          EXPECT_EQ(Hint->ReturnComponents.size(), PairResult ? 2U : 0U);
      }
}

TEST(NativeSourceHints, TypedRecordCallResultsDefineTheNativeReturnCarrier) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Swift : {false, true})
      for (bool SeparateReturn : {false, true}) {
        NativeRecordResultFixture Fixture(Architecture, Swift);
        if (SeparateReturn) {
          const auto Return = Fixture.Med.Blocks[0].Ops.back();
          Fixture.Med.Blocks[0].Ops.pop_back();
          Fixture.Med.Blocks[0].Succs = {1};
          Fixture.Med.Blocks.emplace_back();
          Fixture.Med.Blocks[1].Id = 1;
          Fixture.Med.Blocks[1].Preds = {0};
          Fixture.Med.Blocks[1].Ops = {Return};
        }
        std::string Error;
        const auto Hint = Fixture.infer(Error);
        ASSERT_TRUE(Hint) << Error;
        EXPECT_EQ(Hint->ReturnLocation.RegisterOffset,
                  getTargetRegInfo(Architecture).IntReturnReg);
        EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 8U);
        EXPECT_TRUE(Hint->Parameters.empty());
        EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                  MedReturnValueEvidence::Unknown);
      }
}

TEST(NativeSourceHints, IntegerPairReturnsPreserveBothWordsOfBoundRecordCalls) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Swift : {false, true}) {
      NativeRecordResultFixture F(Architecture, Swift);
      std::string Error;
      const auto Pair = inferNativeSourceTypeHint(
          F.Image, F.Med, F.High, F.Audit, Error, nullptr, true);
      ASSERT_TRUE(Pair) << Error;
      EXPECT_EQ(Pair->ReturnType->Kind, NdTypeKind::Struct);
      EXPECT_EQ(Pair->ReturnComponents.size(), 2U);
      // A later scalar call clobbers the second result even when its first
      // result is a complete eight-byte value.
      auto Call = F.Med.Blocks[0].Ops[0];
      auto Binding = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
      Binding->Signature.ReturnType = NdType::makeInt(8);
      ASSERT_TRUE(
          assignDarwinFixedSourceABI(Binding->Signature, Architecture, Error));
      Call.SourceCallHint = Binding;
      Call.Output = F.Med.Blocks[0].Ops[1].Output;
      F.Med.Blocks[0].Ops.insert(F.Med.Blocks[0].Ops.end() - 1, Call);
      const auto Scalar = inferNativeSourceTypeHint(
          F.Image, F.Med, F.High, F.Audit, Error, nullptr, true);
      ASSERT_TRUE(Scalar) << Error;
      EXPECT_TRUE(Scalar->ReturnComponents.empty());
    }
}

TEST(NativeSourceHints, RecordResultDefinitionsRequireTheCompleteCallPrefix) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 15; ++Mutation) {
      SCOPED_TRACE(Mutation);
      NativeRecordResultFixture Fixture(Architecture);
      auto &Ops = Fixture.Med.Blocks[0].Ops;
      switch (Mutation) {
      case 0:
        Ops.erase(Ops.begin() + 2);
        break;
      case 1:
        std::swap(Ops[1], Ops[2]);
        break;
      case 2:
        Ops[1].Inputs[1].ConstVal = 8;
        break;
      case 3:
        ++Ops[1].Inputs[0].Id;
        break;
      case 4:
        ++Ops[1].Inputs[0].SSAVer;
        break;
      case 5:
        Ops[1].Inputs[0].Size = 8;
        break;
      case 6:
        Ops[1].Output.Size = 4;
        break;
      case 7:
        Ops[2].Output.Size = 4;
        break;
      case 8:
        Ops[1].Output.RegOff = getTargetRegInfo(Architecture).IntReturnRegs[1];
        break;
      case 9:
        Ops[0].Output.Size = 8;
        break;
      case 10:
        Ops[0].SourceCallHint.reset();
        break;
      case 11: {
        auto Hint =
            std::make_shared<SourceCallTypeHint>(*Ops[0].SourceCallHint);
        Hint->Signature.ReturnComponents.pop_back();
        Ops[0].SourceCallHint = std::move(Hint);
        break;
      }
      case 12: {
        auto Copy = Ops[1];
        Copy.Opcode = NdOp::COPY;
        Copy.Output = Ops[0].Output;
        Copy.Output.Id = 200;
        Copy.NumInputs = 1;
        Ops.insert(Ops.begin() + 2, Copy);
        break;
      }
      case 13: {
        auto Clobber = Ops[0];
        auto Hint = std::make_shared<SourceCallTypeHint>();
        Hint->Signature.ReturnType = NdType::makeVoid();
        std::string Error;
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error));
        Clobber.SourceCallHint = std::move(Hint);
        Clobber.Output = {};
        Ops.insert(Ops.end() - 1, Clobber);
        break;
      }
      case 14:
        Ops[2].Inputs[1].ConstVal = 0;
        break;
      }
      std::string Error;
      EXPECT_FALSE(Fixture.infer(Error)) << Error;
    }
}

struct NativeContextFixture : NativeFixture {
  LowFunc Low;
  MedVar Context;

  explicit NativeContextFixture(Arch Architecture, uint64_t Register)
      : NativeFixture(Architecture) {
    Med.Params[1].Id = -1;
    Context.Kind = MedVar::Reg;
    Context.Id = 200;
    Context.RegOff = Register;
    Context.Size = 8;
    Context.TheArch = Architecture;
    MedOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output = Context;
    Load.Output.Kind = MedVar::Temp;
    Load.Output.Id = 201;
    Load.Output.Size = 4;
    Load.addInput(Context);
    Med.Blocks[0].Ops[0].Inputs[1] = Load.Output;
    Med.Blocks[0].Ops.insert(Med.Blocks[0].Ops.begin(), Load);
    Low.Entry = Med.Entry;
    Low.Blocks.emplace_back();
    Low.Blocks[0].Id = 0;
    LowOp NativeLoad;
    NativeLoad.Opcode = NdOp::LOAD;
    NativeLoad.Output = NdVar::tmp(TmpBase, 4);
    NativeLoad.addInput(NdVar::reg(Register, 8));
    Low.Blocks[0].Ops.push_back(NativeLoad);
  }

  std::optional<SourceFunctionTypeHint> inferContext(std::string &Error) const {
    return inferNativeSourceTypeHint(Image, Med, High, Audit, Error, &Low);
  }
};

TEST(NativeSourceHints, ReadOnlyContextsRetainObservedPreservedRegisters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    unsigned Checked = 0;
    for (uint64_t Register : TRI.CalleeSaveRegs) {
      if (TRI.isFrameOrLinkReg(Register) || TRI.isVectorReg(Register))
        continue;
      NativeContextFixture Fixture(Architecture, Register);
      std::string Error;
      auto Hint = Fixture.inferContext(Error);
      ASSERT_TRUE(Hint) << Error;
      ASSERT_EQ(Hint->Parameters.size(), 2U) << Register;
      EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset,
                TRI.IntParamRegs[0]);
      EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, Register);
      EXPECT_EQ(Hint->Parameters[1].Location.ValueBytes, 8U);
      EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::C);
      EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                MedReturnValueEvidence::Unknown);
      auto WithoutNativeProof = Fixture.infer(Error);
      ASSERT_TRUE(WithoutNativeProof) << Error;
      EXPECT_EQ(WithoutNativeProof->Parameters.size(), 1U);
      ++Checked;
    }
    EXPECT_GT(Checked, 0U);
  }
}

TEST(NativeSourceHints, AuxiliaryInputsAllowLaterCallerSavedWrites) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto Register =
        Architecture == Arch::AArch64 ? a64reg::X8 : x86reg::R10;
    for (bool RecordedParameter : {false, true}) {
      NativeContextFixture Fixture(Architecture, Register);
      if (RecordedParameter) {
        Fixture.Context.Kind = MedVar::Param;
        Fixture.Med.Params[1] = Fixture.Context;
        Fixture.Med.TypedParams[1].Type = NdType::makeInt(8);
        Fixture.High.Params[1].Type = NdType::makeInt(8);
        Fixture.Med.Blocks[0].Ops[0].Inputs[0] = Fixture.Context;
      }
      LowOp Write;
      Write.Opcode = NdOp::COPY;
      Write.Output = NdVar::reg(Register, 8);
      Write.addInput(NdVar::cst(7, 8));
      Fixture.Low.Blocks[0].Ops.push_back(Write);
      std::string Error;
      const auto Hint = Fixture.inferContext(Error);
      ASSERT_TRUE(Hint) << Error;
      ASSERT_EQ(Hint->Parameters.size(), 2U);
      EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, Register);
      EXPECT_EQ(Hint->Parameters[1].Location.ValueBytes, 8U);
      EXPECT_FALSE(Hint->Parameters[1].Location.ExtendTo32Bits);
      EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                MedReturnValueEvidence::Unknown);
      EXPECT_EQ(bool(Fixture.infer(Error)), !RecordedParameter);
    }
  }
}

TEST(NativeSourceHints, AuxiliaryParametersRequireCompleteNativeReadEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto Register =
        Architecture == Arch::AArch64 ? a64reg::X8 : x86reg::R10;
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      NativeContextFixture Fixture(Architecture, Register);
      Fixture.Context.Kind = MedVar::Param;
      Fixture.Med.Params[1] = Fixture.Context;
      Fixture.Med.TypedParams[1].Type = NdType::makeInt(8);
      Fixture.High.Params[1].Type = NdType::makeInt(8);
      Fixture.Med.Blocks[0].Ops[0].Inputs[0] = Fixture.Context;
      if (Mutation == 0)
        Fixture.Low.Blocks[0].Ops[0].Inputs[0].Size = 4;
      else if (Mutation == 1)
        Fixture.Low.Blocks[0].Ops.clear();
      else if (Mutation == 2)
        ++Fixture.Low.Entry;
      else if (Mutation == 3)
        Fixture.Low.Blocks[0].Ops[0].NumInputs = 7;
      else {
        Fixture.Med.Params[1].Size = 4;
        Fixture.Med.TypedParams[1].Type = NdType::makeInt(4);
      }
      std::string Error;
      EXPECT_FALSE(Fixture.inferContext(Error)) << Mutation;
    }
  }
}

TEST(NativeSourceHints, AuxiliaryCallClobbersCannotBecomeEntryParameters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto Register =
        Architecture == Arch::AArch64 ? a64reg::X8 : x86reg::R10;
    NativeContextFixture Fixture(Architecture, Register);
    Fixture.Med.CallClobbers.push_back({Fixture.Context, 1});
    std::string Error;
    const auto Hint = Fixture.inferContext(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->Parameters.size(), 1U);
  }
}

TEST(NativeSourceHints,
     ContextInputsSurviveProvenRestorationOfScratchRegisters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    std::vector<uint64_t> Preserved;
    for (auto R : TRI.CalleeSaveRegs)
      if (!TRI.isFrameOrLinkReg(R) && !TRI.isVectorReg(R))
        Preserved.push_back(R);
    ASSERT_GE(Preserved.size(), 2U);
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      SCOPED_TRACE(Mutation);
      NativeContextFixture F(Architecture, Preserved[0]);
      auto &Ops = F.Low.Blocks[0].Ops;
      LowOp Save;
      Save.Opcode = NdOp::COPY;
      Save.Output = NdVar::tmp(TmpBase + 16, 8);
      Save.addInput(NdVar::reg(Preserved[1], 8));
      LowOp Write;
      Write.Opcode = NdOp::COPY;
      Write.Output = NdVar::reg(Preserved[1], 8);
      Write.addInput(NdVar::cst(19, 8));
      LowOp Restore;
      Restore.Opcode = NdOp::COPY;
      Restore.Output = Write.Output;
      Restore.addInput(Save.Output);
      LowOp Return;
      Return.Opcode = NdOp::RETURN;
      if (Architecture == Arch::AArch64)
        Return.addInput(NdVar::reg(TRI.LinkRegister, 8));
      Ops.insert(Ops.begin(), Save);
      Ops.push_back(Write);
      if (Mutation == 2)
        Restore.Output.Size = Restore.Inputs[0].Size = 4;
      if (Mutation != 1)
        Ops.push_back(Restore);
      if (Mutation == 3) {
        Write.Output = NdVar::reg(TRI.StackPointer, 8);
        Ops.push_back(Write);
      }
      Ops.push_back(Return);
      if (Mutation == 4)
        F.Low.Blocks[0].ExceptionalSuccs.emplace_back();
      if (Mutation == 5) {
        F.Low.Blocks[0].Succs = {99};
      }
      std::string Error;
      const auto Hint = F.inferContext(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters.size(), Mutation == 0 ? 2U : 1U);
    }
  }
}

TEST(NativeSourceHints, ContextProofRejectsClobbersAndIncompleteNativeReads) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto ContextRegister = *std::find_if(
        TRI.CalleeSaveRegs.begin(), TRI.CalleeSaveRegs.end(), [&](uint64_t R) {
          return !TRI.isFrameOrLinkReg(R) && !TRI.isVectorReg(R);
        });
    for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
      NativeContextFixture Fixture(Architecture, ContextRegister);
      auto &Ops = Fixture.Low.Blocks[0].Ops;
      if (Mutation < 3) {
        LowOp Write;
        Write.Opcode = NdOp::COPY;
        const auto Offset =
            Mutation == 2 ? ContextRegister + 1 : ContextRegister;
        Write.Output = NdVar::reg(Offset, Mutation ? 1 : 8);
        Write.addInput(NdVar::cst(0, Write.Output.Size));
        Ops.push_back(Write);
      } else if (Mutation == 3) {
        ++Fixture.Low.Entry;
      } else if (Mutation == 4) {
        Ops[0].Inputs[0] = NdVar::cst(0, 8);
      } else if (Mutation == 5) {
        Ops[0].Inputs[0].Size = 4;
      } else if (Mutation == 6) {
        Fixture.Low.Blocks.resize(16385);
      } else if (Mutation == 7) {
        Ops[0].NumInputs = 7;
      } else {
        LowOp Write;
        Write.Opcode = NdOp::COPY;
        auto Other = std::find_if(TRI.CalleeSaveRegs.begin(),
                                  TRI.CalleeSaveRegs.end(), [&](uint64_t R) {
                                    return R != ContextRegister &&
                                           !TRI.isFrameOrLinkReg(R) &&
                                           !TRI.isVectorReg(R);
                                  });
        ASSERT_NE(Other, TRI.CalleeSaveRegs.end());
        Write.Output = NdVar::reg(*Other, 8);
        Write.addInput(NdVar::cst(0, 8));
        Ops.push_back(Write);
      }
      std::string Error;
      auto Hint = Fixture.inferContext(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters.size(), 1U) << Mutation;
    }
  }
}

TEST(NativeSourceHints, ContextDemandExcludesSeedsAndInternalDefinitions) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto Register = *std::find_if(
        TRI.CalleeSaveRegs.begin(), TRI.CalleeSaveRegs.end(), [&](uint64_t R) {
          return !TRI.isFrameOrLinkReg(R) && !TRI.isVectorReg(R);
        });
    for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
      NativeContextFixture Fixture(Architecture, Register);
      if (Mutation == 0) {
        auto &Load = Fixture.Med.Blocks[0].Ops[0];
        Load.Opcode = NdOp::COPY;
        Load.Output = Fixture.Context;
        Fixture.Med.Blocks[0].Ops[1].Inputs[1] = MedVar::makeConst(7, 4);
      } else if (Mutation == 1) {
        MedOp Define;
        Define.Opcode = NdOp::COPY;
        Define.Output = Fixture.Context;
        Define.addInput(MedVar::makeConst(0, 8));
        Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(),
                                         Define);
      } else {
        PhiNode Phi;
        Phi.Output = Fixture.Context;
        Phi.Args = {{0, MedVar::makeConst(0, 8)}};
        Fixture.Med.Blocks[0].Phis.push_back(Phi);
      }
      std::string Error;
      auto Hint = Fixture.inferContext(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters.size(), 1U) << Mutation;
    }
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

// The source hint consumes already verified MedIR. These graphs isolate the
// all-path carrier proof; the runtime fixture exercises real lifting and SSA.
void returnDiamond(NativeFixture &Fixture) {
  const auto Compute = Fixture.Med.Blocks[0].Ops.front();
  const auto Return = Fixture.Med.Blocks[0].Ops.back();
  Fixture.Med.Blocks.resize(4);
  for (unsigned I = 0; I < 4; ++I) {
    auto &Block = Fixture.Med.Blocks[I];
    Block.Id = I;
    Block.StartAddr = Fixture.Med.Entry + I * 16;
    Block.Ops.clear();
  }
  auto &Blocks = Fixture.Med.Blocks;
  Blocks[0].Succs = {1, 2};
  for (unsigned I : {1U, 2U}) {
    Blocks[I].Preds = {0};
    Blocks[I].Succs = {3};
    Blocks[I].Ops = {Compute};
  }
  Blocks[3].Preds = {1, 2};
  Blocks[3].Ops = {Return};
}

TEST(NativeSourceHints, ComputedReturnsMergeAcrossEveryPathAndBlockOrder) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Complete : {false, true}) {
      NativeFixture Fixture(Architecture);
      // Isolate computed results from the separately proven entry carrier.
      Fixture.Med.Params[0].Id = -1;
      Fixture.Med.Blocks[0].Ops[0].Inputs[0] = MedVar::makeConst(3, 4);
      returnDiamond(Fixture);
      if (!Complete)
        Fixture.Med.Blocks[2].Ops.clear();
      const auto Blocks = Fixture.Med.Blocks;
      std::array<unsigned, 4> Order{0, 1, 2, 3};
      do {
        for (unsigned I = 0; I < 4; ++I)
          Fixture.Med.Blocks[I] = Blocks[Order[I]];
        std::string Error;
        EXPECT_EQ(bool(Fixture.infer(Error)), Complete) << Error;
        EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                  MedReturnValueEvidence::Unknown);
      } while (std::next_permutation(Order.begin(), Order.end()));
    }
  }
}

TEST(NativeSourceHints, ReturnLoopsRequireComputationOnTheirEntryPath) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Seeded : {false, true}) {
      NativeFixture Fixture(Architecture);
      Fixture.Med.Params[0].Id = -1;
      Fixture.Med.Blocks[0].Ops[0].Inputs[0] = MedVar::makeConst(3, 4);
      returnDiamond(Fixture);
      auto &Blocks = Fixture.Med.Blocks;
      // Entry -> header -> body -> header, or header -> return. Computing
      // only in the body cannot prove a result on the zero-trip path.
      if (Seeded)
        Blocks[0].Ops = Blocks[2].Ops;
      Blocks[0].Succs = {1};
      Blocks[1].Ops.clear();
      Blocks[1].Preds = {0, 2};
      Blocks[1].Succs = {2, 3};
      Blocks[2].Preds = {1};
      Blocks[2].Succs = {1};
      Blocks[3].Preds = {1};
      std::string Error;
      EXPECT_EQ(bool(Fixture.infer(Error)), Seeded) << Error;
      // A disconnected cycle cannot establish its own incoming fact either.
      Blocks[0].Succs.clear();
      Blocks[1].Preds = {2};
      EXPECT_FALSE(Fixture.infer(Error));
    }
  }
}

TEST(NativeSourceHints, ReturnPathsInvalidateCallsAndPartialCarrierWrites) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      NativeFixture Fixture(Architecture);
      returnDiamond(Fixture);
      auto &Ops = Fixture.Med.Blocks[2].Ops;
      MedOp Overwrite = Ops.front();
      Overwrite.Opcode = NdOp::COPY;
      Overwrite.NumInputs = 1;
      Overwrite.Inputs[0] = MedVar::makeConst(7, 4);
      if (Mutation == 0)
        Overwrite.Output.Size = 2;
      if (Mutation == 1) {
        ++Overwrite.Output.RegOff;
        Overwrite.Output.Size = 1;
      }
      if (Mutation == 2) {
        Overwrite.Opcode = NdOp::CALL;
        Overwrite.Output = {};
        Overwrite.Inputs[0] = MedVar::makeConst(0x1020, 8);
        auto Hint = std::make_shared<SourceCallTypeHint>();
        Hint->Signature.ReturnType = NdType::makeVoid();
        std::string Error;
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error))
            << Error;
        Overwrite.SourceCallHint = std::move(Hint);
      }
      if (Mutation == 3)
        Overwrite.Inputs[0] = Overwrite.Output; // SSA seed, not a write.
      if (Mutation == 4) {
        Overwrite.Opcode = NdOp::SUBBYTES; // Register view, not a write.
        Overwrite.Output.Size = 1;
        Overwrite.addInput(MedVar::makeConst(0, 4));
      }
      Ops.push_back(Overwrite);
      std::string Error;
      EXPECT_EQ(bool(Fixture.infer(Error)), Mutation >= 3) << Mutation << Error;
      // A subsequent complete computation reestablishes the carrier.
      Ops.push_back(Ops.front());
      EXPECT_TRUE(Fixture.infer(Error)) << Mutation << Error;
    }
  }
}

TEST(NativeSourceHints, ReturnPathsRejectUnobservedInputAndEpilogueRestores) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    returnDiamond(Fixture);
    for (unsigned I : {1U, 2U}) {
      auto &Op = Fixture.Med.Blocks[I].Ops.front();
      Op.Opcode = NdOp::COPY;
      Op.NumInputs = 1;
      Op.Inputs[0] = Op.Output;
    }
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error));
  }
  NativeFixture Fixture(Arch::X64);
  returnDiamond(Fixture);
  auto &Restore = Fixture.Med.Blocks[2].Ops.front();
  Restore.Opcode = NdOp::LOAD;
  Restore.NumInputs = 1;
  Restore.Inputs[0] = Restore.Output;
  Restore.Inputs[0].RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  std::string Error;
  EXPECT_FALSE(Fixture.infer(Error));
}

TEST(NativeSourceHints, ObservedIncomingResultsKeepExactWidthsAndLocations) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (uint16_t Width : {1, 2, 4, 8}) {
      NativeFixture Fixture(Architecture);
      Fixture.Med.ReturnType = Fixture.High.ReturnType = NdType::makeInt(Width);
      for (unsigned I = 0; I < 2; ++I) {
        Fixture.Med.Params[I].Size = Width;
        Fixture.Med.TypedParams[I].Type = Fixture.High.Params[I].Type =
            NdType::makeInt(Width);
        Fixture.Med.Blocks[0].Ops[0].Inputs[I] = Fixture.Med.Params[I];
      }
      Fixture.Med.Blocks[0].Ops[0].Output.Size = Width;
      returnDiamond(Fixture);
      Fixture.Med.Blocks[2].Ops.clear();
      const auto Blocks = Fixture.Med.Blocks;
      std::array<unsigned, 4> Order{0, 1, 2, 3};
      do {
        for (unsigned I = 0; I < 4; ++I)
          Fixture.Med.Blocks[I] = Blocks[Order[I]];
        std::string Error;
        const auto Hint = Fixture.infer(Error);
        const auto &TRI = getTargetRegInfo(Architecture);
        EXPECT_EQ(bool(Hint), TRI.IntParamRegs[0] == TRI.IntReturnReg) << Error;
        if (Hint) {
          EXPECT_EQ(Hint->ReturnLocation.ValueBytes, Width);
          EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset,
                    TRI.IntReturnReg);
          EXPECT_EQ(Hint->Parameters[0].Location.ValueBytes, Width);
        }
      } while (std::next_permutation(Order.begin(), Order.end()));
    }
}

TEST(NativeSourceHints, IncomingResultsRejectUnobservedNarrowAndLaterVersions) {
  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    NativeFixture Fixture;
    returnDiamond(Fixture);
    Fixture.Med.Blocks[2].Ops.clear();
    auto &Use = Fixture.Med.Blocks[1].Ops[0].Inputs[0];
    if (Mutation == 0)
      Use = MedVar::makeConst(3, 4);
    if (Mutation == 1)
      Use.Size = 2;
    if (Mutation == 2) {
      Use.Kind = MedVar::Reg;
      Use.SSAVer = 1;
    }
    if (Mutation == 3) {
      Fixture.Med.Params[0].Size = 2;
      Fixture.Med.TypedParams[0].Type = Fixture.High.Params[0].Type =
          NdType::makeInt(2);
      Use = Fixture.Med.Params[0];
    }
    if (Mutation == 4) {
      // A matching parameter listed only in an unreachable component does
      // not make the actual entry register an observed input.
      Fixture.Med.Blocks[0].Succs = {2};
      Fixture.Med.Blocks[1].Preds.clear();
    }
    if (Mutation == 5 || Mutation == 6) {
      Use.Kind = MedVar::Reg;
      Use.Id = 60;
      Use.SSAVer = 0;
      // An ordinary definition or a PHI may own the first SSA version.
      // Neither is evidence that the parameter's incoming bytes were used.
      if (Mutation == 5) {
        MedOp Internal;
        Internal.Opcode = NdOp::COPY;
        Internal.Output = Use;
        Internal.addInput(MedVar::makeConst(42, 4));
        auto &Ops = Fixture.Med.Blocks[1].Ops;
        Ops.insert(Ops.begin(), Internal);
      } else {
        PhiNode Phi;
        Phi.Output = Use;
        Phi.Args = {{0, MedVar::makeConst(42, 4)}};
        Fixture.Med.Blocks[1].Phis.push_back(std::move(Phi));
      }
    }
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
  }
}

TEST(NativeSourceHints, IncomingResultFactsInvalidateNarrowedSelfCopies) {
  for (uint16_t WriteWidth : {2, 4}) {
    NativeFixture Fixture;
    returnDiamond(Fixture);
    auto &Ops = Fixture.Med.Blocks[2].Ops;
    MedOp Copy = Ops.front();
    Copy.Opcode = NdOp::COPY;
    Copy.NumInputs = 1;
    Copy.Inputs[0] = Copy.Output;
    Copy.Output.Size = WriteWidth;
    Ops = {Copy};
    std::string Error;
    EXPECT_EQ(bool(Fixture.infer(Error)), WriteWidth == 4) << Error;
  }
}

TEST(NativeSourceHints, IncomingResultBackedgesMeetInitialAndClobberedStates) {
  for (bool Clobber : {false, true}) {
    NativeFixture Fixture;
    const auto Return = Fixture.Med.Blocks[0].Ops.back();
    MedOp Observe;
    Observe.Opcode = NdOp::COPY;
    Observe.Output.Kind = MedVar::Temp;
    Observe.Output.Id = 50;
    Observe.Output.Size = 4;
    Observe.addInput(Fixture.Med.Params[0]);
    Fixture.Med.Blocks.resize(3);
    for (unsigned I = 0; I < 3; ++I) {
      Fixture.Med.Blocks[I].Id = I;
      Fixture.Med.Blocks[I].StartAddr = Fixture.Med.Entry + I * 16;
    }
    auto &Blocks = Fixture.Med.Blocks;
    Blocks[0].Ops = {Observe};
    Blocks[0].Preds = {1};
    Blocks[0].Succs = {1, 2};
    Blocks[1].Preds = {0};
    Blocks[1].Succs = {0};
    Blocks[2].Preds = {0};
    Blocks[2].Ops = {Return};
    if (Clobber) {
      MedOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(MedVar::makeConst(0x1020, 8));
      auto Hint = std::make_shared<SourceCallTypeHint>();
      Hint->Signature.ReturnType = NdType::makeVoid();
      std::string Error;
      ASSERT_TRUE(
          assignDarwinScalarSourceABI(Hint->Signature, Arch::AArch64, Error));
      Call.SourceCallHint = std::move(Hint);
      Blocks[1].Ops = {Call};
    }
    std::string Error;
    EXPECT_EQ(bool(Fixture.infer(Error)), !Clobber) << Error;
  }
}

TEST(NativeSourceHints, ReturnProofRejectsMalformedOrUnboundedControlFlow) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
      NativeFixture Fixture(Architecture);
      returnDiamond(Fixture);
      auto &Blocks = Fixture.Med.Blocks;
      if (Mutation == 0)
        Blocks[0].Succs.push_back(99);
      if (Mutation == 1)
        Blocks[0].Succs.push_back(1);
      if (Mutation == 2)
        Blocks[1].Preds.push_back(0);
      if (Mutation == 3)
        Blocks[1].Preds.clear();
      if (Mutation == 4)
        Blocks[1].Id = Blocks[0].Id;
      if (Mutation == 5)
        Blocks[0].Id = -1;
      if (Mutation == 6)
        Blocks[1].StartAddr = Fixture.Med.Entry;
      if (Mutation == 7)
        Blocks[0].StartAddr += 4;
      if (Mutation == 8)
        Blocks.resize(16385);
      if (Mutation == 9)
        Blocks[1].Ops.resize(262144, Blocks[1].Ops.front());
      std::string Error;
      EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
      EXPECT_FALSE(Error.empty()) << Mutation;
    }
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

void addPointerCall(NativeFixture &Fixture) {
  for (size_t Index = 0; Index < Fixture.Med.Params.size(); ++Index) {
    Fixture.Med.Params[Index].Size = 8;
    Fixture.Med.TypedParams[Index].Type = NdType::makeInt(8);
    Fixture.High.Params[Index].Type = NdType::makeInt(8);
    Fixture.Med.Blocks[0].Ops[0].Inputs[Index] = MedVar::makeConst(21, 4);
  }
  auto CallHint = std::make_shared<SourceCallTypeHint>();
  CallHint->Signature.Origin =
      SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  CallHint->Signature.ReturnType = NdType::makeVoid();
  CallHint->Signature.Parameters = {
      {"first", NdType::makePtr(NdType::makeVoid())},
      {"second", NdType::makePtr(NdType::makeVoid())}};
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(CallHint->Signature,
                                          Fixture.Image.Arch, Error))
      << Error;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.SourceCallHint = std::move(CallHint);
  Call.addInput(MedVar::makeConst(0x1020, 8));
  Call.addInput(Fixture.Med.Params[0]);
  Call.addInput(Fixture.Med.Params[1]);
  // Generic MedIR keeps the incoming SSA registers until source parameters
  // are bound in the second pipeline run. Their IDs need not match Param IDs.
  for (unsigned Index = 1; Index <= 2; ++Index) {
    Call.Inputs[Index].Kind = MedVar::Reg;
    Call.Inputs[Index].Id += 100;
  }
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Call);
}

TEST(NativeSourceHints,
     PointerUsesFollowWholeValuesWithoutChangingMachineTypes) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Shape = 0; Shape < 4; ++Shape) {
      NativeFixture Fixture(Architecture);
      addPointerCall(Fixture);
      if (Shape == 3) {
        auto &Ops = Fixture.Med.Blocks[0].Ops;
        MedOp SelfCopy;
        SelfCopy.Opcode = NdOp::COPY;
        SelfCopy.Output = Ops[0].Inputs[1];
        SelfCopy.addInput(SelfCopy.Output);
        Ops.insert(Ops.begin(), SelfCopy);
      } else if (Shape) {
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output.Kind = MedVar::Temp;
        Copy.Output.Id = 30;
        Copy.Output.SSAVer = 1;
        Copy.Output.Size = 8;
        Copy.addInput(Fixture.Med.Params[0]);
        auto &Entry = Fixture.Med.Blocks[0];
        Entry.Ops[0].Inputs[1] = Copy.Output;
        if (Shape == 1) {
          Entry.Ops.insert(Entry.Ops.begin(), Copy);
        } else {
          // A loop PHI retains the exact incoming value through its backedge.
          MedBlock Exit = std::move(Entry);
          Exit.Id = 1;
          Exit.Preds = {2};
          MedBlock Loop;
          Loop.Id = 2;
          Loop.Preds = {0, 2};
          Loop.Succs = {2, 1};
          PhiNode Phi;
          Phi.Output = Copy.Output;
          ++Phi.Output.Id;
          Phi.Args = {{0, Copy.Output}, {2, Phi.Output}};
          Loop.Phis.push_back(Phi);
          Exit.Ops[0].Inputs[1] = Phi.Output;
          Entry = {};
          Entry.Id = 0;
          Entry.Succs = {2};
          Entry.Ops.push_back(Copy);
          // Deliberately keep the consumer before the producer in block order.
          Fixture.Med.Blocks.push_back(std::move(Exit));
          Fixture.Med.Blocks.push_back(std::move(Loop));
        }
      }
      std::string Error;
      auto Hint = Fixture.infer(Error);
      ASSERT_TRUE(Hint) << Error;
      ASSERT_EQ(Hint->Parameters.size(), 2U);
      for (size_t Index = 0; Index < 2; ++Index) {
        EXPECT_EQ(Hint->Parameters[Index].Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Hint->Parameters[Index].Location.ValueBytes, 8U);
        EXPECT_EQ(Hint->Parameters[Index].Location.RegisterOffset,
                  getTargetRegInfo(Architecture).IntParamRegs[Index]);
        EXPECT_EQ(Fixture.Med.TypedParams[Index].Type->Kind, NdTypeKind::Int);
        EXPECT_EQ(Fixture.High.Params[Index].Type->Kind, NdTypeKind::Int);
      }
      EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                MedReturnValueEvidence::Unknown);
    }
  }
}

TEST(NativeSourceHints, ConflictingOrPartialUsesCannotInventPointerParameters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      NativeFixture Fixture(Architecture);
      addPointerCall(Fixture);
      auto &Ops = Fixture.Med.Blocks[0].Ops;
      if (Mutation == 0) {
        Ops[1].Inputs[0] = Fixture.Med.Params[0];
      } else if (Mutation == 1) {
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output.Kind = MedVar::Temp;
        Copy.Output.Id = 30;
        Copy.Output.Size = 4;
        Copy.addInput(Fixture.Med.Params[0]);
        Ops.insert(Ops.begin(), Copy);
      } else if (Mutation == 2) {
        auto Hint =
            std::make_shared<SourceCallTypeHint>(*Ops[0].SourceCallHint);
        Hint->Signature.Parameters[0].Type = NdType::makeInt(8);
        auto ScalarCall = Ops[0];
        ScalarCall.SourceCallHint = std::move(Hint);
        Ops.insert(Ops.begin(), ScalarCall);
      } else if (Mutation == 3) {
        // A later register version is not the incoming parameter.
        Ops[0].Inputs[1].Kind = MedVar::Reg;
        ++Ops[0].Inputs[1].SSAVer;
      } else {
        Ops[0].Inputs[1].Size = 4;
      }
      std::string Error;
      auto Hint = Fixture.infer(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Int) << Mutation;
      EXPECT_EQ(Hint->Parameters[1].Type->Kind, NdTypeKind::Ptr) << Mutation;
    }
  }
}

TEST(NativeSourceHints, AmbiguousValuesAndUnboundCallsDoNotSupplyPointerFacts) {
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    NativeFixture Fixture;
    addPointerCall(Fixture);
    auto &Ops = Fixture.Med.Blocks[0].Ops;
    if (Mutation == 0) {
      Ops[0].SourceCallHint.reset();
    } else if (Mutation == 1) {
      auto Hint = std::make_shared<SourceCallTypeHint>(*Ops[0].SourceCallHint);
      Hint->Signature.Parameters[0].Location.ValueBytes = 4;
      Ops[0].SourceCallHint = std::move(Hint);
    } else if (Mutation == 2) {
      const auto Duplicate = Ops[1];
      Ops.insert(Ops.begin(), Duplicate);
    } else if (Mutation == 3) {
      Ops[0].Inputs[1] = Fixture.Med.Params[0];
      Ops[0].Inputs[1].RegOff = Fixture.Med.Params[1].RegOff;
    } else {
      PhiNode Phi;
      Phi.Output = Ops[0].Inputs[1];
      Phi.Args = {{0, MedVar::makeConst(7, 8)}};
      Fixture.Med.Blocks[0].Phis.push_back(Phi);
    }
    std::string Error;
    auto Hint = Fixture.infer(Error);
    if (Mutation < 2) {
      EXPECT_FALSE(Hint);
    } else {
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Int);
      EXPECT_EQ(Hint->Parameters[1].Type->Kind, NdTypeKind::Int);
    }
  }
}
} // namespace
