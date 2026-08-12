//===- MedLLVMCall.cpp - CALL/INDIR_CALL and RETURN lowering ------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Lowering for the two largest MedOp cases carved out of the emitOp opcode
/// switch in MedLLVMOpEmitter.cpp: the direct/indirect CALL path (libc/libm
/// signature reconstruction, argument coercion, and small-struct-return
/// flattening) and the RETURN path (return-register recovery, 32-bit register-
/// pair splicing, and by-value struct assembly).  Housing them here mirrors how
/// INTRINSIC lives in MedLLVMIntrinsic.cpp and keeps emitOp a readable
/// dispatcher.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/libc/LibCNames.h"

#define DEBUG_TYPE "neverd-med-llvm-call"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"

#include <set>
#include <string>
#include <vector>

namespace neverd {

//===----------------------------------------------------------------------===//
// CALL / INDIR_CALL -- direct and indirect call lowering
//===----------------------------------------------------------------------===//

void MedLLVMEmitter::defineCallClobbers(const MedOp &Op,
                                        llvm::IRBuilder<> &Builder) {
  if (!CurMedFunc || Op.CallSiteId == 0 || Op.PreservesCallerSaved)
    return;

  for (const MedCallClobber &Clobber : CurMedFunc->CallClobbers) {
    if (Clobber.CallSiteId != Op.CallSiteId || Clobber.Value.Size == 0)
      continue;
    auto *ClobberTy =
        llvm::cast<llvm::IntegerType>(sizeToType(Clobber.Value.Size));
    auto *Unknown = llvm::UndefValue::get(ClobberTy);
    llvm::Value *Frozen = Builder.CreateFreeze(
        Unknown, Clobber.Value.display() + "_call_clobber_unknown");
    if (Clobber.PreservedPrefixSize > 0) {
      llvm::Value *Preserved = getVar(Clobber.PreservedInput, Builder);
      if (Preserved->getType()->isPointerTy())
        Preserved = Builder.CreatePtrToInt(Preserved, ClobberTy);
      else if (Preserved->getType() != ClobberTy)
        Preserved = Builder.CreateZExtOrTrunc(Preserved, ClobberTy);

      unsigned Width = ClobberTy->getBitWidth();
      unsigned PrefixWidth = Clobber.PreservedPrefixSize * 8u;
      llvm::APInt LowMask = llvm::APInt::getLowBitsSet(Width, PrefixWidth);
      auto *LowMaskValue = llvm::ConstantInt::get(ClobberTy, LowMask);
      auto *HighMaskValue = llvm::ConstantInt::get(ClobberTy, ~LowMask);
      llvm::Value *KnownLow = Builder.CreateAnd(Preserved, LowMaskValue);
      llvm::Value *UnknownHigh = Builder.CreateAnd(Frozen, HighMaskValue);
      Frozen = Builder.CreateOr(UnknownHigh, KnownLow,
                                Clobber.Value.display() + "_call_clobber");
    }
    setVar(Clobber.Value, Frozen, Builder);
  }
}

void MedLLVMEmitter::emitCallOp(const MedOp &Op, llvm::IRBuilder<> &Builder,
                                int BlockId, int OpIdx) {
  auto GetInput = [&](uint8_t Idx) -> llvm::Value * {
    if (Idx >= Op.NumInputs)
      return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), 0);
    return getVar(Op.Inputs[Idx], Builder);
  };
  llvm::Value *Result = nullptr;
  // Elide a Darwin stack-probe call (____chkstk_darwin).  It is invoked
  // GOT-indirect with its allocation size in a fixed register (x9) the lifter
  // does not model, so re-emitting it with the recovered argument registers
  // makes the probe walk off the stack and fault.  The probe is pure (it only
  // touches guard pages) and the dynamic allocation it guards is
  // independently lowered to a real alloca, so dropping it preserves
  // semantics.  chkstk leaves the result register untouched and clang never
  // reads it, so a defined-but-unused zero keeps any (dead) downstream use
  // well-typed.
  if (isStackProbeCall(Op)) {
    if (Op.Output.Size > 0)
      setVar(Op.Output,
             llvm::ConstantInt::get(
                 llvm::Type::getIntNTy(*Ctx, Op.Output.Size * 8u), 0),
             Builder);
    return;
  }
  auto *Target = GetInput(0);
  uint64_t CallAddr = Op.Inputs[0].isConst() ? Op.Inputs[0].ConstVal : 0;

  const MedCallInfo *CI =
      CurMedFunc ? CurMedFunc->findCall(BlockId, OpIdx) : nullptr;

  std::vector<llvm::Value *> Args;
  if (CI) {
    for (auto &Arg : CI->Args)
      Args.push_back(getVar(Arg, Builder));
  }

  auto *RetTy = llvm::Type::getInt64Ty(*Ctx);
  auto *I8Ty = llvm::Type::getInt8Ty(*Ctx);
  auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
  auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
  auto *PtrTy = llvm::PointerType::getUnqual(*Ctx);

  std::vector<llvm::Type *> ArgTypes;
  for (auto *A : Args)
    ArgTypes.push_back(A->getType());

  auto resolveCalleeName = [&]() -> std::string {
    if (CI && !CI->TargetName.empty() &&
        !CI->TargetName.starts_with(kAutoFuncPrefix))
      return CI->TargetName;
    if (CallAddr != 0) {
      auto FnIt = FuncNames.find(CallAddr);
      if (FnIt != FuncNames.end())
        return FnIt->second;
    }
    return {};
  };

  if (Op.Opcode == NdOp::CALL && CI && Args.size() >= 3) {
    std::string LibNameStr = resolveCalleeName();
    llvm::StringRef LibName = stripLeadingUnderscores(LibNameStr);
    if (libc::isMemSetName(LibName)) {
      llvm::Value *Dest = Args[0];
      if (Dest->getType()->isIntegerTy()) {
        llvm::Value *Sym = (CI && !CI->Args.empty())
                               ? tryResolvePointerArg(CI->Args[0], Builder)
                               : nullptr;
        Dest = Sym ? Sym : Builder.CreateIntToPtr(Dest, PtrTy);
      }
      llvm::Value *Val = Args[1];
      if (!Val->getType()->isIntegerTy(8))
        Val = Builder.CreateTrunc(Val, I8Ty);
      llvm::Value *Len = Args[2];
      if (Len->getType()->getIntegerBitWidth() < 64)
        Len = Builder.CreateZExt(Len, I64Ty);
      Builder.CreateMemSet(Dest, Val, Len, llvm::MaybeAlign(1));
      if (Op.Output.Size > 0)
        setVar(Op.Output, Dest, Builder);
      defineCallClobbers(Op, Builder);
      return;
    }
  }

  llvm::Function *Callee = nullptr;
  if (Op.Opcode == NdOp::CALL) {
    std::string CalleeName = resolveCalleeName();
    if (!CalleeName.empty() && CallAddr == 0)
      CallAddr = 1; // force direct-call resolution below
    if (CallAddr != 0) {

      // On AArch64, variadic and non-variadic functions use different
      // calling conventions (variadic args go on the stack).  We detect
      // known variadic functions by pattern/whitelist, and for any
      // remaining unknown external function we conservatively declare
      // it as variadic (all actual args as fixed + "...") so the
      // backend never puts arguments in the wrong location.
      unsigned NumFixed = 0;
      if (!CalleeName.empty()) {
        llvm::StringRef Bare = stripLeadingUnderscores(CalleeName);
        NumFixed = libc::varArgFixedCount(Bare);
      }

      bool IsKnownVarArg = (NumFixed > 0);

      if (IsKnownVarArg && !CalleeName.empty()) {
        Callee = Mod->getFunction(CalleeName);
        if (!Callee) {
          std::vector<llvm::Type *> FixedTypes;
          for (unsigned I = 0; I < NumFixed && I < Args.size(); ++I)
            FixedTypes.push_back(PtrTy);
          auto *VarFT = llvm::FunctionType::get(I32Ty, FixedTypes, true);
          Callee = llvm::Function::Create(
              VarFT, llvm::GlobalValue::ExternalLinkage, CalleeName, Mod);
          Callee->setCallingConv(llvm::CallingConv::C);
        }
        for (unsigned I = 0; I < NumFixed && I < Args.size(); ++I) {
          if (Args[I]->getType()->isIntegerTy()) {
            llvm::Value *Sym = (CI && I < CI->Args.size())
                                   ? tryResolvePointerArg(CI->Args[I], Builder)
                                   : nullptr;
            Args[I] = Sym ? Sym : Builder.CreateIntToPtr(Args[I], PtrTy);
          }
        }
      }

      if (!Callee && !CalleeName.empty())
        Callee = Mod->getFunction(CalleeName);

      // A known non-variadic libc function with floating-point arguments (the
      // math.h family sqrt/pow/sin/...) must be declared with FP
      // `double`/`float` parameters and return so its arguments travel in the
      // FP registers (d0-d7 / s0-s7) and the result is read from d0/s0.  The
      // recovered arguments are the FP register bit pattern tracked as an
      // integer/vector (i128); the per-argument coercion below bridges them
      // to the FP type, and setVar bitcasts the FP result back to the
      // integer-tracked return register.  Declared only for the all-FP shape
      // (IntArgs==0 && FpArgs>0) we model; mixed int/FP forms fall to the
      // conservative variadic fallback. Without this an external `pow`/`sin`
      // is declared `(i128,...)->i64`, so the double args go in
      // integer/vector registers (pow reads garbage d0/d1) and the double
      // result is read from x0 -- patched math returns wrong values.
      if (!Callee && !CalleeName.empty()) {
        llvm::StringRef Bare = stripLeadingUnderscores(CalleeName);
        auto Arity = libc::libcArity(Bare);
        // A known libc function whose RESULT is floating-point.  Three shapes
        // are modelled: (a) all-FP-args (sqrt/pow), (b) FP-first mixed int/FP
        // (ldexp/frexp), and (c) integer/pointer args with an FP return
        // (atof/strtod/difftime/nan -- FpRet).  long double is only ABI-equal
        // to double on Apple AArch64, so FpRetLongDouble (strtold) is
        // honoured only there; elsewhere it falls to the conservative
        // fallback.  (d) a
        // `_Complex` result (csqrt/cexp/cpow -- FpRetComplex): a 2-element FP
        // aggregate returned in two FP registers, declared as a `{fp,fp}`
        // struct return so the struct-return flatten below packs both result
        // registers.
        bool LongDoubleAsDouble = Arity && Arity->FpRetLongDouble && Img &&
                                  Img->isMachO() && TargetArch == Arch::AArch64;
        bool ReturnsFp = Arity && (Arity->FpArgs > 0 || Arity->FpRet ||
                                   LongDoubleAsDouble || Arity->FpRetComplex);
        bool ArgsModelled = Arity && (Arity->FpArgs == 0 ||
                                      Arity->IntArgs == 0 || Arity->FpFirst);
        if (Arity && ReturnsFp && ArgsModelled) {
          auto *FpTy = Arity->FpIsFloat ? llvm::Type::getFloatTy(*Ctx)
                                        : llvm::Type::getDoubleTy(*Ctx);
          const int NFp = Arity->FpArgs, NInt = Arity->IntArgs;
          // A `_Complex` return is two FP registers.  When the lifter
          // recovered the call as a multi-register aggregate
          // (modelCallStructReturn turned the output into a flat temp with
          // two SUBBYTES extracts), declare a
          // `{fp,fp}` struct return so the backend's HFA lowering writes both
          // result registers and the struct-return flatten below repacks
          // them. The struct-return flatten packs the two elements
          // CONTIGUOUSLY, so the element width must equal the lifter's
          // per-field slot (Output.Size/2) for the second element to land at
          // its return register's byte offset. On AArch64 a `str d0,d1` of a
          // `_Complex float` result reads full D registers, so the lifter
          // lays the fields out 8 bytes apart even though the values are
          // 32-bit floats — declaring `{double,double}` there keeps the
          // layout aligned and the float values occupy the low 32 bits, read
          // back correctly via the S0/S1 sub-register views.  If only the
          // real part was used (output stayed a single register), a scalar FP
          // return (real part in d0) is exactly right, so fall through.
          llvm::Type *RetTy2 = FpTy;
          if (Arity->FpRetComplex && Op.Output.Kind == MedVar::Temp &&
              Op.Output.Size >= 2) {
            unsigned Half = static_cast<unsigned>(Op.Output.Size) / 2;
            llvm::Type *ElemTy = Half >= 8 ? llvm::Type::getDoubleTy(*Ctx)
                                           : llvm::Type::getFloatTy(*Ctx);
            RetTy2 = llvm::StructType::get(*Ctx, {ElemTy, ElemTy});
          }
          // Mixed FP-first libm (ldexp/frexp/modf/scalbn): the recovered
          // arguments are in the int-then-FP model order, but the real
          // signature is the FP arg(s) first.  Reorder by count (the leading
          // NInt are the int/pointer args, the next NFp are the FP args) so
          // they line up with the declared FP-first signature.
          if (Arity->FpFirst && NInt > 0 &&
              static_cast<int>(Args.size()) >= NInt + NFp) {
            std::vector<llvm::Value *> Reordered;
            for (int I = 0; I < NFp; ++I)
              Reordered.push_back(Args[NInt + I]);
            for (int I = 0; I < NInt; ++I)
              Reordered.push_back(Args[I]);
            for (size_t I = static_cast<size_t>(NInt + NFp); I < Args.size();
                 ++I)
              Reordered.push_back(Args[I]);
            Args = std::move(Reordered);
          }
          Callee = Mod->getFunction(CalleeName);
          if (!Callee) {
            // FP params first, then int/pointer params (modelled as i64; the
            // callee reads the low 32 bits for an `int` arg, and an i64
            // pointer value occupies the same x-register).
            std::vector<llvm::Type *> ParamTys;
            for (int I = 0; I < NFp; ++I)
              ParamTys.push_back(FpTy);
            for (int I = 0; I < NInt; ++I)
              ParamTys.push_back(I64Ty);
            auto *FT = llvm::FunctionType::get(RetTy2, ParamTys, false);
            Callee = llvm::Function::Create(
                FT, llvm::GlobalValue::ExternalLinkage, CalleeName, Mod);
            Callee->setCallingConv(llvm::CallingConv::C);
          }
        }
      }

      if (!Callee && !CalleeName.empty()) {
        bool UseVarArg = getTargetRegInfo(TargetArch).UnknownExternIsVarArg;
        auto *FT = llvm::FunctionType::get(RetTy, ArgTypes, UseVarArg);
        Callee = llvm::Function::Create(FT, llvm::GlobalValue::ExternalLinkage,
                                        CalleeName, Mod);
        Callee->setCallingConv(llvm::CallingConv::C);
      }

      if (!Callee) {
        auto StubName = (kAutoFuncPrefix + llvm::utohexstr(CallAddr)).str();
        Callee = Mod->getFunction(StubName);
        if (!Callee) {
          auto *StubTy = llvm::FunctionType::get(RetTy, ArgTypes, false);
          Callee = llvm::Function::Create(
              StubTy, llvm::GlobalValue::ExternalLinkage, StubName, Mod);
          Callee->addFnAttr(llvm::Attribute::NullPointerIsValid);
        }
      }

      // setjmp/longjmp control-flow semantics.  setjmp may return twice
      // (control re-enters the call site when a matching longjmp restores the
      // saved context): without `returns_twice` the backend can leave a value
      // live across the call in a caller-saved register that longjmp does not
      // restore, so the longjmp-return path reads garbage. longjmp/abort/exit
      // never return; `noreturn` lets the defensive dead `ret` after the call
      // (emitted for the no-successor block) fold to `unreachable`.  The CFG
      // builder uses the same isNoReturnFunction set to stop the
      // fall-through, so the attribute can never contradict a genuinely
      // reachable continuation.
      if (Callee && !CalleeName.empty()) {
        llvm::StringRef Bare = stripLeadingUnderscores(CalleeName);
        if (libc::isReturnsTwiceFunction(Bare))
          Callee->addFnAttr(llvm::Attribute::ReturnsTwice);
        else if (libc::isNoReturnFunction(Bare))
          Callee->addFnAttr(llvm::Attribute::NoReturn);
      }
    }
  }

  if (Callee) {
    auto *CalleeTy = Callee->getFunctionType();
    std::vector<llvm::Value *> CoercedArgs;
    for (size_t AI = 0; AI < Args.size(); ++AI) {
      llvm::Value *AV = Args[AI];
      if (AI < CalleeTy->getNumParams()) {
        auto *Want = CalleeTy->getParamType(AI);
        // Symbolize a pointer-width integer arg resolving to a global address
        // when the callee param is itself an integer.  A callee recovered
        // with integer params (e.g. a variadic call whose args were recovered
        // as fixed integer params) never reaches the pointer-param path
        // below, so a computed `&global[i]` would be passed as a stale
        // absolute VA the callee dereferences into unmapped memory.
        // tryResolvePointerArg only fires for a provable data/rodata address
        // and the width guard keeps a narrow integer untouched; converting
        // back to the integer param keeps the recovered signature valid. Runs
        // before the type-mismatch block so a same-width (i64==i64) arg is
        // symbolized too.
        if (unsigned PtrSz = getTargetRegInfo(TargetArch).PointerSize;
            Want->isIntegerTy() && AV->getType()->isIntegerTy() && CI &&
            AI < CI->Args.size() && PtrSz &&
            Want->getIntegerBitWidth() == PtrSz * 8)
          if (llvm::Value *Sym = tryResolvePointerArg(CI->Args[AI], Builder))
            AV = Builder.CreatePtrToInt(Sym, Want);
        if (AV->getType() != Want) {
          llvm::Type *AT = AV->getType();
          if (AT->isIntegerTy() && Want->isIntegerTy()) {
            if (AT->getIntegerBitWidth() > Want->getIntegerBitWidth())
              AV = Builder.CreateTrunc(AV, Want);
            else
              AV = Builder.CreateZExt(AV, Want);
          } else if (AT->isIntegerTy() && Want->isPointerTy()) {
            // Symbolize a computed `&global[index]` so the callee
            // dereferences the recompiled global rather than a stale absolute
            // table VA.
            llvm::Value *Sym = nullptr;
            if (CI && AI < CI->Args.size())
              Sym = tryResolvePointerArg(CI->Args[AI], Builder);
            AV = Sym ? Sym : Builder.CreateIntToPtr(AV, Want);
          } else if (AT->isPointerTy() && Want->isIntegerTy()) {
            AV = Builder.CreatePtrToInt(AV, Want);
          } else if (!AT->isPointerTy() && !Want->isPointerTy()) {
            // A floating-point/vector argument is held in its register as an
            // integer (XMM/V tracked as i128, a scalar in the low lane as
            // i64). Bridge through an integer of each side's width to reach
            // the vector ABI type <2 x i64>: a same-width reinterpret, or a
            // scalar in the low lane widened (zero high lane) to the full
            // vector.
            unsigned ABits = AT->getPrimitiveSizeInBits();
            unsigned WBits = Want->getPrimitiveSizeInBits();
            if (ABits && WBits) {
              llvm::Value *AsInt =
                  AT->isIntegerTy()
                      ? AV
                      : Builder.CreateBitCast(
                            AV, llvm::Type::getIntNTy(*Ctx, ABits));
              auto *WInt = llvm::Type::getIntNTy(*Ctx, WBits);
              if (ABits > WBits)
                AsInt = Builder.CreateTrunc(AsInt, WInt);
              else if (ABits < WBits)
                AsInt = Builder.CreateZExt(AsInt, WInt);
              AV = Want->isIntegerTy() ? AsInt
                                       : Builder.CreateBitCast(AsInt, Want);
            }
          }
        }
      }
      CoercedArgs.push_back(AV);
    }
    if (!CalleeTy->isVarArg()) {
      if (CoercedArgs.size() > CalleeTy->getNumParams())
        CoercedArgs.resize(CalleeTy->getNumParams());
      while (CoercedArgs.size() < CalleeTy->getNumParams()) {
        auto *PadTy = CalleeTy->getParamType(CoercedArgs.size());
        CoercedArgs.push_back(llvm::Constant::getNullValue(PadTy));
      }
    }
    Result = Builder.CreateCall(Callee, CoercedArgs, "call");
  } else {
    // Indirect call: the callee is unknown, so its LLVM signature is rebuilt
    // from the recovered register classes.  A target with a separate FP
    // register file (AArch64 v0-v7, x86-64 xmm0-7) passes a floating-point
    // argument in a vector register and returns an FP result in one, so an
    // argument whose recovered source is an FP/vector register must take the
    // vector ABI type (<2 x i64>) — typing it as an integer would route it
    // through the GP registers and the callee would read d0 garbage — and an
    // FP-return-register output makes the call return that vector type so the
    // result is read from d0/v0 instead of x0.  A small struct returned
    // across multiple registers (modelCallStructReturn rewrote the output to
    // a flat aggregate temp) is declared as the matching aggregate so every
    // field register (x0:x1 / v0:v1) is read.  Non-FP, non-aggregate indirect
    // calls keep the original integer signature unchanged.
    const auto &ITRI = getTargetRegInfo(TargetArch);
    auto *Vec2 = llvm::FixedVectorType::get(I64Ty, 2);

    // Floating-point arguments past the FP-argument registers (v0-v7 /
    // xmm0-7) are passed on the stack as their natural 8-byte (double) or
    // 4-byte (float) scalar -- not the 16-byte vector ABI type used for the
    // in-register ones.  A <2 x i64> stack argument would occupy two 8-byte
    // slots and shift every slot the callee reads past it (the 10th double of
    // a `double(*)(double x10)` call then lands in the 9th's high half).  The
    // overflow ones share the FP register file with the in-register ones, so
    // the leading FP arguments (assembled FP-register-first, then stack) fill
    // the registers and only the surplus spills.
    const int NumFPRegs = static_cast<int>(ITRI.FPParamRegs.size());
    int FPSeen = 0;
    // A variadic indirect call (Darwin AArch64, marked by VarArgFixedCount):
    // every variadic argument -- FP included -- is passed on the stack, none
    // in a vector register.  A variadic FP argument therefore takes its
    // natural 8-byte double type, NOT the 16-byte <2 x i64> register ABI type
    // used for the fixed FP prefix (a <2 x i64> would occupy two stack slots
    // and shift every slot the callee reads off its va_list, so
    // `dsum(n, 1.5, 2.5, ...)` would read 1.5, 0, 2.5, 0, ...).
    const bool IsVariadicCall = CI && CI->VarArgFixedCount >= 0;
    std::vector<llvm::Type *> ParamTys;
    for (size_t AI = 0; AI < Args.size(); ++AI) {
      bool IsFP = CI && AI < CI->Args.size() &&
                  CI->Args[AI].Kind == MedVar::Reg &&
                  ITRI.isFPArgReg(CI->Args[AI].RegOff);
      bool IsVariadicTail =
          IsVariadicCall && AI >= static_cast<size_t>(CI->VarArgFixedCount);
      if (IsFP) {
        if (FPSeen < NumFPRegs && !IsVariadicTail) {
          ParamTys.push_back(Vec2);
        } else {
          uint16_t Sz = CI->Args[AI].Size;
          ParamTys.push_back(Sz && Sz <= 4 ? llvm::Type::getFloatTy(*Ctx)
                                           : llvm::Type::getDoubleTy(*Ctx));
        }
        ++FPSeen;
      } else {
        ParamTys.push_back(Args[AI]->getType());
      }
    }

    llvm::Type *IRetTy = RetTy; // default i64
    if (Op.Output.Kind == MedVar::Reg && Op.Output.Size > 0 &&
        ITRI.isVectorReg(Op.Output.RegOff)) {
      IRetTy = Vec2; // scalar FP result returned in v0/d0
    } else if (Op.Output.Kind == MedVar::Reg && Op.Output.Size > 0 &&
               ITRI.isX87StackReg(Op.Output.RegOff)) {
      // i386 returns a scalar float/double in the x87 st0 register, not in
      // the integer EAX:EDX pair.  Typing the indirect call as `i64` makes the
      // backend read the result from EAX:EDX while the recompiled callee left
      // it in st0 (its `ret double` lowers to an x87 return), so the caller
      // reads integer-register garbage.  Type the call as `double` so the
      // backend reads st0; a float-returning callee's st0 value converts to
      // double exactly, and the post-call FPExt below widens it to the
      // x86_fp80 the st0 output register models.
      IRetTy = llvm::Type::getDoubleTy(*Ctx);
    } else if (Op.Output.Kind == MedVar::Temp && Op.Output.Size > 0 &&
               CurMedFunc) {
      // Reconstruct the aggregate from the SUBBYTES extracts that slice this
      // call's flat output temp into the field return registers.
      std::vector<llvm::Type *> FieldTys;
      for (const auto &Blk2 : CurMedFunc->Blocks) {
        if (Blk2.Id != BlockId)
          continue;
        for (size_t J = static_cast<size_t>(OpIdx) + 1; J < Blk2.Ops.size();
             ++J) {
          const auto &Ex = Blk2.Ops[J];
          if (Ex.Opcode != NdOp::SUBBYTES || Ex.NumInputs < 1 ||
              Ex.Inputs[0].Kind != MedVar::Temp ||
              Ex.Inputs[0].Id != Op.Output.Id || Ex.Output.Kind != MedVar::Reg)
            break;
          uint16_t Sz = Ex.Output.Size ? Ex.Output.Size : 8;
          if (ITRI.isVectorReg(Ex.Output.RegOff))
            FieldTys.push_back(Sz <= 4 ? llvm::Type::getFloatTy(*Ctx)
                                       : llvm::Type::getDoubleTy(*Ctx));
          else
            FieldTys.push_back(llvm::Type::getIntNTy(*Ctx, Sz * 8));
        }
        break;
      }
      if (FieldTys.size() >= 2)
        IRetTy = llvm::StructType::get(*Ctx, FieldTys);
    }

    // Coerce each argument to its reconstructed ABI parameter type.  An
    // integer argument that resolves to a recompiled global address is
    // symbolized (the direct path's tryResolvePointerArg) so a computed
    // `&global[i]` passed to a function pointer dereferences the recompiled
    // global rather than a stale absolute VA; FP/vector arguments are bridged
    // through an integer of each side's width to reach <2 x i64>.
    std::vector<llvm::Value *> CoercedArgs;
    for (size_t AI = 0; AI < Args.size(); ++AI) {
      llvm::Value *AV = Args[AI];
      llvm::Type *Want = ParamTys[AI];
      if (Want->isIntegerTy() && AV->getType()->isIntegerTy() && CI &&
          AI < CI->Args.size())
        if (llvm::Value *Sym = tryResolvePointerArg(CI->Args[AI], Builder))
          AV = Builder.CreatePtrToInt(Sym, AV->getType());
      if (AV->getType() != Want) {
        llvm::Type *AT = AV->getType();
        if (AT->isIntegerTy() && Want->isIntegerTy()) {
          AV = AT->getIntegerBitWidth() > Want->getIntegerBitWidth()
                   ? Builder.CreateTrunc(AV, Want)
                   : Builder.CreateZExt(AV, Want);
        } else if (AT->isPointerTy() && Want->isIntegerTy()) {
          AV = Builder.CreatePtrToInt(AV, Want);
        } else if (AT->isIntegerTy() && Want->isPointerTy()) {
          AV = Builder.CreateIntToPtr(AV, Want);
        } else if (!AT->isPointerTy() && !Want->isPointerTy()) {
          unsigned ABits = AT->getPrimitiveSizeInBits();
          unsigned WBits = Want->getPrimitiveSizeInBits();
          if (ABits && WBits) {
            llvm::Value *AsInt =
                AT->isIntegerTy() ? AV
                                  : Builder.CreateBitCast(
                                        AV, llvm::Type::getIntNTy(*Ctx, ABits));
            auto *WInt = llvm::Type::getIntNTy(*Ctx, WBits);
            if (ABits > WBits)
              AsInt = Builder.CreateTrunc(AsInt, WInt);
            else if (ABits < WBits)
              AsInt = Builder.CreateZExt(AsInt, WInt);
            AV = Want->isIntegerTy() ? AsInt
                                     : Builder.CreateBitCast(AsInt, Want);
          }
        }
      }
      CoercedArgs.push_back(AV);
    }

    auto *IndirectFT = llvm::FunctionType::get(IRetTy, ParamTys, false);
    if (CI && CI->VarArgFixedCount >= 0 &&
        static_cast<size_t>(CI->VarArgFixedCount) <= ParamTys.size()) {
      std::vector<llvm::Type *> FixedTys(
          ParamTys.begin(), ParamTys.begin() + CI->VarArgFixedCount);
      IndirectFT = llvm::FunctionType::get(IRetTy, FixedTys, true);
    }
    auto *FPtr =
        Builder.CreateIntToPtr(Target, llvm::PointerType::get(*Ctx, 0));
    Result = Builder.CreateCall(IndirectFT, FPtr, CoercedArgs,
                                CoercedArgs.empty() ? "icall" : "call");
  }

  // Itanium call-site ranges are tight around individual calls rather than
  // aligned to machine blocks, so the address this call came from has to
  // survive lowering for the LSDA to be able to name it.
  if (auto *Emitted = llvm::dyn_cast_or_null<llvm::CallInst>(Result))
    CallSiteAddrs[Emitted] = Op.Addr;

  // A direct call returning a small struct by value across multiple registers
  // (modelCallStructReturn rewrote it to a flat aggregate temp): the callee's
  // LLVM type is the matching struct, so flatten the returned aggregate into
  // a packed wide integer that the SUBBYTES extract ops slice into each field
  // return register (the inverse of the callee-side aggregate assembly).
  if (Result && Result->getType()->isStructTy() &&
      Op.Output.Kind == MedVar::Temp && Op.Output.Size > 0) {
    auto *ST = llvm::cast<llvm::StructType>(Result->getType());
    unsigned TotalBits = static_cast<unsigned>(Op.Output.Size) * 8;
    auto *WideTy = llvm::Type::getIntNTy(*Ctx, TotalBits);
    llvm::Value *Acc = llvm::ConstantInt::get(WideTy, 0);
    unsigned CumBits = 0;
    for (unsigned I = 0; I < ST->getNumElements(); ++I) {
      llvm::Value *F = Builder.CreateExtractValue(Result, I);
      auto *FT = ST->getElementType(I);
      unsigned FBits = FT->getPrimitiveSizeInBits();
      llvm::Value *Fi =
          FT->isIntegerTy()
              ? F
              : Builder.CreateBitCast(F, llvm::Type::getIntNTy(*Ctx, FBits));
      Fi = Builder.CreateZExt(Fi, WideTy);
      if (CumBits)
        Fi = Builder.CreateShl(Fi, llvm::ConstantInt::get(WideTy, CumBits));
      Acc = Builder.CreateOr(Acc, Fi);
      CumBits += FBits;
    }
    setVar(Op.Output, Acc, Builder);
    defineCallClobbers(Op, Builder);
    return;
  }

  if (Op.Output.Size > 0) {
    // A scalar float/double returned through the x87 stack (i386 st0) is held
    // in the 80-bit ST register as the widened FP value, not raw bits: fp-
    // extend it to x86_fp80 so the caller's `fstp` conversion (FLOAT2FLOAT
    // back to float/double) reads the right number instead of a denormal
    // formed from zero-extended narrow-FP bits.
    if (Result && Result->getType()->isFloatingPointTy() &&
        Result->getType()->getPrimitiveSizeInBits() < 80 &&
        getTargetRegInfo(TargetArch).isX87StackReg(Op.Output.RegOff))
      Result = Builder.CreateFPExt(Result, llvm::Type::getX86_FP80Ty(*Ctx));
    setVar(Op.Output, Result, Builder);
  }
  defineCallClobbers(Op, Builder);
  return;
}

