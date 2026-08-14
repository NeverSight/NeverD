//===- SemanticCAPITests.cpp - Public semantic C ABI tests ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace {

constexpr size_t kLegacySimplifyOptionsEnd =
    offsetof(neverd_simplify_options, allow_growth) + sizeof(int);
static_assert(offsetof(neverd_simplify_options, exhaustive) ==
                  kLegacySimplifyOptionsEnd,
              "simplify options fields must remain append-only");
#if INTPTR_MAX == INT64_MAX
static_assert(offsetof(neverd_simplify_options, struct_size) == 0);
static_assert(offsetof(neverd_simplify_options, width) == 8);
static_assert(offsetof(neverd_simplify_options, shallow) == 12);
static_assert(offsetof(neverd_simplify_options, max_atoms) == 16);
static_assert(offsetof(neverd_simplify_options, max_work) == 24);
static_assert(offsetof(neverd_simplify_options, verify_samples) == 32);
static_assert(offsetof(neverd_simplify_options, allow_growth) == 36);
static_assert(offsetof(neverd_simplify_options, exhaustive) == 40);
static_assert(sizeof(neverd_simplify_result) == 112,
              "the legacy simplify result layout is frozen");
#endif
static_assert(offsetof(neverd_simplify_result, evidence_name) +
                      sizeof(const char *) ==
                  sizeof(neverd_simplify_result),
              "the legacy MBA result must not acquire new result fields");

llvm::json::Object parseObject(const char *Text) {
  EXPECT_NE(Text, nullptr);
  if (!Text)
    return {};
  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Text);
  EXPECT_TRUE(static_cast<bool>(Parsed));
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    return {};
  }
  llvm::json::Object *Object = Parsed->getAsObject();
  EXPECT_NE(Object, nullptr);
  return Object ? std::move(*Object) : llvm::json::Object{};
}

std::string nestedVariable(size_t Depth) {
  std::string Expression(Depth, '(');
  Expression += 'x';
  Expression.append(Depth, ')');
  return Expression;
}

neverd_synthesize_options quickSynthesisOptions() {
  neverd_synthesize_options Options{};
  Options.struct_size = sizeof(Options);
  Options.max_cost = 5;
  Options.max_samples = 32;
  Options.verify_samples = 32;
  Options.max_work = size_t(1) << 18;
  return Options;
}

TEST(NeverDSemanticCAPI, StableEnumsAndNamesArePinned) {
  static_assert(NEVERD_PROOF_NOT_RUN == 0);
  static_assert(NEVERD_PROOF_EQUIVALENT == 1);
  static_assert(NEVERD_PROOF_DIFFERENT == 2);
  static_assert(NEVERD_PROOF_UNKNOWN == 3);
  static_assert(NEVERD_PROOF_INVALID == 4);
  static_assert(NEVERD_SYNTHESIS_NOT_APPLICABLE == 0);
  static_assert(NEVERD_SYNTHESIS_ALREADY_SHORTEST == 1);
  static_assert(NEVERD_SYNTHESIS_TOO_MANY_INPUTS == 2);
  static_assert(NEVERD_SYNTHESIS_SEARCH_BUDGET_EXHAUSTED == 3);
  static_assert(NEVERD_SYNTHESIS_COUNTEREXAMPLE == 4);
  static_assert(NEVERD_SYNTHESIS_PROOF_INCOMPLETE == 5);
  static_assert(NEVERD_SYNTHESIS_REWRITTEN == 6);
  static_assert(NEVERD_OPTIMIZATION_STABLE == 0);
  static_assert(NEVERD_OPTIMIZATION_CYCLE_DETECTED == 1);
  static_assert(NEVERD_OPTIMIZATION_BUDGET_EXHAUSTED == 2);
  static_assert(NEVERD_OPTIMIZATION_VERIFICATION_FAILED == 3);
  static_assert(NEVERD_OPTIMIZATION_INPUT_INVALID == 4);

  EXPECT_STREQ(neverd_proof_status_name(NEVERD_PROOF_NOT_RUN), "not-run");
  EXPECT_STREQ(neverd_proof_status_name(NEVERD_PROOF_EQUIVALENT), "equivalent");
  EXPECT_STREQ(neverd_proof_status_name(NEVERD_PROOF_DIFFERENT), "different");
  EXPECT_STREQ(neverd_proof_status_name(NEVERD_PROOF_UNKNOWN), "unknown");
  EXPECT_STREQ(neverd_proof_status_name(NEVERD_PROOF_INVALID), "invalid");
  EXPECT_STREQ(
      neverd_proof_status_name(static_cast<neverd_proof_status_t>(255)),
      "invalid");

  const char *SynthesisNames[] = {"not-applicable",  "already-shortest",
                                  "too-many-inputs", "search-budget-exhausted",
                                  "counterexample",  "proof-incomplete",
                                  "rewritten"};
  for (unsigned I = 0; I < 7; ++I)
    EXPECT_STREQ(neverd_synthesis_outcome_name(
                     static_cast<neverd_synthesis_outcome_t>(I)),
                 SynthesisNames[I]);
  EXPECT_STREQ(neverd_synthesis_outcome_name(
                   static_cast<neverd_synthesis_outcome_t>(255)),
               "invalid");

  const char *StopNames[] = {"stable", "cycle-detected", "budget-exhausted",
                             "verification-failed", "input-invalid"};
  for (unsigned I = 0; I < 5; ++I)
    EXPECT_STREQ(neverd_optimization_stop_name(
                     static_cast<neverd_optimization_stop_t>(I)),
                 StopNames[I]);
  EXPECT_STREQ(neverd_optimization_stop_name(
                   static_cast<neverd_optimization_stop_t>(255)),
               "invalid");
}

