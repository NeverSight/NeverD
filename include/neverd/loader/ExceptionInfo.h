//===- ExceptionInfo.h - Normalized exception metadata --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the format-independent representation used to carry table-based
/// unwind and language exception metadata from loaders through NeverD's IR and
/// rewrite pipelines.  Raw file offsets never escape the loader: consumers see
/// checked half-open VA ranges, normalized targets, parse provenance, and an
/// explicit completeness state.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_EXCEPTIONINFO_H
#define NEVERD_LOADER_EXCEPTIONINFO_H

#include "neverd/Common.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

/// Quality of a decoded record.  Partial records remain useful for analysis,
/// while Malformed records must never be used to regenerate native metadata.
enum class ExceptionParseStatus : uint8_t {
  Complete,
  Partial,
  Malformed,
};

inline const char *getExceptionParseStatusName(ExceptionParseStatus Status) {
  switch (Status) {
  case ExceptionParseStatus::Complete:
    return "complete";
  case ExceptionParseStatus::Partial:
    return "partial";
  case ExceptionParseStatus::Malformed:
    return "malformed";
  }
  return "unknown";
}

inline ExceptionParseStatus mergeExceptionParseStatus(ExceptionParseStatus A,
                                                      ExceptionParseStatus B) {
  return static_cast<ExceptionParseStatus>(
      std::max(static_cast<unsigned>(A), static_cast<unsigned>(B)));
}

/// Checked half-open virtual-address range [Begin, End).
struct ExceptionAddressRange {
  va_t Begin = 0;
  va_t End = 0;

  static std::optional<ExceptionAddressRange> fromStartAndSize(va_t Start,
                                                               uint64_t Size) {
    if (Size == 0 || Size > std::numeric_limits<va_t>::max() - Start)
      return std::nullopt;
    return ExceptionAddressRange{Start, Start + Size};
  }

  bool isValid() const { return Begin < End; }
  uint64_t size() const { return isValid() ? End - Begin : 0; }
  bool contains(va_t Address) const {
    return isValid() && Address >= Begin && Address < End;
  }
  bool contains(const ExceptionAddressRange &Other) const {
    return isValid() && Other.isValid() && Other.Begin >= Begin &&
           Other.End <= End;
  }
  bool overlaps(const ExceptionAddressRange &Other) const {
    return isValid() && Other.isValid() && Begin < Other.End &&
           Other.Begin < End;
  }
};

/// Native runtime-function representation that produced a normalized record.
enum class ExceptionEncoding : uint8_t {
  Unknown,
  X64UnwindV1,
  X64UnwindV2,
  X64UnwindV3,
  ARM32Packed,
  ARM32PackedFragment,
  ARM32Unpacked,
  ARM64Packed,
  ARM64PackedFragment,
  ARM64Unpacked,
};

inline const char *getExceptionEncodingName(ExceptionEncoding Encoding) {
  switch (Encoding) {
  case ExceptionEncoding::X64UnwindV1:
    return "x64-unwind-v1";
  case ExceptionEncoding::X64UnwindV2:
    return "x64-unwind-v2";
  case ExceptionEncoding::X64UnwindV3:
    return "x64-unwind-v3";
  case ExceptionEncoding::ARM32Packed:
    return "arm32-packed";
  case ExceptionEncoding::ARM32PackedFragment:
    return "arm32-packed-fragment";
  case ExceptionEncoding::ARM32Unpacked:
    return "arm32-unpacked";
  case ExceptionEncoding::ARM64Packed:
    return "arm64-packed";
  case ExceptionEncoding::ARM64PackedFragment:
    return "arm64-packed-fragment";
  case ExceptionEncoding::ARM64Unpacked:
    return "arm64-unpacked";
  case ExceptionEncoding::Unknown:
    return "unknown";
  }
  return "unknown";
}

enum class RuntimeFunctionKind : uint8_t {
  Primary,
  Chained,
  Fragment,
};

