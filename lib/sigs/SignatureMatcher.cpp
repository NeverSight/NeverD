//===- SignatureMatcher.cpp - FLIRT pattern matching engine ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/SignatureMatcher.h"

#include "neverd/support/Parallel.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/bit.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>

using namespace neverd::sigs;

namespace {

#define NEVERD_PATTERN_VALUE(Name, Value)                                      \
  [[maybe_unused]] constexpr unsigned Name = Value;
#include "neverd/sigs/PatternSyntax.def"

/// One entry for each value of a byte.
constexpr size_t ByteValues = size_t{1} << CHAR_BIT;

/// Bytes are compared a word at a time: as many as one byte of a module's
/// stated bits covers.
constexpr size_t WordBytes = sizeof(uint64_t);
static_assert(WordBytes == CHAR_BIT, "a word's stated bits are one byte");

constexpr std::array<uint16_t, ByteValues> makeCRC16Table() {
  std::array<uint16_t, ByteValues> Table{};
  for (size_t I = 0; I < Table.size(); ++I) {
    uint16_t Value = static_cast<uint16_t>(I);
    for (unsigned Bit = 0; Bit < CHAR_BIT; ++Bit)
      Value = static_cast<uint16_t>((Value >> 1) ^
                                    ((Value & 1) ? CRC16Polynomial : 0u));
    Table[I] = Value;
  }
  return Table;
}

constexpr auto CRC16Table = makeCRC16Table();

/// For a byte of stated bits, the mask of the bytes they state in a word
/// read little-endian: byte K is all ones when bit K is set.
constexpr std::array<uint64_t, ByteValues> makeByteMasks() {
  std::array<uint64_t, ByteValues> Masks{};
  for (size_t Bits = 0; Bits < Masks.size(); ++Bits)
    for (unsigned Byte = 0; Byte < WordBytes; ++Byte)
      if ((Bits >> Byte) & 1)
        Masks[Bits] |= uint64_t(std::numeric_limits<uint8_t>::max())
                       << (Byte * CHAR_BIT);
  return Masks;
}

constexpr auto ByteMasks = makeByteMasks();

bool checkedAdd(size_t Left, size_t Right, size_t &Result) {
  if (Right > std::numeric_limits<size_t>::max() - Left)
    return false;
  Result = Left + Right;
  return true;
}

// What the rules below need of a module, for each of its two forms: the
// sizes of its leading pattern and tail, whether data agrees with a stretch
// of either, and how many bytes a stretch states.

size_t leadingCount(const PatternModule &Mod) {
  return Mod.LeadingBytes.size();
}
size_t tailCount(const PatternModule &Mod) { return Mod.TailBytes.size(); }
size_t leadingCount(const StoredModule &Mod) { return Mod.LeadingCount; }
size_t tailCount(const StoredModule &Mod) { return Mod.TailCount; }

bool bytesMatch(const std::vector<PatternByte> &Pattern, const uint8_t *Data,
                size_t Count) {
  for (size_t I = 0; I < Count; ++I)
    if (!Pattern[I].IsWildcard && Data[I] != Pattern[I].Value)
      return false;
  return true;
}

size_t statedBytes(const std::vector<PatternByte> &Pattern, size_t Count) {
  size_t Stated = 0;
  for (size_t I = 0; I < Count; ++I)
    Stated += Pattern[I].IsWildcard ? 0 : 1;
  return Stated;
}

/// Whether \p Data agrees with the \p Count bytes of \p Mod from its byte
/// \p First.
bool bytesMatch(const StoredModule &Mod, size_t First, const uint8_t *Data,
                size_t Count) {
  const uint8_t *const Pattern = Mod.Bytes + First;
  size_t I = 0;
  // A word at a time, while their stated bits are one byte.
  if (First % WordBytes == 0) {
    for (; I + WordBytes <= Count; I += WordBytes) {
      const uint8_t Stated = Mod.Stated[(First + I) / CHAR_BIT];
      const uint64_t Differ = llvm::support::endian::read64le(Data + I) ^
                              llvm::support::endian::read64le(Pattern + I);
      if (Differ & ByteMasks[Stated])
        return false;
    }
  }
  for (; I < Count; ++I)
    if (Mod.isStated(First + I) && Data[I] != Pattern[I])
      return false;
  return true;
}

size_t statedBytes(const StoredModule &Mod, size_t First, size_t Count) {
  size_t Stated = 0;
  while (Count && First % CHAR_BIT) {
    Stated += Mod.isStated(First++);
    --Count;
  }
  constexpr size_t WordBits = std::numeric_limits<uint64_t>::digits;
  while (Count >= WordBits) {
    Stated += llvm::popcount(
        llvm::support::endian::read64le(Mod.Stated + First / CHAR_BIT));
    First += WordBits;
    Count -= WordBits;
  }
  while (Count >= CHAR_BIT) {
    Stated += llvm::popcount(Mod.Stated[First / CHAR_BIT]);
    First += CHAR_BIT;
    Count -= CHAR_BIT;
  }
  if (Count)
    Stated += llvm::popcount(static_cast<uint8_t>(
        Mod.Stated[First / CHAR_BIT] & ((unsigned{1} << Count) - 1)));
  return Stated;
}

bool leadingMatches(const PatternModule &Mod, const uint8_t *Data,
                    size_t Count) {
  return bytesMatch(Mod.LeadingBytes, Data, Count);
}
bool tailMatches(const PatternModule &Mod, const uint8_t *Data, size_t Count) {
  return bytesMatch(Mod.TailBytes, Data, Count);
}
bool leadingMatches(const StoredModule &Mod, const uint8_t *Data,
                    size_t Count) {
  return bytesMatch(Mod, 0, Data, Count);
}
bool tailMatches(const StoredModule &Mod, const uint8_t *Data, size_t Count) {
  return bytesMatch(Mod, Mod.LeadingCount, Data, Count);
}

size_t statedLeading(const PatternModule &Mod, size_t Count) {
  return statedBytes(Mod.LeadingBytes, Count);
}
size_t statedTail(const PatternModule &Mod, size_t Count) {
  return statedBytes(Mod.TailBytes, Count);
}
size_t statedLeading(const StoredModule &Mod, size_t Count) {
  return statedBytes(Mod, 0, Count);
}
size_t statedTail(const StoredModule &Mod, size_t Count) {
  return statedBytes(Mod, Mod.LeadingCount, Count);
}

// The rules, one set for both forms.

struct DirectCRC {
  void candidate() {}
  uint16_t crc(const uint8_t *Data, size_t Leading, uint8_t Length) {
    return SignatureMatcher::computeCRC16(Data + Leading, Length);
  }
};

/// Candidates often share a prologue and CRC window. Cache checksums only
/// for this input address, after each candidate has passed its bounds and
/// leading-byte checks. A different leading width starts a fresh cache.
/// Storage is bounded by the pattern format's byte-sized CRC length, not
/// by the number of modules. Negative prefixes never initialize the arrays.
template <bool Track> class EntryCRC {
public:
  explicit EntryCRC(SignatureMatcher::ScanStatistics *Statistics = nullptr)
      : Statistics(Statistics) {}

  void candidate() {
    if constexpr (Track)
      ++Statistics->Candidates;
  }

  // Keep the cache lookup and checksum loop off the common prefix-mismatch
  // path; otherwise their code and register pressure penalize negative scans.
  LLVM_ATTRIBUTE_NOINLINE uint16_t crc(const uint8_t *Data, size_t Leading,
                                       uint8_t Length) {
    if constexpr (Track)
      ++Statistics->CRCRequests;
    if (Leading != CachedLeading) {
      Known.fill(0);
      CachedLeading = Leading;
    }
    const size_t Word = Length / WordBits;
    const uint64_t Bit = uint64_t{1} << (Length % WordBits);
    if ((Known[Word] & Bit) == 0) {
      Values[Length] = SignatureMatcher::computeCRC16(Data + Leading, Length);
      Known[Word] |= Bit;
      if constexpr (Track) {
        ++Statistics->CRCComputations;
        Statistics->CRCBytes += Length;
      }
    }
    return Values[Length];
  }

private:
  static constexpr size_t WordBits = std::numeric_limits<uint64_t>::digits;
  SignatureMatcher::ScanStatistics *Statistics;
  size_t CachedLeading = std::numeric_limits<size_t>::max();
  std::array<uint64_t, ByteValues / WordBits> Known;
  std::array<uint16_t, ByteValues> Values;
};

template <typename Module> size_t effectiveLeadingCount(const Module &Mod) {
  return Mod.TotalLen == 0 ? leadingCount(Mod)
                           : std::min(leadingCount(Mod), size_t{Mod.TotalLen});
}

template <typename Module, typename Checksum>
// Candidates that fail bounds or leading bytes need no per-candidate call.
LLVM_ATTRIBUTE_ALWAYS_INLINE bool matchModule(const Module &Mod,
                                              const uint8_t *Data,
                                              size_t Available, Checksum &CRC) {
  CRC.candidate();
  const size_t Leading = leadingCount(Mod);
  size_t MatchLimit = Mod.TotalLen;
  if (MatchLimit == 0) {
    if (!checkedAdd(Leading, Mod.CRCLen, MatchLimit) ||
        !checkedAdd(MatchLimit, tailCount(Mod), MatchLimit))
      return false;
  }
  if (MatchLimit > Available || (MatchLimit != 0 && !Data))
    return false;
  if (!leadingMatches(Mod, Data, std::min(Leading, MatchLimit)))
    return false;

  // The CRC covers the bytes after the leading pattern.
  if (Mod.CRCLen != 0) {
    size_t CRCEnd = 0;
    if (Leading > MatchLimit || !checkedAdd(Leading, Mod.CRCLen, CRCEnd) ||
        CRCEnd > MatchLimit)
      return false;
    if (CRC.crc(Data, Leading, Mod.CRCLen) != Mod.CRC16)
      return false;
  }

  size_t TailStart = 0;
  if (!checkedAdd(Leading, Mod.CRCLen, TailStart))
    return false;
  if (TailStart >= MatchLimit)
    return true;
  return tailMatches(Mod, Data + TailStart,
                     std::min(tailCount(Mod), MatchLimit - TailStart));
}

template <typename Module> size_t fixedBytes(const Module &Mod) {
  const size_t MatchLimit =
      Mod.TotalLen == 0 ? std::numeric_limits<size_t>::max() : Mod.TotalLen;
  size_t Fixed = statedLeading(Mod, std::min(leadingCount(Mod), MatchLimit));

  size_t TailStart = 0;
  if (!checkedAdd(leadingCount(Mod), Mod.CRCLen, TailStart) ||
      TailStart > MatchLimit)
    return Fixed;
  return Fixed +
         statedTail(Mod, std::min(tailCount(Mod), MatchLimit - TailStart));
}

template <typename Module> bool fullyVerified(const Module &Mod) {
  if (Mod.TotalLen == 0)
    return false;
  if (leadingCount(Mod) >= Mod.TotalLen)
    return Mod.CRCLen == 0;
  size_t Verified = leadingCount(Mod);
  if (!checkedAdd(Verified, Mod.CRCLen, Verified) || Verified > Mod.TotalLen)
    return false;
  if (!checkedAdd(Verified, tailCount(Mod), Verified))
    return false;
  return Verified >= Mod.TotalLen;
}

/// The indexed prefix of one module: its bytes, and which of them it states.
struct IndexedPrefix {
  std::array<uint8_t, SignatureMatcher::HashIndex::kIndexedBytes> Bytes{};
  uint32_t Stated = 0;
};
static_assert(SignatureMatcher::HashIndex::kIndexedBytes <=
                  std::numeric_limits<uint32_t>::digits,
              "the stated bytes of a prefix are one 32-bit mask");

IndexedPrefix prefixOf(const PatternModule &Mod) {
  IndexedPrefix Prefix;
  const size_t Count = std::min(effectiveLeadingCount(Mod),
                                SignatureMatcher::HashIndex::kIndexedBytes);
  for (size_t K = 0; K < Count; ++K) {
    if (Mod.LeadingBytes[K].IsWildcard)
      continue;
    Prefix.Bytes[K] = Mod.LeadingBytes[K].Value;
    Prefix.Stated |= uint32_t(1) << K;
  }
  return Prefix;
}

IndexedPrefix prefixOf(const StoredModule &Mod) {
  IndexedPrefix Prefix;
  const size_t Count = std::min(effectiveLeadingCount(Mod),
                                SignatureMatcher::HashIndex::kIndexedBytes);
  if (Count == 0)
    return Prefix;
  // An unstated byte is zero in both.
  std::memcpy(Prefix.Bytes.data(), Mod.Bytes, Count);
  uint64_t Stated = 0;
  for (size_t Byte = 0; Byte < llvm::divideCeil(Count, CHAR_BIT); ++Byte)
    Stated |= uint64_t(Mod.Stated[Byte]) << (Byte * CHAR_BIT);
  Prefix.Stated = static_cast<uint32_t>(Stated & ((uint64_t(1) << Count) - 1));
  return Prefix;
}

/// Builds the decision nodes over a permutation of the modules: a node
/// partitions its range of the permutation, stably, into the modules that
/// leave its byte unstated and those stating each value, and its children
/// own those ranges.
class IndexBuilder {
  using HashIndex = SignatureMatcher::HashIndex;
  /// The key of a module that leaves the byte unstated: past every byte.
  static constexpr uint16_t kUnstated = ByteValues;
  /// Per key, the modules counted; kept zero between uses.
  using Counts = std::array<size_t, kUnstated + 1>;

