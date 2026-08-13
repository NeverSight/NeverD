//===- COFFExceptionFH3.cpp - MSVC C++ EH3 decoding -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFExceptionDetail.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::coff_loader::detail {

bool parseFH3(ExceptionFunction &F, const BinaryImage &Img) {
  auto FuncInfoRVA = readScalar<uint32_t>(Img, F.HandlerDataVA);
  va_t FuncInfoVA = 0;
  if (!FuncInfoRVA || *FuncInfoRVA == 0 ||
      !addRVA(Img.Base, *FuncInfoRVA, FuncInfoVA)) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "invalid C++ FuncInfo3 reference");
    return false;
  }
  // `magicNumber` is a 29-bit field sharing its word with `bbtFlags`, and the
  // magic decides where the record ends: `EH_MAGIC_NUMBER1` stops after the
  // unwind-help displacement, `2` adds the exception-specification list, and
  // `3` adds `EHFlags`.  Reading the newest layout out of an older record both
  // invents trailing fields from whatever follows in the section and rejects a
  // legacy record that legitimately sits within eight bytes of the end.
  auto MagicWord = readScalar<uint32_t>(Img, FuncInfoVA);
  if (!MagicWord) {
    diagnose(F, ExceptionParseStatus::Malformed, "truncated C++ FuncInfo3");
    return false;
  }
  const uint32_t Magic = *MagicWord & 0x1FFFFFFFu;
  CxxFuncInfoVersion Version;
  size_t FuncInfoSize;
  switch (Magic) {
  case 0x19930520:
    Version = CxxFuncInfoVersion::Original;
    FuncInfoSize = 32;
    break;
  case 0x19930521:
    Version = CxxFuncInfoVersion::WithExceptionSpecs;
    FuncInfoSize = 36;
    break;
  case 0x19930522:
    Version = CxxFuncInfoVersion::WithEHFlags;
    FuncInfoSize = 40;
    break;
  default:
    diagnose(F, ExceptionParseStatus::Malformed, "unknown C++ FuncInfo3 magic");
    return false;
  }

  const uint8_t *FI = nullptr;
  if (!readBytes(Img, FuncInfoVA, FuncInfoSize, FI)) {
    diagnose(F, ExceptionParseStatus::Malformed, "truncated C++ FuncInfo3");
    return false;
  }

  CxxExceptionInfo Info;
  Info.Magic = Magic;
  Info.Version = Version;
  Info.BBTFlags = *MagicWord >> 29;
  std::vector<ExceptionAddressRange> FunctionGroupRanges{F.CodeRange};
  for (const ExceptionFunction &Candidate : Img.ExceptionMetadata.Functions) {
    if (&Candidate == &F || Candidate.Kind != RuntimeFunctionKind::Primary ||
        Candidate.HandlerDataVA == 0 || !Candidate.CodeRange.isValid())
      continue;
    auto CandidateFuncInfoRVA =
        readScalar<uint32_t>(Img, Candidate.HandlerDataVA);
    if (CandidateFuncInfoRVA && *CandidateFuncInfoRVA == *FuncInfoRVA)
      FunctionGroupRanges.push_back(Candidate.CodeRange);
  }
  std::sort(FunctionGroupRanges.begin(), FunctionGroupRanges.end(),
            [](const ExceptionAddressRange &A, const ExceptionAddressRange &B) {
              return std::tie(A.Begin, A.End) < std::tie(B.Begin, B.End);
            });
  FunctionGroupRanges.erase(
      std::unique(
          FunctionGroupRanges.begin(), FunctionGroupRanges.end(),
          [](const ExceptionAddressRange &A, const ExceptionAddressRange &B) {
            return A.Begin == B.Begin && A.End == B.End;
          }),
      FunctionGroupRanges.end());
  Info.IsSeparated = FunctionGroupRanges.size() > 1;
  auto IsFunctionGroupAddress = [&](va_t Address) {
    auto It = std::upper_bound(
        FunctionGroupRanges.begin(), FunctionGroupRanges.end(), Address,
        [](va_t Value, const ExceptionAddressRange &Range) {
          return Value < Range.Begin;
        });
    if (It == FunctionGroupRanges.begin())
      return false;
    --It;
    return It->contains(Address) || Address == It->End;
  };
  int32_t MaxState = readLE<int32_t>(FI + 4);
  uint32_t UnwindMapRVA = readLE<uint32_t>(FI + 8);
  uint32_t TryCount = readLE<uint32_t>(FI + 12);
  uint32_t TryMapRVA = readLE<uint32_t>(FI + 16);
  uint32_t IPCount = readLE<uint32_t>(FI + 20);
  uint32_t IPMapRVA = readLE<uint32_t>(FI + 24);
  Info.UnwindHelpOffset = readLE<int32_t>(FI + 28);
  if (Version >= CxxFuncInfoVersion::WithExceptionSpecs &&
      !readRVAField(Img.Base, FI + 32, Info.ESTypeListVA)) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "C++ ESTypeList RVA overflows");
    return false;
  }
  if (Version >= CxxFuncInfoVersion::WithEHFlags) {
    Info.Flags = readLE<uint32_t>(FI + 36);
    Info.IsSynchronous = (Info.Flags & 1u) != 0;
    Info.HasDynamicStackAlignment = (Info.Flags & 2u) != 0;
    Info.IsNoExcept = (Info.Flags & 4u) != 0;
  } else {
    // A record that predates `EHFlags` cannot say whether it was built /EHs or
    // /EHa.  Leaving the synchronous claim unset is what keeps a consumer that
    // requires synchronous EH -- native regeneration, for one -- from acting
    // on a guess the image never made.
    Info.Flags = 0;
  }

  if (MaxState < 0 || static_cast<uint32_t>(MaxState) > MaxLanguageRecords ||
      TryCount > MaxLanguageRecords || IPCount > MaxLanguageRecords) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "C++ FuncInfo3 count exceeds decode budget");
    return false;
  }
  Info.MaxState = static_cast<uint32_t>(MaxState);
  LanguageRecordBudget Budget;
  if (!Budget.consume(Info.MaxState) || !Budget.consume(TryCount) ||
      !Budget.consume(IPCount)) {
    diagnose(F, ExceptionParseStatus::Partial,
             "FH3 aggregate language graph exceeds decode budget");
    return false;
  }

  if (Info.MaxState != 0) {
    va_t MapVA = 0;
    if (UnwindMapRVA == 0 || !addRVA(Img.Base, UnwindMapRVA, MapVA)) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid C++ unwind-map RVA");
      return false;
    }
    const uint8_t *Map = nullptr;
    uint64_t Bytes = uint64_t(Info.MaxState) * 8;
    if (Bytes > std::numeric_limits<size_t>::max() ||
        !readBytes(Img, MapVA, static_cast<size_t>(Bytes), Map)) {
      diagnose(F, ExceptionParseStatus::Malformed, "truncated C++ unwind map");
      return false;
    }
    Info.UnwindMap.reserve(Info.MaxState);
    for (uint32_t I = 0; I < Info.MaxState; ++I) {
      const uint8_t *E = Map + uint64_t(I) * 8;
      CxxUnwindAction Action;
      Action.ToState = readLE<int32_t>(E);
      uint32_t ActionRVA = readLE<uint32_t>(E + 4);
      if (ActionRVA == 0)
        Action.Kind = CxxUnwindAction::ActionKind::None;
      if (ActionRVA != 0 && !addCodeRVA(Img, ActionRVA, Action.ActionVA)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "C++ unwind action RVA overflows");
        return false;
      }
      if (Action.ActionVA != 0 && !isExecutableAddress(Img, Action.ActionVA)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "C++ unwind action is not mapped executable code");
        return false;
      }
      Info.UnwindMap.push_back(Action);
    }
  }

  if (TryCount != 0) {
    va_t MapVA = 0;
    if (TryMapRVA == 0 || !addRVA(Img.Base, TryMapRVA, MapVA)) {
      diagnose(F, ExceptionParseStatus::Malformed, "invalid C++ try-map RVA");
      return false;
    }
    const uint8_t *Map = nullptr;
    uint64_t Bytes = uint64_t(TryCount) * 20;
    if (Bytes > std::numeric_limits<size_t>::max() ||
        !readBytes(Img, MapVA, static_cast<size_t>(Bytes), Map)) {
      diagnose(F, ExceptionParseStatus::Malformed, "truncated C++ try map");
      return false;
    }
    Info.TryBlocks.reserve(TryCount);
    for (uint32_t I = 0; I < TryCount; ++I) {
      const uint8_t *E = Map + uint64_t(I) * 20;
      CxxTryBlock Try;
      Try.TryLow = readLE<int32_t>(E);
      Try.TryHigh = readLE<int32_t>(E + 4);
      Try.CatchHigh = readLE<int32_t>(E + 8);
      uint32_t CatchCount = readLE<uint32_t>(E + 12);
      uint32_t HandlerMapRVA = readLE<uint32_t>(E + 16);
      if (CatchCount > MaxLanguageRecords) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "C++ catch count exceeds decode budget");
        return false;
      }
      if (!Budget.consume(CatchCount)) {
        diagnose(F, ExceptionParseStatus::Partial,
                 "FH3 aggregate language graph exceeds decode budget");
        return false;
      }
      if (CatchCount != 0) {
        va_t HandlerMapVA = 0;
        if (HandlerMapRVA == 0 ||
            !addRVA(Img.Base, HandlerMapRVA, HandlerMapVA)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "invalid C++ handler-map RVA");
          return false;
        }
        const uint8_t *Handlers = nullptr;
        uint64_t HandlerBytes = uint64_t(CatchCount) * 20;
        if (HandlerBytes > std::numeric_limits<size_t>::max() ||
            !readBytes(Img, HandlerMapVA, static_cast<size_t>(HandlerBytes),
                       Handlers)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "truncated C++ handler map");
          return false;
        }
        Try.Handlers.reserve(CatchCount);
        for (uint32_t J = 0; J < CatchCount; ++J) {
          const uint8_t *H = Handlers + uint64_t(J) * 20;
          CxxCatchHandler Catch;
          Catch.Adjectives = readLE<uint32_t>(H);
          if (!readRVAField(Img.Base, H + 4, Catch.TypeDescriptorVA)) {
            diagnose(F, ExceptionParseStatus::Malformed,
                     "C++ type-descriptor RVA overflows");
            return false;
          }
          if (Catch.TypeDescriptorVA != 0 &&
              !Img.readVA(Catch.TypeDescriptorVA, 1)) {
            diagnose(F, ExceptionParseStatus::Malformed,
                     "C++ type descriptor is not mapped");
            return false;
          }
          Catch.CatchObjectOffset = readLE<int32_t>(H + 8);
          if (!readCodeRVAField(Img, H + 12, Catch.HandlerVA)) {
            diagnose(F, ExceptionParseStatus::Malformed,
                     "C++ catch handler RVA overflows");
            return false;
          }
          if (Catch.HandlerVA == 0 ||
              !isExecutableAddress(Img, Catch.HandlerVA)) {
            diagnose(F, ExceptionParseStatus::Malformed,
                     "C++ catch handler is not mapped executable code");
            return false;
          }
          Catch.ParentFrameOffset = readLE<int32_t>(H + 16);
          Try.Handlers.push_back(std::move(Catch));
        }
      }
      Info.TryBlocks.push_back(std::move(Try));
    }
  }

  for (const CxxTryBlock &Try : Info.TryBlocks)
    for (const CxxCatchHandler &Catch : Try.Handlers)
      Info.IsCatchFunclet |= Catch.HandlerVA == F.CodeRange.Begin;

  if (IPCount != 0) {
    va_t MapVA = 0;
    if (IPMapRVA == 0 || !addRVA(Img.Base, IPMapRVA, MapVA)) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid C++ IP-to-state RVA");
      return false;
    }
    const uint8_t *Map = nullptr;
    uint64_t Bytes = uint64_t(IPCount) * 8;
    if (Bytes > std::numeric_limits<size_t>::max() ||
        !readBytes(Img, MapVA, static_cast<size_t>(Bytes), Map)) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "truncated C++ IP-to-state map");
      return false;
    }
    Info.IPMap.reserve(IPCount);
    for (uint32_t I = 0; I < IPCount; ++I) {
      const uint8_t *E = Map + uint64_t(I) * 8;
      CxxIPState State;
      uint32_t IPRVA = readLE<uint32_t>(E);
      if (!addCodeRVA(Img, IPRVA, State.IP)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "C++ IP-to-state address overflows");
        return false;
      }
      if (!IsFunctionGroupAddress(State.IP)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "C++ IP-to-state entry 0x" + llvm::utohexstr(State.IP) +
                     " leaves its FuncInfo function group");
        return false;
      }
      State.State = readLE<int32_t>(E + 4);
      Info.IPMap.push_back(State);
    }
  }

  // The exception-specification list names the types a `throw(...)` permits.
  // It is spelled with the same `HandlerType` record a catch clause uses, but
  // only the adjectives and the type descriptor mean anything in this
  // position: there is no handler to run and no object to construct, because
  // violating the specification calls `unexpected` rather than dispatching.
  if (Info.ESTypeListVA != 0) {
    const uint8_t *List = nullptr;
    if (!readBytes(Img, Info.ESTypeListVA, 8, List)) {
      diagnose(F, ExceptionParseStatus::Malformed, "truncated C++ ESTypeList");
      return false;
    }
    int32_t SpecCount = readLE<int32_t>(List);
    va_t SpecArrayVA = 0;
    if (SpecCount < 0 ||
        static_cast<uint32_t>(SpecCount) > MaxLanguageRecords) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "C++ ESTypeList count exceeds decode budget");
      return false;
    }
    if (!readRVAField(Img.Base, List + 4, SpecArrayVA)) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "C++ ESTypeList array RVA overflows");
      return false;
    }
    if (!Budget.consume(static_cast<uint32_t>(SpecCount))) {
      diagnose(F, ExceptionParseStatus::Partial,
               "FH3 aggregate language graph exceeds decode budget");
      return false;
    }
    if (SpecCount != 0) {
      const uint8_t *Specs = nullptr;
      uint64_t SpecBytes = uint64_t(SpecCount) * 20;
      if (SpecArrayVA == 0 || SpecBytes > std::numeric_limits<size_t>::max() ||
          !readBytes(Img, SpecArrayVA, static_cast<size_t>(SpecBytes), Specs)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "truncated C++ ESTypeList type array");
        return false;
      }
      Info.ExceptionSpecTypes.reserve(static_cast<size_t>(SpecCount));
      for (int32_t I = 0; I < SpecCount; ++I) {
        const uint8_t *S = Specs + uint64_t(I) * 20;
        CxxExceptionSpecType Spec;
        Spec.Adjectives = readLE<uint32_t>(S);
        if (!readRVAField(Img.Base, S + 4, Spec.TypeDescriptorVA)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "C++ ESTypeList type-descriptor RVA overflows");
          return false;
        }
        if (Spec.TypeDescriptorVA != 0 &&
            !Img.readVA(Spec.TypeDescriptorVA, 1)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "C++ ESTypeList type descriptor is not mapped");
          return false;
        }
        Info.ExceptionSpecTypes.push_back(Spec);
      }
    }
  }

  if (!Info.hasValidStateGraph()) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "invalid C++ exception state graph");
    return false;
  }
  F.Cxx = std::move(Info);
  return true;
}

} // namespace neverd::coff_loader::detail
