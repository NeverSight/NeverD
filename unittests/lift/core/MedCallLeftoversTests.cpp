//===- MedCallLeftoversTests.cpp - Values calls leave in registers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedCallLeftovers.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/AArch64/AArch64Regs.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace {

using namespace neverd;

MedVar value(int Id, int Version, uint64_t Offset, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = Id;
  V.SSAVer = Version;
  V.Size = Size;
  V.RegOff = Offset;
  V.TheArch = Arch::AArch64;
  return V;
}

PhiNode join(MedVar Output, std::vector<std::pair<int, MedVar>> Args) {
  PhiNode Phi;
  Phi.Output = Output;
  Phi.Args = std::move(Args);
  return Phi;
}

} // namespace

// What a register holds after calls change it, and every join of nothing
// else, loops of joins included, is left by calls; a join that anything else
// reaches is not, nor is a clobber that keeps part of its value or may be a
// result field.
TEST(MedCallLeftovers, JoinsOfCallClobbersOnly) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const uint64_t X1 = TRI.IntParamRegs[1], X2 = TRI.IntParamRegs[2],
                 X3 = TRI.IntParamRegs[3], V0 = TRI.FPParamRegs[0];
  MedFunc F;
  F.Blocks.resize(5);
  for (int I = 0; I < 5; ++I)
    F.Blocks[I].Id = I;
  // Calls in blocks 1 and 2 clobber x1-x3, and one keeps a vector
  // register's low half; block 3 joins them, and block 4 loops.
  const MedVar X2A = value(2, 1, X2), X2B = value(2, 2, X2);
  const MedVar X3A = value(3, 1, X3), X3Entry = value(3, 0, X3);
  const MedVar X1A = value(1, 1, X1), X1B = value(1, 2, X1);
  const MedVar Kept = value(8, 1, V0, 16);
  F.CallClobbers = {{X2A, 1}, {X2B, 2}, {X3A, 1},
                    {X1A, 1}, {X1B, 2}, {Kept, 1, value(8, 0, V0, 16), 8}};
  F.StructReturnCandidates = {{1, {value(0, 1, TRI.IntParamRegs[0]), X1A}}};
  const MedVar X2Join = value(2, 3, X2), X2Loop = value(2, 4, X2);
  const MedVar X3Join = value(3, 2, X3), X1Join = value(1, 3, X1);
  const MedVar X1Partial = value(1, 4, X1);
  F.Blocks[3].Phis = {join(X2Join, {{1, X2A}, {2, X2B}}),
                      join(X3Join, {{1, X3A}, {2, X3Entry}}),
                      join(X1Join, {{1, X1B}, {2, X1B}}),
                      join(X1Partial, {{1, X1A}, {2, X1B}})};
  F.Blocks[4].Phis = {join(X2Loop, {{3, X2Join}, {4, X2Loop}})};

  const ValuesLeftByCalls Left(F);
  EXPECT_TRUE(Left.contains(X2A));
  EXPECT_TRUE(Left.contains(X2Join));
  EXPECT_TRUE(Left.contains(X2Loop));
  EXPECT_TRUE(Left.contains(X1Join));
  // Version zero is what x3 brought in: the join keeps a value.
  EXPECT_FALSE(Left.contains(X3Join));
  EXPECT_FALSE(Left.contains(X3Entry));
  // Call 1 may return a field in x1, which the join then carries.
  EXPECT_FALSE(Left.contains(X1A));
  EXPECT_FALSE(Left.contains(X1Partial));
  // The call keeps the register's low half.
  EXPECT_FALSE(Left.contains(Kept));

  // The exception dispatcher entering the loop brings another value.
  F.Blocks[4].Phis[0].ExceptionalEntry = X3Entry;
  EXPECT_FALSE(ValuesLeftByCalls(F).contains(X2Loop));
  EXPECT_TRUE(ValuesLeftByCalls(F).contains(X2Join));
}

TEST(MedCallLeftovers, SameCarrierSurvivesTemporaryCopiesAndLoopJoins) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const auto X2 = TRI.IntParamRegs[2];
  const MedVar A = value(2, 1, X2), B = value(2, 2, X2);
  auto Temp = [](int Id, int Version = 1) {
    auto V = value(Id, Version, 0);
    V.Kind = MedVar::Temp;
    return V;
  };
  const auto TA = Temp(20), TB = Temp(21), Joined = Temp(22), Loop = Temp(23),
             Back = Temp(24);
  const auto Restored = value(2, 3, X2);
  auto Copy = [](MedVar Out, MedVar In) {
    MedOp Op;
    Op.Opcode = NdOp::COPY;
    Op.Output = Out;
    Op.addInput(In);
    return Op;
  };
  MedFunc F;
  F.Blocks.resize(5);
  F.CallClobbers = {{A, 1}, {B, 2}};
  F.Blocks[1].Ops = {Copy(TA, A)};
  F.Blocks[2].Ops = {Copy(TB, B)};
  F.Blocks[3].Phis = {join(Joined, {{1, TA}, {2, TB}})};
  F.Blocks[4].Phis = {join(Loop, {{3, Joined}, {4, Back}})};
  F.Blocks[4].Ops = {Copy(Back, Loop), Copy(Restored, Loop)};
  const ValuesLeftByCalls Left(F);
  EXPECT_TRUE(Left.contains(Restored));
  EXPECT_FALSE(Left.contains(Joined));
  // Blocks are storage order, not provenance order.
  std::reverse(F.Blocks.begin(), F.Blocks.end());
  EXPECT_TRUE(ValuesLeftByCalls(F).contains(Restored));
  // One real input invalidates the whole loop, including its copies.
  F.Blocks.front().Phis.front().ExceptionalEntry = value(2, 0, X2);
  EXPECT_FALSE(ValuesLeftByCalls(F).contains(Restored));
}

