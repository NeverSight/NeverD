//===- RegistrationEntryABITests.cpp - PE32 mixed entry conventions ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reconstruct real compiler entries and reject edited return/parameter ABIs.
//===----------------------------------------------------------------------===//
#include "RegistrationSourceReceiptTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/low/RegistrationCallEffects.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/ir/med/X86/X86RegistrationEntry.h"
#include "neverd/loader/COFF/COFFLoader.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstdlib>

namespace {
using namespace neverd;

TEST(RegistrationEntryABI, InputPE32NonReturningCleanupHasNoReturnStack) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_NORETURN_PE32");
  if (!Path)
    GTEST_SKIP() << "requires the non-returning cleanup-only PE32 fixture";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Found =
      llvm::find_if(Image->ExceptionMetadata.Functions, [](const auto &F) {
        return F.Cxx && F.Cxx->TryBlocks.empty();
      });
  ASSERT_NE(Found, Image->ExceptionMetadata.Functions.end());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  const auto Low = CFGBuilder().build(*Image, Decode, Found->CodeRange.Begin,
                                      "nonreturning_cleanup");
  ASSERT_TRUE(Low.RegistrationStates);
  ASSERT_EQ(getCheckedX86RegistrationCxxParentABI(Low, *Image), 0);
  EXPECT_FALSE(getCheckedX86CalleeStackPop(*Image, Low.Entry));
  const auto Callee = getCheckedX86RegistrationCxxCalleeABI(*Image, Low.Entry);
  ASSERT_TRUE(Callee);
  EXPECT_TRUE(Callee->isCxxParent());
  EXPECT_TRUE(Callee->DoesNotReturn);
  EXPECT_FALSE(Callee->HasIndependentScalarReturn);
  EXPECT_EQ(Callee->StackPopBytes, 0u);
  ASSERT_FALSE(Callee->RequiredImports.empty());
  ASSERT_FALSE(Callee->RuntimeThrowInfos.empty());
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    auto Edited = *Callee;
    if (Mutation == 0)
      Edited.DoesNotReturn = false;
    else if (Mutation == 1)
      Edited.HasIndependentScalarReturn = true;
    else
      Edited.RequiredImports.clear();
    size_t Work = 0;
    EXPECT_FALSE(hasSameX86RegistrationCxxEffects(Edited, *Callee, Work));
  }
  for (bool NoReturn : {false, true}) {
    const auto Name =
        NoReturn ? "cleanup_only_forward" : "cleanup_only_returning_wrapper";
    SCOPED_TRACE(Name);
    const auto Export = llvm::find_if(
        Image->Exports, [&](const auto &E) { return E.Name == Name; });
    ASSERT_NE(Export, Image->Exports.end());
    const auto Nested =
        getCheckedX86RegistrationCxxCalleeABI(*Image, Export->Addr);
    ASSERT_TRUE(Nested);
    EXPECT_EQ(Nested->DoesNotReturn, NoReturn);
    EXPECT_EQ(Nested->HasIndependentScalarReturn, !NoReturn);
    EXPECT_FALSE(Nested->RequiredImports.empty());
    EXPECT_FALSE(Nested->RuntimeThrowInfos.empty());
    const auto Pop = getCheckedX86CalleeStackPop(*Image, Export->Addr);
    EXPECT_EQ(Pop, NoReturn ? std::nullopt : std::optional<uint32_t>(0));
    const auto Body = CFGBuilder().build(*Image, Decode, Export->Addr, Name);
    EXPECT_EQ(getCheckedX86RegistrationCxxParentABI(Body, *Image), 0);
  }
  auto Changed = Low;
  Changed.CalleePopBytes = 65532;
  EXPECT_EQ(getCheckedX86RegistrationCxxParentABI(Changed, *Image), 0);
  const auto &States = *Low.RegistrationStates;
  const auto Call = llvm::find_if(
      States.CallFrameEffects, [](const auto &C) { return C.DoesNotReturn; });
  ASSERT_NE(Call, States.CallFrameEffects.end());
  const size_t Index = Call - States.CallFrameEffects.begin();
  ASSERT_LT(Call->CalleeIndex, States.CalleeContracts.size());
  for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
    SCOPED_TRACE(Mutation);
    Changed = Low;
    auto &Proof = *Changed.RegistrationStates;
    auto &Effect = Proof.CallFrameEffects[Index];
    auto &Contract = Proof.CalleeContracts[Effect.CalleeIndex];
    switch (Mutation) {
    case 0:
      Effect.DoesNotReturn = false;
      break;
    case 1:
      ++Effect.EndAddress;
      break;
    case 2:
      Effect.CalleeIndex = Proof.CalleeContracts.size();
      break;
    case 3:
      ++Effect.Target;
      break;
    case 4:
      Effect.StackPopBytes = 4;
      break;
    case 5:
      Contract.DoesNotReturn = false;
      break;
    case 6:
      Contract.StackPopBytes = 4;
      break;
    case 7:
      Contract.CalleeKind = RegistrationCalleeFrameContract::Kind::Leaf;
      break;
    case 8:
      Contract.CodeRanges.clear();
      break;
    case 9:
      Proof.IncomingFrameAccesses.emplace_back();
      break;
    case 10:
      Proof.IncomingFrameAccessesComplete = false;
      break;
    case 11:
      Proof.CallFrameEffectsComplete = false;
      break;
    case 12:
      Proof.CxxContinuations.emplace_back();
      break;
    case 13:
      Changed.ExceptionMetadata->Cxx->TryBlocks.emplace_back();
      break;
    case 14:
      for (auto &State : Proof.Blocks)
        if (State.Range.Begin == Low.Entry)
          State.CallbackOnly = true;
      break;
    }
    EXPECT_FALSE(getCheckedX86RegistrationCxxParentABI(Changed, *Image));
  }
  unsigned Branches = 0;
  for (size_t B = 0; B != Low.Blocks.size(); ++B) {
    const auto &Block = Low.Blocks[B];
    if (Block.InstructionBoundaries.back().Control !=
        LowInstructionControl::Branch)
      continue;
    ++Branches;
    Changed = Low;
    Changed.Blocks[B].Succs.clear();
    EXPECT_FALSE(getCheckedX86RegistrationCxxParentABI(Changed, *Image));
    Changed = Low;
    Changed.Blocks[B].Succs.push_back(-1);
    EXPECT_FALSE(getCheckedX86RegistrationCxxParentABI(Changed, *Image));
    Changed = Low;
    Changed.Blocks[B].InstructionBoundaries.back().Immediate.reset();
    EXPECT_FALSE(getCheckedX86RegistrationCxxParentABI(Changed, *Image));
  }
  EXPECT_GT(Branches, 0u);
  // An edited caller opcode or a now-returning helper cannot reuse the saved
  // DoesNotReturn bit, source boundaries or callee code-range receipt.
  for (va_t Address : {Call->Address, Call->Target}) {
    SCOPED_TRACE(Address);
    auto Edited = *Image;
    unsigned Replacements = 0;
    for (auto &Segment : Edited.Segments)
      if (Address >= Segment.VA && Address - Segment.VA < Segment.Data.size()) {
        Segment.Data[Address - Segment.VA] = 0xc3;
        ++Replacements;
      }
    ASSERT_EQ(Replacements, 1u);
    EXPECT_FALSE(getCheckedX86RegistrationCxxParentABI(Low, Edited));
  }
}

