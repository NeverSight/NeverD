//===- CAnalysisDispatch.h - Unresolved branch annotations -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original indirect-jump text shared by the two C display routes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_CANALYSISDISPATCH_H
#define NEVERD_BACKEND_C_CANALYSISDISPATCH_H

#include "neverd/Common.h"

#include <string>

namespace neverd {

struct BinaryImage;

std::string analysisDispatchComment(const BinaryImage *Image, Arch TheArch,
                                    va_t Address);

} // namespace neverd

#endif // NEVERD_BACKEND_C_CANALYSISDISPATCH_H
