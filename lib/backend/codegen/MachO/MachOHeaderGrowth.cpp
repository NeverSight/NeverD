//===- MachOHeaderGrowth.cpp - Source patch support ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Grow load commands with stable code addresses.
//===----------------------------------------------------------------------===//

#include "MachODependencyState.h"
#include "MachOExportLayout.h"
#include "MachOStrictLayout.h"

#include "neverd/backend/Limits.h"
#include "neverd/backend/codegen/MachO/MachOLayoutGrowth.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/loader/MachO/MachOArm64e.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/LEB128.h"

#include <capstone/capstone.h>
#include <set>
namespace neverd {
namespace {
using namespace llvm::MachO;
using Bytes = std::vector<uint8_t>;
#define MACHO_HEADER_TEXT(Name, Text) constexpr char Name[] = Text;
#define MACHO_HEADER_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "MachOHeaderGrowth.def"

llvm::Error fail(const llvm::Twine &S) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "Mach-O header growth: " + S);
}
// The strict dyld reader owns slot discovery, including multi-start pages and
// threaded roots. Rewriting only target bits preserves links and signing data.
llvm::Expected<uint64_t> relocatedChainWord(uint64_t Raw, uint16_t Format,
                                            const MachOChainedFixups &Chains,
                                            uint64_t HeaderSize,
                                            uint64_t Shift) {
  namespace arm64e = macho_loader::arm64e;
  uint64_t Value;
  unsigned Width = kOrdinaryTargetBits;
  bool Relative = Format == DYLD_CHAINED_PTR_64_OFFSET;
  if (const auto F = arm64e::chainFormat(Format)) {
    const auto Word = *arm64e::decodePointerWord(Raw, *F);
    if (Word.Bind)
      return Raw;
    if (F->Segmented &&
        Chains.Segments[Word.SegmentIndex].VMAddr != Chains.HeaderVA)
      return Raw;
    Value = Word.Target;
    Width = F->Segmented         ? arm64e::kSegmentedTargetOffset.Width
            : Word.Authenticated ? arm64e::kAuthenticatedTarget.Width
                                 : arm64e::kTarget.Width;
    Relative = F->Segmented || Word.Authenticated || F->Offset;
  } else {
    dyld_chained_ptr_64_rebase Word;
    std::memcpy(&Word, &Raw, sizeof(Word));
    if (Word.bind)
      return Raw;
    Value = Word.target | (uint64_t(Word.high8) << arm64e::kHigh8Shift);
  }

  // An exact reference to the header follows it. Other live targets retain
  // their VA when the header grows down into its prefix.
  const uint64_t Base = Relative ? 0 : Chains.HeaderVA;
  if (Value > Base && Value - Base < HeaderSize)
    return fail(kChainHeaderInterior);
  const uint64_t Mask = llvm::maskTrailingOnes<uint64_t>(Width);
  if (Relative) {
    if (!Value)
      return Raw;
    if (Shift > Mask - (Raw & Mask))
      return fail(kChainTargetOverflow);
    return Raw + Shift;
  }
  if (Value != Base)
    return Raw;
  const uint64_t NewBase = Base - Shift;
  if ((Base & ~Mask) != (NewBase & ~Mask))
    return fail(kChainTargetOverflow);
  return (Raw & ~Mask) | (NewBase & Mask);
}

llvm::Error relocateChainSlots(Bytes &Binary, const MachOChainedFixups &Chains,
                               uint64_t HeaderSize, uint64_t Shift) {
  for (const auto &Segment : Chains.Segments) {
    if (Segment.StartsFileOffset && Segment.VMAddr != Chains.HeaderVA) {
      if (Segment.VMAddr < Chains.HeaderVA ||
          Segment.VMAddr - Chains.HeaderVA > UINT64_MAX - Shift)
        return fail(kChainStartRange);
      writeLE<uint64_t>(
          Binary.data() + Shift + Segment.StartsFileOffset +
              offsetof(dyld_chained_starts_in_segment, segment_offset),
          Segment.VMAddr - Chains.HeaderVA + Shift);
    }
    for (const auto &Slot : Segment.Slots) {
      auto Word = relocatedChainWord(Slot.Raw, Segment.PointerFormat, Chains,
                                     HeaderSize, Shift);
      if (!Word)
        return Word.takeError();
      writeLE<uint64_t>(Binary.data() + Shift + Segment.FileOff + Slot.Offset,
                        *Word);
    }
  }
  return llvm::Error::success();
}

