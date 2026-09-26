//===- LLVMCFrameMemoryTests.cpp - Frame memory preservation tests
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/pass/LLVMC/LLVMCPasses.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {
using namespace neverd;

struct FrameFixture {
  llvm::LLVMContext Context;
  llvm::Module Module{"frame-memory", Context};
  llvm::IRBuilder<> B{Context};
  llvm::Function *Fn;
  llvm::AllocaInst *Frame;
  llvm::Value *Base;

  FrameFixture() {
    Module.setDataLayout("e-p:64:64");
    Fn = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt32Ty(), {B.getInt1Ty()}, false),
        llvm::GlobalValue::ExternalLinkage, "frame_memory", Module);
    B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Fn));
    Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 152), nullptr,
                           "frame");
    Base = B.CreatePtrToInt(B.CreateGEP(B.getInt8Ty(), Frame, B.getInt64(72)),
                            B.getInt64Ty(), "rsp_init");
  }

  llvm::Value *address(llvm::Value *Origin, int64_t Offset) {
    return B.CreateIntToPtr(B.CreateAdd(Origin, B.getInt64(Offset)),
                            B.getPtrTy());
  }
};

TEST(LLVMCFrameMemory, PhiReadsKeepBothBranchStores) {
  FrameFixture F;
  auto *Normal = llvm::BasicBlock::Create(F.Context, "normal", F.Fn);
  auto *Handler = llvm::BasicBlock::Create(F.Context, "handler", F.Fn);
  auto *Join = llvm::BasicBlock::Create(F.Context, "join", F.Fn);
  auto *Adjusted = F.B.CreateSub(F.Base, F.B.getInt64(56));
  F.B.CreateCondBr(F.Fn->getArg(0), Normal, Handler);
  F.B.SetInsertPoint(Normal);
  auto *NormalStore =
      F.B.CreateStore(F.B.getInt32(-100), F.address(Adjusted, 32));
  F.B.CreateBr(Join);
  F.B.SetInsertPoint(Handler);
  auto *HandlerStore = F.B.CreateStore(F.B.getInt32(41), F.address(F.Base, 32));
  F.B.CreateBr(Join);
  F.B.SetInsertPoint(Join);
  auto *Address = F.B.CreatePHI(F.B.getInt64Ty(), 2);
  Address->addIncoming(Adjusted, Normal);
  Address->addIncoming(F.Base, Handler);
  auto *Load = F.B.CreateLoad(F.B.getInt32Ty(), F.address(Address, 32));
  F.B.CreateRet(Load);

  LLVMCAnalysisState State;
  analyzeDeadFrameStores(State, *F.Fn);
  EXPECT_EQ(State.DeadFrameStores.count(NormalStore), 0u);
  EXPECT_EQ(State.DeadFrameStores.count(HandlerStore), 0u);
  EXPECT_EQ(State.RawFrameLocations.count({F.Frame, 48}), 1u);
  EXPECT_EQ(State.RawFrameLocations.count({F.Frame, 104}), 1u);
  EXPECT_EQ(State.FramePointerLocations.count(Load->getPointerOperand()), 0u);
  analyzeStoreForwarding(State, *F.Fn);
  EXPECT_EQ(State.DeadFrameAllocas.count(F.Frame), 0u);
  EXPECT_EQ(State.DeadFrameStores.count(NormalStore), 0u);
  EXPECT_EQ(State.DeadFrameStores.count(HandlerStore), 0u);
  EXPECT_EQ(State.ForwardedLoads.count(Load), 0u);
}

