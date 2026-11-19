//===- X64UnwindFrame.h - Windows x64 fixed frames ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// The fixed frame a Windows x64 function's unwind codes record.

#ifndef NEVERD_LOADER_COFF_X64UNWINDFRAME_H
#define NEVERD_LOADER_COFF_X64UNWINDFRAME_H

#include "neverd/loader/ExceptionFunction.h"

#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {

/// What the unwind codes of a Windows x64 function say about its frame,
/// relative to the stack pointer the function is entered with.
struct X64UnwindFrame {
  /// How far below the entry stack pointer the EstablisherFrame lies: the
  /// allocations the codes record or, with a frame register, those made
  /// before UWOP_SET_FPREG.  RtlVirtualUnwind computes this frame for the
  /// function and passes it to the function's handlers.
  uint64_t EstablisherBytes = 0;
  /// Every allocation the codes record.
  uint64_t AllocatedBytes = 0;
  /// The allocations, each by the prologue offset the codes give the
  /// instruction that makes it: the end of that instruction.
  std::map<uint32_t, uint64_t> Adjustments;
  /// The frame register, numbered as the codes number it, and the end of the
  /// prologue instruction that sets it.
  std::optional<uint16_t> FrameRegister;
  uint32_t SetFramePointerOffset = 0;
  /// Where the prologue keeps nonvolatile registers, pushed or saved, as byte
  /// ranges [first, second) relative to the entry stack pointer.
  std::vector<std::pair<int64_t, int64_t>> SavedRegisterSlots;
};

/// Reads the fixed frame of \p EH from its unwind codes: complete version 1
/// or 2 codes of a primary entry that is not chained and carries no flags
/// besides its handlers, whose operations each move the stack pointer by a
/// fixed amount, keep a register in a fixed slot or set the frame register,
/// and come in prologue order.  \p AllowFrameRegister admits a frame
/// register.  Each operation costs one unit of \p WorkRemaining when one is
/// given; running out refuses.
llvm::Expected<X64UnwindFrame>
decodeX64UnwindFrame(const ExceptionFunction &EH, bool AllowFrameRegister,
                     size_t *WorkRemaining = nullptr);

} // namespace neverd

#endif // NEVERD_LOADER_COFF_X64UNWINDFRAME_H
