//===- ObjCEHCorpusTests.cpp - Objective-C EH corpus tests ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace llvm;
using namespace neverd;

namespace {

struct ObjCArtifact {
  std::string Path;
  std::string Target;
  std::string Program;
  std::string Exceptions;
  std::string Execution;
  std::string Architecture;
  std::string ObjectFormat;
  std::string SHA256;
  bool Stripped = false;
  uint64_t Size = 0;

  bool ExpectARC = false;
  bool ExpectNoLSDA = false;
  bool ExpectRuntimeProven = false;
  uint64_t MinExceptionFunctions = 0;
  uint64_t MinLandingPads = 0;
  uint64_t MinCatchClauses = 0;
  uint64_t MinClassClauses = 0;
  uint64_t MinAnyObjectClauses = 0;
  uint64_t MinCatchAllClauses = 0;
  uint64_t MinCatchFrames = 0;
  uint64_t MinCleanupFrames = 0;
  uint64_t MinSynchronizedFrames = 0;
  uint64_t MinThrowSites = 0;
  std::vector<std::string> RequiredClassNames;
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

Expected<uint64_t> requireUInt(const json::Object &Object, StringRef Key,
                               StringRef Context) {
  std::optional<int64_t> Value = Object.getInteger(Key);
  if (!Value || *Value < 0)
    return manifestError(Context + ": missing non-negative integer '" + Key +
                         "'");
  return static_cast<uint64_t>(*Value);
}

Expected<std::vector<std::string>>
requireStringArray(const json::Object &Object, StringRef Key,
                   StringRef Context) {
  const json::Array *Values = Object.getArray(Key);
  if (!Values)
    return manifestError(Context + ": missing string array '" + Key + "'");
  std::vector<std::string> Result;
  Result.reserve(Values->size());
  for (const json::Value &Value : *Values) {
    std::optional<StringRef> String = Value.getAsString();
    if (!String || String->empty())
      return manifestError(Context + ": invalid value in '" + Key + "'");
    Result.push_back(String->str());
  }
  return Result;
}

Expected<std::vector<ObjCArtifact>> loadExpectations() {
  const std::filesystem::path ManifestPath =
      std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) / "manifests" /
      "objc-eh.json";
  auto BufferOrErr = MemoryBuffer::getFile(ManifestPath.string());
  if (!BufferOrErr)
    return errorCodeToError(BufferOrErr.getError());
  Expected<json::Value> Parsed = json::parse((*BufferOrErr)->getBuffer());
  if (!Parsed)
    return Parsed.takeError();
  const json::Object *Root = Parsed->getAsObject();
  if (!Root || Root->getString("corpus") != std::optional<StringRef>("objc-eh"))
    return manifestError("invalid Objective-C EH corpus root");
  const json::Array *Artifacts = Root->getArray("artifacts");
  if (!Artifacts)
    return manifestError("Objective-C EH manifest has no artifacts");

