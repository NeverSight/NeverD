//===- AdaDEHCorpusManifest.cpp - Corpus manifest reader ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "AdaDEHCorpusManifest.h"

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

struct TargetIdentity {
  Arch TheArch;
  StringRef Architecture;
  bool Native;
};

struct ToolchainIdentity {
  StringRef Language;
  StringRef Personality;
  AdaDDescriptorABI DescriptorABI;
  uint64_t MinCleanupPads;
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
    return TargetIdentity{Arch::X64, "x86_64", true};
  if (Target == "aarch64-linux-gnu")
    return TargetIdentity{Arch::AArch64, "aarch64", false};
  return std::nullopt;
}

std::optional<ToolchainIdentity> getToolchainIdentity(StringRef Toolchain) {
  if (Toolchain == "gnat")
    return ToolchainIdentity{"ada", "__gnat_personality_v0",
                             AdaDDescriptorABI::GnatExceptionId, 0};
  if (Toolchain == "gdc")
    return ToolchainIdentity{"d", "__gdc_personality_v0",
                             AdaDDescriptorABI::DClassInfo, 1};
  if (Toolchain == "dmd")
    return ToolchainIdentity{"d", "__dmd_personality_v0",
                             AdaDDescriptorABI::DClassInfo, 1};
  if (Toolchain == "ldc")
    return ToolchainIdentity{"d", "_d_eh_personality",
                             AdaDDescriptorABI::DClassInfo, 1};
  return std::nullopt;
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

Error verifyContract(const AdaDEHArtifactExpectation &Expectation,
                     StringRef Context) {
  std::optional<ToolchainIdentity> Toolchain =
      getToolchainIdentity(Expectation.Toolchain);
  std::optional<TargetIdentity> Target = getTargetIdentity(Expectation.Target);
  if (!Toolchain || !Target)
    return manifestError(Context + ": unsupported cell");
  if (Expectation.SourceLanguage != Toolchain->Language)
    return manifestError(Context + ": language disagrees with the toolchain");
  if (Expectation.Architecture != Target->Architecture ||
      Expectation.ExpectedArch != Target->TheArch)
    return manifestError(Context + ": architecture disagrees with the target");
  if (Expectation.ObjectFormat != "elf" ||
      Expectation.ExpectedFormat != BinaryFormat::ELF)
    return manifestError(Context + ": Ada/D corpus artifacts are ELF only");
  const StringRef ExpectedExecution =
      Target->Native ? StringRef("passed") : StringRef("not-run-cross-target");
  if (Expectation.Execution != ExpectedExecution)
    return manifestError(Context + ": execution status disagrees with target");
  if (Expectation.Personalities !=
      std::vector<std::string>{Toolchain->Personality.str()})
    return manifestError(Context + ": personality disagrees with the toolchain");
  if (Expectation.DescriptorABI != Toolchain->DescriptorABI)
    return manifestError(Context +
                         ": descriptor ABI disagrees with the toolchain");
  if (Expectation.MinCleanupPads != Toolchain->MinCleanupPads)
    return manifestError(Context +
                         ": cleanup floor disagrees with the language");
  if (Expectation.MinCallSites == 0 || Expectation.MinLandingPads == 0 ||
      Expectation.MinCatchClauses == 0 || Expectation.MinTypeTableEntries == 0)
    return manifestError(Context + ": lsda-graph contract has a zero floor");

  const StringRef Probe =
      Expectation.SourceLanguage == "ada" ? "ada_eh_probe" : "d_eh_probe";
  const std::string ExpectedPath =
      ("corpus/ada-d-eh/" + Expectation.SourceLanguage + "/" +
       Expectation.Toolchain + "/" + Expectation.Target + "/" +
       Expectation.Optimization + "/" + Probe + "-" + Expectation.Toolchain +
       "-" + Expectation.Target + "-" + Expectation.Optimization)
          .str();
  if (Expectation.Path != ExpectedPath)
    return manifestError(Context + ": artifact path disagrees with build axes");
  return Error::success();
}

Expected<AdaDEHArtifactExpectation> parseArtifact(const json::Object &Object,
                                                  size_t Index) {
  AdaDEHArtifactExpectation Result;
  const std::string Context = ("artifacts[" + Twine(Index) + "]").str();

  auto Path = requireString(Object, "path", Context);
  if (!Path)
    return Path.takeError();
  Result.Path = std::move(*Path);
  if (!isNormalizedCorpusPath(Result.Path))
    return manifestError(Context + ": artifact path is not repository-relative");

  auto Hash = requireString(Object, "sha256", Context);
  if (!Hash)
    return Hash.takeError();
  Result.SHA256 = std::move(*Hash);
  if (!isLowerSHA256(Result.SHA256))
    return manifestError(Context + ": sha256 is not a lowercase hex digest");
  auto Size = requireNonNegativeInteger(Object, "size", Context);
  if (!Size)
    return Size.takeError();
  Result.Size = *Size;
  if (Result.Size == 0)
    return manifestError(Context + ": artifact size is zero");

  auto Toolchain = requireString(Object, "toolchain", Context);
  if (!Toolchain)
    return Toolchain.takeError();
  Result.Toolchain = std::move(*Toolchain);
  std::optional<ToolchainIdentity> Shape =
      getToolchainIdentity(Result.Toolchain);
  if (!Shape)
    return manifestError(Context + ": unsupported toolchain");

  auto Target = requireString(Object, "target", Context);
  if (!Target)
    return Target.takeError();
  Result.Target = std::move(*Target);
  std::optional<TargetIdentity> Identity = getTargetIdentity(Result.Target);
  if (!Identity)
    return manifestError(Context + ": unsupported target");
  Result.ExpectedArch = Identity->TheArch;

  auto Architecture = requireString(Object, "architecture", Context);
  if (!Architecture)
    return Architecture.takeError();
  Result.Architecture = std::move(*Architecture);
  auto ObjectFormat = requireString(Object, "object_format", Context);
  if (!ObjectFormat)
    return ObjectFormat.takeError();
  Result.ObjectFormat = std::move(*ObjectFormat);
  Result.ExpectedFormat = BinaryFormat::ELF;

  auto Language = requireString(Object, "source_language", Context);
  if (!Language)
    return Language.takeError();
  Result.SourceLanguage = std::move(*Language);
  auto Optimization = requireString(Object, "optimization", Context);
  if (!Optimization)
    return Optimization.takeError();
  Result.Optimization = std::move(*Optimization);
  if (Result.Optimization != "o0" && Result.Optimization != "o2")
    return manifestError(Context + ": unsupported optimization");
  auto Execution = requireString(Object, "execution", Context);
  if (!Execution)
    return Execution.takeError();
  Result.Execution = std::move(*Execution);

  if (Object.getString("exception_model") != "itanium-dwarf")
    return manifestError(Context + ": exception model is not itanium-dwarf");

  const json::Object *Evidence = Object.getObject("evidence");
  if (!Evidence)
    return manifestError(Context + ": missing object 'evidence'");
  auto Sections = requireStringArray(*Evidence, "required_sections",
                                     Context + ".evidence", false);
  if (!Sections)
    return Sections.takeError();
  Result.RequiredSections = std::move(*Sections);
  auto Symbols = requireStringArray(*Evidence, "required_symbols",
                                    Context + ".evidence", false);
  if (!Symbols)
    return Symbols.takeError();
  Result.RequiredSymbols = std::move(*Symbols);
  auto Strings = requireStringArray(*Evidence, "required_strings",
                                    Context + ".evidence", false);
  if (!Strings)
    return Strings.takeError();
  Result.RequiredStrings = std::move(*Strings);
  auto Unwind =
      requireBoolean(*Evidence, "require_unwind_tables", Context + ".evidence");
  if (!Unwind)
    return Unwind.takeError();
  if (!*Unwind)
    return manifestError(Context + ": Ada/D cells require unwind tables");
  auto EhFrame =
      requireBoolean(*Evidence, "eh_frame_present", Context + ".evidence");
  if (!EhFrame)
    return EhFrame.takeError();
  if (!*EhFrame)
    return manifestError(Context + ": Ada/D cells require .eh_frame");

  const json::Object *NeverD = Object.getObject("neverd");
  if (!NeverD)
    return manifestError(Context + ": missing object 'neverd'");
  if (NeverD->getString("validation_level") != "lsda-graph")
    return manifestError(Context + ": validation level is not lsda-graph");
  if (NeverD->getString("type_table_interpretation") != "opaque-descriptor")
    return manifestError(Context +
                         ": type-table interpretation is not opaque-descriptor");
  if (NeverD->getString("native_reconstruction") != "address-clauses")
    return manifestError(Context +
                         ": native reconstruction is not address-clauses");
  auto Proven = requireBoolean(*NeverD, "corpus_proven", Context + ".neverd");
  if (!Proven)
    return Proven.takeError();
  if (!*Proven)
    return manifestError(Context + ": corpus-proven must stay a separate true claim");

  auto Descriptor =
      requireString(*NeverD, "descriptor_abi", Context + ".neverd");
  if (!Descriptor)
    return Descriptor.takeError();
  if (*Descriptor == "gnat-exception-id")
    Result.DescriptorABI = AdaDDescriptorABI::GnatExceptionId;
  else if (*Descriptor == "d-classinfo")
    Result.DescriptorABI = AdaDDescriptorABI::DClassInfo;
  else
    return manifestError(Context + ": unsupported descriptor ABI");

  auto Personalities = requireStringArray(*NeverD, "personalities_any",
                                          Context + ".neverd", false);
  if (!Personalities)
    return Personalities.takeError();
  Result.Personalities = std::move(*Personalities);

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

std::set<std::string> expectedCells() {
  return {
      "dmd-x86_64-linux-gnu",
      "gdc-aarch64-linux-gnu",
      "gdc-x86_64-linux-gnu",
      "gnat-aarch64-linux-gnu",
      "gnat-x86_64-linux-gnu",
      "ldc-x86_64-linux-gnu",
  };
}

Error verifyCompleteMatrix(ArrayRef<AdaDEHArtifactExpectation> Expectations) {
  std::map<std::string, std::set<std::string>> VariantsByCell;
  for (const AdaDEHArtifactExpectation &Expectation : Expectations) {
    const std::string Key = cellKey(Expectation.Toolchain, Expectation.Target);
    if (!VariantsByCell[Key].insert(Expectation.Optimization).second)
      return manifestError("duplicate variant in corpus matrix cell " + Key);
  }

  std::map<std::string, std::set<std::string>> Expected;
  for (const std::string &Cell : expectedCells())
    Expected.emplace(Cell, std::set<std::string>{"o0", "o2"});
  if (VariantsByCell != Expected)
    return manifestError(
        "corpus manifest does not contain the complete build matrix");
  if (Expectations.size() != 12u)
    return manifestError(
        "corpus manifest artifact count disagrees with its build matrix");
  return Error::success();
}

} // namespace

const char *getAdaDDescriptorABIName(AdaDDescriptorABI ABI) {
  switch (ABI) {
  case AdaDDescriptorABI::GnatExceptionId:
    return "gnat-exception-id";
  case AdaDDescriptorABI::DClassInfo:
    return "d-classinfo";
  }
  return "unknown";
}

Expected<std::vector<AdaDEHArtifactExpectation>>
parseAdaDEHCorpusManifest(StringRef Contents, bool RequireCompleteMatrix) {
  auto Parsed = json::parse(Contents);
  if (!Parsed)
    return Parsed.takeError();
  const json::Object *Root = Parsed->getAsObject();
  if (!Root)
    return manifestError("corpus manifest root is not an object");
  if (Root->getInteger("schema_version") != 1 ||
      Root->getString("corpus") != "ada-d-eh")
    return manifestError("unsupported corpus manifest identity");
  const json::Array *Artifacts = Root->getArray("artifacts");
  if (!Artifacts || Artifacts->empty())
    return manifestError("corpus manifest contains no artifacts");

  std::vector<AdaDEHArtifactExpectation> Result;
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

Expected<std::vector<AdaDEHArtifactExpectation>>
loadAdaDEHCorpusManifest(StringRef Path, bool RequireCompleteMatrix) {
  auto BufferOrErr = MemoryBuffer::getFile(Path);
  if (!BufferOrErr)
    return manifestError("cannot read corpus manifest '" + Path +
                         "': " + BufferOrErr.getError().message());
  return parseAdaDEHCorpusManifest((*BufferOrErr)->getBuffer(),
                                   RequireCompleteMatrix);
}

} // namespace neverd::test
