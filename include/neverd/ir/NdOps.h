//===- NdOps.h - NeverD IR opcode definitions ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the NdOp enumeration covering all opcodes in the NeverD IR,
/// including integer, floating-point, boolean, memory, and control-flow
/// operations.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_NDOPS_H
#define NEVERD_IR_NDOPS_H

#include "neverd/Common.h"

namespace neverd {

#define ND_OP_LIST(X)                                                          \
  X(COPY)                                                                      \
  X(LOAD)                                                                      \
  X(STORE)                                                                     \
  X(ATOMIC_XCHG)                                                               \
  X(ATOMIC_ADD)                                                                \
  X(ATOMIC_CMPXCHG)                                                            \
  X(INT_ADD)                                                                   \
  X(INT_SUB)                                                                   \
  X(INT_AND)                                                                   \
  X(INT_OR)                                                                    \
  X(INT_XOR)                                                                   \
  X(INT_LEFT)                                                                  \
  X(INT_RIGHT)                                                                 \
  X(INT_ASHR)                                                                \
  X(INT_MULT)                                                                  \
  X(INT_DIV)                                                                   \
  X(INT_SDIV)                                                                  \
  X(INT_REM)                                                                   \
  X(INT_SREM)                                                                  \
  X(INT_EQUAL)                                                                 \
  X(INT_NOTEQUAL)                                                              \
  X(INT_LESS)                                                                  \
  X(INT_SLESS)                                                                 \
  X(INT_LESSEQUAL)                                                             \
  X(INT_SLESSEQUAL)                                                            \
  X(INT_ZEXT)                                                                  \
  X(INT_SEXT)                                                                  \
  X(INT_NEGATE)                                                                \
  X(INT_NOT)                                                                   \
  X(INT_CARRY)                                                                 \
  X(INT_SOVF)                                                                \
  X(INT_SBOR)                                                               \
  X(BOOL_AND)                                                                  \
  X(BOOL_OR)                                                                   \
  X(BOOL_XOR)                                                                  \
  X(BOOL_NOT)                                                                  \
  X(FLOAT_ADD)                                                                 \
  X(FLOAT_SUB)                                                                 \
  X(FLOAT_MULT)                                                                \
  X(FLOAT_DIV)                                                                 \
  X(FLOAT_NEG)                                                                 \
  X(FLOAT_ABS)                                                                 \
  X(FLOAT_SQRT)                                                                \
  X(FLOAT_EQUAL)                                                               \
  X(FLOAT_NOTEQUAL)                                                            \
  X(FLOAT_LESS)                                                                \
  X(FLOAT_INT2FLOAT)                                                           \
  X(FLOAT_FLOAT2INT)                                                           \
  X(FLOAT_TRUNC)                                                               \
  X(FLOAT_CEIL)                                                                \
  X(FLOAT_FLOOR)                                                               \
  X(CONCAT)                                                                     \
  X(SUBBYTES)                                                                  \
  X(BRANCH)                                                                    \
  X(COND_BR)                                                                   \
  X(INDIR_BR)                                                                 \
  X(CALL)                                                                      \
  X(INDIR_CALL)                                                                   \
  X(RETURN)                                                                    \
  X(INT_NEG2)                                                                 \
  X(POPCOUNT)                                                                  \
  X(SELECT)                                                                    \
  X(NOP)                                                                       \
  X(INTRINSIC)                                                                 \
  X(FLOAT_LESSEQUAL)                                                           \
  X(FLOAT_ISNAN)                                                                 \
  X(FLOAT_FLOAT2FLOAT)                                                         \
  X(FLOAT_ROUND)                                                               \
  X(LZCOUNT)                                                                   \
  X(INSERT)                                                                    \
  X(EXTRACT)                                                                   \
  X(CAST)                                                                      \
  X(FLOAT_UINT2FLOAT)                                                          \
  X(FLOAT_FLOAT2UINT)                                                          \
  X(FLOAT_FMA)                                                                 \
  X(FLOAT_ROUNDEVEN)                                                           \
  X(FLOAT_MIN)                                                                 \
  X(FLOAT_MAX)                                                                 \
  X(FLOAT_MINNUM)                                                              \
  X(FLOAT_MAXNUM)

enum class NdOp : uint8_t {
#define ND_X_ENUM_(name) name,
  ND_OP_LIST(ND_X_ENUM_)
#undef ND_X_ENUM_
      _COUNT
};

/// Ordering attached to a memory access.  None denotes an ordinary,
/// non-atomic LOAD/STORE; every other value keeps the access atomic while it
/// moves through LowIR, MedIR, HighIR, and the backends.  Operations whose
/// opcode is intrinsically atomic (for example ATOMIC_XCHG, ATOMIC_ADD, or
/// ATOMIC_CMPXCHG) must use a non-None ordering.
enum class NdMemoryOrdering : uint8_t {
  None,
  Relaxed,
  Acquire,
  Release,
  AcquireRelease,
  SequentiallyConsistent,
};

/// Architecture-defined address space for a memory effect.  This is separate
/// from the numeric effective-address expression: on x86, LEA ignores segment
/// bases while a LOAD/STORE with the same base/index/displacement must retain
/// an FS/GS override all the way to code generation.
enum class NdMemoryAddressSpace : uint8_t {
  Default,
  X86FS,
  X86GS,
};

constexpr bool isKnownMemoryAddressSpace(NdMemoryAddressSpace AddressSpace) {
  switch (AddressSpace) {
  case NdMemoryAddressSpace::Default:
  case NdMemoryAddressSpace::X86FS:
  case NdMemoryAddressSpace::X86GS:
    return true;
  }
  return false;
}

/// Return whether an opcode owns an architectural memory address.  Validation
/// uses this to keep an FS/GS tag from being silently carried by arithmetic or
/// control-flow operations that have no address-space semantics.
constexpr bool opcodeSupportsMemoryAddressSpace(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::LOAD:
  case NdOp::STORE:
  case NdOp::ATOMIC_XCHG:
  case NdOp::ATOMIC_ADD:
  case NdOp::ATOMIC_CMPXCHG:
  case NdOp::INTRINSIC:
    return true;
  default:
    return false;
  }
}

const char *ndOpName(NdOp Op);

} // namespace neverd

#endif // NEVERD_IR_NDOPS_H
