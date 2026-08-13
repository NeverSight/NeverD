//===- WindowsEHCorpusManifest.cpp - Corpus manifest reader -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "WindowsEHCorpusManifest.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

using namespace llvm;

namespace neverd::test {
namespace {

struct ArtifactIdentity {
  StringRef Suite;
  StringRef Extension;
  StringRef Kind;
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

std::optional<ArtifactIdentity> getArtifactIdentity(StringRef Name) {
  if (Name == "xcpt4")
    return ArtifactIdentity{"windows-seh-tests", ".exe", "mixed"};
  if (Name == "nested_collided" || Name == "xframe_eh_exe")
    return ArtifactIdentity{"windows-seh-tests", ".exe", "seh"};
  if (Name == "xframe_eh_dll")
    return ArtifactIdentity{"windows-seh-tests", ".dll", "seh"};
  if (Name == "seh_probe")
    return ArtifactIdentity{"abi-probe", ".exe", "seh"};
  if (Name == "cxx_eh_probe")
    return ArtifactIdentity{"abi-probe", ".exe", "cxx"};
  return std::nullopt;
}

Expected<std::pair<Arch, StringRef>> getArchitecture(StringRef Name) {
  if (Name == "x86")
    return std::pair{Arch::X86, StringRef("i686-pc-windows-msvc")};
  if (Name == "x86_64")
    return std::pair{Arch::X64, StringRef("x86_64-pc-windows-msvc")};
  if (Name == "arm")
    return std::pair{Arch::ARM, StringRef("thumbv7-pc-windows-msvc")};
  if (Name == "aarch64")
    return std::pair{Arch::AArch64, StringRef("aarch64-pc-windows-msvc")};
  return manifestError("unsupported corpus architecture '" + Name + "'");
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

std::string cellKey(StringRef Toolchain, StringRef Architecture,
                    StringRef CxxFormat, bool SecurityCookie,
                    StringRef Optimization) {
  return (Toolchain + "|" + Architecture + "|" + CxxFormat + "|" +
          (SecurityCookie ? "gs" : "no-gs") + "|" + Optimization)
      .str();
}

Expected<WindowsEHArtifactExpectation> parseArtifact(const json::Object &Object,
                                                     size_t Index) {
  const std::string Context = "artifacts[" + std::to_string(Index) + "]";
  WindowsEHArtifactExpectation Result;

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

  auto Name = requireString(Object, "name", Context);
  if (!Name)
    return Name.takeError();
  Result.Name = std::move(*Name);
  std::optional<ArtifactIdentity> Identity = getArtifactIdentity(Result.Name);
  if (!Identity)
    return manifestError(Context + ": unsupported artifact name");
  auto Suite = requireString(Object, "suite", Context);
  if (!Suite)
    return Suite.takeError();
  auto Kind = requireString(Object, "kind", Context);
  if (!Kind)
    return Kind.takeError();
  if (*Suite != Identity->Suite || *Kind != Identity->Kind)
    return manifestError(Context + ": artifact identity is inconsistent");

  auto Architecture = requireString(Object, "architecture", Context);
  if (!Architecture)
    return Architecture.takeError();
  Result.Architecture = std::move(*Architecture);
  auto ArchitectureInfo = getArchitecture(Result.Architecture);
  if (!ArchitectureInfo)
    return ArchitectureInfo.takeError();
  Result.ExpectedArch = ArchitectureInfo->first;

  const json::Object *Build = Object.getObject("build");
  if (!Build)
    return manifestError(Context + ": missing object 'build'");
  auto Toolchain = requireString(*Build, "toolchain", Context + ".build");
  if (!Toolchain)
    return Toolchain.takeError();
  Result.Toolchain = std::move(*Toolchain);
  if (Result.Toolchain != "msvc" && Result.Toolchain != "clang-cl")
    return manifestError(Context + ": unsupported toolchain");
  auto Optimization = requireString(*Build, "optimization", Context + ".build");
  if (!Optimization)
    return Optimization.takeError();
  Result.Optimization = std::move(*Optimization);
  if (Result.Optimization != "o0" && Result.Optimization != "o2")
    return manifestError(Context + ": unsupported optimization");
  auto CxxFormat = requireString(*Build, "cxx_format", Context + ".build");
  if (!CxxFormat)
    return CxxFormat.takeError();
  Result.CxxFormat = std::move(*CxxFormat);
  std::optional<bool> SecurityCookie = Build->getBoolean("security_cookie");
  if (!SecurityCookie)
    return manifestError(Context + ".build: missing security_cookie");
  Result.SecurityCookie = *SecurityCookie;
  auto Execution = requireString(*Build, "execution", Context + ".build");
  if (!Execution)
    return Execution.takeError();
  Result.Execution = std::move(*Execution);
  auto TargetTriple =
      requireString(*Build, "target_triple", Context + ".build");
  if (!TargetTriple)
    return TargetTriple.takeError();
  if (*TargetTriple != ArchitectureInfo->second)
    return manifestError(Context +
                         ": target triple disagrees with architecture");

  const bool IsX64 = Result.Architecture == "x86_64";
  const bool IsNativeExecution =
      Result.Architecture == "x86" || Result.Architecture == "x86_64";
  if (Result.Execution !=
      (IsNativeExecution ? "passed" : "not-run-cross-target"))
    return manifestError(Context + ": execution status disagrees with target");
  if (IsX64) {
    if (Result.Toolchain == "msvc") {
      if (Result.CxxFormat != "fh3" && Result.CxxFormat != "fh4")
        return manifestError(Context + ": unsupported MSVC x64 EH format");
    } else if (Result.CxxFormat != "fh3") {
      return manifestError(Context +
                           ": clang-cl x64 supports only EH3 corpus cells");
    }
  } else if (Result.CxxFormat != "native") {
    return manifestError(Context + ": non-x64 corpus cell must use native EH");
  }

  const json::Object *Compiler = Build->getObject("compiler");
  const json::Object *Linker = Build->getObject("linker");
  if (!Compiler || !Linker)
    return manifestError(Context + ": missing per-artifact tool identity");
  auto CompilerName =
      requireString(*Compiler, "name", Context + ".build.compiler");
  if (!CompilerName)
    return CompilerName.takeError();
  auto LinkerName = requireString(*Linker, "name", Context + ".build.linker");
  if (!LinkerName)
    return LinkerName.takeError();
  const StringRef ExpectedCompiler =
      Result.Toolchain == "msvc" ? "cl.exe" : "clang-cl.exe";
  const StringRef ExpectedLinker =
      Result.Toolchain == "msvc" ? "link.exe" : "lld-link.exe";
  if (*CompilerName != ExpectedCompiler || *LinkerName != ExpectedLinker)
    return manifestError(Context + ": tool identity disagrees with toolchain");
  auto CompilerProductVersion =
      requireString(*Compiler, "product_version", Context + ".build.compiler");
  if (!CompilerProductVersion)
    return CompilerProductVersion.takeError();
  auto CompilerFileVersion =
      requireString(*Compiler, "file_version", Context + ".build.compiler");
  if (!CompilerFileVersion)
    return CompilerFileVersion.takeError();
  auto LinkerProductVersion =
      requireString(*Linker, "product_version", Context + ".build.linker");
  if (!LinkerProductVersion)
    return LinkerProductVersion.takeError();
  auto LinkerFileVersion =
      requireString(*Linker, "file_version", Context + ".build.linker");
  if (!LinkerFileVersion)
    return LinkerFileVersion.takeError();

  const std::string CookieLabel = Result.SecurityCookie ? "gs" : "no-gs";
  const std::string ExpectedFilename =
      Result.Name + "-" + Result.Toolchain + "-" + Result.Architecture + "-" +
      Result.CxxFormat + "-" + CookieLabel + "-" + Result.Optimization +
      Identity->Extension.str();
  const std::string ExpectedPath =
      "corpus/windows-eh/" + Result.Toolchain + "/" + Result.Architecture +
      "/" + Result.CxxFormat + "/" + CookieLabel + "/" + Result.Optimization +
      "/" + Identity->Suite.str() + "/" + ExpectedFilename;
  if (Result.Path != ExpectedPath)
    return manifestError(Context + ": artifact path disagrees with build axes");

  const json::Object *NeverD = Object.getObject("neverd");
  if (!NeverD)
    return manifestError(Context + ": missing object 'neverd'");
  auto Level = requireString(*NeverD, "validation_level", Context + ".neverd");
  if (!Level)
    return Level.takeError();
  if (*Level == "exception-graph")
    Result.ValidationLevel = CorpusValidationLevel::ExceptionGraph;
  else if (*Level == "unwind-only")
    Result.ValidationLevel = CorpusValidationLevel::UnwindOnly;
  else if (*Level == "load-only")
    Result.ValidationLevel = CorpusValidationLevel::LoadOnly;
  else
    return manifestError(Context + ": unsupported validation level");

  const CorpusValidationLevel ExpectedLevel =
      IsX64                          ? CorpusValidationLevel::ExceptionGraph
      : Result.Architecture == "x86" ? CorpusValidationLevel::LoadOnly
                                     : CorpusValidationLevel::UnwindOnly;
  if (Result.ValidationLevel != ExpectedLevel)
    return manifestError(Context +
                         ": validation level disagrees with architecture");

  auto Statuses = requireStringArray(*NeverD, "allowed_parse_status",
                                     Context + ".neverd", false);
  if (!Statuses)
    return Statuses.takeError();
  Result.AllowedParseStatuses = std::move(*Statuses);
  auto Personalities = requireStringArray(*NeverD, "personalities_any",
                                          Context + ".neverd", true);
  if (!Personalities)
    return Personalities.takeError();
  Result.Personalities = std::move(*Personalities);
  auto MinFunctions = requireNonNegativeInteger(
      *NeverD, "min_exception_functions", Context + ".neverd");
  if (!MinFunctions)
    return MinFunctions.takeError();
  Result.MinExceptionFunctions = *MinFunctions;
  auto MinCxx = requireNonNegativeInteger(*NeverD, "min_cxx_functions",
                                          Context + ".neverd");
  if (!MinCxx)
    return MinCxx.takeError();
  Result.MinCxxFunctions = *MinCxx;
  auto MinTry =
      requireNonNegativeInteger(*NeverD, "min_try_blocks", Context + ".neverd");
  if (!MinTry)
    return MinTry.takeError();
  Result.MinTryBlocks = *MinTry;
  auto MinSEH =
      requireNonNegativeInteger(*NeverD, "min_seh_scopes", Context + ".neverd");
  if (!MinSEH)
    return MinSEH.takeError();
  Result.MinSEHScopes = *MinSEH;

  if (Result.ValidationLevel == CorpusValidationLevel::LoadOnly &&
      (!Result.Personalities.empty() || Result.MinExceptionFunctions != 0 ||
       Result.MinCxxFunctions != 0 || Result.MinTryBlocks != 0 ||
       Result.MinSEHScopes != 0))
    return manifestError(Context +
                         ": load-only contract claims exception parsing");
  if (Result.ValidationLevel == CorpusValidationLevel::UnwindOnly &&
      (!Result.Personalities.empty() || Result.MinExceptionFunctions == 0 ||
       Result.MinCxxFunctions != 0 || Result.MinTryBlocks != 0 ||
       Result.MinSEHScopes != 0))
    return manifestError(Context + ": unwind-only contract is inconsistent");

  return Result;
}

std::map<std::string, std::set<std::string>> expectedInventory() {
  std::map<std::string, std::set<std::string>> Result;
  const std::set<std::string> FullNames{
      "xcpt4",         "nested_collided", "xframe_eh_dll",
      "xframe_eh_exe", "seh_probe",       "cxx_eh_probe",
  };
  const std::set<std::string> NativeClangNames{"nested_collided", "seh_probe",
                                               "cxx_eh_probe"};
  for (StringRef Toolchain : {"msvc", "clang-cl"}) {
    for (StringRef Architecture : {"x86", "x86_64", "arm", "aarch64"}) {
      if (Toolchain == "clang-cl" && Architecture == "arm")
        continue;
      SmallVector<StringRef, 2> Formats;
      if (Architecture != "x86_64")
        Formats.push_back("native");
      else if (Toolchain == "msvc") {
        Formats.push_back("fh3");
        Formats.push_back("fh4");
      } else
        Formats.push_back("fh3");
      for (StringRef Format : Formats)
        for (bool SecurityCookie : {false, true})
          for (StringRef Optimization : {"o0", "o2"})
            Result.emplace(cellKey(Toolchain, Architecture, Format,
                                   SecurityCookie, Optimization),
                           Toolchain == "clang-cl" && (Architecture == "x86" ||
                                                       Architecture == "x86_64")
                               ? NativeClangNames
                               : FullNames);
    }
  }
  return Result;
}

Error verifyCompleteMatrix(
    ArrayRef<WindowsEHArtifactExpectation> Expectations) {
  std::map<std::string, std::set<std::string>> NamesByCell;
  for (const WindowsEHArtifactExpectation &Expectation : Expectations) {
    std::string Key = cellKey(Expectation.Toolchain, Expectation.Architecture,
                              Expectation.CxxFormat, Expectation.SecurityCookie,
                              Expectation.Optimization);
    if (!NamesByCell[Key].insert(Expectation.Name).second)
      return manifestError("duplicate artifact name in corpus matrix cell");
  }
  const std::map<std::string, std::set<std::string>> Expected =
      expectedInventory();
  if (NamesByCell.size() != Expected.size())
    return manifestError(
        "corpus manifest does not contain the complete capability matrix");
  for (const auto &[Key, Names] : NamesByCell) {
    const auto ExpectedCell = Expected.find(Key);
    if (ExpectedCell == Expected.end())
      return manifestError("corpus manifest contains unsupported matrix cell");
    if (Names != ExpectedCell->second)
      return manifestError(
          "corpus matrix cell disagrees with its capability inventory");
  }
  size_t ExpectedArtifactCount = 0;
  for (const auto &Entry : Expected)
    ExpectedArtifactCount += Entry.second.size();
  if (Expectations.size() != ExpectedArtifactCount)
    return manifestError(
        "corpus manifest artifact count disagrees with its capability matrix");
  return Error::success();
}

} // namespace

Expected<std::vector<WindowsEHArtifactExpectation>>
parseWindowsEHCorpusManifest(StringRef Contents, bool RequireCompleteMatrix) {
  auto Parsed = json::parse(Contents);
  if (!Parsed)
    return Parsed.takeError();
  const json::Object *Root = Parsed->getAsObject();
  if (!Root)
    return manifestError("corpus manifest root is not an object");
  if (Root->getInteger("schema_version") != 2 ||
      Root->getString("corpus") != "windows-eh")
    return manifestError("unsupported corpus manifest identity");
  const json::Array *Artifacts = Root->getArray("artifacts");
  if (!Artifacts || Artifacts->empty())
    return manifestError("corpus manifest contains no artifacts");

  std::vector<WindowsEHArtifactExpectation> Result;
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
    if (Error Error = verifyCompleteMatrix(Result))
      return std::move(Error);
  return Result;
}

Expected<std::vector<WindowsEHArtifactExpectation>>
loadWindowsEHCorpusManifest(StringRef Path, bool RequireCompleteMatrix) {
  auto BufferOrErr = MemoryBuffer::getFile(Path);
  if (!BufferOrErr)
    return manifestError("cannot read corpus manifest '" + Path +
                         "': " + BufferOrErr.getError().message());
  return parseWindowsEHCorpusManifest((*BufferOrErr)->getBuffer(),
                                      RequireCompleteMatrix);
}

} // namespace neverd::test
