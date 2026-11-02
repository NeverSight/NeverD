//===- RegistrationSEHRegisterInputTests.cpp - Finally input uses -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Distinguish observable register results from unused finally transport.

#include "gtest/gtest.h"

#include "neverd/ir/med/X86/X86RegistrationSEHCall.h"
#include "neverd/ir/med/X86/X86RegistrationSEHRegisters.h"

#include <algorithm>

using namespace neverd;
using namespace neverd::x86_registration;

namespace {

MedVar registerValue(uint64_t Offset, int Version) {
  MedVar Value;
  Value.Kind = MedVar::Reg;
  Value.TheArch = Arch::X86;
  Value.Id = 20;
  Value.RegOff = Offset;
  Value.Size = 4;
  Value.SSAVer = Version;
  return Value;
}

MedOp exchange(SEHRegisterRole Role, va_t Address, MedVar Value) {
  const bool Read = Role == SEHRegisterRole::CallbackInput ||
                    Role == SEHRegisterRole::CallOutput;
  MedOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Addr = Address;
  Op.addInput(MedVar::makeConst(unsigned(Read ? Intrinsic::X86SEHRegisterRead
                                              : Intrinsic::X86SEHRegisterWrite),
                                2));
  Op.addInput(MedVar::makeConst(unsigned(Role), 1));
  Op.addInput(MedVar::makeConst(0x1800, 4));
  Op.addInput(MedVar::makeConst(Address, 4));
  Op.addInput(MedVar::makeConst(Value.RegOff, 2));
  if (Read)
    Op.Output = Value;
  else
    Op.addInput(Value);
  return Op;
}

MedFunc roundTrip(uint64_t Register) {
  MedFunc Function;
  Function.Entry = 0x1000;
  auto &EH = Function.ExceptionMetadata.emplace();
  EH.CodeRange = {0x1000, 0x1900};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
  EH.Registration.emplace().Scopes = {{-1, 0, 0x1800, true}};
  auto &States = Function.RegistrationStates.emplace();
  States.Complete = States.CallbackStatesComplete =
      States.RegistrationLifetimeComplete = true;
  const va_t Starts[] = {0x1000, 0x1800, 0x1005};
  const va_t Ends[] = {0x1005, 0x1801, 0x1006};
  for (int Id = 0; Id != 3; ++Id) {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Starts[Id];
    Block.EndAddr = Ends[Id];
    Function.Blocks.push_back(Block);
    RegistrationBlockState State;
    State.BlockId = Id;
    State.Range = {Starts[Id], Ends[Id]};
    State.Reached = true;
    State.CallbackOnly = Id == 1;
    States.Blocks.push_back(State);
  }
  auto &Caller = Function.Blocks[0];
  Caller.Succs = {2};
  Caller.Ops.push_back(
      exchange(SEHRegisterRole::CallInput, 0x1000, registerValue(Register, 0)));
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x1000;
  Call.OriginSeq = 0;
  Call.addInput(MedVar::makeConst(0x1800, 4));
  Caller.Ops.push_back(Call);
  Caller.Ops.push_back(exchange(SEHRegisterRole::CallOutput, 0x1000,
                                registerValue(Register, 2)));
  auto &Callback = Function.Blocks[1];
  Callback.Ops.push_back(exchange(SEHRegisterRole::CallbackInput, 0x1800,
                                  registerValue(Register, 1)));
  Callback.Ops.push_back(exchange(SEHRegisterRole::CallbackOutput, 0x1800,
                                  registerValue(Register, 1)));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = 0x1800;
  Return.OriginSeq = 0;
  Callback.Ops.push_back(Return);
  Return.Addr = 0x1005;
  Function.Blocks[2].Ops.push_back(Return);
  RegistrationSEHInvocationExit Exit;
  Exit.Exit = RegistrationSEHInvocationExit::Kind::NearReturn;
  Exit.EntryVA = 0x1800;
  Exit.CallVA = 0x1000;
  Exit.Address = 0x1800;
  Exit.EndAddress = 0x1801;
  Exit.OpSeq = 0;
  Exit.TargetVA = 0x1005;
  States.SEHInvocationExits.push_back(Exit);
  return Function;
}

} // namespace

