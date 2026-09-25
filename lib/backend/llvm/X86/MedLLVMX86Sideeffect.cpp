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

#define DEBUG_TYPE "neverd-med-llvm-x86-sideeffect"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicsX86.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// Debug traps (INT3, INT1, INT N, INTO, UD2, UD0, UD1)
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitX86DebugTrap(const MedOp &Op, Intrinsic IC,
                                      llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  switch (IC) {
  case I::Int3:
  case I::Int1: {
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::debugtrap);
    Builder.CreateCall(Fn, {});
    return true;
  }
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
    Builder.CreateCall(Fn, {});
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
    Builder.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
    return true;
  case I::Lfence:
    Builder.CreateFence(llvm::AtomicOrdering::Acquire);
    return true;
  case I::Sfence:
    Builder.CreateFence(llvm::AtomicOrdering::Release);
    return true;
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

  // Descriptor-table loads/stores + INVLPG: single memory-address operand.
  const char *MemMn = nullptr;
  switch (IC) {
  case I::Lgdt:
    MemMn = "lgdt";
    break;
  case I::Lidt:
    MemMn = "lidt";
    break;
  case I::Sgdt:
    MemMn = "sgdt";
    break;
  case I::Sidt:
    MemMn = "sidt";
    break;
  case I::Invlpg:
    MemMn = "invlpg";
    break;
  default:
    break;
  }
  if (MemMn) {
    emitX86MemPtrAsm(MemMn, Op, Builder);
    return true;
  }

  // XABORT imm8: immediate baked into the asm text (like INT N).
  if (IC == I::Xabort) {
    auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
    uint64_t Imm = (Op.NumInputs > 1 && Op.Inputs[1].isConst())
                       ? Op.Inputs[1].ConstVal
                       : 0;
    auto *I8 = llvm::Type::getInt8Ty(*Ctx);
    auto *AsmFnTy = llvm::FunctionType::get(VoidTy, {I8}, false);
    auto *IA = llvm::InlineAsm::get(AsmFnTy, "xabort $0", "i,~{memory}",
                                    /*hasSideEffects=*/true);
    Builder.CreateCall(IA, {llvm::ConstantInt::get(I8, Imm & 0xFF)});
    return true;
  }

  // OUT port, acc.
  if (IC == I::Out) {
    emitX86PortOut(Op, Builder);
    return true;
  }

  // r/m16 system-register loads (read): LLDT/LTR/LMSW.
  const char *RmRead = nullptr;
  switch (IC) {
  case I::Lldt:
    RmRead = "lldt";
    break;
  case I::Ltr:
    RmRead = "ltr";
    break;
  case I::Lmsw:
    RmRead = "lmsw";
    break;
  default:
    break;
  }
  if (RmRead) {
    if (Op.NumInputs > 1 && Op.Inputs[1].Size == 8) {
      emitX86MemPtrAsm(RmRead, Op, Builder); // memory form: `mnemonic (addr)`
    } else if (Op.NumInputs > 1) {
      auto *V = getVar(Op.Inputs[1], Builder); // register form: `mnemonic reg`
      auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
      auto *FnTy = llvm::FunctionType::get(VoidTy, {V->getType()}, false);
      auto *IA = llvm::InlineAsm::get(FnTy, std::string(RmRead) + " $0",
                                      "r,~{memory}", /*hasSideEffects=*/true);
      Builder.CreateCall(IA, {V});
    } else {
      emitX86MemPtrAsm(RmRead, Op, Builder); // no operand captured: bare
    }
    return true;
  }

  // r/m16 system-register stores: SLDT/STR/SMSW.  Memory form is a side-effect
  // (store through pointer); the register-destination form is value-producing
  // and handled in the value emitter (return false here).
  if (IC == I::Sldt || IC == I::Str || IC == I::Smsw) {
    if (Op.NumInputs > 1) {
      const char *Mn = (IC == I::Sldt)  ? "sldt"
                       : (IC == I::Str) ? "str"
                                        : "smsw";
      emitX86MemPtrAsm(Mn, Op, Builder);
      return true;
    }
    return false;
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

//===----------------------------------------------------------------------===//
// SLDT/STR/SMSW with a register destination (value-producing)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitX86SysRegStore(const MedOp &Op, Intrinsic IC,
                                                llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  const char *Mn = (IC == I::Sldt) ? "sldt" : (IC == I::Str) ? "str" : "smsw";
  unsigned Bytes = Op.Output.Size ? Op.Output.Size : 2;
  auto *Ty = sizeToType(Bytes);
  auto *FnTy = llvm::FunctionType::get(Ty, {}, false);
  auto *IA = llvm::InlineAsm::get(FnTy, std::string(Mn) + " $0", "=r,~{memory}",
                                  /*hasSideEffects=*/true);
  return Builder.CreateCall(IA, {});
}

//===----------------------------------------------------------------------===//
// Port I/O (IN / OUT)
//===----------------------------------------------------------------------===//

static const char *portIoSuffix(unsigned Bytes) {
  return Bytes == 1 ? "b" : Bytes == 2 ? "w" : "l";
}

llvm::Value *MedLLVMEmitter::emitX86PortIn(const MedOp &Op,
                                           llvm::IRBuilder<> &Builder) {
  // INTRINSIC inputs: [code, port].  port const => imm8, else DX register.
  unsigned Bytes = Op.Output.Size ? Op.Output.Size : 4;
  auto *Ty = sizeToType(Bytes);
  bool ImmPort = Op.NumInputs > 1 && Op.Inputs[1].isConst();
  // AT&T: `in <port>, <acc>` — src(port) first, dest(acc=AX) second.
  std::string Asm = std::string("in") + portIoSuffix(Bytes) + " $1, $0";
  std::string Cons =
      std::string("={ax},") + (ImmPort ? "N" : "{dx}") + ",~{memory}";
  llvm::Value *Port;
  if (ImmPort)
    Port = llvm::ConstantInt::get(llvm::Type::getInt8Ty(*Ctx),
                                  Op.Inputs[1].ConstVal & 0xFF);
  else if (Op.NumInputs > 1)
    Port = getVar(Op.Inputs[1], Builder);
  else
    Port = llvm::ConstantInt::get(llvm::Type::getInt8Ty(*Ctx), 0);
  auto *AsmFnTy = llvm::FunctionType::get(Ty, {Port->getType()}, false);
  auto *IA = llvm::InlineAsm::get(AsmFnTy, Asm, Cons, /*hasSideEffects=*/true);
  return Builder.CreateCall(IA, {Port});
}

void MedLLVMEmitter::emitX86PortOut(const MedOp &Op,
                                    llvm::IRBuilder<> &Builder) {
  // INTRINSIC inputs: [code, port, acc].  port const => imm8, else DX register.
  if (Op.NumInputs < 3)
    return;
  auto *Acc = getVar(Op.Inputs[2], Builder);
  unsigned Bytes = Acc->getType()->isIntegerTy()
                       ? Acc->getType()->getIntegerBitWidth() / 8
                       : 4;
  bool ImmPort = Op.Inputs[1].isConst();
  // AT&T: `out <acc>, <port>` — src(acc=AX) first, dest(port) second.
  std::string Asm = std::string("out") + portIoSuffix(Bytes) + " $0, $1";
  std::string Cons =
      std::string("{ax},") + (ImmPort ? "N" : "{dx}") + ",~{memory}";
  llvm::Value *Port = ImmPort
                          ? llvm::ConstantInt::get(llvm::Type::getInt8Ty(*Ctx),
                                                   Op.Inputs[1].ConstVal & 0xFF)
                          : getVar(Op.Inputs[1], Builder);
  auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
  auto *AsmFnTy =
      llvm::FunctionType::get(VoidTy, {Acc->getType(), Port->getType()}, false);
  auto *IA = llvm::InlineAsm::get(AsmFnTy, Asm, Cons, /*hasSideEffects=*/true);
  Builder.CreateCall(IA, {Acc, Port});
}

//===----------------------------------------------------------------------===//
// MPX / transactional memory (treated as NOP)
//===----------------------------------------------------------------------===//

static bool isX86MpxOrTransactional(Intrinsic IC) {
  using I = Intrinsic;
  switch (IC) {
  case I::BndMk:
  case I::BndMov:
  case I::BndCl:
  case I::BndCu:
  case I::BndCn:
  case I::BndLdx:
  case I::BndStx:
  case I::Xacquire:
  case I::Xrelease:
  case I::ShaGeneric:
    return true;
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Privileged / system instructions (CLI, STI, INVLPG, WRMSR, etc.)
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitX86Privileged(const MedOp &Op, Intrinsic IC,
                                       llvm::IRBuilder<> &Builder) {
  const bool SystemRegisterWrite =
      IC == Intrinsic::WriteCr0 || IC == Intrinsic::WriteCr2 ||
      IC == Intrinsic::WriteCr3 || IC == Intrinsic::WriteCr4 ||
      IC == Intrinsic::WriteCr8 || IC == Intrinsic::WriteDr;
  if (auto Reg = !SystemRegisterWrite
                     ? std::nullopt
                     : x86SystemRegisterName(IC, Op.NumInputs > 1 &&
                                                         Op.Inputs[1].isConst()
                                                     ? Op.Inputs[1].ConstVal
                                                     : UINT64_MAX)) {
    // WriteDr carries the register number before the value.
    const uint16_t ValueInput = IC == Intrinsic::WriteDr ? 2 : 1;
    if (Op.NumInputs <= ValueInput)
      llvm::report_fatal_error("x86 system register write has no value");
    llvm::Value *Value = getVar(Op.Inputs[ValueInput], Builder);
    auto *FnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*Ctx),
                                         {Value->getType()}, false);
    auto *IA = llvm::InlineAsm::get(FnTy, "mov $0, " + *Reg, "r,~{memory}",
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
          FnTy, Mnemonic, "={di},={cx},0,1,{dx},r,~{memory},~{dirflag},~{cc}",
          true);
      Call = Builder.CreateCall(IA, {RDI, RCX, RDX, DF}, "rep_ins");
    } else {
      auto *FnTy =
          llvm::FunctionType::get(RetTy, {AddrTy, AddrTy, I16Ty}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mnemonic, "={di},={cx},0,1,{dx},~{memory},~{dirflag},~{cc}",
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
          FnTy, Mnemonic, "={si},={cx},0,1,{dx},r,~{memory},~{dirflag},~{cc}",
          true);
      Call = Builder.CreateCall(IA, {RSI, RCX, RDX, DF}, "rep_outs");
    } else {
      auto *FnTy =
          llvm::FunctionType::get(RetTy, {AddrTy, AddrTy, I16Ty}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mnemonic, "={si},={cx},0,1,{dx},~{memory},~{dirflag},~{cc}",
          true);
      Call = Builder.CreateCall(IA, {RSI, RCX, RDX}, "rep_outs");
    }
    (void)Call;
    return true;
  }

  // The x64 `syscall` with its service number returns a status in RAX; the
  // value emitter lowers it.
  if (IC == I::Syscall && Op.NumInputs == 2 && Op.Output.Size > 0)
    return false;

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
        "r,~{memory},~{cc},~{dirflag},~{flags}", /*hasSideEffects=*/true);
    Builder.CreateCall(IA, {Flags});
    return true;
  }

  // WRMSR writes EDX:EAX to the MSR selected by ECX.
  if (IC == I::Wrmsr) {
    if (Op.NumInputs != 3)
      llvm::report_fatal_error("x86 WRMSR has an invalid operand shape");
    auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
    auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
    llvm::Value *Selector =
        Builder.CreateZExtOrTrunc(getVar(Op.Inputs[1], Builder), I32Ty);
    llvm::Value *Value =
        Builder.CreateZExtOrTrunc(getVar(Op.Inputs[2], Builder), I64Ty);
    llvm::Value *Lo = Builder.CreateTrunc(Value, I32Ty, "msr_lo");
    llvm::Value *Hi =
        Builder.CreateTrunc(Builder.CreateLShr(Value, 32), I32Ty, "msr_hi");
    auto *FnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*Ctx),
                                         {I32Ty, I32Ty, I32Ty}, false);
    auto *IA =
        llvm::InlineAsm::get(FnTy, "wrmsr", "{ecx},{eax},{edx},~{memory}",
                             /*hasSideEffects=*/true);
    Builder.CreateCall(IA, {Selector, Lo, Hi});
    return true;
  }

  switch (IC) {
  case I::Cli:
  case I::Sti:
  case I::Wrpkru:
  case I::Swapgs:
  case I::Wbinvd:
  case I::Vmcall:
  case I::Vmmcall:
  case I::X87Fninit:
  case I::X87Fnclex:
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
  if (IC == Intrinsic::X86MsrAccess) {
    if (!intrinsicX86MsrAccessShapeIsValid(IC, x86MsrAccessMedShape(Op)))
      llvm::report_fatal_error(
          "x86 MSR access intrinsic has an invalid operand/output contract");
    llvm::report_fatal_error(
        "x86 MSR access requires an authenticated architectural execution "
        "environment");
  }
  if (IC == Intrinsic::CetWrss || IC == Intrinsic::CetWruss ||
      IC == Intrinsic::Enqcmd || IC == Intrinsic::Enqcmds)
    llvm::report_fatal_error(
        "x86 system-memory effect requires an authenticated architectural "
        "execution environment");
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
  if (isX86MpxOrTransactional(IC))
    return true;
  if (emitX86Privileged(Op, IC, Builder))
    return true;
  return false;
}

} // namespace neverd
