//===- WindowsRegistrationNativeTests.cpp - PE32 SEH lowering tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationCxxContinuationTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/LanguageEHMetadata.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/object/PELayout.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/MCFixup.h"
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
#include "llvm/IR/WinEHFrame.h"
#endif
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace neverd {
class MedLLVMEmitterTestPeer {
public:
  static void prepare(MedLLVMEmitter &E, llvm::Module &M, llvm::Function &F,
                      const MedFunc &Source, llvm::AllocaInst &Frame) {
    E.Ctx = &M.getContext();
    E.Mod = &M;
    E.TargetArch = Arch::X86;
    E.TargetFormat = BinaryFormat::COFF;
    E.CurFunc = &F;
    E.CurMedFunc = &Source;
    E.FrameAlloca = &Frame;
    E.FrameEntrySPOffset = 64;
  }
  static void chain(MedLLVMEmitter &E, llvm::Instruction &I, va_t Address,
                    int Seq) {
    E.RegistrationChainIR.emplace(std::make_pair(Address, Seq), &I);
  }
  static void call(MedLLVMEmitter &E, llvm::CallInst &I, va_t Address) {
    E.CallSiteAddrs.emplace(&I, Address);
    I.setMetadata(language_eh_md::InternalSourceCallAttachment,
                  llvm::MDNode::get(
                      I.getContext(),
                      {llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                          llvm::Type::getInt64Ty(I.getContext()), Address))}));
  }
  static void forgetCall(MedLLVMEmitter &E, llvm::CallInst &I) {
    E.CallSiteAddrs.erase(&I);
  }
  static bool lower(MedLLVMEmitter &E, const MedFunc &Source, llvm::Function &F,
                    const std::map<int, llvm::BasicBlock *> &Blocks) {
    return E.emitNativeX86RegistrationSEH(Source, F, Blocks);
  }
};
} // namespace neverd

namespace {
using namespace neverd;

#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
TEST(WindowsRegistrationCxxSource, InputPE32EmitsTypedCatchAndCleanupIR) {
  const char *Path = std::getenv("NEVERD_REGISTRATION_INPUT_CXX_PE32");
  if (!Path)
    GTEST_SKIP()
        << "set NEVERD_REGISTRATION_INPUT_CXX_PE32 to a genuine fixture";
  auto Loaded = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  const ExceptionFunction *Source = nullptr;
  for (const auto &EH : Loaded->ExceptionMetadata.Functions) {
    const auto Classification =
        classifyWindowsEHNativeSource(EH, Arch::X86, BinaryFormat::COFF,
                                      WindowsEHNativeCapability::IRLowering);
    if (!Classification.canLowerNativeIR() ||
        Classification.Model != WindowsEHNativeSourceModel::X86RegistrationCxx)
      continue;
    if (EH.Cxx->TryBlocks.size() == 1 &&
        EH.Cxx->TryBlocks[0].Handlers.size() == 1) {
      ASSERT_EQ(Source, nullptr);
      Source = &EH;
    }
  }
  ASSERT_NE(Source, nullptr);
#if defined(LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS) &&                          \
    defined(LLVM_NEVERD_X86_CXX_HANDLER_RECEIPTS)
  EXPECT_TRUE(
      classifyWindowsEHNativeSource(*Source, Arch::X86, BinaryFormat::COFF)
          .canPatchOutput());
#else
  EXPECT_EQ(
      classifyWindowsEHNativeSource(*Source, Arch::X86, BinaryFormat::COFF)
          .Reason,
      WindowsEHNativeSourceReason::OutputReconstructionUnavailable);
#endif
  Decoder Decoder;
  ASSERT_TRUE(Decoder.init(*Loaded));
  auto Low = CFGBuilder().build(*Loaded, Decoder, Source->CodeRange.Begin,
                                "registration_cxx_source");
  ASSERT_TRUE(Low.RegistrationStates);
  for (const auto &Diagnostic : Low.RegistrationStates->Diagnostics)
    llvm::errs() << Diagnostic << '\n';
  ASSERT_TRUE(Low.RegistrationStates->Complete);
  ASSERT_TRUE(hasCallerCleanupRegistrationABI(Low, *Loaded));
  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Loaded);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  inferMedTypes(Med, Arch::X86);
  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  auto Module = Emitter.emit({Med}, Context, "source-cxx-registration",
                             Arch::X86, {}, &*Loaded, BinaryFormat::COFF);
  ASSERT_TRUE(Module);
  auto *Parent = Module->getFunction(Med.Name);
  ASSERT_TRUE(Parent);
  if (!Parent->getMetadata(windows_eh_md::NativeAttachment))
    Module->print(llvm::errs(), nullptr);
  ASSERT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
  auto Control =
      validateCOFFRegistrationCxxControlIR(*Parent, *Source, *Loaded);
  ASSERT_FALSE(bool(Control)) << llvm::toString(std::move(Control));
  auto FrameContract = validateCOFFRegistrationCxxIR(*Parent, *Source, *Loaded);
  ASSERT_FALSE(bool(FrameContract)) << llvm::toString(std::move(FrameContract));
  auto SharedContract = validateCOFFRegistrationIR(*Parent, *Source, *Loaded);
  ASSERT_FALSE(bool(SharedContract))
      << llvm::toString(std::move(SharedContract));
  registration_test::checkCxxContinuationEdits(*Parent, *Source, *Loaded);
  // Reparse the unchanged input for every post-emission edit. A completeness
  // marker alone cannot bind a moved source operation or runtime control edge.
  for (unsigned Mutation = 0; Mutation != 24; ++Mutation) {
    auto Edited = llvm::CloneModule(*Module);
    auto *Function = Edited->getFunction(Parent->getName());
    llvm::CatchPadInst *CatchPad = nullptr;
    llvm::CleanupPadInst *CleanupPad = nullptr;
    llvm::InvokeInst *Invoke = nullptr;
    llvm::CatchReturnInst *CatchReturn = nullptr;
    llvm::CallInst *CleanupCall = nullptr;
    llvm::Instruction *BlockAnchor = nullptr;
    llvm::Instruction *SourceOperation = nullptr;
    llvm::IntrinsicInst *Escape = nullptr;
    llvm::LoadInst *Head = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        if (auto *Pad = llvm::dyn_cast<llvm::CatchPadInst>(&I))
          CatchPad = Pad;
        if (auto *Pad = llvm::dyn_cast<llvm::CleanupPadInst>(&I))
          CleanupPad = Pad;
        if (auto *Call = llvm::dyn_cast<llvm::InvokeInst>(&I))
          Invoke = Call;
        if (auto *Return = llvm::dyn_cast<llvm::CatchReturnInst>(&I))
          CatchReturn = Return;
        if (I.getMetadata(windows_eh_md::RegistrationBlockAttachment))
          BlockAnchor = &I;
        if (I.getMetadata(windows_eh_md::RegistrationOperationAttachment))
          SourceOperation = &I;
        if (auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
            Call && Call->getIntrinsicID() == llvm::Intrinsic::localescape)
          Escape = Call;
        if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I);
            Load && Load->getPointerAddressSpace() == 257)
          Head = Load;
      }
    ASSERT_TRUE(CatchPad && CleanupPad && Invoke && CatchReturn &&
                BlockAnchor && SourceOperation && Escape && Head);
    for (auto &I : *CleanupPad->getParent())
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
          Call && !llvm::isa<llvm::IntrinsicInst>(Call))
        CleanupCall = Call;
    ASSERT_TRUE(CleanupCall);
    auto AlterMetadata = [&](llvm::Instruction &I, llvm::StringRef Name,
                             unsigned Index, uint64_t Value, unsigned Width) {
      auto *MD = I.getMetadata(Name);
      llvm::SmallVector<llvm::Metadata *, 8> Operands;
      for (const auto &Operand : MD->operands())
        Operands.push_back(Operand.get());
      Operands[Index] = llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
          llvm::IntegerType::get(Context, Width), Value));
      I.setMetadata(Name, llvm::MDNode::get(Context, Operands));
    };
    switch (Mutation) {
    case 0:
      Function->setMetadata(windows_eh_md::NativeAttachment, nullptr);
      break;
    case 1:
      Function->removeFnAttr(llvm::RewriteWinX86CxxFrameAttribute);
      break;
    case 2:
      Function->setCallingConv(Function->getCallingConv() ==
                                       llvm::CallingConv::C
                                   ? llvm::CallingConv::X86_ThisCall
                                   : llvm::CallingConv::C);
      break;
    case 3:
      rewrite_source::setOriginalVA(
          *llvm::cast<llvm::Function>(Function->getPersonalityFn()),
          Source->PersonalityVA);
      break;
    case 4:
      BlockAnchor->setMetadata(windows_eh_md::RegistrationBlockAttachment,
                               nullptr);
      break;
    case 5:
      AlterMetadata(*BlockAnchor, windows_eh_md::RegistrationBlockAttachment, 1,
                    Source->CodeRange.Begin, 64);
      break;
    case 6: {
      auto *Duplicate = BlockAnchor->clone();
      Duplicate->insertBefore(BlockAnchor->getIterator());
      break;
    }
    case 7:
      SourceOperation->setMetadata(
          windows_eh_md::RegistrationOperationAttachment, nullptr);
      break;
    case 8:
      AlterMetadata(*SourceOperation,
                    windows_eh_md::RegistrationOperationAttachment, 2, 999, 32);
      break;
    case 9: {
      auto *Duplicate = SourceOperation->clone();
      Duplicate->insertBefore(SourceOperation->getIterator());
      break;
    }
    case 10:
      Invoke->setUnwindDest(CatchPad->getCatchSwitch()->getParent());
      break;
    case 11:
      Invoke->setCallingConv(llvm::CallingConv::X86_ThisCall);
      break;
    case 12:
      rewrite_source::setOriginalVA(*Invoke->getCalledFunction(),
                                    Source->CodeRange.Begin);
      break;
    case 13:
      Invoke->removeFnAttr(llvm::Attribute::NoReturn);
      Invoke->getCalledFunction()->removeFnAttr(llvm::Attribute::NoReturn);
      break;
    case 14:
      CatchPad->setMetadata(llvm::mc_rewrite::RewriteWinEHSemanticAttachment,
                            nullptr);
      break;
    case 15:
      CleanupPad->setMetadata(llvm::mc_rewrite::RewriteWinEHSemanticAttachment,
                              nullptr);
      break;
    case 16:
      AlterMetadata(*CatchPad, llvm::RewriteWinX86CxxCatchObjectAttachment, 1,
                    0, 32);
      break;
    case 17:
      AlterMetadata(*CatchPad, llvm::RewriteWinX86CxxCatchObjectAttachment, 2,
                    1, 32);
      break;
    case 18:
      CatchPad->setArgOperand(
          0, new llvm::GlobalVariable(*Edited, llvm::Type::getInt8Ty(Context),
                                      false, llvm::GlobalValue::ExternalLinkage,
                                      nullptr, "unproved_type"));
      break;
    case 19:
      llvm::cast<llvm::CleanupReturnInst>(
          CleanupPad->getParent()->getTerminator())
          ->setUnwindDest(CleanupPad->getParent());
      break;
    case 20:
      CatchReturn->setSuccessor(&Function->getEntryBlock());
      break;
    case 21:
      CleanupCall->clone()->insertBefore(CleanupCall->getIterator());
      break;
    case 22:
      Head->setOperand(
          0, llvm::ConstantExpr::getIntToPtr(
                 llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 4),
                 Head->getPointerOperand()->getType()));
      break;
    case 23: {
      llvm::IRBuilder<> B(Escape);
      Escape->setArgOperand(0, B.CreateAlloca(B.getInt32Ty()));
      break;
    }
    }
    auto Changed =
        validateCOFFRegistrationCxxControlIR(*Function, *Source, *Loaded);
    EXPECT_TRUE(bool(Changed)) << Mutation;
    if (Changed)
      llvm::consumeError(std::move(Changed));
  }
#endif
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
  for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
    const bool Reference = Low.RegistrationStates->CxxCatchObjects[0].Reference;
    if (!Reference && Mutation >= 12)
      continue;
    auto Edited = llvm::CloneModule(*Module);
    auto *Function = Edited->getFunction(Parent->getName());
    llvm::AllocaInst *Frame = nullptr;
    llvm::CallInst *Borrow = nullptr;
    llvm::StoreInst *Initialize = nullptr;
    llvm::StoreInst *SourceStore = nullptr;
    llvm::LoadInst *RuntimeLoad = nullptr;
    llvm::CatchPadInst *CatchPad = nullptr;
    uint64_t EntrySP = 0;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        if (auto *Pad = llvm::dyn_cast<llvm::CatchPadInst>(&I))
          CatchPad = Pad;
        if (auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&I))
          if (auto *MD = Slot->getMetadata(
                  windows_eh_md::RegistrationFrameAttachment)) {
            Frame = Slot;
            EntrySP =
                llvm::mdconst::extract<llvm::ConstantInt>(MD->getOperand(1))
                    ->getZExtValue();
          }
        if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
            Call && !llvm::isa<llvm::IntrinsicInst>(Call) &&
            Call->arg_size() == 1)
          Borrow = Call;
        if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
            Store && Store->getMetadata(
                         windows_eh_md::RegistrationOperationAttachment)) {
          SourceStore = Store;
          auto *Value =
              llvm::dyn_cast<llvm::ConstantInt>(Store->getValueOperand());
          if (Value && Value->getBitWidth() == 32 && Value->getZExtValue() == 1)
            Initialize = Store;
        }
        if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I))
          if (auto *MD = Load->getMetadata(
                  windows_eh_md::RegistrationOperationAttachment))
            for (const auto &Access :
                 Low.RegistrationStates->RuntimeObjectAccesses)
              if (!Access.Write &&
                  llvm::mdconst::extract<llvm::ConstantInt>(MD->getOperand(1))
                          ->getZExtValue() == Access.Address &&
                  llvm::mdconst::extract<llvm::ConstantInt>(MD->getOperand(2))
                          ->getZExtValue() == uint32_t(Access.OpSeq))
                RuntimeLoad = Load;
      }
    ASSERT_TRUE(Frame && Borrow && Initialize && SourceStore);
    llvm::IRBuilder<> B(Borrow);
    switch (Mutation) {
    case 0:
      Borrow->setArgOperand(0, Frame);
      break;
    case 1:
      Borrow->setArgOperand(
          0, B.CreateInBoundsGEP(B.getInt8Ty(), Frame, B.getInt32(1)));
      break;
    case 2: {
      llvm::IRBuilder<> Entry(Function->getEntryBlock().getTerminator());
      Borrow->setArgOperand(0, Entry.CreateAlloca(Entry.getInt32Ty()));
      break;
    }
    case 3: {
      llvm::IRBuilder<> Entry(Function->getEntryBlock().getTerminator());
      Initialize->setOperand(1, Entry.CreateAlloca(Entry.getInt32Ty()));
      break;
    }
    case 4:
      Initialize->setOperand(0, llvm::UndefValue::get(B.getInt32Ty()));
      break;
    case 5:
      Initialize->setOperand(0, llvm::PoisonValue::get(B.getInt32Ty()));
      break;
    case 6:
      Initialize->setOperand(0, B.getInt8(1));
      break;
    case 7: {
      llvm::IRBuilder<> At(Initialize);
      Initialize->setOperand(0, At.CreatePtrToInt(Frame, At.getInt32Ty()));
      break;
    }
    case 8: {
      auto *Frozen = new llvm::GlobalVariable(
          *Edited, B.getInt8Ty(), false, llvm::GlobalValue::ExternalLinkage,
          nullptr, makeNdDataSymbol(Source->Registration->ScopeTableVA));
      SourceStore->setOperand(1, Frozen);
      break;
    }
    case 9: {
      auto Runtime =
          coff_loader::getCheckedX86CxxPersonalityABI(*Loaded, *Source);
      ASSERT_TRUE(Runtime);
      auto *Frozen = new llvm::GlobalVariable(
          *Edited, B.getInt8Ty(), false, llvm::GlobalValue::ExternalLinkage,
          nullptr, makeNdDataSymbol(Runtime->IATVA));
      SourceStore->setOperand(1, Frozen);
      break;
    }
    case 10:
      for (auto &Global : Edited->globals())
        if (parseNdCodePtrSymbol(Global.getName()) ==
            Loaded
                ->getSegmentFor(Low.RegistrationStates->CleanupContracts[0]
                                    .Leaf.ImageWrites[0]
                                    .Begin)
                ->VA) {
          Global.setInitializer(
              llvm::Constant::getNullValue(Global.getValueType()));
          break;
        }
      break;
    case 11:
      SourceStore->setOperand(1, llvm::ConstantPointerNull::get(B.getPtrTy()));
      break;
    case 12:
      ASSERT_TRUE(RuntimeLoad);
      RuntimeLoad->setOperand(0, Frame);
      break;
    case 13:
      ASSERT_TRUE(RuntimeLoad);
      {
        llvm::IRBuilder<> At(RuntimeLoad);
        RuntimeLoad->setOperand(
            0, At.CreateInBoundsGEP(At.getInt8Ty(),
                                    RuntimeLoad->getPointerOperand(),
                                    At.getInt32(4)));
      }
      break;
    case 14: {
      ASSERT_TRUE(CatchPad);
      llvm::StoreInst *CatchStore = nullptr;
      for (auto &I : *CatchPad->getParent())
        if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
            Store &&
            Store->getMetadata(windows_eh_md::RegistrationOperationAttachment))
          CatchStore = Store;
      ASSERT_TRUE(CatchStore);
      llvm::IRBuilder<> At(CatchStore);
      const auto Home = Low.RegistrationStates->CxxCatchObjects[0].FrameOffset;
      CatchStore->setOperand(
          1, At.CreateInBoundsGEP(At.getInt8Ty(), Frame,
                                  At.getInt32(int64_t(EntrySP) - 4 + Home)));
      break;
    }
    }
    auto StillControl =
        validateCOFFRegistrationCxxControlIR(*Function, *Source, *Loaded);
    ASSERT_FALSE(bool(StillControl))
        << Mutation << ": " << llvm::toString(std::move(StillControl));
    auto Changed = validateCOFFRegistrationCxxIR(*Function, *Source, *Loaded);
    EXPECT_TRUE(bool(Changed)) << Mutation;
    if (Changed)
      llvm::consumeError(std::move(Changed));
  }