TEST(MedCallLeftovers, TransportCannotBorrowACarrierOrChangeTheValue) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const auto X1 = TRI.IntParamRegs[1], X2 = TRI.IntParamRegs[2];
  const auto A = value(2, 1, X2);
  MedFunc F;
  F.Blocks.resize(1);
  F.CallClobbers = {{A, 1}};
  auto Temp = value(20, 1, 0);
  Temp.Kind = MedVar::Temp;
  MedOp Save;
  Save.Opcode = NdOp::COPY;
  Save.Output = Temp;
  Save.addInput(A);
  F.Blocks[0].Ops.push_back(Save);
  MedOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Output = value(1, 2, X1);
  Copy.addInput(Temp);
  F.Blocks[0].Ops.push_back(Copy);
  EXPECT_FALSE(ValuesLeftByCalls(F).contains(Copy.Output));

  Copy.Output = value(2, 2, X2);
  Copy.Output.Size = 4;
  F.Blocks[0].Ops.back() = Copy;
  EXPECT_FALSE(ValuesLeftByCalls(F).contains(Copy.Output));
  // A different-width view of the clobber is not the same untouched word.
  auto Narrow = A;
  Narrow.Size = 4;
  EXPECT_FALSE(ValuesLeftByCalls(F).contains(Narrow));

  Copy.Output.Size = 8;
  Copy.Opcode = NdOp::INT_ADD;
  Copy.addInput(MedVar::makeConst(1, 8));
  F.Blocks[0].Ops.back() = Copy;
  EXPECT_FALSE(ValuesLeftByCalls(F).contains(Copy.Output));
  // A possible extra return field remains a real value through exact copies.
  F.StructReturnCandidates = {{1, {Narrow}}};
  F.Blocks[0].Ops.back().Opcode = NdOp::COPY;
  F.Blocks[0].Ops.back().NumInputs = 1;
  EXPECT_FALSE(ValuesLeftByCalls(F).contains(Copy.Output));
}

TEST(MedCallLeftovers, AnUnseededCycleIsNotACallClobber) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const auto X2 = TRI.IntParamRegs[2];
  const auto A = value(2, 1, X2), B = value(2, 2, X2);
  MedFunc F;
  F.Blocks.resize(1);
  F.CallClobbers = {{value(3, 1, TRI.IntParamRegs[3]), 1}};
  F.Blocks[0].Phis = {join(A, {{0, B}}), join(B, {{0, A}})};
  EXPECT_FALSE(ValuesLeftByCalls(F).contains(A));
  EXPECT_FALSE(ValuesLeftByCalls(F).contains(B));
}

TEST(MedCallLeftovers, UnknownTransportNeedsAnActualCallOrigin) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const auto X2 = TRI.IntParamRegs[2];
  const auto Unknown = MedVar::makeUnspecified(8, Arch::AArch64);
  const auto Joined = value(2, 1, X2), Restored = value(2, 2, X2);
  auto Saved = value(20, 1, 0);
  Saved.Kind = MedVar::Temp;
  const auto Copy = [](MedVar Out, MedVar In) {
    MedOp Op;
    Op.Opcode = NdOp::COPY;
    Op.Output = Out;
    Op.addInput(In);
    return Op;
  };
  MedFunc F;
  F.Blocks.resize(1);
  // A clobber in an unrelated register cannot authenticate this chain.
  F.CallClobbers = {{value(3, 1, TRI.IntParamRegs[3]), 1}};
  F.Blocks[0].Phis = {join(Joined, {{0, Unknown}})};
  F.Blocks[0].Ops = {Copy(Saved, Joined), Copy(Restored, Saved)};
  EXPECT_FALSE(ValuesLeftByCalls(F).contains(Joined));
  EXPECT_FALSE(ValuesLeftByCalls(F).contains(Restored));
  // Unavailable incoming values are neutral only beside a real call origin.
  const auto Clobber = value(2, 3, X2);
  F.CallClobbers.push_back({Clobber, 2});
  F.Blocks[0].Phis[0].Args.emplace_back(1, Clobber);
  EXPECT_TRUE(ValuesLeftByCalls(F).contains(Joined));
  EXPECT_TRUE(ValuesLeftByCalls(F).contains(Restored));
}

TEST(MedCallLeftovers, PlatformRegisterPreservationFollowsTheABI) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(MedVar::makeConst(0x2000, 8));
  for (BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::Unknown,
                              BinaryFormat::MachO, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<int>(Format));
    const bool Reserved =
        Format == BinaryFormat::MachO || Format == BinaryFormat::COFF;
    const auto Ranges = TRI.callPreservedRanges(Format);
    EXPECT_EQ(std::any_of(Ranges.begin(), Ranges.end(),
                          [](const auto &Range) {
                            return Range.Offset == a64reg::X18 &&
                                   Range.Bytes == 8;
                          }),
              Reserved);
    for (uint16_t Size : {4, 8}) {
      EXPECT_EQ(TRI.isCallPreserved(a64reg::X18, Size, Format), Reserved);
      EXPECT_EQ(callClobbersRegister(Call, a64reg::X18, Size, TRI, Format),
                !Reserved);
    }
    EXPECT_EQ(TRI.callPreservedPrefixSize(a64reg::X18 + 4, 4, Format),
              Reserved ? 4 : 0);
    EXPECT_FALSE(TRI.isCallPreserved(a64reg::X18, 16, Format));
    EXPECT_TRUE(callClobbersRegister(Call, a64reg::X18, 16, TRI, Format));
    EXPECT_TRUE(callClobbersRegister(Call, a64reg::X17, 8, TRI, Format));
  }
}
