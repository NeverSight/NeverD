//===- SourcePatchExceptionTests.cpp - OS exception runtime contracts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
int executeEH(const std::vector<std::string> &Arguments) {
  std::vector<llvm::StringRef> Args(Arguments.begin(), Arguments.end());
  return llvm::sys::ExecuteAndWait(Args.front(), Args, std::nullopt, {}, 30);
}
std::string takeEH(const char *Value) {
  std::string Result = Value ? Value : "";
  neverd_free_string(Value);
  return Result;
}

class SourcePatchException : public testing::Test {
protected:
  std::filesystem::path Directory, Original, Output;
  neverd_session_t Session = nullptr;
  neverd_source_patch_options_v1 Options{};
  neverd_source_patch_mapping_v1 Mapping{};
  neverd_source_patch_binding_v1 Binding{};
  const char *Arguments[1] = {"-O0"};

  void SetUp() override {
#if !((defined(__APPLE__) && (defined(__aarch64__) || defined(__x86_64__))) || \
      (defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))))
    GTEST_SKIP()
        << "native exception runtime profile requires Darwin ARM64/x64 or "
           "Linux ARM64/x64";
#endif
    if (std::string(NEVERD_TEST_CLANG).empty())
      GTEST_SKIP() << "a compatible Clang is required";
    llvm::SmallString<256> Temp;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-source-eh", Temp));
    Directory = Temp.str().str();
    Original = Directory / "original";
    Output = Directory / "patched";
    const auto Source = Directory / "original.cpp";
    std::ofstream(Source) << R"(
extern "C" { int cleanup_count = 0; void _Exit(int); }
struct Guard { ~Guard() { ++cleanup_count; } };
extern "C" __attribute__((noinline)) int original_thrower(int x) { if (x) throw x; return x; }
// The original call site must allow an exception; otherwise the compiler is
// entitled to erase its catch/cleanup path, which a replacement cannot restore.
extern "C" __attribute__((noinline)) int target(int x) {
  volatile int y=x; if (y == -999) throw y; return y+1;
}
__attribute__((noinline)) int reraiser(int x) {
  try { return original_thrower(x); } catch (...) { Guard g; throw; }
}
namespace std {
  using terminate_handler = void (*)();
  terminate_handler set_terminate(terminate_handler) noexcept;
  void terminate() noexcept;
}
void on_terminate() { _Exit(77); }
int main(int argc, char **argv) {
  std::set_terminate(on_terminate);
  int x=argc>1 ? argv[1][0]-'0' : 0;
  if (x==8) std::terminate();
  try { if (x==9) return reraiser(9); return target(x); }
  catch (int value) { return value + cleanup_count*10; }
}
)";
    std::vector<std::string> Command{
        NEVERD_TEST_CLANG, "--driver-mode=g++", "-O0", Source.string(), "-o",
        Original.string()};
#if defined(__APPLE__)
    bool IntelFixture = false;
#if defined(__x86_64__)
    IntelFixture = true;
#endif
    if (const char *Architecture = std::getenv("NEVERD_TEST_DARWIN_ARCH")) {
      ASSERT_EQ(std::string(Architecture), "x86_64");
      Command.insert(Command.end(), {"-arch", Architecture});
      IntelFixture = true;
    }
    if (IntelFixture) {
      // Header space and unwind-table growth space are separate contracts.
      // Intel's default 4 KiB file alignment cannot hold a new compact page.
      Command.push_back("-Wl,-segalign,0x4000");
    }
    Command.push_back("-Wl,-headerpad,0x1000");
#else
    // Preserve GCC's default stack protection: make the matching runtime
    // import available in this original, instead of disabling it in the patch.
    Command.push_back("-fstack-protector-all");
