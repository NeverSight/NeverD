//===- SourcePatchCAPITests.cpp - Source replacement behavior tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/object/MachOLayout.h"
#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

TEST(SourcePatchTrampoline, RejectsOutOfRangeWithoutTruncatingTheDestination) {
  const auto X64 = neverd::getTargetCodegenInfo(neverd::Arch::X64);
  const uint64_t From = 0x200000000;
  const uint64_t Next = From + 5;
  for (uint64_t Target : {Next + 0x80000000, Next - 0x80000001}) {
    std::vector<uint8_t> Bytes(5, 0xcc);
    const auto Before = Bytes;
    EXPECT_FALSE(X64.writeTrampoline(Bytes, 0, Target, From));
    EXPECT_EQ(Bytes, Before);
  }
  for (uint64_t Target : {Next + 0x7fffffff, Next - 0x80000000}) {
    std::vector<uint8_t> Bytes(5);
    ASSERT_TRUE(X64.writeTrampoline(Bytes, 0, Target, From));
    const int32_t Displacement = neverd::readLE<int32_t>(Bytes.data() + 1);
    EXPECT_EQ(Next + Displacement, Target);
  }
  const auto ARM64 = neverd::getTargetCodegenInfo(neverd::Arch::AArch64);
  for (uint64_t Target :
       {uint64_t(0x1001), uint64_t(0x8001000), UINT64_MAX - 3}) {
    std::vector<uint8_t> Bytes(4, 0xcc);
    const auto Before = Bytes;
    EXPECT_FALSE(ARM64.writeTrampoline(Bytes, 0, Target, 0x1000));
    EXPECT_EQ(Bytes, Before);
  }
}

std::string takeString(const char *Value) {
  std::string Text = Value ? Value : "";
  neverd_free_string(Value);
  return Text;
}

std::string readFile(const std::filesystem::path &Path) {
  std::ifstream Stream(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(Stream), {}};
}

int execute(const std::vector<std::string> &Arguments,
            std::optional<llvm::StringRef> Stdout = std::nullopt) {
  std::vector<llvm::StringRef> Args(Arguments.begin(), Arguments.end());
  const std::array<std::optional<llvm::StringRef>, 3> Redirects = {
      std::nullopt, Stdout, std::nullopt};
  return llvm::sys::ExecuteAndWait(Args.front(), Args, std::nullopt, Redirects,
                                   30);
}

class SourcePatch : public testing::Test {
protected:
  std::filesystem::path Directory, Original, Output;
  neverd_session_t Session = nullptr;
  neverd_va_t First = 0, Second = 0;
  neverd_va_t Counter = 0, Constant = 0;
  neverd_source_patch_options_v1 Options{};
  neverd_source_patch_mapping_v1 Mapping{};
  bool ReserveStorageHeader = false;
  static constexpr const char *Replacement =
      "int replacement(int x) { return x + 40; }";

  void SetUp() override {
#if !((defined(__APPLE__) && (defined(__aarch64__) || defined(__x86_64__))) || \
      (defined(__linux__) && defined(__x86_64__)))
    GTEST_SKIP() << "native source runtime profile requires Darwin ARM64/x64 "
                    "or Linux x64";
#endif
    if (std::string(NEVERD_TEST_CLANG).empty())
      GTEST_SKIP() << "Clang is required for native replacement fixtures";
    llvm::SmallString<256> Path;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd source patch", Path));
    Directory = Path.str().str();
    Original = Directory / "original";
    Output = Directory / "patched";
    const auto Source = Directory / "original.c";
    std::ofstream(Source) << R"(
extern int puts(const char *);
int original_counter = 5;
const int original_constant = 7;
__attribute__((noinline)) int target_one(int x) { volatile int y = x; return y + 1; }
__attribute__((noinline)) int target_two(int x) { volatile int y = x; return y + 2; }
int main(int argc, char **argv) {
  if (argc > 1 && argv[1][0] == 'p') return puts("original") < 0;
  if (argc > 1 && argv[1][0] == 's') {
    int (*volatile f)(int) = target_one;
    int result = target_one(2) + f(2);
    return result + original_counter;
  }
  if (argc > 1 && argv[1][0] == '2') {
    int (*volatile f)(int) = target_two;
    return target_two(2) + f(2);
  }
  int (*volatile f)(int) = target_one;
  return target_one(2) + f(2);
}
)";
    std::vector<std::string> Compile{NEVERD_TEST_CLANG, "-O0", Source.string(),
                                     "-o", Original.string()};