TEST(NeverDSemanticCAPI, LegacySimplificationRemainsMBAOnly) {
  constexpr const char *Expression = "(x >> 4) + ((x >> 2) >> 2)";
  neverd_simplify_options Options{};
  Options.struct_size = sizeof(Options);
  neverd_simplify_result Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(neverd_simplify_expr(Expression, &Options, &Result), 0);
  ASSERT_EQ(Result.ok, 1);
  EXPECT_EQ(Result.changed, 0);
  EXPECT_NE(Result.outcome, NEVERD_SIMPLIFY_REWRITTEN);
  neverd_simplify_result_dispose(&Result);

  const char *JSON = neverd_simplify_expr_json(Expression, 32, 1);
  llvm::json::Object Object = parseObject(JSON);
  EXPECT_EQ(Object.get("proofStatus"), nullptr);
  EXPECT_EQ(Object.get("stop"), nullptr);
  EXPECT_EQ(Object.get("counterexample"), nullptr);
  neverd_free_string(JSON);
}

TEST(NeverDSemanticCAPI, ShortSimplifyOptionsDoNotReadAppendedFields) {
  neverd_simplify_options Options{};
  Options.struct_size = kLegacySimplifyOptionsEnd;
  Options.width = 65537;
  // This models non-owned storage immediately after the old struct.  A newer
  // library must use struct_size rather than observing it as an appended flag.
  Options.exhaustive = 1;
  neverd_simplify_result Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(neverd_simplify_expr("x", &Options, &Result), 0);
  EXPECT_EQ(Result.ok, 0);
  ASSERT_NE(Result.error, nullptr);
  EXPECT_NE(std::string(Result.error).find("width"), std::string::npos);
  neverd_simplify_result_dispose(&Result);
}

TEST(NeverDSemanticCAPI, ExhaustiveSimplificationRemovesParserPolicyCeilings) {
  const std::string Deep = nestedVariable(520);
  neverd_simplify_options Options{};
  Options.struct_size = sizeof(Options);
  neverd_simplify_result Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(neverd_simplify_expr(Deep.c_str(), &Options, &Result), 0);
  EXPECT_EQ(Result.ok, 0);
  ASSERT_NE(Result.error, nullptr);
  EXPECT_NE(std::string(Result.error).find("nests too deeply"),
            std::string::npos);
  neverd_simplify_result_dispose(&Result);

  Options.exhaustive = 1;
  Result = {};
  Result.struct_size = sizeof(Result);
  ASSERT_EQ(neverd_simplify_expr(Deep.c_str(), &Options, &Result), 0);
  ASSERT_EQ(Result.ok, 1) << (Result.error ? Result.error : "");
  neverd_simplify_result_dispose(&Result);

  Options.width = 65537;
  Options.exhaustive = 0;
  Result = {};
  Result.struct_size = sizeof(Result);
  ASSERT_EQ(neverd_simplify_expr("x", &Options, &Result), 0);
  EXPECT_EQ(Result.ok, 0);
  neverd_simplify_result_dispose(&Result);

  Options.exhaustive = 1;
  Result = {};
  Result.struct_size = sizeof(Result);
  ASSERT_EQ(neverd_simplify_expr("x", &Options, &Result), 0);
  ASSERT_EQ(Result.ok, 1) << (Result.error ? Result.error : "");
  neverd_simplify_result_dispose(&Result);
}

