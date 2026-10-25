//===- WindowsRegistrationCleanupOnlyTests.cpp - PE32 unwind-only IR -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/codegen/COFF/COFFRegistrationCxxIRProof.h"
#include "../../../lib/backend/codegen/COFF/COFFRegistrationCxxStateProof.h"
#include "RegistrationEntryRelocationTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/ir/med/X86RegistrationCallback.h"
#include "neverd/ir/med/X86RegistrationPhysicalFrame.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/object/PELayout.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Verifier.h"
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
#include "llvm/IR/WinEHFrame.h"
#endif
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

using namespace neverd;

namespace {
#ifdef LLVM_NEVERD_X86_CXX_EH_MODE
void checkSynchronousCleanupMode(const llvm::Module &Module,
                                 const MedFunc &Source,
                                 const BinaryImage &Image) {
  ASSERT_TRUE(Source.ExceptionMetadata);
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Mixed = llvm::CloneModule(Module);
    Mixed->addModuleFlag(llvm::Module::Warning, "eh-asynch", 1);
    auto *Parent = Mixed->getFunction(Source.Name);
    ASSERT_TRUE(Parent);
    ASSERT_TRUE(
        Parent->hasFnAttribute(llvm::RewriteWinX86SynchronousCxxAttribute));
    if (Mutation == 1)
      Parent->removeFnAttr(llvm::RewriteWinX86SynchronousCxxAttribute);
    if (Mutation == 2)
      Parent->addFnAttr(llvm::RewriteWinX86SynchronousCxxAttribute, "async");
    if (Mutation == 3)
      Parent->removeFnAttr(llvm::Attribute::NoInline);
    if (Mutation == 4)
      Parent->removeFnAttr(llvm::RewriteWinX86CxxFrameAttribute);
    auto Checked = validateCOFFRegistrationCxxIR(
        *Parent, *Source.ExceptionMetadata, Image);
    if (Mutation) {
      EXPECT_TRUE(bool(Checked));
      llvm::consumeError(std::move(Checked));
    } else {
      ASSERT_FALSE(bool(Checked)) << llvm::toString(std::move(Checked));
      ASSERT_FALSE(llvm::verifyModule(*Mixed, &llvm::errs()));
      auto Compiled = Codegen().compile(*Mixed, Arch::X86, BinaryFormat::COFF);
      EXPECT_TRUE(Compiled.Success);
    }
  }
}
#endif

#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
TEST(WindowsRegistrationCxxStateMap, LargeGeneratedStateMapsKeepExactEdges) {
  for (bool Sparse : {false, true}) {
    SCOPED_TRACE(Sparse);
    CxxExceptionInfo Source;
    Source.MaxState = limits::kMaxRegistrationEHRecords;
    CompiledImage Native;
    Native.BaseVA = 0x600000;
    const uint32_t Count = Sparse ? 2 : Source.MaxState;
    Native.Bytes.resize(Count * 8);
    std::vector<CompiledWinEHSemanticRecord> Rows(Count);
    std::map<uint32_t, const CompiledWinEHSemanticRecord *> BySource;
    for (uint32_t State = 0; State != Source.MaxState; ++State) {
      const bool Action = !Sparse || State == 0 || State + 1 == Source.MaxState;
      const int32_t Outer = State && (Sparse || State % 128) ? State - 1 : -1;
      Source.UnwindMap.push_back(
          {Outer, Action ? va_t(0x401000 + State * 16) : 0});
      if (!Action)
        continue;
      const uint32_t Generated = Sparse ? unsigned(State != 0) : State;
      Rows[Generated].GeneratedState = Generated;
      BySource.emplace(State, &Rows[Generated]);
      writeLE<uint32_t>(Native.Bytes.data() + Generated * 8,
                        Sparse && State ? 0 : uint32_t(Outer));
    }
    COFFRegistrationCxxTableReceipt Receipt;
    auto Checked = coff_registration::validateCxxGeneratedStates(
        Source, Native, {}, Native.BaseVA, Count, BySource, Receipt);
    ASSERT_FALSE(bool(Checked)) << llvm::toString(std::move(Checked));
    ASSERT_EQ(Receipt.SourceToGeneratedStates.size(), Source.MaxState + 1);
    for (uint32_t State = 0; State != Source.MaxState; ++State)
      EXPECT_EQ(Receipt.SourceToGeneratedStates.at(State),
                Sparse ? int32_t(State + 1 == Source.MaxState)
                       : int32_t(State));
    for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto Changed = Native;
      auto Mapped = BySource;
      uint32_t States = Count;
      if (Mutation == 0)
        writeLE<uint32_t>(Changed.Bytes.data() + 8, uint32_t(-1));
      if (Mutation == 1)
        Mapped.erase(std::prev(Mapped.end()));
      if (Mutation == 2)
        ++States;
      COFFRegistrationCxxTableReceipt Failed;
      auto Rejected = coff_registration::validateCxxGeneratedStates(
          Source, Changed, {}, Changed.BaseVA, States, Mapped, Failed);
      EXPECT_TRUE(bool(Rejected));
      llvm::consumeError(std::move(Rejected));
    }
  }
}

