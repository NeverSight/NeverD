//===- CxxItaniumEHCorpusManifestTests.cpp - Corpus contract tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CxxItaniumEHCorpusManifest.h"
#include "gtest/gtest.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <string>

using namespace llvm;
using namespace neverd;
using namespace neverd::test;

namespace {

/// One artifact's axes, defaulted to the x86-64 ELF cell the producer matrix
/// writes.  A test bends exactly the axis it is about and leaves the rest as
/// the matrix would have spelled it, so a rejection can only come from that
/// axis and not from a hand-written manifest being incidentally wrong.
struct Cell {
  StringRef Path = "corpus/cxx-itanium-eh/gcc/x86_64-linux-gnu/o0/symtab/exe/"
                   "cxx_eh_probe-gcc-x86_64-linux-gnu-o0-symtab";
  StringRef Toolchain = "gcc";
  StringRef Target = "x86_64-linux-gnu";
  StringRef Architecture = "x86_64";
  StringRef ObjectFormat = "elf";
  StringRef Program = "cxx_eh_probe";
  StringRef ArtifactKind = "exe";
  StringRef SourceLanguage = "cxx";
  StringRef Exceptions = "on";
  StringRef Optimization = "o0";
  StringRef Stripped = "false";
  StringRef Execution = "passed";
  StringRef CompilerFlags = R"(["-std=c++17","-O0","-fexceptions"])";
  StringRef RequiredSections =
      R"([".text",".eh_frame",".eh_frame_hdr",".gcc_except_table"])";
  StringRef ForbiddenSections = "[]";
  StringRef RequiredSymbols = "[]";
  StringRef ForbiddenSymbols = "[]";
  StringRef RequiredStrings = R"(["15CxxEhProbeError"])";
  StringRef SymbolNamesExpected = "true";
  StringRef EhFramePresent = "true";
  StringRef ArmExidxPresent = "false";
  StringRef MinArmExidxEntries = "0";
  StringRef RequireUnwindTables = "true";
  StringRef ValidationLevel = "lsda-graph";
  StringRef Personalities = R"(["__gxx_personality_v0"])";
  StringRef ExpectNoLSDA = "false";
  StringRef ExpectArmEHABI = "false";
  StringRef MinCallSites = "10";
  StringRef MinLandingPads = "6";
  StringRef MinCatchClauses = "4";
  StringRef MinCleanupPads = "3";
  StringRef MinTypeTableEntries = "2";
};

/// The ARM cell, whose container is the reason this product line exists: EHABI
/// replaces the DWARF chain with an index, so what it claims and what it
/// refuses to claim both differ from every other target.
Cell armCell() {
  Cell C;
  C.Path = "corpus/cxx-itanium-eh/gcc/armv7-linux-gnueabihf/o0/symtab/exe/"
           "cxx_eh_probe-gcc-armv7-linux-gnueabihf-o0-symtab";
  C.Target = "armv7-linux-gnueabihf";
  C.Architecture = "arm";
  C.Execution = "not-run-cross-target";
  C.RequiredSections = R"([".text",".ARM.exidx",".ARM.extab"])";
  C.EhFramePresent = "false";
  C.ArmExidxPresent = "true";
  C.MinArmExidxEntries = "8";
  C.RequireUnwindTables = "false";
  C.ValidationLevel = "ehabi";
  C.Personalities = R"(["__gxx_personality_v0","__aeabi_unwind_cpp_pr0",)"
                    R"("__aeabi_unwind_cpp_pr1"])";
  C.ExpectArmEHABI = "true";
  return C;
}

/// The mingw cell: Itanium language semantics reached through Windows SEH, so
/// the personality is spelled with the `seh0` suffix and the tables are
/// `.pdata`/`.xdata`.
Cell mingwCell() {
  Cell C;
  C.Path = "corpus/cxx-itanium-eh/gcc/x86_64-w64-mingw32/o0/symtab/exe/"
           "cxx_eh_probe-gcc-x86_64-w64-mingw32-o0-symtab.exe";
  C.Target = "x86_64-w64-mingw32";
  C.ObjectFormat = "pe";
  C.Execution = "not-run-cross-target";
  C.RequiredSections = R"([".text",".pdata",".xdata"])";
  C.EhFramePresent = "false";
  C.Personalities = R"(["__gxx_personality_seh0"])";
  return C;
}

/// The exception-free control, which is a negative control rather than a
/// weaker probe: the same source compiled with `-fno-exceptions` keeps its
/// unwind tables and has no language data at all.
Cell controlCell() {
  Cell C;
  C.Path = "corpus/cxx-itanium-eh/gcc/x86_64-linux-gnu/o2/symtab/exe/"
           "cxx_eh_probe_noexc-gcc-x86_64-linux-gnu-o2-symtab";
  C.Program = "cxx_eh_probe_noexc";
  C.Exceptions = "off";
  C.Optimization = "o2";
  C.CompilerFlags = R"(["-std=c++17","-O2","-fno-exceptions"])";
  C.RequiredSections = R"([".text",".eh_frame",".eh_frame_hdr"])";
  C.RequiredStrings = "[]";
  C.ValidationLevel = "cfi-only";
  C.Personalities = "[]";
  C.ExpectNoLSDA = "true";
  C.MinCallSites = "0";
  C.MinLandingPads = "0";
  C.MinCatchClauses = "0";
  C.MinCleanupPads = "0";
  C.MinTypeTableEntries = "0";
  return C;
}

/// The C probe, which the corpus carries because a C frame runs cleanups and
/// names no type at all.
Cell cProbeCell() {
  Cell C;
  C.Path = "corpus/cxx-itanium-eh/gcc/x86_64-linux-gnu/o2/symtab/exe/"
           "c_eh_probe-gcc-x86_64-linux-gnu-o2-symtab";
  C.Program = "c_eh_probe";
  C.SourceLanguage = "c";
  C.Optimization = "o2";
  C.CompilerFlags = R"(["-std=c11","-O2","-fexceptions"])";
  // A C frame raises through the shared library beside it and names no type of
  // its own, so there is nothing here for a stripped build to be found by.
  C.RequiredStrings = "[]";
  C.Personalities = R"(["__gcc_personality_v0"])";
  C.MinCallSites = "1";
  C.MinLandingPads = "1";
  C.MinCatchClauses = "0";
  C.MinCleanupPads = "1";
  C.MinTypeTableEntries = "0";
  return C;
}

std::string manifestFor(const Cell &C) {
  return ("{\"schema_version\":1,\"corpus\":\"cxx-itanium-eh\",\"artifacts\":[{"
          "\"path\":\"" +
          C.Path + "\",\"sha256\":\"" + std::string(64, 'a') +
          "\",\"size\":1024,\"toolchain\":\"" + C.Toolchain +
          "\",\"toolchain_version\":\"13.3.0\",\"target\":\"" + C.Target +
          "\",\"architecture\":\"" + C.Architecture +
          "\",\"object_format\":\"" + C.ObjectFormat + "\",\"program\":\"" +
          C.Program + "\",\"artifact_kind\":\"" + C.ArtifactKind +
          "\",\"source_language\":\"" + C.SourceLanguage +
          "\",\"exceptions\":\"" + C.Exceptions + "\",\"optimization\":\"" +
          C.Optimization + "\",\"stripped\":" + C.Stripped +
          ",\"execution\":\"" + C.Execution +
          "\",\"build\":{\"compiler\":\"g++\",\"compiler_flags\":" +
          C.CompilerFlags +
          ",\"environment\":{\"LC_ALL\":\"C\"},\"linked_artifacts\":[],"
          "\"runner_arch\":\"x64\",\"runner_image\":\"test\","
          "\"runner_os\":\"linux\",\"strip_tool\":\"strip\"},"
          "\"evidence\":{\"required_sections\":" +
          C.RequiredSections + ",\"forbidden_sections\":" +
          C.ForbiddenSections + ",\"required_symbols\":" + C.RequiredSymbols +
          ",\"forbidden_symbols\":" + C.ForbiddenSymbols +
          ",\"required_strings\":" + C.RequiredStrings +
          ",\"symbol_names_expected\":" + C.SymbolNamesExpected +
          ",\"eh_frame_present\":" + C.EhFramePresent +
          ",\"arm_exidx_present\":" + C.ArmExidxPresent +
          ",\"min_arm_exidx_entries\":" + C.MinArmExidxEntries +
          ",\"require_unwind_tables\":" + C.RequireUnwindTables +
          "},\"neverd\":{\"validation_level\":\"" + C.ValidationLevel +
          "\",\"personalities_any\":" + C.Personalities +
          ",\"expect_no_lsda\":" + C.ExpectNoLSDA + ",\"expect_arm_ehabi\":" +
          C.ExpectArmEHABI + ",\"min_call_sites\":" + C.MinCallSites +
          ",\"min_landing_pads\":" + C.MinLandingPads +
          ",\"min_catch_clauses\":" + C.MinCatchClauses +
          ",\"min_cleanup_pads\":" + C.MinCleanupPads +
          ",\"min_type_table_entries\":" + C.MinTypeTableEntries + "}}]}")
      .str();
}

Expected<std::vector<CxxItaniumEHArtifactExpectation>> parse(const Cell &C) {
  return parseCxxItaniumEHCorpusManifest(manifestFor(C), false);
}

void expectRejected(const Cell &C, const char *Because) {
  auto Result = parse(C);
  if (Result) {
    ADD_FAILURE() << "accepted a manifest that " << Because;
    return;
  }
  consumeError(Result.takeError());
}

TEST(CxxItaniumEHCorpusManifest, ReadsEveryContainerTheMatrixBuilds) {
  const Cell Cells[] = {Cell(), armCell(), mingwCell(), controlCell(),
                        cProbeCell()};
  const BinaryFormat Formats[] = {BinaryFormat::ELF, BinaryFormat::ELF,
                                  BinaryFormat::COFF, BinaryFormat::ELF,
                                  BinaryFormat::ELF};
  const Arch Architectures[] = {Arch::X64, Arch::ARM, Arch::X64, Arch::X64,
                                Arch::X64};
  for (size_t I = 0; I < std::size(Cells); ++I) {
    SCOPED_TRACE(Cells[I].Path.str());
    auto Result = parse(Cells[I]);
    ASSERT_TRUE(static_cast<bool>(Result)) << toString(Result.takeError());
    ASSERT_EQ(Result->size(), 1u);
    EXPECT_EQ(Result->front().ExpectedFormat, Formats[I]);
    EXPECT_EQ(Result->front().ExpectedArch, Architectures[I]);
  }
}

TEST(CxxItaniumEHCorpusManifest, NamesTheValidationLevelEachTargetEarns) {
  auto Lsda = parse(Cell());
  ASSERT_TRUE(static_cast<bool>(Lsda)) << toString(Lsda.takeError());
  EXPECT_EQ(Lsda->front().ValidationLevel,
            CxxItaniumCorpusValidationLevel::LsdaGraph);

  auto Ehabi = parse(armCell());
  ASSERT_TRUE(static_cast<bool>(Ehabi)) << toString(Ehabi.takeError());
  EXPECT_EQ(Ehabi->front().ValidationLevel,
            CxxItaniumCorpusValidationLevel::Ehabi);

  auto Cfi = parse(controlCell());
  ASSERT_TRUE(static_cast<bool>(Cfi)) << toString(Cfi.takeError());
  EXPECT_EQ(Cfi->front().ValidationLevel,
            CxxItaniumCorpusValidationLevel::CfiOnly);

  EXPECT_EQ(getCxxItaniumValidationLevelName(
                CxxItaniumCorpusValidationLevel::LsdaGraph),
            "lsda-graph");
  EXPECT_EQ(
      getCxxItaniumValidationLevelName(CxxItaniumCorpusValidationLevel::Ehabi),
      "ehabi");
  EXPECT_EQ(getCxxItaniumValidationLevelName(
                CxxItaniumCorpusValidationLevel::CfiOnly),
            "cfi-only");
}

// The level is derived from the target and the exception setting, so a
// manifest that states one the axes do not imply describes a build nobody
// made.  Reading it would silently ask an ARM artifact for a graph that is not
// in it, or excuse an x86-64 one from the graph that is.
TEST(CxxItaniumEHCorpusManifest, RejectsAValidationLevelTheAxesDoNotImply) {
  Cell C;
  C.ValidationLevel = "ehabi";
  expectRejected(C, "claims EHABI on an x86-64 target");

  Cell Arm = armCell();
  Arm.ValidationLevel = "lsda-graph";
  expectRejected(Arm, "claims an LSDA graph on an EHABI target");

  Cell Control = controlCell();
  Control.ValidationLevel = "lsda-graph";
  expectRejected(Control, "claims a graph in an exception-free build");
}

TEST(CxxItaniumEHCorpusManifest, RejectsAContainerClaimTheTargetContradicts) {
  Cell C;
  C.ArmExidxPresent = "true";
  expectRejected(C, "claims an EHABI index on an x86-64 target");

  Cell Arm = armCell();
  Arm.EhFramePresent = "true";
  expectRejected(Arm, "claims a DWARF frame chain on an EHABI target");

  Cell Arm2 = armCell();
  Arm2.MinArmExidxEntries = "0";
  expectRejected(Arm2, "claims no EHABI index floor on an EHABI target");

  Cell Arm3 = armCell();
  Arm3.RequireUnwindTables = "true";
  expectRejected(Arm3, "asks for unwind tables EHABI does not carry");
}

TEST(CxxItaniumEHCorpusManifest, RejectsAPersonalityTheTargetDoesNotInstall) {
  Cell C;
  C.Personalities = R"(["__gxx_personality_seh0"])";
  expectRejected(C, "names the mingw personality on a Linux target");

  Cell CProbe = cProbeCell();
  CProbe.Personalities = R"(["__gxx_personality_v0"])";
  expectRejected(CProbe, "names the C++ personality for a C frame");

  Cell Arm = armCell();
  Arm.Personalities = R"(["__gxx_personality_v0"])";
  expectRejected(Arm, "omits the EHABI compact-model personalities");

  Cell Control = controlCell();
  Control.Personalities = R"(["__gxx_personality_v0"])";
  expectRejected(Control, "names a personality an exception-free build omits");
}

// The control exists to be a build that cannot name a handler.  A minimum
// above zero on it would make it a weaker probe instead, and the corpus would
// have no negative control at all.
TEST(CxxItaniumEHCorpusManifest, RejectsRecoveredStateInTheControl) {
  Cell C = controlCell();
  C.MinCatchClauses = "1";
  expectRejected(C, "claims a catch in an exception-free build");

  Cell C2 = controlCell();
  C2.ExpectNoLSDA = "false";
  expectRejected(C2, "claims language data in an exception-free build");

  Cell C3 = controlCell();
  C3.RequiredStrings = R"(["15CxxEhProbeError"])";
  expectRejected(C3, "requires a thrown type a control never throws");
}

// A C frame reaches a pad through cleanup actions and has no type table at
// all, which is the whole reason the corpus carries one.  A contract that
// claims a catch for it would be satisfied by a reader that could not tell C
// from C++.
TEST(CxxItaniumEHCorpusManifest, RejectsACatchClaimedForACFrame) {
  Cell C = cProbeCell();
  C.MinCatchClauses = "1";
  expectRejected(C, "claims a catch in a C frame");

  Cell C2 = cProbeCell();
  C2.MinTypeTableEntries = "1";
  expectRejected(C2, "claims a type table in a C frame");

  Cell C3 = cProbeCell();
  C3.RequiredStrings = R"(["15CxxEhProbeError"])";
  expectRejected(C3, "requires a thrown type a C frame does not define");
}

TEST(CxxItaniumEHCorpusManifest, RejectsAPathThatDisagreesWithItsAxes) {
  Cell C;
  C.Optimization = "o2";
  expectRejected(C, "records an optimization its path does not spell");

  Cell C2;
  C2.Stripped = "true";
  expectRejected(C2, "records a strip axis its path does not spell");

  Cell C3;
  C3.Path = "../escape";
  expectRejected(C3, "leaves the corpus root");
}

TEST(CxxItaniumEHCorpusManifest, RejectsAShapeThatDisagreesWithItsProgram) {
  Cell C;
  C.SourceLanguage = "c";
  expectRejected(C, "calls the C++ probe a C program");

  Cell C2;
  C2.Exceptions = "off";
  expectRejected(C2, "compiles the throwing probe without exceptions");

  Cell C3;
  C3.ArtifactKind = "shared";
  expectRejected(C3, "calls an executable a shared library");
}

// The setting is a compiler flag before it is an axis, so a cell whose flags
// and recorded setting disagree built something other than what the rest of
// the contract describes.
TEST(CxxItaniumEHCorpusManifest,
     RejectsFlagsThatDoNotSelectTheRecordedSetting) {
  Cell C;
  C.CompilerFlags = R"(["-std=c++17","-O0","-fno-exceptions"])";
  expectRejected(C, "records exceptions its flags disable");

  Cell C2 = controlCell();
  C2.CompilerFlags = R"(["-std=c++17","-O2","-fexceptions"])";
  expectRejected(C2, "records a control its flags do not build");
}

TEST(CxxItaniumEHCorpusManifest, RejectsASymbolClaimTheStripAxisContradicts) {
  Cell C;
  C.SymbolNamesExpected = "false";
  expectRejected(C, "expects no names from an unstripped artifact");
}

TEST(CxxItaniumEHCorpusManifest, RejectsAnIncompleteMatrix) {
  auto Result = parseCxxItaniumEHCorpusManifest(manifestFor(Cell()), true);
  EXPECT_FALSE(static_cast<bool>(Result))
      << "one artifact is not the 72 the matrix builds";
  consumeError(Result.takeError());
}

TEST(CxxItaniumEHCorpusManifest, RejectsAnotherCorpusUnderThisReader) {
  auto Result = parseCxxItaniumEHCorpusManifest(
      R"({"schema_version":1,"corpus":"rust-eh","artifacts":[]})", false);
  EXPECT_FALSE(static_cast<bool>(Result));
  consumeError(Result.takeError());
}

} // namespace
