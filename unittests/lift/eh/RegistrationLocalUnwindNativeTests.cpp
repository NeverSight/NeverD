//===- RegistrationLocalUnwindNativeTests.cpp - Native unwind binding ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check the source invocation, escaped parent chain and resulting state.

#include "../../../lib/backend/codegen/COFF/COFFRegistrationSEHLocalUnwind.h"
#include "../../../lib/backend/codegen/COFF/COFFRegistrationSEHParentExit.h"
#include "../../../lib/backend/llvm/X86/MedLLVMRegistrationSEHLocalUnwind.h"
#include "../../../lib/backend/llvm/X86/MedLLVMRegistrationSEHParentExit.h"
#include "../../../lib/backend/llvm/X86/MedLLVMRegistrationSEHProof.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/X86/X86RegistrationLocalUnwind.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace neverd;

namespace {

MedFunc localUnwindSource(bool EH4, bool Indirect) {
  MedFunc Source;
  Source.Entry = 0x1000;
  auto &EH = Source.ExceptionMetadata.emplace();
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Encoding = EH4 ? ExceptionEncoding::X86ScopeTableEH4
                    : ExceptionEncoding::X86ScopeTableEH3;
  EH.Personality = EH4 ? ExceptionPersonality::ExceptHandler4
                       : ExceptionPersonality::ExceptHandler3;
  EH.CodeRange = {0x1000, 0x1900};
  auto &Chain = EH.Registration.emplace();
  const int32_t Outside = EH4 ? -2 : -1;
  Chain.RegistrationOffset = -16;
  Chain.TryLevelOffset = -4;
  Chain.SeededTryLevel = Outside;
  Chain.Scopes = {{Outside, 0, 0x1800, true}};
  auto &States = Source.RegistrationStates.emplace();
  States.Complete = States.CallbackStatesComplete =
      States.RegistrationLifetimeComplete = States.SecurityCookiesComplete =
          States.HasSEHLocalUnwinds = true;
  States.SecurityCookieVA = 0x4000;
  constexpr va_t Addresses[] = {0x1000, 0x1800, 0x1100};
  for (int Id = 0; Id != 3; ++Id) {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Addresses[Id];
    Block.EndAddr = Addresses[Id] + 16;
    Source.Blocks.push_back(Block);
    RegistrationBlockState State;
    State.BlockId = Id;
    State.Range = {Block.StartAddr, Block.EndAddr};
    State.Reached = true;
    State.CallbackOnly = Id == 1;
    State.ExplicitLevels = {0};
    States.Blocks.push_back(State);
  }
  Source.Blocks[0].Succs = {2};
  auto &Callback = Source.Blocks[1];
  Callback.Succs = {2};
  RegistrationSEHLocalUnwind Receipt;
  Receipt.Address = 0x1800;
  Receipt.EndAddress = 0x1800 + (Indirect ? 6 : 5);
  Receipt.OpSeq = 0;
  Receipt.Runtime = {Indirect ? 0x5000u : 0x2000u, Indirect,
                     EH4 ? RegistrationLocalUnwindContract::Kind::EH4
                         : RegistrationLocalUnwindContract::Kind::EH3,
                     0x5000};
  Receipt.StackOffset = -52;
  Receipt.EntryVA = 0x1800;
  Receipt.Bound = uint32_t(Outside);
  Receipt.Transitions = {{0, Outside}};
  States.SEHLocalUnwinds.push_back(Receipt);
  MedOp Call;
  Call.Opcode = Indirect ? NdOp::INDIR_CALL : NdOp::CALL;
  Call.Addr = Receipt.Address;
  Call.OriginSeq = Receipt.OpSeq;
  Call.Output.Kind = MedVar::Reg;
  Call.Output.TheArch = Arch::X86;
  Call.Output.RegOff = x86reg::RAX;
  Call.Output.Id = 20;
  Call.Output.SSAVer = 1;
  Call.Output.Size = 4;
  Call.addInput(MedVar::makeConst(Receipt.Runtime.Target, 4));
  Callback.Ops.push_back(Call);
  MedOp Restore;
  Restore.Opcode = NdOp::LOAD;
  Restore.Addr = 0x1808;
  Restore.OriginSeq = 1;
  Restore.Output.Kind = MedVar::Reg;
  Restore.Output.TheArch = Arch::X86;
  Restore.Output.RegOff = x86reg::RSP;
  Restore.Output.Id = 50;
  Restore.Output.SSAVer = 1;
  Restore.Output.Size = 4;
  MedVar Address;
  Address.Size = 4;
  Restore.addInput(Address);
  Callback.Ops.push_back(Restore);
  MedOp Branch;
  Branch.Opcode = NdOp::BRANCH;
  Branch.Addr = 0x180b;
  Branch.OriginSeq = 0;
  Branch.addInput(MedVar::makeConst(0x1100, 4));
  Callback.Ops.push_back(Branch);
  States.FrameValues.push_back({Restore.Addr, Restore.OriginSeq, -64});
  RegistrationSEHInvocationExit Exit;
  Exit.Exit = RegistrationSEHInvocationExit::Kind::ParentJump;
  Exit.EntryVA = 0x1800;
  Exit.Address = Branch.Addr;
  Exit.EndAddress = Callback.EndAddr;
  Exit.OpSeq = Branch.OriginSeq;
  Exit.TargetVA = 0x1100;
  Exit.StackRestoreVA = Restore.Addr;
  Exit.StackRestoreSeq = Restore.OriginSeq;
  Exit.StackOffset = -64;
  States.SEHInvocationExits.push_back(Exit);
  return Source;
}

struct NativeLocalUnwind {
  llvm::LLVMContext Context;
  llvm::Module Module{"local-unwind-native", Context};
  MedFunc Source;
  llvm::Function *Parent = nullptr;
  llvm::Function *Runtime = nullptr;
  llvm::Function *Callback = nullptr;
  llvm::AllocaInst *Frame = nullptr;
  llvm::CallInst *Call = nullptr;
  std::map<coff_registration::CallbackKey, const llvm::Function *> Callbacks;
  std::string ErrorText;

