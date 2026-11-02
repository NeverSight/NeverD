//===- SignatureCache.cpp - Parsed signature files, kept on disk ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/SignatureCache.h"

#include "CodeLayoutContext.h"
#include "LocalDataReference.h"
#include "RuntimePointerContext.h"
#include "VTableContext.h"

#include "neverd/Common.h"
#include "neverd/sigs/SignatureMatcher.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

using namespace neverd;
using namespace neverd::sigs;

namespace {

#define NEVERD_SIGNATURE_CACHE_STRING(Name, Value)                             \
  constexpr llvm::StringLiteral Name(Value);
#define NEVERD_SIGNATURE_CACHE_VALUE(Name, Value)                              \
  constexpr uint64_t Name = Value;
#include "neverd/sigs/SignatureCache.def"

/// The start of an entry.  After it come the hashes of the payload's blocks,
/// then the payload: the canonical path of the source and the NeverD version
/// that wrote the entry, then, each at a multiple of EntryAlignment, the
/// module records, the name records, the pattern bytes and the names' text.
struct Header {
  char Magic[EntryMagic.size()];
  uint32_t FormatVersion;
  uint32_t ByteOrderMark;
  SignatureCache::SourceIdentity Source;
  uint64_t PathBytes;
  uint64_t VersionBytes;
  uint64_t ModuleCount;
  uint64_t NameCount;
  uint64_t BlobBytes;
  uint64_t TextBytes;
  uint64_t BlockCount;
  /// The hash of the block hashes, so that the header vouches for them.
  uint64_t BlockHashesHash;
};

/// A StoredModule, with offsets for its pointers: its bytes and the bits
/// saying which are stated start at \c BlobOffset of the pattern bytes, its
/// names at \c FirstName of the name records.
enum class ContextRequirement : uint8_t {
  None,
  References,
  Caller,
  VTable,
  LoadConfig,
  CodeLayout
};

struct ModuleRecord {
  uint64_t BlobOffset;
  uint64_t FirstName;
  uint32_t LeadingCount;
  uint32_t TailCount;
  uint32_t TotalLen;
  uint32_t PublicNameCount;
  uint32_t ReferenceCount;
  uint16_t CRC16;
  uint8_t CRCLen;
  uint8_t Context;
};

/// A StoredName, with its text at \c TextOffset of the names' text.
struct NameRecord {
  // The top two bits distinguish external and source-local data references
  // without enlarging each name. Text offsets must fit below those bits.
  uint64_t TextOffset;
  uint32_t Length;
  uint32_t Offset;
};

constexpr uint64_t DataReferenceBit = uint64_t(1) << 63;
constexpr uint64_t LocalDataReferenceBit = DataReferenceBit >> 1;
constexpr uint64_t NameKindBits = DataReferenceBit | LocalDataReferenceBit;

static_assert(std::is_trivially_copyable_v<Header> &&
                  std::is_trivially_copyable_v<ModuleRecord> &&
                  std::is_trivially_copyable_v<NameRecord>,
              "records are copied to and from an entry as bytes");
static_assert(sizeof(Header) % EntryAlignment == 0 &&
                  sizeof(uint64_t) % EntryAlignment == 0 &&
                  sizeof(ModuleRecord) % EntryAlignment == 0 &&
                  sizeof(NameRecord) % EntryAlignment == 0,
              "records keep the sections after them aligned");

/// How many bytes say which of \p Count pattern bytes are stated.
uint64_t statedBytes(uint64_t Count) {
  return llvm::divideCeil(Count, CHAR_BIT);
}

/// Where the sections of an entry start; each ends where the next starts.
struct Layout {
  uint64_t BlockHashes = 0;
  uint64_t Payload = 0;
  uint64_t ModuleRecords = 0;
  uint64_t NameRecords = 0;
  uint64_t Blob = 0;
  uint64_t Text = 0;
  uint64_t End = 0;
};

/// The layout \p H describes, or none when a section would not fit in
/// \p Limit bytes or the payload has another number of blocks.
std::optional<Layout> layoutOf(const Header &H, uint64_t Limit) {
  Layout L;
  uint64_t Cursor = sizeof(Header);
  auto take = [&](uint64_t Count, uint64_t Size) {
    if (Cursor > Limit || Count > (Limit - Cursor) / Size)
      return false;
    Cursor += Count * Size;
    return true;
  };
  L.BlockHashes = Cursor;
  if (!take(H.BlockCount, sizeof(uint64_t)))
    return std::nullopt;
  L.Payload = Cursor;
  if (!take(H.PathBytes, 1) || !take(H.VersionBytes, 1))
    return std::nullopt;
  Cursor = llvm::alignTo(Cursor, EntryAlignment);
  L.ModuleRecords = Cursor;
  if (!take(H.ModuleCount, sizeof(ModuleRecord)))
    return std::nullopt;
  L.NameRecords = Cursor;
  if (!take(H.NameCount, sizeof(NameRecord)))
    return std::nullopt;
  L.Blob = Cursor;
  if (!take(H.BlobBytes, 1))
    return std::nullopt;
  L.Text = Cursor;
  if (!take(H.TextBytes, 1))
    return std::nullopt;
  L.End = Cursor;
  if (H.BlockCount != llvm::divideCeil(L.End - L.Payload, EntryBlockBytes))
    return std::nullopt;
  return L;
}

/// The hash of block \p Block of \p Payload.
uint64_t blockHash(llvm::StringRef Payload, size_t Block) {
  return llvm::xxh3_64bits(llvm::arrayRefFromStringRef(
      Payload.substr(Block * EntryBlockBytes, EntryBlockBytes)));
}

/// The canonical path of \p Source, which names its entry.
std::optional<std::string> canonicalPath(const std::filesystem::path &Source) {
  llvm::SmallString<256> Path;
  if (llvm::sys::fs::real_path(Source.string(), Path))
    return std::nullopt;
  return std::string(Path);
}

template <typename Record> Record readRecord(const char *At) {
  Record R;
  std::memcpy(&R, At, sizeof(R));
  return R;
}

template <typename Record> void writeRecord(char *At, const Record &R) {
  std::memcpy(At, &R, sizeof(R));
}

} // namespace

