//===- PipelineDebugReceivers.h - Scoped receiver types --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind register-resident debug locals to native indirect-call receivers.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PIPELINE_PIPELINEDEBUGRECEIVERS_H
#define NEVERD_PIPELINE_PIPELINEDEBUGRECEIVERS_H

#include "neverd/ir/FloatingCallArguments.h"
#include "neverd/ir/IntegerCallArguments.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/low/SourceCallOccurrence.h"
#include "neverd/loader/ItaniumVTable.h"

#include <functional>

namespace neverd {
class DebugContext;
struct BinaryImage;

struct DebugVirtualCallArguments {
  std::map<SourceCallOccurrenceKey, FixedIntegerCallArguments> Integers;
  std::map<SourceCallOccurrenceKey, FloatingCallArguments> Floating;
};

struct DebugPointerOperand {
  MedVar Value;
  TypeRef Type;
};
using DebugPointerOperands =
    std::map<std::pair<va_t, int>, std::vector<DebugPointerOperand>>;

/// Bind source pointer declarations to unchanged full-width register inputs
/// of native address computations and memory operations. Scope and location
/// must hold at the use, including branch-local downcasts.
DebugPointerOperands debugPointerOperandTypes(const BinaryImage &Image,
                                              const LowFunc &Low,
                                              const MedFunc &Med,
                                              DebugContext &Debug);

/// Follow pointer-width copies and primary-vtable loads from declared entry
/// objects. Bind a complete virtual prototype to its call occurrence before
/// SSA; the function pointer still comes from the runtime object's table.
DebugVirtualCallArguments debugVirtualCallArguments(
    const BinaryImage &Image, const LowFunc &Function, DebugContext &Debug,
    const std::function<const ItaniumVTableIndex &()> &Tables);

/// Whole-width copies within the original block establish register identity.
/// The recovered argument must still be that register's MedIR value.
/// Location declarations are read at the call, never at the value's first
/// definition: a downcast can have a shorter scope than its native carrier.
std::map<va_t, TypeRef> debugCallReceiverTypes(const BinaryImage &Image,
                                               const LowFunc &Low,
                                               const MedFunc &Med,
                                               const HighFunc &High,
                                               DebugContext &Debug);
} // namespace neverd

#endif // NEVERD_PIPELINE_PIPELINEDEBUGRECEIVERS_H
