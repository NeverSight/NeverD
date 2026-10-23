//===- NeverDCAPIX87.cpp - x87 capability query C ABI --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapt the shared semantic inventory without changing a loaded session.
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPIX87.h"

#include "SessionImpl.h"

#include "neverd/ir/X87Capabilities.h"

#include <exception>
#include <string>

extern "C" const char *neverd_x87_capabilities_json(neverd_session_t Sess) {
  auto *Session = neverd::sdk::toSession(Sess);
  if (!Session)
    return nullptr;
  Session->clearError();
  try {
    const auto Report = neverd::x87CapabilitiesJSON();
    char *Result = neverd::sdk::dupStr(Report);
    if (!Result)
      Session->setError("x87 capability report allocation failed");
    return Result;
  } catch (const std::exception &Error) {
    Session->setError(std::string("x87 capability query failed: ") +
                      Error.what());
  } catch (...) {
    Session->setError(
        "x87 capability query failed with an unexpected exception");
  }
  return nullptr;
}
