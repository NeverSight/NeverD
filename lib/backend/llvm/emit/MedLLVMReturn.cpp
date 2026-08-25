//===- MedLLVMReturn.cpp - RETURN lowering ----------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// RETURN lowering: return-register recovery, 32-bit register-pair splicing,
/// and by-value struct assembly.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#include "neverd/backend/llvm/LanguageEHMetadata.h"

#define DEBUG_TYPE "neverd-med-llvm-return"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"

#include <set>
#include <vector>

namespace neverd {

//===----------------------------------------------------------------------===//
// RETURN -- return-value recovery and materialization
//===----------------------------------------------------------------------===//

void MedLLVMEmitter::markCxxContinuationReturn(const MedOp &Op, int BlockId,
                                               llvm::ReturnInst &Return) {
  if (!ActiveCxxContinuationPlan ||
      *ActiveCxxContinuationPlan >= CxxContinuationPlans.size())
    return;
  CxxContinuationFunctionPlan &Plan =
      CxxContinuationPlans[*ActiveCxxContinuationPlan];
  if (!Plan.PreconditionsComplete || Op.NumInputs != 1)
    return;

  std::optional<size_t> Match;
  for (size_t I = 0; I < Plan.Bindings.size(); ++I) {
    const CxxContinuationReturnBinding &Binding = Plan.Bindings[I];
    if (Binding.BlockId != BlockId || Binding.ReturnAddr != Op.Addr ||
        Binding.ReturnSeq != Op.OriginSeq ||
        !sameCxxContinuationReturnValue(Binding.ReturnValue, Op.Inputs[0]))
      continue;
    if (Match) {
      Plan.PreconditionsComplete = false;
      Return.setMetadata(
          language_eh_md::InternalCxxContinuationReturnAttachment, nullptr);
      return;
    }
    Match = I;
  }
  if (!Match)
    return;
  if (Return.getMetadata(
          language_eh_md::InternalCxxContinuationReturnAttachment)) {
    Plan.PreconditionsComplete = false;
    Return.setMetadata(language_eh_md::InternalCxxContinuationReturnAttachment,
                       nullptr);
    return;
  }

  llvm::Metadata *Operands[] = {
      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
          llvm::Type::getInt64Ty(*Ctx), *ActiveCxxContinuationPlan)),
      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
          llvm::Type::getInt64Ty(*Ctx), *Match))};
  Return.setMetadata(language_eh_md::InternalCxxContinuationReturnAttachment,
                     llvm::MDNode::get(*Ctx, Operands));
}

