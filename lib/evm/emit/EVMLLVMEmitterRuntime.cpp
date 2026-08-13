//===- EVMLLVMEmitterRuntime.cpp - EVM LLVM backend module runtime ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMLLVMEmitterDetail.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace neverd::evm::detail {
namespace {

inline constexpr llvm::StringLiteral kStackPushFunctionName = "evm_stack_push";
inline constexpr llvm::StringLiteral kStackPopFunctionName = "evm_stack_pop";
inline constexpr llvm::StringLiteral kStackPeekFunctionName = "evm_stack_peek";
inline constexpr llvm::StringLiteral kStackSwapFunctionName = "evm_stack_swap";
inline constexpr llvm::StringLiteral kExponentFunctionName = "evm_exp";

} // namespace

llvm::ConstantInt *word(llvm::IntegerType *Type, uint64_t Value) {
  return llvm::ConstantInt::get(Type, Value);
}

llvm::ConstantInt *word(llvm::IntegerType *Type, const llvm::APInt &Value) {
  return llvm::ConstantInt::get(Type->getContext(), Value);
}

StackHelpers buildStackHelpers(llvm::Module &Module, llvm::Type *StackType,
                               llvm::IntegerType *WordType) {
  llvm::LLVMContext &Context = Module.getContext();
  auto *Ptr = llvm::PointerType::getUnqual(Context);
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *Void = llvm::Type::getVoidTy(Context);
  auto *Trap =
      llvm::Intrinsic::getOrInsertDeclaration(&Module, llvm::Intrinsic::trap);
  StackHelpers Helpers;

  auto *PushTy = llvm::FunctionType::get(Void, {Ptr, Ptr, WordType}, false);
  Helpers.Push =
      llvm::Function::Create(PushTy, llvm::GlobalValue::InternalLinkage,
                             kStackPushFunctionName, Module);
  {
    auto Args = Helpers.Push->arg_begin();
    llvm::Value *Stack = &*Args++;
    llvm::Value *SP = &*Args++;
    llvm::Value *Value = &*Args;
    Stack->setName("stack");
    SP->setName("sp");
    Value->setName("value");
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Helpers.Push);
    auto *Good = llvm::BasicBlock::Create(Context, "stack.ok", Helpers.Push);
    auto *Bad =
        llvm::BasicBlock::Create(Context, "stack.overflow", Helpers.Push);
    llvm::IRBuilder<> B(Entry);
    llvm::Value *Old = B.CreateLoad(I32, SP, "old.sp");
    B.CreateCondBr(
        B.CreateICmpULT(Old, llvm::ConstantInt::get(I32, kStackLimit)), Good,
        Bad);
    B.SetInsertPoint(Bad);
    B.CreateCall(Trap);
    B.CreateUnreachable();
    B.SetInsertPoint(Good);
    llvm::Value *Slot = B.CreateInBoundsGEP(
        StackType, Stack, {llvm::ConstantInt::get(I32, 0), Old}, "slot");
    B.CreateStore(Value, Slot);
    B.CreateStore(B.CreateAdd(Old, llvm::ConstantInt::get(I32, 1)), SP);
    B.CreateRetVoid();
  }

  auto *PopTy = llvm::FunctionType::get(WordType, {Ptr, Ptr}, false);
  Helpers.Pop = llvm::Function::Create(
      PopTy, llvm::GlobalValue::InternalLinkage, kStackPopFunctionName, Module);
  {
    auto Args = Helpers.Pop->arg_begin();
    llvm::Value *Stack = &*Args++;
    llvm::Value *SP = &*Args;
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Helpers.Pop);
    auto *Good = llvm::BasicBlock::Create(Context, "stack.ok", Helpers.Pop);
    auto *Bad =
        llvm::BasicBlock::Create(Context, "stack.underflow", Helpers.Pop);
    llvm::IRBuilder<> B(Entry);
    llvm::Value *Old = B.CreateLoad(I32, SP, "old.sp");
    B.CreateCondBr(B.CreateICmpUGT(Old, llvm::ConstantInt::get(I32, 0)), Good,
                   Bad);
    B.SetInsertPoint(Bad);
    B.CreateCall(Trap);
    B.CreateUnreachable();
    B.SetInsertPoint(Good);
    llvm::Value *New =
        B.CreateSub(Old, llvm::ConstantInt::get(I32, 1), "new.sp");
    B.CreateStore(New, SP);
    llvm::Value *Slot = B.CreateInBoundsGEP(
        StackType, Stack, {llvm::ConstantInt::get(I32, 0), New}, "slot");
    B.CreateRet(B.CreateLoad(WordType, Slot));
  }

  auto *PeekTy = llvm::FunctionType::get(WordType, {Ptr, Ptr, I32}, false);
  Helpers.Peek =
      llvm::Function::Create(PeekTy, llvm::GlobalValue::InternalLinkage,
                             kStackPeekFunctionName, Module);
  {
    auto Args = Helpers.Peek->arg_begin();
    llvm::Value *Stack = &*Args++;
    llvm::Value *SP = &*Args++;
    llvm::Value *Depth = &*Args;
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Helpers.Peek);
    auto *Good = llvm::BasicBlock::Create(Context, "stack.ok", Helpers.Peek);
    auto *Bad =
        llvm::BasicBlock::Create(Context, "stack.underflow", Helpers.Peek);
    llvm::IRBuilder<> B(Entry);
    llvm::Value *Old = B.CreateLoad(I32, SP);
    B.CreateCondBr(B.CreateICmpUGE(Old, Depth), Good, Bad);
    B.SetInsertPoint(Bad);
    B.CreateCall(Trap);
    B.CreateUnreachable();
    B.SetInsertPoint(Good);
    llvm::Value *Index = B.CreateSub(Old, Depth);
    llvm::Value *Slot = B.CreateInBoundsGEP(
        StackType, Stack, {llvm::ConstantInt::get(I32, 0), Index});
    B.CreateRet(B.CreateLoad(WordType, Slot));
  }

  auto *SwapTy = llvm::FunctionType::get(Void, {Ptr, Ptr, I32}, false);
  Helpers.Swap =
      llvm::Function::Create(SwapTy, llvm::GlobalValue::InternalLinkage,
                             kStackSwapFunctionName, Module);
  {
    auto Args = Helpers.Swap->arg_begin();
    llvm::Value *Stack = &*Args++;
    llvm::Value *SP = &*Args++;
    llvm::Value *Depth = &*Args;
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Helpers.Swap);
    auto *Good = llvm::BasicBlock::Create(Context, "stack.ok", Helpers.Swap);
    auto *Bad =
        llvm::BasicBlock::Create(Context, "stack.underflow", Helpers.Swap);
    llvm::IRBuilder<> B(Entry);
    llvm::Value *Old = B.CreateLoad(I32, SP);
    llvm::Value *Required = B.CreateAdd(Depth, llvm::ConstantInt::get(I32, 1));
    B.CreateCondBr(B.CreateICmpUGE(Old, Required), Good, Bad);
    B.SetInsertPoint(Bad);
    B.CreateCall(Trap);
    B.CreateUnreachable();
    B.SetInsertPoint(Good);
    llvm::Value *TopIndex = B.CreateSub(Old, llvm::ConstantInt::get(I32, 1));
    llvm::Value *OtherIndex = B.CreateSub(TopIndex, Depth);
    llvm::Value *TopSlot = B.CreateInBoundsGEP(
        StackType, Stack, {llvm::ConstantInt::get(I32, 0), TopIndex});
    llvm::Value *OtherSlot = B.CreateInBoundsGEP(
        StackType, Stack, {llvm::ConstantInt::get(I32, 0), OtherIndex});
    llvm::Value *Top = B.CreateLoad(WordType, TopSlot);
    llvm::Value *Other = B.CreateLoad(WordType, OtherSlot);
    B.CreateStore(Other, TopSlot);
    B.CreateStore(Top, OtherSlot);
    B.CreateRetVoid();
  }
  return Helpers;
}

