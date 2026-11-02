//===- RegistrationFrameScalarTests.cpp - PE32 relative scalar proofs ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve exact scalar results while rejecting ambiguous frame provenance.

#include "../../../lib/ir/low/X86/RegistrationFrame.h"
#include "../../../lib/ir/low/X86/RegistrationFrameBranch.h"
#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "gtest/gtest.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86/X86Regs.h"

namespace {
using namespace neverd;
using namespace neverd::registration_state;

FrameValue coordinate(unsigned Space, int32_t Offset) {
  return Space == 0   ? FrameValue::frame(Offset)
         : Space == 1 ? FrameValue::entryFrame(Offset)
                      : FrameValue::callbackFrame(0x1800, Offset);
}

LowOp operation(NdOp Opcode) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = NdVar::tmp(1, Opcode == NdOp::INT_SUB ? 4 : 1);
  Op.NumInputs = 2;
  Op.Inputs[0] = NdVar::reg(x86reg::RAX, 4);
  Op.Inputs[1] = NdVar::reg(x86reg::RCX, 4);
  return Op;
}

FrameValue evaluate(FrameValue Left, FrameValue Right, const LowOp &Op) {
  FrameState Frame;
  Frame.Registers[x86reg::RAX / x86reg::GeneralRegStride] = Left;
  Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride] = Right;
  return FrameTransfer(Frame, -16).evaluate(Op, true);
}

TEST(RegistrationFrameScalar, ConstantBranchesRequireBothPhysicalEdges) {
  for (bool Taken : {false, true})
    for (bool Reversed : {false, true})
      for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
        SCOPED_TRACE(Taken);
        SCOPED_TRACE(Reversed);
        SCOPED_TRACE(Mutation);
        FrameState State;
        auto &Condition =
            State.Registers[x86reg::RAX / x86reg::GeneralRegStride];
        Condition = FrameValue::constant(Taken);
        FrameTransfer Transfer(State, -16);
        LowBlock Block;
        Block.StartAddr = 0x1000;
        Block.EndAddr = 0x1010;
        Block.Succs =
            Reversed ? std::vector<int>{2, 1} : std::vector<int>{1, 2};
        LowOp Branch;
        Branch.Opcode = NdOp::COND_BR;
        Branch.NumInputs = 2;
        Branch.Inputs[0] = NdVar::cst(0x1020, 4);
        Branch.Inputs[1] = NdVar::reg(x86reg::RAX, 4);
        std::map<va_t, int> Entries{{0x1010, 1}, {0x1020, 2}};
        if (Mutation == 1)
          Entries.erase(0x1010);
        if (Mutation == 2)
          Entries.erase(0x1020);
        if (Mutation == 3)
          Entries[0x1020] = 1;
        if (Mutation == 4)
          Entries[0x1020] = 3;
        if (Mutation == 5)
          Branch.Inputs[0] = NdVar::reg(x86reg::RCX, 4);
        if (Mutation == 6)
          Condition.MayBeFrame = true;
        if (Mutation == 7)
          Condition = FrameValue::imageAddress(0x1000);
        if (Mutation == 8)
          Condition = {};
        if (Mutation == 10)
          Branch.NumInputs = 1;
        if (Mutation == 11)
          Block.Succs = {1, 1};
        if (Mutation != 9)
          Block.Ops.push_back(Branch);
        const auto Target = knownFrameBranchTarget(
            Block, Transfer, [&](va_t Address) -> std::optional<int> {
              const auto Found = Entries.find(Address);
              return Found == Entries.end() ? std::nullopt
                                            : std::optional<int>{Found->second};
            });
        if (Mutation == 0)
          EXPECT_EQ(Target, Taken ? 0x1020u : 0x1010u);
        else
          EXPECT_FALSE(Target);
      }
}

