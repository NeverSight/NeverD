//===- InterprocReachabilityTests.cpp - Known-entry reachability ----------===//

#include "gtest/gtest.h"

#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/safety/InterprocReachability.h"
#include "neverd/safety/SinkCatalog.h"

using namespace neverd;
using namespace neverd::safety;

namespace {

MedVar parameter(int Id, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = Id;
  V.Size = Size;
  return V;
}

MedVar temporary(int Id, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = Size;
  return V;
}

MedFunc function(va_t Entry, llvm::StringRef Name,
                 std::vector<MedVar> Params = {}) {
  MedFunc F;
  F.Entry = Entry;
  F.Name = Name.str();
  F.Params = std::move(Params);
  F.CC = CallingConv::SysV_AMD64;
  return F;
}

void call(MedFunc &Caller, va_t CallVA, va_t Callee,
          std::vector<MedVar> Args = {}, bool Indirect = false) {
  if (Caller.Blocks.empty()) {
    Caller.Blocks.emplace_back();
    Caller.Blocks.back().Id = 0;
    Caller.Blocks.back().StartAddr = Caller.Entry;
  }
  MedBlock &Block = Caller.Blocks.back();
  MedOp Op;
  Op.Opcode = Indirect ? NdOp::INDIR_CALL : NdOp::CALL;
  Op.Addr = CallVA;
  Op.OriginSeq = static_cast<int>(Block.Ops.size());
  if (!Indirect)
    Op.addInput(MedVar::makeConst(Callee, 8));
  const int OpIdx = static_cast<int>(Block.Ops.size());
  Block.Ops.push_back(Op);

  MedCallInfo Info;
  Info.BlockId = Block.Id;
  Info.OpIdx = OpIdx;
  Info.TargetAddr = Callee;
  Info.Args = std::move(Args);
  Info.IsIndirect = Indirect;
  Caller.CallInfos.push_back(std::move(Info));
}

void ret(MedFunc &Function) {
  if (Function.Blocks.empty()) {
    Function.Blocks.emplace_back();
    Function.Blocks.back().Id = 0;
    Function.Blocks.back().StartAddr = Function.Entry;
  }
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Function.Blocks.back().Ops.push_back(Return);
}

MedVar sourceCall(MedFunc &Caller, va_t CallVA, llvm::StringRef Name,
                  MedVar Output, std::vector<MedVar> Args = {}) {
  if (Caller.Blocks.empty()) {
    Caller.Blocks.emplace_back();
    Caller.Blocks.back().Id = 0;
    Caller.Blocks.back().StartAddr = Caller.Entry;
  }
  MedBlock &Block = Caller.Blocks.back();
  MedOp Op;
  Op.Opcode = NdOp::CALL;
  Op.Output = Output;
  Op.Addr = CallVA;
  Op.OriginSeq = static_cast<int>(Block.Ops.size());
  Op.addInput(MedVar::makeConst(0x900000, 8));
  const int OpIdx = static_cast<int>(Block.Ops.size());
  Block.Ops.push_back(std::move(Op));

  MedCallInfo Info;
  Info.BlockId = Block.Id;
  Info.OpIdx = OpIdx;
  Info.TargetAddr = 0x900000;
  Info.TargetName = Name.str();
  Info.Args = std::move(Args);
  Caller.CallInfos.push_back(std::move(Info));
  return Output;
}

AnalysisInput input(BinaryImage &Image, std::vector<MedFunc> &Functions) {
  if (Image.Bits == Bitness::Unknown) {
    if (Image.Arch == Arch::X64 || Image.Arch == Arch::AArch64)
      Image.Bits = Bitness::Bits64;
    else if (Image.Arch == Arch::X86 || Image.Arch == Arch::ARM)
      Image.Bits = Bitness::Bits32;
  }
  AnalysisInput In;
  In.Img = &Image;
  In.MedFuncs = &Functions;
  return In;
}

} // namespace

