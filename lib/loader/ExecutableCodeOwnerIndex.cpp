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
#include <limits>
#include <set>

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

template <typename Visitor>
void visitFunctionMetadataRanges(const BinaryImage &Image, Visitor Visit) {
  // InvalidVA means there is no finite ownership end, even if a sized symbol
  // reaches that address without overflowing.
  for (const auto &[Start, End] : Image.KnownCodeRanges)
    if (End > Start && End != InvalidVA)
      Visit(Start, End);
  for (const Symbol &Sym : Image.Symbols)
    if (Sym.IsFunc && Sym.Size != 0 && Sym.Size < InvalidVA - Sym.Addr)
      Visit(Sym.Addr, Sym.Addr + Sym.Size);
}

template <typename Visitor>
void visitFunctionFragments(const BinaryImage &Image, std::optional<va_t> Owner,
                            Visitor Visit) {
  const auto &Functions = Image.ExceptionMetadata.Functions;
  std::set<va_t> Primaries;
  for (const auto &Function : Functions)
    if (Function.Kind == RuntimeFunctionKind::Primary &&
        (!Owner || Function.CodeRange.Begin == *Owner))
      Primaries.insert(Function.CodeRange.Begin);
  if (Primaries.empty())
    return;
  for (const auto &Fragment : Functions) {
    if (Fragment.Kind == RuntimeFunctionKind::Primary ||
        Fragment.CodeRange.Begin >= Fragment.CodeRange.End)
      continue;
    if (Fragment.PrimaryFunctionIndex &&
        *Fragment.PrimaryFunctionIndex < Functions.size()) {
      const auto &Primary = Functions[*Fragment.PrimaryFunctionIndex];
      if (Primary.Kind == RuntimeFunctionKind::Primary &&
          (!Owner || Primary.CodeRange.Begin == *Owner))
        Visit(Primary.CodeRange.Begin, Fragment.CodeRange);
    }
    if (Fragment.ChainedPrimaryRange &&
        Primaries.count(Fragment.ChainedPrimaryRange->Begin))
      Visit(Fragment.ChainedPrimaryRange->Begin, Fragment.CodeRange);
  }
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
  visitFunctionMetadataRanges(Image, [&](va_t Start, va_t End) {
    FunctionMetadataEnds.emplace_back(Start, End);
  });
  std::sort(FunctionMetadataEnds.begin(), FunctionMetadataEnds.end());
  FunctionMetadataEnds.erase(std::unique(FunctionMetadataEnds.begin(),
                                         FunctionMetadataEnds.end(),
                                         [](const auto &A, const auto &B) {
                                           return A.first == B.first;
                                         }),
                             FunctionMetadataEnds.end());

  visitFunctionFragments(Image, std::nullopt, [&](va_t Owner, auto Range) {
    FunctionFragments.emplace_back(Owner, Range.Begin, Range.End);
  });
  std::sort(FunctionFragments.begin(), FunctionFragments.end());
  size_t Count = 0;
  for (const auto &[Owner, Begin, End] : FunctionFragments) {
    if (Count != 0 && Owner == std::get<0>(FunctionFragments[Count - 1]) &&
        Begin <= std::get<2>(FunctionFragments[Count - 1])) {
      auto &PreviousEnd = std::get<2>(FunctionFragments[Count - 1]);
      PreviousEnd = std::max(PreviousEnd, End);
    } else {
      FunctionFragments[Count++] = {Owner, Begin, End};
    }
  }
  FunctionFragments.resize(Count);
}

std::optional<size_t>
ExecutableCodeOwnerIndex::fragmentLookupWork(const BinaryImage &Image) const {
  if (this->Image != &Image)
    return std::nullopt;
  // Each binary-search comparison checks two address fields. Include the
  // image identity, endpoint lookup and final owner/range comparisons.
  size_t Work = 8;
  for (size_t N = FunctionFragments.size(); N != 0; N /= 2)
    Work += 4;
  return Work;
}

bool ExecutableCodeOwnerIndex::ownsFunctionFragment(va_t Entry,
                                                    va_t Target) const {
  const auto Key = std::pair{Entry, Target};
  auto It = std::upper_bound(FunctionFragments.begin(), FunctionFragments.end(),
                             Key, [](auto Address, const auto &Range) {
                               return Address < std::pair{std::get<0>(Range),
                                                          std::get<1>(Range)};
                             });
  return It != FunctionFragments.begin() && std::get<0>(*--It) == Entry &&
         Target < std::get<2>(*It);
}

bool isExplicitlyOwnedFunctionFragment(const BinaryImage &Image,
                                       va_t FunctionEntry, va_t Target,
                                       const ExecutableCodeOwnerIndex *Index) {
  if (Index && Index->Image == &Image)
    return Index->ownsFunctionFragment(FunctionEntry, Target);
  bool Found = false;
  visitFunctionFragments(Image, FunctionEntry, [&](va_t, auto Range) {
    Found |= Range.contains(Target);
  });
  return Found;
}

std::optional<size_t>
explicitFunctionFragmentLookupWork(const BinaryImage &Image,
                                   const ExecutableCodeOwnerIndex *Index) {
  if (Index)
    if (auto Work = Index->fragmentLookupWork(Image))
      return Work;
  const size_t Count = Image.ExceptionMetadata.Functions.size();
  size_t Lookup = 1;
  for (size_t N = Count; N > 1; N = N / 2 + N % 2)
    ++Lookup;
  const size_t PerEntry = Lookup * 2 + 3;
  if (Count > std::numeric_limits<size_t>::max() / PerEntry)
    return std::nullopt;
  return Count * PerEntry;
}

va_t ExecutableCodeOwnerIndex::getFunctionMetadataEnd(va_t Entry) const {
  const auto It = std::lower_bound(
      FunctionMetadataEnds.begin(), FunctionMetadataEnds.end(), Entry,
      [](const auto &Range, va_t Address) { return Range.first < Address; });
  if (It == FunctionMetadataEnds.end() || It->first != Entry)
    return InvalidVA;
  return It->second;
}

va_t BinaryImage::getFunctionMetadataEnd(
    va_t Entry, const ExecutableCodeOwnerIndex *Index) const {
  if (Index && Index->Image == this)
    return Index->getFunctionMetadataEnd(Entry);
  va_t End = InvalidVA;
  visitFunctionMetadataRanges(*this, [&](va_t Start, va_t Candidate) {
    if (Start == Entry)
      End = std::min(End, Candidate);
  });
  return End;
}

bool ExecutableCodeOwnerIndex::isImportStubAt(va_t Addr) const {
  return std::binary_search(ImportStubs.begin(), ImportStubs.end(), Addr) ||
         contains(ImportRanges, Addr);
}

bool ExecutableCodeOwnerIndex::hasFunctionSymbolAt(va_t Addr) const {
  return std::binary_search(FunctionStarts.begin(), FunctionStarts.end(), Addr);
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

bool BinaryImage::hasFunctionSymbolAt(
    va_t Addr, const ExecutableCodeOwnerIndex *Index) const {
  const va_t Normalized = normalizeCodeAddress(Addr, Arch, Mode);
  if (Index && Index->Image == this)
    return Index->hasFunctionSymbolAt(Normalized);
  for (const Symbol &Sym : Symbols)
    if (Sym.IsFunc && normalizeCodeAddress(Sym.Addr, Arch, Mode) == Normalized)
      return true;
  return false;
}

} // namespace neverd