void checkSharedCleanupDeclarations(const BinaryImage &Image,
                                    const MedFunc &Outer, va_t InnerEntry) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  auto Low = CFGBuilder().build(Image, Decode, InnerEntry, "cleanup_inner");
  ASSERT_TRUE(Low.RegistrationStates && Low.RegistrationStates->Complete);
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  auto Inner = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  inferMedTypes(Inner, Arch::X86);
  recoverCallAbi(Inner, Arch::X86, {}, &Image);
  for (bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    std::vector<MedFunc> Functions{Outer, Inner};
    if (Reverse)
      std::reverse(Functions.begin(), Functions.end());
    llvm::LLVMContext Context;
    MedLLVMEmitter Emitter;
    auto Module = Emitter.emit(Functions, Context, "shared-cleanup", Arch::X86,
                               {}, &Image, BinaryFormat::COFF);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    for (const auto &Source : Functions) {
      const auto *Parent = Module->getFunction(Source.Name);
      ASSERT_TRUE(Parent);
      ASSERT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment))
          << Source.Name;
      ASSERT_TRUE(Source.ExceptionMetadata);
      auto Error = validateCOFFRegistrationCxxIR(
          *Parent, *Source.ExceptionMetadata, Image);
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    }
  }
}

void checkCleanupOnlyInput(const char *Path, bool RegisterStateWrite) {
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Export = llvm::find_if(Image->Exports, [](const auto &Entry) {
    return Entry.Name == "cleanup_only_parent";
  });
  ASSERT_NE(Export, Image->Exports.end());
  const va_t ParentEntry = Export->Addr;
  const auto Nested = llvm::find_if(Image->Exports, [](const auto &Entry) {
    return Entry.Name == "cleanup_only_inner";
  });
  const va_t NestedTarget = Nested == Image->Exports.end() ? 0 : Nested->Addr;
  const auto *Source = Image->ExceptionMetadata.findFunction(ParentEntry);
  ASSERT_TRUE(Source && Source->Cxx && Source->Registration);
  if (RegisterStateWrite) {
    const auto &Chain = *Source->Registration;
    const auto Store = llvm::find_if(Chain.TryLevelStores, [](const auto &S) {
      return S.Level == 0 || S.Level == 1;
    });
    ASSERT_NE(Store, Chain.TryLevelStores.end());
    const va_t Address = Store->StoreVA;
    const bool StackBased =
        Chain.RealignedFrame && Chain.RealignedFrame->BaseRegister == 4;
    const unsigned StoreSize = StackBased ? 8 : 7;
    const unsigned Displacement = StackBased ? 3 : 2;
    const auto *Bytes = Image->readVA(Address, StoreSize + 5);
    ASSERT_TRUE(Bytes);
    ASSERT_EQ(Store->EndVA - Address, StoreSize);
    ASSERT_EQ(Bytes[0], 0xc7);
    ASSERT_EQ(Bytes[1], StackBased ? 0x44 : 0x45);
    if (StackBased)
      ASSERT_EQ(Bytes[2], 0x24);
    ASSERT_EQ((StackBased ? Chain.RealignedFrame->BaseOffset : 0) +
                  int8_t(Bytes[Displacement]),
              Chain.TryLevelOffset);
    ASSERT_EQ(readLE<uint32_t>(Bytes + Displacement + 1),
              uint32_t(Store->Level));
    // EAX is scratch immediately before either authored no-argument callee.
    ASSERT_EQ(Bytes[StoreSize], 0xe8);
    const auto Throw = llvm::find_if(Image->Exports, [](const auto &Entry) {
      return Entry.Name == "cleanup_only_throw";
    });
    ASSERT_NE(Throw, Image->Exports.end());
    ASSERT_EQ(Address + StoreSize + 5 +
                  int32_t(readLE<uint32_t>(Bytes + StoreSize + 1)),
              NestedTarget ? NestedTarget : Throw->Addr);
    std::vector<uint8_t> Replacement = {
        0x31, 0xc0, uint8_t(Store->Level ? 0x40 : 0x90), 0x89,
        uint8_t(StackBased ? 0x44 : 0x45)};
    if (StackBased)
      Replacement.push_back(0x24);
    Replacement.push_back(Bytes[Displacement]);
    Replacement.push_back(0x90);
    auto Input = llvm::MemoryBuffer::getFile(Path);
    ASSERT_TRUE(bool(Input));
    const auto Buffer = (*Input)->getBuffer();
    std::vector<uint8_t> Changed(Buffer.bytes_begin(), Buffer.bytes_end());
    bool Patched = false;
    for (const auto &Segment : Image->Segments)
      if (Address >= Segment.VA && Address - Segment.VA < Segment.Data.size() &&
          Replacement.size() <= Segment.Data.size() - (Address - Segment.VA)) {
        ASSERT_FALSE(Patched);
        const uint64_t Offset = Segment.FileOff + Address - Segment.VA;
        ASSERT_TRUE(rangeInBounds(Offset, Replacement.size(), Changed.size()));
        std::copy(Replacement.begin(), Replacement.end(),
                  Changed.begin() + Offset);
        Patched = true;
      }
    ASSERT_TRUE(Patched);
    // Reload the changed PE through all loader phases. Registration parsing
    // consumes and prunes discovered callback symbols, so clearing metadata
    // alone cannot reconstruct the original discovery input. Neither new
    // instruction sequence supplies a state-store observation; LowIR must
    // prove its value from the actual bytes.
    Image = COFFLoader().loadBuffer(llvm::MemoryBufferRef(
        llvm::StringRef(reinterpret_cast<const char *>(Changed.data()),
                        Changed.size()),
        Path));
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    Source = Image->ExceptionMetadata.findFunction(ParentEntry);
    ASSERT_TRUE(Source && Source->Cxx && Source->Registration);
    EXPECT_TRUE(
        llvm::none_of(Source->Registration->TryLevelStores, [&](const auto &S) {
          return S.StoreVA >= Address &&
                 S.StoreVA < Address + Replacement.size();
        }));
  }
  ASSERT_TRUE(Source->Cxx->TryBlocks.empty());
  const auto Actions =
      llvm::count_if(Source->Cxx->UnwindMap,
                     [](const auto &Action) { return Action.ActionVA; });
  ASSERT_GE(Actions, 1);
  ASSERT_LE(Actions, 2);
  ASSERT_TRUE(
      classifyWindowsEHNativeSource(*Source, Arch::X86, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::IRLowering)
          .canLowerNativeIR());
#ifdef LLVM_NEVERD_X86_CXX_HANDLER_RECEIPTS
  EXPECT_TRUE(
      classifyWindowsEHNativeSource(*Source, Arch::X86, BinaryFormat::COFF)
          .canPatchOutput());
#else
  EXPECT_FALSE(
      classifyWindowsEHNativeSource(*Source, Arch::X86, BinaryFormat::COFF)
          .canPatchOutput());
#endif
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  auto Low = CFGBuilder().build(*Image, Decode, ParentEntry, "cleanup_only");
  ASSERT_TRUE(Low.RegistrationStates);
  for (const auto &Diagnostic : Low.RegistrationStates->Diagnostics)
    llvm::errs() << Diagnostic << '\n';
  ASSERT_TRUE(Low.RegistrationStates->Complete);
  ASSERT_TRUE(Low.RegistrationStates->CleanupFrameEffectsComplete);
  ASSERT_EQ(Low.RegistrationStates->CleanupContracts.size(), Actions);
  size_t CleanupCalls = 0;
  for (const auto &Contract : Low.RegistrationStates->CleanupContracts)
    CleanupCalls += Contract.Calls.size();
  ASSERT_EQ(CleanupCalls, 2u);
  EXPECT_TRUE(Low.RegistrationStates->CxxContinuations.empty());
  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  if (Low.RegistrationStates->CxxPrologue) {
    ASSERT_TRUE(Med.RegistrationPhysicalFrame);
    ASSERT_TRUE(hasCheckedX86RegistrationCxxPhysicalFrame(*Image, Med));
    EXPECT_EQ(Med.RegistrationStates->CxxPrologue,
              Low.RegistrationStates->CxxPrologue);
  }
  inferMedTypes(Med, Arch::X86);
  recoverCallAbi(Med, Arch::X86, {}, &*Image);
  if (NestedTarget)
    for (const auto &Block : Med.Blocks)
      for (size_t I = 0; I != Block.Ops.size(); ++I) {
        const auto &Op = Block.Ops[I];
        if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
            !Op.Inputs[0].isConst() || Op.Inputs[0].ConstVal != NestedTarget)
          continue;
        const auto *Call = Med.findCall(Block.Id, I);
        ASSERT_TRUE(Call);
        EXPECT_TRUE(Call->Args.empty());
        EXPECT_EQ(Call->IntegerRegisterArgCount, 0);
        EXPECT_EQ(Call->FloatingRegisterArgCount, 0);
      }
  const auto Owners = registrationCatchBlocks(Med);
  const auto Parents = registrationCleanupParents(Med);
  ASSERT_TRUE(Owners && Parents);
  EXPECT_TRUE(Owners->empty());
  ASSERT_EQ(Parents->size(), Actions);
  for (const auto &[State, Owner] : *Parents)
    EXPECT_FALSE(Owner);
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Med;
    auto &States = *Changed.RegistrationStates;
    if (Mutation == 0)
      States.CallbackStatesComplete = false;
    if (Mutation == 1)
      States.RegistrationLifetimeComplete = false;
    if (Mutation == 2) {
      auto State = llvm::find_if(
          States.Blocks, [](const auto &Block) { return Block.Reached; });
      ASSERT_NE(State, States.Blocks.end());
      State->CallbackOnly = true;
      State->CxxCatchStacks = {{{0, 0}}};
    }
    if (Mutation == 3)
      States.CxxContinuations.emplace_back();
    EXPECT_FALSE(registrationCatchBlocks(Changed));
    EXPECT_FALSE(registrationCleanupParents(Changed));
  }
  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  auto Module = Emitter.emit({Med}, Context, "cleanup-only", Arch::X86, {},
                             &*Image, BinaryFormat::COFF);
  ASSERT_TRUE(Module);
  auto *Parent = Module->getFunction(Med.Name);
  ASSERT_TRUE(Parent);
  ASSERT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
