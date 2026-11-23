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
#include <map>
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
/// \p A: R11 or X25 for GoWriteBarrierKind::Buffer and BufferBody, none for
/// Store.
std::optional<uint64_t> goWriteBarrierResult(Arch A, GoWriteBarrierKind Kind);

/// Whether a write barrier of \p Kind keeps the register at \p Register on
/// \p A for its caller: every general-purpose register but the stack pointer,
/// the one it returns buffer space in (goWriteBarrierResult) and arm64's
/// assembler temporary R27 (GoWriteBarrierKind).
bool goWriteBarrierKeeps(Arch A, GoWriteBarrierKind Kind, uint64_t Register);

/// The operands of the write barrier at \p Entry where it performs the store
/// itself (GoWriteBarrierKind::Store), in its own order: the address it writes
/// at and the pointer it writes there (GoWriteBarrier::Destination, Value).
/// They follow neither the platform's argument order nor Go's.  The shared
/// body of the buffer barriers (BufferBody) takes one, the bytes of buffer
/// space wanted, in the register it returns that space in
/// (goWriteBarrierResult).  None for a Buffer barrier, which takes no
/// argument, or for any other routine.
std::optional<FixedIntegerCallArguments>
goWriteBarrierArguments(const BinaryImage &Img, va_t Entry);

/// Whether the indirect call at \p Call of \p Blocks[\p Block], in arm64
/// assembly at \p Caller of a register-ABI image, reaches code the Go
/// toolchain did not build.  Assembly calls Go code by name or through a
/// func value, whose
/// code pointer is the word the closure context register points at when the
/// call is made (`MOVD R3, R26; MOVD 0(R3), R3; BL (R3)`).  A call through
/// any other pointer it loads is a call into the platform's code: a vDSO
/// routine whose address a variable holds (`runtime.nanotime1`), a C
/// function a cgo call passes (`runtime.asmcgocall`).  The target is
/// followed to its load in the call's block or, with no call between, in
/// the blocks that alone lead to it (kGoIndirectCallTargetBlocks); a target
/// no such load defines says nothing, and the call stays Go's.
bool goAssemblyCallsPlatformCode(const BinaryImage &Img, va_t Caller,
                                 llvm::ArrayRef<LowBlock> Blocks, size_t Block,
                                 size_t Call);

/// The contract of a call the code at \p Caller makes to \p Callee, which is
/// InvalidVA where the call names no fixed target.  Go code's call to a
/// routine the `pclntab` does not record, as a libc routine's stub, follows
/// the platform's.
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

/// Go's floating argument and result registers on \p A, in order: X0-X14 on
/// amd64 (X15 holds zero), F0-F15 (V0-V15) on arm64.  Empty elsewhere.
llvm::ArrayRef<uint64_t> goFloatArgumentRegisters(Arch A);

/// The registers x86-64 Go assembly at \p Entry takes its arguments in where
/// neither of Go's ABIs nor the platform's convention passes them: assembly
/// that declares no argument word (goFrameArgumentBytes) and takes no Go
/// registers (goAssemblyTakesRegisterArguments), whose summary \p Reads
/// shows it reading at entry a general-purpose register the platform passes
/// no argument in.  `indexbytebody<>` takes the data in SI, its length in BX,
/// the byte it seeks in AL and the address of its result in R8
/// (src/internal/bytealg/indexbyte_amd64.s), and its callers set them before
/// they enter it.  Each register it reads, but the stack and frame pointers,
/// in the platform's argument order, then Go's, then the others' in theirs.
/// None for any other routine: assembly the platform's code enters, as the
/// kernel enters `runtime.sigtramp`, reads its arguments where the platform
/// passes them.  Its callers bind them before SSA as a prototype's operands
/// are (FixedIntegerCallArguments).
std::optional<FixedIntegerCallArguments>
goEntryRegisterOperands(const BinaryImage &Img, va_t Entry,
                        const GPRReadWidths &Reads);

/// Whether a call the code at \p Caller makes to \p Callee (InvalidVA for an
/// indirect call) enters Go code by Go's internal ABI, which returns its
/// results in Go's registers (goResultRegister): a direct call to compiled Go
/// code, or to assembly that takes Go's argument registers (\p CalleeReads,
/// goAssemblyTakesRegisterArguments), and an indirect call compiled Go code
/// makes, through a function value.  Go assembly also calls C through a
/// pointer (darwin's `runtime.syscall6` calls libc), which returns in the
/// platform's registers; an image without the assembly flag tells neither.
bool goCallEntersInternalABI(const BinaryImage &Img, va_t Caller, va_t Callee,
                             const GPRReadWidths *CalleeReads);

/// Whether a result of a call under Go's internal ABI on \p A can be in the
/// register at \p RegOff: an integer argument register, which results take in
/// the same order, or X0-X14 (arm64 V0-V15).  RDX, the closure context, is
/// none, nor is X15, which the ABI keeps zero.
bool goResultRegister(Arch A, uint64_t RegOff);

/// The register Go passes a func value its closure in on \p A: RDX, or R26 on
/// arm64.  It is no positional argument.
std::optional<uint64_t> goClosureContextRegister(Arch A);