  void create(bool EH4, bool Indirect) {
    Source = localUnwindSource(EH4, Indirect);
    ASSERT_TRUE(getX86RegistrationSEHLocalUnwinds(Source));
    Module.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
    Module.setDataLayout("e-m:x-p:32:32-i64:64-f80:32-n8:16:32-a:0:32-S32");
    Parent = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    auto *Personality = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getInt32Ty(Context), true),
        llvm::GlobalValue::ExternalLinkage,
        EH4 ? "_except_handler4" : "_except_handler3", Module);
    Parent->setPersonalityFn(Personality);
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Parent);
    auto *Body = llvm::BasicBlock::Create(Context, "body", Parent);
    auto *Finally = llvm::BasicBlock::Create(Context, "finally", Parent);
    auto *Resume = llvm::BasicBlock::Create(Context, "resume", Parent);
    llvm::IRBuilder<> B(Entry);
    Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 96));
    auto *Root = B.CreateAlloca(B.getInt32Ty());
    B.CreateBr(Body);
    B.SetInsertPoint(Body);
    B.CreateBr(Resume);
    B.SetInsertPoint(Resume);
    B.CreateRetVoid();
    const auto &Receipt = Source.RegistrationStates->SEHLocalUnwinds.front();
    Runtime = llvm::Function::Create(x86_registration::localUnwindRuntimeType(
                                         Context, Receipt.Runtime.Protocol),
                                     llvm::GlobalValue::ExternalLinkage,
                                     "runtime", Module);
    rewrite_source::setOriginalVA(*Runtime, Receipt.Runtime.Target);
    if (Indirect)
      Runtime->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
    B.SetInsertPoint(Finally);
    auto *Seed = B.CreateStore(B.getInt32(0), Root);
    llvm::SmallVector<llvm::Value *, 3> Arguments;
    if (EH4)
      Arguments.push_back(new llvm::GlobalVariable(
          Module, B.getInt32Ty(), false, llvm::GlobalValue::ExternalLinkage,
          nullptr, "__security_cookie"));
    Arguments.push_back(
        B.CreateInBoundsGEP(B.getInt8Ty(), Frame, B.getInt32(76)));
    Arguments.push_back(B.getInt32(Receipt.Bound));
    auto *Original = B.CreateCall(Runtime, Arguments);
    const auto MD = [&](uint64_t Value, unsigned Width) {
      return llvm::ConstantAsMetadata::get(B.getIntN(Width, Value));
    };
    Original->setMetadata(
        windows_eh_md::RegistrationOperationAttachment,
        llvm::MDNode::get(Context,
                          {MD(Source.Entry, 64), MD(Receipt.Address, 64),
                           MD(Receipt.OpSeq, 32), MD(1, 32), MD(2, 8)}));
    auto *SavedStack =
        B.CreateLoad(B.getInt32Ty(),
                     B.CreateInBoundsGEP(B.getInt8Ty(), Frame, B.getInt32(68)),
                     "saved.stack");
    SavedStack->setVolatile(true);
    B.CreateBr(Resume);
    const std::map<int, llvm::BasicBlock *> Blocks{
        {0, Body}, {1, Finally}, {2, Resume}};
    auto Exits = x86_registration::prepareSEHParentExits(Source, Blocks, {});
    ASSERT_TRUE(Exits);
    X86RegistrationCallbackRequest Request;
    Request.Entry = Finally;
    Request.Blocks = {Finally};
    Request.Kind = X86RegistrationCallbackKind::Finally;
    Request.Name = "finally.callback";
    Request.Frame.SyntheticFrame = Frame;
    Request.Frame.EstablishedFramePointerOffset = 92;
    const X86RegistrationRootSeed RootSeed{
        Seed, X86RegistrationRootKind::FramePointer};
    Request.Frame.RootSeeds = llvm::ArrayRef(RootSeed);
    const auto Plan = x86_registration::prepareSEHLocalUnwinds(
        Source, *Parent, *Frame, 96, *Exits, {{{0x1800, true}, 0}}, {Request},
        {{Original, Receipt.Address}}, {{{0x1808, 1}, SavedStack}});
    ASSERT_TRUE(Plan);
    x86_registration::SEHParentExitProjection Projection(*Parent, Source.Entry,
                                                         *Exits);
    for (const auto &[Id, Transfer] : Projection.calls()) {
      Request.NonReturningCalls.push_back(Transfer);
      Request.Blocks.push_back(Transfer->getParent());
    }
    std::vector<X86RegistrationCallbackInstructionMap> Instructions;
    auto Outlined =
        outlineX86RegistrationCallbacks(*Parent, {Request}, &Instructions);
    ASSERT_TRUE(bool(Outlined)) << llvm::toString(Outlined.takeError());
    Projection.commit();
    Callback = Outlined->front();
    llvm::appendToCompilerUsed(Module, {Callback});
    Projection.emitCapture(Blocks);
    x86_registration::emitSEHLocalUnwinds(*Plan, *Parent, *Frame, 96,
                                          *Projection.chain(), Source.Entry,
                                          *Outlined, Instructions);
    for (auto *Block : Request.Blocks)
      Block->dropAllReferences();
    for (auto *Block : Request.Blocks)
      Block->eraseFromParent();
    Callbacks.emplace(std::make_pair(0x1800, true), Callback);
    for (auto &Block : *Callback)
      for (auto &I : Block)
        if (I.getMetadata(windows_eh_md::RegistrationOperationAttachment))
          Call = llvm::dyn_cast<llvm::CallInst>(&I);
    ASSERT_TRUE(Call);
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  }

  bool valid() {
    auto Contract = coff_registration::bindSEHLocalUnwinds(
        Source, *Parent, {Frame, 96, 92}, Callbacks);
    if (!Contract) {
      ErrorText = llvm::toString(Contract.takeError());
      return false;
    }
    auto NativeLoads = coff_registration::collectSEHLocalUnwindChainLoads(
        Source.ExceptionMetadata->Personality ==
                ExceptionPersonality::ExceptHandler4
            ? RegistrationLocalUnwindContract::Kind::EH4
            : RegistrationLocalUnwindContract::Kind::EH3,
        Callbacks);
    if (!NativeLoads) {
      ErrorText = llvm::toString(NativeLoads.takeError());
      return false;
    }
    if (*NativeLoads != Contract->ChainLoads) {
      ErrorText = "source and native local unwind chain uses differ";
      return false;
    }
    auto ParentContract = coff_registration::bindSEHParentExitContext(
        *Parent, Source.Entry, Callbacks, Contract->ChainLoads);
    if (!ParentContract) {
      ErrorText = llvm::toString(ParentContract.takeError());
      return false;
    }
    return true;
  }
};

