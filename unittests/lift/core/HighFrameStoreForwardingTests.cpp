//===- HighFrameStoreForwardingTests.cpp - Frame value semantics ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check frame load forwarding across widths, effects and source boundaries.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/ir/high/HighSourceMap.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace neverd {
void forwardPrivateFrameLoads(HighFunc &Function, Arch Architecture);
bool forwardPrivateFrameLoads(HighFunc &Function, Arch Architecture,
                              bool AcrossJoins);
bool forwardBoundPrivateFrameCopies(HighFunc &Function, Arch Architecture);
void simplifyExprSemantics(std::vector<HighStmt> &Statements);
void structureIfElse(HighFunc &, int, const MedFunc *);
bool simplifyAllExprs(std::vector<HighStmt> &Statements);
} // namespace neverd

namespace {
using namespace neverd;

constexpr Arch Architectures[] = {Arch::X86, Arch::X64, Arch::ARM,
                                  Arch::AArch64};

ExprPtr parameter(unsigned Index, TypeRef Type, Arch Architecture) {
  MedVar Variable;
  Variable.Kind = MedVar::Param;
  Variable.Id = Index;
  Variable.Size = Type->Size;
  Variable.TheArch = Architecture;
  return HighExpr::makeVar(Variable, Type);
}

ExprPtr temporary(unsigned Index, TypeRef Type, Arch Architecture) {
  auto Result = parameter(Index, Type, Architecture);
  Result->Var.Kind = MedVar::Temp;
  return Result;
}

ExprPtr entryStackPointer(Arch Architecture) {
  const auto &Target = getTargetRegInfo(Architecture);
  auto Result =
      temporary(900, NdType::makeInt(Target.PointerSize, false), Architecture);
  Result->Var.Kind = MedVar::Reg;
  Result->Var.RegOff = Target.StackPointer;
  return Result;
}

ExprPtr frameSlot(Arch Architecture, uint64_t Distance = 16) {
  return HighExpr::makeBinop(
      NdOp::INT_SUB, entryStackPointer(Architecture),
      HighExpr::makeConst(Distance,
                          getTargetRegInfo(Architecture).PointerSize));
}

HighStmt store(ExprPtr Address, ExprPtr Value) {
  HighStmt Result;
  Result.Kind = StmtKind::Store;
  Result.StoreAddr = std::move(Address);
  Result.StoreVal = std::move(Value);
  return Result;
}

HighStmt assign(ExprPtr Destination, ExprPtr Value) {
  HighStmt Result;
  Result.Kind = StmtKind::Assign;
  Result.Dst = std::move(Destination);
  Result.Val = std::move(Value);
  return Result;
}

HighStmt returning(ExprPtr Value) {
  HighStmt Result;
  Result.Kind = StmtKind::Return;
  Result.RetVal = std::move(Value);
  return Result;
}

HighFunc roundTrip(Arch Architecture, TypeRef Type) {
  HighFunc Result;
  Result.Name = "frame_round_trip";
  Result.FrameSize = 64;
  Result.ReturnType = Type;
  Result.Params = {{"arg0", Type}};
  Result.Body = {
      store(frameSlot(Architecture), parameter(0, Type, Architecture)),
      returning(HighExpr::makeLoad(frameSlot(Architecture), Type))};
  return Result;
}

bool hasLoad(const ExprPtr &Root) {
  std::vector<ExprPtr> Pending{Root};
  std::unordered_set<const HighExpr *> Seen;
  while (!Pending.empty()) {
    auto Expression = Pending.back();
    Pending.pop_back();
    if (!Expression || !Seen.insert(Expression.get()).second)
      continue;
    if (Expression->Kind == ExprKind::Load)
      return true;
    Expression->forEachChildExpr(
        [&](const ExprPtr &Child) { Pending.push_back(Child); });
  }
  return false;
}

void compileAndRun(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for emitted C execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-frame-value", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-frame-value", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-frame-value", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream Out(SourcePath, EC);
    ASSERT_FALSE(EC);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (const char *Optimization : {"-O0", "-O2"}) {
    const llvm::SmallVector<llvm::StringRef, 12> Arguments{
        Compiler,
        "-std=c11",
        Optimization,
        "-fsanitize=undefined",
        "-fsanitize-trap=undefined",
        "-Werror=uninitialized",
        "-Werror=return-type",
        SourcePath,
        "-o",
        BinaryPath};
    std::string Error;
    int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                           Redirects, 30, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
    Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                       Redirects, 30, 0, &Error);
    ASSERT_EQ(Result, 0) << Error << '\n' << Source;
  }
}

TEST(HighFrameStoreForwarding, ForwardsExactIntegerReadsAcrossArchitectures) {
  for (Arch Architecture : Architectures)
    for (uint16_t Bytes : {1, 2, 4, 8})
      for (bool Signed : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(Bytes);
        SCOPED_TRACE(Signed);
        auto Function = roundTrip(Architecture, NdType::makeInt(Bytes, Signed));
        forwardPrivateFrameLoads(Function, Architecture);
        EXPECT_FALSE(hasLoad(Function.Body.back().RetVal));
        ASSERT_TRUE(Function.Body.back().RetVal->Type);
        EXPECT_EQ(Function.Body.back().RetVal->Type->Size, Bytes);
        EXPECT_EQ(Function.Body.back().RetVal->Type->IsSigned, Signed);
      }
}

TEST(HighFrameStoreForwarding, AcceptsCommutedFrameAdditionOnly) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto Bytes = getTargetRegInfo(Architecture).PointerSize;
    const auto NegativeOffset =
        Bytes == 4 ? uint64_t(uint32_t(-16)) : uint64_t(int64_t(-16));
    const auto Commuted = [&] {
      return HighExpr::makeBinop(NdOp::INT_ADD,
                                 HighExpr::makeConst(NegativeOffset, Bytes),
                                 entryStackPointer(Architecture));
    };
    for (bool CommutedStore : {false, true}) {
      auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
      if (CommutedStore)
        Function.Body.front().StoreAddr = Commuted();
      else
        Function.Body.back().RetVal->Operands[0] = Commuted();
      forwardPrivateFrameLoads(Function, Architecture);
      EXPECT_FALSE(hasLoad(Function.Body.back().RetVal));
    }
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    Function.Body.back().RetVal->Operands[0] =
        HighExpr::makeBinop(NdOp::INT_SUB, HighExpr::makeConst(16, Bytes),
                            entryStackPointer(Architecture));
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, EmptyDeadStoreAnchorsPreservePrivateFacts) {
  for (Arch Architecture : Architectures)
    for (unsigned Variant = 0; Variant != 3; ++Variant) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Variant);
      auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
      HighStmt Anchor;
      Anchor.Kind = StmtKind::Block;
      Anchor.Addr = 0x401004;
      if (Variant == 1)
        Anchor.Body.push_back(
            store(frameSlot(Architecture), HighExpr::makeConst(7, 4)));
      else if (Variant == 2)
        Anchor.Val = HighExpr::makeConst(7, 4);
      Function.Body.insert(Function.Body.begin() + 1, Anchor);
      forwardPrivateFrameLoads(Function, Architecture);
      EXPECT_EQ(hasLoad(Function.Body.back().RetVal), Variant != 0);
      EXPECT_EQ(Function.Body[1].Kind, StmtKind::Block);
      EXPECT_EQ(Function.Body[1].Addr, 0x401004U);
    }
}

TEST(HighFrameStoreForwarding, PreservesFloatingOperatorsAroundIntegerReads) {
  for (Arch Architecture : Architectures)
    for (bool FusedMultiplyAdd : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(FusedMultiplyAdd);
      const auto Integer = NdType::makeInt(8, false);
      const auto Floating = NdType::makeFloat(8);
      HighFunc Function;
      Function.Name = "stored_floating_bits";
      Function.FrameSize = 64;
      Function.ReturnType = Floating;
      std::vector<ExprPtr> Values;
      for (unsigned Index = 0; Index != (FusedMultiplyAdd ? 3u : 1u); ++Index) {
        const auto Address = frameSlot(Architecture, 16 + 8 * Index);
        Function.Params.push_back({"arg" + std::to_string(Index), Integer});
        Function.Body.push_back(
            store(Address, parameter(Index, Integer, Architecture)));
        Values.push_back(HighExpr::makeBitCast(
            HighExpr::makeLoad(Address, Integer), Floating));
      }
      ExprPtr Result = Values[0];
      if (FusedMultiplyAdd) {
        Result = std::make_shared<HighExpr>();
        Result->Kind = ExprKind::BinOp;
        Result->Op = NdOp::FLOAT_FMA;
        Result->Type = Floating;
        Result->Operands = Values;
      }
      Function.Body.push_back(returning(Result));
      forwardPrivateFrameLoads(Function, Architecture);
      const auto &After = Function.Body.back().RetVal;
      ASSERT_TRUE(After);
      EXPECT_FALSE(hasLoad(After));
      EXPECT_EQ(After->Type, Floating);
      if (FusedMultiplyAdd) {
        EXPECT_EQ(After->Kind, ExprKind::BinOp);
        EXPECT_EQ(After->Op, NdOp::FLOAT_FMA);
        ASSERT_EQ(After->Operands.size(), 3u);
        for (const auto &Operand : After->Operands) {
          EXPECT_EQ(Operand->Kind, ExprKind::BitCast);
          EXPECT_EQ(Operand->Type, Floating);
        }
      } else {
        EXPECT_EQ(After->Kind, ExprKind::BitCast);
      }
    }
}

TEST(HighFrameStoreForwarding, RejectsMalformedOpaqueOperatorArities) {
  for (Arch Architecture : Architectures)
    for (unsigned Operands : {2u, 4u}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Operands);
      const auto Integer = NdType::makeInt(8, false);
      const auto Floating = NdType::makeFloat(8);
      auto Function = roundTrip(Architecture, Integer);
      auto Invalid = std::make_shared<HighExpr>();
      Invalid->Kind = ExprKind::BinOp;
      Invalid->Op = NdOp::FLOAT_FMA;
      Invalid->Type = Floating;
      for (unsigned Index = 0; Index != Operands; ++Index)
        Invalid->Operands.push_back(HighExpr::makeBitCast(
            HighExpr::makeLoad(frameSlot(Architecture), Integer), Floating));
      Function.ReturnType = Floating;
      Function.Body.back().RetVal = Invalid;
      forwardPrivateFrameLoads(Function, Architecture);
      EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
    }
}

TEST(HighFrameStoreForwarding, FollowsDominatingFrameAliases) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto Type = NdType::makeInt(4, false);
    auto Function = roundTrip(Architecture, Type);
    auto Alias = temporary(
        12, NdType::makeInt(getTargetRegInfo(Architecture).PointerSize, false),
        Architecture);
    Function.Body.insert(Function.Body.begin(),
                         assign(Alias, frameSlot(Architecture)));
    Function.Body[1].StoreAddr = Alias;
    Function.Body[2].RetVal->Operands[0] = Alias;
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_FALSE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, LearnsFrameAliasesAfterEarlierSpills) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto Type = NdType::makeInt(4, false);
    const auto PointerType =
        NdType::makeInt(getTargetRegInfo(Architecture).PointerSize, false);
    auto Function = roundTrip(Architecture, Type);
    auto Alias = temporary(12, PointerType, Architecture);
    Function.Body.insert(
        Function.Body.begin(),
        store(frameSlot(Architecture, 32), parameter(1, Type, Architecture)));
    Function.Body.insert(Function.Body.begin() + 1,
                         assign(Alias, frameSlot(Architecture)));
    Function.Body[2].StoreAddr = Alias;
    Function.Body.back().RetVal->Operands[0] = Alias;
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_FALSE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, DoesNotUseLaterAliasForEarlierReads) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto Type = NdType::makeInt(4, false);
    const auto PointerType =
        NdType::makeInt(getTargetRegInfo(Architecture).PointerSize, false);
    auto Function = roundTrip(Architecture, Type);
    auto Alias = temporary(12, PointerType, Architecture);
    auto Earlier = temporary(13, Type, Architecture);
    Function.Body.insert(Function.Body.begin() + 1,
                         assign(Earlier, HighExpr::makeLoad(Alias, Type)));
    Function.Body.insert(Function.Body.begin() + 2,
                         assign(Alias, frameSlot(Architecture)));
    Function.Body.back().RetVal = Earlier;
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body[1].Val));
  }
}

TEST(HighFrameStoreForwarding, RequiresExactTargetWidthForAddressViews) {
  for (Arch Architecture : Architectures)
    for (bool Reinterpret : {false, true})
      for (bool SameWidth : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(Reinterpret);
        SCOPED_TRACE(SameWidth);
        const auto Bytes = getTargetRegInfo(Architecture).PointerSize;
        auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
        auto View = [&](ExprPtr Address) {
          if (!SameWidth)
            Address->Type = NdType::makeInt(Bytes == 4 ? 8 : 4, true);
          auto Result = std::make_shared<HighExpr>();
          Result->Kind = Reinterpret ? ExprKind::BitCast : ExprKind::Cast;
          Result->Type = Result->CastTo = NdType::makeInt(Bytes, false);
          Result->Operands = {std::move(Address)};
          return Result;
        };
        Function.Body[0].StoreAddr = View(frameSlot(Architecture));
        Function.Body.back().RetVal->Operands[0] =
            View(frameSlot(Architecture));
        forwardPrivateFrameLoads(Function, Architecture);
        EXPECT_EQ(hasLoad(Function.Body.back().RetVal), !SameWidth);
      }
}

TEST(HighFrameStoreForwarding, PreservesNarrowStoredComplementsWhenExecuted) {
  std::vector<HighFunc> Functions;
  std::string Main = "int main(void) {\n";
  unsigned Index = 0;
  for (uint16_t Bytes : {1, 2})
    for (bool Signed : {false, true})
      for (bool Spilled : {false, true}) {
        const auto Type = NdType::makeInt(Bytes, Signed);
        auto Function = roundTrip(Arch::X64, Type);
        Function.Name = "stored_complement" + std::to_string(Index++);
        Function.ReturnType = NdType::makeInt(8, Signed);
        auto Value =
            HighExpr::makeUnary(NdOp::INT_NOT, parameter(0, Type, Arch::X64));
        Value->Type = Type;
        if (Spilled) {
          Function.Body[0].StoreVal = Value;
          forwardPrivateFrameLoads(Function, Arch::X64);
        } else {
          Function.FrameSize = 0;
          Function.Body = {returning(Value)};
        }
        simplifyExprSemantics(Function.Body);
        ASSERT_FALSE(hasLoad(Function.Body.back().RetVal));
        const std::string NativeType =
            (Signed ? "int" : "uint") + std::to_string(Bytes * 8) + "_t";
        Main += "for (uint32_t x = 0; x < 65536; ++x) {\n";
        Main += "if (" + Function.Name + "((" + NativeType + ")x) != (" +
                NativeType + ")(~(" + NativeType + ")x)) return " +
                std::to_string(Index) + "; }\n";
        Functions.push_back(std::move(Function));
      }
  Main += "return 0; }\n";
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit(Functions, Out, Options));
  compileAndRun(Source + Main);
}

