//===- SignatureCacheTests.cpp - Parsed signature file cache tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sigs/PatternParser.h"
#include "neverd/sigs/SignatureCache.h"
#include "neverd/sigs/SignatureDB.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::sigs;

namespace {

#define NEVERD_SIGNATURE_CACHE_STRING(Name, Value)                             \
  [[maybe_unused]] constexpr llvm::StringLiteral Name(Value);
#include "neverd/sigs/SignatureCache.def"

/// Lines as the generator writes them: wildcards, CRC spans, tails, names
/// at one offset, and references, alternatives among them.
constexpr llvm::StringLiteral Lines =
    "40534883EC2033D2488BD94881C1EC000000448D4206E8........48837B4000 0F 6061 "
    "0041 !refs :0000 ?ResetKey@CMFCAcceleratorKeyAssignCtrl@@QEAAXXZ ^0017 "
    "memset "
    "^0037 ?SetWindowTextA@CWnd@@QEAAXPEBD@Z ^0037 "
    "?SetWindowTextW@CWnd@@QEAAXPEB_W@Z ........488BCBE8........4883C4205BC3\n"
    "0330010023000000........0228........0A062D170228........2C0C0228 00 0000 "
    "002F :0000 ??2@$$FYMPEAX_K@Z ........0A062CEE2B03166A2A062A\n"
    "0330010007000000000000000328........2A00 00 0000 0014 :0000 "
    "??$?RE@__crt_internal_free_policy@@$$FQEBMXQEBE@Z\n"
    "41574989CF41564989D641554989FD415455534889F34883EC084C8B26488B3F 07 5DF5 "
    "0061 :0000 argz_append :0000 __argz_append ^0027 realloc ^0045 memcpy "
    "........4889C5B80C0000004885ED741A488B3B4C89FA4C89F64801EFE8........"
    "49896D0031C04C89234883C4085B5D415C415D415E415FC3\n";

/// How many copies of \ref Lines the test source holds: enough that it spans
/// several parser chunks.
constexpr unsigned Copies = 2000;

class SignatureCacheTest : public ::testing::Test {
protected:
  void SetUp() override {
    llvm::SmallString<128> UniqueDirectory;
    const std::error_code Error = llvm::sys::fs::createUniqueDirectory(
        "neverd-signature-cache", UniqueDirectory);
    ASSERT_FALSE(Error) << Error.message();
    Directory = UniqueDirectory.c_str();
    Source = Directory / "library.pat";
    std::string Text;
    for (unsigned I = 0; I < Copies; ++I)
      Text += Lines;
    write(Source, Text);
  }

  void TearDown() override {
    std::error_code Error;
    std::filesystem::remove_all(Directory, Error);
  }

  static void write(const std::filesystem::path &Path,
                    llvm::StringRef Contents) {
    std::ofstream Output(Path, std::ios::binary);
    ASSERT_TRUE(Output.good());
    Output.write(Contents.data(),
                 static_cast<std::streamsize>(Contents.size()));
    ASSERT_TRUE(Output.good());
  }

  SignatureCache cache(uint64_t MinSourceBytes = 0) const {
    return SignatureCache((Directory / "cache").string(), MinSourceBytes);
  }

  /// The modules \ref Source parses to, and what they point into.
  struct Parsed {
    std::vector<PatternChunk> Chunks;
    std::unique_ptr<uint8_t[]> Bytes;
    std::vector<StoredModule> Modules;
  };
  Parsed parse() const {
    Parsed Result;
    std::ifstream Input(Source, std::ios::binary);
    Text.assign(std::istreambuf_iterator<char>(Input),
                std::istreambuf_iterator<char>());
    Result.Chunks = PatternParser::splitChunks(Text);
    Result.Bytes = PatternParser::reserveBytes(Result.Chunks);
    PatternParser::parseChunks(Result.Chunks);
    EXPECT_FALSE(PatternParser::firstError(Result.Chunks));
    for (const PatternChunk &Chunk : Result.Chunks)
      Result.Modules.insert(Result.Modules.end(), Chunk.Modules.begin(),
                            Chunk.Modules.end());
    return Result;
  }

