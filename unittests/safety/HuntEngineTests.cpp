//===- HuntEngineTests.cpp - Overflow verdicts and witnesses -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/safety/HuntEngine.h"
#include "neverd/safety/SinkScanner.h"

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

struct Builder {
  MedFunc F;
  explicit Builder(const std::string &Name = "f") {
    F.Entry = 0x100;
    F.Name = Name;
    MedBlock B;
    B.Id = 0;
    F.Blocks.push_back(std::move(B));
  }
  void op(NdOp Op, MedVar Out, std::vector<MedVar> Ins) {
    MedOp O;
    O.Opcode = Op;
    O.Output = Out;
    for (auto &I : Ins)
      O.addInput(I);
    F.Blocks[0].Ops.push_back(O);
  }
  void call(const std::string &Name, MedVar Ret, std::vector<MedVar> Args) {
    int Idx = static_cast<int>(F.Blocks[0].Ops.size());
    MedOp O;
    O.Opcode = NdOp::CALL;
    O.Output = Ret;
    O.addInput(MedVar::makeConst(0x9000, 8));
    F.Blocks[0].Ops.push_back(O);
    MedCallInfo CI;
    CI.BlockId = 0;
    CI.OpIdx = Idx;
    CI.TargetName = Name;
    CI.Args = std::move(Args);
    F.CallInfos.push_back(CI);
  }
};

// Run the hunt over one function and return the copy finding, if any.
std::optional<Finding> hunt(MedFunc &F, bool StackRegs = false,
                            LowFunc *LF = nullptr, Arch A = Arch::Unknown) {
  BinaryImage Img;
  Img.Arch = A;
  std::vector<MedFunc> Funcs{F};
  std::vector<LowFunc> Lows;
  if (LF) {
    LF->Entry = F.Entry;
    Lows.push_back(*LF);
  }
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &Funcs;
  if (!Lows.empty())
    In.LowFuncs = &Lows;
  In.StackRegsKnown = StackRegs;
  In.StackPointerReg = kSP;
  SinkCatalog Cat = SinkCatalog::defaults();
  SafetyBudgets Budgets;
  for (const SinkSite &S : scanSinks(In, Cat))
    if (S.Kind == SinkKind::Copy)
      if (auto Fnd = huntSink(In, Cat, Budgets, Funcs[0], S))
        return Fnd;
  return std::nullopt;
}

LowOp lop(NdOp Opcode, NdVar Output, std::vector<NdVar> Inputs, va_t Addr = 0) {
  LowOp O;
  O.Opcode = Opcode;
  O.Output = Output;
  O.Addr = Addr;
  for (const NdVar &I : Inputs)
    O.addInput(I);
  return O;
}

LowFunc guardedMemcpyLow(va_t MemcpyVA, bool OverflowGuard) {
  constexpr uint64_t kRdx = 16;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock B0, B1, B2;
  B0.Id = 0;
  B0.StartAddr = kEntry;
  B0.EndAddr = kEntry + 0x10;
  B1.Id = 1;
  B1.StartAddr = MemcpyVA;
  B1.EndAddr = MemcpyVA + 0x10;
  B2.Id = 2;
  B2.StartAddr = MemcpyVA + 0x10;
  B2.EndAddr = MemcpyVA + 0x20;
  if (OverflowGuard)
    B0.Ops.push_back(lop(NdOp::INT_LESS, NdVar::reg(kFlag, 1),
                         {NdVar::cst(16, 8), NdVar::reg(kRdx, 8)}));
  else
    B0.Ops.push_back(lop(NdOp::INT_LESSEQUAL, NdVar::reg(kFlag, 1),
                         {NdVar::reg(kRdx, 8), NdVar::cst(8, 8)}));
  B0.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                       {NdVar::cst(MemcpyVA, 8), NdVar::reg(kFlag, 1)}));
  B0.Succs = {1, 2};
  B1.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, MemcpyVA));
  B1.Succs = {2};
  B1.Preds = {0};
  B2.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  B2.Preds = {0, 1};
  LF.Blocks.push_back(std::move(B0));
  LF.Blocks.push_back(std::move(B1));
  LF.Blocks.push_back(std::move(B2));
  return LF;
}

LowFunc fortifiedMemcpyLow(va_t MemcpyVA) {
  constexpr uint64_t kRuntimeCap = 24;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock B0, B1, B2;
  B0.Id = 0;
  B0.StartAddr = kEntry;
  B0.EndAddr = kEntry + 0x10;
  B1.Id = 1;
  B1.StartAddr = MemcpyVA;
  B1.EndAddr = MemcpyVA + 0x10;
  B2.Id = 2;
  B2.StartAddr = MemcpyVA + 0x10;
  B2.EndAddr = MemcpyVA + 0x20;
  B0.Ops.push_back(lop(NdOp::INT_LESSEQUAL, NdVar::reg(kFlag, 1),
                       {NdVar::reg(kRuntimeCap, 8), NdVar::cst(8, 8)}));
  B0.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                       {NdVar::cst(MemcpyVA, 8), NdVar::reg(kFlag, 1)}));
  B0.Succs = {1, 2};
  B1.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, MemcpyVA));
  B1.Succs = {2};
  B1.Preds = {0};
  B2.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  B2.Preds = {0, 1};
  LF.Blocks.push_back(std::move(B0));
  LF.Blocks.push_back(std::move(B1));
  LF.Blocks.push_back(std::move(B2));
  return LF;
}

