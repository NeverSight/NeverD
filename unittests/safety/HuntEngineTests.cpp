//===- HuntEngineTests.cpp - Overflow verdicts and witnesses -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/HuntEngine.h"

#include "neverd/safety/SinkScanner.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"

#include "gtest/gtest.h"

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

LowOp lop(NdOp Opcode, NdVar Output, std::vector<NdVar> Inputs,
          va_t Addr = 0) {
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

LowFunc strlenGuardedStrcpyLow(va_t StrlenVA, va_t StrcpyVA, bool OverflowGuard) {
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
  B.op(NdOp::INT_SUB, mkReg(kSP, 1), {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("strcpy", temp(0), {temp(10), param(2)});

  auto Fnd = hunt(B.F, /*StackRegs=*/true);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, TaintedMemcpyIntoHeapIsUnsafe) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5), {});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});

  auto Fnd = hunt(B.F);
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

TEST(HuntEngine, ConstLengthExceedingCapacityIsUnsafe) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), MedVar::makeConst(32, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, StrlenBoundedCopyIsSafeSkip) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("strlen", temp(5), {});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_FALSE(Fnd->SkipReason.empty());
}

TEST(HuntEngine, FortifiedCopyIsSafe) {
  Builder B;
  // __strcpy_chk(dst, src, dstlen)
  B.call("___strcpy_chk", temp(0),
         {temp(1), param(2), MedVar::makeConst(16, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_NE(Fnd->Detail.find("fortified"), std::string::npos);
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
  LowFunc LF = strlenGuardedStrcpyLow(0x400004, 0x400010, /*OverflowGuard=*/false);
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
  LowFunc LF = strlenGuardedStrcpyLow(0x400004, 0x400010, /*OverflowGuard=*/true);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->Witness.empty());
}
