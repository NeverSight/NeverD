//===- GoRuntimeEH.h - Go runtime frame metadata --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recovers Go's exceptional control flow from the runtime's own frame
/// metadata.
///
/// Go does not use any of the platform exception models.  A Go image carries
/// none of `.pdata`, `.eh_frame`, or `__unwind_info` for its own code; instead
/// the linker emits a `pclntab` describing every function, and the runtime
/// walks it to run deferred calls and to unwind a panic.  So on all three
/// container formats — ELF, PE, and Mach-O — the same table is the only source
/// of truth, and recovery is a matter of finding it rather than of reading a
/// directory the container points at.
///
/// The table survives `-ldflags=-s -w`, which strips the symbol table: a
/// stripped Go binary still names every one of its functions here.  That makes
/// this decoder the difference between an anonymous blob and a fully labelled
/// program, so it is written to keep going wherever a partial answer is still
/// sound — a missing `moduledata` costs only the funcdata-derived fields — and
/// to report what it could not prove instead of inventing it.
///
/// Two table layouts are read.  The modern one, from Go 1.16, opens with a
/// block of offsets naming each sub-table.  The Go 1.2 one has no such block:
/// the function table follows the count directly and every offset inside it is
/// measured from the head of the whole table.  That magic covers Go 1.2
/// through Go 1.15, eight years in which the record shape changed and the
/// pcdata and funcdata arrays were renumbered twice, and the header says
/// nothing about which release wrote it — so what can be inferred from the
/// records is inferred and reported as such, and what cannot is left out with
/// a diagnostic saying so rather than guessed at.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_GO_GORUNTIMEEH_H
#define NEVERD_LOADER_GO_GORUNTIMEEH_H

#include "neverd/loader/BinaryImage.h"

namespace neverd::go_loader {

/// Locate the `pclntab`, decode every `_func` record, and attribute the
/// defer/panic/recover call sites found in each function body.  Records are
/// added to `Img.ExceptionMetadata` only for functions that have exceptional
/// control flow; the module-level state is recorded whenever a `pclntab` was
/// found, even if no function needed a record.
///
/// A no-op for images with no Go runtime metadata.
void parseGoExceptions(BinaryImage &Img);

/// True when the image carries a decodable Go `pclntab`.  Exposed so a caller
/// can classify an image without paying for the full walk.
bool hasGoRuntimeMetadata(const BinaryImage &Img);

} // namespace neverd::go_loader

#endif // NEVERD_LOADER_GO_GORUNTIMEEH_H
