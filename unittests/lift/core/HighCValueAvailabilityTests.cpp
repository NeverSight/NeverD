//===- HighCValueAvailabilityTests.cpp - Final scalar C path proofs ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent reaching-path controls and native observations of HighC's
/// deferred scalar values, including paths that must still trap.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/HighC/HighCWriter.h"
#include "../../../lib/ir/high/pass/HighFlowGraph.h"
#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"

#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;
using source_call_execution_test::compileAndRun;

ExprPtr local(int Id, uint16_t Bytes = 1) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = Bytes;
  return HighExpr::makeVar(V, NdType::makeInt(Bytes, false));
}

ExprPtr param(int Id, uint16_t Bytes = 1) {
  auto E = local(Id, Bytes);
  E->Var.Kind = MedVar::Param;
  return E;
}

HighStmt assign(ExprPtr Dst, ExprPtr Value, va_t Addr = 0) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Dst = std::move(Dst);
  S.Val = std::move(Value);
  S.IsPhiCopy = true;
  S.Addr = Addr;
  return S;
}

HighStmt result(ExprPtr Value, va_t Addr = 0) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = std::move(Value);
  S.Addr = Addr;
  return S;
}

HighStmt condition(ExprPtr Test, std::vector<HighStmt> Body) {
  HighStmt S;
  S.Kind = StmtKind::If;
  S.Cond = std::move(Test);
  S.Body = std::move(Body);
  return S;
}

HighFunc diamond(uint16_t Bytes = 1) {
  HighFunc F;
  F.Name = "observed";
  F.ReturnType = NdType::makeInt(1, false);
  F.Params = {{"arg0", NdType::makeInt(Bytes, false)},
              {"arg1", NdType::makeInt(Bytes, false)}};
  const auto A = param(0, Bytes), B = param(1, Bytes), X = local(100);
  const auto Compare = [&](NdOp Op) { return HighExpr::makeBinop(Op, A, B); };
  F.Body = {
      assign(X, HighExpr::makeUndef(1), 0x1000),
      condition(Compare(NdOp::INT_LESS),
                {assign(X, HighExpr::makeConst(37, 1), 0x1010)}),
      condition(Compare(NdOp::INT_LESSEQUAL),
                {condition(Compare(NdOp::INT_NOTEQUAL), {result(X, 0x1030)})}),
      result(HighExpr::makeConst(7, 1), 0x1040)};
  return F;
}

std::string emit(const HighFunc &F) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  Options.EmitComments = false;
  EXPECT_TRUE(HighCEmitter().emit({F}, OS, Options));
  return Text;
}

bool hasGuard(llvm::StringRef Source) {
  return Source.contains("no recovered semantics");
}

std::string exhaustiveHarness() {
  return R"c(
int main(void) {
  for (unsigned a = 0; a < 256; ++a)
    for (unsigned b = 0; b < 256; ++b)
      if (observed(a, b) != (a < b ? 37 : 7)) return 1;
  return 0;
}
)c";
}

std::string trapHarness(llvm::StringRef Arguments,
                        llvm::StringRef Positive = "0, 1") {
  return R"c(
#include <signal.h>
#include <stdlib.h>
static void expected_trap(int sig) { _Exit(0); }
int main(void) {
)c" +
         (Positive.empty()
              ? std::string()
              : "  if (observed(" + Positive.str() + ") != 37) return 1;\n") +
         R"c(
  signal(SIGILL, expected_trap);
#ifdef SIGTRAP
  signal(SIGTRAP, expected_trap);
#endif
  observed()c" +
         Arguments.str() + R"c();
  return 2;
}
)c";
}

TEST(HighCValueAvailability, AllReachingComparisonsDischargeTheUnknownArm) {
  for (const uint16_t Bytes : {1, 4, 8}) {
    SCOPED_TRACE(Bytes);
    const std::string Source = emit(diamond(Bytes));
    EXPECT_FALSE(hasGuard(Source)) << Source;
    for (const char *Level : {"-O0", "-O2"})
      compileAndRun(
          Source + exhaustiveHarness(),
          {Level, "-fsanitize=undefined", "-fno-sanitize-recover=all"});
  }
}

