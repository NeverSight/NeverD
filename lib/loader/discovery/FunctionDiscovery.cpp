//===- FunctionDiscovery.cpp - Heuristic function start detection ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/FunctionDiscovery.h"

#include "FunctionDiscoveryDetail.h"

#include "neverd/loader/LinkerThunk.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"
#include "neverd/support/Limits.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <set>

#define DEBUG_TYPE "neverd-func-discovery"

namespace neverd {

const Import *BinaryImage::decodeImportThunkAt(va_t Addr) const {
  if (Addr == 0 || Addr == InvalidVA || Imports.empty())
    return nullptr;
  const Segment *Seg = getSegmentFor(Addr);
  if (!Seg || !Seg->isExecutable())
    return nullptr;

  auto ByIAT = [&](va_t Slot) -> const Import * {
    if (Slot == 0)
      return nullptr;
    for (const Import &Imp : Imports)
      if (Imp.IATAddr == Slot)
        return &Imp;
    return nullptr;
  };

  if (decodesImportThunks()) {
    if (!hasExecutableCodeOwnerRange(Addr, x86::kJmpIndirectLen))
      return nullptr;
    const uint8_t *Bytes = readVA(Addr, x86::kJmpIndirectLen);
    if (!Bytes || Bytes[0] != x86::kJmpIndirectOp)
      return nullptr;
    // An i386 PIC PLT entry jumps through its GOT entry by the base the
    // dynamic section names, DT_PLTGOT, which its caller holds in EBX.
    if (Arch == Arch::X86 && Bytes[1] == x86::kJmpIndirectGOTModRM) {
      if (!DynInfo.PltGotAddr)
        return nullptr;
      int32_t Disp = 0;
      std::memcpy(&Disp, Bytes + x86::kJmpIndirectDispOffset, sizeof(Disp));
      return ByIAT(static_cast<va_t>(static_cast<uint32_t>(
          DynInfo.PltGotAddr + static_cast<int64_t>(Disp))));
    }
    if (Bytes[1] != x86::kJmpIndirectModRM)
      return nullptr;
    va_t Slot = 0;
    if (Arch == Arch::X64) {
      int32_t Disp = 0;
      std::memcpy(&Disp, Bytes + x86::kJmpIndirectDispOffset, sizeof(Disp));
      Slot = Addr + x86::kJmpIndirectLen + static_cast<int64_t>(Disp);
    } else {
      uint32_t Abs = 0;
      std::memcpy(&Abs, Bytes + x86::kJmpIndirectDispOffset, sizeof(Abs));
      Slot = Abs;
    }
    return ByIAT(Slot);
  }
  return nullptr;
}

// ===--------------------------------------------------------------------===//
// Import thunk scanning — shared by COFF (IAT thunks) and ELF (PLT stubs)
// ===--------------------------------------------------------------------===//
//
// The per-architecture machine-code recognition lives in
// FunctionDiscovery{X86,AArch64,ARM}.cpp; this dispatcher only builds the
// import-target map and routes each executable segment by Arch.

void registerLoaderRunFunctions(BinaryImage &Img) {
  std::set<va_t> Functions;
  for (const auto &Sym : Img.Symbols)
    if (Sym.IsFunc)
      Functions.insert(Sym.Addr);
  const DynamicInfo &Dynamic = Img.DynInfo;
  std::vector<va_t> Entries{Dynamic.InitAddr, Dynamic.FiniAddr};
  for (const auto *Array :
       {&Dynamic.PreinitArray, &Dynamic.InitArray, &Dynamic.FiniArray})
    Entries.insert(Entries.end(), Array->begin(), Array->end());
  [[maybe_unused]] size_t Added = 0;
  for (va_t Addr : Entries) {
    // The loaders record only executable code as runtime-called.
    if (!Addr || !Img.isRuntimeFunctionAt(Addr) ||
        !Functions.insert(Addr).second)
      continue;
    Img.Symbols.push_back(Symbol::makeFunc(Addr));
    ++Added;
  }
  LLVM_DEBUG(llvm::dbgs() << "func-discovery: registered " << Added
                          << " loader-run functions\n");
}

ImportThunkCandidates::ImportThunkCandidates(const BinaryImage &Img) {
  Entries.reserve(Img.Symbols.size() + Img.COFFPDataRecords.size());
  Bodies.reserve(Img.KnownCodeRanges.size() + Img.Symbols.size() +
                 Img.COFFPDataRecords.size());
  // One ELF FDE can cover a whole PLT, including several callable veneers.
  // Only a wholly contained range has that loader-owned role. Stated function
  // symbols still own their bodies, even when they overlap such a range.
  const auto IsPLT = [&](va_t Start, va_t End) {
    return Img.isELF() && Start < End &&
           llvm::any_of(
               Img.ImportStubRanges,
               [&](const auto &Range) {
                 return Range.first <= Start && End <= Range.second;
               });
  };
  const auto Add = [&](va_t Start, va_t End) {
    if (Start >= End || End == InvalidVA)
      return;
    Bodies.emplace_back(Start, End);
  };
  for (const auto &[Start, End] : Img.KnownCodeRanges)
    if (!IsPLT(Start, End))
      Add(Start, End);
  for (const Symbol &Sym : Img.Symbols) {
    if (!Sym.IsFunc)
      continue;
    const va_t Start = normalizeCodeAddress(Sym.Addr, Img.Arch, Img.Mode);
    Entries.push_back(Start);
    if (Sym.Size && Sym.Size < InvalidVA - Start &&
        (Sym.Origin != NameOrigin::Synthesized ||
         !IsPLT(Start, Start + Sym.Size)))
      Add(Start, Start + Sym.Size);
  }
  // Restricted PE loads keep the other unwind ranges in their raw table.
  for (const auto &Record : Img.COFFPDataRecords)
    if (Record.BeginRVA < Record.EndRVA &&
        Record.EndRVA < InvalidVA - Img.Base) {
      Entries.push_back(Img.Base + Record.BeginRVA);
      Add(Img.Base + Record.BeginRVA, Img.Base + Record.EndRVA);
    }
  llvm::sort(Entries);
  Entries.erase(std::unique(Entries.begin(), Entries.end()), Entries.end());
  llvm::sort(Bodies);
  size_t Count = 0;
  for (const auto &Range : Bodies) {
    if (Count && Range.first <= Bodies[Count - 1].second)
      Bodies[Count - 1].second =
          std::max(Bodies[Count - 1].second, Range.second);
    else
      Bodies[Count++] = Range;
  }
  Bodies.resize(Count);
}

bool ImportThunkCandidates::allows(va_t Start, uint64_t Size) const {
  if (!Size || Size >= InvalidVA - Start)
    return false;
  if (std::binary_search(Entries.begin(), Entries.end(), Start))
    return true;
  const auto Next = std::lower_bound(
      Bodies.begin(), Bodies.end(), Start + Size,
      [](const auto &Range, va_t End) { return Range.first < End; });
  return Next == Bodies.begin() || std::prev(Next)->second <= Start;
}

/// Pair each long-branch or interworking thunk a linker wrote that jumps to
/// an import's stub with that import: a Thumb tail call reaches an ARM PLT
/// entry through `__ThumbV7PILongThunk_puts`, a distant A64 call through
/// `__AArch64ADRPThunk_*`.  Only the forms a linker alone writes count
/// (linkerLongBranchThunkAt); a direct jump to a stub ends many a routine.
static size_t
pairLinkerThunksWithImports(BinaryImage &Img, std::set<va_t> &Existing,
                            const ImportThunkCandidates &Candidates) {
  if ((Img.Arch != Arch::ARM && Img.Arch != Arch::AArch64) ||
      Img.ImportStubIndices.empty())
    return 0;
  // A Thumb thunk may start at any halfword, an ARM or A64 one at a word.
  const size_t Step =
      Img.Arch == Arch::ARM ? arm::kThumbInsnSize : aarch64::kInsnSize;
  size_t Added = 0;
  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable())
      continue;
    for (size_t Offset = 0; Offset + Step <= Seg.Data.size(); Offset += Step) {
      const va_t VA = Seg.VA + Offset;
      for (const InstructionMode Mode :
           {InstructionMode::Thumb, InstructionMode::ARM}) {
        if (Img.Arch == Arch::AArch64 && Mode == InstructionMode::Thumb)
          continue;
        if (Img.Arch == Arch::ARM && Mode == InstructionMode::ARM &&
            VA % arm::kInsnSize != 0)
          continue;
        const std::optional<LinkerThunk> Thunk = linkerLongBranchThunkAt(
            Img, VA, Img.Arch == Arch::ARM ? Mode : InstructionMode::Default);
        if (!Thunk)
          continue;
        const auto Stub = Img.ImportStubIndices.find(Thunk->Target);
        if (Stub == Img.ImportStubIndices.end())
          continue;
        // Bytes known to run in the other instruction set are no such thunk.
        if (Img.Arch == Arch::ARM)
          if (const std::optional<InstructionMode> Known =
                  Img.instructionModeAt(VA);
              Known && *Known != Mode)
            continue;
        if (!Img.isCodeRange(VA, Thunk->Size) ||
            !Candidates.allows(VA, Thunk->Size))
          continue;
        Img.recordImportStub(VA, Stub->second);
        if (Existing.insert(VA).second) {
          Img.Symbols.push_back(Symbol::makeFunc(VA, Thunk->Size));
          ++Added;
        }
        break;
      }
    }
  }
  return Added;
}

