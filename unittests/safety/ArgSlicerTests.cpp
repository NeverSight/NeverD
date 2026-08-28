//===- ArgSlicerTests.cpp - Backward argument classification -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SourceSemantics.h"
#include "gtest/gtest.h"

#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/safety/ArgSlicer.h"

using namespace neverd;
using namespace neverd::safety;

namespace {

constexpr uint64_t kSP = 0x1000;

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

MedVar mkReg(uint64_t Off, int Ver, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = static_cast<int>(Off);
  V.RegOff = Off;
  V.SSAVer = Ver;
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

va_t addCString(BinaryImage &Img, llvm::StringRef Value,
                va_t Address = 0x1000) {
  Segment Seg;
  Seg.Name = "data";
  Seg.VA = Address;
  Seg.Flags = SegmentFlags::Readable;
  Seg.Data.assign(Value.bytes_begin(), Value.bytes_end());
  Seg.Data.push_back(0);
  Seg.Size = Seg.Data.size();
  Seg.FileSz = Seg.Data.size();
  Img.Segments.push_back(std::move(Seg));
  return Address;
}

// Register a call at the end of block 0 that defines Ret and record its info.
void addCall(MedFunc &F, const std::string &Callee, MedVar Ret,
             std::vector<MedVar> Args = {}) {
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
  CI.Args = std::move(Args);
  F.CallInfos.push_back(CI);
}

// Add the sink call whose argument is being classified; returns its index.
size_t addSink(MedFunc &F, const std::string &Callee,
               std::vector<MedVar> Args) {
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
  size_t Idx =
      addSink(F, "memcpy", {temp(1), temp(2), MedVar::makeConst(8, 8)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Bounded);
  ASSERT_TRUE(C.ConstValue.has_value());
  EXPECT_EQ(*C.ConstValue, 8u);
}

TEST(ArgSlicer, RelocationAddressIsNotANumericCopyBound) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  const MedVar RelocatedBound =
      MedVar::makeConst(8, 8, ConstantAddressProvenance::DataAddress, 0);
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), RelocatedBound});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_FALSE(C.ConstValue.has_value());
  EXPECT_FALSE(C.UpperBound.has_value());
}

TEST(ArgSlicer, CanonicalSingleInputSubbytesKeepsTheLowLaneBound) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  MedFunc F = newFunc();
  defOp(F, NdOp::SUBBYTES, temp(1, 4), MedVar::makeConst(0x1234, 8));
  const size_t Idx = addSink(F, "memcpy", {temp(9), temp(8), temp(1, 4)});

  const ArgClassification C =
      classifyArgument(In, SinkCatalog::defaults(), F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Bounded);
  ASSERT_TRUE(C.UpperBound.has_value());
  EXPECT_EQ(*C.UpperBound, 0x1234u);
  EXPECT_FALSE(C.RequiresPathValidation);
}

TEST(ArgSlicer, NonzeroSubbytesOffsetShiftsTheProvenBound) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  MedFunc F = newFunc();
  MedOp Extract;
  Extract.Opcode = NdOp::SUBBYTES;
  Extract.Output = temp(1, 1);
  Extract.addInput(MedVar::makeConst(0x3400, 8));
  Extract.addInput(MedVar::makeConst(1, 1));
  F.Blocks.front().Ops.push_back(Extract);
  const size_t Idx = addSink(F, "memcpy", {temp(9), temp(8), temp(1, 1)});

  const ArgClassification C =
      classifyArgument(In, SinkCatalog::defaults(), F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Bounded);
  ASSERT_TRUE(C.UpperBound.has_value());
  EXPECT_EQ(*C.UpperBound, 0x34u);
  EXPECT_FALSE(C.RequiresPathValidation);
}

TEST(ArgSlicer, StrlenWithoutDestinationGuardIsNotBounded) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  // n = strlen(...); memcpy(dst, src, n)
  MedFunc F = newFunc();
  addCall(F, "strlen", temp(5), {temp(4)});
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_FALSE(C.UpperBound.has_value());
}

TEST(ArgSlicer, ReadReturnIsTainted) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  // n = read(...); memcpy(dst, src, n)
  MedFunc F = newFunc();
  addCall(F, "read", temp(5), {temp(90), temp(91), temp(92)});
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_EQ(C.TaintSource, "read");
}

TEST(ArgSlicer, CustomReturnSourceDiscoveryDoesNotImplySemantics) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();
  Cat.addSource(SourceEntry{"custom_input", -1});

  MedFunc F = newFunc();
  addCall(F, "custom_input", temp(5));
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_TRUE(C.TaintSource.empty());
}

TEST(ArgSlicer, ScanfReturnCountIsNotInputContent) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  addCall(F, "scanf", temp(5));
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_TRUE(C.TaintSource.empty());
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

