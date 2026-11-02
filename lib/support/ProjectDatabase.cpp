//===- ProjectDatabase.cpp - Shared NDDB reader ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded streaming and transaction-consistent validation of saved projects.
//===----------------------------------------------------------------------===//

#include "neverd/support/ProjectDatabase.h"

#include "neverd/support/FilePath.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sqlite3.h>
#include <utility>
#include <zlib.h>

namespace neverd::project_database {

std::string_view fileKey(const project_files::Description &File) {
  return File.Legacy.empty() ? File.Path : File.Legacy;
}

bool isPackedFile(const project_files::Description &File) {
  return File.Id != project_files::File::Journal &&
         File.Id != project_files::File::Workbench &&
         File.Id != project_files::File::WriterLock;
}

std::vector<std::string> legacyFileKeys() {
  std::vector<std::string> Keys;
  for (const auto Id :
       {project_files::File::Annotations, project_files::File::Renames,
        project_files::File::Functions, project_files::File::Items,
        project_files::File::Operands, project_files::File::Code,
        project_files::File::History, project_files::File::Load})
    Keys.emplace_back(fileKey(project_files::describe(Id)));
  return Keys;
}

bool validInputName(std::string_view Name) {
  if (Name.empty() || Name == "." || Name == ".." ||
      Name.find_first_of("/\\:") != std::string_view::npos)
    return false;
  // A name is always a single portable leaf, never a device or NTFS stream.
  if (Name.back() == '.' || Name.back() == ' ' ||
      std::any_of(Name.begin(), Name.end(), [](unsigned char C) {
        return C < ' ' || C == 127 || std::strchr("<>\"|?*", C);
      }))
    return false;
  std::string Stem(Name.substr(0, Name.find('.')));
  for (char &C : Stem)
    if (C >= 'a' && C <= 'z')
      C -= 'a' - 'A';
  if (Stem == "CON" || Stem == "PRN" || Stem == "AUX" || Stem == "NUL" ||
      Stem == "CONIN$" || Stem == "CONOUT$" ||
      ((Stem.starts_with("COM") || Stem.starts_with("LPT")) &&
       ((Stem.size() == 4 && Stem.back() >= '1' && Stem.back() <= '9') ||
        Stem.substr(3) == "¹" || Stem.substr(3) == "²" ||
        Stem.substr(3) == "³")))
    return false;
  return true;
}

bool isDatabase(const std::filesystem::path &Path) {
  std::string Suffix = pathToUTF8(Path.extension());
  for (char &C : Suffix)
    if (C >= 'A' && C <= 'Z')
      C += 'a' - 'A';
  if (Suffix == Extension)
    return true;
  std::ifstream In(Path, std::ios::binary);
  std::array<unsigned char, HeaderPrefixBytes> Header{};
  if (!In.read(reinterpret_cast<char *>(Header.data()), Header.size()) ||
      std::memcmp(Header.data(), SqliteMagic, sizeof(SqliteMagic)))
    return false;
  uint32_t Id = 0;
  for (size_t I = ApplicationIdOffset; I < Header.size(); ++I)
    Id = (Id << 8) | Header[I];
  return Id == ApplicationId;
}

namespace {
[[noreturn]] void fail(std::string Message) {
  throw Error("NDDB: " + std::move(Message));
}

void require(bool Condition, const char *Message) {
  if (!Condition)
    fail(Message);
}

bool isPackedKey(std::string_view Key) {
  return std::any_of(std::begin(project_files::Files),
                     std::end(project_files::Files), [&](const auto &File) {
                       return isPackedFile(File) && fileKey(File) == Key;
                     });
}

bool isDigest(std::string_view Text) {
  return Text.size() == 64 && std::all_of(Text.begin(), Text.end(), [](char C) {
           return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f');
         });
}

bool validUTF8(std::string_view Text) {
  for (size_t I = 0; I < Text.size();) {
    const auto Lead = static_cast<uint8_t>(Text[I++]);
    if (Lead < 0x80)
      continue;
    const unsigned Tail = Lead >= 0xc2 && Lead <= 0xdf   ? 1
                          : Lead >= 0xe0 && Lead <= 0xef ? 2
                          : Lead >= 0xf0 && Lead <= 0xf4 ? 3
                                                         : 0;
    if (!Tail || Text.size() - I < Tail)
      return false;
    uint32_t Point = Lead & ((1u << (6 - Tail)) - 1);
    for (unsigned J = 0; J < Tail; ++J) {
      const auto C = static_cast<uint8_t>(Text[I++]);
      if ((C & 0xc0) != 0x80)
        return false;
      Point = (Point << 6) | (C & 0x3f);
    }
    if ((Tail == 2 && Point < 0x800) || (Tail == 3 && Point < 0x10000) ||
        (Point >= 0xd800 && Point <= 0xdfff) || Point > 0x10ffff)
      return false;
  }
  return true;
}

void debit(uint64_t &Remaining, uint64_t Size, const char *Description) {
  if (Size > Remaining)
    fail(std::string(Description) + " budget exceeded");
  Remaining -= Size;
}

using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
using Blob = std::unique_ptr<sqlite3_blob, decltype(&sqlite3_blob_close)>;
} // namespace

struct Reader::Impl {
  Database DB{nullptr, sqlite3_close};
  LoadProgress Progress;
  ReadLimits Limits;
  Contents Data;
  bool Stopped = false;
  int SQLiteLengthLimit = 0;