  struct Group {
    uint16_t Key;
    size_t Begin, End;
  };

public:
  template <typename ModuleRange>
  IndexBuilder(HashIndex &Index, const ModuleRange &Modules,
               std::vector<size_t> &CandidateOrder)
      : Index(Index), CandidateOrder(CandidateOrder), Prefixes(Modules.size()),
        Order(Modules.size()), Spare(Modules.size()) {
    constexpr size_t Block = SignatureLimits::IndexBuildBlock;
    neverd::parallelForEach(
        (Modules.size() + Block - 1) / Block, [&](auto Claim, size_t Total) {
          for (size_t B = Claim(); B < Total; B = Claim()) {
            const size_t End = std::min(Modules.size(), (B + 1) * Block);
            for (size_t I = B * Block; I < End; ++I) {
              Prefixes[I] = prefixOf(Modules[I]);
              Order[I] = I;
            }
          }
        });
    std::erase_if(Order, [&](size_t I) { return Modules[I].RequireCaller; });
    Spare.resize(Order.size());
    CandidateOrder.resize(Order.size());
  }

  /// Build the index.  The subtrees of the first decision are built on the
  /// worker threads, each into nodes of its own, and then joined in the order
  /// one thread builds them in, so the nodes are the same either way.
  void build() {
    Index.Nodes.clear();
    Index.Root = HashIndex::kNoNode;
    if (Order.empty())
      return;
    Counts Count{};
    size_t Offset = 0;
    std::vector<size_t> *From = &Order, *Other = &Spare;
    llvm::SmallVector<Group, 16> Groups;
    if (!split(0, Order.size(), Offset, From, Other, Count, Groups)) {
      Index.Root = leaf(Index.Nodes, 0, Order.size(), *From);
      return;
    }

    std::vector<std::vector<HashIndex::Node>> Subtrees(Groups.size());
    std::vector<size_t> Heaviest(Groups.size());
    for (size_t G = 0; G < Groups.size(); ++G)
      Heaviest[G] = G;
    std::sort(Heaviest.begin(), Heaviest.end(), [&](size_t A, size_t B) {
      return Groups[A].End - Groups[A].Begin > Groups[B].End - Groups[B].Begin;
    });
    std::atomic<size_t> Next{0};
    auto Work = [&] {
      Counts LocalCount{};
      for (size_t I = Next++; I < Heaviest.size(); I = Next++) {
        const Group &G = Groups[Heaviest[I]];
        buildRange(Subtrees[Heaviest[I]], LocalCount, G.Begin, G.End,
                   Offset + 1, From, Other);
      }
    };
    // A small set is not worth a thread.
    const unsigned Threads =
        Order.size() < SignatureLimits::ParallelIndexModules
            ? 1
            : static_cast<unsigned>(
                  std::min<size_t>(neverd::workerThreadCount(), Groups.size()));
    if (Threads <= 1)
      Work();
    else
      neverd::runWithLargeStackThreads(Threads, Work);

    Index.Root = 0;
    size_t NodeCount = 1;
    for (const auto &Subtree : Subtrees)
      NodeCount += Subtree.size();
    Index.Nodes.reserve(NodeCount);
    Index.Nodes.emplace_back();
    Index.Nodes[0].Offset = static_cast<uint8_t>(Offset);
    Index.Nodes[0].ExactChildren.reserve(Groups.size() -
                                         (Groups.front().Key == kUnstated));
    for (size_t G = 0; G < Groups.size(); ++G) {
      const size_t Base = Index.Nodes.size();
      for (HashIndex::Node &Node : Subtrees[G]) {
        if (Node.WildcardChild != HashIndex::kNoNode)
          Node.WildcardChild += Base;
        for (HashIndex::Edge &Edge : Node.ExactChildren)
          Edge.Child += Base;
        Index.Nodes.push_back(std::move(Node));
      }
      attach(Index.Nodes[0], Groups[G].Key, Base);
    }
  }

private:
  uint16_t keyAt(size_t Module, size_t Offset) const {
    const IndexedPrefix &Prefix = Prefixes[Module];
    return (Prefix.Stated >> Offset) & 1 ? Prefix.Bytes[Offset] : kUnstated;
  }