TEST(InterprocReachability, PropagatesEntryTaintThroughDirectWrapper) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;

  MedFunc Main = function(0x1000, "main", {parameter(0, 4), parameter(1)});
  call(Main, 0x1010, 0x2000, {Main.Params[1]});
  MedFunc Wrapper = function(0x2000, "wrapper", {parameter(7)});
  std::vector<MedFunc> Functions{Main, Wrapper};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});

  const FunctionReachability *Reach = Result.findFunction(Wrapper.Entry);
  ASSERT_NE(Reach, nullptr);
  EXPECT_EQ(Reach->Status, ReachabilityStatus::Reachable);
  ASSERT_EQ(Reach->CallChain.size(), 1u);
  EXPECT_EQ(Reach->CallChain.front().CallVA, 0x1010u);
  EXPECT_EQ(Reach->EntryVA, Main.Entry);

  const ParameterFlow *Flow = Result.findParameter(Wrapper.Entry, 0);
  ASSERT_NE(Flow, nullptr);
  EXPECT_EQ(Flow->Flow, ArgFlow::Tainted);
  EXPECT_EQ(Flow->Source, "argv");
}

TEST(InterprocReachability, SeedsOnlyValidatedApplicationParameterRoles) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;

  MedFunc Main =
      function(0x1000, "main", {parameter(0, 4), parameter(1), parameter(2)});
  std::vector<MedFunc> Functions{Main};
  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});

  const ParameterFlow *Argc = Result.findParameter(Main.Entry, 0);
  const ParameterFlow *Argv = Result.findParameter(Main.Entry, 1);
  const ParameterFlow *Envp = Result.findParameter(Main.Entry, 2);
  ASSERT_NE(Argc, nullptr);
  ASSERT_NE(Argv, nullptr);
  ASSERT_NE(Envp, nullptr);
  EXPECT_EQ(Argc->Source, "argc");
  EXPECT_EQ(Argv->Source, "argv");
  EXPECT_EQ(Envp->Source, "envp");
}

TEST(InterprocReachability, WinMainTaintsOnlyCommandLineParameter) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;

  MedFunc WinMain =
      function(0x1000, "WinMain",
               {parameter(0), parameter(1), parameter(2), parameter(3, 4)});
  WinMain.CC = CallingConv::Win64;
  std::vector<MedFunc> Functions{WinMain};
  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});

  EXPECT_EQ(Result.findParameter(WinMain.Entry, 0), nullptr);
  EXPECT_EQ(Result.findParameter(WinMain.Entry, 1), nullptr);
  const ParameterFlow *CommandLine = Result.findParameter(WinMain.Entry, 2);
  ASSERT_NE(CommandLine, nullptr);
  EXPECT_EQ(CommandLine->Source, "command_line");
  EXPECT_EQ(Result.findParameter(WinMain.Entry, 3), nullptr);
}

TEST(InterprocReachability, FamiliarEntryNameWithUnknownABISeedsNothing) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main", {parameter(0, 4), parameter(1)});
  Main.CC = CallingConv::Unknown;
  std::vector<MedFunc> Functions{Main};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  EXPECT_EQ(Result.findParameter(Main.Entry, 0), nullptr);
  EXPECT_EQ(Result.findParameter(Main.Entry, 1), nullptr);
}

TEST(InterprocReachability, MalformedEntrySignatureSeedsNothing) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main", {parameter(0, 4)});
  std::vector<MedFunc> Functions{Main};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  EXPECT_EQ(Result.findParameter(Main.Entry, 0), nullptr);
}

TEST(InterprocReachability, IgnoresCallInDisconnectedCFGBlock) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main");
  Main.Blocks.emplace_back();
  Main.Blocks.back().Id = 0;
  Main.Blocks.back().StartAddr = Main.Entry;
  ret(Main);
  Main.Blocks.emplace_back();
  Main.Blocks.back().Id = 1;
  Main.Blocks.back().StartAddr = 0x1100;
  call(Main, 0x1100, 0x2000);
  MedFunc DeadLeaf = function(0x2000, "dead_leaf");
  std::vector<MedFunc> Functions{Main, DeadLeaf};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  const FunctionReachability *Reach = Result.findFunction(DeadLeaf.Entry);
  ASSERT_NE(Reach, nullptr);
  EXPECT_EQ(Reach->Status, ReachabilityStatus::Unreachable);
}