  Impl(const std::filesystem::path &Path, const LoadProgress &Progress,
       ReadLimits Limits)
      : Progress(Progress), Limits(Limits) {
    Progress.report("project-open", 0, 1, "reading saved project");
    sqlite3 *Raw = nullptr;
    const int Open = sqlite3_open_v2(
        pathToUTF8(Path).c_str(), &Raw,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE,
        nullptr);
    DB.reset(Raw);
    check(Open);
    SQLiteLengthLimit = sqlite3_limit(DB.get(), SQLITE_LIMIT_LENGTH, -1);
    valueLimit(Limits.MetadataBytes);
    sqlite3_progress_handler(
        DB.get(), limits::support::kProjectSqlCheckpointOps,
        [](void *User) noexcept {
          auto &Self = *static_cast<Impl *>(User);
          if (Self.Progress.Stop && Self.Progress.Stop(Self.Progress.StopUser))
            Self.Stopped = true;
          return Self.Stopped ? 1 : 0;
        },
        this);
    check(sqlite3_db_config(DB.get(), SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr));
    check(sqlite3_db_config(DB.get(), SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0,
                            nullptr));
    check(sqlite3_db_config(DB.get(), SQLITE_DBCONFIG_ENABLE_VIEW, 0, nullptr));
    exec("PRAGMA query_only = ON");
    exec("BEGIN");
    auto Id = prepare("PRAGMA application_id");
    require(next(Id), "missing application id");
    const auto App = integer(Id, 0);
    require(App == 0 || App == ApplicationId, "not a NeverD database");
    // Require ordinary tables, not views or virtual tables whose schema can
    // supply executable expressions. Actual fields are checked on every row.
    for (const char *Table : {"meta", "blobs", "sidecars", "state"}) {
      auto S =
          prepare("SELECT type, rootpage FROM sqlite_schema WHERE name = ?");
      check(sqlite3_bind_text(S.get(), 1, Table, -1, SQLITE_STATIC));
      require(next(S) && text(S, 0) == "table" && integer(S, 1) > 0 && !next(S),
              "missing or unsupported project table");
    }
    uint64_t Bytes = Limits.MetadataBytes, Rows = Limits.Rows;
    std::map<std::string, std::string> Meta;
    auto M = prepare("SELECT key, value FROM meta");
    while (next(M)) {
      debit(Rows, 1, "metadata row");
      auto Key = text(M, 0, &Bytes);
      auto Value = text(M, 1, &Bytes);
      require(Meta.emplace(std::move(Key), std::move(Value)).second,
              "duplicate metadata key");
    }
    const auto Required = [&](const char *Key) -> const std::string & {
      const auto I = Meta.find(Key);
      if (I == Meta.end())
        fail(std::string("missing metadata: ") + Key);
      return I->second;
    };
    const auto &Format = Required("format");
    if (Format != std::to_string(FormatVersion))
      fail("unsupported database format: " + Format);
    Data.InputName = Required("input_name");
    require(validInputName(Data.InputName), "invalid input filename");
    Data.InputSHA256 = Required("input_sha256");
    require(isDigest(Data.InputSHA256), "invalid input SHA-256");
    const auto &Size = Required("input_size");
    const auto Parsed =
        std::from_chars(Size.data(), Size.data() + Size.size(), Data.InputSize);
    require(!Size.empty() && Parsed.ec == std::errc{} &&
                Parsed.ptr == Size.data() + Size.size() &&
                Data.InputSize <= uint64_t(std::numeric_limits<int64_t>::max()),
            "invalid input size");
    Data.Generator = Meta["generator"];
    Data.SavedAt = Meta["saved_at"];
    const auto &Packed = Meta["sidecar_suffixes"];
    if (Packed.empty()) {
      Data.PackedFiles = legacyFileKeys();
    } else {
      std::string_view Rest(Packed);
      std::set<std::string> Seen;
      while (!Rest.empty()) {
        const auto Space = Rest.find(' ');
        const auto Key = Rest.substr(0, Space);
        if (!Key.empty()) {
          require(isPackedKey(Key), "unsupported packed project file");
          require(Seen.emplace(Key).second, "duplicate packed project file");
          Data.PackedFiles.emplace_back(Key);
        }
        Rest = Space == std::string_view::npos ? std::string_view{}
                                               : Rest.substr(Space + 1);
      }
    }
    Progress.report("project-open", 1, 1);
  }

