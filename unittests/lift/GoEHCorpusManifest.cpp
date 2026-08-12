//===- GoEHCorpusManifest.cpp - Corpus manifest reader ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "GoEHCorpusManifest.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <optional>
#include <set>
#include <string>
#include <utility>

using namespace llvm;

namespace neverd::test {
namespace {

/// What the toolchain release alone decides.  The `pcHeader` magic is the only
/// self-describing thing in a Go image, and it is what fixes the record shapes
/// every other claim is read through.
struct ReleaseIdentity {
  StringRef PclnTabVersion;
  uint32_t Magic;
  GoCorpusValidationLevel Level;
  /// Go 1.18 moved the funcdata base out of the `pclntab` and into
  /// `moduledata`, so from there on the table cannot be read to the end
  /// without locating the module.
  bool RequiresModuleData;
  /// `go:func.*` gained its colon in the Go 1.20 symbol naming change.
  StringRef GoFuncSymbol;
  /// True while a position-independent ELF still carried the table in the
  /// relro segment, which renamed its section.  CL 718065 moved it to plain
  /// read-only data for Go 1.26, on the grounds that the table holds no
  /// relocations, so from there a PIE names it like an ordinary executable.
  bool RelroPclnTab;
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

std::optional<ReleaseIdentity> getReleaseIdentity(StringRef Version) {
  SmallVector<StringRef, 4> Parts;
  Version.split(Parts, '.');
  unsigned Minor = 0;
  if (Parts.size() != 3 || Parts[0] != "1" || Parts[1].getAsInteger(10, Minor))
    return std::nullopt;
  if (Parts[2].empty() ||
      !llvm::all_of(Parts[2], [](char C) { return C >= '0' && C <= '9'; }))
    return std::nullopt;
  if (Minor >= 26)
    return ReleaseIdentity{"go1.20", 0xFFFFFFF1u,
                           GoCorpusValidationLevel::RuntimeGraph, true,
                           "go:func.*", false};
  if (Minor >= 20)
    return ReleaseIdentity{"go1.20", 0xFFFFFFF1u,
                           GoCorpusValidationLevel::RuntimeGraph, true,
                           "go:func.*", true};
  if (Minor >= 18)
    return ReleaseIdentity{"go1.18", 0xFFFFFFF0u,
                           GoCorpusValidationLevel::RuntimeGraph, true,
                           "go.func.*", true};
  if (Minor >= 16)
    return ReleaseIdentity{"go1.16", 0xFFFFFFFAu,
                           GoCorpusValidationLevel::RuntimeGraph, false,
                           "go.func.*", true};
  if (Minor >= 2)
    return ReleaseIdentity{"go1.2", 0xFFFFFFFBu,
                           GoCorpusValidationLevel::TableOnly, false,
                           "go.func.*", true};
  return std::nullopt;
}

std::optional<std::pair<StringRef, BinaryFormat>> getContainer(StringRef GOOS) {
  if (GOOS == "linux")
    return std::pair{StringRef("elf"), BinaryFormat::ELF};
  if (GOOS == "windows")
    return std::pair{StringRef("pe"), BinaryFormat::COFF};
  if (GOOS == "darwin")
    return std::pair{StringRef("macho"), BinaryFormat::MachO};
  return std::nullopt;
}

/// The machine and its `sys.PCQuantum`, the unit every pc delta in the table
/// is divided by.
std::optional<std::pair<Arch, uint8_t>> getMachine(StringRef GOARCH) {
  if (GOARCH == "amd64")
    return std::pair{Arch::X64, uint8_t(1)};
  if (GOARCH == "arm64")
    return std::pair{Arch::AArch64, uint8_t(4)};
  return std::nullopt;
}

/// Where the linker leaves the table.  Through Go 1.25 a position-independent
/// ELF moved it into the relro segment, which renamed its section; PE gives it
/// no section of its own at all.
StringRef getPclnTabSection(StringRef GOOS, StringRef BuildMode,
                            bool RelroPclnTab) {
  if (GOOS == "windows")
    return ".rdata";
  if (GOOS == "darwin")
    return "__gopclntab";
  if (BuildMode == "exe" || !RelroPclnTab)
    return ".gopclntab";
  return ".data.rel.ro.gopclntab";
}

StringRef getArtifactExtension(StringRef GOOS, StringRef BuildMode) {
  if (BuildMode == "c-shared") {
    if (GOOS == "windows")
      return ".dll";
    return GOOS == "darwin" ? StringRef(".dylib") : StringRef(".so");
  }
  return GOOS == "windows" ? StringRef(".exe") : StringRef();
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

Expected<GoEHArtifactExpectation> parseArtifact(const json::Object &Object,
                                                size_t Index) {
  const std::string Context = "artifacts[" + std::to_string(Index) + "]";
  GoEHArtifactExpectation Result;

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

  auto GoVersion = requireString(Object, "go_version", Context);
  if (!GoVersion)
    return GoVersion.takeError();
  Result.GoVersion = std::move(*GoVersion);
  std::optional<ReleaseIdentity> Release = getReleaseIdentity(Result.GoVersion);
  if (!Release)
    return manifestError(Context + ": unsupported Go release");

  auto GOOS = requireString(Object, "goos", Context);
  if (!GOOS)
    return GOOS.takeError();
  Result.GOOS = std::move(*GOOS);
  std::optional<std::pair<StringRef, BinaryFormat>> Container =
      getContainer(Result.GOOS);
  if (!Container)
    return manifestError(Context + ": unsupported GOOS");
  Result.ExpectedFormat = Container->second;
  auto GOARCH = requireString(Object, "goarch", Context);
  if (!GOARCH)
    return GOARCH.takeError();
  Result.GOARCH = std::move(*GOARCH);
  std::optional<std::pair<Arch, uint8_t>> Machine = getMachine(Result.GOARCH);
  if (!Machine)
    return manifestError(Context + ": unsupported GOARCH");
  Result.ExpectedArch = Machine->first;

  auto ObjectFormat = requireString(Object, "object_format", Context);
  if (!ObjectFormat)
    return ObjectFormat.takeError();
  Result.ObjectFormat = std::move(*ObjectFormat);
  if (Result.ObjectFormat != Container->first)
    return manifestError(Context + ": object format disagrees with GOOS");

  auto BuildMode = requireString(Object, "buildmode", Context);
  if (!BuildMode)
    return BuildMode.takeError();
  Result.BuildMode = std::move(*BuildMode);
  if (Result.BuildMode != "exe" && Result.BuildMode != "pie" &&
      Result.BuildMode != "c-shared")
    return manifestError(Context + ": unsupported buildmode");
  auto CgoEnabled = requireBoolean(Object, "cgo_enabled", Context);
  if (!CgoEnabled)
    return CgoEnabled.takeError();
  Result.CgoEnabled = *CgoEnabled;
  // `-buildmode=c-shared` is linked externally, which cannot be done without
  // a C toolchain, so the two axes are not independent.
  if (Result.BuildMode == "c-shared" && !Result.CgoEnabled)
    return manifestError(Context + ": c-shared artifact claims cgo is off");
  auto Stripped = requireBoolean(Object, "stripped", Context);
  if (!Stripped)
    return Stripped.takeError();
  Result.Stripped = *Stripped;
  auto Optimization = requireString(Object, "optimization", Context);
  if (!Optimization)
    return Optimization.takeError();
  Result.Optimization = std::move(*Optimization);
  if (Result.Optimization != "default" && Result.Optimization != "none")
    return manifestError(Context + ": unsupported optimization");

  const std::string CgoLabel = Result.CgoEnabled ? "cgo1" : "cgo0";
  const std::string LinkLabel = Result.Stripped ? "stripped" : "symtab";
  const std::string OptLabel =
      Result.Optimization == "default" ? "opt" : "noopt";
  const std::string VariantKey = "go" + Result.GoVersion + "-" + Result.GOOS +
                                 "-" + Result.GOARCH + "-" + Result.BuildMode +
                                 "-" + CgoLabel + "-" + LinkLabel + "-" +
                                 OptLabel;
  const std::string ExpectedPath =
      "corpus/go-eh/go" + Result.GoVersion + "/" + Result.GOOS + "-" +
      Result.GOARCH + "/" + Result.BuildMode + "/" + CgoLabel + "/" +
      LinkLabel + "/" + OptLabel + "/eh_probe-" + VariantKey +
      getArtifactExtension(Result.GOOS, Result.BuildMode).str();
  if (Result.Path != ExpectedPath)
    return manifestError(Context + ": artifact path disagrees with build axes");

  const json::Object *Build = Object.getObject("build");
  if (!Build)
    return manifestError(Context + ": missing object 'build'");
  auto Flags = requireStringArray(*Build, "flags", Context + ".build", false);
  if (!Flags)
    return Flags.takeError();
  // The link and compile flags are what the stripped and optimization axes
  // mean, so an artifact whose flags and axes disagree is not the variant its
  // path names.
  if (llvm::is_contained(*Flags, "-ldflags=-s -w") != Result.Stripped)
    return manifestError(Context + ".build: link flags disagree with the "
                                   "stripped axis");
  if (llvm::is_contained(*Flags, "-gcflags=all=-N -l") !=
      (Result.Optimization == "none"))
    return manifestError(Context + ".build: compile flags disagree with the "
                                   "optimization axis");
  auto Execution = requireString(*Build, "execution", Context + ".build");
  if (!Execution)
    return Execution.takeError();
  if (Result.BuildMode == "c-shared" && *Execution != "not-run-shared-object")
    return manifestError(Context +
                         ".build: a shared object cannot report an execution");

  const json::Object *Evidence = Object.getObject("evidence");
  if (!Evidence)
    return manifestError(Context + ": missing object 'evidence'");
  auto Sections = requireStringArray(*Evidence, "required_sections",
                                     Context + ".evidence", false);
  if (!Sections)
    return Sections.takeError();
  Result.RequiredSections = std::move(*Sections);
  auto PclnTabSection =
      requireString(*Evidence, "pclntab_section", Context + ".evidence");
  if (!PclnTabSection)
    return PclnTabSection.takeError();
  Result.PclnTabSection = std::move(*PclnTabSection);
  if (Result.PclnTabSection != getPclnTabSection(Result.GOOS, Result.BuildMode,
                                                 Release->RelroPclnTab))
    return manifestError(Context +
                         ".evidence: pclntab section disagrees with the "
                         "container and buildmode");
  if (!llvm::is_contained(Result.RequiredSections, Result.PclnTabSection))
    return manifestError(Context + ".evidence: the pclntab's own section is "
                                   "not required of the image");
  auto AtStart = requireBoolean(*Evidence, "pclntab_at_section_start",
                                Context + ".evidence");
  if (!AtStart)
    return AtStart.takeError();
  Result.PclnTabAtSectionStart = *AtStart;
  if (Result.PclnTabAtSectionStart == (Result.ObjectFormat == "pe"))
    return manifestError(Context +
                         ".evidence: pclntab placement disagrees with the "
                         "container");

  auto Magic = requireNonNegativeInteger(*Evidence, "pclntab_magic",
                                         Context + ".evidence");
  if (!Magic)
    return Magic.takeError();
  if (*Magic != Release->Magic)
    return manifestError(Context +
                         ".evidence: pclntab magic disagrees with the Go "
                         "release");
  Result.PclnTabMagic = static_cast<uint32_t>(*Magic);
  auto MinLC = requireNonNegativeInteger(*Evidence, "pclntab_min_lc",
                                         Context + ".evidence");
  if (!MinLC)
    return MinLC.takeError();
  if (*MinLC != Machine->second)
    return manifestError(Context +
                         ".evidence: pc quantum disagrees with GOARCH");
  Result.PclnTabMinLC = static_cast<uint8_t>(*MinLC);
  auto PointerSize = requireNonNegativeInteger(*Evidence, "pclntab_ptr_size",
                                               Context + ".evidence");
  if (!PointerSize)
    return PointerSize.takeError();
  if (*PointerSize != 8)
    return manifestError(Context +
                         ".evidence: the corpus carries only 64-bit targets");
  Result.PclnTabPointerSize = static_cast<uint8_t>(*PointerSize);
  auto FunctionCount = requireNonNegativeInteger(
      *Evidence, "pclntab_function_count", Context + ".evidence");
  if (!FunctionCount)
    return FunctionCount.takeError();
  Result.PclnTabFunctionCount = *FunctionCount;
  if (Result.PclnTabFunctionCount == 0)
    return manifestError(Context + ".evidence: the pclntab names no function");

  auto SymbolTable =
      requireString(*Evidence, "symbol_table", Context + ".evidence");
  if (!SymbolTable)
    return SymbolTable.takeError();
  Result.SymbolTable = std::move(*SymbolTable);
  if (Result.SymbolTable != "go-names" && Result.SymbolTable != "loader-only" &&
      Result.SymbolTable != "absent")
    return manifestError(Context + ".evidence: unsupported symbol table kind");
  const bool NamesGoFunctions = Result.SymbolTable == "go-names";
  // What `-s -w` leaves behind is settled for two of the three containers and
  // open for the third.  An unstripped link names its functions everywhere; a
  // stripped ELF or PE names none.  Mach-O sits in between and has moved:
  // through Go 1.20 the link still emitted an `LC_SYMTAB` carrying
  // `_go.func.*`, and by Go 1.26 it does not.  Where between those it changed
  // is not established, so both readings are accepted rather than one of them
  // being guessed at -- the artifact itself is checked against whichever the
  // manifest claims, which is the part that actually has to be true.
  const bool StrippedOfGoNames =
      Result.Stripped && Result.ObjectFormat != "macho";
  if (!Result.Stripped && !NamesGoFunctions)
    return manifestError(Context +
                         ".evidence: an unstripped artifact whose symbol table "
                         "names no Go function");
  if (StrippedOfGoNames && NamesGoFunctions)
    return manifestError(Context +
                         ".evidence: a stripped " + Result.ObjectFormat +
                         " artifact that still names Go functions");

  const json::Value *GoFunc = Evidence->get("gofunc_symbol");
  if (!GoFunc)
    return manifestError(Context + ".evidence: missing 'gofunc_symbol'");
  if (std::optional<StringRef> Name = GoFunc->getAsString()) {
    if (*Name != Release->GoFuncSymbol)
      return manifestError(Context +
                           ".evidence: funcdata base symbol disagrees with the "
                           "Go release");
    Result.GoFuncSymbol = Name->str();
  } else if (GoFunc->kind() != json::Value::Null) {
    return manifestError(Context +
                         ".evidence: 'gofunc_symbol' is neither a name nor "
                         "null");
  }
  // The two are one fact seen twice: a table that names Go functions is
  // exactly one that can name the funcdata base.  Tying them to each other
  // rather than to the stripped axis keeps the check decisive without
  // deciding what a stripped Mach-O link does.
  if (Result.GoFuncSymbol.empty() == NamesGoFunctions)
    return manifestError(Context +
                         ".evidence: funcdata base symbol disagrees with the "
                         "symbol table kind");

  auto NativeUnwind = requireStringArray(*Evidence, "native_unwind_sections",
                                         Context + ".evidence", true);
  if (!NativeUnwind)
    return NativeUnwind.takeError();
  Result.NativeUnwindSections = std::move(*NativeUnwind);
  // Go emits no platform unwind table for its own code, so anything here was
  // contributed by a C object and can only exist where cgo linked one.
  if (!Result.CgoEnabled && !Result.NativeUnwindSections.empty() &&
      Result.ObjectFormat != "pe")
    return manifestError(Context +
                         ".evidence: a pure Go image claims native unwind "
                         "tables");

  const json::Object *NeverD = Object.getObject("neverd");
  if (!NeverD)
    return manifestError(Context + ": missing object 'neverd'");
  auto Level = requireString(*NeverD, "validation_level", Context + ".neverd");
  if (!Level)
    return Level.takeError();
  if (*Level == "runtime-graph")
    Result.ValidationLevel = GoCorpusValidationLevel::RuntimeGraph;
  else if (*Level == "table-only")
    Result.ValidationLevel = GoCorpusValidationLevel::TableOnly;
  else
    return manifestError(Context + ": unsupported validation level");
  if (Result.ValidationLevel != Release->Level)
    return manifestError(Context +
                         ": validation level disagrees with the Go release");

  auto Statuses = requireStringArray(*NeverD, "allowed_parse_status",
                                     Context + ".neverd", false);
  if (!Statuses)
    return Statuses.takeError();
  Result.AllowedParseStatuses = std::move(*Statuses);
  for (const std::string &Status : Result.AllowedParseStatuses)
    if (Status != "complete" && Status != "partial")
      return manifestError(Context + ".neverd: unsupported parse status");
  // The Go 1.2 header spans releases whose `_func` records differ, so nothing
  // read through it can be claimed complete.
  if (Result.ValidationLevel == GoCorpusValidationLevel::TableOnly &&
      Result.AllowedParseStatuses != std::vector<std::string>{"partial"})
    return manifestError(Context +
                         ".neverd: a table-only contract allows a complete "
                         "parse");

  auto Version =
      requireString(*NeverD, "expected_pclntab_version", Context + ".neverd");
  if (!Version)
    return Version.takeError();
  Result.ExpectedPclnTabVersion = std::move(*Version);
  if (Result.ExpectedPclnTabVersion != Release->PclnTabVersion)
    return manifestError(Context +
                         ".neverd: pclntab version disagrees with the Go "
                         "release");

  struct Minimum {
    StringRef Key;
    uint64_t *Field;
  };
  const Minimum Minimums[] = {
      {"min_go_functions", &Result.MinGoFunctions},
      {"min_defer_sites", &Result.MinDeferSites},
      {"min_recover_sites", &Result.MinRecoverSites},
      {"min_panic_sites", &Result.MinPanicSites},
      {"min_open_coded_defer_funcs", &Result.MinOpenCodedDeferFuncs},
  };
  for (const Minimum &Entry : Minimums) {
    auto Value =
        requireNonNegativeInteger(*NeverD, Entry.Key, Context + ".neverd");
    if (!Value)
      return Value.takeError();
    *Entry.Field = *Value;
  }
  if (Result.MinGoFunctions == 0)
    return manifestError(Context + ".neverd: the contract claims no function");
  // `-N` clears `ssagen.hasOpenDefers` for every function, so no frame carries
  // `FUNCDATA_OpenCodedDeferInfo` and every defer lowers to a runtime call.
  if ((Result.MinOpenCodedDeferFuncs != 0) !=
      (Result.Optimization == "default"))
    return manifestError(Context +
                         ".neverd: open-coded defer claim disagrees with the "
                         "optimization axis");

  auto RequiresModuleData =
      requireBoolean(*NeverD, "requires_moduledata", Context + ".neverd");
  if (!RequiresModuleData)
    return RequiresModuleData.takeError();
  Result.RequiresModuleData = *RequiresModuleData;
  if (Result.RequiresModuleData != Release->RequiresModuleData)
    return manifestError(Context +
                         ".neverd: moduledata requirement disagrees with the "
                         "Go release");

  return Result;
}

/// The Go matrix is a curated list of variants rather than a cross product --
/// most axes are only exercised where they change a decoder path -- so
/// completeness is stated as the capabilities the corpus must cover.  Pinning
/// an artifact count instead would make every new focus variant a test change
/// while still not saying which decoder path went missing.
Error verifyCapabilityCoverage(ArrayRef<GoEHArtifactExpectation> Expectations) {
  std::set<std::string> Generations;
  std::set<std::string> ObjectFormats;
  std::set<std::string> Machines;
  std::set<std::string> BuildModes;
  std::set<std::string> Optimizations;
  std::set<bool> CgoStates;
  std::set<bool> StripStates;
  for (const GoEHArtifactExpectation &Expectation : Expectations) {
    Generations.insert(Expectation.ExpectedPclnTabVersion);
    ObjectFormats.insert(Expectation.ObjectFormat);
    Machines.insert(Expectation.GOARCH);
    BuildModes.insert(Expectation.BuildMode);
    Optimizations.insert(Expectation.Optimization);
    CgoStates.insert(Expectation.CgoEnabled);
    StripStates.insert(Expectation.Stripped);
  }
  if (Generations !=
      std::set<std::string>{"go1.2", "go1.16", "go1.18", "go1.20"})
    return manifestError(
        "corpus manifest does not cover every pclntab generation");
  if (ObjectFormats != std::set<std::string>{"elf", "pe", "macho"})
    return manifestError("corpus manifest does not cover every container");
  if (Machines != std::set<std::string>{"amd64", "arm64"})
    return manifestError("corpus manifest does not cover both pc quanta");
  if (BuildModes != std::set<std::string>{"exe", "pie", "c-shared"})
    return manifestError("corpus manifest does not cover every buildmode");
  if (Optimizations != std::set<std::string>{"default", "none"})
    return manifestError(
        "corpus manifest does not cover both open-coded defer states");
  if (CgoStates != std::set<bool>{false, true})
    return manifestError("corpus manifest does not cover both cgo states");
  if (StripStates != std::set<bool>{false, true})
    return manifestError("corpus manifest does not cover both link states");
  return Error::success();
}

} // namespace

Expected<std::vector<GoEHArtifactExpectation>>
parseGoEHCorpusManifest(StringRef Contents, bool RequireCompleteMatrix) {
  auto Parsed = json::parse(Contents);
  if (!Parsed)
    return Parsed.takeError();
  const json::Object *Root = Parsed->getAsObject();
  if (!Root)
    return manifestError("corpus manifest root is not an object");
  if (Root->getInteger("schema_version") != 1 ||
      Root->getString("corpus") != "go-eh")
    return manifestError("unsupported corpus manifest identity");
  const json::Array *Artifacts = Root->getArray("artifacts");
  if (!Artifacts || Artifacts->empty())
    return manifestError("corpus manifest contains no artifacts");

  std::vector<GoEHArtifactExpectation> Result;
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
    if (Error Failure = verifyCapabilityCoverage(Result))
      return std::move(Failure);
  return Result;
}

Expected<std::vector<GoEHArtifactExpectation>>
loadGoEHCorpusManifest(StringRef Path, bool RequireCompleteMatrix) {
  auto BufferOrErr = MemoryBuffer::getFile(Path);
  if (!BufferOrErr)
    return manifestError("cannot read corpus manifest '" + Path +
                         "': " + BufferOrErr.getError().message());
  return parseGoEHCorpusManifest((*BufferOrErr)->getBuffer(),
                                 RequireCompleteMatrix);
}

} // namespace neverd::test