bool SignatureCache::PendingEntry::run(size_t Piece) {
  if (Piece < BlockCount)
    return checkBlock(Piece);
  Piece -= BlockCount;
  if (Piece < NameRuns)
    return makeNames(Piece);
  return makeModules(Piece - NameRuns);
}

bool SignatureCache::PendingEntry::checkBlock(size_t Block) const {
  return blockHash(llvm::StringRef(Data + Payload, PayloadBytes), Block) ==
         readRecord<uint64_t>(Data + BlockHashes + Block * sizeof(uint64_t));
}

bool SignatureCache::PendingEntry::makeNames(size_t Run) {
  std::vector<StoredName> &Names = Result.Names.Names;
  const size_t End = std::min<size_t>(Names.size(), (Run + 1) * NamesPerRun);
  for (size_t I = Run * NamesPerRun; I < End; ++I) {
    const NameRecord R =
        readRecord<NameRecord>(Data + NameRecords + I * sizeof(NameRecord));
    const uint64_t Offset = R.TextOffset & ~NameKindBits;
    if (Offset > TextBytes || R.Length > TextBytes - Offset)
      return false;
    Names[I] = {R.Offset, std::string_view(Data + Text + Offset, R.Length),
                (R.TextOffset & DataReferenceBit) != 0,
                (R.TextOffset & LocalDataReferenceBit) != 0};
  }
  return true;
}

