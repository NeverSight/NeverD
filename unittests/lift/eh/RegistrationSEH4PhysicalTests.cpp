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
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/X86RegistrationSEH4PhysicalFrame.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <optional>
#include <vector>

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

TEST(RegistrationSEH4Physical, DeclaresStdcallBeforeEitherBodyEmissionOrder) {
  auto Image =
      sharedSEH4Image(0x400000, false, 32, SharedSEH4Body::FilteredCall, 16);
  const auto Low = liftSharedSEH4(Image);
  const auto Target = convertSharedFrame(Image, Low);
  MedFunc Caller;
  Caller.Entry = 0x402400;
  Caller.Name = "caller";
  Caller.CC = CallingConv::CDECL;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Caller.Entry;
  Block.EndAddr = Caller.Entry + 6;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Caller.Entry;
  Call.OriginSeq = 0;
  Call.addInput(MedVar::makeConst(Target.Entry, 4));
  Block.Ops.push_back(Call);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Caller.Entry + 5;
  Return.OriginSeq = 1;
  Return.addInput(MedVar::makeConst(7, 4));
  Block.Ops.push_back(Return);
  Caller.Blocks.push_back(Block);
  MedCallInfo Info;
  Info.BlockId = 0;
  Info.OpIdx = 0;
  Info.TargetAddr = Target.Entry;
  for (unsigned I = 0; I != 4; ++I)
    Info.Args.push_back(MedVar::makeConst(7 + I, 4));
  Caller.CallInfos.push_back(Info);
  std::vector<MedFunc> Functions{Caller, Target};
  for (unsigned Order = 0; Order != 2; ++Order) {
    SCOPED_TRACE(Order);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit(Functions, Context, "stdcall-order", Arch::X86,
                              {}, &Image, BinaryFormat::COFF);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    const auto *Parent = Module->getFunction(Target.Name);
    ASSERT_TRUE(Parent);
    ASSERT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
    unsigned Calls = 0;
    for (const auto &IRBlock : *Module->getFunction(Caller.Name))
      for (const auto &I : IRBlock)
        if (const auto *Site = llvm::dyn_cast<llvm::CallBase>(&I);
            Site && Site->getCalledFunction() == Parent) {
          ++Calls;
          EXPECT_EQ(Site->getCallingConv(), llvm::CallingConv::X86_StdCall);
          EXPECT_EQ(Site->arg_size(), 4u);
        }
    EXPECT_EQ(Calls, 1u);
    std::reverse(Functions.begin(), Functions.end());
  }
}

TEST(RegistrationSEH4Physical, SeparatesStdcallParentFromCdeclCallbacks) {
  for (uint16_t Pop : {4, 16, 20}) {
    SCOPED_TRACE(Pop);
    auto Image =
        sharedSEH4Image(0x400000, false, 32, SharedSEH4Body::FilteredCall, Pop);
    const auto Low = liftSharedSEH4(Image);
    ASSERT_TRUE(Low.RegistrationStates);
    ASSERT_TRUE(Low.RegistrationStates->Complete);
    EXPECT_EQ(getCheckedX86RegistrationSEHParentABI(Low, Image), Pop);
    EXPECT_FALSE(getCheckedX86RegistrationCxxParentABI(Low, Image));
    const auto Med = convertSharedFrame(Image, Low);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Med}, Context, "shared-seh4-stdcall", Arch::X86,
                              {}, &Image, BinaryFormat::COFF);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    const auto *Parent = Module->getFunction(Med.Name);
    ASSERT_TRUE(Parent);
    ASSERT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
    EXPECT_EQ(Parent->getCallingConv(), llvm::CallingConv::X86_StdCall);
    EXPECT_EQ(Parent->arg_size(), Pop / 4u);
    for (const auto &Function : *Module)
      if (&Function != Parent && !Function.isDeclaration())
        EXPECT_EQ(Function.getCallingConv(), llvm::CallingConv::C);
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
    auto Error =
        validateCOFFRegistrationIR(*Parent, *Low.ExceptionMetadata, Image);
    EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    auto *Mutable = Module->getFunction(Med.Name);
    for (const auto Convention :
         {llvm::CallingConv::C, llvm::CallingConv::X86_ThisCall,
          llvm::CallingConv::X86_FastCall}) {
      Mutable->setCallingConv(Convention);
      auto Wrong =
          validateCOFFRegistrationIR(*Mutable, *Low.ExceptionMetadata, Image);
      EXPECT_TRUE(bool(Wrong));
      llvm::consumeError(std::move(Wrong));
    }
    Mutable->setCallingConv(llvm::CallingConv::X86_StdCall);
    Mutable->addParamAttr(0, llvm::Attribute::InReg);
    auto Wrong =
        validateCOFFRegistrationIR(*Mutable, *Low.ExceptionMetadata, Image);
    EXPECT_TRUE(bool(Wrong));
    llvm::consumeError(std::move(Wrong));