#endif
  auto CheckSharedImageRoots = [&](const llvm::Module &M, size_t Minimum = 2) {
    size_t SharedRoots = 0;
    for (const auto &Global : M.globals()) {
      auto Address = parseNdCodePtrSymbol(Global.getName());
      if (!Address)
        Address = parseNdDataSymbol(Global.getName());
      if (!Address)
        continue;
      const auto *Segment = Loaded->getSegmentFor(*Address);
      if (!Segment)
        continue;
      auto Observed = [&](const RegistrationCalleeFrameContract &Contract) {
        for (const auto *Ranges : {&Contract.ImageReads, &Contract.ImageWrites,
                                   &Contract.CallerPCWrites})
          for (const auto &Range : *Ranges)
            if (Range.Begin < Segment->VA + Segment->Size &&
                Segment->VA < Range.End)
              return true;
        return false;
      };
      bool Shared = false;
      for (const auto &Contract : Low.RegistrationStates->CalleeContracts)
        Shared |= Observed(Contract);
      for (const auto &Contract : Low.RegistrationStates->CleanupContracts)
        Shared |= Observed(Contract.Leaf);
      if (!Shared)
        continue;
      ++SharedRoots;
      EXPECT_TRUE(Global.isDeclaration()) << Global.getName().str();
      EXPECT_TRUE(Global.hasExternalLinkage()) << Global.getName().str();
    }
    EXPECT_GE(SharedRoots, Minimum);
  };
  CheckSharedImageRoots(*Module);
  ASSERT_FALSE(Low.RegistrationStates->CleanupContracts.empty());
  auto HelperLow = CFGBuilder().build(
      *Loaded, Decoder, Low.RegistrationStates->CleanupContracts[0].Leaf.Target,
      "registration_image_identity_first");
  auto Helper = Converter.convert(HelperLow, Arch::X86, BinaryFormat::COFF);
  inferMedTypes(Helper, Arch::X86);
  llvm::LLVMContext ReorderedContext;
  auto Reordered = Emitter.emit({Helper, Med}, ReorderedContext,
                                "source-cxx-registration-reordered", Arch::X86,
                                {}, &*Loaded, BinaryFormat::COFF);
  ASSERT_TRUE(Reordered);
  ASSERT_FALSE(llvm::verifyModule(*Reordered, &llvm::errs()));
  ASSERT_TRUE(Reordered->getFunction(Med.Name));
  EXPECT_TRUE(Reordered->getFunction(Med.Name)->getMetadata(
      windows_eh_md::NativeAttachment));
  CheckSharedImageRoots(*Reordered);
  llvm::LLVMContext ShardContext;
  const std::vector<char> Mask = {1, 0};
  auto Shard = Emitter.emit({Helper, Med}, ShardContext,
                            "source-cxx-registration-helper-shard", Arch::X86,
                            {}, &*Loaded, BinaryFormat::COFF,
                            /*MergeableGlobals=*/true, &Mask);
  ASSERT_TRUE(Shard);
  ASSERT_FALSE(llvm::verifyModule(*Shard, &llvm::errs()));
  EXPECT_TRUE(Shard->getFunction(Med.Name)->isDeclaration());
  CheckSharedImageRoots(*Shard, 1);
  EXPECT_TRUE(Parent->hasFnAttribute(llvm::RewriteWinX86CxxFrameAttribute));
  unsigned Catches = 0, Returns = 0, Cleanups = 0, Throws = 0;
  for (const auto &Block : *Parent)
    for (const auto &I : Block) {
      if (const auto *Pad = llvm::dyn_cast<llvm::CatchPadInst>(&I)) {
        ++Catches;
        EXPECT_TRUE(
            Pad->getMetadata(llvm::RewriteWinX86CxxCatchObjectAttachment));
        EXPECT_TRUE(
            Pad->getMetadata(llvm::mc_rewrite::RewriteWinEHSemanticAttachment));
      }
      Returns += llvm::isa<llvm::CatchReturnInst>(I);
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I))
        if (const auto *Callee = Call->getCalledFunction()) {
          if (Callee->getName().starts_with("__nd_registration_cleanup.ecx_")) {
            ++Cleanups;
            EXPECT_EQ(Call->getCallingConv(), llvm::CallingConv::X86_ThisCall);
            EXPECT_EQ(Call->arg_size(), 1u);
            EXPECT_TRUE(Call->getType()->isVoidTy());
          }
          if (Callee->getName().starts_with("__nd_registration_throw_")) {
            ++Throws;
            EXPECT_TRUE(llvm::isa<llvm::InvokeInst>(Call));
            EXPECT_EQ(Call->arg_size(), 0u);
            EXPECT_TRUE(Call->doesNotReturn());
          }
        }
    }
  EXPECT_EQ(Catches, 1u);
  EXPECT_EQ(Returns, 1u);
  EXPECT_EQ(Cleanups, 2u);
  EXPECT_EQ(Throws, 1u);
  if (const char *Output = std::getenv("NEVERD_REGISTRATION_OUTPUT_IR")) {
    std::error_code EC;
    llvm::raw_fd_ostream Stream(Output, EC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    Module->print(Stream, nullptr);
  }
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();
  auto Resolve = [&](llvm::StringRef Symbol,
                     uint32_t) -> std::optional<uint64_t> {
    if (auto Address = parseNdDataSymbol(Symbol))
      return *Address;
    if (auto Address = parseNdCodePtrSymbol(Symbol))
      return *Address;
    for (const auto &Function : *Module) {
      llvm::SmallString<64> ObjectName;
      llvm::Mangler Mangler;
      Mangler.getNameWithPrefix(ObjectName, &Function, false);
      if (ObjectName != Symbol)
        continue;
      auto Address = rewrite_source::getOriginalVA(Function);
      if (!Address) {
        llvm::consumeError(Address.takeError());
        return std::nullopt;
      }
      return *Address;
    }
    return std::nullopt;
  };
  auto Buffer = llvm::MemoryBuffer::getFile(Path);
  ASSERT_TRUE(bool(Buffer));
  const auto InputBytes = (*Buffer)->getBuffer();
  std::vector<uint8_t> Binary(InputBytes.bytes_begin(), InputBytes.bytes_end());
  COFFPatcher Patcher;
  const va_t CodeVA = Patcher.plannedExecSegmentVA(Binary, Arch::X86);
  ASSERT_GE(CodeVA, Loaded->Base);
  auto Compiled = compileImageForPatch(*Module, Arch::X86, BinaryFormat::COFF,
                                       CodeVA, Resolve, Loaded->Base);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_TRUE(Compiled.Unresolved.empty());
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 3u);
#else
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 1u);
#endif
  const auto CatchRow =
      llvm::find_if(Compiled.WinEHSemanticRecords, [](const auto &Record) {
        return Record.Token.Kind ==
               llvm::mc_rewrite::RewriteWinEHSemanticKind::CxxCatch;
      });
  ASSERT_NE(CatchRow, Compiled.WinEHSemanticRecords.end());
  const auto &Row = *CatchRow;
  EXPECT_EQ(Row.Encoding,
            llvm::mc_rewrite::RewriteWinEHSemanticEncoding::X86CxxFH3);
  EXPECT_EQ(Row.Token, *windows_eh_semantics::getCxxCatchSemanticToken(
                           *Source, Arch::X86, 0, 0));
  EXPECT_EQ(Row.RecordSize, 16u);
  EXPECT_GE(Row.OwnerVA, CodeVA);
  EXPECT_GT(Row.HandlerVA, Row.OwnerVA);
  EXPECT_GE(Row.ContainerVA, CodeVA);
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
  ASSERT_TRUE(Row.X86CxxLayout);
  const auto &Layout = *Row.X86CxxLayout;
  for (const auto &Table : Layout.Tables) {
    EXPECT_FALSE(Table.BeginSymbol.empty());
    EXPECT_FALSE(Table.EndSymbol.empty());
    ASSERT_GE(Table.BeginVA, Compiled.BaseVA);
    ASSERT_GT(Table.EndVA, Table.BeginVA);
    ASSERT_LE(Table.EndVA - Compiled.BaseVA, Compiled.Bytes.size());
  }
  auto Word = [&](va_t Address) {
    return llvm::support::endian::read32le(Compiled.Bytes.data() + Address -
                                           Compiled.BaseVA);
  };
  const auto &Info = Layout.Tables[0];
  const auto &Unwinds = Layout.Tables[1];
  const auto &Tries = Layout.Tables[2];
  const auto &Handlers = Layout.Tables[3];
  ASSERT_EQ(Info.EndVA - Info.BeginVA, 36u);
  EXPECT_EQ(Word(Info.BeginVA), 0x19930522u);
  EXPECT_EQ(Word(Info.BeginVA + 4), (Unwinds.EndVA - Unwinds.BeginVA) / 8);
  EXPECT_EQ(Word(Info.BeginVA + 8), Unwinds.BeginVA);
  EXPECT_EQ(Word(Info.BeginVA + 12), (Tries.EndVA - Tries.BeginVA) / 20);
  EXPECT_EQ(Word(Info.BeginVA + 16), Tries.BeginVA);
  EXPECT_EQ(Word(Info.BeginVA + 20), 0u);
  EXPECT_EQ(Word(Info.BeginVA + 24), 0u);
  EXPECT_EQ(Word(Info.BeginVA + 28), 0u);
  EXPECT_EQ(Word(Info.BeginVA + 32), 1u);
  ASSERT_EQ(Row.ContainerEndVA, Row.ContainerVA + 20);
  EXPECT_EQ(Word(Row.ContainerVA + 12),
            (Handlers.EndVA - Handlers.BeginVA) / 16);
  EXPECT_EQ(Word(Row.ContainerVA + 16), Handlers.BeginVA);
  EXPECT_LE(Word(Row.ContainerVA), Word(Row.ContainerVA + 4));
  EXPECT_LT(Word(Row.ContainerVA + 4), Word(Row.ContainerVA + 8));
  EXPECT_LT(Word(Row.ContainerVA + 8), Word(Info.BeginVA + 4));
  EXPECT_EQ(Word(Row.RecordVA),
            Source->Cxx->TryBlocks[0].Handlers[0].Adjectives);
  EXPECT_EQ(Word(Row.RecordVA + 4),
            Source->Cxx->TryBlocks[0].Handlers[0].TypeDescriptorVA);
  EXPECT_EQ(int32_t(Word(Row.RecordVA + 8)), Layout.Frame[0] + Layout.Frame[2]);
  EXPECT_EQ(Word(Row.RecordVA + 12), Row.HandlerVA);
  EXPECT_GT(Layout.Frame[1], 0);
  EXPECT_EQ(Layout.Frame[3],
            Low.RegistrationStates->CxxCatchObjects[0].Reference
                ? 4u
                : Low.RegistrationStates->CxxCatchObjects[0].ObjectSize);
  size_t CleanupRows = 0;
  for (const auto &Cleanup : Compiled.WinEHSemanticRecords) {
    if (Cleanup.Token.Kind !=
        llvm::mc_rewrite::RewriteWinEHSemanticKind::CxxCleanup)
      continue;
    ++CleanupRows;
    ASSERT_TRUE(Cleanup.X86CxxLayout);
    EXPECT_EQ(Cleanup.RecordSize, 8u);
    EXPECT_EQ(Cleanup.Token, *windows_eh_semantics::getCxxCleanupSemanticToken(
                                 *Source, Arch::X86, Cleanup.Token.Region));
    EXPECT_EQ(Cleanup.ContainerVA, Unwinds.BeginVA);
    EXPECT_EQ(Cleanup.ContainerEndVA, Unwinds.EndVA);
    EXPECT_EQ(Cleanup.RecordVA,
              Unwinds.BeginVA + uint64_t(Cleanup.GeneratedState) * 8);
    EXPECT_EQ(int32_t(Word(Cleanup.RecordVA)), Cleanup.EnclosingState);
    EXPECT_EQ(Word(Cleanup.RecordVA + 4), Cleanup.HandlerVA);
  }
  EXPECT_EQ(CleanupRows, 2u);
  auto TableReceipt = getCheckedCOFFRegistrationCxxTableReceipt(
      *Parent, *Source, *Loaded, Compiled);
  ASSERT_TRUE(bool(TableReceipt)) << llvm::toString(TableReceipt.takeError());
  EXPECT_EQ(TableReceipt->FuncInfoVA, Info.BeginVA);
  EXPECT_EQ(TableReceipt->AbsolutePointerFields.size(), 7u);
  EXPECT_EQ(TableReceipt->SourceToGeneratedStates.size(),
            Source->Cxx->UnwindMap.size() + 1);
#ifdef LLVM_NEVERD_X86_CXX_HANDLER_RECEIPTS
  auto HandlerReceipt = getCheckedCOFFRegistrationCxxHandlerReceipt(
      *Parent, *Source, *Loaded, Compiled);
  ASSERT_TRUE(bool(HandlerReceipt))
      << llvm::toString(HandlerReceipt.takeError());
  EXPECT_EQ(HandlerReceipt->CodeRange.Begin, Layout.RegistrationHandlerVA);
  EXPECT_EQ(HandlerReceipt->CodeRange.End - HandlerReceipt->CodeRange.Begin,
            10u);
  EXPECT_EQ(HandlerReceipt->AbsolutePointerFields.size(), 9u);
#endif
  const auto TableSection =
      llvm::find_if(Compiled.Sections, [&](const auto &S) {
        return S.VA <= Info.BeginVA && Info.EndVA <= S.VA + S.Size;
      });
  ASSERT_NE(TableSection, Compiled.Sections.end());
  const auto PointerIndex =
      llvm::find_if(TableSection->FixupReferences, [&](const auto &F) {
        return TableSection->VA + F.Offset == Info.BeginVA + 8;
      });
  ASSERT_NE(PointerIndex, TableSection->FixupReferences.end());
  const auto SectionIndex = TableSection - Compiled.Sections.begin();
  const auto FixupIndex = PointerIndex - TableSection->FixupReferences.begin();
  const auto CleanupRow =
      llvm::find_if(Compiled.WinEHSemanticRecords, [](const auto &R) {
        return R.Token.Kind ==
               llvm::mc_rewrite::RewriteWinEHSemanticKind::CxxCleanup;
      });
  ASSERT_NE(CleanupRow, Compiled.WinEHSemanticRecords.end());
  for (unsigned Mutation = 0; Mutation != 30; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Compiled;
    auto Write = [&](va_t Address, uint32_t Value) {
      llvm::support::endian::write32le(
          Changed.Bytes.data() + Address - Changed.BaseVA, Value);
    };
    auto &Section = Changed.Sections[SectionIndex];
    auto &Fixups = Section.FixupReferences;
    switch (Mutation) {
    case 0:
      Write(Info.BeginVA, 0x19930521);
      break;
    case 1:
      Write(Info.BeginVA + 4, Word(Info.BeginVA + 4) + 1);
      break;
    case 2:
      Write(Info.BeginVA + 8, uint32_t(Unwinds.BeginVA) + 4);
      break;
    case 3:
      Write(Info.BeginVA + 12, 2);
      break;
    case 4:
      Write(Info.BeginVA + 16, uint32_t(Tries.BeginVA) + 4);
      break;
    case 5:
      Write(Info.BeginVA + 20, 1);
      break;
    case 6:
      Write(Info.BeginVA + 24, uint32_t(Unwinds.BeginVA));
      break;
    case 7:
      Write(Info.BeginVA + 28, uint32_t(Unwinds.BeginVA));
      break;
    case 8:
      Write(Info.BeginVA + 32, 4);
      break;
    case 9:
      Write(Row.ContainerVA, Word(Row.ContainerVA) + 1);
      break;
    case 10:
      Write(Row.ContainerVA + 4, Word(Row.ContainerVA + 4) - 1);
      break;
    case 11:
      Write(Row.ContainerVA + 8, Word(Info.BeginVA + 4));
      break;
    case 12:
      Write(Row.ContainerVA + 12, 0);
      break;
    case 13:
      Write(Row.ContainerVA + 16, uint32_t(Handlers.BeginVA) + 4);
      break;
    case 14:
      Write(Row.RecordVA, Word(Row.RecordVA) ^ 8);
      break;
    case 15:
      Write(Row.RecordVA + 4, Word(Row.RecordVA + 4) + 4);
      break;
    case 16:
      Write(Row.RecordVA + 8, Word(Row.RecordVA + 8) + 4);
      break;
    case 17:
      Write(Row.RecordVA + 12, Word(Row.RecordVA + 12) + 1);
      break;
    case 18:
      Write(CleanupRow->RecordVA, uint32_t(CleanupRow->GeneratedState));
      break;
    case 19:
      Write(CleanupRow->RecordVA + 4, 0);
      break;
    case 20:
      Changed.WinEHSemanticRecords.erase(
          Changed.WinEHSemanticRecords.begin() +
          (CleanupRow - Compiled.WinEHSemanticRecords.begin()));
      break;
    case 21:
      Changed.Sections.push_back(Section);
      break;
    case 22: {
      auto Overlap = Section;
      Overlap.VA = Info.BeginVA + 4;
      Overlap.Offset = Overlap.VA - Compiled.BaseVA;
      Overlap.Size = 4;
      Changed.Sections.push_back(Overlap);
      break;
    }
    case 23:
      Fixups.erase(Fixups.begin() + FixupIndex);
      break;
    case 24:
      Fixups[FixupIndex].IsPCRel = true;
      break;
    case 25:
      Fixups[FixupIndex].BitWidth = 16;
      break;
    case 26:
      Fixups[FixupIndex].Addend = 4;
      break;
    case 27:
      ++Fixups[FixupIndex].ResolvedValue;
      break;
    case 28: {
      auto Overlap = Fixups[FixupIndex];
      ++Overlap.Offset;
      Fixups.push_back(Overlap);
      break;
    }
    case 29:
      Fixups.push_back(Fixups[FixupIndex]);
      break;
    }
    auto Rejected = getCheckedCOFFRegistrationCxxTableReceipt(*Parent, *Source,
                                                              *Loaded, Changed);
    EXPECT_FALSE(bool(Rejected));
    if (!Rejected)
      llvm::consumeError(Rejected.takeError());
  }
#ifdef LLVM_NEVERD_X86_CXX_HANDLER_RECEIPTS
  // Table validation remains valid for these edits to executable handler,
  // parent registration and linker metadata. Its success cannot substitute
  // for independently authenticating the actual dispatch contract.
  const va_t RegistrationVA = HandlerReceipt->CodeRange.Begin;
  const auto CodeSection = llvm::find_if(Compiled.Sections, [&](const auto &S) {
    return S.VA <= RegistrationVA && RegistrationVA + 10 <= S.VA + S.Size;
  });
  ASSERT_NE(CodeSection, Compiled.Sections.end());
  const size_t CodeIndex = CodeSection - Compiled.Sections.begin();
  const auto FindFixup = [&](va_t Field) {
    return llvm::find_if(CodeSection->FixupReferences, [&](const auto &F) {
      return CodeSection->VA + F.Offset == Field;
    });
  };
  const auto FuncInfoFixup = FindFixup(RegistrationVA + 1);
  const auto RuntimeFixup = FindFixup(RegistrationVA + 6);
  ASSERT_NE(FuncInfoFixup, CodeSection->FixupReferences.end());
  ASSERT_NE(RuntimeFixup, CodeSection->FixupReferences.end());
  const size_t InfoIndex = FuncInfoFixup - CodeSection->FixupReferences.begin();
  const size_t RuntimeIndex =
      RuntimeFixup - CodeSection->FixupReferences.begin();
  const auto ParentFixup =
      llvm::find_if(CodeSection->FixupReferences, [&](const auto &F) {
        return !F.IsPCRel && F.ResolvedValue == RegistrationVA;
      });
  ASSERT_NE(ParentFixup, CodeSection->FixupReferences.end());
  const size_t ParentIndex = ParentFixup - CodeSection->FixupReferences.begin();
  const va_t ParentField = CodeSection->VA + ParentFixup->Offset;
  neverd::Decoder RegistrationDecode;
  ASSERT_TRUE(RegistrationDecode.init(Arch::X86));
  va_t StoreVA = 0, DisplacementVA = 0;
  uint8_t DisplacementWidth = 0;
  for (va_t Cursor = Row.OwnerVA; Cursor <= ParentField;) {
    DecodedInsn I{};
    ASSERT_TRUE(RegistrationDecode.decodeOne(
        Compiled.Bytes.data() + Cursor - Compiled.BaseVA,
        Compiled.Bytes.size() - (Cursor - Compiled.BaseVA), Cursor, I));
    ASSERT_TRUE(I.Raw && I.Raw->detail && I.Size);
    if (Cursor + I.Size > ParentField) {
      const auto &X = I.Raw->detail->x86;
      StoreVA = Cursor;
      DisplacementVA = Cursor + X.encoding.disp_offset;
      DisplacementWidth = X.encoding.disp_size;
      ASSERT_EQ(Compiled.Bytes[StoreVA - Compiled.BaseVA], 0xc7);
      break;
    }
    Cursor += I.Size;
  }
  ASSERT_TRUE(StoreVA);
  ASSERT_TRUE(DisplacementWidth == 1 || DisplacementWidth == 4);
  const auto SafeSection = llvm::find_if(
      Compiled.Sections, [](const auto &S) { return S.Name == ".sxdata"; });
  ASSERT_NE(SafeSection, Compiled.Sections.end());
  const size_t SafeIndex = SafeSection - Compiled.Sections.begin();
  const auto SafeRow =
      llvm::find_if(SafeSection->SymbolIndexReferences, [&](const auto &R) {
        return R.TargetVA == RegistrationVA;
      });
  ASSERT_NE(SafeRow, SafeSection->SymbolIndexReferences.end());
  const size_t SafeRowIndex =
      SafeRow - SafeSection->SymbolIndexReferences.begin();
  for (unsigned Mutation = 0; Mutation != 33; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Compiled;
    auto &Fixups = Changed.Sections[CodeIndex].FixupReferences;
    auto &Safe = Changed.Sections[SafeIndex];
    auto Write = [&](va_t Address, uint32_t Value) {
      llvm::support::endian::write32le(
          Changed.Bytes.data() + Address - Changed.BaseVA, Value);
    };
    switch (Mutation) {
    case 0:
      Changed.Bytes[RegistrationVA - Changed.BaseVA] = 0xb9;
      break;
    case 1:
      Changed.Bytes[RegistrationVA + 5 - Changed.BaseVA] = 0xe8;
      break;
    case 2:
      Write(RegistrationVA + 1, Source->Cxx->NativeFuncInfoVA);
      break;
    case 3:
      Write(RegistrationVA + 6,
            uint32_t(Source->PersonalityVA - RegistrationVA - 10));
      break;
    case 4:
      Fixups.erase(Fixups.begin() + InfoIndex);
      break;
    case 5:
      Fixups[InfoIndex].Symbol = "foreign.funcinfo";
      break;
    case 6:
      Fixups[InfoIndex].Addend = 4;
      break;
    case 7:
      Fixups[InfoIndex].BitWidth = 16;
      break;
    case 8:
      Fixups[InfoIndex].ResolvedValue++;
      break;
    case 9:
      Fixups.push_back(Fixups[InfoIndex]);
      break;
    case 10:
      Fixups.erase(Fixups.begin() + RuntimeIndex);
      break;
    case 11:
      Fixups[RuntimeIndex].Kind = llvm::FK_Data_2;
      break;
    case 12:
      Fixups[RuntimeIndex].KindName = "FK_Data_2";
      break;
    case 13:
      Fixups[RuntimeIndex].IsPCRel = false;
      break;
    case 14:
      Fixups[RuntimeIndex].IsResolved = false;
      break;
    case 15:
      Fixups[RuntimeIndex].Symbol = "foreign.runtime";
      break;
    case 16:
      Fixups[RuntimeIndex].Addend = 4;
      break;
    case 17:
      Fixups[RuntimeIndex].ResolvedValue++;
      break;
    case 18:
      Fixups.push_back(Fixups[RuntimeIndex]);
      break;
    case 19:
      Fixups[RuntimeIndex].BitWidth = 64;
      break;
    case 20:
      Fixups.erase(Fixups.begin() + ParentIndex);
      break;
    case 21:
      Fixups[ParentIndex].Symbol = "foreign.handler";
      break;
    case 22:
      Write(ParentField, Source->PersonalityVA);
      break;
    case 23:
      Fixups[ParentIndex].Addend = 4;
      break;
    case 24:
      Fixups.push_back(Fixups[ParentIndex]);
      break;
    case 25:
      Safe.SymbolIndexReferences[SafeRowIndex].Symbol = "foreign.handler";
      break;
    case 26:
      Safe.SymbolIndexReferences[SafeRowIndex].TargetVA++;
      break;
    case 27:
      Safe.SymbolIndexReferences.clear();
      break;
    case 28:
      Safe.SymbolIndexReferences[SafeRowIndex].Offset++;
      break;
    case 29:
      Safe.IsAllocated = true;
      break;
    case 30:
      Changed.Bytes[StoreVA - Changed.BaseVA] = 0x81;
      break;
    case 31:
      ++Changed.Bytes[DisplacementVA - Changed.BaseVA];
      break;
    case 32:
      Changed.Bytes[StoreVA + 1 - Changed.BaseVA] ^= 1;
      break;
    }
    auto StillTables = getCheckedCOFFRegistrationCxxTableReceipt(
        *Parent, *Source, *Loaded, Changed);
    ASSERT_TRUE(bool(StillTables)) << llvm::toString(StillTables.takeError());
    auto Rejected = getCheckedCOFFRegistrationCxxHandlerReceipt(
        *Parent, *Source, *Loaded, Changed);
    EXPECT_FALSE(bool(Rejected));
    if (!Rejected)
      llvm::consumeError(Rejected.takeError());
  }
