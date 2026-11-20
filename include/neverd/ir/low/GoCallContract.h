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
#include "neverd/ir/IntegerCallArguments.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"
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

/// Whether Go assembly at \p Entry takes its arguments in Go's registers,
/// in Go's order, as `memmove(to, from, n)` takes them in RAX, RBX and RCX.
/// The image's compiled code must (usesGoRegisterArguments), and the call
/// summary \p Reads must show the entry reading, of the general-purpose
/// registers, the first one or more of Go's integer argument registers and
/// otherwise only registers with a fixed meaning: the stack and frame
/// pointers, the closure context RDX and the goroutine register R14.  ABI0
/// assembly, which Go code reaches through a wrapper, reads its arguments
/// from the stack, and assembly entered another way, such as `rt0_go` with
/// the process's RDI and RSI, reads no such run.
bool goAssemblyTakesRegisterArguments(const BinaryImage &Img, va_t Entry,
                                      const GPRReadWidths &Reads);

/// The integer argument registers of Go's internal ABI on \p A, in order
/// (abi-internal.md: RAX, RBX, RCX, RDI, RSI, R8, R9, R10, R11), or none.
llvm::ArrayRef<uint64_t> goIntegerArgumentRegisters(Arch A);

/// The register Go passes a func value its closure in on \p A: RDX, or R26 on
/// arm64.  It is no positional argument.
std::optional<uint64_t> goClosureContextRegister(Arch A);

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

/// One operand of a Go bounds failure: a register's offset, or a constant.
struct GoBoundsOperand {
  bool Register = false;
  uint64_t Value = 0;
};

/// The bounds failure a call to Go 1.26's `runtime.panicBounds` reports:
/// abi.BoundsErrorCode, whether the index is signed, and the index and the
/// length or capacity it failed against (internal/abi/bounds.go).
struct GoBoundsFailure {
  uint8_t Code = 0;
  bool Signed = false;
  GoBoundsOperand Index, Bound;
};

/// The bounds failure the call at \p CallAddr to \p Callee reports, where
/// \p Callee is `runtime.panicBounds` and the call's PCDATA_PanicBounds value
/// decodes to registers the routine saves (`BoundsDecode`): x86-64's by their
/// hardware number, AArch64's R0-R15.  nullopt otherwise.
std::optional<GoBoundsFailure> goBoundsFailureAt(const BinaryImage &Img,
                                                 va_t CallAddr, va_t Callee);

/// Whether a call to \p Callee resumes at its return address although the
/// callee's own code never returns: Go's stack-growth routines, whose
/// `newstack` resumes the goroutine at the call's return address once the
/// stack has grown (src/runtime/stack.go).  `runtime.morestackc` throws.
bool goCallResumesAfter(const BinaryImage &Img, va_t Callee);

/// Whether the Go function at \p Entry takes every argument and returns every
/// result on the stack, as Go code did before its internal register ABI
/// (GoModuleInfo::StackArguments).
bool goTakesStackArguments(const BinaryImage &Img, va_t Entry);

/// Whether Go code before its internal ABI reads \p Register's incoming value
/// without taking an argument in it: the stack and frame pointers, the link
/// register, the closure context, and arm64's goroutine register R28.  Code
/// that reads any other register at its entry, as a register stub or a
/// trampoline the platform's code calls does, takes arguments there.
bool goStackABIEntryRegister(const TargetRegInfo &TRI, uint64_t Register);

/// The layout of a Go call before Go's internal ABI, on \p A: no register,
/// and words from the call's stack pointer (the word above it on arm64, whose
/// lowest one the callee saves its link register in), which the callee finds
/// past its return address.
IntegerArgumentLayout goStackArgumentLayout(Arch A);

/// The bytes of arguments and results the Go function at \p Entry takes on
/// its caller's frame, as its `pclntab` record says (GoFrameArguments), if
/// it says.
std::optional<uint32_t> goFrameArgumentBytes(const BinaryImage &Img,
                                             va_t Entry);

/// Whether the `pclntab` records a function starting at \p Entry, whether
/// or not its record gives a size (GoModuleInfo::FrameArguments): a block
/// NeverD splits off a function's interior is no such function.
bool goRecordsFunction(const BinaryImage &Img, va_t Entry);

/// Whether \p Callee drops its return address and returns to its caller's
/// caller (GoModuleInfo::ReturnsPastCallerEntries): an x86-64 call to it is
/// its caller's last act, a tail call, and never reaches the next instruction.
bool goCallReturnsPastCaller(const BinaryImage &Img, va_t Callee);

/// The registers the Go stub at \p Entry takes its arguments in, in the
/// order of the argument words it stores them into before it enters a
/// stack-ABI routine with them: Go 1.16's bounds stubs, as `panicIndex`'s
/// `mov [rsp+8], rax; mov [rsp+16], rcx; jmp goPanicIndex` takes AX and CX,
/// which no platform position names.  \p Blocks, the stub's body, is one
/// block that stores the incoming value of a general-purpose register into
/// each word its record gives (goFrameArgumentBytes), writes no register,
/// and ends in a tail call to a stack-ABI routine (goTakesStackArguments).
/// None for any other body.  Its callers pass exactly these registers, bound
/// before SSA as a prototype's operands are (FixedIntegerCallArguments).
std::optional<FixedIntegerCallArguments>
goRegisterStubArguments(const BinaryImage &Img, va_t Entry,
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
