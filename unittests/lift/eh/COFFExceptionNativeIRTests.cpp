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
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ARMWinEH.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

struct CxxContinuationPlanSnapshot {
  bool Complete = false;
  std::vector<std::pair<const llvm::ReturnInst *, const llvm::BasicBlock *>>
      Bindings;
};

class MedLLVMEmitterTestPeer {
public:
  static void prepare(MedLLVMEmitter &Emitter, llvm::LLVMContext &Context,
                      llvm::Module &Module, llvm::Function &Function,
                      const MedFunc &Source, Arch TargetArch = Arch::X64) {
    Emitter.Ctx = &Context;
    Emitter.Mod = &Module;
    Emitter.Img = nullptr;
    Emitter.TargetArch = TargetArch;
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
    Emitter.EmittedFuncNames[Address] = Name.str();
  }

  static std::optional<CxxContinuationPlanSnapshot>
  cxxContinuationPlan(const MedLLVMEmitter &Emitter, va_t SourceVA) {
    const MedLLVMEmitter::CxxContinuationFunctionPlan *Found = nullptr;
    for (const auto &Plan : Emitter.CxxContinuationPlans) {
      if (Plan.SourceVA != SourceVA)
        continue;
      if (Found)
        return std::nullopt;
      Found = &Plan;
    }
    if (!Found)
      return std::nullopt;

    CxxContinuationPlanSnapshot Snapshot;
    Snapshot.Complete = Found->Complete;
    for (const auto &Binding : Found->Bindings)
      Snapshot.Bindings.emplace_back(Binding.Return, Binding.TargetBlock);
    return Snapshot;
  }

  static bool sameCxxContinuationReturnValue(const MedVar &Left,
                                             const MedVar &Right) {
    return MedLLVMEmitter::sameCxxContinuationReturnValue(Left, Right);
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

MedFunc makeContinuationPlanFunction(va_t Entry, llvm::StringRef Name,
                                     llvm::ArrayRef<va_t> BlockAddresses) {
  MedFunc Func;
  Func.Entry = Entry;
  Func.Name = Name.str();
  Func.ReturnType = NdType::makeVoid();
  Func.CxxContinuationExitAnalysisComplete = true;
  for (size_t I = 0; I < BlockAddresses.size(); ++I) {
    MedBlock Block;
    Block.Id = static_cast<int>(I);
    Block.StartAddr = BlockAddresses[I];
    Block.EndAddr = Block.StartAddr + 0x10;
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Block.StartAddr + 8;
    Return.OriginSeq = static_cast<int>(I + 1);
    Block.Ops.push_back(std::move(Return));
    Func.Blocks.push_back(std::move(Block));
  }
  return Func;
}

void setSeparatedFH3GroupIdentity(MedFunc &Func, va_t NativeFuncInfoVA,
                                  bool IsCatchFunclet) {
  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x100};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.NativeEncoding = CxxExceptionInfo::Encoding::FH3;
  Cxx.NativeFuncInfoVA = NativeFuncInfoVA;
  Cxx.IsSeparated = true;
  Cxx.IsCatchFunclet = IsCatchFunclet;
  Cxx.IsSynchronous = true;
  EH.Cxx = std::move(Cxx);
  Func.ExceptionMetadata = std::move(EH);
}

void setSeparatedFH3GroupIdentity(MedFunc &Parent, MedFunc &Catch,
                                  va_t NativeFuncInfoVA) {
  setSeparatedFH3GroupIdentity(Parent, NativeFuncInfoVA,
                               /*IsCatchFunclet=*/false);
  setSeparatedFH3GroupIdentity(Catch, NativeFuncInfoVA,
                               /*IsCatchFunclet=*/true);
  CxxTryBlock Try;
  CxxCatchHandler Handler;
  Handler.HandlerVA = Catch.Entry;
  Try.Handlers.push_back(std::move(Handler));
  Parent.ExceptionMetadata->Cxx->TryBlocks.push_back(std::move(Try));
}

void addContinuationEvidence(MedFunc &Func, size_t BlockIndex,
                             std::vector<va_t> Targets) {
  ASSERT_LT(BlockIndex, Func.Blocks.size());
  MedOp &Return = Func.Blocks[BlockIndex].Ops.back();
  Return.addInput(MedVar::makeConst(
      Targets.empty() ? 0 : Targets.front(), /*Sz=*/8,
      ConstantAddressProvenance::CodeAddress));

  MedCxxContinuationExitEvidence Evidence;
  Evidence.ReturnAddr = Return.Addr;
  Evidence.ReturnSeq = Return.OriginSeq;
  Evidence.BlockId = Func.Blocks[BlockIndex].Id;
  Evidence.ReturnValue = Return.Inputs[0];
  Evidence.Targets = std::move(Targets);
  Evidence.Complete = true;
  Func.CxxContinuationExits.push_back(std::move(Evidence));
}

void expectNoContinuationReturnMarkers(const llvm::Module &Module) {
  for (const llvm::Function &Function : Module)
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block)
        EXPECT_EQ(Instruction.getMetadata(
                      language_eh_md::InternalCxxContinuationReturnAttachment),
                  nullptr);
}

bool hasCompleteContinuationPlan(std::vector<MedFunc> Funcs, va_t SourceVA,
                                 const std::vector<char> *BodyMask = nullptr,
                                 bool UseImage = true,
                                 BinaryFormat Format = BinaryFormat::COFF) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = Format;
  Image.Base = 0x140000000;

  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  auto Module = Emitter.emit(Funcs, Context, "continuation_plan_case",
                             Arch::X64, {}, UseImage ? &Image : nullptr,
                             Format, /*MergeableGlobals=*/false, BodyMask);
  EXPECT_NE(Module, nullptr);
  if (!Module)
    return false;
  expectVerifierClean(*Module);
  expectNoContinuationReturnMarkers(*Module);
  const auto Plan =
      MedLLVMEmitterTestPeer::cxxContinuationPlan(Emitter, SourceVA);
  EXPECT_TRUE(Plan.has_value());
  return Plan && Plan->Complete;
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

TEST(COFFExceptionIR, EmitsVerifierCleanBoundedFH4CatchAllContract) {
  DirectCxxFixture Fixture("native_bounded_fh4", true);
  ExceptionFunction &EH = *Fixture.Source.ExceptionMetadata;
  CxxExceptionInfo &Cxx = *EH.Cxx;
  EH.Personality = ExceptionPersonality::CxxFrameHandler4;
  Cxx.NativeEncoding = CxxExceptionInfo::Encoding::FH4;
  Cxx.Magic = 0;
  Cxx.Flags = 0x38;
  Cxx.UnwindMap[1].ToState = -1;
  Cxx.IPMap = {{EH.CodeRange.Begin, -1},
               {EH.CodeRange.Begin + 4, 0},
               {EH.CodeRange.Begin + 0x10, -1}};
  Cxx.TryBlocks.front().Handlers.front().TypeDescriptorVA = 0;
  Fixture.Source.Blocks.front().StartAddr = EH.CodeRange.Begin + 4;

  const WindowsEHNativeSourceClassification Classification =
      classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::IRLowering);
  ASSERT_TRUE(Classification.canLowerNativeIR());
  ASSERT_EQ(Classification.Model, WindowsEHNativeSourceModel::CxxFH4);

  MedLLVMEmitter Emitter;
  MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                  *Fixture.Function, Fixture.Source);
  MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                             EH.CodeRange.Begin + 4);
  ASSERT_TRUE(MedLLVMEmitterTestPeer::emitCxx(
      Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap));
  expectVerifierClean(Fixture.Module);

  ASSERT_TRUE(Fixture.Function->hasPersonalityFn());
  const auto *Personality = llvm::dyn_cast<llvm::Function>(
      Fixture.Function->getPersonalityFn()->stripPointerCasts());
  ASSERT_NE(Personality, nullptr);
  EXPECT_EQ(Personality->getName(), "__CxxFrameHandler4");
  EXPECT_TRUE(Fixture.Function->hasFnAttribute(
      llvm::mc_rewrite::RewriteWinCxxFH4Attribute));
  expectCompleteRewriteContract(*Fixture.Function, 1);

  const llvm::MDNode *Native =
      Fixture.Function->getMetadata(windows_eh_md::NativeAttachment);
  ASSERT_NE(Native, nullptr);
  ASSERT_EQ(Native->getNumOperands(), 2u);
  const auto *Kind =
      llvm::dyn_cast_or_null<llvm::MDString>(Native->getOperand(1).get());
  ASSERT_NE(Kind, nullptr);
  EXPECT_EQ(Kind->getString(), "cxx-fh4-native");

  size_t FH4ProvenanceAnchors = 0;
  for (const llvm::BasicBlock &Block : *Fixture.Function)
    for (const llvm::Instruction &Instruction : Block) {
      const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction);
      if (!Call)
        continue;
      auto Bundle = Call->getOperandBundle(windows_eh_md::ProvenanceBundle);
      if (!Bundle)
        continue;
      ASSERT_EQ(Bundle->Inputs.size(),
                windows_eh_md::ProvenanceOperandCount);
      const auto *Model = llvm::dyn_cast<llvm::ConstantInt>(
          Bundle->Inputs[windows_eh_md::ProvenanceModel].get());
      ASSERT_NE(Model, nullptr);
      EXPECT_EQ(Model->getZExtValue(), static_cast<unsigned>(
                                          windows_eh_md::NativeProvenanceModel::
                                              CxxFH4));
      ++FH4ProvenanceAnchors;
    }
  EXPECT_EQ(FH4ProvenanceAnchors, 3u);
}

TEST(COFFExceptionIR, EmitsCompilerOwnedGSContractForBoundedFH4) {
  DirectCxxFixture Fixture("native_bounded_gs_fh4", true);
  ExceptionFunction &EH = *Fixture.Source.ExceptionMetadata;
  CxxExceptionInfo &Cxx = *EH.Cxx;
  EH.Personality = ExceptionPersonality::GSHandlerCheckEH4;
  Cxx.NativeEncoding = CxxExceptionInfo::Encoding::FH4;
  Cxx.Magic = 0;
  Cxx.Flags = 0x38;
  Cxx.UnwindMap[1].ToState = -1;
  Cxx.IPMap = {{EH.CodeRange.Begin, -1},
               {EH.CodeRange.Begin + 4, 0},
               {EH.CodeRange.Begin + 0x10, -1}};
  Cxx.TryBlocks.front().Handlers.front().TypeDescriptorVA = 0;
  Fixture.Source.Blocks.front().StartAddr = EH.CodeRange.Begin + 4;
  GSCookieInfo Cookie;
  Cookie.ParseStatus = ExceptionParseStatus::Complete;
  Cookie.CookieOffset = 0x20;
  Cookie.HasExceptionHandler = true;
  Cookie.HasUnwindHandler = true;
  Cookie.Payload = {0x23, 0, 0, 0};
  EH.GSCookie = Cookie;

  const WindowsEHNativeSourceClassification Classification =
      classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::IRLowering);
  ASSERT_TRUE(Classification.canLowerNativeIR());
  ASSERT_EQ(Classification.Model, WindowsEHNativeSourceModel::CxxFH4);

  MedLLVMEmitter Emitter;
  MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                  *Fixture.Function, Fixture.Source);
  MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                             EH.CodeRange.Begin + 4);
  ASSERT_TRUE(MedLLVMEmitterTestPeer::emitCxx(
      Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap));
  expectVerifierClean(Fixture.Module);

  ASSERT_TRUE(Fixture.Function->hasPersonalityFn());
  const auto *Personality = llvm::dyn_cast<llvm::Function>(
      Fixture.Function->getPersonalityFn()->stripPointerCasts());
  ASSERT_NE(Personality, nullptr);
  EXPECT_EQ(Personality->getName(), "__CxxFrameHandler4");
  EXPECT_TRUE(Fixture.Function->hasFnAttribute(
      llvm::mc_rewrite::RewriteWinCxxFH4Attribute));
  const llvm::Attribute GS = Fixture.Function->getFnAttribute(
      llvm::mc_rewrite::RewriteWinGSHandlerAttribute);
  ASSERT_TRUE(GS.isStringAttribute());
  EXPECT_EQ(GS.getValueAsString(), llvm::mc_rewrite::RewriteWinGSHandlerCxxFH4);
  EXPECT_TRUE(
      Fixture.Function->hasFnAttribute(llvm::Attribute::StackProtectReq));
  const llvm::Function *Wrapper =
      Fixture.Module.getFunction("__GSHandlerCheck_EH4");
  ASSERT_NE(Wrapper, nullptr);
  EXPECT_TRUE(Wrapper->isDeclaration());
  EXPECT_EQ(Wrapper->getFunctionType(), Personality->getFunctionType());
  expectCompleteRewriteContract(*Fixture.Function, 1);

  constexpr va_t MayThrowVA = 0x140002000;
  constexpr va_t SecurityCookieVA = 0x140003000;
  constexpr va_t SecurityCheckVA = 0x140004000;
  ensureCOFFCodegenTargets();
  CompiledImage Compiled = compileImageForPatch(
      Fixture.Module, Arch::X64, BinaryFormat::COFF,
      /*BaseVA=*/0x140005000,
      [&](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "may_throw")
          return MayThrowVA;
        if (Symbol == "__CxxFrameHandler4")
          return Fixture.Source.Entry + 0x180;
        if (Symbol == "__GSHandlerCheck_EH4")
          return EH.PersonalityVA;
        if (Symbol == "__security_cookie")
          return SecurityCookieVA;
        if (Symbol == "__security_check_cookie")
          return SecurityCheckVA;
        return std::nullopt;
      },
      /*ImageBaseVA=*/0x140000000);
  ASSERT_TRUE(Compiled.Success);
  EXPECT_TRUE(Compiled.Unresolved.empty());
  EXPECT_TRUE(Compiled.WinEHSemanticsValid);
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 1u);
  EXPECT_EQ(Compiled.WinEHSemanticRecords.front().Encoding,
            llvm::mc_rewrite::RewriteWinEHSemanticEncoding::CxxFH4);
  EXPECT_EQ(Compiled.WinEHSemanticRecords.front().RecordSize, 6u);
}

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
  EH.PersonalityVA = Func.Entry + 0x300;
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