TEST(InterprocReachability, EqualLengthTiePrefersApplicationRoot) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Exports.push_back({"public_api", 1, 0x1000});
  MedFunc Export = function(0x1000, "public_api");
  MedFunc Main = function(0x5000, "main", {parameter(0, 4), parameter(1)});
  MedFunc Leaf = function(0x9000, "leaf");
  call(Export, 0x1010, Leaf.Entry);
  call(Main, 0x5010, Leaf.Entry);
  std::vector<MedFunc> Functions{Export, Main, Leaf};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  const FunctionReachability *Reach = Result.findFunction(Leaf.Entry);
  ASSERT_NE(Reach, nullptr);
  ASSERT_EQ(Reach->Status, ReachabilityStatus::Reachable);
  EXPECT_EQ(Reach->Kind, SafetyEntryKind::Application);
  EXPECT_EQ(Reach->EntryName, "main");
  ASSERT_EQ(Reach->CallChain.size(), 1u);
  EXPECT_EQ(Reach->CallChain.front().CallVA, 0x5010u);
}

TEST(InterprocReachability, IgnoresTaintFromStructurallyUnreachableCaller) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main", {parameter(0, 4), parameter(1)});
  MedFunc Victim = function(0x3000, "victim", {parameter(7)});
  call(Main, 0x1010, Victim.Entry, {MedVar::makeConst(8, 8)});

  MedFunc Dead = function(0x2000, "dead_helper");
  const MedVar External = sourceCall(
      Dead, 0x2010, "read", temporary(9),
      {MedVar::makeConst(0, 8), temporary(8), MedVar::makeConst(32, 8)});
  call(Dead, 0x2020, Victim.Entry, {External});
  std::vector<MedFunc> Functions{Main, Dead, Victim};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  EXPECT_EQ(Result.findFunction(Victim.Entry)->Status,
            ReachabilityStatus::Reachable);
  EXPECT_EQ(Result.findParameter(Victim.Entry, 0), nullptr);
}

TEST(InterprocReachability, KeepsTaintedAlternatePathOutsideShortestTree) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main", {parameter(0, 4), parameter(1)});
  MedFunc Wrapper = function(0x2000, "wrapper");
  MedFunc Victim = function(0x3000, "victim", {parameter(7)});

  call(Main, 0x1010, Victim.Entry, {MedVar::makeConst(8, 8)});
  call(Main, 0x1020, Wrapper.Entry);
  const MedVar External = sourceCall(
      Wrapper, 0x2010, "read", temporary(9),
      {MedVar::makeConst(0, 8), temporary(8), MedVar::makeConst(32, 8)});
  call(Wrapper, 0x2020, Victim.Entry, {External});
  std::vector<MedFunc> Functions{Main, Wrapper, Victim};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  const FunctionReachability *Structural = Result.findFunction(Victim.Entry);
  ASSERT_NE(Structural, nullptr);
  ASSERT_EQ(Structural->CallChain.size(), 1u);
  EXPECT_EQ(Structural->CallChain.front().CallVA, 0x1010u);

  const ParameterFlow *Flow = Result.findParameter(Victim.Entry, 0);
  ASSERT_NE(Flow, nullptr);
  ASSERT_TRUE(Flow->Witness.has_value());
  ASSERT_EQ(Flow->Witness->CallChain.size(), 2u);
  EXPECT_EQ(Flow->Witness->CallChain[0].CallVA, 0x1020u);
  EXPECT_EQ(Flow->Witness->CallChain[1].CallVA, 0x2020u);
  EXPECT_EQ(Flow->Source, "read");
}