TEST(RegistrationFrameScalar, ExactCoordinatesPreserveRelocatedResults) {
  for (unsigned Space = 0; Space != 3; ++Space)
    for (int32_t Left : {-64, -16, 0, INT32_MIN, INT32_MAX})
      for (int32_t Right : {-64, -16, 0, INT32_MIN, INT32_MAX})
        for (NdOp Opcode :
             {NdOp::INT_SUB, NdOp::INT_EQUAL, NdOp::INT_NOTEQUAL}) {
          SCOPED_TRACE(::testing::Message()
                       << Space << ':' << Left << ':' << Right << ':'
                       << unsigned(Opcode));
          const auto Result =
              evaluate(coordinate(Space, Left), coordinate(Space, Right),
                       operation(Opcode));
          ASSERT_TRUE(Result.Constant);
          EXPECT_FALSE(Result.MayBeFrame);
          // Check the observable result at different native frame bases,
          // including wraparound in the PE32 integer address domain.
          for (uint32_t Base : {0u, 0x10000u, 0x14000000u, UINT32_MAX}) {
            const uint32_t L = Base + uint32_t(Left);
            const uint32_t R = Base + uint32_t(Right);
            const uint32_t Expected = Opcode == NdOp::INT_SUB     ? L - R
                                      : Opcode == NdOp::INT_EQUAL ? L == R
                                                                  : L != R;
            EXPECT_EQ(*Result.Constant, Expected);
          }
        }
}

TEST(RegistrationFrameScalar, AddressBitsAreNotAffineScalarConstants) {
  for (uint32_t Address : {0x401000u, 0x18001000u}) {
    FrameState State;
    FrameTransfer Transfer(State, -16);
    auto Input = NdVar::cst(Address, 4);
    const auto Numeric = Transfer.read(Input);
    ASSERT_EQ(Numeric.Constant, Address);
    for (auto Provenance : {ConstantAddressProvenance::Address,
                            ConstantAddressProvenance::DataAddress,
                            ConstantAddressProvenance::CodeAddress}) {
      Input.Provenance = Provenance;
      const auto Pointer = Transfer.read(Input);
      EXPECT_EQ(Pointer.ImageAddress, Address);
      EXPECT_FALSE(Pointer.Constant);
      EXPECT_NE(Pointer, Numeric);
      for (unsigned Space = 0; Space != 3; ++Space) {
        const auto Result =
            evaluate(coordinate(Space, -16), Pointer, operation(NdOp::INT_SUB));
        EXPECT_FALSE(Result.Offset);
        EXPECT_FALSE(Result.EntryOffset);
        EXPECT_FALSE(Result.CallbackAddress);
        EXPECT_TRUE(Result.MayBeFrame);
      }
      for (NdOp Opcode : {NdOp::INT_SUB, NdOp::INT_EQUAL, NdOp::INT_NOTEQUAL})
        EXPECT_FALSE(evaluate(Pointer, Numeric, operation(Opcode)).Constant);
      auto Add = operation(NdOp::INT_ADD);
      Add.Output.Size = 4;
      EXPECT_EQ(evaluate(Pointer, FrameValue::constant(4), Add).ImageAddress,
                Address + 4);
      EXPECT_EQ(evaluate(FrameValue::constant(4), Pointer, Add).ImageAddress,
                Address + 4);
      State.store(-32, 4, Pointer);
      EXPECT_EQ(State.load(-32, 4), Pointer);
      EXPECT_FALSE(State.load(-32, 2).ImageAddress);
      FrameState NumericState = State;
      NumericState.store(-32, 4, Numeric);
      EXPECT_NE(State, NumericState);
      EXPECT_NE(State.snapshotHash(), NumericState.snapshotHash());
    }
    Input.Provenance = ConstantAddressProvenance::AddressFragment;
    EXPECT_EQ(Transfer.read(Input), FrameValue{});
    Input.Provenance = ConstantAddressProvenance::DataAddress;
    for (uint16_t Size : {1, 2, 8}) {
      Input.Size = Size;
      EXPECT_EQ(Transfer.read(Input), FrameValue{});
    }
  }
}

