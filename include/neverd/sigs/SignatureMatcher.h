//===- SignatureMatcher.h - FLIRT pattern matching engine ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Engine for matching FLIRT signature patterns against binary data.
///
/// Matching process:
///   1. Compare leading bytes (with wildcard mask) at candidate address.
///   2. Verify CRC16 of trailing bytes (after leading pattern).
///   3. Optionally verify tail bytes at known offsets.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_SIGNATUREMATCHER_H
#define NEVERD_SIGS_SIGNATUREMATCHER_H

#include "neverd/sigs/Signature.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

namespace neverd {
namespace sigs {

class SignatureMatcher {
public:
  /// Compute the FLIRT CRC16 of a byte range.
  static uint16_t computeCRC16(const uint8_t *Data, size_t Len);

  /// Match a single pattern module against binary data at the given offset.
  /// \p Data points to the candidate function start.
  /// \p Available is the number of bytes available from that point.
  static bool matchPattern(const PatternModule &Mod, const uint8_t *Data,
                           size_t Available);
  static bool matchPattern(const StoredModule &Mod, const uint8_t *Data,
                           size_t Available);

  /// Bytes a module states exactly, across its leading pattern and its tail.
  ///
  /// A wildcard stands for a byte the linker rewrites, so a pattern made
  /// mostly of them agrees with far more code than it describes.  Counting
  /// what is left is how a caller sets a floor on that.
  static size_t fixedByteCount(const PatternModule &Mod);
  static size_t fixedByteCount(const StoredModule &Mod);

  /// The fewest bytes a module may state exactly and still identify a
  /// routine.
  ///
  /// So many exact bytes are several instructions of one specific routine,
  /// which no unrelated function reaches by coincidence.  Below that a
  /// pattern is mostly wildcards and agrees with a great deal of code -- a
  /// function whose every instruction is relocated states nothing at all --
  /// so PatternGenerator does not write such a line, and naming a
  /// personality routine demands the same floor.
  static constexpr size_t MinStatedBytes = SignatureLimits::MinStatedBytes;

  /// True when the leading pattern, the CRC span, and the tail together reach
  /// the end of the function the module claims to describe.
  ///
  /// A module that stops short still matches soundly, but it has only checked
  /// a prefix: two routines sharing a prologue and a first block are the same
  /// thing to it.  That is tolerable when the answer is a name shown to a
  /// reader and not when it is a name the rest of the analysis will act on --
  /// a personality decides which schema the language data is decoded with, so
  /// a caller settling one asks for whole-function agreement first.
  static bool isFullyVerified(const PatternModule &Mod);
  static bool isFullyVerified(const StoredModule &Mod);

  /// Scan a region of binary data for all matching patterns from a module set.
  /// Calls \p Callback with the region offset and matching module.
  using MatchCallback =
      std::function<void(uint64_t Offset, const PatternModule &Mod)>;

  /// Sparse byte-decision index for the fixed-width leading pattern.
  ///
  /// At a decision, matching can continue through only the edge for the input
  /// byte and the wildcard edge.  Modules are partitioned rather than copied,
  /// so each reaches exactly one leaf and is fully verified at most once.
  struct HashIndex {
    static constexpr size_t kIndexedBytes = SignatureLimits::IndexedBytes;
    static constexpr size_t kLeafCandidates = SignatureLimits::LeafCandidates;
    static constexpr size_t kNoNode = std::numeric_limits<size_t>::max();

    struct Edge {
      uint8_t Value = 0;
      size_t Child = kNoNode;
    };

    struct Node {
      uint8_t Offset = 0;
      size_t WildcardChild = kNoNode;
      std::vector<Edge> ExactChildren;
      llvm::ArrayRef<size_t> Candidates;

      bool isLeaf() const { return !Candidates.empty(); }
    };

    std::vector<Node> Nodes;
    size_t Root = kNoNode;

    void build(const std::vector<PatternModule> &Modules);
    void build(llvm::ArrayRef<StoredModule> Modules);
    uint16_t keyOf(const PatternModule &Mod) const;
    bool isWildcardKey(const PatternModule &Mod) const;

    /// Full pattern verifications scheduled for one input prefix.  This is a
    /// deterministic performance counter; it does not perform a match.
    size_t candidateCount(const uint8_t *Data, size_t Available) const;

