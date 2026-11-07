//===- VariadicStackArguments.h - Stack slots a call stores -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The stack arguments a call stores.  The pass that sizes a System V variadic
/// callee's overflow parameters (sizeVariadicOverflowFromStores) and the
/// conversion of each call to it read them the same way, so every call passes
/// exactly the values its callee's parameters take.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_VARIADICSTACKARGUMENTS_H
#define NEVERD_IR_HIGH_VARIADICSTACKARGUMENTS_H

#include "neverd/Common.h"
#include "neverd/ir/med/MedIR.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neverd {
struct TargetRegInfo;
} // namespace neverd

namespace neverd::detail {

class HighEntryStackOffsets;

/// The value a stack slot holds: the bytes from \p Byte of \p Value, which a
/// store wider than a slot writes across several.
struct StackSlotValue {
  MedVar Value;
  uint16_t Byte = 0;
};

/// The phi of the register at \p RegOff that \p Block of \p Func begins
/// with.  A view narrower than another phi of the register (`w0` beside
/// `x0`) keeps step with writes to the whole register only for the reads of
/// it that follow; with none left it may be stale, and where a write to the
/// view zero-extends into the register, the wider phi holds the value.  The
/// view's phi stands for the register where it is read and the wider phi
/// joins an unmaterialized incoming value, an unspecified exceptional value,
/// or an incomplete predecessor. This choice is independent of the two
/// PHIs' order.
const PhiNode *registerPhi(const MedFunc &Func, const MedBlock &Block,
                           uint64_t RegOff, const TargetRegInfo &TRI);

/// The register at \p RegOff as it enters \p Block of \p Func: the block's
/// phi of it, else the last definition along the first predecessor path that
/// has one, else at function entry the copy that names its incoming value.
/// With \p ParameterAtEntry, a register the function takes as a parameter
/// and its entry holds no copy of enters as that parameter.
bool reachingRegisterAtBlockEntry(const MedFunc &Func, const MedBlock &Block,
                                  uint64_t RegOff, const TargetRegInfo &TRI,
                                  MedVar &Out, bool ParameterAtEntry = false);

/// The integer argument register at \p RegOff as the call at \p CallIdx of
/// \p Block reads it: the block's last write to it before the call, else as
/// it enters the block (reachingRegisterAtBlockEntry).  A call that may
/// change the register (callClobbersRegister under \p Format) ends a path as
/// a write does, and so does an exception handler's entry, where the
/// unwinder leaves caller-saved registers unspecified.  Where every path
/// that finds nothing else ends so, none reaching the function's entry, the
/// value is what the calls left there (valueLeftByCall).  With
/// \p ParameterAtEntry, a register the function takes as a parameter and its
/// entry holds no copy of (the body only passes it on) enters as that
/// parameter.
bool reachingRegisterAtCall(const MedFunc &Func, const MedBlock &Block,
                            size_t CallIdx, uint64_t RegOff,
                            const TargetRegInfo &TRI, BinaryFormat Format,
                            MedVar &Out, bool ParameterAtEntry = false);

/// The values the call at \p CallIdx in \p Block stores to the pointer-wide
/// slots from the stack pointer it is made with up, in slot order as far as
/// they run unbroken.  A slot holds the latest store at its start since the
/// block's previous call.  One that holds only a scratch register's undefined
/// incoming value under the convention of a \p Format image passes nothing
/// (holdsUndefinedIncomingScratch), as call-argument recovery decides: above
/// the arguments it is left out, and below one it passes zero.
std::vector<StackSlotValue>
storedStackArguments(const MedFunc &Func, const MedBlock &Block, size_t CallIdx,
                     Arch TheArch, BinaryFormat Format,
                     HighEntryStackOffsets &Offsets);

} // namespace neverd::detail

#endif // NEVERD_IR_HIGH_VARIADICSTACKARGUMENTS_H