TEST(RegistrationFrameScalar, IdenticalImageAddressesHaveExactRelations) {
  for (uint32_t Address : {0u, 0x401000u, 0x80000000u, UINT32_MAX})
    for (NdOp Opcode :
         {NdOp::INT_SUB, NdOp::INT_EQUAL, NdOp::INT_NOTEQUAL, NdOp::INT_LESS,
          NdOp::INT_LESSEQUAL, NdOp::INT_SLESS, NdOp::INT_SLESSEQUAL})
      for (unsigned Mutation = 0; Mutation != 18; ++Mutation) {
        SCOPED_TRACE(::testing::Message()
                     << Address << ':' << unsigned(Opcode) << ':' << Mutation);
        auto Left = FrameValue::imageAddress(Address);
        auto Right = Left;
        auto Op = operation(Opcode);
        if (Mutation == 1)
          Right = FrameValue::imageAddress(Address + 1);
        if (Mutation == 2)
          Right = FrameValue::constant(Address);
        if (Mutation == 3)
          Left = FrameValue::constant(Address);
        if (Mutation == 4)
          Right.MayBeFrame = true;
        if (Mutation == 5)
          Right.SecurityCookie = true;
        if (Mutation == 6)
          Right.Offset = 0;
        if (Mutation == 7)
          Right.ImportTarget = Address;
        if (Mutation == 8)
          Right.SavedRegister = 1;
        if (Mutation == 9)
          Op.Inputs[0].Size = 2;
        if (Mutation == 10)
          Op.Inputs[1].Size = 8;
        if (Mutation == 11)
          Op.Output.Size = Opcode == NdOp::INT_SUB ? 1 : 4;
        if (Mutation == 12)
          Op.NumInputs = 1;
        if (Mutation == 13)
          Right = join(Right, {});
        if (Mutation == 14)
          Right.FrameOnlyFromCall = true;
        if (Mutation == 15)
          Right.CookieImageXOR = Address;
        if (Mutation == 16)
          Right.ThreadAddress = SEHThreadAddress{0x1000, 0, 0};
        if (Mutation == 17)
          Right.FrameOnlyFromSEHScalar = true;
        const auto Result = evaluate(Left, Right, Op);
        if (Mutation) {
          EXPECT_FALSE(Result.Constant);
        } else {
          const bool Reflexive = Opcode == NdOp::INT_EQUAL ||
                                 Opcode == NdOp::INT_LESSEQUAL ||
                                 Opcode == NdOp::INT_SLESSEQUAL;
          EXPECT_EQ(Result, FrameValue::constant(Reflexive));
          // Rebase, wraparound and movement of this one image object cannot
          // change a relation between two complete copies of its identity.
          FrameState Spilled;
          Spilled.store(-4, 4, Right);
          EXPECT_EQ(evaluate(Left, Spilled.load(-4, 4), Op), Result);
          Spilled.store(-3, 1, {});
          EXPECT_FALSE(evaluate(Left, Spilled.load(-4, 4), Op).Constant);
        }
      }
}

