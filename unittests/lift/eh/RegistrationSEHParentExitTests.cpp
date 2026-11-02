//===- RegistrationSEHParentExitTests.cpp - Finally parent-exit identity --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Tests source parent-exit identity, chain lifetime and failed projection.

#include "../../../lib/backend/codegen/COFF/COFFRegistrationSEHParentExit.h"
#include "../../../lib/backend/llvm/X86/MedLLVMRegistrationSEHParentExit.h"
#include "../../../lib/ir/med/X86/RegistrationRoots.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/X86/X86RegistrationCallback.h"
#include "neverd/ir/med/X86/X86RegistrationSEHBoundCall.h"
#include "neverd/ir/med/X86/X86RegistrationSEHCall.h"
#include "neverd/ir/med/X86/X86RegistrationSEHParentExit.h"
#include "neverd/ir/med/X86/X86RegistrationSEHRegisters.h"
#include "neverd/ir/med/X86/X86RegistrationSEHStateStore.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/COFF/COFFLoader.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdlib>

using namespace neverd;

namespace {

MedFunc parentExit(bool Runtime) {
  MedFunc Function;
  Function.Entry = 0x1000;
  auto &EH = Function.ExceptionMetadata.emplace();
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
  EH.CodeRange = {0x1000, 0x1900};
  auto &Chain = EH.Registration.emplace();
  Chain.RegistrationOffset = -16;
  Chain.Scopes = {{-1, 0, 0x1800, true}};
  auto &States = Function.RegistrationStates.emplace();
  States.Complete = States.CallbackStatesComplete =
      States.RegistrationLifetimeComplete = true;
  constexpr va_t Addresses[] = {0x1000, 0x1800, 0x1100};
  for (int Id = 0; Id != 3; ++Id) {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Addresses[Id];
    Block.EndAddr = Addresses[Id] + 5;
    Function.Blocks.push_back(Block);
    RegistrationBlockState State;
    State.BlockId = Id;
    State.Range = {Block.StartAddr, Block.EndAddr};
    State.Reached = true;
    State.CallbackOnly = Id == 1;
    States.Blocks.push_back(State);
  }
  auto &Call = Function.Blocks[0];
  Call.Succs = {2};
  MedOp Invoke;
  Invoke.Opcode = NdOp::CALL;
  Invoke.Addr = 0x1000;
  Invoke.OriginSeq = 0;
  Invoke.addInput(MedVar::makeConst(0x1800, 4));
  Call.Ops.push_back(Invoke);
  auto &Finally = Function.Blocks[1];
  Finally.Succs = {2};
  MedOp Restore;
  Restore.Opcode = NdOp::LOAD;
  Restore.Addr = 0x1800;
  Restore.OriginSeq = 1;
  Restore.Output.Kind = MedVar::Reg;
  Restore.Output.TheArch = Arch::X86;
  Restore.Output.Size = 4;
  Restore.Output.RegOff = x86reg::RSP;
  MedVar Pointer;
  Pointer.Size = 4;
  Restore.addInput(Pointer);
  Finally.Ops.push_back(Restore);
  MedOp Branch;
  Branch.Opcode = NdOp::BRANCH;
  Branch.Addr = 0x1803;
  Branch.OriginSeq = 0;
  Branch.addInput(MedVar::makeConst(0x1100, 8));
  Finally.Ops.push_back(Branch);
  States.FrameValues.push_back({0x1800, 1, -64});
  RegistrationSEHInvocationExit Exit;
  Exit.Exit = RegistrationSEHInvocationExit::Kind::ParentJump;
  Exit.EntryVA = 0x1800;
  Exit.CallVA = Runtime ? 0 : 0x1000;
  Exit.Address = 0x1803;
  Exit.EndAddress = 0x1805;
  Exit.OpSeq = 0;
  Exit.TargetVA = 0x1100;
  Exit.StackRestoreVA = 0x1800;
  Exit.StackRestoreSeq = 1;
  Exit.StackOffset = -64;
  States.SEHInvocationExits.push_back(Exit);
  return Function;
}

MedFunc parentFallthrough(bool Runtime) {
  auto Function = parentExit(Runtime);
  auto &States = *Function.RegistrationStates;
  auto &Exit = States.SEHInvocationExits.front();
  Exit.Exit = RegistrationSEHInvocationExit::Kind::ParentFallthrough;
  Exit.TargetVA = Exit.EndAddress = 0x180a;
  auto &Finally = Function.Blocks[1];
  Finally.EndAddr = 0x180a;
  States.Blocks[1].Range.End = 0x180a;
  auto &Remove = Finally.Ops.back();
  Remove.Opcode = NdOp::STORE;
  Remove.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  Remove.Inputs[0] = MedVar::makeConst(0, 4);
  MedVar Value;
  Value.Kind = MedVar::Reg;
  Value.TheArch = Arch::X86;
  Value.RegOff = x86reg::RCX;
  Value.Size = 4;
  Remove.addInput(Value);
  auto &Target = Function.Blocks[2];
  Target.StartAddr = 0x180a;
  Target.EndAddr = 0x1811;
  States.Blocks[2].Range = {Target.StartAddr, Target.EndAddr};
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = 0x1810;
  Return.OriginSeq = 0;
  Target.Ops.push_back(Return);
  States.ChainOperationsComplete = true;
  States.ChainAccesses.push_back(
      {0x1803, 0x180a, 0, RegistrationChainAccess::Kind::Remove});
  States.SEHControls.push_back(
      {RegistrationSEHControlContract::Kind::ParentEpilogue, 0x180a, 0x1811,
       0x1810});
  return Function;
}

} // namespace

