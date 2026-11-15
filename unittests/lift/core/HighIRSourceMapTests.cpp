//===- HighIRSourceMapTests.cpp - HighIR navigation ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Surviving identity, multi-output relationships and structured row ownership.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/HighIRPrint.h"
#include "neverd/ir/high/HighSourceMap.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/Support/raw_ostream.h"

#include <set>

using namespace neverd;

TEST(HighIRSources, PhiSliceComputationsRetainTheirOwnOrigins) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    BinaryImage Image;
    Image.Arch = Architecture;
    Image.Format = BinaryFormat::ELF;
    PipelineResult Result;
    auto &Function = Result.MedFuncs.emplace_back();
    Function.Entry = 0x1000;
    Function.Name = "computed_phi_byte";
    Function.ReturnType = NdType::makeInt(1, false);
    auto Value = [&](int Id, uint16_t Bytes, bool Parameter = false) {
      MedVar V;
      V.Kind = Parameter ? MedVar::Param : MedVar::Temp;
      V.TheArch = Architecture;
      V.Id = Id;
      V.Size = Bytes;
      V.RegOff = kNoParamReg;
      return V;
    };
    const auto Input = Value(0, 4, true), Choice = Value(1, 1, true);
    Function.Params = {Input, Choice};
    Function.SourceParametersBound = true;
    Function.Blocks.resize(4);
    for (int I = 0; I < 4; ++I) {
      auto &Block = Function.Blocks[I];
      Block.Id = I;
      Block.StartAddr = 0x1000 + I * 0x10;
      Block.EndAddr = Block.StartAddr + 8;
    }
    auto Add = [&](int Block, NdOp Opcode, va_t Address, MedVar Output,
                   std::initializer_list<MedVar> Inputs) {
      MedOp Op;
      Op.Opcode = Opcode;
      Op.Addr = Address;
      Op.OriginSeq = 0;
      Op.Output = Output;
      for (const auto &Input : Inputs)
        Op.addInput(Input);
      Function.Blocks[Block].Ops.push_back(Op);
    };
    Function.Blocks[0].Succs = {1, 2};
    Add(0, NdOp::COND_BR, 0x1000, {}, {MedVar::makeConst(0x1010, 8), Choice});
    for (int I : {1, 2}) {
      Function.Blocks[I].Preds = {0};
      Function.Blocks[I].Succs = {3};
      Add(I, I == 1 ? NdOp::INT_ADD : NdOp::INT_SUB, 0x1000 + I * 0x10,
          Value(10 + I, 4), {Input, MedVar::makeConst(I == 1 ? 13 : 29, 4)});
      Add(I, NdOp::BRANCH, 0x1004 + I * 0x10, {},
          {MedVar::makeConst(0x1030, 8)});
    }
    Function.Blocks[3].Preds = {1, 2};
    Function.Blocks[3].Phis.push_back(
        {Value(20, 4), {{1, Value(11, 4)}, {2, Value(12, 4)}}});
    auto ReturnValue = Value(21, 1);
    ReturnValue.Kind = MedVar::Reg;
    ReturnValue.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
    Add(3, NdOp::SUBBYTES, 0x1030, ReturnValue,
        {Value(20, 4), MedVar::makeConst(0, 4)});
    Add(3, NdOp::RETURN, 0x1034, {}, {ReturnValue});
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.SymbolicHighC = false;
    Pipeline().rebuildHighIR(Image, Options, Result);
    ASSERT_EQ(Result.HighFuncs.size(), 1u);
    ASSERT_TRUE(Result.HighFuncs[0].ConversionFailure.empty());
    CEmitterOptions COptions;
    COptions.TheArch = Architecture;
    COptions.Format = Image.Format;
    std::string Ordinary, Mapped;
    llvm::raw_string_ostream Plain(Ordinary), Out(Mapped);
    ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Plain, COptions));
    CSourceMap Map;
    Map.HighSources = &Result.HighSources;
    COptions.SourceMap = &Map;
    ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, COptions));
    ASSERT_EQ(Ordinary, Mapped);
    ASSERT_EQ(Mapped.find("__builtin_trap"), std::string::npos) << Mapped;
    for (va_t Address : {0x1010, 0x1020}) {
      bool Found = false;
      for (const auto &Anchor : Map.Anchors)
        for (const auto &Occurrence : Anchor.Occurrences)
          if (Occurrence.Address == Address) {
            const auto Text = llvm::StringRef(Mapped).slice(Anchor.Span.Begin,
                                                            Anchor.Span.End);
            EXPECT_TRUE(Text.contains(Address == 0x1010 ? "+" : "-"))
                << Text.str();
            Found = true;
          }
      EXPECT_TRUE(Found) << Address << '\n' << Mapped;
    }
  }
}

