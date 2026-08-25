//===- COFFFH4Encoding.h - Canonical C++ EH4 wire values -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_COFF_COFFFH4ENCODING_H
#define NEVERD_BACKEND_CODEGEN_COFF_COFFFH4ENCODING_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::coff_fh4 {

struct CompressedUInt {
  uint32_t Value = 0;
  uint8_t Size = 0;

  friend bool operator==(const CompressedUInt &,
                         const CompressedUInt &) = default;
};

/// Decode one canonical unsigned integer from the start of \p Bytes. The
/// returned size is the exact number of consumed bytes. Truncated, reserved,
/// and overlong spellings fail closed.
llvm::Expected<CompressedUInt>
decodeCompressedUInt(llvm::ArrayRef<uint8_t> Bytes);

/// Return the unique canonical 1--5 byte spelling of \p Value.
llvm::SmallVector<uint8_t, 5> encodeCompressedUInt(uint32_t Value);

struct ByteRange {
  uint32_t BeginRVA = 0;
  uint32_t EndRVA = 0;

  uint32_t size() const { return EndRVA - BeginRVA; }
  bool empty() const { return BeginRVA == EndRVA; }

  friend bool operator==(const ByteRange &, const ByteRange &) = default;
};

struct ParseLimits {
  uint32_t MaxRecords = 1u << 16;
  uint32_t MaxBytes = 1u << 20;
};

using ReadBytes = llvm::function_ref<llvm::Expected<llvm::ArrayRef<uint8_t>>(
    uint32_t RVA, uint32_t Size)>;

enum class UnwindActionKind : uint8_t {
  None,
  DestructorWithObject,
  DestructorWithObjectPointer,
  Direct,
};

struct UnwindEntry {
  ByteRange Range;
  uint32_t Encoded = 0;
  int32_t ToState = -1;
  UnwindActionKind Kind = UnwindActionKind::None;
  uint32_t ActionRVA = 0;
  uint32_t ObjectOffset = 0;
};

struct UnwindMap {
  ByteRange Range;
  std::vector<UnwindEntry> Entries;
};

struct HandlerEntry {
  ByteRange Range;
  uint8_t Header = 0;
  uint32_t Adjectives = 0;
  uint32_t TypeDescriptorRVA = 0;
  uint32_t CatchObjectOffset = 0;
  uint32_t HandlerRVA = 0;
  bool ContinuationsAreImageRelative = false;
  std::vector<uint32_t> Continuations;
};

struct HandlerMap {
  ByteRange Range;
  std::vector<HandlerEntry> Entries;
};

struct TryEntry {
  ByteRange Range;
  uint32_t TryLow = 0;
  uint32_t TryHigh = 0;
  uint32_t CatchHigh = 0;
  uint32_t HandlerMapRVA = 0;
  HandlerMap Handlers;
};

struct TryMap {
  ByteRange Range;
  std::vector<TryEntry> Entries;
};

struct IPEntry {
  ByteRange Range;
  uint32_t Delta = 0;
  uint32_t EncodedState = 0;
  int32_t State = -1;
  uint32_t FunctionOffset = 0;
};

struct IPMap {
  ByteRange Range;
  std::vector<IPEntry> Entries;
};

struct SeparatedIPEntry {
  ByteRange Range;
  uint32_t FunctionStartRVA = 0;
  uint32_t IPMapRVA = 0;
  std::optional<IPMap> States;
};

struct SeparatedIPMap {
  ByteRange Range;
  std::vector<SeparatedIPEntry> Entries;
};

struct FuncInfoLayout {
  ByteRange HeaderRange;
  uint8_t Header = 0;
  uint32_t BBTFlags = 0;
  uint32_t UnwindMapRVA = 0;
  uint32_t TryMapRVA = 0;
  uint32_t IPMapRVA = 0;
  uint32_t FrameOffset = 0;
  std::optional<UnwindMap> Unwind;
  std::optional<TryMap> Try;
  std::optional<IPMap> States;
  std::optional<SeparatedIPMap> SeparatedStates;
};

/// Parse one complete FH4 line-format graph through a bounded RVA reader.
/// Every retained container and row carries its exact half-open source range.
/// Canonical integer spelling, count/byte budgets, arithmetic, predecessor
/// boundaries, and physical table overlap are validated before success.
llvm::Expected<FuncInfoLayout> parseFuncInfoLayout(uint32_t FuncInfoRVA,
                                                   ReadBytes Read,
                                                   ParseLimits Limits = {});

} // namespace neverd::coff_fh4

#endif // NEVERD_BACKEND_CODEGEN_COFF_COFFFH4ENCODING_H
