//===- LLVMEmitter.cpp - Solana SBF to LLVM IR backend ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/LLVMEmitter.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

namespace neverd::sbf {
namespace {

struct RuntimeABI {
  llvm::FunctionCallee Load;
  llvm::FunctionCallee Store;
  llvm::FunctionCallee Syscall;
  llvm::FunctionCallee Fault;
};

void addRuntimeAttributes(llvm::FunctionCallee Callee) {
  auto *Function = llvm::cast<llvm::Function>(Callee.getCallee());
  Function->addFnAttr(llvm::Attribute::NoUnwind);
}

void addRuntimeOutputAttributes(llvm::FunctionCallee Callee,
                                unsigned Parameter) {
  auto *Function = llvm::cast<llvm::Function>(Callee.getCallee());
  Function->addParamAttr(
      Parameter, llvm::Attribute::getWithCaptureInfo(
                     Function->getContext(), llvm::CaptureInfo::none()));
  Function->addParamAttr(Parameter, llvm::Attribute::WriteOnly);
}

struct EmitContext {
  const SBFProgram &Program;
  llvm::Module &Module;
  llvm::Function &Function;
  llvm::IRBuilder<> Builder;
  llvm::IntegerType *I32;
  llvm::IntegerType *I64;
  llvm::IntegerType *I128;
  llvm::PointerType *Ptr;
  llvm::ArrayType *RegisterArrayType;
  llvm::ArrayType *ReturnArrayType;
  llvm::ArrayType *SavedFPArrayType;
  llvm::ArrayType *SavedRegisterRowType;
  llvm::ArrayType *SavedRegisterArrayType;
  llvm::AllocaInst *Registers;
  llvm::AllocaInst *Depth;
  llvm::AllocaInst *ReturnPC;
  llvm::AllocaInst *SavedFP;
  llvm::AllocaInst *SavedRegisters;
  llvm::AllocaInst *RuntimeResult;
  llvm::Value *Environment;
  RuntimeABI Runtime;
  llvm::DenseMap<size_t, llvm::BasicBlock *> Blocks;
  llvm::DenseMap<size_t, const MedInstruction *> Instructions;
  llvm::BasicBlock *ReturnDispatch = nullptr;
  llvm::BasicBlock *ReturnValue = nullptr;
  unsigned FaultSerial = 0;

  EmitContext(const SBFProgram &Program, llvm::Module &Module,
              llvm::Function &Function, RuntimeABI Runtime)
      : Program(Program), Module(Module), Function(Function),
        Builder(Module.getContext()),
        I32(llvm::Type::getInt32Ty(Module.getContext())),
        I64(llvm::Type::getInt64Ty(Module.getContext())),
        I128(llvm::IntegerType::get(Module.getContext(),
                                    kDoubleWordBitWidth * 2)),
        Ptr(llvm::PointerType::getUnqual(Module.getContext())),
        RegisterArrayType(llvm::ArrayType::get(I64, kRegisterCount)),
        ReturnArrayType(llvm::ArrayType::get(I32, Program.Config.MaxCallDepth)),
        SavedFPArrayType(
            llvm::ArrayType::get(I64, Program.Config.MaxCallDepth)),
        SavedRegisterRowType(
            llvm::ArrayType::get(I64, kCalleeSavedRegisterCount)),
        SavedRegisterArrayType(llvm::ArrayType::get(
            SavedRegisterRowType, Program.Config.MaxCallDepth)),
        Registers(nullptr), Depth(nullptr), ReturnPC(nullptr), SavedFP(nullptr),
        SavedRegisters(nullptr), RuntimeResult(nullptr), Environment(nullptr),
        Runtime(Runtime) {}