TEST(HighIRSources, LargeStatementStreamsKeepEveryNavigationOccurrence) {
  // Independent input larger than the former expression-observation cap.
  constexpr unsigned Stores = 100019;
  for (Arch Architecture : {Arch::X64, Arch::AArch64})
    for (bool Recognized : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Recognized);
      BinaryImage Image;
      Image.Arch = Architecture;
      Image.Format = BinaryFormat::ELF;
      PipelineResult Result;
      auto &Function = Result.MedFuncs.emplace_back();
      Function.Entry = 0x1000;
      Function.Name = "store_sequence";
      Function.ReturnType = NdType::makeVoid();
      Function.ReturnsNoValue = true;
      if (Recognized)
        Result.LibraryRecognitions.emplace_back().Function = Function.Entry;
      auto &Block = Function.Blocks.emplace_back();
      Block.Id = 0;
      Block.StartAddr = Function.Entry;
      Block.Ops.reserve(Stores + 1);
      for (unsigned I = 0; I < Stores; ++I) {
        MedOp Store;
        Store.Opcode = NdOp::STORE;
        Store.Addr = Function.Entry + I * 4;
        Store.OriginSeq = 0;
        Store.addInput(MedVar::makeConst(0x400000 + uint64_t(I) * 64, 8));
        Store.addInput(MedVar::makeConst(I + 7, 8));
        Block.Ops.push_back(Store);
      }
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.Addr = Function.Entry + Stores * 4;
      Return.OriginSeq = 0;
      Block.Ops.push_back(Return);
      Block.EndAddr = Return.Addr + 4;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.SymbolicHighC = false;
      Pipeline().rebuildHighIR(Image, Options, Result);
      ASSERT_EQ(Result.HighFuncs.size(), 1u);
      ASSERT_TRUE(Result.HighFuncs[0].ConversionFailure.empty());
      ASSERT_GE(Result.HighSources.size(), Stores);
      const HighStatementSourceIndex Index(Result.HighSources);
      unsigned Observed = 0;
      walkStmts(Result.HighFuncs[0].Body, [&](const HighStmt &Statement) {
        if (Statement.Kind != StmtKind::Store)
          return;
        const auto Origin = Index.lookup(Function.Entry, Statement);
        ASSERT_TRUE(Origin) << Statement.Addr;
        EXPECT_EQ(Origin->Address, Statement.Addr);
        EXPECT_EQ(Origin->Sequence, 0);
        ++Observed;
      });
      EXPECT_EQ(Observed, Stores);
      CSourceMap Map;
      Map.HighSources = &Result.HighSources;
      CEmitterOptions COptions;
      COptions.TheArch = Architecture;
      COptions.Format = Image.Format;
      COptions.SourceMap = &Map;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, COptions));
      std::set<va_t> Addresses;
      for (const auto &Anchor : Map.Anchors) {
        EXPECT_LT(Anchor.Span.Begin, Anchor.Span.End);
        EXPECT_LE(Anchor.Span.End, Source.size());
        for (const auto &Origin : Anchor.Occurrences)
          Addresses.insert(Origin.Address);
      }
      for (unsigned I = 0; I < Stores; ++I)
        ASSERT_TRUE(Addresses.contains(Function.Entry + I * 4)) << I;
    }
}