bool SignatureCache::PendingEntry::makeModules(size_t Run) {
  std::vector<StoredModule> &Modules = Result.Modules;
  const size_t NameCount = Result.Names.Names.size();
  const auto *const Bytes = reinterpret_cast<const uint8_t *>(Data + Blob);
  const size_t End =
      std::min<size_t>(Modules.size(), (Run + 1) * ModulesPerRun);
  for (size_t I = Run * ModulesPerRun; I < End; ++I) {
    const ModuleRecord R = readRecord<ModuleRecord>(Data + ModuleRecords +
                                                    I * sizeof(ModuleRecord));
    const uint64_t Count = uint64_t(R.LeadingCount) + R.TailCount;
    const uint64_t Names = uint64_t(R.PublicNameCount) + R.ReferenceCount;
    if (R.BlobOffset > BlobBytes ||
        Count + statedBytes(Count) > BlobBytes - R.BlobOffset ||
        R.FirstName > NameCount || Names > NameCount - R.FirstName ||
        R.Context > uint8_t(ContextRequirement::CodeLayout) ||
        (R.Context == uint8_t(ContextRequirement::References) &&
         R.ReferenceCount == 0) ||
        (R.Context == uint8_t(ContextRequirement::Caller) &&
         R.PublicNameCount == 0))
      return false;
    if (R.Context == uint8_t(ContextRequirement::Caller))
      // Name and module pieces can run in either order, or concurrently.
      // Read immutable records rather than another piece's output buffer.
      for (uint64_t N = R.FirstName; N < R.FirstName + R.PublicNameCount; ++N)
        if (readRecord<NameRecord>(Data + NameRecords + N * sizeof(NameRecord))
                .Offset != 0)
          return false;
    bool HasData = false;
    for (uint64_t N = R.FirstName; N < R.FirstName + Names; ++N) {
      const NameRecord Name =
          readRecord<NameRecord>(Data + NameRecords + N * sizeof(NameRecord));
      if (Name.TextOffset & LocalDataReferenceBit) {
        const uint64_t Offset = Name.TextOffset & ~NameKindBits;
        if (!(Name.TextOffset & DataReferenceBit) || Offset > TextBytes ||
            Name.Length > TextBytes - Offset ||
            !localDataAnchor(
                llvm::StringRef(Data + Text + Offset, Name.Length)) ||
            R.Context == uint8_t(ContextRequirement::LoadConfig))
          return false;
      }
      if ((Name.TextOffset & DataReferenceBit) &&
          (N < R.FirstName + R.PublicNameCount ||
           R.Context == uint8_t(ContextRequirement::None)))
        return false;
      HasData |= (Name.TextOffset & DataReferenceBit) != 0;
    }
    if (HasData && R.Context != uint8_t(ContextRequirement::LoadConfig)) {
      llvm::SmallVector<std::pair<uint32_t, uint64_t>, 8> Sites;
      for (uint64_t N = R.FirstName + R.PublicNameCount;
           N < R.FirstName + Names; ++N) {
        const NameRecord Name =
            readRecord<NameRecord>(Data + NameRecords + N * sizeof(NameRecord));
        if (Name.Offset >= R.TotalLen)
          return false;
        Sites.emplace_back(Name.Offset, Name.TextOffset & NameKindBits);
      }
      std::sort(Sites.begin(), Sites.end());
      for (size_t N = 1; N < Sites.size(); ++N)
        if (Sites[N].first == Sites[N - 1].first &&
            Sites[N].second != Sites[N - 1].second)
          return false;
    }
    StoredModule &M = Modules[I];
    M.Bytes = Bytes + R.BlobOffset;
    M.Stated = M.Bytes + Count;
    M.Names = Result.Names.Names.data() + R.FirstName;
    M.LeadingCount = R.LeadingCount;
    M.TailCount = R.TailCount;
    M.TotalLen = R.TotalLen;
    M.PublicNameCount = R.PublicNameCount;
    M.ReferenceCount = R.ReferenceCount;
    M.CRC16 = R.CRC16;
    M.CRCLen = R.CRCLen;
    M.RequireReferences = R.Context == uint8_t(ContextRequirement::References);
    M.RequireCaller = R.Context == uint8_t(ContextRequirement::Caller);
    M.IsVTable = R.Context == uint8_t(ContextRequirement::VTable);
    M.IsLoadConfig = R.Context == uint8_t(ContextRequirement::LoadConfig);
    M.IsCodeLayout = R.Context == uint8_t(ContextRequirement::CodeLayout);
    if (M.IsVTable || M.IsLoadConfig || M.IsCodeLayout) {
      // Module and name pieces may be decoded in either order. Validate a
      // temporary view of immutable wire names, never another piece's output.
      llvm::SmallVector<StoredName, 8> TableNames;
      for (uint64_t N = R.FirstName; N < R.FirstName + Names; ++N) {
        const NameRecord Name =
            readRecord<NameRecord>(Data + NameRecords + N * sizeof(NameRecord));
        const uint64_t Offset = Name.TextOffset & ~NameKindBits;
        if (Offset > TextBytes || Name.Length > TextBytes - Offset)
          return false;
        TableNames.emplace_back(
            Name.Offset, std::string_view(Data + Text + Offset, Name.Length),
            (Name.TextOffset & DataReferenceBit) != 0,
            (Name.TextOffset & LocalDataReferenceBit) != 0);
      }
      StoredModule Table = M;
      Table.Names = TableNames.data();
      if (M.IsVTable       ? !isValidVTableContext(Table)
          : M.IsCodeLayout ? !isValidCodeLayoutContext(Table)
                           : !isValidRuntimePointerContext(Table))
        return false;
    }
    if (R.Context != uint8_t(ContextRequirement::None) &&
        !SignatureMatcher::isFullyVerified(M))
      return false;
  }
  return true;
}

