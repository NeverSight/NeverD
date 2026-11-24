//===- I386WideStackArguments.h - Whole i386 stack operands ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check the source projection of integer arguments that occupy several
/// Windows i386 stack slots. Call recovery binds each as one value.
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_I386WIDESTACKARGUMENTS_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_I386WIDESTACKARGUMENTS_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/libc/LibCFixedIntegerArguments.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd {

inline std::optional<FixedIntegerCallArguments>
i386WideStackArguments(const FunctionSym &Function, Arch A, BinaryFormat F) {
  if (A != Arch::X86 || F != BinaryFormat::COFF ||
      !(Function.PrototypeFromHeader || Function.PrototypeFromCLibrary))
    return std::nullopt;
  const auto Word = getTargetRegInfo(A).PointerSize;
  if (!Function.ReturnType || Function.ReturnType->Size > Word ||
      !llvm::any_of(Function.Params, [Word](const auto &Param) {
        return Param.second && Param.second->Kind == NdTypeKind::Int &&
               Param.second->Size > Word;
      }))
    return std::nullopt;
  return libc::fixedIntegerCallArguments(Function, A, F);
}

/// Each argument must still be present at its declared width, including the
/// one after a multiword integer. Flat lists of pushed words do not match.
template <typename WidthOf>
bool matchesWholeStackArguments(const FixedIntegerCallArguments &Arguments,
                                size_t Count, WidthOf Width) {
  if (Arguments.Parameters.size() != Count)
    return false;
  for (size_t I = 0; I < Count; ++I)
    if (Arguments.Parameters[I].Bytes != Width(I))
      return false;
  return true;
}

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_I386WIDESTACKARGUMENTS_H
