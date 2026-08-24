//===- COFFExceptionNativeIRTests.cpp - Native WinEH lowering tests ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/llvm/LanguageEHMetadata.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

class MedLLVMEmitterTestPeer {
public:
  static void prepare(MedLLVMEmitter &Emitter, llvm::LLVMContext &Context,
                      llvm::Module &Module, llvm::Function &Function,
                      const MedFunc &Source) {
    Emitter.Ctx = &Context;
    Emitter.Mod = &Module;
    Emitter.Img = nullptr;
    Emitter.TargetArch = Arch::X64;
    Emitter.TargetFormat = BinaryFormat::COFF;
    Emitter.CurFunc = &Function;
    Emitter.CurMedFunc = &Source;
    Emitter.CallSiteAddrs.clear();
    Emitter.EmittedFuncNames.clear();
    Emitter.FuncNames.clear();
  }

  static bool
  emitSEH(MedLLVMEmitter &Emitter, const MedFunc &Source,
          llvm::Function &Function,
          const std::map<int, llvm::BasicBlock *> &OriginalBlockMap) {
    return Emitter.emitNativeSEH(Source, Function, OriginalBlockMap);
  }

  static bool
  emitCxx(MedLLVMEmitter &Emitter, const MedFunc &Source,
          llvm::Function &Function,
          const std::map<int, llvm::BasicBlock *> &OriginalBlockMap) {
    return Emitter.emitNativeCxxEH(Source, Function, OriginalBlockMap);
  }

  static void setCallSiteAddress(MedLLVMEmitter &Emitter, llvm::CallInst &Call,
                                 va_t Address) {
    Emitter.CallSiteAddrs[&Call] = Address;
  }

  static std::map<const llvm::CallInst *, va_t>
  callSiteAddresses(const MedLLVMEmitter &Emitter) {
    return Emitter.CallSiteAddrs;
  }

  static void setFunctionName(MedLLVMEmitter &Emitter, va_t Address,
                              llvm::StringRef Name) {
    Emitter.FuncNames[Address] = Name.str();
  }
};

} // namespace neverd

namespace {

using namespace neverd;

void ensureCOFFCodegenTargets() {
  static std::once_flag Once;
  std::call_once(Once, [] {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();
  });
}

std::string printModuleIR(const llvm::Module &Module) {
  std::string IR;
  llvm::raw_string_ostream OS(IR);
  Module.print(OS, nullptr);
  return IR;
}

void markSourceCall(llvm::CallInst &Call, va_t Address) {
  llvm::LLVMContext &Context = Call.getContext();
  llvm::Metadata *Value = llvm::ConstantAsMetadata::get(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), Address));
  Call.setMetadata(language_eh_md::InternalSourceCallAttachment,
                   llvm::MDNode::get(Context, {Value}));
}

MedFunc makeAddressBackedPersonality(va_t Address, llvm::StringRef Name) {
  MedFunc Personality;
  Personality.Entry = Address;
  Personality.Name = Name.str();
  MedBlock Body;
  Body.Id = 0;
  Body.StartAddr = Address;
  Body.EndAddr = Address + 1;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Address;
  Body.Ops.push_back(std::move(Return));
  Personality.Blocks.push_back(std::move(Body));
  return Personality;
}

llvm::CallInst *addUnmarkedForeignCall(llvm::Module &Module,
                                       llvm::StringRef Name) {
  llvm::LLVMContext &Context = Module.getContext();
  auto *Type = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  llvm::Function *Callee = Module.getFunction("foreign_may_throw");
  if (!Callee)
    Callee = llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                                    "foreign_may_throw", Module);
  llvm::Function *Function = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, Name, Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::CallInst *Call = Builder.CreateCall(Callee);
  Builder.CreateRetVoid();
  return Call;
}

void expectVerifierClean(const llvm::Module &Module) {
  std::string Verification;
  llvm::raw_string_ostream OS(Verification);
  EXPECT_FALSE(llvm::verifyModule(Module, &OS)) << Verification;
}

llvm::FunctionType *sehCallbackType(llvm::LLVMContext &Context,
                                    SEHScopeKind Kind) {
  auto *PtrTy = llvm::PointerType::get(Context, 0);
  if (Kind == SEHScopeKind::Filter)
    return llvm::FunctionType::get(llvm::Type::getInt32Ty(Context),
                                   {PtrTy, PtrTy}, false);
  return llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                 {llvm::Type::getInt8Ty(Context), PtrTy},
                                 false);
}

std::optional<uint64_t> metadataInteger(const llvm::MDNode *Node,
                                        unsigned Index, unsigned Width) {
  if (!Node || Index >= Node->getNumOperands())
    return std::nullopt;
  const auto *Metadata = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
      Node->getOperand(Index).get());
  const auto *Integer =
      Metadata ? llvm::dyn_cast<llvm::ConstantInt>(Metadata->getValue())
               : nullptr;
  if (!Integer || Integer->getBitWidth() != Width)
    return std::nullopt;
  return Integer->getZExtValue();
}

void expectCompleteRewriteContract(const llvm::Function &Function,
                                   uint64_t ProtectedCalls) {
  const llvm::MDNode *Contract =
      Function.getMetadata(exception_rewrite::FunctionAttachment);
  ASSERT_NE(Contract, nullptr);
  ASSERT_EQ(Contract->getNumOperands(), exception_rewrite::OperandCount);
  EXPECT_EQ(metadataInteger(Contract, exception_rewrite::Version, 32),
            exception_rewrite::SchemaVersion);
  EXPECT_EQ(metadataInteger(Contract, exception_rewrite::Source, 8),
            static_cast<uint8_t>(exception_rewrite::SourceState::Complete));
  EXPECT_EQ(metadataInteger(Contract, exception_rewrite::Lowering, 8),
            static_cast<uint8_t>(exception_rewrite::LoweringState::Complete));
  EXPECT_EQ(
      metadataInteger(Contract, exception_rewrite::RequiredProtectedCalls, 64),
      ProtectedCalls);
  EXPECT_EQ(
      metadataInteger(Contract, exception_rewrite::LoweredProtectedCalls, 64),
      ProtectedCalls);
  EXPECT_EQ(
      metadataInteger(Contract, exception_rewrite::SkippedLandingPads, 64), 0u);
}

std::pair<size_t, size_t>
countNativeEHProvenanceAnchors(const llvm::Function &Function) {
  size_t Anchors = 0;
  size_t FuncletAnchors = 0;
  for (const llvm::BasicBlock &Block : Function)
    for (const llvm::Instruction &Instruction : Block)
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
          Call && Call->countOperandBundlesOfType(
                      windows_eh_md::ProvenanceBundle) == 1) {
        ++Anchors;
        FuncletAnchors += Call->countOperandBundlesOfType("funclet") == 1;
      }
  return {Anchors, FuncletAnchors};
}

struct DirectSEHFixture {
  llvm::LLVMContext Context;
  llvm::Module Module;
  MedFunc Source;
  llvm::Function *Function = nullptr;
  llvm::CallInst *Call = nullptr;
  std::map<int, llvm::BasicBlock *> OriginalBlockMap;

