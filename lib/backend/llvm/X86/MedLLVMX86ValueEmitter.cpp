//===- MedLLVMX86ValueEmitter.cpp - x86 value-producing intrinsics -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86-specific value-producing intrinsic emission: CPUID, XGETBV,
/// RDTSC/RDTSCP, STMXCSR/LDMXCSR, and REP string operations (MOVSB, STOSB,
/// LODSB, etc.).
///
/// Side-effect-only intrinsics live in MedLLVMX86Sideeffect.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/backend/LLVMValueProvenance.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86/LLVMX86DivRemAsm.h"
#include "neverd/backend/llvm/X86/LLVMX86FPStateAsm.h"
#include "neverd/backend/llvm/X86/LLVMX86ShadowStackAsm.h"
#include "neverd/backend/llvm/X86/LLVMX86StackEffects.h"
#include "neverd/backend/llvm/X86/LLVMX87ConstantAsm.h"
#include "neverd/backend/llvm/X86/LLVMX87ContextAsm.h"
#include "neverd/backend/llvm/X86/LLVMX87GuardedValueAsm.h"
#include "neverd/backend/llvm/X86/LLVMX87ValueAsm.h"
#include "neverd/backend/llvm/X86/X86SystemRegisterAsm.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#define DEBUG_TYPE "neverd-med-llvm-x86-value"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/X86/X64Syscall.h"
#include "neverd/ir/intrinsics/X86/X86SegmentRegisters.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/TargetParser/Triple.h"

#include <cassert>
#include <iterator>
#include <string>

namespace neverd {

//===----------------------------------------------------------------------===//
// RDTSC / RDTSCP
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitRdtscValue(const MedOp &Op, Intrinsic IC,
                                            llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;

  if ((IC != I::Rdtsc && IC != I::Rdtscp) || Op.Output.Size == 0)
    return nullptr;

  auto *OutTy = sizeToType(Op.Output.Size);
  llvm::Value *R = nullptr;
  llvm::Value *Aux = nullptr;
  if (IC == I::Rdtscp) {
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::x86_rdtscp);
    auto *Pair = Builder.CreateCall(Fn, {}, "rdtscp");
    R = Builder.CreateExtractValue(Pair, {0}, "rdtscp_tsc");
    Aux = Builder.CreateExtractValue(Pair, {1}, "rdtscp_aux");
  } else {
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::readcyclecounter);
    R = Builder.CreateCall(Fn, {}, "rdtsc");
  }
  auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
  auto *Lo = Builder.CreateTrunc(R, I32Ty, "tsc_lo");
  auto *Hi = Builder.CreateTrunc(Builder.CreateLShr(R, 32), I32Ty, "tsc_hi");
  PendingIntrinsicOutputs[0] = Lo;
  PendingIntrinsicOutputs[1] = Hi;
  if (Aux)
    PendingIntrinsicOutputs[2] = Aux;
  PendingIntrinsicCount = Aux ? 3 : 2;

  llvm::Value *Result = R;
  if (Result->getType() != OutTy) {
    if (OutTy->isIntegerTy() &&
        Result->getType()->getIntegerBitWidth() > OutTy->getIntegerBitWidth())
      Result = Builder.CreateTrunc(Result, OutTy);
    else if (OutTy->isIntegerTy())
      Result = Builder.CreateZExt(Result, OutTy);
  }
  return Result;
}

//===----------------------------------------------------------------------===//
// CPUID
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitCpuidValue(const MedOp &Op,
                                            llvm::IRBuilder<> &Builder) {
  if (Op.Output.Size == 0)
    return nullptr;

  auto *OutTy = sizeToType(Op.Output.Size);
  auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
  llvm::Value *Leaf = llvm::ConstantInt::get(I32Ty, 0);
  if (Op.NumInputs > 1) {
    Leaf = getVar(Op.Inputs[1], Builder);
    if (Leaf->getType() != I32Ty)
      Leaf = Builder.CreateTruncOrBitCast(Leaf, I32Ty);
  }
  llvm::Value *Subleaf = llvm::ConstantInt::get(I32Ty, 0);
  if (Op.NumInputs > 2) {
    Subleaf = getVar(Op.Inputs[2], Builder);
    if (Subleaf->getType() != I32Ty)
      Subleaf = Builder.CreateTruncOrBitCast(Subleaf, I32Ty);
  }
  auto *StructTy = llvm::StructType::get(*Ctx, {I32Ty, I32Ty, I32Ty, I32Ty});
  auto *AsmFnTy = llvm::FunctionType::get(StructTy, {I32Ty, I32Ty}, false);
  auto *IA = llvm::InlineAsm::get(
      AsmFnTy, "cpuid", "={eax},={ebx},={ecx},={edx},{eax},{ecx},~{memory}",
      true);
  auto *Res = Builder.CreateCall(IA, {Leaf, Subleaf}, "cpuid");
  auto *EAX = Builder.CreateExtractValue(Res, {0}, "cpuid_eax");
  auto *EBX = Builder.CreateExtractValue(Res, {1}, "cpuid_ebx");
  auto *ECX = Builder.CreateExtractValue(Res, {2}, "cpuid_ecx");
  auto *EDX = Builder.CreateExtractValue(Res, {3}, "cpuid_edx");
  PendingIntrinsicOutputs[0] = EAX;
  PendingIntrinsicOutputs[1] = EBX;
  PendingIntrinsicOutputs[2] = ECX;
  PendingIntrinsicOutputs[3] = EDX;
  PendingIntrinsicCount = 4;

  if (Op.Output.Size <= 4) {
    if (OutTy == I32Ty)
      return EAX;
    return Builder.CreateZExtOrTrunc(EAX, OutTy);
  }
  auto *I128Ty = llvm::IntegerType::get(*Ctx, 128);
  auto *V = Builder.CreateZExt(EAX, I128Ty);
  V = Builder.CreateOr(V,
                       Builder.CreateShl(Builder.CreateZExt(EBX, I128Ty), 32));
  V = Builder.CreateOr(V,
                       Builder.CreateShl(Builder.CreateZExt(ECX, I128Ty), 64));
  V = Builder.CreateOr(V,
                       Builder.CreateShl(Builder.CreateZExt(EDX, I128Ty), 96));
  if (OutTy != I128Ty)
    V = Builder.CreateTruncOrBitCast(V, OutTy);
  return V;
}

//===----------------------------------------------------------------------===//
// XGETBV
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitXgetbvValue(const MedOp &Op,
                                             llvm::IRBuilder<> &Builder) {
  if (Op.Output.Size == 0)
    return nullptr;

  auto *OutTy = sizeToType(Op.Output.Size);
  auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
  llvm::Value *EcxIn = llvm::ConstantInt::get(I32Ty, 0);
  if (Op.NumInputs > 1) {
    EcxIn = getVar(Op.Inputs[1], Builder);
    if (EcxIn->getType() != I32Ty)
      EcxIn = Builder.CreateTruncOrBitCast(EcxIn, I32Ty);
  }
  auto *StructTy = llvm::StructType::get(*Ctx, {I32Ty, I32Ty});
  auto *AsmFnTy = llvm::FunctionType::get(StructTy, {I32Ty}, false);
  auto *IA = llvm::InlineAsm::get(AsmFnTy, "xgetbv",
                                  "={eax},={edx},{ecx},~{memory}", true);
  auto *Res = Builder.CreateCall(IA, {EcxIn}, "xgetbv");
  auto *Lo = Builder.CreateExtractValue(Res, {0}, "xgetbv_lo");
  auto *Hi = Builder.CreateExtractValue(Res, {1}, "xgetbv_hi");
  PendingIntrinsicOutputs[0] = Lo;
  PendingIntrinsicOutputs[1] = Hi;
  PendingIntrinsicCount = 2;

  if (Op.Output.Size <= 4)
    return Lo;
  auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
  auto *V = Builder.CreateZExt(Lo, I64Ty);
  V = Builder.CreateOr(V, Builder.CreateShl(Builder.CreateZExt(Hi, I64Ty), 32));
  if (OutTy != I64Ty)
    V = Builder.CreateZExtOrTrunc(V, OutTy);
  return V;
}