LowFunc strlenGuardedStrcpyLow(va_t StrlenVA, va_t StrcpyVA,
                               bool OverflowGuard) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock B0, B1, B2;
  B0.Id = 0;
  B0.StartAddr = kEntry;
  B0.EndAddr = StrlenVA + 8;
  B1.Id = 1;
  B1.StartAddr = StrcpyVA;
  B1.EndAddr = StrcpyVA + 0x10;
  B2.Id = 2;
  B2.StartAddr = StrcpyVA + 0x10;
  B2.EndAddr = StrcpyVA + 0x20;
  B0.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9100, 8)}, StrlenVA));
  if (OverflowGuard)
    B0.Ops.push_back(lop(NdOp::INT_LESS, NdVar::reg(kFlag, 1),
                         {NdVar::cst(16, 8), NdVar::reg(kRax, 8)}));
  else
    B0.Ops.push_back(lop(NdOp::INT_LESS, NdVar::reg(kFlag, 1),
                         {NdVar::reg(kRax, 8), NdVar::cst(16, 8)}));
  B0.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                       {NdVar::cst(StrcpyVA, 8), NdVar::reg(kFlag, 1)}));
  B0.Succs = {1, 2};
  B1.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, StrcpyVA));
  B1.Succs = {2};
  B1.Preds = {0};
  B2.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  B2.Preds = {0, 1};
  LF.Blocks.push_back(std::move(B0));
  LF.Blocks.push_back(std::move(B1));
  LF.Blocks.push_back(std::move(B2));
  return LF;
}

LowFunc reachableSinkLow(va_t SinkVA) {
  LowFunc LF;
  LowBlock B;
  B.Id = 0;
  B.StartAddr = SinkVA;
  B.EndAddr = SinkVA + 8;
  B.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, SinkVA));
  B.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  LF.Blocks.push_back(std::move(B));
  return LF;
}

LowFunc unmodelledThenSinkLow(va_t SinkVA) {
  LowFunc LF = reachableSinkLow(SinkVA);
  LF.Blocks[0].Ops.insert(
      LF.Blocks[0].Ops.begin(),
      lop(NdOp::INTRINSIC, NdVar::tmp(7, 8), {NdVar::reg(0, 8)}, SinkVA - 4));
  LF.Blocks[0].StartAddr = SinkVA - 4;
  return LF;
}

LowFunc unresolvedIndirectThenSinkLow(va_t SinkVA) {
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = kEntry + 8;
  Entry.Ops.push_back(
      lop(NdOp::INDIR_BR, NdVar{}, {NdVar::reg(64, 8)}, kEntry));
  Entry.Succs = {1, 2};
  Sink.Id = 1;
  Sink.StartAddr = SinkVA;
  Sink.EndAddr = SinkVA + 8;
  Sink.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, SinkVA));
  Sink.Preds = {0};
  Exit.Id = 2;
  Exit.StartAddr = SinkVA + 8;
  Exit.EndAddr = SinkVA + 16;
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  Exit.Preds = {0};
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

MedVar rdxLen() {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.RegOff = 16;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return V;
}

} // namespace

