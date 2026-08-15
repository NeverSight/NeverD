//===- TranslationBlock.h - Trusted translated block descriptor -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the trusted, versioned result of decoding and lifting one guest
/// basic block.  Generated code never receives a pointer to this descriptor;
/// the dispatcher owns it and uses it to interpret the canonical block return.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_TRANSLATIONBLOCK_H
#define NEVERD_TRANSLATE_TRANSLATIONBLOCK_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/translate/GuestMemoryRuntime.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <type_traits>
#include <vector>

namespace neverd::translate {

/// Little-endian bytes spell "NVBD" when this value is serialized.
inline constexpr uint32_t kTranslationBlockDescriptorMagicV1 = 0x4442564e;
inline constexpr uint16_t kTranslationBlockDescriptorVersionV1 = 1;
inline constexpr uint16_t kTranslationBlockDescriptorHeaderSizeV1 = 64;

/// Exact control-transfer class of the final guest instruction in a block.
/// Append values without renumbering existing entries.
enum class TranslationBlockTerminatorKindV1 : uint32_t {
  Invalid = 0,
  DirectBranch = 1,
  ConditionalBranch = 2,
  IndirectBranch = 3,
  DirectCall = 4,
  IndirectCall = 5,
  Return = 6,
  Opaque = 7,
};

enum class TranslationBlockDescriptorFlagV1 : uint32_t {
  None = 0,
  HasStaticTarget = 1u << 0,
  HasReturnImmediate = 1u << 1,
};

constexpr TranslationBlockDescriptorFlagV1
operator|(TranslationBlockDescriptorFlagV1 Left,
          TranslationBlockDescriptorFlagV1 Right) {
  return static_cast<TranslationBlockDescriptorFlagV1>(
      static_cast<uint32_t>(Left) | static_cast<uint32_t>(Right));
}

constexpr TranslationBlockDescriptorFlagV1 &
operator|=(TranslationBlockDescriptorFlagV1 &Left,
           TranslationBlockDescriptorFlagV1 Right) {
  Left = Left | Right;
  return Left;
}

constexpr bool
hasTranslationBlockDescriptorFlag(TranslationBlockDescriptorFlagV1 Set,
                                  TranslationBlockDescriptorFlagV1 Value) {
  return (static_cast<uint32_t>(Set) & static_cast<uint32_t>(Value)) != 0;
}

/// Fixed scalar identity and control summary for one descriptor.  Size covers
/// this fixed header only; the owned byte, LowIR, boundary, and generation
/// arrays are validated independently by validateTranslationBlockDescriptorV1.
struct alignas(8) TranslationBlockDescriptorHeaderV1 {
  uint32_t Magic = kTranslationBlockDescriptorMagicV1;
  uint16_t Version = kTranslationBlockDescriptorVersionV1;
  uint16_t Size = kTranslationBlockDescriptorHeaderSizeV1;
  TranslationBlockTerminatorKindV1 Terminator =
      TranslationBlockTerminatorKindV1::Invalid;
  TranslationBlockDescriptorFlagV1 Flags =
      TranslationBlockDescriptorFlagV1::None;
  uint64_t EntryPC = 0;
  uint64_t FallthroughPC = 0;
  uint64_t StaticTargetPC = 0;
  uint64_t GuestInstructionCount = 0;
  uint64_t GuestByteCount = 0;
  /// Exact encoded `ret imm16` cleanup, excluding the popped return address.
  uint64_t ReturnImmediate = 0;
};

static_assert(sizeof(TranslationBlockDescriptorHeaderV1) ==
              kTranslationBlockDescriptorHeaderSizeV1);
static_assert(alignof(TranslationBlockDescriptorHeaderV1) == 8);
static_assert(std::is_standard_layout_v<TranslationBlockDescriptorHeaderV1>);
static_assert(std::is_trivially_copyable_v<TranslationBlockDescriptorHeaderV1>);

/// Owned compiler input for one exact x86-64 guest block.  Bytes and bindings
/// come from one final transactional fetch over the complete half-open range,
/// so bindings retain the GuestMemoryRuntime's underlying region boundaries.
struct TranslationBlockDescriptorV1 {
  TranslationBlockDescriptorHeaderV1 Header;
  std::vector<uint8_t> Bytes;
  std::vector<LowOp> Ops;
  std::vector<LowInstructionBoundary> InstructionBoundaries;
  std::vector<GuestExecutableRangeBinding> GenerationBindings;
};

/// Reject an unknown schema, inconsistent counts/ranges/control metadata, an
/// incomplete LowIR slice, or generation bindings that do not exactly cover the
/// block.  Validation never normalizes or mutates the descriptor.
llvm::Error
validateTranslationBlockDescriptorV1(const TranslationBlockDescriptorV1 &Block);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_TRANSLATIONBLOCK_H