#endif
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Rows = Compiled.WinEHSemanticRecords;
    auto &Changed = Rows[CatchRow - Compiled.WinEHSemanticRecords.begin()];
    switch (Mutation) {
    case 0:
      Changed.X86CxxLayout.reset();
      break;
    case 1:
      Changed.X86CxxLayout->Tables[0].EndVA += 4;
      break;
    case 2:
      Changed.X86CxxLayout->Tables[1].EndVA += 4;
      break;
    case 3:
      Changed.X86CxxLayout->Tables[2].EndVA += 4;
      break;
    case 4:
      Changed.X86CxxLayout->Tables[3].EndVA += 16;
      break;
    case 5:
      Changed.X86CxxLayout->Tables[3].BeginSymbol.clear();
      break;
    case 6:
      Changed.ContainerEndVA += 4;
      break;
    case 7:
      ++Changed.GeneratedState;
      break;
    case 8:
      Changed.X86CxxLayout->Frame[1] = 0;
      break;
    case 9:
      Changed.X86CxxLayout->Frame[2] = Changed.X86CxxLayout->Frame[1];
      break;
    case 10:
      Changed.X86CxxLayout->Frame[3] = 0;
      break;
    case 11:
      Rows.erase(Rows.begin() +
                 (CatchRow - Compiled.WinEHSemanticRecords.begin()));
      break;
    }
    EXPECT_FALSE(llvm::mc_rewrite::validateRewriteWinEHSemanticRecords(
        Rows, Compiled.SourceFunctionOwners, Compiled.FunctionRanges,
        Compiled.FunctionOwnerAddrs));
  }
#else
  EXPECT_EQ(Row.ContainerEndVA, 0u);
  EXPECT_TRUE(Row.ContainerEndSymbol.empty());
#endif
  const auto HandlerRange =
      llvm::find_if(Compiled.FunctionRanges, [&](const auto &Range) {
        return Range.OwnerSymbol == Row.HandlerSymbol &&
               Range.OwnerVA == Row.HandlerVA &&
               Range.BeginVA == Row.HandlerVA &&
               Range.ParentOwnerSymbol == Row.OwnerSymbol &&
               Range.ParentOwnerVA == Row.OwnerVA;
      });
  ASSERT_NE(HandlerRange, Compiled.FunctionRanges.end());
  EXPECT_GT(HandlerRange->EndVA, HandlerRange->BeginVA);
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Ranges = Compiled.FunctionRanges;
    auto &Changed = Ranges[HandlerRange - Compiled.FunctionRanges.begin()];
    switch (Mutation) {
    case 0:
      Changed.ParentOwnerSymbol.clear();
      break;
    case 1:
      ++Changed.ParentOwnerVA;
      break;
    case 2:
      ++Changed.BeginVA;
      break;
    case 3:
      Changed.EndVA = Changed.BeginVA;
      break;
    case 4:
      ++Changed.OwnerVA;
      break;
    case 5:
      Ranges.erase(Ranges.begin() +
                   (HandlerRange - Compiled.FunctionRanges.begin()));
      break;
    }
    EXPECT_FALSE(llvm::mc_rewrite::validateRewriteWinEHSemanticRecords(
        Compiled.WinEHSemanticRecords, Compiled.SourceFunctionOwners, Ranges,
        Compiled.FunctionOwnerAddrs));
  }
