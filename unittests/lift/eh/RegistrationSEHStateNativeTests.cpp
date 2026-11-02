//===- RegistrationSEHStateNativeTests.cpp - State contracts -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks callback logical writes, physical state updates and retained scopes.

#include "../../../lib/backend/codegen/COFF/COFFRegistrationSEHState.h"
#include "../../../lib/backend/codegen/COFF/COFFRegistrationSEHStateWrites.h"
#include "RegistrationNativeEmitterTestPeer.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/backend/llvm/X86/X86RegistrationSEHJump.h"
#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/BinaryRewrite.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace neverd;

namespace {

struct StateFixture {
  llvm::LLVMContext Context;
  llvm::Module Module{"callback-state", Context};
  MedFunc Source;
  MedLLVMEmitter Emitter;
  llvm::Function *Parent;
  llvm::AllocaInst *Frame;
  std::map<int, llvm::BasicBlock *> Blocks;
  std::vector<llvm::StoreInst *> Stores;

  explicit StateFixture(bool EH4 = false) {
    Module.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
    Module.setDataLayout("e-m:x-p:32:32-i64:64-f80:32-n8:16:32-a:0:32-S32");
    llvm::IRBuilder<> B(Context);
    Parent = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt32Ty(), false),
        llvm::GlobalValue::ExternalLinkage, "guarded.state", Module);
    rewrite_source::setOriginalVA(*Parent, 0x1000);
    Source.Entry = 0x1000;
    Source.RegistrationCallerCleanupABIComplete = true;
    auto &EH = Source.ExceptionMetadata.emplace();
    EH.Kind = RuntimeFunctionKind::Primary;
    EH.ParseStatus = ExceptionParseStatus::Complete;
    EH.Encoding = EH4 ? ExceptionEncoding::X86ScopeTableEH4
                      : ExceptionEncoding::X86ScopeTableEH3;
    EH.Personality = EH4 ? ExceptionPersonality::ExceptHandler4
                         : ExceptionPersonality::ExceptHandler3;
    EH.PersonalityVA = 0x2000;
    EH.HandlerDataVA = 0x3000;
    EH.CodeRange = {0x1000, 0x1041};
    const int32_t Sentinel = EH4 ? -2 : -1;
    auto &Chain = EH.Registration.emplace();
    Chain.HandlerVA = EH.PersonalityVA;
    Chain.ScopeTableVA = EH.HandlerDataVA;
    Chain.RegistrationOffset = -16;
    Chain.TryLevelOffset = -4;
    Chain.SeededTryLevel = Sentinel;
    Chain.ChainInstallVA = 0x1002;
    Chain.ChainRemoveVA = 0x1032;
    // Only scope one is active in ordinary parent code. Scope zero is selected
    // by a callback and therefore needs a compiler retention edge.
    Chain.Scopes = {{Sentinel, 0, 0x1040, true}, {Sentinel, 0, 0x1020, true}};
    Chain.TryLevelStores = {{0x1004, 0x1010, 1},
                            {0x1014, 0x1015, Sentinel},
                            {0x1020, 0x1025, 0},
                            {0x1025, 0x102a, Sentinel}};
    auto &States = Source.RegistrationStates.emplace();
    States.Complete = States.CallbackStatesComplete = true;
    States.RegistrationLifetimeComplete = States.ChainOperationsComplete = true;
    States.ChainAccesses = {
        {0x1001, 0x1002, 1, RegistrationChainAccess::Kind::ReadPreviousHead},
        {0x1002, 0x1003, 2, RegistrationChainAccess::Kind::Install},
        {0x1032, 0x1033, 3, RegistrationChainAccess::Kind::Remove}};
    if (EH4) {
      Chain.GSCookieOffset = -2;
      Chain.EHCookieOffset = -20;
      States.SecurityCookiesComplete = true;
      States.SecurityCookieVA = 0x2500;
    }
    const va_t Begins[] = {0x1000, 0x1010, 0x1020, 0x1030,
                           0x1025, 0x102a, 0x1040};
    const va_t Ends[] = {0x1010, 0x1020, 0x1025, 0x1040,
                         0x102a, 0x1030, 0x1041};
    const int32_t Levels[] = {Sentinel, 1,        Sentinel, Sentinel,
                              0,        Sentinel, Sentinel};
    for (int I = 0; I != 7; ++I) {
      MedBlock Block;
      Block.Id = I;
      Block.StartAddr = Begins[I];
      Block.EndAddr = Ends[I];
      Source.Blocks.push_back(Block);
      const bool Callback = I == 2 || I >= 4;
      States.Blocks.push_back(
          {I, {Begins[I], Ends[I]}, {Levels[I]}, false, Callback, true});
      Blocks.emplace(I, llvm::BasicBlock::Create(Context, "source", Parent));
    }
    Source.Blocks[0].Succs = {1};
    Source.Blocks[1].Succs = {3};
    Source.Blocks[2].Succs = {4};
    Source.Blocks[4].Succs = {5};
    auto *Setup =
        llvm::BasicBlock::Create(Context, "frame_setup", Parent, Blocks.at(0));
    B.SetInsertPoint(Setup);
    Frame = B.CreateAlloca(B.getInt8Ty(), B.getInt32(128), "frame");
    B.CreateBr(Blocks.at(0));
    B.SetInsertPoint(Blocks.at(0));
    auto *FS = llvm::ConstantPointerNull::get(B.getPtrTy(257));
    auto *Previous = B.CreateLoad(B.getInt32Ty(), FS);
    auto *Install = B.CreateStore(B.getInt32(0), FS);
    B.CreateBr(Blocks.at(1));
    MedLLVMEmitterTestPeer::prepare(Emitter, Module, *Parent, Source, *Frame);
    MedLLVMEmitterTestPeer::chain(Emitter, *Previous, 0x1001, 1);
    MedLLVMEmitterTestPeer::chain(Emitter, *Install, 0x1002, 2);
    B.SetInsertPoint(Blocks.at(1));
    auto *Call =
        B.CreateCall(Module.getOrInsertFunction("raise", B.getVoidTy()));
    MedLLVMEmitterTestPeer::call(Emitter, *Call, 0x1011);
    MedOp SourceCall;
    SourceCall.Opcode = NdOp::CALL;
    SourceCall.Addr = 0x1011;
    SourceCall.OriginSeq = 0;
    SourceCall.NumInputs = 1;
    SourceCall.Inputs[0] = MedVar::makeConst(0x4000, 4);
    Source.Blocks[1].Ops.push_back(SourceCall);
    B.CreateBr(Blocks.at(3));
    for (int Id : {2, 4}) {
      auto &Block = Source.Blocks[Id];
      const uint32_t Value = Id == 2 ? 0 : uint32_t(Sentinel);
      MedOp Address;
      Address.Opcode = NdOp::INT_ADD;
      Address.Addr = Block.StartAddr;
      Address.OriginSeq = 0;
      Address.Output.Kind = MedVar::Temp;
      Address.Output.Id = 100 + Id;
      Address.Output.SSAVer = 1;
      Address.Output.Size = 4;
      Address.NumInputs = 2;
      Address.Inputs[0].Kind = MedVar::Reg;
      Address.Inputs[0].Id = 1;
      Address.Inputs[0].SSAVer = 1;
      Address.Inputs[0].Size = 4;
      Address.Inputs[0].RegOff = x86reg::RBP;
      Address.Inputs[1] = MedVar::makeConst(uint32_t(-4), 4);
      Block.Ops.push_back(Address);
      States.FrameValues.push_back({Address.Addr, 0, -4});
      MedOp Store;
      Store.Opcode = NdOp::STORE;
      Store.Addr = Block.StartAddr;
      Store.OriginSeq = 1;
      Store.NumInputs = 2;
      Store.Inputs[0] = Address.Output;
      Store.Inputs[1] = MedVar::makeConst(Value, 4);
      Block.Ops.push_back(Store);
      States.SEHStateStores.push_back(
          {Id,
           Block.StartAddr,
           Block.EndAddr,
           1,
           4,
           Value,
           {{Id == 2 ? Sentinel : 0, int32_t(Value)}}});
      B.SetInsertPoint(Blocks.at(Id));
      auto *Pointer = B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(56));
      auto *Write = B.CreateStore(B.getInt32(Value), Pointer);
      Write->setAlignment(llvm::Align(1));
      MedLLVMEmitterTestPeer::memory(Emitter, *Write, Block.StartAddr, 1);
      Stores.push_back(Write);
      B.CreateBr(Blocks.at(Id == 2 ? 4 : 5));
    }
    for (int Id : {5, 6}) {
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.Addr = Source.Blocks[Id].StartAddr;
      Source.Blocks[Id].Ops.push_back(Return);
      B.SetInsertPoint(Blocks.at(Id));
      B.CreateRet(B.getInt32(0));
    }
    B.SetInsertPoint(Blocks.at(3));
    auto *Remove = B.CreateStore(B.getInt32(0), FS);
    MedLLVMEmitterTestPeer::chain(Emitter, *Remove, 0x1032, 3);
    B.CreateRet(B.getInt32(7));
  }

  bool lower() {
    return MedLLVMEmitterTestPeer::lower(Emitter, Source, *Parent, Blocks);
  }

  std::string print() const {
    std::string Text;
    llvm::raw_string_ostream Stream(Text);
    Module.print(Stream, nullptr);
    return Text;
  }
};

