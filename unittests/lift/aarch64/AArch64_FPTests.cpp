#include "AArch64HighCBehavior.h"

class AArch64_FP : public AArch64HighCBehaviorTest {
protected:
  void expectPairedClangSyntax(const fs::path &CFile,
                               const std::string &Source) {
    auto syntax = checkHighCClangSyntax(
        CFile, {"-target", "aarch64-none-elf", "-ffreestanding",
                "-march=armv8.6-a+bf16", "-std=gnu11"});
    EXPECT_EQ(syntax.exitCode, 0) << syntax.err << "\n" << Source;
  }
};

static fs::path testObj() { return fs::path(TEST_OBJ_DIR) / "test_fp_a64.o"; }

static std::string functionIR(const std::string &IR, const std::string &Name) {
  auto NamePos = IR.find("@" + Name + "(");
  if (NamePos == std::string::npos)
    return {};
  auto Begin = IR.rfind("define ", NamePos);
  auto End = IR.find("\n}", NamePos);
  if (Begin == std::string::npos || End == std::string::npos)
    return {};
  return IR.substr(Begin, End + 2 - Begin);
}

static std::string functionSource(const std::string &Source,
                                  const std::string &Name) {
  auto NamePos = Source.find(Name + "(");
  if (NamePos == std::string::npos)
    return {};
  auto Begin = Source.rfind('\n', NamePos);
  Begin = Begin == std::string::npos ? 0 : Begin + 1;
  auto End = Source.find("\n}", NamePos);
  if (End == std::string::npos)
    return {};
  return Source.substr(Begin, End + 2 - Begin);
}

TEST_F(AArch64_FP, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_fp_a64.o not built";
  verifyAllStages(testObj());
}

TEST_F(AArch64_FP, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(AArch64_FP, ZeroCompareFlagsExecuteThroughBothCRoutes) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "floating-point source execution requires Clang";
  const auto Assembly = tmpFile("fp-zero.s");
  const auto Object = tmpFile("fp-zero.o");
  {
    std::ofstream Out(Assembly);
    Out << ".text\n";
    for (const char *Register : {"s", "d"}) {
      for (bool Signaling : {false, true}) {
        const auto Name =
            std::string("fp_zero_") + Register + (Signaling ? "e" : "");
        Out << ".global " << Name << "\n.type " << Name << ",%function\n"
            << Name << ":\n  fmov " << Register << "0, "
            << (Register[0] == 'd' ? "x0" : "w0") << "\n  "
            << (Signaling ? "fcmpe" : "fcmp") << " " << Register << "0, #0.0\n"
            << "  cset w1, mi\n  cset w2, cs\n  cset w3, vs\n"
            << "  b.ne 1f\n  mov w0, #2\n  b 2f\n1:\n  mov w0, #0\n2:\n"
            << "  orr w0, w0, w1\n  orr w0, w0, w2, lsl #2\n"
            << "  orr w0, w0, w3, lsl #3\n  ret\n.size " << Name << ",.-"
            << Name << "\n";
      }
    }
  }
  const auto Assembled =
      exec(NEVERD_TEST_CLANG,
           {"-target", "aarch64-none-elf", "-march=armv8.2-a+fp16", "-c",
            Assembly.string(), "-o", Object.string()});
  ASSERT_EQ(Assembled.exitCode, 0) << Assembled.err;
  const std::string Harness = R"C(
#include <stdint.h>
int main(void) {
  const uint64_t signs[] = {UINT64_C(0x80000000),
                            UINT64_C(0x8000000000000000)};
  const uint64_t exponents[] = {UINT64_C(0x7f800000),
                                UINT64_C(0x7ff0000000000000)};
  const uint64_t mantissas[] = {UINT64_C(0x7fffff),
                                UINT64_C(0xfffffffffffff)};
  uint64_t random = UINT64_C(0xb71943d2836aa815);
  for (unsigned width = 0; width != 2; ++width) {
    const uint64_t sign = signs[width], exponent = exponents[width];
    const uint64_t mantissa = mantissas[width];
    const uint64_t edges[] = {0, sign, 1, sign | 1, exponent,
      sign | exponent, exponent | 1, sign | exponent | 1,
      exponent | (mantissa >> 1), exponent - 1, sign | (exponent - 1)};
    for (unsigned i = 0; i != 523; ++i) {
      random = random * UINT64_C(6364136223846793005) + 1;
      uint64_t bits = (i < 11 ? edges[i] : random) &
                      (sign | exponent | mantissa);
      int nan = (bits & exponent) == exponent && (bits & mantissa) != 0;
      int zero = (bits & ~sign) == 0;
      int negative = (bits & sign) != 0 && !zero && !nan;
      // Return N | (Z << 1) | (C << 2) | (V << 3). Unordered is 0b1100.
      unsigned expected = nan ? 12 : zero ? 6 : negative ? 1 : 4;
      if (width == 0 && (fp_zero_s(bits) != expected ||
                        fp_zero_se(bits) != expected)) return 1;
      if (width == 1 && (fp_zero_d(bits) != expected ||
                        fp_zero_de(bits) != expected)) return 2;
    }
  }
  return 0;
}
)C";
  for (bool LLVMRoute : {false, true}) {
    SCOPED_TRACE(LLVMRoute ? "LLVMC" : "HighC");
    const auto Decompiled =
        LLVMRoute ? decompileToC(Object) : decompileToHighC(Object);
    ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
    std::ifstream Input(
        tmpFile(LLVMRoute ? "decompiled.c" : "decompiled_high.c"));
    ASSERT_TRUE(Input.good());
    const std::string Source((std::istreambuf_iterator<char>(Input)),
                             std::istreambuf_iterator<char>());
    ASSERT_EQ(Source.find("unknown value"), std::string::npos) << Source;
    const auto CFile = tmpFile("fp-zero-generated.c");
    std::ofstream(CFile) << Source << "\n" << Harness;
    const auto Program = tmpFile("fp-zero-generated.exe");
    for (const char *Level : {"-O0", "-O2"}) {
      SCOPED_TRACE(Level);
      const auto Compiled =
          exec(NEVERD_TEST_CLANG, {"-std=gnu11", Level, CFile.string(), "-lm",
                                   "-o", Program.string()});
      ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err << "\n" << Source;
      const auto Executed = exec(Program.string(), {});
      EXPECT_EQ(Executed.exitCode, 0) << Executed.err << "\n" << Source;
    }
  }
}