#ifdef LLVM_NEVERD_X86_CXX_HANDLER_RECEIPTS
  const std::pair<va_t, va_t> Mapping{Source->CodeRange.Begin, Row.OwnerVA};
  const auto OriginalCompiled = Compiled;
  auto Update = prepareCOFFRegistrationPatch(Binary, *Loaded, Compiled,
                                             {Mapping}, CodeVA, *Module);
  ASSERT_TRUE(bool(Update)) << llvm::toString(Update.takeError());
  ASSERT_TRUE(Update->Apply);
  EXPECT_GT(Compiled.Bytes.size(), OriginalCompiled.Bytes.size());
  auto RawOffset = [&](llvm::ArrayRef<uint8_t> Bytes, uint32_t RVA,
                       size_t Size) -> std::optional<size_t> {
    auto PE =
        locatePEHeaders(const_cast<uint8_t *>(Bytes.data()), Bytes.size());
    std::optional<size_t> Found;
    forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
      if (RVA >= Section.VirtualAddress &&
          rangeInBounds(RVA - Section.VirtualAddress, Size,
                        Section.SizeOfRawData)) {
        const size_t Offset =
            Section.PointerToRawData + RVA - Section.VirtualAddress;
        if (rangeInBounds(Offset, Size, Bytes.size()))
          Found = Offset;
      }
    });
    return Found;
  };
  auto PE = locatePEHeaders(Binary.data(), Binary.size());
  ASSERT_FALSE(PE.Is64);
  ASSERT_FALSE(PE.FileHeader->Characteristics &
               llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED);
  const auto *LoadConfig =
      getPEDataDirectory(PE, llvm::COFF::LOAD_CONFIG_TABLE);
  ASSERT_TRUE(LoadConfig);
  using LC = llvm::object::coff_load_configuration32;
  const uint32_t SafeFieldsRVA =
      LoadConfig->RelativeVirtualAddress + offsetof(LC, SEHandlerTable);
  auto SafeFields = RawOffset(Binary, SafeFieldsRVA, 8);
  ASSERT_TRUE(SafeFields);
  const uint32_t OriginalTable =
      llvm::support::endian::read32le(Binary.data() + *SafeFields);
  const uint32_t OriginalCount =
      llvm::support::endian::read32le(Binary.data() + *SafeFields + 4);
  ASSERT_GT(OriginalCount, 0u);
  auto OriginalSafe =
      RawOffset(Binary, OriginalTable - Loaded->Base, OriginalCount * 4);
  ASSERT_TRUE(OriginalSafe);
  EXPECT_EQ(Update->SafeSEHHandlers.size(), size_t(OriginalCount) + 1);
  for (uint32_t I = 0; I < OriginalCount; ++I)
    EXPECT_TRUE(llvm::is_contained(Update->SafeSEHHandlers,
                                   llvm::support::endian::read32le(
                                       Binary.data() + *OriginalSafe + I * 4)));
  EXPECT_TRUE(llvm::is_contained(Update->SafeSEHHandlers,
                                 uint32_t(RegistrationVA - Loaded->Base)));
  ASSERT_EQ(Update->LoadConfigBytes.size(), 8u);
  EXPECT_EQ(llvm::support::endian::read32le(Update->LoadConfigBytes.data() + 4),
            Update->SafeSEHHandlers.size());
  std::set<uint32_t> Relocations;
  ASSERT_GE(Update->RelocationRVA, CodeVA - Loaded->Base);
  const size_t RelocOffset = Update->RelocationRVA - (CodeVA - Loaded->Base);
  ASSERT_TRUE(rangeInBounds(RelocOffset, Update->RelocationSize,
                            Compiled.Bytes.size()));
  for (size_t Cursor = RelocOffset;
       Cursor < RelocOffset + Update->RelocationSize;) {
    ASSERT_TRUE(rangeInBounds(Cursor, 8, Compiled.Bytes.size()));
    const uint32_t Page =
        llvm::support::endian::read32le(Compiled.Bytes.data() + Cursor);
    const uint32_t Size =
        llvm::support::endian::read32le(Compiled.Bytes.data() + Cursor + 4);
    ASSERT_GE(Size, 8u);
    ASSERT_TRUE(
        rangeInBounds(Cursor, Size, RelocOffset + Update->RelocationSize));
    for (size_t I = 8; I < Size; I += 2) {
      const uint16_t Entry =
          llvm::support::endian::read16le(Compiled.Bytes.data() + Cursor + I);
      if (!Entry)
        continue;
      ASSERT_EQ(Entry >> 12, llvm::COFF::IMAGE_REL_BASED_HIGHLOW);
      EXPECT_TRUE(Relocations.insert(Page + (Entry & 0xfff)).second);
    }
    Cursor += Size;
  }
  for (va_t Field : HandlerReceipt->AbsolutePointerFields)
    EXPECT_TRUE(Relocations.count(uint32_t(Field - Loaded->Base)));
  EXPECT_TRUE(Relocations.count(SafeFieldsRVA));

  auto RejectTransaction = [&](const auto &Input, auto Changed,
                               const llvm::Module &IR, const auto &Mappings,
                               llvm::StringRef Reason) {
    const auto Before = Changed.Bytes;
    auto Failure = prepareCOFFRegistrationPatch(Input, *Loaded, Changed,
                                                Mappings, CodeVA, IR);
    EXPECT_FALSE(bool(Failure));
    if (!Failure)
      EXPECT_NE(llvm::toString(Failure.takeError()).find(Reason.str()),
                std::string::npos);
    EXPECT_EQ(Changed.Bytes, Before);
  };
  const std::vector<std::pair<va_t, va_t>> Mappings{Mapping};
  {
    auto Input = Binary;
    uint32_t NewCount = 0;
    for (uint32_t I = 0; I < OriginalCount; ++I) {
      const uint32_t RVA =
          llvm::support::endian::read32le(Input.data() + *OriginalSafe + I * 4);
      if (RVA != Source->PersonalityVA - Loaded->Base)
        llvm::support::endian::write32le(
            Input.data() + *OriginalSafe + NewCount++ * 4, RVA);
    }
    ASSERT_EQ(NewCount + 1, OriginalCount);
    ASSERT_GT(NewCount, 0u);
    llvm::support::endian::write32le(Input.data() + *SafeFields + 4, NewCount);
    RejectTransaction(Input, OriginalCompiled, *Module, Mappings,
                      "absent from SafeSEH");
  }
  {
    auto Input = Binary;
    auto AlteredPE = locatePEHeaders(Input.data(), Input.size());
    setPEDataDirectory(AlteredPE, llvm::COFF::BASE_RELOCATION_TABLE, 0, 0);
    RejectTransaction(Input, OriginalCompiled, *Module, Mappings,
                      "relocation contract");
  }
  {
    auto Changed = OriginalCompiled;
    Changed.Sections[SafeIndex].SymbolIndexReferences[SafeRowIndex].TargetVA =
        Source->PersonalityVA;
    RejectTransaction(Binary, Changed, *Module, Mappings, "handler");
  }
  {
    auto Edited = llvm::CloneModule(*Module);
    Edited->getFunction(Parent->getName())
        ->removeFnAttr(llvm::RewriteWinX86CxxFrameAttribute);
    RejectTransaction(Binary, OriginalCompiled, *Edited, Mappings, "C++");
  }
  {
    auto Changed = OriginalCompiled;
    bool Added = false;
    for (auto &Section : Changed.Sections) {
      for (const auto &Fixup : Section.FixupReferences) {
        const va_t Field = Section.VA + Fixup.Offset;
        if (Fixup.IsPCRel || Fixup.Kind != llvm::FK_Data_4 ||
            Fixup.BitWidth != 32 || !Fixup.SubtractSymbol.empty() ||
            llvm::is_contained(HandlerReceipt->AbsolutePointerFields, Field) ||
            !rangeInBounds(Fixup.Offset, 5, Section.Size))
          continue;
        auto Extra = Fixup;
        ++Extra.Offset;
        Extra.ResolvedValue = llvm::support::endian::read32le(
            Changed.Bytes.data() + Section.Offset + Extra.Offset);
        Section.FixupReferences.push_back(Extra);
        Added = true;
        break;
      }
      if (Added)
        break;
    }
    ASSERT_TRUE(Added);
    auto StillChecked = getCheckedCOFFRegistrationCxxHandlerReceipt(
        *Parent, *Source, *Loaded, Changed);
    ASSERT_TRUE(bool(StillChecked)) << llvm::toString(StillChecked.takeError());
    RejectTransaction(Binary, Changed, *Module, Mappings,
                      "fixup fields overlap");
  }
  {
    ASSERT_FALSE(Low.RegistrationStates->CalleeContracts.empty());
    auto ChangedMappings = Mappings;
    ChangedMappings.emplace_back(
        Low.RegistrationStates->CalleeContracts[0].Target, Row.OwnerVA);
    RejectTransaction(Binary, OriginalCompiled, *Module, ChangedMappings,
                      "preserved C++ callee");
  }
  {
    auto Runtime =
        coff_loader::getCheckedX86CxxPersonalityABI(*Loaded, *Source);
    ASSERT_TRUE(Runtime);
    auto ChangedMappings = Mappings;
    ChangedMappings.emplace_back(Runtime->RuntimeVA, Row.OwnerVA);
    RejectTransaction(Binary, OriginalCompiled, *Module, ChangedMappings,
                      "preserved C++ CRT");
  }
  {
    auto ChangedMappings = Mappings;
    ChangedMappings.emplace_back(
        Low.RegistrationStates->CalleeContracts[0].Target + 1, Row.OwnerVA);
    RejectTransaction(Binary, OriginalCompiled, *Module, ChangedMappings,
                      "preserved C++ callee");
  }
  {
    const auto Throw = llvm::find_if(
        Low.RegistrationStates->CalleeContracts, [](const auto &Contract) {
          return Contract.CalleeKind ==
                 RegistrationCalleeFrameContract::Kind::PrivateThrow;
        });
    ASSERT_NE(Throw, Low.RegistrationStates->CalleeContracts.end());
    auto ABI = getCheckedX86RegistrationThrowCalleeABI(*Loaded, Throw->Target);
    ASSERT_TRUE(ABI);
    auto ChangedMappings = Mappings;
    ChangedMappings.emplace_back(ABI->ImportVA + 1, Row.OwnerVA);
    RejectTransaction(Binary, OriginalCompiled, *Module, ChangedMappings,
                      "preserved C++ callee");
  }
  {
    auto Expanded = llvm::CloneModule(*Module);
    auto *Unrelated = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getInt32Ty(Context), false),
        llvm::GlobalValue::ExternalLinkage, "unrelated_replacement", *Expanded);
    rewrite_source::setOriginalVA(*Unrelated, Loaded->Entry);
    llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Unrelated));
    B.CreateRet(B.getInt32(0));
    auto Changed =
        compileImageForPatch(*Expanded, Arch::X86, BinaryFormat::COFF, CodeVA,
                             Resolve, Loaded->Base);
    ASSERT_TRUE(Changed.Success);
    ASSERT_TRUE(Changed.Unresolved.empty());
    std::vector<std::pair<va_t, va_t>> ExpandedMappings;
    for (const auto &Owner : Changed.SourceFunctionOwners) {
      if (Owner.Kind !=
          llvm::mc_rewrite::RewriteSourceFunctionOwnerKind::FunctionEntry)
        continue;
      if (Owner.SourceFunction == Parent->getName())
        ExpandedMappings.emplace_back(Source->CodeRange.Begin, Owner.OwnerVA);
      if (Owner.SourceFunction == Unrelated->getName())
        ExpandedMappings.emplace_back(Loaded->Entry, Owner.OwnerVA);
    }
    ASSERT_EQ(ExpandedMappings.size(), 2u);
    auto ExpandedUpdate = prepareCOFFRegistrationPatch(
        Binary, *Loaded, Changed, ExpandedMappings, CodeVA, *Expanded);
    ASSERT_TRUE(bool(ExpandedUpdate))
        << llvm::toString(ExpandedUpdate.takeError());
    EXPECT_TRUE(ExpandedUpdate->Apply);
  }
  {
    auto Input = Binary;
    auto AlteredPE = locatePEHeaders(Input.data(), Input.size());
    AlteredPE.FileHeader->Characteristics |=
        llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED;
    setPEDataDirectory(AlteredPE, llvm::COFF::BASE_RELOCATION_TABLE, 0, 0);
    auto Changed = OriginalCompiled;
    auto Fixed = prepareCOFFRegistrationPatch(Input, *Loaded, Changed, Mappings,
                                              CodeVA, *Module);
    ASSERT_TRUE(bool(Fixed)) << llvm::toString(Fixed.takeError());
    EXPECT_TRUE(Fixed->Apply);
    EXPECT_EQ(Fixed->RelocationRVA, 0u);
    EXPECT_EQ(Fixed->RelocationSize, 0u);
    EXPECT_EQ(Fixed->SafeSEHHandlers, Update->SafeSEHHandlers);
  }
  {
    auto Input = Binary;
    auto AlteredPE = locatePEHeaders(Input.data(), Input.size());
    const auto *Directory =
        getPEDataDirectory(AlteredPE, llvm::COFF::BASE_RELOCATION_TABLE);
    auto Offset =
        RawOffset(Input, Directory->RelativeVirtualAddress, Directory->Size);
    ASSERT_TRUE(Offset);
    bool Removed = false;
    for (size_t Cursor = 0; Cursor < Directory->Size;) {
      const uint32_t Page =
          llvm::support::endian::read32le(Input.data() + *Offset + Cursor);
      const uint32_t Size =
          llvm::support::endian::read32le(Input.data() + *Offset + Cursor + 4);
      for (size_t I = 8; I < Size; I += 2) {
        auto *Field = Input.data() + *Offset + Cursor + I;
        const uint16_t Entry = llvm::support::endian::read16le(Field);
        if (Entry >> 12 == llvm::COFF::IMAGE_REL_BASED_HIGHLOW &&
            Page + (Entry & 0xfff) == SafeFieldsRVA) {
          llvm::support::endian::write16le(Field, 0);
          Removed = true;
        }
      }
      Cursor += Size;
    }
    ASSERT_TRUE(Removed);
    llvm::support::endian::write64le(Input.data() + *SafeFields, 0);
    auto Changed = OriginalCompiled;
    auto Disabled = prepareCOFFRegistrationPatch(Input, *Loaded, Changed,
                                                 Mappings, CodeVA, *Module);
    ASSERT_TRUE(bool(Disabled)) << llvm::toString(Disabled.takeError());
    EXPECT_TRUE(Disabled->SafeSEHHandlers.empty());
    EXPECT_EQ(Disabled->LoadConfigBytes, std::vector<uint8_t>(8, 0));
  }
  ASSERT_EQ(Patcher.appendExecSegment(Binary, Compiled.Bytes, kNdTextSection,
                                      Arch::X86),
            CodeVA);
  auto Entry = RawOffset(Binary, Source->CodeRange.Begin - Loaded->Base, 5);
  ASSERT_TRUE(Entry);
  Binary[*Entry] = 0xe9;
  llvm::support::endian::write32le(
      Binary.data() + *Entry + 1,
      uint32_t(Row.OwnerVA - Source->CodeRange.Begin - 5));
  auto Installed = applyCOFFRegistrationPatch(Binary, *Update);
  ASSERT_FALSE(bool(Installed)) << llvm::toString(std::move(Installed));
  auto Checked = validateCOFFRegistrationPatch(Binary, *Update);
  ASSERT_FALSE(bool(Checked)) << llvm::toString(std::move(Checked));
  ASSERT_EQ(Update->GeneratedCxxGraphs.size(), 1u);
  EXPECT_EQ(Update->GeneratedCxxGraphs[0].NativeFuncInfoVA,
            HandlerReceipt->Tables.FuncInfoVA);
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    auto Changed = *Update;
    if (Mutation == 0)
      Changed.GeneratedCxxGraphs.clear();
    else if (Mutation == 1)
      Changed.EntryEncodings.clear();
    else if (Mutation == 2)
      ++Changed.GeneratedCxxGraphs[0].TryBlocks[0].TryHigh;
    else
      ++Changed.GeneratedCxxGraphs[0].UnwindMap[1].ToState;
    auto Failure = validateCOFFRegistrationPatch(Binary, Changed);
    ASSERT_TRUE(bool(Failure));
    llvm::consumeError(std::move(Failure));
  }
  {
    auto Changed = Binary;
    const auto Field =
        RawOffset(Changed, HandlerReceipt->Tables.FuncInfoVA - Loaded->Base, 4);
    ASSERT_TRUE(Field);
    Changed[*Field] ^= 1;
    auto Rehashed = *Update;
    const auto Section =
        RawOffset(Changed, Rehashed.SectionRVA, Rehashed.SectionSize);
    ASSERT_TRUE(Section);
    Rehashed.SectionSHA256 = llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
        Changed.data() + *Section, Rehashed.SectionSize));
    auto Failure = validateCOFFRegistrationPatch(Changed, Rehashed);
    ASSERT_TRUE(bool(Failure));
    EXPECT_NE(llvm::toString(std::move(Failure)).find("normalized graph"),
              std::string::npos);
  }
  ASSERT_EQ(Update->PatchedEntryRVAs.size(), 1u);
  EXPECT_EQ(Update->PatchedEntryRVAs[0],
            (std::pair<uint32_t, uint32_t>{
                uint32_t(Source->CodeRange.Begin - Loaded->Base),
                uint32_t(Row.OwnerVA - Loaded->Base)}));
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    auto Changed = *Update;
    if (Mutation == 0)
      Changed.PatchedEntryRVAs.clear();
    else if (Mutation == 1)
      Changed.PatchedEntryRVAs.push_back(Changed.PatchedEntryRVAs[0]);
    else if (Mutation == 2)
      ++Changed.PatchedEntryRVAs[0].first;
    else
      ++Changed.PatchedEntryRVAs[0].second;
    auto Failure = validateCOFFRegistrationPatch(Binary, Changed);
    ASSERT_TRUE(bool(Failure));
    llvm::consumeError(std::move(Failure));
  }
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Binary;
    if (Mutation == 0)
      Changed[*Entry] = 0x90;
    else if (Mutation == 1)
      Changed[*Entry + 1] ^= 1;
    else if (Mutation == 2) {
      auto Header = locatePEHeaders(Changed.data(), Changed.size());
      Header.FileHeader->Machine = llvm::COFF::IMAGE_FILE_MACHINE_ARMNT;
    } else {
      auto Header = locatePEHeaders(Changed.data(), Changed.size());
      forEachPESection(Header, [&](const PESectionFields &Section, uint16_t I) {
        if (Source->CodeRange.Begin - Loaded->Base >= Section.VirtualAddress &&
            Source->CodeRange.Begin - Loaded->Base - Section.VirtualAddress <
                Section.VirtualSize) {
          auto *RawSection = reinterpret_cast<llvm::object::coff_section *>(
                                 Header.SectionTable) +
                             I;
          RawSection->Characteristics =
              uint32_t(RawSection->Characteristics) &
              ~uint32_t(llvm::COFF::IMAGE_SCN_MEM_EXECUTE);
        }
      });
    }
    auto Failure = validateCOFFRegistrationPatch(Changed, *Update);
    ASSERT_TRUE(bool(Failure));
    llvm::consumeError(std::move(Failure));
  }
  auto FinalImage = validatePatchedCOFFImage(Binary, Arch::X86, false);
  ASSERT_FALSE(bool(FinalImage)) << llvm::toString(std::move(FinalImage));
  {
    using LC = llvm::object::coff_load_configuration32;
    auto Fields = RawOffset(Binary, Update->LoadConfigRVA,
                            Update->LoadConfigDeclaredSize);
    ASSERT_TRUE(Fields);
    ASSERT_GE(Update->LoadConfigDeclaredSize, offsetof(LC, GuardFlags) + 4);
    const uint32_t Flags[] = {
        uint32_t(llvm::COFF::GuardFlags::CF_FUNCTION_TABLE_PRESENT),
        uint32_t(llvm::COFF::GuardFlags::EH_CONTINUATION_TABLE_PRESENT)};
    for (bool CF : {false, true}) {
      SCOPED_TRACE(CF);
      const size_t Table = CF ? offsetof(LC, GuardCFFunctionTable)
                              : offsetof(LC, GuardEHContinuationTable);
      const size_t Count = CF ? offsetof(LC, GuardCFFunctionCount)
                              : offsetof(LC, GuardEHContinuationCount);
      auto Valid = Binary;
      // The compatibility directory may remain64. The appended structure's
      // declared size owns these later fields, and both readers must see them.
      llvm::support::endian::write32le(
          Valid.data() + *Fields + offsetof(LC, GuardFlags), Flags[CF ? 0 : 1]);
      llvm::support::endian::write32le(
          Valid.data() + *Fields + Table,
          llvm::support::endian::read32le(Update->LoadConfigBytes.data()));
      llvm::support::endian::write32le(Valid.data() + *Fields + Count,
                                       Update->SafeSEHHandlers.size());
      auto Complete = validatePatchedCOFFImage(Valid, Arch::X86, false);
      ASSERT_FALSE(bool(Complete)) << llvm::toString(std::move(Complete));
      llvm::support::endian::write32le(Valid.data() + *Fields + Table, 0);
      auto Missing = validatePatchedCOFFImage(Valid, Arch::X86, false);
      ASSERT_TRUE(bool(Missing));
      EXPECT_NE(
          llvm::toString(std::move(Missing)).find("invalid pointer/count"),
          std::string::npos);
    }
    uint32_t BeyondRaw = 0;
    auto FinalPE = locatePEHeaders(Binary.data(), Binary.size());
    forEachPESection(FinalPE, [&](const PESectionFields &Section, uint16_t) {
      if (Update->LoadConfigRVA >= Section.VirtualAddress &&
          Update->LoadConfigRVA - Section.VirtualAddress <
              Section.SizeOfRawData)
        BeyondRaw = Section.SizeOfRawData -
                    (Update->LoadConfigRVA - Section.VirtualAddress) + 1;
    });
    ASSERT_GT(BeyondRaw, Update->LoadConfigDeclaredSize);
    for (uint32_t Declared : {uint32_t(Binary.size() * 2), BeyondRaw}) {
      auto Truncated = Binary;
      llvm::support::endian::write32le(Truncated.data() + *Fields, Declared);
      auto Missing = validatePatchedCOFFImage(Truncated, Arch::X86, false);
      ASSERT_TRUE(bool(Missing));
      EXPECT_NE(llvm::toString(std::move(Missing)).find("declared load"),
                std::string::npos);
    }
  }
  const auto FinalSafeFields = RawOffset(Binary, SafeFieldsRVA, 8);
  ASSERT_TRUE(FinalSafeFields);
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Binary;
    auto ChangedPE = locatePEHeaders(Changed.data(), Changed.size());
    switch (Mutation) {
    case 0: {
      auto Offset = RawOffset(Changed, RegistrationVA - Loaded->Base, 1);
      ASSERT_TRUE(Offset);
      Changed[*Offset] ^= 1;
      break;
    }
    case 1:
      setPEDataDirectory(ChangedPE, llvm::COFF::BASE_RELOCATION_TABLE, 0, 0);
      break;
    case 2:
      Changed[*FinalSafeFields + 4] ^= 1;
      break;
    case 3: {
      const uint32_t Table =
          llvm::support::endian::read32le(Changed.data() + *FinalSafeFields);
      auto Offset = RawOffset(Changed, Table - Loaded->Base, 4);
      ASSERT_TRUE(Offset);
      Changed[*Offset] ^= 1;
      break;
    }
    case 4: {
      auto Offset = RawOffset(Changed, Update->LoadConfigRVA, 4);
      ASSERT_TRUE(Offset);
      Changed[*Offset] ^= 1;
      break;
    }
    }
    auto Failure = validateCOFFRegistrationPatch(Changed, *Update);
    EXPECT_TRUE(bool(Failure));
    llvm::consumeError(std::move(Failure));
  }
  Patcher.setImageContext(&*Loaded);
  for (bool Collision : {false, true}) {
    auto ProductModule = llvm::CloneModule(*Module);
    if (Collision) {
      llvm::Function *Callee = nullptr;
      for (auto &Function : *ProductModule) {
        if (Function.isDeclaration())
          continue;
        for (auto &Block : Function)
          for (auto &I : Block) {
            const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
            auto *Target = Call ? Call->getCalledFunction() : nullptr;
            if (Callee || !Target || !Target->isDeclaration() ||
                Target->isIntrinsic())
              continue;
            auto Address = rewrite_source::getOriginalVA(*Target);
            ASSERT_TRUE(bool(Address)) << llvm::toString(Address.takeError());
            if (*Address)
              Callee = Target;
          }
      }
      ASSERT_TRUE(Callee);
      std::string ImportName;
      for (const auto &Import : Loaded->Imports)
        if (!Import.Name.empty() && !ProductModule->getFunction(Import.Name)) {
          ImportName = Import.Name;
          break;
        }
      ASSERT_FALSE(ImportName.empty());
      Callee->setName(ImportName);
    }
    const char *Output =
        std::getenv(Collision ? "NEVERD_REGISTRATION_OUTPUT_CXX_COLLISION_PE32"
                              : "NEVERD_REGISTRATION_OUTPUT_CXX_PRODUCT_PE32");
    llvm::SmallString<128> Temporary;
    if (!Output) {
      auto Error = llvm::sys::fs::createTemporaryFile("neverd-registration-cxx",
                                                      "exe", Temporary);
      ASSERT_FALSE(bool(Error)) << Error.message();
      Output = Temporary.c_str();
    }
    const auto Result = Patcher.patch(Path, Output, *ProductModule, Arch::X86);
    ASSERT_TRUE(Result.Success);
    EXPECT_EQ(Result.TrampolineCount, 1u);
    EXPECT_EQ(Result.PatchedOriginalEntries,
              (std::vector<va_t>{Source->CodeRange.Begin}));
    auto ProductBuffer = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(ProductBuffer));
    const auto ProductBytes = llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t *>((*ProductBuffer)->getBufferStart()),
        (*ProductBuffer)->getBufferSize());
    // Both public routes compile this same authenticated IR. Require the exact
    // generated section, SafeSEH and relocation closure already checked above.
    auto ProductContract = validateCOFFRegistrationPatch(ProductBytes, *Update);
    ASSERT_FALSE(bool(ProductContract))
        << llvm::toString(std::move(ProductContract));
    auto Reloaded = COFFLoader().load(Output);
    ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
    const auto GeneratedGraph = llvm::find_if(
        Reloaded->ExceptionMetadata.Functions, [&](const auto &EH) {
          return EH.Cxx && EH.Cxx->NativeFuncInfoVA ==
                               Update->GeneratedCxxGraphs[0].NativeFuncInfoVA;
        });
    ASSERT_NE(GeneratedGraph, Reloaded->ExceptionMetadata.Functions.end());
    EXPECT_EQ(*GeneratedGraph->Cxx, Update->GeneratedCxxGraphs[0]);
    EXPECT_EQ(GeneratedGraph->ParseStatus, ExceptionParseStatus::Complete);
    ASSERT_TRUE(GeneratedGraph->Registration);
    const auto &GeneratedChain = *GeneratedGraph->Registration;
    ASSERT_TRUE(GeneratedChain.RealignedFrame);
    const auto &GeneratedFrame = *GeneratedChain.RealignedFrame;
    EXPECT_EQ(GeneratedFrame.BaseRegister, 6u);
    EXPECT_EQ(GeneratedFrame.DefinitionVA,
              GeneratedGraph->CodeRange.Begin + 15);
    EXPECT_EQ(GeneratedFrame.Alignment, 16u);
    EXPECT_LE(-int64_t(GeneratedFrame.BaseOffset),
              GeneratedFrame.AllocationBytes);
    EXPECT_EQ(GeneratedFrame.SavedParentFrameOffset, -20);
    EXPECT_EQ(GeneratedChain.RegistrationOffset, -12);
    EXPECT_EQ(GeneratedChain.TryLevelOffset, -4);
    EXPECT_EQ(GeneratedChain.chainInstallInstructionSize(), 6u);
    ASSERT_FALSE(GeneratedChain.TryLevelStores.empty());
    EXPECT_EQ(GeneratedChain.TryLevelStores.front().Level, -1);
    EXPECT_TRUE(
        llvm::any_of(GeneratedChain.TryLevelStores,
                     [](const auto &Store) { return Store.Level >= 0; }));
    LowFunc CoordinateOnly;
    CoordinateOnly.ExceptionMetadata = *GeneratedGraph;
    const auto UnprovedStates = analyzeRegistrationStates(CoordinateOnly);
    EXPECT_FALSE(UnprovedStates.Complete);
    EXPECT_FALSE(UnprovedStates.ChainOperationsComplete);
    EXPECT_TRUE(
        llvm::any_of(UnprovedStates.Diagnostics, [](const auto &Message) {
          return Message.find("coordinate transfer proof") != std::string::npos;
        }));
    neverd::Decoder ReliftDecoder;
    ASSERT_TRUE(ReliftDecoder.init(*Reloaded));
    auto Relift = CFGBuilder().build(*Reloaded, ReliftDecoder,
                                     GeneratedGraph->CodeRange.Begin,
                                     "realigned-generated-parent");
    ASSERT_TRUE(Relift.RegistrationStates);
    const auto &ReliftStates = *Relift.RegistrationStates;
    const auto Installed =
        llvm::find_if(ReliftStates.Blocks, [&](const auto &B) {
          return B.Range.Begin == GeneratedChain.ChainInstallVA + 6;
        });
    ASSERT_NE(Installed, ReliftStates.Blocks.end());
    EXPECT_TRUE(Installed->Reached);
    EXPECT_TRUE(Installed->CanDispatch);
    EXPECT_FALSE(Installed->Unknown);
    // Ordinary coordinate transfer does not prove the generated catch's
    // distinct runtime restore and continuation protocol.
    EXPECT_FALSE(ReliftStates.CxxContinuationsComplete);
    // These generated cleanup funclets share the recovered function range.
    // Frame coordinates alone do not establish their source unwind contract.
    const auto GeneratedSource = classifyWindowsEHNativeSource(
        *GeneratedGraph, Arch::X86, BinaryFormat::COFF);
    EXPECT_FALSE(GeneratedSource.canPatchOutput());
    EXPECT_EQ(GeneratedSource.Reason,
              WindowsEHNativeSourceReason::UnsupportedCxxUnwindAction);
    EXPECT_TRUE(
        llvm::any_of(GeneratedGraph->Cxx->UnwindMap, [&](const auto &Action) {
          return Action.ActionVA &&
                 GeneratedGraph->CodeRange.contains(Action.ActionVA);
        }));
    for (unsigned Byte : {3u, 8u, 16u, 17u, 23u, 39u, 62u}) {
      SCOPED_TRACE(Byte);
      BinaryImage Disproved = *Reloaded;
      const va_t Address = GeneratedGraph->CodeRange.Begin + Byte;
      bool Changed = false;
      for (auto &Segment : Disproved.Segments)
        if (Segment.VA <= Address &&
            Address - Segment.VA < Segment.Data.size()) {
          Segment.Data[Address - Segment.VA] ^= 1;
          Changed = true;
          break;
        }
      ASSERT_TRUE(Changed);
      Disproved.ExceptionMetadata = {};
      Disproved.VerifiedFunctionEntries.clear();
      coff_loader::parseX86RegistrationExceptions(Disproved);
      for (const auto &Candidate : Disproved.ExceptionMetadata.Functions)
        if (Candidate.Cxx && Candidate.Cxx->NativeFuncInfoVA ==
                                 GeneratedGraph->Cxx->NativeFuncInfoVA) {
          ASSERT_TRUE(Candidate.Registration);
          EXPECT_FALSE(Candidate.Registration->RealignedFrame);
          EXPECT_FALSE(classifyWindowsEHNativeSource(Candidate, Arch::X86,
                                                     BinaryFormat::COFF)
                           .canPatchOutput());
        }
    }
    if (!Temporary.empty())
      EXPECT_FALSE(bool(llvm::sys::fs::remove(Temporary)));
  }
  if (const char *Output = std::getenv("NEVERD_REGISTRATION_OUTPUT_CXX_PE32")) {
    std::error_code EC;
    llvm::raw_fd_ostream Stream(Output, EC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    Stream.write(reinterpret_cast<const char *>(Binary.data()), Binary.size());
  }
  if (const char *Output =
          std::getenv("NEVERD_REGISTRATION_OUTPUT_CXX_RECEIPT")) {
    const auto ParentRange =
        llvm::find_if(OriginalCompiled.FunctionRanges, [&](const auto &Range) {
          return Range.OwnerVA == Row.OwnerVA && Range.BeginVA == Row.OwnerVA &&
                 Range.ParentOwnerSymbol.empty();
        });
    ASSERT_NE(ParentRange, OriginalCompiled.FunctionRanges.end());
    llvm::json::Array Fields;
    for (va_t Field : HandlerReceipt->AbsolutePointerFields)
      Fields.push_back(llvm::json::Object{
          {"rva", Field - Loaded->Base},
          {"value", llvm::support::endian::read32le(
                        OriginalCompiled.Bytes.data() + Field - CodeVA)}});
    llvm::json::Object Receipt{
        {"schema", 1},
        {"evidence", "checked-cxx-manual-installation"},
        {"image_base", Loaded->Base},
        {"source_entry_rva", Source->CodeRange.Begin - Loaded->Base},
        {"generated_code_begin_rva", ParentRange->BeginVA - Loaded->Base},
        {"generated_code_end_rva", ParentRange->EndVA - Loaded->Base},
        {"registration_handler_rva", RegistrationVA - Loaded->Base},
        {"func_info_rva", HandlerReceipt->Tables.FuncInfoVA - Loaded->Base},
        {"absolute_pointer_fields", std::move(Fields)},
        {"image_sha256", llvm::toHex(llvm::SHA256::hash(Binary), true)},
        {"source_image_sha256",
         llvm::toHex(llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
                         reinterpret_cast<const uint8_t *>(InputBytes.data()),
                         InputBytes.size())),
                     true)}};
    std::error_code EC;
    llvm::raw_fd_ostream Stream(Output, EC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    Stream << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(Receipt)));
  }
#endif
}
#endif