TEST(RegistrationSEHStateNative,
     CallbackWritesUseSemanticTokensAndPhysicalFrame) {
  for (bool EH4 : {false, true}) {
    SCOPED_TRACE(EH4);
    StateFixture F(EH4);
#ifndef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES
    const auto Before = F.print();
    EXPECT_FALSE(F.lower());
    EXPECT_EQ(F.print(), Before);
#else
    ASSERT_TRUE(F.lower()) << F.print();
    EXPECT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
    unsigned Writes = 0, Retained = 0;
    const auto Token = windows_eh_semantics::getSEHScopeSemanticToken(
        *F.Source.ExceptionMetadata, Arch::X86, 0);
    ASSERT_TRUE(Token);
    for (auto &Function : F.Module)
      for (auto &Block : Function)
        for (auto &Instruction : Block) {
          auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
          if (Call &&
              Call->getIntrinsicID() == llvm::Intrinsic::x86_seh_setstate) {
            ++Writes;
            EXPECT_NE(&Function, F.Parent);
            ASSERT_EQ(Call->arg_size(), 8u);
            EXPECT_EQ(Call->getArgOperand(0), F.Parent);
            auto *Store =
                llvm::dyn_cast_or_null<llvm::StoreInst>(Call->getPrevNode());
            ASSERT_TRUE(Store);
            EXPECT_TRUE(Store->isVolatile());
            EXPECT_EQ(Store->getValueOperand(), Call->getArgOperand(2));
            const auto Level =
                llvm::cast<llvm::ConstantInt>(Call->getArgOperand(2))
                    ->getSExtValue();
            for (unsigned I = 0; I != 4; ++I)
              EXPECT_EQ(
                  llvm::cast<llvm::ConstantInt>(Call->getArgOperand(I + 3))
                      ->getZExtValue(),
                  Level >= 0 ? Token->Digest[I] : 0u);
            EXPECT_EQ(llvm::cast<llvm::ConstantInt>(Call->getArgOperand(7))
                          ->getZExtValue(),
                      Level >= 0 ? 1u : 2u);
          }
          if (Instruction.getMetadata(
                  windows_eh_md::RegistrationScopeRetentionAttachment)) {
            ++Retained;
            auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(&Instruction);
            ASSERT_TRUE(Invoke);
            EXPECT_EQ(Invoke->getFunction(), F.Parent);
            EXPECT_EQ(Invoke->getIntrinsicID(), llvm::Intrinsic::donothing);
          }
        }
    EXPECT_EQ(Writes, 2u);
    EXPECT_EQ(Retained, 1u);
#endif
  }
}