TEST(HighFrameStoreForwarding, InvalidatesWritesToTheSameEmittedLocal) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    auto Before = temporary(10, Function.ReturnType, Architecture);
    Before->Var.RenameTag = 3;
    Before->Var.SSAVer = 1;
    auto After = temporary(11, Function.ReturnType, Architecture);
    After->Var.RenameTag = 3;
    After->Var.SSAVer = 2;
    Function.Body[0].StoreVal = Before;
    Function.Body.insert(Function.Body.begin() + 1,
                         assign(After, HighExpr::makeConst(7, 4)));
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, DistinguishesUnrenamedVariableKinds) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    auto Temp = temporary(10, Function.ReturnType, Architecture);
    auto Reg = temporary(10, Function.ReturnType, Architecture);
    Reg->Var.Kind = MedVar::Reg;
    Reg->Var.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
    Function.Body[0].StoreVal = Temp;
    Function.Body.insert(Function.Body.begin() + 1,
                         assign(Reg, HighExpr::makeConst(7, 4)));
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_FALSE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, InvalidatesWritesThroughStackVariables) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    auto Home = temporary(10, Function.ReturnType, Architecture);
    Home->Var.Kind = MedVar::Stack;
    Home->Var.StackOff = -16;
    Function.Body.insert(Function.Body.begin() + 1,
                         assign(Home, HighExpr::makeConst(7, 4)));
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, AliasDefinitionsUseTheEmittedLocalIdentity) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto PointerType =
        NdType::makeInt(getTargetRegInfo(Architecture).PointerSize, false);
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    auto Before = temporary(10, PointerType, Architecture);
    Before->Var.RenameTag = 3;
    Before->Var.SSAVer = 1;
    auto After = temporary(11, PointerType, Architecture);
    After->Var.RenameTag = 3;
    After->Var.SSAVer = 2;
    Function.Body.insert(Function.Body.begin(),
                         assign(Before, frameSlot(Architecture)));
    Function.Body[1].StoreAddr = Before;
    Function.Body.insert(Function.Body.begin() + 2,
                         assign(After, frameSlot(Architecture, 32)));
    Function.Body.back().RetVal->Operands[0] = Before;
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, StackHomesCannotBecomeImmutableAddressAliases) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto PointerType =
        NdType::makeInt(getTargetRegInfo(Architecture).PointerSize, false);
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    auto Home = temporary(10, PointerType, Architecture);
    Home->Var.Kind = MedVar::Stack;
    Home->Var.StackOff = -8;
    Function.Body.insert(Function.Body.begin(),
                         assign(Home, frameSlot(Architecture)));
    Function.Body.insert(
        Function.Body.begin() + 1,
        store(frameSlot(Architecture, 8), frameSlot(Architecture, 32)));
    Function.Body[2].StoreAddr = Home;
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, PreservesStoredBitsAcrossSignedIntegerViews) {
  std::vector<HighFunc> Functions;
  std::string Main = R"(
int main(void) {
  const uint64_t boundaries[] = {
    0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff,
    0x7fffffff, 0x80000000, 0xffffffff,
    UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000), UINT64_MAX
  };
)";
  for (Arch Architecture : Architectures)
    for (uint16_t Bytes : {1, 2, 4, 8})
      for (bool Signed : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(Bytes);
        SCOPED_TRACE(Signed);
        const auto Stored = NdType::makeInt(Bytes, Signed);
        const auto Read = NdType::makeInt(Bytes, !Signed);
        auto Function = roundTrip(Architecture, Stored);
        Function.ReturnType = Read;
        Function.Body.back().RetVal->Type = Read;
        forwardPrivateFrameLoads(Function, Architecture);
        ASSERT_FALSE(hasLoad(Function.Body.back().RetVal));
        ASSERT_EQ(Function.Body.back().RetVal->Kind, ExprKind::Cast);
        EXPECT_EQ(Function.Body.back().RetVal->Type, Read);
        EXPECT_EQ(Function.Body.back().RetVal->Type->IsSigned, !Signed);
        if (Architecture != Arch::X64)
          continue;
        Function.Name = "stored_view" + std::to_string(Functions.size());
        const std::string Bits = std::to_string(Bytes * 8) + "_t";
        const std::string WriteType = (Signed ? "int" : "uint") + Bits;
        const std::string ReadType = (Signed ? "uint" : "int") + Bits;
        Main += "{ uint64_t x = 1; for (unsigned i = 0; i < 65536; ++i) {\n";
        Main += "x = x * UINT64_C(6364136223846793005) + 1;\n";
        Main += "uint64_t bits = i < sizeof(boundaries) / sizeof(*boundaries)"
                " ? boundaries[i] : x;\n";
        Main += WriteType + " stored = (" + WriteType + ")bits;\n";
        Main += ReadType + " expected; memcpy(&expected, &stored, "
                           "sizeof(expected));\n";
        Main += "if (" + Function.Name + "(stored) != expected) return " +
                std::to_string(Functions.size() + 1) + "; }}\n";
        Functions.push_back(std::move(Function));
      }
  Main += "return 0; }\n";
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit(Functions, Out, Options));
  compileAndRun("#include <string.h>\n" + Source + Main);
}

TEST(HighFrameStoreForwarding, InvalidatesUnknownAndOverlappingWrites) {
  for (Arch Architecture : Architectures)
    for (unsigned Variant = 0; Variant != 3; ++Variant) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Variant);
      auto Function = roundTrip(Architecture, NdType::makeInt(8, false));
      ExprPtr Address =
          Variant == 0
              ? parameter(1, frameSlot(Architecture)->Type, Architecture)
              : frameSlot(Architecture, Variant == 1 ? 12 : 8);
      Function.Body.insert(Function.Body.begin() + 1,
                           store(Address, HighExpr::makeConst(7, 4)));
      forwardPrivateFrameLoads(Function, Architecture);
      EXPECT_EQ(hasLoad(Function.Body.back().RetVal), Variant != 2);
    }
}

TEST(HighFrameStoreForwarding, DoesNotInventTheWidthOfAnAssignmentStore) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Function = roundTrip(Architecture, NdType::makeInt(8, false));
    Function.Body[0] = assign(
        HighExpr::makeLoad(frameSlot(Architecture), NdType::makeInt(1, false)),
        parameter(0, NdType::makeInt(8, false), Architecture));
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, RetainsUnprovenAccessesAndFrameBounds) {
  for (Arch Architecture : Architectures)
    for (unsigned Variant = 0; Variant != 8; ++Variant) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Variant);
      auto Function = roundTrip(Architecture, NdType::makeInt(8, false));
      if (Variant == 0)
        Function.Body.back().RetVal->Type = NdType::makeInt(4, false);
      if (Variant == 1)
        Function.Body.back().RetVal->MemoryOrdering =
            NdMemoryOrdering::SequentiallyConsistent;
      if (Variant == 2)
        Function.Body[0].MemoryOrdering =
            NdMemoryOrdering::SequentiallyConsistent;
      if (Variant == 3)
        Function.FrameSize = 0;
      if (Variant == 4)
        Function.FrameSize = std::numeric_limits<int64_t>::min();
      if (Variant >= 5) {
        const uint64_t Distance = Variant == 5 ? 4 : Variant == 6 ? 0 : 72;
        Function.Body[0].StoreAddr = frameSlot(Architecture, Distance);
        Function.Body.back().RetVal->Operands[0] =
            frameSlot(Architecture, Distance);
      }
      forwardPrivateFrameLoads(Function, Architecture);
      EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
    }
}

TEST(HighFrameStoreForwarding, KeepsEffectsAndMalformedValuesOutOfSnapshots) {
  for (Arch Architecture : Architectures)
    for (unsigned Variant = 0; Variant != 10; ++Variant) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Variant);
      const auto Type = NdType::makeInt(4, false);
      auto Function = roundTrip(Architecture, Type);
      ExprPtr Value =
          HighExpr::makeBinop(NdOp::INT_ADD, parameter(0, Type, Architecture),
                              HighExpr::makeConst(1, 4));
      switch (Variant) {
      case 0:
        Value->Op = NdOp::ATOMIC_ADD;
        break;
      case 1:
        Value->Op = NdOp::ATOMIC_XCHG;
        break;
      case 2:
        Value->Op = NdOp::INT_DIV;
        break;
      case 3:
        Value->Operands[1] = nullptr;
        break;
      case 4:
        Value->Operands.pop_back();
        break;
      case 5:
        Value->Type = nullptr;
        break;
      case 6:
        Value->IndirectTarget = HighExpr::makeConst(7, 4);
        break;
      case 7:
        Value = HighExpr::makeUndef(4);
        break;
      case 8:
        Value = HighExpr::makeLoad(frameSlot(Architecture, 8), Type);
        break;
      case 9:
        Value = HighExpr::makeCall("effect", 0x2000, {});
        Value->Type = Type;
        break;
      }
      Function.Body[0].StoreVal = Value;
      forwardPrivateFrameLoads(Function, Architecture);
      EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
    }
}

TEST(HighFrameStoreForwarding, DoesNotEraseMalformedMemoryExpressions) {
  for (Arch Architecture : Architectures)
    for (unsigned Variant = 0; Variant != 4; ++Variant) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Variant);
      auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
      if (Variant == 3) {
        auto Address = frameSlot(Architecture);
        // A synthetic entry-SP leaf with children is not a valid address.
        Address->Operands[0]->Operands.push_back(HighExpr::makeConst(7, 4));
        Function.Body[0].StoreAddr = Address;
        Function.Body.back().RetVal->Operands[0] = Address;
      } else {
        auto Invalid =
            HighExpr::makeLoad(frameSlot(Architecture, 8), Function.ReturnType);
        if (Variant == 0)
          Invalid->Operands.clear();
        if (Variant == 1)
          Invalid->Operands.push_back(HighExpr::makeConst(7, 4));
        if (Variant == 2)
          Invalid->Type = nullptr;
        Function.Body.back().RetVal = HighExpr::makeBinop(
            NdOp::INT_ADD, Function.Body.back().RetVal, Invalid);
      }
      forwardPrivateFrameLoads(Function, Architecture);
      auto Observed = Function.Body.back().RetVal;
      if (Variant != 3)
        Observed = Observed->Operands[0];
      EXPECT_EQ(Observed->Kind, ExprKind::Load);
    }
}

TEST(HighFrameStoreForwarding, UnchangedCallInputsRetainSourceIdentity) {
  for (Arch A : Architectures)
    for (bool Assigned : {false, true}) {
      const auto T = NdType::makeInt(getTargetRegInfo(A).PointerSize, false);
      auto F = roundTrip(A, T);
      F.Entry = 0x1000;
      auto Call = HighExpr::makeCall("observe", 0x2000, {frameSlot(A)});
      Call->Type = Assigned ? T : NdType::makeVoid();
      HighStmt S;
      S.Addr = 0x1020;
      if (Assigned) {
        S = assign(temporary(10, T, A), Call);
        S.Addr = 0x1020;
      } else {
        S.Kind = StmtKind::Call;
        S.CallExpr = Call;
      }
      const HighSourceMap Sources = {
          observeHighStatement(F.Entry, {S.Addr, 0}, S)};
      F.Body.insert(F.Body.begin() + 1, std::move(S));
      Call.reset();
      forwardPrivateFrameLoads(F, A);
      const HighStatementSourceIndex Index(Sources);
      const auto Origin = Index.lookup(F.Entry, F.Body[1]);
      ASSERT_TRUE(Origin);
      EXPECT_EQ(Origin->Address, 0x1020u);
      EXPECT_EQ(Origin->Sequence, 0);
      EXPECT_TRUE(hasLoad(F.Body.back().RetVal));
    }
}

TEST(HighFrameStoreForwarding, ForwardsCallInputsAndClearsFactsAfterTheCall) {
  for (Arch Architecture : Architectures)
    for (StmtKind Kind : {StmtKind::Assign, StmtKind::ExprStmt, StmtKind::Call,
                          StmtKind::Return}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(static_cast<int>(Kind));
      const auto Type = NdType::makeInt(4, false);
      auto Function = roundTrip(Architecture, Type);
      auto Input = HighExpr::makeLoad(frameSlot(Architecture), Type);
      auto Call = HighExpr::makeCall("consume", 0x2000, {Input});
      Call->Type = Type;
      HighStmt Barrier;
      Barrier.Kind = Kind;
      if (Kind == StmtKind::Call)
        Barrier.CallExpr = Call;
      else if (Kind == StmtKind::Return)
        Barrier.RetVal = Call;
      else {
        Barrier.Val = Call;
        if (Kind == StmtKind::Assign)
          Barrier.Dst = temporary(1, Type, Architecture);
      }
      Function.Body.insert(Function.Body.begin() + 1, std::move(Barrier));
      if (Kind == StmtKind::Return)
        Function.Body.pop_back();
      forwardPrivateFrameLoads(Function, Architecture);
      const auto &Observed = Function.Body[1];
      const auto Rebuilt = Kind == StmtKind::Call     ? Observed.CallExpr
                           : Kind == StmtKind::Return ? Observed.RetVal
                                                      : Observed.Val;
      ASSERT_EQ(Rebuilt->Kind, ExprKind::Call);
      ASSERT_EQ(Rebuilt->Operands.size(), 1u);
      EXPECT_FALSE(hasLoad(Rebuilt->Operands[0]));
      EXPECT_EQ(Function.Body.front().Kind, StmtKind::Store);
      if (Kind != StmtKind::Return)
        EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
    }
}

TEST(HighFrameStoreForwarding, KeepsReadsAroundNestedCallsAndIndirectTargets) {
  for (Arch Architecture : Architectures)
    for (unsigned Variant = 0; Variant != 3; ++Variant) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Variant);
      auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
      const auto Read = Function.Body.back().RetVal;
      auto Effect = HighExpr::makeCall("effect", 0x2000, {});
      Effect->Type = Function.ReturnType;
      if (Variant == 0)
        Function.Body.back().RetVal =
            HighExpr::makeBinop(NdOp::INT_ADD, Read, Effect);
      else {
        auto Call = HighExpr::makeCall("consume", 0x3000, {Read});
        if (Variant == 1)
          Call->Operands.push_back(Effect);
        else
          Call->IndirectTarget = Effect;
        Function.Body.back().RetVal = Call;
      }
      forwardPrivateFrameLoads(Function, Architecture);
      EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
    }
}

TEST(HighFrameStoreForwarding, DoesNotCarryFactsAcrossBranchesOrCalls) {
  for (Arch Architecture : Architectures)
    for (bool Call : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
      HighStmt Barrier;
      if (Call) {
        Barrier.Kind = StmtKind::ExprStmt;
        Barrier.Val = HighExpr::makeCall("effect", 0x2000, {});
      } else {
        Barrier.Kind = StmtKind::If;
        Barrier.Cond = parameter(1, Function.ReturnType, Architecture);
        Barrier.Body = {
            store(frameSlot(Architecture), HighExpr::makeConst(7, 4))};
      }
      Function.Body.insert(Function.Body.begin() + 1, std::move(Barrier));
      forwardPrivateFrameLoads(Function, Architecture);
      EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
    }
}

