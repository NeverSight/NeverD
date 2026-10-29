//===- SEHLocalUnwindProtocolDetail.h - Local unwind conventions -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Each architecture's local unwind convention, which
/// sehLocalUnwindConvention dispatches to.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_SEHLOCALUNWINDPROTOCOLDETAIL_H
#define NEVERD_IR_LOW_SEHLOCALUNWINDPROTOCOLDETAIL_H

#include "neverd/ir/low/SEHLocalUnwindProtocol.h"

namespace neverd::seh_local_unwind {

/// Windows x64 (X86/SEHLocalUnwindX64.cpp).
llvm::Expected<SEHLocalUnwindConvention> x64Convention(const LowFunc &Function,
                                                       const TargetRegInfo &TRI,
                                                       size_t &WorkRemaining);

} // namespace neverd::seh_local_unwind

#endif // NEVERD_IR_LOW_SEHLOCALUNWINDPROTOCOLDETAIL_H
