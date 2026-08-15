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
/// opcode is intrinsically atomic (for example ATOMIC_XCHG) must use a
/// non-None ordering.
enum class NdMemoryOrdering : uint8_t {
  None,
  Relaxed,
  Acquire,
  Release,
  AcquireRelease,
  SequentiallyConsistent,
};

const char *ndOpName(NdOp Op);

} // namespace neverd

#endif // NEVERD_IR_NDOPS_H