TEST(RegistrationSEHParentExit, UnusedTransportDoesNotAddAParentParameter) {
  using namespace x86_registration;
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = parentFallthrough(false);
    MedVar Value;
    Value.Kind = MedVar::Reg;
    Value.TheArch = Arch::X86;
    Value.RegOff = x86reg::RCX;
    Value.Id = 20;
    Value.Size = 4;
    MedOp Write;
    Write.Opcode = NdOp::INTRINSIC;
    Write.Addr = 0x1000;
    Write.addInput(
        MedVar::makeConst(unsigned(Intrinsic::X86SEHRegisterWrite), 2));
    Write.addInput(MedVar::makeConst(unsigned(SEHRegisterRole::CallInput), 1));
    Write.addInput(MedVar::makeConst(0x1800, 4));
    Write.addInput(MedVar::makeConst(Write.Addr, 4));
    Write.addInput(MedVar::makeConst(x86reg::RCX, 2));
    Write.addInput(Value);
    MedOp Read = Write;
    Read.Addr = 0x1800;
    Read.NumInputs = 5;
    Read.Inputs[0] =
        MedVar::makeConst(unsigned(Intrinsic::X86SEHRegisterRead), 2);
    Read.Inputs[1] =
        MedVar::makeConst(unsigned(SEHRegisterRole::CallbackInput), 1);
    Read.Inputs[3] = MedVar::makeConst(Read.Addr, 4);
    Read.Output = Value;
    Read.Output.SSAVer = 1;
    if (Mutation == 4)
      Read.NumInputs = 4;
    if (Mutation == 5)
      Function.RegistrationStates->Blocks[1].Unknown = true;
    auto &Caller = Function.Blocks[0];
    auto &Callback = Function.Blocks[1];
    Caller.Ops.insert(Caller.Ops.begin(), Write);
    if (Mutation == 9)
      Caller.Ops.insert(Caller.Ops.begin() + 1, Read);
    else
      Callback.Ops.insert(Callback.Ops.begin(), Read);
    if (Mutation == 1 || Mutation == 2 || Mutation == 3) {
      MedOp Use;
      Use.Addr = 0x1801;
      if (Mutation == 1) {
        Use.Opcode = NdOp::STORE;
        Use.addInput(MedVar::makeConst(0x5000, 4));
        Use.addInput(Read.Output);
      } else if (Mutation == 2) {
        Use.Opcode = NdOp::COPY;
        Use.Output = Read.Output;
        Use.Output.Id = 21;
        Use.addInput(Read.Output);
      } else {
        Use.Opcode = NdOp::CALL;
        Use.addInput(MedVar::makeConst(0x5000, 4));
      }
      Callback.Ops.insert(Callback.Ops.end() - 1, Use);
    }
    if (Mutation == 6) {
      PhiNode Phi;
      Phi.Output = Read.Output;
      Phi.Output.Id = 21;
      Phi.Args.emplace_back(1, Read.Output);
      Function.Blocks[2].Phis.push_back(Phi);
    }
    if (Mutation == 7) {
      MedCallClobber Clobber;
      Clobber.PreservedPrefixSize = 4;
      Clobber.PreservedInput = Read.Output;
      Function.CallClobbers.push_back(Clobber);
    }
    if (Mutation == 8) {
      Function.CallInfos.emplace_back();
      Function.CallInfos.back().Args.push_back(Read.Output);
    }
    discardUnusedSEHCallInputs(Function);
    EXPECT_EQ(Caller.Ops.front().Inputs[5].Kind == MedVar::Unspecified,
              Mutation == 0 || Mutation == 6);
  }
}

