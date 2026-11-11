//===- SymMBAProductColumn.h - Masked product parity -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_MBA_SYMMBAPRODUCTCOLUMN_H
#define NEVERD_SYMBOLIC_MBA_SYMMBAPRODUCTCOLUMN_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstddef>
#include <optional>
#include <utility>

namespace neverd::symbolic::detail {

struct AffineProductColumn {
  unsigned XBits = 0, YBits = 0;
  bool Constant = false;
};

// Recover the XOR of the top bits of ((X+x) & -2^A) * ((Y+y) & -2^B)
// at Count-bit precision. Verify the affine reading on the complete domain.
std::optional<AffineProductColumn>
readAffineProductColumn(unsigned Count, unsigned A, unsigned B,
                        llvm::ArrayRef<std::pair<unsigned, unsigned>> Offsets,
                        llvm::function_ref<bool(size_t)> Charge);

} // namespace neverd::symbolic::detail

#endif // NEVERD_SYMBOLIC_MBA_SYMMBAPRODUCTCOLUMN_H
