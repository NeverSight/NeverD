//===- BinaryDiffParameterViewTests.cpp - Native low-bit observations
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Positive correspondence and fail-closed parameter provenance, resource and
/// saved-evidence checks. These fixtures do not execute compared programs.
//===----------------------------------------------------------------------===//

#include "neverd/backend/ParameterSites.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/diff/DataFlowJSON.h"
#include "neverd/diff/FlowFragments.h"
#include "neverd/diff/FlowMatch.h"
#include "neverd/diff/LLVMDataFlow.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include <atomic>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::diff;
namespace {
using Edit = llvm::function_ref<void(llvm::Function &, LLVMSourceMap &)>;
DataFlowGraph graph(llvm::StringRef Text, Edit Change = {},
                    const DataFlowOptions &Options = {},
                    const ComparisonControl &Control = {},
                    bool Legacy = false) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(Text, Diagnostic, Context);
  if (!Module)
    throw std::runtime_error(Diagnostic.getMessage().str());
  auto &F = *Module->getFunction("f");
  std::vector<parameter_sites::Site> Sites;
  for (const auto &A : F.args())
    Sites.push_back({0x10 + A.getArgNo() * 8, int64_t(A.getArgNo())});
  parameter_sites::setParameterSites(F, Sites);
  LLVMSourceMap Sources;
  Sources.Functions[0x1000] = &F;
  uint64_t Address = 0x1000;
  for (auto &B : F)
    for (auto &I : B) {
      if (!llvm::isa<llvm::TruncInst>(I))
        Sources.Observations.push_back({0x1000, {Address, 0}, &I});
      Address += 4;
    }
  if (Change)
    Change(F, Sources);
  const auto Build = Legacy ? buildLLVMDataFlow : buildLLVMParameterDataFlow;
  return Build(F, Sources, 0x1000, "owned", "same-abi", true, Options, Control);
}
llvm::json::Value json(const DataFlowGraph &G) {
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  llvm::json::OStream Out(Stream);
  writeDataFlowGraph(Out, G);
  auto Parsed = llvm::json::parse(Text);
  if (!Parsed)
    throw std::runtime_error(llvm::toString(Parsed.takeError()));
  return std::move(*Parsed);
}
FlowNeighborhoodMatch match(const DataFlowGraph &A, const DataFlowGraph &B) {
  DataFlowOptions O;
  O.Rounds = 16;
  return matchDataFlowNeighborhood(
      A, A.Nodes.size() - 1, fingerprintDataFlow(A, O), B, B.Nodes.size() - 1,
      fingerprintDataFlow(B, O), O);
}
constexpr llvm::StringLiteral Narrow = R"(
define i32 @f(i32 %x) {
 %a = add i32 %x, 23
 %b = mul i32 %a, 7
 %c = xor i32 %b, 31
 ret i32 %c
})";
constexpr llvm::StringLiteral Wide = R"(
define i32 @f(i64 %raw) {
 %x = trunc i64 %raw to i32
 %a = add i32 %x, 23
 %b = mul i32 %a, 7
 %c = xor i32 %b, 31
 ret i32 %c
})";
} // namespace

TEST(BinaryDiffParameterViews, CorrespondenceRetainsSourceAndExactLowBits) {
  const auto A = graph(Narrow), B = graph(Wide);
  ASSERT_EQ(B.ParameterViews.size(), 1u);
  const auto &V = B.ParameterViews[0];
  EXPECT_EQ(V.SourceBits, 64u);
  EXPECT_EQ(V.ViewBits, 32u);
  EXPECT_EQ(V.ElidedTruncations, 1u);
  ASSERT_TRUE(V.NativeSite);
  EXPECT_EQ(V.NativeSite->Register, 0x10u);
  EXPECT_EQ(V.NativeSite->Index, 0);
  EXPECT_EQ(A.Nodes.size(), 4u);
  EXPECT_EQ(B.Nodes.size(), 4u);
  EXPECT_EQ(B.retainedItems(), 5u);
  EXPECT_EQ(match(A, B).Outcome, FlowMatchOutcome::Matched);
  const auto AK = fingerprintFlowFragments(A), BK = fingerprintFlowFragments(B);
  EXPECT_EQ(matchFlowFragment(FlowFragmentInput(A, AK), 3,
                              FlowFragmentInput(B, BK), 3, 4)
                .Outcome,
            FlowMatchOutcome::Matched);
  EXPECT_EQ(
      match(graph(Narrow, {}, {}, {}, true), graph(Wide, {}, {}, {}, true))
          .Outcome,
      FlowMatchOutcome::Different);
  EXPECT_EQ(readDataFlowGraph(json(B)), B);
  const auto Page = dataFlowPageJSON(B, false, 0, 1);
  EXPECT_EQ(Page.getInteger("schema_version"), 4);
  EXPECT_EQ(Page.getInteger("parameter_count"), 1);
  EXPECT_EQ(Page.getInteger("projected_parameter_count"), 1);
  EXPECT_FALSE(Page.get("parameter_views"));
  const auto &Row = *Page.getArray("rows")->front().getAsObject();
  ASSERT_EQ(Row.getArray("parameter_views")->size(), 1u);
  EXPECT_EQ(Row.getArray("parameter_views")
                ->front()
                .getAsObject()
                ->getInteger("source_bits"),
            64);
  EXPECT_TRUE(dataFlowNodeJSON(B, 1).getArray("parameter_views")->empty());
  for (const auto &N : B.Nodes)
    ASSERT_EQ(N.Origins.size(), 1u);
}