TEST(ArgSlicer, StoreAfterLoadDoesNotBoundTheLoadedValue) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  {
    MedOp Sub;
    Sub.Opcode = NdOp::INT_SUB;
    Sub.Output = mkReg(kSP, 1);
    Sub.addInput(mkReg(kSP, 0));
    Sub.addInput(MedVar::makeConst(0x20, 8));
    F.Blocks[0].Ops.push_back(Sub);
  }
  {
    MedOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Output = temp(10);
    Add.addInput(mkReg(kSP, 1));
    Add.addInput(MedVar::makeConst(8, 8));
    F.Blocks[0].Ops.push_back(Add);
  }
  defOp(F, NdOp::LOAD, temp(5), temp(10));
  {
    MedOp Store;
    Store.Opcode = NdOp::STORE;
    Store.addInput(temp(10));
    Store.addInput(MedVar::makeConst(8, 8));
    F.Blocks[0].Ops.push_back(Store);
  }
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_FALSE(C.UpperBound.has_value());
}

TEST(ArgSlicer, TruncatedStackAddressDoesNotBorrowAStackStore) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc();
  MedOp Sub;
  Sub.Opcode = NdOp::INT_SUB;
  Sub.Output = mkReg(kSP, 1);
  Sub.addInput(mkReg(kSP, 0));
  Sub.addInput(MedVar::makeConst(0x20, 8));
  F.Blocks[0].Ops.push_back(Sub);

  MedOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(mkReg(kSP, 1));
  Store.addInput(MedVar::makeConst(7, 8));
  F.Blocks[0].Ops.push_back(Store);

  defOp(F, NdOp::COPY, temp(10, 4), mkReg(kSP, 1));
  defOp(F, NdOp::LOAD, temp(5), temp(10, 4));
  const size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5)});

  const ArgClassification C =
      classifyArgument(In, SinkCatalog::defaults(), F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_FALSE(C.UpperBound.has_value());
}

TEST(ArgSlicer, NarrowEncodedStackImmediateStillFindsStackStore) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc();
  MedOp Sub;
  Sub.Opcode = NdOp::INT_SUB;
  Sub.Output = mkReg(kSP, 1, 8);
  Sub.addInput(mkReg(kSP, 0, 8));
  Sub.addInput(MedVar::makeConst(0x20, 4));
  F.Blocks[0].Ops.push_back(Sub);

  MedOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(mkReg(kSP, 1, 8));
  Store.addInput(MedVar::makeConst(7, 8));
  F.Blocks[0].Ops.push_back(Store);

  defOp(F, NdOp::LOAD, temp(5, 8), mkReg(kSP, 1, 8));
  const size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5, 8)});

  const ArgClassification C =
      classifyArgument(In, SinkCatalog::defaults(), F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Bounded);
  ASSERT_TRUE(C.UpperBound.has_value());
  EXPECT_EQ(*C.UpperBound, 7u);
}

TEST(ArgSlicer, SharedNonFrameAddressDiamondIsCheckedInLinearWork) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  {
    MedOp Sub;
    Sub.Opcode = NdOp::INT_SUB;
    Sub.Output = mkReg(kSP, 1);
    Sub.addInput(mkReg(kSP, 0));
    Sub.addInput(MedVar::makeConst(0x20, 8));
    F.Blocks[0].Ops.push_back(Sub);
  }
  {
    MedOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Output = temp(10);
    Add.addInput(mkReg(kSP, 1));
    Add.addInput(MedVar::makeConst(8, 8));
    F.Blocks[0].Ops.push_back(Add);
  }
  {
    MedOp Store;
    Store.Opcode = NdOp::STORE;
    Store.addInput(temp(10));
    Store.addInput(MedVar::makeConst(7, 8));
    F.Blocks[0].Ops.push_back(Store);
  }

  MedVar Address = param(50);
  for (int I = 0; I < 28; ++I) {
    MedOp Sel;
    Sel.Opcode = NdOp::SELECT;
    Sel.Output = temp(100 + I);
    Sel.addInput(param(1000 + I, 1));
    Sel.addInput(Address);
    Sel.addInput(Address);
    F.Blocks[0].Ops.push_back(Sel);
    Address = Sel.Output;
  }
  {
    MedOp Store;
    Store.Opcode = NdOp::STORE;
    Store.addInput(Address);
    Store.addInput(MedVar::makeConst(1, 8));
    F.Blocks[0].Ops.push_back(Store);
  }
  defOp(F, NdOp::LOAD, temp(5), temp(10));
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5)});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Bounded);
  ASSERT_TRUE(C.UpperBound.has_value());
  EXPECT_EQ(*C.UpperBound, 7u);
}

TEST(ArgSlicer, FutureTaintedStoreDoesNotTaintAnEarlierLoad) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  {
    MedOp Sub;
    Sub.Opcode = NdOp::INT_SUB;
    Sub.Output = mkReg(kSP, 1);
    Sub.addInput(mkReg(kSP, 0));
    Sub.addInput(MedVar::makeConst(0x20, 8));
    F.Blocks[0].Ops.push_back(Sub);
  }
  {
    MedOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Output = temp(10);
    Add.addInput(mkReg(kSP, 1));
    Add.addInput(MedVar::makeConst(8, 8));
    F.Blocks[0].Ops.push_back(Add);
  }
  defOp(F, NdOp::LOAD, temp(5), temp(10));
  addCall(F, "read", temp(6), {temp(90), temp(91), temp(92)});
  {
    MedOp Store;
    Store.Opcode = NdOp::STORE;
    Store.addInput(temp(10));
    Store.addInput(temp(6));
    F.Blocks[0].Ops.push_back(Store);
  }
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_TRUE(C.TaintSource.empty());
}

