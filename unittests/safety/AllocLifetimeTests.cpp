//===- AllocLifetimeTests.cpp - Heap lifetime defect detection -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/safety/AllocLifetime.h"

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

MedVar mkReg(uint64_t Off, int Ver, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = static_cast<int>(Off);
  V.RegOff = Off;
  V.SSAVer = Ver;
  V.Size = Size;
  return V;
}

LowOp lowOp(NdOp Opcode, NdVar Output, std::vector<NdVar> Inputs,
            va_t Address = 0) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.Addr = Address;
  for (NdVar &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

LowFunc solverHeavyReturnedPath(va_t EntryVA) {
  constexpr uint64_t kInputA = 16;
  constexpr uint64_t kInputB = 24;
  constexpr uint64_t kAValue = 201;
  constexpr uint64_t kA = 202;
  constexpr uint64_t kBValue = 203;
  constexpr uint64_t kB = 204;
  constexpr uint64_t kNotB = 205;
  constexpr uint64_t kClause1 = 206;
  constexpr uint64_t kClause2 = 207;
  constexpr uint64_t kFlag = 208;

  LowFunc F;
  F.Entry = EntryVA;
  F.DecodedInstructionCount = 1;
  F.LiftedInstructionCount = 1;
  F.Blocks.resize(3);
  for (int I = 0; I < 3; ++I)
    F.Blocks[I].Id = I;

  LowBlock &Entry = F.Blocks[0];
  Entry.StartAddr = EntryVA;
  Entry.EndAddr = EntryVA + 0x10;
  Entry.Succs = {1, 2};
  Entry.Ops.push_back(lowOp(NdOp::INT_AND, NdVar::reg(kAValue, 8),
                            {NdVar::reg(kInputA, 8), NdVar::cst(1, 8)}));
  Entry.Ops.push_back(lowOp(NdOp::INT_NOTEQUAL, NdVar::reg(kA, 1),
                            {NdVar::reg(kAValue, 8), NdVar::cst(0, 8)}));
  Entry.Ops.push_back(lowOp(NdOp::INT_AND, NdVar::reg(kBValue, 8),
                            {NdVar::reg(kInputB, 8), NdVar::cst(1, 8)}));
  Entry.Ops.push_back(lowOp(NdOp::INT_NOTEQUAL, NdVar::reg(kB, 1),
                            {NdVar::reg(kBValue, 8), NdVar::cst(0, 8)}));
  Entry.Ops.push_back(
      lowOp(NdOp::INT_NOT, NdVar::reg(kNotB, 1), {NdVar::reg(kB, 1)}));
  Entry.Ops.push_back(lowOp(NdOp::INT_OR, NdVar::reg(kClause1, 1),
                            {NdVar::reg(kA, 1), NdVar::reg(kB, 1)}));
  Entry.Ops.push_back(lowOp(NdOp::INT_OR, NdVar::reg(kClause2, 1),
                            {NdVar::reg(kA, 1), NdVar::reg(kNotB, 1)}));
  Entry.Ops.push_back(
      lowOp(NdOp::INT_AND, NdVar::reg(kFlag, 1),
            {NdVar::reg(kClause1, 1), NdVar::reg(kClause2, 1)}));
  Entry.Ops.push_back(
      lowOp(NdOp::COND_BR, NdVar{},
            {NdVar::cst(EntryVA + 0x10, 8), NdVar::reg(kFlag, 1)}));

  LowBlock &Read = F.Blocks[1];
  Read.StartAddr = EntryVA + 0x10;
  Read.EndAddr = EntryVA + 0x20;
  Read.Preds = {0};
  Read.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}));

  LowBlock &Exit = F.Blocks[2];
  Exit.StartAddr = EntryVA + 0x20;
  Exit.EndAddr = EntryVA + 0x30;
  Exit.Preds = {0};
  Exit.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}));
  return F;
}

struct FB {
  MedFunc F;
  FB(const std::string &Name, va_t Entry) {
    F.Name = Name;
    F.Entry = Entry;
  }
  int block() {
    MedBlock B;
    B.Id = static_cast<int>(F.Blocks.size());
    F.Blocks.push_back(std::move(B));
    return F.Blocks.back().Id;
  }
  void succ(int From, int To) {
    F.Blocks[From].Succs.push_back(To);
    F.Blocks[To].Preds.push_back(From);
  }
  void op(int Blk, NdOp Op, MedVar Out, std::vector<MedVar> Ins,
          va_t Addr = 0) {
    MedOp O;
    O.Opcode = Op;
    O.Output = Out;
    O.Addr = Addr;
    for (auto &I : Ins)
      O.addInput(I);
    F.Blocks[Blk].Ops.push_back(O);
  }
  void call(int Blk, const std::string &Name, MedVar Ret,
            std::vector<MedVar> Args, va_t Target = 0x9000, va_t Addr = 0) {
    int Idx = static_cast<int>(F.Blocks[Blk].Ops.size());
    MedOp O;
    O.Opcode = NdOp::CALL;
    O.Output = Ret;
    O.Addr = Addr;
    O.addInput(MedVar::makeConst(Target, 8));
    F.Blocks[Blk].Ops.push_back(O);
    MedCallInfo CI;
    CI.BlockId = Blk;
    CI.OpIdx = Idx;
    CI.TargetAddr = Target;
    CI.TargetName = Name;
    CI.Args = std::move(Args);
    F.CallInfos.push_back(CI);
  }
  void ret(int Blk, std::vector<MedVar> Ins) {
    op(Blk, NdOp::RETURN, MedVar{}, Ins);
  }
};

std::vector<Finding> audit(std::vector<MedFunc> Funcs,
                           const BinaryImage *Image = nullptr,
                           bool StackRegs = false,
                           bool IncludeStackReads = false) {
  static BinaryImage Img;
  AnalysisInput In;
  In.Img = Image ? Image : &Img;
  In.MedFuncs = &Funcs;
  In.StackRegsKnown = StackRegs;
  In.StackPointerReg = kSP;
  return IncludeStackReads
             ? auditMemory(In, SinkCatalog::defaults(), SafetyBudgets{})
             : auditHeap(In, SinkCatalog::defaults(), SafetyBudgets{});
}

bool has(const std::vector<Finding> &Fs, VulnClass C) {
  for (const Finding &F : Fs)
    if (F.Class == C)
      return true;
  return false;
}

size_t count(const std::vector<Finding> &Fs, VulnClass C) {
  size_t N = 0;
  for (const Finding &F : Fs)
    if (F.Class == C)
      ++N;
  return N;
}

const Finding *find(const std::vector<Finding> &Fs, VulnClass C) {
  for (const Finding &F : Fs)
    if (F.Class == C)
      return &F;
  return nullptr;
}

} // namespace

TEST(AllocLifetime, LeakWhenNeitherFreedNorEscaped) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.ret(b0, {});
  auto Fs = audit({B.F});
  EXPECT_TRUE(has(Fs, VulnClass::HeapLeak));
}

TEST(AllocLifetime, FindingUsesAllocatorIdentityOrigin) {
  BinaryImage Img;
  Import Malloc;
  Malloc.Name = "malloc";
  Malloc.IATAddr = 0x9000;
  Img.Imports.push_back(Malloc);

  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, 0x400);
  B.ret(b0, {});
  auto Fs = audit({B.F}, &Img);
  ASSERT_EQ(Fs.size(), 1u);
  EXPECT_EQ(Fs[0].Class, VulnClass::HeapLeak);
  EXPECT_EQ(Fs[0].Source, NameSource::Import);
}

