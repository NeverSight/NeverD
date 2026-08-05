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

#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"

#include <map>

namespace neverd {

int regToArgIdx(uint64_t RegOff, Arch TheArch);

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
// \p CalleeReturnsVec marks callees whose result is returned in a vector
// register (x86-64 scalar/vector FP in XMM0); such a call defines the FP return
// register, so its result must flow to the post-call reads of that register.
// \p CalleeHasSret marks callees that take a hidden indirect-result (sret)
// pointer in the dedicated indirect-result register (AArch64 x8); such a call
// must pass the result-buffer pointer set up before it as a trailing argument.
// \p CalleeIsVariadic marks callees that are variadic.  On Mach-O AArch64 a
// direct call to such a callee passes every variadic argument on the stack
// right after the fixed register prefix, so its outgoing stack stores are
// recovered as arguments (CalleeRegArgs + k) -- the same Darwin treatment
// applied to the libc printf/scanf family -- instead of being mapped past the 8
// register slots and dropped at the first gap.
void recoverCallAbi(
    MedFunc &Func, Arch TheArch, const std::map<va_t, std::string> &FuncNames,
    const BinaryImage *Img = nullptr,
    const std::map<va_t, int> *CalleeRegArity = nullptr,
    const std::map<va_t, int> *CalleeTotalArity = nullptr,
    const std::map<va_t, int> *CalleeFPArity = nullptr,
    const std::map<va_t, bool> *CalleeReturnsVec = nullptr,
    const std::map<va_t, std::vector<uint64_t>> *CalleeFPRegs = nullptr,
    const std::map<va_t, bool> *CalleeHasSret = nullptr,
    const std::map<va_t, bool> *CalleeIsVariadic = nullptr);

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