TEST(ArgSlicer, ConstantAddressLoadWithoutValueIsUnknown) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  defOp(F, NdOp::LOAD, temp(1), MedVar::makeConst(0x4000, 8));
  size_t Idx = addSink(F, "memcpy", {temp(9), temp(8), temp(1)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_FALSE(C.UpperBound.has_value());
}

TEST(ArgSlicer, StatStatusIsNotAProvenCopyBound) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();
  MedFunc F = newFunc();
  addCall(F, "stat", temp(5));
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_FALSE(C.UpperBound.has_value());
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
  ASSERT_TRUE(C.UpperBound.has_value());
  EXPECT_EQ(*C.UpperBound, 0x0fu);
  EXPECT_TRUE(C.RequiresPathValidation);
}

TEST(ArgSlicer, MaskedCallClobberRequiresPathValidation) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  const MedVar Clobber = mkReg(0x20, 1);
  F.CallClobbers.push_back({Clobber, 1});
  MedOp And;
  And.Opcode = NdOp::INT_AND;
  And.Output = temp(2);
  And.addInput(Clobber);
  And.addInput(MedVar::makeConst(0x0f, 8));
  F.Blocks[0].Ops.push_back(And);
  const size_t Idx = addSink(F, "memcpy", {temp(9), temp(8), temp(2)});

  const ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Bounded);
  ASSERT_TRUE(C.UpperBound.has_value());
  EXPECT_EQ(*C.UpperBound, 0x0fu);
  EXPECT_TRUE(C.RequiresPathValidation);
}

TEST(ArgSlicer, MaskedOpaqueValueRequiresPathValidation) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  defOp(F, NdOp::INTRINSIC, temp(1), param(1));
  MedOp And;
  And.Opcode = NdOp::INT_AND;
  And.Output = temp(2);
  And.addInput(temp(1));
  And.addInput(MedVar::makeConst(0x0f, 8));
  F.Blocks[0].Ops.push_back(And);
  const size_t Idx = addSink(F, "memcpy", {temp(9), temp(8), temp(2)});

  const ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Bounded);
  ASSERT_TRUE(C.UpperBound.has_value());
  EXPECT_EQ(*C.UpperBound, 0x0fu);
  EXPECT_TRUE(C.RequiresPathValidation);
}

TEST(ArgSlicer, MaskedMalformedProducerRequiresPathValidation) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  MedOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Output = temp(1);
  F.Blocks[0].Ops.push_back(Copy);
  MedOp And;
  And.Opcode = NdOp::INT_AND;
  And.Output = temp(2);
  And.addInput(temp(1));
  And.addInput(MedVar::makeConst(0x0f, 8));
  F.Blocks[0].Ops.push_back(And);
  const size_t Idx = addSink(F, "memcpy", {temp(9), temp(8), temp(2)});

  const ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Bounded);
  ASSERT_TRUE(C.UpperBound.has_value());
  EXPECT_EQ(*C.UpperBound, 0x0fu);
  EXPECT_TRUE(C.RequiresPathValidation);
}

TEST(ArgSlicer, MaskedCastRequiresPathValidation) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  defOp(F, NdOp::CAST, temp(1), param(1));
  MedOp And;
  And.Opcode = NdOp::INT_AND;
  And.Output = temp(2);
  And.addInput(temp(1));
  And.addInput(MedVar::makeConst(0x0f, 8));
  F.Blocks[0].Ops.push_back(And);
  const size_t Idx = addSink(F, "memcpy", {temp(9), temp(8), temp(2)});

  const ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Bounded);
  ASSERT_TRUE(C.UpperBound.has_value());
  EXPECT_EQ(*C.UpperBound, 0x0fu);
  EXPECT_TRUE(C.RequiresPathValidation);
}

TEST(ArgSlicer, RelocationAddressMaskDoesNotBoundCopyLength) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  defOp(F, NdOp::LOAD, temp(1), MedVar::makeConst(0x4000, 8));
  {
    MedOp And;
    And.Opcode = NdOp::INT_AND;
    And.Output = temp(2);
    And.addInput(temp(1));
    And.addInput(
        MedVar::makeConst(0x0f, 8, ConstantAddressProvenance::DataAddress, 0));
    F.Blocks[0].Ops.push_back(And);
  }
  size_t Idx = addSink(F, "memcpy", {temp(9), temp(8), temp(2)});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_FALSE(C.ConstValue.has_value());
  EXPECT_FALSE(C.UpperBound.has_value());
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
  addCall(F, "read", temp(5), {temp(90), temp(91), temp(92)});
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
  ASSERT_TRUE(C.UpperBound.has_value());
  EXPECT_EQ(*C.UpperBound, 8u);
}

