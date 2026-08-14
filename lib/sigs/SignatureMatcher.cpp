//===- SignatureMatcher.cpp - FLIRT pattern matching engine ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/SignatureMatcher.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

using namespace neverd::sigs;

namespace {

constexpr std::array<uint16_t, 256> makeCRC16Table() {
  std::array<uint16_t, 256> Table{};
  for (size_t I = 0; I < Table.size(); ++I) {
    uint16_t Value = static_cast<uint16_t>(I);
    for (unsigned Bit = 0; Bit < 8; ++Bit)
      Value =
          static_cast<uint16_t>((Value >> 1) ^ ((Value & 1) ? 0x8408u : 0u));
    Table[I] = Value;
  }
  return Table;
}

constexpr auto CRC16Table = makeCRC16Table();

bool checkedAdd(size_t Left, size_t Right, size_t &Result) {
  if (Right > std::numeric_limits<size_t>::max() - Left)
    return false;
  Result = Left + Right;
  return true;
}

size_t effectiveLeadingCount(const PatternModule &Mod) {
  return Mod.TotalLen == 0
             ? Mod.LeadingBytes.size()
             : std::min(Mod.LeadingBytes.size(), size_t{Mod.TotalLen});
}

bool fixedLeadingByte(const PatternModule &Mod, size_t Offset, uint8_t &Value) {
  if (Offset >= effectiveLeadingCount(Mod) ||
      Mod.LeadingBytes[Offset].IsWildcard)
    return false;
  Value = Mod.LeadingBytes[Offset].Value;
  return true;
}

void matchBucket(const std::vector<size_t> &Bucket,
                 const std::vector<PatternModule> &Modules, const uint8_t *Data,
                 size_t Available, uint64_t Address,
                 const SignatureMatcher::MatchCallback &Callback) {
  for (size_t ModuleIndex : Bucket) {
    if (ModuleIndex >= Modules.size())
      continue;
    const PatternModule &Module = Modules[ModuleIndex];
    if (SignatureMatcher::matchPattern(Module, Data, Available))
      Callback(Address, Module);
  }
}

size_t buildIndexNode(SignatureMatcher::HashIndex &Index,
                      const std::vector<PatternModule> &Modules,
                      std::vector<size_t> Candidates, size_t Offset) {
  using HashIndex = SignatureMatcher::HashIndex;
  if (Candidates.empty())
    return HashIndex::kNoNode;
  if (Candidates.size() <= HashIndex::kLeafCandidates ||
      Offset >= HashIndex::kIndexedBytes) {
    const size_t NodeIndex = Index.Nodes.size();
    Index.Nodes.emplace_back();
    Index.Nodes[NodeIndex].Candidates = std::move(Candidates);
    return NodeIndex;
  }

  std::array<std::vector<size_t>, 256> Exact;
  std::vector<size_t> Wildcard;
  std::vector<uint8_t> UsedValues;
  Wildcard.reserve(Candidates.size());
  for (size_t ModuleIndex : Candidates) {
    uint8_t Value = 0;
    if (ModuleIndex >= Modules.size() ||
        !fixedLeadingByte(Modules[ModuleIndex], Offset, Value)) {
      Wildcard.push_back(ModuleIndex);
      continue;
    }
    if (Exact[Value].empty())
      UsedValues.push_back(Value);
    Exact[Value].push_back(ModuleIndex);
  }
  std::vector<size_t>().swap(Candidates);

  // A byte every remaining module leaves unspecified cannot dispatch a query.
  if (UsedValues.empty())
    return buildIndexNode(Index, Modules, std::move(Wildcard), Offset + 1);

  std::sort(UsedValues.begin(), UsedValues.end());
  const size_t NodeIndex = Index.Nodes.size();
  Index.Nodes.emplace_back();
  Index.Nodes[NodeIndex].Offset = static_cast<uint8_t>(Offset);

  if (!Wildcard.empty()) {
    const size_t Child =
        buildIndexNode(Index, Modules, std::move(Wildcard), Offset + 1);
    Index.Nodes[NodeIndex].WildcardChild = Child;
  }
  Index.Nodes[NodeIndex].ExactChildren.reserve(UsedValues.size());
  for (uint8_t Value : UsedValues) {
    const size_t Child =
        buildIndexNode(Index, Modules, std::move(Exact[Value]), Offset + 1);
    Index.Nodes[NodeIndex].ExactChildren.push_back({Value, Child});
  }
  return NodeIndex;
}

const SignatureMatcher::HashIndex::Edge *
findExactEdge(const SignatureMatcher::HashIndex::Node &Node, uint8_t Value) {
  auto It = std::lower_bound(Node.ExactChildren.begin(),
                             Node.ExactChildren.end(), Value,
                             [](const SignatureMatcher::HashIndex::Edge &Edge,
                                uint8_t Byte) { return Edge.Value < Byte; });
  return It != Node.ExactChildren.end() && It->Value == Value ? &*It : nullptr;
}

void matchIndexNode(const SignatureMatcher::HashIndex &Index, size_t NodeIndex,
                    const std::vector<PatternModule> &Modules,
                    const uint8_t *Data, size_t Available, uint64_t Address,
                    const SignatureMatcher::MatchCallback &Callback) {
  if (NodeIndex >= Index.Nodes.size())
    return;
  const SignatureMatcher::HashIndex::Node &Node = Index.Nodes[NodeIndex];
  if (Node.isLeaf()) {
    matchBucket(Node.Candidates, Modules, Data, Available, Address, Callback);
    return;
  }

  if (Node.WildcardChild != SignatureMatcher::HashIndex::kNoNode)
    matchIndexNode(Index, Node.WildcardChild, Modules, Data, Available, Address,
                   Callback);
  if (Node.Offset >= Available)
    return;
  if (const auto *Edge = findExactEdge(Node, Data[Node.Offset]))
    matchIndexNode(Index, Edge->Child, Modules, Data, Available, Address,
                   Callback);
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

} // namespace

uint16_t SignatureMatcher::computeCRC16(const uint8_t *Data, size_t Len) {
  uint16_t CRC = 0xFFFF;
  for (size_t I = 0; I < Len; ++I) {
    uint8_t Idx = static_cast<uint8_t>(CRC ^ Data[I]);
    CRC = (CRC >> 8) ^ CRC16Table[Idx];
  }
  // Pattern files print the complemented remainder in byte-stream order.
  CRC = static_cast<uint16_t>(~CRC);
  return static_cast<uint16_t>((CRC << 8) | (CRC >> 8));
}

bool SignatureMatcher::matchLeading(const std::vector<PatternByte> &Pattern,
                                    const uint8_t *Data, size_t Count) {
  if (Count > Pattern.size())
    return false;
  for (size_t I = 0; I < Count; ++I) {
    if (Pattern[I].IsWildcard)
      continue;
    if (Data[I] != Pattern[I].Value)
      return false;
  }
  return true;
}

bool SignatureMatcher::verifyCRC(const PatternModule &Mod, const uint8_t *Data,
                                 size_t MatchLimit) {
  const size_t CRCStart = Mod.LeadingBytes.size();
  if (Mod.CRCLen == 0)
    return true;
  if (CRCStart > MatchLimit)
    return false;
  size_t CRCEnd = 0;
  if (!checkedAdd(CRCStart, Mod.CRCLen, CRCEnd) || CRCEnd > MatchLimit)
    return false;
  return computeCRC16(Data + CRCStart, Mod.CRCLen) == Mod.CRC16;
}

bool SignatureMatcher::matchTail(const PatternModule &Mod, const uint8_t *Data,
                                 size_t MatchLimit) {
  size_t TailStart = 0;
  if (!checkedAdd(Mod.LeadingBytes.size(), Mod.CRCLen, TailStart))
    return false;
  if (TailStart >= MatchLimit)
    return true;
  const size_t TailCount =
      std::min(Mod.TailBytes.size(), MatchLimit - TailStart);
  for (size_t I = 0; I < TailCount; ++I) {
    if (Mod.TailBytes[I].IsWildcard)
      continue;
    if (Data[TailStart + I] != Mod.TailBytes[I].Value)
      return false;
  }
  return true;
}

bool SignatureMatcher::matchPattern(const PatternModule &Mod,
                                    const uint8_t *Data, size_t Available) {
  size_t MatchLimit = Mod.TotalLen;
  if (MatchLimit == 0) {
    if (!checkedAdd(Mod.LeadingBytes.size(), Mod.CRCLen, MatchLimit) ||
        !checkedAdd(MatchLimit, Mod.TailBytes.size(), MatchLimit))
      return false;
  }
  if (MatchLimit > Available || (MatchLimit != 0 && !Data))
    return false;
  const size_t LeadingCount = std::min(Mod.LeadingBytes.size(), MatchLimit);
  if (!matchLeading(Mod.LeadingBytes, Data, LeadingCount))
    return false;
  if (!verifyCRC(Mod, Data, MatchLimit))
    return false;
  return matchTail(Mod, Data, MatchLimit);
}

size_t SignatureMatcher::fixedByteCount(const PatternModule &Mod) {
  size_t Fixed = 0;
  const size_t MatchLimit =
      Mod.TotalLen == 0 ? std::numeric_limits<size_t>::max() : Mod.TotalLen;
  const size_t LeadingCount = std::min(Mod.LeadingBytes.size(), MatchLimit);
  for (size_t I = 0; I < LeadingCount; ++I)
    Fixed += Mod.LeadingBytes[I].IsWildcard ? 0 : 1;

  size_t TailStart = 0;
  if (!checkedAdd(Mod.LeadingBytes.size(), Mod.CRCLen, TailStart) ||
      TailStart > MatchLimit)
    return Fixed;
  const size_t TailCount =
      std::min(Mod.TailBytes.size(), MatchLimit - TailStart);
  for (size_t I = 0; I < TailCount; ++I)
    Fixed += Mod.TailBytes[I].IsWildcard ? 0 : 1;
  return Fixed;
}

bool SignatureMatcher::isFullyVerified(const PatternModule &Mod) {
  if (Mod.TotalLen == 0)
    return false;
  if (Mod.LeadingBytes.size() >= Mod.TotalLen)
    return Mod.CRCLen == 0;
  size_t Verified = Mod.LeadingBytes.size();
  if (!checkedAdd(Verified, Mod.CRCLen, Verified) || Verified > Mod.TotalLen)
    return false;
  if (!checkedAdd(Verified, Mod.TailBytes.size(), Verified))
    return false;
  return Verified >= Mod.TotalLen;
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
    matchIndexNode(Idx, Idx.Root, Modules, Data + Offset, Available, Offset,
                   Callback);
  }
}