TEST(RegistrationLocalUnwindNative,
     BindsSourceFrameStateAndExceptionalRuntime) {
  for (bool EH4 : {false, true})
    for (bool Indirect : {false, true}) {
      SCOPED_TRACE(EH4);
      SCOPED_TRACE(Indirect);
      NativeLocalUnwind Test;
      Test.create(EH4, Indirect);
      ASSERT_FALSE(testing::Test::HasFatalFailure());
      EXPECT_TRUE(Test.valid()) << Test.ErrorText;
    }
}

TEST(RegistrationLocalUnwindNative, SavedParentStackRequiresExactBoundedSSA) {
  for (unsigned Mutation = 0; Mutation != 20; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Source = localUnwindSource(false, false);
    auto &Callback = Source.Blocks[1];
    auto &Definition = Callback.Ops[1];
    Definition.Output.Id = 50;
    Definition.Output.SSAVer = 1;
    MedOp Store;
    Store.Opcode = NdOp::STORE;
    Store.Addr = 0x1809;
    Store.OriginSeq = 0;
    Store.addInput(Definition.Output);
    Store.addInput(MedVar::makeConst(0, 4));
    auto &States = *Source.RegistrationStates;
    States.FrameValues.front().EstablishedFrameOffset = -48;
    switch (Mutation) {
    case 1:
      States.FrameValues.front().EstablishedFrameOffset = -92;
      break;
    case 2:
      States.FrameValues.front().EstablishedFrameOffset = 0;
      break;
    case 3:
      States.FrameValues.front().EstablishedFrameOffset.reset();
      break;
    case 4:
      States.FrameValues.front().EstablishedFrameOffset = -93;
      break;
    case 5:
      States.FrameValues.front().EstablishedFrameOffset = 1;
      break;
    case 6:
      Store.Inputs[0].Size = 2;
      break;
    case 7:
      ++Store.Inputs[0].SSAVer;
      break;
    case 8: {
      auto Duplicate = Definition;
      Duplicate.Addr = 0x1806;
      Callback.Ops.push_back(Duplicate);
      break;
    }
    case 9:
      Callback.Phis.emplace_back();
      Callback.Phis.back().Output = Definition.Output;
      break;
    case 10:
      Source.CallClobbers.emplace_back();
      Source.CallClobbers.back().Value = Definition.Output;
      break;
    case 11:
      Callback.Ops.front().IntrinsicOutputs.push_back(Definition.Output);
      break;
    case 12:
      Definition.Output.Size = 8;
      break;
    case 13:
      States.FrameValues.push_back(States.FrameValues.front());
      break;
    case 14: {
      auto Duplicate = Definition;
      ++Duplicate.Output.Id;
      Callback.Ops.push_back(Duplicate);
      break;
    }
    case 15:
      Definition.Dead = true;
      break;
    case 16:
      States.Complete = false;
      break;
    case 17:
      Store.MemoryOrdering = NdMemoryOrdering::Release;
      break;
    case 18:
      Store.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 19:
      Source.SkippedSSA = true;
      break;
    }
    Callback.Ops.push_back(Store);
    const auto Stores = x86_registration::getSEHFrameStores(Source, 96, 96);
    EXPECT_EQ(Stores && Stores->count(&Callback.Ops.back()), Mutation <= 2);
  }
}