TEST(HighCValueAvailability, WholeWidthZeroTestsPreserveEquality) {
  for (const uint16_t Bytes : {1, 4, 8})
    for (const NdOp Op : {NdOp::INT_SUB, NdOp::INT_XOR})
      for (const bool ZeroFirst : {false, true}) {
        SCOPED_TRACE(Bytes);
        SCOPED_TRACE(static_cast<int>(Op));
        SCOPED_TRACE(ZeroFirst);
        HighFunc F = diamond(Bytes);
        auto Difference =
            HighExpr::makeBinop(Op, param(0, Bytes), param(1, Bytes));
        Difference->Type = NdType::makeInt(Bytes, false);
        auto Zero = HighExpr::makeConst(0, Bytes);
        F.Body[2].Body[0].Cond = HighExpr::makeBinop(
            NdOp::INT_NOTEQUAL, ZeroFirst ? Zero : Difference,
            ZeroFirst ? Difference : Zero);
        const auto Source = emit(F);
        EXPECT_FALSE(hasGuard(Source)) << Source;
        for (const char *Level : {"-O0", "-O2"})
          compileAndRun(
              Source + exhaustiveHarness(),
              {Level, "-fsanitize=undefined", "-fno-sanitize-recover=all"});
      }
}

TEST(HighCValueAvailability, MissingAndMismatchedPredicatesKeepTheGuard) {
  for (int Mode = 0; Mode < 4; ++Mode) {
    SCOPED_TRACE(Mode);
    HighFunc F = diamond();
    std::string Bad = "1, 1", Good = "0, 1";
    if (Mode == 0) {
      F.Body[2].Body = F.Body[2].Body[0].Body;
    } else if (Mode == 1) {
      F.Body[1].Cond->Op = NdOp::INT_SLESS;
      Bad = "0, 255";
    } else if (Mode == 2) {
      F.Body.insert(
          F.Body.begin() + 2,
          assign(param(0), HighExpr::makeBinop(NdOp::INT_ADD, param(0),
                                               HighExpr::makeConst(1, 1))));
      Bad = "255, 1";
      Good = "0, 2";
    } else {
      F.Body.insert(F.Body.begin() + 2,
                    assign(local(100), HighExpr::makeUndef(1)));
      Bad = "0, 1";
      Good.clear();
    }
    const auto Source = emit(F);
    EXPECT_TRUE(hasGuard(Source)) << Source;
    for (const char *Level : {"-O0", "-O2"})
      compileAndRun(Source + trapHarness(Bad, Good), {Level});
  }
}

TEST(HighCValueAvailability, IntegerCopiesTransportOnlyTheirAvailability) {
  for (const bool Narrow : {false, true}) {
    SCOPED_TRACE(Narrow);
    HighFunc F = diamond();
    if (Narrow) {
      F.Body[0] = assign(local(100, 8), HighExpr::makeUndef(8), 0x1000);
      F.Body[1].Body[0] =
          assign(local(100, 8), HighExpr::makeConst(37, 8), 0x1010);
    }
    auto Value = local(100, Narrow ? 8 : 1);
    if (Narrow) {
      Value =
          HighExpr::makeBinop(NdOp::SUBBYTES, Value, HighExpr::makeConst(0, 8));
      Value->Type = NdType::makeInt(1, false);
    }
    F.Body.insert(F.Body.begin() + 2, assign(local(101), Value, 0x1020));
    F.Body[3].Body[0].Body[0].RetVal = local(101);
    const auto Source = emit(F);
    EXPECT_FALSE(hasGuard(Source)) << Source;
    for (const char *Level : {"-O0", "-O2"})
      compileAndRun(Source + exhaustiveHarness(), {Level});
  }
}

TEST(HighCValueAvailability, AUsePrecedesItsOwnKnownAssignment) {
  HighFunc F = diamond();
  HighStmt Observe;
  Observe.Kind = StmtKind::Call;
  Observe.CallExpr = HighExpr::makeCall("consume", 0x5000, {local(100)});
  Observe.CallExpr->Type = NdType::makeVoid();
  F.Body[1].Body.insert(F.Body[1].Body.begin(), Observe);
  const auto Source = emit(F);
  EXPECT_TRUE(hasGuard(Source)) << Source;
  for (const char *Level : {"-O0", "-O2"})
    compileAndRun(Source + "\nint consume(int x) { return x; }\n" +
                      trapHarness("0, 1", ""),
                  {Level});
}

