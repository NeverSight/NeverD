#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"

namespace neverd {
void elimConsecutiveDeadStores(std::vector<HighStmt> &Statements);
void eliminateUnusedValues(std::vector<HighStmt> &Statements);
void elimUnreadPrivateFrameStores(HighFunc &Function, Arch Architecture);
void narrowSourceConcatLocals(HighFunc &Function);
} // namespace neverd
using namespace neverd;
namespace {
ExprPtr variable(int Version, uint16_t Size = 8, int Rename = -1) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = 8;
  V.SSAVer = Version;
  V.RegOff = 64;
  V.Size = Size;
  V.RenameTag = Rename;
  V.TheArch = Arch::AArch64;
  return HighExpr::makeVar(V, NdType::makeInt(Size, false));
}
HighStmt assign(ExprPtr Dst, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Dst = std::move(Dst);
  S.Val = std::move(Value);
  return S;
}
HighStmt returning(ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = std::move(Value);
  return S;
}

TEST(HighSSADefinitions, SamePhysicalRegisterDoesNotOverwriteEarlierSSAValue) {
  auto A = variable(5), B = variable(8);
  auto Sum = HighExpr::makeBinop(NdOp::INT_ADD, A, B);
  std::vector<HighStmt> Body{assign(A, HighExpr::makeConst(93, 8)),
                             assign(B, HighExpr::makeConst(93, 8)),
                             returning(Sum)};
  elimConsecutiveDeadStores(Body);
  ASSERT_EQ(Body.size(), 3U);
  EXPECT_EQ(Body[0].Dst->Var.SSAVer, 5);
  EXPECT_EQ(Body[1].Dst->Var.SSAVer, 8);
}

TEST(HighSSADefinitions, WideningKeepsBothDistinctValuesAndTheConversion) {
  for (NdOp Extension : {NdOp::INT_ZEXT, NdOp::INT_SEXT}) {
    auto A = variable(1, 4), B = variable(2, 8);
    auto Bits = HighExpr::makeConst(0x80000001, 4);
    auto Wide = HighExpr::makeUnary(Extension, Bits);
    Wide->Type = NdType::makeInt(8, Extension == NdOp::INT_SEXT);
    std::vector<HighStmt> Body{assign(A, Bits), assign(B, Wide), returning(B)};
    elimConsecutiveDeadStores(Body);
    ASSERT_EQ(Body.size(), 3U);
    EXPECT_EQ(Body[1].Val->Op, Extension);
    EXPECT_EQ(Body[2].RetVal->Var.SSAVer, 2);
    EXPECT_EQ(Body[2].RetVal->Type->Size, 8);
  }
}

TEST(HighSSADefinitions, RenamedLocalsAndWidthsDoNotBorrowAnOverwrite) {
  for (bool DifferentWidth : {false, true}) {
    auto A = variable(3, 4, 7);
    auto B = variable(3, DifferentWidth ? 8 : 4, DifferentWidth ? 7 : 8);
    std::vector<HighStmt> Body{assign(A, HighExpr::makeConst(7, 4)),
                               assign(B, HighExpr::makeConst(9, B->Type->Size)),
                               returning(A)};
    elimConsecutiveDeadStores(Body);
    EXPECT_EQ(Body.size(), 3U);
  }
}

TEST(HighSSADefinitions, RealOverwriteKeepsCallEffectAndDependentRead) {
  auto A = variable(3);
  std::vector<HighStmt> Body{assign(A, HighExpr::makeConst(7, 8)),
                             assign(A, HighExpr::makeConst(9, 8)),
                             returning(A)};
  elimConsecutiveDeadStores(Body);
  ASSERT_EQ(Body.size(), 2U);
  EXPECT_EQ(Body[0].Val->ConstVal, 9U);
  Body = {assign(A, HighExpr::makeCall("effect", 0x2000, {})),
          assign(A, HighExpr::makeConst(9, 8)), returning(A)};
  elimConsecutiveDeadStores(Body);
  ASSERT_EQ(Body.size(), 3U);
  EXPECT_EQ(Body[0].Kind, StmtKind::Call);
  auto Call = HighExpr::makeCall("nested_effect", 0x3000, {});
  Call->Type = NdType::makeInt(8);
  Body = {assign(A, HighExpr::makeBinop(NdOp::INT_ADD, Call,
                                        HighExpr::makeConst(1, 8))),
          assign(A, HighExpr::makeConst(9, 8)), returning(A)};
  elimConsecutiveDeadStores(Body);
  ASSERT_EQ(Body.size(), 3U);
  EXPECT_EQ(Body[0].Kind, StmtKind::ExprStmt);
  EXPECT_EQ(Body[0].Val->Operands[0]->Kind, ExprKind::Call);
  Body = {assign(A, HighExpr::makeConst(7, 8)),
          assign(A, HighExpr::makeBinop(NdOp::INT_ADD, A,
                                        HighExpr::makeConst(9, 8))),
          returning(A)};
  elimConsecutiveDeadStores(Body);
  ASSERT_EQ(Body.size(), 3U);
}

TEST(HighSSADefinitions, DeadPhiAssignmentsAreRemovedButLivePhiValuesRemain) {
  auto Dead = variable(3);
  Dead->Kind = ExprKind::Phi;
  auto Unknown = HighExpr::makeUndef(8);
  std::vector<HighStmt> Body{assign(Dead, Unknown),
                             returning(HighExpr::makeConst(7, 8))};
  eliminateUnusedValues(Body);
  ASSERT_EQ(Body.size(), 1U);
  EXPECT_EQ(Body[0].Kind, StmtKind::Return);

  auto Live = variable(4);
  Live->Kind = ExprKind::Phi;
  Body = {assign(Live, HighExpr::makeConst(9, 8)), returning(Live)};
  eliminateUnusedValues(Body);
  ASSERT_EQ(Body.size(), 2U);
  EXPECT_EQ(Body[0].Dst->Kind, ExprKind::Phi);
  EXPECT_EQ(Body[1].RetVal->Kind, ExprKind::Phi);
}