TEST(HighFrameStoreForwarding, UnchangedCallsKeepTheirInstructionNavigation) {
  for (Arch A : Architectures)
    for (StmtKind Kind : {StmtKind::Assign, StmtKind::Call})
      for (bool Indirect : {false, true}) {
        SCOPED_TRACE(static_cast<int>(A));
        SCOPED_TRACE(static_cast<int>(Kind));
        SCOPED_TRACE(Indirect);
        auto F = roundTrip(A, NdType::makeInt(4, false));
        F.Entry = 0x1000;
        HighStmt Call;
        Call.Kind = Kind;
        Call.Addr = 0x1010;
        auto Expression =
            HighExpr::makeCall("effect", 0x2000, {HighExpr::makeConst(7, 4)});
        if (Indirect) {
          Expression->IsIndirectCall = true;
          Expression->IndirectTarget = parameter(
              1, NdType::makeInt(getTargetRegInfo(A).PointerSize, false), A);
        }
        if (Kind == StmtKind::Assign) {
          Call.Dst = temporary(20, F.ReturnType, A);
          Call.Val = std::move(Expression);
        } else {
          Call.CallExpr = std::move(Expression);
        }
        F.Body.insert(F.Body.begin() + 1, std::move(Call));
        const HighSourceMap Sources = {
            observeHighStatement(F.Entry, {0x1010, 3}, F.Body[1])};
        forwardPrivateFrameLoads(F, A);
        forwardPrivateFrameLoads(F, A);
        const auto Origin =
            HighStatementSourceIndex(Sources).lookup(F.Entry, F.Body[1]);
        ASSERT_TRUE(Origin);
        EXPECT_EQ(Origin->Address, 0x1010u);
        EXPECT_EQ(Origin->Sequence, 3);
        EXPECT_TRUE(hasLoad(F.Body.back().RetVal));
      }
}

TEST(HighFrameStoreForwarding, OptimizesBothSidesOfACallIndependently) {
  for (Arch A : Architectures) {
    auto F = roundTrip(A, NdType::makeInt(4, false));
    auto Before = temporary(20, F.ReturnType, A);
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    Call.CallExpr = HighExpr::makeCall(
        "effect", 0x2000, {HighExpr::makeLoad(frameSlot(A), F.ReturnType)});
    const auto SharedCall = Call.CallExpr;
    F.Body = {F.Body.front(),
              assign(Before, HighExpr::makeLoad(frameSlot(A), F.ReturnType)),
              Call, store(frameSlot(A), HighExpr::makeConst(7, 4)),
              F.Body.back()};
    forwardPrivateFrameLoads(F, A);
    EXPECT_FALSE(hasLoad(F.Body[1].Val));
    EXPECT_FALSE(hasLoad(F.Body[2].CallExpr->Operands[0]));
    EXPECT_TRUE(hasLoad(SharedCall->Operands[0]));
    EXPECT_FALSE(hasLoad(F.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, CapturedReadsRemoveOnlyRedundantWritebacks) {
  const auto A = Arch::X64;
  const auto Type = NdType::makeInt(8, false);
  auto F = roundTrip(A, Type);
  auto Captured = temporary(20, Type, A);
  F.Body = {assign(Captured, HighExpr::makeLoad(frameSlot(A), Type)),
            store(frameSlot(A, 24), Captured), store(frameSlot(A), Captured),
            returning(HighExpr::makeLoad(frameSlot(A), Type))};
  forwardPrivateFrameLoads(F, A);
  EXPECT_TRUE(hasLoad(F.Body[0].Val));
  EXPECT_EQ(F.Body[1].Kind, StmtKind::Store);
  EXPECT_EQ(F.Body[2].Kind, StmtKind::Block);
  EXPECT_FALSE(hasLoad(F.Body.back().RetVal));
  // The independent original read is retained; rewriting shared observations
  // never changes when memory was sampled.
  EXPECT_EQ(F.Body[0].Val->Kind, ExprKind::Load);
}

TEST(HighFrameStoreForwarding, CapturedReadFactsExpireOnWritesAndCalls) {
  const auto A = Arch::X64;
  const auto Type = NdType::makeInt(8, false);
  for (bool Call : {false, true}) {
    auto F = roundTrip(A, Type);
    auto Captured = temporary(20, Type, A);
    HighStmt Barrier = assign(Captured, HighExpr::makeConst(99, 8));
    if (Call) {
      Barrier.Kind = StmtKind::Call;
      Barrier.Dst.reset();
      Barrier.Val.reset();
      Barrier.CallExpr = HighExpr::makeCall("effect", 0x2000, {});
    }
    F.Body = {assign(Captured, HighExpr::makeLoad(frameSlot(A), Type)), Barrier,
              store(frameSlot(A), Captured), returning(Captured)};
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[2].Kind, StmtKind::Store);
  }
}

TEST(HighFrameStoreForwarding, NestedArgumentEffectsDoNotUseEarlierFacts) {
  auto F = roundTrip(Arch::X64, NdType::makeInt(4, false));
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.CallExpr = HighExpr::makeCall(
      "outer", 0x2000,
      {HighExpr::makeCall("mutate", 0x3000, {}),
       HighExpr::makeLoad(frameSlot(Arch::X64), F.ReturnType)});
  F.Body.insert(F.Body.begin() + 1, Call);
  forwardPrivateFrameLoads(F, Arch::X64);
  EXPECT_TRUE(hasLoad(F.Body[1].CallExpr->Operands[1]));
  EXPECT_TRUE(hasLoad(F.Body.back().RetVal));
}

TEST(HighFrameStoreForwarding, BranchesAndLoopBodiesHavePrivateRegions) {
  for (bool Loop : {false, true}) {
    auto F = roundTrip(Arch::X64, NdType::makeInt(4, false));
    HighStmt Region;
    Region.Kind = Loop ? StmtKind::While : StmtKind::If;
    Region.Cond = parameter(1, F.ReturnType, Arch::X64);
    Region.Body = {
        store(frameSlot(Arch::X64), HighExpr::makeConst(7, 4)),
        assign(temporary(20, F.ReturnType, Arch::X64),
               HighExpr::makeLoad(frameSlot(Arch::X64), F.ReturnType))};
    F.Body.insert(F.Body.begin() + 1, Region);
    forwardPrivateFrameLoads(F, Arch::X64);
    EXPECT_FALSE(hasLoad(F.Body[1].Body[1].Val));
    EXPECT_TRUE(hasLoad(F.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, DoesNotForwardPastAnAlternateEntry) {
  auto F = roundTrip(Arch::X64, NdType::makeInt(4, false));
  F.Body.back().Addr = 0x3000;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x3000;
  F.Body.insert(F.Body.begin(), Jump);
  forwardPrivateFrameLoads(F, Arch::X64);
  EXPECT_TRUE(hasLoad(F.Body.back().RetVal));
}

TEST(HighFrameStoreForwarding, SignExtensionAndBooleanInversionExecuteExactly) {
  std::string Source;
  for (unsigned Bytes : {1u, 2u, 4u}) {
    const auto Type = NdType::makeInt(8, false);
    auto Input = parameter(0, Type, Arch::X64);
    const auto Shift = HighExpr::makeConst(64 - Bytes * 8, 8);
    auto Left = HighExpr::makeBinop(NdOp::INT_LEFT, Input, Shift);
    Left->Type = Type;
    auto Right = HighExpr::makeBinop(NdOp::INT_ASHR, Left, Shift);
    Right->Type = Type;
    auto F = roundTrip(Arch::X64, Type);
    F.Name = "signed_" + std::to_string(Bytes);
    F.FrameSize = 0;
    F.Body = {returning(Right)};
    EXPECT_TRUE(simplifyAllExprs(F.Body));
    EXPECT_EQ(F.Body.back().RetVal->Kind, ExprKind::UnaryOp);
    EXPECT_EQ(F.Body.back().RetVal->Op, NdOp::INT_SEXT);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
    Source += Text;
  }
  Source += R"(
int main(void) {
  uint64_t inputs[] = {0,1,127,128,255,256,32767,32768,65535,
                       0x7fffffff,0x80000000,0xffffffff,UINT64_MAX};
  for (unsigned i=0;i<sizeof(inputs)/sizeof(inputs[0]);++i) {
    uint64_t x=inputs[i];
    if (signed_1(x)!=(uint64_t)(int64_t)(int8_t)x) return 1;
    if (signed_2(x)!=(uint64_t)(int64_t)(int16_t)x) return 2;
    if (signed_4(x)!=(uint64_t)(int64_t)(int32_t)x) return 3;
  }
  return 0;
})";
  compileAndRun(Source);
}

TEST(HighFrameStoreForwarding, InvertedWidenedPredicateKeepsNarrowSignedTest) {
  const auto T = NdType::makeInt(4, true);
  HighFunc F;
  F.Name = "status_guard";
  F.ReturnType = T;
  F.Params = {{"arg0", T}};
  auto Compare =
      HighExpr::makeBinop(NdOp::INT_SLESSEQUAL, HighExpr::makeConst(0, 4),
                          parameter(0, T, Arch::X64));
  auto Extended = HighExpr::makeUnary(NdOp::INT_ZEXT, Compare);
  Extended->Type = NdType::makeInt(8, false);
  HighStmt Guard;
  Guard.Kind = StmtKind::IfElse;
  Guard.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, Extended, HighExpr::makeConst(0, 8));
  Guard.ElseBody = {returning(HighExpr::makeConst(11, 4))};
  F.Body = {Guard, returning(HighExpr::makeConst(7, 4))};
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
  EXPECT_EQ(Source.find("if ((uint64_t)"), std::string::npos) << Source;
  EXPECT_NE(Source.find("arg0 >= 0"), std::string::npos) << Source;
  Source += R"(
int main(void) {
  int32_t inputs[]={INT32_MIN,-1,0,1,INT32_MAX};
  for (unsigned i=0;i<sizeof(inputs)/sizeof(inputs[0]);++i)
    if (status_guard(inputs[i])!=(inputs[i]>=0 ? 11 : 7)) return 1;
  return 0;
})";
  compileAndRun(Source);
}

TEST(HighFrameStoreForwarding, RejectsWritesToTheSyntheticEntryStackPointer) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    Function.Body.insert(
        Function.Body.begin() + 1,
        assign(entryStackPointer(Architecture), frameSlot(Architecture, 32)));
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, BoundsRenderedExpansionOfSharedDags) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    auto Value = parameter(0, Function.ReturnType, Architecture);
    // Only 21 distinct nodes, but over two million rendered occurrences.
    for (unsigned Level = 0; Level != 20; ++Level)
      Value = HighExpr::makeBinop(NdOp::INT_ADD, Value, Value);
    Function.Body[0].StoreVal = Value;
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, DoesNotForwardCyclicValues) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    auto Value = HighExpr::makeBinop(
        NdOp::INT_ADD, parameter(0, Function.ReturnType, Architecture),
        HighExpr::makeConst(1, 4));
    Value->Operands[1] = Value;
    Function.Body[0].StoreVal = Value;
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
    Value->Operands[1] = nullptr;
  }
}

ExprPtr integerView(ExprPtr Value, TypeRef Type) {
  auto Result = std::make_shared<HighExpr>();
  Result->Kind = ExprKind::Cast;
  Result->Type = Result->CastTo = std::move(Type);
  Result->Operands = {std::move(Value)};
  return Result;
}

HighStmt boundCall(Arch Architecture, ExprPtr Argument) {
  auto Hint = std::make_shared<SourceCallTypeHint>();
  Hint->Signature.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
  Hint->Signature.ReturnType = NdType::makeVoid();
  Hint->Signature.Parameters = {{"value", Argument->Type}};
  std::string Error;
  EXPECT_TRUE(assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error))
      << Error;
  HighStmt Result;
  Result.Kind = StmtKind::Call;
  Result.CallExpr = HighExpr::makeCall("observe", 0x2000, {Argument});
  Result.CallExpr->Type = NdType::makeVoid();
  Result.CallExpr->SourceCallHint = std::move(Hint);
  return Result;
}

std::string statementText(const HighFunc &Function) {
  std::string Result;
  for (const auto &S : Function.Body)
    Result += S.str();
  return Result;
}

TEST(HighBoundPrivateFrameCopies, ExecutesSnapshotsAcrossBranchesAndSlotReuse) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    std::vector<HighFunc> Functions;
    std::string Checks;
    for (uint16_t Bytes : {1, 2, 4, 8}) {
      const auto Type = NdType::makeInt(Bytes, false);
      HighFunc F;
      F.Name = "reused_frame_" + std::to_string(Bytes);
      F.FrameSize = 64;
      F.ReturnType = Type;
      F.Params = {{"choose", NdType::makeInt(8, false)},
                  {"first", Type},
                  {"second", Type}};
      HighStmt Branch;
      Branch.Kind = StmtKind::IfElse;
      Branch.Cond = parameter(0, F.Params[0].Type, Architecture);
      Branch.Body = {assign(temporary(1, Type, Architecture),
                            temporary(0, Type, Architecture))};
      Branch.ElseBody = {assign(temporary(1, Type, Architecture),
                                integerView(temporary(0, Type, Architecture),
                                            NdType::makeInt(Bytes, true)))};
      F.Body = {
          store(frameSlot(Architecture), parameter(1, Type, Architecture)),
          assign(temporary(0, Type, Architecture),
                 HighExpr::makeLoad(frameSlot(Architecture), Type)),
          Branch,
          store(frameSlot(Architecture), parameter(2, Type, Architecture)),
          returning(HighExpr::makeBinop(
              NdOp::INT_XOR, temporary(1, Type, Architecture),
              HighExpr::makeLoad(frameSlot(Architecture), Type)))};
      ASSERT_TRUE(forwardBoundPrivateFrameCopies(F, Architecture));
      eliminateHighDeadPhiCopies(F);
      const auto Report = analyzeHighSourceFlow(F, true);
      ASSERT_TRUE(Report.Complete);
      ASSERT_TRUE(Report.Items.empty());
      walkStmts(F.Body, [&](const HighStmt &S) {
        EXPECT_NE(S.Kind, StmtKind::Store);
        forEachRhsExpr(S, [&](const ExprPtr &E) { EXPECT_FALSE(hasLoad(E)); });
      });
      const auto Ty = "uint" + std::to_string(Bytes * 8) + "_t";
      Checks += "if (" + F.Name + "(i & 1, (" + Ty + ")a, (" + Ty + ")b) != (" +
                Ty + ")(a ^ b)) return " + std::to_string(Bytes) + ";\n";
      Functions.push_back(std::move(F));
    }
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    Options.EmitComments = false;
    ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
    compileAndRun(Source + R"(
int main(void) {
  uint64_t a = 0, b = UINT64_MAX;
  for (unsigned i = 0; i < 4096; ++i) {
)" + Checks + R"(
    a = a * UINT64_C(6364136223846793005) + 1;
    b = (b << 7) | (b >> 57);
    b ^= a;
  }
  return 0;
}
)");
  }
}

TEST(HighBoundPrivateFrameCopies, IntersectsAllReachingStores) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (unsigned Variant = 0; Variant != 4; ++Variant) {
      SCOPED_TRACE(Variant);
      const auto Type = NdType::makeInt(8, false);
      auto F = roundTrip(A, Type);
      F.Params.push_back({"other", Type});
      HighStmt Branch;
      Branch.Kind = StmtKind::IfElse;
      Branch.Cond = parameter(0, Type, A);
      Branch.Body = {F.Body[0]};
      Branch.ElseBody = {F.Body[0]};
      if (Variant == 1)
        Branch.ElseBody.clear();
      if (Variant == 2)
        Branch.ElseBody[0].StoreVal = parameter(1, Type, A);
      if (Variant == 3)
        Branch.ElseBody[0].StoreVal =
            integerView(parameter(0, Type, A), NdType::makeInt(8, true));
      F.Body[0] = Branch;
      const auto Before = statementText(F);
      const bool Expected = Variant == 0 || Variant == 3;
      EXPECT_EQ(forwardBoundPrivateFrameCopies(F, A), Expected);
      EXPECT_EQ(hasLoad(F.Body.back().RetVal), !Expected);
      if (!Expected)
        EXPECT_EQ(statementText(F), Before);
    }
}

