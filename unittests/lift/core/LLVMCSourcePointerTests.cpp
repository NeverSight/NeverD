//===- LLVMCSourcePointerTests.cpp - Source call pointer bridges --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute calls whose source declarations and machine carriers differ.
///
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"

#include <map>
#include <optional>
#include <string>

namespace {
using namespace neverd;

class PointerContext final : public NullDebugContext {
public:
  std::map<va_t, FunctionSym> Functions;
  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    const auto It = Functions.find(Address);
    return It == Functions.end() ? std::nullopt : std::optional(It->second);
  }
  bool hasInfo() const override { return true; }
};

TEST(LLVMCSourcePointers, DefinitionAndCallsShareObjectAndCallbackTypes) {
  for (const auto Arch : {Arch::X64, Arch::AArch64})
    for (const auto Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (unsigned Mode = 0; Mode != 5; ++Mode) {
        SCOPED_TRACE(static_cast<int>(Arch));
        SCOPED_TRACE(static_cast<int>(Format));
        SCOPED_TRACE(Mode);
        const bool Selected = Mode == 1;
        const bool Preserved = Mode == 2;
        const bool Alias = Mode == 3;
        llvm::LLVMContext C;
        llvm::Module M("source-pointer-calls", C);
        M.setDataLayout("e-p:64:64");
        llvm::IRBuilder<> B(C);
        auto *Word = B.getInt64Ty();
        auto *Unary = llvm::FunctionType::get(Word, {Word}, false);
        // The caller precedes its definitions, as a module's physical order
        // carries no authority over a source prototype.
        auto *Caller = llvm::Function::Create(
            Unary, llvm::GlobalValue::ExternalLinkage, "bridge_entry", M);
        auto *RoundTrip = llvm::Function::Create(
            Unary, llvm::GlobalValue::ExternalLinkage, "round_trip", M);
        auto *Apply = llvm::Function::Create(
            llvm::FunctionType::get(Word, {Word, Word}, false),
            llvm::GlobalValue::ExternalLinkage, "apply_callback", M);
        auto *Callback = llvm::Function::Create(
            Unary, llvm::GlobalValue::ExternalLinkage, "add_seven", M);
        rewrite_source::setOriginalVA(*RoundTrip, 0x2000);
        rewrite_source::setOriginalVA(*Apply, 0x3000);
        B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Caller));
        auto *Pointer = B.CreateCall(RoundTrip, {Caller->getArg(0)});
        auto *Value =
            B.CreateLoad(Word, B.CreateIntToPtr(Pointer, B.getPtrTy()));
        B.CreateRet(
            B.CreateCall(Apply, {B.CreatePtrToInt(Callback, Word), Value}));
        B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", RoundTrip));
        B.CreateRet(RoundTrip->getArg(0));
        B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Apply));
        B.CreateRet(B.CreateCall(
            Unary, B.CreateIntToPtr(Apply->getArg(0), B.getPtrTy()),
            {Apply->getArg(1)}));
        B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Callback));
        B.CreateRet(B.CreateAdd(Callback->getArg(0), B.getInt64(7)));
        ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
        const auto Scalar = NdType::makeInt(8, false);
        const auto PointerType = NdType::makePtr(Scalar);
        // A source pointer narrower than its machine carrier cannot change
        // either side of the signature into the compiler's native pointer.
        if (Mode == 4)
          PointerType->Size = 4;
        if (Alias)
          PointerType->TypedefName = "WORD_POINTER";
        auto Function = std::make_shared<NdType>();
        Function->Kind = NdTypeKind::Func;
        Function->RetType = Scalar;
        Function->ParamTypes = {Scalar};
        PointerContext Debug;
        FunctionSym Round;
        Round.Addr = 0x2000;
        Round.Name = "round_trip";
        Round.ReturnType = PointerType;
        Round.Params = {{"address", PointerType}};
        Debug.Functions[Round.Addr] = Round;
        FunctionSym Call;
        Call.Addr = 0x3000;
        Call.Name = "apply_callback";
        Call.ReturnType = Scalar;
        Call.Params = {{"callback", NdType::makePtr(Function)},
                       {"value", Scalar}};
        Debug.Functions[Call.Addr] = Call;
        CEmitterOptions Options;
        Options.TheArch = Arch;
        Options.Format = Format;
        Options.PreserveLLVMFunctionTypes = Preserved;
        Options.RecordDefinitions = [Alias](const std::vector<TypeRef> &) {
          CRecordDefinitions Definitions;
          if (Alias) {
            Definitions.Text = "typedef uint64_t *WORD_POINTER;\n";
            Definitions.Identifiers = {"WORD_POINTER"};
          }
          return Definitions;
        };
        std::string Text;
        llvm::raw_string_ostream Out(Text);
        ASSERT_TRUE(LLVMCEmitter().emit(M, Out, Options, &Debug, nullptr,
                                        Selected ? Caller : nullptr));
        Out.flush();
        if (Selected)
          Text += R"c(
uint64_t round_trip(uint64_t address) { return address; }
uint64_t apply_callback(uint64_t callback, uint64_t value) {
  return ((uint64_t (*)(uint64_t))(uintptr_t)callback)(value);
}
uint64_t add_seven(uint64_t value) { return value + 7; }
)c";
        Text += R"c(
int main(void) {
  uint64_t value = 11;
  if (bridge_entry((uintptr_t)&value) != 18) return 1;
  value = 71;
  return bridge_entry((uintptr_t)&value) != 78;
}
)c";
        for (const auto Optimization : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(
              Text, {Optimization, "-Werror=int-conversion",
                     "-Werror=incompatible-pointer-types",
                     "-fsanitize=undefined", "-fsanitize-trap=all"});
      }
}
} // namespace
