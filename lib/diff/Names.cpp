//===- Names.cpp - Stable spellings of comparison vocabulary --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The names saved comparisons and the C API spell steps, change kinds,
/// overrides, row kinds and states with, read from their definition tables.
///
//===----------------------------------------------------------------------===//

#include "neverd/diff/Comparison.h"
#include "neverd/diff/FunctionDelta.h"

#include "llvm/ADT/StringSwitch.h"

#include <array>

namespace neverd::diff {

namespace {

struct StepInfo {
  llvm::StringLiteral Name;
  double Confidence;
  llvm::StringLiteral Description;
};

constexpr StepInfo FunctionSteps[] = {
#define NEVERD_DIFF_FUNCTION_STEP(Id, Name, Confidence, Description)           \
  {Name, Confidence, Description},
#include "neverd/diff/MatchSteps.def"
};

constexpr StepInfo BlockSteps[] = {
#define NEVERD_DIFF_BLOCK_STEP(Id, Name, Confidence, Description)              \
  {Name, Confidence, Description},
#include "neverd/diff/MatchSteps.def"
};

struct ChangeInfo {
  uint32_t Bit;
  char Letter;
  llvm::StringLiteral Name;
  llvm::StringLiteral Description;
};

constexpr ChangeInfo Changes[] = {
#define NEVERD_DIFF_CHANGE(Id, Bit, Letter, Name, Description)                 \
  {Bit, Letter, Name, Description},
#include "neverd/diff/ChangeKinds.def"
};

constexpr llvm::StringLiteral RowKinds[] = {
    "identical", "moved",    "constant", "operand",
    "replaced",  "inserted", "deleted",  "uncertain",
};

constexpr llvm::StringLiteral AlignmentProfiles[] = {"paired-blocks-v1",
                                                     "single-block-spans-v1"};

} // namespace

llvm::StringRef stepName(FunctionStep Step) {
  return FunctionSteps[size_t(Step)].Name;
}
llvm::StringRef stepDescription(FunctionStep Step) {
  return FunctionSteps[size_t(Step)].Description;
}
double stepConfidence(FunctionStep Step) {
  return FunctionSteps[size_t(Step)].Confidence;
}
std::optional<FunctionStep> functionStepNamed(llvm::StringRef Name) {
  for (size_t I = 0; I < std::size(FunctionSteps); ++I)
    if (FunctionSteps[I].Name == Name)
      return static_cast<FunctionStep>(I);
  return std::nullopt;
}
llvm::StringRef stepName(BlockStep Step) {
  return BlockSteps[size_t(Step)].Name;
}
llvm::StringRef stepDescription(BlockStep Step) {
  return BlockSteps[size_t(Step)].Description;
}
std::optional<BlockStep> blockStepNamed(llvm::StringRef Name) {
  for (size_t I = 0; I < std::size(BlockSteps); ++I)
    if (BlockSteps[I].Name == Name)
      return static_cast<BlockStep>(I);
  return std::nullopt;
}

std::string changeLetters(uint32_t Mask) {
  std::string Out;
  for (const ChangeInfo &C : Changes)
    Out += (Mask >> C.Bit) & 1 ? C.Letter : '.';
  return Out;
}
std::vector<llvm::StringRef> changeNames(uint32_t Mask) {
  std::vector<llvm::StringRef> Out;
  for (const ChangeInfo &C : Changes)
    if ((Mask >> C.Bit) & 1)
      Out.push_back(C.Name);
  return Out;
}
std::optional<ChangeKind> changeNamed(llvm::StringRef Name) {
  for (const ChangeInfo &C : Changes)
    if (C.Name == Name)
      return static_cast<ChangeKind>(1u << C.Bit);
  return std::nullopt;
}
llvm::StringRef changeDescription(ChangeKind Kind) {
  for (const ChangeInfo &C : Changes)
    if (Kind == (1u << C.Bit))
      return C.Description;
  return {};
}

llvm::StringRef overrideName(OverrideKind Kind) {
  switch (Kind) {
  case OverrideKind::Match:
    return "match";
  case OverrideKind::Unmatch:
    return "unmatch";
  case OverrideKind::Confirm:
    return "confirm";
  }
  return {};
}
std::optional<OverrideKind> overrideNamed(llvm::StringRef Name) {
  return llvm::StringSwitch<std::optional<OverrideKind>>(Name)
      .Case("match", OverrideKind::Match)
      .Case("unmatch", OverrideKind::Unmatch)
      .Case("confirm", OverrideKind::Confirm)
      .Default(std::nullopt);
}

llvm::StringRef stateName(ComparisonState State) {
  switch (State) {
  case ComparisonState::Complete:
    return "complete";
  case ComparisonState::Cancelled:
    return "cancelled";
  case ComparisonState::TimedOut:
    return "timed_out";
  case ComparisonState::ResourceLimited:
    return "resource_limited";
  }
  return {};
}

llvm::StringRef rowKindName(RowKind Kind) { return RowKinds[size_t(Kind)]; }
llvm::StringRef alignmentProfileName(InstructionAlignmentProfile Profile) {
  return AlignmentProfiles[size_t(Profile)];
}
std::optional<InstructionAlignmentProfile>
alignmentProfileNamed(llvm::StringRef Name) {
  for (size_t I = 0; I < std::size(AlignmentProfiles); ++I)
    if (AlignmentProfiles[I] == Name)
      return static_cast<InstructionAlignmentProfile>(I);
  return std::nullopt;
}
std::optional<ComparisonState> stateNamed(llvm::StringRef Name) {
  for (auto State :
       {ComparisonState::Complete, ComparisonState::Cancelled,
        ComparisonState::TimedOut, ComparisonState::ResourceLimited})
    if (Name == stateName(State))
      return State;
  return {};
}
std::optional<RowKind> rowKindNamed(llvm::StringRef Name) {
  for (size_t I = 0; I < std::size(RowKinds); ++I)
    if (RowKinds[I] == Name)
      return static_cast<RowKind>(I);
  return std::nullopt;
}

} // namespace neverd::diff
