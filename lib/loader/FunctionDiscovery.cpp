//===- FunctionDiscovery.cpp - Heuristic function start detection ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/FunctionDiscovery.h"

#include "FunctionDiscoveryDetail.h"

#include "neverd/Support/BinaryEncoding.h"

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
// import-target set and routes each executable segment by Arch.

void scanImportThunks(BinaryImage &Img) {
  std::set<va_t> TargetAddrs;
  for (const auto &Imp : Img.Imports)
    if (Imp.IATAddr)
      TargetAddrs.insert(Imp.IATAddr);
  if (TargetAddrs.empty())
    return;

  auto Existing = Img.getSymbolAddresses();
  [[maybe_unused]] size_t Added = 0;

  for (const auto &Seg : Img.Segments) {
    if (!Seg.isExecutable())
      continue;
    switch (Img.Arch) {
    case Arch::X64:
    case Arch::X86:
      Added += scanImportThunksX86(Img, Seg, TargetAddrs, Existing);
      break;
    case Arch::AArch64:
      Added += scanImportThunksAArch64(Img, Seg, TargetAddrs, Existing);
      break;
    case Arch::ARM:
      Added += scanImportThunksARM(Img, Seg, TargetAddrs, Existing);
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

void scanPaddingBoundaries(BinaryImage &Img) {
  const auto &Known = Img.KnownCodeRanges;
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
      if (checkPrologueAtOffset(Seg, I, Img.Arch) &&
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

  const auto &Known = Img.KnownCodeRanges;
  auto Existing = Img.getSymbolAddresses();

  auto InExecSeg = [&](va_t Addr) -> const Segment * {
    const auto *S = Img.getSegmentFor(Addr);
    return (S && S->isExecutable()) ? S : nullptr;
  };

  [[maybe_unused]] size_t Added = 0;
  for (const auto &Seg : Img.Segments) {
    if (!Seg.isReadable() || Seg.isExecutable() || Seg.isWritable())
      continue;
    if (Seg.Data.size() < PtrSize)
      continue;
    const uint8_t *D = Seg.Data.data();
    size_t N = Seg.Data.size();
    size_t Start = static_cast<size_t>(Seg.VA) % PtrSize;
    for (size_t I = Start; I + PtrSize <= N; I += PtrSize) {
      uint64_t Val = normalizeCodeAddress(readPtr(D + I, Img.is64Bit()),
                                          Img.Arch, Img.Mode);
      if (Val < Img.Base)
        continue;
      const auto *ESeg = InExecSeg(Val);
      if (!ESeg)
        continue;
      size_t Off = static_cast<size_t>(Val - ESeg->VA);
      if (!checkPrologueAtOffset(*ESeg, Off, Img.Arch))
        continue;
      if (insideInterval(Known, Val))
        continue;
      if (!Existing.insert(Val).second)
        continue;
      Img.Symbols.push_back(Symbol::makeFunc(Val));
      ++Added;
    }
  }
  LLVM_DEBUG(llvm::dbgs() << "func-discovery: data ptr scan added " << Added
                          << " functions\n");
}

} // namespace neverd
