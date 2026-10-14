//===- SourcePatchObjectTests.cpp - Native object input contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>

namespace {
using ELF = llvm::object::ELF64LE;

std::string read(const std::filesystem::path &Path) {
  std::ifstream Input(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(Input), {}};
}
std::string take(const char *Text) {
  std::string Result = Text ? Text : "";
  neverd_free_string(Text);
  return Result;
}
int run(const std::vector<std::string> &Arguments) {
  std::vector<llvm::StringRef> Args(Arguments.begin(), Arguments.end());
  return llvm::sys::ExecuteAndWait(Args.front(), Args, std::nullopt, {}, 30);
}

class SourcePatchObject : public testing::Test {
protected:
  std::filesystem::path Directory, Original, Output, Object;
  neverd_session_t Session = nullptr;
  neverd_source_patch_options_v1 Options{};
  neverd_source_patch_mapping_v1 Mapping{};
  void SetUp() override {
    if (std::string(NEVERD_TEST_CLANG).empty())
      GTEST_SKIP() << "cross Clang and LLD required for object fixtures";
    auto LLD =
        std::filesystem::path(NEVERD_TEST_CLANG).parent_path() / "ld.lld";
    if (!std::filesystem::exists(LLD)) {
      auto Program = llvm::sys::findProgramByName("ld.lld");
      if (Program)
        LLD = *Program;
    }
    if (!std::filesystem::exists(LLD))
      GTEST_SKIP() << "matching ld.lld is unavailable";
    llvm::SmallString<256> Temp;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-object", Temp));
    Directory = Temp.str().str();
    Original = Directory / "original";
    Output = Directory / "patched";
    Object = Directory / "replacement.o";
    const auto Source = Directory / "original.c";
    std::ofstream(Source) << R"(
__attribute__((noinline)) int target(int x) { volatile int y=x; return y+1; }
void _start(void) {
  int value = target(2);
  __asm__ volatile("syscall" : : "a"(60L), "D"((long)value) : "rcx", "r11", "memory");
  __builtin_unreachable();
})";
    ASSERT_EQ(run({NEVERD_TEST_CLANG, "--target=x86_64-unknown-linux-gnu",
                   "-fuse-ld=" + LLD.string(), "-O0", "-nostdlib", "-static",
                   "-fno-pie", "-Wl,--eh-frame-hdr", Source.string(), "-o",
                   Original.string()}),
              0);
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
    ASSERT_EQ(neverd_session_load(Session, Original.string().c_str()), 1)
        << error();
    for (int I = 0; I < neverd_func_count(Session); ++I)
      if (take(neverd_func_name(Session, I)) == "target")
        Mapping.original_va = neverd_func_entry(Session, I);
    ASSERT_NE(Mapping.original_va, 0u);
    Mapping.symbol = "replacement";
    Options.struct_size = sizeof(Options);
    Options.input_kind = NEVERD_SOURCE_PATCH_OBJECT;
    Options.artifact = NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT;
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
  std::string error() { return take(neverd_last_error(Session)); }
  void compile(const char *Source = "int replacement(int x) { return x+40; }") {
    const auto Path = Directory / "replacement.c";
    std::ofstream(Path) << Source;
    ASSERT_EQ(
        run({NEVERD_TEST_CLANG, "--target=x86_64-unknown-linux-gnu", "-O0",
             "-fPIC", "-fno-lto", "-c", Path.string(), "-o", Object.string()}),
        0);
  }
  int patch(const std::string &Bytes) {
    return neverd_patch_source(Session, Bytes.data(), Bytes.size(), &Options,
                               Output.string().c_str());
  }
};

class SourcePatchAArch64Object : public SourcePatchObject {
protected:
  void SetUp() override {
    SourcePatchObject::SetUp();
    if (HasFatalFailure() || IsSkipped())
      return;
    auto LLD =
        std::filesystem::path(NEVERD_TEST_CLANG).parent_path() / "ld.lld";
    if (!std::filesystem::exists(LLD))
      LLD = *llvm::sys::findProgramByName("ld.lld");
    Original = Directory / "original-aarch64";
    const auto Source = Directory / "original-aarch64.c";
    std::ofstream(Source) << R"(
__attribute__((noinline)) int target(int x) { volatile int y=x; return y+1; }
void _start(void) {
  register long result __asm__("x0") = target(2);
  register long syscall_number __asm__("x8") = 93;
  __asm__ volatile("svc 0" : : "r"(result), "r"(syscall_number) : "memory");
  __builtin_unreachable();
})";
    ASSERT_EQ(run({NEVERD_TEST_CLANG, "--target=aarch64-unknown-linux-gnu",
                   "-fuse-ld=" + LLD.string(), "-O0", "-nostdlib", "-static",
                   "-fno-pie", "-Wl,--eh-frame-hdr", Source.string(), "-o",
                   Original.string()}),
              0);
    ASSERT_EQ(neverd_session_load(Session, Original.string().c_str()), 1)
        << error();
    Mapping.original_va = 0;
    for (int I = 0; I < neverd_func_count(Session); ++I)
      if (take(neverd_func_name(Session, I)) == "target")
        Mapping.original_va = neverd_func_entry(Session, I);
    ASSERT_NE(Mapping.original_va, 0u);
    const auto Assembly = Directory / "relative.s";
    std::ofstream(Assembly) << R"(
.text
.global replacement
.type replacement,%function
replacement:
.cfi_startproc
  add w0, w0, #40
  ret
.cfi_endproc
.size replacement, .-replacement
.section .rodata,"a",%progbits
.balign 8
relative_entries:
  .xword replacement - . + 0x10000000007
  .xword replacement - . - 0x1000000000d
)";
    ASSERT_EQ(run({NEVERD_TEST_CLANG, "--target=aarch64-unknown-linux-gnu",
                   "-c", Assembly.string(), "-o", Object.string()}),
              0);
  }

  void checkRelativeFields() {
    auto Report = llvm::json::parse(take(neverd_patch_source_report(Session)));
    ASSERT_TRUE(bool(Report));
    const auto *Installed = Report->getAsObject()->getArray("installed");
    ASSERT_NE(Installed, nullptr);
    ASSERT_EQ(Installed->size(), 1u);
    const auto Entry =
        (*Installed)[0].getAsObject()->getInteger("replacement_va");
    ASSERT_TRUE(Entry.has_value());
    const auto Bytes = read(Output);
    auto File = llvm::object::ELFFile<ELF>::create(Bytes);
    ASSERT_TRUE(bool(File));
    auto Sections = File->sections();
    ASSERT_TRUE(bool(Sections));
    unsigned Found = 0;
    for (const auto &Section : *Sections) {
      auto Name = File->getSectionName(Section);
      ASSERT_TRUE(bool(Name));
      if (*Name != ".neverd.rodata")
        continue;
      auto Contents = File->getSectionContents(Section);
      ASSERT_TRUE(bool(Contents));
      ASSERT_EQ(Contents->size(), 16u);
      EXPECT_EQ(neverd::readLE<uint64_t>(Contents->data()) + Section.sh_addr -
                    (UINT64_C(1) << 40) - 7,
                uint64_t(*Entry));
      EXPECT_EQ(neverd::readLE<uint64_t>(Contents->data() + 8) +
                    Section.sh_addr + 8 + (UINT64_C(1) << 40) + 13,
                uint64_t(*Entry));
      ++Found;
    }
    EXPECT_EQ(Found, 1u);
  }
};

