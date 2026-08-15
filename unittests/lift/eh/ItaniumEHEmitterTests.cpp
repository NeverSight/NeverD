//===- ItaniumEHEmitterTests.cpp - Native Itanium EH emission tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/llvm/LanguageEHMetadata.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace {

using namespace neverd;

const llvm::ConstantInt *metadataInteger(const llvm::MDNode *Node,
                                         unsigned Index, unsigned Width) {
  if (!Node || Index >= Node->getNumOperands())
    return nullptr;
  const auto *Metadata = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
      Node->getOperand(Index).get());
  const auto *Value =
      Metadata ? llvm::dyn_cast<llvm::ConstantInt>(Metadata->getValue())
               : nullptr;
  return Value && Value->getBitWidth() == Width ? Value : nullptr;
}

MedFunc makeSingleCallCleanupFunction(llvm::StringRef Name, va_t FunctionVA,
                                      va_t MayThrowVA) {
  MedFunc Func;
  Func.Entry = FunctionVA;
  Func.Name = Name.str();
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
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = FunctionVA + 8;
  Protected.Ops.push_back(std::move(Return));

  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = FunctionVA + 0x20;
  Handler.EndAddr = FunctionVA + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = FunctionVA + 0x20;
  Handler.Ops.push_back(std::move(HandlerReturn));
  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x30};
  EH.Encoding = ExceptionEncoding::DwarfFDE;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  EH.Itanium.emplace();
  EH.Itanium->IsCallSiteAddressForm = true;
  ItaniumCallSite Site;
  Site.GuardedRange = {FunctionVA, FunctionVA + 0x10};
  Site.LandingPadVA = FunctionVA + 0x20;
  EH.Itanium->CallSites.push_back(Site);
  Func.ExceptionMetadata = std::move(EH);
  return Func;
}

TEST(ItaniumEHEmitter, PreservesAdaAndDAddressFormPersonalities) {
  struct PersonalityCase {
    ExceptionPersonality Personality;
    const char *Symbol;
  };
  constexpr PersonalityCase Cases[] = {
      {ExceptionPersonality::GnatPersonalityV0, "__gnat_personality_v0"},
      {ExceptionPersonality::GnatPersonalitySEH0, "__gnat_personality_seh0"},
      {ExceptionPersonality::DmdPersonalityV0, "__dmd_personality_v0"},
      {ExceptionPersonality::DRuntimeEhPersonality, "_d_eh_personality"},
      {ExceptionPersonality::GdcPersonalityV0, "__gdc_personality_v0"},
      {ExceptionPersonality::GdcPersonalitySEH0, "__gdc_personality_seh0"},
  };

  for (size_t I = 0; I < std::size(Cases); ++I) {
    const PersonalityCase &Case = Cases[I];
    SCOPED_TRACE(Case.Symbol);
    const va_t FunctionVA = 0x100001000 + I * 0x1000;
    const va_t MayThrowVA = 0x100010000 + I * 0x1000;
    MedFunc Func = makeSingleCallCleanupFunction(
        "native_language_personality_" + std::to_string(I), FunctionVA,
        MayThrowVA);
    Func.ExceptionMetadata->Personality = Case.Personality;

    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Func}, Context, "native-language-personality", Arch::AArch64,
        {{MayThrowVA, "may_throw"}}, nullptr, BinaryFormat::ELF);
    ASSERT_NE(Module, nullptr);
    llvm::Function *Emitted = Module->getFunction(Func.Name);
    ASSERT_NE(Emitted, nullptr);
    ASSERT_TRUE(Emitted->hasPersonalityFn());
    const auto *Personality = llvm::dyn_cast<llvm::Function>(
        Emitted->getPersonalityFn()->stripPointerCasts());
    ASSERT_NE(Personality, nullptr);
    EXPECT_EQ(Personality->getName(), Case.Symbol);

    const llvm::MDNode *Native =
        Emitted->getMetadata(language_eh_md::ItaniumAttachment);
    ASSERT_NE(Native, nullptr);
    const auto *RecordedSymbol =
        llvm::dyn_cast<llvm::MDString>(Native->getOperand(0).get());
    ASSERT_NE(RecordedSymbol, nullptr);
    EXPECT_EQ(RecordedSymbol->getString(), Case.Symbol);

    unsigned Invokes = 0;
    unsigned LandingPads = 0;
    for (llvm::BasicBlock &Block : *Emitted)
      for (llvm::Instruction &Instruction : Block) {
        Invokes += llvm::isa<llvm::InvokeInst>(Instruction);
        LandingPads += llvm::isa<llvm::LandingPadInst>(Instruction);
      }
    EXPECT_EQ(Invokes, 1u);
    EXPECT_EQ(LandingPads, 1u);
  }
}

