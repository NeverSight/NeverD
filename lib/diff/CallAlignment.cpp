//===- CallAlignment.cpp - Unambiguous call-sequence anchors --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Intersects the two extreme optimal LCS paths. A shared diagonal belongs to
/// every optimal path, so repeated calls cannot arbitrarily anchor new pairs.
/// Large ranges use linear score frontiers instead of a Cartesian table.
///
//===----------------------------------------------------------------------===//

#include "CallAlignment.h"

#include "neverd/diff/Limits.h"
#include "neverd/diff/Program.h"

#include <algorithm>

namespace neverd::diff {
namespace {

struct Call {
  uint32_t Target;
  uint32_t Position;
};

using Pairs = std::vector<std::pair<uint32_t, uint32_t>>;

class CallAligner {
public:
  explicit CallAligner(const ComparisonControl &Control) : Control(Control) {}

  bool align(llvm::ArrayRef<Call> X, llvm::ArrayRef<Call> Y, bool Lower,
             Pairs &Out);

private:
  const ComparisonControl &Control;
  std::vector<uint32_t> Scores;

  bool dense(llvm::ArrayRef<Call> X, llvm::ArrayRef<Call> Y, bool Lower,
             Pairs &Out);
  bool frontier(llvm::ArrayRef<Call> X, llvm::ArrayRef<Call> Y, bool Reverse,
                std::vector<uint32_t> &Out) const;
};

bool CallAligner::dense(llvm::ArrayRef<Call> X, llvm::ArrayRef<Call> Y,
                        bool Lower, Pairs &Out) {
  const size_t N = X.size(), M = Y.size();
  Scores.assign((N + 1) * (M + 1), 0);
  const auto At = [&](size_t I, size_t J) -> uint32_t & {
    return Scores[I * (M + 1) + J];
  };
  for (size_t I = N; I-- > 0;)
    for (size_t J = M; J-- > 0;) {
      if (J % limits::diff::kAlignmentPollCells == 0 && Control.stop())
        return false;
      At(I, J) = X[I].Target == Y[J].Target
                     ? At(I + 1, J + 1) + 1
                     : std::max(At(I + 1, J), At(I, J + 1));
    }
  for (size_t I = 0, J = 0; I < N && J < M;) {
    if (Control.stop())
      return false;
    // Skipping an equal call can also be optimal. Prefer opposite boundaries
    // before the diagonal, so both traces agree only on mandatory anchors.
    if (Lower && At(I, J) == At(I + 1, J)) {
      ++I;
    } else if (!Lower && At(I, J) == At(I, J + 1)) {
      ++J;
    } else if (X[I].Target == Y[J].Target) {
      Out.emplace_back(X[I++].Position, Y[J++].Position);
    } else if (Lower) {
      ++J;
    } else {
      ++I;
    }
  }
  return true;
}

bool CallAligner::frontier(llvm::ArrayRef<Call> X, llvm::ArrayRef<Call> Y,
                           bool Reverse, std::vector<uint32_t> &Out) const {
  Out.assign(Y.size() + 1, 0);
  for (size_t I = 0; I < X.size(); ++I) {
    uint32_t Diagonal = 0;
    const auto &A = X[Reverse ? X.size() - I - 1 : I];
    for (size_t J = 0; J < Y.size(); ++J) {
      if (J % limits::diff::kAlignmentPollCells == 0 && Control.stop())
        return false;
      const auto &B = Y[Reverse ? Y.size() - J - 1 : J];
      const uint32_t Previous = Out[J + 1];
      Out[J + 1] =
          A.Target == B.Target ? Diagonal + 1 : std::max(Previous, Out[J]);
      Diagonal = Previous;
    }
  }
  return true;
}

bool CallAligner::align(llvm::ArrayRef<Call> X, llvm::ArrayRef<Call> Y,
                        bool Lower, Pairs &Out) {
  if (Control.stop())
    return false;
  if (X.empty() || Y.empty())
    return true;
  if (X.size() == Y.size()) {
    size_t I = 0;
    for (; I < X.size() && X[I].Target == Y[I].Target; ++I)
      if (I % limits::diff::kAlignmentPollCells == 0 && Control.stop())
        return false;
    if (I == X.size()) {
      // Matching every position has only one alignment, even for duplicates.
      for (I = 0; I < X.size(); ++I) {
        if (I % limits::diff::kAlignmentPollCells == 0 && Control.stop())
          return false;
        Out.emplace_back(X[I].Position, Y[I].Position);
      }
      return true;
    }
  }
  if (uint64_t(X.size()) + 1 <=
      limits::diff::kMaxCallAlignmentCells / (uint64_t(Y.size()) + 1))
    return dense(X, Y, Lower, Out);
  if (X.size() == 1 || Y.size() == 1) {
    std::optional<std::pair<uint32_t, uint32_t>> Found;
    for (const auto &A : X)
      for (const auto &B : Y) {
        if (Control.stop())
          return false;
        if (A.Target == B.Target) {
          Found = {A.Position, B.Position};
          if (Lower == (X.size() == 1)) {
            Out.push_back(*Found);
            return true;
          }
        }
      }
    if (Found)
      Out.push_back(*Found);
    return true;
  }

  const size_t Middle = X.size() / 2;
  size_t Split = 0;
  uint32_t Best = 0;
  {
    std::vector<uint32_t> Forward, Backward;
    if (!frontier(X.take_front(Middle), Y, false, Forward) ||
        !frontier(X.drop_front(Middle), Y, true, Backward))
      return false;
    for (size_t J = 0; J <= Y.size(); ++J) {
      if (J % limits::diff::kAlignmentPollCells == 0 && Control.stop())
        return false;
      const uint32_t Total = Forward[J] + Backward[Y.size() - J];
      if (Total > Best || (!Lower && Total == Best)) {
        Best = Total;
        Split = J;
      }
    }
  }
  // First/last optimal crossings preserve the opposite path boundaries.
  // Release frontiers before recursion to retain linear auxiliary storage.
  return !Best ||
         (align(X.take_front(Middle), Y.take_front(Split), Lower, Out) &&
          align(X.drop_front(Middle), Y.drop_front(Split), Lower, Out));
}

} // namespace

std::optional<std::vector<std::pair<uint32_t, uint32_t>>>
uniqueCallAnchors(llvm::ArrayRef<uint32_t> Primary,
                  llvm::ArrayRef<uint32_t> Secondary,
                  const ComparisonControl &Control) {
  std::vector<Call> X, Y;
  const auto Collect = [&](llvm::ArrayRef<uint32_t> Input,
                           std::vector<Call> &Out) {
    for (size_t I = 0; I < Input.size(); ++I) {
      if (I % limits::diff::kAlignmentPollCells == 0 && Control.stop())
        return false;
      if (Input[I] != NoIndex)
        Out.push_back({Input[I], uint32_t(I)});
    }
    return true;
  };
  if (!Collect(Primary, X) || !Collect(Secondary, Y))
    return std::nullopt;
  const bool Transposed = X.size() < Y.size();
  if (Transposed)
    std::swap(X, Y);
  Pairs Lower, Upper, Common;
  CallAligner Aligner(Control);
  if (!Aligner.align(X, Y, true, Lower) || !Aligner.align(X, Y, false, Upper))
    return std::nullopt;
  for (size_t I = 0; I < Lower.size(); ++I) {
    if (I % limits::diff::kAlignmentPollCells == 0 && Control.stop())
      return std::nullopt;
    if (Lower[I] == Upper[I]) {
      auto [A, B] = Lower[I];
      Common.emplace_back(Transposed ? B : A, Transposed ? A : B);
    }
  }
  return Common;
}

} // namespace neverd::diff
