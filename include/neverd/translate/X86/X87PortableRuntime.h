//===- X87PortableRuntime.h - Checked portable x87 steps -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Host-owned state and transactional guest-memory adaptation for the portable
/// x87 semantic owner. This is separate from the integer translation ABI.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_X87PORTABLERUNTIME_H
#define NEVERD_TRANSLATE_X86_X87PORTABLERUNTIME_H

#include "neverd/ir/X86/X87Context.h"
#include "neverd/translate/GuestMemoryRuntime.h"

#include "llvm/Support/Error.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace neverd::translate {

inline constexpr uint32_t kPortableX87StateMagicV1 = 0x3738564e;
inline constexpr uint16_t kPortableX87StateVersionV1 = 1;
inline constexpr uint16_t kPortableX87StateSizeV1 = 528;

/// An explicit environment and physical-payload provenance boundary. Image
/// uses the existing FXSAVE64 logical-slot layout; KnownPayload uses physical
/// bits, including retained empty slots. A zero-initialized object establishes
/// no architectural state. Only a trusted caller can authenticate an image;
/// an ABI declaration, memory mapping or nonempty tag is not authentication.
struct alignas(16) PortableX87StateV1 {
  uint32_t Magic = kPortableX87StateMagicV1;
  uint16_t Version = kPortableX87StateVersionV1;
  uint16_t Size = kPortableX87StateSizeV1;
  uint8_t EnvironmentKnown = 0;
  uint8_t KnownPayload = 0;
  uint8_t Reserved[6] = {};
  std::array<uint8_t, X87ContextHostOffset> Image{};
};

static_assert(sizeof(PortableX87StateV1) == kPortableX87StateSizeV1);
static_assert(offsetof(PortableX87StateV1, Image) == 16);
static_assert(std::is_standard_layout_v<PortableX87StateV1>);
static_assert(std::is_trivially_copyable_v<PortableX87StateV1>);

/// Stable protocol statuses, mapped explicitly from the internal semantic
/// outcome. Refusal is not OS exception delivery. SelfModification means the
/// entire instruction completed and its corresponding state must be retained.
enum class PortableX87StepStatusV1 : uint32_t {
#define NEVERD_X87_PORTABLE_STATUS(CxxName, CName, Value, Diagnostic)          \
  CxxName = Value,
#include "neverd/ir/X86/X87PortableStatus.def"
#undef NEVERD_X87_PORTABLE_STATUS
};

struct PortableX87StepResultV1 {
  PortableX87StepStatusV1 Status = PortableX87StepStatusV1::InvalidState;
  bool InstructionCompleted = false;
  bool HasScalarResult = false;
  uint64_t ScalarResult = 0;
  /// Present only if this instruction checked/accessed its memory operand;
  /// never copy a previous fault into a preflight refusal or register step.
  std::optional<GuestMemoryAccessResult> MemoryAccess;
};

llvm::Error validatePortableX87StateV1(const PortableX87StateV1 &State);
const char *portableX87StepStatusNameV1(PortableX87StepStatusV1 Status);

/// Execute one admitted operation using guest addresses and separately owned
/// checked memory. Pending waiting exceptions precede operand access. The
/// current instruction changes no architectural state on refusal or fault;
/// successful executable writes commit both bytes and x87 state before exit.
/// The caller owns PC advancement, accounting and control-protocol selection.
PortableX87StepResultV1 executePortableX87RuntimeStepV1(
    PortableX87StateV1 &State, GuestMemoryRuntime &Memory, uint32_t Selector,
    uint64_t InstructionAddress, uint64_t DataAddress);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X87PORTABLERUNTIME_H