TEST(HighCValueAvailability, ATruncatedDifferenceDoesNotEquateWholeWords) {
  for (const bool Stored : {false, true}) {
    SCOPED_TRACE(Stored);
    HighFunc F = diamond(8);
    auto Difference =
        HighExpr::makeBinop(NdOp::INT_SUB, param(0, 8), param(1, 8));
    auto Byte = Stored ? local(200)
                       : HighExpr::makeBinop(NdOp::SUBBYTES, Difference,
                                             HighExpr::makeConst(0, 8));
    Byte->Type = NdType::makeInt(1, false);
    F.Body[1].Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, Byte,
                                         HighExpr::makeConst(0, 1));
    if (Stored) {
      auto Capture = assign(local(200), Difference);
      Capture.KeepsName = true;
      F.Body.insert(F.Body.begin() + 1, Capture);
    }
    const auto Source = emit(F);
    EXPECT_TRUE(hasGuard(Source)) << Source;
    for (const char *Level : {"-O0", "-O2"})
      compileAndRun(Source + trapHarness("0, 256"), {Level});
  }
}

TEST(HighCValueAvailability, LoopWritesInvalidateEarlierKnownPayloads) {
  HighFunc F = diamond();
  F.Params.push_back({"arg2", NdType::makeInt(1, false)});
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Cond = param(2);
  Loop.Body = {
      assign(local(100), HighExpr::makeUndef(1)),
      assign(param(2), HighExpr::makeBinop(NdOp::INT_SUB, param(2),
                                           HighExpr::makeConst(1, 1)))};
  F.Body.insert(F.Body.begin() + 2, Loop);
  const auto Source = emit(F);
  EXPECT_TRUE(hasGuard(Source)) << Source;
  for (const char *Level : {"-O0", "-O2"})
    compileAndRun(Source + trapHarness("0, 1, 1", "0, 1, 0"), {Level});
}

TEST(HighCValueAvailability, GotoJoinsUseEveryIncomingPath) {
  for (const bool DirectEntry : {false, true}) {
    SCOPED_TRACE(DirectEntry);
    HighFunc F = diamond();
    HighStmt Goto;
    Goto.Kind = StmtKind::Goto;
    Goto.GotoTarget = 0x1020;
    auto Choose = F.Body[1];
    Choose.Body = {Goto};
    Goto.GotoTarget = 0x1030;
    auto Observe = F.Body[2];
    Observe.Addr = 0x1030;
    Observe.Body[0].Body[0].Addr = 0x1034;
    F.Body = {F.Body[0], Choose,
              Goto,      assign(local(100), HighExpr::makeConst(37, 1), 0x1020),
              Observe,   F.Body.back()};
    if (DirectEntry) {
      Goto.GotoTarget = 0x1030;
      F.Body.insert(F.Body.begin() + 1, Goto);
    }
    const auto Source = emit(F);
    EXPECT_EQ(hasGuard(Source), DirectEntry) << Source;
    for (const char *Level : {"-O0", "-O2"})
      compileAndRun(Source + (DirectEntry ? trapHarness("0, 1", "")
                                          : exhaustiveHarness()),
                    {Level});
  }
}

