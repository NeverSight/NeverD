//===- RegistrationOrdinaryGraph.h - PE32 physical graphs -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode ordinary C++ frame paths without granting native EH evidence.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONORDINARYGRAPH_H
#define NEVERD_IR_LOW_X86_REGISTRATIONORDINARYGRAPH_H

#include "neverd/Common.h"

namespace neverd {
struct BinaryImage;
struct LowFunc;
class Decoder;
class RegistrationCallCalleeIndex;
} // namespace neverd

namespace neverd::registration_abi {

/// A private, unnormalised integer graph for a cleanup-only C++ parent.
/// Registration states and exceptional edges remain absent. Only the exact
/// builder, image and entry requested here skip state analysis; nested builds
/// keep their ordinary policy. Never emit or cache this as a native frame.
/// The supplied index retains the caller's work and recursion boundaries.
LowFunc buildOrdinaryCxxFrameGraph(const BinaryImage &Image, Decoder &Decode,
                                   va_t Entry,
                                   RegistrationCallCalleeIndex &Callees);

} // namespace neverd::registration_abi

#endif // NEVERD_IR_LOW_X86_REGISTRATIONORDINARYGRAPH_H