TEST(RegistrationEntryABI, InputPE32NonReturningCalleeKeepsOuterCatch) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_NORETURN_PE32");
  if (!Path)
    GTEST_SKIP() << "requires the non-returning cleanup-only PE32 fixture";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto InnerEH =
      llvm::find_if(Image->ExceptionMetadata.Functions, [](const auto &F) {
        return F.Cxx && F.Cxx->TryBlocks.empty();
      });
  const auto OuterEH =
      llvm::find_if(Image->ExceptionMetadata.Functions, [](const auto &F) {
        return F.Cxx && !F.Cxx->TryBlocks.empty();
      });
  ASSERT_NE(InnerEH, Image->ExceptionMetadata.Functions.end());
  ASSERT_NE(OuterEH, Image->ExceptionMetadata.Functions.end());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  auto Inner = CFGBuilder().build(*Image, Decode, InnerEH->CodeRange.Begin,
                                  "nonreturning_inner");
  auto Outer =
      CFGBuilder().build(*Image, Decode, OuterEH->CodeRange.Begin,
                         "sub_" + llvm::utohexstr(OuterEH->CodeRange.Begin));
  ASSERT_TRUE(Outer.RegistrationStates);
  for (const auto &Diagnostic : Outer.RegistrationStates->Diagnostics)
    llvm::errs() << Diagnostic << '\n';
  ASSERT_TRUE(Outer.RegistrationStates->CallFrameEffectsComplete);
  const auto Effect =
      llvm::find_if(Outer.RegistrationStates->CallFrameEffects,
                    [&](const auto &C) { return C.Target == Inner.Entry; });
  ASSERT_NE(Effect, Outer.RegistrationStates->CallFrameEffects.end());
  EXPECT_TRUE(Effect->DoesNotReturn);
  ASSERT_EQ(getCheckedX86RegistrationCxxParentABI(Outer, *Image), 0);
  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Image);
  auto InnerMed = Converter.convert(Inner, Arch::X86, BinaryFormat::COFF);
  auto OuterMed = Converter.convert(Outer, Arch::X86, BinaryFormat::COFF);
  inferMedTypes(InnerMed, Arch::X86);
  inferMedTypes(OuterMed, Arch::X86);
  recoverCallAbi(InnerMed, Arch::X86, {}, &*Image);
  recoverCallAbi(OuterMed, Arch::X86, {}, &*Image);
  for (unsigned Order = 0; Order != 3; ++Order) {
    SCOPED_TRACE(Order);
    llvm::LLVMContext Context;
    std::vector<MedFunc> Functions =
        Order == 0   ? std::vector<MedFunc>{InnerMed, OuterMed}
        : Order == 1 ? std::vector<MedFunc>{OuterMed, InnerMed}
                     : std::vector<MedFunc>{OuterMed};
    auto Module =
        MedLLVMEmitter().emit(Functions, Context, "nonreturning-call",
                              Arch::X86, {}, &*Image, BinaryFormat::COFF);
    ASSERT_TRUE(Module);
    auto *Parent = Module->getFunction(OuterMed.Name);
    ASSERT_TRUE(Parent);
    ASSERT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Proof = validateCOFFRegistrationCxxIR(*Parent, *OuterEH, *Image);
    ASSERT_FALSE(bool(Proof)) << llvm::toString(std::move(Proof));
    if (Order != 2) {
      auto *Child = Module->getFunction(InnerMed.Name);
      ASSERT_TRUE(Child);
      ASSERT_TRUE(Child->getMetadata(windows_eh_md::NativeAttachment));
      auto ChildProof = validateCOFFRegistrationCxxIR(*Child, *InnerEH, *Image);
      ASSERT_FALSE(bool(ChildProof)) << llvm::toString(std::move(ChildProof));
    }
    unsigned Calls = 0;
    for (auto &Block : *Parent)
      for (auto &Instruction : Block) {
        auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
        if (!Call || !Call->getCalledFunction())
          continue;
        auto Target = rewrite_source::getOriginalVA(*Call->getCalledFunction());
        if (!Target) {
          llvm::consumeError(Target.takeError());
          continue;
        }
        if (*Target != Inner.Entry)
          continue;
        ++Calls;
        EXPECT_TRUE(llvm::isa<llvm::InvokeInst>(Call));
        EXPECT_TRUE(Call->doesNotReturn());
        EXPECT_FALSE(Call->doesNotThrow());
        EXPECT_TRUE(Call->getType()->isVoidTy());
        for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
          if (Mutation == 0) {
            // CallBase inherits declaration attributes. Remove both copies
            // to actually withdraw the non-returning invocation contract.
            Call->removeFnAttr(llvm::Attribute::NoReturn);
            Call->getCalledFunction()->removeFnAttr(llvm::Attribute::NoReturn);
          } else if (Mutation == 1)
            Call->getCalledFunction()->removeFnAttr(llvm::Attribute::NoReturn);
          else
            Call->setDoesNotThrow();
          ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
          auto Rejected =
              validateCOFFRegistrationCxxIR(*Parent, *OuterEH, *Image);
          EXPECT_TRUE(bool(Rejected));
          llvm::consumeError(std::move(Rejected));
          Call->setDoesNotReturn();
          Call->getCalledFunction()->addFnAttr(llvm::Attribute::NoReturn);
          Call->removeFnAttr(llvm::Attribute::NoUnwind);
        }
      }
    EXPECT_EQ(Calls, 1u);
    if (Order == 2)
      if (const auto *Output =
              std::getenv("NEVERD_REGISTRATION_NORETURN_OUTER_IR")) {
        std::error_code Error;
        llvm::raw_fd_ostream Stream(Output, Error);
        ASSERT_FALSE(Error) << Error.message();
        Module->print(Stream, nullptr);
      }
  }
}

