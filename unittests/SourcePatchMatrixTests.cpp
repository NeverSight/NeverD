//===- SourcePatchMatrixTests.cpp - Source patch target execution matrix
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Cross-target source compilation, publication and isolated CPU execution.
/// This is not an operating-system loader or exception-runtime test.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/codegen/SourcePatch.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Loader.h"
#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <unicorn/unicorn.h>

namespace {
using namespace neverd;
struct Profile {
  Arch Architecture;
  BinaryFormat Format;
  const char *Name;
  const char *ReceiptTriple;
  const char *FrontendTriple = nullptr;
};
const Profile Profiles[] = {
    {Arch::X86, BinaryFormat::ELF, "ELF_X86", "i386-unknown-linux-gnu"},
    {Arch::X64, BinaryFormat::ELF, "ELF_X64", "x86_64-unknown-linux-gnu"},
    {Arch::ARM, BinaryFormat::ELF, "ELF_ARM32",
     "armv7-unknown-linux-gnueabihf"},
    {Arch::AArch64, BinaryFormat::ELF, "ELF_ARM64",
     "aarch64-unknown-linux-gnu"},
    {Arch::X86, BinaryFormat::COFF, "PE_X86", "i686-pc-windows-msvc"},
    {Arch::X64, BinaryFormat::COFF, "PE_X64", "x86_64-pc-windows-msvc"},
    {Arch::ARM, BinaryFormat::COFF, "PE_ARM32", "thumbv7-pc-windows-msvc"},
    {Arch::AArch64, BinaryFormat::COFF, "PE_ARM64", "aarch64-pc-windows-msvc"},
    {Arch::X86, BinaryFormat::MachO, "MachO_X86", "i386-apple-darwin"},
    {Arch::X64, BinaryFormat::MachO, "MachO_X64", "x86_64-apple-macos14.0"},
    {Arch::ARM, BinaryFormat::MachO, "MachO_ARM32", "thumbv7k-apple-watchos"},
    {Arch::AArch64, BinaryFormat::MachO, "MachO_ARM64",
     "arm64-apple-macos14.0"},
    {Arch::ARM, BinaryFormat::MachO, "MachO_ARMv7", "thumbv7-apple-ios5.0",
     "armv7-apple-ios5.0"},
};
std::string take(const char *Text) {
  std::string Result = Text ? Text : "";
  neverd_free_string(Text);
  return Result;
}
std::string read(const std::filesystem::path &Path) {
  std::ifstream Input(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(Input), {}};
}
// Map image bytes and invoke an ordinary ABI function. The original entry is
// used after installation, so this verifies the entry branch as well as
// codegen.
void execute(const BinaryImage &Image, const Profile &P, int Argument,
             int Expected, uint64_t Slide = 0) {
  uc_engine *UC = nullptr;
  const auto CPU = P.Architecture == Arch::ARM       ? UC_ARCH_ARM
                   : P.Architecture == Arch::AArch64 ? UC_ARCH_ARM64
                                                     : UC_ARCH_X86;
  const auto Mode =
      P.Architecture == Arch::X86   ? UC_MODE_32
      : P.Architecture == Arch::X64 ? UC_MODE_64
      : P.Architecture == Arch::ARM &&
              Image.instructionModeAt(Image.Entry) == InstructionMode::Thumb
          ? UC_MODE_THUMB
          : UC_MODE_ARM;
  ASSERT_EQ(uc_open(CPU, Mode, &UC), UC_ERR_OK);
  auto Close = llvm::scope_exit([&] { uc_close(UC); });
  std::set<uint64_t> Pages;
  for (const auto &Segment : Image.Segments) {
    if (Segment.Data.empty())
      continue;
    ASSERT_LT(Segment.Data.size(), 16u * 1024 * 1024);
    for (uint64_t Page = (Segment.VA + Slide) & ~uint64_t(4095);
         Page < Segment.VA + Slide + Segment.Data.size(); Page += 4096)
      if (Pages.insert(Page).second)
        ASSERT_EQ(uc_mem_map(UC, Page, 4096, UC_PROT_ALL), UC_ERR_OK);
    ASSERT_EQ(uc_mem_write(UC, Segment.VA + Slide, Segment.Data.data(),
                           Segment.Data.size()),
              UC_ERR_OK);
  }
  constexpr uint64_t Stack = 0x70000000, Return = 0x7fff0000;
  ASSERT_EQ(uc_mem_map(UC, Stack, 0x10000, UC_PROT_ALL), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(UC, Return, 4096, UC_PROT_ALL), UC_ERR_OK);
  uint64_t SP = Stack + 0xff00, Arg = uint32_t(Argument), LR = Return;
  int ResultReg;
  switch (P.Architecture) {
  case Arch::X64:
    SP -= 8;
    ASSERT_EQ(uc_mem_write(UC, SP, &LR, 8), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(UC, UC_X86_REG_RSP, &SP), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(UC,
                           P.Format == BinaryFormat::COFF ? UC_X86_REG_RCX
                                                          : UC_X86_REG_RDI,
                           &Arg),
              UC_ERR_OK);
    ResultReg = UC_X86_REG_RAX;
    break;
  case Arch::X86:
    SP -= 4;
    ASSERT_EQ(uc_mem_write(UC, SP, &LR, 4), UC_ERR_OK);
    ASSERT_EQ(uc_mem_write(UC, SP + 4, &Arg, 4), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(UC, UC_X86_REG_ESP, &SP), UC_ERR_OK);
    ResultReg = UC_X86_REG_EAX;
    break;
  case Arch::ARM: {
    uint32_t CPACR = 0;
    ASSERT_EQ(uc_reg_read(UC, UC_ARM_REG_C1_C0_2, &CPACR), UC_ERR_OK);
    CPACR |= 0x00f00000;
    uint32_t FPEXC = 0x40000000;
    ASSERT_EQ(uc_reg_write(UC, UC_ARM_REG_C1_C0_2, &CPACR), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(UC, UC_ARM_REG_FPEXC, &FPEXC), UC_ERR_OK);
  }
    ASSERT_EQ(uc_reg_write(UC, UC_ARM_REG_SP, &SP), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(UC, UC_ARM_REG_LR, &LR), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(UC, UC_ARM_REG_R0, &Arg), UC_ERR_OK);
    ResultReg = UC_ARM_REG_R0;
    break;
  case Arch::AArch64:
    ASSERT_EQ(uc_reg_write(UC, UC_ARM64_REG_SP, &SP), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(UC, UC_ARM64_REG_LR, &LR), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(UC, UC_ARM64_REG_W0, &Arg), UC_ERR_OK);
    ResultReg = UC_ARM64_REG_W0;
    break;
  default:
    llvm_unreachable("matrix architecture");
  }
  const uint64_t Start =
      (Image.Entry + Slide) |
      (P.Architecture == Arch::ARM &&
               Image.instructionModeAt(Image.Entry) == InstructionMode::Thumb
           ? 1
           : 0);
  uint64_t FaultVA = 0;
  uc_hook FaultHook;
  ASSERT_EQ(uc_hook_add(UC, &FaultHook, UC_HOOK_MEM_UNMAPPED,
                        reinterpret_cast<void *>(+[](uc_engine *, uc_mem_type,
                                                     uint64_t VA, int, int64_t,
                                                     void *Context) -> bool {
                          *static_cast<uint64_t *>(Context) = VA;
                          return false;
                        }),
                        &FaultVA, 1, 0),
            UC_ERR_OK);
  ASSERT_EQ(uc_emu_start(UC, Start, Return, 1000000, 10000), UC_ERR_OK)
      << "fault address 0x" << std::hex << FaultVA;
  uint64_t Value = 0, PC = 0;
  ASSERT_EQ(uc_reg_read(UC, ResultReg, &Value), UC_ERR_OK);
  const int PCReg = P.Architecture == Arch::X64   ? UC_X86_REG_RIP
                    : P.Architecture == Arch::X86 ? UC_X86_REG_EIP
                    : P.Architecture == Arch::ARM ? int(UC_ARM_REG_PC)
                                                  : int(UC_ARM64_REG_PC);
  ASSERT_EQ(uc_reg_read(UC, PCReg, &PC), UC_ERR_OK);
  EXPECT_EQ(PC, Return)
      << "must return, not merely exhaust the instruction budget";
  EXPECT_EQ(int32_t(Value), Expected) << "argument " << Argument;
}

int run(const std::vector<std::string> &Arguments) {
  std::vector<llvm::StringRef> Args(Arguments.begin(), Arguments.end());
  return llvm::sys::ExecuteAndWait(Args.front(), Args, std::nullopt, {}, 30);
}
std::string linker(const char *Name) {
  auto Program = llvm::sys::findProgramByName(Name);
  if (Program)
    return *Program;
  const auto Candidate =
      std::filesystem::path(NEVERD_TEST_CLANG).parent_path() / Name;
  return std::filesystem::exists(Candidate) ? Candidate.string() : "";
}

class SourcePatchMatrix : public testing::TestWithParam<Profile> {
protected:
  std::filesystem::path Directory, Original, Output;
  BinaryImage Image;
  std::string OriginalBytes;
  neverd_session_t Session = nullptr;
  neverd_source_patch_options_v1 Options{};
  neverd_source_patch_mapping_v1 Mapping{};
  std::string originalSource() {
    const std::string Export =
        GetParam().Format == BinaryFormat::COFF ? "__declspec(dllexport) " : "";
    return (GetParam().Format == BinaryFormat::COFF ? "int _fltused=0; " : "") +
           Export +
           "__attribute__((noinline)) int target(int x) { volatile int y=x; "
           "return y+1; } " +
           Export +
           "__attribute__((noinline)) int boundary(int x) {return x+2;} " +
           Export +
           "__attribute__((noinline)) double original_fp(float a, double b, "
           "int x, int c, int d, int e, int f, int g, int h, int i) {return "
           "a*2+b+x+c+d+e+f+g+h+i;}";
  }
  void SetUp() override {
    ASSERT_FALSE(std::string(NEVERD_TEST_CLANG).empty());
    llvm::SmallString<256> Temp;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-source-matrix", Temp));
    Directory = Temp.str().str();
    Original = Directory / "original";
    Output = Directory / "patched";
    const auto P = GetParam();
    if (P.Format == BinaryFormat::MachO &&
        (P.Architecture == Arch::X86 || P.Architecture == Arch::ARM)) {
      const auto Source = Directory / "original.c",
                 IR = Directory / "original.ll";
      std::ofstream(Source) << originalSource();
      ASSERT_EQ(run({NEVERD_TEST_CLANG,
                     std::string("--target=") +
                         (P.Architecture == Arch::ARM
                              ? (P.FrontendTriple ? P.FrontendTriple
                                                  : "armv7k-apple-watchos")
                              : "i386-apple-macos10.6"),
                     P.Architecture == Arch::ARM ? "-mthumb" : "-m32", "-O0",
                     "-funwind-tables", "-emit-llvm", "-S", Source.string(),
                     "-o", IR.string()}),
                0);
      ASSERT_EQ(
          run({NEVERD_TEST_LEGACY_FIXTURE, IR.string(), Original.string()}), 0);
    } else {
      const auto Source = Directory / "original.c",
                 Object = Directory / "original.o";
      std::ofstream(Source) << originalSource();
      ASSERT_EQ(run({NEVERD_TEST_CLANG,
                     std::string("--target=") +
                         llvmCodegenTriple(P.Architecture, P.Format),
                     "-O0", "-funwind-tables", "-fno-pie", "-c",
                     Source.string(), "-o", Object.string()}),
                0);
      const auto Linker = linker(P.Format == BinaryFormat::ELF    ? "ld.lld"
                                 : P.Format == BinaryFormat::COFF ? "lld-link"
                                                                  : "ld64.lld");
      ASSERT_FALSE(Linker.empty())
          << "cross LLD is required for the complete matrix";
      if (P.Format == BinaryFormat::ELF) {
        ASSERT_EQ(run({Linker, "--eh-frame-hdr", "-e", "target",
                       Object.string(), "-o", Original.string()}),
                  0);
      } else if (P.Format == BinaryFormat::COFF) {
        ASSERT_EQ(run({Linker, Object.string(), "/out:" + Original.string(),
                       "/entry:target", "/subsystem:console", "/nodefaultlib",
                       "/debug:dwarf"}),
                  0);
      } else {
        ASSERT_EQ(run({Linker, "-arch",
                       P.Architecture == Arch::X64 ? "x86_64" : "arm64",
                       "-platform_version", "macos", "14.0", "14.0", "-e",
                       "_target", "-headerpad", "0x800", "-no_adhoc_codesign",
                       Object.string(), "-o", Original.string()}),
                  0);
      }
    }
    auto Reader = Loader::create(Original);
    ASSERT_NE(Reader, nullptr);
    auto Loaded = Reader->load(Original);
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    Image = std::move(*Loaded);
    OriginalBytes = read(Original);
    ASSERT_EQ(Image.Arch, GetParam().Architecture);
    ASSERT_EQ(Image.Format, GetParam().Format);
    execute(Image, GetParam(), 7, 8);
    ASSERT_FALSE(HasFatalFailure());
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
    ASSERT_EQ(neverd_session_load(Session, Original.string().c_str()), 1)
        << error();
    Mapping = {"replacement", Image.Entry};
    Options.struct_size = sizeof(Options);
    Options.compiler_path = NEVERD_TEST_CLANG;
    // Keep the original Clang image as an independent ABI oracle when
    // qualifying the optional ordinary NeverC source producer.
    if (const char *NeverC = std::getenv("NEVERD_TEST_MATRIX_NEVERC");
        NeverC && *NeverC) {
      Options.compiler = NEVERD_SOURCE_PATCH_NEVERC;
      Options.compiler_path = NeverC;
    }
    Options.mappings = &Mapping;
    Options.mapping_count = 1;
  }
  void TearDown() override {
    neverd_session_destroy(Session);
    if (HasFailure()) {
      std::cerr << "Source matrix diagnostic fixture: " << Directory << '\n';
      return;
    }
    std::error_code Ignored;
    if (!Directory.empty())
      std::filesystem::remove_all(Directory, Ignored);
  }
  std::string error() { return take(neverd_last_error(Session)); }
  void expectObjectReceipt() {
    auto Report = llvm::json::parse(take(neverd_patch_source_report(Session)));
    ASSERT_TRUE(bool(Report));
    const auto *Object = Report->getAsObject();
    ASSERT_NE(Object, nullptr);
    EXPECT_EQ(Object->getBoolean("success"), true);
    EXPECT_EQ(Object->getString("target"), GetParam().ReceiptTriple);
    EXPECT_EQ(Object->getInteger("artifact"),
              NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT);
    if (Options.input_kind == NEVERD_SOURCE_PATCH_OBJECT) {
      EXPECT_EQ(Object->getString("producer"), "external");
      EXPECT_EQ(Object->getInteger("input_kind"), NEVERD_SOURCE_PATCH_OBJECT);
    }
  }
  int patch() {
    constexpr const char *Source =
        "__attribute__((noinline)) static int helper(int x) { volatile int "
        "y=x; return y*3; } int replacement(int x) { if(x<0) return -7; return "
        "helper(x)+40; }";
    return neverd_patch_source(Session, Source, std::strlen(Source), &Options,
                               Output.string().c_str());
  }
};

TEST_P(SourcePatchMatrix, CompilesInstallsAndExecutes) {
  const auto P = GetParam();
  const int Result = patch();
  ASSERT_EQ(Result, 1) << error();
  auto Report = llvm::json::parse(take(neverd_patch_source_report(Session)));
  ASSERT_TRUE(bool(Report));
  EXPECT_EQ(Report->getAsObject()->getInteger("compiler"), Options.compiler);
  EXPECT_EQ(Report->getAsObject()->getString("compiler_path"),
            Options.compiler_path);
  if (Options.compiler == NEVERD_SOURCE_PATCH_NEVERC &&
      P.Format == BinaryFormat::COFF) {
    const auto *Arguments =
        Report->getAsObject()->getArray("compiler_arguments");
    ASSERT_NE(Arguments, nullptr);
    EXPECT_TRUE(llvm::any_of(*Arguments, [](const llvm::json::Value &Argument) {
      return Argument.getAsString() == "-fno-autolink";
    }));
  }
  const auto *Installed = Report->getAsObject()->getArray("installed");
  ASSERT_NE(Installed, nullptr);
  ASSERT_EQ(Installed->size(), 1u);
  EXPECT_EQ(Installed->front().getAsObject()->getInteger("original_va"),
            Image.Entry);
  auto Reader = Loader::create(Output);
  ASSERT_NE(Reader, nullptr);
  auto Reloaded = Reader->load(Output);
  ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
  for (int X : {-1, 0, 1, 7, 25})
    execute(*Reloaded, P, X, X < 0 ? -7 : X * 3 + 40);
  EXPECT_EQ(read(Original), OriginalBytes);
}

TEST_P(SourcePatchMatrix, SuppliedIRUsesTheSameInstallationContract) {
  const auto P = GetParam();
  Options.input_kind = NEVERD_SOURCE_PATCH_LLVM_IR;
  const std::string IR =
      std::string("target triple = \"") +
      (P.Format == BinaryFormat::MachO && P.Architecture == Arch::ARM
           ? (P.FrontendTriple ? "thumbv7-apple-ios5.0"
                               : "thumbv7k-apple-watchos")
           : llvmCodegenTriple(P.Architecture, P.Format)) +
      "\"\n" + "target datalayout = \"e-p:" +
      std::to_string(Image.getPointerSize() * 8) + ":" +
      std::to_string(Image.getPointerSize() * 8) + "\"\n" +
      "define i32 @replacement(i32 %x) uwtable {\n"
      "  %value = add i32 %x, 40\n  ret i32 %value\n}\n";
  const int Result = neverd_patch_source(Session, IR.data(), IR.size(),
                                         &Options, Output.string().c_str());
  ASSERT_EQ(Result, 1) << error();
  auto Reader = Loader::create(Output);
  ASSERT_NE(Reader, nullptr);
  auto Reloaded = Reader->load(Output);
  ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
  for (int X : {-1, 0, 7, 25})
    execute(*Reloaded, P, X, X + 40);
  EXPECT_EQ(read(Original), OriginalBytes);
}

TEST_P(SourcePatchMatrix, MutableStateAndReadOnlyData) {
  constexpr const char *Source =
      "static volatile int counter=5; static const int table[3]={9,3,7}; "
      "__attribute__((noinline)) static int next(void) {return counter++;} "
      "int replacement(int x) {return next()+next()+table[(unsigned)x%3];}";
  ASSERT_EQ(neverd_patch_source(Session, Source, std::strlen(Source), &Options,
                                Output.string().c_str()),
            1)
      << error();
  auto Reader = Loader::create(Output);
  auto Reloaded = Reader->load(Output);
  ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
  for (int X : {0, 1, 2, 7})
    execute(*Reloaded, GetParam(), X,
            11 + (X % 3 == 0   ? 9
                  : X % 3 == 1 ? 3
                               : 7));
  EXPECT_EQ(read(Original), OriginalBytes);
}

TEST_P(SourcePatchMatrix, OriginalFloatingPointAndStackArgumentABI) {
  uint64_t Address = 0;
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.IsFunc &&
        (Symbol.Name == "original_fp" || Symbol.Name == "_original_fp"))
      Address = Symbol.Addr;
  for (const auto &Export : Image.Exports)
    if (Export.Name == "original_fp" || Export.Name == "_original_fp")
      Address = Export.Addr;
  ASSERT_NE(Address, 0u);
  ASSERT_TRUE(Image.hasAuthenticatedFunctionEntryAt(Address));
  neverd_source_patch_binding_v1 Binding{
      "original_fp", NEVERD_SOURCE_BIND_FUNCTION, Address, 0};
  Options.bindings = &Binding;
  Options.binding_count = 1;
  constexpr const char *Source =
      "extern double "
      "original_fp(float,double,int,int,int,int,int,int,int,int); "
      "int replacement(int x) {return "
      "(int)original_fp(2.5f,3.5,x,2,3,4,5,6,7,8);}";
  ASSERT_EQ(neverd_patch_source(Session, Source, std::strlen(Source), &Options,
                                Output.string().c_str()),
            1)
      << error();
  auto Reader = Loader::create(Output);
  auto Reloaded = Reader->load(Output);
  ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
  for (int X : {-1, 0, 7, 25})
    execute(*Reloaded, GetParam(), X, X + 43);
  EXPECT_EQ(read(Original), OriginalBytes);
}

