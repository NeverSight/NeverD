//===- HighCNativeMemory.cpp - Native x86 address arguments -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native segment offsets use the raw address owner, beside ordinary DS data.
//===----------------------------------------------------------------------===//

#include "../HighCWriter.h"

#include "neverd/ir/intrinsics/X86/X86NativeMemory.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace neverd {
std::vector<std::string>
HighCWriter::x86NativeMemoryArguments(const HighExpr &E) {
  bool SegmentOffset = E.MemoryAddressSpace != NdMemoryAddressSpace::Default;
  if (!E.Operands.empty()) {
    const auto &Control = E.Operands.back();
    SegmentOffset |=
        Control && Control->Kind == ExprKind::Const &&
        x86NativeMemoryHasSegmentOffset(E.IntrinsicId, E.Operands.size() + 1,
                                        Control->ConstVal);
  }
  std::vector<std::string> Arguments;
  for (unsigned I = 0; I < E.Operands.size(); ++I) {
    if (!E.Operands[I])
      throw std::invalid_argument("missing x86 native memory operand");
    Arguments.push_back(I == 0 && SegmentOffset
                            ? addrStr(*E.Operands[I], 0, false)
                            : intrinsicOperandStr(*E.Operands[I]));
  }
  return Arguments;
}
} // namespace neverd