  llvm::ConstantInt *i32(uint32_t Value) const {
    return llvm::ConstantInt::get(I32, Value);
  }
  llvm::ConstantInt *i64(uint64_t Value) const {
    return llvm::ConstantInt::get(I64, Value);
  }
  llvm::Value *regPtr(unsigned Register) {
    return Builder.CreateInBoundsGEP(RegisterArrayType, Registers,
                                     {i32(0), i32(Register)},
                                     "r" + std::to_string(Register) + ".ptr");
  }
  llvm::Value *loadReg(unsigned Register) {
    return Builder.CreateLoad(I64, regPtr(Register),
                              "r" + std::to_string(Register));
  }
  void storeReg(unsigned Register, llvm::Value *Value) {
    Builder.CreateStore(Value, regPtr(Register));
  }
  llvm::Value *arraySlot(llvm::ArrayType *Type, llvm::Value *Array,
                         llvm::Value *Index, llvm::StringRef Name) {
    return Builder.CreateInBoundsGEP(Type, Array, {i32(0), Index}, Name);
  }
  llvm::Value *savedRegSlot(llvm::Value *Frame, unsigned Register) {
    return Builder.CreateInBoundsGEP(SavedRegisterArrayType, SavedRegisters,
                                     {i32(0), Frame, i32(Register)},
                                     "saved.reg.ptr");
  }
  void guard(llvm::Value *Valid, FaultCode Code, va_t Address,
             llvm::StringRef Name) {
    llvm::LLVMContext &C = Module.getContext();
    auto *Good = llvm::BasicBlock::Create(
        C, (Name + ".ok." + std::to_string(FaultSerial)).str(), &Function);
    auto *Bad = llvm::BasicBlock::Create(
        C, (Name + ".fault." + std::to_string(FaultSerial++)).str(), &Function);
    Builder.CreateCondBr(Valid, Good, Bad);
    Builder.SetInsertPoint(Bad);
    Builder.CreateCall(
        Runtime.Fault,
        {Environment, i32(static_cast<uint32_t>(Code)), i64(Address)});
    Builder.CreateRet(i64(0));
    Builder.SetInsertPoint(Good);
  }
};

RuntimeABI declareRuntime(llvm::Module &Module) {
  llvm::LLVMContext &Context = Module.getContext();
  auto *Ptr = llvm::PointerType::getUnqual(Context);
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Void = llvm::Type::getVoidTy(Context);
  llvm::SmallVector<llvm::Type *, kRuntimeSyscallArgumentCount>
      SyscallParameters{Ptr, I32};
  for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index)
    SyscallParameters.push_back(I64);
  SyscallParameters.push_back(Ptr);
  RuntimeABI ABI{
      Module.getOrInsertFunction(
          kRuntimeLoadName,
          llvm::FunctionType::get(I32, {Ptr, I64, I32, Ptr}, false)),
      Module.getOrInsertFunction(
          kRuntimeStoreName,
          llvm::FunctionType::get(I32, {Ptr, I64, I32, I64}, false)),
      Module.getOrInsertFunction(
          kRuntimeSyscallName,
          llvm::FunctionType::get(I32, SyscallParameters, false)),
      Module.getOrInsertFunction(
          kRuntimeFaultName,
          llvm::FunctionType::get(Void, {Ptr, I32, I64}, false)),
  };
  addRuntimeAttributes(ABI.Load);
  addRuntimeAttributes(ABI.Store);
  addRuntimeAttributes(ABI.Syscall);
  addRuntimeAttributes(ABI.Fault);
  addRuntimeOutputAttributes(ABI.Load, kRuntimeLoadOutputParameter);
  addRuntimeOutputAttributes(ABI.Syscall, kRuntimeSyscallOutputParameter);
  llvm::cast<llvm::Function>(ABI.Fault.getCallee())
      ->addFnAttr(llvm::Attribute::Cold);
  return ABI;
}

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

llvm::BasicBlock *blockFor(EmitContext &Context, size_t Slot) {
  auto It = Context.Blocks.find(Slot);
  return It == Context.Blocks.end() ? nullptr : It->second;
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
    Context.guard(B.CreateICmpEQ(Status, Context.i32(0)),
                  FaultCode::MemoryAccess, Instruction.Address, "memory.load");
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
    Context.guard(B.CreateICmpEQ(Status, Context.i32(0)),
                  FaultCode::MemoryAccess, Instruction.Address, "memory.store");
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
    if (Instruction.Call == CallKind::Syscall) {
      llvm::SmallVector<llvm::Value *, kRuntimeSyscallArgumentCount> Arguments{
          Context.Environment, Context.i32(Instruction.SyscallHash)};
      for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index)
        Arguments.push_back(Context.loadReg(kFirstArgumentRegister + Index));
      Arguments.push_back(Context.RuntimeResult);
      llvm::Value *Status =
          B.CreateCall(Context.Runtime.Syscall, Arguments, "syscall.status");
      Context.guard(B.CreateICmpEQ(Status, Context.i32(0)),
                    FaultCode::UnknownSyscall, Instruction.Address, "syscall");
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
  case Operation::Invalid:
    B.CreateCall(
        Context.Runtime.Fault,
        {Context.Environment,
         Context.i32(static_cast<uint32_t>(FaultCode::InvalidInstruction)),
         Context.i64(Instruction.Address)});
    B.CreateRet(Context.i64(0));
    return;
  }