#endif
    ASSERT_EQ(executeEH(Command), 0);
    ASSERT_EQ(executeEH({Original.string(), "0"}), 1);
    ASSERT_EQ(executeEH({Original.string(), "9"}), 19);
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
    ASSERT_EQ(neverd_session_load(Session, Original.string().c_str()), 1)
        << error();
    for (int I = 0; I < neverd_func_count(Session); ++I) {
      const auto Name = takeEH(neverd_func_name(Session, I));
      if (Name == "target" || Name == "_target")
        Mapping.original_va = neverd_func_entry(Session, I);
    }
    ASSERT_NE(Mapping.original_va, 0u);
    auto Object = llvm::object::ObjectFile::createObjectFile(Original.string());
    ASSERT_TRUE(bool(Object));
    for (const auto &Symbol : Object->getBinary()->symbols()) {
      auto Name = Symbol.getName();
      auto Address = Symbol.getAddress();
      ASSERT_TRUE(bool(Name));
      ASSERT_TRUE(bool(Address));
      if (*Name == "cleanup_count" || *Name == "_cleanup_count")
        Binding.original_va = *Address;
    }
    ASSERT_NE(Binding.original_va, 0u);
    Binding.symbol = "cleanup_count";
    Binding.kind = NEVERD_SOURCE_BIND_WRITABLE_DATA;
    Binding.size = 4;
    Mapping.symbol = "replacement";
    Options.struct_size = sizeof(Options);
    Options.language = NEVERD_SOURCE_PATCH_CXX;
    Options.compiler_path = NEVERD_TEST_CLANG;
    Options.compiler_args = Arguments;
    Options.compiler_arg_count = 1;
    Options.mappings = &Mapping;
    Options.mapping_count = 1;
    Options.bindings = &Binding;
    Options.binding_count = 1;
  }
  void TearDown() override {
    neverd_session_destroy(Session);
    if (!Directory.empty()) {
      std::error_code Ignored;
      std::filesystem::remove_all(Directory, Ignored);
    }
  }
  std::string error() { return takeEH(neverd_last_error(Session)); }
  void check() {
    const llvm::StringRef Source = R"(
extern "C" int cleanup_count;
extern "C" int original_thrower(int);
struct Guard { ~Guard() { ++cleanup_count; } };
__attribute__((noinline)) int broken() noexcept { throw 31; }
extern "C" int replacement(int x) {
  if (x==5) return broken();
  if (x==0) return 40;
  if (x==1) { try { throw 7; } catch(int value) { return value+3; } }
  if (x==2) { Guard g; throw 11; }
  if (x==3) { Guard g; return original_thrower(17); }
  try { throw 21; } catch (...) { Guard g; throw; }
}
)";
    ASSERT_EQ(neverd_patch_source(Session, Source.data(), Source.size(),
                                  &Options, Output.string().c_str()),
              1)
        << error();
    for (const auto &[Mode, Expected] :
         std::vector<std::pair<const char *, int>>{{"0", 40},
                                                   {"1", 10},
                                                   {"2", 21},
                                                   {"3", 27},
                                                   {"4", 31},
                                                   {"5", 77},
                                                   {"9", 19}})
      EXPECT_EQ(executeEH({Output.string(), Mode}), Expected)
          << "mode " << Mode;
  }
};

TEST_F(SourcePatchException, LLVMIRRetainsCrossBoundaryExceptionSemantics) {
#if defined(__linux__) && NEVERD_TEST_PREBUILT_LLVM
  if (!std::getenv("NEVERD_TEST_REQUIRE_FIXED_LLVM"))
    GTEST_SKIP()
        << "ELF cleanup libcalls require the corrected LLVM backend; "
           "set NEVERD_TEST_REQUIRE_FIXED_LLVM for capability validation";
#endif
  check();
}
TEST_F(SourcePatchException,
       NativeObjectRetainsCrossBoundaryExceptionSemantics) {
#if !(defined(__linux__) && defined(__x86_64__))
  GTEST_SKIP() << "native object exception runtime profile requires Linux x64";
#endif
  Options.artifact = NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT;
  check();
}

TEST_F(SourcePatchException, GCCObjectRetainsCrossBoundaryExceptionSemantics) {
#if !(defined(__linux__) && defined(__x86_64__))
  GTEST_SKIP() << "GCC exception runtime profile requires Linux x64";
#endif
  auto GCC = llvm::sys::findProgramByName("g++");
  if (!GCC)
    GTEST_SKIP() << "g++ is unavailable";
  Options.compiler = NEVERD_SOURCE_PATCH_GCC;
  Options.compiler_path = GCC->c_str();
  Options.artifact = NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT;
  check();
}