TEST(BinaryDiffParameterViews, EveryUseMustQualifyForEachIndependentArgument) {
  const auto Body = [](llvm::StringRef Bad) {
    return "define i32 @f(i64 %x, i64 %y) {\n"
           "%a = trunc i64 %x to i32\n" +
           Bad.str() +
           "\n%c = trunc i64 %y to i32\n%d = add i32 %a, %c\nret i32 %d\n}";
  };
  for (const auto *Bad :
       {"%b = trunc nuw i64 %x to i32", "%b = trunc nsw i64 %x to i32",
        "%b = trunc i64 %x to i16", "%b = lshr i64 %x, 32",
        "%b = freeze i64 %x"}) {
    const auto G = graph(Body(Bad));
    EXPECT_EQ(G.ParameterViews[0].ElidedTruncations, 0u) << Bad;
    EXPECT_EQ(G.ParameterViews[1].ElidedTruncations, 1u) << Bad;
    EXPECT_EQ(G.Nodes.size(), 4u);
  }
  const auto Origin =
      graph(Body("%b = trunc i64 %x to i32"), [](auto &F, auto &Sources) {
        for (auto &I : F.getEntryBlock())
          if (I.getName() == "b")
            Sources.Observations.push_back({0x1000, {0x1010, 0}, &I});
      });
  EXPECT_EQ(Origin.ParameterViews[0].ElidedTruncations, 0u);
  EXPECT_EQ(Origin.ParameterViews[1].ElidedTruncations, 1u);
  EXPECT_EQ(Origin.Nodes.size(), 4u);
  EXPECT_EQ(Origin.Nodes[1].Origins.size(), 1u);
}

TEST(BinaryDiffParameterViews, PhiSubstitutionKeepsPredecessorsAndArmMeaning) {
  const auto A = graph(R"(
define i32 @f(i32 %x, i32 %y, i1 %c) {
entry: br i1 %c, label %a, label %b
a: br label %exit
b: br label %exit
exit: %v = phi i32 [%x, %a], [%y, %b]
 ret i32 %v
})");
  const std::string Text = R"(
define i32 @f(i64 %x, i64 %y, i1 %c) {
entry: br i1 %c, label %a, label %b
a: %ax = trunc i64 %x to i32
 %ay = trunc i64 %y to i32
 br label %exit
b: %bx = trunc i64 %x to i32
 %by = trunc i64 %y to i32
 br label %exit
exit: %v = phi i32 [%ax, %a], [%by, %b]
 ret i32 %v
})";
  const auto B = graph(Text);
  ASSERT_EQ(B.Nodes.size(), 5u);
  ASSERT_TRUE(B.Nodes[3].Phi);
  EXPECT_EQ(B.Nodes[3].Inputs[0].Predecessor, 1u);
  EXPECT_EQ(B.Nodes[3].Inputs[1].Predecessor, 2u);
  EXPECT_EQ(B.ParameterViews[0].ElidedTruncations, 2u);
  EXPECT_EQ(B.ParameterViews[1].ElidedTruncations, 2u);
  EXPECT_EQ(match(A, B).Outcome, FlowMatchOutcome::Matched);
  auto Changed = Text;
  Changed.replace(Changed.find("[%ax, %a], [%by, %b]"),
                  std::string("[%ax, %a], [%by, %b]").size(),
                  "[%ay, %a], [%bx, %b]");
  EXPECT_NE(match(A, graph(Changed)).Outcome, FlowMatchOutcome::Matched);
}