TEST(RegistrationFrameScalar, ImportedCallablesAreNotAddressesOrScalars) {
  const auto Callable = FrameValue::importTarget(0x403000);
  EXPECT_FALSE(Callable.Constant);
  EXPECT_FALSE(Callable.ImageAddress);
  EXPECT_FALSE(Callable.preferredAddress());
  EXPECT_FALSE(Callable.Offset);
  EXPECT_NE(Callable, FrameValue::constant(0x403000));
  EXPECT_NE(Callable, FrameValue::imageAddress(0x403000));
  auto Arithmetic = operation(NdOp::INT_ADD);
  Arithmetic.Output.Size = 4;
  EXPECT_FALSE(
      evaluate(Callable, FrameValue::constant(0), Arithmetic).ImportTarget);
  EXPECT_EQ(join(Callable, Callable), Callable);
  EXPECT_FALSE(join(Callable, FrameValue::importTarget(0x403004)).ImportTarget);
  EXPECT_FALSE(join(Callable, {}).ImportTarget);
  FrameState State;
  State.Registers[x86reg::RSI / x86reg::GeneralRegStride] = Callable;
  State.Registers[x86reg::RAX / x86reg::GeneralRegStride] = Callable;
  State.store(-32, 4, Callable);
  EXPECT_EQ(State.load(-32, 4), Callable);
  EXPECT_FALSE(State.load(-32, 2).ImportTarget);
  FrameState Changed = State;
  Changed.Registers[x86reg::RSI / x86reg::GeneralRegStride] = {};
  EXPECT_NE(State.snapshotHash(), Changed.snapshotHash());
  FrameTransfer Transfer(State, -16);
  LowOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.addInput(NdVar::cst(0x403000, 4));
  Transfer.write(Call, {}, 0, true);
  EXPECT_FALSE(State.ImportBindingsIntact);
  EXPECT_EQ(State.Registers[x86reg::RSI / x86reg::GeneralRegStride], Callable);
  EXPECT_FALSE(
      State.Registers[x86reg::RAX / x86reg::GeneralRegStride].ImportTarget);
  Changed = State;
  Changed.ImportBindingsIntact = true;
  EXPECT_NE(State.snapshotHash(), Changed.snapshotHash());
  EXPECT_TRUE(Changed.merge(State));
  EXPECT_FALSE(Changed.ImportBindingsIntact);
}

TEST(RegistrationFrameScalar,
     OpaqueCallsCannotCertifyCachedTargetPreservation) {
  FrameState State;
  State.Registers[x86reg::RSI / x86reg::GeneralRegStride] =
      FrameValue::importTarget(0x403000);
  FrameTransfer Transfer(State, -16);
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(NdVar::cst(0x401000, 4));
  // A returning-stack fact for this helper does not prove that its body
  // preserves ESI. Keep other frame facts separate from cached-call identity.
  Transfer.write(Call, {});
  EXPECT_FALSE(
      State.Registers[x86reg::RSI / x86reg::GeneralRegStride].ImportTarget);
}

TEST(RegistrationFrameScalar, ImportPreservationDoesNotRestoreLostFacts) {
  for (bool Intact : {false, true})
    for (bool PreserveTargets : {false, true})
      for (bool PreserveBindings : {false, true}) {
        FrameState State;
        State.ImportBindingsIntact = Intact;
        State.Registers[x86reg::RSI / x86reg::GeneralRegStride] =
            FrameValue::importTarget(0x403000);
        State.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
            FrameValue::frame(-64);
        FrameTransfer Transfer(State, -16);
        LowOp Call;
        Call.Opcode = NdOp::INDIR_CALL;
        Call.Output = NdVar::reg(x86reg::RAX, 4);
        Call.addInput(NdVar::reg(x86reg::RSI, 4));
        Transfer.write(Call, {}, 0, PreserveTargets, PreserveBindings);
        EXPECT_EQ(State.ImportBindingsIntact, Intact && PreserveBindings);
        EXPECT_EQ(State.Registers[x86reg::RSI / x86reg::GeneralRegStride]
                      .ImportTarget.has_value(),
                  PreserveTargets);
        const auto &Result =
            State.Registers[x86reg::RAX / x86reg::GeneralRegStride];
        EXPECT_TRUE(Result.MayBeFrame);
        EXPECT_FALSE(Result.Constant);
        EXPECT_FALSE(Result.ImageAddress);
        EXPECT_FALSE(Result.ImportTarget);
        EXPECT_FALSE(
            State.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset);
      }
}

TEST(RegistrationFrameScalar, SegmentedWritesNeedAnImageDisjointnessProof) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    FrameState State;
    FrameTransfer Transfer(State, -16);
    LowOp Store;
    Store.Opcode = NdOp::STORE;
    Store.MemoryAddressSpace = Mutation == 3 ? NdMemoryAddressSpace::X86GS
                                             : NdMemoryAddressSpace::X86FS;
    Store.addInput(Mutation == 4 ? NdVar::reg(x86reg::RCX, 4)
                                 : NdVar::cst(Mutation == 1 ? 0x18 : 0, 4));
    Store.addInput(NdVar::cst(0, Mutation == 2 ? 2 : 4));
    Transfer.invalidateImportBindings(Store, {0x403000});
    LowOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output = NdVar::tmp(0, 4);
    Load.addInput(NdVar::cst(0x403000, 4));
    EXPECT_EQ(bool(Transfer.evaluate(Load, false, 0x403000).ImportTarget),
              Mutation == 0);
  }
}

