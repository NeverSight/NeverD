//===- SymParseDetail.h - Shared symbolic text precedence -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the precedence ladder shared by the symbolic expression parser and
/// printer.
///
/// This header is an implementation detail of the symbolic library and should
/// not be included outside lib/symbolic/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMPARSEDETAIL_H
#define NEVERD_SYMBOLIC_SYMPARSEDETAIL_H

namespace neverd::symbolic::detail {

/// Binding strength, loosest first, following C.
enum Prec : int {
  PrecTernary = 1,
  PrecLogOr,
  PrecLogAnd,
  PrecBitOr,
  PrecBitXor,
  PrecBitAnd,
  PrecEquality,
  PrecRelational,
  PrecShift,
  PrecAdditive,
  PrecMultiplicative,
  PrecUnary,
  PrecPrimary,
};

} // namespace neverd::symbolic::detail

#endif // NEVERD_SYMBOLIC_SYMPARSEDETAIL_H