TEST(WindowsRegistrationNative, InputPE32PreservesItsCheckedSourceContract) {
  const char *Path = std::getenv("NEVERD_REGISTRATION_INPUT_PE32");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_REGISTRATION_INPUT_PE32 to the runtime fixture";
  auto Loaded = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto &Image = *Loaded;
  for (const auto &Diagnostic : Image.ExceptionMetadata.Diagnostics)
    llvm::errs() << Diagnostic << '\n';
  ASSERT_EQ(Image.ExceptionMetadata.Functions.size(), 1u);
  const auto &EH = Image.ExceptionMetadata.Functions.front();
  for (const auto &Diagnostic : EH.Diagnostics)
    llvm::errs() << Diagnostic << '\n';
  ASSERT_TRUE(EH.Registration);
  ASSERT_EQ(EH.ParseStatus, ExceptionParseStatus::Complete);
  Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image));
  auto Low = CFGBuilder().build(Image, Decoder, EH.CodeRange.Begin, "guarded");
  ASSERT_TRUE(Low.RegistrationStates);
  EXPECT_TRUE(Low.RegistrationStates->Complete);
  EXPECT_TRUE(Low.RegistrationStates->RegistrationLifetimeComplete);
  EXPECT_TRUE(Low.RegistrationStates->ChainOperationsComplete);
  for (const auto &Diagnostic : Low.RegistrationStates->Diagnostics)
    llvm::errs() << Diagnostic << '\n';
  if (!Low.RegistrationStates->Complete)
    for (const auto &State : Low.RegistrationStates->Blocks) {
      llvm::errs() << "block " << State.BlockId << " @ "
                   << llvm::format_hex(State.Range.Begin, 10)
                   << " unknown=" << State.Unknown
                   << " callback=" << State.CallbackOnly << " levels=";
      for (auto Level : State.Levels)
        llvm::errs() << Level << ',';
      llvm::errs() << '\n';
    }
  ASSERT_TRUE(Low.RegistrationStates->Complete);
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  auto Module = Emitter.emit({Med}, Context, "input-registration", Arch::X86,
                             {}, &Image, BinaryFormat::COFF);
  ASSERT_TRUE(Module);
  auto *Parent = Module->getFunction("guarded");
  ASSERT_TRUE(Parent);
  if (!Parent->getMetadata(windows_eh_md::NativeAttachment))
    Module->print(llvm::errs(), nullptr);
  ASSERT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  if (const char *Output = std::getenv("NEVERD_REGISTRATION_OUTPUT_IR")) {
    std::error_code EC;
    llvm::raw_fd_ostream Stream(Output, EC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    Module->print(Stream, nullptr);
  }
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  auto Error = validateCOFFRegistrationIR(*Parent, EH, Image);
  if (Error)
    Module->print(llvm::errs(), nullptr);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  auto RejectMutation = [&](llvm::StringRef Label, auto Mutate,
                            llvm::StringRef Diagnostic) {
    SCOPED_TRACE(Label.str());
    auto Altered = llvm::CloneModule(*Module);
    auto *Function = Altered->getFunction(Parent->getName());
    ASSERT_TRUE(Function);
    ASSERT_TRUE(Mutate(*Function));
    ASSERT_FALSE(llvm::verifyModule(*Altered, &llvm::errs()));
    auto Failure = validateCOFFRegistrationIR(*Function, EH, Image);
    ASSERT_TRUE(bool(Failure));
    const auto Message = llvm::toString(std::move(Failure));
    EXPECT_NE(Message.find(Diagnostic.str()), std::string::npos) << Message;
  };
  RejectMutation(
      "edited LLVM cannot overwrite either registration scope format",
      [&](llvm::Function &F) {
        llvm::IRBuilder<> B(F.getEntryBlock().getTerminator());
        const std::string Name =
            "__nd_data_" + llvm::utohexstr(EH.Registration->ScopeTableVA);
        auto *Target = F.getParent()->getNamedGlobal(Name);
        if (!Target)
          Target = new llvm::GlobalVariable(
              *F.getParent(), B.getInt32Ty(), false,
              llvm::GlobalValue::ExternalLinkage, nullptr, Name);
        B.CreateStore(B.getInt32(42), Target)->setVolatile(true);
        return true;
      },
      "registration scope table or cookie is written");
  if (EH.Personality == ExceptionPersonality::ExceptHandler4) {
    ASSERT_TRUE(Low.RegistrationStates->SecurityCookiesComplete);
    EXPECT_EQ(Low.RegistrationStates->SecurityCookieVA,
              Image.Base + Image.DynInfo.SecurityCookieRVA);
    for (unsigned Mutation = 0; Mutation != 3; ++Mutation)
      RejectMutation(
          "compiler cookie retains the exact external image storage",
          [Mutation](llvm::Function &F) {
            auto *Cookie = F.getParent()->getNamedGlobal("__security_cookie");
            if (!Cookie)
              return false;
            if (Mutation == 0)
              Cookie->setInitializer(llvm::ConstantInt::get(
                  llvm::Type::getInt32Ty(F.getContext()), 42));
            if (Mutation == 1)
              Cookie->setConstant(true);
            if (Mutation == 2)
              Cookie->setDLLStorageClass(
                  llvm::GlobalValue::DLLImportStorageClass);
            return true;
          },
          "cookie frame, image storage or CRT wrapper");
    for (bool Table : {false, true})
      RejectMutation(
          "edited LLVM cannot overwrite the EH4 runtime contract",
          [&](llvm::Function &F) {
            auto *Cookie = F.getParent()->getNamedGlobal("__security_cookie");
            if (!Cookie)
              return false;
            llvm::IRBuilder<> B(F.getEntryBlock().getTerminator());
            llvm::Value *Target = Cookie;
            if (Table)
              Target = new llvm::GlobalVariable(
                  *F.getParent(), B.getInt32Ty(), false,
                  llvm::GlobalValue::ExternalLinkage, nullptr,
                  "__nd_data_" +
                      llvm::utohexstr(EH.Registration->ScopeTableVA));
            B.CreateStore(B.getInt32(42), Target)->setVolatile(true);
            return true;
          },
          "registration scope table or cookie is written");
    if (EH.Registration->GSCookieOffset != -2) {
      for (unsigned Mutation = 0; Mutation != 4; ++Mutation)
        RejectMutation(
            "compiler GS checker keeps its ABI and authenticated identity",
            [Mutation](llvm::Function &F) {
              auto *Check =
                  F.getParent()->getFunction("__security_check_cookie");
              if (!Check)
                return false;
              if (Mutation == 0)
                Check->setCallingConv(llvm::CallingConv::C);
              if (Mutation == 1)
                Check->removeParamAttr(0, llvm::Attribute::InReg);
              if (Mutation == 2) {
                Check->setDSOLocal(false);
                Check->setDLLStorageClass(
                    llvm::GlobalValue::DLLImportStorageClass);
              }
              if (Mutation == 3)
                rewrite_source::setOriginalVA(*Check, 0x401000);
              return true;
            },
            "EH4 GS cookie checker");
      if (!Low.RegistrationStates->CookieChecks.empty())
        for (unsigned Mutation = 0; Mutation != 4; ++Mutation)
          RejectMutation(
              "source GS check replacement retains its exact occurrence",
              [Mutation](llvm::Function &F) {
                for (auto &Block : F)
                  for (auto &I : Block)
                    if (auto *MD = I.getMetadata(
                            windows_eh_md::RegistrationOperationAttachment);
                        MD && MD->getNumOperands() == 5) {
                      const auto *Kind =
                          llvm::dyn_cast<llvm::ConstantAsMetadata>(
                              MD->getOperand(4));
                      const auto *Number =
                          Kind ? llvm::dyn_cast<llvm::ConstantInt>(
                                     Kind->getValue())
                               : nullptr;
                      if (!Number || Number->getZExtValue() != 3)
                        continue;
                      if (Mutation == 0)
                        I.eraseFromParent();
                      if (Mutation == 1)
                        I.setMetadata(
                            windows_eh_md::RegistrationOperationAttachment,
                            nullptr);
                      if (Mutation == 2)
                        I.clone()->insertBefore(I.getIterator());
                      if (Mutation == 3) {
                        llvm::SmallVector<llvm::Metadata *, 5> Fields;
                        for (unsigned N = 0; N != 4; ++N)
                          Fields.push_back(MD->getOperand(N));
                        Fields.push_back(llvm::ConstantAsMetadata::get(
                            llvm::ConstantInt::get(
                                llvm::Type::getInt8Ty(F.getContext()), 2)));
                        I.setMetadata(
                            windows_eh_md::RegistrationOperationAttachment,
                            llvm::MDNode::get(F.getContext(), Fields));
                      }
                      return true;
                    }
                return false;
              },
              "execution");
    }
  }
  RejectMutation(
      "unmarked image memory read cannot bypass source occurrence checks",
      [](llvm::Function &F) {
        llvm::IRBuilder<> B(F.getEntryBlock().getTerminator());
        auto *Global =
            new llvm::GlobalVariable(*F.getParent(), B.getInt32Ty(), false,
                                     llvm::GlobalValue::ExternalLinkage,
                                     nullptr, "__nd_data_403000.unproved-read");
        B.CreateLoad(B.getInt32Ty(), Global)->setVolatile(true);
        return true;
      },
      "unproved LLVM escape or access");
  for (bool Atomic : {false, true})
    RejectMutation(
        "unindexed image memory operation cannot launder a caller address",
        [Atomic](llvm::Function &F) {
          llvm::IRBuilder<> B(F.getEntryBlock().getTerminator());
          auto *Global = new llvm::GlobalVariable(
              *F.getParent(), B.getInt32Ty(), false,
              llvm::GlobalValue::ExternalLinkage, nullptr,
              "__nd_data_403000.unproved-memory");
          if (Atomic)
            B.CreateAtomicRMW(llvm::AtomicRMWInst::Add, Global, B.getInt32(0),
                              llvm::Align(4),
                              llvm::AtomicOrdering::SequentiallyConsistent);
          else
            B.CreateMemCpy(Global, llvm::Align(1), Global, llvm::Align(1), 4);
          return true;
        },
        "unproved LLVM escape or access");
  RejectMutation(
      "changed parent stack cleanup",
      [](llvm::Function &F) {
        F.setCallingConv(llvm::CallingConv::X86_StdCall);
        return true;
      },
      "stack cleanup ABI");
  bool HasNormalFinally = false;
  for (const auto &Block : *Parent)
    for (const auto &I : Block)
      HasNormalFinally |=
          I.getMetadata(windows_eh_md::RegistrationFinallyCallAttachment) !=
          nullptr;
  if (HasNormalFinally)
    for (unsigned Mutation = 0; Mutation != 3; ++Mutation)
      RejectMutation(
          "changed ordinary finally entry protocol",
          [Mutation](llvm::Function &F) {
            for (auto &Block : F)
              for (auto &I : Block)
                if (I.getMetadata(
                        windows_eh_md::RegistrationFinallyCallAttachment)) {
                  auto *Call = llvm::cast<llvm::CallBase>(&I);
                  if (Mutation == 0)
                    I.setMetadata(
                        windows_eh_md::RegistrationFinallyCallAttachment,
                        nullptr);
                  else if (Mutation == 1)
                    Call->setArgOperand(
                        0, llvm::ConstantInt::get(
                               llvm::Type::getInt8Ty(F.getContext()), 1));
                  else
                    Call->setArgOperand(
                        1, llvm::ConstantPointerNull::get(
                               llvm::PointerType::get(F.getContext(), 0)));
                  return true;
                }
            return false;
          },
          Mutation == 0 ? "no recovered callback" : "normal parent-frame ABI");
  bool HasIncomingFrame = false;
  for (const auto &F : *Module)
    for (const auto &Block : F)
      for (const auto &I : Block)
        HasIncomingFrame |=
            I.getMetadata(windows_eh_md::RegistrationIncomingFrameAttachment) !=
            nullptr;
  if (HasIncomingFrame)
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation)
      RejectMutation(
          "changed incoming caller stack projection",
          [Mutation](llvm::Function &F) {
            for (auto &Function : *F.getParent())
              for (auto &Block : Function)
                for (auto &I : Block)
                  if (I.getMetadata(
                          windows_eh_md::RegistrationIncomingFrameAttachment)) {
                    if (Mutation == 0)
                      I.setMetadata(
                          windows_eh_md::RegistrationIncomingFrameAttachment,
                          nullptr);
                    else {
                      auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I);
                      auto *Pointer = Load ? Load->getPointerOperand()
                                           : llvm::cast<llvm::StoreInst>(&I)
                                                 ->getPointerOperand();
                      auto *GEP = llvm::cast<llvm::GetElementPtrInst>(Pointer);
                      if (Mutation == 4) {
                        if (!Load)
                          continue;
                        auto *Base = llvm::cast<llvm::Instruction>(
                            GEP->getPointerOperand());
                        auto *Term = Function.getEntryBlock().getTerminator();
                        Base->moveBefore(Term->getIterator());
                        GEP->moveBefore(Term->getIterator());
                        I.moveBefore(Term->getIterator());
                      } else if (Mutation == 3) {
                        if (Load)
                          Load->setAlignment(llvm::Align(16));
                        else
                          llvm::cast<llvm::StoreInst>(&I)->setAlignment(
                              llvm::Align(16));
                      } else if (Mutation == 1)
                        GEP->setOperand(
                            1, llvm::ConstantInt::get(
                                   llvm::Type::getInt32Ty(F.getContext()), 12));
                      else
                        GEP->setIsInBounds(true);
                    }
                    return true;
                  }
            return false;
          },
          Mutation == 0   ? "access set is incomplete"
          : Mutation == 3 ? "source memory"
          : Mutation == 4 ? "execution segment"
                          : "physical stack projection");
  if (HasIncomingFrame)
    RejectMutation(
        "swapped incoming recipes keep their original execution events",
        [](llvm::Function &F) {
          std::vector<llvm::Instruction *> Loads;
          for (auto &Function : *F.getParent())
            for (auto &Block : Function)
              for (auto &I : Block)
                if (llvm::isa<llvm::LoadInst>(I) &&
                    I.getMetadata(
                        windows_eh_md::RegistrationIncomingFrameAttachment))
                  Loads.push_back(&I);
          if (Loads.size() < 2)
            return false;
          auto *A = Loads[0]->getMetadata(
              windows_eh_md::RegistrationIncomingFrameAttachment);
          auto *B = Loads[1]->getMetadata(
              windows_eh_md::RegistrationIncomingFrameAttachment);
          Loads[0]->setMetadata(
              windows_eh_md::RegistrationIncomingFrameAttachment, B);
          Loads[1]->setMetadata(
              windows_eh_md::RegistrationIncomingFrameAttachment, A);
          auto *PA = llvm::cast<llvm::GetElementPtrInst>(
              llvm::cast<llvm::LoadInst>(Loads[0])->getPointerOperand());
          auto *PB = llvm::cast<llvm::GetElementPtrInst>(
              llvm::cast<llvm::LoadInst>(Loads[1])->getPointerOperand());
          auto *Offset = PA->getOperand(1);
          PA->setOperand(1, PB->getOperand(1));
          PB->setOperand(1, Offset);
          return true;
        },
        "source execution occurrence");
  RejectMutation(
      "changed preserved callee stack cleanup",
      [](llvm::Function &F) {
        for (auto &Block : F)
          for (auto &I : Block)
            if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I))
              if (auto *Callee = Call->getCalledFunction();
                  Callee && Callee->isDeclaration() && !Callee->isIntrinsic()) {
                Callee->setCallingConv(llvm::CallingConv::X86_StdCall);
                return true;
              }
        return false;
      },
      "stack cleanup ABI");
  if (EH.Registration->Scopes.size() > 1 &&
      !EH.Registration->Scopes.back().IsFinally)
    RejectMutation(
        "nested handler bypasses outer scope entry",
        [](llvm::Function &F) {
          for (auto &Block : F)
            if (auto *Return = llvm::dyn_cast<llvm::CatchReturnInst>(
                    Block.getTerminator()))
              if (auto *Enter = llvm::dyn_cast<llvm::InvokeInst>(
                      Return->getSuccessor()->getTerminator());
                  Enter && Enter->getCalledFunction() &&
                  Enter->getCalledFunction()->getIntrinsicID() ==
                      llvm::Intrinsic::seh_scope_begin) {
                Return->setSuccessor(Enter->getNormalDest());
                return true;
              }
          return false;
        },
        "scope entry");
  RejectMutation(
      "missing logical frame identity",
      [](llvm::Function &F) {
        for (auto &B : F)
          for (auto &I : B)
            if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment)) {
              I.setMetadata(windows_eh_md::RegistrationFrameAttachment,
                            nullptr);
              return true;
            }
        return false;
      },
      "compiler-owned identity");
  RejectMutation(
      "bypass asynchronous scope entry",
      [](llvm::Function &F) {
        for (auto &B : F)
          if (auto *Invoke =
                  llvm::dyn_cast<llvm::InvokeInst>(B.getTerminator());
              Invoke && Invoke->getCalledFunction() &&
              Invoke->getCalledFunction()->getIntrinsicID() ==
                  llvm::Intrinsic::seh_scope_begin) {
            for (auto &Predecessor : F) {
              auto *Terminator = Predecessor.getTerminator();
              for (unsigned I = 0; I < Terminator->getNumSuccessors(); ++I)
                if (Terminator->getSuccessor(I) == &B) {
                  Terminator->setSuccessor(I, Invoke->getNormalDest());
                  return true;
                }
            }
          }
        return false;
      },
      "coff registration patch:");
  for (bool Write : {false, true})
    RejectMutation(
        Write ? "ordinary store before logical frame"
              : "ordinary load after logical frame",
        [Write](llvm::Function &F) {
          llvm::AllocaInst *Frame = nullptr;
          for (auto &B : F)
            for (auto &I : B)
              if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment))
                Frame = llvm::dyn_cast<llvm::AllocaInst>(&I);
          if (!Frame)
            return false;
          for (auto &Block : F)
            if (auto *Invoke =
                    llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
                Invoke && Invoke->getCalledFunction() &&
                Invoke->getCalledFunction()->getIntrinsicID() ==
                    llvm::Intrinsic::seh_scope_begin) {
              llvm::IRBuilder<> B(
                  &*Invoke->getNormalDest()->getFirstInsertionPt());
              auto Bytes =
                  Frame->getAllocationSize(F.getParent()->getDataLayout());
              if (!Bytes || Bytes->isScalable())
                return false;
              auto *Address = B.CreateGEP(
                  B.getInt8Ty(), Frame,
                  B.getInt32(Write ? uint32_t(-4) : Bytes->getFixedValue()));
              if (Write)
                B.CreateStore(B.getInt32(0), Address);
              else
                B.CreateLoad(B.getInt32Ty(), Address);
              return true;
            }
          return false;
        },
        "unproved LLVM escape or access");
  RejectMutation(
      "truncated frame pointer laundering",
      [](llvm::Function &F) {
        llvm::AllocaInst *Frame = nullptr;
        for (auto &Block : F)
          for (auto &I : Block)
            if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment))
              Frame = llvm::dyn_cast<llvm::AllocaInst>(&I);
        if (!Frame)
          return false;
        for (auto &Block : F)
          if (auto *Invoke =
                  llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
              Invoke && Invoke->getCalledFunction() &&
              Invoke->getCalledFunction()->getIntrinsicID() ==
                  llvm::Intrinsic::seh_scope_begin) {
            llvm::IRBuilder<> B(
                &*Invoke->getNormalDest()->getFirstInsertionPt());
            auto *Address = B.CreatePtrToInt(Frame, B.getInt32Ty());
            auto *Low = B.CreateTrunc(Address, B.getInt8Ty());
            auto *Wide = B.CreateZExt(Low, B.getInt32Ty());
            auto *Pointer = B.CreateIntToPtr(Wide, B.getPtrTy());
            B.CreateStore(Address, Pointer);
            return true;
          }
        return false;
      },
      "unproved LLVM escape or access");
  RejectMutation(
      "compiler registration head escape",
      [](llvm::Function &F) {
        for (auto &Block : F)
          for (auto &I : Block) {
            auto *Head = llvm::dyn_cast<llvm::LoadInst>(&I);
            if (!Head || Head->getPointerAddressSpace() != 257)
              continue;
            auto *Pointer = Head->getNextNode();
            auto *Previous = Pointer ? Pointer->getNextNode() : nullptr;
            if (!Previous || !Previous->getNextNode())
              return false;
            llvm::IRBuilder<> B(Previous->getNextNode());
            auto *Global = new llvm::GlobalVariable(
                *F.getParent(), B.getInt32Ty(), false,
                llvm::GlobalValue::ExternalLinkage, nullptr, "leaked.head");
            B.CreateStore(Head, Global);
            return true;
          }
        return false;
      },
      "source previous head");
  RejectMutation(
      "inline assembly registration clobber",
      [](llvm::Function &F) {
        for (auto &Block : F)
          if (auto *Invoke =
                  llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
              Invoke && Invoke->getCalledFunction() &&
              Invoke->getCalledFunction()->getIntrinsicID() ==
                  llvm::Intrinsic::seh_scope_begin) {
            llvm::IRBuilder<> B(
                &*Invoke->getNormalDest()->getFirstInsertionPt());
            auto *Type = llvm::FunctionType::get(B.getVoidTy(), false);
            auto *Assembly = llvm::InlineAsm::get(Type, "movl $$0, %fs:0",
                                                  "~{memory}", true);
            B.CreateCall(Type, Assembly);
            return true;
          }
        return false;
      },
      "unproved LLVM escape or access");
  for (bool ScopeEnd : {false, true})
    RejectMutation(
        ScopeEnd ? "unanchored scope end" : "unanchored plain call",
        [ScopeEnd](llvm::Function &F) {
          for (auto &Block : F)
            if (auto *Invoke =
                    llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
                Invoke && Invoke->getCalledFunction() &&
                Invoke->getCalledFunction()->getIntrinsicID() ==
                    llvm::Intrinsic::seh_scope_begin) {
              llvm::IRBuilder<> B(
                  &*Invoke->getNormalDest()->getFirstInsertionPt());
              if (ScopeEnd)
                B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                    F.getParent(), llvm::Intrinsic::seh_scope_end));
              else
                B.CreateCall(F.getParent()->getOrInsertFunction(
                    "unknown_state_call", B.getVoidTy()));
              return true;
            }
          return false;
        },
        ScopeEnd ? "unanchored SEH scope state change" : "unanchored call");
  RejectMutation(
      "missing callback runtime root identity",
      [](llvm::Function &F) {
        for (auto &Helper : *F.getParent())
          for (auto &Block : Helper)
            for (auto &I : Block)
              if (I.getMetadata(windows_eh_md::RegistrationRootAttachment)) {
                I.setMetadata(windows_eh_md::RegistrationRootAttachment,
                              nullptr);
                return true;
              }
        return false;
      },
      "runtime root set");
  for (bool Redirect : {false, true})
    RejectMutation(
        Redirect ? "redirected callback root" : "late callback root",
        [Redirect](llvm::Function &F) {
          for (auto &Helper : *F.getParent())
            for (auto &Block : Helper)
              for (auto &I : Block) {
                auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
                if (!Store || !Store->getMetadata(
                                  windows_eh_md::RegistrationRootAttachment))
                  continue;
                if (Redirect) {
                  llvm::IRBuilder<> B(
                      &*Helper.getEntryBlock().getFirstInsertionPt());
                  Store->setOperand(1, B.CreateAlloca(B.getInt32Ty()));
                  return true;
                }
                for (auto *User : Store->getPointerOperand()->users())
                  if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(User)) {
                    Store->moveAfter(Load);
                    return true;
                  }
              }
          return false;
        },
        Redirect ? "unused storage" : "dominate");
  RejectMutation(
      "logical exception cell read before runtime bridge",
      [](llvm::Function &F) {
        for (auto &Helper : *F.getParent())
          for (auto &Block : Helper)
            for (auto &I : Block) {
              auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
              auto *Load =
                  Store
                      ? llvm::dyn_cast<llvm::LoadInst>(Store->getValueOperand())
                      : nullptr;
              auto *Cell = Load ? llvm::dyn_cast<llvm::GetElementPtrInst>(
                                      Load->getPointerOperand())
                                : nullptr;
              auto *Runtime = Cell ? llvm::dyn_cast<llvm::IntrinsicInst>(
                                         Cell->getPointerOperand())
                                   : nullptr;
              if (!Runtime ||
                  Runtime->getIntrinsicID() != llvm::Intrinsic::frameaddress)
                continue;
              llvm::IRBuilder<> B(Store);
              B.CreateLoad(B.getPtrTy(), Store->getPointerOperand());
              return true;
            }
        return false;
      },
      "unproved LLVM escape or access");
  RejectMutation(
      "callback spill overwritten on a self-loop backedge",
      [](llvm::Function &F) {
        llvm::AllocaInst *LogicalFrame = nullptr;
        llvm::IntrinsicInst *Escape = nullptr;
        for (auto &Block : F)
          for (auto &I : Block) {
            if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment))
              LogicalFrame = llvm::dyn_cast<llvm::AllocaInst>(&I);
            if (auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
                Call && Call->getIntrinsicID() == llvm::Intrinsic::localescape)
              Escape = Call;
          }
        if (!LogicalFrame || !Escape)
          return false;
        for (auto &Helper : *F.getParent()) {
          if (&Helper == &F || Helper.isDeclaration() || Helper.arg_size())
            continue;
          llvm::IntrinsicInst *Recover = nullptr;
          for (auto &I : Helper.getEntryBlock())
            if (auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
                Call &&
                Call->getIntrinsicID() == llvm::Intrinsic::localrecover &&
                llvm::isa<llvm::ConstantInt>(Call->getArgOperand(2)) &&
                llvm::cast<llvm::ConstantInt>(Call->getArgOperand(2))
                        ->getZExtValue() < Escape->arg_size() &&
                Escape->getArgOperand(
                    llvm::cast<llvm::ConstantInt>(Call->getArgOperand(2))
                        ->getZExtValue()) == LogicalFrame)
              Recover = Call;
          if (!Recover)
            continue;
          auto &Context = F.getContext();
          llvm::IRBuilder<> Setup(Helper.getEntryBlock().getTerminator());
          auto *Spill = Setup.CreateAlloca(Setup.getInt32Ty());
          auto *Frame32 = Setup.CreatePtrToInt(Recover, Setup.getInt32Ty());
          Setup.CreateStore(Frame32, Spill);
          auto *Loop =
              llvm::BasicBlock::Create(Context, "mutated.loop", &Helper);
          for (auto &Block : Helper)
            if (auto *Return =
                    llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator())) {
              auto *Done = Block.splitBasicBlock(Return, "mutated.done");
              llvm::cast<llvm::UncondBrInst>(Block.getTerminator())
                  ->setSuccessor(0, Loop);
              llvm::IRBuilder<> B(Loop);
              auto *Address = B.CreateLoad(B.getInt32Ty(), Spill);
              B.CreateStore(Frame32, B.CreateIntToPtr(Address, B.getPtrTy()));
              B.CreateStore(B.getInt32(0), Spill);
              auto *Again = new llvm::GlobalVariable(
                  *F.getParent(), B.getInt1Ty(), false,
                  llvm::GlobalValue::ExternalLinkage, nullptr, "repeat");
              B.CreateCondBr(B.CreateLoad(B.getInt1Ty(), Again), Loop, Done);
              return true;
            }
        }
        return false;
      },
      "unproved LLVM escape or access");
  for (bool WideGEP : {false, true})
    RejectMutation(
        WideGEP ? "inbounds wide-index overflow" : "nonnegative frame cast",
        [WideGEP](llvm::Function &F) {
          llvm::AllocaInst *Frame = nullptr;
          for (auto &Block : F)
            for (auto &I : Block)
              if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment))
                Frame = llvm::dyn_cast<llvm::AllocaInst>(&I);
          if (!Frame)
            return false;
          for (auto &Block : F)
            if (auto *Invoke =
                    llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
                Invoke && Invoke->getCalledFunction() &&
                Invoke->getCalledFunction()->getIntrinsicID() ==
                    llvm::Intrinsic::seh_scope_begin) {
              llvm::IRBuilder<> B(
                  &*Invoke->getNormalDest()->getFirstInsertionPt());
              llvm::Value *Address;
              if (WideGEP)
                Address = B.CreateInBoundsGEP(B.getInt8Ty(), Frame,
                                              B.getInt64(uint64_t(1) << 32));
              else {
                auto *Integer = B.CreatePtrToInt(Frame, B.getInt32Ty());
                auto *Wide = llvm::cast<llvm::Instruction>(
                    B.CreateZExt(Integer, B.getInt64Ty()));
                Wide->setNonNeg();
                Address = B.CreateIntToPtr(Wide, B.getPtrTy());
              }
              B.CreateLoad(B.getInt32Ty(), Address);
              return true;
            }
          return false;
        },
        "unproved LLVM escape or access");
  RejectMutation(
      "poison nusw intermediate canceled by integer wrap",
      [](llvm::Function &F) {
        llvm::AllocaInst *Frame = nullptr;
        for (auto &Block : F)
          for (auto &I : Block)
            if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment))
              Frame = llvm::dyn_cast<llvm::AllocaInst>(&I);
        if (!Frame)
          return false;
        for (auto &Block : F)
          if (auto *Invoke =
                  llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
              Invoke && Invoke->getCalledFunction() &&
              Invoke->getCalledFunction()->getIntrinsicID() ==
                  llvm::Intrinsic::seh_scope_begin) {
            llvm::IRBuilder<> B(
                &*Invoke->getNormalDest()->getFirstInsertionPt());
            auto *GEP = llvm::cast<llvm::GetElementPtrInst>(
                B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(0x80000000)));
            GEP->setNoWrapFlags(llvm::GEPNoWrapFlags::noUnsignedSignedWrap());
            auto *Integer = B.CreatePtrToInt(GEP, B.getInt32Ty());
            auto *Canceled = B.CreateAdd(Integer, B.getInt32(0x80000000));
            B.CreateLoad(B.getInt32Ty(),
                         B.CreateIntToPtr(Canceled, B.getPtrTy()));
            return true;
          }
        return false;
      },
      "unproved LLVM escape or access");
  RejectMutation(
      "wrong dispatch try level",
      [](llvm::Function &F) {
        for (auto &B : F)
          for (auto &I : B) {
            auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
            auto Bundle =
                Call ? Call->getOperandBundle(windows_eh_md::ProvenanceBundle)
                     : std::nullopt;
            const auto *Role =
                Bundle ? llvm::dyn_cast<llvm::ConstantInt>(
                             Bundle->Inputs[windows_eh_md::ProvenanceRole])
                       : nullptr;
            if (!Role ||
                Role->getZExtValue() !=
                    unsigned(
                        windows_eh_md::NativeProvenanceRole::RegionDispatch))
              continue;
            auto *Address = I.getNextNode();
            auto *Store = Address ? llvm::dyn_cast_or_null<llvm::StoreInst>(
                                        Address->getNextNode())
                                  : nullptr;
            if (!Store)
              return false;
            Store->setOperand(0,
                              llvm::ConstantInt::get(
                                  llvm::Type::getInt32Ty(F.getContext()), 42));
            return true;
          }
        return false;
      },
      "try-level synchronization");
  RejectMutation(
      "wrong exception pointer cell",
      [](llvm::Function &F) {
        for (auto &Helper : *F.getParent())
          for (auto &B : Helper)
            for (auto &I : B) {
              auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&I);
              auto *Frame = GEP ? llvm::dyn_cast<llvm::IntrinsicInst>(
                                      GEP->getPointerOperand())
                                : nullptr;
              if (Frame &&
                  Frame->getIntrinsicID() == llvm::Intrinsic::frameaddress &&
                  GEP->getNumIndices() == 1) {
                GEP->setOperand(
                    1, llvm::ConstantInt::get(
                           llvm::Type::getInt32Ty(F.getContext()), -16));
                return true;
              }
            }
        return false;
      },
      "exception pointers");
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();
  auto Buffer = llvm::MemoryBuffer::getFile(Path);
  ASSERT_TRUE(bool(Buffer));
  const auto Bytes = (*Buffer)->getBuffer();
  std::vector<uint8_t> Binary(Bytes.bytes_begin(), Bytes.bytes_end());
  COFFPatcher Patcher;
  const uint64_t CodeVA = Patcher.plannedExecSegmentVA(Binary, Arch::X86);
  ASSERT_NE(CodeVA, 0u);
  auto Resolve = [&](llvm::StringRef Symbol,
                     uint32_t) -> std::optional<uint64_t> {
    if (auto Cookie = findCOFFRegistrationRuntimeVA(Image, Symbol))
      return *Cookie;
    if (auto Address = parseNdDataSymbol(Symbol))
      return *Address;
    if (auto Address = parseNdCodePtrSymbol(Symbol))
      return *Address;
    for (const auto &Function : *Module) {
      llvm::SmallString<64> ObjectName;
      llvm::Mangler Mangler;
      Mangler.getNameWithPrefix(ObjectName, &Function, false);
      if (ObjectName != Symbol)
        continue;
      auto Address = rewrite_source::getOriginalVA(Function);
      if (!Address) {
        llvm::consumeError(Address.takeError());
        return std::nullopt;
      }
      return *Address;
    }
    return std::nullopt;
  };
  auto Compiled = compileImageForPatch(*Module, Arch::X86, BinaryFormat::COFF,
                                       CodeVA, Resolve, Image.Base);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_TRUE(Compiled.Unresolved.empty());
  auto Rows = validateCOFFRegistrationSemanticRows(*Parent, EH, Compiled);
  ASSERT_FALSE(bool(Rows)) << llvm::toString(std::move(Rows));
  va_t Generated = 0;
  for (const auto &Owner : Compiled.SourceFunctionOwners)
    if (Owner.SourceFunction == Parent->getName())
      Generated = Owner.OwnerVA;
  ASSERT_GE(Generated, CodeVA);
  const std::pair<va_t, va_t> Mapping{EH.CodeRange.Begin, Generated};
  auto GuardUpdate = prepareCOFFGuardTables(Binary, Image, Compiled, {Mapping},
                                            CodeVA, Arch::X86, true);
  ASSERT_TRUE(bool(GuardUpdate)) << llvm::toString(GuardUpdate.takeError());
  ASSERT_TRUE(GuardUpdate->ApplyCF);
  ASSERT_GT(GuardUpdate->CFFunctionCount, 0u);
  auto Update = prepareCOFFRegistrationPatch(Binary, Image, Compiled, {Mapping},
                                             CodeVA, *Module, &*GuardUpdate);
  ASSERT_TRUE(bool(Update)) << llvm::toString(Update.takeError());
  ASSERT_TRUE(Update->Apply);
  {
    auto AlteredModule = llvm::CloneModule(*Module);
    auto *Function = AlteredModule->getFunction(Parent->getName());
    llvm::IRBuilder<> B(&*Function->getEntryBlock().getFirstInsertionPt());
    B.CreateLoad(B.getInt32Ty(),
                 llvm::ConstantExpr::getIntToPtr(
                     B.getInt32(Image.Base + 0x1000), B.getPtrTy()));
    auto AlteredCompiled = Compiled;
    auto Failure = prepareCOFFRegistrationPatch(
        Binary, Image, AlteredCompiled, {Mapping}, CodeVA, *AlteredModule);
    ASSERT_FALSE(bool(Failure));
    EXPECT_NE(llvm::toString(Failure.takeError()).find("raw image pointer"),
              std::string::npos);
  }
  {
    auto AlteredModule = llvm::CloneModule(*Module);
    auto *Function = AlteredModule->getFunction(Parent->getName());
    llvm::IRBuilder<> Entry(&*Function->getEntryBlock().getFirstInsertionPt());
    auto *Spill = Entry.CreateAlloca(Entry.getInt32Ty());
    Entry.CreateStore(Entry.getInt32(Image.Base + 0x2000), Spill);
    bool Added = false;
    for (auto &Block : *Function)
      if (auto *Invoke =
              llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
          Invoke && Invoke->getCalledFunction() &&
          Invoke->getCalledFunction()->getIntrinsicID() ==
              llvm::Intrinsic::seh_scope_begin) {
        llvm::IRBuilder<> B(&*Invoke->getNormalDest()->getFirstInsertionPt());
        auto *Address = B.CreateLoad(B.getInt32Ty(), Spill);
        Address->setVolatile(true);
        auto *Value = B.CreateLoad(B.getInt32Ty(),
                                   B.CreateIntToPtr(Address, B.getPtrTy()));
        Value->setVolatile(true);
        Added = true;
        break;
      }
    ASSERT_TRUE(Added);
    ASSERT_FALSE(llvm::verifyModule(*AlteredModule, &llvm::errs()));
    auto AlteredCompiled = Compiled;
    auto Failure = prepareCOFFRegistrationPatch(
        Binary, Image, AlteredCompiled, {Mapping}, CodeVA, *AlteredModule);
    ASSERT_FALSE(bool(Failure));
    EXPECT_NE(llvm::toString(Failure.takeError()).find("raw image pointer"),
              std::string::npos);
  }
  for (unsigned Origin : {0u, 1u, 2u, 3u, 4u}) {
    SCOPED_TRACE(
        Origin == 1   ? "raw image VA through selected globals"
        : Origin == 2 ? "raw image VA assembled with two partial stores"
        : Origin == 3 ? "raw image VA returned by a local helper"
        : Origin == 4 ? "raw image VA returned by selected local helpers"
                      : "raw image VA synthesized through two scalar spills");
    auto AlteredModule = llvm::CloneModule(*Module);
    auto *Function = AlteredModule->getFunction(Parent->getName());
    llvm::IRBuilder<> Entry(&*Function->getEntryBlock().getFirstInsertionPt());
    auto *Left = Entry.CreateAlloca(Entry.getInt32Ty());
    auto *Right = Entry.CreateAlloca(Entry.getInt32Ty());
    if (Origin == 2) {
      Entry.CreateStore(Entry.getInt16((Image.Base + 0x1000) & 0xffff), Left);
      Entry.CreateStore(
          Entry.getInt16((Image.Base + 0x1000) >> 16),
          Entry.CreateGEP(Entry.getInt8Ty(), Left, Entry.getInt32(2)));
    } else
      Entry.CreateStore(Entry.getInt32(Image.Base / 2), Left);
    Entry.CreateStore(Entry.getInt32(Image.Base - Image.Base / 2 + 0x1000),
                      Right);
    bool Added = false;
    for (auto &Block : *Function)
      if (auto *Invoke =
              llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
          Invoke && Invoke->getCalledFunction() &&
          Invoke->getCalledFunction()->getIntrinsicID() ==
              llvm::Intrinsic::seh_scope_begin) {
        llvm::IRBuilder<> B(&*Invoke->getNormalDest()->getFirstInsertionPt());
        auto *A = B.CreateLoad(B.getInt32Ty(), Left);
        auto *C = B.CreateLoad(B.getInt32Ty(), Right);
        llvm::Value *Address = Origin == 2 ? A : B.CreateAdd(A, C);
        if (Origin == 1) {
          auto *Raw = new llvm::GlobalVariable(
              *AlteredModule, B.getInt32Ty(), true,
              llvm::GlobalValue::PrivateLinkage,
              B.getInt32(Image.Base + 0x1000), "raw.image.va");
          auto *Zero = new llvm::GlobalVariable(
              *AlteredModule, B.getInt32Ty(), true,
              llvm::GlobalValue::PrivateLinkage, B.getInt32(0), "zero.va");
          auto *Selected = B.CreateSelect(B.CreateICmpEQ(A, C), Raw, Zero);
          Address = B.CreateLoad(B.getInt32Ty(), Selected);
        }
        if (Origin >= 3) {
          auto *Type = llvm::FunctionType::get(B.getInt32Ty(), false);
          auto Helper = [&](llvm::StringRef Name, uint32_t Value) {
            auto *F = llvm::Function::Create(
                Type, llvm::GlobalValue::InternalLinkage, Name, *AlteredModule);
            llvm::IRBuilder<> Body(
                llvm::BasicBlock::Create(Context, "entry", F));
            Body.CreateRet(Body.getInt32(Value));
            return F;
          };
          llvm::Value *Target = Helper("raw.image.helper", Image.Base + 0x1000);
          if (Origin == 4)
            Target = B.CreateSelect(B.CreateICmpEQ(A, C), Target,
                                    Helper("zero.helper", 0));
          Address = B.CreateCall(Type, Target);
        }
        auto *Value = B.CreateLoad(B.getInt32Ty(),
                                   B.CreateIntToPtr(Address, B.getPtrTy()));
        Value->setVolatile(true);
        Added = true;
        break;
      }
    ASSERT_TRUE(Added);
    ASSERT_FALSE(llvm::verifyModule(*AlteredModule, &llvm::errs()));
    auto AlteredCompiled = Compiled;
    auto Failure = prepareCOFFRegistrationPatch(
        Binary, Image, AlteredCompiled, {Mapping}, CodeVA, *AlteredModule);
    ASSERT_FALSE(bool(Failure));
    EXPECT_NE(llvm::toString(Failure.takeError()).find("raw image pointer"),
              std::string::npos);
  }
  {
    auto AlteredModule = llvm::CloneModule(*Module);
    auto *Function = AlteredModule->getFunction(Parent->getName());
    auto *EntryBranch = llvm::cast<llvm::UncondBrInst>(
        Function->getEntryBlock().getTerminator());
    auto *OriginalEntry = EntryBranch->getSuccessor(0);
    auto *Loop = llvm::BasicBlock::Create(Context, "numeric.origin.loop",
                                          Function, OriginalEntry);
    EntryBranch->setSuccessor(0, Loop);
    llvm::IRBuilder<> B(Loop);
    auto *Phi = B.CreatePHI(B.getInt32Ty(), 2);
    auto *Next =
        B.CreateAdd(Phi, B.getInt32(Image.Base - Image.Base / 2 + 0x1000));
    Phi->addIncoming(B.getInt32(Image.Base / 2), &Function->getEntryBlock());
    Phi->addIncoming(Next, Loop);
    B.CreateLoad(B.getInt32Ty(), B.CreateIntToPtr(Phi, B.getPtrTy()));
    auto *Again = new llvm::GlobalVariable(*AlteredModule, B.getInt1Ty(), false,
                                           llvm::GlobalValue::ExternalLinkage,
                                           nullptr, "numeric.repeat");
    B.CreateCondBr(B.CreateLoad(B.getInt1Ty(), Again), Loop, OriginalEntry);
    ASSERT_FALSE(llvm::verifyModule(*AlteredModule, &llvm::errs()));
    auto AlteredCompiled = Compiled;
    auto Failure = prepareCOFFRegistrationPatch(
        Binary, Image, AlteredCompiled, {Mapping}, CodeVA, *AlteredModule);
    ASSERT_FALSE(bool(Failure));
    EXPECT_NE(llvm::toString(Failure.takeError()).find("raw image pointer"),
              std::string::npos);
  }
  ASSERT_EQ(Patcher.appendExecSegment(Binary, Compiled.Bytes, kNdTextSection,
                                      Arch::X86),
            CodeVA);
  auto PE = locatePEHeaders(Binary.data(), Binary.size());
  std::optional<size_t> EntryOffset;
  const uint32_t EntryRVA = EH.CodeRange.Begin - Image.Base;
  forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
    if (EntryRVA >= Section.VirtualAddress &&
        rangeInBounds(EntryRVA - Section.VirtualAddress, 5,
                      Section.SizeOfRawData))
      EntryOffset =
          Section.PointerToRawData + EntryRVA - Section.VirtualAddress;
  });
  ASSERT_TRUE(EntryOffset);
  Binary[*EntryOffset] = 0xe9;
  llvm::support::endian::write32le(
      Binary.data() + *EntryOffset + 1,
      uint32_t(Generated - EH.CodeRange.Begin - 5));
  auto GuardInstalled = applyCOFFGuardTableUpdate(Binary, Image, *GuardUpdate);
  ASSERT_FALSE(bool(GuardInstalled))
      << llvm::toString(std::move(GuardInstalled));
  auto Installed = applyCOFFRegistrationPatch(Binary, *Update);
  ASSERT_FALSE(bool(Installed)) << llvm::toString(std::move(Installed));
  auto Checked = validateCOFFRegistrationPatch(Binary, *Update);
  ASSERT_FALSE(bool(Checked)) << llvm::toString(std::move(Checked));
  {
    auto Altered = Binary;
    auto NewPE = locatePEHeaders(Altered.data(), Altered.size());
    forEachPESection(NewPE, [&](const PESectionFields &Section, uint16_t) {
      if (Image.Base + Section.VirtualAddress == CodeVA)
        Altered[Section.PointerToRawData] ^= 1;
    });
    auto Failure = validateCOFFRegistrationPatch(Altered, *Update);
    ASSERT_TRUE(bool(Failure));
    EXPECT_NE(llvm::toString(std::move(Failure)).find("differs"),
              std::string::npos);
  }
  {
    Patcher.setImageContext(&Image);
    const auto ProductPath =
        std::filesystem::path(Path).parent_path() / "product-patched.exe";
    const auto Result = Patcher.patch(Path, ProductPath, *Module, Arch::X86);
    ASSERT_TRUE(Result.Success);
    EXPECT_EQ(Result.TrampolineCount, 1u);
    // Exact source-callee identity must win even when its spelling aliases an
    // import. Otherwise a direct call can jump into an IAT slot as code.
    auto CollisionModule = llvm::CloneModule(*Module);
    llvm::Function *OriginalCallee = nullptr;
    auto *CollisionParent = CollisionModule->getFunction(Parent->getName());
    ASSERT_TRUE(CollisionParent);
    for (auto &Block : *CollisionParent)
      for (auto &I : Block) {
        auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
        auto *Callee = Call ? Call->getCalledFunction() : nullptr;
        if (!Callee || !Callee->isDeclaration() || Callee->isIntrinsic())
          continue;
        auto Address = rewrite_source::getOriginalVA(*Callee);
        ASSERT_TRUE(bool(Address)) << llvm::toString(Address.takeError());
        if (*Address) {
          ASSERT_TRUE(!OriginalCallee || OriginalCallee == Callee);
          OriginalCallee = Callee;
        }
      }
    ASSERT_TRUE(OriginalCallee);
    ASSERT_EQ(CollisionModule->getFunction("RaiseException"), nullptr);
    OriginalCallee->setName("RaiseException");
    const auto CollisionPath =
        std::filesystem::path(Path).parent_path() / "collision-patched.exe";
    const auto CollisionResult =
        Patcher.patch(Path, CollisionPath, *CollisionModule, Arch::X86);
    ASSERT_TRUE(CollisionResult.Success);
    EXPECT_EQ(CollisionResult.TrampolineCount, 1u);
  }
  if (const char *Output = std::getenv("NEVERD_REGISTRATION_OUTPUT_PE32")) {
    std::error_code EC;
    llvm::raw_fd_ostream Stream(Output, EC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    Stream.write(reinterpret_cast<const char *>(Binary.data()), Binary.size());
  }
#endif
}