  DirectSEHFixture(llvm::StringRef Name, bool TerminateProtectedBlock)
      : Module(Name, Context) {
    Source.Entry = 0x140001000;
    Source.Name = Name.str();
    Source.ReturnType = NdType::makeVoid();

    MedBlock ProtectedSource;
    ProtectedSource.Id = 0;
    ProtectedSource.StartAddr = Source.Entry;
    ProtectedSource.EndAddr = Source.Entry + 0x10;
    MedBlock HandlerSource;
    HandlerSource.Id = 1;
    HandlerSource.StartAddr = Source.Entry + 0x20;
    HandlerSource.EndAddr = Source.Entry + 0x30;
    Source.Blocks.push_back(std::move(ProtectedSource));
    Source.Blocks.push_back(std::move(HandlerSource));

    ExceptionFunction EH;
    EH.CodeRange = {Source.Entry, Source.Entry + 0x40};
    EH.Encoding = ExceptionEncoding::X64UnwindV1;
    EH.ParseStatus = ExceptionParseStatus::Complete;
    EH.Personality = ExceptionPersonality::CSpecificHandler;
    EH.PersonalityVA = Source.Entry + 0x100;
    SEHExceptionInfo SEH;
    SEHScopeRecord Scope;
    Scope.ParseStatus = ExceptionParseStatus::Complete;
    Scope.GuardedRange = {Source.Entry, Source.Entry + 0x10};
    Scope.Kind = SEHScopeKind::CatchAll;
    Scope.HandlerVA = Source.Entry + 0x20;
    Scope.ContinuationVA = Scope.HandlerVA;
    SEH.Scopes.push_back(Scope);
    EH.SEH = std::move(SEH);
    Source.ExceptionMetadata = std::move(EH);

    auto *FunctionType =
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
    Function = llvm::Function::Create(
        FunctionType, llvm::GlobalValue::ExternalLinkage, Name, Module);
    auto *MayThrow = llvm::Function::Create(
        FunctionType, llvm::GlobalValue::ExternalLinkage, "may_throw", Module);
    auto *Protected = llvm::BasicBlock::Create(Context, "protected", Function);
    auto *Handler = llvm::BasicBlock::Create(Context, "handler", Function);
    llvm::IRBuilder<> ProtectedBuilder(Protected);
    Call = ProtectedBuilder.CreateCall(MayThrow);
    markSourceCall(*Call, Source.Entry + 4);
    if (TerminateProtectedBlock)
      ProtectedBuilder.CreateRetVoid();
    llvm::IRBuilder<> HandlerBuilder(Handler);
    HandlerBuilder.CreateRetVoid();
    OriginalBlockMap.emplace(0, Protected);
    OriginalBlockMap.emplace(1, Handler);
  }
};

struct DirectCxxFixture {
  llvm::LLVMContext Context;
  llvm::Module Module;
  MedFunc Source;
  llvm::Function *Function = nullptr;
  llvm::CallInst *Call = nullptr;
  std::map<int, llvm::BasicBlock *> OriginalBlockMap;

  DirectCxxFixture(llvm::StringRef Name, bool TerminateProtectedBlock)
      : Module(Name, Context) {
    Source.Entry = 0x140001000;
    Source.Name = Name.str();
    Source.ReturnType = NdType::makeVoid();

    MedBlock ProtectedSource;
    ProtectedSource.Id = 0;
    ProtectedSource.StartAddr = Source.Entry;
    ProtectedSource.EndAddr = Source.Entry + 0x10;
    MedBlock HandlerSource;
    HandlerSource.Id = 1;
    HandlerSource.StartAddr = Source.Entry + 0x20;
    HandlerSource.EndAddr = Source.Entry + 0x30;
    Source.Blocks.push_back(std::move(ProtectedSource));
    Source.Blocks.push_back(std::move(HandlerSource));

    ExceptionFunction EH;
    EH.CodeRange = {Source.Entry, Source.Entry + 0x40};
    EH.Encoding = ExceptionEncoding::X64UnwindV1;
    EH.ParseStatus = ExceptionParseStatus::Complete;
    EH.Personality = ExceptionPersonality::CxxFrameHandler3;
    EH.PersonalityVA = Source.Entry + 0x100;
    CxxExceptionInfo Cxx;
    Cxx.Magic = 0x19930522;
    Cxx.Version = CxxFuncInfoVersion::WithEHFlags;
    Cxx.Flags = 1;
    Cxx.IsSynchronous = true;
    Cxx.MaxState = 2;
    CxxUnwindAction State0;
    State0.ToState = -1;
    State0.Kind = CxxUnwindAction::ActionKind::None;
    CxxUnwindAction State1;
    State1.ToState = 0;
    State1.Kind = CxxUnwindAction::ActionKind::None;
    Cxx.UnwindMap = {State0, State1};
    Cxx.IPMap = {{Source.Entry, 0}, {Source.Entry + 0x10, -1}};
    CxxTryBlock Try;
    Try.TryLow = 0;
    Try.TryHigh = 0;
    Try.CatchHigh = 1;
    CxxCatchHandler Catch;
    Catch.Adjectives = 0x40;
    Catch.TypeDescriptorVA = Source.Entry + 0x200;
    Catch.HandlerVA = Source.Entry + 0x20;
    Try.Handlers.push_back(Catch);
    Cxx.TryBlocks.push_back(std::move(Try));
    EH.Cxx = std::move(Cxx);
    Source.ExceptionMetadata = std::move(EH);

    auto *FunctionType =
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
    Function = llvm::Function::Create(
        FunctionType, llvm::GlobalValue::ExternalLinkage, Name, Module);
    auto *MayThrow = llvm::Function::Create(
        FunctionType, llvm::GlobalValue::ExternalLinkage, "may_throw", Module);
    auto *Protected = llvm::BasicBlock::Create(Context, "protected", Function);
    auto *Handler = llvm::BasicBlock::Create(Context, "handler", Function);
    llvm::IRBuilder<> ProtectedBuilder(Protected);
    Call = ProtectedBuilder.CreateCall(MayThrow);
    markSourceCall(*Call, Source.Entry + 4);
    if (TerminateProtectedBlock)
      ProtectedBuilder.CreateRetVoid();
    llvm::IRBuilder<> HandlerBuilder(Handler);
    HandlerBuilder.CreateRetVoid();
    OriginalBlockMap.emplace(0, Protected);
    OriginalBlockMap.emplace(1, Handler);
  }
};

