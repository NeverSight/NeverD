//===- MachOSourceDependencies.cpp - Source patch support ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Install explicit dynamic library imports.
//===----------------------------------------------------------------------===//

#include "MachOSourceDependencies.h"

#include "MachODependencyState.h"
#include "MachOStrictLayout.h"

#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"

#include "llvm/Object/MachO.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/LEB128.h"

#include <map>

namespace neverd {
namespace {
using namespace llvm::MachO;
#define MACHO_DEPENDENCIES_TEXT(Name, Text) constexpr char Name[] = Text;
#include "MachOSourceDependencies.def"
llvm::Error fail(const llvm::Twine &S) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "Mach-O dependency: " + S);
}
void uleb(std::vector<uint8_t> &B, uint64_t N) {
  uint8_t Bytes[16];
  auto Size = llvm::encodeULEB128(N, Bytes);
  B.insert(B.end(), Bytes, Bytes + Size);
}
void word(std::vector<uint8_t> &B, uint32_t N) {
  auto At = B.size();
  B.resize(At + 4);
  writeLE<uint32_t>(B.data() + At, N);
}
std::string nativeName(llvm::StringRef Symbol) { return "_" + Symbol.str(); }

// Locate the ordinary bind stream's real opcode terminator. NUL bytes in
// symbols and LEB operands are not instruction boundaries.
llvm::Expected<size_t> bindEnd(llvm::ArrayRef<uint8_t> B, unsigned Linkedit) {
  if (B.empty())
    return size_t(0);
  size_t At = 0;
  auto leb = [&](bool Signed) -> bool {
    if (At >= B.size())
      return false;
    unsigned N = 0;
    const char *Error = nullptr;
    if (Signed)
      llvm::decodeSLEB128(B.data() + At, &N, B.end(), &Error);
    else
      llvm::decodeULEB128(B.data() + At, &N, B.end(), &Error);
    At += N;
    return !Error && N;
  };
  while (At < B.size()) {
    const size_t Start = At;
    const auto Byte = B[At++];
    const auto Op = Byte & BIND_OPCODE_MASK, Imm = Byte & BIND_IMMEDIATE_MASK;
    switch (Op) {
    case BIND_OPCODE_DONE:
      if (Byte ||
          llvm::any_of(B.drop_front(At), [](uint8_t V) { return V != 0; }))
        return fail("noncanonical bind stream terminator");
      return Start;
    case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
    case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
    case BIND_OPCODE_SET_TYPE_IMM:
    case BIND_OPCODE_DO_BIND:
    case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
      break;
    case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: {
      auto End = std::find(B.begin() + At, B.end(), 0);
      if (End == B.end())
        return fail("unterminated dyld symbol");
      At = End - B.begin() + 1;
      break;
    }
    case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
      if (Imm >= Linkedit)
        return fail("bind stream refers to relocatable LINKEDIT");
      [[fallthrough]];
    case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
    case BIND_OPCODE_ADD_ADDR_ULEB:
    case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
      if (!leb(false))
        return fail("invalid bind ULEB operand");
      break;
    case BIND_OPCODE_SET_ADDEND_SLEB:
      if (!leb(true))
        return fail("invalid bind SLEB operand");
      break;
    case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
      if (!leb(false) || !leb(false))
        return fail("invalid bind repetition");
      break;
    default:
      return fail("unmodeled bind opcode");
    }
  }
  return fail("ordinary bind stream has no terminator");
}
struct Layout {
  MachOHeaderInfo H;
  uint64_t Linkedit = 0, Bind = 0, Chained = 0, Slots = 0, Symtab = 0,
           Dysymtab = 0;
  unsigned SegmentCount = 0, LinkeditIndex = 0, SlotIndex = 0;
};
llvm::Expected<Layout> layout(llvm::ArrayRef<uint8_t> B) {
  Layout L;
  if (!macho_patch_detail::validateLoadCommandRegion(B, L.H))
    return fail("invalid commands");
  std::vector<macho_patch_detail::MachOFileRange> Segments, Sections;
  if (!macho_patch_detail::collectMachOFileRanges(B, Segments, Sections))
    return fail("ambiguous layout");
  bool Bad = false;
  forEachMachOLoadCommand(
      B.data(), B.size(),
      [&](const uint8_t *C, uint32_t ID, uint32_t, bool Is64) {
        if (ID == LC_SYMTAB) {
          Bad |= L.Symtab != 0;
          L.Symtab = C - B.data();
        }
        if (ID == LC_DYSYMTAB) {
          Bad |= L.Dysymtab != 0;
          L.Dysymtab = C - B.data();
        }
        if (ID == LC_DYLD_INFO || ID == LC_DYLD_INFO_ONLY) {
          Bad |= L.Bind != 0;
          L.Bind = C - B.data();
        }
        if (ID == LC_DYLD_CHAINED_FIXUPS) {
          Bad |= L.Chained != 0;
          L.Chained = C - B.data();
        }
        if (ID != getMachOSegmentCmdID(Is64))
          return;
        const auto F = readMachOSegment(C, Is64);
        if (readMachOName(F.SegName) == "__LINKEDIT") {
          Bad |= L.Linkedit != 0;
          L.Linkedit = C - B.data();
          L.LinkeditIndex = L.SegmentCount;
        }
        if (readMachOName(F.SegName) == "__NDIMPD") {
          Bad |= L.Slots != 0;
          L.Slots = C - B.data();
          L.SlotIndex = L.SegmentCount;
        }
        ++L.SegmentCount;
      });
  if (Bad || !L.Symtab || !L.Linkedit ||
      L.LinkeditIndex + 1 != L.SegmentCount || (L.Bind && L.Chained))
    return fail("ambiguous dyld metadata or nonterminal LINKEDIT");
  const auto LE = readMachOSegment(B.data() + L.Linkedit, L.H.Is64);
  if (LE.NSects || LE.FileOff + LE.FileSize != B.size())
    return fail("unmodeled LINKEDIT ownership");
  return L;
}
llvm::Error appendCommands(std::vector<uint8_t> &B,
                           llvm::ArrayRef<std::vector<uint8_t>> Commands) {
  if (Commands.empty())
    return llvm::Error::success();
  auto H = parseMachOHeader(B.data(), B.size());
  uint64_t Size = 0;
  for (const auto &C : Commands)
    Size += C.size();
  const uint64_t End = uint64_t(H.HeaderSize) + H.SizeOfCmds;
  const uint64_t First = findFirstMachODataOffset(B.data(), B.size());
  if (Size > UINT32_MAX - H.SizeOfCmds ||
      Commands.size() > UINT32_MAX - H.NCmds || End + Size > First ||
      !rangeInBounds(End, Size, B.size()) ||
      llvm::any_of(llvm::ArrayRef<uint8_t>(B).slice(End, Size),
                   [](uint8_t V) { return V != 0; }))
    return fail("new dylibs require authenticated load-command storage");
  uint64_t At = End;
  for (const auto &C : Commands) {
    std::copy(C.begin(), C.end(), B.begin() + At);
    At += C.size();
  }
  setMachOHeaderCmds(B.data(), H.Is64, H.NCmds + Commands.size(),
                     H.SizeOfCmds + Size);
  return llvm::Error::success();
}