struct RegistrationNativeFixture {
  llvm::LLVMContext Context;
  llvm::Module Module{"registration", Context};
  MedFunc Source;
  MedLLVMEmitter Emitter;
  llvm::Function *Parent = nullptr;
  llvm::BasicBlock *Entry = nullptr;
  llvm::BasicBlock *Try = nullptr;
  llvm::BasicBlock *Filter = nullptr;
  llvm::BasicBlock *Handler = nullptr;
  std::map<int, llvm::BasicBlock *> Blocks;

  RegistrationNativeFixture() {
    Module.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
    Module.setDataLayout("e-m:x-p:32:32-i64:64-f80:32-n8:16:32-a:0:32-S32");
    Parent = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getInt32Ty(Context), false),
        llvm::GlobalValue::ExternalLinkage, "guarded", Module);
    rewrite_source::setOriginalVA(*Parent, 0x1000);
    Entry = llvm::BasicBlock::Create(Context, "entry", Parent);
    Try = llvm::BasicBlock::Create(Context, "body", Parent);
    Filter = llvm::BasicBlock::Create(Context, "filter", Parent);
    Handler = llvm::BasicBlock::Create(Context, "handler", Parent);
    Blocks = {{0, Entry}, {1, Try}, {2, Filter}, {3, Handler}};
    Source.Entry = 0x1000;
    Source.RegistrationCallerCleanupABIComplete = true;
    Source.ExceptionMetadata.emplace();
    auto &EH = *Source.ExceptionMetadata;
    EH.Kind = RuntimeFunctionKind::Primary;
    EH.ParseStatus = ExceptionParseStatus::Complete;
    EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
    EH.Personality = ExceptionPersonality::ExceptHandler3;
    EH.PersonalityVA = 0x2000;
    EH.HandlerDataVA = 0x3000;
    EH.CodeRange = {0x1000, 0x1040};
    auto &Chain = EH.Registration.emplace();
    Chain.HandlerVA = EH.PersonalityVA;
    Chain.ScopeTableVA = EH.HandlerDataVA;
    Chain.RegistrationOffset = -16;
    Chain.TryLevelOffset = -4;
    Chain.SeededTryLevel = -1;
    Chain.ChainInstallVA = 0x1002;
    Chain.ChainRemoveVA = 0x1032;
    Chain.TryLevelStores = {{0x1004, 0x1010, 0}, {0x1014, 0x1015, -1}};
    Chain.Scopes = {{-1, 0x1020, 0x1030, false}};
    auto &States = Source.RegistrationStates.emplace();
    States.Complete = States.CallbackStatesComplete = true;
    States.RegistrationLifetimeComplete = States.ChainOperationsComplete = true;
    const int32_t Levels[] = {-1, 0, -1, -1};
    for (int I = 0; I != 4; ++I) {
      MedBlock B;
      B.Id = I;
      B.StartAddr = 0x1000 + I * 0x10;
      B.EndAddr = B.StartAddr + 0x10;
      Source.Blocks.push_back(B);
      States.Blocks.push_back(
          {I, {B.StartAddr, B.EndAddr}, {Levels[I]}, false, I == 2, I != 2});
    }
    States.ChainAccesses = {
        {0x1001, 0x1002, 1, RegistrationChainAccess::Kind::ReadPreviousHead},
        {0x1002, 0x1003, 2, RegistrationChainAccess::Kind::Install},
        {0x1032, 0x1033, 3, RegistrationChainAccess::Kind::Remove}};
    llvm::IRBuilder<> B(Entry);
    auto *Frame = B.CreateAlloca(B.getInt8Ty(), B.getInt32(128), "frame");
    auto *FS = llvm::ConstantPointerNull::get(B.getPtrTy(257));
    auto *Prev = B.CreateLoad(B.getInt32Ty(), FS);
    auto *Install = B.CreateStore(B.getInt32(0), FS);
    B.CreateBr(Try);
    MedLLVMEmitterTestPeer::prepare(Emitter, Module, *Parent, Source, *Frame);
    MedLLVMEmitterTestPeer::chain(Emitter, *Prev, 0x1001, 1);
    MedLLVMEmitterTestPeer::chain(Emitter, *Install, 0x1002, 2);
    B.SetInsertPoint(Try);
    auto Callee = Module.getOrInsertFunction("raise", B.getVoidTy());
    auto *Call = B.CreateCall(Callee);
    MedLLVMEmitterTestPeer::call(Emitter, *Call, 0x1011);
    auto *Memory = B.CreateIntToPtr(B.getInt32(0x4000), B.getPtrTy());
    B.CreateLoad(B.getInt32Ty(), Memory, "source.faulting.load");
    B.CreateBr(Handler);
    B.SetInsertPoint(Filter);
    B.CreateRet(B.getInt32(1));
    B.SetInsertPoint(Handler);
    auto *Remove = B.CreateStore(B.getInt32(0), FS);
    MedLLVMEmitterTestPeer::chain(Emitter, *Remove, 0x1032, 3);
    B.CreateRet(B.getInt32(7));
  }
  std::string print() const {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    Module.print(OS, nullptr);
    return Text;
  }
  bool lower() {
    return MedLLVMEmitterTestPeer::lower(Emitter, Source, *Parent, Blocks);
  }
};