TEST(COFFExceptionIR, EmitsVerifierCleanNativeCatchAllSEH) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "native_seh_test";
  Func.ReturnType = NdType::makeVoid();
  constexpr va_t MayThrowVA = 0x140001100;

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = Func.Entry;
  Protected.EndAddr = Func.Entry + 0x10;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Func.Entry + 4;
  Call.addInput(MedVar::makeConst(MayThrowVA, 8));
  Protected.Ops.push_back(Call);
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = Func.Entry + 8;
  Protected.Ops.push_back(ProtectedReturn);

  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = Func.Entry + 0x20;
  Handler.EndAddr = Func.Entry + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = Func.Entry + 0x28;
  Handler.Ops.push_back(HandlerReturn);

  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x40};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = Func.Entry + 0x100;
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Func.Entry, Func.Entry + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Func.Entry + 0x20;
  Scope.ContinuationVA = Scope.HandlerVA;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);
  const va_t PersonalityVA = EH.PersonalityVA;
  Func.ExceptionMetadata = std::move(EH);
  MedFunc Personality =
      makeAddressBackedPersonality(PersonalityVA, "\01__C_specific_handler");

  llvm::LLVMContext Ctx;
  MedLLVMEmitter Emitter;
  auto Mod =
      Emitter.emit({Func, Personality}, Ctx, "native_seh", Arch::X64,
                   {{MayThrowVA, "may_throw"}}, nullptr, BinaryFormat::COFF);
  ASSERT_NE(Mod, nullptr);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  EXPECT_FALSE(llvm::verifyModule(*Mod, &VerificationOS)) << Verification;

  llvm::Function *F = Mod->getFunction(Func.Name);
  ASSERT_NE(F, nullptr);
  llvm::Function *Canonical = Mod->getFunction("__C_specific_handler");
  ASSERT_NE(Canonical, nullptr);
  EXPECT_TRUE(Canonical->isDeclaration());
  EXPECT_TRUE(Canonical->getReturnType()->isIntegerTy(32));
  EXPECT_TRUE(Canonical->isVarArg());
  const std::string PersonalityBodyName =
      (kAutoFuncPrefix + llvm::utohexstr(PersonalityVA)).str();
  llvm::Function *PersonalityBody = Mod->getFunction(PersonalityBodyName);
  ASSERT_NE(PersonalityBody, nullptr);
  EXPECT_FALSE(PersonalityBody->isDeclaration());
  EXPECT_TRUE(PersonalityBody->getReturnType()->isIntegerTy(64));
  EXPECT_TRUE(F->hasPersonalityFn());
  EXPECT_NE(F->getMetadata(windows_eh_md::NativeAttachment), nullptr);
  expectCompleteRewriteContract(*F, 1);
  EXPECT_NE(Mod->getModuleFlag("eh-asynch"), nullptr);

  std::string IR;
  llvm::raw_string_ostream OS(IR);
  Mod->print(OS, nullptr);
  EXPECT_NE(IR.find("invoke void @llvm.seh.try.begin"), std::string::npos);
  EXPECT_NE(IR.find("invoke void @llvm.seh.try.end"), std::string::npos);
  EXPECT_NE(IR.find("invoke i64 @may_throw"), std::string::npos);
  EXPECT_NE(IR.find("catchswitch within none"), std::string::npos);
  EXPECT_NE(IR.find("catchpad within"), std::string::npos);
  EXPECT_NE(IR.find("catchret from"), std::string::npos);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  ASSERT_TRUE(Func.ExceptionMetadata.has_value());
  Image.ExceptionMetadata.Functions.push_back(*Func.ExceptionMetadata);
  Image.ExceptionMetadata.rebuildIndex();

  llvm::CatchReturnInst *CatchReturn = nullptr;
  llvm::InvokeInst *ProtectedInvoke = nullptr;
  for (llvm::BasicBlock &Block : *F) {
    for (llvm::Instruction &Instruction : Block) {
      if (!CatchReturn)
        CatchReturn = llvm::dyn_cast<llvm::CatchReturnInst>(&Instruction);
      auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(&Instruction);
      const llvm::Function *Callee =
          Invoke ? Invoke->getCalledFunction() : nullptr;
      if (Callee && Callee->getName() == "may_throw")
        ProtectedInvoke = Invoke;
    }
  }
  ASSERT_NE(CatchReturn, nullptr);
  ASSERT_NE(ProtectedInvoke, nullptr);
  llvm::BasicBlock *HandlerTarget = CatchReturn->getSuccessor();
  CatchReturn->setSuccessor(ProtectedInvoke->getNormalDest());
  expectVerifierClean(*Mod);
  auto RetargetedPlan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(RetargetedPlan));
  EXPECT_NE(llvm::toString(RetargetedPlan.takeError())
                .find("SEH catchret continuation"),
            std::string::npos);
  CatchReturn->setSuccessor(HandlerTarget);
  expectVerifierClean(*Mod);

  auto Plan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
  EXPECT_EQ(countNativeEHProvenanceAnchors(*F),
            (std::pair<size_t, size_t>{7, 1}));

  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Deep;
  Options.LLVMLevel = llvm::OptimizationLevel::O3;
  OptimizationResult Optimization = Pipeline::optimizeModule(*Mod, Options);
  EXPECT_NE(Optimization.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_NE(Optimization.Stop, OptimizationStopReason::VerificationFailed);
  expectVerifierClean(*Mod);

  F = Mod->getFunction(Func.Name);
  ASSERT_NE(F, nullptr);
  EXPECT_EQ(countNativeEHProvenanceAnchors(*F),
            (std::pair<size_t, size_t>{7, 1}));
  auto OptimizedPlan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(OptimizedPlan))
      << llvm::toString(OptimizedPlan.takeError());

  ensureCOFFCodegenTargets();
  CompiledImage Compiled = compileImageForPatch(
      *Mod, Arch::X64, BinaryFormat::COFF, 0x140004000,
      [&](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "may_throw")
          return MayThrowVA;
        if (Symbol == "__C_specific_handler")
          return PersonalityVA;
        return std::nullopt;
      },
      Image.Base);
  ASSERT_TRUE(Compiled.Success);
  EXPECT_TRUE(Compiled.Unresolved.empty());
}

TEST(COFFExceptionIR, PreservesAndAuthenticatesHardwareOnlySEHRanges) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "native_hardware_only_seh";
  Func.ReturnType = NdType::makeVoid();
  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = Func.Entry;
  Protected.EndAddr = Func.Entry + 0x10;
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = Func.Entry + 8;
  Protected.Ops.push_back(ProtectedReturn);
  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = Func.Entry + 0x20;
  Handler.EndAddr = Func.Entry + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = Func.Entry + 0x28;
  Handler.Ops.push_back(HandlerReturn);
  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x40};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = Func.Entry + 0x100;
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.ParseStatus = ExceptionParseStatus::Complete;
  Scope.GuardedRange = {Func.Entry, Func.Entry + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Func.Entry + 0x20;
  Scope.ContinuationVA = Scope.HandlerVA;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);
  const va_t PersonalityVA = EH.PersonalityVA;
  Func.ExceptionMetadata = std::move(EH);
  MedFunc Personality =
      makeAddressBackedPersonality(PersonalityVA, "\01__C_specific_handler");

  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  auto Mod = Emitter.emit({Func, Personality}, Context, "hardware_only_seh",
                          Arch::X64, {}, nullptr, BinaryFormat::COFF);
  ASSERT_NE(Mod, nullptr);
  expectVerifierClean(*Mod);
  llvm::Function *Function = Mod->getFunction(Func.Name);
  ASSERT_NE(Function, nullptr);
  expectCompleteRewriteContract(*Function, 0);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  ASSERT_TRUE(Func.ExceptionMetadata.has_value());
  Image.ExceptionMetadata.Functions.push_back(*Func.ExceptionMetadata);
  Image.ExceptionMetadata.rebuildIndex();
  ASSERT_NE(Function->getMetadata(windows_eh_md::FunctionAttachment), nullptr);
  auto Plan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
  ASSERT_EQ(Plan->LanguageExceptionFunctionEntries.size(), 1u);

  auto FindRangeEnter = [](llvm::Function &Function) -> llvm::InvokeInst * {
    for (llvm::BasicBlock &Block : Function)
      for (llvm::Instruction &Instruction : Block) {
        auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(&Instruction);
        const llvm::Function *Callee =
            Invoke ? Invoke->getCalledFunction() : nullptr;
        if (Callee &&
            Callee->getIntrinsicID() == llvm::Intrinsic::seh_try_begin)
          return Invoke;
      }
    return nullptr;
  };
  auto ExpectRejected = [&](llvm::Module &Module, llvm::StringRef Message) {
    expectVerifierClean(Module);
    auto Rejected = planCOFFExceptionPatch(Module, Image, Arch::X64);
    ASSERT_FALSE(static_cast<bool>(Rejected));
    EXPECT_NE(llvm::toString(Rejected.takeError()).find(Message),
              std::string::npos);
  };

  std::unique_ptr<llvm::Module> Retargeted = llvm::CloneModule(*Mod);
  llvm::Function *RetargetedFunction =
      Retargeted->getFunction(Function->getName());
  ASSERT_NE(RetargetedFunction, nullptr);
  ASSERT_NE(RetargetedFunction->getMetadata(windows_eh_md::FunctionAttachment),
            nullptr);
  llvm::InvokeInst *RetargetedEnter = FindRangeEnter(*RetargetedFunction);
  ASSERT_NE(RetargetedEnter, nullptr);
  llvm::CatchReturnInst *RetargetedCatchReturn = nullptr;
  for (llvm::BasicBlock &Block : *RetargetedFunction)
    if (auto *Return =
            llvm::dyn_cast<llvm::CatchReturnInst>(Block.getTerminator()))
      RetargetedCatchReturn = Return;
  ASSERT_NE(RetargetedCatchReturn, nullptr);
  RetargetedEnter->setNormalDest(RetargetedCatchReturn->getSuccessor());
  ExpectRejected(*Retargeted, "range marker has an altered");

  std::unique_ptr<llvm::Module> Deleted = llvm::CloneModule(*Mod);
  llvm::Function *DeletedFunction = Deleted->getFunction(Function->getName());
  ASSERT_NE(DeletedFunction, nullptr);
  llvm::InvokeInst *DeletedEnter = FindRangeEnter(*DeletedFunction);
  ASSERT_NE(DeletedEnter, nullptr);
  llvm::BasicBlock *DeletedBlock = DeletedEnter->getParent();
  llvm::BasicBlock *DeletedNormalDest = DeletedEnter->getNormalDest();
  DeletedEnter->eraseFromParent();
  llvm::UncondBrInst::Create(DeletedNormalDest, DeletedBlock);
  ExpectRejected(*Deleted, "range-marker provenance");

  std::unique_ptr<llvm::Module> Duplicated = llvm::CloneModule(*Mod);
  llvm::Function *DuplicatedFunction =
      Duplicated->getFunction(Function->getName());
  ASSERT_NE(DuplicatedFunction, nullptr);
  llvm::InvokeInst *DuplicatedEnter = FindRangeEnter(*DuplicatedFunction);
  ASSERT_NE(DuplicatedEnter, nullptr);
  llvm::Instruction *RangeAnchor = DuplicatedEnter->getPrevNode();
  ASSERT_NE(RangeAnchor, nullptr);
  llvm::Instruction *DuplicateAnchor = RangeAnchor->clone();
  DuplicateAnchor->insertBefore(DuplicatedEnter->getIterator());
  ExpectRejected(*Duplicated, "range-marker provenance");

  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Deep;
  Options.LLVMLevel = llvm::OptimizationLevel::O3;
  OptimizationResult Optimization = Pipeline::optimizeModule(*Mod, Options);
  EXPECT_NE(Optimization.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_NE(Optimization.Stop, OptimizationStopReason::VerificationFailed);
  expectVerifierClean(*Mod);
  auto OptimizedPlan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(OptimizedPlan))
      << llvm::toString(OptimizedPlan.takeError());

  ensureCOFFCodegenTargets();
  CompiledImage Compiled = compileImageForPatch(
      *Mod, Arch::X64, BinaryFormat::COFF, 0x140004000,
      [&](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "__C_specific_handler")
          return PersonalityVA;
        return std::nullopt;
      },
      Image.Base);
  ASSERT_TRUE(Compiled.Success);
  EXPECT_TRUE(Compiled.Unresolved.empty());
}