llvm::Function *buildExponentHelper(llvm::Module &Module,
                                    llvm::IntegerType *WordType) {
  llvm::LLVMContext &Context = Module.getContext();
  auto *Ty = llvm::FunctionType::get(WordType, {WordType, WordType}, false);
  auto *Function = llvm::Function::Create(
      Ty, llvm::GlobalValue::InternalLinkage, kExponentFunctionName, Module);
  auto Args = Function->arg_begin();
  llvm::Value *InitialBase = &*Args++;
  llvm::Value *InitialExponent = &*Args;
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  auto *Loop = llvm::BasicBlock::Create(Context, "loop", Function);
  auto *Body = llvm::BasicBlock::Create(Context, "body", Function);
  auto *Done = llvm::BasicBlock::Create(Context, "done", Function);
  llvm::IRBuilder<> B(Entry);
  B.CreateBr(Loop);
  B.SetInsertPoint(Loop);
  auto *Result = B.CreatePHI(WordType, 2, "result");
  auto *Base = B.CreatePHI(WordType, 2, "base");
  auto *Exponent = B.CreatePHI(WordType, 2, "exponent");
  Result->addIncoming(word(WordType, 1), Entry);
  Base->addIncoming(InitialBase, Entry);
  Exponent->addIncoming(InitialExponent, Entry);
  B.CreateCondBr(B.CreateICmpEQ(Exponent, word(WordType, 0)), Done, Body);
  B.SetInsertPoint(Body);
  llvm::Value *Bit = B.CreateTrunc(Exponent, llvm::Type::getInt1Ty(Context));
  llvm::Value *NextResult =
      B.CreateSelect(Bit, B.CreateMul(Result, Base), Result, "next.result");
  llvm::Value *NextBase = B.CreateMul(Base, Base, "next.base");
  llvm::Value *NextExponent = B.CreateLShr(Exponent, word(WordType, 1));
  B.CreateBr(Loop);
  Result->addIncoming(NextResult, Body);
  Base->addIncoming(NextBase, Body);
  Exponent->addIncoming(NextExponent, Body);
  B.SetInsertPoint(Done);
  B.CreateRet(Result);
  return Function;
}