TEST_F(SourcePatchAArch64Object,
       ExplicitAndImplicitPREL64KeepCompleteSignedAddends) {
  const auto OriginalBytes = read(Original), ObjectBytes = read(Object);
  ASSERT_EQ(patch(ObjectBytes), 1) << error();
  checkRelativeFields();
  auto Bytes = ObjectBytes;
  auto File = llvm::object::ELFFile<ELF>::create(Bytes);
  ASSERT_TRUE(bool(File));
  auto Sections = File->sections();
  ASSERT_TRUE(bool(Sections));
  bool Changed = false;
  for (size_t I = 0; I < Sections->size(); ++I) {
    const auto &Section = (*Sections)[I];
    auto Name = File->getSectionName(Section);
    ASSERT_TRUE(bool(Name));
    if (*Name != ".rela.rodata")
      continue;
    auto Rows = File->relas(Section);
    ASSERT_TRUE(bool(Rows));
    ASSERT_EQ(Rows->size(), 2u);
    const auto Target = (*Sections)[Section.sh_info];
    const auto OriginalRows = *Rows;
    ELF::Rela Relocations[2] = {OriginalRows[0], OriginalRows[1]};
    auto *Header = reinterpret_cast<ELF::Shdr *>(Bytes.data() +
                                                 File->getHeader().e_shoff) +
                   I;
    Header->sh_type = llvm::ELF::SHT_REL;
    Header->sh_entsize = sizeof(ELF::Rel);
    Header->sh_size = 2 * sizeof(ELF::Rel);
    for (unsigned Index = 0; Index != 2; ++Index) {
      ELF::Rel Rel;
      Rel.r_offset = Relocations[Index].r_offset;
      Rel.r_info = Relocations[Index].r_info;
      std::memcpy(Bytes.data() + Section.sh_offset + Index * sizeof(Rel), &Rel,
                  sizeof(Rel));
      neverd::writeLE<uint64_t>(reinterpret_cast<uint8_t *>(Bytes.data()) +
                                    Target.sh_offset + Rel.r_offset,
                                uint64_t(Relocations[Index].r_addend));
    }
    Changed = true;
  }
  ASSERT_TRUE(Changed);
  Output = Directory / "implicit";
  ASSERT_EQ(patch(Bytes), 1) << error();
  checkRelativeFields();
  EXPECT_EQ(read(Original), OriginalBytes);
}

