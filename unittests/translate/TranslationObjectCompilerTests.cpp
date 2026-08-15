//===- TranslationObjectCompilerTests.cpp - Object compiler boundary -----===//

#include "gtest/gtest.h"

#include "neverd/translate/RuntimeHelpers.h"
#include "neverd/translate/RuntimeSymbolRegistry.h"
#include "neverd/translate/TranslationArtifactVerifier.h"
#include "neverd/translate/TranslationObjectCompiler.h"
#include "neverd/translate/TranslationTargetMachine.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::translate;

namespace {

TranslationOptions explicitOptions(GuestArchitecture Host,
                                   llvm::StringRef Triple) {
  TranslationOptions Options;
  switch (Host) {
  case GuestArchitecture::X86_32:
  case GuestArchitecture::X86_64:
    Options.Guest = GuestArchitecture::ARM32;
    break;
  case GuestArchitecture::ARM32:
    Options.Guest = GuestArchitecture::X86_32;
    break;
  case GuestArchitecture::AArch64:
    Options.Guest = GuestArchitecture::X86_64;
    break;
  }
  Options.Mode = TranslationMode::AOT;
  Options.Target.Kind = HostTargetKind::Explicit;
  Options.Target.Architecture = Host;
  Options.Target.Triple = Triple.str();
  Options.Optimization = TranslationOptimizationPolicy::None;
  Options.LLVMLevel = LLVMOptimizationLevel::O0;
  Options.BlockCache = BlockCachePolicy::Disabled;
  Options.CodeInvalidation = CodeInvalidationPolicy::RejectExecutableWrites;
  return Options;
}

std::unique_ptr<llvm::Module>
makeCanonicalModule(llvm::LLVMContext &Context,
                    const TranslationOptions &Options,
                    llvm::ArrayRef<llvm::StringRef> BlockNames) {
  llvm::Expected<TranslationTargetMachineV1> TargetOrErr =
      createTranslationTargetMachineV1(Options);
  if (!TargetOrErr) {
    ADD_FAILURE() << llvm::toString(TargetOrErr.takeError());
    return nullptr;
  }
  TranslationTargetMachineV1 Target = std::move(*TargetOrErr);

  auto Module =
      std::make_unique<llvm::Module>("translation-object-test", Context);
  Module->setTargetTriple(llvm::Triple(Target.hostTarget().triple()));
  Module->setDataLayout(Target.dataLayout());
  llvm::Type *Pointer = llvm::PointerType::getUnqual(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::FunctionType *Type =
      llvm::FunctionType::get(I32, {Pointer, Pointer}, false);
  for (llvm::StringRef Name : BlockNames) {
    llvm::Function *Function = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, Name, Module.get());
    Function->setVisibility(llvm::GlobalValue::HiddenVisibility);
    Function->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::BasicBlock *Entry =
        llvm::BasicBlock::Create(Context, "entry", Function);
    llvm::IRBuilder<> Builder(Entry);
    Builder.CreateRet(llvm::ConstantInt::get(I32, 0));
  }
  return Module;
}

std::unique_ptr<llvm::Module>
makeSemanticModule(llvm::LLVMContext &Context,
                   const TranslationOptions &Options) {
  constexpr llvm::StringLiteral BlockName("translated_block");
  std::unique_ptr<llvm::Module> Module =
      makeCanonicalModule(Context, Options, {BlockName});
  if (!Module)
    return nullptr;

  llvm::Function *Function = Module->getFunction(BlockName);
  Function->deleteBody();
  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::IRBuilder<> Builder(Entry);
  llvm::Value *State = Function->getArg(0);
  llvm::Type *I8 = llvm::Type::getInt8Ty(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::Value *XPointer = Builder.CreateGEP(I8, State, Builder.getInt64(0));
  llvm::Value *YPointer = Builder.CreateGEP(I8, State, Builder.getInt64(4));
  llvm::Value *ResultPointer =
      Builder.CreateGEP(I8, State, Builder.getInt64(8));
  llvm::Value *X = Builder.CreateAlignedLoad(I32, XPointer, llvm::Align(4));
  llvm::Value *Y = Builder.CreateAlignedLoad(I32, YPointer, llvm::Align(4));
  llvm::Value *Xor = Builder.CreateXor(X, Y);
  llvm::Value *And = Builder.CreateAnd(X, Y);
  llvm::Value *Twice = Builder.CreateMul(And, Builder.getInt32(2));
  llvm::Value *CarrySave = Builder.CreateAdd(Xor, Twice);
  Builder.CreateAlignedStore(CarrySave, ResultPointer, llvm::Align(4));
  Builder.CreateRet(Builder.getInt32(0));
  return Module;
}

std::unique_ptr<llvm::Module>
makeRuntimeCallModule(llvm::LLVMContext &Context,
                      const TranslationOptions &Options) {
  constexpr llvm::StringLiteral BlockName("translated_block");
  std::unique_ptr<llvm::Module> Module =
      makeCanonicalModule(Context, Options, {BlockName});
  if (!Module)
    return nullptr;

  llvm::Function *Function = Module->getFunction(BlockName);
  Function->deleteBody();
  llvm::Type *Pointer = llvm::PointerType::getUnqual(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::Type *I64 = llvm::Type::getInt64Ty(Context);
  llvm::FunctionType *HelperType =
      llvm::FunctionType::get(I32, {Pointer, I64, I64, I32}, false);
  llvm::Function *Helper =
      llvm::Function::Create(HelperType, llvm::GlobalValue::ExternalLinkage,
                             "nvd_rt_v1_store32_le", Module.get());
  Helper->addFnAttr(llvm::Attribute::NoUnwind);

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Success =
      llvm::BasicBlock::Create(Context, "success", Function);
  llvm::BasicBlock *Failure =
      llvm::BasicBlock::Create(Context, "failure", Function);
  llvm::IRBuilder<> Builder(Entry);
  llvm::CallInst *Status =
      Builder.CreateCall(Helper, {Function->getArg(1), Builder.getInt64(0x1000),
                                  Builder.getInt64(7), Builder.getInt32(4)});
  Status->addFnAttr(llvm::Attribute::NoUnwind);
  Builder.CreateCondBr(Builder.CreateICmpEQ(Status, Builder.getInt32(0)),
                       Success, Failure);
  Builder.SetInsertPoint(Success);
  Builder.CreateRet(Builder.getInt32(0));
  Builder.SetInsertPoint(Failure);
  Builder.CreateRet(Status);
  return Module;
}

TranslationObjectPolicyV1
objectPolicy(llvm::ArrayRef<llvm::StringRef> BlockNames,
             llvm::ArrayRef<TranslationIRMemorySlot> StateSlots = {},
             uint64_t StateSize = 1) {
  TranslationObjectPolicyV1 Policy;
  Policy.StateSize = StateSize;
  Policy.StateSlots = StateSlots;
  Policy.RequiredBlockSymbols = BlockNames;
  return Policy;
}

std::string printModule(const llvm::Module &Module) {
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  Module.print(Stream, nullptr);
  return Text;
}

template <typename T>
void expectCompilerError(llvm::Expected<T> Result,
                         TranslationObjectCompilerErrorCode ExpectedCode) {
  ASSERT_FALSE(Result);
  bool SawCompilerError = false;
  llvm::Error Unhandled = llvm::handleErrors(
      Result.takeError(), [&](const TranslationObjectCompilerError &Error) {
        SawCompilerError = true;
        EXPECT_EQ(Error.code(), ExpectedCode);
        if (ExpectedCode ==
            TranslationObjectCompilerErrorCode::GeneratedCodeBudgetExceeded) {
          ASSERT_TRUE(Error.budgetObserved().has_value());
          ASSERT_TRUE(Error.budgetLimit().has_value());
          EXPECT_GT(*Error.budgetObserved(), *Error.budgetLimit());
        }
      });
  if (Unhandled)
    ADD_FAILURE() << llvm::toString(std::move(Unhandled));
  EXPECT_TRUE(SawCompilerError);
}

void expectObjectParses(const TranslationObjectArtifactV1 &Artifact) {
  const llvm::ArrayRef<uint8_t> Bytes = Artifact.bytes();
  const llvm::StringRef Contents(reinterpret_cast<const char *>(Bytes.data()),
                                 Bytes.size());
  llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> Object =
      llvm::object::ObjectFile::createObjectFile(
          llvm::MemoryBufferRef(Contents, "translation-object"));
  if (!Object) {
    ADD_FAILURE() << llvm::toString(Object.takeError());
    return;
  }
  EXPECT_TRUE((*Object)->isRelocatableObject());
}

void expectObjectVerifies(const TranslationObjectArtifactV1 &Artifact) {
  llvm::SmallVector<llvm::StringRef, 4> Blocks;
  llvm::SmallVector<llvm::StringRef, 8> Runtime;
  for (const TranslationObjectSymbolV1 &Symbol : Artifact.blockSymbols())
    Blocks.push_back(Symbol.ObjectName);
  for (const TranslationObjectSymbolV1 &Symbol : Artifact.runtimeSymbols())
    Runtime.push_back(Symbol.ObjectName);
  const TranslationArtifactPolicyV1 Policy(Blocks, Runtime);
  llvm::Error Error = verifyTranslationArtifact(
      Artifact.bytes(), llvm::Triple(Artifact.hostTarget().triple()), Policy);
  if (Error)
    ADD_FAILURE() << llvm::toString(std::move(Error));
}

TEST(TranslationObjectCompiler, EmitsParsableAuditedObjectsAtO0AndO2) {
  llvm::LLVMContext Context;
  constexpr llvm::StringLiteral BlockName("translated_block");
  TranslationOptions O0 =
      explicitOptions(GuestArchitecture::X86_64, "x86_64-pc-linux-gnu");
  O0.Optimization = TranslationOptimizationPolicy::ProvenSemanticAndLLVM;
  O0.LLVMLevel = LLVMOptimizationLevel::O0;
  std::unique_ptr<llvm::Module> Module =
      makeCanonicalModule(Context, O0, {BlockName});
  ASSERT_NE(Module, nullptr);
  const llvm::StringRef Blocks[] = {BlockName};
  const TranslationObjectPolicyV1 Policy = objectPolicy(Blocks);

  llvm::Expected<TranslationObjectArtifactV1> O0Artifact =
      compileTranslationObjectV1(*Module, O0, Policy);
  if (!O0Artifact)
    FAIL() << llvm::toString(O0Artifact.takeError());
  expectObjectParses(*O0Artifact);
  expectObjectVerifies(*O0Artifact);
  EXPECT_GT(O0Artifact->semanticReport().FunctionPassInvocations, 0u);

  TranslationOptions O2 = O0;
  O2.LLVMLevel = LLVMOptimizationLevel::O2;
  llvm::Expected<TranslationObjectArtifactV1> O2Artifact =
      compileTranslationObjectV1(*Module, O2, Policy);
  if (!O2Artifact)
    FAIL() << llvm::toString(O2Artifact.takeError());
  expectObjectParses(*O2Artifact);
  expectObjectVerifies(*O2Artifact);
  EXPECT_GT(O2Artifact->semanticReport().FunctionPassInvocations, 0u);
  EXPECT_NE(O0Artifact->requestCacheKey(), O2Artifact->requestCacheKey());
}

TEST(TranslationObjectCompiler, NeverMutatesTheInputOnSuccessOrFailure) {
  llvm::LLVMContext Context;
  constexpr llvm::StringLiteral BlockName("translated_block");
  TranslationOptions Options =
      explicitOptions(GuestArchitecture::X86_64, "x86_64-pc-linux-gnu");
  std::unique_ptr<llvm::Module> Module =
      makeCanonicalModule(Context, Options, {BlockName});
  ASSERT_NE(Module, nullptr);
  const llvm::StringRef Blocks[] = {BlockName};
  const TranslationObjectPolicyV1 Policy = objectPolicy(Blocks);
  const std::string Before = printModule(*Module);

  llvm::Expected<TranslationObjectArtifactV1> Artifact =
      compileTranslationObjectV1(*Module, Options, Policy);
  if (!Artifact)
    FAIL() << llvm::toString(Artifact.takeError());
  EXPECT_EQ(printModule(*Module), Before);

  Options.GeneratedCodeByteBudget = 1;
  expectCompilerError(
      compileTranslationObjectV1(*Module, Options, Policy),
      TranslationObjectCompilerErrorCode::GeneratedCodeBudgetExceeded);
  EXPECT_EQ(printModule(*Module), Before);
}

TEST(TranslationObjectCompiler, RequiresTheExactDefinitionManifest) {
  llvm::LLVMContext Context;
  const llvm::StringRef Blocks[] = {"block_a", "block_b"};
  TranslationOptions Options =
      explicitOptions(GuestArchitecture::X86_64, "x86_64-pc-linux-gnu");
  std::unique_ptr<llvm::Module> Module =
      makeCanonicalModule(Context, Options, Blocks);
  ASSERT_NE(Module, nullptr);

  const llvm::StringRef Missing[] = {"block_a"};
  expectCompilerError(
      compileTranslationObjectV1(*Module, Options, objectPolicy(Missing)),
      TranslationObjectCompilerErrorCode::InvalidArtifactPolicy);
  const llvm::StringRef Duplicate[] = {"block_a", "block_a"};
  expectCompilerError(
      compileTranslationObjectV1(*Module, Options, objectPolicy(Duplicate)),
      TranslationObjectCompilerErrorCode::InvalidArtifactPolicy);

  llvm::Expected<TranslationObjectArtifactV1> Artifact =
      compileTranslationObjectV1(*Module, Options, objectPolicy(Blocks));
  if (!Artifact)
    FAIL() << llvm::toString(Artifact.takeError());
  ASSERT_EQ(Artifact->blockSymbols().size(), 2u);
  EXPECT_EQ(Artifact->blockSymbols()[0].IRName, "block_a");
  EXPECT_EQ(Artifact->blockSymbols()[1].IRName, "block_b");
  EXPECT_EQ(Artifact->runtimeSymbols().size(),
            runtimeABIHelperBindingsV1().size());
  RuntimeSymbolRegistryV1 Registry =
      llvm::cantFail(RuntimeSymbolRegistryV1::create());
  EXPECT_EQ(Artifact->runtimeRegistryIdentity(), Registry.identity());
  ASSERT_EQ(Artifact->runtimeSymbols().size(), Registry.entries().size());
  for (size_t Index = 0; Index != Registry.entries().size(); ++Index)
    EXPECT_EQ(Artifact->runtimeSymbols()[Index].IRName,
              Registry.entries()[Index].name());
  expectObjectVerifies(*Artifact);
}

TEST(TranslationObjectCompiler, EnforcesOnlyANonzeroGeneratedByteBudget) {
  llvm::LLVMContext Context;
  constexpr llvm::StringLiteral BlockName("translated_block");
  TranslationOptions Options =
      explicitOptions(GuestArchitecture::X86_64, "x86_64-pc-linux-gnu");
  std::unique_ptr<llvm::Module> Module =
      makeCanonicalModule(Context, Options, {BlockName});
  ASSERT_NE(Module, nullptr);
  const llvm::StringRef Blocks[] = {BlockName};
  const TranslationObjectPolicyV1 Policy = objectPolicy(Blocks);

  Options.GeneratedCodeByteBudget = 0;
  llvm::Expected<TranslationObjectArtifactV1> Unlimited =
      compileTranslationObjectV1(*Module, Options, Policy);
  if (!Unlimited)
    FAIL() << llvm::toString(Unlimited.takeError());
  EXPECT_FALSE(Unlimited->bytes().empty());

  Options.GeneratedCodeByteBudget = Unlimited->bytes().size() - 1;
  expectCompilerError(
      compileTranslationObjectV1(*Module, Options, Policy),
      TranslationObjectCompilerErrorCode::GeneratedCodeBudgetExceeded);
}

TEST(TranslationObjectCompiler, RejectsWrongTripleAndDataLayout) {
  llvm::LLVMContext Context;
  constexpr llvm::StringLiteral BlockName("translated_block");
  TranslationOptions Options =
      explicitOptions(GuestArchitecture::X86_64, "x86_64-pc-linux-gnu");
  std::unique_ptr<llvm::Module> Module =
      makeCanonicalModule(Context, Options, {BlockName});
  ASSERT_NE(Module, nullptr);
  const llvm::StringRef Blocks[] = {BlockName};
  const TranslationObjectPolicyV1 Policy = objectPolicy(Blocks);

  const llvm::Triple CorrectTriple(Module->getTargetTriple());
  const llvm::DataLayout CorrectLayout = Module->getDataLayout();
  Module->setTargetTriple(llvm::Triple("aarch64-unknown-linux-gnu"));
  expectCompilerError(
      compileTranslationObjectV1(*Module, Options, Policy),
      TranslationObjectCompilerErrorCode::InputIRVerificationFailed);

  Module->setTargetTriple(CorrectTriple);
  Module->setDataLayout(llvm::DataLayout(""));
  expectCompilerError(
      compileTranslationObjectV1(*Module, Options, Policy),
      TranslationObjectCompilerErrorCode::InputIRVerificationFailed);
  Module->setDataLayout(CorrectLayout);
}

TEST(TranslationObjectCompiler, RejectsMalformedLLVMIRBeforeCloning) {
  llvm::LLVMContext Context;
  constexpr llvm::StringLiteral BlockName("translated_block");
  TranslationOptions Options =
      explicitOptions(GuestArchitecture::X86_64, "x86_64-pc-linux-gnu");
  std::unique_ptr<llvm::Module> Module =
      makeCanonicalModule(Context, Options, {BlockName});
  ASSERT_NE(Module, nullptr);
  llvm::Function *Function = Module->getFunction(BlockName);
  Function->back().getTerminator()->eraseFromParent();

  expectCompilerError(
      compileTranslationObjectV1(*Module, Options, objectPolicy({BlockName})),
      TranslationObjectCompilerErrorCode::InputIRVerificationFailed);
}

TEST(TranslationObjectCompiler, ProducesDeterministicBytesAndVersionedKeys) {
  llvm::LLVMContext Context;
  const llvm::StringRef Definitions[] = {"block_b", "block_a"};
  const llvm::StringRef ReversedPolicy[] = {"block_a", "block_b"};
  TranslationOptions Options =
      explicitOptions(GuestArchitecture::X86_64, "x86_64-pc-linux-gnu");
  std::unique_ptr<llvm::Module> Module =
      makeCanonicalModule(Context, Options, Definitions);
  ASSERT_NE(Module, nullptr);

  llvm::Expected<TranslationObjectArtifactV1> First =
      compileTranslationObjectV1(*Module, Options, objectPolicy(Definitions));
  if (!First)
    FAIL() << llvm::toString(First.takeError());
  llvm::Expected<TranslationObjectArtifactV1> Second =
      compileTranslationObjectV1(*Module, Options,
                                 objectPolicy(ReversedPolicy));
  if (!Second)
    FAIL() << llvm::toString(Second.takeError());

  EXPECT_EQ(First->bytes(), Second->bytes());
  EXPECT_EQ(First->blockSymbols(), Second->blockSymbols());
  EXPECT_EQ(First->runtimeSymbols(), Second->runtimeSymbols());
  EXPECT_EQ(First->requestCacheKey(), Second->requestCacheKey());
  EXPECT_EQ(First->artifactCacheKey(), Second->artifactCacheKey());
  EXPECT_TRUE(First->requestCacheKey().starts_with(
      "neverd.translation-object-request.v1.sha256:"));
  EXPECT_TRUE(First->artifactCacheKey().starts_with(
      "neverd.translation-object-artifact.v1.sha256:"));
  static_assert(TranslationObjectArtifactV1::CacheIdentityVersion == 1);
  static_assert(TranslationObjectArtifactV1::PipelineSchemaVersion == 3);
}

TEST(TranslationObjectCompiler, EmitsEveryContractHostArchitecture) {
  struct TestCase {
    GuestArchitecture Architecture;
    const char *Triple;
  };
  constexpr TestCase Cases[] = {
      {GuestArchitecture::X86_32, "i686-pc-linux-gnu"},
      {GuestArchitecture::X86_64, "x86_64-pc-linux-gnu"},
      {GuestArchitecture::ARM32, "armv7-none-linux-gnueabihf"},
      {GuestArchitecture::AArch64, "aarch64-unknown-linux-gnu"},
  };

  for (const TestCase &Case : Cases) {
    SCOPED_TRACE(Case.Triple);
    llvm::LLVMContext Context;
    constexpr llvm::StringLiteral BlockName("translated_block");
    TranslationOptions Options =
        explicitOptions(Case.Architecture, Case.Triple);
    std::unique_ptr<llvm::Module> Module =
        makeCanonicalModule(Context, Options, {BlockName});
    ASSERT_NE(Module, nullptr);
    llvm::Expected<TranslationObjectArtifactV1> Artifact =
        compileTranslationObjectV1(*Module, Options, objectPolicy({BlockName}));
    if (!Artifact)
      FAIL() << llvm::toString(Artifact.takeError());
    EXPECT_EQ(Artifact->hostTarget().architecture(), Case.Architecture);
    expectObjectParses(*Artifact);
    expectObjectVerifies(*Artifact);
  }
}

TEST(TranslationObjectCompiler, UsesLLVMsMachOSymbolPrefixAtTheBoundary) {
  llvm::LLVMContext Context;
  constexpr llvm::StringLiteral BlockName("translated_block");
  TranslationOptions Options =
      explicitOptions(GuestArchitecture::X86_64, "x86_64-apple-macosx");
  std::unique_ptr<llvm::Module> Module =
      makeCanonicalModule(Context, Options, {BlockName});
  ASSERT_NE(Module, nullptr);
  llvm::Expected<TranslationObjectArtifactV1> Artifact =
      compileTranslationObjectV1(*Module, Options, objectPolicy({BlockName}));
  if (!Artifact)
    FAIL() << llvm::toString(Artifact.takeError());

  ASSERT_EQ(Artifact->blockSymbols().size(), 1u);
  EXPECT_EQ(Artifact->blockSymbols()[0].IRName, BlockName);
  EXPECT_EQ(Artifact->blockSymbols()[0].ObjectName, "_translated_block");
  for (const TranslationObjectSymbolV1 &Symbol : Artifact->runtimeSymbols())
    EXPECT_EQ(Symbol.ObjectName, "_" + Symbol.IRName);
  expectObjectParses(*Artifact);
  expectObjectVerifies(*Artifact);
}

TEST(TranslationObjectCompiler,
     EmitsAuditedDirectRuntimeCallsAcrossObjectFormats) {
  constexpr llvm::StringLiteral BlockName("translated_block");
  struct TestCase {
    GuestArchitecture Architecture;
    const char *Triple;
  };
  constexpr TestCase Cases[] = {
      {GuestArchitecture::X86_64, "x86_64-pc-linux-gnu"},
      {GuestArchitecture::X86_64, "x86_64-pc-windows-msvc"},
      {GuestArchitecture::X86_64, "x86_64-apple-macosx"},
  };

  for (const TestCase &Case : Cases) {
    SCOPED_TRACE(Case.Triple);
    llvm::LLVMContext Context;
    TranslationOptions Options =
        explicitOptions(Case.Architecture, Case.Triple);
    std::unique_ptr<llvm::Module> Module =
        makeRuntimeCallModule(Context, Options);
    ASSERT_NE(Module, nullptr);
    llvm::Expected<TranslationObjectArtifactV1> Artifact =
        compileTranslationObjectV1(*Module, Options, objectPolicy({BlockName}));
    if (!Artifact)
      FAIL() << llvm::toString(Artifact.takeError());
    expectObjectParses(*Artifact);
    expectObjectVerifies(*Artifact);
  }
}

TEST(TranslationObjectCompiler, ComposesSemanticAndLLVMOptimization) {
  llvm::LLVMContext Context;
  constexpr llvm::StringLiteral BlockName("translated_block");
  TranslationOptions Options =
      explicitOptions(GuestArchitecture::X86_64, "x86_64-pc-linux-gnu");
  Options.Optimization = TranslationOptimizationPolicy::ProvenSemanticAndLLVM;
  Options.LLVMLevel = LLVMOptimizationLevel::O2;
  std::unique_ptr<llvm::Module> Module = makeSemanticModule(Context, Options);
  ASSERT_NE(Module, nullptr);
  const TranslationIRMemorySlot Slots[] = {
      {TranslationIRMemoryRegion::State, 0, 4, TranslationIRMemoryAccess::Read,
       4},
      {TranslationIRMemoryRegion::State, 4, 4, TranslationIRMemoryAccess::Read,
       4},
      {TranslationIRMemoryRegion::State, 8, 4, TranslationIRMemoryAccess::Write,
       4},
  };

  llvm::Expected<TranslationObjectArtifactV1> Artifact =
      compileTranslationObjectV1(*Module, Options,
                                 objectPolicy({BlockName}, Slots, 12));
  if (!Artifact)
    FAIL() << llvm::toString(Artifact.takeError());
  EXPECT_TRUE(Artifact->semanticReport().Changed);
  EXPECT_GT(Artifact->semanticReport().Rewrites, 0u);
  EXPECT_GT(Artifact->semanticReport().FunctionPassInvocations, 0u);
  EXPECT_EQ(Artifact->semanticReport().Proof,
            neverd::solver::ProofStatus::NotRun);
  expectObjectVerifies(*Artifact);
}

TEST(TranslationObjectCompiler, RejectsNonDeterministicProofProviders) {
  llvm::LLVMContext Context;
  constexpr llvm::StringLiteral BlockName("translated_block");
  TranslationOptions Options =
      explicitOptions(GuestArchitecture::X86_64, "x86_64-pc-linux-gnu");
  std::unique_ptr<llvm::Module> Module =
      makeCanonicalModule(Context, Options, {BlockName});
  ASSERT_NE(Module, nullptr);
  const llvm::StringRef Blocks[] = {BlockName};
  TranslationObjectPolicyV1 Policy = objectPolicy(Blocks);
  Policy.Semantic.Simplify.Provider = neverd::ProofProvider::Disabled;
  expectCompilerError(
      compileTranslationObjectV1(*Module, Options, Policy),
      TranslationObjectCompilerErrorCode::InvalidSemanticPolicy);
}

} // namespace