HighFunc privateFrameStore(Arch Architecture) {
  const auto &TRI = getTargetRegInfo(Architecture);
  HighFunc Function;
  Function.FrameSize = 64;
  auto Base = variable(0, TRI.PointerSize);
  Base->Var.RegOff = TRI.StackPointer;
  Base->Var.TheArch = Architecture;
  auto Address = HighExpr::makeBinop(NdOp::INT_SUB, Base,
                                     HighExpr::makeConst(8, TRI.PointerSize));
  Address = HighExpr::makeBinop(NdOp::INT_SUB, Address,
                                HighExpr::makeConst(16, TRI.PointerSize));
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.Addr = 0x1010;
  Store.StoreAddr = Address;
  Store.StoreVal = variable(0);
  Store.StoreVal->Var.RegOff = TRI.IntReturnReg;
  Store.StoreVal->Var.TheArch = Architecture;
  Function.Body = {Store, returning(HighExpr::makeConst(42, 8))};
  return Function;
}

TEST(HighPrivateFrameStores, DiscardsUnreadPaddingAndPreservesBranchEntry) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (uint16_t ImmediateBytes : {1, 2, 4}) {
      auto Function = privateFrameStore(Architecture);
      auto &Address = Function.Body[0].StoreAddr;
      Address->Operands[1] = HighExpr::makeConst(16, ImmediateBytes);
      Address->Operands[0]->Operands[1] =
          HighExpr::makeConst(8, ImmediateBytes);
      elimUnreadPrivateFrameStores(Function, Architecture);
      ASSERT_EQ(Function.Body.size(), 2U);
      EXPECT_EQ(Function.Body[0].Kind, StmtKind::Block);
      EXPECT_EQ(Function.Body[0].Addr, 0x1010U);
      EXPECT_EQ(Function.Body[1].RetVal->ConstVal, 42U);
    }
  }
}

TEST(HighPrivateFrameStores, KeepsReadsEscapesEffectsAndUnprovenRanges) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (unsigned Variant = 0; Variant != 14; ++Variant) {
      SCOPED_TRACE(Variant);
      auto Function = privateFrameStore(Architecture);
      auto &Store = Function.Body[0];
      const auto Address = Store.StoreAddr;
      const auto Base = Address->Operands[0]->Operands[0];
      const auto Size = getTargetRegInfo(Architecture).PointerSize;
      if (Variant == 0)
        Function.Body[1].RetVal =
            HighExpr::makeLoad(Address, NdType::makeInt(8));
      if (Variant == 1)
        Function.Body[1].RetVal = Address;
      if (Variant == 2) {
        auto &Escape = Function.Body[1];
        Escape.Kind = StmtKind::Store;
        Escape.RetVal = nullptr;
        Escape.StoreAddr = HighExpr::makeConst(0x9000, Size);
        Escape.StoreVal = Address;
      }
      if (Variant == 3)
        Function.Body[1] = assign(variable(9), Address);
      if (Variant >= 4 && Variant <= 6)
        Store.StoreAddr =
            HighExpr::makeBinop(NdOp::INT_SUB, Base,
                                HighExpr::makeConst(Variant == 4   ? 4
                                                    : Variant == 5 ? 0
                                                                   : 72,
                                                    Size));
      if (Variant == 7)
        Store.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      if (Variant == 8) {
        Store.StoreVal = HighExpr::makeCall("effect", 0x3000, {});
        Store.StoreVal->Type = NdType::makeInt(8);
      }
      if (Variant == 9)
        Function.Body[1].RetVal = HighExpr::makeCall("unknown", 0x3000, {});
      if (Variant == 10)
        Base->Var.SSAVer = 1;
      if (Variant == 11)
        Base->Var.RenameTag = 2;
      if (Variant == 12)
        Address->Operands[1] = HighExpr::makeConst(UINT64_C(1) << 40, 4);
      if (Variant == 13)
        Address->Operands[1] = HighExpr::makeConst(16, Size * 2);
      elimUnreadPrivateFrameStores(Function, Architecture);
      ASSERT_EQ(Function.Body[0].Kind, StmtKind::Store);
    }
  }
}

ExprPtr paddedInteger() {
  auto Unknown = std::make_shared<HighExpr>();
  Unknown->Kind = ExprKind::Undef;
  Unknown->Type = NdType::makeInt(4, false);
  auto Value = HighExpr::makeBinop(NdOp::CONCAT, Unknown,
                                   HighExpr::makeConst(0x12345678, 4));
  Value->Type = NdType::makeInt(8, false);
  return Value;
}

TEST(HighPrivateFrameStores, TrimsOnlyUnobservedIntegerTailBytes) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    auto Function = privateFrameStore(Architecture);
    auto &Store = Function.Body[0];
    Store.StoreVal = paddedInteger();
    Function.Body[1].RetVal =
        HighExpr::makeLoad(Store.StoreAddr, NdType::makeInt(4, false));
    elimUnreadPrivateFrameStores(Function, Architecture);
    ASSERT_EQ(Store.Kind, StmtKind::Store);
    ASSERT_EQ(Store.StoreVal->Kind, ExprKind::Const);
    EXPECT_EQ(Store.StoreVal->Type->Size, 4U);
    EXPECT_EQ(Store.StoreVal->ConstVal, 0x12345678U);
    EXPECT_EQ(Store.Addr, 0x1010U);
    EXPECT_EQ(Function.Body[1].RetVal->Type->Size, 4U);
  }
}

TEST(HighPrivateFrameStores, PromotedScalarsDiscardOnlyUnobservedUnknownBits) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (auto Extension : {NdOp::INT_ZEXT, NdOp::INT_SEXT}) {
      auto Function = privateFrameStore(Architecture);
      auto &Store = Function.Body[0];
      auto Unknown = std::make_shared<HighExpr>();
      Unknown->Kind = ExprKind::Undef;
      Unknown->Type = NdType::makeInt(8, false);
      auto Upper = HighExpr::makeBinop(NdOp::SUBBYTES, Unknown,
                                       HighExpr::makeConst(4, 4));
      Upper->Type = NdType::makeInt(4, false);
      auto Promoted = HighExpr::makeUnary(Extension, variable(7, 1));
      Promoted->Type = NdType::makeInt(4, Extension == NdOp::INT_SEXT);
      Store.StoreVal = HighExpr::makeBinop(NdOp::CONCAT, Upper, Promoted);
      Store.StoreVal->Type = NdType::makeInt(8, false);
      Function.Body[1].RetVal =
          HighExpr::makeLoad(Store.StoreAddr, NdType::makeInt(4));
      elimUnreadPrivateFrameStores(Function, Architecture);
      EXPECT_EQ(Store.Kind, StmtKind::Store);
      EXPECT_EQ(Store.StoreVal, Promoted);
      EXPECT_EQ(Store.StoreVal->Op, Extension);
      EXPECT_EQ(Store.StoreVal->Operands[0]->Var.SSAVer, 7);
    }
  }
}