// Half-precision arithmetic computes in _Float16, each result rounded to
// half precision as the instruction rounds it, where it used to print every
// value as unknown.  Compile the C on the host, which computes _Float16 in
// float and rounds once per operation: exact for these operations, since
// float has more than twice the bits of half precision.
TEST_F(AArch64_FP, HalfPrecisionArithmeticExecutesThroughHighC) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "floating-point source execution requires Clang";
  const auto Assembly = tmpFile("fp16-ops.s");
  const auto Object = tmpFile("fp16-ops.o");
  std::ofstream(Assembly) << R"(.text
.global half_ops
.type half_ops,%function
half_ops:
  fmov h0, w0
  fmov h1, w1
  fadd h2, h0, h1
  fmul h2, h2, h0
  fsub h2, h2, h1
  fdiv h3, h2, h1
  fmadd h0, h0, h1, h3
  fmov w0, h0
  ret
.size half_ops,.-half_ops
)";
  const auto Assembled =
      exec(NEVERD_TEST_CLANG,
           {"-target", "aarch64-none-elf", "-march=armv8.2-a+fp16", "-c",
            Assembly.string(), "-o", Object.string()});
  ASSERT_EQ(Assembled.exitCode, 0) << Assembled.err;
  const auto Decompiled = decompileToHighC(Object);
  ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  ASSERT_TRUE(Input.good());
  const std::string Source((std::istreambuf_iterator<char>(Input)),
                           std::istreambuf_iterator<char>());
  ASSERT_EQ(Source.find("unknown value"), std::string::npos) << Source;
  EXPECT_NE(Source.find("_Float16"), std::string::npos) << Source;
  const std::string Harness = R"C(
