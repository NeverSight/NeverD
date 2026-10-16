//===- LinkerThunk.h - Thunks a linker writes that only jump on -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The thunks a linker writes between a branch and its target that do nothing
/// but jump on: a branch island or incremental-linking thunk, which is one
/// direct jump, and the long-branch and interworking thunks lld and GNU ld
/// write for 32-bit ARM and A64 (neverd/support/BranchEncoding.def lists their
/// encodings).  Signature matching follows a reference through one; function
/// discovery pairs one that jumps to an import's stub with that import.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LINKERTHUNK_H
#define NEVERD_LOADER_LINKERTHUNK_H

#include "neverd/Common.h"

#include <cstdint>
#include <optional>

namespace neverd {

struct BinaryImage;

/// A thunk that only jumps on.
struct LinkerThunk {
  /// Where it jumps.
  uint64_t Target = 0;
  /// On 32-bit ARM, whether it enters its target in Thumb state.
  bool Thumb = false;
  /// The bytes its instructions, and the literal they read, occupy.
  uint8_t Size = 0;
  /// It is one direct jump, which also ends many a routine: unlike a
  /// long-branch thunk, its form alone does not make it a thunk.
  bool DirectJump = false;
};

/// The thunk at \p Address, entered in \p Mode (ARM or Thumb on 32-bit ARM,
/// Default elsewhere), when the image holds one there.
std::optional<LinkerThunk>
linkerThunkAt(const BinaryImage &Img, uint64_t Address, InstructionMode Mode);

/// The long-branch or interworking thunk at \p Address, entered in \p Mode:
/// one of the forms only a linker writes, never a direct jump.
std::optional<LinkerThunk> linkerLongBranchThunkAt(const BinaryImage &Img,
                                                   uint64_t Address,
                                                   InstructionMode Mode);

} // namespace neverd

#endif // NEVERD_LOADER_LINKERTHUNK_H