TEST(ItaniumEHEmitter, UsesTheOriginalIndirectTypeInfoSlotForCatchClauses) {
  constexpr va_t FunctionVA = 0x100001000;
  constexpr va_t MayThrowVA = 0x100001100;
  constexpr va_t TypeInfoVA = 0x100003000;
  constexpr va_t TypeInfoSlotVA = 0x100004000;

  MedFunc Func;
  Func.Entry = FunctionVA;
  Func.Name = "itanium_indirect_typeinfo";
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
  HandlerReturn.Addr = FunctionVA + 0x24;
  Handler.Ops.push_back(std::move(HandlerReturn));

  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
  EH.Encoding = ExceptionEncoding::DwarfFDE;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  EH.Itanium.emplace();
  EH.Itanium->IsCallSiteAddressForm = true;

  ItaniumCallSite Site;
  Site.GuardedRange = {FunctionVA, FunctionVA + 0x10};
  Site.LandingPadVA = FunctionVA + 0x20;
  Site.FirstActionOffset = 1;
  EH.Itanium->CallSites.push_back(Site);

  ItaniumAction Action;
  Action.TableOffset = 1;
  Action.TypeFilter = 1;
  EH.Itanium->Actions.push_back(Action);

  ItaniumTypeEntry Type;
  Type.Index = 1;
  Type.TypeInfoVA = TypeInfoVA;
  Type.TypeInfoSlotVA = TypeInfoSlotVA;
  Type.TypeName = "_ZTI18IndirectCatchType";
  EH.Itanium->TypeTable.push_back(std::move(Type));
  Func.ExceptionMetadata = std::move(EH);

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Func}, Context, "itanium-indirect-typeinfo", Arch::AArch64,
      {{MayThrowVA, "may_throw"}}, nullptr, BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  EXPECT_FALSE(llvm::verifyModule(*Module, &VerificationOS)) << Verification;

  llvm::GlobalVariable *Slot =
      Module->getNamedGlobal(makeNdDataSymbol(TypeInfoSlotVA));
  ASSERT_NE(Slot, nullptr);
  EXPECT_EQ(Module->getNamedGlobal(makeNdDataSymbol(TypeInfoVA)), nullptr);

  llvm::LandingPadInst *LandingPad = nullptr;
  llvm::Function *Emitted = Module->getFunction(Func.Name);
  ASSERT_NE(Emitted, nullptr);
  for (llvm::BasicBlock &Block : *Emitted)
    for (llvm::Instruction &Instruction : Block)
      if (auto *Candidate = llvm::dyn_cast<llvm::LandingPadInst>(&Instruction))
        LandingPad = Candidate;

  ASSERT_NE(LandingPad, nullptr);
  ASSERT_EQ(LandingPad->getNumClauses(), 1u);
  EXPECT_EQ(LandingPad->getClause(0)->stripPointerCasts(), Slot);
}