TEST(RegistrationFrameScalar, DispatchCannotReseedCachedImportBindings) {
  for (bool Intact : {false, true}) {
    Domain Source;
    Source.Frame.ImportBindingsIntact = Intact;
    auto Root = dispatchSource(Source);
    LowOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output = NdVar::tmp(0, 4);
    Load.addInput(NdVar::cst(0x403000, 4));
    EXPECT_FALSE(Root.Frame.ImportBindingsIntact);
    EXPECT_FALSE(FrameTransfer(Root.Frame, -16)
                     .evaluate(Load, false, 0x403000)
                     .ImportTarget);
  }
}

TEST(RegistrationFrameScalar, CookieAddressXORDoesNotBecomeAnIntegerMask) {
  FrameValue Cookie;
  Cookie.SecurityCookie = true;
  auto Xor = operation(NdOp::INT_XOR);
  Xor.Output.Size = 4;
  const auto Pointer = FrameValue::imageAddress(0x403000);
  const auto Encoded = evaluate(Cookie, Pointer, Xor);
  EXPECT_EQ(Encoded.cookieAddress(), 0x403000u);
  EXPECT_FALSE(Encoded.Constant);
  EXPECT_EQ(Encoded.CookieXOR, 0u);
  EXPECT_EQ(evaluate(Encoded, Pointer, Xor), Cookie);
  const auto Numeric = evaluate(Encoded, FrameValue::constant(0x403000), Xor);
  EXPECT_NE(Numeric, Cookie);
  EXPECT_FALSE(Numeric.cookieAddress());
  EXPECT_EQ(evaluate(Encoded, Encoded, Xor).Constant, 0u);
  EXPECT_FALSE(evaluate(Encoded, Cookie, Xor).Constant);
}

TEST(RegistrationFrameScalar, DistinctOrAmbiguousRootsRetainTaint) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    FrameValue Left = FrameValue::frame(-16), Right = Left;
    switch (Mutation) {
    case 0:
      Right = FrameValue::entryFrame(-16);
      break;
    case 1:
      Right = FrameValue::callbackFrame(0x1800, -16);
      break;
    case 2:
      Left = FrameValue::entryFrame(-16);
      Right = FrameValue::callbackFrame(0x1800, -16);
      break;
    case 3:
      Left = FrameValue::callbackFrame(0x1800, -16);
      Right = FrameValue::callbackFrame(0x1900, -16);
      break;
    case 4:
      Right = join(Right, FrameValue::frame(-20));
      break;
    case 5:
      Right = join(Right, {});
      break;
    case 6:
      Right = FrameValue::exceptionObject(0, 0);
      Left = Right;
      break;
    case 7:
      Right.SecurityCookie = true;
      break;
    case 8:
      Right.ReturnPC = true;
      break;
    case 9:
      Right = FrameValue::taint(true, true);
      break;
    }
    for (NdOp Opcode : {NdOp::INT_SUB, NdOp::INT_EQUAL, NdOp::INT_NOTEQUAL}) {
      const auto Result = evaluate(Left, Right, operation(Opcode));
      EXPECT_FALSE(Result.Constant);
      EXPECT_TRUE(Result.MayBeFrame);
    }
  }
}