TEST(NeverDSemanticCAPI, ExhaustiveSimplificationOverridesLegacyCeilings) {
  neverd_simplify_options Options{};
  Options.struct_size = sizeof(Options);
  Options.max_atoms = 1;
  Options.max_work = 1;
  Options.exhaustive = 1;
  neverd_simplify_result Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(neverd_simplify_expr("(x ^ y) + 2 * (x & y)", &Options, &Result),
            0);
  ASSERT_EQ(Result.ok, 1) << (Result.error ? Result.error : "");
  EXPECT_EQ(Result.outcome, NEVERD_SIMPLIFY_REWRITTEN);
  EXPECT_EQ(Result.changed, 1);
  ASSERT_NE(Result.output, nullptr);
  EXPECT_STREQ(Result.output, "x + y");
  neverd_simplify_result_dispose(&Result);
}

TEST(NeverDSemanticCAPI, ExhaustiveSynthesisRemovesParserPolicyCeilings) {
  const std::string Deep = nestedVariable(520);
  neverd_synthesize_options Options{};
  Options.struct_size = sizeof(Options);
  Options.width = 65537;
  neverd_synthesize_result Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(neverd_synthesize_expr(Deep.c_str(), &Options, &Result), 0);
  EXPECT_EQ(Result.ok, 0);
  neverd_synthesize_result_dispose(&Result);

  Options.exhaustive = 1;
  Result = {};
  Result.struct_size = sizeof(Result);
  ASSERT_EQ(neverd_synthesize_expr(Deep.c_str(), &Options, &Result), 0);
  ASSERT_EQ(Result.ok, 1) << (Result.error ? Result.error : "");
  EXPECT_EQ(Result.changed, 0);
  neverd_synthesize_result_dispose(&Result);
}

TEST(NeverDSemanticCAPI, ExhaustiveSynthesisRemovesBitBlastPolicyCeilings) {
  constexpr const char *Expression = "(x >> 4) + ((x >> 2) >> 2)";
  neverd_synthesize_options Options = quickSynthesisOptions();
  Options.width = 257;
  neverd_synthesize_result Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(neverd_synthesize_expr(Expression, &Options, &Result), 0);
  ASSERT_EQ(Result.ok, 1) << (Result.error ? Result.error : "");
  EXPECT_EQ(Result.changed, 0);
  EXPECT_EQ(Result.outcome, NEVERD_SYNTHESIS_PROOF_INCOMPLETE);
  EXPECT_EQ(Result.proof_status, NEVERD_PROOF_UNKNOWN);
  neverd_synthesize_result_dispose(&Result);

  Options.exhaustive = 1;
  Result = {};
  Result.struct_size = sizeof(Result);
  ASSERT_EQ(neverd_synthesize_expr(Expression, &Options, &Result), 0);
  ASSERT_EQ(Result.ok, 1) << (Result.error ? Result.error : "");
  EXPECT_EQ(Result.changed, 1);
  EXPECT_EQ(Result.outcome, NEVERD_SYNTHESIS_REWRITTEN);
  EXPECT_EQ(Result.proof_status, NEVERD_PROOF_EQUIVALENT);
  neverd_synthesize_result_dispose(&Result);
}

TEST(NeverDSemanticCAPI, SynthesisRewriteRequiresAnEquivalentProof) {
  constexpr const char *Expression = "(x >> 4) + ((x >> 2) >> 2)";
  neverd_synthesize_options Options = quickSynthesisOptions();
  neverd_synthesize_result Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(neverd_synthesize_expr(Expression, &Options, &Result), 0);
  ASSERT_EQ(Result.ok, 1) << (Result.error ? Result.error : "");
  EXPECT_EQ(Result.changed, 1);
  EXPECT_EQ(Result.outcome, NEVERD_SYNTHESIS_REWRITTEN);
  EXPECT_EQ(Result.proof_status, NEVERD_PROOF_EQUIVALENT);
  EXPECT_LT(Result.cost_after, Result.cost_before);
  EXPECT_GT(Result.search_work, 0u);
  EXPECT_GT(Result.proof_queries, 0u);
  EXPECT_EQ(Result.counterexample_json, nullptr);
  ASSERT_NE(Result.input, nullptr);
  ASSERT_NE(Result.output, nullptr);
  EXPECT_STRNE(Result.input, Result.output);
  neverd_synthesize_result_dispose(&Result);
}