enum class UnwindOperationKind : uint8_t {
  PushNonVolatile,
  PushTwoRegisters,
  PushConsecutiveRegisters,
  AllocateLarge,
  AllocateHuge,
  AllocateSmall,
  SetFramePointer,
  SaveNonVolatile,
  SaveNonVolatileFar,
  Epilog,
  Spare,
  SaveXMM128,
  SaveXMM128Far,
  PushMachineFrame,
  PushCanonicalFrame,
  Opaque,
};

/// One decoded unwind action.  OperandBytes retains the exact native payload
/// when an operation is unknown or cannot yet be represented semantically.
struct UnwindOperation {
  UnwindOperationKind Kind = UnwindOperationKind::Opaque;
  uint32_t CodeOffset = 0;
  uint8_t OpInfo = 0;
  uint8_t SlotCount = 0;
  uint16_t Register = 0;
  uint64_t StackOffset = 0;
  std::vector<uint8_t> OperandBytes;
};

struct UnwindEpilog {
  int64_t StartOffset = 0;
  uint8_t Flags = 0;
  uint32_t FirstOperationOffset = 0;
  uint32_t LastInstructionOffset = 0;
  std::vector<UnwindOperation> Operations;
};

/// The exact Windows personality identity.  GS wrappers deliberately remain
/// distinct from their base language handlers because their cookie payload is
/// part of the native contract.
enum class ExceptionPersonality : uint8_t {
  None,
  Unknown,
  CSpecificHandler,
  CxxFrameHandler3,
  CxxFrameHandler4,
  GSHandlerCheckSEH,
  GSHandlerCheckEH,
  GSHandlerCheckEH4,
};

inline const char *getExceptionPersonalityName(ExceptionPersonality P) {
  switch (P) {
  case ExceptionPersonality::None:
    return "none";
  case ExceptionPersonality::Unknown:
    return "unknown";
  case ExceptionPersonality::CSpecificHandler:
    return "__C_specific_handler";
  case ExceptionPersonality::CxxFrameHandler3:
    return "__CxxFrameHandler3";
  case ExceptionPersonality::CxxFrameHandler4:
    return "__CxxFrameHandler4";
  case ExceptionPersonality::GSHandlerCheckSEH:
    return "__GSHandlerCheck_SEH";
  case ExceptionPersonality::GSHandlerCheckEH:
    return "__GSHandlerCheck_EH";
  case ExceptionPersonality::GSHandlerCheckEH4:
    return "__GSHandlerCheck_EH4";
  }
  return "unknown";
}

inline bool isSEHPersonality(ExceptionPersonality P) {
  return P == ExceptionPersonality::CSpecificHandler ||
         P == ExceptionPersonality::GSHandlerCheckSEH;
}

inline bool isCxxPersonality(ExceptionPersonality P) {
  return P == ExceptionPersonality::CxxFrameHandler3 ||
         P == ExceptionPersonality::CxxFrameHandler4 ||
         P == ExceptionPersonality::GSHandlerCheckEH ||
         P == ExceptionPersonality::GSHandlerCheckEH4;
}

inline bool isGSWrappedPersonality(ExceptionPersonality P) {
  return P == ExceptionPersonality::GSHandlerCheckSEH ||
         P == ExceptionPersonality::GSHandlerCheckEH ||
         P == ExceptionPersonality::GSHandlerCheckEH4;
}

enum class SEHScopeKind : uint8_t {
  Filter,
  CatchAll,
  Finally,
};

struct SEHScopeRecord {
  ExceptionAddressRange GuardedRange;
  SEHScopeKind Kind = SEHScopeKind::Filter;
  va_t FilterOrFinallyVA = 0;
  va_t HandlerVA = 0;
  va_t ContinuationVA = 0;
  ExceptionParseStatus ParseStatus = ExceptionParseStatus::Complete;
};

struct SEHExceptionInfo {
  std::vector<SEHScopeRecord> Scopes;
};

struct CxxUnwindAction {
  int32_t ToState = -1;
  va_t ActionVA = 0;
  enum class ActionKind : uint8_t {
    None,
    Direct,
    DestructorWithObject,
    DestructorWithObjectPointer,
  } Kind = ActionKind::Direct;
  int32_t ObjectOffset = 0;
};