TEST(COFFExceptionIR,
     EmitsVerifierCleanReconstructableLegacyAArch64CatchAllSEH) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "aarch64_native_seh_analysis";
  Func.ReturnType = NdType::makeVoid();
  constexpr va_t MayThrowVA = 0x140002000;

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = Func.Entry;
  Protected.EndAddr = Func.Entry + 0x10;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Func.Entry + 4;
  Call.addInput(MedVar::makeConst(MayThrowVA, 8));
  Protected.Ops.push_back(std::move(Call));
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = Func.Entry + 8;
  Protected.Ops.push_back(std::move(ProtectedReturn));

  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = Func.Entry + 0x20;
  Handler.EndAddr = Func.Entry + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = Func.Entry + 0x28;
  Handler.Ops.push_back(std::move(HandlerReturn));
  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x40};
  EH.Encoding = ExceptionEncoding::ARM64Unpacked;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = Func.Entry + 0x100;
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.ParseStatus = ExceptionParseStatus::Complete;
  // LLVM through 20 encoded the exclusive end one byte past the address of
  // the final fixed-width instruction.  Preserve that raw spelling while the
  // native lowering uses its equivalent aligned instruction-domain range.
  Scope.GuardedRange = {Func.Entry, Func.Entry + 0x0d};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Func.Entry + 0x20;
  Scope.ContinuationVA = Scope.HandlerVA;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);
  const va_t PersonalityVA = EH.PersonalityVA;

  const WindowsEHNativeSourceClassification IRSource =
      classifyWindowsEHNativeSource(EH, Arch::AArch64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::IRLowering);
  EXPECT_TRUE(IRSource.canLowerNativeIR());
  EXPECT_FALSE(IRSource.canPatchOutput());
  EXPECT_EQ(IRSource.Model, WindowsEHNativeSourceModel::SEH);
  EXPECT_EQ(IRSource.Reason, WindowsEHNativeSourceReason::Eligible);

  const WindowsEHNativeSourceClassification PatchSource =
      classifyWindowsEHNativeSource(EH, Arch::AArch64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::OutputPatch);
  EXPECT_FALSE(PatchSource.canLowerNativeIR());
  EXPECT_TRUE(PatchSource.canPatchOutput());
  EXPECT_TRUE(PatchSource.canRegenerateLanguageMetadata());
  EXPECT_EQ(PatchSource.Model, WindowsEHNativeSourceModel::SEH);
  EXPECT_EQ(PatchSource.Reason, WindowsEHNativeSourceReason::Eligible);

  auto ExpectRejected = [&](const ExceptionFunction &Candidate, Arch TargetArch,
                            WindowsEHNativeSourceReason Reason) {
    for (WindowsEHNativeCapability Capability :
         {WindowsEHNativeCapability::IRLowering,
          WindowsEHNativeCapability::OutputPatch}) {
      const WindowsEHNativeSourceClassification Result =
          classifyWindowsEHNativeSource(Candidate, TargetArch,
                                        BinaryFormat::COFF, Capability);
      EXPECT_FALSE(Result.isEligible());
      EXPECT_EQ(Result.Reason, Reason);
    }
  };
  ExceptionFunction Packed = EH;
  Packed.Encoding = ExceptionEncoding::ARM64Packed;
  ExpectRejected(Packed, Arch::AArch64,
                 WindowsEHNativeSourceReason::UnsupportedUnwindEncoding);
  ExceptionFunction FH3 = EH;
  FH3.Personality = ExceptionPersonality::CxxFrameHandler3;
  ExpectRejected(FH3, Arch::AArch64,
                 WindowsEHNativeSourceReason::ConflictingLanguageModel);
  ExceptionFunction WithGS = EH;
  WithGS.GSCookie.emplace();
  ExpectRejected(WithGS, Arch::AArch64,
                 WindowsEHNativeSourceReason::UnexpectedGSCookie);
  ExceptionFunction Fragment = EH;
  Fragment.Kind = RuntimeFunctionKind::Fragment;
  ExpectRejected(Fragment, Arch::AArch64,
                 WindowsEHNativeSourceReason::NonPrimaryRuntimeFunction);
  ExceptionFunction Partial = EH;
  Partial.ParseStatus = ExceptionParseStatus::Partial;
  ExpectRejected(Partial, Arch::AArch64,
                 WindowsEHNativeSourceReason::IncompleteDecode);
  ExceptionFunction Overlay = EH;
  Overlay.Rust.emplace();
  ExpectRejected(Overlay, Arch::AArch64,
                 WindowsEHNativeSourceReason::LanguageOverlay);
  ExceptionFunction Filter = EH;
  Filter.SEH->Scopes.front().Kind = SEHScopeKind::Filter;
  Filter.SEH->Scopes.front().FilterOrFinallyVA = Func.Entry + 0x200;
  ExpectRejected(Filter, Arch::AArch64,
                 WindowsEHNativeSourceReason::UnsupportedSEHCallbackABI);
  ExceptionFunction Finally = EH;
  Finally.SEH->Scopes.front().Kind = SEHScopeKind::Finally;
  Finally.SEH->Scopes.front().FilterOrFinallyVA = Func.Entry + 0x200;
  Finally.SEH->Scopes.front().HandlerVA = Func.Entry + 0x200;
  Finally.SEH->Scopes.front().ContinuationVA = 0;
  ExpectRejected(Finally, Arch::AArch64,
                 WindowsEHNativeSourceReason::UnsupportedSEHCallbackABI);
  ExpectRejected(EH, Arch::ARM,
                 WindowsEHNativeSourceReason::UnsupportedUnwindEncoding);

  Func.ExceptionMetadata = EH;
  MedFunc Personality =
      makeAddressBackedPersonality(PersonalityVA, "\01__C_specific_handler");

  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  auto Module = Emitter.emit({Func, Personality}, Context, "aarch64_native_seh",
                             Arch::AArch64, {{MayThrowVA, "may_throw"}},
                             nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectVerifierClean(*Module);

  llvm::Function *Function = Module->getFunction(Func.Name);
  ASSERT_NE(Function, nullptr);
  EXPECT_TRUE(Function->hasPersonalityFn());
  llvm::Function *Canonical = Module->getFunction("__C_specific_handler");
  ASSERT_NE(Canonical, nullptr);
  EXPECT_TRUE(Canonical->isDeclaration());
  EXPECT_TRUE(Canonical->getReturnType()->isIntegerTy(32));
  EXPECT_TRUE(Canonical->isVarArg());
  const std::string PersonalityBodyName =
      (kAutoFuncPrefix + llvm::utohexstr(PersonalityVA)).str();
  llvm::Function *PersonalityBody = Module->getFunction(PersonalityBodyName);
  ASSERT_NE(PersonalityBody, nullptr);
  EXPECT_FALSE(PersonalityBody->isDeclaration());

  const llvm::MDNode *Native =
      Function->getMetadata(windows_eh_md::NativeAttachment);
  ASSERT_NE(Native, nullptr);
  ASSERT_EQ(Native->getNumOperands(), 2u);
  const auto *NativeKind =
      llvm::dyn_cast<llvm::MDString>(Native->getOperand(1).get());
  ASSERT_NE(NativeKind, nullptr);
  EXPECT_EQ(NativeKind->getString(), "seh-aarch64-native");

  const llvm::MDNode *Contract =
      Function->getMetadata(exception_rewrite::FunctionAttachment);
  ASSERT_NE(Contract, nullptr);
  EXPECT_EQ(metadataInteger(Contract, exception_rewrite::Source, 8),
            static_cast<uint8_t>(exception_rewrite::SourceState::Complete));
  EXPECT_EQ(metadataInteger(Contract, exception_rewrite::Lowering, 8),
            static_cast<uint8_t>(exception_rewrite::LoweringState::Complete));
  EXPECT_EQ(
      metadataInteger(Contract, exception_rewrite::RequiredProtectedCalls, 64),
      1u);
  EXPECT_EQ(
      metadataInteger(Contract, exception_rewrite::LoweredProtectedCalls, 64),
      1u);
  EXPECT_EQ(
      metadataInteger(Contract, exception_rewrite::SkippedLandingPads, 64), 0u);

  const llvm::MDNode *SourceMetadata =
      Function->getMetadata(windows_eh_md::FunctionAttachment);
  ASSERT_NE(SourceMetadata, nullptr);
  EXPECT_EQ(metadataInteger(SourceMetadata, windows_eh_md::CanRegenerate, 1),
            1u);
  EXPECT_NE(Module->getModuleFlag("eh-asynch"), nullptr);

  bool SawMayThrowInvoke = false;
  for (const llvm::BasicBlock &Block : *Function)
    for (const llvm::Instruction &Instruction : Block) {
      const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(&Instruction);
      const llvm::Function *Callee =
          Invoke ? Invoke->getCalledFunction() : nullptr;
      SawMayThrowInvoke |= Callee && Callee->getName() == "may_throw";
    }
  EXPECT_TRUE(SawMayThrowInvoke);

  const std::string IR = printModuleIR(*Module);
  EXPECT_NE(IR.find("invoke void @llvm.seh.try.begin"), std::string::npos);
  EXPECT_NE(IR.find("invoke void @llvm.seh.try.end"), std::string::npos);
  EXPECT_NE(IR.find("catchswitch within none"), std::string::npos);
  EXPECT_NE(IR.find("catchpad within"), std::string::npos);
  EXPECT_NE(IR.find("catchret from"), std::string::npos);

  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();
  auto InitialPlan = planCOFFExceptionPatch(*Module, Image, Arch::AArch64);
  ASSERT_TRUE(static_cast<bool>(InitialPlan))
      << llvm::toString(InitialPlan.takeError());

  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Deep;
  Options.LLVMLevel = llvm::OptimizationLevel::O3;
  OptimizationResult Optimization = Pipeline::optimizeModule(*Module, Options);
  EXPECT_NE(Optimization.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_NE(Optimization.Stop, OptimizationStopReason::VerificationFailed);
  expectVerifierClean(*Module);
  auto OptimizedPlan =
      planCOFFExceptionPatch(*Module, Image, Arch::AArch64);
  ASSERT_TRUE(static_cast<bool>(OptimizedPlan))
      << llvm::toString(OptimizedPlan.takeError());

  ensureCOFFCodegenTargets();
  CompiledImage Compiled = compileImageForPatch(
      *Module, Arch::AArch64, BinaryFormat::COFF, 0x140004000,
      [&](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "may_throw")
          return MayThrowVA;
        if (Symbol == "__C_specific_handler")
          return PersonalityVA;
        return std::nullopt;
      },
      0x140000000);
  ASSERT_TRUE(Compiled.Success);
  EXPECT_TRUE(Compiled.Unresolved.empty());
  EXPECT_TRUE(
      llvm::any_of(Compiled.Sections, [](const CompiledSection &Section) {
        return llvm::StringRef(Section.Name).starts_with(".pdata");
      }));
  EXPECT_TRUE(
      llvm::any_of(Compiled.Sections, [](const CompiledSection &Section) {
        return llvm::StringRef(Section.Name).starts_with(".xdata");
      }));

  constexpr uint64_t ImageBase = 0x140000000;
  const auto FunctionAddress = Compiled.SymbolAddrs.find(Func.Name);
  ASSERT_NE(FunctionAddress, Compiled.SymbolAddrs.end());
  ASSERT_GE(FunctionAddress->second, ImageBase);
  ASSERT_LE(FunctionAddress->second - ImageBase,
            std::numeric_limits<uint32_t>::max());
  const uint32_t FunctionRVA =
      static_cast<uint32_t>(FunctionAddress->second - ImageBase);

  auto SectionBytes = [&](const CompiledSection &Section) {
    if (!Section.IsInImage)
      return llvm::ArrayRef<uint8_t>(Section.ExternalBytes);
    if (!rangeInBounds(Section.Offset, Section.Size, Compiled.Bytes.size()))
      return llvm::ArrayRef<uint8_t>();
    return llvm::ArrayRef<uint8_t>(Compiled.Bytes)
        .slice(static_cast<size_t>(Section.Offset),
               static_cast<size_t>(Section.Size));
  };

  std::optional<uint32_t> XDataRVA;
  for (const CompiledSection &Section : Compiled.Sections) {
    if (!llvm::StringRef(Section.Name).starts_with(".pdata"))
      continue;
    const llvm::ArrayRef<uint8_t> Bytes = SectionBytes(Section);
    ASSERT_EQ(Bytes.size(), Section.Size);
    ASSERT_EQ(Bytes.size() % 8, 0u);
    for (size_t Offset = 0; Offset < Bytes.size(); Offset += 8) {
      llvm::support::ulittle32_t RuntimeWords[2];
      RuntimeWords[0] = readLE<uint32_t>(Bytes.data() + Offset);
      RuntimeWords[1] = readLE<uint32_t>(Bytes.data() + Offset + 4);
      llvm::ARM::WinEH::RuntimeFunctionARM64 Runtime(RuntimeWords);
      if (Runtime.BeginAddress != FunctionRVA)
        continue;
      ASSERT_FALSE(XDataRVA.has_value());
      ASSERT_EQ(Runtime.Flag(),
                llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Unpacked);
      XDataRVA = Runtime.ExceptionInformationRVA();
    }
  }
  ASSERT_TRUE(XDataRVA.has_value());

  const uint64_t XDataVA = ImageBase + *XDataRVA;
  const CompiledSection *XDataSection = nullptr;
  size_t XDataOffset = 0;
  for (const CompiledSection &Section : Compiled.Sections) {
    if (XDataVA < Section.VA || XDataVA - Section.VA >= Section.Size)
      continue;
    ASSERT_EQ(XDataSection, nullptr);
    XDataSection = &Section;
    XDataOffset = static_cast<size_t>(XDataVA - Section.VA);
  }
  ASSERT_NE(XDataSection, nullptr);
  EXPECT_TRUE(llvm::StringRef(XDataSection->Name).starts_with(".xdata"));
  const llvm::ArrayRef<uint8_t> XDataBytes = SectionBytes(*XDataSection);
  ASSERT_EQ(XDataBytes.size(), XDataSection->Size);
  ASSERT_EQ(XDataOffset % sizeof(uint32_t), 0u);
  ASSERT_LE(XDataOffset + 2 * sizeof(uint32_t), XDataBytes.size());

  std::vector<llvm::support::ulittle32_t> XDataWords(
      (XDataBytes.size() - XDataOffset) / sizeof(uint32_t));
  for (size_t I = 0; I < XDataWords.size(); ++I)
    XDataWords[I] = readLE<uint32_t>(XDataBytes.data() + XDataOffset +
                                     I * sizeof(uint32_t));
  ASSERT_GE(XDataWords.size(), 2u);

  llvm::ARM::WinEH::ExceptionDataRecord XData(XDataWords.data(),
                                              /*isAArch64=*/true);
  EXPECT_EQ(XData.Vers(), 0u);
  ASSERT_TRUE(XData.X());
  ASSERT_GT(XData.FunctionLengthInBytesAArch64(), 0u);
  const size_t HandlerWord = llvm::ARM::WinEH::HeaderWords(XData) +
                             (XData.E() ? 0 : XData.EpilogueCount()) +
                             XData.CodeWords();
  ASSERT_LE(HandlerWord + 2, XDataWords.size());
  ASSERT_GE(PersonalityVA, ImageBase);
  EXPECT_EQ(XData.ExceptionHandlerRVA(), PersonalityVA - ImageBase);

  const uint32_t ScopeCount = XData.ExceptionHandlerParameter();
  ASSERT_EQ(ScopeCount, 1u);
  ASSERT_LE(ScopeCount, (XDataWords.size() - (HandlerWord + 2)) / 4);
  const uint64_t FunctionEndRVA =
      uint64_t(FunctionRVA) + XData.FunctionLengthInBytesAArch64();
  ASSERT_LE(FunctionEndRVA, std::numeric_limits<uint32_t>::max());
  for (uint32_t I = 0; I < ScopeCount; ++I) {
    const size_t ScopeWord = HandlerWord + 2 + size_t(I) * 4;
    const uint32_t BeginRVA = XDataWords[ScopeWord];
    const uint32_t EndRVA = XDataWords[ScopeWord + 1];
    const uint32_t FilterOrFinallyRVA = XDataWords[ScopeWord + 2];
    const uint32_t HandlerRVA = XDataWords[ScopeWord + 3];
    EXPECT_GE(BeginRVA, FunctionRVA);
    EXPECT_LT(BeginRVA, EndRVA);
    EXPECT_LE(EndRVA, FunctionEndRVA);
    EXPECT_EQ(EndRVA & 3u, 0u);
    EXPECT_EQ(FilterOrFinallyRVA, 1u);
    EXPECT_GE(HandlerRVA, FunctionRVA);
    EXPECT_LT(HandlerRVA, FunctionEndRVA);
  }

  std::unique_ptr<llvm::Module> WrongMarker = llvm::CloneModule(*Module);
  llvm::Function *WrongFunction = WrongMarker->getFunction(Func.Name);
  ASSERT_NE(WrongFunction, nullptr);
  WrongFunction->setMetadata(
      windows_eh_md::NativeAttachment,
      llvm::MDNode::get(
          WrongMarker->getContext(),
          {llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
               llvm::Type::getInt1Ty(WrongMarker->getContext()), 1)),
           llvm::MDString::get(WrongMarker->getContext(), "seh-x64-native")}));
  auto WrongMarkerPlan =
      planCOFFExceptionPatch(*WrongMarker, Image, Arch::AArch64);
  ASSERT_FALSE(static_cast<bool>(WrongMarkerPlan));
  EXPECT_NE(llvm::toString(WrongMarkerPlan.takeError())
                .find("native WinEH lowering is unavailable"),
            std::string::npos);
}

TEST(COFFExceptionIR, ClassifiesExactARM32CatchAllSEHForNativeReconstruction) {
  ExceptionFunction EH;
  EH.CodeRange = {0x10001000, 0x10001040};
  EH.Encoding = ExceptionEncoding::ARM32Unpacked;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = 0x10002000;
  EH.SEH.emplace();

  SEHScopeRecord Scope;
  Scope.GuardedRange = {0x10001000, 0x10001010};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = 0x10001020;
  Scope.ContinuationVA = Scope.HandlerVA;
  EH.SEH->Scopes.push_back(Scope);

  for (WindowsEHNativeCapability Capability :
       {WindowsEHNativeCapability::IRLowering,
        WindowsEHNativeCapability::OutputPatch}) {
    const WindowsEHNativeSourceClassification Source =
        classifyWindowsEHNativeSource(EH, Arch::ARM, BinaryFormat::COFF,
                                      Capability);
    EXPECT_TRUE(Source.isEligible())
        << getWindowsEHNativeSourceReasonName(Source.Reason);
    EXPECT_EQ(Source.Model, WindowsEHNativeSourceModel::SEH);
    EXPECT_EQ(Source.Reason, WindowsEHNativeSourceReason::Eligible);
  }

  auto ExpectRejected = [&](const ExceptionFunction &Candidate,
                            WindowsEHNativeSourceReason Reason) {
    for (WindowsEHNativeCapability Capability :
         {WindowsEHNativeCapability::IRLowering,
          WindowsEHNativeCapability::OutputPatch}) {
      const WindowsEHNativeSourceClassification Source =
          classifyWindowsEHNativeSource(Candidate, Arch::ARM,
                                        BinaryFormat::COFF, Capability);
      EXPECT_FALSE(Source.isEligible());
      EXPECT_EQ(Source.Reason, Reason);
    }
  };

  ExceptionFunction Packed = EH;
  Packed.Encoding = ExceptionEncoding::ARM32Packed;
  ExpectRejected(Packed,
                 WindowsEHNativeSourceReason::UnsupportedUnwindEncoding);

  ExceptionFunction Filter = EH;
  Filter.SEH->Scopes.front().Kind = SEHScopeKind::Filter;
  Filter.SEH->Scopes.front().FilterOrFinallyVA = 0x10002020;
  ExpectRejected(Filter,
                 WindowsEHNativeSourceReason::UnsupportedSEHCallbackABI);

  ExceptionFunction Finally = EH;
  Finally.SEH->Scopes.front().Kind = SEHScopeKind::Finally;
  Finally.SEH->Scopes.front().FilterOrFinallyVA = 0x10002020;
  Finally.SEH->Scopes.front().HandlerVA = 0x10002020;
  Finally.SEH->Scopes.front().ContinuationVA = 0;
  ExpectRejected(Finally,
                 WindowsEHNativeSourceReason::UnsupportedSEHCallbackABI);

  ExceptionFunction Multiple = EH;
  SEHScopeRecord Second = Scope;
  Second.GuardedRange = {0x10001010, 0x10001018};
  Second.HandlerVA = 0x10001028;
  Second.ContinuationVA = Second.HandlerVA;
  Multiple.SEH->Scopes.push_back(Second);
  ExpectRejected(Multiple,
                 WindowsEHNativeSourceReason::UnsupportedSEHScopeGraph);

  ExceptionFunction OddHandler = EH;
  OddHandler.SEH->Scopes.front().HandlerVA |= 1;
  OddHandler.SEH->Scopes.front().ContinuationVA =
      OddHandler.SEH->Scopes.front().HandlerVA;
  ExpectRejected(OddHandler, WindowsEHNativeSourceReason::InvalidSEHScope);

  ExceptionFunction WithGS = EH;
  WithGS.GSCookie.emplace();
  ExpectRejected(WithGS, WindowsEHNativeSourceReason::UnexpectedGSCookie);
}