TEST(HighBoundPrivateFrameCopies, KeepsFactsSeparateAcrossGuardContexts) {
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    const auto T = NdType::makeInt(8, false);
    auto F = roundTrip(A, T);
    F.Params.push_back({"other", T});
    HighStmt Store;
    Store.Kind = StmtKind::IfElse;
    Store.Cond = parameter(0, T, A);
    Store.Body = {store(frameSlot(A), HighExpr::makeConst(71, 8))};
    Store.ElseBody = {store(frameSlot(A), HighExpr::makeConst(23, 8))};
    HighStmt Read;
    Read.Kind = StmtKind::IfElse;
    Read.Cond = parameter(0, T, A);
    Read.Body = {F.Body.back()};
    Read.ElseBody = {F.Body.back()};
    F.Body = {Store, Read};
    ASSERT_TRUE(forwardBoundPrivateFrameCopies(F, A));
    EXPECT_FALSE(hasLoad(F.Body[1].Body[0].RetVal));
    EXPECT_FALSE(hasLoad(F.Body[1].ElseBody[0].RetVal));
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = A;
    ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options));
    compileAndRun(Source + R"(
int main(void) {
  return frame_round_trip(0, 0) != 23 || frame_round_trip(1, 0) != 71 ||
         frame_round_trip(UINT64_MAX, 0) != 71;
}
)");
  }
}

TEST(HighBoundPrivateFrameCopies, InvalidatesWritesToTheSameSourceLocal) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (bool Renamed : {false, true}) {
      const auto T = NdType::makeInt(8, false);
      auto F = roundTrip(A, T);
      auto V = temporary(0, T, A);
      auto W = temporary(Renamed ? 1 : 0, T, A);
      if (Renamed)
        V->Var.RenameTag = W->Var.RenameTag = 12;
      F.Body[0].StoreVal = V;
      F.Body.insert(F.Body.begin(), assign(V, parameter(0, T, A)));
      F.Body.insert(F.Body.begin() + 2,
                    assign(parameter(0, T, A), HighExpr::makeConst(99, 8)));
      F.Body.insert(F.Body.begin() + 3, assign(W, HighExpr::makeConst(12, 8)));
      ASSERT_TRUE(forwardBoundPrivateFrameCopies(F, A));
      EXPECT_TRUE(hasLoad(F.Body.back().RetVal));
    }
}

TEST(HighBoundPrivateFrameCopies, RequiresInitializedInvariantFrameAliases) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (unsigned Variant = 0; Variant != 4; ++Variant) {
      SCOPED_TRACE(Variant);
      const auto T = NdType::makeInt(8, false);
      auto F = roundTrip(A, T);
      auto Alias = temporary(20, T, A);
      HighStmt Define;
      Define.Kind = StmtKind::IfElse;
      Define.Cond = parameter(0, T, A);
      Define.Body = {assign(Alias, frameSlot(A))};
      Define.ElseBody = {assign(Alias, frameSlot(A, Variant == 2 ? 24 : 16))};
      if (Variant == 1)
        Define.ElseBody.clear();
      F.Body[0].StoreAddr = Alias;
      F.Body[1].RetVal = HighExpr::makeLoad(Alias, T);
      F.Body.insert(F.Body.begin() + (Variant == 3 ? 1 : 0), Define);
      const auto Before = statementText(F);
      EXPECT_EQ(forwardBoundPrivateFrameCopies(F, A), Variant == 0);
      if (Variant != 0)
        EXPECT_EQ(statementText(F), Before);
    }
}

TEST(HighBoundPrivateFrameCopies, LaterFrameAssignmentCannotRebindEntryInput) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (bool Renamed : {false, true}) {
      const auto T = NdType::makeInt(8, false);
      auto F = roundTrip(A, T);
      auto Input = parameter(0, T, A);
      auto Destination = Renamed ? temporary(20, T, A) : parameter(0, T, A);
      if (Renamed)
        Input->Var.RenameTag = Destination->Var.RenameTag = 7;
      F.Body = {store(Input, HighExpr::makeConst(7, 8)),
                assign(Destination, frameSlot(A)),
                returning(HighExpr::makeLoad(Destination, T))};
      const auto Before = statementText(F);
      EXPECT_FALSE(forwardBoundPrivateFrameCopies(F, A));
      EXPECT_EQ(statementText(F), Before);
    }
}

TEST(HighBoundPrivateFrameCopies, OrdinaryConversionsDoNotBecomePhiCopies) {
  constexpr auto A = Arch::AArch64;
  const auto T = NdType::makeInt(8, false);
  for (const auto &Destination : {NdType::makeFloat(8), NdType::makeInt(4)}) {
    auto F = roundTrip(A, T);
    F.Body.insert(F.Body.begin(),
                  assign(temporary(20, Destination, A), parameter(0, T, A)));
    ASSERT_TRUE(forwardBoundPrivateFrameCopies(F, A));
    EXPECT_FALSE(F.Body[0].IsPhiCopy);
  }
}

TEST(HighBoundPrivateFrameCopies, AliasViewsMustRetainTheFullPointerWidth) {
  constexpr auto A = Arch::AArch64;
  const auto T = NdType::makeInt(8, false);
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    SCOPED_TRACE(Variant);
    auto F = roundTrip(A, T);
    auto Dst = temporary(20, T, A);
    auto Use = temporary(20, T, A);
    if (Variant == 0 || Variant == 1) {
      Dst->Type = NdType::makeInt(4, false);
      if (Variant == 0)
        Dst->Var.Size = 4;
    } else if (Variant == 2) {
      Use->Type = NdType::makeFloat(8);
    } else {
      Use->Var.Size = 4;
    }
    F.Body[0].StoreAddr = Use;
    F.Body.back().RetVal = HighExpr::makeLoad(Use, T);
    F.Body.insert(F.Body.begin(), assign(Dst, frameSlot(A)));
    const auto Before = statementText(F);
    EXPECT_FALSE(forwardBoundPrivateFrameCopies(F, A));
    EXPECT_EQ(statementText(F), Before);
  }
}

TEST(HighBoundPrivateFrameCopies, CallsRequireABIBindingAndNoFrameEscape) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (unsigned Variant = 0; Variant != 7; ++Variant) {
      SCOPED_TRACE(Variant);
      const auto T = NdType::makeInt(8, false);
      auto F = roundTrip(A, T);
      auto Call = boundCall(A, parameter(0, T, A));
      switch (Variant) {
      case 0:
        break;
      case 1:
        Call.CallExpr->SourceCallHint = nullptr;
        break;
      case 2:
        Call.CallExpr->Operands[0] = frameSlot(A);
        break;
      case 3:
        Call.CallExpr->IndirectTarget = frameSlot(A);
        break;
      case 4:
        Call.CallExpr->Operands.push_back(frameSlot(A));
        break;
      case 5:
        Call.CallExpr->Operands[0] = temporary(20, T, A);
        F.Body.insert(F.Body.begin(),
                      assign(temporary(20, T, A), frameSlot(A)));
        break;
      case 6:
        Call.CallExpr->IntrinsicOutputs = {temporary(2, T, A)->Var};
        break;
      }
      F.Body.insert(F.Body.end() - 1, Call);
      const auto Before = statementText(F);
      EXPECT_EQ(forwardBoundPrivateFrameCopies(F, A), Variant == 0);
      EXPECT_EQ(F.Body[F.Body.size() - 2].CallExpr, Call.CallExpr);
      if (Variant != 0)
        EXPECT_EQ(statementText(F), Before);
    }
}

TEST(HighBoundPrivateFrameCopies, RejectsEscapesEffectsAndIncompleteFlow) {
  for (unsigned Variant = 0; Variant != 15; ++Variant) {
    SCOPED_TRACE(Variant);
    constexpr auto A = Arch::AArch64;
    const auto T = NdType::makeInt(8, false);
    auto F = roundTrip(A, T);
    HighStmt Extra;
    switch (Variant) {
    case 0:
      Extra = store(parameter(0, T, A), frameSlot(A));
      break;
    case 1:
      Extra = returning(frameSlot(A));
      break;
    case 2:
      Extra.Kind = StmtKind::ExprStmt;
      Extra.Val = std::make_shared<HighExpr>();
      Extra.Val->Kind = ExprKind::Addr;
      Extra.Val->Operands = {parameter(0, T, A)};
      break;
    case 3:
      F.Body[0].MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    case 4:
      F.Body.back().RetVal->MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 5:
      F.Body.back().RetVal->Op = NdOp::ATOMIC_ADD;
      break;
    case 6:
      Extra = assign(entryStackPointer(A), parameter(0, T, A));
      break;
    case 7:
      Extra.Kind = StmtKind::Goto;
      Extra.GotoTarget = 0x8000;
      break;
    case 8:
      Extra.Kind = StmtKind::While;
      Extra.Cond = parameter(0, T, A);
      break;
    case 9:
      F.Body[0].Addr = 0x8000;
      Extra.Kind = StmtKind::Goto;
      Extra.GotoTarget = 0x8000;
      break;
    case 10:
      F.Body[0].StoreAddr = frameSlot(A, 72);
      break;
    case 11:
      F.Body[0].StoreVal->Operands = {HighExpr::makeConst(12, 8)};
      break;
    case 12:
      F.Body[0].Body = {boundCall(A, parameter(0, T, A))};
      break;
    case 13:
      F.Body.back().RetVal->Operands[0] = nullptr;
      break;
    case 14:
      F.Body[0].CallExpr = boundCall(A, parameter(0, T, A)).CallExpr;
      break;
    }
    F.Body.insert(F.Body.end() - 1, Extra);
    const auto OriginalLoad = F.Body.back().RetVal;
    EXPECT_FALSE(forwardBoundPrivateFrameCopies(F, A));
    EXPECT_EQ(F.Body.back().RetVal, OriginalLoad);
  }
}

TEST(HighBoundPrivateFrameCopies, KeepsOverlappingAndMixedWidthSlots) {
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    constexpr auto A = Arch::AArch64;
    const auto T = NdType::makeInt(8, false);
    auto F = roundTrip(A, T);
    const auto Other = Variant == 2
                           ? NdType::makeFloat(8)
                           : NdType::makeInt(Variant == 0 ? 4 : 8, false);
    if (Variant == 3)
      F.Body[0].StoreVal = HighExpr::makeBinop(
          NdOp::INT_ADD, parameter(0, T, A), HighExpr::makeConst(1, 8));
    else
      F.Body.insert(
          F.Body.end() - 1,
          store(frameSlot(A, Variant == 1 ? 20 : 16), temporary(2, Other, A)));
    EXPECT_FALSE(forwardBoundPrivateFrameCopies(F, A));
    EXPECT_TRUE(hasLoad(F.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, OverwritesWithinOneRegionKeepTheFinalWrite) {
  for (const auto A : Architectures) {
    const auto T = NdType::makeInt(4, false);
    for (const bool Same : {false, true}) {
      auto F = roundTrip(A, T);
      auto First = store(frameSlot(A), HighExpr::makeConst(3, 4));
      First.Addr = 0x1000;
      F.Body = {First, store(frameSlot(A, 24), HighExpr::makeConst(8, 4)),
                store(frameSlot(A), HighExpr::makeConst(Same ? 3 : 5, 4)),
                returning(HighExpr::makeLoad(frameSlot(A), T))};
      forwardPrivateFrameLoads(F, A);
      EXPECT_EQ(F.Body[Same ? 2 : 0].Kind, StmtKind::Block);
      EXPECT_EQ(F.Body[Same ? 0 : 2].Kind, StmtKind::Store);
      EXPECT_EQ(F.Body[0].Addr, 0x1000u);
      EXPECT_FALSE(hasLoad(F.Body.back().RetVal));
    }
  }
}

TEST(HighFrameStoreForwarding, OverwriteProofStopsAtObservableBoundaries) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    auto F = roundTrip(A, T);
    HighStmt Between;
    Between.Kind = StmtKind::ExprStmt;
    if (Variant == 0)
      Between.Val = HighExpr::makeLoad(parameter(0, T, A), T);
    else if (Variant == 1)
      Between.Val = HighExpr::makeCall("observe", 0x2000, {frameSlot(A)});
    else if (Variant == 2)
      Between.Val = HighExpr::makeBinop(
          NdOp::INT_DIV, HighExpr::makeConst(7, 8), parameter(0, T, A));
    else {
      Between.Kind = StmtKind::Store;
      Between.StoreAddr = frameSlot(A, 12);
      Between.StoreVal = HighExpr::makeConst(7, 4);
    }
    F.Body.insert(F.Body.begin() + 1, Between);
    F.Body.insert(F.Body.end() - 1,
                  store(frameSlot(A), HighExpr::makeConst(9, 8)));
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[0].Kind, StmtKind::Store) << Variant;
  }
}

TEST(HighFrameStoreForwarding, StackHomeReadObservesAnOverwrittenStore) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto F = roundTrip(Architecture, NdType::makeInt(4, false));
    // The Stack home names the bytes both stores write, as in
    // InvalidatesWritesThroughStackVariables. Reading it observes the first.
    auto Home = temporary(10, F.ReturnType, Architecture);
    Home->Var.Kind = MedVar::Stack;
    Home->Var.StackOff = -16;
    auto Read = temporary(11, F.ReturnType, Architecture);
    F.Body = {store(frameSlot(Architecture), HighExpr::makeConst(3, 4)),
              assign(Read, Home),
              store(frameSlot(Architecture), HighExpr::makeConst(5, 4)),
              returning(Read)};
    forwardPrivateFrameLoads(F, Architecture);
    EXPECT_EQ(F.Body[0].Kind, StmtKind::Store);
  }
}

TEST(HighFrameStoreForwarding, UnreachableOverwriteCannotDeleteAReachingStore) {
  const auto A = Arch::X64;
  auto F = roundTrip(A, NdType::makeInt(8, false));
  F.Body = {store(frameSlot(A), HighExpr::makeConst(3, 8)),
            returning(frameSlot(A)),
            store(frameSlot(A), HighExpr::makeConst(5, 8))};
  forwardPrivateFrameLoads(F, A);
  EXPECT_EQ(F.Body[0].Kind, StmtKind::Store);
}

