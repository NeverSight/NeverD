//===- LLVMEmitter.cpp - EVM to LLVM i256 backend -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/LLVMEmitter.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <array>

namespace neverd::evm {
namespace {

inline constexpr llvm::StringLiteral kStackPushFunctionName = "evm_stack_push";
inline constexpr llvm::StringLiteral kStackPopFunctionName = "evm_stack_pop";
inline constexpr llvm::StringLiteral kStackPeekFunctionName = "evm_stack_peek";
inline constexpr llvm::StringLiteral kStackSwapFunctionName = "evm_stack_swap";
inline constexpr llvm::StringLiteral kExponentFunctionName = "evm_exp";

struct StackHelpers {
  llvm::Function *Push = nullptr;
  llvm::Function *Pop = nullptr;
  llvm::Function *Peek = nullptr;
  llvm::Function *Swap = nullptr;
};

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

} // namespace

llvm::Expected<std::unique_ptr<llvm::Module>>
emitLLVM(const EVMProgram &Program, llvm::LLVMContext &Context,
         const LLVMEmitterOptions &Options) {
  auto Module = std::make_unique<llvm::Module>(Options.ModuleName, Context);
  auto *I8 = llvm::Type::getInt8Ty(Context);
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *I256 = llvm::IntegerType::get(Context, kWordBits);
  auto *I512 = llvm::IntegerType::get(Context, kWideWordBits);
  auto *Ptr = llvm::PointerType::getUnqual(Context);
  auto *StackType = llvm::ArrayType::get(I256, kStackLimit);

  StackHelpers Stack = buildStackHelpers(*Module, StackType, I256);
  llvm::Function *Exponent = buildExponentHelper(*Module, I256);
  auto *Trap = llvm::Intrinsic::getOrInsertDeclaration(Module.get(),
                                                       llvm::Intrinsic::trap);

  std::vector<llvm::Type *> HostArgs{Ptr, I8};
  HostArgs.insert(HostArgs.end(), maxHostOpcodeArguments(), I256);
  auto Host = Module->getOrInsertFunction(
      kHostFunctionName, llvm::FunctionType::get(I256, HostArgs, false));
  auto Trace = Module->getOrInsertFunction(
      kTraceFunctionName,
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), {Ptr, I64, I8},
                              false));

  auto *ExecuteTy = llvm::FunctionType::get(I32, {Ptr}, false);
  auto *Execute =
      llvm::Function::Create(ExecuteTy, llvm::GlobalValue::ExternalLinkage,
                             Options.FunctionName, *Module);
  llvm::Value *Environment = Execute->getArg(0);
  Environment->setName("environment");
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Execute);
  llvm::IRBuilder<> EntryBuilder(Entry);
  llvm::Value *StackStorage =
      EntryBuilder.CreateAlloca(StackType, nullptr, "stack");
  llvm::Value *SP = EntryBuilder.CreateAlloca(I32, nullptr, "sp");
  EntryBuilder.CreateStore(llvm::ConstantInt::get(I32, 0), SP);

  llvm::DenseMap<uint64_t, llvm::BasicBlock *> Blocks;
  for (const auto &Instruction : Program.Low.Instructions)
    Blocks[Instruction.PC] = llvm::BasicBlock::Create(
        Context, "pc_" + std::to_string(Instruction.PC), Execute);
  if (auto It = Blocks.find(kEntryPC); It != Blocks.end())
    EntryBuilder.CreateBr(It->second);
  else
    EntryBuilder.CreateRet(
        llvm::ConstantInt::get(I32, exitStatusCode(ExitStatus::Stopped)));

  auto EmitPop = [&](llvm::IRBuilder<> &B) {
    return B.CreateCall(Stack.Pop, {StackStorage, SP}, "pop");
  };
  auto EmitPush = [&](llvm::IRBuilder<> &B, llvm::Value *Value) {
    B.CreateCall(Stack.Push, {StackStorage, SP, Value});
  };
  auto EmitHost = [&](llvm::IRBuilder<> &B, Opcode Op,
                      const std::vector<llvm::Value *> &Inputs) {
    std::vector<llvm::Value *> Args{Environment,
                                    llvm::ConstantInt::get(I8, opcodeByte(Op))};
    for (size_t I = 0; I < maxHostOpcodeArguments(); ++I)
      Args.push_back(I < Inputs.size() ? Inputs[I] : word(I256, 0));
    return B.CreateCall(Host, Args, "host.result");
  };
  auto EmitNext = [&](llvm::IRBuilder<> &B, uint64_t NextPC) {
    if (auto It = Blocks.find(NextPC); It != Blocks.end())
      B.CreateBr(It->second);
    else
      B.CreateRet(
          llvm::ConstantInt::get(I32, exitStatusCode(ExitStatus::Stopped)));
  };
  auto EmitJumpSwitch = [&](llvm::IRBuilder<> &B, llvm::Value *Destination,
                            llvm::StringRef Prefix) {
    auto *Bad = llvm::BasicBlock::Create(Context, Prefix + ".invalid", Execute);
    auto *Switch =
        B.CreateSwitch(Destination, Bad, Program.Low.JumpDestinations.size());
    for (uint64_t Target : Program.Low.JumpDestinations)
      Switch->addCase(word(I256, Target), Blocks.lookup(Target));
    llvm::IRBuilder<> BadBuilder(Bad);
    BadBuilder.CreateCall(Trap);
    BadBuilder.CreateUnreachable();
  };

  for (const auto &Instruction : Program.Low.Instructions) {
    llvm::IRBuilder<> B(Blocks.lookup(Instruction.PC));
    const Opcode Op = Instruction.opcode();
    if (Options.EmitTraceHooks)
      B.CreateCall(Trace,
                   {Environment, llvm::ConstantInt::get(I64, Instruction.PC),
                    llvm::ConstantInt::get(I8, opcodeByte(Op))});

    if (!Instruction.isExecutable() || Instruction.is(Opcode::INVALID)) {
      B.CreateCall(Trap);
      B.CreateUnreachable();
      continue;
    }
    if (Instruction.is(Opcode::STOP)) {
      B.CreateRet(
          llvm::ConstantInt::get(I32, exitStatusCode(ExitStatus::Stopped)));
      continue;
    }
    if (Instruction.isPush()) {
      EmitPush(B, word(I256, Instruction.Immediate));
      EmitNext(B, Instruction.NextPC);
      continue;
    }
    if (Instruction.isDup()) {
      llvm::Value *Value = B.CreateCall(
          Stack.Peek, {StackStorage, SP,
                       llvm::ConstantInt::get(I32, Instruction.dupDepth())});
      EmitPush(B, Value);
      EmitNext(B, Instruction.NextPC);
      continue;
    }
    if (Instruction.isSwap()) {
      B.CreateCall(Stack.Swap,
                   {StackStorage, SP,
                    llvm::ConstantInt::get(I32, Instruction.swapDepth())});
      EmitNext(B, Instruction.NextPC);
      continue;
    }
    if (Instruction.isExchange()) {
      const auto EmitSwap = [&](uint16_t Depth) {
        B.CreateCall(Stack.Swap,
                     {StackStorage, SP, llvm::ConstantInt::get(I32, Depth)});
      };
      const auto [First, Second] = *Instruction.exchangeDepths();
      EmitSwap(First);
      EmitSwap(Second);
      EmitSwap(First);
      EmitNext(B, Instruction.NextPC);
      continue;
    }
    if (Instruction.isJump()) {
      llvm::Value *Destination = EmitPop(B);
      if (Op == Opcode::JUMP) {
        EmitJumpSwitch(B, Destination,
                       "jump." + std::to_string(Instruction.PC));
      } else {
        llvm::Value *Condition = EmitPop(B);
        auto *Taken = llvm::BasicBlock::Create(
            Context, "jumpi." + std::to_string(Instruction.PC) + ".taken",
            Execute);
        llvm::BasicBlock *NotTaken = nullptr;
        if (auto It = Blocks.find(Instruction.NextPC); It != Blocks.end())
          NotTaken = It->second;
        else {
          NotTaken = llvm::BasicBlock::Create(
              Context, "jumpi." + std::to_string(Instruction.PC) + ".end",
              Execute);
          llvm::IRBuilder<> EndBuilder(NotTaken);
          EndBuilder.CreateRet(
              llvm::ConstantInt::get(I32, exitStatusCode(ExitStatus::Stopped)));
        }
        B.CreateCondBr(B.CreateICmpNE(Condition, word(I256, 0)), Taken,
                       NotTaken);
        llvm::IRBuilder<> TakenBuilder(Taken);
        EmitJumpSwitch(TakenBuilder, Destination,
                       "jumpi." + std::to_string(Instruction.PC));
      }
      continue;
    }

    std::vector<llvm::Value *> Inputs;
    Inputs.reserve(Instruction.Info.StackPops);
    for (uint8_t I = 0; I < Instruction.Info.StackPops; ++I)
      Inputs.push_back(EmitPop(B));

    if (Op == Opcode::POP || Op == Opcode::JUMPDEST) {
      EmitNext(B, Instruction.NextPC);
      continue;
    }
    if (Op == Opcode::PC) {
      EmitPush(B, word(I256, Instruction.PC));
      EmitNext(B, Instruction.NextPC);
      continue;
    }
    if (Op == Opcode::CODESIZE) {
      EmitPush(B, word(I256, Program.Low.Code.size()));
      EmitNext(B, Instruction.NextPC);
      continue;
    }

    llvm::Value *Output = nullptr;
    if (isALU(Instruction.Info)) {
      llvm::Value *A = Inputs.empty() ? nullptr : Inputs[0];
      llvm::Value *Second = Inputs.size() > 1 ? Inputs[1] : nullptr;
      switch (Op) {
      case Opcode::ADD:
        Output = B.CreateAdd(A, Second);
        break;
      case Opcode::MUL:
        Output = B.CreateMul(A, Second);
        break;
      case Opcode::SUB:
        Output = B.CreateSub(A, Second);
        break;
      case Opcode::DIV:
        Output = safeUnsignedDiv(B, A, Second, false);
        break;
      case Opcode::SDIV:
        Output = safeSignedDiv(B, A, Second, false);
        break;
      case Opcode::MOD:
        Output = safeUnsignedDiv(B, A, Second, true);
        break;
      case Opcode::SMOD:
        Output = safeSignedDiv(B, A, Second, true);
        break;
      case Opcode::ADDMOD:
      case Opcode::MULMOD: {
        llvm::Value *Modulus = Inputs[2];
        llvm::Value *WideA = B.CreateZExt(A, I512);
        llvm::Value *WideB = B.CreateZExt(Second, I512);
        llvm::Value *WideM = B.CreateZExt(Modulus, I512);
        llvm::Value *IsZero =
            B.CreateICmpEQ(WideM, llvm::ConstantInt::get(I512, 0));
        llvm::Value *SafeM =
            B.CreateSelect(IsZero, llvm::ConstantInt::get(I512, 1), WideM);
        llvm::Value *WideValue = Op == Opcode::ADDMOD
                                     ? B.CreateAdd(WideA, WideB)
                                     : B.CreateMul(WideA, WideB);
        llvm::Value *Reduced = B.CreateURem(WideValue, SafeM);
        Output =
            B.CreateSelect(IsZero, word(I256, 0), B.CreateTrunc(Reduced, I256));
        break;
      }
      case Opcode::EXP:
        Output = B.CreateCall(Exponent, {A, Second});
        break;
      case Opcode::SIGNEXTEND: {
        // Sanitizing k before constructing shifts avoids poison when the
        // shift count reaches the EVM word width.
        llvm::Value *NoOp = B.CreateICmpUGE(A, word(I256, kWordBytes - 1));
        llvm::Value *SafeK = B.CreateSelect(NoOp, word(I256, 0), A);
        llvm::Value *Bit =
            B.CreateAdd(B.CreateMul(SafeK, word(I256, kBitsPerByte)),
                        word(I256, kBitsPerByte - 1));
        llvm::Value *SignBit = B.CreateShl(word(I256, 1), Bit);
        llvm::Value *LowMask =
            B.CreateSub(B.CreateShl(SignBit, word(I256, 1)), word(I256, 1));
        llvm::Value *SignSet =
            B.CreateICmpNE(B.CreateAnd(Second, SignBit), word(I256, 0));
        llvm::Value *Extended =
            B.CreateSelect(SignSet, B.CreateOr(Second, B.CreateNot(LowMask)),
                           B.CreateAnd(Second, LowMask));
        Output = B.CreateSelect(NoOp, Second, Extended);
        break;
      }
      case Opcode::LT:
        Output = B.CreateZExt(B.CreateICmpULT(A, Second), I256);
        break;
      case Opcode::GT:
        Output = B.CreateZExt(B.CreateICmpUGT(A, Second), I256);
        break;
      case Opcode::SLT:
        Output = B.CreateZExt(B.CreateICmpSLT(A, Second), I256);
        break;
      case Opcode::SGT:
        Output = B.CreateZExt(B.CreateICmpSGT(A, Second), I256);
        break;
      case Opcode::EQ:
        Output = B.CreateZExt(B.CreateICmpEQ(A, Second), I256);
        break;
      case Opcode::ISZERO:
        Output = B.CreateZExt(B.CreateICmpEQ(A, word(I256, 0)), I256);
        break;
      case Opcode::AND:
        Output = B.CreateAnd(A, Second);
        break;
      case Opcode::OR:
        Output = B.CreateOr(A, Second);
        break;
      case Opcode::XOR:
        Output = B.CreateXor(A, Second);
        break;
      case Opcode::NOT:
        Output = B.CreateNot(A);
        break;
      case Opcode::BYTE: {
        llvm::Value *TooLarge = B.CreateICmpUGE(A, word(I256, kWordBytes));
        llvm::Value *SafeIndex = B.CreateSelect(TooLarge, word(I256, 0), A);
        llvm::Value *Shift =
            B.CreateMul(B.CreateSub(word(I256, kWordBytes - 1), SafeIndex),
                        word(I256, kBitsPerByte));
        llvm::Value *Byte =
            B.CreateAnd(B.CreateLShr(Second, Shift), word(I256, kByteMax));
        Output = B.CreateSelect(TooLarge, word(I256, 0), Byte);
        break;
      }
      case Opcode::SHL:
      case Opcode::SHR:
      case Opcode::SAR: {
        llvm::Value *TooLarge = B.CreateICmpUGE(A, word(I256, kWordBits));
        llvm::Value *SafeShift = B.CreateSelect(TooLarge, word(I256, 0), A);
        llvm::Value *Shifted =
            Op == Opcode::SHL   ? B.CreateShl(Second, SafeShift)
            : Op == Opcode::SHR ? B.CreateLShr(Second, SafeShift)
                                : B.CreateAShr(Second, SafeShift);
        llvm::Value *LargeResult = word(I256, 0);
        if (Op == Opcode::SAR) {
          llvm::Value *Negative = B.CreateICmpSLT(Second, word(I256, 0));
          LargeResult = B.CreateSelect(
              Negative,
              llvm::ConstantInt::get(I256, llvm::APInt::getAllOnes(kWordBits)),
              word(I256, 0));
        }
        Output = B.CreateSelect(TooLarge, LargeResult, Shifted);
        break;
      }
      case Opcode::CLZ: {
        auto *Ctlz = llvm::Intrinsic::getOrInsertDeclaration(
            Module.get(), llvm::Intrinsic::ctlz, {I256});
        Output = B.CreateCall(Ctlz, {A, llvm::ConstantInt::getFalse(Context)});
        break;
      }
      default:
        llvm_unreachable("unhandled EVM ALU opcode in LLVM backend");
      }
    } else {
      Output = EmitHost(B, Op, Inputs);
    }

    if (Op == Opcode::RETURN || Op == Opcode::REVERT ||
        Op == Opcode::SELFDESTRUCT) {
      (void)Output;
      const ExitStatus Status = Op == Opcode::RETURN ? ExitStatus::Returned
                                : Op == Opcode::REVERT
                                    ? ExitStatus::Reverted
                                    : ExitStatus::SelfDestructed;
      B.CreateRet(llvm::ConstantInt::get(I32, exitStatusCode(Status)));
      continue;
    }
    if (Instruction.Info.IsTerminator) {
      B.CreateRet(
          llvm::ConstantInt::get(I32, exitStatusCode(ExitStatus::Stopped)));
      continue;
    }
    if (Instruction.Info.StackPushes != 0) {
      if (!Output)
        llvm_unreachable("EVM opcode output was not lowered");
      EmitPush(B, Output);
    }
    EmitNext(B, Instruction.NextPC);
  }

  std::string Verification;
  llvm::raw_string_ostream VerificationStream(Verification);
  if (llvm::verifyModule(*Module, &VerificationStream))
    return llvm::make_error<llvm::StringError>(
        "evm: generated invalid LLVM IR: " + VerificationStream.str(),
        llvm::inconvertibleErrorCode());
  return Module;
}

std::string emitLLVMText(const llvm::Module &Module) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  Module.print(OS, nullptr);
  return Text;
}

} // namespace neverd::evm
