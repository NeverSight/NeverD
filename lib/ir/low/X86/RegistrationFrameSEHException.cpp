//===- RegistrationFrameSEHException.cpp - SEH runtime field coordinates
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Restricts exception-object facts to their authenticated search invocation.

#include "RegistrationFrame.h"

namespace neverd::registration_state {

void FrameState::enterSEHFilter(va_t Entry, int32_t ExceptionPointersOffset,
                                std::optional<SEHExceptionOrigin> Origin) {
  enterCallback(Entry);
  SEHFilterEntry = Entry;
  SEHOrigin = Origin;
  store(ExceptionPointersOffset, 4,
        FrameValue::sehException(
            {SEHExceptionAddress::Kind::Pointers, Entry, 0}));
}

std::optional<FrameValue>
FrameState::sehExceptionLoad(const SEHExceptionAddress &Address,
                             uint16_t Width) const {
  if (CallbackEntry != Address.Filter || SEHFilterEntry != Address.Filter ||
      Width != 4)
    return std::nullopt;
  if (Address.Storage == SEHExceptionAddress::Kind::Pointers &&
      Address.Offset == 0)
    return FrameValue::sehException(
        {SEHExceptionAddress::Kind::Record, Address.Filter, 0});
  if (Address.Storage == SEHExceptionAddress::Kind::Record &&
      (Address.Offset == 0 || Address.Offset == 4)) {
    if (Address.Offset == 0 && SEHOrigin)
      return FrameValue::constant(SEHOrigin->Code);
    // Generic faults have no bound code. Runtime-maintained flags remain
    // unknown even for an exact RaiseException. An unknown DWORD may carry
    // pointer bits, so memory sinks still need an independent address proof.
    return FrameValue::taint(true, true);
  }
  return std::nullopt;
}

} // namespace neverd::registration_state
