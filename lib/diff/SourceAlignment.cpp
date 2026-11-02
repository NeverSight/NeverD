//===- SourceAlignment.cpp - Bounded exact source-line alignment ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Linear-space shortest edit paths over exact source lines with explicit
/// stop states and output-sensitive work for sparse changes.
//===----------------------------------------------------------------------===//

#include "neverd/diff/SourceAlignment.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <utility>

namespace neverd::diff {
namespace {
struct Interrupted {
  ComparisonState State;
};

class Alignment {
  const SourceAlignmentOptions &Options;
  const ComparisonControl &Control;
  SourceAlignment &Result;
  std::vector<uint64_t> A, B;
  std::vector<std::pair<size_t, size_t>> Matches;

  void charge(uint64_t Amount = 1) {
    if (auto Stop = Control.stop())
      throw Interrupted{*Stop};
    if (Amount > UINT64_MAX - Result.Work ||
        (Options.MaxWork && Amount > Options.MaxWork - Result.Work))
      throw Interrupted{ComparisonState::ResourceLimited};
    Result.Work += Amount;
  }

  bool equal(size_t I, size_t J) {
    charge();
    return A[I] == B[J];
  }

  struct Snake {
    size_t ABegin, BBegin, AEnd, BEnd;
  };

  // Bidirectional shortest-edit-path search (Myers, 1986, section 4b;
  // doi:10.1007/BF01840446). Each frontier keeps only its furthest position
  // per edit diagonal. The first overlap identifies an optimal middle snake;
  // its buffers are released before descending into either subproblem.
  Snake middle(size_t ABegin, size_t AEnd, size_t BBegin, size_t BEnd) {
    const uint64_t ASize = AEnd - ABegin, BSize = BEnd - BBegin;
    const auto MaxCoordinates = std::min<uint64_t>(INT64_MAX, SIZE_MAX);
    // Leave room for both frontier sentinels and round-up to an even width.
    if (ASize > MaxCoordinates - 4 || BSize > MaxCoordinates - 4 - ASize)
      throw Interrupted{ComparisonState::ResourceLimited};
    const int64_t N = ASize, M = BSize, Delta = N - M;
    const int64_t Radius = (N + M + 1) / 2, Base = Radius + 1;
    const size_t Storage = 2 * Radius + 3;
    charge(uint64_t(Storage) * 2);
    std::vector<int64_t> Forward(Storage, -1), Backward(Storage, -1);
    const bool Odd = Delta % 2 != 0;
    for (int64_t Distance = 0; Distance <= Radius; ++Distance) {
      if (Control.Progress)
        Control.Progress("source_alignment", Result.Work, 0);
      // A path cannot delete more than N lines or insert more than M.
      // Restricting the diagonals also keeps highly unequal inputs efficient.
      const int64_t First = -Distance + 2 * std::max<int64_t>(0, Distance - M);
      const int64_t Last = Distance - 2 * std::max<int64_t>(0, Distance - N);
      for (const bool Reverse : {false, true}) {
        auto &Frontier = Reverse ? Backward : Forward;
        const auto &Opposite = Reverse ? Forward : Backward;
        for (int64_t Diagonal = First; Diagonal <= Last; Diagonal += 2) {
          charge();
          const auto At = Base + Diagonal;
          int64_t X = Distance == 0 ? 0 : -1;
          const auto Accept = [&](int64_t Candidate) {
            const int64_t Y = Candidate - Diagonal;
            if (Candidate >= 0 && Candidate <= N && Y >= 0 && Y <= M)
              X = std::max(X, Candidate);
          };
          if (Diagonal > -Distance && Frontier[At - 1] >= 0)
            Accept(Frontier[At - 1] + 1);
          if (Diagonal < Distance && Frontier[At + 1] >= 0)
            Accept(Frontier[At + 1]);
          if (X < 0) {
            Frontier[At] = -1;
            continue;
          }
          int64_t Y = X - Diagonal;
          const int64_t StartX = X, StartY = Y;
          while (X < N && Y < M &&
                 equal(Reverse ? AEnd - X - 1 : ABegin + X,
                       Reverse ? BEnd - Y - 1 : BBegin + Y)) {
            ++X;
            ++Y;
          }
          Frontier[At] = X;
          const int64_t OtherDiagonal = Delta - Diagonal;
          const int64_t OtherDistance = Reverse ? Distance : Distance - 1;
          if (Reverse == Odd || OtherDistance < 0 ||
              OtherDiagonal < -OtherDistance +
                                  2 * std::max<int64_t>(0, OtherDistance - M) ||
              OtherDiagonal >
                  OtherDistance - 2 * std::max<int64_t>(0, OtherDistance - N))
            continue;
          const int64_t Other = Opposite[Base + OtherDiagonal];
          if (Other < 0 || X < N - Other)
            continue;
          if (Reverse)
            return {AEnd - X, BEnd - Y, AEnd - StartX, BEnd - StartY};
          return {ABegin + StartX, BBegin + StartY, ABegin + X, BBegin + Y};
        }
      }
    }
    llvm_unreachable("finite source edit graph has no middle snake");
  }