TEST(ArgSlicer, SharedSelectDiamondIsBoundedInLinearWork) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  MedVar Value = MedVar::makeConst(7, 8);
  for (int I = 0; I < 28; ++I) {
    MedOp Sel;
    Sel.Opcode = NdOp::SELECT;
    Sel.Output = temp(100 + I);
    Sel.addInput(param(1000 + I, 1));
    Sel.addInput(Value);
    Sel.addInput(Value);
    F.Blocks[0].Ops.push_back(Sel);
    Value = Sel.Output;
  }
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), Value});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Bounded);
  ASSERT_TRUE(C.UpperBound.has_value());
  EXPECT_EQ(*C.UpperBound, 7u);
}

TEST(ArgSlicer, SharedSelectDiamondPreservesTaintEvidence) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  addCall(F, "read", temp(50), {temp(90), temp(91), temp(92)});
  MedVar Value = temp(50);
  for (int I = 0; I < 28; ++I) {
    MedOp Sel;
    Sel.Opcode = NdOp::SELECT;
    Sel.Output = temp(100 + I);
    Sel.addInput(param(1000 + I, 1));
    Sel.addInput(Value);
    Sel.addInput(Value);
    F.Blocks[0].Ops.push_back(Sel);
    Value = Sel.Output;
  }
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), Value});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_EQ(C.TaintSource, "read");
  EXPECT_FALSE(C.UpperBound.has_value());
}

TEST(ArgSlicer, RecurrentPhiDoesNotPublishAContextualBound) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  PhiNode Phi;
  Phi.Output = temp(50);
  Phi.Args.push_back({0, MedVar::makeConst(8, 8)});
  Phi.Args.push_back({0, Phi.Output});
  F.Blocks[0].Phis.push_back(std::move(Phi));
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(50)});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_FALSE(C.UpperBound.has_value());
}

TEST(ArgSlicer, RelocationAddressDoesNotProveASelectClamp) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  addCall(F, "read", temp(5), {temp(90), temp(91), temp(92)});
  const MedVar RelocatedBound =
      MedVar::makeConst(8, 8, ConstantAddressProvenance::DataAddress, 0);
  {
    MedOp Cmp;
    Cmp.Opcode = NdOp::INT_LESSEQUAL;
    Cmp.Output = temp(6);
    Cmp.addInput(temp(5));
    Cmp.addInput(RelocatedBound);
    F.Blocks[0].Ops.push_back(Cmp);
  }
  {
    MedOp Sel;
    Sel.Opcode = NdOp::SELECT;
    Sel.Output = temp(7);
    Sel.addInput(temp(6));
    Sel.addInput(temp(5));
    Sel.addInput(RelocatedBound);
    F.Blocks[0].Ops.push_back(Sel);
  }
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(7)});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_FALSE(C.ConstValue.has_value());
  EXPECT_FALSE(C.UpperBound.has_value());
}

TEST(ArgSlicer, RelocationAddressDoesNotCapLengthReturn) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  const MedVar RelocatedCap =
      MedVar::makeConst(8, 8, ConstantAddressProvenance::DataAddress, 0);
  addCall(F, "strnlen", temp(5), {temp(4), RelocatedCap});
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5)});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_FALSE(C.ConstValue.has_value());
  EXPECT_FALSE(C.UpperBound.has_value());
}

TEST(ArgSlicer, SelectMaximumIsNotAClamp) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  addCall(F, "read", temp(5), {temp(90), temp(91), temp(92)});
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
    Sel.addInput(MedVar::makeConst(8, 8));
    Sel.addInput(temp(5));
    F.Blocks[0].Ops.push_back(Sel);
  }
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(7)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_FALSE(C.UpperBound.has_value());
}

TEST(ArgSlicer, SignedClampDoesNotBoundAnUnsignedCopyLength) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  addCall(F, "read", temp(5), {temp(90), temp(91), temp(92)});
  {
    MedOp Cmp;
    Cmp.Opcode = NdOp::INT_SLESSEQUAL;
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
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_FALSE(C.UpperBound.has_value());
}

TEST(ArgSlicer, SourceOutputBufferIsTainted) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  addCall(F, "read", temp(5),
          {MedVar::makeConst(0, 8), temp(4), MedVar::makeConst(32, 8)});
  size_t Idx = addSink(F, "strcpy", {temp(1), temp(4)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 1);
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_EQ(C.TaintSource, "read");
}

TEST(ArgSlicer, SourceOutputSubrangeLoadIsTainted) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  addCall(F, "read", temp(5),
          {MedVar::makeConst(0, 8), temp(4), MedVar::makeConst(32, 8)});
  MedOp Add;
  Add.Opcode = NdOp::INT_ADD;
  Add.Output = temp(6);
  Add.addInput(temp(4));
  Add.addInput(MedVar::makeConst(8, 8));
  F.Blocks[0].Ops.push_back(Add);
  defOp(F, NdOp::LOAD, temp(7), temp(6));
  const size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(7)});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_EQ(C.TaintSource, "read");
}