TEST(RegistrationSEHParentExit, ParentCallsDoNotRequireAnAbsentJumpProtocol) {
  auto Function = parentExit(false);
  auto &States = *Function.RegistrationStates;
  States.HasBoundSEHCallees = true;
  States.SEHBoundCallsComplete = false;
  EXPECT_FALSE(getX86RegistrationSEHBoundCalls(Function));
  EXPECT_TRUE(getRegistrationSEHNonlocalCalls(Function));
  States.SEHJumpCapturesComplete = false;
  EXPECT_FALSE(getRegistrationSEHNonlocalCalls(Function));
}

TEST(RegistrationSEHParentExit, AdjacentExitRequiresEveryPhysicalAndCFGEdge) {
  for (bool Detached : {false, true})
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      SCOPED_TRACE(Detached);
      SCOPED_TRACE(Mutation);
      auto Function = parentFallthrough(false);
      auto &States = *Function.RegistrationStates;
      auto &Caller = Function.Blocks[0];
      auto &Callback = Function.Blocks[1];
      Function.Entry = Caller.StartAddr = 0x17fb;
      Caller.EndAddr = Callback.StartAddr;
      States.Blocks[0].Range = {Caller.StartAddr, Caller.EndAddr};
      Caller.Ops.back().Addr = Caller.StartAddr;
      States.SEHInvocationExits.front().CallVA = Caller.StartAddr;
      States.SEHControls.push_back(
          {RegistrationSEHControlContract::Kind::NearCall, Caller.StartAddr,
           Caller.EndAddr, Callback.StartAddr});
      Caller.Ops.back().DoesNotReturn = Detached;
      Caller.Succs = Detached ? std::vector<int>{} : std::vector<int>{1};
      Callback.Preds = Detached ? std::vector<int>{} : std::vector<int>{0};
      if (Mutation == 1)
        ++Caller.Ops.back().Inputs[0].ConstVal;
      if (Mutation == 2)
        States.SEHControls.pop_back();
      if (Mutation == 3)
        ++States.SEHControls.back().Target;
      if (Mutation == 4)
        Callback.Preds.push_back(1);
      if (Mutation == 5)
        Caller.Ops.back().Dead = true;
      if (Mutation == 6) {
        Caller.Succs.clear();
        Caller.Ops.back().DoesNotReturn = false;
      }
      const auto Exits = getRegistrationSEHAdjacentExits(Function);
      EXPECT_EQ(bool(Exits), Mutation == 0);
      if (!Mutation) {
        ASSERT_TRUE(Exits);
        EXPECT_EQ(*Exits, (std::map<int, int>{{0, 1}}));
      }
    }
}

TEST(RegistrationSEHParentExit, FallthroughBindsTheSourceSegmentOffsetCopy) {
  for (bool Wide : {false, true})
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      SCOPED_TRACE(Wide);
      SCOPED_TRACE(Mutation);
      auto Function = parentFallthrough(true);
      auto &Finally = Function.Blocks[1];
      auto &States = *Function.RegistrationStates;
      MedOp Copy;
      Copy.Opcode = NdOp::COPY;
      Copy.Addr = Finally.Ops.back().Addr;
      Copy.OriginSeq = 0;
      Copy.Output.Kind = MedVar::Temp;
      Copy.Output.Id = 10;
      Copy.Output.Size = 4;
      Copy.addInput(MedVar::makeConst(0, 4));
      MedOp Extend;
      Extend.Opcode = NdOp::INT_ZEXT;
      Extend.Addr = Copy.Addr;
      Extend.OriginSeq = 1;
      Extend.Output = Copy.Output;
      Extend.Output.Id = 12;
      Extend.Output.Size = 8;
      Extend.addInput(Copy.Output);
      Finally.Ops.back().Inputs[0] = Wide ? Extend.Output : Copy.Output;
      Finally.Ops.back().OriginSeq = Wide ? 2 : 1;
      States.ChainAccesses.front().OpSeq = Wide ? 2 : 1;
      States.SEHInvocationExits.front().OpSeq = Wide ? 2 : 1;
      if (Mutation == 1)
        Copy.Inputs[0].ConstVal = 4;
      if (Mutation == 2)
        --Copy.Addr;
      if (Mutation == 3)
        Copy.OriginSeq = -1;
      if (Mutation == 4)
        Copy.Output.Id = 11;
      if (Mutation == 5)
        Copy.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      if (Mutation == 6)
        Copy.Inputs[0].Size = 2;
      if (Mutation == 7)
        Copy.Dead = true;
      Finally.Ops.insert(Finally.Ops.end() - 1, Copy);
      if (Wide)
        Finally.Ops.insert(Finally.Ops.end() - 1, Extend);
      EXPECT_EQ(bool(getRegistrationSEHParentExits(Function)), Mutation == 0);
    }
}

