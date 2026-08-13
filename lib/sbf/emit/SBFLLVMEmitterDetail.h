//===- SBFLLVMEmitterDetail.h - Private SBF LLVM emitter state --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The runtime ABI handles and the per-function emission state shared by the
/// SBF LLVM backend's translation units: module setup (SBFLLVMEmitter.cpp)
/// and per-instruction lowering (SBFLLVMEmitterInstruction.cpp).
///
/// This header is an implementation detail of the sbf/emit library and should
/// NOT be included by code outside lib/sbf/emit/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_EMIT_SBFLLVMEMITTERDETAIL_H
#define NEVERD_SBF_EMIT_SBFLLVMEMITTERDETAIL_H

#include "neverd/sbf/emit/SBFLLVMEmitter.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Compiler.h"

#include <cstddef>
#include <string>

namespace neverd::sbf {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE llvm_emitter_detail {

struct RuntimeABI {
  llvm::FunctionCallee Load;
  llvm::FunctionCallee Store;
  llvm::FunctionCallee Syscall;
  llvm::FunctionCallee Fault;
};

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

/// The LLVM block a recovered slot starts, or null when the slot does not
/// begin one.
llvm::BasicBlock *blockFor(EmitContext &Context, size_t Slot);

/// Lower one MedIR instruction into the block the builder is positioned in.
void emitInstruction(EmitContext &Context, const MedInstruction &Instruction);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE llvm_emitter_detail
} // namespace neverd::sbf

#endif // NEVERD_SBF_EMIT_SBFLLVMEMITTERDETAIL_H
