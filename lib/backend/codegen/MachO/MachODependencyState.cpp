//===- MachODependencyState.cpp - Source patch support ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate and preserve native dyld bindings.
//===----------------------------------------------------------------------===//

#include "MachODependencyState.h"

#include "MachOExportLayout.h"
#include "MachOStrictLayout.h"

#include "neverd/backend/Limits.h"
#include "neverd/backend/codegen/MachO/MachOLayoutGrowth.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/MachO/MachOArm64e.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/loader/MachO/MachOThreadedBind.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SHA256.h"

#include <cstddef>
#include <iterator>
#include <optional>
#include <set>

namespace neverd {
namespace {
using namespace llvm::MachO;
using Bytes = std::vector<uint8_t>;
#define MACHO_DEPENDENCY_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define MACHO_DEPENDENCY_TEXT(Name, Text) constexpr char Name[] = Text;
#include "MachODependencyState.def"
llvm::Error fail(const llvm::Twine &S) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "Mach-O dyld proof: " + S);
}
void number(Bytes &B, uint64_t V) {
  const auto At = B.size();
  B.resize(At + 8);
  writeLE<uint64_t>(B.data() + At, V);
}
void bytes(Bytes &B, llvm::ArrayRef<uint8_t> V) {
  number(B, V.size());
  B.insert(B.end(), V.begin(), V.end());
}
void text(Bytes &B, llvm::StringRef V) {
  bytes(B, llvm::arrayRefFromStringRef(V));
}
struct Reader {
  struct Import {
    int64_t Ordinal, Addend;
    unsigned Flags;
    llvm::StringRef Name;
  };
  llvm::ArrayRef<uint8_t> B;
  MachOHeaderInfo H;
  std::vector<MachOSegFields> Segments;
  uint64_t HeaderVA = 0, LinkeditVA = 0, LinkeditSize = 0;
  uint32_t LinkeditOff = 0;
  const dyld_info_command *Dyld = nullptr;
  const linkedit_data_command *Chain = nullptr, *Export = nullptr;
  const symtab_command *Sym = nullptr;
  const dysymtab_command *Dyn = nullptr;
  MachODependencyState State;
  MachOChainedFixups *Chained = nullptr;
  std::optional<size_t> ThreadedCopiedBytes;
  Bytes Digest;
  bool Bad = false;
  unsigned Pointer = 0;

  llvm::Error chargeThreadedNames(size_t Size) {
    if (!ThreadedCopiedBytes)
      return llvm::Error::success();
    if (Size >
        limits::loader::kMachOThreadedMaxCopiedBytes - *ThreadedCopiedBytes)
      return fail(kThreadedCopies);
    *ThreadedCopiedBytes += Size;
    return llvm::Error::success();
  }