TEST(NeverDSemanticCAPI, SolverUnknownAndSearchBudgetAreDistinct) {
  // The carry-save spelling reaches real SAT propagation when the candidate
  // is `x + y`; unlike a shift identity, it cannot be discharged before the
  // watched-clause budget is observed.
  constexpr const char *Expression = "(x ^ y) + 2 * (x & y)";
  neverd_synthesize_options UnknownOptions = quickSynthesisOptions();
  UnknownOptions.solver_max_watch_visits = 1;
  neverd_synthesize_result Unknown{};
  Unknown.struct_size = sizeof(Unknown);

  ASSERT_EQ(neverd_synthesize_expr(Expression, &UnknownOptions, &Unknown), 0);
  ASSERT_EQ(Unknown.ok, 1);
  EXPECT_EQ(Unknown.changed, 0);
  EXPECT_EQ(Unknown.outcome, NEVERD_SYNTHESIS_PROOF_INCOMPLETE);
  EXPECT_EQ(Unknown.proof_status, NEVERD_PROOF_UNKNOWN);
  EXPECT_EQ(Unknown.proof_queries, 1u);
  EXPECT_EQ(Unknown.proof_watch_visits, 1u);
  EXPECT_EQ(Unknown.counterexample_json, nullptr);
  ASSERT_NE(Unknown.input, nullptr);
  ASSERT_NE(Unknown.output, nullptr);
  EXPECT_STREQ(Unknown.input, Unknown.output);
  neverd_synthesize_result_dispose(&Unknown);

  neverd_synthesize_options BudgetOptions = quickSynthesisOptions();
  BudgetOptions.max_work = 1;
  neverd_synthesize_result Budget{};
  Budget.struct_size = sizeof(Budget);
  ASSERT_EQ(neverd_synthesize_expr(Expression, &BudgetOptions, &Budget), 0);
  ASSERT_EQ(Budget.ok, 1);
  EXPECT_EQ(Budget.changed, 0);
  EXPECT_EQ(Budget.outcome, NEVERD_SYNTHESIS_SEARCH_BUDGET_EXHAUSTED);
  EXPECT_EQ(Budget.proof_status, NEVERD_PROOF_NOT_RUN);
  EXPECT_EQ(Budget.proof_queries, 0u);
  EXPECT_EQ(Budget.counterexample_json, nullptr);
  neverd_synthesize_result_dispose(&Budget);
}

TEST(NeverDSemanticCAPI, SolverCounterexampleIsCanonicalAndFinal) {
  constexpr const char *Expression = "x + ((1 << y) & 16)";
  neverd_synthesize_options Options{};
  Options.struct_size = sizeof(Options);
  Options.max_cost = 1;
  Options.max_samples = 1;
  Options.verify_samples = 1;
  Options.max_work = 20000;
  Options.stochastic_slots = 1;
  Options.stochastic_restarts = 1;
  Options.stochastic_iterations = 1;
  neverd_synthesize_result Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(neverd_synthesize_expr(Expression, &Options, &Result), 0);
  ASSERT_EQ(Result.ok, 1);
  EXPECT_EQ(Result.changed, 0);
  EXPECT_EQ(Result.outcome, NEVERD_SYNTHESIS_COUNTEREXAMPLE);
  EXPECT_EQ(Result.proof_status, NEVERD_PROOF_DIFFERENT);
  EXPECT_GT(Result.proof_queries, 0u);
  ASSERT_NE(Result.input, nullptr);
  ASSERT_NE(Result.output, nullptr);
  EXPECT_STREQ(Result.input, Result.output);
  ASSERT_NE(Result.counterexample_json, nullptr);

  llvm::json::Object Counterexample = parseObject(Result.counterexample_json);
  EXPECT_EQ(Counterexample.getString("candidate"), "x");
  const llvm::json::Array *Variables = Counterexample.getArray("variables");
  ASSERT_NE(Variables, nullptr);
  ASSERT_EQ(Variables->size(), 2u);
  const llvm::json::Object *Y = (*Variables)[1].getAsObject();
  ASSERT_NE(Y, nullptr);
  EXPECT_EQ(Y->getString("name"), "y");
  EXPECT_EQ(Y->getString("value"), "0x00000004");

  const char *JSON = neverd_synthesize_expr_json_v1(Expression, &Options);
  llvm::json::Object Adapter = parseObject(JSON);
  EXPECT_EQ(Adapter.getString("proofStatus"), "different");
  ASSERT_NE(Adapter.getObject("counterexample"), nullptr);
  EXPECT_EQ(Adapter.getObject("counterexample")->getString("candidate"), "x");
  neverd_free_string(JSON);
  neverd_synthesize_result_dispose(&Result);
}

