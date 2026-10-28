//===- RegistrationScalarPackets.h - Private scalar barriers -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Share nontrapping integer-operation policy for private local receipts.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONSCALARPACKETS_H
#define NEVERD_IR_LOW_X86_REGISTRATIONSCALARPACKETS_H

namespace neverd {
struct LowOp;
namespace registration_abi {

/// Memory, FP, divide, calls and control transfers require separate evidence.
bool isNontrappingFrameScalarOperation(const LowOp &Op);

} // namespace registration_abi
} // namespace neverd

#endif // NEVERD_IR_LOW_X86_REGISTRATIONSCALARPACKETS_H
