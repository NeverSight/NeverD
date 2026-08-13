//===- ExceptionWindowsEH.h - Windows language records --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The language data a Windows personality reads out of `.xdata`: the SEH
/// scope table `__C_specific_handler` walks, the `FuncInfo` state machine
/// `__CxxFrameHandler` walks, and the `__GSHandlerData` a GS wrapper prefixes
/// either of them with.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_EXCEPTIONWINDOWSEH_H
#define NEVERD_LOADER_EXCEPTIONWINDOWSEH_H

#include "neverd/loader/ExceptionCommon.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neverd {

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

/// One type named by a dynamic exception specification (`void f() throw(A)`).
/// MSVC spells the list with the same `HandlerType` record a catch clause
/// uses, so an entry carries the same adjectives and type descriptor; the
/// handler and catch-object fields are meaningless here and are not kept.
struct CxxExceptionSpecType {
  uint32_t Adjectives = 0;
  va_t TypeDescriptorVA = 0;
};

/// Which `FuncInfo` fields the record's magic declares.  MSVC only ever
/// appends, so a newer magic is a superset of an older one; what the version
/// decides is where the record *ends*, and therefore which trailing words are
/// part of it rather than whatever data follows in the section.
enum class CxxFuncInfoVersion : uint8_t {
  /// `EH_MAGIC_NUMBER1` (0x19930520): no exception-specification list and no
  /// `EHFlags`.
  Original,
  /// `EH_MAGIC_NUMBER2` (0x19930521): adds `pESTypeList`.
  WithExceptionSpecs,
  /// `EH_MAGIC_NUMBER3` (0x19930522): adds `EHFlags`.
  WithEHFlags,
};

struct CxxExceptionInfo {
  enum class Encoding : uint8_t {
    FH3,
    FH4,
  } NativeEncoding = Encoding::FH3;
  /// The 29-bit `magicNumber` field, with the three `bbtFlags` bits that share
  /// its word already split out into \ref BBTFlags.
  uint32_t Magic = 0;
  CxxFuncInfoVersion Version = CxxFuncInfoVersion::WithEHFlags;
  uint32_t Flags = 0;
  uint32_t MaxState = 0;
  int32_t UnwindHelpOffset = 0;
  va_t ESTypeListVA = 0;
  /// Decoded `ESTypeList`.  An empty vector with a nonzero \ref ESTypeListVA
  /// is `throw()`, which is a different contract from having no list at all.
  std::vector<CxxExceptionSpecType> ExceptionSpecTypes;
  uint32_t BBTFlags = 0;
  uint32_t FrameOffset = 0;
  bool IsCatchFunclet = false;
  bool IsSeparated = false;
  bool IsSynchronous = false;
  bool IsNoExcept = false;
  /// `FI_DYNSTKALIGN_FLAG`: the frame is dynamically aligned, so the unwinder
  /// reaches locals through an established frame pointer rather than from the
  /// stack pointer the unwind codes describe.
  bool HasDynamicStackAlignment = false;
  std::vector<CxxUnwindAction> UnwindMap;
  std::vector<CxxTryBlock> TryBlocks;
  std::vector<CxxIPState> IPMap;

  /// True when the function declares a dynamic exception specification, which
  /// only a record whose magic reaches `EH_MAGIC_NUMBER2` can do.
  bool hasExceptionSpecification() const { return ESTypeListVA != 0; }

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
/// `__GSHandlerData`: where in the frame the stack cookie lives, plus how the
/// CRT recomputes it.  The record's shape follows the target's pointer width,
/// because the flags occupy the low bits the cookie's frame offset cannot use.
struct GSCookieInfo {
  ExceptionParseStatus ParseStatus = ExceptionParseStatus::Partial;
  int32_t CookieOffset = 0;
  /// Only encoded on a 64-bit target, which has a third spare bit to spend on
  /// them.  A 32-bit record conveys the same thing by which wrapper is
  /// installed, so these stay false there rather than reading as "absent".
  bool HasExceptionHandler = false;
  bool HasUnwindHandler = false;
  /// The frame was dynamically aligned, so the cookie's slot is found relative
  /// to the realigned base rather than to the establisher frame.
  bool HasAlignment = false;
  /// Meaningful only alongside \ref HasAlignment on a 64-bit target: the 32-bit
  /// CRT derives the same adjustment arithmetically and stores nothing.
  int32_t AlignmentBaseOffset = 0;
  uint32_t Alignment = 0;
  std::vector<uint8_t> Payload;
};

} // namespace neverd

#endif // NEVERD_LOADER_EXCEPTIONWINDOWSEH_H