TEST(HighPrivateFrameStores, ByteReadsUniteAcrossBranchesAndOverlappingRanges) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (unsigned Byte : {3U, 4U, 6U, 7U}) {
      auto Function = privateFrameStore(Architecture);
      auto &Store = Function.Body[0];
      Store.StoreVal = paddedInteger();
      auto &Branch = Function.Body[1];
      Branch = {};
      Branch.Kind = StmtKind::If;
      Branch.Cond = HighExpr::makeConst(1, 1);
      Branch.Body = {returning(
          HighExpr::makeLoad(Store.StoreAddr, NdType::makeInt(4, false)))};
      auto LastByte = HighExpr::makeBinop(
          NdOp::INT_ADD, Store.StoreAddr,
          HighExpr::makeConst(Byte,
                              getTargetRegInfo(Architecture).PointerSize));
      Branch.ElseBody = {
          returning(HighExpr::makeLoad(LastByte, NdType::makeInt(1, false)))};
      elimUnreadPrivateFrameStores(Function, Architecture);
      ASSERT_EQ(Store.Kind, StmtKind::Store);
      EXPECT_EQ(Store.StoreVal->Type->Size, Byte + 1);
      EXPECT_EQ(Branch.Body.size(), 1U);
      EXPECT_EQ(Branch.ElseBody.size(), 1U);
    }
    auto Function = privateFrameStore(Architecture);
    auto &Store = Function.Body[0];
    Store.StoreVal = paddedInteger();
    auto Disjoint = HighExpr::makeBinop(
        NdOp::INT_ADD, Store.StoreAddr,
        HighExpr::makeConst(8, getTargetRegInfo(Architecture).PointerSize));
    Function.Body[1].RetVal = HighExpr::makeLoad(Disjoint, NdType::makeInt(8));
    elimUnreadPrivateFrameStores(Function, Architecture);
    EXPECT_EQ(Store.Kind, StmtKind::Block);
    EXPECT_EQ(Function.Body[1].RetVal->Kind, ExprKind::Load);
  }
}

TEST(HighPrivateFrameStores, FrameAliasesRequireAnImmutableEntryPrefix) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
      auto Function = privateFrameStore(Architecture);
      auto Alias = variable(9, getTargetRegInfo(Architecture).PointerSize);
      Alias->Var.TheArch = Architecture;
      auto Definition = assign(Alias, Function.Body[0].StoreAddr);
      Function.Body[0].StoreAddr = Alias;
      Function.Body[0].StoreVal = paddedInteger();
      Function.Body[1].RetVal = HighExpr::makeLoad(Alias, NdType::makeInt(4));
      Function.Body.insert(Function.Body.begin(), Definition);
      if (Mutation == 1)
        Function.Body.push_back(Definition);
      if (Mutation == 2) {
        Function.Body[0] = {};
        Function.Body[0].Kind = StmtKind::If;
        Function.Body[0].Cond = HighExpr::makeConst(1, 1);
        Function.Body[0].Body = {Definition};
      }
      if (Mutation == 3)
        Function.Body.back().RetVal = Alias;
      if (Mutation == 4) {
        Function.Body[0].Dst = variable(9, 2);
        Function.Body[0].Dst->Var.TheArch = Architecture;
      }
      if (Mutation == 5)
        Function.Body[0].MemoryOrdering =
            NdMemoryOrdering::SequentiallyConsistent;
      if (Mutation == 6) {
        auto PartialAlias = std::make_shared<HighExpr>(*Alias);
        PartialAlias->Var.Size = 2;
        Function.Body.back().RetVal =
            HighExpr::makeLoad(PartialAlias, NdType::makeInt(4));
      }
      elimUnreadPrivateFrameStores(Function, Architecture);
      EXPECT_EQ(Function.Body[1].StoreVal->Type->Size, Mutation ? 8U : 4U)
          << Mutation;
    }
  }
}

TEST(HighPrivateFrameStores,
     TrimmingPreservesEscapesOrderedReadsAndValueEffects) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
      auto Function = privateFrameStore(Architecture);
      auto &Store = Function.Body[0];
      Store.StoreVal = paddedInteger();
      auto Read = HighExpr::makeLoad(Store.StoreAddr, NdType::makeInt(4));
      Function.Body[1].RetVal = Read;
      if (Mutation == 0)
        Read->MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      if (Mutation == 1)
        Store.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      if (Mutation == 2)
        Function.Body[1].RetVal =
            HighExpr::makeCall("escape", 0x3000, {Store.StoreAddr});
      if (Mutation == 3)
        Store.StoreVal->Operands[0] = HighExpr::makeLoad(
            HighExpr::makeConst(0x9000, 8), NdType::makeInt(4));
      if (Mutation == 4) {
        Store.StoreVal->Operands[0] =
            HighExpr::makeBinop(NdOp::INT_DIV, HighExpr::makeConst(3, 4),
                                HighExpr::makeConst(0, 4));
        Store.StoreVal->Operands[0]->Type = NdType::makeInt(4);
      }
      if (Mutation == 5)
        Store.StoreVal->Operands.pop_back();
      if (Mutation == 6)
        Store.StoreVal->Type = NdType::makeInt(7);
      elimUnreadPrivateFrameStores(Function, Architecture);
      EXPECT_EQ(Store.Kind, StmtKind::Store) << Mutation;
      EXPECT_EQ(Store.StoreVal->Type->Size, Mutation == 6 ? 7U : 8U)
          << Mutation;
    }
  }
}

