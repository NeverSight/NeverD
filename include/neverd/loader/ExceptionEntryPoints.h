//===- ExceptionEntryPoints.h - Exception table entries ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Known independent entries of a normalized exception record.

#ifndef NEVERD_LOADER_EXCEPTIONENTRYPOINTS_H
#define NEVERD_LOADER_EXCEPTIONENTRYPOINTS_H

#include "neverd/loader/ExceptionFunction.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstdint>

namespace neverd {

enum class ExceptionEntryKind : uint8_t { Handler, Continuation };

/// Enumerate the entries the decoded tables name inside their owning code.
/// Continuations resume ordinary execution; handlers enter through unwinding.
/// Neither inherits the register facts of an ordinary predecessor. This does
/// not certify the personality routine, its dispatch rules or frame semantics.
inline void visitExceptionEntryPoints(
    const ExceptionFunction &F,
    llvm::function_ref<void(va_t, ExceptionEntryKind)> Visit) {
  const auto Add = [&](va_t A,
                       ExceptionEntryKind K = ExceptionEntryKind::Handler) {
    if (A != 0 && F.ownsCode(A))
      Visit(A, K);
  };
  if (F.SEH)
    for (const auto &Scope : F.SEH->Scopes) {
      Add(Scope.FilterOrFinallyVA);
      Add(Scope.HandlerVA);
    }
  if (F.Cxx) {
    for (const auto &Action : F.Cxx->UnwindMap)
      Add(Action.ActionVA);
    for (const auto &Try : F.Cxx->TryBlocks)
      for (const auto &Catch : Try.Handlers) {
        Add(Catch.HandlerVA);
        for (va_t Continuation : Catch.ContinuationVAs)
          Add(Continuation, ExceptionEntryKind::Continuation);
      }
  }
  if (F.Itanium && F.Itanium->IsCallSiteAddressForm)
    for (const auto &Site : F.Itanium->CallSites)
      Add(Site.LandingPadVA);
  if (F.Rust)
    for (const auto &Pad : F.Rust->LandingPads)
      Add(Pad.PadVA);
  if (F.Registration)
    for (const auto &Scope : F.Registration->Scopes) {
      Add(Scope.FilterVA);
      Add(Scope.HandlerVA);
    }
  if (F.Delphi) {
    Add(F.Delphi->FinallyBodyVA);
    Add(F.Delphi->ExceptBodyVA);
    for (const auto &Arm : F.Delphi->OnExceptions)
      Add(Arm.HandlerVA);
  }
  if (F.DelphiScopes)
    for (const auto &Scope : F.DelphiScopes->Scopes) {
      Add(Scope.TargetVA);
      for (const auto &Arm : Scope.OnExceptions)
        Add(Arm.HandlerVA);
    }
  if (F.Go && F.Go->DeferReturnOffset)
    Add(F.CodeRange.Begin + *F.Go->DeferReturnOffset);
}

/// Whether every entry of the decoded table model is available. The LSDA's
/// own parse result stays distinct from personality identification: a missing
/// routine name does not truncate the list of addresses read from the table.
/// SJLJ indices and partially decoded tables cannot supply an address graph.
/// This is table coverage, not a proof of runtime exceptional control flow.
inline bool hasCompleteExceptionEntryInventory(const ExceptionFunction &F) {
  if (F.ParseStatus == ExceptionParseStatus::Malformed)
    return false;
  if (F.Itanium && !F.Itanium->IsCallSiteAddressForm)
    return false;
  if (F.describesUnwindOnly())
    return true;
  // A loaded Windows unwind record can be structurally complete before its
  // handler tables have been requested. Absence then supplies no entry proof.
  if (F.model() == ExceptionModel::WindowsTable && !F.LanguageTablesResolved)
    return false;
  if (F.ParseStatus == ExceptionParseStatus::Complete && F.hasLanguageTable())
    return true;
  return F.Encoding == ExceptionEncoding::DwarfFDE && F.Itanium &&
         F.Itanium->ParseStatus == ExceptionParseStatus::Complete &&
         F.DecodeProvenance &&
         F.DecodeProvenance->Structural.ParseStatus ==
             ExceptionParseStatus::Complete &&
         !F.SEH && !F.Cxx && !F.GSCookie && !F.Registration && !F.Delphi &&
         !F.DelphiScopes && !F.Go && !F.Rust && !F.ObjC && !F.ARMEHABI;
}

} // namespace neverd

#endif // NEVERD_LOADER_EXCEPTIONENTRYPOINTS_H