TEST(LLVMCFrameMemory, EqualPhiAddressesShareTheStoresFrameLocation) {
  FrameFixture F;
  auto *Normal = llvm::BasicBlock::Create(F.Context, "normal", F.Fn);
  auto *Handler = llvm::BasicBlock::Create(F.Context, "handler", F.Fn);
  auto *Join = llvm::BasicBlock::Create(F.Context, "join", F.Fn);
  F.B.CreateCondBr(F.Fn->getArg(0), Normal, Handler);
  F.B.SetInsertPoint(Normal);
  auto *NormalSP = F.B.CreateSub(F.Base, F.B.getInt64(56));
  auto *NormalPtr = F.address(NormalSP, 32);
  F.B.CreateStore(F.B.getInt32(-100), NormalPtr);
  F.B.CreateBr(Join);
  F.B.SetInsertPoint(Handler);
  auto *HandlerSP = F.B.CreateSub(F.Base, F.B.getInt64(56));
  auto *HandlerPtr = F.address(HandlerSP, 32);
  F.B.CreateStore(F.B.getInt32(41), HandlerPtr);
  F.B.CreateBr(Join);
  F.B.SetInsertPoint(Join);
  auto *SP = F.B.CreatePHI(F.B.getInt64Ty(), 2);
  SP->addIncoming(NormalSP, Normal);
  SP->addIncoming(HandlerSP, Handler);
  auto *JoinedPtr = F.address(SP, 32);
  F.B.CreateRet(F.B.CreateLoad(F.B.getInt32Ty(), JoinedPtr));

  LLVMCAnalysisState State;
  analyzeDeadFrameStores(State, *F.Fn);
  for (const llvm::Value *Ptr : {NormalPtr, HandlerPtr, JoinedPtr}) {
    const auto It = State.FramePointerLocations.find(Ptr);
    ASSERT_NE(It, State.FramePointerLocations.end());
    EXPECT_EQ(It->second.first, F.Frame);
    EXPECT_EQ(It->second.second, 48);
  }
  EXPECT_EQ(State.RawFrameLocations.count({F.Frame, 48}), 0u);
}

TEST(LLVMCFrameMemory, StoreOnOneBranchCannotFeedJoinedLoad) {
  FrameFixture F;
  auto *Left = llvm::BasicBlock::Create(F.Context, "left", F.Fn);
  auto *Right = llvm::BasicBlock::Create(F.Context, "right", F.Fn);
  auto *Join = llvm::BasicBlock::Create(F.Context, "join", F.Fn);
  auto *Ptr = F.address(F.Base, 8);
  F.B.CreateCondBr(F.Fn->getArg(0), Left, Right);
  F.B.SetInsertPoint(Left);
  auto *Store = F.B.CreateStore(F.B.getInt32(17), Ptr);
  F.B.CreateBr(Join);
  F.B.SetInsertPoint(Right);
  F.B.CreateBr(Join);
  F.B.SetInsertPoint(Join);
  auto *Load = F.B.CreateLoad(F.B.getInt32Ty(), Ptr);
  F.B.CreateRet(Load);
  LLVMCAnalysisState State;
  analyzeDeadFrameStores(State, *F.Fn);
  analyzeStoreForwarding(State, *F.Fn);
  EXPECT_EQ(State.DeadFrameAllocas.count(F.Frame), 0u);
  EXPECT_EQ(State.DeadFrameStores.count(Store), 0u);
  EXPECT_EQ(State.ForwardedLoads.count(Load), 0u);
}