TEST(HighFrameStoreForwarding,
     EntryPreservedRegisterSpillReadAsDataStaysDefined) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  auto SP = entryStackPointer(A)->Var;
  auto Saved = SP;
  Saved.Id = 901;
  Saved.RegOff = x86reg::RDI;
  auto Address = temporary(902, T, A)->Var;
  auto Result = temporary(903, T, A)->Var;
  const auto Op = [](NdOp Opcode, MedVar Output,
                     std::initializer_list<MedVar> Inputs) {
    MedOp O;
    O.Opcode = Opcode;
    O.Output = Output;
    for (const auto &Input : Inputs)
      O.addInput(Input);
    return O;
  };
  MedFunc Med;
  Med.Name = "saved_as_data";
  Med.Entry = 0x1000;
  Med.FrameSize = 16;
  Med.CC = CallingConv::Win64;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Med.Entry;
  Block.Ops = {Op(NdOp::COPY, SP, {SP}),
               Op(NdOp::COPY, Saved, {Saved}),
               Op(NdOp::INT_SUB, Address, {SP, MedVar::makeConst(8, 8)}),
               Op(NdOp::STORE, {}, {Address, Saved}),
               Op(NdOp::LOAD, Result, {Address}),
               Op(NdOp::RETURN, {}, {Result})};
  for (size_t I = 0; I < Block.Ops.size(); ++I)
    Block.Ops[I].Addr = Med.Entry + I;
  Med.Blocks = {Block};
  MedToHighConverter Converter;
  for (unsigned Variant = 0; Variant != 3; ++Variant) {
    if (Variant) {
      Result = Saved;
      Result.SSAVer = 1;
      Med.Blocks[0].Ops[4].Output = Result;
      Med.Blocks[0].Ops[5].Inputs[0] =
          Variant == 1 ? Result : MedVar::makeConst(0, 8);
    }
    auto High = Converter.convert(Med, A);
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = A;
    Options.Format = BinaryFormat::COFF;
    ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
    // Its unknown entry value must remain an explicit observation. Erasing
    // only the initializing store leaves an uninitialized C frame read.
    EXPECT_EQ(Source.find("unknown register") != std::string::npos,
              Variant != 2)
        << Source;
  }
}

TEST(HighBoundPrivateFrameCopies, RejectsCyclesAndBudgetExhaustionAtomically) {
  constexpr auto A = Arch::AArch64;
  const auto T = NdType::makeInt(8, false);
  auto F = roundTrip(A, T);
  auto Cyclic = integerView(parameter(0, T, A), T);
  Cyclic->Operands[0] = Cyclic;
  F.Body[0].StoreVal = Cyclic;
  EXPECT_FALSE(forwardBoundPrivateFrameCopies(F, A));
  EXPECT_TRUE(hasLoad(F.Body.back().RetVal));
  Cyclic->Operands[0] = nullptr;
  F = roundTrip(A, T);
  F.Body.insert(F.Body.begin(), 100001, HighStmt{});
  const auto Original = F.Body.back().RetVal;
  EXPECT_FALSE(forwardBoundPrivateFrameCopies(F, A));
  EXPECT_EQ(F.Body.back().RetVal, Original);
}

TEST(HighFrameStoreForwarding, CapturedWritebackCrossesAnOrdinaryBranch) {
  for (const auto A : Architectures) {
    const auto T = NdType::makeInt(8, false);
    auto F = roundTrip(A, T);
    auto Saved = temporary(1, T, A);
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = parameter(0, T, A);
    Branch.Body = {store(frameSlot(A), Saved)};
    Branch.Body[0].Addr = 0x1040;
    // The source cell is allowed to escape later; the proof concerns the
    // unchanged bits at this writeback, not whole-function frame privacy.
    F.Body = {assign(Saved, HighExpr::makeLoad(frameSlot(A), T)), Branch,
              returning(frameSlot(A))};
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[1].Body[0].Kind, StmtKind::Block);
    EXPECT_EQ(F.Body[1].Body[0].Addr, 0x1040u);
    EXPECT_TRUE(hasLoad(F.Body[0].Val));
  }
}

TEST(HighFrameStoreForwarding, JoinProofRunsOnlyWhenRequested) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (const bool AcrossJoins : {false, true}) {
    auto F = roundTrip(A, T);
    auto Saved = temporary(1, T, A);
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = parameter(0, T, A);
    Branch.Body = {store(frameSlot(A), Saved)};
    F.Body = {assign(Saved, HighExpr::makeLoad(frameSlot(A), T)), Branch,
              returning(frameSlot(A))};
    EXPECT_EQ(forwardPrivateFrameLoads(F, A, AcrossJoins), AcrossJoins);
    EXPECT_EQ(F.Body[1].Body[0].Kind,
              AcrossJoins ? StmtKind::Block : StmtKind::Store);
    // The proof removes the writeback once.
    EXPECT_EQ(forwardPrivateFrameLoads(F, A, /*AcrossJoins=*/true),
              !AcrossJoins);
    EXPECT_EQ(F.Body[1].Body[0].Kind, StmtKind::Block);
  }
}

TEST(HighFrameStoreForwarding, JoinedWritebackNeedsEveryIncomingValue) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (const bool Agree : {false, true}) {
    auto F = roundTrip(A, T);
    HighStmt Branch;
    Branch.Kind = StmtKind::IfElse;
    Branch.Cond = parameter(0, T, A);
    Branch.Body = {store(frameSlot(A), HighExpr::makeConst(3, 8))};
    Branch.ElseBody = {
        store(frameSlot(A), HighExpr::makeConst(Agree ? 3 : 5, 8))};
    F.Body = {Branch, store(frameSlot(A), HighExpr::makeConst(3, 8)),
              returning(frameSlot(A))};
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[1].Kind, Agree ? StmtKind::Block : StmtKind::Store);
  }
}

TEST(HighFrameStoreForwarding, BranchWritebackKeepsClobberedOrChangedBits) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (unsigned Variant = 0; Variant != 6; ++Variant) {
    SCOPED_TRACE(Variant);
    auto F = roundTrip(A, T);
    auto Saved = temporary(1, T, A);
    HighStmt Effect;
    Effect.Kind = StmtKind::ExprStmt;
    if (Variant == 0)
      Effect.Val = HighExpr::makeCall("modify", 0x2000, {frameSlot(A)});
    else if (Variant == 1)
      Effect = store(parameter(0, T, A), HighExpr::makeConst(5, 8));
    else if (Variant == 2)
      Effect = store(frameSlot(A, 12), HighExpr::makeConst(5, 4));
    else if (Variant == 3)
      Effect = assign(Saved, HighExpr::makeConst(5, 8));
    else if (Variant == 4) {
      Effect.Val = HighExpr::makeLoad(frameSlot(A), T);
      Effect.Val->MemoryOrdering = NdMemoryOrdering::Acquire;
    } else {
      Effect.Val = HighExpr::makeBinop(NdOp::ATOMIC_XCHG, frameSlot(A),
                                       HighExpr::makeConst(5, 8));
    }
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = parameter(0, T, A);
    Branch.Body = {Effect, store(frameSlot(A), Saved)};
    F.Body = {assign(Saved, HighExpr::makeLoad(frameSlot(A), T)), Branch,
              returning(frameSlot(A))};
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[1].Body[1].Kind, StmtKind::Store);
  }
}

TEST(HighFrameStoreForwarding, FrameCopiesKeepCapturedBitsAcrossSlotReuse) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (const bool Replace : {false, true}) {
    auto F = roundTrip(A, T);
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = parameter(0, T, A);
    if (Replace)
      Branch.Body.push_back(store(frameSlot(A, 24), HighExpr::makeConst(7, 8)));
    Branch.Body.push_back(
        store(frameSlot(A), HighExpr::makeLoad(frameSlot(A, 24), T)));
    F.Body = {store(frameSlot(A, 24), HighExpr::makeLoad(frameSlot(A), T)),
              Branch, returning(frameSlot(A))};
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[1].Body.back().Kind,
              Replace ? StmtKind::Store : StmtKind::Block);
  }
}

TEST(HighFrameStoreForwarding, BranchProofPreservesExceptionAndUnwindRegions) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    auto F = roundTrip(A, T);
    auto Saved = temporary(1, T, A);
    HighStmt Region;
    Region.Kind = Variant < 2    ? StmtKind::If
                  : Variant == 2 ? StmtKind::SEHTry
                                 : StmtKind::CxxTry;
    Region.Cond = Variant < 2 ? parameter(0, T, A) : nullptr;
    Region.Body = {store(frameSlot(A), Saved)};
    F.StructuredExceptionRegions = Variant == 0 ? 1 : 0;
    F.UnstructuredExceptionRegions = Variant == 1 ? 1 : 0;
    F.Body = {assign(Saved, HighExpr::makeLoad(frameSlot(A), T)), Region,
              returning(frameSlot(A))};
    const auto Original = F.Body[1].Body[0].StoreVal;
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[1].Kind, Region.Kind);
    EXPECT_EQ(F.Body[1].Body[0].Kind, StmtKind::Store);
    EXPECT_EQ(F.Body[1].Body[0].StoreVal, Original);
  }
}

TEST(HighFrameStoreForwarding, BranchWritebackExecutionKeepsCallEffects) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (const bool CallBeforeRestore : {false, true}) {
    auto F = roundTrip(A, T);
    F.Name = "writeback_branch";
    F.Params.push_back({"arg1", T});
    auto Saved = temporary(1, T, A);
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    Call.CallExpr = HighExpr::makeCall("modify", 0x2000, {frameSlot(A)});
    Call.CallExpr->Type = NdType::makeVoid();
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = parameter(1, T, A);
    if (CallBeforeRestore)
      Branch.Body.push_back(Call);
    Branch.Body.push_back(store(frameSlot(A), Saved));
    F.Body = {store(frameSlot(A), parameter(0, T, A)),
              assign(Saved, HighExpr::makeLoad(frameSlot(A), T)), Branch, Call,
              returning(HighExpr::makeLoad(frameSlot(A), T))};
    for (const bool Optimize : {false, true}) {
      auto Output = F;
      if (Optimize)
        forwardPrivateFrameLoads(Output, A);
      std::string Text;
      llvm::raw_string_ostream OS(Text);
      OS << "#include <stdint.h>\nstatic unsigned calls;\n"
            "int modify(uintptr_t address) { uint64_t *p = (uint64_t "
            "*)address; "
            "++calls; *p += 3; return 0; }\n";
      CEmitterOptions Options;
      Options.TheArch = A;
      Options.Format = BinaryFormat::COFF;
      ASSERT_TRUE(HighCEmitter().emit({Output}, OS, Options));
      OS << "\nint main(void) {\n"
            "  if (writeback_branch(17, 0) != 20 || calls != 1) return 1;\n"
            "  if (writeback_branch(23, 1) != 26 || calls != "
         << (CallBeforeRestore ? 3 : 2) << ") return 2;\nreturn 0; }\n";
      compileAndRun(Text);
    }
  }
}

TEST(HighFrameStoreForwarding, BranchAliasesUseTheirReachingFrameOffsets) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (const bool Same : {false, true}) {
    auto F = roundTrip(A, T);
    auto Base = temporary(3, T, A);
    auto Saved = temporary(1, T, A);
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = parameter(0, T, A);
    Branch.Body = {assign(Base, frameSlot(A, Same ? 16 : 24)),
                   store(Base, Saved)};
    F.Body = {assign(Base, frameSlot(A)),
              assign(Saved, HighExpr::makeLoad(Base, T)), Branch,
              returning(frameSlot(A))};
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[2].Body[1].Kind, Same ? StmtKind::Block : StmtKind::Store);
  }
}

TEST(HighFrameStoreForwarding, BranchProofDoesNotCommitAcrossCycles) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  auto F = roundTrip(A, T);
  auto Saved = temporary(1, T, A);
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = parameter(0, T, A);
  Branch.Body = {store(frameSlot(A), Saved)};
  HighStmt Again;
  Again.Kind = StmtKind::Goto;
  Again.GotoTarget = 0x1040;
  Branch.Addr = 0x1040;
  F.Body = {assign(Saved, HighExpr::makeLoad(frameSlot(A), T)), Branch, Again};
  forwardPrivateFrameLoads(F, A);
  EXPECT_EQ(F.Body[1].Body[0].Kind, StmtKind::Store);
  EXPECT_EQ(F.Body[2].GotoTarget, 0x1040u);
}

TEST(HighFrameStoreForwarding, OnlyLoopsThatRepeatStopTheBranchProof) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (const auto Kind : {StmtKind::While, StmtKind::DoWhile})
    for (unsigned Shape = 0; Shape != 3; ++Shape) {
      SCOPED_TRACE(testing::Message()
                   << (Kind == StmtKind::While ? "while" : "do") << Shape);
      auto F = roundTrip(A, T);
      F.Params.push_back({"arg1", T});
      auto Saved = temporary(1, T, A);
      HighStmt Branch;
      Branch.Kind = StmtKind::If;
      Branch.Cond = parameter(0, T, A);
      Branch.Body = {store(frameSlot(A), Saved)};
      // A constant false test and a body that always breaks never repeat;
      // a body that can fall back to its test does.
      HighStmt Loop;
      Loop.Kind = Kind;
      Loop.Cond = Shape == 0 ? HighExpr::makeConst(0, 4) : parameter(1, T, A);
      Loop.Body = {assign(temporary(2, T, A), HighExpr::makeConst(7, 8))};
      if (Shape == 1) {
        HighStmt Exit;
        Exit.Kind = StmtKind::Break;
        Loop.Body.push_back(Exit);
      }
      F.Body = {assign(Saved, HighExpr::makeLoad(frameSlot(A), T)), Branch,
                Loop, returning(frameSlot(A))};
      forwardPrivateFrameLoads(F, A);
      EXPECT_EQ(F.Body[1].Body[0].Kind,
                Shape == 2 ? StmtKind::Store : StmtKind::Block);
    }
}

TEST(HighFrameStoreForwarding, DoWhileTestKeepsTheReadAfterItsBody) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  auto F = roundTrip(A, T);
  auto Saved = temporary(1, T, A);
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = parameter(0, T, A);
  Branch.Body = {store(frameSlot(A), Saved)};
  // The body always leaves, so no cycle stops the proof. The test would read
  // the slot after the body's store, never the bits captured before the loop.
  HighStmt Exit;
  Exit.Kind = StmtKind::Break;
  HighStmt Once;
  Once.Kind = StmtKind::DoWhile;
  Once.Cond = HighExpr::makeLoad(frameSlot(A), T);
  Once.Body = {store(frameSlot(A), HighExpr::makeConst(7, 8)), Exit};
  F.Body = {assign(Saved, HighExpr::makeLoad(frameSlot(A), T)), Branch, Once,
            returning(frameSlot(A))};
  forwardPrivateFrameLoads(F, A);
  EXPECT_EQ(F.Body[1].Body[0].Kind, StmtKind::Block);
  EXPECT_TRUE(hasLoad(F.Body[2].Cond));
}

TEST(HighFrameStoreForwarding, BranchProofKeepsExtraStatementEffects) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  auto F = roundTrip(A, T);
  auto Saved = temporary(1, T, A);
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = parameter(0, T, A);
  Branch.Body = {store(frameSlot(A), Saved)};
  Branch.Body[0].Val = HighExpr::makeConst(1, 8);
  F.Body = {assign(Saved, HighExpr::makeLoad(frameSlot(A), T)), Branch,
            returning(frameSlot(A))};
  forwardPrivateFrameLoads(F, A);
  EXPECT_EQ(F.Body[1].Body[0].Kind, StmtKind::Store);
  EXPECT_NE(F.Body[1].Body[0].Val, nullptr);
}
ExprPtr wordDifference(ExprPtr Left, ExprPtr Right, unsigned Form) {
  const auto T = Left->Type;
  if (Form < 2)
    return HighExpr::makeBinop(Form == 0 ? NdOp::INT_SUB : NdOp::INT_XOR, Left,
                               Right);
  auto Negative = [&](ExprPtr V) {
    if (Form == 4)
      return HighExpr::makeUnary(NdOp::INT_NEG2, V);
    auto Factor = HighExpr::makeConst(~uint64_t(0), T->Size,
                                      ConstantAddressProvenance::Scalar);
    Factor->Type = T;
    return HighExpr::makeBinop(NdOp::INT_MULT, V, Factor);
  };
  return Form == 3 ? HighExpr::makeBinop(NdOp::INT_ADD, Left, Negative(Right))
                   : HighExpr::makeBinop(NdOp::INT_ADD, Negative(Left), Right);
}

