//===- SBFLLVMEmitter.cpp - Solana SBF to LLVM IR backend -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Builds the LLVM module for a recovered SBF program: declares the runtime
/// ABI the generated code calls, creates the entry function and its blocks,
/// drives per-instruction lowering, and emits the return dispatch, the
/// read-only data, and the version metadata.
///
//===----------------------------------------------------------------------===//

#include "neverd/sbf/emit/SBFLLVMEmitter.h"

#include "SBFLLVMEmitterDetail.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace neverd::sbf {

using namespace llvm_emitter_detail;

namespace {

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
