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

namespace neverd {
struct LowFunc;
struct LowBlock;

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

/// Bind one block-final shared exit to its exact current call and removal
/// receipt. Its entry prologue needs the separate check above. A guarded
/// result still requires preserving its checker and failure path.
std::optional<coff_loader::X86CxxEpilogueCall>
getCheckedX86RegistrationCxxEpilogueCall(const BinaryImage &Image,
                                         const LowFunc &Source,
                                         const LowBlock &Block);

} // namespace neverd

#endif
