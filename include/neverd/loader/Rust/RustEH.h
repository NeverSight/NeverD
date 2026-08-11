//===- RustEH.h - Rust panic machinery recovery ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reads a Rust image's panic behaviour out of the exception tables another
/// decoder already normalized.
///
/// Rust does not have a table format of its own.  On every non-MSVC target it
/// emits an Itanium LSDA, and on `*-pc-windows-msvc` it emits an MSVC
/// `FuncInfo` -- the same structures C++ uses.  What differs is what those
/// structures mean: Rust never emits a typed catch, so a catch-all is a
/// `catch_unwind` boundary rather than `catch (...)`, and an empty filter list
/// is a boundary that aborts rather than a `throw()` specification.
///
/// This pass therefore runs after the table decoders and annotates their
/// output.  It never re-reads a table, and it draws no conclusion that the
/// already-decoded records plus the image's symbols do not support.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_RUST_RUSTEH_H
#define NEVERD_LOADER_RUST_RUSTEH_H

#include "neverd/loader/BinaryImage.h"

namespace neverd::rust_eh {

/// True when \p Img links the Rust runtime.  Cheap enough to gate the pass on.
bool hasRustRuntime(const BinaryImage &Img);

/// Classify every decoded exception record that belongs to Rust code, and
/// record image-wide panic machinery in `Img.ExceptionMetadata.RustRuntime`.
///
/// Does nothing when the image is not Rust.  Safe to call on an image whose
/// tables are partly malformed: a record that could not be decoded is left
/// alone rather than classified from a guess.
void parseRustExceptions(BinaryImage &Img);

} // namespace neverd::rust_eh

#endif // NEVERD_LOADER_RUST_RUSTEH_H
