//===- COFFRegistrationEH.h - x86-32 registration-chain EH ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recovers Windows exception state for x86-32 images, which carry no
/// `.pdata` directory at all.
///
/// On x86-32 the unwinder walks a linked list of `EXCEPTION_REGISTRATION_
/// RECORD`s rooted at `FS:[0]` that each frame's prologue pushes onto the
/// stack.  The exception tables are therefore not reachable from a directory:
/// they are reachable only from the instructions that install the record, so
/// recovery starts by proving that install sequence in the code and follows
/// the operands it pushes.  Every address that reaches a caller has been
/// checked against the image, and a table whose shape could not be proven is
/// reported rather than guessed.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_COFFREGISTRATIONEH_H
#define NEVERD_LOADER_COFF_COFFREGISTRATIONEH_H

#include "neverd/loader/BinaryImage.h"

namespace neverd::coff_loader {

/// Scan an x86-32 image for `FS:[0]` registration installs and decode the
/// `_except_handler3`/`_except_handler4` scope tables and `__CxxFrameHandler`
/// `FuncInfo` records they reference.  A no-op for other architectures.
///
/// Must run after function discovery and the COFF symbol table are available,
/// because a recovered record is attributed to the function that contains its
/// install site and handler names come from symbols and import veneers.
void parseX86RegistrationExceptions(BinaryImage &Img);

} // namespace neverd::coff_loader

#endif // NEVERD_LOADER_COFF_COFFREGISTRATIONEH_H