TEST(RegistrationLocalUnwindNative,
     SavedStackReadsBindTheExactLoadAndCallback) {
  for (unsigned Mutation = 0; Mutation != 27; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Source = localUnwindSource(false, false);
    auto &Ops = Source.Blocks[1].Ops;
    if (Mutation == 1 || Mutation == 2) {
      auto Copy = Ops[1];
      Copy.Opcode = NdOp::COPY;
      Copy.Addr = 0x1809;
      Copy.Inputs[0] = Ops[1].Output;
      ++Copy.Output.Id;
      Ops.insert(Ops.begin() + 2, Copy);
      if (Mutation == 2)
        Ops[2].Inputs[0] = Ops[2].Output;
    }
    llvm::LLVMContext Context;
    llvm::Module Module("saved-stack", Context);
    auto *Parent = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    auto *Block = llvm::BasicBlock::Create(Context, "callback", Parent);
    llvm::IRBuilder<> B(Block);
    auto *Slot = B.CreateAlloca(B.getInt32Ty());
    auto *Load =
        B.CreateLoad(Mutation == 19 ? B.getInt64Ty() : B.getInt32Ty(), Slot);
    Load->setVolatile(true);
    B.CreateRetVoid();
    X86RegistrationCallbackRequest Request;
    Request.Entry = Block;
    Request.Blocks = {Block};
    uint64_t EntrySP = 96;
    switch (Mutation) {
    case 3:
      Ops[1].Dead = true;
      break;
    case 4:
      Ops[1].NumInputs = 0;
      break;
    case 5:
      Ops[1].Output.Size = 8;
      break;
    case 6:
      Ops[1].MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 7:
      Ops[1].MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 8:
      ++Ops[1].OriginSeq;
      break;
    case 9: {
      auto Duplicate = Ops[1];
      ++Duplicate.Addr;
      Ops.push_back(Duplicate);
      break;
    }
    case 10:
      Source.Blocks[1].Phis.emplace_back();
      Source.Blocks[1].Phis.back().Output = Ops[1].Output;
      break;
    case 11:
      Source.CallClobbers.emplace_back();
      Source.CallClobbers.back().Value = Ops[1].Output;
      break;
    case 12:
      Ops[0].IntrinsicOutputs.push_back(Ops[1].Output);
      break;
    case 13:
      Source.RegistrationStates->FrameValues.clear();
      break;
    case 14:
      Source.RegistrationStates->FrameValues.front().EstablishedFrameOffset =
          -60;
      break;
    case 15:
      Source.RegistrationStates->FrameValues.push_back(
          Source.RegistrationStates->FrameValues.front());
      break;
    case 16:
      Load->setVolatile(false);
      break;
    case 17:
      Load->setAtomic(llvm::AtomicOrdering::Acquire);
      break;
    case 18:
      Request.Blocks.clear();
      break;
    case 20:
      Source.SkippedSSA = true;
      break;
    case 21:
      Source.RegistrationStates->Complete = false;
      break;
    case 22:
      EntrySP = 3;
      break;
    case 23:
      EntrySP = uint64_t(UINT32_MAX) + 1;
      break;
    }
    x86_registration::SEHParentExitPlan Exits;
    auto &Exit = Exits.Sources[1];
    Exit.EntryVA = 0x1800;
    Exit.StackOffset = -64;
    Exit.Restore = &Ops[Mutation == 1 || Mutation == 2 ? 2 : 1];
    x86_registration::SEHLocalUnwindPlan Plan;
    auto &Call = Plan.Calls[nullptr];
    Call.Source.Source = &Source.RegistrationStates->SEHLocalUnwinds.front();
    Call.Callback = Mutation == 25 ? 1 : 0;
    if (Mutation == 26)
      Call.Source.Source = nullptr;
    std::map<std::pair<va_t, int>, llvm::Instruction *> Memory{
        {{0x1808, 1}, Load}};
    if (Mutation == 24)
      Memory.clear();
    const bool Valid = Mutation <= 1 || Mutation == 16;
    EXPECT_EQ(x86_registration::bindSEHLocalUnwindFrameReads(
                  Plan, Source, EntrySP, Exits, {Request}, Memory),
              Valid);
    if (Valid) {
      ASSERT_EQ(Plan.FrameReads.size(), 1u);
      EXPECT_EQ(Plan.FrameReads.at(Load).Offset, -64);
    }
  }
}

