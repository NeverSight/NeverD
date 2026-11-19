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
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/LanguageEHGo.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
#include <vector>

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

/// Whether the function at \p Entry takes its arguments in Go's internal
/// register ABI: Go code the toolchain compiled rather than assembled, in an
/// x86-64 image whose pclntab records which is which
/// (GoModuleInfo::RegisterArguments).
bool usesGoRegisterArguments(const BinaryImage &Img, va_t Entry);

/// The integer argument registers of Go's internal ABI on \p A, in order
/// (abi-internal.md: RAX, RBX, RCX, RDI, RSI, R8, R9, R10, R11), or none.
llvm::ArrayRef<uint64_t> goIntegerArgumentRegisters(Arch A);

/// One register argument a function's stack-growth path spills.
struct GoArgumentSpill {
  uint64_t Register = 0;
  uint16_t Size = 0;
  /// The spill slot, as an offset from the stack pointer at entry.
  int64_t EntryOffset = 0;
};

/// A Go function's stack-growth path: the entry compares the stack pointer
/// with the goroutine's stack guard and branches to a block that spills the
/// register arguments, calls the runtime's morestack, reloads them and starts
/// the function again.
struct GoStackGrowth {
  /// The register arguments the path spills and reloads, sorted by slot.
  /// The compiler spills exactly the register arguments the function uses,
  /// each to its slot in the spill area above the return address
  /// (cmd/compile/internal/amd64/ssa.go, RegArgs).
  std::vector<GoArgumentSpill> Spills;
  /// Where the growth block starts.
  va_t GrowthStart = 0;
  /// The morestack it calls grows the stack and restarts the function, so
  /// the path changes nothing the function computes; `runtime.morestackc`
  /// throws instead (GoModuleInfo::ThrowingMorestackEntries).
  bool Restarts = false;
};

/// The stack-growth path of a function whose entry is block 0 of \p Blocks,
/// or nullopt where the entry has no such path, or one of another shape.
std::optional<GoStackGrowth> goStackGrowth(const BinaryImage &Img,
                                           llvm::ArrayRef<LowBlock> Blocks);

/// The register arguments the stack-growth path spills
/// (GoStackGrowth::Spills).
std::optional<std::vector<GoArgumentSpill>>
goMorestackSpills(const BinaryImage &Img, llvm::ArrayRef<LowBlock> Blocks);

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
