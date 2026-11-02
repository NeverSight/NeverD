//===- AArch64RawSPProvenanceTests.cpp - Raw SP source flow --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Complete dataflow paths distinguish original SP from loaded data and
/// uninitialized or image-frame coordinates in SSA and mutable source IR.
//===----------------------------------------------------------------------===//

#include "AArch64ImmediateMemoryBase.h"
#include "gtest/gtest.h"

#include "neverd/lift/AArch64/AArch64Regs.h"

using namespace neverd;

namespace {
#define RAW_SP_FLOW_VALUE(Type, Name, Value) inline constexpr Type Name = Value;
#include "AArch64RawSPFlowFixtures.def"

MedVar variable(int Id, decltype(MedVar::Kind) Kind = MedVar::Temp,
                unsigned Bytes = kAddressMemoryPointerBytes) {
  MedVar V;
  V.Kind = Kind;
  V.Id = Id;
  V.SSAVer = 1;
  V.Size = Bytes;
  V.TheArch = Arch::AArch64;
  return V;
}

MedOp copy(MedVar To, MedVar From) {
  MedOp Op;
  Op.Opcode = NdOp::COPY;
  Op.Output = To;
  Op.addInput(From);
  return Op;
}

HighStmt assign(const MedVar &To, ExprPtr From) {
  HighStmt Statement;
  Statement.Kind = StmtKind::Assign;
  Statement.Dst = HighExpr::makeVar(To, NdType::makeInt(To.Size, false));
  Statement.Val = std::move(From);
  return Statement;
}

ExprPtr expression(const MedVar &V) {
  return HighExpr::makeVar(V, NdType::makeInt(V.Size, false));
}
} // namespace

TEST(AArch64RawSPProvenance, SSAJoinsRequireEveryArmToReachAParameter) {
  const auto Parameter = variable(0, MedVar::Param);
  const auto Other = variable(1, MedVar::Param);
  const auto Selected = variable(2);
  const auto Joined = variable(3);
  MedFunc F;
  F.Blocks.emplace_back();
  auto &B = F.Blocks.front();
  MedOp Select;
  Select.Opcode = NdOp::SELECT;
  Select.Output = Selected;
  Select.addInput(MedVar::makeConst(1, 1));
  Select.addInput(Parameter);
  Select.addInput(Other);
  B.Ops = {Select};
  PhiNode Phi;
  Phi.Output = Joined;
  Phi.Args = {{0, Selected}, {1, Joined}};
  B.Phis = {Phi};
  EXPECT_TRUE(AArch64ImmediateMedBases(F).supportsRawSP(Joined));
  B.Phis.front().ExceptionalEntry = Other;
  EXPECT_TRUE(AArch64ImmediateMedBases(F).supportsRawSP(Joined));
  B.Ops.front().Inputs[2] = MedVar::makeConst(0, kAddressMemoryPointerBytes);
  EXPECT_FALSE(AArch64ImmediateMedBases(F).supportsRawSP(Joined));
  B.Ops.front().Inputs[2] = Other;
  B.Phis.front().ExceptionalEntry = variable(4);
  EXPECT_FALSE(AArch64ImmediateMedBases(F).supportsRawSP(Joined));
  B.Phis.front().ExceptionalEntry.reset();
  // A separate uninitialized cycle cannot borrow the other SELECT arm's root.
  const auto Cycle = variable(5);
  B.Ops.front().Inputs[2] = Cycle;
  B.Ops.push_back(copy(Cycle, Cycle));
  EXPECT_FALSE(AArch64ImmediateMedBases(F).supportsRawSP(Joined));
  B.Ops.front().Inputs[2] = Other;
  B.Ops.pop_back();
  F.SkippedSSA = true;
  EXPECT_FALSE(AArch64ImmediateMedBases(F).supportsRawSP(Joined));
}

TEST(AArch64RawSPProvenance, CompletedBaseIsDistinctFromTheLoadedValue) {
  const auto Parameter = variable(0, MedVar::Param);
  const auto Pair = variable(1, MedVar::Temp, kWritebackPairBytes);
  const auto Extracted = variable(2);
  const auto Copied = variable(3);
  MedFunc F;
  F.Blocks.emplace_back();
  auto &B = F.Blocks.front();
  MedOp Load;
  Load.Opcode = NdOp::INTRINSIC;
  Load.Output = Pair;
  Load.addInput(MedVar::makeConst(static_cast<uint64_t>(Intrinsic::A64_Ldrab),
                                  kAddressMemoryIdentifierBytes));
  Load.addInput(Parameter);
  Load.addInput(MedVar::makeConst(kFlowAuthWord, kPAuthLoadWordBytes));
  MedOp Extract;
  Extract.Opcode = NdOp::SUBBYTES;
  Extract.Output = Extracted;
  Extract.addInput(Pair);
  Extract.addInput(
      MedVar::makeConst(kAddressMemoryPointerBytes, kAddressMemoryWordBytes));
  B.Ops = {Load, Extract, copy(Copied, Extracted)};
  ASSERT_TRUE(aarch64PAuthLoadMedShapeIsValid(B.Ops.front()));
  EXPECT_TRUE(AArch64ImmediateMedBases(F).supportsRawSP(Copied));
  B.Ops[1].Inputs[1].ConstVal = 0;
  EXPECT_FALSE(AArch64ImmediateMedBases(F).supportsRawSP(Copied));
  B.Ops[1].Inputs[1].ConstVal = kAddressMemoryPointerBytes;
  B.Ops.front().Inputs[2].ConstVal = kFlowGeneralAuthWord;
  EXPECT_FALSE(AArch64ImmediateMedBases(F).supportsRawSP(Copied));
  B.Ops.front().Inputs[2].ConstVal = kFlowAuthWord;
  B.Ops[1].Output.Size = kAddressMemoryNarrowIndexBytes;
  EXPECT_FALSE(AArch64ImmediateMedBases(F).supportsRawSP(Copied));
}

