//===- PrologueStackAllocation.h - Exact prologue allocations ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Canonicalizes proved constant stack allocations before frame consumers.

#ifndef NEVERD_IR_LOW_PROLOGUESTACKALLOCATION_H
#define NEVERD_IR_LOW_PROLOGUESTACKALLOCATION_H

namespace neverd {
struct BinaryImage;
struct LowFunc;
void canonicalizePrologueStackAllocation(const BinaryImage &, LowFunc &);
} // namespace neverd

#endif // NEVERD_IR_LOW_PROLOGUESTACKALLOCATION_H