TEST(HighPrivateFrameStores,
     ObjCSuperRecordReadDoesNotExposeDisjointPrivateBytes) {
  auto Function = privateFrameStore(Arch::AArch64);
  auto &Store = Function.Body[0];
  Store.StoreVal = paddedInteger();
  const auto Base = Store.StoreAddr->Operands[0]->Operands[0];
  const auto SuperAddress =
      HighExpr::makeBinop(NdOp::INT_SUB, Base, HighExpr::makeConst(16, 8));

  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCSuper2;
  Hint.TargetName = "objc_msgSendSuper2";
  Hint.Selector = "init";
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  Hint.Signature.ReturnType = Pointer;
  Hint.Signature.Parameters = {{"receiver", Pointer}, {"command", Pointer}};
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Hint.Signature, Arch::AArch64, Error))
      << Error;
  auto Call = HighExpr::makeCall(
      Hint.TargetName, 0x2000, {SuperAddress, HighExpr::makeConst(0x3000, 8)});
  Call->Type = Pointer;
  Call->SourceCallHint =
      std::make_shared<const SourceCallTypeHint>(std::move(Hint));
  HighStmt CallStatement;
  CallStatement.Kind = StmtKind::Call;
  CallStatement.CallExpr = Call;
  Function.Body.insert(Function.Body.begin() + 1, CallStatement);
  Function.Body.back().RetVal =
      HighExpr::makeLoad(Function.Body[0].StoreAddr, NdType::makeInt(4, false));

  elimUnreadPrivateFrameStores(Function, Arch::AArch64);

  ASSERT_EQ(Function.Body[0].Kind, StmtKind::Store);
  ASSERT_TRUE(Function.Body[0].StoreVal);
  EXPECT_EQ(Function.Body[0].StoreVal->Type->Size, 4U);

  // objc_msgSendSuper2 reads the complete two-pointer objc_super record. A
  // store overlapping that bounded range cannot discard any observed byte.
  auto Overlapping = privateFrameStore(Arch::AArch64);
  Overlapping.Body[0].StoreVal = paddedInteger();
  Overlapping.Body.insert(Overlapping.Body.begin() + 1,
                          std::move(CallStatement));
  Overlapping.Body.back().RetVal = HighExpr::makeLoad(
      Overlapping.Body[0].StoreAddr, NdType::makeInt(4, false));
  Overlapping.Body[1].CallExpr->Operands[0] = Overlapping.Body[0].StoreAddr;
  elimUnreadPrivateFrameStores(Overlapping, Arch::AArch64);
  EXPECT_EQ(Overlapping.Body[0].StoreVal->Type->Size, 8U);
}

TEST(HighPrivateFrameStores,
     StackValueReadsTrackTheirBytesWithoutExposingTheFrame) {
  auto Function = privateFrameStore(Arch::AArch64);
  Function.Body[0].StoreVal = paddedInteger();
  MedVar Incoming;
  Incoming.Kind = MedVar::Stack;
  Incoming.Id = 17;
  Incoming.Size = 8;
  Incoming.StackOff = 0;
  Function.Body.insert(Function.Body.begin() + 1,
                       assign(variable(18), HighExpr::makeVar(Incoming)));
  Function.Body.back().RetVal =
      HighExpr::makeLoad(Function.Body[0].StoreAddr, NdType::makeInt(4, false));

  elimUnreadPrivateFrameStores(Function, Arch::AArch64);

  ASSERT_EQ(Function.Body[0].Kind, StmtKind::Store);
  EXPECT_EQ(Function.Body[0].StoreVal->Type->Size, 4U);

  auto Overlapping = privateFrameStore(Arch::AArch64);
  Overlapping.Body[0].StoreVal = paddedInteger();
  MedVar Local = Incoming;
  Local.Id = 19;
  Local.Size = 8;
  Local.StackOff = -24;
  Overlapping.Body.insert(Overlapping.Body.begin() + 1,
                          assign(variable(20), HighExpr::makeVar(Local)));
  Overlapping.Body.back().RetVal = HighExpr::makeLoad(
      Overlapping.Body[0].StoreAddr, NdType::makeInt(4, false));

  elimUnreadPrivateFrameStores(Overlapping, Arch::AArch64);

  EXPECT_EQ(Overlapping.Body[0].StoreVal->Type->Size, 8U);
}

TEST(HighPrivateFrameStores, ExhaustedByteReadProofLeavesEveryStoreUnchanged) {
  auto Function = privateFrameStore(Arch::AArch64);
  Function.FrameSize = 131072;
  Function.Body[0].StoreVal = paddedInteger();
  auto Base = Function.Body[0].StoreAddr->Operands[0]->Operands[0];
  for (unsigned I = 0; I < 2; ++I) {
    auto At = HighExpr::makeBinop(NdOp::INT_SUB, Base,
                                  HighExpr::makeConst(65536 + I * 65536, 8));
    Function.Body.push_back(assign(
        variable(10 + I), HighExpr::makeLoad(At, NdType::makeInt(65535))));
  }
  elimUnreadPrivateFrameStores(Function, Arch::AArch64);
  EXPECT_EQ(Function.Body[0].Kind, StmtKind::Store);
  EXPECT_EQ(Function.Body[0].StoreVal->Type->Size, 8U);
}

HighFunc sourceConcatLocal(Arch Architecture, unsigned Width,
                           unsigned Carrier = 16) {
  HighFunc F;
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = NdType::makeInt(Width);
  std::string Error;
  EXPECT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error));
  F.SourceTypeHint = Hint;
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = HighExpr::makeConst(1, 1);
  for (auto *Body : {&Branch.Body, &Branch.ElseBody}) {
    auto Upper = std::make_shared<HighExpr>();
    Upper->Kind = ExprKind::Undef;
    Upper->Type = NdType::makeInt(Carrier - Width, false);
    auto Low = HighExpr::makeCall("effect", 0x2000, {});
    Low->Type = NdType::makeInt(Width, false);
    auto Join = HighExpr::makeBinop(NdOp::CONCAT, Upper, Low);
    Join->Type = NdType::makeInt(Carrier, false);
    auto Definition = assign(variable(4, Carrier), Join);
    Definition.Addr = Body == &Branch.Body ? 0x1010 : 0x1020;
    Body->push_back(Definition);
  }
  auto Prefix = HighExpr::makeBinop(NdOp::SUBBYTES, variable(4, Carrier),
                                    HighExpr::makeConst(0, 8));
  Prefix->Type = NdType::makeInt(Width, false);
  F.Body = {Branch, returning(Prefix)};
  return F;
}

TEST(HighSourceScalarLocals, NarrowsEveryDefinitionWithoutMovingLowEffects) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Width : {4U, 8U}) {
      auto F = sourceConcatLocal(Architecture, Width);
      auto Left = F.Body[0].Body[0].Val->Operands[1];
      auto Right = F.Body[0].ElseBody[0].Val->Operands[1];
      narrowSourceConcatLocals(F);
      EXPECT_EQ(F.Body[0].Body[0].Val, Left);
      EXPECT_EQ(F.Body[0].ElseBody[0].Val, Right);
      EXPECT_EQ(F.Body[0].Body[0].Addr, 0x1010U);
      EXPECT_EQ(F.Body[0].ElseBody[0].Addr, 0x1020U);
      EXPECT_EQ(F.Body[0].Body[0].Dst->Var.Size, Width);
      EXPECT_EQ(F.Body[0].ElseBody[0].Dst->Type->Size, Width);
      EXPECT_EQ(F.Body[1].RetVal->Operands[0]->Type->Size, Width);
      EXPECT_EQ(F.Body[1].RetVal->Operands[0]->Var.SSAVer, 4);
    }
}