TEST(COFFExceptionIR, EmitsVerifierCleanNativeARM32CatchAllSEH) {
  MedFunc Func;
  Func.Entry = 0x10001000;
  Func.Name = "arm32_native_seh_test";
  Func.ReturnType = NdType::makeVoid();
  constexpr va_t MayThrowVA = 0x10002000;

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = Func.Entry;
  Protected.EndAddr = Func.Entry + 0x10;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Func.Entry + 4;
  Call.addInput(MedVar::makeConst(MayThrowVA, 4));
  Protected.Ops.push_back(std::move(Call));
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = Func.Entry + 8;
  Protected.Ops.push_back(std::move(ProtectedReturn));

  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = Func.Entry + 0x20;
  Handler.EndAddr = Func.Entry + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = Func.Entry + 0x28;
  Handler.Ops.push_back(std::move(HandlerReturn));
  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x40};
  EH.Encoding = ExceptionEncoding::ARM32Unpacked;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  constexpr va_t PersonalityBodyVA = 0x10003000;
  EH.PersonalityVA = PersonalityBodyVA | 1u;
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Func.Entry, Func.Entry + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Func.Entry + 0x20;
  Scope.ContinuationVA = Scope.HandlerVA;
  EH.SEH->Scopes.push_back(Scope);
  const va_t PersonalityVA = EH.PersonalityVA;
  Func.ExceptionMetadata = std::move(EH);
  MedFunc Personality =
      makeAddressBackedPersonality(PersonalityBodyVA,
                                   "\01__C_specific_handler");

  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  auto Module = Emitter.emit({Func, Personality}, Context, "arm32_native_seh",
                             Arch::ARM, {{MayThrowVA, "may_throw"}}, nullptr,
                             BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectVerifierClean(*Module);

  llvm::Function *Lifted = Module->getFunction(Func.Name);
  ASSERT_NE(Lifted, nullptr);
  llvm::Function *Canonical = Module->getFunction("__C_specific_handler");
  ASSERT_NE(Canonical, nullptr);
  EXPECT_TRUE(Canonical->isDeclaration());
  EXPECT_TRUE(Canonical->getReturnType()->isIntegerTy(32));
  EXPECT_TRUE(Canonical->isVarArg());
  const std::string PersonalityBodyName =
      (kAutoFuncPrefix + llvm::utohexstr(PersonalityBodyVA)).str();
  llvm::Function *PersonalityBody = Module->getFunction(PersonalityBodyName);
  ASSERT_NE(PersonalityBody, nullptr);
  EXPECT_FALSE(PersonalityBody->isDeclaration());
  EXPECT_EQ(Module->getFunction(Personality.Name), nullptr);
  const llvm::MDNode *Native =
      Lifted->getMetadata(windows_eh_md::NativeAttachment);
  ASSERT_NE(Native, nullptr);
  ASSERT_EQ(Native->getNumOperands(), 2u);
  const auto *NativeKind =
      llvm::dyn_cast<llvm::MDString>(Native->getOperand(1).get());
  ASSERT_NE(NativeKind, nullptr);
  EXPECT_EQ(NativeKind->getString(), "seh-arm32-native");
  expectCompleteRewriteContract(*Lifted, 1);

  BinaryImage Image;
  Image.Arch = Arch::ARM;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x10000000;
  ASSERT_TRUE(Func.ExceptionMetadata.has_value());
  Image.ExceptionMetadata.Functions.push_back(*Func.ExceptionMetadata);
  Image.ExceptionMetadata.rebuildIndex();

  auto Plan = planCOFFExceptionPatch(*Module, Image, Arch::ARM);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());

  ensureCOFFCodegenTargets();
  CompiledImage Compiled = compileImageForPatch(
      *Module, Arch::ARM, BinaryFormat::COFF, 0x10004000,
      [&](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "may_throw")
          return MayThrowVA | 1u;
        if (Symbol == "__C_specific_handler")
          return PersonalityVA | 1u;
        return std::nullopt;
      },
      Image.Base);
  ASSERT_TRUE(Compiled.Success);
  EXPECT_TRUE(Compiled.Unresolved.empty());
  EXPECT_TRUE(Compiled.WinEHSemanticsValid);
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 1u);
  const CompiledWinEHSemanticRecord &Record =
      Compiled.WinEHSemanticRecords.front();
  EXPECT_EQ(Record.RecordSize, 4 * sizeof(uint32_t));

  auto ReadCompiledU32 = [&](va_t Address) -> std::optional<uint32_t> {
    for (const CompiledSection &Section : Compiled.Sections) {
      if (!Section.IsInImage || Address < Section.VA ||
          Address - Section.VA > Section.Size ||
          sizeof(uint32_t) > Section.Size - (Address - Section.VA) ||
          !rangeInBounds(Section.Offset, Section.Size, Compiled.Bytes.size()))
        continue;
      const size_t Offset = static_cast<size_t>(
          Section.Offset + (Address - Section.VA));
      return readLE<uint32_t>(Compiled.Bytes.data() + Offset);
    }
    return std::nullopt;
  };
  ASSERT_GE(Record.BeginVA, Image.Base);
  ASSERT_GE(Record.EndVA, Image.Base);
  ASSERT_GE(Record.HandlerVA, Image.Base);
  ASSERT_LE(Record.BeginVA - Image.Base,
            std::numeric_limits<uint32_t>::max());
  ASSERT_LE(Record.EndVA - Image.Base, std::numeric_limits<uint32_t>::max());
  ASSERT_LE(Record.HandlerVA - Image.Base,
            std::numeric_limits<uint32_t>::max());
  const uint32_t BeginRVA =
      static_cast<uint32_t>(Record.BeginVA - Image.Base);
  const uint32_t EndRVA = static_cast<uint32_t>(Record.EndVA - Image.Base);
  const uint32_t HandlerRVA =
      static_cast<uint32_t>(Record.HandlerVA - Image.Base);
  EXPECT_EQ(ReadCompiledU32(Record.RecordVA), BeginRVA | 1u);
  EXPECT_EQ(ReadCompiledU32(Record.RecordVA + 4), EndRVA | 1u);
  EXPECT_EQ(ReadCompiledU32(Record.RecordVA + 8), 1u);
  EXPECT_EQ(ReadCompiledU32(Record.RecordVA + 12), HandlerRVA | 1u);
}

TEST(COFFExceptionIR,
     RestoresARM32TaggedPersonalityNameWhenNativeLoweringPreflightFails) {
  constexpr va_t FunctionVA = 0x10005000;
  constexpr va_t MissingHandlerVA = FunctionVA + 0x20;
  constexpr va_t PersonalityBodyVA = 0x10006000;

  MedFunc Function;
  Function.Entry = FunctionVA;
  Function.Name = "arm32_missing_handler_block";
  Function.ReturnType = NdType::makeVoid();
  MedBlock Body;
  Body.Id = 0;
  Body.StartAddr = FunctionVA;
  Body.EndAddr = FunctionVA + 0x10;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = FunctionVA;
  Body.Ops.push_back(std::move(Return));
  Function.Blocks.push_back(std::move(Body));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
  EH.Encoding = ExceptionEncoding::ARM32Unpacked;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = PersonalityBodyVA | 1u;
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.ParseStatus = ExceptionParseStatus::Complete;
  Scope.GuardedRange = {FunctionVA, FunctionVA + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = MissingHandlerVA;
  Scope.ContinuationVA = MissingHandlerVA;
  EH.SEH->Scopes.push_back(Scope);
  Function.ExceptionMetadata = EH;

  const WindowsEHNativeSourceClassification Source =
      classifyWindowsEHNativeSource(EH, Arch::ARM, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::IRLowering);
  ASSERT_TRUE(Source.canLowerNativeIR());

  MedFunc Personality = makeAddressBackedPersonality(PersonalityBodyVA,
                                                     "\01__C_specific_handler");
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Function, Personality}, Context, "arm32_atomic_personality_restore",
      Arch::ARM, {}, nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectVerifierClean(*Module);

  llvm::Function *PersonalityBody = Module->getFunction(Personality.Name);
  ASSERT_NE(PersonalityBody, nullptr);
  EXPECT_FALSE(PersonalityBody->isDeclaration());
  EXPECT_EQ(Module->getFunction(
                (kAutoFuncPrefix + llvm::utohexstr(PersonalityBodyVA)).str()),
            nullptr);
  EXPECT_EQ(Module->getFunction("__C_specific_handler"), nullptr);

  llvm::Function *Rejected = Module->getFunction(Function.Name);
  ASSERT_NE(Rejected, nullptr);
  EXPECT_FALSE(Rejected->hasPersonalityFn());
  EXPECT_EQ(Rejected->getMetadata(windows_eh_md::NativeAttachment), nullptr);
}

TEST(COFFExceptionIR,
     KeepsAArch64RejectedPersonalityBodyUnderCanonicalSourceName) {
  constexpr va_t FunctionVA = 0x140011000;
  constexpr va_t PersonalityVA = 0x140012000;

  MedFunc Function;
  Function.Entry = FunctionVA;
  Function.Name = "aarch64_rejected_seh";
  Function.ReturnType = NdType::makeVoid();
  MedBlock Body;
  Body.Id = 0;
  Body.StartAddr = FunctionVA;
  Body.EndAddr = FunctionVA + 1;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = FunctionVA;
  Body.Ops.push_back(std::move(Return));
  Function.Blocks.push_back(std::move(Body));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 1};
  EH.Encoding = ExceptionEncoding::ARM64Packed;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = PersonalityVA;
  Function.ExceptionMetadata = EH;

  const WindowsEHNativeSourceClassification Source =
      classifyWindowsEHNativeSource(EH, Arch::AArch64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::IRLowering);
  ASSERT_FALSE(Source.canLowerNativeIR());
  ASSERT_EQ(Source.Reason,
            WindowsEHNativeSourceReason::UnsupportedUnwindEncoding);

  MedFunc Personality =
      makeAddressBackedPersonality(PersonalityVA, "\01__C_specific_handler");
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Function, Personality}, Context, "aarch64_rejected_personality",
      Arch::AArch64, {}, nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectVerifierClean(*Module);

  llvm::Function *BodyFunction = Module->getFunction(Personality.Name);
  ASSERT_NE(BodyFunction, nullptr);
  EXPECT_FALSE(BodyFunction->isDeclaration());
  EXPECT_EQ(Module->getFunction(
                (kAutoFuncPrefix + llvm::utohexstr(PersonalityVA)).str()),
            nullptr);

  llvm::Function *Rejected = Module->getFunction(Function.Name);
  ASSERT_NE(Rejected, nullptr);
  EXPECT_FALSE(Rejected->hasPersonalityFn());
  EXPECT_EQ(Rejected->getMetadata(windows_eh_md::NativeAttachment), nullptr);
}

TEST(COFFExceptionIR,
     RestoresAArch64PersonalityNameWhenNativeLoweringPreflightFails) {
  constexpr va_t FunctionVA = 0x140021000;
  constexpr va_t MissingHandlerVA = FunctionVA + 0x20;
  constexpr va_t PersonalityVA = 0x140022000;

  MedFunc Function;
  Function.Entry = FunctionVA;
  Function.Name = "aarch64_missing_handler_block";
  Function.ReturnType = NdType::makeVoid();
  MedBlock Body;
  Body.Id = 0;
  Body.StartAddr = FunctionVA;
  Body.EndAddr = FunctionVA + 0x10;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = FunctionVA;
  Body.Ops.push_back(std::move(Return));
  Function.Blocks.push_back(std::move(Body));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
  EH.Encoding = ExceptionEncoding::ARM64Unpacked;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = PersonalityVA;
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.ParseStatus = ExceptionParseStatus::Complete;
  Scope.GuardedRange = {FunctionVA, FunctionVA + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = MissingHandlerVA;
  Scope.ContinuationVA = MissingHandlerVA;
  EH.SEH->Scopes.push_back(Scope);
  Function.ExceptionMetadata = EH;

  const WindowsEHNativeSourceClassification Source =
      classifyWindowsEHNativeSource(EH, Arch::AArch64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::IRLowering);
  ASSERT_TRUE(Source.canLowerNativeIR());

  MedFunc Personality =
      makeAddressBackedPersonality(PersonalityVA, "\01__C_specific_handler");
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Function, Personality}, Context, "aarch64_atomic_personality_restore",
      Arch::AArch64, {}, nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectVerifierClean(*Module);

  llvm::Function *PersonalityBody = Module->getFunction(Personality.Name);
  ASSERT_NE(PersonalityBody, nullptr);
  EXPECT_FALSE(PersonalityBody->isDeclaration());
  EXPECT_EQ(Module->getFunction(
                (kAutoFuncPrefix + llvm::utohexstr(PersonalityVA)).str()),
            nullptr);
  EXPECT_EQ(Module->getFunction("__C_specific_handler"), nullptr);

  llvm::Function *Rejected = Module->getFunction(Function.Name);
  ASSERT_NE(Rejected, nullptr);
  EXPECT_FALSE(Rejected->hasPersonalityFn());
  EXPECT_EQ(Rejected->getMetadata(windows_eh_md::NativeAttachment), nullptr);
}

TEST(COFFExceptionIR,
     KeepsAutoPersonalityNameWhenCanonicalDeclarationHasOrdinaryUse) {
  constexpr va_t FunctionVA = 0x140031000;
  constexpr va_t MissingHandlerVA = FunctionVA + 0x20;
  constexpr va_t PersonalityVA = 0x140032000;
  constexpr va_t CallerVA = 0x140033000;
  constexpr va_t ImportVA = 0x140034000;

  MedFunc Function;
  Function.Entry = FunctionVA;
  Function.Name = "aarch64_missing_handler_with_canonical_use";
  Function.ReturnType = NdType::makeVoid();
  MedBlock Body;
  Body.Id = 0;
  Body.StartAddr = FunctionVA;
  Body.EndAddr = FunctionVA + 0x10;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = FunctionVA;
  Body.Ops.push_back(std::move(Return));
  Function.Blocks.push_back(std::move(Body));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
  EH.Encoding = ExceptionEncoding::ARM64Unpacked;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = PersonalityVA;
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.ParseStatus = ExceptionParseStatus::Complete;
  Scope.GuardedRange = {FunctionVA, FunctionVA + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = MissingHandlerVA;
  Scope.ContinuationVA = MissingHandlerVA;
  EH.SEH->Scopes.push_back(Scope);
  Function.ExceptionMetadata = EH;

  MedFunc Caller;
  Caller.Entry = CallerVA;
  Caller.Name = "ordinary_canonical_personality_caller";
  Caller.ReturnType = NdType::makeVoid();
  MedBlock CallerBody;
  CallerBody.Id = 0;
  CallerBody.StartAddr = CallerVA;
  CallerBody.EndAddr = CallerVA + 2;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = CallerVA;
  Call.addInput(MedVar::makeConst(ImportVA, 8));
  CallerBody.Ops.push_back(std::move(Call));
  MedOp CallerReturn;
  CallerReturn.Opcode = NdOp::RETURN;
  CallerReturn.Addr = CallerVA + 1;
  CallerBody.Ops.push_back(std::move(CallerReturn));
  Caller.Blocks.push_back(std::move(CallerBody));

  MedFunc Personality =
      makeAddressBackedPersonality(PersonalityVA, "\01__C_specific_handler");
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Function, Personality, Caller}, Context,
      "aarch64_personality_ordinary_use", Arch::AArch64,
      {{ImportVA, "__C_specific_handler"}}, nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectVerifierClean(*Module);

  llvm::Function *Canonical = Module->getFunction("__C_specific_handler");
  ASSERT_NE(Canonical, nullptr);
  EXPECT_TRUE(Canonical->isDeclaration());
  EXPECT_FALSE(Canonical->use_empty());

  llvm::Function *PersonalityBody = Module->getFunction(
      (kAutoFuncPrefix + llvm::utohexstr(PersonalityVA)).str());
  ASSERT_NE(PersonalityBody, nullptr);
  EXPECT_FALSE(PersonalityBody->isDeclaration());
  EXPECT_EQ(Module->getFunction(Personality.Name), nullptr);

  llvm::Function *Rejected = Module->getFunction(Function.Name);
  ASSERT_NE(Rejected, nullptr);
  EXPECT_FALSE(Rejected->hasPersonalityFn());
  EXPECT_EQ(Rejected->getMetadata(windows_eh_md::NativeAttachment), nullptr);
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
  constexpr va_t CallbackVA = 0x140001200;
  for (SEHScopeKind ScopeKind : {SEHScopeKind::Filter, SEHScopeKind::Finally}) {
    for (const ConflictCase &Case :
         {ConflictCase{CallbackConflict::Type, "function type"},
          ConflictCase{CallbackConflict::Linkage, "linkage"},
          ConflictCase{CallbackConflict::CallingConvention,
                       "calling convention"}}) {
      SCOPED_TRACE("x64");
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
                                      *Fixture.Function, Fixture.Source,
                                      Arch::X64);
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

TEST(COFFExceptionIR,
     RejectsUnpreservableSEHCallbackAttributesWithoutMutation) {
  struct ConflictCase {
    llvm::Attribute::AttrKind Attribute;
    const char *Name;
  };
  constexpr va_t CallbackVA = 0x140001200;
  for (const ConflictCase &Case :
       {ConflictCase{llvm::Attribute::AlwaysInline, "alwaysinline"},
        ConflictCase{llvm::Attribute::NoReturn, "noreturn"},
        ConflictCase{llvm::Attribute::ReturnsTwice, "returns_twice"},
        ConflictCase{llvm::Attribute::Speculatable, "speculatable"},
        ConflictCase{llvm::Attribute::OptimizeForSize, "optsize"},
        ConflictCase{llvm::Attribute::MinSize, "minsize"},
        ConflictCase{llvm::Attribute::OptimizeForDebugging, "optdebug"}}) {
    SCOPED_TRACE(Case.Name);
    DirectSEHFixture Fixture("atomic_seh_callback_attribute_conflict",
                             /*TerminateProtectedBlock=*/true);
    SEHScopeRecord &Scope =
        Fixture.Source.ExceptionMetadata->SEH->Scopes.front();
    Scope.Kind = SEHScopeKind::Finally;
    Scope.FilterOrFinallyVA = CallbackVA;
    Scope.HandlerVA = CallbackVA;
    Scope.ContinuationVA = 0;

    llvm::Function *Callback = llvm::Function::Create(
        sehCallbackType(Fixture.Context, SEHScopeKind::Finally),
        llvm::GlobalValue::ExternalLinkage, "seh_finally_callback",
        Fixture.Module);
    llvm::IRBuilder<>(
        llvm::BasicBlock::Create(Fixture.Context, "entry", Callback))
        .CreateRetVoid();
    Callback->addFnAttr(Case.Attribute);

    MedLLVMEmitter Emitter;
    MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                    *Fixture.Function, Fixture.Source,
                                    Arch::X64);
    MedLLVMEmitterTestPeer::setFunctionName(Emitter, CallbackVA,
                                            Callback->getName());
    MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                               Fixture.Source.Entry + 4);
    expectVerifierClean(Fixture.Module);
    const std::string BeforeIR = printModuleIR(Fixture.Module);
    const auto BeforeCallSites =
        MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

    EXPECT_FALSE(MedLLVMEmitterTestPeer::emitSEH(
        Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap));
    EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
    EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
              BeforeCallSites);
    expectVerifierClean(Fixture.Module);
  }

  for (llvm::Attribute::AttrKind ReturnAttribute :
       {llvm::Attribute::WillReturn, llvm::Attribute::MustProgress}) {
    SCOPED_TRACE(ReturnAttribute == llvm::Attribute::WillReturn
                     ? "explicit willreturn"
                     : "mustprogress-implied willreturn");
    for (bool IsDefinition : {false, true}) {
      SCOPED_TRACE(IsDefinition ? "pure definition" : "pure declaration");
    DirectSEHFixture Fixture("atomic_seh_pure_callback",
                             /*TerminateProtectedBlock=*/true);
    SEHScopeRecord &Scope =
        Fixture.Source.ExceptionMetadata->SEH->Scopes.front();
    Scope.Kind = SEHScopeKind::Finally;
    Scope.FilterOrFinallyVA = CallbackVA;
    Scope.HandlerVA = CallbackVA;
    Scope.ContinuationVA = 0;

    llvm::Function *Callback = llvm::Function::Create(
        sehCallbackType(Fixture.Context, SEHScopeKind::Finally),
        llvm::GlobalValue::ExternalLinkage, "pure_seh_finally_callback",
        Fixture.Module);
    if (IsDefinition)
      llvm::IRBuilder<>(
          llvm::BasicBlock::Create(Fixture.Context, "entry", Callback))
          .CreateRetVoid();
    Callback->addFnAttr(llvm::Attribute::NoUnwind);
    Callback->addFnAttr(ReturnAttribute);
    Callback->setDoesNotAccessMemory();

    MedLLVMEmitter Emitter;
    MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                    *Fixture.Function, Fixture.Source,
                                    Arch::X64);
    MedLLVMEmitterTestPeer::setFunctionName(Emitter, CallbackVA,
                                            Callback->getName());
    MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                               Fixture.Source.Entry + 4);
    expectVerifierClean(Fixture.Module);
    const std::string BeforeIR = printModuleIR(Fixture.Module);
    const auto BeforeCallSites =
        MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

    EXPECT_FALSE(MedLLVMEmitterTestPeer::emitSEH(
        Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap));
    EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
    EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
              BeforeCallSites);
    expectVerifierClean(Fixture.Module);
    }
  }
}