TEST(HighCValueAvailability, AmbiguousLabelsAndExceptionEntriesStayUnproved) {
  for (int Mode = 0; Mode < 3; ++Mode) {
    SCOPED_TRACE(Mode);
    HighFunc F = diamond();
    if (Mode == 0) {
      HighStmt Goto;
      Goto.Kind = StmtKind::Goto;
      Goto.GotoTarget = 0x1010;
      F.Body.push_back(Goto);
      F.Body.push_back(assign(local(100), HighExpr::makeConst(19, 1), 0x1010));
    } else if (Mode == 1) {
      HighStmt Region;
      Region.Kind = StmtKind::CxxTry;
      Region.Body = std::move(F.Body);
      F.Body = {std::move(Region)};
    } else {
      F.Body[2].CxxContinuationReturnTargets = {0x1000};
    }
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    CEmitterOptions Options;
    HighCWriter Writer(OS, Options, nullptr);
    Writer.CurrentFunc = &F;
    Writer.DeferredUnknownFlags["t100"] = "t100_known";
    if (Mode == 0)
      Writer.GotoTargets.insert(0x1010);
    Writer.buildAvailabilityFlow(F);
    ASSERT_TRUE(Writer.AvailabilityFlow);
    if (Mode == 0) {
      EXPECT_TRUE(Writer.AvailabilityFlow->Complete);
      const auto First = Writer.AvailabilityPoints.at(&F.Body[1].Body[0]);
      const auto Second = Writer.AvailabilityPoints.at(&F.Body.back());
      EXPECT_TRUE(Writer.AvailabilityFlow->Nodes[First.Node].Entry);
      EXPECT_TRUE(Writer.AvailabilityFlow->Nodes[Second.Node].Entry);
    } else {
      EXPECT_FALSE(Writer.AvailabilityFlow->Complete);
    }
  }
}

TEST(HighCValueAvailability, AnEmptyLastCaseFallsIntoTheDefault) {
  HighStmt Switch;
  Switch.Kind = StmtKind::Switch;
  Switch.SwitchExpr = param(0);
  Switch.Cases = {{1, {}, true}, {2, {}, true}};
  Switch.DefaultBody = {assign(local(100), HighExpr::makeConst(37, 1))};
  HighFunc F = diamond();
  F.Params.resize(1);
  F.Body = {F.Body[0], Switch, result(local(100))};
  HighFlowGraph Flow(F.Body, [](const HighStmt &) { return false; });
  const auto Default = Flow.NodeOf.at(&F.Body[1].DefaultBody[0]);
  EXPECT_EQ(Flow.Nodes[Flow.NodeOf.at(&F.Body[1])].Successors,
            (std::vector<size_t>{Default, Default, Default}));
  const auto Source = emit(F);
  EXPECT_FALSE(hasGuard(Source)) << Source;
  for (const char *Level : {"-O0", "-O2"})
    compileAndRun(Source + R"c(
int main(void) {
  for (unsigned x = 0; x < 256; ++x)
    if (observed(x) != 37) return 1;
  return 0;
}


)c",
                  {Level});
}

TEST(HighCValueAvailability, AnOmittedSwitchCannotDefineAPayload) {
  HighFunc F = diamond();
  HighStmt Switch;
  Switch.Kind = StmtKind::Switch;
  Switch.DefaultBody = {assign(local(100), HighExpr::makeConst(37, 1))};
  // The renderer omits a switch without its selector. Its unprinted body
  // cannot supply the payload that a later observation reads.
  F.Body = {F.Body[0], Switch, result(local(100))};
  const auto Source = emit(F);
  EXPECT_TRUE(hasGuard(Source)) << Source;
  for (const char *Level : {"-O0", "-O2"})
    compileAndRun(Source + trapHarness("0, 1", ""), {Level});
}

TEST(HighCValueAvailability, ANewFunctionDoesNotBorrowThePreviousGraph) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  HighCWriter Writer(OS, Options, nullptr);
  auto First = diamond();
  Writer.writeFunctionProjection(First);
  const size_t EndFirst = Text.size();
  auto Second = diamond();
  Second.Body[2].Body = Second.Body[2].Body[0].Body;
  Writer.writeFunctionProjection(Second);
  EXPECT_FALSE(hasGuard(llvm::StringRef(Text).take_front(EndFirst)));
  EXPECT_TRUE(hasGuard(llvm::StringRef(Text).drop_front(EndFirst)));
}

