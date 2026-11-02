//===- RegistrationSEHLocalUnwindTests.cpp - Local unwind call binding ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep source identity, runtime ABI and returning-state evidence together.

#include "gtest/gtest.h"

#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86/X86RegistrationSEHLocalUnwind.h"
#include "neverd/lift/X86/X86Regs.h"

using namespace neverd;

namespace {

MedVar makeReg(int Id, uint64_t Offset) {
  MedVar Value;
  Value.Kind = MedVar::Reg;
  Value.Id = Id;
  Value.SSAVer = 1;
  Value.Size = 4;
  Value.RegOff = Offset;
  Value.TheArch = Arch::X86;
  return Value;
}

MedOp operation(NdOp Opcode, MedVar Output,
                std::initializer_list<MedVar> Inputs) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

MedFunc localUnwind(bool EH4, bool Callback, bool Indirect) {
  MedFunc Function;
  Function.Entry = 0x1000;
  auto &EH = Function.ExceptionMetadata.emplace();
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
  auto &States = Function.RegistrationStates.emplace();
  States.Complete = States.CallbackStatesComplete =
      States.RegistrationLifetimeComplete = States.SecurityCookiesComplete =
          States.HasSEHLocalUnwinds = true;
  States.SecurityCookieVA = 0x4000;
  for (int Id = 0; Id != 2; ++Id) {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Id ? 0x1800 : 0x1000;
    Block.EndAddr = Block.StartAddr + 0x40;
    Function.Blocks.push_back(Block);
    RegistrationBlockState State;
    State.BlockId = Id;
    State.Range = {Block.StartAddr, Block.EndAddr};
    State.Reached = true;
    State.CallbackOnly = Id == 1;
    State.ExplicitLevels = {0};
    States.Blocks.push_back(State);
  }
  RegistrationSEHLocalUnwind Call;
  Call.Address = Callback ? 0x1830 : 0x1030;
  Call.EndAddress = Call.Address + (Indirect ? 6 : 5);
  Call.OpSeq = 7;
  Call.Runtime = {Indirect ? 0x5000u : 0x2000u, Indirect,
                  EH4 ? RegistrationLocalUnwindContract::Kind::EH4
                      : RegistrationLocalUnwindContract::Kind::EH3,
                  0x5000};
  Call.StackOffset = -52;
  Call.EntryVA = Callback ? 0x1800 : 0;
  Call.Bound = uint32_t(Outside);
  Call.Transitions = {{0, Outside}};
  States.SEHLocalUnwinds.push_back(Call);
  auto Op = operation(Indirect ? NdOp::INDIR_CALL : NdOp::CALL,
                      makeReg(20, x86reg::RAX),
                      {MedVar::makeConst(Call.Runtime.Target, 4)});
  Op.Addr = Call.Address;
  Op.OriginSeq = Call.OpSeq;
  Op.CallSiteId = 1;
  Function.Blocks[Callback].Ops.push_back(Op);
  return Function;
}

} // namespace

TEST(RegistrationSEHLocalUnwind, BindsOnlyCompleteCurrentOccurrences) {
  for (bool EH4 : {false, true})
    for (bool Callback : {false, true})
      for (bool Indirect : {false, true})
        for (unsigned Mutation = 0; Mutation != 24; ++Mutation) {
          SCOPED_TRACE(EH4);
          SCOPED_TRACE(Callback);
          SCOPED_TRACE(Indirect);
          SCOPED_TRACE(Mutation);
          auto Function = localUnwind(EH4, Callback, Indirect);
          auto &States = *Function.RegistrationStates;
          auto &Call = States.SEHLocalUnwinds.front();
          auto &Block = Function.Blocks[Callback];
          auto &Op = Block.Ops.front();
          switch (Mutation) {
          case 1:
            States.Complete = false;
            break;
          case 2:
            States.SEHLocalUnwindsComplete = false;
            break;
          case 3:
            States.HasSEHLocalUnwinds = false;
            break;
          case 4:
            Call.Runtime.ImportSlot = 0;
            break;
          case 5:
            Call.StackOffset = -16;
            break;
          case 6:
            ++Call.EndAddress;
            break;
          case 7:
            ++Call.OpSeq;
            break;
          case 8:
            ++Call.EntryVA;
            break;
          case 9:
            Call.Transitions = {{1, -1}};
            break;
          case 10:
            Call.Transitions.clear();
            break;
          case 11:
            Call.Transitions.push_back(Call.Transitions.front());
            break;
          case 12:
            Call.Transitions = {{0, 0}};
            break;
          case 13:
            Op.OriginSeq = -1;
            break;
          case 14:
            Op.Dead = true;
            break;
          case 15:
            Op.Inputs[0].ConstVal += 1;
            break;
          case 16:
            Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
            break;
          case 17:
            Op.MemoryOrdering = NdMemoryOrdering::Acquire;
            break;
          case 18:
            Function.SkippedSSA = true;
            break;
          case 19:
            Block.Ops.push_back(Op);
            break;
          case 20:
            Block.Ops.push_back(operation(NdOp::RETURN, {}, {Op.Output}));
            break;
          case 21:
            States.CallbackStatesComplete = false;
            break;
          case 22:
            States.RegistrationLifetimeComplete = false;
            break;
          case 23:
            Function.ExceptionMetadata->ParseStatus =
                ExceptionParseStatus::Partial;
            break;
          }
          const auto Calls = getX86RegistrationSEHLocalUnwinds(Function);
          ASSERT_EQ(bool(Calls), Mutation == 0);
          if (Calls) {
            ASSERT_EQ(Calls->size(), 1u);
            EXPECT_EQ(Calls->at(Call.Address).Operation, &Op);
            EXPECT_EQ(Calls->at(Call.Address).Block, Block.Id);
          }
        }
}