TEST(COFFExceptionIR, RejectsAArch64SEHCallbacksUntilLiftedABIIsProven) {
  constexpr va_t CallbackVA = 0x140001200;
  for (SEHScopeKind ScopeKind : {SEHScopeKind::Filter, SEHScopeKind::Finally}) {
    SCOPED_TRACE(ScopeKind == SEHScopeKind::Filter ? "filter" : "finally");
    DirectSEHFixture Fixture("aarch64_seh_callback_abi",
                             /*TerminateProtectedBlock=*/true);
    Fixture.Module.setTargetTriple(llvm::Triple("aarch64-pc-windows-msvc"));
    Fixture.Source.ExceptionMetadata->Encoding =
        ExceptionEncoding::ARM64Unpacked;
    SEHScopeRecord &Scope =
        Fixture.Source.ExceptionMetadata->SEH->Scopes.front();
    Scope.Kind = ScopeKind;
    Scope.FilterOrFinallyVA = CallbackVA;
    if (ScopeKind == SEHScopeKind::Finally) {
      Scope.HandlerVA = CallbackVA;
      Scope.ContinuationVA = 0;
    }

    llvm::Function *Callback = llvm::Function::Create(
        sehCallbackType(Fixture.Context, ScopeKind),
        llvm::GlobalValue::ExternalLinkage, "seh_callback", Fixture.Module);
    MedLLVMEmitter Emitter;
    MedLLVMEmitterTestPeer::prepare(Emitter, Fixture.Context, Fixture.Module,
                                    *Fixture.Function, Fixture.Source,
                                    Arch::AArch64);
    MedLLVMEmitterTestPeer::setFunctionName(Emitter, CallbackVA,
                                            Callback->getName());
    MedLLVMEmitterTestPeer::setCallSiteAddress(Emitter, *Fixture.Call,
                                               Fixture.Source.Entry + 4);
    expectVerifierClean(Fixture.Module);
    const std::string BeforeIR = printModuleIR(Fixture.Module);
    const auto BeforeCallSites =
        MedLLVMEmitterTestPeer::callSiteAddresses(Emitter);

    EXPECT_FALSE(MedLLVMEmitterTestPeer::emitSEH(
        Emitter, Fixture.Source, *Fixture.Function, Fixture.OriginalBlockMap));
    EXPECT_EQ(printModuleIR(Fixture.Module), BeforeIR);
    EXPECT_EQ(MedLLVMEmitterTestPeer::callSiteAddresses(Emitter),
              BeforeCallSites);
    expectVerifierClean(Fixture.Module);

    const WindowsEHNativeSourceClassification Classification =
        classifyWindowsEHNativeSource(*Fixture.Source.ExceptionMetadata,
                                      Arch::AArch64, BinaryFormat::COFF,
                                      WindowsEHNativeCapability::IRLowering);
    EXPECT_FALSE(Classification.canLowerNativeIR());
    EXPECT_EQ(Classification.Reason,
              WindowsEHNativeSourceReason::UnsupportedSEHCallbackABI);
  }
}

TEST(COFFExceptionIR, RejectsRealLiftedAArch64FilterWithWidenedReturnABI) {
  constexpr va_t ParentVA = 0x140001000;
  constexpr va_t FilterVA = 0x140002000;
  constexpr va_t MayThrowVA = 0x140003000;
  constexpr va_t PersonalityVA = 0x140004000;

  MedFunc Parent;
  Parent.Entry = ParentVA;
  Parent.Name = "aarch64_filter_parent";
  Parent.ReturnType = NdType::makeVoid();
  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = ParentVA;
  Protected.EndAddr = ParentVA + 0x10;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = ParentVA + 4;
  Call.addInput(MedVar::makeConst(MayThrowVA, 8));
  Protected.Ops.push_back(std::move(Call));
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = ParentVA + 8;
  Protected.Ops.push_back(std::move(ProtectedReturn));
  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = ParentVA + 0x20;
  Handler.EndAddr = ParentVA + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = ParentVA + 0x28;
  Handler.Ops.push_back(std::move(HandlerReturn));
  Parent.Blocks.push_back(std::move(Protected));
  Parent.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {ParentVA, ParentVA + 0x40};
  EH.Encoding = ExceptionEncoding::ARM64Unpacked;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = PersonalityVA;
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.ParseStatus = ExceptionParseStatus::Complete;
  Scope.GuardedRange = {ParentVA, ParentVA + 0x10};
  Scope.Kind = SEHScopeKind::Filter;
  Scope.FilterOrFinallyVA = FilterVA;
  Scope.HandlerVA = ParentVA + 0x20;
  Scope.ContinuationVA = Scope.HandlerVA;
  EH.SEH->Scopes.push_back(Scope);
  Parent.ExceptionMetadata = EH;

  MedFunc Filter;
  Filter.Entry = FilterVA;
  Filter.Name = "lifted_aarch64_filter";
  Filter.ReturnType = NdType::makeInt(4, /*Signed=*/true);
  for (unsigned I = 0; I < 2; ++I) {
    MedVar Param;
    Param.Kind = MedVar::Param;
    Param.TheArch = Arch::AArch64;
    Param.Id = static_cast<int>(I);
    Param.Size = 8;
    Param.RegOff = I == 0 ? a64reg::X0 : a64reg::X1;
    Filter.Params.push_back(Param);
    Filter.TypedParams.push_back(
        {I == 0 ? "exception_pointers" : "establisher_frame",
         NdType::makePtr()});
  }
  MedBlock FilterBody;
  FilterBody.Id = 0;
  FilterBody.StartAddr = FilterVA;
  FilterBody.EndAddr = FilterVA + 4;
  MedOp FilterReturn;
  FilterReturn.Opcode = NdOp::RETURN;
  FilterReturn.Addr = FilterVA;
  FilterReturn.addInput(MedVar::makeConst(1, 4));
  FilterBody.Ops.push_back(std::move(FilterReturn));
  Filter.Blocks.push_back(std::move(FilterBody));

  MedFunc Personality =
      makeAddressBackedPersonality(PersonalityVA, "\01__C_specific_handler");
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Parent, Filter, Personality}, Context, "aarch64_filter_fail_closed",
      Arch::AArch64, {{MayThrowVA, "may_throw"}}, nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectVerifierClean(*Module);

  llvm::Function *LiftedFilter = Module->getFunction(Filter.Name);
  ASSERT_NE(LiftedFilter, nullptr);
  EXPECT_TRUE(LiftedFilter->getReturnType()->isIntegerTy(64));
  ASSERT_EQ(LiftedFilter->arg_size(), 2u);
  EXPECT_TRUE(LiftedFilter->getFunctionType()->getParamType(0)->isPointerTy());
  EXPECT_TRUE(LiftedFilter->getFunctionType()->getParamType(1)->isPointerTy());

  llvm::Function *LiftedParent = Module->getFunction(Parent.Name);
  ASSERT_NE(LiftedParent, nullptr);
  EXPECT_FALSE(LiftedParent->hasPersonalityFn());
  EXPECT_EQ(LiftedParent->getMetadata(windows_eh_md::NativeAttachment),
            nullptr);
  const llvm::MDNode *Contract =
      LiftedParent->getMetadata(exception_rewrite::FunctionAttachment);
  ASSERT_NE(Contract, nullptr);
  EXPECT_EQ(metadataInteger(Contract, exception_rewrite::Lowering, 8),
            static_cast<uint8_t>(exception_rewrite::LoweringState::Missing));

  const WindowsEHNativeSourceClassification Classification =
      classifyWindowsEHNativeSource(EH, Arch::AArch64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::IRLowering);
  EXPECT_FALSE(Classification.canLowerNativeIR());
  EXPECT_EQ(Classification.Reason,
            WindowsEHNativeSourceReason::UnsupportedSEHCallbackABI);
}