TEST_F(SourcePatchAArch64Object,
       PREL64RejectsTruncationOverlapAndSignedOverflow) {
  const auto ObjectBytes = read(Object), OriginalBytes = read(Original);
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    auto Bytes = ObjectBytes;
    auto File = llvm::object::ELFFile<ELF>::create(Bytes);
    ASSERT_TRUE(bool(File));
    auto Sections = File->sections();
    ASSERT_TRUE(bool(Sections));
    bool Changed = false;
    for (const auto &Section : *Sections) {
      auto Name = File->getSectionName(Section);
      ASSERT_TRUE(bool(Name));
      if (*Name != ".rela.rodata")
        continue;
      auto *Rows =
          reinterpret_cast<ELF::Rela *>(Bytes.data() + Section.sh_offset);
      if (Mutation == 0)
        Rows[0].r_offset = (*Sections)[Section.sh_info].sh_size - 4;
      else if (Mutation == 1)
        Rows[1].r_offset = 4;
      else
        Rows[0].r_addend = std::numeric_limits<int64_t>::min();
      Changed = true;
    }
    ASSERT_TRUE(Changed);
    EXPECT_EQ(patch(Bytes), 0) << Mutation;
    EXPECT_NE(error().find(Mutation == 0   ? "relocation exceeds"
                           : Mutation == 1 ? "overlapping"
                                           : "overflows"),
              std::string::npos)
        << error();
    EXPECT_FALSE(std::filesystem::exists(Output));
    EXPECT_EQ(read(Original), OriginalBytes);
  }
}

TEST_F(SourcePatchObject, InstallsExactMachineCodeWithoutRecompilation) {
  compile();
  const std::string Before = read(Original), Bytes = read(Object);
  ASSERT_EQ(patch(Bytes), 1) << error();
  EXPECT_EQ(read(Original), Before);
  EXPECT_EQ(neverd_patch_trampoline_count(Session), 1);
  const auto ReportText = take(neverd_patch_source_report(Session));
  auto Report = llvm::json::parse(ReportText);
  ASSERT_TRUE(bool(Report));
  ASSERT_EQ(Report->getAsObject()->getBoolean("success"), true);
  auto Input = llvm::object::ELFFile<ELF>::create(Bytes);
  ASSERT_TRUE(bool(Input));
  const std::string Patched = read(Output);
  auto Result = llvm::object::ELFFile<ELF>::create(Patched);
  ASSERT_TRUE(bool(Result));
  auto InSections = Input->sections(), OutSections = Result->sections();
  ASSERT_TRUE(bool(InSections));
  ASSERT_TRUE(bool(OutSections));
  bool Compared = false;
  for (const auto &Section : *InSections) {
    auto Name = Input->getSectionName(Section);
    ASSERT_TRUE(bool(Name));
    if (*Name != ".text")
      continue;
    auto Code = Input->getSectionContents(Section);
    ASSERT_TRUE(bool(Code));
    for (const auto &Installed : *OutSections) {
      auto OutName = Result->getSectionName(Installed);
      ASSERT_TRUE(bool(OutName));
      if (*OutName != ".neverd.text")
        continue;
      auto NewCode = Result->getSectionContents(Installed);
      ASSERT_TRUE(bool(NewCode));
      EXPECT_EQ(*Code, *NewCode);
      Compared = true;
    }
  }
  EXPECT_TRUE(Compared);
#if defined(__linux__) && defined(__x86_64__)
  EXPECT_EQ(run({Original.string()}), 3);
  EXPECT_EQ(run({Output.string()}), 42);
#endif
}

TEST_F(SourcePatchObject, NativeCompilerAndPrecompiledInputShareTheInstaller) {
  Options.input_kind = NEVERD_SOURCE_PATCH_SOURCE;
  Options.compiler_path = NEVERD_TEST_CLANG;
  ASSERT_EQ(patch("int replacement(int x) { return x+40; }"), 1) << error();
  const auto First = read(Output);
  Output = Directory / "precompiled";
  const auto Path = Directory / "same.c";
  std::ofstream(Path) << "int replacement(int x) { return x+40; }";
  ASSERT_EQ(run({NEVERD_TEST_CLANG, "--target=x86_64-unknown-linux-gnu", "-O2",
                 "-fno-discard-value-names", "-fno-lto", "-fPIC", "-c",
                 Path.string(), "-o", Object.string()}),
            0);
  Options.input_kind = NEVERD_SOURCE_PATCH_OBJECT;
  ASSERT_EQ(patch(read(Object)), 1) << error();
  EXPECT_EQ(read(Output), First);
}

