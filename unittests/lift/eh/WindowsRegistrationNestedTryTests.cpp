//===- WindowsRegistrationNestedTryTests.cpp - PE32 nested search --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationDirectThrowTestUtils.h"
#include "RegistrationNestedTryTestUtils.h"
#include "RegistrationRethrowTestUtils.h"
#include "RegistrationSourceReceiptTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/low/RegistrationCxxState.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/ir/med/X86/X86RegistrationCallback.h"
#include "neverd/loader/COFF/COFFLoader.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
#include "llvm/IR/WinEHFrame.h"
#endif
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
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
                                      Edge.singleContext() &&
                                      Edge.singleContext()->RegionIndex == 1;
                             }),
              2u);
  }
  EXPECT_EQ(SecondaryCalls, unsigned(Secondary));
  ASSERT_TRUE(hasCallerCleanupRegistrationABI(Low, *Image));
  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  // Ordinary catch members need the same physical receiver recovery as the
  // production pipeline, even when the throw itself remains in a private
  // helper.
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
  // Source contracts describe checked code. Flipping a return fact cannot
  // authorize an observed pointer or erase a current scalar result proof.
  for (size_t I = 0; I != States.CalleeContracts.size(); ++I) {
    if (States.CalleeContracts[I].CalleeKind !=
            RegistrationCalleeFrameContract::Kind::Leaf ||
        llvm::none_of(States.CallFrameEffects,
                      [&](const auto &Call) { return Call.CalleeIndex == I; }))
      continue;
    auto Changed = Med;
    auto &Contract = Changed.RegistrationStates->CalleeContracts[I];
    Contract.HasIndependentScalarReturn = !Contract.HasIndependentScalarReturn;
    llvm::LLVMContext ChangedContext;
    auto Rejected =
        MedLLVMEmitter().emit({Changed}, ChangedContext, "changed-return",
                              Arch::X86, {}, &*Image, BinaryFormat::COFF);
    ASSERT_TRUE(Rejected);
    const auto *ChangedParent = Rejected->getFunction(Changed.Name);
    ASSERT_TRUE(ChangedParent);
    EXPECT_FALSE(ChangedParent->getMetadata(windows_eh_md::NativeAttachment));
  }
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
    registration_test::writeSourceReceipt(
        Path, Output, *Image, EH, States, *Generated,
        llvm::json::Object{{"secondary_search", Secondary},
                           {"rethrow_search", Rethrow},
                           {"inline_rethrow", InlineRethrow},
                           {"direct_throw", DirectThrow},
                           {"catch_try", false}});
  }
}

TEST(WindowsRegistrationNestedTry, InputPE32ReconstructsNestedSearch) {
  reconstructNestedSearch(false);
}

TEST(WindowsRegistrationNestedTry, InputPE32ReconstructsSecondarySearch) {
  reconstructNestedSearch(true);
}

TEST(WindowsRegistrationNestedTry, InputPE32DispatchRequiresCurrentSources) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32";
  auto Loaded = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Loaded));
  const auto EH =
      llvm::find_if(Loaded->ExceptionMetadata.Functions,
                    [](const auto &F) { return F.Cxx && F.Registration; });
  ASSERT_NE(EH, Loaded->ExceptionMetadata.Functions.end());
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Loaded));
  const auto Source = CFGBuilder().build(*Loaded, Dec, EH->CodeRange.Begin,
                                         "dispatch-binding-source");
  ASSERT_TRUE(Source.RegistrationStates);
  const auto &States = *Source.RegistrationStates;
  ASSERT_TRUE(States.SourceCallFrameEffectsComplete);
  const auto Secondary =
      llvm::find_if(States.CallFrameEffects, [&](const auto &Call) {
        return Call.DoesNotReturn &&
               llvm::any_of(States.Blocks, [&](const auto &Block) {
                 return Block.CallbackOnly &&
                        Block.Range.contains(Call.Address);
               });
      });
  if (Secondary == States.CallFrameEffects.end())
    GTEST_SKIP() << "requires a scalar secondary-throw fixture";
  const auto Primary =
      llvm::find_if(States.CalleeContracts, [](const auto &Call) {
        return Call.CalleeKind ==
               RegistrationCalleeFrameContract::Kind::PrivateThrow;
      });
  ASSERT_NE(Primary, States.CalleeContracts.end());
  const auto Throw =
      getCheckedX86RegistrationThrowCalleeABI(*Loaded, Primary->Target);
  ASSERT_TRUE(Throw);
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = *Loaded;
    auto Changed = Source;
    Changed.RegistrationStates.reset();
    auto ChangeByte = [&](va_t Address) {
      auto Segment = llvm::find_if(Image.Segments, [&](const auto &S) {
        return Address >= S.VA && Address - S.VA < S.Data.size();
      });
      ASSERT_NE(Segment, Image.Segments.end());
      Segment->Data[Address - Segment->VA] ^= 1;
    };
    if (Mutation == 1)
      ChangeByte(Source.Entry);
    if (Mutation == 2)
      Changed.Blocks.front().Ops.front().Opcode = NdOp::NOP;
    if (Mutation == 3)
      Changed.ExceptionMetadata->Cxx->IsSynchronous = false;
    if (Mutation == 4)
      ChangeByte(Throw->ThrowInfo.Address + 4); // Changed destructor metadata.
    if (Mutation == 5)
      for (auto &Import : Image.Imports)
        if (Import.Name == "__CxxFrameHandler3")
          Import.Module = "application.dll";
    if (Mutation == 6)
      for (auto &Segment : Image.Segments)
        if (Segment.isWritable())
          Segment.Flags = SegmentFlags::Readable;
    if (Mutation == 7)
      for (auto &Segment : Image.Segments)
        if (Segment.isExecutable())
          Segment.Flags = Segment.Flags | SegmentFlags::Writable;
    if (Mutation == 8)
      std::reverse(Changed.Blocks.begin(), Changed.Blocks.end());
    const auto Current = analyzeCheckedX86CxxStates(
        Image, Changed, &States.CalleeContracts, &States.CleanupContracts);
    EXPECT_EQ(llvm::any_of(Current.CallFrameEffects,
                           [&](const auto &Call) {
                             return Call.Address == Secondary->Address;
                           }),
              Mutation == 0 || Mutation == 8);
  }
}

