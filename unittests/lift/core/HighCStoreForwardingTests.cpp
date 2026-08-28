//===- HighCStoreForwardingTests.cpp - Bounded forwarding tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"

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
  Func.ReturnType = I32;
  Func.Params.push_back({"arg0", I32});
  Func.Params.push_back({"arg1", I32Ptr});

  auto Seed = [&] { return makeParam(0, 4, I32); };
  auto Slot = [&](unsigned Index) {
    return HighExpr::makeBinop(NdOp::INT_ADD, makeParam(1, 8, I32Ptr),
                               HighExpr::makeConst(Index * 4, 8));
  };

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