void scanImportThunks(BinaryImage &Img) {
  std::map<va_t, size_t> TargetImports;
  for (size_t I = 0; I < Img.Imports.size(); ++I)
    if (Img.Imports[I].IATAddr != 0)
      TargetImports.try_emplace(Img.Imports[I].IATAddr, I);
  if (TargetImports.empty() && Img.RuntimeCallablePointerSlots.empty())
    return;

  auto Existing = Img.getSymbolAddresses();
  const ImportThunkCandidates Candidates(Img);
  [[maybe_unused]] size_t Added = 0;

  for (const auto &Seg : Img.Segments) {
    if (!Seg.isExecutable())
      continue;
    switch (Img.Arch) {
    case Arch::X64:
    case Arch::X86:
      Added +=
          scanImportThunksX86(Img, Seg, TargetImports, Existing, Candidates);
      break;
    case Arch::AArch64:
      Added += scanImportThunksAArch64(Img, Seg, TargetImports, Existing,
                                       Candidates);
      break;
    case Arch::ARM:
      Added +=
          scanImportThunksARM(Img, Seg, TargetImports, Existing, Candidates);
      break;
    default:
      break;
    }
  }
  Added += pairLinkerThunksWithImports(Img, Existing, Candidates);
  LLVM_DEBUG(llvm::dbgs() << "func-discovery: import thunk scan added " << Added
                          << " functions\n");
}