  if (Result) {
    Result = extendResult(Context, Result, Instruction.Semantics.Result);
    Context.storeReg(Instruction.Dst, Result);
  }
  branchNext(Context, Instruction);
}

} // namespace

llvm::Expected<std::unique_ptr<llvm::Module>>
emitLLVM(const SBFProgram &Program, llvm::LLVMContext &Context,
         const LLVMEmitterOptions &Options) {
  if (llvm::Error Error = validateVMConfig(Program.Config))
    return std::move(Error);
  if (Program.Med.Instructions.empty())
    return llvm::make_error<llvm::StringError>(
        "sbf: cannot emit LLVM IR for an empty MedIR",
        llvm::inconvertibleErrorCode());
  auto Module = std::make_unique<llvm::Module>(Options.ModuleName, Context);
  RuntimeABI Runtime = declareRuntime(*Module);
  auto *Ptr = llvm::PointerType::getUnqual(Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  // The loader hands the program two addresses, so the recovered entry point
  // takes two. Dropping the second would not be a simplification: a program
  // compiled for a runtime that supplies it reads the register, and a callable
  // that cannot be given a value for it cannot reproduce that program's
  // behaviour at all.
  auto *FunctionType = llvm::FunctionType::get(I64, {Ptr, I64, I64}, false);
  auto *Function =
      llvm::Function::Create(FunctionType, llvm::GlobalValue::ExternalLinkage,
                             Options.FunctionName, *Module);
  Function->addFnAttr(llvm::Attribute::NoUnwind);
  auto Arguments = Function->arg_begin();
  llvm::Value *Environment = &*Arguments++;
  llvm::Value *Input = &*Arguments++;
  llvm::Value *InstructionData = &*Arguments;
  Environment->setName("environment");
  Input->setName("input");
  InstructionData->setName("instruction_data");

  EmitContext EC(Program, *Module, *Function, Runtime);
  EC.Environment = Environment;
  for (const MedInstruction &Instruction : Program.Med.Instructions) {
    if (Instruction.Slot >= Program.Low.Instructions.size() ||
        Program.Low.Instructions[Instruction.Slot].IsContinuation ||
        Program.Low.Instructions[Instruction.Slot].Address !=
            Instruction.Address)
      return llvm::make_error<llvm::StringError>(
          "sbf: LLVM emitter received inconsistent LowIR and MedIR slots",
          llvm::inconvertibleErrorCode());
    if (!EC.Instructions.insert({Instruction.Slot, &Instruction}).second)
      return llvm::make_error<llvm::StringError>(
          "sbf: LLVM emitter found duplicate MedIR instruction slots",
          llvm::inconvertibleErrorCode());
  }
  for (size_t Slot = 0; Slot < Program.Low.Instructions.size(); ++Slot)
    if (Program.Low.Instructions[Slot].Slot != Slot)
      return llvm::make_error<llvm::StringError>(
          "sbf: LLVM emitter received a non-canonical LowIR slot table",
          llvm::inconvertibleErrorCode());
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  EC.ReturnDispatch =
      llvm::BasicBlock::Create(Context, "return.dispatch", Function);
  EC.ReturnValue = llvm::BasicBlock::Create(Context, "return.value", Function);
  if (Program.Low.Blocks.empty())
    return llvm::make_error<llvm::StringError>(
        "sbf: LLVM emitter requires a non-empty LowIR CFG",
        llvm::inconvertibleErrorCode());
  for (const BasicBlock &Block : Program.Low.Blocks) {
    if (Block.StartSlot >= Block.EndSlot ||
        Block.EndSlot > Program.Low.Instructions.size() ||
        !EC.Instructions.contains(Block.StartSlot) ||
        !EC.Blocks
             .insert({Block.StartSlot,
                      llvm::BasicBlock::Create(
                          Context,
                          "sbf.bb." + std::to_string(Block.ID) + ".pc." +
                              std::to_string(Block.StartSlot),
                          Function)})
             .second)
      return llvm::make_error<llvm::StringError>(
          "sbf: LLVM emitter received an inconsistent LowIR CFG",
          llvm::inconvertibleErrorCode());
  }

  // CALLX accepts any complete instruction address at runtime. Splitting every
  // possible dynamic entry preserves that contract while ordinary programs
  // retain one LLVM block per analyzed SBF basic block.
  const bool HasIndirectCall = std::any_of(
      Program.Med.Instructions.begin(), Program.Med.Instructions.end(),
      [](const MedInstruction &Instruction) {
        return Instruction.Op == Operation::CallX;
      });
  if (HasIndirectCall)
    for (const LowInstruction &Instruction : Program.Low.Instructions)
      if (!EC.Blocks.contains(Instruction.Slot))
        EC.Blocks[Instruction.Slot] = llvm::BasicBlock::Create(
            Context, "sbf.callx.pc." + std::to_string(Instruction.Slot),
            Function);

  EC.Builder.SetInsertPoint(Entry);
  EC.Registers = EC.Builder.CreateAlloca(EC.RegisterArrayType, nullptr, "regs");
  EC.Depth = EC.Builder.CreateAlloca(EC.I32, nullptr, "call.depth");
  EC.ReturnPC =
      EC.Builder.CreateAlloca(EC.ReturnArrayType, nullptr, "return.pc");
  EC.SavedFP =
      EC.Builder.CreateAlloca(EC.SavedFPArrayType, nullptr, "saved.fp");
  EC.SavedRegisters = EC.Builder.CreateAlloca(EC.SavedRegisterArrayType,
                                              nullptr, "saved.registers");
  EC.RuntimeResult = EC.Builder.CreateAlloca(EC.I64, nullptr, "runtime.result");
  for (unsigned Register = 0; Register < kRegisterCount; ++Register)
    EC.storeReg(Register, EC.i64(0));
  EC.storeReg(kFirstArgumentRegister, Input);
  EC.storeReg(kInstructionDataRegister, InstructionData);
  EC.storeReg(
      kFramePointerRegister,
      EC.i64(initialFramePointer(Program.Low.TheVersion, Program.Config)));
  EC.Builder.CreateStore(EC.i32(0), EC.Depth);
  llvm::BasicBlock *Entrypoint = blockFor(EC, Program.Low.EntrySlot);
  if (!Entrypoint)
    return llvm::make_error<llvm::StringError>(
        "sbf: LLVM emitter cannot find the entry block",
        llvm::inconvertibleErrorCode());
  EC.Builder.CreateBr(Entrypoint);

  for (const BasicBlock &Block : Program.Low.Blocks) {
    EC.Builder.SetInsertPoint(blockFor(EC, Block.StartSlot));
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
      const LowInstruction &Low = Program.Low.Instructions[Slot];
      if (Low.IsContinuation)
        continue;
      if (llvm::BasicBlock *Split = blockFor(EC, Slot);
          Slot != Block.StartSlot && Split)
        EC.Builder.SetInsertPoint(Split);
      auto It = EC.Instructions.find(Slot);
      if (It == EC.Instructions.end()) {
        EC.Builder.CreateCall(
            Runtime.Fault,
            {Environment,
             EC.i32(static_cast<uint32_t>(FaultCode::InvalidInstruction)),
             EC.i64(Low.Address)});
        EC.Builder.CreateRet(EC.i64(0));
        break;
      }
      if (EC.Builder.GetInsertBlock()->hasTerminator())
        return llvm::make_error<llvm::StringError>(
            "sbf: LLVM emitter found an instruction after a CFG terminator",
            llvm::inconvertibleErrorCode());
      emitInstruction(EC, *It->second);
    }
  }
  if (HasIndirectCall)
    for (const LowInstruction &Low : Program.Low.Instructions) {
      if (!Low.IsContinuation)
        continue;
      EC.Builder.SetInsertPoint(blockFor(EC, Low.Slot));
      EC.Builder.CreateCall(
          Runtime.Fault,
          {Environment,
           EC.i32(static_cast<uint32_t>(FaultCode::InvalidInstruction)),
           EC.i64(Low.Address)});
      EC.Builder.CreateRet(EC.i64(0));
    }

  EC.Builder.SetInsertPoint(EC.ReturnDispatch);
  llvm::Value *Depth = EC.Builder.CreateLoad(EC.I32, EC.Depth);
  llvm::Value *ReturnSlot = EC.Builder.CreateLoad(
      EC.I32,
      EC.arraySlot(EC.ReturnArrayType, EC.ReturnPC, Depth, "return.pc.ptr"));
  auto *BadReturn =
      llvm::BasicBlock::Create(Context, "return.invalid", Function);
  llvm::SwitchInst *ReturnSwitch =
      EC.Builder.CreateSwitch(ReturnSlot, BadReturn, EC.Blocks.size());
  for (const auto &Block : EC.Blocks)
    ReturnSwitch->addCase(EC.i32(static_cast<uint32_t>(Block.first)),
                          Block.second);

  EC.Builder.SetInsertPoint(BadReturn);
  EC.Builder.CreateCall(
      Runtime.Fault,
      {Environment, EC.i32(static_cast<uint32_t>(FaultCode::ExecutionOverrun)),
       EC.i64(0)});
  EC.Builder.CreateRet(EC.i64(0));

  EC.Builder.SetInsertPoint(EC.ReturnValue);
  EC.Builder.CreateRet(EC.loadReg(kReturnRegister));

  unsigned RegionNumber = 0;
  for (const ProgramRegion &Region : Program.ExecutableImage.regions()) {
    if (!Region.DataVisible || Region.Bytes.empty())
      continue;
    auto *Data = llvm::ConstantDataArray::get(Context, Region.Bytes);
    const std::string Name =
        Region.Kind == ProgramRegionKind::ReadOnly && RegionNumber == 0
            ? "sbf.rodata"
            : "sbf.program." + std::to_string(RegionNumber);
    auto *Global = new llvm::GlobalVariable(*Module, Data->getType(), true,
                                            llvm::GlobalValue::InternalLinkage,
                                            Data, Name);
    Global->setAlignment(llvm::Align(kInstructionSize));
    ++RegionNumber;
  }
  llvm::NamedMDNode *VersionMetadata =
      Module->getOrInsertNamedMetadata("neverd.sbf.version");
  VersionMetadata->addOperand(llvm::MDNode::get(
      Context,
      llvm::MDString::get(Context, versionName(Program.Low.TheVersion))));

  std::string VerificationError;
  llvm::raw_string_ostream VerificationStream(VerificationError);
  if (llvm::verifyModule(*Module, &VerificationStream))
    return llvm::make_error<llvm::StringError>(
        "sbf: generated LLVM module is invalid: " + VerificationError,
        llvm::inconvertibleErrorCode());
  return Module;
}

std::string emitLLVMText(const llvm::Module &Module) {
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  Module.print(OS, nullptr);
  return Buffer;
}

} // namespace neverd::sbf
