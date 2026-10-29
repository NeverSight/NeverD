//===- MedPrologueSaves.h - Callee-save prologue storage -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A prologue saves the callee-saved registers its body uses and the
/// epilogue restores them: storage that keeps the caller's registers, which
/// no source statement reads.  A source view leaves those saves out, as every
/// C function leaves its caller's state alone.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDPROLOGUESAVES_H
#define NEVERD_IR_MED_MEDPROLOGUESAVES_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>

namespace neverd {
struct BinaryImage;
struct IntegerArgumentLayout;

/// Whether \p V is the value register \p RegOff holds at \p Func's entry:
/// its version zero, or the entry seed that copies it (`COPY RSI.1 = RSI`).
bool isEntryLiveInValue(const MedFunc &Func, const MedVar &V, uint64_t RegOff);

/// A bounded extent in the function's private frame or incoming ABI home
/// storage. This classifies storage, not its initialization or capture state.
bool isSourcePrivateFrameAccess(const MedFunc &Func, Arch Architecture,
                                BinaryFormat Format, int64_t Offset,
                                uint16_t Bytes);

/// Cheap source-proof admission for an entry register forwarded in its own
/// ordinary argument position. This requests the callee's current body; it
/// grants no unused-input or destruction fact. Body discovery, retained LowIR
/// and proof scheduling must admit the same full-width forwarding chains.
bool forwardsSourceEntryRegister(const MedOp &Call,
                                 const IntegerArgumentLayout &Layout);

/// Mark source-mode prologue saves, unobserved scratch spills and
/// return-address reads and neutral return-slot transactions using one frame
/// access analysis. An unread atomic addition of zero wholly within an x86
/// CALL return slot retains its transaction on the compiler-owned slot.
/// Complete fixed integer
/// call operands reading private frame bytes proved never written on any
/// incoming path become explicit unavailable values in source only. General
/// partial-byte observations are separate. The analysis is discarded before
/// returning; operation positions stay stable. An optional current image
/// identifies complete data-object reads disjoint from the private source
/// frame; unknown addresses and external stores still stop the spill proof.
/// Optional Overwritten receives only stores whose old bytes are reset before
/// any read, including source-dead restores; its pointers expire on mutation.
void markSourceStackAccesses(MedFunc &Func, Arch Architecture,
                             BinaryFormat Format,
                             StackEntryKind Entry = StackEntryKind::Call,
                             const BinaryImage *Image = nullptr,
                             std::set<const MedOp *> *Overwritten = nullptr);

/// Positional Win64 register inputs whose only uses are source-proved dead
/// spills and dead identity transports. Every implicit call observation must
/// follow a full overwrite or have a bound unread-register proof. The caller
/// must supply a complete current body with freshly prepared source stores.
/// This does not remove parameter slots or authorize a native ABI rewrite.
std::set<unsigned> unobservedSourceSpillParameters(const MedFunc &Func);

/// Source-only low-prefix demand, by positional Win64 integer parameter.
/// Missing entries retain the whole carrier; zero requires a proved discarded
/// spill or forwarding to another proved unobserved input. Facts are local to
/// one preparation of complete bodies and must never enter native summaries.
using SourceRegisterParameterReads = std::map<unsigned, uint16_t>;
using SourceCalleeParameterReads = std::map<va_t, SourceRegisterParameterReads>;

/// Original control-flow prefixes that destroy an input without leaving a copy
/// in another register or live memory. This is stronger than a may-write mask
/// or an unused source parameter, and is private to the current source proof.
using SourceCalleeParameterOverwrites = std::map<va_t, std::set<unsigned>>;

/// Prove destruction on every original control-flow path, never from dead-code
/// elimination of physical register writes. Each ignored store must occur in
/// freshly prepared Overwritten: no old byte is read before its full reset.
/// The caller supplies the complete original CFG of this body. Cycles before
/// destruction and unresolved or exceptional edges do not prove destruction.
/// A standalone, unaligned GS observer without a frame register may also prove
/// an input directly when its cookie excludes the callee home/private area.
/// That case permits no original spill or other copy of the input at all;
/// the ordinary source-unused-spill proof never supplies its absence.
std::set<unsigned> sourceRegisterInputOverwrites(
    const MedFunc &Func, llvm::ArrayRef<LowBlock> OriginalBlocks,
    const SourceRegisterParameterReads &Reads,
    const SourceCalleeParameterOverwrites &Callees,
    const std::set<const MedOp *> &Overwritten, size_t &Remaining);

/// Unread positional stack inputs of a complete straight-line Win64 wrapper.
/// Original loads, including removed ones, observe their entire byte ranges.
/// Only exact SP-relative addresses and declared, bounded ordinary calls are
/// admitted; frame escapes, lost offsets and unknown effects reject the proof.
/// OriginalEntry must be the complete original body, including its return.
std::set<unsigned> unobservedSourceStackParameters(
    const MedFunc &Func, llvm::ArrayRef<LowOp> OriginalEntry,
    const std::set<va_t> &DeclaredCallees, size_t &Remaining);

/// Extend source spill evidence through current direct calls. Identity copies
/// and PHIs retain byte positions; narrow views may read the retained prefix.
/// Opaque implicit readers, changed call bindings and other transports reject
/// a suffix proof. The shared per-function budget also bounds repeated queries.
SourceRegisterParameterReads sourceRegisterParameterReads(
    const MedFunc &Func, const SourceCalleeParameterReads &Callees,
    size_t &Remaining, const SourceCalleeParameterOverwrites &Overwrites = {});

/// Choose zero for unobserved argument bytes only when the complete retained
/// prefix is an exact scalar constant. Follow COPY/CONCAT/SUBBYTES/ZEXT views
/// without evaluating an unavailable neighbor or removing producer effects.
/// Parameter positions, widths, native operands and signatures stay intact.
void replaceKnownSourceCallArguments(MedFunc &Func,
                                     const SourceCalleeParameterReads &Callees);

/// After call argument recovery, omit private stores only when a complete
/// noncapturing use proof of the frame finds no reads of their surviving
/// bytes on any path. Explicit partial overwrites kill only their bytes. Direct
/// callees contribute their actual bounded pointer reads, including negative
/// displacements. Unknown callees, escapes and unsupported uses keep stores.
void markUnobservedBorrowedFrameStores(
    MedFunc &Func, Arch Architecture, BinaryFormat Format,
    const std::map<va_t, const MedFunc *> &Callees);

/// After call argument recovery, omit each store of a scratch register's
/// undefined incoming value (holdsUndefinedIncomingScratch), as an alignment
/// `push rax` makes one, into the function's own frame below the entry stack
/// pointer, where no load or stack variable of the function reads those
/// bytes: they held no defined value before the store and hold none after
/// it.  A store of the convention's variadic count register (System V's
/// RAX), which carries nothing into a function that is not variadic, goes
/// also where a call receives an address in its slot before anything may
/// read the bytes, as for a local a routine fills through its address
/// (`int x; get(&x, a); return x;`): a read then sees what the routine wrote
/// or bytes the program never defined, as the local's would.  Any other
/// store whose bytes the function reads back stays, so that the read fails
/// where it uses the value.  A variadic function's register save area and a
/// function an exception dispatcher enters keep theirs.
void markUndefinedScratchStores(MedFunc &Func, Arch Architecture,
                                BinaryFormat Format);

/// In source mode, mark each store that saves a callee-saved register's entry
/// value into prologue storage as a PrologueSave.  Prologue storage is the
/// function's own frame below the entry stack pointer, or on Win64 the home
/// slots the caller hands it.  A save stays unmarked when anything but an
/// unused restore of the same register reads its bytes, or when another store
/// writes them.  Operations keep their positions: call records index them.
void markCalleeSavePrologueSaves(MedFunc &Func, Arch Architecture,
                                 BinaryFormat Format,
                                 StackEntryKind Entry = StackEntryKind::Call);

/// In source mode, mark each pointer-width load of the slot at the entry stack
/// pointer as a ReturnAddressRead when the call pushed its return address
/// there and no store in \p Func writes that slot: the value is the function's
/// own return address, which C spells `__builtin_return_address(0)`.
void markReturnAddressReads(MedFunc &Func, Arch Architecture,
                            BinaryFormat Format,
                            StackEntryKind Entry = StackEntryKind::Call);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDPROLOGUESAVES_H
