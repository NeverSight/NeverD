//===- AArch64_VariadicSaveAreaTests.cpp - AAPCS64 vector save area -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What the C keeps of an AAPCS64 variadic function's register save area.
/// q0-q7 carry only the floating and vector arguments a caller passes, so a
/// function whose va_arg reads none leaves that half unread and neither C
/// route writes it; one that reads a double keeps it.  Windows ARM64 and
/// Apple arm64 save no vector register for va_arg.
///
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/backend/c/render/AArch64/AArch64AddressMemoryC.h"

#include "llvm/ADT/StringRef.h"

#include <fstream>
#include <initializer_list>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>

namespace {

// Twelve ints: x1-x7 and five on the stack.
constexpr char kIntegerKernel[] = R"C(
int vmany(int n, ...) __attribute__((noinline));
int vmany_e(int x) {
  unsigned u = (unsigned)x;
  return vmany(12, (int)u, (int)(u + 1u), (int)(u * 2u), (int)(u ^ 3u),
               (int)(u - 4u), (int)(u * 5u), (int)(u + 6u), (int)(u ^ 7u),
               (int)(u - 8u), (int)(u * 9u), (int)(u + 10u), (int)(u ^ 11u));
}
int vmany(int n, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; i++)
    s = s * 131 + __builtin_va_arg(ap, int);
  __builtin_va_end(ap);
  return s;
}
)C";

// Doubles, and longs between doubles: both read the vector half.
constexpr char kFloatingKernel[] = R"C(
double vsumd(int n, ...) __attribute__((noinline));
long vmix(int n, ...) __attribute__((noinline));
int vsumd_e(int x) { return (int)vsumd(3, (double)x, x + 1.0, x * 2.0); }
long vmix_e(int x) { return vmix(4, (long)x, x * 0.5, (long)(x + 1), x * 0.25); }
double vsumd(int n, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, n);
  double s = 0;
  for (int i = 0; i < n; i++)
    s = s * 3 + __builtin_va_arg(ap, double);
  __builtin_va_end(ap);
  return s;
}
long vmix(int n, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, n);
  long s = 0;
  for (int i = 0; i < n; i++)
    s = s * 7 + ((i & 1) ? (long)(__builtin_va_arg(ap, double) * 8)
                         : __builtin_va_arg(ap, long));
  __builtin_va_end(ap);
  return s;
}
)C";

std::string readText(const fs::path &Path) {
  std::ifstream Input(Path);
  return std::string(std::istreambuf_iterator<char>(Input), {});
}

} // namespace

class AArch64_VariadicSaveArea : public NeverDLiftTest {
protected:
  /// \p Kernel compiled for \p Target at -O2 as an object.
  fs::path compile(const std::string &Target, const std::string &Kernel,
                   const std::string &Name) {
    const auto Source = tmpFile(Name + ".c");
    const auto Object = tmpFile(Name + ".o");
    std::ofstream(Source) << Kernel;
    const auto Compiled = exec(
        NEVERD_TEST_CLANG, {"-target", Target, "-O2", "-fno-stack-protector",
                            "-fno-asynchronous-unwind-tables", "-c",
                            Source.string(), "-o", Object.string()});
    EXPECT_EQ(Compiled.exitCode, 0) << Compiled.err;
    return Object;
  }

  /// The MedIR of function \p Name (with or without Mach-O's underscore).
  std::string medFunction(const fs::path &Object, const std::string &Name) {
    const auto Lifted = exec(ndBin(), {"lift", "-dump-med", Object.string()});
    EXPECT_EQ(Lifted.exitCode, 0) << Lifted.err;
    std::smatch Header;
    if (!std::regex_search(Lifted.out, Header,
                           std::regex("func _?" + Name + " @")))
      return {};
    const size_t Begin = static_cast<size_t>(Header.position(0));
    const size_t End = Lifted.out.find("\nfunc ", Begin + 1);
    return Lifted.out.substr(Begin, End == std::string::npos ? std::string::npos
                                                             : End - Begin);
  }

  /// How many entry values of the registers \p Registers matches \p Body
  /// stores: `Q[0-7]` for the vector half of the save area.
  static size_t spills(const std::string &Body, const std::string &Registers) {
    const std::regex Spill("\\s*STORE \\S+ \\S+ " + Registers);
    size_t Count = 0;
    std::istringstream Lines(Body);
    for (std::string Line; std::getline(Lines, Line);)
      Count += std::regex_match(Line, Spill);
    return Count;
  }

