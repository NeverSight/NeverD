//===- COFFRegistrationEHRange.cpp - PE32 callback code ownership --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Attribute disjoint table-owned callbacks to their registration function.
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "llvm/ADT/STLExtras.h"

#include <tuple>

namespace neverd::coff_loader::registration_detail {

void recoverRegistrationCallbackRanges(ExceptionFunction &F,
                                       const BinaryImage &Img,
                                       const FunctionRangeMap &Functions,
                                       const RegistrationChainInfo &Chain) {
  std::set<va_t> Callbacks;
  std::set<va_t> Cleanups;
  for (const auto &Scope : Chain.Scopes) {
    if (Scope.FilterVA)
      Callbacks.insert(Scope.FilterVA);
    if (Scope.HandlerVA)
      Callbacks.insert(Scope.HandlerVA);
  }
  if (F.Cxx) {
    for (const auto &Try : F.Cxx->TryBlocks)
      for (const auto &Catch : Try.Handlers)
        if (Catch.HandlerVA)
          Callbacks.insert(Catch.HandlerVA);
    for (const auto &Action : F.Cxx->UnwindMap)
      if (Action.ActionVA)
        Cleanups.insert(Action.ActionVA);
  }

  std::vector<ExceptionAddressRange> Ranges{F.CodeRange};
  for (va_t Address : Callbacks) {
    if (!isExecutableAddress(Img, Address) || Cleanups.count(Address))
      continue;
    const auto Range = Functions.find(Img, Address, true);
    if (!Range ||
        (Address < F.CodeRange.Begin && Range->End >= F.CodeRange.Begin))
      continue;
    // The language table owns the callback entry, not any preceding bytes
    // that an inferred function range happens to include.
    Ranges.push_back({Address, Range->End});
  }
  for (auto &Range : Ranges) {
    // A cleanup has its own establisher-frame ABI. It remains an external
    // callee even when it lies between two catches of the same function.
    auto Cleanup = Cleanups.lower_bound(Range.Begin);
    if (Cleanup != Cleanups.end())
      Range.End = std::min(Range.End, *Cleanup);
  }
  llvm::sort(Ranges, [](const auto &A, const auto &B) {
    return std::tie(A.Begin, A.End) < std::tie(B.Begin, B.End);
  });
  std::vector<ExceptionAddressRange> Chunks;
  for (const auto &Range : Ranges) {
    if (!Range.isValid())
      continue;
    if (!Chunks.empty() && Range.Begin <= Chunks.back().End)
      Chunks.back().End = std::max(Chunks.back().End, Range.End);
    else
      Chunks.push_back(Range);
  }
  const va_t Entry = F.CodeRange.Begin;
  F.FragmentRanges.clear();
  for (const auto &Chunk : Chunks)
    if (Chunk.contains(Entry))
      F.CodeRange = Chunk;
    else
      F.FragmentRanges.push_back(Chunk);
}

} // namespace neverd::coff_loader::registration_detail