// Give the loader-owned cells native section and indirect-symbol identities.
// Append undefined symbols after the old table so existing indexes stay valid.
llvm::Error registerSlots(std::vector<uint8_t> &B, const Layout &L,
                          llvm::ArrayRef<SourcePatchImport> Imports,
                          const std::map<std::string, uint32_t> &Libraries) {
  if (!L.Dysymtab || !L.Slots)
    return fail("missing slot symbol registration commands");
  auto T = *reinterpret_cast<const symtab_command *>(B.data() + L.Symtab);
  auto D = *reinterpret_cast<const dysymtab_command *>(B.data() + L.Dysymtab);
  const unsigned NS = L.H.Is64 ? sizeof(nlist_64) : sizeof(nlist);
  const unsigned SS = getMachOSectionSize(L.H.Is64);
  const auto S = readMachOSegment(B.data() + L.Slots, L.H.Is64);
  const auto LE = readMachOSegment(B.data() + L.Linkedit, L.H.Is64);
  auto InLE = [&](uint64_t Off, uint64_t Size) {
    return Off >= LE.FileOff &&
           rangeInBounds(Off - LE.FileOff, Size, LE.FileSize);
  };
  if (!InLE(T.symoff, uint64_t(T.nsyms) * NS) || !InLE(T.stroff, T.strsize) ||
      (D.nindirectsyms &&
       !InLE(D.indirectsymoff, uint64_t(D.nindirectsyms) * 4)) ||
      D.ilocalsym || D.iextdefsym != D.nlocalsym ||
      uint64_t(D.iextdefsym) + D.nextdefsym != D.iundefsym ||
      uint64_t(D.iundefsym) + D.nundefsym != T.nsyms ||
      T.nsyms > UINT32_MAX - Imports.size() ||
      D.nindirectsyms > UINT32_MAX - Imports.size())
    return fail(
        "symbol ranges cannot be extended without changing old indexes");
  for (unsigned I = 0; I < T.nsyms; ++I) {
    const auto *N = B.data() + T.symoff + uint64_t(I) * NS;
    const unsigned Name = readLE<uint32_t>(N);
    if (Name >= T.strsize ||
        !std::memchr(B.data() + T.stroff + Name, 0, T.strsize - Name) ||
        (I >= D.iundefsym &&
         ((N[4] & (N_STAB | N_TYPE | N_EXT)) != (N_UNDF | N_EXT))))
      return fail("invalid original symbol identity");
  }
  std::vector<uint8_t> Symbols(B.begin() + T.symoff,
                               B.begin() + T.symoff + uint64_t(T.nsyms) * NS);
  std::vector<uint8_t> Names(B.begin() + T.stroff,
                             B.begin() + T.stroff + T.strsize);
  std::vector<uint8_t> Indirect;
  if (D.nindirectsyms)
    Indirect.assign(B.begin() + D.indirectsymoff,
                    B.begin() + D.indirectsymoff +
                        uint64_t(D.nindirectsyms) * 4);
  for (size_t I = 0; I < Imports.size(); ++I) {
    const auto Ordinal = Libraries.at(Imports[I].Request.Library);
    if (Ordinal >= MAX_LIBRARY_ORDINAL || Names.size() > UINT32_MAX)
      return fail("symbol library ordinal exceeds native nlist encoding");
    const auto At = Symbols.size();
    Symbols.resize(At + NS, 0);
    writeLE<uint32_t>(Symbols.data() + At, Names.size());
    Symbols[At + 4] = N_UNDF | N_EXT;
    writeLE<uint16_t>(Symbols.data() + At + 6, Ordinal << 8);
    const auto Name = nativeName(Imports[I].Request.Symbol);
    Names.insert(Names.end(), Name.begin(), Name.end());
    Names.push_back(0);
    word(Indirect, T.nsyms + I);
  }
  const uint64_t End = uint64_t(L.H.HeaderSize) + L.H.SizeOfCmds;
  if (End + SS > findFirstMachODataOffset(B.data(), B.size()) ||
      !rangeInBounds(End, SS, B.size()) ||
      llvm::any_of(llvm::ArrayRef<uint8_t>(B).slice(End, SS),
                   [](uint8_t V) { return V != 0; }))
    return fail("import section needs authenticated load-command storage");
  const uint64_t Insert = L.Slots + getMachOSegmentCmdSize(L.H.Is64);
  std::memmove(B.data() + Insert + SS, B.data() + Insert, End - Insert);
  std::memset(B.data() + Insert, 0, SS);
  if (L.H.Is64) {
    auto *G = reinterpret_cast<segment_command_64 *>(B.data() + L.Slots);
    G->cmdsize += SS;
    G->nsects = 1;
    auto *Section = reinterpret_cast<section_64 *>(B.data() + Insert);
    std::memcpy(Section->sectname, "__nl_symbol_ptr", 15);
    std::memcpy(Section->segname, "__NDIMPD", 8);
    Section->addr = S.VMAddr;
    Section->offset = S.FileOff;
    Section->size = Imports.size() * 8;
    Section->align = 3;
    Section->flags = S_NON_LAZY_SYMBOL_POINTERS;
    Section->reserved1 = D.nindirectsyms;
  } else {
    auto *G = reinterpret_cast<segment_command *>(B.data() + L.Slots);
    G->cmdsize += SS;
    G->nsects = 1;
    auto *Section = reinterpret_cast<section *>(B.data() + Insert);
    std::memcpy(Section->sectname, "__nl_symbol_ptr", 15);
    std::memcpy(Section->segname, "__NDIMPD", 8);
    Section->addr = S.VMAddr;
    Section->offset = S.FileOff;
    Section->size = Imports.size() * 4;
    Section->align = 2;
    Section->flags = S_NON_LAZY_SYMBOL_POINTERS;
    Section->reserved1 = D.nindirectsyms;
  }
  setMachOHeaderCmds(B.data(), L.H.Is64, L.H.NCmds, L.H.SizeOfCmds + SS);
  auto Append = [&](const std::vector<uint8_t> &Bytes) {
    B.resize(llvm::alignTo(B.size(), 8), 0);
    const uint64_t Offset = B.size();
    B.insert(B.end(), Bytes.begin(), Bytes.end());
    return Offset;
  };
  const auto SymbolOffset = Append(Symbols), NameOffset = Append(Names),
             IndirectOffset = Append(Indirect);
  if (B.size() > UINT32_MAX)
    return fail("extended symbol metadata exceeds file limits");
  T.symoff = SymbolOffset;
  T.nsyms += Imports.size();
  T.stroff = NameOffset;
  T.strsize = Names.size();
  D.nundefsym += Imports.size();
  D.indirectsymoff = IndirectOffset;
  D.nindirectsyms += Imports.size();
  std::memcpy(B.data() + L.Symtab + (L.Symtab >= Insert ? SS : 0), &T,
              sizeof(T));
  std::memcpy(B.data() + L.Dysymtab + (L.Dysymtab >= Insert ? SS : 0), &D,
              sizeof(D));
  auto NewLE = LE;
  NewLE.FileSize = B.size() - LE.FileOff;
  NewLE.VMSize = llvm::alignTo(NewLE.FileSize, 16384);
  writeMachOSegment(B.data() + L.Linkedit + SS, L.H.Is64, NewLE);
  return llvm::Error::success();
}

