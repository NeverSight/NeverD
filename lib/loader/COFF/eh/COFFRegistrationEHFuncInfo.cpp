//===- COFFRegistrationEHFuncInfo.cpp - x86-32 C++ FuncInfo decode -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <utility>

namespace neverd::coff_loader::registration_detail {

/// Decode the x86-32 `FuncInfo` reached through an `__ehhandler$` thunk.
///
/// The field order matches the x64 form, but three things differ.  Every
/// pointer is an absolute virtual address rather than an image-relative
/// offset.  There is no unwind-help displacement -- that field exists only for
/// the relative-offset targets -- so every field after the IP map count sits
/// four bytes earlier than it does on x64.  And the handler array element is
/// four bytes shorter, because x86 has no parent-frame displacement.
///
/// There is also no IP-to-state map: the current state lives in the
/// registration record the prologue pushed, so the compiler updates it with a
/// store instead of describing it in a table.
bool decodeX86FuncInfo(ExceptionFunction &F, const BinaryImage &Img,
                       va_t FuncInfoVA, std::map<va_t, va_t> *CallbackSources,
                       std::vector<ExceptionAddressRange> *RecordRanges) {
  if (RecordRanges)
    RecordRanges->clear();
  auto ReadRecord = [&](va_t VA, uint64_t Size) -> const uint8_t * {
    if (!VA || VA > UINT32_MAX || !Size ||
        Size > uint64_t(UINT32_MAX) + 1 - VA ||
        Size > std::numeric_limits<size_t>::max())
      return nullptr;
    const auto *Bytes = Img.readVA(VA, size_t(Size));
    if (Bytes && RecordRanges)
      RecordRanges->push_back({VA, VA + Size});
    return Bytes;
  };
  if (CallbackSources)
    CallbackSources->clear();
  auto RecordCallbackSource = [&](va_t Slot, va_t Target) {
    if (!CallbackSources || !Target)
      return true;
    if (Slot > UINT32_MAX - 3) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "x86 C++ callback pointer field exceeds PE32 address space");
      return false;
    }
    const auto [It, New] = CallbackSources->emplace(Slot, Target);
    if (!New && It->second != Target) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "overlapping x86 C++ callback pointer fields disagree");
      return false;
    }
    return true;
  };
  // `magicNumber` occupies 29 bits and shares its word with `bbtFlags`, and
  // the magic fixes the length of the record: the original form ends after the
  // IP map pointer, the second adds the exception-specification list, and the
  // third adds `EHFlags`.  Demanding the longest layout would both reject a
  // legacy record near the end of a section and read trailing fields out of
  // whatever data follows it.
  const uint8_t *MagicField = FuncInfoVA && FuncInfoVA <= UINT32_MAX - 3
                                  ? Img.readVA(FuncInfoVA, sizeof(uint32_t))
                                  : nullptr;
  if (!MagicField) {
    diagnose(F, ExceptionParseStatus::Malformed, "truncated x86 C++ FuncInfo");
    return false;
  }
  const uint32_t MagicWord = readLE<uint32_t>(MagicField);
  const uint32_t Magic = MagicWord & 0x1FFFFFFFu;
  CxxFuncInfoVersion Version;
  size_t FuncInfoSize;
  switch (Magic) {
  case 0x19930520:
    Version = CxxFuncInfoVersion::Original;
    FuncInfoSize = 0x1c;
    break;
  case 0x19930521:
    Version = CxxFuncInfoVersion::WithExceptionSpecs;
    FuncInfoSize = 0x20;
    break;
  case 0x19930522:
    Version = CxxFuncInfoVersion::WithEHFlags;
    FuncInfoSize = 0x24;
    break;
  default:
    diagnose(F, ExceptionParseStatus::Malformed,
             "unknown x86 C++ FuncInfo magic 0x" + llvm::utohexstr(Magic));
    return false;
  }

  const uint8_t *FI = ReadRecord(FuncInfoVA, FuncInfoSize);
  if (!FI) {
    diagnose(F, ExceptionParseStatus::Malformed, "truncated x86 C++ FuncInfo");
    return false;
  }

  CxxExceptionInfo Info;
  Info.NativeEncoding = CxxExceptionInfo::Encoding::FH3;
  Info.NativeFuncInfoVA = FuncInfoVA;
  Info.Magic = Magic;
  Info.Version = Version;
  Info.BBTFlags = MagicWord >> 29;

  int32_t MaxState = readLE<int32_t>(FI + 4);
  uint32_t UnwindMapVA = readLE<uint32_t>(FI + 8);
  uint32_t TryCount = readLE<uint32_t>(FI + 12);
  uint32_t TryMapVA = readLE<uint32_t>(FI + 16);
  uint32_t IPCount = readLE<uint32_t>(FI + 20);
  if (Version >= CxxFuncInfoVersion::WithExceptionSpecs)
    Info.ESTypeListVA = readLE<uint32_t>(FI + 28);
  if (Version >= CxxFuncInfoVersion::WithEHFlags) {
    Info.Flags = readLE<uint32_t>(FI + 32);
    Info.IsSynchronous = (Info.Flags & 1u) != 0;
    Info.HasDynamicStackAlignment = (Info.Flags & 2u) != 0;
    Info.IsNoExcept = (Info.Flags & 4u) != 0;
  }
  if (MaxState < 0 ||
      static_cast<uint32_t>(MaxState) > MaxRegistrationRecords ||
      TryCount > MaxRegistrationRecords) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "x86 C++ FuncInfo count exceeds decode budget");
    return false;
  }
  Info.MaxState = static_cast<uint32_t>(MaxState);
  uint32_t TotalRecords = Info.MaxState;
  if (TryCount > MaxRegistrationRecords - TotalRecords) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "x86 C++ FuncInfo graph exceeds aggregate decode budget");
    return false;
  }
  TotalRecords += TryCount;
  // x86 tracks the current state in the frame, not in a table.  A non-empty
  // IP map here means the record is not the x86 form this decoder proved.
  if (IPCount != 0) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "x86 C++ FuncInfo declares an IP-to-state map");
    return false;
  }

  if (Info.MaxState != 0) {
    uint64_t Bytes = uint64_t(Info.MaxState) * 8;
    const uint8_t *Map = ReadRecord(UnwindMapVA, Bytes);
    if (!Map) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "truncated x86 C++ unwind map");
      return false;
    }
    Info.UnwindMap.reserve(Info.MaxState);
    for (uint32_t I = 0; I < Info.MaxState; ++I) {
      const uint8_t *E = Map + uint64_t(I) * 8;
      CxxUnwindAction Action;
      Action.ToState = readLE<int32_t>(E);
      Action.ActionVA = readLE<uint32_t>(E + 4);
      if (Action.ActionVA == 0)
        Action.Kind = CxxUnwindAction::ActionKind::None;
      else if (!isExecutableAddress(Img, Action.ActionVA)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "x86 C++ unwind action is not executable");
        return false;
      }
      if (!RecordCallbackSource(uint64_t(UnwindMapVA) + uint64_t(I) * 8 + 4,
                                Action.ActionVA))
        return false;
      Info.UnwindMap.push_back(Action);
    }
  }

  if (TryCount != 0) {
    uint64_t Bytes = uint64_t(TryCount) * 20;
    const uint8_t *Map = ReadRecord(TryMapVA, Bytes);
    if (!Map) {
      diagnose(F, ExceptionParseStatus::Malformed, "truncated x86 C++ try map");
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
      uint32_t HandlerArrayVA = readLE<uint32_t>(E + 16);
      if (CatchCount > MaxRegistrationRecords - TotalRecords) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "x86 C++ catch graph exceeds aggregate decode budget");
        return false;
      }
      TotalRecords += CatchCount;
      uint64_t HandlerBytes = uint64_t(CatchCount) * 16;
      const uint8_t *Handlers =
          CatchCount == 0 ? nullptr : ReadRecord(HandlerArrayVA, HandlerBytes);
      if (CatchCount != 0 && !Handlers) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "truncated x86 C++ handler map");
        return false;
      }
      Try.Handlers.reserve(CatchCount);
      for (uint32_t J = 0; J < CatchCount; ++J) {
        const uint8_t *H = Handlers + uint64_t(J) * 16;
        CxxCatchHandler Catch;
        Catch.Adjectives = readLE<uint32_t>(H);
        Catch.TypeDescriptorVA = readLE<uint32_t>(H + 4);
        Catch.CatchObjectOffset = readLE<int32_t>(H + 8);
        Catch.HandlerVA = readLE<uint32_t>(H + 12);
        if (Catch.TypeDescriptorVA != 0 &&
            !Img.readVA(Catch.TypeDescriptorVA, 1)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "x86 C++ type descriptor is not mapped");
          return false;
        }
        if (Catch.HandlerVA == 0 ||
            !isExecutableAddress(Img, Catch.HandlerVA)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "x86 C++ catch handler is not executable code");
          return false;
        }
        if (!RecordCallbackSource(uint64_t(HandlerArrayVA) + uint64_t(J) * 16 +
                                      12,
                                  Catch.HandlerVA))
          return false;
        Try.Handlers.push_back(std::move(Catch));
      }
      Info.TryBlocks.push_back(std::move(Try));
    }
  }

  // The exception-specification list, in the same absolute-pointer spelling
  // the rest of the x86 record uses.  Its elements are `HandlerType` records,
  // so they are the shorter x86 form here too.
  if (Info.ESTypeListVA != 0) {
    const uint8_t *List = ReadRecord(Info.ESTypeListVA, 8);
    if (!List) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "truncated x86 C++ ESTypeList");
      return false;
    }
    int32_t SpecCount = readLE<int32_t>(List);
    uint32_t SpecArrayVA = readLE<uint32_t>(List + 4);
    if (SpecCount < 0 || static_cast<uint32_t>(SpecCount) >
                             MaxRegistrationRecords - TotalRecords) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "x86 C++ ESTypeList count exceeds decode budget");
      return false;
    }
    if (SpecCount != 0) {
      const uint8_t *Specs = ReadRecord(SpecArrayVA, uint64_t(SpecCount) * 16);
      if (!Specs) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "truncated x86 C++ ESTypeList type array");
        return false;
      }
      Info.ExceptionSpecTypes.reserve(static_cast<size_t>(SpecCount));
      for (int32_t I = 0; I < SpecCount; ++I) {
        const uint8_t *S = Specs + uint64_t(I) * 16;
        CxxExceptionSpecType Spec;
        Spec.Adjectives = readLE<uint32_t>(S);
        Spec.TypeDescriptorVA = readLE<uint32_t>(S + 4);
        if (Spec.TypeDescriptorVA != 0 &&
            !Img.readVA(Spec.TypeDescriptorVA, 1)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "x86 C++ ESTypeList type descriptor is not mapped");
          return false;
        }
        Info.ExceptionSpecTypes.push_back(Spec);
      }
    }
  }

  if (!Info.hasValidStateGraph()) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "invalid x86 C++ exception state graph");
    return false;
  }
  F.Cxx = std::move(Info);
  return true;
}

} // namespace neverd::coff_loader::registration_detail