TEST(COFFExceptionIR, RejectsMissingSEHCallSiteWithoutMutation) {
  DirectSEHFixture Fixture("atomic_seh_missing_call_site",
                           /*TerminateProtectedBlock=*/true);
  MedLLVMEmitter Emitter;
  MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                  *Fixture.Function, Fixture.Source);

  std::string BeforeVerification;
  llvm::raw_string_ostream BeforeVerificationOS(BeforeVerification);
  ASSERT_FALSE(llvm::verifyModule(Fixture.Module, &BeforeVerificationOS))
      << BeforeVerification;
  const std::string BeforeIR = printModuleIR(Fixture.Module);
  const auto BeforeCallSites =
      MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

  bool Lowered = MedLLVMEmitterTestPeer::emitSEH(
      Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap);

  EXPECT_FALSE(Lowered);
  EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
  EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
            BeforeCallSites);
  std::string AfterVerification;
  llvm::raw_string_ostream AfterVerificationOS(AfterVerification);
  EXPECT_FALSE(llvm::verifyModule(Fixture.Module, &AfterVerificationOS))
      << AfterVerification;
}

TEST(COFFExceptionIR, RejectsUnterminatedSEHIntermediateWithoutMutation) {
  // Native lowering runs on an emitter intermediate. Exercise its defensive
  // Next==nullptr rejection directly; an unterminated block is deliberately
  // not presented as verifier-clean product IR.
  DirectSEHFixture Fixture("atomic_seh_unterminated",
                           /*TerminateProtectedBlock=*/false);
  MedLLVMEmitter Emitter;
  MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                  *Fixture.Function, Fixture.Source);
  MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                             Fixture.Source.Entry + 4);
  const std::string BeforeIR = printModuleIR(Fixture.Module);
  const auto BeforeCallSites =
      MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

  bool Lowered = MedLLVMEmitterTestPeer::emitSEH(
      Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap);

  EXPECT_FALSE(Lowered);
  EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
  EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
            BeforeCallSites);
}

TEST(COFFExceptionIR, RejectsMissingCxxCallSiteWithoutMutation) {
  DirectCxxFixture Fixture("atomic_cxx_missing_call_site",
                           /*TerminateProtectedBlock=*/true);
  ASSERT_TRUE(Fixture.Source.ExceptionMetadata &&
              Fixture.Source.ExceptionMetadata->Cxx &&
              Fixture.Source.ExceptionMetadata->Cxx->hasValidStateGraph());
  MedLLVMEmitter Emitter;
  MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                  *Fixture.Function, Fixture.Source);

  std::string BeforeVerification;
  llvm::raw_string_ostream BeforeVerificationOS(BeforeVerification);
  ASSERT_FALSE(llvm::verifyModule(Fixture.Module, &BeforeVerificationOS))
      << BeforeVerification;
  const std::string BeforeIR = printModuleIR(Fixture.Module);
  const auto BeforeCallSites =
      MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

  bool Lowered = MedLLVMEmitterTestPeer::emitCxx(
      Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap);

  EXPECT_FALSE(Lowered);
  EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
  EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
            BeforeCallSites);
  std::string AfterVerification;
  llvm::raw_string_ostream AfterVerificationOS(AfterVerification);
  EXPECT_FALSE(llvm::verifyModule(Fixture.Module, &AfterVerificationOS))
      << AfterVerification;
}

TEST(COFFExceptionIR, RejectsUnterminatedCxxIntermediateWithoutMutation) {
  // As with the SEH counterpart, this exercises a defensive emitter
  // intermediate and does not present an unterminated block as product IR.
  DirectCxxFixture Fixture("atomic_cxx_unterminated",
                           /*TerminateProtectedBlock=*/false);
  ASSERT_TRUE(Fixture.Source.ExceptionMetadata &&
              Fixture.Source.ExceptionMetadata->Cxx &&
              Fixture.Source.ExceptionMetadata->Cxx->hasValidStateGraph());
  MedLLVMEmitter Emitter;
  MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                  *Fixture.Function, Fixture.Source);
  MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                             Fixture.Source.Entry + 4);
  const std::string BeforeIR = printModuleIR(Fixture.Module);
  const auto BeforeCallSites =
      MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

  bool Lowered = MedLLVMEmitterTestPeer::emitCxx(
      Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap);

  EXPECT_FALSE(Lowered);
  EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
  EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
            BeforeCallSites);
}

TEST(COFFExceptionIR, RejectsCxxLanguageOverlaysWithoutMutation) {
  enum class OverlayKind { Rust, ObjectiveC };
  for (OverlayKind Kind : {OverlayKind::Rust, OverlayKind::ObjectiveC}) {
    SCOPED_TRACE(Kind == OverlayKind::Rust ? "rust" : "objective-c");
    DirectCxxFixture Fixture("atomic_cxx_language_overlay",
                             /*TerminateProtectedBlock=*/true);
    if (Kind == OverlayKind::Rust)
      Fixture.Source.ExceptionMetadata->Rust.emplace();
    else
      Fixture.Source.ExceptionMetadata->ObjC.emplace();

    MedLLVMEmitter Emitter;
    MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                    *Fixture.Function, Fixture.Source);
    MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                               Fixture.Source.Entry + 4);
    expectVerifierClean(Fixture.Module);
    const std::string BeforeIR = printModuleIR(Fixture.Module);
    const auto BeforeCallSites =
        MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

    bool Lowered = MedLLVMEmitterTestPeer::emitCxx(
        Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap);

    EXPECT_FALSE(Lowered);
    EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
    EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
              BeforeCallSites);
    expectVerifierClean(Fixture.Module);
  }
}

TEST(COFFExceptionIR, RejectsInconsistentDecodeProvenanceWithoutMutation) {
  DirectSEHFixture Fixture("atomic_seh_stale_decode_summary",
                           /*TerminateProtectedBlock=*/true);
  ExceptionFunctionDecodeProvenance Provenance;
  Provenance.Language.Diagnostics = {"language decode changed"};
  Fixture.Source.ExceptionMetadata->DecodeProvenance = std::move(Provenance);

  MedLLVMEmitter Emitter;
  MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                  *Fixture.Function, Fixture.Source);
  MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                             Fixture.Source.Entry + 4);
  expectVerifierClean(Fixture.Module);
  const std::string BeforeIR = printModuleIR(Fixture.Module);
  const auto BeforeCallSites =
      MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

  bool Lowered = MedLLVMEmitterTestPeer::emitSEH(
      Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap);

  EXPECT_FALSE(Lowered);
  EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
  EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
            BeforeCallSites);
  expectVerifierClean(Fixture.Module);
}