SignatureCache::SignatureCache(std::string Directory, uint64_t MinSourceBytes)
    : Directory(std::move(Directory)), MinSourceBytes(MinSourceBytes) {}

std::optional<SignatureCache> SignatureCache::fromEnvironment() {
  if (std::optional<std::string> Value =
          llvm::sys::Process::GetEnv(EnvironmentVariable);
      Value && !Value->empty()) {
    if (*Value == DisabledValue)
      return std::nullopt;
    return SignatureCache(std::move(*Value), DefaultMinSourceBytes);
  }
  llvm::SmallString<256> Path;
  if (!llvm::sys::path::cache_directory(Path))
    return std::nullopt;
  llvm::sys::path::append(Path, VendorDirectory, CacheDirectory);
  return SignatureCache(std::string(Path), DefaultMinSourceBytes);
}

std::optional<SignatureCache::SourceIdentity>
SignatureCache::identify(const std::filesystem::path &Source) {
  llvm::sys::fs::file_status Status;
  if (llvm::sys::fs::status(Source.string(), Status) ||
      !llvm::sys::fs::is_regular_file(Status))
    return std::nullopt;
  const llvm::sys::fs::UniqueID ID = Status.getUniqueID();
  return SourceIdentity{Status.getSize(),
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            Status.getLastModificationTime().time_since_epoch())
                            .count(),
                        ID.getDevice(), ID.getFile()};
}

std::optional<std::string>
SignatureCache::entryPath(const std::filesystem::path &Source) const {
  const std::optional<std::string> Canonical = canonicalPath(Source);
  if (!Canonical)
    return std::nullopt;
  llvm::SmallString<256> Path(Directory);
  llvm::sys::path::append(
      Path, llvm::utohexstr(llvm::xxh3_64bits(*Canonical), /*LowerCase=*/true) +
                EntryExtension);
  return std::string(Path);
}

