//===- MedCodeIdentity.h - Retained machine code extents --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original block labels and function extents shared by LLVM scheduling and
/// code-address materialization. These extents do not prove pointer provenance.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDCODEIDENTITY_H
#define NEVERD_IR_MED_MEDCODEIDENTITY_H

#include "neverd/Common.h"
#include "neverd/ir/med/MedIR.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace neverd {

inline va_t medBlockCodeAddress(const MedBlock &Block) {
  return Block.StartAddr != 0 || Block.Ops.empty() ? Block.StartAddr
                                                   : Block.Ops.front().Addr;
}

inline std::vector<std::pair<va_t, va_t>>
medFunctionCodeRanges(const MedFunc &Func) {
  std::vector<std::pair<va_t, va_t>> Ranges;
  const auto Add = [&](va_t Begin, va_t End) {
    if (End > Begin)
      Ranges.emplace_back(Begin, End);
  };
  // An unwind function's size may span disjoint fragments and other bodies.
  // Its contiguous entry extent stops at the first metadata gap.
  if (Func.OriginalSize) {
    va_t End = Func.Entry + Func.OriginalSize;
    if (Func.ExceptionMetadata)
      End = std::min(End, Func.ExceptionMetadata->contiguousEnd());
    Add(Func.Entry, End);
  }
  if (Func.ExceptionMetadata) {
    Add(Func.ExceptionMetadata->CodeRange.Begin,
        Func.ExceptionMetadata->CodeRange.End);
    for (const ExceptionAddressRange &Fragment :
         Func.ExceptionMetadata->FragmentRanges)
      Add(Fragment.Begin, Fragment.End);
  }
  for (const MedBlock &Block : Func.Blocks)
    Add(Block.StartAddr, Block.EndAddr);
  std::sort(Ranges.begin(), Ranges.end());
  std::vector<std::pair<va_t, va_t>> Merged;
  for (const auto &Range : Ranges)
    if (!Merged.empty() && Range.first <= Merged.back().second)
      Merged.back().second = std::max(Merged.back().second, Range.second);
    else
      Merged.push_back(Range);
  return Merged;
}

} // namespace neverd

#endif // NEVERD_IR_MED_MEDCODEIDENTITY_H
