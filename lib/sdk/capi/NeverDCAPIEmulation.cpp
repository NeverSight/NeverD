//===- NeverDCAPIEmulation.cpp - Bounded driver execution C API -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// C ABI adaptation for strict driver execution and optional lifecycle
/// scenarios.
///
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPIEmulation.h"

#include "SessionImpl.h"

#ifdef NEVERD_ENABLE_DRIVER_EMULATION
#include "neverd/emulation/DriverSession.h"
#endif

#include <exception>

using namespace neverd;
using namespace neverd::sdk;

namespace {

const char *runDriver(neverd_session_t Sess, const char *Path,
                      const char *ScenarioJSON,
                      const neverd_driver_options_v1 *Options) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  try {
    if (Path && !Path[0]) {
      S->setError("driver emulation path must not be empty");
      return nullptr;
    }
    if (!Path && !S->Loaded) {
      S->setError("driver emulation requires a loaded session");
      return nullptr;
    }
    if (Options && Options->struct_size != sizeof(*Options)) {
      S->setError("driver emulation options require the exact v1 struct_size");
      return nullptr;
    }
    if (Options && (!Options->instruction_limit || !Options->memory_limit ||
                    !Options->event_limit || !Options->timeout_milliseconds)) {
      S->setError("driver emulation budgets must be positive");
      return nullptr;
    }
#ifdef NEVERD_ENABLE_DRIVER_EMULATION
    emulation::DriverOptions EngineOptions;
    if (Options) {
      EngineOptions.InstructionLimit = Options->instruction_limit;
      EngineOptions.MemoryLimit = Options->memory_limit;
      EngineOptions.EventLimit = Options->event_limit;
      EngineOptions.TimeoutMilliseconds = Options->timeout_milliseconds;
      if (Options->service_name)
        EngineOptions.ServiceName = Options->service_name;
    }
    if (ScenarioJSON) {
      size_t Length = 0;
      while (Length <= emulation::DriverScenarioJSONLimit &&
             ScenarioJSON[Length])
        ++Length;
      if (Length > emulation::DriverScenarioJSONLimit) {
        S->setError("driver scenario JSON exceeds 2 MiB");
        return nullptr;
      }
      auto Scenario = emulation::driverOptionsFromScenarioJSON(
          llvm::StringRef(ScenarioJSON, Length), std::move(EngineOptions));
      if (!Scenario) {
        S->setError(llvm::toString(Scenario.takeError()));
        return nullptr;
      }
      EngineOptions = std::move(*Scenario);
    }
    const auto Input = Path ? std::filesystem::path(Path) : S->FilePath;
    auto Result = emulation::emulateDriver(Input, EngineOptions);
    if (!Result) {
      S->setError(llvm::toString(Result.takeError()));
      return nullptr;
    }
    char *JSON = dupStr(emulation::driverResultJSON(*Result));
    if (!JSON)
      S->setError("could not allocate driver emulation report");
    return JSON;
#else
    S->setError("driver emulation is disabled; rebuild with "
                "-DNEVERD_ENABLE_DRIVER_EMULATION=ON");
    return nullptr;
#endif
  } catch (const std::exception &Error) {
    S->setError(std::string("driver emulation failed: ") + Error.what());
  } catch (...) {
    S->setError("driver emulation failed with an unexpected exception");
  }
  return nullptr;
}

} // namespace

extern "C" const char *
neverd_emulate_driver_json(neverd_session_t Sess, const char *Path,
                           const neverd_driver_options_v1 *Options) {
  return runDriver(Sess, Path, nullptr, Options);
}

extern "C" const char *
neverd_emulate_driver_scenario_json(neverd_session_t Sess, const char *Path,
                                    const char *ScenarioJSON,
                                    const neverd_driver_options_v1 *Options) {
  if (!ScenarioJSON) {
    if (auto *S = toSession(Sess))
      S->setError("driver scenario JSON is required");
    return nullptr;
  }
  return runDriver(Sess, Path, ScenarioJSON, Options);
}
