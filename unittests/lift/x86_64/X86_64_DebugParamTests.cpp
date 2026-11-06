//===- X86_64_DebugParamTests.cpp - Debug parameters by location ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// DWARF lists a function's parameters in source order, but System V passes
/// them by class: `f(double x, int n)` has n in EDI and x in XMM0, a 16-byte
/// record arrives in two registers, a complex number in two vector registers
/// and integers past the sixth on the stack.  Each recovered parameter is
/// named after the source parameter the convention put there, in HighC and
/// in LLVM-C.  The decompiled functions take their arguments in the same
/// registers, so callers built from the source declarations get the
/// source's results.
///
//===----------------------------------------------------------------------===//

#include "../../HostCompiler.h"
#include "NeverDLiftFixture.h"

#include <fstream>
#include <iterator>

class X86_64_DebugParams : public NeverDLiftTest {};

static fs::path debugParamsObj() {
  return fs::path(TEST_OBJ_DIR) / "test_debug_params.o";
}

/// Calls the decompiled functions through their source declarations.
static const char *const SourceCallers = R"c(
struct Pair {
  long a, b;
};
double dp_scale(double x, int n);
long dp_pair_sum(struct Pair p, long k);
double dp_complex_mix(_Complex double z, int n);
long dp_stack_tail(long a, long b, long c, long d, long e, long f, long g,
                   long h);

int main(void) {
  struct Pair p = {1, 2};
  _Complex double z = 1.5;
  __imag__ z = 2.0;
  if (dp_scale(2.5, 3) != 10.5)
    return 1;
  if (dp_pair_sum(p, 3) != 16)
    return 2;
  if (dp_complex_mix(z, 4) != 8.0)
    return 3;
  if (dp_stack_tail(1, 2, 3, 4, 5, 6, 7, 8) != 204)
    return 4;
  return 0;
}
)c";

TEST_F(X86_64_DebugParams, ParametersAreNamedWhereTheConventionPassesThem) {
  ASSERT_TRUE(fs::exists(debugParamsObj())) << debugParamsObj();
  const auto CFile = tmpFile("debug_params.c");
  const auto R = exec(
      ndBin(), {"decompile", "-o", CFile.string(), debugParamsObj().string()});
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::ifstream Ifs(CFile);
  ASSERT_TRUE(Ifs.good()) << CFile;
  const std::string Source((std::istreambuf_iterator<char>(Ifs)),
                           std::istreambuf_iterator<char>());
  for (const char *Definition :
       {"double dp_scale(int32_t n, double x)",
        "int64_t dp_pair_sum(int64_t p, int64_t p_8, int64_t k)",
        "double dp_complex_mix(int32_t n, double z, double z_8)",
        "int64_t dp_stack_tail(int64_t a, int64_t b, int64_t c, int64_t d, "
        "int64_t e, int64_t f, int64_t g, int64_t h)"})
    EXPECT_NE(Source.find(Definition), std::string::npos) << Definition << "\n"
                                                          << Source;

#if defined(_WIN32)
  GTEST_SKIP() << "Microsoft x64 passes arguments by position, not by class";
#endif
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "no C compiler to build the callers";
  if (const auto Reason = neverd::test::cannotRunX86C(Source))
    GTEST_SKIP() << *Reason;
  const auto Callers = tmpFile("debug_params_callers.c");
  std::ofstream(Callers) << SourceCallers;
  const auto Executable =
      tmpFile(std::string("debug_params") + neverd::test::executableSuffix());
  const auto Built =
      exec(NEVERD_TEST_CLANG,
           {"-std=c11", "-O0", "-fno-strict-aliasing", "-Werror=int-conversion",
            CFile.string(), Callers.string(), "-o", Executable.string()});
  ASSERT_TRUE(Built.ok()) << Built.err << "\n" << Source;
  const auto Run = exec(Executable.string(), {});
  EXPECT_TRUE(Run.ok()) << "exit " << Run.exitCode << "\n" << Source;
}

TEST_F(X86_64_DebugParams, LLVMCNamesArgumentsWhereTheConventionPassesThem) {
  // The lifted functions record where each argument arrives, and LLVM-C binds
  // the debug signature by those places as HighC does: n before x, the
  // record's second register p_8 rather than k, and g and h on the stack.
  // dp_scale sits at address zero, as an object's first function does.
  ASSERT_TRUE(fs::exists(debugParamsObj())) << debugParamsObj();
  const auto CFile = tmpFile("debug_params_llvm.c");
  const auto R = exec(ndBin(), {"decompile", "--llvm", "-o", CFile.string(),
                                debugParamsObj().string()});
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::ifstream Ifs(CFile);
  ASSERT_TRUE(Ifs.good()) << CFile;
  const std::string Source((std::istreambuf_iterator<char>(Ifs)),
                           std::istreambuf_iterator<char>());
  auto Signature = [&](const std::string &Name) {
    const size_t Start = Source.find(" " + Name + "(");
    const size_t End = Source.find(") {", Start);
    return Start == std::string::npos || End == std::string::npos
               ? std::string()
               : Source.substr(Start, End - Start + 1);
  };
  const std::string Scale = Signature("dp_scale");
  EXPECT_NE(Scale.find("(uint32_t n, "), std::string::npos) << Source;
  EXPECT_NE(Scale.find(" x)"), std::string::npos) << Source;
  EXPECT_NE(
      Signature("dp_pair_sum").find("(uint64_t p, uint64_t p_8, uint64_t k)"),
      std::string::npos)
      << Source;
  const std::string Mix = Signature("dp_complex_mix");
  EXPECT_NE(Mix.find("(uint32_t n, "), std::string::npos) << Source;
  EXPECT_NE(Mix.find(" z, "), std::string::npos) << Source;
  EXPECT_NE(Mix.find(" z_8)"), std::string::npos) << Source;
  EXPECT_NE(Signature("dp_stack_tail")
                .find("(uint64_t a, uint64_t b, uint64_t c, uint64_t d, "
                      "uint64_t e, uint64_t f, uint64_t g, uint64_t h)"),
            std::string::npos)
      << Source;
}
