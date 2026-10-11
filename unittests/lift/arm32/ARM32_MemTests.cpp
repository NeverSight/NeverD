#include "NeverDLiftFixture.h"

#include <fstream>
#include <iterator>

class ARM32_Mem : public NeverDLiftTest {};

static fs::path testObj() { return fs::path(TEST_OBJ_DIR) / "test_mem_arm.o"; }

TEST_F(ARM32_Mem, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_mem_arm.o not built";
  verifyAllStages(testObj());
}

TEST_F(ARM32_Mem, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(ARM32_Mem, LdrLifts) {
  verifyLowIRContains(testObj(), "test_ldr_str", "LOAD");
}

TEST_F(ARM32_Mem, StrLifts) {
  verifyLowIRContains(testObj(), "test_str", "STORE");
}

TEST_F(ARM32_Mem, LdrhLifts) {
  verifyLowIRContains(testObj(), "test_ldrh", "LOAD");
}

TEST_F(ARM32_Mem, LdrbLifts) {
  verifyLowIRContains(testObj(), "test_ldrb", "LOAD");
}

TEST_F(ARM32_Mem, PushPopLifts) {
  verifyLowIRContains(testObj(), "test_push_pop", "STORE");
}

TEST_F(ARM32_Mem, NoUnreachableInFunctions) {
  verifyLLVMIRNotContains(testObj(), "", "unreachable");
}

// STREXD reports success in its status register, so the retry loop around it
// runs once.  The flags `cmp rS, #0` sets from that constant status leave no
// reason to test the `adds` before the store instead: an odd x stores a
// nonzero low word, which would add again and carry it into the high word.
TEST_F(ARM32_Mem, ExclusiveStoreRetriesOnItsStatus) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const auto Source = tmpFile("strexd-retry.c");
  const auto Object = tmpFile("strexd-retry.o");
  std::ofstream(Source) << R"C(
unsigned long long counter;
int bump(int x) {
  counter = (unsigned long long)(unsigned)x << 40;
  __atomic_fetch_add(&counter, (unsigned long long)(x & 1) << 31,
                     __ATOMIC_RELAXED);
  return (int)counter;
}
)C";
  const auto Compiled =
      exec(NEVERD_TEST_CLANG, {"-target", "arm-linux-gnueabi",
                               "-mcpu=cortex-a15", "-mfloat-abi=softfp", "-O2",
                               "-c", Source.string(), "-o", Object.string()});
  ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;

  const auto Decompiled = decompileToHighC(Object);
  ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  ASSERT_TRUE(Input.good());
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
  ASSERT_EQ(C.find("unknown"), std::string::npos) << C;

  const auto Program = tmpFile("strexd-retry-host.exe");
  std::ofstream(tmpFile("strexd-retry-host.c")) << C << R"C(
int main(void) {
  return bump(1) != (int)0x80000000u || bump(2) != 0 ||
         bump(3) != (int)0x80000000u;
}
)C";
  const auto Built =
      exec(NEVERD_TEST_CLANG,
           {"-std=gnu11", "-O2", tmpFile("strexd-retry-host.c").string(), "-o",
            Program.string()});
  ASSERT_EQ(Built.exitCode, 0) << Built.err << "\n" << C;
  EXPECT_EQ(exec(Program.string(), {}).exitCode, 0) << C;
}