  void check(int Result) {
    if (Stopped)
      throw LoadCancelled{};
    Progress.check();
    if (Result != SQLITE_OK)
      fail(DB ? sqlite3_errmsg(DB.get()) : sqlite3_errstr(Result));
  }

  void valueLimit(uint64_t Budget) {
    const auto Limit = std::min<uint64_t>(Budget, SQLiteLengthLimit);
    sqlite3_limit(DB.get(), SQLITE_LIMIT_LENGTH, static_cast<int>(Limit));
  }

  Statement prepare(const char *SQL) {
    sqlite3_stmt *Raw = nullptr;
    const int Result = sqlite3_prepare_v2(DB.get(), SQL, -1, &Raw, nullptr);
    Statement S(Raw, sqlite3_finalize);
    check(Result);
    return S;
  }

  bool next(Statement &S) {
    const int Result = sqlite3_step(S.get());
    check(Result == SQLITE_ROW || Result == SQLITE_DONE ? SQLITE_OK : Result);
    return Result == SQLITE_ROW;
  }

  void exec(const char *SQL) {
    auto S = prepare(SQL);
    require(!next(S), "unexpected query result");
  }

  std::string text(const Statement &S, int Column, uint64_t *Budget = nullptr) {
    require(sqlite3_column_type(S.get(), Column) == SQLITE_TEXT,
            "expected a text field");
    const int Size = sqlite3_column_bytes(S.get(), Column);
    if (Budget)
      debit(*Budget, Size, "metadata byte");
    const auto *Bytes = sqlite3_column_text(S.get(), Column);
    require(Bytes != nullptr, "cannot read text field");
    std::string Value(reinterpret_cast<const char *>(Bytes), Size);
    require(Value.find('\0') == std::string::npos, "text contains a NUL byte");
    require(validUTF8(Value), "text is not UTF-8");
    return Value;
  }

  int64_t integer(const Statement &S, int Column) {
    require(sqlite3_column_type(S.get(), Column) == SQLITE_INTEGER,
            "expected an integer field");
    return sqlite3_column_int64(S.get(), Column);
  }

  void readTable(const char *SQL, std::map<std::string, std::string> &Output,
                 uint64_t Budget, bool Sidecars) {
    uint64_t Rows = Limits.Rows, Keys = Limits.MetadataBytes;
    valueLimit(Budget);
    std::map<std::string, std::string> Values;
    auto S = prepare(SQL);
    while (next(S)) {
      debit(Rows, 1, "project row");
      auto Key = text(S, 0, &Keys);
      if (Sidecars) {
        require(isPackedKey(Key), "unsupported project sidecar");
        // Historical files had no packed list. Recognized newer file kinds
        // may still be present, as the Qt reader has always permitted.
      }
      require(sqlite3_column_type(S.get(), 1) == SQLITE_BLOB,
              "expected project bytes");
      const int Size = sqlite3_column_bytes(S.get(), 1);
      debit(Budget, Size,
            Sidecars ? "project sidecar byte" : "project state byte");
      const auto *Bytes =
          static_cast<const char *>(sqlite3_column_blob(S.get(), 1));
      require(Size == 0 || Bytes, "cannot read project bytes");
      require(
          Values.emplace(std::move(Key), std::string(Bytes ? Bytes : "", Size))
              .second,
          "duplicate project key");
    }
    Output = std::move(Values);
  }