  size_t leaf(std::vector<HashIndex::Node> &Nodes, size_t Begin, size_t End,
              const std::vector<size_t> &From) const {
    const size_t NodeIndex = Nodes.size();
    Nodes.emplace_back();
    std::copy(From.begin() + Begin, From.begin() + End,
              CandidateOrder.begin() + Begin);
    Nodes[NodeIndex].Candidates =
        llvm::ArrayRef(CandidateOrder).slice(Begin, End - Begin);
    return NodeIndex;
  }

  static void attach(HashIndex::Node &Node, uint16_t Key, size_t Child) {
    if (Key == kUnstated)
      Node.WildcardChild = Child;
    else
      Node.ExactChildren.push_back({static_cast<uint8_t>(Key), Child});
  }

  /// The first column where this range can differ, after a uniform column.
  /// Read each prefix once rather than count and repartition the same range
  /// for every byte of a shared prologue. The decision nodes still check all
  /// stated bytes; this only avoids repeating construction work.
  size_t commonPrefixEnd(size_t Begin, size_t End, size_t Offset,
                         const std::vector<size_t> &From) const {
    size_t Limit = HashIndex::kIndexedBytes;
    if (Offset >= Limit)
      return Limit;
    const IndexedPrefix &First = Prefixes[From[Begin]];
    for (size_t I = Begin + 1; I < End && Limit > Offset; ++I) {
      const IndexedPrefix &Other = Prefixes[From[I]];
      const uint32_t Different = (First.Stated ^ Other.Stated) >> Offset;
      if (Different)
        Limit = std::min(Limit, Offset + llvm::countr_zero(Different));
      const auto Mismatch = std::mismatch(First.Bytes.begin() + Offset,
                                          First.Bytes.begin() + Limit,
                                          Other.Bytes.begin() + Offset);
      Limit = Mismatch.first - First.Bytes.begin();
    }
    return Limit;
  }