#if defined(__APPLE__)
    bool IntelFixture = false;
#if defined(__x86_64__)
    IntelFixture = true;
#endif
    if (const char *Architecture = std::getenv("NEVERD_TEST_DARWIN_ARCH")) {
      ASSERT_EQ(std::string(Architecture), "x86_64");
      Compile.insert(Compile.end(), {"-arch", Architecture});
      IntelFixture = true;
    }
    if (IntelFixture)
      Compile.push_back("-Wl,-segalign,0x4000");
    if (ReserveStorageHeader)
      Compile.push_back("-Wl,-headerpad,0x400");
#endif
    ASSERT_EQ(execute(Compile), 0);
    ASSERT_EQ(execute({Original.string()}), 6);
    ASSERT_EQ(execute({Original.string(), "2"}), 8);
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
    ASSERT_EQ(neverd_session_load(Session, Original.string().c_str()), 1)
        << error();
    for (int I = 0; I < neverd_func_count(Session); ++I) {
      const std::string Name = takeString(neverd_func_name(Session, I));
      if (Name == "target_one" || Name == "_target_one")
        First = neverd_func_entry(Session, I);
      if (Name == "target_two" || Name == "_target_two")
        Second = neverd_func_entry(Session, I);
    }
    ASSERT_NE(First, 0u);
    ASSERT_NE(Second, 0u);
    auto Object = llvm::object::ObjectFile::createObjectFile(Original.string());
    ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
    for (const auto &Symbol : Object->getBinary()->symbols()) {
      auto Name = Symbol.getName();
      auto Address = Symbol.getAddress();
      ASSERT_TRUE(bool(Name)) << llvm::toString(Name.takeError());
      ASSERT_TRUE(bool(Address)) << llvm::toString(Address.takeError());
      if (*Name == "original_counter" || *Name == "_original_counter")
        Counter = *Address;
      if (*Name == "original_constant" || *Name == "_original_constant")
        Constant = *Address;
    }
    ASSERT_NE(Counter, 0u);
    ASSERT_NE(Constant, 0u);
    Options.struct_size = sizeof(Options);
    Options.compiler_path = NEVERD_TEST_CLANG;
    Mapping = {"replacement", First};
    Options.mappings = &Mapping;
    Options.mapping_count = 1;
  }

  void TearDown() override {
    neverd_session_destroy(Session);
    if (!Directory.empty()) {
      std::error_code Ignored;
      std::filesystem::remove_all(Directory, Ignored);
    }
  }

  std::string error() { return takeString(neverd_last_error(Session)); }

  int patch(llvm::StringRef Source = Replacement) {
    return neverd_patch_source(Session, Source.data(), Source.size(), &Options,
                               Output.string().c_str());
  }

  void expectReceipt(neverd_va_t Address) {
    auto Report =
        llvm::json::parse(takeString(neverd_patch_source_report(Session)));
    ASSERT_TRUE(static_cast<bool>(Report));
    const auto *Object = Report->getAsObject();
    ASSERT_NE(Object, nullptr);
    EXPECT_EQ(Object->getBoolean("success"), true);
    EXPECT_TRUE(Object->getString("compiler_path").has_value());
    const auto *Installed = Object->getArray("installed");
    ASSERT_NE(Installed, nullptr);
    ASSERT_EQ(Installed->size(), 1u);
    const auto *Entry = Installed->front().getAsObject();
    ASSERT_NE(Entry, nullptr);
    EXPECT_EQ(Entry->getString("symbol"), "replacement");
    EXPECT_EQ(Entry->getInteger("original_va"), Address);
    ASSERT_TRUE(Entry->getInteger("replacement_va").has_value());
    EXPECT_NE(*Entry->getInteger("replacement_va"), Address);
    EXPECT_EQ(neverd_patch_trampoline_count(Session), 1);
  }
};

