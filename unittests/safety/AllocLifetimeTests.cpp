//===- AllocLifetimeTests.cpp - Heap lifetime defect detection -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

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
                           bool StackRegs = false) {
  static BinaryImage Img;
  AnalysisInput In;
  In.Img = Image ? Image : &Img;
  In.MedFuncs = &Funcs;
  In.StackRegsKnown = StackRegs;
  In.StackPointerReg = kSP;
  return auditHeap(In, SinkCatalog::defaults(), SafetyBudgets{});
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