void uleb(Bytes &B, uint64_t V) {
  uint8_t T[10];
  auto N = llvm::encodeULEB128(V, T);
  B.insert(B.end(), T, T + N);
}
// Original instruction addresses and bytes remain stable. Header-relative
// arithmetic has no complete producer relocation evidence in linked images;
// reject it rather than rewriting only the first instruction of a computation.
llvm::Error headerReferences(Bytes &B, const section_64 &S, uint64_t Old,
                             uint64_t HeaderEnd, uint64_t Shift, Arch A) {
  if (!(S.flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)))
    return llvm::Error::success();
  const uint64_t Off = S.offset + Shift;
  if (A == Arch::AArch64) {
    if (S.size % 4)
      return fail("unaligned instruction section");
    for (uint64_t I = 0; I < S.size; I += 4) {
      auto *P = B.data() + Off + I;
      const auto W = readLE<uint32_t>(P);
      const auto PC = S.addr + I;
      if ((W & 0x1f000000) != 0x10000000)
        continue;
      const bool Page = W & 0x80000000;
      const int64_t Imm =
          llvm::SignExtend64<21>(((W >> 29) & 3) | (((W >> 5) & 0x7ffff) << 2));
      const uint64_t Target =
          Page ? (PC & ~uint64_t(4095)) + Imm * 4096 : PC + Imm;
      uint64_t Exact = Target;
      if (Page && Target == (Old & ~uint64_t(4095))) {
        if (I + 8 > S.size)
          return fail("unresolved header-page ADRP");
        const auto Next = readLE<uint32_t>(P + 4), Reg = W & 31;
        if ((Next & 0xffc00000) != 0x91000000 || (Next & 31) != Reg ||
            ((Next >> 5) & 31) != Reg)
          return fail("header-page ADRP needs a proven consuming ADD");
        Exact += (Next >> 10) & 4095;
      }
      if (Exact < Old || Exact - Old >= HeaderEnd)
        continue;
      return fail("code computes an address in the relocated Mach header");
    }
    return llvm::Error::success();
  }
  csh H = 0;
  if (cs_open(CS_ARCH_X86, CS_MODE_64, &H) != CS_ERR_OK)
    return fail("instruction decoder unavailable");
  auto Close = llvm::scope_exit([&] { cs_close(&H); });
  cs_option(H, CS_OPT_DETAIL, CS_OPT_ON);
  auto *I = cs_malloc(H);
  if (!I)
    return fail("instruction allocation failed");
  auto Free = llvm::scope_exit([&] { cs_free(I, 1); });
  const uint8_t *P = B.data() + Off;
  size_t Left = S.size;
  uint64_t PC = S.addr;
  while (Left) {
    if (!cs_disasm_iter(H, &P, &Left, &PC, I) || !I->detail)
      return fail("undecodable instruction section");
    auto &D = I->detail->x86;
    for (unsigned N = 0; N < D.op_count; ++N) {
      const auto &Op = D.operands[N];
      if (Op.type == X86_OP_IMM && uint64_t(Op.imm) >= Old &&
          uint64_t(Op.imm) - Old < HeaderEnd)
        return fail(
            "absolute/control reference into the header is not relocatable");
      if (Op.type != X86_OP_MEM || Op.mem.base != X86_REG_RIP)
        continue;
      const uint64_t Target = PC + Op.mem.disp;
      if (Target < Old || Target - Old >= HeaderEnd)
        continue;
      return fail("code computes an address in the relocated Mach header");
    }
  }
  return llvm::Error::success();
}
llvm::Error compact(Bytes &B, Arch TargetArch, uint64_t Off, uint64_t Size,
                    uint64_t Shift) {
  auto R = macho_unwind::parseCompactUnwindRaw(
      llvm::ArrayRef<uint8_t>(B).slice(Off, Size), {.TargetArch = TargetArch});
  if (!R)
    return R.takeError();
  std::set<uint64_t> Fields;
  for (unsigned I = 0; I < R->Header.PersonalityArrayCount; ++I)
    Fields.insert(R->Header.PersonalityArraySectionOffset + I * 4);
  for (unsigned I = 0; I < R->Index.size(); ++I)
    Fields.insert(R->Header.IndexSectionOffset + I * 12);
  for (unsigned I = 0; I + 1 < R->Index.size(); ++I)
    for (auto O = R->Index[I].LSDAIndexArraySectionOffset;
         O < R->Index[I + 1].LSDAIndexArraySectionOffset; O += 8) {
      Fields.insert(O);
      Fields.insert(O + 4);
    }
  for (const auto &P : R->Pages)
    if (P.Kind == 2)
      for (unsigned I = 0; I < P.EntryCount; ++I)
        Fields.insert(P.SectionOffset + P.EntryPageOffset + I * 8);
  for (auto O : Fields) {
    const auto V = readLE<uint32_t>(B.data() + Off + O);
    if (V > UINT32_MAX - Shift)
      return fail("compact-unwind RVA overflow");
    writeLE<uint32_t>(B.data() + Off + O, V + Shift);
  }
  auto Check = macho_unwind::parseCompactUnwindRaw(
      llvm::ArrayRef<uint8_t>(B).slice(Off, Size), {.TargetArch = TargetArch});
  if (!Check)
    return Check.takeError();
  return llvm::Error::success();
}
} // namespace