TEST(HighCValueAvailability, StoredComparisonResultsKeepTheirEvaluationPoint) {
  for (const bool Overwrite : {false, true}) {
    SCOPED_TRACE(Overwrite);
    HighFunc F = diamond();
    if (!Overwrite) {
      auto Capture = assign(local(200), F.Body[1].Cond);
      Capture.KeepsName = true;
      F.Body[1].Cond = local(200);
      F.Body.insert(F.Body.begin() + 1, Capture);
      auto Second = assign(local(201), F.Body[3].Cond);
      Second.KeepsName = true;
      F.Body[3].Cond = local(201);
      F.Body.insert(F.Body.begin() + 3, Second);
    } else {
      auto Capture = assign(
          local(200), HighExpr::makeBinop(NdOp::INT_LESS, param(0), param(1)));
      Capture.KeepsName = true;
      F.Body = {
          F.Body[0],
          F.Body[1],
          assign(param(0), HighExpr::makeBinop(NdOp::INT_ADD, param(0),
                                               HighExpr::makeConst(1, 1))),
          Capture,
          assign(param(0), HighExpr::makeBinop(NdOp::INT_SUB, param(0),
                                               HighExpr::makeConst(1, 1))),
          condition(local(200), {result(local(100))}),
          F.Body.back()};
    }
    const auto Source = emit(F);
    EXPECT_EQ(hasGuard(Source), Overwrite) << Source;
    for (const char *Level : {"-O0", "-O2"})
      compileAndRun(
          Source +
              (Overwrite ? trapHarness("255, 1", "0, 2") : exhaustiveHarness()),
          {Level, "-fsanitize=undefined", "-fno-sanitize-recover=all"});
  }
}

TEST(HighCValueAvailability, UnrelatedLiveStatementsDoNotSpendThePathBudget) {
  HighFunc F = diamond();
  std::vector<HighStmt> Noise;
  for (unsigned I = 0; I < 512; ++I) {
    HighStmt S;
    S.Kind = StmtKind::Call;
    auto Value =
        HighExpr::makeBinop(NdOp::INT_ADD, param(0), HighExpr::makeConst(I, 4));
    Value->Type = NdType::makeInt(4, false);
    S.CallExpr = HighExpr::makeCall("note_noise", 0x5000, {Value});
    S.CallExpr->Type = NdType::makeInt(4);
    Noise.push_back(std::move(S));
  }
  F.Body.insert(F.Body.begin() + 2, Noise.begin(), Noise.end());
  const auto Source = emit(F);
  EXPECT_FALSE(hasGuard(Source)) << Source;
  for (const char *Level : {"-O0", "-O2"})
    compileAndRun(Source + R"c(
static unsigned calls, sum;
int note_noise(int x) { ++calls; sum += (unsigned)x; return x; }
int main(void) {
  for (unsigned a = 0; a < 256; ++a) {
    unsigned b = (a * 37 + 19) & 255;
    calls = sum = 0;
    if (observed(a, b) != (a < b ? 37 : 7)) return 1;
    if (calls != 512 || sum != a * 512 + 511 * 512 / 2) return 2;
  }
  return 0;
}
)c",
                  {Level});
}

TEST(HighCValueAvailability, StoredZeroTestsKeepTheArithmeticEvaluationPoint) {
  for (const bool Overwrite : {false, true})
    for (const auto Op : {NdOp::INT_SUB, NdOp::INT_XOR}) {
      SCOPED_TRACE(Overwrite);
      SCOPED_TRACE(static_cast<int>(Op));
      HighFunc F = diamond(4);
      auto Difference = assign(
          local(200, 4), HighExpr::makeBinop(Op, param(0, 4), param(1, 4)));
      Difference.KeepsName = true;
      auto Equal =
          assign(local(201), HighExpr::makeBinop(NdOp::INT_EQUAL, local(200, 4),
                                                 HighExpr::makeConst(0, 4)));
      Equal.KeepsName = true;
      auto Negated =
          assign(local(202), HighExpr::makeUnary(NdOp::BOOL_NOT, local(201)));
      Negated.KeepsName = true;
      F.Body[2].Body[0].Cond = local(202);
      F.Body.insert(F.Body.begin() + 2, {Difference, Equal, Negated});
      if (Overwrite)
        F.Body.insert(F.Body.begin() + 5,
                      assign(param(0, 4),
                             HighExpr::makeBinop(NdOp::INT_ADD, param(0, 4),
                                                 HighExpr::makeConst(1, 4))));
      const auto Source = emit(F);
      EXPECT_EQ(hasGuard(Source), Overwrite) << Source;
      for (const char *Level : {"-O0", "-O2"})
        compileAndRun(
            Source + (Overwrite ? trapHarness("0xffffffffU, 1", "0, 2")
                                : exhaustiveHarness()),
            {Level, "-fsanitize=undefined", "-fno-sanitize-recover=all"});
    }
}