TEST(COFFExceptionIR, PreservesNativeFilterCallbackThroughO3AndCodegen) {
  constexpr va_t ParentVA = 0x140091000;
  constexpr va_t HandlerVA = ParentVA + 0x20;
  constexpr va_t FilterVA = 0x140092000;
  constexpr va_t MayThrowVA = 0x140093000;
  constexpr va_t PersonalityVA = 0x140094000;

  MedFunc Parent;
  Parent.Entry = ParentVA;
  Parent.Name = "native_seh_filter_parent";
  Parent.ReturnType = NdType::makeVoid();

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = ParentVA;
  Protected.EndAddr = ParentVA + 0x10;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = ParentVA + 4;
  Call.addInput(MedVar::makeConst(MayThrowVA, 8));
  Protected.Ops.push_back(std::move(Call));
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = ParentVA + 8;
  Protected.Ops.push_back(std::move(ProtectedReturn));

  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = HandlerVA;
  Handler.EndAddr = HandlerVA + 0x10;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = HandlerVA;
  Handler.Ops.push_back(std::move(HandlerReturn));
  Parent.Blocks.push_back(std::move(Protected));
  Parent.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {ParentVA, ParentVA + 0x40};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = PersonalityVA;
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.ParseStatus = ExceptionParseStatus::Complete;
  Scope.GuardedRange = {ParentVA, ParentVA + 0x10};
  Scope.Kind = SEHScopeKind::Filter;
  Scope.FilterOrFinallyVA = FilterVA;
  Scope.HandlerVA = HandlerVA;
  Scope.ContinuationVA = HandlerVA;
  EH.SEH->Scopes.push_back(Scope);
  Parent.ExceptionMetadata = EH;

  MedFunc Filter;
  Filter.Entry = FilterVA;
  Filter.Name = "native_seh_filter_callback";
  Filter.ReturnType = NdType::makeInt(4, /*Signed=*/true);
  for (unsigned I = 0; I < 2; ++I) {
    MedVar Param;
    Param.Kind = MedVar::Param;
    Param.TheArch = Arch::X64;
    Param.Id = static_cast<int>(I);
    Param.Size = 8;
    Param.RegOff = I == 0 ? x86reg::RCX : x86reg::RDX;
    Filter.Params.push_back(Param);
    Filter.TypedParams.push_back(
        {I == 0 ? "exception_pointers" : "establisher_frame",
         NdType::makePtr()});
  }
  MedBlock FilterBody;
  FilterBody.Id = 0;
  FilterBody.StartAddr = FilterVA;
  FilterBody.EndAddr = FilterVA + 1;
  MedOp FilterReturn;
  FilterReturn.Opcode = NdOp::RETURN;
  FilterReturn.Addr = FilterVA;
  FilterReturn.addInput(MedVar::makeConst(1, 4));
  FilterBody.Ops.push_back(std::move(FilterReturn));
  Filter.Blocks.push_back(std::move(FilterBody));

  MedFunc Personality =
      makeAddressBackedPersonality(PersonalityVA, "\01__C_specific_handler");
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Parent, Filter, Personality}, Context, "native_seh_filter", Arch::X64,
      {{MayThrowVA, "may_throw"}}, nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectVerifierClean(*Module);

  llvm::Function *ParentFunction = Module->getFunction(Parent.Name);
  llvm::Function *FilterFunction = Module->getFunction(Filter.Name);
  ASSERT_NE(ParentFunction, nullptr);
  ASSERT_NE(FilterFunction, nullptr);
  ASSERT_TRUE(ParentFunction->hasPersonalityFn());
  EXPECT_TRUE(FilterFunction->getReturnType()->isIntegerTy(32));
  ASSERT_EQ(FilterFunction->arg_size(), 2u);
  EXPECT_TRUE(FilterFunction->hasFnAttribute(llvm::Attribute::NoInline));
  EXPECT_TRUE(FilterFunction->hasFnAttribute(llvm::Attribute::OptimizeNone));
  expectCompleteRewriteContract(*ParentFunction, /*ProtectedCalls=*/1);

  llvm::CatchPadInst *FilterPad = nullptr;
  for (llvm::BasicBlock &Block : *ParentFunction)
    for (llvm::Instruction &Instruction : Block)
      if (auto *Pad = llvm::dyn_cast<llvm::CatchPadInst>(&Instruction)) {
        ASSERT_EQ(FilterPad, nullptr);
        FilterPad = Pad;
      }
  ASSERT_NE(FilterPad, nullptr);
  ASSERT_EQ(FilterPad->arg_size(), 1u);
  EXPECT_EQ(FilterPad->getArgOperand(0)->stripPointerCasts(), FilterFunction);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();
  auto InitialPlan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(InitialPlan))
      << llvm::toString(InitialPlan.takeError());

  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Deep;
  Options.LLVMLevel = llvm::OptimizationLevel::O3;
  OptimizationResult Optimization = Pipeline::optimizeModule(*Module, Options);
  EXPECT_NE(Optimization.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_NE(Optimization.Stop, OptimizationStopReason::VerificationFailed);
  expectVerifierClean(*Module);

  FilterFunction = Module->getFunction(Filter.Name);
  ASSERT_NE(FilterFunction, nullptr);
  EXPECT_TRUE(FilterFunction->hasFnAttribute(llvm::Attribute::NoInline));
  EXPECT_TRUE(FilterFunction->hasFnAttribute(llvm::Attribute::OptimizeNone));
  auto OptimizedPlan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(OptimizedPlan))
      << llvm::toString(OptimizedPlan.takeError()) << '\n'
      << printModuleIR(*Module);

  ensureCOFFCodegenTargets();
  CompiledImage Compiled = compileImageForPatch(
      *Module, Arch::X64, BinaryFormat::COFF, 0x140098000,
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

TEST(COFFExceptionIR, EmitsNestedFinallyCallbackInvokeToExactParentDispatch) {
  constexpr va_t ParentVA = 0x140101000;
  constexpr va_t TailVA = ParentVA + 8;
  constexpr va_t HandlerVA = ParentVA + 0x20;
  constexpr va_t FinallyVA = 0x140102000;
  constexpr va_t FinallyMayThrowVA = 0x140102100;
  constexpr va_t MayThrowVA = 0x140103000;
  constexpr va_t PersonalityVA = 0x140104000;

  MedFunc Parent;
  Parent.Entry = ParentVA;
  Parent.Name = "nested_seh_parent";
  Parent.ReturnType = NdType::makeVoid();

  MedBlock Inner;
  Inner.Id = 0;
  Inner.StartAddr = ParentVA;
  Inner.EndAddr = TailVA;
  Inner.Succs = {1};
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = ParentVA + 2;
  Call.addInput(MedVar::makeConst(MayThrowVA, 8));
  Inner.Ops.push_back(std::move(Call));
  MedOp Branch;
  Branch.Opcode = NdOp::BRANCH;
  Branch.Addr = ParentVA + 4;
  Branch.addInput(MedVar::makeConst(TailVA, 8));
  Inner.Ops.push_back(std::move(Branch));

  MedBlock Tail;
  Tail.Id = 1;
  Tail.StartAddr = TailVA;
  Tail.EndAddr = ParentVA + 0x10;
  Tail.Preds = {0};
  MedOp TailReturn;
  TailReturn.Opcode = NdOp::RETURN;
  TailReturn.Addr = TailVA;
  Tail.Ops.push_back(std::move(TailReturn));

  MedBlock Handler;
  Handler.Id = 2;
  Handler.StartAddr = HandlerVA;
  Handler.EndAddr = HandlerVA + 0x10;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = HandlerVA;
  Handler.Ops.push_back(std::move(HandlerReturn));
  Parent.Blocks.push_back(std::move(Inner));
  Parent.Blocks.push_back(std::move(Tail));
  Parent.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {ParentVA, ParentVA + 0x40};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = PersonalityVA;
  EH.SEH.emplace();
  SEHScopeRecord Outer;
  Outer.ParseStatus = ExceptionParseStatus::Complete;
  Outer.GuardedRange = {ParentVA, ParentVA + 0x10};
  Outer.Kind = SEHScopeKind::CatchAll;
  Outer.HandlerVA = HandlerVA;
  Outer.ContinuationVA = HandlerVA;
  SEHScopeRecord InnerFinally;
  InnerFinally.ParseStatus = ExceptionParseStatus::Complete;
  InnerFinally.GuardedRange = {ParentVA, TailVA};
  InnerFinally.Kind = SEHScopeKind::Finally;
  InnerFinally.FilterOrFinallyVA = FinallyVA;
  InnerFinally.HandlerVA = FinallyVA;
  EH.SEH->Scopes.push_back(InnerFinally);
  EH.SEH->Scopes.push_back(Outer);
  Parent.ExceptionMetadata = EH;

  MedFunc Finally;
  Finally.Entry = FinallyVA;
  Finally.Name = "nested_finally_callback";
  Finally.ReturnType = NdType::makeVoid();
  MedVar Abnormal;
  Abnormal.Kind = MedVar::Param;
  Abnormal.TheArch = Arch::X64;
  Abnormal.Id = 0;
  Abnormal.Size = 1;
  Abnormal.RegOff = x86reg::RCX;
  Finally.Params.push_back(Abnormal);
  Finally.TypedParams.push_back(
      {"abnormal_termination", NdType::makeInt(1, /*Signed=*/false)});
  MedVar Frame;
  Frame.Kind = MedVar::Param;
  Frame.TheArch = Arch::X64;
  Frame.Id = 1;
  Frame.Size = 8;
  Frame.RegOff = x86reg::RDX;
  Finally.Params.push_back(Frame);
  Finally.TypedParams.push_back({"establisher_frame", NdType::makePtr()});
  MedBlock FinallyBody;
  FinallyBody.Id = 0;
  FinallyBody.StartAddr = FinallyVA;
  FinallyBody.EndAddr = FinallyVA + 2;
  MedOp FinallyCall;
  FinallyCall.Opcode = NdOp::CALL;
  FinallyCall.Addr = FinallyVA;
  FinallyCall.addInput(MedVar::makeConst(FinallyMayThrowVA, 8));
  FinallyBody.Ops.push_back(std::move(FinallyCall));
  MedOp FinallyReturn;
  FinallyReturn.Opcode = NdOp::RETURN;
  FinallyReturn.Addr = FinallyVA + 1;
  FinallyBody.Ops.push_back(std::move(FinallyReturn));
  Finally.Blocks.push_back(std::move(FinallyBody));

  MedFunc Personality =
      makeAddressBackedPersonality(PersonalityVA, "\01__C_specific_handler");
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Parent, Finally, Personality}, Context, "nested_seh_finally", Arch::X64,
      {{MayThrowVA, "may_throw"}, {FinallyMayThrowVA, "finally_may_throw"}},
      nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectVerifierClean(*Module);

  llvm::Function *ParentFunction = Module->getFunction(Parent.Name);
  ASSERT_NE(ParentFunction, nullptr);
  expectCompleteRewriteContract(*ParentFunction, /*ProtectedCalls=*/1);
  llvm::InvokeInst *FinallyInvoke = nullptr;
  for (llvm::BasicBlock &Block : *ParentFunction)
    for (llvm::Instruction &Instruction : Block) {
      auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(&Instruction);
      const llvm::Function *Callee =
          Invoke ? Invoke->getCalledFunction() : nullptr;
      if (Callee && Callee->getName() == Finally.Name) {
        ASSERT_EQ(FinallyInvoke, nullptr);
        FinallyInvoke = Invoke;
      }
    }
  ASSERT_NE(FinallyInvoke, nullptr);
  ASSERT_NE(FinallyInvoke->getUnwindDest(), nullptr);
  EXPECT_NE(llvm::dyn_cast<llvm::CatchSwitchInst>(
                FinallyInvoke->getUnwindDest()->getTerminator()),
            nullptr);
  const auto *FinallyReturnInst = llvm::dyn_cast<llvm::CleanupReturnInst>(
      FinallyInvoke->getNormalDest()->getTerminator());
  ASSERT_NE(FinallyReturnInst, nullptr);
  EXPECT_EQ(FinallyReturnInst->getUnwindDest(), FinallyInvoke->getUnwindDest());

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();
  auto Plan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());

  auto FindFinallyCallback = [&](llvm::Module &Candidate) -> llvm::CallBase * {
    llvm::Function *CandidateParent = Candidate.getFunction(Parent.Name);
    if (!CandidateParent)
      return nullptr;
    llvm::CallBase *Found = nullptr;
    for (llvm::BasicBlock &Block : *CandidateParent)
      for (llvm::Instruction &Instruction : Block) {
        auto *CallBase = llvm::dyn_cast<llvm::CallBase>(&Instruction);
        const llvm::Function *Callee =
            CallBase ? CallBase->getCalledFunction() : nullptr;
        if (!Callee || Callee->getName() != Finally.Name)
          continue;
        if (Found)
          return nullptr;
        Found = CallBase;
      }
    return Found;
  };
  auto ExpectRejected = [&](llvm::Module &Candidate, llvm::StringRef Message) {
    expectVerifierClean(Candidate);
    auto Rejected = planCOFFExceptionPatch(Candidate, Image, Arch::X64);
    ASSERT_FALSE(static_cast<bool>(Rejected));
    EXPECT_NE(llvm::toString(Rejected.takeError()).find(Message),
              std::string::npos);
  };

  std::unique_ptr<llvm::Module> MissingUnwind = llvm::CloneModule(*Module);
  auto *MissingUnwindInvoke = llvm::dyn_cast_or_null<llvm::InvokeInst>(
      FindFinallyCallback(*MissingUnwind));
  ASSERT_NE(MissingUnwindInvoke, nullptr);
  llvm::IRBuilder<> MissingUnwindBuilder(MissingUnwindInvoke);
  llvm::SmallVector<llvm::Value *, 2> MissingUnwindArgs;
  for (llvm::Use &Arg : MissingUnwindInvoke->args())
    MissingUnwindArgs.push_back(Arg.get());
  llvm::SmallVector<llvm::OperandBundleDef, 1> MissingUnwindBundles;
  MissingUnwindInvoke->getOperandBundlesAsDefs(MissingUnwindBundles);
  llvm::CallInst *DowngradedCallback =
      MissingUnwindBuilder.CreateCall(MissingUnwindInvoke->getFunctionType(),
                                      MissingUnwindInvoke->getCalledOperand(),
                                      MissingUnwindArgs, MissingUnwindBundles);
  DowngradedCallback->setCallingConv(MissingUnwindInvoke->getCallingConv());
  DowngradedCallback->setAttributes(MissingUnwindInvoke->getAttributes());
  DowngradedCallback->setDebugLoc(MissingUnwindInvoke->getDebugLoc());
  DowngradedCallback->copyMetadata(*MissingUnwindInvoke);
  MissingUnwindBuilder.CreateBr(MissingUnwindInvoke->getNormalDest());
  MissingUnwindInvoke->replaceAllUsesWith(DowngradedCallback);
  MissingUnwindInvoke->eraseFromParent();
  ExpectRejected(*MissingUnwind,
                 "nested native SEH finally callback has an altered unwind "
                 "edge");

  std::unique_ptr<llvm::Module> RetargetedUnwind = llvm::CloneModule(*Module);
  auto *RetargetedUnwindInvoke = llvm::dyn_cast_or_null<llvm::InvokeInst>(
      FindFinallyCallback(*RetargetedUnwind));
  ASSERT_NE(RetargetedUnwindInvoke, nullptr);
  llvm::Function *RetargetedParent = RetargetedUnwind->getFunction(Parent.Name);
  ASSERT_NE(RetargetedParent, nullptr);
  llvm::BasicBlock *WrongUnwind =
      llvm::BasicBlock::Create(RetargetedUnwind->getContext(),
                               "tampered.finally.unwind", RetargetedParent);
  llvm::IRBuilder<> WrongUnwindBuilder(WrongUnwind);
  llvm::CleanupPadInst *WrongPad = WrongUnwindBuilder.CreateCleanupPad(
      llvm::ConstantTokenNone::get(RetargetedUnwind->getContext()), {});
  WrongUnwindBuilder.CreateCleanupRet(WrongPad, nullptr);
  auto *RetargetedCleanupReturn = llvm::dyn_cast<llvm::CleanupReturnInst>(
      RetargetedUnwindInvoke->getNormalDest()->getTerminator());
  ASSERT_NE(RetargetedCleanupReturn, nullptr);
  RetargetedUnwindInvoke->setUnwindDest(WrongUnwind);
  RetargetedCleanupReturn->setUnwindDest(WrongUnwind);
  ExpectRejected(*RetargetedUnwind,
                 "nested native SEH finally callback has an altered unwind "
                 "edge");

  std::unique_ptr<llvm::Module> SharedContinuation = llvm::CloneModule(*Module);
  auto *SharedContinuationInvoke = llvm::dyn_cast_or_null<llvm::InvokeInst>(
      FindFinallyCallback(*SharedContinuation));
  ASSERT_NE(SharedContinuationInvoke, nullptr);
  llvm::BasicBlock *Continuation = SharedContinuationInvoke->getNormalDest();
  ASSERT_NE(Continuation, nullptr);
  llvm::Instruction *ContinuationTerminator = Continuation->getTerminator();
  ASSERT_NE(ContinuationTerminator, nullptr);
  new llvm::FreezeInst(
      llvm::PoisonValue::get(llvm::Type::getInt1Ty(Module->getContext())),
      "tampered.finally.continuation", ContinuationTerminator->getIterator());
  ExpectRejected(*SharedContinuation,
                 "nested native SEH finally callback has an altered unwind "
                 "edge");

  std::unique_ptr<llvm::Module> MissingCleanupReturn =
      llvm::CloneModule(*Module);
  auto *MissingCleanupInvoke = llvm::dyn_cast_or_null<llvm::InvokeInst>(
      FindFinallyCallback(*MissingCleanupReturn));
  ASSERT_NE(MissingCleanupInvoke, nullptr);
  auto *OldCleanupReturn = llvm::dyn_cast<llvm::CleanupReturnInst>(
      MissingCleanupInvoke->getNormalDest()->getTerminator());
  ASSERT_NE(OldCleanupReturn, nullptr);
  llvm::IRBuilder<>(OldCleanupReturn).CreateUnreachable();
  OldCleanupReturn->eraseFromParent();
  ExpectRejected(*MissingCleanupReturn,
                 "native SEH finally cleanupret was altered");

  std::unique_ptr<llvm::Module> UsedPoisonArgument = llvm::CloneModule(*Module);
  llvm::CallBase *PoisonedCallback = FindFinallyCallback(*UsedPoisonArgument);
  ASSERT_NE(PoisonedCallback, nullptr);
  llvm::Function *PoisonedCallbackFunction =
      PoisonedCallback->getCalledFunction();
  ASSERT_NE(PoisonedCallbackFunction, nullptr);
  ASSERT_FALSE(PoisonedCallbackFunction->isDeclaration());
  ASSERT_EQ(PoisonedCallbackFunction->arg_size(), 2u);
  llvm::Argument *UsedArgument = PoisonedCallbackFunction->getArg(0);
  llvm::BasicBlock::iterator CallbackInsertionPoint =
      PoisonedCallbackFunction->getEntryBlock().getFirstInsertionPt();
  new llvm::FreezeInst(UsedArgument, "tampered.finally.argument",
                       CallbackInsertionPoint);
  PoisonedCallback->setArgOperand(
      0, llvm::PoisonValue::get(PoisonedCallback->getArgOperand(0)->getType()));
  ExpectRejected(*UsedPoisonArgument,
                 "native SEH finally callback ABI was altered");

  llvm::Function *FinallyFunction = Module->getFunction(Finally.Name);
  ASSERT_NE(FinallyFunction, nullptr);
  EXPECT_TRUE(FinallyFunction->hasFnAttribute(llvm::Attribute::NoInline));
  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Deep;
  Options.LLVMLevel = llvm::OptimizationLevel::O3;
  OptimizationResult Optimization = Pipeline::optimizeModule(*Module, Options);
  EXPECT_NE(Optimization.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_NE(Optimization.Stop, OptimizationStopReason::VerificationFailed);
  expectVerifierClean(*Module);
  auto OptimizedPlan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(OptimizedPlan))
      << llvm::toString(OptimizedPlan.takeError()) << '\n'
      << printModuleIR(*Module);

  ensureCOFFCodegenTargets();
  CompiledImage Compiled = compileImageForPatch(
      *Module, Arch::X64, BinaryFormat::COFF, 0x140110000,
      [&](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "may_throw")
          return MayThrowVA;
        if (Symbol == "finally_may_throw")
          return FinallyMayThrowVA;
        if (Symbol == "__C_specific_handler")
          return PersonalityVA;
        return std::nullopt;
      },
      Image.Base);
  ASSERT_TRUE(Compiled.Success);
  EXPECT_TRUE(Compiled.Unresolved.empty());
}

TEST(COFFExceptionIR, EmitsOuterFinallyCallbackWithoutSyntheticLocalUnwind) {
  constexpr va_t ParentVA = 0x140105000;
  constexpr va_t TailVA = ParentVA + 8;
  constexpr va_t FinallyVA = 0x140106000;
  constexpr va_t FinallyMayThrowVA = 0x140106100;
  constexpr va_t MayThrowVA = 0x140107000;
  constexpr va_t PersonalityVA = 0x140108000;

  MedFunc Parent;
  Parent.Entry = ParentVA;
  Parent.Name = "outer_seh_finally_parent";
  Parent.ReturnType = NdType::makeVoid();

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = ParentVA;
  Protected.EndAddr = TailVA;
  Protected.Succs = {1};
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = ParentVA + 2;
  Call.addInput(MedVar::makeConst(MayThrowVA, 8));
  Protected.Ops.push_back(std::move(Call));
  MedOp Branch;
  Branch.Opcode = NdOp::BRANCH;
  Branch.Addr = ParentVA + 4;
  Branch.addInput(MedVar::makeConst(TailVA, 8));
  Protected.Ops.push_back(std::move(Branch));

  MedBlock Tail;
  Tail.Id = 1;
  Tail.StartAddr = TailVA;
  Tail.EndAddr = ParentVA + 0x10;
  Tail.Preds = {0};
  MedOp TailReturn;
  TailReturn.Opcode = NdOp::RETURN;
  TailReturn.Addr = TailVA;
  Tail.Ops.push_back(std::move(TailReturn));
  Parent.Blocks.push_back(std::move(Protected));
  Parent.Blocks.push_back(std::move(Tail));

  ExceptionFunction EH;
  EH.CodeRange = {ParentVA, ParentVA + 0x20};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = PersonalityVA;
  EH.SEH.emplace();
  SEHScopeRecord FinallyScope;
  FinallyScope.ParseStatus = ExceptionParseStatus::Complete;
  FinallyScope.GuardedRange = {ParentVA, TailVA};
  FinallyScope.Kind = SEHScopeKind::Finally;
  FinallyScope.FilterOrFinallyVA = FinallyVA;
  FinallyScope.HandlerVA = FinallyVA;
  EH.SEH->Scopes.push_back(FinallyScope);
  Parent.ExceptionMetadata = EH;

  MedFunc Finally;
  Finally.Entry = FinallyVA;
  Finally.Name = "outer_finally_callback";
  Finally.ReturnType = NdType::makeVoid();
  MedVar Abnormal;
  Abnormal.Kind = MedVar::Param;
  Abnormal.TheArch = Arch::X64;
  Abnormal.Id = 0;
  Abnormal.Size = 1;
  Abnormal.RegOff = x86reg::RCX;
  Finally.Params.push_back(Abnormal);
  Finally.TypedParams.push_back(
      {"abnormal_termination", NdType::makeInt(1, /*Signed=*/false)});
  MedVar Frame;
  Frame.Kind = MedVar::Param;
  Frame.TheArch = Arch::X64;
  Frame.Id = 1;
  Frame.Size = 8;
  Frame.RegOff = x86reg::RDX;
  Finally.Params.push_back(Frame);
  Finally.TypedParams.push_back({"establisher_frame", NdType::makePtr()});
  MedBlock FinallyBody;
  FinallyBody.Id = 0;
  FinallyBody.StartAddr = FinallyVA;
  FinallyBody.EndAddr = FinallyVA + 2;
  MedOp FinallyCall;
  FinallyCall.Opcode = NdOp::CALL;
  FinallyCall.Addr = FinallyVA;
  FinallyCall.addInput(MedVar::makeConst(FinallyMayThrowVA, 8));
  FinallyBody.Ops.push_back(std::move(FinallyCall));
  MedOp FinallyReturn;
  FinallyReturn.Opcode = NdOp::RETURN;
  FinallyReturn.Addr = FinallyVA + 1;
  FinallyBody.Ops.push_back(std::move(FinallyReturn));
  Finally.Blocks.push_back(std::move(FinallyBody));

  MedFunc Personality =
      makeAddressBackedPersonality(PersonalityVA, "\01__C_specific_handler");
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Parent, Finally, Personality}, Context, "outer_seh_finally", Arch::X64,
      {{MayThrowVA, "may_throw"}, {FinallyMayThrowVA, "finally_may_throw"}},
      nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectVerifierClean(*Module);

  llvm::Function *ParentFunction = Module->getFunction(Parent.Name);
  ASSERT_NE(ParentFunction, nullptr);
  expectCompleteRewriteContract(*ParentFunction, /*ProtectedCalls=*/1);
  llvm::CallInst *FinallyCallInst = nullptr;
  for (llvm::BasicBlock &Block : *ParentFunction)
    for (llvm::Instruction &Instruction : Block) {
      auto *Candidate = llvm::dyn_cast<llvm::CallInst>(&Instruction);
      const llvm::Function *Callee =
          Candidate ? Candidate->getCalledFunction() : nullptr;
      if (Callee && Callee->getName() == Finally.Name) {
        ASSERT_EQ(FinallyCallInst, nullptr);
        FinallyCallInst = Candidate;
      }
    }
  ASSERT_NE(FinallyCallInst, nullptr);
  const auto *FinallyCleanupReturn = llvm::dyn_cast<llvm::CleanupReturnInst>(
      FinallyCallInst->getParent()->getTerminator());
  ASSERT_NE(FinallyCleanupReturn, nullptr);
  EXPECT_TRUE(FinallyCleanupReturn->unwindsToCaller());

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();
  auto Plan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());

  llvm::Function *FinallyFunction = Module->getFunction(Finally.Name);
  ASSERT_NE(FinallyFunction, nullptr);
  EXPECT_TRUE(FinallyFunction->hasFnAttribute(llvm::Attribute::NoInline));
  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Deep;
  Options.LLVMLevel = llvm::OptimizationLevel::O3;
  OptimizationResult Optimization = Pipeline::optimizeModule(*Module, Options);
  EXPECT_NE(Optimization.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_NE(Optimization.Stop, OptimizationStopReason::VerificationFailed);
  expectVerifierClean(*Module);
  auto OptimizedPlan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(OptimizedPlan))
      << llvm::toString(OptimizedPlan.takeError()) << '\n'
      << printModuleIR(*Module);

  ensureCOFFCodegenTargets();
  CompiledImage Compiled = compileImageForPatch(
      *Module, Arch::X64, BinaryFormat::COFF, 0x140120000,
      [&](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "may_throw")
          return MayThrowVA;
        if (Symbol == "finally_may_throw")
          return FinallyMayThrowVA;
        if (Symbol == "__C_specific_handler")
          return PersonalityVA;
        return std::nullopt;
      },
      Image.Base);
  ASSERT_TRUE(Compiled.Success);
  EXPECT_TRUE(Compiled.Unresolved.empty());
}

