//===- X86InstructionScan.h - Scoped x86 classification scan -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Batch classification over an immutable, explicitly bounded input range.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_X86INSTRUCTIONSCAN_H
#define NEVERD_DECODE_X86INSTRUCTIONSCAN_H

#include "neverd/decode/Decoder.h"
#include "neverd/decode/Limits.h"

#include "llvm/ADT/ArrayRef.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace neverd {

/// The input must remain alive and unchanged for this scan's lifetime. The
/// decoder must remain in the same x86 mode, with text/detail disabled when
/// calling decode(). Selected instructions may be decoded separately with
/// full operands between calls. No borrowed cs_insn is cached or returned.
class X86InstructionScan {
public:
  X86InstructionScan(Decoder &Dec, llvm::ArrayRef<uint8_t> Bytes, va_t Base)
      : Dec(Dec), Bytes(Bytes), Base(Base) {}

  /// Return size zero at an invalid input or outside the bounded range.
  /// A discontinuous offset starts a fresh batch at that exact address.
  ScannedInsn decode(size_t Offset) {
    if (Offset >= Bytes.size() || Dec.detailEnabled() || Dec.textEnabled())
      return {};
    if (Offset != NextOffset || Index == Count) {
      // A short batch ends at the input bound or the first invalid encoding.
      // The bound was checked above; do not decode the same failure again
      // after consuming its valid prefix. A seek starts a fresh scan.
      if (HaveBatch && Offset == NextOffset && Count < Batch.size())
        return {};
      Count = Dec.scanX86Light(Bytes.data() + Offset, Bytes.size() - Offset,
                               Base + Offset, Batch);
      Index = 0;
      NextOffset = Offset;
      HaveBatch = true;
    }
    if (!Count)
      return {};
    const ScannedInsn Result = ScannedInsn::unpack(Batch[Index++]);
    NextOffset = Offset + Result.Size;
    return Result;
  }

private:
  Decoder &Dec;
  llvm::ArrayRef<uint8_t> Bytes;
  va_t Base;
  std::array<uint32_t, limits::decode::kScanBatchInstructions> Batch;
  size_t NextOffset = 0;
  size_t Index = 0;
  size_t Count = 0;
  bool HaveBatch = false;
};

} // namespace neverd

#endif // NEVERD_DECODE_X86INSTRUCTIONSCAN_H
