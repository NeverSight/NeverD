//===- COFFRegistrationEHRange.cpp - PE32 callback code ownership --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Attribute disjoint table-owned callbacks to their registration function.
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/COFF/COFFSEH4Entry.h"

#include "llvm/ADT/STLExtras.h"

#include <tuple>

namespace neverd::coff_loader::registration_detail {

void recoverRegistrationCallbackRanges(ExceptionFunction &F,
                                       const BinaryImage &Img,
                                       const FunctionRangeMap &Functions,
                                       const RegistrationChainInfo &Chain) {
  // A proved installing prologue owns its straight-line body just as a
  // table-owned callback does. Immediate bytes can look like alignment
  // padding inside a large generated frame initializer; they cannot split
  // an instruction into a new function.
  if (Chain.RegistrationOffset && Chain.TryLevelOffset &&
      F.CodeRange.contains(Chain.ChainInstallVA)) {
    const auto Parent = Functions.find(Img, F.CodeRange.Begin, true);
    if (Parent && Parent->Begin == F.CodeRange.Begin)
      F.CodeRange.End = std::max(F.CodeRange.End, Parent->End);
  }
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

  // The shared prologue is no part of the frame it builds, and a tentative
  // range past a handler can hold it.
  const auto Prologue = getX86SEH4PrologueShape(Img, F);
  std::vector<ExceptionAddressRange> Ranges{F.CodeRange};
  for (va_t Address : Callbacks) {
    if (!isExecutableAddress(Img, Address) || Cleanups.count(Address))
      continue;
    va_t Limit = InvalidVA;
    if (const auto Next = Callbacks.upper_bound(Address);
        Next != Callbacks.end())
      Limit = std::min(Limit, *Next);
    if (const auto Next = Cleanups.upper_bound(Address); Next != Cleanups.end())
      Limit = std::min(Limit, *Next);
    const auto Range = Functions.findCallback(Img, Address, Limit);
    if (!Range ||
        (Address < F.CodeRange.Begin && Range->End >= F.CodeRange.Begin))
      continue;
    // The language table owns the callback entry, not any preceding bytes
    // that an inferred function range happens to include.
    va_t End = Range->End;
    if (Prologue && Address < Prologue->Helper.Begin)
      End = std::min(End, Prologue->Helper.Begin);
    Ranges.push_back({Address, End});
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
  const bool KnownSEH =
      (F.Personality == ExceptionPersonality::ExceptHandler3 &&
       F.Encoding == ExceptionEncoding::X86ScopeTableEH3) ||
      (F.Personality == ExceptionPersonality::ExceptHandler4 &&
       F.Encoding == ExceptionEncoding::X86ScopeTableEH4);
  const bool CleanupOnlyCxx =
      F.Encoding == ExceptionEncoding::X86CxxFuncInfo && F.Cxx &&
      F.Cxx->TryBlocks.empty() &&
      (F.Personality == ExceptionPersonality::CxxFrameHandler3 ||
       F.Personality == ExceptionPersonality::CxxFrameHandlerX86);
  size_t Work = 0;
  for (auto Chunk : Chunks) {
    if (((KnownSEH && !F.Cxx) || CleanupOnlyCxx) &&
        ((Chain.RegistrationOffset && Chain.TryLevelOffset) || Prologue)) {
      std::vector<va_t> Roots;
      if (Chunk.contains(Entry))
        Roots.push_back(Entry);
      for (va_t Callback : Callbacks)
        if (Chunk.contains(Callback))
          Roots.push_back(Callback);
      if (auto End = checkedRegistrationRangeEnd(Img, Chunk, Roots, Work))
        Chunk.End = *End;
    }
    if (Chunk.contains(Entry))
      F.CodeRange = Chunk;
    else
      F.FragmentRanges.push_back(Chunk);
  }
}

} // namespace neverd::coff_loader::registration_detail
