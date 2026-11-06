//===- X86DebugRules.h - Instruction debug status -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared nontransactional instruction debug controls, matching and status.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_X86_X86DEBUGRULES_H
#define NEVERD_IR_X86_X86DEBUGRULES_H

#include <array>
#include <cstdint>

namespace neverd {

/// Intel's fault-class RF rule and the clear-RF result observed through VMX
/// are separate bindings. Absence refuses an actual GD cause, not other MOVs.
enum class X86GeneralDetectFrame : uint8_t { FaultResume, ClearResume };

/// The comparator profiles own canonical instruction comparators (RW=LEN=0).
/// All profiles clear inactive B0..B3 on #DB. Single-step traps also report
/// enabled comparators at the next PC unless restored RF inhibits them.
/// These are explicit target choices, never inferred from the executing host.
enum class X86InstructionDebugProfile : uint8_t {
  InstructionStep,
  ExecuteBreakpoints,
  /// MOV-DR with preserved arithmetic flags, fixed DR6 BLD/RTM, and GD.
  /// Other debug extensions and data/I/O comparators remain unbound.
  DebugRegisters,
  /// Four Intel length-masked data/I/O comparators, including eight-byte
  /// ranges. Event-table/frame accesses participate; page walks do not.
  /// Delayed MOV/POP-SS causes survive software entry, whose new accesses
  /// replace their B0..B3 status. Other debug extensions remain unbound.
  Watchpoints,
};

template <typename Word> struct X86InstructionDebugState {
  Word DR6 = 0xffff0ff0;
  Word DR7 = 0x400;
  uint64_t DebugCTL = 0;
  std::array<Word, 4> DR{};
  /// Data causes postponed by MOV/POP SS or a bound REP group, distinct from
  /// published DR6. Retain across host stops; accepted ordinary events clear.
  uint8_t PendingData = 0;
};

inline bool
x86InstructionDebugValid(uint64_t DR6, uint64_t DR7, uint64_t DebugCTL,
                         X86InstructionDebugProfile Profile =
                             X86InstructionDebugProfile::InstructionStep,
                         uint64_t CR4 = 0, uint8_t PendingData = 0) {
  const bool Watch = Profile == X86InstructionDebugProfile::Watchpoints;
  const bool Controls =
      Profile == X86InstructionDebugProfile::InstructionStep ? DR7 == 0x400
      : Profile == X86InstructionDebugProfile::ExecuteBreakpoints
          ? (DR7 & ~uint64_t{0x3ff}) == 0x400
          : (Profile == X86InstructionDebugProfile::DebugRegisters || Watch) &&
                (DR7 & ~(Watch ? uint64_t{0xffff23ff} : uint64_t{0x23ff})) ==
                    0x400 &&
                (DR6 & 0x10800) == 0x10800;
  if (PendingData & (Watch ? 0xf0 : 0xff))
    return false;
  if (Watch)
    for (unsigned I = 0; I != 4; ++I) {
      if (!(DR7 & (uint64_t{3} << (I * 2))))
        continue;
      const unsigned Kind = (DR7 >> (16 + I * 4)) & 3;
      const unsigned Length = (DR7 >> (18 + I * 4)) & 3;
      if ((!Kind && Length) || (Kind == 2 && (!(CR4 & 8) || Length == 2)))
        return false;
    }
  return Controls && DebugCTL == 0 && (DR6 & 0xffffffff00000000ULL) == 0 &&
         (DR6 & 0xfffe07f0u) == 0xfffe07f0u && !(DR6 & 0x1000);
}

/// Address is the original linear instruction start, before paging. Width
/// normalization belongs to the architecture's address owner. RF and the
/// incoming MOV/POP-SS window suppress the fault, without postponing a cause.
template <typename Address>
inline uint8_t x86ExecutionBreakpoints(const std::array<Address, 4> &DR,
                                       uint64_t DR7, uint64_t LinearAddress,
                                       uint64_t Flags, bool SSBlocked) {
  if ((Flags & 0x10000) || SSBlocked)
    return 0;
  uint8_t Matches = 0;
  for (unsigned I = 0; I != DR.size(); ++I)
    if ((DR7 & (uint64_t{3} << (I * 2))) &&
        !(DR7 & (uint64_t{3} << (16 + I * 4))) && DR[I] == LinearAddress)
      Matches |= 1u << I;
  return Matches;
}

enum class X86WatchpointAccess : uint8_t { Read, Write, Port };

/// One completed memory-string element, before its debug cause publishes.
/// Index/count values have the instruction's address width. This is a target
/// input: fast-string groups need not end after each element, even on another
/// core of the same processor. No policy is inferred from the executing host.
struct X86RepeatDebugStep {
  uint64_t PC = 0, Flags = 0, Count = 0, Source = 0, Destination = 0;
  uint8_t AddressBytes = 0, ElementBytes = 0;
  bool SourceIndex = false, DestinationIndex = false, Compare = false;
};

struct X86RepeatDebugBinding {
  /// A pure decision over the completed step and caller-owned target context.
  /// Returning false retains data causes until a later MOVS/STOS group
  /// boundary. It does not repeat RAM/device effects or delay TF, other string
  /// operations, ports or final completion. {} explicitly selects each element.
  bool (*EndGroup)(void *, const X86RepeatDebugStep &) =
      [](void *, const X86RepeatDebugStep &) { return true; };
  void *Context = nullptr;
};

inline bool x86RepeatDebugDefers(const X86RepeatDebugBinding &Binding,
                                 const X86RepeatDebugStep &Step) {
  return !(Step.Flags & 0x100) && Step.Count && !Step.Compare &&
         Step.DestinationIndex && !Binding.EndGroup(Binding.Context, Step);
}

/// Linear byte ranges, independent of CPL, AC and RF. The qualified target
/// masks misaligned DR addresses to the encoded length. Word supplies legacy
/// linear wrap; callers authenticate the complete operand before publication.
template <typename Word>
inline uint8_t x86DataBreakpoints(const std::array<Word, 4> &DR, uint64_t DR7,
                                  Word Address, uint64_t Bytes,
                                  X86WatchpointAccess Access) {
  uint8_t Matches = 0;
  for (unsigned I = 0; I != DR.size(); ++I) {
    if (!(DR7 & (uint64_t{3} << (I * 2))))
      continue;
    const unsigned Kind = (DR7 >> (16 + I * 4)) & 3;
    if (Access == X86WatchpointAccess::Port
            ? Kind != 2
            : Kind != 3 && !(Kind == 1 && Access == X86WatchpointAccess::Write))
      continue;
    const unsigned Encoding = (DR7 >> (18 + I * 4)) & 3;
    const unsigned Length = std::array<unsigned, 4>{1, 2, 8, 4}[Encoding];
    const Word Base = DR[I] & ~Word(Length - 1);
    if (Word(Address - Base) < Length || Word(Base - Address) < Bytes)
      Matches |= 1u << I;
  }
  return Matches;
}

template <typename Bank>
inline uint8_t x86CompleteDataBreakpoints(Bank &Debug, uint8_t Matches,
                                          bool NewSSShadow,
                                          bool RepeatGroupContinues = false) {
  Debug.PendingData |= Matches;
  if (NewSSShadow || RepeatGroupContinues)
    return 0;
  const uint8_t Pending = Debug.PendingData;
  Debug.PendingData = 0;
  return Pending;
}

/// Inactive B0..B3 clear, RTM becomes one, sticky status survives. ICEBP does
/// not set BS merely because entry TF was set. Historical BLD/RTM inputs
/// require a target capable of representing those statuses.
inline uint64_t x86InstructionDebugStatus(uint64_t DR6, bool SingleStep,
                                          uint8_t Breakpoints = 0) {
  return (DR6 & ~uint64_t{0xf}) | 0x10000 | (SingleStep ? 0x4000 : 0) |
         Breakpoints;
}

/// Accepted event entry consumes the old delayed cause. Software entry keeps
/// its trap request but the target reports only the entry's new data matches.
/// A newly recognized data trap also reports next-PC execution comparators.
template <typename Bank>
inline bool x86EventDataBreakpoints(Bank &Debug, bool Software, uint8_t Matches,
                                    uint64_t LinearPC, uint64_t Flags) {
  const bool Deliver = Matches || (Software && Debug.PendingData);
  Debug.PendingData = 0;
  if (!Deliver)
    return false;
  Matches |=
      x86ExecutionBreakpoints(Debug.DR, Debug.DR7, LinearPC, Flags, false);
  Debug.DR6 = x86InstructionDebugStatus(Debug.DR6, false, Matches);
  return true;
}

} // namespace neverd
#endif // NEVERD_IR_X86_X86DEBUGRULES_H
