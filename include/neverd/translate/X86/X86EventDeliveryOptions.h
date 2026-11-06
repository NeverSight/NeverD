//===- X86EventDeliveryOptions.h - Event work budget ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bound host work without replacing pending architectural events.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_X86EVENTDELIVERYOPTIONS_H
#define NEVERD_TRANSLATE_X86_X86EVENTDELIVERYOPTIONS_H

#include <cstdint>

namespace neverd::translate {

struct X86EventDeliveryOptions {
  /// Positive entry attempts per delivery/resume call. Exhaustion preserves
  /// the next event and all committed state. Fixed-IST recursive debug entry
  /// need not exhaust its guest stack, so instruction budgets cannot bound it.
  uint32_t MaxAttempts = 4096;
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86EVENTDELIVERYOPTIONS_H
