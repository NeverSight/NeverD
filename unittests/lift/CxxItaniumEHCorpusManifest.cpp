//===- CxxItaniumEHCorpusManifest.cpp - Corpus manifest reader ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CxxItaniumEHCorpusManifest.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

using namespace llvm;

namespace neverd::test {
namespace {

/// Everything the target name alone decides.  One C++ ABI reaches three
/// different unwinders here, and which one a target uses is what the rest of
/// an artifact's contract has to agree with.
struct TargetIdentity {
  Arch TheArch;
  StringRef Architecture;
  StringRef ObjectFormat;
  BinaryFormat Format;
  StringRef ExeExtension;
  StringRef SharedExtension;
  /// True when the hosted runner that builds this cell can also run its
  /// executable, which is what separates `passed` from `not-run-cross-target`.
  bool Native;
};

/// Everything the program name alone decides.  A variant is one program at one
/// optimization level with symbols kept or removed; nothing else varies.
struct ProgramIdentity {
  StringRef ArtifactKind;
  StringRef SourceLanguage;
  StringRef Exceptions;
};

Error manifestError(const Twine &Message) {
  return make_error<StringError>(Message, inconvertibleErrorCode());
}

Expected<std::string> requireString(const json::Object &Object, StringRef Key,
                                    StringRef Context) {
  std::optional<StringRef> Value = Object.getString(Key);
  if (!Value || Value->empty())
    return manifestError(Context + ": missing string '" + Key + "'");
  return Value->str();
}

Expected<bool> requireBoolean(const json::Object &Object, StringRef Key,
                              StringRef Context) {
  std::optional<bool> Value = Object.getBoolean(Key);
  if (!Value)
    return manifestError(Context + ": missing boolean '" + Key + "'");
  return *Value;
}

Expected<uint64_t> requireNonNegativeInteger(const json::Object &Object,
                                             StringRef Key, StringRef Context) {
  std::optional<int64_t> Value = Object.getInteger(Key);
  if (!Value || *Value < 0)
    return manifestError(Context + ": missing non-negative integer '" + Key +
                         "'");
  return static_cast<uint64_t>(*Value);
}

Expected<std::vector<std::string>>
requireStringArray(const json::Object &Object, StringRef Key, StringRef Context,
                   bool AllowEmpty) {
  const json::Array *Values = Object.getArray(Key);
  if (!Values || (!AllowEmpty && Values->empty()))
    return manifestError(Context + ": invalid string array '" + Key + "'");
  std::vector<std::string> Result;
  Result.reserve(Values->size());
  for (const json::Value &Value : *Values) {
    std::optional<StringRef> String = Value.getAsString();
    if (!String || String->empty())
      return manifestError(Context + ": non-string value in '" + Key + "'");
    Result.push_back(String->str());
  }
  return Result;
}

std::optional<TargetIdentity> getTargetIdentity(StringRef Target) {
  if (Target == "x86_64-linux-gnu")
    return TargetIdentity{Arch::X64,  "x86_64", "elf", BinaryFormat::ELF,
                          "",         ".so",    true};
  if (Target == "aarch64-linux-gnu")
    return TargetIdentity{Arch::AArch64, "aarch64", "elf", BinaryFormat::ELF,
                          "",            ".so",     false};
  if (Target == "armv7-linux-gnueabihf")
    return TargetIdentity{Arch::ARM, "arm", "elf", BinaryFormat::ELF,
                          "",        ".so", false};
  if (Target == "x86_64-w64-mingw32")
    return TargetIdentity{Arch::X64, "x86_64", "pe",  BinaryFormat::COFF,
                          ".exe",    ".dll",   false};
  if (Target == "x86_64-apple-darwin")
    return TargetIdentity{Arch::X64, "x86_64", "macho", BinaryFormat::MachO,
                          "",        ".dylib", false};
  if (Target == "arm64-apple-darwin")
    return TargetIdentity{Arch::AArch64, "aarch64", "macho",
                          BinaryFormat::MachO, "", ".dylib", true};
  return std::nullopt;
}

std::optional<ProgramIdentity> getProgramIdentity(StringRef Program) {
  if (Program == "cxx_eh_probe")
    return ProgramIdentity{"exe", "cxx", "on"};
  if (Program == "cxx_eh_probe_noexc")
    return ProgramIdentity{"exe", "cxx", "off"};
  if (Program == "libcxx_eh_shared")
    return ProgramIdentity{"shared", "cxx", "on"};
  if (Program == "c_eh_probe")
    return ProgramIdentity{"exe", "c", "on"};
  return std::nullopt;
}

/// The routine the personality slot names.  C and C++ install different ones,
/// and mingw spells both with the `seh0` suffix because the language semantics
/// stay Itanium while the unwinder underneath is Windows SEH.
StringRef getPersonality(StringRef ObjectFormat, StringRef SourceLanguage) {
  if (SourceLanguage == "c")
    return ObjectFormat == "pe" ? StringRef("__gcc_personality_seh0")
                                : StringRef("__gcc_personality_v0");
  return ObjectFormat == "pe" ? StringRef("__gxx_personality_seh0")
                              : StringRef("__gxx_personality_v0");
}

/// Every personality an artifact is allowed to name.  EHABI lets a frame with
/// no table of its own use the compact model, whose personality is one of the
/// `__aeabi_unwind_cpp_pr*` routines rather than the language's.
std::vector<std::string> expectedPersonalities(StringRef ObjectFormat,
                                               StringRef SourceLanguage,
                                               StringRef Architecture,
                                               StringRef Exceptions) {
  if (Exceptions == "off")
    return {};
  std::vector<std::string> Names{
      getPersonality(ObjectFormat, SourceLanguage).str()};
  if (Architecture == "arm") {
    Names.emplace_back("__aeabi_unwind_cpp_pr0");
    Names.emplace_back("__aeabi_unwind_cpp_pr1");
  }
  return Names;
}

CxxItaniumCorpusValidationLevel expectedValidationLevel(StringRef Architecture,
                                                        StringRef Exceptions) {
  if (Exceptions == "off")
    return CxxItaniumCorpusValidationLevel::CfiOnly;
  if (Architecture == "arm")
    return CxxItaniumCorpusValidationLevel::Ehabi;
  return CxxItaniumCorpusValidationLevel::LsdaGraph;
}

bool isNormalizedCorpusPath(StringRef Path) {
  if (Path.empty() || Path.starts_with('/') || Path.contains('\\'))
    return false;
  SmallVector<StringRef, 16> Parts;
  Path.split(Parts, '/', -1, true);
  if (Parts.empty())
    return false;
  return llvm::none_of(Parts, [](StringRef Part) {
    return Part.empty() || Part == "." || Part == "..";
  });
}

bool isLowerSHA256(StringRef Hash) {
  return Hash.size() == 64 && llvm::all_of(Hash, [](char C) {
           return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f');
         });
}

std::string cellKey(StringRef Toolchain, StringRef Target) {
  return (Toolchain + "-" + Target).str();
}

std::string variantKey(StringRef Program, StringRef Optimization,
                       bool Stripped) {
  return (Program + "|" + Optimization + "|" + (Stripped ? "stripped" : "symtab"))
      .str();
}

/// Check the contract against the axes that decide it.  Every relation here is
/// one the schema states as a conditional subschema, so a manifest that
/// satisfies the schema satisfies this, and one that drifts from the producer
/// matrix is rejected before a test reads it.
Error verifyContract(const CxxItaniumEHArtifactExpectation &Expectation,
                     StringRef Context) {
  const bool Throws = Expectation.Exceptions == "on";
  const bool IsARM = Expectation.Architecture == "arm";
  const bool IsELF = Expectation.ObjectFormat == "elf";

  if (Expectation.ValidationLevel !=
      expectedValidationLevel(Expectation.Architecture, Expectation.Exceptions))
    return manifestError(Context +
                         ": validation level disagrees with the target and the "
                         "exception setting");
  if (Expectation.ExpectNoLSDA == Throws)
    return manifestError(Context +
                         ": language-data claim disagrees with the exception "
                         "setting");
  if (Expectation.ExpectArmEHABI != IsARM)
    return manifestError(Context + ": EHABI claim disagrees with the target");
  if (Expectation.Personalities !=
      expectedPersonalities(Expectation.ObjectFormat,
                            Expectation.SourceLanguage,
                            Expectation.Architecture, Expectation.Exceptions))
    return manifestError(Context +
                         ": personalities disagree with the target and the "
                         "source language");

  // What a container carries is decided by the container, not by the source.
  // ARM EHABI replaces the DWARF chain outright, so a claim about `.eh_frame`
  // there would be a claim about a section that does not exist.
  if (Expectation.EhFramePresent != (IsELF && !IsARM))
    return manifestError(Context +
                         ": DWARF frame claim disagrees with the container");
  if (Expectation.ArmExidxPresent != IsARM)
    return manifestError(Context +
                         ": EHABI index claim disagrees with the container");
  if ((Expectation.MinArmExidxEntries != 0) != IsARM)
    return manifestError(Context +
                         ": EHABI index floor disagrees with the container");
  if (Expectation.RequireUnwindTables == IsARM)
    return manifestError(Context +
                         ": unwind-table claim disagrees with the container");
  if (Expectation.SymbolNamesExpected == Expectation.Stripped)
    return manifestError(Context +
                         ": symbol expectation disagrees with the strip axis");

  // A stripped artifact keeps no name to key on, so what identifies it is the
  // mangled type it throws, which is data and survives stripping.  Only a C++
  // program that can throw defines one: the control throws nothing, and the C
  // probe raises through the shared library rather than naming a type of its
  // own.
  const bool DefinesAThrownType = Throws && Expectation.SourceLanguage == "cxx";
  if (DefinesAThrownType != !Expectation.RequiredStrings.empty())
    return manifestError(Context +
                         ": required type strings disagree with what the "
                         "program can throw");

  if (!Throws) {
    if (Expectation.MinCallSites != 0 || Expectation.MinLandingPads != 0 ||
        Expectation.MinCatchClauses != 0 || Expectation.MinCleanupPads != 0 ||
        Expectation.MinTypeTableEntries != 0)
      return manifestError(Context +
                           ": exception-free contract claims recovered state");
    return Error::success();
  }

  // A frame that can throw reaches a pad through a call site, so a throwing
  // artifact claiming neither would be claiming nothing at all.
  if (Expectation.MinCallSites == 0 || Expectation.MinLandingPads == 0)
    return manifestError(Context +
                         ": throwing contract claims no call site or pad");
  // Only C++ has a type table; a C frame runs cleanups and names no type,
  // which is the whole reason the corpus carries one.
  if ((Expectation.MinTypeTableEntries != 0 ||
       Expectation.MinCatchClauses != 0) &&
      Expectation.SourceLanguage != "cxx")
    return manifestError(Context + ": a C frame cannot claim a catch or a type");
  return Error::success();
}

Expected<CxxItaniumEHArtifactExpectation>
parseArtifact(const json::Object &Object, size_t Index) {
  const std::string Context = "artifacts[" + std::to_string(Index) + "]";
  CxxItaniumEHArtifactExpectation Result;

  auto Path = requireString(Object, "path", Context);
  if (!Path)
    return Path.takeError();
  Result.Path = std::move(*Path);
  if (!isNormalizedCorpusPath(Result.Path))
    return manifestError(Context + ": artifact path is not normalized");

  auto Hash = requireString(Object, "sha256", Context);
  if (!Hash)
    return Hash.takeError();
  Result.SHA256 = std::move(*Hash);
  if (!isLowerSHA256(Result.SHA256))
    return manifestError(Context + ": artifact hash is not lowercase SHA-256");
  auto Size = requireNonNegativeInteger(Object, "size", Context);
  if (!Size)
    return Size.takeError();
  Result.Size = *Size;
  if (Result.Size == 0)
    return manifestError(Context + ": artifact size must be positive");

  auto Toolchain = requireString(Object, "toolchain", Context);
  if (!Toolchain)
    return Toolchain.takeError();
  Result.Toolchain = std::move(*Toolchain);
  if (Result.Toolchain != "gcc" && Result.Toolchain != "clang")
    return manifestError(Context + ": unsupported toolchain");

  auto Target = requireString(Object, "target", Context);
  if (!Target)
    return Target.takeError();
  Result.Target = std::move(*Target);
  std::optional<TargetIdentity> Identity = getTargetIdentity(Result.Target);
  if (!Identity)
    return manifestError(Context + ": unsupported corpus target");
  Result.ExpectedArch = Identity->TheArch;
  Result.ExpectedFormat = Identity->Format;

  auto Architecture = requireString(Object, "architecture", Context);
  if (!Architecture)
    return Architecture.takeError();
  Result.Architecture = std::move(*Architecture);
  auto ObjectFormat = requireString(Object, "object_format", Context);
  if (!ObjectFormat)
    return ObjectFormat.takeError();
  Result.ObjectFormat = std::move(*ObjectFormat);
  if (Result.Architecture != Identity->Architecture ||
      Result.ObjectFormat != Identity->ObjectFormat)
    return manifestError(
        Context + ": architecture or object format disagrees with the target");

  auto Program = requireString(Object, "program", Context);
  if (!Program)
    return Program.takeError();
  Result.Program = std::move(*Program);
  std::optional<ProgramIdentity> Shape = getProgramIdentity(Result.Program);
  if (!Shape)
    return manifestError(Context + ": unsupported program");

  auto ArtifactKind = requireString(Object, "artifact_kind", Context);
  if (!ArtifactKind)
    return ArtifactKind.takeError();
  Result.ArtifactKind = std::move(*ArtifactKind);
  auto SourceLanguage = requireString(Object, "source_language", Context);
  if (!SourceLanguage)
    return SourceLanguage.takeError();
  Result.SourceLanguage = std::move(*SourceLanguage);
  auto Exceptions = requireString(Object, "exceptions", Context);
  if (!Exceptions)
    return Exceptions.takeError();
  Result.Exceptions = std::move(*Exceptions);
  if (Result.ArtifactKind != Shape->ArtifactKind ||
      Result.SourceLanguage != Shape->SourceLanguage ||
      Result.Exceptions != Shape->Exceptions)
    return manifestError(Context + ": artifact shape disagrees with the program");

  auto Optimization = requireString(Object, "optimization", Context);
  if (!Optimization)
    return Optimization.takeError();
  Result.Optimization = std::move(*Optimization);
  if (Result.Optimization != "o0" && Result.Optimization != "o2")
    return manifestError(Context + ": unsupported optimization");
  auto Stripped = requireBoolean(Object, "stripped", Context);
  if (!Stripped)
    return Stripped.takeError();
  Result.Stripped = *Stripped;

  auto Execution = requireString(Object, "execution", Context);
  if (!Execution)
    return Execution.takeError();
  Result.Execution = std::move(*Execution);
  StringRef ExpectedExecution = "not-run-cross-target";
  if (Result.ArtifactKind == "shared")
    ExpectedExecution = "not-run-library";
  else if (Identity->Native)
    ExpectedExecution = "passed";
  if (Result.Execution != ExpectedExecution)
    return manifestError(Context + ": execution status disagrees with target");

  const StringRef Symbols = Result.Stripped ? "stripped" : "symtab";
  const StringRef Extension = Result.ArtifactKind == "shared"
                                  ? Identity->SharedExtension
                                  : Identity->ExeExtension;
  const std::string ExpectedPath =
      ("corpus/cxx-itanium-eh/" + Result.Toolchain + "/" + Result.Target + "/" +
       Result.Optimization + "/" + Symbols + "/" + Result.ArtifactKind + "/" +
       Result.Program + "-" + Result.Toolchain + "-" + Result.Target + "-" +
       Result.Optimization + "-" + Symbols + Extension)
          .str();
  if (Result.Path != ExpectedPath)
    return manifestError(Context + ": artifact path disagrees with build axes");

  const json::Object *Build = Object.getObject("build");
  if (!Build)
    return manifestError(Context + ": missing object 'build'");
  auto Flags =
      requireStringArray(*Build, "compiler_flags", Context + ".build", false);
  if (!Flags)
    return Flags.takeError();
  // The exception setting is a compiler flag before it is an axis, so a cell
  // whose flags and recorded setting disagree built something other than what
  // the rest of the contract describes.
  const StringRef ExceptionFlag =
      Result.Exceptions == "on" ? "-fexceptions" : "-fno-exceptions";
  if (!llvm::is_contained(*Flags, ExceptionFlag))
    return manifestError(Context +
                         ".build: compiler flags do not select the recorded "
                         "exception setting");

  const json::Object *Evidence = Object.getObject("evidence");
  if (!Evidence)
    return manifestError(Context + ": missing object 'evidence'");
  struct StringList {
    StringRef Key;
    std::vector<std::string> *Field;
    bool AllowEmpty;
  };
  const StringList Lists[] = {
      {"required_sections", &Result.RequiredSections, false},
      {"forbidden_sections", &Result.ForbiddenSections, true},
      {"required_symbols", &Result.RequiredSymbols, true},
      {"forbidden_symbols", &Result.ForbiddenSymbols, true},
      {"required_strings", &Result.RequiredStrings, true},
  };
  for (const StringList &Entry : Lists) {
    auto Values = requireStringArray(*Evidence, Entry.Key, Context + ".evidence",
                                     Entry.AllowEmpty);
    if (!Values)
      return Values.takeError();
    *Entry.Field = std::move(*Values);
  }
  struct Flag {
    StringRef Key;
    bool *Field;
  };
  const Flag Flags2[] = {
      {"symbol_names_expected", &Result.SymbolNamesExpected},
      {"eh_frame_present", &Result.EhFramePresent},
      {"arm_exidx_present", &Result.ArmExidxPresent},
      {"require_unwind_tables", &Result.RequireUnwindTables},
  };
  for (const Flag &Entry : Flags2) {
    auto Value = requireBoolean(*Evidence, Entry.Key, Context + ".evidence");
    if (!Value)
      return Value.takeError();
    *Entry.Field = *Value;
  }
  auto ExidxEntries = requireNonNegativeInteger(
      *Evidence, "min_arm_exidx_entries", Context + ".evidence");
  if (!ExidxEntries)
    return ExidxEntries.takeError();
  Result.MinArmExidxEntries = *ExidxEntries;

  const json::Object *NeverD = Object.getObject("neverd");
  if (!NeverD)
    return manifestError(Context + ": missing object 'neverd'");
  auto Level = requireString(*NeverD, "validation_level", Context + ".neverd");
  if (!Level)
    return Level.takeError();
  if (*Level == "lsda-graph")
    Result.ValidationLevel = CxxItaniumCorpusValidationLevel::LsdaGraph;
  else if (*Level == "ehabi")
    Result.ValidationLevel = CxxItaniumCorpusValidationLevel::Ehabi;
  else if (*Level == "cfi-only")
    Result.ValidationLevel = CxxItaniumCorpusValidationLevel::CfiOnly;
  else
    return manifestError(Context + ": unsupported validation level");

  auto Personalities = requireStringArray(*NeverD, "personalities_any",
                                          Context + ".neverd", true);
  if (!Personalities)
    return Personalities.takeError();
  Result.Personalities = std::move(*Personalities);
  auto NoLSDA = requireBoolean(*NeverD, "expect_no_lsda", Context + ".neverd");
  if (!NoLSDA)
    return NoLSDA.takeError();
  Result.ExpectNoLSDA = *NoLSDA;
  auto EHABI = requireBoolean(*NeverD, "expect_arm_ehabi", Context + ".neverd");
  if (!EHABI)
    return EHABI.takeError();
  Result.ExpectArmEHABI = *EHABI;

  struct Minimum {
    StringRef Key;
    uint64_t *Field;
  };
  const Minimum Minimums[] = {
      {"min_call_sites", &Result.MinCallSites},
      {"min_landing_pads", &Result.MinLandingPads},
      {"min_catch_clauses", &Result.MinCatchClauses},
      {"min_cleanup_pads", &Result.MinCleanupPads},
      {"min_type_table_entries", &Result.MinTypeTableEntries},
  };
  for (const Minimum &Entry : Minimums) {
    auto Value =
        requireNonNegativeInteger(*NeverD, Entry.Key, Context + ".neverd");
    if (!Value)
      return Value.takeError();
    *Entry.Field = *Value;
  }

  if (Error Failure = verifyContract(Result, Context))
    return std::move(Failure);
  return Result;
}

/// Every cell builds the same eight variants, so completeness can be stated
/// exactly rather than approximated.
std::set<std::string> expectedVariants() {
  return {
      variantKey("cxx_eh_probe", "o0", true),
      variantKey("cxx_eh_probe", "o0", false),
      variantKey("cxx_eh_probe", "o2", true),
      variantKey("cxx_eh_probe", "o2", false),
      variantKey("cxx_eh_probe_noexc", "o2", false),
      variantKey("libcxx_eh_shared", "o0", false),
      variantKey("libcxx_eh_shared", "o2", false),
      variantKey("c_eh_probe", "o2", false),
  };
}

std::set<std::string> expectedCells() {
  std::set<std::string> Result;
  for (StringRef Toolchain : {"gcc", "clang"})
    for (StringRef Target :
         {"x86_64-linux-gnu", "aarch64-linux-gnu", "armv7-linux-gnueabihf",
          "x86_64-w64-mingw32", "x86_64-apple-darwin", "arm64-apple-darwin"}) {
      // Apple's clang is the only compiler for Mach-O and mingw's GCC the only
      // one for PE, so those three targets are one cell each rather than two.
      const bool AppleOnly = Target.ends_with("-apple-darwin");
      const bool MinGWOnly = Target == "x86_64-w64-mingw32";
      if (AppleOnly && Toolchain != "clang")
        continue;
      if (MinGWOnly && Toolchain != "gcc")
        continue;
      Result.insert(cellKey(Toolchain, Target));
    }
  return Result;
}

Error verifyCompleteMatrix(
    ArrayRef<CxxItaniumEHArtifactExpectation> Expectations) {
  std::map<std::string, std::set<std::string>> VariantsByCell;
  for (const CxxItaniumEHArtifactExpectation &Expectation : Expectations) {
    const std::string Key = cellKey(Expectation.Toolchain, Expectation.Target);
    if (!VariantsByCell[Key]
             .insert(variantKey(Expectation.Program, Expectation.Optimization,
                                Expectation.Stripped))
             .second)
      return manifestError("duplicate variant in corpus matrix cell " + Key);
  }

  std::map<std::string, std::set<std::string>> Expected;
  const std::set<std::string> Variants = expectedVariants();
  for (const std::string &Cell : expectedCells())
    Expected.emplace(Cell, Variants);
  if (VariantsByCell != Expected)
    return manifestError(
        "corpus manifest does not contain the complete build matrix");
  if (Expectations.size() != Expected.size() * Variants.size())
    return manifestError(
        "corpus manifest artifact count disagrees with its build matrix");
  return Error::success();
}

} // namespace

const char *
getCxxItaniumValidationLevelName(CxxItaniumCorpusValidationLevel Level) {
  switch (Level) {
  case CxxItaniumCorpusValidationLevel::LsdaGraph:
    return "lsda-graph";
  case CxxItaniumCorpusValidationLevel::Ehabi:
    return "ehabi";
  case CxxItaniumCorpusValidationLevel::CfiOnly:
    return "cfi-only";
  }
  return "unknown";
}

Expected<std::vector<CxxItaniumEHArtifactExpectation>>
parseCxxItaniumEHCorpusManifest(StringRef Contents, bool RequireCompleteMatrix) {
  auto Parsed = json::parse(Contents);
  if (!Parsed)
    return Parsed.takeError();
  const json::Object *Root = Parsed->getAsObject();
  if (!Root)
    return manifestError("corpus manifest root is not an object");
  if (Root->getInteger("schema_version") != 1 ||
      Root->getString("corpus") != "cxx-itanium-eh")
    return manifestError("unsupported corpus manifest identity");
  const json::Array *Artifacts = Root->getArray("artifacts");
  if (!Artifacts || Artifacts->empty())
    return manifestError("corpus manifest contains no artifacts");

  std::vector<CxxItaniumEHArtifactExpectation> Result;
  Result.reserve(Artifacts->size());
  std::set<std::string> Paths;
  for (size_t I = 0; I < Artifacts->size(); ++I) {
    const json::Object *Object = (*Artifacts)[I].getAsObject();
    if (!Object)
      return manifestError("artifact entry is not an object");
    auto Expectation = parseArtifact(*Object, I);
    if (!Expectation)
      return Expectation.takeError();
    if (!Paths.insert(Expectation->Path).second)
      return manifestError("duplicate artifact path '" + Expectation->Path +
                           "'");
    Result.push_back(std::move(*Expectation));
  }
  if (RequireCompleteMatrix)
    if (Error Failure = verifyCompleteMatrix(Result))
      return std::move(Failure);
  return Result;
}

Expected<std::vector<CxxItaniumEHArtifactExpectation>>
loadCxxItaniumEHCorpusManifest(StringRef Path, bool RequireCompleteMatrix) {
  auto BufferOrErr = MemoryBuffer::getFile(Path);
  if (!BufferOrErr)
    return manifestError("cannot read corpus manifest '" + Path +
                         "': " + BufferOrErr.getError().message());
  return parseCxxItaniumEHCorpusManifest((*BufferOrErr)->getBuffer(),
                                         RequireCompleteMatrix);
}

} // namespace neverd::test
