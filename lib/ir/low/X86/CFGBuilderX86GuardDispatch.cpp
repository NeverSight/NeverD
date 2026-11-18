//===- CFGBuilderX86GuardDispatch.cpp - x64 Control Flow Guard dispatch ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x64 Control Flow Guard sends an indirect call or indirect tail jump
/// through the dispatch slot the load configuration names, with the target
/// in RAX: `mov rax, target; call [__guard_dispatch_icall_fptr]`, and for
/// eXtended Flow Guard `[__guard_xfg_dispatch_icall_fptr]` with the type hash
/// in R10.  The loader fills the slot with a dispatcher that checks RAX and
/// jumps to it with the caller's arguments, stack and return address intact,
/// or ends the process; while the guard is not enforced the slot keeps the
/// image's `_guard_dispatch_icall_nop`, a plain `jmp rax`.  An image whose
/// objects were compiled with /guard:cf but linked without it still calls
/// this way.  CFG construction lifts such a transfer as the one through RAX
/// it is: read through the slot, it would call the dispatcher with no
/// target.  A function that is only that jump, as ntoskrnl's
/// KeGuardDispatchICall is, keeps it: its RAX is its caller's, and its
/// callers already call the function in RAX (forwardsToGuardDispatch).
/// https://learn.microsoft.com/windows/win32/secbp/pe-metadata
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/X86/X86Regs.h"

namespace neverd {

namespace {
bool isGuardDispatchSlot(const BinaryImage &Img, va_t Slot) {
  return Img.hasRuntimeCallablePointerSlotAt(
             Slot, RuntimeCallablePointerSlotKind::GuardCFDispatch) ||
         Img.hasRuntimeCallablePointerSlotAt(
             Slot, RuntimeCallablePointerSlotKind::GuardXFGDispatch);
}
} // namespace

void CFGBuilder::rewriteGuardDispatchTransfer(const BinaryImage &Img,
                                              InsnRecord &Rec) {
  if (Img.Arch != Arch::X64 || Img.Format != BinaryFormat::COFF)
    return;
  constexpr uint16_t PtrSize = 8;
  const bool Forwarder = Rec.Addr == CurrentFuncEntry;
  for (LowOp &Op : Rec.Ops)
    if ((Op.Opcode == NdOp::INDIR_CALL ||
         (Op.Opcode == NdOp::INDIR_BR && !Forwarder)) &&
        Op.NumInputs >= 1 && Op.Inputs[0].isConst() &&
        Op.Inputs[0].Size == PtrSize &&
        isGuardDispatchSlot(Img, Op.Inputs[0].Offset))
      Op.Inputs[0] = NdVar::reg(x86reg::RAX, PtrSize);
}

} // namespace neverd
