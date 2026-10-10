//===- NeverDCmdUnpack.cpp - Packed executable recovery routing -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../NeverDCLI.h"

#include "neverd/sdk/NeverDCAPIUnpack.h"
#include "neverd/unpack/UnpackCLIStrings.h"
#include "neverd/unpack/UnpackStrings.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::cli {
int runUnpack() {
  namespace text = unpack::strings;
  auto Session = neverd_session_create();
  if (!Session) {
    llvm::WithColor::error() << text::SessionFailed << '\n';
    return unpack_cli::Error;
  }
  SessionGuard Guard(Session);
  const char *Report = neverd_unpack_json(
      Session, UnpackInput.getValue().c_str(), UnpackOutput.getValue().c_str(),
      UnpackOptions.getValue().c_str());
  if (!Report) {
    llvm::WithColor::error() << takeLastError(Session) << '\n';
    return unpack_cli::Error;
  }
  auto Parsed = llvm::json::parse(Report);
  llvm::outs() << Report << '\n';
  neverd_free_string(Report);
  if (!Parsed) {
    llvm::WithColor::error() << text::InvalidReport << ": "
                             << llvm::toString(Parsed.takeError()) << '\n';
    return unpack_cli::Error;
  }
  const auto *Root = Parsed->getAsObject();
  if (!Root || !Root->getString(text::OutcomeField)) {
    llvm::WithColor::error() << text::InvalidReport << '\n';
    return unpack_cli::Error;
  }
  return Root->getString(text::OutcomeField) == text::UnpackedOutcome ||
                 Root->getString(text::OutcomeField) == text::SnapshotOutcome ||
                 Root->getString(text::OutcomeField) == text::RestoredOutcome
             ? unpack_cli::Success
             : unpack_cli::Incomplete;
}
} // namespace neverd::cli
