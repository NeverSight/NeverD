//===- HighEntryScratchStoreTests.cpp - Entry scratch stores read back ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A private frame store of a scratch register's value at entry (an alignment
// `push rax`) goes only when no path lets anything observe a byte of it
// before the function overwrites that byte.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/lift/X86/X86Regs.h"

namespace neverd {
void elimUndefinedEntryScratchStores(HighFunc &Function, Arch Architecture,
                                     BinaryFormat Format);
} // namespace neverd
using namespace neverd;

namespace {

struct Frame {
  Arch A;
  uint16_t Word;
  explicit Frame(Arch A) : A(A), Word(getTargetRegInfo(A).PointerSize) {}

  ExprPtr reg(uint64_t Offset, int Id, int Version = 0) const {
    MedVar V;
    V.Kind = MedVar::Reg;
    V.TheArch = A;
    V.Id = Id;
    V.SSAVer = Version;
    V.RegOff = Offset;
    V.Size = Word;
    return HighExpr::makeVar(V, NdType::makeInt(Word, false));
  }
  ExprPtr sp() const { return reg(getTargetRegInfo(A).StackPointer, 1); }
  ExprPtr rax() const { return reg(x86reg::RAX, 2); }
  ExprPtr temp(int Id, uint16_t Size) const {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = A;
    V.Id = Id;
    V.Size = Size;
    return HighExpr::makeVar(V, NdType::makeInt(Size, false));
  }
  ExprPtr slot(int64_t Below) const {
    auto Address = HighExpr::makeBinop(NdOp::INT_SUB, sp(),
                                       HighExpr::makeConst(Below, Word));
    Address->Type = NdType::makeInt(Word, false);
    return Address;
  }
  HighStmt store(ExprPtr Address, ExprPtr Value) const {
    HighStmt S;
    S.Kind = StmtKind::Store;
    S.StoreAddr = std::move(Address);
    S.StoreVal = std::move(Value);
    return S;
  }
  ExprPtr load(ExprPtr Address, uint16_t Size) const {
    return HighExpr::makeLoad(std::move(Address), NdType::makeInt(Size, false));
  }
  HighStmt assign(ExprPtr Dst, ExprPtr Value) const {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Dst = std::move(Dst);
    S.Val = std::move(Value);
    return S;
  }
  HighStmt call(std::vector<ExprPtr> Arguments = {}) const {
    HighStmt S;
    S.Kind = StmtKind::Call;
    S.CallExpr = HighExpr::makeCall("g", 0x2000, std::move(Arguments));
    return S;
  }
  HighStmt returning(ExprPtr Value) const {
    HighStmt S;
    S.Kind = StmtKind::Return;
    S.RetVal = std::move(Value);
    return S;
  }
  HighFunc function(std::vector<HighStmt> Body) const {
    HighFunc F;
    F.Name = "f";
    F.FrameSize = 16;
    F.Body = std::move(Body);
    return F;
  }
};

bool removed(const HighFunc &F, size_t Index = 0) {
  return F.Body[Index].Kind == StmtKind::Block;
}

// x64fpc_chain, x64o68_fptern: `push rax`, an intrinsic, then the slot is
// overwritten, a call runs and the slot is read.  RAX has no value at a
// System V entry and no read can see the pushed bytes.
TEST(HighEntryScratchStores, AnOverwrittenAlignmentPushIsNotReadBack) {
  const Frame X(Arch::X64);
  auto Mxcsr = HighExpr::makeCall("neverd_x86_read_mxcsr", 0, {});
  Mxcsr->IntrinsicId = Intrinsic::X86ReadMXCSR;
  Mxcsr->Type = NdType::makeInt(4, false);
  HighFunc F =
      X.function({X.store(X.slot(8), X.rax()), X.assign(X.temp(4, 4), Mxcsr),
                  X.store(X.slot(8), X.temp(5, 8)), X.call(),
                  X.returning(X.load(X.slot(8), 8))});
  elimUndefinedEntryScratchStores(F, Arch::X64, BinaryFormat::ELF);
  EXPECT_TRUE(removed(F));
  EXPECT_EQ(F.Body[2].Kind, StmtKind::Store);
}

// x64o210_dualchase, x64o69_f32acc: only the upper half of the slot is
// rewritten and read; the lower half stays pending across calls and is never
// read.  A read of all eight bytes would still see four pushed ones.
TEST(HighEntryScratchStores, OnlyBytesStillPendingAreReadBack) {
  const Frame X(Arch::X64);
  for (const uint16_t ReadBytes : {4, 8}) {
    SCOPED_TRACE(ReadBytes);
    HighFunc F = X.function(
        {X.store(X.slot(8), X.rax()), X.store(X.slot(4), X.temp(5, 4)),
         X.call(),
         X.returning(X.load(X.slot(ReadBytes == 4 ? 4 : 8), ReadBytes))});
    elimUndefinedEntryScratchStores(F, Arch::X64, BinaryFormat::ELF);
    EXPECT_EQ(removed(F), ReadBytes == 4);
  }
}

// A path that reaches the read without the overwrite keeps the store, and so
// does a loop whose read comes before its overwrite.
TEST(HighEntryScratchStores, AReadOnAnyPathBeforeTheOverwriteKeepsThePush) {
  const Frame X(Arch::X64);
  HighStmt Guarded;
  Guarded.Kind = StmtKind::If;
  Guarded.Cond = X.temp(6, 1);
  Guarded.Body = {X.store(X.slot(8), X.temp(5, 8))};
  HighFunc Branch = X.function({X.store(X.slot(8), X.rax()), Guarded,
                                X.returning(X.load(X.slot(8), 8))});
  elimUndefinedEntryScratchStores(Branch, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(removed(Branch));

  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Cond = X.temp(6, 1);
  Loop.Body = {X.assign(X.temp(7, 8), X.load(X.slot(8), 8)),
               X.store(X.slot(8), X.temp(5, 8))};
  HighFunc Cycle = X.function({X.store(X.slot(8), X.rax()), Loop,
                               X.returning(HighExpr::makeConst(0, 8))});
  elimUndefinedEntryScratchStores(Cycle, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(removed(Cycle));
}

// i386 gives EAX no convention this pass may rely on (a regparm caller sets
// it), so the store goes only by a proof that holds for any value: no call
// before the overwrite (it may read an outgoing stack slot, or be setjmp),
// no frame address handed on or stored, no load the proof cannot place.
TEST(HighEntryScratchStores, AnyOtherScratchRegisterNeedsACallFreeOverwrite) {
  const Frame X(Arch::X86);
  enum { Overwritten, CallFirst, AddressStored, AddressPassed, IndexedLoad };
  for (const int Mode :
       {Overwritten, CallFirst, AddressStored, AddressPassed, IndexedLoad}) {
    SCOPED_TRACE(Mode);
    std::vector<HighStmt> Body{X.store(X.slot(4), X.rax())};
    if (Mode == CallFirst)
      Body.push_back(X.call());
    if (Mode == AddressStored)
      Body.push_back(X.store(HighExpr::makeConst(0x4000, 4), X.slot(4)));
    if (Mode == AddressPassed)
      Body.push_back(X.call({X.slot(4)}));
    if (Mode == IndexedLoad) {
      auto Indexed =
          HighExpr::makeBinop(NdOp::INT_ADD, X.slot(4), X.temp(8, 4));
      Indexed->Type = NdType::makeInt(4, false);
      Body.push_back(X.assign(X.temp(9, 4), X.load(Indexed, 4)));
    }
    Body.push_back(X.store(X.slot(4), X.temp(5, 4)));
    // A later call no longer matters: the pushed bytes are gone.
    Body.push_back(X.call());
    Body.push_back(X.returning(X.load(X.slot(4), 4)));
    HighFunc F = X.function(std::move(Body));
    elimUndefinedEntryScratchStores(F, Arch::X86, BinaryFormat::ELF);
    EXPECT_EQ(removed(F), Mode == Overwritten);
  }
}

} // namespace
