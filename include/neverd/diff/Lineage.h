//===- Lineage.h - Evidence across analyzed versions ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Offline correspondence graphs retain original reports, contradictory paths
/// and analyst history. Connectivity is a reported association, never proof of
/// behavior, patch presence, release ancestry or fresh native authentication.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_LINEAGE_H
#define NEVERD_DIFF_LINEAGE_H

#include "neverd/diff/ComparisonJSON.h"
#include "neverd/diff/Control.h"
#include "neverd/diff/Limits.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd::diff {

inline constexpr llvm::StringLiteral LineageSchema = "neverd.bindiff.lineage";
inline constexpr unsigned LineageSchemaVersion = 1;
inline constexpr unsigned LineageReleaseSchemaVersion = 2;

struct LineageSettings {
  uint64_t MaxSourceBytes = limits::diff::kDefaultLineageSourceBytes;
  uint32_t MaxNodes = limits::diff::kDefaultLineageNodes;
  uint32_t MaxEdges = limits::diff::kDefaultLineageEdges;
  uint64_t MaxWork = limits::diff::kDefaultLineageWork;
  std::optional<std::string> validate() const;
};

/// Source bytes and their sole, shared-reader interpretation. The digest is
/// computed from these exact bytes, including their history and settings.
struct LineageEvidence {
  std::string ID, Text;
  SavedComparison Saved;
  /// Exact JSON string size from the shared serializer, retained with Text.
  uint64_t EncodedBytes = 0;
};

struct LineageReport {
  std::shared_ptr<const LineageEvidence> Evidence;
  uint32_t PrimaryVersion = NoIndex, SecondaryVersion = NoIndex;
};

struct LineageVersion {
  std::string ID, Label, Branch;
  ProgramIdentity Identity;
  /// Inventory is borrowed from this retained report, not copied per edge.
  uint32_t Report = NoIndex;
  bool Secondary = false;
  uint32_t FirstNode = 0;
};

/// Analyst-declared release identity, independent of analyzed content identity.
/// Parent links never promote correspondence into patch or ancestry evidence.
struct LineageRelease {
  std::string ID, Label, Branch, PublishedAt;
  uint32_t Version = NoIndex;
  std::vector<uint32_t> Parents, Children;
};

struct LineageNode {
  uint32_t Version = NoIndex, Function = NoIndex, Component = NoIndex;
};

struct LineageEdge {
  /// Original report direction remains visible, although connectivity is
  /// undirected. Parallel evidence from different reports is not collapsed.
  uint32_t Primary = NoIndex, Secondary = NoIndex;
  uint32_t Report = NoIndex, Match = NoIndex;
};

enum class LineageConflictKind : uint8_t { MultipleFunctions, RejectedPair };

struct LineageConflict {
  LineageConflictKind Kind = LineageConflictKind::MultipleFunctions;
  uint32_t Component = NoIndex, Version = NoIndex, Report = NoIndex;
  /// One group, not every pairwise combination. RejectedPair has two members.
  std::vector<uint32_t> Members;
};

struct LineageTraceNode {
  uint32_t Node = NoIndex, Predecessor = NoIndex, Edge = NoIndex;
  uint32_t Distance = 0;
};

class Lineage;

/// Immutable projection with one shared shortest-path witness tree. Only the
/// core can construct it; page consumers cannot inject source coordinates.
class LineageTrace {
public:
  llvm::StringRef revision() const { return Revision; }
  uint32_t source() const { return Source; }
  llvm::ArrayRef<LineageTraceNode> nodes() const { return Nodes; }
  llvm::ArrayRef<uint32_t> edges() const { return Edges; }
  llvm::ArrayRef<uint32_t> conflicts() const { return Conflicts; }
  uint64_t work() const { return Work; }

private:
  friend class Lineage;
  LineageTrace() = default;
  std::string Revision;
  uint32_t Source = NoIndex;
  std::vector<LineageTraceNode> Nodes;
  std::vector<uint32_t> Edges, Conflicts;
  uint64_t Work = 0;
};

