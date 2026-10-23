//===- RegistrationSEH4PhysicalTests.cpp - Native shared SEH4 binding -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check source/physical separation and the independent native IR installer.
//===----------------------------------------------------------------------===//

#include "RegistrationSEH4NativeTestUtils.h"
#include "RegistrationSEH4SharedTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/X86RegistrationSEH4PhysicalFrame.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"

using namespace neverd;
using namespace neverd::registration_test;

namespace {
MedFunc convertSharedFrame(const BinaryImage &Image, const LowFunc &Low) {
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  auto Result = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  recoverCallAbi(Result, Arch::X86, {}, &Image);
  return Result;
}
} // namespace

TEST(RegistrationSEH4Physical, ReplaysOriginalCallsThroughIndependentNativeIR) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (bool Wide : {false, true})
      for (uint32_t Allocation : {12u, 32u, 92u}) {
        SCOPED_TRACE(::testing::Message()
                     << Base << ' ' << Wide << ' ' << Allocation);
        auto Image = sharedSEH4Image(Base, Wide, Allocation);
        const auto Low = liftSharedSEH4(Image);
        ASSERT_TRUE(Low.RegistrationStates);
        ASSERT_TRUE(Low.RegistrationStates->SEH4Prologue);
        const auto Med = convertSharedFrame(Image, Low);
        ASSERT_TRUE(Med.RegistrationSEH4PhysicalFrame);
        ASSERT_TRUE(Med.RegistrationCallerCleanupABIComplete);
        ASSERT_TRUE(Med.RegistrationStates->SEH4Prologue);
        EXPECT_EQ(Med.RegistrationStates->SEH4Prologue,
                  Low.RegistrationStates->SEH4Prologue);
        EXPECT_EQ(Med.RegistrationStates->FrameValues.size(),
                  Low.RegistrationStates->FrameValues.size());
        const auto *Physical = getX86RegistrationSEHPhysicalStates(Med);
        ASSERT_NE(Physical, nullptr);
        EXPECT_NE(Physical, &*Med.RegistrationStates);
        EXPECT_FALSE(Physical->SEH4Prologue);
        EXPECT_GT(Physical->FrameValues.size(),
                  Med.RegistrationStates->FrameValues.size());
        EXPECT_EQ(Physical->ChainAccesses.size(), 3u);
        EXPECT_TRUE(hasCheckedX86RegistrationSEHPhysicalFrame(Image, Med));
        EXPECT_EQ(getX86RegistrationSEHFrameStorage(Med), Allocation + 40);
        llvm::LLVMContext Context;
        auto Module =
            MedLLVMEmitter().emit({Med}, Context, "shared-seh4-native",
                                  Arch::X86, {}, &Image, BinaryFormat::COFF);
        ASSERT_TRUE(Module);
        ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
        const auto *Parent = Module->getFunction(Med.Name);
        ASSERT_TRUE(Parent);
        ASSERT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
        auto Error =
            validateCOFFRegistrationIR(*Parent, *Low.ExceptionMetadata, Image);
        EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
#endif
      }
}

TEST(RegistrationSEH4Physical,
     RechecksOrdinaryCalleeStacksDuringPhysicalReplay) {
  auto Image =
      sharedSEH4Image(0x400000, false, 32, SharedSEH4Body::FilteredCall);
  const auto Low = liftSharedSEH4(Image);
  ASSERT_TRUE(Low.RegistrationStates);
  ASSERT_TRUE(Low.RegistrationStates->Complete);
  const auto Med = convertSharedFrame(Image, Low);
  ASSERT_TRUE(Med.RegistrationSEH4PhysicalFrame);
  ASSERT_TRUE(hasCheckedX86RegistrationSEHPhysicalFrame(Image, Med));
  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Med}, Context, "shared-seh4-filter", Arch::X86, {},
                            &Image, BinaryFormat::COFF);
  ASSERT_TRUE(Module);
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  const auto *Parent = Module->getFunction(Med.Name);
  ASSERT_TRUE(Parent);
  ASSERT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
  for (const auto &Block : *Parent)
    for (const auto &Instruction : Block)
      if (const auto *Allocation =
              llvm::dyn_cast<llvm::AllocaInst>(&Instruction))
        EXPECT_TRUE(Allocation->isStaticAlloca());
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  auto Error =
      validateCOFFRegistrationIR(*Parent, *Low.ExceptionMetadata, Image);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  checkSharedSEH4SavedStackEdits(*Module->getFunction(Med.Name),
                                 *Low.ExceptionMetadata, Image);