    /// !caller records never enter the byte decision tree. Lookup names from
    /// an established caller instead, then verify only those complete bodies.
    /// Existing !refs bodies participate too, so callers can reuse explicitly
    /// contextual records without duplicating them. Ordinary nominal byte
    /// claims do not acquire incoming support merely by being loaded.
    /// Numeric indices retain the same copy/move lifetime as byte candidates.
    struct CallerEntry {
      size_t Module = 0;
      size_t PublicName = 0;
      size_t NameHash = 0;
    };
    bool hasCallerModules() const {
      return CallerStorage && !CallerStorage->empty();
    }
    llvm::ArrayRef<size_t> vtableModules() const {
      return VTableStorage ? llvm::ArrayRef(*VTableStorage)
                           : llvm::ArrayRef<size_t>();
    }
    llvm::ArrayRef<size_t> loadConfigModules() const {
      return LoadConfigStorage ? llvm::ArrayRef(*LoadConfigStorage)
                               : llvm::ArrayRef<size_t>();
    }
    bool hasCodeLayouts() const {
      return CodeLayoutStorage && !CodeLayoutStorage->empty();
    }
    llvm::ArrayRef<CallerEntry>
    codeLayoutModules(llvm::ArrayRef<StoredModule> Modules,
                      std::string_view Name) const;
    bool hasDataContext() const { return DataContext; }
    llvm::ArrayRef<size_t> dataAnchorModules() const {
      return DataAnchorStorage ? llvm::ArrayRef(*DataAnchorStorage)
                               : llvm::ArrayRef<size_t>();
    }
    llvm::ArrayRef<CallerEntry>
    callerModules(llvm::ArrayRef<StoredModule> Modules,
                  std::string_view Name) const;

  private:
    std::shared_ptr<const std::vector<CallerEntry>> CodeLayoutStorage;
    std::shared_ptr<const std::vector<size_t>> CodeLayoutNameLengths;
    std::shared_ptr<const std::vector<size_t>> VTableStorage;
    std::shared_ptr<const std::vector<size_t>> LoadConfigStorage;
    bool DataContext = false;
    std::shared_ptr<const std::vector<size_t>> DataAnchorStorage;
    std::shared_ptr<const std::vector<CallerEntry>> CallerStorage;
    /// Leaves view one immutable permutation instead of allocating per leaf.
    /// Copies retain its lifetime; rebuilding an index replaces only its own
    /// storage, so copies and moved indexes keep valid views.
    std::shared_ptr<const std::vector<size_t>> CandidateStorage;
  };

  static void scanRegion(const uint8_t *Data, size_t Size,
                         const std::vector<PatternModule> &Modules,
                         MatchCallback Callback);

  /// Optimized scan: only check at known function entry points.
  /// Uses the sparse leading-byte decision index above.
  static void scanAtAddresses(const uint8_t *ImageBase, size_t ImageSize,
                              uint64_t BaseVA,
                              const std::vector<uint64_t> &FuncEntries,
                              const std::vector<PatternModule> &Modules,
                              MatchCallback Callback);

  /// Scan with an index already built for \p Modules.  Reusing one across
  /// executable segments avoids rebuilding a large database for each segment.
  static void scanAtAddresses(const uint8_t *ImageBase, size_t ImageSize,
                              uint64_t BaseVA,
                              const std::vector<uint64_t> &FuncEntries,
                              const std::vector<PatternModule> &Modules,
                              const HashIndex &Index, MatchCallback Callback);

  /// A module that matches at an address: its index in the module set.
  struct Hit {
    uint64_t Address = 0;
    size_t Module = 0;
  };

  /// Deterministic work counters for one scan, summed across worker blocks.
  /// A CRC request may reuse a value computed for another candidate at the
  /// same input address. CRCBytes counts only bytes actually checksummed.
  struct ScanStatistics {
    uint64_t Entries = 0;
    uint64_t Candidates = 0;
    uint64_t CRCRequests = 0;
    uint64_t CRCComputations = 0;
    uint64_t CRCBytes = 0;
  };

  /// The modules of \p Modules that match at each of \p FuncEntries, in the
  /// order \ref scanAtAddresses reports them, with the entries checked on
  /// the worker threads.
  static std::vector<Hit> findAtAddresses(const uint8_t *ImageBase,
                                          size_t ImageSize, uint64_t BaseVA,
                                          llvm::ArrayRef<uint64_t> FuncEntries,
                                          llvm::ArrayRef<StoredModule> Modules,
                                          const HashIndex &Index);

  /// As above, resetting and filling \p Statistics for this call. The overload
  /// without counters avoids recording work in the matching loop.
  static std::vector<Hit> findAtAddresses(const uint8_t *ImageBase,
                                          size_t ImageSize, uint64_t BaseVA,
                                          llvm::ArrayRef<uint64_t> FuncEntries,
                                          llvm::ArrayRef<StoredModule> Modules,
                                          const HashIndex &Index,
                                          ScanStatistics &Statistics);
};

} // namespace sigs
} // namespace neverd

#endif // NEVERD_SIGS_SIGNATUREMATCHER_H