TEST(HighSourceScalarLocals, NarrowsPureExtendedAlternativeDefinition) {
  auto F = sourceConcatLocal(Arch::AArch64, 8);
  auto Low = variable(7, 8);
  auto Extended = HighExpr::makeUnary(NdOp::INT_ZEXT, Low);
  Extended->Type = NdType::makeInt(16, false);
  F.Body[0].ElseBody[0].Val = Extended;

  narrowSourceConcatLocals(F);

  EXPECT_EQ(F.Body[0].Body[0].Dst->Var.Size, 8U);
  EXPECT_EQ(F.Body[0].ElseBody[0].Dst->Var.Size, 8U);
  EXPECT_EQ(F.Body[0].ElseBody[0].Val->Kind, ExprKind::BinOp);
  EXPECT_EQ(F.Body[0].ElseBody[0].Val->Op, NdOp::SUBBYTES);
  EXPECT_EQ(F.Body[0].ElseBody[0].Val->Type->Size, 8U);
  EXPECT_EQ(F.Body[1].RetVal->Operands[0]->Var.Size, 8U);
}

TEST(HighSourceScalarLocals, NarrowsFullWidthCopyAlternativeDefinition) {
  auto F = sourceConcatLocal(Arch::AArch64, 8);
  F.Body[0].ElseBody[0].Val = variable(7, 16);

  narrowSourceConcatLocals(F);

  EXPECT_EQ(F.Body[0].Body[0].Dst->Var.Size, 8U);
  EXPECT_EQ(F.Body[0].ElseBody[0].Dst->Var.Size, 8U);
  EXPECT_EQ(F.Body[0].ElseBody[0].Val->Kind, ExprKind::BinOp);
  EXPECT_EQ(F.Body[0].ElseBody[0].Val->Op, NdOp::SUBBYTES);
  EXPECT_EQ(F.Body[0].ElseBody[0].Val->Type->Size, 8U);
  EXPECT_EQ(F.Body[0].ElseBody[0].Val->Operands[0]->Var.SSAVer, 7);
  EXPECT_EQ(F.Body[1].RetVal->Operands[0]->Var.Size, 8U);
}

TEST(HighSourceScalarLocals,
     NarrowsExtendedAndCopiedDefinitionsWithoutConcat) {
  auto F = sourceConcatLocal(Arch::AArch64, 8);
  auto Extended = HighExpr::makeUnary(NdOp::INT_ZEXT, variable(6, 8));
  Extended->Type = NdType::makeInt(16, false);
  F.Body[0].Body[0].Val = Extended;
  F.Body[0].ElseBody[0].Val = variable(7, 16);

  narrowSourceConcatLocals(F);

  EXPECT_EQ(F.Body[0].Body[0].Dst->Var.Size, 8U);
  EXPECT_EQ(F.Body[0].ElseBody[0].Dst->Var.Size, 8U);
  EXPECT_EQ(F.Body[0].Body[0].Val->Op, NdOp::SUBBYTES);
  EXPECT_EQ(F.Body[0].ElseBody[0].Val->Op, NdOp::SUBBYTES);
  EXPECT_EQ(F.Body[1].RetVal->Operands[0]->Var.Size, 8U);
}

TEST(HighSourceScalarLocals,
     RetainsUpperReadsEscapesEffectsAndIncompleteProofs) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
      auto F = sourceConcatLocal(Architecture, 8);
      auto &Left = F.Body[0].Body[0];
      auto &Right = F.Body[0].ElseBody[0];
      switch (Mutation) {
      case 0:
        F.Body[1].RetVal->Operands[1]->ConstVal = 8;
        break;
      case 1:
        F.Body[1].RetVal =
            HighExpr::makeCall("escape", 0x3000, {variable(4, 16)});
        break;
      case 2:
        Left.Val->Operands[0] = HighExpr::makeLoad(
            HighExpr::makeConst(0x8000, 8), NdType::makeInt(8));
        break;
      case 3:
        Left.Val->Operands[0] = HighExpr::makeCall("upper_effect", 0x4000, {});
        Left.Val->Operands[0]->Type = NdType::makeInt(8);
        break;
      case 4:
        Right.Val = HighExpr::makeConst(0, 16);
        break;
      case 5:
        Right.Val->Operands[0]->Type = NdType::makeInt(12);
        Right.Val->Operands[1]->Type = NdType::makeInt(4);
        break;
      case 6:
        F.SourceTypeHint.reset();
        break;
      case 7:
        Left.Val->Operands[0]->MemoryOrdering =
            NdMemoryOrdering::SequentiallyConsistent;
        break;
      case 8:
        Left.Val->Operands[0]->IntrinsicOutputs = {variable(9)->Var};
        break;
      case 9:
        F.Body[1].RetVal->Type = NdType::makeInt(16);
        break;
      case 10:
        F.Body[1].RetVal->Operands[0]->Var.RenameTag = 7;
        break;
      case 11:
        F.Body[1].RetVal->Kind = ExprKind::Cast;
        F.Body[1].RetVal->Operands.resize(1);
        F.Body[1].RetVal->CastTo = NdType::makeFloat(8);
        break;
      }
      narrowSourceConcatLocals(F);
      EXPECT_EQ(Left.Dst->Var.Size, 16U) << Mutation;
      EXPECT_EQ(Right.Dst->Type->Size, 16U) << Mutation;
    }
}
TEST(HighSourceScalarLocals, NarrowsRegisterMergesWithExplicitLowWordReads) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Alternative = 0; Alternative < 5; ++Alternative) {
      auto F = sourceConcatLocal(Architecture, 4, 8);
      auto Left = F.Body[0].Body[0].Val->Operands[1];
      auto &Right = F.Body[0].ElseBody[0];
      auto RightLow = Right.Val->Operands[1];
      if (Alternative == 1)
        Right.Val = variable(7, 8);
      else if (Alternative >= 2) {
        Right.Val = HighExpr::makeUnary(
            Alternative == 2 ? NdOp::INT_ZEXT : NdOp::INT_SEXT, variable(7, 4));
        Right.Val->Type = NdType::makeInt(8, Alternative != 2);
        if (Alternative == 4) {
          Right.Val->Kind = ExprKind::Cast;
          Right.Val->CastTo = Right.Val->Type;
        }
      }
      auto OriginalRight = Right.Val;
      F.Body[1].RetVal->Kind = ExprKind::Cast;
      F.Body[1].RetVal->Operands.resize(1);
      F.Body[1].RetVal->CastTo = NdType::makeInt(4, false);
      narrowSourceConcatLocals(F);
      EXPECT_EQ(F.Body[0].Body[0].Val, Left);
      EXPECT_EQ(F.Body[0].Body[0].Addr, 0x1010U);
      EXPECT_EQ(Right.Addr, 0x1020U);
      EXPECT_EQ(F.Body[0].Body[0].Dst->Var.Size, 4U);
      EXPECT_EQ(Right.Dst->Var.Size, 4U);
      EXPECT_EQ(F.Body[1].RetVal->Operands[0]->Var.Size, 4U);
      EXPECT_EQ(F.Body[1].RetVal->Operands[0]->Var.SSAVer, 4);
      if (!Alternative)
        EXPECT_EQ(Right.Val, RightLow);
      else {
        EXPECT_EQ(Right.Val->Op, NdOp::SUBBYTES);
        EXPECT_EQ(Right.Val->Operands[0], OriginalRight);
      }
    }
}