static uint16_t half_reference(uint16_t A, uint16_t B) {
  const _Float16 a = __builtin_bit_cast(_Float16, A);
  const _Float16 b = __builtin_bit_cast(_Float16, B);
  _Float16 t = a + b;
  t = t * a;
  t = t - b;
  t = t / b;
  return __builtin_bit_cast(uint16_t, __builtin_fmaf16(a, b, t));
}
int main(void) {
  static const uint16_t Values[] = {0x3C00, 0x4000, 0x3555, 0xC500, 0x7BFF,
                                    0x0001, 0x3800, 0x8400, 0x5BFF};
  for (unsigned I = 0; I != sizeof(Values) / sizeof(Values[0]); ++I)
    for (unsigned J = 0; J != sizeof(Values) / sizeof(Values[0]); ++J)
      if ((uint16_t)half_ops(Values[I], Values[J]) !=
          half_reference(Values[I], Values[J]))
        return 1;
  return 0;
}
)C";
  const auto CFile = tmpFile("fp16-ops-host.c");
  std::ofstream(CFile) << Source << "\n" << Harness;
  // The host may emulate half operations through float, masking contraction
  // across the recovered multiply/subtract. Check a target with native half
  // arithmetic too: only the original explicit FMADD may be fused.
  const auto IRFile = tmpFile("fp16-ops-recompiled.ll");
  const auto CrossCompiled =
      exec(NEVERD_TEST_CLANG,
           {"-target", "aarch64-none-elf", "-march=armv8.2-a+fp16",
            "-ffreestanding", "-std=gnu11", "-O2", "-ffp-contract=on", "-S",
            "-emit-llvm", CFile.string(), "-o", IRFile.string()});
  ASSERT_EQ(CrossCompiled.exitCode, 0) << CrossCompiled.err << "\n" << Source;
  std::ifstream IRInput(IRFile);
  ASSERT_TRUE(IRInput.good());
  const std::string IR((std::istreambuf_iterator<char>(IRInput)),
                       std::istreambuf_iterator<char>());
  const auto Body = functionIR(IR, "half_ops");
  ASSERT_FALSE(Body.empty()) << IR;
  EXPECT_EQ(Body.find("@llvm.fmuladd"), std::string::npos) << Body;
  EXPECT_NE(Body.find("fmul half"), std::string::npos) << Body;
  EXPECT_NE(Body.find("fsub half"), std::string::npos) << Body;
  const auto Program = tmpFile("fp16-ops-host.exe");
  const auto Compiled =
      exec(NEVERD_TEST_CLANG, {"-std=gnu11", "-O2", CFile.string(), "-lm", "-o",
                               Program.string()});
  ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err << "\n" << Source;
  const auto Executed = exec(Program.string(), {});
  EXPECT_EQ(Executed.exitCode, 0) << Source;
}

TEST_F(AArch64_FP, FaddLifts) {
  verifyLowIRContains(testObj(), "test_fadd_a64", "FLOAT_ADD");
}

TEST_F(AArch64_FP, FsubLifts) {
  verifyLowIRContains(testObj(), "test_fsub_a64", "FLOAT_SUB");
}

TEST_F(AArch64_FP, FmulLifts) {
  verifyLowIRContains(testObj(), "test_fmul_a64", "FLOAT_MULT");
}

TEST_F(AArch64_FP, FdivLifts) {
  verifyLowIRContains(testObj(), "test_fdiv_a64", "FLOAT_DIV");
}

TEST_F(AArch64_FP, FsqrtLifts) {
  verifyLowIRContains(testObj(), "test_fsqrt_a64", "FLOAT_SQRT");
}

TEST_F(AArch64_FP, FnegLifts) {
  verifyLowIRContains(testObj(), "test_fneg_a64", "FLOAT_NEG");
}

TEST_F(AArch64_FP, FabsLifts) {
  verifyLowIRContains(testObj(), "test_fabs_a64", "FLOAT_ABS");
}

TEST_F(AArch64_FP, ScvtfLifts) {
  verifyLowIRContains(testObj(), "test_scvtf_a64", "FLOAT_INT2FLOAT");
}

TEST_F(AArch64_FP, FcvtzsLifts) {
  verifyLowIRContains(testObj(), "test_fcvtzs_a64", "FLOAT_FLOAT2INT");
}

TEST_F(AArch64_FP, FjcvtzsUpdatesExactnessFlag) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto F = functionIR(r.out, "test_fjcvtzs_z_a64");
  ASSERT_FALSE(F.empty()) << r.out;

  EXPECT_NE(F.find("@llvm.aarch64.fjcvtzs"), std::string::npos) << F;
  EXPECT_NE(F.find("sitofp i32"), std::string::npos) << F;
  EXPECT_NE(F.find("fcmp oeq double"), std::string::npos) << F;
  EXPECT_EQ(F.find("ret i64 1"), std::string::npos) << F;
}