TEST(RegistrationSEHParentExit, FallthroughRequiresItsRemovalAndWholeEpilogue) {
  for (bool Runtime : {false, true})
    for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
      SCOPED_TRACE(Runtime);
      SCOPED_TRACE(Mutation);
      auto Function = parentFallthrough(Runtime);
      auto &States = *Function.RegistrationStates;
      auto &Remove = Function.Blocks[1].Ops.back();
      switch (Mutation) {
      case 1:
        States.ChainOperationsComplete = false;
        break;
      case 2:
        States.ChainAccesses.clear();
        break;
      case 3:
        States.ChainAccesses.front().AccessKind =
            RegistrationChainAccess::Kind::Install;
        break;
      case 4:
        States.ChainAccesses.push_back(States.ChainAccesses.front());
        break;
      case 5:
        ++States.ChainAccesses.front().OpSeq;
        break;
      case 6:
        States.SEHControls.clear();
        break;
      case 7:
        ++States.SEHControls.front().EndAddress;
        break;
      case 8:
        States.SEHControls.front().Control =
            RegistrationSEHControlContract::Kind::NearReturn;
        break;
      case 9:
        Remove.MemoryAddressSpace = NdMemoryAddressSpace::Default;
        break;
      case 10:
        Remove.Inputs[1] = MedVar::makeConst(0, 4);
        break;
      case 11:
        Function.Blocks[2].Ops.back().Dead = true;
        break;
      case 12:
        Function.Blocks[2].Succs = {0};
        break;
      case 13:
        ++States.SEHInvocationExits.front().EndAddress;
        break;
      }
      auto Exits = getRegistrationSEHParentExits(Function);
      EXPECT_EQ(bool(Exits), Mutation == 0);
      if (Mutation)
        continue;
      ASSERT_EQ(Exits->size(), 1u);
      EXPECT_EQ(Exits->at(1).Operation, &Remove);
      const auto Returns = getRegistrationSEHReturns(Function);
      ASSERT_TRUE(Returns);
      EXPECT_TRUE(Returns->ReturnBlocks.empty());
    }
}

TEST(RegistrationSEHParentExit, BindsOrdinaryAndRuntimeSourceExits) {
  for (bool Runtime : {false, true}) {
    auto Function = parentExit(Runtime);
    auto Exits = getRegistrationSEHParentExits(Function);
    ASSERT_TRUE(Exits);
    ASSERT_EQ(Exits->size(), 1u);
    EXPECT_EQ(Exits->at(1).EntryVA, 0x1800u);
    EXPECT_EQ(Exits->at(1).TargetBlock, 2);
    EXPECT_EQ(Exits->at(1).RestoreBlock, 1);
    EXPECT_EQ(Exits->at(1).StackOffset, -64);
    EXPECT_EQ(Exits->at(1).Operation, &Function.Blocks[1].Ops[1]);
    EXPECT_EQ(Exits->at(1).Restore, &Function.Blocks[1].Ops[0]);
    EXPECT_EQ(Exits->at(1).Invocations.size(), 1u);
    auto Owners = registrationSEHCallbackOwners(Function);
    ASSERT_TRUE(Owners);
    EXPECT_EQ(*Owners,
              (std::map<int, std::pair<va_t, bool>>{{1, {0x1800, true}}}));
    std::reverse(Function.Blocks.begin(), Function.Blocks.end());
    EXPECT_EQ(registrationSEHCallbackOwners(Function), Owners);
    EXPECT_TRUE(getRegistrationSEHParentExits(Function));
    // A source parent branch is not the callback's ordinary C return ABI.
    const auto Returns = getRegistrationSEHReturns(Function);
    ASSERT_TRUE(Returns);
    EXPECT_TRUE(Returns->ReturnBlocks.empty());
    EXPECT_TRUE(Returns->ReturningCalls.empty());
  }
}