TEST(RegistrationSEHStateNative, RejectsUnboundCallbackStateStoresAtomically) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    StateFixture F;
    auto &States = *F.Source.RegistrationStates;
    auto &Receipt = States.SEHStateStores.front();
    switch (Mutation) {
    case 0:
      States.SEHStateStoresComplete = false;
      break;
    case 1:
      ++Receipt.OpSeq;
      break;
    case 2:
      ++Receipt.EndAddress;
      break;
    case 3:
      Receipt.Value = 1;
      break;
    case 4:
      Receipt.Transitions.front().second = 1;
      break;
    case 5:
      Receipt.Width = 1;
      break;
    case 6:
      Receipt.Transitions.clear();
      break;
    case 7:
      F.Source.Blocks[2].Ops.back().Inputs[1] = MedVar::makeConst(1, 4);
      break;
    case 8:
      F.Stores.front()->setOperand(
          0, llvm::ConstantInt::get(llvm::Type::getInt32Ty(F.Context), 1));
      break;
    case 9:
      F.Stores.front()->setAtomic(llvm::AtomicOrdering::Monotonic);
      break;
    }
    const auto Before = F.print();
    EXPECT_FALSE(F.lower());
    EXPECT_EQ(F.print(), Before);
  }
}

#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES

TEST(RegistrationSEHStateNative, RejectsConflictingModuleContractsAtomically) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    StateFixture F;
    llvm::IRBuilder<> B(F.Context);
    if (Mutation == 0) {
      new llvm::GlobalVariable(F.Module, B.getInt32Ty(), false,
                               llvm::GlobalValue::ExternalLinkage, nullptr,
                               "llvm.donothing");
    } else if (Mutation == 1) {
      llvm::Function::Create(llvm::FunctionType::get(B.getInt32Ty(), false),
                             llvm::GlobalValue::ExternalLinkage,
                             "llvm.donothing", F.Module);
    } else if (Mutation == 2 || Mutation == 3) {
      llvm::Intrinsic::ID ID = llvm::Intrinsic::donothing;
      if (Mutation == 3)
        ID = llvm::Intrinsic::x86_seh_setstate;
      auto *Function = llvm::Intrinsic::getOrInsertDeclaration(&F.Module, ID);
      auto *Body = llvm::BasicBlock::Create(F.Context, "body", Function);
      B.SetInsertPoint(Body);
      B.CreateRetVoid();
    } else {
      auto *ArrayType = llvm::ArrayType::get(B.getPtrTy(), 1);
      auto *Array = llvm::ConstantArray::get(ArrayType, {F.Parent});
      auto *Used = new llvm::GlobalVariable(F.Module, Array->getType(), false,
                                            llvm::GlobalValue::AppendingLinkage,
                                            Array, "llvm.compiler.used");
      Used->setSection("llvm.metadata");
      if (Mutation == 4)
        Used->setLinkage(llvm::GlobalValue::InternalLinkage);
      else if (Mutation == 5)
        Used->setSection(".data");
      else if (Mutation == 6)
        Used->setInitializer(llvm::ConstantArray::get(
            ArrayType, {llvm::ConstantPointerNull::get(B.getPtrTy())}));
      else
        Used->setThreadLocal(true);
    }
    const auto Before = F.print();
    EXPECT_FALSE(F.lower());
    EXPECT_EQ(F.print(), Before);
  }
}

