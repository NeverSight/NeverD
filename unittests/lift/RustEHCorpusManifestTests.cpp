//===- RustEHCorpusManifestTests.cpp - Corpus contract tests -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RustEHCorpusManifest.h"
#include "gtest/gtest.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <string>

using namespace llvm;
using namespace neverd;
using namespace neverd::test;

namespace {

const StringRef kItaniumPath =
    "corpus/rust-eh/x86_64-unknown-linux-gnu/unwind/o0/bin/"
    "rust_eh_probe-x86_64-unknown-linux-gnu-unwind-o0";
const StringRef kMSVCPath =
    "corpus/rust-eh/x86_64-pc-windows-msvc/unwind/o0/bin/"
    "rust_eh_probe-x86_64-pc-windows-msvc-unwind-o0.exe";

/// One artifact's axes, defaulted to the Itanium cell the producer matrix
/// writes.  A test bends exactly the axis it is about and leaves the rest as
/// the matrix would have spelled it, so a rejection can only come from that
/// axis and not from a hand-written manifest being incidentally wrong.
struct RustCell {
  StringRef Path = kItaniumPath;
  StringRef Architecture = "x86_64";
  StringRef ObjectFormat = "elf";
  StringRef TargetTriple = "x86_64-unknown-linux-gnu";
  StringRef PanicStrategy = "unwind";
  StringRef Optimization = "o0";
  StringRef Execution = "passed";
  StringRef RequiredSections = R"([".text",".eh_frame",".gcc_except_table"])";
  StringRef RequiredSymbols = R"(["_Unwind_RaiseException","rust_eh_personality"])";
  StringRef ForbiddenSymbols = "[]";
  StringRef RequiredStrings = "[]";
  StringRef SymbolNamesExpected = "true";
  StringRef ValidationLevel = "panic-graph";
  StringRef AllowedStatuses = R"(["complete"])";
  StringRef Personality = "rust_eh_personality";
  StringRef ExpectNoLandingPads = "false";
  StringRef MinLandingPads = "4";
  StringRef MinDropGluePads = "2";
  StringRef MinCatchUnwindPads = "1";
  StringRef MinNoUnwindGuardPads = "1";
  StringRef MinPanicSites = "3";
};

/// The MSVC cell, whose contract is narrower on purpose: a Rust frame there is
/// spelled with the same tables as a C++ one, so only the `catch_unwind` pad a
/// `rust_panic` catch names is attributable to Rust.
RustCell msvcCell() {
  RustCell Cell;
  Cell.Path = kMSVCPath;
  Cell.ObjectFormat = "pe";
  Cell.TargetTriple = "x86_64-pc-windows-msvc";
  Cell.RequiredSections = R"([".text",".pdata",".rdata"])";
  Cell.RequiredSymbols = "[]";
  Cell.RequiredStrings = R"(["rust_panic"])";
  Cell.SymbolNamesExpected = "false";
  Cell.Personality = "__CxxFrameHandler3";
  Cell.MinLandingPads = "1";
  Cell.MinDropGluePads = "0";
  Cell.MinNoUnwindGuardPads = "0";
  Cell.MinPanicSites = "0";
  return Cell;
}

std::string manifestFor(const RustCell &Cell) {
  return ("{\"schema_version\":1,\"corpus\":\"rust-eh\",\"artifacts\":[{"
          "\"path\":\"" +
          Cell.Path + "\",\"sha256\":\"" + std::string(64, 'a') +
          "\",\"size\":1024,\"architecture\":\"" + Cell.Architecture +
          "\",\"object_format\":\"" + Cell.ObjectFormat +
          "\",\"target_triple\":\"" + Cell.TargetTriple +
          "\",\"crate_name\":\"rust_eh_probe\",\"crate_type\":\"bin\","
          "\"panic_strategy\":\"" +
          Cell.PanicStrategy + "\",\"optimization\":\"" + Cell.Optimization +
          "\",\"execution\":\"" + Cell.Execution +
          "\",\"build\":{\"edition\":\"2024\",\"linker\":\"rustc-default\","
          "\"rustc_flags\":[\"--target\",\"" +
          Cell.TargetTriple + "\",\"-C\",\"panic=" + Cell.PanicStrategy +
          "\"],\"rustc_host\":\"x86_64-unknown-linux-gnu\","
          "\"runner_image\":\"test\",\"runner_os\":\"linux\","
          "\"runner_arch\":\"x64\"},"
          "\"evidence\":{\"required_sections\":" +
          Cell.RequiredSections + ",\"required_symbols\":" +
          Cell.RequiredSymbols + ",\"forbidden_symbols\":" +
          Cell.ForbiddenSymbols + ",\"required_strings\":" +
          Cell.RequiredStrings +
          ",\"require_unwind_tables\":true,\"symbol_names_expected\":" +
          Cell.SymbolNamesExpected +
          "},"
          "\"neverd\":{\"validation_level\":\"" +
          Cell.ValidationLevel + "\",\"allowed_parse_status\":" +
          Cell.AllowedStatuses + ",\"personalities_any\":[\"" +
          Cell.Personality + "\"],\"expect_no_landing_pads\":" +
          Cell.ExpectNoLandingPads +
          ",\"landing_pad_free_symbols\":[\"rust_eh_c_leaf_nounwind\"],"
          "\"min_landing_pads\":" +
          Cell.MinLandingPads + ",\"min_drop_glue_pads\":" +
          Cell.MinDropGluePads + ",\"min_catch_unwind_pads\":" +
          Cell.MinCatchUnwindPads + ",\"min_nounwind_guard_pads\":" +
          Cell.MinNoUnwindGuardPads + ",\"min_panic_sites\":" +
          Cell.MinPanicSites + "}}]}")
      .str();
}

TEST(RustEHCorpusManifest, AcceptsSchemaV1SingleArtifactForUnitTesting) {
  auto Parsed = parseRustEHCorpusManifest(manifestFor(RustCell{}), false);

  ASSERT_TRUE(static_cast<bool>(Parsed)) << toString(Parsed.takeError());
  ASSERT_EQ(Parsed->size(), 1u);
  EXPECT_EQ((*Parsed)[0].ExpectedArch, Arch::X64);
  EXPECT_EQ((*Parsed)[0].ExpectedFormat, BinaryFormat::ELF);
  EXPECT_EQ((*Parsed)[0].ValidationLevel,
            RustCorpusValidationLevel::PanicGraph);
  EXPECT_EQ((*Parsed)[0].MinCatchUnwindPads, 1u);
  EXPECT_TRUE((*Parsed)[0].SymbolNamesExpected);
}

// The MSVC cell is not a degraded Itanium one: its lower minimums are what the
// target makes provable, so a parser that normalized them away would let a
// Windows artifact pass a contract it was never given.
TEST(RustEHCorpusManifest, AcceptsTheNarrowerMSVCContract) {
  auto Parsed = parseRustEHCorpusManifest(manifestFor(msvcCell()), false);

  ASSERT_TRUE(static_cast<bool>(Parsed)) << toString(Parsed.takeError());
  ASSERT_EQ(Parsed->size(), 1u);
  EXPECT_EQ((*Parsed)[0].ExpectedFormat, BinaryFormat::COFF);
  EXPECT_EQ((*Parsed)[0].MinDropGluePads, 0u);
  EXPECT_FALSE((*Parsed)[0].SymbolNamesExpected);
}

TEST(RustEHCorpusManifest, RejectsUnsafeArtifactPath) {
  RustCell Cell;
  Cell.Path = "../escape";
  auto Parsed = parseRustEHCorpusManifest(manifestFor(Cell), false);

  ASSERT_FALSE(static_cast<bool>(Parsed));
  EXPECT_NE(toString(Parsed.takeError()).find("not normalized"),
            std::string::npos);
}

TEST(RustEHCorpusManifest, RejectsPathThatDisagreesWithBuildAxes) {
  RustCell Cell;
  Cell.Path = "corpus/rust-eh/x86_64-unknown-linux-gnu/unwind/o2/bin/"
              "rust_eh_probe-x86_64-unknown-linux-gnu-unwind-o2";
  auto Parsed = parseRustEHCorpusManifest(manifestFor(Cell), false);

  ASSERT_FALSE(static_cast<bool>(Parsed));
  EXPECT_NE(toString(Parsed.takeError()).find("disagrees with build axes"),
            std::string::npos);
}

TEST(RustEHCorpusManifest, RejectsAbortingCellThatKeepsAPanicGraph) {
  RustCell Cell;
  Cell.Path = "corpus/rust-eh/x86_64-unknown-linux-gnu/abort/o0/bin/"
              "rust_eh_probe-x86_64-unknown-linux-gnu-abort-o0";
  Cell.PanicStrategy = "abort";
  auto Parsed = parseRustEHCorpusManifest(manifestFor(Cell), false);

  ASSERT_FALSE(static_cast<bool>(Parsed));
  EXPECT_NE(toString(Parsed.takeError()).find("validation level disagrees"),
            std::string::npos);
}

TEST(RustEHCorpusManifest, RejectsItaniumPersonalityOnAnMSVCTarget) {
  RustCell Cell = msvcCell();
  Cell.Personality = "rust_eh_personality";
  auto Parsed = parseRustEHCorpusManifest(manifestFor(Cell), false);

  ASSERT_FALSE(static_cast<bool>(Parsed));
  EXPECT_NE(toString(Parsed.takeError()).find("personality disagrees"),
            std::string::npos);
}

TEST(RustEHCorpusManifest, RejectsMSVCContractThatClaimsDropGlue) {
  RustCell Cell = msvcCell();
  Cell.MinDropGluePads = "1";
  auto Parsed = parseRustEHCorpusManifest(manifestFor(Cell), false);

  ASSERT_FALSE(static_cast<bool>(Parsed));
  EXPECT_NE(toString(Parsed.takeError()).find("Itanium-only classification"),
            std::string::npos);
}

TEST(RustEHCorpusManifest, RejectsPartialTargetMatrix) {
  auto Parsed = parseRustEHCorpusManifest(manifestFor(RustCell{}), true);

  ASSERT_FALSE(static_cast<bool>(Parsed));
  EXPECT_NE(toString(Parsed.takeError()).find("complete target matrix"),
            std::string::npos);
}

} // namespace
