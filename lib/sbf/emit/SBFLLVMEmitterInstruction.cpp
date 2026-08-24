//===- SBFLLVMEmitterInstruction.cpp - SBF instruction to LLVM IR ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Lowers one normalized MedIR instruction at a time into LLVM IR: operand
/// selection and width handling, the guarded arithmetic and memory forms, the
/// branch and call forms, and the software call-frame protocol.
///
//===----------------------------------------------------------------------===//

#include "SBFLLVMEmitterDetail.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Intrinsics.h"

#include <string>
#include <utility>

namespace neverd::sbf {

using namespace llvm_emitter_detail;

namespace {

llvm::Value *immediate(EmitContext &Context,
                       const MedInstruction &Instruction) {
  return Context.i64(normalizeImmediate(Instruction.Immediate,
                                        Instruction.Semantics.Immediate));
}

llvm::Value *source(EmitContext &Context, const MedInstruction &Instruction) {
  switch (Instruction.Semantics.Source) {
  case OperandSourceKind::SourceRegister:
    return Context.loadReg(Instruction.Src);
  case OperandSourceKind::None:
  case OperandSourceKind::Immediate:
  case OperandSourceKind::VersionedCallRegister:
  default:
    return immediate(Context, Instruction);
  }
}

llvm::Value *widthValue(EmitContext &Context, llvm::Value *Value,
                        unsigned Width) {
  if (Width == kWordBitWidth)
    return Context.Builder.CreateTrunc(Value, Context.I32);
  return Value;
}

llvm::Value *extendResult(EmitContext &Context, llvm::Value *Value,
                          ResultExtension Extension) {
  if (Value->getType() == Context.I64)
    return Value;
  switch (Extension) {
  case ResultExtension::Sign32:
    return Context.Builder.CreateSExt(Value, Context.I64);
  case ResultExtension::Zero32:
  case ResultExtension::None:
    return Context.Builder.CreateZExt(Value, Context.I64);
  }
  return Context.Builder.CreateZExt(Value, Context.I64);
}

llvm::Value *comparison(EmitContext &Context,
                        const MedInstruction &Instruction) {
  llvm::Value *L =
      widthValue(Context, Context.loadReg(Instruction.Dst), Instruction.Width);
  llvm::Value *R =
      widthValue(Context, source(Context, Instruction), Instruction.Width);
  switch (Instruction.Op) {
  case Operation::Eq:
    return Context.Builder.CreateICmpEQ(L, R);
  case Operation::Ne:
    return Context.Builder.CreateICmpNE(L, R);
  case Operation::UGt:
    return Context.Builder.CreateICmpUGT(L, R);
  case Operation::UGe:
    return Context.Builder.CreateICmpUGE(L, R);
  case Operation::ULt:
    return Context.Builder.CreateICmpULT(L, R);
  case Operation::ULe:
    return Context.Builder.CreateICmpULE(L, R);
  case Operation::SGt:
    return Context.Builder.CreateICmpSGT(L, R);
  case Operation::SGe:
    return Context.Builder.CreateICmpSGE(L, R);
  case Operation::SLt:
    return Context.Builder.CreateICmpSLT(L, R);
  case Operation::SLe:
    return Context.Builder.CreateICmpSLE(L, R);
  case Operation::Set:
    return Context.Builder.CreateICmpNE(
        Context.Builder.CreateAnd(L, R),
        llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(L->getType()), 0));
  default:
    return llvm::ConstantInt::getFalse(Context.Module.getContext());
  }
}

void pushFrame(EmitContext &Context, const MedInstruction &Instruction) {
  llvm::Value *Depth = Context.Builder.CreateLoad(Context.I32, Context.Depth);
  Context.guard(Context.Builder.CreateICmpULT(
                    Depth, Context.i32(static_cast<uint32_t>(
                               Context.Program.Config.MaxCallDepth - 1))),
                FaultCode::CallDepth, Instruction.Address, "call.depth");
  Depth = Context.Builder.CreateLoad(Context.I32, Context.Depth);
  Context.Builder.CreateStore(
      Context.i32(static_cast<uint32_t>(Instruction.Slot + 1)),
      Context.arraySlot(Context.ReturnArrayType, Context.ReturnPC, Depth,
                        "return.pc.ptr"));
  Context.Builder.CreateStore(Context.loadReg(kFramePointerRegister),
                              Context.arraySlot(Context.SavedFPArrayType,
                                                Context.SavedFP, Depth,
                                                "saved.fp.ptr"));
  for (unsigned I = 0; I < kCalleeSavedRegisterCount; ++I)
    Context.Builder.CreateStore(Context.loadReg(kFirstCalleeSavedRegister + I),
                                Context.savedRegSlot(Depth, I));
  Context.Builder.CreateStore(Context.Builder.CreateAdd(Depth, Context.i32(1)),
                              Context.Depth);
  if (!versionHasFeature(Context.Program.Low.TheVersion,
                         VersionFeature::ManualStackFrames)) {
    Context.storeReg(
        kFramePointerRegister,
        Context.Builder.CreateAdd(
            Context.loadReg(kFramePointerRegister),
            Context.i64(automaticFrameStride(Context.Program.Low.TheVersion,
                                             Context.Program.Config))));
  }
}

void branchNext(EmitContext &Context, const MedInstruction &Instruction) {
  const size_t Next = Instruction.Slot + Instruction.SlotWidth;
  if (llvm::BasicBlock *Block = blockFor(Context, Next)) {
    Context.Builder.CreateBr(Block);
    return;
  }
  // A missing LLVM block is the common straight-line case: the next SBF
  // instruction is represented in the current LLVM basic block.
  if (Context.Instructions.contains(Next))
    return;
  Context.Builder.CreateCall(
      Context.Runtime.Fault,
      {Context.Environment,
       Context.i32(static_cast<uint32_t>(FaultCode::ExecutionOverrun)),
       Context.i64(Instruction.Address)});
  Context.Builder.CreateRet(Context.i64(0));
}

} // namespace

