#include "HighCHostExecution.h"
#include "NeverDLiftFixture.h"

#include <fstream>
#include <iterator>
#include <regex>

class ARM32_CallAbi : public HighCHostExecutionTest {};

// The base AAPCS (softfp) passes an external routine's float arguments in
// r0-r3, not in the VFP registers the image's own code computes in.  Here
// the VFP registers hold x + 1 and x * 0.5 while r0 and r1 carry them with a
// mantissa bit flipped, so binding the call from the VFP registers changes
// what powf receives.
TEST_F(ARM32_CallAbi, SoftFloatExternalTakesItsFloatsInCoreRegisters) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const std::string Kernel = R"C(
float powf(float, float);
int pw(int a) {
  float x = (float)a;
  union { float f; unsigned u; } b = {x + 1.0f}, e = {x * 0.5f}, r;
  b.u ^= 0x00400000u;
  e.u ^= 0x00200000u;
  r.f = powf(b.f, e.f);
  return (int)r.u;
}
)C";
  const auto Source = tmpFile("softfp-powf.c");
  const auto Object = tmpFile("softfp-powf.o");
  std::ofstream(Source) << Kernel;
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
  ASSERT_EQ(C.find("unknown value"), std::string::npos) << C;

  const std::string Reference = std::regex_replace(
      Kernel, std::regex("\\bpw\\("), std::string("ref_pw("));
  const auto Program = tmpFile("softfp-powf-host.exe");
  std::ofstream(tmpFile("softfp-powf-host.c")) << C << "\n"
                                               << Reference << R"C(
int main(void) {
  static const int Values[] = {-2, 0, 1, 2, 3, 4, 6};
  for (unsigned I = 0; I != sizeof(Values) / sizeof(Values[0]); ++I)
    if (pw(Values[I]) != ref_pw(Values[I]))
      return 1;
  return 0;
}
)C";
  const auto Built =
      exec(NEVERD_TEST_CLANG,
           {"-std=gnu11", "-O2", tmpFile("softfp-powf-host.c").string(), "-lm",
            "-o", Program.string()});
  ASSERT_EQ(Built.exitCode, 0) << Built.err << "\n" << C;
  const auto Run = exec(Program.string(), {});
  EXPECT_EQ(Run.exitCode, 0) << C;
}

// AAPCS returns a narrow value extended to 32 bits.  This function's -O0
// code loads its unsigned byte with LDRB, so it returns the byte
// zero-extended: its C must return an unsigned type, which the host extends
// alike, rather than sign-extend 0xA5 to 0xFFFFFFA5.
TEST_F(ARM32_CallAbi, AZeroExtendedByteReturnIsUnsigned) {
  expectHighCRunsLikeSource({"-target", "arm-linux-gnueabi", "-mcpu=cortex-a15",
                             "-mfloat-abi=softfp"},
                            R"C(
__attribute__((optnone)) int rev8(int x) {
  unsigned char b = (unsigned char)x;
  b = (unsigned char)(((b >> 4) & 0x0F) | ((b << 4) & 0xF0));
  b = (unsigned char)(((b >> 2) & 0x33) | ((b << 2) & 0xCC));
  b = (unsigned char)(((b >> 1) & 0x55) | ((b << 1) & 0xAA));
  return b;
}
)C",
                            "rev8", {"rev8"},
                            {0, 1, -1, 0xA5, 0x80, 0x55AA, 0x12345678});
}