TEST(AllocLifetime, ResolvedImportIdentityDrivesAllocationSemantics) {
  BinaryImage Img;
  Img.Imports.push_back({"runtime", "malloc", 0, 0x9000});

  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "sub_9000", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, 0x400);
  B.ret(b0, {});

  auto Fs = audit({B.F}, &Img);
  ASSERT_EQ(Fs.size(), 1u);
  EXPECT_EQ(Fs[0].Class, VulnClass::HeapLeak);
  EXPECT_EQ(Fs[0].Sink, "malloc");
  EXPECT_EQ(Fs[0].Source, NameSource::Import);
}

TEST(AllocLifetime, MicrosoftOperatorNamesDriveLifetimeAnalysis) {
  FB Released("released", 0x100);
  int ReleasedBlock = Released.block();
  Released.call(ReleasedBlock, "??2@YAPEAX_K@Z", temp(1),
                {MedVar::makeConst(16, 8)});
  Released.call(ReleasedBlock, "??3@YAXPEAX@Z", MedVar{}, {temp(1)});
  Released.ret(ReleasedBlock, {});
  EXPECT_FALSE(has(audit({Released.F}), VulnClass::HeapLeak));

  FB Leaked("leaked", 0x200);
  int LeakedBlock = Leaked.block();
  Leaked.call(LeakedBlock, "??_U@YAPEAX_K@Z", temp(2),
              {MedVar::makeConst(32, 8)});
  Leaked.ret(LeakedBlock, {});
  EXPECT_TRUE(has(audit({Leaked.F}), VulnClass::HeapLeak));
}

TEST(AllocLifetime, ItaniumAlignedOperatorNamesDriveLifetimeAnalysis) {
  FB Released("released", 0x100);
  int ReleasedBlock = Released.block();
  Released.call(ReleasedBlock, "_ZnwmSt11align_val_t", temp(1),
                {MedVar::makeConst(64, 8), MedVar::makeConst(64, 8)});
  Released.call(ReleasedBlock, "_ZdlPvmSt11align_val_t", MedVar{},
                {temp(1), MedVar::makeConst(64, 8), MedVar::makeConst(64, 8)});
  Released.ret(ReleasedBlock, {});
  EXPECT_FALSE(has(audit({Released.F}), VulnClass::HeapLeak));

  FB Leaked("leaked", 0x200);
  int LeakedBlock = Leaked.block();
  Leaked.call(LeakedBlock, "_ZnamSt11align_val_t", temp(2),
              {MedVar::makeConst(64, 8), MedVar::makeConst(64, 8)});
  Leaked.ret(LeakedBlock, {});
  EXPECT_TRUE(has(audit({Leaked.F}), VulnClass::HeapLeak));
}

TEST(AllocLifetime, StackAllocationIsNotAHeapLeak) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "alloca", temp(1), {MedVar::makeConst(16, 8)});
  B.ret(b0, {});
  auto Fs = audit({B.F});
  EXPECT_FALSE(has(Fs, VulnClass::HeapLeak));
}

TEST(AllocLifetime, NoLeakWhenFreed) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.ret(b0, {});
  auto Fs = audit({B.F});
  EXPECT_FALSE(has(Fs, VulnClass::HeapLeak));
}

TEST(AllocLifetime, MayAliasFreeDoesNotProveAllocationReleased) {
  FB B("f", 0x100);
  int entry = B.block();
  int allocated = B.block();
  int other = B.block();
  int join = B.block();
  B.call(entry, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.succ(entry, allocated);
  B.succ(entry, other);
  B.succ(allocated, join);
  B.succ(other, join);
  PhiNode Phi;
  Phi.Output = temp(2);
  Phi.Args = {{allocated, temp(1)}, {other, temp(9)}};
  B.F.Blocks[join].Phis.push_back(std::move(Phi));
  B.call(join, "free", MedVar{}, {temp(2)});
  B.ret(join, {});

  auto Fs = audit({B.F});
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_FALSE(has(Fs, VulnClass::DoubleFree));
  EXPECT_FALSE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, AdjustedPointerFreeDoesNotReleaseAllocation) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(b0, NdOp::INT_ADD, temp(2), {temp(1), MedVar::makeConst(8, 8)});
  B.call(b0, "free", MedVar{}, {temp(2)});
  B.ret(b0, {});

  auto Fs = audit({B.F});
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, SelectOfSamePointerCanReleaseAllocation) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(b0, NdOp::SELECT, temp(2), {temp(9), temp(1), temp(1)});
  B.call(b0, "free", MedVar{}, {temp(2)});
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::HeapLeak));
}

TEST(AllocLifetime, NoLeakWhenReturned) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.ret(b0, {temp(1)}); // handle escapes through the return value.
  auto Fs = audit({B.F});
  EXPECT_FALSE(has(Fs, VulnClass::HeapLeak));
}

TEST(AllocLifetime, NonEscapingHelperDoesNotHideLeak) {
  FB Helper("inspect", 0x200);
  Helper.F.Params.push_back(temp(0));
  int h0 = Helper.block();
  Helper.op(h0, NdOp::LOAD, temp(4), {temp(0)});
  Helper.ret(h0, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "inspect", MedVar{}, {temp(1)}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Helper.F, User.F});
  bool UserLeak = false;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == 0x100)
      UserLeak = true;
  EXPECT_TRUE(UserLeak);
}

TEST(AllocLifetime, MayAliasFreeWrapperDoesNotProveCallerReleased) {
  FB Helper("maybe_free", 0x200);
  Helper.F.Params = {temp(0), temp(1)};
  int hEntry = Helper.block();
  int hFirst = Helper.block();
  int hSecond = Helper.block();
  int hJoin = Helper.block();
  Helper.succ(hEntry, hFirst);
  Helper.succ(hEntry, hSecond);
  Helper.succ(hFirst, hJoin);
  Helper.succ(hSecond, hJoin);
  PhiNode Phi;
  Phi.Output = temp(2);
  Phi.Args = {{hFirst, temp(0)}, {hSecond, temp(1)}};
  Helper.F.Blocks[hJoin].Phis.push_back(std::move(Phi));
  Helper.call(hJoin, "free", MedVar{}, {temp(2)});
  Helper.ret(hJoin, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(3), {MedVar::makeConst(16, 8)});
  User.call(u0, "maybe_free", MedVar{}, {temp(3), temp(9)}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Helper.F, User.F});
  const Finding *UserLeak = nullptr;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry)
      UserLeak = &F;
  ASSERT_NE(UserLeak, nullptr);
  EXPECT_EQ(UserLeak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, ConditionalFreeWrapperDoesNotProveCallerReleased) {
  FB Helper("conditional_free", 0x200);
  Helper.F.Params = {temp(0)};
  int hEntry = Helper.block();
  int hFree = Helper.block();
  int hSkip = Helper.block();
  int hJoin = Helper.block();
  Helper.succ(hEntry, hFree);
  Helper.succ(hEntry, hSkip);
  Helper.succ(hFree, hJoin);
  Helper.succ(hSkip, hJoin);
  Helper.call(hFree, "free", MedVar{}, {temp(0)});
  Helper.ret(hJoin, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "conditional_free", MedVar{}, {temp(1)}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Helper.F, User.F});
  const Finding *UserLeak = nullptr;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry)
      UserLeak = &F;
  ASSERT_NE(UserLeak, nullptr);
  EXPECT_EQ(UserLeak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, AlternateFreeSitesProveWrapperReleased) {
  FB Helper("always_free", 0x200);
  Helper.F.Params = {temp(0)};
  int hEntry = Helper.block();
  int hLeft = Helper.block();
  int hRight = Helper.block();
  int hJoin = Helper.block();
  Helper.succ(hEntry, hLeft);
  Helper.succ(hEntry, hRight);
  Helper.succ(hLeft, hJoin);
  Helper.succ(hRight, hJoin);
  Helper.call(hLeft, "free", MedVar{}, {temp(0)});
  Helper.call(hRight, "free", MedVar{}, {temp(0)});
  Helper.ret(hJoin, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "always_free", MedVar{}, {temp(1)}, 0x200);
  User.ret(u0, {});

  EXPECT_FALSE(has(audit({Helper.F, User.F}), VulnClass::HeapLeak));
}

