//===- RegistrationSEHThreadTests.cpp - Current-thread read proofs -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise root ownership, partial aliases, exact fields and source guards.
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationFrame.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86/X86Regs.h"

namespace {
using namespace neverd;
using namespace neverd::registration_state;
using namespace neverd::registration_test;

enum class Change {
  None,
  Spill,
  WrongSelf,
  NarrowSelf,
  WrongField,
  NarrowField,
  PartialSpill,
  AtomicSelf,
  AtomicField,
  WrongSegment,
  ThreadWrite,
  UnknownCall,
  Guard,
  Conditional,
  MissingBoundary,
  NarrowAddress,
  NarrowTemp,
  NarrowArithmetic,
  FrameMix,
  ScalarDereference,
  Count
};

LowFunc threadFrame(Change Mutation, uint16_t AddressWidth = 4) {
  auto F = makeBranchingFrame();
  auto &B = F.Blocks[3];
  B.EndAddr = 0x104f;
  B.InstructionBoundaries.clear();
  emitOp(
      B, 0x1030, NdOp::LOAD,
      NdVar::reg(x86reg::RAX, Mutation == Change::NarrowSelf ? 2 : 4),
      {NdVar::cst(Mutation == Change::WrongSelf ? 0x1c : 0x18, AddressWidth)},
      NdMemoryAddressSpace::X86FS);
  if (Mutation == Change::AtomicSelf)
    B.Ops.back().MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  if (Mutation == Change::Spill || Mutation == Change::PartialSpill) {
    emitOp(B, 0x1036, NdOp::INT_SUB, NdVar::tmp(8, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(28, 4)});
    emitOp(B, 0x1036, NdOp::STORE, {},
           {NdVar::tmp(8, 4), NdVar::reg(x86reg::RAX, 4)});
    if (Mutation == Change::PartialSpill)
      emitOp(B, 0x1036, NdOp::STORE, {}, {NdVar::tmp(8, 4), NdVar::cst(0, 2)});
    emitOp(B, 0x1036, NdOp::LOAD, NdVar::reg(x86reg::RAX, 4),
           {NdVar::tmp(8, 4)});
  }
  if (Mutation == Change::UnknownCall)
    emitOp(B, 0x1036, NdOp::CALL, {}, {NdVar::cst(0x5000, 4)});
  emitOp(B, 0x1040, NdOp::INT_ADD,
         NdVar::tmp(8, Mutation == Change::NarrowArithmetic ? 2 : 4),
         {NdVar::reg(x86reg::RAX, 4),
          NdVar::cst(Mutation == Change::WrongField ? 8 : 4, 4)});
  if (Mutation == Change::NarrowTemp)
    emitOp(B, 0x1040, NdOp::INT_ADD, NdVar::tmp(9, 4),
           {NdVar::tmp(8, 2), NdVar::cst(0, 4)});
  if (AddressWidth == 8) {
    emitOp(B, 0x1040, NdOp::INT_ZEXT, NdVar::tmp(8, 8),
           {NdVar::tmp(8, Mutation == Change::NarrowArithmetic ? 2 : 4)});
  }
  const auto Address =
      Mutation == Change::NarrowTemp
          ? NdVar::tmp(9, 4)
          : NdVar::tmp(8, Mutation == Change::NarrowAddress ? 2 : AddressWidth);
  if (Mutation == Change::ThreadWrite)
    emitOp(B, 0x1040, NdOp::STORE, {}, {Address, NdVar::cst(0, 4)});
  else
    emitOp(B, 0x1040, NdOp::LOAD,
           NdVar::reg(x86reg::RDX, Mutation == Change::NarrowField ? 2 : 4),
           {Address},
           Mutation == Change::WrongSegment ? NdMemoryAddressSpace::X86FS
                                            : NdMemoryAddressSpace::Default);
  if (Mutation == Change::AtomicField)
    B.Ops.back().MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  if (Mutation == Change::FrameMix)
    emitOp(B, 0x1048, NdOp::INT_ADD, NdVar::reg(x86reg::RDX, 4),
           {NdVar::reg(x86reg::RDX, 4), NdVar::reg(x86reg::RBP, 4)});
  emitOp(B, 0x1048, NdOp::STORE, {},
         {Mutation == Change::ScalarDereference ? NdVar::reg(x86reg::RDX, 4)
                                                : NdVar::cst(0x6000, 4),
          Mutation == Change::ScalarDereference ? NdVar::cst(0, 4)
                                                : NdVar::reg(x86reg::RDX, 4)});
  for (size_t Begin = 0; Begin != B.Ops.size();) {
    size_t End = Begin + 1;
    while (End != B.Ops.size() && B.Ops[End].Addr == B.Ops[Begin].Addr)
      ++End;
    const va_t Next = End == B.Ops.size() ? B.EndAddr : B.Ops[End].Addr;
    B.InstructionBoundaries.push_back({B.Ops[Begin].Addr,
                                       uint16_t(Next - B.Ops[Begin].Addr),
                                       Begin, End - Begin});
    Begin = End;
  }
  if (Mutation == Change::Guard)
    B.InstructionBoundaries.front().ControlFlags |=
        LowInstructionControlFlag::InstructionGuard;
  if (Mutation == Change::Conditional)
    B.InstructionBoundaries.front().ControlFlags |=
        LowInstructionControlFlag::Conditional;
  if (Mutation == Change::MissingBoundary)
    B.InstructionBoundaries.erase(B.InstructionBoundaries.begin());
  return F;
}

TEST(RegistrationSEHThread, OnlyExactParentReadsCloseTheMemoryProof) {
  for (uint16_t Width : {4, 8})
    for (unsigned I = 0; I != unsigned(Change::Count); ++I) {
      const auto Mutation = Change(I);
      SCOPED_TRACE(::testing::Message() << Width << ' ' << I);
      const auto A = analyzeRegistrationStates(threadFrame(Mutation, Width));
      const bool Valid = Mutation == Change::None || Mutation == Change::Spill;
      EXPECT_EQ(A.Complete && A.CallbackStatesComplete &&
                    A.ImageReadsComplete && A.SEHThreadAccessesComplete,
                Valid);
      if (!Valid)
        continue;
      ASSERT_EQ(A.SEHThreadAccesses.size(), 2u);
      const auto &Root = A.SEHThreadAccesses[0];
      const auto &Field = A.SEHThreadAccesses[1];
      EXPECT_EQ(Root.Field, RegistrationSEHThreadAccess::Kind::Self);
      EXPECT_EQ(Field.Field, RegistrationSEHThreadAccess::Kind::StackBase);
      EXPECT_EQ(Field.RootAddress, Root.Address);
      EXPECT_EQ(Field.RootOpSeq, Root.OpSeq);
    }
}

TEST(RegistrationSEHThread, CopiesAndSpillsRetainOnlyUnanimousFullRoots) {
  FrameState State;
  FrameTransfer Transfer(State, -16);
  LowBlock B;
  emitOp(B, 0x1000, NdOp::LOAD, NdVar::tmp(0, 4), {NdVar::cst(0x18, 4)},
         NdMemoryAddressSpace::X86FS);
  Transfer.beginInstruction(0x1000);
  const auto Root = Transfer.evaluate(B.Ops.back(), true);
  ASSERT_TRUE(Root.ThreadAddress);
  EXPECT_FALSE(Root.MayBeFrame);
  EXPECT_FALSE(Root.Offset);
  EXPECT_FALSE(Root.Constant);
  Transfer.write(B.Ops.back(), Root);
  EXPECT_EQ(Transfer.read(NdVar::tmp(0, 4)), Root);
  EXPECT_FALSE(Transfer.read(NdVar::tmp(0, 2)).ThreadAddress);
  State.store(-28, 4, Root);
  EXPECT_EQ(State.load(-28, 4), Root);
  EXPECT_FALSE(State.load(-28, 2).ThreadAddress);
  State.store(-27, 1, FrameValue::constant(0));
  EXPECT_FALSE(State.load(-28, 4).ThreadAddress);
  EXPECT_EQ(join(Root, Root), Root);
  auto Other = Root;
  ++Other.ThreadAddress->RootOpSeq;
  EXPECT_FALSE(join(Root, Other).ThreadAddress);
  EXPECT_FALSE(join(Root, FrameValue{}).ThreadAddress);
  State.Registers[0] = Root;
  const auto Hash = State.snapshotHash();
  State.Registers[0] = Other;
  EXPECT_NE(State.snapshotHash(), Hash);
  EXPECT_FALSE(projectX86CallReturnValue(Root).ThreadAddress);
}

TEST(RegistrationSEHThread, StackBaseIsOpaqueAndHasNoFrameCoordinate) {
  FrameState State;
  State.Registers[0] = FrameValue::threadBlock({0x1000, 0, 4});
  FrameTransfer Transfer(State, -16);
  LowBlock B;
  emitOp(B, 0x1006, NdOp::LOAD, NdVar::reg(x86reg::RDX, 4),
         {NdVar::reg(x86reg::RAX, 4)});
  const auto Scalar = Transfer.evaluate(B.Ops.back(), true);
  EXPECT_TRUE(Scalar.MayBeFrame);
  EXPECT_TRUE(Scalar.isFrameIndependentScalar());
  EXPECT_FALSE(Scalar.Offset);
  EXPECT_FALSE(Scalar.Constant);
  EXPECT_FALSE(Scalar.ThreadAddress);
  EXPECT_FALSE(join(Scalar, FrameValue::frame(0)).isFrameIndependentScalar());
}
} // namespace