TEST(COFFExceptionIR, RejectsWrongSEHSourceCallAddressWithoutMutation) {
  DirectSEHFixture Fixture("atomic_seh_wrong_call_address",
                           /*TerminateProtectedBlock=*/true);
  MedLLVMEmitter Emitter;
  MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                  *Fixture.Function, Fixture.Source);
  MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                             Fixture.Source.Entry + 8);
  expectVerifierClean(Fixture.Module);
  const std::string BeforeIR = printModuleIR(Fixture.Module);
  const auto BeforeCallSites =
      MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

  bool Lowered = MedLLVMEmitterTestPeer::emitSEH(
      Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap);

  EXPECT_FALSE(Lowered);
  EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
  EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
            BeforeCallSites);
  expectVerifierClean(Fixture.Module);
}

TEST(COFFExceptionIR, RejectsStaleSEHCallSiteWithoutMutation) {
  DirectSEHFixture Fixture("atomic_seh_stale_call_site",
                           /*TerminateProtectedBlock=*/true);
  llvm::CallInst *Stale =
      addUnmarkedForeignCall(Fixture.Module, "seh_foreign_call_owner");
  MedLLVMEmitter Emitter;
  MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                  *Fixture.Function, Fixture.Source);
  MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                             Fixture.Source.Entry + 4);
  MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Stale,
                                             Fixture.Source.Entry + 8);
  expectVerifierClean(Fixture.Module);
  const std::string BeforeIR = printModuleIR(Fixture.Module);
  const auto BeforeCallSites =
      MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

  bool Lowered = MedLLVMEmitterTestPeer::emitSEH(
      Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap);

  EXPECT_FALSE(Lowered);
  EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
  EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
            BeforeCallSites);
  expectVerifierClean(Fixture.Module);
}

TEST(COFFExceptionIR, RejectsWrongCxxSourceCallAddressWithoutMutation) {
  DirectCxxFixture Fixture("atomic_cxx_wrong_call_address",
                           /*TerminateProtectedBlock=*/true);
  MedLLVMEmitter Emitter;
  MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                  *Fixture.Function, Fixture.Source);
  MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                             Fixture.Source.Entry + 8);
  expectVerifierClean(Fixture.Module);
  const std::string BeforeIR = printModuleIR(Fixture.Module);
  const auto BeforeCallSites =
      MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

  bool Lowered = MedLLVMEmitterTestPeer::emitCxx(
      Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap);

  EXPECT_FALSE(Lowered);
  EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
  EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
            BeforeCallSites);
  expectVerifierClean(Fixture.Module);
}

TEST(COFFExceptionIR, RejectsStaleCxxCallSiteWithoutMutation) {
  DirectCxxFixture Fixture("atomic_cxx_stale_call_site",
                           /*TerminateProtectedBlock=*/true);
  llvm::CallInst *Stale =
      addUnmarkedForeignCall(Fixture.Module, "cxx_foreign_call_owner");
  MedLLVMEmitter Emitter;
  MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                  *Fixture.Function, Fixture.Source);
  MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                             Fixture.Source.Entry + 4);
  MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Stale,
                                             Fixture.Source.Entry + 8);
  expectVerifierClean(Fixture.Module);
  const std::string BeforeIR = printModuleIR(Fixture.Module);
  const auto BeforeCallSites =
      MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

  bool Lowered = MedLLVMEmitterTestPeer::emitCxx(
      Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap);

  EXPECT_FALSE(Lowered);
  EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
  EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
            BeforeCallSites);
  expectVerifierClean(Fixture.Module);
}

TEST(COFFExceptionIR, RejectsConflictingSEHPersonalityWithoutMutation) {
  DirectSEHFixture Fixture("atomic_seh_personality_conflict",
                           /*TerminateProtectedBlock=*/true);
  auto *WrongType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Fixture.Context), false);
  llvm::Function::Create(WrongType, llvm::GlobalValue::ExternalLinkage,
                         "__C_specific_handler", Fixture.Module);
  MedLLVMEmitter Emitter;
  MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                  *Fixture.Function, Fixture.Source);
  MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                             Fixture.Source.Entry + 4);
  expectVerifierClean(Fixture.Module);
  const std::string BeforeIR = printModuleIR(Fixture.Module);
  const auto BeforeCallSites =
      MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

  bool Lowered = MedLLVMEmitterTestPeer::emitSEH(
      Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap);

  EXPECT_FALSE(Lowered);
  EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
  EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
            BeforeCallSites);
  expectVerifierClean(Fixture.Module);
}

TEST(COFFExceptionIR, RejectsConflictingCxxPersonalityWithoutMutation) {
  DirectCxxFixture Fixture("atomic_cxx_personality_conflict",
                           /*TerminateProtectedBlock=*/true);
  auto *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Fixture.Context), {}, true);
  llvm::Function *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage, "__CxxFrameHandler3",
      Fixture.Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Fixture.Context, "entry", Personality));
  Builder.CreateRet(
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(Fixture.Context), 0));
  MedLLVMEmitter Emitter;
  MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                  *Fixture.Function, Fixture.Source);
  MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                             Fixture.Source.Entry + 4);
  expectVerifierClean(Fixture.Module);
  const std::string BeforeIR = printModuleIR(Fixture.Module);
  const auto BeforeCallSites =
      MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

  bool Lowered = MedLLVMEmitterTestPeer::emitCxx(
      Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap);

  EXPECT_FALSE(Lowered);
  EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
  EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
            BeforeCallSites);
  expectVerifierClean(Fixture.Module);
}

TEST(COFFExceptionIR, RejectsConflictingCxxTypeDescriptorWithoutMutation) {
  enum class ConflictKind { WrongType, Definition, Function };
  struct ConflictCase {
    ConflictKind Kind;
    const char *Name;
  };
  for (const ConflictCase &Case :
       {ConflictCase{ConflictKind::WrongType, "wrong element type"},
        ConflictCase{ConflictKind::Definition, "defined data"},
        ConflictCase{ConflictKind::Function, "function with RTTI name"}}) {
    SCOPED_TRACE(Case.Name);
    DirectCxxFixture Fixture("atomic_cxx_type_descriptor_conflict",
                             /*TerminateProtectedBlock=*/true);
    const std::string Symbol = makeNdDataSymbol(Fixture.Source.Entry + 0x200);
    switch (Case.Kind) {
    case ConflictKind::WrongType:
      new llvm::GlobalVariable(
          Fixture.Module, llvm::Type::getInt32Ty(Fixture.Context),
          /*isConstant=*/true, llvm::GlobalValue::ExternalLinkage, nullptr,
          Symbol);
      break;
    case ConflictKind::Definition:
      new llvm::GlobalVariable(
          Fixture.Module, llvm::Type::getInt8Ty(Fixture.Context),
          /*isConstant=*/true, llvm::GlobalValue::ExternalLinkage,
          llvm::ConstantInt::get(llvm::Type::getInt8Ty(Fixture.Context), 0),
          Symbol);
      break;
    case ConflictKind::Function: {
      auto *Type = llvm::FunctionType::get(
          llvm::Type::getVoidTy(Fixture.Context), false);
      llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage, Symbol,
                             Fixture.Module);
      break;
    }
    }
    MedLLVMEmitter Emitter;
    MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                    *Fixture.Function, Fixture.Source);
    MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                               Fixture.Source.Entry + 4);
    expectVerifierClean(Fixture.Module);
    const std::string BeforeIR = printModuleIR(Fixture.Module);
    const auto BeforeCallSites =
        MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

    bool Lowered = MedLLVMEmitterTestPeer::emitCxx(
        Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap);

    EXPECT_FALSE(Lowered);
    EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
    EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
              BeforeCallSites);
    expectVerifierClean(Fixture.Module);
  }
}

