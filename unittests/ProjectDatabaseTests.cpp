//===- ProjectDatabaseTests.cpp - Saved project reader regressions --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent SQLite/zlib fixtures exercise integrity, isolation and budgets.
//===----------------------------------------------------------------------===//

#include "neverd/support/FilePath.h"
#include "neverd/support/ProjectDatabase.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <filesystem>
#include <gtest/gtest.h>
#include <span>
#include <sqlite3.h>
#include <string>
#include <vector>
#include <zlib.h>

using namespace neverd;
namespace {
namespace database = project_database;

struct HashSink : database::InputSink {
  llvm::SHA256 Hash;
  uint64_t Count = 0;
  size_t Largest = 0;
  void write(std::span<const uint8_t> Bytes) override {
    Hash.update(llvm::ArrayRef(Bytes.data(), Bytes.size()));
    Count += Bytes.size();
    Largest = std::max(Largest, Bytes.size());
  }
  std::string sha256() override { return llvm::toHex(Hash.final(), true); }
};

class ProjectDatabaseTest : public testing::Test {
protected:
  std::filesystem::path Directory, Path;
  std::string Bytes;
  void SetUp() override {
    llvm::SmallString<128> Folder;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-nddb-test", Folder));
    Directory = std::filesystem::u8path(Folder.str().str());
    Path = Directory / "project.nddb";
    Bytes.assign(600000, 'A');
    for (size_t I = 0; I < Bytes.size(); ++I)
      Bytes[I] = static_cast<char>(I * 17);
    sqlite3 *DB = nullptr;
    ASSERT_EQ(sqlite3_open(pathToUTF8(Path).c_str(), &DB), SQLITE_OK);
    for (const auto *Statement : database::Schema)
      ASSERT_EQ(sqlite3_exec(DB, Statement, nullptr, nullptr, nullptr),
                SQLITE_OK);
    const std::string SQL =
        "PRAGMA application_id = " + std::to_string(database::ApplicationId) +
        "; INSERT INTO meta VALUES('format', '1'), ('input_name','测试.bin'), "
        "('input_sha256','" +
        llvm::toHex(
            llvm::SHA256::hash(llvm::ArrayRef(
                reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size())),
            true) +
        "'), ('input_size','" + std::to_string(Bytes.size()) +
        "');"
        "INSERT INTO sidecars VALUES('.neverd-renames.json',CAST('[]' AS "
        "BLOB));"
        "INSERT INTO state VALUES('location',CAST('{}' AS BLOB));";
    ASSERT_EQ(sqlite3_exec(DB, SQL.c_str(), nullptr, nullptr, nullptr),
              SQLITE_OK);
    constexpr size_t Piece = 100000;
    for (size_t I = 0; I < Bytes.size(); I += Piece) {
      std::vector<uint8_t> Packed(4 + compressBound(Piece));
      for (size_t J = 0; J < 4; ++J)
        Packed[J] = static_cast<uint8_t>(Piece >> (8 * (3 - J)));
      uLongf Length = Packed.size() - 4;
      ASSERT_EQ(compress2(Packed.data() + 4, &Length,
                          reinterpret_cast<const uint8_t *>(Bytes.data() + I),
                          Piece, 6),
                Z_OK);
      sqlite3_stmt *S = nullptr;
      ASSERT_EQ(sqlite3_prepare_v2(DB,
                                   "INSERT INTO blobs VALUES('input',?,?,?)",
                                   -1, &S, nullptr),
                SQLITE_OK);
      sqlite3_bind_int64(S, 1, I / Piece);
      sqlite3_bind_int64(S, 2, Piece);
      sqlite3_bind_blob(S, 3, Packed.data(), Length + 4, SQLITE_TRANSIENT);
      ASSERT_EQ(sqlite3_step(S), SQLITE_DONE);
      sqlite3_finalize(S);
    }
    ASSERT_EQ(sqlite3_close(DB), SQLITE_OK);
  }
  void TearDown() override {
    std::error_code EC;
    std::filesystem::remove_all(Directory, EC);
  }
  void sql(const std::string &Text) {
    sqlite3 *DB = nullptr;
    ASSERT_EQ(sqlite3_open(pathToUTF8(Path).c_str(), &DB), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(DB, Text.c_str(), nullptr, nullptr, nullptr),
              SQLITE_OK)
        << sqlite3_errmsg(DB);
    ASSERT_EQ(sqlite3_close(DB), SQLITE_OK);
  }
};

TEST_F(ProjectDatabaseTest, StreamsAndAuthenticatesInputAndProject) {
  ASSERT_TRUE(database::isDatabase(Path));
  database::Reader Reader(Path);
  Reader.readProject();
  EXPECT_EQ(Reader.contents().InputName, "测试.bin");
  EXPECT_EQ(Reader.contents().Sidecars.at(".neverd-renames.json"), "[]");
  EXPECT_EQ(Reader.contents().State.at("location"), "{}");
  HashSink Sink;
  Reader.unpackInput(Sink);
  EXPECT_EQ(Sink.Count, Bytes.size());
  EXPECT_LE(Sink.Largest, limits::support::kProjectStreamBytes);
}

TEST_F(ProjectDatabaseTest, KeepsOneSnapshotAcrossConcurrentSave) {
  sql("PRAGMA journal_mode = WAL");
  database::Reader Reader(Path);
  sql("UPDATE sidecars SET data=CAST('[1]' AS BLOB); "
      "UPDATE meta SET value='different.bin' WHERE key='input_name'");
  Reader.readProject();
  EXPECT_EQ(Reader.contents().InputName, "测试.bin");
  EXPECT_EQ(Reader.contents().Sidecars.at(".neverd-renames.json"), "[]");
  database::Reader Later(Path);
  Later.readProject();
  EXPECT_EQ(Later.contents().InputName, "different.bin");
  EXPECT_EQ(Later.contents().Sidecars.at(".neverd-renames.json"), "[1]");
}

TEST_F(ProjectDatabaseTest, RefusesChunkGapsBeforePublishing) {
  sql("DELETE FROM blobs WHERE chunk=2");
  database::Reader Reader(Path);
  HashSink Sink;
  EXPECT_THROW(Reader.unpackInput(Sink), database::Error);
  EXPECT_LT(Sink.Count, Bytes.size());
}

TEST_F(ProjectDatabaseTest, RefusesTrailingCompressedBytes) {
  sql("UPDATE blobs SET data=CAST(data || X'00' AS BLOB) WHERE chunk=3");
  database::Reader Reader(Path);
  HashSink Sink;
  EXPECT_THROW(Reader.unpackInput(Sink), database::Error);
}

TEST_F(ProjectDatabaseTest, RefusesDigestMismatch) {
  sql("UPDATE meta SET "
      "value='0000000000000000000000000000000000000000000000000000000000000000'"
      " WHERE key='input_sha256'");
  database::Reader Reader(Path);
  HashSink Sink;
  EXPECT_THROW(Reader.unpackInput(Sink), database::Error);
  EXPECT_EQ(Sink.Count, Bytes.size());
}

TEST_F(ProjectDatabaseTest, HonorsMetadataAndSidecarBudgets) {
  database::ReadLimits Limits;
  Limits.MetadataBytes = 20;
  EXPECT_THROW(database::Reader(Path, {}, Limits), database::Error);
  Limits = {};
  Limits.SidecarBytes = 1;
  database::Reader Reader(Path, {}, Limits);
  EXPECT_THROW(Reader.readProject(), database::Error);
  Limits = {};
  Limits.Rows = 2;
  EXPECT_THROW(database::Reader(Path, {}, Limits), database::Error);
}

TEST_F(ProjectDatabaseTest, RefusesUnsafeNamesAndForeignFiles) {
  for (const std::string Name :
       {"../outside", "C:outside", "NUL", "con.txt", "a.", "a ", ".", ".."}) {
    SCOPED_TRACE(Name);
    sql("UPDATE meta SET value='" + Name + "' WHERE key='input_name'");
    EXPECT_THROW(database::Reader Reader(Path), database::Error);
  }
  sql("PRAGMA application_id = 1196444487");
  EXPECT_THROW(database::Reader Reader(Path), database::Error);
}

TEST_F(ProjectDatabaseTest, CancelsDuringStreamingWithoutBorrowingCallbacks) {
  struct State {
    bool Stop = false;
    static void progress(void *User, const char *Phase, unsigned long long Done,
                         unsigned long long, const char *) {
      auto &S = *static_cast<State *>(User);
      if (std::string_view(Phase) == "project-unpack" && Done > 0)
        S.Stop = true;
    }
    static int stop(void *User) { return static_cast<State *>(User)->Stop; }
  } State;
  LoadProgress Progress{State::progress, &State, State::stop, &State};
  {
    database::Reader Reader(Path, Progress);
    HashSink Sink;
    EXPECT_THROW(Reader.unpackInput(Sink), LoadCancelled);
    EXPECT_LT(Sink.Count, Bytes.size());
  }
  database::Reader Retry(Path);
  HashSink Sink;
  EXPECT_NO_THROW(Retry.unpackInput(Sink));
  EXPECT_EQ(Sink.Count, Bytes.size());
}

TEST_F(ProjectDatabaseTest, RejectsUnknownSidecars) {
  sql("INSERT INTO sidecars VALUES('../outside', X'5b5d')");
  database::Reader Reader(Path);
  EXPECT_THROW(Reader.readProject(), database::Error);
}

TEST_F(ProjectDatabaseTest, AcceptsQtEmptyInputFraming) {
  sql("DELETE FROM blobs; INSERT INTO blobs VALUES('input',0,0,X'00000000'); "
      "UPDATE meta SET value='0' WHERE key='input_size'; "
      "UPDATE meta SET "
      "value='e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'"
      " "
      "WHERE key='input_sha256'");
  database::Reader Reader(Path);
  HashSink Sink;
  EXPECT_NO_THROW(Reader.unpackInput(Sink));
  EXPECT_EQ(Sink.Count, 0u);
}

TEST_F(ProjectDatabaseTest, RejectsViewsAndDuplicateMetadata) {
  sql("ALTER TABLE meta RENAME TO old_meta; CREATE TABLE meta(key TEXT,value "
      "TEXT); "
      "INSERT INTO meta SELECT * FROM old_meta; INSERT INTO meta "
      "VALUES('format','1')");
  EXPECT_THROW(database::Reader Reader(Path), database::Error);
  sql("DELETE FROM meta WHERE rowid=(SELECT max(rowid) FROM meta); "
      "DROP TABLE sidecars; CREATE VIEW sidecars AS SELECT 'x' AS suffix, "
      "X'00' AS data");
  EXPECT_THROW(database::Reader Reader(Path), database::Error);
}

TEST_F(ProjectDatabaseTest, RejectsMissingOrOversizedChunkFraming) {
  sql("UPDATE blobs SET data = X'ffffffff789c030000000001' WHERE chunk=0");
  {
    database::Reader Reader(Path);
    HashSink Sink;
    EXPECT_THROW(Reader.unpackInput(Sink), database::Error);
    EXPECT_EQ(Sink.Count, 0u);
  }
  sql("DELETE FROM blobs");
  database::Reader Reader(Path);
  HashSink Sink;
  EXPECT_THROW(Reader.unpackInput(Sink), database::Error);
}

} // namespace
