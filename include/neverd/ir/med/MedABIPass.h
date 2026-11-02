//===- MedABIPass.h - ABI analysis pass for MedIR -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the ABI recovery pass that maps register operands to
/// argument indices and recovers per-call-site argument lists.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDABIPASS_H
#define NEVERD_IR_MED_MEDABIPASS_H

#include "neverd/ir/FloatingCallArguments.h"
#include "neverd/ir/IntegerCallArguments.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/DataSymbolBinding.h"

#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

int regToArgIdx(uint64_t RegOff, Arch TheArch);

/// Validate the complete integer operand sequence retained from before SSA.
bool hasBoundIntegerCallArguments(const MedOp &Op);

/// A complete source-ordered scalar call bound before SSA. Recheck its stored
/// placement and the surviving operands/result; HighC does not need a second
/// argument-recovery pass for conventions with callee summaries.
std::optional<FloatingCallArguments>
boundFloatingCallArguments(const MedOp &Op, Arch A, BinaryFormat F);

/// A complete fixed scalar FP call established by ABI recovery. Bound mixed
/// calls additionally require agreement with the recovered operand list.
std::optional<FloatingCallArguments>
recoveredFloatingCallArguments(const MedOp &Op, const MedCallInfo &Call, Arch A,
                               BinaryFormat F);

/// Entries whose leaf bodies only write below their own entry stack pointer.
/// Compute before ABI recovery mutates the functions. Unknown effects fail
/// closed.
std::set<va_t> findFrameLocalLeafCallees(const std::vector<MedFunc> &Funcs,
                                         Arch TheArch);

// \p CalleeRegArity maps a callee entry address to its integer
// register-argument count (recovered from that callee's own parameters).  It
// lets a direct call bound how many incoming registers a *forwarder*
// (`f(a){return g(a);}`) passes through, so a register the forwarder never
// reads is still recovered as the argument the callee takes — and only up to
// that arity, never beyond it.
// \p CalleeTotalArity maps a callee entry to its full integer-argument count
// (register + stack).  It bounds how many *stack* arguments a tail-call
// forwarder passes straight through, which leave no store the scan can see.
// \p CalleeFPArity maps a callee entry to its floating-point/vector argument
// count (XMM0-7 / V0-7), a register class with an index independent of the
// integer arguments; it bounds how many FP arguments a direct call recovers.
// \p CalleeFPRegs maps a callee entry to the exact FP-argument register offsets
// in ABI order; ARM `float` args land in the single-width S registers
// (s0,s1,..) while `double` args land in the D registers (d0,d1,..), so the
// caller must recover each FP argument at the register the callee truly reads.
// \p CalleeFPReturnSize maps scalar-FP-returning callees to their logical
// result width (4 for float, 8 for double); absent/zero entries are not scalar
// FP. Such a call defines the FP return register, so its result must flow to
// the post-call reads of that register.  ARM S0 and D0 share a register offset,
// so the width is part of the authoritative call-clobber identity.
// \p CalleeFPScalarBytes records the scalar lane consumed in each FP argument
// register. Forwarded arguments retain these widths independently of returns.
// \p CalleeHasSret marks callees that take a hidden indirect-result (sret)
// pointer in the dedicated indirect-result register (AArch64 x8); such a call
// must pass the result-buffer pointer set up before it as a trailing argument.
// \p CalleeIsVariadic marks callees that are variadic.  On Mach-O AArch64 a
// direct call to such a callee passes every variadic argument on the stack
// right after the fixed register prefix, so its outgoing stack stores are
// recovered as arguments (CalleeRegArgs + k) -- the same Darwin treatment
// applied to the libc printf/scanf family -- instead of being mapped past the 8
// register slots and dropped at the first gap.
// \p FrameLocalLeafCallees is an immutable summary from
// findFrameLocalLeafCallees for these same bodies, before ABI mutation. It lets
// Darwin indirect-target recovery preserve a caller-frame spill across a call
// only when the caller slot and the callee writes are provably disjoint.
// \p CalleeIntRegs gives the integer argument registers, in order, of each
// direct callee that takes them in another order than the convention's
// (MedFunc::IntegerArgumentRegisters); a call to it passes them so.
// \p VectorArgumentCallees marks the variadic callees that take every
// argument register (MedFunc::takesVectorArgumentRegisters); a call to one
// passes each register, zero where it sets none, before its stack arguments.
// \p PointerCallees gives the routine each function-pointer variable starts
// at (collectPointerVariableCallees): a call through the variable passes that
// routine's arguments, though it never takes its name.
// \p PrototypeArgumentCallees holds the imports, by their stubs and slots,
// whose integer arguments a user declaration or header record layout gives
// (PipelineResult::CallPrototypeArguments): a
// call to one passes what its summary counts, which the prototype gives,
// however many arguments the arity tables give a routine of its name.
// \p SummaryKeysRead, where given, gains every key at which the recovery
// reads a callee summary, a user prototype or a name: \p Func's entry, the
// only key at which it writes summaries, first.  A recovery of the same body
// of the same image, with the same function-pointer variables and frame-local
// leaf callees, whose summaries, user prototypes and names read the same at
// those keys, recovers the same.
// LowToMed runs before whole-program return types are known.  It can
// therefore leave a scalar-FP call modeled as defining the integer return
// register: when the result is read only by the next call's implicit XMM0/V0
// argument, or only returned.  Once the callee is known to return \p Size
// bytes through the vector register, promote the authoritative FP-return
// clobber definition of the call at \p OpIdx of \p Blk into its output. If the
// FP return bank has no references at all, introduce its first SSA definition.
// Existing SSA reads already name that definition. A call already routed by
// modelCallFPReturn gains its scalar width without changing the output. One
// with an aggregate return keeps its output; the
// exact synthetic EDX:EAX/R1:R0 split is the one false aggregate this
// scalar-FP certificate replaces.  Returns whether the call was rebound.
bool promoteFloatCallResult(MedFunc &Func, MedBlock &Blk, size_t OpIdx,
                            uint16_t Size, Arch TheArch);