TEST(COFFExceptionIR, RejectsConflictingSEHAsyncFlagWithoutMutation) {
  enum class FlagConflict { Disabled, WrongType, WrongBehavior };
  struct ConflictCase {
    FlagConflict Kind;
    const char *Name;
  };
  for (const ConflictCase &Case :
       {ConflictCase{FlagConflict::Disabled, "disabled value"},
        ConflictCase{FlagConflict::WrongType, "non-integer value"},
        ConflictCase{FlagConflict::WrongBehavior, "conflicting behavior"}}) {
    SCOPED_TRACE(Case.Name);
    DirectSEHFixture Fixture("atomic_seh_async_flag_conflict",
                             /*TerminateProtectedBlock=*/true);
    switch (Case.Kind) {
    case FlagConflict::Disabled:
      Fixture.Module.addModuleFlag(llvm::Module::Warning, "eh-asynch",
                                   uint32_t{0});
      break;
    case FlagConflict::WrongType:
      Fixture.Module.addModuleFlag(
          llvm::Module::Warning, "eh-asynch",
          llvm::MDString::get(Fixture.Context, "enabled"));
      break;
    case FlagConflict::WrongBehavior:
      Fixture.Module.addModuleFlag(llvm::Module::Error, "eh-asynch", 1);
      break;
    }
    MedLLVMEmitter Emitter;
    MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                    *Fixture.Function, Fixture.Source);
    MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                               Fixture.Source.Entry + 4);
    expectVerifierClean(Fixture.Module);
    const std::string BeforeIR = printModuleIR(Fixture.Module);
    const auto BeforeCallSites =
        MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

    bool Lowered = MedLLVMEmitterTestPeer::emitSEH(
        Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap);

    EXPECT_FALSE(Lowered);
    EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
    EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
              BeforeCallSites);
    expectVerifierClean(Fixture.Module);
  }
}

TEST(COFFExceptionIR, RejectsConflictingSEHCallbackABIWithoutMutation) {
  enum class CallbackConflict { Type, Linkage, CallingConvention };
  struct ConflictCase {
    CallbackConflict Kind;
    const char *Name;
  };
  constexpr va_t CallbackVA = 0x140001100;
  for (SEHScopeKind ScopeKind : {SEHScopeKind::Filter, SEHScopeKind::Finally}) {
    for (const ConflictCase &Case :
         {ConflictCase{CallbackConflict::Type, "function type"},
          ConflictCase{CallbackConflict::Linkage, "linkage"},
          ConflictCase{CallbackConflict::CallingConvention,
                       "calling convention"}}) {
      SCOPED_TRACE(ScopeKind == SEHScopeKind::Filter ? "filter" : "finally");
      SCOPED_TRACE(Case.Name);
      DirectSEHFixture Fixture("atomic_seh_callback_conflict",
                               /*TerminateProtectedBlock=*/true);
      SEHScopeRecord &Scope =
          Fixture.Source.ExceptionMetadata->SEH->Scopes.front();
      Scope.Kind = ScopeKind;
      Scope.FilterOrFinallyVA = CallbackVA;
      if (ScopeKind == SEHScopeKind::Finally) {
        Scope.HandlerVA = CallbackVA;
        Scope.ContinuationVA = 0;
      }

      llvm::FunctionType *ExpectedType =
          sehCallbackType(Fixture.Context, ScopeKind);
      llvm::FunctionType *ActualType = ExpectedType;
      llvm::GlobalValue::LinkageTypes Linkage =
          llvm::GlobalValue::ExternalLinkage;
      if (Case.Kind == CallbackConflict::Type)
        ActualType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(Fixture.Context), false);
      if (Case.Kind == CallbackConflict::Linkage)
        Linkage = llvm::GlobalValue::ExternalWeakLinkage;
      llvm::Function *Callback = llvm::Function::Create(
          ActualType, Linkage, "seh_callback", Fixture.Module);
      if (Case.Kind == CallbackConflict::CallingConvention)
        Callback->setCallingConv(llvm::CallingConv::Fast);

      MedLLVMEmitter Emitter;
      MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                      *Fixture.Function, Fixture.Source);
      MedLLVMEmitterTestPeer::setFunctionName(Emitter, CallbackVA,
                                              Callback->getName());
      MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                                 Fixture.Source.Entry + 4);
      expectVerifierClean(Fixture.Module);
      const std::string BeforeIR = printModuleIR(Fixture.Module);
      const auto BeforeCallSites =
          MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

      bool Lowered = MedLLVMEmitterTestPeer::emitSEH(
          Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap);

      EXPECT_FALSE(Lowered);
      EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
      EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
                BeforeCallSites);
      expectVerifierClean(Fixture.Module);
    }
  }
}

TEST(COFFExceptionIR, ReportsUnresolvedNativePersonalityExactlyOnce) {
  ensureCOFFCodegenTargets();

  llvm::LLVMContext Context;
  llvm::Module Module("unresolved-native-personality", Context);
  auto *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  auto *Personality = llvm::Function::Create(PersonalityType,
                                             llvm::GlobalValue::ExternalLinkage,
                                             "__CxxFrameHandler3", Module);
  auto *FunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *MayThrow = llvm::Function::Create(
      FunctionType, llvm::GlobalValue::ExternalLinkage, "may_throw", Module);
  auto *PtrTy = llvm::PointerType::get(Context, 0);
  auto AddFrame = [&](llvm::StringRef Name) {
    auto *Function = llvm::Function::Create(
        FunctionType, llvm::GlobalValue::ExternalLinkage, Name, Module);
    Function->setPersonalityFn(Personality);
    Function->setUWTableKind(llvm::UWTableKind::Default);

    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
    auto *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
    auto *Dispatch =
        llvm::BasicBlock::Create(Context, "catch.dispatch", Function);
    auto *Pad = llvm::BasicBlock::Create(Context, "catch.pad", Function);

    llvm::IRBuilder<> EntryBuilder(Entry);
    EntryBuilder.CreateInvoke(MayThrow, Exit, Dispatch);
    llvm::IRBuilder<> ExitBuilder(Exit);
    ExitBuilder.CreateRetVoid();
    llvm::IRBuilder<> DispatchBuilder(Dispatch);
    auto *CatchSwitch = DispatchBuilder.CreateCatchSwitch(
        llvm::ConstantTokenNone::get(Context), nullptr, 1);
    CatchSwitch->addHandler(Pad);
    llvm::IRBuilder<> PadBuilder(Pad);
    llvm::SmallVector<llvm::Value *, 3> Args{
        llvm::ConstantPointerNull::get(PtrTy),
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0x40),
        llvm::ConstantPointerNull::get(PtrTy)};
    auto *CatchPad = PadBuilder.CreateCatchPad(CatchSwitch, Args);
    PadBuilder.CreateCatchRet(CatchPad, Exit);
  };
  AddFrame("unresolved_personality_frame_a");
  AddFrame("unresolved_personality_frame_b");

  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  ASSERT_FALSE(llvm::verifyModule(Module, &VerificationOS)) << Verification;

  CompiledImage Compiled = compileImageForPatch(
      Module, Arch::X64, BinaryFormat::COFF, 0x140004000,
      [](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "may_throw")
          return 0x140001100;
        return std::nullopt;
      },
      0x140000000);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_EQ(Compiled.Unresolved.size(), 1u);
  EXPECT_EQ(Compiled.Unresolved.front(), "__CxxFrameHandler3");
}