TEST(InterprocReachability, UsesTaintingRootForSharedCalleeFinding) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  Image.Exports.push_back({"public_api", 1, 0x5000});
  MedFunc Main = function(0x1000, "main", {parameter(0, 4), parameter(1)});
  MedFunc Public = function(0x5000, "public_api");
  MedFunc Victim = function(0x9000, "victim", {parameter(7)});
  call(Main, 0x1010, Victim.Entry, {MedVar::makeConst(8, 8)});
  const MedVar External = sourceCall(
      Public, 0x5010, "read", temporary(9),
      {MedVar::makeConst(0, 8), temporary(8), MedVar::makeConst(32, 8)});
  call(Public, 0x5020, Victim.Entry, {External});
  std::vector<MedFunc> Functions{Main, Public, Victim};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  const FunctionReachability *Structural = Result.findFunction(Victim.Entry);
  ASSERT_NE(Structural, nullptr);
  EXPECT_EQ(Structural->Kind, SafetyEntryKind::Application);

  const ParameterFlow *Flow = Result.findParameter(Victim.Entry, 0);
  ASSERT_NE(Flow, nullptr);
  ASSERT_TRUE(Flow->Witness.has_value());
  EXPECT_EQ(Flow->Witness->Kind, SafetyEntryKind::Export);

  Finding Record;
  Record.FuncEntry = Victim.Entry;
  Record.Flow = ArgFlow::Tainted;
  Record.AttackerWitness = Flow->Witness;
  Result.annotate(Record);
  EXPECT_EQ(Record.Reachability.Status, ReachabilityStatus::Reachable);
  EXPECT_EQ(Record.Reachability.Kind, SafetyEntryKind::Export);
  EXPECT_EQ(Record.Reachability.EntryName, "public_api");
  ASSERT_EQ(Record.Reachability.CallChain.size(), 1u);
  EXPECT_EQ(Record.Reachability.CallChain.front().CallVA, 0x5020u);
}

TEST(InterprocReachability, CallDepthBoundsTaintAsWellAsControl) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main", {parameter(0, 4), parameter(1)});
  MedFunc Middle = function(0x2000, "middle", {parameter(7)});
  MedFunc Leaf = function(0x3000, "leaf", {parameter(8)});
  call(Main, 0x1010, Middle.Entry, {Main.Params[1]});
  call(Middle, 0x2010, Leaf.Entry, {Middle.Params[0]});
  std::vector<MedFunc> Functions{Main, Middle, Leaf};
  SafetyBudgets Budgets;
  Budgets.MaxCallDepth = 1;

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), Budgets);
  ASSERT_NE(Result.findParameter(Middle.Entry, 0), nullptr);
  EXPECT_EQ(Result.findParameter(Leaf.Entry, 0), nullptr);
  EXPECT_EQ(Result.findFunction(Leaf.Entry)->Status,
            ReachabilityStatus::Unknown);
  EXPECT_TRUE(Result.BudgetHit);
}

TEST(InterprocReachability,
     RecursiveLongerWitnessDoesNotReportCallDepthExhaustion) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main", {parameter(0, 4), parameter(1)});
  MedFunc Middle = function(0x2000, "middle", {parameter(7)});
  call(Main, 0x1010, Middle.Entry, {Main.Params[1]});
  call(Middle, 0x2010, Middle.Entry, {Middle.Params[0]});
  std::vector<MedFunc> Functions{Main, Middle};
  SafetyBudgets Budgets;
  Budgets.MaxCallDepth = 1;

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), Budgets);
  const ParameterFlow *Flow = Result.findParameter(Middle.Entry, 0);
  ASSERT_NE(Flow, nullptr);
  ASSERT_TRUE(Flow->Witness.has_value());
  EXPECT_EQ(Flow->Witness->CallChain.size(), 1u);
  EXPECT_FALSE(Result.SummaryBudgetHit);
  EXPECT_FALSE(Result.BudgetHit);
}