TEST(RegistrationSEHLocalUnwind, RetainsUnsignedPartialUnwindStopConditions) {
  for (bool EH4 : {false, true}) {
    auto Function = localUnwind(EH4, false, false);
    auto &Call = Function.RegistrationStates->SEHLocalUnwinds.front();
    Call.Transitions = {{0, 0}};
    for (uint32_t Bound : {0u, 1u, 0x80000000u}) {
      Call.Bound = Bound;
      EXPECT_TRUE(getX86RegistrationSEHLocalUnwinds(Function));
    }
    Call.Bound = uint32_t(EH4 ? -2 : -1);
    EXPECT_FALSE(getX86RegistrationSEHLocalUnwinds(Function));
  }
}

TEST(RegistrationSEHLocalUnwind, CookieIdentityRemainsRequiredForEH4) {
  auto Function = localUnwind(true, false, false);
  Function.RegistrationStates->SecurityCookiesComplete = false;
  EXPECT_FALSE(getX86RegistrationSEHLocalUnwinds(Function));
  Function.RegistrationStates->SecurityCookiesComplete = true;
  Function.RegistrationStates->SecurityCookieVA = 0;
  EXPECT_FALSE(getX86RegistrationSEHLocalUnwinds(Function));
}

TEST(RegistrationSEHLocalUnwind, RuntimeABIExcludesIncidentalRegisterValues) {
  for (bool EH4 : {false, true})
    for (bool Indirect : {false, true}) {
      auto Function = localUnwind(EH4, false, Indirect);
      auto &Ops = Function.Blocks.front().Ops;
      const MedOp Call = Ops.front();
      Ops.clear();
      for (const auto &[Id, Offset] :
           {std::pair{1, x86reg::RCX}, {2, x86reg::RDX}, {3, x86reg::XMM0}})
        Ops.push_back(operation(NdOp::COPY, makeReg(Id, Offset),
                                {MedVar::makeConst(88, 4)}));
      const auto SP = makeReg(4, x86reg::RSP);
      auto IncomingSP = SP;
      IncomingSP.SSAVer = 0;
      Ops.push_back(
          operation(NdOp::INT_SUB, SP, {IncomingSP, MedVar::makeConst(52, 4)}));
      const unsigned Words = EH4 ? 3 : 2;
      for (unsigned I = 0; I != Words + 1; ++I) {
        auto Address = makeReg(10 + I, 0);
        Address.Kind = MedVar::Temp;
        Ops.push_back(operation(NdOp::INT_ADD, Address,
                                {SP, MedVar::makeConst(I * 4, 4)}));
        Ops.push_back(operation(NdOp::STORE, {},
                                {Address, MedVar::makeConst(100 + I, 4)}));
      }
      Ops.push_back(Call);
      ASSERT_TRUE(getX86RegistrationSEHLocalUnwinds(Function));
      BinaryImage Image;
      Image.Arch = Arch::X86;
      Image.Bits = Bitness::Bits32;
      Image.Format = BinaryFormat::COFF;
      recoverCallAbi(Function, Arch::X86, {}, &Image);
      ASSERT_EQ(Function.CallInfos.size(), 1u);
      const auto &Info = Function.CallInfos.front();
      ASSERT_EQ(Info.Args.size(), Words);
      EXPECT_EQ(Info.IntegerRegisterArgCount, 0);
      EXPECT_EQ(Info.FloatingRegisterArgCount, 0);
      for (unsigned I = 0; I != Words; ++I)
        EXPECT_EQ(Info.Args[I].ConstVal, 100 + I);
    }
}