TEST(RegistrationSEHStateNative, PreservesExistingCompilerUsedOwners) {
  StateFixture F;
  auto *Existing = new llvm::GlobalVariable(
      F.Module, llvm::Type::getInt32Ty(F.Context), false,
      llvm::GlobalValue::InternalLinkage,
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(F.Context), 0), "kept");
  llvm::appendToCompilerUsed(F.Module, {Existing});
  ASSERT_TRUE(F.lower()) << F.print();
  EXPECT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
  auto Callbacks =
      coff_registration::callbacks(*F.Parent, *F.Source.ExceptionMetadata);
  ASSERT_TRUE(bool(Callbacks)) << llvm::toString(Callbacks.takeError());
  const auto *Used = F.Module.getNamedGlobal("llvm.compiler.used");
  ASSERT_TRUE(Used);
  const auto *Array =
      llvm::dyn_cast<llvm::ConstantArray>(Used->getInitializer());
  ASSERT_TRUE(Array);
  std::set<const llvm::Value *> Owners;
  for (const auto &Operand : Array->operands())
    Owners.insert(Operand->stripPointerCasts());
  EXPECT_TRUE(Owners.count(Existing));
  for (const auto &[Key, Callback] : *Callbacks)
    EXPECT_TRUE(Owners.count(Callback));
}