llvm::Expected<std::vector<uint8_t>>
chained(std::vector<uint8_t> &B, const Layout &L, const BinaryImage &Original,
        llvm::ArrayRef<SourcePatchImport> Imports,
        const std::map<std::string, uint32_t> &Libraries) {
  if (!L.H.Is64)
    return fail("32-bit chained fixups are not modeled");
  const auto *C =
      reinterpret_cast<const linkedit_data_command *>(B.data() + L.Chained);
  if (!rangeInBounds(C->dataoff, C->datasize, B.size()) ||
      C->datasize < sizeof(dyld_chained_fixups_header))
    return fail("truncated chained fixups");
  const auto Old = llvm::ArrayRef<uint8_t>(B).slice(C->dataoff, C->datasize);
  dyld_chained_fixups_header H{};
  std::memcpy(&H, Old.data(), sizeof(H));
  const unsigned ES = H.imports_format == DYLD_CHAINED_IMPORT            ? 4
                      : H.imports_format == DYLD_CHAINED_IMPORT_ADDEND   ? 8
                      : H.imports_format == DYLD_CHAINED_IMPORT_ADDEND64 ? 16
                                                                         : 0;
  if (H.fixups_version || H.symbols_format || !ES ||
      H.imports_count > 0xffffff - Imports.size() ||
      H.starts_offset < sizeof(H) ||
      !rangeInBounds(H.starts_offset, 4, Old.size()) ||
      H.imports_offset < sizeof(H) ||
      !rangeInBounds(H.imports_offset, uint64_t(H.imports_count) * ES,
                     Old.size()) ||
      H.symbols_offset < H.imports_offset + uint64_t(H.imports_count) * ES ||
      H.symbols_offset > Old.size())
    return fail("unsupported chained header");
  const unsigned OldSegments = readLE<uint32_t>(Old.data() + H.starts_offset);
  if (OldSegments >= L.SegmentCount ||
      !rangeInBounds(H.starts_offset, 4 + uint64_t(OldSegments) * 4,
                     H.imports_offset))
    return fail("chained segment identity changed");
  std::vector<uint8_t> Starts(4 + L.SegmentCount * 4, 0);
  writeLE<uint32_t>(Starts.data(), L.SegmentCount);
  unsigned PointerFormat = 0;
  for (unsigned N = 0; N < OldSegments; ++N) {
    const unsigned Off =
        readLE<uint32_t>(Old.data() + H.starts_offset + 4 + N * 4);
    if (!Off)
      continue;
    if (N >= L.SlotIndex || Off < 4 + OldSegments * 4 ||
        !rangeInBounds(uint64_t(H.starts_offset) + Off, 22, H.imports_offset))
      return fail("chained start has no stable segment owner");
    const auto *S = Old.data() + H.starts_offset + Off;
    const uint32_t Size = readLE<uint32_t>(S);
    const unsigned Page = readLE<uint16_t>(S + 4),
                   Format = readLE<uint16_t>(S + 6),
                   Count = readLE<uint16_t>(S + 20);
    if ((Size != 22 + Count * 2 && Size != llvm::alignTo(22 + Count * 2, 8)) ||
        (Page != 4096 && Page != 16384) ||
        (Format != DYLD_CHAINED_PTR_64 &&
         Format != DYLD_CHAINED_PTR_64_OFFSET) ||
        !rangeInBounds(uint64_t(H.starts_offset) + Off, Size, H.imports_offset))
      return fail("unmodeled chained start encoding");
    if (PointerFormat && PointerFormat != Format)
      return fail("mixed chained pointer formats");
    PointerFormat = Format;
    for (unsigned P = 0; P < Count; ++P) {
      const auto V = readLE<uint16_t>(S + 22 + P * 2);
      if (V != DYLD_CHAINED_PTR_START_NONE &&
          (V >= Page || (V & DYLD_CHAINED_PTR_START_MULTI)))
        return fail("unmodeled chained page start");
    }
    Starts.resize(llvm::alignTo(Starts.size(), 8), 0);
    writeLE<uint32_t>(Starts.data() + 4 + N * 4, Starts.size());
    Starts.insert(Starts.end(), S, S + Size);
  }
  const auto Slots = readMachOSegment(B.data() + L.Slots, true);
  const unsigned Page = Original.Arch == Arch::AArch64 ? 16384 : 4096;
  const unsigned Count = (Imports.size() * 8 + Page - 1) / Page;
  Starts.resize(llvm::alignTo(Starts.size(), 8), 0);
  writeLE<uint32_t>(Starts.data() + 4 + L.SlotIndex * 4, Starts.size());
  const size_t At = Starts.size();
  Starts.resize(At + llvm::alignTo(22 + Count * 2, 8), 0);
  writeLE<uint32_t>(Starts.data() + At, llvm::alignTo(22 + Count * 2, 8));
  writeLE<uint16_t>(Starts.data() + At + 4, Page);
  writeLE<uint16_t>(Starts.data() + At + 6,
                    PointerFormat ? PointerFormat : DYLD_CHAINED_PTR_64_OFFSET);
  uint64_t HeaderVA = 0;
  forEachMachOLoadCommand(
      B.data(), B.size(),
      [&](const uint8_t *C, uint32_t ID, uint32_t, bool Is64) {
        if (ID != getMachOSegmentCmdID(Is64))
          return;
        const auto S = readMachOSegment(C, Is64);
        if (!S.FileOff && S.FileSize)
          HeaderVA = S.VMAddr;
      });
  writeLE<uint64_t>(Starts.data() + At + 8, Slots.VMAddr - HeaderVA);
  writeLE<uint16_t>(Starts.data() + At + 20, Count);
  std::vector<uint8_t> Names(Old.begin() + H.symbols_offset, Old.end());
  std::vector<uint8_t> Table(Old.begin() + H.imports_offset,
                             Old.begin() + H.imports_offset +
                                 uint64_t(H.imports_count) * ES);
  for (size_t I = 0; I < Imports.size(); ++I) {
    const auto &Import = Imports[I];
    const auto Library = Libraries.at(Import.Request.Library);
    const auto Name = nativeName(Import.Request.Symbol);
    if (Library > 127 || Names.size() >= (1u << 23))
      return fail("chained import name or library limit");
    const size_t Entry = Table.size();
    Table.resize(Entry + ES, 0);
    if (ES == 16)
      writeLE<uint64_t>(Table.data() + Entry,
                        uint64_t(Library) | (uint64_t(Names.size()) << 32));
    else
      writeLE<uint32_t>(Table.data() + Entry,
                        Library | (uint32_t(Names.size()) << 9));
    Names.insert(Names.end(), Name.begin(), Name.end());
    Names.push_back(0);
    const bool Next = I + 1 != Imports.size() && ((I + 1) * 8) % Page;
    const uint64_t Pointer = uint64_t(1) << 63 |
                             (uint64_t(Next ? 2 : 0) << 51) |
                             (H.imports_count + I);
    writeLE<uint64_t>(B.data() + Slots.FileOff + I * 8, Pointer);
  }
  std::vector<uint8_t> Out(sizeof(H), 0);
  H.starts_offset = Out.size();
  Out.insert(Out.end(), Starts.begin(), Starts.end());
  Out.resize(llvm::alignTo(Out.size(), 8), 0);
  H.imports_offset = Out.size();
  Out.insert(Out.end(), Table.begin(), Table.end());
  H.symbols_offset = Out.size();
  Out.insert(Out.end(), Names.begin(), Names.end());
  H.imports_count += Imports.size();
  std::memcpy(Out.data(), &H, sizeof(H));
  return Out;
}
} // namespace

