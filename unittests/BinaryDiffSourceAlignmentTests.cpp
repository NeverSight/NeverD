//===- BinaryDiffSourceAlignmentTests.cpp - Source alignment contracts ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently scored text fixtures, complete line coverage and stop states.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/diff/SourceAlignment.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <string>
#include <vector>

using namespace neverd::diff;

namespace {
std::vector<llvm::StringRef> lines(const std::vector<std::string> &Text) {
  return {Text.begin(), Text.end()};
}

// Independent full-table edit distance. A replacement costs a deletion and
// insertion; the production implementation searches edit diagonals.
size_t distance(const std::vector<std::string> &A,
                const std::vector<std::string> &B) {
  std::vector<std::vector<size_t>> D(A.size() + 1,
                                     std::vector<size_t>(B.size() + 1));
  for (size_t I = 0; I <= A.size(); ++I)
    D[I][0] = I;
  for (size_t J = 0; J <= B.size(); ++J)
    D[0][J] = J;
  for (size_t I = 1; I <= A.size(); ++I)
    for (size_t J = 1; J <= B.size(); ++J)
      D[I][J] = std::min({D[I - 1][J] + 1, D[I][J - 1] + 1,
                          D[I - 1][J - 1] + (A[I - 1] == B[J - 1] ? 0 : 2)});
  return D.back().back();
}

void check(const std::vector<std::string> &A, const std::vector<std::string> &B,
           const SourceAlignment &R, size_t ExpectedCost) {
  ASSERT_TRUE(R.ComputationComplete);
  ASSERT_EQ(R.State, ComparisonState::Complete);
  size_t I = 0, J = 0, Cost = 0;
  for (const auto &Row : R.Rows) {
    const bool HasA = Row.Primary != NoSourceLine;
    const bool HasB = Row.Secondary != NoSourceLine;
    ASSERT_TRUE(HasA || HasB);
    if (HasA) {
      ASSERT_LT(I, A.size());
      EXPECT_EQ(Row.Primary, I++);
    }
    if (HasB) {
      ASSERT_LT(J, B.size());
      EXPECT_EQ(Row.Secondary, J++);
    }
    switch (Row.Kind) {
    case SourceLineKind::Unchanged:
      ASSERT_TRUE(HasA && HasB);
      EXPECT_EQ(A[Row.Primary], B[Row.Secondary]);
      break;
    case SourceLineKind::Changed:
      ASSERT_TRUE(HasA && HasB);
      EXPECT_NE(A[Row.Primary], B[Row.Secondary]);
      Cost += 2;
      break;
    case SourceLineKind::Inserted:
      EXPECT_FALSE(HasA);
      EXPECT_TRUE(HasB);
      ++Cost;
      break;
    case SourceLineKind::Deleted:
      EXPECT_TRUE(HasA);
      EXPECT_FALSE(HasB);
      ++Cost;
      break;
    }
  }
  EXPECT_EQ(I, A.size());
  EXPECT_EQ(J, B.size());
  EXPECT_EQ(Cost, ExpectedCost);
}

void check(const std::vector<std::string> &A, const std::vector<std::string> &B,
           const SourceAlignment &R) {
  check(A, B, R, distance(A, B));
}
} // namespace

TEST(BinaryDiffSourceAlignment, ExhaustiveRepeatedLinesMatchIndependentOracle) {
  std::vector<std::vector<std::string>> Inputs(1);
  size_t Begin = 0, End = 1;
  for (size_t Length = 0; Length < 4; ++Length) {
    for (size_t I = Begin; I < End; ++I)
      for (const auto *Line : {"", "return x;", "}"}) {
        auto Text = Inputs[I];
        Text.emplace_back(Line);
        Inputs.push_back(std::move(Text));
      }
    Begin = End;
    End = Inputs.size();
  }
  for (const auto &A : Inputs)
    for (const auto &B : Inputs) {
      const auto R = alignSourceLines(lines(A), lines(B));
      check(A, B, R);
      EXPECT_EQ(alignSourceLines(lines(A), lines(B)).Rows, R.Rows);
    }
}

TEST(BinaryDiffSourceAlignment, TextChangesKeepLiteralsNamesAndLineEndings) {
  const std::vector<std::string> A = {"int original() {\n", "  // 检查\n",
                                      "  return 0x11;\n", "}\n"};
  const std::vector<std::string> B = {"int renamed() {\n", "  // 检查\n",
                                      "  return 9;\n", "}"};
  const auto R = alignSourceLines(lines(A), lines(B));
  check(A, B, R);
  ASSERT_EQ(R.Rows.size(), 4u);
  EXPECT_EQ(R.Rows[0].Kind, SourceLineKind::Changed);
  EXPECT_EQ(R.Rows[1].Kind, SourceLineKind::Unchanged);
  EXPECT_EQ(R.Rows[2].Kind, SourceLineKind::Changed);
  EXPECT_EQ(R.Rows[3].Kind, SourceLineKind::Changed);
}

TEST(BinaryDiffSourceAlignment,
     RandomUnequalRepeatedLinesMatchIndependentOracle) {
  std::mt19937 Random(0x4D594552);
  for (size_t Trial = 0; Trial < 1500; ++Trial) {
    SCOPED_TRACE(Trial);
    const auto Alphabet = 1 + Random() % 11;
    std::vector<std::string> A(Random() % 97), B(Random() % 97);
    for (auto *Text : {&A, &B})
      for (auto &Line : *Text)
        Line = std::to_string(Random() % Alphabet);
    const auto R = alignSourceLines(lines(A), lines(B));
    check(A, B, R);
    EXPECT_EQ(alignSourceLines(lines(A), lines(B)).Rows, R.Rows);
    check(B, A, alignSourceLines(lines(B), lines(A)));
  }
}

