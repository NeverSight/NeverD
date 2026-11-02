//===- RecoverySourcePublicationTests.cpp - Source input-domain checks ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../lift/NeverDLiftFixture.h"

#include "neverd/Limits.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

namespace {

std::string readPublicationFile(const fs::path &Path) {
  std::ifstream Input(Path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(Input), {});
}

class RecoverySourcePublicationTest : public NeverDLiftTest {};

TEST_F(RecoverySourcePublicationTest,
       LargeMutableRecoveryExecutesBothCOutputs) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "source recovery checks require clang";
  const auto Assembly = tmpFile("large-mutable.S");
  const auto Binary = tmpFile("large-mutable.elf");
  {
    std::ofstream Output(Assembly);
    Output << ".text\n.globl large_probe\n.type large_probe,@function\n"
              "large_probe:\n.rept "
           << neverd::limits::kMaxSSAFunctionOps + 1
           << "\n movq %rdi,%rax\n.endr\nret\n"
              ".size large_probe,.-large_probe\n";
  }
  const auto Built =
      exec(NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-fuse-ld=lld",
                               "-nostdlib", "-static", "-Wl,-e,large_probe",
                               Assembly.string(), "-o", Binary.string()});
  ASSERT_TRUE(Built.ok()) << Built.err;
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
    const auto Source = tmpFile(LLVM ? "large-llvm.c" : "large-high.c");
    const auto Report = tmpFile(LLVM ? "large-llvm.json" : "large-high.json");
    std::vector<std::string> Args{"decompile",
                                  "--func",
                                  "large_probe",
                                  "--devirtualize",
                                  "--vm-machine-state",
                                  "--vm-chain-transfers=4096",
                                  "--vm-max-operations=10000000",
                                  "--vm-max-nodes=1000000",
                                  "--vm-no-control-discovery",
                                  "--recovery-report=" + Report.string(),
                                  "-o",
                                  Source.string(),
                                  Binary.string()};
    if (LLVM)
      Args.push_back("--llvm");
    const auto Recovered = exec(ndBin(), Args);
    ASSERT_TRUE(Recovered.ok()) << Recovered.err;
    auto Evidence = llvm::json::parse(readPublicationFile(Report));
    ASSERT_TRUE(bool(Evidence));
    ASSERT_NE(Evidence->getAsObject(), nullptr);
    EXPECT_EQ(Evidence->getAsObject()->getBoolean("complete"), true);
    {
      std::ofstream Output(Source, std::ios::app);
      Output << R"(
int main(void) {
  const uint64_t values[] = {0, 1, UINT64_MAX, UINT64_C(0x123456789abcdef0)};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    uint64_t state[17] = {0}, stack[8] = {0};
    state[4] = (uintptr_t)&stack[4];
    state[7] = values[i];
    state[16] = 2;
    if (large_probe((void *)state) != 0 || state[0] != values[i]) return 1;
    if (state[4] != (uintptr_t)&stack[4] || state[7] != values[i]) return 2;
  }
  return 0;
}
)";
    }
    for (const char *Optimization : {"-O0", "-O2"}) {
      const auto Executable = tmpFile(std::string("large-check") +
                                      neverd::test::executableSuffix());
      const auto Compiled = exec(
          NEVERD_TEST_CLANG, {"-std=c11", Optimization, "-fsanitize=undefined",
                              "-fno-sanitize-recover=all", Source.string(),
                              "-o", Executable.string()});
      ASSERT_TRUE(Compiled.ok()) << Compiled.err;
      const auto Ran = exec(Executable.string(), {});
      EXPECT_TRUE(Ran.ok()) << Ran.err;
    }
  }
}

