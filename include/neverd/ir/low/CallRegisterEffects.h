//===- CallRegisterEffects.h - Callee GPR write summaries ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Which general-purpose registers a direct callee may write.
///
/// The ABI lets a callee clobber every volatile register, but a linker with
/// whole-program register allocation (MSVC /LTCG, for example) relies on the
/// callee's actual body: a caller keeps a value in RDX across a call to a
/// helper that never touches RDX.  Treating that call as an ABI clobber loses
/// the value.  This summary is the one place that answers "may the call
/// change this register", from the lifted callee bodies and their callees.
///
/// A summary exists only when the whole call tree is known: every reached
/// instruction lifted, every indirect branch resolved, and every callee is a
/// lifted function with its own summary.  Anything else keeps the ABI answer.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_CALLREGISTEREFFECTS_H
#define NEVERD_IR_LOW_CALLREGISTEREFFECTS_H

#include "neverd/Common.h"
#include "neverd/ir/low/CallRegisterBody.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

namespace libc {
class NoReturnTargetIndex;
class StackProbeIndex;
} // namespace libc

struct BinaryImage;

/// Bit I names the x86-64 GPR whose 8-byte slot starts at register offset
/// 8 * I (RAX = bit 0 ... R15 = bit 15).  A write to any byte of a slot
/// (AL, AH, EAX, ...) sets the whole family.  Bits 16 through 23 name the
/// vector argument registers XMM0 through XMM7, families as well.
using GPRFamilyMask = uint32_t;

/// The family of XMM0, the first of the eight vector argument registers.
constexpr unsigned kX64VectorFamilyBase = 16;
constexpr unsigned kX64VectorArgumentFamilies = 8;

/// In a may-write mask, a write to the x86-64 floating-point return register
/// (any view of XMM0, which is its family).  A callee that leaves RAX alone
/// may still return a value there.
constexpr GPRFamilyMask kFPReturnWriteBit = GPRFamilyMask(1)
                                            << kX64VectorFamilyBase;

/// The mask of the first \p Count vector argument families.
constexpr GPRFamilyMask vectorArgumentFamilies(unsigned Count) {
  return ((GPRFamilyMask(1) << Count) - 1) << kX64VectorFamilyBase;
}

/// Per family, how many low bytes of the register are read (0 = none, 1 for
/// CL, 2 for CX or CH, 4 for ECX, 8 for RCX; up to 16 for an XMM register).
using GPRReadWidths =
    std::array<uint8_t, kX64VectorFamilyBase + kX64VectorArgumentFamilies>;

/// Extra full-width scalar words left on every ordinary return, ordered by
/// register byte offset. ABI result registers and address identities stay out.
using GPRExitConstants = std::vector<std::pair<uint64_t, uint64_t>>;

/// The GPR family of a register slice, or nullopt when \p RegOff is not one
/// of the sixteen x86-64 GPRs this summary tracks.
std::optional<unsigned> gprFamilyOf(Arch A, uint64_t RegOff);

/// The family of a register slice, a GPR or a vector argument register, and
/// the slice's byte offset in its register.
std::optional<std::pair<unsigned, unsigned>> registerFamilyOf(Arch A,
                                                              uint64_t RegOff);

/// Whether a call keeps the \p Size bytes at \p RegOff when its direct
/// callee provably never writes the families in \p Preserved (a may-write
/// mask's complement, MedOp::CallPreservedGPRs).  A general register's family
/// covers the whole register; a vector family covers its low sixteen bytes,
/// all a summary tracks.
bool callPreservesRegisterBytes(Arch A, uint32_t Preserved, uint64_t RegOff,
                                uint16_t Size);

/// One straight-line step of a function, for register liveness.
struct RegisterStep {
  GPRReadWidths Reads{};
  /// Families this step fully redefines (a 32- or 64-bit write).
  GPRFamilyMask Kills = 0;
  /// Low bytes of a family this step writes without redefining the rest (1
  /// for DL, 2 for DX). A later read no wider than that is satisfied here.
  GPRReadWidths LowWrites{};
  /// A direct call or branch into another function's entry.
  va_t Callee = InvalidVA;
  /// \p Callee is entered by a tail call (a branch into its entry or a
  /// rewritten tail jump), so it receives this function's registers.
  bool TailCallee = false;
  /// The constant the call's own setup loads into AL, the count of vector
  /// registers a System V caller passes a variadic callee
  /// (lowByteConstantAtCall), or -1 where it loads none.  A callee whose
  /// register save area holds the vector registers
  /// (LocalRegisterEffect::SavesVectorArguments) reads the low double of
  /// each of that many as an argument.
  int8_t VectorCount = -1;
  /// Control leaves for code that no summary describes: an import tail
  /// call or an indirect tail jump.
  bool UnknownTailCall = false;
  /// A copy of a whole vector register into another (destination, source
  /// family): the source is read as wide as the destination is live.
  std::optional<std::pair<uint8_t, uint8_t>> VectorCopy;
  /// An indirect or import call that returns.
  bool UnknownCall = false;
  /// The address an import call or tail call names: an executable stub, or
  /// the slot the loader binds.  An import whose prototype fixes its entry
  /// reads (solveCallRegisterEffects) reads exactly those.
  va_t ImportCallee = InvalidVA;
  /// The routine whose entry reads give an indirect call its arguments: the
  /// one the function-pointer variable it goes through starts at
  /// (PointerVariableCallee), keyed as that routine's summary is (the
  /// variable's own address for another module's routine, whose prototype
  /// fixes the reads).  Unlike ImportCallee, nothing else about the call
  /// follows from it.
  va_t ArgumentsFrom = InvalidVA;
  /// Nothing after this step executes (a call that does not return).
  bool Exits = false;
  /// A call whose arguments a contract at this site fixes (a printf-family
  /// call with a constant format, FormattedCall): it reads exactly these and
  /// may change every volatile register, and a tail call passes nothing else
  /// on.  No summarized function is called.
  std::optional<GPRReadWidths> FixedArguments;
  bool FixedTailCall = false;
};