TEST(ItaniumEHEmitter, LowersPadsThatShareAHandlerThroughANormalEdge) {
  constexpr va_t FunctionVA = 0x100001000;
  constexpr va_t FirstThrowVA = 0x100001100;
  constexpr va_t SecondThrowVA = 0x100001200;
  constexpr va_t BridgeVA = FunctionVA + 0x20;
  constexpr va_t HandlerVA = FunctionVA + 0x30;

  MedFunc Func;
  Func.Entry = FunctionVA;
  Func.Name = "itanium_shared_handler";
  Func.ReturnType = NdType::makeVoid();

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = FunctionVA;
  Protected.EndAddr = FunctionVA + 0x10;
  for (const auto &[Address, Target] :
       {std::pair{FunctionVA + 4, FirstThrowVA},
        std::pair{FunctionVA + 8, SecondThrowVA}}) {
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Address;
    Call.addInput(MedVar::makeConst(Target, 8));
    Protected.Ops.push_back(std::move(Call));
  }
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = FunctionVA + 0xc;
  Protected.Ops.push_back(std::move(ProtectedReturn));

  // The second native landing pad is only a branch into the first pad's
  // shared handler body.  That normal predecessor must not make the first
  // call lose its exceptional edge.
  MedBlock Bridge;
  Bridge.Id = 1;
  Bridge.StartAddr = BridgeVA;
  Bridge.EndAddr = HandlerVA;
  Bridge.Succs.push_back(2);

  MedBlock Handler;
  Handler.Id = 2;
  Handler.StartAddr = HandlerVA;
  Handler.EndAddr = HandlerVA + 0x10;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = HandlerVA;
  Handler.Ops.push_back(std::move(HandlerReturn));

  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Bridge));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, HandlerVA + 0x10};
  EH.Encoding = ExceptionEncoding::DwarfFDE;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::ObjCPersonalityV0;
  EH.Itanium.emplace();
  EH.Itanium->IsCallSiteAddressForm = true;

  ItaniumCallSite FirstSite;
  FirstSite.GuardedRange = {FunctionVA + 4, FunctionVA + 8};
  FirstSite.LandingPadVA = HandlerVA;
  EH.Itanium->CallSites.push_back(FirstSite);

  ItaniumCallSite SecondSite;
  SecondSite.GuardedRange = {FunctionVA + 8, FunctionVA + 0xc};
  SecondSite.LandingPadVA = BridgeVA;
  EH.Itanium->CallSites.push_back(SecondSite);
  Func.ExceptionMetadata = std::move(EH);

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Func}, Context, "itanium-shared-handler", Arch::AArch64,
      {{FirstThrowVA, "first_throw"}, {SecondThrowVA, "second_throw"}}, nullptr,
      BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  EXPECT_FALSE(llvm::verifyModule(*Module, &VerificationOS)) << Verification;

  unsigned Invokes = 0;
  unsigned LandingPads = 0;
  llvm::Function *Emitted = Module->getFunction(Func.Name);
  ASSERT_NE(Emitted, nullptr);
  for (llvm::BasicBlock &Block : *Emitted)
    for (llvm::Instruction &Instruction : Block) {
      Invokes += llvm::isa<llvm::InvokeInst>(Instruction);
      LandingPads += llvm::isa<llvm::LandingPadInst>(Instruction);
      EXPECT_EQ(
          Instruction.getMetadata(language_eh_md::InternalSourceCallAttachment),
          nullptr);
    }
  EXPECT_EQ(Invokes, 2u);
  EXPECT_EQ(LandingPads, 2u);
}

TEST(ItaniumEHEmitter, RejectsAnUnusedCallSiteWithACyclicActionChain) {
  constexpr va_t FunctionVA = 0x100001000;

  MedFunc Func;
  Func.Entry = FunctionVA;
  Func.Name = "itanium_unused_bad_action";
  Func.ReturnType = NdType::makeVoid();
  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = FunctionVA;
  Entry.EndAddr = FunctionVA + 0x10;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = FunctionVA + 4;
  Entry.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Entry));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
  EH.Encoding = ExceptionEncoding::DwarfFDE;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  EH.Itanium.emplace();
  EH.Itanium->IsCallSiteAddressForm = true;
  ItaniumCallSite Site;
  Site.GuardedRange = {FunctionVA + 8, FunctionVA + 0x10};
  Site.LandingPadVA = FunctionVA + 0x20;
  Site.FirstActionOffset = 1;
  EH.Itanium->CallSites.push_back(Site);
  ItaniumAction Cycle;
  Cycle.TableOffset = 1;
  Cycle.TypeFilter = 0;
  Cycle.NextActionOffset = 1;
  EH.Itanium->Actions.push_back(Cycle);
  Func.ExceptionMetadata = std::move(EH);

  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Func}, Context, "unused-bad-action", Arch::AArch64,
                            {}, nullptr, BinaryFormat::ELF);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Emitted = Module->getFunction(Func.Name);
  ASSERT_NE(Emitted, nullptr);
  EXPECT_FALSE(Emitted->hasPersonalityFn());
  EXPECT_EQ(Emitted->getMetadata(language_eh_md::ItaniumAttachment), nullptr);
  EXPECT_TRUE(Module->global_empty());
  unsigned Invokes = 0;
  unsigned LandingPads = 0;
  for (llvm::BasicBlock &Block : *Emitted)
    for (llvm::Instruction &Instruction : Block) {
      Invokes += llvm::isa<llvm::InvokeInst>(Instruction);
      LandingPads += llvm::isa<llvm::LandingPadInst>(Instruction);
    }
  EXPECT_EQ(Invokes, 0u);
  EXPECT_EQ(LandingPads, 0u);

  const llvm::MDNode *Contract =
      Emitted->getMetadata(exception_rewrite::FunctionAttachment);
  const llvm::ConstantInt *Lowering =
      metadataInteger(Contract, exception_rewrite::Lowering, /*Width=*/8);
  ASSERT_NE(Lowering, nullptr);
  EXPECT_EQ(Lowering->getZExtValue(),
            static_cast<uint8_t>(exception_rewrite::LoweringState::Missing));
}