TEST(AllocLifetime, ConditionalEscapeWrapperDoesNotHideCallerLeak) {
  FB Helper("conditional_escape", 0x200);
  Helper.F.Params = {temp(0)};
  int hEntry = Helper.block();
  int hPublish = Helper.block();
  int hSkip = Helper.block();
  int hJoin = Helper.block();
  Helper.succ(hEntry, hPublish);
  Helper.succ(hEntry, hSkip);
  Helper.succ(hPublish, hJoin);
  Helper.succ(hSkip, hJoin);
  Helper.op(hPublish, NdOp::STORE, MedVar{},
            {MedVar::makeConst(0x5000, 8), temp(0)});
  Helper.ret(hJoin, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "conditional_escape", MedVar{}, {temp(1)}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Helper.F, User.F});
  const Finding *UserLeak = nullptr;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry)
      UserLeak = &F;
  ASSERT_NE(UserLeak, nullptr);
  EXPECT_EQ(UserLeak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, AlternateEscapeSitesProveWrapperEscaped) {
  FB Helper("always_escape", 0x200);
  Helper.F.Params = {temp(0)};
  int hEntry = Helper.block();
  int hLeft = Helper.block();
  int hRight = Helper.block();
  int hJoin = Helper.block();
  Helper.succ(hEntry, hLeft);
  Helper.succ(hEntry, hRight);
  Helper.succ(hLeft, hJoin);
  Helper.succ(hRight, hJoin);
  Helper.op(hLeft, NdOp::STORE, MedVar{},
            {MedVar::makeConst(0x5000, 8), temp(0)});
  Helper.op(hRight, NdOp::STORE, MedVar{},
            {MedVar::makeConst(0x6000, 8), temp(0)});
  Helper.ret(hJoin, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "always_escape", MedVar{}, {temp(1)}, 0x200);
  User.ret(u0, {});

  EXPECT_FALSE(has(audit({Helper.F, User.F}), VulnClass::HeapLeak));
}

TEST(AllocLifetime, ExceptionalExitDoesNotProveWrapperEscaped) {
  FB Helper("exceptional_escape", 0x200);
  Helper.F.Params = {temp(0)};
  int hEntry = Helper.block();
  int hPublish = Helper.block();
  int hHandler = Helper.block();
  Helper.succ(hEntry, hPublish);
  ExceptionalEdge Edge;
  Edge.BlockId = hHandler;
  Helper.F.Blocks[hEntry].ExceptionalSuccs.push_back(Edge);
  Helper.op(hPublish, NdOp::STORE, MedVar{},
            {MedVar::makeConst(0x5000, 8), temp(0)});
  Helper.ret(hPublish, {});
  Helper.ret(hHandler, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "exceptional_escape", MedVar{}, {temp(1)}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Helper.F, User.F});
  const Finding *UserLeak = nullptr;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry)
      UserLeak = &F;
  ASSERT_NE(UserLeak, nullptr);
  EXPECT_EQ(UserLeak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, ExceptionalExitDoesNotProveWrapperReleased) {
  FB Helper("exceptional_free", 0x200);
  Helper.F.Params = {temp(0)};
  int hEntry = Helper.block();
  int hFree = Helper.block();
  int hHandler = Helper.block();
  Helper.succ(hEntry, hFree);
  ExceptionalEdge Edge;
  Edge.BlockId = hHandler;
  Helper.F.Blocks[hEntry].ExceptionalSuccs.push_back(Edge);
  Helper.call(hFree, "free", MedVar{}, {temp(0)});
  Helper.ret(hFree, {});
  Helper.ret(hHandler, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "exceptional_free", MedVar{}, {temp(1)}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Helper.F, User.F});
  const Finding *UserLeak = nullptr;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry)
      UserLeak = &F;
  ASSERT_NE(UserLeak, nullptr);
  EXPECT_EQ(UserLeak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, DoubleFreeSequential) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x400);
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x408);
  B.ret(b0, {});
  auto Fs = audit({B.F});
  EXPECT_TRUE(has(Fs, VulnClass::DoubleFree));
}

TEST(AllocLifetime, SiblingFreesAreNotDoubleFree) {
  FB B("f", 0x100);
  int b0 = B.block();
  int b1 = B.block();
  int b2 = B.block();
  int b3 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.succ(b0, b1);
  B.succ(b0, b2);
  B.call(b1, "free", MedVar{}, {temp(1)}, 0x9100, 0x410);
  B.call(b2, "free", MedVar{}, {temp(1)}, 0x9100, 0x420);
  B.succ(b1, b3);
  B.succ(b2, b3);
  B.ret(b3, {});
  auto Fs = audit({B.F});
  EXPECT_FALSE(has(Fs, VulnClass::DoubleFree));
}

TEST(AllocLifetime, RepeatedFreeSiteAcrossLoopBackedgeIsDoubleFree) {
  FB B("f", 0x100);
  int entry = B.block();
  int loop = B.block();
  int exit = B.block();
  B.call(entry, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.succ(entry, loop);
  B.call(loop, "free", MedVar{}, {temp(1)}, 0x9100, 0x408);
  B.succ(loop, loop);
  B.succ(loop, exit);
  B.ret(exit, {});

  EXPECT_TRUE(has(audit({B.F}), VulnClass::DoubleFree));
}

TEST(AllocLifetime, ReallocationOnLoopBackedgePreventsSameSiteDoubleFree) {
  FB B("f", 0x100);
  int loop = B.block();
  int exit = B.block();
  B.call(loop, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(loop, "free", MedVar{}, {temp(1)}, 0x9100, 0x408);
  B.succ(loop, loop);
  B.succ(loop, exit);
  B.ret(exit, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::DoubleFree));
}

TEST(AllocLifetime, UseAfterFree) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x400);
  // *p = load(p) after free.
  B.op(b0, NdOp::LOAD, temp(2), {temp(1)}, 0x408);
  B.ret(b0, {});
  auto Fs = audit({B.F});
  EXPECT_TRUE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, StringDuplicationReadsFreedSource) {
  for (const char *Name : {"strdup", "strndup"}) {
    SCOPED_TRACE(Name);
    FB B("f", 0x100);
    int b0 = B.block();
    B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x400);
    std::vector<MedVar> Args = {temp(1)};
    if (llvm::StringRef(Name) == "strndup")
      Args.push_back(MedVar::makeConst(1, 8));
    B.call(b0, Name, temp(2), std::move(Args), 0x9200, 0x408);
    B.call(b0, "free", MedVar{}, {temp(2)});
    B.ret(b0, {});

    EXPECT_TRUE(has(audit({B.F}), VulnClass::UseAfterFree));
  }
}