  void match(size_t ABegin, size_t AEnd, size_t BBegin, size_t BEnd) {
    charge(0);
    while (ABegin < AEnd && BBegin < BEnd && equal(ABegin, BBegin))
      Matches.emplace_back(ABegin++, BBegin++);
    const size_t OriginalAEnd = AEnd;
    while (ABegin < AEnd && BBegin < BEnd && equal(AEnd - 1, BEnd - 1)) {
      --AEnd;
      --BEnd;
    }
    if (ABegin < AEnd && BBegin < BEnd) {
      if (AEnd - ABegin == 1) {
        for (size_t J = BBegin; J < BEnd; ++J)
          if (equal(ABegin, J)) {
            Matches.emplace_back(ABegin, J);
            break;
          }
      } else {
        const auto Middle = middle(ABegin, AEnd, BBegin, BEnd);
        match(ABegin, Middle.ABegin, BBegin, Middle.BBegin);
        for (size_t I = 0; I < Middle.AEnd - Middle.ABegin; ++I) {
          charge();
          Matches.emplace_back(Middle.ABegin + I, Middle.BBegin + I);
        }
        match(Middle.AEnd, AEnd, Middle.BEnd, BEnd);
      }
    }
    for (size_t I = 0; I < OriginalAEnd - AEnd; ++I) {
      charge();
      Matches.emplace_back(AEnd + I, BEnd + I);
    }
  }

  void row(uint64_t Primary, uint64_t Secondary, SourceLineKind Kind) {
    charge();
    Result.Rows.push_back({Primary, Secondary, Kind});
  }

public:
  Alignment(const SourceAlignmentOptions &Options,
            const ComparisonControl &Control, SourceAlignment &Result)
      : Options(Options), Control(Control), Result(Result) {}

  void run(llvm::ArrayRef<llvm::StringRef> Primary,
           llvm::ArrayRef<llvm::StringRef> Secondary) {
    llvm::StringMap<uint64_t> Symbols;
    const auto Intern = [&](llvm::ArrayRef<llvm::StringRef> Lines,
                            std::vector<uint64_t> &IDs) {
      for (const auto Line : Lines) {
        charge(Line.size());
        charge();
        // StringMap checks complete bytes even if two hashes collide.
        IDs.push_back(Symbols.try_emplace(Line, Symbols.size()).first->second);
        if (Control.Progress)
          Control.Progress("source_lines", Result.Work, 0);
      }
    };
    Intern(Primary, A);
    Intern(Secondary, B);
    match(0, A.size(), 0, B.size());
    size_t I = 0, J = 0;
    const auto Gap = [&](size_t AEnd, size_t BEnd) {
      while (I < AEnd && J < BEnd)
        row(I++, J++, SourceLineKind::Changed);
      while (I < AEnd)
        row(I++, NoSourceLine, SourceLineKind::Deleted);
      while (J < BEnd)
        row(NoSourceLine, J++, SourceLineKind::Inserted);
    };
    for (const auto &[AI, BI] : Matches) {
      Gap(AI, BI);
      row(I++, J++, SourceLineKind::Unchanged);
    }
    Gap(A.size(), B.size());
    charge(0);
    Result.ComputationComplete = true;
  }
};
} // namespace

llvm::StringRef sourceLineKindName(SourceLineKind Kind) {
  switch (Kind) {
  case SourceLineKind::Unchanged:
    return "unchanged";
  case SourceLineKind::Changed:
    return "changed";
  case SourceLineKind::Inserted:
    return "inserted";
  case SourceLineKind::Deleted:
    return "deleted";
  }
  llvm_unreachable("invalid source line kind");
}

SourceAlignment alignSourceLines(llvm::ArrayRef<llvm::StringRef> Primary,
                                 llvm::ArrayRef<llvm::StringRef> Secondary,
                                 const SourceAlignmentOptions &Options,
                                 const ComparisonControl &Control) {
  SourceAlignment Result;
  try {
    Alignment(Options, Control, Result).run(Primary, Secondary);
  } catch (const Interrupted &Stop) {
    Result.State = Stop.State;
    Result.Rows.clear();
  }
  return Result;
}
} // namespace neverd::diff