TEST(HighIRSources, SameInstructionStatementsHaveIndependentSurvivingIdentity) {
  HighStmt Call;
  Call.Kind = StmtKind::Assign;
  Call.Addr = 0x1010;
  Call.Val = HighExpr::makeCall("query", 0, {});
  HighStmt Transport = Call;
  Transport.Val = HighExpr::makeConst(7, 4);
  const HighSourceMap Sources = {
      observeHighStatement(0x1000, {Call.Addr, 3}, Call),
      observeHighStatement(0x1000, {Call.Addr, 4}, Transport)};
  HighStatementSourceIndex Index(Sources);
  ASSERT_TRUE(Index.lookup(0x1000, Call));
  EXPECT_EQ(Index.lookup(0x1000, Call)->Sequence, 3);
  EXPECT_EQ(Index.lookup(0x1000, Transport)->Sequence, 4);
  Call.Kind = StmtKind::Call;
  Call.CallExpr = Call.Val;
  Call.Val.reset();
  EXPECT_EQ(Index.lookup(0x1000, Call)->Sequence, 3);
  EXPECT_FALSE(Index.lookup(0x2000, Call));
  Call.IsPhiCopy = true;
  EXPECT_FALSE(Index.lookup(0x1000, Call));
}

TEST(HighIRSources, PairedStoresKeepTheirIndependentInstructionOrigins) {
  HighStmt First;
  First.Kind = StmtKind::Store;
  First.Addr = 0x1010;
  First.StoreAddr = HighExpr::makeConst(0x2000, 8);
  First.StoreVal = HighExpr::makeConst(7, 8);
  HighStmt Second = First;
  Second.StoreAddr = HighExpr::makeConst(0x2008, 8);
  const HighSourceMap Sources = {
      observeHighStatement(0x1000, {First.Addr, 3}, First),
      observeHighStatement(0x1000, {Second.Addr, 4}, Second)};
  HighStatementSourceIndex Index(Sources);
  ASSERT_TRUE(Index.lookup(0x1000, First));
  ASSERT_TRUE(Index.lookup(0x1000, Second));
  EXPECT_EQ(Index.lookup(0x1000, First)->Sequence, 3);
  EXPECT_EQ(Index.lookup(0x1000, Second)->Sequence, 4);
  First.StoreAddr = HighExpr::makeConst(0x3000, 8);
  EXPECT_FALSE(Index.lookup(0x1000, First));
  Second.Kind = StmtKind::Assign;
  EXPECT_FALSE(Index.lookup(0x1000, Second));
}

TEST(HighIRSources, StoreValuesSurviveAddressSimplificationWithoutGuessing) {
  HighStmt First;
  First.Kind = StmtKind::Store;
  First.Addr = 0x1010;
  First.StoreAddr = HighExpr::makeConst(0x2000, 8);
  First.StoreVal = HighExpr::makeConst(7, 8);
  HighStmt Second = First;
  Second.StoreAddr = HighExpr::makeConst(0x2008, 8);
  Second.StoreVal = HighExpr::makeConst(11, 8);
  HighSourceMap Sources;
  int Sequence = 3;
  for (const auto *S : {&First, &Second}) {
    auto O = observeHighStatement(0x1000, {S->Addr, Sequence++}, *S);
    Sources.push_back(O);
    O.Kind = HighSourceKind::StoreValueExpression;
    O.Expression = S->StoreVal;
    O.ExpressionKind = static_cast<uint8_t>(S->StoreVal->Kind);
    Sources.push_back(O);
  }
  const HighStatementSourceIndex Index(Sources);
  Second.StoreAddr = HighExpr::makeConst(0x2008, 8);
  ASSERT_TRUE(Index.lookup(0x1000, Second));
  EXPECT_EQ(Index.lookup(0x1000, Second)->Sequence, 4);
  // Address and value witnesses from different stores cannot be combined.
  Second.StoreAddr = First.StoreAddr;
  EXPECT_FALSE(Index.lookup(0x1000, Second));
  Second.StoreAddr = HighExpr::makeConst(0x2008, 8);
  Second.StoreVal = HighExpr::makeConst(11, 8);
  EXPECT_FALSE(Index.lookup(0x1000, Second));
}