TEST_F(AArch64_FP, FjcvtzsHighCUsesBuiltinAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("__jcvt(__builtin_bit_cast(double"), std::string::npos)
      << source;
  EXPECT_NE(source.find("0x8000000000000000"), std::string::npos) << source;
  EXPECT_EQ(source.find("__neverd"), std::string::npos) << source;
  auto F = functionSource(source, "test_fjcvtzs_z_a64");
  ASSERT_FALSE(F.empty()) << source;
  EXPECT_NE(F.find("int32_t test_fjcvtzs_z_a64"), std::string::npos) << F;
  expectPairedClangSyntax(cFile, source);

  llvm::LLVMContext Context;
  auto Module = compileHighCFlow(cFile, "armv8.6-a+bf16", Context);
  ASSERT_TRUE(Module);
  const auto *Function = Module->getFunction("test_fjcvtzs_z_a64");
  ASSERT_TRUE(Function);
  ASSERT_EQ(Function->arg_size(), 1u);
  a64_highc_test::ValueFlow Flow(*Function);
  ASSERT_TRUE(Flow.valid());
  auto Converted = a64_highc_test::calls(*Function, "llvm.aarch64.fjcvtzs");
  ASSERT_EQ(Converted.size(), 1u);
  EXPECT_EQ(Flow.origin(Converted[0]->getArgOperand(0)), Function->getArg(0));
  const llvm::Value *SignedConversion = nullptr;
  const llvm::Value *Comparison = nullptr;
  for (const auto &Instruction : Function->front()) {
    if (const auto *Convert = llvm::dyn_cast<llvm::SIToFPInst>(&Instruction)) {
      ASSERT_FALSE(SignedConversion);
      SignedConversion = Convert;
      EXPECT_EQ(Flow.origin(Convert->getOperand(0)), Converted[0]);
    } else if (const auto *Call =
                   llvm::dyn_cast<llvm::CallBase>(&Instruction)) {
      if (Call->getCalledFunction()->getName().starts_with(
              "llvm.experimental.constrained.sitofp")) {
        ASSERT_FALSE(SignedConversion);
        SignedConversion = Call;
        EXPECT_EQ(Flow.origin(Call->getArgOperand(0)), Converted[0]);
      }
    }
  }
  ASSERT_TRUE(SignedConversion);
  for (const auto &Instruction : Function->front()) {
    const llvm::Value *Left = nullptr, *Right = nullptr;
    if (const auto *Compare = llvm::dyn_cast<llvm::FCmpInst>(&Instruction)) {
      EXPECT_EQ(Compare->getPredicate(), llvm::CmpInst::FCMP_OEQ);
      Left = Compare->getOperand(0);
      Right = Compare->getOperand(1);
    } else if (const auto *Call =
                   llvm::dyn_cast<llvm::CallBase>(&Instruction)) {
      if (Call->getCalledFunction()->getName().starts_with(
              "llvm.experimental.constrained.fcmp")) {
        EXPECT_TRUE(
            a64_highc_test::metadataText(Call->getArgOperand(2), "oeq"));
        Left = Call->getArgOperand(0);
        Right = Call->getArgOperand(1);
      }
    }
    if (Left && Right) {
      ASSERT_FALSE(Comparison);
      Comparison = &Instruction;
      EXPECT_TRUE((Flow.origin(Left) == Function->getArg(0) &&
                   Flow.origin(Right) == SignedConversion) ||
                  (Flow.origin(Right) == Function->getArg(0) &&
                   Flow.origin(Left) == SignedConversion));
    }
  }
  ASSERT_TRUE(Comparison);
  const auto *Return =
      llvm::dyn_cast<llvm::ReturnInst>(Function->front().getTerminator());
  ASSERT_TRUE(Return);
  // Ordered equality must control the result, except that negative zero is
  // never exact. This exercises both truth values through the actual return
  // expression and catches a constant return or a disconnected flag value.
  for (uint64_t Bits :
       {UINT64_C(0), UINT64_C(0x3ff0000000000000), UINT64_C(0xbff0000000000000),
        UINT64_C(0x8000000000000000)}) {
    for (bool Equal : {false, true}) {
      auto Result = Flow.integer(Return->getReturnValue(),
                                 {{Function->getArg(0), llvm::APInt(64, Bits)},
                                  {Comparison, llvm::APInt(1, Equal)}});
      ASSERT_TRUE(Result);
      EXPECT_EQ(*Result,
                llvm::APInt(32, Equal && Bits != UINT64_C(0x8000000000000000)));
    }
  }
}