//===----------------------------------------------------------------------===//
// RETURN -- return-value recovery and materialization
//===----------------------------------------------------------------------===//

void MedLLVMEmitter::emitReturnOp(const MedOp &Op, llvm::IRBuilder<> &Builder) {
  auto GetInput = [&](uint8_t Idx) -> llvm::Value * {
    if (Idx >= Op.NumInputs)
      return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), 0);
    return getVar(Op.Inputs[Idx], Builder);
  };
  auto *RetTy = CurFunc->getReturnType();
  if (RetTy->isVoidTy()) {
    Builder.CreateRetVoid();
    return;
  }

  // A small struct returned by value across multiple registers: assemble the
  // LLVM aggregate from each field register's value so the backend places the
  // fields back in their return registers (the inverse of the call-site
  // flatten).  Each field register's value is found like the scalar return
  // (widest in-block write, else a phi, else a predecessor walk).
  if (RetTy->isStructTy() && CurMedFunc && !CurMedFunc->MultiReturn.empty()) {
    auto *ST = llvm::cast<llvm::StructType>(RetTy);
    const MedBlock *RetBlk = nullptr;
    for (auto &Blk : CurMedFunc->Blocks)
      for (auto &BOp : Blk.Ops)
        if (&BOp == &Op) {
          RetBlk = &Blk;
          break;
        }
    auto findRegVar = [&](uint64_t RegOff) -> const MedVar * {
      if (RetBlk) {
        const MedVar *RV = nullptr;
        for (auto RIt = RetBlk->Ops.rbegin(); RIt != RetBlk->Ops.rend();
             ++RIt) {
          if (&*RIt == &Op)
            continue;
          if (RIt->Output.Kind == MedVar::Reg && RIt->Output.Size > 0 &&
              RIt->Output.RegOff == RegOff &&
              (!RV || RIt->Output.Size > RV->Size))
            RV = &RIt->Output;
        }
        if (!RV)
          for (auto &Phi : RetBlk->Phis)
            if (Phi.Output.Kind == MedVar::Reg && Phi.Output.Size > 0 &&
                Phi.Output.RegOff == RegOff &&
                (!RV || Phi.Output.Size > RV->Size))
              RV = &Phi.Output;
        if (RV)
          return RV;
      }
      // Predecessor walk (shared epilogue / value computed upstream).
      if (RetBlk) {
        std::set<int> Visited{RetBlk->Id};
        std::vector<int> Work(RetBlk->Preds.begin(), RetBlk->Preds.end());
        while (!Work.empty()) {
          int BId = Work.back();
          Work.pop_back();
          if (!Visited.insert(BId).second || BId < 0 ||
              BId >= static_cast<int>(CurMedFunc->Blocks.size()))
            continue;
          auto &B = CurMedFunc->Blocks[BId];
          const MedVar *RV = nullptr;
          for (auto BIt = B.Ops.rbegin(); BIt != B.Ops.rend(); ++BIt)
            if (BIt->Output.Kind == MedVar::Reg && BIt->Output.Size > 0 &&
                BIt->Output.RegOff == RegOff &&
                (!RV || BIt->Output.Size > RV->Size))
              RV = &BIt->Output;
          if (!RV)
            for (auto &Phi : B.Phis)
              if (Phi.Output.Kind == MedVar::Reg && Phi.Output.Size > 0 &&
                  Phi.Output.RegOff == RegOff)
                RV = &Phi.Output;
          if (RV)
            return RV;
          for (int P : B.Preds)
            Work.push_back(P);
        }
      }
      return nullptr;
    };
    // Coerce a register's value (held as an integer / pointer / FP) to the
    // field type, narrowing through an integer of the field's bit width.
    auto coerce = [&](llvm::Value *V, llvm::Type *FT) -> llvm::Value * {
      unsigned FBits = FT->getPrimitiveSizeInBits();
      llvm::Value *Iv;
      if (V->getType()->isIntegerTy())
        Iv = V;
      else if (V->getType()->isPointerTy())
        Iv = Builder.CreatePtrToInt(V, llvm::Type::getInt64Ty(*Ctx));
      else
        Iv = Builder.CreateBitCast(
            V, llvm::Type::getIntNTy(*Ctx,
                                     V->getType()->getPrimitiveSizeInBits()));
      unsigned VBits = Iv->getType()->getIntegerBitWidth();
      if (VBits > FBits)
        Iv = Builder.CreateTrunc(Iv, llvm::Type::getIntNTy(*Ctx, FBits));
      else if (VBits < FBits)
        Iv = Builder.CreateZExt(Iv, llvm::Type::getIntNTy(*Ctx, FBits));
      return FT->isIntegerTy() ? Iv : Builder.CreateBitCast(Iv, FT);
    };
    llvm::Value *Agg = llvm::UndefValue::get(ST);
    for (unsigned I = 0;
         I < CurMedFunc->MultiReturn.size() && I < ST->getNumElements(); ++I) {
      const auto &RR = CurMedFunc->MultiReturn[I];
      const MedVar *RV = findRegVar(RR.RegOff);
      auto *FT = ST->getElementType(I);
      llvm::Value *V = nullptr;
      if (RV) {
        // Symbolize a pointer-width integer field holding a writable-global
        // address (`return (struct){ &g[i], n }`) — the aggregate analog of
        // the scalar return symbolization, else the caller dereferences a raw
        // absolute VA.  FP fields and plain integers fall through to getVar.
        if (!RR.IsFP && FT->isIntegerTy())
          if (llvm::Value *Sym = tryResolveWritableData(
                  *RV, RV->Size, Builder, /*IsValueOperand=*/true))
            V = Builder.CreatePtrToInt(Sym, llvm::Type::getInt64Ty(*Ctx));
        if (!V)
          V = getVar(*RV, Builder);
      }
      V = V ? coerce(V, FT) : llvm::Constant::getNullValue(FT);
      Agg = Builder.CreateInsertValue(Agg, V, I);
    }
    Builder.CreateRet(Agg);
    return;
  }

  llvm::Value *RetVal = nullptr;
  if (CurMedFunc) {
    const auto &TRI = getTargetRegInfo(TargetArch);
    // A vector return type (x86-64 models a scalar FP return as the 128-bit
    // XMM0 vector) is also carried in the FP return register, not RAX.
    bool WantFloat =
        RetTy->isFloatTy() || RetTy->isDoubleTy() || RetTy->isVectorTy();
    // i386 cdecl returns scalar FP values through the x87 stack.  The physical
    // register carrying logical st0 depends on TOP at the return site (often
    // ST7 after a final `fld`), so match the newest x87-stack write instead of
    // an older XMM0 temporary.  Apple Clang commonly leaves the final value in
    // XMM0 as well, but upstream Clang is free to schedule the calculation so
    // only the x87 load materializes the ABI return.
    const bool WantX87 = WantFloat && CurMedFunc->FPReturnViaX87;
    // x86 returns FP in XMM0; on ARM/AArch64 the FP return value is modeled
    // in the integer return register here (V0/D0 are not the tracked var).
    uint64_t FloatRetOff = TRI.fpReturnModelReg();
    uint64_t IntRetOff = TRI.IntReturnReg;
    // A 32-bit target returns a 64-bit integer in a register pair: the low
    // half in IntReturnReg (EAX/R0) and the high half in IntReturnReg2
    // (EDX/R1).  Combine both halves so the high 32 bits are not dropped.
    bool WantWide64 = !WantFloat && RetTy->isIntegerTy(64) &&
                      TRI.PointerSize == 4 && TRI.IntReturnReg2 != 0;
    uint64_t HiRetOff = TRI.IntReturnReg2;

    for (auto &Blk : CurMedFunc->Blocks) {
      bool HasThisRet = false;
      for (auto &BOp : Blk.Ops)
        if (&BOp == &Op) {
          HasThisRet = true;
          break;
        }
      if (!HasThisRet)
        continue;

      const MedVar *RetVar = nullptr;
      const MedVar *HiVar = nullptr;
      for (auto RIt = Blk.Ops.rbegin(); RIt != Blk.Ops.rend(); ++RIt) {
        if (&*RIt == &Op)
          continue;
        if (RIt->Output.Kind != MedVar::Reg || RIt->Output.Size == 0)
          continue;

        if (WantX87 && TRI.isX87StackReg(RIt->Output.RegOff)) {
          // Reverse iteration sees the value at the current x87 top first.
          if (!RetVar)
            RetVar = &RIt->Output;
        } else if (WantFloat && !WantX87 && RIt->Output.RegOff == FloatRetOff) {
          if (!RetVar || RIt->Output.Size > RetVar->Size)
            RetVar = &RIt->Output;
        }
        if (!WantFloat && RIt->Output.RegOff == IntRetOff) {
          if (!RetVar || RIt->Output.Size > RetVar->Size)
            RetVar = &RIt->Output;
        }
        if (WantWide64 && RIt->Output.RegOff == HiRetOff) {
          // A SUBBYTES that extracts the high PointerSize bytes (e.g. ARM
          // `vmov rLo,rHi,dN` lowers the high register to SUBBYTES(d,4)) is a
          // genuine high half; only a sub-register narrowing at another
          // offset is rejected.
          bool IsHighSubpiece = RIt->Opcode == NdOp::SUBBYTES &&
                                RIt->NumInputs >= 2 &&
                                RIt->Inputs[1].isConst() &&
                                RIt->Inputs[1].ConstVal == TRI.PointerSize;
          if ((RIt->Opcode != NdOp::SUBBYTES || IsHighSubpiece) &&
              (!HiVar || RIt->Output.Size > HiVar->Size))
            HiVar = &RIt->Output;
        }
      }
      if (!RetVar) {
        // Pick the *widest* matching phi, not the first.  When a block has
        // both a narrow (EAX) and wide (RAX) phi for the return register,
        // the wide one carries the true 64-bit value (bug #157c).
        for (auto &Phi : Blk.Phis) {
          if (Phi.Output.Kind != MedVar::Reg || Phi.Output.Size == 0)
            continue;
          if (WantX87 && TRI.isX87StackReg(Phi.Output.RegOff)) {
            if (!RetVar || Phi.Output.Size > RetVar->Size)
              RetVar = &Phi.Output;
          } else if (WantFloat && !WantX87 &&
                     Phi.Output.RegOff == FloatRetOff) {
            if (!RetVar || Phi.Output.Size > RetVar->Size)
              RetVar = &Phi.Output;
          }
          if (!WantFloat && Phi.Output.RegOff == IntRetOff) {
            if (!RetVar || Phi.Output.Size > RetVar->Size)
              RetVar = &Phi.Output;
          }
        }
      }
      if (WantWide64 && !HiVar) {
        for (auto &Phi : Blk.Phis)
          if (Phi.Output.Kind == MedVar::Reg && Phi.Output.Size > 0 &&
              Phi.Output.RegOff == HiRetOff)
            if (!HiVar || Phi.Output.Size > HiVar->Size)
              HiVar = &Phi.Output;
      }
      if (RetVar) {
        RetVal = getVar(*RetVar, Builder);
        // A returned address into a WRITABLE global (`return &g[index]` /
        // `return cond?A:B`) must be symbolized — otherwise the value is a
        // raw inttoptr(baseVA+index) and the caller, dereferencing the
        // returned pointer, hits a stale absolute VA.  Only the writable-data
        // resolver is used here (not the full pointer-arg chain): every
        // integer return flows through this path, and the heuristic
        // rodata-table resolvers would false-positive on a computed integer
        // result that merely traces to a read-only base (e.g. a NEON kernel
        // folding a rodata vector).
        if (!WantFloat && !WantWide64 && RetTy->isIntegerTy() && RetVal &&
            RetVal->getType()->isIntegerTy())
          if (llvm::Value *Sym = tryResolveWritableData(
                  *RetVar, RetVar->Size, Builder, /*IsValueOperand=*/true))
            RetVal = Builder.CreatePtrToInt(Sym, RetTy);
      }
      // Splice the high half above the low half into the 64-bit result.
      if (WantWide64 && RetVal && HiVar) {
        auto *I64 = llvm::Type::getInt64Ty(*Ctx);
        auto toI64 = [&](llvm::Value *V) -> llvm::Value * {
          if (V->getType() == I64)
            return V;
          if (V->getType()->getIntegerBitWidth() > 64)
            return Builder.CreateTrunc(V, I64);
          return Builder.CreateZExt(V, I64);
        };
        llvm::Value *Lo = toI64(RetVal);
        llvm::Value *Hi = toI64(getVar(*HiVar, Builder));
        RetVal = Builder.CreateOr(
            Builder.CreateShl(Hi, llvm::ConstantInt::get(I64, 32)), Lo,
            "ret64");
      }
      break;
    }
  }

  if (!RetVal && CurMedFunc) {
    const auto &TRI = getTargetRegInfo(TargetArch);
    // An FP-returning function whose RETURN block has no FP-return-register
    // write (a shared epilogue, or an early `return arg` that passes the FP
    // argument straight through) must still walk predecessors for the FP
    // register, not fall back to the integer return register — returning RAX
    // for a `<2 x i64>` function type produces a type-mismatched `ret`.
    bool WantFloat =
        RetTy->isFloatTy() || RetTy->isDoubleTy() || RetTy->isVectorTy();
    const bool WantX87 = WantFloat && CurMedFunc->FPReturnViaX87;
    uint64_t RetRegOff = WantFloat ? TRI.fpReturnModelReg() : TRI.IntReturnReg;
    int RetBlkId = -1;
    for (auto &Blk : CurMedFunc->Blocks) {
      for (auto &BOp : Blk.Ops)
        if (&BOp == &Op) {
          RetBlkId = Blk.Id;
          break;
        }
      if (RetBlkId >= 0)
        break;
    }
    if (RetBlkId >= 0) {
      auto &RetBlk = CurMedFunc->Blocks[RetBlkId];
      std::set<int> Visited;
      Visited.insert(RetBlkId);
      std::vector<int> Worklist(RetBlk.Preds.begin(), RetBlk.Preds.end());
      while (!Worklist.empty() && !RetVal) {
        int BId = Worklist.back();
        Worklist.pop_back();
        if (!Visited.insert(BId).second)
          continue;
        if (BId < 0 || BId >= static_cast<int>(CurMedFunc->Blocks.size()))
          continue;
        auto &Blk = CurMedFunc->Blocks[BId];
        // Select the *widest* write to the return register, not the last
        // one in program order.  A narrow sub-register SUBBYTES (e.g.
        // EAX = SUBBYTES(RAX)) often follows the full-width write but does
        // not represent the canonical return value (bug #152 extension).
        const MedVar *RetVar = nullptr;
        for (auto BIt = Blk.Ops.rbegin(); BIt != Blk.Ops.rend(); ++BIt) {
          if (BIt->Output.Kind == MedVar::Reg && BIt->Output.Size > 0 &&
              (WantX87 ? TRI.isX87StackReg(BIt->Output.RegOff)
                       : BIt->Output.RegOff == RetRegOff)) {
            if (!RetVar || BIt->Output.Size > RetVar->Size)
              RetVar = &BIt->Output;
          }
        }
        if (RetVar) {
          RetVal = getVar(*RetVar, Builder);
          break;
        }
        for (auto &Phi : Blk.Phis) {
          if (Phi.Output.Kind == MedVar::Reg && Phi.Output.Size > 0 &&
              (WantX87 ? TRI.isX87StackReg(Phi.Output.RegOff)
                       : Phi.Output.RegOff == RetRegOff)) {
            RetVal = getVar(Phi.Output, Builder);
            break;
          }
        }
        if (!RetVal) {
          for (int P : Blk.Preds)
            Worklist.push_back(P);
        }
      }
    }

    // Still nothing: the FP result is the function's incoming FP argument,
    // returned unchanged (e.g. `double f(int n,double x){ if(n<=0) return x;
    // …}` on the `n<=0` path, where the FP return register is never written).
    // Read it as the live-in value of the FP return register.
    if (!RetVal && WantFloat && !WantX87 && RetRegOff != 0) {
      MedVar FpLiveIn;
      FpLiveIn.Kind = MedVar::Reg;
      FpLiveIn.RegOff = RetRegOff;
      FpLiveIn.Size = TRI.VecRegStride >= 16 ? 16 : 8;
      FpLiveIn.SSAVer = 0;
      FpLiveIn.Id = -1;
      FpLiveIn.TheArch = TargetArch;
      RetVal = getVar(FpLiveIn, Builder);
    }
  }

  if (!RetVal) {
    const auto &TRI = getTargetRegInfo(TargetArch);
    // ARM/AArch64 RETURN operands are control-flow targets (LR or a pc value
    // restored from the stack), never ABI return values.  If no write to the
    // real return register was found above, materialize the neutral residual
    // used by void inference instead of returning that branch target.  X86
    // lifters put RAX/EAX in the RETURN operand, so retain their fallback.
    if (TRI.LinkRegister != 0 || Op.NumInputs == 0)
      RetVal = llvm::ConstantInt::get(RetTy, 0);
    else
      RetVal = GetInput(0);
  }

  if (RetVal->getType() != RetTy) {
    if ((RetTy->isFloatTy() || RetTy->isDoubleTy()) && CurMedFunc &&
        CurMedFunc->FPReturnViaX87 &&
        (RetVal->getType()->isX86_FP80Ty() ||
         RetVal->getType()->isIntegerTy(80))) {
      // The x87 register is an 80-bit numeric value, not a wider bit container
      // whose low bits encode an IEEE float/double.  Convert its precision;
      // bitcasting and truncating the i80 representation corrupts the result.
      if (RetVal->getType()->isIntegerTy())
        RetVal = Builder.CreateBitCast(RetVal, llvm::Type::getX86_FP80Ty(*Ctx));
      RetVal = Builder.CreateFPTrunc(RetVal, RetTy);
    } else if (RetTy->isIntegerTy() && RetVal->getType()->isIntegerTy()) {
      if (RetVal->getType()->getIntegerBitWidth() > RetTy->getIntegerBitWidth())
        RetVal = Builder.CreateTrunc(RetVal, RetTy);
      else
        RetVal = Builder.CreateZExt(RetVal, RetTy);
    } else if ((RetTy->isFloatTy() || RetTy->isDoubleTy()) &&
               RetVal->getType()->getPrimitiveSizeInBits() >
                   RetTy->getPrimitiveSizeInBits()) {
      // A scalar FP return (i386 x87 st0 convention) extracted from the wider
      // XMM return register: reinterpret the wide value as an integer, take
      // the low lane, then bitcast to the scalar FP type.
      unsigned WideBits = RetVal->getType()->getPrimitiveSizeInBits();
      llvm::Value *WideInt =
          Builder.CreateBitCast(RetVal, llvm::Type::getIntNTy(*Ctx, WideBits));
      llvm::Value *LoInt = Builder.CreateTrunc(
          WideInt,
          llvm::Type::getIntNTy(*Ctx, RetTy->getPrimitiveSizeInBits()));
      RetVal = Builder.CreateBitCast(LoInt, RetTy);
    } else if (RetVal->getType()->getPrimitiveSizeInBits() ==
               RetTy->getPrimitiveSizeInBits()) {
      // Same-width reinterpret: the 16-byte XMM return register is tracked as
      // an i128 but the FP/vector return type is <2 x i64> (and vice versa).
      RetVal = Builder.CreateBitCast(RetVal, RetTy);
    } else if (!RetVal->getType()->isPointerTy() && !RetTy->isPointerTy() &&
               RetVal->getType()->getPrimitiveSizeInBits() <
                   RetTy->getPrimitiveSizeInBits()) {
      // A scalar FP value occupying the low lane (e.g. an 8-byte D0 carrying
      // the FP argument passed straight through on a `return arg` path)
      // widened to the full vector return type (x86-64/AArch64 model the
      // scalar FP return as the 16-byte <2 x i64>): zero-extend through an
      // integer of the return type's width, then reinterpret, leaving the
      // high lane zero.
      unsigned NarrowBits = RetVal->getType()->getPrimitiveSizeInBits();
      unsigned WideBits = RetTy->getPrimitiveSizeInBits();
      llvm::Value *AsInt =
          RetVal->getType()->isIntegerTy()
              ? RetVal
              : Builder.CreateBitCast(RetVal,
                                      llvm::Type::getIntNTy(*Ctx, NarrowBits));
      AsInt = Builder.CreateZExt(AsInt, llvm::Type::getIntNTy(*Ctx, WideBits));
      RetVal = Builder.CreateBitCast(AsInt, RetTy);
    }
  }
  Builder.CreateRet(RetVal);
  return;
}

} // namespace neverd