TEST(NeverDSemanticCAPI, SynthesisErrorsAreDisposableAndSizeBounded) {
  neverd_synthesize_result Result{};
  Result.struct_size = sizeof(Result);
  ASSERT_EQ(neverd_synthesize_expr("(x +", nullptr, &Result), 0);
  EXPECT_EQ(Result.ok, 0);
  EXPECT_NE(Result.error, nullptr);
  EXPECT_EQ(Result.input, nullptr);
  EXPECT_EQ(Result.output, nullptr);
  EXPECT_EQ(Result.counterexample_json, nullptr);
  neverd_synthesize_result_dispose(&Result);
  neverd_synthesize_result_dispose(&Result);
  EXPECT_EQ(Result.error, nullptr);

  struct ShortResult {
    size_t struct_size;
    int ok;
    uint32_t canary;
  } Short{offsetof(ShortResult, canary), -1, 0x7a11c0deu};
  ASSERT_EQ(neverd_synthesize_expr(
                "(x +", nullptr,
                reinterpret_cast<neverd_synthesize_result *>(&Short)),
            0);
  EXPECT_EQ(Short.ok, 0);
  EXPECT_EQ(Short.canary, 0x7a11c0deu);
}

TEST(NeverDSemanticCAPI, SynthesisJSONV1MatchesTheTypedResult) {
  constexpr const char *Expression = "(x >> 4) + ((x >> 2) >> 2)";
  neverd_synthesize_options Options = quickSynthesisOptions();
  neverd_synthesize_result Typed{};
  Typed.struct_size = sizeof(Typed);
  ASSERT_EQ(neverd_synthesize_expr(Expression, &Options, &Typed), 0);
  ASSERT_EQ(Typed.ok, 1);

  const char *JSON = neverd_synthesize_expr_json_v1(Expression, &Options);
  llvm::json::Object Object = parseObject(JSON);
  EXPECT_EQ(Object.getInteger("schemaVersion"), 1);
  EXPECT_EQ(Object.getBoolean("ok"), true);
  EXPECT_EQ(Object.getBoolean("changed"), Typed.changed != 0);
  EXPECT_EQ(Object.getString("outcome"),
            neverd_synthesis_outcome_name(Typed.outcome));
  EXPECT_EQ(Object.getString("proofStatus"),
            neverd_proof_status_name(Typed.proof_status));
  EXPECT_EQ(Object.getInteger("proofQueries"),
            static_cast<int64_t>(Typed.proof_queries));
  EXPECT_EQ(Object.get("counterexample"), nullptr);
  neverd_free_string(JSON);
  neverd_synthesize_result_dispose(&Typed);
}

TEST(NeverDSemanticCAPI, LLVMParseFailureReturnsInputInvalidWithoutIR) {
  neverd_optimize_llvm_ir_result Result{};
  Result.struct_size = sizeof(Result);
  ASSERT_EQ(neverd_optimize_llvm_ir("define i32 @broken( {", nullptr, &Result),
            0);
  EXPECT_EQ(Result.ok, 0);
  EXPECT_EQ(Result.stop, NEVERD_OPTIMIZATION_INPUT_INVALID);
  EXPECT_NE(Result.error, nullptr);
  EXPECT_EQ(Result.output_ir, nullptr);
  neverd_optimize_llvm_ir_result_dispose(&Result);
  neverd_optimize_llvm_ir_result_dispose(&Result);

  const char *JSON =
      neverd_optimize_llvm_ir_json_v1("define i32 @broken( {", nullptr);
  llvm::json::Object Object = parseObject(JSON);
  EXPECT_EQ(Object.getInteger("schemaVersion"), 1);
  EXPECT_EQ(Object.getBoolean("ok"), false);
  EXPECT_EQ(Object.getString("stop"), "input-invalid");
  EXPECT_EQ(Object.getString("outputIR"), std::nullopt);
  neverd_free_string(JSON);
}