TEST(AllocLifetime, ZeroLengthStrndupDoesNotReadFreedSource) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x400);
  B.call(b0, "strndup", temp(2), {temp(1), MedVar::makeConst(0, 8)}, 0x9200,
         0x408);
  B.call(b0, "free", MedVar{}, {temp(2)});
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, RuntimeLengthStrndupUseAfterFreeFailsClosed) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x400);
  B.call(b0, "strndup", temp(2), {temp(1), temp(8)}, 0x9200, 0x408);
  B.call(b0, "free", MedVar{}, {temp(2)});
  B.ret(b0, {});

  const std::vector<Finding> Fs = audit({B.F});
  const Finding *Use = find(Fs, VulnClass::UseAfterFree);
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Use->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, AtomicMemoryUseAfterFree) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x400);
  B.op(b0, NdOp::ATOMIC_ADD, temp(2), {temp(1), MedVar::makeConst(1, 8)},
       0x408);
  B.ret(b0, {});

  EXPECT_TRUE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, AtomicValueDoesNotDereferenceFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x400);
  B.op(b0, NdOp::ATOMIC_XCHG, temp(2), {MedVar::makeConst(0x5000, 8), temp(1)},
       0x408);
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, ZeroLengthMemcpyDoesNotUseFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "memcpy", temp(2), {temp(1), temp(3), MedVar::makeConst(0, 8)});
  B.ret(b0, {});

  auto Fs = audit({B.F});
  EXPECT_FALSE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, PositiveLengthMemcpyUsesFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "memcpy", temp(2), {temp(1), temp(3), MedVar::makeConst(1, 8)});
  B.ret(b0, {});

  auto Fs = audit({B.F});
  EXPECT_TRUE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, RejectedFortifiedCopyDoesNotUseFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "memcpy_chk", temp(2),
         {temp(1), temp(1), MedVar::makeConst(8, 8), MedVar::makeConst(4, 8)});
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, AcceptedFortifiedCopyUsesFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "memcpy_chk", temp(2),
         {temp(1), temp(1), MedVar::makeConst(4, 8), MedVar::makeConst(8, 8)});
  B.ret(b0, {});

  EXPECT_TRUE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, RejectedFortifiedStringCopyDoesNotUseFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "strncpy_chk", temp(2),
         {temp(1), temp(1), MedVar::makeConst(8, 8), MedVar::makeConst(4, 8)});
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, AcceptedFortifiedStringCopyUsesFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "strncpy_chk", temp(2),
         {temp(1), temp(1), MedVar::makeConst(4, 8), MedVar::makeConst(8, 8)});
  B.ret(b0, {});

  EXPECT_TRUE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, ZeroLengthWideCopyDoesNotUseFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "wmemmove", temp(2), {temp(1), temp(3), MedVar::makeConst(0, 8)});
  B.ret(b0, {});

  auto Fs = audit({B.F});
  EXPECT_FALSE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, RuntimeLengthMemcpyUseAfterFreeFailsClosed) {
  constexpr va_t MallocVA = 0x400;
  constexpr va_t FreeVA = 0x408;
  constexpr va_t MemcpyVA = 0x410;
  FB B("f", 0x100);
  B.F.Params.push_back(temp(8));
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, MallocVA);
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, FreeVA);
  B.call(b0, "memcpy", temp(2), {temp(1), temp(3), temp(8)}, 0x9200, MemcpyVA);
  B.ret(b0, {});

  LowFunc LF;
  LF.Entry = B.F.Entry;
  LF.DecodedInstructionCount = 1;
  LF.LiftedInstructionCount = 1;
  LowBlock LB;
  LB.Id = b0;
  LB.StartAddr = B.F.Entry;
  LB.EndAddr = MemcpyVA + 8;
  LB.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9000, 8)}, MallocVA));
  LB.Ops.push_back(lowOp(NdOp::CALL, NdVar{}, {NdVar::cst(0x9100, 8)}, FreeVA));
  LB.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9200, 8)}, MemcpyVA));
  LB.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}, MemcpyVA + 4));
  LF.Blocks.push_back(std::move(LB));

  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::vector<MedFunc> MedFuncs{B.F};
  std::vector<LowFunc> LowFuncs{std::move(LF)};
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &MedFuncs;
  In.LowFuncs = &LowFuncs;
  const std::vector<Finding> Fs =
      auditHeap(In, SinkCatalog::defaults(), SafetyBudgets{});
  const Finding *Use = find(Fs, VulnClass::UseAfterFree);
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->TheVerdict, Verdict::Unknown) << Use->Detail;
  EXPECT_EQ(Use->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, ZeroLengthStrncpyDoesNotUseFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "strncpy", temp(2), {temp(1), temp(3), MedVar::makeConst(0, 8)});
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, ZeroSizeStrlOperationsUseOnlyTheirSource) {
  for (const char *Name : {"strlcpy", "strlcat"}) {
    FB FreedDst("freed_dst", 0x100);
    int d0 = FreedDst.block();
    FreedDst.call(d0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
    FreedDst.call(d0, "free", MedVar{}, {temp(1)});
    FreedDst.call(d0, Name, temp(2),
                  {temp(1), temp(3), MedVar::makeConst(0, 8)});
    FreedDst.ret(d0, {});
    EXPECT_FALSE(has(audit({FreedDst.F}), VulnClass::UseAfterFree)) << Name;

    FB FreedSrc("freed_src", 0x200);
    int s0 = FreedSrc.block();
    FreedSrc.call(s0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
    FreedSrc.call(s0, "free", MedVar{}, {temp(1)});
    FreedSrc.call(s0, Name, temp(2),
                  {temp(3), temp(1), MedVar::makeConst(0, 8)});
    FreedSrc.ret(s0, {});
    EXPECT_TRUE(has(audit({FreedSrc.F}), VulnClass::UseAfterFree)) << Name;
  }
}

TEST(AllocLifetime, ZeroCountStrncatUsesOnlyItsDestination) {
  FB FreedSrc("freed_src", 0x100);
  int s0 = FreedSrc.block();
  FreedSrc.call(s0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  FreedSrc.call(s0, "free", MedVar{}, {temp(1)});
  FreedSrc.call(s0, "strncat", temp(2),
                {temp(3), temp(1), MedVar::makeConst(0, 8)});
  FreedSrc.ret(s0, {});
  EXPECT_FALSE(has(audit({FreedSrc.F}), VulnClass::UseAfterFree));

  FB FreedDst("freed_dst", 0x200);
  int d0 = FreedDst.block();
  FreedDst.call(d0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  FreedDst.call(d0, "free", MedVar{}, {temp(1)});
  FreedDst.call(d0, "strncat", temp(2),
                {temp(1), temp(3), MedVar::makeConst(0, 8)});
  FreedDst.ret(d0, {});
  EXPECT_TRUE(has(audit({FreedDst.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, ZeroSizeSnprintfUsesOnlyItsFormat) {
  FB FreedDst("freed_dst", 0x100);
  int d0 = FreedDst.block();
  FreedDst.call(d0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  FreedDst.call(d0, "free", MedVar{}, {temp(1)});
  FreedDst.call(d0, "snprintf", temp(2),
                {temp(1), MedVar::makeConst(0, 8), temp(3)});
  FreedDst.ret(d0, {});
  EXPECT_FALSE(has(audit({FreedDst.F}), VulnClass::UseAfterFree));

  FB FreedFormat("freed_format", 0x200);
  int f0 = FreedFormat.block();
  FreedFormat.call(f0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  FreedFormat.call(f0, "free", MedVar{}, {temp(1)});
  FreedFormat.call(f0, "snprintf", temp(2),
                   {temp(3), MedVar::makeConst(0, 8), temp(1)});
  FreedFormat.ret(f0, {});
  EXPECT_TRUE(has(audit({FreedFormat.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, PositiveSizeSnprintfUsesFreedDestination) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "snprintf", temp(2), {temp(1), MedVar::makeConst(1, 8), temp(3)});
  B.ret(b0, {});

  EXPECT_TRUE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, RejectedFortifiedSnprintfDoesNotUseFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "snprintf_chk", temp(2),
         {temp(1), MedVar::makeConst(8, 8), MedVar::makeConst(2, 4),
          MedVar::makeConst(4, 8), temp(1)});
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, AcceptedFortifiedSnprintfUsesFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "snprintf_chk", temp(2),
         {temp(1), MedVar::makeConst(4, 8), MedVar::makeConst(2, 4),
          MedVar::makeConst(8, 8), temp(1)});
  B.ret(b0, {});

  EXPECT_TRUE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, AtomicReadResultPreservesHeapAlias) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), temp(1)});
  B.op(b0, NdOp::ATOMIC_XCHG, temp(2), {temp(10), MedVar::makeConst(0, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.op(b0, NdOp::LOAD, temp(3), {temp(2)});
  B.ret(b0, {});

  EXPECT_TRUE(
      has(audit({B.F}, nullptr, /*StackRegs=*/true), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, UseAfterFreeAcrossLoopBackedge) {
  FB B("f", 0x100);
  int entry = B.block();
  int loop = B.block();
  int exit = B.block();
  B.call(entry, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.succ(entry, loop);
  B.op(loop, NdOp::LOAD, temp(2), {temp(1)}, 0x400);
  B.call(loop, "free", MedVar{}, {temp(1)}, 0x9100, 0x408);
  B.succ(loop, loop);
  B.succ(loop, exit);
  B.ret(exit, {});

  EXPECT_TRUE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, UseBeforeFreeIsClean) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(b0, NdOp::LOAD, temp(2), {temp(1)}, 0x400); // use first
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x408);
  B.ret(b0, {});
  auto Fs = audit({B.F});
  EXPECT_FALSE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, FreedPointerUsedAsAllocatorSizeIsNotAUseAfterFree) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "malloc", temp(2), {temp(1)});
  B.call(b0, "free", MedVar{}, {temp(2)});
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, UnknownCallUseOfFreedPointerIsNotDefinite) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "observe_pointer", MedVar{}, {temp(1)}, 0x9200);
  B.ret(b0, {});

  const std::vector<Finding> Fs = audit({B.F});
  const Finding *Use = find(Fs, VulnClass::UseAfterFree);
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->TheVerdict, Verdict::Unknown);
  EXPECT_NE(Use->Detail.find("may access"), std::string::npos);
}

TEST(AllocLifetime, ZeroLengthInputSourcesDoNotUseFreedOutputBuffer) {
  struct SourceCase {
    const char *Name;
    std::vector<MedVar> Args;
  };
  const auto Buffer = temp(1);
  const std::vector<SourceCase> Cases = {
      {"read", {temp(3), Buffer, MedVar::makeConst(0, 8)}},
      {"pread",
       {temp(3), Buffer, MedVar::makeConst(0, 8), MedVar::makeConst(0, 8)}},
      {"recv",
       {temp(3), Buffer, MedVar::makeConst(0, 8), MedVar::makeConst(0, 8)}},
      {"recvfrom",
       {temp(3), Buffer, MedVar::makeConst(0, 8), MedVar::makeConst(0, 8),
        temp(4), temp(5)}},
      {"fread",
       {Buffer, MedVar::makeConst(0, 8), MedVar::makeConst(4, 8), temp(3)}},
      {"fread",
       {Buffer, MedVar::makeConst(4, 8), MedVar::makeConst(0, 8), temp(3)}},
      {"ReadFile",
       {temp(3), Buffer, MedVar::makeConst(0, 8), temp(4), temp(5)}},
      {"GetEnvironmentVariableA", {temp(3), Buffer, MedVar::makeConst(0, 8)}}};

  for (const SourceCase &C : Cases) {
    SCOPED_TRACE(C.Name);
    FB B("f", 0x100);
    int b0 = B.block();
    B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call(b0, "free", MedVar{}, {temp(1)});
    B.call(b0, C.Name, temp(2), C.Args);
    B.ret(b0, {});

    EXPECT_FALSE(has(audit({B.F}), VulnClass::UseAfterFree));
  }
}

TEST(AllocLifetime, FallibleInputSourceUseRemainsUnknown) {
  constexpr va_t MallocVA = 0x400;
  constexpr va_t FreeVA = 0x408;
  constexpr va_t SourceVA = 0x410;
  struct SourceCase {
    const char *Name;
    std::vector<MedVar> Args;
  };
  for (const SourceCase &C :
       {SourceCase{"read", {temp(3), temp(1), MedVar::makeConst(1, 8)}},
        SourceCase{"fgets", {temp(1), MedVar::makeConst(16, 8), temp(3)}},
        SourceCase{"gets", {temp(1)}}}) {
    SCOPED_TRACE(C.Name);
    FB B("f", 0x100);
    int b0 = B.block();
    B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, MallocVA);
    B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, FreeVA);
    B.call(b0, C.Name, temp(2), C.Args, 0x9200, SourceVA);
    B.ret(b0, {});

    LowFunc LF;
    LF.Entry = B.F.Entry;
    LF.DecodedInstructionCount = 1;
    LF.LiftedInstructionCount = 1;
    LowBlock LB;
    LB.Id = b0;
    LB.StartAddr = B.F.Entry;
    LB.EndAddr = SourceVA + 8;
    LB.Ops.push_back(
        lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9000, 8)}, MallocVA));
    LB.Ops.push_back(
        lowOp(NdOp::CALL, NdVar{}, {NdVar::cst(0x9100, 8)}, FreeVA));
    LB.Ops.push_back(
        lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9200, 8)}, SourceVA));
    LB.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}, SourceVA + 4));
    LF.Blocks.push_back(std::move(LB));

    BinaryImage Img;
    Img.Arch = Arch::X64;
    std::vector<MedFunc> MedFuncs{B.F};
    std::vector<LowFunc> LowFuncs{std::move(LF)};
    AnalysisInput In;
    In.Img = &Img;
    In.MedFuncs = &MedFuncs;
    In.LowFuncs = &LowFuncs;
    const std::vector<Finding> Fs =
        auditHeap(In, SinkCatalog::defaults(), SafetyBudgets{});
    const Finding *Use = find(Fs, VulnClass::UseAfterFree);
    ASSERT_NE(Use, nullptr);
    EXPECT_EQ(Use->TheVerdict, Verdict::Unknown);
    EXPECT_EQ(Use->TheConfidence, Confidence::Low);
    EXPECT_NE(Use->Detail.find("may access"), std::string::npos);
  }
}

