//===- COFFDelphiEH.h - Delphi x86-32 registration frames -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recovers Delphi's exception structure from the `FS:[0]` registration frames
/// its prologues install.
///
/// Delphi borrows only the mechanism from Windows SEH.  It has no scope table,
/// no try level, and no filter expression: a frame pushes a single `TExcDesc`,
/// which is *code* — a jump to one of four RTL routines — and whichever
/// routine that is decides what the bytes after the jump mean.  A
/// `@HandleFinally` descriptor is followed by a second jump to the cleanup
/// body, a `@HandleAnyException` descriptor by the `except` body itself, and a
/// `@HandleOnException` descriptor by a count and an array of class/handler
/// arms.  Nothing about this is reachable from a data directory, so recovery
/// starts from the prologue and follows what it pushed.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_COFFDELPHIEH_H
#define NEVERD_LOADER_COFF_COFFDELPHIEH_H

#include "neverd/loader/BinaryImage.h"

namespace neverd::coff_loader {

/// Recover every Delphi `TExcFrame` in \p Img and add a record per frame to
/// `Img.ExceptionMetadata`.
///
/// A no-op for images that are not x86-32 PE or that install no Delphi frame.
/// Must run before \ref parseX86RegistrationExceptions so the MSVC decoder can
/// leave the frames this one claimed alone.
void parseDelphiExceptions(BinaryImage &Img);

/// True when \p Img installs at least one Delphi registration frame.
bool hasDelphiRegistrationFrames(const BinaryImage &Img);

/// Decode the `TExcData` scope array a Delphi x86-64 frame keeps at
/// `F.HandlerDataVA`, filling `F.DelphiScopes` and returning true on success.
///
/// Delphi's x86-64 compiler installs no registration record: it uses the
/// ordinary table mechanism and marks the frame with `@DelphiExceptionHandler`,
/// whose handler data is a count-prefixed array of sixteen-byte `TExcScope`
/// records.  Each holds four RVAs — the guarded range, a `TableOffset` that is
/// a discriminant below 3 and a `TExcDesc` address above it, and the handler
/// body.  The layout comes from `System.pas`; every field read here is checked
/// against the image so a frame that does not match returns false with
/// \p Diagnostic set rather than producing a plausible-looking wrong table.
bool parseDelphiScopeTable(const BinaryImage &Img, ExceptionFunction &F,
                           std::string &Diagnostic);

} // namespace neverd::coff_loader

#endif // NEVERD_LOADER_COFF_COFFDELPHIEH_H