TEST(WindowsRegistrationNative, ResolvesOnlyTheAuthenticatedEH4RuntimeSymbols) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x400000;
  Image.DynInfo.SecurityCookieRVA = 0x3000;
  Segment Text;
  Text.VA = 0x401000;
  Text.Size = 192;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  const uint8_t Wrapper[] = {
      0x55, 0x89, 0xe5, 0xff, 0x75, 0x14, 0xff, 0x75, 0x10, 0xff, 0x75, 0x0c,
      0xff, 0x75, 0x08, 0x68, 0,    0,    0,    0,    0x68, 0,    0,    0,
      0,    0xff, 0x15, 0,    0,    0,    0,    0x83, 0xc4, 0x18, 0x5d, 0xc3};
  std::copy(std::begin(Wrapper), std::end(Wrapper), Text.Data.begin());
  llvm::support::endian::write32le(Text.Data.data() + 16, 0x401080);
  llvm::support::endian::write32le(Text.Data.data() + 21, 0x403000);
  llvm::support::endian::write32le(Text.Data.data() + 27, 0x403004);
  const uint8_t Check[] = {0x3b, 0x0d, 0x00, 0x30, 0x40,
                           0x00, 0x75, 0x10, 0xc3};
  std::copy(std::begin(Check), std::end(Check), Text.Data.begin() + 128);
  std::copy(std::begin(Check), std::end(Check), Text.Data.begin() + 160);
  Image.Segments.push_back(Text);
  Segment Data;
  Data.VA = 0x403000;
  Data.Size = 16;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(Data);
  Image.Imports.push_back(
      {"msvcrt.dll", "_except_handler4_common", 0, 0x403004});
  ExceptionFunction EH;
  EH.Personality = ExceptionPersonality::ExceptHandler4;
  EH.PersonalityVA = Text.VA;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Registration.emplace();
  Image.ExceptionMetadata.Functions.push_back(EH);
  EXPECT_EQ(findCOFFRegistrationRuntimeVA(Image, "___security_cookie"),
            0x403000);
  EXPECT_EQ(findCOFFRegistrationRuntimeVA(Image, "@__security_check_cookie@4"),
            0x401080);
  EXPECT_FALSE(findCOFFRegistrationRuntimeVA(Image, "__security_check_cookie"));
  auto Conflicting = Image;
  auto &Bytes = Conflicting.Segments.front().Data;
  std::copy(Bytes.begin(), Bytes.begin() + sizeof(Wrapper), Bytes.begin() + 64);
  llvm::support::endian::write32le(Bytes.data() + 64 + 16, 0x4010a0);
  EH.PersonalityVA += 64;
  Conflicting.ExceptionMetadata.Functions.push_back(EH);
  EXPECT_FALSE(
      findCOFFRegistrationRuntimeVA(Conflicting, "@__security_check_cookie@4"));
  EXPECT_EQ(findCOFFRegistrationRuntimeVA(Conflicting, "___security_cookie"),
            0x403000);
  Image.Imports.front().Module = "custom.dll";
  EXPECT_FALSE(findCOFFRegistrationRuntimeVA(Image, "___security_cookie"));
  EXPECT_FALSE(
      findCOFFRegistrationRuntimeVA(Image, "@__security_check_cookie@4"));
}

TEST(WindowsRegistrationNative, UsesCompilerOwnedScopesAndRecoveredCallbacks) {
  RegistrationNativeFixture F;
  ASSERT_TRUE(F.lower()) << F.print();
  EXPECT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
  EXPECT_TRUE(F.Parent->hasPersonalityFn());
  EXPECT_TRUE(F.Parent->hasFnAttribute(llvm::Attribute::OptimizeNone));
  EXPECT_TRUE(
      F.Parent->hasFnAttribute("llvm.rewrite.win-x86-registration-state"));
  EXPECT_NE(F.Module.getFunction("guarded.registration.callback.0"), nullptr);
  const auto Text = F.print();
  EXPECT_NE(Text.find("invoke void @llvm.seh.scope.begin"), std::string::npos);
  EXPECT_NE(Text.find("invoke void @llvm.seh.scope.end"), std::string::npos);
  EXPECT_NE(Text.find("source.faulting.load = load volatile"),
            std::string::npos);
  EXPECT_EQ(Text.find("store i32 0, ptr addrspace(257) null"),
            std::string::npos);
}

TEST(WindowsRegistrationNative, NormalFinallyRequiresAnUnobservedSourceResult) {
  for (bool Observe : {false, true}) {
    RegistrationNativeFixture F;
    auto &Scope = F.Source.ExceptionMetadata->Registration->Scopes.front();
    Scope.IsFinally = true;
    Scope.FilterVA = 0;
    Scope.HandlerVA = 0x1020;
    F.Source.RegistrationStates->Blocks[2].CanDispatch = true;
    MedOp SourceCall;
    SourceCall.Opcode = NdOp::CALL;
    SourceCall.Addr = 0x1011;
    SourceCall.OriginSeq = 1;
    SourceCall.NumInputs = 1;
    SourceCall.Inputs[0] = MedVar::makeConst(0x1020, 4);
    F.Source.Blocks[1].Ops.push_back(SourceCall);
    auto *Old = llvm::cast<llvm::CallInst>(&F.Try->front());
    llvm::IRBuilder<> B(Old);
    auto *Call = B.CreateCall(
        F.Module.getOrInsertFunction("direct.finally", B.getInt64Ty()));
    MedLLVMEmitterTestPeer::forgetCall(F.Emitter, *Old);
    Old->eraseFromParent();
    MedLLVMEmitterTestPeer::call(F.Emitter, *Call, 0x1011);
    llvm::IRBuilder<> Entry(F.Entry->getTerminator());
    auto *Slot = Entry.CreateAlloca(B.getInt32Ty());
    B.SetInsertPoint(Call->getNextNode());
    B.CreateStore(B.CreateTrunc(Call, B.getInt32Ty()), Slot);
    if (Observe)
      B.CreateLoad(B.getInt32Ty(), Slot)->setVolatile(true);
    const auto Before = F.print();
    EXPECT_EQ(F.lower(), !Observe);
    if (Observe) {
      EXPECT_EQ(F.print(), Before);
      continue;
    }
    EXPECT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
    unsigned NormalCalls = 0;
    for (const auto &Block : *F.Parent)
      for (const auto &I : Block)
        if (I.getMetadata(windows_eh_md::RegistrationFinallyCallAttachment)) {
          ++NormalCalls;
          const auto *Call = llvm::cast<llvm::CallBase>(&I);
          ASSERT_EQ(Call->arg_size(), 2u);
          EXPECT_EQ(llvm::cast<llvm::ConstantInt>(Call->getArgOperand(0))
                        ->getZExtValue(),
                    0u);
        }
    EXPECT_EQ(NormalCalls, 1u);
  }
}

TEST(WindowsRegistrationNative,
     RejectsAnUnauthenticatedFSOccurrenceAtomically) {
  RegistrationNativeFixture F;
  llvm::IRBuilder<> B(F.Try->getTerminator());
  B.CreateLoad(B.getInt32Ty(), llvm::ConstantPointerNull::get(B.getPtrTy(257)));
  const auto Before = F.print();
  EXPECT_FALSE(F.lower());
  EXPECT_EQ(F.print(), Before);
}

TEST(WindowsRegistrationNative, RejectsUnprovedStackCleanupAtomically) {
  for (bool CalleePop : {false, true}) {
    RegistrationNativeFixture F;
    if (CalleePop)
      F.Source.CalleePopBytes = 4;
    else
      F.Source.RegistrationCallerCleanupABIComplete = false;
    const auto Before = F.print();
    EXPECT_FALSE(F.lower());
    EXPECT_EQ(F.print(), Before);
  }
}

TEST(WindowsRegistrationNative, ReplaysPreservedCalleeReturnCleanup) {
  for (bool CalleePop : {false, true}) {
    BinaryImage Image;
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Segment Text;
    Text.Name = ".text";
    Text.VA = 0x401000;
    Text.Size = 16;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.assign(16, 0xcc);
    Text.Data[0] = CalleePop ? 0xc2 : 0xc3;
    Text.Data[1] = 4;
    Text.Data[2] = 0;
    Image.Segments.push_back(Text);
    LowFunc Source;
    Source.Entry = 0x402000;
    LowBlock Block;
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.NumInputs = 1;
    Call.Inputs[0] = NdVar::cst(Text.VA, 4);
    Block.Ops.push_back(Call);
    Source.Blocks.push_back(Block);
    EXPECT_EQ(hasCallerCleanupRegistrationABI(Source, Image), !CalleePop);
    Source.CalleePopBytes = 4;
    EXPECT_FALSE(hasCallerCleanupRegistrationABI(Source, Image));
    Source.CalleePopBytes = 0;
    Source.Blocks.front().Ops.front().Opcode = NdOp::INDIR_CALL;
    EXPECT_FALSE(hasCallerCleanupRegistrationABI(Source, Image));
  }
}

TEST(WindowsRegistrationNative, ProvesSeparateLeafStackAndECXObjectDomains) {
  struct Case {
    std::vector<uint8_t> Code;
    bool Accepted;
    std::vector<RegistrationObjectExtent> Reads;
    std::vector<RegistrationObjectExtent> Writes;
  };
  const std::vector<Case> Cases = {
      // Read a scalar through the borrowed object.
      {{0x8b, 0x01, 0xc3}, true, {{0, 4}}, {}},
      // Real MSVC /Od thiscall destructor: spill this in its private frame,
      // update an image trace from this->Tag, restore its frame and return.
      {{0x55, 0x8b, 0xec, 0x51, 0x89, 0x4d, 0xfc, 0x6b, 0x05, 0x00,
        0x30, 0x40, 0x00, 0x0a, 0x8b, 0x4d, 0xfc, 0x03, 0x01, 0xa3,
        0x00, 0x30, 0x40, 0x00, 0x8b, 0xe5, 0x5d, 0xc3},
       true,
       {{0, 4}},
       {}},
      // Private stack offset zero and object offset zero are different cells.
      // Writing the object must not replace the spilled this pointer.
      {{0x51, 0xc7, 0x01, 0x07, 0x00, 0x00, 0x00, 0x8b, 0x0c, 0x24, 0x8b, 0x41,
        0x04, 0x83, 0xc4, 0x04, 0xc3},
       true,
       {{4, 8}},
       {{0, 4}}},
      // Partial pointer spills retain unknown object provenance.
      {{0x51, 0xc6, 0x04, 0x24, 0x00, 0x59, 0x8b, 0x01, 0xc3}, false, {}, {}},
      // Byte reads retain their exact extent rather than claiming a word.
      {{0x0f, 0xb6, 0x01, 0xc3}, true, {{0, 1}}, {}},
      // Object pointers may not escape to the image, an object or a result.
      {{0x89, 0x0d, 0x00, 0x30, 0x40, 0x00, 0xc3}, false, {}, {}},
      {{0x89, 0x09, 0xc3}, false, {}, {}},
      {{0x8b, 0xc1, 0xc3}, false, {}, {}},
      // Unknown and unbounded object addresses need a different proof.
      {{0x03, 0xca, 0x8b, 0x01, 0xc3}, false, {}, {}},
      {{0x8b, 0x41, 0xfc, 0xc3}, false, {}, {}},
      {{0x8b, 0x81, 0x00, 0x00, 0x10, 0x00, 0xc3}, false, {}, {}},
      {{0x8b, 0x01, 0x8b, 0x00, 0xc3}, false, {}, {}},
      // The private domain still rejects uninitialized and caller-frame reads.
      {{0x8b, 0x44, 0x24, 0xf4, 0xc3}, false, {}, {}},
      {{0x55, 0x8b, 0xec, 0x8b, 0x45, 0x00, 0x8b, 0x00, 0x5d, 0xc3},
       false,
       {},
       {}},
      {{0x8b, 0x44, 0x24, 0x08, 0xc3}, false, {}, {}},
      // Address-dependent control and nonvolatile corruption are not scalar
      // object access, even when every return uses caller cleanup.
      {{0xf7, 0xc1, 0x0f, 0x00, 0x00, 0x00, 0x74, 0x03, 0x31, 0xc0, 0xc3, 0x31,
        0xc0, 0xc3},
       false,
       {},
       {}},
      {{0x31, 0xdb, 0xc3}, false, {}, {}},
      {{0x83, 0xec, 0x04, 0xc3}, false, {}, {}},
      // Neither a callee-pop return, nested call, atomic object effect nor
      // live FS observer belongs to this leaf certificate.
      {{0xc2, 0x04, 0x00}, false, {}, {}},
      {{0xff, 0xd0, 0xc3}, false, {}, {}},
      {{0xf0, 0x87, 0x01, 0xc3}, false, {}, {}},
      {{0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0xc3}, false, {}, {}},
      // Image accesses must fit a real mapped allocation.
      {{0xa1, 0x07, 0x30, 0x40, 0x00, 0xc3}, false, {}, {}},
      // Vector aliases cannot launder the borrowed object address.
      {{0x66, 0x0f, 0x6e, 0xc1, 0x66, 0x0f, 0x7e, 0xc0, 0xc3}, false, {}, {}}};
  for (size_t Index = 0; Index != Cases.size(); ++Index) {
    SCOPED_TRACE(Index);
    BinaryImage Image;
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Segment Text;
    Text.Name = ".text";
    Text.VA = 0x401000;
    Text.Data = Cases[Index].Code;
    Text.Size = Text.Data.size();
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Image.Segments.push_back(Text);
    Segment Data;
    Data.Name = ".data";
    Data.VA = 0x403000;
    Data.Data.resize(8);
    Data.Size = Data.Data.size();
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Segments.push_back(Data);
    const auto Proof = getCheckedX86RegistrationLeafCalleeABI(Image, Text.VA);
    ASSERT_EQ(Proof.has_value(), Cases[Index].Accepted);
    if (!Proof)
      continue;
    EXPECT_EQ(Proof->Target, Text.VA);
    EXPECT_EQ(Proof->StackPopBytes, 0u);
    EXPECT_EQ(Proof->ECXReads, Cases[Index].Reads);
    EXPECT_EQ(Proof->ECXWrites, Cases[Index].Writes);
    if (Index == 1) {
      ASSERT_EQ(Proof->ImageReads.size(), 1u);
      ASSERT_EQ(Proof->ImageWrites.size(), 1u);
      EXPECT_EQ(Proof->ImageReads.front().Begin, Data.VA);
      EXPECT_EQ(Proof->ImageReads.front().End, Data.VA + 4);
      EXPECT_EQ(Proof->ImageWrites.front().Begin, Data.VA);
      EXPECT_EQ(Proof->ImageWrites.front().End, Data.VA + 4);
    }
  }
}

