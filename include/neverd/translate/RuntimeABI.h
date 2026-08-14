//===- RuntimeABI.h - Backend-private translation runtime ABI -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the fixed-layout control block and the finite helper surface used
/// at the host-IR validation boundary.  This is not GuestState's C++ layout or
/// wire format.  A backend must explicitly convert logical state into this ABI
/// version and bind every helper from the exact table below.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_RUNTIMEABI_H
#define NEVERD_TRANSLATE_RUNTIMEABI_H

#include "neverd/translate/TranslationIRVerifier.h"
#include "neverd/translate/TranslationOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace llvm {
class LLVMContext;
} // namespace llvm

namespace neverd::translate {

/// Little-endian bytes spell "NVRT" when this value is serialized.
inline constexpr uint32_t kRuntimeABIMagicV1 = 0x5452564e;
inline constexpr uint16_t kRuntimeABIVersionV1 = 1;
inline constexpr uint16_t kRuntimeControlBlockSizeV1 = 128;

enum class RuntimeABIExitKindV1 : uint32_t {
  None = 0,
  MemoryFault = 1,
  SelfModification = 2,
  BudgetExhausted = 3,
  Cancelled = 4,
};

enum class RuntimeABIFlagV1 : uint32_t {
  None = 0,
  /// The last runtime operation validated CurrentPC's executable generation.
  GenerationValidated = 1u << 0,
};

/// Exact guest-memory failure recorded in RuntimeABIExitV1::Fault.
enum class RuntimeMemoryFaultKindV1 : uint32_t {
  None = 0,
  InvalidAccessWidth = 1,
  InvalidAlignment = 2,
  Misaligned = 3,
  AddressOverflow = 4,
  Unmapped = 5,
  CrossRegion = 6,
  PermissionDenied = 7,
  ExecutableWriteRejected = 8,
  ExecutableGenerationOverflow = 9,
  ExecutableGenerationMismatch = 10,
  PolicyViolation = 11,
  InvalidRuntimeFrame = 12,
};

/// Stable guest-memory operation identities used inside a memory-fault detail
/// encoding.  Append values without renumbering; zero and values above Execute
/// are invalid in ABI v1.
enum class RuntimeMemoryAccessKindV1 : uint8_t {
  Read = 1,
  Write = 2,
  Execute = 3,
};

static_assert(static_cast<uint8_t>(RuntimeMemoryAccessKindV1::Read) == 1 &&
              static_cast<uint8_t>(RuntimeMemoryAccessKindV1::Write) == 2 &&
              static_cast<uint8_t>(RuntimeMemoryAccessKindV1::Execute) == 3);

/// Semantic memory-fault fields independent of their packed exit storage.
struct RuntimeMemoryFaultDetailsV1 {
  RuntimeMemoryAccessKindV1 Access = RuntimeMemoryAccessKindV1::Read;
  uint32_t RequiredAlignment = 0;
  uint64_t ExpectedGeneration = 0;
  uint64_t ObservedGeneration = 0;
};

struct RuntimeMemoryFaultDetailEncodingV1 {
  uint64_t Detail0 = 0;
  uint64_t Detail1 = 0;
};

static_assert(sizeof(RuntimeMemoryFaultDetailEncodingV1) == 16);
static_assert(alignof(RuntimeMemoryFaultDetailEncodingV1) == 8);
static_assert(std::is_standard_layout_v<RuntimeMemoryFaultDetailEncodingV1>);
static_assert(std::is_trivially_copyable_v<RuntimeMemoryFaultDetailEncodingV1>);
static_assert(offsetof(RuntimeMemoryFaultDetailEncodingV1, Detail0) == 0);
static_assert(offsetof(RuntimeMemoryFaultDetailEncodingV1, Detail1) == 8);

/// Pack memory-fault semantics into RuntimeABIExitV1::Detail0/Detail1.
///
/// Ordinary faults store Access in Detail0 bits 0..7 and RequiredAlignment in
/// bits 32..63; bits 8..31 and Detail1 are reserved zero.  A generation
/// mismatch stores arbitrary expected/observed generations in Detail0/Detail1
/// and implies Execute/alignment-zero.  Generation overflow stores the normal
/// Write/alignment word in Detail0, the observed maximum generation in Detail1,
/// and implies expected generation zero.
llvm::Expected<RuntimeMemoryFaultDetailEncodingV1>
packRuntimeMemoryFaultDetailsV1(RuntimeMemoryFaultKindV1 Fault,
                                const RuntimeMemoryFaultDetailsV1 &Details);

/// Decode and validate a fault-specific detail representation.  Unknown
/// access values, reserved bits, contradictory access kinds, and invalid
/// alignment shapes are rejected rather than normalized.  InvalidAlignment is
/// the sole kind for which a nonzero non-power-of-two value is valid evidence;
/// that kind rejects zero and power-of-two values instead.
llvm::Expected<RuntimeMemoryFaultDetailsV1> unpackRuntimeMemoryFaultDetailsV1(
    RuntimeMemoryFaultKindV1 Fault,
    const RuntimeMemoryFaultDetailEncodingV1 &Encoding);

/// Fixed exit record.  Detail fields are interpreted only by Kind/Fault:
/// memory faults use packRuntimeMemoryFaultDetailsV1(), self-modification
/// stores old/new generations, and a budget exit stores kind/limit.
struct alignas(8) RuntimeABIExitV1 {
  RuntimeABIExitKindV1 Kind = RuntimeABIExitKindV1::None;
  RuntimeMemoryFaultKindV1 Fault = RuntimeMemoryFaultKindV1::None;
  uint64_t Address = 0;
  uint64_t Size = 0;
  uint64_t Detail0 = 0;
  uint64_t Detail1 = 0;
};

/// Backend-private v1 control block visible to translated host IR.
///
/// The structure contains no C++ containers, host pointers, or guest-address
/// aliases.  It is a control/result record; the checked memory index remains
/// owned by GuestMemoryRuntime and cannot be addressed by guest virtual
/// address arithmetic.  Unknown flags and all reserved fields must be zero.
struct alignas(8) RuntimeControlBlockV1 {
  uint32_t Magic = kRuntimeABIMagicV1;
  uint16_t Version = kRuntimeABIVersionV1;
  uint16_t Size = kRuntimeControlBlockSizeV1;
  uint32_t CodeInvalidation = 0;
  uint32_t Flags = 0;
  uint64_t CurrentPC = 0;
  uint64_t ExpectedGeneration = 0;
  uint64_t ObservedGeneration = 0;
  uint64_t InstructionBudget = 0;
  uint64_t InstructionCount = 0;
  uint64_t BlockBudget = 0;
  uint64_t BlockCount = 0;
  uint64_t ScalarResult = 0;
  uint32_t CancellationRequested = 0;
  uint32_t Reserved1 = 0;
  RuntimeABIExitV1 Exit;
};

static_assert(sizeof(RuntimeABIExitV1) == 40);
static_assert(alignof(RuntimeABIExitV1) == 8);
static_assert(std::is_standard_layout_v<RuntimeABIExitV1>);
static_assert(std::is_trivially_copyable_v<RuntimeABIExitV1>);
static_assert(sizeof(RuntimeControlBlockV1) == kRuntimeControlBlockSizeV1);
static_assert(alignof(RuntimeControlBlockV1) == 8);
static_assert(std::is_standard_layout_v<RuntimeControlBlockV1>);
static_assert(std::is_trivially_copyable_v<RuntimeControlBlockV1>);
static_assert(offsetof(RuntimeControlBlockV1, Flags) == 12);
static_assert(offsetof(RuntimeControlBlockV1, CurrentPC) == 16);
static_assert(offsetof(RuntimeControlBlockV1, ExpectedGeneration) == 24);
static_assert(offsetof(RuntimeControlBlockV1, ObservedGeneration) == 32);
static_assert(offsetof(RuntimeControlBlockV1, InstructionBudget) == 40);
static_assert(offsetof(RuntimeControlBlockV1, InstructionCount) == 48);
static_assert(offsetof(RuntimeControlBlockV1, BlockBudget) == 56);
static_assert(offsetof(RuntimeControlBlockV1, BlockCount) == 64);
static_assert(offsetof(RuntimeControlBlockV1, ScalarResult) == 72);
static_assert(offsetof(RuntimeControlBlockV1, CancellationRequested) == 80);
static_assert(offsetof(RuntimeControlBlockV1, Exit) == 88);

RuntimeControlBlockV1
makeRuntimeControlBlockV1(CodeInvalidationPolicy CodeInvalidation,
                          uint64_t InstructionBudget = 0,
                          uint64_t BlockBudget = 0);

/// Rejects unknown versions, sizes, policies, reserved values, and incoherent
/// exit records before dispatch consumes the control block.
llvm::Error validateRuntimeControlBlockV1(const RuntimeControlBlockV1 &Block);

enum class RuntimeABIValueKind : uint8_t {
  Void = 0,
  I32 = 1,
  I64 = 2,
  StatePointer = 3,
  RuntimePointer = 4,
};

/// One exact helper signature.  Parameters describe both LLVM scalar width
/// and pointer provenance; the parallel arrays always have equal length.
struct RuntimeABIHelperSignatureV1 {
  llvm::StringRef Name;
  RuntimeABIValueKind Result = RuntimeABIValueKind::Void;
  llvm::ArrayRef<RuntimeABIValueKind> Parameters;
  llvm::ArrayRef<TranslationRuntimeParameterKind> VerifierParameters;
};

/// The only v1 runtime helper names admitted by backend IR policy.  The table
/// contains signatures, never process addresses; a backend must bind it
/// explicitly and must not fall back to ambient symbol lookup.  Every i32
/// result is the numeric RuntimeABIExitKindV1 value.  A successful load returns
/// None and writes the zero-extended scalar to ScalarResult.
llvm::ArrayRef<RuntimeABIHelperSignatureV1> runtimeABIHelperSignaturesV1();

/// Direct translated-code access is limited to the load-result slot.  Runtime
/// helpers and the outer host dispatcher own all remaining control-block
/// fields.  Dispatch itself is deliberately not callable from translated IR.
llvm::ArrayRef<TranslationIRMemorySlot> runtimeABIMemorySlotsV1();

/// Exact-name lookup.  Prefix, suffix, and case-folded matches are rejected.
const RuntimeABIHelperSignatureV1 *
findRuntimeABIHelperSignatureV1(llvm::StringRef Name);

/// Materialize the fixed table as TranslationIRVerifier contracts in Context.
/// LLVM owns the FunctionType objects; parameter provenance refers to static
/// table storage.
std::vector<TranslationRuntimeHelper>
createRuntimeABIHelperPolicyV1(llvm::LLVMContext &Context);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_RUNTIMEABI_H
