//===- MedLLVMX86Sideeffect.cpp - x86 side-effect intrinsics --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86-specific side-effect intrinsic emission: debug traps (INT3/UD2),
/// fences (MFENCE/LFENCE/SFENCE), cache management (CLFLUSH/CLFLUSHOPT/CLWB),
/// prefetch, MPX bounds, and privileged instructions (CLI/STI/WRMSR/etc.).
///
/// Value-producing x86 intrinsics (CPUID, XGETBV, RDTSC, REP
/// string operations) live in MedLLVMX86ValueEmitter.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/MedLLVMInstructionText.h"
#include "neverd/backend/llvm/X86/LLVMX86StackEffects.h"
#include "neverd/backend/llvm/X86/LLVMX86X87StateAsm.h"
#include "neverd/backend/llvm/X86/X86DescriptorStateNative.h"
#include "neverd/backend/llvm/X86/X86ScalarSystemNative.h"
#include "neverd/backend/llvm/X86/X86SystemRegisterAsm.h"

#define DEBUG_TYPE "neverd-med-llvm-x86-sideeffect"
#include "neverd/ir/X86/X87Stack.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/X86/X86SegmentRegisters.h"
#include "neverd/ir/intrinsics/X86/X86Unsupported.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicsX86.h"

#include <utility>

namespace neverd {

//===----------------------------------------------------------------------===//
// Debug traps (INT3, INT1, INT N, INTO, UD2, UD0, UD1)
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitX86DebugTrap(const MedOp &Op, Intrinsic IC,
                                      llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  switch (IC) {
  case I::Int3: {
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::debugtrap);
    Builder.CreateCall(Fn, {});
    return true;
  }
  case I::Int1:
    // ICEBP raises #DB, whereas llvm.debugtrap lowers to INT3 (#BP).
    // Emit its byte because the LLVM assembler does not accept either alias.
    emitVoidInlineAsm(intrinsicAsmMnemonic(IC), Op, Builder);
    return true;
  case I::IntN: {
    // An interrupt with a register result is value-producing; the value
    // emitter binds that result.
    if (Op.Output.Size > 0)
      return false;
    auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
    // Windows `int 0x29` is `__fastfail(ecx)`.  Emit a named noreturn call
    // so LLVMC prints the intrinsic and does not treat the interrupt vector
    // (0x29 / 41) as the fail code.
    if (Op.NumInputs > 1 && Op.Inputs[1].isConst() &&
        (Op.Inputs[1].ConstVal & 0xFF) == 0x29) {
      auto *I32 = llvm::Type::getInt32Ty(*Ctx);
      auto *FnTy = llvm::FunctionType::get(VoidTy, {I32}, false);
      llvm::FunctionCallee Callee =
          Mod->getOrInsertFunction("__fastfail", FnTy);
      if (auto *Fn = llvm::dyn_cast<llvm::Function>(Callee.getCallee()))
        Fn->addFnAttr(llvm::Attribute::NoReturn);
      llvm::Value *Code = llvm::ConstantInt::get(I32, 0);
      if (Op.NumInputs >= 3) {
        Code = getVar(Op.Inputs[2], Builder);
        if (!Code->getType()->isIntegerTy())
          llvm::report_fatal_error("__fastfail code is not integer");
        if (Code->getType() != I32)
          Code = Builder.CreateZExtOrTrunc(Code, I32, "fastfail.code");
      }
      llvm::CallInst *Call = Builder.CreateCall(Callee, {Code});
      Call->setDoesNotReturn();
      return true;
    }
    // `int imm8` for every other vector.  The interrupt vector is an
    // immediate that MUST appear in the asm text, so bake it in via the `$0`
    // template with an immediate constraint rather than a register operand
    // (a bare "int" would be rejected by the assembler: too few operands).
    if (Op.NumInputs > 1 && Op.Inputs[1].isConst()) {
      auto *I8 = llvm::Type::getInt8Ty(*Ctx);
      auto *AsmFnTy = llvm::FunctionType::get(VoidTy, {I8}, false);
      auto *IA = llvm::InlineAsm::get(AsmFnTy, "int $0", "i,~{memory}",
                                      /*hasSideEffects=*/true);
      Builder.CreateCall(
          IA, {llvm::ConstantInt::get(I8, Op.Inputs[1].ConstVal & 0xFF)});
    } else {
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::debugtrap);
      Builder.CreateCall(Fn, {});
    }
    return true;
  }
  case I::Into: {
    // `into` — interrupt on overflow (32-bit only); no operand.
    auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
    auto *AsmFnTy = llvm::FunctionType::get(VoidTy, {}, false);
    auto *IA = llvm::InlineAsm::get(AsmFnTy, "into", "~{memory}",
                                    /*hasSideEffects=*/true);
    Builder.CreateCall(IA, {});
    return true;
  }
  case I::Ud2:
  case I::Ud0:
  case I::Ud1: {
    auto *Fn =
        llvm::Intrinsic::getOrInsertDeclaration(Mod, llvm::Intrinsic::trap);
    // A UD1 a sanitizer raised carries the check it names.
    markInstructionText(*Builder.CreateCall(Fn, {}), Op, IC);
    return true;
  }
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Memory fences (MFENCE, LFENCE, SFENCE, PAUSE, NOP)
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitX86Fence(const MedOp & /*Op*/, Intrinsic IC,
                                  llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  switch (IC) {
  case I::Mfence:
  case I::Lfence:
  case I::Sfence: {
    // LLVM's acquire/release fences can disappear on x86. In particular a
    // release fence does not drain MOVDIR/non-temporal stores. Keep the actual
    // hardware barrier, including LFENCE's execution-ordering semantics.
    const char *Mnemonic = IC == I::Mfence   ? "mfence"
                           : IC == I::Lfence ? "lfence"
                                             : "sfence";
    auto *Signature = llvm::FunctionType::get(Builder.getVoidTy(), false);
    Builder.CreateCall(
        llvm::InlineAsm::get(Signature, Mnemonic, "~{memory}", true));
    return true;
  }
  case I::Pause: {
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::x86_sse2_pause);
    Builder.CreateCall(Fn, {});
    return true;
  }
  case I::Nop:
    return true;
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Cache management (CLFLUSH, CLFLUSHOPT, CLWB, PREFETCH)
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitX86CacheOp(const MedOp &Op, Intrinsic IC,
                                    llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;

  const char *Mnemonic = nullptr;
  switch (IC) {
  case I::Clflush:
    Mnemonic = "clflush";
    break;
  case I::Clflushopt:
    Mnemonic = "clflushopt";
    break;
  case I::Clwb:
    Mnemonic = "clwb";
    break;
  case I::Prefetch:
  case I::PrefetchT0:
    Mnemonic = "prefetcht0";
    break;
  case I::PrefetchT1:
    Mnemonic = "prefetcht1";
    break;
  case I::PrefetchT2:
    Mnemonic = "prefetcht2";
    break;
  case I::PrefetchNta:
    Mnemonic = "prefetchnta";
    break;
  case I::PrefetchW:
    Mnemonic = "prefetchw";
    break;
  case I::PrefetchWT1:
    Mnemonic = "prefetchwt1";
    break;
  case I::Ldmxcsr:
    Mnemonic = "ldmxcsr";
    break;
  case I::Stmxcsr:
    Mnemonic = "stmxcsr";
    break;
  case I::Fxsave:
  case I::Fxrstor:
  case I::Fxsave64Mem:
  case I::Fxrstor64Mem:
  case I::X87Fldenv:
  case I::X87Fnstenv:
  case I::X87Frstor:
  case I::X87Fnsave:
  case I::Xsave:
  case I::Xsavec:
  case I::Xsaves:
  case I::Xsaveopt:
  case I::Xrstor:
  case I::Xrstors:
  case I::Xsave64:
  case I::Xsavec64:
  case I::Xsaves64:
  case I::Xsaveopt64:
  case I::Xrstor64:
  case I::Xrstors64:
    emitX86StateSnapshot(Op, IC, Builder);
    return true;
  default:
    break;
  }
  if (Mnemonic) {
    emitX86MemPtrAsm(Mnemonic, Op, Builder);
    return true;
  }

  return false;
}

//===----------------------------------------------------------------------===//
// Operand-carrying system instructions (LGDT/LIDT/SGDT/SIDT/INVLPG, XABORT)
//===----------------------------------------------------------------------===//

void MedLLVMEmitter::emitX86MemPtrAsm(const char *Mn, const MedOp &Op,
                                      llvm::IRBuilder<> &Builder) {
  auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
  if (Op.NumInputs <= 1)
    llvm::report_fatal_error("x86 memory intrinsic has no address operand");

  llvm::Value *Addr = Op.MemoryAddressSpace == NdMemoryAddressSpace::Default
                          ? getVar(Op.Inputs[1], Builder)
                          : getRawSegmentOffset(Op.Inputs[1], Builder);
  auto *NativeAddrTy =
      llvm::Type::getIntNTy(*Ctx, TargetArch == Arch::X86 ? 32 : 64);
  if (Addr->getType()->isPointerTy())
    Addr = Builder.CreatePtrToInt(Addr, NativeAddrTy, "memory_offset");
  if (!Addr->getType()->isIntegerTy())
    llvm::report_fatal_error("x86 memory intrinsic address is not integral");
  if (Addr->getType() != NativeAddrTy)
    Addr =
        Builder.CreateZExtOrTrunc(Addr, NativeAddrTy, "native_memory_offset");

  const char *Segment = "";
  if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS)
    Segment = "%fs:";
  else if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86GS)
    Segment = "%gs:";
  else if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    llvm::report_fatal_error("unknown x86 memory intrinsic address space");