TEST(ArgSlicer, SourceOutputSubrangeRequiresTheSameRelocationOwner) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();
  const MedVar Buffer = MedVar::makeConst(
      0x2000, 8, ConstantAddressProvenance::DataAddress, 0x2000);

  for (const auto &[Owner, Expected] :
       {std::pair<va_t, ArgFlow>{0x2000, ArgFlow::Tainted},
        {0x3000, ArgFlow::Unknown}}) {
    SCOPED_TRACE(Owner);
    MedFunc F = newFunc();
    addCall(F, "read", temp(5),
            {MedVar::makeConst(0, 8), Buffer, MedVar::makeConst(32, 8)});
    const MedVar Address = MedVar::makeConst(
        0x2008, 8, ConstantAddressProvenance::DataAddress, Owner);
    defOp(F, NdOp::LOAD, temp(6), Address);
    const size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(6)});

    ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
    EXPECT_EQ(C.Flow, Expected);
    EXPECT_EQ(C.TaintSource, Expected == ArgFlow::Tainted ? "read" : "");
  }
}

TEST(ArgSlicer, ExactGlobalSourceOutputDoesNotTaintNumericCoincidences) {
  BinaryImage Img;
  Segment Data;
  Data.Name = "data";
  Data.VA = 0x2000;
  Data.Size = 16;
  Data.FileSz = 16;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(16);
  Img.Segments.push_back(std::move(Data));
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();
  const MedVar Global = MedVar::makeConst(
      0x2000, 8, ConstantAddressProvenance::DataAddress, 0x2000);

  MedFunc F = newFunc();
  addCall(F, "read", temp(5),
          {MedVar::makeConst(0, 8), Global, MedVar::makeConst(32, 8)});
  size_t GlobalIdx = addSink(F, "strcpy", {temp(1), Global});
  const MedVar Scalar =
      MedVar::makeConst(0x2000, 8, ConstantAddressProvenance::Scalar);
  size_t ScalarIdx = addSink(F, "strcpy", {temp(1), Scalar});

  ArgClassification GlobalResult = classifyArgument(In, Cat, F, GlobalIdx, 1);
  EXPECT_EQ(GlobalResult.Flow, ArgFlow::Tainted);
  EXPECT_EQ(GlobalResult.TaintSource, "read");
  ArgClassification ScalarResult = classifyArgument(In, Cat, F, ScalarIdx, 1);
  EXPECT_NE(ScalarResult.Flow, ArgFlow::Tainted);
  EXPECT_TRUE(ScalarResult.TaintSource.empty());

  MedFunc Coincidence = newFunc();
  addCall(Coincidence, "read", temp(5),
          {MedVar::makeConst(0, 8), Scalar, MedVar::makeConst(32, 8)});
  size_t CoincidenceIdx = addSink(Coincidence, "strcpy", {temp(1), Scalar});
  ArgClassification CoincidenceResult =
      classifyArgument(In, Cat, Coincidence, CoincidenceIdx, 1);
  EXPECT_NE(CoincidenceResult.Flow, ArgFlow::Tainted);
  EXPECT_TRUE(CoincidenceResult.TaintSource.empty());
}

TEST(ArgSlicer, ScanfVariadicBufferOutputIsTainted) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%s");
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  addCall(F, "scanf", temp(5), {MedVar::makeConst(FormatVA, 8), temp(4)});
  size_t Idx = addSink(F, "strcpy", {temp(1), temp(4)});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 1);
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_EQ(C.TaintSource, "scanf");
}

TEST(ArgSlicer, ScanfFormatDoesNotBorrowAnAdjacentRelocationOwner) {
  auto classifyOwner = [](va_t Owner) {
    BinaryImage Img;
    Segment First;
    First.Name = "first";
    First.VA = 0x1000;
    First.Size = 4;
    First.FileSz = 4;
    First.Flags = SegmentFlags::Readable;
    First.Data = {'x', 'x', 'x', 'x'};
    Img.Segments.push_back(std::move(First));
    Segment Second;
    Second.Name = "second";
    Second.VA = 0x1004;
    Second.Flags = SegmentFlags::Readable;
    Second.Data = {'%', '7', 's', 0};
    Second.Size = Second.Data.size();
    Second.FileSz = Second.Data.size();
    Img.Segments.push_back(std::move(Second));

    AnalysisInput In;
    In.Img = &Img;
    SinkCatalog Cat = SinkCatalog::defaults();
    MedFunc F = newFunc();
    const MedVar Format = MedVar::makeConst(
        0x1004, 8, ConstantAddressProvenance::DataAddress, Owner);
    addCall(F, "scanf", temp(5), {Format, temp(4)});
    size_t Idx = addSink(F, "strcpy", {temp(1), temp(4)});
    return classifyArgument(In, Cat, F, Idx, 1);
  };

  ArgClassification OnePastFirst = classifyOwner(0x1000);
  EXPECT_EQ(OnePastFirst.Flow, ArgFlow::Unknown);
  EXPECT_TRUE(OnePastFirst.TaintSource.empty());

  ArgClassification StartOfSecond = classifyOwner(0x1004);
  EXPECT_EQ(StartOfSecond.Flow, ArgFlow::Tainted);
  EXPECT_EQ(StartOfSecond.TaintSource, "scanf");
}

