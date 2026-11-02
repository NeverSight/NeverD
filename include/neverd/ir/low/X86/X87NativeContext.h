//===- X87NativeContext.h - Native exact x87 context evaluator -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute one admitted instruction while preserving the analysis host state.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_LOW_X86_X87NATIVECONTEXT_H
#define NEVERD_IR_LOW_X86_X87NATIVECONTEXT_H
#include <array>
#include <cstdint>
namespace neverd {
bool executeNativeX87Context(uint32_t Selector, std::array<uint8_t, 512> &State,
                             void *Memory, uint64_t Flags, uint64_t &Result);
}
#endif