//===----------------------------------------------------------------------===//
// REP string operations (MOVSB/W/D/Q, STOSB/W/D/Q)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitRepString(const MedOp &Op, Intrinsic IC,
                                           llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
  const unsigned AddrRegBits = getTargetRegInfo(TargetArch).PointerSize * 8;
  assert((AddrRegBits == 32 || AddrRegBits == 64) &&
         "x86 REP emission requires a 32- or 64-bit target");
  auto *AddrRegTy = llvm::Type::getIntNTy(*Ctx, AddrRegBits);

  // Element size + AT&T mnemonic suffix (b/w/l/q) per intrinsic variant.
  unsigned ElemSz = 0;
  char Suffix = 'b';
  switch (IC) {
  case I::Movsb:
  case I::Stosb:
  case I::Lodsb:
  case I::Cmpsb:
  case I::Scasb:
    ElemSz = 1;
    Suffix = 'b';
    break;
  case I::Movsw:
  case I::Stosw:
  case I::Lodsw:
  case I::Cmpsw:
  case I::Scasw:
    ElemSz = 2;
    Suffix = 'w';
    break;
  case I::Movsd:
  case I::Stosd:
  case I::Lodsd:
  case I::Cmpsd_str:
  case I::Scasd:
    ElemSz = 4;
    Suffix = 'l';
    break;
  case I::Movsq:
  case I::Stosq:
  case I::Lodsq:
  case I::Cmpsq:
  case I::Scasq:
    ElemSz = 8;
    Suffix = 'q';
    break;
  default:
    return nullptr;
  }
  bool IsStos =
      (IC == I::Stosb || IC == I::Stosw || IC == I::Stosd || IC == I::Stosq);
  bool IsMovs =
      (IC == I::Movsb || IC == I::Movsw || IC == I::Movsd || IC == I::Movsq);
  bool IsLods =
      (IC == I::Lodsb || IC == I::Lodsw || IC == I::Lodsd || IC == I::Lodsq);
  bool IsCmps = (IC == I::Cmpsb || IC == I::Cmpsw || IC == I::Cmpsd_str ||
                 IC == I::Cmpsq);
  bool IsScas =
      (IC == I::Scasb || IC == I::Scasw || IC == I::Scasd || IC == I::Scasq);

  auto withSourceSegment = [&](std::string Mnemonic) -> std::string {
    if (!IsMovs && !IsLods && !IsCmps)
      return Mnemonic;
    switch (Op.MemoryAddressSpace) {
    case NdMemoryAddressSpace::Default:
      return Mnemonic;
    case NdMemoryAddressSpace::X86FS:
      return "fs " + Mnemonic;
    case NdMemoryAddressSpace::X86GS:
      return "gs " + Mnemonic;
    }
    llvm_unreachable("unknown x86 string source address space");
  };

  const unsigned NativeAddressBytes = AddrRegBits / 8;
  const unsigned StringAddressBytes =
      Op.NumInputs > 1 ? Op.Inputs[1].Size : NativeAddressBytes;
  auto withAddressSize = [&](std::string Mnemonic) -> std::string {
    if (StringAddressBytes == NativeAddressBytes)
      return Mnemonic;
    if (StringAddressBytes == 4 && NativeAddressBytes == 8)
      return "addr32 " + Mnemonic;
    if (StringAddressBytes == 2 && NativeAddressBytes == 4)
      return "addr16 " + Mnemonic;
    llvm::report_fatal_error("unsupported x86 string address size");
  };

  auto Coerce = [&](llvm::Value *V, llvm::Type *T) -> llvm::Value * {
    return (V->getType() == T) ? V : Builder.CreateZExtOrTrunc(V, T);
  };
  // The rep-string INTRINSIC carries a throwaway primary output (the real
  // register/memory effects are emitted as the inline asm + MedIR updates), so
  // hand back a benign 0 rather than null — returning null would make the
  // dispatcher fall through to the "unhandled intrinsic" warning + default 0.
  auto Handled = [&]() -> llvm::Value * {
    return (Op.Output.Size > 0)
               ? llvm::ConstantInt::get(sizeToType(Op.Output.Size), 0)
               : nullptr;
  };

  // Direction flag: the lifter threads the modelled DF as the final input
  // (index 4 for STOS/MOVS/LODS/CMPS/SCAS; absent on legacy stubs => forward).
  // A known constant bakes the direction in with no runtime branch; a dynamic
  // DF is tested before the rep.  Either way a trailing `cld` restores the
  // SysV forward invariant.  The hardware `rep` honours the live DF, so this
  // is the recompiled counterpart of `std; rep ...` over overlapping/backward
  // buffers.
  enum DfKind { DfFwd, DfBwd, DfDyn };
  DfKind DfK = DfFwd;
  if (Op.NumInputs > 4) {
    const auto &DfV = Op.Inputs[4];
    DfK = DfV.isConst() ? (DfV.ConstVal != 0 ? DfBwd : DfFwd) : DfDyn;
  }
  // Operand index of the dynamic DF GPR is supplied per template below because
  // each string family has a different number of tied in/out registers.
  auto dirWrap = [&](const std::string &Rep,
                     unsigned DfOperand) -> std::string {
    if (DfK == DfBwd)
      return "std\n\t" + Rep + "\n\tcld";
    if (DfK == DfDyn)
      return "test $" + std::to_string(DfOperand) + ",$" +
             std::to_string(DfOperand) + "\n\tje 1f\n\tstd\n\t1:\n\t" + Rep +
             "\n\tcld";
    return Rep;
  };

  // STOS: inputs [RDI, RCX, AL/AX/EAX/RAX].  RDI/RCX are written by the
  // hardware (captured as tied in-out outputs so LLVM models the clobber); the
  // values are discarded because the lifter already updates the register slots
  // in MedIR.
  if (IsStos && Op.NumInputs >= 5) {
    auto *RDI = Coerce(getVar(Op.Inputs[1], Builder), AddrRegTy);
    auto *RCX = Coerce(getVar(Op.Inputs[2], Builder), AddrRegTy);
    auto *ValTy = llvm::Type::getIntNTy(*Ctx, ElemSz * 8);
    auto *Val = Coerce(getVar(Op.Inputs[3], Builder), ValTy);
    std::string Mn =
        dirWrap(withAddressSize(std::string("rep stos") + Suffix), 5);
    auto *RetTy = llvm::StructType::get(*Ctx, {AddrRegTy, AddrRegTy});
    if (DfK == DfDyn) {
      auto *Df = Coerce(getVar(Op.Inputs[4], Builder), AddrRegTy);
      auto *FnTy = llvm::FunctionType::get(
          RetTy, {AddrRegTy, AddrRegTy, ValTy, AddrRegTy}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mn, "={di},={cx},0,1,{ax},r,~{memory},~{dirflag},~{flags}",
          true);
      Builder.CreateCall(IA, {RDI, RCX, Val, Df});
    } else {
      auto *FnTy =
          llvm::FunctionType::get(RetTy, {AddrRegTy, AddrRegTy, ValTy}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mn, "={di},={cx},0,1,{ax},~{memory},~{dirflag},~{flags}", true);
      Builder.CreateCall(IA, {RDI, RCX, Val});
    }
    return Handled();
  }

  // CMPS / SCAS: unlike MOVS/STOS these stop on a data-dependent ZF transition,
  // so the trip count is only known at run time.  The hardware's leftover RCX
  // is the intrinsic's result and the lifter derives the advanced pointers from
  // it (see liftRepCmpScas).  LAHF + SETO capture all six arithmetic flags in
  // AX without a post-loop memory probe; this also preserves the architectural
  // no-operand-access behavior when the entry count is zero.  REPE (`repz`)
  // repeats while the elements match, REPNE (`repnz`) while they differ; the
  // lifter passes which one as the trailing constant input.
  if ((IsCmps || IsScas) && Op.NumInputs >= 6) {
    const bool RepNE = Op.NumInputs > 5 && Op.Inputs[5].isConst() &&
                       Op.Inputs[5].ConstVal != 0;
    const std::string RepPfx = RepNE ? "repnz " : "repz ";
    llvm::Value *LeftCount = nullptr;
    llvm::Value *PackedFlags = nullptr;
    if (IsScas) {
      // Inputs [RDI, RCX, AL/AX/EAX/RAX]: RDI/RCX/RAX are tied in-out.  SCAS
      // consumes the incoming accumulator before LAHF/SETO reuse AX for the
      // returned flag snapshot.
      auto *RDI = Coerce(getVar(Op.Inputs[1], Builder), AddrRegTy);
      auto *RCX = Coerce(getVar(Op.Inputs[2], Builder), AddrRegTy);
      auto *Val = Coerce(getVar(Op.Inputs[3], Builder), AddrRegTy);
      std::string Mn = dirWrap(withAddressSize(RepPfx + "scas" + Suffix), 6) +
                       "\n\tlahf\n\tseto %al";
      auto *RetTy =
          llvm::StructType::get(*Ctx, {AddrRegTy, AddrRegTy, AddrRegTy});
      llvm::CallInst *Call = nullptr;
      if (DfK == DfDyn) {
        auto *Df = Coerce(getVar(Op.Inputs[4], Builder), AddrRegTy);
        auto *FnTy = llvm::FunctionType::get(
            RetTy, {AddrRegTy, AddrRegTy, AddrRegTy, AddrRegTy}, false);
        auto *IA = llvm::InlineAsm::get(
            FnTy, Mn, "={di},={cx},={ax},0,1,2,r,~{memory},~{dirflag},~{flags}",
            true);
        Call = Builder.CreateCall(IA, {RDI, RCX, Val, Df}, "rep_scas");
      } else {
        auto *FnTy = llvm::FunctionType::get(
            RetTy, {AddrRegTy, AddrRegTy, AddrRegTy}, false);
        auto *IA = llvm::InlineAsm::get(
            FnTy, Mn, "={di},={cx},={ax},0,1,2,~{memory},~{dirflag},~{flags}",
            true);
        Call = Builder.CreateCall(IA, {RDI, RCX, Val}, "rep_scas");
      }
      LeftCount = Builder.CreateExtractValue(Call, {1}, "scas_cx");
      PackedFlags = Builder.CreateExtractValue(Call, {2}, "scas_flags");
    } else {
      // Inputs [RSI, RDI, RCX], all three tied in-out; AX is a flag-snapshot
      // output and does not represent an architectural RAX write.
      auto *RSI = Coerce(getVar(Op.Inputs[1], Builder), AddrRegTy);
      auto *RDI = Coerce(getVar(Op.Inputs[2], Builder), AddrRegTy);
      auto *RCX = Coerce(getVar(Op.Inputs[3], Builder), AddrRegTy);
      std::string Mn =
          dirWrap(withAddressSize(withSourceSegment(RepPfx + "cmps" + Suffix)),
                  7) +
          "\n\tlahf\n\tseto %al";
      auto *RetTy = llvm::StructType::get(
          *Ctx, {AddrRegTy, AddrRegTy, AddrRegTy, AddrRegTy});
      llvm::CallInst *Call = nullptr;
      if (DfK == DfDyn) {
        auto *Df = Coerce(getVar(Op.Inputs[4], Builder), AddrRegTy);
        auto *FnTy = llvm::FunctionType::get(
            RetTy, {AddrRegTy, AddrRegTy, AddrRegTy, AddrRegTy}, false);
        auto *IA = llvm::InlineAsm::get(FnTy, Mn,
                                        "={si},={di},={cx},={ax},0,1,2,r,"
                                        "~{memory},~{dirflag},~{flags}",
                                        true);
        Call = Builder.CreateCall(IA, {RSI, RDI, RCX, Df}, "rep_cmps");
      } else {
        auto *FnTy = llvm::FunctionType::get(
            RetTy, {AddrRegTy, AddrRegTy, AddrRegTy}, false);
        auto *IA = llvm::InlineAsm::get(FnTy, Mn,
                                        "={si},={di},={cx},={ax},0,1,2,"
                                        "~{memory},~{dirflag},~{flags}",
                                        true);
        Call = Builder.CreateCall(IA, {RSI, RDI, RCX}, "rep_cmps");
      }
      LeftCount = Builder.CreateExtractValue(Call, {2}, "cmps_cx");
      PackedFlags = Builder.CreateExtractValue(Call, {3}, "cmps_flags");
    }
    auto *I16Ty = llvm::Type::getInt16Ty(*Ctx);
    PendingIntrinsicOutputs[0] = Builder.CreateTrunc(PackedFlags, I16Ty);
    PendingIntrinsicCount = 1;
    if (Op.Output.Size == 0)
      return nullptr;
    return Coerce(LeftCount, sizeToType(Op.Output.Size));
  }

  // LODS: RSI/RCX/RAX are tied in-out.  Returning the full accumulator keeps
  // every partial-register rule exact (including EAX zero-extension) while a
  // zero entry count naturally returns the unchanged incoming RAX.
  if (IsLods && Op.NumInputs >= 5) {
    auto *RSI = Coerce(getVar(Op.Inputs[1], Builder), AddrRegTy);
    auto *RCX = Coerce(getVar(Op.Inputs[2], Builder), AddrRegTy);
    auto *RAX = Coerce(getVar(Op.Inputs[3], Builder), AddrRegTy);
    std::string Mn = dirWrap(
        withAddressSize(withSourceSegment(std::string("rep lods") + Suffix)),
        6);
    auto *RetTy =
        llvm::StructType::get(*Ctx, {AddrRegTy, AddrRegTy, AddrRegTy});
    llvm::CallInst *Call = nullptr;
    if (DfK == DfDyn) {
      auto *Df = Coerce(getVar(Op.Inputs[4], Builder), AddrRegTy);
      auto *FnTy = llvm::FunctionType::get(
          RetTy, {AddrRegTy, AddrRegTy, AddrRegTy, AddrRegTy}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mn, "={si},={cx},={ax},0,1,2,r,~{memory},~{dirflag},~{flags}",
          true);
      Call = Builder.CreateCall(IA, {RSI, RCX, RAX, Df}, "rep_lods");
    } else {
      auto *FnTy = llvm::FunctionType::get(
          RetTy, {AddrRegTy, AddrRegTy, AddrRegTy}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mn, "={si},={cx},={ax},0,1,2,~{memory},~{dirflag},~{flags}",
          true);
      Call = Builder.CreateCall(IA, {RSI, RCX, RAX}, "rep_lods");
    }
    auto *Accumulator = Builder.CreateExtractValue(Call, {2}, "lods_ax");
    return Op.Output.Size > 0 ? Coerce(Accumulator, sizeToType(Op.Output.Size))
                              : nullptr;
  }

  // MOVS: inputs [RSI, RDI, RCX], all written by the hardware (discarded).
  if (IsMovs && Op.NumInputs >= 5) {
    auto *RSI = Coerce(getVar(Op.Inputs[1], Builder), AddrRegTy);
    auto *RDI = Coerce(getVar(Op.Inputs[2], Builder), AddrRegTy);
    auto *RCX = Coerce(getVar(Op.Inputs[3], Builder), AddrRegTy);
    std::string Mn = dirWrap(
        withAddressSize(withSourceSegment(std::string("rep movs") + Suffix)),
        6);
    auto *RetTy =
        llvm::StructType::get(*Ctx, {AddrRegTy, AddrRegTy, AddrRegTy});
    if (DfK == DfDyn) {
      auto *Df = Coerce(getVar(Op.Inputs[4], Builder), AddrRegTy);
      auto *FnTy = llvm::FunctionType::get(
          RetTy, {AddrRegTy, AddrRegTy, AddrRegTy, AddrRegTy}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mn, "={si},={di},={cx},0,1,2,r,~{memory},~{dirflag},~{flags}",
          true);
      Builder.CreateCall(IA, {RSI, RDI, RCX, Df});
    } else {
      auto *FnTy = llvm::FunctionType::get(
          RetTy, {AddrRegTy, AddrRegTy, AddrRegTy}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mn, "={si},={di},={cx},0,1,2,~{memory},~{dirflag},~{flags}",
          true);
      Builder.CreateCall(IA, {RSI, RDI, RCX});
    }
    return Handled();
  }

  // REP string instructions use implicit architectural registers.  Emitting a
  // bare mnemonic for malformed IR would silently consume the host RSI/RDI/RCX
  // instead of the lifted values, so an incomplete shape must never reach
  // native code generation.
  llvm::report_fatal_error(
      "x86 REP string intrinsic is missing architectural operands");
}

