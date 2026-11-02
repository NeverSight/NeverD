//===- JumpTableSourceExecution.h - Run native table projections -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute unchanged jump-table C on the input architecture, including the
/// original memory instructions retained beside recovered control flow.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_CORE_JUMPTABLESOURCEEXECUTION_H
#define NEVERD_UNITTESTS_LIFT_CORE_JUMPTABLESOURCEEXECUTION_H

#include "NeverDLiftFixture.h"

#include "llvm/Support/Program.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

class JumpTableSourceExecution : public NeverDLiftTest {
protected:
  static std::optional<std::string> executor(const std::string &Arch) {
    const llvm::Triple Host(llvm::sys::getDefaultTargetTriple());
    if (Host.isOSLinux() && Host.getArchName() == Arch)
      return std::string{};
    auto Emulator = llvm::sys::findProgramByName("qemu-" + Arch);
    if (!Emulator)
      return std::nullopt;
    return *Emulator;
  }

  void expectSourceRuns(const fs::path &Source, const std::string &Arch,
                        const std::string &Emulator,
                        const std::vector<std::string> &Objects = {}) {
    std::ifstream Input(Source);
    ASSERT_TRUE(Input.good());
    const std::string SourceText((std::istreambuf_iterator<char>(Input)), {});
    // No target libc is needed. Keep the generated source unchanged and use
    // Clang's freestanding integer headers plus these ordinary byte helpers.
    const auto Headers = tmpFile("native-headers");
    fs::create_directories(Headers);
    std::ofstream(Headers / "string.h")
        << "#include <stddef.h>\n"
           "void *memcpy(void *, const void *, size_t);\n"
           "void *memmove(void *, const void *, size_t);\n"
           "void *memset(void *, int, size_t);\n";
    const auto Runtime = tmpFile("native-runtime.c");
    std::ofstream Out(Runtime);
    Out << R"C(
#include <stddef.h>
#include <stdint.h>
void *memcpy(void *d, const void *s, size_t n) {
  unsigned char *out = d;
  const unsigned char *in = s;
  for (size_t i = 0; i < n; ++i) out[i] = in[i];
  return d;
}
void *memmove(void *d, const void *s, size_t n) {
  unsigned char *out = d;
  const unsigned char *in = s;
  if ((uintptr_t)d < (uintptr_t)s)
    for (size_t i = 0; i < n; ++i) out[i] = in[i];
  else
    while (n) { --n; out[n] = in[n]; }
  return d;
}
void *memset(void *d, int value, size_t n) {
  unsigned char *out = d;
  for (size_t i = 0; i < n; ++i) out[i] = (unsigned char)value;
  return d;
}
#if defined(__aarch64__)
__asm__(".global _start\n_start:\nbl main\nmov x8, #93\nsvc #0\n");
#elif defined(__x86_64__)
__asm__(".global _start\n_start:\nandq $-16, %rsp\ncall main\n"
        "movl %eax, %edi\nmovl $60, %eax\nsyscall\n");
#else
#error Unsupported jump-table execution target
#endif
)C";
    Out.close();
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      const auto Product = tmpFile("native-selection.elf");
      std::vector<std::string> Args{"-target",
                                    Arch + "-linux-gnu",
                                    "-std=c11",
                                    Optimization,
                                    "-ffreestanding",
                                    "-fno-builtin",
                                    "-fno-strict-aliasing",
                                    "-fno-stack-protector",
                                    "-nostdlib",
                                    "-static",
                                    "-fuse-ld=lld",
                                    "-Wl,-e,_start",
                                    "-Werror",
                                    "-fsanitize=undefined",
                                    "-fsanitize-trap=undefined",
                                    "-I" + Headers.string(),
                                    Source.string(),
                                    Runtime.string(),
                                    "-o",
                                    Product.string()};
      Args.insert(Args.end(), Objects.begin(), Objects.end());
      const auto Built = exec(NEVERD_TEST_CLANG, Args);
      ASSERT_TRUE(Built.ok()) << Built.err << "\n" << SourceText;
      const auto Run = Emulator.empty() ? exec(Product.string(), {})
                                        : exec(Emulator, {Product.string()});
      ASSERT_TRUE(Run.ok()) << Run.err << "\n" << SourceText;
    }
  }
};

#endif // NEVERD_UNITTESTS_LIFT_CORE_JUMPTABLESOURCEEXECUTION_H