  std::vector<llvm::Type *> ParamTys{Addr->getType()};
  std::vector<llvm::Value *> ParamVals{Addr};
  std::string Constraints = "r";
  Constraints += ",~{memory}";
  auto *AsmFnTy = llvm::FunctionType::get(VoidTy, ParamTys, false);
  auto *IA = llvm::InlineAsm::get(
      AsmFnTy, std::string(Mn) + " " + Segment + "($0)", Constraints,
      /*hasSideEffects=*/true);
  Builder.CreateCall(IA, ParamVals);
}

// The processor's x87/SSE/AVX state, as the source's _xsave64/_fxrstor
// intrinsics name it.  Lifted values live in SSA registers, not in that
// state, so HighC marks the statement; here the LLVM intrinsic is the same
// source operation.
void MedLLVMEmitter::emitX86StateSnapshot(const MedOp &Op, Intrinsic IC,
                                          llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  llvm::Intrinsic::ID ID = llvm::Intrinsic::not_intrinsic;
  bool TakesMask = true;
  switch (IC) {
  case I::Fxsave:
    ID = llvm::Intrinsic::x86_fxsave;
    TakesMask = false;
    break;
  case I::Fxrstor:
    ID = llvm::Intrinsic::x86_fxrstor;
    TakesMask = false;
    break;
  case I::Fxsave64Mem:
    ID = llvm::Intrinsic::x86_fxsave64;
    TakesMask = false;
    break;
  case I::Fxrstor64Mem:
    ID = llvm::Intrinsic::x86_fxrstor64;
    TakesMask = false;
    break;
  case I::Xsave:
    ID = llvm::Intrinsic::x86_xsave;
    break;
  case I::Xsavec:
    ID = llvm::Intrinsic::x86_xsavec;
    break;
  case I::Xsaves:
    ID = llvm::Intrinsic::x86_xsaves;
    break;
  case I::Xsaveopt:
    ID = llvm::Intrinsic::x86_xsaveopt;
    break;
  case I::Xrstor:
    ID = llvm::Intrinsic::x86_xrstor;
    break;
  case I::Xrstors:
    ID = llvm::Intrinsic::x86_xrstors;
    break;
  case I::Xsave64:
    ID = llvm::Intrinsic::x86_xsave64;
    break;
  case I::Xsavec64:
    ID = llvm::Intrinsic::x86_xsavec64;
    break;
  case I::Xsaves64:
    ID = llvm::Intrinsic::x86_xsaves64;
    break;
  case I::Xsaveopt64:
    ID = llvm::Intrinsic::x86_xsaveopt64;
    break;
  case I::Xrstor64:
    ID = llvm::Intrinsic::x86_xrstor64;
    break;
  case I::Xrstors64:
    ID = llvm::Intrinsic::x86_xrstors64;
    break;
  default:
    // x87 environment forms have no LLVM intrinsic.
    emitX86MemPtrAsm(intrinsicAsmMnemonic(IC), Op, Builder);
    return;
  }
  if (Op.NumInputs < (TakesMask ? 4 : 2))
    llvm::report_fatal_error(
        "x86 state save/restore intrinsic is missing its operands");
  if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
    if (!TakesMask) {
      emitX86MemPtrAsm(intrinsicAsmMnemonic(IC), Op, Builder);
      return;
    }
    // No intrinsic takes a segment-relative area; keep the override in asm.
    auto *I32 = llvm::Type::getInt32Ty(*Ctx);
    llvm::Value *Offset = getRawSegmentOffset(Op.Inputs[1], Builder);
    auto *OffsetTy = llvm::Type::getInt64Ty(*Ctx);
    if (Offset->getType()->isPointerTy())
      Offset = Builder.CreatePtrToInt(Offset, OffsetTy, "state_offset");
    Offset = Builder.CreateZExtOrTrunc(Offset, OffsetTy, "state_offset");
    llvm::Value *Lo =
        Builder.CreateZExtOrTrunc(getVar(Op.Inputs[2], Builder), I32, "lo");
    llvm::Value *Hi =
        Builder.CreateZExtOrTrunc(getVar(Op.Inputs[3], Builder), I32, "hi");
    const char *Segment =
        Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS ? "%fs:" : "%gs:";
    auto *AsmTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*Ctx),
                                          {OffsetTy, I32, I32}, false);
    auto *IA = llvm::InlineAsm::get(
        AsmTy, std::string(intrinsicAsmMnemonic(IC)) + " " + Segment + "($0)",
        "r,{eax},{edx},~{memory}", /*hasSideEffects=*/true);
    Builder.CreateCall(IA, {Offset, Lo, Hi});
    return;
  }
  llvm::Value *Addr = getVar(Op.Inputs[1], Builder);
  auto *PtrTy = llvm::PointerType::getUnqual(*Ctx);
  if (!Addr->getType()->isPointerTy())
    Addr = Builder.CreateIntToPtr(Addr, PtrTy, "state_area");
  std::vector<llvm::Value *> Args{Addr};
  if (TakesMask) {
    auto *I32 = llvm::Type::getInt32Ty(*Ctx);
    // LLVM's XSAVE intrinsics take the EDX half first, then EAX.
    Args.push_back(
        Builder.CreateZExtOrTrunc(getVar(Op.Inputs[3], Builder), I32, "hi"));
    Args.push_back(
        Builder.CreateZExtOrTrunc(getVar(Op.Inputs[2], Builder), I32, "lo"));
  }
  Builder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(Mod, ID), Args);
}