#endif
  }
}

TEST(RegistrationSEH4Physical, ParentCleanupRequiresCurrentCompleteSource) {
  auto Image =
      sharedSEH4Image(0x400000, false, 32, SharedSEH4Body::FilteredCall, 16);
  const auto Low = liftSharedSEH4(Image);
  ASSERT_EQ(getCheckedX86RegistrationSEHParentABI(Low, Image), 16);
  auto Changed = Low;
  Changed.CalleePopBytes = 65532;
  EXPECT_EQ(getCheckedX86RegistrationSEHParentABI(Changed, Image), 16);
  for (auto Flag : {&RegistrationStateAnalysis::Complete,
                    &RegistrationStateAnalysis::CallbackStatesComplete,
                    &RegistrationStateAnalysis::RegistrationLifetimeComplete,
                    &RegistrationStateAnalysis::ChainOperationsComplete,
                    &RegistrationStateAnalysis::SEHInvocationExitsComplete,
                    &RegistrationStateAnalysis::SEHParentChainComplete}) {
    Changed = Low;
    Changed.RegistrationStates.value().*Flag = false;
    EXPECT_FALSE(getCheckedX86RegistrationSEHParentABI(Changed, Image));
  }
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
      EXPECT_FALSE(getCheckedX86RegistrationSEHParentABI(Changed, Image));
      auto EditedImage = Image;
      EditedImage.Segments.front()
          .Data[Boundary.Address - EditedImage.Segments.front().VA] = 0xcb;
      EXPECT_FALSE(getCheckedX86RegistrationSEHParentABI(Low, EditedImage));
    }
  EXPECT_GE(Returns, 2u);
  auto Unaligned =
      sharedSEH4Image(0x400000, false, 32, SharedSEH4Body::FilteredCall, 6);
  EXPECT_FALSE(getCheckedX86RegistrationSEHParentABI(liftSharedSEH4(Unaligned),
                                                     Unaligned));
}

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

TEST(RegistrationSEH4Physical, KeepsCheckedThreadReadsSeparateFromTheChain) {
  auto Image = sharedSEH4Image(0x400000, false, 32, SharedSEH4Body::ThreadRead);
  const auto Low = liftSharedSEH4(Image);
  ASSERT_TRUE(Low.RegistrationStates);
  ASSERT_TRUE(Low.RegistrationStates->Complete);
  ASSERT_EQ(Low.RegistrationStates->SEHThreadAccesses.size(), 2u);
  const auto Med = convertSharedFrame(Image, Low);
  ASSERT_TRUE(Med.RegistrationSEH4PhysicalFrame);
  const auto *Physical = getX86RegistrationSEHPhysicalStates(Med);
  ASSERT_TRUE(Physical);
  EXPECT_EQ(Physical->SEHThreadAccesses,
            Low.RegistrationStates->SEHThreadAccesses);
  EXPECT_EQ(Physical->ChainAccesses.size(), 3u);
  ASSERT_TRUE(hasCheckedX86RegistrationSEHPhysicalFrame(Image, Med));
  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Med}, Context, "shared-seh4-thread", Arch::X86, {},
                            &Image, BinaryFormat::COFF);
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
  const auto &Self = Low.RegistrationStates->SEHThreadAccesses.front();
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Med;
    auto &States = *Changed.RegistrationStates;
    if (Mutation == 0)
      States.SEHThreadAccessesComplete = false;
    if (Mutation == 1)
      States.SEHThreadAccesses.pop_back();
    if (Mutation == 2)
      ++States.SEHThreadAccesses.back().RootOpSeq;
    bool Edited = Mutation < 3;
    for (auto &Block : Changed.Blocks) {
      std::optional<MedOp> Extra;
      for (auto &Op : Block.Ops) {
        if (Op.Addr != Self.Address)
          continue;
        if (Mutation == 8)
          for (unsigned I = 0; I != Op.NumInputs; ++I) {
            auto &Input = Op.Inputs[I];
            if (Input.isConst() && Input.ConstVal == 0x18) {
              Input = MedVar::makeConst(0x1c, Input.Size);
              Edited = true;
            }
          }
        if (Op.OriginSeq != Self.OpSeq)
          continue;
        if (Mutation == 3)
          Op.Inputs[0] = MedVar::makeConst(0, Op.Inputs[0].Size);
        if (Mutation == 4)
          ++Op.OriginSeq;
        if (Mutation == 5)
          Op.Dead = true;
        if (Mutation == 6)
          Extra = Op;
        if (Mutation == 7)
          Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
        Edited |= Mutation >= 3 && Mutation <= 7;
      }
      if (Extra)
        Block.Ops.push_back(*Extra);
    }
    ASSERT_TRUE(Edited);
    EXPECT_FALSE(getX86RegistrationSEHPhysicalStates(Changed));
    EXPECT_FALSE(hasCheckedX86RegistrationSEHPhysicalFrame(Image, Changed));
  }
}