TEST_F(AArch64_FP, FrecpxUsesDedicatedLLVMIntrinsic) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto F = functionIR(r.out, "test_frecpx_a64");
  ASSERT_FALSE(F.empty()) << r.out;

  EXPECT_NE(F.find("@llvm.aarch64.neon.frecpx.f32"), std::string::npos) << F;
  EXPECT_EQ(F.find("@llvm.aarch64.neon.frecpe"), std::string::npos) << F;
}

TEST_F(AArch64_FP, FrecpxHighCUsesACLEAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  auto F = functionSource(source, "test_frecpx_a64");
  ASSERT_FALSE(F.empty()) << source;
  EXPECT_NE(F.find("float test_frecpx_a64"), std::string::npos) << F;
  EXPECT_NE(F.find("vrecpxs_f32"), std::string::npos) << F;
  EXPECT_NE(F.find("__builtin_bit_cast(float"), std::string::npos) << F;
  EXPECT_EQ(F.find("vrecpe"), std::string::npos) << F;

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_FP, BfmmlaUsesDedicatedLLVMIntrinsic) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto F = functionIR(r.out, "test_bfmmla_a64");
  ASSERT_FALSE(F.empty()) << r.out;

  EXPECT_NE(F.find("@llvm.aarch64.neon.bfmmla"), std::string::npos) << F;
  EXPECT_EQ(F.find("fmul double"), std::string::npos) << F;
}

TEST_F(AArch64_FP, BfmmlaHighCUsesACLEAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  auto F = functionSource(source, "test_bfmmla_a64");
  ASSERT_FALSE(F.empty()) << source;
  EXPECT_NE(F.find("vbfmmlaq_f32"), std::string::npos) << F;
  EXPECT_NE(F.find("bfloat16x8_t"), std::string::npos) << F;
  EXPECT_EQ(F.find("__neverd"), std::string::npos) << F;

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_FP, BfmmlaLLVMCUsesACLEAndCompiles) {
  auto Result = decompileToC(testObj());
  ASSERT_EQ(Result.exitCode, 0) << Result.err;
  const auto CFile = tmpFile("decompiled.c");
  std::ifstream Input(CFile);
  ASSERT_TRUE(Input.good());
  const std::string Source((std::istreambuf_iterator<char>(Input)),
                           std::istreambuf_iterator<char>());
  const auto Function = functionSource(Source, "test_bfmmla_a64");
  ASSERT_FALSE(Function.empty()) << Source;
  EXPECT_NE(Function.find("vbfmmlaq_f32"), std::string::npos) << Function;
  EXPECT_NE(Source.find("arm_neon.h"), std::string::npos) << Source;
  // Compile the exact projection of this function. Other FPCR-sensitive
  // functions in the fixture have independent C projection contracts.
  const auto Fragment = tmpFile("bfmmla_llvmc.c");
  std::ofstream Output(Fragment);
  Output << "#include <stdint.h>\n#include <arm_neon.h>\n" << Function << '\n';
  Output.close();
  expectPairedClangSyntax(Fragment, Function);
  const auto Assembly = tmpFile("bfmmla_llvmc.s");
  auto Compile =
      exec("clang", {"-target", "aarch64-none-elf", "-ffreestanding",
                     "-march=armv8.6-a+bf16", "-std=gnu11", "-O2", "-S",
                     Fragment.string(), "-o", Assembly.string()});
  ASSERT_EQ(Compile.exitCode, 0) << Compile.err << Function;
  std::ifstream AsmInput(Assembly);
  const std::string Text((std::istreambuf_iterator<char>(AsmInput)),
                         std::istreambuf_iterator<char>());
  EXPECT_TRUE(std::regex_search(Text, std::regex(R"(\bbfmmla\s+v[0-9]+\.4s)")))
      << Text;
}

TEST_F(AArch64_FP, FrintiPreservesDynamicFPCRInLLVMIR) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto F = functionIR(r.out, "test_frinti_fpcr_a64");
  ASSERT_FALSE(F.empty()) << r.out;

  EXPECT_NE(F.find("@llvm.aarch64.get.fpcr"), std::string::npos) << F;
  EXPECT_NE(F.find("@llvm.aarch64.set.fpcr"), std::string::npos) << F;
  EXPECT_NE(F.find("@llvm.experimental.constrained.nearbyint.f64"),
            std::string::npos)
      << F;
  EXPECT_NE(F.find("metadata !\"round.dynamic\""), std::string::npos) << F;
  EXPECT_EQ(F.find("@llvm.roundeven.f64"), std::string::npos) << F;
}