#ifdef LLVM_NEVERD_X86_CXX_EH_MODE
  ASSERT_NO_FATAL_FAILURE(checkSynchronousCleanupMode(*Module, Med, *Image));
#endif
  if (NestedTarget)
    ASSERT_NO_FATAL_FAILURE(
        checkSharedCleanupDeclarations(*Image, Med, NestedTarget));
  if (Med.RegistrationPhysicalFrame)
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto Changed = Med;
      if (Mutation == 0)
        Changed.RegistrationPhysicalFrame.reset();
      else if (Mutation == 1)
        Changed.RegistrationStates->CxxPrologue.reset();
      else {
        auto Frame = std::make_shared<X86RegistrationCxxPhysicalFrame>(
            *Changed.RegistrationPhysicalFrame);
        if (Mutation == 2)
          Frame->Exits.clear();
        else
          Frame->EntryCall.Writes.clear();
        Changed.RegistrationPhysicalFrame = Frame;
      }
      llvm::LLVMContext OtherContext;
      MedLLVMEmitter OtherEmitter;
      auto Other =
          OtherEmitter.emit({Changed}, OtherContext, "changed-frame", Arch::X86,
                            {}, &*Image, BinaryFormat::COFF);
      if (Other) {
        const auto *Function = Other->getFunction(Med.Name);
        ASSERT_TRUE(Function);
        EXPECT_FALSE(Function->getMetadata(windows_eh_md::NativeAttachment));
      }
    }
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  auto Control = validateCOFFRegistrationCxxControlIR(*Parent, *Source, *Image);
  ASSERT_FALSE(bool(Control)) << llvm::toString(std::move(Control));
  auto Frame = validateCOFFRegistrationCxxIR(*Parent, *Source, *Image);
  ASSERT_FALSE(bool(Frame)) << llvm::toString(std::move(Frame));

  unsigned Invokes = 0, Cleanups = 0, ToCaller = 0;
  for (auto &Block : *Parent)
    for (auto &I : Block) {
      EXPECT_FALSE((llvm::isa<llvm::CatchPadInst, llvm::CatchSwitchInst,
                              llvm::CatchReturnInst>(I)));
      Invokes += llvm::isa<llvm::InvokeInst>(I);
      Cleanups += llvm::isa<llvm::CleanupPadInst>(I);
      if (const auto *Return = llvm::dyn_cast<llvm::CleanupReturnInst>(&I))
        ToCaller += !Return->getUnwindDest();
    }
  EXPECT_GE(Invokes, 1u);
  EXPECT_EQ(Cleanups, Actions);
  EXPECT_EQ(ToCaller, 1u);
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = llvm::CloneModule(*Module);
    auto *Function = Changed->getFunction(Parent->getName());
    llvm::CleanupReturnInst *Inner = nullptr;
    for (auto &Block : *Function)
      if (auto *Return =
              llvm::dyn_cast<llvm::CleanupReturnInst>(Block.getTerminator());
          Return && (!Inner || Return->getUnwindDest()))
        Inner = Return;
    ASSERT_TRUE(Inner);
    auto *Outer = Inner->getUnwindDest();
    llvm::InvokeInst *Invoke = nullptr;
    std::vector<llvm::CallInst *> CleanupCalls;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        if (auto *Call = llvm::dyn_cast<llvm::InvokeInst>(&I);
            Call && Call->getUnwindDest() == Inner->getParent())
          Invoke = Call;
        if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
            Call && &Block == Inner->getParent() &&
            !llvm::isa<llvm::IntrinsicInst>(Call))
          CleanupCalls.push_back(Call);
      }
    ASSERT_TRUE(Invoke && !CleanupCalls.empty());
    auto *CleanupCall = CleanupCalls.front();
    llvm::IRBuilder<> B(Inner);
    switch (Mutation) {
    case 0: {
      // A verifier-valid ordinary call drops the complete cleanup path.
      B.SetInsertPoint(Invoke);
      llvm::SmallVector<llvm::Value *, 4> Args(Invoke->args());
      auto *Call = B.CreateCall(Invoke->getFunctionType(),
                                Invoke->getCalledOperand(), Args);
      Call->setCallingConv(Invoke->getCallingConv());
      Call->setAttributes(Invoke->getAttributes());
      Call->copyMetadata(*Invoke);
      Invoke->replaceAllUsesWith(Call);
      B.CreateBr(Invoke->getNormalDest());
      Invoke->eraseFromParent();
      break;
    }
    case 1:
      if (Outer) {
        B.CreateCleanupRet(Inner->getCleanupPad(), nullptr);
        Inner->eraseFromParent();
      } else {
        ASSERT_EQ(CleanupCalls.size(), 2u);
        // O1 combines both actions. Reverse the borrowed objects while
        // preserving dominance and otherwise valid LLVM control flow.
        B.SetInsertPoint(CleanupCall);
        auto *Second = B.Insert(
            llvm::cast<llvm::Instruction>(CleanupCalls.back()->getArgOperand(0))
                ->clone());
        CleanupCalls.back()->setArgOperand(0, CleanupCall->getArgOperand(0));
        CleanupCall->setArgOperand(0, Second);
      }
      break;
    case 2:
      CleanupCall->eraseFromParent();
      break;
    case 3:
      B.SetInsertPoint(CleanupCall);
      CleanupCall->setArgOperand(0, B.CreateGEP(B.getInt8Ty(),
                                                CleanupCall->getArgOperand(0),
                                                B.getInt32(4)));
      break;
    case 4:
      CleanupCall->removeFnAttr(llvm::Attribute::NoUnwind);
      break;
    case 5:
      Invoke->setMetadata(windows_eh_md::RegistrationOperationAttachment,
                          nullptr);
      break;
    case 6:
      Function->setMetadata(windows_eh_md::NativeAttachment, nullptr);
      break;
    case 7: {
      llvm::StoreInst *Initialization = nullptr;
      for (auto &Block : *Function)
        for (auto &I : Block) {
          auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
          auto *Value =
              Store
                  ? llvm::dyn_cast<llvm::ConstantInt>(Store->getValueOperand())
                  : nullptr;
          if (!Initialization && Value &&
              Value->equalsInt(NestedTarget ? 3 : 1) &&
              Store->getMetadata(
                  windows_eh_md::RegistrationOperationAttachment))
            Initialization = Store;
        }
      ASSERT_TRUE(Initialization);
      Initialization->setOperand(
          0,
          llvm::UndefValue::get(Initialization->getValueOperand()->getType()));
      auto Control =
          validateCOFFRegistrationCxxControlIR(*Function, *Source, *Image);
      ASSERT_FALSE(bool(Control)) << llvm::toString(std::move(Control));
      break;
    }
    }
    ASSERT_FALSE(llvm::verifyModule(*Changed, &llvm::errs()));
    auto Error = validateCOFFRegistrationCxxIR(*Function, *Source, *Image);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
  for (unsigned Lifetime = 0; Lifetime != 3; ++Lifetime) {
    SCOPED_TRACE(Lifetime);
    auto Changed = llvm::CloneModule(*Module);
    auto *Function = Changed->getFunction(Parent->getName());
    auto Proof = coff_registration::getCheckedCxxControlIRProof(
        *Function, *Source, *Image);
    ASSERT_TRUE(bool(Proof)) << llvm::toString(Proof.takeError());
    auto *Allocation = const_cast<llvm::AllocaInst *>(Proof->Frame.Slot);
    ASSERT_TRUE(Allocation && Allocation->getNextNode());
    llvm::IRBuilder<> B(Allocation->getNextNode());
    B.CreateLifetimeStart(Allocation);
    llvm::InvokeInst *Invoke = nullptr;
    for (auto &Block : *Function)
      if (auto *Call =
              llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator())) {
        Invoke = Call;
        break;
      }
    ASSERT_TRUE(Invoke);
    ASSERT_TRUE(Invoke->getPrevNode());
    B.SetInsertPoint(Invoke->getPrevNode());
    if (Lifetime == 1)
      B.CreateLifetimeEnd(Allocation);
    if (Lifetime == 2)
      B.CreateLifetimeStart(Allocation);
    ASSERT_FALSE(llvm::verifyModule(*Changed, &llvm::errs()));
    auto Control =
        validateCOFFRegistrationCxxControlIR(*Function, *Source, *Image);
    ASSERT_FALSE(bool(Control)) << llvm::toString(std::move(Control));
    auto Error = validateCOFFRegistrationCxxIR(*Function, *Source, *Image);
    // A start permits access, but every start also discards initialized bytes.
    EXPECT_EQ(bool(Error), Lifetime != 0);
    const auto Message = llvm::toString(std::move(Error));
    if (Lifetime == 1)
      EXPECT_NE(Message.find("outside its allocation lifetime"),
                std::string::npos)
          << Message;
    if (Lifetime == 2)
      EXPECT_NE(Message.find("not initialized on every path"),
                std::string::npos)
          << Message;
  }
  // Keep the object compile independent: code generation may mutate its IR.
  auto ObjectModule = llvm::CloneModule(*Module);
  auto Compiled =
      Codegen().compile(*ObjectModule, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(Compiled.Success);
#ifdef LLVM_NEVERD_X86_CXX_HANDLER_RECEIPTS
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
      llvm::SmallString<64> Name;
      llvm::Mangler Mangler;
      Mangler.getNameWithPrefix(Name, &Function, false);
      if (Name != Symbol)
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
  // The register-state variant above edits the decoded source in memory.
  // Exercise installation against the unmodified on-disk fixture here.
  if (RegisterStateWrite)
    return;
  auto Input = llvm::MemoryBuffer::getFile(Path);
  ASSERT_TRUE(bool(Input));
  const auto Bytes = (*Input)->getBuffer();
  std::vector<uint8_t> Binary(Bytes.bytes_begin(), Bytes.bytes_end());
  COFFPatcher Patcher;
  const va_t CodeVA = Patcher.plannedExecSegmentVA(Binary, Arch::X86);
  auto Native = compileImageForPatch(*Module, Arch::X86, BinaryFormat::COFF,
                                     CodeVA, Resolve, Image->Base);
  ASSERT_TRUE(Native.Success);
  ASSERT_TRUE(Native.Unresolved.empty());
  ASSERT_EQ(Native.WinEHSemanticRecords.size(), Actions);
  for (const auto &Row : Native.WinEHSemanticRecords) {
    ASSERT_EQ(Row.Token.Kind,
              llvm::mc_rewrite::RewriteWinEHSemanticKind::CxxCleanup);
    ASSERT_TRUE(Row.X86CxxLayout);
    EXPECT_EQ(Row.X86CxxLayout->Tables[2].BeginVA, 0u);
    EXPECT_EQ(Row.X86CxxLayout->Tables[2].EndVA, 0u);
    EXPECT_EQ(Row.X86CxxLayout->Tables[3].BeginVA, 0u);
    EXPECT_EQ(Row.X86CxxLayout->Tables[3].EndVA, 0u);
  }
  auto Tables = getCheckedCOFFRegistrationCxxTableReceipt(*Parent, *Source,
                                                          *Image, Native);
  ASSERT_TRUE(bool(Tables)) << llvm::toString(Tables.takeError());
#ifdef LLVM_NEVERD_X86_CXX_EH_MODE
  {
    auto Mixed = llvm::CloneModule(*Module);
    Mixed->addModuleFlag(llvm::Module::Warning, "eh-asynch", 1);
    auto MixedNative = compileImageForPatch(
        *Mixed, Arch::X86, BinaryFormat::COFF, CodeVA, Resolve, Image->Base);
    ASSERT_TRUE(MixedNative.Success);
    ASSERT_TRUE(MixedNative.Unresolved.empty());
    const auto *MixedParent = Mixed->getFunction(Med.Name);
    ASSERT_TRUE(MixedParent);
    auto MixedTables = getCheckedCOFFRegistrationCxxTableReceipt(
        *MixedParent, *Source, *Image, MixedNative);
    ASSERT_TRUE(bool(MixedTables)) << llvm::toString(MixedTables.takeError());
  }
#endif
  EXPECT_EQ(Tables->Tables[2], (std::pair<va_t, va_t>{0, 0}));
  EXPECT_EQ(Tables->SourceToGeneratedStates.size(),
            Source->Cxx->UnwindMap.size() + 1);
  EXPECT_TRUE(Tables->GeneratedCxxGraph.TryBlocks.empty());
  EXPECT_EQ(Tables->AbsolutePointerFields.size(), Actions + 1);
  auto Handler = getCheckedCOFFRegistrationCxxHandlerReceipt(*Parent, *Source,
                                                             *Image, Native);
  ASSERT_TRUE(bool(Handler)) << llvm::toString(Handler.takeError());
  EXPECT_EQ(Handler->AbsolutePointerFields.size(), Actions + 3);
  for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Native;
    auto Word = [&](va_t Address, uint32_t Value) {
      writeLE<uint32_t>(Changed.Bytes.data() + Address - Changed.BaseVA, Value);
    };
    const va_t Info = Tables->FuncInfoVA;
    const auto Row = Changed.WinEHSemanticRecords.front();
    switch (Mutation) {
    case 0:
      Changed.WinEHSemanticRecords.clear();
      break;
    case 1:
      Changed.WinEHSemanticRecords.push_back(Row);
      break;
    case 2:
      Changed.WinEHSemanticRecords.front().X86CxxLayout.reset();
      break;
    case 3:
      Changed.WinEHSemanticRecords.front().X86CxxLayout->Tables[2] =
          Row.X86CxxLayout->Tables[1];
      break;
    case 4:
      Word(Info + 12, 1);
      break;
    case 5:
      Word(Info + 16, Tables->Tables[1].first);
      break;
    case 6:
      Changed.WinEHSemanticRecords.front().EnclosingState = Row.GeneratedState;
      break;
    case 7:
      Word(Row.RecordVA, Row.GeneratedState);
      break;
    case 8:
      Word(Row.RecordVA + 4, 0);
      break;
    case 9:
      for (auto &Section : Changed.Sections)
        std::erase_if(Section.FixupReferences, [&](const auto &Fixup) {
          return Section.VA + Fixup.Offset == Info + 8;
        });
      break;
    case 10: {
      bool Added = false;
      for (auto &Section : Changed.Sections) {
        const auto Fixup =
            llvm::find_if(Section.FixupReferences, [&](const auto &Candidate) {
              return Section.VA + Candidate.Offset == Info + 8;
            });
        if (Fixup == Section.FixupReferences.end())
          continue;
        auto Zero = *Fixup;
        Zero.Offset = Info + 16 - Section.VA;
        Zero.ResolvedValue = 0;
        Section.FixupReferences.push_back(Zero);
        Added = true;
        break;
      }
      ASSERT_TRUE(Added);
      break;
    }
    case 11:
      Word(Info + 4,
           (Tables->Tables[1].second - Tables->Tables[1].first) / 8 + 1);
      break;
    case 12:
      Changed.WinEHSemanticRecords.erase(Changed.WinEHSemanticRecords.begin());
      break;
    case 13:
      Changed.WinEHSemanticRecords.front().X86CxxLayout->Tables[0].EndVA += 4;
      break;
    }
    auto Rejected = getCheckedCOFFRegistrationCxxTableReceipt(*Parent, *Source,
                                                              *Image, Changed);
    EXPECT_FALSE(bool(Rejected));
    if (!Rejected)
      llvm::consumeError(Rejected.takeError());
  }
  {
    auto Changed = Native;
    Changed.Bytes[Handler->CodeRange.Begin - Changed.BaseVA] = 0x90;
    auto Rejected = getCheckedCOFFRegistrationCxxHandlerReceipt(
        *Parent, *Source, *Image, Changed);
    EXPECT_FALSE(bool(Rejected));
    if (!Rejected)
      llvm::consumeError(Rejected.takeError());
  }
  const std::pair<va_t, va_t> Mapping{Source->CodeRange.Begin, Tables->OwnerVA};
  if (Med.RegistrationPhysicalFrame &&
      Med.RegistrationPhysicalFrame->EntryCall.CallVA == Mapping.first + 7) {
    // The exact narrow entry retires the complete MOV handler operand even
    // though only two of its bytes lie under the five-byte jump. Adjacent
    // relocation fields and different operand bytes have no such proof.
    registration_test::checkRetiredEntryRelocationEdits(
        Binary, Mapping.first + 3 - Image->Base,
        [&](const std::vector<uint8_t> &Changed) {
          auto CompiledCopy = Native;
          auto Rejected = prepareCOFFRegistrationPatch(
              Changed, *Image, CompiledCopy, {Mapping}, CodeVA, *Module);
          EXPECT_FALSE(bool(Rejected));
          if (!Rejected)
            llvm::consumeError(Rejected.takeError());
        });
  }
  auto Update = prepareCOFFRegistrationPatch(Binary, *Image, Native, {Mapping},
                                             CodeVA, *Module);
  ASSERT_TRUE(bool(Update)) << llvm::toString(Update.takeError());
  ASSERT_TRUE(Update->Apply);
  ASSERT_EQ(Update->GeneratedCxxGraphs.size(), 1u);
  EXPECT_TRUE(Update->GeneratedCxxGraphs.front().TryBlocks.empty());
  ASSERT_EQ(Patcher.appendExecSegment(Binary, Native.Bytes, kNdTextSection,
                                      Arch::X86),
            CodeVA);
  auto PE = locatePEHeaders(Binary.data(), Binary.size());
  std::optional<size_t> Entry;
  forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
    const uint64_t RVA = Source->CodeRange.Begin - Image->Base;
    if (RVA >= Section.VirtualAddress &&
        rangeInBounds(RVA - Section.VirtualAddress, 5, Section.SizeOfRawData))
      Entry = Section.PointerToRawData + RVA - Section.VirtualAddress;
  });
  ASSERT_TRUE(Entry);
  ASSERT_TRUE(rangeInBounds(*Entry, 5, Binary.size()));
  Binary[*Entry] = 0xe9;
  writeLE<uint32_t>(Binary.data() + *Entry + 1,
                    uint32_t(Tables->OwnerVA - Source->CodeRange.Begin - 5));
  auto Applied = applyCOFFRegistrationPatch(Binary, *Update);
  ASSERT_FALSE(bool(Applied)) << llvm::toString(std::move(Applied));
  auto Checked = validateCOFFRegistrationPatch(Binary, *Update);
  ASSERT_FALSE(bool(Checked)) << llvm::toString(std::move(Checked));
  if (const char *Output =
          std::getenv("NEVERD_REGISTRATION_CLEANUP_ONLY_OUTPUT_PE32")) {
    std::error_code Error;
    llvm::raw_fd_ostream Stream(Output, Error, llvm::sys::fs::OF_None);
    ASSERT_FALSE(bool(Error)) << Error.message();
    Stream.write(reinterpret_cast<const char *>(Binary.data()), Binary.size());
    Stream.close();
    ASSERT_FALSE(Stream.has_error());
  }
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = *Update;
    if (Mutation == 0)
      Changed.GeneratedCxxGraphs.clear();
    if (Mutation == 1)
      Changed.GeneratedCxxGraphs.front().UnwindMap.front().ToState = 0;
    if (Mutation == 2)
      Changed.GeneratedCxxGraphs.front().UnwindMap.front().ActionVA += 1;
    auto Rejected = validateCOFFRegistrationPatch(Binary, Changed);
    EXPECT_TRUE(bool(Rejected));
    llvm::consumeError(std::move(Rejected));
  }
#endif
}

TEST(WindowsRegistrationCleanupOnly, InputPE32PreservesEveryUnwindAction) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_CLEANUP_ONLY_PE32");
  if (!Path)
    GTEST_SKIP() << "requires the registration_cleanup_only.cpp PE32 fixture";
  for (bool RegisterStateWrite : {false, true}) {
    SCOPED_TRACE(RegisterStateWrite);
    ASSERT_NO_FATAL_FAILURE(checkCleanupOnlyInput(Path, RegisterStateWrite));
  }
}
#endif
} // namespace