  void unpack(InputSink &Sink) {
    valueLimit(SQLiteLengthLimit);
    Progress.report("project-unpack", 0, Data.InputSize,
                    "unpacking saved input");
    auto Chunks = prepare("SELECT rowid, chunk, size, typeof(data) FROM blobs "
                          "WHERE name = 'input' ORDER BY chunk");
    uint64_t Index = 0, Total = 0;
    std::array<uint8_t, limits::support::kProjectStreamBytes> Input{}, Output{};
    while (next(Chunks)) {
      require(integer(Chunks, 1) >= 0 && uint64_t(integer(Chunks, 1)) == Index,
              "input chunk sequence is damaged");
      const auto Size = integer(Chunks, 2);
      require(Size >= 0 &&
                  uint64_t(Size) <= std::numeric_limits<uint32_t>::max() &&
                  uint64_t(Size) <= Data.InputSize - Total,
              "invalid input chunk size");
      require(Size != 0 || (Index == 0 && Data.InputSize == 0),
              "empty input chunk");
      require(text(Chunks, 3) == "blob", "expected compressed input bytes");
      sqlite3_blob *Raw = nullptr;
      const int Open = sqlite3_blob_open(DB.get(), "main", "blobs", "data",
                                         integer(Chunks, 0), 0, &Raw);
      Blob B(Raw, sqlite3_blob_close);
      check(Open);
      const int Length = sqlite3_blob_bytes(B.get());
      constexpr int PrefixBytes = sizeof(uint32_t);
      require(Length >= PrefixBytes, "truncated compressed input");
      std::array<uint8_t, PrefixBytes> Prefix{};
      check(sqlite3_blob_read(B.get(), Prefix.data(), Prefix.size(), 0));
      uint32_t Declared = 0;
      for (uint8_t Byte : Prefix)
        Declared = (Declared << 8) | Byte;
      require(Declared == Size,
              "compressed input length disagrees with metadata");
      // Qt stores an empty QByteArray as just its four-byte zero prefix.
      if (Size == 0 && Length == PrefixBytes) {
        ++Index;
        continue;
      }
      z_stream Stream{};
      require(inflateInit(&Stream) == Z_OK, "cannot initialize decompression");
      const auto End = [](z_stream *S) { inflateEnd(S); };
      std::unique_ptr<z_stream, decltype(End)> Guard(&Stream, End);
      int Offset = PrefixBytes;
      uint64_t Expanded = 0;
      int Result = Z_OK;
      do {
        Progress.check();
        if (!Stream.avail_in && Offset < Length) {
          const int Read = std::min<int>(Input.size(), Length - Offset);
          check(sqlite3_blob_read(B.get(), Input.data(), Read, Offset));
          Offset += Read;
          Stream.next_in = Input.data();
          Stream.avail_in = Read;
        }
        Stream.next_out = Output.data();
        Stream.avail_out = Output.size();
        Result = inflate(&Stream, Z_NO_FLUSH);
        require(Result == Z_OK || Result == Z_STREAM_END,
                "damaged compressed input");
        const auto Produced = Output.size() - Stream.avail_out;
        require(Produced <= uint64_t(Size) - Expanded,
                "input chunk expands beyond its size");
        Sink.write(std::span(Output).first(Produced));
        Expanded += Produced;
        Progress.report("project-unpack", Total + Expanded, Data.InputSize);
      } while (Result != Z_STREAM_END);
      require(Expanded == uint64_t(Size) && Stream.avail_in == 0 &&
                  Offset == Length,
              "compressed input has a wrong size or trailing bytes");
      Total += Expanded;
      ++Index;
    }
    require(Index != 0 && Total == Data.InputSize,
            "stored input size does not match metadata");
    require(Sink.sha256() == Data.InputSHA256,
            "stored input SHA-256 does not match metadata");
    Progress.report("project-unpack", Total, Data.InputSize,
                    "saved input verified");
  }
};

Reader::Reader(const std::filesystem::path &Path, const LoadProgress &Progress,
               ReadLimits Limits)
    : P(std::make_unique<Impl>(Path, Progress, Limits)) {}
Reader::~Reader() = default;
const Contents &Reader::contents() const { return P->Data; }
void Reader::readProject(bool IncludeState) {
  P->Progress.report("project-files", 0, 1, "reading saved project files");
  P->readTable("SELECT suffix, data FROM sidecars", P->Data.Sidecars,
               P->Limits.SidecarBytes, true);
  if (IncludeState)
    P->readTable("SELECT key, value FROM state", P->Data.State,
                 P->Limits.StateBytes, false);
  P->Progress.report("project-files", 1, 1);
}
void Reader::unpackInput(InputSink &Sink) { P->unpack(Sink); }

} // namespace neverd::project_database