TEST(NeverDSemanticCAPI, LLVMConservativeModeRejectsSynthesis) {
  constexpr const char *IR = "define void @f() { ret void }";
  neverd_optimize_llvm_ir_options Options{};
  Options.struct_size = sizeof(Options);
  Options.mode = NEVERD_OPTIMIZATION_MODE_CONSERVATIVE;
  Options.enable_synthesis = 1;
  neverd_optimize_llvm_ir_result Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(neverd_optimize_llvm_ir(IR, &Options, &Result), 0);
  EXPECT_EQ(Result.ok, 0);
  EXPECT_EQ(Result.stop, NEVERD_OPTIMIZATION_INPUT_INVALID);
  ASSERT_NE(Result.error, nullptr);
  EXPECT_NE(std::string(Result.error).find("conservative"), std::string::npos);
  EXPECT_EQ(Result.output_ir, nullptr);
  neverd_optimize_llvm_ir_result_dispose(&Result);

  const char *JSON = neverd_optimize_llvm_ir_json_v1(IR, &Options);
  llvm::json::Object Object = parseObject(JSON);
  EXPECT_EQ(Object.getBoolean("ok"), false);
  EXPECT_EQ(Object.getString("stop"), "input-invalid");
  EXPECT_EQ(Object.getString("outputIR"), std::nullopt);
  ASSERT_TRUE(Object.getString("error").has_value());
  EXPECT_TRUE(Object.getString("error")->contains("conservative"));
  neverd_free_string(JSON);
}

TEST(NeverDSemanticCAPI, LLVMSynthesisOptionsRequireSynthesis) {
  constexpr const char *IR = "define void @f() { ret void }";
  neverd_optimize_llvm_ir_options Options{};
  Options.struct_size = sizeof(Options);
  Options.solver_max_conflicts = 1;
  neverd_optimize_llvm_ir_result Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(neverd_optimize_llvm_ir(IR, &Options, &Result), 0);
  EXPECT_EQ(Result.ok, 0);
  EXPECT_EQ(Result.stop, NEVERD_OPTIMIZATION_INPUT_INVALID);
  ASSERT_NE(Result.error, nullptr);
  EXPECT_NE(std::string(Result.error).find("enable_synthesis"),
            std::string::npos);
  EXPECT_EQ(Result.output_ir, nullptr);
  neverd_optimize_llvm_ir_result_dispose(&Result);
}

TEST(NeverDSemanticCAPI, LLVMOptimizationUsesTheTransactionalDeepPipeline) {
  constexpr const char *IR = R"(
    define internal i32 @helper() {
    entry:
      ret i32 42
    }
    define i32 @entry() {
    entry:
      %value = call i32 @helper()
      ret i32 %value
    }
  )";
  neverd_optimize_llvm_ir_result Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(neverd_optimize_llvm_ir(IR, nullptr, &Result), 0);
  ASSERT_EQ(Result.ok, 1) << (Result.error ? Result.error : "");
  EXPECT_EQ(Result.changed, 1);
  EXPECT_EQ(Result.stop, NEVERD_OPTIMIZATION_STABLE);
  EXPECT_EQ(Result.functions_visited, 2u);
  ASSERT_NE(Result.output_ir, nullptr);
  EXPECT_NE(std::string(Result.output_ir).find("ret i32 42"),
            std::string::npos);
  neverd_optimize_llvm_ir_result_dispose(&Result);
}

TEST(NeverDSemanticCAPI, LLVMResultHonorsAnOlderStructSize) {
  struct ShortResult {
    size_t struct_size;
    int ok;
    uint32_t canary;
  } Short{offsetof(ShortResult, canary), -1, 0x51a7e55u};

  ASSERT_EQ(neverd_optimize_llvm_ir(
                "define i32 @broken( {", nullptr,
                reinterpret_cast<neverd_optimize_llvm_ir_result *>(&Short)),
            0);
  EXPECT_EQ(Short.ok, 0);
  EXPECT_EQ(Short.canary, 0x51a7e55u);
}

} // namespace