TEST_F(SourcePatch,
       ExplicitAddressChangesDirectAndIndirectCallsOnlyForSelectedFunction) {
  const std::string Before = readFile(Original);
  ASSERT_EQ(patch(), 1) << error();
  expectReceipt(First);
  EXPECT_EQ(execute({Output.string()}), 84);
  EXPECT_EQ(execute({Output.string(), "2"}), 8);
  EXPECT_EQ(readFile(Original), Before);
  Mapping.original_va = Second;
  Output = Directory / "second patch";
  ASSERT_EQ(patch(), 1) << error();
  expectReceipt(Second);
  EXPECT_EQ(execute({Output.string()}), 6);
  EXPECT_EQ(execute({Output.string(), "2"}), 84);
}

TEST_F(SourcePatch, LegacyCAPIUsesTheProvidedAddress) {
#ifndef _WIN32
  // The legacy entry has no executable option. Exercise its documented PATH
  // lookup using the same explicitly configured compiler as the other cases.
  const char *OldPathValue = std::getenv("PATH");
  const std::string OldPath = OldPathValue ? OldPathValue : "";
  const bool HadPath = OldPathValue != nullptr;
  const std::string SearchPath =
      std::filesystem::path(NEVERD_TEST_CLANG).parent_path().string() + ":" +
      OldPath;
  ASSERT_EQ(setenv("PATH", SearchPath.c_str(), 1), 0);
  auto RestorePath = llvm::scope_exit([&] {
    if (HadPath)
      setenv("PATH", OldPath.c_str(), 1);
    else
      unsetenv("PATH");
  });
#endif
  ASSERT_EQ(neverd_patch_from_c(Session, Replacement, Second,
                                Output.string().c_str()),
            1)
      << error();
  expectReceipt(Second);
  EXPECT_EQ(execute({Output.string()}), 6);
  EXPECT_EQ(execute({Output.string(), "2"}), 84);
}

