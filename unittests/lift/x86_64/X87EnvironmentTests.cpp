//===- X87EnvironmentTests.cpp - Native x87 environment formats -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve legacy image formats through decoding, native LLVM and both C
/// routes.
//===----------------------------------------------------------------------===//

#include "LLVMHostFixture.h"
#include "X86FPStateAccuracyFixture.h"

#include "neverd/backend/llvm/X86/LLVMX87NativeState.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"

namespace {
constexpr Intrinsic EnvironmentIds[] = {
    Intrinsic::X87Fnstenv16, Intrinsic::X87Fnstenv,  Intrinsic::X87Fldenv16,
    Intrinsic::X87Fldenv,    Intrinsic::X87Fnsave16, Intrinsic::X87Fnsave,
    Intrinsic::X87Frstor16,  Intrinsic::X87Frstor};

TEST(X87Environment,
     DecodeRoutesPreserveOperandFormatIndependentlyOfAddresses) {
  for (Arch Mode : {Arch::X86, Arch::X64})
    for (unsigned Family = 0; Family < 4; ++Family)
      for (bool Short : {false, true})
        for (bool AddressOverride : {false, true})
          for (bool RexW : {false, true})
            for (unsigned Route = 0; Route < 3; ++Route) {
              if (RexW && Mode == Arch::X86)
                continue;
              std::vector<uint8_t> Bytes;
              if (Short)
                Bytes.push_back(0x66);
              if (AddressOverride)
                Bytes.push_back(0x67);
              if (RexW)
                Bytes.push_back(0x48);
              Bytes.push_back(Family < 2 ? 0xd9 : 0xdd);
              Bytes.push_back(Family & 1 ? 0x20 : 0x30);
              Decoder D;
              ASSERT_TRUE(D.init(Mode));
              D.setX87ContextOwnership(false);
              D.setStrict(true);
              DecodedInsn Insn{};
              const auto Size =
                  Route == 0
                      ? D.decodeOne(Bytes.data(), Bytes.size(), Entry, Insn)
                  : Route == 1 ? D.decodeOneLight(Bytes.data(), Bytes.size(),
                                                  Entry, Insn)
                               : D.decodeOneForLift(Bytes.data(), Bytes.size(),
                                                    Entry, Insn);
              ASSERT_EQ(Size, Bytes.size()) << testing::PrintToString(Bytes);
              std::vector<LowOp> Ops;
              ASSERT_NO_THROW(D.liftToLow(Insn, Ops));
              unsigned Found = 0;
              for (const auto &Op : Ops)
                if (Op.Opcode == NdOp::INTRINSIC) {
                  ++Found;
                  ASSERT_EQ(Op.NumInputs, 2);
                  EXPECT_EQ(static_cast<Intrinsic>(Op.Inputs[0].Offset),
                            EnvironmentIds[Family * 2 + !(Short && !RexW)]);
                }
              EXPECT_EQ(Found, 1u);
            }
}

TEST(X87Environment, SegmentOverridesRemainInstructionOperandsInBothCRoutes) {
  for (auto Id : EnvironmentIds)
    for (auto Space :
         {NdMemoryAddressSpace::X86FS, NdMemoryAddressSpace::X86GS}) {
      const auto &Spec = *x87NativeStateOfIntrinsic(Id);
      const char *Segment = Space == NdMemoryAddressSpace::X86FS ? "fs" : "gs";
      HighExpr Call;
      Call.Kind = ExprKind::Call;
      Call.IntrinsicId = Id;
      Call.MemoryAddressSpace = Space;
      Call.Operands = {HighExpr::makeConst(0, 8)};
      const auto Statement = renderX86SegmentedIntrinsicStatement(
          Arch::X64, Call, nullptr, [](const HighExpr &) { return "offset"; },
          [](const MedVar &) { return "unused"; });
      EXPECT_EQ(Statement.find(Spec.Builtin), std::string::npos);
      EXPECT_NE(Statement.find("%%" + std::string(Segment) + ":(%0)"),
                std::string::npos);
      llvm::LLVMContext Context;
      llvm::Module Module("segmented-environment", Context);
      auto *Type =
          llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                  {llvm::Type::getInt64Ty(Context)}, false);
      auto *Function = llvm::Function::Create(
          Type, llvm::GlobalValue::ExternalLinkage, "probe", Module);
      llvm::IRBuilder<> Builder(
          llvm::BasicBlock::Create(Context, "entry", Function));
      Builder.CreateCall(
          llvm::InlineAsm::get(
              Type, std::string(Spec.Mnemonic) + " %" + Segment + ":($0)",
              "r," + x87StateConstraints(x87NativeStateEffect(Spec)).str(),
              true),
          {Function->getArg(0)});
      Builder.CreateRetVoid();
      CEmitterOptions Options;
      Options.TheArch = Arch::X64;
      Options.PreserveLLVMFunctionTypes = true;
      std::string Source;
      llvm::raw_string_ostream Stream(Source);
      ASSERT_TRUE(LLVMCEmitter().emit(Module, Stream, Options));
      EXPECT_EQ(Source.find(Spec.Builtin), std::string::npos);
      EXPECT_NE(Source.find("%%" + std::string(Segment) + ":(%0)"),
                std::string::npos);
      if (Spec.ReplacesBank) {
        EXPECT_NE(Statement.find("\"mm7\""), std::string::npos);
        EXPECT_NE(Source.find("\"mm7\""), std::string::npos);
      }
    }
}