TEST(HighIRSources, ReplacedExpiredChangedKindAndAmbiguousExpressionsNeverMap) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Addr = 0x1020;
  S.Val = HighExpr::makeCall("query", 0, {});
  auto Source = observeHighStatement(0x1000, {S.Addr, 1}, S);
  HighStatementSourceIndex Index({Source});
  const auto Original = S.Val;
  S.Val = HighExpr::makeCall("query", 0, {});
  EXPECT_FALSE(Index.lookup(0x1000, S));
  S.Val = Original;
  S.Val->Kind = ExprKind::Const;
  EXPECT_FALSE(Index.lookup(0x1000, S));
  S.Val->Kind = ExprKind::Call;
  auto Other = Source;
  Other.Occurrence.Sequence = 2;
  EXPECT_FALSE(HighStatementSourceIndex({Source, Other}).lookup(0x1000, S));
  S.Val.reset();
  EXPECT_FALSE(Index.lookup(0x1000, S));
  auto Expired = Source;
  {
    S.Val = HighExpr::makeCall("other_query", 0, {});
    Expired = observeHighStatement(0x1000, {S.Addr, 3}, S);
    S.Val.reset();
  }
  ASSERT_TRUE(Expired.Expression.expired());
  S.Val = HighExpr::makeCall("other_query", 0, {});
  EXPECT_FALSE(HighStatementSourceIndex({Expired}).lookup(0x1000, S));
}

TEST(HighIRSources, StructuredBranchesKeepTheirOriginalPredicate) {
  MedVar Input;
  Input.Kind = MedVar::Param;
  Input.Size = 1;
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Addr = 0x1010;
  Branch.Cond = HighExpr::makeVar(Input);
  const auto Predicate = Branch.Cond;
  const HighStatementSourceIndex Index(
      {observeHighStatement(0x1000, {0x1010, 2}, Branch)});
  for (const auto Kind :
       {StmtKind::IfElse, StmtKind::While, StmtKind::DoWhile}) {
    Branch.Kind = Kind;
    for (bool Inverted : {false, true}) {
      Branch.Cond =
          Inverted ? HighExpr::makeUnary(NdOp::BOOL_NOT, Predicate) : Predicate;
      ASSERT_TRUE(Index.lookup(0x1000, Branch));
      EXPECT_EQ(Index.lookup(0x1000, Branch)->Sequence, 2);
    }
    Branch.Cond = HighExpr::makeConst(1, 1);
    EXPECT_FALSE(Index.lookup(0x1000, Branch));
  }
}

TEST(HighIRSources, ComputationOriginsRequireSurvivingIdentityAcrossViews) {
  MedVar Input;
  Input.Kind = MedVar::Param;
  Input.Size = 4;
  const auto Sum = HighExpr::makeBinop(NdOp::INT_ADD, HighExpr::makeVar(Input),
                                       HighExpr::makeConst(13, 4));
  const auto Observation = observeHighComputation(0x1000, {0x1010, 2}, Sum);
  ASSERT_TRUE(Observation);
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Addr = 0x1084; // The edge's branch is not the calculation's address.
  S.IsPhiCopy = true;
  S.Val = Sum;
  HighStatementSourceIndex Index({*Observation});
  EXPECT_FALSE(Index.lookup(0x1000, S));
  ASSERT_TRUE(Index.lookupComputation(0x1000, S));
  EXPECT_EQ(Index.lookupComputation(0x1000, S)->Address, 0x1010u);
  EXPECT_FALSE(Index.lookupComputation(0x2000, S));
  const auto Slice =
      HighExpr::makeBinop(NdOp::SUBBYTES, Sum, HighExpr::makeConst(0, 4));
  Slice->Type = NdType::makeInt(1, false);
  S.Val = HighExpr::makeUnary(NdOp::INT_ZEXT, Slice);
  S.Val->Type = NdType::makeInt(4, false);
  ASSERT_TRUE(Index.lookupComputation(0x1000, S));
  EXPECT_EQ(Index.lookupComputation(0x1000, S)->Sequence, 2);
  // An unrelated arithmetic parent cannot borrow its operand's origin.
  S.Val = HighExpr::makeBinop(NdOp::INT_XOR, Sum, HighExpr::makeConst(7, 4));
  EXPECT_FALSE(Index.lookupComputation(0x1000, S));
  S.Val = std::make_shared<HighExpr>(*Sum);
  EXPECT_FALSE(Index.lookupComputation(0x1000, S));
  S.Val = Sum;
  Sum->Kind = ExprKind::Const;
  EXPECT_FALSE(Index.lookupComputation(0x1000, S));
  Sum->Kind = ExprKind::BinOp;
  auto Other = *Observation;
  Other.Occurrence.Address = 0x1020;
  EXPECT_FALSE(HighStatementSourceIndex({*Observation, Other})
                   .lookupComputation(0x1000, S));
  Other.Occurrence = Observation->Occurrence;
  EXPECT_TRUE(HighStatementSourceIndex({*Observation, Other})
                  .lookupComputation(0x1000, S));
  for (auto Occurrence : {sigs::LibraryOccurrence{InvalidVA, 2},
                          sigs::LibraryOccurrence{0x1010, -1}}) {
    Other.Occurrence = Occurrence;
    EXPECT_FALSE(
        HighStatementSourceIndex({Other}).lookupComputation(0x1000, S));
  }
  Other = *Observation;
  {
    auto Expired = std::make_shared<HighExpr>(*Sum);
    Other.Expression = Expired;
  }
  ASSERT_TRUE(Other.Expression.expired());
  EXPECT_FALSE(HighStatementSourceIndex({Other}).lookupComputation(0x1000, S));
  for (const auto &Leaf : {HighExpr::makeConst(7, 4), HighExpr::makeVar(Input)})
    EXPECT_FALSE(observeHighComputation(0x1000, {0x1010, 2}, Leaf));
}

