//===- BinaryDiffCallAlignmentTests.cpp - Call-anchor ambiguity -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent enumeration and full-table oracles for mandatory call anchors,
/// including repeated callees, large sequences, holes and interrupted work.
///
//===----------------------------------------------------------------------===//

#include "../lib/diff/CallAlignment.h"

#include "neverd/diff/Program.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <iterator>
#include <thread>
#include <utility>
#include <vector>

using namespace neverd::diff;

namespace {

using Pairs = std::vector<std::pair<uint32_t, uint32_t>>;

// Enumerate every monotone matching without a DP recurrence or path tie rule.
Pairs enumerateMandatory(llvm::ArrayRef<uint32_t> X,
                         llvm::ArrayRef<uint32_t> Y) {
  size_t Longest = 0;
  Pairs Current, Common;
  const auto Visit = [&](auto &&Self, size_t I, size_t J) -> void {
    if (Current.size() > Longest) {
      Longest = Current.size();
      Common = Current;
    } else if (Current.size() == Longest) {
      Pairs Both;
      std::set_intersection(Common.begin(), Common.end(), Current.begin(),
                            Current.end(), std::back_inserter(Both));
      Common = std::move(Both);
    }
    for (size_t A = I; A < X.size(); ++A)
      for (size_t B = J; B < Y.size(); ++B)
        if (X[A] != NoIndex && X[A] == Y[B]) {
          Current.emplace_back(A, B);
          Self(Self, A + 1, B + 1);
          Current.pop_back();
        }
  };
  Visit(Visit, 0, 0);
  return Common;
}

// A pair is viable at rank k iff its optimal prefix and suffix fill an LCS.
// Exactly one viable pair at that rank makes it mandatory. This retains full
// prefix/suffix tables and does not trace the implementation's extreme paths.
Pairs rankOracle(llvm::ArrayRef<uint32_t> X, llvm::ArrayRef<uint32_t> Y) {
  const size_t N = X.size(), M = Y.size();
  std::vector<uint32_t> Prefix((N + 1) * (M + 1)), Suffix(Prefix.size());
  const auto At = [&](std::vector<uint32_t> &Table, size_t I,
                      size_t J) -> uint32_t & {
    return Table[I * (M + 1) + J];
  };
  const auto Same = [&](size_t I, size_t J) {
    return X[I] != NoIndex && X[I] == Y[J];
  };
  for (size_t I = 0; I < N; ++I)
    for (size_t J = 0; J < M; ++J)
      At(Prefix, I + 1, J + 1) =
          Same(I, J) ? At(Prefix, I, J) + 1
                     : std::max(At(Prefix, I + 1, J), At(Prefix, I, J + 1));
  for (size_t I = N; I-- > 0;)
    for (size_t J = M; J-- > 0;)
      At(Suffix, I, J) =
          Same(I, J) ? At(Suffix, I + 1, J + 1) + 1
                     : std::max(At(Suffix, I + 1, J), At(Suffix, I, J + 1));
  const uint32_t Length = At(Prefix, N, M);
  std::vector<size_t> Viable(Length);
  Pairs Last(Length), Result;
  for (size_t I = 0; I < N; ++I)
    for (size_t J = 0; J < M; ++J)
      if (Same(I, J) &&
          At(Prefix, I, J) + 1 + At(Suffix, I + 1, J + 1) == Length) {
        const uint32_t Rank = At(Prefix, I, J);
        ++Viable[Rank];
        Last[Rank] = {I, J};
      }
  for (uint32_t I = 0; I < Length; ++I)
    if (Viable[I] == 1)
      Result.push_back(Last[I]);
  return Result;
}

TEST(BinaryDiffCallAlignment, ExhaustiveSmallSequencesRetainOnlyForcedPairs) {
  std::vector<std::vector<uint32_t>> Inputs(1);
  size_t Begin = 0, End = 1;
  for (unsigned Length = 1; Length <= 4; ++Length) {
    for (size_t I = Begin; I < End; ++I)
      for (uint32_t Value : {NoIndex, 0u, 1u}) {
        auto Next = Inputs[I];
        Next.push_back(Value);
        Inputs.push_back(std::move(Next));
      }
    Begin = End;
    End = Inputs.size();
  }
  for (const auto &X : Inputs)
    for (const auto &Y : Inputs) {
      const auto Actual = uniqueCallAnchors(X, Y);
      ASSERT_TRUE(Actual);
      EXPECT_EQ(*Actual, enumerateMandatory(X, Y));
    }
}

TEST(BinaryDiffCallAlignment, LinearFrontiersAgreeWithAllViableRanks) {
  for (const auto &[N, M] : {std::pair(1100u, 1200u), std::pair(20u, 80000u)}) {
    std::vector<uint32_t> X(N), Y(M);
    for (unsigned I = 0; I < N; ++I)
      X[I] = I % 13;
    for (unsigned J = 0; J < M; ++J)
      Y[J] = (J + 5) % 13;
    X[N / 2] = Y[M / 2] = 99;
    const Pairs Expected = rankOracle(X, Y);
    const auto Actual = uniqueCallAnchors(X, Y);
    ASSERT_TRUE(Actual);
    EXPECT_EQ(*Actual, Expected);
    const auto Reverse = uniqueCallAnchors(Y, X);
    ASSERT_TRUE(Reverse);
    Pairs Swapped;
    for (const auto &[A, B] : *Reverse)
      Swapped.emplace_back(B, A);
    EXPECT_EQ(Swapped, Expected);
  }
}

TEST(BinaryDiffCallAlignment, VeryUnequalRepeatedCallsHaveNoForcedOccurrence) {
  const std::vector<uint32_t> X = {7}, Y(600000, 7);
  for (const auto &[A, B] : {std::pair(&X, &Y), std::pair(&Y, &X)}) {
    const auto Result = uniqueCallAnchors(*A, *B);
    ASSERT_TRUE(Result);
    EXPECT_TRUE(Result->empty());
  }
}

TEST(BinaryDiffCallAlignment, StopsWithoutReturningPartialAnchors) {
  std::vector<uint32_t> X(20000), Y(20000);
  for (unsigned I = 0; I < X.size(); ++I) {
    X[I] = I % 17;
    Y[I] = (I + 3) % 17;
  }
  std::atomic<bool> Cancel{false};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  std::thread Stop([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    Cancel.store(true);
  });
  const auto Cancelled = uniqueCallAnchors(X, Y, Control);
  Stop.join();
  EXPECT_FALSE(Cancelled);
  Control.Cancel = nullptr;
  Control.Deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
  EXPECT_FALSE(uniqueCallAnchors(X, Y, Control));
  EXPECT_EQ(uniqueCallAnchors({1, 2, 1}, {1, 2, 1}),
            std::optional<Pairs>({{0, 0}, {1, 1}, {2, 2}}));
}

} // namespace