llvm::Expected<std::map<std::string, uint32_t>>
admitMachOSourceLibraries(std::vector<uint8_t> &B,
                          llvm::ArrayRef<SourcePatchDependency> Requests) {
  auto State = readMachODependencyState(B);
  if (!State)
    return State.takeError();
  const auto Header = parseMachOHeader(B.data(), B.size());
  auto Object = llvm::object::MachOObjectFile::create(
      llvm::MemoryBufferRef(
          llvm::StringRef(reinterpret_cast<const char *>(B.data()), B.size()),
          kLibraryImage),
      true, Header.Is64);
  if (!Object)
    return Object.takeError();
  BinaryImage Parsed;
  macho_loader::parseNeededLibraries(**Object, Parsed);
  std::map<std::string, uint32_t> Libraries;
  for (size_t I = 0; I < Parsed.MachODylibReferences.size(); ++I) {
    const auto &R = Parsed.MachODylibReferences[I];
    if (!Libraries.emplace(R.Name, I + 1).second)
      return fail(kDuplicateLibraries);
  }
  std::vector<std::vector<uint8_t>> Commands;
  for (const auto &I : Requests) {
    if (Libraries.contains(I.Library)) {
      const auto Ordinal = Libraries.at(I.Library);
      if (Ordinal > Parsed.MachODylibReferences.size())
        continue;
      const auto &R = Parsed.MachODylibReferences[Ordinal - 1];
      if (R.Weak)
        return fail(kWeakLibrary);
      continue;
    }
    Libraries.emplace(I.Library, Libraries.size() + 1);
    std::vector<uint8_t> C(
        llvm::alignTo(sizeof(dylib_command) + I.Library.size() + 1,
                      Header.Is64 ? 8 : 4),
        0);
    auto *D = reinterpret_cast<dylib_command *>(C.data());
    D->cmd = LC_LOAD_DYLIB;
    D->cmdsize = C.size();
    D->dylib.name = sizeof(*D);
    std::copy(I.Library.begin(), I.Library.end(), C.begin() + sizeof(*D));
    Commands.push_back(std::move(C));
  }
  if (auto E = appendCommands(B, Commands))
    return E;
  return Libraries;
}