TEST(ItaniumEHEmitter, CompletesAValidSourceWithNoProtectedCalls) {
  constexpr va_t FunctionVA = 0x100001000;

  MedFunc Func;
  Func.Entry = FunctionVA;
  Func.Name = "itanium_zero_protected_calls";
  Func.ReturnType = NdType::makeVoid();
  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = FunctionVA;
  Entry.EndAddr = FunctionVA + 0x10;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = FunctionVA + 4;
  Entry.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Entry));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x40};
  EH.Encoding = ExceptionEncoding::DwarfFDE;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  EH.Itanium.emplace();
  EH.Itanium->IsCallSiteAddressForm = true;
  ItaniumCallSite Site;
  Site.GuardedRange = {FunctionVA + 8, FunctionVA + 0x10};
  Site.LandingPadVA = FunctionVA + 0x20;
  EH.Itanium->CallSites.push_back(Site);
  Func.ExceptionMetadata = std::move(EH);

  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Func}, Context, "zero-protected-calls",
                            Arch::AArch64, {}, nullptr, BinaryFormat::ELF);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Emitted = Module->getFunction(Func.Name);
  ASSERT_NE(Emitted, nullptr);
  EXPECT_FALSE(Emitted->hasPersonalityFn());
  EXPECT_TRUE(Module->global_empty());

  const llvm::MDNode *Native =
      Emitted->getMetadata(language_eh_md::ItaniumAttachment);
  ASSERT_NE(Native, nullptr);
  ASSERT_EQ(Native->getNumOperands(), language_eh_md::ItaniumOperandCount);
  for (unsigned Operand = language_eh_md::LoweredPads;
       Operand < language_eh_md::ItaniumOperandCount; ++Operand) {
    const llvm::ConstantInt *Value = metadataInteger(Native, Operand, 32);
    ASSERT_NE(Value, nullptr) << Operand;
    EXPECT_EQ(Value->getZExtValue(), 0u) << Operand;
  }

  const llvm::MDNode *Contract =
      Emitted->getMetadata(exception_rewrite::FunctionAttachment);
  ASSERT_NE(Contract, nullptr);
  const llvm::ConstantInt *Lowering =
      metadataInteger(Contract, exception_rewrite::Lowering, /*Width=*/8);
  ASSERT_NE(Lowering, nullptr);
  EXPECT_EQ(Lowering->getZExtValue(),
            static_cast<uint8_t>(exception_rewrite::LoweringState::Complete));
  for (unsigned Operand = exception_rewrite::RequiredProtectedCalls;
       Operand <= exception_rewrite::SkippedLandingPads; ++Operand) {
    const llvm::ConstantInt *Value = metadataInteger(Contract, Operand, 64);
    ASSERT_NE(Value, nullptr) << Operand;
    EXPECT_EQ(Value->getZExtValue(), 0u) << Operand;
  }
}