TEST(AArch64RawSPProvenance, MutableJoinsUseAllReachingDefinitions) {
  const auto Parameter = variable(0, MedVar::Param);
  const auto Other = variable(1, MedVar::Param);
  const auto Local = variable(2);
  HighFunc F;
  F.ReturnType = NdType::makeVoid();
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = expression(variable(2, MedVar::Param, 1));
  Branch.Body = {assign(Local, expression(Parameter))};
  Branch.ElseBody = {assign(Local, expression(Other))};
  HighStmt Use;
  Use.Kind = StmtKind::ExprStmt;
  Use.Val = expression(Local);
  F.Body = {Branch, Use};
  EXPECT_TRUE(AArch64ImmediateHighBases(F).supportsRawSP(*F.Body.back().Val,
                                                         &F.Body.back()));
  F.Body.front().ElseBody.front().Val =
      HighExpr::makeConst(0, kAddressMemoryPointerBytes);
  EXPECT_FALSE(AArch64ImmediateHighBases(F).supportsRawSP(*F.Body.back().Val,
                                                          &F.Body.back()));
  F.Body.front().ElseBody.clear();
  EXPECT_FALSE(AArch64ImmediateHighBases(F).supportsRawSP(*F.Body.back().Val,
                                                          &F.Body.back()));
}

TEST(AArch64RawSPProvenance, MutableLoopsRetainEntryAndBackedgeValues) {
  const auto Parameter = variable(0, MedVar::Param);
  const auto Local = variable(1);
  HighFunc F;
  F.ReturnType = NdType::makeVoid();
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Cond = expression(variable(1, MedVar::Param, 1));
  Loop.Body = {assign(Local, expression(Local))};
  HighStmt Use;
  Use.Kind = StmtKind::ExprStmt;
  Use.Val = expression(Local);
  F.Body = {assign(Local, expression(Parameter)), Loop, Use};
  EXPECT_TRUE(AArch64ImmediateHighBases(F).supportsRawSP(*F.Body.back().Val,
                                                         &F.Body.back()));
  F.Body.front().Val = HighExpr::makeConst(0, kAddressMemoryPointerBytes);
  EXPECT_FALSE(AArch64ImmediateHighBases(F).supportsRawSP(*F.Body.back().Val,
                                                          &F.Body.back()));
  F.Body.erase(F.Body.begin());
  EXPECT_FALSE(AArch64ImmediateHighBases(F).supportsRawSP(*F.Body.back().Val,
                                                          &F.Body.back()));
}

TEST(AArch64RawSPProvenance, MutableValuesAreProvedAtTheirExactUse) {
  const auto Parameter = variable(0, MedVar::Param);
  const auto Local = variable(1);
  HighFunc F;
  F.ReturnType = NdType::makeVoid();
  HighStmt Use;
  Use.Kind = StmtKind::ExprStmt;
  Use.Val = expression(Local);
  F.Body = {assign(Local, expression(Parameter)), Use,
            assign(Local, HighExpr::makeConst(0, kAddressMemoryPointerBytes)),
            Use};
  const AArch64ImmediateHighBases Bases(F);
  EXPECT_TRUE(Bases.supportsRawSP(*F.Body[1].Val, &F.Body[1]));
  EXPECT_FALSE(Bases.supportsRawSP(*F.Body[3].Val, &F.Body[3]));
  // An indirect write through the parameter's address is not an entry value.
  auto Address = std::make_shared<HighExpr>();
  Address->Kind = ExprKind::Addr;
  Address->Type = NdType::makePtr(NdType::makeInt(kAddressMemoryPointerBytes));
  Address->Operands = {expression(Parameter)};
  HighStmt Write;
  Write.Kind = StmtKind::Store;
  Write.StoreAddr = Address;
  Write.StoreVal = HighExpr::makeConst(0, kAddressMemoryPointerBytes);
  Use.Val = expression(Parameter);
  F.Body = {Write, Use};
  EXPECT_FALSE(AArch64ImmediateHighBases(F).supportsRawSP(*F.Body.back().Val,
                                                          &F.Body.back()));
}
