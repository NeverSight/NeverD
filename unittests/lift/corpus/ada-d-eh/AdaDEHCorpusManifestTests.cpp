//===- AdaDEHCorpusManifestTests.cpp - Corpus contract tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "AdaDEHCorpusManifest.h"
#include "gtest/gtest.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"

#include <string>

using namespace llvm;
using namespace neverd;
using namespace neverd::test;

namespace {

struct Cell {
  StringRef Path =
      "corpus/ada-d-eh/ada/gnat/x86_64-linux-gnu/o0/"
      "ada_eh_probe-gnat-x86_64-linux-gnu-o0";
  StringRef Toolchain = "gnat";
  StringRef Target = "x86_64-linux-gnu";
  StringRef Architecture = "x86_64";
  StringRef SourceLanguage = "ada";
  StringRef Optimization = "o0";
  StringRef Execution = "passed";
  StringRef Personality = "__gnat_personality_v0";
  StringRef DescriptorABI = "gnat-exception-id";
  StringRef MinCleanupPads = "0";
};

Cell dmdCell() {
  Cell C;
  C.Path = "corpus/ada-d-eh/d/dmd/x86_64-linux-gnu/o0/"
           "d_eh_probe-dmd-x86_64-linux-gnu-o0";
  C.Toolchain = "dmd";
  C.SourceLanguage = "d";
  C.Personality = "__dmd_personality_v0";
  C.DescriptorABI = "d-classinfo";
  C.MinCleanupPads = "1";
  return C;
}

std::string artifactJSON(const Cell &C) {
  return (Twine("{\"path\":\"") + C.Path + "\",\"sha256\":\"" +
          std::string(64, 'a') + "\",\"size\":1024,\"toolchain\":\"" +
          C.Toolchain + "\",\"toolchain_version\":\"13.3.0\",\"target\":\"" +
          C.Target + "\",\"architecture\":\"" + C.Architecture +
          "\",\"object_format\":\"elf\",\"source_language\":\"" +
          C.SourceLanguage + "\",\"optimization\":\"" + C.Optimization +
          "\",\"execution\":\"" + C.Execution +
          "\",\"exception_model\":\"itanium-dwarf\","
          "\"evidence\":{\"required_sections\":[\".eh_frame\","
          "\".gcc_except_table\"],\"required_symbols\":[\"" +
          C.Personality +
          "\"],\"required_strings\":[\"ada-d-eh probe passed\"],"
          "\"require_unwind_tables\":true,\"eh_frame_present\":true,"
          "\"checkout_path_absent\":true},"
          "\"neverd\":{\"validation_level\":\"lsda-graph\","
          "\"personalities_any\":[\"" +
          C.Personality +
          "\"],\"type_table_interpretation\":\"opaque-descriptor\","
          "\"descriptor_abi\":\"" +
          C.DescriptorABI +
          "\",\"native_reconstruction\":\"address-clauses\","
          "\"corpus_proven\":true,\"min_call_sites\":1,\"min_landing_pads\":1,"
          "\"min_catch_clauses\":3,\"min_cleanup_pads\":" +
          C.MinCleanupPads + ",\"min_type_table_entries\":3}}")
      .str();
}

std::string manifestFor(const Cell &C) {
  return (Twine("{\"schema_version\":1,\"corpus\":\"ada-d-eh\",\"artifacts\":[") +
          artifactJSON(C) + "]}")
      .str();
}

Expected<std::vector<AdaDEHArtifactExpectation>> parse(const Cell &C) {
  return parseAdaDEHCorpusManifest(manifestFor(C), false);
}

void expectRejected(const Cell &C, const char *Because) {
  auto Result = parse(C);
  if (Result) {
    ADD_FAILURE() << "accepted a manifest that " << Because;
    return;
  }
  consumeError(Result.takeError());
}

} // namespace

TEST(AdaDEHCorpusManifest, ReadsAdaAndDPersonalities) {
  auto Ada = parse(Cell());
  ASSERT_TRUE(static_cast<bool>(Ada)) << toString(Ada.takeError());
  ASSERT_EQ(Ada->size(), 1u);
  EXPECT_EQ(Ada->front().ExpectedFormat, BinaryFormat::ELF);
  EXPECT_EQ(Ada->front().ExpectedArch, Arch::X64);
  EXPECT_EQ(Ada->front().Personalities.front(), "__gnat_personality_v0");
  EXPECT_EQ(Ada->front().DescriptorABI, AdaDDescriptorABI::GnatExceptionId);

  auto D = parse(dmdCell());
  ASSERT_TRUE(static_cast<bool>(D)) << toString(D.takeError());
  ASSERT_EQ(D->size(), 1u);
  EXPECT_EQ(D->front().Personalities.front(), "__dmd_personality_v0");
  EXPECT_EQ(D->front().DescriptorABI, AdaDDescriptorABI::DClassInfo);
}

TEST(AdaDEHCorpusManifest, RejectsACXXRTTIInterpretation) {
  std::string Text = manifestFor(Cell());
  const std::string From = "\"type_table_interpretation\":\"opaque-descriptor\"";
  const std::string To = "\"type_table_interpretation\":\"cxx-rtti\"";
  const auto Pos = Text.find(From);
  ASSERT_NE(Pos, std::string::npos);
  Text.replace(Pos, From.size(), To);
  auto Result = parseAdaDEHCorpusManifest(Text, false);
  ASSERT_FALSE(static_cast<bool>(Result));
  consumeError(Result.takeError());
}

TEST(AdaDEHCorpusManifest, RejectsAPersonalityThatDoesNotMatchTheToolchain) {
  Cell C;
  C.Personality = "__gxx_personality_v0";
  expectRejected(C, "installed a C++ personality on a GNAT cell");
}

TEST(AdaDEHCorpusManifest, RejectsAnIncompleteMatrix) {
  auto Result = parseAdaDEHCorpusManifest(manifestFor(Cell()), true);
  ASSERT_FALSE(static_cast<bool>(Result));
  consumeError(Result.takeError());
}
