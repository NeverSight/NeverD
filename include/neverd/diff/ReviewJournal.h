//===- ReviewJournal.h - Input-bound analyst assessments -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Append-only manual assessments, separate from computed comparison evidence.
/// Digest links detect inconsistent records; they do not authenticate authors.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_REVIEWJOURNAL_H
#define NEVERD_DIFF_REVIEWJOURNAL_H

#include "neverd/diff/Control.h"
#include "neverd/diff/Program.h"

#include "llvm/Support/JSON.h"

namespace neverd::diff {
struct ComparisonResult;

inline constexpr llvm::StringLiteral ReviewJournalProfile = "analyst-review-v1";
enum class ReviewAssessment : uint8_t {
  Investigating,
  RelevantChange,
  UnrelatedChange,
  CandidateFix,
  PatchPresent,
  PatchAbsent,
  PartialPatch,
  RevertedPatch,
  Inconclusive,
};
llvm::StringRef reviewAssessmentName(ReviewAssessment Assessment);
std::optional<ReviewAssessment> reviewAssessmentNamed(llvm::StringRef Name);

struct ReviewDraft {
  uint64_t Primary = 0, Secondary = 0;
  std::string Case, Author, Note, Reference;
  ReviewAssessment Assessment = ReviewAssessment::Investigating;
  bool operator==(const ReviewDraft &) const = default;
};
struct ReviewRecord : ReviewDraft {
  uint32_t Sequence = 0;
  /// Previous journal event and previous event for this case/function pair.
  std::string ID, Previous, Supersedes;
  bool operator==(const ReviewRecord &) const = default;
};
struct ReviewJournal {
  std::vector<ReviewRecord> Records;
  llvm::StringRef head() const {
    return Records.empty() ? llvm::StringRef() : Records.back().ID;
  }
  bool operator==(const ReviewJournal &) const = default;
};

/// Reject invalid fields, sequence/digest links or size. No content is repaired
/// or inferred. The complete input identities are part of each record digest.
std::optional<std::string> validateReviewJournal(
    const ReviewJournal &Journal, const ProgramIdentity &Primary,
    const ProgramIdentity &Secondary, const ComparisonControl &Control = {});
/// Adds an explicitly authored assessment of an accepted pair. ExpectedHead
/// prevents lost updates; failed admission leaves the journal unchanged.
/// Undoing a match does not undo this independent history. Add a new record to
/// revise it.
std::optional<std::string> appendReview(ComparisonResult &Result,
                                        const Program &Primary,
                                        const Program &Secondary,
                                        const ReviewDraft &Draft,
                                        llvm::StringRef ExpectedHead);
/// Whether the original pair is still accepted, independent of latest status.
bool reviewPairActive(const ReviewRecord &Record,
                      const ComparisonResult &Result, const Program &Primary,
                      const Program &Secondary);

llvm::json::Object reviewRecordJSON(const ReviewRecord &Record);
llvm::json::Object reviewJournalJSON(const ReviewJournal &Journal);
std::optional<ReviewJournal>
readReviewJournal(const llvm::json::Value &Value,
                  const ProgramIdentity &Primary,
                  const ProgramIdentity &Secondary, std::string &Error,
                  const ComparisonControl &Control = {});
} // namespace neverd::diff

#endif // NEVERD_DIFF_REVIEWJOURNAL_H