inline const char *
getCxxUnwindActionKindName(CxxUnwindAction::ActionKind Kind) {
  switch (Kind) {
  case CxxUnwindAction::ActionKind::None:
    return "none";
  case CxxUnwindAction::ActionKind::Direct:
    return "direct";
  case CxxUnwindAction::ActionKind::DestructorWithObject:
    return "destructor-object";
  case CxxUnwindAction::ActionKind::DestructorWithObjectPointer:
    return "destructor-object-pointer";
  }
  return "unknown";
}

struct CxxIPState {
  va_t IP = 0;
  int32_t State = -1;
};

struct CxxCatchHandler {
  uint32_t Adjectives = 0;
  va_t TypeDescriptorVA = 0;
  int32_t CatchObjectOffset = 0;
  va_t HandlerVA = 0;
  int32_t ParentFrameOffset = 0;
  std::vector<va_t> ContinuationVAs;
};

struct CxxTryBlock {
  int32_t TryLow = -1;
  int32_t TryHigh = -1;
  int32_t CatchHigh = -1;
  std::vector<CxxCatchHandler> Handlers;
};

struct CxxExceptionInfo {
  enum class Encoding : uint8_t {
    FH3,
    FH4,
  } NativeEncoding = Encoding::FH3;
  uint32_t Magic = 0;
  uint32_t Flags = 0;
  uint32_t MaxState = 0;
  int32_t UnwindHelpOffset = 0;
  va_t ESTypeListVA = 0;
  uint32_t BBTFlags = 0;
  uint32_t FrameOffset = 0;
  bool IsCatchFunclet = false;
  bool IsSeparated = false;
  bool IsSynchronous = false;
  bool IsNoExcept = false;
  std::vector<CxxUnwindAction> UnwindMap;
  std::vector<CxxTryBlock> TryBlocks;
  std::vector<CxxIPState> IPMap;

  /// Validate normalized state relationships without consulting native table
  /// layout.  Each unwind transition must move to a strictly older state (or
  /// -1), and IP map entries must be strictly ordered and name valid states.
  bool hasValidStateGraph() const {
    if (UnwindMap.size() != MaxState)
      return false;
    for (size_t I = 0; I < UnwindMap.size(); ++I) {
      int32_t To = UnwindMap[I].ToState;
      if (To < -1 || To >= static_cast<int32_t>(I))
        return false;
    }
    for (size_t I = 0; I < IPMap.size(); ++I) {
      if (I != 0 && IPMap[I - 1].IP >= IPMap[I].IP)
        return false;
      if (IPMap[I].State < -1 ||
          IPMap[I].State >= static_cast<int32_t>(MaxState))
        return false;
    }
    for (const CxxTryBlock &Try : TryBlocks) {
      if (Try.TryLow < 0 || Try.TryHigh < Try.TryLow ||
          Try.CatchHigh <= Try.TryHigh ||
          Try.CatchHigh >= static_cast<int32_t>(MaxState))
        return false;
    }
    return true;
  }
};

/// GS wrapper data is intentionally opaque until its target-specific payload
/// has been validated.  Keeping the bytes and their status prevents a caller
/// from accidentally regenerating only the wrapped base personality.
struct GSCookieInfo {
  ExceptionParseStatus ParseStatus = ExceptionParseStatus::Partial;
  int32_t CookieOffset = 0;
  bool HasExceptionHandler = false;
  bool HasUnwindHandler = false;
  bool HasAlignment = false;
  int32_t AlignmentBaseOffset = 0;
  uint32_t Alignment = 0;
  std::vector<uint8_t> Payload;
};

enum class ExceptionalEdgeKind : uint8_t {
  SEHFilter,
  SEHHandler,
  SEHFinally,
  CxxCleanup,
  CxxCatch,
  Unknown,
};

/// IR-level exceptional transfer kept separate from ordinary CFG edges.  In a
/// successor list BlockId is the target block; in a predecessor list it is the
/// source block.  A value of -1 denotes a valid target outside the current
/// function/funclet while TargetVA retains the exact destination.
struct ExceptionalEdge {
  int BlockId = -1;
  va_t TargetVA = 0;
  ExceptionalEdgeKind Kind = ExceptionalEdgeKind::Unknown;
  uint32_t RegionIndex = 0;
  int32_t State = -1;