llvm::Expected<MachODependencyState> slideMachOZeroBaseDylib(Bytes &Binary,
                                                             uint64_t Slide) {
  namespace arm64e = macho_loader::arm64e;
  MachOChainedFixups Chains;
  auto Before = readMachODependencyState(Binary, &Chains);
  if (!Before)
    return Before.takeError();
  const auto H = parseMachOHeader(Binary.data(), Binary.size());
  if (!H.Is64 || H.FileType != MH_DYLIB || Before->CPUType != CPU_TYPE_ARM64 ||
      (Before->CPUSubtype & ~CPU_SUBTYPE_MASK) != CPU_SUBTYPE_ARM64E ||
      Chains.HeaderVA || !Chains.CommandOffset || Chains.Threaded)
    return fail(kSlideImage);
  // Encoded binds are independent of the preferred base. Rebases in
  // userland offset formats and authenticated rebases are image-relative.
  for (const auto &Segment : Chains.Segments) {
    for (const auto &Slot : Segment.Slots) {
      const auto Format = arm64e::chainFormat(Segment.PointerFormat);
      if (Format) {
        const auto W = arm64e::decodePointerWord(Slot.Raw, *Format);
        if (W && (W->Bind || W->Authenticated || Format->Offset ||
                  Format->Segmented))
          continue;
      } else if (Segment.PointerFormat == DYLD_CHAINED_PTR_64_OFFSET) {
        continue;
      }
      return fail(kSlideImage);
    }
  }
  const symtab_command *Sym = nullptr;
  const uint64_t HeaderEnd = H.HeaderSize + H.SizeOfCmds;
  auto Candidate = Binary;
  for (const auto &Segment : Chains.Segments) {
    auto *Command = Candidate.data() + Segment.CommandOffset;
    auto Fields = readMachOSegment(Command, true);
    if (Fields.VMAddr > UINT64_MAX - Fields.VMSize ||
        Fields.VMAddr + Fields.VMSize > UINT64_MAX - Slide)
      return fail(kSlideOverflow);
    const bool Text =
        readMachOName(Fields.SegName) == section_names::macho::TextSeg;
    auto *S =
        reinterpret_cast<section_64 *>(Command + sizeof(segment_command_64));
    uint64_t Cursor = HeaderEnd;
    for (unsigned I = 0; I < Fields.NSects; ++I) {
      if (Text && S[I].size && !isVirtualSection(S[I].flags & SECTION_TYPE)) {
        if (S[I].offset < Cursor ||
            !llvm::all_of(
                llvm::ArrayRef(Candidate).slice(Cursor, S[I].offset - Cursor),
                [](uint8_t Byte) { return Byte == 0; }))
          return fail(kSlideOpaque);
        Cursor = S[I].offset + S[I].size;
      }
      if (S[I].addr > UINT64_MAX - Slide)
        return fail(kSlideOverflow);
      S[I].addr += Slide;
    }
    const uint64_t End = Segment.FileOff + Segment.FileSize;
    if (Text &&
        (Cursor > End ||
         !llvm::all_of(llvm::ArrayRef(Candidate).slice(Cursor, End - Cursor),
                       [](uint8_t Byte) { return Byte == 0; })))
      return fail(kSlideOpaque);
    Fields.VMAddr += Slide;
    writeMachOSegment(Command, true, Fields);
  }
  bool ClassicDyld = false;
  forEachMachOLoadCommand(Binary.data(), Binary.size(),
                          [&](const uint8_t *C, uint32_t Cmd, uint32_t, bool) {
                            if (Cmd == LC_SYMTAB)
                              Sym = reinterpret_cast<const symtab_command *>(C);
                            ClassicDyld |=
                                Cmd == LC_DYLD_INFO || Cmd == LC_DYLD_INFO_ONLY;
                          });
  if (ClassicDyld)
    return fail(kSlideImage);
  if (Sym) {
    auto *Symbols =
        reinterpret_cast<nlist_64 *>(Candidate.data() + Sym->symoff);
    for (unsigned I = 0; I < Sym->nsyms; ++I) {
      auto &N = Symbols[I];
      if (N.n_type & N_STAB)
        return fail(kSlideSymbol);
      if ((N.n_type & N_TYPE) != N_SECT)
        continue;
      if (!N.n_sect || N.n_value > UINT64_MAX - Slide)
        return fail(kSlideSymbol);
      N.n_value += Slide;
    }
  }
  auto After = readMachODependencyState(Candidate);
  if (!After)
    return After.takeError();
  Binary.swap(Candidate);
  return After;
}