TEST(ItaniumEHEmitter, CountsEveryCallCoveredByOneCallSite) {
  constexpr va_t FunctionVA = 0x100001000;
  constexpr va_t FirstThrowVA = 0x100001100;
  constexpr va_t SecondThrowVA = 0x100001200;
  constexpr va_t HandlerVA = FunctionVA + 0x20;

  MedFunc Func;
  Func.Entry = FunctionVA;
  Func.Name = "itanium_one_site_two_calls";
  Func.ReturnType = NdType::makeVoid();
  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = FunctionVA;
  Protected.EndAddr = FunctionVA + 0x10;
  for (const auto &[Address, Target] :
       {std::pair{FunctionVA + 4, FirstThrowVA},
        std::pair{FunctionVA + 8, SecondThrowVA}}) {
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Address;
    Call.addInput(MedVar::makeConst(Target, 8));
    Protected.Ops.push_back(std::move(Call));
  }
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = FunctionVA + 0xc;
  Protected.Ops.push_back(std::move(Return));
  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = HandlerVA;
  Handler.EndAddr = HandlerVA + 0x10;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = HandlerVA;
  Handler.Ops.push_back(std::move(HandlerReturn));
  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, HandlerVA + 0x10};
  EH.Encoding = ExceptionEncoding::DwarfFDE;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  EH.Itanium.emplace();
  EH.Itanium->IsCallSiteAddressForm = true;
  ItaniumCallSite Site;
  Site.GuardedRange = {FunctionVA, FunctionVA + 0x10};
  Site.LandingPadVA = HandlerVA;
  EH.Itanium->CallSites.push_back(Site);
  Func.ExceptionMetadata = std::move(EH);

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Func}, Context, "one-site-two-calls", Arch::AArch64,
      {{FirstThrowVA, "first_throw"}, {SecondThrowVA, "second_throw"}}, nullptr,
      BinaryFormat::ELF);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Emitted = Module->getFunction(Func.Name);
  ASSERT_NE(Emitted, nullptr);
  unsigned Invokes = 0;
  unsigned LandingPads = 0;
  for (llvm::BasicBlock &Block : *Emitted)
    for (llvm::Instruction &Instruction : Block) {
      Invokes += llvm::isa<llvm::InvokeInst>(Instruction);
      LandingPads += llvm::isa<llvm::LandingPadInst>(Instruction);
    }
  EXPECT_EQ(Invokes, 2u);
  EXPECT_EQ(LandingPads, 1u);

  const llvm::MDNode *Native =
      Emitted->getMetadata(language_eh_md::ItaniumAttachment);
  ASSERT_NE(Native, nullptr);
  for (unsigned Operand :
       {language_eh_md::LoweredCalls, language_eh_md::RequiredProtectedCalls}) {
    const llvm::ConstantInt *Value = metadataInteger(Native, Operand, 32);
    ASSERT_NE(Value, nullptr);
    EXPECT_EQ(Value->getZExtValue(), 2u);
  }
  const llvm::MDNode *Contract =
      Emitted->getMetadata(exception_rewrite::FunctionAttachment);
  ASSERT_NE(Contract, nullptr);
  for (unsigned Operand : {exception_rewrite::RequiredProtectedCalls,
                           exception_rewrite::LoweredProtectedCalls}) {
    const llvm::ConstantInt *Value = metadataInteger(Contract, Operand, 64);
    ASSERT_NE(Value, nullptr);
    EXPECT_EQ(Value->getZExtValue(), 2u);
  }

  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Deep;
  Options.LLVMLevel = llvm::OptimizationLevel::O2;
  OptimizationResult Result = Pipeline::optimizeModule(*Module, Options);
  EXPECT_NE(Result.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  EXPECT_FALSE(llvm::verifyModule(*Module, &VerificationOS)) << Verification;

  Emitted = Module->getFunction(Func.Name);
  ASSERT_NE(Emitted, nullptr);
  EXPECT_TRUE(Emitted->hasPersonalityFn());
  EXPECT_NE(Emitted->getMetadata(language_eh_md::ItaniumAttachment), nullptr);
  EXPECT_NE(Emitted->getMetadata(exception_rewrite::FunctionAttachment),
            nullptr);
  Invokes = 0;
  LandingPads = 0;
  for (llvm::BasicBlock &Block : *Emitted)
    for (llvm::Instruction &Instruction : Block) {
      Invokes += llvm::isa<llvm::InvokeInst>(Instruction);
      LandingPads += llvm::isa<llvm::LandingPadInst>(Instruction);
    }
  EXPECT_EQ(Invokes, 2u);
  EXPECT_EQ(LandingPads, 1u);
}