TEST(RegistrationLocalUnwindNative, RejectsAlteredInvocationAndParentCapture) {
  for (bool EH4 : {false, true})
    for (bool Indirect : {false, true})
      for (unsigned Mutation = 0; Mutation != 19; ++Mutation) {
        SCOPED_TRACE(EH4);
        SCOPED_TRACE(Indirect);
        SCOPED_TRACE(Mutation);
        NativeLocalUnwind Test;
        Test.create(EH4, Indirect);
        ASSERT_FALSE(testing::Test::HasFatalFailure());
        ASSERT_TRUE(Test.valid()) << Test.ErrorText;
        auto *Call = Test.Call;
        auto *Pointer = llvm::cast<llvm::IntToPtrInst>(Call->getArgOperand(0));
        auto *Head = llvm::cast<llvm::LoadInst>(Pointer->getOperand(0));
        auto *Slot = llvm::cast<llvm::GetElementPtrInst>(Call->getNextNode());
        auto *State = llvm::cast<llvm::StoreInst>(Slot->getNextNode());
        llvm::IRBuilder<> B(Call);
        switch (Mutation) {
        case 0:
          Test.Source.RegistrationStates->SEHLocalUnwinds.front().Bound = 0;
          break;
        case 1:
          rewrite_source::setOriginalVA(*Test.Runtime, 0x2200);
          break;
        case 2:
          Call->getCalledFunction()->addFnAttr(llvm::Attribute::NoUnwind);
          break;
        case 3:
          Head->setOperand(0, Slot->getPointerOperand());
          break;
        case 4:
          Head->setVolatile(false);
          break;
        case 5:
          Head->setAlignment(llvm::Align(1));
          break;
        case 6:
          B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
              &Test.Module, llvm::Intrinsic::sideeffect));
          break;
        case 7:
          State->setOperand(0, B.getInt32(0));
          break;
        case 8:
          Slot->setOperand(1, B.getInt32(92));
          break;
        case 9:
          State->setVolatile(false);
          break;
        case 10:
          Slot->setOperand(0, Head->getPointerOperand());
          break;
        case 11:
          Call->addFnAttr(llvm::Attribute::NoUnwind);
          break;
        case 12:
          Call->setMetadata(windows_eh_md::RegistrationOperationAttachment,
                            nullptr);
          break;
        case 13: {
          llvm::SmallVector<llvm::Value *> Arguments(Call->args());
          B.CreateCall(Call->getCalledFunction(), Arguments);
          break;
        }
        case 14:
          Call->setArgOperand(EH4 ? 1 : 0,
                              llvm::ConstantPointerNull::get(B.getPtrTy()));
          break;
        case 15: {
          auto *Duplicate = Call->clone();
          Duplicate->insertBefore(Call->getIterator());
          break;
        }
        case 16:
          Test.Source.RegistrationStates->SEHInvocationExits.clear();
          break;
        case 17:
          Slot->setOperand(1, B.getInt64((uint64_t(1) << 32) + 88));
          break;
        case 18:
          Slot->setNoWrapFlags(llvm::GEPNoWrapFlags::noUnsignedWrap());
          break;
        }
        EXPECT_FALSE(Test.valid()) << Test.ErrorText;
      }
}

} // namespace
