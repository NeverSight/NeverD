//===- X86StackFrameTests.cpp - Native stack-frame lifting regressions ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute original and identity-patched ELF stack instructions on the CPU.
//===----------------------------------------------------------------------===//

#include "../NeverDLiftFixture.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

class X86StackFrameTest : public NeverDLiftTest {
protected:
  void compare(const std::string &Assembly, bool Wide) {
#if !defined(__linux__) || !defined(__x86_64__)
    GTEST_SKIP() << "Native Linux x86/i386 execution required";
#else
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "Clang required for original ELF fixture";
    const auto Source = tmpFile("stack.S"), Original = tmpFile("stack");
    std::ofstream File(Source);
    File << ".text\n.globl _start\n.type _start,@function\n_start:\n"
            "call f\n";
    File << (Wide ? "movl %eax,%edi\nmovl $60,%eax\nsyscall\n"
                  : "movl %eax,%ebx\nmovl $1,%eax\nint $0x80\n");
    File << ".size _start,.-_start\n.globl f\n.type f,@function\nf:\n"
         << Assembly << "\n.section .note.GNU-stack,\"\",@progbits\n";
    File.close();
    auto Built = exec(NEVERD_TEST_CLANG,
                      {Wide ? "-m64" : "-m32", "-nostdlib", "-static",
                       "-no-pie", Source.string(), "-o", Original.string()});
    ASSERT_EQ(Built.exitCode, 0) << Built.err;
    const auto Native = [](const fs::path &Path) {
      return neverd::test::systemExitCode(neverd::test::runShellCommand(
          neverd::test::shellQuote(Path.string())));
    };
    ASSERT_EQ(Native(Original), 42);
    auto Patched = patchBinary(Original);
    ASSERT_EQ(Patched.exitCode, 0) << Patched.err;
    EXPECT_EQ(Native(tmpFile("patched")), 42) << Patched.out;
    RecordProperty("cpu_program_comparisons", 1);
    if (const char *Artifacts =
            std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
      const auto Output =
          fs::path(Artifacts) /
          testing::UnitTest::GetInstance()->current_test_info()->name();
      fs::create_directories(Output);
      for (const auto &Path : {Source, Original, tmpFile("patched")})
        fs::copy_file(Path, Output / Path.filename(),
                      fs::copy_options::overwrite_existing);
      std::ofstream(Output / "patch.log") << Patched.out << Patched.err;
    }
#endif
  }
};

TEST_F(X86StackFrameTest, WordPushAllPopAllKeepFullI386StackAddress) {
  compare(R"ASM(
pushl %ebp
pushl %ebx
pushl %esi
pushl %edi
data16 pusha
movw $42,14(%esp)
movl $0xabcd0000,%eax
data16 popa
andl $0xffff,%eax
popl %edi
popl %esi
popl %ebx
popl %ebp
ret
.size f,.-f
)ASM",
          false);
}

void appendDisplayData(std::string &Assembly) {
  Assembly += R"ASM(
.size f,.-f
.data
.balign 65536
frames:
.zero 65534
.short 42
.short 17
.zero 65532
.short 99
)ASM";
}

TEST_F(X86StackFrameTest, WordEnterDisplayBorrowsAcrossBit16OnI386) {
  std::string Assembly = R"ASM(
pushl %ebp
movl %esp,%edx
movl $frames+65538,%ebp
data16 enter $0,$3
movzwl 2(%esp),%eax
movl %edx,%esp
popl %ebp
ret
)ASM";
  appendDisplayData(Assembly);
  compare(Assembly, false);
}

TEST_F(X86StackFrameTest, WordEnterDisplayBorrowsAcrossBit16OnX64) {
  std::string Assembly = R"ASM(
pushq %rbp
movq %rsp,%rdx
leaq frames+65538(%rip),%rbp
data16 enter $0,$3
movzwl 2(%rsp),%eax
movq %rdx,%rsp
popq %rbp
ret
)ASM";
  appendDisplayData(Assembly);
  compare(Assembly, true);
}

} // namespace