TEST(HighSourceExpressions, WordZeroTestsBecomeEqualWidthComparisons) {
  for (uint16_t Bytes : {1, 2, 4, 8})
    for (unsigned Form = 0; Form != 5; ++Form)
      for (bool Reverse : {false, true})
        for (const auto Op : {NdOp::INT_EQUAL, NdOp::INT_NOTEQUAL}) {
          const auto T = NdType::makeInt(Bytes, true);
          auto L = parameter(0, T, Arch::X64);
          auto R = parameter(1, NdType::makeInt(Bytes, false), Arch::X64);
          auto Difference = wordDifference(L, R, Form);
          auto View = integerView(Difference, NdType::makeInt(Bytes, false));
          auto Zero = HighExpr::makeConst(0, Bytes);
          std::vector<HighStmt> Body{returning(nullptr)};
          Body[0].RetVal = HighExpr::makeBinop(Op, Reverse ? Zero : View,
                                               Reverse ? View : Zero);
          ASSERT_TRUE(simplifyAllExprs(Body));
          const auto &Result = Body[0].RetVal;
          ASSERT_EQ(Result->Kind, ExprKind::BinOp);
          ASSERT_EQ(Result->Op, Op);
          for (const auto &Operand : Result->Operands) {
            ASSERT_TRUE(Operand->Type);
            EXPECT_EQ(Operand->Type->Size, Bytes);
            if (Bytes < 4)
              EXPECT_FALSE(Operand->Type->IsSigned);
            EXPECT_TRUE(Operand->Kind == ExprKind::Var ||
                        Operand->Kind == ExprKind::Cast);
          }
          // Another use still observes the original arithmetic word.
          EXPECT_EQ(Difference->Kind, ExprKind::BinOp);
        }
}

TEST(HighSourceExpressions, WordZeroTestsPreserveWidthsAndEffects) {
  const auto T = NdType::makeInt(8, false);
  for (unsigned Case = 0; Case != 10; ++Case) {
    SCOPED_TRACE(Case);
    auto L = parameter(0, T, Arch::X64);
    auto R = parameter(1, T, Arch::X64);
    auto Difference = wordDifference(L, R, 2);
    ExprPtr Input = Difference;
    auto Zero = HighExpr::makeConst(0, 8);
    if (Case == 0)
      Input = integerView(Difference, NdType::makeInt(4, false));
    else if (Case == 1)
      Input = HighExpr::makeBitCast(Difference, NdType::makeFloat(8));
    else if (Case == 2)
      Difference->Operands.push_back(HighExpr::makeConst(7, 8));
    else if (Case == 3)
      Difference->Operands[0]->Operands[1]->ConstProvenance =
          ConstantAddressProvenance::CodeAddress;
    else if (Case == 4)
      Difference->Operands[0]->Operands[1]->ConstVal = 3;
    else if (Case == 5)
      Zero = HighExpr::makeConst(0, 4);
    else if (Case == 6) {
      auto Load = HighExpr::makeLoad(L, T);
      Load->MemoryOrdering = NdMemoryOrdering::Acquire;
      Difference->Operands[0]->Operands[0] = Load;
    } else if (Case == 7)
      Difference->IntrinsicOutputs = {temporary(9, T, Arch::X64)->Var};
    else if (Case == 8)
      Input = integerView(Difference, NdType::makeInt(16, false));
    else
      Difference->Operands[0] = HighExpr::makeUnary(NdOp::INT_NEGATE, L);
    std::vector<HighStmt> Body{
        returning(HighExpr::makeBinop(NdOp::INT_EQUAL, Input, Zero))};
    simplifyAllExprs(Body);
    ASSERT_EQ(Body[0].RetVal->Operands.size(), 2u);
    // No unproved comparison of the two original parameters.
    EXPECT_EQ(Body[0].RetVal->Operands[1], Zero);
  }
}

TEST(HighSourceExpressions, WordZeroTestsExecuteAtIntegerBoundaries) {
  std::string Source;
  std::vector<std::string> Checks;
  for (uint16_t Bytes : {1, 2, 4, 8})
    for (unsigned Form = 0; Form != 5; ++Form)
      for (bool Equal : {false, true}) {
        const auto T = NdType::makeInt(Bytes, true);
        const auto U = NdType::makeInt(Bytes, false);
        HighFunc F;
        const auto Suffix = std::to_string(Bytes) + "_" + std::to_string(Form) +
                            (Equal ? "_eq" : "_ne");
        F.Params = {{"arg0", T}, {"arg1", U}};
        F.ReturnType = NdType::makeInt(1, false);
        auto Difference = wordDifference(parameter(0, T, Arch::X64),
                                         parameter(1, U, Arch::X64), Form);
        F.Body = {returning(HighExpr::makeBinop(
            Equal ? NdOp::INT_EQUAL : NdOp::INT_NOTEQUAL,
            HighExpr::makeConst(0, Bytes), integerView(Difference, U)))};
        CEmitterOptions Options;
        Options.TheArch = Arch::X64;
        for (bool Simplified : {false, true}) {
          F.Name = std::string(Simplified ? "after_" : "before_") + Suffix;
          if (Simplified)
            ASSERT_TRUE(simplifyAllExprs(F.Body));
          llvm::raw_string_ostream Out(Source);
          ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
        }
        const auto Width = std::to_string(Bytes * 8);
        const auto Arguments = "((int" + Width + "_t)x,(uint" + Width + "_t)y)";
        Checks.push_back("if (before_" + Suffix + Arguments + " != after_" +
                         Suffix + Arguments + " || after_" + Suffix +
                         Arguments + " != ((uint" + Width + "_t)x " +
                         (Equal ? "==" : "!=") + " (uint" + Width +
                         "_t)y)) return 1;\n");
      }
  Source += R"(
static int check(uint64_t x, uint64_t y) {
)";
  for (const auto &Check : Checks)
    Source += Check;
  Source += R"(
  return 0;
}
int main(void) {
  uint64_t values[]={0,1,127,128,255,256,32767,32768,65535,65536,
    UINT64_C(0x7fffffff),UINT64_C(0x80000000),UINT64_C(0xffffffff),
    UINT64_C(0x100000000),UINT64_C(0x7fffffffffffffff),
    UINT64_C(0x8000000000000000),UINT64_MAX};
  for (unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i)
    for (unsigned j=0;j<sizeof(values)/sizeof(values[0]);++j)
      if (check(values[i],values[j])) return 2;
  for (uint64_t i=0;i<256;++i)
    for (uint64_t j=0;j<256;++j)
      if (check(i,j)) return 3;
  return 0;
}
)";
  compileAndRun(Source);
}

TEST(HighSourceExpressions, ZeroTestsKeepTruncationButDropOuterExtensions) {
  const auto T = NdType::makeInt(8, false);
  const auto Narrow = NdType::makeInt(4, true);
  for (auto Op : {NdOp::INT_ZEXT, NdOp::INT_SEXT}) {
    HighFunc F;
    F.Name = "low_word_zero";
    F.Params = {{"arg0", T}};
    F.ReturnType = NdType::makeInt(1, false);
    auto Low = integerView(parameter(0, T, Arch::X64), Narrow);
    auto Extended = HighExpr::makeUnary(Op, Low);
    Extended->Type = T;
    F.Body = {returning(HighExpr::makeBinop(NdOp::INT_EQUAL, Extended,
                                            HighExpr::makeConst(0, 8)))};
    ASSERT_TRUE(simplifyAllExprs(F.Body));
    EXPECT_EQ(F.Body[0].RetVal->Operands[0], Low);
    EXPECT_EQ(F.Body[0].RetVal->Operands[1]->Type->Size, 4u);
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
    Source += R"(
int main(void) {
  return !low_word_zero(0) || !low_word_zero(UINT64_C(0x100000000)) ||
    low_word_zero(UINT64_C(0x80000000)) || low_word_zero(UINT64_MAX);
}
)";
    compileAndRun(Source);
  }
}

TEST(HighSourceExpressions, DifferenceComparisonsKeepEveryCallEvaluation) {
  std::string Source;
  HighFunc F;
  F.ReturnType = NdType::makeInt(1, false);
  auto Call = HighExpr::makeCall("observe_word", 0x2000, {});
  Call->Type = NdType::makeInt(8, false);
  F.Body = {returning(HighExpr::makeBinop(NdOp::INT_NOTEQUAL,
                                          wordDifference(Call, Call, 2),
                                          HighExpr::makeConst(0, 8)))};
  for (bool Simplified : {false, true}) {
    F.Name = Simplified ? "after_calls" : "before_calls";
    if (Simplified)
      ASSERT_TRUE(simplifyAllExprs(F.Body));
    llvm::raw_string_ostream Out(Source);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
  }
  Source += R"(
static unsigned calls;
int64_t observe_word(void) { ++calls; return 6; }
int main(void) {
  if (before_calls() || calls!=2) return 1;
  if (after_calls() || calls!=4) return 2;
  return 0;
}
)";
  compileAndRun(Source);
}

TEST(HighFrameStoreForwarding, CapturedCallResultReloadsAcrossABranch) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (bool Clobber : {false, true}) {
    auto F = roundTrip(A, T);
    auto Captured = temporary(20, T, A);
    auto Call = HighExpr::makeCall("produce", 0x2000, {});
    Call->Type = T;
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = parameter(0, T, A);
    if (Clobber)
      Branch.Body = {store(frameSlot(A), HighExpr::makeConst(77, 8))};
    F.Body = {assign(Captured, Call), store(frameSlot(A), Captured), Branch,
              returning(HighExpr::makeLoad(frameSlot(A), T))};
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(hasLoad(F.Body.back().RetVal), Clobber);
    EXPECT_EQ(F.Body[0].Val, Call);
  }
}

TEST(HighFrameStoreForwarding, CapturedReloadDoesNotReplayAnExternalRead) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  HighFunc F;
  F.Name = "captured_load";
  F.FrameSize = 64;
  F.ReturnType = T;
  F.Params = {{"arg0", NdType::makePtr(T)}, {"arg1", T}};
  auto Captured = temporary(20, T, A);
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = parameter(1, T, A);
  Branch.Body = {
      store(parameter(0, NdType::makePtr(T), A), HighExpr::makeConst(77, 8))};
  F.Body = {assign(Captured,
                   HighExpr::makeLoad(parameter(0, NdType::makePtr(T), A), T)),
            store(frameSlot(A), Captured), Branch,
            returning(HighExpr::makeLoad(frameSlot(A), T))};
  // A possible aliasing external write invalidates the frame proof. Either
  // way the initial external read must remain a captured observation.
  forwardPrivateFrameLoads(F, A);
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = A;
  ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
  Source += R"(
int main(void) {
  uint64_t x=11;
  if (captured_load(&x,1)!=11 || x!=77) return 1;
  x=19;
  return captured_load(&x,0)!=19 || x!=19;
}
)";
  compileAndRun(Source);
}

TEST(HighFrameStoreForwarding, OverwritesMustCoverEverySourcePath) {
  for (const auto A : Architectures) {
    const auto T = NdType::makeInt(4, false);
    for (unsigned Variant = 0; Variant != 5; ++Variant) {
      SCOPED_TRACE(static_cast<int>(A));
      SCOPED_TRACE(Variant);
      auto F = roundTrip(A, T);
      HighStmt Branch;
      Branch.Kind = Variant == 1 ? StmtKind::If : StmtKind::IfElse;
      Branch.Cond = parameter(0, T, A);
      Branch.Body = {store(frameSlot(A), HighExpr::makeConst(11, 4))};
      if (Variant != 1)
        Branch.ElseBody = {store(frameSlot(A), HighExpr::makeConst(12, 4))};
      if (Variant == 2) {
        HighStmt Call;
        Call.Kind = StmtKind::Call;
        Call.CallExpr = HighExpr::makeCall("observe", 0x2000, {frameSlot(A)});
        Branch.Body.insert(Branch.Body.begin(), Call);
      }
      if (Variant == 3)
        Branch.Body[0].StoreVal = HighExpr::makeConst(11, 2);
      if (Variant == 4)
        Branch.Body[0].MemoryOrdering =
            NdMemoryOrdering::SequentiallyConsistent;
      F.Body.insert(F.Body.begin() + 1, Branch);
      ASSERT_TRUE(buildHighSourceFlowGraph(F).Diagnostics.Complete);
      forwardPrivateFrameLoads(F, A);
      EXPECT_EQ(F.Body[0].Kind == StmtKind::Block, Variant == 0);
    }
  }
}

TEST(HighFrameStoreForwarding, DeadUnknownSpillsDisappearOnlyBeforeOverwrite) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  auto F = roundTrip(A, T);
  F.Name = "overwritten_spill";
  auto Unknown = temporary(22, T, A);
  Unknown->Var.Kind = MedVar::Reg;
  Unknown->Var.RegOff = x86reg::RDI;
  F.Body[0].StoreVal = Unknown;
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = parameter(0, T, A);
  Branch.Body = {store(frameSlot(A), HighExpr::makeConst(11, 8))};
  Branch.ElseBody = {store(frameSlot(A), HighExpr::makeConst(12, 8))};
  F.Body.insert(F.Body.begin() + 1, Branch);
  ASSERT_TRUE(buildHighSourceFlowGraph(F).Diagnostics.Complete);
  forwardPrivateFrameLoads(F, A);
  ASSERT_EQ(F.Body[0].Kind, StmtKind::Block);
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = A;
  ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
  EXPECT_EQ(Source.find("unknown register"), std::string::npos) << Source;
  Source += R"(
int main(void) {
  return overwritten_spill(0)!=12 || overwritten_spill(1)!=11;
}
)";
  compileAndRun(Source);
}

TEST(HighFrameStoreForwarding, RemovedWritebacksCannotKillTheirProducer) {
  for (const auto A : Architectures) {
    const auto T = NdType::makeInt(4, false);
    auto F = roundTrip(A, T);
    HighStmt Branch;
    Branch.Kind = StmtKind::IfElse;
    Branch.Cond = parameter(0, T, A);
    Branch.Body = {store(frameSlot(A), parameter(0, T, A))};
    Branch.ElseBody = Branch.Body;
    auto Observe = HighExpr::makeCall("observe", 0x2000, {frameSlot(A)});
    Observe->Type = T;
    F.Body = {store(frameSlot(A), parameter(0, T, A)), Branch,
              returning(Observe)};
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[0].Kind, StmtKind::Store);
    EXPECT_EQ(F.Body[1].Body[0].Kind, StmtKind::Block);
    EXPECT_EQ(F.Body[1].ElseBody[0].Kind, StmtKind::Block);
  }
}

