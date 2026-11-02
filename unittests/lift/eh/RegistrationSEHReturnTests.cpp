//===- RegistrationSEHReturnTests.cpp - Finally CALL/RET contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/X86/X86RegistrationSEHControl.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86/X86RegistrationSEHCall.h"
#include "neverd/lift/X86/X86Regs.h"

namespace {
using namespace neverd;

MedFunc returningFinally() {
  MedFunc Function;
  Function.ExceptionMetadata.emplace();
  Function.ExceptionMetadata->ParseStatus = ExceptionParseStatus::Complete;
  Function.ExceptionMetadata->Encoding = ExceptionEncoding::X86ScopeTableEH3;
  Function.ExceptionMetadata->Registration.emplace();
  Function.ExceptionMetadata->Registration->Scopes.push_back(
      {-1, 0, 0x1040, true});
  Function.RegistrationStates.emplace();
  Function.RegistrationStates->Complete = true;
  Function.RegistrationStates->CallbackStatesComplete = true;
  Function.RegistrationStates->RegistrationLifetimeComplete = true;
  Function.Blocks.resize(3);
  auto &Call = Function.Blocks[0];
  Call.Id = 1;
  Call.StartAddr = 0x1000;
  Call.EndAddr = 0x1005;
  Call.Succs = {2};
  MedOp Invoke;
  Invoke.Opcode = NdOp::CALL;
  Invoke.Addr = 0x1000;
  Invoke.OriginSeq = 0;
  Invoke.NumInputs = 1;
  Invoke.Inputs[0] = MedVar::makeConst(0x1040, 4);
  Call.Ops.push_back(Invoke);
  auto &Continuation = Function.Blocks[1];
  Continuation.Id = 2;
  Continuation.StartAddr = 0x1005;
  Continuation.EndAddr = 0x1006;
  auto &Callback = Function.Blocks[2];
  Callback.Id = 3;
  Callback.StartAddr = 0x1040;
  Callback.EndAddr = 0x1041;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = 0x1040;
  Return.OriginSeq = 2;
  Callback.Ops.push_back(Return);
  RegistrationSEHInvocationExit Exit;
  Exit.EntryVA = 0x1040;
  Exit.CallVA = 0x1000;
  Exit.Address = 0x1040;
  Exit.EndAddress = 0x1041;
  Exit.OpSeq = 2;
  Exit.TargetVA = 0x1005;
  Exit.StackOffset = -32;
  Function.RegistrationStates->SEHInvocationExits.push_back(Exit);
  return Function;
}

TEST(RegistrationSEHReturn, BindsPhysicalReturnToItsCallingContinuation) {
  auto Function = returningFinally();
  const auto Returns = getX86RegistrationSEHReturnBlocks(Function);
  ASSERT_TRUE(Returns);
  EXPECT_EQ(*Returns, (std::map<int, va_t>{{3, 0x1040}}));
  std::reverse(Function.Blocks.begin(), Function.Blocks.end());
  EXPECT_EQ(getX86RegistrationSEHReturnBlocks(Function), Returns);
}

TEST(RegistrationSEHReturn, RejectsChangedInvocationAndContinuationIdentities) {
  for (unsigned Mutation = 0; Mutation != 20; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = returningFinally();
    auto &States = *Function.RegistrationStates;
    auto &Exit = States.SEHInvocationExits.front();
    switch (Mutation) {
    case 0:
      States.SEHInvocationExitsComplete = false;
      break;
    case 1:
      Exit.Exit = RegistrationSEHInvocationExit::Kind::ParentJump;
      break;
    case 2:
      Exit.CallVA = 0;
      break;
    case 3:
      Exit.EntryVA += 1;
      break;
    case 4:
      Exit.OpSeq += 1;
      break;
    case 5:
      Exit.EndAddress += 1;
      break;
    case 6:
      Exit.TargetVA += 1;
      break;
    case 7:
      Exit.StackRestoreVA = Exit.Address;
      break;
    case 8:
      Function.Blocks[0].Succs.push_back(3);
      break;
    case 9:
      Function.Blocks[2].Ops.front().Opcode = NdOp::BRANCH;
      break;
    case 10:
      Function.Blocks[0].Ops.front().Inputs[0].ConstVal += 1;
      break;
    case 11:
      States.SEHInvocationExits.push_back(Exit);
      break;
    case 12:
      States.Complete = false;
      break;
    case 13:
      States.CallbackStatesComplete = false;
      break;
    case 14:
      States.RegistrationLifetimeComplete = false;
      break;
    case 15:
      Function.Blocks[0].Ops.front().MemoryAddressSpace =
          NdMemoryAddressSpace::X86FS;
      break;
    case 16:
      Function.Blocks[0].Ops.front().DoesNotReturn = true;
      break;
    case 17:
      Function.Blocks[0].Ops.front().ExactArguments = true;
      break;
    case 18:
      Function.Blocks[0].Ops.front().OriginSeq = -1;
      break;
    case 19:
      Function.Blocks[2].Ops.front().Dead = true;
      break;
    }
    EXPECT_FALSE(getX86RegistrationSEHReturnBlocks(Function));
  }
}

TEST(RegistrationSEHReturn, OneReturnCanServeDistinctOrdinaryInvocations) {
  auto Function = returningFinally();
  MedBlock Caller = Function.Blocks.front();
  Caller.Id = 4;
  Caller.StartAddr += 0x20;
  Caller.EndAddr += 0x20;
  Caller.Ops.front().Addr += 0x20;
  Function.Blocks.push_back(Caller);
  auto Exit = Function.RegistrationStates->SEHInvocationExits.front();
  Exit.CallVA += 0x20;
  Function.RegistrationStates->SEHInvocationExits.push_back(Exit);
  EXPECT_TRUE(getX86RegistrationSEHReturnBlocks(Function));
  const auto Returns = getRegistrationSEHReturns(Function);
  ASSERT_TRUE(Returns);
  EXPECT_EQ(Returns->ReturningCalls.size(), 2u);
  EXPECT_EQ(Returns->ReturningCalls.at({1, 0x1000, 0}), 0x1040u);
  EXPECT_EQ(Returns->ReturningCalls.at({4, 0x1020, 0}), 0x1040u);
}

TEST(RegistrationSEHReturn, KeepsInvocationStorageOutOfOrdinaryCallArguments) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Format = BinaryFormat::COFF;
  for (bool Complete : {false, true}) {
    SCOPED_TRACE(Complete);
    auto Function = returningFinally();
    Function.RegistrationStates->Complete = Complete;
    auto &Block = Function.Blocks.front();
    auto Invoke = Block.Ops.front();
    Block.Ops.clear();
    MedVar EntrySP;
    EntrySP.Kind = MedVar::Reg;
    EntrySP.Id = 1;
    EntrySP.RegOff = x86reg::RSP;
    EntrySP.Size = 4;
    EntrySP.TheArch = Arch::X86;
    MedVar PrivateSP = EntrySP;
    PrivateSP.SSAVer = 1;
    MedOp Push;
    Push.Opcode = NdOp::INT_SUB;
    Push.Output = PrivateSP;
    Push.addInput(EntrySP);
    Push.addInput(MedVar::makeConst(4, 4));
    Block.Ops.push_back(Push);
    MedOp Store;
    Store.Opcode = NdOp::STORE;
    Store.addInput(PrivateSP);
    Store.addInput(MedVar::makeConst(7, 4));
    Block.Ops.push_back(Store);
    Block.Ops.push_back(Invoke);
    // Generic cdecl recovery would treat the private PUSH as an argument,
    // even when an unrelated signature summary suggests one stack word.
    std::map<va_t, int> RegisterArity{{0x1040, 0}};
    std::map<va_t, int> TotalArity{{0x1040, 1}};
    recoverCallAbi(Function, Arch::X86, {}, &Image, &RegisterArity,
                   &TotalArity);
    ASSERT_EQ(Function.CallInfos.size(), 1u);
    const auto &Arguments = Function.CallInfos.front().Args;
    if (Complete) {
      EXPECT_TRUE(Arguments.empty());
    } else {
      ASSERT_EQ(Arguments.size(), 1u);
      EXPECT_TRUE(Arguments.front().isConst());
      EXPECT_EQ(Arguments.front().ConstVal, 7u);
    }
    // ABI recovery classifies storage; it must retain the actual source write.
    EXPECT_EQ(Block.Ops[1].Opcode, NdOp::STORE);
    EXPECT_EQ(Block.Ops[1].Inputs[1].ConstVal, 7u);
  }
}

} // namespace