TEST(HighSourceScalarLocals, NarrowsOnlyWhollyMaskedRegisterHighWords) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
      auto F = sourceConcatLocal(Architecture, 4, 8);
      auto Mask = HighExpr::makeConst(
          Mutation == 1 ? UINT64_C(0x100000001) : uint64_t{1}, 8);
      if (Mutation == 2)
        Mask = variable(9, 8);
      if (Mutation == 5)
        Mask = HighExpr::makeConst(1, 4);
      if (Mutation == 6 || Mutation == 7) {
        Mask = HighExpr::makeConst(UINT64_C(0x80000000), 4);
        Mask->Type = NdType::makeInt(4, Mutation == 6);
      }
      auto And = HighExpr::makeBinop(NdOp::INT_AND, variable(4, 8), Mask);
      And->Type = NdType::makeInt(8, false);
      F.Body.back().RetVal = And;
      if (Mutation == 3) {
        HighStmt WideUse;
        WideUse.Kind = StmtKind::ExprStmt;
        WideUse.Val = variable(4, 8);
        F.Body.insert(F.Body.end() - 1, WideUse);
      }
      if (Mutation == 8) {
        HighStmt SharedUse;
        SharedUse.Kind = StmtKind::ExprStmt;
        SharedUse.Val = And;
        F.Body.insert(F.Body.end() - 1, SharedUse);
      }
      if (Mutation == 4) {
        auto Effect = HighExpr::makeCall("upper_effect", 0x4000, {});
        Effect->Type = NdType::makeInt(4, false);
        F.Body[0].Body[0].Val->Operands[0] = Effect;
      }

      narrowSourceConcatLocals(F);

      const bool Narrowed =
          Mutation == 0 || Mutation == 5 || Mutation == 7 || Mutation == 8;
      EXPECT_EQ(F.Body[0].Body[0].Dst->Var.Size, Narrowed ? 4U : 8U)
          << Mutation;
      EXPECT_EQ(F.Body[0].ElseBody[0].Dst->Var.Size, Narrowed ? 4U : 8U)
          << Mutation;
      if (!Narrowed)
        continue;
      const auto &Masked = F.Body.back().RetVal;
      ASSERT_EQ(Masked->Kind, ExprKind::BinOp);
      ASSERT_EQ(Masked->Op, NdOp::INT_AND);
      ASSERT_EQ(Masked->Operands[0]->Kind, ExprKind::UnaryOp);
      EXPECT_EQ(Masked->Operands[0]->Op, NdOp::INT_ZEXT);
      EXPECT_EQ(Masked->Operands[0]->Type->Size, 8U);
      EXPECT_EQ(Masked->Operands[0]->Operands[0]->Var.Size, 4U);
      EXPECT_EQ(Masked->Operands[1]->ConstVal,
                Mutation == 7 ? UINT64_C(0x80000000) : uint64_t{1});
    }
}

TEST(HighSourceScalarLocals, RetainsObservedOrUnprovenRegisterCarrierBytes) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 15; ++Mutation) {
      auto F = sourceConcatLocal(Architecture, 4, 8);
      auto &Left = F.Body[0].Body[0];
      auto &Right = F.Body[0].ElseBody[0];
      switch (Mutation) {
      case 0:
        F.Body[1].RetVal->Operands[1]->ConstVal = 4;
        break;
      case 1:
        F.Body[1].RetVal =
            HighExpr::makeCall("escape", 0x3000, {variable(4, 8)});
        break;
      case 2:
        F.Body[1].RetVal = variable(4, 8);
        break;
      case 3:
        F.Body[1].Kind = StmtKind::Store;
        F.Body[1].StoreAddr = HighExpr::makeConst(0x8000, 8);
        F.Body[1].StoreVal = variable(4, 8);
        F.Body[1].RetVal.reset();
        break;
      case 4:
        Left.Val->Operands[0] = HighExpr::makeLoad(
            HighExpr::makeConst(0x8000, 8), NdType::makeInt(4));
        break;
      case 5:
        Left.Val->Operands[0] = HighExpr::makeCall("upper_effect", 0x4000, {});
        Left.Val->Operands[0]->Type = NdType::makeInt(4);
        break;
      case 6:
        Right.Dst = variable(4, 16);
        Right.Val->Type = NdType::makeInt(16);
        Right.Val->Operands[0]->Type = NdType::makeInt(12);
        break;
      case 7:
        F.Body[1].RetVal->Operands[0] = variable(4, 16);
        break;
      case 8:
        Right.Val = HighExpr::makeUnary(NdOp::INT_ZEXT, variable(7, 2));
        Right.Val->Type = NdType::makeInt(8);
        break;
      case 9:
        Right.Val = variable(7, 16);
        break;
      case 10:
        Left.Val->Operands[0]->MemoryOrdering =
            NdMemoryOrdering::SequentiallyConsistent;
        break;
      case 11:
        Right.Val->Operands[0]->Type = NdType::makeInt(6);
        Right.Val->Operands[1]->Type = NdType::makeInt(2);
        break;
      case 12:
        Left.Val->Operands[0]->IntrinsicOutputs = {variable(4, 8)->Var};
        break;
      case 13:
        Right.Dst->Var.Size = 16;
        break;
      case 14:
        F.Body[1].RetVal->Operands[0]->Var.Size = 16;
        break;
      }
      narrowSourceConcatLocals(F);
      EXPECT_EQ(Left.Dst->Var.Size, 8U) << Mutation;
      EXPECT_EQ(Right.Dst->Var.Size, Mutation == 6 || Mutation == 13 ? 16U : 8U)
          << Mutation;
    }
}
HighFunc sourceCopyChain(Arch Architecture, unsigned Width, unsigned Carrier) {
  auto F = sourceConcatLocal(Architecture, Width, Carrier);
  auto Return = F.Body.back();
  Return.RetVal->Operands[0] = variable(7, Carrier);
  F.Body.resize(1);
  F.Body.push_back(assign(variable(5, Carrier), variable(4, Carrier)));
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = HighExpr::makeConst(1, 1);
  Branch.Body = {assign(variable(6, Carrier), variable(5, Carrier))};
  Branch.ElseBody = {assign(variable(6, Carrier), variable(4, Carrier))};
  F.Body.push_back(Branch);
  F.Body.push_back(assign(variable(7, Carrier), variable(6, Carrier)));
  F.Body.push_back(Return);
  return F;
}