llvm::Expected<std::vector<uint8_t>>
appendMachOSourceDependencies(const BinaryImage &Original,
                              std::vector<SourcePatchImport> &Imports) {
  auto B = Original.Raw;
  auto OriginalState = readMachODependencyState(B);
  if (!OriginalState)
    return OriginalState.takeError();
  auto Before = layout(B);
  if (!Before)
    return Before.takeError();
  if (Before->Slots)
    return fail(kStorageExists);
  std::vector<SourcePatchDependency> Requests;
  for (const auto &Import : Imports)
    Requests.push_back(Import.Request);
  auto Admitted = admitMachOSourceLibraries(B, Requests);
  if (!Admitted)
    return Admitted.takeError();
  const auto &Libraries = *Admitted;
  MachOPatcher P;
  P.setInputKind(RewriteInputKind::ReplacementIR);
  std::vector<uint8_t> Zeros(Imports.size() * Original.getPointerSize(), 0);
  const uint64_t VA = P.appendExecSegment(B, Zeros, "__NDIMPD", Original.Arch);
  if (!VA)
    return fail("cannot allocate native import slots");
  auto L = layout(B);
  if (!L)
    return L.takeError();
  if (!L->Slots || L->SegmentCount != Before->SegmentCount + 1 ||
      L->SlotIndex != Before->LinkeditIndex)
    return fail("slot allocation changed an original segment");
  auto S = readMachOSegment(B.data() + L->Slots, L->H.Is64);
  if (S.VMAddr != VA || S.NSects || S.FileSize < Zeros.size())
    return fail("invalid added slot storage");
  S.InitProt = S.MaxProt = VM_PROT_READ | VM_PROT_WRITE;
  writeMachOSegment(B.data() + L->Slots, L->H.Is64, S);
  for (size_t I = 0; I < Imports.size(); ++I)
    Imports[I].SlotVA = VA + I * Original.getPointerSize();
  if (auto E = registerSlots(B, *L, Imports, Libraries))
    return E;
  L = layout(B);
  if (!L)
    return L.takeError();
  std::vector<uint8_t> Metadata;
  if (L->Chained) {
    auto Encoded = chained(B, *L, Original, Imports, Libraries);
    if (!Encoded)
      return Encoded.takeError();
    Metadata = std::move(*Encoded);
  } else {
    if (L->SlotIndex >= 16)
      return fail("dyld opcode segment ordinal limit");
    const auto *D =
        reinterpret_cast<const dyld_info_command *>(B.data() + L->Bind);
    if (!rangeInBounds(D->bind_off, D->bind_size, B.size()))
      return fail("invalid ordinary bind extent");
    auto Old = llvm::ArrayRef<uint8_t>(B).slice(D->bind_off, D->bind_size);
    auto End = bindEnd(Old, Before->LinkeditIndex);
    if (!End)
      return End.takeError();
    Metadata.assign(Old.begin(), Old.begin() + *End);
    for (size_t I = 0; I < Imports.size(); ++I) {
      Metadata.push_back(BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB);
      uleb(Metadata, Libraries.at(Imports[I].Request.Library));
      Metadata.push_back(BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM);
      const auto Name = nativeName(Imports[I].Request.Symbol);
      Metadata.insert(Metadata.end(), Name.begin(), Name.end());
      Metadata.push_back(0);
      Metadata.push_back(BIND_OPCODE_SET_TYPE_IMM | BIND_TYPE_POINTER);
      Metadata.push_back(BIND_OPCODE_SET_ADDEND_SLEB);
      Metadata.push_back(0);
      Metadata.push_back(BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB |
                         L->SlotIndex);
      uleb(Metadata, I * Original.getPointerSize());
      Metadata.push_back(BIND_OPCODE_DO_BIND);
    }
    Metadata.push_back(BIND_OPCODE_DONE);
  }
  B.resize(llvm::alignTo(B.size(), 8), 0);
  if (B.size() + Metadata.size() > UINT32_MAX)
    return fail("new metadata exceeds file limits");
  const uint32_t Offset = B.size();
  B.insert(B.end(), Metadata.begin(), Metadata.end());
  if (L->Chained) {
    auto *D = reinterpret_cast<linkedit_data_command *>(B.data() + L->Chained);
    D->dataoff = Offset;
    D->datasize = Metadata.size();
  } else {
    auto *D = reinterpret_cast<dyld_info_command *>(B.data() + L->Bind);
    D->bind_off = Offset;
    D->bind_size = Metadata.size();
  }
  auto LE = readMachOSegment(B.data() + L->Linkedit, L->H.Is64);
  LE.FileSize = B.size() - LE.FileOff;
  LE.VMSize =
      llvm::alignTo(LE.FileSize, Original.Arch == Arch::AArch64 ? 16384 : 4096);
  if (LE.VMAddr > UINT64_MAX - LE.VMSize ||
      (!L->H.Is64 && LE.VMAddr + LE.VMSize > UINT32_MAX))
    return fail("LINKEDIT address overflow");
  writeMachOSegment(B.data() + L->Linkedit, L->H.Is64, LE);
  auto FinalState = readMachODependencyState(B);
  if (!FinalState)
    return FinalState.takeError();
  if (auto E = preserveMachODependencyState(*OriginalState, *FinalState))
    return E;
  for (auto &I : Imports)
    I.MachODyldDigest = FinalState->Digest;
  return B;
}
llvm::Error
verifyMachOSourceDependencies(const BinaryImage &Image,
                              llvm::ArrayRef<SourcePatchImport> Imports) {
  auto State = readMachODependencyState(Image.Raw);
  if (!State)
    return State.takeError();
  for (const auto &I : Imports) {
    if (!I.MachODyldDigest || *I.MachODyldDigest != State->Digest)
      return fail("native dyld state differs from the prepared receipt");
    const auto Found = Image.DyldBindSlots.find(I.SlotVA);
    const auto *S = Image.getSegmentFor(I.SlotVA);
    if (Found == Image.DyldBindSlots.end() ||
        Found->second.Name != nativeName(I.Request.Symbol) ||
        Found->second.Module != I.Request.Library || Found->second.Addend ||
        Found->second.WeakImport || !S || !S->isReadable() ||
        !S->isWritable() || S->isExecutable() ||
        !Image.isValidImportStorageSlot(I.SlotVA, Found->second.Name))
      return fail(
          "added import failed exact dyld binding replay: " + I.Request.Symbol +
          ", slot=" + llvm::utohexstr(I.SlotVA) + ", binding=" +
          (Found == Image.DyldBindSlots.end()
               ? std::string("absent")
               : Found->second.Name + ":" + Found->second.Module) +
          ", segment=" + (S ? S->Name : std::string("absent")));
  }
  return llvm::Error::success();
}
} // namespace neverd
