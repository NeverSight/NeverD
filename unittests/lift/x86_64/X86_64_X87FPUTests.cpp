#include "NeverDLiftFixture.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86/X87Context.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/Support/raw_ostream.h"

using namespace neverd;

class X86_64_X87FPU : public NeverDLiftTest {
protected:
  void verifyContextStep(uint8_t Escape, uint8_t ModRM,
                         unsigned MemoryBytes = 0) {
    Decoder D;
    ASSERT_TRUE(D.init(Arch::X64));
    D.setX87ContextOwnership(true);
    const uint8_t Bytes[] = {Escape, ModRM};
    DecodedInsn I;
    ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), 0x1000, I), 2);
    std::vector<LowOp> Ops;
    ASSERT_NO_THROW(D.liftToLow(I, Ops));
    const auto Step = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
      return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
             Op.Inputs[0].isConst() &&
             Op.Inputs[0].Offset == unsigned(Intrinsic::X87ContextStep);
    });
    ASSERT_NE(Step, Ops.end());
    const uint32_t Selector =
        Escape | (uint32_t(MemoryBytes ? ModRM & 0x38 : ModRM) << 8) |
        (MemoryBytes ? X87ContextMemory |
                           (uint32_t(ModRM) << X87ContextOriginalModRMShift)
                     : 0);
    ASSERT_TRUE(x87ContextLowShapeIsValid(*Step, Arch::X64));
    EXPECT_EQ(Step->Inputs[2].Offset, Selector);
    ASSERT_TRUE(x87ContextInstruction(Selector));
    EXPECT_EQ(x87ContextInstruction(Selector)->MemoryBytes, MemoryBytes);
    EXPECT_EQ(Step->Inputs[1], NdVar::reg(x86reg::X87ContextHandle, 8));
    EXPECT_EQ(Step->Inputs[5], NdVar::cst(0x1000, 8));
  }
};

static fs::path testObj() { return fs::path(TEST_OBJ_DIR) / "test_x87_fpu.o"; }

TEST_F(X86_64_X87FPU, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_x87_fpu.o not built";
  verifyAllStages(testObj());
}

TEST_F(X86_64_X87FPU, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(X86_64_X87FPU, NoUnreachable) { verifyLLVMIRNoUnreachable(testObj()); }

TEST_F(X86_64_X87FPU, FaddPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_fadd", "FLOAT_ADD");
  verifyContextStep(0xdc, 0x07, 8);
}

TEST_F(X86_64_X87FPU, FsubPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_fsub", "FLOAT_SUB");
  verifyContextStep(0xdc, 0x27, 8);
}

TEST_F(X86_64_X87FPU, FmulPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_fmul", "FLOAT_MULT");
  verifyContextStep(0xdc, 0x0f, 8);
}

TEST_F(X86_64_X87FPU, FdivPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_fdiv", "FLOAT_DIV");
  verifyContextStep(0xdc, 0x37, 8);
}

TEST_F(X86_64_X87FPU, FabsPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_fabs", "INT_AND");
  verifyContextStep(0xd9, 0xe1, 0);
}

TEST_F(X86_64_X87FPU, FchsPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_fchs", "INT_XOR");
  verifyContextStep(0xd9, 0xe0, 0);
}

TEST_F(X86_64_X87FPU, FsqrtPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_fsqrt", "FLOAT_SQRT");
  verifyContextStep(0xd9, 0xfa, 0);
}

TEST_F(X86_64_X87FPU, FildFistpPreservesSemantics) {
  auto r = liftToLowIR(testObj());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.contains("FLOAT_INT2FLOAT") || r.contains("FLOAT_FLOAT2INT"))
      << "FILD/FISTP should produce float conversion ops";
  verifyContextStep(0xdb, 0x07, 4);
  verifyContextStep(0xdb, 0x1f, 4);
}

TEST_F(X86_64_X87FPU, FxchPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_fxch", "COPY");
  verifyContextStep(0xd9, 0xc9, 0);
}

TEST_F(X86_64_X87FPU, Fld1FldZPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_fld1_fldz", "COPY");
  verifyContextStep(0xd9, 0xe8, 0);
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

TEST_F(X86_64_X87FPU, EmittedCExecutesArithmeticAndDynamicRounding) {
#if !defined(__x86_64__) || !defined(__linux__)
  GTEST_SKIP() << "native x87 C execution requires Linux x86-64";
#else
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Clang is required for emitted C execution";
  auto Decompiled = decompileToC(testObj());
  ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
  const auto Source = tmpFile("decompiled.c");
  std::ofstream Output(Source, std::ios::app);
  Output << R"(
typedef uint64_t lanes __attribute__((vector_size(16)));
static uint64_t bits(double value) {
  uint64_t result;
  __builtin_memcpy(&result, &value, sizeof(result));
  return result;
}
int main(void) {
  const double values[][2] = {{2.5, 1.25}, {-6, 2}, {-0.0, 2}, {4, 4}};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    const double a = values[i][0], b = values[i][1];
    const lanes left = {bits(a), UINT64_MAX}, right = {bits(b), 123};
    if (test_fadd(left, right)[0] != bits((double)((long double)a + b)) ||
        test_fsub(left, right)[0] != bits((double)((long double)a - b)) ||
        test_fmul(left, right)[0] != bits((double)((long double)a * b)) ||
        test_fdiv(left, right)[0] != bits((double)((long double)a / b)) ||
        test_fabs(left)[0] != bits(__builtin_fabs(a)) ||
        test_fchs(left)[0] != bits(-a)) return 1;
  }
  if (test_fsqrt((lanes){bits(4.0), 0})[0] != bits(2.0)) return 2;
  const int32_t integers[] = {0, -1, 17, INT32_MIN, INT32_MAX};
  for (unsigned i = 0; i < sizeof(integers) / sizeof(integers[0]); ++i)
    if (test_fild_fistp((uint32_t)integers[i]) != (uint32_t)integers[i]) return 3;
  uint16_t saved;
  __asm__ volatile("fnstcw %0" : "=m"(saved));
  int failed = 0;
  for (unsigned mode = 0; mode < 4; ++mode) {
    uint16_t control = (saved & ~0x0c00u) | (mode << 10);
    __asm__ volatile("fldcw %0" : : "m"(control));
    for (unsigned i = 0; i < 4; ++i) {
      const double input = i & 1 ? -2.5 : 1.75;
      long double expected;
      __asm__ volatile("frndint" : "=t"(expected) : "0"((long double)input));
      if (test_frndint((lanes){bits(input), 0})[0] != bits((double)expected))
        failed = 4;
    }
  }
  __asm__ volatile("fldcw %0" : : "m"(saved));
  return failed;
}
)";
  Output.close();
  const auto Binary = tmpFile(std::string("x87-c-execution") +
                              neverd::test::executableSuffix());
  for (const std::string Optimization : {"-O0", "-O2"}) {
    auto Compiled = exec(NEVERD_TEST_CLANG,
                         {"-std=c11", Optimization, "-Werror=uninitialized",
                          "-Werror=return-type", Source.string(), "-lm", "-o",
                          Binary.string()});
    ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;
    auto Executed = exec(Binary.string(), {});
    EXPECT_EQ(Executed.exitCode, 0) << Executed.err << Optimization;
  }
#endif
}

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