  bool operator==(const ExceptionalEdge &Other) const {
    return BlockId == Other.BlockId && TargetVA == Other.TargetVA &&
           Kind == Other.Kind && RegionIndex == Other.RegionIndex &&
           State == Other.State;
  }
};

struct ExceptionFunction {
  ExceptionAddressRange CodeRange;
  RuntimeFunctionKind Kind = RuntimeFunctionKind::Primary;
  ExceptionEncoding Encoding = ExceptionEncoding::Unknown;
  ExceptionParseStatus ParseStatus = ExceptionParseStatus::Complete;

  /// Native table provenance.  RVAs are retained for diagnostics and patch
  /// replacement; addresses are normalized image VAs for IR consumers.
  uint32_t RuntimeFunctionRVA = 0;
  uint32_t UnwindInfoRVA = 0;
  va_t UnwindInfoVA = 0;
  uint8_t UnwindVersion = 0;
  uint8_t UnwindFlags = 0;
  uint32_t PrologueSize = 0;
  uint16_t FrameRegister = 0;
  uint32_t FrameOffset = 0;
  uint32_t PackedUnwindData = 0;
  std::vector<uint8_t> NativeUnwindBytes;
  std::vector<UnwindOperation> UnwindOperations;
  std::vector<UnwindEpilog> Epilogs;

  va_t PersonalityVA = 0;
  va_t HandlerDataVA = 0;
  ExceptionPersonality Personality = ExceptionPersonality::None;
  std::string PersonalityName;
  std::optional<SEHExceptionInfo> SEH;
  std::optional<CxxExceptionInfo> Cxx;
  std::optional<GSCookieInfo> GSCookie;

  /// Index of the primary record for a chained/fragment record, when known.
  std::optional<size_t> PrimaryFunctionIndex;
  std::optional<ExceptionAddressRange> ChainedPrimaryRange;
  uint32_t ChainedUnwindInfoRVA = 0;
  std::vector<std::string> Diagnostics;

  bool canRegenerateLanguageMetadata() const {
    return ParseStatus == ExceptionParseStatus::Complete &&
           !isGSWrappedPersonality(Personality) &&
           Personality != ExceptionPersonality::CxxFrameHandler4 &&
           Encoding != ExceptionEncoding::X64UnwindV3;
  }
};

/// Image-wide exception table plus a stable address index.  Runtime records
/// may overlap because chained entries and ARM fragments describe one logical
/// function, so lookup returns the most specific containing range.
struct ExceptionInfo {
  std::vector<ExceptionFunction> Functions;
  ExceptionParseStatus ParseStatus = ExceptionParseStatus::Complete;
  uint32_t DirectoryRVA = 0;
  uint32_t DirectorySize = 0;
  std::vector<size_t> FunctionIndex;
  std::vector<std::string> Diagnostics;

  void rebuildIndex() {
    FunctionIndex.resize(Functions.size());
    for (size_t I = 0; I < Functions.size(); ++I)
      FunctionIndex[I] = I;
    std::stable_sort(FunctionIndex.begin(), FunctionIndex.end(),
                     [&](size_t A, size_t B) {
                       const auto &RA = Functions[A].CodeRange;
                       const auto &RB = Functions[B].CodeRange;
                       if (RA.Begin != RB.Begin)
                         return RA.Begin < RB.Begin;
                       return RA.size() < RB.size();
                     });
  }

  const ExceptionFunction *findFunction(va_t Address) const {
    const ExceptionFunction *Best = nullptr;
    for (size_t I : FunctionIndex) {
      if (I >= Functions.size())
        continue;
      const ExceptionFunction &F = Functions[I];
      if (F.CodeRange.Begin > Address)
        break;
      if (!F.CodeRange.contains(Address))
        continue;
      if (!Best || F.CodeRange.size() < Best->CodeRange.size())
        Best = &F;
    }
    return Best;
  }

  ExceptionFunction *findFunction(va_t Address) {
    return const_cast<ExceptionFunction *>(
        static_cast<const ExceptionInfo *>(this)->findFunction(Address));
  }
};

} // namespace neverd

#endif // NEVERD_LOADER_EXCEPTIONINFO_H