  /// Run \p C, the C of \p Kernel, against \p Kernel itself, renamed, at
  /// each of \p Values of \p Entry, which takes and returns an int.  As in
  /// HighCHostExecution.h, C that keeps an original AArch64 memory
  /// instruction runs only on an AArch64 host.
  void expectRunsLikeKernel(const std::string &C, const std::string &Kernel,
                            const std::string &Entry,
                            std::initializer_list<const char *> Names,
                            std::initializer_list<int> Values) {
#if !defined(__aarch64__)
    if (C.find(neverd::kAddressMemoryLoadCAsm) != std::string::npos ||
        C.find(neverd::kAddressMemoryStoreCAsm) != std::string::npos)
      GTEST_SKIP() << "the C retains original AArch64 memory instructions; "
                      "executing it requires an AArch64 host";
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
    const auto ReferenceSource = tmpFile("reference.c");
    const auto ReferenceObject = tmpFile("reference.o");
    const auto HostSource = tmpFile("host.c");
    const auto Program = tmpFile("host.exe");
    std::ofstream(ReferenceSource) << Reference;
    std::ofstream(HostSource) << C << "\nextern int ref_" << Entry << "(int);\n"
                              << Main;
    const auto ReferenceBuilt =
        exec(NEVERD_TEST_CLANG,
             {"-std=gnu11", "-O0", "-fno-strict-aliasing", "-c",
              ReferenceSource.string(), "-o", ReferenceObject.string()});
    ASSERT_EQ(ReferenceBuilt.exitCode, 0) << ReferenceBuilt.err;
    const auto Built =
        exec(NEVERD_TEST_CLANG,
             {"-std=gnu11", "-O2", "-fno-strict-aliasing", HostSource.string(),
              ReferenceObject.string(), "-o", Program.string()});
    ASSERT_EQ(Built.exitCode, 0) << Built.err << "\n" << C;
    EXPECT_EQ(exec(Program.string(), {}).exitCode, 0) << C;
  }
};

// The walk reads ints from __gr_top and __stack only, never __vr_top or
// __vr_offs: q0-q7 are saved where nothing reads them.  Neither C route
// writes them, so neither traps on registers no caller set, and the LLVM C
// runs like the kernel, as the HighC does (AArch64_HighCExecution).
TEST_F(AArch64_VariadicSaveArea, AnIntegerWalkLeavesTheVectorHalfUnread) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const auto Object = compile("aarch64-linux-gnu", kIntegerKernel, "ints");
  const std::string Body = medFunction(Object, "vmany");
  ASSERT_FALSE(Body.empty());
  EXPECT_EQ(spills(Body, "Q[0-7]"), 0u) << Body;
  EXPECT_EQ(spills(Body, "X[1-7]"), 7u) << Body;

  const auto High = decompileToHighC(Object);
  ASSERT_EQ(High.exitCode, 0) << High.err;
  const std::string HighC = readText(tmpFile("decompiled_high.c"));
  EXPECT_EQ(HighC.find("unknown"), std::string::npos) << HighC;

  const auto Low = decompileToC(Object);
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string LLVMC = readText(tmpFile("decompiled.c"));
  ASSERT_EQ(LLVMC.find("unknown"), std::string::npos) << LLVMC;
  expectRunsLikeKernel(LLVMC, kIntegerKernel, "vmany_e", {"vmany_e", "vmany"},
                       {0, 1, -1, 0x15, 0x27, 0x3F, 0x12345678});
}

// va_arg of a double reads __vr_offs and __vr_top, and then the saved
// vector registers: they stay, and the C still fails explicitly where it
// stores a value no caller is known to set rather than store zero.
TEST_F(AArch64_VariadicSaveArea, AFloatingWalkKeepsTheVectorHalf) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const auto Object = compile("aarch64-linux-gnu", kFloatingKernel, "floats");
  for (const char *Name : {"vsumd", "vmix"}) {
    SCOPED_TRACE(Name);
    const std::string Body = medFunction(Object, Name);
    ASSERT_FALSE(Body.empty());
    EXPECT_EQ(spills(Body, "Q[0-7]"), 8u) << Body;
  }
  const auto High = decompileToHighC(Object);
  ASSERT_EQ(High.exitCode, 0) << High.err;
  EXPECT_NE(readText(tmpFile("decompiled_high.c")).find("unknown register"),
            std::string::npos);
}

// Windows ARM64 passes floating varargs in integer registers and Apple
// arm64 every vararg on the stack: neither saves a vector register for
// va_arg, so there is no vector half to leave out and no floating
// parameter to take.
TEST_F(AArch64_VariadicSaveArea, WindowsAndAppleVariadicsSaveNoVectorHalf) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  for (const char *Target : {"aarch64-pc-windows-msvc", "arm64-apple-macos"}) {
    SCOPED_TRACE(Target);
    const auto Object = compile(Target, kFloatingKernel, "abi");
    const std::string Body = medFunction(Object, "vmix");
    ASSERT_FALSE(Body.empty());
    EXPECT_EQ(spills(Body, "Q[0-7]"), 0u) << Body;
    const auto High = decompileToHighC(Object);
    ASSERT_EQ(High.exitCode, 0) << High.err;
    const std::string C = readText(tmpFile("decompiled_high.c"));
    std::smatch Prototype;
    ASSERT_TRUE(
        std::regex_search(C, Prototype, std::regex(R"(\bvmix\(([^)]*)\) \{)")))
        << C;
    const std::string Parameters = Prototype[1].str();
    for (const char *Floating : {"double", "float", "__int128", "vector"})
      EXPECT_EQ(Parameters.find(Floating), std::string::npos) << Parameters;
    EXPECT_EQ(C.find("unknown"), std::string::npos) << C;
  }
}