std::string emitWithDeadFrameJoin(HighFunc &F, const HighStmt &Join,
                                  bool UseGraph, bool CollectFlags = false) {
  std::string Source = "#include <stdint.h>\n"
                       "uint8_t observed(uint8_t arg0, uint8_t arg1) {\n"
                       "uint8_t t100;\n";
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  HighCWriter Writer(OS, Options, nullptr);
  Writer.CurrentFunc = &F;
  Writer.FuncReturnType = F.ReturnType;
  Writer.AssignedNames.insert("t100");
  Writer.Analysis.AssignedVars.insert("t100");
  Writer.DeclaredCNames.insert("t100");
  Writer.DeclaredCTypes["t100"] = F.ReturnType;
  // Ambiguous frame joins are the renderer's existing exception to its
  // ordinary dead-statement omission after alias forwarding is rejected.
  Writer.Analysis.DeadStmts.insert(&Join);
  Writer.AmbiguousFrameAliases.insert("t100");
  if (Join.Addr)
    Writer.GotoTargets.insert(Join.Addr);
  if (CollectFlags) {
    Writer.collectDeferredUnknowns(F);
    EXPECT_TRUE(Writer.DeferredUnknownFlags.count("t100"));
  } else {
    OS << "uint8_t t100_known = 0;\n";
    Writer.DeferredUnknownFlags["t100"] = "t100_known";
  }
  if (UseGraph)
    Writer.buildAvailabilityFlow(F);
  Writer.writeStmts(F.Body, 1);
  OS << "}\n";
  return Source;
}

TEST(HighCValueAvailability, APrintedDeadJoinStillClearsAvailability) {
  for (const bool UseGraph : {false, true})
    for (int Shape = 0; Shape < 3; ++Shape) {
      SCOPED_TRACE(UseGraph);
      SCOPED_TRACE(Shape);
      HighFunc F = diamond();
      auto Join =
          assign(local(100), HighExpr::makeUndef(1), Shape == 2 ? 0x1010 : 0);
      F.Body = {assign(local(100), HighExpr::makeConst(37, 1)),
                Shape == 1 ? condition(param(0), {Join}) : Join,
                result(local(100))};
      const auto &Dead = Shape == 1 ? F.Body[1].Body[0] : F.Body[1];
      const auto Source = emitWithDeadFrameJoin(F, Dead, UseGraph);
      EXPECT_TRUE(hasGuard(Source)) << Source;
      EXPECT_NE(Source.find("t100_known = 0;", Source.find("t100_known = 1;")),
                std::string::npos)
          << Source;
      for (const char *Level : {"-O0", "-O2"})
        compileAndRun(Source + trapHarness("1, 0", Shape == 1 ? "0, 0" : ""),
                      {Level});
    }
}

TEST(HighCValueAvailability, ALabelDoesNotSuppressARetainedKnownJoin) {
  HighFunc F = diamond();
  F.Body = {assign(local(100), HighExpr::makeConst(37, 1)),
            assign(local(100), HighExpr::makeConst(19, 1), 0x1010),
            result(local(100))};
  const auto Source = emitWithDeadFrameJoin(F, F.Body[1], true);
  for (const char *Level : {"-O0", "-O2"})
    compileAndRun(Source + "int main(void) { return observed(0, 0) != 19; }\n",
                  {Level});
}

TEST(HighCValueAvailability, RetainedDeadJoinsParticipateInDeferredTransport) {
  HighFunc F = diamond();
  F.Body = {assign(local(100), HighExpr::makeConst(37, 1)),
            condition(param(0), {assign(local(100), HighExpr::makeUndef(1))}),
            result(local(100))};
  const auto Source = emitWithDeadFrameJoin(F, F.Body[1].Body[0], true, true);
  EXPECT_TRUE(hasGuard(Source)) << Source;
  for (const char *Level : {"-O0", "-O2"})
    compileAndRun(Source + trapHarness("1, 0", "0, 0"), {Level});
}

} // namespace