std::optional<SignatureCache::PendingEntry>
SignatureCache::open(const std::filesystem::path &Source,
                     const SourceIdentity &Identity) const {
  const std::optional<std::string> Canonical = canonicalPath(Source);
  const std::optional<std::string> Path = entryPath(Source);
  if (!Canonical || !Path)
    return std::nullopt;
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Mapping =
      llvm::MemoryBuffer::getFile(*Path, /*IsText=*/false,
                                  /*RequiresNullTerminator=*/false);
  if (!Mapping)
    return std::nullopt;
  const llvm::StringRef Data = (*Mapping)->getBuffer();
  if (Data.size() < sizeof(Header))
    return std::nullopt;
  const Header H = readRecord<Header>(Data.data());
  if (llvm::StringRef(H.Magic, sizeof(H.Magic)) != EntryMagic ||
      H.FormatVersion != EntryFormatVersion ||
      H.ByteOrderMark != EntryByteOrderMark || H.Source != Identity)
    return std::nullopt;
  const std::optional<Layout> L = layoutOf(H, Data.size());
  if (!L || L->End != Data.size() ||
      llvm::xxh3_64bits(llvm::arrayRefFromStringRef(
          Data.slice(L->BlockHashes, L->Payload))) != H.BlockHashesHash ||
      Data.substr(L->Payload, H.PathBytes + H.VersionBytes) !=
          *Canonical + VersionString)
    return std::nullopt;

  PendingEntry Pending;
  Pending.Data = Data.data();
  Pending.BlockHashes = L->BlockHashes;
  Pending.Payload = L->Payload;
  Pending.PayloadBytes = L->End - L->Payload;
  Pending.ModuleRecords = L->ModuleRecords;
  Pending.NameRecords = L->NameRecords;
  Pending.Blob = L->Blob;
  Pending.Text = L->Text;
  Pending.BlobBytes = H.BlobBytes;
  Pending.TextBytes = H.TextBytes;
  Pending.BlockCount = H.BlockCount;
  Pending.NameRuns = llvm::divideCeil(H.NameCount, NamesPerRun);
  Pending.ModuleRuns = llvm::divideCeil(H.ModuleCount, ModulesPerRun);
  Pending.Result.Names.Names.resize(H.NameCount);
  Pending.Result.Modules.resize(H.ModuleCount);
  Pending.Result.Mapping = std::move(*Mapping);
  return Pending;
}

std::optional<SignatureCache::Entry>
SignatureCache::load(const std::filesystem::path &Source,
                     const SourceIdentity &Identity) const {
  std::optional<PendingEntry> Pending = open(Source, Identity);
  if (!Pending)
    return std::nullopt;
  for (size_t Piece = 0; Piece < Pending->pieces(); ++Piece)
    if (!Pending->run(Piece))
      return std::nullopt;
  return std::move(*Pending).take();
}

