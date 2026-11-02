//===- CFGBuilderX86PLT.cpp - i386 PIC PLT entries ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A PLT entry of an i386 position-independent object jumps through its GOT
/// entry, `jmp *name@GOT(%ebx)`, by the address of the GOT, which the i386
/// psABI obliges every caller that reaches the entry to hold in EBX.  The
/// dynamic section names that address, DT_PLTGOT, by which the loader pairs
/// the entry with its import (BinaryImage::decodeImportThunkAt).  CFG
/// construction lifts the jump of such an entry as the jump through that
/// constant slot that a non-PIC entry, `jmp *name@GOT`, makes, so the entry
/// reaches its import as any other does rather than through whatever EBX
/// held on entry.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/support/ISAEncoding.h"

namespace neverd {

void CFGBuilder::rewriteI386PLTJump(const BinaryImage &Img, InsnRecord &Rec) {
  if (Img.Arch != Arch::X86 || !Img.DynInfo.PltGotAddr ||
      Rec.Size != x86::kJmpIndirectLen)
    return;
  const uint8_t *Bytes = Img.readVA(Rec.Addr, x86::kJmpIndirectLen);
  if (!Bytes || Bytes[0] != x86::kJmpIndirectOp ||
      Bytes[1] != x86::kJmpIndirectGOTModRM)
    return;
  const Import *Imp = Img.decodeImportThunkAt(Rec.Addr);
  if (!Imp)
    return;
  constexpr uint16_t PtrSize = 4;
  LowOp Jump;
  Jump.Opcode = NdOp::INDIR_BR;
  Jump.addInput(NdVar::cst(Imp->IATAddr, PtrSize));
  Jump.Addr = Rec.Addr;
  Jump.Seq = 0;
  Rec.Ops = {Jump};
}

} // namespace neverd