namespace llvm_emitter_detail {

llvm::BasicBlock *blockFor(EmitContext &Context, size_t Slot) {
  auto It = Context.Blocks.find(Slot);
  return It == Context.Blocks.end() ? nullptr : It->second;
}

void emitInstruction(EmitContext &Context, const MedInstruction &Instruction) {
  llvm::IRBuilder<> &B = Context.Builder;
  llvm::Value *Dst64 = nullptr;
  llvm::Value *Src64 = nullptr;
  llvm::Value *Result = nullptr;
  auto Operands = [&] {
    Dst64 = Context.loadReg(Instruction.Dst);
    Src64 = source(Context, Instruction);
    return std::pair{widthValue(Context, Dst64, Instruction.Width),
                     widthValue(Context, Src64, Instruction.Width)};
  };

  switch (Instruction.Op) {
  case Operation::LoadImm:
    Result = immediate(Context, Instruction);
    break;
  case Operation::Mov:
    Result =
        widthValue(Context, source(Context, Instruction), Instruction.Width);
    break;
  case Operation::Add: {
    auto [L, R] = Operands();
    Result = B.CreateAdd(L, R);
    break;
  }
  case Operation::Sub: {
    auto [L, R] = Operands();
    Result = Instruction.Semantics.SwapOperands ? B.CreateSub(R, L)
                                                : B.CreateSub(L, R);
    break;
  }
  case Operation::Mul: {
    auto [L, R] = Operands();
    Result = B.CreateMul(L, R);
    break;
  }
  case Operation::UHighMul: {
    auto [L, R] = Operands();
    llvm::Value *Wide = B.CreateMul(B.CreateZExt(L, Context.I128),
                                    B.CreateZExt(R, Context.I128));
    Result = B.CreateTrunc(
        B.CreateLShr(Wide,
                     llvm::ConstantInt::get(Context.I128, kDoubleWordBitWidth)),
        Context.I64);
    break;
  }
  case Operation::SHighMul: {
    auto [L, R] = Operands();
    llvm::Value *Wide = B.CreateMul(B.CreateSExt(L, Context.I128),
                                    B.CreateSExt(R, Context.I128));
    Result = B.CreateTrunc(
        B.CreateAShr(Wide,
                     llvm::ConstantInt::get(Context.I128, kDoubleWordBitWidth)),
        Context.I64);
    break;
  }
  case Operation::UDiv:
  case Operation::URem:
  case Operation::SDiv:
  case Operation::SRem: {
    auto [L, R] = Operands();
    auto *Ty = llvm::cast<llvm::IntegerType>(L->getType());
    Context.guard(B.CreateICmpNE(R, llvm::ConstantInt::get(Ty, 0)),
                  FaultCode::DivideByZero, Instruction.Address, "divide.zero");
    if (Instruction.Op == Operation::SDiv ||
        Instruction.Op == Operation::SRem) {
      llvm::Value *IsMin = B.CreateICmpEQ(
          L, llvm::ConstantInt::get(
                 Ty, llvm::APInt::getSignedMinValue(Ty->getBitWidth())));
      llvm::Value *IsMinusOne =
          B.CreateICmpEQ(R, llvm::ConstantInt::get(Ty, llvm::APInt::getAllOnes(
                                                           Ty->getBitWidth())));
      Context.guard(B.CreateNot(B.CreateAnd(IsMin, IsMinusOne)),
                    FaultCode::DivideOverflow, Instruction.Address,
                    "divide.overflow");
      Result = Instruction.Op == Operation::SDiv ? B.CreateSDiv(L, R)
                                                 : B.CreateSRem(L, R);
    } else {
      Result = Instruction.Op == Operation::UDiv ? B.CreateUDiv(L, R)
                                                 : B.CreateURem(L, R);
    }
    break;
  }
  case Operation::Or: {
    auto [L, R] = Operands();
    Result = B.CreateOr(L, R);
    break;
  }
  case Operation::And: {
    auto [L, R] = Operands();
    Result = B.CreateAnd(L, R);
    break;
  }
  case Operation::Xor: {
    auto [L, R] = Operands();
    Result = B.CreateXor(L, R);
    break;
  }
  case Operation::LSh:
  case Operation::RSh:
  case Operation::ARSh: {
    auto [L, R] = Operands();
    auto *Ty = llvm::cast<llvm::IntegerType>(L->getType());
    R = B.CreateAnd(R, llvm::ConstantInt::get(Ty, Ty->getBitWidth() - 1));
    if (Instruction.Op == Operation::LSh)
      Result = B.CreateShl(L, R);
    else if (Instruction.Op == Operation::RSh)
      Result = B.CreateLShr(L, R);
    else
      Result = B.CreateAShr(L, R);
    break;
  }
  case Operation::Neg: {
    llvm::Value *L = widthValue(Context, Context.loadReg(Instruction.Dst),
                                Instruction.Width);
    Result = B.CreateNeg(L);
    break;
  }
  case Operation::EndianLE: {
    llvm::Value *Value = Context.loadReg(Instruction.Dst);
    if (Instruction.Immediate == kHalfWordBitWidth)
      Result = B.CreateZExt(
          B.CreateTrunc(Value,
                        llvm::Type::getInt16Ty(Context.Module.getContext())),
          Context.I64);
    else if (Instruction.Immediate == kWordBitWidth)
      Result = B.CreateZExt(B.CreateTrunc(Value, Context.I32), Context.I64);
    else
      Result = Value;
    break;
  }
  case Operation::EndianBE: {
    llvm::Value *Value = Context.loadReg(Instruction.Dst);
    llvm::IntegerType *Ty = Context.I64;
    if (Instruction.Immediate == kHalfWordBitWidth)
      Ty = llvm::Type::getInt16Ty(Context.Module.getContext());
    else if (Instruction.Immediate == kWordBitWidth)
      Ty = Context.I32;
    llvm::Value *Narrow = Ty == Context.I64 ? Value : B.CreateTrunc(Value, Ty);
    llvm::Function *BSwap = llvm::Intrinsic::getOrInsertDeclaration(
        &Context.Module, llvm::Intrinsic::bswap, {Ty});
    llvm::Value *Swapped = B.CreateCall(BSwap, {Narrow});
    Result = Ty == Context.I64 ? Swapped : B.CreateZExt(Swapped, Context.I64);
    break;
  }
  case Operation::HighOr:
    Result = B.CreateOr(
        Context.loadReg(Instruction.Dst),
        Context.i64(
            static_cast<uint64_t>(static_cast<uint32_t>(Instruction.Immediate))
            << kWordBitWidth));
    break;
  case Operation::Load: {
    llvm::Value *Base = Context.loadReg(Instruction.Src);
    llvm::Value *Address =
        B.CreateAdd(Base, Context.i64(static_cast<uint64_t>(
                              static_cast<int64_t>(Instruction.Offset))));
    llvm::Value *Status =
        B.CreateCall(Context.Runtime.Load,
                     {Context.Environment, Address,
                      Context.i32(Instruction.Width), Context.RuntimeResult},
                     "load.status");
    Context.guardRuntimeStatus(Status, Instruction.Address, "memory.load",
                               FaultCode::MemoryAccess);
    Result = B.CreateLoad(Context.I64, Context.RuntimeResult, "load.value");
    break;
  }
  case Operation::Store: {
    llvm::Value *Base = Context.loadReg(Instruction.Dst);
    llvm::Value *Address =
        B.CreateAdd(Base, Context.i64(static_cast<uint64_t>(
                              static_cast<int64_t>(Instruction.Offset))));
    llvm::Value *Status = B.CreateCall(Context.Runtime.Store,
                                       {Context.Environment, Address,
                                        Context.i32(Instruction.Width),
                                        source(Context, Instruction)},
                                       "store.status");
    Context.guardRuntimeStatus(Status, Instruction.Address, "memory.store",
                               FaultCode::MemoryAccess);
    branchNext(Context, Instruction);
    return;
  }
  case Operation::Jump:
    if (llvm::BasicBlock *Target =
            blockFor(Context, Instruction.BranchTarget.value_or(-1)))
      B.CreateBr(Target);
    else {
      B.CreateCall(
          Context.Runtime.Fault,
          {Context.Environment,
           Context.i32(static_cast<uint32_t>(FaultCode::InvalidBranch)),
           Context.i64(Instruction.Address)});
      B.CreateRet(Context.i64(0));
    }
    return;
  case Operation::Eq:
  case Operation::Ne:
  case Operation::UGt:
  case Operation::UGe:
  case Operation::ULt:
  case Operation::ULe:
  case Operation::SGt:
  case Operation::SGe:
  case Operation::SLt:
  case Operation::SLe:
  case Operation::Set: {
    llvm::BasicBlock *Taken =
        blockFor(Context, Instruction.BranchTarget.value_or(-1));
    llvm::BasicBlock *Fallthrough = blockFor(Context, Instruction.Slot + 1);
    if (!Taken || !Fallthrough) {
      B.CreateCall(
          Context.Runtime.Fault,
          {Context.Environment,
           Context.i32(static_cast<uint32_t>(FaultCode::InvalidBranch)),
           Context.i64(Instruction.Address)});
      B.CreateRet(Context.i64(0));
    } else {
      B.CreateCondBr(comparison(Context, Instruction), Taken, Fallthrough);
    }
    return;
  }
  case Operation::Call:
    if (Instruction.Call == CallKind::Unsupported) {
      B.CreateCall(
          Context.Runtime.Fault,
          {Context.Environment,
           Context.i32(static_cast<uint32_t>(FaultCode::InvalidInstruction)),
           Context.i64(Instruction.Address)});
      B.CreateRet(Context.i64(0));
      return;
    }
    if (Instruction.Dispatch == CallDispatchPolicy::LegacyRuntimeThenFunction &&
        Instruction.Call == CallKind::Internal && Instruction.CallTarget) {
      auto Arguments = Context.runtimeSyscallArguments(Instruction.SyscallHash);
      llvm::Value *Status = B.CreateCall(Context.Runtime.Syscall, Arguments,
                                         "legacy.syscall.status");
      Status =
          Context.normalizeRuntimeStatus(Status, FaultCode::InvalidInstruction);
      auto *Handled =
          llvm::BasicBlock::Create(Context.Module.getContext(),
                                   "legacy.syscall.handled", &Context.Function);
      auto *Classify = llvm::BasicBlock::Create(Context.Module.getContext(),
                                                "legacy.syscall.classify",
                                                &Context.Function);
      auto *InvokeFunction =
          llvm::BasicBlock::Create(Context.Module.getContext(),
                                   "legacy.function.invoke", &Context.Function);
      auto *Fault =
          llvm::BasicBlock::Create(Context.Module.getContext(),
                                   "legacy.syscall.fault", &Context.Function);
      B.CreateCondBr(B.CreateICmpEQ(Status, Context.i32(static_cast<uint32_t>(
                                                FaultCode::None))),
                     Handled, Classify);

      B.SetInsertPoint(Handled);
      Context.storeReg(
          kReturnRegister,
          B.CreateLoad(Context.I64, Context.RuntimeResult, "syscall.result"));
      B.CreateBr(InvokeFunction);

      B.SetInsertPoint(Classify);
      B.CreateCondBr(B.CreateICmpEQ(Status, Context.i32(static_cast<uint32_t>(
                                                FaultCode::UnknownSyscall))),
                     InvokeFunction, Fault);

      B.SetInsertPoint(Fault);
      B.CreateCall(Context.Runtime.Fault, {Context.Environment, Status,
                                           Context.i64(Instruction.Address)});
      B.CreateRet(Context.i64(0));

      B.SetInsertPoint(InvokeFunction);
      pushFrame(Context, Instruction);
      if (llvm::BasicBlock *Target =
              blockFor(Context, *Instruction.CallTarget)) {
        B.CreateBr(Target);
      } else {
        B.CreateCall(
            Context.Runtime.Fault,
            {Context.Environment,
             Context.i32(static_cast<uint32_t>(FaultCode::InvalidBranch)),
             Context.i64(Instruction.Address)});
        B.CreateRet(Context.i64(0));
      }
      return;
    }
    if (Instruction.Call == CallKind::Syscall ||
        Instruction.Call == CallKind::Unresolved) {
      auto Arguments = Context.runtimeSyscallArguments(Instruction.SyscallHash);
      llvm::Value *Status =
          B.CreateCall(Context.Runtime.Syscall, Arguments, "syscall.status");
      Context.guardRuntimeStatus(Status, Instruction.Address, "syscall",
                                 FaultCode::InvalidInstruction);
      Result =
          B.CreateLoad(Context.I64, Context.RuntimeResult, "syscall.result");
      Context.storeReg(kReturnRegister, Result);
      branchNext(Context, Instruction);
      return;
    }
    if (Instruction.Call == CallKind::Internal && Instruction.CallTarget) {
      pushFrame(Context, Instruction);
      if (llvm::BasicBlock *Target = blockFor(Context, *Instruction.CallTarget))
        B.CreateBr(Target);
      else {
        B.CreateCall(
            Context.Runtime.Fault,
            {Context.Environment,
             Context.i32(static_cast<uint32_t>(FaultCode::InvalidBranch)),
             Context.i64(Instruction.Address)});
        B.CreateRet(Context.i64(0));
      }
      return;
    }
    B.CreateCall(Context.Runtime.Fault,
                 {Context.Environment,
                  Context.i32(static_cast<uint32_t>(FaultCode::UnknownSyscall)),
                  Context.i64(Instruction.Address)});
    B.CreateRet(Context.i64(0));
    return;
  case Operation::CallX: {
    llvm::Value *Target = Context.loadReg(Instruction.CallRegister);
    llvm::Value *Offset =
        B.CreateSub(Target, Context.i64(Context.Program.Low.TextAddress));
    pushFrame(Context, Instruction);
    llvm::Value *TargetSlot64 =
        B.CreateUDiv(Offset, Context.i64(kInstructionSize));
    Context.guard(
        B.CreateICmpULT(TargetSlot64,
                        Context.i64(Context.Program.Low.Instructions.size())),
        FaultCode::UnknownIndirectCall, Instruction.Address, "callx.target");
    llvm::Value *TargetSlot = B.CreateTrunc(TargetSlot64, Context.I32);
    auto *Default = llvm::BasicBlock::Create(
        Context.Module.getContext(), "callx.invalid", &Context.Function);
    llvm::SwitchInst *Switch =
        B.CreateSwitch(TargetSlot, Default, Context.Blocks.size());
    for (const auto &Entry : Context.Blocks)
      Switch->addCase(Context.i32(static_cast<uint32_t>(Entry.first)),
                      Entry.second);
    Context.Builder.SetInsertPoint(Default);
    Context.Builder.CreateCall(
        Context.Runtime.Fault,
        {Context.Environment,
         Context.i32(static_cast<uint32_t>(FaultCode::UnknownIndirectCall)),
         Context.i64(Instruction.Address)});
    Context.Builder.CreateRet(Context.i64(0));
    return;
  }
  case Operation::Exit: {
    llvm::Value *Depth = B.CreateLoad(Context.I32, Context.Depth);
    auto *Pop = llvm::BasicBlock::Create(Context.Module.getContext(),
                                         "return.pop", &Context.Function);
    B.CreateCondBr(B.CreateICmpEQ(Depth, Context.i32(0)), Context.ReturnValue,
                   Pop);
    B.SetInsertPoint(Pop);
    llvm::Value *NewDepth = B.CreateSub(Depth, Context.i32(1));
    B.CreateStore(NewDepth, Context.Depth);
    for (unsigned I = 0; I < kCalleeSavedRegisterCount; ++I)
      Context.storeReg(
          kFirstCalleeSavedRegister + I,
          B.CreateLoad(Context.I64, Context.savedRegSlot(NewDepth, I)));
    Context.storeReg(
        kFramePointerRegister,
        B.CreateLoad(Context.I64, Context.arraySlot(Context.SavedFPArrayType,
                                                    Context.SavedFP, NewDepth,
                                                    "saved.fp.ptr")));
    B.CreateBr(Context.ReturnDispatch);
    return;
  }
  case Operation::Invalid: {
    B.CreateCall(
        Context.Runtime.Fault,
        {Context.Environment,
         Context.i32(static_cast<uint32_t>(
             executionFaultForValidationRule(Instruction.InvalidReason))),
         Context.i64(Instruction.Address)});
    B.CreateRet(Context.i64(0));
    return;
  }
  }

  if (Result) {
    Result = extendResult(Context, Result, Instruction.Semantics.Result);
    Context.storeReg(Instruction.Dst, Result);
  }
  branchNext(Context, Instruction);
}

} // namespace llvm_emitter_detail
} // namespace neverd::sbf