TEST(HighSourceScalarLocals, NarrowsSeededCopyChainsWithoutMovingEffects) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Carrier : {8U, 16U})
      for (unsigned Width : {4U, 8U}) {
        if (Width >= Carrier)
          continue;
        for (const bool Cycle : {false, true}) {
          auto F = sourceCopyChain(Architecture, Width, Carrier);
          auto Left = F.Body[0].Body[0].Val->Operands[1];
          auto Right = F.Body[0].ElseBody[0].Val->Operands[1];
          if (Cycle)
            F.Body[0].ElseBody.push_back(
                assign(variable(4, Carrier), variable(7, Carrier)));
          narrowSourceConcatLocals(F);
          EXPECT_EQ(F.Body[0].Body[0].Val, Left);
          EXPECT_EQ(F.Body[0].ElseBody[0].Val, Right);
          EXPECT_EQ(F.Body[0].Body[0].Addr, 0x1010U);
          EXPECT_EQ(F.Body[0].ElseBody[0].Addr, 0x1020U);
          walkStmts(F.Body, [&](const HighStmt &S) {
            if (S.Kind == StmtKind::Assign) {
              EXPECT_EQ(S.Dst->Var.Size, Width);
              EXPECT_EQ(S.Val->Type->Size, Width);
              if (S.Val->Kind == ExprKind::Var)
                EXPECT_EQ(S.Val->Var.Size, Width);
            }
          });
          EXPECT_EQ(F.Body.back().RetVal->Operands[0]->Var.Size, Width);
        }
      }
}

TEST(HighSourceScalarLocals, CopyChainsRequireEveryWholeCopyConsumerToNarrow) {
  for (unsigned Carrier : {8U, 16U})
    for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
      auto F = sourceCopyChain(Arch::AArch64, 4, Carrier);
      if (Mutation == 0)
        F.Body.back().RetVal = variable(7, Carrier);
      if (Mutation == 1)
        F.Body.back().RetVal =
            HighExpr::makeCall("escape", 0x3000, {variable(7, Carrier)});
      if (Mutation == 2) {
        HighStmt Store;
        Store.Kind = StmtKind::Store;
        Store.StoreAddr = HighExpr::makeConst(0x8000, 8);
        Store.StoreVal = variable(5, Carrier);
        F.Body.push_back(Store);
      }
      if (Mutation == 3)
        F.Body[2].ElseBody[0].Val = HighExpr::makeConst(42, Carrier);
      if (Mutation == 4) {
        auto Shared = F.Body[1].Val;
        HighStmt Call;
        Call.Kind = StmtKind::ExprStmt;
        Call.CallExpr = HighExpr::makeCall("escape_shared", 0x3000, {Shared});
        F.Body.insert(F.Body.end() - 1, Call);
      }
      if (Mutation == 5)
        F.Body.push_back(assign(variable(9, Carrier), variable(7, Carrier)));
      if (Mutation == 6) {
        F.Body[3].Dst->Var.Size = Carrier == 8 ? 16 : 8;
        F.Body[3].Dst->Type = NdType::makeInt(F.Body[3].Dst->Var.Size);
      }
      if (Mutation == 7) {
        auto Other =
            sourceConcatLocal(Arch::AArch64, Carrier == 8 ? 2 : 8, Carrier);
        Other.Body[0].Body[0].Dst = variable(6, Carrier);
        F.Body[2].ElseBody[0] = Other.Body[0].Body[0];
      }
      narrowSourceConcatLocals(F);
      EXPECT_EQ(F.Body[0].Body[0].Dst->Var.Size, Carrier) << Mutation;
      EXPECT_EQ(F.Body[0].ElseBody[0].Dst->Var.Size, Carrier) << Mutation;
      EXPECT_EQ(F.Body[0].Body[0].Val->Op, NdOp::CONCAT) << Mutation;
      if (Mutation == 4) {
        ASSERT_EQ(F.Body[1].Val->Kind, ExprKind::BinOp) << Carrier;
        EXPECT_EQ(F.Body[1].Dst->Var.Size, 4U);
        EXPECT_EQ(F.Body[1].Val->Op, NdOp::SUBBYTES);
        EXPECT_EQ(F.Body[1].Val->Type->Size, 4U);
        EXPECT_EQ(F.Body[1].Val->Operands[0]->Var.Size, Carrier);
      }
    }
}

TEST(HighSourceScalarLocals, UnseededCopiesAndExhaustedProofStayUnchanged) {
  auto F = sourceCopyChain(Arch::AArch64, 4, 8);
  F.Body[0].Body[0].Val = variable(7, 8);
  F.Body[0].ElseBody[0].Val = variable(7, 8);
  narrowSourceConcatLocals(F);
  EXPECT_EQ(F.Body[0].Body[0].Dst->Var.Size, 8U);
  EXPECT_EQ(F.Body[3].Dst->Var.Size, 8U);
  auto Large = sourceCopyChain(Arch::AArch64, 4, 8);
  Large.Body.resize(100001);
  narrowSourceConcatLocals(Large);
  EXPECT_EQ(Large.Body[0].Body[0].Dst->Var.Size, 8U);
  EXPECT_EQ(Large.Body[0].Body[0].Val->Op, NdOp::CONCAT);
}
ExprPtr integerPrefixView(ExprPtr Value, unsigned Bytes) {
  auto E =
      HighExpr::makeBinop(NdOp::SUBBYTES, Value, HighExpr::makeConst(0, 4));
  E->Type = NdType::makeInt(Bytes, false);
  return E;
}