void MedLLVMEmitter::emitReturnOp(const MedOp &Op, llvm::IRBuilder<> &Builder,
                                  int BlockId) {
  auto GetReturnValue = [&](const MedVar &V) -> llvm::Value * {
    if (llvm::Value *Code =
            tryResolveCodeAddressValue(V, /*RequireCodeRole=*/false, Builder))
      return Code;
    return getVar(V, Builder);
  };
  auto GetInput = [&](uint8_t Idx) -> llvm::Value * {
    if (Idx >= Op.NumInputs)
      return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), 0);
    return GetReturnValue(Op.Inputs[Idx]);
  };
  auto *RetTy = CurFunc->getReturnType();
  if (RetTy->isVoidTy()) {
    llvm::ReturnInst *Return = Builder.CreateRetVoid();
    markCxxContinuationReturn(Op, BlockId, *Return);
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
        rejectEscapingAddressFragment(*RV, "an aggregate return field");
        // Symbolize a pointer-width integer field holding a writable-global
        // address (`return (struct){ &g[i], n }`) — the aggregate analog of
        // the scalar return symbolization, else the caller dereferences a raw
        // absolute VA.  FP fields and plain integers fall through to getVar.
        const ConstantProvenanceSummary FieldOccurrence =
            summarizeConstantProvenance(*RV);
        const bool FieldAddressRelation =
            FieldOccurrence.Model ==
            ConstantProvenanceSummary::ValueModel::Address;
        if (!RR.IsFP && FT->isIntegerTy() &&
            (!FieldOccurrence.hasExplicitProvenance() || FieldAddressRelation))
          if (llvm::Value *Sym = tryResolveWritableData(
                  *RV, RV->Size, Builder, /*IsValueOperand=*/true))
            V = Builder.CreatePtrToInt(Sym, llvm::Type::getInt64Ty(*Ctx));
        if (!V)
          V = GetReturnValue(*RV);
      }
      V = V ? coerce(V, FT) : llvm::Constant::getNullValue(FT);
      Agg = Builder.CreateInsertValue(Agg, V, I);
    }
    llvm::ReturnInst *Return = Builder.CreateRet(Agg);
    markCxxContinuationReturn(Op, BlockId, *Return);
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
    const uint16_t IntegerReturnSize =
        WantWide64 ? TRI.PointerSize
                   : RetTy->isIntegerTy()
                         ? static_cast<uint16_t>(
                               (RetTy->getIntegerBitWidth() + 7) / 8)
                         : 0;
    auto isAuthoritativeIntegerReturnView = [&](const MedVar &V) {
      if (WantFloat || V.RegOff != IntRetOff || IntegerReturnSize == 0)
        return false;
      if (V.Size >= IntegerReturnSize)
        return true;
      return TRI.writeZeroExtends(V.RegOff, V.Size) &&
             TRI.isSubRegOf(V.RegOff, V.Size, IntRetOff, IntegerReturnSize);
    };

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
      bool SawReturnView = false;
      bool PassedReturn = false;
      for (auto RIt = Blk.Ops.rbegin(); RIt != Blk.Ops.rend(); ++RIt) {
        if (&*RIt == &Op) {
          PassedReturn = true;
          continue;
        }
        if (!PassedReturn)
          continue;
        if (RIt->Output.Kind != MedVar::Reg || RIt->Output.Size == 0)
          continue;

        if (WantX87 && TRI.isX87StackReg(RIt->Output.RegOff)) {
          // Reverse iteration sees the value at the current x87 top first.
          if (!SawReturnView) {
            SawReturnView = true;
            RetVar = &RIt->Output;
          }
        } else if (WantFloat && !WantX87 &&
                   RIt->Output.RegOff == FloatRetOff) {
          // The newest physical XMM/V register view is authoritative.  Do not
          // replace it with an older wider view merely to avoid coercion.
          if (!SawReturnView) {
            SawReturnView = true;
            RetVar = &RIt->Output;
          }
        } else if (!WantFloat && RIt->Output.RegOff == IntRetOff &&
                   !SawReturnView) {
          // A partial integer view is authoritative only when its ABI write
          // semantics define the requested return width.  If it does not,
          // stop at that newest view and fail closed instead of using stale
          // bits from an older full-width definition.
          SawReturnView = true;
          if (isAuthoritativeIntegerReturnView(RIt->Output))
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
      if (!RetVar && !SawReturnView) {
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
          if (isAuthoritativeIntegerReturnView(Phi.Output) && !RetVar)
            RetVar = &Phi.Output;
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
        rejectEscapingAddressFragment(*RetVar, "a return value");
        RetVal = GetReturnValue(*RetVar);
        const ConstantProvenanceSummary ReturnOccurrence =
            summarizeConstantProvenance(*RetVar);
        // A returned address into a WRITABLE global (`return &g[index]` /
        // `return cond?A:B`) must be symbolized — otherwise the value is a
        // raw inttoptr(baseVA+index) and the caller, dereferencing the
        // returned pointer, hits a stale absolute VA.  Only the writable-data
        // resolver is used here (not the full pointer-arg chain): every
        // integer return flows through this path, and the heuristic
        // rodata-table resolvers would false-positive on a computed integer
        // result that merely traces to a read-only base (e.g. a NEON kernel
        // folding a rodata vector).
        const bool ReturnAddressRelation =
            ReturnOccurrence.Model ==
            ConstantProvenanceSummary::ValueModel::Address;
        if (!WantFloat && !WantWide64 && RetTy->isIntegerTy() && RetVal &&
            RetVal->getType()->isIntegerTy() &&
            (!ReturnOccurrence.hasExplicitProvenance() ||
             ReturnAddressRelation))
          if (llvm::Value *Sym = tryResolveWritableData(
                  *RetVar, RetVar->Size, Builder, /*IsValueOperand=*/true))
            RetVal = Builder.CreatePtrToInt(Sym, RetTy);
      }
      // Splice the high half above the low half into the 64-bit result.
      if (WantWide64 && RetVal && HiVar) {
        rejectEscapingAddressFragment(*HiVar,
                                      "the high half of a return value");
        auto *I64 = llvm::Type::getInt64Ty(*Ctx);
        auto toI64 = [&](llvm::Value *V) -> llvm::Value * {
          if (V->getType() == I64)
            return V;
          if (V->getType()->getIntegerBitWidth() > 64)
            return Builder.CreateTrunc(V, I64);
          return Builder.CreateZExt(V, I64);
        };
        llvm::Value *Lo = toI64(RetVal);
        llvm::Value *Hi = toI64(GetReturnValue(*HiVar));
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
        // FP/vector return registers are live at the shared epilogue as the
        // newest physical view.  Keep the legacy widest preference for integer
        // predecessor recovery, where a narrow SUBBYTES may not define the
        // canonical full-width value (bug #152 extension).
        const MedVar *RetVar = nullptr;
        for (auto BIt = Blk.Ops.rbegin(); BIt != Blk.Ops.rend(); ++BIt) {
          if (BIt->Output.Kind == MedVar::Reg && BIt->Output.Size > 0 &&
              (WantX87 ? TRI.isX87StackReg(BIt->Output.RegOff)
                       : BIt->Output.RegOff == RetRegOff)) {
            if (!RetVar ||
                (!WantFloat && BIt->Output.Size > RetVar->Size))
              RetVar = &BIt->Output;
          }
        }
        if (RetVar) {
          rejectEscapingAddressFragment(*RetVar, "a return value");
          RetVal = GetReturnValue(*RetVar);
          break;
        }
        for (auto &Phi : Blk.Phis) {
          if (Phi.Output.Kind == MedVar::Reg && Phi.Output.Size > 0 &&
              (WantX87 ? TRI.isX87StackReg(Phi.Output.RegOff)
                       : Phi.Output.RegOff == RetRegOff)) {
            rejectEscapingAddressFragment(Phi.Output, "a return value");
            RetVal = GetReturnValue(Phi.Output);
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
      RetVal = GetReturnValue(FpLiveIn);
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
    else {
      rejectEscapingAddressFragment(Op.Inputs[0], "a return value");
      RetVal = GetInput(0);
    }
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
    } else if (!RetVal->getType()->isPointerTy() && !RetTy->isPointerTy() &&
               RetVal->getType()->getPrimitiveSizeInBits() >
                   RetTy->getPrimitiveSizeInBits()) {
      // The selected physical return-register value may be a wider alias than
      // the recovered ABI type (for example a 32-byte YMM parent carrying the
      // 16-byte XMM0 result).  Preserve the register model for real wide
      // consumers, but project its low ABI lane at the RETURN boundary.
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
  llvm::ReturnInst *Return = Builder.CreateRet(RetVal);
  markCxxContinuationReturn(Op, BlockId, *Return);
  return;
}

} // namespace neverd