TEST(AllocLifetime, StringLengthCallDefinitelyUsesFreedStorage) {
  constexpr va_t MallocVA = 0x400;
  constexpr va_t FreeVA = 0x408;
  constexpr va_t StrlenVA = 0x410;
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, MallocVA);
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, FreeVA);
  B.call(b0, "strlen", temp(2), {temp(1)}, 0x9200, StrlenVA);
  B.ret(b0, {});

  LowFunc LF;
  LF.Entry = B.F.Entry;
  LF.DecodedInstructionCount = 1;
  LF.LiftedInstructionCount = 1;
  LowBlock LB;
  LB.Id = b0;
  LB.StartAddr = B.F.Entry;
  LB.EndAddr = StrlenVA + 8;
  LB.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9000, 8)}, MallocVA));
  LB.Ops.push_back(lowOp(NdOp::CALL, NdVar{}, {NdVar::cst(0x9100, 8)}, FreeVA));
  LB.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9200, 8)}, StrlenVA));
  LB.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}, StrlenVA + 4));
  LF.Blocks.push_back(std::move(LB));

  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::vector<MedFunc> MedFuncs{B.F};
  std::vector<LowFunc> LowFuncs{std::move(LF)};
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &MedFuncs;
  In.LowFuncs = &LowFuncs;
  const std::vector<Finding> Fs =
      auditHeap(In, SinkCatalog::defaults(), SafetyBudgets{});
  const Finding *Use = find(Fs, VulnClass::UseAfterFree);
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->TheVerdict, Verdict::Unsafe) << Use->Detail;
  EXPECT_EQ(Use->TheConfidence, Confidence::High);
  EXPECT_FALSE(Use->Corroboration.empty());
}