TEST(InterprocReachability, SummaryBudgetDistinguishesNFromNPlusOne) {
  auto Analyze = [](unsigned Rounds) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Entry = 0x1000;
    MedFunc Main = function(0x1000, "main", {parameter(0, 4), parameter(1)});
    MedFunc Middle = function(0x2000, "middle", {parameter(7)});
    MedFunc Leaf = function(0x3000, "leaf", {parameter(8)});
    call(Main, 0x1010, Middle.Entry, {Main.Params[1]});
    call(Middle, 0x2010, Leaf.Entry, {Middle.Params[0]});
    std::vector<MedFunc> Functions{Main, Middle, Leaf};
    SafetyBudgets Budgets;
    Budgets.MaxSummaryIterations = Rounds;
    InterprocResult Result = analyzeInterprocedural(
        input(Image, Functions), SinkCatalog::defaults(), Budgets);
    const bool HasLeaf = Result.findParameter(Leaf.Entry, 0) != nullptr;
    return std::make_pair(Result.BudgetHit, HasLeaf);
  };

  EXPECT_EQ(Analyze(1), std::make_pair(true, false));
  EXPECT_EQ(Analyze(2), std::make_pair(false, true));
}

TEST(InterprocReachability, MalformedSuccessorInventoryFailsClosed) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main");
  Main.Blocks.emplace_back();
  Main.Blocks.back().Id = 0;
  Main.Blocks.back().StartAddr = Main.Entry;
  Main.Blocks.back().Succs = {99};
  MedFunc Maybe = function(0x3000, "maybe");
  std::vector<MedFunc> Functions{Main, Maybe};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  EXPECT_FALSE(Result.GraphComplete);
  EXPECT_EQ(Result.findFunction(Maybe.Entry)->Status,
            ReachabilityStatus::Unknown);
}

TEST(InterprocReachability,
     DisconnectedMalformedCallerDoesNotPoisonReachableInventory) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main");
  ret(Main);
  MedFunc Dead = function(0x2000, "dead");
  Dead.Blocks.emplace_back();
  Dead.Blocks.back().Id = 0;
  Dead.Blocks.back().StartAddr = Dead.Entry;
  Dead.Blocks.back().Succs = {99};
  MedFunc Maybe = function(0x3000, "maybe");
  std::vector<MedFunc> Functions{Main, Dead, Maybe};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  EXPECT_TRUE(Result.GraphComplete);
  EXPECT_EQ(Result.findFunction(Dead.Entry)->Status,
            ReachabilityStatus::Unreachable);
  EXPECT_EQ(Result.findFunction(Maybe.Entry)->Status,
            ReachabilityStatus::Unreachable);
}

TEST(InterprocReachability,
     DisconnectedUnresolvedCallDoesNotPoisonReachableInventory) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main");
  ret(Main);
  MedFunc Dead = function(0x2000, "dead");
  call(Dead, 0x2010, 0);
  MedFunc Maybe = function(0x3000, "maybe");
  std::vector<MedFunc> Functions{Main, Dead, Maybe};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  EXPECT_TRUE(Result.GraphComplete);
  EXPECT_EQ(Result.findFunction(Dead.Entry)->Status,
            ReachabilityStatus::Unreachable);
  EXPECT_EQ(Result.findFunction(Maybe.Entry)->Status,
            ReachabilityStatus::Unreachable);
}

TEST(InterprocReachability,
     ReachableMalformedChildKeepsPositiveWitnessButBlocksNegatives) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main");
  call(Main, 0x1010, 0x2000);
  ret(Main);
  MedFunc Child = function(0x2000, "child");
  Child.Blocks.emplace_back();
  Child.Blocks.back().Id = 0;
  Child.Blocks.back().StartAddr = Child.Entry;
  Child.Blocks.back().Succs = {99};
  MedFunc Maybe = function(0x3000, "maybe");
  std::vector<MedFunc> Functions{Main, Child, Maybe};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  EXPECT_FALSE(Result.GraphComplete);
  EXPECT_EQ(Result.findFunction(Child.Entry)->Status,
            ReachabilityStatus::Reachable);
  EXPECT_EQ(Result.findFunction(Maybe.Entry)->Status,
            ReachabilityStatus::Unknown);
}