TEST(RegistrationEntryABI, InputPE32PreservesPhysicalArgumentsAndCleanup) {
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
  const auto *Path = std::getenv("NEVERD_REGISTRATION_ENTRY_PE32");
  const auto *Pop = std::getenv("NEVERD_REGISTRATION_ENTRY_POP");
  const auto *Registers = std::getenv("NEVERD_REGISTRATION_ENTRY_REGISTERS");
  if (!Path || !Pop || !Registers)
    GTEST_SKIP() << "set the registration entry image, pop and register count";
  const unsigned ExpectedPop = std::strtoul(Pop, nullptr, 0);
  const unsigned ExpectedRegisters = std::strtoul(Registers, nullptr, 0);
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Found = llvm::find_if(Image->ExceptionMetadata.Functions,
                                   [](const auto &F) { return F.Cxx; });
  ASSERT_NE(Found, Image->ExceptionMetadata.Functions.end());
  const auto &EH = *Found;
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  auto Low = CFGBuilder().build(*Image, Decode, EH.CodeRange.Begin,
                                "registration_entry_source");
  ASSERT_TRUE(Low.RegistrationStates);
  for (const auto &Diagnostic : Low.RegistrationStates->Diagnostics)
    llvm::errs() << Diagnostic << '\n';
  ASSERT_EQ(getCheckedX86RegistrationCxxParentABI(Low, *Image), ExpectedPop);
  auto Changed = Low;
  Changed.CalleePopBytes = 65532;
  EXPECT_EQ(getCheckedX86RegistrationCxxParentABI(Changed, *Image),
            ExpectedPop);
  Changed.RegistrationStates->CxxContinuationsComplete = false;
  EXPECT_FALSE(getCheckedX86RegistrationCxxParentABI(Changed, *Image));
  unsigned Returns = 0;
  for (size_t B = 0; B != Low.Blocks.size(); ++B)
    for (size_t I = 0; I != Low.Blocks[B].InstructionBoundaries.size(); ++I) {
      const auto &Boundary = Low.Blocks[B].InstructionBoundaries[I];
      if (Boundary.Control != LowInstructionControl::Return)
        continue;
      ++Returns;
      Changed = Low;
      Changed.Blocks[B].InstructionBoundaries[I].Immediate =
          Boundary.Immediate.value_or(0) + 4;
      EXPECT_FALSE(getCheckedX86RegistrationCxxParentABI(Changed, *Image));
    }
  EXPECT_GE(Returns, 4u);

  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  inferMedTypes(Med, Arch::X86);
  const auto ABI = projectX86RegistrationEntry(Med);
  ASSERT_TRUE(ABI);
  EXPECT_EQ(ABI->RegisterCount, ExpectedRegisters);
  EXPECT_EQ(ABI->PopBytes, ExpectedPop);
  EXPECT_EQ(Med.Params.size(), 4u);
  const auto High = MedToHighConverter().convert(Med, Arch::X86);
  EXPECT_EQ(High.StructuredExceptionRegions, 2u);
  EXPECT_EQ(High.UnstructuredExceptionRegions, 0u);
  for (unsigned Mutation = 0; Mutation != 2; ++Mutation) {
    auto Edited = Med;
    auto &States = *Edited.RegistrationStates;
    ASSERT_FALSE(States.CxxContinuations.empty());
    if (Mutation == 0)
      States.CxxContinuationsComplete = false;
    else
      States.CxxContinuations.front().TargetVA = Edited.Entry;
    const auto Rejected = MedToHighConverter().convert(Edited, Arch::X86);
    EXPECT_LT(Rejected.StructuredExceptionRegions, 2u);
  }
  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Med}, Context, "registration-entry", Arch::X86, {},
                            &*Image, BinaryFormat::COFF);
  ASSERT_TRUE(Module);
  if (const auto *Output = std::getenv("NEVERD_REGISTRATION_OUTPUT_IR")) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Output, Error);
    ASSERT_FALSE(Error) << Error.message();
    Module->print(Out, nullptr);
  }
  auto *Parent = Module->getFunction(Med.Name);
  ASSERT_TRUE(Parent);
  ASSERT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  auto Proof = validateCOFFRegistrationCxxIR(*Parent, EH, *Image);
  ASSERT_FALSE(bool(Proof)) << llvm::toString(std::move(Proof));
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto Edited = llvm::CloneModule(*Module);
    auto *Function = Edited->getFunction(Med.Name);
    if (Mutation == 0)
      Function->setCallingConv(Parent->getCallingConv() == llvm::CallingConv::C
                                   ? llvm::CallingConv::X86_StdCall
                                   : llvm::CallingConv::C);
    if (Mutation == 1) {
      if (ExpectedRegisters == 2)
        Function->removeParamAttr(0, llvm::Attribute::InReg);
      else
        Function->addParamAttr(0, llvm::Attribute::InReg);
    }
    if (Mutation == 2)
      Function->addParamAttr(1, llvm::Attribute::SExt);
    if (Mutation == 3) {
      if (ExpectedRegisters == 2)
        Function->removeParamAttr(1, llvm::Attribute::InReg);
      else
        Function->addParamAttr(1, llvm::Attribute::InReg);
    }
    if (Mutation == 4)
      Function->addParamAttr(2, llvm::Attribute::InReg);
    ASSERT_FALSE(llvm::verifyModule(*Edited, &llvm::errs()));
    auto Rejected = validateCOFFRegistrationCxxIR(*Function, EH, *Image);
    EXPECT_TRUE(bool(Rejected)) << Mutation;
    llvm::consumeError(std::move(Rejected));
  }
  if (const auto *Output = std::getenv("NEVERD_REGISTRATION_ENTRY_OUTPUT")) {
    COFFPatcher Patcher;
    Patcher.setImageContext(&*Image);
    const auto Result = Patcher.patch(Path, Output, *Module, Arch::X86);
    ASSERT_TRUE(Result.Success);
    EXPECT_EQ(Result.TrampolineCount, 1u);
    EXPECT_EQ(Result.PatchedOriginalEntries,
              (std::vector<va_t>{EH.CodeRange.Begin}));
    auto Reloaded = COFFLoader().load(Output);
    ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
    const auto *Trampoline = Reloaded->readVA(EH.CodeRange.Begin, 5);
    ASSERT_TRUE(Trampoline);
    ASSERT_EQ(Trampoline[0], 0xe9);
    const va_t Target =
        uint32_t(EH.CodeRange.Begin + 5 +
                 llvm::support::endian::read32le(Trampoline + 1));
    const auto Generated = llvm::find_if(
        Reloaded->ExceptionMetadata.Functions,
        [&](const auto &F) { return F.CodeRange.Begin == Target; });
    ASSERT_NE(Generated, Reloaded->ExceptionMetadata.Functions.end());
    ASSERT_EQ(Generated->ParseStatus, ExceptionParseStatus::Complete);
    ASSERT_TRUE(Generated->Registration && Generated->Cxx);
    // Check the emitted parent epilogue independently of its LLVM signature.
    // Runtime callbacks are separate roots and have their own RET convention.
    Decoder GeneratedDecoder;
    ASSERT_TRUE(GeneratedDecoder.init(*Reloaded));
    const auto GeneratedLow = CFGBuilder().build(*Reloaded, GeneratedDecoder,
                                                 Target, "generated_entry");
    std::map<int, const LowBlock *> Blocks;
    std::vector<int> Pending;
    for (const auto &Block : GeneratedLow.Blocks) {
      ASSERT_TRUE(Blocks.emplace(Block.Id, &Block).second);
      if (Block.StartAddr == Target)
        Pending.push_back(Block.Id);
    }
    ASSERT_EQ(Pending.size(), 1u);
    std::set<int> Seen;
    unsigned ParentReturns = 0;
    while (!Pending.empty()) {
      const auto Id = Pending.back();
      Pending.pop_back();
      ASSERT_TRUE(Blocks.count(Id));
      if (!Seen.insert(Id).second)
        continue;
      const auto &Block = *Blocks.at(Id);
      for (const auto &Boundary : Block.InstructionBoundaries)
        if (Boundary.Control == LowInstructionControl::Return) {
          ++ParentReturns;
          EXPECT_EQ(Boundary.Immediate.value_or(0), ExpectedPop);
        }
      Pending.insert(Pending.end(), Block.Succs.begin(), Block.Succs.end());
    }
    EXPECT_NE(ParentReturns, 0u);
    registration_test::writeSourceReceipt(
        Path, Output, *Image, EH, *Low.RegistrationStates, *Generated,
        llvm::json::Object{{"entry_pop", ExpectedPop},
                           {"entry_registers", ExpectedRegisters}});
  }
#else
  GTEST_SKIP() << "LLVM lacks native PE32 C++ contracts";
#endif
}
} // namespace
