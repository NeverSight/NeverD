//===- ARM32_ThreadPointerTests.cpp - __aeabi_read_tp ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The Arm run-time ABI's __aeabi_read_tp returns the thread pointer and
// corrupts only r0, ip, lr and the flags, so clang keeps values in r1-r3
// across a call to it.  A call to it is the thread pointer read, which both C
// routes print as __builtin_thread_pointer(): it takes no argument, and the
// values the caller keeps survive (test_thread_pointer_*_arm.s).
//
//===----------------------------------------------------------------------===//

#include "../../HostCompiler.h"
#include "CSourceText.h"
#include "NeverDLiftFixture.h"

#include <fstream>
#include <sstream>
#include <utility>

namespace {

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

class ARM32_ThreadPointer : public NeverDLiftTest {
protected:
  /// The unit a C route writes for \p Fixture.
  std::string source(const char *Fixture, bool LLVM) {
    const fs::path Object = fs::path(TEST_OBJ_DIR) / Fixture;
    const auto R = LLVM ? decompileToC(Object) : decompileToHighC(Object);
    EXPECT_EQ(R.exitCode, 0) << R.err;
    return readText(tmpFile(LLVM ? "decompiled.c" : "decompiled_high.c"));
  }
};

TEST_F(ARM32_ThreadPointer, CallsAreTheThreadPointerRead) {
  const std::pair<const char *, const char *> Fixtures[] = {
      {"test_thread_pointer_import_arm.o", "tls_bump"},
      {"test_thread_pointer_defined_arm.o", "tls_bump_static"}};
  for (const auto &[Fixture, Bump] : Fixtures)
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(std::string(Fixture) + (LLVM ? " LLVM C" : " HighC"));
      const std::string C = source(Fixture, LLVM);
      const std::string Body = definition(C, Bump);
      EXPECT_NE(Body.find("__builtin_thread_pointer()"), std::string::npos)
          << C;
      EXPECT_EQ(Body.find("__aeabi_read_tp("), std::string::npos) << C;
      // n, kept in r1 across the call, is what the function adds; the TLS
      // offset kept in r2 is no unknown value.
      EXPECT_NE(Body.find("arg0"), std::string::npos) << C;
      EXPECT_EQ(Body.find("__builtin_trap"), std::string::npos) << C;
      if (hasCrossTargetClang()) {
        const auto File = tmpFile(LLVM ? "tp_llvm.c" : "tp_high.c");
        std::ofstream(File) << C;
        const auto Built = exec(
            "clang", {"--target=armv7a-linux-gnueabihf", "-mcpu=cortex-a15",
                      "-ffreestanding", "-c", File.string(), "-o",
                      tmpFile(LLVM ? "tp_llvm.o" : "tp_high.o").string()});
        if (Built.exitCode != 0 && neverd::test::lacksCrossHeaders(Built.err))
          GTEST_SKIP() << "no headers for ARM Linux: " << Built.err;
        EXPECT_EQ(Built.exitCode, 0) << Built.err << "\n" << C;
      }
    }
  // A wrapper returns the thread pointer.
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const std::string C = source("test_thread_pointer_import_arm.o", LLVM);
    const std::string Get = definition(C, "get_tp");
    EXPECT_NE(Get.rfind("void ", 0), 0u) << C;
    EXPECT_NE(Get.find("__builtin_thread_pointer()"), std::string::npos) << C;
  }
  // The routine an image defines still reads the register itself.
  const std::string Static = source("test_thread_pointer_defined_arm.o", false);
  EXPECT_NE(
      definition(Static, "__aeabi_read_tp").find("__arm_mrc(15, 0, 13, 0, 3)"),
      std::string::npos)
      << Static;
}