TEST(ItaniumEHEmitter, LowersAPredicatedCallFromItsSyntheticEffectBlock) {
  constexpr va_t FunctionVA = 0x1000;
  constexpr va_t ContinueVA = FunctionVA + 4;
  constexpr va_t HandlerVA = FunctionVA + 0x20;
  constexpr va_t MayThrowVA = 0x2000;
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::ARM);

  MedFunc Func;
  Func.Entry = FunctionVA;
  Func.Name = "itanium_predicated_call";
  Func.CC = CallingConv::ARM_AAPCS;
  Func.ReturnType = NdType::makeVoid();

  MedVar GuardValue;
  GuardValue.Kind = MedVar::Reg;
  GuardValue.TheArch = Arch::ARM;
  GuardValue.Id = 1;
  GuardValue.Size = 1;
  GuardValue.RegOff = TRI.IntParamRegs.front();
  Func.Params.push_back(GuardValue);

  MedBlock Guarded;
  Guarded.Id = 0;
  Guarded.StartAddr = FunctionVA;
  Guarded.EndAddr = ContinueVA;
  Guarded.Succs = {1};
  MedOp Guard;
  Guard.Opcode = NdOp::COND_BR;
  Guard.Addr = FunctionVA;
  Guard.addInput(MedVar::makeConst(ContinueVA, TRI.PointerSize));
  Guard.addInput(GuardValue);
  Guarded.Ops.push_back(Guard);
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = FunctionVA;
  Call.addInput(MedVar::makeConst(MayThrowVA, TRI.PointerSize));
  Guarded.Ops.push_back(Call);

  MedBlock Continued;
  Continued.Id = 1;
  Continued.StartAddr = ContinueVA;
  Continued.EndAddr = ContinueVA + 4;
  Continued.Preds = {0};
  MedOp ContinuedReturn;
  ContinuedReturn.Opcode = NdOp::RETURN;
  ContinuedReturn.Addr = ContinueVA;
  Continued.Ops.push_back(ContinuedReturn);

  MedBlock Handler;
  Handler.Id = 2;
  Handler.StartAddr = HandlerVA;
  Handler.EndAddr = HandlerVA + 4;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = HandlerVA;
  Handler.Ops.push_back(HandlerReturn);

  Func.Blocks.push_back(std::move(Guarded));
  Func.Blocks.push_back(std::move(Continued));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, HandlerVA + 4};
  EH.Encoding = ExceptionEncoding::DwarfFDE;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  EH.Itanium.emplace();
  EH.Itanium->IsCallSiteAddressForm = true;
  ItaniumCallSite Site;
  Site.GuardedRange = {FunctionVA, ContinueVA};
  Site.LandingPadVA = HandlerVA;
  EH.Itanium->CallSites.push_back(Site);
  Func.ExceptionMetadata = std::move(EH);

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Func}, Context, "itanium-predicated-call", Arch::ARM,
      {{MayThrowVA, "may_throw"}}, nullptr, BinaryFormat::ELF);
  ASSERT_NE(Module, nullptr);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  EXPECT_FALSE(llvm::verifyModule(*Module, &VerificationOS)) << Verification;

  llvm::Function *Emitted = Module->getFunction(Func.Name);
  ASSERT_NE(Emitted, nullptr);
  ASSERT_TRUE(Emitted->hasPersonalityFn());
  auto *GuardBranch = llvm::dyn_cast<llvm::CondBrInst>(
      Emitted->getEntryBlock().getTerminator());
  ASSERT_NE(GuardBranch, nullptr);
  llvm::BasicBlock *Skip = GuardBranch->getSuccessor(0);
  llvm::BasicBlock *Effect = GuardBranch->getSuccessor(1);
  EXPECT_EQ(Skip->getName(), "bb_1");
  EXPECT_TRUE(Effect->getName().starts_with("predeffect_"));

  auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Effect->getTerminator());
  ASSERT_NE(Invoke, nullptr)
      << "the selected predicate path must retain native unwind semantics";
  EXPECT_NE(Invoke->getNormalDest(), Skip);
  auto *Rejoin = llvm::dyn_cast<llvm::UncondBrInst>(
      Invoke->getNormalDest()->getTerminator());
  ASSERT_NE(Rejoin, nullptr);
  EXPECT_EQ(Rejoin->getSuccessor(0), Skip);
  bool HasLandingPad = false;
  for (llvm::Instruction &Instruction : *Invoke->getUnwindDest())
    HasLandingPad |= llvm::isa<llvm::LandingPadInst>(Instruction);
  EXPECT_TRUE(HasLandingPad);

  const llvm::MDNode *Contract =
      Emitted->getMetadata(exception_rewrite::FunctionAttachment);
  ASSERT_NE(Contract, nullptr);
  for (unsigned Operand : {exception_rewrite::RequiredProtectedCalls,
                           exception_rewrite::LoweredProtectedCalls}) {
    const llvm::ConstantInt *Count = metadataInteger(Contract, Operand, 64);
    ASSERT_NE(Count, nullptr);
    EXPECT_EQ(Count->getZExtValue(), 1u);
  }
}

TEST(ItaniumEHEmitter, RejectsDuplicateSourceCallAddressesWithoutMutation) {
  constexpr va_t FunctionVA = 0x100001000;
  constexpr va_t FirstThrowVA = 0x100002000;
  constexpr va_t SecondThrowVA = 0x100003000;
  MedFunc Func = makeSingleCallCleanupFunction("itanium_duplicate_source_call",
                                               FunctionVA, FirstThrowVA);
  MedOp Duplicate;
  Duplicate.Opcode = NdOp::CALL;
  Duplicate.Addr = Func.Blocks.front().Ops.front().Addr;
  Duplicate.addInput(MedVar::makeConst(SecondThrowVA, 8));
  Func.Blocks.front().Ops.insert(Func.Blocks.front().Ops.begin() + 1,
                                 Duplicate);

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Func}, Context, "duplicate-source-call", Arch::AArch64,
      {{FirstThrowVA, "first_throw"}, {SecondThrowVA, "second_throw"}}, nullptr,
      BinaryFormat::ELF);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Emitted = Module->getFunction(Func.Name);
  ASSERT_NE(Emitted, nullptr);
  EXPECT_FALSE(Emitted->hasPersonalityFn());
  EXPECT_EQ(Emitted->getMetadata(language_eh_md::ItaniumAttachment), nullptr);
  unsigned Calls = 0;
  for (llvm::BasicBlock &Block : *Emitted)
    for (llvm::Instruction &Instruction : Block) {
      Calls += llvm::isa<llvm::CallInst>(Instruction);
      EXPECT_FALSE(llvm::isa<llvm::InvokeInst>(Instruction));
      EXPECT_EQ(
          Instruction.getMetadata(language_eh_md::InternalSourceCallAttachment),
          nullptr);
    }
  EXPECT_EQ(Calls, 2u);
}