bool MedLLVMEmitter::emitX86SystemAsm(const MedOp &Op, Intrinsic IC,
                                      llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;

  // OUT port, acc.
  if (IC == I::Out) {
    emitX86ScalarSystem(Op, IC, Builder);
    return true;
  }

  // BOUND idx, (base) — 32-bit range check (side-effect).
  if (IC == I::Bound) {
    if (Op.NumInputs >= 3) {
      auto *Idx = getVar(Op.Inputs[1], Builder);
      auto *Base = getVar(Op.Inputs[2], Builder);
      auto *PtrTy = llvm::PointerType::get(*Ctx, 0);
      auto *P = Builder.CreateIntToPtr(Base, PtrTy, "bound_ptr");
      auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
      auto *FnTy =
          llvm::FunctionType::get(VoidTy, {Idx->getType(), PtrTy}, false);
      auto *IA = llvm::InlineAsm::get(FnTy, "bound $0, ($1)", "r,r,~{memory}",
                                      /*hasSideEffects=*/true);
      Builder.CreateCall(IA, {Idx, P});
    }
    return true;
  }

  return false;
}

// Descriptor memory accesses remain inside their native instruction.
llvm::Value *
MedLLVMEmitter::emitX86DescriptorState(const MedOp &Op, Intrinsic IC,
                                       llvm::IRBuilder<> &Builder) {
  if (!x86DescriptorStateMedShapeIsValid(Op, TargetArch) ||
      Op.Inputs[0].ConstVal != unsigned(IC))
    llvm::report_fatal_error(
        llvm::Twine("invalid x86 descriptor state contract: ") +
        intrinsicName(IC) + ", inputs=" + llvm::Twine(unsigned(Op.NumInputs)) +
        ", output bytes=" + llvm::Twine(Op.Output.Size) + ", output kind=" +
        llvm::Twine(unsigned(Op.Output.Kind)) + ", operand bytes=" +
        llvm::Twine(Op.NumInputs > 1 ? Op.Inputs[1].Size : 0) +
        ", operand kind=" +
        llvm::Twine(Op.NumInputs > 1 ? unsigned(Op.Inputs[1].Kind) : 0));
  const auto P = x86DescriptorStateNative(
      IC, TargetArch, Op.NumInputs == 2 && Op.Inputs[1].Size == 8,
      Op.Output.Size, Op.MemoryAddressSpace);
  if (!P)
    llvm::report_fatal_error("invalid x86 descriptor state mode");
  llvm::SmallVector<llvm::Value *, 3> Arguments;
  if (P->OperandBytes) {
    llvm::Value *Value;
    if (P->Memory && Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
      Value = resolveAtomicMemoryPtr(Op.Inputs[1], P->MemoryBytes,
                                     Builder.getInt8Ty(), Builder);
    else
      Value = P->Memory ? getRawSegmentOffset(Op.Inputs[1], Builder)
                        : getVar(Op.Inputs[1], Builder);
    if (!P->Memory || P->Address) {
      auto *Ty = sizeToType(P->OperandBytes);
      Value = Value->getType()->isPointerTy()
                  ? Builder.CreatePtrToInt(Value, Ty)
                  : Builder.CreateZExtOrTrunc(Value, Ty);
    }
    Arguments.push_back(Value);
  }
  const auto Native = llvm::Intrinsic::lookupIntrinsicID(P->LLVMName);
  if (Native != llvm::Intrinsic::not_intrinsic) {
    if (P->Address) {
      Arguments.push_back(Builder.getInt32(P->Address->Bits));
      Arguments.push_back(Builder.getInt32(P->Address->Prefix));
    }
    return Builder.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(Mod, Native), Arguments);
  }
  llvm::SmallVector<llvm::Type *, 1> Types;
  for (const auto *Value : Arguments)
    Types.push_back(Value->getType());
  auto *ResultTy =
      P->ResultBytes ? sizeToType(P->ResultBytes) : Builder.getVoidTy();
  auto *FnTy = llvm::FunctionType::get(ResultTy, Types, false);
  return Builder.CreateCall(
      llvm::InlineAsm::get(FnTy, P->Text, P->Constraints, true), Arguments);
}