llvm::Expected<bool> reserveMachOHeaderStorage(Bytes &Binary, Arch A,
                                               uint64_t Extra) {
  if (Extra > ::neverd::limits::backend::kMaxMachOHeaderGrowthBytes)
    return fail("command request exceeds checked bounds");
  MachOHeaderInfo H;
  std::vector<macho_patch_detail::MachOFileRange> Segments, Sections;
  if (!macho_patch_detail::validateLoadCommandRegion(Binary, H) ||
      !macho_patch_detail::collectMachOFileRanges(Binary, Segments, Sections))
    return fail("invalid original layout");
  const uint64_t End = H.HeaderSize + H.SizeOfCmds;
  uint64_t First = Binary.size();
  for (auto R : Sections)
    First = std::min(First, R.Begin);
  // Zero bytes can still be live loader metadata. Authenticate all external
  // payload owners before treating a header gap as disposable command space.
  std::vector<macho_patch_detail::MachOFileRange> Payloads;
  const bool ValidPayloads =
      macho_patch_detail::collectMachOPayloadRanges(Binary, Payloads);
  bool Overlap = false;
  for (const auto &Range : Payloads) {
    First = std::min(First, Range.Begin);
    Overlap |= Range.Begin < End && End < Range.End;
  }
  if (!ValidPayloads || Overlap)
    return fail("unmodeled or overlapping load-command payload");
  if (End + Extra <= First &&
      llvm::all_of(llvm::ArrayRef<uint8_t>(Binary).slice(End, Extra),
                   [](uint8_t V) { return !V; }))
    return false;
  const auto FileType = H.FileType;
  const bool Executable = FileType == MH_EXECUTE;
  if (!H.Is64 || (A != Arch::X64 && A != Arch::AArch64) ||
      (Executable ? !(H.Flags & MH_PIE)
                  : FileType != MH_DYLIB && FileType != MH_BUNDLE))
    return fail(kImageKind);
  const uint64_t Page = A == Arch::AArch64 ? 16384 : 4096;
  const uint64_t Shift = llvm::alignTo(End + Extra, Page);
  if (Shift > UINT32_MAX || Binary.size() > UINT32_MAX - Shift)
    return fail("expanded file exceeds native offsets");
  Bytes B(Shift, 0);
  B.insert(B.end(), Binary.begin(), Binary.end());
  std::copy_n(Binary.begin(), End, B.begin());
  uint64_t Text = UINT64_MAX, Zero = UINT64_MAX, LE = UINT64_MAX, OldBase = 0;
  bool Bad = false, Dyld = false;
  std::vector<uint64_t> Commands;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *P, uint32_t ID, uint32_t, bool) {
        const auto Off = P - Binary.data();
        Commands.push_back(Off);
        Dyld |= ID == LC_DYLD_INFO || ID == LC_DYLD_INFO_ONLY ||
                ID == LC_DYLD_CHAINED_FIXUPS;
        if (ID != LC_SEGMENT_64)
          return;
        auto S = readMachOSegment(P, true);
        auto Name = readMachOName(S.SegName);
        if (Name == "__TEXT") {
          Bad |= Text != UINT64_MAX;
          Text = Off;
          OldBase = S.VMAddr;
          Bad |= S.FileOff || !S.FileSize || S.MaxProt != 5 || S.InitProt != 5;
        }
        if (Name == "__PAGEZERO") {
          Bad |= Zero != UINT64_MAX;
          Zero = Off;
          Bad |= S.FileOff || S.FileSize || S.VMAddr || S.InitProt ||
                 S.MaxProt || S.NSects;
        }
        if (Name == "__LINKEDIT") {
          Bad |= LE != UINT64_MAX;
          LE = Off;
          Bad |= S.FileOff + S.FileSize != Binary.size() || S.NSects;
        }
      });
  if (!Executable && !Dyld)
    return fail(kDyldPrefix);
  if (Bad || Text == UINT64_MAX || LE == UINT64_MAX || OldBase % Page ||
      OldBase < Shift ||
      (Executable && (Zero == UINT64_MAX || OldBase - Shift < Page)) ||
      (Zero != UINT64_MAX &&
       readMachOSegment(Binary.data() + Zero, true).VMSize != OldBase))
    return fail(kPrefixRange);
  std::vector<std::pair<uint64_t, uint64_t>> Code;
  for (const auto &R : Sections) {
    const auto &S =
        *reinterpret_cast<const section_64 *>(Binary.data() + R.HeaderOff);
    if ((S.flags & S_ATTR_SOME_INSTRUCTIONS) &&
        !(S.flags & S_ATTR_PURE_INSTRUCTIONS))
      return fail("mixed code/data sections lack a relocation contract");
    if (S.flags & S_ATTR_PURE_INSTRUCTIONS)
      Code.push_back({S.addr, S.size});
  }
  auto InCode = [&](uint64_t VA) {
    return llvm::any_of(
        Code, [&](auto R) { return VA >= R.first && VA - R.first < R.second; });
  };
  bool Starts = false;
  for (const auto Off : Commands) {
    const auto ID = readLE<uint32_t>(Binary.data() + Off);
    if (ID == LC_DATA_IN_CODE &&
        reinterpret_cast<const linkedit_data_command *>(Binary.data() + Off)
            ->datasize)
      return fail("embedded code data needs producer relocation evidence");
    if (ID == LC_MAIN &&
        !InCode(OldBase + reinterpret_cast<const entry_point_command *>(
                              Binary.data() + Off)
                              ->entryoff))
      return fail("entry point is outside an instruction section");
    if (ID != LC_FUNCTION_STARTS)
      continue;
    const auto *L =
        reinterpret_cast<const linkedit_data_command *>(Binary.data() + Off);
    if (!rangeInBounds(L->dataoff, L->datasize, Binary.size()))
      return fail("invalid function-start storage");
    auto Raw = llvm::ArrayRef<uint8_t>(Binary).slice(L->dataoff, L->datasize);
    size_t At = 0;
    uint64_t VA = OldBase;
    bool Done = false;
    while (At < Raw.size()) {
      unsigned N = 0;
      const char *Err = nullptr;
      auto V = llvm::decodeULEB128(Raw.data() + At, &N, Raw.end(), &Err);
      if (Err || !N)
        return fail("invalid function-start encoding");
      At += N;
      if (!V) {
        Done = true;
        break;
      }
      if (V > UINT64_MAX - VA || !InCode(VA += V))
        return fail("function is outside an instruction section");
      Starts = true;
    }
    if (!Done || llvm::any_of(Raw.drop_front(At), [](uint8_t V) { return V; }))
      return fail("invalid function-start terminator");
  }
  if (!Starts)
    return fail("header relocation requires typed function-start coverage");
  std::optional<MachODependencyState> Before;
  MachOChainedFixups Chains;
  if (Dyld) {
    auto S = readMachODependencyState(Binary, &Chains);
    if (!S)
      return S.takeError();
    Before = std::move(*S);
  }
  const auto OldLE = readMachOSegment(Binary.data() + LE, true);
  auto ShiftOff = [&](uint32_t &Off, uint64_t Size) {
    if (!Size) {
      if (Off) {
        if (Off > Binary.size())
          Bad = true;
        else
          Off += Shift;
      }
      return;
    }
    if (Off < End || !rangeInBounds(Off, Size, Binary.size()))
      Bad = true;
    else
      Off += Shift;
  };
  auto Append = [&](llvm::ArrayRef<uint8_t> Data) -> uint32_t {
    // dyld requires pointer-aligned LINKEDIT payloads in 64-bit images.
    // Re-encoded ULEB records do not necessarily end on that boundary.
    const auto Off = llvm::alignTo(B.size(), 8);
    if (Off > UINT32_MAX || Data.size() > UINT32_MAX - Off) {
      Bad = true;
      return 0;
    }
    B.resize(Off, 0);
    B.insert(B.end(), Data.begin(), Data.end());
    return Off;
  };
  const auto RelocateExports =
      [&](llvm::ArrayRef<uint8_t> Raw) -> llvm::Expected<Bytes> {
    auto Exports = readMachOExportLayout(Raw);
    if (!Exports)
      return Exports.takeError();
    for (auto &[Name, Entry] : *Exports) {
      if ((Entry.Flags & EXPORT_SYMBOL_FLAGS_REEXPORT) ||
          (Entry.Flags & EXPORT_SYMBOL_FLAGS_KIND_MASK) ==
              EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE)
        continue;
      const auto Relocate = [&](uint64_t &Offset) -> llvm::Error {
        if (Offset < End || Offset > UINT64_MAX - Shift)
          return fail(kExportRange);
        Offset += Shift;
        return llvm::Error::success();
      };
      if (Entry.Address || !isMachOHeaderSymbol(Name, FileType))
        if (auto E = Relocate(Entry.Address))
          return E;
      if (Entry.Flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER)
        if (auto E = Relocate(Entry.Resolver))
          return E;
    }
    return writeMachOExportLayout(*Exports);
  };
  for (auto Off : Commands) {
    const uint32_t ID = readLE<uint32_t>(B.data() + Off);
    switch (ID) {
    case LC_SEGMENT_64: {
      const auto Original = readMachOSegment(Binary.data() + Off, true);
      auto S = Original;
      if (Off == Text) {
        S.VMAddr -= Shift;
        S.VMSize += Shift;
        S.FileSize += Shift;
      } else if (Off == Zero)
        S.VMSize -= Shift;
      else if (S.FileOff)
        S.FileOff += Shift;
      writeMachOSegment(B.data() + Off, true, S);
      for (unsigned I = 0; I < S.NSects; ++I) {
        const auto SO =
            Off + sizeof(segment_command_64) + I * sizeof(section_64);
        const auto &Old =
            *reinterpret_cast<const section_64 *>(Binary.data() + SO);
        if (Old.nreloc || Old.reloff)
          return fail("classic section relocations need a relocation contract");
        auto *Now = reinterpret_cast<section_64 *>(B.data() + SO);
        if (Old.offset)
          Now->offset += Shift;
        if (!isVirtualSection(Old.flags & SECTION_TYPE) && Old.size) {
          if (auto E = headerReferences(B, Old, OldBase, End, Shift, A))
            return E;
          if (readMachOName(Old.sectname) == "__unwind_info")
            if (auto E = compact(B, A, Old.offset + Shift, Old.size, Shift))
              return E;
        }
      }
      break;
    }
    case LC_MAIN:
      reinterpret_cast<entry_point_command *>(B.data() + Off)->entryoff +=
          Shift;
      break;
    case LC_SYMTAB: {
      auto *T = reinterpret_cast<symtab_command *>(B.data() + Off);
      ShiftOff(T->symoff, uint64_t(T->nsyms) * sizeof(nlist_64));
      ShiftOff(T->stroff, T->strsize);
      if (Bad)
        return fail("invalid native symbol extents");
      for (unsigned I = 0; I < T->nsyms; ++I) {
        auto *N = reinterpret_cast<nlist_64 *>(B.data() + T->symoff +
                                               uint64_t(I) * sizeof(nlist_64));
        if (N->n_strx >= T->strsize ||
            !std::memchr(B.data() + T->stroff + N->n_strx, 0,
                         T->strsize - N->n_strx))
          return fail("invalid symbol name");
        if (N->n_value < OldBase || N->n_value - OldBase >= End)
          continue;
        const llvm::StringRef Name(
            reinterpret_cast<const char *>(B.data() + T->stroff + N->n_strx));
        if (!isMachOHeaderSymbol(Name, FileType) || N->n_value != OldBase ||
            (N->n_type & N_TYPE) != N_SECT)
          return fail("unmodeled symbol inside the header");
        N->n_value -= Shift;
      }
      break;
    }
    case LC_DYSYMTAB: {
      auto *D = reinterpret_cast<dysymtab_command *>(B.data() + Off);
      if (D->ntoc || D->nmodtab || D->nextrefsyms || D->nextrel || D->nlocrel)
        return fail("classic external metadata is not modeled");
      ShiftOff(D->indirectsymoff, uint64_t(D->nindirectsyms) * 4);
      break;
    }
    case LC_DYLD_INFO:
    case LC_DYLD_INFO_ONLY: {
      auto *D = reinterpret_cast<dyld_info_command *>(B.data() + Off);
      ShiftOff(D->rebase_off, D->rebase_size);
      ShiftOff(D->bind_off, D->bind_size);
      ShiftOff(D->weak_bind_off, D->weak_bind_size);
      ShiftOff(D->lazy_bind_off, D->lazy_bind_size);
      const auto Original =
          *reinterpret_cast<const dyld_info_command *>(Binary.data() + Off);
      if (Original.export_size) {
        auto Enc = RelocateExports(llvm::ArrayRef<uint8_t>(Binary).slice(
            Original.export_off, Original.export_size));
        if (!Enc)
          return Enc.takeError();
        const auto NewOff = Append(*Enc);
        D = reinterpret_cast<dyld_info_command *>(B.data() + Off);
        D->export_off = NewOff;
        D->export_size = Enc->size();
      }
      break;
    }
    case LC_DYLD_EXPORTS_TRIE:
    case LC_FUNCTION_STARTS: {
      const auto &L =
          *reinterpret_cast<const linkedit_data_command *>(Binary.data() + Off);
      if (!L.datasize)
        break;
      if (L.dataoff < OldLE.FileOff ||
          !rangeInBounds(L.dataoff, L.datasize, Binary.size()))
        return fail("invalid relative-address metadata");
      auto Raw = llvm::ArrayRef<uint8_t>(Binary).slice(L.dataoff, L.datasize);
      Bytes Enc;
      if (ID == LC_DYLD_EXPORTS_TRIE) {
        auto V = RelocateExports(Raw);
        if (!V)
          return V.takeError();
        Enc = std::move(*V);
      } else {
        size_t At = 0;
        bool First = true, Done = false;
        while (At < Raw.size()) {
          unsigned N = 0;
          const char *Err = nullptr;
          auto V = llvm::decodeULEB128(Raw.data() + At, &N, Raw.end(), &Err);
          At += N;
          if (Err || !N || (Done && V))
            return fail("invalid function-start encoding");
          if (!V) {
            Done = true;
            uleb(Enc, 0);
            break;
          }
          if (First) {
            if (V > UINT64_MAX - Shift)
              return fail("function-start overflow");
            V += Shift;
            First = false;
          }
          uleb(Enc, V);
        }
        if (!Done ||
            llvm::any_of(Raw.drop_front(At), [](uint8_t V) { return V; }))
          return fail("invalid function-start terminator");
      }
      const auto NewOff = Append(Enc);
      auto *LNew = reinterpret_cast<linkedit_data_command *>(B.data() + Off);
      LNew->dataoff = NewOff;
      LNew->datasize = Enc.size();
      break;
    }
    case LC_DYLD_CHAINED_FIXUPS: {
      auto *L = reinterpret_cast<linkedit_data_command *>(B.data() + Off);
      ShiftOff(L->dataoff, L->datasize);
      break;
    }
    case LC_DATA_IN_CODE:
    case LC_CODE_SIGNATURE: {
      auto *L = reinterpret_cast<linkedit_data_command *>(B.data() + Off);
      ShiftOff(L->dataoff, L->datasize);
      if (Bad)
        return fail("invalid file metadata");
      if (ID == LC_DATA_IN_CODE) {
        if (L->datasize % sizeof(data_in_code_entry))
          return fail("invalid data-in-code table");
        for (unsigned I = 0; I < L->datasize; I += sizeof(data_in_code_entry)) {
          auto *D =
              reinterpret_cast<data_in_code_entry *>(B.data() + L->dataoff + I);
          ShiftOff(D->offset, D->length);
        }
      }
      break;
    }
    case LC_UUID:
    case LC_BUILD_VERSION:
    case LC_VERSION_MIN_MACOSX:
    case LC_VERSION_MIN_IPHONEOS:
    case LC_VERSION_MIN_TVOS:
    case LC_VERSION_MIN_WATCHOS:
    case LC_LOAD_DYLINKER:
    case LC_ID_DYLIB:
    case LC_LOAD_DYLIB:
    case LC_LOAD_WEAK_DYLIB:
    case LC_REEXPORT_DYLIB:
    case LC_LOAD_UPWARD_DYLIB:
    case LC_RPATH:
    case LC_SOURCE_VERSION:
      break;
    default:
      return fail("load command has no relocation contract: " +
                  llvm::Twine(ID));
    }
    if (Bad)
      return fail("invalid file-offset range");
  }
  // Legacy dyld rebases carry absolute pointers, not encoded chain words.
  if (Chains.CommandOffset) {
    if (auto E = relocateChainSlots(B, Chains, End, Shift))
      return E;
  } else if (Before)
    for (const auto &[K, V] : Before->Slots)
      if (K.second == 0 && readLE<uint64_t>(V.data()) == 1) {
        for (const auto &R : Segments) {
          const auto S = readMachOSegment(Binary.data() + R.HeaderOff, true);
          if (K.first >= S.VMAddr &&
              rangeInBounds(K.first - S.VMAddr, 8, S.FileSize)) {
            auto *P = B.data() + S.FileOff + Shift + K.first - S.VMAddr;
            if (readLE<uint64_t>(P) == OldBase)
              writeLE<uint64_t>(P, OldBase - Shift);
          }
        }
      }
  auto L = readMachOSegment(B.data() + LE, true);
  L.FileSize = B.size() - L.FileOff;
  L.VMSize = llvm::alignTo(L.FileSize, Page);
  writeMachOSegment(B.data() + LE, true, L);
  Segments.clear();
  Sections.clear();
  if (!macho_patch_detail::collectMachOFileRanges(B, Segments, Sections))
    return fail("expanded layout overlaps");
  if (Before) {
    auto After = readMachODependencyState(B);
    if (!After)
      return After.takeError();
    if (auto E = preserveMachODependencyState(*Before, *After))
      return E;
  }
  Binary.swap(B);
  return true;
}
} // namespace neverd