TEST(COFFExceptionIR, PreservesNestedFinallyToFinallyUnwindThroughO3) {
  constexpr va_t ParentVA = 0x140121000;
  constexpr va_t InnerEndVA = ParentVA + 8;
  constexpr va_t OuterEndVA = ParentVA + 0x10;
  constexpr va_t InnerFinallyVA = 0x140122000;
  constexpr va_t OuterFinallyVA = 0x140123000;
  constexpr va_t MayThrowVA = 0x140124000;
  constexpr va_t PersonalityVA = 0x140125000;

  auto MakeFinallyCallback = [](va_t Address, llvm::StringRef Name) {
    MedFunc Callback;
    Callback.Entry = Address;
    Callback.Name = Name.str();
    Callback.ReturnType = NdType::makeVoid();

    MedVar Abnormal;
    Abnormal.Kind = MedVar::Param;
    Abnormal.TheArch = Arch::X64;
    Abnormal.Id = 0;
    Abnormal.Size = 1;
    Abnormal.RegOff = x86reg::RCX;
    Callback.Params.push_back(Abnormal);
    Callback.TypedParams.push_back(
        {"abnormal_termination", NdType::makeInt(1, /*Signed=*/false)});

    MedVar Frame;
    Frame.Kind = MedVar::Param;
    Frame.TheArch = Arch::X64;
    Frame.Id = 1;
    Frame.Size = 8;
    Frame.RegOff = x86reg::RDX;
    Callback.Params.push_back(Frame);
    Callback.TypedParams.push_back({"establisher_frame", NdType::makePtr()});

    MedBlock Body;
    Body.Id = 0;
    Body.StartAddr = Address;
    Body.EndAddr = Address + 1;
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Address;
    Body.Ops.push_back(std::move(Return));
    Callback.Blocks.push_back(std::move(Body));
    return Callback;
  };

  MedFunc Parent;
  Parent.Entry = ParentVA;
  Parent.Name = "nested_finally_to_finally_parent";
  Parent.ReturnType = NdType::makeVoid();

  MedBlock Inner;
  Inner.Id = 0;
  Inner.StartAddr = ParentVA;
  Inner.EndAddr = InnerEndVA;
  Inner.Succs = {1};
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = ParentVA + 2;
  Call.addInput(MedVar::makeConst(MayThrowVA, 8));
  Inner.Ops.push_back(std::move(Call));
  MedOp Branch;
  Branch.Opcode = NdOp::BRANCH;
  Branch.Addr = ParentVA + 4;
  Branch.addInput(MedVar::makeConst(InnerEndVA, 8));
  Inner.Ops.push_back(std::move(Branch));

  MedBlock OuterTail;
  OuterTail.Id = 1;
  OuterTail.StartAddr = InnerEndVA;
  OuterTail.EndAddr = OuterEndVA;
  OuterTail.Preds = {0};
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = InnerEndVA;
  OuterTail.Ops.push_back(std::move(Return));
  Parent.Blocks.push_back(std::move(Inner));
  Parent.Blocks.push_back(std::move(OuterTail));

  ExceptionFunction EH;
  EH.CodeRange = {ParentVA, OuterEndVA};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = PersonalityVA;
  EH.SEH.emplace();

  SEHScopeRecord OuterFinally;
  OuterFinally.ParseStatus = ExceptionParseStatus::Complete;
  OuterFinally.GuardedRange = {ParentVA, OuterEndVA};
  OuterFinally.Kind = SEHScopeKind::Finally;
  OuterFinally.FilterOrFinallyVA = OuterFinallyVA;
  OuterFinally.HandlerVA = OuterFinallyVA;

  SEHScopeRecord InnerFinally;
  InnerFinally.ParseStatus = ExceptionParseStatus::Complete;
  InnerFinally.GuardedRange = {ParentVA, InnerEndVA};
  InnerFinally.Kind = SEHScopeKind::Finally;
  InnerFinally.FilterOrFinallyVA = InnerFinallyVA;
  InnerFinally.HandlerVA = InnerFinallyVA;
  EH.SEH->Scopes.push_back(InnerFinally);
  EH.SEH->Scopes.push_back(OuterFinally);
  Parent.ExceptionMetadata = EH;

  MedFunc InnerCallback =
      MakeFinallyCallback(InnerFinallyVA, "inner_finally_callback");
  MedFunc OuterCallback =
      MakeFinallyCallback(OuterFinallyVA, "outer_finally_callback_nested");
  MedFunc Personality =
      makeAddressBackedPersonality(PersonalityVA, "\01__C_specific_handler");

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Parent, InnerCallback, OuterCallback, Personality}, Context,
      "nested_finally_to_finally", Arch::X64, {{MayThrowVA, "may_throw"}},
      nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectVerifierClean(*Module);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();

  auto CheckNestedDispatch = [&](llvm::Module &Candidate) {
    llvm::Function *CandidateParent = Candidate.getFunction(Parent.Name);
    ASSERT_NE(CandidateParent, nullptr);
    llvm::InvokeInst *InnerInvoke = nullptr;
    llvm::CallInst *OuterCall = nullptr;
    for (llvm::BasicBlock &Block : *CandidateParent)
      for (llvm::Instruction &Instruction : Block) {
        auto *CallBase = llvm::dyn_cast<llvm::CallBase>(&Instruction);
        const llvm::Function *Callee =
            CallBase ? CallBase->getCalledFunction() : nullptr;
        if (!Callee)
          continue;
        if (Callee->getName() == InnerCallback.Name)
          InnerInvoke = llvm::dyn_cast<llvm::InvokeInst>(CallBase);
        if (Callee->getName() == OuterCallback.Name)
          OuterCall = llvm::dyn_cast<llvm::CallInst>(CallBase);
      }

    ASSERT_NE(InnerInvoke, nullptr);
    ASSERT_NE(OuterCall, nullptr);
    EXPECT_EQ(InnerInvoke->getUnwindDest(), OuterCall->getParent());
    EXPECT_NE(
        llvm::dyn_cast<llvm::CleanupPadInst>(&OuterCall->getParent()->front()),
        nullptr);
    const auto *Continuation = llvm::dyn_cast<llvm::CleanupReturnInst>(
        InnerInvoke->getNormalDest()->getTerminator());
    ASSERT_NE(Continuation, nullptr);
    EXPECT_EQ(Continuation->getUnwindDest(), OuterCall->getParent());
  };

  CheckNestedDispatch(*Module);
  auto InitialPlan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(InitialPlan))
      << llvm::toString(InitialPlan.takeError());

  llvm::Function *InnerFunction = Module->getFunction(InnerCallback.Name);
  llvm::Function *OuterFunction = Module->getFunction(OuterCallback.Name);
  ASSERT_NE(InnerFunction, nullptr);
  ASSERT_NE(OuterFunction, nullptr);
  EXPECT_TRUE(InnerFunction->hasFnAttribute(llvm::Attribute::NoInline));
  EXPECT_TRUE(OuterFunction->hasFnAttribute(llvm::Attribute::NoInline));
  EXPECT_TRUE(InnerFunction->hasFnAttribute(llvm::Attribute::OptimizeNone));
  EXPECT_TRUE(OuterFunction->hasFnAttribute(llvm::Attribute::OptimizeNone));

  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Deep;
  Options.LLVMLevel = llvm::OptimizationLevel::O3;
  OptimizationResult Optimization = Pipeline::optimizeModule(*Module, Options);
  EXPECT_NE(Optimization.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_NE(Optimization.Stop, OptimizationStopReason::VerificationFailed);
  expectVerifierClean(*Module);
  CheckNestedDispatch(*Module);

  auto OptimizedPlan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(OptimizedPlan))
      << llvm::toString(OptimizedPlan.takeError()) << '\n'
      << printModuleIR(*Module);
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

TEST(COFFExceptionIR,
     BindsSeparatedCxxContinuationReturnToExactParentBlockPrivately) {
  constexpr va_t ParentVA = 0x140001000;
  constexpr va_t ContinuationVA = ParentVA + 0x20;
  constexpr va_t CatchVA = 0x140002000;

  MedFunc Parent = makeContinuationPlanFunction(
      ParentVA, "separated_parent", {ParentVA, ContinuationVA});
  MedFunc Catch =
      makeContinuationPlanFunction(CatchVA, "separated_catch", {CatchVA});
  Catch.ReturnType = NdType::makeInt(/*Sz=*/8, /*Signed=*/false);
  setSeparatedFH3GroupIdentity(Parent, Catch, /*NativeFuncInfoVA=*/0x140010800);
  addContinuationEvidence(Catch, /*BlockIndex=*/0, {ContinuationVA});

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;

  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  auto Module = Emitter.emit({Parent, Catch}, Context, "separated_cxx_return",
                             Arch::X64, {}, &Image, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectVerifierClean(*Module);
  expectNoContinuationReturnMarkers(*Module);

  const auto Plan =
      MedLLVMEmitterTestPeer::cxxContinuationPlan(Emitter, CatchVA);
  ASSERT_TRUE(Plan.has_value());
  ASSERT_TRUE(Plan->Complete);
  ASSERT_EQ(Plan->Bindings.size(), 1u);
  ASSERT_NE(Plan->Bindings.front().first, nullptr);
  ASSERT_NE(Plan->Bindings.front().second, nullptr);

  const llvm::ReturnInst *Return = Plan->Bindings.front().first;
  const llvm::BasicBlock *Target = Plan->Bindings.front().second;
  EXPECT_EQ(Return->getFunction(), Module->getFunction("separated_catch"));
  EXPECT_EQ(Target->getParent(), Module->getFunction("separated_parent"));
  EXPECT_EQ(Target->getName(), "bb_1");
  const auto *ReturnValue =
      llvm::dyn_cast<llvm::ConstantExpr>(Return->getReturnValue());
  ASSERT_NE(ReturnValue, nullptr);
  ASSERT_EQ(ReturnValue->getOpcode(), llvm::Instruction::PtrToInt);
  const auto *BlockAddress =
      llvm::dyn_cast<llvm::BlockAddress>(ReturnValue->getOperand(0));
  ASSERT_NE(BlockAddress, nullptr);
  EXPECT_EQ(BlockAddress->getBasicBlock(), Target);
}

TEST(COFFExceptionIR, RejectsIncompleteSeparatedCxxContinuationPlans) {
  constexpr va_t ParentVA = 0x140011000;
  constexpr va_t ContinuationA = ParentVA + 0x20;
  constexpr va_t ContinuationB = ParentVA + 0x40;
  constexpr va_t CatchVA = 0x140012000;

  {
    SCOPED_TRACE("completed analysis with no exit evidence");
    MedFunc Catch =
        makeContinuationPlanFunction(CatchVA, "empty_evidence", {CatchVA});
    setSeparatedFH3GroupIdentity(Catch, /*NativeFuncInfoVA=*/0x140010810,
                                 /*IsCatchFunclet=*/true);
    EXPECT_FALSE(hasCompleteContinuationPlan({Catch}, CatchVA));
  }
  {
    SCOPED_TRACE("exit evidence has no target");
    MedFunc Parent = makeContinuationPlanFunction(
        ParentVA, "empty_target_parent", {ParentVA, ContinuationA});
    MedFunc Catch =
        makeContinuationPlanFunction(CatchVA, "empty_target_catch", {CatchVA});
    setSeparatedFH3GroupIdentity(Parent, Catch, /*NativeFuncInfoVA=*/0x140010820);
    addContinuationEvidence(Catch, 0, {});
    EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA,
                                             /*BodyMask=*/nullptr,
                                             /*UseImage=*/false));
  }
  {
    SCOPED_TRACE("exit evidence has multiple targets");
    MedFunc Parent = makeContinuationPlanFunction(
        ParentVA, "multi_target_parent",
        {ParentVA, ContinuationA, ContinuationB});
    MedFunc Catch =
        makeContinuationPlanFunction(CatchVA, "multi_target_catch", {CatchVA});
    Catch.ReturnType = NdType::makeInt(8, false);
    setSeparatedFH3GroupIdentity(Parent, Catch, /*NativeFuncInfoVA=*/0x140010830);
    addContinuationEvidence(Catch, 0, {ContinuationA, ContinuationB});
    EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));
  }
  {
    SCOPED_TRACE("exact RETURN occurrence is missing");
    MedFunc Parent = makeContinuationPlanFunction(
        ParentVA, "missing_bind_parent", {ParentVA, ContinuationA});
    MedFunc Catch =
        makeContinuationPlanFunction(CatchVA, "missing_bind_catch", {CatchVA});
    Catch.ReturnType = NdType::makeInt(8, false);
    setSeparatedFH3GroupIdentity(Parent, Catch, /*NativeFuncInfoVA=*/0x140010840);
    addContinuationEvidence(Catch, 0, {ContinuationA});
    Catch.CxxContinuationExits.front().ReturnSeq += 1;
    EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));
  }
  {
    SCOPED_TRACE("target belongs to the source function");
    MedFunc Catch = makeContinuationPlanFunction(
        CatchVA, "same_function_target", {CatchVA, CatchVA + 0x20});
    Catch.ReturnType = NdType::makeInt(8, false);
    setSeparatedFH3GroupIdentity(Catch, /*NativeFuncInfoVA=*/0x140010850,
                                 /*IsCatchFunclet=*/true);
    addContinuationEvidence(Catch, 0, {CatchVA + 0x20});
    EXPECT_FALSE(hasCompleteContinuationPlan({Catch}, CatchVA));
  }
  {
    SCOPED_TRACE("target address has two lifted block owners");
    constexpr va_t SharedTarget = 0x140013080;
    MedFunc OwnerA = makeContinuationPlanFunction(
        0x140013000, "ambiguous_owner_a", {0x140013000, SharedTarget});
    MedFunc OwnerB = makeContinuationPlanFunction(
        0x140014000, "ambiguous_owner_b", {0x140014000, SharedTarget});
    MedFunc Catch = makeContinuationPlanFunction(
        CatchVA, "ambiguous_target_catch", {CatchVA});
    setSeparatedFH3GroupIdentity(OwnerA, /*NativeFuncInfoVA=*/0x140010860,
                                 /*IsCatchFunclet=*/false);
    setSeparatedFH3GroupIdentity(OwnerB, /*NativeFuncInfoVA=*/0x140010860,
                                 /*IsCatchFunclet=*/false);
    setSeparatedFH3GroupIdentity(Catch, /*NativeFuncInfoVA=*/0x140010860,
                                 /*IsCatchFunclet=*/true);
    addContinuationEvidence(Catch, 0, {SharedTarget});
    EXPECT_FALSE(
        hasCompleteContinuationPlan({OwnerA, OwnerB, Catch}, CatchVA));
  }
  {
    SCOPED_TRACE("target address has no lifted block");
    constexpr va_t MissingTarget = 0x14001f000;
    MedFunc Catch =
        makeContinuationPlanFunction(CatchVA, "missing_target", {CatchVA});
    setSeparatedFH3GroupIdentity(Catch, /*NativeFuncInfoVA=*/0x140010870,
                                 /*IsCatchFunclet=*/true);
    addContinuationEvidence(Catch, 0, {MissingTarget});
    EXPECT_FALSE(hasCompleteContinuationPlan({Catch}, CatchVA,
                                             /*BodyMask=*/nullptr,
                                             /*UseImage=*/false));
  }
  {
    SCOPED_TRACE("invalid target sentinel");
    MedFunc Catch =
        makeContinuationPlanFunction(CatchVA, "invalid_target", {CatchVA});
    setSeparatedFH3GroupIdentity(Catch, /*NativeFuncInfoVA=*/0x140010880,
                                 /*IsCatchFunclet=*/true);
    addContinuationEvidence(Catch, 0, {InvalidVA});
    EXPECT_FALSE(hasCompleteContinuationPlan({Catch}, CatchVA,
                                             /*BodyMask=*/nullptr,
                                             /*UseImage=*/false));
  }
  {
    SCOPED_TRACE("duplicate evidence for one full occurrence identity");
    MedFunc Parent = makeContinuationPlanFunction(
        ParentVA, "duplicate_evidence_parent", {ParentVA, ContinuationA});
    MedFunc Catch = makeContinuationPlanFunction(
        CatchVA, "duplicate_evidence_catch", {CatchVA});
    Catch.ReturnType = NdType::makeInt(8, false);
    setSeparatedFH3GroupIdentity(Parent, Catch, /*NativeFuncInfoVA=*/0x140010890);
    addContinuationEvidence(Catch, 0, {ContinuationA});
    Catch.CxxContinuationExits.push_back(Catch.CxxContinuationExits.front());
    EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));
  }
}

