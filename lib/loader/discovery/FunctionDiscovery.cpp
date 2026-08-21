//===- FunctionDiscovery.cpp - Heuristic function start detection ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/FunctionDiscovery.h"

#include "FunctionDiscoveryDetail.h"

#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <set>

#define DEBUG_TYPE "neverd-func-discovery"

namespace neverd {

// ===--------------------------------------------------------------------===//
// Import thunk scanning — shared by COFF (IAT thunks) and ELF (PLT stubs)
// ===--------------------------------------------------------------------===//
//
// The per-architecture machine-code recognition lives in
// FunctionDiscovery{X86,AArch64,ARM}.cpp; this dispatcher only builds the
// import-target map and routes each executable segment by Arch.

void scanImportThunks(BinaryImage &Img) {
  std::map<va_t, size_t> TargetImports;
  for (size_t I = 0; I < Img.Imports.size(); ++I)
    if (Img.Imports[I].IATAddr != 0)
      TargetImports.try_emplace(Img.Imports[I].IATAddr, I);
  if (TargetImports.empty())
    return;

  auto Existing = Img.getSymbolAddresses();
  [[maybe_unused]] size_t Added = 0;

  for (const auto &Seg : Img.Segments) {
    if (!Seg.isExecutable())
      continue;
    switch (Img.Arch) {
    case Arch::X64:
    case Arch::X86:
      Added += scanImportThunksX86(Img, Seg, TargetImports, Existing);
      break;
    case Arch::AArch64:
      Added += scanImportThunksAArch64(Img, Seg, TargetImports, Existing);
      break;
    case Arch::ARM:
      Added += scanImportThunksARM(Img, Seg, TargetImports, Existing);
      break;
    default:
      break;
    }
  }
  LLVM_DEBUG(llvm::dbgs() << "func-discovery: import thunk scan added " << Added
                          << " functions\n");
}

bool checkPrologueAtOffset(const Segment &Seg, size_t Off, Arch A) {
  if (Off >= Seg.Data.size())
    return false;
  return isPrologueAt(Seg.Data.data() + Off, Seg.Data.size() - Off, A);
}

static bool checkCodePrologueAtOffset(const BinaryImage &Img,
                                      const Segment &Seg, size_t Off, Arch A) {
  if (Off >= Seg.Data.size())
    return false;
  const uint64_t ProbeSize = A == Arch::X64 || A == Arch::X86 ? 1
                             : A == Arch::Unknown             ? 0
                                                              : 4;
  if (!Img.hasExecutableCodeOwnerRange(Seg.VA + Off, ProbeSize))
    return false;
  return checkPrologueAtOffset(Seg, Off, A);
}

static std::vector<std::pair<va_t, va_t>>
collectClaimedCodeRanges(const BinaryImage &Img) {
  std::vector<std::pair<va_t, va_t>> Ranges = Img.KnownCodeRanges;
  for (const Symbol &Sym : Img.Symbols) {
    if (!Sym.IsFunc || Sym.Size == 0 || Sym.Size > InvalidVA - Sym.Addr)
      continue;
    Ranges.emplace_back(Sym.Addr, Sym.Addr + Sym.Size);
  }

  std::sort(Ranges.begin(), Ranges.end());
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

void scanPaddingBoundaries(BinaryImage &Img) {
  const auto Known = collectClaimedCodeRanges(Img);
  auto Existing = Img.getSymbolAddresses();

  const uint8_t PadByte = codePaddingByte(Img.Arch);
  auto IsPadByte = [&](uint8_t B) -> bool { return B == PadByte; };

  [[maybe_unused]] size_t Added = 0;
  for (const auto &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.size() < 4)
      continue;
    const uint8_t *D = Seg.Data.data();
    size_t N = Seg.Data.size();
    size_t I = 0;
    while (I < N && IsPadByte(D[I]))
      ++I;
    while (I + 1 < N) {
      if (!IsPadByte(D[I])) {
        ++I;
        continue;
      }
      while (I < N && IsPadByte(D[I]))
        ++I;
      if (I >= N)
        break;
      va_t Addr = Seg.VA + I;
      if (checkCodePrologueAtOffset(Img, Seg, I, Img.Arch) &&
          !insideInterval(Known, Addr) && Existing.insert(Addr).second) {
        Img.Symbols.push_back(Symbol::makeFunc(Addr));
        ++Added;
      }
    }
  }
  LLVM_DEBUG(llvm::dbgs() << "func-discovery: padding scan added " << Added
                          << " functions\n");
}

void scanDataFuncPointers(BinaryImage &Img) {
  const size_t PtrSize = Img.getPointerSize();
  if (PtrSize == 0)
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
    return (S && Img.hasExecutableCodeOwnerAt(Addr)) ? S : nullptr;
  };

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
    va_t Cur = Start;
    const uint64_t Misalignment = Cur % PtrSize;
    if (Misalignment != 0)
      Cur += PtrSize - Misalignment;
    for (; Cur <= End && End - Cur >= PtrSize; Cur += PtrSize) {
      const std::optional<va_t> OwnerEnd = Img.mappedObjectOwnerEnd(Cur);
      if (!OwnerEnd || *OwnerEnd < Cur || PtrSize > *OwnerEnd - Cur)
        break;
      const size_t I = static_cast<size_t>(Cur - Seg->VA);
      uint64_t Val = normalizeCodeAddress(
          readPtr(Seg->Data.data() + I, Img.is64Bit()), Img.Arch, Img.Mode);
      const auto *ESeg = InExecSeg(Val);
      if (!ESeg)
        continue;
      size_t Off = static_cast<size_t>(Val - ESeg->VA);
      if (!checkCodePrologueAtOffset(Img, *ESeg, Off, Img.Arch))
        continue;
      if (insideInterval(Known, Val))
        continue;
      if (!Existing.insert(Val).second)
        continue;
      Img.Symbols.push_back(Symbol::makeFunc(Val));
      ++Added;
    }
  };

  for (const Section &Sec : Img.Sections)
    if (Sec.Size != 0 && Sec.isReadable() && !Sec.isWritable() &&
        !Img.isCodeAddress(Sec.VA))
      ScanRange(Img.getSegmentFor(Sec.VA), Sec.VA, Sec.Size);
  for (const Segment &Seg : Img.Segments) {
    if (Img.segmentHasReadableSectionMetadata(Seg) || !Seg.isReadable() ||
        Seg.isExecutable() || Seg.isWritable())
      continue;
    ScanRange(&Seg, Seg.VA, Seg.Data.size());
  }
  LLVM_DEBUG(llvm::dbgs() << "func-discovery: data ptr scan added " << Added
                          << " functions\n");
}

} // namespace neverd