  std::filesystem::path Directory;
  std::filesystem::path Source;
  mutable std::string Text;
};

void expectSameModules(llvm::ArrayRef<StoredModule> Expected,
                       llvm::ArrayRef<StoredModule> Actual) {
  ASSERT_EQ(Expected.size(), Actual.size());
  for (size_t I = 0; I < Expected.size(); ++I) {
    const StoredModule &E = Expected[I], &A = Actual[I];
    ASSERT_EQ(E.LeadingCount, A.LeadingCount) << "module " << I;
    ASSERT_EQ(E.TailCount, A.TailCount) << "module " << I;
    EXPECT_EQ(E.TotalLen, A.TotalLen) << "module " << I;
    EXPECT_EQ(E.CRC16, A.CRC16) << "module " << I;
    EXPECT_EQ(E.CRCLen, A.CRCLen) << "module " << I;
    EXPECT_EQ(E.RequireReferences, A.RequireReferences) << "module " << I;
    EXPECT_EQ(E.RequireCaller, A.RequireCaller) << "module " << I;
    const size_t Count = E.LeadingCount + E.TailCount;
    for (size_t B = 0; B < Count; ++B) {
      ASSERT_EQ(E.isStated(B), A.isStated(B))
          << "module " << I << " byte " << B;
      if (E.isStated(B))
        ASSERT_EQ(E.Bytes[B], A.Bytes[B]) << "module " << I << " byte " << B;
    }
    ASSERT_EQ(E.PublicNameCount, A.PublicNameCount) << "module " << I;
    ASSERT_EQ(E.ReferenceCount, A.ReferenceCount) << "module " << I;
    for (size_t N = 0; N < E.PublicNameCount + E.ReferenceCount; ++N) {
      EXPECT_EQ(E.Names[N].Offset, A.Names[N].Offset) << "module " << I;
      EXPECT_EQ(E.Names[N].Name, A.Names[N].Name) << "module " << I;
    }
  }
}

} // namespace

TEST_F(SignatureCacheTest, AnEntryMapsTheModulesItsSourceParsesTo) {
  const Parsed Expected = parse();
  ASSERT_EQ(Expected.Modules.size(), 4u * Copies);
  const SignatureCache Cache = cache();
  const std::optional<SignatureCache::SourceIdentity> Identity =
      SignatureCache::identify(Source);
  ASSERT_TRUE(Identity);
  EXPECT_FALSE(Cache.load(Source, *Identity));

  Cache.store(Source, *Identity, Expected.Modules);
  std::optional<SignatureCache::Entry> Entry = Cache.load(Source, *Identity);
  ASSERT_TRUE(Entry);
  expectSameModules(Expected.Modules, Entry->Modules);

  // Moving the entry, as a load does, keeps what its modules point into.
  const SignatureCache::Entry Moved = std::move(*Entry);
  expectSameModules(Expected.Modules, Moved.Modules);
}

TEST_F(SignatureCacheTest, AChangedSourceIsAMiss) {
  const SignatureCache Cache = cache();
  const std::optional<SignatureCache::SourceIdentity> Before =
      SignatureCache::identify(Source);
  ASSERT_TRUE(Before);
  Cache.store(Source, *Before, parse().Modules);
  ASSERT_TRUE(Cache.load(Source, *Before));

  std::string Changed(Lines);
  Changed += Lines;
  write(Source, Changed);
  const std::optional<SignatureCache::SourceIdentity> After =
      SignatureCache::identify(Source);
  ASSERT_TRUE(After);
  ASSERT_NE(*Before, *After);
  EXPECT_FALSE(Cache.load(Source, *After));

  // Nor is an entry kept for a source that changed after it was parsed.
  Cache.store(Source, *Before, parse().Modules);
  EXPECT_FALSE(Cache.load(Source, *After));
}

TEST_F(SignatureCacheTest, ADamagedEntryIsAMiss) {
  const SignatureCache Cache = cache();
  const std::optional<SignatureCache::SourceIdentity> Identity =
      SignatureCache::identify(Source);
  ASSERT_TRUE(Identity);
  Cache.store(Source, *Identity, parse().Modules);
  const std::optional<std::string> Entry = Cache.entryPath(Source);
  ASSERT_TRUE(Entry);
  std::string Written;
  {
    std::ifstream Input(*Entry, std::ios::binary);
    Written.assign(std::istreambuf_iterator<char>(Input),
                   std::istreambuf_iterator<char>());
  }
  ASSERT_FALSE(Written.empty());

  // A byte of the payload flipped, the entry cut short, and one that holds
  // no more than a header's worth of bytes.
  std::string Flipped = Written;
  Flipped[Flipped.size() / 2] ^= 1;
  for (const std::string &Damaged :
       {Flipped, Written.substr(0, Written.size() - 1),
        Written.substr(0, 64)}) {
    write(*Entry, Damaged);
    EXPECT_FALSE(Cache.load(Source, *Identity));
  }
  write(*Entry, Written);
  EXPECT_TRUE(Cache.load(Source, *Identity));
}

