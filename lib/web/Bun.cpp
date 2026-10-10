#include "neverd/web/Bun.h"

#include "neverd/web/Limits.h"
#include "neverd/web/Session.h"
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
struct Range {
  uint64_t Offset = 0, Size = 0;
  uint64_t end() const { return Offset + Size; }
};

void within(Range R, uint64_t Size) {
  if (R.Offset > Size || R.Size > Size - R.Offset)
    throw Error("bun_range_out_of_bounds");
}

bool overlaps(Range A, Range B) {
  return A.Size && B.Size && A.Offset < B.end() && B.Offset < A.end();
}

uint64_t number(std::string_view Bytes, size_t Offset, size_t Width) {
  within({Offset, Width}, Bytes.size());
  uint64_t Value = 0;
  for (size_t I = 0; I < Width; ++I)
    Value |= uint64_t(uint8_t(Bytes[Offset + I])) << (8 * I);
  return Value;
}

struct Reader {
  Blob Bytes;
  std::string read(Range R) const {
    within(R, Bytes.size());
    return Bytes.read(R.Offset, R.Size);
  }
  uint64_t integer(uint64_t Offset, size_t Width) const {
    return number(read({Offset, Width}), 0, Width);
  }
};

bool powerOfTwo(uint64_t V) { return V && !(V & (V - 1)); }

struct Section {
  Range File;
  uint64_t Address = 0, Flags = 0, Alignment = 0;
  uint32_t Type = 0, Name = 0;
};

Range locate(const Reader &R) {
  if (R.Bytes.size() < 64)
    throw Error("bun_unsupported_container");
  const auto H = R.read({0, 64});
  if (H.compare(0, 4, "\177ELF") || uint8_t(H[4]) != 2 || H[5] != 1 ||
      H[6] != 1 || (H[7] != 0 && H[7] != 3) ||
      (number(H, 16, 2) != 2 && number(H, 16, 2) != 3) ||
      number(H, 18, 2) != 62 || number(H, 20, 4) != 1 ||
      number(H, 52, 2) != 64 || number(H, 54, 2) != 56 ||
      number(H, 58, 2) != 64)
    throw Error("bun_unsupported_container");
  const auto PN = number(H, 56, 2), SN = number(H, 60, 2);
  const auto NamesIndex = number(H, 62, 2);
  if (!PN || PN > 1024 || !SN || SN > 4096 || !NamesIndex || NamesIndex >= SN)
    throw Error("bun_unsupported_elf_tables");
  const Range PH{number(H, 32, 8), PN * 56};
  const Range SH{number(H, 40, 8), SN * 64};
  const auto P = R.read(PH), S = R.read(SH);
  if (overlaps(PH, SH) || overlaps(PH, {0, 64}) || overlaps(SH, {0, 64}))
    throw Error("bun_overlapping_elf_tables");
  std::vector<Section> Sections;
  for (uint64_t I = 0; I < SN; ++I) {
    const auto At = I * 64;
    Section V{{number(S, At + 24, 8), number(S, At + 32, 8)},
              number(S, At + 16, 8),
              number(S, At + 8, 8),
              number(S, At + 48, 8),
              uint32_t(number(S, At + 4, 4)),
              uint32_t(number(S, At, 4))};
    if (V.Type != 8)
      within(V.File, R.Bytes.size());
    if (V.Alignment > 1 && !powerOfTwo(V.Alignment))
      throw Error("bun_invalid_elf_alignment");
    Sections.push_back(V);
  }
  const auto &NS = Sections[NamesIndex];
  if (NS.Type != 3 || !NS.File.Size || NS.File.Size > MaxBunNameBytes)
    throw Error("bun_invalid_section_names");
  const auto Names = R.read(NS.File);
  if (Names.front() || Names.back())
    throw Error("bun_invalid_section_names");
  size_t Found = Sections.size();
  for (size_t I = 0; I < Sections.size(); ++I) {
    const auto Off = Sections[I].Name;
    if (Off >= Names.size())
      throw Error("bun_invalid_section_names");
    // Only compare the fixed candidate; no unbounded per-section strlen.
    if (std::string_view(Names).substr(Off, 5) ==
        std::string_view(".bun\0", 5)) {
      if (Found != Sections.size())
        throw Error("bun_duplicate_section");
      Found = I;
    }
  }
  if (Found == Sections.size())
    throw Error("bun_section_not_found");
  const auto &B = Sections[Found];
  if (B.Type != 1 || B.Flags != 3 || B.File.Size < 8 || overlaps(B.File, PH) ||
      overlaps(B.File, SH) || overlaps(B.File, {0, 64}))
    throw Error("bun_invalid_section");
  for (size_t I = 0; I < Sections.size(); ++I)
    if (I != Found && Sections[I].Type != 8 &&
        overlaps(B.File, Sections[I].File))
      throw Error("bun_overlapping_section");
  unsigned Owners = 0;
  within({B.Address, B.File.Size}, UINT64_MAX);
  for (uint64_t I = 0; I < PN; ++I) {
    const auto At = I * 56;
    const Range F{number(P, At + 8, 8), number(P, At + 32, 8)};
    const Range V{number(P, At + 16, 8), number(P, At + 40, 8)};
    const auto Alignment = number(P, At + 48, 8);
    within(F, R.Bytes.size());
    within(V, UINT64_MAX);
    if (number(P, At, 4) != 1)
      continue;
    if (F.Size > V.Size ||
        (Alignment > 1 && (!powerOfTwo(Alignment) ||
                           F.Offset % Alignment != V.Offset % Alignment)))
      throw Error("bun_invalid_load_segment");
    const bool Physical = overlaps(F, B.File);
    const bool Virtual = overlaps(V, {B.Address, B.File.Size});
    if (!Physical && !Virtual)
      continue;
    if (!Physical || !Virtual || number(P, At + 4, 4) != 6 ||
        F.Offset > B.File.Offset || B.File.end() > F.end() ||
        V.Offset > B.Address || B.Address + B.File.Size > V.end() ||
        B.File.Offset - F.Offset != B.Address - V.Offset || F.Size != V.Size)
      throw Error("bun_ambiguous_load_mapping");
    ++Owners;
  }
  if (Owners != 1)
    throw Error("bun_ambiguous_load_mapping");
  const auto Size = R.integer(B.File.Offset, 8);
  if (!Size && B.File.Size == 8)
    throw Error("bun_not_standalone");
  if (B.Address % 4096 || Size != B.File.Size - 8 || Size < 48)
    throw Error("bun_invalid_graph_header");
  return {B.File.Offset + 8, Size};
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
          (!Text.starts_with("/$bunfs/") || !Names.insert(Text).second))
        throw Error("bun_invalid_module_name");
    }
    return Index;
  }

public:
  GraphReader(const Artifact &Input, Range Graph)
      : Input(Input), R{Input.Content.slice(Graph.Offset, Graph.Size)} {
    Out.ArtifactID = Input.ID;
    Out.GraphOffset = Graph.Offset;
    Out.GraphSize = Graph.Size;
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
    Out.Profile = (Out.Flags & ((1 << 11) | (1 << 12))) ? BunPrelinkedProfile
                                                        : BunProfile;
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