TEST_F(RecoverySourcePublicationTest,
       NarrowArithmeticShiftCarrySurvivesBothSourceBackends) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "source recovery checks require clang";
  for (unsigned Width : {8u, 16u}) {
    SCOPED_TRACE(Width);
    const auto Assembly = tmpFile("narrow-carry.S");
    const auto Binary = tmpFile("narrow-carry.elf");
    {
      std::ofstream Output(Assembly);
      Output << ".text\n.globl carry_probe\n.type carry_probe,@function\n"
                "carry_probe:\n  xorl %eax, %eax\n  movl %esi, %ecx\n";
      Output << (Width == 8 ? "  movb %dil, %al\n" : "  movw %di, %ax\n");
      Output << "  btl $0, %edx\n";
      Output << (Width == 8 ? "  sarb %cl, %al\n" : "  sarw %cl, %ax\n");
      Output << "  setc %dl\n  movzbl %dl, %edx\n";
      Output << (Width == 8 ? "  movzbl %al, %eax\n" : "  movzwl %ax, %eax\n");
      Output << "  shll $" << Width
             << ", %edx\n  orl %edx, %eax\n  ret\n"
                ".size carry_probe, .-carry_probe\n";
    }
    const auto Built =
        exec(NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-fuse-ld=lld",
                                 "-nostdlib", "-static", "-Wl,-e,carry_probe",
                                 Assembly.string(), "-o", Binary.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
      const auto Source = tmpFile(LLVM ? "carry-llvm.c" : "carry-high.c");
      std::vector<std::string> Args{
          "decompile",          "--func", "carry_probe",   "--devirtualize",
          "--vm-machine-state", "-o",     Source.string(), Binary.string()};
      if (LLVM)
        Args.push_back("--llvm");
      const auto Recovered = exec(ndBin(), Args);
      ASSERT_TRUE(Recovered.ok()) << Recovered.err;
      {
        std::ofstream Output(Source, std::ios::app);
        Output << R"(
#include <stdint.h>
int main(void) {
  const unsigned width = )"
               << Width << R"(;
  const uint64_t sign = UINT64_C(1) << (width - 1);
  const uint64_t mask = (sign << 1) - 1;
  const uint64_t words[] = {0, 1, 2, 0x7f, 0x80, 0xff, 0x100,
                           0x7fff, 0x8000, 0x8001, 0xfffe, 0xffff};
  const unsigned values = width == 8 ? 256 : sizeof(words) / sizeof(words[0]);
  for (unsigned v = 0; v < values; ++v)
    for (unsigned count = 0; count < 256; ++count)
      for (unsigned initial = 0; initial < 2; ++initial) {
        uint64_t value = (width == 8 ? v : words[v]) & mask;
        uint64_t expected = value, carry = initial;
        // An independent sequence of one-bit arithmetic shifts avoids a
        // signed C right shift or a shift by/above the host operand width.
        for (unsigned i = 0; i < (count & 31); ++i) {
          carry = expected & 1;
          expected = (expected >> 1) | (expected & sign);
        }
        uint64_t state[17] = {0}, stack[8] = {0};
        state[2] = initial;
        state[4] = (uintptr_t)&stack[4];
        state[6] = count;
        state[7] = value;
        state[16] = 2;
        if (carry_probe((void *)state) != 0)
          return 1;
        if (state[0] != (expected | (carry << width)))
          return 2;
      }
  return 0;
}
)";
      }
      for (const char *Optimization : {"-O0", "-O2"}) {
        SCOPED_TRACE(Optimization);
        const auto Executable = tmpFile(std::string("carry-check") +
                                        neverd::test::executableSuffix());
        const auto Compiled =
            exec(NEVERD_TEST_CLANG,
                 {"-std=c11", Optimization, "-fsanitize=undefined",
                  "-fno-sanitize-recover=all", Source.string(), "-o",
                  Executable.string()});
        ASSERT_TRUE(Compiled.ok()) << Compiled.err;
        const auto Ran = exec(Executable.string(), {});
        EXPECT_TRUE(Ran.ok()) << Ran.err << Ran.out;
      }
    }
  }
}