TEST(HighFrameStoreForwarding, PointerBitViewsKeepCapturedParameterValues) {
  const auto A = Arch::X64;
  const auto Integer = NdType::makeInt(8, false);
  const auto Pointer = NdType::makePtr(Integer);
  for (bool Reassigned : {false, true}) {
    auto F = roundTrip(A, Integer);
    F.Name = "captured_parameter";
    F.FrameSize = 0;
    F.Params = {{"arg0", Pointer}, {"arg1", Pointer}};
    auto Alias = temporary(20, Integer, A);
    auto View = std::make_shared<HighExpr>();
    View->Kind = ExprKind::Cast;
    View->Type = View->CastTo = Integer;
    View->Operands = {parameter(0, Pointer, A)};
    F.Body = {assign(Alias, View)};
    if (Reassigned)
      F.Body.push_back(
          assign(parameter(0, Pointer, A), parameter(1, Pointer, A)));
    F.Body.push_back(store(Alias, HighExpr::makeConst(42, 8)));
    F.Body.push_back(returning(HighExpr::makeConst(0, 8)));
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    CEmitterOptions Options;
    Options.TheArch = A;
    ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
    Source += R"(
int main(void) {
  uint64_t first=11,second=23;
  captured_parameter(&first,&second);
  return first!=42 || second!=23;
})";
    compileAndRun(Source);
  }
}
TEST(HighSourceExpressions, PointerBitViewsKeepTheCapturedLocal) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  const auto P = NdType::makePtr(T);
  for (bool Reassigned : {false, true}) {
    HighFunc F;
    F.Name = "pointer_view";
    F.ReturnType = T;
    F.Params = {{"arg0", P}, {"arg1", P}};
    auto Captured = temporary(20, P, A);
    auto View = integerView(parameter(0, P, A), T);
    View = integerView(View, NdType::makeInt(8, true));
    F.Body = {assign(Captured, View)};
    if (Reassigned)
      F.Body.push_back(assign(parameter(0, P, A), parameter(1, P, A)));
    F.Body.push_back(store(Captured, HighExpr::makeConst(42, 8)));
    F.Body.push_back(returning(HighExpr::makeConst(0, 8)));
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    CEmitterOptions Options;
    Options.TheArch = A;
    ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
    if (Reassigned)
      EXPECT_NE(Source.find(" = arg0;"), std::string::npos) << Source;
    EXPECT_EQ(Source.find("(uintptr_t)((int64_t)"), std::string::npos)
        << Source;
    Source += R"(
int main(void) {
  uint64_t first=11,second=23;
  pointer_view(&first,&second);
  return first!=42 || second!=23;
}
)";
    compileAndRun(Source);
  }
}
TEST(HighSourceExpressions,
     PointerBitViewsKeepNarrowingAndExplicitTypeChanges) {
  const auto T = NdType::makeInt(8, false);
  const auto P = NdType::makePtr(T);
  for (unsigned Case = 0; Case != 3; ++Case) {
    HighFunc F;
    F.Name = "pointer_view_boundary";
    F.ReturnType = T;
    F.Params = {{"arg0", P}};
    const auto Intermediate = Case == 0   ? NdType::makeInt(4, false)
                              : Case == 1 ? NdType::makeFloat(8)
                                          : NdType::makePtr(NdType::makeInt(1));
    auto View = integerView(parameter(0, P, Arch::X64), Intermediate);
    auto Captured = temporary(20, P, Arch::X64);
    F.Body = {assign(Captured, integerView(View, T)),
              returning(integerView(Captured, T))};
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
    EXPECT_EQ(Source.find(" = arg0;"), std::string::npos) << Source;
  }
}
HighFunc guardedEncodedAddress(bool Equal = false) {
  const auto T = NdType::makeInt(8, false);
  const auto U32 = NdType::makeInt(4, false);
  const auto Literal = [](uint64_t V) {
    return HighExpr::makeConst(V, 8, ConstantAddressProvenance::Scalar);
  };
  auto Q = parameter(0, T, Arch::X64);
  auto Bits = HighExpr::makeBinop(NdOp::INT_XOR, Q, Literal(0x27));
  Bits->Type = T;
  auto Address = HighExpr::makeBinop(NdOp::INT_ADD, Bits,
                                     Literal(UINT64_C(0xffffffff00000000)));
  Address->Type = T;
  HighStmt Guard;
  Guard.Kind = Equal ? StmtKind::IfElse : StmtKind::If;
  Guard.Cond = HighExpr::makeBinop(Equal ? NdOp::INT_EQUAL : NdOp::INT_NOTEQUAL,
                                   Q, Literal(UINT64_C(0x100000008) ^ 0x27));
  const auto Read = returning(HighExpr::makeLoad(Address, U32));
  if (Equal) {
    Guard.Body = {returning(HighExpr::makeConst(0, 4))};
    Guard.ElseBody = {Read};
  } else {
    Guard.Body = {Read};
  }
  HighFunc F;
  F.Name = "guarded_address";
  F.ReturnType = U32;
  F.Params = {{"arg0", T}};
  F.Body = {Guard, returning(HighExpr::makeConst(0, 4))};
  return F;
}

TEST(HighSourceExpressions, GuardedAddressKeepsTheDecodedBaseAndFieldOffset) {
  std::string Text;
  for (bool Equal : {false, true}) {
    auto F = guardedEncodedAddress(Equal);
    for (bool Named : {false, true}) {
      F.Name = std::string(Named ? "after_address" : "before_address") +
               (Equal ? "_eq" : "_ne");
      if (Named) {
        ASSERT_TRUE(nameRepeatedValues(F));
        ASSERT_EQ(F.Body.front().Kind, StmtKind::Assign);
        EXPECT_TRUE(F.Body.front().KeepsName);
        const auto &Guard = F.Body[1];
        const auto &Arm = Equal ? Guard.ElseBody : Guard.Body;
        const auto &Address = Arm.front().RetVal->Operands[0];
        ASSERT_EQ(Address->Kind, ExprKind::BinOp);
        ASSERT_EQ(Address->Operands.size(), 2u);
        EXPECT_EQ(Address->Operands[1]->ConstVal, 8u);
      }
      llvm::raw_string_ostream Out(Text);
      CEmitterOptions Options;
      Options.TheArch = Arch::X64;
      ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
    }
  }
  Text += R"(
int main(void) {
  uint32_t words[]={0,UINT32_MAX,0x80000000u,0x12345678u,7,255,1,42};
  for (unsigned i=0;i<6;++i) {
    uint64_t q=((uint64_t)(uintptr_t)&words[i]+UINT64_C(0x100000008))^0x27;
    if (before_address_ne(q)!=words[i+2] || after_address_ne(q)!=words[i+2] ||
        before_address_eq(q)!=words[i+2] || after_address_eq(q)!=words[i+2])
      return 1;
  }
  uint64_t null_q=UINT64_C(0x100000008)^0x27;
  return before_address_ne(null_q) || after_address_ne(null_q) ||
         before_address_eq(null_q) || after_address_eq(null_q);
}
)";
  compileAndRun(Text);
}

TEST(HighSourceExpressions, GuardedAddressNeedsStableCompleteScalarBits) {
  const auto T = NdType::makeInt(8, false);
  for (unsigned Variant = 0; Variant != 6; ++Variant) {
    SCOPED_TRACE(Variant);
    auto F = guardedEncodedAddress();
    auto &Guard = F.Body.front();
    auto &Address = Guard.Body.front().RetVal->Operands[0];
    if (Variant == 0) {
      Guard.Body.insert(Guard.Body.begin(), assign(parameter(0, T, Arch::X64),
                                                   HighExpr::makeConst(7, 8)));
    } else if (Variant == 1) {
      Address->Operands[0]->Operands[1]->ConstProvenance =
          ConstantAddressProvenance::CodeAddress;
    } else if (Variant == 2) {
      auto Low = integerView(Address->Operands[0], NdType::makeInt(4, false));
      auto Extended = HighExpr::makeUnary(NdOp::INT_ZEXT, Low);
      Extended->Type = T;
      Address->Operands[0] = Extended;
    } else if (Variant == 3) {
      Address->Operands[1] =
          HighExpr::makeConst(8, 8, ConstantAddressProvenance::Scalar);
    } else if (Variant == 4) {
      F.Params.push_back({"arg1", T});
      Address->Operands[0]->Operands[0] = parameter(1, T, Arch::X64);
    } else {
      HighStmt Jump;
      Jump.Kind = StmtKind::Goto;
      Jump.GotoTarget = Guard.Body.front().Addr = 0x1040;
      F.Body.insert(F.Body.begin(), Jump);
    }
    EXPECT_FALSE(nameRepeatedValues(F));
  }
}
TEST(HighSourceExpressions, CallbackDeclaratorsBorrowIncompleteRecords) {
  const auto Record = NdType::makeNamedRecord("Context", 24);
  const auto Pointer = NdType::makePtr(Record);
  const auto Callback =
      NdType::makePtr(NdType::makeFunc(NdType::makeVoid(), {Pointer}));
  EXPECT_EQ(declarationToC(Callback, "visit"), "void (*visit)(Context*)");
  EXPECT_THROW((void)declarationToC(NdType::makePtr(NdType::makeFunc(
                                        NdType::makeVoid(), {Record})),
                                    "visit"),
               std::invalid_argument);
  compileAndRun("typedef struct Context Context;\n" +
                declarationToC(Callback, "visit") +
                ";\nint main(void) { return visit!=0; }\n");
}
HighFunc duplicatedContinuation() {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  const auto P = NdType::makePtr(T);
  HighFunc F;
  F.Name = "shared_continuation";
  F.ReturnType = T;
  F.Params = {{"arg0", P}, {"arg1", T}};
  auto Tail = [&](unsigned Id) {
    auto Word = temporary(Id, T, A);
    auto Result = temporary(Id + 1, T, A);
    auto Finish = HighExpr::makeCall("finish", 0x3000, {Word});
    Finish->Type = T;
    Finish->CallSiteAddr = Id;
    HighStmt Guard;
    Guard.Kind = StmtKind::If;
    Guard.Cond = Word;
    Guard.Body = {assign(Result, Finish), returning(Result)};
    return std::vector<HighStmt>{
        assign(Word, HighExpr::makeLoad(parameter(0, P, A), T)), Guard,
        returning(HighExpr::makeConst(0, 8))};
  };
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Addr = 0x1000;
  Branch.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, parameter(1, T, A),
                                    HighExpr::makeConst(1, 8));
  Branch.Body = Tail(20);
  auto Prepared = temporary(10, T, A);
  auto Prepare = HighExpr::makeCall("prepare", 0x2000, {parameter(0, P, A)});
  Prepare->Type = T;
  Prepare->CallSiteAddr = 0x1004;
  HighStmt Failed;
  Failed.Kind = StmtKind::If;
  Failed.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Prepared);
  Failed.Body = {returning(HighExpr::makeConst(0, 8))};
  F.Body = {Branch, assign(Prepared, Prepare), Failed};
  auto Last = Tail(30);
  F.Body.insert(F.Body.end(), Last.begin(), Last.end());
  return F;
}

TEST(HighSourceExpressions, CommonTerminalTailReadsAfterTheOptionalCall) {
  auto F = duplicatedContinuation();
  std::string Source;
  for (bool Folded : {false, true}) {
    F.Name = Folded ? "after_tail" : "before_tail";
    if (Folded) {
      ASSERT_TRUE(foldDuplicateTerminalContinuations(F));
      ASSERT_EQ(F.Body.size(), 4u);
      ASSERT_EQ(F.Body.front().Body.size(), 2u);
      const auto &Prepare = F.Body.front().Body.front().Val;
      ASSERT_EQ(Prepare->Kind, ExprKind::Call);
      EXPECT_EQ(Prepare->CallSiteAddr, 0x1004u);
      EXPECT_FALSE(foldDuplicateTerminalContinuations(F));
    }
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    llvm::raw_string_ostream Out(Source);
    ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
  }
  Source += R"(
static unsigned prepares, finishes, gate;
static uint64_t replacement;
int64_t prepare(uintptr_t address) {
  ++prepares; *(uint64_t *)address=replacement; return gate;
}
int64_t finish(uint64_t word) { ++finishes; return (int64_t)(word+3); }
int main(void) {
  uint64_t seeds[]={0,1,UINT64_MAX,UINT64_C(0x100000000)};
  for (unsigned reason=0;reason<4;++reason)
    for (gate=0;gate<2;++gate)
      for (unsigned i=0;i<4;++i)
        for (unsigned j=0;j<4;++j) {
          uint64_t word=seeds[i]; replacement=seeds[j];
          prepares=finishes=0;
          uint64_t expected=before_tail(&word,reason);
          unsigned p=prepares, f=finishes;
          word=seeds[i]; prepares=finishes=0;
          if (after_tail(&word,reason)!=expected || prepares!=p || finishes!=f)
            return 1;
          if (prepares!=(reason==1) || word!=(reason==1?replacement:seeds[i]))
            return 2;
        }
  return 0;
}
)";
  compileAndRun(Source);
}

TEST(HighSourceExpressions, CommonTerminalTailRequiresExactEffectsAndNames) {
  for (unsigned Variant = 0; Variant != 8; ++Variant) {
    SCOPED_TRACE(Variant);
    auto F = duplicatedContinuation();
    auto &Arm = F.Body.front().Body;
    if (Variant == 0)
      Arm[0].Val->MemoryOrdering = NdMemoryOrdering::Acquire;
    else if (Variant == 1)
      Arm[1].Body[0].Val->CallAddr = 0x4000;
    else if (Variant == 2)
      Arm[1].Body[0].Val->Operands[0] = HighExpr::makeConst(7, 8);
    else if (Variant == 3)
      F.Body.insert(F.Body.begin(),
                    store(HighExpr::makeConst(0x9000, 8), Arm[0].Dst));
    else if (Variant == 4) {
      HighStmt Jump;
      Jump.Kind = StmtKind::Goto;
      Jump.GotoTarget = Arm[0].Addr = 0x1040;
      F.Body.insert(F.Body.begin(), Jump);
    } else if (Variant == 5)
      Arm[1].Body[0].Val->Kind = ExprKind::Undef;
    else if (Variant == 6) {
      Arm[0].Dst->Var.Kind = MedVar::Reg;
    } else {
      HighStmt Try;
      Try.Kind = StmtKind::SEHTry;
      Try.Body = std::move(Arm);
      Arm = {Try};
    }
    foldDuplicateTerminalContinuations(F);
    // A separate common `return 0` may still fold. This differing outer
    // continuation must retain its original condition and arm.
    bool Preserved = false;
    walkStmts(F.Body, [&](const HighStmt &S) {
      if (S.Addr == 0x1000 && S.Cond)
        Preserved = S.Cond->Op == NdOp::INT_NOTEQUAL;
    });
    EXPECT_TRUE(Preserved);
  }
}

TEST(HighSourceExpressions, CommonTerminalTailMatchesRuntimeIndirectTargets) {
  for (bool DifferentTarget : {false, true}) {
    auto F = duplicatedContinuation();
    const auto T = NdType::makeInt(8, false);
    F.Params.push_back({"arg2", T});
    const auto A = F.Body.front().Body[1].Body[0].Val;
    const auto B = F.Body[F.Body.size() - 2].Body[0].Val;
    for (const auto &Call : {A, B}) {
      Call->CallTarget = "indirect";
      Call->IsIndirectCall = true;
      Call->CallAddr = Call->CallSiteAddr;
      Call->IndirectTarget = parameter(2, T, Arch::X64);
    }
    if (DifferentTarget)
      B->IndirectTarget = parameter(1, T, Arch::X64);
    foldDuplicateTerminalContinuations(F);
    EXPECT_EQ(F.Body.front().Cond->Op,
              DifferentTarget ? NdOp::INT_NOTEQUAL : NdOp::INT_EQUAL);
  }
}