TEST(ArgSlicer, ScanfFormatMustTerminateWithinItsExactSectionOwner) {
  BinaryImage Img;
  Segment Data;
  Data.Name = "data";
  Data.VA = 0x1000;
  Data.Flags = SegmentFlags::Readable;
  Data.Data = {'%', '7', 's', 0};
  Data.Size = Data.Data.size();
  Data.FileSz = Data.Data.size();
  Img.Segments.push_back(std::move(Data));

  Section Owner;
  Owner.Name = "first";
  Owner.SegmentName = "data";
  Owner.VA = 0x1000;
  Owner.Size = 1;
  Owner.FileSz = 1;
  Owner.Flags = SegmentFlags::Readable;
  Img.Sections.push_back(std::move(Owner));
  Section Neighbour;
  Neighbour.Name = "second";
  Neighbour.SegmentName = "data";
  Neighbour.VA = 0x1001;
  Neighbour.Size = 3;
  Neighbour.FileSz = 3;
  Neighbour.Flags = SegmentFlags::Readable;
  Img.Sections.push_back(std::move(Neighbour));

  AnalysisInput In;
  In.Img = &Img;
  MedFunc F = newFunc();
  const MedVar Format = MedVar::makeConst(
      0x1000, 8, ConstantAddressProvenance::DataAddress, 0x1000);
  addCall(F, "scanf", temp(5), {Format, temp(4)});
  const size_t Idx = addSink(F, "strcpy", {temp(1), temp(4)});

  const ArgClassification C =
      classifyArgument(In, SinkCatalog::defaults(), F, Idx, 1);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_TRUE(C.TaintSource.empty());
}

TEST(ArgSlicer, VersionedScanfSpellingsPreserveOutputSemantics) {
  for (const char *Name : {"__isoc99_scanf", "__isoc23_scanf"}) {
    SCOPED_TRACE(Name);
    BinaryImage Img;
    const va_t FormatVA = addCString(Img, "%s");
    AnalysisInput In;
    In.Img = &Img;
    SinkCatalog Cat = SinkCatalog::defaults();

    MedFunc F = newFunc();
    addCall(F, Name, temp(5), {MedVar::makeConst(FormatVA, 8), temp(4)});
    size_t Idx = addSink(F, "strcpy", {temp(1), temp(4)});

    ArgClassification C = classifyArgument(In, Cat, F, Idx, 1);
    EXPECT_EQ(C.Flow, ArgFlow::Tainted);
    EXPECT_EQ(C.TaintSource, SinkCatalog::normalize(Name));
  }
}

TEST(ArgSlicer, FscanfAndSscanfUseTheirFixedPrefixes) {
  for (const char *Name : {"fscanf", "sscanf"}) {
    SCOPED_TRACE(Name);
    BinaryImage Img;
    const va_t FormatVA = addCString(Img, "%s");
    AnalysisInput In;
    In.Img = &Img;
    SinkCatalog Cat = SinkCatalog::defaults();

    MedFunc F = newFunc();
    if (llvm::StringRef(Name) == "sscanf")
      addCall(F, "read", temp(7),
              {MedVar::makeConst(0, 8), temp(3), MedVar::makeConst(32, 8)});
    addCall(F, Name, temp(5),
            {temp(3), MedVar::makeConst(FormatVA, 8), temp(4)});
    size_t Idx = addSink(F, "strcpy", {temp(1), temp(4)});

    ArgClassification C = classifyArgument(In, Cat, F, Idx, 1);
    EXPECT_EQ(C.Flow, ArgFlow::Tainted);
    EXPECT_EQ(C.TaintSource, Name);
  }
}

TEST(ArgSlicer, ScanfSuppressedConversionConsumesNoOutputArgument) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%*s%s");
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  addCall(F, "scanf", temp(5),
          {MedVar::makeConst(FormatVA, 8), temp(4), temp(6)});
  size_t TaintedIdx = addSink(F, "strcpy", {temp(1), temp(4)});
  size_t ExcessIdx = addSink(F, "strcpy", {temp(1), temp(6)});

  ArgClassification Tainted = classifyArgument(In, Cat, F, TaintedIdx, 1);
  EXPECT_EQ(Tainted.Flow, ArgFlow::Tainted);
  EXPECT_EQ(Tainted.TaintSource, "scanf");
  ArgClassification Excess = classifyArgument(In, Cat, F, ExcessIdx, 1);
  EXPECT_EQ(Excess.Flow, ArgFlow::Unknown);
  EXPECT_TRUE(Excess.TaintSource.empty());
}

TEST(ArgSlicer, BoundedScanfStringRemainsAttackerControlled) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%7s");
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  addCall(F, "scanf", temp(5), {MedVar::makeConst(FormatVA, 8), temp(4)});
  size_t Idx = addSink(F, "strcpy", {temp(1), temp(4)});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 1);
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_EQ(C.TaintSource, "scanf");
}

TEST(ArgSlicer, ScanfCharacterBufferRemainsAttackerControlled) {
  for (const char *Format : {"%c", "%7c"}) {
    SCOPED_TRACE(Format);
    BinaryImage Img;
    const va_t FormatVA = addCString(Img, Format);
    AnalysisInput In;
    In.Img = &Img;
    SinkCatalog Cat = SinkCatalog::defaults();

    MedFunc F = newFunc();
    addCall(F, "scanf", temp(5), {MedVar::makeConst(FormatVA, 8), temp(4)});
    size_t Idx = addSink(F, "strcpy", {temp(1), temp(4)});

    ArgClassification C = classifyArgument(In, Cat, F, Idx, 1);
    EXPECT_EQ(C.Flow, ArgFlow::Tainted);
    EXPECT_EQ(C.TaintSource, "scanf");
  }
}