CompiledImage compileStateFixture(StateFixture &F) {
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

TEST(RegistrationSEHStateNative, InstallerBindsFinalStateWriteBytesAndOwners) {
  for (bool EH4 : {false, true}) {
    SCOPED_TRACE(EH4);
    StateFixture F(EH4);
    ASSERT_TRUE(F.lower()) << F.print();
    ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
    const auto Compiled = compileStateFixture(F);
    ASSERT_TRUE(Compiled.Success);
    ASSERT_EQ(Compiled.WinX86SEHStateWrites.size(), 2u);
    auto Rows = validateCOFFRegistrationSemanticRows(
        *F.Parent, *F.Source.ExceptionMetadata, Compiled);
    ASSERT_FALSE(bool(Rows)) << llvm::toString(std::move(Rows));
    auto Valid = validateCOFFRegistrationSEHStateWrites(
        *F.Parent, *F.Source.ExceptionMetadata, Compiled);
    ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
    for (unsigned Mutation = 0; Mutation != 16; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto Changed = Compiled;
      auto &Write = Changed.WinX86SEHStateWrites.front();
      const size_t Begin = Write.BeginVA - Changed.BaseVA;
      const size_t End = Write.EndVA - Changed.BaseVA;
      ASSERT_GE(End - Begin, 7u);
      ASSERT_LE(End, Changed.Bytes.size());
      switch (Mutation) {
      case 0:
        Write.AddressRegister ^= 1;
        break;
      case 1: {
        // Change only the machine base, preserving a valid C7 /0 encoding.
        auto &ModRM = Changed.Bytes[Begin + 1];
        if ((ModRM & 7) == 4)
          Changed.Bytes[Begin + 2] ^= 1;
        else
          ModRM = (ModRM & 0xf8) | ((ModRM & 7) == 0 ? 1 : 0);
        break;
      }
      case 2:
        Changed.Bytes[End - 4] ^= 1;
        break;
      case 3:
        // The registration state cell has a nonzero frame displacement.
        Changed.Bytes[Begin + ((Changed.Bytes[Begin + 1] & 7) == 4 ? 3 : 2)] ^=
            4;
        break;
      case 4:
        Changed.WinX86SEHStateWrites.erase(
            Changed.WinX86SEHStateWrites.begin());
        break;
      case 5:
        Write.WriteID = 0;
        break;
      case 6:
        Write.WriteID += 100;
        break;
      case 7: {
        auto Extra = Write;
        Extra.WriteID += 100;
        Changed.WinX86SEHStateWrites.push_back(std::move(Extra));
        break;
      }
      case 8:
        Write.SourceFunction = F.Parent->getName().str();
        break;
      case 9:
        Write.OwnerVA = Write.ParentFrame.OwnerVA;
        break;
      case 10:
        Write.ParentFrame.SourceFunction = Write.SourceFunction;
        break;
      case 11:
        ++Write.ParentFrame.OwnerVA;
        break;
      case 12:
        ++Write.EndVA;
        break;
      case 13:
        ++Write.BeginVA;
        break;
      case 14:
        std::swap(Write.BeginVA, Changed.WinX86SEHStateWrites[1].BeginVA);
        std::swap(Write.EndVA, Changed.WinX86SEHStateWrites[1].EndVA);
        break;
      case 15:
        ++Write.GeneratedState;
        break;
      }
      auto Failure = validateCOFFRegistrationSEHStateWrites(
          *F.Parent, *F.Source.ExceptionMetadata, Changed);
      ASSERT_TRUE(bool(Failure));
      llvm::consumeError(std::move(Failure));
    }
  }
}

TEST(RegistrationSEHStateNative, NarrowLogicalStoresUseFullPhysicalStates) {
  for (bool EH4 : {false, true})
    for (uint8_t Width : {1, 2}) {
      SCOPED_TRACE(EH4);
      SCOPED_TRACE(Width);
      StateFixture F(EH4);
      auto &Chain = *F.Source.ExceptionMetadata->Registration;
      auto &States = *F.Source.RegistrationStates;
      Chain.Scopes[1].EnclosingLevel = 0;
      States.Blocks[2].ExplicitLevels = {0};
      Chain.TryLevelStores[2].Width = Width;
      States.SEHStateStores[0].Width = Width;
      States.SEHStateStores[0].Transitions = {{0, 0}};
      F.Source.Blocks[2].Ops[1].Inputs[1].Size = Width;
      F.Stores[0]->setOperand(
          0, llvm::ConstantInt::get(
                 llvm::IntegerType::get(F.Context, Width * 8), 0));
      ASSERT_TRUE(F.lower()) << F.print();
      ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
      auto Callbacks =
          coff_registration::callbacks(*F.Parent, *F.Source.ExceptionMetadata);
      ASSERT_TRUE(bool(Callbacks)) << llvm::toString(Callbacks.takeError());
      std::vector<const llvm::Function *> Functions{F.Parent};
      for (const auto &[Key, Function] : *Callbacks)
        Functions.push_back(Function);
      auto Segments = coff_registration::validateSourceSegments(
          *F.Parent, F.Source, *Callbacks, Functions);
      ASSERT_TRUE(bool(Segments)) << llvm::toString(Segments.takeError());
      auto Writes = coff_registration::bindSEHCallbackStateWrites(
          F.Source, *F.Parent, *Segments, Functions);
      ASSERT_TRUE(bool(Writes)) << llvm::toString(Writes.takeError());
      ASSERT_EQ(Writes->Writes.size(), 2u);
      const auto Compiled = compileStateFixture(F);
      ASSERT_TRUE(Compiled.Success);
      ASSERT_EQ(Compiled.WinX86SEHStateWrites.size(), 2u);
      auto Valid = validateCOFFRegistrationSEHStateWrites(
          *F.Parent, *F.Source.ExceptionMetadata, Compiled);
      EXPECT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
    }
}

TEST(RegistrationSEHStateNative, NarrowSentinelDoesNotUseItsByteLiteral) {
  for (uint8_t Width : {1, 2}) {
    SCOPED_TRACE(Width);
    StateFixture F;
    auto &Chain = *F.Source.ExceptionMetadata->Registration;
    auto &States = *F.Source.RegistrationStates;
    Chain.Scopes[1].EnclosingLevel = 0;
    States.Blocks[2].ExplicitLevels = {0};
    States.Blocks[4].ExplicitLevels = {-1};
    Chain.TryLevelStores[2].Level = -1;
    States.SEHStateStores[0].Value = UINT32_MAX;
    States.SEHStateStores[0].Transitions = {{0, -1}};
    F.Source.Blocks[2].Ops[1].Inputs[1] = MedVar::makeConst(UINT32_MAX, 4);
    F.Stores[0]->setOperand(
        0,
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(F.Context), UINT32_MAX));
    const uint32_t Mask = Width == 1 ? UINT8_MAX : UINT16_MAX;
    Chain.TryLevelStores[3].Level = Mask;
    Chain.TryLevelStores[3].Width = Width;
    States.SEHStateStores[1].Value = Mask;
    States.SEHStateStores[1].Width = Width;
    States.SEHStateStores[1].Transitions = {{-1, -1}};
    F.Source.Blocks[4].Ops[1].Inputs[1] = MedVar::makeConst(Mask, Width);
    F.Stores[1]->setOperand(
        0, llvm::ConstantInt::get(llvm::IntegerType::get(F.Context, Width * 8),
                                  Mask));
    ASSERT_TRUE(F.lower()) << F.print();
    ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
    const auto Compiled = compileStateFixture(F);
    ASSERT_TRUE(Compiled.Success);
    ASSERT_EQ(Compiled.WinX86SEHStateWrites.size(), 2u);
    for (const auto &Write : Compiled.WinX86SEHStateWrites) {
      EXPECT_EQ(Write.GeneratedState, -1);
      EXPECT_FALSE(Write.Semantic);
    }
    auto Valid = validateCOFFRegistrationSEHStateWrites(
        *F.Parent, *F.Source.ExceptionMetadata, Compiled);
    EXPECT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  }
}