struct RegisterBlock {
  std::vector<RegisterStep> Steps;
  std::vector<size_t> Succs;
};

/// Direct call targets of \p F (including rewritten tail calls), and whether
/// \p F has an effect this summary cannot describe.
struct LocalRegisterEffect {
  GPRFamilyMask Writes = 0;
  GPRExitConstants ExitConstants;
  /// Complete, bounded straight-line scalar/CPUID body with an ordinary RET.
  /// Source call recovery may expand it after checking the caller's physical
  /// return-slot observations. This is not a general cached body for lifting.
  std::shared_ptr<const std::vector<LowOp>> RegisterLeaf;
  std::set<va_t> Callees;
  /// Direct targets of calls that never return, but for those the name list
  /// knows: proved ones, and the code the user states never returns.  Their
  /// writes reach no caller, so they stay out of Callees, but their summaries
  /// still give those calls their parameters.
  std::set<va_t> NoReturnCallees;
  /// Some effect escapes the may-write summary (an unknown call, an
  /// incomplete lift).
  bool Unknown = false;
  /// A call with fixed arguments (RegisterStep::FixedArguments) may change
  /// every volatile register.
  bool CallsFixedContract = false;
  /// The body itself is not fully known, so neither summary exists.
  bool Incomplete = false;
  /// Control leaves for code no summary describes (an indirect tail call)
  /// while this function's argument registers may still be the caller's;
  /// which of them that code reads is unknown.  An import tail call leaves
  /// this to the solver, which knows the prototypes (RegisterStep::
  /// ImportCallee).
  bool UnknownEntryReads = false;
  /// Independent entries, exceptional edges or malformed successors are not
  /// represented by the ordinary register-flow graph. They forbid an absence
  /// of observations proof without changing the argument-inference contract.
  bool UnknownObservers = false;
  /// Liveness skeleton; block 0 is the entry.
  std::vector<RegisterBlock> Blocks;
  /// Positional arguments implied by the incoming stack-argument slots the
  /// body reads (0 when it reads none). Meaningful only without
  /// UnknownStackReads.
  int StackArgs = 0;
  /// The body may read incoming stack arguments that this summary cannot
  /// bound: the stack pointer is lost, a pointer into the incoming arguments
  /// escapes, or control leaves for code no summary describes.
  bool UnknownStackReads = false;
  /// Callees entered by a tail call at the entry stack pointer, imports by
  /// the stub or slot they are reached through; they read this function's
  /// incoming stack arguments as their own.
  std::set<va_t> StackTailCallees;
  /// A Win64 variadic function (`f(fmt, ...)`): the register position of its
  /// first variadic argument, else -1.  Its prologue spills that register and
  /// every later argument register to their home slots and hands a pointer to
  /// the first of those slots on as the va_list.  Those spills are not
  /// argument reads: a caller passes exactly the variadic arguments it sets.
  int VariadicFrom = -1;
  /// A System V variadic function whose prologue tests AL and, where it is
  /// not zero, saves the vector argument registers to its register save
  /// area (findRegisterSaveAreaSpills).  Those spills are not argument reads
  /// either: va_arg reads the floating arguments a caller passes there, the
  /// first AL of them, each promoted to double.  A call that loads AL with a
  /// constant passes that many (RegisterStep::VectorCount).
  bool SavesVectorArguments = false;
};

/// Summarize \p F.  A call into a function \p NoReturnTargets names as
/// never returning (a flagged call, or a tail call the LowIR contract keeps
/// unflagged) ends its path: nothing it writes reaches a caller.
/// A call whose instruction address \p FixedCallReads names reads exactly
/// those bytes (RegisterStep::FixedArguments).  A call through a
/// function-pointer variable whose address \p ArgumentSources names reads
/// what the routine it names reads (RegisterStep::ArgumentsFrom).  \p
/// StackProbes answers which calls enter a stack probe, when given.
LocalRegisterEffect localRegisterEffect(
    const BinaryImage &Img, const LowFunc &F,
    const libc::NoReturnTargetIndex *NoReturnTargets = nullptr,
    const std::map<va_t, GPRReadWidths> *FixedCallReads = nullptr,
    const std::map<va_t, va_t> *ArgumentSources = nullptr,
    const libc::StackProbeIndex *StackProbes = nullptr);

