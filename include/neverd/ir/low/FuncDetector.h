//===- FuncDetector.h - Function entry-point detection --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares FuncDetector, which discovers function entry points from symbol
/// tables, exports, call-target scanning, and heuristic validation.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_FUNCDETECTOR_H
#define NEVERD_IR_LOW_FUNCDETECTOR_H

#include "neverd/decode/Decoder.h"
#include "neverd/loader/BinaryImage.h"

#include <set>
#include <vector>

namespace neverd {

class FuncDetector {
public:
  /// Detect function entry points from the binary image.
  std::vector<std::pair<va_t, std::string>> detect(const BinaryImage &Img,
                                                   Decoder &Dec);

private:
  void scanCallTargets(const BinaryImage &Img, Decoder &Dec);
  /// Validate a heuristic entry.  When KeepInconclusive is true, bounded
  /// probes that exhaust their budget remain candidates for the formal audit;
  /// definite decode or mapping failures are still rejected.
  bool verifyFunctionDecode(const BinaryImage &Img, Decoder &Dec, va_t Addr,
                            bool KeepInconclusive = false);

  std::set<va_t> Entries;
};

} // namespace neverd

#endif // NEVERD_IR_LOW_FUNCDETECTOR_H