llvm::Value *safeUnsignedDiv(llvm::IRBuilder<> &B, llvm::Value *A,
                             llvm::Value *Divisor, bool Remainder) {
  auto *Type = llvm::cast<llvm::IntegerType>(A->getType());
  llvm::Value *IsZero = B.CreateICmpEQ(Divisor, word(Type, 0));
  llvm::Value *Safe = B.CreateSelect(IsZero, word(Type, 1), Divisor);
  llvm::Value *Value =
      Remainder ? B.CreateURem(A, Safe) : B.CreateUDiv(A, Safe);
  return B.CreateSelect(IsZero, word(Type, 0), Value);
}

llvm::Value *safeSignedDiv(llvm::IRBuilder<> &B, llvm::Value *A,
                           llvm::Value *Divisor, bool Remainder) {
  auto *Type = llvm::cast<llvm::IntegerType>(A->getType());
  llvm::Value *IsZero = B.CreateICmpEQ(Divisor, word(Type, 0));
  llvm::Value *IsMin = B.CreateICmpEQ(
      A,
      llvm::ConstantInt::get(Type, llvm::APInt::getSignedMinValue(kWordBits)));
  llvm::Value *IsMinusOne = B.CreateICmpEQ(
      Divisor,
      llvm::ConstantInt::get(Type, llvm::APInt::getAllOnes(kWordBits)));
  llvm::Value *Overflow = B.CreateAnd(IsMin, IsMinusOne);
  llvm::Value *Unsafe = B.CreateOr(IsZero, Overflow);
  llvm::Value *Safe = B.CreateSelect(Unsafe, word(Type, 1), Divisor);
  llvm::Value *Value =
      Remainder ? B.CreateSRem(A, Safe) : B.CreateSDiv(A, Safe);
  llvm::Value *OverflowResult = Remainder ? word(Type, 0) : A;
  Value = B.CreateSelect(Overflow, OverflowResult, Value);
  return B.CreateSelect(IsZero, word(Type, 0), Value);
}

} // namespace neverd::evm::detail