TEST(BinaryDiffParameterViews, MissingStackOverlappingAndInvalidSitesStayRaw) {
  const auto Missing = graph(Wide, [](auto &F, auto &) {
    F.setMetadata(parameter_sites::FunctionAttachment, nullptr);
  });
  EXPECT_FALSE(Missing.ParameterViews[0].NativeSite);
  EXPECT_EQ(Missing.Nodes.size(), 5u);
  for (const auto S :
       {parameter_sites::Site{UINT64_MAX, 0}, parameter_sites::Site{0x10, -1},
        parameter_sites::Site{UINT64_MAX - 4, 0}}) {
    const auto G = graph(Wide, [&](auto &F, auto &) {
      parameter_sites::setParameterSites(F, {S});
    });
    EXPECT_EQ(G.Nodes.size(), 5u);
    ASSERT_TRUE(G.ParameterViews[0].NativeSite);
    EXPECT_EQ(readDataFlowGraph(json(G)), G);
  }
  const auto Body = "define i32 @f(i64 %x, i64 %y) {"
                    "%a = trunc i64 %x to i32 ret i32 %a }";
  for (const auto S :
       {parameter_sites::Site{0x10, 1}, parameter_sites::Site{0x14, 1},
        parameter_sites::Site{0xc, 1}, parameter_sites::Site{0x20, 0}}) {
    const auto G = graph(Body, [&](auto &F, auto &) {
      parameter_sites::setParameterSites(F, {{0x10, 0}, S});
    });
    EXPECT_EQ(G.ParameterViews[0].ElidedTruncations, 0u);
    EXPECT_EQ(G.Nodes.size(), 2u);
    EXPECT_EQ(readDataFlowGraph(json(G)), G);
  }
  const auto Adjacent = graph(Body);
  EXPECT_EQ(Adjacent.ParameterViews[0].ElidedTruncations, 1u);
  EXPECT_EQ(Adjacent.ParameterViews[1].ElidedTruncations, 0u);
}

TEST(BinaryDiffParameterViews,
     ExactAttributesConstantsAndIdentityRemainDistinct) {
  const auto A = graph(Narrow);
  for (const auto &[From, To] :
       {std::pair{"i64 %raw)", "i64 noundef %raw)"}, std::pair{"23", "25"},
        std::pair{"add i32", "add nuw i32"},
        std::pair{"%raw) {", "%raw) mustprogress {"}}) {
    auto Text = Wide.str();
    Text.replace(Text.find(From), std::string(From).size(), To);
    EXPECT_NE(match(A, graph(Text)).Outcome, FlowMatchOutcome::Matched);
  }
  for (const auto S : {parameter_sites::Site{0x20, 0}}) {
    const auto B = graph(Wide, [&](auto &F, auto &) {
      parameter_sites::setParameterSites(F, {S});
    });
    EXPECT_NE(match(A, B).Outcome, FlowMatchOutcome::Matched);
  }
  const auto Renumbered = graph(Wide, [](auto &F, auto &) {
    parameter_sites::setParameterSites(F, {{0x10, 19}});
  });
  EXPECT_EQ(match(A, Renumbered).Outcome, FlowMatchOutcome::Matched);
  EXPECT_EQ(Renumbered.ParameterViews[0].NativeSite->Index, 19);
  const auto Unused = graph("define i32 @f(i64 %raw) { ret i32 0 }");
  EXPECT_EQ(Unused.ParameterViews[0].ViewBits, 64u);
  EXPECT_EQ(Unused.ParameterViews[0].ElidedTruncations, 0u);
}

TEST(BinaryDiffParameterViews, ElisionCannotHideUnsupportedMetadataOrOrigins) {
  auto Text = Wide.str();
  Text.replace(Text.find("to i32"), 6, "to i32, !unknown !0");
  Text += "\n!0 = !{i32 1}";
  EXPECT_THROW(graph(Text), LLVMDataFlowUnsupported);
  EXPECT_THROW(graph(Wide,
                     [](auto &F, auto &Sources) {
                       Sources.Observations.push_back(
                           {0x1000, {0x1000, -1}, &F.front().front()});
                     }),
               LLVMDataFlowUnsupported);
  EXPECT_THROW(graph(Wide,
                     [](auto &F, auto &) {
                       F.setMetadata(parameter_sites::FunctionAttachment,
                                     llvm::MDNode::get(F.getContext(), {}));
                     }),
               LLVMDataFlowUnsupported);
}