  std::vector<ObjCArtifact> Result;
  Result.reserve(Artifacts->size());
  for (size_t I = 0; I < Artifacts->size(); ++I) {
    const json::Object *Object = (*Artifacts)[I].getAsObject();
    const std::string Context = "artifact[" + std::to_string(I) + "]";
    if (!Object)
      return manifestError(Context + ": expected object");
    const json::Object *NeverD = Object->getObject("neverd");
    if (!NeverD)
      return manifestError(Context + ": missing neverd contract");

    ObjCArtifact Artifact;
#define READ_STRING(FIELD, OBJECT, KEY)                                      \
  do {                                                                       \
    auto Value = requireString(OBJECT, KEY, Context);                         \
    if (!Value)                                                              \
      return Value.takeError();                                               \
    Artifact.FIELD = std::move(*Value);                                       \
  } while (false)
#define READ_BOOL(FIELD, OBJECT, KEY)                                        \
  do {                                                                       \
    auto Value = requireBoolean(OBJECT, KEY, Context);                        \
    if (!Value)                                                              \
      return Value.takeError();                                               \
    Artifact.FIELD = *Value;                                                  \
  } while (false)
#define READ_UINT(FIELD, OBJECT, KEY)                                        \
  do {                                                                       \
    auto Value = requireUInt(OBJECT, KEY, Context);                           \
    if (!Value)                                                              \
      return Value.takeError();                                               \
    Artifact.FIELD = *Value;                                                  \
  } while (false)
    READ_STRING(Path, *Object, "path");
    READ_STRING(Target, *Object, "target");
    READ_STRING(Program, *Object, "program");
    READ_STRING(Exceptions, *Object, "exceptions");
    READ_STRING(Execution, *Object, "execution");
    READ_STRING(Architecture, *Object, "architecture");
    READ_STRING(ObjectFormat, *Object, "object_format");
    READ_STRING(SHA256, *Object, "sha256");
    READ_BOOL(Stripped, *Object, "stripped");
    READ_UINT(Size, *Object, "size");

    READ_BOOL(ExpectARC, *NeverD, "expect_arc");
    READ_BOOL(ExpectNoLSDA, *NeverD, "expect_no_lsda");
    READ_BOOL(ExpectRuntimeProven, *NeverD,
              "expect_runtime_proven_by_personality");
    READ_UINT(MinExceptionFunctions, *NeverD, "min_exception_functions");
    READ_UINT(MinLandingPads, *NeverD, "min_landing_pads");
    READ_UINT(MinCatchClauses, *NeverD, "min_catch_clauses");
    READ_UINT(MinClassClauses, *NeverD, "min_class_clauses");
    READ_UINT(MinAnyObjectClauses, *NeverD, "min_any_object_clauses");
    READ_UINT(MinCatchAllClauses, *NeverD, "min_catch_all_clauses");
    READ_UINT(MinCatchFrames, *NeverD, "min_catch_frames");
    READ_UINT(MinCleanupFrames, *NeverD, "min_cleanup_frames");
    READ_UINT(MinSynchronizedFrames, *NeverD, "min_synchronized_frames");
    READ_UINT(MinThrowSites, *NeverD, "min_throw_sites");
#undef READ_UINT
#undef READ_BOOL
#undef READ_STRING

    auto Classes =
        requireStringArray(*NeverD, "required_class_names", Context);
    if (!Classes)
      return Classes.takeError();
    Artifact.RequiredClassNames = std::move(*Classes);
    Result.push_back(std::move(Artifact));
  }
  return Result;
}

std::optional<BinaryImage> loadArtifact(const std::filesystem::path &Path) {
  std::unique_ptr<Loader> ImageLoader = Loader::create(Path);
  if (!ImageLoader) {
    ADD_FAILURE() << "NeverD did not recognize " << Path.string();
    return std::nullopt;
  }
  auto ImageOrErr = ImageLoader->load(Path);
  if (!ImageOrErr) {
    ADD_FAILURE() << "cannot load " << Path.string() << ": "
                  << toString(ImageOrErr.takeError());
    return std::nullopt;
  }
  return std::move(*ImageOrErr);
}

struct ObjCCensus {
  uint64_t ExceptionFunctions = 0;
  uint64_t LandingPads = 0;
  uint64_t CatchClauses = 0;
  uint64_t ClassClauses = 0;
  uint64_t AnyObjectClauses = 0;
  uint64_t CatchAllClauses = 0;
  std::set<std::string> ClassNames;
};

ObjCCensus censusOf(const ExceptionInfo &Info) {
  ObjCCensus Census;
  for (const ExceptionFunction &Function : Info.Functions) {
    if (!Function.ObjC)
      continue;
    ++Census.ExceptionFunctions;
    for (const ObjCLandingPad &Pad : Function.ObjC->LandingPads) {
      ++Census.LandingPads;
      for (const ObjCCatchClause &Clause : Pad.Catches) {
        ++Census.CatchClauses;
        switch (Clause.Kind) {
        case ObjCCatchKind::Class:
          ++Census.ClassClauses;
          if (!Clause.ClassName.empty())
            Census.ClassNames.insert(Clause.ClassName);
          break;
        case ObjCCatchKind::AnyObject:
          ++Census.AnyObjectClauses;
          break;
        case ObjCCatchKind::CatchAll:
          ++Census.CatchAllClauses;
          break;
        }
      }
    }
  }
  return Census;
}

TEST(ObjCEHCorpus, DeclaresCompleteAppleBuildMatrix) {
  auto ArtifactsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ArtifactsOrErr))
      << toString(ArtifactsOrErr.takeError());
  EXPECT_EQ(ArtifactsOrErr->size(), 12u);

  std::map<std::string, unsigned> PerTarget;
  std::set<std::string> Programs;
  for (const ObjCArtifact &Artifact : *ArtifactsOrErr) {
    ++PerTarget[Artifact.Target];
    Programs.insert(Artifact.Program);
    EXPECT_EQ(Artifact.ObjectFormat, "macho");
  }
  EXPECT_EQ(PerTarget,
            (std::map<std::string, unsigned>{
                {"arm64-apple-darwin", 6},
                {"x86_64-apple-darwin", 6},
            }));
  EXPECT_EQ(Programs,
            (std::set<std::string>{"objc_eh_probe", "objc_eh_probe_mrr",
                                   "objc_eh_probe_noexc"}));
}

