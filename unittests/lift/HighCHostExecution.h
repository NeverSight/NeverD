//===- HighCHostExecution.h - Run decompiled HighC on the host --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A fixture that compiles a C kernel for a target, decompiles the object to
/// HighC, and runs the HighC on the host against the kernel's own C.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_HIGHCHOSTEXECUTION_H
#define NEVERD_UNITTESTS_LIFT_HIGHCHOSTEXECUTION_H

#include "NeverDLiftFixture.h"

#include <fstream>
#include <initializer_list>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

class HighCHostExecutionTest : public NeverDLiftTest {
protected:
  /// Compile \p Kernel with Clang and \p TargetFlags, decompile it to HighC,
  /// and expect the C of \p Entry, which takes and returns an int, to return
  /// what \p Kernel's does for each of \p Values.  Only the int the ABI
  /// returns is compared: the C may return the whole register.  \p Names are
  /// the kernel's functions, renamed in the reference copy.  An unknown value
  /// in the C fails.
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
    const auto Source = tmpFile("kernel.c");
    const auto Object = tmpFile("kernel.o");
    std::ofstream(Source) << Kernel;
    std::vector<std::string> Args = TargetFlags;
    Args.insert(Args.end(), {"-O2", "-fno-stack-protector",
                             "-fno-asynchronous-unwind-tables", "-c",
                             Source.string(), "-o", Object.string()});
    const auto Compiled = exec(Compiler, Args);
    if (Compiled.exitCode != 0 && Compiler != NEVERD_TEST_CLANG)
      GTEST_SKIP() << Compiler << " cannot build the kernel: " << Compiled.err;
    ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;

    const auto Decompiled = decompileToHighC(Object);
    ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
    std::ifstream Input(tmpFile("decompiled_high.c"));
    ASSERT_TRUE(Input.good());
    const std::string C((std::istreambuf_iterator<char>(Input)),
                        std::istreambuf_iterator<char>());
    ASSERT_EQ(C.find("unknown"), std::string::npos) << C;

    std::string Reference = Kernel;
    for (const std::string Name : Names)
      Reference = std::regex_replace(
          Reference, std::regex("\\b" + Name + "\\("), "ref_" + Name + "(");
    std::string Main = "int main(void) {\n  static const int Values[] = {";
    for (int Value : Values)
      Main += std::to_string(Value) + ", ";
    Main += "};\n  for (unsigned I = 0; I != sizeof(Values) / sizeof(*Values);"
            " ++I)\n    if ((int)" +
            Entry + "(Values[I]) != ref_" + Entry +
            "(Values[I]))\n      return 1;\n  return 0;\n}\n";
    const auto HostSource = tmpFile("host.c");
    const auto Program = tmpFile("host.exe");
    std::ofstream(HostSource) << C << "\n" << Reference << "\n" << Main;
    const auto Built =
        exec(NEVERD_TEST_CLANG, {"-std=gnu11", "-O2", HostSource.string(), "-o",
                                 Program.string()});
    ASSERT_EQ(Built.exitCode, 0) << Built.err << "\n" << C;
    EXPECT_EQ(exec(Program.string(), {}).exitCode, 0) << C;
  }
};

#endif // NEVERD_UNITTESTS_LIFT_HIGHCHOSTEXECUTION_H
