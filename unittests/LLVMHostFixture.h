//===- LLVMHostFixture.h - IR fixtures for host Clang drivers -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/STLExtras.h"
#include "llvm/AsmParser/LLLexer.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::test {

inline void printHostX87Literals(llvm::StringRef Text,
                                 llvm::LLVMContext &Context,
                                 llvm::raw_ostream &Out) {
  // New LLVM prints binary80 bit patterns as f0x followed by 20 hex digits.
  // Older readers require 0xK with the identical bits. Use LLVM's lexer so
  // names, block labels, inline assembly and metadata strings stay intact.
  llvm::SourceMgr Sources;
  llvm::SMDiagnostic Diagnostic;
  auto Buffer = llvm::MemoryBuffer::getMemBufferCopy(Text, "host-fixture");
  const auto Bytes = Buffer->getBuffer();
  Sources.AddNewSourceBuffer(std::move(Buffer), llvm::SMLoc());
  llvm::LLLexer Lexer(Bytes, Sources, Diagnostic, Context);
  size_t Copied = 0;
  for (auto Kind = Lexer.Lex(); Kind != llvm::lltok::Eof; Kind = Lexer.Lex()) {
    if (Kind == llvm::lltok::Error)
      llvm::report_fatal_error("host compiler fixture has invalid LLVM tokens");
    if (Kind != llvm::lltok::FloatHexLiteral ||
        Lexer.getAPSIntVal().getBitWidth() != 80)
      continue;
    const size_t Start = Lexer.getLoc().getPointer() - Bytes.data();
    Out << Text.slice(Copied, Start) << "0xK" << Text.substr(Start + 3, 20);
    Copied = Start + 23;
  }
  Out << Text.drop_front(Copied);
}

inline void lowerHostLifetimeSignatures(llvm::Module &Module) {
  // LLVM 18 expects (i64 size, ptr), whereas the embedded LLVM spells a
  // whole-object lifetime with just ptr. A newer signature can parse in an
  // old Clang with verification disabled and then crash its SROA pass.
  // Keep the same objects and lifetime boundaries in the compiler copy.
  auto &Context = Module.getContext();
  auto *SizeType = llvm::Type::getInt64Ty(Context);
  auto WithSize = [&](llvm::AttributeList Attributes) {
    return llvm::AttributeList::get(
        Context, Attributes.getFnAttrs(), Attributes.getRetAttrs(),
        {llvm::AttributeSet(), Attributes.getParamAttrs(0)});
  };
  for (auto &Function : llvm::make_early_inc_range(Module)) {
    const auto ID = Function.getIntrinsicID();
    if ((ID != llvm::Intrinsic::lifetime_start &&
         ID != llvm::Intrinsic::lifetime_end) ||
        Function.arg_size() != 1)
      continue;
    const std::string Name = Function.getName().str();
    Function.setName(Name + ".host.new");
    auto *Type = llvm::FunctionType::get(
        Function.getReturnType(), {SizeType, Function.getArg(0)->getType()},
        false);
    auto *Legacy = llvm::Function::Create(
        Type, Function.getLinkage(), Function.getAddressSpace(), Name, &Module);
    Legacy->setAttributes(WithSize(Function.getAttributes()));
    Legacy->addParamAttr(0, llvm::Attribute::ImmArg);
    for (auto *User : llvm::make_early_inc_range(Function.users())) {
      auto *Call = llvm::cast<llvm::CallInst>(User);
      auto *Pointer = Call->getArgOperand(0);
      uint64_t Bytes = UINT64_MAX;
      if (auto *Allocation = llvm::dyn_cast<llvm::AllocaInst>(Pointer))
        if (auto Size = Allocation->getAllocationSize(Module.getDataLayout());
            Size && !Size->isScalable())
          Bytes = Size->getFixedValue();
      auto *Replacement = llvm::CallInst::Create(
          Type, Legacy, {llvm::ConstantInt::get(SizeType, Bytes), Pointer}, "",
          Call->getIterator());
      Replacement->setAttributes(WithSize(Call->getAttributes()));
      Replacement->copyMetadata(*Call);
      Call->eraseFromParent();
    }
    Function.eraseFromParent();
  }
}