TEST(InterprocReachability, MalformedExportIsPartOfTheMultiRootGraph) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  Image.Exports.push_back({"public_bad", 1, 0x2000});
  MedFunc Main = function(0x1000, "main");
  ret(Main);
  MedFunc Public = function(0x2000, "public_bad");
  Public.Blocks.emplace_back();
  Public.Blocks.back().Id = 0;
  Public.Blocks.back().StartAddr = Public.Entry;
  Public.Blocks.back().Succs = {99};
  MedFunc Maybe = function(0x3000, "maybe");
  std::vector<MedFunc> Functions{Main, Public, Maybe};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  EXPECT_FALSE(Result.GraphComplete);
  const FunctionReachability *PublicReach = Result.findFunction(Public.Entry);
  ASSERT_NE(PublicReach, nullptr);
  EXPECT_EQ(PublicReach->Status, ReachabilityStatus::Reachable);
  EXPECT_EQ(PublicReach->Kind, SafetyEntryKind::Export);
  EXPECT_EQ(Result.findFunction(Maybe.Entry)->Status,
            ReachabilityStatus::Unknown);
}

TEST(InterprocReachability, UnresolvedReachableTargetFailsClosed) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main");
  call(Main, 0x1010, 0);
  MedFunc Maybe = function(0x3000, "maybe");
  std::vector<MedFunc> Functions{Main, Maybe};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  EXPECT_FALSE(Result.GraphComplete);
  EXPECT_EQ(Result.findFunction(Maybe.Entry)->Status,
            ReachabilityStatus::Unknown);
}

TEST(InterprocReachability, UnsafeIndirectBranchFailsClosed) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main");
  Main.Blocks.emplace_back();
  Main.Blocks.back().Id = 0;
  Main.Blocks.back().StartAddr = Main.Entry;
  MedOp Indirect;
  Indirect.Opcode = NdOp::INDIR_BR;
  Indirect.Addr = 0x1010;
  Indirect.addInput(temporary(1));
  Main.Blocks.back().Ops.push_back(Indirect);
  Main.UnsafeIndirectBranchAddresses.insert(Indirect.Addr);
  MedFunc Maybe = function(0x3000, "maybe");
  std::vector<MedFunc> Functions{Main, Maybe};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  EXPECT_FALSE(Result.GraphComplete);
  EXPECT_EQ(Result.findFunction(Maybe.Entry)->Status,
            ReachabilityStatus::Unknown);
}

TEST(InterprocReachability, UnmodelledDirectTailTransferFailsClosed) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main");
  Main.Blocks.emplace_back();
  Main.Blocks.back().Id = 0;
  Main.Blocks.back().StartAddr = Main.Entry;
  MedOp Tail;
  Tail.Opcode = NdOp::BRANCH;
  Tail.Addr = 0x1010;
  Tail.addInput(MedVar::makeConst(0x3000, 8));
  Main.Blocks.back().Ops.push_back(Tail);
  MedFunc Maybe = function(0x3000, "maybe");
  std::vector<MedFunc> Functions{Main, Maybe};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  EXPECT_FALSE(Result.GraphComplete);
  EXPECT_EQ(Result.findFunction(Maybe.Entry)->Status,
            ReachabilityStatus::Unknown);
}

TEST(InterprocReachability, NoReturnCallBlocksLaterCallInSameBlock) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main");
  sourceCall(Main, 0x1010, "abort", temporary(1));
  Main.Blocks.front().Ops.front().DoesNotReturn = true;
  call(Main, 0x1020, 0x3000);
  MedFunc Dead = function(0x3000, "after_abort");
  std::vector<MedFunc> Functions{Main, Dead};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  EXPECT_EQ(Result.findFunction(Dead.Entry)->Status,
            ReachabilityStatus::Unreachable);
}

TEST(InterprocReachability, BranchTerminatorBlocksLaterCallInSameBlock) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main");
  Main.Blocks.emplace_back();
  Main.Blocks.back().Id = 0;
  Main.Blocks.back().StartAddr = Main.Entry;
  MedOp Branch;
  Branch.Opcode = NdOp::BRANCH;
  Main.Blocks[0].Ops.push_back(Branch);
  call(Main, 0x1010, 0x3000);
  Main.Blocks.emplace_back();
  Main.Blocks.back().Id = 1;
  Main.Blocks.back().StartAddr = 0x1100;
  Main.Blocks[0].Succs = {1};
  Main.Blocks[1].Preds = {0};
  MedFunc Dead = function(0x3000, "after_branch");
  std::vector<MedFunc> Functions{Main, Dead};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  EXPECT_EQ(Result.findFunction(Dead.Entry)->Status,
            ReachabilityStatus::Unreachable);
}