TEST(ArgSlicer, ScanfWideTextOutputRemainsAttackerControlled) {
  for (const BinaryFormat ImageFormat :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (const char *Format : {"%ls", "%7ls", "%lc"}) {
      SCOPED_TRACE(static_cast<int>(ImageFormat));
      SCOPED_TRACE(Format);
      BinaryImage Img;
      Img.Arch = Arch::X64;
      Img.Format = ImageFormat;
      const va_t FormatVA = addCString(Img, Format);
      AnalysisInput In;
      In.Img = &Img;
      SinkCatalog Cat = SinkCatalog::defaults();

      MedFunc F = newFunc();
      addCall(F, "scanf", temp(5), {MedVar::makeConst(FormatVA, 8), temp(4)});
      size_t Idx = addSink(F, "wprintf", {temp(4)});

      ArgClassification C = classifyArgument(In, Cat, F, Idx, 0);
      EXPECT_EQ(C.Flow, ArgFlow::Tainted);
      EXPECT_EQ(C.TaintSource, "scanf");
    }
}

TEST(ArgSlicer, BoundedScanfTextReportsPlatformByteExtent) {
  struct Case {
    const char *Format;
    BinaryFormat ImageFormat;
    uint64_t ExpectedBytes;
  };
  for (const Case C : {
           Case{"%7s", BinaryFormat::COFF, 8},
           Case{"%7ls", BinaryFormat::COFF, 16},
           Case{"%7ls", BinaryFormat::ELF, 32},
           Case{"%7ls", BinaryFormat::MachO, 32},
       }) {
    SCOPED_TRACE(static_cast<int>(C.ImageFormat));
    SCOPED_TRACE(C.Format);
    BinaryImage Img;
    Img.Arch = Arch::X64;
    Img.Format = C.ImageFormat;
    const va_t FormatVA = addCString(Img, C.Format);
    const std::vector<MedVar> Args = {MedVar::makeConst(FormatVA, 8), temp(4)};

    const std::optional<detail::FormattedSourceOutputs> Outputs =
        detail::recoverFormattedSourceOutputs(&Img, "scanf", Args);
    ASSERT_TRUE(Outputs.has_value());
    ASSERT_EQ(Outputs->BoundedTextArgs.size(), 1u);
    EXPECT_EQ(Outputs->BoundedTextArgs.front().MaxChars, 7u);
    EXPECT_EQ(Outputs->BoundedTextArgs.front().MaxBytes, C.ExpectedBytes);
  }
}

TEST(ArgSlicer, OverflowingScanfWideTextExtentFailsClosed) {
  struct Case {
    const char *Format;
    BinaryFormat ImageFormat;
  };
  for (const Case C : {
           Case{"%9223372036854775807ls", BinaryFormat::COFF},
           Case{"%4611686018427387903ls", BinaryFormat::ELF},
           Case{"%4611686018427387903ls", BinaryFormat::MachO},
       }) {
    SCOPED_TRACE(static_cast<int>(C.ImageFormat));
    BinaryImage Img;
    Img.Arch = Arch::X64;
    Img.Format = C.ImageFormat;
    const va_t FormatVA = addCString(Img, C.Format);
    const std::vector<MedVar> Args = {MedVar::makeConst(FormatVA, 8), temp(4)};
    EXPECT_FALSE(
        detail::recoverFormattedSourceOutputs(&Img, "scanf", Args).has_value());

    AnalysisInput In;
    In.Img = &Img;
    MedFunc F = newFunc();
    addCall(F, "scanf", temp(5), Args);
    const size_t Idx = addSink(F, "wprintf", {temp(4)});
    const ArgClassification Classification =
        classifyArgument(In, SinkCatalog::defaults(), F, Idx, 0);
    EXPECT_EQ(Classification.Flow, ArgFlow::Unknown);
    EXPECT_TRUE(Classification.TaintSource.empty());
  }
}

TEST(ArgSlicer, ScanfReturnIsNotBufferContent) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%s");
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  addCall(F, "scanf", temp(5), {MedVar::makeConst(FormatVA, 8), temp(4)});
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5)});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_TRUE(C.TaintSource.empty());
}

TEST(ArgSlicer, ScanfNumericOutputLoadIsTainted) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%zu");
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  addCall(F, "scanf", temp(5), {MedVar::makeConst(FormatVA, 8), temp(4)});
  defOp(F, NdOp::LOAD, temp(6), temp(4));
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(6)});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_EQ(C.TaintSource, "scanf");
}

TEST(ArgSlicer, FutureScanfNumericOutputDoesNotTaintEarlierLoad) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%zu");
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  defOp(F, NdOp::LOAD, temp(6), temp(4));
  addCall(F, "scanf", temp(5), {MedVar::makeConst(FormatVA, 8), temp(4)});
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(6)});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_TRUE(C.TaintSource.empty());
}

