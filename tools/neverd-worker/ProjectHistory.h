//===- ProjectHistory.h - Durable worker edit history --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Immutable committed user state and journaled publication.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_WORKER_PROJECTHISTORY_H
#define NEVERD_TOOLS_NEVERD_WORKER_PROJECTHISTORY_H

#include "Protocol.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::worker {
/// Replaces a file with the given text in one atomic step.
using AtomicWriter = void (*)(const std::filesystem::path &,
                              const std::string &);

// Durable user-edit history above the public C ABI. No analysis objects or
// pointers enter the store; recovery only replaces known user sidecar files.
class ProjectHistory {
public:
  /// An independently owned, admitted row. Handles never retain an entire
  /// previous snapshot or borrow the SDK's storage. Encoding is memoized on
  /// the owning worker thread, as for immutable history commands.
  class PatchRow {
  public:
    const Json &value() const { return value_; }
    std::uint64_t address() const { return address_; }
    const std::string &encoded() const;
    /// Canonical sidecar row, including its indentation inside the array.
    /// The view belongs to this immutable row, as does its compact encoding.
    std::string_view sidecarRow() const;

  private:
    friend class ProjectHistory;
    explicit PatchRow(Json normalized);
    Json value_;
    std::uint64_t address_;
    mutable std::string text_;
    mutable std::string sidecarText_;
  };
  using PatchRows = std::vector<std::shared_ptr<const PatchRow>>;
  static std::shared_ptr<const PatchRow> patchRow(Json row);
  /// A row of \p table, admitted, where the history keeps that table row by
  /// row (keepsRows): patches and renames, each row shared by every state that
  /// keeps it and printed once.
  static std::shared_ptr<const PatchRow> tableRow(const std::string &table,
                                                  Json row);
  static bool keepsRows(std::string_view table);

  ProjectHistory(std::filesystem::path binary, std::string sourceHash,
                 std::string engineVersion, bool readOnly, AtomicWriter writer);
  static bool recoveryPending(const std::filesystem::path &binary);
  bool recovered() const { return recovered_; }
  const Json &committedState() const;
  const Json &committedTable(const std::string &name) const;
  /// The same rows, held as the state that committed them holds them: they
  /// stay what they are through the edits that follow.
  std::shared_ptr<const Json> committedTableHeld(const std::string &name) const;
  const PatchRows &committedPatches() const;
  /// The committed rows of a table kept row by row.
  const PatchRows &committedRows(const std::string &table) const;
  Json listing(std::size_t offset, std::size_t limit) const;
  /// Navigation/logging uses a patch row's virtual address, not its file key.
  static std::uint64_t displayAddress(const Json &command);
  /// Record an edit: {"kind","address","before","after"}, the row of the
  /// kind's table at that address on either side (null where none), and for
  /// a table of rows, optional "rows" [{"address","before","after"}]: the
  /// other rows the same edit changes, undone and redone with it.
  void stage(Json command);
  Json next(bool redo) const;
  void advance(bool redo);
  void persist(Json state);
  /// As persist, but the history holds the state at once and a thread of its
  /// own writes it: the same transaction, in order, joined with what is queued
  /// behind one already writing, a few milliseconds after the caller goes on.
  /// What the files say of it holds from finishWrites, and a process that ends
  /// sooner leaves them as they were before it, or a journal that completes it.
  /// A write that fails stays queued and fails the next edit that finds it.
  void persistLater(Json state);
  /// As persist and persistLater, for an edit that changes \p table alone:
  /// \p rows are its rows after the edit, and every other table is the
  /// committed one, neither copied nor read again.
  void persistTable(const std::string &table, Json rows);
  void persistTableLater(const std::string &table, Json rows);
  /// Waits for the writes persistLater queued.  Throws what fails them, and the
  /// next call tries them again.
  void finishWrites();
  /// Whether writes persistLater queued are still on their way to the files.
  bool writesPending();
  /// As finishWrites, for a caller about to read what the files hold whatever
  /// that is: a write that fails is given up.
  void settleWrites() noexcept;
  /// Reuse admitted rows, retaining all other tables. Sequence ordering,
  /// uniqueness, file checks and transaction budgets still apply.
  void persistPatches(PatchRows rows);
  /// As persistTable and persistTableLater, for a table kept row by row: \p
  /// rows are its rows after the edit, the unchanged ones shared and printed
  /// once.
  void persistRows(const std::string &table, PatchRows rows);
  void persistRowsLater(const std::string &table, PatchRows rows);
  /// Called after each persist commits the project's files.
  void onCommit(std::function<void()> hook) { committed_ = std::move(hook); }
  void reset();
  /// Whether the project's files hold what this history last committed and
  /// its commands describe them: false after git, another tool or a person
  /// changed the files.
  bool current() const;
  std::string blockedReason() const { return blocked_; }
  /// Refuses an edit while the history is blocked (blockedReason).
  void requireUsable() const;
  void verifyLoadedState(const Json &state);
  static Json normalizedState(Json state);
  /// Read and validate committed user tables without loading edit history or
  /// mutating the project. This does not replay a pending transaction.
  static Json readState(const std::filesystem::path &binary);
  /// The state table a history command kind changes (UserStateTables.def),
  /// or null for an unknown kind.
  static const char *tableOfKind(std::string_view kind);

private:
  std::filesystem::path binary_, historyPath_, journalPath_;
  std::string hash_, engineVersion_, blocked_;
  bool readOnly_, recovered_ = false;
  std::uintmax_t fileSize_ = 0;
  std::filesystem::file_time_type fileTime_;
  AtomicWriter writer_;
  std::function<void()> committed_;
  struct Command;
  std::vector<std::shared_ptr<const Command>> commands_;
  struct Snapshot;
  struct State;
  struct Transaction;
  class Writes;
  /// Shared with copies used for rollback: what the files have yet to take does
  /// not roll back with an edit that failed after it.
  std::shared_ptr<Writes> writes_;
  // Copies used for rollback share only immutable, validated committed data.
  std::shared_ptr<const Snapshot> snapshot_;
  /// A journal this live history published before a failed sidecar write.
  /// Only that exact transaction can be completed by a same-session retry.
  Json interruptedWrite_;
  std::size_t cursor_ = 0;
  Json diskState() const;
  std::shared_ptr<const Snapshot> readSnapshot(Json *document = nullptr) const;
  std::size_t commandTextSize() const;
  std::string document(std::string_view encodedState) const;
  void recover();
  /// \p state is the whole state, or where \p table is given that table's rows.
  void persist(Json state, std::optional<PatchRows> patches, bool later,
               const std::string *table = nullptr);
};
} // namespace neverd::worker

#endif // NEVERD_TOOLS_NEVERD_WORKER_PROJECTHISTORY_H
