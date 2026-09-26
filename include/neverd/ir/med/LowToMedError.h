//===- LowToMedError.h - Explicit conversion failures --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_LOWTOMEDERROR_H
#define NEVERD_IR_MED_LOWTOMEDERROR_H

#include <stdexcept>

namespace neverd {

/// An unsupported semantic contract must not become a partially converted
/// function. The pipeline returns this diagnostic to its caller instead of
/// continuing through the best-effort conversion fallback.
class LowToMedConversionError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

} // namespace neverd

#endif // NEVERD_IR_MED_LOWTOMEDERROR_H