/// The alignment of an address a prologue can begin at on \p A.  ARM probing
/// accepts both ARM and Thumb, so Thumb's halfword alignment is kept.
static size_t prologueAlignment(Arch A) {
  return A == Arch::AArch64 ? aarch64::kInsnSize
         : A == Arch::ARM   ? arm::kThumbInsnSize
                            : 1;
}

bool checkPrologueAtOffset(const Segment &Seg, size_t Off, Arch A) {
  if (Off >= Seg.Data.size() || Off > InvalidVA - Seg.VA)
    return false;
  const va_t Address = Seg.VA + Off;
  // Byte-wise padding scans can stop inside an instruction. Check the guest
  // address, not the buffer offset.
  const size_t Alignment = prologueAlignment(A);
  if (Address == InvalidVA || Address % Alignment != 0)
    return false;
  return isPrologueAt(Seg.Data.data() + Off, Seg.Data.size() - Off, A);
}

static bool checkCodePrologueAtOffset(const BinaryImage &Img,
                                      const Segment &Seg, size_t Off, Arch A) {
  // Most padding boundaries are not prologues. Reject their bounded byte
  // probes before consulting image-wide ownership metadata.
  if (!checkPrologueAtOffset(Seg, Off, A))
    return false;
  const uint64_t ProbeSize = A == Arch::X64 || A == Arch::X86 ? 1
                             : A == Arch::Unknown             ? 0
                                                              : 4;
  return Img.hasExecutableCodeOwnerRange(Seg.VA + Off, ProbeSize);
}

/// Whether no two segments, and no two readable sections, of \p Img overlap:
/// an address then lies in at most one of each.
static bool hasDisjointOwners(const BinaryImage &Img) {
  auto Disjoint = [](std::vector<std::pair<va_t, va_t>> Ranges) {
    llvm::sort(Ranges);
    for (size_t I = 1; I < Ranges.size(); ++I)
      if (Ranges[I].first < Ranges[I - 1].second)
        return false;
    return true;
  };
  std::vector<std::pair<va_t, va_t>> Segments, Sections;
  for (const Segment &Seg : Img.Segments) {
    if (Seg.Size > InvalidVA - Seg.VA)
      return false;
    Segments.emplace_back(Seg.VA, Seg.VA + Seg.Size);
  }
  for (const Section &Sec : Img.Sections) {
    if (!Sec.isReadable())
      continue;
    if (Sec.Size > InvalidVA - Sec.VA)
      return false;
    Sections.emplace_back(Sec.VA, Sec.VA + Sec.Size);
  }
  return Disjoint(std::move(Segments)) && Disjoint(std::move(Sections));
}

