//===- DecoderDetail.h - Private decoder target helpers --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Architecture-owned handle modes, reuse and instruction classification.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_DECODE_DECODERDETAIL_H
#define NEVERD_LIB_DECODE_DECODERDETAIL_H

#include "neverd/decode/Decoder.h"

#include "capstone/capstone.h"

namespace neverd::decoder_detail {

cs_mode x86HandleMode(Arch A);
cs_mode armHandleMode(InstructionMode Mode);

/// Return a reusable x86 handle with detail and text enabled, or zero.
csh takeX86Handle(cs_mode Mode);
/// Transfer ownership to the x86 cache, closing the handle if it is full.
void releaseX86Handle(csh Handle, Arch A);

bool isX86AlignmentNop(const DecodedInsn &Insn);
bool isARMInstructionAligned(va_t Addr, InstructionMode Mode);

} // namespace neverd::decoder_detail

#endif // NEVERD_LIB_DECODE_DECODERDETAIL_H
