//===- COFFRegistrationEHFuncInfo.cpp - x86-32 C++ FuncInfo decode -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
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
                       va_t FuncInfoVA) {
  // `magicNumber` occupies 29 bits and shares its word with `bbtFlags`, and
  // the magic fixes the length of the record: the original form ends after the
  // IP map pointer, the second adds the exception-specification list, and the
  // third adds `EHFlags`.  Demanding the longest layout would both reject a
  // legacy record near the end of a section and read trailing fields out of
  // whatever data follows it.
  const uint8_t *MagicField = Img.readVA(FuncInfoVA, sizeof(uint32_t));
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

  const uint8_t *FI = Img.readVA(FuncInfoVA, FuncInfoSize);
  if (!FI) {
    diagnose(F, ExceptionParseStatus::Malformed, "truncated x86 C++ FuncInfo");
    return false;
  }

  CxxExceptionInfo Info;
  Info.NativeEncoding = CxxExceptionInfo::Encoding::FH3;
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
  // x86 tracks the current state in the frame, not in a table.  A non-empty
  // IP map here means the record is not the x86 form this decoder proved.
  if (IPCount != 0) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "x86 C++ FuncInfo declares an IP-to-state map");
    return false;
  }

  if (Info.MaxState != 0) {
    uint64_t Bytes = uint64_t(Info.MaxState) * 8;
    const uint8_t *Map = Bytes <= std::numeric_limits<size_t>::max()
                             ? Img.readVA(UnwindMapVA, size_t(Bytes))
                             : nullptr;
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
      Info.UnwindMap.push_back(Action);
    }
  }

  if (TryCount != 0) {
    uint64_t Bytes = uint64_t(TryCount) * 20;
    const uint8_t *Map = Bytes <= std::numeric_limits<size_t>::max()
                             ? Img.readVA(TryMapVA, size_t(Bytes))
                             : nullptr;
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
      if (CatchCount > MaxRegistrationRecords) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "x86 C++ catch count exceeds decode budget");
        return false;
      }
      uint64_t HandlerBytes = uint64_t(CatchCount) * 16;
      const uint8_t *Handlers =
          CatchCount == 0 ? nullptr
                          : Img.readVA(HandlerArrayVA, size_t(HandlerBytes));
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
        Try.Handlers.push_back(std::move(Catch));
      }
      Info.TryBlocks.push_back(std::move(Try));
    }
  }

  // The exception-specification list, in the same absolute-pointer spelling
  // the rest of the x86 record uses.  Its elements are `HandlerType` records,
  // so they are the shorter x86 form here too.
  if (Info.ESTypeListVA != 0) {
    const uint8_t *List = Img.readVA(Info.ESTypeListVA, 8);
    if (!List) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "truncated x86 C++ ESTypeList");
      return false;
    }
    int32_t SpecCount = readLE<int32_t>(List);
    uint32_t SpecArrayVA = readLE<uint32_t>(List + 4);
    if (SpecCount < 0 ||
        static_cast<uint32_t>(SpecCount) > MaxRegistrationRecords) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "x86 C++ ESTypeList count exceeds decode budget");
      return false;
    }
    if (SpecCount != 0) {
      const uint8_t *Specs =
          Img.readVA(SpecArrayVA, static_cast<size_t>(SpecCount) * 16);
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