TEST_F(SourcePatchObject, SourceCompilerCannotExpandResponseFileValues) {
  const auto Response = Directory / "extra arguments.rsp";
  std::ofstream(Response) << "unused-directory -DBASE=37\n";
  const std::string ResponseArgument = "@" + Response.string();
  const char *Arguments[] = {"-I", ResponseArgument.c_str()};
  Options.input_kind = NEVERD_SOURCE_PATCH_SOURCE;
  Options.compiler_path = NEVERD_TEST_CLANG;
  Options.compiler_args = Arguments;
  Options.compiler_arg_count = 2;
  EXPECT_EQ(patch("int replacement(int x) { return x+BASE; }"), 0);
  EXPECT_NE(error().find("response files"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatchObject, NeverCEntryTableCannotLoseItsLoadTimeRelocations) {
  const char *NeverC = std::getenv("NEVERD_TEST_NEVERC");
  if (!NeverC || !*NeverC)
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC for the optional native producer";
  const auto Source = Directory / "padded.c";
  std::ofstream(Source)
      << "__attribute__((noinline)) int helper(int x) { return x+1; } "
         "int replacement(int x) { return helper(x)+39; }";
  ASSERT_EQ(run({NeverC, "--target=x86_64-unknown-linux-gnu", "-O2", "-fPIC",
                 "-fno-lto", "-fpatchable-function-entry=16", "-c",
                 Source.string(), "-o", Object.string()}),
            0);
  const auto Bytes = read(Object);
  auto File = llvm::object::ELFFile<ELF>::create(Bytes);
  ASSERT_TRUE(bool(File));
  auto Sections = File->sections();
  ASSERT_TRUE(bool(Sections));
  // This static fixture has no dynamic relocation table. Drop neither the
  // link-order entry table nor its pointers just to make installation succeed.
  EXPECT_EQ(patch(Bytes), 0);
  EXPECT_NE(error().find("PT_DYNAMIC"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatchObject, InvalidOrForeignObjectsCannotPublish) {
  compile();
  const auto Bytes = read(Object);
  for (auto Invalid : {std::string("BC\300\336"), Bytes.substr(0, 40), Bytes}) {
    if (Invalid.size() == Bytes.size())
      neverd::writeLE<uint16_t>(reinterpret_cast<uint8_t *>(Invalid.data()) +
                                    18,
                                llvm::ELF::EM_AARCH64);
    EXPECT_EQ(patch(Invalid), 0);
    EXPECT_FALSE(std::filesystem::exists(Output));
  }
}

TEST_F(SourcePatchObject, OutOfBoundsRelocationCannotPublish) {
  compile("extern int external(int); int replacement(int x) { return "
          "external(x); }");
  auto Bytes = read(Object);
  auto File = llvm::object::ELFFile<ELF>::create(Bytes);
  ASSERT_TRUE(bool(File));
  auto Sections = File->sections();
  ASSERT_TRUE(bool(Sections));
  bool Changed = false;
  for (const auto &Section : *Sections) {
    if (Section.sh_type == llvm::ELF::SHT_RELA && Section.sh_size) {
      neverd::writeLE<uint64_t>(reinterpret_cast<uint8_t *>(Bytes.data()) +
                                    Section.sh_offset,
                                UINT64_MAX - 1);
      Changed = true;
      break;
    }
  }
  ASSERT_TRUE(Changed);
  EXPECT_EQ(patch(Bytes), 0);
  EXPECT_NE(error().find("relocation exceeds"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatchObject, MissingEntryAndUnsupportedTLSCannotPublish) {
  compile();
  Mapping.symbol = "missing";
  EXPECT_EQ(patch(read(Object)), 0);
  EXPECT_FALSE(std::filesystem::exists(Output));
  Mapping.symbol = "replacement";
  compile(
      "_Thread_local int value; int replacement(int x) { return ++value+x; }");
  EXPECT_EQ(patch(read(Object)), 0);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatchObject, LegacyInitializationCannotBeSilentlyDropped) {
  compile("__attribute__((section(\".init\"),used)) void initialize(void) {} "
          "int replacement(int x) { return x+40; }");
  EXPECT_EQ(patch(read(Object)), 0);
  EXPECT_NE(error().find("initialization/finalization"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatchObject, CallableRelocationCannotResolveToData) {
  compile(
      "__asm__(\".data\\n.type external,@object\\nexternal: .long 42\\n\"); "
      "extern int external(int); "
      "int replacement(int x) { return external(x); }");
  EXPECT_EQ(patch(read(Object)), 0);
  EXPECT_NE(error().find("callable symbol does not name code"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

} // namespace
