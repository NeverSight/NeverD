//===- BunSourceMap.cpp - Bun serialized source maps -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bun serialized source maps.
///
//===----------------------------------------------------------------------===//

// Fixed layout references and license: docs/web-bun-profile.md, LICENSES/bun.
// This bounded C++ reader deliberately does not use Bun's trusted-memory
// reader.
#include "BunSourceMap.h"

#include "neverd/web/Error.h"

#include "llvm/Support/Compression.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <array>
#include <limits>

namespace neverd::web {
namespace {
// Byte-addressed little-endian reads work on unaligned embedded maps too.
class Reader {
  std::string_view Bytes;
  uint64_t At = 0;

public:
  explicit Reader(std::string_view Bytes) : Bytes(Bytes) {}
  uint64_t remaining() const { return Bytes.size() - At; }
  std::string_view take(uint64_t Size) {
    if (Size > remaining())
      throw Error("bun_map_truncated");
    const auto Result = Bytes.substr(At, Size);
    At += Size;
    return Result;
  }
  uint64_t number(unsigned Width) {
    const auto B = take(Width);
    uint64_t Result = 0;
    for (unsigned I = 0; I < Width; ++I)
      Result |= uint64_t(uint8_t(B[I])) << (I * 8);
    return Result;
  }
  int64_t varint() {
    uint32_t Raw = 0;
    for (unsigned I = 0; I != 5; ++I) {
      const auto B = number(1);
      if (I == 4 && B > 15)
        throw Error("bun_map_invalid_varint");
      Raw |= uint32_t(B & 127) << (I * 7);
      if (!(B & 128)) {
        if (I && B == 0)
          throw Error("bun_map_invalid_varint");
        return (Raw & 1) ? -int64_t(Raw >> 1) - 1 : int64_t(Raw >> 1);
      }
    }
    throw Error("bun_map_invalid_varint");
  }
  void finish() const {
    if (remaining())
      throw Error("bun_map_unconsumed_bytes");
  }
};

// Preflight only the Zstandard envelope; LLVM's native decoder remains the
// authority for compressed blocks and checksums. A known content size, bounded
// window and exactly one dictionary-free ordinary frame are required before
// allocating output. No skippable/concatenated frames or trailing bytes.
uint64_t frameSize(std::string_view Bytes, uint64_t Budget) {
  Reader R(Bytes);
  if (R.number(4) != 0xfd2fb528)
    throw Error("bun_map_unsupported_zstd_frame");
  const auto Descriptor = R.number(1);
  if (Descriptor & 0x1b) // Reserved/unused bits and dictionary ID field.
    throw Error("bun_map_unsupported_zstd_frame");
  const bool Single = Descriptor & 32;
  uint64_t Window = 0;
  if (!Single) {
    const auto W = R.number(1);
    const uint64_t Base = uint64_t(1) << (10 + (W >> 3));
    Window = Base + (Base / 8) * (W & 7);
  }
  const unsigned SizeCode = Descriptor >> 6;
  const unsigned Width = SizeCode ? 1U << SizeCode : Single ? 1 : 0;
  if (!Width)
    throw Error("bun_map_unknown_zstd_content_size");
  const uint64_t Size = R.number(Width) + (Width == 2 ? 256 : 0);
  if (Single)
    Window = Size;
  if (Size > Budget || Window > MaxBunMapDecodedBytes)
    throw Error("bun_map_decompression_budget_exceeded");
  const auto MaxBlock = std::min<uint64_t>(128 * 1024, Window);
  uint64_t Blocks = 0;
  do {
    if (++Blocks > 100000)
      throw Error("bun_map_zstd_block_budget_exceeded");
    const auto Header = R.number(3);
    const auto Type = (Header >> 1) & 3, Size = Header >> 3;
    if (Type == 3 || Size > MaxBlock)
      throw Error("bun_map_invalid_zstd_block");
    R.take(Type == 1 ? 1 : Size);
    if (Header & 1)
      break;
  } while (true);
  if (Descriptor & 4)
    R.take(4);
  R.finish();
  return Size;
}

std::string decompress(std::string_view Bytes, uint64_t Size) {
  std::string Result(Size, '\0');
  size_t Written = Size;
  const llvm::ArrayRef<uint8_t> Input(
      reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size());
  if (auto E = llvm::compression::zstd::decompress(
          Input, reinterpret_cast<uint8_t *>(Result.data()), Written)) {
    llvm::consumeError(std::move(E));
    throw Error("bun_map_invalid_zstd_data");
  }
  if (Written != Size)
    throw Error("bun_map_zstd_size_mismatch");
  if (!llvm::json::isUTF8(Result))
    throw Error("bun_map_invalid_source_unicode");
  return Result;
}

void coordinate(int64_t &Value, int64_t Delta) {
  Value += Delta; // Both operands are checked i32 values, so i64 cannot wrap.
  if (Value < 0 || Value > INT32_MAX)
    throw Error("bun_map_invalid_coordinate");
}

struct State {
  int64_t Line, Column, OriginalLine, OriginalColumn, Source;
};

uint64_t mask(Reader &R, unsigned Deltas) {
  const auto Value = R.number(8);
  if (Value >> Deltas) // Deltas <= 63; unused bits must be zero.
    throw Error("bun_map_invalid_mask");
  return Value;
}

void decodeMappings(SourceMap &Map, std::string_view Bytes,
                    uint64_t SourceCount) {
  Reader Header(Bytes);
  if (Header.number(8) != Bytes.size())
    throw Error("bun_map_invalid_mapping_header");
  const auto Count = Header.number(8), InputLines = Header.number(8);
  const auto Syncs = Header.number(4), StreamOffset = Header.number(4);
  if (Count > MaxSourceMapSegments)
    throw Error("source_map_segment_budget_exceeded");
  if (Syncs != (Count + 63) / 64 || StreamOffset != 32 + Syncs * 24)
    throw Error("bun_map_invalid_mapping_header");
  const auto Entries = Header.take(Syncs * 24);
  const auto Stream = Header.take(Header.remaining());
  if (Stream.empty() || Stream.back() != 0)
    throw Error("bun_map_invalid_mapping_padding");
  Reader Windows(Stream.substr(0, Stream.size() - 1));
  Reader Seeds(Entries);
  uint64_t MaxOriginalLine = 0;
  Map.Segments.reserve(Count);
  auto Append = [&](const State &S) {
    if (S.Line < 0 || S.Line > INT32_MAX || S.Column < 0 ||
        S.Column > INT32_MAX || S.OriginalLine < 0 ||
        S.OriginalLine > INT32_MAX || S.OriginalColumn < 0 ||
        S.OriginalColumn > INT32_MAX || S.Source < 0 ||
        uint64_t(S.Source) >= SourceCount)
      throw Error("bun_map_invalid_coordinate");
    SourceMapSegment Segment;
    Segment.Generated = {uint64_t(S.Line), uint64_t(S.Column)};
    Segment.Original = {uint64_t(S.OriginalLine), uint64_t(S.OriginalColumn)};
    Segment.SourceIndex = uint64_t(S.Source);
    if (!Map.Segments.empty() &&
        Segment.Generated < Map.Segments.back().Generated)
      throw Error("bun_map_unordered_generated_positions");
    Map.Segments.push_back(Segment);
    MaxOriginalLine = std::max(MaxOriginalLine, uint64_t(S.OriginalLine));
  };
  for (uint64_t W = 0; W < Syncs; ++W) {
    State S;
    S.Line = Seeds.number(4);
    S.Column = Seeds.number(4);
    const auto Start = Seeds.number(4);
    S.OriginalLine = Seeds.number(4);
    S.OriginalColumn = Seeds.number(4);
    S.Source = Seeds.number(4);
    if (Start != Stream.size() - 1 - Windows.remaining())
      throw Error("bun_map_invalid_window_offset");
    const auto N = Windows.number(1), Flags = Windows.number(1);
    if (N != std::min<uint64_t>(64, Count - W * 64))
      throw Error("bun_map_invalid_window_count");
    if (Flags & ~12U)
      throw Error("bun_map_unsupported_window_flags");
    const auto GenSize = Windows.number(2), LineSize = Windows.number(2),
               ColSize = Windows.number(2);
    const unsigned Deltas = unsigned(N - 1);
    const auto GenMask = mask(Windows, Deltas);
    const auto LineMask = mask(Windows, Deltas);
    const auto ColMask = mask(Windows, Deltas);
    Reader Gen(Windows.take(GenSize)), Line(Windows.take(LineSize)),
        Col(Windows.take(ColSize));
    std::array<int64_t, 63> GenLine{};
    for (unsigned I = 0; I < Deltas; ++I)
      GenLine[I] = (GenMask >> I) & 1;
    if (Flags & 4) {
      int Previous = -1;
      for (;;) {
        const auto Index = Windows.number(1);
        if (Index == 255)
          break;
        if (Index >= Deltas || int(Index) <= Previous)
          throw Error("bun_map_invalid_line_exception");
        const auto Delta = Windows.varint();
        if (Delta <= 1 || !GenLine[Index])
          throw Error("bun_map_invalid_line_exception");
        GenLine[Index] = Delta;
        Previous = int(Index);
      }
      if (Previous == -1)
        throw Error("bun_map_invalid_line_exception");
    }
    const auto SourceMask = Flags & 8 ? mask(Windows, Deltas) : UINT64_MAX;
    bool SourceChanged = false;
    Append(S);
    for (unsigned I = 0; I < Deltas; ++I) {
      const auto DGenCol = Gen.varint(), DGenLine = GenLine[I];
      const auto DLine = (LineMask >> I) & 1 ? DGenLine : Line.varint();
      const auto DCol = (ColMask >> I) & 1 ? DGenCol : Col.varint();
      if ((!((LineMask >> I) & 1) && DLine == DGenLine) ||
          (!((ColMask >> I) & 1) && DCol == DGenCol))
        throw Error("bun_map_noncanonical_delta");
      if (!((SourceMask >> I) & 1)) {
        const auto Delta = Windows.varint();
        if (!Delta)
          throw Error("bun_map_noncanonical_delta");
        coordinate(S.Source, Delta);
        SourceChanged = true;
      }
      coordinate(S.Line, DGenLine);
      if (DGenLine)
        S.Column = 0;
      coordinate(S.Column, DGenCol);
      coordinate(S.OriginalLine, DLine);
      coordinate(S.OriginalColumn, DCol);
      Append(S);
    }
    if ((Flags & 8) && !SourceChanged)
      throw Error("bun_map_noncanonical_delta");
    Gen.finish();
    Line.finish();
    Col.finish();
  }
  Windows.finish();
  if (InputLines != MaxOriginalLine + 1)
    throw Error("bun_map_invalid_input_line_count");
}

struct Pointer {
  uint64_t Offset, Size;
};
} // namespace

bool bunSourceMapAvailable() { return llvm::compression::zstd::isAvailable(); }

SourceMap decodeBunSourceMap(const BunExtraction &E, const BunModule &M) {
  if (!bunSourceMapAvailable())
    throw Error("bun_source_map_zstd_unavailable");
  if (M.SourceMap == NoBunIndex || M.SourceMap >= E.Regions.size())
    throw Error("bun_source_map_not_supplied");
  const auto &Region = E.Regions[M.SourceMap];
  if (Region.Content.size() > MaxSourceMapBytes)
    throw Error("bun_map_byte_budget_exceeded");
  const auto Bytes = Region.Content.read(0, Region.Content.size());
  Reader Header(Bytes);
  const auto Count = Header.number(4), MapSize = Header.number(4);
  if (Count > MaxBunMapSources)
    throw Error("source_map_source_budget_exceeded");
  const auto Tables = Header.take(Count * 16);
  const uint64_t MapStart = 8 + Count * 16;
  const auto Mappings = Header.take(MapSize);
  // Writer order is names, then independently compressed contents. Requiring
  // exact partitioning rejects aliases, hidden padding and amplification.
  Reader Pointers(Tables);
  std::vector<Pointer> Names, Contents;
  uint64_t Next = MapStart + MapSize, NameBytes = 0;
  for (uint64_t I = 0; I < Count * 2; ++I) {
    Pointer P{Pointers.number(4), Pointers.number(4)};
    if (P.Offset != Next || P.Offset > Bytes.size() ||
        P.Size > Bytes.size() - P.Offset)
      throw Error("bun_map_invalid_pointer");
    Next += P.Size;
    if (I < Count) {
      if (P.Size > 32768 || P.Size > MaxBunNameBytes - NameBytes)
        throw Error("bun_map_name_budget_exceeded");
      NameBytes += P.Size;
      if (!llvm::json::isUTF8(std::string_view(Bytes).substr(P.Offset, P.Size)))
        throw Error("bun_map_invalid_name_unicode");
      Names.push_back(P);
    } else {
      Contents.push_back(P);
    }
  }
  if (Next != Bytes.size())
    throw Error("bun_map_unconsumed_bytes");
  SourceMap Map;
  Map.Profile = BunSourceMapProfile;
  Map.ArtifactID = Region.ID;
  Map.BlobHash = Region.BlobHash;
  Map.ID = identity("source-map", {Map.ArtifactID, Map.BlobHash, Map.Profile});
  Map.MappedAnchorsOnly = true;
  Map.Embedding = SourceMapEmbedding{E.ArtifactID,
                                     E.ID,
                                     M.ID,
                                     M.SourceArtifactID,
                                     Region.Offset,
                                     Region.Content.size(),
                                     Region.Offset + MapStart,
                                     MapSize};
  decodeMappings(Map, Mappings, Count);
  // Preflight every frame's cumulative output before any decompression.
  std::vector<uint64_t> Sizes;
  uint64_t Decoded = 0;
  for (const auto &P : Contents) {
    const auto Size = frameSize(
        std::string_view(Bytes).substr(P.Offset, P.Size),
        std::min(MaxBunMapSourceBytes, MaxBunMapDecodedBytes - Decoded));
    Sizes.push_back(Size);
    Decoded += Size;
  }
  for (uint64_t I = 0; I < Count; ++I) {
    const auto &N = Names[I], &C = Contents[I];
    const auto Compressed = std::string_view(Bytes).substr(C.Offset, C.Size);
    MappedSource Source;
    const auto Index = std::to_string(I);
    Source.ID = identity("bun-map-source", {Map.ID, Index});
    Source.Name = Bytes.substr(N.Offset, N.Size);
    Source.Contents = decompress(Compressed, Sizes[I]);
    Source.Storage = MappedSourceStorage{Region.Offset + N.Offset, N.Size,
                                         Region.Offset + C.Offset, C.Size,
                                         sha256(Compressed)};
    Map.Sources.push_back(std::move(Source));
  }
  return Map;
}
} // namespace neverd::web
