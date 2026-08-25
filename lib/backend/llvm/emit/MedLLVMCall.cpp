//===- MedLLVMCall.cpp - CALL/INDIR_CALL lowering ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Direct/indirect CALL lowering: libc/libm signature reconstruction, argument
/// coercion, and small-struct-return flattening.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/backend/llvm/LLVMName.h"
#include "neverd/backend/llvm/LanguageEHMetadata.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/object/SectionNames.h"

#define DEBUG_TYPE "neverd-med-llvm-call"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"

#include <map>
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

  const MedCallInfo *CI =
      CurMedFunc ? CurMedFunc->findCall(BlockId, OpIdx) : nullptr;
  // Address fragments are legal only while an address-forming expression is
  // being completed. Audit every call escape before ABI classification: an
  // indirect target, integer-class argument, FP/vector argument, or variadic
  // tail can otherwise bypass tryResolvePointerArg and leak a stale page VA.
  if (Op.Opcode == NdOp::INDIR_CALL && Op.NumInputs > 0)
    rejectEscapingAddressFragment(Op.Inputs[0], "an indirect call target");
  if (CI)
    for (const MedVar &Arg : CI->Args)
      rejectEscapingAddressFragment(Arg, "a call argument");

  llvm::Value *Target = nullptr;
  if (Op.Opcode == NdOp::INDIR_CALL) {
    if (Op.NumInputs > 0)
      Target = tryResolveIndirectCallTarget(Op.Inputs[0], Builder);
    if (!Target)
      Target = GetInput(0);
  }
  uint64_t CallAddr = Op.Inputs[0].isConst() ? Op.Inputs[0].ConstVal : 0;
  const Section *TargetSection =
      Img && CallAddr != 0 ? Img->getSectionFor(CallAddr) : nullptr;
  const bool IsObjCMessageStub =
      TargetSection && Img->isMachO() &&
      TargetSection->Name == section_names::macho::ObjCStubs;

  auto canonicalizeObjectCallee = [&](llvm::StringRef Name) -> std::string {
    if (!Img || TargetFormat != BinaryFormat::MachO || Name.empty())
      return Name.str();

    va_t TargetAddr = CallAddr;
    if (TargetAddr == 0 && CI)
      TargetAddr = CI->TargetAddr;
    bool IsObjectName = false;
    if (TargetAddr != 0) {
      if (const Import *Imp = Img->findImportAt(TargetAddr))
        IsObjectName = Imp->Name == Name;
      if (!IsObjectName)
        if (auto It = Img->ImportPtrSlots.find(TargetAddr);
            It != Img->ImportPtrSlots.end())
          IsObjectName = It->second == Name;
      if (!IsObjectName)
        if (auto It = Img->DyldBindSlots.find(TargetAddr);
            It != Img->DyldBindSlots.end())
          IsObjectName = It->second.Name == Name;
      if (!IsObjectName)
        for (const Symbol &Sym : Img->Symbols)
          if (Sym.Addr == TargetAddr && Sym.Name == Name) {
            IsObjectName = true;
            break;
          }
      if (!IsObjectName)
        for (const Export &Exp : Img->Exports)
          if (Exp.Addr == TargetAddr && Exp.Name == Name) {
            IsObjectName = true;
            break;
          }
    }
    if (!IsObjectName)
      for (const RelocationEntry &Rel : Img->Relocations)
        if ((Rel.Address == Op.Addr || Rel.Address == Op.Addr + 1) &&
            Rel.SymbolName == Name) {
          IsObjectName = true;
          break;
        }

    return IsObjectName ? llvm_name::fromObjectSymbol(Name, TargetFormat).str()
                        : Name.str();
  };

  std::vector<llvm::Value *> Args;
  if (CI) {
    for (auto &Arg : CI->Args) {
      llvm::Value *Value =
          tryResolveCodeAddressValue(Arg, /*RequireCodeRole=*/false, Builder);
      Args.push_back(Value ? Value : getVar(Arg, Builder));
    }
  }

  auto *RetTy = llvm::Type::getInt64Ty(*Ctx);
  auto *I8Ty = llvm::Type::getInt8Ty(*Ctx);
  auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
  auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
  auto *PtrTy = llvm::PointerType::getUnqual(*Ctx);

  std::vector<llvm::Type *> ArgTypes;
  for (auto *A : Args)
    ArgTypes.push_back(A->getType());

  // A multi-register return is represented in MedIR as one flat temporary
  // followed by SUBBYTES extracts into its physical return registers.  Recover
  // the corresponding LLVM aggregate type once, for both direct imports and
  // indirect calls.  Direct unknown externs previously kept the default i64
  // declaration even after call-site modeling had proven an X0:X1 result.
  auto inferAggregateReturnType = [&]() -> llvm::Type * {
    if (Op.Output.Kind != MedVar::Temp || Op.Output.Size == 0 || !CurMedFunc)
      return nullptr;
    const auto &TRI = getTargetRegInfo(TargetArch);
    std::map<unsigned, llvm::Type *> FieldsByOffset;
    for (const auto &Blk2 : CurMedFunc->Blocks) {
      if (Blk2.Id != BlockId)
        continue;
      for (size_t J = static_cast<size_t>(OpIdx) + 1; J < Blk2.Ops.size();
           ++J) {
        const auto &Ex = Blk2.Ops[J];
        if (Ex.Opcode != NdOp::SUBBYTES || Ex.NumInputs < 2 ||
            Ex.Inputs[0].Kind != MedVar::Temp ||
            Ex.Inputs[0].Id != Op.Output.Id ||
            Ex.Inputs[0].SSAVer != Op.Output.SSAVer ||
            !Ex.Inputs[1].isConst() || Ex.Output.Kind != MedVar::Reg)
          continue;
        unsigned Offset = static_cast<unsigned>(Ex.Inputs[1].ConstVal);
        uint16_t Size = Ex.Output.Size ? Ex.Output.Size : 8;
        if (Offset + Size > Op.Output.Size)
          continue;
        llvm::Type *FieldTy = TRI.isVectorReg(Ex.Output.RegOff)
                                  ? (Size <= 4 ? llvm::Type::getFloatTy(*Ctx)
                                               : llvm::Type::getDoubleTy(*Ctx))
                                  : llvm::Type::getIntNTy(*Ctx, Size * 8);
        FieldsByOffset.emplace(Offset, FieldTy);
      }
      break;
    }
    if (FieldsByOffset.size() < 2)
      return nullptr;
    std::vector<llvm::Type *> FieldTys;
    unsigned ExpectedOffset = 0;
    for (const auto &[Offset, FieldTy] : FieldsByOffset) {
      if (Offset != ExpectedOffset)
        return nullptr;
      FieldTys.push_back(FieldTy);
      ExpectedOffset += FieldTy->getPrimitiveSizeInBits() / 8;
    }
    if (ExpectedOffset != Op.Output.Size)
      return nullptr;
    return llvm::StructType::get(*Ctx, FieldTys);
  };
  llvm::Type *AggregateRetTy = inferAggregateReturnType();
  llvm::Type *DefaultRetTy = AggregateRetTy ? AggregateRetTy : RetTy;

  auto resolveCalleeName = [&]() -> std::string {
    if (CI && !CI->TargetName.empty() &&
        !CI->TargetName.starts_with(kAutoFuncPrefix))
      return canonicalizeObjectCallee(CI->TargetName);
    if (CallAddr != 0) {
      auto FnIt = FuncNames.find(CallAddr);
      if (FnIt != FuncNames.end())
        return canonicalizeObjectCallee(FnIt->second);
    }
    return {};
  };

  if (Op.Opcode == NdOp::CALL && CI && Args.size() >= 3) {
    std::string LibNameStr = resolveCalleeName();
    llvm::StringRef LibName = stripLeadingUnderscores(LibNameStr);
    if (libc::isMemSetName(LibName)) {
      llvm::Value *Dest = Args[0];
      if (Dest->getType()->isIntegerTy()) {
        llvm::Value *Sym =
            (CI && !CI->Args.empty())
                ? tryResolvePointerArg(CI->Args[0],
                                       /*FailClosed=*/true, Builder)
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
      // A pointer-table bind can prove a symbol's external identity before a
      // call proves its ABI.  The mirror represents that unresolved identity
      // as an external i8 global.  Temporarily free the canonical name so the
      // existing call-signature recovery can create the correctly typed
      // Function; once it has, rewrite every table initializer use to the
      // Function and erase the placeholder.
      llvm::GlobalVariable *ImportedPlaceholder = nullptr;
      if (!CalleeName.empty()) {
        auto It = ImportedSymbolPlaceholders.find(CalleeName);
        if (It != ImportedSymbolPlaceholders.end() && It->second &&
            It->second->getParent() == Mod) {
          ImportedPlaceholder = It->second;
          ImportedPlaceholder->setName(CalleeName + ".import_data");
        }
      }

      // On AArch64, variadic and non-variadic functions use different
      // calling conventions (variadic args go on the stack).  We detect
      // known variadic functions by pattern/whitelist, and for any
      // remaining unknown external function we conservatively declare
      // it as variadic (all actual args as fixed + "...") so the
      // backend never puts arguments in the wrong location.
      unsigned NumFixed = 0;
      unsigned NamedNumFixed = 0;
      if (CI && CI->VarArgFixedCount >= 0) {
        NumFixed = static_cast<unsigned>(CI->VarArgFixedCount);
        // A stripped Mach-O has no local `_objc_msgSend$selector` symbol, but
        // the preserved __objc_stubs entry still has a concrete address.
        // Encode that address in the backend's resolvable code-pointer symbol
        // instead of falling through to a non-variadic anonymous stub.
        if (CalleeName.empty())
          CalleeName =
              IsObjCMessageStub
                  ? (kNdCodePtrPrefix + llvm::utohexstr(CallAddr)).str()
                  : (kAutoFuncPrefix + llvm::utohexstr(CallAddr)).str();
      } else if (!CalleeName.empty()) {
        llvm::StringRef Bare = stripLeadingUnderscores(CalleeName);
        NamedNumFixed = libc::varArgFixedCount(Bare);
        NumFixed = NamedNumFixed;
      }

      if (!CalleeName.empty() && NamedNumFixed == 0)
        NamedNumFixed =
            libc::varArgFixedCount(stripLeadingUnderscores(CalleeName));

      bool IsKnownVarArg = (NumFixed > 0);
      const bool IsStructurallyRecoveredVarArg =
          IsKnownVarArg && CI && CI->VarArgFixedCount >= 0 &&
          NamedNumFixed == 0 && !IsObjCMessageStub;

      if (IsKnownVarArg && !CalleeName.empty()) {
        Callee = Mod->getFunction(CalleeName);
        if (!Callee) {
          std::vector<llvm::Type *> FixedTypes;
          for (unsigned I = 0; I < NumFixed && I < Args.size(); ++I) {
            const auto Kind = libc::varArgFixedParamKind(CalleeName, I);
            if (IsStructurallyRecoveredVarArg ||
                Kind == libc::VarArgFixedParamKind::Unknown) {
              FixedTypes.push_back(ArgTypes[I]);
            } else if (Kind == libc::VarArgFixedParamKind::Pointer) {
              FixedTypes.push_back(PtrTy);
            } else {
              FixedTypes.push_back(ArgTypes[I]->isIntegerTy() ? ArgTypes[I]
                                                              : I64Ty);
            }
          }
          llvm::Type *VarRetTy =
              IsObjCMessageStub
                  ? RetTy
                  : (IsStructurallyRecoveredVarArg ? DefaultRetTy : I32Ty);
          auto *VarFT = llvm::FunctionType::get(VarRetTy, FixedTypes, true);
          Callee = llvm::Function::Create(
              VarFT, llvm::GlobalValue::ExternalLinkage, CalleeName, Mod);
          Callee->setCallingConv(llvm::CallingConv::C);
        }
        for (unsigned I = 0;
             !IsStructurallyRecoveredVarArg && I < NumFixed && I < Args.size();
             ++I) {
          if (libc::varArgFixedParamKind(CalleeName, I) ==
                  libc::VarArgFixedParamKind::Pointer &&
              Args[I]->getType()->isIntegerTy()) {
            llvm::Value *Sym =
                (CI && I < CI->Args.size())
                    ? tryResolvePointerArg(CI->Args[I],
                                           /*FailClosed=*/true, Builder)
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
        auto Arity = libc::libcArityForSymbol(CalleeName);
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
        bool IsPlainZeroArg = Arity && Arity->IntArgs == 0 &&
                              Arity->FpArgs == 0 && !Arity->FpRet &&
                              !Arity->FpRetLongDouble && !Arity->FpRetComplex;
        if (IsPlainZeroArg) {
          auto *FT = llvm::FunctionType::get(DefaultRetTy, false);
          Callee = llvm::Function::Create(
              FT, llvm::GlobalValue::ExternalLinkage, CalleeName, Mod);
          Callee->setCallingConv(llvm::CallingConv::C);
        } else if (Arity && ReturnsFp && ArgsModelled) {
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
        } else if (Arity && libc::isVaListConsumer(
                               stripLeadingUnderscores(CalleeName))) {
          std::vector<llvm::Type *> ParamTys(
              static_cast<size_t>(Arity->IntArgs), PtrTy);
          auto *FT = llvm::FunctionType::get(DefaultRetTy, ParamTys, false);
          Callee = llvm::Function::Create(
              FT, llvm::GlobalValue::ExternalLinkage, CalleeName, Mod);
          Callee->setCallingConv(llvm::CallingConv::C);
        }
      }

      if (!Callee && !CalleeName.empty()) {
        bool UseVarArg = getTargetRegInfo(TargetArch).UnknownExternIsVarArg &&
                         AggregateRetTy == nullptr;
        auto *FT = llvm::FunctionType::get(DefaultRetTy, ArgTypes, UseVarArg);
        Callee = llvm::Function::Create(FT, llvm::GlobalValue::ExternalLinkage,
                                        CalleeName, Mod);
        Callee->setCallingConv(llvm::CallingConv::C);
      }

      if (!Callee) {
        auto StubName = (kAutoFuncPrefix + llvm::utohexstr(CallAddr)).str();
        Callee = Mod->getFunction(StubName);
        if (!Callee) {
          auto *StubTy = llvm::FunctionType::get(DefaultRetTy, ArgTypes, false);
          Callee = llvm::Function::Create(
              StubTy, llvm::GlobalValue::ExternalLinkage, StubName, Mod);
          Callee->addFnAttr(llvm::Attribute::NullPointerIsValid);
        }
      }

      if (ImportedPlaceholder) {
        if (Callee) {
          ImportedPlaceholder->replaceAllUsesWith(Callee);
          ImportedPlaceholder->eraseFromParent();
          ImportedSymbolPlaceholders.erase(CalleeName);
        } else {
          // Defensive only: every direct-call path above creates a callee, but
          // leave a valid canonical declaration if a future path declines.
          ImportedPlaceholder->setName(CalleeName);
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
        const auto FixedKind =
            CalleeTy->isVarArg()
                ? libc::varArgFixedParamKind(Callee->getName().str(), AI)
                : libc::VarArgFixedParamKind::Unknown;
        // Symbolize a pointer-width integer arg resolving to a global address
        // when the callee param is itself an integer.  A callee recovered
        // with integer params (e.g. a variadic call whose args were recovered
        // as fixed integer params) never reaches the pointer-param path
        // below, so a computed `&global[i]` would be passed as a stale
        // absolute VA the callee dereferences into unmapped memory.
        // tryResolvePointerArg only fires for a provable data/rodata address,
        // but fixed libc scalars such as snprintf's size can legitimately
        // overlap a low-address relocatable data segment.  The libc signature
        // table keeps those integers scalar.  The width guard keeps other
        // narrow integers untouched; converting back to the integer param keeps
        // a structurally recovered signature valid. Runs before the
        // type-mismatch block so a same-width (i64==i64) pointer arg is
        // symbolized too.
        if (unsigned PtrSz = getTargetRegInfo(TargetArch).PointerSize;
            Want->isIntegerTy() && AV->getType()->isIntegerTy() && CI &&
            AI < CI->Args.size() && PtrSz &&
            FixedKind != libc::VarArgFixedParamKind::Integer &&
            Want->getIntegerBitWidth() == PtrSz * 8)
          if (llvm::Value *Sym = tryResolvePointerArg(
                  CI->Args[AI], /*FailClosed=*/false, Builder))
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
              Sym = tryResolvePointerArg(CI->Args[AI],
                                         /*FailClosed=*/true, Builder);
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
    if (!CalleeTy->isVarArg() && CoercedArgs.size() > CalleeTy->getNumParams())
      CoercedArgs.resize(CalleeTy->getNumParams());
    // A reused variadic declaration still has a mandatory fixed prefix.  An
    // unknown external can be recovered with fewer arguments at a later call
    // site; supply neutral values rather than constructing an invalid call and
    // aborting inside LLVM before the module verifier can report anything.
    while (CoercedArgs.size() < CalleeTy->getNumParams()) {
      auto *PadTy = CalleeTy->getParamType(CoercedArgs.size());
      CoercedArgs.push_back(llvm::Constant::getNullValue(PadTy));
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
    } else if (AggregateRetTy) {
      IRetTy = AggregateRetTy;
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
        if (llvm::Value *Sym = tryResolvePointerArg(
                CI->Args[AI], /*FailClosed=*/false, Builder))
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
  if (auto *Emitted = llvm::dyn_cast_or_null<llvm::CallInst>(Result)) {
    llvm::Metadata *Address = llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), Op.Addr));
    Emitted->setMetadata(language_eh_md::InternalSourceCallAttachment,
                         llvm::MDNode::get(*Ctx, {Address}));
    CallSiteAddrs[Emitted] = Op.Addr;
  }

  if (Op.DoesNotReturn) {
    if (auto *Emitted = llvm::dyn_cast_or_null<llvm::CallBase>(Result))
      Emitted->addFnAttr(llvm::Attribute::NoReturn);
    Builder.CreateUnreachable();
    return;
  }

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

} // namespace neverd