TEST(RegistrationSEHStateNative,
     InstallerBindsEachPhysicalWriteToItsSourceOwner) {
  for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    StateFixture F(Mutation & 1);
    ASSERT_TRUE(F.lower()) << F.print();
    auto Callbacks =
        coff_registration::callbacks(*F.Parent, *F.Source.ExceptionMetadata);
    ASSERT_TRUE(bool(Callbacks)) << llvm::toString(Callbacks.takeError());
    std::vector<const llvm::Function *> Functions{F.Parent};
    for (const auto &[Key, Function] : *Callbacks)
      Functions.push_back(Function);
    auto Segments = coff_registration::validateSourceSegments(
        *F.Parent, F.Source, *Callbacks, Functions);
    ASSERT_TRUE(bool(Segments)) << llvm::toString(Segments.takeError());
    auto Original = coff_registration::bindSEHCallbackStateWrites(
        F.Source, *F.Parent, *Segments, Functions);
    ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
    ASSERT_EQ(Original->Writes.size(), 2u);
    llvm::CallInst *Write = nullptr;
    for (const auto *Call : Original->Writes)
      if (llvm::cast<llvm::ConstantInt>(Call->getArgOperand(2))->isZero())
        Write = const_cast<llvm::CallInst *>(llvm::cast<llvm::CallInst>(Call));
    ASSERT_TRUE(Write);
    auto *Store = llvm::cast<llvm::StoreInst>(Write->getPrevNode());
    llvm::IRBuilder<> B(Write);
    switch (Mutation) {
    case 0: {
      auto *Other = llvm::Function::Create(F.Parent->getFunctionType(),
                                           llvm::GlobalValue::ExternalLinkage,
                                           "other.parent", F.Module);
      Write->setArgOperand(0, Other);
      break;
    }
    case 1:
      Write->setArgOperand(1, llvm::ConstantPointerNull::get(B.getPtrTy()));
      break;
    case 2:
      Write->setArgOperand(2, B.getInt32(1));
      break;
    case 3:
      Write->setArgOperand(
          3, B.getInt64(llvm::cast<llvm::ConstantInt>(Write->getArgOperand(3))
                            ->getZExtValue() ^
                        1));
      break;
    case 4:
      Write->setArgOperand(7, B.getInt64(0));
      break;
    case 5:
      Write->clone()->insertBefore(Write->getNextNode()->getIterator());
      break;
    case 6:
      Write->eraseFromParent();
      break;
    case 7:
      // A valid LLVM side effect cannot be inserted between the logical
      // source store and its physical state update.
      B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
          &F.Module, llvm::Intrinsic::sideeffect));
      break;
    case 8:
      Store->setOperand(0, B.getInt32(1));
      break;
    case 9:
      Store->setVolatile(false);
      break;
    case 10:
      Write->setArgOperand(
          1,
          B.CreateGEP(B.getInt8Ty(), Write->getArgOperand(1), B.getInt32(4)));
      break;
    case 11: {
      // Move both instructions to another real callback, repairing every SSA
      // reference with that callback's valid recovered parent and frame.
      auto *Destination =
          const_cast<llvm::Function *>(Callbacks->at({0x1040, true}));
      auto *DestinationFrame = const_cast<llvm::Value *>(
          getX86RegistrationSEHJumpParentFrame(*Destination, *F.Parent));
      ASSERT_TRUE(DestinationFrame);
      llvm::IRBuilder<> Other(Destination->back().getTerminator());
      auto *Recovered =
          Other.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                               &F.Module, llvm::Intrinsic::localrecover),
                           {F.Parent, DestinationFrame, Other.getInt32(0)});
      auto *Pointer =
          Other.CreateGEP(Other.getInt8Ty(), Recovered, Other.getInt32(56));
      Store->moveBefore(Destination->back().getTerminator()->getIterator());
      Store->setOperand(1, Pointer);
      Write->moveBefore(Destination->back().getTerminator()->getIterator());
      Write->setArgOperand(1, DestinationFrame);
      break;
    }
    case 12:
      F.Source.Blocks[2].Ops.back().Inputs[1] = MedVar::makeConst(1, 4);
      break;
    }
    ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs())) << F.print();
    auto Failure = coff_registration::bindSEHCallbackStateWrites(
        F.Source, *F.Parent, *Segments, Functions);
    ASSERT_FALSE(bool(Failure));
    llvm::consumeError(Failure.takeError());
  }
}

