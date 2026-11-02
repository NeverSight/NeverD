//===- HighCHostExecution.h - Run decompiled HighC on the host --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A fixture that compiles a C kernel for a target, decompiles the object to
/// HighC, or to the LLVM route's C, and runs the C on the host against the
/// kernel's own C.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_HIGHCHOSTEXECUTION_H
#define NEVERD_UNITTESTS_LIFT_HIGHCHOSTEXECUTION_H

#include "NeverDLiftFixture.h"

#include "neverd/backend/c/render/AArch64/AArch64AddressMemoryC.h"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

class HighCHostExecutionTest : public NeverDLiftTest {
protected:
  /// Flags for the host build, such as -m32 for C decompiled from a 32-bit
  /// target, whose addresses travel in 32-bit integers.
  std::vector<std::string> HostFlags;
  /// The optimization level the kernel is built at: -O0 keeps every local in
  /// a frame slot.
  std::string KernelOptimization = "-O2";
  /// Run the C the LLVM route writes (`decompile --llvm`) instead of HighC.
  bool LLVMC = false;

  /// Compile \p Kernel with Clang and \p TargetFlags, decompile it to HighC,
  /// and expect the C of \p Entry, which takes and returns an int, to return
  /// what \p Kernel's does for each of \p Values.  Only the int the ABI
  /// returns is compared: the C may return the whole register.  \p Names are
  /// the kernel's functions and globals, renamed in the reference copy.  An
  /// unknown value in the C fails.
  void expectHighCRunsLikeSource(const std::vector<std::string> &TargetFlags,
                                 const std::string &Kernel,
                                 const std::string &Entry,
                                 std::initializer_list<const char *> Names,
                                 std::initializer_list<int> Values) {
    expectHighCRunsLikeSource(NEVERD_TEST_CLANG, TargetFlags, Kernel, Entry,
                              Names, Values);
  }

  /// As above, compiling \p Kernel with \p Compiler.  A compiler other than
  /// the test's Clang that cannot build the kernel skips the test.
  void expectHighCRunsLikeSource(const std::string &Compiler,
                                 const std::vector<std::string> &TargetFlags,
                                 const std::string &Kernel,
                                 const std::string &Entry,
                                 std::initializer_list<const char *> Names,
                                 std::initializer_list<int> Values) {
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "cross-target compilation requires Clang";
#if defined(__APPLE__)
    // These kernels retain addresses in 32-bit integers. A 64-bit host build
    // would change their meaning, and macOS cannot run a 32-bit executable.
    if (std::find(HostFlags.begin(), HostFlags.end(), "-m32") !=
        HostFlags.end())
      GTEST_SKIP() << "32-bit native HighC execution is unavailable on macOS";
#endif
    const auto Source = tmpFile("kernel.c");
    const auto Object = tmpFile("kernel.o");
    std::ofstream(Source) << Kernel;
    std::vector<std::string> Args = TargetFlags;
    Args.insert(Args.end(), {KernelOptimization, "-fno-stack-protector",
                             "-fno-asynchronous-unwind-tables", "-c",
                             Source.string(), "-o", Object.string()});
    const auto Compiled = exec(Compiler, Args);
    if (Compiled.exitCode != 0 && Compiler != NEVERD_TEST_CLANG)
      GTEST_SKIP() << Compiler << " cannot build the kernel: " << Compiled.err;
    ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;

    const auto Decompiled =
        LLVMC ? decompileToC(Object) : decompileToHighC(Object);
    ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
    const auto Output = tmpFile(LLVMC ? "decompiled.c" : "decompiled_high.c");
    std::ifstream Input(Output);
    ASSERT_TRUE(Input.good());
    const std::string C((std::istreambuf_iterator<char>(Input)),
                        std::istreambuf_iterator<char>());
    ASSERT_EQ(C.find("unknown"), std::string::npos) << C;

    // AArch64 C retains each original register-offset memory instruction as
    // its encoded word (AArch64AddressMemory.def), which only an AArch64 host
    // executes. Elsewhere the C must still build for the kernel's target; the
    // comparison against the kernel is then an explicit skip, not a pass.
#if !defined(__aarch64__)
    if (C.find(neverd::kAddressMemoryLoadCAsm) != std::string::npos ||
        C.find(neverd::kAddressMemoryStoreCAsm) != std::string::npos) {
      std::vector<std::string> TargetArgs = TargetFlags;
      TargetArgs.insert(TargetArgs.end(), {"-ffreestanding", "-std=gnu11",
                                           "-fno-strict-aliasing"});
      const auto Target = checkHighCClangCompile(Output, TargetArgs);
      ASSERT_EQ(Target.exitCode, 0) << Target.err << "\n" << C;
      GTEST_SKIP() << "the C retains original AArch64 memory instructions; "
                      "executing it requires an AArch64 host";
    }
#endif

    std::string Reference = Kernel;
    for (const std::string Name : Names)
      Reference = std::regex_replace(
          Reference, std::regex("\\b" + Name + "\\b"), "ref_" + Name);
    std::string Main = "int main(void) {\n  static const int Values[] = {";
    for (int Value : Values)
      Main += std::to_string(Value) + ", ";
    Main += "};\n  for (unsigned I = 0; I != sizeof(Values) / sizeof(*Values);"
            " ++I)\n    if ((int)" +
            Entry + "(Values[I]) != ref_" + Entry +
            "(Values[I]))\n      return 1;\n  return 0;\n}\n";
    const auto HostSource = tmpFile("host.c");
    const auto ReferenceSource = tmpFile("reference.c");
    const auto ReferenceObject = tmpFile("reference.o");
    const auto Program = tmpFile("host.exe");
    std::ofstream(ReferenceSource) << Reference;
    std::ofstream(HostSource) << C << "\nextern int ref_" << Entry << "(int);\n"
                              << Main;
    // Keep the source oracle independent of optimized generated C. Besides
    // preventing cross-copy optimization, this avoids making both sides
    // depend on host optimizer defects (e.g. a mis-reassociated, unrolled
    // subtraction recurrence). The actual target kernel still uses the
    // requested optimization level above.
    std::vector<std::string> ReferenceArgs = {"-std=gnu11", "-O0",
                                              "-fno-strict-aliasing"};
    ReferenceArgs.insert(ReferenceArgs.end(), HostFlags.begin(),
                         HostFlags.end());
    ReferenceArgs.insert(ReferenceArgs.end(), {"-c", ReferenceSource.string(),
                                               "-o", ReferenceObject.string()});
    const auto ReferenceBuilt = exec(NEVERD_TEST_CLANG, ReferenceArgs);
    ASSERT_EQ(ReferenceBuilt.exitCode, 0) << ReferenceBuilt.err;
    // The C reads and writes scalars through plain pointer casts, which its
    // header says to build with -fno-strict-aliasing, as the semantic grid
    // does.
    std::vector<std::string> HostArgs = {"-std=gnu11", "-O2",
                                         "-fno-strict-aliasing"};
    HostArgs.insert(HostArgs.end(), HostFlags.begin(), HostFlags.end());
    HostArgs.insert(HostArgs.end(),
                    {HostSource.string(), ReferenceObject.string(), "-o",
                     Program.string()});
    const auto Built = exec(NEVERD_TEST_CLANG, HostArgs);
    ASSERT_EQ(Built.exitCode, 0) << Built.err << "\n" << C;
    EXPECT_EQ(exec(Program.string(), {}).exitCode, 0) << C;
  }
};

#endif // NEVERD_UNITTESTS_LIFT_HIGHCHOSTEXECUTION_H