TEST(COFFExceptionIR,
     RejectsContinuationPlanWhenReturnValueDoesNotNameExactTargetBlock) {
  constexpr va_t ParentVA = 0x140021000;
  constexpr va_t ExpectedTarget = ParentVA + 0x20;
  constexpr va_t EmittedTarget = ParentVA + 0x40;
  constexpr va_t CatchVA = 0x140022000;
  MedFunc Parent = makeContinuationPlanFunction(
      ParentVA, "wrong_value_parent",
      {ParentVA, ExpectedTarget, EmittedTarget});
  MedFunc Catch =
      makeContinuationPlanFunction(CatchVA, "wrong_value_catch", {CatchVA});
  Catch.ReturnType = NdType::makeInt(8, false);
  setSeparatedFH3GroupIdentity(Parent, Catch, /*NativeFuncInfoVA=*/0x140020800);
  addContinuationEvidence(Catch, 0, {ExpectedTarget});
  MedVar WrongValue = MedVar::makeConst(
      EmittedTarget, /*Sz=*/8, ConstantAddressProvenance::CodeAddress);
  Catch.Blocks.front().Ops.back().Inputs[0] = WrongValue;
  Catch.CxxContinuationExits.front().ReturnValue = WrongValue;

  EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));
}

TEST(COFFExceptionIR, RejectsVoidContinuationReturnEvenWithExactEvidence) {
  constexpr va_t ParentVA = 0x140031000;
  constexpr va_t ContinuationVA = ParentVA + 0x20;
  constexpr va_t CatchVA = 0x140032000;
  MedFunc Parent = makeContinuationPlanFunction(
      ParentVA, "void_return_parent", {ParentVA, ContinuationVA});
  MedFunc Catch =
      makeContinuationPlanFunction(CatchVA, "void_return_catch", {CatchVA});
  setSeparatedFH3GroupIdentity(Parent, Catch, /*NativeFuncInfoVA=*/0x140030800);
  addContinuationEvidence(Catch, 0, {ContinuationVA});

  EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));
}

TEST(COFFExceptionIR, RejectsBodyMaskOmittedContinuationMember) {
  constexpr va_t ParentVA = 0x140041000;
  constexpr va_t ContinuationVA = ParentVA + 0x20;
  constexpr va_t CatchVA = 0x140042000;
  MedFunc Parent = makeContinuationPlanFunction(
      ParentVA, "masked_parent", {ParentVA, ContinuationVA});
  MedFunc Catch =
      makeContinuationPlanFunction(CatchVA, "masked_catch", {CatchVA});
  Catch.ReturnType = NdType::makeInt(8, false);
  setSeparatedFH3GroupIdentity(Parent, Catch, /*NativeFuncInfoVA=*/0x140040800);
  addContinuationEvidence(Catch, 0, {ContinuationVA});
  const std::vector<char> BodyMask{1, 0};

  EXPECT_FALSE(
      hasCompleteContinuationPlan({Parent, Catch}, CatchVA, &BodyMask));
}

TEST(COFFExceptionIR, RequiresExactSetOfSeparatedCatchReturns) {
  constexpr va_t ParentVA = 0x140045000;
  constexpr va_t ContinuationVA = ParentVA + 0x20;
  constexpr va_t CatchVA = 0x140046000;
  MedFunc Parent = makeContinuationPlanFunction(
      ParentVA, "return_set_parent", {ParentVA, ContinuationVA});
  MedFunc Catch = makeContinuationPlanFunction(
      CatchVA, "return_set_catch", {CatchVA, CatchVA + 0x20});
  Catch.ReturnType = NdType::makeInt(8, false);
  setSeparatedFH3GroupIdentity(Parent, Catch,
                               /*NativeFuncInfoVA=*/0x140044800);
  addContinuationEvidence(Catch, 0, {ContinuationVA});

  EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));

  addContinuationEvidence(Catch, 1, {ContinuationVA});
  EXPECT_TRUE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));
}

TEST(COFFExceptionIR, RejectsAmbiguousOrMissingSeparatedFH3Parent) {
  constexpr va_t ParentVA = 0x140047000;
  constexpr va_t ContinuationVA = ParentVA + 0x20;
  constexpr va_t CatchVA = 0x140048000;
  MedFunc ParentA = makeContinuationPlanFunction(
      ParentVA, "duplicate_parent_a", {ParentVA, ContinuationVA});
  MedFunc ParentB = makeContinuationPlanFunction(
      0x140049000, "duplicate_parent_b", {0x140049000});
  MedFunc Catch =
      makeContinuationPlanFunction(CatchVA, "duplicate_parent_catch", {CatchVA});
  Catch.ReturnType = NdType::makeInt(8, false);
  setSeparatedFH3GroupIdentity(ParentA, Catch,
                               /*NativeFuncInfoVA=*/0x140046800);
  setSeparatedFH3GroupIdentity(ParentB, /*NativeFuncInfoVA=*/0x140046800,
                               /*IsCatchFunclet=*/false);
  addContinuationEvidence(Catch, 0, {ContinuationVA});
  EXPECT_FALSE(
      hasCompleteContinuationPlan({ParentA, ParentB, Catch}, CatchVA));

  MedFunc PlainTarget = makeContinuationPlanFunction(
      ParentVA, "missing_parent_target", {ParentVA, ContinuationVA});
  MedFunc OrphanCatch = makeContinuationPlanFunction(
      CatchVA, "missing_parent_catch", {CatchVA});
  OrphanCatch.ReturnType = NdType::makeInt(8, false);
  setSeparatedFH3GroupIdentity(OrphanCatch,
                               /*NativeFuncInfoVA=*/0x140046900,
                               /*IsCatchFunclet=*/true);
  addContinuationEvidence(OrphanCatch, 0, {ContinuationVA});
  EXPECT_FALSE(
      hasCompleteContinuationPlan({PlainTarget, OrphanCatch}, CatchVA));
}

TEST(COFFExceptionIR, RequiresUniqueParentHandlerDeclarationForCatchMember) {
  constexpr va_t ParentVA = 0x14004a000;
  constexpr va_t ContinuationVA = ParentVA + 0x20;
  constexpr va_t CatchVA = 0x14004b000;
  MedFunc Parent = makeContinuationPlanFunction(
      ParentVA, "undeclared_catch_parent", {ParentVA, ContinuationVA});
  MedFunc Catch = makeContinuationPlanFunction(
      CatchVA, "undeclared_catch_member", {CatchVA});
  Catch.ReturnType = NdType::makeInt(8, false);
  setSeparatedFH3GroupIdentity(Parent, /*NativeFuncInfoVA=*/0x140049800,
                               /*IsCatchFunclet=*/false);
  setSeparatedFH3GroupIdentity(Catch, /*NativeFuncInfoVA=*/0x140049800,
                               /*IsCatchFunclet=*/true);
  addContinuationEvidence(Catch, 0, {ContinuationVA});
  EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));

  CxxTryBlock DuplicateTry;
  CxxCatchHandler First;
  First.HandlerVA = CatchVA;
  DuplicateTry.Handlers.push_back(First);
  DuplicateTry.Handlers.push_back(std::move(First));
  Parent.ExceptionMetadata->Cxx->TryBlocks.push_back(std::move(DuplicateTry));
  EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));
}

TEST(COFFExceptionIR, RejectsUnauthenticatedSeparatedFH3SourceShape) {
  constexpr va_t ParentVA = 0x14004c000;
  constexpr va_t ContinuationVA = ParentVA + 0x20;
  constexpr va_t CatchVA = 0x14004d000;
  auto Make = [&] {
    MedFunc Parent = makeContinuationPlanFunction(
        ParentVA, "source_shape_parent", {ParentVA, ContinuationVA});
    MedFunc Catch = makeContinuationPlanFunction(
        CatchVA, "source_shape_catch", {CatchVA});
    Catch.ReturnType = NdType::makeInt(8, false);
    setSeparatedFH3GroupIdentity(Parent, Catch,
                                 /*NativeFuncInfoVA=*/0x14004b800);
    addContinuationEvidence(Catch, 0, {ContinuationVA});
    return std::pair<MedFunc, MedFunc>{std::move(Parent), std::move(Catch)};
  };

  {
    auto [Parent, Catch] = Make();
    Catch.ExceptionMetadata->ParseStatus = ExceptionParseStatus::Partial;
    EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));
  }
  {
    auto [Parent, Catch] = Make();
    Catch.ExceptionMetadata->Kind = RuntimeFunctionKind::Fragment;
    EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));
  }
  {
    auto [Parent, Catch] = Make();
    Catch.ExceptionMetadata->CodeRange.Begin = Catch.Entry + 1;
    EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));
  }
  {
    auto [Parent, Catch] = Make();
    Catch.ExceptionMetadata->CodeRange.End = Catch.Entry;
    EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));
  }
  {
    auto [Parent, Catch] = Make();
    Catch.ExceptionMetadata->CodeRange.End = Catch.Entry - 1;
    EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));
  }
}

TEST(COFFExceptionIR, RejectsContinuationPlanOutsideX64COFF) {
  constexpr va_t ParentVA = 0x14004e000;
  constexpr va_t ContinuationVA = ParentVA + 0x20;
  constexpr va_t CatchVA = 0x14004f000;
  MedFunc Parent = makeContinuationPlanFunction(
      ParentVA, "non_coff_parent", {ParentVA, ContinuationVA});
  MedFunc Catch =
      makeContinuationPlanFunction(CatchVA, "non_coff_catch", {CatchVA});
  Catch.ReturnType = NdType::makeInt(8, false);
  setSeparatedFH3GroupIdentity(Parent, Catch,
                               /*NativeFuncInfoVA=*/0x14004d800);
  addContinuationEvidence(Catch, 0, {ContinuationVA});

  EXPECT_FALSE(hasCompleteContinuationPlan(
      {Parent, Catch}, CatchVA, /*BodyMask=*/nullptr, /*UseImage=*/true,
      BinaryFormat::ELF));
}

TEST(COFFExceptionIR, RejectsContinuationTargetOwnedByUnrelatedFH3Group) {
  constexpr va_t ParentVA = 0x140051000;
  constexpr va_t UnrelatedVA = 0x140052000;
  constexpr va_t UnrelatedTarget = UnrelatedVA + 0x20;
  constexpr va_t CatchVA = 0x140053000;
  MedFunc Parent =
      makeContinuationPlanFunction(ParentVA, "group_parent", {ParentVA});
  MedFunc Unrelated = makeContinuationPlanFunction(
      UnrelatedVA, "unrelated_group_parent", {UnrelatedVA, UnrelatedTarget});
  MedFunc Catch =
      makeContinuationPlanFunction(CatchVA, "group_catch", {CatchVA});
  Catch.ReturnType = NdType::makeInt(8, false);
  setSeparatedFH3GroupIdentity(Parent, Catch,
                               /*NativeFuncInfoVA=*/0x140050800);
  setSeparatedFH3GroupIdentity(Unrelated,
                               /*NativeFuncInfoVA=*/0x140050900,
                               /*IsCatchFunclet=*/false);
  addContinuationEvidence(Catch, 0, {UnrelatedTarget});

  EXPECT_FALSE(
      hasCompleteContinuationPlan({Parent, Unrelated, Catch}, CatchVA));
}

TEST(COFFExceptionIR, RejectsAmbiguousFH3RangeOwnerForContinuationTarget) {
  constexpr va_t ParentVA = 0x140056000;
  constexpr va_t OverlapVA = ParentVA + 0x10;
  constexpr va_t ContinuationVA = ParentVA + 0x40;
  constexpr va_t CatchVA = 0x140057000;
  MedFunc Parent = makeContinuationPlanFunction(
      ParentVA, "overlap_parent", {ParentVA, ContinuationVA});
  MedFunc Overlap = makeContinuationPlanFunction(
      OverlapVA, "overlap_ineligible_contribution", {OverlapVA});
  MedFunc Catch =
      makeContinuationPlanFunction(CatchVA, "overlap_catch", {CatchVA});
  Catch.ReturnType = NdType::makeInt(8, false);
  setSeparatedFH3GroupIdentity(Parent, Catch,
                               /*NativeFuncInfoVA=*/0x140055800);
  setSeparatedFH3GroupIdentity(Overlap,
                               /*NativeFuncInfoVA=*/InvalidVA,
                               /*IsCatchFunclet=*/false);
  Overlap.ExceptionMetadata->CodeRange = {OverlapVA, ParentVA + 0x80};
  Overlap.ExceptionMetadata->Personality = ExceptionPersonality::Unknown;
  Overlap.ExceptionMetadata->Cxx->IsSeparated = false;
  addContinuationEvidence(Catch, 0, {ContinuationVA});

  EXPECT_FALSE(
      hasCompleteContinuationPlan({Parent, Overlap, Catch}, CatchVA));
}

TEST(COFFExceptionIR, RejectsContinuationTargetThatIsAnyMedFuncEntry) {
  constexpr va_t ParentVA = 0x140058000;
  constexpr va_t ContinuationVA = ParentVA + 0x20;
  constexpr va_t CatchVA = 0x140059000;
  MedFunc Parent = makeContinuationPlanFunction(
      ParentVA, "entry_parent", {ParentVA, ContinuationVA});
  MedFunc InteriorEntry;
  InteriorEntry.Entry = ContinuationVA;
  InteriorEntry.Name = "empty_interior_function";
  MedFunc Catch =
      makeContinuationPlanFunction(CatchVA, "entry_catch", {CatchVA});
  Catch.ReturnType = NdType::makeInt(8, false);
  setSeparatedFH3GroupIdentity(Parent, Catch,
                               /*NativeFuncInfoVA=*/0x140057800);
  addContinuationEvidence(Catch, 0, {ContinuationVA});

  EXPECT_FALSE(
      hasCompleteContinuationPlan({Parent, InteriorEntry, Catch}, CatchVA));
}

TEST(COFFExceptionIR, RequiresContinuationOccurrencesInsideOwnerCodeRanges) {
  constexpr va_t ParentVA = 0x140054000;
  constexpr va_t CatchVA = 0x140055000;
  auto Make = [&](va_t Target, va_t NativeFuncInfoVA) {
    std::vector<va_t> ParentBlocks{ParentVA};
    if (Target != ParentVA)
      ParentBlocks.push_back(Target);
    MedFunc Parent = makeContinuationPlanFunction(
        ParentVA, "range_parent", ParentBlocks);
    MedFunc Catch =
        makeContinuationPlanFunction(CatchVA, "range_catch", {CatchVA});
    Catch.ReturnType = NdType::makeInt(8, false);
    setSeparatedFH3GroupIdentity(Parent, Catch, NativeFuncInfoVA);
    addContinuationEvidence(Catch, 0, {Target});
    return std::pair<MedFunc, MedFunc>{std::move(Parent), std::move(Catch)};
  };

  {
    auto [Parent, Catch] = Make(ParentVA + 0x120, 0x140053800);
    EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));
  }
  {
    auto [Parent, Catch] = Make(ParentVA, 0x140053810);
    EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));
  }
  {
    auto [Parent, Catch] = Make(ParentVA + 0x100, 0x140053820);
    EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));
  }
  {
    auto [Parent, Catch] = Make(ParentVA + 0x20, 0x140053830);
    MedOp &Return = Catch.Blocks.front().Ops.back();
    Return.Addr = Catch.ExceptionMetadata->CodeRange.Begin;
    Catch.CxxContinuationExits.front().ReturnAddr = Return.Addr;
    EXPECT_TRUE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));

    Return.Addr = Catch.ExceptionMetadata->CodeRange.End;
    Catch.CxxContinuationExits.front().ReturnAddr = Return.Addr;
    EXPECT_FALSE(hasCompleteContinuationPlan({Parent, Catch}, CatchVA));
  }
}

TEST(COFFExceptionIR, RejectsShortBodyMaskWithoutReadingPastIt) {
  MedFunc Parent =
      makeContinuationPlanFunction(0x140061000, "short_mask_parent",
                                   {0x140061000, 0x140061020});
  MedFunc Catch = makeContinuationPlanFunction(
      0x140062000, "short_mask_catch", {0x140062000});
  Catch.ReturnType = NdType::makeInt(8, false);
  setSeparatedFH3GroupIdentity(Parent, Catch,
                               /*NativeFuncInfoVA=*/0x140060800);
  addContinuationEvidence(Catch, 0, {0x140061020});
  const std::vector<MedFunc> Functions{Parent, Catch};
  const std::vector<char> ShortBodyMask{1};

  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  auto CompleteModule =
      Emitter.emit(Functions, Context, "complete_before_short_body_mask",
                   Arch::X64, {}, nullptr, BinaryFormat::COFF);
  ASSERT_NE(CompleteModule, nullptr);
  const auto CompletePlan =
      MedLLVMEmitterTestPeer::cxxContinuationPlan(Emitter, Catch.Entry);
  ASSERT_TRUE(CompletePlan.has_value());
  ASSERT_TRUE(CompletePlan->Complete);

  EXPECT_EQ(Emitter.emit(Functions, Context, "short_body_mask", Arch::X64, {},
                         nullptr, BinaryFormat::COFF,
                         /*MergeableGlobals=*/false, &ShortBodyMask),
            nullptr);
  EXPECT_FALSE(
      MedLLVMEmitterTestPeer::cxxContinuationPlan(Emitter, Catch.Entry)
          .has_value());
}