#endif

  // Keep the original source receipts but make the current callee return
  // from the wrong stack slot. A fresh physical proof cannot reuse its ABI.
  Image.Segments.front().Data[0x180] = 0x50;
  Image.Segments.front().Data[0x181] = 0x90;
  const auto Changed = convertSharedFrame(Image, Low);
  EXPECT_FALSE(Changed.RegistrationSEH4PhysicalFrame);
  EXPECT_FALSE(hasCheckedX86RegistrationSEHPhysicalFrame(Image, Changed));
}

TEST(RegistrationSEH4Physical,
     RemovedOrChangedReceiptsCannotBecomeDirectFrames) {
  const auto Original = sharedSEH4Image(0x400000, false);
  auto ImageForLift = Original;
  const auto Low = liftSharedSEH4(ImageForLift);
  const auto Proven = convertSharedFrame(Original, Low);
  ASSERT_TRUE(hasCheckedX86RegistrationSEHPhysicalFrame(Original, Proven));
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = Original;
    auto Med = Proven;
    switch (Mutation) {
    case 0:
      Med.RegistrationSEH4PhysicalFrame.reset();
      break;
    case 1:
      Med.RegistrationStates->SEH4Prologue.reset();
      break;
    case 2:
      Med.RegistrationSEH4PhysicalFrame.reset();
      Med.RegistrationStates->SEH4Prologue.reset();
      break;
    case 3:
      Med.RegistrationStates->Complete = false;
      break;
    case 4:
      ++Med.RegistrationStates->SecurityCookieVA;
      break;
    case 5:
      ++Med.RegistrationStates->ChainAccesses.front().OpSeq;
      break;
    case 6:
      Image.Segments.front().Data[0x119] ^= 1;
      break;
    case 7:
      Image.Segments.front().Data[0x150] ^= 1;
      break;
    case 8:
      Image.Segments.front().Data[1] = 36;
      break;
    case 9:
      for (auto &Block : Med.Blocks)
        for (auto &Op : Block.Ops)
          if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS)
            ++Op.OriginSeq;
      break;
    case 10:
      ++Med.RegistrationStates->SEH4Prologue->StackOffset;
      break;
    }
    EXPECT_FALSE(hasCheckedX86RegistrationSEHPhysicalFrame(Image, Med));
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Med}, Context, "changed-shared-seh4", Arch::X86,
                              {}, &Image, BinaryFormat::COFF);
    if (Module) {
      const auto *Parent = Module->getFunction(Med.Name);
      ASSERT_TRUE(Parent);
      EXPECT_FALSE(Parent->getMetadata(windows_eh_md::NativeAttachment));
    }
  }
}

TEST(RegistrationSEH4Physical,
     ChangedLowIRAndOverlappingSavesHaveNoProjection) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = sharedSEH4Image(0x400000, false, Mutation == 0 ? 4 : 32);
    auto Low = liftSharedSEH4(Image);
    if (Mutation == 1)
      Low.Blocks.front().Ops.back().Inputs[0].Offset += 1;
    if (Mutation == 2)
      Low.OrdinaryModuleAnalysisRoots.insert(Low.Entry + 1);
    if (Mutation == 3)
      Low.RegistrationStates->SEH4Prologue.reset();
    if (Mutation == 4)
      Low.RegistrationStates->ChainAccesses.clear();
    const auto Med = convertSharedFrame(Image, Low);
    EXPECT_FALSE(Med.RegistrationSEH4PhysicalFrame);
    EXPECT_FALSE(hasCheckedX86RegistrationSEHPhysicalFrame(Image, Med));
  }
}