TEST_F(SourcePatch, NoMatchingTargetCannotReportSuccess) {
  Options.mapping_count = 0;
  EXPECT_EQ(patch(), 0);
  EXPECT_NE(error().find("no replacement targets"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
  EXPECT_EQ(neverd_patch_trampoline_count(Session), 0);
}

TEST_F(SourcePatch, ExistingDestinationRemainsUnchanged) {
  const std::string Before = readFile(Original);
  Output = Original;
  EXPECT_EQ(patch(), 0);
  EXPECT_NE(error().find("output already exists"), std::string::npos);
  EXPECT_EQ(readFile(Original), Before);
}

TEST_F(SourcePatch, MissingEntryAndInteriorAddressAreRejected) {
  Mapping.symbol = "missing";
  EXPECT_EQ(patch(), 0);
  EXPECT_NE(error().find("definition not found"), std::string::npos);
  Mapping.symbol = "replacement";
  Mapping.original_va = First + 1;
  EXPECT_EQ(patch(), 0);
  EXPECT_NE(error().find("authenticated function entry"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatch, MultipleExternalDefinitionsNeedAnExplicitEntry) {
  constexpr const char *Source =
      "int helper(int x) { return x; } int replacement(int x) { return x+40; }";
  Mapping.symbol = nullptr;
  EXPECT_EQ(patch(Source), 0);
  EXPECT_NE(error().find("multiple replacement definitions"),
            std::string::npos);
  Mapping.symbol = "replacement";
  ASSERT_EQ(patch(Source), 1) << error();
  EXPECT_EQ(execute({Output.string()}), 84);
  EXPECT_EQ(execute({Output.string(), "2"}), 8);
}

TEST_F(SourcePatch, InvalidSecondMappingDoesNotPublishAPartialPatch) {
  neverd_source_patch_mapping_v1 Mappings[] = {{"replacement", First},
                                               {"another", Second + 1}};
  Options.mappings = Mappings;
  Options.mapping_count = 2;
  EXPECT_EQ(patch("int replacement(int x){return x+40;} int another(int "
                  "x){return x+50;}"),
            0);
  EXPECT_FALSE(std::filesystem::exists(Output));
  EXPECT_EQ(execute({Original.string()}), 6);
}

TEST_F(SourcePatch, ReplacementCanCallANewHelper) {
  Mapping.symbol = nullptr;
  ASSERT_EQ(patch("__attribute__((noinline)) static int helper(int x) {"
                  "volatile int y=x; return y+40;}"
                  "int replacement(int x) {return helper(x)+1;}"),
            1)
      << error();
  EXPECT_EQ(execute({Output.string()}), 86);
  EXPECT_EQ(execute({Output.string(), "2"}), 8);
  expectReceipt(First);
}

TEST_F(SourcePatch, UnknownExternalCannotPublishAnUnresolvedBranch) {
  EXPECT_EQ(
      patch("extern int absent_patch_function(int); "
            "int replacement(int x) { return absent_patch_function(x); }"),
      0);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatch, ExplicitFunctionBindingCallsThePreservedOriginal) {
  const neverd_source_patch_binding_v1 Binding{
      "preserved", NEVERD_SOURCE_BIND_FUNCTION, Second, 0};
  Options.bindings = &Binding;
  Options.binding_count = 1;
  ASSERT_EQ(patch("extern int preserved(int); "
                  "int replacement(int x) { return preserved(x) + 40; }"),
            1)
      << error();
  EXPECT_EQ(execute({Output.string()}), 88);
  EXPECT_EQ(execute({Output.string(), "2"}), 8);
}

TEST_F(SourcePatch, ExplicitDataBindingSharesStateWithTheOriginalProgram) {
  const neverd_source_patch_binding_v1 Binding{
      "counter", NEVERD_SOURCE_BIND_WRITABLE_DATA, Counter, 0};
  Options.bindings = &Binding;
  Options.binding_count = 1;
  ASSERT_EQ(patch("extern int counter; "
                  "int replacement(int x) { return counter++ + x; }"),
            1)
      << error();
  EXPECT_EQ(execute({Original.string(), "s"}), 11);
  EXPECT_EQ(execute({Output.string(), "s"}), 22);
  EXPECT_EQ(execute({Output.string(), "2"}), 8);
}

TEST_F(SourcePatch, ExplicitConstantBindingReadsOriginalReadOnlyStorage) {
  const neverd_source_patch_binding_v1 Binding{
      "constant", NEVERD_SOURCE_BIND_READ_ONLY_DATA, Constant, 0};
  Options.bindings = &Binding;
  Options.binding_count = 1;
  ASSERT_EQ(patch("extern const int constant; "
                  "int replacement(int x) { return constant + x; }"),
            1)
      << error();
  EXPECT_EQ(execute({Output.string()}), 18);
}

class SourcePatchWithStorage : public SourcePatch {
  void SetUp() override {
    ReserveStorageHeader = true;
    SourcePatch::SetUp();
  }
};

TEST_F(SourcePatchWithStorage,
       NewStatePreservesInitializationAndUpdatesAcrossCalls) {
  ASSERT_EQ(patch("static volatile int hits; static volatile int state=40; "
                  "int replacement(int x) { return ++hits + state++ + x; }"),
            1)
      << error();
  EXPECT_EQ(execute({Output.string()}), 88);
  EXPECT_EQ(execute({Output.string(), "2"}), 8);
  EXPECT_EQ(execute({Output.string()}), 88);
#if defined(__APPLE__)
  const std::string Bytes = readFile(Output);
  unsigned WritableSegments = 0;
  neverd::forEachMachOLoadCommand(
      reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size(),
      [&](const uint8_t *Command, uint32_t ID, uint32_t, bool Is64) {
        if (ID != neverd::getMachOSegmentCmdID(Is64))
          return;
        const auto Segment = neverd::readMachOSegment(Command, Is64);
        if (neverd::readMachOName(Segment.SegName) == "__ND_DATA") {
          ++WritableSegments;
          EXPECT_EQ(Segment.InitProt,
                    llvm::MachO::VM_PROT_READ | llvm::MachO::VM_PROT_WRITE);
          EXPECT_EQ(Segment.MaxProt, Segment.InitProt);
        }
      });
  EXPECT_EQ(WritableSegments, 1u);
#endif
}

TEST_F(SourcePatch, ExistingImportedFunctionCanReadNewConstantStorage) {
  ASSERT_EQ(patch("extern int puts(const char *); "
                  "int replacement(int x) { return (puts(\"replacement\") < 0) "
                  "+ x + 40; }"),
            1)
      << error();
  EXPECT_EQ(execute({Output.string()}), 84);
  EXPECT_EQ(execute({Output.string(), "2"}), 8);
}

TEST_F(SourcePatch, BindingCannotPromiseTheReplacedOriginalBody) {
  const neverd_source_patch_binding_v1 Binding{
      "previous", NEVERD_SOURCE_BIND_FUNCTION, First, 0};
  Options.bindings = &Binding;
  Options.binding_count = 1;
  EXPECT_EQ(patch("extern int previous(int); "
                  "int replacement(int x) { return previous(x); }"),
            0);
  EXPECT_NE(error().find("original-body callbacks"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatch, CompilerSelectionNeverFallsBack) {
  const std::string Missing = (Directory / "missing-compiler").string();
  Options.compiler_path = Missing.c_str();
  EXPECT_EQ(patch(), 0);
  EXPECT_NE(error().find("selected executable not found"), std::string::npos);
  Options.compiler_path = NEVERD_TEST_CLANG;
  Options.compiler = NEVERD_SOURCE_PATCH_NEVERC;
  EXPECT_EQ(patch(), 0);
  EXPECT_NE(error().find("identity does not match"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatch, CompilerCannotOverrideTheTarget) {
  const char *Arguments[] = {"--target=unrelated"};
  Options.compiler_args = Arguments;
  Options.compiler_arg_count = 1;
  EXPECT_EQ(patch(), 0);
  EXPECT_NE(error().find("unsupported source option"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatch, CompilerResponseFilesCannotBypassOptionValidation) {
  const auto Response = Directory / "extra arguments.rsp";
  std::ofstream(Response) << "unused-directory -DBASE=37\n";
  const std::string ResponseArgument = "@" + Response.string();
  // Drivers expand response tokens before parsing an option's value. With -I,
  // this file would inject BASE and turn an otherwise invalid source into a
  // successfully published patch.
  for (const char *Option :
       {"-I", "-isystem", "-iquote", "-include", "-D", "-U", "-isysroot", ""}) {
    SCOPED_TRACE(Option);
    const char *Arguments[] = {Option, ResponseArgument.c_str()};
    Options.compiler_args = *Option ? Arguments : Arguments + 1;
    Options.compiler_arg_count = *Option ? 2 : 1;
    ASSERT_EQ(patch("int replacement(int x){return x+BASE;}"), 0) << error();
    EXPECT_NE(error().find("response files"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(Output));
  }

  // The same supported options keep their ordinary values, including spaces.
  const auto Include = Directory / "include path";
  ASSERT_TRUE(std::filesystem::create_directory(Include));
  std::ofstream(Include / "shared.h") << "#define TOTAL (BASE + 3)\n";
  const std::string IncludeArgument = Include.string();
  const char *Arguments[] = {"-I", IncludeArgument.c_str(), "-D", "BASE=37"};
  Options.compiler_args = Arguments;
  Options.compiler_arg_count = 4;
  ASSERT_EQ(patch("#include \"shared.h\"\n"
                  "int replacement(int x){return x+TOTAL;}"),
            1)
      << error();
  EXPECT_EQ(execute({Output.string()}), 84);
}

TEST_F(SourcePatch, IncludesMacrosAndSourcePathsArePreserved) {
  const auto SourcePath = Directory / "replacement file.c";
  const std::string SourceName = SourcePath.string();
  std::ofstream(Directory / "shared.h") << "#define TOTAL (BASE + 3)\n";
  const char *Arguments[] = {"-DBASE=37"};
  Options.compiler_args = Arguments;
  Options.compiler_arg_count = 1;
  Options.source_name = SourceName.c_str();
  ASSERT_EQ(
      patch("#include \"shared.h\"\nint replacement(int x){return x+TOTAL;}"),
      1)
      << error();
  EXPECT_EQ(execute({Output.string()}), 84);
}

TEST_F(SourcePatch, CompilerDiagnosticsReachTheCaller) {
  EXPECT_EQ(patch("int replacement(int x) { this is not C; }"), 0);
  EXPECT_NE(error().find("error:"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatch, CompilerTimeoutDoesNotPublishAnOutput) {
  const auto Compiler = Directory / "slow clang";
  std::ofstream(Compiler) << "#!/bin/sh\n"
                             "if [ \"$1\" = --version ]; then\n"
                             "  echo 'clang version fixture'\n"
                             "else\n"
                             "  exec sleep 10\n"
                             "fi\n";
  std::filesystem::permissions(Compiler, std::filesystem::perms::owner_exec,
                               std::filesystem::perm_options::add);
  const std::string CompilerPath = Compiler.string();
  Options.compiler_path = CompilerPath.c_str();
  Options.timeout_seconds = 1;
  EXPECT_EQ(patch(), 0);
  EXPECT_FALSE(error().empty());
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatch, ForeignLLVMIRCannotBeInstalledIntoTheLoadedTarget) {
  const auto Source = Directory / "replacement.c";
  const auto IR = Directory / "foreign.ll";
  std::ofstream(Source) << Replacement;
#if defined(__APPLE__)
  const char *Target = "--target=x86_64-unknown-linux-gnu";
#else
  const char *Target = "--target=aarch64-unknown-linux-gnu";
#endif
  ASSERT_EQ(execute({NEVERD_TEST_CLANG, Target, "-emit-llvm", "-S",
                     Source.string(), "-o", IR.string()}),
            0);
  Options.input_kind = NEVERD_SOURCE_PATCH_LLVM_IR;
  EXPECT_EQ(patch(readFile(IR)), 0);
  EXPECT_NE(error().find("target does not match"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatch, UnimplementedInstallationStrategyIsNotIgnored) {
  Options.strategy = 1;
  EXPECT_EQ(patch(), 0);
  EXPECT_NE(error().find("requires section"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatch, NeverCNormalModeExecutesWhenAvailable) {
  const char *NeverC = std::getenv("NEVERD_TEST_NEVERC");
  if (!NeverC || !*NeverC)
    GTEST_SKIP()
        << "set NEVERD_TEST_NEVERC to test the optional NeverC compiler";
  Options.compiler = NEVERD_SOURCE_PATCH_NEVERC;
  Options.compiler_path = NeverC;
  ASSERT_EQ(patch(), 1) << error();
  expectReceipt(First);
  EXPECT_EQ(execute({Output.string()}), 84);
  EXPECT_EQ(execute({Output.string(), "2"}), 8);
  const std::string Report = takeString(neverd_patch_source_report(Session));
  EXPECT_EQ(Report.find("-dyncode"), std::string::npos);
}

TEST_F(SourcePatch, CXXEntryUsesTheExactSymbol) {
  Options.language = NEVERD_SOURCE_PATCH_CXX;
  Mapping.symbol = "_Z11replacementi";
  ASSERT_EQ(patch("int replacement(int x) { return x+40; }"), 1) << error();
  EXPECT_EQ(execute({Output.string()}), 84);
  EXPECT_EQ(execute({Output.string(), "2"}), 8);
}

TEST_F(SourcePatch, NeverCRejectsCppBeforeStartingTheCompiler) {
  Options.compiler = NEVERD_SOURCE_PATCH_NEVERC;
  Options.compiler_path = "neverd-missing-neverc-must-not-be-started";
  Options.language = NEVERD_SOURCE_PATCH_CXX;
  for (const auto [Artifact, Mode] :
       {std::pair{NEVERD_SOURCE_PATCH_ARTIFACT_IR, NEVERD_SOURCE_PATCH_NORMAL},
        std::pair{NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT,
                  NEVERD_SOURCE_PATCH_NORMAL},
        std::pair{NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT,
                  NEVERD_SOURCE_PATCH_DYNCODE}}) {
    Options.artifact = Artifact;
    Options.mode = Mode;
    EXPECT_EQ(patch("extern \"C\" int replacement(int x) { return x + 40; }"),
              0);
    EXPECT_NE(error().find("NeverC accepts C source only"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(Output));
  }
}

TEST_F(SourcePatch, CLIHonorsEntryAddressAndRejectsInvalidAddress) {
  const auto Source = Directory / "replacement.c";
  std::ofstream(Source) << Replacement;
  // Use a relative output path to cover same-filesystem staging independently
  // of the process's temporary directory.
  const auto RelativeOutput = std::filesystem::relative(Output).string();
  const auto ReportPath = (Directory / "report.json").string();
  const std::vector<std::string> Prefix = {
      NEVERD_TEST_CLI,    "patch",
      Original.string(),  "--from-source",
      Source.string(),    "--entry=replacement",
      "--compiler=clang", "--compiler-path=" NEVERD_TEST_CLANG,
      "--patch-report",   "-o",
      RelativeOutput};
  auto Arguments = Prefix;
  Arguments.push_back("--func=invalid");
  EXPECT_NE(execute(Arguments), 0);
  EXPECT_FALSE(std::filesystem::exists(Output));
  Arguments = Prefix;
  Arguments.push_back("--func=" + std::to_string(Second));
  ASSERT_EQ(execute(Arguments, ReportPath), 0);
  auto Report = llvm::json::parse(readFile(ReportPath));
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  ASSERT_NE(Report->getAsObject(), nullptr);
  EXPECT_EQ(Report->getAsObject()->getBoolean("success"), true);
  EXPECT_EQ(execute({Output.string()}), 6);
  EXPECT_EQ(execute({Output.string(), "2"}), 84);
}

TEST_F(SourcePatch, CLIAdmitsTypedOriginalDataBindings) {
  const auto Source = Directory / "replacement.c";
  std::ofstream(Source) << "extern int counter; "
                           "int replacement(int x) { return counter++ + x; }";
  std::vector<std::string> Arguments = {NEVERD_TEST_CLI,
                                        "patch",
                                        Original.string(),
                                        "--from-source",
                                        Source.string(),
                                        "--entry=replacement",
                                        "--func=" + std::to_string(First),
                                        "--compiler-path=" NEVERD_TEST_CLANG,
                                        "-o",
                                        Output.string(),
                                        "--bind=data:counter=invalid"};
  EXPECT_NE(execute(Arguments), 0);
  EXPECT_FALSE(std::filesystem::exists(Output));
  Arguments.back() = "--bind=data:counter=" + std::to_string(Counter) + ":4";
  ASSERT_EQ(execute(Arguments), 0);
  EXPECT_EQ(execute({Output.string(), "s"}), 22);
}

} // namespace