//===----------------------------------------------------------------------===//
// Top-level x86 value-producing INTRINSIC dispatch
//===----------------------------------------------------------------------===//

// Maps an x87 value-producing intrinsic to the inline-asm text that computes it
// and leaves the single result on the x87 stack top. Push instructions use
// the guarded two-result contract instead. Returns nullptr for other IDs.
static const char *x87AsmForIntrinsic(Intrinsic IC) {
  switch (IC) {
  case Intrinsic::X87Fsin:
    return "fsin";
  case Intrinsic::X87Fcos:
    return "fcos";
  case Intrinsic::X87F2xm1:
    return "f2xm1";
  case Intrinsic::X87Fscale:
    return "fscale";
  case Intrinsic::X87Fprem:
    return "fprem";
  case Intrinsic::X87Fprem1:
    return "fprem1";
  case Intrinsic::X87Fpatan:
    return "fpatan";
  case Intrinsic::X87Fyl2x:
    return "fyl2x";
  case Intrinsic::X87Fyl2xp1:
    return "fyl2xp1";
  default:
    return nullptr;
  }
}

llvm::Value *MedLLVMEmitter::emitX86IntrinsicValue(const MedOp &Op,
                                                   Intrinsic IC,
                                                   llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;

  if (isX87ContextIntrinsic(IC)) {
    if (!x87ContextMedShapeIsValid(Op, TargetArch))
      llvm::report_fatal_error("invalid complete x87 context contract");
    auto *Ptr = llvm::PointerType::get(*Ctx, 0);
    llvm::Value *Context = nullptr;
    if (IC == I::X87ContextBegin) {
      llvm::IRBuilder<> Entry(&CurFunc->getEntryBlock(),
                              CurFunc->getEntryBlock().begin());
      auto *Storage = Entry.CreateAlloca(
          llvm::ArrayType::get(Builder.getInt8Ty(), X87ContextBytes), nullptr,
          "x87_context");
      Storage->setAlignment(llvm::Align(16));
      Context = Storage;
      Builder.CreateMemSet(Context, Builder.getInt8(0), X87ContextBytes,
                           llvm::Align(16));
    } else {
      Context = getVar(Op.Inputs[1], Builder);
      if (!Context->getType()->isPointerTy())
        Context = Builder.CreateIntToPtr(Context, Ptr);
    }
    auto emit = [&](X87ContextAsmPhase Phase, uint32_t Selector = 0,
                    llvm::ArrayRef<llvm::Value *> Args = {}) {
      llvm::SmallVector<llvm::Value *, 4> Arguments;
      if (Args.empty())
        Arguments.push_back(Context);
      else
        Arguments.append(Args.begin(), Args.end());
      llvm::SmallVector<llvm::Type *, 4> Types(Arguments.size(), Ptr);
      const auto Space = Phase == X87ContextAsmPhase::Step
                             ? Op.MemoryAddressSpace
                             : NdMemoryAddressSpace::Default;
      auto *Asm = llvm::InlineAsm::get(
          llvm::FunctionType::get(Builder.getVoidTy(), Types, false),
          x87ContextAsm(Phase, Selector, Space),
          Phase == X87ContextAsmPhase::Step ? X87ContextStepConstraints
          : Phase == X87ContextAsmPhase::Restore ||
                  Phase == X87ContextAsmPhase::RestoreHost
              ? X87ContextRestoreConstraints
              : X87ContextSnapshotConstraints,
          true);
      auto *Call = Builder.CreateCall(Asm, Arguments);
      Call->setMetadata(
          X87ContextAsmMetadata,
          llvm::MDNode::get(
              *Ctx,
              {llvm::ConstantAsMetadata::get(Builder.getInt32(unsigned(Phase))),
               llvm::ConstantAsMetadata::get(Builder.getInt32(Selector)),
               llvm::ConstantAsMetadata::get(
                   Builder.getInt32(unsigned(Space)))}));
    };
    const auto offset = [&](unsigned Bytes) {
      return Builder.CreateGEP(Builder.getInt8Ty(), Context,
                               Builder.getInt64(Bytes));
    };
    if (IC == I::X87ContextBegin || IC == I::X87ContextRefresh) {
      emit(X87ContextAsmPhase::Capture);
      return IC == I::X87ContextBegin
                 ? Builder.CreatePtrToInt(Context, Builder.getInt64Ty())
                 : nullptr;
    }
    if (IC == I::X87ContextMMXRead) {
      auto *Status = Builder.CreateLoad(Builder.getInt16Ty(), offset(2));
      auto *Top =
          Builder.CreateZExt(Builder.CreateAnd(Builder.CreateLShr(Status, 11),
                                               Builder.getInt16(7)),
                             Builder.getInt64Ty());
      auto *Slot = Builder.CreateAnd(
          Builder.CreateSub(Builder.getInt64(Op.Inputs[2].ConstVal + 8), Top),
          Builder.getInt64(7));
      auto *Address = Builder.CreateGEP(
          Builder.getInt8Ty(), Context,
          Builder.CreateAdd(Builder.getInt64(32),
                            Builder.CreateMul(Slot, Builder.getInt64(16))));
      return Builder.CreateLoad(Builder.getInt64Ty(), Address);
    }
    if (IC == I::X87ContextMMXEnter) {
      auto *Status = Builder.CreateLoad(Builder.getInt16Ty(), offset(2));
      auto *Top =
          Builder.CreateZExt(Builder.CreateAnd(Builder.CreateLShr(Status, 11),
                                               Builder.getInt16(7)),
                             Builder.getInt64Ty());
      llvm::Value *Physical[8];
      auto *Raw = Builder.getIntNTy(128);
      for (unsigned I = 0; I < 8; ++I) {
        auto *Slot =
            Builder.CreateAnd(Builder.CreateSub(Builder.getInt64(I + 8), Top),
                              Builder.getInt64(7));
        auto *Address = Builder.CreateGEP(
            Builder.getInt8Ty(), Context,
            Builder.CreateAdd(Builder.getInt64(32),
                              Builder.CreateMul(Slot, Builder.getInt64(16))));
        Physical[I] = Builder.CreateLoad(Raw, Address);
      }
      for (unsigned I = 0; I < 8; ++I)
        Builder.CreateStore(Physical[I], offset(32 + I * 16));
      Builder.CreateStore(Builder.CreateAnd(Status, Builder.getInt16(0xc7ff)),
                          offset(2));
      Builder.CreateStore(Builder.getInt8(0xff), offset(4));
      return nullptr;
    }
    if (IC == I::X87ContextMMXWrite) {
      const unsigned Slot = Op.Inputs[2].ConstVal;
      Builder.CreateStore(getVar(Op.Inputs[3], Builder),
                          offset(32 + Slot * 16));
      Builder.CreateStore(Builder.getInt16(0xffff), offset(40 + Slot * 16));
      return nullptr;
    }
    emit(X87ContextAsmPhase::SaveHost);
    if (IC == I::X87ContextExport) {
      // Preserve live SIMD and MXCSR. Only the x87 header and eight values
      // cross this context's boundary, including retained empty payloads.
      Builder.CreateMemCpy(offset(512), llvm::Align(16), Context,
                           llvm::Align(16), 24);
      Builder.CreateMemCpy(offset(544), llvm::Align(16), offset(32),
                           llvm::Align(16), 128);
      emit(X87ContextAsmPhase::RestoreHost);
      return nullptr;
    }
    Builder.CreateMemCpy(offset(24), llvm::Align(8), offset(536),
                         llvm::Align(8), 8);
    Builder.CreateMemCpy(offset(160), llvm::Align(16), offset(672),
                         llvm::Align(16), 352);
    const uint32_t Selector = Op.Inputs[2].ConstVal;
    const auto Spec = *x87ContextInstruction(Selector);
    llvm::IRBuilder<> Entry(&CurFunc->getEntryBlock(),
                            CurFunc->getEntryBlock().begin());
    auto *Result =
        Entry.CreateAlloca(Builder.getInt64Ty(), nullptr, "x87_result");
    auto *Flags =
        Entry.CreateAlloca(Builder.getInt64Ty(), nullptr, "x87_flags");
    Builder.CreateStore(Builder.getInt64(0), Result);
    Builder.CreateStore(getVar(Op.Inputs[4], Builder), Flags);
    auto *Address = getVar(Op.Inputs[3], Builder);
    if (!Address->getType()->isPointerTy())
      Address = Builder.CreateIntToPtr(Address, Ptr);
    llvm::Value *PreviousIP = nullptr;
    llvm::Value *PreviousOpcode = nullptr;
    if (x87ContextUpdatesInstructionPointer(Selector))
      PreviousIP = Builder.CreateLoad(Builder.getInt64Ty(), offset(8));
    if (PreviousIP)
      PreviousOpcode = Builder.CreateLoad(Builder.getInt16Ty(), offset(6));
    emit(X87ContextAsmPhase::Step, Selector, {Context, Result, Address, Flags});
    if (PreviousIP) {
      auto *CurrentIP = Builder.CreateLoad(Builder.getInt64Ty(), offset(8));
      auto *Changed = Builder.CreateICmpNE(CurrentIP, PreviousIP);
      Builder.CreateStore(Builder.CreateSelect(Changed,
                                               getVar(Op.Inputs[5], Builder),
                                               CurrentIP),
                          offset(8));
      const unsigned Original =
          (Selector & X87ContextMemory)
              ? (Selector >> X87ContextOriginalModRMShift) & 0xff
              : (Selector >> 8) & 0xff;
      auto *OldOpcode = Builder.CreateLoad(Builder.getInt16Ty(), offset(6));
      auto *Status = Builder.CreateLoad(Builder.getInt16Ty(), offset(2));
      // An FIP update does not imply an FOP update. Preserve an unchanged
      // observed opcode unless this instruction raised an unmasked exception.
      auto *OpcodeChanged = Builder.CreateOr(
          Builder.CreateICmpNE(OldOpcode, PreviousOpcode),
          Builder.CreateICmpNE(
              Builder.CreateAnd(Status, Builder.getInt16(0x80)),
              Builder.getInt16(0)));
      Builder.CreateStore(
          Builder.CreateSelect(
              OpcodeChanged, Builder.getInt16(((Selector & 7) << 8) | Original),
              OldOpcode),
          offset(6));
    }
    if (Spec.WritesAX)
      return Builder.CreateLoad(Builder.getInt16Ty(), Result);
    if (Spec.WritesFlags) {
      auto *Raw = Builder.CreateLoad(Builder.getInt64Ty(), Result);
      return Builder.CreateOr(
          Builder.CreateAnd(Raw, Builder.getInt64(0xd5)),
          Builder.CreateShl(Builder.CreateAnd(Builder.CreateLShr(Raw, 8),
                                              Builder.getInt64(1)),
                            11));
    }
    return nullptr;
  }
  if (IC == I::CetRdSsp) {
    const auto Shape = x86ShadowStackReadMedShape(Op, TargetArch);
    if (!x86ShadowStackReadShapeIsValid(Shape))
      llvm::report_fatal_error("invalid x86 shadow stack read contract");
    auto *Ty = llvm::Type::getIntNTy(*Ctx, Shape.OutputSize * 8);
    auto *Old = getVar(Op.Inputs[1], Builder);
    if (Old->getType()->isPointerTy())
      Old = Builder.CreatePtrToInt(Old, Ty);
    else if (Old->getType() != Ty)
      llvm::report_fatal_error("shadow stack old value is not a full GPR");
    auto *Fn = llvm::FunctionType::get(Ty, {Ty}, false);
    auto *Asm = llvm::InlineAsm::get(
        Fn, x86ShadowStackReadAsm(Shape.OutputSize, Shape.ReadWidth),
        X86ShadowStackReadConstraints, true);
    auto *Result = Builder.CreateCall(Asm, {Old}, "shadow_stack_read");
    Result->setMetadata(X86ShadowStackReadMetadata,
                        llvm::MDNode::get(*Ctx, {}));
    return Result;
  }

  if (isX87GuardedValueIntrinsic(IC)) {
    if (!x87GuardedValueShapeIsValid(x87GuardedValueMedShape(Op, TargetArch)))
      llvm::report_fatal_error("invalid guarded x87 value/control contract");
    llvm::IRBuilder<> Entry(&CurFunc->getEntryBlock(),
                            CurFunc->getEntryBlock().begin());
    auto *Bits = Builder.getIntNTy(80);
    auto *ResultBits = Builder.getIntNTy(x87GuardedValueBytes(IC) * 8);
    auto *Source = Entry.CreateAlloca(Bits, nullptr, "x87_value_source");
    auto *Result = Entry.CreateAlloca(ResultBits, nullptr, "x87_value_result");
    auto *Control =
        Entry.CreateAlloca(Builder.getInt16Ty(), nullptr, "x87_value_control");
    auto *Status =
        Entry.CreateAlloca(Builder.getInt16Ty(), nullptr, "x87_value_status");
    Builder.CreateStore(getVar(Op.Inputs[1], Builder), Source);
    Builder.CreateStore(getVar(Op.Inputs[2], Builder), Control);
    auto *Ptr = llvm::PointerType::get(*Ctx, 0);
    auto *Type = llvm::FunctionType::get(Builder.getVoidTy(),
                                         {Ptr, Ptr, Ptr, Ptr}, false);
    auto *Asm = llvm::InlineAsm::get(Type, x87GuardedValueAsm(IC),
                                     x87GuardedValueConstraints(), true);
    auto *Call = Builder.CreateCall(Asm, {Result, Source, Control, Status});
    Call->setMetadata(X87GuardedValueAsmMetadata, llvm::MDNode::get(*Ctx, {}));
    PendingX87Status = Builder.CreateLoad(Builder.getInt16Ty(), Status);
    PendingX87StatusBlock = Builder.GetInsertBlock();
    return Builder.CreateLoad(ResultBits, Result);
  }
  if (isX87ConstantControlIntrinsic(IC)) {
    if (!x87ConstantControlShapeIsValid(
            IC, x87ConstantControlMedShape(Op, TargetArch)))
      llvm::report_fatal_error("invalid x87 constant/control contract");
    llvm::IRBuilder<> EntryBuilder(&CurFunc->getEntryBlock(),
                                   CurFunc->getEntryBlock().begin());
    auto *Word =
        EntryBuilder.CreateAlloca(Builder.getInt16Ty(), nullptr, "x87_control");
    auto *Ptr = llvm::PointerType::get(*Ctx, 0);
    llvm::SmallVector<llvm::Type *, 2> Types{Ptr};
    llvm::SmallVector<llvm::Value *, 2> Args{Word};
    std::string Text = IC == I::X87ReadControl ? "fnstcw ($0)" : "fldcw ($0)";
    const char *Constraints = X87ControlConstraints;
    llvm::AllocaInst *Value = nullptr;
    if (IC != I::X87ReadControl)
      Builder.CreateStore(
          getVar(Op.Inputs[IC == I::X87LoadConstant ? 2 : 1], Builder), Word);
    if (IC == I::X87LoadConstant) {
      Value = EntryBuilder.CreateAlloca(Builder.getIntNTy(80), nullptr,
                                        "x87_constant");
      Text = x87ConstantAsm(Op.Inputs[1].ConstVal);
      Constraints = X87ConstantConstraints;
      Types.push_back(Ptr);
      Args = {Value, Word};
    }
    auto *Type = llvm::FunctionType::get(Builder.getVoidTy(), Types, false);
    auto *Asm = llvm::InlineAsm::get(Type, Text, Constraints, true);
    auto *Call = Builder.CreateCall(Asm, Args);
    Call->setMetadata(X87ConstantAsmMetadata, llvm::MDNode::get(*Ctx, {}));
    return Value ? Builder.CreateLoad(Builder.getIntNTy(80), Value)
           : IC == I::X87ReadControl
               ? Builder.CreateLoad(Builder.getInt16Ty(), Word)
               : nullptr;
  }
  if (isX86FPStateIntrinsic(IC)) {
    const auto Shape = x86FPStateMedShape(Op, TargetArch);
    if (!x86FPStateShapeIsValid(IC, Shape))
      llvm::report_fatal_error("invalid x86 numerical/state contract");
    if (isX86FPApprox12Intrinsic(IC)) {
      const bool Memory = IC == I::X86FPApprox12MemoryState;
      const unsigned Layout = x86FPStateHelperLayout(IC, Shape);
      auto *Type = x86FPApprox12LLVMType(*Ctx, Layout);
      auto *Source = getVar(Op.Inputs[Memory ? 1 : 2], Builder);
      if (Memory) {
        auto *Integer = Builder.getInt64Ty();
        Source = Source->getType()->isPointerTy()
                     ? Builder.CreatePtrToInt(Source, Integer)
                     : Builder.CreateZExtOrTrunc(Source, Integer);
        Source = Builder.CreateIntToPtr(
            Source, llvm::PointerType::get(*Ctx, llvmX86MemoryAddressSpace(
                                                     Op.MemoryAddressSpace)));
      } else
        Source = Builder.CreateBitCast(Source, Type);
      auto *Fn = llvm::FunctionType::get(Type, {Source->getType()}, false);
      auto *Asm = llvm::InlineAsm::get(Fn, x86FPApprox12Asm(Layout, Memory),
                                       Memory ? X86FPApprox12MemoryConstraints
                                              : X86FPApprox12ValueConstraints,
                                       true);
      auto *Result = Builder.CreateCall(Asm, {Source}, "fp_approx12");
      Result->setMetadata(X86FPStateAsmMetadata, llvm::MDNode::get(*Ctx, {}));
      return Builder.CreateBitCast(Result, sizeToType(Op.Output.Size));
    }
    auto *I32 = llvm::Type::getInt32Ty(*Ctx);
    auto *Ptr = llvm::PointerType::getUnqual(*Ctx);
    llvm::IRBuilder<> EntryBuilder(&CurFunc->getEntryBlock(),
                                   CurFunc->getEntryBlock().begin());
    auto *State = EntryBuilder.CreateAlloca(I32, nullptr, "mxcsr_state");
    const auto Mark = [&](llvm::CallInst *Call) {
      Call->setMetadata(X86FPStateAsmMetadata, llvm::MDNode::get(*Ctx, {}));
    };
    const auto Raw = [&](unsigned Index, unsigned Size) {
      auto *Value = getVar(Op.Inputs[Index], Builder);
      auto *Ty = llvm::Type::getIntNTy(*Ctx, Size * 8);
      if (Value->getType()->isFloatingPointTy() ||
          Value->getType()->isVectorTy())
        Value = Builder.CreateBitCast(Value, Ty);
      else
        Value = Builder.CreateZExtOrTrunc(Value, Ty);
      return Value;
    };
    if (IC == I::X86ReadMXCSR || IC == I::X86WriteMXCSR) {
      if (IC == I::X86WriteMXCSR)
        Builder.CreateStore(Raw(1, 4), State);
      auto *Fn =
          llvm::FunctionType::get(llvm::Type::getVoidTy(*Ctx), {Ptr}, false);
      auto *Asm = llvm::InlineAsm::get(
          Fn, IC == I::X86ReadMXCSR ? X86FPStateReadAsm : X86FPStateWriteAsm,
          X86FPStateTransferConstraints, true);
      Mark(Builder.CreateCall(Asm, {State}));
      return IC == I::X86ReadMXCSR ? Builder.CreateLoad(I32, State) : nullptr;
    }
    if (isX86FPMaskCompareStateIntrinsic(IC)) {
      const bool Memory = IC == I::X86FPMaskCompareMemoryState;
      const unsigned Bytes = Op.Output.Size - 4;
      const unsigned Layout = x86FPStateHelperLayout(IC, Shape);
      auto *Type = x86FPMaskCompareLLVMType(*Ctx, Layout);
      auto *Left = Builder.CreateBitCast(Raw(Memory ? 4 : 3, Bytes), Type);
      llvm::Value *Right;
      if (Memory) {
        auto *Address = getVar(Op.Inputs[1], Builder);
        auto *I64 = Builder.getInt64Ty();
        Address = Address->getType()->isPointerTy()
                      ? Builder.CreatePtrToInt(Address, I64)
                      : Builder.CreateZExtOrTrunc(Address, I64);
        Right = Builder.CreateIntToPtr(
            Address, llvm::PointerType::get(*Ctx, llvmX86MemoryAddressSpace(
                                                      Op.MemoryAddressSpace)));
      } else
        Right = Builder.CreateBitCast(Raw(4, Bytes), Type);
      Builder.CreateStore(Raw(5, 4), State);
      auto *Fn =
          llvm::FunctionType::get(Type, {Type, Right->getType(), Ptr}, false);
      auto *Asm =
          llvm::InlineAsm::get(Fn, x86FPMaskCompareAsm(Layout, Memory),
                               x86FPMaskCompareConstraints(Memory), true);
      auto *Result = Builder.CreateCall(Asm, {Left, Right, State}, "cmp_mask");
      Mark(Result);
      Result->setMetadata(
          X86FPMaskCompareAsmMetadata,
          llvm::MDNode::get(
              *Ctx, llvm::ConstantAsMetadata::get(Builder.getInt32(Layout))));
      auto *Bits =
          Builder.CreateBitCast(Result, llvm::Type::getIntNTy(*Ctx, Bytes * 8));
      auto *OutTy = sizeToType(Op.Output.Size);
      auto *Status = Builder.CreateLoad(I32, State, "cmp_state");
      return Builder.CreateOr(
          Builder.CreateZExt(Bits, OutTy),
          Builder.CreateShl(Builder.CreateZExt(Status, OutTy), Bytes * 8));
    }
    if (isX86FPComiStateIntrinsic(IC)) {
      const bool Memory = IC == I::X86FPComiMemoryState;
      const unsigned Layout = x86FPStateHelperLayout(IC, Shape);
      const unsigned Bytes = x86FPComiElementBytes(Shape.Control);
      auto *Type = Bytes == 4 ? Builder.getFloatTy() : Builder.getDoubleTy();
      auto *Left = Builder.CreateBitCast(Raw(Memory ? 3 : 2, Bytes), Type);
      llvm::Value *Right;
      if (Memory) {
        auto *Address = getVar(Op.Inputs[1], Builder);
        auto *I64 = Builder.getInt64Ty();
        Address = Address->getType()->isPointerTy()
                      ? Builder.CreatePtrToInt(Address, I64)
                      : Builder.CreateZExtOrTrunc(Address, I64);
        Right = Builder.CreateIntToPtr(
            Address, llvm::PointerType::get(*Ctx, llvmX86MemoryAddressSpace(
                                                      Op.MemoryAddressSpace)));
      } else
        Right = Builder.CreateBitCast(Raw(3, Bytes), Type);
      Builder.CreateStore(Raw(4, 4), State);
      auto *Fn =
          llvm::FunctionType::get(I32, {Type, Right->getType(), Ptr}, false);
      auto *Asm = llvm::InlineAsm::get(Fn, x86FPComiAsm(Layout, Memory),
                                       x86FPComiConstraints(Memory), true);
      auto *Flags = Builder.CreateCall(Asm, {Left, Right, State}, "comi_flags");
      Mark(Flags);
      auto *OutTy = Builder.getInt64Ty();
      auto *Status = Builder.CreateLoad(I32, State, "comi_state");
      return Builder.CreateOr(
          Builder.CreateZExt(Flags, OutTy),
          Builder.CreateShl(Builder.CreateZExt(Status, OutTy), 32));
    }
    if (isX86F16CStateIntrinsic(IC)) {
      const bool Store = IC == I::X86F16CStoreState,
                 Read = IC == I::X86F16CReadState;
      const unsigned Control = Shape.Control,
                     Bytes = x86F16CDestinationBytes(Control);
      llvm::Value *Source = nullptr, *Address = nullptr;
      if (Read || Store) {
        Address = getVar(Op.Inputs[1], Builder);
        auto *I64 = Builder.getInt64Ty();
        Address = Address->getType()->isPointerTy()
                      ? Builder.CreatePtrToInt(Address, I64)
                      : Builder.CreateZExtOrTrunc(Address, I64);
        Address = Builder.CreateIntToPtr(
            Address, llvm::PointerType::get(*Ctx, llvmX86MemoryAddressSpace(
                                                      Op.MemoryAddressSpace)));
      }
      if (!Read) {
        auto *Carrier = Builder.CreateZExtOrTrunc(
            Raw(Store ? 2 : 1, x86F16CSourceBytes(Control)),
            llvm::Type::getIntNTy(
                *Ctx, std::max(16u, x86F16CSourceBytes(Control)) * 8));
        Source =
            Builder.CreateBitCast(Carrier, x86F16CSourceType(*Ctx, Control));
      }
      Builder.CreateStore(Raw(Store ? 5 : 4, 4), State);
      std::vector<llvm::Value *> Args;
      if (Store || Read)
        Args.push_back(Address);
      if (!Read)
        Args.push_back(Source);
      Args.push_back(State);
      std::vector<llvm::Type *> Types;
      for (auto *Arg : Args)
        Types.push_back(Arg->getType());
      auto *Type = Store ? llvm::Type::getVoidTy(*Ctx)
                         : x86F16CResultType(*Ctx, Control);
      auto *Fn = llvm::FunctionType::get(Type, Types, false);
      auto *Asm = llvm::InlineAsm::get(
          Fn, x86F16CAsm(IC, x86FPStateHelperLayout(IC, Shape)),
          x86F16CConstraints(IC), true);
      auto *Result = Builder.CreateCall(Asm, Args);
      Mark(Result);
      auto *Status = Builder.CreateLoad(I32, State, "fp_state");
      if (Store)
        return Status;
      auto *Bits =
          Builder.CreateBitCast(Result, llvm::Type::getIntNTy(*Ctx, Bytes * 8));
      auto *OutTy = sizeToType(Op.Output.Size);
      return Builder.CreateOr(
          Builder.CreateZExt(Bits, OutTy),
          Builder.CreateShl(Builder.CreateZExt(Status, OutTy), Bytes * 8));
    }
    if (isX86FPPackedConvertStateIntrinsic(IC)) {
      const unsigned Control = Shape.Control;
      const unsigned SourceBytes = x86FPPackedConvertSourceBytes(Control);
      const unsigned Bytes = x86FPPackedConvertDestinationBytes(Control);
      const bool Memory = IC == I::X86FPPackedConvertMemoryState;
      const unsigned Layout = x86FPStateHelperLayout(IC, Shape);
      auto *Type = x86FPPackedConvertResultType(*Ctx, Control);
      llvm::Value *Source;
      if (Memory) {
        auto *Address = getVar(Op.Inputs[1], Builder);
        auto *I64 = Builder.getInt64Ty();
        Address = Address->getType()->isPointerTy()
                      ? Builder.CreatePtrToInt(Address, I64)
                      : Builder.CreateZExtOrTrunc(Address, I64);
        Source = Builder.CreateIntToPtr(
            Address, llvm::PointerType::get(*Ctx, llvmX86MemoryAddressSpace(
                                                      Op.MemoryAddressSpace)));
      } else {
        // The logical m64/low64 source still occupies one physical XMM carrier.
        // Sub-XMM vectors can legalize into separate scalar asm operands.
        auto *Carrier = Builder.CreateZExtOrTrunc(
            Raw(1, SourceBytes),
            llvm::Type::getIntNTy(*Ctx, std::max(16u, SourceBytes) * 8));
        Source = Builder.CreateBitCast(
            Carrier, x86FPPackedConvertSourceType(*Ctx, Control));
      }
      Builder.CreateStore(Raw(3, 4), State);
      auto *Fn = llvm::FunctionType::get(Type, {Source->getType(), Ptr}, false);
      auto *Asm =
          llvm::InlineAsm::get(Fn, x86FPPackedConvertAsm(Layout, Memory),
                               x86FPPackedConvertConstraints(Memory), true);
      auto *Result = Builder.CreateCall(Asm, {Source, State}, "packed_convert");
      Mark(Result);
      auto *Bits =
          Builder.CreateBitCast(Result, llvm::Type::getIntNTy(*Ctx, Bytes * 8));
      auto *OutTy = sizeToType(Op.Output.Size);
      auto *Status = Builder.CreateLoad(I32, State, "fp_state");
      return Builder.CreateOr(
          Builder.CreateZExt(Bits, OutTy),
          Builder.CreateShl(Builder.CreateZExt(Status, OutTy), Bytes * 8));
    }
    if (isX86FPScalarConvertStateIntrinsic(IC)) {
      const unsigned Control = Shape.Control;
      const unsigned SourceBytes = x86FPScalarConvertSourceBytes(Control);
      const unsigned DestinationBytes =
          x86FPScalarConvertDestinationBytes(Control);
      const bool Memory = IC == I::X86FPScalarConvertMemoryState;
      const unsigned Layout = x86FPStateHelperLayout(IC, Shape);
      auto *Type = x86FPScalarConvertResultType(*Ctx, Control);
      llvm::Value *Source;
      if (Memory) {
        auto *Address = getVar(Op.Inputs[1], Builder);
        auto *I64 = Builder.getInt64Ty();
        Address = Address->getType()->isPointerTy()
                      ? Builder.CreatePtrToInt(Address, I64)
                      : Builder.CreateZExtOrTrunc(Address, I64);
        Source = Builder.CreateIntToPtr(
            Address, llvm::PointerType::get(*Ctx, llvmX86MemoryAddressSpace(
                                                      Op.MemoryAddressSpace)));
      } else
        Source = Builder.CreateBitCast(
            Raw(1, SourceBytes), x86FPScalarConvertSourceType(*Ctx, Control));
      Builder.CreateStore(Raw(3, 4), State);
      auto *Fn =
          llvm::FunctionType::get(Type, {Type, Source->getType(), Ptr}, false);
      auto *Asm = llvm::InlineAsm::get(
          Fn, x86FPScalarConvertAsm(Layout, Memory),
          x86FPScalarConvertConstraints(Control, Memory), true);
      auto *Result = Builder.CreateCall(
          Asm, {llvm::ConstantFP::get(Type, 0.0), Source, State}, "fp_convert");
      Mark(Result);
      auto *Bits = Builder.CreateBitCast(
          Result, llvm::Type::getIntNTy(*Ctx, DestinationBytes * 8));
      auto *OutTy = sizeToType(Op.Output.Size);
      auto *Status = Builder.CreateLoad(I32, State, "fp_state");
      return Builder.CreateOr(
          Builder.CreateZExt(Bits, OutTy),
          Builder.CreateShl(Builder.CreateZExt(Status, OutTy),
                            DestinationBytes * 8));
    }
    if (isX86FPConversionStateIntrinsic(IC)) {
      const unsigned SourceBytes = Op.Inputs[1].Size;
      const unsigned DestinationBytes =
          static_cast<unsigned>(Shape.DestinationBytes);
      auto *Scalar = SourceBytes == 4 ? llvm::Type::getFloatTy(*Ctx)
                                      : llvm::Type::getDoubleTy(*Ctx);
      auto *Integer = llvm::Type::getIntNTy(*Ctx, DestinationBytes * 8);
      auto *Value = Builder.CreateBitCast(Raw(1, SourceBytes), Scalar);
      Builder.CreateStore(Raw(2, 4), State);
      auto *Fn = llvm::FunctionType::get(Integer, {Scalar, Ptr}, false);
      auto *Asm =
          llvm::InlineAsm::get(Fn, x86FPStateConversionAsm(IC, SourceBytes),
                               X86FPStateConversionConstraints, true);
      auto *Result = Builder.CreateCall(Asm, {Value, State}, "fp_integer");
      Mark(Result);
      auto *OutTy = sizeToType(Op.Output.Size);
      auto *Status = Builder.CreateLoad(I32, State, "fp_state");
      return Builder.CreateOr(
          Builder.CreateZExt(Result, OutTy),
          Builder.CreateShl(Builder.CreateZExt(Status, OutTy),
                            DestinationBytes * 8));
    }
    if (isX86FPRoundStateIntrinsic(IC)) {
      const bool Memory = IC == I::X86FPRoundMemoryState;
      const unsigned Bytes = Memory ? Shape.OutputSize - 4 : Shape.RightSize;
      const unsigned Layout = x86FPStateHelperLayout(IC, Shape);
      auto *Type = x86FPRoundStateLLVMType(*Ctx, Layout);
      llvm::Value *Source;
      if (Memory) {
        auto *Address = getVar(Op.Inputs[1], Builder);
        auto *I64 = llvm::Type::getInt64Ty(*Ctx);
        if (Address->getType()->isPointerTy())
          Address = Builder.CreatePtrToInt(Address, I64);
        else
          Address = Builder.CreateZExtOrTrunc(Address, I64);
        Source = Builder.CreateIntToPtr(
            Address, llvm::PointerType::get(*Ctx, llvmX86MemoryAddressSpace(
                                                      Op.MemoryAddressSpace)));
      } else
        Source = Builder.CreateBitCast(Raw(2, Bytes), Type);
      Builder.CreateStore(Raw(4, 4), State);
      auto *Fn = llvm::FunctionType::get(Type, {Source->getType(), Ptr}, false);
      auto *Asm = llvm::InlineAsm::get(Fn,
                                       Memory ? x86FPRoundMemoryStateAsm(Layout)
                                              : x86FPRoundStateAsm(Layout),
                                       Memory ? X86FPStateRoundMemoryConstraints
                                              : X86FPStateRoundConstraints,
                                       true);
      auto *Result = Builder.CreateCall(Asm, {Source, State}, "fp_round");
      Mark(Result);
      auto *Bits =
          Builder.CreateBitCast(Result, llvm::Type::getIntNTy(*Ctx, Bytes * 8));
      auto *OutTy = sizeToType(Op.Output.Size);
      auto *Status = Builder.CreateLoad(I32, State, "fp_state");
      return Builder.CreateOr(
          Builder.CreateZExt(Bits, OutTy),
          Builder.CreateShl(Builder.CreateZExt(Status, OutTy), Bytes * 8));
    }
    if (isX86FPArithStateIntrinsic(IC)) {
      const unsigned Layout = x86FPStateHelperLayout(IC, Shape);
      const unsigned Control = x86FPArithStateControl(Layout);
      const unsigned Bytes = x86FPStateSourceBytes(Layout);
      const bool Memory = isX86FPStateMemoryIntrinsic(IC);
      const bool Fma = isX86FPFmaStateIntrinsic(IC);
      const bool Unary = x86FPArithStateIsUnary(Control);
      auto *Type = x86FPArithStateLLVMType(*Ctx, Layout);
      llvm::Value *RHS;
      if (Memory) {
        auto *Address = getVar(Op.Inputs[1], Builder);
        auto *I64 = Builder.getInt64Ty();
        Address = Address->getType()->isPointerTy()
                      ? Builder.CreatePtrToInt(Address, I64)
                      : Builder.CreateZExtOrTrunc(Address, I64);
        RHS = Builder.CreateIntToPtr(
            Address, llvm::PointerType::get(*Ctx, llvmX86MemoryAddressSpace(
                                                      Op.MemoryAddressSpace)));
      } else
        RHS = Builder.CreateBitCast(Raw(Fma ? 4 : 3, Bytes), Type);
      llvm::SmallVector<llvm::Value *, 3> Args;
      llvm::SmallVector<llvm::Type *, 3> Types;
      if (!Unary) {
        auto *LHS = Builder.CreateBitCast(Raw(Memory ? 3 : 2, Bytes), Type);
        Args.push_back(LHS);
        Types.push_back(Type);
      }
      if (Fma) {
        Args.push_back(Builder.CreateBitCast(Raw(Memory ? 4 : 3, Bytes), Type));
        Types.push_back(Type);
      }
      Builder.CreateStore(Raw(Fma ? 5 : 4, 4), State);
      Args.append({RHS, State});
      Types.append({RHS->getType(), Ptr});
      auto *Fn = llvm::FunctionType::get(Type, Types, false);
      auto *Asm = llvm::InlineAsm::get(
          Fn, x86FPArithStateAsm(Layout, Memory),
          x86FPArithStateConstraints(Layout, Memory), true);
      auto *Result = Builder.CreateCall(Asm, Args, "fp_arithmetic");
      Mark(Result);
      auto *Bits =
          Builder.CreateBitCast(Result, llvm::Type::getIntNTy(*Ctx, Bytes * 8));
      auto *OutTy = sizeToType(Op.Output.Size);
      auto *Status = Builder.CreateLoad(I32, State, "fp_state");
      return Builder.CreateOr(
          Builder.CreateZExt(Bits, OutTy),
          Builder.CreateShl(Builder.CreateZExt(Status, OutTy), Bytes * 8));
    }
    const unsigned Bytes = Op.Inputs[1].Size;
    auto *Scalar = Bytes == 4 ? llvm::Type::getFloatTy(*Ctx)
                              : llvm::Type::getDoubleTy(*Ctx);
    auto *Left = Builder.CreateBitCast(Raw(1, Bytes), Scalar);
    auto *Right = Builder.CreateBitCast(Raw(2, Bytes), Scalar);
    Builder.CreateStore(Raw(3, 4), State);
    auto *Fn = llvm::FunctionType::get(Scalar, {Scalar, Scalar, Ptr}, false);
    // One completion scope fixes dynamic rounding, DAZ/FTZ, sticky flags,
    // unmasked traps and x86 NaN operand priority. Generic LLVM arithmetic
    // cannot promise all of these even with constrained FP intrinsics.
    auto *Asm = llvm::InlineAsm::get(Fn, x86FPStateBinaryAsm(IC, Bytes),
                                     X86FPStateBinaryConstraints, true);
    auto *Result = Builder.CreateCall(Asm, {Left, Right, State}, "fp_value");
    Mark(Result);
    auto *OutTy = sizeToType(Op.Output.Size);
    auto *Bits =
        Builder.CreateBitCast(Result, llvm::Type::getIntNTy(*Ctx, Bytes * 8));
    auto *Status = Builder.CreateLoad(I32, State, "fp_state");
    return Builder.CreateOr(
        Builder.CreateZExt(Bits, OutTy),
        Builder.CreateShl(Builder.CreateZExt(Status, OutTy), Bytes * 8));
  }

  if (IC == I::X64Syscall) {
    if (TargetArch != Arch::X64 || TargetFormat != BinaryFormat::ELF ||
        !llvm::Triple(Mod->getTargetTriple()).isOSLinux())
      llvm::report_fatal_error(
          "X64Syscall requires the Linux x64 execution ABI");
    if (Op.NumInputs != 6 || Op.Output.Size != 16 || Op.Inputs[1].Size != 8 ||
        Op.Inputs[2].Size != 8 || Op.Inputs[3].Size != 16 ||
        Op.Inputs[4].Size != 16 || Op.Inputs[5].Size != 8 ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Op.MemoryOrdering != NdMemoryOrdering::None)
      llvm::report_fatal_error("invalid X64Syscall register operands");
    auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
    auto *I128Ty = llvm::Type::getInt128Ty(*Ctx);
    auto *PairTy =
        llvm::StructType::getTypeByName(*Ctx, "neverd.x64.syscall_result");
    if (!PairTy)
      PairTy = llvm::StructType::create(*Ctx, {I64Ty, I64Ty},
                                        "neverd.x64.syscall_result");
    if (PairTy->isOpaque() || PairTy->getNumElements() != 2 ||
        PairTy->getElementType(0) != I64Ty ||
        PairTy->getElementType(1) != I64Ty)
      llvm::report_fatal_error("invalid x64 syscall result structure");
    llvm::Value *Number = getVar(Op.Inputs[1], Builder);
    // getVar can materialize a known scalar through register backing loads.
    // Use the relocation-aware MedIR control proof to identify the ABI call.
    const auto KnownNumber = controlConstantMayRelocate(Op.Inputs[1])
                                 ? std::nullopt
                                 : traceControlConst(Op.Inputs[1]);
    const std::optional<unsigned> Count =
        KnownNumber ? linuxX64SyscallArgumentCount(*KnownNumber) : std::nullopt;
    // A syscall whose arity is known reads only its own arguments. Do not
    // invent dependencies on undefined entry registers outside its ABI.
    const auto Operand = [&](size_t Index, llvm::Type *Type) -> llvm::Value * {
      return !Count || x64SyscallOperandRead(Index, *Count)
                 ? getVar(Op.Inputs[1 + Index], Builder)
                 : llvm::ConstantInt::get(Type, 0);
    };
    llvm::Value *DxSi = Operand(2, I128Ty);
    llvm::Value *R8R10 = Operand(3, I128Ty);
    llvm::Value *Arguments[] = {
        Number,
        Operand(1, I64Ty),
        Builder.CreateTrunc(DxSi, I64Ty),
        Builder.CreateTrunc(Builder.CreateLShr(DxSi, 64), I64Ty),
        Builder.CreateTrunc(R8R10, I64Ty),
        Builder.CreateTrunc(Builder.CreateLShr(R8R10, 64), I64Ty),
        Operand(4, I64Ty)};
    auto *FnTy = llvm::FunctionType::get(
        PairTy, {I64Ty, I64Ty, I64Ty, I64Ty, I64Ty, I64Ty, I64Ty}, false);
    auto *Asm = llvm::InlineAsm::get(
        FnTy, "syscall",
        "={ax},={r11},0,{di},{si},{dx},{r10},{r8},{r9},~{rcx},~{memory},"
        "~{dirflag},~{fpsr},~{flags}",
        true);
    auto *Result = Builder.CreateCall(Asm, Arguments, "syscall_result");
    auto *Rax =
        Builder.CreateZExt(Builder.CreateExtractValue(Result, 0), I128Ty);
    auto *R11 =
        Builder.CreateZExt(Builder.CreateExtractValue(Result, 1), I128Ty);
    return Builder.CreateOr(Rax, Builder.CreateShl(R11, 64));
  }

  if (IC == I::X64WindowsSyscall) {
    if (TargetArch != Arch::X64 || TargetFormat != BinaryFormat::COFF ||
        !llvm::Triple(Mod->getTargetTriple()).isOSWindows())
      llvm::report_fatal_error(
          "X64WindowsSyscall requires the Windows x64 execution ABI");
    if (Op.NumInputs != 6 || Op.Output.Size != 48 ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Op.MemoryOrdering != NdMemoryOrdering::None)
      llvm::report_fatal_error("invalid X64WindowsSyscall register operands");
    for (unsigned Index = 1; Index < 6; ++Index)
      if (Op.Inputs[Index].Size != 8)
        llvm::report_fatal_error("invalid X64WindowsSyscall register operands");
    auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
    auto *ResultTy = llvm::Type::getIntNTy(*Ctx, 384);
    llvm::Type *Fields[6] = {I64Ty, I64Ty, I64Ty, I64Ty, I64Ty, I64Ty};
    auto *RegsTy = llvm::StructType::getTypeByName(
        *Ctx, "neverd.x64.windows_syscall_result");
    if (!RegsTy)
      RegsTy = llvm::StructType::create(*Ctx, Fields,
                                        "neverd.x64.windows_syscall_result");
    if (RegsTy->isOpaque() || RegsTy->getNumElements() != 6 ||
        !llvm::all_of(RegsTy->elements(),
                      [&](llvm::Type *Field) { return Field == I64Ty; }))
      llvm::report_fatal_error("invalid Windows x64 syscall result structure");
    // Service number in RAX, then R10, RDX, R8, R9; the handler hands back
    // RAX, R11 and the volatile RDX, R8, R9 and R10.
    llvm::Value *Arguments[] = {
        getVar(Op.Inputs[1], Builder), getVar(Op.Inputs[3], Builder),
        getVar(Op.Inputs[4], Builder), getVar(Op.Inputs[5], Builder),
        getVar(Op.Inputs[2], Builder)};
    auto *FnTy = llvm::FunctionType::get(
        RegsTy, {I64Ty, I64Ty, I64Ty, I64Ty, I64Ty}, false);
    auto *Asm = llvm::InlineAsm::get(
        FnTy, "syscall",
        "={ax},={r11},={dx},={r8},={r9},={r10},0,2,3,4,5,~{rcx},~{memory},"
        "~{dirflag},~{fpsr},~{flags}",
        true);
    auto *Regs = Builder.CreateCall(Asm, Arguments, "syscall_result");
    llvm::Value *Packed = llvm::ConstantInt::get(ResultTy, 0);
    for (unsigned Index = 0; Index < 6; ++Index) {
      llvm::Value *Field =
          Builder.CreateZExt(Builder.CreateExtractValue(Regs, Index), ResultTy);
      Packed = Builder.CreateOr(Packed, Builder.CreateShl(Field, 64 * Index));
    }
    return Packed;
  }

  if (x87ValueInfo(IC) &&
      !x87ValueShapeIsValid(x87ValueMedShape(Op, TargetArch)))
    llvm::report_fatal_error("invalid x87 scalar value/status contract");

  // Status-producing x87 intrinsics capture the word in their own asm before
  // a compiler spill can change it. The synthetic read consumes that value.
  if (IC == I::X87ReadStatus) {
    if (Op.NumInputs != 1 || Op.Output.Size != 2 || !PendingX87Status ||
        PendingX87StatusBlock != Builder.GetInsertBlock())
      llvm::report_fatal_error(
          "X87ReadStatus requires an immediately preceding status-producing "
          "x87 intrinsic in the same block");
    llvm::Value *Status = PendingX87Status;
    PendingX87Status = nullptr;
    PendingX87StatusBlock = nullptr;
    return Status;
  }

  // Run the original x87 instruction on binary80 operands. A libm call has
  // different precision, status and processor-specific approximation rules.
  if (const char *Mn = x87AsmForIntrinsic(IC)) {
    // Keep native x87 working precision. Transcendental approximations depend
    // on the processor; this projection does not promise cross-CPU identity.
    auto *F80Ty = llvm::Type::getX86_FP80Ty(*Ctx);
    auto *I80Ty = llvm::Type::getIntNTy(*Ctx, 80);
    auto ToX87 = [&](const MedVar &V) -> llvm::Value * {
      llvm::Value *X = getVar(V, Builder);
      llvm::Type *T = X->getType();
      if (T->isX86_FP80Ty())
        return X;
      if (!T->isIntegerTy(80))
        llvm::report_fatal_error(
            "x87 scalar operand lost its binary80 carrier");
      return Builder.CreateBitCast(X, F80Ty);
    };
    const auto Info = *x87ValueInfo(IC);
    const bool TwoIn = Info.Operands == 2;
    const bool PopsSecondInput = Info.PopsSecond;
    const bool IsRemainder = IC == I::X87Fprem || IC == I::X87Fprem1;
    const bool SamplesStatus = Info.SamplesStatus;
    llvm::Value *In0 = ToX87(Op.Inputs[1]);
    llvm::Value *Res = nullptr;
    if (SamplesStatus) {
      auto *I16Ty = llvm::Type::getInt16Ty(*Ctx);
      const char *PairName = IsRemainder ? "neverd.x87.fprem_result"
                                         : "neverd.x87.value_status_result";
      auto *PairTy = llvm::StructType::getTypeByName(*Ctx, PairName);
      if (!PairTy)
        PairTy = llvm::StructType::create(*Ctx, {F80Ty, I16Ty}, PairName);
      if (PairTy->isOpaque() || PairTy->getNumElements() != 2 ||
          PairTy->getElementType(0) != F80Ty ||
          PairTy->getElementType(1) != I16Ty)
        llvm::report_fatal_error("invalid x87 value/status result structure");
      llvm::SmallVector<llvm::Value *, 2> Inputs{In0};
      llvm::SmallVector<llvm::Type *, 2> Types{F80Ty};
      if (TwoIn) {
        Inputs.push_back(ToX87(Op.Inputs[2]));
        Types.push_back(F80Ty);
      }
      auto *FnTy = llvm::FunctionType::get(PairTy, Types, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, std::string(Mn) + "\n\tfnstsw $1",
          x87ValueStatusConstraints(TwoIn, PopsSecondInput),
          /*hasSideEffects=*/true);
      auto *Pair = Builder.CreateCall(IA, Inputs, "x87_value_status");
      Res = Builder.CreateExtractValue(Pair, {0}, "x87_value");
      PendingX87Status = Builder.CreateExtractValue(Pair, {1}, "x87_status");
      PendingX87StatusBlock = Builder.GetInsertBlock();
    } else if (!TwoIn) {
      auto *FnTy = llvm::FunctionType::get(F80Ty, {F80Ty}, false);
      auto *IA =
          llvm::InlineAsm::get(FnTy, Mn, "=&{st},0,~{dirflag},~{fpsr},~{flags}",
                               /*hasSideEffects=*/true);
      Res = Builder.CreateCall(IA, {In0}, "x87");
    } else {
      llvm::Value *In1 = ToX87(Op.Inputs[2]);
      auto *FnTy = llvm::FunctionType::get(F80Ty, {F80Ty, F80Ty}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mn,
          PopsSecondInput
              ? "=&{st},0,{st(1)},~{st(1)},~{dirflag},~{fpsr},~{flags}"
              : "=&{st},0,{st(1)},~{dirflag},~{fpsr},~{flags}",
          /*hasSideEffects=*/true);
      Res = Builder.CreateCall(IA, {In0, In1}, "x87");
    }
    llvm::Value *Bits = Builder.CreateBitCast(Res, I80Ty);
    auto *OutTy = sizeToType(Op.Output.Size);
    if (OutTy != I80Ty && OutTy->isIntegerTy()) {
      Bits = (OutTy->getIntegerBitWidth() < 80)
                 ? Builder.CreateTrunc(Bits, OutTy)
                 : Builder.CreateZExt(Bits, OutTy);
    }
    return Bits;
  }

  if (IC == I::Rdtsc || IC == I::Rdtscp)
    return emitRdtscValue(Op, IC, Builder);

  // XTEST reports whether execution is transactional.
  if (IC == I::Xtest && Op.Output.Size > 0) {
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::x86_xtest);
    llvm::Value *Value = Builder.CreateCall(Fn, {}, "tsx");
    return Builder.CreateZExtOrTrunc(Value, sizeToType(Op.Output.Size));
  }

  // XBEGIN's status: ~0 when the transaction started, else the abort status.
  // The lifted instruction branches on it to the original fallback, which
  // keeps that continuation and its checkpoint.
  if (IC == I::Xbegin && Op.Output.Size == 4) {
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::x86_xbegin);
    return Builder.CreateCall(Fn, {}, "xbegin");
  }

  if (IC == I::Rdmsr)
    return emitX86ScalarSystem(Op, IC, Builder);

  // A segment selector read.
  if (IC == I::ReadSegment && Op.Output.Size > 0) {
    const char *Segment = Op.NumInputs == 2 && Op.Inputs[1].isConst()
                              ? x86SegmentRegisterName(Op.Inputs[1].ConstVal)
                              : nullptr;
    if (!Segment)
      llvm::report_fatal_error("x86 segment read has an invalid operand shape");
    auto *I16Ty = llvm::Type::getInt16Ty(*Ctx);
    auto *FnTy = llvm::FunctionType::get(I16Ty, {}, false);
    auto *IA =
        llvm::InlineAsm::get(FnTy, std::string("mov %") + Segment + ", $0",
                             "=r", /*hasSideEffects=*/true);
    llvm::Value *Selector = Builder.CreateCall(IA, {}, "selector");
    return Builder.CreateZExtOrTrunc(Selector, sizeToType(Op.Output.Size));
  }

  // RDPMC reads the counter ECX selects into EDX:EAX, combined like RDMSR.
  if (IC == I::Rdpmc && Op.Output.Size > 0) {
    if (Op.NumInputs != 2)
      llvm::report_fatal_error("x86 RDPMC has no counter selector");
    auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
    auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
    llvm::Value *Selector =
        Builder.CreateZExtOrTrunc(getVar(Op.Inputs[1], Builder), I32Ty);
    auto *FnTy = llvm::FunctionType::get(I64Ty, {I32Ty}, false);
    auto *IA = TargetArch == Arch::X64
                   ? llvm::InlineAsm::get(
                         FnTy, "rdpmc\n\tshlq $$32, %rdx\n\torq %rdx, %rax",
                         "={rax},{ecx},~{rdx}", /*hasSideEffects=*/true)
                   : llvm::InlineAsm::get(FnTy, "rdpmc", "=A,{ecx}",
                                          /*hasSideEffects=*/true);
    llvm::Value *Value = Builder.CreateCall(IA, {Selector}, "pmc");
    return Builder.CreateZExtOrTrunc(Value, sizeToType(Op.Output.Size));
  }

  // A Hyper-V hypercall (TLFS): RCX, RDX and R8 in, the status in RAX.
  if ((IC == I::Vmcall || IC == I::Vmmcall) && Op.Output.Size > 0) {
    if (Op.NumInputs != 4)
      llvm::report_fatal_error("x86 hypercall has an invalid operand shape");
    auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
    std::vector<llvm::Value *> Vals;
    for (uint16_t I = 1; I < 4; ++I)
      Vals.push_back(
          Builder.CreateZExtOrTrunc(getVar(Op.Inputs[I], Builder), I64Ty));
    auto *FnTy = llvm::FunctionType::get(I64Ty, {I64Ty, I64Ty, I64Ty}, false);
    auto *IA = llvm::InlineAsm::get(
        FnTy, IC == I::Vmcall ? "vmcall" : "vmmcall",
        "={rax},{rcx},{rdx},{r8},~{memory}", /*hasSideEffects=*/true);
    llvm::Value *Status = Builder.CreateCall(IA, Vals, "hypercall");
    return Builder.CreateZExtOrTrunc(Status, sizeToType(Op.Output.Size));
  }

  // LSL: the segment limit, or its selector check (ZF).
  if ((IC == I::SegmentLimit || IC == I::SegmentLimitValid) &&
      Op.Output.Size > 0) {
    if (Op.NumInputs != 2)
      llvm::report_fatal_error("x86 LSL has an invalid operand shape");
    auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
    auto *I8Ty = llvm::Type::getInt8Ty(*Ctx);
    llvm::Value *Selector =
        Builder.CreateZExtOrTrunc(getVar(Op.Inputs[1], Builder), I32Ty);
    llvm::Value *Result = nullptr;
    if (IC == I::SegmentLimit) {
      auto *FnTy = llvm::FunctionType::get(I32Ty, {I32Ty}, false);
      auto *IA = llvm::InlineAsm::get(FnTy, "lsl ${1:w}, $0", "=r,r,~{flags}",
                                      /*hasSideEffects=*/false);
      Result = Builder.CreateCall(IA, {Selector}, "segment_limit");
    } else {
      auto *RetTy = llvm::StructType::get(*Ctx, {I32Ty, I8Ty});
      auto *FnTy = llvm::FunctionType::get(RetTy, {I32Ty}, false);
      auto *IA = llvm::InlineAsm::get(FnTy, "lsl ${2:w}, $0\n\tsetz $1",
                                      "=&r,=q,r,~{flags}",
                                      /*hasSideEffects=*/false);
      Result = Builder.CreateExtractValue(Builder.CreateCall(IA, {Selector}), 1,
                                          "segment_valid");
    }
    return Builder.CreateZExtOrTrunc(Result, sizeToType(Op.Output.Size));
  }

  // RDRAND/RDSEED: the value with the CF success byte above it.
  if ((IC == I::Rdrand || IC == I::Rdseed) && Op.Output.Size > 0) {
    const unsigned Bytes = Op.Output.Size / 2;
    if (Bytes != 2 && Bytes != 4 && Bytes != 8)
      llvm::report_fatal_error("x86 RDRAND/RDSEED has an invalid result");
    auto *ValueTy = sizeToType(Bytes);
    auto *I8Ty = llvm::Type::getInt8Ty(*Ctx);
    auto *PairTy = sizeToType(Op.Output.Size);
    auto *RetTy = llvm::StructType::get(*Ctx, {ValueTy, I8Ty});
    auto *FnTy = llvm::FunctionType::get(RetTy, {}, false);
    auto *IA = llvm::InlineAsm::get(
        FnTy,
        std::string(IC == I::Rdrand ? "rdrand" : "rdseed") + " $0\n\tsetc $1",
        "=r,=q,~{flags}", /*hasSideEffects=*/true);
    llvm::Value *Pair = Builder.CreateCall(IA, {}, "random");
    llvm::Value *Value =
        Builder.CreateZExt(Builder.CreateExtractValue(Pair, 0), PairTy);
    llvm::Value *Ok =
        Builder.CreateZExt(Builder.CreateExtractValue(Pair, 1), PairTy);
    return Builder.CreateOr(Builder.CreateShl(Ok, Bytes * 8), Value,
                            "random_pair");
  }

  // `int imm8` with a register result, and the x64 debug service (`int 2Dh`),
  // which also reads RAX, RCX, RDX, R8 and R9 in the lifter's input order.
  if ((IC == I::IntN || IC == I::DebugService) && Op.Output.Size > 0) {
    static const char *const DebugServiceRegs[] = {"{rax}", "{rcx}", "{rdx}",
                                                   "{r8}", "{r9}"};
    unsigned Vector = 0x2D;
    uint16_t FirstReg = 1;
    if (IC == I::IntN) {
      if (Op.NumInputs != 2 || !Op.Inputs[1].isConst())
        llvm::report_fatal_error("x86 INT has no immediate vector");
      Vector = Op.Inputs[1].ConstVal & 0xFF;
      FirstReg = 2;
    } else if (TargetArch != Arch::X64 ||
               Op.NumInputs != 1 + std::size(DebugServiceRegs)) {
      llvm::report_fatal_error(
          "x64 debug service has an invalid operand shape");
    }
    auto *OutTy = sizeToType(Op.Output.Size);
    // i386 has no RAX: read EAX and widen it to the lifted output.
    const bool Wide = TargetArch == Arch::X64 && Op.Output.Size == 8;
    auto *AsmTy = Wide ? OutTy : llvm::Type::getInt32Ty(*Ctx);
    std::string Cons = Wide ? "={rax}" : "={eax}";
    std::vector<llvm::Type *> Tys;
    std::vector<llvm::Value *> Vals;
    for (uint16_t I = FirstReg; I < Op.NumInputs; ++I) {
      Vals.push_back(getVar(Op.Inputs[I], Builder));
      Tys.push_back(Vals.back()->getType());
      Cons += std::string(",") + DebugServiceRegs[I - FirstReg];
    }
    Cons += ",~{memory}";
    auto *FnTy = llvm::FunctionType::get(AsmTy, Tys, false);
    auto *IA = llvm::InlineAsm::get(FnTy, "int $$" + std::to_string(Vector),
                                    Cons, /*hasSideEffects=*/true);
    llvm::CallInst *Result = Builder.CreateCall(IA, Vals, "int");
    // The debug service's status is a real return value, not a residue.
    if (IC == I::DebugService)
      llvm_value_provenance::markSemanticProducer(*Result);
    return Builder.CreateZExtOrTrunc(Result, OutTy);
  }

  if (IC == I::Stmxcsr && Op.Output.Size > 0)
    return llvm::ConstantInt::get(sizeToType(Op.Output.Size),
                                  limits::kDefaultMXCSR);

  // PUSHF: the lifter merges the machine's unmodelled EFLAGS bits with the
  // modelled arithmetic flags.
  if (IC == I::Pushf && Op.Output.Size > 0) {
    const bool Wide = TargetArch == Arch::X64;
    auto *AsmTy =
        Wide ? llvm::Type::getInt64Ty(*Ctx) : llvm::Type::getInt32Ty(*Ctx);
    auto *FnTy = llvm::FunctionType::get(AsmTy, false);
    auto *IA = llvm::InlineAsm::get(
        FnTy, Wide ? "pushfq\n\tpopq $0" : "pushfl\n\tpopl $0", "=r,~{memory}",
        /*hasSideEffects=*/true);
    llvm::Value *Flags = Builder.CreateCall(IA, {}, "eflags");
    markX86StackTemporary(*llvm::cast<llvm::CallInst>(Flags));
    return Builder.CreateZExtOrTrunc(Flags, sizeToType(Op.Output.Size));
  }

  if (IC == I::Ldmxcsr || IC == I::Leave || IC == I::Enter || IC == I::Popf)
    return (Op.Output.Size > 0)
               ? llvm::ConstantInt::get(sizeToType(Op.Output.Size), 0)
               : nullptr;

  if (IC == I::In)
    return emitX86ScalarSystem(Op, IC, Builder);

  if (x86SystemRegisterName(IC, 0)) {
    const auto P = x86SystemRegisterAsm(
        IC, TargetArch,
        Op.NumInputs > 1 && Op.Inputs[1].isConst() ? Op.Inputs[1].ConstVal
                                                   : UINT64_MAX);
    if (!P || P->Write || Op.Output.Size != P->Bytes ||
        Op.NumInputs != (P->Debug ? 2 : 1) ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      llvm::report_fatal_error("invalid x86 system register read contract");
    if (const auto IID = llvm::Intrinsic::lookupIntrinsicID(P->LLVMName))
      return Builder.CreateCall(
          llvm::Intrinsic::getOrInsertDeclaration(Mod, IID),
          {Builder.getInt32(P->Index)}, "sysreg");
    auto *FnTy = llvm::FunctionType::get(sizeToType(Op.Output.Size), false);
    auto *IA = llvm::InlineAsm::get(FnTy, P->Text, P->Constraints,
                                    /*hasSideEffects=*/true);
    return Builder.CreateCall(IA, {}, "sysreg");
  }

  // SLDT/STR/SMSW with a register destination (no memory operand captured).

  if (IC == I::Cpuid)
    return emitCpuidValue(Op, Builder);

  if (IC == I::Xgetbv)
    return emitXgetbvValue(Op, Builder);

  if (IC == I::Movsb || IC == I::Movsw || IC == I::Movsd || IC == I::Movsq ||
      IC == I::Stosb || IC == I::Stosw || IC == I::Stosd || IC == I::Stosq ||
      IC == I::Lodsb || IC == I::Lodsw || IC == I::Lodsd || IC == I::Lodsq ||
      IC == I::Cmpsb || IC == I::Cmpsw || IC == I::Cmpsd_str ||
      IC == I::Cmpsq || IC == I::Scasb || IC == I::Scasw || IC == I::Scasd ||
      IC == I::Scasq)
    return emitRepString(Op, IC, Builder);

  // Per-lane saturating add/sub: map to @llvm.{s,u}{add,sub}.sat intrinsics.
  {
    llvm::Intrinsic::ID IID = llvm::Intrinsic::not_intrinsic;
    switch (IC) {
    case I::X86_SaddSat:
      IID = llvm::Intrinsic::sadd_sat;
      break;
    case I::X86_UaddSat:
      IID = llvm::Intrinsic::uadd_sat;
      break;
    case I::X86_SsubSat:
      IID = llvm::Intrinsic::ssub_sat;
      break;
    case I::X86_UsubSat:
      IID = llvm::Intrinsic::usub_sat;
      break;
    case I::X86_Smin:
      IID = llvm::Intrinsic::smin;
      break;
    case I::X86_Smax:
      IID = llvm::Intrinsic::smax;
      break;
    case I::X86_Umin:
      IID = llvm::Intrinsic::umin;
      break;
    case I::X86_Umax:
      IID = llvm::Intrinsic::umax;
      break;
    default:
      break;
    }
    if (IID != llvm::Intrinsic::not_intrinsic && Op.Output.Size > 0 &&
        Op.NumInputs >= 3) {
      auto *OutTy = sizeToType(Op.Output.Size);
      auto *Ty = llvm::IntegerType::get(*Ctx, Op.Output.Size * 8);
      auto coerce = [&](const MedVar &In) -> llvm::Value * {
        llvm::Value *V = getVar(In, Builder);
        if (V->getType()->isPointerTy())
          V = Builder.CreatePtrToInt(V, Ty);
        return Builder.CreateZExtOrTrunc(V, Ty);
      };
      auto *A = coerce(Op.Inputs[1]);
      auto *B = coerce(Op.Inputs[2]);
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID, {Ty});
      llvm::Value *R = Builder.CreateCall(Fn, {A, B});
      return (R->getType() == OutTy) ? R : Builder.CreateZExtOrTrunc(R, OutTy);
    }
  }

  return nullptr;
}