TEST(AllocLifetime, ScanfFixedPrefixDefinitelyUsesFreedStorage) {
  constexpr va_t MallocVA = 0x400;
  constexpr va_t FreeVA = 0x408;
  constexpr va_t ScanfVA = 0x410;
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, MallocVA);
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, FreeVA);
  B.call(b0, "sscanf", temp(2),
         {temp(1), MedVar::makeConst(0x5000, 8), temp(3)}, 0x9200, ScanfVA);
  B.ret(b0, {});

  LowFunc LF;
  LF.Entry = B.F.Entry;
  LF.DecodedInstructionCount = 1;
  LF.LiftedInstructionCount = 1;
  LowBlock LB;
  LB.Id = b0;
  LB.StartAddr = B.F.Entry;
  LB.EndAddr = ScanfVA + 8;
  LB.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9000, 8)}, MallocVA));
  LB.Ops.push_back(lowOp(NdOp::CALL, NdVar{}, {NdVar::cst(0x9100, 8)}, FreeVA));
  LB.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9200, 8)}, ScanfVA));
  LB.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}, ScanfVA + 4));
  LF.Blocks.push_back(std::move(LB));

  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::vector<MedFunc> MedFuncs{B.F};
  std::vector<LowFunc> LowFuncs{std::move(LF)};
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &MedFuncs;
  In.LowFuncs = &LowFuncs;
  const std::vector<Finding> Fs =
      auditHeap(In, SinkCatalog::defaults(), SafetyBudgets{});
  const Finding *Use = find(Fs, VulnClass::UseAfterFree);
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->TheVerdict, Verdict::Unsafe) << Use->Detail;
  EXPECT_EQ(Use->TheConfidence, Confidence::High);
  EXPECT_FALSE(Use->Corroboration.empty());
}

TEST(AllocLifetime, OverwrittenSpillDoesNotRemainAHeapAlias) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), temp(1)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0, 8)});
  B.op(b0, NdOp::LOAD, temp(2), {temp(10)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.op(b0, NdOp::LOAD, temp(3), {temp(2)});
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, UninitializedLocalStackLoadIsReported) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::LOAD, temp(11), {temp(10)}, 0x408);
  B.ret(b0, {temp(11)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->FuncEntry, 0x100u);
  EXPECT_EQ(Read->CallVA, 0x408u);
}

TEST(AllocLifetime, UninitializedLocalStackLoadSurvivesPointerSpill) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x40, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::INT_ADD, temp(11),
       {mkReg(kSP, 1), MedVar::makeConst(0x28, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(11), temp(10)});
  B.op(b0, NdOp::LOAD, temp(12), {temp(11)});
  B.op(b0, NdOp::COPY, temp(13), {temp(12)});
  B.op(b0, NdOp::LOAD, temp(14), {temp(13)}, 0x408);
  B.ret(b0, {temp(14)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->CallVA, 0x408u);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, InitializedLocalStackLoadSurvivesPointerSpill) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x40, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x1234, 8)});
  B.op(b0, NdOp::INT_ADD, temp(11),
       {mkReg(kSP, 1), MedVar::makeConst(0x28, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(11), temp(10)});
  B.op(b0, NdOp::LOAD, temp(12), {temp(11)});
  B.op(b0, NdOp::COPY, temp(13), {temp(12)});
  B.op(b0, NdOp::LOAD, temp(14), {temp(13)}, 0x408);
  B.ret(b0, {temp(14)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, UninitializedLocalStackMemcpySourceIsReported) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "memcpy", temp(11), {temp(12), temp(10), MedVar::makeConst(8, 8)},
         0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->Name, "memcpy");
  EXPECT_EQ(Read->CallVA, 0x408u);
}

TEST(AllocLifetime, InitializedLocalStackMemcpySourceIsClean) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x1234, 8)});
  B.call(b0, "memcpy", temp(11), {temp(12), temp(10), MedVar::makeConst(8, 8)},
         0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, ZeroLengthMemcpyDoesNotReadUninitializedSource) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "memcpy", temp(11), {temp(12), temp(10), MedVar::makeConst(0, 8)},
         0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, RejectedFortifiedCopyDoesNotReadUninitializedSource) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "memcpy_chk", temp(11),
         {temp(12), temp(10), MedVar::makeConst(8, 8), MedVar::makeConst(4, 8)},
         0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, AcceptedFortifiedCopyReadsUninitializedSource) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "memcpy_chk", temp(11),
         {temp(12), temp(10), MedVar::makeConst(4, 8), MedVar::makeConst(8, 8)},
         0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_TRUE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, WideCopyReadsPlatformSizedElements) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    BinaryImage Img;
    Img.Format = Format;

    FB B("f", 0x100);
    int b0 = B.block();
    B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
         {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
    B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
    B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x1234, 4)});
    B.call(b0, "wmemcpy", temp(11),
           {temp(12), temp(10), MedVar::makeConst(4, 8)}, 0x9000, 0x408);
    B.ret(b0, {});

    auto Fs = audit({B.F}, &Img, /*StackRegs=*/true,
                    /*IncludeStackReads=*/true);
    const Finding *Read = find(Fs, VulnClass::UninitializedRead);
    ASSERT_NE(Read, nullptr) << static_cast<int>(Format);
    EXPECT_EQ(Read->TheVerdict, Verdict::Unknown) << static_cast<int>(Format);
  }
}

TEST(AllocLifetime, BoundedStringCopyIsNotTreatedAsAnExactMemoryRead) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0, 1)});
  B.call(b0, "strncpy", temp(11), {temp(12), temp(10), MedVar::makeConst(8, 8)},
         0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, RuntimeLengthStackMemcpyReadFailsClosed) {
  FB B("f", 0x100);
  B.F.Params.push_back(temp(20));
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "memcpy", temp(11), {temp(12), temp(10), temp(20)}, 0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Read->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, RuntimeLengthPastInitializedPrefixFailsClosed) {
  FB B("f", 0x100);
  B.F.Params.push_back(temp(20));
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x12, 1)});
  B.call(b0, "memcpy", temp(11), {temp(12), temp(10), temp(20)}, 0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Read->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, OverflowingWideCopyLengthFailsClosed) {
  BinaryImage Img;
  Img.Format = BinaryFormat::ELF;

  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x12, 1)});
  B.call(b0, "wmemcpy", temp(11),
         {temp(12), temp(10), MedVar::makeConst(0x4000000000000000ULL, 8)},
         0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, &Img, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Read->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, RuntimeLengthStackMemcpyReadSurvivesPointerSpill) {
  FB B("f", 0x100);
  B.F.Params.push_back(temp(20));
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x40, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::INT_ADD, temp(13),
       {mkReg(kSP, 1), MedVar::makeConst(0x28, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(13), temp(10)});
  B.op(b0, NdOp::LOAD, temp(14), {temp(13)});
  B.op(b0, NdOp::COPY, temp(15), {temp(14)});
  B.call(b0, "memcpy", temp(11), {temp(12), temp(15), temp(20)}, 0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, InitializedStackMemcpyReadSurvivesPointerSpill) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x40, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x1234, 8)});
  B.op(b0, NdOp::INT_ADD, temp(13),
       {mkReg(kSP, 1), MedVar::makeConst(0x28, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(13), temp(10)});
  B.op(b0, NdOp::LOAD, temp(14), {temp(13)});
  B.op(b0, NdOp::COPY, temp(15), {temp(14)});
  B.call(b0, "memcpy", temp(11), {temp(12), temp(15), MedVar::makeConst(8, 8)},
         0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, UninitializedLocalStackAtomicReadIsReported) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::ATOMIC_CMPXCHG, temp(11),
       {temp(10), MedVar::makeConst(0, 8), MedVar::makeConst(1, 8)}, 0x408);
  B.ret(b0, {temp(11)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->CallVA, 0x408u);
}

