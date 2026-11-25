//===- MedCallingConvDetail.h - Calling convention queries -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDCALLINGCONVDETAIL_H
#define NEVERD_IR_MED_MEDCALLINGCONVDETAIL_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {
struct BinaryImage;
struct LowFunc;
} // namespace neverd

namespace neverd::med_calling_conv_detail {

using ValueKey = std::tuple<MedVar::VarKind, int, int>;
using ValueSet = llvm::DenseSet<ValueKey>;

/// An authenticated byte read, including instruction-owned FP memory sources.
/// Scalar FP sources can use a proven immutable incoming argument. Packed
/// sources retain their instruction-owned memory/alignment effects and homes.
struct StackMemoryRead {
  unsigned AddressInput;
  unsigned Bytes;
  bool ScalarFP = false;
};
std::optional<StackMemoryRead> stackMemoryRead(const MedOp &Op);
/// Keep every home reached by a write or by an overlapping mutable read.
/// Instruction-owned packed reads and unaligned slot-crossing loads retain
/// physical homes; aligned immutable loads compose their slot parameters.
void preserveStackReadHomes(
    const MedFunc &Func, int64_t Base, unsigned Slot,
    llvm::function_ref<std::optional<int64_t>(const MedVar &)> Trace,
    std::set<int64_t> &MutableSlots);
/// Keep incoming reads reached by physical CALL writes as memory operations.
/// The shared CALL-slot proof owns their byte history; incomplete proofs keep
/// every candidate home mutable until source-frame validation can decide.
void preserveCallStackHomes(
    const MedFunc &Func, Arch Architecture, BinaryFormat Format,
    const BinaryImage *Image, int64_t Base, unsigned Slot,
    llvm::function_ref<std::optional<int64_t>(const MedVar &)> Trace,
    std::set<int64_t> &MutableSlots);
void recoverFPStackReads(
    MedFunc &Func, Arch Architecture, int64_t Base, unsigned Slot,
    unsigned ParameterBase,
    llvm::function_ref<std::optional<int64_t>(const MedVar &)> Trace,
    const std::set<int64_t> &MutableSlots);

/// One above every value id in \p Func. MedIR numbers registers, temporaries
/// and stack values from one counter, and HighIR keys them by {Id, SSAVer}
/// alone: a new temporary takes an id above every value's, or it aliases a
/// register's (`t30` read as EAX.30).
int nextValueId(const MedFunc &Func);

/// Rewrite \p Read, a read of \p Bytes from the first byte of the stack
/// parameter \p First, as the composition of the parameters it covers, each
/// one \p Slot bytes and above the ones below it.  The last gives only the
/// bytes the read reaches; the rest of it is padding, as past a ten-byte x87
/// long double.  The temporaries to compute before \p Read are appended to
/// \p Prefix, numbered from \p NextValueId.
void composeStackSlots(MedOp &Read, MedVar First, unsigned Slot, unsigned Bytes,
                       int &NextValueId, std::vector<MedOp> &Prefix);

ValueKey valueKey(const MedVar &V);
/// Internal register-ABI stack recovery, including a bounded variadic prefix.
/// A validated MIPS record ABI supplies the bounds of address-taken objects.
void detectStackParams(MedFunc &Func, Arch TargetArch, BinaryFormat Format,
                       int64_t MaxStackOff = 0,
                       const BinaryImage *Image = nullptr,
                       const SourceFunctionTypeHint *RecordABI = nullptr);
void detectCdeclStackParams(MedFunc &Func, Arch TargetArch, BinaryFormat Format,
                            const BinaryImage *Image = nullptr);
bool containsValue(const ValueSet &Values, const MedVar &V);

ValueSet computeForwardValueClosure(
    const MedFunc &Func, llvm::ArrayRef<MedVar> Seeds,
    llvm::function_ref<bool(const MedOp &, unsigned)> ForwardsInput);

uint16_t findFirstUseSize(const MedFunc &Func, uint64_t ParamRegOff,
                          const TargetRegInfo &TRI);

/// True for an entry-block self-copy of \p RegOff, which declares the
/// register's incoming value.
bool isEntryLiveInCopy(const MedOp &Op, uint64_t RegOff);

/// True for an entry live-in copy whose output is a new version, so the
/// incoming value it reads is a separate SSA value with uses of its own.
bool isRenamedEntryLiveInCopy(const MedOp &Op, uint64_t RegOff);

/// An i386 parameter register whose live-in value only feeds scratch idioms
/// (MedCallingConvX86.cpp).
bool liveInOnlyFeedsScratch(const MedFunc &Func, uint64_t ParamRegOff);

/// liveInOnlyFeedsScratch where a return reads no register, as one does
/// under Go's stack ABI: `movb (si), al` keeps RAX's upper bytes, which only
/// memmove's return reads.
bool liveInOnlyFeedsScratchOrReturns(const MedFunc &Func, uint64_t ParamRegOff);

/// Whether \p RegOff's incoming value reaches a use other than being pushed
/// or returned (MedCallingConvX86.cpp): a push that only reserves a stack
/// slot stores the register without reading it as a value, and a return
/// reads the return register whatever the function returns.
bool liveInReachesNonPushUse(const MedFunc &Func, uint64_t RegOff);

/// Whether \p RegOff's incoming value reaches an operation whose effect or
/// result the function goes on to use, other than a push or a return
/// (MedCallingConvX86.cpp): Go's nil check `testb al, (reg)` reads AL only to
/// set flags nothing reads.
bool liveInReachesLiveUse(const MedFunc &Func, uint64_t RegOff);

/// The argument registers among \p Regs whose incoming value \p Func only
/// stores in its own frame, where no load that reads anything takes a byte
/// of it, or, saved once in the entry block, none takes a byte no other
/// store overwrote on every way to it (MedSavedArgumentRegisters.cpp):
/// MSVC ARM's alignment `push {r3, ...}`, popped back unread, the words its
/// `push {r2, r3, ...}` reserves for locals, and /Od homes no code reads
/// back.  Removes the stores that save them, whose bytes nothing sees.
/// None while a frame address does anything but address the function's own
/// loads and stores.  A slot a filter or termination handler of \p Image,
/// among the lifted \p Bodies or else decoded, reads through the frame
/// registers the unwinder restores, where it may still hold the register,
/// is read; where such a handler may reach the frame some other way, or
/// \p Image is null, or the function runs C++'s catch and unwind funclets,
/// none is only saved.  ARM only.
std::set<uint64_t> dropOnlySavedArgumentRegisters(
    MedFunc &Func, Arch TargetArch, BinaryFormat Format,
    llvm::ArrayRef<uint64_t> Regs, const BinaryImage *Image,
    const std::map<va_t, const LowFunc *> *Bodies);

/// Remove the store of a prologue's `push ecx` that only makes room for a local
/// the function writes before it reads, and the register's live-in if nothing
/// else reads it: it is no parameter.  i386 only.
void dropReservingPushes(MedFunc &Func);

/// Additional i386 integer inputs read through EBX, ESI or EDI. A saved
/// register alone supplies no evidence: its incoming bits must reach a value
/// use, rather than only storage, restoration or discarded scratch lanes.
std::vector<uint64_t> additionalI386ParameterRegisters(const MedFunc &Func);

/// The candidate carriers shared by i386 parameter detection and retention of
/// call setup before the callee's recovered signature is available.
llvm::ArrayRef<uint64_t> i386NonstandardArgumentRegisters();

} // namespace neverd::med_calling_conv_detail

#endif // NEVERD_IR_MED_MEDCALLINGCONVDETAIL_H