//===----------------------------------------------------------------------===//
// Wide division and remainder (x86-64 i128 DIV/IDIV, i386 i64 DIV/IDIV)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitX86WideDivRem(llvm::IRBuilder<> &Builder,
                                               llvm::Value *Dividend,
                                               llvm::Value *Divisor,
                                               bool IsSigned, bool WantRem) {
  // A double-width DIV/IDIV (the hardware quotient/remainder split) is lowered
  // back to a single native divide via inline asm, so the recompiled object
  // never pulls in a compiler-rt division libcall that the original binary did
  // not have:
  //   * x86-64 : i128 dividend (RDX:RAX) -> divq/idivq
  //   * i386   : i64  dividend (EDX:EAX) -> divl/idivl
  // i64 udiv is already a native `divq` on x86-64, so that case is
  // intentionally left to the generic emitter (return null) there — only i386
  // needs the i64 form (where i64 udiv would otherwise become
  // __udivdi3/__umoddi3).
  unsigned WideBits = 0;
  if (TargetArch == Arch::X64 && Dividend->getType()->isIntegerTy(128))
    WideBits = 128;
  else if (TargetArch == Arch::X86 && Dividend->getType()->isIntegerTy(64))
    WideBits = 64;
  else
    return nullptr;

  unsigned HalfBits = WideBits / 2;
  auto *HalfTy = llvm::IntegerType::get(*Ctx, HalfBits);
  auto *WideTy = llvm::IntegerType::get(*Ctx, WideBits);

  llvm::Value *Lo = Builder.CreateTrunc(Dividend, HalfTy, "div.lo");
  llvm::Value *HiShifted = Builder.CreateLShr(
      Dividend, llvm::ConstantInt::get(WideTy, HalfBits), "div.hi.sh");
  llvm::Value *Hi = Builder.CreateTrunc(HiShifted, HalfTy, "div.hi");
  llvm::Value *DivHalf = Builder.CreateTrunc(Divisor, HalfTy, "div.src");

  auto *RetTy = llvm::StructType::get(*Ctx, {HalfTy, HalfTy});
  auto *FnTy = llvm::FunctionType::get(RetTy, {HalfTy, HalfTy, HalfTy}, false);
  auto *IA = llvm::InlineAsm::get(FnTy, x86DivRemAssembly({HalfBits, IsSigned}),
                                  X86DivRemAsmConstraints,
                                  /*hasSideEffects=*/true);

  llvm::Value *AsmResult = Builder.CreateCall(IA, {Lo, Hi, DivHalf}, "divrem");
  unsigned Idx = WantRem ? 1 : 0;
  llvm::Value *RHalf = Builder.CreateExtractValue(AsmResult, {Idx}, "divrem.r");

  if (IsSigned)
    return Builder.CreateSExt(RHalf, WideTy, "divrem.ext");
  return Builder.CreateZExt(RHalf, WideTy, "divrem.ext");
}

} // namespace neverd