TEST_F(AArch64_FP, FrintiHighCUsesClangBuiltinsAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("#pragma STDC FENV_ACCESS ON"), std::string::npos)
      << source;
  auto F = functionSource(source, "test_frinti_fpcr_a64");
  ASSERT_FALSE(F.empty()) << source;
  EXPECT_NE(F.find("__builtin_arm_rsr64(\"FPCR\")"), std::string::npos) << F;
  EXPECT_NE(F.find("__builtin_arm_wsr64(\"FPCR\""), std::string::npos) << F;
  EXPECT_NE(F.find("__builtin_elementwise_nearbyint"), std::string::npos) << F;
  EXPECT_EQ(F.find("__neverd"), std::string::npos) << F;
  expectPairedClangSyntax(cFile, source);

  llvm::LLVMContext Context;
  auto Module = compileHighCFlow(cFile, "armv8.6-a+bf16", Context);
  ASSERT_TRUE(Module);
  const auto *Function = Module->getFunction("test_frinti_fpcr_a64");
  ASSERT_TRUE(Function);
  ASSERT_EQ(Function->arg_size(), 1u);
  a64_highc_test::ValueFlow Flow(*Function);
  ASSERT_TRUE(Flow.valid());
  auto Reads = a64_highc_test::calls(*Function, "llvm.read_volatile_register");
  if (Reads.empty())
    Reads = a64_highc_test::calls(*Function, "llvm.read_register");
  auto Writes = a64_highc_test::calls(*Function, "llvm.write_register");
  auto Rounds = a64_highc_test::calls(*Function, "llvm.nearbyint");
  if (Rounds.empty())
    Rounds = a64_highc_test::calls(*Function,
                                   "llvm.experimental.constrained.nearbyint");
  ASSERT_EQ(Reads.size(), 1u);
  ASSERT_EQ(Writes.size(), 2u);
  ASSERT_EQ(Rounds.size(), 1u);
  EXPECT_TRUE(a64_highc_test::metadataText(Reads[0]->getArgOperand(0), "fpcr"));
  for (const auto *Write : Writes)
    EXPECT_TRUE(a64_highc_test::metadataText(Write->getArgOperand(0), "fpcr"));
  EXPECT_TRUE(Reads[0]->comesBefore(Writes[0]));
  EXPECT_TRUE(Writes[0]->comesBefore(Rounds[0]));
  EXPECT_TRUE(Rounds[0]->comesBefore(Writes[1]));
  EXPECT_EQ(Flow.origin(Rounds[0]->getArgOperand(0)), Function->getArg(0));
  EXPECT_EQ(Flow.origin(Writes[1]->getArgOperand(1)), Reads[0]);
  if (Rounds[0]->getCalledFunction()->getName().starts_with(
          "llvm.experimental.constrained."))
    EXPECT_TRUE(a64_highc_test::metadataText(Rounds[0]->getArgOperand(1),
                                             "round.dynamic"));
  // Setting toward-zero rounding changes only FPCR[23:22]; every other bit
  // must survive, and the subsequent write must restore the original value.
  for (uint64_t Saved : {UINT64_C(0), UINT64_MAX, UINT64_C(0x123456789abcdef0),
                         UINT64_C(1) << 22, UINT64_C(2) << 22}) {
    auto Mode = Flow.integer(Writes[0]->getArgOperand(1),
                             {{Reads[0], llvm::APInt(64, Saved)}});
    ASSERT_TRUE(Mode);
    EXPECT_EQ(*Mode, llvm::APInt(64, Saved | (UINT64_C(3) << 22)));
  }
  const auto *Return =
      llvm::dyn_cast<llvm::ReturnInst>(Function->front().getTerminator());
  ASSERT_TRUE(Return);
  EXPECT_TRUE(Writes[1]->comesBefore(Return));
  EXPECT_EQ(Flow.origin(Return->getReturnValue()), Rounds[0])
      << "return must use the rounding snapshot taken before FPCR restoration";
}

TEST_F(AArch64_FP, NoUnreachableInFunctions) {
  verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
