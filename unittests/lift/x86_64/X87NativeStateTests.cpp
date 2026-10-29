//===- X87NativeStateTests.cpp - Native x87 state projections ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate native state-call admission and compare generated C with opcodes.
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

#include "neverd/backend/llvm/X86/LLVMX86X87StateAsm.h"
#include "neverd/backend/llvm/X86/LLVMX87NativeState.h"

#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"

#include <cstdlib>
#include <stdexcept>

namespace {
TEST(X87NativeStateContract, RejectsMalformedCallsBeforeFunctionFiltering) {
  for (const auto &Spec : X87NativeStateSpecs)
    for (unsigned Change = 0; Change < 12; ++Change)
      for (bool HideBody : {false, true}) {
        // Non-pointer contracts have no address-space or pointer-type variant.
        if (!Spec.HasPointer && (Change == 4 || Change == 5))
          continue;
        SCOPED_TRACE(testing::Message()
                     << Spec.LLVMName << ':' << Change << ':' << HideBody);
        llvm::LLVMContext Context;
        llvm::Module Module("invalid-state", Context);
        auto *Void = llvm::Type::getVoidTy(Context);
        llvm::Type *Return =
            Spec.ReturnsStatus ? llvm::Type::getInt16Ty(Context) : Void;
        if (Change == 1)
          Return = llvm::Type::getInt32Ty(Context);
        llvm::Type *Argument =
            llvm::PointerType::get(Context, Change == 5 ? 1 : 0);
        if (Change == 4)
          Argument = llvm::Type::getInt16Ty(Context);
        const unsigned Count = Change == 2   ? unsigned(!Spec.HasPointer)
                               : Change == 3 ? 2
                                             : unsigned(Spec.HasPointer);
        auto *Type = llvm::FunctionType::get(
            Return, std::vector<llvm::Type *>(Count, Argument), Change == 6);
        auto *Callee = llvm::Function::Create(
            Type, llvm::GlobalValue::ExternalLinkage, Spec.LLVMName, Module);
        auto *BodyType = llvm::FunctionType::get(Void, false);
        auto *Body = llvm::Function::Create(
            BodyType, llvm::GlobalValue::ExternalLinkage, "probe", Module);
        auto *Visible = llvm::Function::Create(
            BodyType, llvm::GlobalValue::ExternalLinkage, "visible", Module);
        llvm::IRBuilder<> Builder(
            llvm::BasicBlock::Create(Context, "entry", Visible));
        Builder.CreateRetVoid();
        Builder.SetInsertPoint(
            llvm::BasicBlock::Create(Context, "entry", Body));
        std::vector<llvm::Value *> Args(Count,
                                        llvm::Constant::getNullValue(Argument));
        if (Change == 9) {
          auto *Normal = llvm::BasicBlock::Create(Context, "normal", Body);
          auto *Unwind = llvm::BasicBlock::Create(Context, "unwind", Body);
          Builder.CreateInvoke(Callee, Normal, Unwind, Args);
          Builder.SetInsertPoint(Unwind);
          Builder.CreateUnreachable();
          Builder.SetInsertPoint(Normal);
        } else if (Change == 10) {
          auto *Mismatched =
              llvm::FunctionType::get(Void, {Builder.getInt32Ty()}, false);
          Builder.CreateCall(Mismatched, Callee, {Builder.getInt32(0)});
        } else if (Change == 11) {
          auto *Alias = llvm::GlobalAlias::create(
              Type, 0, llvm::GlobalValue::ExternalLinkage, "alias", Callee,
              &Module);
          Builder.CreateCall(Type, Alias, Args);
        } else {
          auto *Call = Builder.CreateCall(Callee, Args);
          if (Change == 7) {
            Callee->setCallingConv(llvm::CallingConv::Fast);
            Call->setCallingConv(llvm::CallingConv::Fast);
          }
        }
        Builder.CreateRetVoid();
        if (Change == 8) {
          Builder.SetInsertPoint(
              llvm::BasicBlock::Create(Context, "entry", Callee));
          if (Return->isVoidTy())
            Builder.CreateRetVoid();
          else
            Builder.CreateRet(llvm::Constant::getNullValue(Return));
        }
        CEmitterOptions Options;
        Options.TheArch = Change == 0 ? Arch::AArch64 : Arch::X64;
        std::string Source;
        llvm::raw_string_ostream Stream(Source);
        EXPECT_THROW(LLVMCEmitter().emit(Module, Stream, Options, nullptr,
                                         nullptr, HideBody ? Visible : nullptr),
                     std::invalid_argument);
      }
}

class X87NativeStateProjection : public X86FPStateFixture {
protected:
  void checkProjection(llvm::StringRef SourceCompiler, bool Legacy) {
#if defined(__x86_64__) && defined(__linux__)
    llvm::LLVMContext Context;
    llvm::Module Module("native-state", Context);
    Module.setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
    auto *Ptr = llvm::PointerType::get(Context, 0);
    auto *Void = llvm::Type::getVoidTy(Context);
    auto *Type = llvm::FunctionType::get(Void, {Ptr}, false);
    auto *Function = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, "state_probe", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    const auto Read = Module.getOrInsertFunction(
        "llvm.x86.fnstsw",
        llvm::FunctionType::get(Builder.getInt16Ty(), false));
    const auto Pointer = [&](unsigned Offset) {
      return Builder.CreateGEP(Builder.getInt8Ty(), Function->getArg(0),
                               Builder.getInt64(Offset));
    };
    const auto Save = [&](unsigned Offset) {
      Builder.CreateStore(Builder.CreateCall(Read), Pointer(Offset))
          ->setAlignment(llvm::Align(1));
    };
    const auto State = [&](Intrinsic Id) {
      auto *NoArgs = llvm::FunctionType::get(Void, false);
      if (Legacy)
        Builder.CreateCall(llvm::InlineAsm::get(
            NoArgs, intrinsicAsmMnemonic(Id),
            x87StateConstraints(*x87StateEffectOfIntrinsic(Id)), true));
      else
        Builder.CreateCall(Module.getOrInsertFunction(
            x87NativeStateOfIntrinsic(Id)->LLVMName, NoArgs));
    };
    Builder.CreateCall(Read); // Dead reads must still execute.
    Save(0);
    Builder.CreateCall(Module.getOrInsertFunction("llvm.x86.fnstsw.mem", Type),
                       {Pointer(3)});
    State(Intrinsic::X87Fnclex);
    Save(6);
    State(Intrinsic::X87Fninit);
    Save(8);
    State(Intrinsic::X87Wait);
    Builder.CreateRetVoid();
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.Format = hostFormat();
    Options.PreserveLLVMFunctionTypes = true;
    std::string Source;
    llvm::raw_string_ostream Stream(Source);
    ASSERT_TRUE(LLVMCEmitter().emit(Module, Stream, Options));
    for (const auto &Spec : X87NativeStateSpecs)
      if (!Spec.ImageBytes && Spec.Kind != X87NativeStateKind::ReadControl &&
          Spec.Kind != X87NativeStateKind::WriteControl)
        EXPECT_NE(Source.find(Spec.Builtin), std::string::npos);
    // HighC shares the zero-operand builtin/fallback path with LLVMC.
    for (Intrinsic Id :
         {Intrinsic::X87Fnclex, Intrinsic::X87Fninit, Intrinsic::X87Wait}) {
      bool HasCIntrinsics = false;
      const auto Call =
          renderIntrinsicCall(Id, Arch::X64, {}, 0, HasCIntrinsics, true);
      EXPECT_NE(Call.find(x87NativeStateOfIntrinsic(Id)->Builtin),
                std::string::npos);
      Source += "void highc_" + std::string(intrinsicAsmMnemonic(Id)) +
                "(void) { " + Call + "; }\n";
    }
    const auto Unit = file("state.c");
    write(Unit, Source + R"(
#include <string.h>
extern void original(void *), invoke(void (*)(void *),
    unsigned char *, const void *, void *);
__asm__(".text\n.globl original\noriginal:\n"
        ".byte 0xdf,0xe0\n.byte 0xdf,0xe0\nmovw %ax,0(%rdi)\n"
        ".byte 0xdd,0x7f,0x03\n.byte 0xdb,0xe2\n"
        ".byte 0xdf,0xe0\nmovw %ax,6(%rdi)\n.byte 0xdb,0xe3\n"
        ".byte 0xdf,0xe0\nmovw %ax,8(%rdi)\n.byte 0x9b\nret\n"
        ".globl invoke\ninvoke:\n"
        "push %rbx\npush %r12\npush %r13\nsub $512,%rsp\n"
        "mov %rdx,%rbx\nmov %rcx,%r12\nmov %rdi,%r13\nmov %rsi,%rdi\n"
        "fxsave64 (%rsp)\nfxrstor64 (%rbx)\ncall *%r13\n"
        "fxsave64 (%r12)\nfxrstor64 (%rsp)\n"
        "add $512,%rsp\npop %r13\npop %r12\npop %rbx\nret\n");
int main(void) {
  for (unsigned top = 0; top < 8; ++top)
    for (unsigned tags = 0; tags < 3; ++tags)
      for (unsigned rc = 0; rc < 4; ++rc) {
        _Alignas(16) unsigned char before[512] = {0}, after[2][512] = {{0}};
        unsigned char output[2][16];
        memset(output, 0xa5, sizeof(output));
        const unsigned short cw = 0x37f | rc << 10, sw = 0x4765 | top << 11;
        memcpy(before, &cw, 2); memcpy(before + 2, &sw, 2);
        before[4] = (unsigned char[]){0,0x55,0xff}[tags];
        before[24] = 0x80; before[25] = 0x1f;
        for (unsigned reg = 0; reg < 8; ++reg) {
          before[32 + reg * 16] = reg + 1;
          before[39 + reg * 16] = 0x80;
          before[40 + reg * 16] = 0xff;
          before[41 + reg * 16] = 0x3f;
        }
        invoke(original, output[0], before, after[0]);
        invoke(state_probe, output[1], before, after[1]);
        // C0-C3 after FNCLEX are undefined; compare only defined status bits.
        output[0][7] &= 0xb8; output[1][7] &= 0xb8;
        if (memcmp(output[0], output[1], 16) || memcmp(after[0], after[1], 24))
          return 1;
        for (unsigned reg = 0; reg < 8; ++reg)
          if (memcmp(after[0] + 32 + reg * 16, after[1] + 32 + reg * 16, 10))
            return 2;
      }
  highc_fnclex(); highc_fninit(); highc_fwait();
  return 0;
}
)");
    retainFile(Legacy ? "legacy-state.c" : "native-state.c", Unit);
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      const auto Object = file(std::string("state") + Optimization + ".o");
      const auto Binary = file(std::string("state") + Optimization + ".exe");
      auto Result = command(SourceCompiler,
                            {Optimization, "-fno-lto", "-fno-builtin-mimalloc",
                             "-c", Unit, "-o", Object});
      ASSERT_EQ(Result.Status, 0) << Result.Error << Source;
      Result = command(Compiler, {Object, "-o", Binary});
      ASSERT_EQ(Result.Status, 0) << Result.Error;
      Result = command(Binary, {});
      EXPECT_EQ(Result.Status, 0) << Result.Out << Result.Error;
    }
#else
    GTEST_SKIP() << "native Linux x64 state execution required";
#endif
  }
};