TEST(HighIRSources, ConvertedCallResultsKeepTheirCallInstruction) {
  const auto Call = HighExpr::makeCall("external", 0x2000, {});
  Call->Type = NdType::makeFloat(4);
  const auto Origin = observeHighComputation(0x1000, {0x1020, 3}, Call);
  ASSERT_TRUE(Origin);
  const HighStatementSourceIndex Index({*Origin});
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Val = HighExpr::makeBitCast(Call, NdType::makeInt(4));
  ASSERT_TRUE(Index.lookupComputation(0x1000, S));
  EXPECT_EQ(Index.lookupComputation(0x1000, S)->Address, 0x1020u);
  S.Kind = StmtKind::If;
  S.Cond = Call;
  ASSERT_TRUE(Index.lookupComputation(0x1000, S));
  S.Cond = HighExpr::makeCall("external", 0x2000, {});
  EXPECT_FALSE(Index.lookupComputation(0x1000, S));
}

TEST(HighIRSources, CpuidPrintsAllOutputBindingsAndComparesTheirIdentity) {
  auto Call = HighExpr::makeCall(
      "__cpuid", 0, {HighExpr::makeConst(7, 4), HighExpr::makeConst(1, 4)});
  Call->IntrinsicId = Intrinsic::Cpuid;
  for (int I = 0; I < 4; ++I) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = 10 + I;
    V.Size = 4;
    Call->IntrinsicOutputs.push_back(V);
  }
  const auto Text = Call->str();
  EXPECT_NE(Text.find(" -> ("), std::string::npos);
  for (const auto &V : Call->IntrinsicOutputs)
    EXPECT_NE(Text.find(V.display()), std::string::npos);
  HighExpr Other = *Call;
  EXPECT_TRUE(Call->structuralEq(Other));
  Other.IntrinsicOutputs[2].Id += 10;
  EXPECT_FALSE(Call->structuralEq(Other));
}

TEST(HighIRSources, StructuredRowsKeepChildrenAndExcludeBracesAndLabels) {
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = HighExpr::makeConst(1, 1);
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.CallExpr = HighExpr::makeCall("query", 0, {});
  Branch.Body = {Call};
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(9, 4);
  Branch.ElseBody = {Return};
  std::vector<const HighStmt *> Owners;
  std::string Text;
  printHighStatementRows(Branch, 0,
                         [&](llvm::StringRef Row, const HighStmt *Owner) {
                           Text += Row.str();
                           Owners.push_back(Owner);
                         });
  EXPECT_EQ(Text, "if (1) {\n  query();\n} else {\n  return 9;\n}\n");
  ASSERT_EQ(Owners.size(), 5u);
  EXPECT_EQ(Owners[0], &Branch);
  EXPECT_EQ(Owners[1], &Branch.Body[0]);
  EXPECT_EQ(Owners[2], nullptr);
  EXPECT_EQ(Owners[3], &Branch.ElseBody[0]);
  EXPECT_EQ(Owners[4], nullptr);
}
