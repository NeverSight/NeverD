//===- X64CallRCX.h - Bind native call register results --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Source-only transport for an additional result of an ordinary native call.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_X86_X64CALLRCX_H
#define NEVERD_LIB_PIPELINE_X86_X64CALLRCX_H

#include "neverd/ir/low/CallRegisterEffects.h"

#include <map>
#include <set>

namespace neverd {
struct BinaryImage;
struct MedFunc;
void captureX64CallRCX(MedFunc &Function, const BinaryImage &Image,
                       const std::map<va_t, GPRFamilyMask> &Unread,
                       const std::set<va_t> &Emitted);
} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64CALLRCX_H
