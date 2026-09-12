//===- ExecutableCodeOwnerIndex.cpp - Scoped code ownership --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/ExecutableCodeOwnerIndex.h"

#include "neverd/loader/BinaryImageModel.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace neverd {
namespace {

void sortUnique(std::vector<va_t> &Addresses) {
  std::sort(Addresses.begin(), Addresses.end());
  Addresses.erase(std::unique(Addresses.begin(), Addresses.end()),
                  Addresses.end());
}

void mergeRanges(std::vector<std::pair<va_t, va_t>> &Ranges) {
  Ranges.erase(std::remove_if(Ranges.begin(), Ranges.end(),
                              [](const auto &Range) {
                                return Range.first >= Range.second;
                              }),
               Ranges.end());
  std::sort(Ranges.begin(), Ranges.end());
  size_t Count = 0;
  for (const auto Range : Ranges) {
    if (Count != 0 && Range.first <= Ranges[Count - 1].second) {
      Ranges[Count - 1].second =
          std::max(Ranges[Count - 1].second, Range.second);
    } else {
      Ranges[Count++] = Range;
    }
  }
  Ranges.resize(Count);
}

bool contains(const std::vector<std::pair<va_t, va_t>> &Ranges, va_t Addr) {
  const auto It = std::upper_bound(
      Ranges.begin(), Ranges.end(), Addr,
      [](va_t Address, const auto &Range) { return Address < Range.first; });
  return It != Ranges.begin() && Addr < std::prev(It)->second;
}

} // namespace

ExecutableCodeOwnerIndex::ExecutableCodeOwnerIndex(const BinaryImage &Image)
    : Image(&Image), ImportRanges(Image.ImportStubRanges),
      CodeRanges(Image.KnownCodeRanges) {
  for (const auto &[Addr, Index] : Image.ImportStubIndices)
    if (Index < Image.Imports.size())
      ImportStubs.push_back(Addr);
  // Preserve the legacy Mach-O IAT spelling without promoting PE/ELF data
  // slots or Mach-O non-instruction sections in coarse executable mappings.
  if (Image.isMachO())
    for (const Import &Imp : Image.Imports)
      if (Image.isCodeAddress(Imp.IATAddr))
        ImportStubs.push_back(Imp.IATAddr);
  for (const Symbol &Sym : Image.Symbols) {
    if (!Sym.IsFunc)
      continue;
    const va_t Start = normalizeCodeAddress(Sym.Addr, Image.Arch, Image.Mode);
    FunctionStarts.push_back(Start);
    if (Sym.Size != 0 && Sym.Size <= InvalidVA - Start)
      CodeRanges.emplace_back(Start, Start + Sym.Size);
  }
  sortUnique(ImportStubs);
  sortUnique(FunctionStarts);
  mergeRanges(ImportRanges);
  mergeRanges(CodeRanges);
}

bool ExecutableCodeOwnerIndex::isImportStubAt(va_t Addr) const {
  return std::binary_search(ImportStubs.begin(), ImportStubs.end(), Addr) ||
         contains(ImportRanges, Addr);
}

bool ExecutableCodeOwnerIndex::hasKnownOrTypedOwnerAt(va_t Addr) const {
  return std::binary_search(FunctionStarts.begin(), FunctionStarts.end(),
                            Addr) ||
         contains(CodeRanges, Addr);
}

bool BinaryImage::hasExecutableCodeOwnerAt(
    va_t Addr, const ExecutableCodeOwnerIndex *Index) const {
  const va_t Normalized = normalizeCodeAddress(Addr, Arch, Mode);
  const Segment *Seg = getSegmentFor(Normalized);
  if (!Seg || !Seg->isExecutable())
    return false;
  if (isCodeAddress(Normalized) ||
      (Entry != 0 && normalizeCodeAddress(Entry, Arch, Mode) == Normalized) ||
      RuntimeFunctionAddrs.count(Addr) != 0 ||
      RuntimeFunctionAddrs.count(Normalized) != 0 ||
      VerifiedFunctionEntries.count(Normalized) != 0)
    return true;
  if (Index && Index->Image == this)
    return Index->isImportStubAt(Addr) || Index->isImportStubAt(Normalized) ||
           Index->hasKnownOrTypedOwnerAt(Normalized);

  if (isImportStubAt(Addr) || isImportStubAt(Normalized))
    return true;
  for (const auto &[Start, End] : KnownCodeRanges)
    if (Normalized >= Start && Normalized < End)
      return true;
  for (const Symbol &Sym : Symbols) {
    if (!Sym.IsFunc)
      continue;
    const va_t Start = normalizeCodeAddress(Sym.Addr, Arch, Mode);
    if (Normalized == Start)
      return true;
    if (Sym.Size != 0 && Sym.Size <= InvalidVA - Start && Normalized > Start &&
        Normalized < Start + Sym.Size)
      return true;
  }
  return false;
}

} // namespace neverd