//===----------------------------------------------------------------------===//
// Scalar MSR and port I/O (RDMSR / WRMSR / IN / OUT)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitX86ScalarSystem(const MedOp &Op, Intrinsic IC,
                                                 llvm::IRBuilder<> &Builder) {
  const bool Write = IC == Intrinsic::Wrmsr || IC == Intrinsic::Out;
  const unsigned Bytes =
      Write && Op.NumInputs == 3 ? Op.Inputs[2].Size : Op.Output.Size;
  const auto P = x86ScalarSystemNative(IC, TargetArch, Bytes);
  if (!P || !hasValidMedIntrinsicId(Op) ||
      Op.Inputs[0].ConstVal != unsigned(IC) ||
      Op.NumInputs != P->arguments() + 1 ||
      Op.Output.Size != (Write ? 0 : Bytes) || !Op.IntrinsicOutputs.empty() ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    llvm::report_fatal_error("invalid scalar x86 system instruction shape");
  llvm::SmallVector<llvm::Value *, 2> Arguments;
  for (unsigned I = 0; I < P->arguments(); ++I) {
    const auto &Input = Op.Inputs[I + 1];
    // The lifter retains an imm8 port as one byte. A propagated DX constant
    // still has all 16 bits; it must never be silently truncated to imm8.
    const bool ImmediatePort = P->Port && I == 0 && Input.Size == 1 &&
                               Input.isConst() && Input.ConstVal <= UINT8_MAX;
    if (Input.Size != P->argumentBytes(I) && !ImmediatePort)
      llvm::report_fatal_error("invalid scalar x86 system operand width");
    auto *Value = getVar(Input, Builder);
    if (!Value->getType()->isIntegerTy())
      llvm::report_fatal_error("scalar x86 system operand is not an integer");
    Arguments.push_back(
        Builder.CreateZExtOrTrunc(Value, sizeToType(P->argumentBytes(I))));
  }
  if (auto ID = llvm::Intrinsic::lookupIntrinsicID(P->LLVMName))
    return Builder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(Mod, ID),
                              Arguments);

  auto *ResultTy = Write ? llvm::Type::getVoidTy(*Ctx) : sizeToType(Bytes);
  std::string Text = P->Mnemonic, Constraints;
  if (P->Port) {
    const auto *Port = llvm::dyn_cast<llvm::ConstantInt>(Arguments[0]);
    const bool Immediate = Port && Port->getZExtValue() <= UINT8_MAX;
    if (Immediate)
      Arguments[0] = Builder.CreateTrunc(Arguments[0], sizeToType(1));
    Constraints = std::string(Write ? "{ax}," : "={ax},") +
                  (Immediate ? "N" : "{dx}") + ",~{memory}";
    Text += Write ? " $0, $1" : " $1, $0";
    if (Write)
      std::swap(Arguments[0], Arguments[1]);
  } else if (Write) {
    auto *Value = Arguments.pop_back_val();
    Arguments.push_back(Builder.CreateTrunc(Value, sizeToType(4)));
    Arguments.push_back(
        Builder.CreateTrunc(Builder.CreateLShr(Value, 32), sizeToType(4)));
    Constraints = "{ecx},{eax},{edx},~{memory}";
  } else if (TargetArch == Arch::X64) {
    Text += "\n\tshlq $$32, %rdx\n\torq %rdx, %rax";
    Constraints = "={rax},{ecx},~{rdx},~{memory}";
  } else {
    Constraints = "=A,{ecx},~{memory}";
  }
  llvm::SmallVector<llvm::Type *, 3> Types;
  for (const auto *Value : Arguments)
    Types.push_back(Value->getType());
  auto *FunctionTy = llvm::FunctionType::get(ResultTy, Types, false);
  auto *Asm = llvm::InlineAsm::get(FunctionTy, Text, Constraints,
                                   /*hasSideEffects=*/true);
  return Builder.CreateCall(Asm, Arguments);
}