static std::vector<std::pair<va_t, va_t>>
collectClaimedCodeRanges(const BinaryImage &Img) {
  std::vector<std::pair<va_t, va_t>> Known = Img.KnownCodeRanges, Sized;
  Known.insert(Known.end(), Img.ImportStubRanges.begin(),
               Img.ImportStubRanges.end());
  for (const Symbol &Sym : Img.Symbols) {
    if (!Sym.IsFunc || Sym.Size == 0 || Sym.Size > InvalidVA - Sym.Addr)
      continue;
    Sized.emplace_back(Sym.Addr, Sym.Addr + Sym.Size);
  }
  // Each part usually comes sorted -- the code ranges once an unwind table
  // is committed, the sized symbols in that table's order -- and merging
  // them is linear, where sorting the two runs together is not.
  if (!std::is_sorted(Known.begin(), Known.end()))
    llvm::sort(Known);
  if (!std::is_sorted(Sized.begin(), Sized.end()))
    llvm::sort(Sized);
  std::vector<std::pair<va_t, va_t>> Ranges(Known.size() + Sized.size());
  std::merge(Known.begin(), Known.end(), Sized.begin(), Sized.end(),
             Ranges.begin());

  std::vector<std::pair<va_t, va_t>> Merged;
  Merged.reserve(Ranges.size());
  for (const auto &Range : Ranges) {
    if (Merged.empty() || Range.first > Merged.back().second) {
      Merged.push_back(Range);
      continue;
    }
    Merged.back().second = std::max(Merged.back().second, Range.second);
  }
  return Merged;
}

void recordRelocatedLiteralTargets(BinaryImage &Img) {
  if ((Img.Arch != Arch::X86 && Img.Arch != Arch::X64) ||
      Img.CodePtrRelocSlots.empty())
    return;
  X86StringLiteralRuns Literals(Img);
  std::map<va_t, bool> Classified;
  std::vector<va_t> Objects;
  // Text alone is not enough: the shared literal decoder also requires every
  // path through its bytes and padding to end in a trap. Decide every target
  // before publishing any data objects or changing relocation classifications.
  for (va_t Slot : Img.CodePtrRelocSlots) {
    const uint8_t *Bytes = Img.readVA(Slot, Img.getPointerSize());
    if (!Bytes)
      continue;
    const va_t Addr = readPtr(Bytes, Img.is64Bit());
    const auto [It, Inserted] = Classified.try_emplace(Addr, false);
    if (!Inserted)
      continue;
    const Segment *Seg = Img.getSegmentFor(Addr);
    if (Seg && Seg->isExecutable() &&
        Literals.at(*Seg, static_cast<size_t>(Addr - Seg->VA))) {
      It->second = true;
      Objects.push_back(Addr);
    }
  }
  // With no proved literal, neither publication nor relocation reclassification
  // can change the image. Avoid building their image-wide code indices.
  if (Objects.empty())
    return;
  // Existing code metadata, including a function's exclusive end, has priority
  // over this proof. Reuse the shared rules used for debug data objects.
  recordDataInCode(Img, Objects);
  recordDataPointerTargets(Img, [&](va_t Addr) {
    const auto It = Classified.find(Addr);
    return It != Classified.end() && It->second && Img.DataInCode.count(Addr);
  });
}

static const void *findLoadByte(const uint8_t *Data, int Byte, size_t Size,
                                const LoadProgress &Progress) {
  if (!Progress)
    return std::memchr(Data, Byte, Size);
  for (size_t Done = 0; Done < Size;) {
    Progress.check();
    const size_t Count =
        std::min(limits::support::kLoadByteCheckpointBytes, Size - Done);
    if (const void *Found = std::memchr(Data + Done, Byte, Count))
      return Found;
    Done += Count;
  }
  return nullptr;
}