/// The relocations of an image that name a symbol, by the address each
/// relocates, read from the image on the first query, so a module recovery
/// that names no call by its relocation reads none.  For one thread.
class RelocationSymbolIndex {
public:
  explicit RelocationSymbolIndex(const BinaryImage &Img) : Img(Img) {}
  /// The first relocation, in the image's order, at \p Address that names
  /// a symbol; null for none.
  const RelocationEntry *at(va_t Address) const;

private:
  const BinaryImage &Img;
  /// Each such relocation's address and position, in order.
  mutable std::optional<std::vector<std::pair<va_t, size_t>>> Named;
};

void recoverCallAbi(
    MedFunc &Func, Arch TheArch, const std::map<va_t, std::string> &FuncNames,
    const BinaryImage *Img = nullptr,
    std::map<va_t, int> *CalleeRegArity = nullptr,
    std::map<va_t, int> *CalleeTotalArity = nullptr,
    const std::map<va_t, int> *CalleeFPArity = nullptr,
    const std::map<va_t, uint16_t> *CalleeFPReturnSize = nullptr,
    const std::map<va_t, std::vector<uint64_t>> *CalleeFPRegs = nullptr,
    const std::map<va_t, bool> *CalleeHasSret = nullptr,
    std::map<va_t, bool> *CalleeIsVariadic = nullptr,
    const std::map<va_t, bool> *CalleeConsumesVaList = nullptr,
    const std::set<va_t> *FrameLocalLeafCallees = nullptr,
    const std::map<va_t, std::vector<uint64_t>> *CalleeIntRegs = nullptr,
    const std::set<va_t> *VectorArgumentCallees = nullptr,
    const std::map<va_t, PointerVariableCallee> *PointerCallees = nullptr,
    const std::set<va_t> *PrototypeArgumentCallees = nullptr,
    const RelocationSymbolIndex *RelocationSymbols = nullptr,
    std::vector<va_t> *SummaryKeysRead = nullptr,
    const std::map<va_t, std::map<uint64_t, uint16_t>> *CalleeFPScalarBytes =
        nullptr);

/// The integer argument layout \p Func takes its register arguments in: its
/// own register order (MedFunc::IntegerArgumentRegisters) where it has one,
/// else its convention's.  The layout refers to \p Func's registers.
IntegerArgumentLayout integerArgumentLayoutOf(const MedFunc &Func,
                                              const TargetRegInfo &TRI);

/// The value an argument-register-defining op at index \p J contributes to its
/// slot, resolving a low-half sub-register sync to the paired full-width write
/// so a wide (pointer/struct) argument keeps its high bits.
MedVar argRegSourceValueInBlock(const MedBlock &Blk, int J,
                                const TargetRegInfo &TRI,
                                const IntegerArgumentLayout &Layout);

/// The index in \p Blk of the whole argument-register write that the
/// synthetic low-register sync at index \p J mirrors, or -1 where the op at
/// \p J is no such sync (argRegSourceValueInBlock).
int wholeArgRegWriteInBlock(const MedBlock &Blk, int J,
                            const TargetRegInfo &TRI,
                            const IntegerArgumentLayout &Layout);

// Finalize the overflow stack parameters of every variadic callee once all call
// sites have been recovered.  A variadic function's va_arg overflow reads land
// above the synthetic frame, so they cannot be rewritten to ordinary parameter
// reads; instead each callee gains one trailing stack parameter per overflow
// argument (sized from its widest call site), every call to it is padded to
// that arity, and MedFunc::VariadicOverflowCount drives the emitter to spill
// those parameters into the frame headroom where the unchanged walk reads them.
void finalizeVariadicCallees(std::vector<MedFunc> &Funcs, Arch TheArch,
                             BinaryFormat Fmt);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDABIPASS_H