TEST(BinaryDiffParameterViews, DetachedAndForeignUsersCannotBeProjected) {
  for (bool Foreign : {false, true}) {
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    auto Module = llvm::parseAssemblyString(Wide, Diagnostic, Context);
    ASSERT_TRUE(Module);
    auto &F = *Module->getFunction("f");
    parameter_sites::setParameterSites(F, {{0x10, 0}});
    LLVMSourceMap Sources;
    Sources.Functions[0x1000] = &F;
    auto *Extra =
        new llvm::TruncInst(F.getArg(0), llvm::Type::getInt32Ty(Context));
    if (Foreign) {
      auto *Other = llvm::Function::Create(F.getFunctionType(),
                                           llvm::GlobalValue::InternalLinkage,
                                           "other", Module.get());
      auto *Block = llvm::BasicBlock::Create(Context, "entry", Other);
      auto *Return = llvm::ReturnInst::Create(
          Context, llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0),
          Block);
      Extra->insertBefore(Return->getIterator());
    }
    const auto G = buildLLVMParameterDataFlow(F, Sources, 0x1000, "owned",
                                              "same-abi", true);
    EXPECT_EQ(G.ParameterViews[0].ElidedTruncations, 0u);
    if (Foreign)
      Extra->eraseFromParent();
    else
      Extra->deleteValue();
  }
}

TEST(BinaryDiffParameterViews, RawAndRetainedBudgetsAreTransactional) {
  DataFlowOptions O;
  O.MaxNodes = 4;
  EXPECT_THROW(graph(Wide, {}, O), DataFlowInterrupted);
  EXPECT_THROW(graph(Narrow, {}, O), DataFlowInterrupted);
  O.MaxNodes = 5;
  EXPECT_NO_THROW(graph(Wide, {}, O));
  EXPECT_NO_THROW(graph(Narrow, {}, O));
  auto G = graph(Wide);
  G.ParameterViews[0].ElidedTruncations = 2;
  EXPECT_TRUE(G.validate(O));
  O = {};
  O.MaxWork = 10;
  EXPECT_THROW(graph(Wide, {}, O), DataFlowInterrupted);
  std::atomic<bool> Cancel{true};
  ComparisonControl C;
  C.Cancel = &Cancel;
  EXPECT_THROW(graph(Wide, {}, {}, C), DataFlowInterrupted);
}

TEST(BinaryDiffParameterViews, SavedViewsRequireCanonicalCompleteProvenance) {
  const auto G = graph(Wide);
  const auto Reject = [&](auto Change) {
    auto J = json(G);
    Change(*J.getAsObject());
    EXPECT_THROW(readDataFlowGraph(J), std::invalid_argument);
  };
  Reject([](auto &J) { J.erase("parameter_views"); });
  Reject([](auto &J) { J["parameter_count"] = 0; });
  Reject([](auto &J) { J["projected_parameter_count"] = 0; });
  Reject([](auto &J) { J["guessed"] = true; });
  Reject([](auto &J) {
    (*J.getArray("parameter_views"))[0].getAsObject()->operator[](
        "source_bits") = 32;
  });
  Reject([](auto &J) {
    (*J.getArray("parameter_views"))[0].getAsObject()->operator[](
        "elided_truncations") = 0;
  });
  Reject([](auto &J) {
    (*J.getArray("parameter_views"))[0].getAsObject()->operator[](
        "native_site") = nullptr;
  });
  Reject([](auto &J) {
    auto &Inputs =
        *J.getArray("nodes")->front().getAsObject()->getArray("inputs");
    (*Inputs[0].getAsArray())[7] = "i32;attrs=";
  });
  Reject([](auto &J) {
    auto &Inputs =
        *J.getArray("nodes")->front().getAsObject()->getArray("inputs");
    (*Inputs[0].getAsArray())[3] = "0x1";
  });
  auto Legacy = json(graph(Wide, {}, {}, {}, true));
  (*Legacy.getAsObject())["parameter_views"] = llvm::json::Array();
  EXPECT_THROW(readDataFlowGraph(Legacy), std::invalid_argument);
}

TEST(BinaryDiffParameterViews, CollectionRowsConsumeSharedStorageAllowance) {
  DataFlowEvidence E;
  E.Recipe = "owned";
  E.EligibleFunctions = 2;
  const auto A = graph(Narrow);
  auto B = A;
  B.Function = 0x2000;
  E.Entries = {{0x1000, FlowAvailability::Available, "", A},
               {0x2000, FlowAvailability::Available, "", B}};
  EXPECT_EQ(E.nodes(), 8u);
  EXPECT_EQ(E.retainedItems(), 10u);
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  llvm::json::OStream Out(Stream);
  writeDataFlowEvidence(Out, &E);
  auto Parsed = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Parsed));
  DataFlowSettings S;
  S.Enabled = true;
  S.Profile = LLVMParameterDataFlowProfile.str();
  S.MaxTotalNodes = 9;
  EXPECT_THROW(readDataFlowEvidence(*Parsed, S), std::invalid_argument);
  S.MaxTotalNodes = 10;
  EXPECT_EQ(readDataFlowEvidence(*Parsed, S)->Entries, E.Entries);
}
