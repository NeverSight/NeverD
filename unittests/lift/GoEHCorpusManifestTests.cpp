//===- GoEHCorpusManifestTests.cpp - Corpus contract tests ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "GoEHCorpusManifest.h"
#include "gtest/gtest.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <string>

using namespace llvm;
using namespace neverd;
using namespace neverd::test;

namespace {

const StringRef kStrippedExePath =
    "corpus/go-eh/go1.26.5/linux-amd64/exe/cgo0/stripped/opt/"
    "eh_probe-go1.26.5-linux-amd64-exe-cgo0-stripped-opt";

/// One variant's axes, defaulted to the stripped linux/amd64 executable the
/// matrix builds for the current release.  A test bends exactly the axis it is
/// about, so a rejection can only come from that axis and not from a
/// hand-written manifest being incidentally wrong elsewhere.
struct GoVariant {
  StringRef Path = kStrippedExePath;
  StringRef GoVersion = "1.26.5";
  StringRef GOOS = "linux";
  StringRef GOARCH = "amd64";
  StringRef ObjectFormat = "elf";
  StringRef BuildMode = "exe";
  StringRef CgoEnabled = "false";
  StringRef Stripped = "true";
  StringRef Optimization = "default";
  StringRef Flags = R"(["-trimpath","-buildmode=exe","-ldflags=-s -w"])";
  StringRef Execution = "passed";
  StringRef RequiredSections = R"([".gopclntab",".noptrdata",".text"])";
  StringRef PclnTabSection = ".gopclntab";
  StringRef AtSectionStart = "true";
  StringRef Magic = "4294967281";
  StringRef MinLC = "1";
  StringRef PointerSize = "8";
  StringRef FunctionCount = "2400";
  StringRef SymbolTable = "loader-only";
  StringRef GoFuncSymbol = "null";
  StringRef NativeUnwindSections = "[]";
  StringRef ValidationLevel = "runtime-graph";
  StringRef AllowedStatuses = R"(["complete"])";
  StringRef PclnTabVersion = "go1.20";
  StringRef MinGoFunctions = "800";
  StringRef MinDeferSites = "5";
  StringRef MinRecoverSites = "8";
  StringRef MinPanicSites = "20";
  StringRef MinOpenCodedDeferFuncs = "12";
  StringRef RequiresModuleData = "true";
};

std::string manifestFor(const GoVariant &Variant) {
  return ("{\"schema_version\":1,\"corpus\":\"go-eh\",\"artifacts\":[{"
          "\"path\":\"" +
          Variant.Path + "\",\"sha256\":\"" + std::string(64, 'a') +
          "\",\"size\":4096,\"goos\":\"" + Variant.GOOS + "\",\"goarch\":\"" +
          Variant.GOARCH + "\",\"go_version\":\"" + Variant.GoVersion +
          "\",\"object_format\":\"" + Variant.ObjectFormat +
          "\",\"buildmode\":\"" + Variant.BuildMode + "\",\"cgo_enabled\":" +
          Variant.CgoEnabled + ",\"stripped\":" + Variant.Stripped +
          ",\"optimization\":\"" + Variant.Optimization +
          "\",\"build\":{\"package\":\"./cmd/eh_probe\",\"flags\":" +
          Variant.Flags +
          ",\"env\":{\"CGO_ENABLED\":\"0\",\"GO111MODULE\":\"on\","
          "\"GOARCH\":\"amd64\",\"GOFLAGS\":\"\",\"GOOS\":\"linux\","
          "\"GOPROXY\":\"off\",\"GOTOOLCHAIN\":\"local\",\"GOWORK\":\"off\"},"
          "\"execution\":\"" +
          Variant.Execution + "\"},\"evidence\":{\"required_sections\":" +
          Variant.RequiredSections + ",\"pclntab_section\":\"" +
          Variant.PclnTabSection + "\",\"pclntab_at_section_start\":" +
          Variant.AtSectionStart + ",\"pclntab_magic\":" + Variant.Magic +
          ",\"pclntab_min_lc\":" + Variant.MinLC + ",\"pclntab_ptr_size\":" +
          Variant.PointerSize + ",\"pclntab_function_count\":" +
          Variant.FunctionCount + ",\"symbol_table\":\"" +
          Variant.SymbolTable + "\",\"gofunc_symbol\":" +
          Variant.GoFuncSymbol + ",\"native_unwind_sections\":" +
          Variant.NativeUnwindSections +
          "},\"neverd\":{\"validation_level\":\"" + Variant.ValidationLevel +
          "\",\"allowed_parse_status\":" + Variant.AllowedStatuses +
          ",\"expected_pclntab_version\":\"" + Variant.PclnTabVersion +
          "\",\"min_go_functions\":" + Variant.MinGoFunctions +
          ",\"min_defer_sites\":" + Variant.MinDeferSites +
          ",\"min_recover_sites\":" + Variant.MinRecoverSites +
          ",\"min_panic_sites\":" + Variant.MinPanicSites +
          ",\"min_open_coded_defer_funcs\":" + Variant.MinOpenCodedDeferFuncs +
          ",\"requires_moduledata\":" + Variant.RequiresModuleData + "}}]}")
      .str();
}

TEST(GoEHCorpusManifest, AcceptsSchemaV1SingleArtifactForUnitTesting) {
  auto Parsed = parseGoEHCorpusManifest(manifestFor(GoVariant{}), false);

  ASSERT_TRUE(static_cast<bool>(Parsed)) << toString(Parsed.takeError());
  ASSERT_EQ(Parsed->size(), 1u);
  EXPECT_EQ((*Parsed)[0].ExpectedArch, Arch::X64);
  EXPECT_EQ((*Parsed)[0].ExpectedFormat, BinaryFormat::ELF);
  EXPECT_EQ((*Parsed)[0].ValidationLevel,
            GoCorpusValidationLevel::RuntimeGraph);
  EXPECT_EQ((*Parsed)[0].PclnTabMagic, 0xFFFFFFF1u);
  EXPECT_EQ((*Parsed)[0].PclnTabMinLC, 1u);
  EXPECT_TRUE((*Parsed)[0].GoFuncSymbol.empty());
  EXPECT_TRUE((*Parsed)[0].RequiresModuleData);
}

TEST(GoEHCorpusManifest, RejectsUnsafeArtifactPath) {
  GoVariant Variant;
  Variant.Path = "../escape";
  auto Parsed = parseGoEHCorpusManifest(manifestFor(Variant), false);

  ASSERT_FALSE(static_cast<bool>(Parsed));
  EXPECT_NE(toString(Parsed.takeError()).find("not normalized"),
            std::string::npos);
}

// The `pcHeader` magic is the only thing in a Go image that names its own
// layout, so a manifest whose magic and release disagree describes two
// different record shapes at once and cannot be used to check either.
TEST(GoEHCorpusManifest, RejectsMagicThatDisagreesWithTheRelease) {
  GoVariant Variant;
  Variant.Magic = "4294967280";
  auto Parsed = parseGoEHCorpusManifest(manifestFor(Variant), false);

  ASSERT_FALSE(static_cast<bool>(Parsed));
  EXPECT_NE(toString(Parsed.takeError()).find("pclntab magic disagrees"),
            std::string::npos);
}

TEST(GoEHCorpusManifest, RejectsPcQuantumThatDisagreesWithGOARCH) {
  GoVariant Variant;
  Variant.MinLC = "4";
  auto Parsed = parseGoEHCorpusManifest(manifestFor(Variant), false);

  ASSERT_FALSE(static_cast<bool>(Parsed));
  EXPECT_NE(toString(Parsed.takeError()).find("pc quantum disagrees"),
            std::string::npos);
}

TEST(GoEHCorpusManifest, RejectsPclnTabSectionThatDisagreesWithTheBuildMode) {
  GoVariant Variant;
  Variant.PclnTabSection = ".data.rel.ro.gopclntab";
  auto Parsed = parseGoEHCorpusManifest(manifestFor(Variant), false);

  ASSERT_FALSE(static_cast<bool>(Parsed));
  EXPECT_NE(toString(Parsed.takeError()).find("pclntab section disagrees"),
            std::string::npos);
}

// `-s -w` removes every Go name from an ELF or PE image, which is exactly what
// forces structural discovery; a manifest that claims both a stripped link and
// surviving names describes an artifact neither path can be tested against.
TEST(GoEHCorpusManifest, RejectsGoNamesOnAStrippedELF) {
  GoVariant Variant;
  Variant.SymbolTable = "go-names";
  auto Parsed = parseGoEHCorpusManifest(manifestFor(Variant), false);

  ASSERT_FALSE(static_cast<bool>(Parsed));
  EXPECT_NE(toString(Parsed.takeError()).find("symbol table kind disagrees"),
            std::string::npos);
}

// `-N` clears `ssagen.hasOpenDefers` for every function, so an unoptimized
// image has no frame carrying `FUNCDATA_OpenCodedDeferInfo` to find.
TEST(GoEHCorpusManifest, RejectsOpenCodedDeferClaimWithoutOptimization) {
  GoVariant Variant;
  Variant.Path = "corpus/go-eh/go1.26.5/linux-amd64/exe/cgo0/stripped/noopt/"
                 "eh_probe-go1.26.5-linux-amd64-exe-cgo0-stripped-noopt";
  Variant.Optimization = "none";
  Variant.Flags = R"(["-trimpath","-buildmode=exe","-gcflags=all=-N -l",)"
                  R"("-ldflags=-s -w"])";
  auto Parsed = parseGoEHCorpusManifest(manifestFor(Variant), false);

  ASSERT_FALSE(static_cast<bool>(Parsed));
  EXPECT_NE(toString(Parsed.takeError()).find("open-coded defer claim"),
            std::string::npos);
}

TEST(GoEHCorpusManifest, RejectsCorpusThatCoversOneGeneration) {
  auto Parsed = parseGoEHCorpusManifest(manifestFor(GoVariant{}), true);

  ASSERT_FALSE(static_cast<bool>(Parsed));
  EXPECT_NE(toString(Parsed.takeError()).find("every pclntab generation"),
            std::string::npos);
}

} // namespace
