#include "X86FPStateFixture.h"

#include <cctype>

class X86_64_SSE_FP : public X86FPStateLiftTest {};

static fs::path testObj() { return fs::path(TEST_OBJ_DIR) / "test_sse_fp.o"; }

// LLVM canonicalizes the scalar-FP-return idiom — building `<x, 0>` from a
// scalar via `bitcast(zext(x i64 -> i128)) -> <2 x i64>` — into
// `insertelement <i64 poison, i64 0>, x, 0`. The `poison` marks the lane that
// the insertelement immediately overwrites, so it is provably dead and the
// result vector contains no poison. That is correct upstream LLVM behavior, not
// a lifter or codegen bug (the lifter's pre-opt IR is poison-free). This helper
// recognizes ONLY that idiom: a `poison` element of the constant base vector of
// an `insertelement` whose (constant) insert index equals the poisoned lane.
// Any other poison — real poison values, live poisoned lanes, a non-constant
// index, or poison outside an insertelement — is still reported.
static bool isOverwrittenLanePoison(const std::string &Line, size_t PoisonPos) {
  if (Line.find("insertelement") == std::string::npos)
    return false;
  // The poison must sit inside a `<...>` vector literal: find its opening '<'
  // and confirm no '>' closes that literal before the poison token.
  size_t Open = Line.rfind('<', PoisonPos);
  if (Open == std::string::npos)
    return false;
  size_t CloseAfterOpen = Line.find('>', Open);
  if (CloseAfterOpen == std::string::npos || CloseAfterOpen < PoisonPos)
    return false;
  // Lane index of the poison = number of commas between '<' and the poison.
  size_t PoisonLane = 0;
  for (size_t I = Open; I < PoisonPos; ++I)
    if (Line[I] == ',')
      ++PoisonLane;
  // Insert index = trailing integer literal after the last comma on the line,
  // which must come after the base vector literal we matched above.
  size_t LastComma = Line.rfind(',');
  if (LastComma == std::string::npos || LastComma < CloseAfterOpen)
    return false;
  size_t End = Line.size();
  while (End > LastComma && !std::isdigit((unsigned char)Line[End - 1]))
    --End;
  size_t Begin = End;
  while (Begin > LastComma && std::isdigit((unsigned char)Line[Begin - 1]))
    --Begin;
  if (Begin == End)
    return false; // non-constant / missing index -> cannot prove overwrite
  size_t InsertIdx = 0;
  for (size_t I = Begin; I < End; ++I)
    InsertIdx = InsertIdx * 10 + size_t(Line[I] - '0');
  return InsertIdx == PoisonLane;
}

TEST_F(X86_64_SSE_FP, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_sse_fp.o not built";
  verifyAllStages(testObj());
}

TEST_F(X86_64_SSE_FP, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(X86_64_SSE_FP, AddsdHasFloatAdd) {
  verifyScalarFPState(testObj(), "test_addsd",
                      neverd::Intrinsic::X86FPAddState);
}

TEST_F(X86_64_SSE_FP, SubsdHasFloatSub) {
  verifyScalarFPState(testObj(), "test_subsd",
                      neverd::Intrinsic::X86FPSubState);
}

TEST_F(X86_64_SSE_FP, MulsdHasFloatMult) {
  verifyScalarFPState(testObj(), "test_mulsd",
                      neverd::Intrinsic::X86FPMulState);
}

TEST_F(X86_64_SSE_FP, DivsdHasFloatDiv) {
  verifyScalarFPState(testObj(), "test_divsd",
                      neverd::Intrinsic::X86FPDivState);
}

TEST_F(X86_64_SSE_FP, AddssHasFloatAdd) {
  verifyScalarFPState(testObj(), "test_addss",
                      neverd::Intrinsic::X86FPAddState);
}

TEST_F(X86_64_SSE_FP, MulssHasFloatMult) {
  verifyScalarFPState(testObj(), "test_mulss",
                      neverd::Intrinsic::X86FPMulState);
}

TEST_F(X86_64_SSE_FP, Cvtsd2siHasTrunc) {
  verifyScalarFPState(testObj(), "test_cvtsd2si",
                      neverd::Intrinsic::X86FPCvtToIntState);
}

TEST_F(X86_64_SSE_FP, Cvtsi2sdRetainsNumericalAndMxcsrCompletion) {
  verifyScalarFPState(testObj(), "test_cvtsi2sd",
                      neverd::Intrinsic::X86FPScalarConvertState, 5);
}

TEST_F(X86_64_SSE_FP, NoUnreachableInFunctions) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0);
  EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
      << "Found 'unreachable' in LLVM IR:\n"
      << r.out;
}

TEST_F(X86_64_SSE_FP, NoPoisonInFunctions) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0);
  size_t pos = 0;
  while ((pos = r.out.find("poison", pos)) != std::string::npos) {
    size_t line_start = r.out.rfind('\n', pos);
    if (line_start == std::string::npos)
      line_start = 0;
    std::string line =
        r.out.substr(line_start, r.out.find('\n', pos) - line_start);
    size_t poison_in_line = pos - line_start;
    // `nocreateundeforpoison` is an attribute substring (false positive); the
    // dead-lane insertelement poison is LLVM's canonical, provably-benign
    // form for scalar FP returns. Flag anything else.
    bool benign = line.find("nocreateundeforpoison") != std::string::npos ||
                  isOverwrittenLanePoison(line, poison_in_line);
    if (!benign) {
      FAIL() << "Found 'poison' in LLVM IR line: " << line;
    }
    pos += 6;
  }
}
