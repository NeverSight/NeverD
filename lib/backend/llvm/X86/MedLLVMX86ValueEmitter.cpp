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
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"

#define DEBUG_TYPE "neverd-med-llvm-x86-value"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicsX86.h"

#include <cassert>
#include <map>

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
  auto *AsmFnTy =
      llvm::FunctionType::get(StructTy, {I32Ty, I32Ty}, false);
  auto *IA = llvm::InlineAsm::get(
      AsmFnTy, "cpuid",
      "={eax},={ebx},={ecx},={edx},{eax},{ecx},~{memory}", true);
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
  case I::Cmpsb:
  case I::Scasb:
    ElemSz = 1;
    Suffix = 'b';
    break;
  case I::Movsw:
  case I::Stosw:
  case I::Cmpsw:
  case I::Scasw:
    ElemSz = 2;
    Suffix = 'w';
    break;
  case I::Movsd:
  case I::Stosd:
  case I::Cmpsd_str:
  case I::Scasd:
    ElemSz = 4;
    Suffix = 'l';
    break;
  case I::Movsq:
  case I::Stosq:
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
  bool IsCmps = (IC == I::Cmpsb || IC == I::Cmpsw || IC == I::Cmpsd_str ||
                 IC == I::Cmpsq);
  bool IsScas =
      (IC == I::Scasb || IC == I::Scasw || IC == I::Scasd || IC == I::Scasq);

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
  // (index 4 for both STOS and MOVS; absent on legacy stubs => forward).  A
  // known constant bakes the direction in with no runtime branch; a dynamic DF
  // is tested before the rep.  Either way a trailing `cld` restores the SysV
  // forward invariant.  The hardware `rep` honours the live DF, so this is the
  // recompiled counterpart of `std; rep ...` over overlapping/backward buffers.
  enum DfKind { DfFwd, DfBwd, DfDyn };
  DfKind DfK = DfFwd;
  if (Op.NumInputs > 4) {
    const auto &DfV = Op.Inputs[4];
    DfK = DfV.isConst() ? (DfV.ConstVal != 0 ? DfBwd : DfFwd) : DfDyn;
  }
  // Operand index of the dynamic DF GPR in the inline-asm template: STOS has
  // outputs di/cx (%0,%1) + tied/{ax} inputs %2..%4, MOVS has si/di/cx (%0..%2)
  // + tied inputs %3..%5, so the appended `r` DF lands at %5 / %6 respectively.
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
  if (IsStos && Op.NumInputs >= 4) {
    auto *RDI = Coerce(getVar(Op.Inputs[1], Builder), AddrRegTy);
    auto *RCX = Coerce(getVar(Op.Inputs[2], Builder), AddrRegTy);
    auto *ValTy = llvm::Type::getIntNTy(*Ctx, ElemSz * 8);
    auto *Val = Coerce(getVar(Op.Inputs[3], Builder), ValTy);
    std::string Mn = dirWrap(std::string("rep stos") + Suffix, 5);
    auto *RetTy = llvm::StructType::get(*Ctx, {AddrRegTy, AddrRegTy});
    if (DfK == DfDyn) {
      auto *Df = Coerce(getVar(Op.Inputs[4], Builder), AddrRegTy);
      auto *FnTy = llvm::FunctionType::get(
          RetTy, {AddrRegTy, AddrRegTy, ValTy, AddrRegTy}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mn, "={di},={cx},0,1,{ax},r,~{memory},~{dirflag},~{cc}", true);
      Builder.CreateCall(IA, {RDI, RCX, Val, Df});
    } else {
      auto *FnTy =
          llvm::FunctionType::get(RetTy, {AddrRegTy, AddrRegTy, ValTy}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mn, "={di},={cx},0,1,{ax},~{memory},~{dirflag},~{cc}", true);
      Builder.CreateCall(IA, {RDI, RCX, Val});
    }
    return Handled();
  }

  // CMPS / SCAS: unlike MOVS/STOS these stop on a data-dependent ZF transition,
  // so the trip count is only known at run time.  The hardware's leftover RCX
  // is the intrinsic's result and the lifter derives the advanced pointers from
  // it (see liftRepCmpScas); the pointer outputs are still declared so the
  // register allocator models the clobber.  REPE (`repz`) repeats while the
  // elements match, REPNE (`repnz`) while they differ; the lifter passes which
  // one as the trailing constant input.
  if ((IsCmps || IsScas) && Op.NumInputs >= 5) {
    const bool RepNE = Op.NumInputs > 5 && Op.Inputs[5].isConst() &&
                       Op.Inputs[5].ConstVal != 0;
    const std::string RepPfx = RepNE ? "repnz " : "repz ";
    llvm::Value *LeftCount = nullptr;
    if (IsScas) {
      // Inputs [RDI, RCX, AL/AX/EAX/RAX]: RDI/RCX are tied in-out, the
      // accumulator is a plain {ax} input (SCAS never writes it).
      auto *RDI = Coerce(getVar(Op.Inputs[1], Builder), AddrRegTy);
      auto *RCX = Coerce(getVar(Op.Inputs[2], Builder), AddrRegTy);
      auto *ValTy = llvm::Type::getIntNTy(*Ctx, ElemSz * 8);
      auto *Val = Coerce(getVar(Op.Inputs[3], Builder), ValTy);
      std::string Mn = dirWrap(RepPfx + "scas" + Suffix, 5);
      auto *RetTy = llvm::StructType::get(*Ctx, {AddrRegTy, AddrRegTy});
      llvm::CallInst *Call = nullptr;
      if (DfK == DfDyn) {
        auto *Df = Coerce(getVar(Op.Inputs[4], Builder), AddrRegTy);
        auto *FnTy = llvm::FunctionType::get(
            RetTy, {AddrRegTy, AddrRegTy, ValTy, AddrRegTy}, false);
        auto *IA = llvm::InlineAsm::get(
            FnTy, Mn, "={di},={cx},0,1,{ax},r,~{memory},~{dirflag},~{cc}",
            true);
        Call = Builder.CreateCall(IA, {RDI, RCX, Val, Df}, "rep_scas");
      } else {
        auto *FnTy = llvm::FunctionType::get(
            RetTy, {AddrRegTy, AddrRegTy, ValTy}, false);
        auto *IA = llvm::InlineAsm::get(
            FnTy, Mn, "={di},={cx},0,1,{ax},~{memory},~{dirflag},~{cc}", true);
        Call = Builder.CreateCall(IA, {RDI, RCX, Val}, "rep_scas");
      }
      LeftCount = Builder.CreateExtractValue(Call, {1}, "scas_cx");
    } else {
      // Inputs [RSI, RDI, RCX], all three tied in-out.
      auto *RSI = Coerce(getVar(Op.Inputs[1], Builder), AddrRegTy);
      auto *RDI = Coerce(getVar(Op.Inputs[2], Builder), AddrRegTy);
      auto *RCX = Coerce(getVar(Op.Inputs[3], Builder), AddrRegTy);
      std::string Mn = dirWrap(RepPfx + "cmps" + Suffix, 6);
      auto *RetTy =
          llvm::StructType::get(*Ctx, {AddrRegTy, AddrRegTy, AddrRegTy});
      llvm::CallInst *Call = nullptr;
      if (DfK == DfDyn) {
        auto *Df = Coerce(getVar(Op.Inputs[4], Builder), AddrRegTy);
        auto *FnTy = llvm::FunctionType::get(
            RetTy, {AddrRegTy, AddrRegTy, AddrRegTy, AddrRegTy}, false);
        auto *IA = llvm::InlineAsm::get(
            FnTy, Mn, "={si},={di},={cx},0,1,2,r,~{memory},~{dirflag},~{cc}",
            true);
        Call = Builder.CreateCall(IA, {RSI, RDI, RCX, Df}, "rep_cmps");
      } else {
        auto *FnTy = llvm::FunctionType::get(
            RetTy, {AddrRegTy, AddrRegTy, AddrRegTy}, false);
        auto *IA = llvm::InlineAsm::get(
            FnTy, Mn, "={si},={di},={cx},0,1,2,~{memory},~{dirflag},~{cc}",
            true);
        Call = Builder.CreateCall(IA, {RSI, RDI, RCX}, "rep_cmps");
      }
      LeftCount = Builder.CreateExtractValue(Call, {2}, "cmps_cx");
    }
    if (Op.Output.Size == 0)
      return nullptr;
    return Coerce(LeftCount, sizeToType(Op.Output.Size));
  }

  // MOVS: inputs [RSI, RDI, RCX], all written by the hardware (discarded).
  if (IsMovs && Op.NumInputs >= 4) {
    auto *RSI = Coerce(getVar(Op.Inputs[1], Builder), AddrRegTy);
    auto *RDI = Coerce(getVar(Op.Inputs[2], Builder), AddrRegTy);
    auto *RCX = Coerce(getVar(Op.Inputs[3], Builder), AddrRegTy);
    std::string Mn = dirWrap(std::string("rep movs") + Suffix, 6);
    auto *RetTy =
        llvm::StructType::get(*Ctx, {AddrRegTy, AddrRegTy, AddrRegTy});
    if (DfK == DfDyn) {
      auto *Df = Coerce(getVar(Op.Inputs[4], Builder), AddrRegTy);
      auto *FnTy = llvm::FunctionType::get(
          RetTy, {AddrRegTy, AddrRegTy, AddrRegTy, AddrRegTy}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mn, "={si},={di},={cx},0,1,2,r,~{memory},~{dirflag},~{cc}",
          true);
      Builder.CreateCall(IA, {RSI, RDI, RCX, Df});
    } else {
      auto *FnTy = llvm::FunctionType::get(
          RetTy, {AddrRegTy, AddrRegTy, AddrRegTy}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mn, "={si},={di},={cx},0,1,2,~{memory},~{dirflag},~{cc}", true);
      Builder.CreateCall(IA, {RSI, RDI, RCX});
    }
    return Handled();
  }

  // Fallback for a stub that did not capture the full register inputs: emit the
  // bare rep with whatever inputs were provided.
  static const std::map<Intrinsic, const char *> RepAsmTable = {
      {I::Movsb, "rep movsb"}, {I::Movsw, "rep movsw"}, {I::Movsd, "rep movsl"},
      {I::Movsq, "rep movsq"}, {I::Stosb, "rep stosb"}, {I::Stosw, "rep stosw"},
      {I::Stosd, "rep stosl"}, {I::Stosq, "rep stosq"},
  };
  auto It = RepAsmTable.find(IC);
  if (It == RepAsmTable.end())
    return nullptr;
  std::vector<llvm::Type *> ParamTys;
  std::vector<llvm::Value *> ParamVals;
  for (uint16_t Idx = 1; Idx < Op.NumInputs; ++Idx) {
    auto *V = getVar(Op.Inputs[Idx], Builder);
    ParamTys.push_back(V->getType());
    ParamVals.push_back(V);
  }
  std::string Cstr;
  for (size_t Idx = 0; Idx < ParamVals.size(); ++Idx)
    Cstr += (Cstr.empty() ? "r" : ",r");
  Cstr += (Cstr.empty() ? "~{memory},~{dirflag}" : ",~{memory},~{dirflag}");
  auto *AsmFnTy = llvm::FunctionType::get(VoidTy, ParamTys, false);
  auto *IA = llvm::InlineAsm::get(AsmFnTy, It->second, Cstr, true);
  Builder.CreateCall(IA, ParamVals);
  return Handled();
}