TEST(AllocLifetime, UninitializedLocalStackAtomicReadSurvivesPointerSpill) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x40, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::INT_ADD, temp(11),
       {mkReg(kSP, 1), MedVar::makeConst(0x28, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(11), temp(10)});
  B.op(b0, NdOp::LOAD, temp(12), {temp(11)});
  B.op(b0, NdOp::ATOMIC_CMPXCHG, temp(13),
       {temp(12), MedVar::makeConst(0, 8), MedVar::makeConst(1, 8)}, 0x408);
  B.ret(b0, {temp(13)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->CallVA, 0x408u);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, InitializedLocalStackAtomicReadIsClean) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x1234, 8)});
  B.op(b0, NdOp::ATOMIC_ADD, temp(11), {temp(10), MedVar::makeConst(1, 8)},
       0x408);
  B.ret(b0, {temp(11)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, InitializedLocalStackLoadIsClean) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x1234, 8)});
  B.op(b0, NdOp::LOAD, temp(11), {temp(10)}, 0x408);
  B.ret(b0, {temp(11)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, ConditionalStackInitializationFailsClosed) {
  FB B("f", 0x100);
  int entry = B.block();
  int initialized = B.block();
  int uninitialized = B.block();
  int join = B.block();
  B.op(entry, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(entry, NdOp::INT_ADD, temp(10),
       {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.succ(entry, initialized);
  B.succ(entry, uninitialized);
  B.op(initialized, NdOp::STORE, MedVar{},
       {temp(10), MedVar::makeConst(0x1234, 8)});
  B.succ(initialized, join);
  B.succ(uninitialized, join);
  B.op(join, NdOp::LOAD, temp(11), {temp(10)}, 0x420);
  B.ret(join, {temp(11)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Read->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, PartialStackInitializationFailsClosed) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x1234, 4)});
  B.op(b0, NdOp::LOAD, temp(11), {temp(10)}, 0x408);
  B.ret(b0, {temp(11)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Read->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, EveryBranchInitializingStackSlotIsClean) {
  FB B("f", 0x100);
  int entry = B.block();
  int left = B.block();
  int right = B.block();
  int join = B.block();
  B.op(entry, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(entry, NdOp::INT_ADD, temp(10),
       {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.succ(entry, left);
  B.succ(entry, right);
  B.op(left, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(1, 8)});
  B.op(right, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(2, 8)});
  B.succ(left, join);
  B.succ(right, join);
  B.op(join, NdOp::LOAD, temp(11), {temp(10)}, 0x420);
  B.ret(join, {temp(11)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, CallerStackArgumentIsNotTreatedAsUninitializedLocal) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 0), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::LOAD, temp(11), {temp(10)}, 0x408);
  B.ret(b0, {temp(11)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, ReachableUninitializedStackReadIsCorroborated) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::LOAD, temp(11), {temp(10)}, 0x408);
  B.ret(b0, {temp(11)});

  LowFunc LF;
  LF.Entry = B.F.Entry;
  LF.DecodedInstructionCount = 1;
  LF.LiftedInstructionCount = 1;
  LowBlock LB;
  LB.Id = b0;
  LB.StartAddr = B.F.Entry;
  LB.EndAddr = B.F.Entry + 0x10;
  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.Addr = 0x410;
  LB.Ops.push_back(Ret);
  LF.Blocks.push_back(std::move(LB));

  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::vector<MedFunc> MedFuncs{B.F};
  std::vector<LowFunc> LowFuncs{std::move(LF)};
  AnalysisInput In;
  In.Img = &Img;
  In.LowFuncs = &LowFuncs;
  In.MedFuncs = &MedFuncs;
  In.StackRegsKnown = true;
  In.StackPointerReg = kSP;
  auto Fs = auditMemory(In, SinkCatalog::defaults(), SafetyBudgets{});

  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Read->TheConfidence, Confidence::High);
  EXPECT_FALSE(Read->Corroboration.empty());
}

TEST(AllocLifetime, SolverBudgetExhaustionIsReported) {
  FB B("f", 0x100);
  int Entry = B.block();
  int ReadBlock = B.block();
  int Exit = B.block();
  B.op(Entry, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(Entry, NdOp::INT_ADD, temp(10),
       {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.succ(Entry, ReadBlock);
  B.succ(Entry, Exit);
  B.op(ReadBlock, NdOp::LOAD, temp(11), {temp(10)}, 0x118);
  B.ret(ReadBlock, {temp(11)});
  B.ret(Exit, {});

  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::vector<MedFunc> MedFuncs{B.F};
  std::vector<LowFunc> LowFuncs{solverHeavyReturnedPath(B.F.Entry)};
  AnalysisInput In;
  In.Img = &Img;
  In.LowFuncs = &LowFuncs;
  In.MedFuncs = &MedFuncs;
  In.StackRegsKnown = true;
  In.StackPointerReg = kSP;
  SafetyBudgets Budgets;
  Budgets.SolverConflicts = 1;

  auto Fs = auditMemory(In, SinkCatalog::defaults(), Budgets);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Read->BudgetHit) << Read->Detail;
}

TEST(AllocLifetime, WrapperAllocationLeakIsInterprocedural) {
  // xmalloc(n){ return malloc(n); }  user(){ p = xmalloc(16); /* leak */ }
  FB Wrap("xmalloc", 0x200);
  int w0 = Wrap.block();
  Wrap.call(w0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  Wrap.ret(w0, {temp(1)});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "xmalloc", temp(9), {MedVar::makeConst(16, 8)},
            /*Target=*/0x200);
  User.ret(u0, {});

  auto Fs = audit({Wrap.F, User.F});
  bool UserLeak = false;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == 0x100)
      UserLeak = true;
  EXPECT_TRUE(UserLeak);
}

TEST(AllocLifetime, FreedAllocationIsNotReturnedAsOwnedHeap) {
  FB Wrap("released_factory", 0x200);
  int w0 = Wrap.block();
  Wrap.call(w0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  Wrap.call(w0, "free", MedVar{}, {temp(1)});
  Wrap.ret(w0, {temp(1)});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "released_factory", temp(9), {}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Wrap.F, User.F});
  for (const Finding &F : Fs)
    EXPECT_FALSE(F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry)
        << F.Detail;
}

TEST(AllocLifetime, PartiallyFreedAllocationMayStillReturnOwnedHeap) {
  FB Wrap("conditional_factory", 0x200);
  int wEntry = Wrap.block();
  int wFree = Wrap.block();
  int wKeep = Wrap.block();
  int wJoin = Wrap.block();
  Wrap.call(wEntry, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  Wrap.succ(wEntry, wFree);
  Wrap.succ(wEntry, wKeep);
  Wrap.succ(wFree, wJoin);
  Wrap.succ(wKeep, wJoin);
  Wrap.call(wFree, "free", MedVar{}, {temp(1)});
  Wrap.ret(wJoin, {temp(1)});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "conditional_factory", temp(9), {}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Wrap.F, User.F});
  bool UserLeak = false;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry)
      UserLeak = true;
  EXPECT_TRUE(UserLeak);
}

TEST(AllocLifetime, NestedWrapperAllocationLeak) {
  FB Inner("xmalloc", 0x200);
  int i0 = Inner.block();
  Inner.call(i0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  Inner.ret(i0, {temp(1)});

  FB Outer("ymalloc", 0x300);
  int o0 = Outer.block();
  Outer.call(o0, "xmalloc", temp(2), {MedVar::makeConst(16, 8)}, 0x200);
  Outer.ret(o0, {temp(2)});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "ymalloc", temp(9), {MedVar::makeConst(16, 8)}, 0x300);
  User.ret(u0, {});

  auto Fs = audit({User.F, Outer.F, Inner.F});
  bool UserLeak = false;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == 0x100)
      UserLeak = true;
  EXPECT_TRUE(UserLeak);
}