void scanPaddingBoundaries(BinaryImage &Img, const LoadProgress &Progress) {
  Progress.check();
  // LC_FUNCTION_STARTS lists every function a linked Mach-O image lays out,
  // so a byte between two of its starts belongs to the function before it.
  // Padding there begins no function: an `int3` byte inside an instruction
  // (`call` with a displacement of 0xffffcc9a) or among the jump tables a
  // function keeps in __text reads as padding, and the byte after it as a
  // prologue.
  if (Img.Format == BinaryFormat::MachO && !Img.IsRelocatable &&
      Img.MachOHasFunctionStarts)
    return;

  const auto Known = collectClaimedCodeRanges(Img);
  auto Existing = Img.getSymbolAddresses();

  const uint8_t PadByte = codePaddingByte(Img.Arch);
  const size_t Alignment = prologueAlignment(Img.Arch);
  // An x86 prologue is a single byte, and text is full of them.
  std::optional<X86StringLiteralRuns> Literals;
  if (Img.Arch == Arch::X86 || Img.Arch == Arch::X64)
    Literals.emplace(Img);

  [[maybe_unused]] size_t Added = 0;
  for (const auto &Seg : Img.Segments) {
    Progress.check();
    if (!Seg.isExecutable() || Seg.Data.size() < 4)
      continue;
    const uint8_t *D = Seg.Data.data();
    const size_t N = Seg.Data.size();
    auto Consider = [&](size_t I) {
      const va_t Addr = Seg.VA + I;
      if (checkCodePrologueAtOffset(Img, Seg, I, Img.Arch) &&
          !insideInterval(Known, Addr) && !(Literals && Literals->at(Seg, I)) &&
          Existing.insert(Addr).second) {
        Symbol Guess = Symbol::makeFunc(Addr);
        Guess.IsBoundaryGuess = true;
        Img.Symbols.push_back(std::move(Guess));
        ++Added;
      }
    };
    // A boundary is a byte that is no padding right after one that is, past
    // the padding the segment may open with.
    size_t First = 0;
    while (First < N && D[First] == PadByte) {
      if (First % limits::support::kLoadByteCheckpointBytes == 0)
        Progress.check();
      ++First;
    }
    if (Alignment == 1) {
      for (size_t From = First; From < N;) {
        Progress.check();
        const void *Pad = findLoadByte(D + From, PadByte, N - From, Progress);
        if (!Pad)
          break;
        size_t I = static_cast<size_t>(static_cast<const uint8_t *>(Pad) - D);
        while (I < N && D[I] == PadByte) {
          if (I % limits::support::kLoadByteCheckpointBytes == 0)
            Progress.check();
          ++I;
        }
        if (I >= N)
          break;
        Consider(I);
        From = I + 1;
      }
      continue;
    }
    // Only a boundary a prologue can begin at is looked at.
    const size_t Skew = static_cast<size_t>((Seg.VA + First + 1) % Alignment);
    for (size_t I = First + 1 + (Skew ? Alignment - Skew : 0); I < N;
         I += Alignment) {
      Progress.check();
      if (D[I - 1] == PadByte && D[I] != PadByte)
        Consider(I);
    }
  }
  LLVM_DEBUG(llvm::dbgs() << "func-discovery: padding scan added " << Added
                          << " functions\n");
}

void scanX86HotpatchEntries(BinaryImage &Img, const LoadProgress &Progress) {
  Progress.check();
  if (Img.Arch != Arch::X86 || Img.Format != BinaryFormat::COFF ||
      Img.IsRelocatable)
    return;
  const auto Known = collectClaimedCodeRanges(Img);
  auto Existing = Img.getSymbolAddresses();

  [[maybe_unused]] size_t Added = 0;
  for (const auto &Seg : Img.Segments) {
    Progress.check();
    if (!Seg.isExecutable())
      continue;
    const uint8_t *D = Seg.Data.data();
    const size_t N = Seg.Data.size();
    // Every entry begins with 8B, so only those bytes are examined.
    for (size_t Off = 0; Off < N; ++Off) {
      Progress.check();
      const void *Hit = findLoadByte(D + Off, 0x8B, N - Off, Progress);
      if (!Hit)
        break;
      Off = static_cast<size_t>(static_cast<const uint8_t *>(Hit) - D);
      if (!isX86HotpatchEntryAt(D, N, Off))
        continue;
      const va_t Addr = Seg.VA + Off;
      // A hotpatch entry is the compiler's own mark of a function start, so
      // unlike a guess after padding it is not flagged IsBoundaryGuess: a
      // direct jump to it is a tail call.
      if (insideInterval(Known, Addr) ||
          !Img.hasExecutableCodeOwnerRange(Addr, 3) ||
          !Existing.insert(Addr).second)
        continue;
      Img.Symbols.push_back(Symbol::makeFunc(Addr));
      ++Added;
    }
  }
  LLVM_DEBUG(llvm::dbgs() << "func-discovery: x86 hotpatch scan added " << Added
                          << " functions\n");
}

