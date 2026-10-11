//===- Bun.cpp - Qualified Bun graph extraction ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Qualified Bun graph extraction.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/Bun.h"

#include "BunContainer.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"
#include "neverd/web/SourceLocation.h"

#include <algorithm>
#include <array>
#include <optional>
#include <set>

// Layout reference: oven-sh/bun 744846f844374847c902b5e7fd59b4342a51ef99,
// src/standalone_graph/StandaloneModuleGraph.rs and src/exe_format/elf.rs.
// This reader does not link Bun or JavaScriptCore. See docs/web-bun-profile.md.
namespace neverd::web {
namespace {
using namespace bun_detail;

std::string profile(std::string_view Platform, std::string_view Architecture,
                    std::string_view Format, bool Prelinked) {
  std::string Result = Prelinked ? "bun-71d0d439-prelinked-" : "bun-1.4.2-";
  Result += Platform;
  Result += "-";
  Result += Architecture;
  Result += "-";
  Result += Format;
  return Result + "-v1";
}

class GraphReader {
  const Artifact &Input;
  Reader R;
  BunExtraction Out;
  uint64_t Table = 0;
  uint64_t NameBytes = 0;
  std::vector<Range> Occupied;
  std::set<std::string> Names;

  Range pointer(std::string_view Bytes, size_t At) {
    return {number(Bytes, At, 4), number(Bytes, At + 4, 4)};
  }

  uint32_t region(Range Part, const char *Kind, uint32_t Module = NoBunIndex) {
    within(Part, Input.Content.size());
    BunRegion V;
    V.Offset = Part.Offset;
    V.Content = Input.Content.slice(Part.Offset, Part.Size);
    V.Kind = Kind;
    V.Module = Module;
    // Defer hashing until all pointer ranges are proved disjoint. Otherwise
    // malicious aliases could force thousands of full-container hash reads.
    Out.Regions.push_back(std::move(V));
    return uint32_t(Out.Regions.size() - 1);
  }

  uint32_t payload(Range Part, const char *Kind, uint32_t Module,
                   unsigned Terminator = 0, bool Required = false,
                   bool Aligned = false) {
    within(Part, Table);
    if (!Part.Size && !Required)
      return NoBunIndex;
    if (Aligned && Part.Offset % 128 != 120)
      throw Error("bun_invalid_bytecode_alignment");
    within({Part.Offset, Part.Size + Terminator}, Table);
    if (Terminator &&
        R.read({Part.end(), Terminator}) != std::string(Terminator, '\0'))
      throw Error("bun_missing_terminator");
    Occupied.push_back({Part.Offset, Part.Size + Terminator});
    return region({Out.GraphOffset + Part.Offset, Part.Size}, Kind, Module);
  }

  uint32_t name(Range Part, const char *Kind, uint32_t Module,
                bool Required = false) {
    if (Part.Size > 32768 || Part.Size > MaxBunNameBytes - NameBytes)
      throw Error("bun_name_budget_exceeded");
    NameBytes += Part.Size;
    const auto Index = payload(Part, Kind, Module, 1, Required);
    if (Index != NoBunIndex) {
      const auto Text = R.read(Part);
      if (Text.find('\0') != std::string::npos)
        throw Error("bun_invalid_name");
      if (Required &&
          (!Text.starts_with(Out.Platform == "windows" ? "B:/~BUN/"
                                                       : "/$bunfs/") ||
           !Names.insert(Text).second))
        throw Error("bun_invalid_module_name");
    }
    return Index;
  }

public:
  GraphReader(const Artifact &Input, const Container &C)
      : Input(Input), R{Input.Content.slice(C.Graph.Offset, C.Graph.Size)} {
    Out.ArtifactID = Input.ID;
    Out.GraphOffset = C.Graph.Offset;
    Out.GraphSize = C.Graph.Size;
    Out.ContainerFormat = C.Format;
    Out.Platform = C.Platform;
    Out.Architecture = C.Architecture;
  }

