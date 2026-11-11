//===- X86LegacySegmentExecution.h - Protected segment moves --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute explicit protected/VM86 segment transfers over owned paged RAM.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_X86LEGACYSEGMENTEXECUTION_H
#define NEVERD_TRANSLATE_X86_X86LEGACYSEGMENTEXECUTION_H

#include "neverd/translate/X86/X86PagedMemory.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd::translate {

struct X86LegacySegmentState {
  /// Encoding order: EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI.
  std::array<uint32_t, 8> GPR{};
  /// X86SegmentRegister encoding order: ES, CS, SS, DS, FS, GS.
  std::array<X86LegacySegmentCache, 6> Segments{};
  uint32_t GDTRBase = 0;
  uint16_t GDTRLimit = 0;
  X86LegacySegmentCache LDTR;
  uint8_t CPL = 0;
};

/// Explicit target binding: POP/PUSH access a selector word even with a
/// four-byte stack adjustment; LxS reads offset then selector; descriptor
/// accessed updates use eight bytes. MOV r32,Sreg clears the high word.
enum class X86LegacySegmentProfile : uint8_t { WordStackOffsetFirst };

struct X86LegacySegmentExecutionOptions {
  std::optional<X86LegacySegmentProfile> Profile;
  /// Required when CPL3 alignment checking is active.
  std::optional<X86AlignmentPriority> AlignmentPriority;
  /// Only used by a particular explicit operand crossing a full 4-GiB limit.
  std::optional<X86FullSegmentLimitChoice> FullLimitChoice;
};

struct X86LegacySegmentExecutionResult : X86MemoryAccessResult {
  /// Successful MOV/POP SS starts a scheduler-owned inhibition window,
  /// subject to the scheduler's incoming MOV-SS shadow rule. LSS does not.
  bool LoadsSSWithInhibition = false;
};

/// Shared external-state admission; performs no RAM accesses or mutations.
llvm::Error validateX86LegacySegmentState(const X86LegacySegmentState &State,
                                          const X86PagingState &Controls,
                                          bool Compatibility = false);

/// Instruction-transition boundary, like loadX64PagedFarPointer. The caller
/// owns original instruction fetch/authentication, EIP, RF, interruptibility,
/// event arbitration and fault delivery. This function authenticates the
/// complete byte grammar, executes MOV Sreg/rm, PUSH/POP Sreg and all five LxS
/// forms in protected 16/32-bit code, and commits coupled GPR/cache changes
/// only on success. Incoming caches are already loaded; descriptor accesses
/// are implicit supervisor accesses. Paging A/D effects survive failure.
///
/// One confined CPU and ordinary RAM, no concurrent table mutation or MMIO.
/// An unusable null-loaded cache retains opaque old fields; only its selector
/// and unusable state are defined. No consumer may observe those old fields
/// as a usable segment. VM86 uses paragraph caches selected by RFlags.VM;
/// real/IA-32e and unbound profiles fail explicitly.
llvm::Expected<X86LegacySegmentExecutionResult>
executeX86LegacySegmentInstruction(
    X86LegacySegmentState &State, X86PagedMemory &Memory,
    llvm::ArrayRef<uint8_t> Instruction,
    const X86LegacySegmentExecutionOptions &Options);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86LEGACYSEGMENTEXECUTION_H