  /// Decide [\p Begin, \p End) of \p From, the order its modules are in:
  /// false for a leaf, or else the byte, from \p Offset on, that the node
  /// reads, and \p Groups, the ranges its children own, in order.  When the
  /// modules are sorted into place, \p From and \p Other trade places.
  bool split(size_t Begin, size_t End, size_t &Offset,
             std::vector<size_t> *&From, std::vector<size_t> *&Other,
             Counts &Count, llvm::SmallVectorImpl<Group> &Groups) const {
    // A byte every module of the range leaves unstated cannot dispatch a
    // query, so the next one is tried.
    llvm::SmallVector<uint16_t, 16> Keys;
    while (true) {
      if (End - Begin <= HashIndex::kLeafCandidates ||
          Offset >= HashIndex::kIndexedBytes)
        return false;
      for (size_t I = Begin; I < End; ++I) {
        const uint16_t Key = keyAt((*From)[I], Offset);
        if (Count[Key]++ == 0)
          Keys.push_back(Key);
      }
      if (Keys.size() != 1 || Keys.front() != kUnstated)
        break;
      Count[kUnstated] = 0;
      Keys.clear();
      ++Offset;
    }

    // The node's children in order: the modules leaving the byte unstated,
    // then those stating each value, from the smallest.
    std::sort(Keys.begin(), Keys.end());
    if (Keys.back() == kUnstated)
      std::rotate(Keys.begin(), Keys.end() - 1, Keys.end());
    size_t Next = Begin;
    for (uint16_t Key : Keys) {
      Groups.push_back({Key, Next, Next + Count[Key]});
      Next += Count[Key];
      Count[Key] = 0;
    }

    // One group keeps the order it has; several are sorted into place.
    if (Groups.size() > 1) {
      for (const Group &G : Groups)
        Count[G.Key] = G.Begin;
      for (size_t I = Begin; I < End; ++I)
        (*Other)[Count[keyAt((*From)[I], Offset)]++] = (*From)[I];
      for (const Group &G : Groups)
        Count[G.Key] = 0;
      std::swap(From, Other);
    }
    return true;
  }