TEST(WindowsRegistrationNestedTry,
     InputPE32CleanupDispatchRequiresCurrentEffects) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32";
  auto Loaded = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Loaded));
  const auto EH =
      llvm::find_if(Loaded->ExceptionMetadata.Functions,
                    [](const auto &F) { return F.Cxx && F.Registration; });
  ASSERT_NE(EH, Loaded->ExceptionMetadata.Functions.end());
  if (llvm::none_of(EH->Cxx->UnwindMap,
                    [](const auto &A) { return A.ActionVA != 0; }))
    GTEST_SKIP() << "requires a catch-cleanup fixture";
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Loaded));
  const auto Source = CFGBuilder().build(*Loaded, Dec, EH->CodeRange.Begin,
                                         "cleanup-dispatch-source");
  ASSERT_TRUE(Source.RegistrationStates);
  const auto &States = *Source.RegistrationStates;
  ASSERT_TRUE(States.SourceCallFrameEffectsComplete);
  ASSERT_TRUE(States.CleanupFrameEffectsComplete);
  ASSERT_FALSE(States.CleanupContracts.empty());
  const auto &Cleanup = States.CleanupContracts.front();
  ASSERT_FALSE(Cleanup.Calls.empty());
  const auto &Leaf = Cleanup.Calls.front().Leaf;
  ASSERT_FALSE(Leaf.ImageWrites.empty());
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = *Loaded;
    auto Changed = Source;
    Changed.RegistrationStates.reset();
    auto Cleanups = States.CleanupContracts;
    auto ChangeByte = [&](va_t Address) {
      auto Segment = llvm::find_if(Image.Segments, [&](const auto &S) {
        return Address >= S.VA && Address - S.VA < S.Data.size();
      });
      ASSERT_NE(Segment, Image.Segments.end());
      Segment->Data[Address - Segment->VA] ^= 1;
    };
    if (Mutation == 1)
      ChangeByte(Cleanup.RelayTarget);
    if (Mutation == 2)
      ChangeByte(Leaf.Target);
    if (Mutation == 3)
      ++Cleanups.front().Calls.front().ObjectFrameOffset;
    if (Mutation == 4)
      Cleanups.front().Calls.front().Leaf.ECXReads.clear();
    if (Mutation == 5)
      for (auto &Section : Image.Sections)
        if (Section.VA <= Leaf.ImageWrites.front().Begin &&
            Leaf.ImageWrites.front().Begin - Section.VA < Section.Size)
          Section.Flags = SegmentFlags::Readable;
    if (Mutation == 6) {
      Import Other;
      Other.Module = "application.dll";
      Other.Name = "callback";
      Other.IATAddr = Leaf.ImageWrites.front().Begin;
      Image.Imports.push_back(Other);
    }
    if (Mutation == 7)
      Cleanups.front().Calls.front().Leaf.ImageWrites.clear();
    if (Mutation == 8)
      for (auto &Segment : Image.Segments)
        if (Segment.isExecutable())
          Segment.Flags = Segment.Flags | SegmentFlags::Writable;
    if (Mutation == 9)
      std::reverse(Changed.Blocks.begin(), Changed.Blocks.end());
    if (Mutation == 10)
      Cleanups.front().ActionState = UINT32_MAX;
    const auto Current = analyzeCheckedX86CxxStates(
        Image, Changed, &States.CalleeContracts, &Cleanups);
    EXPECT_EQ(Current.SourceCallFrameEffectsComplete,
              Mutation == 0 || Mutation == 9);
  }
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
