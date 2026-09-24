//===- CEmitterOptions.h - C emitter configuration -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Options controlling C source emission from decompiled IR.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_CEMITTEROPTIONS_H
#define NEVERD_BACKEND_C_CEMITTEROPTIONS_H

#include "neverd/Common.h"

namespace neverd {

struct BinaryImage;

struct CEmitterOptions {
  bool EmitIncludes = true;
  bool EmitComments = true;
  /// Wrap record declarations in stable preprocessor guards so independently
  /// emitted translation units can be concatenated. A consumer that parses one
  /// complete unit and performs its own cross-unit deduplication can disable
  /// the guards without changing the declarations themselves.
  bool EmitRecordGuards = true;
  bool UseDebugNames = true;
  Arch TheArch = Arch::X64;
  BinaryFormat Format = BinaryFormat::Unknown;
  /// When set, HighC can fold rdata integer loads, print printable
  /// rdata C/wchar literals, and name image-backed data objects
  /// instead of emitting raw virtual addresses.
  const BinaryImage *Image = nullptr;
};

} // namespace neverd

#endif // NEVERD_BACKEND_C_CEMITTEROPTIONS_H
