//===- COFFExceptionSEH.cpp - Native SEH scope-table decoding -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFExceptionDetail.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace neverd::coff_loader::detail {

/// True when \p Range is wholly covered by some runtime function of the image.
///
/// A `__C_specific_handler` scope table is emitted once per function *group*:
/// the parent's table is the union over the parent and every funclet MSVC
/// split out, and each funclet additionally carries its own copy of the
/// entries that fall inside it.  An entry outside the referencing runtime
/// function is therefore not corrupt — it simply cannot be selected from that
/// frame, because dispatch compares the faulting PC against the range.  What
/// must still hold is that the range names real code described by unwind
/// information, which is what this proves.
bool isCoveredByRuntimeFunction(const BinaryImage &Img,
                                const ExceptionAddressRange &Range) {
  const ExceptionInfo &Info = Img.ExceptionMetadata;
  for (size_t I : Info.FunctionIndex) {
    if (I >= Info.Functions.size())
      continue;
    const ExceptionFunction &Candidate = Info.Functions[I];
    if (Candidate.CodeRange.Begin > Range.Begin)
      break;
    if (Candidate.CodeRange.contains(Range))
      return true;
  }
  return false;
}

bool parseSEH(ExceptionFunction &F, const BinaryImage &Img) {
  auto Count = readScalar<uint32_t>(Img, F.HandlerDataVA);
  if (!Count || *Count > MaxLanguageRecords) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "invalid __C_specific_handler scope count");
    return false;
  }
  const uint64_t ByteSize = uint64_t(*Count) * 16;
  if (ByteSize > std::numeric_limits<size_t>::max() ||
      F.HandlerDataVA > InvalidVA - 4) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "SEH scope table size overflows");
    return false;
  }
  const uint8_t *Records = nullptr;
  if (!readBytes(Img, F.HandlerDataVA + 4, static_cast<size_t>(ByteSize),
                 Records)) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "truncated __C_specific_handler scope table");
    return false;
  }

  SEHExceptionInfo Info;
  Info.Scopes.reserve(*Count);
  for (uint32_t I = 0; I < *Count; ++I) {
    const uint8_t *R = Records + uint64_t(I) * 16;
    uint32_t BeginRVA = readLE<uint32_t>(R);
    uint32_t EndRVA = readLE<uint32_t>(R + 4);
    uint32_t FilterRVA = readLE<uint32_t>(R + 8);
    uint32_t JumpRVA = readLE<uint32_t>(R + 12);
    SEHScopeRecord Scope;
    // An optimizer can collapse a guarded body to nothing while the scope
    // record survives.  Dispatch compares `Begin <= Pc < End`, so such an
    // entry can never be selected; it is fully decoded but describes no
    // region, which is exactly what a partial scope means here.
    if (BeginRVA == EndRVA) {
      va_t Point = 0;
      if (!addCodeRVA(Img, BeginRVA, Point)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "SEH empty guarded range address overflows");
        continue;
      }
      Scope.GuardedRange = {Point, Point};
      Scope.ParseStatus = ExceptionParseStatus::Partial;
      F.Diagnostics.push_back("SEH scope at 0x" + llvm::utohexstr(Point) +
                              " guards an empty range");
    } else {
      auto Range = checkedCodeRange(Img, BeginRVA, EndRVA);
      if (!Range || (!F.CodeRange.contains(*Range) &&
                     !isCoveredByRuntimeFunction(Img, *Range))) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "SEH guarded range [0x" +
                     llvm::utohexstr(Range ? Range->Begin : va_t(BeginRVA)) +
                     ", 0x" +
                     llvm::utohexstr(Range ? Range->End : va_t(EndRVA)) +
                     ") is not covered by unwind information");
        continue;
      }
      Scope.GuardedRange = *Range;
    }
    if (JumpRVA == 0) {
      Scope.Kind = SEHScopeKind::Finally;
      if (FilterRVA == 0 ||
          !addCodeRVA(Img, FilterRVA, Scope.FilterOrFinallyVA) ||
          !isExecutableAddress(Img, Scope.FilterOrFinallyVA)) {
        Scope.ParseStatus = ExceptionParseStatus::Malformed;
        diagnose(F, ExceptionParseStatus::Malformed,
                 "invalid SEH finally target");
        continue;
      }
      Scope.HandlerVA = Scope.FilterOrFinallyVA;
    } else {
      Scope.Kind =
          FilterRVA == 1 ? SEHScopeKind::CatchAll : SEHScopeKind::Filter;
      if (FilterRVA > 1 &&
          (!addCodeRVA(Img, FilterRVA, Scope.FilterOrFinallyVA) ||
           !isExecutableAddress(Img, Scope.FilterOrFinallyVA))) {
        Scope.ParseStatus = ExceptionParseStatus::Malformed;
        diagnose(F, ExceptionParseStatus::Malformed,
                 "invalid SEH filter target");
        continue;
      }
      if (!addCodeRVA(Img, JumpRVA, Scope.HandlerVA) ||
          !isExecutableAddress(Img, Scope.HandlerVA)) {
        Scope.ParseStatus = ExceptionParseStatus::Malformed;
        diagnose(F, ExceptionParseStatus::Malformed,
                 "invalid SEH handler target");
        continue;
      }
      Scope.ContinuationVA = Scope.HandlerVA;
    }
    Info.Scopes.push_back(std::move(Scope));
  }
  F.SEH = std::move(Info);
  return F.ParseStatus != ExceptionParseStatus::Malformed;
}

} // namespace neverd::coff_loader::detail
