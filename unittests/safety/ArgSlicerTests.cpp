//===- ArgSlicerTests.cpp - Backward argument classification -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

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
  addCall(F, "read", temp(5));
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_EQ(C.TaintSource, "read");
}

TEST(ArgSlicer, CustomReturnSourceUsesCatalogSemantics) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();
  Cat.addSource(SourceEntry{"custom_input", -1});

  MedFunc F = newFunc();
  addCall(F, "custom_input", temp(5));
  size_t Idx = addSink(F, "memcpy", {temp(1), temp(2), temp(5)});
  ArgClassification C = classifyArgument(In, Cat, F, Idx, 2);
  EXPECT_EQ(C.Flow, ArgFlow::Tainted);
  EXPECT_EQ(C.TaintSource, "custom_input");
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
  addCall(F, "read", temp(6));
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
  ASSERT_TRUE(C.UpperBound.has_value());
  EXPECT_EQ(*C.UpperBound, 8u);
}

TEST(ArgSlicer, SelectMaximumIsNotAClamp) {
  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

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
  addCall(F, "read", temp(5));
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
  for (const char *Format : {"%ls", "%7ls", "%lc"}) {
    SCOPED_TRACE(Format);
    BinaryImage Img;
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