TEST(COFFExceptionIR, UsesRegisterIdentityForContinuationParamValues) {
  MedVar Left;
  Left.Kind = MedVar::Param;
  Left.TheArch = Arch::X64;
  Left.Id = 7;
  Left.SSAVer = 3;
  Left.Size = 8;
  Left.RegOff = 0x10;
  MedVar Right = Left;
  Right.RegOff = 0x18;

  EXPECT_FALSE(
      MedLLVMEmitterTestPeer::sameCxxContinuationReturnValue(Left, Right));
}

TEST(COFFExceptionIR, CompilesBoundedTypedFH4LiftToNativeCOFFObject) {
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t MayThrowVA = 0x140002000;
  constexpr va_t PersonalityVA = 0x140003000;
  constexpr va_t TypeDescriptorVA = 0x140003100;

  MedFunc Func;
  Func.Entry = FunctionVA;
  Func.Name = "native_fh4_object";
  Func.ReturnType = NdType::makeVoid();
  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = FunctionVA + 4;
  Protected.EndAddr = FunctionVA + 0x10;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = FunctionVA + 4;
  Call.addInput(MedVar::makeConst(MayThrowVA, 8));
  Protected.Ops.push_back(Call);
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = FunctionVA + 8;
  Protected.Ops.push_back(ProtectedReturn);
  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = FunctionVA + 0x20;
  Handler.EndAddr = FunctionVA + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = FunctionVA + 0x28;
  Handler.Ops.push_back(HandlerReturn);
  Func.Blocks = {std::move(Protected), std::move(Handler)};

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.Personality = ExceptionPersonality::CxxFrameHandler4;
  EH.PersonalityVA = PersonalityVA;
  CxxExceptionInfo Cxx;
  Cxx.NativeEncoding = CxxExceptionInfo::Encoding::FH4;
  Cxx.Flags = 0x38;
  Cxx.IsSynchronous = true;
  Cxx.MaxState = 2;
  CxxUnwindAction State0;
  State0.ToState = -1;
  State0.Kind = CxxUnwindAction::ActionKind::None;
  CxxUnwindAction State1 = State0;
  Cxx.UnwindMap = {State0, State1};
  Cxx.IPMap = {{FunctionVA, -1}, {FunctionVA + 4, 0}, {FunctionVA + 0x10, -1}};
  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.Adjectives = 9;
  Catch.TypeDescriptorVA = TypeDescriptorVA;
  Catch.HandlerVA = FunctionVA + 0x20;
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(std::move(Try));
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

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, "native_fh4_object",
                                      Arch::X64, {{MayThrowVA, "may_throw"}},
                                      &Image, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectVerifierClean(*Module);
  llvm::Function *Function = Module->getFunction(Func.Name);
  ASSERT_NE(Function, nullptr);
  EXPECT_TRUE(
      Function->hasFnAttribute(llvm::mc_rewrite::RewriteWinCxxFH4Attribute));
  auto Plan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
  ASSERT_EQ(Plan->LanguageExceptionFunctionEntries,
            std::vector<va_t>{FunctionVA});

  ensureCOFFCodegenTargets();
  CompiledImage Compiled = compileImageForPatch(
      *Module, Arch::X64, BinaryFormat::COFF, /*SectionVA=*/0x140004000,
      [&](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "may_throw")
          return MayThrowVA;
        if (Symbol == "__CxxFrameHandler4")
          return PersonalityVA;
        if (Symbol == makeNdDataSymbol(TypeDescriptorVA))
          return TypeDescriptorVA;
        return std::nullopt;
      },
      Image.Base);
  ASSERT_TRUE(Compiled.Success);
  EXPECT_TRUE(Compiled.Unresolved.empty());
  EXPECT_TRUE(Compiled.WinEHSemanticsValid);
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 1u);
  EXPECT_EQ(Compiled.WinEHSemanticRecords.front().Encoding,
            llvm::mc_rewrite::RewriteWinEHSemanticEncoding::CxxFH4);
  EXPECT_EQ(Compiled.WinEHSemanticRecords.front().RecordSize, 10u);
}

TEST(COFFExceptionIR, CompilesBoundedGSFH4LiftToNativeCOFFObject) {
  constexpr va_t FunctionVA = 0x140011000;
  constexpr va_t MayThrowVA = 0x140012000;
  constexpr va_t PersonalityVA = 0x140013000;
  constexpr va_t SecurityCookieVA = 0x140014000;
  constexpr va_t SecurityCheckVA = 0x140014100;

  MedFunc Func;
  Func.Entry = FunctionVA;
  Func.Name = "native_gs_fh4_object";
  Func.ReturnType = NdType::makeVoid();
  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = FunctionVA + 4;
  Protected.EndAddr = FunctionVA + 0x10;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = FunctionVA + 4;
  Call.addInput(MedVar::makeConst(MayThrowVA, 8));
  Protected.Ops.push_back(Call);
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = FunctionVA + 8;
  Protected.Ops.push_back(ProtectedReturn);
  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = FunctionVA + 0x20;
  Handler.EndAddr = FunctionVA + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = FunctionVA + 0x28;
  Handler.Ops.push_back(HandlerReturn);
  Func.Blocks = {std::move(Protected), std::move(Handler)};

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.Personality = ExceptionPersonality::GSHandlerCheckEH4;
  EH.PersonalityVA = PersonalityVA;
  CxxExceptionInfo Cxx;
  Cxx.NativeEncoding = CxxExceptionInfo::Encoding::FH4;
  Cxx.Flags = 0x38;
  Cxx.IsSynchronous = true;
  Cxx.MaxState = 2;
  CxxUnwindAction State0;
  State0.ToState = -1;
  State0.Kind = CxxUnwindAction::ActionKind::None;
  CxxUnwindAction State1 = State0;
  Cxx.UnwindMap = {State0, State1};
  Cxx.IPMap = {{FunctionVA, -1}, {FunctionVA + 4, 0}, {FunctionVA + 0x10, -1}};
  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.Adjectives = 0x40;
  Catch.HandlerVA = FunctionVA + 0x20;
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(std::move(Try));
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  GSCookieInfo Cookie;
  Cookie.ParseStatus = ExceptionParseStatus::Complete;
  Cookie.CookieOffset = 0x20;
  Cookie.HasExceptionHandler = true;
  Cookie.HasUnwindHandler = true;
  Cookie.Payload = {0x23, 0, 0, 0};
  EH.GSCookie = Cookie;
  Func.ExceptionMetadata = EH;
  MedFunc Personality =
      makeAddressBackedPersonality(PersonalityVA, "__GSHandlerCheck_EH4");

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Func, Personality}, Context, "native_gs_fh4_object", Arch::X64,
      {{MayThrowVA, "may_throw"}}, &Image, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectVerifierClean(*Module);
  llvm::Function *Function = Module->getFunction(Func.Name);
  ASSERT_NE(Function, nullptr);
  EXPECT_TRUE(
      Function->hasFnAttribute(llvm::mc_rewrite::RewriteWinCxxFH4Attribute));
  EXPECT_EQ(
      Function->getFnAttribute(llvm::mc_rewrite::RewriteWinGSHandlerAttribute)
          .getValueAsString(),
      llvm::mc_rewrite::RewriteWinGSHandlerCxxFH4);
  EXPECT_TRUE(Function->hasFnAttribute(llvm::Attribute::StackProtectReq));
  llvm::Function *Wrapper = Module->getFunction("__GSHandlerCheck_EH4");
  ASSERT_NE(Wrapper, nullptr);
  EXPECT_TRUE(Wrapper->isDeclaration());
  EXPECT_TRUE(Wrapper->getReturnType()->isIntegerTy(32));
  EXPECT_TRUE(Wrapper->isVarArg());
  const std::string PersonalityBodyName =
      (kAutoFuncPrefix + llvm::utohexstr(PersonalityVA)).str();
  llvm::Function *PersonalityBody = Module->getFunction(PersonalityBodyName);
  ASSERT_NE(PersonalityBody, nullptr);
  EXPECT_FALSE(PersonalityBody->isDeclaration());
  EXPECT_TRUE(PersonalityBody->getReturnType()->isIntegerTy(64));
  auto Plan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
  ASSERT_EQ(Plan->LanguageExceptionFunctionEntries,
            std::vector<va_t>{FunctionVA});

  auto ExpectGSPlanRejected = [&](std::unique_ptr<llvm::Module> Candidate,
                                  llvm::StringRef Message) {
    expectVerifierClean(*Candidate);
    auto Rejected = planCOFFExceptionPatch(*Candidate, Image, Arch::X64);
    ASSERT_FALSE(static_cast<bool>(Rejected));
    const std::string Error = llvm::toString(Rejected.takeError());
    EXPECT_NE(Error.find(Message), std::string::npos) << Error;
  };

  std::unique_ptr<llvm::Module> MissingGSWriter = llvm::CloneModule(*Module);
  llvm::Function *MissingGSWriterFunction =
      MissingGSWriter->getFunction(Func.Name);
  ASSERT_NE(MissingGSWriterFunction, nullptr);
  MissingGSWriterFunction->removeFnAttr(
      llvm::mc_rewrite::RewriteWinGSHandlerAttribute);
  ExpectGSPlanRejected(std::move(MissingGSWriter),
                       "native C++ GS writer contract was altered");

  std::unique_ptr<llvm::Module> WrongGSWriter = llvm::CloneModule(*Module);
  llvm::Function *WrongGSWriterFunction = WrongGSWriter->getFunction(Func.Name);
  ASSERT_NE(WrongGSWriterFunction, nullptr);
  WrongGSWriterFunction->removeFnAttr(
      llvm::mc_rewrite::RewriteWinGSHandlerAttribute);
  WrongGSWriterFunction->addFnAttr(
      llvm::mc_rewrite::RewriteWinGSHandlerAttribute, "invalid");
  ExpectGSPlanRejected(std::move(WrongGSWriter),
                       "native C++ GS writer contract was altered");

  std::unique_ptr<llvm::Module> MissingSSP = llvm::CloneModule(*Module);
  llvm::Function *MissingSSPFunction = MissingSSP->getFunction(Func.Name);
  ASSERT_NE(MissingSSPFunction, nullptr);
  MissingSSPFunction->removeFnAttr(llvm::Attribute::StackProtectReq);
  ExpectGSPlanRejected(std::move(MissingSSP),
                       "native C++ GS writer contract was altered");

  std::unique_ptr<llvm::Module> WrongWrapperABI = llvm::CloneModule(*Module);
  llvm::Function *WrongWrapper =
      WrongWrapperABI->getFunction("__GSHandlerCheck_EH4");
  ASSERT_NE(WrongWrapper, nullptr);
  WrongWrapper->setCallingConv(llvm::CallingConv::Win64);
  ExpectGSPlanRejected(std::move(WrongWrapperABI),
                       "native C++ GS wrapper declaration was altered");

  std::unique_ptr<llvm::Module> HiddenWrapperABI = llvm::CloneModule(*Module);
  llvm::Function *HiddenWrapper =
      HiddenWrapperABI->getFunction("__GSHandlerCheck_EH4");
  ASSERT_NE(HiddenWrapper, nullptr);
  HiddenWrapper->setVisibility(llvm::GlobalValue::HiddenVisibility);
  ExpectGSPlanRejected(std::move(HiddenWrapperABI),
                       "native C++ GS wrapper declaration was altered");

  std::unique_ptr<llvm::Module> HiddenPersonality = llvm::CloneModule(*Module);
  llvm::Function *HiddenBase =
      HiddenPersonality->getFunction("__CxxFrameHandler4");
  ASSERT_NE(HiddenBase, nullptr);
  HiddenBase->setVisibility(llvm::GlobalValue::HiddenVisibility);
  ExpectGSPlanRejected(std::move(HiddenPersonality),
                       "native WinEH personality declaration was altered");

  std::unique_ptr<llvm::Module> DefinedPersonality = llvm::CloneModule(*Module);
  llvm::Function *DefinedBase =
      DefinedPersonality->getFunction("__CxxFrameHandler4");
  ASSERT_NE(DefinedBase, nullptr);
  llvm::BasicBlock *PersonalityEntry = llvm::BasicBlock::Create(
      DefinedPersonality->getContext(), "entry", DefinedBase);
  llvm::ReturnInst::Create(
      DefinedPersonality->getContext(),
      llvm::ConstantInt::get(DefinedBase->getReturnType(), 0),
      PersonalityEntry);
  ExpectGSPlanRejected(std::move(DefinedPersonality),
                       "exception rewrite contract for '__CxxFrameHandler4'");

  ensureCOFFCodegenTargets();
  CompiledImage Compiled = compileImageForPatch(
      *Module, Arch::X64, BinaryFormat::COFF, /*SectionVA=*/0x140015000,
      [&](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "may_throw")
          return MayThrowVA;
        if (Symbol == "__CxxFrameHandler4")
          return 0x140013100;
        if (Symbol == "__GSHandlerCheck_EH4")
          return PersonalityVA;
        if (Symbol == "__security_cookie")
          return SecurityCookieVA;
        if (Symbol == "__security_check_cookie")
          return SecurityCheckVA;
        return std::nullopt;
      },
      Image.Base);
  ASSERT_TRUE(Compiled.Success);
  EXPECT_TRUE(Compiled.Unresolved.empty());
  EXPECT_TRUE(Compiled.WinEHSemanticsValid);
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 1u);
  EXPECT_EQ(Compiled.WinEHSemanticRecords.front().Encoding,
            llvm::mc_rewrite::RewriteWinEHSemanticEncoding::CxxFH4);
  EXPECT_EQ(Compiled.WinEHSemanticRecords.front().RecordSize, 6u);
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

TEST(COFFExceptionIR, CompilesBoundedAArch64FH3LiftToNativeCOFFObject) {
  constexpr va_t ImageBase = 0x140000000;
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t MayThrowVA = 0x140001200;
  constexpr va_t PersonalityVA = 0x140001300;

  MedFunc Func;
  Func.Entry = FunctionVA;
  Func.Name = "native_aarch64_fh3_test";
  Func.ReturnType = NdType::makeVoid();

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = FunctionVA;
  Protected.EndAddr = FunctionVA + 0x10;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = FunctionVA + 4;
  Call.addInput(MedVar::makeConst(MayThrowVA, 8));
  Protected.Ops.push_back(std::move(Call));
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = FunctionVA + 8;
  Protected.Ops.push_back(std::move(ProtectedReturn));

  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = FunctionVA + 0x20;
  Handler.EndAddr = FunctionVA + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = FunctionVA + 0x28;
  Handler.Ops.push_back(std::move(HandlerReturn));
  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
  EH.Encoding = ExceptionEncoding::ARM64Unpacked;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  EH.PersonalityVA = PersonalityVA;
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
      {FunctionVA, 0}, {FunctionVA + 0x10, -1}, {FunctionVA + 0x20, 1}};
  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.Adjectives = 0x40;
  Catch.HandlerVA = FunctionVA + 0x20;
  Try.Handlers.push_back(std::move(Catch));
  Cxx.TryBlocks.push_back(std::move(Try));
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  Func.ExceptionMetadata = EH;

  for (WindowsEHNativeCapability Capability :
       {WindowsEHNativeCapability::IRLowering,
        WindowsEHNativeCapability::OutputPatch}) {
    const WindowsEHNativeSourceClassification Source =
        classifyWindowsEHNativeSource(EH, Arch::AArch64, BinaryFormat::COFF,
                                      Capability);
    EXPECT_TRUE(Source.isEligible())
        << getWindowsEHNativeSourceReasonName(Source.Reason);
    EXPECT_EQ(Source.Model, WindowsEHNativeSourceModel::CxxFH3);
  }

  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = ImageBase;
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();

  MedFunc Personality =
      makeAddressBackedPersonality(PersonalityVA, "__CxxFrameHandler3");
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Func, Personality}, Context, "native_aarch64_fh3", Arch::AArch64,
      {{MayThrowVA, "may_throw"}}, &Image, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectVerifierClean(*Module);
  llvm::Function *Lifted = Module->getFunction(Func.Name);
  ASSERT_NE(Lifted, nullptr);
  EXPECT_NE(Lifted->getMetadata(windows_eh_md::NativeAttachment), nullptr);
  expectCompleteRewriteContract(*Lifted, 1);

  auto Plan = planCOFFExceptionPatch(*Module, Image, Arch::AArch64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
  ASSERT_EQ(Plan->LanguageExceptionFunctionEntries,
            std::vector<va_t>{FunctionVA});

  ensureCOFFCodegenTargets();
  CompiledImage Compiled = compileImageForPatch(
      *Module, Arch::AArch64, BinaryFormat::COFF,
      /*SectionVA=*/0x140004000,
      [&](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "may_throw")
          return MayThrowVA;
        if (Symbol == "__CxxFrameHandler3")
          return PersonalityVA;
        return std::nullopt;
      },
      ImageBase);
  ASSERT_TRUE(Compiled.Success);
  EXPECT_TRUE(Compiled.Unresolved.empty());
  EXPECT_TRUE(Compiled.WinEHSemanticsValid);
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 1u);
  EXPECT_EQ(Compiled.WinEHSemanticRecords.front().Encoding,
            llvm::mc_rewrite::RewriteWinEHSemanticEncoding::CxxFH3);
  EXPECT_TRUE(
      llvm::any_of(Compiled.Sections, [](const CompiledSection &Section) {
        return llvm::StringRef(Section.Name).starts_with(".pdata");
      }));
  EXPECT_TRUE(
      llvm::any_of(Compiled.Sections, [](const CompiledSection &Section) {
        return llvm::StringRef(Section.Name).starts_with(".xdata");
      }));
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
  // patch must reject a protected-invoke anchor that no longer authenticates
  // an exact adjacent invoke.
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
  const std::string TamperedError = llvm::toString(TamperedPlan.takeError());
  EXPECT_NE(TamperedError.find("malformed protected-invoke provenance"),
            std::string::npos)
      << TamperedError;
}

} // namespace