TEST(RegistrationSEHParentExit, RejectsChangedSourceAndInvocationEvidence) {
  for (unsigned Mutation = 0; Mutation != 37; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = parentExit(false);
    auto &States = *Function.RegistrationStates;
    auto &Exit = States.SEHInvocationExits.front();
    auto &Block = Function.Blocks[1];
    auto &Restore = Block.Ops.front();
    auto &Branch = Block.Ops.back();
    switch (Mutation) {
    case 0:
      States.SEHInvocationExitsComplete = false;
      break;
    case 1:
      ++Exit.Address;
      break;
    case 2:
      ++Exit.OpSeq;
      break;
    case 3:
      ++Exit.EndAddress;
      break;
    case 4:
      ++Exit.TargetVA;
      break;
    case 5:
      ++Exit.StackRestoreVA;
      break;
    case 6:
      ++Exit.StackRestoreSeq;
      break;
    case 7:
      ++Exit.StackOffset;
      break;
    case 8:
      ++Exit.EntryVA;
      break;
    case 9:
      ++Exit.CallVA;
      break;
    case 10:
      Branch.Inputs[0].ConstVal = 0x1000;
      break;
    case 11:
      Branch.Inputs[0].Size = 2;
      break;
    case 12:
      Branch.Opcode = NdOp::COND_BR;
      break;
    case 13:
      Branch.Dead = true;
      break;
    case 14:
      Restore.Dead = true;
      break;
    case 15:
      Restore.Output.RegOff = x86reg::RBP;
      break;
    case 16:
      Restore.Output.Size = 2;
      break;
    case 17:
      Restore.Opcode = NdOp::INT_ADD;
      break;
    case 18:
      Restore.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 19:
      States.FrameValues.clear();
      break;
    case 20:
      States.FrameValues.front().EstablishedFrameOffset.reset();
      break;
    case 21:
      States.SEHInvocationExits.push_back(Exit);
      break;
    case 22:
      Block.Succs.clear();
      break;
    case 23:
      Block.Succs.push_back(0);
      break;
    case 24:
      States.Blocks[2].CallbackOnly = true;
      break;
    case 25:
      States.Blocks[1].CallbackOnly = false;
      break;
    case 26:
      States.Blocks[2].Reached = false;
      break;
    case 27:
      States.RegistrationLifetimeComplete = false;
      break;
    case 28:
      Function.Blocks[0].Ops[0].Inputs[0].ConstVal += 1;
      break;
    case 29:
      Function.Blocks[0].Ops[0].ExactArguments = true;
      break;
    case 30:
      Function.ExceptionMetadata->Registration->Scopes[0].IsFinally = false;
      break;
    case 31:
      Restore.Output.TheArch = Arch::X64;
      break;
    case 32:
      Restore.Inputs[0] = MedVar::makeConst(0, 4);
      break;
    case 33:
      Block.Ops.push_back(Branch);
      break;
    case 34:
      States.Blocks.push_back(States.Blocks[1]);
      break;
    case 35:
      Function.SkippedSSA = true;
      break;
    case 36:
      States.Blocks[1].Reached = false;
      break;
    }
    EXPECT_FALSE(getRegistrationSEHParentExits(Function));
    EXPECT_FALSE(registrationSEHCallbackOwners(Function));
  }
}

TEST(RegistrationSEHParentExit, RetainsOnlyAnAuthenticatedOrdinaryCallback) {
  auto Function = parentExit(false);
  auto &Chain = *Function.ExceptionMetadata->Registration;
  Chain.TryLevelOffset = -4;
  Chain.SeededTryLevel = -1;
  Chain.Scopes = {{-1, 0x1700, 0x1750, false}, {0, 0, 0x1800, true}};
  for (auto &State : Function.RegistrationStates->Blocks)
    State.ExplicitLevels = {0};
  ASSERT_TRUE(getRegistrationSEHNonlocalCalls(Function));
  EXPECT_EQ(getX86RegistrationSEHRetainedScopes(Function),
            std::optional<std::vector<uint32_t>>({1}));
  Function.Blocks[0].Ops[0].Inputs[0].ConstVal += 1;
  EXPECT_FALSE(getX86RegistrationSEHRetainedScopes(Function));
}

