//===- WindowsRegistrationCatchContextTests.cpp - Nested catch resumes ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reconstruct a compiler-generated try inside a live reference catch.
//===----------------------------------------------------------------------===//
#include "RegistrationSourceReceiptTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/X86RegistrationCatch.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/ir/med/X86RegistrationCallback.h"
#include "neverd/loader/COFF/COFFLoader.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstdlib>

namespace {
using namespace neverd;

TEST(WindowsRegistrationCatchContext, InputPE32RestoresTheOuterReferenceCatch) {
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Found =
      llvm::find_if(Image->ExceptionMetadata.Functions,
                    [](const auto &EH) { return EH.Registration && EH.Cxx; });
  ASSERT_NE(Found, Image->ExceptionMetadata.Functions.end());
  const auto &EH = *Found;
  ASSERT_EQ(EH.Cxx->TryBlocks.size(), 3u);
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  auto Low = CFGBuilder().build(*Image, Decode, EH.CodeRange.Begin,
                                "catch_context_source");
  ASSERT_TRUE(Low.RegistrationStates);
  const auto &States = *Low.RegistrationStates;
  for (const auto &Message : States.Diagnostics)
    llvm::errs() << Message << '\n';
  ASSERT_TRUE(States.Complete);
  ASSERT_TRUE(States.RuntimeObjectAccessesComplete);
  ASSERT_TRUE(States.CxxContinuationsComplete);
  ASSERT_EQ(States.CxxCatchObjects.size(), 3u);
  ASSERT_EQ(States.CxxContinuations.size(), 4u);
  ASSERT_EQ(
      llvm::count_if(States.CxxContinuations,
                     [](const auto &R) { return R.SavedCallbackVA != 0; }),
      1u);
  ASSERT_TRUE(llvm::any_of(States.Blocks, [](const auto &B) {
    return B.CxxCatchStacks.size() == 1 && B.CxxCatchStacks[0].size() == 2;
  }));
  ASSERT_TRUE(hasCallerCleanupRegistrationABI(Low, *Image));
  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  inferMedTypes(Med, Arch::X86);
  const auto Owners = projectX86RegistrationCatchBlocks(Med);
  ASSERT_TRUE(Owners);
  const auto Parents = projectX86RegistrationCatchParents(Med);
  ASSERT_TRUE(Parents);
  ASSERT_EQ(Parents->size(), 3u);
  EXPECT_EQ((*Parents)[0], (X86RegistrationCatchIdentity{1, 0}));
  EXPECT_FALSE((*Parents)[1]);
  EXPECT_FALSE((*Parents)[2]);
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    auto Changed = Med;
    auto Resume =
        llvm::find_if(Changed.RegistrationStates->CxxContinuations,
                      [](const auto &R) { return R.SavedCallbackVA; });
    ASSERT_NE(Resume, Changed.RegistrationStates->CxxContinuations.end());
    if (Mutation == 0)
      Resume->SavedCallbackVA = 0;
    else if (Mutation == 1)
      Resume->SavedCallbackVA = EH.Cxx->TryBlocks[0].Handlers[0].HandlerVA;
    else
      for (auto &Block : Changed.RegistrationStates->Blocks)
        if (Block.CxxCatchStacks.size() == 1 &&
            Block.CxxCatchStacks[0].size() == 2)
          Block.CxxCatchStacks[0].erase(Block.CxxCatchStacks[0].begin());
    EXPECT_FALSE(projectX86RegistrationCatchParents(Changed));
  }
  unsigned Restored = 0;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.RegistrationRoot ==
          MedOp::RegistrationRootKind::RestoredCallbackStackPointer) {
        const auto Coordinate = registrationCallbackStackCoordinate(Med, Op);
        ASSERT_TRUE(Coordinate);
        EXPECT_EQ(Coordinate->Entry,
                  EH.Cxx->TryBlocks[1].Handlers[0].HandlerVA);
        EXPECT_EQ(Coordinate->Offset, -4);
        ++Restored;
      }
  EXPECT_EQ(Restored, 1u);
  const auto High = MedToHighConverter().convert(Med, Arch::X86);
  EXPECT_EQ(High.StructuredExceptionRegions, 3u);
  EXPECT_EQ(High.UnstructuredExceptionRegions, 0u);
  CEmitterOptions Options;
  Options.TheArch = Arch::X86;
  Options.Format = BinaryFormat::COFF;
  for (bool Structured : {false, true}) {
    Options.StructuredExceptionSyntax = Structured;
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    ASSERT_TRUE(HighCEmitter().emit({High}, Out, Options));
    EXPECT_NE(Text.find("__neverd_x86_callback_esp(0x"), std::string::npos);
    if (!Structured)
      EXPECT_NE(Text.find("Native x86 callback @"), std::string::npos);
  }
  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Med}, Context, "catch-context-source", Arch::X86,
                            {}, &*Image, BinaryFormat::COFF);
  ASSERT_TRUE(Module);
  if (const auto *Output = std::getenv("NEVERD_REGISTRATION_OUTPUT_IR")) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Output, Error);
    ASSERT_FALSE(Error) << Error.message();
    Module->print(Out, nullptr);
  }
  auto *Function = Module->getFunction(Med.Name);
  ASSERT_TRUE(Function);
  ASSERT_TRUE(Function->getMetadata(windows_eh_md::NativeAttachment));
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  auto Proof = validateCOFFRegistrationCxxIR(*Function, EH, *Image);
  ASSERT_FALSE(bool(Proof)) << llvm::toString(std::move(Proof));
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = llvm::CloneModule(*Module);
    auto *Parent = Changed->getFunction(Med.Name);
    llvm::StoreInst *Seed = nullptr;
    llvm::CatchReturnInst *Return = nullptr;
    for (auto &Block : *Parent)
      for (auto &I : Block) {
        if (auto *S = llvm::dyn_cast<llvm::StoreInst>(&I);
            S && S->getMetadata(windows_eh_md::RegistrationRootAttachment) &&
            S->getMetadata(windows_eh_md::RegistrationRootAttachment)
                    ->getNumOperands() == 5)
          Seed = S;
        if (auto *R = llvm::dyn_cast<llvm::CatchReturnInst>(&I);
            R && llvm::isa<llvm::CatchPadInst>(
                     R->getCatchPad()->getCatchSwitch()->getParentPad()))
          Return = R;
      }
    ASSERT_TRUE(Seed && Return);
    auto *Restore = llvm::cast<llvm::StoreInst>(Return->getPrevNode());
    if (Mutation == 0)
      Seed->setOperand(
          0, llvm::ConstantInt::get(Seed->getValueOperand()->getType(), 0));
    else if (Mutation == 1)
      Restore->setOperand(
          0, llvm::ConstantInt::get(Restore->getValueOperand()->getType(), 0));
    else if (Mutation == 2)
      Restore->setVolatile(false);
    else if (Mutation < 6 || Mutation == 7) {
      const auto *Original =
          Seed->getMetadata(windows_eh_md::RegistrationRootAttachment);
      llvm::SmallVector<llvm::Metadata *> Fields;
      for (const auto &Field : Original->operands())
        Fields.push_back(Field.get());
      const unsigned Index = Mutation == 7 ? 1 : Mutation - 1;
      const auto *Value =
          llvm::mdconst::extract<llvm::ConstantInt>(Fields[Index]);
      Fields[Index] = llvm::ConstantAsMetadata::get(
          llvm::ConstantInt::get(Value->getType(), Value->getZExtValue() + 4));
      Seed->setMetadata(windows_eh_md::RegistrationRootAttachment,
                        llvm::MDNode::get(Context, Fields));
    } else
      Seed->setVolatile(true);
    ASSERT_FALSE(llvm::verifyModule(*Changed, &llvm::errs()));
    auto Error = validateCOFFRegistrationCxxIR(*Parent, EH, *Image);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
  if (const auto *Output =
          std::getenv("NEVERD_REGISTRATION_REALIGNED_OUTPUT_PE32")) {
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
        Reloaded->ExceptionMetadata.Functions, [&](const auto &Candidate) {
          return Candidate.CodeRange.Begin == Target;
        });
    ASSERT_NE(Generated, Reloaded->ExceptionMetadata.Functions.end());
    ASSERT_EQ(Generated->ParseStatus, ExceptionParseStatus::Complete);
    ASSERT_TRUE(Generated->Registration);
    ASSERT_TRUE(Generated->Cxx);
    ASSERT_EQ(Generated->Cxx->TryBlocks.size(), 3u);
    ASSERT_EQ(Generated->Cxx->TryBlocks[0].Handlers.size(), 1u);
    ASSERT_EQ(Generated->Cxx->TryBlocks[1].Handlers.size(), 1u);
    ASSERT_EQ(Generated->Cxx->TryBlocks[2].Handlers.size(), 2u);
    registration_test::writeSourceReceipt(
        Path, Output, *Image, EH, States, *Generated,
        llvm::json::Object{{"secondary_search", false},
                           {"rethrow_search", false},
                           {"inline_rethrow", false},
                           {"direct_throw", false},
                           {"catch_try", true}});
  }
#else
  GTEST_SKIP() << "LLVM PE32 C++ catch subfields unavailable";
#endif
}
} // namespace
