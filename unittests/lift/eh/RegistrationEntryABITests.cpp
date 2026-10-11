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

#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/ir/med/X86RegistrationEntry.h"
#include "neverd/loader/COFF/COFFLoader.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstdlib>

namespace {
using namespace neverd;

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