TEST(ObjCEHCorpus, MatchesBytesAndRecoversDeclaredExceptionGraph) {
  auto ArtifactsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ArtifactsOrErr))
      << toString(ArtifactsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  for (const ObjCArtifact &Artifact : *ArtifactsOrErr) {
    SCOPED_TRACE(Artifact.Path);
    const std::filesystem::path Path = CorpusRoot / Artifact.Path;
    auto BufferOrErr = MemoryBuffer::getFile(Path.string());
    ASSERT_TRUE(static_cast<bool>(BufferOrErr));
    StringRef Bytes = (*BufferOrErr)->getBuffer();
    ASSERT_EQ(Bytes.size(), Artifact.Size);
    const std::array<uint8_t, 32> Digest =
        SHA256::hash(arrayRefFromStringRef(Bytes));
    EXPECT_EQ(toHex(ArrayRef<uint8_t>(Digest), true), Artifact.SHA256);

    std::optional<BinaryImage> Image = loadArtifact(Path);
    if (!Image)
      continue;
    EXPECT_EQ(Image->Format, BinaryFormat::MachO);
    EXPECT_EQ(Image->Arch,
              Artifact.Architecture == "aarch64" ? Arch::AArch64 : Arch::X64);

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    ASSERT_TRUE(Info.ObjCRuntime.has_value());
    EXPECT_EQ(Info.ObjCRuntime->Runtime, ObjCRuntimeKind::AppleNonFragile);
    EXPECT_EQ(Info.ObjCRuntime->UsesARC, Artifact.ExpectARC);
    EXPECT_EQ(Info.ObjCRuntime->RuntimeProvenByPersonality,
              Artifact.ExpectRuntimeProven);

    const ObjCCensus Census = censusOf(Info);
    EXPECT_GE(Census.ExceptionFunctions, Artifact.MinExceptionFunctions);
    EXPECT_GE(Census.LandingPads, Artifact.MinLandingPads);
    EXPECT_GE(Census.CatchClauses, Artifact.MinCatchClauses);
    EXPECT_GE(Census.ClassClauses, Artifact.MinClassClauses);
    EXPECT_GE(Census.AnyObjectClauses, Artifact.MinAnyObjectClauses);
    EXPECT_GE(Census.CatchAllClauses, Artifact.MinCatchAllClauses);
    EXPECT_GE(Info.ObjCRuntime->CatchFrames, Artifact.MinCatchFrames);
    EXPECT_GE(Info.ObjCRuntime->CleanupFrames, Artifact.MinCleanupFrames);
    EXPECT_GE(Info.ObjCRuntime->SynchronizedFrames,
              Artifact.MinSynchronizedFrames);
    EXPECT_GE(Info.ObjCRuntime->ThrowSites, Artifact.MinThrowSites);
    for (const std::string &ClassName : Artifact.RequiredClassNames)
      EXPECT_TRUE(Census.ClassNames.contains(ClassName))
          << "missing Objective-C catch class " << ClassName;

    if (Artifact.ExpectNoLSDA) {
      EXPECT_EQ(Census.ExceptionFunctions, 0u);
      EXPECT_EQ(Census.LandingPads, 0u);
      for (const ExceptionFunction &Function : Info.Functions)
        EXPECT_FALSE(Function.ObjC.has_value());
    }
  }
}