TEST(LLVMCFrameMemory, MutableCarrierIsNotAnSSAFrameAddressProof) {
  FrameFixture F;
  auto *Carrier = F.B.CreateAlloca(F.B.getPtrTy());
  F.B.CreateStore(F.address(F.Base, 8), Carrier);
  auto *Mutate = llvm::Function::Create(
      llvm::FunctionType::get(F.B.getVoidTy(), {F.B.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "mutate_carrier", F.Module);
  F.B.CreateCall(Mutate, {Carrier});
  auto *Ptr = F.B.CreateLoad(F.B.getPtrTy(), Carrier);
  F.B.CreateRet(F.B.CreateLoad(F.B.getInt32Ty(), Ptr));

  LLVMCAnalysisState State;
  analyzeDeadFrameStores(State, *F.Fn);
  EXPECT_EQ(State.FramePointerLocations.count(Ptr), 0u);
}

TEST(LLVMCFrameMemory, UnresolvedSubLoadKeepsFrame) {
  FrameFixture F;
  auto *Ptr = F.address(F.Base, 8);
  auto *Store = F.B.CreateStore(F.B.getInt32(17), Ptr);
  auto *Adjusted = F.B.CreateSub(F.Base, F.B.getInt64(24));
  auto *Load = F.B.CreateLoad(F.B.getInt32Ty(), F.address(Adjusted, 32));
  F.B.CreateRet(Load);
  LLVMCAnalysisState State;
  analyzeDeadFrameStores(State, *F.Fn);
  analyzeStoreForwarding(State, *F.Fn);
  // A proved address may forward; otherwise the backing store must survive.
  EXPECT_TRUE(State.ForwardedLoads.count(Load) ||
              (!State.DeadFrameAllocas.count(F.Frame) &&
               !State.DeadFrameStores.count(Store)));
}

TEST(LLVMCFrameMemory, DifferentWidthReadCannotUseWholeStoredValue) {
  FrameFixture F;
  auto *Ptr = F.address(F.Base, 8);
  auto *Store = F.B.CreateStore(F.B.getInt64(0x1234567800000011ULL), Ptr);
  auto *Load = F.B.CreateLoad(F.B.getInt32Ty(), Ptr);
  F.B.CreateRet(Load);
  LLVMCAnalysisState State;
  analyzeDeadFrameStores(State, *F.Fn);
  analyzeStoreForwarding(State, *F.Fn);
  EXPECT_EQ(State.ForwardedLoads.count(Load), 0u);
  EXPECT_EQ(State.DeadFrameStores.count(Store), 0u);
}

TEST(LLVMCFrameMemory, StraightLineStoreStillForwards) {
  FrameFixture F;
  auto *Ptr = F.address(F.Base, 8);
  auto *Value = F.B.getInt32(17);
  auto *Store = F.B.CreateStore(Value, Ptr);
  auto *Load = F.B.CreateLoad(F.B.getInt32Ty(), Ptr);
  F.B.CreateRet(Load);
  LLVMCAnalysisState State;
  analyzeDeadFrameStores(State, *F.Fn);
  analyzeStoreForwarding(State, *F.Fn);
  EXPECT_EQ(State.ForwardedLoads[Load], Value);
  EXPECT_EQ(State.DeadFrameStores.count(Store), 1u);
}

TEST(LLVMCFrameMemory, NarrowedAddressCannotForwardFromOriginalFrame) {
  for (bool TruncatePointer : {false, true}) {
    SCOPED_TRACE(TruncatePointer);
    FrameFixture F;
    auto *Ptr = F.address(F.Base, 8);
    auto *Store = F.B.CreateStore(F.B.getInt32(17), Ptr);
    llvm::Value *Narrow =
        TruncatePointer
            ? F.B.CreatePtrToInt(Ptr, F.B.getInt32Ty())
            : F.B.CreateTrunc(F.B.CreatePtrToInt(Ptr, F.B.getInt64Ty()),
                              F.B.getInt32Ty());
    auto *Wrapped = F.B.CreateIntToPtr(F.B.CreateZExt(Narrow, F.B.getInt64Ty()),
                                       F.B.getPtrTy());
    auto *Load = F.B.CreateLoad(F.B.getInt32Ty(), Wrapped);
    F.B.CreateRet(Load);
    LLVMCAnalysisState State;
    analyzeDeadFrameStores(State, *F.Fn);
    analyzeStoreForwarding(State, *F.Fn);
    // Removing the upper address bits can select unrelated memory. It does
    // not prove an exact read from the original frame allocation.
    EXPECT_EQ(State.ForwardedLoads.count(Load), 0u);
    EXPECT_EQ(State.DeadFrameStores.count(Store), 0u);
    EXPECT_EQ(State.DeadFrameAllocas.count(F.Frame), 0u);
    EXPECT_EQ(State.FramePointerLocations.count(Wrapped), 0u);
  }
}

TEST(LLVMCFrameMemory, PartialOverlappingReadKeepsProducer) {
  FrameFixture F;
  auto *Store = F.B.CreateStore(F.B.getInt64(0x1234567800000011ULL),
                                F.address(F.Base, 8));
  auto *Load = F.B.CreateLoad(F.B.getInt32Ty(), F.address(F.Base, 12));
  F.B.CreateRet(Load);
  LLVMCAnalysisState State;
  analyzeDeadFrameStores(State, *F.Fn);
  EXPECT_EQ(State.DeadFrameStores.count(Store), 0u);
  analyzeStoreForwarding(State, *F.Fn);
  EXPECT_EQ(State.DeadFrameStores.count(Store), 0u);
  EXPECT_EQ(State.ForwardedLoads.count(Load), 0u);
}

TEST(LLVMCFrameMemory, LoadedScalarIsNotItsStorageAddress) {
  FrameFixture F;
  auto *Ptr = F.address(F.Base, 8);
  auto *Value = F.B.getInt64(4096);
  F.B.CreateStore(Value, Ptr);
  auto *Load = F.B.CreateLoad(F.B.getInt64Ty(), Ptr);
  auto *Indirect = F.B.CreateLoad(F.B.getInt32Ty(),
                                  F.B.CreateIntToPtr(Load, F.B.getPtrTy()));
  F.B.CreateRet(Indirect);
  LLVMCAnalysisState State;
  analyzeDeadFrameStores(State, *F.Fn);
  analyzeStoreForwarding(State, *F.Fn);
  EXPECT_EQ(State.ForwardedLoads[Load], Value);
  EXPECT_EQ(State.ForwardedLoads.count(Indirect), 0u);
}

TEST(LLVMCFrameMemory, SelectReadsKeepBothPossibleStores) {
  FrameFixture F;
  auto *A = F.address(F.Base, 8);
  auto *B = F.address(F.Base, 16);
  auto *SA = F.B.CreateStore(F.B.getInt32(17), A);
  auto *SB = F.B.CreateStore(F.B.getInt32(41), B);
  auto *Load =
      F.B.CreateLoad(F.B.getInt32Ty(), F.B.CreateSelect(F.Fn->getArg(0), A, B));
  F.B.CreateRet(Load);
  LLVMCAnalysisState State;
  analyzeDeadFrameStores(State, *F.Fn);
  analyzeStoreForwarding(State, *F.Fn);
  EXPECT_EQ(State.DeadFrameStores.count(SA), 0u);
  EXPECT_EQ(State.DeadFrameStores.count(SB), 0u);
  EXPECT_EQ(State.ForwardedLoads.count(Load), 0u);
}

TEST(LLVMCFrameMemory, MixedAddressAlternativesCannotForward) {
  for (bool UsePhi : {false, true}) {
    for (bool Literal : {false, true}) {
      SCOPED_TRACE(UsePhi);
      SCOPED_TRACE(Literal);
      FrameFixture F;
      auto *FramePtr = F.address(F.Base, 8);
      auto *Store = F.B.CreateStore(F.B.getInt32(17), FramePtr);
      llvm::Value *Other = nullptr;
      if (Literal) {
        Other = F.B.CreateIntToPtr(F.B.getInt64(4096), F.B.getPtrTy());
      } else {
        Other = F.B.CreateAlloca(F.B.getInt32Ty());
        F.B.CreateStore(F.B.getInt32(41), Other);
      }
      llvm::Value *Ptr = nullptr;
      if (UsePhi) {
        auto *Left = llvm::BasicBlock::Create(F.Context, "left", F.Fn);
        auto *Right = llvm::BasicBlock::Create(F.Context, "right", F.Fn);
        auto *Join = llvm::BasicBlock::Create(F.Context, "join", F.Fn);
        F.B.CreateCondBr(F.Fn->getArg(0), Left, Right);
        F.B.SetInsertPoint(Left);
        F.B.CreateBr(Join);
        F.B.SetInsertPoint(Right);
        F.B.CreateBr(Join);
        F.B.SetInsertPoint(Join);
        auto *Phi = F.B.CreatePHI(F.B.getPtrTy(), 2);
        Phi->addIncoming(FramePtr, Left);
        Phi->addIncoming(Other, Right);
        Ptr = Phi;
      } else {
        Ptr = F.B.CreateSelect(F.Fn->getArg(0), FramePtr, Other);
      }
      auto *Load = F.B.CreateLoad(F.B.getInt32Ty(), Ptr);
      F.B.CreateRet(Load);
      LLVMCAnalysisState State;
      analyzeDeadFrameStores(State, *F.Fn);
      analyzeStoreForwarding(State, *F.Fn);
      EXPECT_EQ(State.ForwardedLoads.count(Load), 0u);
      EXPECT_EQ(State.DeadFrameStores.count(Store), 0u);
      EXPECT_EQ(State.DeadFrameAllocas.count(F.Frame), 0u);
    }
  }
}

TEST(LLVMCFrameMemory, MixedDestinationStoreKeepsExternalWrite) {
  FrameFixture F;
  auto *Other = F.B.CreateAlloca(F.B.getInt32Ty());
  auto *Ptr = F.B.CreateSelect(F.Fn->getArg(0), F.address(F.Base, 8), Other);
  auto *Store = F.B.CreateStore(F.B.getInt32(17), Ptr);
  F.B.CreateRet(F.B.CreateLoad(F.B.getInt32Ty(), Other));
  LLVMCAnalysisState State;
  analyzeDeadFrameStores(State, *F.Fn);
  analyzeStoreForwarding(State, *F.Fn);
  EXPECT_EQ(State.DeadFrameStores.count(Store), 0u);
  EXPECT_EQ(State.DeadFrameAllocas.count(F.Frame), 0u);
}

TEST(LLVMCFrameMemory, ExportedFrameAddressKeepsStoredBytes) {
  FrameFixture F;
  auto *Out = new llvm::GlobalVariable(F.Module, F.B.getPtrTy(), false,
                                       llvm::GlobalValue::ExternalLinkage,
                                       nullptr, "out");
  auto *Store = F.B.CreateStore(F.B.getInt32(17), F.address(F.Base, 8));
  auto *Escape = F.B.CreateStore(F.Frame, Out);
  F.B.CreateRet(F.B.getInt32(0));
  LLVMCAnalysisState State;
  analyzeDeadFrameStores(State, *F.Fn);
  analyzeStoreForwarding(State, *F.Fn);
  EXPECT_EQ(State.DeadFrameStores.count(Store), 0u);
  EXPECT_EQ(State.DeadFrameStores.count(Escape), 0u);
  EXPECT_EQ(State.DeadFrameAllocas.count(F.Frame), 0u);
}

TEST(LLVMCFrameMemory, DeadResultKeepsObservableMemoryOperation) {
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    SCOPED_TRACE(Kind);
    FrameFixture F;
    auto *External = new llvm::GlobalVariable(
        F.Module, F.B.getInt32Ty(), false, llvm::GlobalValue::ExternalLinkage,
        nullptr, "external");
    llvm::Instruction *Effect = nullptr;
    if (Kind < 2) {
      auto *Load = F.B.CreateLoad(F.B.getInt32Ty(), External);
      if (Kind == 0)
        Load->setVolatile(true);
      else
        Load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
      Effect = Load;
    } else {
      Effect = F.B.CreateAtomicRMW(
          llvm::AtomicRMWInst::Add, External, F.B.getInt32(1),
          llvm::MaybeAlign(4), llvm::AtomicOrdering::SequentiallyConsistent);
    }
    auto *Dead = F.B.CreateAlloca(llvm::ArrayType::get(F.B.getInt8Ty(), 16));
    F.B.CreateStore(Effect, Dead);
    auto *Ptr = F.address(F.Base, 8);
    F.B.CreateStore(F.B.getInt32(17), Ptr);
    auto *Load = F.B.CreateLoad(F.B.getInt32Ty(), Ptr);
    F.B.CreateRet(Load);
    LLVMCAnalysisState State;
    analyzeDeadFrameStores(State, *F.Fn);
    EXPECT_EQ(State.DeadFrameStores.count(Effect), 0u);
    analyzeStoreForwarding(State, *F.Fn);
    EXPECT_EQ(State.DeadFrameStores.count(Effect), 0u);
    EXPECT_EQ(State.ForwardedLoads.count(Load), 1u);
  }
}

TEST(LLVMCFrameMemory, DeadInvokeResultKeepsCallAndUnwind) {
  FrameFixture F;
  auto *Normal = llvm::BasicBlock::Create(F.Context, "normal", F.Fn);
  auto *Unwind = llvm::BasicBlock::Create(F.Context, "unwind", F.Fn);
  auto *Dead = F.B.CreateAlloca(llvm::ArrayType::get(F.B.getInt8Ty(), 16));
  auto *Ptr = F.address(F.Base, 8);
  F.B.CreateStore(F.B.getInt32(17), Ptr);
  auto Callee = F.Module.getOrInsertFunction("may_throw", F.B.getInt32Ty());
  auto Personality = F.Module.getOrInsertFunction(
      "personality", llvm::FunctionType::get(F.B.getInt32Ty(), true));
  F.Fn->setPersonalityFn(llvm::cast<llvm::Constant>(Personality.getCallee()));
  auto *Invoke = F.B.CreateInvoke(Callee, Normal, Unwind);
  F.B.SetInsertPoint(Normal);
  F.B.CreateStore(Invoke, Dead);
  auto *Load = F.B.CreateLoad(F.B.getInt32Ty(), Ptr);
  F.B.CreateRet(Load);
  F.B.SetInsertPoint(Unwind);
  auto *Pad = F.B.CreateLandingPad(
      llvm::StructType::get(F.B.getPtrTy(), F.B.getInt32Ty()), 0);
  Pad->setCleanup(true);
  auto *Resume = F.B.CreateResume(Pad);
  LLVMCAnalysisState State;
  analyzeDeadFrameStores(State, *F.Fn);
  EXPECT_EQ(State.DeadFrameStores.count(Invoke), 0u);
  analyzeStoreForwarding(State, *F.Fn);
  EXPECT_EQ(State.DeadFrameStores.count(Invoke), 0u);
  EXPECT_EQ(State.DeadFrameStores.count(Resume), 0u);
  EXPECT_EQ(State.ForwardedLoads.count(Load), 1u);
}

TEST(LLVMCFrameMemory, ObservableFrameStoreRemainsWithoutRead) {
  for (bool Atomic : {false, true}) {
    FrameFixture F;
    auto *Store = F.B.CreateStore(F.B.getInt32(17), F.address(F.Base, 8));
    if (Atomic)
      Store->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
    else
      Store->setVolatile(true);
    F.B.CreateRet(F.B.getInt32(0));
    LLVMCAnalysisState State;
    analyzeDeadFrameStores(State, *F.Fn);
    analyzeStoreForwarding(State, *F.Fn);
    EXPECT_EQ(State.DeadFrameStores.count(Store), 0u);
    EXPECT_EQ(State.DeadFrameAllocas.count(F.Frame), 0u);
  }
}
TEST(LLVMCFrameMemory, AtomicFrameReadKeepsEarlierStore) {
  for (bool CompareExchange : {false, true}) {
    FrameFixture F;
    auto *Ptr = F.address(F.Base, 8);
    auto *Store = F.B.CreateStore(F.B.getInt32(17), Ptr);
    llvm::Instruction *Effect = nullptr;
    if (CompareExchange)
      Effect = F.B.CreateAtomicCmpXchg(
          Ptr, F.B.getInt32(17), F.B.getInt32(41), llvm::MaybeAlign(4),
          llvm::AtomicOrdering::SequentiallyConsistent,
          llvm::AtomicOrdering::SequentiallyConsistent);
    else
      Effect = F.B.CreateAtomicRMW(
          llvm::AtomicRMWInst::Add, Ptr, F.B.getInt32(1), llvm::MaybeAlign(4),
          llvm::AtomicOrdering::SequentiallyConsistent);
    F.B.CreateRet(F.B.getInt32(0));
    LLVMCAnalysisState State;
    analyzeDeadFrameStores(State, *F.Fn);
    analyzeStoreForwarding(State, *F.Fn);
    EXPECT_EQ(State.DeadFrameStores.count(Store), 0u);
    EXPECT_EQ(State.DeadFrameStores.count(Effect), 0u);
    EXPECT_EQ(State.DeadFrameAllocas.count(F.Frame), 0u);
  }
}
TEST(LLVMCFrameMemory, ExportedCarrierKeepsItsFrameAddressStore) {
  FrameFixture F;
  auto *Carrier = F.B.CreateAlloca(F.B.getPtrTy());
  auto *Store = F.B.CreateStore(F.B.getInt32(17), F.address(F.Base, 8));
  auto *Escape = F.B.CreateStore(F.Frame, Carrier);
  auto Consume =
      F.Module.getOrInsertFunction("consume", F.B.getVoidTy(), F.B.getPtrTy());
  F.B.CreateCall(Consume, {Carrier});
  auto *Dead = F.B.CreateAlloca(llvm::ArrayType::get(F.B.getInt8Ty(), 16));
  F.B.CreateStore(F.B.CreateLoad(F.B.getPtrTy(), Carrier), Dead);
  F.B.CreateRet(F.B.getInt32(0));
  LLVMCAnalysisState State;
  analyzeDeadFrameStores(State, *F.Fn);
  analyzeStoreForwarding(State, *F.Fn);
  EXPECT_EQ(State.DeadFrameStores.count(Store), 0u);
  EXPECT_EQ(State.DeadFrameStores.count(Escape), 0u);
  EXPECT_EQ(State.DeadFrameAllocas.count(F.Frame), 0u);
}
TEST(LLVMCFrameMemory, ObservableCarrierReadKeepsItsValueProducer) {
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    SCOPED_TRACE(Kind);
    FrameFixture F;
    auto *Carrier = F.B.CreateAlloca(F.B.getInt32Ty());
    auto *Producer = F.B.CreateAdd(
        F.B.CreateZExt(F.Fn->getArg(0), F.B.getInt32Ty()), F.B.getInt32(1));
    auto *Store = F.B.CreateStore(Producer, Carrier);
    auto *Load = F.B.CreateLoad(F.B.getInt32Ty(), Carrier);
    if (Kind == 0)
      Load->setVolatile(true);
    else
      Load->setAtomic(Kind == 1 ? llvm::AtomicOrdering::Unordered
                                : llvm::AtomicOrdering::SequentiallyConsistent);
    auto *DeadStore = F.B.CreateStore(Load, F.address(F.Base, 8));
    F.B.CreateRet(F.B.getInt32(0));
    LLVMCAnalysisState State;
    analyzeDeadFrameStores(State, *F.Fn);
    EXPECT_EQ(State.DeadFrameStores.count(DeadStore), 1u);
    EXPECT_EQ(
        State.DeadFrameStores.count(llvm::cast<llvm::Instruction>(Producer)),
        0u);
    EXPECT_EQ(State.DeadFrameStores.count(Store), 0u);
    EXPECT_EQ(State.DeadFrameStores.count(Load), 0u);
    analyzeStoreForwarding(State, *F.Fn);
    EXPECT_EQ(
        State.DeadFrameStores.count(llvm::cast<llvm::Instruction>(Producer)),
        0u);
  }
}
} // namespace