  /// Build the node for [\p Begin, \p End) of \p From, the order the
  /// range's modules are in, into \p Nodes; \p Other is free to reorder them
  /// into.
  size_t buildRange(std::vector<HashIndex::Node> &Nodes, Counts &Count,
                    size_t Begin, size_t End, size_t Offset,
                    std::vector<size_t> *From,
                    std::vector<size_t> *Other) const {
    llvm::SmallVector<Group, 16> Groups;
    if (!split(Begin, End, Offset, From, Other, Count, Groups))
      return leaf(Nodes, Begin, End, *From);

    const size_t NodeIndex = Nodes.size();
    Nodes.emplace_back();
    Nodes[NodeIndex].Offset = static_cast<uint8_t>(Offset);
    Nodes[NodeIndex].ExactChildren.reserve(Groups.size() -
                                           (Groups.front().Key == kUnstated));
    if (Groups.size() == 1) {
      // split() skips columns that are entirely unstated, so this is an
      // exact edge. Emit the remaining uniform columns without recounting
      // modules. Wildcard columns need no decision node.
      const size_t CommonEnd = commonPrefixEnd(Begin, End, Offset + 1, *From);
      size_t Parent = NodeIndex;
      uint16_t Key = Groups.front().Key;
      for (size_t Column = Offset + 1; Column < CommonEnd; ++Column) {
        const uint16_t NextKey = keyAt((*From)[Begin], Column);
        if (NextKey == kUnstated)
          continue;
        const size_t Child = Nodes.size();
        Nodes.emplace_back();
        Nodes[Child].Offset = static_cast<uint8_t>(Column);
        attach(Nodes[Parent], Key, Child);
        Parent = Child;
        Key = NextKey;
      }
      const size_t Child =
          buildRange(Nodes, Count, Begin, End, CommonEnd, From, Other);
      attach(Nodes[Parent], Key, Child);
      return NodeIndex;
    }
    for (const Group &G : Groups) {
      const size_t Child =
          buildRange(Nodes, Count, G.Begin, G.End, Offset + 1, From, Other);
      attach(Nodes[NodeIndex], G.Key, Child);
    }
    return NodeIndex;
  }