  BunExtraction run() {
    const auto Footer = R.read({Out.GraphSize - 48, 48});
    if (Footer.substr(32) != "\n---- Bun! ----\n")
      throw Error("bun_invalid_trailer");
    const auto DataSize = number(Footer, 0, 8);
    if (DataSize != Out.GraphSize - 48)
      throw Error("bun_invalid_byte_count");
    const auto Modules = pointer(Footer, 8);
    const auto Args = pointer(Footer, 20);
    Out.EntryPoint = number(Footer, 16, 4);
    Out.Flags = number(Footer, 28, 4);
    constexpr uint32_t RequiredFlags =
        (1 << 4) | (1 << 5) | (1 << 6) | (1 << 8);
    if ((Out.Flags & ~uint32_t(0x1fff)) ||
        (Out.Flags & RequiredFlags) != RequiredFlags)
      throw Error("bun_unsupported_graph_flags");
    Out.Profile = profile(Out.Platform, Out.Architecture, Out.ContainerFormat,
                          Out.Flags & ((1 << 11) | (1 << 12)));
    Out.ID =
        identity("bun-extraction", {Input.ID, Input.BlobHash, Out.Profile});
    within(Modules, DataSize);
    if (!Modules.Size || Modules.Size % 52)
      throw Error("bun_invalid_module_table");
    const auto Count = Modules.Size / 52;
    if (Count > MaxBunModules)
      throw Error("bun_module_budget_exceeded");
    if (Out.EntryPoint >= Count)
      throw Error("bun_invalid_entry_point");
    Table = Modules.Offset;
    const auto Records = R.read(Modules);
    uint64_t SourceLo = Table, SourceHi = 0;
    for (uint32_t I = 0; I < Count; ++I) {
      const auto At = I * 52;
      BunModule M;
      M.Encoding = number(Records, At + 48, 1);
      M.Loader = number(Records, At + 49, 1);
      M.Format = number(Records, At + 50, 1);
      M.Side = number(Records, At + 51, 1);
      const bool JS = M.Loader <= 3;
      if (M.Encoding > 2 || M.Loader > 21 || M.Format > 2 || M.Side > 1 ||
          (JS && !M.Format) || (!JS && M.Format) ||
          (M.Encoding && !JS && M.Loader != 13) ||
          (JS && !M.Side && !M.Encoding) || (JS && M.Side && M.Encoding))
        throw Error("bun_unsupported_module_encoding");
      const auto Contents = pointer(Records, At + 8);
      if (M.Encoding == 2 && ((Contents.Offset | Contents.Size) & 1))
        throw Error("bun_invalid_utf16_range");
      M.Name = name(pointer(Records, At), "module_name", I, true);
      M.Contents = payload(Contents, JS ? "javascript_storage" : "asset", I,
                           M.Encoding == 2 ? 2 : 1, true);
      SourceLo = std::min(SourceLo, Contents.Offset);
      SourceHi = std::max(SourceHi, Contents.end() + (M.Encoding == 2 ? 2 : 1));
      M.SourceMap =
          payload(pointer(Records, At + 16), "serialized_source_map", I);
      M.Bytecode =
          payload(pointer(Records, At + 24), "jsc_bytecode", I, 0, false, true);
      M.ModuleInfo = payload(pointer(Records, At + 32), "module_info", I);
      M.BytecodeOrigin = name(pointer(Records, At + 40), "bytecode_origin", I);
      Out.Modules.push_back(std::move(M));
    }

    // Fixed 1.4.2 writer order; no probing of arbitrary trailing bytes.
    uint64_t At = Modules.end();
    within({At, Count * 4 + 4}, DataSize);
    At += Count * 4; // Non-cryptographic WTF source hashes, not trust evidence.
    const auto Builtins = R.integer(At, 4);
    At += 4;
    if (Builtins > MaxBunBuiltins)
      throw Error("bun_builtin_budget_exceeded");
    within({At, Builtins * 12}, DataSize);
    std::set<uint32_t> BuiltinIDs;
    for (uint64_t I = 0; I < Builtins; ++I) {
      const auto Record = R.read({At, 12});
      At += 12;
      if (!BuiltinIDs.insert(number(Record, 0, 4)).second)
        throw Error("bun_duplicate_builtin_id");
      payload(pointer(Record, 4), "builtin_bytecode", NoBunIndex, 0, false,
              true);
    }
    auto SharedTable = [&](const char *Kind, bool Aligned) {
      within({At, 8}, DataSize);
      const auto Ptr = pointer(R.read({At, 8}), 0);
      At += 8;
      if (!Ptr.Size)
        throw Error("bun_empty_shared_table");
      payload(Ptr, Kind, NoBunIndex, 0, false, Aligned);
    };
    if (Out.Flags & (1 << 7))
      SharedTable("bytecode_string_table", true);
    within({At, 4}, DataSize);
    Out.StartupCount = R.integer(At, 4);
    At += 4;
    if (Out.StartupCount > Count)
      throw Error("bun_invalid_startup_count");
    if (Out.Flags & (1 << 9))
      SharedTable("module_info_string_table", false);
    // 71d0d439 adds these records after the module-info table. The linked
    // payload flag (bit 13) changes cache aliasing and is not admitted here.
    if (Out.Flags & (1 << 11)) {
      SharedTable("prelinked_module_graph", true);
      within({At, 4}, DataSize);
      const auto Files = R.integer(At, 4);
      At += 4;
      if (Files > Count)
        throw Error("bun_invalid_prelinked_files");
      within({At, Files * 4}, DataSize);
      std::set<uint32_t> Seen;
      for (uint64_t I = 0; I < Files; ++I) {
        const auto File = R.integer(At + I * 4, 4);
        if (File >= Count || !Seen.insert(File).second)
          throw Error("bun_invalid_prelinked_files");
      }
      At += Files * 4;
    }
    if (Out.Flags & (1 << 12)) {
      // Preserve build-time runtime defaults as opaque graph metadata. They
      // cannot configure the host or change the source decoder's semantics.
      within({At, 8}, DataSize);
      At += 8;
    }
    // The fixed StringBuilder writes a terminator even for empty argv.
    within(Args, DataSize);
    if (Args.Offset != At || Args.end() + 1 != DataSize ||
        R.integer(Args.end(), 1) != 0) {
      throw Error("bun_invalid_compile_arguments");
    }
    for (const auto &V : Out.Regions)
      if (V.Kind != "javascript_storage" && V.Kind != "asset" &&
          overlaps({V.Offset - Out.GraphOffset, V.Content.size()},
                   {SourceLo, SourceHi - SourceLo}))
        throw Error("bun_noncontiguous_source_region");
    std::sort(Occupied.begin(), Occupied.end(),
              [](Range A, Range B) { return A.Offset < B.Offset; });
    for (size_t I = 1; I < Occupied.size(); ++I)
      if (overlaps(Occupied[I - 1], Occupied[I]))
        throw Error("bun_overlapping_payloads");
    region({Out.GraphOffset + Table, Out.GraphSize - Table}, "graph_metadata");
    region({Out.GraphOffset - 8, 8}, "graph_length");
    region({0, Out.GraphOffset - 8}, "native_container_prefix");
    const auto End = Out.GraphOffset + Out.GraphSize;
    region({End, Input.Content.size() - End}, "native_container_suffix");
    for (size_t I = 0; I < Out.Regions.size(); ++I) {
      auto &V = Out.Regions[I];
      V.BlobHash = V.Content.digest();
      const auto Offset = std::to_string(V.Offset);
      const auto Size = std::to_string(V.Content.size());
      const auto Index = std::to_string(I);
      // The extraction already binds the complete original hash. Range
      // identity therefore needs no separate, publicly guessable name hash.
      V.ID = identity("bun-region", {Out.ID, Index, V.Kind, Offset, Size});
    }
    for (size_t I = 0; I < Out.Modules.size(); ++I) {
      auto &M = Out.Modules[I];
      const auto Index = std::to_string(I);
      M.ID = identity("bun-module", {Out.ID, Index});
      if (M.Loader <= 3)
        M.SourceArtifactID =
            identity("bun-text-projection", {M.ID, Out.Regions[M.Contents].ID,
                                             "strict-unicode-utf8-v1"});
    }
    return std::move(Out);
  }
};
} // namespace

BunExtraction extractBun(const Artifact &Input) {
  if (Input.Directory)
    throw Error("artifact_has_no_bytes");
  if (Input.Content.size() > Limits::HardMemberBytes)
    throw Error("bun_input_budget_exceeded");
  return GraphReader(Input, locate({Input.Content})).run();
}

std::vector<std::string> bunProfiles() {
  std::vector<std::string> Result;
  for (const auto Platform : {"linux", "macos", "windows"}) {
    const std::string_view P = Platform;
    const auto Format = P == "linux" ? "elf" : P == "macos" ? "macho" : "pe";
    for (const auto Architecture : {"x64", "arm64"})
      for (const bool Prelinked : {false, true})
        Result.push_back(profile(P, Architecture, Format, Prelinked));
  }
  return Result;
}

namespace {
BunSourceRange decodeSource(const BunExtraction &E, const BunModule &M,
                            uint64_t MaxBytes, std::optional<Range> Query) {
  if (M.SourceArtifactID.empty() || M.Contents >= E.Regions.size())
    throw Error("bun_source_unavailable");
  const auto &Content = E.Regions[M.Contents].Content;
  const auto Budget = std::min(MaxBytes, MaxBunDecodedSourceBytes);
  if (M.Encoding > 2)
    throw Error("bun_invalid_source_unicode");
  if (Query && (Query->Offset > Budget || Query->Size > Budget - Query->Offset))
    throw Error("invalid_source_position");
  if (Content.size() > Budget * (M.Encoding == 2 ? 2 : 1))
    throw Error("source_byte_budget_exceeded");
  // Raw storage stays in the immutable spool. Export can decode a large
  // module without lifting the separate parser or Blob materialization caps.
  std::string Chunk;
  uint64_t ChunkAt = 0, Cursor = 0;
  auto Byte = [&]() -> uint32_t {
    if (Cursor >= Content.size())
      throw Error("bun_invalid_source_unicode");
    if (Chunk.empty() || Cursor - ChunkAt == Chunk.size()) {
      ChunkAt = Cursor;
      Chunk = Content.read(
          Cursor, std::min(BlobTransferBytes, Content.size() - Cursor));
    }
    return uint8_t(Chunk[size_t(Cursor++ - ChunkAt)]);
  };
  BunSourceRange Projection;
  auto &Result = Projection.Text;
  std::optional<uint64_t> Begin, End;
  auto Mark = [&](uint64_t RawOffset) {
    if (!Query)
      return;
    if (Query->Offset == Result.size())
      Begin = RawOffset;
    if (Query->end() == Result.size())
      End = RawOffset;
  };
  if (!M.Encoding) {
    for (uint64_t At = 0; At < Content.size();) {
      const auto N = std::min(BlobTransferBytes, Content.size() - At);
      Result += Content.read(At, N);
      At += N;
    }
    if (!validUtf8(Result))
      throw Error("bun_invalid_source_unicode");
    if (Query) {
      Begin = Query->Offset;
      End = Query->end();
    }
  } else {
    auto Append = [&](uint32_t C) {
      const unsigned Size = C < 0x80 ? 1 : C < 0x800 ? 2 : C < 0x10000 ? 3 : 4;
      if (Size > Budget - Result.size())
        throw Error("source_byte_budget_exceeded");
      if (Size == 1) {
        Result.push_back(char(C));
      } else {
        Result.push_back(char((Size == 2   ? 0xc0
                               : Size == 3 ? 0xe0
                                           : 0xf0) |
                              (C >> (6 * (Size - 1)))));
        for (unsigned I = Size - 1; I; --I)
          Result.push_back(char(0x80 | ((C >> (6 * (I - 1))) & 0x3f)));
      }
    };
    while (Cursor < Content.size()) {
      Mark(Cursor);
      uint32_t C = Byte();
      if (M.Encoding == 2) {
        C |= Byte() << 8;
        if (C >= 0xd800 && C <= 0xdbff) {
          uint32_t Low = Byte();
          Low |= Byte() << 8;
          if (Low < 0xdc00 || Low > 0xdfff)
            throw Error("bun_invalid_source_unicode");
          C = 0x10000 + ((C - 0xd800) << 10) + (Low - 0xdc00);
        } else if (C >= 0xdc00 && C <= 0xdfff) {
          throw Error("bun_invalid_source_unicode");
        }
      }
      Append(C);
    }
    Mark(Content.size());
  }
  if (Query) {
    if (!Begin || !End || !sourceByteBoundary(Result, Query->Offset) ||
        !sourceByteBoundary(Result, Query->end()))
      throw Error("invalid_source_position");
    const auto Base = E.Regions[M.Contents].Offset;
    if (*End > std::numeric_limits<uint64_t>::max() - Base)
      throw Error("bun_range_out_of_bounds");
    Projection.Offset = Base + *Begin;
    Projection.Size = *End - *Begin;
  }
  return Projection;
}
} // namespace

std::string bunSourceBytes(const BunExtraction &E, const BunModule &M,
                           uint64_t MaxBytes) {
  return decodeSource(E, M, MaxBytes, std::nullopt).Text;
}

BunSourceRange bunSourceRange(const BunExtraction &E, const BunModule &M,
                              uint64_t Offset, uint64_t Length,
                              uint64_t MaxBytes) {
  return decodeSource(E, M, MaxBytes, Range{Offset, Length});
}
} // namespace neverd::web