TEST_F(SignatureCacheTest, ASourceBelowTheMinimumIsNotKept) {
  const std::optional<SignatureCache::SourceIdentity> Identity =
      SignatureCache::identify(Source);
  ASSERT_TRUE(Identity);
  const SignatureCache Cache = cache(Identity->Size + 1);
  Cache.store(Source, *Identity, parse().Modules);
  EXPECT_FALSE(Cache.load(Source, *Identity));
}

TEST_F(SignatureCacheTest, ADatabaseLoadsTheSameThroughTheCache) {
  // An image holding the third line's routine, its unstated bytes zero.
  neverd::BinaryImage Image;
  neverd::Segment Code;
  Code.VA = 0x1000;
  Code.Flags =
      neverd::SegmentFlags::Readable | neverd::SegmentFlags::Executable;
  Code.Data = {0x03, 0x30, 0x01, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x03, 0x28, 0x00, 0x00, 0x00, 0x00, 0x2A, 0x00};
  Code.Size = Code.FileSz = Code.Data.size();
  Image.Segments.push_back(std::move(Code));

  auto matchesOf = [&](std::optional<SignatureCache> Cache) {
    SignatureDB Database;
    Database.setCache(std::move(Cache));
    EXPECT_FALSE(Database.loadFiles({Source}));
    EXPECT_EQ(Database.moduleCount(), 4u * Copies);
    Database.apply(Image, {0x1000});
    std::vector<std::pair<uint64_t, std::string>> Found;
    for (const SigMatch &Match : Database.matches())
      Found.emplace_back(Match.Address, Match.Name);
    return Found;
  };
  const auto Parsed = matchesOf(std::nullopt);
  ASSERT_FALSE(Parsed.empty());
  EXPECT_EQ(Parsed.front().second,
            "??$?RE@__crt_internal_free_policy@@$$FQEBMXQEBE@Z");

  // The first load through the cache parses the source and keeps it; the
  // second maps what the first kept.
  EXPECT_EQ(matchesOf(cache()), Parsed);
  const std::optional<SignatureCache::SourceIdentity> Identity =
      SignatureCache::identify(Source);
  ASSERT_TRUE(Identity);
  ASSERT_TRUE(cache().load(Source, *Identity));
  EXPECT_EQ(matchesOf(cache()), Parsed);

  // A single file loads through the cache too.
  SignatureDB Single;
  Single.setCache(cache());
  ASSERT_FALSE(Single.loadFile(Source));
  EXPECT_EQ(Single.moduleCount(), 4u * Copies);
}

TEST_F(SignatureCacheTest, ADamagedEntryAmongOthersIsParsedAlone) {
  // Three sources, each kept; one entry is then damaged.  A load maps the
  // other two and parses the third, and returns what parsing all three does.
  std::vector<std::filesystem::path> Sources = {Source};
  for (const char *Name : {"second.pat", "third.pat"}) {
    Sources.push_back(Directory / Name);
    std::ifstream Input(Source, std::ios::binary);
    std::string Copy((std::istreambuf_iterator<char>(Input)),
                     std::istreambuf_iterator<char>());
    write(Sources.back(), Copy);
  }
  auto filesLoaded = [&](std::optional<SignatureCache> Cache) {
    SignatureDB Database;
    Database.setCache(std::move(Cache));
    EXPECT_FALSE(Database.loadFiles(Sources));
    EXPECT_EQ(Database.moduleCount(), 3u * 4u * Copies);
    return Database.fileCount();
  };
  ASSERT_EQ(filesLoaded(cache()), 3u);
  for (const std::filesystem::path &Kept : Sources) {
    const std::optional<SignatureCache::SourceIdentity> Identity =
        SignatureCache::identify(Kept);
    ASSERT_TRUE(Identity);
    ASSERT_TRUE(cache().load(Kept, *Identity));
  }
  const std::optional<std::string> Damaged = cache().entryPath(Sources[1]);
  ASSERT_TRUE(Damaged);
  std::string Written;
  {
    std::ifstream Input(*Damaged, std::ios::binary);
    Written.assign(std::istreambuf_iterator<char>(Input),
                   std::istreambuf_iterator<char>());
  }
  Written[Written.size() - 1] ^= 1;
  write(*Damaged, Written);
  EXPECT_EQ(filesLoaded(cache()), 3u);

  // The damaged entry was replaced once its source was parsed again.
  const std::optional<SignatureCache::SourceIdentity> Identity =
      SignatureCache::identify(Sources[1]);
  ASSERT_TRUE(Identity);
  EXPECT_TRUE(cache().load(Sources[1], *Identity));
}