/// The register Go's internal ABI keeps the current goroutine in on \p A
/// (abi-internal.md: "R14 | Current goroutine | Same | Same"), and R28 on
/// arm64, where Go's assembly calls it `g` too.
std::optional<uint64_t> goRoutineRegister(Arch A);

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
  /// Where the entry's second check starts, for a frame too large for one
  /// comparison: the entry proves the stack pointer less the frame does not
  /// wrap (`sub r12, N; jb`), and the block after it compares that with the
  /// guard (`cmp r12, [r14+16]; jbe`), each branching to the growth block.
  /// InvalidVA where the entry checks once.
  va_t SecondCheck = InvalidVA;
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
/// stack has grown (src/runtime/stack.go), and `runtime.mcall`, whose caller
/// the scheduler resumes there when it runs the goroutine again
/// (GoModuleInfo::ScheduleCallEntries).  `runtime.morestackc` throws.
bool goCallResumesAfter(const BinaryImage &Img, va_t Callee);

/// What is stated of each function's calls in \p Img, for the proofs that
/// find the calls that never return: what the user states
/// (BinaryImage::UserNoReturn), and that a call of a routine above returns
/// (goCallResumesAfter), which no proof of its body may overturn.
std::map<va_t, bool> goStatedCallReturns(const BinaryImage &Img);

/// Whether the Go function at \p Entry takes every argument and returns every
/// result on the stack, as Go code did before its internal register ABI
/// (GoModuleInfo::StackArguments), and as Go assembly still does under it
/// (ABI0) unless declared `<ABIInternal>`.  detectCc tells those apart first:
/// they take Go's argument registers (goAssemblyTakesRegisterArguments).
bool goTakesStackArguments(const BinaryImage &Img, va_t Entry);

/// Whether Go code before its internal ABI reads \p Register's incoming value
/// without taking an argument in it: the stack and frame pointers, the link
/// register, the closure context, arm64's goroutine register R28, and every
/// vector register, since floating arguments are on the stack too.  Code
/// that reads any other register at its entry, as a register stub or a
/// trampoline the platform's code calls does, takes arguments there.
bool goStackABIEntryRegister(const TargetRegInfo &TRI, uint64_t Register);

/// The layout of a Go call before Go's internal ABI, on \p A: no register,
/// and words from the call's stack pointer (the word above it on arm64, whose
/// lowest one the callee saves its link register in), which the callee finds
/// past its return address.
IntegerArgumentLayout goStackArgumentLayout(Arch A);

/// The layout of a Go call by Go's internal ABI, on \p A: the integer
/// arguments in Go's registers (goIntegerArgumentRegisters), and the ones Go
/// assigns no register, past the last or too large for those left, in words
/// where the stack ABI puts every argument (goStackArgumentLayout).  Go
/// reserves no home space, on Windows either, and a stack argument follows
/// Go's registers whichever of them the routine takes: none implies a
/// register argument.
IntegerArgumentLayout goRegisterArgumentLayout(Arch A);

/// goRegisterArgumentLayout where \p Registers are the registers Go passes
/// integer arguments in on \p A: a routine that takes its arguments in them
/// (MedFunc::IntegerArgumentRegisters) takes the rest so.
std::optional<IntegerArgumentLayout>
goRegisterArgumentLayout(Arch A, llvm::ArrayRef<uint64_t> Registers);

/// The bytes of arguments and results the Go function at \p Entry takes on
/// its caller's frame, as its `pclntab` record says (GoFrameArguments), if
/// it says.
std::optional<uint32_t> goFrameArgumentBytes(const BinaryImage &Img,
                                             va_t Entry);

/// Whether the `pclntab` records a function starting at \p Entry, whether
/// or not its record gives a size (GoModuleInfo::FrameArguments): a block
/// NeverD splits off a function's interior is no such function.
bool goRecordsFunction(const BinaryImage &Img, va_t Entry);

/// Whether \p Address lies inside a function the `pclntab` records rather
/// than at its start.  The table records every function of the image's Go
/// code once its text base is proven (GoModuleInfo::FrameArguments), so such
/// an address begins none, and a branch to it is no tail call: a jump-table
/// case label (`runtime.suspendG`'s), which reads as a code address.  A call
/// into a routine that runs in its caller's place runs its body there
/// instead (goInlinedRoutineBody).  False where the image records no
/// function.
bool goInsideRecordedFunction(const BinaryImage &Img, va_t Address);

/// Whether \p Address lies in one of the image's Duff's device routines,
/// `runtime.duffzero` or `runtime.duffcopy` (GoModuleInfo::DuffDevices).
bool goDuffDeviceAt(const BinaryImage &Img, va_t Address);

/// Whether \p Address lies in a Go routine that runs in its caller's place
/// (goInlinedRoutineBody): a Duff's device routine (goDuffDeviceAt) or a
/// register argument transfer (GoModuleInfo::RegisterArgumentTransfers).
bool goInlinedRoutineAt(const BinaryImage &Img, va_t Address);

/// The most integer registers the Go function at \p Entry takes its
/// arguments in, in Go's order, where the image passes them in registers:
/// one for each scalar component its traceback lists
/// (GoFrameArguments::ArgumentComponents), since each takes one register at
/// most and the integer ones take the integer registers in order.  None where
/// the image passes arguments otherwise or the table is unknown.
std::optional<unsigned> goRegisterArgumentBound(const BinaryImage &Img,
                                                va_t Entry);

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