TEST(COFFExceptionIR, EmitsVerifierCleanNativeSimpleFH3Catch) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "native_cxx_test";
  Func.ReturnType = NdType::makeVoid();
  constexpr va_t MayThrowVA = 0x140001100;
  constexpr va_t TypeDescriptorVA = 0x140001200;

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = Func.Entry;
  Protected.EndAddr = Func.Entry + 0x10;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Func.Entry + 4;
  Call.addInput(MedVar::makeConst(MayThrowVA, 8));
  Protected.Ops.push_back(Call);
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = Func.Entry + 8;
  Protected.Ops.push_back(ProtectedReturn);

  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = Func.Entry + 0x20;
  Handler.EndAddr = Func.Entry + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = Func.Entry + 0x28;
  Handler.Ops.push_back(HandlerReturn);

  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x40};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  EH.PersonalityVA = Func.Entry + 0x180;
  CxxExceptionInfo Cxx;
  Cxx.Magic = 0x19930522;
  Cxx.Version = CxxFuncInfoVersion::WithEHFlags;
  Cxx.Flags = 1;
  Cxx.IsSynchronous = true;
  Cxx.MaxState = 2;
  CxxUnwindAction State0;
  State0.ToState = -1;
  State0.Kind = CxxUnwindAction::ActionKind::None;
  CxxUnwindAction State1;
  State1.ToState = 0;
  State1.Kind = CxxUnwindAction::ActionKind::None;
  Cxx.UnwindMap = {State0, State1};
  Cxx.IPMap = {
      {Func.Entry, 0}, {Func.Entry + 0x10, -1}, {Func.Entry + 0x20, 1}};
  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.Adjectives = 0x40;
  Catch.TypeDescriptorVA = TypeDescriptorVA;
  Catch.HandlerVA = Func.Entry + 0x20;
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(std::move(Try));
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  const va_t PersonalityVA = EH.PersonalityVA;
  Func.ExceptionMetadata = EH;
  MedFunc Personality =
      makeAddressBackedPersonality(PersonalityVA, "__CxxFrameHandler3");

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  Image.DynInfo.GuardFlags =
      uint32_t(llvm::COFF::GuardFlags::EH_CONTINUATION_TABLE_PRESENT) |
      uint32_t(llvm::COFF::GuardFlags::CF_INSTRUMENTED) |
      uint32_t(llvm::COFF::GuardFlags::CF_FUNCTION_TABLE_PRESENT);
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();

  llvm::LLVMContext Ctx;
  MedLLVMEmitter Emitter;
  auto Mod =
      Emitter.emit({Func, Personality}, Ctx, "native_cxx", Arch::X64,
                   {{MayThrowVA, "may_throw"}}, &Image, BinaryFormat::COFF);
  ASSERT_NE(Mod, nullptr);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  EXPECT_FALSE(llvm::verifyModule(*Mod, &VerificationOS)) << Verification;

  llvm::Function *F = Mod->getFunction(Func.Name);
  ASSERT_NE(F, nullptr);
  llvm::Function *Canonical = Mod->getFunction("__CxxFrameHandler3");
  ASSERT_NE(Canonical, nullptr);
  EXPECT_TRUE(Canonical->isDeclaration());
  EXPECT_TRUE(Canonical->getReturnType()->isIntegerTy(32));
  EXPECT_TRUE(Canonical->isVarArg());
  const std::string PersonalityBodyName =
      (kAutoFuncPrefix + llvm::utohexstr(PersonalityVA)).str();
  llvm::Function *PersonalityBody = Mod->getFunction(PersonalityBodyName);
  ASSERT_NE(PersonalityBody, nullptr);
  EXPECT_FALSE(PersonalityBody->isDeclaration());
  EXPECT_TRUE(PersonalityBody->getReturnType()->isIntegerTy(64));
  EXPECT_TRUE(F->hasPersonalityFn());
  EXPECT_NE(F->getMetadata(windows_eh_md::NativeAttachment), nullptr);
  expectCompleteRewriteContract(*F, 1);
  EXPECT_EQ(Mod->getModuleFlag("eh-asynch"), nullptr);
  EXPECT_NE(Mod->getModuleFlag("cfguard"), nullptr);
  EXPECT_NE(Mod->getModuleFlag("ehcontguard"), nullptr);

  std::string IR;
  llvm::raw_string_ostream OS(IR);
  Mod->print(OS, nullptr);
  EXPECT_NE(IR.find("personality ptr @__CxxFrameHandler3"), std::string::npos);
  EXPECT_NE(IR.find("invoke"), std::string::npos);
  EXPECT_NE(IR.find("catchswitch within none"), std::string::npos);
  EXPECT_NE(IR.find("catchpad within"), std::string::npos);
  EXPECT_NE(IR.find("i32 64"), std::string::npos);

  auto Plan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());

  llvm::CatchReturnInst *CatchReturn = nullptr;
  llvm::CatchPadInst *CatchPad = nullptr;
  llvm::InvokeInst *ProtectedInvoke = nullptr;
  for (llvm::BasicBlock &Block : *F)
    for (llvm::Instruction &Instruction : Block) {
      if (auto *Return = llvm::dyn_cast<llvm::CatchReturnInst>(&Instruction))
        CatchReturn = Return;
      if (auto *Pad = llvm::dyn_cast<llvm::CatchPadInst>(&Instruction))
        CatchPad = Pad;
      if (auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(&Instruction))
        ProtectedInvoke = Invoke;
    }
  ASSERT_NE(CatchReturn, nullptr);
  ASSERT_NE(ProtectedInvoke, nullptr);
  llvm::BasicBlock *HandlerContinuation = CatchReturn->getSuccessor();
  llvm::BasicBlock *WrongContinuation = ProtectedInvoke->getNormalDest();
  ASSERT_NE(HandlerContinuation, WrongContinuation);
  CatchReturn->setSuccessor(WrongContinuation);
  expectVerifierClean(*Mod);
  auto WrongContinuationPlan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(WrongContinuationPlan));
  EXPECT_NE(llvm::toString(WrongContinuationPlan.takeError())
                .find("catchret continuation"),
            std::string::npos);
  CatchReturn->setSuccessor(HandlerContinuation);
  expectVerifierClean(*Mod);
  auto RestoredContinuationPlan =
      planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(RestoredContinuationPlan))
      << llvm::toString(RestoredContinuationPlan.takeError());

  ASSERT_NE(CatchPad, nullptr);
  ASSERT_EQ(CatchPad->arg_size(), 3u);
  llvm::Value *OriginalRTTI = CatchPad->getArgOperand(0);
  CatchPad->setArgOperand(
      0, llvm::ConstantPointerNull::get(llvm::PointerType::get(Ctx, 0)));
  expectVerifierClean(*Mod);
  auto WrongRTTIPlan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(WrongRTTIPlan));
  EXPECT_NE(llvm::toString(WrongRTTIPlan.takeError()).find("catchpad RTTI"),
            std::string::npos);
  CatchPad->setArgOperand(0, OriginalRTTI);
  expectVerifierClean(*Mod);
  auto RestoredRTTIPlan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(RestoredRTTIPlan))
      << llvm::toString(RestoredRTTIPlan.takeError());

  llvm::Value *OriginalAdjectives = CatchPad->getArgOperand(1);
  CatchPad->setArgOperand(
      1, llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), 0x41));
  expectVerifierClean(*Mod);
  auto WrongAdjectivesPlan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(WrongAdjectivesPlan));
  EXPECT_NE(llvm::toString(WrongAdjectivesPlan.takeError())
                .find("catchpad adjectives"),
            std::string::npos);
  CatchPad->setArgOperand(1, OriginalAdjectives);
  expectVerifierClean(*Mod);
  auto RestoredAdjectivesPlan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(RestoredAdjectivesPlan))
      << llvm::toString(RestoredAdjectivesPlan.takeError());

  ensureCOFFCodegenTargets();
  constexpr uint64_t GeneratedVA = 0x140004000;
  CompiledImage Compiled = compileImageForPatch(
      *Mod, Arch::X64, BinaryFormat::COFF, GeneratedVA,
      [&](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "may_throw")
          return MayThrowVA;
        if (Symbol == "__CxxFrameHandler3")
          return PersonalityVA;
        if (Symbol == makeNdDataSymbol(TypeDescriptorVA))
          return TypeDescriptorVA;
        return std::nullopt;
      },
      Image.Base);
  ASSERT_TRUE(Compiled.Success);
  EXPECT_TRUE(Compiled.Unresolved.empty());
  EXPECT_TRUE(Compiled.SymbolAddrs.count(Func.Name));
  EXPECT_TRUE(Compiled.SymbolAddrs.count(PersonalityBodyName));

  const CompiledSection *EHCont = nullptr;
  const CompiledSection *Text = nullptr;
  for (const CompiledSection &Section : Compiled.Sections) {
    if (llvm::StringRef(Section.Name).starts_with(".gehcont"))
      EHCont = &Section;
    if (Section.Kind == llvm::mc_rewrite::RewriteSectionKind::Code)
      Text = &Section;
  }
  ASSERT_NE(Text, nullptr);
  ASSERT_NE(EHCont, nullptr);
  EXPECT_FALSE(EHCont->IsAllocated);
  EXPECT_EQ(EHCont->VA, 0u);
  ASSERT_FALSE(EHCont->SymbolIndexReferences.empty());
  for (const auto &Reference : EHCont->SymbolIndexReferences) {
    EXPECT_GE(Reference.TargetVA, Text->VA);
    EXPECT_LT(Reference.TargetVA, Text->VA + Text->Size);
  }

  auto *WrongPersonalityTy =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Ctx), {}, true);
  llvm::FunctionCallee WrongPersonality =
      Mod->getOrInsertFunction("__C_specific_handler", WrongPersonalityTy);
  F->setPersonalityFn(llvm::cast<llvm::Constant>(WrongPersonality.getCallee()));
  auto TamperedPlan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(TamperedPlan));
  EXPECT_NE(llvm::toString(TamperedPlan.takeError())
                .find("native WinEH IR contract was altered"),
            std::string::npos);
}

