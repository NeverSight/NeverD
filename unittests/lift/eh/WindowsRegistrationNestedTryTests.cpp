//===- WindowsRegistrationNestedTryTests.cpp - PE32 nested search --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationDirectThrowTestUtils.h"
#include "RegistrationNestedTryTestUtils.h"
#include "RegistrationRethrowTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/ir/med/X86RegistrationCallback.h"
#include "neverd/loader/COFF/COFFLoader.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
#include "llvm/IR/WinEHFrame.h"
#endif
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstdlib>

using namespace neverd;

namespace {
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
void reconstructNestedSearch(bool Secondary, bool Rethrow = false,
                             bool InlineRethrow = false,
                             bool DirectThrow = false) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto It =
      llvm::find_if(Image->ExceptionMetadata.Functions,
                    [](const auto &EH) { return EH.Registration && EH.Cxx; });
  ASSERT_NE(It, Image->ExceptionMetadata.Functions.end());
  const auto &EH = *It;
  registration_test::checkNestedTrySource(*Image, EH);
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  auto Low = CFGBuilder().build(*Image, Decode, EH.CodeRange.Begin,
                                "realigned_source");
  ASSERT_TRUE(Low.RegistrationStates);
  for (const auto &Diagnostic : Low.RegistrationStates->Diagnostics)
    llvm::errs() << Diagnostic << '\n';
  const auto &States = *Low.RegistrationStates;
  ASSERT_TRUE(States.Complete);
  ASSERT_TRUE(States.CxxContinuationsComplete);
  ASSERT_TRUE(States.CxxCatchObjectsComplete);
  ASSERT_TRUE(States.RuntimeObjectAccessesComplete);
  ASSERT_EQ(EH.Cxx->TryBlocks.size(), 2u);
  ASSERT_EQ(EH.Cxx->TryBlocks[0].Handlers.size(), 1u);
  ASSERT_EQ(EH.Cxx->TryBlocks[1].Handlers.size(), 2u);
  ASSERT_EQ(States.CxxCatchObjects.size(), 2u);
  ASSERT_EQ(States.CxxContinuations.size(), 3u);
  EXPECT_EQ(llvm::count_if(
                States.CallFrameEffects,
                [&](const auto &Call) {
                  return States.CalleeContracts[Call.CalleeIndex].isRethrow() ||
                         (Call.RuntimeThrow && Call.RuntimeThrow->isRethrow());
                }),
            unsigned(Rethrow));
  EXPECT_EQ(llvm::count_if(
                States.CalleeContracts,
                [](const auto &Callee) { return Callee.isRuntimeThrow(); }),
            unsigned(InlineRethrow || DirectThrow));
  EXPECT_EQ(llvm::count_if(States.CallFrameEffects,
                           [](const auto &Call) {
                             return Call.RuntimeThrow &&
                                    !Call.RuntimeThrow->isRethrow();
                           }),
            DirectThrow ? 3u + unsigned(Secondary && !Rethrow) : 0u);
  unsigned SecondaryCalls = 0;
  for (const auto &Call : States.CallFrameEffects) {
    const auto State = llvm::find_if(States.Blocks, [&](const auto &Candidate) {
      return Candidate.Range.contains(Call.Address);
    });
    ASSERT_NE(State, States.Blocks.end());
    if (!State->CallbackOnly || !Call.DoesNotReturn)
      continue;
    ++SecondaryCalls;
    ASSERT_EQ(State->CxxSearches.size(), 1u);
    EXPECT_EQ(State->CxxSearches[0].TryIndex, 1u);
    EXPECT_EQ(State->CxxSearches[0].ExitedCatches, 1u);
    const auto Block = llvm::find_if(Low.Blocks, [&](const auto &Candidate) {
      return Candidate.Id == State->BlockId;
    });
    ASSERT_NE(Block, Low.Blocks.end());
    EXPECT_EQ(llvm::count_if(Block->ExceptionalSuccs,
                             [&](const auto &Edge) {
                               return Edge.Kind ==
                                          ExceptionalEdgeKind::CxxCatch &&
                                      Edge.RegionIndex == 1;
                             }),
              2u);
  }
  EXPECT_EQ(SecondaryCalls, unsigned(Secondary));
  ASSERT_TRUE(hasCallerCleanupRegistrationABI(Low, *Image));
  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  if (InlineRethrow || DirectThrow)
    recoverCallAbi(Med, Arch::X86, {}, &*Image);
  inferMedTypes(Med, Arch::X86);
  for (const auto &State : States.Blocks)
    if (State.Reached)
      EXPECT_TRUE(llvm::any_of(Med.Blocks, [&](const auto &Block) {
        return Block.StartAddr == State.Range.Begin &&
               Block.EndAddr == State.Range.End;
      }));
  const auto Throw =
      llvm::find_if(States.CallFrameEffects,
                    [](const auto &Call) { return Call.DoesNotReturn; });
  ASSERT_NE(Throw, States.CallFrameEffects.end());
  const auto High = MedToHighConverter().convert(Med, Arch::X86);
  EXPECT_EQ(High.StructuredExceptionRegions, 2u);
  EXPECT_EQ(High.UnstructuredExceptionRegions, 0u);
  registration_test::checkNestedTryHigh(Med, High);
  if (Secondary) {
    const va_t Catch = EH.Cxx->TryBlocks[0].Handlers[0].HandlerVA;
    ASSERT_TRUE(registrationCallbackRegion(Med, Catch));
    for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto Changed = Med;
      auto State = llvm::find_if(
          Changed.RegistrationStates->Blocks, [&](const auto &Candidate) {
            return Candidate.CallbackOnly &&
                   llvm::any_of(States.CallFrameEffects, [&](const auto &Call) {
                     return Call.DoesNotReturn &&
                            Candidate.Range.contains(Call.Address);
                   });
          });
      ASSERT_NE(State, Changed.RegistrationStates->Blocks.end());
      auto Block = llvm::find_if(Changed.Blocks, [&](const auto &Candidate) {
        return Candidate.StartAddr == State->Range.Begin;
      });
      ASSERT_NE(Block, Changed.Blocks.end());
      auto Effect = llvm::find_if(
          Changed.RegistrationStates->CallFrameEffects, [&](const auto &Call) {
            return State->Range.contains(Call.Address) && Call.DoesNotReturn;
          });
      ASSERT_NE(Effect, Changed.RegistrationStates->CallFrameEffects.end());
      if (Mutation == 0)
        Effect->EndAddress = Effect->Address;
      if (Mutation == 1)
        Effect->EndAddress = Block->EndAddr + 1;
      if (Mutation == 2) {
        MedOp Extra;
        Extra.Opcode = NdOp::STORE;
        Extra.Addr = Effect->EndAddress;
        Extra.NumInputs = 2;
        Extra.Inputs[0] = MedVar::makeConst(Image->Base, 4);
        Extra.Inputs[1] = MedVar::makeConst(1, 4);
        Block->Ops.push_back(Extra);
      }
      if (Mutation == 3)
        Effect->DoesNotReturn = false;
      if (Mutation == 4)
        State->CxxSearches.clear();
      if (Mutation == 5)
        State->CxxSearches[0].ExitedCatches = 0;
      if (Mutation < 4) {
        EXPECT_FALSE(registrationCallbackRegion(Changed, Catch));
      } else {
        const auto Rejected = MedToHighConverter().convert(Changed, Arch::X86);
        EXPECT_LT(Rejected.StructuredExceptionRegions, 2u);
      }
    }
  }
  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Med}, Context, "realigned-source", Arch::X86, {},
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
  registration_test::checkNestedTryEdits(*Parent, EH, *Image, Secondary);
  if (InlineRethrow && !DirectThrow)
    registration_test::checkRuntimeThrowEdits(Med, *Parent, *Image);
  if (DirectThrow)
    registration_test::checkDirectThrowEdits(Med, *Parent, *Image);

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
    ASSERT_EQ(Generated->Cxx->TryBlocks.size(), 2u);
    ASSERT_EQ(Generated->Cxx->TryBlocks[0].Handlers.size(), 1u);
    ASSERT_EQ(Generated->Cxx->TryBlocks[1].Handlers.size(), 2u);
    if (EH.Registration->RealignedFrame)
      ASSERT_TRUE(Generated->Registration->RealignedFrame);
    if (const auto *Receipt =
            std::getenv("NEVERD_REGISTRATION_REALIGNED_RECEIPT")) {
      auto Digest = [](const char *File) {
        auto Buffer = llvm::MemoryBuffer::getFile(File);
        EXPECT_TRUE(bool(Buffer));
        if (!Buffer)
          return std::string();
        return llvm::toHex(llvm::SHA256::hash(llvm::arrayRefFromStringRef(
                               (*Buffer)->getBuffer())),
                           true);
      };
      llvm::json::Object Record{
          {"incoming_reads",
           llvm::count_if(States.IncomingFrameAccesses,
                          [](const auto &A) { return !A.Write; })},
          {"incoming_writes",
           llvm::count_if(States.IncomingFrameAccesses,
                          [](const auto &A) { return A.Write; })},
          {"schema", 1},
          {"secondary_search", Secondary},
          {"rethrow_search", Rethrow},
          {"inline_rethrow", InlineRethrow},
          {"direct_throw", DirectThrow},
          {"evidence", "checked-realigned-source-reconstruction"},
          {"source_frame", EH.Registration->RealignedFrame ? "realigned"
                           : EH.Registration->hasCxxCallbackStack()
                               ? "fixed-displaced"
                               : "direct"},
          {"source_image_sha256", Digest(Path)},
          {"image_sha256", Digest(Output)},
          {"base", Image->Base},
          {"source_begin", EH.CodeRange.Begin - Image->Base},
          {"source_end", EH.CodeRange.End - Image->Base},
          {"generated_begin", Generated->CodeRange.Begin - Image->Base},
          {"generated_end", Generated->CodeRange.End - Image->Base}};
      std::error_code Error;
      llvm::raw_fd_ostream Out(Receipt, Error);
      ASSERT_FALSE(Error) << Error.message();
      Out << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(Record)));
    }
  }
}

TEST(WindowsRegistrationNestedTry, InputPE32ReconstructsNestedSearch) {
  reconstructNestedSearch(false);
}

TEST(WindowsRegistrationNestedTry, InputPE32ReconstructsSecondarySearch) {
  reconstructNestedSearch(true);
}

TEST(WindowsRegistrationNestedTry, InputPE32ReconstructsRethrow) {
  reconstructNestedSearch(true, true);
}
TEST(WindowsRegistrationNestedTry, InputPE32ReconstructsInlineRethrow) {
  reconstructNestedSearch(true, true, true);
}
TEST(WindowsRegistrationNestedTry, InputPE32ReconstructsDirectThrows) {
  reconstructNestedSearch(false, false, false, true);
}
TEST(WindowsRegistrationNestedTry, InputPE32ReconstructsDirectSecondaryThrow) {
  reconstructNestedSearch(true, false, false, true);
}
TEST(WindowsRegistrationNestedTry, InputPE32ReconstructsDirectThrowAndRethrow) {
  reconstructNestedSearch(true, true, true, true);
}
#endif
} // namespace