void SignatureMatcher::HashIndex::build(
    const std::vector<PatternModule> &Modules) {
  Nodes.clear();
  Root = kNoNode;
  if (Modules.empty())
    return;

  std::vector<size_t> Candidates;
  Candidates.reserve(Modules.size());
  for (size_t I = 0; I < Modules.size(); ++I)
    Candidates.push_back(I);
  Root = buildIndexNode(*this, Modules, std::move(Candidates), 0);
}

uint16_t SignatureMatcher::HashIndex::keyOf(const PatternModule &Mod) const {
  if (effectiveLeadingCount(Mod) < 2)
    return 0;
  return (static_cast<uint16_t>(Mod.LeadingBytes[0].Value) << 8) |
         Mod.LeadingBytes[1].Value;
}

bool SignatureMatcher::HashIndex::isWildcardKey(
    const PatternModule &Mod) const {
  const size_t LeadingCount = effectiveLeadingCount(Mod);
  return LeadingCount < 2 || Mod.LeadingBytes[0].IsWildcard ||
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

  for (uint64_t Entry : FuncEntries) {
    if (Entry < BaseVA)
      continue;
    uint64_t Offset = Entry - BaseVA;
    if (Offset >= ImageSize)
      continue;
    const size_t Available = ImageSize - static_cast<size_t>(Offset);
    matchIndexNode(Index, Index.Root, Modules, ImageBase + Offset, Available,
                   Entry, Callback);
  }
}
