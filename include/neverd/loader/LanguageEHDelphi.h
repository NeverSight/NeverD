//===- LanguageEHDelphi.h - Delphi registration frames --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Normalized Delphi exception records: the x86-32 `TExcFrame` whose handler
/// descriptor is itself code, and the x86-64 `TExcData` scope array Delphi
/// moved to once it adopted the ordinary Windows table mechanism.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LANGUAGEEHDELPHI_H
#define NEVERD_LOADER_LANGUAGEEHDELPHI_H

#include "neverd/loader/ExceptionCommon.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd {

/// Which RTL routine a Delphi `TExcDesc` forwards to, and therefore what the
/// bytes after its jump mean.  Delphi puts no table in a data section: the
/// descriptor *is* code, and the dispatch data — when there is any — follows
/// the jump inline, so the routine identity is what says whether the next
/// bytes are an arm table or the handler body itself.
enum class DelphiHandlerKind : uint8_t {
  Unknown,
  /// `@HandleFinally`: a second jump follows, naming the cleanup body.
  Finally,
  /// `@HandleAnyException`: the `except` body follows inline.
  AnyException,
  /// `@HandleOnException`: a count and an array of class/handler arms follow.
  OnException,
  /// `@HandleAutoException`: the `safecall` wrapper, which converts an
  /// escaping exception into an HRESULT instead of running a handler.
  AutoException,
};

const char *getDelphiHandlerKindName(DelphiHandlerKind Kind);

/// One `except on <class> do` arm.
struct DelphiOnExceptionEntry {
  /// Address of the slot the arm names, which holds the class reference.  The
  /// RTL loads the class through it, so the slot is the identity that appears
  /// in the image and the class address is what it currently points at.
  va_t ClassSlotVA = 0;
  /// Class reference (VMT address) the arm matches.  Zero for an `else` arm,
  /// which the RTL treats as matching anything.
  va_t ClassVA = 0;
  /// Class name read from the VMT, when the VMT proved to be one.
  std::string ClassName;
  va_t HandlerVA = 0;
  bool IsCatchAll = false;
};

/// A Delphi `TExcFrame` the prologue linked onto the `FS:[0]` chain.
///
/// Delphi shares the registration mechanism with Windows SEH but nothing else:
/// there is no scope table, no try level, and no filter expression.  A frame
/// pushes one `TExcDesc`, and which of four RTL routines that descriptor jumps
/// to is the whole of its dispatch semantics.
struct DelphiFrameInfo {
  /// The `TExcDesc` the prologue pushed as the handler.
  va_t DescriptorVA = 0;
  /// RTL routine the descriptor's leading jump reaches.
  va_t RuntimeHandlerVA = 0;
  std::string RuntimeHandlerName;
  DelphiHandlerKind Kind = DelphiHandlerKind::Unknown;
  /// `Finally`: the cleanup body named by the descriptor's second jump.
  va_t FinallyBodyVA = 0;
  /// `AnyException`/`AutoException`: the handler body, which begins at the
  /// first byte after the descriptor's jump.
  va_t ExceptBodyVA = 0;
  /// `OnException`: the arms, in the order the RTL tests them.
  std::vector<DelphiOnExceptionEntry> OnExceptions;
  /// Where the prologue wrote the new record to `FS:[0]`.
  va_t ChainInstallVA = 0;
  /// True when the routine identity came from a symbol rather than from the
  /// shape of the descriptors that target it.
  bool RuntimeHandlerNamed = false;
};

/// What a Delphi x86-64 `TExcScope.TableOffset` selects.  The field is a
/// discriminant for its three smallest values and an RVA for everything else,
/// which is how one 16-byte record spells four dispatch shapes.
enum class DelphiScopeKind : uint8_t {
  /// `TableOffset == 0`: `TargetOffset` is a `try..finally` cleanup body.
  Finally,
  /// `TableOffset == 1`: `TargetOffset` is the catch a `safecall` wrapper
  /// enters to turn an escaping exception into an HRESULT.
  SafecallCatch,
  /// `TableOffset == 2`: `TargetOffset` is a `try..except` body that catches
  /// anything.
  CatchAll,
  /// `TableOffset > 2`: it is the RVA of a `TExcDesc` arm table, and
  /// `TargetOffset` carries nothing.
  OnException,
};

const char *getDelphiScopeKindName(DelphiScopeKind Kind);

/// One `TExcScope`: four RVAs in sixteen bytes.
struct DelphiScopeRecord {
  ExceptionAddressRange GuardedRange;
  DelphiScopeKind Kind = DelphiScopeKind::Finally;
  /// Cleanup or catch body, for every kind but \ref
  /// DelphiScopeKind::OnException.
  va_t TargetVA = 0;
  /// The `TExcDesc` arm table, for \ref DelphiScopeKind::OnException.
  va_t DescriptorVA = 0;
  std::vector<DelphiOnExceptionEntry> OnExceptions;
};

/// The `TExcData` a Delphi x86-64 frame keeps in its unwind handler data.
///
/// This shares nothing with \ref DelphiFrameInfo but the vendor.  Delphi on
/// x86-64 abandoned the registration chain for the ordinary table mechanism,
/// so the dispatch data is a count-prefixed array of pure data in `.xdata`
/// rather than a descriptor that is itself code, and every address in it is a
/// 32-bit RVA rather than an absolute pointer.
struct DelphiScopeTable {
  /// Address of the `TExcData`, which is the first byte after the unwind
  /// info's handler RVA.
  va_t TableVA = 0;
  std::vector<DelphiScopeRecord> Scopes;
};

} // namespace neverd

#endif // NEVERD_LOADER_LANGUAGEEHDELPHI_H
