//===- MIPSCachePlatform.h - Physical MIPS cache transactions -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A board supplies translation, cache geometry, tags, coherence and bus
/// behavior. The ISA owns privilege checks, suppression and operation
/// selection.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_LOW_MIPS_MIPSCACHEPLATFORM_H
#define NEVERD_IR_LOW_MIPS_MIPSCACHEPLATFORM_H

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {
enum class MIPSCacheMapping { Cached, Uncached, AddressException };
struct MIPSCacheTranslation {
  MIPSCacheMapping Mapping;
  uint64_t PhysicalAddress;
};
enum class MIPSCacheAction {
  IndexInvalidate,
  IndexWritebackInvalidate,
  IndexLoadTag,
  IndexStoreTag,
  HitInvalidate,
  Fill,
  HitWritebackInvalidate,
  HitWriteback,
  FetchAndLock,
  ImplementationSpecific,
  Prefetch
};
struct MIPSCacheRequest {
  uint64_t InstructionAddress;
  uint64_t VirtualAddress;
  MIPSCacheTranslation Translation;
  uint8_t Encoding;
  MIPSCacheAction Action;
  uint8_t ISARevision = 2;
};
struct MIPSCacheLine {
  uint64_t PhysicalBase = 0;
  std::vector<uint8_t> Data;
  bool Valid = false, Dirty = false, Locked = false;
};
struct MIPSCacheSelection {
  MIPSCacheLine &Line;
  bool Hit;
  bool ContainsExecutingInstruction;
};
class MIPSCachePlatform {
public:
  virtual ~MIPSCachePlatform() = default;
  /// nullopt means insufficient translation evidence, even for a prefetch.
  virtual std::optional<MIPSCacheTranslation>
  translateDataRead(uint32_t VA) = 0;
  virtual std::optional<MIPSCacheTranslation> translateDataRead64(uint64_t VA) {
    if (VA > UINT32_MAX)
      return std::nullopt;
    return translateDataRead(uint32_t(VA));
  }
  /// Apply one cache transaction atomically. Return false for a fault, an
  /// unsupported cache, or an undefined operation. The board determines the
  /// addressed set/way and any writeback, fill, tag and coherence effects.
  virtual bool apply(const MIPSCacheRequest &Request) = 0;
  /// Synchronous single-line Fill/Fetch-and-Lock in ordinary backing memory.
  /// The board selects a way and supplies its aligned physical base. Read
  /// stages clean bytes without effects. Write commits a dirty eviction.
  /// Coherent or device-backed boards implement their own transaction.
  template <class ReadLine, class Writeback>
  static bool fill(const MIPSCacheRequest &R, MIPSCacheSelection S,
                   uint64_t PhysicalBase, ReadLine &&Read, Writeback &&Write) {
    const bool Lock = R.Action == MIPSCacheAction::FetchAndLock;
    if (!Lock && R.Action != MIPSCacheAction::Fill)
      return false;
    if (Lock && S.Line.Valid && S.Hit) {
      S.Line.Locked = true;
      return true;
    }
    if (S.Line.Valid && S.Line.Locked && !S.Hit)
      return false;
    std::vector<uint8_t> Data(S.Line.Data.size());
    if (!Read(PhysicalBase, Data))
      return false;
    if (S.Line.Valid && S.Line.Dirty &&
        !Write(S.Line.PhysicalBase, S.Line.Data))
      return false;
    const bool KeepLock = Lock || (S.Line.Valid && S.Hit && S.Line.Locked);
    S.Line = {PhysicalBase, std::move(Data), true, false, KeepLock};
    return true;
  }
  /// Standard line maintenance for a board which has selected its set/way.
  /// Writeback receives the evicted tag and the actual dirty bytes. Platforms
  /// with coherent broadcast or asynchronous retirement may implement apply
  /// directly. Tag/data formats and replacement policy belong to the board.
  template <class Writeback>
  static bool maintain(const MIPSCacheRequest &R, MIPSCacheSelection S,
                       Writeback &&Write) {
    const bool Index = R.Action == MIPSCacheAction::IndexInvalidate ||
                       R.Action == MIPSCacheAction::IndexWritebackInvalidate;
    const bool WriteOnly = R.Action == MIPSCacheAction::HitWriteback;
    const bool WB = WriteOnly ||
                    R.Action == MIPSCacheAction::HitWritebackInvalidate ||
                    R.Action == MIPSCacheAction::IndexWritebackInvalidate;
    if (!Index && !WB && R.Action != MIPSCacheAction::HitInvalidate)
      return false;
    if (!S.Line.Valid || (!Index && !S.Hit))
      return true;
    if (!WriteOnly && S.ContainsExecutingInstruction)
      return false;
    if (WB && S.Line.Dirty && !Write(S.Line.PhysicalBase, S.Line.Data))
      return false;
    S.Line.Dirty = false;
    if (!WriteOnly)
      S.Line.Locked = S.Line.Valid = false;
    return true;
  }
};
} // namespace neverd
#endif // NEVERD_IR_LOW_MIPS_MIPSCACHEPLATFORM_H