TEST(HuntEngine, TaintedStrcpyIntoStackBufferIsUnsafe) {
  Builder B("main");
  B.op(NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("strcpy", temp(0), {temp(10), param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = reachableSinkLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/true, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, TaintedStrcpyWithoutReachabilityIsUnknown) {
  Builder B("main");
  B.op(NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("strcpy", temp(0), {temp(10), param(2)});

  auto Fnd = hunt(B.F, /*StackRegs=*/true);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, UnmodelledOperationBeforeSinkFailsClosed) {
  Builder B("main");
  B.op(NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("strcpy", temp(0), {temp(10), param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = unmodelledThenSinkLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/true, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, IncompleteInstructionLiftFailsClosed) {
  Builder B("main");
  B.op(NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("strcpy", temp(0), {temp(10), param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = reachableSinkLow(0x400010);
  LF.DecodedInstructionCount = 2;
  LF.LiftedInstructionCount = 1;

  auto Fnd = hunt(B.F, /*StackRegs=*/true, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, UnresolvedIndirectBranchDoesNotGuessASuccessor) {
  Builder B("main");
  B.F.Entry = 0x400000;
  B.op(NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("strcpy", temp(0), {temp(10), param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = unresolvedIndirectThenSinkLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/true, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, MissingCallAddressDoesNotGuessFirstCallAsSink) {
  Builder B("main");
  B.op(NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("strcpy", temp(0), {temp(10), param(2)});
  LowFunc LF = reachableSinkLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/true, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, TaintedMemcpyIntoHeapIsUnsafe) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5), {});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  B.F.CC = CallingConv::SysV_AMD64;
  LowFunc LF = reachableSinkLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  ASSERT_TRUE(Fnd->Capacity.has_value());
  EXPECT_EQ(*Fnd->Capacity, 16u);
}

TEST(HuntEngine, ConstLengthWithinCapacityIsSafe) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), MedVar::makeConst(8, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
}

TEST(HuntEngine, ConstLengthExceedingCapacityWithoutReachabilityIsUnknown) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), MedVar::makeConst(32, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, ReachableConstLengthExceedingCapacityIsUnsafe) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = reachableSinkLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, ConstLengthWithinStackFrameBoundIsUnknown) {
  Builder B;
  B.op(NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("memcpy", temp(0), {temp(10), temp(2), MedVar::makeConst(16, 8)});

  auto Fnd = hunt(B.F, /*StackRegs=*/true);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->SkipReason.empty());
}

TEST(HuntEngine, StrlenWithoutDestinationGuardIsUnknown) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("strlen", temp(5), {});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->SkipReason.empty());
}

TEST(HuntEngine, MaskBoundMustFitDestinationBeforeSafeSkip) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(NdOp::INT_AND, temp(5), {param(1), MedVar::makeConst(0xff, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->SkipReason.empty());
}

TEST(HuntEngine, MaskBoundWithinDestinationIsSafeSkip) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(NdOp::INT_AND, temp(5), {param(1), MedVar::makeConst(0x0f, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_FALSE(Fnd->SkipReason.empty());
}

TEST(HuntEngine, FortifiedCopyIsSafe) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  // __strcpy_chk(dst, src, dstlen)
  B.call("___strcpy_chk", temp(0),
         {temp(1), param(2), MedVar::makeConst(16, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_NE(Fnd->Detail.find("fortified"), std::string::npos);
}

TEST(HuntEngine, FortifiedCopyWithoutDestinationCapacityIsUnknown) {
  Builder B;
  B.call("___strcpy_chk", temp(0),
         {temp(1), param(2), MedVar::makeConst(16, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
}

TEST(HuntEngine, FortifiedBoundLargerThanObjectStillAllowsOverflow) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call(
      "___memcpy_chk", temp(0),
      {temp(1), temp(2), MedVar::makeConst(12, 8), MedVar::makeConst(16, 8)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = reachableSinkLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, FortifiedBoundRejectingCopyPreventsOverflow) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("___memcpy_chk", temp(0),
         {temp(1), temp(2), MedVar::makeConst(12, 8), MedVar::makeConst(8, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, FortifiedRuntimeBoundParticipatesInOverflowQuery) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("___memcpy_chk", temp(0), {temp(1), temp(2), rdxLen(), mkReg(24, 0)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = fortifiedMemcpyLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, ConstantStringPointerIsNotAConstantStringLength) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("strcpy", temp(0), {temp(1), MedVar::makeConst(0x100000, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, AppendRequiresDestinationStringState) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("strlen", temp(5), {param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.call("strcat", temp(0), {temp(1), param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = strlenGuardedStrcpyLow(0x400004, 0x400010,
                                      /*OverflowGuard=*/false);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
}

TEST(HuntEngine, SizeLimitedStringCopyRequiresSourceLength) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("strlcpy", temp(0),
         {temp(1), MedVar::makeConst(0x100000, 8), MedVar::makeConst(16, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, WideCopyFailsClosedUntilElementWidthIsModelled) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("wmemcpy", temp(0), {temp(1), temp(2), MedVar::makeConst(4, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
}

TEST(HuntEngine, UnknownCapacityFailsClosed) {
  Builder B;
  // dst is a bare load; length comes from read (tainted) but capacity unknown.
  B.op(NdOp::LOAD, temp(1), {MedVar::makeConst(0x4000, 8)});
  B.call("read", temp(5), {});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
}

TEST(HuntEngine, PathConstraintKeepsCopyInBound) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), rdxLen()});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = guardedMemcpyLow(0x400010, /*OverflowGuard=*/false);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, FunctionEntrySelectsTheStartBlock) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), rdxLen()});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = guardedMemcpyLow(0x400010, /*OverflowGuard=*/false);
  std::swap(LF.Blocks[0], LF.Blocks[1]);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, PathConstraintWitnessesOverflow) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), rdxLen()});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = guardedMemcpyLow(0x400010, /*OverflowGuard=*/true);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->Witness.empty());
  EXPECT_FALSE(Fnd->Constraints.empty());
}

TEST(HuntEngine, StrlenGuardKeepsStrcpyInBound) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("strlen", temp(5), {param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.call("strcpy", temp(0), {temp(1), param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF =
      strlenGuardedStrcpyLow(0x400004, 0x400010, /*OverflowGuard=*/false);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, StrlenGuardWitnessesOverflow) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("strlen", temp(5), {param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.call("strcpy", temp(0), {temp(1), param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF =
      strlenGuardedStrcpyLow(0x400004, 0x400010, /*OverflowGuard=*/true);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->Witness.empty());
}
