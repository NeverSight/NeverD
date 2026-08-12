//===- RustEHCorpusManifest.cpp - Corpus manifest reader ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RustEHCorpusManifest.h"

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

/// Everything the target triple alone decides.  Rust has no unwind format of
/// its own, so the triple is what picks between an Itanium LSDA, a Mach-O
/// compact-unwind pair, and the MSVC C++ tables -- and therefore what the rest
/// of an artifact's contract has to agree with.
struct TargetIdentity {
  Arch TheArch;
  StringRef Architecture;
  StringRef ObjectFormat;
  BinaryFormat Format;
  /// True when the hosted runner that builds this cell can also run its
  /// executable, which is what separates `passed` from `not-run-cross-target`.
  bool Native;
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

std::optional<TargetIdentity> getTargetIdentity(StringRef Triple) {
  if (Triple == "x86_64-unknown-linux-gnu")
    return TargetIdentity{Arch::X64, "x86_64", "elf", BinaryFormat::ELF, true};
  if (Triple == "aarch64-unknown-linux-gnu")
    return TargetIdentity{Arch::AArch64, "aarch64", "elf", BinaryFormat::ELF,
                          false};
  if (Triple == "x86_64-pc-windows-msvc")
    return TargetIdentity{Arch::X64, "x86_64", "pe", BinaryFormat::COFF, true};
  if (Triple == "x86_64-apple-darwin")
    return TargetIdentity{Arch::X64, "x86_64", "macho", BinaryFormat::MachO,
                          false};
  if (Triple == "aarch64-apple-darwin")
    return TargetIdentity{Arch::AArch64, "aarch64", "macho",
                          BinaryFormat::MachO, true};
  return std::nullopt;
}

std::optional<StringRef> getCrateName(StringRef CrateType) {
  if (CrateType == "bin")
    return StringRef("rust_eh_probe");
  if (CrateType == "cdylib")
    return StringRef("rust_eh_cdylib");
  return std::nullopt;
}

StringRef getArtifactExtension(StringRef CrateType, StringRef ObjectFormat) {
  if (CrateType == "bin")
    return ObjectFormat == "pe" ? StringRef(".exe") : StringRef();
  if (ObjectFormat == "elf")
    return ".so";
  if (ObjectFormat == "macho")
    return ".dylib";
  return ".dll";
}

/// The personality the target installs.  On every Itanium target Rust names
/// its own routine; on MSVC it borrows `__CxxFrameHandler3` from C++ and is
/// told apart only by a catch on the unmangled `rust_panic` descriptor.
StringRef getPersonality(StringRef ObjectFormat) {
  return ObjectFormat == "pe" ? StringRef("__CxxFrameHandler3")
                              : StringRef("rust_eh_personality");
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

std::string cellKey(StringRef Triple, StringRef PanicStrategy,
                    StringRef Optimization) {
  return (Triple + "|" + PanicStrategy + "|" + Optimization).str();
}

/// Check the `neverd` block against the two axes that decide it.  Every
/// relation here is one the schema states as a conditional subschema, so a
/// manifest that satisfies the schema satisfies this and a hand-written one
/// that drifts from the matrix is rejected before a test reads it.
Error verifyContract(const RustEHArtifactExpectation &Expectation,
                     StringRef Context) {
  const bool Unwinds = Expectation.PanicStrategy == "unwind";
  const bool IsMSVC = Expectation.ObjectFormat == "pe";

  if (Expectation.ValidationLevel !=
      (Unwinds ? RustCorpusValidationLevel::PanicGraph
               : RustCorpusValidationLevel::UnwindOnly))
    return manifestError(Context +
                         ": validation level disagrees with panic strategy");
  const std::vector<std::string> ExpectedStatuses =
      Unwinds ? std::vector<std::string>{"complete"}
              : std::vector<std::string>{"complete", "partial"};
  if (Expectation.AllowedParseStatuses != ExpectedStatuses)
    return manifestError(Context +
                         ": allowed parse status disagrees with panic strategy");
  if (Expectation.Personalities !=
      std::vector<std::string>{getPersonality(Expectation.ObjectFormat).str()})
    return manifestError(Context + ": personality disagrees with the target");
  if (Expectation.ExpectNoLandingPads == Unwinds)
    return manifestError(Context +
                         ": landing-pad claim disagrees with panic strategy");
  // An unwinding build gives every frame it compiles a landing pad, including
  // the `extern "C"` leaf whose body cannot panic, because rustc emits the
  // abort guard from whether it can prove the body nounwind and an opaque
  // `black_box` call defeats that proof.  So the pad-free set is exactly the
  // probe symbols when aborting and exactly empty when unwinding.
  if (Unwinds != Expectation.LandingPadFreeSymbols.empty())
    return manifestError(
        Context + ": landing-pad-free symbols disagree with panic strategy");

  if (!Unwinds) {
    if (Expectation.MinLandingPads != 0 || Expectation.MinDropGluePads != 0 ||
        Expectation.MinCatchUnwindPads != 0 ||
        Expectation.MinNoUnwindGuardPads != 0 ||
        Expectation.MinPanicSites != 0)
      return manifestError(Context +
                           ": aborting contract claims recovered panic state");
    return Error::success();
  }

  if (Expectation.MinLandingPads == 0 || Expectation.MinCatchUnwindPads == 0)
    return manifestError(Context +
                         ": unwinding contract claims no catch_unwind pad");
  // Only an Itanium target makes the remaining classifications recoverable:
  // the personality names Rust outright, so a cleanup pad is Rust's own and
  // the abort guard is an empty filter.  A frame that merely runs Drop glue on
  // MSVC is spelled exactly like a C++ one and is therefore not claimed.
  const bool ClaimsItaniumOnlyState = Expectation.MinDropGluePads != 0 ||
                                      Expectation.MinNoUnwindGuardPads != 0 ||
                                      Expectation.MinPanicSites != 0;
  if (IsMSVC && ClaimsItaniumOnlyState)
    return manifestError(Context +
                         ": MSVC contract claims Itanium-only classification");
  if (!IsMSVC && !ClaimsItaniumOnlyState)
    return manifestError(Context + ": Itanium contract claims no Rust frames");
  return Error::success();
}

Expected<RustEHArtifactExpectation> parseArtifact(const json::Object &Object,
                                                  size_t Index) {
  const std::string Context = "artifacts[" + std::to_string(Index) + "]";
  RustEHArtifactExpectation Result;

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

  auto TargetTriple = requireString(Object, "target_triple", Context);
  if (!TargetTriple)
    return TargetTriple.takeError();
  Result.TargetTriple = std::move(*TargetTriple);
  std::optional<TargetIdentity> Target = getTargetIdentity(Result.TargetTriple);
  if (!Target)
    return manifestError(Context + ": unsupported corpus target");
  Result.ExpectedArch = Target->TheArch;
  Result.ExpectedFormat = Target->Format;

  auto Architecture = requireString(Object, "architecture", Context);
  if (!Architecture)
    return Architecture.takeError();
  Result.Architecture = std::move(*Architecture);
  auto ObjectFormat = requireString(Object, "object_format", Context);
  if (!ObjectFormat)
    return ObjectFormat.takeError();
  Result.ObjectFormat = std::move(*ObjectFormat);
  if (Result.Architecture != Target->Architecture ||
      Result.ObjectFormat != Target->ObjectFormat)
    return manifestError(Context +
                         ": architecture or object format disagrees with the "
                         "target triple");

  auto CrateType = requireString(Object, "crate_type", Context);
  if (!CrateType)
    return CrateType.takeError();
  Result.CrateType = std::move(*CrateType);
  std::optional<StringRef> CrateName = getCrateName(Result.CrateType);
  if (!CrateName)
    return manifestError(Context + ": unsupported crate type");
  auto DeclaredCrateName = requireString(Object, "crate_name", Context);
  if (!DeclaredCrateName)
    return DeclaredCrateName.takeError();
  Result.CrateName = std::move(*DeclaredCrateName);
  if (Result.CrateName != *CrateName)
    return manifestError(Context + ": crate name disagrees with crate type");

  auto PanicStrategy = requireString(Object, "panic_strategy", Context);
  if (!PanicStrategy)
    return PanicStrategy.takeError();
  Result.PanicStrategy = std::move(*PanicStrategy);
  if (Result.PanicStrategy != "unwind" && Result.PanicStrategy != "abort")
    return manifestError(Context + ": unsupported panic strategy");
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
  StringRef ExpectedExecution = "not-run-cross-target";
  if (Result.CrateType != "bin")
    ExpectedExecution = "not-run-library";
  else if (Target->Native)
    ExpectedExecution = "passed";
  if (Result.Execution != ExpectedExecution)
    return manifestError(Context + ": execution status disagrees with target");

  const std::string ExpectedPath =
      "corpus/rust-eh/" + Result.TargetTriple + "/" + Result.PanicStrategy +
      "/" + Result.Optimization + "/" + Result.CrateType + "/" +
      Result.CrateName + "-" + Result.TargetTriple + "-" +
      Result.PanicStrategy + "-" + Result.Optimization +
      getArtifactExtension(Result.CrateType, Result.ObjectFormat).str();
  if (Result.Path != ExpectedPath)
    return manifestError(Context + ": artifact path disagrees with build axes");

  const json::Object *Build = Object.getObject("build");
  if (!Build)
    return manifestError(Context + ": missing object 'build'");
  auto Edition = requireString(*Build, "edition", Context + ".build");
  if (!Edition)
    return Edition.takeError();
  if (*Edition != "2024")
    return manifestError(Context + ".build: unsupported edition");
  auto Flags =
      requireStringArray(*Build, "rustc_flags", Context + ".build", false);
  if (!Flags)
    return Flags.takeError();
  // The strategy is a `-C panic=` flag before it is an axis, so a cell whose
  // flags and recorded strategy disagree built something other than what the
  // rest of the contract describes.
  if (!llvm::is_contained(*Flags, "panic=" + Result.PanicStrategy))
    return manifestError(Context +
                         ".build: rustc flags do not select the recorded panic "
                         "strategy");

  const json::Object *Evidence = Object.getObject("evidence");
  if (!Evidence)
    return manifestError(Context + ": missing object 'evidence'");
  auto Sections = requireStringArray(*Evidence, "required_sections",
                                     Context + ".evidence", false);
  if (!Sections)
    return Sections.takeError();
  Result.RequiredSections = std::move(*Sections);
  auto NamesExpected = requireBoolean(*Evidence, "symbol_names_expected",
                                      Context + ".evidence");
  if (!NamesExpected)
    return NamesExpected.takeError();
  Result.SymbolNamesExpected = *NamesExpected;
  const bool NamesRecoverable =
      Result.ObjectFormat != "pe" || Result.CrateType == "cdylib";
  if (Result.SymbolNamesExpected != NamesRecoverable)
    return manifestError(Context +
                         ".evidence: symbol expectation disagrees with the "
                         "container");

  const json::Object *NeverD = Object.getObject("neverd");
  if (!NeverD)
    return manifestError(Context + ": missing object 'neverd'");
  auto Level = requireString(*NeverD, "validation_level", Context + ".neverd");
  if (!Level)
    return Level.takeError();
  if (*Level == "panic-graph")
    Result.ValidationLevel = RustCorpusValidationLevel::PanicGraph;
  else if (*Level == "unwind-only")
    Result.ValidationLevel = RustCorpusValidationLevel::UnwindOnly;
  else
    return manifestError(Context + ": unsupported validation level");

  auto Statuses = requireStringArray(*NeverD, "allowed_parse_status",
                                     Context + ".neverd", false);
  if (!Statuses)
    return Statuses.takeError();
  Result.AllowedParseStatuses = std::move(*Statuses);
  auto Personalities = requireStringArray(*NeverD, "personalities_any",
                                          Context + ".neverd", false);
  if (!Personalities)
    return Personalities.takeError();
  Result.Personalities = std::move(*Personalities);
  auto NoPads =
      requireBoolean(*NeverD, "expect_no_landing_pads", Context + ".neverd");
  if (!NoPads)
    return NoPads.takeError();
  Result.ExpectNoLandingPads = *NoPads;
  auto PadFree = requireStringArray(*NeverD, "landing_pad_free_symbols",
                                    Context + ".neverd", true);
  if (!PadFree)
    return PadFree.takeError();
  Result.LandingPadFreeSymbols = std::move(*PadFree);

  struct Minimum {
    StringRef Key;
    uint64_t *Field;
  };
  const Minimum Minimums[] = {
      {"min_landing_pads", &Result.MinLandingPads},
      {"min_drop_glue_pads", &Result.MinDropGluePads},
      {"min_catch_unwind_pads", &Result.MinCatchUnwindPads},
      {"min_nounwind_guard_pads", &Result.MinNoUnwindGuardPads},
      {"min_panic_sites", &Result.MinPanicSites},
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

/// Rust's producer matrix is a plain cross product -- every crate builds for
/// every target under both panic strategies at both optimization levels -- so
/// completeness can be stated exactly rather than approximated.
std::map<std::string, std::set<std::string>> expectedInventory() {
  std::map<std::string, std::set<std::string>> Result;
  const std::set<std::string> CrateTypes{"bin", "cdylib"};
  for (StringRef Triple :
       {"x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu",
        "x86_64-pc-windows-msvc", "x86_64-apple-darwin",
        "aarch64-apple-darwin"})
    for (StringRef PanicStrategy : {"unwind", "abort"})
      for (StringRef Optimization : {"o0", "o2"})
        Result.emplace(cellKey(Triple, PanicStrategy, Optimization),
                       CrateTypes);
  return Result;
}

Error verifyCompleteMatrix(ArrayRef<RustEHArtifactExpectation> Expectations) {
  std::map<std::string, std::set<std::string>> TypesByCell;
  for (const RustEHArtifactExpectation &Expectation : Expectations) {
    std::string Key = cellKey(Expectation.TargetTriple,
                              Expectation.PanicStrategy,
                              Expectation.Optimization);
    if (!TypesByCell[Key].insert(Expectation.CrateType).second)
      return manifestError("duplicate crate type in corpus matrix cell");
  }
  const std::map<std::string, std::set<std::string>> Expected =
      expectedInventory();
  if (TypesByCell != Expected)
    return manifestError(
        "corpus manifest does not contain the complete target matrix");
  size_t ExpectedArtifactCount = 0;
  for (const auto &Entry : Expected)
    ExpectedArtifactCount += Entry.second.size();
  if (Expectations.size() != ExpectedArtifactCount)
    return manifestError(
        "corpus manifest artifact count disagrees with its target matrix");
  return Error::success();
}

} // namespace

Expected<std::vector<RustEHArtifactExpectation>>
parseRustEHCorpusManifest(StringRef Contents, bool RequireCompleteMatrix) {
  auto Parsed = json::parse(Contents);
  if (!Parsed)
    return Parsed.takeError();
  const json::Object *Root = Parsed->getAsObject();
  if (!Root)
    return manifestError("corpus manifest root is not an object");
  if (Root->getInteger("schema_version") != 1 ||
      Root->getString("corpus") != "rust-eh")
    return manifestError("unsupported corpus manifest identity");
  const json::Array *Artifacts = Root->getArray("artifacts");
  if (!Artifacts || Artifacts->empty())
    return manifestError("corpus manifest contains no artifacts");

  std::vector<RustEHArtifactExpectation> Result;
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

Expected<std::vector<RustEHArtifactExpectation>>
loadRustEHCorpusManifest(StringRef Path, bool RequireCompleteMatrix) {
  auto BufferOrErr = MemoryBuffer::getFile(Path);
  if (!BufferOrErr)
    return manifestError("cannot read corpus manifest '" + Path +
                         "': " + BufferOrErr.getError().message());
  return parseRustEHCorpusManifest((*BufferOrErr)->getBuffer(),
                                   RequireCompleteMatrix);
}

} // namespace neverd::test