TEST(BinaryDiffSourceAlignment, HighlyUnequalInputsKeepOptimalCoverage) {
  for (const size_t Count : {1, 2, 3, 7}) {
    SCOPED_TRACE(Count);
    std::vector<std::string> A(Count, "left"), B(10000, "right");
    for (const bool Shared : {false, true}) {
      SCOPED_TRACE(Shared);
      if (Shared) {
        A[Count / 2] = "shared";
        B[B.size() / 2] = "shared";
      }
      check(A, B, alignSourceLines(lines(A), lines(B)));
      check(B, A, alignSourceLines(lines(B), lines(A)));
    }
  }
}

TEST(BinaryDiffSourceAlignment, SparseEditsFinishWithinDefaultWorkAllowance) {
  for (const size_t Count : {10000, 100000}) {
    SCOPED_TRACE(Count);
    std::vector<std::string> A, B;
    for (size_t I = 0; I < Count; ++I) {
      const auto Line = "  value_" + std::to_string(I) + " = input + ";
      A.push_back(Line + "7;\n");
      B.push_back(Line + (I % (Count / 10) == 0 ? "9;\n" : "7;\n"));
    }
    // All original lines are unique; ten replacements cost exactly 20 edits.
    const auto Changed = alignSourceLines(lines(A), lines(B));
    check(A, B, Changed, 20);
    EXPECT_EQ(Changed.Rows.size(), Count);

    B.clear();
    for (size_t I = 0; I < Count; ++I) {
      if (I % (Count / 10) == 0) {
        if ((I / (Count / 10)) % 2 != 0)
          continue;
        B.push_back("  added_" + std::to_string(I) + "();\n");
      }
      B.push_back(A[I]);
    }
    // Five insertions and five deletions are separated by unchanged runs.
    const auto Gapped = alignSourceLines(lines(A), lines(B));
    check(A, B, Gapped, 10);
    EXPECT_EQ(Gapped.Rows.size(), Count + 5);
  }
}

TEST(BinaryDiffSourceAlignment, LongUnchangedRunsAvoidQuadraticWork) {
  std::vector<std::string> A(20000, "unchanged");
  auto B = A;
  B[B.size() / 2] = "changed";
  const auto R = alignSourceLines(lines(A), lines(B), {.MaxWork = 1000000});
  ASSERT_TRUE(R.ComputationComplete);
  ASSERT_EQ(R.Rows.size(), A.size());
  for (size_t I = 0; I < A.size(); ++I) {
    EXPECT_EQ(R.Rows[I].Primary, I);
    EXPECT_EQ(R.Rows[I].Secondary, I);
    EXPECT_EQ(R.Rows[I].Kind, I == A.size() / 2 ? SourceLineKind::Changed
                                                : SourceLineKind::Unchanged);
  }
}

TEST(BinaryDiffSourceAlignment,
     ExactWorkBoundAndRemovalNeverPublishPartialRows) {
  const std::vector<std::string> A = {"one", "two", "three", "four"};
  const std::vector<std::string> B = {"three", "two", "one", "five"};
  const auto Full = alignSourceLines(lines(A), lines(B), {.MaxWork = 0});
  check(A, B, Full);
  const auto Exact =
      alignSourceLines(lines(A), lines(B), {.MaxWork = Full.Work});
  EXPECT_TRUE(Exact.ComputationComplete);
  EXPECT_EQ(Exact.Rows, Full.Rows);
  for (const auto Budget : {uint64_t(1), Full.Work - 1}) {
    const auto Limited =
        alignSourceLines(lines(A), lines(B), {.MaxWork = Budget});
    EXPECT_EQ(Limited.State, ComparisonState::ResourceLimited);
    EXPECT_FALSE(Limited.ComputationComplete);
    EXPECT_TRUE(Limited.Rows.empty());
    EXPECT_LE(Limited.Work, Budget);
  }
}

TEST(BinaryDiffSourceAlignment, CancellationAndDeadlineDiscardClassifiedRows) {
  const std::vector<std::string> A = {"one", "two", "three", "four"};
  const std::vector<std::string> B = {"three", "two", "one", "five"};
  std::atomic<bool> Cancel = true;
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  auto R = alignSourceLines(lines(A), lines(B), {}, Control);
  EXPECT_EQ(R.State, ComparisonState::Cancelled);
  EXPECT_EQ(R.Work, 0u);
  EXPECT_TRUE(R.Rows.empty());
  Cancel = false;
  Control.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
    if (Phase == "source_alignment")
      Cancel = true;
  };
  R = alignSourceLines(lines(A), lines(B), {}, Control);
  EXPECT_EQ(R.State, ComparisonState::Cancelled);
  EXPECT_GT(R.Work, 0u);
  EXPECT_FALSE(R.ComputationComplete);
  EXPECT_TRUE(R.Rows.empty());
  Cancel = false;
  Control.Progress = {};
  Control.Deadline = std::chrono::steady_clock::now();
  R = alignSourceLines(lines(A), lines(B), {}, Control);
  EXPECT_EQ(R.State, ComparisonState::TimedOut);
  EXPECT_FALSE(R.ComputationComplete);
  EXPECT_TRUE(R.Rows.empty());
  check(A, B, alignSourceLines(lines(A), lines(B)));
}