TEST(InterprocReachability, ZeroVirtualAddressImageEntryIsRepresentable) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.Entry = 0;
  MedFunc Start = function(0, "start");
  std::vector<MedFunc> Functions{Start};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  const FunctionReachability *Reach = Result.findFunction(0);
  ASSERT_NE(Reach, nullptr);
  EXPECT_EQ(Reach->Status, ReachabilityStatus::Reachable);
  ASSERT_TRUE(Reach->EntryVA.has_value());
  EXPECT_EQ(*Reach->EntryVA, 0u);
  EXPECT_EQ(Reach->Kind, SafetyEntryKind::Image);
}

TEST(InterprocReachability, ProvesDisconnectedFunctionUnreachable) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main");
  ret(Main);
  std::vector<MedFunc> Functions{Main, function(0x3000, "dead_helper")};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  const FunctionReachability *Reach = Result.findFunction(0x3000);
  ASSERT_NE(Reach, nullptr);
  EXPECT_EQ(Reach->Status, ReachabilityStatus::Unreachable);
  EXPECT_EQ(Reach->Reason, "no path from a known entry");
}

TEST(InterprocReachability, TreatsAuthenticatedExportAsKnownEntry) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Exports.push_back({"public_api", 1, 0x4000});
  MedFunc Public = function(0x4000, "public_api", {parameter(3)});
  std::vector<MedFunc> Functions{Public};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  const FunctionReachability *Reach = Result.findFunction(Public.Entry);
  ASSERT_NE(Reach, nullptr);
  EXPECT_EQ(Reach->Status, ReachabilityStatus::Reachable);
  EXPECT_EQ(Reach->Kind, SafetyEntryKind::Export);
  EXPECT_EQ(Reach->EntryName, "public_api");
  const ParameterFlow *Flow = Result.findParameter(Public.Entry, 0);
  EXPECT_EQ(Flow, nullptr);
}

TEST(InterprocReachability, UnknownCallingConventionDoesNotPropagateControl) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main", {parameter(0, 4), parameter(1)});
  MedFunc Wrapper = function(0x2000, "wrapper", {parameter(7)});
  Wrapper.CC = CallingConv::Unknown;
  call(Main, 0x1010, Wrapper.Entry, {Main.Params[1]});
  std::vector<MedFunc> Functions{Main, Wrapper};

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), SafetyBudgets{});
  EXPECT_EQ(Result.findFunction(Wrapper.Entry)->Status,
            ReachabilityStatus::Reachable);
  EXPECT_EQ(Result.findParameter(Wrapper.Entry, 0), nullptr);
}

TEST(InterprocReachability, DepthExhaustionIsUnknownNotUnreachable) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Entry = 0x1000;
  MedFunc Main = function(0x1000, "main");
  MedFunc Middle = function(0x2000, "middle");
  MedFunc Leaf = function(0x3000, "leaf");
  call(Main, 0x1010, Middle.Entry);
  call(Middle, 0x2010, Leaf.Entry);
  std::vector<MedFunc> Functions{Main, Middle, Leaf};
  SafetyBudgets Budgets;
  Budgets.MaxCallDepth = 1;

  InterprocResult Result = analyzeInterprocedural(
      input(Image, Functions), SinkCatalog::defaults(), Budgets);
  const FunctionReachability *Reach = Result.findFunction(Leaf.Entry);
  ASSERT_NE(Reach, nullptr);
  EXPECT_EQ(Reach->Status, ReachabilityStatus::Unknown);
  EXPECT_TRUE(Reach->BudgetHit);
  EXPECT_EQ(Reach->Reason, "interprocedural call-depth budget exhausted");
}