//===----------------------------------------------------------------------===//
// Privileged / system instructions (CLI, STI, INVLPG, WRMSR, etc.)
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitX86Privileged(const MedOp &Op, Intrinsic IC,
                                       llvm::IRBuilder<> &Builder) {
  if (x86SystemRegisterName(IC, 0)) {
    const auto P = x86SystemRegisterAsm(
        IC, TargetArch,
        Op.NumInputs > 1 && Op.Inputs[1].isConst() ? Op.Inputs[1].ConstVal
                                                   : UINT64_MAX);
    // A read is handled by the value emitter, including a dead result.
    if (P && !P->Write)
      return false;
    const uint16_t ValueInput = IC == Intrinsic::WriteDr ? 2 : 1;
    if (!P || Op.NumInputs != ValueInput + 1 || Op.Output.Size != 0 ||
        Op.Inputs[ValueInput].Size != P->Bytes ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      llvm::report_fatal_error("invalid x86 system register write contract");
    llvm::Value *Value = getVar(Op.Inputs[ValueInput], Builder);
    if (Value->getType()->isPointerTy())
      Value =
          Builder.CreatePtrToInt(Value, sizeToType(P->Bytes), "sysreg_value");
    if (!Value->getType()->isIntegerTy(8 * P->Bytes))
      llvm::report_fatal_error(
          "x86 system register write has a non-native value");
    if (const auto IID = llvm::Intrinsic::lookupIntrinsicID(P->LLVMName)) {
      Builder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(Mod, IID),
                         {Builder.getInt32(P->Index), Value});
      return true;
    }
    auto *FnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*Ctx),
                                         {Value->getType()}, false);
    auto *IA = llvm::InlineAsm::get(FnTy, P->Text, P->Constraints,
                                    /*hasSideEffects=*/true);
    Builder.CreateCall(IA, {Value});
    return true;
  }
  using I = Intrinsic;
  if (IC == I::Insb || IC == I::Insw || IC == I::Insd) {
    if (Op.NumInputs < 5 || Op.Output.Size != 0 ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      llvm::report_fatal_error(
          "INS intrinsic has an invalid operand/output shape");

    const unsigned NativeAddressBytes = TargetArch == Arch::X64 ? 8 : 4;
    const unsigned StringAddressBytes = Op.Inputs[1].Size;
    std::string Mnemonic = "rep ";
    Mnemonic += intrinsicAsmMnemonic(IC);
    if (StringAddressBytes != NativeAddressBytes) {
      if (StringAddressBytes == 4 && NativeAddressBytes == 8)
        Mnemonic = "addr32 " + Mnemonic;
      else if (StringAddressBytes == 2 && NativeAddressBytes == 4)
        Mnemonic = "addr16 " + Mnemonic;
      else
        llvm::report_fatal_error("unsupported INS address size");
    }

    auto *AddrTy = llvm::Type::getIntNTy(*Ctx, NativeAddressBytes * 8);
    auto *I16Ty = llvm::Type::getInt16Ty(*Ctx);
    auto Coerce = [&](llvm::Value *Value, llvm::Type *Ty) {
      return Value->getType() == Ty ? Value
                                    : Builder.CreateZExtOrTrunc(Value, Ty);
    };
    auto *RDI = Coerce(getVar(Op.Inputs[1], Builder), AddrTy);
    auto *RCX = Coerce(getVar(Op.Inputs[2], Builder), AddrTy);
    auto *RDX = Coerce(getVar(Op.Inputs[3], Builder), I16Ty);
    const MedVar &DfVar = Op.Inputs[4];
    const bool DynamicDf = !DfVar.isConst();
    if (DfVar.isConst() && DfVar.ConstVal != 0)
      Mnemonic = "std\n\t" + Mnemonic + "\n\tcld";
    else if (DynamicDf)
      Mnemonic = "test $5,$5\n\tje 1f\n\tstd\n\t1:\n\t" + Mnemonic + "\n\tcld";

    auto *RetTy = llvm::StructType::get(*Ctx, {AddrTy, AddrTy});
    llvm::CallInst *Call = nullptr;
    if (DynamicDf) {
      auto *DF = Coerce(getVar(DfVar, Builder), AddrTy);
      auto *FnTy = llvm::FunctionType::get(
          RetTy, {AddrTy, AddrTy, I16Ty, AddrTy}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mnemonic,
          "={di},={cx},0,1,{dx},r,~{memory},~{dirflag},~{flags}", true);
      Call = Builder.CreateCall(IA, {RDI, RCX, RDX, DF}, "rep_ins");
    } else {
      auto *FnTy =
          llvm::FunctionType::get(RetTy, {AddrTy, AddrTy, I16Ty}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mnemonic, "={di},={cx},0,1,{dx},~{memory},~{dirflag},~{flags}",
          true);
      Call = Builder.CreateCall(IA, {RDI, RCX, RDX}, "rep_ins");
    }
    (void)Call;
    return true;
  }

  if (IC == I::Outsb || IC == I::Outsw || IC == I::Outsd) {
    if (Op.NumInputs < 5 || Op.Output.Size != 0)
      llvm::report_fatal_error(
          "OUTS intrinsic has an invalid operand/output shape");

    const unsigned NativeAddressBytes = TargetArch == Arch::X64 ? 8 : 4;
    const unsigned StringAddressBytes = Op.Inputs[1].Size;
    std::string Mnemonic = "rep ";
    Mnemonic += intrinsicAsmMnemonic(IC);
    switch (Op.MemoryAddressSpace) {
    case NdMemoryAddressSpace::Default:
      break;
    case NdMemoryAddressSpace::X86FS:
      Mnemonic = "fs " + Mnemonic;
      break;
    case NdMemoryAddressSpace::X86GS:
      Mnemonic = "gs " + Mnemonic;
      break;
    }
    if (StringAddressBytes != NativeAddressBytes) {
      if (StringAddressBytes == 4 && NativeAddressBytes == 8)
        Mnemonic = "addr32 " + Mnemonic;
      else if (StringAddressBytes == 2 && NativeAddressBytes == 4)
        Mnemonic = "addr16 " + Mnemonic;
      else
        llvm::report_fatal_error("unsupported OUTS address size");
    }

    auto *AddrTy = llvm::Type::getIntNTy(*Ctx, NativeAddressBytes * 8);
    auto *I16Ty = llvm::Type::getInt16Ty(*Ctx);
    auto Coerce = [&](llvm::Value *Value, llvm::Type *Ty) {
      return Value->getType() == Ty ? Value
                                    : Builder.CreateZExtOrTrunc(Value, Ty);
    };
    auto *RSI = Coerce(getVar(Op.Inputs[1], Builder), AddrTy);
    auto *RCX = Coerce(getVar(Op.Inputs[2], Builder), AddrTy);
    auto *RDX = Coerce(getVar(Op.Inputs[3], Builder), I16Ty);
    const MedVar &DfVar = Op.Inputs[4];
    const bool DynamicDf = !DfVar.isConst();
    if (DfVar.isConst() && DfVar.ConstVal != 0)
      Mnemonic = "std\n\t" + Mnemonic + "\n\tcld";
    else if (DynamicDf)
      Mnemonic = "test $5,$5\n\tje 1f\n\tstd\n\t1:\n\t" + Mnemonic + "\n\tcld";

    auto *RetTy = llvm::StructType::get(*Ctx, {AddrTy, AddrTy});
    llvm::CallInst *Call = nullptr;
    if (DynamicDf) {
      auto *DF = Coerce(getVar(DfVar, Builder), AddrTy);
      auto *FnTy = llvm::FunctionType::get(
          RetTy, {AddrTy, AddrTy, I16Ty, AddrTy}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mnemonic,
          "={si},={cx},0,1,{dx},r,~{memory},~{dirflag},~{flags}", true);
      Call = Builder.CreateCall(IA, {RSI, RCX, RDX, DF}, "rep_outs");
    } else {
      auto *FnTy =
          llvm::FunctionType::get(RetTy, {AddrTy, AddrTy, I16Ty}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mnemonic, "={si},={cx},0,1,{dx},~{memory},~{dirflag},~{flags}",
          true);
      Call = Builder.CreateCall(IA, {RSI, RCX, RDX}, "rep_outs");
    }
    (void)Call;
    return true;
  }

  // POPF writes the whole EFLAGS image, including the system flags.
  if (IC == I::Popf) {
    if (Op.NumInputs != 2)
      llvm::report_fatal_error("x86 POPF has no EFLAGS image");
    const bool Wide = TargetArch == Arch::X64;
    auto *AsmTy =
        Wide ? llvm::Type::getInt64Ty(*Ctx) : llvm::Type::getInt32Ty(*Ctx);
    llvm::Value *Flags =
        Builder.CreateZExtOrTrunc(getVar(Op.Inputs[1], Builder), AsmTy);
    auto *FnTy =
        llvm::FunctionType::get(llvm::Type::getVoidTy(*Ctx), {AsmTy}, false);
    auto *IA = llvm::InlineAsm::get(
        FnTy, Wide ? "pushq $0\n\tpopfq" : "pushl $0\n\tpopfl",
        "r,~{memory},~{dirflag},~{flags}", /*hasSideEffects=*/true);
    markX86StackTemporary(*Builder.CreateCall(IA, {Flags}));
    return true;
  }

  // WRMSR writes EDX:EAX to the MSR selected by ECX.
  if (IC == I::Wrmsr) {
    emitX86ScalarSystem(Op, IC, Builder);
    return true;
  }

  // A segment selector write; a read with a live result is lowered by the
  // value emitter, and a dead read has no effect to keep.
  if (IC == I::ReadSegment || IC == I::WriteSegment) {
    const char *Segment = Op.NumInputs >= 2 && Op.Inputs[1].isConst()
                              ? x86SegmentRegisterName(Op.Inputs[1].ConstVal)
                              : nullptr;
    if (!Segment || (IC == I::WriteSegment && Op.NumInputs != 3))
      llvm::report_fatal_error("x86 segment move has an invalid operand shape");
    if (IC == I::ReadSegment)
      return Op.Output.Size == 0;
    auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
    llvm::Value *Value =
        Builder.CreateZExtOrTrunc(getVar(Op.Inputs[2], Builder), I32Ty);
    auto *FnTy =
        llvm::FunctionType::get(llvm::Type::getVoidTy(*Ctx), {I32Ty}, false);
    auto *IA =
        llvm::InlineAsm::get(FnTy, std::string("mov ${0:w}, %") + Segment,
                             "r,~{memory}", /*hasSideEffects=*/true);
    Builder.CreateCall(IA, {Value});
    return true;
  }

  // Instructions reading fixed registers: the operands go to those registers.
  // A hypercall with a live RAX result is lowered by the value emitter.
  if (IC == I::Monitor || IC == I::Monitorx || IC == I::Mwait ||
      IC == I::Mwaitx || IC == I::Xsetbv ||
      ((IC == I::Vmcall || IC == I::Vmmcall) && Op.Output.Size == 0)) {
    std::vector<const char *> Regs;
    switch (IC) {
    case I::Monitor:
    case I::Monitorx:
      Regs = {"{rax}", "{ecx}", "{edx}"};
      break;
    case I::Mwait:
      Regs = {"{eax}", "{ecx}"};
      break;
    case I::Mwaitx:
      Regs = {"{eax}", "{ecx}", "{ebx}"};
      break;
    case I::Xsetbv:
      Regs = {"{ecx}", "{eax}", "{edx}"};
      break;
    default:
      Regs = {"{rcx}", "{rdx}", "{r8}"};
      break;
    }
    std::vector<llvm::Value *> Vals;
    if (IC == I::Xsetbv) {
      if (Op.NumInputs != 3)
        llvm::report_fatal_error("x86 XSETBV has an invalid operand shape");
      auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
      auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
      llvm::Value *Value =
          Builder.CreateZExtOrTrunc(getVar(Op.Inputs[2], Builder), I64Ty);
      Vals = {
          Builder.CreateZExtOrTrunc(getVar(Op.Inputs[1], Builder), I32Ty),
          Builder.CreateTrunc(Value, I32Ty, "xcr_lo"),
          Builder.CreateTrunc(Builder.CreateLShr(Value, 32), I32Ty, "xcr_hi")};
    } else {
      if (Op.NumInputs != 1 + Regs.size())
        llvm::report_fatal_error(
            "x86 implicit-register instruction has an invalid operand shape");
      for (uint16_t I = 1; I < Op.NumInputs; ++I)
        Vals.push_back(getVar(Op.Inputs[I], Builder));
    }
    std::vector<llvm::Type *> Tys;
    std::string Cons;
    for (size_t I = 0; I < Vals.size(); ++I) {
      Tys.push_back(Vals[I]->getType());
      Cons += std::string(I ? "," : "") + Regs[I];
    }
    Cons += ",~{memory}";
    auto *FnTy =
        llvm::FunctionType::get(llvm::Type::getVoidTy(*Ctx), Tys, false);
    auto *IA = llvm::InlineAsm::get(FnTy, intrinsicAsmMnemonic(IC), Cons,
                                    /*hasSideEffects=*/true);
    Builder.CreateCall(IA, Vals);
    return true;
  }

  switch (IC) {
  case I::Cli:
  case I::Sti:
  case I::Wrpkru:
  case I::Hlt:
  case I::Invd:
  case I::Swapgs:
  case I::Wbinvd:
  case I::Syscall: {
    const char *Mn = intrinsicAsmMnemonic(IC);
    if (Mn)
      emitVoidInlineAsm(Mn, Op, Builder);
    return true;
  }
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Top-level x86 side-effect dispatch
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitX86Sideeffect(const MedOp &Op, Intrinsic IC,
                                       llvm::IRBuilder<> &Builder) {
  if (const auto Effect = x87StateEffectOfIntrinsic(IC)) {
    if (Op.Output.Size != 0 ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      llvm::report_fatal_error(
          "invalid x87 state intrinsic output/address space");
    std::string Mnemonic;
    const auto Stack = x87StackEffect(IC);
    if (Stack && Stack->FreesOperand) {
      if (Op.NumInputs != 3 || Op.Inputs[1].Size != 10 ||
          !Op.Inputs[2].isConst() || Op.Inputs[2].Size != 1 ||
          Op.Inputs[2].ConstVal >= 8)
        llvm::report_fatal_error("invalid x87 free-register operand contract");
      Mnemonic = std::string(intrinsicAsmMnemonic(IC)) + " %st(" +
                 std::to_string(Op.Inputs[2].ConstVal) + ")";
    } else {
      if (Op.NumInputs != 1)
        llvm::report_fatal_error(
            "invalid x87 state intrinsic operand contract");
      Mnemonic = intrinsicAsmMnemonic(IC);
    }
    // Emptying a register or moving TOP invalidates every x87 value LLVM
    // might keep in the register stack; the constraints say so.
    auto *FnTy =
        llvm::FunctionType::get(llvm::Type::getVoidTy(*Ctx), {}, false);
    auto *IA =
        llvm::InlineAsm::get(FnTy, Mnemonic, x87StateConstraints(*Effect),
                             /*hasSideEffects=*/true);
    Builder.CreateCall(IA, {});
    return true;
  }
  if (IC == Intrinsic::X86RequireDivPrecondition) {
    if (!intrinsicX86DivPreconditionShapeIsValid(
            IC, x86DivPreconditionMedShape(Op)))
      llvm::report_fatal_error("invalid x86 divide precondition intrinsic");

    llvm::Value *Dividend = getVar(Op.Inputs[1], Builder);
    llvm::Value *Divisor = getVar(Op.Inputs[2], Builder);
    auto *FullTy = llvm::dyn_cast<llvm::IntegerType>(Dividend->getType());
    auto *HalfTy = llvm::dyn_cast<llvm::IntegerType>(Divisor->getType());
    if (!FullTy || !HalfTy || FullTy->getBitWidth() != Op.Inputs[1].Size * 8 ||
        HalfTy->getBitWidth() != Op.Inputs[2].Size * 8 ||
        FullTy->getBitWidth() != HalfTy->getBitWidth() * 2)
      llvm::report_fatal_error(
          "x86 divide precondition operands are not matching integers");

    const unsigned FullBits = FullTy->getBitWidth();
    const unsigned HalfBits = HalfTy->getBitWidth();
    llvm::Value *DivisorZero = Builder.CreateICmpEQ(
        Divisor, llvm::ConstantInt::get(HalfTy, 0), "divisor.zero");
    const bool IsSigned =
        Op.Inputs[3].ConstVal == static_cast<uint64_t>(X86DivKind::Signed);
    llvm::Value *Overflow = nullptr;
    if (!IsSigned) {
      llvm::Value *High = Builder.CreateTrunc(
          Builder.CreateLShr(Dividend,
                             llvm::ConstantInt::get(FullTy, HalfBits)),
          HalfTy, "dividend.high");
      Overflow = Builder.CreateOr(
          DivisorZero,
          Builder.CreateICmpUGE(High, Divisor, "quotient.overflow"),
          "divide.error");
    } else {
      llvm::Value *DividendNegative = Builder.CreateICmpSLT(
          Dividend, llvm::ConstantInt::get(FullTy, 0), "dividend.negative");
      llvm::Value *DivisorNegative = Builder.CreateICmpSLT(
          Divisor, llvm::ConstantInt::get(HalfTy, 0), "divisor.negative");
      llvm::Value *DividendMagnitude =
          Builder.CreateSelect(DividendNegative, Builder.CreateNeg(Dividend),
                               Dividend, "dividend.magnitude");
      llvm::Value *ExtendedDivisor =
          Builder.CreateSExt(Divisor, FullTy, "divisor.extended");
      llvm::Value *DivisorMagnitude = Builder.CreateSelect(
          DivisorNegative, Builder.CreateNeg(ExtendedDivisor), ExtendedDivisor,
          "divisor.magnitude");
      llvm::Value *NegativeQuotient = Builder.CreateXor(
          DividendNegative, DivisorNegative, "quotient.negative");
      llvm::APInt PositiveLimit =
          llvm::APInt::getOneBitSet(FullBits, HalfBits - 1);
      llvm::Value *Limit = Builder.CreateSelect(
          NegativeQuotient, llvm::ConstantInt::get(FullTy, PositiveLimit + 1),
          llvm::ConstantInt::get(FullTy, PositiveLimit), "quotient.limit");
      llvm::Value *Threshold =
          Builder.CreateMul(DivisorMagnitude, Limit, "quotient.threshold");
      Overflow =
          Builder.CreateOr(DivisorZero,
                           Builder.CreateICmpUGE(DividendMagnitude, Threshold,
                                                 "quotient.overflow"),
                           "divide.error");
    }

    llvm::Function *Function = Builder.GetInsertBlock()->getParent();
    llvm::BasicBlock *Trap =
        llvm::BasicBlock::Create(*Ctx, "div.overflow", Function);
    llvm::BasicBlock *Continue =
        llvm::BasicBlock::Create(*Ctx, "div.precondition.ok", Function);
    Builder.CreateCondBr(Overflow, Trap, Continue);
    Builder.SetInsertPoint(Trap);
    llvm::Function *TrapFn =
        llvm::Intrinsic::getOrInsertDeclaration(Mod, llvm::Intrinsic::trap);
    Builder.CreateCall(TrapFn, {});
    Builder.CreateUnreachable();
    Builder.SetInsertPoint(Continue);
    return true;
  }
  if (IC == Intrinsic::X86Invalidate) {
    if (!intrinsicX86InvalidateShapeIsValid(IC, x86InvalidateMedShape(Op)))
      llvm::report_fatal_error(
          "x86 invalidation intrinsic has an invalid operand contract");
    // INVPCID, as the source's _invpcid(type, descriptor) compiles to.
    llvm::Value *Descriptor = getVar(Op.Inputs[1], Builder);
    llvm::Value *Type = getVar(Op.Inputs[3], Builder);
    auto *FnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(*Ctx), {Type->getType(), Descriptor->getType()},
        false);
    auto *IA = llvm::InlineAsm::get(FnTy, "invpcid ($1), $0", "r,r,~{memory}",
                                    /*hasSideEffects=*/true);
    Builder.CreateCall(IA, {Type, Descriptor});
    return true;
  }
  if (IC == Intrinsic::AMXLoadConfig || IC == Intrinsic::AMXStoreConfig ||
      IC == Intrinsic::AMXTileLoad || IC == Intrinsic::AMXTileStore ||
      IC == Intrinsic::AMXTileZero || IC == Intrinsic::AMXClearStartRow ||
      IC == Intrinsic::AMXTileCompute || IC == Intrinsic::AMXTileRow)
    llvm::report_fatal_error(
        "AMX architectural-state intrinsic requires exact concrete "
        "emulation");
  if (IC == Intrinsic::F16CConvert)
    llvm::report_fatal_error(
        "F16C conversion intrinsic requires exact concrete emulation");
  if (IC == Intrinsic::X86ApproxFloat)
    llvm::report_fatal_error(
        "x86 approximation intrinsic requires exact concrete emulation");
  if (IC == Intrinsic::X86FPClass)
    llvm::report_fatal_error(
        "x86 floating-point classification intrinsic requires exact "
        "concrete emulation");
  if (IC == Intrinsic::X86FPArith)
    llvm::report_fatal_error(
        "x86 floating-point arithmetic intrinsic requires exact concrete "
        "emulation");
  if (IC == Intrinsic::X86FPConvert)
    llvm::report_fatal_error(
        "x86 floating-point conversion intrinsic requires exact concrete "
        "emulation");
  if (IC == Intrinsic::X86FPRoundTransform)
    llvm::report_fatal_error(
        "x86 floating-point round transformation intrinsic requires exact "
        "concrete emulation");
  if (IC == Intrinsic::X86FPExtract)
    llvm::report_fatal_error(
        "x86 floating-point extraction intrinsic requires exact concrete "
        "emulation");
  if (IC == Intrinsic::X86FPRange)
    llvm::report_fatal_error(
        "x86 floating-point range intrinsic requires exact concrete "
        "emulation");
  if (IC == Intrinsic::X86FPFixup)
    llvm::report_fatal_error(
        "x86 floating-point fixup intrinsic requires exact concrete "
        "emulation");
  if (IC == Intrinsic::X86FPScale)
    llvm::report_fatal_error(
        "x86 floating-point scale intrinsic requires exact concrete "
        "emulation");
  if (IC == Intrinsic::X86FPCompare)
    llvm::report_fatal_error(
        "x86 floating-point comparison intrinsic requires exact concrete "
        "emulation");
  if (IC == Intrinsic::RequireAligned) {
    if ((Op.NumInputs != 3 && Op.NumInputs != 4) || Op.Output.Size != 0 ||
        Op.Inputs[1].Size != 8 || Op.Inputs[2].Size != 8 ||
        !Op.Inputs[2].isConst() ||
        (Op.NumInputs == 4 && Op.Inputs[3].Size != 1))
      llvm::report_fatal_error("invalid alignment precondition intrinsic");
    if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      llvm::report_fatal_error(
          "segmented alignment precondition requires an architectural "
          "FS/GS base");
    const uint64_t Alignment = Op.Inputs[2].ConstVal;
    if (Alignment == 0 || (Alignment & (Alignment - 1)) != 0)
      llvm::report_fatal_error("alignment precondition is not a power of two");
    llvm::Value *Value = getVar(Op.Inputs[1], Builder);
    if (!Value->getType()->isIntegerTy())
      llvm::report_fatal_error("alignment precondition value is not integer");
    llvm::Value *Misaligned = Builder.CreateICmpNE(
        Builder.CreateAnd(
            Value, llvm::ConstantInt::get(Value->getType(), Alignment - 1)),
        llvm::ConstantInt::get(Value->getType(), 0), "misaligned");
    if (Op.NumInputs == 4) {
      llvm::Value *Guard = getVar(Op.Inputs[3], Builder);
      if (!Guard->getType()->isIntegerTy(8))
        llvm::report_fatal_error(
            "alignment precondition guard is not an i8 predicate");
      llvm::Value *Active = Builder.CreateICmpNE(
          Guard, llvm::ConstantInt::get(Guard->getType(), 0),
          "alignment.active");
      Misaligned =
          Builder.CreateAnd(Active, Misaligned, "active.and.misaligned");
    }
    llvm::Function *Function = Builder.GetInsertBlock()->getParent();
    llvm::BasicBlock *Trap =
        llvm::BasicBlock::Create(*Ctx, "alignment.trap", Function);
    llvm::BasicBlock *Continue =
        llvm::BasicBlock::Create(*Ctx, "alignment.ok", Function);
    Builder.CreateCondBr(Misaligned, Trap, Continue);
    Builder.SetInsertPoint(Trap);
    llvm::Function *TrapFn =
        llvm::Intrinsic::getOrInsertDeclaration(Mod, llvm::Intrinsic::trap);
    Builder.CreateCall(TrapFn, {});
    Builder.CreateUnreachable();
    Builder.SetInsertPoint(Continue);
    return true;
  }
  if (emitX86DebugTrap(Op, IC, Builder))
    return true;
  if (emitX86Fence(Op, IC, Builder))
    return true;
  if (emitX86CacheOp(Op, IC, Builder))
    return true;
  if (emitX86SystemAsm(Op, IC, Builder))
    return true;
  if (const char *Reason = x86UnsupportedIntrinsicReason(IC))
    llvm::report_fatal_error(Reason);
  if (emitX86Privileged(Op, IC, Builder))
    return true;
  return false;
}

} // namespace neverd