TEST(RegistrationSEHParentExit, OneBranchMayEndSeveralInvocations) {
  auto Function = parentExit(false);
  auto Exit = Function.RegistrationStates->SEHInvocationExits.front();
  Exit.CallVA = 0;
  Function.RegistrationStates->SEHInvocationExits.push_back(Exit);
  auto Exits = getRegistrationSEHParentExits(Function);
  ASSERT_TRUE(Exits);
  ASSERT_EQ(Exits->at(1).Invocations.size(), 2u);
  --Function.RegistrationStates->SEHInvocationExits.back().StackOffset;
  EXPECT_FALSE(getRegistrationSEHParentExits(Function));
}

TEST(RegistrationSEHParentExit, ParentFrameRequiresOnlyBoundIncomingTransfers) {
  for (bool Runtime : {false, true}) {
    for (unsigned Mutation = 0; Mutation != 20; ++Mutation) {
      SCOPED_TRACE(Runtime);
      SCOPED_TRACE(Mutation);
      auto Function = parentExit(Runtime);
      Function.ExceptionMetadata->Registration->TryLevelOffset = -4;
      Function.Blocks[0].Succs.clear();
      Function.Blocks[2].Preds = {1};
      MedOp Frame;
      Frame.Opcode = NdOp::COPY;
      Frame.Addr = 0x1800;
      Frame.RegistrationRoot =
          MedOp::RegistrationRootKind::EstablishedFramePointer;
      Frame.Output.Kind = MedVar::Reg;
      Frame.Output.TheArch = Arch::X86;
      Frame.Output.RegOff = x86reg::RBP;
      Frame.Output.Size = 4;
      Frame.Output.Id = 17;
      Frame.addInput(Frame.Output);
      Function.Blocks[1].Ops.insert(Function.Blocks[1].Ops.begin(), Frame);
      LowFunc Low;
      Low.Entry = Function.Entry;
      Low.ExceptionMetadata = Function.ExceptionMetadata;
      Arch Architecture = Arch::X86;
      BinaryFormat Format = BinaryFormat::COFF;
      auto &Root = Function.Blocks[1].Ops.front();
      switch (Mutation) {
      case 1:
        Function.Blocks[0].Succs = {2};
        Function.Blocks[2].Preds.push_back(0);
        break;
      case 2:
        Low.OrdinaryModuleAnalysisRoots.insert(0x1100);
        break;
      case 3:
        ++Low.Entry;
        break;
      case 4:
        Root.Output.Size = 2;
        break;
      case 5:
        Root.Inputs[0].RegOff = x86reg::RSP;
        break;
      case 6:
        Root.OriginSeq = 9;
        break;
      case 7:
        Function.Blocks[1].Ops.insert(Function.Blocks[1].Ops.begin(), Frame);
        break;
      case 8:
        Root.RegistrationRoot = MedOp::RegistrationRootKind::None;
        break;
      case 9:
        ++Root.Addr;
        break;
      case 10:
        ++Function.RegistrationStates->SEHInvocationExits.front().TargetVA;
        break;
      case 11:
        Function.RegistrationStates->Blocks[2].Unknown = true;
        break;
      case 12:
        Low.ExceptionMetadata->Registration->RegistrationOffset = -20;
        break;
      case 13:
        Architecture = Arch::X64;
        break;
      case 14:
        Format = BinaryFormat::ELF;
        break;
      case 15:
        Function.Blocks[2].Preds = {0};
        break;
      case 16:
        Function.Blocks[2].Preds.push_back(1);
        break;
      case 17:
        Function.Blocks[2].ExceptionalPreds.emplace_back();
        break;
      case 18:
        Function.RegistrationStates->SEHParentChainComplete = false;
        break;
      case 19:
        std::reverse(Function.Blocks.begin(), Function.Blocks.end());
        break;
      }
      x86_registration::RegistrationRoots(Low, Architecture, Format)
          .seedSEHParentFrames(Function);
      const auto Target = llvm::find_if(
          Function.Blocks, [](const auto &Block) { return Block.Id == 2; });
      ASSERT_NE(Target, Function.Blocks.end());
      const bool Seeded = Mutation == 0 || Mutation == 18 || Mutation == 19;
      ASSERT_EQ(Target->Ops.size(), Seeded ? 1u : 0u);
      if (Seeded) {
        EXPECT_TRUE(hasValidRegistrationRootShape(Target->Ops.front()));
        EXPECT_EQ(Target->Ops.front().Output, Frame.Output);
        EXPECT_EQ(Target->Ops.front().Addr, Target->StartAddr);
      }
    }
  }
}

