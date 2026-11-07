//===- ARM32_CoprocessorTests.cpp - Coprocessor register transfers -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// MRC and MRRC read a system register into their registers, and MCR and
// MCRR write one (test_coprocessor_arm.s).  Both C routes call ACLE's
// __arm_mrc, __arm_mcr, __arm_mrrc and __arm_mcrr with the instruction's
// numbers, and the C builds for the target.
//
//===----------------------------------------------------------------------===//

#include "../../HostCompiler.h"
#include "CSourceText.h"
#include "NeverDLiftFixture.h"

#include <fstream>
#include <sstream>

namespace {

fs::path coprocessorObj() {
  return fs::path(TEST_OBJ_DIR) / "test_coprocessor_arm.o";
}

std::string readText(const fs::path &Path) {
  std::ifstream In(Path);
  std::ostringstream Text;
  Text << In.rdbuf();
  return Text.str();
}

/// The definition of \p Name in C source \p C, from its return type.
std::string definition(const std::string &C, const std::string &Name) {
  const size_t At = neverd::test::cDefinitionAt(C, Name);
  if (At == std::string::npos)
    return {};
  const size_t Begin = C.rfind('\n', At) + 1;
  return C.substr(Begin, C.find("\n}", At) - Begin);
}

} // namespace

class ARM32_Coprocessor : public NeverDLiftTest {
protected:
  /// The unit a C route writes for the fixture.
  std::string source(bool LLVM) {
    const auto R = LLVM ? decompileToC(coprocessorObj())
                        : decompileToHighC(coprocessorObj());
    EXPECT_EQ(R.exitCode, 0) << R.err;
    return readText(tmpFile(LLVM ? "decompiled.c" : "decompiled_high.c"));
  }
};

TEST_F(ARM32_Coprocessor, BothRoutesCallTheACLEIntrinsics) {
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const std::string C = source(LLVM);
    const std::string ReadTls = definition(C, "read_tls");
    EXPECT_NE(ReadTls.find("__arm_mrc(15, 0, 13, 0, 3)"), std::string::npos)
        << C;
    // A function that returns the register returns a value.
    EXPECT_NE(ReadTls.rfind("void ", 0), 0u) << C;
    EXPECT_NE(
        definition(C, "tls_slot_thumb").find("__arm_mrc(15, 0, 13, 0, 3)"),
        std::string::npos)
        << C;
    const std::string Write = definition(C, "write_tpidrurw");
    EXPECT_NE(Write.find("__arm_mcr(15, 0, "), std::string::npos) << C;
    EXPECT_NE(Write.find(", 13, 0, 2)"), std::string::npos) << C;
    EXPECT_NE(definition(C, "read_cntvct").find("__arm_mrrc(15, 1, 14)"),
              std::string::npos)
        << C;
    const std::string Compare = definition(C, "write_cntv_cval");
    EXPECT_NE(Compare.find("__arm_mcrr(15, 3, "), std::string::npos) << C;
    EXPECT_NE(Compare.find(", 14)"), std::string::npos) << C;
    // Reading the debug transfer register takes its word, so the read stays
    // though nothing uses the value.
    EXPECT_NE(definition(C, "drain_dcc").find("__arm_mrc(14, 0, 0, 5, 0)"),
              std::string::npos)
        << C;
    EXPECT_NE(C.find("#include <arm_acle.h>"), std::string::npos) << C;
    EXPECT_EQ(C.find("__builtin_trap"), std::string::npos) << C;
    // The MRC that writes the flags has no typed lift, and the unit says so.
    EXPECT_NE(C.find("/* neverd.not-decompiled: dscr_flags at 0x"),
              std::string::npos)
        << C;
    if (hasCrossTargetClang()) {
      const auto File = tmpFile(LLVM ? "coprocessor_llvm.c" : "coprocessor.c");
      std::ofstream(File) << C;
      const auto Syntax =
          exec("clang", {"--target=armv7a-linux-gnueabihf", "-mcpu=cortex-a15",
                         "-ffreestanding", "-fsyntax-only", File.string()});
      if (Syntax.exitCode != 0 && neverd::test::lacksCrossHeaders(Syntax.err))
        GTEST_SKIP() << "no headers for ARM Linux: " << Syntax.err;
      EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err << "\n" << C;
    }
  }
}