void scanDataFuncPointers(BinaryImage &Img, const LoadProgress &Progress) {
  Progress.check();
  const size_t PtrSize = Img.getPointerSize();
  if (PtrSize == 0)
    return;

  // LC_FUNCTION_STARTS is the linked Mach-O image's format-native function
  // identity table.  Scanning arbitrary read-only sections after it succeeds
  // is weaker evidence and is actively ambiguous: Objective-C and Swift
  // metadata store adjacent 32-bit relative fields which can look like an
  // aligned 64-bit absolute code pointer when read without their schema.  Keep
  // this heuristic for old/packed Mach-O images that lack a usable starts
  // stream, and for formats whose data pointer tables are not described by
  // that load command.
  if (Img.Format == BinaryFormat::MachO && !Img.IsRelocatable &&
      Img.MachOHasFunctionStarts)
    return;

  // Sized function symbols claim their whole body just as unwind-derived code
  // ranges do.  Relocatable objects often have the former but no unwind
  // metadata; without folding those extents into Known, an absolute jump table
  // makes every case label look like a new function when its bytes happen to
  // resemble a prologue.
  const auto Known = collectClaimedCodeRanges(Img);
  auto Existing = Img.getSymbolAddresses();

  auto InExecSeg = [&](va_t Addr) -> const Segment * {
    const auto *S = Img.getSegmentFor(Addr);
    // Most read-only data values are not prologue addresses. Reject their
    // bounded byte probes before consulting image-wide ownership metadata.
    if (!S || !S->isExecutable() ||
        !checkPrologueAtOffset(*S, static_cast<size_t>(Addr - S->VA), Img.Arch))
      return nullptr;
    return Img.hasExecutableCodeOwnerAt(Addr) ? S : nullptr;
  };

  // A value is a code pointer only if it lies in an executable segment; most
  // read-only data does not, and is rejected before any lookup.
  std::vector<const Segment *> ExecutableSegments;
  for (const Segment &Seg : Img.Segments)
    if (Seg.isExecutable())
      ExecutableSegments.push_back(&Seg);
  auto InExecutableSegment = [&](va_t Addr) {
    return llvm::any_of(ExecutableSegments, [&](const Segment *Seg) {
      return Seg->contains(Addr);
    });
  };
  // Every slot of one section, or of a segment no section describes, has the
  // same owner end, unless an ARM mapping symbol makes a slot data or
  // segments or sections overlap; otherwise it is looked up once per range.
  const bool OwnerPerRange = Img.Arch != Arch::ARM && hasDisjointOwners(Img);
  // A table of strings an executable section keeps points at data.
  std::optional<X86StringLiteralRuns> Literals;
  if (Img.Arch == Arch::X86 || Img.Arch == Arch::X64)
    Literals.emplace(Img);

  // Only a relocated slot confirms the pointer interpretation of its bytes.
  // An untyped integer or string can coincide with a prologue-looking address
  // inside another instruction. Keep that candidate as a boundary guess so
  // the same decoded ownership proof used for padding can disprove it.
  // Indices: the scan below appends symbols.
  std::map<va_t, size_t> Guesses;
  if (!Img.CodePtrRelocSlots.empty())
    for (size_t I = 0; I < Img.Symbols.size(); ++I)
      if (Img.Symbols[I].IsFunc && Img.Symbols[I].IsBoundaryGuess)
        Guesses.emplace(Img.Symbols[I].Addr, I);

  [[maybe_unused]] size_t Added = 0;
  auto ScanRange = [&](const Segment *Seg, va_t Start, uint64_t RequestedLen) {
    if (!Seg || !Seg->isReadable() || Seg->isWritable() ||
        Seg->Data.size() < PtrSize || Start < Seg->VA)
      return;
    const uint64_t StartOff64 = Start - Seg->VA;
    if (StartOff64 >= Seg->Data.size())
      return;
    const size_t StartOff = static_cast<size_t>(StartOff64);
    const size_t ScanLen = static_cast<size_t>(
        std::min<uint64_t>(RequestedLen, Seg->Data.size() - StartOff));
    if (ScanLen < PtrSize || ScanLen > InvalidVA - Start)
      return;
    const va_t End = Start + ScanLen;
    // Even overlapping or unreadable section metadata must not let a coarse
    // segment fallback reinterpret debug records as runtime pointer storage.
    for (const Section &Sec : Img.Sections)
      if (Sec.isDebugInfo() && Sec.Size &&
          (Sec.VA <= Start ? Start - Sec.VA < Sec.Size : Sec.VA < End))
        return;
    va_t Cur = Start;
    const uint64_t Misalignment = Cur % PtrSize;
    if (Misalignment != 0)
      Cur += PtrSize - Misalignment;
    std::optional<va_t> OwnerEnd;
    Progress.check();
    for (; Cur <= End && End - Cur >= PtrSize; Cur += PtrSize) {
      // Pointer scans can visit millions of slots. Poll by covered bytes,
      // preserving every semantic visit without calling the host per pointer.
      if (Cur % limits::support::kLoadByteCheckpointBytes == 0)
        Progress.check();
      if (!OwnerEnd || !OwnerPerRange)
        OwnerEnd = Img.mappedObjectOwnerEnd(Cur);
      if (!OwnerEnd || *OwnerEnd < Cur || PtrSize > *OwnerEnd - Cur)
        break;
      const size_t I = static_cast<size_t>(Cur - Seg->VA);
      uint64_t Val = normalizeCodeAddress(
          readPtr(Seg->Data.data() + I, Img.is64Bit()), Img.Arch, Img.Mode);
      // An indexed table's entries are its reader's case labels or
      // callbacks, not independent functions, whatever their first bytes
      // look like: a jump table in .rdata would otherwise promote every
      // switch case.
      if (Img.CodePtrRelocSlots.count(Cur)) {
        bool InIndexedRun = false;
        for (const auto &[Begin, End] : Img.IndexedTableRuns)
          if (Cur >= Begin && Cur < End) {
            InIndexedRun = true;
            break;
          }
        if (InIndexedRun)
          continue;
      }
      if (const auto Guess = Guesses.find(Val);
          Guess != Guesses.end() && Img.CodePtrRelocSlots.count(Cur)) {
        Img.Symbols[Guess->second].IsBoundaryGuess = false;
        Guesses.erase(Guess);
      }
      if (!InExecutableSegment(Val) || insideInterval(Known, Val) ||
          Existing.count(Val))
        continue;
      const auto *ESeg = InExecSeg(Val);
      if (!ESeg)
        continue;
      size_t Off = static_cast<size_t>(Val - ESeg->VA);
      if (!checkCodePrologueAtOffset(Img, *ESeg, Off, Img.Arch) ||
          (Literals && Literals->at(*ESeg, Off)))
        continue;
      if (!Existing.insert(Val).second)
        continue;
      auto Candidate = Symbol::makeFunc(Val);
      Candidate.IsBoundaryGuess = !Img.CodePtrRelocSlots.count(Cur);
      if (Candidate.IsBoundaryGuess && !Img.CodePtrRelocSlots.empty())
        Guesses.emplace(Val, Img.Symbols.size());
      Img.Symbols.push_back(std::move(Candidate));
      ++Added;
    }
  };

  for (const Section &Sec : Img.Sections)
    if (!Sec.isDebugInfo() && Sec.Size != 0 && Sec.isReadable() &&
        !Sec.isWritable() && !Img.isCodeAddress(Sec.VA))
      ScanRange(Img.getSegmentFor(Sec.VA), Sec.VA, Sec.Size);
  for (const Segment &Seg : Img.Segments) {
    Progress.check();
    if (section_names::isDebugSectionName(Seg.Name) || Seg.Name == "__DWARF" ||
        Img.segmentHasReadableSectionMetadata(Seg) || !Seg.isReadable() ||
        Seg.isExecutable() || Seg.isWritable())
      continue;
    ScanRange(&Seg, Seg.VA, Seg.Data.size());
  }
  LLVM_DEBUG(llvm::dbgs() << "func-discovery: data ptr scan added " << Added
                          << " functions\n");
}

} // namespace neverd