TEST(RegistrationSEHParentExit, RestoreMustDominateTheBranchInItsOwnCallback) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = parentExit(true);
    auto &States = *Function.RegistrationStates;
    MedBlock Jump = Function.Blocks[1];
    Jump.Id = 3;
    Jump.StartAddr = 0x1803;
    Jump.Ops.erase(Jump.Ops.begin());
    Function.Blocks[1].Ops.pop_back();
    Function.Blocks[1].EndAddr = 0x1803;
    Function.Blocks[1].Succs = {3};
    States.Blocks[1].Range.End = 0x1803;
    auto JumpState = States.Blocks[1];
    JumpState.BlockId = 3;
    JumpState.Range = {0x1803, 0x1805};
    States.Blocks.push_back(JumpState);
    Function.Blocks.push_back(Jump);
    if (Mutation == 1)
      Function.ExceptionMetadata->Registration->Scopes.front().HandlerVA =
          States.SEHInvocationExits.front().EntryVA = 0x1803;
    if (Mutation == 2)
      Function.Blocks[1].Succs = {1};
    if (Mutation == 3) {
      MedBlock Bypass = Function.Blocks[1];
      Bypass.Id = 4;
      Bypass.StartAddr = 0x1700;
      Bypass.EndAddr = 0x1702;
      Bypass.Ops.clear();
      Bypass.Succs = {1, 3};
      Function.Blocks.push_back(Bypass);
      auto State = States.Blocks[1];
      State.BlockId = 4;
      State.Range = {0x1700, 0x1702};
      States.Blocks.push_back(State);
      Function.ExceptionMetadata->Registration->Scopes.front().HandlerVA =
          States.SEHInvocationExits.front().EntryVA = 0x1700;
    }
    EXPECT_EQ(bool(getRegistrationSEHParentExits(Function)), Mutation == 0);
  }
}

TEST(RegistrationSEHParentExit, InputPE32BindsItsPhysicalParentExit) {
  const char *Path = std::getenv("NEVERD_SEH_PARENT_EXIT_INPUT_PE32");
  if (!Path)
    GTEST_SKIP()
        << "set NEVERD_SEH_PARENT_EXIT_INPUT_PE32 to the source fixture";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto *Export = Image->findExport("registration_entry");
  ASSERT_TRUE(Export);
  Decoder Decoder;
  ASSERT_TRUE(Decoder.init(*Image));
  auto Low = CFGBuilder().build(*Image, Decoder, Export->Addr, "parent_exit");
  ASSERT_TRUE(Low.RegistrationStates);
  ASSERT_TRUE(Low.RegistrationStates->Complete);
  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  const auto Exits = getRegistrationSEHParentExits(Med);
  ASSERT_TRUE(Exits);
  ASSERT_FALSE(Exits->empty());
  const auto Owners = registrationSEHCallbackOwners(Med);
  ASSERT_TRUE(Owners);
  for (const auto &[Block, Exit] : *Exits) {
    EXPECT_EQ(Owners->at(Block), std::make_pair(Exit.EntryVA, true));
    EXPECT_FALSE(Owners->count(Exit.TargetBlock));
  }
}

TEST(RegistrationSEHParentExit, InputPE32RejectsAnAbandonedUnwindGuard) {
  const char *Path = std::getenv("NEVERD_SEH_UNREPAIRED_PARENT_INPUT_PE32");
  if (!Path)
    GTEST_SKIP()
        << "set NEVERD_SEH_UNREPAIRED_PARENT_INPUT_PE32 to the control";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto *Export = Image->findExport("registration_entry");
  ASSERT_TRUE(Export);
  Decoder Decoder;
  ASSERT_TRUE(Decoder.init(*Image));
  auto Low = CFGBuilder().build(*Image, Decoder, Export->Addr, "parent_exit");
  ASSERT_TRUE(Low.RegistrationStates);
  EXPECT_TRUE(Low.RegistrationStates->Complete);
  EXPECT_TRUE(Low.RegistrationStates->ChainOperationsComplete);
  EXPECT_TRUE(Low.RegistrationStates->SEHInvocationExitsComplete);
  EXPECT_FALSE(Low.RegistrationStates->SEHParentChainComplete);
  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(getRegistrationSEHParentExits(Med));
  EXPECT_FALSE(x86_registration::prepareSEHParentExits(Med, {}, {}));
}

