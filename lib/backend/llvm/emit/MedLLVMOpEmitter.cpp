//===- MedLLVMOpEmitter.cpp - MedIR operation emission ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Core MedIR operation emission: translates individual MedOps to LLVM IR
/// instructions via the main opcode switch.  CALL/INDIR_CALL and RETURN
/// lowering live in MedLLVMCall.cpp and MedLLVMReturn.cpp; intrinsic dispatch
/// lives in MedLLVMIntrinsic.cpp; architecture-specific SIMD/crypto intrinsics
/// in the per-arch emitters under X86/, AArch64/, and ARM/.
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/libc/LibCNames.h"

#define DEBUG_TYPE "neverd-med-llvm-op-emitter"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"

namespace neverd {

namespace {

llvm::AtomicOrdering toLLVMAtomicOrdering(NdMemoryOrdering Ordering) {
  switch (Ordering) {
  case NdMemoryOrdering::None:
    return llvm::AtomicOrdering::NotAtomic;
  case NdMemoryOrdering::Relaxed:
    return llvm::AtomicOrdering::Monotonic;
  case NdMemoryOrdering::Acquire:
    return llvm::AtomicOrdering::Acquire;
  case NdMemoryOrdering::Release:
    return llvm::AtomicOrdering::Release;
  case NdMemoryOrdering::AcquireRelease:
    return llvm::AtomicOrdering::AcquireRelease;
  case NdMemoryOrdering::SequentiallyConsistent:
    return llvm::AtomicOrdering::SequentiallyConsistent;
  }
  llvm_unreachable("unknown NeverD memory ordering");
}

llvm::AtomicOrdering
toLLVMAtomicCmpXchgFailureOrdering(NdMemoryOrdering Ordering) {
  switch (Ordering) {
  case NdMemoryOrdering::Relaxed:
  case NdMemoryOrdering::Release:
    return llvm::AtomicOrdering::Monotonic;
  case NdMemoryOrdering::Acquire:
  case NdMemoryOrdering::AcquireRelease:
    return llvm::AtomicOrdering::Acquire;
  case NdMemoryOrdering::SequentiallyConsistent:
    return llvm::AtomicOrdering::SequentiallyConsistent;
  case NdMemoryOrdering::None:
    break;
  }
  llvm::report_fatal_error("atomic compare-exchange requires memory ordering");
}

} // anonymous namespace

llvm::Value *MedLLVMEmitter::resolveAtomicMemoryPtr(
    const MedVar &AddressVar, uint16_t AccessBytes, llvm::Type *AccessTy,
    llvm::IRBuilder<> &Builder) {
  llvm::Value *Address = nullptr;
  std::optional<uint64_t> ResolvedAddress;
  if (Img) {
    if (AddressVar.isConst())
      ResolvedAddress = AddressVar.ConstVal;
    else
      ResolvedAddress = traceSSAConst(AddressVar);
  }
  if (ResolvedAddress) {
    const unsigned AddressBits = AddressVar.Size > 0 ? AddressVar.Size * 8 : 64;
    if (!isFrameRelativeDisplacement(*ResolvedAddress, AddressBits))
      Address = tryResolveGlobalData(*ResolvedAddress, AccessBytes);
  }
  if (!Address && Img)
    Address = tryResolveWritableData(AddressVar, AccessBytes, Builder);
  if (!Address) {
    rejectEscapingAddressFragment(AddressVar,
                                  "an unresolved atomic memory address");
    Address = getMemoryPtr(getVar(AddressVar, Builder), AccessTy, Builder);
  }
  return Address;
}

void MedLLVMEmitter::emitOp(const MedOp &Op, llvm::IRBuilder<> &Builder,
                            int BlockId, int OpIdx) {
  if (Op.Dead)
    return;

  auto GetInput = [&](uint8_t Idx) -> llvm::Value * {
    if (Idx >= Op.NumInputs)
      return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), 0);
    return getVar(Op.Inputs[Idx], Builder);
  };

  // Some operation contexts prove that a constant is a bit pattern or offset,
  // not a standalone address.  Materialize it without the general data-global
  // symbolization performed by getVar.  A completed address remains eligible
  // for normal symbolization when it is later used by a memory operation.
  auto GetRawInput = [&](uint8_t Idx) -> llvm::Value * {
    if (Idx >= Op.NumInputs)
      return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), 0);
    const MedVar &V = Op.Inputs[Idx];
    if (!V.isConst())
      return getVar(V, Builder);
    auto *IntTy = llvm::cast<llvm::IntegerType>(sizeToType(V.Size));
    return llvm::ConstantInt::get(
        *Ctx, llvm::APInt(IntTy->getBitWidth(), V.ConstVal,
                          /*isSigned=*/false, /*implicitTrunc=*/true));
  };

  // Integer relations and arithmetic must consume the same relocatable code
  // identity that observable value sinks use.  Keep this occurrence-scoped:
  // an architectural PC/interior label used as a switch arithmetic base has
  // no FuncNames identity and therefore stays with its established numeric
  // owner, while an ADR/LEA of an emitted function becomes ptrtoint @func.
  auto GetRelocatableIdentityInput = [&](uint8_t Idx) -> llvm::Value * {
    if (Idx >= Op.NumInputs)
      return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), 0);
    if (llvm::Value *Code =
            tryResolveCodeIdentityOperand(Op.Inputs[Idx], Builder))
      return Code;
    return GetInput(Idx);
  };

  auto Coerce = [&](llvm::Value *A,
                    llvm::Value *B) -> std::pair<llvm::Value *, llvm::Value *> {
    if (A->getType() == B->getType())
      return {A, B};
    if (A->getType()->isPointerTy())
      A = Builder.CreatePtrToInt(A, llvm::Type::getInt64Ty(*Ctx));
    if (B->getType()->isPointerTy())
      B = Builder.CreatePtrToInt(B, llvm::Type::getInt64Ty(*Ctx));
    if (A->getType() != B->getType()) {
      unsigned ABits = A->getType()->getIntegerBitWidth();
      unsigned BBits = B->getType()->getIntegerBitWidth();
      if (ABits > BBits)
        B = Builder.CreateZExt(B, A->getType());
      else
        A = Builder.CreateZExt(A, B->getType());
    }
    return {A, B};
  };

  // AArch64/ARM SDIV/UDIV are non-trapping: divide-by-zero yields 0 and the
  // signed INT_MIN / -1 overflow wraps to INT_MIN.  LLVM's div/rem make both
  // undefined (poison), so guard the divisor to keep the IR well-defined.  The
  // guard folds away when the divisor is provably non-zero; x86 div faults on a
  // zero divisor, so it is emitted raw.
  auto GuardDivRem = [&](llvm::Value *L, llvm::Value *R, bool IsSigned,
                         bool IsRem) -> llvm::Value * {
    auto Raw = [&](llvm::Value *Den) -> llvm::Value * {
      if (IsRem)
        return IsSigned ? Builder.CreateSRem(L, Den, "srem")
                        : Builder.CreateURem(L, Den, "urem");
      return IsSigned ? Builder.CreateSDiv(L, Den, "sdiv")
                      : Builder.CreateUDiv(L, Den, "udiv");
    };
    bool NonTrapping = (TargetArch == Arch::AArch64 || TargetArch == Arch::ARM);
    if (!NonTrapping || !L->getType()->isIntegerTy())
      return Raw(R);
    auto *Ty = L->getType();
    auto *Zero = llvm::ConstantInt::get(Ty, 0);
    auto *One = llvm::ConstantInt::get(Ty, 1);
    auto *IsZero = Builder.CreateICmpEQ(R, Zero, "divz");
    llvm::Value *NeedSafe = IsZero;
    if (IsSigned) {
      auto *IntMin = llvm::ConstantInt::get(
          Ty, llvm::APInt::getSignedMinValue(Ty->getIntegerBitWidth()));
      auto *IsOvf = Builder.CreateAnd(
          Builder.CreateICmpEQ(L, IntMin),
          Builder.CreateICmpEQ(R, llvm::ConstantInt::getSigned(Ty, -1)));
      NeedSafe = Builder.CreateOr(IsZero, IsOvf, "divguard");
    }
    auto *SafeR = Builder.CreateSelect(NeedSafe, One, R, "divsafe");
    return Builder.CreateSelect(IsZero, Zero, Raw(SafeR), "divres");
  };

  llvm::Value *Result = nullptr;

  switch (Op.Opcode) {
  case NdOp::COPY: {
    // A direct occurrence that names an emitted function is unambiguous even
    // before a later use supplies a code role. Materialize it once at the COPY
    // so PHI/SELECT transports and no-opt IR cannot retain a dead original VA.
    // Keep executable interior addresses on the ordinary path: architectural
    // PC/switch arithmetic needs use-role analysis before choosing
    // BlockAddress versus numeric layout semantics.
    const MedVar &Input = Op.Inputs[0];
    llvm::Function *KnownFunction = nullptr;
    if (Input.isConst() && isExactAddressProvenance(Input.Provenance)) {
      const va_t Normalized =
          normalizeCodeAddress(Input.ConstVal, Img ? Img->Arch : TargetArch,
                               Img ? Img->Mode : InstructionMode::Default);
      if (auto It = FuncNames.find(Normalized); It != FuncNames.end())
        KnownFunction = Mod->getFunction(It->second);
    }
    Result = KnownFunction
                 ? Builder.CreatePtrToInt(KnownFunction,
                                          sizeToType(Op.Output.Size),
                                          "code.copy")
                 : GetInput(0);
    break;
  }
  case NdOp::INT_ADD: {
    if (auto *Dyn = tryResolveDynVlaAddr(Op, Builder)) {
      Result = Dyn;
      break;
    }
    // ARM materializes a function pointer as `add rN, pc, ldr[pc]` (literal
    // pool); the LOAD keeps this INT_ADD from folding to a constant, so resolve
    // it to `ptrtoint @func` here.  Other arches fold to a constant the getVar
    // path symbolizes, so this trace is confined to ARM to avoid the cost.
    if (TargetArch == Arch::ARM) {
      if (auto *FP = tryResolveCodeRefValue(Op.Output, Builder)) {
        Result = FP;
        break;
      }
    }
    // A constant added to a frame-derived value is a stack displacement.  Its
    // numeric value may coincide with a low-VA data relocation (notably i386
    // array offsets such as 0x100); symbolizing that operand would turn
    // `sp + index + offset` into `sp + index + &global`.
    bool RawL = Op.NumInputs > 1 && Op.Inputs[0].isConst() &&
                varIsFrameDerived(Op.Inputs[1]);
    bool RawR = Op.NumInputs > 1 && Op.Inputs[1].isConst() &&
                varIsFrameDerived(Op.Inputs[0]);
    auto AddInput = [&](uint8_t Idx) -> llvm::Value * {
      return ((Idx == 0 && RawL) || (Idx == 1 && RawR))
                 ? GetRawInput(Idx)
                 : GetRelocatableIdentityInput(Idx);
    };
    if (TargetArch == Arch::AArch64 && Op.Output.Size > 0 &&
        Op.Output.Size <= 4) {
      auto *Ty = sizeToType(Op.Output.Size);
      llvm::Value *L = AddInput(0);
      llvm::Value *R = AddInput(1);
      if (L->getType() != Ty)
        L = L->getType()->getIntegerBitWidth() > Ty->getIntegerBitWidth()
                ? Builder.CreateTrunc(L, Ty)
                : Builder.CreateZExt(L, Ty);
      if (R->getType() != Ty)
        R = R->getType()->getIntegerBitWidth() > Ty->getIntegerBitWidth()
                ? Builder.CreateTrunc(R, Ty)
                : Builder.CreateZExt(R, Ty);
      Result = Builder.CreateAdd(L, R, "add");
    } else {
      auto [L, R] = Coerce(AddInput(0), AddInput(1));
      Result = Builder.CreateAdd(L, R, "add");
    }
    break;
  }
  case NdOp::INT_SUB: {
    if (auto *Dyn = tryEmitDynamicStackAlloc(Op, Builder)) {
      Result = Dyn;
      break;
    }
    if (auto *Dyn = tryResolveDynVlaAddr(Op, Builder)) {
      Result = Dyn;
      break;
    }
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    Result = Builder.CreateSub(L, R, "sub");
    break;
  }
  case NdOp::INT_MULT: {
    if (TargetArch == Arch::AArch64 && Op.Output.Size > 0 &&
        Op.Output.Size <= 4) {
      auto *Ty = sizeToType(Op.Output.Size);
      llvm::Value *L = GetRelocatableIdentityInput(0);
      llvm::Value *R = GetRelocatableIdentityInput(1);
      if (L->getType() != Ty)
        L = L->getType()->getIntegerBitWidth() > Ty->getIntegerBitWidth()
                ? Builder.CreateTrunc(L, Ty)
                : Builder.CreateZExt(L, Ty);
      if (R->getType() != Ty)
        R = R->getType()->getIntegerBitWidth() > Ty->getIntegerBitWidth()
                ? Builder.CreateTrunc(R, Ty)
                : Builder.CreateZExt(R, Ty);
      Result = Builder.CreateMul(L, R, "mul");
    } else {
      auto [L, R] = Coerce(GetRelocatableIdentityInput(0),
                           GetRelocatableIdentityInput(1));
      Result = Builder.CreateMul(L, R, "mul");
    }
    break;
  }
  case NdOp::INT_DIV: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    Result = emitX86WideDivRem(Builder, L, R, /*IsSigned=*/false,
                               /*WantRem=*/false);
    if (!Result)
      Result = GuardDivRem(L, R, /*IsSigned=*/false, /*IsRem=*/false);
    break;
  }
  case NdOp::INT_SDIV: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    Result = emitX86WideDivRem(Builder, L, R, /*IsSigned=*/true,
                               /*WantRem=*/false);
    if (!Result)
      Result = GuardDivRem(L, R, /*IsSigned=*/true, /*IsRem=*/false);
    break;
  }
  case NdOp::INT_REM: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    Result = emitX86WideDivRem(Builder, L, R, /*IsSigned=*/false,
                               /*WantRem=*/true);
    if (!Result)
      Result = GuardDivRem(L, R, /*IsSigned=*/false, /*IsRem=*/true);
    break;
  }
  case NdOp::INT_SREM: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    Result = emitX86WideDivRem(Builder, L, R, /*IsSigned=*/true,
                               /*WantRem=*/true);
    if (!Result)
      Result = GuardDivRem(L, R, /*IsSigned=*/true, /*IsRem=*/true);
    break;
  }
  case NdOp::INT_AND: {
    auto *L = GetRelocatableIdentityInput(0);
    auto *R = GetRelocatableIdentityInput(1);
    // Fix i128 AND masks created from 64-bit NdVar::cst: sign-extend
    // negative constants so bitmasks like 0xFFFFFFFFFFFF0000 become
    // 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFF0000 instead of being zero-extended.
    auto MaybeSExtMask = [&](llvm::Value *V) -> llvm::Value * {
      auto *CI = llvm::dyn_cast<llvm::ConstantInt>(V);
      if (!CI)
        return V;
      unsigned Bits = CI->getType()->getIntegerBitWidth();
      if (Bits <= 64)
        return V;
      const llvm::APInt &Val = CI->getValue();
      if (Val.getActiveBits() <= 64 && Val.trunc(64).isNegative()) {
        llvm::APInt SExt = Val.trunc(64).sext(Bits);
        return llvm::ConstantInt::get(CI->getType(), SExt);
      }
      return V;
    };
    L = MaybeSExtMask(L);
    R = MaybeSExtMask(R);
    if (L->getType() != R->getType()) {
      auto [Lc, Rc] = Coerce(L, R);
      L = Lc;
      R = Rc;
    }
    Result = Builder.CreateAnd(L, R, "and");
    break;
  }
  case NdOp::INT_OR: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    Result = Builder.CreateOr(L, R, "or");
    break;
  }
  case NdOp::INT_XOR: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    Result = Builder.CreateXor(L, R, "xor");
    break;
  }
  case NdOp::INT_LEFT: {
    // Shift operands are bit patterns, but an explicitly relocatable code
    // occurrence must first become ptrtoint(function/blockaddress).  The
    // occurrence-aware helper leaves ordinary ARM MOVT immediates raw even
    // when their numeric value collides with a relocation VA.
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    unsigned Bits = L->getType()->getIntegerBitWidth();
    auto *Zero = llvm::ConstantInt::get(L->getType(), 0);
    auto *Limit = llvm::ConstantInt::get(R->getType(), Bits);
    auto *Safe = Builder.CreateICmpULT(R, Limit);
    auto *Shifted = Builder.CreateShl(L, Builder.CreateURem(R, Limit), "shl");
    Result = Builder.CreateSelect(Safe, Shifted, Zero);
    break;
  }
  case NdOp::INT_RIGHT: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    unsigned Bits = L->getType()->getIntegerBitWidth();
    auto *Zero = llvm::ConstantInt::get(L->getType(), 0);
    auto *Limit = llvm::ConstantInt::get(R->getType(), Bits);
    auto *Safe = Builder.CreateICmpULT(R, Limit);
    auto *Shifted = Builder.CreateLShr(L, Builder.CreateURem(R, Limit), "lshr");
    Result = Builder.CreateSelect(Safe, Shifted, Zero);
    break;
  }
  case NdOp::INT_ASHR: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    unsigned Bits = L->getType()->getIntegerBitWidth();
    auto *MaxShift = llvm::ConstantInt::get(R->getType(), Bits - 1);
    auto *Limit = llvm::ConstantInt::get(R->getType(), Bits);
    auto *Safe = Builder.CreateICmpULT(R, Limit);
    auto *ClampedR = Builder.CreateSelect(Safe, R, MaxShift);
    Result = Builder.CreateAShr(L, ClampedR, "ashr");
    break;
  }
  case NdOp::INT_EQUAL: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    Result = Builder.CreateZExt(Builder.CreateICmpEQ(L, R, "eq"),
                                llvm::Type::getInt8Ty(*Ctx));
    break;
  }
  case NdOp::INT_NOTEQUAL: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    Result = Builder.CreateZExt(Builder.CreateICmpNE(L, R, "ne"),
                                llvm::Type::getInt8Ty(*Ctx));
    break;
  }
  case NdOp::INT_LESS: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    Result = Builder.CreateZExt(Builder.CreateICmpULT(L, R, "ult"),
                                llvm::Type::getInt8Ty(*Ctx));
    break;
  }
  case NdOp::INT_SLESS: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    Result = Builder.CreateZExt(Builder.CreateICmpSLT(L, R, "slt"),
                                llvm::Type::getInt8Ty(*Ctx));
    break;
  }
  case NdOp::INT_LESSEQUAL: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    Result = Builder.CreateZExt(Builder.CreateICmpULE(L, R, "ule"),
                                llvm::Type::getInt8Ty(*Ctx));
    break;
  }
  case NdOp::INT_SLESSEQUAL: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    Result = Builder.CreateZExt(Builder.CreateICmpSLE(L, R, "sle"),
                                llvm::Type::getInt8Ty(*Ctx));
    break;
  }
  case NdOp::INT_CARRY: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    auto *Sum = Builder.CreateAdd(L, R, "uadd");
    Result = Builder.CreateZExt(Builder.CreateICmpULT(Sum, L, "carry"),
                                llvm::Type::getInt8Ty(*Ctx));
    break;
  }
  case NdOp::INT_SOVF: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    auto *Sum = Builder.CreateAdd(L, R);
    auto *XorAB = Builder.CreateXor(L, R);
    auto *XorSA = Builder.CreateXor(Sum, L);
    auto *NxorAB = Builder.CreateNot(XorAB);
    auto *OvBits = Builder.CreateAnd(NxorAB, XorSA);
    auto *SignBit = Builder.CreateLShr(
        OvBits,
        llvm::ConstantInt::get(OvBits->getType(),
                               OvBits->getType()->getIntegerBitWidth() - 1));
    Result = Builder.CreateZExt(
        Builder.CreateTrunc(SignBit, llvm::Type::getInt1Ty(*Ctx)),
        llvm::Type::getInt8Ty(*Ctx), "scarry");
    break;
  }
  case NdOp::INT_SBOR: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    auto *Diff = Builder.CreateSub(L, R);
    auto *XorAB = Builder.CreateXor(L, R);
    auto *XorDA = Builder.CreateXor(Diff, L);
    auto *OvBits = Builder.CreateAnd(XorAB, XorDA);
    auto *SignBit = Builder.CreateLShr(
        OvBits,
        llvm::ConstantInt::get(OvBits->getType(),
                               OvBits->getType()->getIntegerBitWidth() - 1));
    Result = Builder.CreateZExt(
        Builder.CreateTrunc(SignBit, llvm::Type::getInt1Ty(*Ctx)),
        llvm::Type::getInt8Ty(*Ctx), "sborrow");
    break;
  }
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT: {
    Result = Builder.CreateNot(GetRelocatableIdentityInput(0), "not");
    break;
  }
  case NdOp::INT_NEG2: {
    Result = Builder.CreateNeg(GetRelocatableIdentityInput(0), "neg");
    break;
  }
  case NdOp::BOOL_AND: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    Result = Builder.CreateAnd(L, R, "band");
    break;
  }
  case NdOp::BOOL_OR: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    Result = Builder.CreateOr(L, R, "bor");
    break;
  }
  case NdOp::BOOL_XOR: {
    auto [L, R] =
        Coerce(GetRelocatableIdentityInput(0), GetRelocatableIdentityInput(1));
    Result = Builder.CreateXor(L, R, "bxor");
    break;
  }
  case NdOp::BOOL_NOT: {
    auto *Operand = GetRelocatableIdentityInput(0);
    auto *Zero = llvm::ConstantInt::get(Operand->getType(), 0);
    auto *IsZero = Builder.CreateICmpEQ(Operand, Zero, "bnot");
    Result = Builder.CreateZExt(IsZero, Operand->getType());
    break;
  }
  case NdOp::INT_ZEXT: {
    auto *Operand = GetRelocatableIdentityInput(0);
    auto *DstTy = sizeToType(Op.Output.Size);
    unsigned SrcW = Operand->getType()->isIntegerTy()
                        ? Operand->getType()->getIntegerBitWidth()
                        : 64;
    unsigned DstW = DstTy->isIntegerTy() ? DstTy->getIntegerBitWidth() : 64;
    if (SrcW < DstW)
      Result = Builder.CreateZExt(Operand, DstTy, "zext");
    else if (SrcW > DstW)
      Result = Builder.CreateTrunc(Operand, DstTy, "ztrunc");
    else
      Result = Operand;
    break;
  }
  case NdOp::INT_SEXT: {
    auto *Operand = GetRelocatableIdentityInput(0);
    auto *DstTy = sizeToType(Op.Output.Size);
    unsigned SrcW = Operand->getType()->isIntegerTy()
                        ? Operand->getType()->getIntegerBitWidth()
                        : 64;
    unsigned DstW = DstTy->isIntegerTy() ? DstTy->getIntegerBitWidth() : 64;
    if (SrcW < DstW)
      Result = Builder.CreateSExt(Operand, DstTy, "sext");
    else if (SrcW > DstW)
      Result = Builder.CreateTrunc(Operand, DstTy, "strunc");
    else
      Result = Operand;
    break;
  }
  case NdOp::FLOAT_TRUNC: {
    Result = emitFloatOp(Op, Builder);
    break;
  }
  case NdOp::CONCAT: {
    // A SIMD broadcast/pack of an ARM32 PC-relative literal-pool address-of a
    // WRITABLE global (clang vectorizes `tab[i] = &G` into a `vdup`/`pshufd`
    // broadcast of &A/&B packed into one wide store — the ptab512 shape).  The
    // per-lane scalar pointer is packed into a vector lane here, so the wide
    // store can no longer be symbolized as a single pointer and the scalar
    // never reaches the store-value resolver: symbolize it to the recompiled @G
    // now. Scoped to CONCAT (vector / register-pair construction) so a scalar
    // address-of used as an ordinary address keeps its existing re-based
    // handling — symbolizing it in getVar instead double-relocates those uses.
    auto LaneInput = [&](uint8_t Idx) -> llvm::Value * {
      if (Idx < Op.NumInputs)
        if (llvm::Value *Code =
                tryResolveCodeIdentityOperand(Op.Inputs[Idx], Builder))
          return Code;
      if (Idx < Op.NumInputs && TargetArch == Arch::ARM && Img && CurMedFunc &&
          !Op.Inputs[Idx].isConst()) {
        bool SawLoad = false;
        if (auto VA = traceTableBaseConst(Op.Inputs[Idx], 0, &SawLoad))
          if (SawLoad && *VA != 0 &&
              symbolizesWritableRelocPtr(*VA, Op.Inputs[Idx].Size))
            if (auto *G = tryResolveGlobalData(*VA))
              return Builder.CreatePtrToInt(G, sizeToType(Op.Inputs[Idx].Size),
                                            "wlitlane");
      }
      return GetInput(Idx);
    };
    auto *Hi = LaneInput(0);
    auto *Lo = LaneInput(1);
    unsigned OutBits = Op.Output.Size * 8;
    if (OutBits == 0)
      OutBits = 64;
    unsigned HiBits =
        Hi->getType()->isIntegerTy() ? Hi->getType()->getIntegerBitWidth() : 64;
    unsigned LoBits =
        Lo->getType()->isIntegerTy() ? Lo->getType()->getIntegerBitWidth() : 64;
    if (OutBits < HiBits + LoBits)
      OutBits = HiBits + LoBits;
    auto *WideTy = llvm::IntegerType::get(*Ctx, OutBits);
    auto *HiExt = (HiBits < OutBits) ? Builder.CreateZExt(Hi, WideTy)
                                     : Builder.CreateTrunc(Hi, WideTy);
    auto *LoExt = (LoBits < OutBits) ? Builder.CreateZExt(Lo, WideTy)
                                     : Builder.CreateTrunc(Lo, WideTy);
    auto *HiShifted =
        Builder.CreateShl(HiExt, llvm::ConstantInt::get(WideTy, LoBits));
    Result = Builder.CreateOr(HiShifted, LoExt, "piece");
    if (Op.Output.Size > 0) {
      auto *FinalTy = sizeToType(Op.Output.Size);
      if (Result->getType() != FinalTy) {
        if (OutBits > Op.Output.Size * 8u)
          Result = Builder.CreateTrunc(Result, FinalTy);
        else
          Result = Builder.CreateZExt(Result, FinalTy);
      }
    }
    break;
  }
  case NdOp::SUBBYTES: {
    auto *Src = GetRelocatableIdentityInput(0);
    if (Op.Output.Size == 0) {
      Result = Src;
      break;
    }
    auto *OffVal = GetInput(1);
    uint64_t ByteOff = 0;
    if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(OffVal))
      ByteOff = CI->getZExtValue();

    // Keep aligned x86 SIMD subpieces in their exact element type.  Expressing
    // the operation as an integer shift and truncate allows combines to widen
    // the extract across adjacent lanes before it reaches instruction
    // selection.  A vector extract preserves the byte-slice boundary while
    // remaining a bit-identical operation on the supported little-endian x86
    // targets.
    unsigned SrcBits = Src->getType()->getIntegerBitWidth();
    unsigned OutBits = Op.Output.Size * 8;
    bool IsX86 = TargetArch == Arch::X86 || TargetArch == Arch::X64;
    uint64_t SrcBytes = SrcBits / 8;
    if (IsX86 && SrcBits >= 128 && OutBits != 0 && OutBits <= 64 &&
        SrcBits % OutBits == 0 && ByteOff % Op.Output.Size == 0 &&
        ByteOff <= SrcBytes && Op.Output.Size <= SrcBytes - ByteOff) {
      auto *ElemTy = llvm::IntegerType::get(*Ctx, OutBits);
      auto *VecTy = llvm::FixedVectorType::get(ElemTy, SrcBits / OutBits);
      auto *Vec = Builder.CreateBitCast(Src, VecTy, "subbytes.vec");
      Result = Builder.CreateExtractElement(
          Vec, Builder.getInt64(ByteOff / Op.Output.Size), "subbytes.lane");
      break;
    }

    if (ByteOff != 0) {
      unsigned ShiftBits = static_cast<unsigned>(ByteOff * 8);
      if (ShiftBits >= SrcBits) {
        Src = llvm::ConstantInt::get(Src->getType(), 0);
      } else {
        Src = Builder.CreateLShr(
            Src, llvm::ConstantInt::get(Src->getType(), ShiftBits));
      }
    }
    if (OutBits < Src->getType()->getIntegerBitWidth())
      Src = Builder.CreateTrunc(Src, llvm::IntegerType::get(*Ctx, OutBits));
    Result = Src;
    break;
  }
  case NdOp::POPCOUNT: {
    auto *Operand = GetRelocatableIdentityInput(0);
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::ctpop, {Operand->getType()});
    Result = Builder.CreateCall(Fn, {Operand}, "popcount");
    auto *OutTy = sizeToType(Op.Output.Size);
    if (Result->getType() != OutTy)
      Result = Builder.CreateTrunc(Result, OutTy);
    break;
  }
  case NdOp::LZCOUNT: {
    auto *Operand = GetRelocatableIdentityInput(0);
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::ctlz, {Operand->getType()});
    Result = Builder.CreateCall(
        Fn, {Operand, llvm::ConstantInt::getFalse(*Ctx)}, "lzcnt");
    auto *OutTy = sizeToType(Op.Output.Size);
    if (Result->getType() != OutTy)
      Result = Builder.CreateTrunc(Result, OutTy);
    break;
  }
  case NdOp::FLOAT_ADD:
  case NdOp::FLOAT_SUB:
  case NdOp::FLOAT_MULT:
  case NdOp::FLOAT_DIV:
  case NdOp::FLOAT_FMA:
  case NdOp::FLOAT_NEG:
  case NdOp::FLOAT_ABS:
  case NdOp::FLOAT_SQRT:
  case NdOp::FLOAT_CEIL:
  case NdOp::FLOAT_FLOOR:
  case NdOp::FLOAT_ROUND:
  case NdOp::FLOAT_ROUNDEVEN:
  case NdOp::FLOAT_MIN:
  case NdOp::FLOAT_MAX:
  case NdOp::FLOAT_MINNUM:
  case NdOp::FLOAT_MAXNUM:
  case NdOp::FLOAT_EQUAL:
  case NdOp::FLOAT_NOTEQUAL:
  case NdOp::FLOAT_LESS:
  case NdOp::FLOAT_LESSEQUAL:
  case NdOp::FLOAT_ISNAN:
  case NdOp::FLOAT_FLOAT2INT:
  case NdOp::FLOAT_FLOAT2UINT:
  case NdOp::FLOAT_INT2FLOAT:
  case NdOp::FLOAT_UINT2FLOAT:
  case NdOp::FLOAT_FLOAT2FLOAT: {
    Result = emitFloatOp(Op, Builder);
    break;
  }
  case NdOp::LOAD: {
    auto *ValTy = sizeToType(Op.Output.Size);
    llvm::Value *Ptr = nullptr;
    const MedVar &AddrVar = Op.Inputs[0];
    uint64_t ResolvedAddr = 0;
    if (Op.NumInputs >= 1 && Img) {
      if (AddrVar.isConst()) {
        ResolvedAddr = AddrVar.ConstVal;
      } else if (auto Traced = traceSSAConst(AddrVar)) {
        ResolvedAddr = *Traced;
      }
    }
    if (ResolvedAddr != 0) {
      unsigned AddrBits = AddrVar.Size > 0 ? AddrVar.Size * 8 : 64;
      if (!isFrameRelativeDisplacement(ResolvedAddr, AddrBits)) {
        uint16_t Hint = Op.Output.Size;
        Ptr = tryResolveGlobalData(ResolvedAddr, Hint);
      }
    }
    bool IsGlobalData = (Ptr != nullptr);
    if (!Ptr && Img && Op.NumInputs >= 1) {
      // Runtime-indexed access into a writable .data / .bss segment: redirect
      // into the cohesive mutable global so it aliases the direct (constant)
      // loads/stores of the same global.  Checked first as it is the only path
      // that claims writable data (the rodata resolvers below reject it).
      Ptr = tryResolveWritableData(AddrVar, Op.Output.Size, Builder);
      if (!Ptr)
        // Function-pointer table dispatch: addr = code_ptr_table + idx*ptrsize.
        // Checked next so a `.data.rel.ro` code-pointer table is rebuilt as a
        // `ptrtoint @func` array instead of falling into the raw-byte data
        // path.
        Ptr = tryResolveCodePtrTablePtr(AddrVar, Builder);
      if (!Ptr)
        // Immutable-data resolver arbitration is shared with pointer
        // arguments: the generic all-arms audit defers a pure recurrent PHI to
        // the induction owner, while ambiguous provenance fails closed before
        // any narrower indexed/literal matcher can claim it.
        Ptr = tryResolveReadOnlyDataPtr(AddrVar, Op.Output.Size,
                                        /*FailClosed=*/true, Builder);
      if (Ptr)
        IsGlobalData = true;
    }
    if (!Ptr) {
      rejectEscapingAddressFragment(AddrVar, "an unresolved load address");
      auto *Addr = GetInput(0);
      Ptr = getMemoryPtr(Addr, ValTy, Builder);
    }
    auto *LI = Builder.CreateLoad(ValTy, Ptr, "ld");
    if (Op.MemoryOrdering != NdMemoryOrdering::None) {
      if (Op.MemoryOrdering == NdMemoryOrdering::Release ||
          Op.MemoryOrdering == NdMemoryOrdering::AcquireRelease)
        llvm::report_fatal_error("release ordering is invalid on a load");
      if (!llvm::isPowerOf2_64(Op.Output.Size))
        llvm::report_fatal_error("atomic load size must be a power of two");
      LI->setAlignment(llvm::Align(Op.Output.Size));
      LI->setAtomic(toLLVMAtomicOrdering(Op.MemoryOrdering));
    }
    if (IsGlobalData)
      LI->setVolatile(true);
    Result = LI;
    break;
  }
  case NdOp::STORE: {
    auto *Val = GetInput(1);
    llvm::Value *Ptr = nullptr;
    const MedVar &AddrVar = Op.Inputs[0];
    uint64_t ResolvedAddr = 0;
    if (Op.NumInputs >= 1 && Img) {
      if (AddrVar.isConst()) {
        ResolvedAddr = AddrVar.ConstVal;
      } else if (auto Traced = traceSSAConst(AddrVar)) {
        ResolvedAddr = *Traced;
      }
    }
    if (ResolvedAddr != 0) {
      unsigned AddrBits = AddrVar.Size > 0 ? AddrVar.Size * 8 : 64;
      if (!isFrameRelativeDisplacement(ResolvedAddr, AddrBits)) {
        uint16_t Hint = 0;
        if (Val->getType()->isSized()) {
          uint64_t Bits =
              Mod->getDataLayout().getTypeSizeInBits(Val->getType());
          if (Bits > 0 && Bits <= 64 && (Bits % 8) == 0)
            Hint = static_cast<uint16_t>(Bits / 8);
        }
        Ptr = tryResolveGlobalData(ResolvedAddr, Hint);
      }
    }
    bool IsGlobalData = (Ptr != nullptr);
    if (!Ptr && Img && Op.NumInputs >= 1) {
      // Runtime-indexed store into a writable .data / .bss segment: redirect
      // into the cohesive mutable global so it aliases that global's loads.
      Ptr = tryResolveWritableData(AddrVar, AddrVar.Size, Builder);
      if (!Ptr)
        // Store into a writable function-pointer table (`ft[i] = &f`): use the
        // SAME base+index resolver the LOAD uses.  Its Path 1 isolates the RAW
        // table base const and forms `GEP(@codeptr, (Base - segVA) + index)`,
        // getVar-ing only the runtime index terms.  This MUST precede
        // tryResolveCodePtrSegPtr, whose `getVar(addr) - segVA` rebase double-
        // applies the base when getVar has already symbolized the table base
        // const to `@codeptr` — yielding `@codeptr + (@codeptr + idx - segVA)`
        // (the fptab store, whose load was already correct via this path).
        Ptr = tryResolveCodePtrTablePtr(AddrVar, Builder);
      if (!Ptr)
        // Store into a writable function-pointer global (.data slot holding a
        // code pointer): route to the relocated code-pointer segment mirror.
        Ptr = tryResolveCodePtrSegPtr(AddrVar, Builder);
      if (Ptr)
        IsGlobalData = true;
    }
    if (!Ptr) {
      rejectEscapingAddressFragment(AddrVar, "an unresolved store address");
      auto *Addr = GetInput(0);
      Ptr = getMemoryPtr(Addr, Val->getType(), Builder);
    }
    // A stored VALUE that is itself a writable-global address (`*pp = &g`, a
    // global pointer array `gp[i] = A`, a global function pointer) must be
    // symbolized, or the pointer written to memory is a stale absolute VA the
    // later dereference misses — a value read back through a global/heap
    // pointer is never re-symbolized at its use (the resolver stops at the
    // LOAD).  A spill to a STACK slot of a global base the consumer
    // re-symbolizes via `GEP(@run, val - segVA)` must keep the RAW VA
    // (symbolizing it would double- relocate), so a frame-derived store is
    // normally excluded — UNLESS the slot is read only through a runtime index
    // (a local pointer array `t[k]={A,B}` whose indexed load key never matches
    // the constant store key), where the use is NOT re-symbolized and the slot
    // must therefore hold the recompiled pointer.  frameSlotHasMatchingKeyLoad
    // distinguishes the two (the dual of the #482 store-to-load forward).  Only
    // a pointer-width value and only the conservative writable-data resolver
    // are considered (an ordinary integer store is never an address; a code
    // pointer is already symbolized at source).
    const bool IsLocalValueTransport =
        Op.NumInputs >= 2 &&
        addressFragmentCanStayInLocalFrameSlot(Op.Inputs[0], Op.Inputs[1].Size);
    if (Op.NumInputs >= 2 && !IsLocalValueTransport)
      if (llvm::Value *Code = tryResolveCodeAddressValue(
              Op.Inputs[1], /*RequireCodeRole=*/false, Builder))
        Val = Code;
    const ConstantProvenanceSummary StoredOccurrence =
        Op.NumInputs >= 2 ? summarizeConstantProvenance(Op.Inputs[1])
                          : ConstantProvenanceSummary{};
    if (Op.NumInputs >= 2 && !IsLocalValueTransport)
      rejectEscapingAddressFragment(Op.Inputs[1], "a stored value");
    if (Img && Op.NumInputs >= 2 && Val && Val->getType()->isIntegerTy() &&
        !StoredOccurrence.hasExplicitProvenance()) {
      const bool HasMatchingSlotReload =
          frameSlotHasMatchingKeyLoad(Op.Inputs[0]);
      bool FrameReSymbolized =
          varIsFrameDerived(Op.Inputs[0]) && HasMatchingSlotReload;
      unsigned PtrSz = getTargetRegInfo(TargetArch).PointerSize;
      llvm::Value *Sym = nullptr;
      if (!FrameReSymbolized && PtrSz && Op.Inputs[1].Size == PtrSz) {
        Sym = tryResolveWritableData(Op.Inputs[1], Op.Inputs[1].Size, Builder,
                                     /*IsValueOperand=*/true);
        // The stored value may be a pointer to a read-only pointer TABLE — a
        // vtable / const dispatch-table base (`obj->vt = &VT`).  Its slots are
        // relocated by the code-pointer mirror, so a raw store leaves the later
        // `vt->m()` dereference reading the stale absolute VA (unmapped).  The
        // writable-data resolver above only covers .data/.bss, so symbolize a
        // const-table base whose segment carries relocated pointer slots
        // through tryResolveGlobalData (which routes it to the mirror).  A
        // frame-derived base the consumer re-symbolizes is already excluded
        // above.
        if (!Sym)
          if (auto C = traceSSAConst(Op.Inputs[1])) {
            // A vtable / dispatch-table base lives in read-only-after-reloc
            // DATA
            // (`.data.rel.ro` / `.rodata`), never in executable code.  An
            // integer value that merely equals a low .text VA — an i386 PIC
            // get-PC seed
            // (`pop %eax` after `call .+0` lifts to the constant next-PC) or a
            // small literal accumulator seed (`long long sum = 1`) — lands in
            // the executable segment ONLY because that segment carries a GOTOFF
            // function-pointer reloc slot, which makes segHasPtrRelocSlots true
            // for .text.  Such a value is a plain integer, not a data pointer,
            // so never embed the code bytes as a fabricated data global (the
            // #456/ #459/#499 "small constant collides with a reloc-target VA"
            // family).
            const Segment *VSeg = Img->getSegmentFor(*C);
            if (VSeg && !VSeg->isExecutable() && segHasPtrRelocSlots(VSeg))
              Sym = tryResolveGlobalData(*C, Op.Inputs[1].Size);
          }
        // The i386 PIC form `GOT_base + slot@GOTOFF` of a code-pointer-table
        // base (a vtable / dispatch-table address `obj->vt = &VT`) does not
        // fold to a constant, so traceSSAConst above misses it.  Resolve it
        // through the segment walk tryResolveCodePtrSegPtr uses — the same
        // resolver the STORE-address path already applies for storing INTO such
        // a segment — so the stored vtable pointer is the recompiled mirror,
        // not a stale VA the later `vt->m()` indirect call would fetch from
        // unmapped memory.
        // A fixed stack-slot reload will reach this same pointer-table resolver
        // later in an ADDRESS context, which is the point where an arithmetic
        // value has actual pointer evidence.  Keep the stored value raw here.
        // Besides avoiding a double rebase, this prevents an unrelated scalar
        // constant inside the value expression from becoming evidence merely
        // because it numerically falls inside a low-VA i386 jump-table segment
        // (for example `h ^ 0xff` beside a table at 0xe4).
        if (!Sym && !HasMatchingSlotReload)
          Sym = tryResolveCodePtrSegPtr(Op.Inputs[1], Builder);
      }
      // A stored read-only (rodata) pointer — a const string/table address
      // returned or passed by value, which the consumer (often a different
      // function, e.g. a switch returning const string pointers `pick()`) can
      // no longer trace back — must be symbolized to the rebuilt rodata global,
      // or the value written to memory is a stale absolute VA the later
      // dereference reads as unmapped.  Unlike the writable-data spill this is
      // NOT gated by FrameReSymbolized (a read-only pointer is never re-based
      // at its use); it is instead restricted to a slot whose reload merely
      // ESCAPES — a slot whose reload is dereferenced/walked locally (`p = &W;
      // *p; p++`) keeps the original VA so the re-base resolvers resolve it
      // exactly once.  ARM32's PC-relative literal pool does not fold to a
      // constant getVar can symbolize (resolve through the literal-pool base);
      // x86-64/AArch64/i386 fold the address to a constant rodata VA.
      if (!Sym && PtrSz && Op.Inputs[1].Size == PtrSz &&
          !frameSlotReloadUsedLocally(Op.Inputs[0])) {
        if (TargetArch == Arch::ARM)
          Sym = tryResolveLiteralPoolBase(Op.Inputs[1], Op.Inputs[1].Size,
                                          Builder);
        if (!Sym)
          if (auto C = traceSSAConst(Op.Inputs[1])) {
            const Segment *Seg = Img->getSegmentFor(*C);
            if (Seg && Seg->isReadable() && !Seg->isWritable() &&
                !Seg->isExecutable() && !Seg->Data.empty())
              Sym = tryResolveGlobalData(*C, Op.Inputs[1].Size);
          }
      }
      if (Sym)
        Val = Builder.CreatePtrToInt(Sym, Val->getType());
    }
    auto *SI = Builder.CreateStore(Val, Ptr);
    if (Op.MemoryOrdering != NdMemoryOrdering::None) {
      if (Op.MemoryOrdering == NdMemoryOrdering::Acquire ||
          Op.MemoryOrdering == NdMemoryOrdering::AcquireRelease)
        llvm::report_fatal_error("acquire ordering is invalid on a store");
      if (Op.NumInputs < 2 || !llvm::isPowerOf2_64(Op.Inputs[1].Size))
        llvm::report_fatal_error("atomic store size must be a power of two");
      SI->setAlignment(llvm::Align(Op.Inputs[1].Size));
      SI->setAtomic(toLLVMAtomicOrdering(Op.MemoryOrdering));
    }
    if (IsGlobalData)
      SI->setVolatile(true);
    return;
  }
  case NdOp::ATOMIC_XCHG:
  case NdOp::ATOMIC_ADD: {
    if (Op.NumInputs < 2 || Op.Output.Size == 0 ||
        Op.Inputs[1].Size != Op.Output.Size)
      llvm::report_fatal_error("atomic RMW has inconsistent operands");
    if (Op.MemoryOrdering == NdMemoryOrdering::None)
      llvm::report_fatal_error("atomic RMW requires memory ordering");
    if (!llvm::isPowerOf2_64(Op.Output.Size))
      llvm::report_fatal_error("atomic RMW size must be a power of two");

    llvm::Value *Val = GetRelocatableIdentityInput(1);
    llvm::Type *ValTy = sizeToType(Op.Output.Size);
    if (Val->getType() != ValTy)
      Val = Builder.CreateIntCast(Val, ValTy, false, "atomic_rmw_val");
    llvm::Value *Ptr =
        resolveAtomicMemoryPtr(Op.Inputs[0], Op.Output.Size, ValTy, Builder);
    llvm::AtomicRMWInst::BinOp RMWOp = Op.Opcode == NdOp::ATOMIC_ADD
                                           ? llvm::AtomicRMWInst::Add
                                           : llvm::AtomicRMWInst::Xchg;
    Result = Builder.CreateAtomicRMW(RMWOp, Ptr, Val,
                                     llvm::MaybeAlign(Op.Output.Size),
                                     toLLVMAtomicOrdering(Op.MemoryOrdering));
    break;
  }
  case NdOp::ATOMIC_CMPXCHG: {
    if (Op.NumInputs < 3 || Op.Output.Size == 0 ||
        Op.Inputs[1].Size != Op.Output.Size ||
        Op.Inputs[2].Size != Op.Output.Size)
      llvm::report_fatal_error(
          "atomic compare-exchange has inconsistent operands");
    if (Op.MemoryOrdering == NdMemoryOrdering::None)
      llvm::report_fatal_error(
          "atomic compare-exchange requires memory ordering");
    if (!llvm::isPowerOf2_64(Op.Output.Size))
      llvm::report_fatal_error(
          "atomic compare-exchange size must be a power of two");

    llvm::Type *ValTy = sizeToType(Op.Output.Size);
    llvm::Value *Expected = GetRelocatableIdentityInput(1);
    llvm::Value *Desired = GetRelocatableIdentityInput(2);
    if (Expected->getType() != ValTy)
      Expected =
          Builder.CreateIntCast(Expected, ValTy, false, "atomic_cmp_expected");
    if (Desired->getType() != ValTy)
      Desired =
          Builder.CreateIntCast(Desired, ValTy, false, "atomic_cmp_desired");
    llvm::Value *Ptr =
        resolveAtomicMemoryPtr(Op.Inputs[0], Op.Output.Size, ValTy, Builder);
    auto *CmpXchg = Builder.CreateAtomicCmpXchg(
        Ptr, Expected, Desired, llvm::MaybeAlign(Op.Output.Size),
        toLLVMAtomicOrdering(Op.MemoryOrdering),
        toLLVMAtomicCmpXchgFailureOrdering(Op.MemoryOrdering));
    CmpXchg->setWeak(false);
    Result = Builder.CreateExtractValue(CmpXchg, 0, "atomic_cmp_old");
    break;
  }
  case NdOp::BRANCH: {
    return;
  }
  case NdOp::COND_BR: {
    return;
  }
  case NdOp::SELECT: {
    auto *Cond = GetInput(0);
    auto *TrueVal = GetRelocatableIdentityInput(1);
    auto *FalseVal = GetRelocatableIdentityInput(2);
    auto [TV, FV] = Coerce(TrueVal, FalseVal);
    if (Cond->getType() != llvm::Type::getInt1Ty(*Ctx)) {
      auto *Zero = llvm::ConstantInt::get(Cond->getType(), 0);
      Cond = Builder.CreateICmpNE(Cond, Zero, "sel_cond");
    }
    Result = Builder.CreateSelect(Cond, TV, FV, "sel");
    break;
  }
  case NdOp::INDIR_CALL:
  case NdOp::CALL:
    emitCallOp(Op, Builder, BlockId, OpIdx);
    return;
  case NdOp::INTRINSIC: {
    Result = emitIntrinsic(Op, Builder);
    if (Result && Op.Output.Size > 0)
      setVar(Op.Output, Result, Builder);

    if (PendingIntrinsicCount > 0) {
      for (auto &Blk : CurMedFunc->Blocks) {
        if (Blk.Id != BlockId)
          continue;
        unsigned CI = 0;
        for (size_t J = static_cast<size_t>(OpIdx) + 1;
             J < Blk.Ops.size() && CI < PendingIntrinsicCount; ++J) {
          auto &NOp = Blk.Ops[J];
          if (NOp.Opcode == NdOp::COPY && NOp.NumInputs >= 1 &&
              NOp.Inputs[0].Kind == MedVar::Temp) {
            setVar(NOp.Inputs[0], PendingIntrinsicOutputs[CI], Builder);
            ++CI;
            continue;
          }
          // LowToMed inserts sub-register normalization (zero/sign extension
          // of the value just copied, sub-piece extraction) between the
          // INTRINSIC output copies on targets with sub-register writes
          // (e.g. an x86-64 EAX write zeroes the upper half of RAX).  Tolerate
          // these so every auxiliary output (e.g. RDTSC's EDX high half) is
          // still wired to its pending value instead of defaulting to zero.
          if (NOp.Opcode == NdOp::INT_ZEXT || NOp.Opcode == NdOp::INT_SEXT ||
              NOp.Opcode == NdOp::SUBBYTES)
            continue;
          break;
        }
        break;
      }
      for (auto &V : PendingIntrinsicOutputs)
        V = nullptr;
      PendingIntrinsicCount = 0;
    }
    return;
  }
  case NdOp::RETURN:
    emitReturnOp(Op, Builder);
    return;
  case NdOp::NOP:
    return;
  default:
    if (Op.Output.Size > 0) {
      Result = llvm::ConstantInt::get(sizeToType(Op.Output.Size), 0);
    } else {
      return;
    }
    break;
  }

  if (Result && Op.Output.Size > 0)
    setVar(Op.Output, Result, Builder);
}

} // namespace neverd
