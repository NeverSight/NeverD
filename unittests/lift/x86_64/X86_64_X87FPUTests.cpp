#include "NeverDLiftFixture.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/Support/raw_ostream.h"

using namespace neverd;

class X86_64_X87FPU : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_x87_fpu.o";
}

TEST_F(X86_64_X87FPU, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_x87_fpu.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_X87FPU, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_X87FPU, NoUnreachable) {
    verifyLLVMIRNoUnreachable(testObj());
}

TEST_F(X86_64_X87FPU, FaddPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fadd", "FLOAT_ADD");
}

TEST_F(X86_64_X87FPU, FsubPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fsub", "FLOAT_SUB");
}

TEST_F(X86_64_X87FPU, FmulPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fmul", "FLOAT_MULT");
}

TEST_F(X86_64_X87FPU, FdivPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fdiv", "FLOAT_DIV");
}

TEST_F(X86_64_X87FPU, FabsPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fabs", "FLOAT_ABS");
}

TEST_F(X86_64_X87FPU, FchsPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fchs", "FLOAT_NEG");
}

TEST_F(X86_64_X87FPU, FsqrtPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fsqrt", "FLOAT_SQRT");
}

TEST_F(X86_64_X87FPU, FildFistpPreservesSemantics) {
    auto r = liftToLowIR(testObj());
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.contains("FLOAT_INT2FLOAT") || r.contains("FLOAT_FLOAT2INT"))
        << "FILD/FISTP should produce float conversion ops";
}

TEST_F(X86_64_X87FPU, FxchPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fxch", "COPY");
}

TEST_F(X86_64_X87FPU, Fld1FldZPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fld1_fldz", "COPY");
}

TEST_F(X86_64_X87FPU, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(X86_64_X87FPU, NoConstantTrueBranch) {
    verifyNoConstantTrueBranch(testObj());
}

TEST_F(X86_64_X87FPU, DecompileSucceeds) {
    verifyDecompileProducesOutput(testObj());
}

TEST_F(X86_64_X87FPU, AllModesSucceed) { verifyAllModesSucceed(testObj()); }

TEST_F(X86_64_X87FPU, HighCFpremKeeps80BitOperandsAndStatus) {
  HighFunc Func;
  Func.Entry = 0x1000;
  Func.Name = "fprem_status";
  Func.ReturnType = NdType::makeInt(2, false);
  Func.Params = {{"dividend", NdType::makeInt(10, false)},
                 {"divisor", NdType::makeInt(10, false)}};

  auto Param = [](int Id) {
    MedVar V;
    V.Kind = MedVar::Param;
    V.TheArch = Arch::X64;
    V.Id = Id;
    V.Size = 10;
    V.RegOff = kNoParamReg;
    return V;
  };
  MedVar Result;
  Result.Kind = MedVar::Temp;
  Result.TheArch = Arch::X64;
  Result.Id = 2;
  Result.Size = 10;

  auto Fprem = HighExpr::makeCall(
      intrinsicName(Intrinsic::X87Fprem), 0,
      {HighExpr::makeVar(Param(0), NdType::makeInt(10, false)),
       HighExpr::makeVar(Param(1), NdType::makeInt(10, false))});
  Fprem->IntrinsicId = Intrinsic::X87Fprem;
  Fprem->Type = NdType::makeInt(10, false);
  HighStmt Compute;
  Compute.Kind = StmtKind::Assign;
  Compute.Dst = HighExpr::makeVar(Result, NdType::makeInt(10, false));
  Compute.Val = std::move(Fprem);
  Func.Body.push_back(std::move(Compute));

  auto Status =
      HighExpr::makeCall(intrinsicName(Intrinsic::X87ReadStatus), 0, {});
  Status->IntrinsicId = Intrinsic::X87ReadStatus;
  Status->Type = NdType::makeInt(2, false);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = std::move(Status);
  Func.Body.push_back(std::move(Return));

  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("fldt %[rhs]"), std::string::npos) << Source;
  EXPECT_NE(Source.find("fprem\\n\\tfnstsw"), std::string::npos) << Source;
  EXPECT_NE(Source.find("neverd_x87_fprem("), std::string::npos) << Source;
  EXPECT_NE(Source.find("neverd_x87_read_status()"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("<unknown_"), std::string::npos) << Source;
}