inline void lowerHostConversionAnnotations(llvm::Instruction &Instruction) {
  auto *Trunc = llvm::dyn_cast<llvm::TruncInst>(&Instruction);
  auto *Extend = llvm::dyn_cast<llvm::ZExtInst>(&Instruction);
  if ((!Trunc || (!Trunc->hasNoUnsignedWrap() && !Trunc->hasNoSignedWrap())) &&
      (!Extend || !Extend->hasNonNeg()))
    return;
  // Older Clang IR parsers cannot spell these conversion guarantees. Keep
  // their poison conditions explicit in the compiler copy, including lanes.
  llvm::IRBuilder<> Builder(&Instruction);
  auto *Input = Instruction.getOperand(0);
  llvm::Value *Value = nullptr, *Allowed = nullptr;
  if (Trunc) {
    Value = Builder.CreateTrunc(Input, Trunc->getType(), "host.trunc");
    if (Trunc->hasNoUnsignedWrap())
      Allowed = Builder.CreateICmpEQ(
          Builder.CreateZExt(Value, Input->getType()), Input);
    if (Trunc->hasNoSignedWrap()) {
      auto *Signed = Builder.CreateICmpEQ(
          Builder.CreateSExt(Value, Input->getType()), Input);
      Allowed = Allowed ? Builder.CreateAnd(Allowed, Signed) : Signed;
    }
  } else {
    Value = Builder.CreateZExt(Input, Extend->getType(), "host.zext");
    Allowed = Builder.CreateICmpSGE(
        Input, llvm::Constant::getNullValue(Input->getType()));
  }
  auto *Checked = Builder.CreateSelect(
      Allowed, Value, llvm::PoisonValue::get(Instruction.getType()));
  if (auto *Replacement = llvm::dyn_cast<llvm::Instruction>(Checked))
    Replacement->takeName(&Instruction);
  Instruction.replaceAllUsesWith(Checked);
  Instruction.eraseFromParent();
}

inline void printHostCompilerFixture(const llvm::Module &Module,
                                     llvm::raw_ostream &Out) {
  // The embedded LLVM can be newer than the host Clang used to compile an
  // executable oracle over source-defined samples. These function/call
  // guarantees have newer IR spellings; omitting them is conservative for
  // those samples. Conversion poison conditions use explicit guards. Semantic
  // proofs keep the unchanged module, including its original range contracts.
  auto Fixture = llvm::CloneModule(Module);
  auto RemoveNewAnnotations = [](auto &FunctionOrCall, unsigned Parameters) {
    FunctionOrCall.removeFnAttr(llvm::Attribute::NoCreateUndefOrPoison);
    FunctionOrCall.removeFnAttr(llvm::Attribute::Memory);
    FunctionOrCall.removeRetAttr(llvm::Attribute::Range);
    for (unsigned I = 0; I != Parameters; ++I) {
      FunctionOrCall.removeParamAttr(I, llvm::Attribute::Captures);
      FunctionOrCall.removeParamAttr(I, llvm::Attribute::Range);
    }
  };
  for (auto &Function : *Fixture) {
    RemoveNewAnnotations(Function, Function.arg_size());
    for (auto &Block : Function)
      for (auto &Instruction : llvm::make_early_inc_range(Block)) {
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
          RemoveNewAnnotations(*Call, Call->arg_size());
        lowerHostConversionAnnotations(Instruction);
      }
  }
  lowerHostLifetimeSignatures(*Fixture);
  std::string Text;
  llvm::raw_string_ostream Buffer(Text);
  Fixture->print(Buffer, nullptr);
  printHostX87Literals(Text, Fixture->getContext(), Out);
}

inline std::string printHostCompilerFixture(const llvm::Module &Module) {
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  printHostCompilerFixture(Module, Out);
  return Text;
}

} // namespace neverd::test