TEST(ItaniumEHEmitter, KeepsOrderedActionChainsDistinctAtOneHandler) {
  constexpr va_t FunctionVA = 0x100001000;
  constexpr va_t FirstThrowVA = 0x100001100;
  constexpr va_t SecondThrowVA = 0x100001200;
  constexpr va_t HandlerVA = FunctionVA + 0x20;
  constexpr va_t FirstTypeVA = 0x100003000;
  constexpr va_t SecondTypeVA = 0x100003100;

  MedFunc Func;
  Func.Entry = FunctionVA;
  Func.Name = "itanium_ordered_action_chains";
  Func.ReturnType = NdType::makeVoid();
  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = FunctionVA;
  Protected.EndAddr = FunctionVA + 0x10;
  for (const auto &[Address, Target] :
       {std::pair{FunctionVA + 4, FirstThrowVA},
        std::pair{FunctionVA + 8, SecondThrowVA}}) {
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Address;
    Call.addInput(MedVar::makeConst(Target, 8));
    Protected.Ops.push_back(std::move(Call));
  }
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = FunctionVA + 0xc;
  Protected.Ops.push_back(std::move(Return));
  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = HandlerVA;
  Handler.EndAddr = HandlerVA + 0x10;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = HandlerVA;
  Handler.Ops.push_back(std::move(HandlerReturn));
  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, HandlerVA + 0x10};
  EH.Encoding = ExceptionEncoding::DwarfFDE;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  EH.Itanium.emplace();
  EH.Itanium->IsCallSiteAddressForm = true;
  ItaniumCallSite FirstSite;
  FirstSite.GuardedRange = {FunctionVA + 4, FunctionVA + 8};
  FirstSite.LandingPadVA = HandlerVA;
  FirstSite.FirstActionOffset = 1;
  EH.Itanium->CallSites.push_back(FirstSite);
  ItaniumCallSite SecondSite;
  SecondSite.GuardedRange = {FunctionVA + 8, FunctionVA + 0xc};
  SecondSite.LandingPadVA = HandlerVA;
  SecondSite.FirstActionOffset = 3;
  EH.Itanium->CallSites.push_back(SecondSite);
  EH.Itanium->Actions = {
      {1, 1, 2}, {2, 2, std::nullopt}, {3, 2, 4}, {4, 1, std::nullopt}};
  ItaniumTypeEntry FirstType;
  FirstType.Index = 1;
  FirstType.TypeInfoVA = FirstTypeVA;
  ItaniumTypeEntry SecondType;
  SecondType.Index = 2;
  SecondType.TypeInfoVA = SecondTypeVA;
  EH.Itanium->TypeTable.push_back(FirstType);
  EH.Itanium->TypeTable.push_back(SecondType);
  Func.ExceptionMetadata = std::move(EH);

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Func}, Context, "ordered-action-chains", Arch::AArch64,
      {{FirstThrowVA, "first_throw"}, {SecondThrowVA, "second_throw"}}, nullptr,
      BinaryFormat::ELF);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Emitted = Module->getFunction(Func.Name);
  ASSERT_NE(Emitted, nullptr);

  std::vector<std::vector<const llvm::Value *>> ClauseOrders;
  for (llvm::BasicBlock &Block : *Emitted)
    for (llvm::Instruction &Instruction : Block)
      if (auto *LandingPad =
              llvm::dyn_cast<llvm::LandingPadInst>(&Instruction)) {
        std::vector<const llvm::Value *> Clauses;
        for (unsigned I = 0; I < LandingPad->getNumClauses(); ++I)
          Clauses.push_back(LandingPad->getClause(I)->stripPointerCasts());
        ClauseOrders.push_back(std::move(Clauses));
      }
  ASSERT_EQ(ClauseOrders.size(), 2u);
  for (const auto &Clauses : ClauseOrders)
    ASSERT_EQ(Clauses.size(), 2u);
  EXPECT_NE(ClauseOrders[0], ClauseOrders[1]);
  EXPECT_EQ(ClauseOrders[0][0], ClauseOrders[1][1]);
  EXPECT_EQ(ClauseOrders[0][1], ClauseOrders[1][0]);
}