TEST(HighSourceExpressions, CommonTerminalTailKeepsExceptionEntries) {
  // The unwinder enters a catch continuation, landing pad or dispatcher entry
  // without a goto. Folding the arm that holds one would delete that entry.
  constexpr va_t Entry = 0x1040;
  for (unsigned Variant = 0; Variant != 3; ++Variant) {
    SCOPED_TRACE(Variant);
    auto F = duplicatedContinuation();
    F.Body.front().Body.front().Addr = Entry;
    MedFunc Med;
    if (Variant == 0) {
      HighEHClause Catch;
      Catch.Kind = HighEHClauseKind::CxxCatch;
      Catch.HandlerVA = 0x5000;
      Catch.ContinuationVAs = {Entry};
      HighStmt Try;
      Try.Kind = StmtKind::CxxTry;
      Try.EHIsReducible = false;
      Try.EHClauses = {Catch};
      Try.EHClauseBodies = {{}};
      F.Body.insert(F.Body.begin(), Try);
    } else if (Variant == 1) {
      F.ExceptionMetadata = ExceptionFunction{};
      F.ExceptionMetadata->PersonalityVA = 0x9000;
    } else {
      MedBlock Pad;
      Pad.StartAddr = Entry;
      Pad.ExceptionalPreds.emplace_back();
      Med.Blocks.push_back(std::move(Pad));
    }
    EXPECT_FALSE(foldDuplicateTerminalContinuations(F, &Med));
    bool Kept = false;
    walkStmts(F.Body, [&](const HighStmt &S) { Kept |= S.Addr == Entry; });
    EXPECT_TRUE(Kept);
  }
  auto Plain = duplicatedContinuation();
  MedFunc Med;
  EXPECT_TRUE(foldDuplicateTerminalContinuations(Plain, &Med));
}

TEST(HighSourceExpressions, TypedIndirectCallUsesTheCapturedRuntimeTarget) {
  class CalleeTypes : public NullDebugContext {
  public:
    bool hasInfo() const override { return true; }
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      if (Address != 0x2000)
        return std::nullopt;
      FunctionSym F;
      F.Name = "signature_only";
      F.Addr = Address;
      F.ReturnType = NdType::makeInt(4, true);
      F.Params = {{"value", NdType::makeInt(8, false)}};
      return F;
    }
  } Debug;
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  const auto Result = NdType::makeInt(4, true);
  HighFunc F;
  F.Name = "captured_callback";
  F.ReturnType = Result;
  F.Params = {{"arg0", T}, {"arg1", T}};
  auto Captured = temporary(20, T, A);
  auto Call = HighExpr::makeCall("indirect", 0x2000, {parameter(1, T, A)});
  Call->Type = Result;
  Call->IsIndirectCall = true;
  Call->IndirectTarget = integerView(Captured, NdType::makeInt(8, true));
  F.Body = {assign(Captured, parameter(0, T, A)),
            assign(parameter(0, T, A), HighExpr::makeConst(0, 8)),
            returning(Call)};
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = A;
  Options.Format = BinaryFormat::COFF;
  ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options, &Debug));
  EXPECT_NE(Source.find("int32_t (*t20)(uint64_t)"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("t20(arg1)"), std::string::npos) << Source;
  Source += R"(
static int32_t actual(uint64_t x) { return (int32_t)x+7; }
int32_t signature_only(uint64_t x) { return -99; }
int main(void) {
  return captured_callback((uint64_t)(uintptr_t)&actual,5)!=12;
}
)";
  compileAndRun(Source);
}
TEST(HighSourceExpressions, UnspellableCallbackPrototypeKeepsUntypedCall) {
  // declarationToC spells no callback taking a pointer to an enumeration.
  // The captured import's call keeps its unprototyped callee instead of
  // failing the whole rendering.
  class CalleeTypes : public NullDebugContext {
  public:
    bool hasInfo() const override { return true; }
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      if (Address != 0x2000)
        return std::nullopt;
      FunctionSym F;
      F.Name = "signature_only";
      F.Addr = Address;
      F.ReturnType = NdType::makeInt(4, true);
      F.Params = {{"value", NdType::makePtr(NdType::makeNamedRecord(
                                "STATE", 4, /*Enum=*/true))}};
      return F;
    }
  } Debug;
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  const auto Result = NdType::makeInt(4, true);
  HighFunc F;
  F.Name = "captured_callback";
  F.ReturnType = Result;
  F.Params = {{"arg0", T}, {"arg1", T}};
  auto Captured = temporary(20, T, A);
  auto Call = HighExpr::makeCall("indirect", 0x2000, {parameter(1, T, A)});
  Call->Type = Result;
  Call->IsIndirectCall = true;
  Call->IndirectTarget = integerView(Captured, NdType::makeInt(8, true));
  F.Body = {assign(Captured, parameter(0, T, A)),
            assign(parameter(0, T, A), HighExpr::makeConst(0, 8)),
            returning(Call)};
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = A;
  Options.Format = BinaryFormat::COFF;
  bool Emitted = false;
  ASSERT_NO_THROW(Emitted = HighCEmitter().emit({F}, Out, Options, &Debug));
  ASSERT_TRUE(Emitted);
  EXPECT_EQ(Source.find("(*t20)"), std::string::npos) << Source;
  EXPECT_NE(Source.find("(int32_t (*)())"), std::string::npos) << Source;
}
TEST(HighSourceExpressions, PredicateCleanupKeepsDistinctIndirectCallEffects) {
  const auto T = NdType::makeInt(8, false);
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    SCOPED_TRACE(Variant);
    HighFunc F;
    F.Entry = 0x1000;
    F.Name = "kept_predicate_calls";
    F.ReturnType = T;
    F.Params = {{"arg0", T}, {"arg1", T}, {"arg2", T}};
    auto First =
        HighExpr::makeCall("indirect", 0x2000, {parameter(0, T, Arch::X64)});
    First->Type = T;
    First->CallSiteAddr = 0x1000;
    First->IsIndirectCall = true;
    First->IndirectTarget = parameter(1, T, Arch::X64);
    auto Next = std::make_shared<HighExpr>(*First);
    Next->CallSiteAddr = 0x1010;
    Next->CallAddr = Variant == 0 ? 0x3000 : First->CallAddr;
    Next->IndirectTarget = parameter(2, T, Arch::X64);
    Next->Operands = {HighExpr::makeBinop(
        NdOp::INT_ADD, parameter(0, T, Arch::X64), HighExpr::makeConst(1, 8))};
    if (Variant == 2)
      Next->CallSiteAddr = First->CallSiteAddr = 0;
    if (Variant == 3)
      Next->CallSiteAddr = First->CallSiteAddr;
    HighStmt Inner;
    Inner.Kind = StmtKind::If;
    Inner.Addr = 0x1020;
    Inner.Cond = parameter(0, T, Arch::X64);
    Inner.Body = {returning(parameter(0, T, Arch::X64))};
    HighStmt Outer;
    Outer.Kind = StmtKind::If;
    Outer.Addr = F.Entry;
    Outer.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, First,
                                     HighExpr::makeConst(UINT64_MAX, 8));
    auto Effect = assign(temporary(20, T, Arch::X64), Next);
    Effect.Addr = 0x1010;
    Outer.Body = {Effect, Inner, returning(HighExpr::makeConst(0, 8))};
    F.Body = {Outer, returning(HighExpr::makeConst(0, 8))};
    MedFunc Med;
    Med.CC = CallingConv::Win64;
    structureIfElse(F, 10, &Med);
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
    Text += R"(
static unsigned predicates, normal_calls, shared_calls;
static uint64_t selected, observed;
static uint64_t predicate(uint64_t value) { ++predicates; return selected; }
static uint64_t normal(uint64_t value) { ++normal_calls; observed=value; return 7; }
static uint64_t shared(uint64_t value) { ++shared_calls; observed=value; return selected; }
int main(void) {
  uint64_t choices[]={0,1,UINT64_MAX,UINT64_C(0x8000000000000000)};
  for (unsigned i=0;i!=4;++i) for (unsigned value=0;value!=2;++value) {
    selected=choices[i]; predicates=normal_calls=0; observed=0;
    uint64_t result=kept_predicate_calls(value,(uintptr_t)&predicate,
                                       (uintptr_t)&normal);
    unsigned taken=selected!=UINT64_MAX;
    if (result!=(taken?value:0) || predicates!=1 || normal_calls!=taken ||
        observed!=(taken?value+1:0)) return 1;
    shared_calls=0; observed=0;
    result=kept_predicate_calls(value,(uintptr_t)&shared,(uintptr_t)&shared);
    if (result!=(taken?value:0) || shared_calls!=1+taken ||
        observed!=(taken?value+1:value)) return 2;
  }
  return 0;
}
)";
    compileAndRun(Text);
  }
}

TEST(HighSourceExpressions, ComposedPredicateCallRunsOnce) {
  // if (arg1) { t1 = arg0 + 1; t2 = call(t1); if (t2 == 0) return 1; }
  // Composing the prefix spells the condition's call with arg0 + 1. The
  // original assignment is the same execution and must not remain as well.
  const auto T = NdType::makeInt(8, false);
  HighFunc F;
  F.Entry = 0x1000;
  F.Name = "composed_predicate_call";
  F.ReturnType = T;
  F.Params = {{"arg0", T}, {"arg1", T}, {"arg2", T}};
  auto Plus = temporary(10, T, Arch::X64);
  auto Result = temporary(20, T, Arch::X64);
  auto Call = HighExpr::makeCall("indirect", 0x2000, {Plus});
  Call->Type = T;
  Call->CallSiteAddr = 0x1010;
  Call->IsIndirectCall = true;
  Call->IndirectTarget = parameter(2, T, Arch::X64);
  HighStmt Inner;
  Inner.Kind = StmtKind::If;
  Inner.Addr = 0x1020;
  Inner.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, Result, HighExpr::makeConst(0, 8));
  Inner.Body = {returning(HighExpr::makeConst(1, 8))};
  HighStmt Outer;
  Outer.Kind = StmtKind::If;
  Outer.Addr = F.Entry;
  Outer.Cond =
      HighExpr::makeBinop(NdOp::INT_NOTEQUAL, parameter(1, T, Arch::X64),
                          HighExpr::makeConst(0, 8));
  auto Add = assign(Plus, HighExpr::makeBinop(NdOp::INT_ADD,
                                              parameter(0, T, Arch::X64),
                                              HighExpr::makeConst(1, 8)));
  Add.Addr = 0x1008;
  auto Effect = assign(Result, Call);
  Effect.Addr = 0x1010;
  Outer.Body = {Add, Effect, Inner};
  F.Body = {Outer, returning(HighExpr::makeConst(0, 8))};
  MedFunc Med;
  Med.CC = CallingConv::Win64;
  structureIfElse(F, 10, &Med);
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
  const std::string Emitted = Text;
  Text += R"(
static unsigned calls;
static uint64_t probe(uint64_t value) { ++calls; return value == 5 ? 0 : 1; }
int main(void) {
  for (uint64_t value = 3; value != 7; ++value)
    for (uint64_t guard = 0; guard != 2; ++guard) {
      calls = 0;
      uint64_t result = composed_predicate_call(value, guard,
                                                (uintptr_t)&probe);
      if (result != (guard && value + 1 == 5) || calls != guard)
        return 1;
    }
  return 0;
}
)";
  SCOPED_TRACE(Emitted);
  compileAndRun(Text);
}

TEST(HighSourceExpressions, GuardAndCallShareTheComputedTarget) {
  MedFunc Med;
  Med.Entry = 0x1000;
  Med.Name = "shared_target";
  Med.CC = CallingConv::Win64;
  Med.ReturnType = NdType::makeInt(8, false);
  auto P0 = parameter(0, NdType::makeInt(8, false), Arch::X64)->Var;
  auto P1 = parameter(1, NdType::makeInt(4, false), Arch::X64)->Var;
  Med.Params = {P0, P1};
  const auto Value = [](int Id, unsigned Bytes) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = Bytes;
    V.TheArch = Arch::X64;
    return V;
  };
  const auto Op = [](NdOp Code, MedVar Output,
                     std::initializer_list<MedVar> Inputs) {
    MedOp O;
    O.Opcode = Code;
    O.Output = Output;
    for (const auto &V : Inputs)
      O.addInput(V);
    return O;
  };
  const auto Return = [&](MedVar Result, int Block) {
    auto RAX = Value(20 + Block, 8);
    RAX.Kind = MedVar::Reg;
    RAX.RegOff = x86reg::RAX;
    return std::vector<MedOp>{Op(NdOp::COPY, RAX, {Result}),
                              Op(NdOp::RETURN, {}, {RAX})};
  };
  MedBlock Entry, Call, Empty;
  Entry.Id = 0;
  Entry.StartAddr = 0x1000;
  Entry.Succs = {2, 1};
  Entry.Ops = {
      Op(NdOp::INT_XOR, Value(2, 4), {P1, MedVar::makeConst(0x55, 4)}),
      Op(NdOp::INT_ZEXT, Value(3, 8), {Value(2, 4)}),
      Op(NdOp::INT_ADD, Value(4, 8), {P0, Value(3, 8)}),
      Op(NdOp::INT_EQUAL, Value(5, 1), {Value(4, 8), MedVar::makeConst(0, 8)}),
      Op(NdOp::COND_BR, {}, {MedVar::makeConst(0x1020, 8), Value(5, 1)})};
  Call.Id = 1;
  Call.StartAddr = 0x1010;
  Call.Preds = {0};
  Call.Ops = {Op(NdOp::INDIR_CALL, Value(6, 8), {Value(4, 8), Value(4, 8)})};
  Call.Ops[0].ExactArguments = true;
  Call.Ops[0].CallSiteId = 1;
  Call.Ops[0].CalleeRegisterArgs = 1;
  auto Tail = Return(Value(6, 8), 1);
  Call.Ops.insert(Call.Ops.end(), Tail.begin(), Tail.end());
  Empty.Id = 2;
  Empty.StartAddr = 0x1020;
  Empty.Preds = {0};
  Empty.Ops = Return(MedVar::makeConst(0, 8), 2);
  Med.Blocks = {Entry, Call, Empty};
  for (auto &B : Med.Blocks) {
    B.EndAddr = B.StartAddr + B.Ops.size();
    for (size_t I = 0; I < B.Ops.size(); ++I)
      B.Ops[I].Addr = B.StartAddr + I;
  }
  MedCallInfo Info;
  Info.BlockId = 1;
  Info.OpIdx = 0;
  Info.IsIndirect = true;
  Info.Args = {Value(4, 8)};
  Med.CallInfos.push_back(Info);
  auto F = MedToHighConverter().convert(Med, Arch::X64);
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
  const auto Xor = Text.find("0x55");
  ASSERT_NE(Xor, std::string::npos) << Text;
  EXPECT_EQ(Text.find("0x55", Xor + 1), std::string::npos) << Text;
  EXPECT_EQ(Text.find("__builtin_bit_cast"), std::string::npos) << Text;
  Text += R"(
static uint64_t actual(uint64_t target) { return target^0x123; }
int main(void) {
  uint32_t words[]={0,0x55,UINT32_MAX,UINT32_C(0x80000000)};
  uint64_t address=(uint64_t)(uintptr_t)&actual;
  for (unsigned i=0;i<4;++i) {
    uint64_t offset=(uint64_t)(words[i]^0x55u);
    if (shared_target(address-offset,words[i])!=(address^0x123)) return 1;
    if (shared_target(0-offset,words[i])) return 2;
  }
  return 0;
}
)";
  compileAndRun(Text);
}

} // namespace