TEST(WindowsRegistrationNative, RejectsPreservedCallerFrameAndFSObservers) {
  const std::vector<std::vector<uint8_t>> Programs = {
      // Follow the saved caller EBP, then read its try-level slot.
      {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x00, 0x8b, 0x40, 0xfc, 0x5d, 0xc3},
      // Observe the live thread registration chain without an explicit
      // argument.
      {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0xc3},
      // An ordinary cdecl frame may read a private initialized local.
      {0x55, 0x89, 0xe5, 0x83, 0xec, 0x04, 0xc7, 0x45, 0xfc, 0x07,
       0x00, 0x00, 0x00, 0x8b, 0x45, 0xfc, 0x89, 0xec, 0x5d, 0xc3},
      // The same forbidden observation behind a second internal call.
      {0x55, 0x89, 0xe5, 0xe8, 0x08, 0x00, 0x00, 0x00, 0x89, 0xec, 0x5d, 0xc3,
       0xcc, 0xcc, 0xcc, 0xcc, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0xc3},
      // A closed, frame-private internal callee remains supported.
      {0x55, 0x89, 0xe5, 0xe8, 0x08, 0x00, 0x00, 0x00, 0x89, 0xec, 0x5d, 0xc3,
       0xcc, 0xcc, 0xcc, 0xcc, 0xc3},
      // Atomic RMWs must not bypass caller-frame privacy.
      {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x00, 0xf0, 0x87, 0x48, 0xfc, 0x5d, 0xc3},
      // A saved caller frame cannot escape in a branch condition.
      {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x00, 0x85, 0xc0, 0x74, 0x02, 0x5d, 0xc3,
       0x5d, 0xc3},
      // Unbounded positive offsets can reach the parent's registration.
      {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x24, 0x5d, 0xc3},
      // A saved caller frame cannot be passed in an outgoing stack slot.
      {0x55, 0x89, 0xe5, 0xff, 0x75, 0x00, 0xe8, 0x05, 0x00, 0x00, 0x00, 0x89,
       0xec, 0x5d, 0xc3, 0xcc, 0xc3},
      // Stack bytes below ESP are not initialized by this invocation.
      {0x8b, 0x44, 0x24, 0xf4, 0xa3, 0x00, 0x30, 0x40, 0x00, 0x31, 0xc0, 0xc3},
      // Moving a saved caller frame through XMM0 retains its provenance.
      {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x00, 0x66, 0x0f, 0x6e,
       0xc0, 0x66, 0x0f, 0x7e, 0xc0, 0x8b, 0x40, 0xfc, 0xa3,
       0x00, 0x30, 0x40, 0x00, 0x31, 0xc0, 0x5d, 0xc3},
      // Flags cannot hide a branch on the physical stack address.
      {0x55, 0x89, 0xe5, 0x89, 0xe0, 0xa9, 0x0f, 0x00, 0x00, 0x00,
       0x74, 0x04, 0x31, 0xc0, 0x5d, 0xc3, 0x31, 0xc0, 0x5d, 0xc3},
      // A preserved helper must restore the callee-saved registers and ESP.
      {0x31, 0xed, 0x31, 0xc0, 0xc3},
      {0x83, 0xec, 0x04, 0x31, 0xc0, 0xc3},
      // The return PC may identify the generated caller, but cannot select a
      // branch whose outcome changes with the generated instruction address.
      {0x8b, 0x04, 0x24, 0xa9, 0x01, 0x00, 0x00, 0x00, 0x74, 0x03, 0x31, 0xc0,
       0xc3, 0x31, 0xc0, 0xc3},
      // An external observer may record the real generated call site.
      {0x8b, 0x04, 0x24, 0xa3, 0x00, 0x30, 0x40, 0x00, 0x31, 0xc0, 0xc3},
      // Reloading that global inside the checked closure cannot launder PC.
      {0x8b, 0x04, 0x24, 0xa3, 0x00, 0x30, 0x40, 0x00, 0xa1,
       0x00, 0x30, 0x40, 0x00, 0xa9, 0x01, 0x00, 0x00, 0x00,
       0x74, 0x03, 0x31, 0xc0, 0xc3, 0x31, 0xc0, 0xc3},
      // The same reload in a second callee belongs to the same closure.
      {0x8b, 0x04, 0x24, 0xa3, 0x00, 0x30, 0x40, 0x00, 0xe8, 0x07,
       0x00, 0x00, 0x00, 0x31, 0xc0, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc,
       0xa1, 0x00, 0x30, 0x40, 0x00, 0x31, 0xc0, 0xc3},
      // An unallocated negative displacement is not private stack storage.
      {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x00, 0x89, 0x85, 0x00, 0x00, 0xff, 0xff,
       0x31, 0xc0, 0x5d, 0xc3},
      // CALL invalidates a previously cleared volatile XMM register.
      {0x55, 0x89, 0xe5, 0x31, 0xc0, 0x66, 0x0f, 0x6e, 0xc0, 0xe8,
       0x12, 0x00, 0x00, 0x00, 0x66, 0x0f, 0x7e, 0xc0, 0xa3, 0x00,
       0x30, 0x40, 0x00, 0x31, 0xc0, 0x5d, 0xc3, 0xcc, 0xcc, 0xcc,
       0xcc, 0xcc, 0x66, 0x0f, 0x6e, 0xc5, 0x31, 0xc0, 0xc3},
      // CALL also invalidates flags cleared before a frame-observing callee.
      {0x55, 0x89, 0xe5, 0x31, 0xc0, 0xe8, 0x16, 0x00, 0x00, 0x00, 0x74, 0x04,
       0x31, 0xc0, 0x5d, 0xc3, 0x31, 0xc0, 0x5d, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc,
       0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x89, 0xe0, 0xa9, 0x0f,
       0x00, 0x00, 0x00, 0xb8, 0x00, 0x00, 0x00, 0x00, 0xc3}};
  for (size_t Index = 0; Index != Programs.size(); ++Index) {
    BinaryImage Image;
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Segment Text;
    Text.Name = ".text";
    Text.VA = 0x401000;
    Text.Data = Programs[Index];
    Text.Size = Text.Data.size();
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Image.Segments.push_back(Text);
    Segment Data;
    Data.Name = ".data";
    Data.VA = 0x403000;
    Data.Data.resize(8);
    Data.Size = Data.Data.size();
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Segments.push_back(Data);
    LowFunc Source;
    Source.Entry = 0x402000;
    Source.Blocks.emplace_back();
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.NumInputs = 1;
    Call.Inputs[0] = NdVar::cst(Text.VA, 4);
    Source.Blocks.front().Ops.push_back(Call);
    EXPECT_EQ(hasCallerCleanupRegistrationABI(Source, Image),
              Index == 2 || Index == 4 || Index == 15)
        << Index;
    if (Index == 15) {
      auto &States = Source.RegistrationStates.emplace();
      States.ImageReadsComplete = true;
      States.ImageReads = {{0x403002, 0x403003}};
      EXPECT_FALSE(hasCallerCleanupRegistrationABI(Source, Image));
      States.ImageReads = {{0x403004, 0x403008}};
      EXPECT_TRUE(hasCallerCleanupRegistrationABI(Source, Image));
      States.ImageReadsComplete = false;
      EXPECT_FALSE(hasCallerCleanupRegistrationABI(Source, Image));
    }
    if (Index == 2) {
      Source.Entry = Text.VA;
      EXPECT_FALSE(hasCallerCleanupRegistrationABI(Source, Image));
    }
  }
}

TEST(WindowsRegistrationNative, RejectsCallbackChainObservationsAtomically) {
  RegistrationNativeFixture F;
  llvm::IRBuilder<> B(F.Filter->getTerminator());
  auto *Read = B.CreateLoad(B.getInt32Ty(),
                            llvm::ConstantPointerNull::get(B.getPtrTy(257)));
  MedLLVMEmitterTestPeer::chain(F.Emitter, *Read, 0x1021, 4);
  F.Source.RegistrationStates->ChainAccesses.push_back(
      {0x1021, 0x1022, 4, RegistrationChainAccess::Kind::ReadInstalledHead});
  const auto Before = F.print();
  EXPECT_FALSE(F.lower());
  EXPECT_EQ(F.print(), Before);
}

TEST(WindowsRegistrationNative, RejectsAnAtomicFSOccurrenceAtomically) {
  RegistrationNativeFixture F;
  llvm::IRBuilder<> B(F.Try->getTerminator());
  B.CreateAtomicRMW(llvm::AtomicRMWInst::Add,
                    llvm::ConstantPointerNull::get(B.getPtrTy(257)),
                    B.getInt32(1), llvm::Align(4),
                    llvm::AtomicOrdering::SequentiallyConsistent);
  const auto Before = F.print();
  EXPECT_FALSE(F.lower());
  EXPECT_EQ(F.print(), Before);
}

TEST(WindowsRegistrationNative, RejectsFramePointersPassedToUnknownCallees) {
  RegistrationNativeFixture F;
  MedOp Root;
  Root.Opcode = NdOp::COPY;
  Root.Addr = 0x1011;
  Root.OriginSeq = 10;
  Root.Output.Kind = MedVar::Temp;
  Root.Output.Id = 12;
  Root.Output.SSAVer = 1;
  Root.Output.Size = 4;
  F.Source.Blocks[1].Ops.push_back(Root);
  F.Source.RegistrationStates->FrameValues.push_back({0x1011, 10, -4});
  MedCallInfo Call;
  Call.BlockId = 1;
  Call.Args.push_back(Root.Output);
  F.Source.CallInfos.push_back(Call);
  const auto Before = F.print();
  EXPECT_FALSE(F.lower());
  EXPECT_EQ(F.print(), Before);
}

TEST(WindowsRegistrationNative, RejectsFramePointerEscapeThroughAPhi) {
  RegistrationNativeFixture F;
  MedOp Root;
  Root.Opcode = NdOp::COPY;
  Root.Addr = 0x1011;
  Root.OriginSeq = 10;
  Root.Output.Kind = MedVar::Temp;
  Root.Output.Id = 12;
  Root.Output.SSAVer = 1;
  Root.Output.Size = 4;
  F.Source.Blocks[1].Ops.push_back(Root);
  F.Source.RegistrationStates->FrameValues.push_back(
      {0x1011, 10, std::nullopt});
  PhiNode Phi;
  Phi.Output = Root.Output;
  Phi.Output.Id = 13;
  Phi.Args.emplace_back(1, Root.Output);
  F.Source.Blocks[3].Phis.push_back(Phi);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Phi.Output);
  F.Source.Blocks[3].Ops.push_back(Return);
  const auto Before = F.print();
  EXPECT_FALSE(F.lower());
  EXPECT_EQ(F.print(), Before);
}

TEST(WindowsRegistrationNative, SynchronizesRuntimeTryLevelBeforeHandlerEntry) {
  RegistrationNativeFixture F;
  ASSERT_TRUE(F.lower());
  const auto Text = F.print();
  EXPECT_NE(Text.find("store volatile i32 -1, ptr"), std::string::npos);
  EXPECT_NE(Text.find("i32 56"), std::string::npos);
}

#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
CompiledImage compileFixture(RegistrationNativeFixture &F) {
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();
  return compileImageForPatch(
      F.Module, Arch::X86, BinaryFormat::COFF, 0x6000,
      [](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "__except_handler3" || Symbol == "__except_handler4")
          return 0x2000;
        if (Symbol == "___security_cookie")
          return 0x2500;
        if (Symbol == "@__security_check_cookie@4")
          return 0x2600;
        if (Symbol == "_raise")
          return 0x5000;
        return std::nullopt;
      },
      0x1000, "i686-pc-windows-msvc");
}

TEST(WindowsRegistrationNative, CompilerClosesTheIndexedEH3ScopeTable) {
  RegistrationNativeFixture F;
  ASSERT_TRUE(F.lower());
  ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
  auto Compiled = compileFixture(F);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_TRUE(Compiled.WinEHSemanticsValid);
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 1u);
  const auto &Row = Compiled.WinEHSemanticRecords.front();
  EXPECT_EQ(Row.Encoding,
            llvm::mc_rewrite::RewriteWinEHSemanticEncoding::X86SEH3);
  EXPECT_EQ(Row.GeneratedState, 0u);
  EXPECT_EQ(Row.EnclosingState, -1);
  EXPECT_EQ(Row.RecordVA, Row.ContainerVA);
  EXPECT_EQ(Row.ContainerEndVA, Row.ContainerVA + 12);
  EXPECT_NE(Row.FilterVA, 0u);
  EXPECT_NE(Row.HandlerVA, 0u);
  auto Error = validateCOFFRegistrationSemanticRows(
      *F.Parent, *F.Source.ExceptionMetadata, Compiled);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
}

TEST(WindowsRegistrationNative, AuthenticatesEveryEH4CookieFrameOffset) {
  RegistrationNativeFixture F;
  auto &EH = *F.Source.ExceptionMetadata;
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH4;
  EH.Personality = ExceptionPersonality::ExceptHandler4;
  auto &Chain = *EH.Registration;
  Chain.SeededTryLevel = -2;
  Chain.GSCookieOffset = -2;
  Chain.EHCookieOffset = -20;
  Chain.Scopes.front().EnclosingLevel = -2;
  Chain.TryLevelStores.back().Level = -2;
  F.Source.RegistrationStates->SecurityCookiesComplete = true;
  F.Source.RegistrationStates->SecurityCookieVA = 0x2500;
  for (auto &State : F.Source.RegistrationStates->Blocks)
    for (auto &Level : State.Levels)
      if (Level == -1)
        Level = -2;
  ASSERT_TRUE(F.lower());
  auto Compiled = compileFixture(F);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 1u);
  const auto &Row = Compiled.WinEHSemanticRecords.front();
  EXPECT_EQ(Row.Encoding,
            llvm::mc_rewrite::RewriteWinEHSemanticEncoding::X86SEH4);
  EXPECT_EQ(Row.RegistrationCookieOffsets[0], -2);
  EXPECT_LT(Row.RegistrationCookieOffsets[2], 0);
  auto Valid = validateCOFFRegistrationSemanticRows(*F.Parent, EH, Compiled);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  // A different, plausible negative offset used to pass a sign-only check.
  llvm::support::endian::write32le(
      Compiled.Bytes.data() + Row.ContainerVA - Compiled.BaseVA + 8,
      uint32_t(Row.RegistrationCookieOffsets[2] - 4));
  auto Altered = validateCOFFRegistrationSemanticRows(*F.Parent, EH, Compiled);
  ASSERT_TRUE(bool(Altered));
  EXPECT_NE(llvm::toString(std::move(Altered)).find("machine frame offsets"),
            std::string::npos);
}

#ifdef LLVM_NEVERD_X86_REGISTRATION_GS
TEST(WindowsRegistrationNative, EH4GSUsesTheCompilerCookieAndFastcallChecker) {
  RegistrationNativeFixture F;
  for (auto &I : *F.Entry)
    if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(&I);
        Alloca && Alloca->getName() == "frame")
      Alloca->setAlignment(llvm::Align(64));
  auto &EH = *F.Source.ExceptionMetadata;
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH4;
  EH.Personality = ExceptionPersonality::ExceptHandler4;
  auto &Chain = *EH.Registration;
  Chain.SeededTryLevel = -2;
  Chain.GSCookieOffset = -32;
  Chain.EHCookieOffset = -28;
  Chain.HasSecurityCookies = true;
  Chain.Scopes.front().EnclosingLevel = -2;
  Chain.TryLevelStores.back().Level = -2;
  F.Source.RegistrationStates->SecurityCookiesComplete = true;
  F.Source.RegistrationStates->SecurityCookieVA = 0x2500;
  for (auto &State : F.Source.RegistrationStates->Blocks)
    for (auto &Level : State.Levels)
      if (Level == -1)
        Level = -2;
  ASSERT_TRUE(F.lower());
  EXPECT_TRUE(F.Parent->hasFnAttribute(llvm::Attribute::StackProtectReq));
  const auto *Check = F.Module.getFunction("__security_check_cookie");
  ASSERT_NE(Check, nullptr);
  EXPECT_TRUE(hasX86RegistrationSecurityCheckABI(*Check));
  auto Compiled = compileFixture(F);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 1u);
  const auto &Row = Compiled.WinEHSemanticRecords.front();
  EXPECT_NE(Row.RegistrationCookieOffsets[0], -2);
  EXPECT_EQ(Row.RegistrationCookieOffsets[1], 0);
  auto Valid = validateCOFFRegistrationSemanticRows(*F.Parent, EH, Compiled);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
}
#endif

TEST(WindowsRegistrationNative, BindsGeneratedParentsToTheSourceScopeGraph) {
  RegistrationNativeFixture F;
  auto &EH = *F.Source.ExceptionMetadata;
  EH.CodeRange.End = 0x1050;
  EH.Registration->Scopes.push_back({0, 0x1020, 0x1040, false});
  EH.Registration->TryLevelStores.front().Level = 1;
  F.Source.RegistrationStates->Blocks[1].Levels = {1};
  F.Source.RegistrationStates->Blocks.push_back(
      {4, {0x1040, 0x1050}, {0}, false, false, true});
  MedBlock Handler;
  Handler.Id = 4;
  Handler.StartAddr = 0x1040;
  Handler.EndAddr = 0x1050;
  F.Source.Blocks.push_back(Handler);
  auto *InnerHandler =
      llvm::BasicBlock::Create(F.Context, "inner.handler", F.Parent);
  llvm::IRBuilder<>(InnerHandler).CreateBr(F.Handler);
  F.Blocks.emplace(4, InnerHandler);
  ASSERT_TRUE(F.lower());
  auto Compiled = compileFixture(F);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 2u);
  auto Valid = validateCOFFRegistrationSemanticRows(*F.Parent, EH, Compiled);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  for (auto &Row : Compiled.WinEHSemanticRecords)
    if (Row.Token.Region == 1) {
      Row.EnclosingState = -1;
      llvm::support::endian::write32le(
          Compiled.Bytes.data() + Row.RecordVA - Compiled.BaseVA, UINT32_MAX);
    }
  EXPECT_TRUE(llvm::mc_rewrite::validateRewriteWinEHSemanticRecords(
      Compiled.WinEHSemanticRecords, Compiled.SourceFunctionOwners,
      Compiled.FunctionRanges, Compiled.FunctionOwnerAddrs));
  auto Altered = validateCOFFRegistrationSemanticRows(*F.Parent, EH, Compiled);
  ASSERT_TRUE(bool(Altered));
  EXPECT_NE(llvm::toString(std::move(Altered)).find("source scope graph"),
            std::string::npos);
}

TEST(WindowsRegistrationNative, CompilerOwnsTheFinallyTableTarget) {
  RegistrationNativeFixture F;
  auto &Scope = F.Source.ExceptionMetadata->Registration->Scopes.front();
  Scope.IsFinally = true;
  Scope.FilterVA = 0;
  Scope.HandlerVA = 0x1020;
  F.Source.RegistrationStates->Blocks[2].CanDispatch = true;
  ASSERT_TRUE(F.lower());
  auto Compiled = compileFixture(F);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 1u);
  const auto &Row = Compiled.WinEHSemanticRecords.front();
  EXPECT_EQ(Row.FilterVA, 0u);
  EXPECT_NE(Row.HandlerVA, Compiled.SourceFunctionOwners.back().OwnerVA);
  auto Error = validateCOFFRegistrationSemanticRows(
      *F.Parent, *F.Source.ExceptionMetadata, Compiled);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
}
#endif
} // namespace