LocalRegisterEffect localRegisterEffect(
    const BinaryImage &Img, CallRegisterBodyView F,
    const libc::NoReturnTargetIndex *NoReturnTargets = nullptr,
    const std::map<va_t, GPRReadWidths> *FixedCallReads = nullptr,
    const std::map<va_t, va_t> *ArgumentSources = nullptr,
    const libc::StackProbeIndex *StackProbes = nullptr);

/// Join each run of \p Effect's steps that only read and write registers
/// (no callee, no import, no exit, no fixed arguments, no vector copy) into
/// one step with the same effect on the bytes live before it.
/// localRegisterEffect returns summaries so joined: solving them gives what
/// solving the instruction-by-instruction steps gives, over fewer steps.
void joinPlainSteps(LocalRegisterEffect &Effect);

struct CallRegisterSummaries {
  /// Families a call may change, for functions whose whole call tree is
  /// known.
  std::map<va_t, GPRFamilyMask> MayWrite;
  /// Constants established locally on every return of a bounded call-free
  /// body. Unknown paths, exceptional roots and addresses supply no fact.
  std::map<va_t, GPRExitConstants> ExitConstants;
  /// Bytes of each family read before being written on some path from
  /// entry, including through callees.  A pass-through argument counts; a
  /// register a nested call merely receives does not.  The vector registers
  /// a call's AL counts for a callee that saves them
  /// (LocalRegisterEffect::SavesVectorArguments) are read as doubles.
  std::map<va_t, GPRReadWidths> EntryReads;
  /// Non-result/control GPR families whose incoming values no path can observe.
  /// Preserved-register exit transport still needs the caller's ABI check.
  /// Unlike argument inference, this proof treats opaque calls as reading every
  /// register and
  /// never uses a nested call's may-write mask as a definite overwrite.
  std::map<va_t, GPRFamilyMask> Unread;
  /// Positional arguments implied by the incoming stack slots a function
  /// reads, including through tail calls (0 when none), for functions whose
  /// stack reads are bounded, and for the imports whose counts are fixed.
  std::map<va_t, int> EntryStackArgs;
  /// Register position of the first variadic argument of each variadic
  /// function (LocalRegisterEffect::VariadicFrom).
  std::map<va_t, int> VariadicFrom;
};

/// Solve may-write and entry-read GPR sets over \p Funcs (entry -> local
/// effect).  A callee missing from \p Funcs, or reaching an unknown effect,
/// has no may-write summary.  An unknown call is taken to clobber
/// \p VolatileFamilies.  A function that tail-calls code no summary
/// describes (an import, an indirect target, or a function without an
/// entry-read summary) has no entry-read summary either.
///
/// A call to one of \p DispatchThunks (an indirect-call dispatcher such as
/// MSVC's `_guard_dispatch_icall`, which jumps to RAX with the caller's
/// argument registers) is an indirect call whose target is unknown: it
/// clobbers \p VolatileFamilies and does not make the caller read its own
/// incoming argument registers.  The call site's arguments are the registers
/// the caller itself set.  A function in \p FixedEntryReads (a documented
/// prototype) reads exactly those bytes.  So does an import whose stub or
/// bound slot it lists: a call to the import passes them, and a tail call
/// into it forwards exactly them, while a tail call into any other import
/// leaves the caller's entry reads unknown.  Likewise an import whose stub
/// or slot \p FixedEntryStackArgs lists reads that many stack arguments, and
/// a tail call into it forwards them; a tail call into another import
/// leaves the caller's stack reads unbounded.
CallRegisterSummaries solveCallRegisterEffects(
    const std::map<va_t, LocalRegisterEffect> &Funcs,
    GPRFamilyMask VolatileFamilies, GPRFamilyMask ArgumentFamilies,
    const std::set<va_t> &DispatchThunks = {},
    const std::map<va_t, GPRReadWidths> &FixedEntryReads = {},
    const std::map<va_t, int> &FixedEntryStackArgs = {});

/// Each function's local effect, by entry, where another owner keeps it (as
/// a cache of callee effects does).
using LocalRegisterEffectRefs = std::map<va_t, const LocalRegisterEffect *>;

/// Prove unobserved incoming GPR families from the same register-flow bodies.
/// Opaque effects, partial writes and exhausted work retain the input. Argument
/// counts and pointer-variable initializer hints cannot establish this fact.
std::map<va_t, GPRFamilyMask>
proveUnreadCallRegisters(const LocalRegisterEffectRefs &Funcs);

/// As above, over effects \p Funcs points at.
CallRegisterSummaries solveCallRegisterEffects(
    const LocalRegisterEffectRefs &Funcs, GPRFamilyMask VolatileFamilies,
    GPRFamilyMask ArgumentFamilies, const std::set<va_t> &DispatchThunks = {},
    const std::map<va_t, GPRReadWidths> &FixedEntryReads = {},
    const std::map<va_t, int> &FixedEntryStackArgs = {});

} // namespace neverd

#endif // NEVERD_IR_LOW_CALLREGISTEREFFECTS_H
