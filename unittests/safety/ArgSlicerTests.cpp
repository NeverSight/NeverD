//===- ArgSlicerTests.cpp - Backward argument classification -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/ArgSlicer.h"

#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"

#include "gtest/gtest.h"

using namespace neverd;
using namespace neverd::safety;

namespace {

MedVar temp(int Id, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = Size;
  return V;
}

MedVar param(int Id, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = Id;
  V.Size = Size;
  return V;
}

// Append an op defining Out from a single input in block 0.
void defOp(MedFunc &F, NdOp Op, MedVar Out, MedVar In0) {
  MedOp O;
  O.Opcode = Op;
  O.Output = Out;
  O.addInput(In0);
  F.Blocks[0].Ops.push_back(O);
}

MedFunc newFunc(const std::string &Name = "f") {
  MedFunc F;
  F.Name = Name;
  F.Entry = 0x100;
  MedBlock B;
  B.Id = 0;
  F.Blocks.push_back(std::move(B));
  return F;
}

// Register a call at the end of block 0 that defines Ret and record its info.
void addCall(MedFunc &F, const std::string &Callee, MedVar Ret) {
  int OpIdx = static_cast<int>(F.Blocks[0].Ops.size());
  MedOp O;
  O.Opcode = NdOp::CALL;
  O.Output = Ret;
  O.addInput(MedVar::makeConst(0x9000, 8));
  F.Blocks[0].Ops.push_back(O);
  MedCallInfo CI;
  CI.BlockId = 0;
  CI.OpIdx = OpIdx;
  CI.TargetName = Callee;
  F.CallInfos.push_back(CI);
}

// Add the sink call whose argument is being classified; returns its index.
size_t addSink(MedFunc &F, const std::string &Callee, std::vector<MedVar> Args) {
  MedOp O;
  O.Opcode = NdOp::CALL;
  O.Addr = 0x1234;
  O.addInput(MedVar::makeConst(0x8000, 8));
  int OpIdx = static_cast<int>(F.Blocks[0].Ops.size());
  F.Blocks[0].Ops.push_back(O);
  MedCallInfo CI;
  CI.BlockId = 0;
  CI.OpIdx = OpIdx;
  CI.TargetName = Callee;
  CI.Args = std::move(Args);
  F.CallInfos.push_back(CI);
  return F.CallInfos.size() - 1;
}

} // namespace

TEST(ArgSlicer, ConstantIsBounded) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  size_t Idx = addSink(F, "memcpy",
                       {temp(1), temp(2), MedVar::makeConst(8, 8)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Bounded);
  ASSERT_TRUE(C.ConstValue.has_value());
  EXPECT_EQ(*C.ConstValue, 8u);
}

TEST(ArgSlicer, StrlenGuardedIsBounded) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  // n = strlen(...); memcpy(dst, src, n)
  MedFunc F = newFunc();
  addCall(F, "strlen", temp(5));
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Bounded);
}

TEST(ArgSlicer, ReadReturnIsTainted) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  // n = read(...); memcpy(dst, src, n)
  MedFunc F = newFunc();
  addCall(F, "read", temp(5));
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_EQ(C.TaintSource, "read");
}

TEST(ArgSlicer, UnknownLoadIsUnknown) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  // t1 = load(t0); memcpy(dst, src, t1) — t0 is an unresolved pointer.
  MedFunc F = newFunc();
  defOp(F, NdOp::LOAD, temp(1), temp(0));
  size_t Idx = addSink(F, "memcpy", {temp(9), temp(8), temp(1)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
}

TEST(ArgSlicer, ConstantAddressLoadIsBounded) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  defOp(F, NdOp::LOAD, temp(1), MedVar::makeConst(0x4000, 8));
  size_t Idx = addSink(F, "memcpy", {temp(9), temp(8), temp(1)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Bounded);
}

TEST(ArgSlicer, StatReturnIsBounded) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();
  MedFunc F = newFunc();
  addCall(F, "stat", temp(5));
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5)});
  EXPECT_EQ(classifyArgument(In, Cat, F, Idx, 2).Flow, ArgFlow::Bounded);
}

TEST(ArgSlicer, MaskedValueIsBounded) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  // t1 = load(...); t2 = t1 & 0x0f; memcpy(dst, src, t2)
  MedFunc F = newFunc();
  defOp(F, NdOp::LOAD, temp(1), MedVar::makeConst(0x4000, 8));
  {
    MedOp And;
    And.Opcode = NdOp::INT_AND;
    And.Output = temp(2);
    And.addInput(temp(1));
    And.addInput(MedVar::makeConst(0x0f, 8));
    F.Blocks[0].Ops.push_back(And);
  }
  size_t Idx = addSink(F, "memcpy", {temp(9), temp(8), temp(2)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Bounded);
}

TEST(ArgSlicer, MainParameterIsTaintedArgv) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  // strcpy(dst, argv-derived): the source argument comes from a main parameter.
  MedFunc F = newFunc("main");
  size_t Idx = addSink(F, "strcpy", {temp(1), param(2)});
  // strcpy's deciding argument is the source (implicit length).
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 1);
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_EQ(C.TaintSource, "argv");
}

TEST(ArgSlicer, SelectClampIsBounded) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  // n = read(); t = n <= 8 ? n : 8; memcpy(dst, src, t)
  MedFunc F = newFunc();
  addCall(F, "read", temp(5));
  {
    MedOp Cmp;
    Cmp.Opcode = NdOp::INT_LESSEQUAL;
    Cmp.Output = temp(6);
    Cmp.addInput(temp(5));
    Cmp.addInput(MedVar::makeConst(8, 8));
    F.Blocks[0].Ops.push_back(Cmp);
  }
  {
    MedOp Sel;
    Sel.Opcode = NdOp::SELECT;
    Sel.Output = temp(7);
    Sel.addInput(temp(6));
    Sel.addInput(temp(5));
    Sel.addInput(MedVar::makeConst(8, 8));
    F.Blocks[0].Ops.push_back(Sel);
  }
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(7)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Bounded);
}

TEST(ArgSlicer, UnrecoveredArgumentFailsClosed) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  size_t Idx = addSink(F, "memcpy", {temp(1)}); // only one arg recovered.
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
}
