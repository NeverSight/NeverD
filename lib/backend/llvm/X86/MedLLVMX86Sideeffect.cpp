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

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
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
    // `int imm8` (e.g. int 0x29 = __fastfail).  The interrupt vector is an
    // immediate that MUST appear in the asm text, so bake it in via the `$0`
    // template with an immediate constraint rather than a register operand
    // (a bare "int" would be rejected by the assembler: too few operands).
    auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
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

  if (IC == I::Clflush || IC == I::Clflushopt || IC == I::Clwb) {
    const char *Mn = (IC == I::Clflush)      ? "clflush"
                     : (IC == I::Clflushopt) ? "clflushopt"
                                             : "clwb";
    emitX86MemPtrAsm(Mn, Op, Builder);
    return true;
  }

  if (IC == I::Prefetch) {
    if (Op.NumInputs >= 2) {
      auto *Addr = getVar(Op.Inputs[1], Builder);
      auto *Ptr = Builder.CreateIntToPtr(Addr, llvm::PointerType::get(*Ctx, 0));
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::prefetch, {llvm::PointerType::get(*Ctx, 0)});
      Builder.CreateCall(
          Fn, {Ptr, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*Ctx), 0),
               llvm::ConstantInt::get(llvm::Type::getInt32Ty(*Ctx), 3),
               llvm::ConstantInt::get(llvm::Type::getInt32Ty(*Ctx), 1)});
    }
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
  if (Op.NumInputs > 1) {
    auto *PtrVal = getVar(Op.Inputs[1], Builder);
    auto *PtrTy = llvm::PointerType::get(*Ctx, 0);
    auto *AsmFnTy = llvm::FunctionType::get(VoidTy, {PtrTy}, false);
    auto *IA = llvm::InlineAsm::get(AsmFnTy, std::string(Mn) + " ($0)",
                                    "r,~{memory}", true);
    auto *P = Builder.CreateIntToPtr(PtrVal, PtrTy, "sys_ptr");
    Builder.CreateCall(IA, {P});
  } else {
    auto *AsmFnTy = llvm::FunctionType::get(VoidTy, {}, false);
    auto *IA = llvm::InlineAsm::get(AsmFnTy, Mn, "~{memory}", true);
    Builder.CreateCall(IA, {});
  }
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
  using I = Intrinsic;
  switch (IC) {
  case I::Cli:
  case I::Sti:
  case I::Wrmsr:
  case I::Wrpkru:
  case I::Swapgs:
  case I::Wbinvd:
  case I::Fxsave:
  case I::Fxrstor:
  case I::Outsb:
  case I::Outsw:
  case I::Outsd:
  case I::Vmcall:
  case I::Vmmcall:
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
