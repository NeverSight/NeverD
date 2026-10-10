//===- BunContainer.cpp - Bun native container locations ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bun native container locations.
///
//===----------------------------------------------------------------------===//

#include "BunContainer.h"

#include <algorithm>
#include <optional>

// Container writers: oven-sh/bun 744846f844374847c902b5e7fd59b4342a51ef99,
// src/exe_format/{elf,macho,pe}.rs. Independently implemented in C++.
// No target loader/runtime executes. See docs/web-bun-profile.md.
namespace neverd::web::bun_detail {
namespace {
bool powerOfTwo(uint64_t V) { return V && !(V & (V - 1)); }

struct Section {
  Range File;
  uint64_t Address = 0, Flags = 0, Alignment = 0;
  uint32_t Type = 0, Name = 0;
};

Container elf(const Reader &R) {
  if (R.Bytes.size() < 64)
    throw Error("bun_unsupported_container");
  const auto H = R.read({0, 64});
  if (H.compare(0, 4, "\177ELF") || uint8_t(H[4]) != 2 || H[5] != 1 ||
      H[6] != 1 || (H[7] != 0 && H[7] != 3) ||
      (number(H, 16, 2) != 2 && number(H, 16, 2) != 3) ||
      (number(H, 18, 2) != 62 && number(H, 18, 2) != 183) ||
      number(H, 20, 4) != 1 || number(H, 52, 2) != 64 ||
      number(H, 54, 2) != 56 || number(H, 58, 2) != 64)
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
  if (B.Address % (number(H, 18, 2) == 183 ? 65536 : 4096) ||
      Size != B.File.Size - 8 || Size < 48)
    throw Error("bun_invalid_graph_header");
  return {{B.File.Offset + 8, Size},
          "elf",
          "linux",
          number(H, 18, 2) == 183 ? "arm64" : "x64"};
}
uint64_t align(uint64_t Value, uint64_t Alignment) {
  within({Value, Alignment - 1}, UINT64_MAX);
  return (Value + Alignment - 1) & ~(Alignment - 1);
}

bool contains(Range Outer, Range Inner) {
  return Inner.Offset >= Outer.Offset && Inner.end() <= Outer.end();
}

bool fixedName(std::string_view Bytes, size_t At, size_t Width,
               std::string_view Name) {
  const auto Field = Bytes.substr(At, Width);
  return Field.size() == Width && Field.starts_with(Name) &&
         std::all_of(Field.begin() + Name.size(), Field.end(),
                     [](char C) { return C == 0; });
}

Range graph(const Reader &R, Range Section, Range Storage) {
  if (Section.Size < 8 || !contains(Storage, Section))
    throw Error("bun_invalid_section");
  const auto Size = R.integer(Section.Offset, 8);
  if (!Size && Section.Size == 8)
    throw Error("bun_not_standalone");
  if (Size < 48 || Size != Section.Size - 8)
    throw Error("bun_invalid_graph_header");
  // These writers zero page/file padding. Never treat padding as graph bytes.
  const Range Padding{Section.end(), Storage.end() - Section.end()};
  if (Padding.Size > 65536)
    throw Error("bun_invalid_section_padding");
  const auto Bytes = R.read(Padding);
  if (std::any_of(Bytes.begin(), Bytes.end(), [](char C) { return C != 0; }))
    throw Error("bun_invalid_section_padding");
  return {Section.Offset + 8, Size};
}

struct Mapping {
  Range File, Virtual;
};

// Validate the commands used by the pinned native templates before accepting
// their file references. They remain opaque metadata, never runtime actions.
void machoCommand(uint64_t Command, std::string_view C, uint64_t FileSize,
                  std::vector<Range> &References, unsigned &Platforms) {
  auto Exact = [&](uint64_t Size) {
    if (C.size() != Size)
      throw Error("bun_invalid_macho_command");
  };
  auto Reference = [&](uint64_t Offset, uint64_t Size) {
    within({Offset, Size}, FileSize);
    if (Size)
      References.push_back({Offset, Size});
  };
  auto Table = [&](uint64_t At, uint64_t Stride) {
    Reference(number(C, At, 4), number(C, At + 4, 4) * Stride);
  };
  auto String = [&](uint64_t HeaderSize) {
    if (C.size() < HeaderSize)
      throw Error("bun_invalid_macho_command");
    const auto Offset = number(C, 8, 4);
    if (Offset < HeaderSize || Offset >= C.size() ||
        C.find('\0', Offset) == std::string_view::npos)
      throw Error("bun_invalid_macho_command");
  };
  switch (Command) {
  case 0x2: // LC_SYMTAB: nlist_64 and string table
    Exact(24);
    Table(8, 16);
    Table(16, 1);
    break;
  case 0xb: // LC_DYSYMTAB
    Exact(80);
    Table(32, 8);
    Table(40, 56);
    Table(48, 4);
    Table(56, 4);
    Table(64, 8);
    Table(72, 8);
    break;
  case 0x22: // LC_DYLD_INFO[_ONLY]
  case 0x80000022:
    Exact(48);
    for (uint64_t At = 8; At < 48; At += 8)
      Table(At, 1);
    break;
  case 0x1d:       // LC_CODE_SIGNATURE
  case 0x1e:       // LC_SEGMENT_SPLIT_INFO
  case 0x26:       // LC_FUNCTION_STARTS
  case 0x29:       // LC_DATA_IN_CODE
  case 0x2b:       // LC_DYLIB_CODE_SIGN_DRS
  case 0x2e:       // LC_LINKER_OPTIMIZATION_HINT
  case 0x80000033: // LC_DYLD_EXPORTS_TRIE
  case 0x80000034: // LC_DYLD_CHAINED_FIXUPS
    Exact(16);
    Table(8, 1);
    break;
  case 0x16: // LC_TWOLEVEL_HINTS
    Exact(16);
    Table(8, 4);
    break;
  case 0x80000028: // LC_MAIN
    Exact(24);
    Reference(number(C, 8, 8), 1);
    break;
  case 0x2c: // LC_ENCRYPTION_INFO_64
    Exact(24);
    Table(8, 1);
    break;
  case 0x31: // LC_NOTE
    Exact(40);
    Reference(number(C, 24, 8), number(C, 32, 8));
    break;
  case 0x32: // LC_BUILD_VERSION
    if (C.size() < 24 || C.size() != 24 + number(C, 20, 4) * 8)
      throw Error("bun_invalid_macho_command");
    if (number(C, 8, 4) != 1 || ++Platforms != 1) // PLATFORM_MACOS
      throw Error("bun_unsupported_macho_platform");
    break;
  case 0x24: // LC_VERSION_MIN_MACOSX
    Exact(16);
    if (++Platforms != 1)
      throw Error("bun_unsupported_macho_platform");
    break;
  case 0x25: // LC_VERSION_MIN_IPHONEOS
  case 0x2f: // LC_VERSION_MIN_TVOS
  case 0x30: // LC_VERSION_MIN_WATCHOS
    throw Error("bun_unsupported_macho_platform");
  case 0x1b: // LC_UUID
    Exact(24);
    break;
  case 0x2a: // LC_SOURCE_VERSION
    Exact(16);
    break;
  case 0xc: // dylib_command
  case 0xd:
  case 0x80000018:
  case 0x8000001f:
  case 0x20:
  case 0x80000023:
    String(24);
    break;
  case 0xe: // dylinker_command / rpath_command
  case 0xf:
  case 0x8000001c:
  case 0x27:
    String(12);
    break;
  default:
    throw Error("bun_unsupported_macho_command");
  }
}

Container macho(const Reader &R) {
  const auto H = R.read({0, 32});
  const auto CPU = number(H, 4, 4), Subtype = number(H, 8, 4) & 0xffffff;
  const uint64_t Page = CPU == 0x100000c ? 16384 : 4096;
  if (number(H, 0, 4) != 0xfeedfacf || (CPU != 0x1000007 && CPU != 0x100000c) ||
      Subtype != (CPU == 0x1000007 ? 3U : 0U) || number(H, 12, 4) != 2 ||
      number(H, 28, 4))
    throw Error("bun_unsupported_container");
  const auto Count = number(H, 16, 4), Size = number(H, 20, 4);
  if (!Count || Count > 4096 || Size > 1024 * 1024 || Size < Count * 8)
    throw Error("bun_unsupported_macho_commands");
  const auto Commands = R.read({32, Size});
  const Range Headers{0, 32 + Size};
  std::vector<Mapping> Segments;
  std::vector<Range> References;
  std::optional<Mapping> Bun;
  Range BunSection;
  uint64_t At = 0, Sections = 0;
  unsigned Platforms = 0;
  for (uint64_t I = 0; I < Count; ++I) {
    within({At, 8}, Size);
    const auto Command = number(Commands, At, 4);
    const auto Length = number(Commands, At + 4, 4);
    if (Length < 8 || Length % 8)
      throw Error("bun_invalid_macho_command");
    within({At, Length}, Size);
    const auto C = std::string_view(Commands).substr(At, Length);
    At += Length;
    if (Command != 0x19) { // LC_SEGMENT_64
      machoCommand(Command, C, R.Bytes.size(), References, Platforms);
      continue;
    }
    if (Length < 72 || Segments.size() >= 1024)
      throw Error("bun_invalid_macho_segment");
    const auto N = number(C, 64, 4);
    if (N > 4096 - Sections || Length != 72 + N * 80)
      throw Error("bun_invalid_macho_sections");
    Sections += N;
    Mapping M{{number(C, 40, 8), number(C, 48, 8)},
              {number(C, 24, 8), number(C, 32, 8)}};
    within(M.File, R.Bytes.size());
    within(M.Virtual, UINT64_MAX);
    if (M.File.Size > M.Virtual.Size)
      throw Error("bun_invalid_macho_segment");
    const bool IsBun = fixedName(C, 8, 16, "__BUN");
    if (IsBun) {
      if (Bun)
        throw Error("bun_duplicate_section");
      if (N != 1 || number(C, 56, 4) != 3 || number(C, 60, 4) != 3 ||
          number(C, 68, 4) || M.File.Offset % Page || M.Virtual.Offset % Page ||
          M.File.Size != M.Virtual.Size || overlaps(M.File, Headers))
        throw Error("bun_invalid_macho_segment");
      Bun = M;
    }
    for (uint64_t J = 0; J < N; ++J) {
      const auto S = C.substr(72 + J * 80, 80);
      const Range V{number(S, 32, 8), number(S, 40, 8)};
      const Range F{number(S, 48, 4), V.Size};
      const Range Relocations{number(S, 56, 4), number(S, 60, 4) * 8};
      within(V, UINT64_MAX);
      within(Relocations, R.Bytes.size());
      if (Relocations.Size)
        References.push_back(Relocations);
      const auto Flags = number(S, 64, 4), Alignment = number(S, 52, 4);
      if (Alignment > 31 || !contains(M.Virtual, V))
        throw Error("bun_invalid_macho_section");
      const bool ZeroFill =
          (Flags & 255) == 1 || (Flags & 255) == 12 || (Flags & 255) == 18;
      if (!ZeroFill) {
        within(F, R.Bytes.size());
        if (!contains(M.File, F) ||
            F.Offset - M.File.Offset != V.Offset - M.Virtual.Offset)
          throw Error("bun_ambiguous_load_mapping");
      }
      if (IsBun) {
        if (!fixedName(S, 0, 16, "__bun") || !fixedName(S, 16, 16, "__BUN") ||
            F.Offset != M.File.Offset || V.Offset != M.Virtual.Offset ||
            Flags != 0x10000000 || Alignment != 14 || number(S, 56, 4) ||
            number(S, 60, 4) || number(S, 68, 4) || number(S, 72, 4) ||
            number(S, 76, 4) || align(F.Size, 16384) != M.File.Size)
          throw Error("bun_invalid_macho_section");
        BunSection = F;
      } else if (fixedName(S, 0, 16, "__bun")) {
        throw Error("bun_ambiguous_load_mapping");
      }
    }
    Segments.push_back(M);
  }
  if (At != Size)
    throw Error("bun_invalid_macho_command");
  if (!Bun)
    throw Error("bun_section_not_found");
  unsigned Owners = 0;
  for (const auto &M : Segments)
    if (overlaps(M.File, Bun->File) || overlaps(M.Virtual, Bun->Virtual))
      ++Owners;
  if (Owners != 1)
    throw Error("bun_ambiguous_load_mapping");
  for (const auto &Ref : References)
    if (overlaps(Ref, Bun->File))
      throw Error("bun_ambiguous_load_mapping");
  if (Platforms != 1)
    throw Error("bun_unsupported_macho_platform");
  return {graph(R, BunSection, Bun->File), "macho", "macos",
          CPU == 0x100000c ? "arm64" : "x64"};
}

Container pe(const Reader &R) {
  const auto DOS = R.read({0, 64});
  const auto PE = number(DOS, 60, 4);
  if (DOS.substr(0, 2) != "MZ" || PE < 64)
    throw Error("bun_unsupported_container");
  const auto H = R.read({PE, 24});
  const auto CPU = number(H, 4, 2), Count = number(H, 6, 2);
  const auto OptSize = number(H, 20, 2), Flags = number(H, 22, 2);
  if (number(H, 0, 4) != 0x4550 || (CPU != 0x8664 && CPU != 0xaa64) ||
      !(Flags & 2) || (Flags & 0x2000) || OptSize < 240 || OptSize > 4096)
    throw Error("bun_unsupported_container");
  if (!Count || Count > 96)
    throw Error("bun_unsupported_pe_sections");
  const auto O = R.read({PE + 24, OptSize});
  if (number(O, 0, 2) != 0x20b)
    throw Error("bun_unsupported_container");
  const auto SA = number(O, 32, 4), FA = number(O, 36, 4);
  const auto ImageSize = number(O, 56, 4), HeaderSize = number(O, 60, 4);
  const auto Directories = number(O, 108, 4);
  if (!powerOfTwo(FA) || FA < 512 || FA > 65536 || !powerOfTwo(SA) ||
      SA < 4096 || SA < FA || !ImageSize || ImageSize % SA || HeaderSize % FA ||
      Directories > 16 || HeaderSize < PE + 24 + OptSize + Count * 40 ||
      HeaderSize > ImageSize)
    throw Error("bun_invalid_pe_headers");
  within({0, HeaderSize}, R.Bytes.size());
  const auto Table = R.read({PE + 24 + OptSize, Count * 40});
  std::vector<Mapping> Sections;
  std::optional<Mapping> Bun;
  Range BunSection;
  for (uint64_t I = 0; I < Count; ++I) {
    const auto S = std::string_view(Table).substr(I * 40, 40);
    const auto VS = number(S, 8, 4);
    Mapping M{{number(S, 20, 4), number(S, 16, 4)},
              {number(S, 12, 4), align(std::max(VS, number(S, 16, 4)), SA)}};
    within(M.File, R.Bytes.size());
    within(M.Virtual, ImageSize);
    if (M.Virtual.Offset % SA || M.Virtual.Offset < HeaderSize ||
        (M.File.Size && (M.File.Offset < HeaderSize || M.File.Offset % FA ||
                         M.File.Size % FA)))
      throw Error("bun_invalid_pe_section");
    if (fixedName(S, 0, 8, ".bun")) {
      if (Bun)
        throw Error("bun_duplicate_section");
      if (number(S, 36, 4) != 0x40000040 || number(S, 24, 4) ||
          number(S, 28, 4) || number(S, 32, 4) || VS < 8 ||
          align(VS, FA) != M.File.Size)
        throw Error("bun_invalid_pe_section");
      Bun = M;
      BunSection = {M.File.Offset, VS};
    }
    Sections.push_back(M);
  }
  if (!Bun)
    throw Error("bun_section_not_found");
  unsigned Owners = 0;
  for (const auto &M : Sections)
    if (overlaps(M.File, Bun->File) || overlaps(M.Virtual, Bun->Virtual))
      ++Owners;
  if (Owners != 1)
    throw Error("bun_ambiguous_load_mapping");
  for (uint64_t I = 0; I < Directories; ++I) {
    const Range D{number(O, 112 + I * 8, 4), number(O, 116 + I * 8, 4)};
    // The security directory uses a file offset; other directories use RVAs.
    within(D, I == 4 ? R.Bytes.size() : ImageSize);
    if (overlaps(D, I == 4 ? Bun->File : Bun->Virtual))
      throw Error("bun_ambiguous_load_mapping");
  }
  return {graph(R, BunSection, Bun->File), "pe", "windows",
          CPU == 0xaa64 ? "arm64" : "x64"};
}
} // namespace

Container locate(const Reader &R) {
  if (R.Bytes.size() < 64)
    throw Error("bun_unsupported_container");
  const auto Magic = R.integer(0, 4);
  if (Magic == 0x464c457f)
    return elf(R);
  if (Magic == 0xfeedfacf)
    return macho(R);
  if ((Magic & 0xffff) == 0x5a4d)
    return pe(R);
  // No trailer scans, guessed offsets, universal-slice choice or runtime load.
  throw Error("bun_unsupported_container");
}
} // namespace neverd::web::bun_detail