TEST_F(RecoverySourcePublicationTest, UnboundEntryBytesRequireExplicitState) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "source publication checks require clang";
  const auto Assembly = tmpFile("entry-inputs.S");
  const auto Binary = tmpFile("entry-inputs.elf");
  {
    std::ofstream Output(Assembly);
    Output << R"(.text
.globl entry_return_input
.type entry_return_input,@function
entry_return_input:
  addq $1, %rax
  ret
.size entry_return_input, .-entry_return_input
.globl entry_byte_store
.type entry_byte_store,@function
entry_byte_store:
  movb %al, (%rdi)
  xorl %eax, %eax
  ret
.size entry_byte_store, .-entry_byte_store
.globl entry_preserved_store
.type entry_preserved_store,@function
entry_preserved_store:
  movq %r12, (%rdi)
  xorl %eax, %eax
  ret
.size entry_preserved_store, .-entry_preserved_store
.globl entry_frame_return
.type entry_frame_return,@function
entry_frame_return:
  subq $16, %rsp
  movq $0, (%rsp)
  movq %rsp, %rax
  addq $1, %rax
  addq $16, %rsp
  ret
.size entry_frame_return, .-entry_frame_return
.globl entry_frame_branch
.type entry_frame_branch,@function
entry_frame_branch:
  subq $16, %rsp
  movq $0, (%rsp)
  testq $64, %rsp
  jz 1f
  movl $1, %eax
  jmp 2f
1:
  movl $2, %eax
2:
  addq $16, %rsp
  ret
.size entry_frame_branch, .-entry_frame_branch
.globl entry_frame_store
.type entry_frame_store,@function
entry_frame_store:
  subq $16, %rsp
  movq %rsp, (%rdi)
  xorl %eax, %eax
  addq $16, %rsp
  ret
.size entry_frame_store, .-entry_frame_store
.globl entry_frame_uninitialized
.type entry_frame_uninitialized,@function
entry_frame_uninitialized:
  subq $16, %rsp
  movq (%rsp), %rax
  addq $1, %rax
  addq $16, %rsp
  ret
.size entry_frame_uninitialized, .-entry_frame_uninitialized
)";
  }
  const auto Built = exec(NEVERD_TEST_CLANG,
                          {"-target", "x86_64-linux-gnu", "-fuse-ld=lld",
                           "-nostdlib", "-static", "-Wl,-e,entry_return_input",
                           Assembly.string(), "-o", Binary.string()});
  ASSERT_TRUE(Built.ok()) << Built.err;

  const char *Names[] = {"entry_return_input",       "entry_byte_store",
                         "entry_preserved_store",    "entry_frame_return",
                         "entry_frame_branch",       "entry_frame_store",
                         "entry_frame_uninitialized"};
  for (unsigned Kind = 0; Kind < 7; ++Kind)
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Names[Kind]);
      SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
      const std::string Stem =
          std::string(Names[Kind]) + (LLVM ? "-llvm" : "-high");
      const auto Source = tmpFile(Stem + ".c");
      const auto Report = tmpFile(Stem + ".json");
      std::vector<std::string> Arguments{"decompile",
                                         "--func",
                                         Names[Kind],
                                         "--devirtualize",
                                         "--vm-control=r10",
                                         "--recovery-report=" + Report.string(),
                                         "-o",
                                         Source.string(),
                                         Binary.string()};
      if (LLVM)
        Arguments.push_back("--llvm");
      const auto Refused = exec(ndBin(), Arguments);
      EXPECT_FALSE(Refused.ok());
      EXPECT_FALSE(fs::exists(Source));
      auto Evidence = llvm::json::parse(readPublicationFile(Report));
      ASSERT_TRUE(static_cast<bool>(Evidence))
          << llvm::toString(Evidence.takeError());
      const auto *Object = Evidence->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getBoolean("controlComplete"), true);
      EXPECT_EQ(Object->getBoolean("complete"), false);
      EXPECT_TRUE(Object->getString("frameContract").has_value());
      ASSERT_TRUE(Object->getString("error").has_value());
      EXPECT_NE(Object->getString("error")->find("entry register byte"),
                std::string::npos);

      Arguments.push_back("--vm-machine-state");
      const auto Recovered = exec(ndBin(), Arguments);
      ASSERT_TRUE(Recovered.ok()) << Recovered.err;
      auto MachineEvidence = llvm::json::parse(readPublicationFile(Report));
      ASSERT_TRUE(static_cast<bool>(MachineEvidence))
          << llvm::toString(MachineEvidence.takeError());
      ASSERT_NE(MachineEvidence->getAsObject(), nullptr);
      EXPECT_EQ(MachineEvidence->getAsObject()->getBoolean("complete"), true);
      EXPECT_FALSE(MachineEvidence->getAsObject()
                       ->getString("frameContract")
                       .has_value());
      {
        std::ofstream Output(Source, std::ios::app);
        Output << "\n#include <stdint.h>\nint main(void) {\n"
                  "  const uint64_t inputs[] = {0, 1, 255, "
                  "UINT64_C(0x923456789abcdef0)};\n"
                  "  for (unsigned k=0; k<4; ++k) {\n"
                  "    uint64_t state[17] = {0}, stack[16] = {0};\n"
                  "    uint64_t memory = UINT64_C(0xa1b2c3d4e5f60718);\n"
                  "    stack[6] = inputs[k];\n"
                  "    state[0] = inputs[k];\n"
                  "    state[4] = (uintptr_t)&stack[8];\n"
                  "    state[7] = (uintptr_t)&memory;\n"
                  "    state[12] = inputs[k];\n"
                  "    state[16] = 2;\n"
                  "    if ("
               << Names[Kind]
               << "((void*)state) != 0) return 1;\n"
                  "    if (state[4] != (uintptr_t)&stack[8]) return 2;\n"
                  "    if (state[12] != inputs[k]) return 3;\n";
        if (Kind == 0)
          Output << "    if (state[0] != inputs[k]+1) return 4;\n";
        else if (Kind == 1)
          Output << "    if (state[0] != 0 || memory != "
                    "(UINT64_C(0xa1b2c3d4e5f60700) | (inputs[k]&255))) "
                    "return 5;\n";
        else if (Kind == 2)
          Output << "    if (state[0] != 0 || memory != inputs[k]) return 6;\n";
        else if (Kind == 3)
          Output << "    if (state[0] != (uintptr_t)&stack[6]+1) return 7;\n";
        else if (Kind == 4)
          Output << "    if (state[0] != (((uintptr_t)&stack[6]&64) ? 1 : 2)) "
                    "return 8;\n";
        else if (Kind == 5)
          Output << "    if (state[0] != 0 || memory != (uintptr_t)&stack[6]) "
                    "return 9;\n";
        else
          Output << "    if (state[0] != inputs[k]+1) return 10;\n";
        Output << "  }\n  return 0;\n}\n";
      }
      const auto Executable = tmpFile(Stem + neverd::test::executableSuffix());
      const auto Compiled =
          exec(NEVERD_TEST_CLANG,
               {"-x", "c", "-std=c2x", "-O2", "-fsanitize=undefined",
                "-fno-sanitize-recover=all", Source.string(), "-o",
                Executable.string()});
      ASSERT_TRUE(Compiled.ok()) << Compiled.err;
      const auto Ran = exec(Executable.string(), {});
      EXPECT_TRUE(Ran.ok()) << Ran.err << "exit=" << Ran.exitCode;
    }
}

} // namespace