  HashIndex &Index;
  std::vector<size_t> &CandidateOrder;
  std::vector<IndexedPrefix> Prefixes;
  std::vector<size_t> Order, Spare;
};

const SignatureMatcher::HashIndex::Edge *
findExactEdge(const SignatureMatcher::HashIndex::Node &Node, uint8_t Value) {
  auto It = std::lower_bound(Node.ExactChildren.begin(),
                             Node.ExactChildren.end(), Value,
                             [](const SignatureMatcher::HashIndex::Edge &Edge,
                                uint8_t Byte) { return Edge.Value < Byte; });
  return It != Node.ExactChildren.end() && It->Value == Value ? &*It : nullptr;
}

/// Report, by index, the modules of \p Modules that match \p Data.
template <typename ModuleRange, typename Checksum, typename Report>
void matchIndexNode(const SignatureMatcher::HashIndex &Index, size_t NodeIndex,
                    const ModuleRange &Modules, const uint8_t *Data,
                    size_t Available, Checksum &CRC, Report &&Found) {
  if (NodeIndex >= Index.Nodes.size())
    return;
  const SignatureMatcher::HashIndex::Node &Node = Index.Nodes[NodeIndex];
  if (Node.isLeaf()) {
    for (size_t ModuleIndex : Node.Candidates)
      if (ModuleIndex < Modules.size() &&
          matchModule(Modules[ModuleIndex], Data, Available, CRC))
        Found(ModuleIndex);
    return;
  }

  if (Node.WildcardChild != SignatureMatcher::HashIndex::kNoNode)
    matchIndexNode(Index, Node.WildcardChild, Modules, Data, Available, CRC,
                   Found);
  if (Node.Offset >= Available)
    return;
  if (const auto *Edge = findExactEdge(Node, Data[Node.Offset]))
    matchIndexNode(Index, Edge->Child, Modules, Data, Available, CRC, Found);
}

size_t countIndexNode(const SignatureMatcher::HashIndex &Index,
                      size_t NodeIndex, const uint8_t *Data, size_t Available) {
  if (NodeIndex >= Index.Nodes.size())
    return 0;
  const SignatureMatcher::HashIndex::Node &Node = Index.Nodes[NodeIndex];
  if (Node.isLeaf())
    return Node.Candidates.size();

  size_t Count = 0;
  if (Node.WildcardChild != SignatureMatcher::HashIndex::kNoNode)
    Count = countIndexNode(Index, Node.WildcardChild, Data, Available);
  if (Node.Offset >= Available)
    return Count;
  const auto *Edge = findExactEdge(Node, Data[Node.Offset]);
  if (!Edge)
    return Count;
  const size_t Exact = countIndexNode(Index, Edge->Child, Data, Available);
  return Exact > std::numeric_limits<size_t>::max() - Count
             ? std::numeric_limits<size_t>::max()
             : Count + Exact;
}

/// Report the modules that match at \p Entry, when it lies in the image.
template <bool Track = false, bool CallbackMayMutate = false,
          typename ModuleRange, typename Report>
void scanEntry(const uint8_t *ImageBase, size_t ImageSize, uint64_t BaseVA,
               uint64_t Entry, const ModuleRange &Modules,
               const SignatureMatcher::HashIndex &Index, Report &&Found,
               SignatureMatcher::ScanStatistics *Statistics = nullptr) {
  if (Entry < BaseVA)
    return;
  const uint64_t Offset = Entry - BaseVA;
  if (Offset >= ImageSize)
    return;
  if constexpr (Track)
    ++Statistics->Entries;
  const size_t Available = ImageSize - static_cast<size_t>(Offset);
  if constexpr (CallbackMayMutate) {
    DirectCRC CRC;
    matchIndexNode(Index, Index.Root, Modules, ImageBase + Offset, Available,
                   CRC, Found);
  } else {
    EntryCRC<Track> CRC(Statistics);
    matchIndexNode(Index, Index.Root, Modules, ImageBase + Offset, Available,
                   CRC, Found);
  }
}

} // namespace

uint16_t SignatureMatcher::computeCRC16(const uint8_t *Data, size_t Len) {
  uint16_t CRC = CRC16Initial;
  for (size_t I = 0; I < Len; ++I) {
    uint8_t Idx = static_cast<uint8_t>(CRC ^ Data[I]);
    CRC = (CRC >> CHAR_BIT) ^ CRC16Table[Idx];
  }
  // Pattern files print the complemented remainder in byte-stream order.
  return llvm::byteswap(static_cast<uint16_t>(~CRC));
}

bool SignatureMatcher::matchPattern(const PatternModule &Mod,
                                    const uint8_t *Data, size_t Available) {
  DirectCRC CRC;
  return matchModule(Mod, Data, Available, CRC);
}

bool SignatureMatcher::matchPattern(const StoredModule &Mod,
                                    const uint8_t *Data, size_t Available) {
  DirectCRC CRC;
  return matchModule(Mod, Data, Available, CRC);
}