/// Published only after complete wrapper validation and graph admission.
class Lineage {
public:
  llvm::StringRef revision() const { return Revision; }
  unsigned schemaVersion() const { return SchemaVersion; }
  llvm::ArrayRef<LineageVersion> versions() const { return Versions; }
  llvm::ArrayRef<LineageRelease> releases() const { return Releases; }
  llvm::ArrayRef<uint32_t> releaseTimeline() const { return ReleaseTimeline; }
  uint64_t releaseLinks() const { return ReleaseLinks; }
  llvm::ArrayRef<LineageReport> reports() const { return Reports; }
  llvm::ArrayRef<LineageNode> nodes() const { return Nodes; }
  llvm::ArrayRef<LineageEdge> edges() const { return Edges; }
  llvm::ArrayRef<LineageConflict> conflicts() const { return Conflicts; }
  uint64_t reportBytes() const { return ReportBytes; }
  /// Retained report and snapshot/release metadata text; not process RSS.
  uint64_t sourceBytes() const { return ReportBytes + MetadataBytes; }
  uint64_t work() const { return Work; }
  llvm::ArrayRef<SavedFunction> functions(uint32_t Version) const;
  std::optional<uint32_t> version(llvm::StringRef ID) const;
  std::optional<uint32_t> release(llvm::StringRef ID) const;
  std::optional<uint32_t> node(uint32_t Version, uint64_t Address) const;
  /// Throws ComparisonInterrupted on stop/exhaustion; no partial projection.
  LineageTrace trace(uint32_t Source, uint64_t MaxWork,
                     const ComparisonControl &Control = {}) const;

private:
  friend std::shared_ptr<const Lineage>
  readLineage(llvm::StringRef, const LineageSettings &, std::string &,
              const ComparisonControl &, const Lineage *);
  friend std::shared_ptr<const Lineage>
  editLineageReleases(const Lineage &, const llvm::json::Object &,
                      const LineageSettings &, std::string &,
                      const ComparisonControl &);
  Lineage() = default;
  void readReleases(const llvm::json::Value *Metadata,
                    const LineageSettings &Settings, uint64_t PriorNodes,
                    uint64_t HeldEdges, uint64_t &HeldBytes,
                    const ComparisonControl &Control);
  void updateRevision(uint64_t MaxWork, const ComparisonControl &Control);
  void admitSerialization(uint64_t MaxWork, const ComparisonControl &Control);
  std::string Revision;
  unsigned SchemaVersion = LineageSchemaVersion;
  std::vector<LineageVersion> Versions;
  std::vector<LineageRelease> Releases;
  std::vector<uint32_t> ReleaseTimeline;
  std::vector<LineageReport> Reports;
  std::vector<LineageNode> Nodes;
  std::vector<LineageEdge> Edges;
  std::vector<LineageConflict> Conflicts;
  std::vector<std::vector<uint32_t>> Adjacency, ComponentConflicts;
  uint64_t ReportBytes = 0, MetadataBytes = 0, Work = 0;
  uint64_t ReleaseLinks = 0;
  uint64_t EncodedReportBytes = 0;
};

/// Strict lineage wrapper plus the existing readComparison for every unique
/// report. Optional cache reuses exact validated bytes, with current admission
/// still charged. Errors return null; cancellation/deadline/resource stops
/// throw.
std::shared_ptr<const Lineage>
readLineage(llvm::StringRef Text, const LineageSettings &Settings,
            std::string &Error, const ComparisonControl &Control = {},
            const Lineage *Cache = nullptr);

/// Source-only persistence; reopen always reconstructs the graph and conflicts.
std::string writeLineage(const Lineage &Value,
                         const ComparisonControl &Control = {});

/// Apply a revision-bound {revision, upsert, remove, parent_updates} edit.
/// Existing upserts may omit parents to retain them. Validation,
/// budget accounting and cancellation precede publication by the caller.
std::shared_ptr<const Lineage>
editLineageReleases(const Lineage &Value, const llvm::json::Object &Edit,
                    const LineageSettings &Settings, std::string &Error,
                    const ComparisonControl &Control = {});

} // namespace neverd::diff

#endif // NEVERD_DIFF_LINEAGE_H
