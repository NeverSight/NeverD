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
#include "neverd/ir/low/LowIR.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace neverd {

struct BinaryImage;

/// Bit I names the x86-64 GPR whose 8-byte slot starts at register offset
/// 8 * I (RAX = bit 0 ... R15 = bit 15).  A write to any byte of a slot
/// (AL, AH, EAX, ...) sets the whole family.
using GPRFamilyMask = uint32_t;

/// The GPR family of a register slice, or nullopt when \p RegOff is not one
/// of the sixteen x86-64 GPRs this summary tracks.
std::optional<unsigned> gprFamilyOf(Arch A, uint64_t RegOff);

/// Direct call targets of \p F (including rewritten tail calls), and whether
/// \p F has an effect this summary cannot describe.
struct LocalRegisterEffect {
  GPRFamilyMask Writes = 0;
  std::set<va_t> Callees;
  bool Unknown = false;
};

LocalRegisterEffect localRegisterEffect(const BinaryImage &Img,
                                        const LowFunc &F);

/// Solve may-write GPR sets over \p Funcs (entry -> local effect).  A callee
/// missing from \p Funcs, or reaching an unknown effect, has no summary.
std::map<va_t, GPRFamilyMask>
solveCallRegisterEffects(const std::map<va_t, LocalRegisterEffect> &Funcs);

} // namespace neverd

#endif // NEVERD_IR_LOW_CALLREGISTEREFFECTS_H