HighFunc sourceCopyViewChain() {
  auto F = sourceCopyChain(Arch::AArch64, 4, 16);
  auto View = integerPrefixView(variable(4, 16), 8);
  View = HighExpr::makeUnary(NdOp::INT_ZEXT, View);
  View->Type = NdType::makeInt(16, false);
  F.Body[1] = assign(variable(5, 8), integerPrefixView(View, 8));
  F.Body[2].Body[0] = assign(variable(6, 8), variable(5, 8));
  F.Body[2].ElseBody[0] = assign(variable(6, 8), variable(5, 8));
  F.Body[3] = assign(variable(7, 8), variable(6, 8));
  F.Body.back().RetVal->Operands[0] = variable(7, 8);
  return F;
}

TEST(HighSourceScalarLocals, CopyViewsPreserveEveryIntermediatePrefix) {
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    auto F = sourceCopyViewChain();
    auto &View = F.Body[1].Val;
    if (Mutation == 1)
      View->Operands[0]->Op = NdOp::INT_SEXT;
    if (Mutation == 2) {
      View->Kind = ExprKind::Cast;
      View->CastTo = NdType::makeInt(8, true);
      View->Type = View->CastTo;
      View->Operands.resize(1);
    }
    if (Mutation == 3) {
      // Eight wrappers are within the witness bound.
      for (unsigned I = 0; I < 5; ++I)
        View = integerPrefixView(View, 8);
    }
    auto Low = F.Body[0].Body[0].Val->Operands[1];
    narrowSourceConcatLocals(F);
    EXPECT_EQ(F.Body[0].Body[0].Dst->Var.Size, 4U) << Mutation;
    EXPECT_EQ(F.Body[0].Body[0].Val, Low);
    EXPECT_EQ(F.Body[1].Dst->Var.Size, 4U) << Mutation;
    EXPECT_EQ(F.Body[1].Val->Type->Size, 4U);
    EXPECT_EQ(F.Body[3].Dst->Var.Size, 4U);
    EXPECT_EQ(F.Body.back().RetVal->Operands[0]->Var.Size, 4U);
  }
}

TEST(HighSourceScalarLocals, CopyViewProofIsBoundedAndScopedToItsRoot) {
  for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
    auto F = sourceCopyViewChain();
    auto &View = F.Body[1].Val;
    if (Mutation == 0)
      View->Operands[0]->Operands[0]->Type = NdType::makeInt(2, false);
    if (Mutation == 1)
      View->Operands[1]->ConstVal = 1;
    if (Mutation == 2) {
      View->Kind = ExprKind::Cast;
      View->Operands.resize(1);
      View->CastTo = NdType::makeInt(4, false);
    }
    if (Mutation == 3)
      View->Operands[0]->Type = NdType::makeFloat(16);
    if (Mutation == 4)
      View->MemoryOrdering = NdMemoryOrdering::Acquire;
    if (Mutation == 5)
      View->IntrinsicOutputs = {variable(9)->Var};
    if (Mutation == 6)
      for (unsigned I = 0; I < 6; ++I)
        View = integerPrefixView(View, 8); // Nine wrappers exceed the bound.
    if (Mutation == 7)
      F.Body.back().RetVal = variable(7, 8);
    if (Mutation == 8) {
      HighStmt Call;
      Call.Kind = StmtKind::ExprStmt;
      Call.CallExpr = HighExpr::makeCall("escape_view", 0x3000, {View});
      F.Body.insert(F.Body.end() - 1, Call);
    }
    if (Mutation == 9) {
      auto Shared = View;
      F.Body.push_back(assign(variable(9, 8), Shared));
      F.Body.push_back(returning(variable(9, 8)));
    }
    if (Mutation == 10) {
      View->Kind = ExprKind::Cast;
      View->Operands.resize(1);
      View->CastTo = NdType::makeInt(8, true); // Type still unsigned.
    }
    if (Mutation == 11)
      View->Operands[0]->Operands[0]->Operands[0]->Type = NdType::makePtr();
    narrowSourceConcatLocals(F);
    if (!Mutation) {
      // The direct two-byte source read is safe, but re-extension cannot
      // establish the four-byte destination prefix.
      EXPECT_EQ(F.Body[0].Body[0].Dst->Var.Size, 4U);
      EXPECT_EQ(F.Body[1].Dst->Var.Size, 8U);
    } else {
      EXPECT_EQ(F.Body[0].Body[0].Dst->Var.Size, 16U) << Mutation;
      EXPECT_EQ(F.Body[0].Body[0].Val->Op, NdOp::CONCAT) << Mutation;
    }
  }
}
TEST(HighSourceScalarLocals, VectorExtensionExposesASecondProvenPrefix) {
  auto F = sourceCopyChain(Arch::AArch64, 4, 16);
  auto View = integerPrefixView(variable(4, 16), 8);
  View = HighExpr::makeUnary(NdOp::INT_ZEXT, View);
  View->Type = NdType::makeInt(16, false);
  F.Body[1].Val = View;
  F.Body[2].ElseBody[0].Val = variable(5, 16);
  narrowSourceConcatLocals(F);
  EXPECT_EQ(F.Body[0].Body[0].Dst->Var.Size, 16U);
  EXPECT_EQ(F.Body[1].Dst->Var.Size, 8U);
  narrowSourceConcatLocals(F);
  EXPECT_EQ(F.Body[0].Body[0].Dst->Var.Size, 4U);
  EXPECT_EQ(F.Body[1].Dst->Var.Size, 4U);
  EXPECT_EQ(F.Body[3].Dst->Var.Size, 4U);
  EXPECT_EQ(F.Body.back().RetVal->Operands[0]->Var.Size, 4U);
}

TEST(HighSourceScalarLocals, AnExplicitPrefixDoesNotDependOnItsConsumerWidth) {
  auto F = sourceConcatLocal(Arch::AArch64, 8, 16);
  auto Prefix = integerPrefixView(variable(4, 16), 8);
  F.Body.insert(F.Body.begin() + 1, assign(variable(5, 8), Prefix));
  F.Body.back().RetVal = variable(5, 8);
  narrowSourceConcatLocals(F);
  EXPECT_EQ(F.Body[0].Body[0].Dst->Var.Size, 8U);
  EXPECT_EQ(F.Body[1].Dst->Var.Size, 8U);
  EXPECT_EQ(F.Body[1].Val->Operands[0]->Var.Size, 8U);
}
} // namespace