void SignatureCache::store(const std::filesystem::path &Source,
                           const SourceIdentity &Identity,
                           llvm::ArrayRef<StoredModule> Modules) const {
  if (Identity.Size < MinSourceBytes)
    return;
  const std::optional<std::string> Canonical = canonicalPath(Source);
  const std::optional<std::string> Path = entryPath(Source);
  if (!Canonical || !Path)
    return;

  Header H = {};
  std::memcpy(H.Magic, EntryMagic.data(), sizeof(H.Magic));
  H.FormatVersion = EntryFormatVersion;
  H.ByteOrderMark = EntryByteOrderMark;
  H.Source = Identity;
  H.PathBytes = Canonical->size();
  H.VersionBytes = std::string_view(VersionString).size();
  H.ModuleCount = Modules.size();
  for (const StoredModule &M : Modules) {
    const uint64_t Count = uint64_t(M.LeadingCount) + M.TailCount;
    H.BlobBytes += Count + statedBytes(Count);
    for (const StoredName &Name :
         llvm::ArrayRef(M.Names, M.PublicNameCount + M.ReferenceCount)) {
      ++H.NameCount;
      H.TextBytes += Name.Name.size();
    }
  }
  if (H.TextBytes >= LocalDataReferenceBit)
    return;
  // The header and the block hashes keep the payload aligned, so the
  // payload's size does not depend on how many blocks it has.
  H.BlockCount = llvm::divideCeil(
      llvm::alignTo(H.PathBytes + H.VersionBytes, EntryAlignment) +
          H.ModuleCount * sizeof(ModuleRecord) +
          H.NameCount * sizeof(NameRecord) + H.BlobBytes + H.TextBytes,
      EntryBlockBytes);
  const std::optional<Layout> L =
      layoutOf(H, std::numeric_limits<uint64_t>::max());
  if (!L)
    return;

  // The whole entry is laid out in memory, so that its blocks are hashed
  // before the hashes and the header that vouches for them are written.
  std::string Written(L->End, '\0');
  std::memcpy(Written.data() + L->Payload, Canonical->data(),
              Canonical->size());
  std::memcpy(Written.data() + L->Payload + H.PathBytes, VersionString,
              H.VersionBytes);
  uint64_t Blob = 0, Name = 0, Text = 0;
  for (size_t I = 0; I < Modules.size(); ++I) {
    const StoredModule &M = Modules[I];
    const uint64_t Count = uint64_t(M.LeadingCount) + M.TailCount;
    writeRecord(Written.data() + L->ModuleRecords + I * sizeof(ModuleRecord),
                ModuleRecord{
                    Blob, Name, M.LeadingCount, M.TailCount, M.TotalLen,
                    M.PublicNameCount, M.ReferenceCount, M.CRC16, M.CRCLen,
                    uint8_t(M.RequireReferences ? ContextRequirement::References
                            : M.RequireCaller   ? ContextRequirement::Caller
                            : M.IsVTable        ? ContextRequirement::VTable
                            : M.IsLoadConfig    ? ContextRequirement::LoadConfig
                            : M.IsCodeLayout    ? ContextRequirement::CodeLayout
                                                : ContextRequirement::None)});
    if (Count != 0) {
      std::memcpy(Written.data() + L->Blob + Blob, M.Bytes, Count);
      std::memcpy(Written.data() + L->Blob + Blob + Count, M.Stated,
                  statedBytes(Count));
    }
    Blob += Count + statedBytes(Count);
    for (const StoredName &N :
         llvm::ArrayRef(M.Names, M.PublicNameCount + M.ReferenceCount)) {
      writeRecord(Written.data() + L->NameRecords + Name * sizeof(NameRecord),
                  NameRecord{Text | (N.IsData ? DataReferenceBit : 0) |
                                 (N.IsLocalData ? LocalDataReferenceBit : 0),
                             static_cast<uint32_t>(N.Name.size()), N.Offset});
      std::memcpy(Written.data() + L->Text + Text, N.Name.data(),
                  N.Name.size());
      ++Name;
      Text += N.Name.size();
    }
  }
  const llvm::StringRef Payload =
      llvm::StringRef(Written).drop_front(L->Payload);
  neverd::parallelForEach(H.BlockCount, [&](auto Claim, size_t Total) {
    for (size_t Block = Claim(); Block < Total; Block = Claim())
      writeRecord(Written.data() + L->BlockHashes + Block * sizeof(uint64_t),
                  blockHash(Payload, Block));
  });
  H.BlockHashesHash = llvm::xxh3_64bits(llvm::arrayRefFromStringRef(
      llvm::StringRef(Written).slice(L->BlockHashes, L->Payload)));
  writeRecord(Written.data(), H);

  // Only a source that is still as it was parsed may be kept, and an entry
  // replaces the one before it at once or not at all.
  if (identify(Source) != Identity ||
      llvm::sys::fs::create_directories(Directory))
    return;
  llvm::consumeError(
      llvm::writeToOutput(*Path, [&](llvm::raw_ostream &OS) -> llvm::Error {
        OS << Written;
        return llvm::Error::success();
      }));
}
