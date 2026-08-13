//===- ExceptionUnwindOp.h - Normalized unwind operations -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The target-independent vocabulary a decoded unwind action is expressed in,
/// plus the epilogue scope that groups a run of them.  Every operation is
/// stated in the saving direction whatever direction the native table spelled
/// it, so a consumer never has to ask which way round a record was written.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_EXCEPTIONUNWINDOP_H
#define NEVERD_LOADER_EXCEPTIONUNWINDOP_H

#include <cstdint>
#include <vector>

namespace neverd {

/// Unwind actions, normalized across targets.
///
/// Every operation is stated in the *saving* direction, as the prologue
/// performs it, even where the native table spells the epilogue instruction
/// instead — ARM32's codes name `pop` and `add sp` where ARM64's name `stp`
/// and `sub sp`.  Both describe the same frame, and a consumer that has to
/// ask which way round a record was written cannot use it.
///
/// New enumerators are appended: the value is serialized into the lifted
/// Windows EH metadata, so reordering would silently reinterpret it.
enum class UnwindOperationKind : uint8_t {
  PushNonVolatile,
  PushTwoRegisters,
  PushConsecutiveRegisters,
  AllocateLarge,
  AllocateHuge,
  AllocateSmall,
  SetFramePointer,
  SaveNonVolatile,
  SaveNonVolatileFar,
  Epilog,
  Spare,
  SaveXMM128,
  SaveXMM128Far,
  PushMachineFrame,
  PushCanonicalFrame,
  Opaque,
  /// `sub sp,sp,#N`.  Covers ARM64's `alloc_s`/`alloc_m`/`alloc_l` and ARM32's
  /// `add sp` codes, which differ only in how wide an immediate they encode.
  AllocateStack,
  /// `alloc_z`: an allocation counted in SVE vector lengths rather than bytes,
  /// so its size is not known until the implementation's width is.
  AllocateVectorLengthStack,
  /// Store one register at a non-negative offset from sp.
  SaveRegister,
  /// Store one register and decrement sp in the same instruction.
  SaveRegisterPreIndexed,
  /// Store a register pair at a non-negative offset from sp.
  SaveRegisterPair,
  /// Store a register pair and decrement sp in the same instruction.
  SaveRegisterPairPreIndexed,
  /// `save_next`: repeat the previous pair save for the next pair up, at the
  /// next slot up.  Kept as its own operation because the pair it names is
  /// only defined relative to the operation before it.
  SaveNextPair,
  /// `add x29,sp,#N`: establish the frame pointer above the stack pointer.
  AddFramePointer,
  /// `mov sp,rX`: the stack pointer was restored from another register, so the
  /// frame's extent is not recoverable from the unwind codes alone.
  SetStackPointerFromRegister,
  /// `ldr lr,[sp],#X`: the return address alone is reloaded and sp advanced.
  LoadReturnAddress,
  /// `pacibsp`: the return address in lr is signed against sp, so a reader of
  /// the saved value must strip the pointer authentication code.
  SignReturnAddress,
  /// One of the `MSFT_OP_*` codes an assembly routine uses to declare that a
  /// trap frame, machine frame, or context record sits on the stack in place
  /// of an ordinary frame.  \ref UnwindOperation::OpInfo carries which.
  CustomStackFrame,
  /// An instruction the unwinder must step over but that changes no state.
  Nop,
  End,
  /// `end_c`: end of the codes for the current chained scope, with the parent
  /// scope's codes continuing after it.
  EndChained,
  /// `add sp,sp,#N`: the frame the prologue leaves is *smaller* than the one
  /// it was entered with.  ARM EHABI is the only target here that can say so,
  /// and it says it often enough that folding it into \ref AllocateStack with
  /// a sign nobody reads would lose the size of the frame.
  DeallocateStack,
};

/// Register file an operation's register operand is numbered in.
enum class UnwindRegisterClass : uint8_t {
  None,
  /// x64 general-purpose, ARM `r0`-`r15`, ARM64 `x0`-`x30`.
  GeneralPurpose,
  /// ARM and ARM64 `d0`-`d31`.
  FloatingPoint,
  /// ARM64 `q0`-`q31`.  Only the Arm64EC entry thunks save these: x64 treats
  /// the full 128-bit register as non-volatile where ARM64 treats only its low
  /// half that way, so a thunk between the two has to preserve the difference.
  Vector,
};

/// One decoded unwind action.  OperandBytes retains the exact native payload
/// when an operation is unknown or cannot yet be represented semantically.
struct UnwindOperation {
  UnwindOperationKind Kind = UnwindOperationKind::Opaque;
  /// Position of the operation in the native array: a slot index on x64,
  /// where the array is an array of 2-byte slots, and a byte offset on ARM,
  /// where it is an array of bytes and an epilogue scope points into it.
  uint32_t CodeOffset = 0;
  uint8_t OpInfo = 0;
  /// Native encoding size, in the same unit as \ref CodeOffset.
  uint8_t SlotCount = 0;
  /// Lowest register the operation acts on, numbered within \ref
  /// RegisterClass.
  uint16_t Register = 0;
  uint64_t StackOffset = 0;
  UnwindRegisterClass RegisterClass = UnwindRegisterClass::None;
  /// Every register the operation acts on, as a bitmask over \ref
  /// RegisterClass's numbering.  Pairs, ranges, and ARM's arbitrary pop masks
  /// are all expanded here, so a consumer never has to re-derive the set from
  /// a base register and a count it would have to know the encoding to read.
  uint32_t RegisterMask = 0;
  /// Size of the machine instruction this operation stands against, for the
  /// targets whose unwind codes map one-to-one onto instructions.  Zero on
  /// x64, where no such mapping exists.
  uint8_t InstructionSize = 0;
  std::vector<uint8_t> OperandBytes;
};

struct UnwindEpilog {
  int64_t StartOffset = 0;
  uint8_t Flags = 0;
  uint32_t FirstOperationOffset = 0;
  uint32_t LastInstructionOffset = 0;
  std::vector<UnwindOperation> Operations;
};

} // namespace neverd

#endif // NEVERD_LOADER_EXCEPTIONUNWINDOP_H
