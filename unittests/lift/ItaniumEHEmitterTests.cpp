//===- ItaniumEHEmitterTests.cpp - Native Itanium EH emission tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace {

using namespace neverd;

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
  auto Module =
      MedLLVMEmitter().emit({Func}, Context, "itanium-indirect-typeinfo",
                            Arch::AArch64, {{MayThrowVA, "may_throw"}}, nullptr,
                            BinaryFormat::MachO);
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
    }
  EXPECT_EQ(Invokes, 2u);
  EXPECT_EQ(LandingPads, 2u);
}

} // namespace
