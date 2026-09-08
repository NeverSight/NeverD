//===- HighCStoreForwardingTests.cpp - Bounded forwarding tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/pass/HighC/HighCPasses.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <optional>
#include <string>

namespace {

using namespace neverd;

ExprPtr makeParam(unsigned Id, uint16_t Size, TypeRef Type) {
  MedVar Param;
  Param.Kind = MedVar::Param;
  Param.Id = static_cast<int>(Id);
  Param.Size = Size;
  Param.TheArch = Arch::X64;
  return HighExpr::makeVar(Param, Type);
}

ExprPtr frameSlot(unsigned Offset) {
  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.Id = 100;
  SP.Size = 8;
  SP.TheArch = Arch::X64;
  SP.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  return HighExpr::makeBinop(NdOp::INT_SUB, HighExpr::makeVar(SP),
                             HighExpr::makeConst(Offset, 8));
}

std::optional<llvm::StringRef> functionBody(llvm::StringRef Output,
                                            llvm::StringRef Name) {
  size_t NamePos = Output.find(Name.str() + "(");
  if (NamePos == llvm::StringRef::npos)
    return std::nullopt;
  size_t OpenBrace = Output.find('{', NamePos);
  if (OpenBrace == llvm::StringRef::npos)
    return std::nullopt;

  unsigned Depth = 0;
  for (size_t I = OpenBrace; I < Output.size(); ++I) {
    if (Output[I] == '{')
      ++Depth;
    else if (Output[I] == '}' && --Depth == 0)
      return Output.slice(OpenBrace + 1, I);
  }
  return std::nullopt;
}

size_t countOccurrences(llvm::StringRef Text, llvm::StringRef Needle) {
  size_t Count = 0;
  size_t From = 0;
  while ((From = Text.find(Needle, From)) != llvm::StringRef::npos) {
    ++Count;
    From += Needle.size();
  }
  return Count;
}

TEST(HighCStoreForwarding, BoundsRepeatedTransitiveExpansion) {
  constexpr unsigned ChainLength = 18;
  auto I32 = NdType::makeInt(4);
  auto I32Ptr = NdType::makePtr(I32);

  HighFunc Func;
  Func.Name = "bounded_store_forwarding";
  Func.FrameSize = ChainLength * 4;
  Func.ReturnType = I32;
  Func.Params.push_back({"arg0", I32});

  auto Seed = [&] { return makeParam(0, 4, I32); };
  auto Slot = [&](unsigned Index) { return frameSlot((Index + 1) * 4); };

  for (unsigned I = 0; I < ChainLength; ++I) {
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr = Slot(I);
    if (I == 0) {
      Store.StoreVal = Seed();
    } else {
      auto Previous = [&] { return HighExpr::makeLoad(Slot(I - 1), I32); };
      Store.StoreVal =
          HighExpr::makeBinop(NdOp::INT_ADD, Previous(), Previous());
    }
    Func.Body.push_back(std::move(Store));
  }

  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeLoad(Slot(ChainLength - 1), I32);
  Func.Body.push_back(std::move(Return));

  std::string Output;
  llvm::raw_string_ostream OS(Output);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  OS.flush();

  auto Body = functionBody(Output, Func.Name);
  ASSERT_TRUE(Body.has_value());
  EXPECT_LT(Body->size(), 32u * 1024u) << Body->take_front(4096).str();

  // Crossing the inline budget must keep a real memory boundary.  Keeping
  // every store would avoid the blow-up but regress ordinary forwarding, so
  // require both retained and eliminated stores in this same chain.
  const size_t StoreCalls = countOccurrences(*Body, "neverd_mem_store_");
  EXPECT_GT(StoreCalls, 0u) << Body->take_front(4096).str();
  EXPECT_LT(StoreCalls, ChainLength) << Body->take_front(4096).str();
  EXPECT_TRUE(Body->contains("neverd_mem_load_"))
      << Body->take_front(4096).str();
  EXPECT_TRUE(Body->contains("return ")) << Body->take_front(4096).str();
  EXPECT_TRUE(Body->contains("arg0")) << Body->take_front(4096).str();
  EXPECT_FALSE(Body->contains("truncated: expr too deep"))
      << Body->take_front(4096).str();
}

std::string emitBody(const HighFunc &Func) {
  std::string Output;
  llvm::raw_string_ostream OS(Output);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  EXPECT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  OS.flush();
  auto Body = functionBody(Output, Func.Name);
  EXPECT_TRUE(Body.has_value()) << Output;
  return Body ? Body->str() : Output;
}

HighStmt store(ExprPtr Address, ExprPtr Value) {
  HighStmt Result;
  Result.Kind = StmtKind::Store;
  Result.StoreAddr = std::move(Address);
  Result.StoreVal = std::move(Value);
  return Result;
}

HighStmt ret(ExprPtr Value) {
  HighStmt Result;
  Result.Kind = StmtKind::Return;
  Result.RetVal = std::move(Value);
  return Result;
}

TEST(HighCStoreForwarding, KeepsObservableStoresAndPrecedingLoads) {
  const auto I32 = NdType::makeInt(4);
  const auto Ptr = NdType::makePtr(I32);
  for (bool ReadBeforeWrite : {false, true}) {
    HighFunc Func;
    Func.Name = "observable_write";
    Func.ReturnType = I32;
    Func.Params = {{"arg0", Ptr}, {"arg1", I32}};
    auto Address = [&] { return makeParam(0, 8, Ptr); };
    MedVar Old;
    Old.Kind = MedVar::Temp;
    Old.Id = 7;
    Old.Size = 4;
    if (ReadBeforeWrite) {
      HighStmt Load;
      Load.Kind = StmtKind::Assign;
      Load.Dst = HighExpr::makeVar(Old, I32);
      Load.Val = HighExpr::makeLoad(Address(), I32);
      Func.Body.push_back(Load);
    }
    Func.Body.push_back(store(Address(), makeParam(1, 4, I32)));
    Func.Body.push_back(ret(ReadBeforeWrite ? HighExpr::makeVar(Old, I32)
                                            : makeParam(1, 4, I32)));
    const std::string Body = emitBody(Func);
    EXPECT_EQ(countOccurrences(Body, "neverd_mem_store_"), 1u) << Body;
    if (ReadBeforeWrite) {
      EXPECT_EQ(countOccurrences(Body, "neverd_mem_load_"), 1u) << Body;
      EXPECT_LT(Body.find("neverd_mem_load_"), Body.find("neverd_mem_store_"))
          << Body;
    }
  }
}

TEST(HighCStoreForwarding, DoesNotMoveFrameWritesBeforeLoadsOrAcrossBranches) {
  const auto I32 = NdType::makeInt(4);
  for (bool Branch : {false, true}) {
    HighFunc Func;
    Func.Name = "ordered_frame";
    Func.FrameSize = 8;
    Func.ReturnType = I32;
    Func.Params = {{"arg0", I32}};
    HighStmt Load;
    Load.Kind = StmtKind::Assign;
    MedVar Old;
    Old.Kind = MedVar::Temp;
    Old.Id = 7;
    Old.Size = 4;
    Load.Dst = HighExpr::makeVar(Old, I32);
    Load.Val = HighExpr::makeLoad(frameSlot(4), I32);
    if (Branch) {
      HighStmt If;
      If.Kind = StmtKind::If;
      If.Cond = makeParam(0, 4, I32);
      If.Body.push_back(store(frameSlot(4), makeParam(0, 4, I32)));
      Func.Body.push_back(std::move(If));
      Func.Body.push_back(std::move(Load));
    } else {
      Func.Body.push_back(std::move(Load));
      Func.Body.push_back(store(frameSlot(4), makeParam(0, 4, I32)));
    }
    Func.Body.push_back(ret(HighExpr::makeVar(Old, I32)));
    const auto Body = emitBody(Func);
    EXPECT_EQ(countOccurrences(Body, "neverd_mem_store_"), 1u) << Body;
    EXPECT_EQ(countOccurrences(Body, "neverd_mem_load_"), 1u) << Body;
  }
}

TEST(HighCStoreForwarding, PartialOverlapAndEscapedFramesKeepTheirStores) {
  const auto I32 = NdType::makeInt(4);
  for (bool Escape : {false, true}) {
    HighFunc Func;
    Func.Name = "observable_frame";
    Func.FrameSize = 8;
    Func.ReturnType = NdType::makeInt(8);
    Func.Body.push_back(store(frameSlot(8), HighExpr::makeConst(42, 4)));
    Func.Body.push_back(
        ret(Escape ? frameSlot(8) : HighExpr::makeLoad(frameSlot(7), I32)));
    const auto Body = emitBody(Func);
    EXPECT_EQ(countOccurrences(Body, "neverd_mem_store_"), 1u) << Body;
  }
}

TEST(HighCStoreForwarding, RequiresPrivateFullWidthAndImmutableSlots) {
  const auto I32 = NdType::makeInt(4);
  for (unsigned Variant = 0; Variant < 4; ++Variant) {
    SCOPED_TRACE(Variant);
    HighFunc Func;
    Func.Name = "memory_ownership_boundary";
    Func.FrameSize = 8;
    Func.ReturnType = I32;
    auto Address = [&] {
      if (Variant == 0) {
        auto Cast = std::make_shared<HighExpr>();
        Cast->Kind = ExprKind::Cast;
        Cast->Type = Cast->CastTo = NdType::makeInt(4);
        Cast->Operands.push_back(frameSlot(4));
        return Cast;
      }
      if (Variant == 1)
        return frameSlot(0); // Caller-owned memory above the local frame.
      return frameSlot(4);
    };
    Func.Body.push_back(store(Address(), HighExpr::makeConst(42, 4)));
    if (Variant == 2) {
      auto Call = std::make_shared<HighExpr>();
      Call->Kind = ExprKind::Call;
      Call->CallTarget = "memory_barrier";
      HighStmt Invoke;
      Invoke.Kind = StmtKind::ExprStmt;
      Invoke.Val = Call;
      Func.Body.push_back(std::move(Invoke));
    }
    if (Variant == 3)
      Func.Body.push_back(store(Address(), HighExpr::makeConst(43, 4)));
    Func.Body.push_back(ret(HighExpr::makeLoad(Address(), I32)));
    const auto Body = emitBody(Func);
    EXPECT_EQ(countOccurrences(Body, "neverd_mem_store_"),
              Variant == 3 ? 2u : 1u)
        << Body;
    EXPECT_EQ(countOccurrences(Body, "neverd_mem_load_"), 1u) << Body;
  }
}

TEST(HighCStoreForwarding,
     LaterFrameAssignmentCannotReclassifyAnExternalWrite) {
  const auto I32 = NdType::makeInt(4);
  const auto Ptr = NdType::makePtr(I32);
  HighFunc Func;
  Func.Name = "external_before_frame_alias";
  Func.FrameSize = 8;
  Func.ReturnType = I32;
  Func.Params = {{"arg0", Ptr}};
  Func.Body.push_back(store(makeParam(0, 8, Ptr), HighExpr::makeConst(42, 4)));
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = makeParam(0, 8, Ptr);
  Assign.Val = frameSlot(8);
  Func.Body.push_back(std::move(Assign));
  Func.Body.push_back(ret(HighExpr::makeConst(0, 4)));
  const auto Body = emitBody(Func);
  EXPECT_EQ(countOccurrences(Body, "neverd_mem_store_"), 1u) << Body;
  EXPECT_LT(Body.find("neverd_mem_store_"), Body.find("arg0 =")) << Body;
}

TEST(HighCStoreForwarding, ReinterpretsEachIntegerLoadAfterStoreTruncation) {
  for (uint16_t Size : {1, 2, 4, 8}) {
    for (bool StoreSigned : {false, true}) {
      SCOPED_TRACE(Size);
      SCOPED_TRACE(StoreSigned);
      const auto StoreType = NdType::makeInt(Size, StoreSigned);
      const auto LoadType = NdType::makeInt(Size, !StoreSigned);
      HighFunc Func;
      Func.Name = "forward_integer_interpretation";
      Func.FrameSize = 8;
      Func.ReturnType = NdType::makeInt(8);
      Func.Params = {{"arg0", StoreType}};
      Func.Body.push_back(store(frameSlot(8), makeParam(0, Size, StoreType)));
      Func.Body.push_back(ret(HighExpr::makeLoad(frameSlot(8), LoadType)));
      const auto Body = emitBody(Func);
      // For example, a stored uint8_t(255), reloaded as int8_t and then
      // returned as int64_t, is -1. Returning the forwarded arg0 gives 255.
      const auto Expected = "return (" + typeToC(LoadType) + ")((" +
                            typeToC(StoreType) + ")(arg0));";
      EXPECT_NE(Body.find(Expected), std::string::npos) << Body;
      EXPECT_EQ(countOccurrences(Body, "neverd_mem_store_"), 0u) << Body;
      EXPECT_EQ(countOccurrences(Body, "neverd_mem_load_"), 0u) << Body;
    }
  }
}

TEST(HighCStoreForwarding, TruncatesPromotedArithmeticBeforeWideningTheResult) {
  for (uint16_t Size : {1, 2}) {
    for (bool Signed : {false, true}) {
      SCOPED_TRACE(Size);
      SCOPED_TRACE(Signed);
      const auto Type = NdType::makeInt(Size, Signed);
      HighFunc Func;
      Func.Name = "forward_promoted_arithmetic";
      Func.FrameSize = 8;
      Func.ReturnType = NdType::makeInt(8);
      Func.Params = {{"arg0", Type}, {"arg1", Type}};
      Func.Body.push_back(
          store(frameSlot(8),
                HighExpr::makeBinop(NdOp::INT_ADD, makeParam(0, Size, Type),
                                    makeParam(1, Size, Type))));
      Func.Body.push_back(ret(HighExpr::makeLoad(frameSlot(8), Type)));
      const auto Body = emitBody(Func);
      // uint8_t(250 + 10) must become 4 at the store boundary even when
      // the store and load have exactly the same type; C evaluates + as int.
      const auto Expected = "return (" + typeToC(Type) + ")((" + typeToC(Type) +
                            ")(arg0 + arg1));";
      EXPECT_NE(Body.find(Expected), std::string::npos) << Body;
      EXPECT_EQ(countOccurrences(Body, "neverd_mem_store_"), 0u) << Body;
      EXPECT_EQ(countOccurrences(Body, "neverd_mem_load_"), 0u) << Body;
    }
  }
}

TEST(HighCStoreForwarding, KeepsNonIntegerReinterpretationInMemory) {
  const auto I32 = NdType::makeInt(4);
  const auto F32 = NdType::makeFloat(4);
  const auto Ptr = NdType::makePtr(I32);
  for (const auto &[StoreType, LoadType] :
       std::vector<std::pair<TypeRef, TypeRef>>{{I32, F32},
                                                {F32, I32},
                                                {F32, F32},
                                                {Ptr, NdType::makeInt(8)},
                                                {NdType::makeInt(8), Ptr}}) {
    SCOPED_TRACE(typeToC(StoreType) + " -> " + typeToC(LoadType));
    HighFunc Func;
    Func.Name = "forward_reinterpretation_boundary";
    Func.FrameSize = 8;
    Func.ReturnType = LoadType;
    Func.Params = {{"arg0", StoreType}};
    Func.Body.push_back(
        store(frameSlot(8), makeParam(0, StoreType->Size, StoreType)));
    Func.Body.push_back(ret(HighExpr::makeLoad(frameSlot(8), LoadType)));
    const auto Body = emitBody(Func);
    EXPECT_EQ(countOccurrences(Body, "neverd_mem_store_"), 1u) << Body;
    EXPECT_EQ(countOccurrences(Body, "neverd_mem_load_"), 1u) << Body;
  }
}

TEST(HighCStoreForwarding, KeepsStoreWhenOneAliasWouldRemainUnsubstituted) {
  const auto I32 = NdType::makeInt(4);
  auto Subtracted = frameSlot(8);
  auto Added =
      HighExpr::makeBinop(NdOp::INT_ADD, frameSlot(0)->Operands[0],
                          HighExpr::makeConst(static_cast<uint64_t>(-8), 8));
  HighFunc Func;
  Func.Name = "forward_address_alias";
  Func.FrameSize = 8;
  Func.ReturnType = I32;
  Func.Body.push_back(store(Subtracted, HighExpr::makeConst(42, 4)));
  Func.Body.push_back(ret(
      HighExpr::makeBinop(NdOp::INT_ADD, HighExpr::makeLoad(Subtracted, I32),
                          HighExpr::makeLoad(Added, I32))));
  const auto Body = emitBody(Func);
  EXPECT_EQ(countOccurrences(Body, "neverd_mem_store_"), 1u) << Body;
  EXPECT_EQ(countOccurrences(Body, "neverd_mem_load_"), 2u) << Body;
}

TEST(HighCStoreForwarding, IncludesStoreAndLoadCastsInExpressionBudget) {
  constexpr size_t Limit = 16 * 1024;
  const auto U8 = NdType::makeInt(1, false);
  const auto I8 = NdType::makeInt(1, true);
  const size_t StoreCastBytes = typeToC(U8).size() + 4;
  const size_t LoadCastBytes = typeToC(I8).size() + 4;
  for (bool ExceedsBudget : {false, true}) {
    SCOPED_TRACE(ExceedsBudget);
    HighFunc Func;
    Func.Name = "forward_cast_budget";
    Func.FrameSize = 8;
    Func.ReturnType = NdType::makeInt(8);
    Func.Params = {{"arg0", U8}};
    Func.Body.push_back(store(frameSlot(8), makeParam(0, 1, U8)));
    Func.Body.push_back(ret(HighExpr::makeLoad(frameSlot(8), I8)));
    const size_t ValueBytes =
        Limit - StoreCastBytes - LoadCastBytes + ExceedsBudget;
    auto VarFn = [](const MedVar &Var) {
      return Var.Kind == MedVar::Param ? "arg0" : "frame_base";
    };
    auto ExprFn = [&](const HighExpr &Expr) {
      if (Expr.Kind == ExprKind::Var && Expr.Var.Kind == MedVar::Param)
        return std::string(ValueBytes, 'x');
      return std::string("slot");
    };
    HighCAnalysisState State;
    analyzeDeadStores(State, Func, VarFn, ExprFn);
    ASSERT_TRUE(State.CanElideFrameStores);
    analyzeStoreForwarding(State, Func, VarFn, ExprFn);
    EXPECT_EQ(State.StoreFwd.empty(), ExceedsBudget);
    EXPECT_EQ(State.DeadStmts.count(&Func.Body[0]), !ExceedsBudget);
    if (!State.StoreFwd.empty()) {
      // The final read cast lives in the expression writer rather than the
      // cached store value; both still consume the same expression budget.
      EXPECT_EQ(State.StoreFwd.at("slot").size() + LoadCastBytes, Limit);
    }
  }
}

TEST(HighCStoreForwarding, BoundsTotalExpansionAcrossAllReaders) {
  const auto I8 = NdType::makeInt(1);
  HighFunc Func;
  Func.Name = "forward_total_budget";
  Func.FrameSize = 8;
  Func.ReturnType = NdType::makeInt(8);
  Func.Params = {{"arg0", I8}, {"arg1", I8}};
  Func.Body.push_back(store(frameSlot(8), makeParam(0, 1, I8)));
  Func.Body.push_back(store(frameSlot(4), makeParam(1, 1, I8)));
  Func.Body.push_back(ret(HighExpr::makeBinop(
      NdOp::INT_ADD, HighExpr::makeLoad(frameSlot(8), I8),
      HighExpr::makeBinop(NdOp::INT_ADD, HighExpr::makeLoad(frameSlot(4), I8),
                          HighExpr::makeLoad(frameSlot(4), I8)))));
  auto VarFn = [](const MedVar &Var) {
    return Var.Kind == MedVar::Param ? "arg" + std::to_string(Var.Id)
                                     : std::string("frame_base");
  };
  auto ExprFn = [](const HighExpr &Expr) {
    if (Expr.Kind == ExprKind::Var && Expr.Var.Kind == MedVar::Param)
      return std::string(6000, 'x');
    if (Expr.Kind == ExprKind::BinOp && Expr.Op == NdOp::INT_SUB)
      return "slot" + std::to_string(Expr.Operands[1]->ConstVal);
    return std::string("value");
  };
  HighCAnalysisState State;
  analyzeDeadStores(State, Func, VarFn, ExprFn);
  ASSERT_TRUE(State.CanElideFrameStores);
  analyzeStoreForwarding(State, Func, VarFn, ExprFn);
  // Each cached value fits 16 KiB and their sum also fits, but substituting
  // all three reads does not. One real store/load boundary must remain.
  ASSERT_EQ(State.StoreFwd.size(), 1u);
  EXPECT_EQ(State.DeadStmts.count(&Func.Body[0]) +
                State.DeadStmts.count(&Func.Body[1]),
            1u);
  const auto &[Addr, Value] = *State.StoreFwd.begin();
  const size_t Readers = Addr == "slot4" ? 2 : 1;
  EXPECT_LE((Value.size() + typeToC(I8).size() + 4) * Readers, 16u * 1024u);
}

TEST(HighCStoreForwarding, KeepsCyclicDependenciesMaterialized) {
  auto I32 = NdType::makeInt(4);
  auto I32Ptr = NdType::makePtr(I32);

  HighFunc Func;
  Func.Name = "cyclic_store_forwarding";
  Func.ReturnType = I32;
  Func.Params.push_back({"arg0", I32Ptr});

  auto Slot = [&](unsigned Index) {
    return HighExpr::makeBinop(NdOp::INT_ADD, makeParam(0, 8, I32Ptr),
                               HighExpr::makeConst(Index * 4, 8));
  };

  HighStmt StoreA;
  StoreA.Kind = StmtKind::Store;
  StoreA.StoreAddr = Slot(0);
  StoreA.StoreVal = HighExpr::makeLoad(Slot(1), I32);
  Func.Body.push_back(std::move(StoreA));

  HighStmt StoreB;
  StoreB.Kind = StmtKind::Store;
  StoreB.StoreAddr = Slot(1);
  StoreB.StoreVal = HighExpr::makeLoad(Slot(0), I32);
  Func.Body.push_back(std::move(StoreB));

  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeLoad(Slot(0), I32);
  Func.Body.push_back(std::move(Return));

  std::string Output;
  llvm::raw_string_ostream OS(Output);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  OS.flush();

  auto Body = functionBody(Output, Func.Name);
  ASSERT_TRUE(Body.has_value());
  EXPECT_LT(Body->size(), 4u * 1024u) << Body->take_front(4096).str();
  EXPECT_EQ(countOccurrences(*Body, "neverd_mem_store_"), 2u)
      << Body->take_front(4096).str();
  EXPECT_GE(countOccurrences(*Body, "neverd_mem_load_"), 2u)
      << Body->take_front(4096).str();
}

} // namespace
