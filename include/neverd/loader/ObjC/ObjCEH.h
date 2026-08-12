//===- ObjCEH.h - Objective-C exception machinery recovery ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reads an Objective-C image's exception behaviour out of the tables another
/// decoder already normalized.
///
/// Objective-C has no table format of its own.  Every runtime emits an Itanium
/// LSDA -- and on `*-pc-windows-msvc` clang emits an MSVC `FuncInfo` whatever
/// runtime was asked for -- so the structures here are the same ones C++ uses.
/// What differs is the type table, and it differs completely rather than in
/// degree: Apple's slots address an `objc_typeinfo` whose first two fields
/// imitate `std::type_info`, GNUstep's Objective-C++ slots address a real
/// `std::type_info` subclass, and the GNU runtime's slots are not pointers at
/// all but the class name string itself.  A reader that applies one
/// convention to another runtime's table does not fail; it reports a class
/// name it read out of the middle of something else.
///
/// The runtime is therefore established before any slot is read, from the
/// personality a frame installed where there is one and from the image's own
/// evidence where there is not.
///
/// This pass runs after the table decoders and annotates their output.  It
/// never re-reads a call-site table, and it draws no conclusion the decoded
/// records plus the image's symbols do not support.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_OBJC_OBJCEH_H
#define NEVERD_LOADER_OBJC_OBJCEH_H

#include "neverd/loader/BinaryImage.h"

namespace neverd::objc_eh {

/// True when \p Img links one of the Objective-C runtimes.  Cheap enough to
/// gate the pass on.
bool hasObjCRuntime(const BinaryImage &Img);

/// Classify every decoded exception record that belongs to Objective-C code,
/// and record image-wide machinery in `Img.ExceptionMetadata.ObjCRuntime`.
///
/// Does nothing when the image is not Objective-C.  Safe to call on an image
/// whose tables are partly malformed: a record that could not be decoded is
/// left alone rather than classified from a guess.
void parseObjCExceptions(BinaryImage &Img);

} // namespace neverd::objc_eh

#endif // NEVERD_LOADER_OBJC_OBJCEH_H