TEST(COFFExceptionIR, EmitsVerifierCleanNestedFH3CatchRegions) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "native_nested_cxx_test";
  Func.ReturnType = NdType::makeVoid();
  constexpr va_t MayThrowVA = 0x140001200;

  auto AddCallBlock = [&](int Id, va_t Begin) {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Begin;
    Block.EndAddr = Begin + 0x10;
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Begin + 4;
    Call.addInput(MedVar::makeConst(MayThrowVA, 8));
    Block.Ops.push_back(Call);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Begin + 8;
    Block.Ops.push_back(Return);
    Func.Blocks.push_back(std::move(Block));
  };
  auto AddHandlerBlock = [&](int Id, va_t Begin) {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Begin;
    Block.EndAddr = Begin + 0x10;
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Begin + 8;
    Block.Ops.push_back(Return);
    Func.Blocks.push_back(std::move(Block));
  };
  AddCallBlock(0, Func.Entry);
  AddCallBlock(1, Func.Entry + 0x10);
  AddHandlerBlock(2, Func.Entry + 0x40);
  AddHandlerBlock(3, Func.Entry + 0x50);

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x60};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  EH.PersonalityVA = Func.Entry + 0x180;
  CxxExceptionInfo Cxx;
  Cxx.Magic = 0x19930522;
  Cxx.Version = CxxFuncInfoVersion::WithEHFlags;
  Cxx.Flags = 1;
  Cxx.IsSynchronous = true;
  Cxx.MaxState = 4;
  for (int32_t State = 0; State < 4; ++State) {
    CxxUnwindAction Action;
    Action.ToState = State - 1;
    Action.Kind = CxxUnwindAction::ActionKind::None;
    Cxx.UnwindMap.push_back(Action);
  }
  Cxx.IPMap = {{Func.Entry, 0},
               {Func.Entry + 0x10, 1},
               {Func.Entry + 0x20, -1},
               {Func.Entry + 0x40, 3},
               {Func.Entry + 0x50, 2}};

  CxxTryBlock Outer;
  Outer.TryLow = 0;
  Outer.TryHigh = 1;
  Outer.CatchHigh = 3;
  CxxCatchHandler OuterCatch;
  OuterCatch.HandlerVA = Func.Entry + 0x40;
  Outer.Handlers.push_back(OuterCatch);
  Cxx.TryBlocks.push_back(std::move(Outer));

  CxxTryBlock Inner;
  Inner.TryLow = 1;
  Inner.TryHigh = 1;
  Inner.CatchHigh = 2;
  CxxCatchHandler InnerCatch;
  InnerCatch.HandlerVA = Func.Entry + 0x50;
  Inner.Handlers.push_back(InnerCatch);
  Cxx.TryBlocks.push_back(std::move(Inner));
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  Func.ExceptionMetadata = EH;

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();

  llvm::LLVMContext Ctx;
  MedLLVMEmitter Emitter;
  auto Mod =
      Emitter.emit({Func}, Ctx, "native_nested_cxx", Arch::X64,
                   {{MayThrowVA, "may_throw"}}, &Image, BinaryFormat::COFF);
  ASSERT_NE(Mod, nullptr);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  EXPECT_FALSE(llvm::verifyModule(*Mod, &VerificationOS)) << Verification;

  llvm::Function *F = Mod->getFunction(Func.Name);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->hasPersonalityFn());
  EXPECT_NE(F->getMetadata(windows_eh_md::NativeAttachment), nullptr);
  expectCompleteRewriteContract(*F, 2);

  std::string IR;
  llvm::raw_string_ostream OS(IR);
  Mod->print(OS, nullptr);
  EXPECT_NE(IR.find("cxx.catch.dispatch.0"), std::string::npos);
  EXPECT_NE(IR.find("cxx.catch.dispatch.1"), std::string::npos);
  EXPECT_NE(IR.find("unwind label %cxx.catch.dispatch.0"), std::string::npos);

  auto Plan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());

  // Retargeting the inner protected call to the outer dispatch preserves the
  // invoke count and leaves a verifier-clean funclet graph, but changes which
  // source try region handles the call.  Patch planning must authenticate that
  // edge rather than accepting an unchanged aggregate count.
  llvm::InvokeInst *InnerInvoke = nullptr;
  llvm::BasicBlock *OuterDispatch = nullptr;
  for (llvm::BasicBlock &Block : *F) {
    if (Block.getName() == "cxx.catch.dispatch.0")
      OuterDispatch = &Block;
    auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
    if (Invoke && Invoke->getUnwindDest()->getName() == "cxx.catch.dispatch.1")
      InnerInvoke = Invoke;
  }
  ASSERT_NE(InnerInvoke, nullptr);
  ASSERT_NE(OuterDispatch, nullptr);
  llvm::BasicBlock *InnerDispatch = InnerInvoke->getUnwindDest();
  InnerInvoke->setUnwindDest(OuterDispatch);

  std::string RetargetedVerification;
  llvm::raw_string_ostream RetargetedVerificationOS(RetargetedVerification);
  ASSERT_FALSE(llvm::verifyModule(*Mod, &RetargetedVerificationOS))
      << RetargetedVerification;
  auto RetargetedPlan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(RetargetedPlan));
  EXPECT_NE(
      llvm::toString(RetargetedPlan.takeError()).find("unwind destination"),
      std::string::npos);

  InnerInvoke->setUnwindDest(InnerDispatch);
  std::string RestoredVerification;
  llvm::raw_string_ostream RestoredVerificationOS(RestoredVerification);
  ASSERT_FALSE(llvm::verifyModule(*Mod, &RestoredVerificationOS))
      << RestoredVerification;
  auto RestoredPlan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(RestoredPlan))
      << llvm::toString(RestoredPlan.takeError());

  // A legal optimizer may turn one protected invoke back into an ordinary
  // call.  The remaining invoke and the complete funclet tree keep the module
  // verifier-clean, so a mere "has some WinEH" shape check is insufficient:
  // patch must recount the actual protected calls and reject the stale
  // producer contract.
  llvm::InvokeInst *DroppedInvoke = nullptr;
  for (llvm::BasicBlock &Block : *F)
    if (auto *Invoke =
            llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator())) {
      DroppedInvoke = Invoke;
      break;
    }
  ASSERT_NE(DroppedInvoke, nullptr);
  llvm::IRBuilder<> Builder(DroppedInvoke);
  llvm::SmallVector<llvm::Value *, 8> Args;
  for (llvm::Use &Arg : DroppedInvoke->args())
    Args.push_back(Arg.get());
  llvm::SmallVector<llvm::OperandBundleDef, 2> Bundles;
  DroppedInvoke->getOperandBundlesAsDefs(Bundles);
  llvm::CallInst *PlainCall =
      Builder.CreateCall(DroppedInvoke->getFunctionType(),
                         DroppedInvoke->getCalledOperand(), Args, Bundles);
  PlainCall->setCallingConv(DroppedInvoke->getCallingConv());
  PlainCall->setAttributes(DroppedInvoke->getAttributes());
  PlainCall->setDebugLoc(DroppedInvoke->getDebugLoc());
  PlainCall->copyMetadata(*DroppedInvoke);
  Builder.CreateBr(DroppedInvoke->getNormalDest());
  DroppedInvoke->replaceAllUsesWith(PlainCall);
  DroppedInvoke->eraseFromParent();

  std::string TamperedVerification;
  llvm::raw_string_ostream TamperedVerificationOS(TamperedVerification);
  ASSERT_FALSE(llvm::verifyModule(*Mod, &TamperedVerificationOS))
      << TamperedVerification;
  auto TamperedPlan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(TamperedPlan));
  EXPECT_NE(
      llvm::toString(TamperedPlan.takeError()).find("IR contains 1 invokes"),
      std::string::npos);
}

} // namespace