TEST(AllocLifetime, OutParameterAllocatorStatusIsNotAHeapReturn) {
  FB Wrap("aligned_status", 0x200);
  int w0 = Wrap.block();
  Wrap.call(w0, "posix_memalign", temp(1),
            {temp(0), MedVar::makeConst(16, 8), MedVar::makeConst(64, 8)});
  Wrap.ret(w0, {temp(1)});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "aligned_status", temp(9), {}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Wrap.F, User.F});
  ASSERT_EQ(Fs.size(), 1u);
  EXPECT_EQ(Fs[0].FuncEntry, 0x200u);
  EXPECT_EQ(Fs[0].Class, VulnClass::HeapLeak);
  EXPECT_EQ(Fs[0].TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fs[0].Detail, "allocation output handle was not recovered");
}

TEST(AllocLifetime, VoidFunctionDoesNotReturnStaleAllocationRegister) {
  FB Helper("work", 0x200);
  Helper.F.ReturnType = NdType::makeVoid();
  int h0 = Helper.block();
  Helper.call(h0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  Helper.ret(h0, {temp(1)});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "work", temp(9), {}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Helper.F, User.F});
  bool HelperLeak = false;
  bool UserLeak = false;
  for (const Finding &F : Fs) {
    if (F.Class != VulnClass::HeapLeak)
      continue;
    HelperLeak |= F.FuncEntry == 0x200;
    UserLeak |= F.FuncEntry == 0x100;
  }
  EXPECT_TRUE(HelperLeak);
  EXPECT_FALSE(UserLeak);
}

TEST(AllocLifetime, LeakOnOneExitPath) {
  FB B("f", 0x100);
  int b0 = B.block();
  int b1 = B.block();
  int b2 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.succ(b0, b1);
  B.succ(b0, b2);
  B.ret(b1, {});
  B.call(b2, "free", MedVar{}, {temp(1)});
  B.ret(b2, {});
  auto Fs = audit({B.F});
  EXPECT_TRUE(has(Fs, VulnClass::HeapLeak));
}

TEST(AllocLifetime, GuardedFreeWithoutLowIRFailsClosed) {
  FB B("f", 0x100);
  int b0 = B.block();
  int b1 = B.block();
  int b2 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(b0, NdOp::INT_NOTEQUAL, temp(3), {temp(1), MedVar::makeConst(0, 8)});
  B.succ(b0, b1);
  B.succ(b0, b2);
  B.call(b1, "free", MedVar{}, {temp(1)});
  B.ret(b1, {});
  B.ret(b2, {});
  auto Fs = audit({B.F});
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, ReallocResultLeak) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "realloc", temp(2), {temp(1), MedVar::makeConst(32, 8)});
  B.ret(b0, {});
  auto Fs = audit({B.F});
  EXPECT_TRUE(has(Fs, VulnClass::HeapLeak));
}

TEST(AllocLifetime, ReallocIsNotAnUnconditionalRelease) {
  FB B("f", 0x100);
  int b0 = B.block();
  int failure = B.block();
  int success = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "realloc", temp(2), {temp(1), MedVar::makeConst(32, 8)});
  B.op(b0, NdOp::INT_EQUAL, temp(3), {temp(2), MedVar::makeConst(0, 8)});
  B.succ(b0, failure);
  B.succ(b0, success);
  B.op(failure, NdOp::LOAD, temp(4), {temp(1)});
  B.call(failure, "free", MedVar{}, {temp(1)});
  B.ret(failure, {});
  B.call(success, "free", MedVar{}, {temp(2)});
  B.ret(success, {});

  auto Fs = audit({B.F});
  EXPECT_FALSE(has(Fs, VulnClass::UseAfterFree));
  EXPECT_FALSE(has(Fs, VulnClass::DoubleFree));
}

TEST(AllocLifetime, ReallocfAlwaysReleasesOriginalAllocation) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "reallocf", temp(2), {temp(1), MedVar::makeConst(32, 8)});
  B.op(b0, NdOp::LOAD, temp(3), {temp(1)}, 0x408);
  B.call(b0, "free", MedVar{}, {temp(2)});
  B.ret(b0, {});

  auto Fs = audit({B.F});
  EXPECT_TRUE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, FreeWrapperUseAfterFree) {
  FB Wrap("xfree", 0x200);
  Wrap.F.Params.push_back(temp(0));
  int w0 = Wrap.block();
  Wrap.call(w0, "free", MedVar{}, {temp(0)});
  Wrap.ret(w0, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "xfree", MedVar{}, {temp(1)}, 0x200, 0x400);
  User.op(u0, NdOp::LOAD, temp(2), {temp(1)}, 0x408);
  User.ret(u0, {});

  auto Fs = audit({Wrap.F, User.F});
  EXPECT_TRUE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, FreeWrapperSummaryFollowsForwardedParameter) {
  FB Wrap("xfree", 0x200);
  Wrap.F.Params.push_back(temp(0));
  int w0 = Wrap.block();
  Wrap.op(w0, NdOp::COPY, temp(4), {temp(0)});
  Wrap.call(w0, "free", MedVar{}, {temp(4)});
  Wrap.ret(w0, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "xfree", MedVar{}, {temp(1)}, 0x200, 0x400);
  User.op(u0, NdOp::LOAD, temp(2), {temp(1)}, 0x408);
  User.ret(u0, {});

  auto Fs = audit({Wrap.F, User.F});
  EXPECT_TRUE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, FreeWrapperSummaryPreservesEveryReleasedParameter) {
  FB Wrap("release_pair", 0x200);
  Wrap.F.Params.push_back(temp(0));
  Wrap.F.Params.push_back(temp(1));
  int w0 = Wrap.block();
  Wrap.call(w0, "free", MedVar{}, {temp(0)});
  Wrap.call(w0, "free", MedVar{}, {temp(1)});
  Wrap.ret(w0, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(10), {MedVar::makeConst(16, 8)});
  User.call(u0, "malloc", temp(11), {MedVar::makeConst(16, 8)});
  User.call(u0, "release_pair", MedVar{}, {temp(10), temp(11)}, 0x200, 0x400);
  User.op(u0, NdOp::LOAD, temp(12), {temp(10)}, 0x408);
  User.op(u0, NdOp::LOAD, temp(13), {temp(11)}, 0x410);
  User.ret(u0, {});

  auto Fs = audit({Wrap.F, User.F});
  EXPECT_EQ(count(Fs, VulnClass::UseAfterFree), 2u);
}

TEST(AllocLifetime, UnknownCalleeReceivingHandleFailsClosed) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, 0x400);
  B.call(b0, "opaque_runtime", MedVar{}, {temp(1)}, 0x9200, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F});
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, MissingFreeArgumentFailsClosed) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, 0x400);
  B.call(b0, "free", MedVar{}, {}, 0x9100, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F});
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, MissingAllocationResultFailsClosed) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", MedVar{}, {MedVar::makeConst(16, 8)}, 0x9000, 0x400);
  B.ret(b0, {});

  auto Fs = audit({B.F});
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low);
}