class X87EnvironmentProjection : public X86FPStateFixture {
protected:
  void check(llvm::StringRef SourceCompiler, bool Legacy) {
#if defined(__x86_64__) && defined(__linux__)
    llvm::LLVMContext Context;
    llvm::Module Module("environment", Context);
    Module.setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
    auto *Ptr = llvm::PointerType::get(Context, 0);
    auto *Type =
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), {Ptr}, false);
    std::string HighC;
    for (unsigned Index = 0; Index < std::size(EnvironmentIds); ++Index) {
      const auto &Spec = *x87NativeStateOfIntrinsic(EnvironmentIds[Index]);
      auto *Function =
          llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                                 "llvmmemory" + std::to_string(Index), Module);
      llvm::IRBuilder<> Builder(
          llvm::BasicBlock::Create(Context, "entry", Function));
      if (Legacy) {
        auto *Address =
            Builder.CreatePtrToInt(Function->getArg(0), Builder.getInt64Ty());
        auto *AsmType = llvm::FunctionType::get(Builder.getVoidTy(),
                                                {Builder.getInt64Ty()}, false);
        Builder.CreateCall(
            llvm::InlineAsm::get(
                AsmType, std::string(Spec.Mnemonic) + " ($0)",
                "r," + x87StateConstraints(x87NativeStateEffect(Spec)).str(),
                true),
            {Address});
      } else {
        Builder.CreateCall(Module.getOrInsertFunction(Spec.LLVMName, Type),
                           {Function->getArg(0)});
      }
      Builder.CreateRetVoid();
      HighExpr Call;
      Call.Kind = ExprKind::Call;
      Call.IntrinsicId = EnvironmentIds[Index];
      Call.Operands = {HighExpr::makeConst(0, 8)};
      const auto Statement = renderX86SegmentedIntrinsicStatement(
          Arch::X64, Call, nullptr, [](const HighExpr &) { return "p"; },
          [](const MedVar &) { return "unused"; });
      EXPECT_NE(Statement.find(Spec.Builtin), std::string::npos);
      HighC += "void highcmemory" + std::to_string(Index) + "(void *p) { " +
               Statement + " }\n";
    }
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.PreserveLLVMFunctionTypes = true;
    std::string Source;
    llvm::raw_string_ostream Stream(Source);
    ASSERT_TRUE(LLVMCEmitter().emit(Module, Stream, Options));
    for (auto Id : EnvironmentIds)
      EXPECT_NE(Source.find(x87NativeStateOfIntrinsic(Id)->Builtin),
                std::string::npos);
    Source += HighC;
    Source += R"(
#include <string.h>
typedef void (*operation)(void *);
extern void invoke(operation, void *, const void *, void *);
#define ORIGINAL(name, bytes) \
  ".globl original" #name "\noriginal" #name ":\n.byte " bytes "\nret\n"
