//===- X64Registers.h - Concrete x64 execution registers -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared normalized register identity, independent of a decoder or backend.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_X64REGISTERS_H
#define NEVERD_EMULATION_X64REGISTERS_H

namespace neverd::emulation {
enum class X64Register {
#define NEVERD_X64_REGISTER(Name, DecoderID, BackendID) Name,
#include "X64Registers.def"
#undef NEVERD_X64_REGISTER
};
} // namespace neverd::emulation

#endif // NEVERD_EMULATION_X64REGISTERS_H
