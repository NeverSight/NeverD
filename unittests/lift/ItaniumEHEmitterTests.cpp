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

} // namespace
