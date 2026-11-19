//===- GoCallContract.h - Registers a call from Go code keeps --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The register contract of the calls Go code makes, decided once for every
/// layer that follows a value across a call: MedIR caches it on each call
/// (MedOp::RegisterContract), and the LowIR analyses ask it directly.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_GOCALLCONTRACT_H
#define NEVERD_IR_LOW_GOCALLCONTRACT_H

#include "neverd/Common.h"
#include "neverd/ir/CallRegisterContract.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/LanguageEHGo.h"

#include <cstdint>
#include <optional>

namespace neverd {

/// Whether Go's register contract governs the calls the code at \p Address
/// makes: the Go `pclntab` gives its function a pc-SP table
/// (GoModuleInfo::GoCode), on x86-64 or AArch64, where Go code keeps a frame
/// pointer.  Go code for other targets keeps the platform contract.
bool isGoCode(const BinaryImage &Img, va_t Address);

/// The runtime write barrier routine at \p Entry, if the Go `pclntab` names
/// one there (GoModuleInfo::WriteBarriers).
const GoWriteBarrier *goWriteBarrierAt(const BinaryImage &Img, va_t Entry);

/// The register a write barrier of \p Kind leaves its buffer space in on
/// \p A: R11 or X25 for GoWriteBarrierKind::Buffer, none for Store.
std::optional<uint64_t> goWriteBarrierResult(Arch A, GoWriteBarrierKind Kind);

/// The contract of a call the code at \p Caller makes to \p Callee, which is
/// InvalidVA where the call names no fixed target.
CallRegisterContract callRegisterContractOf(const BinaryImage &Img, va_t Caller,
                                            va_t Callee);

/// The register a call under \p Contract to \p Callee defines by contract:
/// a Buffer write barrier's (goWriteBarrierResult), none otherwise.
std::optional<uint64_t> goCallResultRegister(const BinaryImage &Img,
                                             CallRegisterContract Contract,
                                             va_t Callee);

/// The low bytes of the register view at \p RegOff of \p Size bytes that a
/// call under \p Contract keeps, before what a summary of its callee proves:
/// all of them or none.  \p Result is the register the call defines, if any.
/// Not for CallRegisterContract::Platform, which each consumer asks of the
/// platform convention.
uint16_t goContractKeptBytes(const TargetRegInfo &TRI,
                             CallRegisterContract Contract, uint64_t RegOff,
                             uint16_t Size, std::optional<uint64_t> Result);

} // namespace neverd

#endif // NEVERD_IR_LOW_GOCALLCONTRACT_H