//===----------------------------------------------------------------------===//
// Top-level x86 value-producing INTRINSIC dispatch
//===----------------------------------------------------------------------===//

// Maps an x87 value-producing intrinsic to the inline-asm text that computes it
// and leaves the single result on the x87 stack top.  FPTAN/FXTRACT pop the
// extra value they would otherwise push so each call yields one scalar (the
// lifter re-creates the stack push).  Returns nullptr for non-x87 intrinsics.
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
  case Intrinsic::X87Fptan:
    return "fptan\n\tfstp %st(0)";
  case Intrinsic::X87Fxtractsig:
    return "fxtract\n\tfstp %st(1)";
  case Intrinsic::X87Fxtractexp:
    return "fxtract\n\tfstp %st(0)";
  default:
    return nullptr;
  }
}

llvm::Value *MedLLVMEmitter::emitX86IntrinsicValue(const MedOp &Op,
                                                   Intrinsic IC,
                                                   llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;

  // FPREM/FPREM1 expose partial-reduction progress and quotient bits through
  // the x87 status word.  Capture it immediately after the value-producing
  // inline asm so a lifted FNSTSW observes the same C0/C1/C2/C3 state.
  if (IC == I::X87ReadStatus) {
    if (Op.Output.Size == 0)
      return nullptr;
    auto *I16Ty = llvm::Type::getInt16Ty(*Ctx);
    auto *FnTy = llvm::FunctionType::get(I16Ty, {}, false);
    auto *IA = llvm::InlineAsm::get(
        FnTy, "fnstsw $0", "={ax},~{dirflag},~{fpsr},~{flags}",
        /*hasSideEffects=*/true);
    llvm::Value *Status = Builder.CreateCall(IA, {}, "x87_status");
    auto *OutTy = sizeToType(Op.Output.Size);
    if (OutTy != I16Ty)
      Status = Builder.CreateZExtOrTrunc(Status, OutTy);
    return Status;
  }

  // x87 transcendental / special ops: emit the genuine x87 instruction via
  // inline asm so the recompiled object runs the same hardware op as the
  // original (Unicorn executes it natively).  ST operands flow as doubles; the
  // backend reloads/stores them around the asm.  Lowering to libm would not
  // roundtrip and would call functions absent under Unicorn.
  if (const char *Mn = x87AsmForIntrinsic(IC)) {
    if (Op.Output.Size == 0 || Op.NumInputs < 2)
      return nullptr;
    // The x87 stack registers are modeled at 80-bit extended precision, so the
    // transcendental runs on x86_fp80 to match hardware (and the original under
    // Unicorn) bit-for-bit instead of truncating to a 64-bit double.
    auto *F80Ty = llvm::Type::getX86_FP80Ty(*Ctx);
    auto *I80Ty = llvm::Type::getIntNTy(*Ctx, 80);
    auto ToX87 = [&](const MedVar &V) -> llvm::Value * {
      llvm::Value *X = getVar(V, Builder);
      llvm::Type *T = X->getType();
      if (T->isX86_FP80Ty())
        return X;
      if (T->isFloatingPointTy())
        return Builder.CreateFPExt(X, F80Ty, "x87in");
      unsigned Bits = T->getIntegerBitWidth();
      if (Bits == 80)
        return Builder.CreateBitCast(X, F80Ty);
      // Narrower register bits hold a float/double pattern: reinterpret at that
      // width, then widen to the 80-bit working precision.
      llvm::Type *SrcF = (Bits <= 32) ? llvm::Type::getFloatTy(*Ctx)
                                      : llvm::Type::getDoubleTy(*Ctx);
      unsigned FBits = SrcF->getPrimitiveSizeInBits();
      if (Bits > FBits)
        X = Builder.CreateTrunc(X, llvm::Type::getIntNTy(*Ctx, FBits));
      else if (Bits < FBits)
        X = Builder.CreateZExt(X, llvm::Type::getIntNTy(*Ctx, FBits));
      return Builder.CreateFPExt(Builder.CreateBitCast(X, SrcF), F80Ty,
                                 "x87in");
    };
    bool TwoIn =
        (IC != I::X87Fsin && IC != I::X87Fcos && IC != I::X87F2xm1 &&
         IC != I::X87Fptan && IC != I::X87Fxtractsig && IC != I::X87Fxtractexp);
    llvm::Value *In0 = ToX87(Op.Inputs[1]);
    llvm::Value *Res = nullptr;
    if (!TwoIn) {
      auto *FnTy = llvm::FunctionType::get(F80Ty, {F80Ty}, false);
      auto *IA =
          llvm::InlineAsm::get(FnTy, Mn, "=&{st},0,~{dirflag},~{fpsr},~{flags}",
                               /*hasSideEffects=*/true);
      Res = Builder.CreateCall(IA, {In0}, "x87");
    } else {
      llvm::Value *In1 = (Op.NumInputs >= 3) ? ToX87(Op.Inputs[2]) : In0;
      auto *FnTy = llvm::FunctionType::get(F80Ty, {F80Ty, F80Ty}, false);
      auto *IA = llvm::InlineAsm::get(
          FnTy, Mn, "=&{st},0,{st(1)},~{dirflag},~{fpsr},~{flags}",
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

  if (IC == I::Stmxcsr && Op.Output.Size > 0)
    return llvm::ConstantInt::get(sizeToType(Op.Output.Size),
                                  limits::kDefaultMXCSR);

  if (IC == I::Ldmxcsr || IC == I::Leave || IC == I::Enter || IC == I::Pushf ||
      IC == I::Popf)
    return (Op.Output.Size > 0)
               ? llvm::ConstantInt::get(sizeToType(Op.Output.Size), 0)
               : nullptr;

  if (IC == I::In)
    return emitX86PortIn(Op, Builder);

  // SLDT/STR/SMSW with a register destination (no memory operand captured).
  if ((IC == I::Sldt || IC == I::Str || IC == I::Smsw) && Op.NumInputs <= 1)
    return emitX86SysRegStore(Op, IC, Builder);

  if (IC == I::Cpuid)
    return emitCpuidValue(Op, Builder);

  if (IC == I::Xgetbv)
    return emitXgetbvValue(Op, Builder);

  if (IC == I::Movsb || IC == I::Movsw || IC == I::Movsd || IC == I::Movsq ||
      IC == I::Stosb || IC == I::Stosw || IC == I::Stosd || IC == I::Stosq ||
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

  const char *AsmStr = (WideBits == 128) ? (IsSigned ? "idivq $4" : "divq $4")
                                         : (IsSigned ? "idivl $4" : "divl $4");
  auto *RetTy = llvm::StructType::get(*Ctx, {HalfTy, HalfTy});
  auto *FnTy = llvm::FunctionType::get(RetTy, {HalfTy, HalfTy, HalfTy}, false);
  auto *IA =
      llvm::InlineAsm::get(FnTy, AsmStr, "={ax},={dx},{ax},{dx},r,~{flags}",
                           /*hasSideEffects=*/true);

  llvm::Value *AsmResult = Builder.CreateCall(IA, {Lo, Hi, DivHalf}, "divrem");
  unsigned Idx = WantRem ? 1 : 0;
  llvm::Value *RHalf = Builder.CreateExtractValue(AsmResult, {Idx}, "divrem.r");

  if (IsSigned)
    return Builder.CreateSExt(RHalf, WideTy, "divrem.ext");
  return Builder.CreateZExt(RHalf, WideTy, "divrem.ext");
}

} // namespace neverd