TEST(SignatureCacheEnvironment, TheEnvironmentMovesOrTurnsOffTheCache) {
  const std::string Variable(EnvironmentVariable);
  const char *Original = std::getenv(Variable.c_str());
  const std::optional<std::string> Saved =
      Original ? std::optional<std::string>(Original) : std::nullopt;
  auto set = [&](const char *Value) {
#ifdef _WIN32
    return _putenv_s(Variable.c_str(), Value ? Value : "");
#else
    return Value ? setenv(Variable.c_str(), Value, 1)
                 : unsetenv(Variable.c_str());
#endif
  };

  ASSERT_EQ(set(std::string(DisabledValue).c_str()), 0);
  EXPECT_FALSE(SignatureCache::fromEnvironment());
  ASSERT_EQ(set("elsewhere"), 0);
  const std::optional<SignatureCache> Moved = SignatureCache::fromEnvironment();
  ASSERT_TRUE(Moved);
  EXPECT_EQ(Moved->directory(), "elsewhere");
  ASSERT_EQ(set(nullptr), 0);
  if (const std::optional<SignatureCache> Default =
          SignatureCache::fromEnvironment())
    EXPECT_NE(Default->directory(), "elsewhere");

  ASSERT_EQ(set(Saved ? Saved->c_str() : nullptr), 0);
}

TEST_F(SignatureCacheTest, CallerContextSurvivesCachedLoading) {
  write(Source, "E8........C3 00 0000 0006 !caller :0000 forward ^0001 zero\n");
  const auto Parsed = parse();
  ASSERT_EQ(Parsed.Modules.size(), 1u);
  ASSERT_TRUE(Parsed.Modules.front().RequireCaller);
  const auto Identity = SignatureCache::identify(Source);
  ASSERT_TRUE(Identity);
  const auto Cache = cache();
  Cache.store(Source, *Identity, Parsed.Modules);
  auto Loaded = Cache.load(Source, *Identity);
  ASSERT_TRUE(Loaded);
  expectSameModules(Parsed.Modules, Loaded->Modules);
}

TEST_F(SignatureCacheTest, ContextValidationIsIndependentOfPieceOrder) {
  write(Source, "E8........C3 00 0000 0006 !caller :0000 forward ^0001 zero\n");
  auto Parsed = parse();
  const auto Identity = SignatureCache::identify(Source);
  ASSERT_TRUE(Identity);
  const auto Cache = cache();
  for (uint32_t Offset : {0u, 1u}) {
    // The cache is checksummed, but the module's context contract must still
    // be validated independently of the order its pieces are materialized.
    std::vector<StoredName> Names(Parsed.Modules.front().Names,
                                  Parsed.Modules.front().Names + 2);
    Names.front().Offset = Offset;
    Parsed.Modules.front().Names = Names.data();
    Cache.store(Source, *Identity, Parsed.Modules);
    for (bool Reverse : {false, true}) {
      auto Pending = Cache.open(Source, *Identity);
      ASSERT_TRUE(Pending);
      bool Accepted = true;
      for (size_t P = 0; P < Pending->pieces(); ++P)
        Accepted &= Pending->run(Reverse ? Pending->pieces() - P - 1 : P);
      EXPECT_EQ(Accepted, Offset == 0) << "reverse=" << Reverse;
    }
    Parsed.Modules.front().Names = Parsed.Chunks.front().Modules.front().Names;
  }
}

TEST_F(SignatureCacheTest, ContextBodiesRetainParserCoverageRequirements) {
  write(Source, "E8........C3 00 0000 0006 !caller :0000 forward ^0001 zero\n");
  auto Parsed = parse();
  const auto Identity = SignatureCache::identify(Source);
  ASSERT_TRUE(Identity);
  const auto Cache = cache();
  const StoredModule Original = Parsed.Modules.front();
  for (bool Caller : {false, true})
    for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
      StoredModule &M = Parsed.Modules.front();
      M = Original;
      M.RequireCaller = Caller;
      M.RequireReferences = !Caller;
      if (Mutation == 0)
        M.TotalLen = 0;
      else if (Mutation == 1)
        M.CRCLen = 1; // A CRC after a body already covered by leading bytes.
      else {
        --M.LeadingCount;
        M.CRCLen = 2; // A CRC crossing the declared function end.
      }
      Cache.store(Source, *Identity, Parsed.Modules);
      EXPECT_FALSE(Cache.load(Source, *Identity))
          << "caller=" << Caller << " mutation=" << Mutation;
    }
}