TEST(ItaniumEHEmitter, RejectsOverlappingAddressFormCallSites) {
  constexpr va_t FunctionVA = 0x100001000;
  constexpr va_t MayThrowVA = 0x100002000;
  MedFunc Func = makeSingleCallCleanupFunction("itanium_overlapping_sites",
                                               FunctionVA, MayThrowVA);
  ItaniumCallSite Overlap = Func.ExceptionMetadata->Itanium->CallSites.front();
  Overlap.GuardedRange = {FunctionVA + 2, FunctionVA + 8};
  Func.ExceptionMetadata->Itanium->CallSites.push_back(Overlap);

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Func}, Context, "overlapping-sites", Arch::AArch64,
      {{MayThrowVA, "may_throw"}}, nullptr, BinaryFormat::ELF);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Emitted = Module->getFunction(Func.Name);
  ASSERT_NE(Emitted, nullptr);
  EXPECT_FALSE(Emitted->hasPersonalityFn());
  EXPECT_EQ(Emitted->getMetadata(language_eh_md::ItaniumAttachment), nullptr);
  unsigned Calls = 0;
  unsigned Invokes = 0;
  for (llvm::BasicBlock &Block : *Emitted)
    for (llvm::Instruction &Instruction : Block) {
      Calls += llvm::isa<llvm::CallInst>(Instruction);
      Invokes += llvm::isa<llvm::InvokeInst>(Instruction);
      EXPECT_EQ(
          Instruction.getMetadata(language_eh_md::InternalSourceCallAttachment),
          nullptr);
    }
  EXPECT_EQ(Calls, 1u);
  EXPECT_EQ(Invokes, 0u);
}

TEST(ItaniumEHEmitter, RejectsPersonalitySymbolTypeConflictsBeforeCommit) {
  constexpr va_t FunctionVA = 0x100001000;
  constexpr va_t MayThrowVA = 0x100002000;
  MedFunc Func = makeSingleCallCleanupFunction("__gxx_personality_v0",
                                               FunctionVA, MayThrowVA);

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Func}, Context, "personality-conflict", Arch::AArch64,
      {{MayThrowVA, "may_throw"}}, nullptr, BinaryFormat::ELF);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Emitted = Module->getFunction(Func.Name);
  ASSERT_NE(Emitted, nullptr);
  EXPECT_FALSE(Emitted->hasPersonalityFn());
  EXPECT_EQ(Emitted->getMetadata(language_eh_md::ItaniumAttachment), nullptr);
  for (llvm::BasicBlock &Block : *Emitted)
    for (llvm::Instruction &Instruction : Block)
      EXPECT_FALSE(llvm::isa<llvm::InvokeInst>(Instruction));
}

TEST(ItaniumEHEmitter, RejectsTypeInfoSymbolKindConflictsBeforeCommit) {
  constexpr va_t FunctionVA = 0x100001000;
  constexpr va_t MayThrowVA = 0x100002000;
  constexpr va_t TypeInfoVA = 0x100003000;
  MedFunc Func = makeSingleCallCleanupFunction("itanium_typeinfo_conflict",
                                               FunctionVA, MayThrowVA);
  ItaniumEHInfo &Itanium = *Func.ExceptionMetadata->Itanium;
  Itanium.CallSites.front().FirstActionOffset = 1;
  Itanium.Actions.push_back({1, 1, std::nullopt});
  ItaniumTypeEntry Type;
  Type.Index = 1;
  Type.TypeInfoVA = TypeInfoVA;
  Itanium.TypeTable.push_back(Type);

  // The same linkage name exists, but as a function rather than the byte
  // object a type-info address requires.
  MedFunc Conflict;
  Conflict.Entry = FunctionVA + 0x100;
  Conflict.Name = makeNdDataSymbol(TypeInfoVA);
  Conflict.ReturnType = NdType::makeVoid();
  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = Conflict.Entry;
  Entry.EndAddr = Conflict.Entry + 4;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Conflict.Entry;
  Entry.Ops.push_back(std::move(Return));
  Conflict.Blocks.push_back(std::move(Entry));

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Func, Conflict}, Context, "typeinfo-conflict", Arch::AArch64,
      {{MayThrowVA, "may_throw"}}, nullptr, BinaryFormat::ELF);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Emitted = Module->getFunction(Func.Name);
  ASSERT_NE(Emitted, nullptr);
  EXPECT_FALSE(Emitted->hasPersonalityFn());
  EXPECT_EQ(Emitted->getMetadata(language_eh_md::ItaniumAttachment), nullptr);
  EXPECT_EQ(Module->getNamedGlobal(makeNdDataSymbol(TypeInfoVA)), nullptr);
  for (llvm::BasicBlock &Block : *Emitted)
    for (llvm::Instruction &Instruction : Block)
      EXPECT_FALSE(llvm::isa<llvm::InvokeInst>(Instruction));
}

} // namespace
