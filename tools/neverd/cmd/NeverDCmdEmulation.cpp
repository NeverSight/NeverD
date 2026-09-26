//===- NeverDCmdEmulation.cpp - Windows driver initialization -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// CLI routing and bounded scenario input for Windows driver emulation.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/emulation/DriverProfile.h"
#include "neverd/emulation/DriverReportFields.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <fstream>

using namespace llvm;

namespace neverd::cli {
namespace {

Expected<std::string> readScenario(StringRef Path) {
  if (Path.empty())
    return createStringError(inconvertibleErrorCode(),
                             "--scenario requires a nonempty file path");
  std::ifstream Input(Path.str(), std::ios::binary);
  if (!Input)
    return createStringError(inconvertibleErrorCode(),
                             "cannot open driver scenario file");
  std::string Text;
  std::array<char, emulation::profile::PageSize> Buffer;
  while (Input) {
    Input.read(Buffer.data(), Buffer.size());
    const size_t Read = static_cast<size_t>(Input.gcount());
    if (Read > NEVERD_DRIVER_SCENARIO_JSON_LIMIT - Text.size())
      return createStringError(inconvertibleErrorCode(),
                               "driver scenario JSON exceeds 2 MiB");
    Text.append(Buffer.data(), Read);
  }
  if (!Input.eof())
    return createStringError(inconvertibleErrorCode(),
                             "could not read driver scenario file");
  if (Text.find('\0') != std::string::npos)
    return createStringError(inconvertibleErrorCode(),
                             "driver scenario JSON contains a NUL byte");
  return Text;
}

} // namespace

int runEmulateDriver() {
  if (!DriverInstructionLimit) {
    WithColor::error() << "--instruction-limit must be positive\n";
    return 1;
  }
  neverd_session_t Sess = neverd_session_create();
  if (!Sess) {
    WithColor::error() << "could not create driver emulation session\n";
    return 1;
  }
  SessionGuard Guard(Sess);
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = DriverInstructionLimit;
  Options.memory_limit = emulation::profile::DefaultMemoryLimit;
  Options.event_limit = emulation::profile::DefaultEventLimit;
  Options.timeout_milliseconds = emulation::profile::DefaultTimeoutMilliseconds;
  const char *Report;
  if (DriverScenarioFile.getNumOccurrences()) {
    auto Scenario = readScenario(DriverScenarioFile.getValue());
    if (!Scenario) {
      WithColor::error() << toString(Scenario.takeError()) << "\n";
      return 1;
    }
    Report = neverd_emulate_driver_scenario_json(
        Sess, EmulateDriverInput.getValue().c_str(), Scenario->c_str(),
        &Options);
  } else {
    Report = neverd_emulate_driver_json(
        Sess, EmulateDriverInput.getValue().c_str(), &Options);
  }
  if (!Report) {
    WithColor::error() << takeLastError(Sess) << "\n";
    return 1;
  }
  auto Parsed = json::parse(Report);
  outs() << Report << "\n";
  neverd_free_string(Report);
  if (!Parsed) {
    WithColor::error() << "invalid driver emulation report: "
                       << toString(Parsed.takeError()) << "\n";
    return 1;
  }
  auto *Root = Parsed->getAsObject();
  if (!Root) {
    WithColor::error() << "driver emulation report is not an object\n";
    return 1;
  }
  if (Root->getString(emulation::field::StopReason) !=
      emulation::stop::Returned)
    return 3;
  auto Success = Root->getBoolean(emulation::field::ScenarioSuccess);
  if (!Success) {
    WithColor::error() << "driver report has no scenario outcome\n";
    return 1;
  }
  return *Success ? 0 : 2;
}

} // namespace neverd::cli