  llvm::Expected<llvm::ArrayRef<uint8_t>> payload(uint64_t O, uint64_t N) {
    if (!N)
      return llvm::ArrayRef<uint8_t>{};
    if (O < LinkeditOff ||
        !rangeInBounds(O - LinkeditOff, N, B.size() - LinkeditOff))
      return fail("metadata is not contained in LINKEDIT");
    return B.slice(O, N);
  }
  llvm::Expected<uint64_t> slot(unsigned Segment, uint64_t Offset,
                                unsigned Alignment = 0) {
    if (Segment >= Segments.size())
      return fail("invalid segment ordinal");
    const auto &S = Segments[Segment];
    if (S.VMAddr == LinkeditVA || !(S.InitProt & VM_PROT_WRITE) ||
        (S.InitProt & VM_PROT_EXECUTE) ||
        !rangeInBounds(Offset, Pointer, S.FileSize) ||
        Offset % (Alignment ? Alignment : Pointer) ||
        S.VMAddr % (Alignment ? Alignment : Pointer))
      return fail("fixup lacks a unique writable, aligned file-backed owner");
    return S.VMAddr + Offset;
  }
  llvm::Error put(uint64_t VA, unsigned Phase, Bytes Value) {
    if (!State.Slots.emplace(std::make_pair(VA, Phase), std::move(Value))
             .second)
      return fail("duplicate fixup in the same dyld phase");
    return llvm::Error::success();
  }
  llvm::Error rebase(unsigned Segment, uint64_t Offset, uint64_t Target,
                     uint64_t Raw, unsigned Alignment = 0) {
    auto VA = slot(Segment, Offset, Alignment);
    if (!VA)
      return VA.takeError();
    if (Target >= LinkeditVA && Target - LinkeditVA < LinkeditSize)
      return fail("rebase target lies in relocatable LINKEDIT");
    Bytes Row;
    number(Row, Target == HeaderVA);
    number(Row, Target == HeaderVA ? 0 : Target);
    number(Digest, Raw);
    return put(*VA, 0, std::move(Row));
  }
  llvm::Error bind(unsigned Segment, uint64_t Offset, int64_t Ordinal,
                   llvm::StringRef Name, int64_t Addend, unsigned Flags,
                   unsigned Phase, uint64_t Raw = 0, unsigned Alignment = 0) {
    auto VA = slot(Segment, Offset, Alignment);
    if (!VA)
      return VA.takeError();
    if (Ordinal > int64_t(State.Libraries.size()) || Ordinal < -3 ||
        Name.empty())
      return fail("invalid bind library or symbol");
    if (auto Error = chargeThreadedNames(Name.size()))
      return Error;
    Bytes Row;
    number(Row, Ordinal);
    text(Row, Name);
    number(Row, Addend);
    number(Row, Flags);
    number(Digest, Raw);
    return put(*VA, Phase, std::move(Row));
  }
  llvm::Error commands() {
    if (!macho_patch_detail::validateLoadCommandRegion(B, H))
      return fail("invalid commands");
    std::vector<macho_patch_detail::MachOFileRange> S, C;
    if (!macho_patch_detail::collectMachOFileRanges(B, S, C))
      return fail("overlapping storage");
    Pointer = H.Is64 ? 8 : 4;
    State.CPUType = readLE<uint32_t>(B.data() + offsetof(mach_header, cputype));
    State.CPUSubtype =
        readLE<uint32_t>(B.data() + offsetof(mach_header, cpusubtype));
    const auto Flags = readLE<uint32_t>(B.data() + 24),
               Type = readLE<uint32_t>(B.data() + 12);
    if (!(Flags & MH_DYLDLINK) || !(Flags & MH_TWOLEVEL) ||
        (Flags & MH_FORCE_FLAT) ||
        (Type != MH_EXECUTE && Type != MH_DYLIB && Type != MH_BUNDLE))
      return fail("input requires an existing two-level dyld loading contract");
    bytes(Digest, B.slice(4, 12));
    number(Digest, Flags);
    unsigned Dylinker = 0, HeaderCount = 0, LECount = 0;
    forEachMachOLoadCommand(
        B.data(), B.size(),
        [&](const uint8_t *P, uint32_t ID, uint32_t Size, bool Is64) {
          if (ID == getMachOSegmentCmdID(Is64)) {
            auto S = readMachOSegment(P, Is64);
            if (Chained) {
              if (readMachOName(S.SegName) == kLinkeditSegment)
                Chained->LinkeditIndex = Segments.size();
              Chained->Segments.push_back({S.VMAddr,
                                           S.VMSize,
                                           S.FileOff,
                                           S.FileSize,
                                           uint32_t(P - B.data()),
                                           S.InitProt,
                                           0,
                                           0,
                                           {}});
            }
            Segments.push_back(S);
            if (S.FileOff == 0 && S.FileSize) {
              HeaderVA = S.VMAddr;
              ++HeaderCount;
            }
            if (readMachOName(S.SegName) == kLinkeditSegment) {
              LinkeditVA = S.VMAddr;
              LinkeditSize = S.VMSize;
              LinkeditOff = S.FileOff;
              ++LECount;
              Bad |= S.NSects || S.FileOff + S.FileSize != B.size();
            }
            for (unsigned I = 0; I < S.NSects; ++I) {
              auto *Q = P + getMachOSegmentCmdSize(Is64) +
                        I * getMachOSectionSize(Is64);
              Bad |= Is64 ? reinterpret_cast<const section_64 *>(Q)->nreloc != 0
                          : reinterpret_cast<const section *>(Q)->nreloc != 0;
            }
          } else if (ID == LC_LOAD_DYLINKER) {
            const auto O = reinterpret_cast<const dylinker_command *>(P)->name;
            Bad |= O < sizeof(dylinker_command) || O >= Size || P[O] != '/' ||
                   !std::memchr(P + O, 0, Size - O);
            ++Dylinker;
          } else if (ID == LC_LOAD_DYLIB || ID == LC_LOAD_WEAK_DYLIB ||
                     ID == LC_REEXPORT_DYLIB || ID == LC_LOAD_UPWARD_DYLIB) {
            auto *D = reinterpret_cast<const dylib_command *>(P);
            const auto O = D->dylib.name;
            if (O < sizeof(*D) || O >= Size ||
                !std::memchr(P + O, 0, Size - O)) {
              Bad = true;
              return;
            }
            Bytes Row;
            number(Row, ID);
            number(Row, D->dylib.timestamp);
            number(Row, D->dylib.current_version);
            number(Row, D->dylib.compatibility_version);
            text(Row, reinterpret_cast<const char *>(P + O));
            State.Libraries.push_back(std::move(Row));
            if (Chained)
              Chained->Libraries.emplace_back(
                  reinterpret_cast<const char *>(P + O));
          } else if (ID == LC_DYLD_INFO_ONLY || ID == LC_DYLD_INFO) {
            Bad |= Dyld != nullptr;
            Dyld = reinterpret_cast<const dyld_info_command *>(P);
          } else if (ID == LC_DYLD_CHAINED_FIXUPS) {
            Bad |= Chain != nullptr;
            Chain = reinterpret_cast<const linkedit_data_command *>(P);
          } else if (ID == LC_DYLD_EXPORTS_TRIE) {
            Bad |= Export != nullptr;
            Export = reinterpret_cast<const linkedit_data_command *>(P);
          } else if (ID == LC_SYMTAB) {
            Bad |= Sym != nullptr;
            Sym = reinterpret_cast<const symtab_command *>(P);
          } else if (ID == LC_DYSYMTAB) {
            Bad |= Dyn != nullptr;
            Dyn = reinterpret_cast<const dysymtab_command *>(P);
          }
        });
    if (Bad || HeaderCount != 1 || LECount != 1 || !Sym || !Dyn ||
        (!Dyld && !Chain) || (Dyld && Chain) ||
        (Type == MH_EXECUTE && Dylinker != 1) || Dylinker > 1 || Dyn->nextrel ||
        Dyn->nlocrel || Dyn->ntoc || Dyn->nmodtab || Dyn->nextrefsyms)
      return fail("ambiguous, static or classic dyld metadata is not modeled");
    for (const auto &L : State.Libraries)
      bytes(Digest, L);
    if (Chained)
      Chained->HeaderVA = HeaderVA;
    if (Chained && Chain) {
      Chained->CommandOffset =
          reinterpret_cast<const uint8_t *>(Chain) - B.data();
    }
    return llvm::Error::success();
  }
  llvm::Error symbols() {
    const unsigned Width = H.Is64 ? sizeof(nlist_64) : sizeof(nlist);
    auto Table = payload(Sym->symoff, uint64_t(Sym->nsyms) * Width);
    if (!Table)
      return Table.takeError();
    auto Names = payload(Sym->stroff, Sym->strsize);
    if (!Names)
      return Names.takeError();
    auto Indirect =
        payload(Dyn->indirectsymoff, uint64_t(Dyn->nindirectsyms) * 4);
    if (!Indirect)
      return Indirect.takeError();
    if (Dyn->ilocalsym || Dyn->iextdefsym != Dyn->nlocalsym ||
        uint64_t(Dyn->iextdefsym) + Dyn->nextdefsym != Dyn->iundefsym ||
        uint64_t(Dyn->iundefsym) + Dyn->nundefsym != Sym->nsyms)
      return fail("dynamic symbol partitions do not cover the table");
    std::vector<std::pair<std::string, std::string>> Sections;
    forEachMachOLoadCommand(
        B.data(), B.size(),
        [&](const uint8_t *P, uint32_t ID, uint32_t, bool Is64) {
          if (ID != getMachOSegmentCmdID(Is64))
            return;
          auto S = readMachOSegment(P, Is64);
          for (unsigned I = 0; I < S.NSects; ++I) {
            const auto *Q = P + getMachOSegmentCmdSize(Is64) +
                            I * getMachOSectionSize(Is64);
            Sections.emplace_back(
                readMachOName(S.SegName),
                readMachOName(reinterpret_cast<const char *>(Q)));
          }
        });
    for (unsigned I = 0; I < Sym->nsyms; ++I) {
      auto N = Table->slice(uint64_t(I) * Width, Width);
      const auto O = readLE<uint32_t>(N.data());
      if (O >= Names->size() ||
          !std::memchr(Names->data() + O, 0, Names->size() - O))
        return fail("invalid nlist name");
      llvm::StringRef Name(reinterpret_cast<const char *>(Names->data() + O));
      Bytes Row;
      text(Row, Name);
      number(Row, N[4]);
      if (N[5] > Sections.size())
        return fail("symbol section ordinal is invalid");
      if (N[5]) {
        text(Row, Sections[N[5] - 1].first);
        text(Row, Sections[N[5] - 1].second);
      } else {
        text(Row, {});
        text(Row, {});
      }
      number(Row, readLE<uint16_t>(N.data() + 6));
      const auto Value = H.Is64 ? readLE<uint64_t>(N.data() + 8)
                                : readLE<uint32_t>(N.data() + 8);
      number(Row,
             Name == "__mh_execute_header" && Value == HeaderVA ? 0 : Value);
      State.Symbols.push_back(Row);
      if (I >= Dyn->iundefsym) {
        if ((N[4] & (N_STAB | N_TYPE | N_EXT)) != (N_UNDF | N_EXT))
          return fail("invalid undefined symbol partition");
        bytes(Digest, Row);
      }
    }
    for (unsigned I = 0; I < Dyn->nindirectsyms; ++I) {
      auto Index = readLE<uint32_t>(Indirect->data() + uint64_t(I) * 4);
      Bytes Row;
      if (Index & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) {
        if (Index & ~(INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS))
          return fail("invalid special indirect symbol");
        number(Row, Index);
      } else {
        if (Index >= State.Symbols.size())
          return fail("invalid indirect symbol index");
        Row = State.Symbols[Index];
      }
      bytes(Digest, Row);
      State.Indirect.push_back(std::move(Row));
    }
    return llvm::Error::success();
  }
  llvm::Error opcodes(llvm::ArrayRef<uint8_t> Stream, unsigned Phase) {
    // Phase 0 rebases; 1 ordinary; 2 weak; 3 lazy. A slot may participate in
    // rebase and lazy binding, or ordinary and weak binding, independently.
    bytes(Digest, Stream);
    if (Stream.empty())
      return llvm::Error::success();
    size_t At = 0;
    unsigned Segment = UINT32_MAX, Type = Phase == 3 ? BIND_TYPE_POINTER : 0,
             Flags = 0;
    uint64_t Offset = 0;
    int64_t Ordinal = 0, Addend = 0;
    llvm::StringRef Name;
    auto Read = [&](bool Signed = false) -> uint64_t {
      if (At >= Stream.size()) {
        Bad = true;
        return 0;
      }
      unsigned N = 0;
      const char *E = nullptr;
      auto V = Signed ? uint64_t(llvm::decodeSLEB128(Stream.data() + At, &N,
                                                     Stream.end(), &E))
                      : llvm::decodeULEB128(Stream.data() + At, &N,
                                            Stream.end(), &E);
      At += N;
      Bad |= E != nullptr || !N;
      return V;
    };
    auto Advance = [&](uint64_t N) {
      if (N > UINT64_MAX - Offset)
        Bad = true;
      else
        Offset += N;
    };
    auto Perform = [&]() -> llvm::Error {
      if (Bad || Type != 1)
        return fail("unmodeled fixup type or malformed operand");
      if (Phase)
        return bind(Segment, Offset, Phase == 2 ? -3 : Ordinal, Name, Addend,
                    Flags, Phase);
      auto VA = slot(Segment, Offset);
      if (!VA)
        return VA.takeError();
      const auto *P = B.data() + Segments[Segment].FileOff + Offset;
      const uint64_t V = H.Is64 ? readLE<uint64_t>(P) : readLE<uint32_t>(P);
      return rebase(Segment, Offset, V, V);
    };
    while (At < Stream.size()) {
      const unsigned Code = Stream[At++], Op = Code & 0xf0, Imm = Code & 15;
      if (!Code) {
        if (Phase == 3) {
          if (At == Stream.size())
            return llvm::Error::success();
          Segment = UINT32_MAX;
          Type = BIND_TYPE_POINTER;
          Flags = 0;
          Offset = 0;
          Ordinal = Addend = 0;
          Name = {};
          continue;
        }
        if (llvm::any_of(Stream.drop_front(At), [](uint8_t V) { return V; }))
          return fail("bytes after dyld DONE");
        return llvm::Error::success();
      }
      if (!Phase) {
        switch (Op) {
        case REBASE_OPCODE_SET_TYPE_IMM:
          Type = Imm;
          break;
        case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
          Segment = Imm;
          Offset = Read();
          break;
        case REBASE_OPCODE_ADD_ADDR_ULEB:
          Advance(Read());
          break;
        case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
          Advance(Imm * Pointer);
          break;
        case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
        case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB: {
          const auto Count = Op == REBASE_OPCODE_DO_REBASE_IMM_TIMES ? Imm
                             : Op == REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB
                                 ? 1
                                 : Read();
          const auto Skip =
              Op == REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB ||
                      Op == REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB
                  ? Read()
                  : 0;
          if (Bad || Count > B.size() / Pointer || Skip > UINT64_MAX - Pointer)
            return fail("invalid rebase repetition");
          for (uint64_t I = 0; I < Count; ++I) {
            if (auto E = Perform())
              return E;
            Advance(Pointer + Skip);
          }
          break;
        }
        default:
          return fail("unmodeled rebase opcode");
        }
      } else {
        switch (Op) {
        case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
          Ordinal = Imm;
          break;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: {
          auto V = Read();
          if (V > INT64_MAX)
            Bad = true;
          Ordinal = V;
          break;
        }
        case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
          Ordinal = Imm ? int8_t(Imm | 0xf0) : 0;
          break;
        case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: {
          if (Imm & ~(BIND_SYMBOL_FLAGS_WEAK_IMPORT |
                      BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION))
            return fail("unknown bind flags");
          Flags = Imm;
          auto End = std::find(Stream.begin() + At, Stream.end(), 0);
          if (End == Stream.end())
            return fail("unterminated bind symbol");
          Name = llvm::StringRef(
              reinterpret_cast<const char *>(Stream.data() + At),
              End - Stream.begin() - At);
          At = End - Stream.begin() + 1;
          break;
        }
        case BIND_OPCODE_SET_TYPE_IMM:
          Type = Imm;
          break;
        case BIND_OPCODE_SET_ADDEND_SLEB:
          Addend = Read(true);
          break;
        case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
          Segment = Imm;
          Offset = Read();
          break;
        case BIND_OPCODE_ADD_ADDR_ULEB:
          Advance(Read());
          break;
        case BIND_OPCODE_DO_BIND:
        case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
        case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
        case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
          const auto Count =
              Op == BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB ? Read() : 1;
          const auto Skip =
              Op == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB ||
                      Op == BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB
                  ? Read()
              : Op == BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED ? Imm * Pointer
                                                              : 0;
          if (Bad || Count > B.size() / Pointer || Skip > UINT64_MAX - Pointer)
            return fail("invalid bind repetition");
          for (uint64_t I = 0; I < Count; ++I) {
            if (auto E = Perform())
              return E;
            Advance(Pointer + Skip);
          }
          break;
        }
        default:
          return fail("unmodeled bind opcode");
        }
      }
      if (Bad)
        return fail("invalid dyld operand or address overflow");
    }
    return fail("dyld stream lacks DONE");
  }
  llvm::Expected<unsigned>
  recordArm64e(unsigned I, uint64_t Offset, uint64_t VA, uint64_t Raw,
               const macho_loader::arm64e::ChainFormat &Format,
               llvm::ArrayRef<Import> Imports) {
    const auto Word = macho_loader::arm64e::decodePointerWord(Raw, Format);
    if (!Word)
      return fail(kInvalidArm64eWord);

    if (Word->Bind) {
      if (Word->Target >= Imports.size())
        return fail(kInvalidArm64eImport);
      const auto &Import = Imports[Word->Target];
      int64_t Addend;
      if (llvm::AddOverflow(Import.Addend, Word->Addend, Addend))
        return fail(kArm64eAddendOverflow);
      if (auto E = bind(I, Offset, Import.Ordinal, Import.Name, Addend,
                        Import.Flags, kBindPhase, Raw, Format.Stride))
        return E;
    } else {
      uint64_t Base = Word->Authenticated || Format.Offset ? HeaderVA : 0;
      if (Format.Segmented) {
        if (Word->SegmentIndex >= Segments.size() ||
            Word->Target > Segments[Word->SegmentIndex].VMSize)
          return fail(kInvalidArm64eSegment);
        Base = Segments[Word->SegmentIndex].VMAddr;
      }
      if (Word->Target > UINT64_MAX - Base)
        return fail(kArm64eTargetOverflow);
      if (auto E = rebase(I, Offset, Base + Word->Target, Raw, Format.Stride))
        return E;
    }
    if (Word->Authenticated) {
      // Slot identity above also owns address diversity. Retaining
      // only the target/import would silently admit a different key
      // or discriminator after image layout edits.
      auto &Row = State.Slots.at({VA, Word->Bind ? kBindPhase : kRebasePhase});
      number(Row, Word->Key);
      number(Row, Word->Diversity);
      number(Row, Word->AddressDiversity);
    }
    return Word->Next * Format.Stride;
  }

  llvm::Error threaded(llvm::ArrayRef<uint8_t> Stream) {
    namespace arm64e = macho_loader::arm64e;
    if (!H.Is64 || State.CPUType != CPU_TYPE_ARM64 ||
        !arm64e::isSubtype(State.CPUSubtype) || Dyld->rebase_size ||
        Dyld->weak_bind_size || Dyld->lazy_bind_size)
      return fail(kThreadedContract);
    const auto Plan =
        macho_loader::threaded::decode(Stream, State.Libraries.size());
    if (!Plan)
      return fail(kThreadedProgram);
    ThreadedCopiedBytes = Plan->CopiedBytes;
    bytes(Digest, Stream);
    std::vector<Import> Imports;
    Imports.reserve(Plan->Imports.size());
    for (const auto &Entry : Plan->Imports) {
      Imports.push_back({Entry.Ordinal, Entry.Addend, Entry.Flags, Entry.Name});
      if (Chained) {
        if (auto Error = chargeThreadedNames(Entry.Name.size()))
          return Error;
        Chained->Imports.push_back(
            {Entry.Ordinal, Entry.Addend, Entry.Flags, Entry.Name});
      }
    }
    if (Chained) {
      Chained->Threaded = true;
      Chained->CommandOffset =
          reinterpret_cast<const uint8_t *>(Dyld) - B.data();
    }
    const auto Format = *arm64e::chainFormat(DYLD_CHAINED_PTR_ARM64E);
    std::set<uint64_t> Slots;
    for (const auto &Root : Plan->Roots) {
      const unsigned I = Root.SegmentIndex;
      if (I >= Segments.size())
        return fail(kThreadedStorage);
      for (unsigned J = 0; J <= I; ++J)
        if (Segments[J].VMAddr == LinkeditVA)
          return fail(kThreadedStorage);
      uint64_t Offset = Root.Offset;
      for (;;) {
        auto VA = slot(I, Offset, Format.Stride);
        if (!VA)
          return VA.takeError();
        if (!Slots.insert(*VA).second ||
            Slots.size() > limits::loader::kMachODyldBindMaxBindSlots)
          return fail(kOverlappingChainSlots);
        const auto Raw =
            readLE<uint64_t>(B.data() + Segments[I].FileOff + Offset);
        auto Next = recordArm64e(I, Offset, *VA, Raw, Format, Imports);
        if (!Next)
          return Next.takeError();
        if (Chained) {
          auto &Segment = Chained->Segments[I];
          Segment.PointerFormat = DYLD_CHAINED_PTR_ARM64E;
          Segment.PageSize = arm64e::kLargePageBytes;
          Segment.Slots.push_back({Offset, Raw});
        }
        if (!*Next)
          break;
        if (*Next > UINT64_MAX - Offset)
          return fail(kThreadedStorage);
        Offset += *Next;
      }
    }
    return llvm::Error::success();
  }

  llvm::Error chains() {
    auto Data = payload(Chain->dataoff, Chain->datasize);
    if (!Data)
      return Data.takeError();
    auto C = *Data;
    if (!H.Is64 || C.size() < sizeof(dyld_chained_fixups_header))
      return fail("unsupported chained header");
    dyld_chained_fixups_header F;
    std::memcpy(&F, C.data(), sizeof(F));
    const unsigned Width = F.imports_format == DYLD_CHAINED_IMPORT          ? 4
                           : F.imports_format == DYLD_CHAINED_IMPORT_ADDEND ? 8
                           : F.imports_format == DYLD_CHAINED_IMPORT_ADDEND64
                               ? 16
                               : 0;
    if (F.fixups_version || F.symbols_format || !Width ||
        F.starts_offset < sizeof(F) ||
        !rangeInBounds(F.starts_offset, 4, F.imports_offset) ||
        F.imports_offset > C.size() ||
        !rangeInBounds(F.imports_offset, uint64_t(F.imports_count) * Width,
                       F.symbols_offset) ||
        F.symbols_offset > C.size())
      return fail("invalid chained table extents or version");
    // Native segment-count completion may repack the starts table. Retain
    // exact import bytes and every decoded/raw slot, not file-relative offsets.
    bytes(Digest, C.slice(F.imports_offset));
    number(Digest, F.imports_format);
    std::vector<Import> Imports;
    for (uint32_t I = 0; I < F.imports_count; ++I) {
      const auto *P = C.data() + F.imports_offset + uint64_t(I) * Width;
      const uint64_t V =
          Width == 16 ? readLE<uint64_t>(P) : readLE<uint32_t>(P);
      const uint32_t Name = Width == 16 ? V >> 32 : V >> 9;
      const int64_t Ordinal = macho_loader::chainedLibraryOrdinal(
          V & (Width == 16 ? 0xffff : 0xff), Width == 16 ? 65536 : 256);
      const unsigned Weak = (V >> (Width == 16 ? 16 : 8)) & 1;
      const int64_t Addend = Width == 16  ? int64_t(readLE<uint64_t>(P + 8))
                             : Width == 8 ? int32_t(readLE<uint32_t>(P + 4))
                                          : 0;
      if ((Width == 16 && (V & 0xfffe0000)) || Ordinal < -3 ||
          Ordinal > int64_t(State.Libraries.size()) ||
          Name >= C.size() - F.symbols_offset ||
          !std::memchr(C.data() + F.symbols_offset + Name, 0,
                       C.size() - F.symbols_offset - Name))
        return fail("invalid chained import identity");
      llvm::StringRef N(
          reinterpret_cast<const char *>(C.data() + F.symbols_offset + Name));
      if (N.empty())
        return fail("empty chained symbol");
      Imports.push_back({Ordinal, Addend, Weak, N});
      if (Chained)
        Chained->Imports.push_back({Ordinal, Addend, Weak, N.str()});
    }
    const uint32_t Count = readLE<uint32_t>(C.data() + F.starts_offset);
    unsigned PointerFormat = 0;
    if (Count > Segments.size() ||
        !rangeInBounds(F.starts_offset, 4 + uint64_t(Count) * 4,
                       F.imports_offset))
      return fail("invalid chained segment count");
    std::vector<std::pair<uint64_t, uint64_t>> Ranges;
    std::set<uint64_t> ChainSlots;
    constexpr size_t StartOffset =
        offsetof(dyld_chained_starts_in_segment, page_start);
    for (unsigned I = 0; I < Count; ++I) {
      const auto Relative =
          readLE<uint32_t>(C.data() + F.starts_offset + 4 + I * 4);
      if (!Relative)
        continue;
      const uint64_t O = uint64_t(F.starts_offset) + Relative;
      if (Relative < 4 + uint64_t(Count) * 4 ||
          !rangeInBounds(O, 22, F.imports_offset))
        return fail("invalid chain start extent");
      const auto *S = C.data() + O;
      const unsigned Size = readLE<uint32_t>(S), Page = readLE<uint16_t>(S + 4),
                     Format = readLE<uint16_t>(S + 6),
                     Pages = readLE<uint16_t>(S + 20);
      const auto Arm64e = macho_loader::arm64e::chainFormat(Format);
      if ((Page != 4096 && Page != 16384) ||
          (Format != DYLD_CHAINED_PTR_64 &&
           Format != DYLD_CHAINED_PTR_64_OFFSET && !Arm64e) ||
          (Arm64e && State.CPUType != CPU_TYPE_ARM64) ||
          Size < StartOffset + Pages * sizeof(uint16_t) ||
          (Size - StartOffset) % sizeof(uint16_t) ||
          !rangeInBounds(O, Size, F.imports_offset) ||
          readLE<uint32_t>(S + 16) || Segments[I].VMAddr < HeaderVA ||
          readLE<uint64_t>(S + 8) != Segments[I].VMAddr - HeaderVA ||
          uint64_t(Pages) * Page > llvm::alignTo(Segments[I].FileSize, Page))
        return fail("chain start has invalid encoding or segment owner");
      if (PointerFormat && PointerFormat != Format)
        return fail("mixed chained pointer formats");
      PointerFormat = Format;
      if (Chained) {
        Chained->Segments[I].PointerFormat = Format;
        Chained->Segments[I].PageSize = Page;
      }
      number(Digest, Format);
      number(Digest, Page);
      Ranges.emplace_back(O, O + Size);
      const auto StartCount = (Size - StartOffset) / sizeof(uint16_t);
      const auto StartAt = [&](unsigned Index) {
        return readLE<uint16_t>(S + StartOffset + Index * sizeof(uint16_t));
      };
      std::set<unsigned> OverflowEntries;
      unsigned UsedStarts = Pages;
      for (unsigned P = 0; P < Pages; ++P) {
        const auto Start = StartAt(P);
        if (Start == DYLD_CHAINED_PTR_START_NONE)
          continue;
        std::vector<unsigned> Starts;
        if (Start & DYLD_CHAINED_PTR_START_MULTI) {
          unsigned Index = Start & ~DYLD_CHAINED_PTR_START_MULTI;
          if (Index < Pages)
            return fail(kInvalidChainOverflow);
          for (;;) {
            if (Index >= StartCount || !OverflowEntries.insert(Index).second)
              return fail(kInvalidChainOverflow);
            const auto Value = StartAt(Index++);
            const unsigned Offset = Value & ~DYLD_CHAINED_PTR_START_LAST;
            if (!Starts.empty() && Offset <= Starts.back())
              return fail(kInvalidChainOverflow);
            Starts.push_back(Offset);
            if (Value & DYLD_CHAINED_PTR_START_LAST)
              break;
          }
          UsedStarts = std::max(UsedStarts, Index);
        } else {
          Starts.push_back(Start);
        }
        for (unsigned At : Starts) {
          while (true) {
            if (!rangeInBounds(At, 8, Page))
              return fail("chained pointer crosses a page");
            const uint64_t Offset = uint64_t(P) * Page + At;
            auto VA = slot(I, Offset, Arm64e ? Arm64e->Stride : Pointer);
            if (!VA)
              return VA.takeError();
            // Distinct starts must not revisit or partially overlap a slot,
            // including one owned by the other bind/rebase phase.
            const auto NextSlot = ChainSlots.lower_bound(*VA);
            if ((NextSlot != ChainSlots.end() && *NextSlot - *VA < Pointer) ||
                (NextSlot != ChainSlots.begin() &&
                 *VA - *std::prev(NextSlot) < Pointer))
              return fail(kOverlappingChainSlots);
            ChainSlots.insert(NextSlot, *VA);
            const uint64_t Raw =
                readLE<uint64_t>(B.data() + Segments[I].FileOff + Offset);
            if (Chained)
              Chained->Segments[I].Slots.push_back({Offset, Raw});
            unsigned NextBytes = ((Raw >> 51) & 0xfff) * kOrdinaryChainStride;
            if (Arm64e) {
              auto Next = recordArm64e(I, Offset, *VA, Raw, *Arm64e, Imports);
              if (!Next)
                return Next.takeError();
              NextBytes = *Next;
            } else if (Raw >> 63) {
              const unsigned Ord = Raw & 0xffffff;
              if (Ord >= Imports.size() || ((Raw >> 32) & 0x7ffff))
                return fail("invalid chained bind ordinal or reserved bits");
              const auto &R = Imports[Ord];
              const int64_t Add = (Raw >> 24) & 0xff;
              if (R.Addend > INT64_MAX - Add)
                return fail("chained addend overflow");
              if (auto E = bind(I, Offset, R.Ordinal, R.Name, R.Addend + Add,
                                R.Flags, 1, Raw))
                return E;
            } else {
              if ((Raw >> 44) & 0x7f)
                return fail("nonzero chained rebase reserved bits");
              uint64_t Target =
                  (Raw & 0xfffffffffULL) | (((Raw >> 36) & 0xff) << 56);
              if (Format == DYLD_CHAINED_PTR_64_OFFSET) {
                if (Target > UINT64_MAX - HeaderVA)
                  return fail("chained rebase overflow");
                Target += HeaderVA;
              }
              if (auto E = rebase(I, Offset, Target, Raw))
                return E;
            }
            if (!NextBytes)
              break;
            if (NextBytes < Pointer || NextBytes > Page - At)
              return fail("overlapping or cross-page chain");
            At += NextBytes;
          }
        }
      }
      const size_t UsedBytes = StartOffset + UsedStarts * sizeof(uint16_t);
      if (Size != UsedBytes && Size != llvm::alignTo(UsedBytes, Pointer))
        return fail(kInvalidChainOverflow);
      for (unsigned Index = Pages; Index < StartCount; ++Index)
        if (!OverflowEntries.count(Index) && StartAt(Index))
          return fail(kInvalidChainOverflow);
    }
    std::sort(Ranges.begin(), Ranges.end());
    for (size_t I = 1; I < Ranges.size(); ++I)
      if (Ranges[I].first < Ranges[I - 1].second)
        return fail("overlapping chain start tables");
    return llvm::Error::success();
  }
};
} // namespace

