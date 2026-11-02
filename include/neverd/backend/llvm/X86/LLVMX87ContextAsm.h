//===- LLVMX87ContextAsm.h - Owned complete x87 context assembly -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate native context snapshots and one original x87 operation.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_LLVM_X86_LLVMX87CONTEXTASM_H
#define NEVERD_BACKEND_LLVM_X86_LLVMX87CONTEXTASM_H

#include "neverd/ir/X86/X87Context.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/ErrorHandling.h"

namespace neverd {
enum class X87ContextAsmPhase : unsigned {
  Capture,
  SaveHost,
  Restore,
  RestoreHost,
  Step
};
inline constexpr char X87ContextAsmMetadata[] = "neverd.x87.context";
inline constexpr char X87ContextSnapshotConstraints[] = "r,~{memory}";
inline constexpr char X87ContextRestoreConstraints[] =
    "r,~{memory},~{st},~{st(1)},~{st(2)},~{st(3)},~{st(4)},~{st(5)},~{st(6)},~{"
    "st(7)},"
    "~{mm0},~{mm1},~{mm2},~{mm3},~{mm4},~{mm5},~{mm6},~{mm7},"
    "~{xmm0},~{xmm1},~{xmm2},~{xmm3},~{xmm4},~{xmm5},~{xmm6},~{xmm7},"
    "~{xmm8},~{xmm9},~{xmm10},~{xmm11},~{xmm12},~{xmm13},~{xmm14},~{xmm15}";
inline constexpr char X87ContextStepConstraints[] =
    "r,r,{r10},r,~{rax},~{r11},~{memory},~{flags},~{fpsr},"
    "~{st},~{st(1)},~{st(2)},~{st(3)},~{st(4)},~{st(5)},~{st(6)},~{st(7)},"
    "~{mm0},~{mm1},~{mm2},~{mm3},~{mm4},~{mm5},~{mm6},~{mm7},"
    "~{xmm0},~{xmm1},~{xmm2},~{xmm3},~{xmm4},~{xmm5},~{xmm6},~{xmm7},"
    "~{xmm8},~{xmm9},~{xmm10},~{xmm11},~{xmm12},~{xmm13},~{xmm14},~{xmm15}";

inline std::string
x87ContextAsm(X87ContextAsmPhase Phase, uint32_t Selector = 0,
              NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default) {
  switch (Phase) {
  case X87ContextAsmPhase::Capture:
    return "fxsave64 ($0)";
  case X87ContextAsmPhase::SaveHost:
    return "fxsave64 512($0)";
  case X87ContextAsmPhase::Restore:
    return "fxrstor64 ($0)";
  case X87ContextAsmPhase::RestoreHost:
    return "fxrstor64 512($0)";
  case X87ContextAsmPhase::Step:
    break;
  }
  auto Spec = x87ContextInstruction(Selector);
  if (!Spec)
    llvm::report_fatal_error("invalid x87 context selector");
  std::string Text = "fxrstor64 ($0)\n\t";
  if (Spec->ReadsFlags) {
    // Seed OF separately, then SAHF restores the other five arithmetic flags.
    // Avoid PUSHF/POPF: transient pushes would overwrite compiler red-zone
    // data.
    Text += "testw $$2048, ($3)\n\tjz 1f\n\t"
            "movl $$2147483647, %r11d\n\taddl $$1, %r11d\n\tjmp 2f\n\t"
            "1: xorl %r11d, %r11d\n\t2: movq $3, %rax\n\tmovb (%rax), "
            "%ah\n\tsahf\n\t";
  }
  if (Spec->WritesAX) {
    if (Selector & X87ContextWaitPrefix)
      Text += "fwait\n\t";
    Text += "fnstsw ($1)\n\t";
  } else {
    std::string Bytes;
    const auto add = [&](unsigned Byte) {
      if (!Bytes.empty())
        Bytes += ",";
      Bytes += std::to_string(Byte);
    };
    if (Selector & X87ContextWaitPrefix)
      add(0x9b);
    if (Selector & X87ContextOperand16)
      add(0x66);
    if (Selector & X87ContextMemory) {
      if (Space == NdMemoryAddressSpace::X86FS)
        add(0x64);
      else if (Space == NdMemoryAddressSpace::X86GS)
        add(0x65);
      else if (Space != NdMemoryAddressSpace::Default)
        llvm::report_fatal_error("unsupported x87 context address space");
      if (Selector & X87ContextStackReference) {
        // Keep the compiler's RBP intact. RBP + (address - RBP) retains the
        // original stack-reference fault class without changing the address.
        if (Space != NdMemoryAddressSpace::Default)
          llvm::report_fatal_error("invalid x87 stack-reference address");
        Text += "movq %r10, %r11\n\tsubq %rbp, %r11\n\t";
        add(0x42); // R11 index, RBP base, disp8=0.
      } else {
        add(0x41); // The admitted original memory group addresses R10.
      }
    }
    add(Selector & 0xff);
    if (Spec->ThreeDNow)
      add(0x0f);
    if ((Selector & 0xff) != 0x9b)
      add(((Selector >> 8) & 0xff) |
          ((Selector & X87ContextStackReference) ? 0x44
           : (Selector & X87ContextMemory)       ? 2
                                                 : 0));
    if (Selector & X87ContextStackReference) {
      add(0x1d);
      add(0);
    }
    if (Spec->ThreeDNow)
      add(x87ContextThreeDNowOpcode(Selector));
    Text += ".byte " + Bytes + "\n\t";
  }
  if (Spec->WritesFlags)
    Text += "lahf\n\tmovzbl %ah, %eax\n\tmovb %al, ($1)\n\tseto 1($1)\n\t";
  Text += "fxsave64 ($0)\n\tfxrstor64 512($0)";
  return Text;
}

struct X87ContextAsmIdentity {
  X87ContextAsmPhase Phase;
  uint32_t Selector;
  NdMemoryAddressSpace Space;
};

inline std::optional<X87ContextAsmIdentity>
classifyX87ContextAsm(const llvm::CallInst &Call) {
  const auto *MD = Call.getMetadata(X87ContextAsmMetadata);
  if (!MD)
    return std::nullopt;
  const auto Fail = []() -> std::optional<X87ContextAsmIdentity> {
    llvm::report_fatal_error("invalid owned x87 context assembly");
  };
  if (MD->getNumOperands() != 3)
    return Fail();
  uint64_t Values[3];
  for (unsigned I = 0; I < 3; ++I) {
    const auto *V =
        llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(I));
    if (!V || !V->getType()->isIntegerTy(32))
      return Fail();
    Values[I] = V->getZExtValue();
  }
  if (Values[0] > unsigned(X87ContextAsmPhase::Step) || Values[1] > UINT32_MAX)
    return Fail();
  X87ContextAsmIdentity Id{static_cast<X87ContextAsmPhase>(Values[0]),
                           uint32_t(Values[1]),
                           static_cast<NdMemoryAddressSpace>(Values[2])};
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  const bool Step = Id.Phase == X87ContextAsmPhase::Step;
  const bool Restore = Id.Phase == X87ContextAsmPhase::Restore ||
                       Id.Phase == X87ContextAsmPhase::RestoreHost;
  if (!Asm || !Asm->hasSideEffects() || Asm->isAlignStack() ||
      Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
      !Call.getType()->isVoidTy() || Call.getFunctionType()->isVarArg() ||
      Call.arg_size() != (Step ? 4U : 1U) ||
      Asm->getConstraintString() != (Step      ? X87ContextStepConstraints
                                     : Restore ? X87ContextRestoreConstraints
                                               : X87ContextSnapshotConstraints))
    return Fail();
  for (const auto &Arg : Call.args())
    if (!Arg->getType()->isPointerTy() ||
        Arg->getType()->getPointerAddressSpace())
      return Fail();
  if ((!Step && (Id.Selector || Id.Space != NdMemoryAddressSpace::Default)) ||
      (Step && !x87ContextInstruction(Id.Selector)))
    return Fail();
  if (Asm->getAsmString() != x87ContextAsm(Id.Phase, Id.Selector, Id.Space))
    return Fail();
  return Id;
}
} // namespace neverd
#endif
