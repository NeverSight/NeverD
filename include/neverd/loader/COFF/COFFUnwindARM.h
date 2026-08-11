//===- COFFUnwindARM.h - ARM and ARM64 unwind code decoding ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decodes the two forms Windows uses to describe an ARM or ARM64 frame.
///
/// Unlike x64, where every function carries an `UNWIND_INFO` record, ARM's
/// `.pdata` entry may describe the whole frame by itself.  A function whose
/// prologue follows the canonical shape gets *packed* unwind data: a handful
/// of bit fields naming how many registers of each file were saved and how big
/// the frame is, from which the exact prologue can be reconstructed.  Anything
/// else gets an `.xdata` record holding a byte string of *unwind codes*, each
/// standing one-to-one against a prologue instruction so that a frame can be
/// unwound from part-way through its own prologue.
///
/// Both forms are expanded here into the same normalized operations, because
/// which one a function got is a property of how simple its prologue is and
/// says nothing about what the frame contains.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_COFFUNWINDARM_H
#define NEVERD_LOADER_COFF_COFFUNWINDARM_H

#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/ArrayRef.h"

#include <string>
#include <vector>

namespace neverd::coff_loader {

/// The outcome of decoding one unwind code array.
struct ARMUnwindDecode {
  std::vector<UnwindOperation> Operations;
  /// Length in bytes of the prologue the operations describe, summed from the
  /// instructions they stand against.  Zero when the codes did not describe a
  /// prologue, which is what a scope whose first code is `end_c` means.
  uint32_t PrologueSize = 0;
  ExceptionParseStatus Status = ExceptionParseStatus::Complete;
  std::vector<std::string> Diagnostics;
};

/// Decode an ARM64 unwind code array.
///
/// \p Codes is the byte array as it appears in `.xdata`, and \p StartOffset the
/// byte index within it to begin at — an epilogue scope names its first code
/// by exactly that index, and the prologue always begins at zero.
///
/// Decoding stops at `end` or `end_c`.  Codes that run past the end of the
/// array, or that name a reserved encoding, produce a Partial result holding
/// everything decoded before them rather than discarding the frame: a
/// prologue's leading operations remain true whatever follows them.
ARMUnwindDecode decodeARM64UnwindCodes(llvm::ArrayRef<uint8_t> Codes,
                                       uint32_t StartOffset = 0);

/// Decode an ARM32 (Thumb-2) unwind code array.  \see decodeARM64UnwindCodes.
ARMUnwindDecode decodeARM32UnwindCodes(llvm::ArrayRef<uint8_t> Codes,
                                       uint32_t StartOffset = 0);

/// Expand the packed unwind data in the second `.pdata` word of an ARM64
/// runtime function into the canonical prologue it stands for.
///
/// \p PackedWord is the whole second word, flag bits included.
ARMUnwindDecode expandARM64PackedUnwind(uint32_t PackedWord);

/// Expand ARM32 packed unwind data.  \see expandARM64PackedUnwind.
ARMUnwindDecode expandARM32PackedUnwind(uint32_t PackedWord);

} // namespace neverd::coff_loader

#endif // NEVERD_LOADER_COFF_COFFUNWINDARM_H