TEST(RegistrationSEHStateNative, InstallerClosesTheExactRetentionEntryChain) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    StateFixture F(Mutation & 1);
    ASSERT_TRUE(F.lower()) << F.print();
    auto Callbacks =
        coff_registration::callbacks(*F.Parent, *F.Source.ExceptionMetadata);
    ASSERT_TRUE(bool(Callbacks)) << llvm::toString(Callbacks.takeError());
    std::vector<const llvm::Function *> Functions{F.Parent};
    for (const auto &[Key, Function] : *Callbacks)
      Functions.push_back(Function);
    auto Segments = coff_registration::validateSourceSegments(
        *F.Parent, F.Source, *Callbacks, Functions);
    ASSERT_TRUE(bool(Segments)) << llvm::toString(Segments.takeError());
    const auto *SourceEntry = Segments->at(0).Enter->getParent();
    std::map<uint32_t, const llvm::BasicBlock *> Unwinds;
    for (const auto &Block : *F.Parent)
      if (const auto *Pad = llvm::dyn_cast<llvm::CleanupPadInst>(
              &*Block.getFirstNonPHIIt())) {
        const auto *Token =
            Pad->getMetadata(llvm::mc_rewrite::RewriteWinEHSemanticAttachment);
        ASSERT_TRUE(Token);
        const auto Region = coff_registration::metadataInteger(*Token, 2, 32);
        ASSERT_TRUE(Region);
        Unwinds.emplace(uint32_t(*Region), &Block);
      }
    ASSERT_EQ(Unwinds.size(), 2u);
    auto Original = coff_registration::validateSEHRetainedScopes(
        F.Source, *F.Parent, *SourceEntry, Unwinds);
    ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
    ASSERT_EQ(Original->size(), 1u);
    auto *Invoke = const_cast<llvm::InvokeInst *>(*Original->begin());
    llvm::IRBuilder<> B(Invoke);
    switch (Mutation) {
    case 0:
    case 1:
      Invoke->setMetadata(
          windows_eh_md::RegistrationScopeRetentionAttachment,
          llvm::MDNode::get(
              F.Context,
              {llvm::ConstantAsMetadata::get(
                   B.getInt64(Mutation ? 0x1000 : 0x2000)),
               llvm::ConstantAsMetadata::get(B.getInt32(Mutation ? 1 : 0))}));
      break;
    case 2:
      Invoke->setUnwindDest(const_cast<llvm::BasicBlock *>(Unwinds.at(1)));
      break;
    case 3:
      Invoke->setNormalDest(F.Blocks.at(3));
      break;
    case 4:
      B.CreateBr(Invoke->getNormalDest());
      Invoke->eraseFromParent();
      break;
    case 5:
    case 6: {
      auto *Duplicate =
          llvm::BasicBlock::Create(F.Context, "extra.retention", F.Parent);
      auto *Copy = llvm::cast<llvm::InvokeInst>(Invoke->clone());
      Copy->insertInto(Duplicate, Duplicate->end());
      if (Mutation == 5) {
        Copy->setNormalDest(Invoke->getParent());
        llvm::cast<llvm::UncondBrInst>(
            F.Parent->getEntryBlock().getTerminator())
            ->setSuccessor(0, Duplicate);
      }
      break;
    }
    case 7:
      B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
          &F.Module, llvm::Intrinsic::sideeffect));
      break;
    case 8:
      Invoke->setMetadata(windows_eh_md::RegistrationScopeRetentionAttachment,
                          nullptr);
      break;
    }
    ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs())) << F.print();
    auto Failure = coff_registration::validateSEHRetainedScopes(
        F.Source, *F.Parent, *SourceEntry, Unwinds);
    ASSERT_FALSE(bool(Failure));
    llvm::consumeError(Failure.takeError());
  }
}

#endif // LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES

} // namespace