TEST(ObjCEHCorpus, RewritesAndRunsEveryHostMachOVariant) {
#if !defined(__APPLE__) ||                                                   \
    (!defined(__aarch64__) && !defined(__x86_64__))
  GTEST_SKIP() << "the committed Objective-C probes require their host ISA";
#else
#if defined(__aarch64__)
  constexpr StringLiteral HostTarget = "arm64-apple-darwin";
#else
  constexpr StringLiteral HostTarget = "x86_64-apple-darwin";
#endif

  auto ArtifactsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ArtifactsOrErr))
      << toString(ArtifactsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);
  unsigned Rewritten = 0;
  unsigned RewrittenStripped = 0;

  for (const ObjCArtifact &Artifact : *ArtifactsOrErr) {
    if (Artifact.Target != HostTarget)
      continue;
    ++Rewritten;
    RewrittenStripped += Artifact.Stripped;
    SCOPED_TRACE(Artifact.Path);

    const std::filesystem::path Input = CorpusRoot / Artifact.Path;
    std::optional<BinaryImage> Original = loadArtifact(Input);
    ASSERT_TRUE(Original.has_value());
    const uint8_t *OriginalEntry = Original->readVA(Original->Entry, 4);
    ASSERT_NE(OriginalEntry, nullptr);
    std::array<uint8_t, 4> OriginalBytes{};
    std::memcpy(OriginalBytes.data(), OriginalEntry, OriginalBytes.size());

    SmallString<128> Output;
    ASSERT_FALSE(
        sys::fs::createTemporaryFile("neverd-objc-eh", "patched", Output));
    FileRemover RemoveOutput(Output);
    ASSERT_FALSE(sys::fs::remove(Output));

    const std::string InputString = Input.string();
    const std::string OutputString = Output.str().str();
    SmallVector<StringRef, 6> PatchArgs{
        NEVERD_BINARY, "patch", InputString, "-o", OutputString};
    std::string Error;
    ASSERT_EQ(sys::ExecuteAndWait(NEVERD_BINARY, PatchArgs, std::nullopt, {}, 0,
                                  0, &Error),
              0)
        << Error;

    std::optional<BinaryImage> Patched = loadArtifact(OutputString);
    ASSERT_TRUE(Patched.has_value());
    const uint8_t *PatchedEntry = Patched->readVA(Original->Entry, 4);
    ASSERT_NE(PatchedEntry, nullptr);
    EXPECT_FALSE(
        std::equal(OriginalBytes.begin(), OriginalBytes.end(), PatchedEntry))
        << "the original entry still contains the unrewritten instructions";
#if defined(__aarch64__)
    uint32_t Branch = 0;
    std::memcpy(&Branch, PatchedEntry, sizeof(Branch));
    EXPECT_EQ(Branch & 0xfc000000u, 0x14000000u);
#else
    EXPECT_EQ(PatchedEntry[0], 0xe9);
#endif

    SmallVector<StringRef, 1> RunArgs{OutputString};
    EXPECT_EQ(sys::ExecuteAndWait(OutputString, RunArgs, std::nullopt, {}, 0, 0,
                                  &Error),
              0)
        << Error;
  }
  EXPECT_EQ(Rewritten, 6u);
  EXPECT_EQ(RewrittenStripped, 2u);
#endif
}

} // namespace
