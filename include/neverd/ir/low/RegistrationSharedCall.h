//===- RegistrationSharedCall.h - Checked C++ helper occurrences -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_REGISTRATIONSHAREDCALL_H
#define NEVERD_IR_LOW_REGISTRATIONSHAREDCALL_H

#include "neverd/loader/COFF/COFFCxxEpilogue.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/COFF/COFFSEH4Entry.h"

#include <cstddef>
#include <set>

namespace neverd {
struct LowFunc;
struct LowBlock;

/// Recheck one rooted, immutable physical C++ entry prefix with the loader's
/// shared instruction protocol. The prefix must be taken from the current
/// graph. This grants frame dataflow only, independently of EH metadata,
/// personality identity, ordinary callee ABI and native registration receipts.
std::optional<coff_loader::X86CxxPrologueCall>
getCheckedX86CxxPhysicalProloguePrefix(const BinaryImage &Image, va_t Entry,
                                       const LowBlock &Prefix,
                                       const std::set<va_t> &Roots,
                                       size_t &Work, size_t Limit);

/// Re-lift the immutable three-instruction SEH4 entry before applying its
/// physical frame effects to the current LowIR.
std::optional<coff_loader::X86SEH4PrologueCall>
getCheckedX86RegistrationSEH4ProloguePrefix(const BinaryImage &Image,
                                            const LowFunc &Source);

/// Bind complete SEH4 source installation/removal receipts to the current
/// immutable helpers, decoded boundaries and exact lifted call operations.
std::optional<coff_loader::X86SEH4PrologueCall>
getCheckedX86RegistrationSEH4PrologueCall(const BinaryImage &Image,
                                          const LowFunc &Source);
std::optional<coff_loader::X86SEH4EpilogueCall>
getCheckedX86RegistrationSEH4EpilogueCall(const BinaryImage &Image,
                                          const LowFunc &Source,
                                          const LowBlock &Block);

/// Recheck the immutable helper and the complete rooted source prefix without
/// requiring the rest of the function's registration-state graph. This permits
/// physical dataflow projection only; it grants no callee ABI or native EH
/// reconstruction receipt.
std::optional<coff_loader::X86CxxPrologueCall>
getCheckedX86RegistrationCxxProloguePrefix(const BinaryImage &Image,
                                           const LowFunc &Source);

/// Bind a complete source frame receipt to its current immutable helper,
/// rooted entry prefix, decoded boundaries and exact lifted operations.
/// These queries describe physical calls, never ordinary callee ABIs or
/// permission to omit memory effects, cookies or native EH checks.
std::optional<coff_loader::X86CxxPrologueCall>
getCheckedX86RegistrationCxxPrologueCall(const BinaryImage &Image,
                                         const LowFunc &Source);

/// Recheck one block-final physical exit without a source chain receipt.
/// This supplies only physical instruction effects. Saved words, return PC,
/// cookies, memory and native registration lifetime require separate proofs.
std::optional<coff_loader::X86CxxEpilogueCall>
getCheckedX86RegistrationCxxEpilogueSuffix(const BinaryImage &Image,
                                           const LowFunc &Source,
                                           const LowBlock &Block);

/// Bind one block-final shared exit to its exact current call and removal
/// receipt. Its entry prologue needs the separate check above. A guarded
/// result still requires preserving its checker and failure path.
std::optional<coff_loader::X86CxxEpilogueCall>
getCheckedX86RegistrationCxxEpilogueCall(const BinaryImage &Image,
                                         const LowFunc &Source,
                                         const LowBlock &Block);

} // namespace neverd

#endif