namespace neverd::coff_loader {

std::optional<X86CxxFuncInfoRecords>
getCheckedX86CxxFuncInfoRecords(const BinaryImage &Img, va_t FuncInfoVA) {
  if (Img.Arch != Arch::X86 || Img.Format != BinaryFormat::COFF)
    return std::nullopt;
  ExceptionFunction Decoded;
  X86CxxFuncInfoRecords Records;
  if (!registration_detail::decodeX86FuncInfo(Decoded, Img, FuncInfoVA,
                                              &Records.CallbackPointerSources,
                                              &Records.Ranges) ||
      Decoded.ParseStatus != ExceptionParseStatus::Complete || !Decoded.Cxx)
    return std::nullopt;
  std::sort(Records.Ranges.begin(), Records.Ranges.end(),
            [](const auto &A, const auto &B) {
              return std::tie(A.Begin, A.End) < std::tie(B.Begin, B.End);
            });
  Records.Ranges.erase(std::unique(Records.Ranges.begin(), Records.Ranges.end(),
                                   [](const auto &A, const auto &B) {
                                     return A.Begin == B.Begin &&
                                            A.End == B.End;
                                   }),
                       Records.Ranges.end());
  for (size_t I = 1; I < Records.Ranges.size(); ++I)
    if (Records.Ranges[I - 1].overlaps(Records.Ranges[I]))
      Records.HasDistinctRanges = false;
  Records.Cxx = std::move(*Decoded.Cxx);
  return Records;
}

namespace {
bool replayCheckedCxxGraph(const BinaryImage &Img,
                           const ExceptionFunction &Function,
                           std::map<va_t, va_t> *Sources,
                           std::vector<ExceptionAddressRange> *Ranges) {
  if (Img.Arch != Arch::X86 || Img.Format != BinaryFormat::COFF ||
      Function.Encoding != ExceptionEncoding::X86CxxFuncInfo ||
      Function.ParseStatus != ExceptionParseStatus::Complete ||
      !Function.Registration || !Function.Cxx ||
      (Function.Personality != ExceptionPersonality::CxxFrameHandler3 &&
       Function.Personality != ExceptionPersonality::CxxFrameHandlerX86) ||
      !Function.Cxx->NativeFuncInfoVA ||
      Function.Cxx->NativeFuncInfoVA != Function.HandlerDataVA ||
      Function.Cxx->NativeFuncInfoVA != Function.Registration->ScopeTableVA)
    return false;
  auto Replay =
      getCheckedX86CxxFuncInfoRecords(Img, Function.Cxx->NativeFuncInfoVA);
  if (!Replay || Replay->Cxx != *Function.Cxx ||
      (Ranges && !Replay->HasDistinctRanges))
    return false;
  if (Sources)
    *Sources = std::move(Replay->CallbackPointerSources);
  if (Ranges)
    *Ranges = std::move(Replay->Ranges);
  return true;
}
} // namespace

std::optional<std::map<va_t, va_t>>
getCheckedX86CxxCallbackPointerSources(const BinaryImage &Img,
                                       const ExceptionFunction &Function) {
  std::map<va_t, va_t> Sources;
  if (!replayCheckedCxxGraph(Img, Function, &Sources, nullptr))
    return std::nullopt;
  return Sources;
}

std::optional<std::vector<ExceptionAddressRange>>
getCheckedX86CxxMetadataRanges(const BinaryImage &Img,
                               const ExceptionFunction &Function) {
  std::vector<ExceptionAddressRange> Ranges;
  if (!replayCheckedCxxGraph(Img, Function, nullptr, &Ranges))
    return std::nullopt;
  return Ranges;
}

} // namespace neverd::coff_loader