TEST(RegistrationSEHParentExit, NativeChainGatePreservesAnalysisOwnership) {
  auto Source = parentExit(false);
  llvm::LLVMContext Context;
  llvm::Module Module("parent-chain", Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
      llvm::GlobalValue::ExternalLinkage, "parent", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  auto *Finally = llvm::BasicBlock::Create(Context, "finally", Function);
  auto *Resume = llvm::BasicBlock::Create(Context, "resume", Function);
  llvm::IRBuilder<> Builder(Entry);
  Builder.CreateBr(Finally);
  Builder.SetInsertPoint(Finally);
  Builder.CreateBr(Resume);
  Builder.SetInsertPoint(Resume);
  Builder.CreateRetVoid();
  const std::map<int, llvm::BasicBlock *> Blocks{
      {0, Entry}, {1, Finally}, {2, Resume}};
  ASSERT_TRUE(x86_registration::prepareSEHParentExits(Source, Blocks, {}));
  Source.RegistrationStates->SEHParentChainComplete = false;
  ASSERT_TRUE(getRegistrationSEHParentExits(Source));
  EXPECT_TRUE(getRegistrationSEHReturns(Source));
  EXPECT_TRUE(registrationSEHCallbackOwners(Source));
  EXPECT_FALSE(x86_registration::prepareSEHParentExits(Source, Blocks, {}));
  const auto &Exit = Source.RegistrationStates->SEHInvocationExits.front();
  coff_registration::RegistrationSEHParentExitContract Contract;
  Contract.Transfers.emplace(
      1, coff_registration::RegistrationSEHParentExitContract::Transfer{
             Finally, Exit.EntryVA, Exit.Address});
  auto Error = coff_registration::bindSEHParentExitSources(Contract, Source);
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
}

TEST(RegistrationSEHParentExit, FailedOutliningRestoresTheOriginalModule) {
  for (bool ExistingIntrinsic : {false, true})
    for (bool Fallthrough : {false, true}) {
      SCOPED_TRACE(ExistingIntrinsic);
      SCOPED_TRACE(Fallthrough);
      auto Source = Fallthrough ? parentFallthrough(false) : parentExit(false);
      llvm::LLVMContext Context;
      llvm::Module Module("parent-rollback", Context);
      Module.setDataLayout("e-p:32:32");
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
          llvm::GlobalValue::ExternalLinkage, "parent", Module);
      auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
      auto *Finally = llvm::BasicBlock::Create(Context, "finally", Function);
      auto *Resume = llvm::BasicBlock::Create(Context, "resume", Function);
      llvm::IRBuilder<> Builder(Entry);
      Builder.CreateBr(Finally);
      Builder.SetInsertPoint(Finally);
      std::map<std::pair<va_t, int>, llvm::Instruction *> ChainIR;
      if (Fallthrough)
        ChainIR.emplace(std::make_pair(0x1803, 0),
                        Builder.CreateStore(Builder.getInt32(7),
                                            llvm::ConstantPointerNull::get(
                                                Builder.getPtrTy(257))));
      Builder.CreateBr(Resume);
      Builder.SetInsertPoint(Resume);
      Builder.CreateRetVoid();
      if (ExistingIntrinsic)
        llvm::Intrinsic::getOrInsertDeclaration(
            &Module, llvm::Intrinsic::eh_sjlj_longjmp);
      const std::map<int, llvm::BasicBlock *> Blocks{
          {0, Entry}, {1, Finally}, {2, Resume}};
      auto Plan =
          x86_registration::prepareSEHParentExits(Source, Blocks, {}, &ChainIR);
      ASSERT_TRUE(Plan);
      const auto Text = [&] {
        std::string Buffer;
        llvm::raw_string_ostream Stream(Buffer);
        Module.print(Stream, nullptr);
        return Buffer;
      };
      const auto Original = Text();
      {
        x86_registration::SEHParentExitProjection Projection(
            *Function, Source.Entry, *Plan);
        EXPECT_NE(Finally->getTerminator()->getSuccessor(0), Resume);
        if (Fallthrough)
          EXPECT_EQ(ChainIR.begin()->second->getParent(), nullptr);
        ASSERT_EQ(Projection.calls().size(), 1u);
        EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
        Projection.rollback();
        Projection.rollback();
      }
      EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      EXPECT_EQ(Text(), Original);
    }
}