size_t SignatureMatcher::fixedByteCount(const PatternModule &Mod) {
  return fixedBytes(Mod);
}

size_t SignatureMatcher::fixedByteCount(const StoredModule &Mod) {
  return fixedBytes(Mod);
}

bool SignatureMatcher::isFullyVerified(const PatternModule &Mod) {
  return fullyVerified(Mod);
}

bool SignatureMatcher::isFullyVerified(const StoredModule &Mod) {
  return fullyVerified(Mod);
}

void SignatureMatcher::scanRegion(const uint8_t *Data, size_t Size,
                                  const std::vector<PatternModule> &Modules,
                                  MatchCallback Callback) {
  if ((Size != 0 && !Data) || !Callback)
    return;
  HashIndex Idx;
  Idx.build(Modules);

  for (size_t Offset = 0; Offset < Size; ++Offset) {
    const size_t Available = Size - Offset;
    // User callbacks can modify the input through another alias. Only the
    // packed batch scan has no external callback during verification.
    DirectCRC CRC;
    matchIndexNode(Idx, Idx.Root, Modules, Data + Offset, Available, CRC,
                   [&](size_t Module) { Callback(Offset, Modules[Module]); });
  }
}

void SignatureMatcher::HashIndex::build(
    const std::vector<PatternModule> &Modules) {
  auto Storage = std::make_shared<std::vector<size_t>>();
  IndexBuilder(*this, Modules, *Storage).build();
  CandidateStorage = std::move(Storage);
  CallerStorage.reset();
}

void SignatureMatcher::HashIndex::build(llvm::ArrayRef<StoredModule> Modules) {
  auto Storage = std::make_shared<std::vector<size_t>>();
  IndexBuilder(*this, Modules, *Storage).build();
  CandidateStorage = std::move(Storage);
  CallerStorage.reset();
  if (llvm::none_of(Modules,
                    [](const StoredModule &Mod) { return Mod.RequireCaller; }))
    return;
  auto Callers = std::make_shared<std::vector<CallerEntry>>();
  for (size_t I = 0; I < Modules.size(); ++I)
    if ((Modules[I].RequireCaller || Modules[I].RequireReferences) &&
        SignatureMatcher::isFullyVerified(Modules[I]))
      for (size_t N = 0; N < Modules[I].PublicNameCount; ++N)
        if (Modules[I].publicNames()[N].Offset == 0)
          Callers->push_back({I, N,
                              std::hash<std::string_view>{}(
                                  Modules[I].publicNames()[N].Name)});
  if (Callers->empty())
    return;
  const auto NameOf = [&](const CallerEntry &Entry) {
    return Modules[Entry.Module].publicNames()[Entry.PublicName].Name;
  };
  llvm::stable_sort(*Callers, [&](const CallerEntry &A, const CallerEntry &B) {
    if (A.NameHash != B.NameHash)
      return A.NameHash < B.NameHash;
    return NameOf(A) < NameOf(B);
  });
  CallerStorage = std::move(Callers);
}

llvm::ArrayRef<SignatureMatcher::HashIndex::CallerEntry>
SignatureMatcher::HashIndex::callerModules(llvm::ArrayRef<StoredModule> Modules,
                                           std::string_view Name) const {
  if (!hasCallerModules())
    return {};
  const size_t Hash = std::hash<std::string_view>{}(Name);
  auto Begin = llvm::lower_bound(*CallerStorage, Hash,
                                 [](const CallerEntry &Entry, size_t Wanted) {
                                   return Entry.NameHash < Wanted;
                                 });
  // Hashes locate a short numeric range; exact names still decide membership.
  // Numeric indices also keep an index copy independent of a source arena's
  // name pointers, just like its byte-candidate permutation.
  while (Begin != CallerStorage->end() && Begin->NameHash == Hash &&
         Modules[Begin->Module].publicNames()[Begin->PublicName].Name < Name)
    ++Begin;
  auto End = Begin;
  while (End != CallerStorage->end() && End->NameHash == Hash &&
         Modules[End->Module].publicNames()[End->PublicName].Name == Name)
    ++End;
  return llvm::ArrayRef(*CallerStorage)
      .slice(Begin - CallerStorage->begin(), End - Begin);
}

uint16_t SignatureMatcher::HashIndex::keyOf(const PatternModule &Mod) const {
  // The first two bytes, the first high.
  if (effectiveLeadingCount(Mod) < sizeof(uint16_t))
    return 0;
  return (static_cast<uint16_t>(Mod.LeadingBytes[0].Value) << CHAR_BIT) |
         Mod.LeadingBytes[1].Value;
}

bool SignatureMatcher::HashIndex::isWildcardKey(
    const PatternModule &Mod) const {
  const size_t LeadingCount = effectiveLeadingCount(Mod);
  return LeadingCount < sizeof(uint16_t) || Mod.LeadingBytes[0].IsWildcard ||
         Mod.LeadingBytes[1].IsWildcard;
}