llvm::Expected<MachODependencyState>
readMachODependencyState(llvm::ArrayRef<uint8_t> B,
                         MachOChainedFixups *Chained) {
  Reader R;
  MachOChainedFixups Details;
  R.Chained = Chained ? &Details : nullptr;
  R.B = B;
  if (auto E = R.commands())
    return E;
  if (auto E = R.symbols())
    return E;
  if (R.Chain) {
    if (auto E = R.chains())
      return E;
  }
  if (R.Dyld) {
    const auto &D = *R.Dyld;
    const std::pair<uint32_t, uint32_t> Streams[] = {
        {D.rebase_off, D.rebase_size},
        {D.bind_off, D.bind_size},
        {D.weak_bind_off, D.weak_bind_size},
        {D.lazy_bind_off, D.lazy_bind_size}};
    for (unsigned I = 0; I < 4; ++I) {
      auto V = R.payload(Streams[I].first, Streams[I].second);
      if (!V)
        return V.takeError();
      const auto Kind = I == kBindPhase ? macho_loader::threaded::classify(*V)
                                        : macho_loader::threaded::StreamKind{};
      if (auto E = Kind.Threaded ? R.threaded(*V) : R.opcodes(*V, I))
        return E;
    }
    auto V = R.payload(D.export_off, D.export_size);
    if (!V)
      return V.takeError();
    auto Facts = machOExportIdentities(*V, R.HeaderVA);
    if (!Facts)
      return Facts.takeError();
    bytes(R.State.Exports, *Facts);
    bytes(R.Digest, *V);
  }
  if (R.Export) {
    auto V = R.payload(R.Export->dataoff, R.Export->datasize);
    if (!V)
      return V.takeError();
    auto Facts = machOExportIdentities(*V, R.HeaderVA);
    if (!Facts)
      return Facts.takeError();
    bytes(R.State.Exports, *Facts);
    bytes(R.Digest, *V);
  }
  bytes(R.Digest, R.State.Exports);
  for (const auto &[Key, Value] : R.State.Slots) {
    number(R.Digest, Key.first);
    number(R.Digest, Key.second);
    bytes(R.Digest, Value);
  }
  R.State.Digest = llvm::SHA256::hash(R.Digest);
  if (Chained)
    *Chained = std::move(Details);
  return std::move(R.State);
}
llvm::Error preserveMachODependencyState(const MachODependencyState &Before,
                                         const MachODependencyState &After) {
  if (Before.CPUType != After.CPUType || Before.CPUSubtype != After.CPUSubtype)
    return fail(kChangedCPUABI);
  for (const auto &[Key, Value] : Before.Slots) {
    auto I = After.Slots.find(Key);
    if (I == After.Slots.end() || I->second != Value)
      return fail("old exact-slot binding or rebase changed");
  }
  auto Prefix = [](const auto &A, const auto &B) {
    return A.size() <= B.size() && std::equal(A.begin(), A.end(), B.begin());
  };
  if (!Prefix(Before.Libraries, After.Libraries) ||
      !Prefix(Before.Indirect, After.Indirect) ||
      Before.Exports != After.Exports)
    return fail("old library, indirect symbol or export identity changed");
  // Callback definitions may be inserted before the undefined partition.
  auto At = After.Symbols.begin();
  for (const auto &S : Before.Symbols) {
    At = std::find(At, After.Symbols.end(), S);
    if (At == After.Symbols.end())
      return fail("old native symbol identity changed");
    ++At;
  }
  return llvm::Error::success();
}
llvm::Error completeMachOChainedSegmentTable(std::vector<uint8_t> &Binary,
                                             Arch A) {
  using namespace llvm::MachO;
  uint64_t Command = 0, Linkedit = 0;
  unsigned Count = 0;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *C, uint32_t ID, uint32_t, bool Is64) {
        if (ID == LC_DYLD_CHAINED_FIXUPS)
          Command = C - Binary.data();
        if (ID == getMachOSegmentCmdID(Is64)) {
          ++Count;
          if (readMachOName(readMachOSegment(C, Is64).SegName) == "__LINKEDIT")
            Linkedit = C - Binary.data();
        }
      });
  if (!Command)
    return llvm::Error::success();
  auto Before = readMachODependencyState(Binary);
  if (!Before)
    return Before.takeError();
  const auto D =
      *reinterpret_cast<const linkedit_data_command *>(Binary.data() + Command);
  auto Old = llvm::ArrayRef<uint8_t>(Binary).slice(D.dataoff, D.datasize);
  auto H = *reinterpret_cast<const dyld_chained_fixups_header *>(Old.data());
  const unsigned OldCount = readLE<uint32_t>(Old.data() + H.starts_offset);
  if (OldCount == Count)
    return llvm::Error::success();
  if (OldCount > Count || !Linkedit || !Count ||
      Count >
          ::neverd::limits::backend::
              kMachODependencyStateCompleteMachOChainedSegmentTableCountLimit)
    return fail("invalid final segment inventory");
  // New code/data segments precede terminal LINKEDIT; all existing nonzero
  // entries were independently authenticated against their exact segment VA.
  Bytes Starts(4 + Count * 4, 0);
  writeLE<uint32_t>(Starts.data(), Count);
  for (unsigned I = 0; I < OldCount; ++I) {
    auto O = readLE<uint32_t>(Old.data() + H.starts_offset + 4 + I * 4);
    if (!O)
      continue;
    const auto Size = readLE<uint32_t>(Old.data() + H.starts_offset + O);
    Starts.resize(llvm::alignTo(Starts.size(), 8), 0);
    writeLE<uint32_t>(Starts.data() + 4 + I * 4, Starts.size());
    auto Body = Old.slice(H.starts_offset + O, Size);
    Starts.insert(Starts.end(), Body.begin(), Body.end());
  }
  Bytes Out(llvm::alignTo(sizeof(H), 8), 0);
  H.starts_offset = Out.size();
  Out.insert(Out.end(), Starts.begin(), Starts.end());
  Out.resize(llvm::alignTo(Out.size(), 8), 0);
  const auto ImportOff = H.imports_offset;
  H.imports_offset = Out.size();
  H.symbols_offset = H.imports_offset + (H.symbols_offset - ImportOff);
  auto Tail = Old.drop_front(ImportOff);
  Out.insert(Out.end(), Tail.begin(), Tail.end());
  std::memcpy(Out.data(), &H, sizeof(H));
  auto B = Binary;
  B.resize(llvm::alignTo(B.size(), 8), 0);
  if (B.size() > UINT32_MAX || Out.size() > UINT32_MAX - B.size())
    return fail("completed chains exceed file limits");
  const auto Offset = B.size();
  B.insert(B.end(), Out.begin(), Out.end());
  auto *LC = reinterpret_cast<linkedit_data_command *>(B.data() + Command);
  LC->dataoff = Offset;
  LC->datasize = Out.size();
  auto S = readMachOSegment(B.data() + Linkedit, true);
  S.FileSize = B.size() - S.FileOff;
  S.VMSize = llvm::alignTo(S.FileSize, A == Arch::AArch64 ? 16384 : 4096);
  writeMachOSegment(B.data() + Linkedit, true, S);
  auto After = readMachODependencyState(B);
  if (!After)
    return After.takeError();
  if (Before->Digest != After->Digest)
    return fail("completing segment inventory changed dyld meaning");
  if (auto E = preserveMachODependencyState(*Before, *After))
    return E;
  Binary.swap(B);
  return llvm::Error::success();
}

} // namespace neverd