TEST(RegistrationSEHRegisterInputs, ReturnedCopiesRequireAnActualObserver) {
  for (uint64_t Register : SEHDataRegisters)
    for (bool Reverse : {false, true})
      for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
        SCOPED_TRACE(Register);
        SCOPED_TRACE(Reverse);
        SCOPED_TRACE(Mutation);
        auto Function = roundTrip(Register);
        ASSERT_TRUE(getRegistrationSEHReturns(Function));
        auto &Caller = Function.Blocks[0];
        auto &Callback = Function.Blocks[1];
        if (Mutation == 1)
          Function.Blocks[2].Ops.back().addInput(registerValue(Register, 2));
        if (Mutation == 2 || Mutation == 3) {
          MedOp Use;
          Use.Addr = 0x1800;
          Use.Opcode = Mutation == 2 ? NdOp::STORE : NdOp::CALL;
          Use.addInput(MedVar::makeConst(0x5000, 4));
          if (Mutation == 2)
            Use.addInput(registerValue(Register, 1));
          Callback.Ops.insert(Callback.Ops.end() - 1, Use);
        }
        if (Mutation == 4)
          ++Function.RegistrationStates->SEHInvocationExits.front().OpSeq;
        if (Mutation == 5)
          Caller.Ops.back().Inputs[2].ConstVal = 0x1900;
        if (Mutation == 6)
          Callback.Ops[1].Inputs[2].ConstVal = 0x1900;
        if (Mutation == 7)
          Callback.Ops.erase(Callback.Ops.begin() + 1);
        if (Mutation == 8 || Mutation >= 12) {
          PhiNode Phi;
          Phi.Output = registerValue(Register, 3);
          if (Mutation == 8 || Mutation == 13)
            Phi.ExceptionalEntry = registerValue(Register, 2);
          else
            Phi.Args.emplace_back(0, registerValue(Register, 2));
          if (Mutation == 8 || Mutation == 14)
            Function.Blocks[2].Ops.back().addInput(Phi.Output);
          Function.Blocks[2].Phis.push_back(Phi);
        }
        if (Mutation == 9)
          Function.RegistrationStates->Blocks[1].Unknown = true;
        if (Mutation == 10) {
          Function.CallInfos.emplace_back();
          Function.CallInfos.back().Args.push_back(registerValue(Register, 2));
        }
        if (Mutation == 11) {
          MedCallClobber Clobber;
          Clobber.PreservedPrefixSize = 4;
          Clobber.PreservedInput = registerValue(Register, 1);
          Function.CallClobbers.push_back(Clobber);
        }
        if (Reverse)
          std::reverse(Function.Blocks.begin(), Function.Blocks.end());
        discardUnusedSEHCallInputs(Function);
        const auto &Input = Function.Blocks[Reverse ? 2 : 0].Ops.front();
        EXPECT_EQ(Input.Inputs[5].Kind == MedVar::Unspecified,
                  Mutation == 0 || Mutation == 12 || Mutation == 13);
      }
}

TEST(RegistrationSEHRegisterInputs, ScalarNearReturnSharesTheRegisterObserver) {
  for (bool Reverse : {false, true})
    for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
      SCOPED_TRACE(Reverse);
      SCOPED_TRACE(Mutation);
      auto Function = roundTrip(x86reg::RAX);
      auto &Caller = Function.Blocks[0];
      auto &Callback = Function.Blocks[1];
      Caller.Ops[1].Output = registerValue(x86reg::RAX, 4);
      Callback.Ops.back().addInput(registerValue(x86reg::RAX, 1));
      switch (Mutation) {
      case 1:
        Caller.Ops[1].Output.Size = 0;
        break;
      case 2:
        Function.Blocks[2].Ops.back().addInput(registerValue(x86reg::RAX, 2));
        break;
      case 3:
        Function.Blocks[2].Ops.back().addInput(Caller.Ops[1].Output);
        break;
      case 4:
        Callback.Ops.back().addInput(MedVar::makeConst(0, 4));
        break;
      case 5:
        Callback.Ops.back().MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      case 6:
        Callback.Ops.back().MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        break;
      case 7:
        Callback.Ops[1].Inputs[4].ConstVal = x86reg::RCX;
        break;
      case 8:
        Callback.Ops.insert(Callback.Ops.end() - 1, Callback.Ops[1]);
        break;
      case 9:
        Callback.Ops.erase(Callback.Ops.begin() + 1);
        break;
      case 10:
        ++Function.RegistrationStates->SEHInvocationExits.front().OpSeq;
        break;
      case 11:
        Caller.Ops[1].Output.Size = 2;
        break;
      case 12:
        Caller.Ops[1].Output.SSAVer = 0;
        break;
      case 13:
        Function.CallInfos.emplace_back();
        Function.CallInfos.back().Args.push_back(Caller.Ops[1].Output);
        break;
      }
      if (Reverse)
        std::reverse(Function.Blocks.begin(), Function.Blocks.end());
      discardUnusedSEHCallInputs(Function);
      const auto &Input = Function.Blocks[Reverse ? 2 : 0].Ops.front();
      EXPECT_EQ(Input.Inputs[5].Kind == MedVar::Unspecified, Mutation <= 1);
    }
}