TEST(RegistrationFrameScalar, PartialWidthsAndOrderingAreNotRelativeProofs) {
  for (NdOp Opcode : {NdOp::INT_SUB, NdOp::INT_EQUAL, NdOp::INT_NOTEQUAL})
    for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
      SCOPED_TRACE(Mutation);
      LowOp Op = operation(Opcode);
      if (Mutation < 6)
        Op.Inputs[Mutation / 3].Size = Mutation % 3 == 0   ? 1
                                       : Mutation % 3 == 1 ? 2
                                                           : 8;
      else if (Mutation == 6)
        Op.Output.Size = Opcode == NdOp::INT_SUB ? 1 : 4;
      else if (Mutation == 7)
        Op.Output.Size = 8;
      else
        Op.Opcode = NdOp::INT_LESS;
      const auto Result =
          evaluate(FrameValue::frame(-16), FrameValue::frame(-16), Op);
      EXPECT_FALSE(Result.Constant);
      EXPECT_TRUE(Result.MayBeFrame);
    }
}

TEST(RegistrationFrameScalar, ReloadedChainComparisonIsAnExactScalar) {
  FrameState Frame;
  Frame.store(-36, 4, FrameValue::frame(-16));
  Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride] =
      FrameValue::frame(-36);
  FrameTransfer Transfer(Frame, -16);
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.NumInputs = 1;
  Load.Output = NdVar::reg(x86reg::RAX, 4);
  Load.Inputs[0] = NdVar::cst(0, 4);
  Load.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  Transfer.write(Load, Transfer.evaluate(Load, true));
  Load.Output = Load.Inputs[0] = NdVar::reg(x86reg::RCX, 4);
  Load.MemoryAddressSpace = NdMemoryAddressSpace::Default;
  Transfer.write(Load, Transfer.evaluate(Load, true));
  const LowOp Difference = operation(NdOp::INT_SUB);
  Transfer.write(Difference, Transfer.evaluate(Difference, true));
  LowOp Equal = operation(NdOp::INT_EQUAL);
  Equal.Inputs[0] = Difference.Output;
  Equal.Inputs[1] = NdVar::cst(0, 4);
  EXPECT_EQ(Transfer.evaluate(Equal, true), FrameValue::constant(1));

  Frame.store(-35, 1, {});
  Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride] =
      FrameValue::frame(-36);
  Transfer.write(Load, Transfer.evaluate(Load, true));
  const auto Partial = Transfer.evaluate(Difference, true);
  EXPECT_FALSE(Partial.Constant);
  EXPECT_TRUE(Partial.MayBeFrame);
}

TEST(RegistrationFrameScalar, CookieSuccessKeepsRegistersAndEqualCompareFlags) {
  FrameState Frame;
  for (unsigned I = 0; I != Frame.Registers.size(); ++I)
    Frame.Registers[I] = FrameValue::constant(0x12340000 + I);
  auto &Cookie = Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride];
  Cookie = {};
  Cookie.SecurityCookie = true;
  for (uint64_t Flag :
       {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF})
    Frame.OtherRegisterBytes[Flag] =
        FrameValue::constant(Flag != x86reg::PF && Flag != x86reg::ZF);
  Frame.OtherRegisterBytes[x86reg::DF] = FrameValue::constant(1);
  Frame.OtherRegisterBytes[x86reg::VectorBase] = FrameValue::constant(0x57);
  const auto Registers = Frame.Registers;
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Output = NdVar::reg(x86reg::RAX, 4);
  Call.NumInputs = 1;
  Call.Inputs[0] = NdVar::codeAddress(0x2000, 4);
  FrameTransfer Transfer(Frame, -12);
  ASSERT_TRUE(Transfer.isCookieCheck(Call, 0x2000));
  Transfer.write(Call, {}, 0x2000);
  EXPECT_EQ(Frame.Registers, Registers);
  for (uint64_t Flag :
       {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF})
    EXPECT_EQ(Frame.OtherRegisterBytes[Flag],
              FrameValue::constant(Flag == x86reg::PF || Flag == x86reg::ZF));
  EXPECT_EQ(Frame.OtherRegisterBytes[x86reg::DF], FrameValue::constant(1));
  EXPECT_EQ(Frame.OtherRegisterBytes[x86reg::VectorBase],
            FrameValue::constant(0x57));
  EXPECT_FALSE(Frame.OtherRegistersMayBeFrame);
}
} // namespace