size_t SignatureMatcher::HashIndex::candidateCount(const uint8_t *Data,
                                                   size_t Available) const {
  if ((Available != 0 && !Data) || Root == kNoNode)
    return 0;
  return countIndexNode(*this, Root, Data, Available);
}

void SignatureMatcher::scanAtAddresses(
    const uint8_t *ImageBase, size_t ImageSize, uint64_t BaseVA,
    const std::vector<uint64_t> &FuncEntries,
    const std::vector<PatternModule> &Modules, MatchCallback Callback) {
  HashIndex Idx;
  Idx.build(Modules);
  scanAtAddresses(ImageBase, ImageSize, BaseVA, FuncEntries, Modules, Idx,
                  std::move(Callback));
}

void SignatureMatcher::scanAtAddresses(
    const uint8_t *ImageBase, size_t ImageSize, uint64_t BaseVA,
    const std::vector<uint64_t> &FuncEntries,
    const std::vector<PatternModule> &Modules, const HashIndex &Index,
    MatchCallback Callback) {
  if ((ImageSize != 0 && !ImageBase) || !Callback)
    return;

  for (uint64_t Entry : FuncEntries)
    scanEntry<false, true>(
        ImageBase, ImageSize, BaseVA, Entry, Modules, Index,
        [&](size_t Module) { Callback(Entry, Modules[Module]); });
}

namespace {

template <bool Track>
std::vector<SignatureMatcher::Hit>
findAtAddresses(const uint8_t *ImageBase, size_t ImageSize, uint64_t BaseVA,
                llvm::ArrayRef<uint64_t> FuncEntries,
                llvm::ArrayRef<StoredModule> Modules,
                const SignatureMatcher::HashIndex &Index,
                SignatureMatcher::ScanStatistics *Statistics) {
  using Hit = SignatureMatcher::Hit;
  if constexpr (Track)
    *Statistics = {};
  if (ImageSize != 0 && !ImageBase)
    return {};

  // Each block of entries keeps its hits apart, so that joining the blocks
  // in order reports them in the order a sequential scan does.
  constexpr size_t BlockEntries = SignatureLimits::MatchBlockEntries;
  const size_t Blocks = (FuncEntries.size() + BlockEntries - 1) / BlockEntries;
  std::vector<std::vector<Hit>> BlockHits(Blocks);
  std::vector<SignatureMatcher::ScanStatistics> BlockStatistics;
  if constexpr (Track)
    BlockStatistics.resize(Blocks);
  neverd::parallelForEach(Blocks, [&](auto Claim, size_t Total) {
    for (size_t B = Claim(); B < Total; B = Claim()) {
      std::vector<Hit> &Hits = BlockHits[B];
      const size_t End = std::min(FuncEntries.size(), (B + 1) * BlockEntries);
      for (size_t I = B * BlockEntries; I < End; ++I)
        scanEntry<Track>(
            ImageBase, ImageSize, BaseVA, FuncEntries[I], Modules, Index,
            [&](size_t Module) { Hits.push_back({FuncEntries[I], Module}); },
            Track ? &BlockStatistics[B] : nullptr);
    }
  });

  std::vector<Hit> Hits;
  for (const std::vector<Hit> &Block : BlockHits)
    Hits.insert(Hits.end(), Block.begin(), Block.end());
  if constexpr (Track) {
    for (const auto &Block : BlockStatistics) {
      Statistics->Entries += Block.Entries;
      Statistics->Candidates += Block.Candidates;
      Statistics->CRCRequests += Block.CRCRequests;
      Statistics->CRCComputations += Block.CRCComputations;
      Statistics->CRCBytes += Block.CRCBytes;
    }
  }
  return Hits;
}

} // namespace

std::vector<SignatureMatcher::Hit> SignatureMatcher::findAtAddresses(
    const uint8_t *ImageBase, size_t ImageSize, uint64_t BaseVA,
    llvm::ArrayRef<uint64_t> FuncEntries, llvm::ArrayRef<StoredModule> Modules,
    const HashIndex &Index) {
  return ::findAtAddresses<false>(ImageBase, ImageSize, BaseVA, FuncEntries,
                                  Modules, Index, nullptr);
}

std::vector<SignatureMatcher::Hit> SignatureMatcher::findAtAddresses(
    const uint8_t *ImageBase, size_t ImageSize, uint64_t BaseVA,
    llvm::ArrayRef<uint64_t> FuncEntries, llvm::ArrayRef<StoredModule> Modules,
    const HashIndex &Index, ScanStatistics &Statistics) {
  return ::findAtAddresses<true>(ImageBase, ImageSize, BaseVA, FuncEntries,
                                 Modules, Index, &Statistics);
}