TEST(ArgSlicer, ScanfPercentNIsNotTreatedAsUnboundedInput) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%n");
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  addCall(F, "scanf", temp(5), {MedVar::makeConst(FormatVA, 8), temp(4)});
  defOp(F, NdOp::LOAD, temp(6), temp(4));
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(6)});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_TRUE(C.TaintSource.empty());
}

TEST(ArgSlicer, ScanfPercentNAfterVariableInputIsTainted) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%s%n");
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  addCall(F, "scanf", temp(5),
          {MedVar::makeConst(FormatVA, 8), temp(4), temp(7)});
  defOp(F, NdOp::LOAD, temp(6), temp(7));
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(6)});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_EQ(C.TaintSource, "scanf");
}

TEST(ArgSlicer, ScanfPercentNAfterSuppressedInputRemainsMayTainted) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%*s%n");
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  addCall(F, "scanf", temp(5), {MedVar::makeConst(FormatVA, 8), temp(7)});
  defOp(F, NdOp::LOAD, temp(6), temp(7));
  const size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(6)});

  const ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_EQ(C.TaintSource, "scanf");
}

TEST(ArgSlicer, ScanfPercentNAfterFixedCharacterCountIsNotTainted) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%7c%n");
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  addCall(F, "scanf", temp(5),
          {MedVar::makeConst(FormatVA, 8), temp(4), temp(7)});
  defOp(F, NdOp::LOAD, temp(6), temp(7));
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(6)});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_TRUE(C.TaintSource.empty());
}

TEST(ArgSlicer, SscanfNumericOutputRequiresTaintedInput) {
  for (bool TaintedInput : {false, true}) {
    SCOPED_TRACE(TaintedInput);
    BinaryImage Img;
    const va_t FormatVA = addCString(Img, "%zu");
    AnalysisInput In;
    In.Img = &Img;
    SinkCatalog Cat = SinkCatalog::defaults();

    MedFunc F = newFunc();
    if (TaintedInput)
      addCall(F, "read", temp(7),
              {MedVar::makeConst(0, 8), temp(3), MedVar::makeConst(32, 8)});
    addCall(F, "sscanf", temp(5),
            {temp(3), MedVar::makeConst(FormatVA, 8), temp(4)});
    defOp(F, NdOp::LOAD, temp(6), temp(4));
    size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(6)});

    ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
    EXPECT_EQ(C.Flow, TaintedInput ? ArgFlow::Tainted : ArgFlow::Unknown);
    EXPECT_EQ(C.TaintSource, TaintedInput ? "sscanf" : "");
  }
}

TEST(ArgSlicer, FutureSourceOutputDoesNotTaintEarlierUse) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  size_t Idx = addSink(F, "strcpy", {temp(1), temp(4)});
  addCall(F, "read", temp(5),
          {MedVar::makeConst(0, 8), temp(4), MedVar::makeConst(32, 8)});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 1);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_TRUE(C.TaintSource.empty());
}

TEST(ArgSlicer, SourceAfterSinkInLoopTaintsTheNextIteration) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc();
  F.Blocks[0].Preds = {0};
  F.Blocks[0].Succs = {0};
  size_t Idx = addSink(F, "strcpy", {temp(1), temp(4)});
  addCall(F, "read", temp(5),
          {MedVar::makeConst(0, 8), temp(4), MedVar::makeConst(32, 8)});

  ArgClassification C = classifyArgument(In, Cat, F, Idx, 1);
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_EQ(C.TaintSource, "read");
}

TEST(ArgSlicer, SourceAndSinkOnSiblingPathsDoNotShareTaint) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F;
  F.Name = "f";
  F.Entry = 0x100;
  F.Blocks.resize(4);
  for (int I = 0; I < 4; ++I)
    F.Blocks[I].Id = I;
  F.Blocks[0].Succs = {1, 2};
  F.Blocks[1].Preds = {0};
  F.Blocks[1].Succs = {3};
  F.Blocks[2].Preds = {0};
  F.Blocks[2].Succs = {3};
  F.Blocks[3].Preds = {1, 2};

  MedOp Read;
  Read.Opcode = NdOp::CALL;
  Read.Output = temp(5);
  Read.addInput(MedVar::makeConst(0x9000, 8));
  F.Blocks[1].Ops.push_back(Read);
  MedCallInfo ReadInfo;
  ReadInfo.BlockId = 1;
  ReadInfo.OpIdx = 0;
  ReadInfo.TargetName = "read";
  ReadInfo.Args = {MedVar::makeConst(0, 8), temp(4), MedVar::makeConst(32, 8)};
  F.CallInfos.push_back(ReadInfo);

  MedOp Copy;
  Copy.Opcode = NdOp::CALL;
  Copy.addInput(MedVar::makeConst(0x8000, 8));
  F.Blocks[2].Ops.push_back(Copy);
  MedCallInfo CopyInfo;
  CopyInfo.BlockId = 2;
  CopyInfo.OpIdx = 0;
  CopyInfo.TargetName = "strcpy";
  CopyInfo.Args = {temp(1), temp(4)};
  F.CallInfos.push_back(CopyInfo);

  ArgClassification C = classifyArgument(In, Cat, F, 1, 1);
  EXPECT_EQ(C.Flow, ArgFlow::Unknown);
  EXPECT_TRUE(C.TaintSource.empty());
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