TEST_P(SourcePatchMatrix, NativeObjectMutableStateAndReadOnlyData) {
  Options.artifact = NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT;
  constexpr const char *Source =
      "static volatile int counter=5; static const int table[3]={9,3,7}; "
      "__attribute__((noinline)) static int next(void) {return counter++;} "
      "int replacement(int x) {return next()+next()+table[(unsigned)x%3];}";
  ASSERT_EQ(neverd_patch_source(Session, Source, std::strlen(Source), &Options,
                                Output.string().c_str()),
            1)
      << error();
  auto Reader = Loader::create(Output);
  auto Reloaded = Reader->load(Output);
  ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
  for (int X : {0, 1, 2, 7})
    execute(*Reloaded, GetParam(), X,
            11 + (X % 3 == 0   ? 9
                  : X % 3 == 1 ? 3
                               : 7));
  EXPECT_EQ(read(Original), OriginalBytes);
}
TEST_P(SourcePatchMatrix, NativeObjectFloatingPointAndStackArgumentABI) {
  Options.artifact = NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT;
  uint64_t Address = 0;
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.IsFunc &&
        (Symbol.Name == "original_fp" || Symbol.Name == "_original_fp"))
      Address = Symbol.Addr;
  for (const auto &Export : Image.Exports)
    if (Export.Name == "original_fp" || Export.Name == "_original_fp")
      Address = Export.Addr;
  ASSERT_NE(Address, 0u);
  ASSERT_TRUE(Image.hasAuthenticatedFunctionEntryAt(Address));
  neverd_source_patch_binding_v1 Binding{
      "original_fp", NEVERD_SOURCE_BIND_FUNCTION, Address, 0};
  Options.bindings = &Binding;
  Options.binding_count = 1;
  constexpr const char *Source =
      "extern double "
      "original_fp(float,double,int,int,int,int,int,int,int,int); "
      "int replacement(int x) {return "
      "(int)original_fp(2.5f,3.5,x,2,3,4,5,6,7,8);}";
  ASSERT_EQ(neverd_patch_source(Session, Source, std::strlen(Source), &Options,
                                Output.string().c_str()),
            1)
      << error();
  auto Reader = Loader::create(Output);
  auto Reloaded = Reader->load(Output);
  ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
  for (int X : {-1, 0, 7, 25})
    execute(*Reloaded, GetParam(), X, X + 43);
  EXPECT_EQ(read(Original), OriginalBytes);
}
TEST_P(SourcePatchMatrix, NativeObjectProfileCannotFallBackToIR) {
  Options.artifact = NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT;
  ASSERT_EQ(patch(), 1) << error();
  expectObjectReceipt();
  auto Reader = Loader::create(Output);
  ASSERT_NE(Reader, nullptr);
  auto Reloaded = Reader->load(Output);
  ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
  execute(*Reloaded, GetParam(), 7, 61);
  EXPECT_EQ(read(Original), OriginalBytes);
}
TEST_P(SourcePatchMatrix, NativeObjectRejectsMismatchedAndTruncatedHeaders) {
  const auto C = Directory / "bad-object.c", O = Directory / "bad-object.o";
  std::ofstream(C)
      << "__attribute__((noinline)) static int helper(int x) {volatile int "
         "y=x;return y*3;} int replacement(int x){return helper(x)+40;}";
  auto Target = sourcePatchTargetTriple(Image);
  ASSERT_TRUE(bool(Target)) << llvm::toString(Target.takeError());
  std::vector<std::string> Args = {NEVERD_TEST_CLANG,
                                   "--target=" + *Target,
                                   "-O2",
                                   "-funwind-tables",
                                   "-fno-lto",
                                   "-c",
                                   C.string(),
                                   "-o",
                                   O.string()};
  if (Image.Format == BinaryFormat::MachO && Image.Arch == Arch::ARM) {
    Args[1] = "--target=arm" + Target->substr(5);
    Args.push_back("-mthumb");
    Args.push_back("-fno-omit-frame-pointer");
    Args.push_back("-mno-omit-leaf-frame-pointer");
  }
  if (Image.Format == BinaryFormat::COFF)
    Args.push_back("-ffunction-sections");
  else
    Args.push_back("-fPIC");
  ASSERT_EQ(run(Args), 0);
  const auto Bytes = read(O);
  ASSERT_GT(Bytes.size(), 24u);
  Options.input_kind = NEVERD_SOURCE_PATCH_OBJECT;
  Options.artifact = NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT;
  auto Wrong = Bytes;
  const size_t Machine = Image.Format == BinaryFormat::ELF    ? 18
                         : Image.Format == BinaryFormat::COFF ? 0
                                                              : 4;
  Wrong[Machine] = char(0xfe);
  Wrong[Machine + 1] = char(0x7f);
  EXPECT_EQ(neverd_patch_source(Session, Wrong.data(), Wrong.size(), &Options,
                                Output.string().c_str()),
            0);
  EXPECT_FALSE(error().empty());
  EXPECT_FALSE(std::filesystem::exists(Output));
  EXPECT_EQ(neverd_patch_source(Session, Bytes.data(), 15, &Options,
                                Output.string().c_str()),
            0);
  EXPECT_FALSE(error().empty());
  EXPECT_FALSE(std::filesystem::exists(Output));
  EXPECT_EQ(read(Original), OriginalBytes);
  if (Image.Format == BinaryFormat::COFF) {
    // Suppressing the source producer's automatic libraries must not weaken
    // the independent contract of an externally supplied linker directive.
    std::ofstream(C) << "#pragma comment(lib, \"unavailable_patch_runtime\")\n"
                        "int replacement(int x) {return x+40;}\n";
    ASSERT_EQ(run(Args), 0);
    const auto Directed = read(O);
    EXPECT_EQ(neverd_patch_source(Session, Directed.data(), Directed.size(),
                                  &Options, Output.string().c_str()),
              0);
    EXPECT_NE(error().find(".drectve"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(Output));
    EXPECT_EQ(read(Original), OriginalBytes);
  }
  // The same independent object must also succeed. This prevents rejection of
  // every object from satisfying the malformed-input assertions above, and
  // checks receipt identity without going through the source compiler adapter.
  ASSERT_EQ(neverd_patch_source(Session, Bytes.data(), Bytes.size(), &Options,
                                Output.string().c_str()),
            1)
      << error();
  expectObjectReceipt();
  auto Reader = Loader::create(Output);
  ASSERT_NE(Reader, nullptr);
  auto Reloaded = Reader->load(Output);
  ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
  execute(*Reloaded, GetParam(), 7, 61);
  EXPECT_EQ(read(Original), OriginalBytes);
}
TEST_P(SourcePatchMatrix, SourceEntrySurvivesImageRebasing) {
  ASSERT_EQ(patch(), 1) << error();
  auto Reader = Loader::create(Output);
  auto Reloaded = Reader->load(Output);
  ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
  for (uint64_t Slide : {0x02000000u, 0x08000000u})
    execute(*Reloaded, GetParam(), 7, 61, Slide);
}
TEST_P(SourcePatchMatrix, ObjectEntrySurvivesImageRebasing) {
  Options.artifact = NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT;
  ASSERT_EQ(patch(), 1) << error();
  auto Reader = Loader::create(Output);
  auto Reloaded = Reader->load(Output);
  ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
  for (uint64_t Slide : {0x02000000u, 0x08000000u})
    execute(*Reloaded, GetParam(), 7, 61, Slide);
}
TEST_P(SourcePatchMatrix, DyncodeCannotPublishAnOrdinaryCompilation) {
  Options.mode = NEVERD_SOURCE_PATCH_DYNCODE;
  EXPECT_EQ(patch(), 0);
  EXPECT_NE(error().find("dyncode"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}
INSTANTIATE_TEST_SUITE_P(AllTargets, SourcePatchMatrix,
                         testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &Info) {
                           return Info.param.Name;
                         });

class NeverCSourcePatchMatrix : public SourcePatchMatrix {
protected:
  void SetUp() override {
    const char *Compiler = std::getenv("NEVERD_TEST_MATRIX_NEVERC");
    if (!Compiler || !*Compiler)
      GTEST_SKIP()
          << "explicit NeverC producer required for its optional matrix";
    SourcePatchMatrix::SetUp();
  }
  void check(llvm::StringRef Source, bool Object, bool DynCode, int Scale,
             int Offset) {
    Options.language = NEVERD_SOURCE_PATCH_C;
    Options.artifact = Object ? NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT
                              : NEVERD_SOURCE_PATCH_ARTIFACT_IR;
    Options.mode =
        DynCode ? NEVERD_SOURCE_PATCH_DYNCODE : NEVERD_SOURCE_PATCH_NORMAL;
    ASSERT_EQ(neverd_patch_source(Session, Source.data(), Source.size(),
                                  &Options, Output.string().c_str()),
              1)
        << error();
    auto Parsed = llvm::json::parse(take(neverd_patch_source_report(Session)));
    ASSERT_TRUE(bool(Parsed));
    auto *Receipt = Parsed->getAsObject();
    ASSERT_NE(Receipt, nullptr);
    EXPECT_EQ(Receipt->getInteger("compiler"), NEVERD_SOURCE_PATCH_NEVERC);
    EXPECT_EQ(Receipt->getInteger("language"), Options.language);
    EXPECT_EQ(Receipt->getObject("translation"), nullptr);
    if (DynCode) {
      ASSERT_NE(Receipt->getObject("dyncode"), nullptr);
      EXPECT_EQ(Receipt->getObject("dyncode")->getString("execution_profile"),
                "hosted-v1");
      EXPECT_EQ(Receipt->getObject("dyncode")->getBoolean("keep_unwind"), true);
    }
    auto Reader = Loader::create(Output);
    auto Reloaded = Reader->load(Output);
    ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
    for (int X : {-3, 0, 7, 25})
      execute(*Reloaded, GetParam(), X, X * Scale + Offset);
    EXPECT_EQ(read(Original), OriginalBytes);
  }
  static constexpr llvm::StringLiteral CSource = R"(
struct State { int value; };
static int twice(int value) { return value * 2; }
int replacement(int x) {
  struct State state = {x + 3};
  state.value = twice(state.value);
  return state.value + 5 + sizeof(long);
})";
};
TEST_P(NeverCSourcePatchMatrix, CIRPreservesTargetLayout) {
  check(CSource, false, false, 2,
        GetParam().Format == BinaryFormat::COFF ? 15 : 19);
}
TEST_P(NeverCSourcePatchMatrix, CObjectPreservesTargetLayout) {
  check(CSource, true, false, 2,
        GetParam().Format == BinaryFormat::COFF ? 15 : 19);
}
TEST_P(NeverCSourcePatchMatrix, HostedCUsesExactPayloadLayout) {
  check(CSource, true, true, 2,
        GetParam().Format == BinaryFormat::COFF ? 15 : 19);
}
TEST_P(NeverCSourcePatchMatrix,
       HostedCHelpersAndRecursionKeepExactPayloadLayout) {
  check(R"(
__attribute__((noinline)) static int recurse(int x) {
  if (x <= 0) return 0;
  volatile int value = recurse(x - 1); return value + 1;
}
__attribute__((noinline)) static int helper(int x) {
  volatile int value = x * 3; return value;
}
int replacement(int x) {
  volatile int value = recurse(3); return helper(x) + value + 37;
})",
        true, true, 3, 40);
}
TEST_P(NeverCSourcePatchMatrix, CppSourceCannotPublishWithNeverC) {
  Options.language = NEVERD_SOURCE_PATCH_CXX;
  for (llvm::StringRef Source : {"extern \"C\" int replacement(int x) { try "
                                 "{throw x;} catch (...) {return 1;} }",
                                 "int replacement(int x) { return x+1; }"}) {
    EXPECT_EQ(neverd_patch_source(Session, Source.data(), Source.size(),
                                  &Options, Output.string().c_str()),
              0);
    EXPECT_NE(error().find("NeverC accepts C source only"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(Output));
  }
}
INSTANTIATE_TEST_SUITE_P(NeverCProfiles, NeverCSourcePatchMatrix,
                         testing::Values(Profiles[1], Profiles[3], Profiles[5],
                                         Profiles[7], Profiles[9],
                                         Profiles[11]),
                         [](const testing::TestParamInfo<Profile> &Info) {
                           return Info.param.Name;
                         });
} // namespace