TEST_F(X87NativeStateProjection, NativeAndLegacyCallsExecuteThroughClang) {
  for (bool Legacy : {false, true})
    checkProjection(Compiler, Legacy);
}

TEST_F(X87NativeStateProjection, NativeAndLegacyCallsExecuteThroughNeverC) {
  const char *NeverC = std::getenv("NEVERD_TEST_NEVERC");
  if (!NeverC || !*NeverC)
    GTEST_SKIP()
        << "NEVERD_TEST_NEVERC is required for native builtin coverage";
  for (bool Legacy : {false, true})
    checkProjection(NeverC, Legacy);
}

TEST_F(X87NativeStateProjection, TypedOperationsSelectRegisteredIntrinsics) {
  X87ContextOverride = false;
  for (bool NoOpt : {false, true}) {
    auto Image = image({0xdb, 0xe2, 0xdb, 0xe3, 0x9b, 0xc3});
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.LiftMode = true;
    Options.SourceProjection = true;
    Options.NoOpt = NoOpt;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Entry};
    const auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_NE(Result.LlvmModule, nullptr);
    for (Intrinsic Id :
         {Intrinsic::X87Fnclex, Intrinsic::X87Fninit, Intrinsic::X87Wait}) {
      const auto *Spec = x87NativeStateOfIntrinsic(Id);
      const bool Registered =
          llvm::Intrinsic::lookupIntrinsicID(Spec->LLVMName);
      unsigned Count = 0;
      for (const auto &Function : *Result.LlvmModule)
        for (const auto &Block : Function)
          for (const auto &Instruction : Block)
            if (const auto *Call =
                    llvm::dyn_cast<llvm::CallInst>(&Instruction)) {
              if (const auto *Callee = Call->getCalledFunction())
                Count += Registered && Callee->getName() == Spec->LLVMName;
              else if (const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(
                           Call->getCalledOperand()))
                Count += !Registered && Asm->getAsmString() == Spec->Mnemonic;
            }
      EXPECT_EQ(Count, 1U) << Spec->LLVMName;
    }
  }
}
} // namespace