extern void original0(void *), original1(void *), original2(void *), original3(void *);
extern void original4(void *), original5(void *), original6(void *), original7(void *);
__asm__(".text\n"
  ORIGINAL(0, "0x66,0xd9,0x37") ORIGINAL(1, "0xd9,0x37")
  ORIGINAL(2, "0x66,0xd9,0x27") ORIGINAL(3, "0xd9,0x27")
  ORIGINAL(4, "0x66,0xdd,0x37") ORIGINAL(5, "0xdd,0x37")
  ORIGINAL(6, "0x66,0xdd,0x27") ORIGINAL(7, "0xdd,0x27")
  ".globl invoke\ninvoke:\n"
  "push %rbx\npush %r12\npush %r13\nsub $512,%rsp\n"
  "mov %rdx,%rbx\nmov %rcx,%r12\nmov %rdi,%r13\nmov %rsi,%rdi\n"
  "fxsave64 (%rsp)\nfxrstor64 (%rbx)\ncall *%r13\n"
  "fxsave64 (%r12)\nfxrstor64 (%rsp)\n"
  "add $512,%rsp\npop %r13\npop %r12\npop %rbx\nret\n");
#define LIST(prefix) prefix##0, prefix##1, prefix##2, prefix##3, prefix##4, prefix##5, prefix##6, prefix##7
int main(void) {
  operation functions[3][8] = {{LIST(original)}, {LIST(llvmmemory)}, {LIST(highcmemory)}};
  for (unsigned top = 0; top < 8; ++top)
    for (unsigned tags = 0; tags < 3; ++tags)
      for (unsigned kind = 0; kind < 8; ++kind) {
        _Alignas(16) unsigned char before[512] = {0}, after[3][512] = {{0}};
        unsigned char areas[3][128];
        before[0] = 0x7f; before[1] = 3; before[3] = top << 3;
        before[4] = (unsigned char[]){0,0x55,0xff}[tags];
        before[24] = 0x80; before[25] = 0x1f;
        for (unsigned reg = 0; reg < 8; ++reg) {
          before[32 + reg * 16] = reg + 1;
          before[39 + reg * 16] = 0x80;
          before[40 + reg * 16] = 0xff; before[41 + reg * 16] = 0x3f;
        }
        memset(areas, 0xa5, sizeof(areas));
        for (unsigned implementation = 0; implementation < 3; ++implementation) {
          unsigned char *area = areas[implementation] + 3;
          if (kind == 2 || kind == 3 || kind >= 6) {
            const unsigned stride = kind & 1 ? 4 : 2;
            area[0] = 0x7f; area[1] = 3;
            area[stride] = 0; area[stride + 1] = ((top + 3) & 7) << 3;
            area[2 * stride] = area[2 * stride + 1] = 0xff;
          }
          invoke(functions[implementation][kind], area, before, after[implementation]);
          if (kind < 2) after[implementation][3] &= 0xb8;
          if (memcmp(areas[0], areas[implementation], 128) ||
              memcmp(after[0], after[implementation], 24)) return 1;
          for (unsigned reg = 0; reg < 8; ++reg)
            if (memcmp(after[0] + 32 + reg * 16, after[implementation] + 32 + reg * 16, 10)) return 2;
        }
      }
  return 0;
}
)";
    const auto Unit = file("environment.c");
    write(Unit, Source);
    retainFile(Legacy ? "legacy-environment.c" : "native-environment.c", Unit);
    for (const char *Opt : {"-O0", "-O2"}) {
      const auto Object = file(std::string("environment") + Opt + ".o");
      const auto Binary = file(std::string("environment") + Opt + ".exe");
      auto R =
          command(SourceCompiler, {Opt, "-fno-lto", "-fno-builtin-mimalloc",
                                   "-c", Unit, "-o", Object});
      ASSERT_EQ(R.Status, 0) << R.Error << Source;
      R = command(Compiler, {Object, "-o", Binary});
      ASSERT_EQ(R.Status, 0) << R.Error;
      R = command(Binary, {});
      EXPECT_EQ(R.Status, 0) << R.Out << R.Error;
    }
#else
    GTEST_SKIP() << "native Linux x64 execution required";
#endif
  }
};

TEST_F(X87EnvironmentProjection, NativeAndLegacyLLVMCallsExecuteThroughClang) {
  check(Compiler, false);
  check(Compiler, true);
}

TEST_F(X87EnvironmentProjection, NativeAndLegacyLLVMCallsExecuteThroughNeverC) {
  const char *NeverC = std::getenv("NEVERD_TEST_NEVERC");
  if (!NeverC || !*NeverC)
    GTEST_SKIP() << "NEVERD_TEST_NEVERC is required for native builtins";
  check(NeverC, false);
  check(NeverC, true);
}

