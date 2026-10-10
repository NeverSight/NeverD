//===- X86ShadowStackShape.h - Typed RDSSP transport -------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_X86SHADOWSTACKSHAPE_H
#define NEVERD_IR_HIGH_X86SHADOWSTACKSHAPE_H

#include "neverd/ir/X86ShadowStack.h"
#include "neverd/ir/high/HighIR.h"

namespace neverd {
inline X86ShadowStackReadShape x86ShadowStackReadHighShape(const HighExpr &Call,
                                                           Arch TargetArch) {
  const HighExpr *Old =
      Call.Operands.empty() ? nullptr : Call.Operands[0].get();
  const HighExpr *Width =
      Call.Operands.size() < 2 ? nullptr : Call.Operands[1].get();
  const auto IsBits = [](const TypeRef &T) {
    return T && (T->Kind == NdTypeKind::Int || T->Kind == NdTypeKind::Ptr);
  };
  return {.TargetArch = TargetArch,
          .MemoryOrdering = Call.MemoryOrdering,
          .MemoryAddressSpace = Call.MemoryAddressSpace,
          .NumInputs = static_cast<unsigned>(Call.Operands.size() + 1),
          .IdIsConst = Call.IntrinsicId == Intrinsic::CetRdSsp,
          .IdSize = 2,
          .OutputIsWritable = IsBits(Call.Type),
          .OutputSize = Call.Type ? Call.Type->Size : 0U,
          .OldIsScalar = Old && IsBits(Old->Type),
          .OldSize = Old && Old->Type ? Old->Type->Size : 0U,
          .WidthIsConst = Width && Width->Kind == ExprKind::Const &&
                          Width->Type && Width->Type->Kind == NdTypeKind::Int,
          .WidthSize = Width && Width->Type ? Width->Type->Size : 0U,
          .ReadWidth = Width ? Width->ConstVal : 0U,
          .HasAuxiliaryOutputs = !Call.IntrinsicOutputs.empty() ||
                                 Call.IsIndirectCall || Call.IndirectTarget ||
                                 Call.SourceCallHint || Call.DoesNotReturn};
}
} // namespace neverd

#endif