TEST_F(SourcePatchException, NeverCCSourcesExecuteInNativeProcesses) {
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  if (!Compiler || !*Compiler)
    GTEST_SKIP() << "explicit NeverC producer required";
  Options.compiler = NEVERD_SOURCE_PATCH_NEVERC;
  Options.compiler_path = Compiler;
  Options.language = NEVERD_SOURCE_PATCH_C;
  Options.bindings = nullptr;
  Options.binding_count = 0;
  constexpr llvm::StringLiteral Source = R"(
static int twice(int x) { return x * 2; }
int replacement(int x) {
  return twice(x + 3) + 5;
})";
  for (const auto [Artifact, CompileMode] :
       {std::pair{NEVERD_SOURCE_PATCH_ARTIFACT_IR, NEVERD_SOURCE_PATCH_NORMAL},
        {NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT, NEVERD_SOURCE_PATCH_NORMAL},
        {NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT, NEVERD_SOURCE_PATCH_DYNCODE}}) {
    Options.artifact = Artifact;
    Options.mode = CompileMode;
    Output = Directory / ("neverc-c-" + std::to_string(Artifact) + "-" +
                          std::to_string(CompileMode));
    ASSERT_EQ(neverd_patch_source(Session, Source.data(), Source.size(),
                                  &Options, Output.string().c_str()),
              1)
        << error();
    for (const auto [Mode, Expected] :
         {std::pair{0, 11}, {1, 13}, {2, 15}, {9, 19}})
      EXPECT_EQ(executeEH({Output.string(), std::to_string(Mode)}), Expected);
  }
}

TEST_F(SourcePatchException, NeverCHostedDynCodeUnwindsThroughTwoNativeFrames) {
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  if (!Compiler || !*Compiler)
    GTEST_SKIP() << "explicit NeverC producer required";
  const auto SourceFile = Directory / "callback-original.cpp";
  Original = Directory / "callback-original";
  std::ofstream(SourceFile) << R"(
using Callback = int (*)(int);
extern "C" __attribute__((noinline)) int throwing(int x) {
  if (x) throw x;
  return 0;
}
extern "C" __attribute__((noinline)) int target(Callback cb, int x) {
  volatile int value = cb(x);
  return value + 1;
}
int main(int argc, char **argv) {
  int x = argc > 1 ? argv[1][0] - '0' : 0;
  try { return target(throwing, x); }
  catch (int value) { return value + 40; }
})";
  std::vector<std::string> Command{
      NEVERD_TEST_CLANG, "--driver-mode=g++", "-O0", SourceFile.string(), "-o",
      Original.string()};
#ifdef __APPLE__
  if (const char *Arch = std::getenv("NEVERD_TEST_DARWIN_ARCH"))
    Command.insert(Command.end(), {"-arch", Arch, "-Wl,-segalign,0x4000"});
  Command.push_back("-Wl,-headerpad,0x1000");
#endif
  ASSERT_EQ(executeEH(Command), 0);
  ASSERT_EQ(executeEH({Original.string(), "0"}), 1);
  ASSERT_EQ(executeEH({Original.string(), "2"}), 42);
  ASSERT_EQ(neverd_session_load(Session, Original.string().c_str()), 1)
      << error();
  Mapping.original_va = 0;
  for (int I = 0; I < neverd_func_count(Session); ++I) {
    const auto Name = takeEH(neverd_func_name(Session, I));
    if (Name == "target" || Name == "_target")
      Mapping.original_va = neverd_func_entry(Session, I);
  }
  ASSERT_NE(Mapping.original_va, 0u);
  Options.compiler = NEVERD_SOURCE_PATCH_NEVERC;
  Options.compiler_path = Compiler;
  Options.language = NEVERD_SOURCE_PATCH_C;
  Options.mode = NEVERD_SOURCE_PATCH_DYNCODE;
  Options.artifact = NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT;
  Options.bindings = nullptr;
  Options.binding_count = 0;
  constexpr llvm::StringLiteral Replacement = R"(
typedef int (*Callback)(int);
__attribute__((noinline)) static int inner(Callback cb, int x) {
  volatile int value = cb(x); return value + 1;
}
int replacement(Callback cb, int x) {
  volatile int value = inner(cb, x); return value + 39;
})";
  ASSERT_EQ(neverd_patch_source(Session, Replacement.data(), Replacement.size(),
                                &Options, Output.string().c_str()),
            1)
      << error();
  EXPECT_EQ(executeEH({Output.string(), "0"}), 40);
  EXPECT_EQ(executeEH({Output.string(), "2"}), 42);
}

} // namespace