TEST_F(X87EnvironmentProjection,
       UnownedSourceLLVMSelectsRegisteredImageFormats) {
  X87ContextOverride = false;
  for (unsigned Index = 0; Index < 8; ++Index) {
    std::vector<uint8_t> Bytes;
    if (!(Index & 1))
      Bytes.push_back(0x66);
    Bytes.push_back(Index < 4 ? 0xd9 : 0xdd);
    Bytes.push_back((Index / 2) & 1 ? 0x27 : 0x37);
    Bytes.push_back(0xc3);
    const auto &Spec = *x87NativeStateOfIntrinsic(EnvironmentIds[Index]);
    auto Image = image(Bytes);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.LiftMode = true;
    Options.SourceProjection = true;
    Options.NoOpt = true;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Entry};
    auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Spec.LLVMName << Result.Error;
    unsigned Found = 0;
    for (const auto &F : *Result.LlvmModule)
      for (const auto &B : F)
        for (const auto &I : B)
          if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&I)) {
            if (const auto *Callee = Call->getCalledFunction())
              Found += Callee->getName() == Spec.LLVMName;
            else if (const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(
                         Call->getCalledOperand())) {
              if (Asm->getAsmString() != std::string(Spec.Mnemonic) + " ($0)")
                continue;
              ++Found;
              EXPECT_EQ(llvm::Intrinsic::lookupIntrinsicID(Spec.LLVMName),
                        llvm::Intrinsic::not_intrinsic);
              EXPECT_EQ(
                  Asm->getConstraintString(),
                  "r," + x87StateConstraints(x87NativeStateEffect(Spec)).str());
            }
          }
    EXPECT_EQ(Found, 1u) << Spec.LLVMName;
  }
}

TEST_F(X87EnvironmentProjection, HostFixtureKeepsStateInAnIndependentClone) {
  for (const char *Triple :
       {"i386-unknown-linux-gnu", "x86_64-unknown-linux-gnu"}) {
    llvm::LLVMContext Context;
    llvm::Module Module("host-state-fixture", Context);
    Module.setTargetTriple(llvm::Triple(Triple));
    auto *Ptr = llvm::PointerType::get(Context, 0);
    auto *Type =
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), {Ptr}, false);
    auto *Function = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, "probe", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    for (const auto &Spec : X87NativeStateSpecs) {
      auto *Return =
          Spec.ReturnsStatus ? Builder.getInt16Ty() : Builder.getVoidTy();
      auto *CallType = llvm::FunctionType::get(
          Return,
          Spec.HasPointer ? llvm::ArrayRef<llvm::Type *>(Ptr)
                          : llvm::ArrayRef<llvm::Type *>(),
          false);
      auto Callee = Module.getOrInsertFunction(Spec.LLVMName, CallType);
      if (Spec.HasPointer)
        Builder.CreateCall(Callee, {Function->getArg(0)});
      else
        Builder.CreateCall(Callee);
    }
    Builder.CreateRetVoid();
    std::string Original;
    llvm::raw_string_ostream OriginalOut(Original);
    Module.print(OriginalOut, nullptr);
    const auto Fixture = neverd::test::printHostCompilerFixture(Module);
    std::string Unchanged;
    llvm::raw_string_ostream UnchangedOut(Unchanged);
    Module.print(UnchangedOut, nullptr);
    EXPECT_EQ(Original, Unchanged);
    llvm::SMDiagnostic Diagnostic;
    auto Parsed = llvm::parseAssemblyString(Fixture, Diagnostic, Context);
    ASSERT_TRUE(Parsed) << Fixture;
    ASSERT_FALSE(llvm::verifyModule(*Parsed, &llvm::errs()));
    unsigned Count = 0;
    for (const auto &Block : *Parsed->getFunction("probe"))
      for (const auto &Instruction : Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction)) {
          const auto *Assembly =
              llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand());
          ASSERT_TRUE(Assembly);
          EXPECT_TRUE(Assembly->hasSideEffects());
          ++Count;
        }
    EXPECT_EQ(Count, std::size(X87NativeStateSpecs));
    const auto Input = file("host-state.ll");
    write(Input, Fixture);
    for (const char *Opt : {"-O0", "-O2"}) {
      const auto Output = file("host-state.o");
      const auto Result = command(Compiler, {std::string("--target=") + Triple,
                                             Opt, "-c", Input, "-o", Output});
      EXPECT_EQ(Result.Status, 0) << Result.Error << Fixture;
    }
  }
}
} // namespace
